// iPhone を Windows の Vulkan の GPU として見せるドライバ（ICD）。
// アプリが呼んだ Vulkan の命令を、USB の先の受け手（vk/remote/server.c）に送って、そこの本物の Vulkan で実行させる。
// 計算（compute）と描画（画像・render pass・graphics pipeline）と、窓に出す仕組み（swapchain）。
// 窓に出すときは、受け手で描いた 1 コマを PC に持ち帰って、Windows の窓に描き写す（GDI）。
//
// つなぎ先は環境変数 IPHONE_VK（既定 127.0.0.1:50061。usb_forward.py --local 50061 --remote 50060 で iPhone へ）。
// IPHONE_VK_LOG=1 で何をしているかを stderr に出す。
// IPHONE_VK_STATS=1 で、終わりに「どの命令を何回送り、何回返事を待ち、何バイト運んだか」を stderr に出す。
//
// メモリの同期
//   PC 側に写しを持ち、アプリにはその写しを map させる。写しは Windows の書き込み監視つきで確保して、
//   vkQueueSubmit の前に、その submit で GPU が読む範囲（コピー元・つないだバッファ）のうち、アプリが書いたページだけを受け手へ送る。
//   GPU が書いたかもしれない範囲（storage buffer・コピー先）は、fence などで終わりを待ったときに引き取る。
//
// 待ち方（取り決めの版 3）
//   持ち帰る物が無い待ち（fence・timeline semaphore・idle）には、往復せずにその場で「終わった」と答える。
//   受け手は、出した仕事が使っているかもしれない物を変える命令の前に、自分の中で仕事の終わりを待つ（drain）。
//   持ち帰る物があるときは、MEM_READ の返事が終わりの知らせを兼ねる（受け手は読む前に drain する）。
//   往復は「GPU の答えを持ち帰る」ときだけになる
#include "../remote/proto.h"
#include "../remote/wire.h"
#include <windows.h>
#define COBJMACROS
#include <wincodec.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#define EXPORT __declspec(dllexport)
#define PAGE 4096u
#define ID(h) ((uint64_t) (uintptr_t) (h))

// ---- 記録 ----
static int g_verbose = -1;
static void say(const char *fmt, ...) {
    if (g_verbose < 0) { const char *e = getenv("IPHONE_VK_LOG"); g_verbose = e && *e && *e != '0'; }
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[iphone-vk] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ---- 落ちたときに、どのプログラムのどこで落ちたかを書き出す（IPHONE_VK_LOG=1 のとき） ----
static void where(const char *tag, void *addr) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) addr, &m)) {
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        // 関数の名前は、横に置いた .pdb から dbghelp で引く
        static int sym_ready;
        if (!sym_ready) { sym_ready = 1; SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), NULL, TRUE); }
        char sbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *) sbuf;
        memset(sbuf, 0, sizeof sbuf);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        int named = SymFromAddr(GetCurrentProcess(), (DWORD64) (uintptr_t) addr, &disp, si);
        fprintf(stderr, "[iphone-vk] %s %s+0x%llx %s%s+0x%llx\n", tag, b ? b + 1 : name, (unsigned long long) ((uintptr_t) addr - (uintptr_t) m),
                named ? "" : "", named ? si->Name : "?", (unsigned long long) disp);
    } else {
        fprintf(stderr, "[iphone-vk] %s %p\n", tag, addr);
    }
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "[iphone-vk] 落ちた：例外 0x%08lx", code);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        fprintf(stderr, "（%s %p）", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "書き込み" : "読み出し",
                (void *) ep->ExceptionRecord->ExceptionInformation[1]);
    fputc('\n', stderr);
    where("場所", ep->ExceptionRecord->ExceptionAddress);
    void *fr[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, fr, NULL);
    for (USHORT i = 0; i < n; i++) where("  呼び出し元", fr[i]);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor)) static void install_crash_handler(void) {
    const char *e = getenv("IPHONE_VK_LOG");
    if (e && *e && *e != '0') AddVectoredExceptionHandler(1, crash_handler);
}

// ---- 数える（IPHONE_VK_STATS=1） ----
static const char *const OP_NAME[VR_OP_COUNT] = {
    [VR_HELLO] = "HELLO", [VR_GET_FEATURES2] = "GET_FEATURES2", [VR_GET_PROPERTIES2] = "GET_PROPERTIES2",
    [VR_GET_FORMAT_PROPS] = "GET_FORMAT_PROPS", [VR_CREATE_DEVICE] = "CREATE_DEVICE", [VR_DESTROY_DEVICE] = "DESTROY_DEVICE",
    [VR_GET_QUEUE] = "GET_QUEUE", [VR_CREATE_BUFFER] = "CREATE_BUFFER", [VR_DESTROY_BUFFER] = "DESTROY_BUFFER",
    [VR_ALLOC_MEMORY] = "ALLOC_MEMORY", [VR_FREE_MEMORY] = "FREE_MEMORY", [VR_BIND_BUFFER_MEMORY] = "BIND_BUFFER_MEMORY",
    [VR_MEM_WRITE] = "MEM_WRITE", [VR_MEM_READ] = "MEM_READ", [VR_CREATE_SHADER_MODULE] = "CREATE_SHADER_MODULE",
    [VR_DESTROY_SHADER_MODULE] = "DESTROY_SHADER_MODULE", [VR_CREATE_DSL] = "CREATE_DSL", [VR_DESTROY_DSL] = "DESTROY_DSL",
    [VR_CREATE_PIPELINE_LAYOUT] = "CREATE_PIPELINE_LAYOUT", [VR_DESTROY_PIPELINE_LAYOUT] = "DESTROY_PIPELINE_LAYOUT",
    [VR_CREATE_COMPUTE_PIPELINE] = "CREATE_COMPUTE_PIPELINE", [VR_DESTROY_PIPELINE] = "DESTROY_PIPELINE",
    [VR_CREATE_DESCRIPTOR_POOL] = "CREATE_DESCRIPTOR_POOL", [VR_DESTROY_DESCRIPTOR_POOL] = "DESTROY_DESCRIPTOR_POOL",
    [VR_RESET_DESCRIPTOR_POOL] = "RESET_DESCRIPTOR_POOL", [VR_ALLOC_DESCRIPTOR_SETS] = "ALLOC_DESCRIPTOR_SETS",
    [VR_FREE_DESCRIPTOR_SETS] = "FREE_DESCRIPTOR_SETS", [VR_UPDATE_DESCRIPTOR_SETS] = "UPDATE_DESCRIPTOR_SETS",
    [VR_CREATE_COMMAND_POOL] = "CREATE_COMMAND_POOL", [VR_DESTROY_COMMAND_POOL] = "DESTROY_COMMAND_POOL",
    [VR_RESET_COMMAND_POOL] = "RESET_COMMAND_POOL", [VR_ALLOC_COMMAND_BUFFERS] = "ALLOC_COMMAND_BUFFERS",
    [VR_FREE_COMMAND_BUFFERS] = "FREE_COMMAND_BUFFERS", [VR_RECORD_COMMAND_BUFFER] = "RECORD_COMMAND_BUFFER",
    [VR_RESET_COMMAND_BUFFER] = "RESET_COMMAND_BUFFER", [VR_CREATE_FENCE] = "CREATE_FENCE", [VR_DESTROY_FENCE] = "DESTROY_FENCE",
    [VR_RESET_FENCES] = "RESET_FENCES", [VR_WAIT_FOR_FENCES] = "WAIT_FOR_FENCES", [VR_GET_FENCE_STATUS] = "GET_FENCE_STATUS",
    [VR_CREATE_SEMAPHORE] = "CREATE_SEMAPHORE", [VR_DESTROY_SEMAPHORE] = "DESTROY_SEMAPHORE", [VR_QUEUE_SUBMIT] = "QUEUE_SUBMIT",
    [VR_QUEUE_WAIT_IDLE] = "QUEUE_WAIT_IDLE", [VR_DEVICE_WAIT_IDLE] = "DEVICE_WAIT_IDLE", [VR_PING] = "PING",
    [VR_WAIT_SEMAPHORES] = "WAIT_SEMAPHORES", [VR_GET_SEMAPHORE_COUNTER] = "GET_SEMAPHORE_COUNTER",
    [VR_SIGNAL_SEMAPHORE] = "SIGNAL_SEMAPHORE", [VR_CREATE_IMAGE] = "CREATE_IMAGE", [VR_DESTROY_IMAGE] = "DESTROY_IMAGE",
    [VR_BIND_IMAGE_MEMORY] = "BIND_IMAGE_MEMORY", [VR_CREATE_IMAGE_VIEW] = "CREATE_IMAGE_VIEW", [VR_DESTROY_IMAGE_VIEW] = "DESTROY_IMAGE_VIEW",
    [VR_CREATE_RENDER_PASS] = "CREATE_RENDER_PASS", [VR_DESTROY_RENDER_PASS] = "DESTROY_RENDER_PASS",
    [VR_CREATE_FRAMEBUFFER] = "CREATE_FRAMEBUFFER", [VR_DESTROY_FRAMEBUFFER] = "DESTROY_FRAMEBUFFER",
    [VR_CREATE_GRAPHICS_PIPELINE] = "CREATE_GRAPHICS_PIPELINE", [VR_GET_IMAGE_FORMAT_PROPS] = "GET_IMAGE_FORMAT_PROPS",
    [VR_CREATE_SAMPLER] = "CREATE_SAMPLER", [VR_DESTROY_SAMPLER] = "DESTROY_SAMPLER", [VR_MEM_READ_DELTA] = "MEM_READ_DELTA",
    [VR_READ_JPEG] = "READ_JPEG",
    [VR_CREATE_RENDER_PASS2] = "CREATE_RENDER_PASS2", [VR_CREATE_QUERY_POOL] = "CREATE_QUERY_POOL",
    [VR_DESTROY_QUERY_POOL] = "DESTROY_QUERY_POOL", [VR_GET_QUERY_POOL_RESULTS] = "GET_QUERY_POOL_RESULTS",
    [VR_RESET_QUERY_POOL] = "RESET_QUERY_POOL", [VR_CREATE_BUFFER_VIEW] = "CREATE_BUFFER_VIEW",
    [VR_DESTROY_BUFFER_VIEW] = "DESTROY_BUFFER_VIEW", [VR_GET_IMAGE_SUBRESOURCE_LAYOUT] = "GET_IMAGE_SUBRESOURCE_LAYOUT",
    [VR_READ_JPEG_PIPE] = "READ_JPEG_PIPE", [VR_GET_BUFFER_ADDRESS] = "GET_BUFFER_ADDRESS",
    [VR_PACKED] = "PACKED", [VR_CREATE_BUFFER_NR] = "CREATE_BUFFER_NR", [VR_CREATE_IMAGE_NR] = "CREATE_IMAGE_NR", [VR_GET_STATUS] = "GET_STATUS",
};
static struct {
    uint64_t sends[VR_OP_COUNT], calls[VR_OP_COUNT], bytes[VR_OP_COUNT];
    double wait_ms[VR_OP_COUNT];
    uint64_t bytes_in, flushes;
    uint64_t ww_calls, ww_pages;   // 書き込み監視を見た回数・見つかった書いたページ
    uint64_t wdiff_pages, wdiff_sent; // 違う所だけ送ったページの数と、送ったバイト
    uint64_t pack_in, pack_out;    // LZ4 で縮める前と後のバイト
    double pack_ms;
    double blit_ms, present_ms, record_ms; // 窓に描く（JPEG をほどく・描く）・present 全体・command buffer を閉じて送る
    uint64_t blits, presents, records;
    uint64_t skipped;              // 往復せずに済ませた待ち
    double ww_ms, rb_ms;           // 書き込み監視にかかった時間・引き取りにかかった時間
    double lat_sum, lat_max;       // present から窓に出るまで
    uint64_t lat_n;
    int heat_max;                  // いちばん熱かったときの iPhone の熱の状態
} g_stat;
static int g_stats = -1;

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static void print_stats(void) {
    uint64_t ts = 0, tc = 0, tb = 0;
    double tw = 0;
    fprintf(stderr, "\n[iphone-vk] 命令の数（送りっぱなし / 返事を待った / 送ったバイト / 待った ms）\n");
    for (int i = 1; i < VR_OP_COUNT; i++) {
        if (!g_stat.sends[i] && !g_stat.calls[i]) continue;
        fprintf(stderr, "  %-24s %9llu %9llu %13llu %10.1f\n", OP_NAME[i] ? OP_NAME[i] : "?", (unsigned long long) g_stat.sends[i],
                (unsigned long long) g_stat.calls[i], (unsigned long long) g_stat.bytes[i], g_stat.wait_ms[i]);
        ts += g_stat.sends[i]; tc += g_stat.calls[i]; tb += g_stat.bytes[i]; tw += g_stat.wait_ms[i];
    }
    fprintf(stderr, "  %-24s %9llu %9llu %13llu %10.1f\n", "合計", (unsigned long long) ts, (unsigned long long) tc, (unsigned long long) tb, tw);
    fprintf(stderr, "  受け取ったバイト %llu・まとめて送った回数 %llu\n", (unsigned long long) g_stat.bytes_in, (unsigned long long) g_stat.flushes);
    fprintf(stderr, "  書き込み監視 %llu 回 %.1f ms 書いたページ %llu / 引き取り %.1f ms\n", (unsigned long long) g_stat.ww_calls, g_stat.ww_ms,
            (unsigned long long) g_stat.ww_pages, g_stat.rb_ms);
    fprintf(stderr, "  往復せずに済ませた待ち %llu\n", (unsigned long long) g_stat.skipped);
    if (g_stat.presents)
        fprintf(stderr, "  present %llu 回 %.1f ms（うち窓に描く %llu 回 %.1f ms）\n", (unsigned long long) g_stat.presents, g_stat.present_ms,
                (unsigned long long) g_stat.blits, g_stat.blit_ms);
    if (g_stat.heat_max) fprintf(stderr, "  iPhone の熱：いちばん熱かったのは %d（0 ふつう〜3 とても熱い）\n", g_stat.heat_max);
    if (g_stat.lat_n)
        fprintf(stderr, "  present から窓に出るまで：平均 %.1f ms・いちばん長い %.1f ms（%llu 枚）\n", g_stat.lat_sum / (double) g_stat.lat_n,
                g_stat.lat_max, (unsigned long long) g_stat.lat_n);
    if (g_stat.pack_in)
        fprintf(stderr, "  LZ4 で縮めた：%llu バイトを %llu バイトに・%.1f ms\n", (unsigned long long) g_stat.pack_in,
                (unsigned long long) g_stat.pack_out, g_stat.pack_ms);
    if (g_stat.wdiff_pages)
        fprintf(stderr, "  違う所だけ送ったページ %llu・送ったバイト %llu・丸ごとなら %llu\n", (unsigned long long) g_stat.wdiff_pages,
                (unsigned long long) g_stat.wdiff_sent, (unsigned long long) g_stat.wdiff_pages * PAGE);
}

// ---- 受け手との通り道 ----
static SRWLOCK g_lock = SRWLOCK_INIT;
static vr_sock g_sock = VR_BAD_SOCK;
static int g_dead;
static vr_buf g_q; // まだ送っていない命令
static void conn_lost(const char *what);
static volatile LONG64 g_next_id;

// 数字だけの物（shader module など）の番号。ポインタと重ならないように上の方の桁を使う
static uint64_t new_id(void) { return (1ull << 62) | (uint64_t) InterlockedIncrement64(&g_next_id); }

// 命令の並びを縮める LZ4（BSD-2）。この 1 つのファイルの中に取り込む
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../third_party/lz4/lz4.c"
#pragma GCC diagnostic pop
static int g_can_pack; // 受け手が VR_CAP_PACK を名乗った

// 送る命令の並びを LZ4 で縮めるか（受け手が名乗ったとき。IPHONE_VK_PACK=0 で縮めない）
static int pack_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_PACK"); v = !(e && *e == '0'); }
    return v;
}

static int flush_locked(void) {
    if (g_dead || g_sock == VR_BAD_SOCK) { g_q.n = 0; return 0; }
    if (g_q.n) g_stat.flushes++;
    // 1 KB 以上なら、命令の並びをまとめて縮め、1 つの命令にして送る。縮まらなければそのまま
    if (g_q.n >= 1024 && g_q.n < (1u << 30) && g_can_pack && pack_on()) {
        static vr_buf pk;
        int bound = LZ4_compressBound((int) g_q.n);
        pk.n = 0;
        vr_reserve(&pk, (size_t) bound + 12);
        double t0 = now_ms();
        int cl = LZ4_compress_default((const char *) g_q.p, (char *) pk.p + 12, (int) g_q.n, bound);
        g_stat.pack_ms += now_ms() - t0;
        if (cl > 0 && (size_t) cl + 12 < g_q.n / 10 * 9) {
            uint32_t h[3] = { VR_PACKED, (uint32_t) cl + 4, (uint32_t) g_q.n };
            memcpy(pk.p, h, 12);
            g_stat.pack_in += g_q.n;
            g_stat.pack_out += (uint64_t) cl + 12;
            if (!vr_send_all(g_sock, pk.p, (size_t) cl + 12)) conn_lost("送る所");
            g_q.n = 0;
            return !g_dead;
        }
    }
    if (g_q.n && !vr_send_all(g_sock, g_q.p, g_q.n)) conn_lost("送る所");
    g_q.n = 0;
    return !g_dead;
}

static void put_msg_locked(uint32_t op, const vr_buf *b) {
    uint32_t h[2] = { op, (uint32_t) b->n };
    if (op < VR_OP_COUNT) g_stat.bytes[op] += 8 + b->n;
    vr_put(&g_q, h, 8);
    vr_put(&g_q, b->p, b->n);
}

// 送りっぱなし（ためておいて、返事の要る命令や submit のときにまとめて送る）
static void rpc_send(uint32_t op, vr_buf *b) {
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.sends[op]++;
    put_msg_locked(op, b);
    if (g_q.n > (8u << 20)) flush_locked();
    ReleaseSRWLockExclusive(&g_lock);
    b->n = 0;
}

static void rpc_flush(void) {
    AcquireSRWLockExclusive(&g_lock);
    flush_locked();
    ReleaseSRWLockExclusive(&g_lock);
}

// 返事を待つ。返事の中身は out に入る（要らなければ NULL）
// あとで受け取る返事。窓に出す絵を頼んだら返事を待たずにアプリを先へ進め、次のコマで受け取る。
// 返事は頼んだ順にしか届かないので、ほかの命令が返事を読む前に、先にこれを読んで取っておく
static struct { int active, ready; VkResult r; vr_buf out; } g_defer;

// つながりが切れたことにする。最初の 1 回だけ書く（IPHONE_VK_LOG が無くても書く）
static void banner(const wchar_t *t);
static void conn_lost(const char *what) {
    if (!g_dead) {
        fprintf(stderr, "[iphone-vk] 受け手とのつながりが切れた（%s）。この先は全部失敗にする\n", what);
        banner(L"iPhone とのつながりが切れました");
    }
    g_dead = 1;
}

// 受け手が止まって返事が来ない間（iPhone の受け手のアプリが後ろに回ったなど）、黙って固まらずに記録と窓に出す。
// 受け手が戻れば、つながりは生きているので続きから動く。IPHONE_VK_GIVEUP=秒 を決めると、その長さで諦めて切れたことにする
static HWND g_stall_hwnd; // 帯を出す窓（いちばん新しい swapchain の窓）
static int g_stalled;

static void banner(const wchar_t *t) {
    HWND w = g_stall_hwnd;
    if (!w || !IsWindow(w)) return;
    RECT rc;
    GetClientRect(w, &rc);
    RECT band = { rc.left, rc.top, rc.right, rc.top + 32 };
    HDC dc = GetDC(w);
    if (!dc) return;
    HBRUSH br = CreateSolidBrush(RGB(24, 24, 24));
    FillRect(dc, &band, br);
    DeleteObject(br);
    HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    DrawTextW(dc, t, -1, &band, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(w, dc);
}

// 帯の文は仮。画面の文章は CEO が書く
static void stall_banner(double sec) {
    wchar_t t[128];
    swprintf(t, 128, L"iPhone の返事を待っています（%.0f 秒）", sec);
    banner(t);
}

static int recv_head(uint32_t h[2], uint32_t op) {
    static double giveup = -1;
    if (giveup < 0) { const char *e = getenv("IPHONE_VK_GIVEUP"); giveup = e ? atof(e) : 0; }
    // 窓の無い計算のアプリ（画像生成など）は GPU の 1 回の仕事が数秒かかることがあるので、GPU を待つ命令は 20 秒まで黙る
    int gpu_wait = op == VR_WAIT_FOR_FENCES || op == VR_WAIT_SEMAPHORES || op == VR_QUEUE_WAIT_IDLE || op == VR_DEVICE_WAIT_IDLE ||
                   op == VR_MEM_READ || op == VR_MEM_READ_DELTA || op == VR_GET_QUERY_POOL_RESULTS;
    double quiet = gpu_wait && !g_stall_hwnd ? 20 : 3;
    WSAPOLLFD pf = { .fd = g_sock, .events = POLLRDNORM };
    double t0 = 0, warned = 0;
    while (WSAPoll(&pf, 1, 1000) == 0) {
        if (!t0) t0 = now_ms() - 1000;
        double sec = (now_ms() - t0) / 1000;
        if (sec >= quiet && (!warned || sec - warned >= 10)) {
            fprintf(stderr, "[iphone-vk] 受け手の返事が %.0f 秒ない（命令 %u）。GPU の仕事が長いか、iPhone の受け手のアプリが後ろに回っている\n", sec, op);
            warned = sec;
            g_stalled = 1;
        }
        if (g_stalled) stall_banner(sec);
        if (giveup > 0 && sec >= giveup) {
            conn_lost("返事を待ちきれなかった");
            return 0;
        }
    }
    int ok = vr_recv_all(g_sock, h, 8);
    if (g_stalled) {
        g_stalled = 0;
        fprintf(stderr, "[iphone-vk] 受け手の返事が戻った（%.1f 秒止まっていた）\n", (now_ms() - t0) / 1000);
        if (g_stall_hwnd) InvalidateRect(g_stall_hwnd, NULL, FALSE);
    }
    if (!ok) conn_lost("返事を待っている所");
    return ok;
}

// g_lock の中で、送り終えたあとに呼ぶ
static void read_deferred_locked(void) {
    if (!g_defer.active) return;
    g_defer.active = 0;
    g_defer.ready = 1;
    g_defer.r = VK_ERROR_DEVICE_LOST;
    g_defer.out.n = 0;
    uint32_t h[2];
    if (!recv_head(h, VR_READ_JPEG_PIPE)) return;
    vr_reserve(&g_defer.out, h[1]);
    if (h[1] && !vr_recv_all(g_sock, g_defer.out.p, h[1])) { conn_lost("窓の絵の返事の途中"); return; }
    g_defer.out.n = h[1];
    g_defer.r = (VkResult) (int32_t) h[0];
    g_stat.bytes_in += 8 + h[1];
}

// 頼んで、返事は待たない。返事は rpc_take_deferred で受け取る（受け取る前に頼めるのは 1 つだけ）
static void rpc_call_deferred(uint32_t op, vr_buf *b) {
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.calls[op]++;
    if (g_defer.active) { flush_locked(); read_deferred_locked(); }
    put_msg_locked(op, b);
    if (flush_locked()) { g_defer.active = 1; g_defer.ready = 0; }
    ReleaseSRWLockExclusive(&g_lock);
    b->n = 0;
}

// あとで受け取る返事を受け取る。まだ届いていなければ待つ。頼んでいなければ VK_NOT_READY
static VkResult rpc_take_deferred(vr_buf *out) {
    AcquireSRWLockExclusive(&g_lock);
    double t0 = now_ms();
    if (g_defer.active && flush_locked()) read_deferred_locked();
    VkResult r = VK_NOT_READY;
    if (g_defer.ready) {
        g_defer.ready = 0;
        r = g_defer.r;
        out->n = 0;
        vr_reserve(out, g_defer.out.n);
        if (g_defer.out.n) memcpy(out->p, g_defer.out.p, g_defer.out.n);
        out->n = g_defer.out.n;
    }
    g_stat.wait_ms[VR_READ_JPEG_PIPE] += now_ms() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

static VkResult rpc_call(uint32_t op, vr_buf *b, vr_buf *out) {
    VkResult res = VK_ERROR_DEVICE_LOST;
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.calls[op]++;
    double t0 = now_ms();
    put_msg_locked(op, b);
    if (flush_locked()) {
        read_deferred_locked();
        uint32_t h[2];
        if (recv_head(h, op)) {
            vr_buf tmp = { 0 }, *dst = out ? out : &tmp;
            dst->n = 0;
            vr_reserve(dst, h[1]);
            if (h[1] == 0 || vr_recv_all(g_sock, dst->p, h[1])) {
                dst->n = h[1];
                res = (VkResult) (int32_t) h[0];
                g_stat.bytes_in += 8 + h[1];
            } else {
                conn_lost("返事の途中");
            }
            free(tmp.p);
        }
    }
    if (op < VR_OP_COUNT) g_stat.wait_ms[op] += now_ms() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    b->n = 0;
    return res;
}

// host:port に TCP でつなぐ。1.5 秒でつながらなければ諦める（iPhone がいないときにアプリを待たせない）
static vr_sock tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) return INVALID_SOCKET;
    vr_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, ai->ai_addr, (int) ai->ai_addrlen);
    freeaddrinfo(ai);
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    FD_ZERO(&ef);
    FD_SET(s, &ef);
    struct timeval tv = { 1, 500000 };
    int ok = select(0, NULL, &wf, &ef, &tv) == 1 && FD_ISSET(s, &wf);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (!ok) { closesocket(s); return INVALID_SOCKET; }
    DWORD to = 5000; // 最初のあいさつまでは 5 秒で諦める
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    return s;
}

// ---- USB の iPhone に直接つなぐ ----
// iTunes と一緒に入る Apple Mobile Device Service が 127.0.0.1:27015 で待っている（usbmuxd）。
// 「この iPhone のこの番号につないで」と頼むと、その接続がそのまま USB の通り道になる。
// Python の中継（usb_forward.py）を通さないので、1 段ぶん速い
static char *mux_call(vr_sock s, const char *type, const char *extra) {
    char body[1024];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>iphone-vk</string><key>ProgName</key><string>iphone-vk</string>%s</dict></plist>\n",
        type, extra);
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    if (!vr_send_all(s, h, sizeof h) || !vr_send_all(s, body, (size_t) n)) return NULL;
    if (!vr_recv_all(s, h, sizeof h) || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = malloc(h[0] - 16 + 1);
    if (!r) return NULL;
    if (!vr_recv_all(s, r, h[0] - 16)) { free(r); return NULL; }
    r[h[0] - 16] = 0;
    return r;
}

// <key>名前</key> の次の <integer> を読む。見つからなければ -1
static long long plist_int_after(const char *at, const char *key, const char *limit) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    if (!q || (limit && q > limit)) return -1;
    q = strstr(q, "<integer>");
    return q ? atoll(q + 9) : -1;
}

static vr_sock usb_connect(int port) {
    // 試験では IPHONE_VK_MUX=番号 で偽物の usbmuxd に向けられる
    const char *mp = getenv("IPHONE_VK_MUX");
    char mport[16];
    snprintf(mport, sizeof mport, "%s", mp && *mp ? mp : "27015");
    vr_sock s = tcp_connect("127.0.0.1", mport);
    if (s == INVALID_SOCKET) { say("Apple Mobile Device Service（127.0.0.1:27015）につながらない"); return s; }
    char *list = mux_call(s, "ListDevices", "");
    closesocket(s);
    if (!list) { say("usbmuxd が機器の一覧を返さない"); return INVALID_SOCKET; }
    // 機器ごとの Properties の中で、ConnectionType が USB のものの DeviceID を使う（Wi-Fi 越しの同期は使わない）
    long long dev = -1;
    for (const char *q = list; dev < 0 && (q = strstr(q, "<key>ConnectionType</key>")); q++) {
        const char *v = strstr(q, "<string>");
        const char *end = strstr(q, "</dict>");
        if (v && !strncmp(v + 8, "USB<", 4)) dev = plist_int_after(q, "DeviceID", end);
    }
    free(list);
    if (dev < 0) { say("USB でつながった iPhone が見えない"); return INVALID_SOCKET; }
    s = tcp_connect("127.0.0.1", mport);
    if (s == INVALID_SOCKET) return s;
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>",
             dev, (unsigned) htons((u_short) port));
    char *res = mux_call(s, "Connect", extra);
    long long num = res ? plist_int_after(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { closesocket(s); say("iPhone の %d 番につながらない（usbmuxd の返事 %lld）", port, num); return INVALID_SOCKET; }
    return s;
}

static int connect_locked(void) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    // IPHONE_VK：「usb」か「usb:番号」なら USB の iPhone に直接。それ以外は host:port
    const char *e = getenv("IPHONE_VK");
    char host[128] = "127.0.0.1", port[16] = "50061";
    vr_sock s;
    if (e && !strncmp(e, "usb", 3) && (e[3] == 0 || e[3] == ':')) {
        int p = e[3] == ':' ? atoi(e + 4) : 50060;
        s = usb_connect(p);
        snprintf(host, sizeof host, "USB の iPhone");
        snprintf(port, sizeof port, "%d", p);
    } else {
        if (e && *e) {
            const char *c = strrchr(e, ':');
            if (c) {
                size_t n = (size_t) (c - e) < sizeof host - 1 ? (size_t) (c - e) : sizeof host - 1;
                memcpy(host, e, n);
                host[n] = 0;
                snprintf(port, sizeof port, "%s", c + 1);
            } else {
                snprintf(host, sizeof host, "%s", e);
            }
        }
        s = tcp_connect(host, port);
    }
    if (s == INVALID_SOCKET) { say("%s:%s につながらない", host, port); return 0; }
    vr_nodelay(s);
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *) &big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *) &big, sizeof big);
    DWORD to = 5000; // 最初のあいさつだけは 5 秒で諦める
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    g_sock = s;
    say("%s:%s につないだ", host, port);
    return 1;
}

// ---- 受け手の GPU の性質（最初のあいさつで受け取る） ----
static struct {
    int tried, ok;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feats;
    VkPhysicalDeviceFeatures real; // 受け手の GPU が本当に持つ機能（名乗りだけの物を、装置を作るときに外すため）
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t nq;
    VkQueueFamilyProperties q[16];
    uint32_t ne;
    VkExtensionProperties ext[64];
    uint32_t caps; // 受け手ができること（VR_CAP_*）
} g_hw;
static SRWLOCK g_hw_lock = SRWLOCK_INIT;

// 新しい DXVK（2 以上）が必須にする物のうち、ドライバがまだ全部は運べない物も名乗る
// （descriptor indexing の 5 つ・inline uniform block・maintenance5 と 6）。確かめ終わったら外す。
// アプリの情報のエンジンの名前が DXVK で版が 2 以上なら自動で名乗る。IPHONE_VK_DXVK3=1 で必ず・=0 で名乗らない
static int g_engine_dxvk2;
static int dxvk3(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("IPHONE_VK_DXVK3");
        v = e && *e ? *e == '1' : g_engine_dxvk2;
        if (v && !(e && *e)) say("新しい DXVK なので、DXVK 3 のための名乗りを入れる");
    }
    return v;
}

// 中継するだけで済む（新しい入口が要らない）拡張だけを見せる
static const char *const ALLOWED_EXT[] = {
    "VK_KHR_16bit_storage", "VK_KHR_8bit_storage", "VK_KHR_shader_float16_int8", "VK_KHR_storage_buffer_storage_class",
    "VK_EXT_subgroup_size_control", "VK_KHR_shader_integer_dot_product", "VK_KHR_shader_non_semantic_info",
    "VK_KHR_driver_properties", "VK_KHR_spirv_1_4", "VK_KHR_shader_float_controls", "VK_KHR_shader_subgroup_extended_types",
    "VK_KHR_variable_pointers", "VK_KHR_uniform_buffer_standard_layout", "VK_EXT_scalar_block_layout",
    "VK_KHR_vulkan_memory_model", "VK_KHR_shader_atomic_int64", "VK_KHR_dedicated_allocation",
    "VK_KHR_get_memory_requirements2", "VK_KHR_bind_memory2", "VK_KHR_maintenance1", "VK_KHR_maintenance3",
    "VK_KHR_relaxed_block_layout", "VK_KHR_shader_draw_parameters", "VK_KHR_timeline_semaphore",
    // DXVK が使う物。中身はシェーダーと値だけなので、受け手の装置で有効にすれば通る
    "VK_KHR_image_format_list", "VK_KHR_sampler_mirror_clamp_to_edge",
    "VK_EXT_shader_stencil_export", "VK_EXT_shader_viewport_index_layer",
    // attachment の読み書きをしない印（値を運ぶだけ）
    "VK_KHR_load_store_op_none", "VK_EXT_load_store_op_none",
};

// 受け手が caps で名乗ったときだけ見せる拡張（受け手の装置でも有効にする）
static const struct { const char *name; uint32_t cap; } CAP_EXT[] = {
    { "VK_KHR_dynamic_rendering", VR_CAP_DYNAMIC_RENDERING }, { "VK_KHR_synchronization2", VR_CAP_SYNC2 },
    { "VK_KHR_create_renderpass2", VR_CAP_RENDER_PASS2 }, { "VK_EXT_extended_dynamic_state", VR_CAP_EXT_DYNAMIC_STATE },
    { "VK_EXT_extended_dynamic_state2", VR_CAP_EXT_DYNAMIC_STATE }, { "VK_EXT_host_query_reset", VR_CAP_QUERY },
};

static int allowed_ext(const char *name) {
    if (dxvk3() && (!strcmp(name, "VK_KHR_maintenance5") || !strcmp(name, "VK_KHR_maintenance6"))) return 1;
    for (size_t i = 0; i < sizeof ALLOWED_EXT / sizeof *ALLOWED_EXT; i++)
        if (!strcmp(name, ALLOWED_EXT[i])) return 1;
    for (size_t i = 0; i < sizeof CAP_EXT / sizeof *CAP_EXT; i++)
        if (!strcmp(name, CAP_EXT[i].name)) return (g_hw.caps & CAP_EXT[i].cap) != 0;
    return 0;
}

// 1.3 と名乗っているか（受け手が VR_CAP_VK13 を名乗った）
static int vk13(void) { return (g_hw.caps & VR_CAP_VK13) != 0; }

static int g_bc; // 1 なら、BC の圧縮テクスチャをドライバがほどいて、ふつうの画素の画像として受け手に送る

// ---- BC の圧縮テクスチャをほどく ----
// iPhone の GPU は BC（DirectX のゲームがよく使う圧縮）を読めないので、受け手にはふつうの画素の画像を作らせ、
// アプリが BC のまま送ってくる中身は、写す命令を記録するときに CPU でほどいてから送る
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/bcdec/bcdec.h"
#pragma GCC diagnostic pop

static VkFormat bc_sub(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static uint32_t bc_block_bytes(VkFormat f) {
    return f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
                   f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK
               ? 8 : 16;
}

static uint32_t texel_bytes(VkFormat sub) {
    return sub == VK_FORMAT_R8_UNORM || sub == VK_FORMAT_R8_SNORM ? 1
         : sub == VK_FORMAT_R8G8_UNORM || sub == VK_FORMAT_R8G8_SNORM ? 2
         : sub == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
}

// 1 つの塊（4x4 の画素）をほどいて、tile に 1 行 4 * tb バイトで並べる
static void bc_block(VkFormat f, const uint8_t *blk, uint8_t *tile, uint32_t tb) {
    int pitch = (int) (4 * tb);
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        bcdec_bc1(blk, tile, pitch);
        for (int i = 0; i < 16; i++) tile[i * 4 + 3] = 255; // 透明の無い BC1
        break;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: bcdec_bc1(blk, tile, pitch); break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: bcdec_bc2(blk, tile, pitch); break;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: bcdec_bc3(blk, tile, pitch); break;
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: bcdec_bc7(blk, tile, pitch); break;
    case VK_FORMAT_BC4_UNORM_BLOCK: bcdec_bc4(blk, tile, pitch, 0); break;
    case VK_FORMAT_BC4_SNORM_BLOCK: bcdec_bc4(blk, tile, pitch, 1); break;
    case VK_FORMAT_BC5_UNORM_BLOCK: bcdec_bc5(blk, tile, pitch, 0); break;
    case VK_FORMAT_BC5_SNORM_BLOCK: bcdec_bc5(blk, tile, pitch, 1); break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
        // bcdec は R・G・B の half を返す（行の長さは half の数）。A に 1.0 を足して 4 つにする
        uint16_t rgb[16 * 3];
        bcdec_bc6h_half(blk, rgb, 4 * 3, f == VK_FORMAT_BC6H_SFLOAT_BLOCK);
        uint16_t *o = (uint16_t *) tile;
        for (int i = 0; i < 16; i++) { o[i * 4] = rgb[i * 3]; o[i * 4 + 1] = rgb[i * 3 + 1]; o[i * 4 + 2] = rgb[i * 3 + 2]; o[i * 4 + 3] = 0x3C00; }
        break;
    }
    default: break;
    }
}

static void mask_features(VkPhysicalDeviceFeatures *f) {
    if (g_bc) f->textureCompressionBC = VK_TRUE;
    // 新しい DXVK は geometry shader と cull distance を必須にする。iPhone の GPU は持たないので名乗りだけする。
    // geometry shader を使う pipeline は作らずに失敗を返す（DXVK はその描画を飛ばす）
    if (dxvk3()) f->geometryShader = f->shaderCullDistance = VK_TRUE;
    // 中継していない機能は「無い」と答える
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = f->sparseResidency4Samples = f->sparseResidency8Samples = f->sparseResidency16Samples = VK_FALSE;
    f->sparseResidencyAliased = f->pipelineStatisticsQuery = f->inheritedQueries = VK_FALSE;
    if (!(g_hw.caps & VR_CAP_QUERY)) f->occlusionQueryPrecise = VK_FALSE;
}

static int hello(void) {
    AcquireSRWLockExclusive(&g_hw_lock);
    if (g_hw.tried) { ReleaseSRWLockExclusive(&g_hw_lock); return g_hw.ok; }
    g_hw.tried = 1;
    if (g_stats < 0) {
        const char *e = getenv("IPHONE_VK_STATS");
        g_stats = e && *e && *e != '0';
        if (g_stats) atexit(print_stats);
    }
    AcquireSRWLockExclusive(&g_lock);
    int c = connect_locked();
    ReleaseSRWLockExclusive(&g_lock);
    if (c) {
        vr_buf b = { 0 }, out = { 0 };
        vr_u32(&b, VR_PROTO_VERSION);
        VkResult r = rpc_call(VR_HELLO, &b, &out);
        if (r == VK_SUCCESS) {
            vr_rd rd = { out.p, out.n, 0, 0 };
            vr_rcopy(&rd, &g_hw.props, sizeof g_hw.props);
            vr_rcopy(&rd, &g_hw.feats, sizeof g_hw.feats);
            g_hw.real = g_hw.feats;
            vr_rcopy(&rd, &g_hw.mem, sizeof g_hw.mem);
            uint32_t nq = vr_ru32(&rd);
            for (uint32_t i = 0; i < nq; i++) {
                VkQueueFamilyProperties q;
                vr_rcopy(&rd, &q, sizeof q);
                if (i < 16) g_hw.q[g_hw.nq++] = q;
            }
            uint32_t ne = vr_ru32(&rd);
            // 拡張の並びの後ろにある caps を先に読む（caps によって見せる拡張が変わる）
            size_t ext_at = rd.at;
            vr_take(&rd, (size_t) ne * sizeof(VkExtensionProperties));
            if (!rd.bad && rd.at + 4 <= rd.n) g_hw.caps = vr_ru32(&rd); // 古い受け手は付けない
            g_can_pack = (g_hw.caps & VR_CAP_PACK) != 0;
            const char *dl = getenv("IPHONE_VK_DELTA");
            if (dl && *dl == '0') g_hw.caps &= ~VR_CAP_DELTA;
            // 調べ用：IPHONE_VK_CAPS=数 で受け手の caps を絞る（0 なら古い受け手と同じに振る舞う）
            const char *cm = getenv("IPHONE_VK_CAPS");
            if (cm && *cm) g_hw.caps &= (uint32_t) strtoul(cm, NULL, 0);
            rd.at = ext_at;
            for (uint32_t i = 0; i < ne; i++) {
                VkExtensionProperties x;
                vr_rcopy(&rd, &x, sizeof x);
                if (allowed_ext(x.extensionName) && g_hw.ne < 62) g_hw.ext[g_hw.ne++] = x;
            }
            if (g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_swapchain", 70 };
            // ドライバの中だけで済む拡張（受け手には送らない）
            if ((g_hw.caps & VR_CAP_NEW_MASK) && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_copy_commands2", 1 };
            // ひな形での descriptor の書き込みは、ドライバがふつうの書き込みに直す（zink が名前で確かめる）
            if (g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_descriptor_update_template", 1 };
            // push descriptor は、ドライバがふつうの set に直す
            if (g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_push_descriptor", 2 };
            // depth clip は pipeline を作るときに depth clamp に置き換える（DXVK 2 が拡張の無い GPU でしていたのと同じ）
            if (dxvk3() && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_EXT_depth_clip_enable", 1 };
            // 空の descriptor は受け手が受ける（GPU が持たなければ代わりの物で）
            if (dxvk3() && (g_hw.caps & VR_CAP_NULL_DESC) && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_EXT_robustness2", 1 };
            if (!rd.bad) {
                char real[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
                memcpy(real, g_hw.props.deviceName, sizeof real);
                real[sizeof real - 1] = 0;
                snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "%.200s (iPhone-RPC)", real);
                // 受け手が 1.3 の入口をそろえて名乗ったときだけ 1.3。古い受け手には今まで通り 1.2
                uint32_t top = vk13() ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
                if (g_hw.props.apiVersion > top)
                    g_hw.props.apiVersion = VK_MAKE_API_VERSION(0, 1, VK_API_VERSION_MINOR(top), VK_API_VERSION_PATCH(g_hw.props.apiVersion));
                if (g_hw.caps & ~VR_CAP_DELTA) say("受け手ができること（caps）0x%x", g_hw.caps);
                // GPU が BC を読めなければ、ドライバがほどく（IPHONE_VK_BC=1 で、読める GPU でもほどく・0 でほどかない）
                const char *bce = getenv("IPHONE_VK_BC");
                g_bc = bce && *bce ? *bce == '1' : !g_hw.feats.textureCompressionBC;
                if (g_bc) say("BC の圧縮テクスチャはドライバがほどく");
                mask_features(&g_hw.feats);
                // PC から見ると iPhone のメモリは USB の先にあるので、「別のメモリを持つ GPU」と名乗る。
                // 共有メモリの GPU（integrated）と名乗ると、llama.cpp などはデータを CPU から直接書ける所に置こうとして、
                // 写しの送り返しが毎回モデル全部になってしまう
                g_hw.props.deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                for (uint32_t i = 0; i < g_hw.mem.memoryTypeCount; i++)
                    if (g_hw.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                        g_hw.mem.memoryTypes[i].propertyFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                for (uint32_t i = 0; i < g_hw.nq; i++) {
                    g_hw.q[i].queueFlags &= VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
                }
                g_hw.ok = 1;
                say("受け手の GPU：%s", g_hw.props.deviceName);
                // あいさつが済んだら、計算を待つ間は切らない
                DWORD to = 0;
                setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
            }
        } else {
            say("あいさつに返事がない (%d)", r);
        }
        free(b.p);
        free(out.p);
    }
    ReleaseSRWLockExclusive(&g_hw_lock);
    return g_hw.ok;
}

// ---- PC 側で覚えておく物 ----
struct phys { VK_LOADER_DATA loader; };
struct inst { VK_LOADER_DATA loader; struct phys pd; };

struct mem {
    uint64_t size;
    uint32_t type;
    int host;          // CPU から見える（写しを持つ）
    uint8_t *shadow;   // 写し
    size_t npages;
    void **addrs;      // GetWriteWatch の受け皿
    uint8_t *dirty;    // ページごとの「まだ送っていない」印
    int delta;         // 窓に出すコマ用：受け手に前回との違いだけを送ってもらう（VR_MEM_READ_DELTA）
    int jpeg;          // 窓に出すコマ用：引き取らずに、受け手に JPEG に縮めて送ってもらう（VR_READ_JPEG）
    uint8_t *sent;     // 受け手が持っている中身の写し。書かれたページは、これと比べて変わった所だけ送る
    uint8_t *known;    // ページごとの「受け手の中身を知っている」印。知らないページは丸ごと送る
    struct mem *prev, *next;
};

struct buf { uint64_t size; VkMemoryRequirements req; struct mem *m; uint64_t memoff; uint64_t addr; }; // addr：受け手の GPU の中のアドレス

typedef struct { struct mem *m; uint64_t off, len, serial; struct queue *q; } rng;

struct dev {
    VK_LOADER_DATA loader;
    struct mem *mems;  // CPU から見えるメモリの一覧
    struct queue *queues[16];
    rng *pend;         // GPU が書いたかもしれない範囲（終わったら引き取る）
    size_t npend, cappend;
    uint64_t serial;   // submit の通し番号
    SRWLOCK lock;
};

struct queue { VK_LOADER_DATA loader; struct dev *d; uint32_t fam, idx; };

struct dent { uint32_t binding, elem, type; struct buf *b; uint64_t off, range; uint64_t h0, h1; uint32_t layout; };
// h0・h1：画像なら sampler と image view、texel buffer なら buffer view（descriptor の写しを、ふつうの書き込みに直すのに使う）
struct dpool;
// descriptor set の形。pool の残りを数えるため、種類ごとの数を覚えておく。
// 形を消したあとも、その形で取った set が残っていれば数を使うので、数え札（refs）で持つ
struct dsl {
    volatile LONG refs;
    uint32_t nt;
    struct { uint32_t type, count; } t[16];
    int push;               // push descriptor 用の形（受け手にはふつうの形として作る）
    VkDescriptorSet *free;  // push に使い終えて、使い回せる set
    uint32_t nfree, capfree;
    // immutable sampler を持つ binding。受け手の形には送らないので、書き込みのときにドライバが差し込む
    uint32_t nimm;
    struct { uint32_t binding, type, count; VkSampler *s; } *imm;
};
struct dset { struct dpool *pool; struct dset *next; uint32_t n, cap; struct dent *e; struct dsl *layout; };
// 受け手に頼まずに set を取るので、pool の残り（set の数と種類ごとの数）はドライバが数える
struct dpool { struct dset *head; uint32_t max_sets, used_sets, nsz; struct { uint32_t type, total, used; } sz[16]; };

static void dsl_unref(struct dsl *l) {
    if (l && InterlockedDecrement(&l->refs) == 0) {
        for (uint32_t i = 0; i < l->nimm; i++) free(l->imm[i].s);
        free(l->imm);
        free(l);
    }
}

// 形の binding の elem 番目に決まった sampler（immutable sampler）があれば返す。無ければ VK_NULL_HANDLE
static VkSampler dsl_imm(const struct dsl *l, uint32_t binding, uint32_t elem) {
    if (!l) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < l->nimm; i++)
        if (l->imm[i].binding == binding) return elem < l->imm[i].count ? l->imm[i].s[elem] : VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
}

static int pool_slot(struct dpool *p, uint32_t type) {
    for (uint32_t j = 0; j < p->nsz; j++) if (p->sz[j].type == type) return (int) j;
    return -1;
}

struct cpool { uint32_t fam; struct cb *head; };
struct cb {
    VK_LOADER_DATA loader;
    struct dev *d;
    struct cpool *pool;
    struct cb *next;
    vr_buf s;          // 記録した命令
    uint32_t flags;
    uint32_t level;    // 2 次（SECONDARY）なら受け手には送らず、vkCmdExecuteCommands で呼んだ側に中身を写す
    rng *r;            // この command buffer で GPU が書くかもしれない範囲
    size_t nr, capr;
    rng *rd;           // GPU が読むかもしれない範囲（submit の前に、ここだけ送る）
    size_t nrd, caprd;
    struct bctmp *bct; // BC をほどいた中身を入れた一時バッファ（記録し直すときに消す）
    const struct gs_pipe *gs; // いまつないでいる、GS を真似た pipeline（無ければ NULL）
    uint32_t app_topo;        // アプリが vkCmdSetPrimitiveTopology で決めた形
    int app_topo_set;
    struct gs_cbst *gsx;      // GS を真似るための、頂点・index の buffer の覚えと、記録・descriptor の一時の物
    struct evact *ev;         // submit のときに立てる・倒す event（記録した順）
    size_t nev, cev;
    struct pushheld *ph;      // push descriptor に使った set（記録し直すか消すまで持つ）
    size_t nph, cph;
};
struct pushheld { struct dsl *l; VkDescriptorSet s; };
static void push_release(struct cb *c);
struct event;
struct evact { struct event *e; int val; };
static void ev_fire(struct cb *c);
struct bctmp { VkBuffer b; VkDeviceMemory m; struct bctmp *next; };

struct fence { uint64_t serial; struct queue *q; double last_poll; };

// timeline semaphore は、どの値をどのキューの何番目の submit が知らせるかを覚える（待ちが済んだら引き取りに使う）
typedef struct { uint64_t value, serial; struct queue *q; } sigrec;
struct sem { int timeline; sigrec *sig; size_t n, cap; uint64_t reached; };

// ---- 範囲の並び ----
static void rng_add(rng **a, size_t *n, size_t *cap, struct mem *m, uint64_t off, uint64_t len, uint64_t serial) {
    if (!m || !m->host || !len) return;
    if (off >= m->size) return;
    if (len > m->size - off) len = m->size - off;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *a = realloc(*a, *cap * sizeof **a);
    }
    (*a)[(*n)++] = (rng) { m, off, len, serial, NULL };
}

static int rng_cmp(const void *x, const void *y) {
    const rng *a = x, *b = y;
    if (a->m != b->m) return (uintptr_t) a->m < (uintptr_t) b->m ? -1 : 1;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return 0;
}

// 重なる範囲をまとめる（serial は大きい方を残す）
static void rng_merge(rng *a, size_t *n) {
    if (*n < 2) return;
    qsort(a, *n, sizeof *a, rng_cmp);
    size_t k = 0;
    for (size_t i = 1; i < *n; i++) {
        rng *last = &a[k];
        if (a[i].m == last->m && a[i].off <= last->off + last->len) {
            uint64_t end = a[i].off + a[i].len;
            if (end > last->off + last->len) last->len = end - last->off;
            if (a[i].serial > last->serial) last->serial = a[i].serial;
        } else {
            a[++k] = a[i];
        }
    }
    *n = k + 1;
}

// ---- メモリの写し ----
// ページ p0〜p1 の書き込み監視を見て、書かれたページに印を付ける（監視の印は消える）
static void collect_dirty(struct mem *m, size_t p0, size_t p1) {
    if (p1 > m->npages) p1 = m->npages;
    if (p0 >= p1) return;
    ULONG_PTR count = p1 - p0;
    DWORD gran = 0;
    double t0 = now_ms();
    UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, m->shadow + p0 * PAGE, (p1 - p0) * PAGE, m->addrs, &count, &gran);
    g_stat.ww_calls++;
    g_stat.ww_ms += now_ms() - t0;
    if (r != 0) return;
    g_stat.ww_pages += count;
    for (ULONG_PTR i = 0; i < count; i++) m->dirty[(size_t) ((uint8_t *) m->addrs[i] - m->shadow) / PAGE] = 1;
}

// 書いたページの中で、受け手の中身と違う所だけを送るか（IPHONE_VK_WDIFF=0 で、今までどおりページを丸ごと送る）
static int wdiff_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_WDIFF"); v = !(e && *e == '0'); }
    return v;
}

static void send_write(vr_buf *b, struct mem *m, uint64_t off, uint64_t len) {
    vr_u64(b, ID(m));
    vr_u64(b, off);
    vr_bytes(b, m->shadow + off, (uint32_t) len);
    rpc_send(VR_MEM_WRITE, b);
    b->n = 0;
}

// 1 ページを、受け手の中身の写しと 8 バイトずつ比べ、違う所だけを送る。64 バイト未満のすき間はまとめて送る
static void send_page_diff(vr_buf *b, struct mem *m, size_t pg) {
    uint64_t base = (uint64_t) pg * PAGE;
    size_t plen = (size_t) (m->size - base < PAGE ? m->size - base : PAGE);
    const uint8_t *cur = m->shadow + base;
    uint8_t *old = m->sent + base;
    size_t a = 0;
    while (a < plen) {
        size_t w = plen - a < 8 ? plen - a : 8;
        if (!memcmp(cur + a, old + a, w)) { a += w; continue; }
        size_t e = a + w;
        while (e < plen) {
            size_t g = e;
            while (g < plen) {
                size_t w2 = plen - g < 8 ? plen - g : 8;
                if (memcmp(cur + g, old + g, w2)) break;
                g += w2;
            }
            if (g >= plen || g - e >= 64) break;
            size_t w3 = plen - g < 8 ? plen - g : 8;
            e = g + w3;
        }
        if (e > plen) e = plen;
        send_write(b, m, base + a, e - a);
        memcpy(old + a, cur + a, e - a);
        g_stat.wdiff_sent += e - a;
        a = e;
    }
    g_stat.wdiff_pages++;
}

// off から len バイトのうち、アプリが書いたページを受け手へ送る
static void send_dirty(struct mem *m, uint64_t off, uint64_t len) {
    if (off >= m->size) return;
    if (len > m->size - off) len = m->size - off;
    size_t p0 = (size_t) (off / PAGE), p1 = (size_t) ((off + len + PAGE - 1) / PAGE);
    if (p1 > m->npages) p1 = m->npages;
    collect_dirty(m, p0, p1);
    const size_t chunk = (16u << 20) / PAGE;
    vr_buf b = { 0 };
    if (wdiff_on() && !m->sent && !m->delta && !m->jpeg) {
        m->sent = malloc(m->npages * PAGE);
        m->known = calloc(m->npages, 1);
        if (!m->sent || !m->known) { free(m->sent); free(m->known); m->sent = m->known = NULL; }
    }
    for (size_t i = p0; i < p1;) {
        if (!m->dirty[i]) { i++; continue; }
        // 受け手の中身を知っているページは、違う所だけを送る
        if (m->sent && m->known[i]) {
            m->dirty[i] = 0;
            send_page_diff(&b, m, i);
            i++;
            continue;
        }
        size_t j = i;
        while (j < p1 && m->dirty[j] && j - i < chunk && !(m->sent && m->known[j])) m->dirty[j++] = 0;
        if (m->sent) {
            // 初めて送るページは丸ごと送り、写しを取っておく
            uint64_t o = (uint64_t) i * PAGE, l = (uint64_t) (j - i) * PAGE;
            if (o + l > m->size) l = m->size - o;
            memcpy(m->sent + o, m->shadow + o, (size_t) l);
            memset(m->known + i, 1, j - i);
        }
        uint64_t off = (uint64_t) i * PAGE, len = (uint64_t) (j - i) * PAGE;
        if (off + len > m->size) len = m->size - off;
        vr_u64(&b, ID(m));
        vr_u64(&b, off);
        vr_bytes(&b, m->shadow + off, (uint32_t) len);
        rpc_send(VR_MEM_WRITE, &b);
        i = j;
    }
    free(b.p);
}

// いくつもの範囲を 1 回の往復で引き取る。頼みを全部送ってから、答えを順に受け取る
static void read_back_many(rng *r, size_t n) {
    const uint64_t piece = 64u << 20;
    vr_buf b = { 0 };
    size_t nreq = 0;
    // アプリが書いた印を先に取っておく（引き取りで消えないように）
    for (size_t i = 0; i < n; i++) {
        struct mem *m = r[i].m;
        if (r[i].off >= m->size) { r[i].len = 0; continue; }
        if (r[i].len > m->size - r[i].off) r[i].len = m->size - r[i].off;
        collect_dirty(m, (size_t) (r[i].off / PAGE), (size_t) ((r[i].off + r[i].len + PAGE - 1) / PAGE));
    }
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < n; i++)
        for (uint64_t at = r[i].off; at < r[i].off + r[i].len; at += piece) {
            uint64_t k = r[i].off + r[i].len - at < piece ? r[i].off + r[i].len - at : piece;
            vr_u64(&b, ID(r[i].m));
            vr_u64(&b, at);
            vr_u64(&b, k);
            uint32_t op = r[i].m->delta && (g_hw.caps & VR_CAP_DELTA) ? VR_MEM_READ_DELTA : VR_MEM_READ;
            g_stat.calls[op]++;
            put_msg_locked(op, &b);
            b.n = 0;
            nreq++;
        }
    double t0 = now_ms();
    int ok = flush_locked();
    if (ok) read_deferred_locked();
    for (size_t i = 0; i < n && ok; i++)
        for (uint64_t at = r[i].off; at < r[i].off + r[i].len && ok; at += piece) {
            uint64_t k = r[i].off + r[i].len - at < piece ? r[i].off + r[i].len - at : piece;
            uint32_t h[2];
            if (!recv_head(h, VR_MEM_READ)) { ok = 0; break; }
            if ((int32_t) h[0] < 0) {
                if (!g_dead) say("受け手が失敗を返した (%d)。この先は全部失敗にする", (int32_t) h[0]);
                g_dead = 1;
            }
            if ((int32_t) h[0] == VK_SUCCESS && r[i].m->delta && (g_hw.caps & VR_CAP_DELTA)) {
                // 形（0 そのまま・1 違い）を読んでから中身
                uint8_t *buf = malloc(h[1] ? h[1] : 1);
                if (!buf || (h[1] && !vr_recv_all(g_sock, buf, h[1]))) { free(buf); ok = 0; conn_lost("読み出しの途中"); break; }
                g_stat.bytes_in += 8 + h[1];
                uint32_t mode = 2;
                if (h[1] >= 4) memcpy(&mode, buf, 4);
                uint8_t *dst = r[i].m->shadow + at;
                if (mode == 0 && h[1] - 4 == k) {
                    memcpy(dst, buf + 4, (size_t) k);
                } else if (mode == 1) {
                    uint32_t *w = (uint32_t *) dst;
                    size_t words = (size_t) (k / 4), pos = 0, q = 4;
                    while (q + 8 <= h[1]) {
                        uint32_t z, lits;
                        memcpy(&z, buf + q, 4);
                        memcpy(&lits, buf + q + 4, 4);
                        q += 8;
                        pos += z;
                        if (pos + lits > words || q + (size_t) lits * 4 > h[1]) { g_dead = 1; break; }
                        for (uint32_t t = 0; t < lits; t++, pos++, q += 4) {
                            uint32_t x;
                            memcpy(&x, buf + q, 4);
                            w[pos] ^= x;
                        }
                    }
                } else {
                    g_dead = 1;
                }
                if (g_dead) say("違いの読み出しが壊れていた。この先は全部失敗にする");
                else if (r[i].m->sent) memcpy(r[i].m->sent + at, r[i].m->shadow + at, (size_t) k);
                free(buf);
                continue;
            }
            if ((int32_t) h[0] == VK_SUCCESS && h[1] == k) {
                if (!vr_recv_all(g_sock, r[i].m->shadow + at, k)) { ok = 0; conn_lost("読み出しの途中"); break; }
                if (r[i].m->sent) memcpy(r[i].m->sent + at, r[i].m->shadow + at, (size_t) k);
            } else {
                // 読めなかった：中身は捨てる
                uint8_t tmp[4096];
                for (uint32_t left = h[1]; left;) {
                    uint32_t c = left < sizeof tmp ? left : (uint32_t) sizeof tmp;
                    if (!vr_recv_all(g_sock, tmp, c)) { ok = 0; conn_lost("読み出しの途中"); break; }
                    left -= c;
                }
            }
            g_stat.bytes_in += 8 + h[1];
        }
    g_stat.wait_ms[VR_MEM_READ] += now_ms() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    free(b.p);
    (void) nreq;
    // 自分で書いたところの監視の印は消す
    for (size_t i = 0; i < n; i++) {
        if (!r[i].len) continue;
        uint64_t p0 = r[i].off / PAGE * PAGE, p1 = (r[i].off + r[i].len + PAGE - 1) / PAGE * PAGE;
        ResetWriteWatch(r[i].m->shadow + p0, (SIZE_T) (p1 - p0));
    }
}

// キュー q（NULL なら全部）の serial までの submit が終わった：GPU が書いた範囲を引き取る
static void complete(struct dev *d, struct queue *q, uint64_t upto) {
    AcquireSRWLockExclusive(&d->lock);
    rng *take = NULL;
    size_t nt = 0, ct = 0, k = 0;
    for (size_t i = 0; i < d->npend; i++) {
        if (d->pend[i].serial <= upto && (!q || d->pend[i].q == q)) {
            if (!d->pend[i].m->jpeg) rng_add(&take, &nt, &ct, d->pend[i].m, d->pend[i].off, d->pend[i].len, d->pend[i].serial);
        }
        else d->pend[k++] = d->pend[i];
    }
    d->npend = k;
    ReleaseSRWLockExclusive(&d->lock);
    rng_merge(take, &nt);
    double t0 = now_ms();
    if (nt) read_back_many(take, nt);
    g_stat.rb_ms += now_ms() - t0;
    free(take);
}

// ---- インスタンスと装置の性質 ----
static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void) a;
    const VkApplicationInfo *ai = ci ? ci->pApplicationInfo : NULL;
    if (ai && ai->pEngineName) {
        uint32_t major = (ai->engineVersion >> 22) & 0x7F; // VK_MAKE_VERSION の形
        say("アプリのエンジン：%s %u.%u.%u", ai->pEngineName, major, (ai->engineVersion >> 12) & 0x3FF, ai->engineVersion & 0xFFF);
        if (!strcmp(ai->pEngineName, "DXVK") && major >= 2) g_engine_dxvk2 = 1;
    }
    struct inst *i = calloc(1, sizeof *i);
    if (!i) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(i);
    set_loader_magic_value(&i->pd);
    *out = (VkInstance) i;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_instance(VkInstance i, const VkAllocationCallbacks *a) {
    (void) a;
    free(i);
}

static VkResult VKAPI_CALL enum_instance_ext(const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void) layer;
    static const VkExtensionProperties ext[] = {
        { "VK_KHR_get_physical_device_properties2", 2 }, { "VK_KHR_surface", 25 }, { "VK_KHR_win32_surface", 6 } };
    const uint32_t all = sizeof ext / sizeof *ext;
    if (!p) { *n = all; return VK_SUCCESS; }
    uint32_t k = *n < all ? *n : all;
    memcpy(p, ext, k * sizeof *p);
    *n = k;
    return k < all ? VK_INCOMPLETE : VK_SUCCESS;
}

// instance の版は受け手につなぐ前に聞かれるので、1.3 と答えておく（装置の版は受け手しだいで 1.2 か 1.3）
static VkResult VKAPI_CALL enum_instance_version(uint32_t *v) {
    *v = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL enum_phys(VkInstance inst, uint32_t *n, VkPhysicalDevice *out) {
    struct inst *i = (struct inst *) inst;
    uint32_t have = hello() ? 1 : 0; // iPhone がいなければ 0 台
    if (!out) { *n = have; return VK_SUCCESS; }
    if (have && *n < 1) return VK_INCOMPLETE;
    if (have) out[0] = (VkPhysicalDevice) &i->pd;
    *n = have;
    return VK_SUCCESS;
}

static void VKAPI_CALL get_props(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void) pd;
    *p = g_hw.props;
}

static void VKAPI_CALL get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    say("機能の一覧");
    (void) pd;
    *f = g_hw.feats;
}

typedef struct { VkStructureType st; uint32_t size; } stinfo;

static const stinfo FEAT_ST[] = {
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, sizeof(VkPhysicalDeviceVulkan11Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, sizeof(VkPhysicalDeviceVulkan12Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, sizeof(VkPhysicalDevice16BitStorageFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, sizeof(VkPhysicalDevice8BitStorageFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, sizeof(VkPhysicalDeviceShaderFloat16Int8Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, sizeof(VkPhysicalDeviceVariablePointersFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, sizeof(VkPhysicalDeviceShaderAtomicInt64Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, sizeof(VkPhysicalDeviceShaderDrawParametersFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures) },
    // 1.3 に向けて足した（受け手の caps で 0 にする）
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, sizeof(VkPhysicalDeviceVulkan13Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, sizeof(VkPhysicalDeviceDynamicRenderingFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, sizeof(VkPhysicalDeviceSynchronization2Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, sizeof(VkPhysicalDeviceHostQueryResetFeatures) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES, sizeof(VkPhysicalDeviceMaintenance5Features) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES, sizeof(VkPhysicalDeviceMaintenance6Features) },
};

// 1.3 に向けて足した機能の構造体。装置を作るときは、中継できない機能を 0 にしてから送る
static int new_feature_st(VkStructureType st) {
    return st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES || st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES ||
           st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES ||
           st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT ||
           st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT || st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES ||
           st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES || st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES;
}

static const stinfo PROP_ST[] = {
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, sizeof(VkPhysicalDeviceVulkan11Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, sizeof(VkPhysicalDeviceVulkan12Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, sizeof(VkPhysicalDeviceSubgroupProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, sizeof(VkPhysicalDeviceDriverProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, sizeof(VkPhysicalDeviceSubgroupSizeControlProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, sizeof(VkPhysicalDeviceMaintenance3Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, sizeof(VkPhysicalDeviceFloatControlsProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES, sizeof(VkPhysicalDeviceShaderIntegerDotProductProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, sizeof(VkPhysicalDeviceIDProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES, sizeof(VkPhysicalDeviceTimelineSemaphoreProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, sizeof(VkPhysicalDeviceVulkan13Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES, sizeof(VkPhysicalDeviceDepthStencilResolveProperties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, sizeof(VkPhysicalDeviceMaintenance4Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES, sizeof(VkPhysicalDeviceMaintenance5Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES, sizeof(VkPhysicalDeviceMaintenance6Properties) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT, sizeof(VkPhysicalDeviceRobustness2PropertiesEXT) },
};

static uint32_t st_size(const stinfo *t, size_t n, VkStructureType st) {
    for (size_t i = 0; i < n; i++) if (t[i].st == st) return t[i].size;
    return 0;
}

#define BODY(p) ((uint8_t *) (p) + sizeof(VkBaseOutStructure))

static void mask_ext_features(VkBaseOutStructure *s) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
        VkPhysicalDeviceVulkan11Features *f = (void *) s;
        f->multiview = f->multiviewGeometryShader = f->multiviewTessellationShader = VK_FALSE;
        f->protectedMemory = f->samplerYcbcrConversion = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
        // 中継できる機能だけ残す
        VkPhysicalDeviceVulkan12Features k = *(VkPhysicalDeviceVulkan12Features *) s, *f = (void *) s;
        memset(BODY(f), 0, sizeof *f - sizeof(VkBaseOutStructure));
        f->storageBuffer8BitAccess = k.storageBuffer8BitAccess;
        f->uniformAndStorageBuffer8BitAccess = k.uniformAndStorageBuffer8BitAccess;
        f->storagePushConstant8 = k.storagePushConstant8;
        f->shaderBufferInt64Atomics = k.shaderBufferInt64Atomics;
        f->shaderSharedInt64Atomics = k.shaderSharedInt64Atomics;
        f->shaderFloat16 = k.shaderFloat16;
        f->shaderInt8 = k.shaderInt8;
        f->scalarBlockLayout = k.scalarBlockLayout;
        f->uniformBufferStandardLayout = k.uniformBufferStandardLayout;
        f->shaderSubgroupExtendedTypes = k.shaderSubgroupExtendedTypes;
        f->vulkanMemoryModel = k.vulkanMemoryModel;
        f->vulkanMemoryModelDeviceScope = k.vulkanMemoryModelDeviceScope;
        f->vulkanMemoryModelAvailabilityVisibilityChains = k.vulkanMemoryModelAvailabilityVisibilityChains;
        f->subgroupBroadcastDynamicId = k.subgroupBroadcastDynamicId;
        f->timelineSemaphore = k.timelineSemaphore;
        if (g_hw.caps & VR_CAP_QUERY) f->hostQueryReset = k.hostQueryReset;
        f->samplerMirrorClampToEdge = k.samplerMirrorClampToEdge; // sampler の値を運ぶだけ
        if (dxvk3()) {
            // binding ごとの flags はまだ運ばない。受け手は set を書き換える前に、その set を使う仕事を待つので、
            // 「使っている間に書き換える」は待つ形で守られる
            f->descriptorIndexing = k.descriptorIndexing;
            f->descriptorBindingSampledImageUpdateAfterBind = k.descriptorBindingSampledImageUpdateAfterBind;
            f->descriptorBindingUpdateUnusedWhilePending = k.descriptorBindingUpdateUnusedWhilePending;
            f->descriptorBindingPartiallyBound = k.descriptorBindingPartiallyBound;
            f->runtimeDescriptorArray = k.runtimeDescriptorArray;
            // アドレスは受け手の本物を返す。shader がアドレスでたどって書いた中身は、ドライバの写しの見張りに入らない
            if (g_hw.caps & VR_CAP_BDA) f->bufferDeviceAddress = k.bufferDeviceAddress;
        }
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
        VkPhysicalDeviceVulkan13Features k = *(VkPhysicalDeviceVulkan13Features *) s, *f = (void *) s;
        memset(BODY(f), 0, sizeof *f - sizeof(VkBaseOutStructure));
        if (!vk13()) return; // 1.2 と名乗っているときは全部「無い」
        // 中継できる機能だけ残す（inline uniform block は descriptor の書き方が違うので、まだ扱わない）
        f->robustImageAccess = k.robustImageAccess;
        f->pipelineCreationCacheControl = k.pipelineCreationCacheControl;
        f->privateData = VK_TRUE; // ドライバの中で扱う
        f->shaderDemoteToHelperInvocation = k.shaderDemoteToHelperInvocation;
        f->shaderTerminateInvocation = k.shaderTerminateInvocation;
        f->subgroupSizeControl = k.subgroupSizeControl;
        f->computeFullSubgroups = k.computeFullSubgroups;
        f->synchronization2 = (g_hw.caps & VR_CAP_SYNC2) ? k.synchronization2 : VK_FALSE;
        f->textureCompressionASTC_HDR = k.textureCompressionASTC_HDR;
        f->shaderZeroInitializeWorkgroupMemory = k.shaderZeroInitializeWorkgroupMemory;
        f->dynamicRendering = (g_hw.caps & VR_CAP_DYNAMIC_RENDERING) ? k.dynamicRendering : VK_FALSE;
        f->shaderIntegerDotProduct = k.shaderIntegerDotProduct;
        f->maintenance4 = k.maintenance4;
        // DXVK 3 は印だけを見る（inline uniform block の descriptor は使わない）。書き込みはまだ飛ばす
        if (dxvk3()) f->inlineUniformBlock = k.inlineUniformBlock;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES) {
        if (!dxvk3()) ((VkPhysicalDeviceMaintenance5Features *) s)->maintenance5 = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES) {
        if (!dxvk3()) ((VkPhysicalDeviceMaintenance6Features *) s)->maintenance6 = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
        VkPhysicalDeviceDynamicRenderingFeatures *f = (void *) s;
        if (!(g_hw.caps & VR_CAP_DYNAMIC_RENDERING)) f->dynamicRendering = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
        VkPhysicalDeviceSynchronization2Features *f = (void *) s;
        if (!(g_hw.caps & VR_CAP_SYNC2)) f->synchronization2 = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT *f = (void *) s;
        if (!(g_hw.caps & VR_CAP_EXT_DYNAMIC_STATE)) f->extendedDynamicState = VK_FALSE;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT *f = (void *) s;
        if (!(g_hw.caps & VR_CAP_EXT_DYNAMIC_STATE)) f->extendedDynamicState2 = VK_FALSE;
        f->extendedDynamicState2LogicOp = f->extendedDynamicState2PatchControlPoints = VK_FALSE; // まだ中継していない
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES) {
        VkPhysicalDeviceHostQueryResetFeatures *f = (void *) s;
        if (!(g_hw.caps & VR_CAP_QUERY)) f->hostQueryReset = VK_FALSE;
    }
}

// 受け手の GPU とは別の装置として名乗る。同じ UUID だと、llama.cpp などは同じ GPU が 2 つ見えたと思って片方を捨てる。
// Windows のアダプター番号（LUID）は、この PC の物ではないので「無し」にする
static void fix_ids(VkBaseOutStructure *s) {
    uint8_t *uuid = NULL;
    VkBool32 *luid_valid = NULL;
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
        VkPhysicalDeviceIDProperties *p = (void *) s;
        uuid = p->deviceUUID;
        luid_valid = &p->deviceLUIDValid;
    } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
        VkPhysicalDeviceVulkan11Properties *p = (void *) s;
        uuid = p->deviceUUID;
        luid_valid = &p->deviceLUIDValid;
    }
    if (!uuid) return;
    static const uint8_t mark[4] = { 'i', 'P', 'h', 'n' };
    for (int i = 0; i < 4; i++) uuid[12 + i] ^= mark[i];
    *luid_valid = VK_FALSE;
}

// pNext の鎖のうち、知っている構造体だけを受け手に尋ねて埋める
static void query_chain(uint32_t op, void *pnext, const stinfo *t, size_t nt, void *base, size_t base_size) {
    vr_buf b = { 0 }, out = { 0 };
    VkBaseOutStructure *items[64];
    uint32_t n = 0;
    for (VkBaseOutStructure *s = pnext; s && n < 64; s = s->pNext)
        if (st_size(t, nt, s->sType)) items[n++] = s;
    vr_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) { vr_u32(&b, items[i]->sType); vr_u32(&b, st_size(t, nt, items[i]->sType)); }
    if (rpc_call(op, &b, &out) == VK_SUCCESS) {
        vr_rd rd = { out.p, out.n, 0, 0 };
        vr_take(&rd, base_size); // 土台の部分は手元のものを使う
        for (uint32_t i = 0; i < n; i++) {
            uint32_t sz = st_size(t, nt, items[i]->sType);
            vr_rcopy(&rd, BODY(items[i]), sz - sizeof(VkBaseOutStructure));
            if (op == VR_GET_FEATURES2) mask_ext_features(items[i]);
            else fix_ids(items[i]);
        }
    }
    (void) base;
    free(b.p);
    free(out.p);
}

static void VKAPI_CALL get_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    get_props(pd, &p->properties);
    query_chain(VR_GET_PROPERTIES2, p->pNext, PROP_ST, sizeof PROP_ST / sizeof *PROP_ST, &p->properties, sizeof p->properties);
    for (VkBaseOutStructure *x = p->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR)
            ((VkPhysicalDevicePushDescriptorPropertiesKHR *) x)->maxPushDescriptors = 32;
}

static void VKAPI_CALL get_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    get_features(pd, &f->features);
    query_chain(VR_GET_FEATURES2, f->pNext, FEAT_ST, sizeof FEAT_ST / sizeof *FEAT_ST, &f->features, sizeof f->features);
    // ドライバの中で済ませる機能
    for (VkBaseOutStructure *x = f->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT)
            ((VkPhysicalDeviceDepthClipEnableFeaturesEXT *) x)->depthClipEnable = dxvk3() ? VK_TRUE : VK_FALSE;
        else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            // 範囲の外の読みは 0（GPU の robustBufferAccess に頼る）。画像の範囲の外は名乗らない
            VkPhysicalDeviceRobustness2FeaturesEXT *r2 = (void *) x;
            VkBool32 on = dxvk3() && (g_hw.caps & VR_CAP_NULL_DESC) ? VK_TRUE : VK_FALSE;
            r2->robustBufferAccess2 = r2->nullDescriptor = on;
            r2->robustImageAccess2 = VK_FALSE;
        }
}

static void VKAPI_CALL get_queue_families(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *q) {
    say("キューの一覧");
    (void) pd;
    if (!q) { *n = g_hw.nq; return; }
    uint32_t k = *n < g_hw.nq ? *n : g_hw.nq;
    memcpy(q, g_hw.q, k * sizeof *q);
    *n = k;
}

static void VKAPI_CALL get_queue_families2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *q) {
    if (!q) { get_queue_families(pd, n, NULL); return; }
    uint32_t k = *n < g_hw.nq ? *n : g_hw.nq;
    for (uint32_t i = 0; i < k; i++) q[i].queueFamilyProperties = g_hw.q[i];
    *n = k;
}

static void VKAPI_CALL get_mem(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) {
    (void) pd;
    *m = g_hw.mem;
}

static void VKAPI_CALL get_mem2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) {
    get_mem(pd, &m->memoryProperties);
}

// p3 があれば 1.3 の 64 ビットの形も埋める（受け手が後ろに付けていればそれを、無ければ 1.0 の形を広げる）
// ---- 問い合わせの答えを覚えておく ----
// 形式の性質などは、同じことを聞けば毎回同じ答え。USB を往復せずに、前の答えを返す
#define QC_SIZE 4096
static struct qcache { int used; uint32_t key[6]; VkResult r; uint32_t n; uint8_t data[64]; } g_qc[QC_SIZE];
static SRWLOCK g_qc_lock = SRWLOCK_INIT;

static int qc_get(const uint32_t key[6], VkResult *r, uint8_t *data, uint32_t *n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ key[i]) * 16777619u;
    int hit = 0;
    AcquireSRWLockShared(&g_qc_lock);
    for (uint32_t k = 0; k < 16; k++) {
        struct qcache *e = &g_qc[(h + k) & (QC_SIZE - 1)];
        if (!e->used) break;
        if (!memcmp(e->key, key, sizeof e->key)) { *r = e->r; *n = e->n; memcpy(data, e->data, e->n); hit = 1; break; }
    }
    ReleaseSRWLockShared(&g_qc_lock);
    return hit;
}

static void qc_put(const uint32_t key[6], VkResult r, const void *data, uint32_t n) {
    if (n > 64) return;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; i++) h = (h ^ key[i]) * 16777619u;
    AcquireSRWLockExclusive(&g_qc_lock);
    for (uint32_t k = 0; k < 16; k++) {
        struct qcache *e = &g_qc[(h + k) & (QC_SIZE - 1)];
        if (e->used && memcmp(e->key, key, sizeof e->key)) continue;
        e->used = 1;
        memcpy(e->key, key, sizeof e->key);
        e->r = r;
        e->n = n;
        memcpy(e->data, data, n);
        break;
    }
    ReleaseSRWLockExclusive(&g_qc_lock);
}

static void get_format_ex(VkFormat f, VkFormatProperties *p, VkFormatProperties3 *p3) {
    // ほどく BC は、置き換えた形式でできることのうち、BC の画像に許されること（読む・写す）だけを見せる
    VkFormat bcf = g_bc ? bc_sub(f) : VK_FORMAT_UNDEFINED;
    if (bcf) {
        get_format_ex(bcf, p, p3);
        const VkFormatFeatureFlags keep = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        p->optimalTilingFeatures &= keep;
        p->linearTilingFeatures = p->bufferFeatures = 0;
        if (p3) {
            p3->optimalTilingFeatures &= keep;
            p3->linearTilingFeatures = p3->bufferFeatures = 0;
        }
        return;
    }
    vr_buf b = { 0 }, out = { 0 };
    vr_u32(&b, (uint32_t) f);
    memset(p, 0, sizeof *p);
    // 前に聞いた形式なら、覚えている答えを使う
    uint32_t key[6] = { 1, (uint32_t) f, 0, 0, 0, 0 };
    uint8_t cached[64];
    uint32_t cn = 0;
    VkResult cr;
    int ok;
    if (qc_get(key, &cr, cached, &cn)) {
        vr_reserve(&out, cn);
        memcpy(out.p, cached, cn);
        out.n = cn;
        ok = cr == VK_SUCCESS && out.n >= sizeof *p;
    } else {
        cr = rpc_call(VR_GET_FORMAT_PROPS, &b, &out);
        if (!g_dead) qc_put(key, cr, out.p, (uint32_t) out.n);
        ok = cr == VK_SUCCESS && out.n >= sizeof *p;
    }
    if (ok) memcpy(p, out.p, sizeof *p);
    // 線形（linear）の画像を CPU から直接書かれると、写しの同期で追えないので見せない（アプリは受け渡し用のバッファから写す）
    p->linearTilingFeatures = 0;
    if (p3) {
        p3->linearTilingFeatures = 0;
        p3->optimalTilingFeatures = p->optimalTilingFeatures;
        p3->bufferFeatures = p->bufferFeatures;
        if (ok && out.n >= sizeof *p + 24) {
            memcpy(&p3->optimalTilingFeatures, out.p + sizeof *p + 8, 8);
            memcpy(&p3->bufferFeatures, out.p + sizeof *p + 16, 8);
        }
    }
    free(b.p);
    free(out.p);
}

static void VKAPI_CALL get_format(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p) {
    (void) pd;
    get_format_ex(f, p, NULL);
}

static void VKAPI_CALL get_format2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    (void) pd;
    VkFormatProperties3 *p3 = NULL;
    for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) p3 = (VkFormatProperties3 *) s;
    get_format_ex(f, &p->formatProperties, p3);
}

// 道具（レイヤーなど）の一覧。ドライバとしては何も無い
static VkResult VKAPI_CALL get_tool_props(VkPhysicalDevice pd, uint32_t *n, VkPhysicalDeviceToolProperties *p) {
    (void) pd; (void) p;
    *n = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL enum_device_ext(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    say("装置の拡張の一覧");
    (void) pd;
    if (layer) { *n = 0; return VK_SUCCESS; }
    if (!p) { *n = g_hw.ne; return VK_SUCCESS; }
    uint32_t k = *n < g_hw.ne ? *n : g_hw.ne;
    memcpy(p, g_hw.ext, k * sizeof *p);
    *n = k;
    return k < g_hw.ne ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL enum_device_layers(VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) {
    (void) pd; (void) p;
    *n = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL get_image_format(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkImageTiling ti, VkImageUsageFlags u, VkImageCreateFlags fl, VkImageFormatProperties *p) {
    (void) pd;
    if (g_bc && bc_sub(f)) {
        memset(p, 0, sizeof *p);
        const VkImageUsageFlags ok = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (ti != VK_IMAGE_TILING_OPTIMAL || (u & ~ok)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        f = bc_sub(f);
        fl &= ~(VkImageCreateFlags) VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    }
    vr_buf b = { 0 }, out = { 0 };
    vr_u32(&b, f);
    vr_u32(&b, t);
    vr_u32(&b, ti);
    vr_u32(&b, u);
    vr_u32(&b, fl);
    memset(p, 0, sizeof *p);
    uint32_t key[6] = { 2, (uint32_t) f, (uint32_t) t, (uint32_t) ti, (uint32_t) u, (uint32_t) fl };
    uint8_t cached[64];
    uint32_t cn = 0;
    VkResult r;
    if (qc_get(key, &r, cached, &cn)) {
        if (cn >= sizeof *p) memcpy(p, cached, sizeof *p);
        free(b.p);
        return r;
    }
    r = rpc_call(VR_GET_IMAGE_FORMAT_PROPS, &b, &out);
    if (!g_dead) qc_put(key, r, out.p, (uint32_t) out.n);
    if (out.n >= sizeof *p) memcpy(p, out.p, sizeof *p);
    free(b.p);
    free(out.p);
    return r;
}

static VkResult VKAPI_CALL get_image_format2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p) {
    return get_image_format(pd, i->format, i->type, i->tiling, i->usage, i->flags, &p->imageFormatProperties);
}

static void VKAPI_CALL get_sparse_format(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageTiling ti, uint32_t *n, VkSparseImageFormatProperties *p) {
    (void) pd; (void) f; (void) t; (void) s; (void) u; (void) ti; (void) p;
    *n = 0;
}

static void VKAPI_CALL get_sparse_format2(VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *i, uint32_t *n, VkSparseImageFormatProperties2 *p) {
    (void) pd; (void) i; (void) p;
    *n = 0;
}

static void VKAPI_CALL get_ext_buffer(VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *i, VkExternalBufferProperties *p) {
    (void) pd; (void) i;
    memset(&p->externalMemoryProperties, 0, sizeof p->externalMemoryProperties);
}

static void VKAPI_CALL get_ext_fence(VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *i, VkExternalFenceProperties *p) {
    (void) pd; (void) i;
    p->exportFromImportedHandleTypes = p->compatibleHandleTypes = 0;
    p->externalFenceFeatures = 0;
}

static void VKAPI_CALL get_ext_sem(VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *i, VkExternalSemaphoreProperties *p) {
    (void) pd; (void) i;
    p->exportFromImportedHandleTypes = p->compatibleHandleTypes = 0;
    p->externalSemaphoreFeatures = 0;
}

// ---- 装置 ----
static VkResult VKAPI_CALL create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    say("装置を作る（create device）");
    (void) pd; (void) a;
    struct dev *d = calloc(1, sizeof *d);
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(d);
    InitializeSRWLock(&d->lock);
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    vr_u32(&b, ci->queueCreateInfoCount);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        vr_u32(&b, ci->pQueueCreateInfos[i].queueFamilyIndex);
        vr_u32(&b, ci->pQueueCreateInfos[i].queueCount);
    }
    uint32_t ne = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) if (allowed_ext(ci->ppEnabledExtensionNames[i])) ne++;
    vr_u32(&b, ne);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (allowed_ext(ci->ppEnabledExtensionNames[i])) vr_str(&b, ci->ppEnabledExtensionNames[i]);
    // 機能：pEnabledFeatures か、鎖の中の VkPhysicalDeviceFeatures2
    const VkPhysicalDeviceFeatures *base = ci->pEnabledFeatures;
    const VkBaseInStructure *chain[32];
    uint32_t nc = 0;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) base = &((const VkPhysicalDeviceFeatures2 *) s)->features;
        else if (st_size(FEAT_ST, sizeof FEAT_ST / sizeof *FEAT_ST, s->sType) && nc < 32) chain[nc++] = s;
    }
    vr_u32(&b, base ? 1 : 0);
    if (base) {
        // 名乗りだけの機能（ドライバがほどく BC など）は、受け手の装置では有効にしない
        VkPhysicalDeviceFeatures en = *base;
        VkBool32 *e = (VkBool32 *) &en;
        const VkBool32 *rf = (const VkBool32 *) &g_hw.real;
        for (size_t k = 0; k < sizeof en / sizeof(VkBool32); k++) e[k] = e[k] && rf[k];
        vr_put(&b, &en, sizeof en);
    }
    vr_u32(&b, nc);
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t sz = st_size(FEAT_ST, sizeof FEAT_ST / sizeof *FEAT_ST, chain[i]->sType);
        vr_u32(&b, chain[i]->sType);
        if (new_feature_st(chain[i]->sType) && sz <= 128) {
            // 1.3 に向けて足した機能は、中継できる物だけを受け手で有効にする（中身は VkBool32 だけが並ぶ）。
            // 全部 1 にした写しを mask_ext_features に通すと「中継できる機能」の印になるので、それと重ねる
            VkBool32 want[32], ok[32];
            const uint32_t h = sizeof(VkBaseOutStructure) / 4, nw = sz / 4;
            memcpy(want, chain[i], sz);
            memcpy(ok, chain[i], sz);
            for (uint32_t k = h; k < nw; k++) ok[k] = VK_TRUE;
            mask_ext_features((VkBaseOutStructure *) ok);
            for (uint32_t k = h; k < nw; k++) want[k] &= ok[k];
            // private data はドライバの中で扱うので、受け手では有効にしない
            if (chain[i]->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) ((VkPhysicalDeviceVulkan13Features *) want)->privateData = VK_FALSE;
            vr_bytes(&b, want + h, sz - sizeof(VkBaseOutStructure));
        } else {
            vr_bytes(&b, BODY(chain[i]), sz - sizeof(VkBaseOutStructure));
        }
    }
    VkResult r = rpc_call(VR_CREATE_DEVICE, &b, NULL);
    free(b.p);
    if (r != VK_SUCCESS) { say("装置を作れない (%d)", r); free(d); return r; }
    say("装置を作った");
    *out = (VkDevice) d;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_device(VkDevice dv, const VkAllocationCallbacks *a) {
    (void) a;
    if (!dv) return;
    struct dev *d = (struct dev *) dv;
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    rpc_send(VR_DESTROY_DEVICE, &b);
    rpc_flush();
    free(b.p);
    for (int i = 0; i < 16; i++) free(d->queues[i]);
    free(d->pend);
    free(d);
}

static void VKAPI_CALL get_queue(VkDevice dv, uint32_t fam, uint32_t idx, VkQueue *out) {
    struct dev *d = (struct dev *) dv;
    AcquireSRWLockExclusive(&d->lock);
    struct queue *q = NULL;
    int slot = -1;
    for (int i = 0; i < 16; i++) {
        if (d->queues[i] && d->queues[i]->fam == fam && d->queues[i]->idx == idx) { q = d->queues[i]; break; }
        if (!d->queues[i] && slot < 0) slot = i;
    }
    if (!q && slot >= 0) {
        q = calloc(1, sizeof *q);
        set_loader_magic_value(q);
        q->d = d;
        q->fam = fam;
        q->idx = idx;
        d->queues[slot] = q;
        vr_buf b = { 0 };
        vr_u64(&b, ID(d));
        vr_u32(&b, fam);
        vr_u32(&b, idx);
        vr_u64(&b, ID(q));
        rpc_send(VR_GET_QUEUE, &b);
        free(b.p);
    }
    ReleaseSRWLockExclusive(&d->lock);
    *out = (VkQueue) q;
}

static void VKAPI_CALL get_queue2(VkDevice dv, const VkDeviceQueueInfo2 *i, VkQueue *out) {
    get_queue(dv, i->queueFamilyIndex, i->queueIndex, out);
}

// ---- メモリ ----
static VkResult VKAPI_CALL alloc_memory(VkDevice dv, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void) a;
    struct dev *d = (struct dev *) dv;
    struct mem *m = calloc(1, sizeof *m);
    if (!m) return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->size = ai->allocationSize;
    m->type = ai->memoryTypeIndex;
    m->host = ai->memoryTypeIndex < g_hw.mem.memoryTypeCount &&
              (g_hw.mem.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (m->host) {
        m->npages = (size_t) ((m->size + PAGE - 1) / PAGE);
        m->shadow = VirtualAlloc(NULL, m->npages * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        m->addrs = malloc(m->npages * sizeof *m->addrs);
        m->dirty = calloc(m->npages, 1);
        if (!m->shadow || !m->addrs || !m->dirty) {
            if (m->shadow) VirtualFree(m->shadow, 0, MEM_RELEASE);
            free(m->addrs);
            free(m->dirty);
            free(m);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    vr_u64(&b, ID(m));
    vr_u64(&b, m->size);
    vr_u32(&b, m->type);
    if (g_hw.caps & VR_CAP_BDA) {
        // アドレスを使うメモリの印（VkMemoryAllocateFlagsInfo）を末尾に付ける
        uint32_t fl = 0;
        for (const VkBaseInStructure *x = ai->pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) fl = ((const VkMemoryAllocateFlagsInfo *) x)->flags;
        vr_u32(&b, fl);
    }
    VkResult r = rpc_call(VR_ALLOC_MEMORY, &b, NULL);
    free(b.p);
    if (r != VK_SUCCESS) {
        if (m->shadow) VirtualFree(m->shadow, 0, MEM_RELEASE);
        free(m->addrs);
        free(m->dirty);
        free(m);
        return r;
    }
    say("メモリ %.1f MiB（種類 %u%s）", m->size / 1048576.0, m->type, m->host ? "・CPU から見える" : "");
    if (m->host) {
        AcquireSRWLockExclusive(&d->lock);
        m->next = d->mems;
        if (d->mems) d->mems->prev = m;
        d->mems = m;
        ReleaseSRWLockExclusive(&d->lock);
    }
    *out = (VkDeviceMemory) m;
    return VK_SUCCESS;
}

static void VKAPI_CALL free_memory(VkDevice dv, VkDeviceMemory mm, const VkAllocationCallbacks *a) {
    (void) a;
    if (!mm) return;
    struct dev *d = (struct dev *) dv;
    struct mem *m = (struct mem *) mm;
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    vr_u64(&b, ID(m));
    rpc_send(VR_FREE_MEMORY, &b);
    free(b.p);
    AcquireSRWLockExclusive(&d->lock);
    if (m->host) {
        if (m->prev) m->prev->next = m->next; else if (d->mems == m) d->mems = m->next;
        if (m->next) m->next->prev = m->prev;
    }
    size_t k = 0;
    for (size_t i = 0; i < d->npend; i++) if (d->pend[i].m != m) d->pend[k++] = d->pend[i];
    d->npend = k;
    ReleaseSRWLockExclusive(&d->lock);
    if (m->shadow) VirtualFree(m->shadow, 0, MEM_RELEASE);
    free(m->addrs);
    free(m->dirty);
    free(m->sent);
    free(m->known);
    free(m);
}

static VkResult VKAPI_CALL map_memory(VkDevice dv, VkDeviceMemory mm, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void **pp) {
    (void) dv; (void) size; (void) fl;
    struct mem *m = (struct mem *) mm;
    if (!m->host) return VK_ERROR_MEMORY_MAP_FAILED;
    *pp = m->shadow + off;
    return VK_SUCCESS;
}

static void VKAPI_CALL unmap_memory(VkDevice dv, VkDeviceMemory mm) { (void) dv; (void) mm; }

// 送るのは submit のとき、引き取るのは終わりを待ったときにまとめてするので、ここでは何もしない
static VkResult VKAPI_CALL flush_ranges(VkDevice dv, uint32_t n, const VkMappedMemoryRange *r) { (void) dv; (void) n; (void) r; return VK_SUCCESS; }

static void VKAPI_CALL get_commitment(VkDevice dv, VkDeviceMemory mm, VkDeviceSize *c) { (void) dv; *c = ((struct mem *) mm)->size; }

// ---- バッファ ----
// buffer の必要なメモリの量は、flags と使い道が同じなら、大きさを並びの単位に切り上げただけになる GPU が多い。
// 最初の 1 回は受け手に聞いて、そうなっていると確かめられたら、次からは手元で計算して返事を待たない（先例 Venus と同じ考え方）
static struct bufreq { int used, linear; uint32_t flags, usage; VkMemoryRequirements req; } g_brq[256];
static SRWLOCK g_brq_lock = SRWLOCK_INIT;

static int brq_find(uint32_t flags, uint32_t usage, VkDeviceSize size, VkMemoryRequirements *req) {
    int hit = 0;
    AcquireSRWLockShared(&g_brq_lock);
    for (int i = 0; i < 256 && g_brq[i].used; i++)
        if (g_brq[i].flags == flags && g_brq[i].usage == usage && g_brq[i].linear) {
            *req = g_brq[i].req;
            VkDeviceSize al = req->alignment ? req->alignment : 1;
            req->size = (size + al - 1) / al * al;
            hit = 1;
            break;
        }
    ReleaseSRWLockShared(&g_brq_lock);
    return hit;
}

static void brq_learn(uint32_t flags, uint32_t usage, VkDeviceSize size, const VkMemoryRequirements *req) {
    VkDeviceSize al = req->alignment ? req->alignment : 1;
    int linear = req->size == (size + al - 1) / al * al;
    AcquireSRWLockExclusive(&g_brq_lock);
    int i = 0;
    while (i < 256 && g_brq[i].used && !(g_brq[i].flags == flags && g_brq[i].usage == usage)) i++;
    if (i < 256 && !g_brq[i].used) {
        g_brq[i].used = 1;
        g_brq[i].flags = flags;
        g_brq[i].usage = usage;
        g_brq[i].req = *req;
        g_brq[i].linear = linear;
    }
    ReleaseSRWLockExclusive(&g_brq_lock);
}

static uint32_t gs_buffer_usage(uint32_t usage);
static VkResult VKAPI_CALL create_buffer(VkDevice dv, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void) a;
    struct buf *bf = calloc(1, sizeof *bf);
    if (!bf) return VK_ERROR_OUT_OF_HOST_MEMORY;
    bf->size = ci->size;
    vr_buf b = { 0 }, o = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bf));
    vr_u32(&b, ci->flags);
    vr_u64(&b, ci->size);
    // maintenance5 では、使い道を pNext の 64 ビットの形で渡せる。今の受け手は 32 ビットまでしか使わない
    uint32_t usage = ci->usage;
    for (const VkBaseInStructure *x = ci->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO) usage = (uint32_t) ((const VkBufferUsageFlags2CreateInfo *) x)->usage;
    usage = gs_buffer_usage(usage); // GS を真似る VS が storage buffer として読む
    vr_u32(&b, usage);
    if ((g_hw.caps & VR_CAP_ASYNC_CREATE) && brq_find(ci->flags, usage, ci->size, &bf->req)) {
        rpc_send(VR_CREATE_BUFFER_NR, &b);
        free(b.p);
        *out = (VkBuffer) bf;
        return VK_SUCCESS;
    }
    VkResult r = rpc_call(VR_CREATE_BUFFER, &b, &o);
    if (r == VK_SUCCESS && o.n >= sizeof bf->req) {
        memcpy(&bf->req, o.p, sizeof bf->req);
        brq_learn(ci->flags, usage, ci->size, &bf->req);
    }
    free(b.p);
    free(o.p);
    if (r != VK_SUCCESS) { free(bf); return r; }
    *out = (VkBuffer) bf;
    return VK_SUCCESS;
}

static void forget_buffer_in_sets(struct buf *bf);

static void VKAPI_CALL destroy_buffer(VkDevice dv, VkBuffer bb, const VkAllocationCallbacks *a) {
    (void) a;
    if (!bb) return;
    forget_buffer_in_sets((struct buf *) bb);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bb));
    rpc_send(VR_DESTROY_BUFFER, &b);
    free(b.p);
    free((struct buf *) bb);
}

static void VKAPI_CALL buffer_reqs(VkDevice dv, VkBuffer bb, VkMemoryRequirements *r) { (void) dv; *r = ((struct buf *) bb)->req; }

static void VKAPI_CALL buffer_reqs2(VkDevice dv, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    buffer_reqs(dv, i->buffer, &r->memoryRequirements);
    for (VkBaseOutStructure *s = r->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dr = (void *) s;
            dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_FALSE;
        }
}

static VkResult VKAPI_CALL bind_buffer(VkDevice dv, VkBuffer bb, VkDeviceMemory mm, VkDeviceSize off) {
    struct buf *bf = (struct buf *) bb;
    bf->m = (struct mem *) mm;
    bf->memoff = off;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bb));
    vr_u64(&b, ID(mm));
    vr_u64(&b, off);
    rpc_send(VR_BIND_BUFFER_MEMORY, &b);
    free(b.p);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bind_buffer2(VkDevice dv, uint32_t n, const VkBindBufferMemoryInfo *i) {
    for (uint32_t k = 0; k < n; k++) bind_buffer(dv, i[k].buffer, i[k].memory, i[k].memoryOffset);
    return VK_SUCCESS;
}

// ---- 画像 ----
struct img { VkMemoryRequirements req; struct mem *m; uint64_t memoff; VkFormat bc; }; // bc：ほどいて持つ BC の形式


// 受け手の装置では swapchain を使わないので、PRESENT_SRC の形は GENERAL に読み替える
static VkImageLayout fix_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || l == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ? VK_IMAGE_LAYOUT_GENERAL : l;
}

static VkResult VKAPI_CALL create_image(VkDevice dv, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void) a;
    struct img *im = calloc(1, sizeof *im);
    if (!im) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkImageCreateInfo bci;
    if (g_bc && bc_sub(ci->format)) {
        bci = *ci;
        bci.format = bc_sub(ci->format);
        bci.flags &= ~(VkImageCreateFlags) VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
        im->bc = ci->format;
        ci = &bci;
    }
    // DXVK 1.10 は深度とステンシルの画像を SAMPLED なしで作り、ステージングへ写すときに view の側だけ SAMPLED にして読む。
    // view の印（pNext）は受け手に運ばないので、受け手では読めない view になり 0 が返る。読める形式なら画像に SAMPLED を足す
    VkImageCreateInfo dci;
    if ((ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) && !(ci->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
        (ci->format == VK_FORMAT_D16_UNORM_S8_UINT || ci->format == VK_FORMAT_D24_UNORM_S8_UINT || ci->format == VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        VkFormatProperties fp;
        get_format_ex(ci->format, &fp, NULL);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
            dci = *ci;
            dci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            ci = &dci;
        }
    }
    vr_buf b = { 0 }, o = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(im));
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->imageType);
    vr_u32(&b, ci->format);
    vr_u32(&b, ci->extent.width);
    vr_u32(&b, ci->extent.height);
    vr_u32(&b, ci->extent.depth);
    vr_u32(&b, ci->mipLevels);
    vr_u32(&b, ci->arrayLayers);
    vr_u32(&b, ci->samples);
    vr_u32(&b, ci->tiling);
    vr_u32(&b, ci->usage);
    vr_u32(&b, ci->initialLayout);
    // まったく同じ条件の画像を前に作っていれば、必要なメモリの量は同じ。覚えている答えを使い、返事を待たずに作る
    uint32_t key[6];
    {
        uint32_t h1 = 2166136261u, h2 = 0x9747b28cu;
        const uint8_t *k = b.p + 16; // device と id の後ろ。作る条件だけ
        for (size_t i = 0; i + 16 < b.n; i++) { h1 = (h1 ^ k[i]) * 16777619u; h2 = (h2 ^ k[i]) * 0x01000193u + 0x5bd1e995u; }
        key[0] = 3; key[1] = h1; key[2] = h2; key[3] = ci->format; key[4] = ci->extent.width; key[5] = ci->extent.height;
    }
    uint8_t cached[64];
    uint32_t cn = 0;
    VkResult cr;
    if ((g_hw.caps & VR_CAP_ASYNC_CREATE) && qc_get(key, &cr, cached, &cn) && cr == VK_SUCCESS && cn >= sizeof im->req) {
        memcpy(&im->req, cached, sizeof im->req);
        rpc_send(VR_CREATE_IMAGE_NR, &b);
        free(b.p);
        free(o.p);
        *out = (VkImage) im;
        return VK_SUCCESS;
    }
    VkResult r = rpc_call(VR_CREATE_IMAGE, &b, &o);
    if (r == VK_SUCCESS && o.n >= sizeof im->req) {
        memcpy(&im->req, o.p, sizeof im->req);
        qc_put(key, r, o.p, (uint32_t) sizeof im->req);
    }
    free(b.p);
    free(o.p);
    if (r != VK_SUCCESS) { free(im); return r; }
    *out = (VkImage) im;
    return VK_SUCCESS;
}

static void send_destroy(uint32_t op, VkDevice dv, uint64_t id) {
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    rpc_send(op, &b);
    free(b.p);
}

static void VKAPI_CALL destroy_image(VkDevice dv, VkImage h, const VkAllocationCallbacks *a) {
    (void) a;
    if (!h) return;
    send_destroy(VR_DESTROY_IMAGE, dv, ID(h));
    free((struct img *) h);
}

static void VKAPI_CALL image_reqs(VkDevice dv, VkImage h, VkMemoryRequirements *r) { (void) dv; *r = ((struct img *) h)->req; }

static void VKAPI_CALL image_reqs2(VkDevice dv, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    image_reqs(dv, i->image, &r->memoryRequirements);
    for (VkBaseOutStructure *s = r->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dr = (void *) s;
            dr->prefersDedicatedAllocation = dr->requiresDedicatedAllocation = VK_FALSE;
        }
}

static VkResult VKAPI_CALL bind_image(VkDevice dv, VkImage h, VkDeviceMemory mm, VkDeviceSize off) {
    struct img *im = (struct img *) h;
    im->m = (struct mem *) mm;
    im->memoff = off;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(h));
    vr_u64(&b, ID(mm));
    vr_u64(&b, off);
    rpc_send(VR_BIND_IMAGE_MEMORY, &b);
    free(b.p);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL bind_image2(VkDevice dv, uint32_t n, const VkBindImageMemoryInfo *i) {
    for (uint32_t k = 0; k < n; k++) bind_image(dv, i[k].image, i[k].memory, i[k].memoryOffset);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_view(VkDevice dv, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags);
    vr_u64(&b, ID(ci->image));
    vr_u32(&b, ci->viewType);
    // ほどいて持つ画像の見え方は、置き換えた形式で
    vr_u32(&b, g_bc && bc_sub(ci->format) ? bc_sub(ci->format) : ci->format);
    vr_put(&b, &ci->components, sizeof ci->components);
    vr_put(&b, &ci->subresourceRange, sizeof ci->subresourceRange);
    rpc_send(VR_CREATE_IMAGE_VIEW, &b);
    free(b.p);
    *out = (VkImageView) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_view(VkDevice dv, VkImageView h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_IMAGE_VIEW, dv, ID(h)); }

// ---- buffer の見え方（texel buffer。DirectX の型付きの buffer はこれになる） ----
// どの buffer のどこを見ているかを覚えておく（descriptor に入ったとき、GPU が書く範囲として使う）
struct bview { struct buf *b; uint64_t off, range; };
static int need_cap(uint32_t cap, const char *what);

static VkResult VKAPI_CALL create_bview(VkDevice dv, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a, VkBufferView *out) {
    (void) a;
    if (!need_cap(VR_CAP_D3D, "vkCreateBufferView")) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct bview *v = calloc(1, sizeof *v);
    if (!v) return VK_ERROR_OUT_OF_HOST_MEMORY;
    v->b = (struct buf *) ci->buffer;
    v->off = ci->offset;
    v->range = ci->range == VK_WHOLE_SIZE && v->b ? v->b->size - ci->offset : ci->range;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(v));
    vr_u64(&b, ID(ci->buffer));
    vr_u32(&b, ci->format);
    vr_u64(&b, ci->offset);
    vr_u64(&b, ci->range);
    rpc_send(VR_CREATE_BUFFER_VIEW, &b);
    free(b.p);
    *out = (VkBufferView) v;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_bview(VkDevice dv, VkBufferView h, const VkAllocationCallbacks *a) {
    (void) a;
    if (!h) return;
    send_destroy(VR_DESTROY_BUFFER_VIEW, dv, ID(h));
    free((struct bview *) h);
}

// 画像の中で、ある mip・layer の画素がどこから並ぶか（CPU から直接読み書きする linear の画像で使う）
static void VKAPI_CALL get_subres_layout(VkDevice dv, VkImage im, const VkImageSubresource *sr, VkSubresourceLayout *l) {
    memset(l, 0, sizeof *l);
    if (!need_cap(VR_CAP_D3D, "vkGetImageSubresourceLayout")) return;
    vr_buf b = { 0 }, o = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(im));
    vr_u32(&b, sr->aspectMask);
    vr_u32(&b, sr->mipLevel);
    vr_u32(&b, sr->arrayLayer);
    if (rpc_call(VR_GET_IMAGE_SUBRESOURCE_LAYOUT, &b, &o) == VK_SUCCESS && o.n >= sizeof *l) memcpy(l, o.p, sizeof *l);
    free(b.p);
    free(o.p);
}

// ---- 描く手順（render pass）と描く先（framebuffer） ----
static void put_refs(vr_buf *b, const VkAttachmentReference *r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        VkAttachmentReference x = r[i];
        x.layout = fix_layout(x.layout);
        vr_put(b, &x, sizeof x);
    }
}
static VkResult VKAPI_CALL create_rpass(VkDevice dv, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        VkAttachmentDescription ad = ci->pAttachments[i];
        ad.initialLayout = fix_layout(ad.initialLayout);
        ad.finalLayout = fix_layout(ad.finalLayout);
        vr_put(&b, &ad, sizeof ad);
    }
    vr_u32(&b, ci->subpassCount);
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription *sp = &ci->pSubpasses[i];
        vr_u32(&b, sp->flags);
        vr_u32(&b, sp->pipelineBindPoint);
        vr_u32(&b, sp->inputAttachmentCount);
        put_refs(&b, sp->pInputAttachments, sp->inputAttachmentCount);
        vr_u32(&b, sp->colorAttachmentCount);
        put_refs(&b, sp->pColorAttachments, sp->colorAttachmentCount);
        vr_u32(&b, sp->pResolveAttachments ? 1 : 0);
        if (sp->pResolveAttachments) put_refs(&b, sp->pResolveAttachments, sp->colorAttachmentCount);
        vr_u32(&b, sp->pDepthStencilAttachment ? 1 : 0);
        if (sp->pDepthStencilAttachment) put_refs(&b, sp->pDepthStencilAttachment, 1);
        vr_u32(&b, sp->preserveAttachmentCount);
        vr_put(&b, sp->pPreserveAttachments, sp->preserveAttachmentCount * sizeof(uint32_t));
    }
    vr_u32(&b, ci->dependencyCount);
    vr_put(&b, ci->pDependencies, ci->dependencyCount * sizeof *ci->pDependencies);
    rpc_send(VR_CREATE_RENDER_PASS, &b);
    free(b.p);
    *out = (VkRenderPass) (uintptr_t) id;
    return VK_SUCCESS;
}

static void put_ref2(vr_buf *b, const VkAttachmentReference2 *r) {
    vr_u32(b, r->attachment);
    vr_u32(b, fix_layout(r->layout));
    vr_u32(b, r->aspectMask);
}

static int g_warned_rp2;
static VkResult VKAPI_CALL create_rpass2(VkDevice dv, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void) a;
    if (!(g_hw.caps & VR_CAP_RENDER_PASS2)) {
        if (!g_warned_rp2++) say("受け手が render pass 2 を知らない（古い受け手）");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        const VkAttachmentDescription2 *ad = &ci->pAttachments[i];
        vr_u32(&b, ad->flags);
        vr_u32(&b, ad->format);
        vr_u32(&b, ad->samples);
        vr_u32(&b, ad->loadOp);
        vr_u32(&b, ad->storeOp);
        vr_u32(&b, ad->stencilLoadOp);
        vr_u32(&b, ad->stencilStoreOp);
        vr_u32(&b, fix_layout(ad->initialLayout));
        vr_u32(&b, fix_layout(ad->finalLayout));
    }
    vr_u32(&b, ci->subpassCount);
    for (uint32_t i = 0; i < ci->subpassCount; i++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[i];
        vr_u32(&b, sp->flags);
        vr_u32(&b, sp->pipelineBindPoint);
        vr_u32(&b, sp->viewMask);
        vr_u32(&b, sp->inputAttachmentCount);
        for (uint32_t k = 0; k < sp->inputAttachmentCount; k++) put_ref2(&b, &sp->pInputAttachments[k]);
        vr_u32(&b, sp->colorAttachmentCount);
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) put_ref2(&b, &sp->pColorAttachments[k]);
        vr_u32(&b, sp->pResolveAttachments ? 1 : 0);
        for (uint32_t k = 0; sp->pResolveAttachments && k < sp->colorAttachmentCount; k++) put_ref2(&b, &sp->pResolveAttachments[k]);
        vr_u32(&b, sp->pDepthStencilAttachment ? 1 : 0);
        if (sp->pDepthStencilAttachment) put_ref2(&b, sp->pDepthStencilAttachment);
        vr_u32(&b, sp->preserveAttachmentCount);
        vr_put(&b, sp->pPreserveAttachments, sp->preserveAttachmentCount * sizeof(uint32_t));
        // depth・stencil の resolve
        const VkSubpassDescriptionDepthStencilResolve *dr = NULL;
        for (const VkBaseInStructure *s = sp->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE) dr = (const VkSubpassDescriptionDepthStencilResolve *) s;
        vr_u32(&b, dr ? 1 : 0);
        if (dr) {
            vr_u32(&b, dr->depthResolveMode);
            vr_u32(&b, dr->stencilResolveMode);
            vr_u32(&b, dr->pDepthStencilResolveAttachment ? 1 : 0);
            if (dr->pDepthStencilResolveAttachment) put_ref2(&b, dr->pDepthStencilResolveAttachment);
        }
    }
    vr_u32(&b, ci->dependencyCount);
    for (uint32_t i = 0; i < ci->dependencyCount; i++) {
        const VkSubpassDependency2 *dp = &ci->pDependencies[i];
        vr_u32(&b, dp->srcSubpass);
        vr_u32(&b, dp->dstSubpass);
        vr_u32(&b, dp->srcStageMask);
        vr_u32(&b, dp->dstStageMask);
        vr_u32(&b, dp->srcAccessMask);
        vr_u32(&b, dp->dstAccessMask);
        vr_u32(&b, dp->dependencyFlags);
        vr_u32(&b, (uint32_t) dp->viewOffset);
        // sync2 の memory barrier（付いていれば stage と access はこちら）
        const VkMemoryBarrier2 *mb = NULL;
        for (const VkBaseInStructure *s = dp->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2) mb = (const VkMemoryBarrier2 *) s;
        vr_u32(&b, mb && (g_hw.caps & VR_CAP_SYNC2) ? 1 : 0);
        if (mb && (g_hw.caps & VR_CAP_SYNC2)) {
            vr_u64(&b, mb->srcStageMask);
            vr_u64(&b, mb->srcAccessMask);
            vr_u64(&b, mb->dstStageMask);
            vr_u64(&b, mb->dstAccessMask);
        }
    }
    vr_u32(&b, ci->correlatedViewMaskCount);
    vr_put(&b, ci->pCorrelatedViewMasks, ci->correlatedViewMaskCount * sizeof(uint32_t));
    rpc_send(VR_CREATE_RENDER_PASS2, &b);
    free(b.p);
    *out = (VkRenderPass) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_rpass(VkDevice dv, VkRenderPass h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_RENDER_PASS, dv, ID(h)); }

static void VKAPI_CALL render_area_granularity(VkDevice dv, VkRenderPass rp, VkExtent2D *g) { (void) dv; (void) rp; g->width = g->height = 1; }

static VkResult VKAPI_CALL create_fb(VkDevice dv, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags);
    vr_u64(&b, ID(ci->renderPass));
    vr_u32(&b, ci->attachmentCount);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) vr_u64(&b, ID(ci->pAttachments[i]));
    vr_u32(&b, ci->width);
    vr_u32(&b, ci->height);
    vr_u32(&b, ci->layers);
    rpc_send(VR_CREATE_FRAMEBUFFER, &b);
    free(b.p);
    *out = (VkFramebuffer) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_fb(VkDevice dv, VkFramebuffer h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_FRAMEBUFFER, dv, ID(h)); }

// geometry shader を頂点シェーダーで真似る。受け手の GPU が持たないときだけ働く
#include "gs/gs_emu.c"
#include "gs/gs_icd.inc"

// ---- 描画用のパイプライン ----
static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out);
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a);

// maintenance5 では、module を作らずに stage の pNext に VkShaderModuleCreateInfo を入れてよい（新しい DXVK はこう書く）。
// 受け手には一時の module を作って送り、pipeline を作り終えたら消す
static VkShaderModule stage_module(VkDevice dv, const VkPipelineShaderStageCreateInfo *st, VkShaderModule *tmp) {
    *tmp = VK_NULL_HANDLE;
    if (st->module) return st->module;
    for (const VkBaseInStructure *x = st->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO && create_shader(dv, (const VkShaderModuleCreateInfo *) x, NULL, tmp) == VK_SUCCESS)
            return *tmp;
    return VK_NULL_HANDLE;
}

static void put_stage(vr_buf *b, VkDevice dv, const VkPipelineShaderStageCreateInfo *st, VkShaderModule *tmp) {
    vr_u32(b, st->stage);
    vr_u32(b, st->flags);
    vr_u64(b, ID(stage_module(dv, st, tmp)));
    vr_str(b, st->pName);
    const VkSpecializationInfo *si = st->pSpecializationInfo;
    vr_u32(b, si ? si->mapEntryCount : 0);
    for (uint32_t k = 0; si && k < si->mapEntryCount; k++) {
        vr_u32(b, si->pMapEntries[k].constantID);
        vr_u32(b, si->pMapEntries[k].offset);
        vr_u32(b, (uint32_t) si->pMapEntries[k].size);
    }
    vr_bytes(b, si ? si->pData : NULL, si ? (uint32_t) si->dataSize : 0);
}

static void put_f(vr_buf *b, float f) { vr_put(b, &f, 4); }

static VkResult VKAPI_CALL create_graphics_pipelines(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void) cache; (void) a;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        const VkGraphicsPipelineCreateInfo *c = &ci[i];
        int gs = 0;
        for (uint32_t k = 0; k < c->stageCount; k++) gs |= c->pStages[k].stage == VK_SHADER_STAGE_GEOMETRY_BIT;
        if (gs && !g_hw.real.geometryShader) {
            VkResult r2 = gs_emu_pipeline(dv, cache, c, a, &out[i]);
            if (r2 != VK_SUCCESS) worst = r2;
            continue;
        }
        uint64_t id = new_id();
        vr_buf b = { 0 };
        vr_u64(&b, ID(dv));
        vr_u64(&b, id);
        vr_u32(&b, c->flags);
        vr_u64(&b, ID(c->layout));
        vr_u64(&b, ID(c->renderPass));
        vr_u32(&b, c->subpass);
        vr_u32(&b, c->stageCount);
        VkShaderModule tmps[8] = { 0 }, extra = VK_NULL_HANDLE;
        for (uint32_t k = 0; k < c->stageCount; k++) {
            put_stage(&b, dv, &c->pStages[k], k < 8 ? &tmps[k] : &extra);
            if (extra) { destroy_shader(dv, extra, NULL); extra = VK_NULL_HANDLE; }
        }
        const VkPipelineVertexInputStateCreateInfo *vi = c->pVertexInputState;
        vr_u32(&b, vi ? 1 : 0);
        if (vi) {
            vr_u32(&b, vi->vertexBindingDescriptionCount);
            vr_put(&b, vi->pVertexBindingDescriptions, vi->vertexBindingDescriptionCount * sizeof *vi->pVertexBindingDescriptions);
            vr_u32(&b, vi->vertexAttributeDescriptionCount);
            vr_put(&b, vi->pVertexAttributeDescriptions, vi->vertexAttributeDescriptionCount * sizeof *vi->pVertexAttributeDescriptions);
        }
        const VkPipelineInputAssemblyStateCreateInfo *ia = c->pInputAssemblyState;
        vr_u32(&b, ia ? 1 : 0);
        if (ia) { vr_u32(&b, ia->topology); vr_u32(&b, ia->primitiveRestartEnable); }
        // 描画しないとき（rasterizerDiscard）は viewport などが無いことがある
        const VkPipelineViewportStateCreateInfo *vp = c->pViewportState;
        vr_u32(&b, vp ? 1 : 0);
        if (vp) {
            vr_u32(&b, vp->viewportCount);
            vr_u32(&b, vp->pViewports ? 1 : 0);
            if (vp->pViewports) vr_put(&b, vp->pViewports, vp->viewportCount * sizeof *vp->pViewports);
            vr_u32(&b, vp->scissorCount);
            vr_u32(&b, vp->pScissors ? 1 : 0);
            if (vp->pScissors) vr_put(&b, vp->pScissors, vp->scissorCount * sizeof *vp->pScissors);
        }
        const VkPipelineRasterizationStateCreateInfo *rs = c->pRasterizationState;
        vr_u32(&b, rs ? 1 : 0);
        if (rs) {
            // depth clip の指定があれば depth clamp に置き換える：切るなら丸めない、切らないなら丸める
            VkBool32 clamp = rs->depthClampEnable;
            for (const VkBaseInStructure *x = rs->pNext; x; x = x->pNext)
                if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT)
                    clamp = !((const VkPipelineRasterizationDepthClipStateCreateInfoEXT *) x)->depthClipEnable;
            vr_u32(&b, clamp);
            vr_u32(&b, rs->rasterizerDiscardEnable);
            vr_u32(&b, rs->polygonMode);
            vr_u32(&b, rs->cullMode);
            vr_u32(&b, rs->frontFace);
            vr_u32(&b, rs->depthBiasEnable);
            put_f(&b, rs->depthBiasConstantFactor);
            put_f(&b, rs->depthBiasClamp);
            put_f(&b, rs->depthBiasSlopeFactor);
            put_f(&b, rs->lineWidth);
        }
        const VkPipelineMultisampleStateCreateInfo *ms = c->pMultisampleState;
        vr_u32(&b, ms ? 1 : 0);
        if (ms) {
            vr_u32(&b, ms->rasterizationSamples);
            vr_u32(&b, ms->sampleShadingEnable);
            put_f(&b, ms->minSampleShading);
            vr_u32(&b, ms->alphaToCoverageEnable);
            vr_u32(&b, ms->alphaToOneEnable);
        }
        const VkPipelineDepthStencilStateCreateInfo *ds = c->pDepthStencilState;
        vr_u32(&b, ds ? 1 : 0);
        if (ds) {
            vr_u32(&b, ds->depthTestEnable);
            vr_u32(&b, ds->depthWriteEnable);
            vr_u32(&b, ds->depthCompareOp);
            vr_u32(&b, ds->depthBoundsTestEnable);
            vr_u32(&b, ds->stencilTestEnable);
            vr_put(&b, &ds->front, sizeof ds->front);
            vr_put(&b, &ds->back, sizeof ds->back);
            put_f(&b, ds->minDepthBounds);
            put_f(&b, ds->maxDepthBounds);
        }
        const VkPipelineColorBlendStateCreateInfo *cb = c->pColorBlendState;
        vr_u32(&b, cb ? 1 : 0);
        if (cb) {
            vr_u32(&b, cb->logicOpEnable);
            vr_u32(&b, cb->logicOp);
            vr_u32(&b, cb->attachmentCount);
            vr_put(&b, cb->pAttachments, cb->attachmentCount * sizeof *cb->pAttachments);
            vr_put(&b, cb->blendConstants, sizeof cb->blendConstants);
        }
        const VkPipelineDynamicStateCreateInfo *dy = c->pDynamicState;
        vr_u32(&b, dy ? dy->dynamicStateCount : 0);
        for (uint32_t k = 0; dy && k < dy->dynamicStateCount; k++) vr_u32(&b, dy->pDynamicStates[k]);
        // 末尾：dynamic rendering の描く先の形式（render pass の代わり）。古い受け手には付けない
        for (const VkBaseInStructure *s = c->pNext; s; s = s->pNext) {
            if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO || !(g_hw.caps & VR_CAP_DYNAMIC_RENDERING)) continue;
            const VkPipelineRenderingCreateInfo *pr = (const VkPipelineRenderingCreateInfo *) s;
            vr_buf t = { 0 };
            vr_u32(&t, pr->viewMask);
            vr_u32(&t, pr->colorAttachmentCount);
            for (uint32_t k = 0; k < pr->colorAttachmentCount; k++) vr_u32(&t, pr->pColorAttachmentFormats ? pr->pColorAttachmentFormats[k] : 0);
            vr_u32(&t, pr->depthAttachmentFormat);
            vr_u32(&t, pr->stencilAttachmentFormat);
            vr_u32(&b, s->sType);
            vr_bytes(&b, t.p, (uint32_t) t.n);
            free(t.p);
        }
        // 末尾：テッセレーションの設定。1 つのパッチの点の数と、面の向きの原点。
        // RTX は無くても描くが、MoltenVK は無いと何も描かない。古い受け手は知らない末尾を読み飛ばす
        if (c->pTessellationState) {
            const VkPipelineTessellationStateCreateInfo *ts = c->pTessellationState;
            uint32_t origin = VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
            for (const VkBaseInStructure *x = ts->pNext; x; x = x->pNext)
                if (x->sType == VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO)
                    origin = ((const VkPipelineTessellationDomainOriginStateCreateInfo *) x)->domainOrigin;
            vr_buf t = { 0 };
            vr_u32(&t, ts->patchControlPoints);
            vr_u32(&t, origin);
            vr_u32(&b, VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO);
            vr_bytes(&b, t.p, (uint32_t) t.n);
            free(t.p);
        }
        VkResult r = rpc_call(VR_CREATE_GRAPHICS_PIPELINE, &b, NULL);
        free(b.p);
        for (uint32_t k = 0; k < 8; k++) if (tmps[k]) destroy_shader(dv, tmps[k], NULL); // 一時のシェーダーは作り終えたら消す
        if (r != VK_SUCCESS) { say("graphics pipeline を作れない (%d)", r); out[i] = VK_NULL_HANDLE; worst = r; continue; }
        out[i] = (VkPipeline) (uintptr_t) id;
    }
    return worst;
}

// ---- サンプラー ----
static VkResult VKAPI_CALL create_sampler(VkDevice dv, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_put(&b, (const uint8_t *) ci + offsetof(VkSamplerCreateInfo, flags), sizeof *ci - offsetof(VkSamplerCreateInfo, flags));
    rpc_send(VR_CREATE_SAMPLER, &b);
    free(b.p);
    *out = (VkSampler) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_sampler(VkDevice dv, VkSampler h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_SAMPLER, dv, ID(h)); }

// ---- 窓（surface） ----
// 新しいローダーは、窓の口をドライバ自身に作らせる（作る入口が無いドライバには、窓の問い合わせを回さない）。
// ここでは VkIcdSurfaceWin32 の形で作って、窓のハンドルを覚えておくだけ
static VkResult VKAPI_CALL create_win32_surface(VkInstance inst, const VkWin32SurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void) inst; (void) a;
    VkIcdSurfaceWin32 *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_WIN32;
    s->hinstance = ci->hinstance;
    s->hwnd = ci->hwnd;
    *out = (VkSurfaceKHR) (uintptr_t) s;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_surface(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks *a) {
    (void) inst; (void) a;
    free((void *) (uintptr_t) s);
}

static HWND surface_hwnd(VkSurfaceKHR s) {
    VkIcdSurfaceBase *b = (VkIcdSurfaceBase *) (uintptr_t) s;
    if (!b || b->platform != VK_ICD_WSI_PLATFORM_WIN32) return NULL;
    return ((VkIcdSurfaceWin32 *) b)->hwnd;
}

static VkResult VKAPI_CALL surface_support(VkPhysicalDevice pd, uint32_t qf, VkSurfaceKHR s, VkBool32 *ok) {
    say("窓に出せるか（surface support）");
    (void) pd;
    *ok = qf < g_hw.nq && (g_hw.q[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) && surface_hwnd(s) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

static VkBool32 VKAPI_CALL win32_presentation_support(VkPhysicalDevice pd, uint32_t qf) {
    (void) pd;
    return qf < g_hw.nq && (g_hw.q[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

static VkResult VKAPI_CALL surface_caps(VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    say("窓の性質（surface caps）");
    (void) pd;
    memset(c, 0, sizeof *c);
    RECT rc = { 0, 0, 1, 1 };
    HWND w = surface_hwnd(s);
    if (w) GetClientRect(w, &rc);
    c->minImageCount = 2;
    c->maxImageCount = 8;
    c->currentExtent.width = rc.right - rc.left > 0 ? (uint32_t) (rc.right - rc.left) : 1;
    c->currentExtent.height = rc.bottom - rc.top > 0 ? (uint32_t) (rc.bottom - rc.top) : 1;
    // 本物の Windows のドライバと同じく、作れる大きさは窓の今の大きさだけにする。
    // 幅を持たせると、DXVK は描く大きさで作り、窓と違うので毎コマ作り直しになっていた
    c->minImageExtent = c->maxImageExtent = c->currentExtent;
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL surface_formats(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    say("窓の形式（surface formats）");
    (void) pd; (void) s;
    // Windows の窓（GDI）にそのまま描き写せる並び（B G R A）
    static const VkSurfaceFormatKHR fm[] = { { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }, { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } };
    const uint32_t all = 2;
    if (!f) { *n = all; return VK_SUCCESS; }
    uint32_t k = *n < all ? *n : all;
    memcpy(f, fm, k * sizeof *f);
    *n = k;
    return k < all ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL surface_modes(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    say("窓の出し方（present modes）");
    (void) pd; (void) s;
    static const VkPresentModeKHR md[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    const uint32_t all = 3;
    if (!m) { *n = all; return VK_SUCCESS; }
    uint32_t k = *n < all ? *n : all;
    memcpy(m, md, k * sizeof *m);
    *n = k;
    return k < all ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---- swapchain：受け手に画像を作り、出すたびに 1 コマを持ち帰って窓に描き写す ----
// 出すときは待たずに、画像を持ち帰り用のバッファに写す仕事だけ頼む。窓に描くのは 1 つ前のコマ
// （1 コマ遅れる代わりに、GPU が次のコマを描いている間に前のコマを運べる）。
// 持ち帰り用は画像ごとに別のメモリにする（受け手は同じメモリを使う仕事までしか待たないので、前のコマだけ先に受け取れる）
struct swapchain {
    struct dev *d;
    HWND hwnd;
    uint32_t w, h, n, next;
    VkFormat fmt;
    int use_jpeg;        // 受け手に JPEG に縮めてもらう
    uint64_t jpeg_bytes; // 受け取った JPEG の合計
    // 窓に描く糸。アプリの糸は JPEG を渡してすぐ戻り、この糸が最新の 1 枚だけをほどいて描く（間に合わない絵は飛ばす）
    struct {
        HANDLE thr;
        SRWLOCK lk;
        CONDITION_VARIABLE cv;
        vr_buf jpg;
        int has, quit;
        uint64_t dropped;
        uint64_t serial; // 渡した絵が何回目の present の物か（0 なら分からない）
    } disp;
    // 窓に出るまでの遅れ：present した時刻を覚え、その絵を窓に描いた時刻との差を足す
    uint64_t npres;
    double pres_t[64];
    uint64_t req[3];        // JPEG を頼んだ present の番号（新しい順）
    double lat_sum, lat_max;
    uint64_t lat_n;
    // present の区切り（書き換えに合わせる出し方・IPHONE_VK_MAX_FPS）。0 なら区切らない
    double gap, next_t, last_end;
    uint32_t asked_w, asked_h; // 作り直しを頼んだ（または作ったときの）窓の大きさ
    // 区切りの待ちがあるときは、今のコマの JPEG をその場で受け取る（遅れが少ない）。間に合わなくなったら先送りに戻す
    int low, slow_run;
    VkImage img[8];
    VkDeviceMemory imem[8];
    VkBuffer rb[8];         // 持ち帰り用（CPU から見えるメモリ）
    VkDeviceMemory rbmem[8];
    uint8_t *px[8];
    VkCommandPool pool;
    VkCommandBuffer cmd[8]; // 画像 i を持ち帰り用 i に写す（あらかじめ記録しておく）
    VkFence fence[8];
    int pend;               // 写しを頼んだが、まだ窓に出していない画像（無ければ -1）
    VkQueue q;
    uint64_t frames;
    double t0;
};

static uint32_t pick_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < g_hw.mem.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (g_hw.mem.memoryTypes[t].propertyFlags & want) == want) return t;
    for (uint32_t t = 0; t < g_hw.mem.memoryTypeCount; t++) if (bits & (1u << t)) return t;
    return 0;
}

static VkResult VKAPI_CALL queue_submit(VkQueue qh, uint32_t n, const VkSubmitInfo *si, VkFence f);
static VkResult VKAPI_CALL wait_fences(VkDevice dv, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout);
static VkResult VKAPI_CALL reset_fences(VkDevice dv, uint32_t n, const VkFence *fs);
static VkResult VKAPI_CALL create_fence(VkDevice dv, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out);
static void VKAPI_CALL destroy_fence(VkDevice dv, VkFence f, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL create_cpool(VkDevice dv, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out);
static void VKAPI_CALL destroy_cpool(VkDevice dv, VkCommandPool pp, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL alloc_cbs(VkDevice dv, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out);
static VkResult VKAPI_CALL begin_cb(VkCommandBuffer cbh, const VkCommandBufferBeginInfo *bi);
static VkResult VKAPI_CALL end_cb(VkCommandBuffer cbh);
static void VKAPI_CALL cmd_barrier(VkCommandBuffer cbh, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                   uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib);
static void VKAPI_CALL cmd_copy_image_to_buffer(VkCommandBuffer cbh, VkImage im, VkImageLayout l, VkBuffer bb, uint32_t n, const VkBufferImageCopy *rg);
static void VKAPI_CALL get_queue(VkDevice dv, uint32_t fam, uint32_t idx, VkQueue *out);

static void disp_stop(struct swapchain *sc);

static void VKAPI_CALL destroy_swapchain(VkDevice dv, VkSwapchainKHR h, const VkAllocationCallbacks *a) {
    (void) a;
    struct swapchain *sc = (struct swapchain *) h;
    if (!sc) return;
    if (sc->pend >= 0 && sc->fence[sc->pend]) wait_fences(dv, 1, &sc->fence[sc->pend], VK_TRUE, UINT64_MAX);
    disp_stop(sc);
    if (g_stall_hwnd == sc->hwnd) g_stall_hwnd = NULL;
    if (sc->pool) destroy_cpool(dv, sc->pool, NULL);
    for (uint32_t i = 0; i < sc->n; i++) {
        if (sc->fence[i]) destroy_fence(dv, sc->fence[i], NULL);
        if (sc->img[i]) destroy_image(dv, sc->img[i], NULL);
        if (sc->imem[i]) free_memory(dv, sc->imem[i], NULL);
        if (sc->rb[i]) destroy_buffer(dv, sc->rb[i], NULL);
        if (sc->rbmem[i]) free_memory(dv, sc->rbmem[i], NULL);
    }
    if (sc->frames) say("swapchain：%llu コマ・%.1f コマ/秒%s", (unsigned long long) sc->frames, sc->frames * 1000.0 / (now_ms() - sc->t0),
                        sc->use_jpeg ? "・JPEG" : "");
    if (sc->frames && sc->use_jpeg) say("JPEG：1 コマ平均 %.1f KB", sc->jpeg_bytes / 1024.0 / (double) sc->frames);
    if (sc->lat_n) say("present から窓に出るまで：平均 %.1f ms・いちばん長い %.1f ms（%llu 枚）", sc->lat_sum / (double) sc->lat_n, sc->lat_max,
                       (unsigned long long) sc->lat_n);
    free(sc);
}

// IPHONE_VK_MONITOR=sub（主でないモニター）か番号（1 から）なら、アプリの窓をそのモニターの左上へ動かす。大きさは変えない。
// 同じ窓は 1 回だけ動かす（作り直しのたびに動かすと、手で動かした場所が戻ってしまう）
static BOOL CALLBACK collect_monitor(HMONITOR m, HDC dc, LPRECT rc, LPARAM lp) {
    (void) dc; (void) rc;
    HMONITOR *list = (HMONITOR *) lp;
    for (int i = 0; i < 8; i++) if (!list[i]) { list[i] = m; break; }
    return TRUE;
}

static void move_to_monitor(HWND hwnd) {
    static HWND moved[16];
    const char *e = getenv("IPHONE_VK_MONITOR");
    if (!hwnd || !e || !*e) return;
    for (int i = 0; i < 16; i++) if (moved[i] == hwnd) return;
    for (int i = 0; i < 16; i++) if (!moved[i]) { moved[i] = hwnd; break; }
    HMONITOR list[8] = { 0 };
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM) list);
    HMONITOR target = NULL;
    if (!strcmp(e, "sub")) {
        for (int i = 0; i < 8 && list[i]; i++) {
            MONITORINFO mi = { .cbSize = sizeof mi };
            if (GetMonitorInfoA(list[i], &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) { target = list[i]; break; }
        }
    } else {
        int k = atoi(e) - 1;
        if (k >= 0 && k < 8) target = list[k];
    }
    MONITORINFO mi = { .cbSize = sizeof mi };
    if (!target || !GetMonitorInfoA(target, &mi)) { say("窓を動かす先のモニターが見つからない（%s）", e); return; }
    SetWindowPos(hwnd, NULL, mi.rcWork.left + 40, mi.rcWork.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    say("窓をモニター（%ld, %ld）へ動かした", mi.rcWork.left, mi.rcWork.top);
}

// 窓のあるモニターの書き換えの速さ（分からなければ 60）
static double monitor_hz(HWND w) {
    HMONITOR mon = w ? MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY) : NULL;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    DEVMODEW dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (mon && GetMonitorInfoW(mon, (MONITORINFO *) &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60;
}

static VkResult VKAPI_CALL create_swapchain(VkDevice dv, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    (void) a;
    struct swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->d = (struct dev *) dv;
    sc->hwnd = surface_hwnd(ci->surface);
    move_to_monitor(sc->hwnd);
    g_stall_hwnd = sc->hwnd;
    {
        RECT rc0 = { 0 };
        if (sc->hwnd) GetClientRect(sc->hwnd, &rc0);
        sc->asked_w = (uint32_t) (rc0.right - rc0.left);
        sc->asked_h = (uint32_t) (rc0.bottom - rc0.top);
    }
    sc->w = ci->imageExtent.width;
    sc->h = ci->imageExtent.height;
    sc->fmt = ci->imageFormat;
    {
        const char *e = getenv("IPHONE_VK_JPEG");
        sc->use_jpeg = (g_hw.caps & VR_CAP_JPEG) && !(e && *e == '0');
    }
    // 書き換えに合わせる出し方（FIFO）なら、窓のあるモニターの書き換えの速さで区切る。
    // これまでは名乗るだけで全速で回し、iPhone に見えないコマまで描かせていた。IPHONE_VK_VSYNC=0 で区切らない（速さを測るとき）
    {
        const char *mf = getenv("IPHONE_VK_MAX_FPS"), *vs = getenv("IPHONE_VK_VSYNC");
        double hz = 0;
        if (mf && atof(mf) > 0) hz = atof(mf);
        else if ((ci->presentMode == VK_PRESENT_MODE_FIFO_KHR || ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) && !(vs && *vs == '0'))
            hz = monitor_hz(sc->hwnd);
        sc->gap = hz > 0 ? 1000.0 / hz : 0;
        sc->low = sc->gap > 0;
        if (hz > 0) {
            timeBeginPeriod(1);
            say("present を %.0f コマ/秒で区切る（%s）", hz, mf && atof(mf) > 0 ? "IPHONE_VK_MAX_FPS" : "書き換えに合わせる出し方");
        }
    }
    sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount > 8 ? 8 : ci->minImageCount;
    sc->pend = -1;
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < sc->n && r == VK_SUCCESS; i++) {
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = ci->imageFormat,
            .extent = { sc->w, sc->h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        if ((r = create_image(dv, &ic, NULL, &sc->img[i])) != VK_SUCCESS) break;
        VkMemoryRequirements req;
        image_reqs(dv, sc->img[i], &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = pick_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        if ((r = alloc_memory(dv, &mai, NULL, &sc->imem[i])) != VK_SUCCESS) break;
        bind_image(dv, sc->img[i], sc->imem[i], 0);
    }
    for (uint32_t i = 0; i < sc->n && r == VK_SUCCESS; i++) {
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize) sc->w * sc->h * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        if ((r = create_buffer(dv, &bc, NULL, &sc->rb[i])) != VK_SUCCESS) break;
        VkMemoryRequirements req;
        buffer_reqs(dv, sc->rb[i], &req);
        // 受け手の CPU が毎コマ読むので、キャッシュの効くメモリを先に探す（効かないと 640 x 480 を写すだけで 8 ms かかった）
        const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t mt = pick_mem(req.memoryTypeBits, vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if ((g_hw.mem.memoryTypes[mt].propertyFlags & vis) != vis) mt = pick_mem(req.memoryTypeBits, vis);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = mt };
        if ((r = alloc_memory(dv, &mai, NULL, &sc->rbmem[i])) != VK_SUCCESS) break;
        // 受け手が JPEG に縮められるなら、引き取らずに JPEG で受け取る（IPHONE_VK_JPEG=0 で止める）。無理なら違いだけ受け取る
        if (sc->use_jpeg) ((struct mem *) sc->rbmem[i])->jpeg = 1;
        else ((struct mem *) sc->rbmem[i])->delta = 1;
        bind_buffer(dv, sc->rb[i], sc->rbmem[i], 0);
        map_memory(dv, sc->rbmem[i], 0, VK_WHOLE_SIZE, 0, (void **) &sc->px[i]);
        VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        r = create_fence(dv, &fc, NULL, &sc->fence[i]);
    }
    uint32_t fam = 0;
    for (uint32_t i = 0; i < g_hw.nq; i++) if (g_hw.q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
    if (r == VK_SUCCESS) {
        VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = fam };
        r = create_cpool(dv, &pc, NULL, &sc->pool);
    }
    if (r == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = sc->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = sc->n };
        r = alloc_cbs(dv, &ai, sc->cmd);
    }
    for (uint32_t i = 0; i < sc->n && r == VK_SUCCESS; i++) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_cb(sc->cmd[i], &bi);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
        cmd_barrier(sc->cmd[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sc->w, sc->h, 1 } };
        cmd_copy_image_to_buffer(sc->cmd[i], sc->img[i], VK_IMAGE_LAYOUT_GENERAL, sc->rb[i], 1, &rg);
        end_cb(sc->cmd[i]);
    }
    if (r == VK_SUCCESS) get_queue(dv, fam, 0, &sc->q);
    if (r != VK_SUCCESS) { destroy_swapchain(dv, (VkSwapchainKHR) sc, NULL); return r; }
    if (ci->oldSwapchain) say("swapchain を作り直した（%u x %u）", sc->w, sc->h);
    else say("swapchain：%u x %u・%u 枚", sc->w, sc->h, sc->n);
    sc->t0 = now_ms();
    *out = (VkSwapchainKHR) sc;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL swapchain_images(VkDevice dv, VkSwapchainKHR h, uint32_t *n, VkImage *out) {
    (void) dv;
    struct swapchain *sc = (struct swapchain *) h;
    if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = *n < sc->n ? *n : sc->n;
    memcpy(out, sc->img, k * sizeof *out);
    *n = k;
    return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL acquire_image(VkDevice dv, VkSwapchainKHR h, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx) {
    (void) dv; (void) timeout;
    struct swapchain *sc = (struct swapchain *) h;
    *idx = sc->next;
    sc->next = (sc->next + 1) % sc->n;
    // 出すときに持ち帰りまで済ませているので、画像はもう空いている。知らせる物があれば、空の submit で知らせる
    if (sem || f) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem };
        return queue_submit(sc->q, 1, &si, f);
    }
    return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult VKAPI_CALL acquire_image2(VkDevice dv, const VkAcquireNextImageInfoKHR *ai, uint32_t *idx) {
    return acquire_image(dv, ai->swapchain, ai->timeout, ai->semaphore, ai->fence, idx);
}

// 持ち帰った画像 j を窓に描き写す
// 受け手に JPEG にしてもらったコマを、WIC でほどいて写し（BGRA）に書く。失敗したら 0
// JPEG をほどいて、w×h の B・G・R・A の並びにする。1 ほどけた・-1 大きさが違う（窓の大きさが変わる前の絵）・0 失敗
static int jpeg_decode(IWICImagingFactory *fac, const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint8_t *px) {
    int ok = 0;
    UINT jw = 0, jh = 0;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    if (!n) return 0;
    if (FAILED(IWICImagingFactory_CreateStream(fac, &st))) goto done;
    if (FAILED(IWICStream_InitializeFromMemory(st, (BYTE *) data, (DWORD) n))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, (IStream *) st, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) goto done;
    // 先送りでは、窓の大きさが変わる前のコマが届くことがある。大きさが違えば出さない
    if (FAILED(IWICBitmapFrameDecode_GetSize(fr, &jw, &jh))) goto done;
    if (jw != w || jh != h) { ok = -1; goto done; }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &cv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *) fr, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(cv, NULL, w * 4, w * 4 * h, px))) goto done;
    ok = 1;
done:
    if (cv) IWICFormatConverter_Release(cv);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (st) IWICStream_Release(st);
    return ok;
}

// 受け手に縮めてもらった JPEG を取ってくる。1 取れた・-1 まだ出す物が無い（先送りの最初）・0 失敗
// 今の設定で、返ってきた JPEG が何回前に頼んだ物か（先送りで 1・返事を次のコマで受け取るので 1）
static int jpeg_lag(void);

static int jpeg_request(struct swapchain *sc, uint32_t j, vr_buf *out) {
    static int q = -1, pipe = -1;
    if (q < 0) { const char *e = getenv("IPHONE_VK_JPEG_Q"); q = e && atoi(e) > 0 ? atoi(e) : 80; }
    // 先送り：受け手は今のコマを後ろで縮め、ひとつ前のコマの JPEG を返す（窓に出るのは 1 コマ遅れる）
    if (pipe < 0) { const char *e = getenv("IPHONE_VK_JPEG_PIPE"); pipe = (g_hw.caps & VR_CAP_JPEG_PIPE) && !(e && *e == '0'); }
    // 返事も待たない：頼んだら次のコマで受け取る。そのあいだにアプリは次のコマを作れる（窓に出るのはもう 1 コマ遅れる）。
    // IPHONE_VK_ASYNC_PRESENT=0 で、今までどおり待つ
    static int async = -1;
    if (async < 0) { const char *e = getenv("IPHONE_VK_ASYNC_PRESENT"); async = pipe && !(e && *e == '0'); }
    vr_buf b = { 0 };
    vr_u64(&b, ID(sc->rbmem[j]));
    vr_u64(&b, 0);
    vr_u32(&b, sc->w);
    vr_u32(&b, sc->h);
    vr_u32(&b, sc->w * 4);
    vr_u32(&b, sc->fmt == VK_FORMAT_B8G8R8A8_UNORM || sc->fmt == VK_FORMAT_B8G8R8A8_SRGB ? 1 : 0);
    vr_u32(&b, (uint32_t) q);
    VkResult r;
    out->n = 0;
    if (async) {
        r = rpc_take_deferred(out); // 前のコマで頼んだ返事
        rpc_call_deferred(VR_READ_JPEG_PIPE, &b);
        if (r == VK_NOT_READY) r = VK_SUCCESS; // 最初のコマ：まだ出す物が無い
    } else {
        r = rpc_call(pipe ? VR_READ_JPEG_PIPE : VR_READ_JPEG, &b, out);
    }
    free(b.p);
    if (r == VK_SUCCESS && !out->n && pipe) return -1;
    if (r != VK_SUCCESS || !out->n) return 0;
    sc->jpeg_bytes += out->n;
    return 1;
}

static int jpeg_lag(void) {
    static int lag = -1;
    if (lag < 0) {
        const char *p = getenv("IPHONE_VK_JPEG_PIPE"), *a = getenv("IPHONE_VK_ASYNC_PRESENT");
        int pipe = (g_hw.caps & VR_CAP_JPEG_PIPE) && !(p && *p == '0');
        lag = pipe + (pipe && !(a && *a == '0'));
    }
    return lag;
}

// 頼んだ present の番号を覚え、いま返ってきた JPEG の番号を返す
static uint64_t jpeg_serial(struct swapchain *sc, uint64_t now) {
    sc->req[2] = sc->req[1];
    sc->req[1] = sc->req[0];
    sc->req[0] = now;
    int lag = jpeg_lag();
    return lag <= 2 ? sc->req[lag] : 0;
}

static int jpeg_fetch(struct swapchain *sc, uint32_t j) {
    static IWICImagingFactory *fac;
    if (!fac) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **) &fac))) return 0;
    }
    vr_buf out = { 0 };
    int k = jpeg_request(sc, j, &out);
    if (k == 1) k = jpeg_decode(fac, out.p, out.n, sc->w, sc->h, sc->px[j]);
    free(out.p);
    return k;
}

static void dump_frame(struct swapchain *sc, const uint8_t *px);

// 窓に描く糸：渡された最新の JPEG をほどいて描く
static DWORD WINAPI disp_thread(LPVOID arg) {
    struct swapchain *sc = (struct swapchain *) arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *fac = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **) &fac);
    uint8_t *px = malloc((size_t) sc->w * sc->h * 4);
    vr_buf mine = { 0 };
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG) sc->w;
    bmi.bmiHeader.biHeight = -(LONG) sc->h; // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    for (;;) {
        AcquireSRWLockExclusive(&sc->disp.lk);
        while (!sc->disp.has && !sc->disp.quit) SleepConditionVariableSRW(&sc->disp.cv, &sc->disp.lk, INFINITE, 0);
        if (!sc->disp.has) { ReleaseSRWLockExclusive(&sc->disp.lk); break; }
        vr_buf t = mine;
        mine = sc->disp.jpg;
        sc->disp.jpg = t;
        sc->disp.has = 0;
        uint64_t serial = sc->disp.serial;
        ReleaseSRWLockExclusive(&sc->disp.lk);
        if (!fac || !px || jpeg_decode(fac, mine.p, mine.n, sc->w, sc->h, px) != 1) continue;
        RECT rc;
        GetClientRect(sc->hwnd, &rc);
        HDC dc = GetDC(sc->hwnd);
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, (int) sc->w, (int) sc->h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(sc->hwnd, dc);
        GdiFlush();
        if (serial && sc->npres - serial < 64) {
            double lat = now_ms() - sc->pres_t[serial % 64];
            sc->lat_sum += lat;
            sc->lat_n++;
            if (lat > sc->lat_max) sc->lat_max = lat;
            g_stat.lat_sum += lat;
            g_stat.lat_n++;
            if (lat > g_stat.lat_max) g_stat.lat_max = lat;
        }
        sc->frames++;
        dump_frame(sc, px);
    }
    free(px);
    free(mine.p);
    if (fac) IWICImagingFactory_Release(fac);
    CoUninitialize();
    return 0;
}

// 表示の糸を使うか（IPHONE_VK_DISPLAY_THREAD=0 で、今までどおりアプリの糸で描く）
static int disp_thread_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_DISPLAY_THREAD"); v = !(e && *e == '0'); }
    return v;
}

static void disp_stop(struct swapchain *sc) {
    if (!sc->disp.thr) return;
    AcquireSRWLockExclusive(&sc->disp.lk);
    sc->disp.quit = 1;
    WakeConditionVariable(&sc->disp.cv);
    ReleaseSRWLockExclusive(&sc->disp.lk);
    WaitForSingleObject(sc->disp.thr, 5000);
    CloseHandle(sc->disp.thr);
    sc->disp.thr = NULL;
    free(sc->disp.jpg.p);
    sc->disp.jpg.p = NULL;
    if (sc->disp.dropped) say("窓に描く糸が間に合わず飛ばした絵：%llu 枚", (unsigned long long) sc->disp.dropped);
}

// IPHONE_VK_DUMP=コマ番号:ファイル名 で、そのコマを PPM に書く（絵が正しいかを確かめる用）
static void dump_frame(struct swapchain *sc, const uint8_t *px) {
    const char *dump = getenv("IPHONE_VK_DUMP");
    if (dump && (uint64_t) atoi(dump) == sc->frames && strchr(dump, ':')) {
        FILE *fp = fopen(strchr(dump, ':') + 1, "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", sc->w, sc->h);
            for (size_t p = 0; p < (size_t) sc->w * sc->h; p++) { uint8_t rgb[3] = { px[p * 4 + 2], px[p * 4 + 1], px[p * 4] }; fwrite(rgb, 1, 3, fp); }
            fclose(fp);
            say("%llu コマ目を %s に書いた", (unsigned long long) sc->frames, strchr(dump, ':') + 1);
        }
    }
}

// 窓の大きさが swapchain と違い、しかもその窓の大きさでまだ頼んでいなければ、作り直しを 1 回だけ頼む。
// DXVK-macOS は窓でなく描く大きさで作るので、毎コマ頼むと作り直しが止まらない（違う大きさのままでも、窓に合わせて伸ばして描く）
static int ask_resize(struct swapchain *sc, const RECT *rc) {
    uint32_t w = (uint32_t) (rc->right - rc->left), h = (uint32_t) (rc->bottom - rc->top);
    if (!w || !h || (w == sc->w && h == sc->h) || (w == sc->asked_w && h == sc->asked_h)) return 0;
    sc->asked_w = w;
    sc->asked_h = h;
    return 1;
}

// JPEG を表示の糸に渡す（out の中身は入れ替わる）
static void disp_give(struct swapchain *sc, vr_buf *out, uint64_t serial) {
    if (!sc->disp.thr) {
        InitializeSRWLock(&sc->disp.lk);
        InitializeConditionVariable(&sc->disp.cv);
        sc->disp.thr = CreateThread(NULL, 0, disp_thread, sc, 0, NULL);
    }
    AcquireSRWLockExclusive(&sc->disp.lk);
    if (sc->disp.has) sc->disp.dropped++;
    vr_buf t = sc->disp.jpg;
    sc->disp.jpg = *out;
    *out = t;
    sc->disp.has = 1;
    sc->disp.serial = serial;
    WakeConditionVariable(&sc->disp.cv);
    ReleaseSRWLockExclusive(&sc->disp.lk);
}

// 今のコマの JPEG をその場で受け取って、表示の糸に渡す（区切りの待ちがあるときの、遅れの少ないやり方）
static VkResult blit_now(struct swapchain *sc, uint32_t j, uint64_t serial) {
    static int q = -1;
    if (q < 0) { const char *e = getenv("IPHONE_VK_JPEG_Q"); q = e && atoi(e) > 0 ? atoi(e) : 80; }
    vr_buf b = { 0 }, out = { 0 };
    vr_u64(&b, ID(sc->rbmem[j]));
    vr_u64(&b, 0);
    vr_u32(&b, sc->w);
    vr_u32(&b, sc->h);
    vr_u32(&b, sc->w * 4);
    vr_u32(&b, sc->fmt == VK_FORMAT_B8G8R8A8_UNORM || sc->fmt == VK_FORMAT_B8G8R8A8_SRGB ? 1 : 0);
    vr_u32(&b, (uint32_t) q);
    VkResult r = rpc_call(VR_READ_JPEG, &b, &out);
    free(b.p);
    if (r == VK_SUCCESS && out.n) {
        sc->jpeg_bytes += out.n;
        disp_give(sc, &out, serial);
    }
    free(out.p);
    if (r != VK_SUCCESS) return r;
    RECT rc;
    GetClientRect(sc->hwnd, &rc);
    if (ask_resize(sc, &rc)) return VK_SUBOPTIMAL_KHR;
    return VK_SUCCESS;
}

static VkResult blit(struct swapchain *sc, uint32_t j, uint64_t serial) {
    if (!sc->hwnd) return VK_SUCCESS;
    uint8_t *px = sc->px[j];
    if (sc->use_jpeg && disp_thread_on()) {
        // JPEG を取ってきて表示の糸に渡し、ほどいて描くのを待たずに戻る
        vr_buf out = { 0 };
        int k = jpeg_request(sc, j, &out);
        uint64_t got = jpeg_serial(sc, serial);
        if (k == 1) disp_give(sc, &out, got);
        free(out.p);
        RECT rc;
        GetClientRect(sc->hwnd, &rc);
        if (ask_resize(sc, &rc)) return VK_SUBOPTIMAL_KHR;
        return VK_SUCCESS;
    }
    // JPEG で受け取ったときは、もう B・G・R・A の並びになっている
    int is_bgra = 0;
    if (sc->use_jpeg) {
        int k = jpeg_fetch(sc, j);
        if (k < 0) return VK_SUCCESS; // 出す物がまだ無い
        is_bgra = k;
    }
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG) sc->w;
    bmi.bmiHeader.biHeight = -(LONG) sc->h; // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    if (!is_bgra && (sc->fmt == VK_FORMAT_R8G8B8A8_UNORM || sc->fmt == VK_FORMAT_R8G8B8A8_SRGB))
        for (size_t p = 0; p < (size_t) sc->w * sc->h; p++) { uint8_t t = px[p * 4]; px[p * 4] = px[p * 4 + 2]; px[p * 4 + 2] = t; }
    RECT rc;
    GetClientRect(sc->hwnd, &rc);
    HDC dc = GetDC(sc->hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, (int) sc->w, (int) sc->h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(sc->hwnd, dc);
    sc->frames++;
    dump_frame(sc, px);
    // 窓の大きさが変わったら、作り直してもらう
    if (ask_resize(sc, &rc)) return VK_SUBOPTIMAL_KHR;
    return VK_SUCCESS;
}

// iPhone の熱の状態を 2 秒ごとに聞く。熱い（2）以上の間は、コマ数を IPHONE_VK_HEAT_FPS（標準 30・0 で落とさない）に落とす。
// iOS は熱いと自分で GPU を遅くするが、コマ数を落として熱を出さない方が、速さの上がり下がりが少ない
static int g_heat = -1;
static double g_heat_gap;
static void heat_check(void) {
    static double last;
    if (!(g_hw.caps & VR_CAP_STATUS) || g_dead) return;
    double t = now_ms();
    if (t - last < 2000) return;
    last = t;
    vr_buf b = { 0 }, out = { 0 };
    VkResult r = rpc_call(VR_GET_STATUS, &b, &out);
    free(b.p);
    uint32_t h = 0;
    if (r == VK_SUCCESS && out.n >= 4) memcpy(&h, out.p, 4);
    free(out.p);
    if (r != VK_SUCCESS || h > 3 || (int) h == g_heat) return;
    static double fps = -1;
    if (fps < 0) { const char *e = getenv("IPHONE_VK_HEAT_FPS"); fps = e ? atof(e) : 30; }
    double gap = h >= 2 && fps > 0 ? 1000.0 / fps : 0;
    if (gap > 0 && g_heat_gap <= 0) timeBeginPeriod(1);
    static const char *const NAME[] = { "ふつう", "やや熱い", "熱い", "とても熱い" };
    if (g_heat >= 0 || h > 0)
        fprintf(stderr, "[iphone-vk] iPhone の熱：%s%s\n", NAME[h], gap > 0 ? "。冷めるまでコマ数を落とす" : g_heat_gap > 0 ? "。コマ数を戻す" : "");
    g_heat_gap = gap;
    g_heat = (int) h;
    if ((int) h > g_stat.heat_max) g_stat.heat_max = (int) h;
}

// present の間を区切りより短くしない（熱いときの区切りの方が長ければそちら）。待った時間を返す
static double pace_present(struct swapchain *sc) {
    double gap = sc->gap > g_heat_gap ? sc->gap : g_heat_gap;
    if (gap <= 0) return 0;
    double t = now_ms(), t0 = t;
    if (sc->next_t > t) {
        if (sc->next_t - t > 2) Sleep((DWORD) (sc->next_t - t - 1.5));
        while (now_ms() < sc->next_t) YieldProcessor();
        t = sc->next_t;
    }
    // 遅れたときは、取り返そうとして詰めずに、今から数え直す
    sc->next_t = (sc->next_t > 0 && t - sc->next_t < gap) ? sc->next_t + gap : t + gap;
    return now_ms() - t0;
}

static VkResult VKAPI_CALL queue_present(VkQueue qh, const VkPresentInfoKHR *pi) {
    VkResult worst = VK_SUCCESS;
    struct swapchain *sc0 = pi->swapchainCount ? (struct swapchain *) pi->pSwapchains[0] : NULL;
    heat_check();
    double slept = sc0 ? pace_present(sc0) : 0;
    double tp = now_ms();
    g_stat.presents++;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct swapchain *sc = (struct swapchain *) pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        VkDevice dv = (VkDevice) sc->d;
        // 描き終わりを待ってから画像を持ち帰り用に写す仕事を頼む（待たない）
        VkPipelineStageFlags st[16];
        for (uint32_t k = 0; k < 16; k++) st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = i == 0 ? (pi->waitSemaphoreCount < 16 ? pi->waitSemaphoreCount : 16) : 0,
            .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = st, .commandBufferCount = 1, .pCommandBuffers = &sc->cmd[idx] };
        uint64_t serial = ++sc->npres;
        sc->pres_t[serial % 64] = now_ms();
        // 窓の絵を先送りで受け取るときは、受け手が写し終わりを待ってから縮めるので、PC からは待たない。
        // 毎コマの往復が 1 回減り、窓に出るのが 1 コマ早くなる。IPHONE_VK_PEND=1 で前のやり方
        static int direct = -1;
        if (direct < 0) {
            const char *e = getenv("IPHONE_VK_PEND");
            direct = !(e && *e == '1') && sc->use_jpeg && disp_thread_on() && jpeg_lag() == 2;
            say(direct ? "窓の絵：前のコマを待たずに頼む" : "窓の絵：前のコマの描き終わりを待ってから頼む");
        }
        if (direct && sc->use_jpeg) {
            VkResult r = queue_submit(qh, 1, &si, VK_NULL_HANDLE);
            double tb = now_ms();
            if (r == VK_SUCCESS) r = sc->low && sc->hwnd ? blit_now(sc, idx, serial) : blit(sc, idx, serial);
            g_stat.blit_ms += now_ms() - tb;
            g_stat.blits++;
            // 前の present の終わりから今までの、区切りで待った分を除いた長さが、区切りの 9 割を 10 回続けて超えたら、先送りに戻す
            if (sc->low && i == 0) {
                double busy = sc->last_end > 0 ? now_ms() - sc->last_end - slept : 0;
                sc->slow_run = busy > sc->gap * 0.9 ? sc->slow_run + 1 : 0;
                if (sc->slow_run >= 10) {
                    sc->low = 0;
                    say("その場で受け取ると間に合わないので、先送りに切り替えた（1 コマ %.1f ms・区切り %.1f ms）", busy, sc->gap);
                }
            }
            if (pi->pResults) pi->pResults[i] = r;
            if (r < 0 || (worst == VK_SUCCESS && r != VK_SUCCESS)) worst = r;
            continue;
        }
        VkResult r = queue_submit(qh, 1, &si, sc->fence[idx]);
        // 1 つ前に頼んだコマを受け取って窓に描く（GPU はその間にさっき頼んだ分を進める）
        if (r == VK_SUCCESS && sc->pend >= 0) {
            uint32_t j = (uint32_t) sc->pend;
            r = wait_fences(dv, 1, &sc->fence[j], VK_TRUE, UINT64_MAX);
            reset_fences(dv, 1, &sc->fence[j]);
            double tb = now_ms();
            if (r == VK_SUCCESS) r = blit(sc, j, serial - 1);
            g_stat.blit_ms += now_ms() - tb;
            g_stat.blits++;
        }
        sc->pend = (int) idx;
        if (pi->pResults) pi->pResults[i] = r;
        if (r < 0 || (worst == VK_SUCCESS && r != VK_SUCCESS)) worst = r;
    }
    g_stat.present_ms += now_ms() - tp;
    if (sc0) sc0->last_end = now_ms();
    return worst;
}

// ---- shader・パイプライン ----
static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_bytes(&b, ci->pCode, (uint32_t) ci->codeSize);
    rpc_send(VR_CREATE_SHADER_MODULE, &b);
    free(b.p);
    gs_keep(id, ci->pCode, ci->codeSize);
    *out = (VkShaderModule) (uintptr_t) id;
    return VK_SUCCESS;
}

#define DESTROY_SIMPLE(fn, T, op) \
    static void VKAPI_CALL fn(VkDevice dv, T h, const VkAllocationCallbacks *a) { \
        (void) a; \
        if (!h) return; \
        vr_buf b = { 0 }; \
        vr_u64(&b, ID(dv)); \
        vr_u64(&b, ID(h)); \
        rpc_send(op, &b); \
        free(b.p); \
    }
DESTROY_SIMPLE(destroy_shader_rpc, VkShaderModule, VR_DESTROY_SHADER_MODULE)
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a) { gs_forget_pipe(VK_NULL_HANDLE, ID(h)); destroy_shader_rpc(dv, h, a); }
DESTROY_SIMPLE(destroy_playout_rpc, VkPipelineLayout, VR_DESTROY_PIPELINE_LAYOUT)
static void pl_forget(uint64_t id);
static void VKAPI_CALL destroy_playout(VkDevice dv, VkPipelineLayout h, const VkAllocationCallbacks *a) { gs_forget_layout(dv, ID(h)); pl_forget(ID(h)); destroy_playout_rpc(dv, h, a); }
DESTROY_SIMPLE(destroy_pipeline_rpc, VkPipeline, VR_DESTROY_PIPELINE)
static void VKAPI_CALL destroy_pipeline(VkDevice dv, VkPipeline h, const VkAllocationCallbacks *a) { gs_forget_pipe(dv, ID(h)); destroy_pipeline_rpc(dv, h, a); }

static VkResult VKAPI_CALL create_dsl(VkDevice dv, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void) a;
    struct dsl *l = calloc(1, sizeof *l);
    if (!l) return VK_ERROR_OUT_OF_HOST_MEMORY;
    l->refs = 1;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        uint32_t t = ci->pBindings[i].descriptorType, k = 0;
        while (k < l->nt && l->t[k].type != t) k++;
        if (k == l->nt && l->nt < 16) l->t[l->nt++].type = t;
        if (k < 16) l->t[k].count += ci->pBindings[i].descriptorCount;
    }
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *bb = &ci->pBindings[i];
        if (!bb->pImmutableSamplers || !bb->descriptorCount ||
            (bb->descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER && bb->descriptorType != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
            continue;
        void *p = realloc(l->imm, (l->nimm + 1) * sizeof *l->imm);
        VkSampler *s = malloc(bb->descriptorCount * sizeof *s);
        if (!p || !s) { if (p) l->imm = p; free(s); break; }
        l->imm = p;
        memcpy(s, bb->pImmutableSamplers, bb->descriptorCount * sizeof *s);
        l->imm[l->nimm].binding = bb->binding;
        l->imm[l->nimm].type = bb->descriptorType;
        l->imm[l->nimm].count = bb->descriptorCount;
        l->imm[l->nimm].s = s;
        l->nimm++;
    }
    uint64_t id = ID(l);
    l->push = (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags & ~(uint32_t) VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    vr_u32(&b, ci->bindingCount);
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        vr_u32(&b, ci->pBindings[i].binding);
        vr_u32(&b, ci->pBindings[i].descriptorType);
        vr_u32(&b, ci->pBindings[i].descriptorCount);
        vr_u32(&b, gs_stages(ci->pBindings[i].stageFlags));
    }
    rpc_send(VR_CREATE_DSL, &b);
    free(b.p);
    *out = (VkDescriptorSetLayout) l;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_dsl(VkDevice dv, VkDescriptorSetLayout h, const VkAllocationCallbacks *a) {
    (void) a;
    if (!h) return;
    send_destroy(VR_DESTROY_DSL, dv, ID(h));
    dsl_unref((struct dsl *) h);
}

static void VKAPI_CALL dsl_support(VkDevice dv, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s) {
    (void) dv; (void) ci;
    s->supported = VK_TRUE;
}

static void pl_note(uint64_t id, const VkPipelineLayoutCreateInfo *ci);
static VkResult VKAPI_CALL create_playout(VkDevice dv, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->setLayoutCount);
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) vr_u64(&b, ID(ci->pSetLayouts[i]));
    vr_u32(&b, ci->pushConstantRangeCount);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) {
        vr_u32(&b, gs_stages(ci->pPushConstantRanges[i].stageFlags));
        vr_u32(&b, ci->pPushConstantRanges[i].offset);
        vr_u32(&b, ci->pPushConstantRanges[i].size);
    }
    rpc_send(VR_CREATE_PIPELINE_LAYOUT, &b);
    free(b.p);
    gs_note_layout(dv, id, ci);
    pl_note(id, ci);
    *out = (VkPipelineLayout) (uintptr_t) id;
    return VK_SUCCESS;
}

// パイプラインの置き場（受け手には送らない。中身は空のまま）
static VkResult VKAPI_CALL create_pcache(VkDevice dv, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void) dv; (void) ci; (void) a;
    *out = (VkPipelineCache) (uintptr_t) new_id();
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_pcache(VkDevice dv, VkPipelineCache c, const VkAllocationCallbacks *a) { (void) dv; (void) c; (void) a; }
static VkResult VKAPI_CALL pcache_data(VkDevice dv, VkPipelineCache c, size_t *n, void *p) { (void) dv; (void) c; (void) p; *n = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL merge_pcache(VkDevice dv, VkPipelineCache c, uint32_t n, const VkPipelineCache *s) { (void) dv; (void) c; (void) n; (void) s; return VK_SUCCESS; }

static VkResult VKAPI_CALL create_compute_pipelines(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void) cache; (void) a;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t id = new_id();
        const VkPipelineShaderStageCreateInfo *st = &ci[i].stage;
        vr_buf b = { 0 };
        vr_u64(&b, ID(dv));
        vr_u64(&b, id);
        vr_u32(&b, ci[i].flags);
        vr_u64(&b, ID(ci[i].layout));
        vr_u32(&b, st->flags);
        // 新しい DXVK は compute でも、シェーダーを段の pNext に直接入れてくる
        VkShaderModule tmp;
        vr_u64(&b, ID(stage_module(dv, st, &tmp)));
        vr_str(&b, st->pName);
        const VkSpecializationInfo *si = st->pSpecializationInfo;
        vr_u32(&b, si ? si->mapEntryCount : 0);
        for (uint32_t k = 0; si && k < si->mapEntryCount; k++) {
            vr_u32(&b, si->pMapEntries[k].constantID);
            vr_u32(&b, si->pMapEntries[k].offset);
            vr_u32(&b, (uint32_t) si->pMapEntries[k].size);
        }
        vr_bytes(&b, si ? si->pData : NULL, si ? (uint32_t) si->dataSize : 0);
        uint32_t req = 0;
        for (const VkBaseInStructure *s = st->pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO)
                req = ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *) s)->requiredSubgroupSize;
        vr_u32(&b, req);
        // 作れたかどうかは返事で確かめる（iPhone では SPIR-V を Metal に直すときに失敗することがある）
        VkResult r = rpc_call(VR_CREATE_COMPUTE_PIPELINE, &b, NULL);
        free(b.p);
        if (tmp) destroy_shader(dv, tmp, NULL);
        if (r != VK_SUCCESS) { say("compute pipeline を作れない (%d)", r); out[i] = VK_NULL_HANDLE; worst = r; continue; }
        out[i] = (VkPipeline) (uintptr_t) id;
    }
    return worst;
}

// ---- descriptor ----
// 全部の descriptor pool の一覧。バッファを消したときに、それを指している descriptor を外すために使う
static SRWLOCK g_pools_lock = SRWLOCK_INIT;
static struct dpool **g_pools;
static size_t g_npools, g_cappools;

// 消すバッファを指している descriptor を外す。外さないと、その set をもう一度使ったときに、
// 消えたバッファ（解放したメモリ）を読みに行って落ちる（stable-diffusion.cpp が作業用のバッファを作り直すときに起きた）
static void forget_buffer_in_sets(struct buf *bf) {
    AcquireSRWLockShared(&g_pools_lock);
    for (size_t i = 0; i < g_npools; i++)
        for (struct dset *s = g_pools[i]->head; s; s = s->next)
            for (uint32_t k = 0; k < s->n; k++)
                if (s->e[k].b == bf) s->e[k].b = NULL;
    ReleaseSRWLockShared(&g_pools_lock);
}

static VkResult VKAPI_CALL create_dpool(VkDevice dv, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void) a;
    struct dpool *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->max_sets = ci->maxSets;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) {
        int j = pool_slot(p, ci->pPoolSizes[i].type);
        if (j < 0 && p->nsz < 16) { j = (int) p->nsz++; p->sz[j].type = ci->pPoolSizes[i].type; }
        if (j >= 0) p->sz[j].total += ci->pPoolSizes[i].descriptorCount;
    }
    AcquireSRWLockExclusive(&g_pools_lock);
    if (g_npools == g_cappools) {
        g_cappools = g_cappools ? g_cappools * 2 : 16;
        g_pools = realloc(g_pools, g_cappools * sizeof *g_pools);
    }
    g_pools[g_npools++] = p;
    ReleaseSRWLockExclusive(&g_pools_lock);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->maxSets);
    vr_u32(&b, ci->poolSizeCount);
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) { vr_u32(&b, ci->pPoolSizes[i].type); vr_u32(&b, ci->pPoolSizes[i].descriptorCount); }
    rpc_send(VR_CREATE_DESCRIPTOR_POOL, &b);
    free(b.p);
    *out = (VkDescriptorPool) p;
    return VK_SUCCESS;
}

static void free_sets_of(struct dpool *p) {
    AcquireSRWLockExclusive(&g_pools_lock);
    for (struct dset *s = p->head, *nx; s; s = nx) { nx = s->next; dsl_unref(s->layout); free(s->e); free(s); }
    p->head = NULL;
    p->used_sets = 0;
    for (uint32_t j = 0; j < p->nsz; j++) p->sz[j].used = 0;
    ReleaseSRWLockExclusive(&g_pools_lock);
}

static void VKAPI_CALL destroy_dpool(VkDevice dv, VkDescriptorPool pp, const VkAllocationCallbacks *a) {
    (void) a;
    if (!pp) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(pp));
    rpc_send(VR_DESTROY_DESCRIPTOR_POOL, &b);
    free(b.p);
    AcquireSRWLockExclusive(&g_pools_lock);
    for (size_t i = 0; i < g_npools; i++)
        if (g_pools[i] == (struct dpool *) pp) { g_pools[i] = g_pools[--g_npools]; break; }
    ReleaseSRWLockExclusive(&g_pools_lock);
    free_sets_of((struct dpool *) pp);
    free((struct dpool *) pp);
}

static VkResult VKAPI_CALL reset_dpool(VkDevice dv, VkDescriptorPool pp, VkDescriptorPoolResetFlags f) {
    (void) f;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(pp));
    rpc_send(VR_RESET_DESCRIPTOR_POOL, &b);
    free(b.p);
    free_sets_of((struct dpool *) pp);
    return VK_SUCCESS;
}

static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c);
static VkResult VKAPI_CALL alloc_dsets(VkDevice dv, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    struct dpool *p = (struct dpool *) ai->descriptorPool;
    // pool の残りが足りなければ、受け手に頼まずに「足りない」と返す（DXVK などは、それを見て新しい pool を作る）
    uint32_t need[16] = { 0 };
    int short_of = 0;
    AcquireSRWLockExclusive(&g_pools_lock);
    if (p->used_sets + ai->descriptorSetCount > p->max_sets) short_of = 1;
    for (uint32_t i = 0; i < ai->descriptorSetCount && !short_of; i++) {
        const struct dsl *l = (const struct dsl *) ai->pSetLayouts[i];
        for (uint32_t k = 0; k < l->nt; k++) {
            if (!l->t[k].count) continue;
            int j = pool_slot(p, l->t[k].type);
            if (j < 0) { short_of = 1; break; }
            need[j] += l->t[k].count;
        }
    }
    for (uint32_t j = 0; j < p->nsz && !short_of; j++) if (p->sz[j].used + need[j] > p->sz[j].total) short_of = 1;
    if (!short_of) {
        p->used_sets += ai->descriptorSetCount;
        for (uint32_t j = 0; j < p->nsz; j++) p->sz[j].used += need[j];
    }
    ReleaseSRWLockExclusive(&g_pools_lock);
    if (short_of) {
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++) out[i] = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    vr_u32(&b, ai->descriptorSetCount);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        struct dset *s = calloc(1, sizeof *s);
        s->pool = p;
        s->layout = (struct dsl *) ai->pSetLayouts[i];
        InterlockedIncrement(&s->layout->refs);
        AcquireSRWLockExclusive(&g_pools_lock);
        s->next = p->head;
        p->head = s;
        ReleaseSRWLockExclusive(&g_pools_lock);
        out[i] = (VkDescriptorSet) s;
        vr_u64(&b, ID(ai->pSetLayouts[i]));
        vr_u64(&b, ID(s));
    }
    rpc_send(VR_ALLOC_DESCRIPTOR_SETS, &b);
    free(b.p);
    // immutable sampler だけの binding（SAMPLER）は、アプリが書かない。受け手の set は空のままなので、ここで書いておく
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        struct dsl *l = ((struct dset *) out[i])->layout;
        for (uint32_t k = 0; k < l->nimm; k++) {
            if (l->imm[k].type != VK_DESCRIPTOR_TYPE_SAMPLER) continue;
            VkDescriptorImageInfo *ii = calloc(l->imm[k].count, sizeof *ii);
            if (!ii) continue;
            for (uint32_t e = 0; e < l->imm[k].count; e++) ii[e].sampler = l->imm[k].s[e];
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out[i], .dstBinding = l->imm[k].binding,
                                       .descriptorCount = l->imm[k].count, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = ii };
            update_dsets(dv, 1, &w, 0, NULL);
            free(ii);
        }
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL free_dsets(VkDevice dv, VkDescriptorPool pp, uint32_t n, const VkDescriptorSet *sets) {
    struct dpool *p = (struct dpool *) pp;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(pp));
    uint32_t real = 0;
    for (uint32_t i = 0; i < n; i++) if (sets[i]) real++;
    vr_u32(&b, real);
    for (uint32_t i = 0; i < n; i++) {
        if (!sets[i]) continue;
        vr_u64(&b, ID(sets[i]));
        struct dset *s = (struct dset *) sets[i], **pp2 = &p->head;
        AcquireSRWLockExclusive(&g_pools_lock);
        while (*pp2 && *pp2 != s) pp2 = &(*pp2)->next;
        if (*pp2) *pp2 = s->next;
        // 返した set の分を、pool の残りに戻す
        if (s->layout) {
            if (p->used_sets) p->used_sets--;
            for (uint32_t k = 0; k < s->layout->nt; k++) {
                int j = pool_slot(p, s->layout->t[k].type);
                if (j >= 0) p->sz[j].used = p->sz[j].used > s->layout->t[k].count ? p->sz[j].used - s->layout->t[k].count : 0;
            }
            dsl_unref(s->layout);
        }
        free(s->e);
        free(s);
        ReleaseSRWLockExclusive(&g_pools_lock);
    }
    rpc_send(VR_FREE_DESCRIPTOR_SETS, &b);
    free(b.p);
    return VK_SUCCESS;
}

static int is_image_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

static int is_buffer_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
}

static int is_texel_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

static void dset_put_full(struct dset *s, struct dent d) {
    for (uint32_t i = 0; i < s->n; i++)
        if (s->e[i].binding == d.binding && s->e[i].elem == d.elem) { s->e[i] = d; return; }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->e = realloc(s->e, s->cap * sizeof *s->e); }
    s->e[s->n++] = d;
}

static void dset_put(struct dset *s, uint32_t binding, uint32_t elem, uint32_t type, struct buf *bf, uint64_t off, uint64_t range) {
    dset_put_full(s, (struct dent) { binding, elem, type, bf, off, range, 0, 0, 0 });
}

static const struct dent *dset_get(const struct dset *s, uint32_t binding, uint32_t elem) {
    for (uint32_t i = 0; i < s->n; i++)
        if (s->e[i].binding == binding && s->e[i].elem == elem) return &s->e[i];
    return NULL;
}

static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c);

// descriptor の写しを、覚えている中身からふつうの書き込みに直す（受け手には写しを送らない）
static void copy_dsets(VkDevice dv, uint32_t nc, const VkCopyDescriptorSet *c) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < nc; i++) total += c[i].descriptorCount;
    if (!total) return;
    VkWriteDescriptorSet *w = calloc(total, sizeof *w);
    VkDescriptorImageInfo *ii = calloc(total, sizeof *ii);
    VkDescriptorBufferInfo *bi = calloc(total, sizeof *bi);
    VkBufferView *tv = calloc(total, sizeof *tv);
    if (!w || !ii || !bi || !tv) { free(w); free(ii); free(bi); free(tv); return; }
    uint32_t n = 0, missing = 0;
    for (uint32_t i = 0; i < nc; i++) {
        const struct dset *src = (const struct dset *) c[i].srcSet;
        for (uint32_t k = 0; k < c[i].descriptorCount; k++) {
            const struct dent *e = src ? dset_get(src, c[i].srcBinding, c[i].srcArrayElement + k) : NULL;
            if (!e) { missing++; continue; }
            w[n] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c[i].dstSet, .dstBinding = c[i].dstBinding,
                                            .dstArrayElement = c[i].dstArrayElement + k, .descriptorCount = 1, .descriptorType = (VkDescriptorType) e->type };
            if (is_image_desc(e->type)) {
                ii[n] = (VkDescriptorImageInfo) { (VkSampler) (uintptr_t) e->h0, (VkImageView) (uintptr_t) e->h1, (VkImageLayout) e->layout };
                w[n].pImageInfo = &ii[n];
            } else if (is_texel_desc(e->type)) {
                tv[n] = (VkBufferView) (uintptr_t) e->h0;
                w[n].pTexelBufferView = &tv[n];
            } else if (is_buffer_desc(e->type)) {
                if (!e->b) { missing++; continue; } // 消えたバッファ
                bi[n] = (VkDescriptorBufferInfo) { (VkBuffer) e->b, e->off, e->range };
                w[n].pBufferInfo = &bi[n];
            } else {
                missing++;
                continue;
            }
            n++;
        }
    }
    if (missing) say("descriptor の写し：中身が分からない %u 個を飛ばした", missing);
    if (n) update_dsets(dv, n, w, 0, NULL);
    free(w); free(ii); free(bi); free(tv);
}

static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    uint32_t real = 0;
#define KNOWN_DESC(t) (is_buffer_desc(t) || is_image_desc(t) || (is_texel_desc(t) && (g_hw.caps & VR_CAP_D3D)))
    for (uint32_t i = 0; i < nw; i++) if (KNOWN_DESC(w[i].descriptorType)) real++;
    vr_u32(&b, real);
    for (uint32_t i = 0; i < nw; i++) {
        if (!KNOWN_DESC(w[i].descriptorType)) { say("この種類の descriptor（%d）はまだ扱わない", w[i].descriptorType); continue; }
        struct dset *s = (struct dset *) w[i].dstSet;
        vr_u64(&b, ID(s));
        vr_u32(&b, w[i].dstBinding);
        vr_u32(&b, w[i].dstArrayElement);
        vr_u32(&b, w[i].descriptorType);
        vr_u32(&b, w[i].descriptorCount);
        if (is_image_desc(w[i].descriptorType)) {
            for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
                const VkDescriptorImageInfo *ii = &w[i].pImageInfo[k];
                // 形に決まった sampler があれば、それが正（アプリが書いた sampler は無視される決まり。DXVK は VK_NULL_HANDLE を書く）
                VkSampler imm = dsl_imm(s ? s->layout : NULL, w[i].dstBinding, w[i].dstArrayElement + k);
                uint64_t smp = w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? ID(imm ? imm : ii->sampler) : 0;
                uint64_t view = w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ? 0 : ID(ii->imageView);
                vr_u64(&b, smp);
                vr_u64(&b, view);
                vr_u32(&b, fix_layout(ii->imageLayout));
                dset_put_full(s, (struct dent) { w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, NULL, 0, 0, smp, view, (uint32_t) ii->imageLayout });
            }
            continue;
        }
        if (is_texel_desc(w[i].descriptorType)) {
            for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
                struct bview *v = (struct bview *) w[i].pTexelBufferView[k];
                vr_u64(&b, ID(v));
                if (v) dset_put_full(s, (struct dent) { w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, v->b, v->off, v->range, ID(v), 0, 0 });
            }
            continue;
        }
        for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
            const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[k];
            struct buf *bf = (struct buf *) bi->buffer;
            uint64_t range = bi->range == VK_WHOLE_SIZE && bf ? bf->size - bi->offset : bi->range;
            vr_u64(&b, ID(bi->buffer));
            vr_u64(&b, bi->offset);
            vr_u64(&b, bi->range);
            dset_put(s, w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, bf, bi->offset, range);
        }
    }
    rpc_send(VR_UPDATE_DESCRIPTOR_SETS, &b);
    free(b.p);
    // 写しは書き込みのあとに行う（vkUpdateDescriptorSets の決まり）
    if (nc) copy_dsets(dv, nc, c);
}

// ---- push descriptor（VK_KHR_push_descriptor） ----
// 受け手には知らせない。push のたびに、隠しの pool から取った set に書いて、その場でつなぐ。
// 使った set は、その command buffer を記録し直すか消すまで持つ（同じ記録を何度 submit しても中身が変わらない）。
// そのあと同じ形の push に使い回す。使っている仕事があれば、書き換えの前に受け手が待つ
struct plinfo { uint64_t id; uint32_t n; struct dsl *l[32]; };
static struct plinfo *g_pl;
static size_t g_npl, g_cappl;
static SRWLOCK g_pl_lock = SRWLOCK_INIT;

static void pl_note(uint64_t id, const VkPipelineLayoutCreateInfo *ci) {
    int any = 0;
    for (uint32_t i = 0; i < ci->setLayoutCount; i++)
        if (ci->pSetLayouts[i] && ((struct dsl *) ci->pSetLayouts[i])->push) any = 1;
    if (!any) return;
    struct plinfo e = { .id = id, .n = ci->setLayoutCount < 32 ? ci->setLayoutCount : 32 };
    for (uint32_t i = 0; i < e.n; i++) {
        e.l[i] = (struct dsl *) ci->pSetLayouts[i];
        if (e.l[i]) InterlockedIncrement(&e.l[i]->refs);
    }
    AcquireSRWLockExclusive(&g_pl_lock);
    if (g_npl == g_cappl) {
        g_cappl = g_cappl ? g_cappl * 2 : 16;
        g_pl = realloc(g_pl, g_cappl * sizeof *g_pl);
    }
    g_pl[g_npl++] = e;
    ReleaseSRWLockExclusive(&g_pl_lock);
}

static struct dsl *pl_set_layout(uint64_t id, uint32_t set) {
    struct dsl *l = NULL;
    AcquireSRWLockShared(&g_pl_lock);
    for (size_t i = 0; i < g_npl; i++)
        if (g_pl[i].id == id) { l = set < g_pl[i].n ? g_pl[i].l[set] : NULL; break; }
    ReleaseSRWLockShared(&g_pl_lock);
    return l;
}

static void pl_forget(uint64_t id) {
    struct plinfo e = { 0 };
    AcquireSRWLockExclusive(&g_pl_lock);
    for (size_t i = 0; i < g_npl; i++)
        if (g_pl[i].id == id) { e = g_pl[i]; g_pl[i] = g_pl[--g_npl]; break; }
    ReleaseSRWLockExclusive(&g_pl_lock);
    for (uint32_t i = 0; i < e.n; i++) if (e.l[i]) dsl_unref(e.l[i]);
}

static SRWLOCK g_push_lock = SRWLOCK_INIT;
static VkDescriptorPool g_push_pool;
static VkDevice g_push_dev;

static VkResult VKAPI_CALL alloc_dsets(VkDevice dv, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out);
static VkResult VKAPI_CALL create_dpool(VkDevice dv, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out);

// push 用の set を 1 つ取る（使い回せる物があればそれ、無ければ隠しの pool から。pool がいっぱいなら新しく作る）
static VkDescriptorSet push_take(VkDevice dv, struct dsl *l) {
    VkDescriptorSet s = VK_NULL_HANDLE;
    AcquireSRWLockExclusive(&g_pools_lock);
    if (l->nfree) s = l->free[--l->nfree];
    ReleaseSRWLockExclusive(&g_pools_lock);
    if (s) return s;
    AcquireSRWLockExclusive(&g_push_lock);
    for (int tries = 0; tries < 2 && !s; tries++) {
        if (!g_push_pool || g_push_dev != dv) {
            VkDescriptorPoolSize ps[11];
            for (uint32_t t = 0; t < 11; t++) ps[t] = (VkDescriptorPoolSize) { (VkDescriptorType) t, 2048 };
            VkDescriptorPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256, .poolSizeCount = 11, .pPoolSizes = ps };
            if (create_dpool(dv, &pc, NULL, &g_push_pool) != VK_SUCCESS) break;
            g_push_dev = dv;
            say("push descriptor 用の pool を作った");
        }
        VkDescriptorSetLayout lh = (VkDescriptorSetLayout) l;
        VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_push_pool,
                                           .descriptorSetCount = 1, .pSetLayouts = &lh };
        if (alloc_dsets(dv, &ai, &s) != VK_SUCCESS) { s = VK_NULL_HANDLE; g_push_pool = VK_NULL_HANDLE; }
    }
    ReleaseSRWLockExclusive(&g_push_lock);
    return s;
}

static void push_hold(struct cb *c, struct dsl *l, VkDescriptorSet s) {
    if (c->nph == c->cph) {
        size_t cap = c->cph ? c->cph * 2 : 8;
        struct pushheld *p = realloc(c->ph, cap * sizeof *p);
        if (!p) return;
        c->ph = p;
        c->cph = cap;
    }
    c->ph[c->nph++] = (struct pushheld) { l, s };
}

static void push_release(struct cb *c) {
    if (!c->nph) return;
    AcquireSRWLockExclusive(&g_pools_lock);
    for (size_t i = 0; i < c->nph; i++) {
        struct dsl *l = c->ph[i].l;
        if (l->nfree == l->capfree) {
            uint32_t cap = l->capfree ? l->capfree * 2 : 8;
            VkDescriptorSet *p = realloc(l->free, cap * sizeof *p);
            if (!p) continue;
            l->free = p;
            l->capfree = cap;
        }
        l->free[l->nfree++] = c->ph[i].s;
    }
    c->nph = 0;
    ReleaseSRWLockExclusive(&g_pools_lock);
}

static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c);
static void VKAPI_CALL cmd_bind_sets(VkCommandBuffer cbh, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn);

// push に使う set を取って持つ。取れなければ NULL
static VkDescriptorSet push_set_for(struct cb *c, VkPipelineLayout layout, uint32_t set) {
    struct dsl *l = pl_set_layout(ID(layout), set);
    if (!l) { say("push descriptor：pipeline layout の %u 番の形が分からない", set); return VK_NULL_HANDLE; }
    VkDescriptorSet s = push_take((VkDevice) c->d, l);
    if (!s) { say("push descriptor の set を取れない"); return VK_NULL_HANDLE; }
    push_hold(c, l, s);
    return s;
}

static void VKAPI_CALL cmd_push_desc(VkCommandBuffer cbh, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t nw,
                                     const VkWriteDescriptorSet *w) {
    struct cb *c = (struct cb *) cbh;
    VkDescriptorSet s = push_set_for(c, layout, set);
    if (!s) return;
    VkWriteDescriptorSet *ww = malloc((nw ? nw : 1) * sizeof *ww);
    if (!ww) return;
    for (uint32_t i = 0; i < nw; i++) { ww[i] = w[i]; ww[i].dstSet = s; }
    update_dsets((VkDevice) c->d, nw, ww, 0, NULL);
    free(ww);
    cmd_bind_sets(cbh, bp, layout, set, 1, &s, 0, NULL);
}

// ---- descriptor の書き込みの型紙（update template） ----
// 受け手には送らない。使うたびに、型紙と中身から普通の書き込み（update_dsets）を組み立てる
struct dtmpl { uint32_t n; VkDescriptorUpdateTemplateEntry *e; VkPipelineBindPoint bp; };

static VkResult VKAPI_CALL create_dtmpl(VkDevice dv, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a,
                                        VkDescriptorUpdateTemplate *out) {
    (void) dv; (void) a;
    struct dtmpl *t = calloc(1, sizeof *t);
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->n = ci->descriptorUpdateEntryCount;
    t->bp = ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR ? ci->pipelineBindPoint : VK_PIPELINE_BIND_POINT_GRAPHICS;
    t->e = malloc((t->n ? t->n : 1) * sizeof *t->e);
    if (t->n) memcpy(t->e, ci->pDescriptorUpdateEntries, t->n * sizeof *t->e);
    *out = (VkDescriptorUpdateTemplate) t;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_dtmpl(VkDevice dv, VkDescriptorUpdateTemplate h, const VkAllocationCallbacks *a) {
    (void) dv; (void) a;
    struct dtmpl *t = (struct dtmpl *) h;
    if (!t) return;
    free(t->e);
    free(t);
}

static void VKAPI_CALL update_dset_tmpl(VkDevice dv, VkDescriptorSet set, VkDescriptorUpdateTemplate h, const void *data);

// 型紙で push する。型紙の中身の並びはふつうの型紙と同じなので、push 用の set に書いてつなぐ
static void VKAPI_CALL cmd_push_desc_tmpl(VkCommandBuffer cbh, VkDescriptorUpdateTemplate h, VkPipelineLayout layout, uint32_t set, const void *data) {
    struct cb *c = (struct cb *) cbh;
    struct dtmpl *t = (struct dtmpl *) h;
    if (!t) return;
    VkDescriptorSet s = push_set_for(c, layout, set);
    if (!s) return;
    update_dset_tmpl((VkDevice) c->d, s, h, data);
    cmd_bind_sets(cbh, t->bp, layout, set, 1, &s, 0, NULL);
}

static void VKAPI_CALL cmd_push_desc2(VkCommandBuffer cbh, const VkPushDescriptorSetInfoKHR *i) {
    if (i->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        cmd_push_desc(cbh, VK_PIPELINE_BIND_POINT_GRAPHICS, i->layout, i->set, i->descriptorWriteCount, i->pDescriptorWrites);
    if (i->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        cmd_push_desc(cbh, VK_PIPELINE_BIND_POINT_COMPUTE, i->layout, i->set, i->descriptorWriteCount, i->pDescriptorWrites);
}

static void VKAPI_CALL cmd_push_desc_tmpl2(VkCommandBuffer cbh, const VkPushDescriptorSetWithTemplateInfoKHR *i) {
    cmd_push_desc_tmpl(cbh, i->descriptorUpdateTemplate, i->layout, i->set, i->pData);
}

static void VKAPI_CALL update_dset_tmpl(VkDevice dv, VkDescriptorSet set, VkDescriptorUpdateTemplate h, const void *data) {
    struct dtmpl *t = (struct dtmpl *) h;
    if (!t || !t->n) return;
    VkWriteDescriptorSet *w = calloc(t->n, sizeof *w);
    void **keep = calloc(t->n, sizeof *keep);
    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->e[i];
        const char *p = (const char *) data + e->offset;
        uint32_t cnt = e->descriptorCount;
        w[i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = e->dstBinding,
                                        .dstArrayElement = e->dstArrayElement, .descriptorCount = cnt, .descriptorType = e->descriptorType };
        if (is_image_desc(e->descriptorType)) {
            VkDescriptorImageInfo *ii = calloc(cnt ? cnt : 1, sizeof *ii);
            for (uint32_t k = 0; k < cnt; k++) memcpy(&ii[k], p + k * e->stride, sizeof *ii);
            w[i].pImageInfo = ii;
            keep[i] = ii;
        } else if (is_buffer_desc(e->descriptorType)) {
            VkDescriptorBufferInfo *bi = calloc(cnt ? cnt : 1, sizeof *bi);
            for (uint32_t k = 0; k < cnt; k++) memcpy(&bi[k], p + k * e->stride, sizeof *bi);
            w[i].pBufferInfo = bi;
            keep[i] = bi;
        } else if (is_texel_desc(e->descriptorType)) {
            VkBufferView *tv = calloc(cnt ? cnt : 1, sizeof *tv);
            for (uint32_t k = 0; k < cnt; k++) memcpy(&tv[k], p + k * e->stride, sizeof *tv);
            w[i].pTexelBufferView = tv;
            keep[i] = tv;
        }
    }
    update_dsets(dv, t->n, w, 0, NULL);
    for (uint32_t i = 0; i < t->n; i++) free(keep[i]);
    free(keep);
    free(w);
}

// ---- command buffer ----
static VkResult VKAPI_CALL create_cpool(VkDevice dv, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void) a;
    struct cpool *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    p->fam = ci->queueFamilyIndex;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->queueFamilyIndex);
    rpc_send(VR_CREATE_COMMAND_POOL, &b);
    free(b.p);
    *out = (VkCommandPool) p;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_buffer(VkDevice dv, VkBuffer h, const VkAllocationCallbacks *a);
static void VKAPI_CALL free_memory(VkDevice dv, VkDeviceMemory h, const VkAllocationCallbacks *a);

static void bct_free(struct cb *c) {
    for (struct bctmp *t = c->bct, *nx; t; t = nx) {
        nx = t->next;
        destroy_buffer((VkDevice) c->d, t->b, NULL);
        free_memory((VkDevice) c->d, t->m, NULL);
        free(t);
    }
    c->bct = NULL;
}

static void cb_clear(struct cb *c) { push_release(c); c->s.n = 0; c->nr = 0; c->nrd = 0; c->nev = 0; bct_free(c); c->gs = NULL; c->app_topo_set = 0; gs_cb_clear(c); }

static void cb_free(struct cb *c) { push_release(c); free(c->ph); gs_cb_free(c); bct_free(c); free(c->s.p); free(c->r); free(c->rd); free(c->ev); free(c); }

static void VKAPI_CALL destroy_cpool(VkDevice dv, VkCommandPool pp, const VkAllocationCallbacks *a) {
    (void) a;
    if (!pp) return;
    struct cpool *p = (struct cpool *) pp;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    rpc_send(VR_DESTROY_COMMAND_POOL, &b);
    free(b.p);
    for (struct cb *c = p->head, *nx; c; c = nx) { nx = c->next; cb_free(c); }
    free(p);
}

static VkResult VKAPI_CALL reset_cpool(VkDevice dv, VkCommandPool pp, VkCommandPoolResetFlags f) {
    (void) f;
    struct cpool *p = (struct cpool *) pp;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    rpc_send(VR_RESET_COMMAND_POOL, &b);
    free(b.p);
    for (struct cb *c = p->head; c; c = c->next) cb_clear(c);
    return VK_SUCCESS;
}

static void VKAPI_CALL trim_cpool(VkDevice dv, VkCommandPool p, VkCommandPoolTrimFlags f) { (void) dv; (void) p; (void) f; }

static VkResult VKAPI_CALL alloc_cbs(VkDevice dv, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    struct cpool *p = (struct cpool *) ai->commandPool;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    vr_u32(&b, ai->level);
    vr_u32(&b, ai->commandBufferCount);
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        struct cb *c = calloc(1, sizeof *c);
        set_loader_magic_value(c);
        c->d = (struct dev *) dv;
        c->pool = p;
        c->level = ai->level;
        c->next = p->head;
        p->head = c;
        out[i] = (VkCommandBuffer) c;
        vr_u64(&b, ID(c));
    }
    rpc_send(VR_ALLOC_COMMAND_BUFFERS, &b);
    free(b.p);
    return VK_SUCCESS;
}

static void VKAPI_CALL free_cbs(VkDevice dv, VkCommandPool pp, uint32_t n, const VkCommandBuffer *cbs) {
    struct cpool *p = (struct cpool *) pp;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(pp));
    uint32_t real = 0;
    for (uint32_t i = 0; i < n; i++) if (cbs[i]) real++;
    vr_u32(&b, real);
    for (uint32_t i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        struct cb *c = (struct cb *) cbs[i], **pp2 = &p->head;
        vr_u64(&b, ID(c));
        while (*pp2 && *pp2 != c) pp2 = &(*pp2)->next;
        if (*pp2) *pp2 = c->next;
        cb_free(c);
    }
    rpc_send(VR_FREE_COMMAND_BUFFERS, &b);
    free(b.p);
}

static VkResult VKAPI_CALL begin_cb(VkCommandBuffer cbh, const VkCommandBufferBeginInfo *bi) {
    struct cb *c = (struct cb *) cbh;
    cb_clear(c);
    c->flags = bi->flags;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL end_cb(VkCommandBuffer cbh) {
    struct cb *c = (struct cb *) cbh;
    rng_merge(c->r, &c->nr);
    rng_merge(c->rd, &c->nrd);
    // 2 次の command buffer は、呼んだ側（1 次）に中身を写して送るので、ここでは送らない
    if (c->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) return VK_SUCCESS;
    vr_buf b = { 0 };
    vr_u64(&b, ID(c));
    vr_u32(&b, c->flags);
    vr_bytes(&b, c->s.p, (uint32_t) c->s.n);
    rpc_send(VR_RECORD_COMMAND_BUFFER, &b);
    free(b.p);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL reset_cb(VkCommandBuffer cbh, VkCommandBufferResetFlags f) {
    (void) f;
    struct cb *c = (struct cb *) cbh;
    cb_clear(c);
    vr_buf b = { 0 };
    vr_u64(&b, ID(c));
    rpc_send(VR_RESET_COMMAND_BUFFER, &b);
    free(b.p);
    return VK_SUCCESS;
}

// GPU が書くかもしれない範囲として覚える
static void cb_writes(struct cb *c, struct buf *bf, uint64_t off, uint64_t len) {
    if (!bf || !bf->m || !bf->m->host) return;
    if (len == VK_WHOLE_SIZE) len = off < bf->size ? bf->size - off : 0;
    rng_add(&c->r, &c->nr, &c->capr, bf->m, bf->memoff + off, len, 0);
}

// GPU が読むかもしれない範囲として覚える
static void cb_reads(struct cb *c, struct buf *bf, uint64_t off, uint64_t len) {
    if (!bf || !bf->m || !bf->m->host) return;
    if (len == VK_WHOLE_SIZE) len = off < bf->size ? bf->size - off : 0;
    rng_add(&c->rd, &c->nrd, &c->caprd, bf->m, bf->memoff + off, len, 0);
}

static void VKAPI_CALL cmd_bind_pipeline(VkCommandBuffer cbh, VkPipelineBindPoint bp, VkPipeline p) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_PIPELINE);
    vr_u32(&c->s, bp);
    vr_u64(&c->s, ID(p));
    gs_bind(c, bp, p);
}

static void VKAPI_CALL cmd_bind_sets(VkCommandBuffer cbh, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_DESCRIPTOR_SETS);
    vr_u32(&c->s, bp);
    vr_u64(&c->s, ID(l));
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    for (uint32_t i = 0; i < n; i++) vr_u64(&c->s, ID(sets[i]));
    vr_u32(&c->s, nd);
    for (uint32_t i = 0; i < nd; i++) vr_u32(&c->s, dyn[i]);
    gs_track_sets(c, bp, l, first, n, sets, nd, dyn);
    // storage buffer は GPU が書くかもしれない
    for (uint32_t i = 0; i < n; i++) {
        struct dset *s = (struct dset *) sets[i];
        if (!s) continue;
        for (uint32_t k = 0; k < s->n; k++) {
            struct dent *e = &s->e[k];
            if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || e->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) cb_writes(c, e->b, e->off, e->range);
            else if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC && e->b) cb_writes(c, e->b, 0, e->b->size);
            if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || e->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) cb_reads(c, e->b, e->off, e->range);
            else if (e->b) cb_reads(c, e->b, 0, e->b->size);
        }
    }
}

static void VKAPI_CALL cmd_push(VkCommandBuffer cbh, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_PUSH_CONSTANTS);
    vr_u64(&c->s, ID(l));
    vr_u32(&c->s, gs_stages(st));
    vr_u32(&c->s, off);
    vr_bytes(&c->s, v, size);
}

static void VKAPI_CALL cmd_dispatch(VkCommandBuffer cbh, uint32_t x, uint32_t y, uint32_t z) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_DISPATCH);
    vr_u32(&c->s, x);
    vr_u32(&c->s, y);
    vr_u32(&c->s, z);
}

static void VKAPI_CALL cmd_dispatch_base(VkCommandBuffer cbh, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_DISPATCH_BASE);
    vr_u32(&c->s, bx);
    vr_u32(&c->s, by);
    vr_u32(&c->s, bz);
    vr_u32(&c->s, x);
    vr_u32(&c->s, y);
    vr_u32(&c->s, z);
}

static void VKAPI_CALL cmd_dispatch_indirect(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_DISPATCH_INDIRECT);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, off);
    cb_reads(c, (struct buf *) bb, off, sizeof(VkDispatchIndirectCommand));
}

static void VKAPI_CALL cmd_copy(VkCommandBuffer cbh, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_COPY_BUFFER);
    vr_u64(&c->s, ID(src));
    vr_u64(&c->s, ID(dst));
    vr_u32(&c->s, n);
    for (uint32_t i = 0; i < n; i++) {
        vr_u64(&c->s, r[i].srcOffset);
        vr_u64(&c->s, r[i].dstOffset);
        vr_u64(&c->s, r[i].size);
        cb_writes(c, (struct buf *) dst, r[i].dstOffset, r[i].size);
        cb_reads(c, (struct buf *) src, r[i].srcOffset, r[i].size);
    }
}

static void VKAPI_CALL cmd_fill(VkCommandBuffer cbh, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_FILL_BUFFER);
    vr_u64(&c->s, ID(dst));
    vr_u64(&c->s, off);
    vr_u64(&c->s, size);
    vr_u32(&c->s, data);
    cb_writes(c, (struct buf *) dst, off, size);
}

static void VKAPI_CALL cmd_update(VkCommandBuffer cbh, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void *data) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_UPDATE_BUFFER);
    vr_u64(&c->s, ID(dst));
    vr_u64(&c->s, off);
    vr_bytes(&c->s, data, (uint32_t) size);
    cb_writes(c, (struct buf *) dst, off, size);
}

static void VKAPI_CALL cmd_barrier(VkCommandBuffer cbh, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                   uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, ni ? VR_CMD_PIPELINE_BARRIER2 : VR_CMD_PIPELINE_BARRIER);
    vr_u32(&c->s, src);
    vr_u32(&c->s, dst);
    vr_u32(&c->s, dep);
    if (nm > 16) nm = 16;
    vr_u32(&c->s, nm);
    for (uint32_t i = 0; i < nm; i++) { vr_u32(&c->s, mb[i].srcAccessMask); vr_u32(&c->s, mb[i].dstAccessMask); }
    vr_u32(&c->s, nb);
    for (uint32_t i = 0; i < nb; i++) {
        vr_u32(&c->s, bb[i].srcAccessMask);
        vr_u32(&c->s, bb[i].dstAccessMask);
        vr_u32(&c->s, bb[i].srcQueueFamilyIndex);
        vr_u32(&c->s, bb[i].dstQueueFamilyIndex);
        vr_u64(&c->s, ID(bb[i].buffer));
        vr_u64(&c->s, bb[i].offset);
        vr_u64(&c->s, bb[i].size);
    }
    if (!ni) return;
    vr_u32(&c->s, ni);
    for (uint32_t i = 0; i < ni; i++) {
        vr_u32(&c->s, ib[i].srcAccessMask);
        vr_u32(&c->s, ib[i].dstAccessMask);
        vr_u32(&c->s, fix_layout(ib[i].oldLayout));
        vr_u32(&c->s, fix_layout(ib[i].newLayout));
        vr_u32(&c->s, ib[i].srcQueueFamilyIndex);
        vr_u32(&c->s, ib[i].dstQueueFamilyIndex);
        vr_u64(&c->s, ID(ib[i].image));
        vr_put(&c->s, &ib[i].subresourceRange, sizeof ib[i].subresourceRange);
    }
}

// ---- 描画の命令 ----
static void VKAPI_CALL cmd_begin_rp(VkCommandBuffer cbh, const VkRenderPassBeginInfo *bi, VkSubpassContents contents) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BEGIN_RENDER_PASS);
    vr_u64(&c->s, ID(bi->renderPass));
    vr_u64(&c->s, ID(bi->framebuffer));
    vr_put(&c->s, &bi->renderArea, sizeof bi->renderArea);
    vr_u32(&c->s, bi->clearValueCount);
    vr_put(&c->s, bi->pClearValues, bi->clearValueCount * sizeof *bi->pClearValues);
    // 2 次の command buffer の中身はここに写して送るので、受け手では常に INLINE
    (void) contents;
    vr_u32(&c->s, VK_SUBPASS_CONTENTS_INLINE);
}

static void VKAPI_CALL cmd_end_rp(VkCommandBuffer cbh) { vr_u32(&((struct cb *) cbh)->s, VR_CMD_END_RENDER_PASS); }

static void VKAPI_CALL cmd_next_subpass(VkCommandBuffer cbh, VkSubpassContents contents) {
    struct cb *c = (struct cb *) cbh;
    (void) contents;
    vr_u32(&c->s, VR_CMD_NEXT_SUBPASS);
    vr_u32(&c->s, VK_SUBPASS_CONTENTS_INLINE);
}

// ---- render pass 2 の記録（中身は 1.0 の命令と同じ） ----
static void VKAPI_CALL cmd_begin_rp2(VkCommandBuffer cbh, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sb) {
    cmd_begin_rp(cbh, bi, sb ? sb->contents : VK_SUBPASS_CONTENTS_INLINE);
}
static void VKAPI_CALL cmd_next_subpass2(VkCommandBuffer cbh, const VkSubpassBeginInfo *sb, const VkSubpassEndInfo *se) {
    (void) se;
    cmd_next_subpass(cbh, sb ? sb->contents : VK_SUBPASS_CONTENTS_INLINE);
}
static void VKAPI_CALL cmd_end_rp2(VkCommandBuffer cbh, const VkSubpassEndInfo *se) { (void) se; cmd_end_rp(cbh); }

// ---- 2 次の command buffer：中身を呼んだ側に写す ----
// 受け手では 2 次の command buffer を使わない（記録に引き継ぎの情報が要り、render pass の中身の扱いも変わるため）。
// 2 次の記録は呼ばれるたびに 1 次の記録へ写すので、GPU が読み書きする範囲も一緒に写す
static void ev_add(struct cb *c, struct event *e, int val);
static void VKAPI_CALL cmd_exec(VkCommandBuffer cbh, uint32_t n, const VkCommandBuffer *sec) {
    struct cb *c = (struct cb *) cbh;
    for (uint32_t i = 0; i < n; i++) {
        struct cb *s = (struct cb *) sec[i];
        if (!s) continue;
        vr_put(&c->s, s->s.p, s->s.n);
        for (size_t k = 0; k < s->nr; k++) rng_add(&c->r, &c->nr, &c->capr, s->r[k].m, s->r[k].off, s->r[k].len, 0);
        for (size_t k = 0; k < s->nrd; k++) rng_add(&c->rd, &c->nrd, &c->caprd, s->rd[k].m, s->rd[k].off, s->rd[k].len, 0);
        for (size_t k = 0; k < s->nev; k++) ev_add(c, s->ev[k].e, s->ev[k].val);
    }
}

static void VKAPI_CALL cmd_draw(VkCommandBuffer cbh, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    if (gs_draw(c, vc, ic, fv, fi)) return;
    vr_u32(&c->s, VR_CMD_DRAW);
    vr_u32(&c->s, vc);
    vr_u32(&c->s, ic);
    vr_u32(&c->s, fv);
    vr_u32(&c->s, fi);
}

static void VKAPI_CALL cmd_draw_indexed(VkCommandBuffer cbh, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    if (gs_draw_indexed(c, n, ic, first, vo, fi)) return;
    vr_u32(&c->s, VR_CMD_DRAW_INDEXED);
    vr_u32(&c->s, n);
    vr_u32(&c->s, ic);
    vr_u32(&c->s, first);
    vr_u32(&c->s, (uint32_t) vo);
    vr_u32(&c->s, fi);
}

static void draw_indirect(VkCommandBuffer cbh, uint32_t op, VkBuffer bb, VkDeviceSize off, uint32_t n, uint32_t stride) {
    struct cb *c = (struct cb *) cbh;
    if (gs_skip(c, "間接")) return;
    vr_u32(&c->s, op);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, off);
    vr_u32(&c->s, n);
    vr_u32(&c->s, stride);
    cb_reads(c, (struct buf *) bb, off, (uint64_t) n * (stride ? stride : 16));
}

static void VKAPI_CALL cmd_draw_indirect(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, uint32_t n, uint32_t stride) {
    draw_indirect(cbh, VR_CMD_DRAW_INDIRECT, bb, off, n, stride);
}

static void VKAPI_CALL cmd_draw_indexed_indirect(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, uint32_t n, uint32_t stride) {
    draw_indirect(cbh, VR_CMD_DRAW_INDEXED_INDIRECT, bb, off, n, stride);
}

static void VKAPI_CALL cmd_bind_vbs(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkBuffer *bs, const VkDeviceSize *offs) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_VERTEX_BUFFERS);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    for (uint32_t i = 0; i < n; i++) {
        vr_u64(&c->s, ID(bs[i]));
        vr_u64(&c->s, offs[i]);
        cb_reads(c, (struct buf *) bs[i], offs[i], VK_WHOLE_SIZE);
    }
    gs_track_vb(c, first, n, bs, offs, NULL);
}

static void VKAPI_CALL cmd_bind_ib(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, VkIndexType t) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_INDEX_BUFFER);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, off);
    vr_u32(&c->s, t);
    cb_reads(c, (struct buf *) bb, off, VK_WHOLE_SIZE);
    gs_track_ib(c, bb, off, t);
}

static void VKAPI_CALL cmd_set_viewport(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkViewport *v) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_SET_VIEWPORT);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_put(&c->s, v, n * sizeof *v);
}

static void VKAPI_CALL cmd_set_scissor(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkRect2D *v) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_SET_SCISSOR);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_put(&c->s, v, n * sizeof *v);
}

static void VKAPI_CALL cmd_copy_image_to_buffer(VkCommandBuffer cbh, VkImage im, VkImageLayout l, VkBuffer bb, uint32_t n, const VkBufferImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
    if (im && ((struct img *) im)->bc) { say("BC：ほどいた画像を BC のままバッファへ読み出すことはまだできない（飛ばした）"); return; }
    vr_u32(&c->s, VR_CMD_COPY_IMAGE_TO_BUFFER);
    vr_u64(&c->s, ID(im));
    vr_u64(&c->s, ID(bb));
    vr_u32(&c->s, fix_layout(l));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
    cb_writes(c, (struct buf *) bb, 0, VK_WHOLE_SIZE);
}

// BC の写す範囲 1 つを、ほどいたあとの大きさ（ふつうの画素で詰めて並べる）
static uint64_t bc_region_size(VkFormat f, const VkBufferImageCopy *r) {
    uint64_t dz = r->imageExtent.depth ? r->imageExtent.depth : 1, layers = r->imageSubresource.layerCount ? r->imageSubresource.layerCount : 1;
    return (uint64_t) r->imageExtent.width * r->imageExtent.height * dz * layers * texel_bytes(bc_sub(f));
}

// BC の中身（CPU から見えるバッファの写し）をほどいて、dst に詰めて並べる
static void bc_decode_region(const struct buf *src, VkFormat f, const VkBufferImageCopy *r, uint8_t *dst) {
    uint32_t tb = texel_bytes(bc_sub(f)), bb = bc_block_bytes(f);
    uint32_t w = r->imageExtent.width, h = r->imageExtent.height, dz = r->imageExtent.depth ? r->imageExtent.depth : 1;
    uint32_t layers = r->imageSubresource.layerCount ? r->imageSubresource.layerCount : 1;
    uint32_t rowlen = r->bufferRowLength ? r->bufferRowLength : w, imgh = r->bufferImageHeight ? r->bufferImageHeight : h;
    uint32_t bw = (rowlen + 3) / 4, bh = (imgh + 3) / 4;
    uint64_t slice = (uint64_t) bw * bh * bb;
    const uint8_t *base = src->m->shadow + src->memoff + r->bufferOffset, *end = src->m->shadow + src->m->size;
    uint8_t tile[4 * 4 * 8];
    for (uint32_t ly = 0; ly < layers; ly++)
        for (uint32_t z = 0; z < dz; z++) {
            const uint8_t *sl = base + ((uint64_t) ly * dz + z) * slice;
            uint8_t *dsl = dst + ((uint64_t) ly * dz + z) * w * h * tb;
            for (uint32_t by = 0; by < (h + 3) / 4; by++)
                for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                    const uint8_t *blk = sl + ((uint64_t) by * bw + bx) * bb;
                    if (blk + bb > end) continue; // アプリの中身が足りない
                    bc_block(f, blk, tile, tb);
                    uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4;
                    for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                        memcpy(dsl + ((uint64_t) (by * 4 + y) * w + bx * 4) * tb, tile + y * 4 * tb, (size_t) cw * tb);
                }
        }
}

// ほどいた中身を入れる一時バッファ（CPU から見えるメモリ）。command buffer を記録し直すときに消す
static VkBuffer bc_tmp(struct cb *c, uint64_t size, uint8_t **ptr) {
    VkDevice dv = (VkDevice) c->d;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer tmp;
    if (create_buffer(dv, &bc, NULL, &tmp) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    buffer_reqs(dv, tmp, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = pick_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    if (alloc_memory(dv, &mai, NULL, &mem) != VK_SUCCESS) { destroy_buffer(dv, tmp, NULL); return VK_NULL_HANDLE; }
    bind_buffer(dv, tmp, mem, 0);
    map_memory(dv, mem, 0, VK_WHOLE_SIZE, 0, (void **) ptr);
    struct bctmp *t = calloc(1, sizeof *t);
    t->b = tmp;
    t->m = mem;
    t->next = c->bct;
    c->bct = t;
    return tmp;
}

static void VKAPI_CALL cmd_copy_buffer_to_image(VkCommandBuffer cbh, VkBuffer bb, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
    struct img *ii = (struct img *) im;
    if (ii && ii->bc) {
        // 全部の範囲をほどいて 1 つの一時バッファに並べ、そこから 1 回で写す（一時バッファを作る往復を 1 回にする）
        const struct buf *src = (const struct buf *) bb;
        if (!src || !src->m || !src->m->shadow) { say("BC：CPU から見えないバッファからは写せない（飛ばした）"); return; }
        VkBufferImageCopy *r2 = malloc((n ? n : 1) * sizeof *r2);
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            r2[i] = rg[i];
            r2[i].bufferOffset = total;
            r2[i].bufferRowLength = r2[i].bufferImageHeight = 0;
            total += (bc_region_size(ii->bc, &rg[i]) + 15) & ~(uint64_t) 15;
        }
        uint8_t *dst = NULL;
        VkBuffer tmp = total ? bc_tmp(c, total, &dst) : VK_NULL_HANDLE;
        if (tmp && dst) {
            for (uint32_t i = 0; i < n; i++) bc_decode_region(src, ii->bc, &rg[i], dst + r2[i].bufferOffset);
            vr_u32(&c->s, VR_CMD_COPY_BUFFER_TO_IMAGE);
            vr_u64(&c->s, ID(tmp));
            vr_u64(&c->s, ID(im));
            vr_u32(&c->s, fix_layout(l));
            vr_u32(&c->s, n);
            vr_put(&c->s, r2, n * sizeof *r2);
            cb_reads(c, (struct buf *) tmp, 0, VK_WHOLE_SIZE);
        }
        free(r2);
        return;
    }
    vr_u32(&c->s, VR_CMD_COPY_BUFFER_TO_IMAGE);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, ID(im));
    vr_u32(&c->s, fix_layout(l));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
    cb_reads(c, (struct buf *) bb, 0, VK_WHOLE_SIZE);
}

static void VKAPI_CALL cmd_clear_color(VkCommandBuffer cbh, VkImage im, VkImageLayout l, const VkClearColorValue *col, uint32_t n, const VkImageSubresourceRange *rg) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_CLEAR_COLOR_IMAGE);
    vr_u64(&c->s, ID(im));
    vr_u32(&c->s, fix_layout(l));
    vr_put(&c->s, col, sizeof *col);
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
}

static void VKAPI_CALL cmd_copy_image(VkCommandBuffer cbh, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_COPY_IMAGE);
    vr_u64(&c->s, ID(s));
    vr_u32(&c->s, fix_layout(sl));
    vr_u64(&c->s, ID(d));
    vr_u32(&c->s, fix_layout(dl));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
}

static void VKAPI_CALL cmd_blit_image(VkCommandBuffer cbh, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BLIT_IMAGE);
    vr_u64(&c->s, ID(s));
    vr_u32(&c->s, fix_layout(sl));
    vr_u64(&c->s, ID(d));
    vr_u32(&c->s, fix_layout(dl));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
    vr_u32(&c->s, f);
}

// ---- 1.3 に向けて足した記録の命令 ----
// 受け手が知らない記録の命令は積まない（積むと受け手はその command buffer を丸ごと捨てる）。知らせるのは名前ごとに 1 回
static int need_cap(uint32_t cap, const char *what) {
    if (g_hw.caps & cap) return 1;
    static const char *told[64];
    static int nt;
    for (int i = 0; i < nt; i++) if (told[i] == what) return 0;
    if (nt < 64) told[nt++] = what;
    say("受け手が %s を知らないので飛ばした（古い受け手）", what);
    return 0;
}

static void VKAPI_CALL cmd_resolve(VkCommandBuffer cbh, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *rg) {
    if (!need_cap(VR_CAP_CMD10, "vkCmdResolveImage")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_RESOLVE_IMAGE);
    vr_u64(&c->s, ID(s));
    vr_u32(&c->s, fix_layout(sl));
    vr_u64(&c->s, ID(d));
    vr_u32(&c->s, fix_layout(dl));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
}

static void VKAPI_CALL cmd_clear_ds(VkCommandBuffer cbh, VkImage im, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    if (!need_cap(VR_CAP_CMD10, "vkCmdClearDepthStencilImage")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_CLEAR_DEPTH_STENCIL_IMAGE);
    vr_u64(&c->s, ID(im));
    vr_u32(&c->s, fix_layout(l));
    vr_put(&c->s, v, sizeof *v);
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
}

static void VKAPI_CALL cmd_clear_atts(VkCommandBuffer cbh, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rc) {
    if (!need_cap(VR_CAP_CMD10, "vkCmdClearAttachments")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_CLEAR_ATTACHMENTS);
    vr_u32(&c->s, na);
    vr_put(&c->s, a, na * sizeof *a);
    vr_u32(&c->s, nr);
    vr_put(&c->s, rc, nr * sizeof *rc);
}

// copy 2・blit 2・resolve 2：1.0 の命令に直して積む（GPU が読み書きする範囲の記録も 1.0 の命令の中でする）
static void VKAPI_CALL cmd_copy2(VkCommandBuffer cbh, const VkCopyBufferInfo2 *i) {
    VkBufferCopy *rg = malloc(sizeof *rg * (i->regionCount ? i->regionCount : 1));
    for (uint32_t k = 0; k < i->regionCount; k++) rg[k] = (VkBufferCopy) { i->pRegions[k].srcOffset, i->pRegions[k].dstOffset, i->pRegions[k].size };
    cmd_copy(cbh, i->srcBuffer, i->dstBuffer, i->regionCount, rg);
    free(rg);
}

static void VKAPI_CALL cmd_copy_image2(VkCommandBuffer cbh, const VkCopyImageInfo2 *i) {
    VkImageCopy *rg = malloc(sizeof *rg * (i->regionCount ? i->regionCount : 1));
    for (uint32_t k = 0; k < i->regionCount; k++) {
        const VkImageCopy2 *x = &i->pRegions[k];
        rg[k] = (VkImageCopy) { x->srcSubresource, x->srcOffset, x->dstSubresource, x->dstOffset, x->extent };
    }
    cmd_copy_image(cbh, i->srcImage, i->srcImageLayout, i->dstImage, i->dstImageLayout, i->regionCount, rg);
    free(rg);
}

static VkBufferImageCopy *bic1(const VkBufferImageCopy2 *r, uint32_t n) {
    VkBufferImageCopy *rg = malloc(sizeof *rg * (n ? n : 1));
    for (uint32_t k = 0; k < n; k++)
        rg[k] = (VkBufferImageCopy) { r[k].bufferOffset, r[k].bufferRowLength, r[k].bufferImageHeight, r[k].imageSubresource, r[k].imageOffset, r[k].imageExtent };
    return rg;
}

static void VKAPI_CALL cmd_copy_b2i2(VkCommandBuffer cbh, const VkCopyBufferToImageInfo2 *i) {
    VkBufferImageCopy *rg = bic1(i->pRegions, i->regionCount);
    cmd_copy_buffer_to_image(cbh, i->srcBuffer, i->dstImage, i->dstImageLayout, i->regionCount, rg);
    free(rg);
}

static void VKAPI_CALL cmd_copy_i2b2(VkCommandBuffer cbh, const VkCopyImageToBufferInfo2 *i) {
    VkBufferImageCopy *rg = bic1(i->pRegions, i->regionCount);
    cmd_copy_image_to_buffer(cbh, i->srcImage, i->srcImageLayout, i->dstBuffer, i->regionCount, rg);
    free(rg);
}

static void VKAPI_CALL cmd_blit2(VkCommandBuffer cbh, const VkBlitImageInfo2 *i) {
    VkImageBlit *rg = malloc(sizeof *rg * (i->regionCount ? i->regionCount : 1));
    for (uint32_t k = 0; k < i->regionCount; k++) {
        const VkImageBlit2 *x = &i->pRegions[k];
        rg[k].srcSubresource = x->srcSubresource;
        rg[k].dstSubresource = x->dstSubresource;
        memcpy(rg[k].srcOffsets, x->srcOffsets, sizeof rg[k].srcOffsets);
        memcpy(rg[k].dstOffsets, x->dstOffsets, sizeof rg[k].dstOffsets);
    }
    cmd_blit_image(cbh, i->srcImage, i->srcImageLayout, i->dstImage, i->dstImageLayout, i->regionCount, rg, i->filter);
    free(rg);
}

static void VKAPI_CALL cmd_resolve2(VkCommandBuffer cbh, const VkResolveImageInfo2 *i) {
    VkImageResolve *rg = malloc(sizeof *rg * (i->regionCount ? i->regionCount : 1));
    for (uint32_t k = 0; k < i->regionCount; k++) {
        const VkImageResolve2 *x = &i->pRegions[k];
        rg[k] = (VkImageResolve) { x->srcSubresource, x->srcOffset, x->dstSubresource, x->dstOffset, x->extent };
    }
    cmd_resolve(cbh, i->srcImage, i->srcImageLayout, i->dstImage, i->dstImageLayout, i->regionCount, rg);
    free(rg);
}

// ---- 後から変える値（VkDynamicState の番号と u32 の並びで送る） ----
static void put_dyn(VkCommandBuffer cbh, uint32_t cap, VkDynamicState st, uint32_t n, const void *v) {
    if (!need_cap(cap, cap == VR_CAP_CMD10 ? "1.0 の値の変え方" : "extended dynamic state")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_SET_DYNAMIC);
    vr_u32(&c->s, st);
    vr_u32(&c->s, n);
    vr_put(&c->s, v, (size_t) n * 4);
}
static void dyn1(VkCommandBuffer cbh, VkDynamicState st, uint32_t v) { put_dyn(cbh, VR_CAP_EXT_DYNAMIC_STATE, st, 1, &v); }

static void VKAPI_CALL cmd_set_line_width(VkCommandBuffer c, float w) { put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_LINE_WIDTH, 1, &w); }
static void VKAPI_CALL cmd_set_depth_bias(VkCommandBuffer c, float k, float cl, float sl) {
    float v[3] = { k, cl, sl };
    put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_DEPTH_BIAS, 3, v);
}
static void VKAPI_CALL cmd_set_blend_constants(VkCommandBuffer c, const float k[4]) { put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_BLEND_CONSTANTS, 4, k); }
static void VKAPI_CALL cmd_set_depth_bounds(VkCommandBuffer c, float lo, float hi) {
    float v[2] = { lo, hi };
    put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_DEPTH_BOUNDS, 2, v);
}
static void VKAPI_CALL cmd_set_stencil_compare(VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m) {
    uint32_t v[2] = { f, m };
    put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, 2, v);
}
static void VKAPI_CALL cmd_set_stencil_write(VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m) {
    uint32_t v[2] = { f, m };
    put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, 2, v);
}
static void VKAPI_CALL cmd_set_stencil_ref(VkCommandBuffer c, VkStencilFaceFlags f, uint32_t m) {
    uint32_t v[2] = { f, m };
    put_dyn(c, VR_CAP_CMD10, VK_DYNAMIC_STATE_STENCIL_REFERENCE, 2, v);
}
// extended dynamic state 1・2（1.3 で本体入り）
static void VKAPI_CALL cmd_set_cull_mode(VkCommandBuffer c, VkCullModeFlags m) { dyn1(c, VK_DYNAMIC_STATE_CULL_MODE, m); }
static void VKAPI_CALL cmd_set_front_face(VkCommandBuffer c, VkFrontFace f) { dyn1(c, VK_DYNAMIC_STATE_FRONT_FACE, f); }
static void VKAPI_CALL cmd_set_topology(VkCommandBuffer c, VkPrimitiveTopology t) { gs_set_topology((struct cb *) c, t); }
static void VKAPI_CALL cmd_set_depth_test(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, v); }
static void VKAPI_CALL cmd_set_depth_write(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, v); }
static void VKAPI_CALL cmd_set_depth_compare(VkCommandBuffer c, VkCompareOp v) { dyn1(c, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, v); }
static void VKAPI_CALL cmd_set_depth_bounds_test(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, v); }
static void VKAPI_CALL cmd_set_stencil_test(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, v); }
static void VKAPI_CALL cmd_set_raster_discard(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, v); }
static void VKAPI_CALL cmd_set_depth_bias_enable(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, v); }
static void VKAPI_CALL cmd_set_restart(VkCommandBuffer c, VkBool32 v) { dyn1(c, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, v); }
static void VKAPI_CALL cmd_set_stencil_op(VkCommandBuffer c, VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail, VkCompareOp cmp) {
    uint32_t v[5] = { f, fail, pass, dfail, cmp };
    put_dyn(c, VR_CAP_EXT_DYNAMIC_STATE, VK_DYNAMIC_STATE_STENCIL_OP, 5, v);
}
static void VKAPI_CALL cmd_set_viewport_count(VkCommandBuffer c, uint32_t n, const VkViewport *v) {
    put_dyn(c, VR_CAP_EXT_DYNAMIC_STATE, VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, n * 6, v);
}
static void VKAPI_CALL cmd_set_scissor_count(VkCommandBuffer c, uint32_t n, const VkRect2D *v) {
    put_dyn(c, VR_CAP_EXT_DYNAMIC_STATE, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, n * 4, v);
}

static void VKAPI_CALL cmd_bind_vbs2(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkBuffer *bs, const VkDeviceSize *offs,
                                     const VkDeviceSize *sizes, const VkDeviceSize *strides) {
    // 大きさも間隔も無ければ 1.0 の命令と同じ
    if (!sizes && !strides) { cmd_bind_vbs(cbh, first, n, bs, offs); return; }
    if (!need_cap(VR_CAP_EXT_DYNAMIC_STATE, "vkCmdBindVertexBuffers2")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_VERTEX_BUFFERS2);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_u32(&c->s, sizes ? 1 : 0);
    vr_u32(&c->s, strides ? 1 : 0);
    for (uint32_t i = 0; i < n; i++) {
        vr_u64(&c->s, ID(bs[i]));
        vr_u64(&c->s, offs[i]);
        vr_u64(&c->s, sizes ? sizes[i] : VK_WHOLE_SIZE);
        vr_u64(&c->s, strides ? strides[i] : 0);
        cb_reads(c, (struct buf *) bs[i], offs[i], sizes ? sizes[i] : VK_WHOLE_SIZE);
    }
    gs_track_vb(c, first, n, bs, offs, strides);
}

// ---- dynamic rendering ----
static void put_rendering_att(vr_buf *s, const VkRenderingAttachmentInfo *a) {
    vr_u64(s, ID(a->imageView));
    vr_u32(s, fix_layout(a->imageLayout));
    vr_u32(s, a->resolveMode);
    vr_u64(s, ID(a->resolveImageView));
    vr_u32(s, fix_layout(a->resolveImageLayout));
    vr_u32(s, a->loadOp);
    vr_u32(s, a->storeOp);
    vr_put(s, &a->clearValue, sizeof a->clearValue);
}

static void VKAPI_CALL cmd_begin_rendering(VkCommandBuffer cbh, const VkRenderingInfo *ri) {
    if (!need_cap(VR_CAP_DYNAMIC_RENDERING, "dynamic rendering")) return;
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BEGIN_RENDERING);
    // 2 次の command buffer の中身はここに写して送るので、受け手では「2 次から描く」の印を外す
    vr_u32(&c->s, ri->flags & ~(uint32_t) VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    vr_put(&c->s, &ri->renderArea, sizeof ri->renderArea);
    vr_u32(&c->s, ri->layerCount);
    vr_u32(&c->s, ri->viewMask);
    vr_u32(&c->s, ri->colorAttachmentCount);
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) put_rendering_att(&c->s, &ri->pColorAttachments[i]);
    vr_u32(&c->s, ri->pDepthAttachment ? 1 : 0);
    if (ri->pDepthAttachment) put_rendering_att(&c->s, ri->pDepthAttachment);
    vr_u32(&c->s, ri->pStencilAttachment ? 1 : 0);
    if (ri->pStencilAttachment) put_rendering_att(&c->s, ri->pStencilAttachment);
}

static void VKAPI_CALL cmd_end_rendering(VkCommandBuffer cbh) {
    if (!need_cap(VR_CAP_DYNAMIC_RENDERING, "dynamic rendering")) return;
    vr_u32(&((struct cb *) cbh)->s, VR_CMD_END_RENDERING);
}

// ---- synchronization2 の仕切り ----
// 古い受け手には 1.0 の仕切りに直して送る（stage と access は 64 ビットから 1.0 の印へ広めに寄せる）
static VkPipelineStageFlags stage1(VkPipelineStageFlags2 s) {
    VkPipelineStageFlags r = (VkPipelineStageFlags) (s & 0xFFFFFFFFull);
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT))
        r |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) r |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (s & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) r |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    if (s >> 39) r |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return r;
}

static VkAccessFlags access1(VkAccessFlags2 a) {
    VkAccessFlags r = (VkAccessFlags) (a & 0xFFFFFFFFull);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) r |= VK_ACCESS_SHADER_READ_BIT;
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) r |= VK_ACCESS_SHADER_WRITE_BIT;
    if (a >> 35) r |= VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    return r;
}

static VkImageLayout layout1(VkImageLayout l, VkImageAspectFlags asp) {
    int color = (asp & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
    if (l == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL) return color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL) return color ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    return l;
}

static void VKAPI_CALL cmd_barrier2(VkCommandBuffer cbh, const VkDependencyInfo *di) {
    struct cb *c = (struct cb *) cbh;
    if (g_hw.caps & VR_CAP_SYNC2) {
        vr_u32(&c->s, VR_CMD_BARRIER_SYNC2);
        vr_u32(&c->s, di->dependencyFlags);
        vr_u32(&c->s, di->memoryBarrierCount);
        for (uint32_t i = 0; i < di->memoryBarrierCount; i++) {
            const VkMemoryBarrier2 *m = &di->pMemoryBarriers[i];
            vr_u64(&c->s, m->srcStageMask);
            vr_u64(&c->s, m->srcAccessMask);
            vr_u64(&c->s, m->dstStageMask);
            vr_u64(&c->s, m->dstAccessMask);
        }
        vr_u32(&c->s, di->bufferMemoryBarrierCount);
        for (uint32_t i = 0; i < di->bufferMemoryBarrierCount; i++) {
            const VkBufferMemoryBarrier2 *m = &di->pBufferMemoryBarriers[i];
            vr_u64(&c->s, m->srcStageMask);
            vr_u64(&c->s, m->srcAccessMask);
            vr_u64(&c->s, m->dstStageMask);
            vr_u64(&c->s, m->dstAccessMask);
            vr_u32(&c->s, m->srcQueueFamilyIndex);
            vr_u32(&c->s, m->dstQueueFamilyIndex);
            vr_u64(&c->s, ID(m->buffer));
            vr_u64(&c->s, m->offset);
            vr_u64(&c->s, m->size);
        }
        vr_u32(&c->s, di->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < di->imageMemoryBarrierCount; i++) {
            const VkImageMemoryBarrier2 *m = &di->pImageMemoryBarriers[i];
            vr_u64(&c->s, m->srcStageMask);
            vr_u64(&c->s, m->srcAccessMask);
            vr_u64(&c->s, m->dstStageMask);
            vr_u64(&c->s, m->dstAccessMask);
            vr_u32(&c->s, fix_layout(m->oldLayout));
            vr_u32(&c->s, fix_layout(m->newLayout));
            vr_u32(&c->s, m->srcQueueFamilyIndex);
            vr_u32(&c->s, m->dstQueueFamilyIndex);
            vr_u64(&c->s, ID(m->image));
            vr_put(&c->s, &m->subresourceRange, sizeof m->subresourceRange);
        }
        return;
    }
    // 古い受け手：1.0 の仕切り 1 つにまとめる（stage はみんな合わせる）
    VkPipelineStageFlags src = 0, dst = 0;
    VkMemoryBarrier mb[16];
    uint32_t nm = di->memoryBarrierCount < 16 ? di->memoryBarrierCount : 16;
    for (uint32_t i = 0; i < nm; i++) {
        const VkMemoryBarrier2 *m = &di->pMemoryBarriers[i];
        src |= stage1(m->srcStageMask);
        dst |= stage1(m->dstStageMask);
        mb[i] = (VkMemoryBarrier) { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, access1(m->srcAccessMask), access1(m->dstAccessMask) };
    }
    uint32_t nb = di->bufferMemoryBarrierCount, ni = di->imageMemoryBarrierCount;
    VkBufferMemoryBarrier *bb = calloc(nb ? nb : 1, sizeof *bb);
    VkImageMemoryBarrier *ib = calloc(ni ? ni : 1, sizeof *ib);
    for (uint32_t i = 0; i < nb; i++) {
        const VkBufferMemoryBarrier2 *m = &di->pBufferMemoryBarriers[i];
        src |= stage1(m->srcStageMask);
        dst |= stage1(m->dstStageMask);
        bb[i] = (VkBufferMemoryBarrier) { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, access1(m->srcAccessMask), access1(m->dstAccessMask),
                                          m->srcQueueFamilyIndex, m->dstQueueFamilyIndex, m->buffer, m->offset, m->size };
    }
    for (uint32_t i = 0; i < ni; i++) {
        const VkImageMemoryBarrier2 *m = &di->pImageMemoryBarriers[i];
        src |= stage1(m->srcStageMask);
        dst |= stage1(m->dstStageMask);
        VkImageAspectFlags asp = m->subresourceRange.aspectMask;
        ib[i] = (VkImageMemoryBarrier) { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, access1(m->srcAccessMask), access1(m->dstAccessMask),
                                         layout1(m->oldLayout, asp), layout1(m->newLayout, asp), m->srcQueueFamilyIndex, m->dstQueueFamilyIndex,
                                         m->image, m->subresourceRange };
    }
    cmd_barrier(cbh, src ? src : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, dst ? dst : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, di->dependencyFlags,
                nm, mb, nb, bb, ni, ib);
    free(bb);
    free(ib);
}

// ---- 同期 ----
static VkResult VKAPI_CALL create_fence(VkDevice dv, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void) a;
    struct fence *f = calloc(1, sizeof *f);
    if (!f) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(f));
    vr_u32(&b, ci->flags);
    rpc_send(VR_CREATE_FENCE, &b);
    free(b.p);
    *out = (VkFence) f;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_fence(VkDevice dv, VkFence f, const VkAllocationCallbacks *a) {
    (void) a;
    if (!f) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(f));
    rpc_send(VR_DESTROY_FENCE, &b);
    free(b.p);
    free((struct fence *) f);
}

static VkResult VKAPI_CALL reset_fences(VkDevice dv, uint32_t n, const VkFence *fs) {
    for (uint32_t i = 0; i < n; i++) { ((struct fence *) fs[i])->q = NULL; ((struct fence *) fs[i])->serial = 0; }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) vr_u64(&b, ID(fs[i]));
    rpc_send(VR_RESET_FENCES, &b);
    free(b.p);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL wait_fences(VkDevice dv, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
    // どれも submit に付けた fence で、終わるまで待つ呼び方なら、往復しない。
    // 持ち帰る物があれば引き取る（その MEM_READ の前に受け手が仕事の終わりを待つ）
    // DXVK は「1 秒の期限で待つ」を繰り返す。0.1 秒以上の期限は、期限なしと同じに扱う
    int known = timeout >= 100000000ull;
    for (uint32_t i = 0; i < n && known; i++) known = ((struct fence *) fs[i])->q != NULL;
    if (known) {
        for (uint32_t i = 0; i < n; i++) complete((struct dev *) dv, ((struct fence *) fs[i])->q, ((struct fence *) fs[i])->serial);
        g_stat.skipped++;
        return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) vr_u64(&b, ID(fs[i]));
    vr_u32(&b, all);
    vr_u64(&b, timeout);
    VkResult r = rpc_call(VR_WAIT_FOR_FENCES, &b, NULL);
    free(b.p);
    if (r == VK_SUCCESS) {
        // fence が知らせたら、そのキューでそれより前に出した submit も終わっている
        for (uint32_t i = 0; i < n; i++) {
            struct fence *f = (struct fence *) fs[i];
            if (!f->q) continue;
            if (all) complete((struct dev *) dv, f->q, f->serial);
            else if (n == 1) complete((struct dev *) dv, f->q, f->serial);
            else {
                // どれが知らせたか分からないので 1 つずつ尋ねる
                vr_buf q = { 0 };
                vr_u64(&q, ID(dv));
                vr_u64(&q, ID(f));
                if (rpc_call(VR_GET_FENCE_STATUS, &q, NULL) == VK_SUCCESS) complete((struct dev *) dv, f->q, f->serial);
                free(q.p);
            }
        }
    }
    return r;
}

// 同じ fence を間を空けずに聞き続けるアプリ（llama.cpp など）は、聞くたびに USB を往復させてしまう。
// 2 ms 以内に続けて聞かれたら空回りと見なし、受け手の側で最大 20 ms 待ってから答える。
// 1 コマに 1 回だけ聞くようなアプリには、今まで通りすぐ答える
static VkResult VKAPI_CALL fence_status(VkDevice dv, VkFence fh) {
    struct fence *fc = (struct fence *) fh;
    if (fc->q) {
        // submit に付けた fence は「終わった」と答える（版 3 の待ち方）
        complete((struct dev *) dv, fc->q, fc->serial);
        g_stat.skipped++;
        return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    }
    double t = now_ms();
    int spinning = t - fc->last_poll < 2.0;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    VkResult r;
    if (spinning) {
        vr_u32(&b, 1);
        vr_u64(&b, ID(fh));
        vr_u32(&b, VK_TRUE);
        vr_u64(&b, 20000000ull);
        r = rpc_call(VR_WAIT_FOR_FENCES, &b, NULL);
        if (r == VK_TIMEOUT) r = VK_NOT_READY;
    } else {
        vr_u64(&b, ID(fh));
        r = rpc_call(VR_GET_FENCE_STATUS, &b, NULL);
    }
    free(b.p);
    fc->last_poll = now_ms();
    VkFence f = fh;
    if (r == VK_SUCCESS && ((struct fence *) f)->q) complete((struct dev *) dv, ((struct fence *) f)->q, ((struct fence *) f)->serial);
    return r;
}

static VkResult VKAPI_CALL create_semaphore(VkDevice dv, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    (void) a;
    struct sem *sm = calloc(1, sizeof *sm);
    if (!sm) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t type = VK_SEMAPHORE_TYPE_BINARY;
    uint64_t initial = 0;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
            type = ((const VkSemaphoreTypeCreateInfo *) s)->semaphoreType;
            initial = ((const VkSemaphoreTypeCreateInfo *) s)->initialValue;
        }
    sm->timeline = type == VK_SEMAPHORE_TYPE_TIMELINE;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(sm));
    vr_u32(&b, type);
    vr_u64(&b, initial);
    rpc_send(VR_CREATE_SEMAPHORE, &b);
    free(b.p);
    *out = (VkSemaphore) sm;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_semaphore(VkDevice dv, VkSemaphore h, const VkAllocationCallbacks *a) {
    (void) a;
    if (!h) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(h));
    rpc_send(VR_DESTROY_SEMAPHORE, &b);
    free(b.p);
    struct sem *sm = (struct sem *) h;
    free(sm->sig);
    free(sm);
}

// semaphore が値 v に届いた：その値までを知らせる submit は終わっている
static void sem_reached(struct dev *d, struct sem *sm, uint64_t v) {
    if (!sm || !sm->timeline) return;
    struct { struct queue *q; uint64_t serial; } done[16];
    int nd = 0;
    AcquireSRWLockExclusive(&d->lock);
    size_t k = 0;
    for (size_t i = 0; i < sm->n; i++) {
        if (sm->sig[i].value <= v) {
            int j = 0;
            while (j < nd && done[j].q != sm->sig[i].q) j++;
            if (j == nd && nd < 16) { done[nd].q = sm->sig[i].q; done[nd].serial = 0; nd++; }
            if (j < nd && sm->sig[i].serial > done[j].serial) done[j].serial = sm->sig[i].serial;
        } else {
            sm->sig[k++] = sm->sig[i];
        }
    }
    sm->n = k;
    if (v > sm->reached) sm->reached = v;
    ReleaseSRWLockExclusive(&d->lock);
    for (int j = 0; j < nd; j++) complete(d, done[j].q, done[j].serial);
}

static VkResult VKAPI_CALL get_sem_counter(VkDevice dv, VkSemaphore h, uint64_t *v) {
    vr_buf b = { 0 }, out = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(h));
    VkResult r = rpc_call(VR_GET_SEMAPHORE_COUNTER, &b, &out);
    *v = 0;
    if (out.n >= 8) memcpy(v, out.p, 8);
    free(b.p);
    free(out.p);
    if (r == VK_SUCCESS) sem_reached((struct dev *) dv, (struct sem *) h, *v);
    return r;
}

static VkResult VKAPI_CALL wait_sems(VkDevice dv, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    struct dev *d = (struct dev *) dv;
    if (timeout == UINT64_MAX && !(wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT)) {
        // それぞれの値を知らせる submit が分かっていれば、往復しない
        int ok = 1;
        uint64_t need[64];
        for (uint32_t i = 0; i < wi->semaphoreCount && ok; i++) {
            struct sem *sm = (struct sem *) wi->pSemaphores[i];
            uint64_t v = wi->pValues[i], best = UINT64_MAX;
            if (i >= 64 || !sm->timeline) { ok = 0; break; }
            if (v <= sm->reached) { need[i] = 0; continue; }
            AcquireSRWLockShared(&d->lock);
            for (size_t k = 0; k < sm->n; k++) if (sm->sig[k].value >= v && sm->sig[k].value < best) best = sm->sig[k].value;
            ReleaseSRWLockShared(&d->lock);
            if (best == UINT64_MAX) ok = 0;
            else need[i] = best;
        }
        if (ok) {
            for (uint32_t i = 0; i < wi->semaphoreCount; i++)
                if (need[i]) sem_reached(d, (struct sem *) wi->pSemaphores[i], need[i]);
            g_stat.skipped++;
            return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        }
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u32(&b, wi->flags);
    vr_u32(&b, wi->semaphoreCount);
    for (uint32_t i = 0; i < wi->semaphoreCount; i++) { vr_u64(&b, ID(wi->pSemaphores[i])); vr_u64(&b, wi->pValues[i]); }
    vr_u64(&b, timeout);
    VkResult r = rpc_call(VR_WAIT_SEMAPHORES, &b, NULL);
    free(b.p);
    if (r != VK_SUCCESS) return r;
    if (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) {
        // どれが届いたか分からないので、いまの値を尋ねる
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
            uint64_t v;
            get_sem_counter(dv, wi->pSemaphores[i], &v);
        }
    } else {
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) sem_reached((struct dev *) dv, (struct sem *) wi->pSemaphores[i], wi->pValues[i]);
    }
    return r;
}

static VkResult VKAPI_CALL signal_sem(VkDevice dv, const VkSemaphoreSignalInfo *si) {
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(si->semaphore));
    vr_u64(&b, si->value);
    rpc_send(VR_SIGNAL_SEMAPHORE, &b);
    free(b.p);
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL queue_submit(VkQueue qh, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    struct queue *q = (struct queue *) qh;
    struct dev *d = q->d;
    // GPU が読む範囲のうち、アプリが書いたページを先に送る
    {
        rng *rd = NULL;
        size_t nrd = 0, crd = 0;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
                for (size_t k = 0; k < c->nrd; k++) rng_add(&rd, &nrd, &crd, c->rd[k].m, c->rd[k].off, c->rd[k].len, 0);
            }
        rng_merge(rd, &nrd);
        for (size_t k = 0; k < nrd; k++) send_dirty(rd[k].m, rd[k].off, rd[k].len);
        free(rd);
    }
    // 受け手は 1 回に 8 まで受け取るので、分けて送る（fence は最後にだけ付ける）
    for (uint32_t at = 0; at < n || (n == 0 && at == 0); at += 8) {
        uint32_t k = n - at < 8 ? n - at : 8;
        vr_buf b = { 0 };
        vr_u64(&b, ID(q));
        vr_u32(&b, k);
        for (uint32_t i = at; i < at + k; i++) {
            const VkTimelineSemaphoreSubmitInfo *tl = NULL;
            for (const VkBaseInStructure *s = si[i].pNext; s; s = s->pNext)
                if (s->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const VkTimelineSemaphoreSubmitInfo *) s;
            vr_u32(&b, si[i].waitSemaphoreCount);
            for (uint32_t j = 0; j < si[i].waitSemaphoreCount; j++) {
                vr_u64(&b, ID(si[i].pWaitSemaphores[j]));
                vr_u32(&b, si[i].pWaitDstStageMask[j]);
                vr_u64(&b, tl && j < tl->waitSemaphoreValueCount ? tl->pWaitSemaphoreValues[j] : 0);
            }
            vr_u32(&b, si[i].commandBufferCount);
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) vr_u64(&b, ID(si[i].pCommandBuffers[j]));
            vr_u32(&b, si[i].signalSemaphoreCount);
            for (uint32_t j = 0; j < si[i].signalSemaphoreCount; j++) {
                vr_u64(&b, ID(si[i].pSignalSemaphores[j]));
                vr_u64(&b, tl && j < tl->signalSemaphoreValueCount ? tl->pSignalSemaphoreValues[j] : 0);
            }
        }
        vr_u64(&b, at + 8 >= n ? ID(f) : 0);
        rpc_send(VR_QUEUE_SUBMIT, &b);
        free(b.p);
        if (n == 0) break;
    }
    rpc_flush();
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
            ev_fire(c);
        }
    AcquireSRWLockExclusive(&d->lock);
    uint64_t serial = ++d->serial;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
            for (size_t k = 0; k < c->nr; k++) {
                size_t before = d->npend;
                rng_add(&d->pend, &d->npend, &d->cappend, c->r[k].m, c->r[k].off, c->r[k].len, serial);
                if (d->npend > before) d->pend[before].q = q;
            }
        }
        const VkTimelineSemaphoreSubmitInfo *tl = NULL;
        for (const VkBaseInStructure *s = si[i].pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const VkTimelineSemaphoreSubmitInfo *) s;
        for (uint32_t j = 0; tl && j < si[i].signalSemaphoreCount && j < tl->signalSemaphoreValueCount; j++) {
            struct sem *sm = (struct sem *) si[i].pSignalSemaphores[j];
            if (!sm->timeline) continue;
            if (sm->n == sm->cap) { sm->cap = sm->cap ? sm->cap * 2 : 8; sm->sig = realloc(sm->sig, sm->cap * sizeof *sm->sig); }
            sm->sig[sm->n++] = (sigrec) { tl->pSignalSemaphoreValues[j], serial, q };
        }
    }
    ReleaseSRWLockExclusive(&d->lock);
    if (f) { ((struct fence *) f)->serial = serial; ((struct fence *) f)->q = q; }
    return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult VKAPI_CALL queue_wait_idle(VkQueue qh) {
    struct queue *q = (struct queue *) qh;
    complete(q->d, q, q->d->serial);
    g_stat.skipped++;
    return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    vr_buf b = { 0 };
    vr_u64(&b, ID(q));
    VkResult r = rpc_call(VR_QUEUE_WAIT_IDLE, &b, NULL);
    free(b.p);
    if (r == VK_SUCCESS) complete(q->d, q, q->d->serial);
    return r;
}

static VkResult VKAPI_CALL device_wait_idle(VkDevice dv) {
    struct dev *d = (struct dev *) dv;
    complete(d, NULL, d->serial);
    g_stat.skipped++;
    return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    VkResult r = rpc_call(VR_DEVICE_WAIT_IDLE, &b, NULL);
    free(b.p);
    if (r == VK_SUCCESS) complete(d, NULL, d->serial);
    return r;
}

// ---- まだ中継していない必須の入口 ----
// Vulkan 1.2 の装置は、必須の入口がそろっていないといけない（そろっていないと装置作りを止めるレイヤーがある）。
// 呼ばれたら「無い」と答える。x64 の呼び方では、引数の数が違っても呼び出し側が片づけるので、1 つの関数で受けられる
// ---- VkEvent ----
// 受け手には送らない。待つ側（vkCmdWaitEvents）を、全部の仕切り（vkCmdPipelineBarrier）に置き換える。
// event は同じキューの中だけで使う物なので、仕切りで submit の順に従わせれば、少し広く待つだけで同じ意味になる。
// llama.cpp は GPU を 2 つ使うとき、GPU どうしの待ち合わせに使う。
// GPU が立てる・倒すのは submit のときにする（fence と同じく、出した仕事は終わったものとして扱う）。
// 記録の時点で立てると、DXVK の「記録してから手元で倒す」順で倒れたままになり、DirectX 11 の EVENT のクエリが返らない
struct event { int set; };
static void ev_add(struct cb *c, struct event *e, int val) {
    if (!e) return;
    if (c->nev == c->cev) {
        size_t cap = c->cev ? c->cev * 2 : 8;
        struct evact *p = realloc(c->ev, cap * sizeof *p);
        if (!p) return;
        c->ev = p;
        c->cev = cap;
    }
    c->ev[c->nev++] = (struct evact) { e, val };
}
// submit のときに、記録した順に立てる・倒す
static void ev_fire(struct cb *c) {
    for (size_t k = 0; k < c->nev; k++) c->ev[k].e->set = c->ev[k].val;
}
static VkResult VKAPI_CALL create_event(VkDevice dv, const VkEventCreateInfo *ci, const VkAllocationCallbacks *a, VkEvent *out) {
    (void) dv; (void) ci; (void) a;
    struct event *e = calloc(1, sizeof *e);
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkEvent) e;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_event(VkDevice dv, VkEvent ev, const VkAllocationCallbacks *a) { (void) dv; (void) a; free((struct event *) ev); }
static VkResult VKAPI_CALL get_event_status(VkDevice dv, VkEvent ev) { (void) dv; return ((struct event *) ev)->set ? VK_EVENT_SET : VK_EVENT_RESET; }
static VkResult VKAPI_CALL set_event(VkDevice dv, VkEvent ev) { (void) dv; ((struct event *) ev)->set = 1; return VK_SUCCESS; }
static VkResult VKAPI_CALL reset_event(VkDevice dv, VkEvent ev) { (void) dv; ((struct event *) ev)->set = 0; return VK_SUCCESS; }
static void VKAPI_CALL cmd_set_event(VkCommandBuffer cbh, VkEvent ev, VkPipelineStageFlags st) { (void) st; ev_add((struct cb *) cbh, (struct event *) ev, 1); }
static void VKAPI_CALL cmd_reset_event(VkCommandBuffer cbh, VkEvent ev, VkPipelineStageFlags st) { (void) st; ev_add((struct cb *) cbh, (struct event *) ev, 0); }
static void VKAPI_CALL cmd_wait_events(VkCommandBuffer cbh, uint32_t n, const VkEvent *evs, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                       uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb,
                                       uint32_t ni, const VkImageMemoryBarrier *ib) {
    (void) n; (void) evs; (void) src;
    VkMemoryBarrier all[16];
    uint32_t k = 0;
    for (; k < nm && k < 15; k++) all[k] = mb[k];
    all[k++] = (VkMemoryBarrier) { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    cmd_barrier(cbh, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dst ? dst : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, k, all, nb, bb, ni, ib);
}

// ---- maintenance5 と 6 の入口。1.0 の入口に直す ----
static void VKAPI_CALL cmd_bind_ib(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, VkIndexType t);

static void VKAPI_CALL cmd_bind_sets2(VkCommandBuffer cbh, const VkBindDescriptorSetsInfo *i) {
    // 描く用と計算用の両方の段に効く指定なら、両方につなぐ
    if (i->stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS))
        cmd_bind_sets(cbh, VK_PIPELINE_BIND_POINT_GRAPHICS, i->layout, i->firstSet, i->descriptorSetCount, i->pDescriptorSets, i->dynamicOffsetCount, i->pDynamicOffsets);
    if (i->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        cmd_bind_sets(cbh, VK_PIPELINE_BIND_POINT_COMPUTE, i->layout, i->firstSet, i->descriptorSetCount, i->pDescriptorSets, i->dynamicOffsetCount, i->pDynamicOffsets);
}

static void VKAPI_CALL cmd_push2(VkCommandBuffer cbh, const VkPushConstantsInfo *i) {
    cmd_push(cbh, i->layout, i->stageFlags, i->offset, i->size, i->pValues);
}

static void VKAPI_CALL cmd_bind_ib2(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, VkDeviceSize size, VkIndexType t) {
    (void) size;
    if (!bb) return; // 空の index buffer：使う描画は無いので、つながない
    cmd_bind_ib(cbh, bb, off, t);
}

static void VKAPI_CALL rendering_area_granularity(VkDevice dv, const VkRenderingAreaInfo *i, VkExtent2D *g) {
    (void) dv; (void) i;
    g->width = g->height = 1;
}

static void VKAPI_CALL get_subres_layout2(VkDevice dv, VkImage im, const VkImageSubresource2 *sr, VkSubresourceLayout2 *l) {
    get_subres_layout(dv, im, &sr->imageSubresource, &l->subresourceLayout);
}

// 作る前の画像の並び方：同じ形の画像を受け手に作らせて尋ね、すぐ消す
static void VKAPI_CALL get_device_image_subres_layout(VkDevice dv, const VkDeviceImageSubresourceInfo *i, VkSubresourceLayout2 *l) {
    memset(&l->subresourceLayout, 0, sizeof l->subresourceLayout);
    VkImage im;
    if (create_image(dv, i->pCreateInfo, NULL, &im) != VK_SUCCESS) return;
    get_subres_layout(dv, im, &i->pSubresource->imageSubresource, &l->subresourceLayout);
    destroy_image(dv, im, NULL);
}

// ---- buffer device address：受け手の GPU の中のアドレスを尋ねて覚える ----
static VkDeviceAddress VKAPI_CALL get_buffer_address(VkDevice dv, const VkBufferDeviceAddressInfo *i) {
    struct buf *bf = (struct buf *) i->buffer;
    if (!bf || !need_cap(VR_CAP_BDA, "vkGetBufferDeviceAddress")) return 0;
    if (bf->addr) return bf->addr;
    vr_buf b = { 0 }, o = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bf));
    if (rpc_call(VR_GET_BUFFER_ADDRESS, &b, &o) == VK_SUCCESS && o.n >= 8) memcpy(&bf->addr, o.p, 8);
    free(b.p);
    free(o.p);
    return bf->addr;
}

static uint64_t VKAPI_CALL get_buffer_capture_address(VkDevice dv, const VkBufferDeviceAddressInfo *i) { (void) dv; (void) i; return 0; }
static uint64_t VKAPI_CALL get_memory_capture_address(VkDevice dv, const VkDeviceMemoryOpaqueCaptureAddressInfo *i) { (void) dv; (void) i; return 0; }

// ---- query（GPU に数えさせる：遮蔽・時刻） ----
// 受け手の VR_CREATE_QUERY_POOL などに送る。結果は受け手が出した仕事を全部待ってから読む
static VkResult VKAPI_CALL create_qpool(VkDevice dv, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void) a;
    if (!(g_hw.caps & VR_CAP_QUERY)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->queryType);
    vr_u32(&b, ci->queryCount);
    vr_u32(&b, ci->pipelineStatistics);
    rpc_send(VR_CREATE_QUERY_POOL, &b);
    free(b.p);
    *out = (VkQueryPool) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_qpool(VkDevice dv, VkQueryPool qp, const VkAllocationCallbacks *a) {
    (void) a;
    if (!qp) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    rpc_send(VR_DESTROY_QUERY_POOL, &b);
    free(b.p);
}

static VkResult VKAPI_CALL get_qpool_results(VkDevice dv, VkQueryPool qp, uint32_t first, uint32_t n, size_t size, void *data,
                                             VkDeviceSize stride, VkQueryResultFlags flags) {
    vr_buf b = { 0 }, out = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    vr_u32(&b, first);
    vr_u32(&b, n);
    vr_u64(&b, size);
    vr_u64(&b, stride);
    vr_u32(&b, flags);
    VkResult r = rpc_call(VR_GET_QUERY_POOL_RESULTS, &b, &out);
    free(b.p);
    if (r >= 0 && out.p) memcpy(data, out.p, out.n < size ? out.n : size);
    free(out.p);
    return r;
}

static void VKAPI_CALL reset_qpool(VkDevice dv, VkQueryPool qp, uint32_t first, uint32_t n) {
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    vr_u32(&b, first);
    vr_u32(&b, n);
    rpc_send(VR_RESET_QUERY_POOL, &b);
    free(b.p);
}

static void VKAPI_CALL cmd_reset_qpool(VkCommandBuffer cbh, VkQueryPool qp, uint32_t first, uint32_t n) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_RESET_QUERY_POOL);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
}

static void VKAPI_CALL cmd_begin_query(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q, VkQueryControlFlags flags) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BEGIN_QUERY);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
    vr_u32(&c->s, flags);
}

static void VKAPI_CALL cmd_end_query(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_END_QUERY);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
}

static void VKAPI_CALL cmd_write_timestamp(VkCommandBuffer cbh, VkPipelineStageFlagBits st, VkQueryPool qp, uint32_t q) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_WRITE_TIMESTAMP);
    vr_u32(&c->s, st);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
}

static void VKAPI_CALL cmd_write_timestamp2(VkCommandBuffer cbh, VkPipelineStageFlags2 st, VkQueryPool qp, uint32_t q) {
    // 2 の stage は、1 の形のいちばん後ろの段にまとめる
    VkPipelineStageFlagBits s1 = (st >> 32) ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : (VkPipelineStageFlagBits) (st ? st : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (st & (st - 1)) s1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; // 複数の段なら一番後ろ
    cmd_write_timestamp(cbh, s1, qp, q);
}

static void VKAPI_CALL cmd_copy_qpool_results(VkCommandBuffer cbh, VkQueryPool qp, uint32_t first, uint32_t n, VkBuffer dst,
                                              VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_COPY_QUERY_RESULTS);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_u64(&c->s, ID(dst));
    vr_u64(&c->s, off);
    vr_u64(&c->s, stride);
    vr_u32(&c->s, flags);
    // GPU が書く範囲として覚える（CPU から見えるメモリなら、待ちのあとに引き取る）
    cb_writes(c, (struct buf *) dst, off, n ? stride * (n - 1) + ((flags & VK_QUERY_RESULT_64_BIT) ? 16 : 8) : 0);
}

// ---- Vulkan 1.3 の必須の残り ----
// vkQueueSubmit2：VkSubmitInfo2 を今までの VkSubmitInfo に直して出す。stage は stage1 で 1 の形に直す

static VkResult VKAPI_CALL queue_submit2(VkQueue qh, uint32_t n, const VkSubmitInfo2 *si, VkFence f) {
    if (n == 0) return queue_submit(qh, 0, NULL, f);
    VkSubmitInfo *s1 = calloc(n, sizeof *s1);
    VkTimelineSemaphoreSubmitInfo *tl = calloc(n, sizeof *tl);
    VkSemaphore **ws = calloc(n, sizeof *ws), **ss = calloc(n, sizeof *ss);
    VkPipelineStageFlags **wst = calloc(n, sizeof *wst);
    uint64_t **wv = calloc(n, sizeof *wv), **sv = calloc(n, sizeof *sv);
    VkCommandBuffer **cbs = calloc(n, sizeof *cbs);
    VkResult r = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!s1 || !tl || !ws || !ss || !wst || !wv || !sv || !cbs) goto out;
    for (uint32_t i = 0; i < n; i++) {
        const VkSubmitInfo2 *a = &si[i];
        uint32_t nw = a->waitSemaphoreInfoCount, nc = a->commandBufferInfoCount, ns = a->signalSemaphoreInfoCount;
        ws[i] = calloc(nw ? nw : 1, sizeof **ws);
        wst[i] = calloc(nw ? nw : 1, sizeof **wst);
        wv[i] = calloc(nw ? nw : 1, sizeof **wv);
        cbs[i] = calloc(nc ? nc : 1, sizeof **cbs);
        ss[i] = calloc(ns ? ns : 1, sizeof **ss);
        sv[i] = calloc(ns ? ns : 1, sizeof **sv);
        if (!ws[i] || !wst[i] || !wv[i] || !cbs[i] || !ss[i] || !sv[i]) goto out;
        for (uint32_t k = 0; k < nw; k++) {
            ws[i][k] = a->pWaitSemaphoreInfos[k].semaphore;
            wst[i][k] = stage1(a->pWaitSemaphoreInfos[k].stageMask);
            wv[i][k] = a->pWaitSemaphoreInfos[k].value;
        }
        for (uint32_t k = 0; k < nc; k++) cbs[i][k] = a->pCommandBufferInfos[k].commandBuffer;
        for (uint32_t k = 0; k < ns; k++) {
            ss[i][k] = a->pSignalSemaphoreInfos[k].semaphore;
            sv[i][k] = a->pSignalSemaphoreInfos[k].value;
        }
        tl[i] = (VkTimelineSemaphoreSubmitInfo) { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = nw, .pWaitSemaphoreValues = wv[i], .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sv[i] };
        s1[i] = (VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl[i],
            .waitSemaphoreCount = nw, .pWaitSemaphores = ws[i], .pWaitDstStageMask = wst[i],
            .commandBufferCount = nc, .pCommandBuffers = cbs[i], .signalSemaphoreCount = ns, .pSignalSemaphores = ss[i] };
    }
    r = queue_submit(qh, n, s1, f);
out:
    for (uint32_t i = 0; i < n; i++) {
        if (ws) free(ws[i]);
        if (wst) free(wst[i]);
        if (wv) free(wv[i]);
        if (cbs) free(cbs[i]);
        if (ss) free(ss[i]);
        if (sv) free(sv[i]);
    }
    free(s1); free(tl); free(ws); free(ss); free(wst); free(wv); free(sv); free(cbs);
    return r;
}

// Event2：今の VkEvent と同じく、待つ所を仕切りに置き換える。仕切りは dependency info をそのまま使う（画像の layout の変更も含む）
static void VKAPI_CALL cmd_set_event2(VkCommandBuffer cbh, VkEvent ev, const VkDependencyInfo *d) { (void) d; ev_add((struct cb *) cbh, (struct event *) ev, 1); }
static void VKAPI_CALL cmd_reset_event2(VkCommandBuffer cbh, VkEvent ev, VkPipelineStageFlags2 st) { (void) st; ev_add((struct cb *) cbh, (struct event *) ev, 0); }
static void VKAPI_CALL cmd_wait_events2(VkCommandBuffer cbh, uint32_t n, const VkEvent *evs, const VkDependencyInfo *d) {
    (void) evs;
    for (uint32_t i = 0; i < n; i++) cmd_barrier2(cbh, &d[i]);
}

// 作る前にメモリの必要量を聞く：仮に作って聞いて消す
static void VKAPI_CALL dev_buffer_reqs(VkDevice dv, const VkDeviceBufferMemoryRequirements *i, VkMemoryRequirements2 *r) {
    VkBuffer b = VK_NULL_HANDLE;
    memset(&r->memoryRequirements, 0, sizeof r->memoryRequirements);
    if (create_buffer(dv, i->pCreateInfo, NULL, &b) == VK_SUCCESS) {
        buffer_reqs(dv, b, &r->memoryRequirements);
        destroy_buffer(dv, b, NULL);
    }
}
static void VKAPI_CALL dev_image_reqs(VkDevice dv, const VkDeviceImageMemoryRequirements *i, VkMemoryRequirements2 *r) {
    VkImage im = VK_NULL_HANDLE;
    memset(&r->memoryRequirements, 0, sizeof r->memoryRequirements);
    if (create_image(dv, i->pCreateInfo, NULL, &im) == VK_SUCCESS) {
        image_reqs(dv, im, &r->memoryRequirements);
        destroy_image(dv, im, NULL);
    }
}
static void VKAPI_CALL dev_image_sparse_reqs(VkDevice dv, const VkDeviceImageMemoryRequirements *i, uint32_t *n, VkSparseImageMemoryRequirements2 *r) {
    (void) dv; (void) i; (void) r;
    *n = 0;
}

// private data：アプリが物に付ける小さな値。ドライバの中だけで持つ
struct pdata { uint64_t obj; uint64_t slot; uint64_t val; struct pdata *next; };
static struct pdata *g_pdata;
static SRWLOCK g_pdata_lock = SRWLOCK_INIT;
static VkResult VKAPI_CALL create_pdslot(VkDevice dv, const VkPrivateDataSlotCreateInfo *ci, const VkAllocationCallbacks *a, VkPrivateDataSlot *out) {
    (void) dv; (void) ci; (void) a;
    void *p = calloc(1, 8);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkPrivateDataSlot) p;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_pdslot(VkDevice dv, VkPrivateDataSlot slot, const VkAllocationCallbacks *a) {
    (void) dv; (void) a;
    AcquireSRWLockExclusive(&g_pdata_lock);
    for (struct pdata **pp = &g_pdata; *pp;) {
        if ((*pp)->slot == ID(slot)) { struct pdata *x = *pp; *pp = x->next; free(x); } else pp = &(*pp)->next;
    }
    ReleaseSRWLockExclusive(&g_pdata_lock);
    free((void *) slot);
}
static VkResult VKAPI_CALL set_pdata(VkDevice dv, VkObjectType t, uint64_t obj, VkPrivateDataSlot slot, uint64_t v) {
    (void) dv; (void) t;
    AcquireSRWLockExclusive(&g_pdata_lock);
    struct pdata *e = g_pdata;
    while (e && !(e->obj == obj && e->slot == ID(slot))) e = e->next;
    if (!e && (e = calloc(1, sizeof *e))) { e->obj = obj; e->slot = ID(slot); e->next = g_pdata; g_pdata = e; }
    if (e) e->val = v;
    ReleaseSRWLockExclusive(&g_pdata_lock);
    return e ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void VKAPI_CALL get_pdata(VkDevice dv, VkObjectType t, uint64_t obj, VkPrivateDataSlot slot, uint64_t *v) {
    (void) dv; (void) t;
    *v = 0;
    AcquireSRWLockShared(&g_pdata_lock);
    for (struct pdata *e = g_pdata; e; e = e->next) if (e->obj == obj && e->slot == ID(slot)) { *v = e->val; break; }
    ReleaseSRWLockShared(&g_pdata_lock);
}

// まだ中継していない入口。呼ばれたら名前を出す（入口ごとに別の関数）
#define NOT_YET(X) \
    X(vkQueueBindSparse) X(vkGetImageSparseMemoryRequirements) X(vkGetImageSparseMemoryRequirements2) \
    X(vkGetDeviceGroupPeerMemoryFeatures) X(vkCmdSetDeviceMask) X(vkCreateSamplerYcbcrConversion) X(vkDestroySamplerYcbcrConversion) \
    X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount) X(vkGetBufferDeviceAddress) X(vkGetBufferOpaqueCaptureAddress) \
    X(vkGetDeviceMemoryOpaqueCaptureAddress) X(vkCmdExecuteCommands)
#define NY_FN(n) \
    static VkResult VKAPI_CALL ny_##n(void) { \
        say("まだ中継していない入口が呼ばれた：%s", #n); \
        return VK_ERROR_FEATURE_NOT_PRESENT; \
    }
NOT_YET(NY_FN)
#undef NY_FN

// ---- 入口の表 ----
static PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice d, const char *pName);

#define F(name, fn) if (!strcmp(pName, name)) return (PFN_vkVoidFunction) fn
static PFN_vkVoidFunction lookup_device(const char *pName) {
    F("vkGetDeviceProcAddr", get_device_proc);
    F("vkDestroyDevice", destroy_device);
    F("vkGetDeviceQueue", get_queue);
    F("vkGetDeviceQueue2", get_queue2);
    F("vkQueueSubmit", queue_submit);
    F("vkQueueWaitIdle", queue_wait_idle);
    F("vkDeviceWaitIdle", device_wait_idle);
    F("vkAllocateMemory", alloc_memory);
    F("vkFreeMemory", free_memory);
    F("vkMapMemory", map_memory);
    F("vkUnmapMemory", unmap_memory);
    F("vkFlushMappedMemoryRanges", flush_ranges);
    F("vkInvalidateMappedMemoryRanges", flush_ranges);
    F("vkGetDeviceMemoryCommitment", get_commitment);
    F("vkCreateBuffer", create_buffer);
    F("vkDestroyBuffer", destroy_buffer);
    F("vkGetBufferMemoryRequirements", buffer_reqs);
    F("vkGetBufferMemoryRequirements2", buffer_reqs2);
    F("vkGetBufferMemoryRequirements2KHR", buffer_reqs2);
    F("vkBindBufferMemory", bind_buffer);
    F("vkBindBufferMemory2", bind_buffer2);
    F("vkBindBufferMemory2KHR", bind_buffer2);
    F("vkCreateShaderModule", create_shader);
    F("vkDestroyShaderModule", destroy_shader);
    F("vkCreateDescriptorSetLayout", create_dsl);
    F("vkDestroyDescriptorSetLayout", destroy_dsl);
    F("vkGetDescriptorSetLayoutSupport", dsl_support);
    F("vkGetDescriptorSetLayoutSupportKHR", dsl_support);
    F("vkCreatePipelineLayout", create_playout);
    F("vkDestroyPipelineLayout", destroy_playout);
    F("vkCreatePipelineCache", create_pcache);
    F("vkDestroyPipelineCache", destroy_pcache);
    F("vkGetPipelineCacheData", pcache_data);
    F("vkMergePipelineCaches", merge_pcache);
    F("vkCreateComputePipelines", create_compute_pipelines);
    F("vkDestroyPipeline", destroy_pipeline);
    F("vkCreateDescriptorPool", create_dpool);
    F("vkDestroyDescriptorPool", destroy_dpool);
    F("vkResetDescriptorPool", reset_dpool);
    F("vkAllocateDescriptorSets", alloc_dsets);
    F("vkFreeDescriptorSets", free_dsets);
    F("vkUpdateDescriptorSets", update_dsets);
    F("vkCreateCommandPool", create_cpool);
    F("vkDestroyCommandPool", destroy_cpool);
    F("vkResetCommandPool", reset_cpool);
    F("vkTrimCommandPool", trim_cpool);
    F("vkTrimCommandPoolKHR", trim_cpool);
    F("vkAllocateCommandBuffers", alloc_cbs);
    F("vkFreeCommandBuffers", free_cbs);
    F("vkBeginCommandBuffer", begin_cb);
    F("vkEndCommandBuffer", end_cb);
    F("vkResetCommandBuffer", reset_cb);
    F("vkCmdBindPipeline", cmd_bind_pipeline);
    F("vkCmdBindDescriptorSets", cmd_bind_sets);
    F("vkCmdPushConstants", cmd_push);
    F("vkCmdDispatch", cmd_dispatch);
    F("vkCmdDispatchBase", cmd_dispatch_base);
    F("vkCmdDispatchBaseKHR", cmd_dispatch_base);
    F("vkCmdDispatchIndirect", cmd_dispatch_indirect);
    F("vkCmdCopyBuffer", cmd_copy);
    F("vkCmdFillBuffer", cmd_fill);
    F("vkCmdUpdateBuffer", cmd_update);
    F("vkCmdPipelineBarrier", cmd_barrier);
    F("vkCreateEvent", create_event);
    F("vkDestroyEvent", destroy_event);
    F("vkGetEventStatus", get_event_status);
    F("vkSetEvent", set_event);
    F("vkResetEvent", reset_event);
    F("vkCmdSetEvent", cmd_set_event);
    F("vkCmdResetEvent", cmd_reset_event);
    F("vkCmdWaitEvents", cmd_wait_events);
    F("vkCreateFence", create_fence);
    F("vkDestroyFence", destroy_fence);
    F("vkResetFences", reset_fences);
    F("vkWaitForFences", wait_fences);
    F("vkGetFenceStatus", fence_status);
    F("vkCreateSemaphore", create_semaphore);
    F("vkDestroySemaphore", destroy_semaphore);
    F("vkCreateSampler", create_sampler);
    F("vkDestroySampler", destroy_sampler);
    F("vkCreateSwapchainKHR", create_swapchain);
    F("vkDestroySwapchainKHR", destroy_swapchain);
    F("vkGetSwapchainImagesKHR", swapchain_images);
    F("vkAcquireNextImageKHR", acquire_image);
    F("vkAcquireNextImage2KHR", acquire_image2);
    F("vkQueuePresentKHR", queue_present);
    F("vkCreateImage", create_image);
    F("vkDestroyImage", destroy_image);
    F("vkGetImageMemoryRequirements", image_reqs);
    F("vkGetImageMemoryRequirements2", image_reqs2);
    F("vkGetImageMemoryRequirements2KHR", image_reqs2);
    F("vkBindImageMemory", bind_image);
    F("vkBindImageMemory2", bind_image2);
    F("vkBindImageMemory2KHR", bind_image2);
    F("vkCreateImageView", create_view);
    F("vkDestroyImageView", destroy_view);
    F("vkCreateRenderPass", create_rpass);
    F("vkDestroyRenderPass", destroy_rpass);
    F("vkGetRenderAreaGranularity", render_area_granularity);
    F("vkCreateFramebuffer", create_fb);
    F("vkDestroyFramebuffer", destroy_fb);
    F("vkCreateGraphicsPipelines", create_graphics_pipelines);
    F("vkCmdBeginRenderPass", cmd_begin_rp);
    F("vkCmdEndRenderPass", cmd_end_rp);
    F("vkCmdNextSubpass", cmd_next_subpass);
    F("vkCmdDraw", cmd_draw);
    F("vkCmdDrawIndexed", cmd_draw_indexed);
    F("vkCmdDrawIndirect", cmd_draw_indirect);
    F("vkCmdDrawIndexedIndirect", cmd_draw_indexed_indirect);
    F("vkCmdBindVertexBuffers", cmd_bind_vbs);
    F("vkCmdBindIndexBuffer", cmd_bind_ib);
    F("vkCmdSetViewport", cmd_set_viewport);
    F("vkCmdSetScissor", cmd_set_scissor);
    F("vkCmdCopyImageToBuffer", cmd_copy_image_to_buffer);
    F("vkCmdCopyBufferToImage", cmd_copy_buffer_to_image);
    F("vkCmdClearColorImage", cmd_clear_color);
    F("vkCmdCopyImage", cmd_copy_image);
    F("vkCmdBlitImage", cmd_blit_image);
    F("vkGetSemaphoreCounterValue", get_sem_counter);
    F("vkGetSemaphoreCounterValueKHR", get_sem_counter);
    F("vkWaitSemaphores", wait_sems);
    F("vkWaitSemaphoresKHR", wait_sems);
    F("vkSignalSemaphore", signal_sem);
    F("vkSignalSemaphoreKHR", signal_sem);
    // Vulkan 1.3 と、その元になった拡張の名前
    F("vkCreateRenderPass2", create_rpass2);
    F("vkCreateRenderPass2KHR", create_rpass2);
    F("vkCmdBeginRenderPass2", cmd_begin_rp2);
    F("vkCmdBeginRenderPass2KHR", cmd_begin_rp2);
    F("vkCmdNextSubpass2", cmd_next_subpass2);
    F("vkCmdNextSubpass2KHR", cmd_next_subpass2);
    F("vkCmdEndRenderPass2", cmd_end_rp2);
    F("vkCmdEndRenderPass2KHR", cmd_end_rp2);
    F("vkCmdExecuteCommands", cmd_exec);
    F("vkCmdClearDepthStencilImage", cmd_clear_ds);
    F("vkCmdClearAttachments", cmd_clear_atts);
    F("vkCmdCopyBuffer2", cmd_copy2);
    F("vkCmdCopyBuffer2KHR", cmd_copy2);
    F("vkCmdCopyImage2", cmd_copy_image2);
    F("vkCmdCopyImage2KHR", cmd_copy_image2);
    F("vkCmdCopyBufferToImage2", cmd_copy_b2i2);
    F("vkCmdCopyBufferToImage2KHR", cmd_copy_b2i2);
    F("vkCmdCopyImageToBuffer2", cmd_copy_i2b2);
    F("vkCmdCopyImageToBuffer2KHR", cmd_copy_i2b2);
    F("vkCmdBlitImage2", cmd_blit2);
    F("vkCmdBlitImage2KHR", cmd_blit2);
    F("vkCmdResolveImage", cmd_resolve);
    F("vkCmdResolveImage2", cmd_resolve2);
    F("vkCmdResolveImage2KHR", cmd_resolve2);
    F("vkCmdSetLineWidth", cmd_set_line_width);
    F("vkCmdSetDepthBias", cmd_set_depth_bias);
    F("vkCmdSetBlendConstants", cmd_set_blend_constants);
    F("vkCmdSetDepthBounds", cmd_set_depth_bounds);
    F("vkCmdSetStencilCompareMask", cmd_set_stencil_compare);
    F("vkCmdSetStencilWriteMask", cmd_set_stencil_write);
    F("vkCmdSetStencilReference", cmd_set_stencil_ref);
    F("vkCmdSetCullMode", cmd_set_cull_mode);
    F("vkCmdSetCullModeEXT", cmd_set_cull_mode);
    F("vkCmdSetFrontFace", cmd_set_front_face);
    F("vkCmdSetFrontFaceEXT", cmd_set_front_face);
    F("vkCmdSetPrimitiveTopology", cmd_set_topology);
    F("vkCmdSetPrimitiveTopologyEXT", cmd_set_topology);
    F("vkCmdSetDepthTestEnable", cmd_set_depth_test);
    F("vkCmdSetDepthTestEnableEXT", cmd_set_depth_test);
    F("vkCmdSetDepthWriteEnable", cmd_set_depth_write);
    F("vkCmdSetDepthWriteEnableEXT", cmd_set_depth_write);
    F("vkCmdSetDepthCompareOp", cmd_set_depth_compare);
    F("vkCmdSetDepthCompareOpEXT", cmd_set_depth_compare);
    F("vkCmdSetDepthBoundsTestEnable", cmd_set_depth_bounds_test);
    F("vkCmdSetDepthBoundsTestEnableEXT", cmd_set_depth_bounds_test);
    F("vkCmdSetStencilTestEnable", cmd_set_stencil_test);
    F("vkCmdSetStencilTestEnableEXT", cmd_set_stencil_test);
    F("vkCmdSetRasterizerDiscardEnable", cmd_set_raster_discard);
    F("vkCmdSetRasterizerDiscardEnableEXT", cmd_set_raster_discard);
    F("vkCmdSetDepthBiasEnable", cmd_set_depth_bias_enable);
    F("vkCmdSetDepthBiasEnableEXT", cmd_set_depth_bias_enable);
    F("vkCmdSetPrimitiveRestartEnable", cmd_set_restart);
    F("vkCmdSetPrimitiveRestartEnableEXT", cmd_set_restart);
    F("vkCmdSetStencilOp", cmd_set_stencil_op);
    F("vkCmdSetStencilOpEXT", cmd_set_stencil_op);
    F("vkCmdSetViewportWithCount", cmd_set_viewport_count);
    F("vkCmdSetViewportWithCountEXT", cmd_set_viewport_count);
    F("vkCmdSetScissorWithCount", cmd_set_scissor_count);
    F("vkCmdSetScissorWithCountEXT", cmd_set_scissor_count);
    F("vkCmdBindVertexBuffers2", cmd_bind_vbs2);
    F("vkCmdBindVertexBuffers2EXT", cmd_bind_vbs2);
    F("vkCmdBeginRendering", cmd_begin_rendering);
    F("vkCmdBeginRenderingKHR", cmd_begin_rendering);
    F("vkCmdEndRendering", cmd_end_rendering);
    F("vkCmdEndRenderingKHR", cmd_end_rendering);
    F("vkCmdPipelineBarrier2", cmd_barrier2);
    F("vkCmdPipelineBarrier2KHR", cmd_barrier2);
    F("vkQueueSubmit2", queue_submit2);
    F("vkQueueSubmit2KHR", queue_submit2);
    F("vkCmdSetEvent2", cmd_set_event2);
    F("vkCmdSetEvent2KHR", cmd_set_event2);
    F("vkCmdResetEvent2", cmd_reset_event2);
    F("vkCmdResetEvent2KHR", cmd_reset_event2);
    F("vkCmdWaitEvents2", cmd_wait_events2);
    F("vkCmdWaitEvents2KHR", cmd_wait_events2);
    F("vkGetDeviceBufferMemoryRequirements", dev_buffer_reqs);
    F("vkGetDeviceBufferMemoryRequirementsKHR", dev_buffer_reqs);
    F("vkGetDeviceImageMemoryRequirements", dev_image_reqs);
    F("vkGetDeviceImageMemoryRequirementsKHR", dev_image_reqs);
    F("vkGetDeviceImageSparseMemoryRequirements", dev_image_sparse_reqs);
    F("vkGetDeviceImageSparseMemoryRequirementsKHR", dev_image_sparse_reqs);
    F("vkCreateQueryPool", create_qpool);
    F("vkDestroyQueryPool", destroy_qpool);
    F("vkGetQueryPoolResults", get_qpool_results);
    F("vkResetQueryPool", reset_qpool);
    F("vkResetQueryPoolEXT", reset_qpool);
    F("vkCmdResetQueryPool", cmd_reset_qpool);
    F("vkCmdBeginQuery", cmd_begin_query);
    F("vkCmdEndQuery", cmd_end_query);
    F("vkCmdWriteTimestamp", cmd_write_timestamp);
    F("vkCmdWriteTimestamp2", cmd_write_timestamp2);
    F("vkCmdWriteTimestamp2KHR", cmd_write_timestamp2);
    F("vkCmdCopyQueryPoolResults", cmd_copy_qpool_results);
    F("vkCreatePrivateDataSlot", create_pdslot);
    F("vkCreatePrivateDataSlotEXT", create_pdslot);
    F("vkDestroyPrivateDataSlot", destroy_pdslot);
    F("vkDestroyPrivateDataSlotEXT", destroy_pdslot);
    F("vkSetPrivateData", set_pdata);
    F("vkSetPrivateDataEXT", set_pdata);
    F("vkGetPrivateData", get_pdata);
    F("vkGetPrivateDataEXT", get_pdata);
    F("vkGetBufferDeviceAddress", get_buffer_address);
    F("vkGetBufferDeviceAddressKHR", get_buffer_address);
    F("vkGetBufferDeviceAddressEXT", get_buffer_address);
    F("vkGetBufferOpaqueCaptureAddress", get_buffer_capture_address);
    F("vkGetBufferOpaqueCaptureAddressKHR", get_buffer_capture_address);
    F("vkGetDeviceMemoryOpaqueCaptureAddress", get_memory_capture_address);
    F("vkGetDeviceMemoryOpaqueCaptureAddressKHR", get_memory_capture_address);
    F("vkCmdBindDescriptorSets2", cmd_bind_sets2);
    F("vkCmdBindDescriptorSets2KHR", cmd_bind_sets2);
    F("vkCmdPushConstants2", cmd_push2);
    F("vkCmdPushConstants2KHR", cmd_push2);
    F("vkCmdBindIndexBuffer2", cmd_bind_ib2);
    F("vkCmdBindIndexBuffer2KHR", cmd_bind_ib2);
    F("vkGetRenderingAreaGranularity", rendering_area_granularity);
    F("vkGetRenderingAreaGranularityKHR", rendering_area_granularity);
    F("vkGetImageSubresourceLayout2", get_subres_layout2);
    F("vkGetImageSubresourceLayout2KHR", get_subres_layout2);
    F("vkGetImageSubresourceLayout2EXT", get_subres_layout2);
    F("vkGetDeviceImageSubresourceLayout", get_device_image_subres_layout);
    F("vkGetDeviceImageSubresourceLayoutKHR", get_device_image_subres_layout);
    F("vkCreateBufferView", create_bview);
    F("vkDestroyBufferView", destroy_bview);
    F("vkGetImageSubresourceLayout", get_subres_layout);
    F("vkCreateDescriptorUpdateTemplate", create_dtmpl);
    F("vkCreateDescriptorUpdateTemplateKHR", create_dtmpl);
    F("vkDestroyDescriptorUpdateTemplate", destroy_dtmpl);
    F("vkDestroyDescriptorUpdateTemplateKHR", destroy_dtmpl);
    F("vkUpdateDescriptorSetWithTemplate", update_dset_tmpl);
    F("vkUpdateDescriptorSetWithTemplateKHR", update_dset_tmpl);
    F("vkCmdPushDescriptorSetKHR", cmd_push_desc);
    F("vkCmdPushDescriptorSet", cmd_push_desc);
    F("vkCmdPushDescriptorSetWithTemplateKHR", cmd_push_desc_tmpl);
    F("vkCmdPushDescriptorSetWithTemplate", cmd_push_desc_tmpl);
    F("vkCmdPushDescriptorSet2KHR", cmd_push_desc2);
    F("vkCmdPushDescriptorSet2", cmd_push_desc2);
    F("vkCmdPushDescriptorSetWithTemplate2KHR", cmd_push_desc_tmpl2);
    F("vkCmdPushDescriptorSetWithTemplate2", cmd_push_desc_tmpl2);
#define NY_F(n) F(#n, ny_##n);
    NOT_YET(NY_F)
#undef NY_F
    return NULL;
}

static PFN_vkVoidFunction lookup_phys(const char *pName) {
    F("vkGetPhysicalDeviceToolProperties", get_tool_props);
    F("vkGetPhysicalDeviceToolPropertiesEXT", get_tool_props);
    F("vkGetPhysicalDeviceProperties", get_props);
    F("vkGetPhysicalDeviceProperties2", get_props2);
    F("vkGetPhysicalDeviceProperties2KHR", get_props2);
    F("vkGetPhysicalDeviceFeatures", get_features);
    F("vkGetPhysicalDeviceFeatures2", get_features2);
    F("vkGetPhysicalDeviceFeatures2KHR", get_features2);
    F("vkGetPhysicalDeviceQueueFamilyProperties", get_queue_families);
    F("vkGetPhysicalDeviceQueueFamilyProperties2", get_queue_families2);
    F("vkGetPhysicalDeviceQueueFamilyProperties2KHR", get_queue_families2);
    F("vkGetPhysicalDeviceMemoryProperties", get_mem);
    F("vkGetPhysicalDeviceMemoryProperties2", get_mem2);
    F("vkGetPhysicalDeviceMemoryProperties2KHR", get_mem2);
    F("vkGetPhysicalDeviceFormatProperties", get_format);
    F("vkGetPhysicalDeviceFormatProperties2", get_format2);
    F("vkGetPhysicalDeviceFormatProperties2KHR", get_format2);
    F("vkGetPhysicalDeviceImageFormatProperties", get_image_format);
    F("vkGetPhysicalDeviceImageFormatProperties2", get_image_format2);
    F("vkGetPhysicalDeviceImageFormatProperties2KHR", get_image_format2);
    F("vkGetPhysicalDeviceSparseImageFormatProperties", get_sparse_format);
    F("vkGetPhysicalDeviceSparseImageFormatProperties2", get_sparse_format2);
    F("vkGetPhysicalDeviceSparseImageFormatProperties2KHR", get_sparse_format2);
    F("vkGetPhysicalDeviceExternalBufferProperties", get_ext_buffer);
    F("vkGetPhysicalDeviceExternalFenceProperties", get_ext_fence);
    F("vkGetPhysicalDeviceExternalSemaphoreProperties", get_ext_sem);
    F("vkEnumerateDeviceExtensionProperties", enum_device_ext);
    F("vkEnumerateDeviceLayerProperties", enum_device_layers);
    F("vkCreateDevice", create_device);
    F("vkGetPhysicalDeviceSurfaceSupportKHR", surface_support);
    F("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", surface_caps);
    F("vkGetPhysicalDeviceSurfaceFormatsKHR", surface_formats);
    F("vkGetPhysicalDeviceSurfacePresentModesKHR", surface_modes);
    F("vkGetPhysicalDeviceWin32PresentationSupportKHR", win32_presentation_support);
    return NULL;
}

static PFN_vkVoidFunction lookup(const char *pName) {
    F("vkCreateInstance", create_instance);
    F("vkDestroyInstance", destroy_instance);
    F("vkEnumerateInstanceExtensionProperties", enum_instance_ext);
    F("vkEnumerateInstanceVersion", enum_instance_version);
    F("vkEnumeratePhysicalDevices", enum_phys);
    F("vkCreateWin32SurfaceKHR", create_win32_surface);
    F("vkDestroySurfaceKHR", destroy_surface);
    PFN_vkVoidFunction f = lookup_phys(pName);
    return f ? f : lookup_device(pName);
}
#undef F

static PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice d, const char *pName) {
    (void) d;
    PFN_vkVoidFunction f = lookup_device(pName);
    if (!f) say("まだ無い入口：%s", pName);
    return f;
}

EXPORT VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) {
    if (*v > 5) *v = 5;
    return VK_SUCCESS;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance i, const char *pName) {
    (void) i;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction) vk_icdGetInstanceProcAddr;
    PFN_vkVoidFunction f = lookup(pName);
    if (!f && strstr(pName, "Surface")) say("インスタンスの入口が無い：%s", pName);
    return f;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance i, const char *pName) {
    (void) i;
    return lookup_phys(pName);
}
