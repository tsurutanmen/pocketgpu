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
//   storage buffer と fill・update の所は、CPU が触ったときに初めて引き取る（IPHONE_VK_LAZY_RB・本体は lz_ の関数）
//
// 待ち方（取り決めの版 3）
//   持ち帰る物が無い待ち（fence・timeline semaphore・idle）には、往復せずにその場で「終わった」と答える。
//   受け手は、出した仕事が使っているかもしれない物を変える命令の前に、自分の中で仕事の終わりを待つ（drain）。
//   持ち帰る物があるときは、MEM_READ の返事が終わりの知らせを兼ねる（受け手は読む前に drain する）。
//   往復は「GPU の答えを持ち帰る」ときだけになる
//
// Linux 版は、窓（swapchain）・動画・書き込み監視・読み戻しの遅延を除いた、計算と「描いて読み戻す」所だけ動く（#ifdef _WIN32 で分けている）。
// 窓の無い Vulkan のアプリが、TCP で受け手（server.c）につないで使う。つなぎ先は IPHONE_VK=host:port
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "../remote/proto.h"
#include "../remote/wire.h"
#ifdef _WIN32
#include <windows.h>
#define COBJMACROS
#include <wincodec.h>
#endif
#include "../remote/video.h"
#ifdef _WIN32
#include <d3d11.h>
#include <d3d10.h>
#include <dbghelp.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#define LINUX_UNUSED
#define PATH_SEP "\\"
#else
#include "plat_posix.h"
#endif
#define PAGE 4096u
// handle → 受け手と取り決めた 64 ビットの番号。32 ビットでは non-dispatchable の handle は uint64_t なので、uintptr_t を通すと上の桁が消える
#if UINTPTR_MAX == 0xffffffffu
#define ID(h) ((uint64_t) (h))
#else
#define ID(h) ((uint64_t) (uintptr_t) (h))
#endif

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
#ifdef _WIN32
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

static void print_stats_fwd(void);
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
    { if (getenv("IPHONE_VK_STATS")) print_stats_fwd(); }
    return EXCEPTION_CONTINUE_SEARCH;
}

__attribute__((constructor)) static void install_crash_handler(void) {
    // 調べ用：IPHONE_VK_ERR_FILE=パス で、stderr（記録・統計）を「パス.プロセス番号.txt」に向ける（Chrome の GPU プロセスは stderr を持たない）
    const char *ef = getenv("IPHONE_VK_ERR_FILE");
    if (ef && *ef) {
        char path[600];
        snprintf(path, sizeof path, "%s.%lu.txt", ef, (unsigned long) GetCurrentProcessId());
        if (freopen(path, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
    const char *e = getenv("IPHONE_VK_LOG");
    if (e && *e && *e != '0') AddVectoredExceptionHandler(1, crash_handler);
}
#else
// Linux：シグナルの中で呼び出し元を書き出し、元の動作（コアダンプなど）に戻して投げ直す。名前は -g と -rdynamic があれば出る
static void crash_signal(int sig, siginfo_t *si, void *uc) {
    (void) uc;
    (void) si;
    static const char head[] = "[iphone-vk] 落ちた：シグナル ";
    char num[16];
    int n = 0, v = sig;
    do { num[n++] = (char) ('0' + v % 10); v /= 10; } while (v && n < 15);
    ssize_t r = write(2, head, sizeof head - 1);
    for (int i = n - 1; i >= 0; i--) r = write(2, &num[i], 1);
    r = write(2, "\n", 1);
    (void) r;
    void *fr[40];
    int k = backtrace(fr, 40);
    backtrace_symbols_fd(fr, k, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((constructor)) static void install_crash_handler(void) {
    const char *e = getenv("IPHONE_VK_LOG");
    if (!(e && *e && *e != '0')) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
#endif

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
    [VR_READ_H264] = "READ_H264", [VR_GET_PROGRESS] = "GET_PROGRESS",
};
static struct {
    uint64_t sends[VR_OP_COUNT], calls[VR_OP_COUNT], bytes[VR_OP_COUNT];
    double wait_ms[VR_OP_COUNT];
    uint64_t bytes_in, flushes;
    uint64_t wire_out;             // 線に実際に出たバイト（LZ4 のあと）
    double send_ms;                // 線に書き出す（send）のにかかった時間
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
    uint64_t wr_bytes[6], wr_n[6]; // GPU が書くかもしれない CPU から見える範囲（読み戻す元）を理由別に
    double cpu0;                   // 最初の present のときのプロセスの CPU 時間（ms）
    uint64_t rp_begin, rp_load, rp_store, rp_clear, rp_dload, rp_dstore, rp_same; // render pass を始めた回数と、画面を読み込む・書き戻す・消す attachment の数
    double rp_px;                  // 読み込みと書き戻しの画素の合計（タイルで描く GPU ではここが重い）
} g_stat;
// u1（アドレスで読み書きする buffer・結んだあとに書いた descriptor）の数。数えるだけ
static struct { volatile LONG64 pv, rd_n, wr_n, whole, ub_sets, ub_ents; } g_u1;
static int g_stats = -1;
// 試す人に配るまとめの印。pc/make_kit.py が「#」の 32 文字を人ごとの番号に書き換える（長さは変えない）。
// 配った物が外に出たとき、誰に渡した分かを見分ける。ふだん組んだ物は「#」のまま
#ifdef __GNUC__
__attribute__((used))
#endif
static const volatile char g_kit_mark[] = "PGKIT-MARK:################################";

#ifdef _WIN32
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}
#endif

static double proc_cpu_ms(void);

static void lz_print(void);

static void ls_print(void);
static inline int ls_on(void);
static volatile long g_lso_n[2];
static int g_lso_fix;
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
    {
        static const char *why[6] = { "storage buffer", "buffer への写し", "fill・update", "画像から buffer へ", "query の結果", "そのほか" };
        uint64_t any = 0;
        for (int k = 0; k < 6; k++) any += g_stat.wr_n[k];
        if (any) {
            fprintf(stderr, "  GPU が書くかもしれない範囲（記録した回数・MB）：");
            for (int k = 0; k < 6; k++)
                if (g_stat.wr_n[k]) fprintf(stderr, "%s %llu 回 %.1f MB・", why[k], (unsigned long long) g_stat.wr_n[k], g_stat.wr_bytes[k] / 1048576.0);
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "  書き込み監視 %llu 回 %.1f ms 書いたページ %llu / 引き取り %.1f ms\n", (unsigned long long) g_stat.ww_calls, g_stat.ww_ms,
            (unsigned long long) g_stat.ww_pages, g_stat.rb_ms);
    if (g_u1.pv || g_u1.ub_sets || g_u1.whole)
        fprintf(stderr, "  u1：push constant のアドレス %lld 回・submit で足した 読む範囲 %lld・書く範囲 %lld・メモリ全体に広げた submit %lld・"
                        "結んだあとに書かれた set %lld 回（descriptor %lld 個）\n",
                (long long) g_u1.pv, (long long) g_u1.rd_n, (long long) g_u1.wr_n, (long long) g_u1.whole, (long long) g_u1.ub_sets, (long long) g_u1.ub_ents);
    lz_print();
    if (g_stat.rp_begin) {
        double per = g_stat.presents ? (double) g_stat.presents : 1;
        fprintf(stderr, "  描く区切り（render pass）%llu 回（1 コマ %.1f 回）・読み込む attachment %llu・書き戻す %llu・消して始める %llu・"
                        "（うち深さ 読み込み %llu・書き戻し %llu）・すぐ前と同じ framebuffer に続けて %llu 回・読み書きした画素 1 コマ %.1f M\n", (unsigned long long) g_stat.rp_begin, g_stat.rp_begin / per,
                (unsigned long long) g_stat.rp_load, (unsigned long long) g_stat.rp_store, (unsigned long long) g_stat.rp_clear,
                (unsigned long long) g_stat.rp_dload, (unsigned long long) g_stat.rp_dstore, (unsigned long long) g_stat.rp_same, g_stat.rp_px / per / 1e6);
    }
    if (ls_on()) ls_print();
    if (g_lso_n[0] || g_lso_n[1]) fprintf(stderr, "  load_store_op_none：アプリが渡した NONE は LOAD %ld 個・STORE %ld 個（%s）\n", g_lso_n[0], g_lso_n[1], g_lso_fix ? "LOAD・STORE に直して送った" : "そのまま送った");
    fprintf(stderr, "  往復せずに済ませた待ち %llu\n", (unsigned long long) g_stat.skipped);
    if (g_stat.presents)
        fprintf(stderr, "  present %llu 回 %.1f ms（うち窓に描く %llu 回 %.1f ms）\n", (unsigned long long) g_stat.presents, g_stat.present_ms,
                (unsigned long long) g_stat.blits, g_stat.blit_ms);
    if (g_stat.heat_max) fprintf(stderr, "  iPhone の熱：いちばん熱かったのは %d（0 ふつう〜3 とても熱い）\n", g_stat.heat_max);
    if (g_kit_mark[11] != '#') fprintf(stderr, "  配った印：%.32s\n", (const char *) g_kit_mark + 11);
    if (g_stat.presents > 1)
        fprintf(stderr, "  最初の present からのプロセスの CPU %.0f ms（present 1 回 %.2f ms）\n", proc_cpu_ms() - g_stat.cpu0,
                (proc_cpu_ms() - g_stat.cpu0) / (double) (g_stat.presents - 1));
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

static void print_stats_fwd(void) { print_stats(); }

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
#ifndef _WIN32
#define LZ4LIB_VISIBILITY // LZ4_* を .so の外に出さない（アプリが読んでいる別の liblz4 と取り違えない）
#endif
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

// 調べ用：buffer から image への写し（vkCmdCopyBufferToImage）を、形式と大きさごとに数える（IPHONE_VK_STATS_EVERY のときに毎回出す）
static struct { uint32_t fmt, w, h; uint64_t n; } g_b2i[24];
static SRWLOCK g_b2i_lock = SRWLOCK_INIT;
static void b2i_note(uint32_t fmt, uint32_t w, uint32_t h) {
    AcquireSRWLockExclusive(&g_b2i_lock);
    int k = -1;
    for (int i = 0; i < 24; i++)
        if (g_b2i[i].n && g_b2i[i].fmt == fmt && g_b2i[i].w == w && g_b2i[i].h == h) { k = i; break; }
    if (k < 0)
        for (int i = 0; i < 24; i++)
            if (!g_b2i[i].n) { k = i; g_b2i[i].fmt = fmt; g_b2i[i].w = w; g_b2i[i].h = h; break; }
    if (k >= 0) g_b2i[k].n++;
    ReleaseSRWLockExclusive(&g_b2i_lock);
}

// 調べ用：IPHONE_VK_STATS_EVERY=秒 で、その秒ごとに「毎秒どれだけ送ったか」を記録に出す（Chrome のように最後まで生きていない物の測りに）
static void stat_tick(void) {
    static double every = -1, last_t;
    static uint64_t l_bytes[VR_OP_COUNT], l_wire, l_in, l_pin, l_pout, l_flush, l_calls[VR_OP_COUNT];
    static double l_wait[VR_OP_COUNT], l_send, l_packms;
    if (every == 0) return;
    if (every < 0) {
        const char *e = getenv("IPHONE_VK_STATS_EVERY");
        every = e ? atof(e) : 0;
        last_t = now_ms();
        if (every <= 0) { every = 0; return; }
    }
    double t = now_ms();
    if (t - last_t < every * 1000.0) return;
    double dt = (t - last_t) / 1000.0;
    uint64_t tot = 0, best[3] = { 0, 0, 0 }; int bi[3] = { 0, 0, 0 };
    for (int i = 1; i < VR_OP_COUNT; i++) {
        uint64_t d = g_stat.bytes[i] - l_bytes[i];
        tot += d;
        for (int k = 0; k < 3; k++)
            if (d > best[k]) {
                for (int j = 2; j > k; j--) { best[j] = best[j - 1]; bi[j] = bi[j - 1]; }
                best[k] = d; bi[k] = i; break;
            }
        l_bytes[i] = g_stat.bytes[i];
    }
    fprintf(stderr, "[iphone-vk] 毎秒（%.1f 秒）：命令 %.1f MB・線に出た %.1f MB（LZ4 の前→後 %.1f→%.1f）・受け取った %.1f MB・送った回数 %.0f", dt, tot / dt / 1e6,
            (g_stat.wire_out - l_wire) / dt / 1e6, (g_stat.pack_in - l_pin) / dt / 1e6, (g_stat.pack_out - l_pout) / dt / 1e6,
            (g_stat.bytes_in - l_in) / dt / 1e6, (g_stat.flushes - l_flush) / dt);
    for (int k = 0; k < 3; k++)
        if (best[k]) fprintf(stderr, "・%s %.1f MB", OP_NAME[bi[k]] ? OP_NAME[bi[k]] : "?", best[k] / dt / 1e6);
    fprintf(stderr, "\n");
    {
        // 返事を待った往復と、その待ち（毎秒）。待ちの長い上位 4 つ
        double wsum = 0, wms[4] = { 0, 0, 0, 0 };
        uint64_t csum = 0, wc[4] = { 0, 0, 0, 0 };
        int wi[4] = { 0, 0, 0, 0 };
        for (int i = 1; i < VR_OP_COUNT; i++) {
            double w = g_stat.wait_ms[i] - l_wait[i];
            uint64_t c = g_stat.calls[i] - l_calls[i];
            wsum += w; csum += c;
            for (int k = 0; k < 4; k++)
                if (w > wms[k]) {
                    for (int j = 3; j > k; j--) { wms[j] = wms[j - 1]; wi[j] = wi[j - 1]; wc[j] = wc[j - 1]; }
                    wms[k] = w; wi[k] = i; wc[k] = c; break;
                }
            l_wait[i] = g_stat.wait_ms[i]; l_calls[i] = g_stat.calls[i];
        }
        fprintf(stderr, "[iphone-vk]   往復 毎秒 %.0f 回・待ち 毎秒 %.0f ms（send %.0f ms・LZ4 %.0f ms）：", csum / dt, wsum / dt,
                (g_stat.send_ms - l_send) / dt, (g_stat.pack_ms - l_packms) / dt);
        for (int k = 0; k < 4; k++)
            if (wms[k] > 0) fprintf(stderr, " %s %.0f回 %.0fms", OP_NAME[wi[k]] ? OP_NAME[wi[k]] : "?", wc[k] / dt, wms[k] / dt);
        fprintf(stderr, "\n");
        l_send = g_stat.send_ms; l_packms = g_stat.pack_ms;
    }
    {
        AcquireSRWLockExclusive(&g_b2i_lock);
        int any = 0;
        for (int i = 0; i < 24; i++)
            if (g_b2i[i].n > (uint64_t) dt) { /* 毎秒 1 回以上のものだけ */
                if (!any) fprintf(stderr, "[iphone-vk]   buffer→image の写し（毎秒の回数）：");
                any = 1;
                fprintf(stderr, " fmt%u %ux%u ×%.1f", g_b2i[i].fmt, g_b2i[i].w, g_b2i[i].h, g_b2i[i].n / dt);
            }
        if (any) fprintf(stderr, "\n");
        memset(g_b2i, 0, sizeof g_b2i);
        ReleaseSRWLockExclusive(&g_b2i_lock);
    }
    l_wire = g_stat.wire_out; l_in = g_stat.bytes_in; l_pin = g_stat.pack_in; l_pout = g_stat.pack_out; l_flush = g_stat.flushes;
    last_t = t;
}

static int flush_locked(void) {
    if (g_dead || g_sock == VR_BAD_SOCK) { g_q.n = 0; return 0; }
    stat_tick();
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
            g_stat.wire_out += (uint64_t) cl + 12;
            double ts0 = now_ms();
            if (!vr_send_all(g_sock, pk.p, (size_t) cl + 12)) conn_lost("送る所");
            g_stat.send_ms += now_ms() - ts0;
            g_q.n = 0;
            return !g_dead;
        }
    }
    g_stat.wire_out += g_q.n;
    double ts1 = now_ms();
    if (g_q.n && !vr_send_all(g_sock, g_q.p, g_q.n)) conn_lost("送る所");
    g_stat.send_ms += now_ms() - ts1;
    g_q.n = 0;
    return !g_dead;
}

static void put_msg_locked(uint32_t op, const vr_buf *b) {
    uint32_t h[2] = { op, (uint32_t) b->n };
    if (op < VR_OP_COUNT) g_stat.bytes[op] += 8 + b->n;
    vr_put(&g_q, h, 8);
    vr_put(&g_q, b->p, b->n);
}

// 送った VR_QUEUE_SUBMIT の数。受け手も届いた順に数えるので、何番目の submit かで話が通じる
static uint64_t g_submit_msgs;

// query 1 個の読みが「まだ（VK_NOT_READY）」だったら、その query を最後に書いた submit の番号（wmsg）と、
// それまでに送った submit の数が変わらないあいだは、読み直しても答えは変わらない
// （受け手は wmsg の submit までを待ち切ってから読んでおり、それより後の submit はその query を書かないから）。
// 結果を待ち続けるアプリ（Unreal Engine は 0.5 秒まで回り続ける）の読み直しを、往復せずに答える。
// 往復のたびに g_lock を持ち続けるので、前は回り続ける糸が g_lock を離さず、
// 「その query を書く submit」を送る糸が錠を取れずに 0.5 秒待たされていた（1 コマに数回）。IPHONE_VK_QUERY_NR_CACHE=0 で切る
// 覚える所は (pool, query) の組で 4096 個に散らす。前は 16 個を順に使い回し、Unreal のように多くの query を読み回すアプリでは
// 覚えがすぐ追い出され、実機で 1 分に 6 万回近く往復していた（1 回約 1 ms）。IPHONE_VK_QUERY_NR_SLOTS=16 で覚えを 16 個に減らせる
#define QNR_MAX 4096
static struct { uint64_t qp, wmsg, nsub; uint32_t q; } g_qnr[QNR_MAX];
static uint64_t g_qnr_miss_none, g_qnr_miss_sub, g_qnr_miss_w; // 往復した理由：覚えが無い・submit が増えた・書き手が変わった
static unsigned qnr_slots(void) {
    static unsigned v;
    if (!v) { const char *e = getenv("IPHONE_VK_QUERY_NR_SLOTS"); v = e && atoi(e) > 0 && atoi(e) <= QNR_MAX ? (unsigned) atoi(e) : QNR_MAX; }
    return v;
}
static unsigned qnr_slot(uint64_t qp, uint32_t q) {
    uint64_t h = (qp ^ ((uint64_t) q * 0x9E3779B97F4A7C15ull)) * 0xBF58476D1CE4E5B9ull;
    return (unsigned) ((h >> 32) % qnr_slots());
}
static SRWLOCK g_qnr_lock = SRWLOCK_INIT;
static uint64_t g_qnr_hits; // 往復せずに「まだ」と答えた回数
static void qnr_print(void) {
    if (g_qnr_hits) fprintf(stderr, "[iphone-vk] query の「まだ」を往復せずに答えた回数 %llu\n", (unsigned long long) g_qnr_hits);
    fprintf(stderr, "[iphone-vk] query を往復して読んだ理由：覚えが無い %llu・submit が増えた %llu・書き手が変わった %llu\n",
            (unsigned long long) g_qnr_miss_none, (unsigned long long) g_qnr_miss_sub, (unsigned long long) g_qnr_miss_w);
}
static int qnr_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_QUERY_NR_CACHE"); v = !(e && *e == '0'); }
    return v;
}
static int qnr_hit(uint64_t qp, uint32_t q, uint64_t wmsg, uint64_t nsub) {
    int hit = 0;
    unsigned k = qnr_slot(qp, q);
    AcquireSRWLockShared(&g_qnr_lock);
    if (g_qnr[k].qp != qp || g_qnr[k].q != q) g_qnr_miss_none++;
    else if (g_qnr[k].wmsg != wmsg) g_qnr_miss_w++;
    else if (g_qnr[k].nsub != nsub) g_qnr_miss_sub++;
    else hit = 1;
    ReleaseSRWLockShared(&g_qnr_lock);
    return hit;
}
static void qnr_put(uint64_t qp, uint32_t q, uint64_t wmsg, uint64_t nsub) {
    static LONG reg;
    if (!InterlockedExchange(&reg, 1)) {
        const char *e = getenv("IPHONE_VK_STATS");
        if (e && *e && *e != '0') atexit(qnr_print);
    }
    AcquireSRWLockExclusive(&g_qnr_lock);
    unsigned k = qnr_slot(qp, q);
    g_qnr[k].qp = qp;
    g_qnr[k].q = q;
    g_qnr[k].wmsg = wmsg;
    g_qnr[k].nsub = nsub;
    ReleaseSRWLockExclusive(&g_qnr_lock);
}

// 送りっぱなし（ためておいて、返事の要る命令や submit のときにまとめて送る）
static void rpc_send(uint32_t op, vr_buf *b) {
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.sends[op]++;
    if (op == VR_QUEUE_SUBMIT) g_submit_msgs++;
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
static struct { int active, ready; VkResult r; vr_buf out; uint32_t op; } g_defer; // op：頼んだ命令（数えるため）

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

#ifndef _WIN32
// 窓の無い Linux 版は、帯を出す窓が無い（記録だけ）
static void banner(const wchar_t *t) { (void) t; }
#else
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

#endif

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
#ifdef _WIN32
        if (g_stall_hwnd) InvalidateRect(g_stall_hwnd, NULL, FALSE);
#endif
    }
    if (!ok) conn_lost("返事を待っている所");
    // 返事の長さは受け手の言うまま確保するので、ありえない大きさなら切る（読み出しは 64 MB ずつ・窓の絵は数 MB）
    if (ok && h[1] > (512u << 20)) {
        say("受け手の返事が大きすぎる（%u バイト）", h[1]);
        conn_lost("返事の長さ");
        ok = 0;
    }
    return ok;
}

// まだ読んでいない返事の列（頼んだ順）。窓の絵（PEND_DEFER）と、錠を持たずに待つ進み具合の問い合わせ（PEND_PROGRESS）。
// 返事は頼んだ順にしか届かないので、ほかの返事を読む所は、先にこの列を全部読む（read_deferred_locked）。
// 前は進み具合の問い合わせを rpc_call で錠を持ったまま待ち、受け手がそれより前の仕事をこなす間（実機で 1 回 6 ms ほど）、
// DXVK の submit を送る糸まで止めていた。IPHONE_VK_PROGRESS_ASYNC=0 で前のやり方
enum { PEND_DEFER = 1, PEND_PROGRESS = 2 };
static unsigned char g_pend[64];
static unsigned g_pend_head, g_pend_n;
static int g_prog_out;                  // 答えを待っている進み具合の問い合わせがある
static volatile uint64_t g_done_msg;    // 受け手が「終わった」と答えた submit の番号（下の「fence を本当の進み具合で答える」で使う）

static void pend_push_locked(int kind) {
    if (g_pend_n < sizeof g_pend) g_pend[(g_pend_head + g_pend_n++) % sizeof g_pend] = (unsigned char) kind;
}

// 列の先頭の返事を 1 つ読む。読めなければ 0（つながりが切れた）
static int pend_read_one_locked(void) {
    if (!g_pend_n) return 0;
    int kind = g_pend[g_pend_head];
    g_pend_head = (g_pend_head + 1) % sizeof g_pend;
    g_pend_n--;
    uint32_t h[2];
    if (kind == PEND_DEFER) {
        g_defer.active = 0;
        g_defer.ready = 1;
        g_defer.r = VK_ERROR_DEVICE_LOST;
        g_defer.out.n = 0;
        if (!recv_head(h, g_defer.op)) return 0;
        vr_reserve(&g_defer.out, h[1]);
        if (h[1] && !vr_recv_all(g_sock, g_defer.out.p, h[1])) { conn_lost("窓の絵の返事の途中"); return 0; }
        g_defer.out.n = h[1];
        g_defer.r = (VkResult) (int32_t) h[0];
        g_stat.bytes_in += 8 + h[1];
        return 1;
    }
    g_prog_out = 0;
    if (!recv_head(h, VR_GET_PROGRESS)) return 0;
    uint8_t tmp[64];
    if (h[1] > sizeof tmp || (h[1] && !vr_recv_all(g_sock, tmp, h[1]))) { conn_lost("進み具合の返事の途中"); return 0; }
    g_stat.bytes_in += 8 + h[1];
    if ((int32_t) h[0] == VK_SUCCESS && h[1] >= 8) {
        uint64_t v;
        memcpy(&v, tmp, 8);
        if (v > g_done_msg) g_done_msg = v;
    }
    return 1;
}

// g_lock の中で、送り終えたあとに呼ぶ。たまっている返事を頼んだ順に全部読む
static void read_deferred_locked(void) {
    while (g_pend_n && !g_dead)
        if (!pend_read_one_locked()) break;
    if (g_dead) { g_pend_n = 0; g_prog_out = 0; g_defer.active = 0; }
}

// 頼んで、返事は待たない。返事は rpc_take_deferred で受け取る（受け取る前に頼めるのは 1 つだけ）
LINUX_UNUSED static void rpc_call_deferred(uint32_t op, vr_buf *b) {
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.calls[op]++;
    if (g_defer.active) { flush_locked(); read_deferred_locked(); }
    put_msg_locked(op, b);
    if (flush_locked()) { g_defer.active = 1; g_defer.ready = 0; g_defer.op = op; pend_push_locked(PEND_DEFER); }
    ReleaseSRWLockExclusive(&g_lock);
    b->n = 0;
}

// あとで受け取る返事を受け取る。まだ届いていなければ待つ。頼んでいなければ VK_NOT_READY
// 届くのを待つ間は錠を放す（IPHONE_VK_DEFER_UNLOCKED=0 で前のやり方）。前は錠を持ったまま待ち、
// iPhone が前のコマを描き終えるまで、DXVK の命令を組み立てる糸も fence を待つ糸も止めていた（1080p の Heaven で両方が暇になっていた）
static int defer_unlocked(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_DEFER_UNLOCKED"); v = !(e && *e == '0'); }
    return v;
}

LINUX_UNUSED static VkResult rpc_take_deferred(vr_buf *out) {
    double t0 = now_ms();
    if (defer_unlocked()) {
        // 返事が届いた分だけ錠の中で読み、まだなら錠の外で 1 ms ずつ待つ。待つ間にほかの糸が先に読んでもよい（g_defer.ready に入る）
        for (;;) {
            AcquireSRWLockExclusive(&g_lock);
            if (g_defer.active && flush_locked()) {
                WSAPOLLFD pf = { .fd = g_sock, .events = POLLRDNORM };
                while (g_defer.active && g_pend_n && !g_dead && WSAPoll(&pf, 1, 0) > 0)
                    if (!pend_read_one_locked()) break;
            }
            int done = !g_defer.active || g_dead;
            ReleaseSRWLockExclusive(&g_lock);
            if (done) break;
            WSAPOLLFD pf = { .fd = g_sock, .events = POLLRDNORM };
            WSAPoll(&pf, 1, 1);
        }
    }
    AcquireSRWLockExclusive(&g_lock);
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
    g_stat.wait_ms[g_defer.op ? g_defer.op : VR_READ_JPEG_PIPE] += now_ms() - t0;
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

// 受信を待つ長さ（ミリ秒・0 は無限）を決める。Winsock は DWORD のミリ秒、POSIX は struct timeval
static void sock_rcvtimeo(vr_sock s, unsigned ms) {
#ifdef _WIN32
    DWORD to = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
#else
    struct timeval to = { (time_t) (ms / 1000), (suseconds_t) (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
#endif
}

// 待たない（non-blocking）にする・戻す
static void sock_nonblock(vr_sock s, int on) {
#ifdef _WIN32
    u_long nb = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

// host:port に TCP でつなぐ。1.5 秒でつながらなければ諦める（iPhone がいないときにアプリを待たせない）
static vr_sock tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) return INVALID_SOCKET;
    vr_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(ai); return INVALID_SOCKET; }
    sock_nonblock(s, 1);
    connect(s, ai->ai_addr, (int) ai->ai_addrlen);
    freeaddrinfo(ai);
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    FD_ZERO(&ef);
    FD_SET(s, &ef);
    struct timeval tv = { 1, 500000 };
#ifdef _WIN32
    int ok = select(0, NULL, &wf, &ef, &tv) == 1 && FD_ISSET(s, &wf);
#else
    // POSIX の select は「つながった」ときも「失敗した」ときも書ける印を返すので、SO_ERROR で成功を確かめる
    int ok = select(s + 1, NULL, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf);
    if (ok) {
        int err = 0;
        socklen_t el = sizeof err;
        ok = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0;
    }
#endif
    sock_nonblock(s, 0);
    if (!ok) { closesocket(s); return INVALID_SOCKET; }
    sock_rcvtimeo(s, 5000); // 最初のあいさつまでは 5 秒で諦める
    return s;
}

// ---- USB の iPhone に直接つなぐ ----
// iTunes と一緒に入る Apple Mobile Device Service が 127.0.0.1:27015 で待っている（usbmuxd）。
// 「この iPhone のこの番号につないで」と頼むと、その接続がそのまま USB の通り道になる。
// Python の中継（usb_forward.py）を通さないので、1 段ぶん速い
// Linux：話し方（plist）は同じだが、usbmuxd の口は Unix socket（/var/run/usbmuxd）で、ここは 127.0.0.1 の TCP しか知らない。
// 今は socat などで TCP に橋渡しして IPHONE_VK_MUX=番号 で向ける（Unix socket を直に開く道は未実装）
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
             dev, (unsigned) htons((uint16_t) port));
    char *res = mux_call(s, "Connect", extra);
    long long num = res ? plist_int_after(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { closesocket(s); say("iPhone の %d 番につながらない（usbmuxd の返事 %lld）", port, num); return INVALID_SOCKET; }
    return s;
}

// ---- インターネット共有の口（Backburner と同じく、USB を有線のネットワークとして使う道） ----
// まず USB の口（usbmux）か host:port につなぎ、VR_GET_LINK で「インターネット共有の口の住所と合言葉」を聞く。
// 聞けたら、その口に TCP でつなぎ直し、最初に "VKRA" と合言葉を送る。だめなら、いつもの口でつなぐ
static vr_sock link_connect(const char *boot, char *host, size_t nhost, char *port, size_t nport) {
    char bh[128] = "127.0.0.1", bp[16] = "50060";
    vr_sock b;
    if (boot && *boot) {
        const char *c = strrchr(boot, ':');
        if (c) {
            size_t n = (size_t) (c - boot) < sizeof bh - 1 ? (size_t) (c - boot) : sizeof bh - 1;
            memcpy(bh, boot, n);
            bh[n] = 0;
            snprintf(bp, sizeof bp, "%s", c + 1);
        } else {
            snprintf(bh, sizeof bh, "%s", boot);
        }
        b = tcp_connect(bh, bp);
    } else {
        b = usb_connect(50060);
        snprintf(bh, sizeof bh, "USB の iPhone");
    }
    if (b == INVALID_SOCKET) { say("%s:%s につながらないので、インターネット共有の口を聞けない", bh, bp); return INVALID_SOCKET; }
    uint32_t h[2] = { VR_GET_LINK, 0 };
    uint8_t body[28];
    uint32_t ok = 0, ip = 0, lport = 0;
    uint8_t token[16];
    if (vr_send_all(b, h, 8) && vr_recv_all(b, h, 8) && h[0] == 0 && h[1] == sizeof body && vr_recv_all(b, body, sizeof body)) {
        memcpy(&ok, body, 4);
        memcpy(token, body + 4, 16);
        memcpy(&ip, body + 20, 4);
        memcpy(&lport, body + 24, 4);
    }
    vr_sock s = INVALID_SOCKET;
    if (ok && ip && lport && lport < 65536) {
        // 前のつなぎを閉じきってから、つなぎ直す（受け手は一度に 1 人の相手しかしない）
        closesocket(b);
        b = INVALID_SOCKET;
        struct in_addr ia;
        ia.s_addr = ip;
        char lh[32], lp[16];
        inet_ntop(AF_INET, &ia, lh, sizeof lh);
        snprintf(lp, sizeof lp, "%u", (unsigned) lport);
        s = tcp_connect(lh, lp);
        uint8_t pre[20];
        uint32_t magic = VR_LINK_MAGIC;
        memcpy(pre, &magic, 4);
        memcpy(pre + 4, token, 16);
        if (s != INVALID_SOCKET && !vr_send_all(s, pre, sizeof pre)) { closesocket(s); s = INVALID_SOCKET; }
        if (s != INVALID_SOCKET) {
            snprintf(host, nhost, "インターネット共有の口 %s", lh);
            snprintf(port, nport, "%s", lp);
            return s;
        }
        say("インターネット共有の口 %s:%s につながらないので、いつもの口でつなぐ", lh, lp);
    } else {
        say("受け手がインターネット共有の口を教えない（共有が切れている・古い受け手）ので、いつもの口でつなぐ");
    }
    if (b != INVALID_SOCKET) closesocket(b);
    if (boot && *boot) {
        snprintf(host, nhost, "%s", bh);
        snprintf(port, nport, "%s", bp);
        return tcp_connect(bh, bp);
    }
    snprintf(host, nhost, "USB の iPhone");
    snprintf(port, nport, "50060");
    return usb_connect(50060);
}

static int connect_locked(void) {
    g_submit_msgs = 0; // 受け手もつながるたびに数え直す
    g_pend_n = 0;
    g_prog_out = 0;
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    // IPHONE_VK：「usb」か「usb:番号」なら USB の iPhone に直接。「link」ならインターネット共有の口。それ以外は host:port
    const char *e = getenv("IPHONE_VK");
    char host[128] = "127.0.0.1", port[16] = "50061";
    vr_sock s;
    if (e && !strncmp(e, "link", 4) && (e[4] == 0 || e[4] == ':')) {
        // 「link」なら USB の口で聞いて、「link:host:port」なら host:port で聞いて、インターネット共有の口につなぐ
        s = link_connect(e[4] == ':' ? e + 5 : NULL, host, sizeof host, port, sizeof port);
    } else if (e && !strncmp(e, "usb", 3) && (e[3] == 0 || e[3] == ':')) {
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
    sock_rcvtimeo(s, 5000); // 最初のあいさつだけは 5 秒で諦める
    g_sock = s;
    say("%s:%s につないだ", host, port);
    // つないだら、このドライバの DLL をプロセスが終わるまで外させない。最後の instance を壊すとローダーが DLL を外して読み直すが、
    // 外れるときにつながりを閉じないので古いつながりが残り、受け手（一度に 1 人の相手）が 2 本目のあいさつに答えず固まった（CTS で見つかった）
#ifdef _WIN32
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR) (void *) &connect_locked, &self);
#else
    // Linux のローダーも、最後の instance を壊すとこの .so を外す。RTLD_NODELETE で開き直して外れないようにする
    Dl_info di;
    if (dladdr((void *) &connect_locked, &di) && di.dli_fname) dlopen(di.dli_fname, RTLD_NOW | RTLD_NODELETE);
#endif
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
// アプリのエンジンが Mesa の zink（OpenGL を Vulkan に直す部品）
static int g_engine_zink;
// アプリのエンジン名が ANGLE（Chrome・Edge などの OpenGL ES / WebGL を Vulkan に直す部品。アプリの名前は chrome.exe になる）
static int g_app_angle;
// アプリのエンジンが vkd3d-proton（DirectX 12 を Vulkan に直す部品）。DXVK 3 の名乗りに加えて、
// transform feedback・画像の範囲の外・1 要素の揃え・descriptor indexing を名乗る（vkd3d() を見る所）
static int g_engine_vkd3d;
static int dxvk3(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("IPHONE_VK_DXVK3");
        v = e && *e ? *e == '1' : g_engine_dxvk2 || g_engine_zink || g_engine_vkd3d;
        // zink（Mesa 26）も nullDescriptor と maintenance5 が無いと装置を断る
        if (v && !(e && *e))
            say(g_engine_zink ? "zink なので、DXVK 3 と同じ名乗りを入れる"
                : g_engine_vkd3d ? "vkd3d-proton なので、DXVK 3 と DirectX 12 のための名乗りを入れる" : "新しい DXVK なので、DXVK 3 のための名乗りを入れる");
    }
    return v;
}
static int vkd3d(void) { return g_engine_vkd3d && dxvk3(); }
// transform feedback を名乗る相手。vkd3d-proton は持たないと装置を作らない。zink は持たないと OpenGL を 2.1 で止める
// （stream output の数を 0 にするため）。IPHONE_VK_ZINK_XFB=0 で zink には名乗らない
static int xfb_named(void) {
    static int z = -1;
    if (z < 0) { const char *e = getenv("IPHONE_VK_ZINK_XFB"); z = !(e && *e == '0'); }
    return vkd3d() || (g_engine_zink && z);
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
    // 1.1・1.2 の核に入った拡張の名前。新しい入口は無く、中身（dynamic rendering の resolve・render pass 2）はもう運んでいる。
    // VK_KHR_dynamic_rendering を有効にするアプリは、依存の名前としてこの 3 つも頼む（Sascha Willems の dynamicrendering など）
    "VK_KHR_maintenance2", "VK_KHR_multiview", "VK_KHR_depth_stencil_resolve",
};

// 受け手が caps で名乗ったときだけ見せる拡張（受け手の装置でも有効にする）
static const struct { const char *name; uint32_t cap; } CAP_EXT[] = {
    { "VK_KHR_dynamic_rendering", VR_CAP_DYNAMIC_RENDERING }, { "VK_KHR_synchronization2", VR_CAP_SYNC2 },
    { "VK_KHR_create_renderpass2", VR_CAP_RENDER_PASS2 }, { "VK_EXT_extended_dynamic_state", VR_CAP_EXT_DYNAMIC_STATE },
    { "VK_EXT_extended_dynamic_state2", VR_CAP_EXT_DYNAMIC_STATE }, { "VK_EXT_host_query_reset", VR_CAP_QUERY },
    { "VK_EXT_vertex_attribute_divisor", VR_CAP_DIVISOR },
};

static int has_ext(const char *name);
static int g_khr_div_native = -1; // 受け手が KHR の divisor を自分で持つか（あいさつで決める）
static int g_hw_ext_khr_div(void) { return g_khr_div_native > 0; }
// 受け手の GPU が VK_EXT_dynamic_rendering_unused_attachments を持つ（持てば vkd3d-proton のときだけ見せて、受け手でも有効にする）。
// vkd3d-proton は無くても装置を作る（注意を出すだけ）ので、持たない GPU（iPhone は未確認）には名乗らない
static int g_srv_unused_att;

static int allowed_ext(const char *name) {
    if (!strcmp(name, "VK_KHR_vertex_attribute_divisor")) return (g_hw.caps & VR_CAP_DIVISOR) != 0;
    if (dxvk3() && (!strcmp(name, "VK_KHR_maintenance5") || !strcmp(name, "VK_KHR_maintenance6"))) return 1;
    if (!strcmp(name, "VK_EXT_dynamic_rendering_unused_attachments")) return g_srv_unused_att && vkd3d();
    for (size_t i = 0; i < sizeof ALLOWED_EXT / sizeof *ALLOWED_EXT; i++)
        if (!strcmp(name, ALLOWED_EXT[i])) return 1;
    for (size_t i = 0; i < sizeof CAP_EXT / sizeof *CAP_EXT; i++)
        if (!strcmp(name, CAP_EXT[i].name)) return (g_hw.caps & CAP_EXT[i].cap) != 0;
    return 0;
}

// VK_EXT_provoking_vertex を名乗るか（IPHONE_VK_PROVOKING=0 で名乗らない・1 で zink にも名乗る）。
// 拡張の一覧はあいさつのときに一度だけ作る。Chrome は ANGLE より前に自分の Vulkan と Dawn の instance を作るので、エンジンの名前では分けず、
// zink（名乗ると GS の真似を使わない道に変わる）のほかは名乗る
static int provoking_named(void) {
    const char *e = getenv("IPHONE_VK_PROVOKING");
    if (e && *e) return *e == '1';
    return !g_engine_zink;
}

static int has_ext(const char *name) {
    for (uint32_t i = 0; i < g_hw.ne; i++)
        if (!strcmp(g_hw.ext[i].extensionName, name)) return 1;
    return 0;
}

// 受け手に渡す拡張の名前（KHR の divisor は、受け手が持つ EXT の名前に替える）。重ねて頼まないように、替えた結果が重なれば NULL
// VK_KHR_load_store_op_none：受け手の GPU が持たない（または IPHONE_VK_LSO=1）ときは、アプリには持つと名乗り、受け手には拡張を頼まず、
// 値を安全な物に直して送る。LOAD_OP_NONE は LOAD（前の中身を残す・読んでよい）、STORE_OP_NONE は STORE（中身は変わっていないので、書き戻しても同じ）。
static int g_lso_fix;
static int g_srv_lso;
static volatile long g_lso_n[2]; // アプリが渡した NONE の数（LOAD・STORE）
static inline uint32_t lso_load(uint32_t o) { if (o == 1000400000u) { g_lso_n[0]++; if (g_lso_fix) return (uint32_t) VK_ATTACHMENT_LOAD_OP_LOAD; } return o; }
static inline uint32_t lso_store(uint32_t o) { if (o == 1000301000u) { g_lso_n[1]++; if (g_lso_fix) return (uint32_t) VK_ATTACHMENT_STORE_OP_STORE; } return o; }
static const char *fwd_ext(const char *name, const char *const *all, uint32_t n) {
    if (g_lso_fix && (!strcmp(name, "VK_KHR_load_store_op_none") || !strcmp(name, "VK_EXT_load_store_op_none"))) return NULL;
    if (!strcmp(name, "VK_KHR_vertex_attribute_divisor") && !g_hw_ext_khr_div()) {
        for (uint32_t k = 0; k < n; k++)
            if (!strcmp(all[k], "VK_EXT_vertex_attribute_divisor")) return NULL;
        return "VK_EXT_vertex_attribute_divisor";
    }
    return name;
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
    // zink には名乗らない。名乗ると zink は四角形（GL_QUADS）や頂点の順（provoking vertex）の真似に GS を作り、
    // それがドライバの GS の真似を通る（10/7 は PrimitiveId の出力で受け手の GPU が止まった）。名乗らなければ zink は GS を使わない道を通る
    if (dxvk3() && !g_engine_zink) f->geometryShader = f->shaderCullDistance = VK_TRUE;
    // 中継していない機能は「無い」と答える
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = f->sparseResidency4Samples = f->sparseResidency8Samples = f->sparseResidency16Samples = VK_FALSE;
    f->sparseResidencyAliased = f->pipelineStatisticsQuery = f->inheritedQueries = VK_FALSE;
    if (!(g_hw.caps & VR_CAP_QUERY)) f->occlusionQueryPrecise = VK_FALSE;
}

// ---- sparse（tiled resources）の「名乗りだけ」の真似（kit_extra/research/sparse_emulation.md の S1） ----
// 機能・性質・sparse binding のキューを名乗る。sparse の buffer と image は、受け手にはふつうの物として作り（SPARSE の印を外す）、
// ドライバが仮の大きさ全部ぶんの隠しメモリを、作ったときに結ぶ。vkQueueBindSparse は中身を動かさず、待つ・知らせる合図だけを空の submit で送る。
// シェーダーの residency の問い合わせ（OpImageSparse*）は、ふつうの読みと「いつもある」に直す（spv_sparse）。
// 守らない約束：結んでいない所を読んでも 0 にならない・書いても捨てられない・タイルを結び直しても中身はついて来ない・
// 同じヒープのタイルを 2 か所に結んでも中身は共有されない・CheckAccessFullyMapped はいつも真
// IPHONE_VK_SPARSE=0 で名乗らない（前と同じ）・1 でどのアプリにも名乗る・無ければ vkd3d-proton のときだけ
static int sparse_mode(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_SPARSE"); v = e && *e ? (*e == '1' ? 2 : *e == '0' ? 0 : 1) : 1; }
    return v;
}
static int sparse_on(void) { int m = sparse_mode(); return m == 2 || (m == 1 && vkd3d()); }
// 結んでいない所の読みが 0 だと名乗るか（residencyNonResidentStrict）。S1 は 0 にしないので、標準は名乗らない。
// vkd3d-proton はこれが無いと TiledResourcesTier を 1 にするが、VKD3D_FEATURE_LEVEL=12_0（IPHONE_VK_FL12）があれば 2 に上げる。
// IPHONE_VK_SPARSE_STRICT=1 で名乗る（嘘になる）
static int sparse_strict(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_SPARSE_STRICT"); v = e && *e == '1'; }
    return v;
}
// 隠しメモリの合計の上限（IPHONE_VK_SPARSE_MAX_MB・標準 1024 MiB）。iPhone のアプリが使えるのは全部で約 5.8 GB で、
// 仮想テクスチャ用の大きな tiled resource を丸ごと持つと、それだけで iPhone のアプリが落とされる。超える物は作らずに断る
static uint64_t sparse_cap(void) {
    static uint64_t v;
    if (!v) {
        const char *e = getenv("IPHONE_VK_SPARSE_MAX_MB");
        uint64_t mb = e && *e ? strtoull(e, NULL, 0) : 1024;
        v = (mb ? mb : 1) << 20;
    }
    return v;
}
static volatile LONG64 g_sparse_bytes; // いま持っている隠しメモリの合計
enum { SPARSE_BLOCK = 65536 };
#define SPARSE_BUF_FLAGS (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)
#define SPARSE_IMG_FLAGS (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)

// 隠しメモリを size バイト取ってよいか。上限を超えるなら断って、記録に出す（IPHONE_VK_LOG が無くても出す）
static int sparse_take(uint64_t size, const char *what, uint64_t app_size) {
    uint64_t cap = sparse_cap();
    LONG64 now = InterlockedAdd64(&g_sparse_bytes, (LONG64) size);
    if ((uint64_t) now > cap) {
        InterlockedAdd64(&g_sparse_bytes, -(LONG64) size);
        static volatile LONG k;
        LONG c = InterlockedIncrement(&k);
        if (c <= 8 || (c & (c - 1)) == 0)
            fprintf(stderr, "[iphone-vk] sparse の %s（仮の大きさ %.1f MiB）を作らずに断った：ふつうのメモリで丸ごと持つと合計 %.1f MiB になり、"
                            "上限 %.0f MiB を超える（IPHONE_VK_SPARSE_MAX_MB で変えられる・%ld 回目）\n",
                    what, app_size / 1048576.0, (double) (now) / 1048576.0, cap / 1048576.0, c);
        return 0;
    }
    say("sparse の %s（仮の大きさ %.1f MiB）を、ふつうのメモリ %.1f MiB で丸ごと持つ（合計 %.1f MiB／上限 %.0f MiB）", what, app_size / 1048576.0,
        size / 1048576.0, (double) now / 1048576.0, cap / 1048576.0);
    return 1;
}
static void sparse_give(uint64_t size) { InterlockedAdd64(&g_sparse_bytes, -(LONG64) size); }

// 色の形式の、塊 1 つのバイト数と塊の大きさ（圧縮でなければ 1x1）。知らない形式・深度とステンシルは 0
static int fmt_block(VkFormat f, uint32_t *bytes, uint32_t *bw, uint32_t *bh) {
    uint32_t b = 0, w = 1, h = 1;
    if (f == VK_FORMAT_R4G4_UNORM_PACK8 || (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB)) b = 1;
    else if ((f >= VK_FORMAT_R4G4B4A4_UNORM_PACK16 && f <= VK_FORMAT_A1R5G5B5_UNORM_PACK16) || (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) ||
             (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT))
        b = 2;
    else if ((f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) || (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) ||
             (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
        b = 4;
    else if ((f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) || (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT) ||
             (f >= VK_FORMAT_R64_UINT && f <= VK_FORMAT_R64_SFLOAT))
        b = 8;
    else if ((f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) || (f >= VK_FORMAT_R64G64_UINT && f <= VK_FORMAT_R64G64_SFLOAT))
        b = 16;
    else if ((f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK ||
             (f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK) || f == VK_FORMAT_EAC_R11_UNORM_BLOCK || f == VK_FORMAT_EAC_R11_SNORM_BLOCK)
        b = 8, w = h = 4;
    else if ((f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC3_SRGB_BLOCK) || (f >= VK_FORMAT_BC5_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) ||
             f == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK || f == VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK || f == VK_FORMAT_EAC_R11G11_UNORM_BLOCK ||
             f == VK_FORMAT_EAC_R11G11_SNORM_BLOCK || f == VK_FORMAT_ASTC_4x4_UNORM_BLOCK || f == VK_FORMAT_ASTC_4x4_SRGB_BLOCK)
        b = 16, w = h = 4;
    *bytes = b; *bw = w; *bh = h;
    return b != 0;
}

// 2D・1 サンプルの標準のブロックの形（Vulkan の Standard Sparse Image Block Shapes。64 KB）。画素で返す。無い形式は 0
static int sparse_shape(VkFormat f, uint32_t *gw, uint32_t *gh, uint32_t *bytes, uint32_t *bw, uint32_t *bh) {
    if (!fmt_block(f, bytes, bw, bh)) return 0;
    uint32_t w, h;
    switch (*bytes) {
    case 1: w = 256; h = 256; break;
    case 2: w = 256; h = 128; break;
    case 4: w = 128; h = 128; break;
    case 8: w = 128; h = 64; break;
    case 16: w = 64; h = 64; break;
    default: return 0;
    }
    *gw = w * *bw;
    *gh = h * *bh;
    return 1;
}

// sparse の image を作ったときの形（必要なメモリと、タイルの並びの答えに使う）
struct spimg { int on, residency; VkImageType type; VkFormat fmt; uint32_t w, h, d, mips, layers; VkSampleCountFlagBits samples; VkImageTiling tiling; };

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t up64k(uint64_t a) { return (a + SPARSE_BLOCK - 1) / SPARSE_BLOCK * SPARSE_BLOCK; }

// residency の image のタイルの並び。ふつうの段のタイルを層ごとに並べ、その後ろに層ごとの mip tail（小さい段をまとめた所）を置く。
// 結び付けは中身を動かさないので、この並びはアプリに答える数のためだけの物。*size は仮の大きさ（アプリに答えるメモリの量）
static int sparse_img_layout(const struct spimg *s, VkSparseImageMemoryRequirements *r, VkDeviceSize *size) {
    uint32_t gw, gh, bytes, bw, bh;
    if (!s->on || !s->residency || s->type != VK_IMAGE_TYPE_2D || s->samples != VK_SAMPLE_COUNT_1_BIT || s->tiling != VK_IMAGE_TILING_OPTIMAL ||
        !sparse_shape(s->fmt, &gw, &gh, &bytes, &bw, &bh))
        return 0;
    uint32_t mips = s->mips ? s->mips : 1, layers = s->layers ? s->layers : 1, first_tail = mips;
    uint64_t tiles = 0, tail = 0;
    for (uint32_t l = 0; l < mips; l++) {
        uint32_t wl = s->w >> l ? s->w >> l : 1, hl = s->h >> l ? s->h >> l : 1;
        if (first_tail == mips && (wl < gw || hl < gh)) first_tail = l; // ブロックより小さい段から後ろは mip tail
        if (l < first_tail) tiles += (uint64_t) div_up(wl, gw) * div_up(hl, gh);
        else tail += (uint64_t) div_up(wl, bw) * div_up(hl, bh) * bytes;
    }
    tail = up64k(tail);
    memset(r, 0, sizeof *r);
    r->formatProperties.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r->formatProperties.imageGranularity = (VkExtent3D) { gw, gh, 1 };
    r->formatProperties.flags = 0; // 層ごとに mip tail・揃えなし・標準の形
    r->imageMipTailFirstLod = first_tail;
    r->imageMipTailSize = tail;
    r->imageMipTailOffset = tiles * SPARSE_BLOCK * layers;
    r->imageMipTailStride = tail;
    *size = tiles * SPARSE_BLOCK * layers + tail * layers;
    if (!*size) *size = SPARSE_BLOCK;
    return 1;
}

// 隠しメモリの種類：GPU だけのメモリ（CPU から見えない・lazily でない）を優先する
static uint32_t sparse_mem_type(uint32_t bits) {
    int best = -1;
    for (uint32_t i = 0; i < g_hw.mem.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = g_hw.mem.memoryTypes[i].propertyFlags;
        if (f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)) continue;
        if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
        if (best < 0) best = (int) i;
    }
    if (best >= 0) return (uint32_t) best;
    for (uint32_t i = 0; i < 32; i++) if (bits & (1u << i)) return i;
    return 0;
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
#if UINTPTR_MAX == 0xffffffffu
            // 32 ビットのドライバ：受け手は 64 ビットなので、VkPhysicalDeviceProperties は 824 バイトで届く。
            // limits の中の size_t の minMemoryMapAlignment が 8 バイト（前に 4 バイトの詰め物）なので、その後ろを 8 バイト前に詰める
            {
                uint8_t p64[824];
                vr_rcopy(&rd, p64, sizeof p64);
                uint8_t *d = (uint8_t *) &g_hw.props;
                size_t lim = offsetof(VkPhysicalDeviceProperties, limits), mm = offsetof(VkPhysicalDeviceLimits, minMemoryMapAlignment);
                memcpy(d, p64, lim + mm);
                uint64_t al;
                memcpy(&al, p64 + lim + 304, 8);
                g_hw.props.limits.minMemoryMapAlignment = (size_t) al;
                memcpy(d + lim + mm + sizeof(size_t), p64 + lim + 312, sizeof g_hw.props - (lim + mm + sizeof(size_t)));
            }
#else
            vr_rcopy(&rd, &g_hw.props, sizeof g_hw.props);
#endif
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
                if (!strcmp(x.extensionName, "VK_EXT_dynamic_rendering_unused_attachments")) g_srv_unused_att = 1;
                if (!strcmp(x.extensionName, "VK_KHR_load_store_op_none") || !strcmp(x.extensionName, "VK_EXT_load_store_op_none")) g_srv_lso = 1;
                if (allowed_ext(x.extensionName) && g_hw.ne < 62) g_hw.ext[g_hw.ne++] = x;
                if (!strcmp(x.extensionName, "VK_KHR_vertex_attribute_divisor")) g_khr_div_native = 1;
            }
            {   // load_store_op_none：受け手が持たない（か、IPHONE_VK_LSO=1 で試す）ときは、値を直して送る
                const char *lso = getenv("IPHONE_VK_LSO");
                g_lso_fix = lso && *lso ? *lso == '1' : !g_srv_lso;
                if (g_lso_fix) {
                    int has_khr = 0, has_ext = 0;
                    for (uint32_t z = 0; z < g_hw.ne; z++) {
                        has_khr |= !strcmp(g_hw.ext[z].extensionName, "VK_KHR_load_store_op_none");
                        has_ext |= !strcmp(g_hw.ext[z].extensionName, "VK_EXT_load_store_op_none");
                    }
                    if (!has_khr && g_hw.ne < 63) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_load_store_op_none", 1 };
                    if (!has_ext && g_hw.ne < 63) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_EXT_load_store_op_none", 1 };
                    say("load_store_op_none：受け手%sので、NONE を LOAD・STORE に直して送る", g_srv_lso ? "は持つが試す" : "が持たない");
                }
            }
#ifdef _WIN32
            if (g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_swapchain", 70 };
#endif
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
            // 頂点の step rate：DXVK 3 は KHR の名前で探す。中身は EXT と同じなので、受け手には EXT として渡す
            if ((g_hw.caps & VR_CAP_DIVISOR) && !has_ext("VK_KHR_vertex_attribute_divisor") && has_ext("VK_EXT_vertex_attribute_divisor") && g_hw.ne < 64)
                g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_vertex_attribute_divisor", 1 };
            // vkd3d-proton が必須にする物で、ドライバの中で済ませる物（受け手には送らない）。
            // transform feedback は名乗りだけ（記録の命令は飛ばす。stream output を使う pipeline が作れるかは受け手の GPU しだい）
            if (xfb_named() && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_EXT_transform_feedback", 1 };
            if (xfb_named() && !has_ext("VK_EXT_transform_feedback")) say("拡張の一覧がいっぱい（%u 個）で、transform feedback を名乗れない", g_hw.ne);
            // GL の頂点の順（flat の値を最後の頂点から取る）は名乗りだけ。ANGLE は無いと OpenGL ES 2.0 までしか作らず、Chrome の WebGL 2 が作れない。
            // pipeline の鎖の VkPipelineRasterizationProvokingVertexStateCreateInfoEXT は受け手に送らない（flat の値は最初の頂点の物になる）
            if (provoking_named() && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_EXT_provoking_vertex", 1 };
            if (provoking_named() && !has_ext("VK_EXT_provoking_vertex")) say("拡張の一覧がいっぱい（%u 個）で、provoking vertex を名乗れない（ANGLE の WebGL 2 が作れない）", g_hw.ne);
            if (!rd.bad) {
                char real[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
                memcpy(real, g_hw.props.deviceName, sizeof real);
                real[sizeof real - 1] = 0;
                // 見せる名前は機種名にする（前は「Apple A19 Pro GPU (iPhone-RPC)」）。チップから機種を引き、分からなければチップの名前を添える。
                // Apple 以外の GPU（3060 の代役）は「iPhone stand-in (…)」。どれも「iPhone」を含むので、DXVK_FILTER_DEVICE_NAME=iPhone で選べる。
                // IPHONE_VK_NAME で好きな名前に替えられる
                static const struct { const char *chip, *model; } MODELS[] = {
                    { "A19 Pro", "iPhone 17 Pro" }, { "A19", "iPhone 17" }, { "A18 Pro", "iPhone 16 Pro" }, { "A18", "iPhone 16" },
                    { "A17 Pro", "iPhone 15 Pro" }, { "A16", "iPhone 15" }, { "A15", "iPhone 13" },
                };
                const char *nm = getenv("IPHONE_VK_NAME"), *model = NULL;
                if (!strncmp(real, "Apple ", 6))
                    for (size_t i = 0; i < sizeof MODELS / sizeof *MODELS && !model; i++) {
                        size_t n = strlen(MODELS[i].chip);
                        if (!strncmp(real + 6, MODELS[i].chip, n) && real[6 + n] == ' ') model = MODELS[i].model;
                    }
                if (nm && *nm) snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "%.200s", nm);
                else if (model) snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "%s", model);
                else if (!strncmp(real, "Apple ", 6)) snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "iPhone (%.200s)", real);
                // PC の GPU（NVIDIA・AMD・Intel）は 3060 などの代役。それ以外（Qualcomm の Adreno・ARM の Mali など）は Android の機種
                // 会社を Apple と名乗らせた代役（VKR_LIKE_IOS_VENDOR）は、名前が Apple で始まらないので代役と分かる
                else if (g_hw.props.vendorID == 0x10DE || g_hw.props.vendorID == 0x1002 || g_hw.props.vendorID == 0x8086 || g_hw.props.vendorID == 0x106b)
                    snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "iPhone stand-in (%.200s)", real);
                else snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "PocketGPU phone (%.200s)", real);
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
                // 調べ用：IPHONE_VK_AS_SWIFTSHADER=1 で、Google の SwiftShader（会社 0x1AE0・型 0xC0DE・CPU）と名乗る。
                // Windows の Chrome は WebGPU に D3D12 しか使わず、Vulkan のアダプタは SwiftShader のときだけ受ける
                // （Dawn の Vulkan は Windows で外部メモリを持たないので、SwiftShader 以外は CanUseAdapter で落ちる）。
                // --use-webgpu-adapter=swiftshader と ANGLE の D3D11 で、WebGPU だけをこのドライバに通すための物
                const char *sws = getenv("IPHONE_VK_AS_SWIFTSHADER");
                if (sws && *sws == '1') {
                    g_hw.props.vendorID = 0x1AE0;
                    g_hw.props.deviceID = 0xC0DE;
                    g_hw.props.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
                    say("SwiftShader と名乗る（IPHONE_VK_AS_SWIFTSHADER）");
                }
                for (uint32_t i = 0; i < g_hw.mem.memoryTypeCount; i++)
                    if (g_hw.mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                        g_hw.mem.memoryTypes[i].propertyFlags &= ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                // 調べ用：IPHONE_VK_QUEUES=数 で、1 つの種類のキューの数をそれ以下に見せる（命令は 1 本の通り道で順に送るので、数で速さは変わらない）
                const char *qe = getenv("IPHONE_VK_QUEUES");
                uint32_t qmax = qe && *qe ? (uint32_t) atoi(qe) : 0;
                for (uint32_t i = 0; i < g_hw.nq; i++) {
                    g_hw.q[i].queueFlags &= VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
                    if (qmax && g_hw.q[i].queueCount > qmax) g_hw.q[i].queueCount = qmax;
                }
                g_hw.ok = 1;
                say("受け手の GPU：%s", g_hw.props.deviceName);
                // あいさつが済んだら、計算を待つ間は切らない
                sock_rcvtimeo(g_sock, 0);
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
    struct lazy *lz;   // 読み戻しを遅らせている所（IPHONE_VK_LAZY_RB）
    struct muse { uint64_t off, end, msg; } *use; // GPU が読む・書く範囲と、それを使う submit の番号（いちばん後の物）
    uint32_t nuse, cuse;
    struct mem *prev, *next;
    uint32_t aflags;   // VkMemoryAllocateFlagsInfo の flags（DEVICE_ADDRESS なら、シェーダーがアドレスで読み書きしうる）
    volatile LONG wide; // 受け手の帳簿に載らない使い方（アドレスで読み書き・結んだあとに書いた descriptor）をされた。
                        // MEM_WRITE・MEM_READ に「受け手の判定より短くせず、ドライバの番号までは必ず待つ」の印を付ける
};
static SRWLOCK g_muse_lock = SRWLOCK_INIT;

// m の off から len バイトを、submit の番号 msg が使う。重なる・隣り合う範囲とはまとめる。64 を超えたら 1 つにまとめる（多めに待つだけ）
static void mu_note(struct mem *m, uint64_t off, uint64_t len, uint64_t msg) {
    if (!m || !len) return;
    uint64_t end = off + len;
    AcquireSRWLockExclusive(&g_muse_lock);
    uint32_t i;
    for (i = 0; i < m->nuse; i++)
        if (m->use[i].off <= end && off <= m->use[i].end) {
            if (off < m->use[i].off) m->use[i].off = off;
            if (end > m->use[i].end) m->use[i].end = end;
            if (msg > m->use[i].msg) m->use[i].msg = msg;
            break;
        }
    if (i == m->nuse) {
        if (m->nuse == m->cuse) {
            uint32_t cap = m->cuse ? m->cuse * 2 : 8;
            void *q = realloc(m->use, cap * sizeof *m->use);
            if (q) { m->use = q; m->cuse = cap; }
        }
        if (m->nuse < m->cuse) m->use[m->nuse++] = (struct muse) { off, end, msg };
    }
    if (m->nuse > 64) {
        struct muse u = m->use[0];
        for (uint32_t k = 1; k < m->nuse; k++) {
            if (m->use[k].off < u.off) u.off = m->use[k].off;
            if (m->use[k].end > u.end) u.end = m->use[k].end;
            if (m->use[k].msg > u.msg) u.msg = m->use[k].msg;
        }
        m->use[0] = u;
        m->nuse = 1;
    }
    ReleaseSRWLockExclusive(&g_muse_lock);
}

// m の off から len バイトを使う submit の番号のうち、いちばん後の物（使う物が無ければ 0）
static uint64_t mu_upto(struct mem *m, uint64_t off, uint64_t len) {
    uint64_t w = 0, end = off + len;
    AcquireSRWLockShared(&g_muse_lock);
    for (uint32_t i = 0; i < m->nuse; i++)
        if (m->use[i].off < end && off < m->use[i].end && m->use[i].msg > w) w = m->use[i].msg;
    ReleaseSRWLockShared(&g_muse_lock);
    return w;
}

// IPHONE_VK_MEMW_UPTO=0 で付けない（受け手は前のやり方で、そのメモリを使う仕事を全部待つ）
static int memw_upto_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("IPHONE_VK_MEMW_UPTO"); on = !(e && *e == '0'); }
    return on;
}

// MEM_WRITE・MEM_READ に付ける番号。受け手の帳簿に載らない使い方をされたメモリ（wide）は、いちばん上の桁を立てる。
// 受け手はそのとき、自分の判定とこの番号の大きい方まで待つ（ふだんは小さい方）。古い受け手は、桁が立つと今までどおりの待ち方になる
#define MEMW_WIDE (1ull << 63)
static uint64_t memw_upto(struct mem *m, uint64_t off, uint64_t len) {
    uint64_t u = mu_upto(m, off, len);
    return m->wide ? u | MEMW_WIDE : u;
}

struct buf { uint64_t size; VkMemoryRequirements req; struct mem *m; uint64_t memoff; uint64_t addr; // addr：受け手の GPU の中のアドレス
             struct mem *hidden; uint64_t hidden_size; }; // hidden：sparse（S1）の隠しメモリ。作ったときに結び、消すときに放す

// ---- アドレスで読み書きされる buffer の見張り（u1。kit_extra/research/uab_tracking.md の筋書き 3） ----
// vkd3d-proton は root の SRV・UAV（Apple なら CBV も）を、set に載せずにアドレス（buffer device address）で渡す。
// そのままだと、そこに CPU が書いた中身を送らず、GPU が書いた中身を読み戻さず、受け手も待たない。
// アドレスを尋ねられた buffer のうち CPU から見えるメモリの物を、アドレスの順に覚えておき、push constant に入った値から引く
struct vab { uint64_t a, e; struct buf *b; };
static struct vab *g_vab;
static size_t g_nvab, g_cvab;
static SRWLOCK g_vab_lock = SRWLOCK_INIT;
static volatile uint64_t g_vab_lo = UINT64_MAX, g_vab_hi; // 表に入ったアドレスの端（外れた値をすぐ捨てる。消しても縮めない）
static volatile LONG g_bda_host;                          // DEVICE_ADDRESS の印の付いた、CPU から見えるメモリの数（0 なら何もしない）

// IPHONE_VK_BDA_SYNC=0 でアドレスの見張りをしない・IPHONE_VK_UAB_SYNC=0 で結んだあとに書いた descriptor を見ない（前のやり方）
static int bda_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_BDA_SYNC"); v = !(e && *e == '0'); }
    return v;
}
static int uab_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_UAB_SYNC"); v = !(e && *e == '0'); }
    return v;
}

// 並びの中で、アドレス a より前に入る位置（a 以上の最初）
static size_t vab_lower(uint64_t a) {
    size_t lo = 0, hi = g_nvab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_vab[mid].a < a) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void vab_add(struct buf *bf) {
    if (!bf || !bf->addr || !bf->m || !bf->m->host || !bf->size) return;
    AcquireSRWLockExclusive(&g_vab_lock);
    if (g_nvab == g_cvab) {
        size_t cap = g_cvab ? g_cvab * 2 : 64;
        struct vab *p = realloc(g_vab, cap * sizeof *p);
        if (!p) { ReleaseSRWLockExclusive(&g_vab_lock); return; }
        g_vab = p;
        g_cvab = cap;
    }
    size_t at = vab_lower(bf->addr);
    for (size_t i = at; i < g_nvab && g_vab[i].a == bf->addr; i++)
        if (g_vab[i].b == bf) { ReleaseSRWLockExclusive(&g_vab_lock); return; } // ほかの糸と同時に尋ねた：もう入っている
    memmove(g_vab + at + 1, g_vab + at, (g_nvab - at) * sizeof *g_vab);
    g_vab[at] = (struct vab) { bf->addr, bf->addr + bf->size, bf };
    g_nvab++;
    if (bf->addr < g_vab_lo) g_vab_lo = bf->addr;
    if (bf->addr + bf->size > g_vab_hi) g_vab_hi = bf->addr + bf->size;
    ReleaseSRWLockExclusive(&g_vab_lock);
}

static void vab_del(struct buf *bf) {
    if (!bf || !bf->addr || !g_nvab) return;
    AcquireSRWLockExclusive(&g_vab_lock);
    for (size_t i = vab_lower(bf->addr); i < g_nvab && g_vab[i].a == bf->addr; i++)
        if (g_vab[i].b == bf) {
            memmove(g_vab + i, g_vab + i + 1, (g_nvab - i - 1) * sizeof *g_vab);
            g_nvab--;
            break;
        }
    ReleaseSRWLockExclusive(&g_vab_lock);
}

// アドレス v を含む buffer を探し、そのメモリと、メモリの中の v の位置・buffer の終わりを返す。
// 同じメモリに重ねてつないだ buffer もあるので、手前を少し見る（いちばん長く後ろへ続く物を取る）
static int vab_find(uint64_t v, struct mem **m, uint64_t *off, uint64_t *end) {
    int hit = 0;
    AcquireSRWLockShared(&g_vab_lock);
    size_t i = vab_lower(v + 1); // a <= v の最後の次
    for (size_t k = 0; i > 0 && k < 64; k++, i--) {
        const struct vab *x = &g_vab[i - 1];
        if (v >= x->e) continue;
        struct buf *b = x->b;
        uint64_t o = b->memoff + (v - x->a), e = b->memoff + b->size;
        if (!hit || e - o > *end - *off) { *m = b->m; *off = o; *end = e; hit = 1; }
    }
    ReleaseSRWLockShared(&g_vab_lock);
    return hit;
}

typedef struct { struct mem *m; uint64_t off, len, serial; struct queue *q; int lazy; } rng; // lazy：読み戻しを遅らせてよい

struct dev {
    VK_LOADER_DATA loader;
    struct mem *mems;  // CPU から見えるメモリの一覧
    struct queue *queues[16];
    rng *pend;         // GPU が書いたかもしれない範囲（終わったら引き取る）
    size_t npend, cappend;
    uint64_t serial;   // submit の通し番号
    SRWLOCK lock;
    volatile LONG skipped_wait; // 往復を省いて「終わった」と答えた待ちがあった（受け手はまだ仕事をしているかもしれない）
    struct pbatch *pb;          // まだ始まれないかもしれない submit（CPU からの合図を待つ timeline の値がある）
    size_t npb, cpb;
};

struct queue { VK_LOADER_DATA loader; struct dev *d; uint32_t fam, idx; uint64_t last_msg; }; // last_msg：このキューに最後に送った submit の番号

struct dent { uint32_t binding, elem, type; struct buf *b; uint64_t off, range; uint64_t h0, h1; uint32_t layout; uint64_t wg; };
// h0・h1：画像なら sampler と image view、texel buffer なら buffer view（descriptor の写しを、ふつうの書き込みに直すのに使う）
// wg：update after bind の set に書いた通し番号（g_dgen。結んだあとに書かれた物を submit のときに見分ける）
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
    // 数を確保のときに決める binding（VARIABLE_DESCRIPTOR_COUNT）。var_max は形の数（上限）
    int var;
    uint32_t var_type, var_max;
    int uab; // update after bind（結んだあとに書き換えてよい）の binding がある
};
// var_count：数を確保のときに決めた set の、その binding の数（pool の残りを数えるのに使う）
// hx：(binding, 番号) から e の位置（+1）を引く表（hcap は 2 の累乗）。vkd3d-proton の set は 100 万個の大きさなので、並びを頭から探すと遅い
// gen：update after bind の set に最後に書いた通し番号
struct dset { struct dpool *pool; struct dset *next; uint32_t n, cap; struct dent *e; struct dsl *layout; uint32_t var_count; uint32_t *hx, hcap; uint64_t gen; };
static volatile LONG64 g_dgen; // update after bind の set に書いた通し番号
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

// 形の k 番目の種類を、set 1 つでいくつ使うか（数を確保のときに決める binding は、その数で数える）
static uint32_t dsl_type_count(const struct dsl *l, uint32_t k, uint32_t var_count) {
    uint32_t c = l->t[k].count;
    if (l->var && l->t[k].type == l->var_type) c = c - l->var_max + var_count;
    return c;
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
    uint64_t rp_last_fb; // 数える用：この command buffer で前に描いた framebuffer
    const struct gs_pipe *gs; // いまつないでいる、GS を真似た pipeline（無ければ NULL）
    uint32_t app_topo;        // アプリが vkCmdSetPrimitiveTopology で決めた形
    int app_topo_set;
    struct gs_cbst *gsx;      // GS を真似るための、頂点・index の buffer の覚えと、記録・descriptor の一時の物
    struct evact *ev;         // submit のときに立てる・倒す event（記録した順）
    size_t nev, cev;
    struct pushheld *ph;      // push descriptor に使った set（記録し直すか消すまで持つ）
    size_t nph, cph;
    struct wrcand *wc;        // storage buffer につないだ物。シェーダーが書く場所かどうかは submit のときに決める
    size_t nwc, cwc;
    uint64_t bpl[64];         // つないだ pipeline の番号（書く場所かどうかを、その pipeline のシェーダーで決める）
    int nbpl, bpl_over;       // 64 を超えたら、全部のシェーダーで決める前のやり方に戻す
    int qcopy_nowait;         // WAIT を付けない query の結果の写しがある（前の仕事が終わっている前提で読む）
    struct qwr { uint64_t qp; uint32_t first, n; } *qw; // query を書いた（始め・終わり・時刻・reset した）範囲
    size_t nqw, cqw;
    // push constant に入っていたアドレスのうち、CPU から見えるメモリの buffer を指す物（u1）。メモリの中の位置・buffer の終わり・push constant の 4 バイトごとの番号
    struct pva { struct mem *m; uint64_t off, end; uint32_t slot; } *pv;
    size_t npv, cpv;
    // 結んだ update after bind の set と、結んだときの通し番号（submit のときに、そのあとに書かれた descriptor を足す）
    struct uabref { struct dset *s; uint64_t gen; uint32_t set; } *ub;
    size_t nub, cub;
    struct lsev *lse; size_t nlse, clse; // LOAD・STORE を調べる・減らすための、この command buffer の出来事（ls.inc）
    int psb_done, psb_any, psb_whole; // psb_collect を済ませた（記録し直すまで）・アドレスで読み書きする・出どころの分からないアドレスを使う
};
struct pushheld { struct dsl *l; VkDescriptorSet s; };
static void qpv_wrote(uint64_t id, uint32_t first, uint32_t n, uint64_t m);
static void cb_note_q(struct cb *c, uint64_t qp, uint32_t first, uint32_t n) {
    if (c->nqw && c->qw[c->nqw - 1].qp == qp && c->qw[c->nqw - 1].first + c->qw[c->nqw - 1].n == first) { c->qw[c->nqw - 1].n += n; return; }
    if (c->nqw == c->cqw) {
        size_t cap = c->cqw ? c->cqw * 2 : 16;
        void *p = realloc(c->qw, cap * sizeof *c->qw);
        if (!p) return;
        c->qw = p;
        c->cqw = cap;
    }
    c->qw[c->nqw++] = (struct qwr) { qp, first, n };
}
struct wrcand { struct buf *b; uint64_t off, len; uint32_t set, binding; int done; };
static void push_release(struct cb *c);
struct event;
struct evact { struct event *e; int val; };
static void ev_fire(struct cb *c);
struct bctmp { VkBuffer b; VkDeviceMemory m; struct bctmp *next; };

struct fence { uint64_t serial; struct queue *q; double last_poll; uint64_t msg; }; // msg：その submit を送った VR_QUEUE_SUBMIT の番号

// timeline semaphore は、どの値をどのキューの何番目の submit が知らせるかを覚える（待ちが済んだら引き取りに使う）
typedef struct { uint64_t value, serial; struct queue *q; } sigrec;
struct sem { int timeline; sigrec *sig; size_t n, cap; uint64_t reached; };

// CPU からの合図を待つかもしれない submit。合図の前に GPU は始まれないので、
// その submit とそれより後に同じ queue に出した物は、終わったと答えず、書いた所も引き取らない。
// 合図のときに、アプリが合図の前に書いたメモリを送る（submit のときにはまだ書かれていない）
typedef struct { struct sem *s; uint64_t v; } semval;
struct pbatch { uint64_t serial; struct queue *q; semval *w; uint32_t nw; struct cb **cbs; uint32_t ncb; };

// q の serial 以前に、まだ止まっている submit があるか（d->lock を持って呼ぶ）
static int pb_blocks(struct dev *d, struct queue *q, uint64_t serial) {
    for (size_t i = 0; i < d->npb; i++)
        if ((!q || d->pb[i].q == q) && d->pb[i].serial <= serial) return 1;
    return 0;
}

// 待つ値に届く道が分かった submit を、一覧から外して out に移す（d->lock を持って呼ぶ）。
// 届く道：その値まで届いたと分かっている（GPU が知らせた・CPU が合図した）か、止まっていない submit が知らせる
static void pb_resolve_locked(struct dev *d, struct pbatch **out, size_t *nout) {
    *out = NULL;
    *nout = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (size_t i = 0; i < d->npb; i++) {
            struct pbatch *b = &d->pb[i];
            int ok = 1;
            for (size_t j = 0; j < d->npb && ok; j++)
                if (j != i && d->pb[j].q == b->q && d->pb[j].serial < b->serial) ok = 0;
            for (uint32_t k = 0; k < b->nw && ok; k++) {
                struct sem *sm = b->w[k].s;
                uint64_t v = b->w[k].v;
                if (v <= sm->reached) continue;
                int found = 0;
                for (size_t r = 0; r < sm->n && !found; r++)
                    if (sm->sig[r].value >= v && !pb_blocks(d, sm->sig[r].q, sm->sig[r].serial)) found = 1;
                if (!found) ok = 0;
            }
            if (!ok) continue;
            *out = realloc(*out, (*nout + 1) * sizeof **out);
            (*out)[(*nout)++] = *b;
            memmove(b, b + 1, (d->npb - i - 1) * sizeof *b);
            d->npb--;
            changed = 1;
            break;
        }
    }
}

// ---- 範囲の並び ----
static void rng_add(rng **a, size_t *n, size_t *cap, struct mem *m, uint64_t off, uint64_t len, uint64_t serial) {
    if (!m || !m->host || !len) return;
    if (off >= m->size) return;
    if (len > m->size - off) len = m->size - off;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *a = realloc(*a, *cap * sizeof **a);
    }
    (*a)[(*n)++] = (rng) { m, off, len, serial, NULL, 0 };
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
            last->lazy &= a[i].lazy; // すぐ読む物と重なれば、すぐ読む
        } else {
            a[++k] = a[i];
        }
    }
    *n = k + 1;
}

// ---- メモリの写し ----
// ページ p0〜p1 の書き込み監視を見て、書かれたページに印を付ける（監視の印は消える）
#ifndef _WIN32
// Linux 版には書き込み監視がまだ無い。範囲の全ページを「書いたかもしれない」とする。
// 受け手の中身の写し（m->sent）がある写しは、そのうち違う所だけを送るので、余計に送るのは写しを初めて送るときだけ。
// （次の一歩：/proc/self/clear_refs と pagemap の soft-dirty、または userfaultfd の write-protect）
static void collect_dirty(struct mem *m, size_t p0, size_t p1) {
    if (p1 > m->npages) p1 = m->npages;
    if (p0 >= p1) return;
    g_stat.ww_calls++;
    memset(m->dirty + p0, 1, p1 - p0);
}
#else
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
#endif

#ifdef _WIN32
// ---- GPU が書いた所の読み戻しを、CPU が触るまで遅らせる（IPHONE_VK_LAZY_RB） ----
// 待ったときにすぐ読み戻さず「古い」と印を付け、そのページを PAGE_NOACCESS にする。CPU（アプリでもドライバでも）が触ると例外になるので、
// ベクトル化例外処理で受け手から読み、ページを開けてから続けさせる。Godot の粒のように、GPU が毎コマ書くが CPU は読まない
// storage buffer が CPU から見えるメモリにあると、今までは毎コマ全部を読み戻していた。
//   読むのは古いバイトだけ。同じページの、アプリが書いてまだ送っていない所は書き換えない
//   開けたページの書き込み監視の印は消さない（消すとアプリの書いた印まで消える）。送るときは sent と比べるので、余計には送らない
//   CPU が触ったページは「熱い」として、しばらくは待ったときにすぐ読む（毎コマ読む物で、例外と往復を増やさない）
//   ドライバ自身が写しに触る前（送る前・引き取る前・BC をほどく前）は、その範囲を先に読む。例外の中で g_lock を取るので、
//   g_lock を持ったまま写しに触らない。g_lz_lock を持って g_lock を取るのはよいが、逆はしない
//   アプリが写しを OS に渡すと（ReadFile など）、例外にならず失敗が返る。そのため既定では storage buffer と fill・update の所だけを
//   遅らせ、CPU が読むための写し（buffer への写し・画像から buffer・query の結果）は今までどおりすぐ読む。
//   buffer を消すとき・vkMapMemory・vkInvalidateMappedMemoryRanges では、その範囲を先に読む（map のたびに番地を OS に渡すアプリ向け。IPHONE_VK_LAZY_MAP=0 で map では読まない）
//   IPHONE_VK_LAZY_RB=0 で今までどおり全部すぐ読む。2 で全部遅らせる（調べ用）
#include <errno.h>
struct lzr { uint64_t a, b; };
struct lazy {
    uint8_t *stale;          // ページごとの「GPU の中身がまだ届いていない」印。印のあるページは PAGE_NOACCESS
    uint8_t *hot;            // ページごとの残り回数。CPU が最近触ったページは、待ったときにすぐ読む
    size_t nhot;             // hot が 0 でないページの数
    struct lzr *r;           // まだ届いていないバイトの範囲（並べて、重ねない）
    size_t nr, cr, nstale;   // nstale：印のあるページの数
    size_t ra_next, ra_win;  // 先読み：続けて触られたら窓を広げる
};
// LZ_HOT：触ったページをすぐ読む回数・LZ_RA0・LZ_RAMAX：先読みの窓（ページ）・LZ_SMALL：丸ごと読む塊・LZ_MAXR：範囲の数の上限
enum { LZ_HOT = 8, LZ_RA0 = 16, LZ_RAMAX = 4096, LZ_SMALL = 64, LZ_MAXR = 1024 };
enum { LZ_FAULT, LZ_SEND, LZ_BACK, LZ_OTHER };
static SRWLOCK g_lz_lock = SRWLOCK_INIT; // 遅らせる仕組みの中身は全部これで守る
static struct mem **g_lz_m;              // 遅らせる仕組みを使ったメモリ（例外のとき、触られた番地から引く）
static volatile LONG g_lz_n;
static size_t g_lz_cap;
static PVOID g_lz_veh;
static struct {
    uint64_t mark_n, mark_bytes, fill_n[4], fill_bytes[4], hot_bytes, back_n, maps, map_stale;
    double fill_ms;
} g_lzs;
static void read_back_many(rng *r, size_t n);

// g_lz_lock を取る・放す。持っている糸を覚える（ドライバの誤りで、持ったまま例外になったら固まらずに落とす）
static volatile DWORD g_lz_owner;
static void lz_lock(void) {
    AcquireSRWLockExclusive(&g_lz_lock);
    g_lz_owner = GetCurrentThreadId();
}

static void lz_unlock(void) {
    g_lz_owner = 0;
    ReleaseSRWLockExclusive(&g_lz_lock);
}

// 調べ用：IPHONE_VK_LAZY_DBG=1 で読むたびに、2 で例外の場所も記録に出す（IPHONE_VK_LOG=1 と一緒に）
static int lz_dbg(int lv) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_LAZY_DBG"); v = e ? atoi(e) : 0; }
    return v >= lv;
}

static int lz_mode(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_LAZY_RB"); v = e && *e == '0' ? 0 : e && *e == '2' ? 2 : 1; }
    return v;
}

// cb_writes の理由（0 storage buffer・1 buffer への写し・2 fill・update・3 画像から buffer・4 query の結果・5 そのほか）ごとに、遅らせてよいか
static int lz_why_ok(int w) {
    int v = lz_mode();
    return v == 2 || (v == 1 && (w == 0 || w == 2));
}

static int lzr_room(struct lazy *lz) {
    if (lz->nr < lz->cr) return 1;
    size_t cap = lz->cr ? lz->cr * 2 : 8;
    struct lzr *p = realloc(lz->r, cap * sizeof *p);
    if (!p) return 0;
    lz->r = p;
    lz->cr = cap;
    return 1;
}

// [a, b) がもう全部古い印の中にあるか
static int lzr_has(const struct lazy *lz, uint64_t a, uint64_t b) {
    for (size_t i = 0; i < lz->nr && lz->r[i].a <= a; i++)
        if (lz->r[i].b >= b) return 1;
    return 0;
}

// [a, b) を足す。重なる・接する範囲とはまとめる
static int lzr_add(struct lazy *lz, uint64_t a, uint64_t b) {
    if (!lzr_room(lz)) return 0;
    size_t i = 0, j;
    while (i < lz->nr && lz->r[i].b < a) i++;
    for (j = i; j < lz->nr && lz->r[j].a <= b; j++) {
        if (lz->r[j].a < a) a = lz->r[j].a;
        if (lz->r[j].b > b) b = lz->r[j].b;
    }
    if (j == i) {
        memmove(&lz->r[i + 1], &lz->r[i], (lz->nr - i) * sizeof *lz->r);
        lz->nr++;
    } else if (j > i + 1) {
        memmove(&lz->r[i + 1], &lz->r[j], (lz->nr - j) * sizeof *lz->r);
        lz->nr -= j - i - 1;
    }
    lz->r[i] = (struct lzr) { a, b };
    return 1;
}

// [a, b) を抜く
static void lzr_cut(struct lazy *lz, uint64_t a, uint64_t b) {
    for (size_t i = 0; i < lz->nr; i++)
        if (lz->r[i].a < a && lz->r[i].b > b) {
            // 真ん中を抜くと 2 つに分かれる（ほかに重なる範囲は無い）
            uint64_t end = lz->r[i].b;
            lz->r[i].b = a;
            if (!lzr_room(lz)) { conn_lost("読み戻しを遅らせる所でメモリが足りない"); return; }
            memmove(&lz->r[i + 2], &lz->r[i + 1], (lz->nr - i - 1) * sizeof *lz->r);
            lz->r[i + 1] = (struct lzr) { b, end };
            lz->nr++;
            return;
        }
    size_t k = 0;
    for (size_t i = 0; i < lz->nr; i++) {
        struct lzr x = lz->r[i];
        if (x.a < a && x.b > a) x.b = a;          // 右を削る
        else if (x.a < b && x.b > b) x.a = b;     // 左を削る
        else if (x.a >= a && x.b <= b) continue;  // 全部抜く
        lz->r[k++] = x;
    }
    lz->nr = k;
}

// m の遅らせる仕組みの中身。使えないメモリ（窓に出すコマ用など）なら NULL。g_lz_lock の中で呼ぶ
static struct lazy *lz_get(struct mem *m) {
    if (m->lz) return m->lz;
    if (!m->host || !m->shadow || m->delta || m->jpeg) return NULL;
    if ((size_t) g_lz_n == g_lz_cap) {
        size_t cap = g_lz_cap ? g_lz_cap * 2 : 16;
        struct mem **p = realloc(g_lz_m, cap * sizeof *p);
        if (!p) return NULL;
        g_lz_m = p;
        g_lz_cap = cap;
    }
    struct lazy *lz = calloc(1, sizeof *lz);
    uint8_t *st = calloc(m->npages, 2);
    if (!lz || !st) { free(lz); free(st); return NULL; }
    lz->stale = st;
    lz->hot = st + m->npages;
    g_lz_m[g_lz_n] = m;
    g_lz_n = g_lz_n + 1;
    m->lz = lz;
    return lz;
}

// ページ p0〜p1 を「CPU が触った」にする。何回すぐ読むかは LZ_HOT（試験では IPHONE_VK_LAZY_HOT=回数。0 なら毎回遅らせる）
static void lz_heat(struct lazy *lz, size_t p0, size_t p1) {
    static int h = -1;
    if (h < 0) { const char *e = getenv("IPHONE_VK_LAZY_HOT"); h = e && *e ? atoi(e) : LZ_HOT; h = h < 0 ? 0 : h > 255 ? 255 : h; }
    for (size_t pg = p0; pg < p1 && h > 0; pg++) {
        if (!lz->hot[pg]) lz->nhot++;
        lz->hot[pg] = (uint8_t) h;
    }
}

// 受け手から m の範囲の並び p を読み、dst に詰める。g_lock は持たずに呼ぶ
static int lz_read(struct mem *m, const struct lzr *p, size_t n, uint8_t *dst) {
    const uint64_t piece = 64u << 20;
    vr_buf b = { 0 };
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < n; i++)
        for (uint64_t at = p[i].a; at < p[i].b; at += piece) {
            uint64_t k = p[i].b - at < piece ? p[i].b - at : piece;
            vr_u64(&b, ID(m));
            vr_u64(&b, at);
            vr_u64(&b, k);
            if (m->wide && memw_upto_on()) vr_u64(&b, memw_upto(m, at, k)); // アドレスで書かれうるメモリ：書いた submit まで待ってもらう（read_back_many と同じ）
            g_stat.calls[VR_MEM_READ]++;
            put_msg_locked(VR_MEM_READ, &b);
            b.n = 0;
        }
    double t0 = now_ms();
    int ok = flush_locked();
    if (ok) read_deferred_locked();
    uint8_t *o = dst;
    for (size_t i = 0; i < n && ok; i++)
        for (uint64_t at = p[i].a; at < p[i].b && ok; at += piece) {
            uint64_t k = p[i].b - at < piece ? p[i].b - at : piece;
            uint32_t h[2];
            if (!recv_head(h, VR_MEM_READ)) { ok = 0; break; }
            if ((int32_t) h[0] < 0) {
                if (!g_dead) say("受け手が失敗を返した (%d)。この先は全部失敗にする", (int32_t) h[0]);
                g_dead = 1;
            }
            if ((int32_t) h[0] == VK_SUCCESS && h[1] == k) {
                if (!vr_recv_all(g_sock, o, k)) { ok = 0; conn_lost("読み出しの途中"); break; }
            } else {
                uint8_t tmp[4096];
                for (uint32_t left = h[1]; left;) {
                    uint32_t c = left < sizeof tmp ? left : (uint32_t) sizeof tmp;
                    if (!vr_recv_all(g_sock, tmp, c)) { ok = 0; conn_lost("読み出しの途中"); break; }
                    left -= c;
                }
            }
            g_stat.bytes_in += 8 + h[1];
            o += k;
        }
    g_stat.wait_ms[VR_MEM_READ] += now_ms() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    free(b.p);
    return ok && !g_dead;
}

// ページ p0〜p1 のうち印のあるページの、まだ届いていないバイトを受け手から読み、ページを開ける。g_lz_lock の中で呼ぶ。
// ほかの糸が古い中身を見る間を狭めるため、読み終えてから、ページごとに開けてすぐ書く
// why：誰が触るか（LZ_FAULT 例外・LZ_SEND 送る前・LZ_BACK 引き取る前・LZ_OTHER buffer を消す・invalidate・map・BC）
static void lz_fill(struct mem *m, size_t p0, size_t p1, int why) {
    struct lazy *lz = m->lz;
    if (p1 > m->npages) p1 = m->npages;
    if (!lz || p0 >= p1) return;
    uint64_t A = (uint64_t) p0 * PAGE, B = (uint64_t) p1 * PAGE, total = 0;
    size_t i0 = 0, n = 0;
    while (i0 < lz->nr && lz->r[i0].b <= A) i0++;
    while (i0 + n < lz->nr && lz->r[i0 + n].a < B) n++;
    if (!n) return; // 古い所が無い（印のあるページには、必ず古い所がある）
    struct lzr *p = malloc(n * sizeof *p);
    for (size_t i = 0; p && i < n; i++) {
        struct lzr x = lz->r[i0 + i];
        p[i].a = x.a < A ? A : x.a;
        p[i].b = x.b > B ? B : x.b;
        total += p[i].b - p[i].a;
    }
    uint8_t *tmp = p ? malloc(total) : NULL;
    if (!tmp) conn_lost("読み戻しを遅らせる所でメモリが足りない"); // 読めないまま開けるので、この先は全部失敗にする
    double t0 = now_ms();
    int ok = tmp && lz_read(m, p, n, tmp);
    g_lzs.fill_ms += now_ms() - t0;
    g_lzs.fill_n[why]++;
    g_lzs.fill_bytes[why] += total;
    if (lz_dbg(1)) say("遅らせた所を読んだ：理由 %d メモリ %p ページ %zu〜%zu %llu バイト", why, (void *) m, p0, p1, (unsigned long long) total);
    size_t i = 0;
    uint64_t pos = 0; // p[i] の中身が tmp のどこから始まるか
    for (size_t pg = p0; pg < p1; pg++) {
        if (!lz->stale[pg]) continue;
        DWORD old;
        VirtualProtect(m->shadow + (size_t) pg * PAGE, PAGE, PAGE_READWRITE, &old);
        uint64_t s = (uint64_t) pg * PAGE, e = s + PAGE;
        // p は並んでいるので、このページに掛かる物を書く
        while (ok && i < n && p[i].a < e) {
            uint64_t a = p[i].a > s ? p[i].a : s, b = p[i].b < e ? p[i].b : e;
            if (a < b) {
                memcpy(m->shadow + a, tmp + pos + (a - p[i].a), (size_t) (b - a));
                if (m->sent) memcpy(m->sent + a, tmp + pos + (a - p[i].a), (size_t) (b - a));
            }
            if (p[i].b > e) break; // 次のページに続く
            pos += p[i].b - p[i].a;
            i++;
        }
        lz->stale[pg] = 0;
        lz->nstale--;
    }
    lzr_cut(lz, A, B);
    free(tmp);
    free(p);
}

// 例外：触られた番地が古いページなら、受け手から読んで開ける。続けてよければ 1
static int lz_fault(uint8_t *addr, void *pc, int wr) {
    if (g_lz_owner == GetCurrentThreadId()) return 0;
    lz_lock();
    struct mem *m = NULL;
    for (LONG i = 0; i < g_lz_n && !m; i++)
        if (addr >= g_lz_m[i]->shadow && addr < g_lz_m[i]->shadow + g_lz_m[i]->npages * PAGE) m = g_lz_m[i];
    int hit = 0;
    if (m) {
        struct lazy *lz = m->lz;
        size_t pg = (size_t) (addr - m->shadow) / PAGE;
        if (lz->stale[pg]) {
            // 古いページの続きが小さければ丸ごと読む。大きければ触った所から先読みし、続けて触られたら窓を倍にする
            size_t s = pg, e = pg + 1;
            while (s > 0 && pg - s < LZ_SMALL && lz->stale[s - 1]) s--;
            while (e < m->npages && e - pg < LZ_SMALL && lz->stale[e]) e++;
            int small = (s == 0 || !lz->stale[s - 1]) && (e >= m->npages || !lz->stale[e]) && e - s <= LZ_SMALL;
            if (!small) {
                // 初めは触ったページだけ（Godot は大きな塊の先頭ページだけに毎コマ触る）。続けて触られたら LZ_RA0 から倍にしていく
                size_t win = pg != lz->ra_next || !lz->ra_win ? 1 : lz->ra_win < LZ_RA0 ? LZ_RA0 : lz->ra_win * 2;
                if (win > LZ_RAMAX) win = LZ_RAMAX;
                lz->ra_win = win;
                s = pg;
                for (e = pg + 1; e < m->npages && e - pg < win && lz->stale[e]; e++) {}
            }
            lz->ra_next = e;
            if (lz_dbg(2)) where(wr ? "遅らせた所に書いた場所" : "遅らせた所を読んだ場所", pc);
            lz_heat(lz, s, e);
            lz_fill(m, s, e, LZ_FAULT);
            hit = 1;
        } else {
            // ほかの糸が先に開けた。開いていればやり直させる
            MEMORY_BASIC_INFORMATION mi;
            hit = VirtualQuery(addr, &mi, sizeof mi) && mi.Protect == PAGE_READWRITE;
        }
    }
    lz_unlock();
    return hit;
}

static LONG CALLBACK lz_veh(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || !g_lz_n) return EXCEPTION_CONTINUE_SEARCH;
    // アプリの GetLastError と errno を変えない
    DWORD le = GetLastError();
    int en = errno;
    int hit = lz_fault((uint8_t *) er->ExceptionInformation[1], er->ExceptionAddress, er->ExceptionInformation[0] == 1);
    errno = en;
    SetLastError(le);
    return hit ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

// ドライバが卸される（vkDestroyInstance）ときは例外処理を外し、残っている古いページは開けておく
__attribute__((destructor)) static void lz_unload(void) {
    if (!g_lz_veh) return;
    RemoveVectoredExceptionHandler(g_lz_veh);
    g_lz_veh = NULL;
    for (LONG i = 0; i < g_lz_n; i++)
        for (size_t pg = 0; pg < g_lz_m[i]->npages; pg++)
            if (g_lz_m[i]->lz->stale[pg]) {
                DWORD old;
                VirtualProtect(g_lz_m[i]->shadow + pg * PAGE, PAGE, PAGE_READWRITE, &old);
            }
}

// ドライバが写しの off から len バイトに触る前：その中の古いページを先に読む
static void lz_settle(struct mem *m, uint64_t off, uint64_t len, int why) {
    if (!m || !m->lz || !m->lz->nstale || !len) return;
    lz_lock();
    lz_fill(m, (size_t) (off / PAGE), (size_t) ((off + len + PAGE - 1) / PAGE), why);
    lz_unlock();
}

// 送る前：書いた印のあるページに古い所があれば、先に読む（アプリが GPU の終わる前に、同じページの別の所に書いた）
static void lz_settle_dirty(struct mem *m, size_t p0, size_t p1) {
    if (!m->lz || !m->lz->nstale) return;
    lz_lock();
    struct lazy *lz = m->lz;
    if (p1 > m->npages) p1 = m->npages;
    for (size_t i = p0; i < p1;) {
        if (!m->dirty[i] || !lz->stale[i]) { i++; continue; }
        size_t j = i;
        while (j < p1 && m->dirty[j] && lz->stale[j]) j++;
        lz_heat(lz, i, j);
        lz_fill(m, i, j, LZ_SEND);
        i = j;
    }
    lz_unlock();
}

// 引き取る前（read_back_many）：古いページが混じっていれば先に読み、読み終えるまでほかの糸に印を付けさせない（受け取りは写しに直接書くので）。
// 持ったら 1 を返す。lz_hold_end で放す
static int lz_hold(rng *r, size_t n) {
    if (!lz_mode()) return 0;
    lz_lock();
    for (size_t i = 0; i < n; i++) {
        struct mem *m = r[i].m;
        if (m->lz && m->lz->nstale && r[i].len)
            lz_fill(m, (size_t) (r[i].off / PAGE), (size_t) ((r[i].off + r[i].len + PAGE - 1) / PAGE), LZ_BACK);
    }
    return 1;
}

static void lz_hold_end(int held) {
    if (held) lz_unlock();
}

// m の [a, b) が、すぐ読む物（take の先頭 ne 個）とページを分け合うか
static int lz_near_eager(const rng *t, size_t ne, const struct mem *m, uint64_t a, uint64_t b) {
    uint64_t pa = a / PAGE * PAGE, pb = (b + PAGE - 1) / PAGE * PAGE;
    for (size_t i = 0; i < ne; i++)
        if (t[i].m == m && t[i].off < pb && t[i].off + t[i].len > pa) return 1;
    return 0;
}

// complete で引き取る範囲から、遅らせる所を抜き出して返す（take にはすぐ読む物が残る）。返した物は lz_mark に渡す。
// CPU が最近触ったページと、すぐ読む物とページを分け合う所は、すぐ読む方に回す
static rng *lz_split(rng **take, size_t *nt, size_t *ct, size_t *nlz) {
    *nlz = 0;
    if (!lz_mode()) return NULL;
    size_t n0 = *nt, k = 0, nin = 0, cout = 0;
    for (size_t i = 0; i < n0; i++) nin += (*take)[i].lazy != 0;
    if (!nin) return NULL;
    rng *in = malloc(nin * sizeof *in), *out = NULL;
    if (!in) { for (size_t i = 0; i < n0; i++) (*take)[i].lazy = 0; return NULL; }
    nin = 0;
    lz_lock();
    for (size_t i = 0; i < n0; i++) {
        rng x = (*take)[i];
        struct lazy *lz = x.lazy ? lz_get(x.m) : NULL;
        if (lz && lz->nr < LZ_MAXR) in[nin++] = x;
        else { x.lazy = 0; (*take)[k++] = x; }
    }
    *nt = k;
    for (size_t i = 0; i < nin; i++) {
        struct mem *m = in[i].m;
        struct lazy *lz = m->lz;
        uint64_t a = in[i].off, b = in[i].off + in[i].len;
        if (!lz->nhot && !lz_near_eager(*take, k, m, a, b)) {
            rng_add(&out, nlz, &cout, m, a, b - a, in[i].serial);
            continue;
        }
        // ページごとに決め、続いている所はまとめる
        uint64_t s = a;
        int cur = -1;
        for (uint64_t pa = a / PAGE * PAGE; pa < b; pa += PAGE) {
            size_t pg = (size_t) (pa / PAGE);
            uint64_t ps = pa < a ? a : pa, pe = pa + PAGE < b ? pa + PAGE : b;
            int now = lz->hot[pg] || lz_near_eager(*take, k, m, ps, pe);
            if (lz->hot[pg] && !--lz->hot[pg]) lz->nhot--;
            if (now != cur && cur >= 0) {
                if (cur) { rng_add(take, nt, ct, m, s, ps - s, in[i].serial); g_lzs.hot_bytes += ps - s; }
                else rng_add(&out, nlz, &cout, m, s, ps - s, in[i].serial);
                s = ps;
            }
            cur = now;
        }
        if (cur) { rng_add(take, nt, ct, m, s, b - s, in[i].serial); g_lzs.hot_bytes += b - s; }
        else rng_add(&out, nlz, &cout, m, s, b - s, in[i].serial);
    }
    lz_unlock();
    free(in);
    return out;
}

// すぐ読む物を読み終えたあと：lz_split が抜き出した範囲に古い印を付け、ページを閉じる。r は放す
static void lz_mark(rng *r, size_t n) {
    if (!n) { free(r); return; }
    rng *back = NULL;
    size_t nb = 0, cb = 0;
    lz_lock();
    if (!g_lz_veh) g_lz_veh = AddVectoredExceptionHandler(1, lz_veh);
    for (size_t i = 0; i < n; i++) {
        struct mem *m = r[i].m;
        struct lazy *lz = m->lz;
        uint64_t a = r[i].off, b = r[i].off + r[i].len;
        g_lzs.mark_n++;
        g_lzs.mark_bytes += r[i].len;
        if (lz && lzr_has(lz, a, b)) continue; // 毎コマ同じ所：もう古いまま
        if (!lz || !g_lz_veh || !lzr_add(lz, a, b)) { rng_add(&back, &nb, &cb, m, a, b - a, r[i].serial); continue; }
        // まだ開いているページを、続いている所ごとに閉じる
        size_t p0 = (size_t) (a / PAGE), p1 = (size_t) ((b + PAGE - 1) / PAGE), rs = p0;
        for (size_t pg = p0; pg <= p1; pg++) {
            if (pg < p1 && !lz->stale[pg]) continue;
            DWORD old;
            if (pg > rs && !VirtualProtect(m->shadow + rs * PAGE, (pg - rs) * PAGE, PAGE_NOACCESS, &old)) {
                // 閉じられなければ、その所は今すぐ読む
                say("読み戻しを遅らせるページを閉じられない（%lu）", GetLastError());
                lzr_cut(lz, (uint64_t) rs * PAGE, (uint64_t) pg * PAGE);
                uint64_t ca = (uint64_t) rs * PAGE > a ? (uint64_t) rs * PAGE : a, cz = (uint64_t) pg * PAGE < b ? (uint64_t) pg * PAGE : b;
                rng_add(&back, &nb, &cb, m, ca, cz - ca, r[i].serial);
            } else {
                for (size_t q = rs; q < pg; q++) lz->stale[q] = 1;
                lz->nstale += pg - rs;
            }
            rs = pg + 1;
        }
    }
    lz_unlock();
    if (nb) {
        g_lzs.back_n += nb;
        read_back_many(back, nb);
    }
    free(back);
    free(r);
}

// vkFreeMemory：中身は要らないので読まずに外す（写しはこのあと VirtualFree で消える）
static void lz_forget(struct mem *m) {
    if (!m->lz) return;
    lz_lock();
    for (LONG i = 0; i < g_lz_n; i++)
        if (g_lz_m[i] == m) {
            g_lz_m[i] = g_lz_m[g_lz_n - 1];
            g_lz_n = g_lz_n - 1;
            break;
        }
    free(m->lz->stale);
    free(m->lz->r);
    free(m->lz);
    m->lz = NULL;
    lz_unlock();
}

// vkDestroyBuffer：あとで同じメモリに別の buffer がつながれ、写しの番地が OS に渡されることがあるので、
// まだ終わっていない submit の分は遅らせないことにし、もう古い所は今読む
static void lz_buffer_gone(struct dev *d, struct buf *bf) {
    if (!lz_mode() || !bf || !bf->m || !bf->m->host) return;
    struct mem *m = bf->m;
    uint64_t a = bf->memoff, b = bf->memoff + bf->size;
    AcquireSRWLockExclusive(&d->lock);
    for (size_t i = 0; i < d->npend; i++)
        if (d->pend[i].m == m && d->pend[i].off < b && d->pend[i].off + d->pend[i].len > a) d->pend[i].lazy = 0;
    ReleaseSRWLockExclusive(&d->lock);
    lz_settle(m, a, bf->size, LZ_OTHER);
}

// vkMapMemory：アプリが写しの番地を受け取る。古い所をここで全部読む（IPHONE_VK_LAZY_MAP=0 で読まない）。いつも map したままのアプリには効かない
static void lz_on_map(struct mem *m) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_LAZY_MAP"); v = !(e && *e == '0'); }
    g_lzs.maps++;
    if (!m->lz || !m->lz->nstale) return;
    g_lzs.map_stale++;
    if (v) lz_settle(m, 0, m->size, LZ_OTHER);
}

// アプリが「これから読む」と言った所は先に読む
static VkResult VKAPI_CALL invalidate_ranges(VkDevice dv, uint32_t n, const VkMappedMemoryRange *r) {
    (void) dv;
    for (uint32_t i = 0; i < n; i++) {
        struct mem *m = (struct mem *) r[i].memory;
        if (!m || !m->lz) continue;
        uint64_t len = r[i].size == VK_WHOLE_SIZE ? (r[i].offset < m->size ? m->size - r[i].offset : 0) : r[i].size;
        lz_settle(m, r[i].offset, len, LZ_OTHER);
    }
    return VK_SUCCESS;
}

static void lz_print(void) {
    if (!g_lzs.mark_n && !g_lzs.fill_n[LZ_FAULT] && !g_lzs.fill_n[LZ_OTHER]) return;
    fprintf(stderr, "  読み戻しを遅らせた：古い印 %llu 回 %.1f MB・読んだ時間 %.1f ms・触ったページを待ったときに読んだ %.2f MB・印を付けられずすぐ読んだ %llu\n",
            (unsigned long long) g_lzs.mark_n, g_lzs.mark_bytes / 1048576.0, g_lzs.fill_ms, g_lzs.hot_bytes / 1048576.0, (unsigned long long) g_lzs.back_n);
    fprintf(stderr, "    あとで読んだ：CPU が触った %llu 回 %.2f MB・送る前 %llu 回 %.2f MB・引き取る前 %llu 回 %.2f MB・"
                    "buffer を消す・invalidate・map・BC %llu 回 %.2f MB・古い所のあるメモリの map %llu / %llu 回\n",
            (unsigned long long) g_lzs.fill_n[LZ_FAULT], g_lzs.fill_bytes[LZ_FAULT] / 1048576.0, (unsigned long long) g_lzs.fill_n[LZ_SEND],
            g_lzs.fill_bytes[LZ_SEND] / 1048576.0, (unsigned long long) g_lzs.fill_n[LZ_BACK], g_lzs.fill_bytes[LZ_BACK] / 1048576.0,
            (unsigned long long) g_lzs.fill_n[LZ_OTHER], g_lzs.fill_bytes[LZ_OTHER] / 1048576.0, (unsigned long long) g_lzs.map_stale,
            (unsigned long long) g_lzs.maps);
}

#else
// ---- 読み戻しの遅延（Linux 版には無い） ----
// 遅らせる仕組みは、ページを PAGE_NOACCESS にして例外（VEH）で読み戻すので、Linux では使わない。
// lz_mode が 0 のままなので、GPU が書いた所は待ったときにすぐ全部読み戻す（IPHONE_VK_LAZY_RB=0 と同じ）
enum { LZ_FAULT, LZ_SEND, LZ_BACK, LZ_OTHER };
LINUX_UNUSED static int lz_mode(void) { return 0; }
static int lz_why_ok(int w) { (void) w; return 0; }
static void lz_settle(struct mem *m, uint64_t off, uint64_t len, int why) { (void) m; (void) off; (void) len; (void) why; }
static void lz_settle_dirty(struct mem *m, size_t p0, size_t p1) { (void) m; (void) p0; (void) p1; }
static int lz_hold(rng *r, size_t n) { (void) r; (void) n; return 0; }
static void lz_hold_end(int held) { (void) held; }
static rng *lz_split(rng **take, size_t *nt, size_t *ct, size_t *nlz) { (void) take; (void) nt; (void) ct; *nlz = 0; return NULL; }
static void lz_mark(rng *r, size_t n) { (void) n; free(r); }
static void lz_forget(struct mem *m) { (void) m; }
static void lz_buffer_gone(struct dev *d, struct buf *bf) { (void) d; (void) bf; }
static void lz_on_map(struct mem *m) { (void) m; }
static void lz_print(void) {}
static VkResult VKAPI_CALL invalidate_ranges(VkDevice dv, uint32_t n, const VkMappedMemoryRange *r) { (void) dv; (void) n; (void) r; return VK_SUCCESS; }
#endif

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
    if (memw_upto_on()) vr_u64(b, memw_upto(m, off, len)); // 古い受け手は読まない
    rpc_send(VR_MEM_WRITE, b);
    b->n = 0;
}

// 調べ用：IPHONE_VK_DUMP_MEMW=パスの頭 で、512 KB 以上の MEM_WRITE の中身を「頭_番号_off_len.bin」に最大 40 個書く（動画のコマの中身を調べる用。IPHONE_VK_WDIFF=0 と一緒に）
static void memw_dump(struct mem *m, uint64_t off, uint64_t len) {
    static int n = -1;
    static const char *pre;
    static double t_start, after;
    if (n < 0) {
        pre = getenv("IPHONE_VK_DUMP_MEMW"); n = 0; t_start = now_ms();
        const char *a = getenv("IPHONE_VK_DUMP_MEMW_AFTER"); // 起動からこの秒数がたってから書く
        after = a ? atof(a) * 1000.0 : 0;
    }
    if (!pre || !*pre || len < 524288 || n >= 40 || now_ms() - t_start < after) return;
    char path[700];
    snprintf(path, sizeof path, "%s_%02d_%llx_%llu.bin", pre, n++, (unsigned long long) off, (unsigned long long) len);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(m->shadow + off, 1, (size_t) len, f); fclose(f); }
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
    lz_settle_dirty(m, p0, p1);
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
        memw_dump(m, off, len);
        vr_u64(&b, ID(m));
        vr_u64(&b, off);
        vr_bytes(&b, m->shadow + off, (uint32_t) len);
        if (memw_upto_on()) vr_u64(&b, memw_upto(m, off, len));
        rpc_send(VR_MEM_WRITE, &b);
        i = j;
    }
    free(b.p);
}

// いくつもの範囲を 1 回の往復で引き取る。頼みを全部送ってから、答えを順に受け取る
static void read_back_many(rng *r, size_t n) {
    int lzh = lz_hold(r, n); // 古いページが混じっていれば先に読む（受け取りは写しに直接書くので）
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
            // アドレスで書かれうるメモリは、受け手がその書き手を知らないので、書いた submit の番号まで待ってもらう（古い受け手は末尾を読まない）
            if (op == VR_MEM_READ && r[i].m->wide && memw_upto_on()) vr_u64(&b, memw_upto(r[i].m, at, k));
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
#ifdef _WIN32
        ResetWriteWatch(r[i].m->shadow + p0, (SIZE_T) (p1 - p0));
#else
        (void) p0; (void) p1; // 書き込み監視が無い
#endif
    }
    lz_hold_end(lzh);
}

// キュー q（NULL なら全部）の serial までの submit が終わった：GPU が書いた範囲を引き取る
static void send_dirty(struct mem *m, uint64_t off, uint64_t len);
static void pb_release(struct pbatch *fr, size_t nfr) {
    for (size_t i = 0; i < nfr; i++) {
        for (uint32_t j = 0; j < fr[i].ncb; j++) {
            struct cb *c = fr[i].cbs[j];
            for (size_t k = 0; k < c->nrd; k++) send_dirty(c->rd[k].m, c->rd[k].off, c->rd[k].len);
            for (size_t k = 0; k < c->nr; k++) send_dirty(c->r[k].m, c->r[k].off, c->r[k].len);
        }
        free(fr[i].w);
        free(fr[i].cbs);
    }
    free(fr);
}

// q の upto までの submit が始まれるようになるまで待つ（ほかのスレッドの合図を待つので、錠は持たずに待つ）。
// ms 待っても解けなければ 0。ドライバの読み違いで固まらないよう、complete からは 10 秒で諦めて前のやり方に戻る
static int pb_wait(struct dev *d, struct queue *q, uint64_t upto, double ms) {
    double t0 = now_ms();
    int said = 0;
    for (;;) {
        AcquireSRWLockExclusive(&d->lock);
        struct pbatch *fr;
        size_t nfr;
        pb_resolve_locked(d, &fr, &nfr);
        int blocked = pb_blocks(d, q, upto);
        ReleaseSRWLockExclusive(&d->lock);
        pb_release(fr, nfr);
        if (!blocked) return 1;
        if (now_ms() - t0 >= ms) return 0;
        if (!said && now_ms() - t0 > 1000) { say("CPU からの合図を待っている submit がある（ほかのスレッドの vkSignalSemaphore を待つ）"); said = 1; }
        Sleep(1);
    }
}

// ---- fence を本当の進み具合で答える ----
// g_done_msg（受け手が「終わった」と答えた submit の番号）は、返事の列のところで決めている
static int g_engine_dxvk_any;        // アプリのエンジンが DXVK（版を問わない）
static volatile LONG g_ndev;         // 生きている装置の数（全部なくなったら submit の番号を数え直す）

static int real_fence(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("IPHONE_VK_REAL_FENCE");
        on = e && *e ? *e == '1' : g_engine_dxvk_any;
        on = on && (g_hw.caps & VR_CAP_PROGRESS);
        if (on) say("fence は受け手の本当の進み具合で答える");
    }
    return on;
}

static int progress_async(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_PROGRESS_ASYNC"); v = !(e && *e == '0'); }
    return v;
}

// 進み具合の問い合わせを頼むだけ（答えを待っている物があれば頼まない）。答えは返事の列で g_done_msg に入る
static void progress_request(void) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_prog_out && !g_dead) {
        vr_buf b = { 0 };
        g_stat.calls[VR_GET_PROGRESS]++;
        put_msg_locked(VR_GET_PROGRESS, &b);
        free(b.p);
        if (flush_locked()) { g_prog_out = 1; pend_push_locked(PEND_PROGRESS); }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// 届いている返事だけを読む（待たない）。進み具合の答えを読めたら 1
static int progress_take(void) {
    int got = 0;
    AcquireSRWLockExclusive(&g_lock);
    double t0 = now_ms();
    WSAPOLLFD pf = { .fd = g_sock, .events = POLLRDNORM };
    while (g_prog_out && g_pend_n && !g_dead && WSAPoll(&pf, 1, 0) > 0) {
        int kind = g_pend[g_pend_head];
        if (!pend_read_one_locked()) break;
        if (kind == PEND_PROGRESS) { got = 1; break; }
    }
    if (g_dead) { g_pend_n = 0; g_prog_out = 0; }
    g_stat.wait_ms[VR_GET_PROGRESS] += now_ms() - t0;
    ReleaseSRWLockExclusive(&g_lock);
    return got;
}

// 受け手に終わった submit の番号を聞く（GPU は待たない）。
// 錠を持たずに待つやり方では、頼んで、届いていれば読むだけ（届いていなければ前の答えのまま）
static void progress_poll(void) {
    if (progress_async()) {
        progress_request();
        progress_take();
        return;
    }
    vr_buf b = { 0 }, out = { 0 };
    if (rpc_call(VR_GET_PROGRESS, &b, &out) == VK_SUCCESS && out.n >= 8) {
        uint64_t v;
        memcpy(&v, out.p, 8);
        if (v > g_done_msg) g_done_msg = v;
    }
    free(b.p);
    free(out.p);
}

// submit の番号 msg まで終わるのを待つ。ms 待っても終わらなければ 0。
// 錠を持たずに待つやり方では、問い合わせを 1 つ出しておき、答えが届くまで錠を放して 1 ms ずつ眠る。
// 待っている間も、ほかの糸は submit を送れる
static int progress_wait(uint64_t msg, double ms) {
    if (!msg || g_done_msg >= msg) return 1;
    double t0 = now_ms();
    for (int n = 0;; n++) {
        if (progress_async()) {
            progress_request();
            if (!progress_take()) Sleep(n < 2 ? 0 : 1);
        } else {
            progress_poll();
        }
        if (g_done_msg >= msg || g_dead) return 1;
        if (now_ms() - t0 >= ms) return 0;
        if (!progress_async() && n > 2) Sleep(1);
    }
}

static void complete(struct dev *d, struct queue *q, uint64_t upto) {
    if (d->npb && !pb_wait(d, q, upto, 10000)) say("合図が 10 秒来ないので、待たずに進める");
    AcquireSRWLockExclusive(&d->lock);
    rng *take = NULL;
    size_t nt = 0, ct = 0, k = 0;
    for (size_t i = 0; i < d->npend; i++) {
        if (d->pend[i].serial <= upto && (!q || d->pend[i].q == q)) {
            if (!d->pend[i].m->jpeg) {
                size_t b0 = nt;
                rng_add(&take, &nt, &ct, d->pend[i].m, d->pend[i].off, d->pend[i].len, d->pend[i].serial);
                if (nt > b0) take[b0].lazy = d->pend[i].lazy;
            }
        }
        else d->pend[k++] = d->pend[i];
    }
    d->npend = k;
    ReleaseSRWLockExclusive(&d->lock);
    size_t nlz;
    rng *lzr = lz_split(&take, &nt, &ct, &nlz); // 遅らせる所は、すぐ読む物を読んでから印を付ける
    rng_merge(take, &nt);
    double t0 = now_ms();
    if (nt) read_back_many(take, nt);
    lz_mark(lzr, nlz);
    g_stat.rb_ms += now_ms() - t0;
    free(take);
}

// ---- インスタンスと装置の性質 ----
static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void) a;
    const VkApplicationInfo *ai = ci ? ci->pApplicationInfo : NULL;
    if (ai) say("アプリの名前：%s・エンジン：%s", ai->pApplicationName ? ai->pApplicationName : "(なし)", ai->pEngineName ? ai->pEngineName : "(なし)");
    if (ai && ai->pEngineName && !strcmp(ai->pEngineName, "ANGLE")) g_app_angle = 1;
    if (ai && ai->pEngineName) {
        uint32_t major = (ai->engineVersion >> 22) & 0x7F; // VK_MAKE_VERSION の形
        say("アプリのエンジン：%s %u.%u.%u", ai->pEngineName, major, (ai->engineVersion >> 12) & 0x3FF, ai->engineVersion & 0xFFF);
        if (!strcmp(ai->pEngineName, "DXVK") && major >= 2) g_engine_dxvk2 = 1;
        if (!strcmp(ai->pEngineName, "DXVK")) g_engine_dxvk_any = 1;
        if (!strcmp(ai->pEngineName, "mesa zink")) g_engine_zink = 1;
        if (!strcmp(ai->pEngineName, "vkd3d")) g_engine_vkd3d = 1;
        // DirectX 12 の feature level 12_0 を名乗らせる。vkd3d-proton は tiled resources（sparse）が無いと 11_x までにするが、
        // 起動時に 12_0 が作れるかだけを見るゲームがある。VKD3D_FEATURE_LEVEL は装置を作るときに読まれる。
        // sparse はふつうのメモリで持つ形で名乗る（IPHONE_VK_SPARSE）。利用者が自分で値を入れていれば触らない。IPHONE_VK_FL12=0 で立てない
        const char *fl = getenv("IPHONE_VK_FL12");
        if (g_engine_vkd3d && !(fl && *fl == '0') && !GetEnvironmentVariableA("VKD3D_FEATURE_LEVEL", NULL, 0)) {
            SetEnvironmentVariableA("VKD3D_FEATURE_LEVEL", "12_0");
            say("DirectX 12 の feature level 12_0 を名乗らせる（VKD3D_FEATURE_LEVEL=12_0）");
        }
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
        { "VK_KHR_get_physical_device_properties2", 2 },
#ifdef _WIN32
        { "VK_KHR_surface", 25 }, { "VK_KHR_win32_surface", 6 }
#endif
    };
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

// vkd3d-proton で、同時につなげる set が 9 個に足りない GPU（iPhone は 8 個）
static int sets8(void) { return vkd3d() && g_hw.props.limits.maxBoundDescriptorSets < 9 && g_hw.props.limits.minStorageBufferOffsetAlignment < 64; }

static void VKAPI_CALL get_props(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void) pd;
    *p = g_hw.props;
    // vkd3d-proton は storage buffer の揃えが 16 バイト以下だと、raw buffer 用の set を 1 つ足す（static sampler があると全部で 9 個）。
    // 同時につなげる set が 8 個の GPU（iPhone）では root signature を作れないので、揃えを 64 と名乗って 8 個に収める
    // （raw buffer は texel buffer で読む。揃えを大きく名乗るのは、アプリに厳しくするだけなので安全）
    if (sets8()) p->limits.minStorageBufferOffsetAlignment = 64;
    // sparse の名乗り（S1）。2D・1 サンプルの標準の形だけ。3D と MSAA は名乗らない
    if (sparse_on()) {
        p->limits.sparseAddressSpaceSize = sparse_cap();
        memset(&p->sparseProperties, 0, sizeof p->sparseProperties);
        p->sparseProperties.residencyStandard2DBlockShape = VK_TRUE;
        p->sparseProperties.residencyNonResidentStrict = sparse_strict() ? VK_TRUE : VK_FALSE;
    }
}

static void VKAPI_CALL get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    say("機能の一覧");
    (void) pd;
    *f = g_hw.feats;
    // sparse の名乗り（S1）。aliased は名乗るが守らない（同じメモリを 2 か所に結んでも中身は共有されない）。
    // shaderResourceResidency はシェーダーを直して「いつもある」と答える（spv_sparse）
    if (sparse_on()) {
        f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyAliased = VK_TRUE;
        f->shaderResourceResidency = VK_TRUE;
    }
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
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, sizeof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT, sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT) },
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
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT, sizeof(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT) },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES, sizeof(VkPhysicalDeviceTexelBufferAlignmentProperties) },
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
        if (vkd3d()) {
            // vkd3d-proton は descriptor heap を、種類ごとの大きな set（update after bind・数は確保のときに決める）で作る。
            // シェーダーの印は受け手の GPU の答えのまま。binding の flags と確保の数は受け手に運ぶ（VR_CAP_DESC_FLAGS）
            f->shaderInputAttachmentArrayDynamicIndexing = k.shaderInputAttachmentArrayDynamicIndexing;
            f->shaderUniformTexelBufferArrayDynamicIndexing = k.shaderUniformTexelBufferArrayDynamicIndexing;
            f->shaderStorageTexelBufferArrayDynamicIndexing = k.shaderStorageTexelBufferArrayDynamicIndexing;
            f->shaderUniformBufferArrayNonUniformIndexing = k.shaderUniformBufferArrayNonUniformIndexing;
            f->shaderSampledImageArrayNonUniformIndexing = k.shaderSampledImageArrayNonUniformIndexing;
            f->shaderStorageBufferArrayNonUniformIndexing = k.shaderStorageBufferArrayNonUniformIndexing;
            f->shaderStorageImageArrayNonUniformIndexing = k.shaderStorageImageArrayNonUniformIndexing;
            f->shaderInputAttachmentArrayNonUniformIndexing = k.shaderInputAttachmentArrayNonUniformIndexing;
            f->shaderUniformTexelBufferArrayNonUniformIndexing = k.shaderUniformTexelBufferArrayNonUniformIndexing;
            f->shaderStorageTexelBufferArrayNonUniformIndexing = k.shaderStorageTexelBufferArrayNonUniformIndexing;
            f->descriptorBindingUniformBufferUpdateAfterBind = k.descriptorBindingUniformBufferUpdateAfterBind;
            f->descriptorBindingStorageImageUpdateAfterBind = k.descriptorBindingStorageImageUpdateAfterBind;
            f->descriptorBindingStorageBufferUpdateAfterBind = k.descriptorBindingStorageBufferUpdateAfterBind;
            f->descriptorBindingUniformTexelBufferUpdateAfterBind = k.descriptorBindingUniformTexelBufferUpdateAfterBind;
            f->descriptorBindingStorageTexelBufferUpdateAfterBind = k.descriptorBindingStorageTexelBufferUpdateAfterBind;
            // 数を確保のときに決める set は、受け手が数を読めないと取れない（古い受け手は 100 万個の set を取ろうとして失敗する）。
            // そのときは名乗らない。vkd3d-proton は「descriptor indexing が足りない」と言って装置を作らない（受け手の組み直しが要る）
            f->descriptorBindingVariableDescriptorCount = (g_hw.caps & VR_CAP_DESC_FLAGS) ? k.descriptorBindingVariableDescriptorCount : VK_FALSE;
            if (!(g_hw.caps & VR_CAP_DESC_FLAGS)) {
                static LONG once;
                if (!InterlockedExchange(&once, 1)) say("受け手が descriptor の数を読めない（古い受け手）ので、DirectX 12 は動かせない。受け手の組み直しが要る");
            }
            f->shaderOutputViewportIndex = k.shaderOutputViewportIndex;
            f->shaderOutputLayer = k.shaderOutputLayer;
            f->separateDepthStencilLayouts = k.separateDepthStencilLayouts;
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

// 鎖の構造体の中身（sType と pNext の後ろ）の、受け手（64 ビット）での長さ。
// 32 ビットでは頭が 8 バイト短く、末尾の詰め物も 8 にそろわないことがあるので、中身を 8 の倍数に切り上げる
// （伝えた大きさの箱に受け手の GPU が書くので、小さく伝えると受け手ではみ出す）
static uint32_t wire_body(uint32_t sz) {
#if UINTPTR_MAX == 0xffffffffu
    return (sz - (uint32_t) sizeof(VkBaseOutStructure) + 7u) & ~7u;
#else
    return sz - (uint32_t) sizeof(VkBaseOutStructure);
#endif
}

// 中身を受け手の長さで送る（足りない分は 0 で埋める）
static void put_body(vr_buf *b, const void *body, uint32_t sz) {
    uint32_t n = sz - (uint32_t) sizeof(VkBaseOutStructure), w = wire_body(sz);
    if (w == n) { vr_bytes(b, body, n); return; }
    uint8_t tmp[1024] = { 0 };
    if (w > sizeof tmp) w = sizeof tmp;
    memcpy(tmp, body, n < w ? n : w);
    vr_bytes(b, tmp, w);
}

// pNext の鎖のうち、知っている構造体だけを受け手に尋ねて埋める
static void query_chain(uint32_t op, void *pnext, const stinfo *t, size_t nt, void *base, size_t base_size) {
    vr_buf b = { 0 }, out = { 0 };
    VkBaseOutStructure *items[64];
    uint32_t n = 0;
    for (VkBaseOutStructure *s = pnext; s && n < 64; s = s->pNext)
        if (st_size(t, nt, s->sType)) items[n++] = s;
    vr_u32(&b, n);
    for (uint32_t i = 0; i < n; i++) { vr_u32(&b, items[i]->sType); vr_u32(&b, 16u + wire_body(st_size(t, nt, items[i]->sType))); }
#if UINTPTR_MAX == 0xffffffffu
    if (base_size == sizeof(VkPhysicalDeviceProperties)) base_size = 824; // 受け手（64 ビット）の並びの大きさ
#endif
    if (rpc_call(op, &b, &out) == VK_SUCCESS) {
        vr_rd rd = { out.p, out.n, 0, 0 };
        vr_take(&rd, base_size); // 土台の部分は手元のものを使う
        for (uint32_t i = 0; i < n; i++) {
            uint32_t sz = st_size(t, nt, items[i]->sType), nb = sz - (uint32_t) sizeof(VkBaseOutStructure), w = wire_body(sz);
            vr_rcopy(&rd, BODY(items[i]), nb);
            if (w > nb) vr_take(&rd, w - nb);
            if (op == VR_GET_FEATURES2) mask_ext_features(items[i]);
            else fix_ids(items[i]);
        }
    }
    (void) base;
    free(b.p);
    free(out.p);
}

// vkd3d-proton のときに直す性質
static void vkd3d_props(VkBaseOutStructure *x) {
    // vkd3d-proton は NVIDIA のドライバだと storage buffer の揃えを 4 に戻す（速さのため）。set が 8 個の受け手
    // （iPhone のふりの代役の RTX）では、iPhone と同じ道を通すため MoltenVK と名乗る。iPhone は元から MoltenVK
    if (sets8() && x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES)
        ((VkPhysicalDeviceVulkan12Properties *) x)->driverID = VK_DRIVER_ID_MOLTENVK;
    else if (sets8() && x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES)
        ((VkPhysicalDeviceDriverProperties *) x)->driverID = VK_DRIVER_ID_MOLTENVK;
    if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES) {
        // 1 要素の揃えで texel buffer の見え方を作れると名乗る（vkd3d-proton は持たないと装置を作らない）。
        // 受け手の GPU が持たないとき（iPhone は 64 バイト）、揃っていない見え方を作ったら記録に出す
        VkPhysicalDeviceVulkan13Properties *p = (void *) x;
        p->storageTexelBufferOffsetSingleTexelAlignment = p->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES) {
        VkPhysicalDeviceTexelBufferAlignmentProperties *p = (void *) x;
        p->storageTexelBufferOffsetSingleTexelAlignment = p->uniformTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT) {
        // 名乗りだけ。数は DirectX 12 の stream output の上限に合わせる
        VkPhysicalDeviceTransformFeedbackPropertiesEXT *p = (void *) x;
        p->maxTransformFeedbackStreams = 4;
        p->maxTransformFeedbackBuffers = 4;
        p->maxTransformFeedbackBufferSize = 0xFFFFFFFFull;
        p->maxTransformFeedbackStreamDataSize = 512;
        p->maxTransformFeedbackBufferDataSize = 512;
        p->maxTransformFeedbackBufferDataStride = 2048;
        p->transformFeedbackQueries = VK_TRUE;
        p->transformFeedbackStreamsLinesTriangles = VK_TRUE;
        // zink には GS の stream を名乗らない（geometryStreams が無い）ので、決まりどおり stream は 1 本
        if (!vkd3d()) { p->maxTransformFeedbackStreams = 1; p->transformFeedbackStreamsLinesTriangles = VK_FALSE; }
        p->transformFeedbackRasterizationStreamSelect = VK_FALSE;
        p->transformFeedbackDraw = VK_TRUE;
    }
}

static void VKAPI_CALL get_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    get_props(pd, &p->properties);
    query_chain(VR_GET_PROPERTIES2, p->pNext, PROP_ST, sizeof PROP_ST / sizeof *PROP_ST, &p->properties, sizeof p->properties);
    for (VkBaseOutStructure *x = p->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR)
            ((VkPhysicalDevicePushDescriptorPropertiesKHR *) x)->maxPushDescriptors = 32;
        else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES) {
            // 受け手に渡す表には KHR の性質の形が無いので、EXT の性質を聞いて写す。最初の instance が 0 でないときの step rate は名乗らない
            VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT e = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT };
            VkPhysicalDeviceProperties dummy;
            query_chain(VR_GET_PROPERTIES2, &e, PROP_ST, sizeof PROP_ST / sizeof *PROP_ST, &dummy, sizeof dummy);
            VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR *k = (void *) x;
            k->maxVertexAttribDivisor = e.maxVertexAttribDivisor;
            k->supportsNonZeroFirstInstance = VK_FALSE;
        }
        else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT) {
            VkPhysicalDeviceProvokingVertexPropertiesEXT *pv = (void *) x;
            pv->provokingVertexModePerPipeline = provoking_named() ? VK_TRUE : VK_FALSE;
            pv->transformFeedbackPreservesTriangleFanProvokingVertex = VK_FALSE;
        }
        else if (vkd3d() || (xfb_named() && x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT)) vkd3d_props(x);
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
            // vkd3d-proton は画像の範囲の外も 0 と名乗らないと装置を作らない。GPU の robustImageAccess に任せる（名乗りだけ）
            r2->robustImageAccess2 = vkd3d() && on ? VK_TRUE : VK_FALSE;
        } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
            // 名乗りだけ（vkd3d-proton は持たないと装置を作らない）。stream output の命令は飛ばす
            VkPhysicalDeviceTransformFeedbackFeaturesEXT *t = (void *) x;
            t->transformFeedback = xfb_named() ? VK_TRUE : VK_FALSE;
            // zink は geometryStreams があると GS の stream を使う道を開けるが、GS は zink に名乗っていないので要らない
            t->geometryStreams = vkd3d() ? VK_TRUE : VK_FALSE;
        } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT) {
            VkPhysicalDeviceProvokingVertexFeaturesEXT *pv = (void *) x;
            pv->provokingVertexLast = provoking_named() ? VK_TRUE : VK_FALSE;
            pv->transformFeedbackPreservesProvokingVertex = VK_FALSE;
        } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT && !(vkd3d() && g_srv_unused_att)) {
            // 受け手の答えのまま。vkd3d-proton のとき以外と、受け手の GPU が持たないときは「無い」
            ((VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT *) x)->dynamicRenderingUnusedAttachments = VK_FALSE;
        }
}

// sparse（S1）を名乗るときは、描く・計算するキューに sparse binding の印を足す（vkQueueBindSparse は合図だけの submit にする）
static VkQueueFamilyProperties qf_fix(VkQueueFamilyProperties q) {
    if (sparse_on() && (q.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) q.queueFlags |= VK_QUEUE_SPARSE_BINDING_BIT;
    return q;
}

static void VKAPI_CALL get_queue_families(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *q) {
    say("キューの一覧");
    (void) pd;
    if (!q) { *n = g_hw.nq; return; }
    uint32_t k = *n < g_hw.nq ? *n : g_hw.nq;
    for (uint32_t i = 0; i < k; i++) q[i] = qf_fix(g_hw.q[i]);
    *n = k;
}

static void VKAPI_CALL get_queue_families2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *q) {
    if (!q) { get_queue_families(pd, n, NULL); return; }
    uint32_t k = *n < g_hw.nq ? *n : g_hw.nq;
    for (uint32_t i = 0; i < k; i++) q[i].queueFamilyProperties = qf_fix(g_hw.q[i]);
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

static void vf_fix_format(VkFormat f, VkFormatProperties *p, VkFormatProperties3 *p3); // 頂点の形式の真似（create_graphics_pipelines のそば）
static void VKAPI_CALL get_format(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p) {
    (void) pd;
    get_format_ex(f, p, NULL);
    vf_fix_format(f, p, NULL);
}

static void VKAPI_CALL get_format2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    (void) pd;
    VkFormatProperties3 *p3 = NULL;
    for (VkBaseOutStructure *s = p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) p3 = (VkFormatProperties3 *) s;
    get_format_ex(f, &p->formatProperties, p3);
    vf_fix_format(f, &p->formatProperties, p3);
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
    // sparse（S1）：受け手にはふつうの image として作るので、印を外して聞く。residency は 2D・optimal・標準の形のある色の形式・1 サンプルだけ
    if (sparse_on() && (fl & SPARSE_IMG_FLAGS)) {
        uint32_t gw, gh, by, bw, bh;
        if ((fl & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) && (t != VK_IMAGE_TYPE_2D || ti != VK_IMAGE_TILING_OPTIMAL || !sparse_shape(f, &gw, &gh, &by, &bw, &bh))) {
            memset(p, 0, sizeof *p);
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        VkResult r = get_image_format(pd, f, t, ti, u, fl & ~(VkImageCreateFlags) SPARSE_IMG_FLAGS, p);
        if (r == VK_SUCCESS && (fl & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT)) p->sampleCounts &= VK_SAMPLE_COUNT_1_BIT;
        return r;
    }
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

// sparse の image の形式の性質（S1）。2D・1 サンプル・optimal で、標準の形のある色の形式だけ 1 つ答える（ほかは 0 個）
static int sparse_fmt_props(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageTiling ti,
                            VkSparseImageFormatProperties *q) {
    uint32_t gw, gh, by, bw, bh;
    if (!sparse_on() || t != VK_IMAGE_TYPE_2D || s != VK_SAMPLE_COUNT_1_BIT || ti != VK_IMAGE_TILING_OPTIMAL || !sparse_shape(f, &gw, &gh, &by, &bw, &bh))
        return 0;
    VkImageFormatProperties ip;
    if (get_image_format(pd, f, t, ti, u, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT, &ip) != VK_SUCCESS) return 0;
    q->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    q->imageGranularity = (VkExtent3D) { gw, gh, 1 };
    q->flags = 0;
    return 1;
}

static void VKAPI_CALL get_sparse_format(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageTiling ti, uint32_t *n, VkSparseImageFormatProperties *p) {
    VkSparseImageFormatProperties q;
    uint32_t k = (uint32_t) sparse_fmt_props(pd, f, t, s, u, ti, &q);
    if (!p) { *n = k; return; }
    if (*n && k) { p[0] = q; *n = 1; } else *n = 0;
}

static void VKAPI_CALL get_sparse_format2(VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *i, uint32_t *n, VkSparseImageFormatProperties2 *p) {
    VkSparseImageFormatProperties q;
    uint32_t k = (uint32_t) sparse_fmt_props(pd, i->format, i->type, i->samples, i->usage, i->tiling, &q);
    if (!p) { *n = k; return; }
    if (*n && k) { p[0].properties = q; *n = 1; } else *n = 0;
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
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (allowed_ext(ci->ppEnabledExtensionNames[i]) && fwd_ext(ci->ppEnabledExtensionNames[i], ci->ppEnabledExtensionNames, ci->enabledExtensionCount)) ne++;
    vr_u32(&b, ne);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *nm = allowed_ext(ci->ppEnabledExtensionNames[i]) ? fwd_ext(ci->ppEnabledExtensionNames[i], ci->ppEnabledExtensionNames, ci->enabledExtensionCount) : NULL;
        if (nm) vr_str(&b, nm);
    }
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
        // sparse は受け手では使わない（ドライバが真似る。iPhone の MoltenVK は持たない）。代役の GPU が持っていても有効にしない
        en.sparseBinding = en.sparseResidencyBuffer = en.sparseResidencyImage2D = en.sparseResidencyImage3D = VK_FALSE;
        en.sparseResidency2Samples = en.sparseResidency4Samples = en.sparseResidency8Samples = en.sparseResidency16Samples = VK_FALSE;
        en.sparseResidencyAliased = VK_FALSE;
        if (sparse_on()) en.shaderResourceResidency = VK_FALSE; // シェーダーの residency の問い合わせはドライバが直す（spv_sparse）
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
            put_body(&b, want + h, sz);
        } else {
            put_body(&b, BODY(chain[i]), sz);
        }
    }
    VkResult r = rpc_call(VR_CREATE_DEVICE, &b, NULL);
    free(b.p);
    if (r != VK_SUCCESS) { say("装置を作れない (%d)", r); free(d); return r; }
    say("装置を作った");
    InterlockedIncrement(&g_ndev);
    *out = (VkDevice) d;
    return VK_SUCCESS;
}

static void push_forget_dev(VkDevice dv);
static void VKAPI_CALL destroy_device(VkDevice dv, const VkAllocationCallbacks *a) {
    (void) a;
    if (!dv) return;
    push_forget_dev(dv);
    struct dev *d = (struct dev *) dv;
    vr_buf b = { 0 };
    vr_u64(&b, ID(d));
    rpc_send(VR_DESTROY_DEVICE, &b);
    rpc_flush();
    free(b.p);
    // 受け手は装置が全部なくなると submit の数を 0 に戻す（cleanup）。こちらも合わせないと、次の装置で
    // 「終わった submit の番号」が食い違い、fence を本当の進みで待つ所が戻らない（Unreal は試しの装置を壊してから作り直す）
    if (InterlockedDecrement(&g_ndev) <= 0) {
        AcquireSRWLockExclusive(&g_lock);
        g_submit_msgs = 0;
        g_done_msg = 0;
        ReleaseSRWLockExclusive(&g_lock);
    }
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
// 写しを OS に返す（Windows は VirtualFree・Linux は munmap）。ADDRS_OK：書き込み監視の受け皿が取れたか（Linux は受け皿が無い）
#ifdef _WIN32
#define ADDRS_OK(m) ((m)->addrs != NULL)
#else
#define ADDRS_OK(m) 1
#endif
static void shadow_free(struct mem *m) {
#ifdef _WIN32
    VirtualFree(m->shadow, 0, MEM_RELEASE);
#else
    munmap(m->shadow, m->npages * PAGE);
#endif
}

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
#ifdef _WIN32
        m->shadow = VirtualAlloc(NULL, m->npages * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        m->addrs = malloc(m->npages * sizeof *m->addrs);
#else
        m->shadow = mmap(NULL, m->npages * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); // 0 で埋まって、ページに揃う
        if (m->shadow == (uint8_t *) MAP_FAILED) m->shadow = NULL;
        m->addrs = NULL; // 書き込み監視が無いので受け皿は要らない
#endif
        m->dirty = calloc(m->npages, 1);
        if (!m->shadow || !ADDRS_OK(m) || !m->dirty) {
            if (m->shadow) shadow_free(m);
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
        m->aflags = fl;
    }
    VkResult r = rpc_call(VR_ALLOC_MEMORY, &b, NULL);
    free(b.p);
    if (r != VK_SUCCESS) {
        if (m->shadow) shadow_free(m);
        free(m->addrs);
        free(m->dirty);
        free(m);
        return r;
    }
    say("メモリ %.1f MiB（種類 %u%s%s）", m->size / 1048576.0, m->type, m->host ? "・CPU から見える" : "",
        (m->aflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) ? "・アドレスで使う" : "");
    if (m->host && (m->aflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT)) InterlockedIncrement(&g_bda_host);
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
        if (m->aflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) InterlockedDecrement(&g_bda_host);
    }
    size_t k = 0;
    for (size_t i = 0; i < d->npend; i++) if (d->pend[i].m != m) d->pend[k++] = d->pend[i];
    d->npend = k;
    ReleaseSRWLockExclusive(&d->lock);
    lz_forget(m);
    AcquireSRWLockExclusive(&g_muse_lock);
    free(m->use);
    m->use = NULL;
    m->nuse = m->cuse = 0;
    ReleaseSRWLockExclusive(&g_muse_lock);
    if (m->shadow) shadow_free(m);
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
    lz_on_map(m);
    *pp = m->shadow + off;
    return VK_SUCCESS;
}

static void VKAPI_CALL unmap_memory(VkDevice dv, VkDeviceMemory mm) { (void) dv; (void) mm; }

// 送るのは submit のとき、引き取るのは終わりを待ったときにまとめてするので、ここでは何もしない
static VkResult VKAPI_CALL flush_ranges(VkDevice dv, uint32_t n, const VkMappedMemoryRange *r) { (void) dv; (void) n; (void) r; return VK_SUCCESS; }

static void VKAPI_CALL get_commitment(VkDevice dv, VkDeviceMemory mm, VkDeviceSize *c) { (void) dv; *c = ((struct mem *) mm)->size; }

// ---- バッファ ----
// buffer の必要なメモリの量は、flags と使い道が同じなら、大きさから決まる GPU が多い。
// 並びの単位に切り上げる物（RTX など）と、大きさをそのまま返す物（iPhone の MoltenVK）がある。
// 受け手に聞いた答えから、どちらの型かが決まったら、次からは手元で計算して返事を待たない（先例 Venus と同じ考え方）。
// 大きさが並びの単位のちょうど倍のときは、どちらの型でも同じ答えになるので、決まるまで聞き続ける
enum { BRQ_UNKNOWN, BRQ_ROUND, BRQ_EXACT, BRQ_ASK };
static struct bufreq { int used, mode; uint32_t flags, usage; VkMemoryRequirements req; } g_brq[256];
static SRWLOCK g_brq_lock = SRWLOCK_INIT;

static int brq_find(uint32_t flags, uint32_t usage, VkDeviceSize size, VkMemoryRequirements *req) {
    int hit = 0;
    AcquireSRWLockShared(&g_brq_lock);
    for (int i = 0; i < 256 && g_brq[i].used; i++)
        if (g_brq[i].flags == flags && g_brq[i].usage == usage) {
            VkDeviceSize al = g_brq[i].req.alignment ? g_brq[i].req.alignment : 1;
            if (g_brq[i].mode == BRQ_ROUND || g_brq[i].mode == BRQ_EXACT) {
                *req = g_brq[i].req;
                req->size = g_brq[i].mode == BRQ_ROUND ? (size + al - 1) / al * al : size;
                hit = 1;
            } else if (g_brq[i].mode == BRQ_UNKNOWN && size % al == 0) {
                // 単位のちょうど倍なら、どちらの型でも答えは大きさそのもの
                *req = g_brq[i].req;
                req->size = size;
                hit = 1;
            }
            break;
        }
    ReleaseSRWLockShared(&g_brq_lock);
    return hit;
}

static void brq_learn(uint32_t flags, uint32_t usage, VkDeviceSize size, const VkMemoryRequirements *req) {
    VkDeviceSize al = req->alignment ? req->alignment : 1;
    VkDeviceSize up = (size + al - 1) / al * al;
    int mode = size % al == 0 ? (req->size == size ? BRQ_UNKNOWN : BRQ_ASK)
             : req->size == up ? BRQ_ROUND : req->size == size ? BRQ_EXACT : BRQ_ASK;
    AcquireSRWLockExclusive(&g_brq_lock);
    int i = 0;
    while (i < 256 && g_brq[i].used && !(g_brq[i].flags == flags && g_brq[i].usage == usage)) i++;
    if (i < 256 && !g_brq[i].used) {
        g_brq[i].used = 1;
        g_brq[i].flags = flags;
        g_brq[i].usage = usage;
        g_brq[i].req = *req;
        g_brq[i].mode = mode;
    } else if (i < 256 && g_brq[i].mode == BRQ_UNKNOWN && mode != BRQ_UNKNOWN) {
        // まだ決まっていなかった型が、この答えで決まった（並びの単位や memoryTypeBits が違えば聞き続ける）
        int same = g_brq[i].req.alignment == req->alignment && g_brq[i].req.memoryTypeBits == req->memoryTypeBits;
        g_brq[i].mode = same ? mode : BRQ_ASK;
    }
    ReleaseSRWLockExclusive(&g_brq_lock);
}

static uint32_t gs_buffer_usage(uint32_t usage);
static VkResult VKAPI_CALL bind_buffer(VkDevice dv, VkBuffer bb, VkDeviceMemory mm, VkDeviceSize off);
static void VKAPI_CALL destroy_buffer(VkDevice dv, VkBuffer bb, const VkAllocationCallbacks *a);

// sparse の buffer（S1）：アプリに答えるメモリの量は 64 KB の倍数、並びは 64 KB。
// 本当の必要量（受け手の答え）ぶんの隠しメモリを取って結ぶ。probe（作る前に必要量を聞くだけ）なら取らない
static VkResult sparse_hide_buffer(VkDevice dv, struct buf *bf, uint32_t usage, int probe) {
    VkMemoryRequirements real = bf->req;
    bf->req.size = up64k(bf->size);
    bf->req.alignment = SPARSE_BLOCK;
    if (probe) return VK_SUCCESS;
    if (!sparse_take(real.size, "buffer", bf->size)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateFlagsInfo fi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = fi.flags ? &fi : NULL,
                                .allocationSize = real.size, .memoryTypeIndex = sparse_mem_type(real.memoryTypeBits) };
    VkDeviceMemory m = VK_NULL_HANDLE;
    VkResult r = alloc_memory(dv, &ai, NULL, &m);
    if (r != VK_SUCCESS) { sparse_give(real.size); return r; }
    bind_buffer(dv, (VkBuffer) bf, m, 0);
    bf->hidden = (struct mem *) m;
    bf->hidden_size = real.size;
    return VK_SUCCESS;
}

static VkResult create_buffer_ex(VkDevice dv, const VkBufferCreateInfo *ci, VkBuffer *out, int probe) {
    struct buf *bf = calloc(1, sizeof *bf);
    if (!bf) return VK_ERROR_OUT_OF_HOST_MEMORY;
    bf->size = ci->size;
    // sparse（S1）：受け手にはふつうの buffer として作る
    int sp = (ci->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) && sparse_on();
    uint32_t flags = sp ? ci->flags & ~(uint32_t) SPARSE_BUF_FLAGS : ci->flags;
    vr_buf b = { 0 }, o = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bf));
    vr_u32(&b, flags);
    vr_u64(&b, ci->size);
    // maintenance5 では、使い道を pNext の 64 ビットの形で渡せる。今の受け手は 32 ビットまでしか使わない
    uint32_t usage = ci->usage;
    for (const VkBaseInStructure *x = ci->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO) usage = (uint32_t) ((const VkBufferUsageFlags2CreateInfo *) x)->usage;
    usage = gs_buffer_usage(usage); // GS を真似る VS が storage buffer として読む
    vr_u32(&b, usage);
    if ((g_hw.caps & VR_CAP_ASYNC_CREATE) && brq_find(flags, usage, ci->size, &bf->req)) {
        rpc_send(VR_CREATE_BUFFER_NR, &b);
        free(b.p);
    } else {
        VkResult r = rpc_call(VR_CREATE_BUFFER, &b, &o);
        if (r == VK_SUCCESS && o.n >= sizeof bf->req) {
            memcpy(&bf->req, o.p, sizeof bf->req);
            brq_learn(flags, usage, ci->size, &bf->req);
        }
        free(b.p);
        free(o.p);
        if (r != VK_SUCCESS) { free(bf); return r; }
    }
    if (sp) {
        VkResult r = sparse_hide_buffer(dv, bf, usage, probe);
        if (r != VK_SUCCESS) { destroy_buffer(dv, (VkBuffer) bf, NULL); return r; }
    }
    *out = (VkBuffer) bf;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_buffer(VkDevice dv, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void) a;
    return create_buffer_ex(dv, ci, out, 0);
}

static void forget_buffer_in_sets(struct buf *bf);

static void VKAPI_CALL destroy_buffer(VkDevice dv, VkBuffer bb, const VkAllocationCallbacks *a) {
    (void) a;
    if (!bb) return;
    lz_buffer_gone((struct dev *) dv, (struct buf *) bb);
    forget_buffer_in_sets((struct buf *) bb);
    vab_del((struct buf *) bb);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(bb));
    rpc_send(VR_DESTROY_BUFFER, &b);
    free(b.p);
    struct buf *bf = (struct buf *) bb;
    if (bf->hidden) { // sparse（S1）の隠しメモリは、buffer を消したあとで放す
        free_memory(dv, (VkDeviceMemory) bf->hidden, NULL);
        sparse_give(bf->hidden_size);
    }
    free(bf);
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
struct img { VkMemoryRequirements req; struct mem *m; uint64_t memoff; VkFormat bc; // bc：ほどいて持つ BC の形式
    uint32_t ls_fmt, ls_w, ls_h, ls_mips, ls_layers, ls_usage, ls_samples; // LOAD・STORE を調べる・減らすために覚える（ls.inc）
    struct spimg sp; struct mem *hidden; uint64_t hidden_size; }; // sparse（S1）の形と隠しメモリ
static void ls_note_rp1(uint64_t id, const VkRenderPassCreateInfo *ci, const vr_buf *pay);
static void ls_note_rp2(uint64_t id, const VkRenderPassCreateInfo2 *ci, const vr_buf *pay);
static void ls_note_fb(uint64_t id, const VkFramebufferCreateInfo *ci);
static void ls_note_view(uint64_t id, const VkImageViewCreateInfo *ci);
static void ls_drop(uint64_t id);


// 受け手の装置では swapchain を使わないので、PRESENT_SRC の形は GENERAL に読み替える
static VkImageLayout fix_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || l == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ? VK_IMAGE_LAYOUT_GENERAL : l;
}

static VkResult VKAPI_CALL bind_image(VkDevice dv, VkImage h, VkDeviceMemory mm, VkDeviceSize off);
static void VKAPI_CALL destroy_image(VkDevice dv, VkImage h, const VkAllocationCallbacks *a);

static struct spimg spimg_of(const VkImageCreateInfo *ci) {
    struct spimg s = { 1, (ci->flags & VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) != 0, ci->imageType, ci->format, ci->extent.width, ci->extent.height,
                       ci->extent.depth, ci->mipLevels, ci->arrayLayers, ci->samples, ci->tiling };
    return s;
}

// sparse の image（S1）：アプリに答えるメモリの量は、タイルの並び（sparse_img_layout）の仮の大きさ、並びは 64 KB。
// 本当の必要量（受け手の答え）ぶんの隠しメモリを取って結ぶ。probe なら取らない
static VkResult sparse_hide_image(VkDevice dv, struct img *im, int probe) {
    VkMemoryRequirements real = im->req;
    VkSparseImageMemoryRequirements sr;
    VkDeviceSize vsize = 0;
    im->req.size = sparse_img_layout(&im->sp, &sr, &vsize) ? vsize : up64k(real.size);
    im->req.alignment = SPARSE_BLOCK;
    if (probe) return VK_SUCCESS;
    if (!sparse_take(real.size, im->sp.residency ? "image" : "image（residency なし）", im->req.size)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = real.size, .memoryTypeIndex = sparse_mem_type(real.memoryTypeBits) };
    VkDeviceMemory m = VK_NULL_HANDLE;
    VkResult r = alloc_memory(dv, &ai, NULL, &m);
    if (r != VK_SUCCESS) { sparse_give(real.size); return r; }
    bind_image(dv, (VkImage) im, m, 0);
    im->hidden = (struct mem *) m;
    im->hidden_size = real.size;
    return VK_SUCCESS;
}

static VkResult create_image_ex(VkDevice dv, const VkImageCreateInfo *ci, VkImage *out, int probe) {
    struct img *im = calloc(1, sizeof *im);
    if (!im) return VK_ERROR_OUT_OF_HOST_MEMORY;
    im->ls_fmt = ci->format; im->ls_w = ci->extent.width; im->ls_h = ci->extent.height; im->ls_mips = ci->mipLevels; im->ls_layers = ci->arrayLayers;
    im->ls_usage = ci->usage; im->ls_samples = ci->samples;
    // sparse（S1）：形を覚えて、受け手にはふつうの image として作る（BC をほどく前の、アプリの形式で覚える）
    VkImageCreateInfo sci;
    int sp = (ci->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) && sparse_on();
    if (sp) {
        im->sp = spimg_of(ci);
        sci = *ci;
        sci.flags &= ~(VkImageCreateFlags) SPARSE_IMG_FLAGS;
        ci = &sci;
    }
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
    } else {
        VkResult r = rpc_call(VR_CREATE_IMAGE, &b, &o);
        if (r == VK_SUCCESS && o.n >= sizeof im->req) {
            memcpy(&im->req, o.p, sizeof im->req);
            qc_put(key, r, o.p, (uint32_t) sizeof im->req);
        }
        free(b.p);
        free(o.p);
        if (r != VK_SUCCESS) { free(im); return r; }
    }
    if (sp) {
        VkResult r = sparse_hide_image(dv, im, probe);
        if (r != VK_SUCCESS) { destroy_image(dv, (VkImage) im, NULL); return r; }
    }
    *out = (VkImage) im;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_image(VkDevice dv, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void) a;
    return create_image_ex(dv, ci, out, 0);
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
    struct img *im = (struct img *) h;
    if (im->hidden) { // sparse（S1）の隠しメモリは、image を消したあとで放す
        free_memory(dv, (VkDeviceMemory) im->hidden, NULL);
        sparse_give(im->hidden_size);
    }
    free(im);
}

static void VKAPI_CALL image_reqs(VkDevice dv, VkImage h, VkMemoryRequirements *r) { (void) dv; *r = ((struct img *) h)->req; }

// sparse の image のタイルの並び（S1）。residency の image だけ 1 つ答える（ほかは 0 個）
static void sparse_reqs_out(const struct spimg *s, uint32_t *n, VkSparseImageMemoryRequirements *r, VkSparseImageMemoryRequirements2 *r2) {
    VkSparseImageMemoryRequirements q;
    VkDeviceSize sz;
    uint32_t k = s && s->on && sparse_img_layout(s, &q, &sz) ? 1 : 0;
    if (!r && !r2) { *n = k; return; }
    if (*n && k) {
        if (r) r[0] = q;
        else r2[0].memoryRequirements = q;
        *n = 1;
    } else *n = 0;
}
static void VKAPI_CALL image_sparse_reqs(VkDevice dv, VkImage h, uint32_t *n, VkSparseImageMemoryRequirements *r) {
    (void) dv;
    sparse_reqs_out(h ? &((struct img *) h)->sp : NULL, n, r, NULL);
}
static void VKAPI_CALL image_sparse_reqs2(VkDevice dv, const VkImageSparseMemoryRequirementsInfo2 *i, uint32_t *n, VkSparseImageMemoryRequirements2 *r) {
    (void) dv;
    sparse_reqs_out(i->image ? &((struct img *) i->image)->sp : NULL, n, NULL, r);
}

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
    ls_note_view(id, ci);
    *out = (VkImageView) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_view(VkDevice dv, VkImageView h, const VkAllocationCallbacks *a) { (void) a; if (h) { send_destroy(VR_DESTROY_IMAGE_VIEW, dv, ID(h)); ls_drop(ID(h)); } }

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
    // vkd3d-proton には 1 要素の揃えで作れると名乗っている。受け手の GPU の揃え（iPhone は 64 バイト）に合わない見え方を数える
    uint64_t al = g_hw.props.limits.minTexelBufferOffsetAlignment;
    if (vkd3d() && al > 1 && ci->offset % al) {
        static volatile LONG n;
        LONG k = InterlockedIncrement(&n);
        if (k <= 3 || (k & (k - 1)) == 0) say("受け手の揃え（%llu バイト）に合わない texel buffer の見え方 %ld 個目（offset %llu・形式 %d）", (unsigned long long) al, k,
                                                (unsigned long long) ci->offset, ci->format);
    }
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
// render pass ごとの、読み込む（LOAD）・書き戻す（STORE）・消す（CLEAR）attachment の数。数えるだけ（IPHONE_VK_STATS で出す）
static struct { uint64_t id; uint8_t nload, nstore, nclear, dload, dstore; } g_rpl[2048];
static void rpl_put(uint64_t id, int nload, int nstore, int nclear, int dload, int dstore) {
    unsigned k = (unsigned) (id * 0x9E3779B97F4A7C15ull >> 40) % 2048;
    g_rpl[k].id = id; g_rpl[k].nload = (uint8_t) nload; g_rpl[k].nstore = (uint8_t) nstore; g_rpl[k].nclear = (uint8_t) nclear;
    g_rpl[k].dload = (uint8_t) dload; g_rpl[k].dstore = (uint8_t) dstore;
}
static void rpl_count(uint64_t id, const VkRect2D *area) {
    unsigned k = (unsigned) (id * 0x9E3779B97F4A7C15ull >> 40) % 2048;
    g_stat.rp_begin++;
    if (g_rpl[k].id != id) return;
    g_stat.rp_load += g_rpl[k].nload;
    g_stat.rp_store += g_rpl[k].nstore;
    g_stat.rp_clear += g_rpl[k].nclear;
    g_stat.rp_dload += g_rpl[k].dload;
    g_stat.rp_dstore += g_rpl[k].dstore;
    g_stat.rp_px += (double) area->extent.width * area->extent.height * (g_rpl[k].nload + g_rpl[k].nstore);
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
        ad.loadOp = (VkAttachmentLoadOp) lso_load((uint32_t) ad.loadOp); ad.stencilLoadOp = (VkAttachmentLoadOp) lso_load((uint32_t) ad.stencilLoadOp);
        ad.storeOp = (VkAttachmentStoreOp) lso_store((uint32_t) ad.storeOp); ad.stencilStoreOp = (VkAttachmentStoreOp) lso_store((uint32_t) ad.stencilStoreOp);
        ad.initialLayout = fix_layout(ad.initialLayout);
        ad.finalLayout = fix_layout(ad.finalLayout);
        vr_put(&b, &ad, sizeof ad);
    }
    {
        int nl = 0, ns = 0, nc = 0, dl = 0, ds = 0;
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            nl += ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            nc += ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
            ns += ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE;
            int dep = ci->pAttachments[i].format >= VK_FORMAT_D16_UNORM && ci->pAttachments[i].format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
            dl += dep && ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            ds += dep && ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE;
        }
        rpl_put(id, nl, ns, nc, dl, ds);
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
    ls_note_rp1(id, ci, &b);
    rpc_send(VR_CREATE_RENDER_PASS, &b);
    free(b.p);
    *out = (VkRenderPass) id;
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
        vr_u32(&b, lso_load((uint32_t) ad->loadOp));
        vr_u32(&b, lso_store((uint32_t) ad->storeOp));
        vr_u32(&b, lso_load((uint32_t) ad->stencilLoadOp));
        vr_u32(&b, lso_store((uint32_t) ad->stencilStoreOp));
        vr_u32(&b, fix_layout(ad->initialLayout));
        vr_u32(&b, fix_layout(ad->finalLayout));
    }
    {
        int nl = 0, ns = 0, nc = 0, dl = 0, ds = 0;
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            nl += ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            nc += ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
            ns += ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE;
            int dep = ci->pAttachments[i].format >= VK_FORMAT_D16_UNORM && ci->pAttachments[i].format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
            dl += dep && ci->pAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            ds += dep && ci->pAttachments[i].storeOp == VK_ATTACHMENT_STORE_OP_STORE;
        }
        rpl_put(id, nl, ns, nc, dl, ds);
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
    ls_note_rp2(id, ci, &b);
    rpc_send(VR_CREATE_RENDER_PASS2, &b);
    free(b.p);
    *out = (VkRenderPass) id;
    return VK_SUCCESS;
}

static void ls_destroy_rp_variants(VkDevice dv, uint64_t id);
static void VKAPI_CALL destroy_rpass(VkDevice dv, VkRenderPass h, const VkAllocationCallbacks *a) { (void) a; if (h) { ls_destroy_rp_variants(dv, ID(h)); send_destroy(VR_DESTROY_RENDER_PASS, dv, ID(h)); ls_drop(ID(h)); } }

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
    ls_note_fb(id, ci);
    *out = (VkFramebuffer) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_fb(VkDevice dv, VkFramebuffer h, const VkAllocationCallbacks *a) { (void) a; if (h) { send_destroy(VR_DESTROY_FRAMEBUFFER, dv, ID(h)); ls_drop(ID(h)); } }

// geometry shader を頂点シェーダーで真似る。受け手の GPU が持たないときだけ働く
#include "gs/gs_emu.c"
#include "gs/gs_icd.inc"

// ---- 描画用のパイプライン ----
static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out);
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a);

// divisor 0 を大きな divisor に置き換えるか。受け手が Apple の GPU（MoltenVK）なら置き換える。IPHONE_VK_DIV0=1 で必ず・0 で置き換えない
static int div0_fix(void) {
    const char *e = getenv("IPHONE_VK_DIV0");
    if (e && *e) return *e == '1';
    return g_hw.props.vendorID == 0x106b;
}

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

static VkResult VKAPI_CALL create_graphics_pipelines_raw(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void) cache; (void) a;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        const VkGraphicsPipelineCreateInfo *c = &ci[i];
        // rasterizerDiscard のとき（動的でなければ）、使われない構造体は壊れたポインタのこともあるので読まない（CTS で見つけた）
        int discard = c->pRasterizationState && c->pRasterizationState->rasterizerDiscardEnable;
        for (uint32_t k = 0; discard && c->pDynamicState && k < c->pDynamicState->dynamicStateCount; k++)
            if (c->pDynamicState->pDynamicStates[k] == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) discard = 0;
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
        const VkPipelineViewportStateCreateInfo *vp = discard ? NULL : c->pViewportState;
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
        const VkPipelineMultisampleStateCreateInfo *ms = discard ? NULL : c->pMultisampleState;
        vr_u32(&b, ms ? 1 : 0);
        if (ms) {
            vr_u32(&b, ms->rasterizationSamples);
            vr_u32(&b, ms->sampleShadingEnable);
            put_f(&b, ms->minSampleShading);
            vr_u32(&b, ms->alphaToCoverageEnable);
            vr_u32(&b, ms->alphaToOneEnable);
        }
        const VkPipelineDepthStencilStateCreateInfo *ds = discard ? NULL : c->pDepthStencilState;
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
        const VkPipelineColorBlendStateCreateInfo *cb = discard ? NULL : c->pColorBlendState;
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
        // 末尾：頂点の step rate（インスタンスごとの頂点データを何個おきに進めるか）。古い受け手は読み飛ばす
        for (const VkBaseInStructure *x = c->pVertexInputState ? c->pVertexInputState->pNext : NULL; x; x = x->pNext) {
            if (x->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) continue;
            const VkPipelineVertexInputDivisorStateCreateInfoEXT *dv2 = (const VkPipelineVertexInputDivisorStateCreateInfoEXT *) x;
            vr_buf t = { 0 };
            vr_u32(&t, dv2->vertexBindingDivisorCount);
            for (uint32_t k = 0; k < dv2->vertexBindingDivisorCount; k++) {
                vr_u32(&t, dv2->pVertexBindingDivisors[k].binding);
                uint32_t dvs = dv2->pVertexBindingDivisors[k].divisor;
                // iPhone（MoltenVK）は divisor 0 を持つと名乗るのに、その描き方が丸ごと消える（DXVK 3 の step rate 0）。
                // divisor 0 の意味は「どのインスタンスも firstInstance の要素」で、大きな divisor でもインスタンスの数が
                // それに届かなければ (instance - firstInstance) / divisor = 0 で同じになる。iPhone の上限 2^30 に置き換える
                if (!dvs && div0_fix()) dvs = 1u << 30;
                vr_u32(&t, dvs);
            }
            vr_u32(&b, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);
            vr_bytes(&b, t.p, (uint32_t) t.n);
            free(t.p);
        }
        VkResult r = rpc_call(VR_CREATE_GRAPHICS_PIPELINE, &b, NULL);
        free(b.p);
        for (uint32_t k = 0; k < 8; k++) if (tmps[k]) destroy_shader(dv, tmps[k], NULL); // 一時のシェーダーは作り終えたら消す
        if (r != VK_SUCCESS) { say("graphics pipeline を作れない (%d)", r); out[i] = VK_NULL_HANDLE; worst = r; continue; }
        out[i] = (VkPipeline) id;
    }
    return worst;
}

// ---- 受け手が頂点に使えない形式を真似る ----
// iPhone の GPU（MoltenVK）は、DirectX 11 で頂点に必須の R10G10B10A2_UINT（VK_FORMAT_A2B10G10R10_UINT_PACK32）などを
// 頂点の buffer に使えない。そのまま pipeline を作ると断られ、その描画が丸ごと消える。
// 32 ビットに詰めた 4 成分の形式なら、属性を R32_UINT で読ませて、頂点シェーダーの頭でほどけば同じ値になる。
// SPIR-V の書き換え：その Location の入力の変数を Private にし、同じ Location に uint の入力を足す。
// 入口の関数の最初の block の頭で uint を読んでほどき、Private の変数に書く。ほかの読み書きはそのまま（access chain の型だけ Private に直す）。
// 受け手が形式を持てば何もしない。IPHONE_VK_VFMT=0 で止める

// ほどける形式：32 ビットに詰めた 4 成分（8・8・8・8 と 10・10・10・2 の UNORM・SNORM・USCALED・SSCALED・UINT・SINT）
static int vf_fmt(uint32_t f, fmti *o) {
    static const uint8_t k[6] = { K_UNORM, K_SNORM, K_USCALED, K_SSCALED, K_UINT, K_SINT };
    memset(o, 0, sizeof *o);
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_R8G8B8A8_SINT) *o = (fmti) { 4, 1, k[f - VK_FORMAT_R8G8B8A8_UNORM], 0, 0 };
    else if (f >= VK_FORMAT_B8G8R8A8_UNORM && f <= VK_FORMAT_B8G8R8A8_SINT) *o = (fmti) { 4, 1, k[f - VK_FORMAT_B8G8R8A8_UNORM], 1, 0 };
    else if (f >= VK_FORMAT_A8B8G8R8_UNORM_PACK32 && f <= VK_FORMAT_A8B8G8R8_SINT_PACK32) *o = (fmti) { 4, 1, k[f - VK_FORMAT_A8B8G8R8_UNORM_PACK32], 0, 0 };
    else if (f >= VK_FORMAT_A2R10G10B10_UNORM_PACK32 && f <= VK_FORMAT_A2R10G10B10_SINT_PACK32) *o = (fmti) { 4, 4, k[f - VK_FORMAT_A2R10G10B10_UNORM_PACK32], 1, 1 };
    else if (f >= VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) *o = (fmti) { 4, 4, k[f - VK_FORMAT_A2B10G10R10_UNORM_PACK32], 0, 1 };
    else return 0;
    return 1;
}

// 受け手がその形式を頂点に使えず、R32_UINT なら使えるか。受け手の答えを覚えておく
static int8_t g_vf_need[VK_FORMAT_A2B10G10R10_SINT_PACK32 + 1]; // 0 まだ聞いていない・1 真似る・2 要らない
static int vf_need(uint32_t f) {
    fmti fi;
    if (f > VK_FORMAT_A2B10G10R10_SINT_PACK32 || !vf_fmt(f, &fi)) return 0;
    if (!g_vf_need[f]) {
        VkFormatProperties p, q;
        get_format_ex((VkFormat) f, &p, NULL);
        get_format_ex(VK_FORMAT_R32_UINT, &q, NULL);
        g_vf_need[f] = !(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) && (q.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    return g_vf_need[f] == 1;
}

// 真似るか：受け手が頂点に使えない形式が 1 つでもあれば
static int vf_on(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *e = getenv("IPHONE_VK_VFMT");
    char list[256] = "";
    int on = 0;
    for (uint32_t f = VK_FORMAT_R8G8B8A8_UNORM; !(e && *e == '0') && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32; f++)
        if (vf_need(f)) {
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, " %u", f);
            on = 1;
        }
    if (on) say("受け手が頂点に使えない形式（%s）は、R32_UINT で読ませて頂点シェーダーでほどく", list + 1);
    v = on;
    return v;
}

// アプリには、真似る形式も頂点に使えると見せる
static void vf_fix_format(VkFormat f, VkFormatProperties *p, VkFormatProperties3 *p3) {
    if (!vf_on() || !vf_need(f)) return;
    p->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (p3) p3->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
}

// VS の SPIR-V を覚えておく。受け手が GS を持たなければ GS の真似が覚えるので、そのときは覚えない
struct vf_mod { uint64_t id; uint32_t *code; size_t words; struct vf_mod *next; };
static struct vf_mod *g_vfmods;
static SRWLOCK g_vf_lock = SRWLOCK_INIT;

static void vf_keep(uint64_t id, const uint32_t *code, size_t bytes) {
    if (bytes < 20 || gs_on() || spv_model(code, bytes / 4) != 0 || !vf_on()) return;
    struct vf_mod *e = calloc(1, sizeof *e);
    if (!e || !(e->code = malloc(bytes))) { free(e); return; }
    e->id = id;
    e->words = bytes / 4;
    memcpy(e->code, code, bytes);
    AcquireSRWLockExclusive(&g_vf_lock);
    e->next = g_vfmods;
    g_vfmods = e;
    ReleaseSRWLockExclusive(&g_vf_lock);
}

static void vf_forget(uint64_t id) {
    if (!g_vfmods) return;
    AcquireSRWLockExclusive(&g_vf_lock);
    for (struct vf_mod **p = &g_vfmods; *p; p = &(*p)->next)
        if ((*p)->id == id) { struct vf_mod *e = *p; *p = e->next; free(e->code); free(e); break; }
    ReleaseSRWLockExclusive(&g_vf_lock);
}

// 段の SPIR-V の写し（free で返す）。pNext の物、GS の真似が覚えた物、ここで覚えた物の順に探す
static uint32_t *vf_code(const VkPipelineShaderStageCreateInfo *st, size_t *words) {
    size_t n = 0;
    const uint32_t *p = gs_code(st, &n);
    uint32_t *r = NULL;
    if (p && n) {
        if ((r = malloc(n * 4))) { memcpy(r, p, n * 4); *words = n; }
        return r;
    }
    AcquireSRWLockShared(&g_vf_lock);
    for (struct vf_mod *e = g_vfmods; e; e = e->next)
        if (e->id == ID(st->module)) {
            if ((r = malloc(e->words * 4))) { memcpy(r, e->code, e->words * 4); *words = e->words; }
            break;
        }
    ReleaseSRWLockShared(&g_vf_lock);
    return r;
}

// 32 ビットの x を形式 f の 4 成分にほどく。*vt に vec4 の型。符号つきは BitFieldSExtract で上の位を符号で埋める
static uint32_t vf_unpack(ctx *c, const fmti *f, uint32_t x, uint32_t *vt) {
    int sgn = f->kind == K_SNORM || f->kind == K_SINT || f->kind == K_SSCALED;
    uint32_t ct = f->kind == K_UINT ? c->t_u32 : f->kind == K_SINT ? c->t_i32 : c->t_f32, comp[4];
    uint32_t xs = sgn ? op1(c, OP_Bitcast, c->t_i32, x) : 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t bits = f->pack ? (i == 3 ? 2 : 10) : 8, shift = ku(c, f->pack ? i * 10 : i * 8), raw = 0, sraw = 0, val;
        if (sgn) sraw = op3(c, OP_BitFieldSExtract, c->t_i32, xs, shift, ku(c, bits));
        else raw = op3(c, OP_BitFieldUExtract, c->t_u32, x, shift, ku(c, bits));
        switch (f->kind) {
        case K_UNORM: val = op2(c, OP_FDiv, c->t_f32, op1(c, OP_ConvertUToF, c->t_f32, raw), kf(c, (float) ((1u << bits) - 1))); break;
        case K_SNORM:
            val = op2(c, OP_FDiv, c->t_f32, op1(c, OP_ConvertSToF, c->t_f32, sraw), kf(c, (float) ((1u << (bits - 1)) - 1)));
            val = ext2(c, c->t_f32, GLSL_FMax, val, kf(c, -1.0f));
            break;
        case K_USCALED: val = op1(c, OP_ConvertUToF, c->t_f32, raw); break;
        case K_SSCALED: val = op1(c, OP_ConvertSToF, c->t_f32, sraw); break;
        case K_UINT: val = raw; break;
        default: val = sraw; break;
        }
        comp[i] = val;
    }
    if (f->bgra) { uint32_t t = comp[0]; comp[0] = comp[2]; comp[2] = t; }
    *vt = ty_vec(c, ct, 4);
    uint32_t r = spv_new_id(c);
    OPW(&c->fn, OP_CompositeConstruct, *vt, r, comp[0], comp[1], comp[2], comp[3]);
    return r;
}

// 入力の変数が使う Location の数
static uint32_t vf_locs(ctx *c, uint32_t t) {
    uint32_t *d = D(c, t);
    if (d && OPC(d) == OP_TypeArray) return const_value(c, d[3]) * vf_locs(c, d[2]);
    if (d && OPC(d) == OP_TypeMatrix) return d[3];
    return 1;
}

// 入口の関数の頭：足した uint の入力を読んでほどき、Private にした変数に書く
static void vf_prologue(ctx *c, const iovar *in, int nin, const uint32_t *newin, const uint32_t *locfmt) {
    for (int i = 0; i < nin && !c->bad; i++) {
        const iovar *v = &in[i];
        if (!c->conv[v->var]) continue;
        fmti f;
        vf_fmt(locfmt[v->loc], &f);
        uint32_t v4t = 0, v4 = vf_unpack(c, &f, load(c, c->t_u32, newin[v->loc]), &v4t);
        uint32_t n, st = scalar_of(c, v->type, &n), n4, s4 = scalar_of(c, v4t, &n4), x;
        (void) n4;
        if (n == 1) x = op2(c, OP_CompositeExtract, s4, v4, v->comp);
        else {
            uint32_t w[10] = { ty_vec(c, s4, n), spv_new_id(c), v4, v4 };
            for (uint32_t k = 0; k < n; k++) w[4 + k] = v->comp + k;
            wb_op(&c->fn, OP_VectorShuffle, 4 + n, w);
            x = w[1];
        }
        if (st != s4) x = op1(c, OP_Bitcast, v->type, x);
        OPW(&c->fn, OP_Store, v->var, x);
    }
}

// VS の入口 entry で、Location L の入力を形式 locfmt[L] からほどく SPIR-V を作る（0 ならそのまま）。
// 作れたら 1。書き換える入力が無ければ *out は NULL のまま。ほかの入口は落とす
static int vf_build(const uint32_t *vs, size_t words, const char *entry, const uint32_t *locfmt, uint32_t nloc,
                    uint32_t **out, size_t *out_words, gs_emu_info *info) {
    ctx cc = { 0 }, *c = &cc;
    iovar in[MAX_IO];
    uint32_t newin[64] = { 0 }, *ep = NULL, if_at = 0;
    int nin = 0, nconv = 0, need_glsl = 0;
    memset(info, 0, sizeof *info);
    c->info = info;
    *out = NULL;
    *out_words = 0;
    if (!split(&c->g, vs, words)) { snprintf(info->why, sizeof info->why, "VS の SPIR-V が読めない"); return 0; }
    c->ver = c->g.ver;
    c->next = c->g.bound;
    c->cap = c->next + 65536;
    c->def = calloc(c->cap, sizeof *c->def);
    c->conv = calloc(c->cap, 1);
    c->plainmap = calloc(c->cap, 4);
    c->sel = calloc(c->cap, 4);
    c->root = calloc(c->cap, 4);
    if (!c->def || !c->conv || !c->plainmap || !c->sel || !c->root) { fail(c, "メモリが足りない"); goto done; }
    for (int s = 0; s < S_N; s++)
        for (size_t i = 0; i < c->g.s[s].n; i++) {
            set_def(c, c->g.s[s].v[i]);
            if (s == S_GLOB && is_type_op(OPC(c->g.s[s].v[i]))) il_push(&c->types, c->g.s[s].v[i]);
        }
    // 入口：Vertex で名前が entry の物
    for (size_t i = 0; i < c->g.s[S_EP].n; i++) {
        uint32_t *e = c->g.s[S_EP].v[i], k = 3;
        while (k < WC(e)) {
            uint32_t x = e[k++];
            if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break;
        }
        if (e[1] == 0 && strlen(entry) < (k - 3) * 4 && !strcmp((const char *) (e + 3), entry)) { ep = e; if_at = k; }
    }
    if (!ep) { fail(c, "頂点の入口 %s が無い", entry); goto done; }
    // 書き換える入力の変数（スカラーかベクトルで、32 ビットの数）
    collect_io(c, &c->g, SC_Input, in, &nin, MAX_IO);
    for (int i = 0; i < nin && !c->bad; i++) {
        iovar *v = &in[i];
        if (v->builtin != ~0u || v->loc == ~0u) continue;
        uint32_t nl = vf_locs(c, v->type), hit = 0, n = 0, st = scalar_of(c, v->type, &n), *sd = D(c, st);
        for (uint32_t L = v->loc; L < v->loc + nl && L < nloc; L++) if (locfmt[L]) hit = 1;
        if (!hit) continue;
        if (nl != 1 || !sd || (OPC(sd) != OP_TypeInt && OPC(sd) != OP_TypeFloat) || sd[2] != 32 || v->comp + n > 4) {
            fail(c, "Location %u の入力の型が読めない", v->loc);
            break;
        }
        fmti f;
        vf_fmt(locfmt[v->loc], &f);
        need_glsl |= f.kind == K_SNORM;
        c->conv[v->var] = 1;
        nconv++;
    }
    if (c->bad || !nconv) goto done;
    compute_roots(c);
    if (c->bad) goto done;
    { uint32_t w[2] = { 32, 0 }; c->t_u32 = get_type(c, OP_TypeInt, 2, w); }
    { uint32_t w[2] = { 32, 1 }; c->t_i32 = get_type(c, OP_TypeInt, 2, w); }
    { uint32_t w[1] = { 32 }; c->t_f32 = get_type(c, OP_TypeFloat, 1, w); }
    if (need_glsl) {
        static const char nm[] = "GLSL.std.450";
        uint32_t w[8] = { 0 };
        memcpy(w + 1, nm, sizeof nm);
        for (size_t i = 0; i < c->g.s[S_IMP].n && !c->glsl; i++)
            if (str_eq(c->g.s[S_IMP].v[i] + 2, w + 1, WC(c->g.s[S_IMP].v[i]) - 2, 4)) c->glsl = c->g.s[S_IMP].v[i][1];
        if (!c->glsl) {
            w[0] = c->glsl = spv_new_id(c);
            il_push(&c->newimp, make_ins(OP_ExtInstImport, 5, w));
        }
    }
    // 変える変数を Private にして、宣言を後ろへ移す（新しいポインターの型より後に置くため）
    for (size_t i = 0; i < c->g.s[S_GLOB].n; i++) {
        uint32_t *d = c->g.s[S_GLOB].v[i];
        if (OPC(d) != OP_Variable || !c->conv[d[2]]) continue;
        uint32_t w[3] = { ty_ptr(c, SC_Private, plain(c, ptr_pointee(c, d[1]))), d[2], SC_Private };
        uint32_t *x = make_ins(OP_Variable, 3, w);
        il_push(&c->convvars, x);
        c->def[d[2]] = x;
    }
    // 足す入力：Location ごとに uint を 1 つ
    {
        uint32_t pin = ty_ptr(c, SC_Input, c->t_u32);
        for (int i = 0; i < nin; i++) {
            uint32_t L = in[i].loc;
            if (!c->conv[in[i].var] || newin[L]) continue;
            char nm[32];
            snprintf(nm, sizeof nm, "vf_packed%u", L);
            newin[L] = new_var(c, pin, SC_Input, nm);
            add_deco(c, newin[L], DEC_Location, L);
        }
    }
    // 関数：入口の関数の最初の block の頭（Function の変数の後ろ）でほどく
    {
        int in_main = 0, need_pro = 0, pro_done = 0;
        for (size_t i = 0; i < c->g.s[S_FUNC].n && !c->bad; i++) {
            uint32_t *ins = c->g.s[S_FUNC].v[i], op = OPC(ins);
            if (op == OP_Function) in_main = ins[2] == ep[2];
            if (need_pro && op != OP_Variable && op != OP_Line && op != OP_NoLine) {
                vf_prologue(c, in, nin, newin, locfmt);
                need_pro = 0;
            }
            if (op == OP_Label && in_main && !pro_done) { wb_ins(&c->fn, ins); need_pro = pro_done = 1; continue; }
            if (chain_op(op) && ins[3] < c->cap && c->root[ins[3]] && c->conv[c->root[ins[3]]]) {
                uint32_t *x = dup_ins(ins);
                x[1] = ty_ptr(c, SC_Private, plain(c, ptr_pointee(c, ins[1])));
                wb_ins(&c->fn, x);
                free(x);
                continue;
            }
            wb_ins(&c->fn, ins);
        }
        if (!pro_done) fail(c, "入口の関数が見つからない");
    }
    if (c->bad) goto done;
    // 組み立て
    {
        wbuf o = { 0 }, e = { 0 };
        uint32_t hdr[5] = { 0x07230203u, c->ver, c->g.gen, 0, 0 };
        for (int i = 0; i < 5; i++) wb_put(&o, hdr[i]);
        int secs[4] = { S_CAP, S_EXT, S_IMP, S_MM };
        for (int s = 0; s < 4; s++) {
            for (size_t i = 0; i < c->g.s[secs[s]].n; i++) wb_ins(&o, c->g.s[secs[s]].v[i]);
            if (secs[s] == S_IMP) for (size_t i = 0; i < c->newimp.n; i++) wb_ins(&o, c->newimp.v[i]);
        }
        // 入口は 1 つ。1.4 より前の interface は Input・Output だけなので、Private にした変数を外す
        for (uint32_t k = 1; k < if_at; k++) wb_put(&e, ep[k]);
        for (uint32_t k = if_at; k < WC(ep); k++)
            if (!(ep[k] < c->cap && c->conv[ep[k]] && c->ver < 0x10400)) wb_put(&e, ep[k]);
        for (uint32_t L = 0; L < 64; L++) if (newin[L]) wb_put(&e, newin[L]);
        wb_op(&o, OP_EntryPoint, (uint32_t) e.n, e.p);
        free(e.p);
        for (size_t i = 0; i < c->g.s[S_EM].n; i++) if (c->g.s[S_EM].v[i][1] == ep[2]) wb_ins(&o, c->g.s[S_EM].v[i]);
        for (size_t i = 0; i < c->g.s[S_DBG].n; i++) wb_ins(&o, c->g.s[S_DBG].v[i]);
        for (size_t i = 0; i < c->newdbg.n; i++) wb_ins(&o, c->newdbg.v[i]);
        // 飾り：Private にした変数の物（Location など）は落とす
        for (size_t i = 0; i < c->g.s[S_ANN].n && !c->bad; i++) {
            uint32_t *a = c->g.s[S_ANN].v[i], op = OPC(a);
            if (op == OP_DecorationGroup || op == OP_GroupDecorate || op == OP_GroupMemberDecorate) { fail(c, "decoration group がある"); break; }
            if ((op == OP_Decorate || op == OP_DecorateId || op == OP_DecorateString) && a[1] < c->cap && c->conv[a[1]]) continue;
            wb_ins(&o, a);
        }
        for (size_t i = 0; i < c->newann.n; i++) wb_ins(&o, c->newann.v[i]);
        for (size_t i = 0; i < c->g.s[S_GLOB].n; i++) {
            uint32_t *d = c->g.s[S_GLOB].v[i];
            if (OPC(d) == OP_Variable && c->conv[d[2]]) continue;
            wb_ins(&o, d);
        }
        for (size_t i = 0; i < c->newglob.n; i++) wb_ins(&o, c->newglob.v[i]);
        for (size_t i = 0; i < c->convvars.n; i++) wb_ins(&o, c->convvars.v[i]);
        for (size_t i = 0; i < c->newvars.n; i++) wb_ins(&o, c->newvars.v[i]);
        for (size_t i = 0; i < c->fn.n; i++) wb_put(&o, c->fn.p[i]);
        o.p[3] = c->next;
        if (!c->bad) { *out = o.p; *out_words = o.n; } else free(o.p);
    }
done:;
    int ok = !c->bad;
    smod_free(&c->g);
    ilist *ls[6] = { &c->newglob, &c->convvars, &c->newvars, &c->newann, &c->newdbg, &c->newimp };
    for (int m = 0; m < 6; m++) {
        for (size_t i = 0; i < ls[m]->n; i++) free(ls[m]->v[i]);
        free(ls[m]->v);
    }
    free(c->types.v);
    free(c->fn.p);
    free(c->def);
    free(c->conv);
    free(c->plainmap);
    free(c->sel);
    free(c->root);
    return ok;
}

// 真似るときの pipeline の作り方（vf_pipeline が書く）
struct vf_pipe {
    VkGraphicsPipelineCreateInfo ci;
    VkPipelineShaderStageCreateInfo st[8];
    VkPipelineVertexInputStateCreateInfo vi;
    VkVertexInputAttributeDescription at[32];
    VkShaderModule mod; // 作った VS。pipeline を作り終えたら消す
};

// 受け手が頂点に使えない形式の属性があれば、R32_UINT にして、ほどく VS に替えた作り方を返す。要らなければ c のまま
static const VkGraphicsPipelineCreateInfo *vf_pipeline(VkDevice dv, const VkGraphicsPipelineCreateInfo *c, struct vf_pipe *t) {
    t->mod = VK_NULL_HANDLE;
    const VkPipelineVertexInputStateCreateInfo *vi = c->pVertexInputState;
    if (!vi || !vi->vertexAttributeDescriptionCount || vi->vertexAttributeDescriptionCount > 32 || c->stageCount > 8) return c;
    uint32_t locfmt[64] = { 0 }, nv = 0, vs = ~0u, loc0 = 0;
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++) {
        const VkVertexInputAttributeDescription *d = &vi->pVertexAttributeDescriptions[i];
        if (d->location < 64 && vf_need(d->format)) { locfmt[d->location] = d->format; if (!nv++) loc0 = d->location; }
    }
    if (!nv) return c;
    for (uint32_t k = 0; k < c->stageCount; k++) if (c->pStages[k].stage == VK_SHADER_STAGE_VERTEX_BIT) vs = k;
    if (vs == ~0u) return c;
    size_t words = 0, ow = 0;
    uint32_t *code = vf_code(&c->pStages[vs], &words), *o = NULL;
    gs_emu_info info;
    if (!code) { say("頂点の形式を真似る：VS の SPIR-V が見つからない"); return c; }
    int ok = vf_build(code, words, c->pStages[vs].pName, locfmt, 64, &o, &ow, &info);
    free(code);
    if (!ok) { say("頂点の形式を真似られない：%s", info.why); return c; }
    if (o) {
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = ow * 4, .pCode = o };
        VkResult r = create_shader(dv, &smi, NULL, &t->mod);
        free(o);
        if (r != VK_SUCCESS) { t->mod = VK_NULL_HANDLE; return c; }
    }
    t->ci = *c;
    memcpy(t->st, c->pStages, c->stageCount * sizeof *t->st);
    if (t->mod) { t->st[vs].module = t->mod; t->st[vs].pNext = NULL; }
    t->ci.pStages = t->st;
    t->vi = *vi;
    memcpy(t->at, vi->pVertexAttributeDescriptions, vi->vertexAttributeDescriptionCount * sizeof *t->at);
    for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++)
        if (t->at[i].location < 64 && locfmt[t->at[i].location]) t->at[i].format = VK_FORMAT_R32_UINT;
    t->vi.pVertexAttributeDescriptions = t->at;
    t->ci.pVertexInputState = &t->vi;
    say("頂点の形式を真似た：属性 %u 個（Location %u の形式 %u など）を R32_UINT で読ませる%s", nv, loc0, locfmt[loc0],
        t->mod ? "・VS でほどく" : "・VS は読まない");
    return &t->ci;
}

static void wr_pipes(uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *out);
static void ls_note_pipes(uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *out);

static VkResult VKAPI_CALL create_graphics_pipelines(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!vf_on()) {
        VkResult r = create_graphics_pipelines_raw(dv, cache, n, ci, a, out);
        wr_pipes(n, ci, out);
        ls_note_pipes(n, ci, out);
        return r;
    }
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        struct vf_pipe t;
        VkResult r = create_graphics_pipelines_raw(dv, cache, 1, vf_pipeline(dv, &ci[i], &t), a, &out[i]);
        if (t.mod) destroy_shader(dv, t.mod, NULL);
        if (r != VK_SUCCESS) worst = r;
    }
    wr_pipes(n, ci, out);
    ls_note_pipes(n, ci, out);
    return worst;
}

// ---- サンプラー ----
// 深さを比べる sampler の滑らかな読み方を、1 点で読む形に直すか。iPhone の GPU は D32_SFLOAT を滑らかに読めず
// （形式の性質でも名乗っている）、確かめずに滑らかに読むアプリは 0 を受け取って影が真っ黒になる（0 A.D. の PCF）。
// 受け手が Apple の GPU なら直す。IPHONE_VK_CMP_NEAREST=1 で必ず・0 で直さない
static int cmp_nearest(void) {
    const char *e = getenv("IPHONE_VK_CMP_NEAREST");
    if (e && *e) return *e == '1';
    return g_hw.props.vendorID == 0x106b;
}

static VkResult VKAPI_CALL create_sampler(VkDevice dv, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    (void) a;
    uint64_t id = new_id();
    VkSamplerCreateInfo fixed;
    if (ci->compareEnable && (ci->magFilter == VK_FILTER_LINEAR || ci->minFilter == VK_FILTER_LINEAR) && cmp_nearest()) {
        fixed = *ci;
        fixed.magFilter = fixed.minFilter = VK_FILTER_NEAREST;
        ci = &fixed;
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_put(&b, (const uint8_t *) ci + offsetof(VkSamplerCreateInfo, flags), sizeof *ci - offsetof(VkSamplerCreateInfo, flags));
    rpc_send(VR_CREATE_SAMPLER, &b);
    free(b.p);
    *out = (VkSampler) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_sampler(VkDevice dv, VkSampler h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_SAMPLER, dv, ID(h)); }

// Linux 版には窓（surface・swapchain）が無い。VK_KHR_surface も名乗らないので、窓を出すアプリは別の装置を選ぶ
#ifdef _WIN32
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

#endif

#ifdef _WIN32
// ---- 窓の絵を H.264 でほどく（Media Foundation の Microsoft H.264 デコーダ・遅れの少ない設定） ----
// 受け手が 1 コマずつ縮めた物（VR_READ_H264）を、表示の糸で順にほどく。絵は NV12 で出てくるので、BGRA に直して描く。
// GPU でほどけるとき（D3D11 の DXVA）は GPU で。だめならソフトで
struct vdec {
    IMFTransform *t;
    IMFDXGIDeviceManager *mgr; // GPU でほどくとき（D3D11 の DXVA）
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *stage;    // 読み出し用（最後にほどけた絵を写しておく）
    uint32_t sw, sh;
    int hw, staged, mapped;
    int provides;           // 出力の sample をデコーダが用意する
    DWORD outsz;
    uint32_t fw, fh, pitch; // 出てくる NV12 の大きさ（16 の倍数に揃っていることがある）・1 行のバイト
    uint8_t *nv12;          // 最後にほどけた絵
    size_t cap;
    uint32_t npitch, nrows; // nv12 の 1 行のバイトと Y の面の行数
    int has;
    uint64_t tag;           // 最後にほどけた絵の札（何回目の present の物か）
};

static void vdec_unmap(struct vdec *d);

static void vdec_close(struct vdec *d) {
    if (d->t) {
        IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->t);
    }
    if (d->mgr) IMFDXGIDeviceManager_Release(d->mgr);
    vdec_unmap(d);
    if (d->stage) ID3D11Texture2D_Release(d->stage);
    if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
    if (d->dev) ID3D11Device_Release(d->dev);
    free(d->nv12);
    memset(d, 0, sizeof *d);
}

// 出す形を NV12 に決める。大きさと 1 行のバイトを覚える
static int vdec_out_type(struct vdec *d) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->t, 0, i, &t)) || !t) return 0;
        GUID sub;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &VG_SUBTYPE, &sub)) && !memcmp(&sub, &VG_NV12, sizeof sub)) {
            HRESULT hr = IMFTransform_SetOutputType(d->t, 0, t, 0);
            UINT64 fs = 0;
            UINT32 st = 0;
            IMFMediaType_GetUINT64(t, &VG_FRAME_SIZE, &fs);
            d->fw = (uint32_t) (fs >> 32), d->fh = (uint32_t) fs;
            d->pitch = SUCCEEDED(IMFMediaType_GetUINT32(t, &VG_STRIDE, &st)) && (int32_t) st > 0 ? st : d->fw;
            IMFMediaType_Release(t);
            if (FAILED(hr)) return 0;
            MFT_OUTPUT_STREAM_INFO si;
            memset(&si, 0, sizeof si);
            IMFTransform_GetOutputStreamInfo(d->t, 0, &si);
            d->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            d->outsz = si.cbSize > d->pitch * d->fh * 3 / 2 ? si.cbSize : d->pitch * d->fh * 3 / 2;
            return 1;
        }
        IMFMediaType_Release(t);
    }
}

// Windows の d3d11.dll を読んでよいか。DXVK のアプリは同じ名前の d3d11.dll・dxgi.dll を横に置いて先に読んでいる。
// そこへ Windows の d3d11.dll を読むと、その中の dxgi.dll がアプリの物（DXVK）につながって落ちる（10/7 の d3d11cube）。
// 読み込み済みの物が Windows の物で、アプリの横にも置いていないときだけ使う
static int vdec_hw_safe(void) {
    WCHAR sys[MAX_PATH], p[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    static const WCHAR *const names[] = { L"dxgi.dll", L"d3d11.dll" };
    for (int i = 0; i < 2; i++) {
        HMODULE m = GetModuleHandleW(names[i]);
        DWORD k = m ? GetModuleFileNameW(m, p, MAX_PATH) : 0;
        if (m && (!k || k >= MAX_PATH || _wcsnicmp(p, sys, n) != 0 || p[n] != L'\\')) return 0;
        k = GetModuleFileNameW(NULL, p, MAX_PATH);
        WCHAR *slash = k && k < MAX_PATH ? wcsrchr(p, L'\\') : NULL;
        if (!slash) return 0;
        wcscpy(slash + 1, names[i]);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return 0;
    }
    return 1;
}

// GPU でほどけるようにする（D3D11 の装置を作り、デコーダに渡す）。だめなら 0 で、ソフトでほどく。
// d3d11.dll は Windows の物をパスで読む
static int vdec_hw(struct vdec *d) {
    static const GUID aware_g = { 0x206b4fc8, 0xfcf9, 0x4c51, { 0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0 } };       // MF_SA_D3D11_AWARE
    static const GUID mt_g = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };          // ID3D10Multithread
    static PFN_D3D11_CREATE_DEVICE create;
    static HRESULT (WINAPI *mk_mgr)(UINT *, IMFDXGIDeviceManager **);
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const char *e = getenv("IPHONE_VK_VIDEO_HWDEC");
    if (e && *e == '0') return 0;
    // 確かめてから読む（読むだけでも、中の dxgi.dll がアプリの物につながる）
    if (!vdec_hw_safe()) {
        say("アプリが自分の d3d11.dll か dxgi.dll（DXVK など）を持っているので、H.264 は GPU でなくソフトでほどく");
        return 0;
    }
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        WCHAR path[MAX_PATH + 16];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        HMODULE m = NULL, mf = GetModuleHandleW(L"mfplat.dll");
        if (n && n < MAX_PATH) { wcscpy(path + n, L"\\d3d11.dll"); m = LoadLibraryW(path); }
        if (m) create = (PFN_D3D11_CREATE_DEVICE) (void *) GetProcAddress(m, "D3D11CreateDevice");
        if (mf) mk_mgr = (HRESULT (WINAPI *)(UINT *, IMFDXGIDeviceManager **)) (void *) GetProcAddress(mf, "MFCreateDXGIDeviceManager");
        InitOnceComplete(&once, 0, NULL);
    }
    if (!create || !mk_mgr) return 0;
    IMFAttributes *at = NULL;
    UINT32 aware = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(d->t, &at)) && at) {
        IMFAttributes_GetUINT32(at, &aware_g, &aware);
        IMFAttributes_Release(at);
    }
    if (!aware || FAILED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &d->dev, NULL, NULL))) {
        d->dev = NULL;
        return 0;
    }
    ID3D11Device_GetImmediateContext(d->dev, &d->ctx);
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(d->dev, &mt_g, (void **) &mt)) && mt) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    UINT tok = 0;
    if (FAILED(mk_mgr(&tok, &d->mgr)) || FAILED(IMFDXGIDeviceManager_ResetDevice(d->mgr, (IUnknown *) d->dev, tok)) ||
        FAILED(IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR) d->mgr))) {
        if (d->mgr) IMFDXGIDeviceManager_Release(d->mgr);
        if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
        ID3D11Device_Release(d->dev);
        d->mgr = NULL, d->dev = NULL, d->ctx = NULL;
        return 0;
    }
    return 1;
}

// hw なら GPU でほどけるかも試す（IPHONE_VK_VIDEO_HWDEC=0 でソフトだけ）
static int vdec_open(struct vdec *d, int hw) {
    memset(d, 0, sizeof *d);
    if (!vid_mf()) return 0;
    if (FAILED(CoCreateInstance(&VG_CLSID_H264_DEC, NULL, CLSCTX_INPROC_SERVER, &VG_IID_TRANSFORM, (void **) &d->t))) { d->t = NULL; return 0; }
    // 遅れの少ない設定：ほどけたらすぐ出す（並べ替えのために待たない）
    IMFAttributes *at = NULL;
    if (SUCCEEDED(IMFTransform_GetAttributes(d->t, &at)) && at) {
        IMFAttributes_SetUINT32(at, &VG_LOW_LATENCY, TRUE);
        IMFAttributes_Release(at);
    }
    ICodecAPI *api = NULL;
    if (SUCCEEDED(IMFTransform_QueryInterface(d->t, &VG_IID_CODECAPI, (void **) &api)) && api) {
        vid_api_bool(api, &VG_LOW_LATENCY, 1);
        ICodecAPI_Release(api);
    }
    d->hw = hw && vdec_hw(d);
    IMFMediaType *in = vid_type(&VG_H264, 0, 0, 0, 0, 0);
    HRESULT hr = in ? IMFTransform_SetInputType(d->t, 0, in, 0) : E_FAIL;
    if (in) IMFMediaType_Release(in);
    if (FAILED(hr)) { vdec_close(d); return 0; }
    vdec_out_type(d); // 絵の大きさが分かるのは最初のコマから（そのとき形が変わったと言ってくる）
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

// ソフトでほどいた絵（CPU のメモリ）を nv12 に写す
static int vdec_take_cpu(struct vdec *d, IMFMediaBuffer *mb) {
    IMF2DBuffer *b2 = NULL;
    BYTE *p = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    int locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &VG_IID_2DBUFFER, (void **) &b2)) && b2 && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch));
    if (!locked2d && SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &len))) pitch = (LONG) d->pitch;
    int ok = 0;
    if (p && pitch > 0) {
        size_t need = (size_t) pitch * d->fh * 3 / 2;
        if (!locked2d && len < need) need = 0;
        if (need && d->cap < need) {
            free(d->nv12);
            d->nv12 = malloc(need);
            d->cap = d->nv12 ? need : 0;
        }
        if (need && d->nv12) {
            memcpy(d->nv12, p, need);
            d->npitch = (uint32_t) pitch, d->nrows = d->fh, d->has = 1, d->staged = 0, ok = 1;
        }
    }
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    else if (p) IMFMediaBuffer_Unlock(mb);
    if (b2) IMF2DBuffer_Release(b2);
    return ok;
}

// GPU でほどいた絵（D3D11 の画像）を、読み出し用の画像に写す（GPU の中で・待たない）。CPU に読み出すのは描くコマだけ
static int vdec_take_gpu(struct vdec *d, IMFMediaBuffer *mb) {
    static const GUID tex_g = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } }; // ID3D11Texture2D
    IMFDXGIBuffer *db = NULL;
    ID3D11Texture2D *tx = NULL;
    UINT sub = 0;
    int ok = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &VG_IID_DXGIBUFFER, (void **) &db)) && db &&
        SUCCEEDED(IMFDXGIBuffer_GetResource(db, &tex_g, (void **) &tx)) && tx && SUCCEEDED(IMFDXGIBuffer_GetSubresourceIndex(db, &sub))) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(tx, &td);
        if (td.Format == DXGI_FORMAT_NV12 && (!d->stage || d->sw != td.Width || d->sh != td.Height)) {
            if (d->stage) ID3D11Texture2D_Release(d->stage);
            D3D11_TEXTURE2D_DESC sd;
            memset(&sd, 0, sizeof sd);
            sd.Width = td.Width, sd.Height = td.Height, sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = td.Format;
            sd.SampleDesc.Count = 1, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &sd, NULL, &d->stage))) d->stage = NULL;
            d->sw = td.Width, d->sh = td.Height;
        }
        if (td.Format == DXGI_FORMAT_NV12 && d->stage) {
            ID3D11DeviceContext_CopySubresourceRegion(d->ctx, (ID3D11Resource *) d->stage, 0, 0, 0, 0, (ID3D11Resource *) tx, sub, NULL);
            d->has = 1, d->staged = 1, ok = 1;
        }
    }
    if (tx) ID3D11Texture2D_Release(tx);
    if (db) IMFDXGIBuffer_Release(db);
    return ok;
}

// ほどけた絵を受け取って nv12 に写す。1 受け取った・0 もう無い・-1 失敗
static int vdec_pull(struct vdec *d) {
    for (int tries = 0; tries < 4; tries++) {
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!d->provides && !(ob.pSample = vid_sample(d->outsz))) return -1;
        DWORD st = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->t, 0, 1, &ob, &st);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            // 最初のコマ・大きさが変わったとき：出す形を決め直して、もう一度
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (!vdec_out_type(d)) return hr == MF_E_TRANSFORM_TYPE_NOT_SET ? 0 : -1;
            continue;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            return hr == MF_E_TRANSFORM_NEED_MORE_INPUT ? 0 : -1;
        }
        int ok = 0;
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(IMFSample_GetBufferByIndex(ob.pSample, 0, &mb)) && mb) {
            ok = (d->hw && vdec_take_gpu(d, mb)) || vdec_take_cpu(d, mb);
            IMFMediaBuffer_Release(mb);
        }
        LONGLONG t = 0;
        if (ok && SUCCEEDED(IMFSample_GetSampleTime(ob.pSample, &t))) d->tag = (uint64_t) t;
        IMFSample_Release(ob.pSample);
        return ok ? 1 : -1;
    }
    return -1;
}

// ほどけた最後の絵の NV12 を指す。GPU でほどいた物はここで読み出す（終わったら vdec_unmap）
static int vdec_map(struct vdec *d, const uint8_t **y, uint32_t *pitch, uint32_t *rows) {
    if (!d->has) return 0;
    if (!d->staged) {
        *y = d->nv12, *pitch = d->npitch, *rows = d->nrows;
        return 1;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *) d->stage, 0, D3D11_MAP_READ, 0, &m))) return 0;
    d->mapped = 1;
    *y = (const uint8_t *) m.pData, *pitch = m.RowPitch, *rows = d->sh; // UV の面は Y の面のすぐ後ろ（1 行は同じ長さ）
    return 1;
}

static void vdec_unmap(struct vdec *d) {
    if (d->mapped) ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *) d->stage, 0);
    d->mapped = 0;
}

// 1 コマぶんの NAL の並びを入れて、出てきた絵を受け取る。札は sample の時刻として通し、出てきた絵の物を d->tag に入れる。
// 1 ほどけた絵がある・0 まだ無い・-1 失敗
static int vdec_push(struct vdec *d, const uint8_t *au, size_t n, uint64_t tag) {
    IMFSample *s = vid_sample((DWORD) n);
    if (!s) return -1;
    IMFMediaBuffer *b = NULL;
    BYTE *p = NULL;
    if (SUCCEEDED(IMFSample_GetBufferByIndex(s, 0, &b)) && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        memcpy(p, au, n);
        IMFMediaBuffer_Unlock(b);
        IMFMediaBuffer_SetCurrentLength(b, (DWORD) n);
    }
    if (b) IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, (LONGLONG) tag);
    int got = 0;
    HRESULT hr = IMFTransform_ProcessInput(d->t, 0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        // 先に出てきた絵を受け取ってから入れ直す
        int k;
        while ((k = vdec_pull(d)) > 0) got = 1;
        if (k < 0) { IMFSample_Release(s); return -1; }
        hr = IMFTransform_ProcessInput(d->t, 0, s, 0);
    }
    IMFSample_Release(s);
    if (FAILED(hr)) return -1;
    int k;
    while ((k = vdec_pull(d)) > 0) got = 1;
    return k < 0 ? -1 : got;
}

// この PC で H.264 をほどけるか（Windows の N 版などは Media Foundation が無い）。最初の 1 回だけ、別の糸で試す
// （アプリの糸の COM の設定に触らないため）
static DWORD WINAPI vdec_probe_thread(LPVOID p) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    struct vdec d;
    *(int *) p = vdec_open(&d, 0);
    vdec_close(&d);
    CoUninitialize();
    return 0;
}

static int vdec_available(void) {
    static volatile LONG v = -1;
    if (v < 0) {
        static int ok; // 糸が待ちきれずに残っても書ける所
        HANDLE t = CreateThread(NULL, 0, vdec_probe_thread, &ok, 0, NULL);
        if (t && WaitForSingleObject(t, 10000) != WAIT_OBJECT_0) ok = 0;
        if (t) CloseHandle(t);
        if (!ok) say("この PC では H.264 をほどけないので、窓の絵は JPEG で受け取る");
        InterlockedExchange(&v, ok);
    }
    return v > 0;
}

static volatile LONG64 g_vstream; // 流れの番号（swapchain ごとに新しくする）

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
    // H.264 で受け取る（受け手が VR_CAP_H264 を名乗ったとき。IPHONE_VK_VIDEO=0 で JPEG に戻す）。use_jpeg も 1 のまま
    int use_video;
    int upscale;               // 受け手に MetalFX で 1.5 倍に広げてもらう（IPHONE_VK_UPSCALE=1）。広げた絵は別の窓に出す
    uint32_t vw, vh;           // H.264 で届く絵の大きさ（広げるときは 1.5 倍）
    HWND vwin;                 // 広げた絵を出す窓（表示の糸が作る）
    uint32_t v_kbps, v_fps, v_gop;
    uint64_t v_stream;         // 流れの番号
    volatile LONG v_need_key;  // 表示の糸がほどけなかった。次に頼むコマをキーフレームにしてもらう
    volatile LONG v_broken;    // 表示の糸でデコーダを作れなかった。JPEG に戻す
    uint64_t v_bytes, v_keys, v_got, v_decoded; // 受け取った H.264 の合計・キーフレームの数・受け取ったコマ・ほどいたコマ
    double v_dec_ms, v_cvt_ms, v_draw_ms;        // 表示の糸：ほどく・NV12 を BGRA に直す・窓に描く（合計）
    int v_hwdec;                                 // GPU でほどいている
    double cpu0, disp_cpu;     // 作ったときのプロセスの CPU 時間・表示の糸の CPU 時間（ms）
    // 窓に描く糸。アプリの糸は JPEG を渡してすぐ戻り、この糸が最新の 1 枚だけをほどいて描く（間に合わない絵は飛ばす）
    struct {
        HANDLE thr;
        SRWLOCK lk;
        CONDITION_VARIABLE cv;
        vr_buf jpg;
        int has, quit;
        uint64_t dropped;
        uint64_t serial; // 渡した絵が何回目の present の物か（0 なら分からない）
        int skip;        // H.264：渡す側がためすぎて捨てた。表示の糸はキーフレームまで待つ
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
    double fps_t;      // IPHONE_VK_FPS_LOG：直近の数え始めの時刻とコマ数
    uint64_t fps_f;
};

// コマを 1 つ数える。IPHONE_VK_FPS_LOG=1 なら、5 秒ごとに直近のコマ数を記録に出す（読み込みの時間を混ぜずに速さを見る）
static void sc_tick(struct swapchain *sc) {
    sc->frames++;
    static int on = -1;
    if (on < 0) { const char *e = getenv("IPHONE_VK_FPS_LOG"); on = e && *e == '1'; }
    if (!on) return;
    double t = now_ms();
    if (!sc->fps_t) { sc->fps_t = t; sc->fps_f = sc->frames; return; }
    if (t - sc->fps_t >= 5000) {
        say("直近 %.1f 秒：%.1f コマ/秒", (t - sc->fps_t) / 1000, (sc->frames - sc->fps_f) * 1000.0 / (t - sc->fps_t));
        sc->fps_t = t;
        sc->fps_f = sc->frames;
    }
}

#endif

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
#include "ls.inc"

static VkResult VKAPI_CALL begin_cb(VkCommandBuffer cbh, const VkCommandBufferBeginInfo *bi);
static VkResult VKAPI_CALL end_cb(VkCommandBuffer cbh);
static void VKAPI_CALL cmd_barrier(VkCommandBuffer cbh, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                                   uint32_t nm, const VkMemoryBarrier *mb, uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *ib);
static void VKAPI_CALL cmd_copy_image_to_buffer(VkCommandBuffer cbh, VkImage im, VkImageLayout l, VkBuffer bb, uint32_t n, const VkBufferImageCopy *rg);
static void VKAPI_CALL get_queue(VkDevice dv, uint32_t fam, uint32_t idx, VkQueue *out);

#ifdef _WIN32
static void disp_stop(struct swapchain *sc);
static int disp_thread_on(void);

// CPU を使った時間（ms）。FILETIME は 100 ns 単位
static double ft_ms(const FILETIME *f) { return (double) (((uint64_t) f->dwHighDateTime << 32) | f->dwLowDateTime) / 1e4; }
static double proc_cpu_ms(void) {
    FILETIME c, e, k, u;
    return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft_ms(&k) + ft_ms(&u) : 0;
}

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
                        sc->use_video ? "・H.264" : sc->use_jpeg ? "・JPEG" : "");
    if (sc->frames && sc->use_jpeg && !sc->use_video) say("JPEG：1 コマ平均 %.1f KB", sc->jpeg_bytes / 1024.0 / (double) sc->frames);
    if (sc->v_got)
        say("H.264：1 コマ平均 %.1f KB・受け取った %llu コマ・ほどいた %llu コマ・キーフレーム %llu 枚", sc->v_bytes / 1024.0 / (double) sc->v_got,
            (unsigned long long) sc->v_got, (unsigned long long) sc->v_decoded, (unsigned long long) sc->v_keys);
    if (sc->v_decoded && sc->frames)
        say("H.264 の表示の糸（%s）：ほどく %.2f ms（1 コマ）・BGRA に直す %.2f ms・窓に描く %.2f ms（1 回）", sc->v_hwdec ? "GPU" : "ソフト",
            sc->v_dec_ms / (double) sc->v_decoded,
            sc->v_cvt_ms / (double) sc->frames, sc->v_draw_ms / (double) sc->frames);
    if (sc->npres)
        say("CPU：このプロセス %.0f ms（present 1 回 %.2f ms）・窓に描く糸 %.0f ms（1 回 %.2f ms）", proc_cpu_ms() - sc->cpu0,
            (proc_cpu_ms() - sc->cpu0) / (double) sc->npres, sc->disp_cpu, sc->disp_cpu / (double) sc->npres);
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
    // 窓の絵を H.264 で受け取る：IPHONE_VK_VIDEO=1 のときで、受け手が名乗り、表示の糸を使い、この PC でほどけるとき。
    // 代役では量は減るが、遅れと PC の CPU はわずかに増えた。iPhone で測るまでは、既定は今までの JPEG。
    // ビットレートは 1 コマあたりの量で決める（区切りの速さ、区切らなければ 60 コマ/秒として）。キーフレームは 10 秒ごと。
    // IPHONE_VK_VIDEO_KBPS・IPHONE_VK_VIDEO_GOP（コマ）で変える
    {
        const char *e = getenv("IPHONE_VK_VIDEO");
        sc->use_video = sc->use_jpeg && (g_hw.caps & VR_CAP_H264) && disp_thread_on() && e && *e == '1' && vdec_available();
        sc->vw = sc->w, sc->vh = sc->h;
        {
            const char *u = getenv("IPHONE_VK_UPSCALE");
            sc->upscale = sc->use_video && u && *u == '1' && (g_hw.caps & VR_CAP_UPSCALE);
            if (u && *u == '1' && !sc->upscale) say("MetalFX で広げられない（H.264 でない・受け手が名乗らない）");
            if (sc->upscale) {
                sc->vw = (sc->w * 3 / 2 + 1) & ~1u, sc->vh = (sc->h * 3 / 2 + 1) & ~1u;
                say("iPhone の MetalFX で %u x %u を %u x %u に広げ、別の窓に出す", sc->w, sc->h, sc->vw, sc->vh);
            }
        }
        if (sc->use_video) {
            const char *k = getenv("IPHONE_VK_VIDEO_KBPS"), *g = getenv("IPHONE_VK_VIDEO_GOP");
            sc->v_fps = sc->gap > 0 ? (uint32_t) (1000.0 / sc->gap + 0.5) : 60;
            if (sc->v_fps < 1) sc->v_fps = 1;
            double kb = (double) sc->vw * sc->vh * 0.15 * sc->v_fps / 1000.0;
            sc->v_kbps = k && atoi(k) > 0 ? (uint32_t) atoi(k) : kb < 1000 ? 1000 : kb > 100000 ? 100000 : (uint32_t) kb;
            sc->v_gop = g && atoi(g) > 0 ? (uint32_t) atoi(g) : sc->v_fps * 10;
            sc->v_stream = (uint64_t) InterlockedIncrement64(&g_vstream);
            sc->v_need_key = 1;
            say("窓の絵は H.264 で受け取る（%u kbps・%u コマ/秒・キーフレームは %u コマごと）", sc->v_kbps, sc->v_fps, sc->v_gop);
        }
    }
    sc->cpu0 = proc_cpu_ms();
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
        sc_tick(sc);
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
    FILETIME c, e, k, u;
    if (GetThreadTimes(sc->disp.thr, &c, &e, &k, &u)) sc->disp_cpu += ft_ms(&k) + ft_ms(&u);
    CloseHandle(sc->disp.thr);
    sc->disp.thr = NULL;
    free(sc->disp.jpg.p);
    sc->disp.jpg.p = NULL;
    if (sc->disp.dropped) say("窓に描く糸が間に合わず飛ばした絵：%llu 枚", (unsigned long long) sc->disp.dropped);
}

// IPHONE_VK_DUMP=コマ番号:ファイル名 で、そのコマを PPM に書く（絵が正しいかを確かめる用）。
// H.264 のときのコマ番号は、ほどいたコマの番号（窓に描かずに飛ばしたコマも数える）
static void dump_frame_at(struct swapchain *sc, const uint8_t *px, uint64_t frame);
static void dump_frame(struct swapchain *sc, const uint8_t *px) { dump_frame_at(sc, px, sc->frames); }
static void dump_frame_at(struct swapchain *sc, const uint8_t *px, uint64_t frame) {
    const char *dump = getenv("IPHONE_VK_DUMP");
    if (dump && (uint64_t) atoi(dump) == frame && strchr(dump, ':')) {
        FILE *fp = fopen(strchr(dump, ':') + 1, "wb");
        if (fp) {
            fprintf(fp, "P6\n%u %u\n255\n", sc->w, sc->h);
            for (size_t p = 0; p < (size_t) sc->w * sc->h; p++) { uint8_t rgb[3] = { px[p * 4 + 2], px[p * 4 + 1], px[p * 4] }; fwrite(rgb, 1, 3, fp); }
            fclose(fp);
            say("%llu コマ目を %s に書いた", (unsigned long long) frame, strchr(dump, ':') + 1);
        }
    }
    ls_ctl_dump(sc->w, sc->h, px);
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

// ---- 窓の絵を H.264 で受け取る ----
// H.264 は前のコマを元にするので、JPEG と違って間のコマを飛ばせない。表示の糸は届いた順に全部ほどき、最後の 1 枚だけを描く
// 広げた絵を出す窓。表示の糸が作り、その糸で知らせを回す。窓の文は仮（言葉は鶴田さんが決める）
// 閉じる知らせは、この窓では壊さずにゲームの窓へ回す（ゲームが閉じれば表示の糸が終わって、この窓も壊す）
static LRESULT CALLBACK up_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) {
        HWND g = (HWND) GetWindowLongPtrW(w, GWLP_USERDATA);
        if (g && IsWindow(g)) PostMessageW(g, WM_CLOSE, 0, 0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
static HWND up_window(uint32_t w, uint32_t h) {
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = up_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"PocketGPUUpscale";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
        cls = RegisterClassW(&wc);
    }
    RECT rc = { 0, 0, (LONG) w, (LONG) h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, L"PocketGPUUpscale", L"PocketGPU MetalFX", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

static void draw_bgra_to(HWND hw, const uint8_t *px, uint32_t w, uint32_t h) {
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG) w;
    bmi.bmiHeader.biHeight = -(LONG) h; // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RECT rc;
    GetClientRect(hw, &rc);
    HDC dc = GetDC(hw);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, (int) w, (int) h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hw, dc);
    GdiFlush();
}

static void draw_bgra(struct swapchain *sc, const uint8_t *px) {
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG) sc->w;
    bmi.bmiHeader.biHeight = -(LONG) sc->h; // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RECT rc;
    GetClientRect(sc->hwnd, &rc);
    HDC dc = GetDC(sc->hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, (int) sc->w, (int) sc->h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(sc->hwnd, dc);
    GdiFlush();
}

// 窓に描く糸（H.264）：渡されたコマを順にほどいて、最後の 1 枚を描く
static DWORD WINAPI vdisp_thread(LPVOID arg) {
    struct swapchain *sc = (struct swapchain *) arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    struct vdec d;
    if (!vdec_open(&d, 1)) {
        say("H.264 のデコーダを作れない。窓の絵は JPEG に戻す");
        InterlockedExchange(&sc->v_broken, 1);
    } else {
        say("H.264 を%sでほどく", d.hw ? " GPU（D3D11）" : "ソフト");
        sc->v_hwdec = d.hw;
    }
    uint8_t *px = malloc((size_t) sc->vw * sc->vh * 4);
    vr_buf mine = { 0 };
    int wait_key = 1; // キーフレームが来るまでほどかない（最初・失敗したあと・ためすぎて捨てたあと）
    uint64_t seen = 0; // ほどいたコマの番号（IPHONE_VK_DUMP 用）
    const char *dump = getenv("IPHONE_VK_DUMP");
    for (;;) {
        AcquireSRWLockExclusive(&sc->disp.lk);
        while (!sc->disp.has && !sc->disp.quit) SleepConditionVariableSRW(&sc->disp.cv, &sc->disp.lk, INFINITE, 0);
        if (!sc->disp.has) { ReleaseSRWLockExclusive(&sc->disp.lk); break; }
        vr_buf t = mine;
        mine = sc->disp.jpg;
        sc->disp.jpg = t;
        sc->disp.jpg.n = 0;
        sc->disp.has = 0;
        if (sc->disp.skip) wait_key = 1, sc->disp.skip = 0;
        ReleaseSRWLockExclusive(&sc->disp.lk);
        if (!d.t || !px) continue;
        int shown = 0;
        for (size_t at = 0; at + 4 <= mine.n;) {
            uint32_t n;
            memcpy(&n, mine.p + at, 4);
            at += 4;
            if (n > mine.n - at) break;
            const uint8_t *a = mine.p + at;
            at += n;
            if (n < VR_H264_HEAD) continue; // 先送りの最初（まだ何も無い）
            uint64_t st, tag;
            uint32_t aw, ah, fl;
            memcpy(&st, a, 8), memcpy(&tag, a + 8, 8), memcpy(&aw, a + 16, 4), memcpy(&ah, a + 20, 4), memcpy(&fl, a + 24, 4);
            // 作り直す前の swapchain の絵は出さない。受け手が落としたコマ（中身が空）は飛ばす（次はキーフレームが来る）
            if (st != sc->v_stream || aw != sc->vw || ah != sc->vh || n == VR_H264_HEAD) continue;
            if (wait_key && !(fl & 1)) continue;
            wait_key = 0;
            double t0 = now_ms();
            int k = vdec_push(&d, a + VR_H264_HEAD, n - VR_H264_HEAD, tag);
            sc->v_dec_ms += now_ms() - t0;
            if (k < 0) {
                say("H.264 をほどけなかった。キーフレームから始め直す");
                wait_key = 1;
                InterlockedExchange(&sc->v_need_key, 1);
                continue;
            }
            if (k > 0) {
                shown = 1;
                sc->v_decoded++;
                if (dump && !sc->upscale && (uint64_t) atoi(dump) == ++seen) {
                    const uint8_t *y;
                    uint32_t pitch, rows;
                    if (vdec_map(&d, &y, &pitch, &rows) && rows >= sc->h && pitch >= sc->w) {
                        vid_nv12_to_bgra(y, pitch, y + (size_t) pitch * rows, pitch, sc->w, sc->h, px);
                        dump_frame_at(sc, px, seen);
                    }
                    vdec_unmap(&d);
                }
            }
        }
        if (!shown) continue;
        double t1 = now_ms();
        const uint8_t *y;
        uint32_t pitch, rows;
        int got = vdec_map(&d, &y, &pitch, &rows) && rows >= sc->vh && pitch >= sc->vw;
        if (got) vid_nv12_to_bgra(y, pitch, y + (size_t) pitch * rows, pitch, sc->vw, sc->vh, px);
        vdec_unmap(&d);
        if (!got) continue;
        double t2 = now_ms();
        if (sc->upscale) {
            if (!sc->vwin && (sc->vwin = up_window(sc->vw, sc->vh))) SetWindowLongPtrW(sc->vwin, GWLP_USERDATA, (LONG_PTR) sc->hwnd);
            if (sc->vwin) draw_bgra_to(sc->vwin, px, sc->vw, sc->vh);
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        } else {
            draw_bgra(sc, px);
        }
        sc->v_cvt_ms += t2 - t1;
        sc->v_draw_ms += now_ms() - t2;
        uint64_t serial = d.tag;
        if (serial && sc->npres - serial < 64) {
            double lat = now_ms() - sc->pres_t[serial % 64];
            sc->lat_sum += lat;
            sc->lat_n++;
            if (lat > sc->lat_max) sc->lat_max = lat;
            g_stat.lat_sum += lat;
            g_stat.lat_n++;
            if (lat > g_stat.lat_max) g_stat.lat_max = lat;
        }
        sc_tick(sc);
    }
    if (sc->vwin) { DestroyWindow(sc->vwin); sc->vwin = NULL; }
    free(px);
    free(mine.p);
    vdec_close(&d);
    CoUninitialize();
    return 0;
}

// 受け取ったコマを表示の糸に渡す（後ろに足す）。ためすぎたら（ほどくのが追いつかない）捨てて、キーフレームから始め直す
static void disp_give_video(struct swapchain *sc, const vr_buf *out) {
    if (!sc->disp.thr) {
        InitializeSRWLock(&sc->disp.lk);
        InitializeConditionVariable(&sc->disp.cv);
        sc->disp.thr = CreateThread(NULL, 0, vdisp_thread, sc, 0, NULL);
    }
    if (out->n >= VR_H264_HEAD) {
        uint32_t fl;
        memcpy(&fl, out->p + 24, 4);
        sc->v_bytes += out->n - VR_H264_HEAD;
        sc->v_keys += fl & 1;
        sc->v_got++;
    }
    AcquireSRWLockExclusive(&sc->disp.lk);
    if (sc->disp.jpg.n > (16u << 20)) {
        sc->disp.jpg.n = 0;
        sc->disp.dropped++;
        sc->disp.skip = 1;
        InterlockedExchange(&sc->v_need_key, 1);
    }
    uint32_t n = (uint32_t) out->n;
    vr_put(&sc->disp.jpg, &n, 4);
    vr_put(&sc->disp.jpg, out->p, out->n);
    sc->disp.has = 1;
    WakeConditionVariable(&sc->disp.cv);
    ReleaseSRWLockExclusive(&sc->disp.lk);
}

// H.264 をやめて JPEG に戻す（受け手が縮められない・この PC でほどけない）。表示の糸は JPEG の物に替える
static void video_fallback(struct swapchain *sc) {
    say("窓の絵を H.264 で受け取れないので、この窓は JPEG に戻す");
    sc->use_video = 0;
    disp_stop(sc);
    sc->disp.quit = 0;
    sc->disp.has = 0;
    sc->disp.skip = 0;
}

// 受け手に H.264 で頼む。now ならその場で今のコマを（区切りの待ちがあるとき）、でなければ JPEG と同じ先送り
// （IPHONE_VK_JPEG_PIPE・IPHONE_VK_ASYNC_PRESENT に従う）。受け取ったコマは表示の糸に渡す。
// 受け手が失敗を返したら、その結果を返す（呼んだ側が JPEG に戻す）
static VkResult video_request(struct swapchain *sc, uint32_t j, uint64_t serial, int now) {
    static int pipe = -1, async = -1;
    if (pipe < 0) {
        const char *p = getenv("IPHONE_VK_JPEG_PIPE"), *a = getenv("IPHONE_VK_ASYNC_PRESENT");
        pipe = (g_hw.caps & VR_CAP_JPEG_PIPE) && !(p && *p == '0');
        async = pipe && !(a && *a == '0');
    }
    if (sc->v_broken) return VK_ERROR_FEATURE_NOT_PRESENT;
    int key = InterlockedExchange(&sc->v_need_key, 0) != 0;
    vr_buf b = { 0 }, out = { 0 };
    vr_u64(&b, ID(sc->rbmem[j]));
    vr_u64(&b, 0);
    vr_u32(&b, sc->w);
    vr_u32(&b, sc->h);
    vr_u32(&b, sc->w * 4);
    vr_u32(&b, sc->fmt == VK_FORMAT_B8G8R8A8_UNORM || sc->fmt == VK_FORMAT_B8G8R8A8_SRGB ? 1 : 0);
    vr_u32(&b, (key ? VR_H264_KEY : 0) | (!now && pipe ? VR_H264_PIPE : 0) | (sc->upscale ? VR_H264_UP15 : 0));
    vr_u32(&b, sc->v_kbps);
    vr_u32(&b, sc->v_fps);
    vr_u32(&b, sc->v_gop);
    vr_u64(&b, sc->v_stream);
    vr_u64(&b, serial);
    VkResult r;
    if (!now && async) {
        // 前のコマで頼んだ返事を受け取ってから、今のコマを頼む（返事は次のコマで受け取る）
        r = rpc_take_deferred(&out);
        if (r == VK_NOT_READY) r = VK_SUCCESS;
        if (r == VK_SUCCESS) rpc_call_deferred(VR_READ_H264, &b);
        else if (key) InterlockedExchange(&sc->v_need_key, 1);
    } else {
        r = rpc_call(VR_READ_H264, &b, &out);
    }
    free(b.p);
    if (r == VK_SUCCESS && out.n) disp_give_video(sc, &out);
    free(out.p);
    return r;
}

// H.264 で窓の絵を頼む。1 頼めた・0 JPEG で頼み直す（JPEG に戻した）・-1 つながりが切れた（r に結果）
static int video_blit(struct swapchain *sc, uint32_t j, uint64_t serial, int now, VkResult *r) {
    if (!sc->use_video) return 0;
    *r = sc->v_broken ? VK_ERROR_FEATURE_NOT_PRESENT : video_request(sc, j, serial, now);
    if (*r == VK_SUCCESS) return 1;
    if (g_dead) return -1;
    video_fallback(sc);
    return 0;
}

// 今のコマの JPEG をその場で受け取って、表示の糸に渡す（区切りの待ちがあるときの、遅れの少ないやり方）
static VkResult blit_now(struct swapchain *sc, uint32_t j, uint64_t serial) {
    VkResult vr = VK_SUCCESS;
    int vk = video_blit(sc, j, serial, 1, &vr);
    if (vk < 0) return vr;
    if (vk > 0) {
        RECT rc;
        GetClientRect(sc->hwnd, &rc);
        return ask_resize(sc, &rc) ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
    }
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
        // H.264 で頼めたら、それで終わり（ほどいて描くのは表示の糸）
        VkResult vr = VK_SUCCESS;
        int vk = video_blit(sc, j, serial, 0, &vr);
        if (vk != 0) {
            RECT rc;
            GetClientRect(sc->hwnd, &rc);
            if (vk > 0 && ask_resize(sc, &rc)) return VK_SUBOPTIMAL_KHR;
            return VK_SUCCESS;
        }
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
    sc_tick(sc);
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
    if (!g_stat.presents) g_stat.cpu0 = proc_cpu_ms();
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
            // 測る用：IPHONE_VK_PRESENT_EVERY=N で N コマに 1 回だけ窓の絵を受け取る（GPU は全部描く）。絵を縮めて送る所の重さを切り分ける
            static int every = -1;
            if (every < 0) { const char *e = getenv("IPHONE_VK_PRESENT_EVERY"); every = e && atoi(e) > 1 ? atoi(e) : 1; if (every > 1) say("窓の絵は %d コマに 1 回だけ受け取る（測る用）", every); }
            if (r == VK_SUCCESS && every > 1 && serial % every) { g_stat.blits++; continue; }
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

#else
// CPU を使った時間（ms）
static double proc_cpu_ms(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru)) return 0;
    return (double) (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 + (double) (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}
#endif

// ---- shader・パイプライン ----
// ---- シェーダーが書く場所 ----
// GPU が書くかもしれない CPU から見えるメモリは、終わったあと PC に読み戻す。これまでは storage buffer につないだ物を
// 全部読み戻していて、vkQuake では 1 コマに約 2 MB、USB くらいの通り道ではそれだけで詰まっていた。
// 作られたシェーダーのどれかが「書く」と宣言した (set, binding) だけを覚え、そこにつないだ物だけを読み戻す。
// 「読むだけ」（NonWritable）の場所は読み戻さない。読み解けないシェーダーがあれば、今までどおり全部読み戻す。
// IPHONE_VK_RO_SKIP=0 で前のやり方
static volatile LONG g_wr[32][8]; // set ごとに binding 256 個までの印
static volatile LONG g_wr_all;
// ---- shader module と pipeline ごとの、書く場所の一覧 ----
// 全部のシェーダーをまとめて (set, binding) で覚えると、同じ場所を計算のシェーダーでは書き、描くシェーダーでは読むだけに使う
// アプリ（Godot など）で、読むだけの buffer まで毎回読み戻してしまう。pipeline ごとに覚え、command buffer でつないだ物だけで決める
struct wrinfo { int all; uint32_t n, cap; uint32_t *sb; int psb; uint64_t pcr, pcw; }; // sb：set << 16 | binding。all は「分からないので全部書く」
// psb：1 アドレス（PhysicalStorageBuffer）で読み書きする・2 出どころの分からないアドレスに書く・4 出どころの分からないアドレスから読む。
// pcr・pcw：アドレスで読む・書くとき、そのアドレスが入っていた push constant の 4 バイトごとの印（63 番は 252 バイト目より後の全部）
#define WRM_N 4096
static struct wrnode { uint64_t id; struct wrinfo *w; struct wrnode *next; } *g_wrm[WRM_N];
static SRWLOCK g_wrm_lock = SRWLOCK_INIT;
static unsigned wrm_slot(uint64_t id) { return (unsigned) ((id * 0x9E3779B97F4A7C15ull) >> 52) & (WRM_N - 1); }
static void wri_add(struct wrinfo *w, uint32_t set, uint32_t binding) {
    if (!w) return;
    if (set > 0xffff || binding > 0xffff) { w->all = 1; return; }
    uint32_t k = set << 16 | binding;
    for (uint32_t i = 0; i < w->n; i++) if (w->sb[i] == k) return;
    if (w->n == w->cap) {
        uint32_t cap = w->cap ? w->cap * 2 : 8;
        uint32_t *q = realloc(w->sb, cap * sizeof *q);
        if (!q) { w->all = 1; return; }
        w->sb = q;
        w->cap = cap;
    }
    w->sb[w->n++] = k;
}
static int wri_has(const struct wrinfo *w, uint32_t set, uint32_t binding) {
    uint32_t k = set << 16 | binding;
    for (uint32_t i = 0; i < w->n; i++) if (w->sb[i] == k) return 1;
    return 0;
}
static void wri_merge(struct wrinfo *u, const struct wrinfo *w) {
    if (!w) { u->all = 1; return; }
    if (w->all) u->all = 1;
    u->psb |= w->psb;
    u->pcr |= w->pcr;
    u->pcw |= w->pcw;
    for (uint32_t i = 0; i < w->n; i++) wri_add(u, w->sb[i] >> 16, w->sb[i] & 0xffff);
}
static void wri_free(struct wrinfo *w) { if (w) { free(w->sb); free(w); } }
static struct wrinfo *wrm_find(uint64_t id) { // g_wrm_lock を持って呼ぶ
    for (struct wrnode *n = g_wrm[wrm_slot(id)]; n; n = n->next) if (n->id == id) return n->w;
    return NULL;
}
static void wrm_del(uint64_t id) {
    AcquireSRWLockExclusive(&g_wrm_lock);
    for (struct wrnode **pp = &g_wrm[wrm_slot(id)]; *pp; pp = &(*pp)->next)
        if ((*pp)->id == id) {
            struct wrnode *n = *pp;
            *pp = n->next;
            wri_free(n->w);
            free(n);
            break;
        }
    ReleaseSRWLockExclusive(&g_wrm_lock);
}
static void wrm_put(uint64_t id, struct wrinfo *w) {
    if (!w) return;
    wrm_del(id);
    struct wrnode *n = malloc(sizeof *n);
    if (!n) { wri_free(w); return; }
    n->id = id;
    n->w = w;
    AcquireSRWLockExclusive(&g_wrm_lock);
    n->next = g_wrm[wrm_slot(id)];
    g_wrm[wrm_slot(id)] = n;
    ReleaseSRWLockExclusive(&g_wrm_lock);
}

static void wr_mark(uint32_t set, uint32_t binding) {
    if (set >= 32 || binding >= 256) { InterlockedExchange(&g_wr_all, 1); return; }
    InterlockedOr(&g_wr[set][binding / 32], (LONG) (1u << (binding % 32)));
}
static int wr_may(uint32_t set, uint32_t binding) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("IPHONE_VK_RO_SKIP"); off = e && *e == '0'; }
    if (off || g_wr_all || set >= 32 || binding >= 256) return 1;
    return ((uint32_t) g_wr[set][binding / 32] >> (binding % 32)) & 1;
}
// 根の変数を結ぶ（0 は根なし・WR_MANY は 2 つ以上）
#define WR_MANY 0xffffffffu
static void wr_link(uint32_t *root, uint32_t bound, uint32_t dst, uint32_t src) {
    if (dst >= bound || src >= bound || !root[src]) return;
    uint32_t r = root[src];
    if (!root[dst]) root[dst] = r;
    else if (root[dst] != r) root[dst] = WR_MANY;
}
static void wr_store(const uint32_t *root, uint8_t *stored, uint32_t bound, uint32_t p, int *all) {
    if (p >= bound || !root[p]) return;
    if (root[p] == WR_MANY) *all = 1;
    else stored[root[p]] = 1;
}
// 中で書くかまで見るか（IPHONE_VK_RO_DEEP=0 で宣言だけを見る前のやり方）
static int wr_deep(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_RO_DEEP"); v = !(e && *e == '0'); }
    return v;
}

// ---- アドレス（PhysicalStorageBuffer）で読み書きする所の、アドレスの出どころ（u1） ----
// vkd3d-proton は root の CBV・SRV・UAV のアドレスを push constant に入れる。シェーダーはそれを読み（Load）、PSB のポインタに直し
// （Bitcast・ConvertUToPtr）、足し算や access chain で進めてから読み書きする。関数の中の変数にいったん入れて取り出す形もある（vkd3d-proton の中のシェーダー）。
// push constant の欄（何バイト目か）を印にしてその筋を追い、アドレスで書く・読む所の印を info に足す。追えないアドレスで書く・読む物があれば psb の 2・4 を立てる。
// w は wr_scan が形を確かめた物（命令の語の数が 0 でなく、はみ出さない）
static uint64_t psb_slot(uint32_t off) { return 1ull << (off / 4 < 63 ? off / 4 : 63); }
static void psb_scan(const uint32_t *w, size_t nw, struct wrinfo *info) {
    if (!info) return;
    int cap = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (op == 17 && wc >= 2 && w[i + 1] == 5347) cap = 1; // OpCapability PhysicalStorageBufferAddresses
        if (op == 54) break;                                  // 関数より前に書く決まり
    }
    if (!cap) return;
    info->psb |= 1;
    uint32_t bound = w[3];
    uint64_t *tag = calloc(bound, 8), *mtag = calloc(bound, 8);
    uint32_t *psc = calloc(bound, 4), *rty = calloc(bound, 4), *vbase = calloc(bound, 4), *cval = calloc(bound, 4), *pcs = calloc(bound, 4);
    uint8_t *isc = calloc(bound, 1), *ispcs = calloc(bound, 1);
    struct { uint32_t s, m, off; } *mo = NULL;
    size_t nmo = 0;
    if (!tag || !mtag || !psc || !rty || !vbase || !cval || !pcs || !isc || !ispcs) { info->psb |= 6; info->pcr = info->pcw = ~0ull; goto out; }
    // 型・定数・変数と、ポインタを作る命令の結果の型
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        if (op == 32 && wc >= 4 && x[1] < bound) psc[x[1]] = x[2] + 1;      // OpTypePointer（+1 で 0 と分ける）
        else if (op == 39 && wc >= 3 && x[1] < bound) psc[x[1]] = x[2] + 1; // OpTypeForwardPointer
        else if (op == 43 && wc >= 4 && x[2] < bound) { cval[x[2]] = x[3]; isc[x[2]] = 1; }
        else if (op == 59 && wc >= 4 && x[2] < bound) {
            if (x[3] == 9) { // PushConstant：中の struct の型を覚える
                uint32_t t = 0;
                for (size_t j = 5; j < i; j += w[j] >> 16)
                    if ((w[j] & 0xffff) == 32 && (w[j] >> 16) >= 4 && w[j + 1] == x[1]) t = w[j + 3];
                if (t && t < bound) { pcs[x[2]] = t; ispcs[t] = 1; }
            } else if (x[3] == 7 || x[3] == 6 || x[3] == 4) vbase[x[2]] = x[2]; // Function・Private・Workgroup の変数
        }
        switch (op) {
        case 1: case 46: case 55: case 57: case 59: case 61: case 65: case 66: case 67: case 70: case 81: case 83: case 120: case 124: case 169: case 245: case 400:
            if (wc >= 3 && x[2] < bound) rty[x[2]] = x[1];
            break;
        default: break;
        }
    }
    // push constant の struct の欄の位置（OpMemberDecorate Offset）
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        const uint32_t *x = &w[i];
        if ((x[0] & 0xffff) == 72 && (x[0] >> 16) >= 5 && x[3] == 35 && x[1] < bound && ispcs[x[1]]) {
            void *p = realloc(mo, (nmo + 1) * sizeof *mo);
            if (!p) { info->psb |= 6; info->pcr = info->pcw = ~0ull; goto out; }
            mo = p;
            mo[nmo].s = x[1];
            mo[nmo].m = x[2];
            mo[nmo].off = x[4];
            nmo++;
        }
    }
    // 変数を指すポインタの元の変数
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        if ((op == 65 || op == 66 || op == 67 || op == 70 || op == 83) && wc >= 4 && x[2] < bound && x[3] < bound && vbase[x[3]]) vbase[x[2]] = vbase[x[3]];
    }
#define PSB_ADD(d, s) do { if ((d) < bound && (s) < bound && (tag[s] & ~tag[d])) { tag[d] |= tag[s]; ch = 1; } } while (0)
    // 印を広げる（phi は後ろで作られる物も指すので、変わらなくなるまで何回か回す）
    for (int pass = 0; pass < 16; pass++) {
        int ch = 0;
        for (size_t i = 5; i < nw; i += w[i] >> 16) {
            uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
            const uint32_t *x = &w[i];
            if ((op == 65 || op == 66) && wc >= 4 && x[2] < bound && x[3] < bound && pcs[x[3]]) {
                // push constant の欄を指す：欄の番号が定数で、その先を掘らなければ、その欄の位置
                uint64_t t = ~0ull;
                if (wc == 5 && x[4] < bound && isc[x[4]])
                    for (size_t k = 0; k < nmo; k++)
                        if (mo[k].s == pcs[x[3]] && mo[k].m == cval[x[4]]) { t = psb_slot(mo[k].off); break; }
                if (t & ~tag[x[2]]) { tag[x[2]] |= t; ch = 1; }
                continue;
            }
            switch (op) {
            case 61: case 124: case 120: case 83: case 113: case 114: case 81: case 65: case 66: case 67: case 70:
                if (wc >= 4) PSB_ADD(x[2], x[3]);
                if (op == 61 && wc >= 4 && x[3] < bound && vbase[x[3]] && x[2] < bound && (mtag[vbase[x[3]]] & ~tag[x[2]])) { tag[x[2]] |= mtag[vbase[x[3]]]; ch = 1; }
                break;
            case 128: case 130: case 197: case 198: case 199: case 79: case 82:
                if (wc >= 5) { PSB_ADD(x[2], x[3]); PSB_ADD(x[2], x[4]); }
                break;
            case 80:
                for (uint32_t k = 3; k < wc; k++) PSB_ADD(x[2], x[k]);
                break;
            case 245:
                for (uint32_t k = 3; k < wc; k += 2) PSB_ADD(x[2], x[k]);
                break;
            case 169:
                if (wc >= 6) { PSB_ADD(x[2], x[4]); PSB_ADD(x[2], x[5]); }
                break;
            case 62: // 関数の中の変数にしまう
                if (wc >= 3 && x[1] < bound && x[2] < bound && vbase[x[1]] && (tag[x[2]] & ~mtag[vbase[x[1]]])) { mtag[vbase[x[1]]] |= tag[x[2]]; ch = 1; }
                break;
            default: break;
            }
        }
        if (!ch) break;
    }
#undef PSB_ADD
    // アドレスで書く・読む所
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        uint32_t pw = 0, pr = 0; // 書く先・読む元のポインタ（0 は無し）
        if ((op == 62 || op == 228) && wc >= 3) pw = x[1];                       // OpStore・OpAtomicStore
        else if ((op == 63 || op == 64) && wc >= 3) { pw = x[1]; pr = x[2]; }    // OpCopyMemory(Sized)
        else if (((op >= 229 && op <= 242) || op == 318 || op == 5614 || op == 5615 || op == 6035) && wc >= 4) pw = pr = x[3]; // 読んで書く atomic
        else if ((op == 61 || op == 227) && wc >= 4) pr = x[3];                 // OpLoad・OpAtomicLoad
        // PSB のポインタだけを見る（push constant そのものや関数の中の変数への読み書きは数えない）。型の分からない物は、印があれば数える
        if (pw && pw < bound) {
            int isp = rty[pw] && rty[pw] < bound && psc[rty[pw]] == 5349 + 1;
            if (tag[pw] && (isp || !rty[pw])) info->pcw |= tag[pw];
            else if (isp) info->psb |= 2;
        }
        if (pr && pr < bound) {
            int isp = rty[pr] && rty[pr] < bound && psc[rty[pr]] == 5349 + 1;
            if (tag[pr] && (isp || !rty[pr])) info->pcr |= tag[pr];
            else if (isp) info->psb |= 4;
        }
    }
out:
    free(tag); free(mtag); free(psc); free(rty); free(vbase); free(cval); free(pcs); free(isc); free(ispcs); free(mo);
}

static void wr_scan(const uint32_t *w, size_t nw, struct wrinfo *info) {
    if (nw < 5 || w[0] != 0x07230203 || w[3] > (1u << 22)) { InterlockedExchange(&g_wr_all, 1); if (info) info->all = 1; return; }
    uint32_t bound = w[3];
    uint32_t *set = calloc(bound, 4), *bnd = calloc(bound, 4), *memnw = calloc(bound, 4), *memn = calloc(bound, 4);
    uint32_t *ptr_t = calloc(bound, 4), *ptr_sc = calloc(bound, 4), *elem = calloc(bound, 4), *imgw = calloc(bound, 4);
    uint8_t *fl = calloc(bound, 1); // 1 set・2 binding・4 NonWritable・8 BufferBlock・16 struct
    uint32_t *vars = calloc(bound, 4), nvars = 0, *root = NULL;
    uint8_t *stored = NULL;
    if (!set || !bnd || !memnw || !memn || !ptr_t || !ptr_sc || !elem || !imgw || !fl || !vars) { InterlockedExchange(&g_wr_all, 1); if (info) info->all = 1; goto out; }
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) { InterlockedExchange(&g_wr_all, 1); if (info) info->all = 1; goto out; }
        const uint32_t *x = &w[i];
        if (op == 71 && wc >= 3 && x[1] < bound) { // OpDecorate
            if (x[2] == 34 && wc >= 4) { set[x[1]] = x[3]; fl[x[1]] |= 1; }
            else if (x[2] == 33 && wc >= 4) { bnd[x[1]] = x[3]; fl[x[1]] |= 2; }
            else if (x[2] == 24) fl[x[1]] |= 4;
            else if (x[2] == 3) fl[x[1]] |= 8;
        } else if (op == 72 && wc >= 4 && x[1] < bound && x[3] == 24) memnw[x[1]]++; // OpMemberDecorate NonWritable
        else if (op == 30 && wc >= 2 && x[1] < bound) { fl[x[1]] |= 16; memn[x[1]] = wc - 2; } // OpTypeStruct
        else if (op == 32 && wc >= 4 && x[1] < bound) { ptr_sc[x[1]] = x[2]; ptr_t[x[1]] = x[3]; } // OpTypePointer
        else if ((op == 28 || op == 29) && wc >= 3 && x[1] < bound) elem[x[1]] = x[2]; // OpTypeArray・OpTypeRuntimeArray
        else if (op == 25 && wc >= 9 && x[1] < bound) imgw[x[1]] = x[3] == 5 && x[7] == 2; // OpTypeImage：Buffer で storage
        else if (op == 59 && wc >= 4 && x[2] < bound) vars[nvars++] = i; // OpVariable
        i += wc;
    }
    // 宣言に readonly が無くても、中で書かない物は書かない（GLSL は readonly を付けない物が多い。GZDoom の光の表など）。
    // 変数から辿れるポインタ（access chain・写し・phi・select・texel pointer・load）を根の変数に結び、
    // store・atomic・画像への書き込み・関数や拡張命令への受け渡しの先が根なら「書く」。根が 2 つ以上に分かれる物へ書くなら、全部書くとみなす
    root = calloc(bound, 4);
    stored = calloc(bound, 1);
    if (!root || !stored) { InterlockedExchange(&g_wr_all, 1); if (info) info->all = 1; goto out; }
    for (uint32_t k = 0; k < nvars; k++) root[w[vars[k] + 2]] = w[vars[k] + 2];
    for (int pass = 0; pass < 3; pass++) // phi は後ろで作られる物も指すので、何回か回す
        for (size_t i = 5; i < nw; i += w[i] >> 16) {
            uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
            const uint32_t *x = &w[i];
            if ((op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 124 || op == 60 || op == 61) && wc >= 4)
                wr_link(root, bound, x[2], x[3]);
            else if (op == 245)
                for (uint32_t k = 3; k < wc; k += 2) wr_link(root, bound, x[2], x[k]);
            else if (op == 169 && wc >= 6) { wr_link(root, bound, x[2], x[4]); wr_link(root, bound, x[2], x[5]); }
        }
    int all = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        if ((op == 62 || op == 63 || op == 64 || op == 228 || op == 319 || op == 99) && wc >= 2) wr_store(root, stored, bound, x[1], &all);
        else if (((op >= 229 && op <= 242) || op == 318 || op == 5614 || op == 5615 || op == 6035) && wc >= 4) wr_store(root, stored, bound, x[3], &all);
        else if (op == 57) for (uint32_t k = 4; k < wc; k++) wr_store(root, stored, bound, x[k], &all);
        else if (op == 12) for (uint32_t k = 5; k < wc; k++) wr_store(root, stored, bound, x[k], &all);
    }
    for (uint32_t k = 0; k < nvars; k++) {
        const uint32_t *x = &w[vars[k]];
        uint32_t v = x[2], pt = x[1] < bound ? x[1] : 0, sc = x[3];
        if (!(fl[v] & 3)) continue;
        if (!all && !stored[v] && wr_deep()) continue; // 中で書いていない
        uint32_t t = pt ? ptr_t[pt] : 0;
        for (int d = 0; d < 4 && t < bound && elem[t]; d++) t = elem[t];
        if (t >= bound) continue;
        int writable = 0;
        if (sc == 12 || (sc == 2 && (fl[t] & 8))) // StorageBuffer、または古い形の BufferBlock
            writable = !(fl[v] & 4) && !((fl[t] & 16) && memn[t] && memnw[t] >= memn[t]);
        else if (sc == 0 && imgw[t]) // storage texel buffer
            writable = !(fl[v] & 4);
        if (writable) { wr_mark(set[v], bnd[v]); wri_add(info, set[v], bnd[v]); }
    }
    psb_scan(w, nw, info); // ここまで来たら、形は確かめてある
out:
    free(set); free(bnd); free(memnw); free(memn); free(ptr_t); free(ptr_sc); free(elem); free(imgw); free(fl); free(vars);
    free(root); free(stored);
}

// MoltenVK（中の SPIRV-Cross）が Metal に訳せない形を、意味を変えずに直す。IPHONE_VK_SPVFIX=0 で直さない
//   1. OpName・OpMemberName を外す。zink の "sampler" のように Metal の型の名前とぶつかる名前があると、訳した MSL が組めない
//   2. 計算の組の大きさを数で書き直す。DXVK 3 は OpExecutionModeId LocalSizeId（大きさを定数の番号で指す）で書く。
//      指す先が全部ふつうの数の定数なら、同じ長さの OpExecutionMode LocalSize（数をそのまま書く）に置き換える
//   3. 組で分け合うメモリ（Workgroup）を 0 で始める計算シェーダーには、組の大きさの定数（BuiltIn WorkgroupSize の印つき）を足す。
//      SPIRV-Cross は 0 で埋める繰り返しに gl_WorkGroupSize を使うのに、その印が無いと宣言しない（DXVK 3 の FluidCS11 で断られた）。
//      値は LocalSize と同じなので意味は変わらない
// 直したら新しく確保した並びを返す（*n に語の数）。直す所が無ければ NULL
static int spvfix_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("IPHONE_VK_SPVFIX"); v = !(e && *e == '0'); }
    return v;
}

static uint32_t *spv_fix(const uint32_t *w, size_t nw, size_t *n) {
    if (!spvfix_on() || nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3];
    uint32_t *cval = calloc(bound, sizeof *cval), *cdef = calloc(bound, sizeof *cdef); // OpConstant の値・何語目で作られたか
    if (!cval || !cdef) { free(cval); free(cdef); return NULL; }
    size_t nnames = 0, lsid_at = 0, ann_end = 0, first_type = 0, first_func = 0;
    int compute = 0, has_wgsize = 0, wg_init = 0, have_ls = 0;
    uint32_t ls[3] = { 0 }, t_uint = 0, t_v3 = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) { free(cval); free(cdef); return NULL; } // 壊れている：触らない
        const uint32_t *x = &w[i];
        if (op == 5 || op == 6) nnames++;
        else if (op == 15 && wc >= 3 && x[1] == 5) compute = 1; // OpEntryPoint GLCompute
        else if (op == 16 && wc == 6 && x[2] == 17) { ls[0] = x[3]; ls[1] = x[4]; ls[2] = x[5]; have_ls = 1; }
        else if (op == 331 && wc == 6 && x[2] == 38 && !lsid_at) lsid_at = i; // OpExecutionModeId LocalSizeId
        else if (op == 71 || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632) {
            ann_end = i + wc; // 印の並びの終わり
            if (op == 71 && wc == 4 && x[2] == 11 && x[3] == 25) has_wgsize = 1;
        } else if (op == 21 && wc == 4 && x[2] == 32 && x[3] == 0 && !t_uint) t_uint = x[1];
        else if (op == 23 && wc == 4 && t_uint && x[2] == t_uint && x[3] == 3 && !t_v3) t_v3 = x[1];
        else if (op == 43 && wc == 4 && x[2] < bound) { cval[x[2]] = x[3]; cdef[x[2]] = (uint32_t) i; }
        else if (op == 59 && wc >= 5 && x[3] == 4) wg_init = 1; // OpVariable Workgroup に初期値
        else if (op == 54 && !first_func) first_func = i;
        if (!first_type && op >= 19 && op <= 39) first_type = i;
        i += wc;
    }
    // LocalSizeId の指す先が全部ふつうの数の定数なら、その数
    int lsid_ok = 0;
    if (lsid_at) {
        lsid_ok = 1;
        uint32_t v[3];
        for (int k = 0; k < 3; k++) {
            uint32_t c = w[lsid_at + 3 + k];
            if (c >= bound || !cdef[c] || (w[cdef[c]] & 0xffff) != 43) lsid_ok = 0;
            else v[k] = cval[c];
        }
        if (lsid_ok) { memcpy(ls, v, sizeof ls); have_ls = 1; }
    }
    free(cval);
    free(cdef);
    int add_wg = compute && wg_init && !has_wgsize && have_ls && first_func && (ann_end || first_type);
    if (!nnames && !lsid_ok && !add_wg) return NULL;
    // 足す命令：（型が無ければ）uint・uint3、数の定数 3 つ、組、印
    uint32_t add_t[64], add_d[4];
    size_t nt = 0, nd = 0;
    uint32_t nb = bound;
    if (add_wg) {
        if (!t_uint) { t_uint = nb++; add_t[nt++] = (4u << 16) | 21; add_t[nt++] = t_uint; add_t[nt++] = 32; add_t[nt++] = 0; }
        if (!t_v3) { t_v3 = nb++; add_t[nt++] = (4u << 16) | 23; add_t[nt++] = t_v3; add_t[nt++] = t_uint; add_t[nt++] = 3; }
        uint32_t c[3];
        for (int k = 0; k < 3; k++) { c[k] = nb++; add_t[nt++] = (4u << 16) | 43; add_t[nt++] = t_uint; add_t[nt++] = c[k]; add_t[nt++] = ls[k]; }
        uint32_t comp = nb++;
        add_t[nt++] = (6u << 16) | 44; add_t[nt++] = t_v3; add_t[nt++] = comp; add_t[nt++] = c[0]; add_t[nt++] = c[1]; add_t[nt++] = c[2];
        add_d[nd++] = (4u << 16) | 71; add_d[nd++] = comp; add_d[nd++] = 11; add_d[nd++] = 25; // OpDecorate BuiltIn WorkgroupSize
    }
    uint32_t *o = malloc((nw + nt + nd) * sizeof *o);
    if (!o) return NULL;
    memcpy(o, w, 5 * sizeof *o);
    o[3] = nb;
    size_t k = 5;
    size_t dec_at = ann_end ? ann_end : first_type; // 印は印の並びの終わりに（無ければ型の前に）
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (nd && i == dec_at) { memcpy(o + k, add_d, nd * sizeof *o); k += nd; }
        if (nt && i == first_func) { memcpy(o + k, add_t, nt * sizeof *o); k += nt; } // 型と定数は関数の前に
        if (!(op == 5 || op == 6)) {
            memcpy(o + k, w + i, wc * sizeof *o);
            if (lsid_ok && i == lsid_at) { o[k] = (6u << 16) | 16; o[k + 2] = 17; o[k + 3] = ls[0]; o[k + 4] = ls[1]; o[k + 5] = ls[2]; } // → OpExecutionMode LocalSize
            k += wc;
        }
        i += wc;
    }
    *n = k;
    return o;
}

// ベクトルの 1 要素への atomic を、同じ並びの配列への atomic に直す。Metal はベクトルの要素のアドレスを取れず、
// vkd3d-proton が装置を作るときの計算シェーダー（GPU のアドレスで指す struct { uint3; uint; uint[] } の .x に atomic）を
// MoltenVK が組めなかった（address of vector element requested）。
// struct のメンバーがベクトルで、atomic がその要素を access chain で指しているとき、そのメンバーの型を
// 同じ大きさの配列（ArrayStride は要素の大きさ）に替える。並びは同じなので意味は変わらない。
// ベクトルそのものを指すポインタや、struct を丸ごと読み書きする所があれば触らない
static uint32_t *spv_vecatomic(const uint32_t *w, size_t nw, size_t *n) {
    if (!spvfix_on() || nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3];
    uint32_t *tdef = calloc(bound, sizeof *tdef), *rty = calloc(bound, sizeof *rty), *cval = calloc(bound, sizeof *cval);
    uint32_t *acdef = calloc(bound, sizeof *acdef);
    uint8_t *iscon = calloc(bound, 1), *bad = calloc(bound, 1); // bad：その struct は替えない
    enum { MAXC = 8 };
    struct { uint32_t s, m; } cand[MAXC];
    int nc = 0;
    size_t ann_end = 0;
    uint32_t t_u32 = 0;
    uint32_t *o = NULL;
    if (!tdef || !rty || !cval || !acdef || !iscon || !bad) goto done;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) goto done;
        const uint32_t *x = &w[i];
        if (op >= 19 && op <= 39 && x[1] < bound) tdef[x[1]] = (uint32_t) i;
        if (op == 21 && wc == 4 && x[2] == 32 && x[3] == 0 && !t_u32) t_u32 = x[1];
        if (op == 43 && wc == 4 && x[2] < bound) { cval[x[2]] = x[3]; iscon[x[2]] = 1; }
        if ((op == 59 || op == 61 || op == 65 || op == 66 || op == 120 || op == 55 || op == 83 || op == 124 || op == 245 || op == 169) && wc >= 3 && x[2] < bound) rty[x[2]] = x[1];
        if ((op == 65 || op == 66) && wc >= 4 && x[2] < bound) acdef[x[2]] = (uint32_t) i;
        if ((op == 61 || op == 80) && wc >= 3 && x[1] < bound) bad[x[1]] = 1; // struct を丸ごと読む・作る
        if (op == 71 || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632) ann_end = i + wc;
        i += wc;
    }
    if (!ann_end) goto done;
    // access chain を型でたどる。最後に着いた所が「struct のメンバーのベクトルそのもの」なら、その struct を bad に。
    // atomic のポインタなら、「struct のメンバーのベクトルの要素」かを見て候補にする
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        uint32_t aptr = 0;
        if (op == 228 && wc >= 2) aptr = x[1];
        else if (((op >= 227 && op <= 242) || op == 5614 || op == 5615 || op == 6035) && wc >= 4) aptr = x[3];
        if ((op == 65 || op == 66) && wc >= 4) {
            uint32_t bt = x[3] < bound ? rty[x[3]] : 0, t = 0;
            if (bt && bt < bound && tdef[bt] && (w[tdef[bt]] & 0xffff) == 32) t = w[tdef[bt] + 3];
            uint32_t s = 0, m = 0;
            for (uint32_t k = 4; k < wc && t && t < bound && tdef[t]; k++) {
                const uint32_t *td = &w[tdef[t]];
                uint32_t top = td[0] & 0xffff;
                if (top == 30) {
                    if (!iscon[x[k]] || cval[x[k]] + 2 >= (td[0] >> 16)) { t = 0; break; }
                    s = t; m = cval[x[k]];
                    t = td[2 + m];
                } else if (top == 28 || top == 29 || top == 23) { s = 0; t = td[2]; }
                else { t = 0; break; }
            }
            // 最後に着いた型がベクトルで、その一つ手前が struct のメンバー＝ベクトルそのものを指す
            if (s && t && t < bound && tdef[t] && (w[tdef[t]] & 0xffff) == 23) bad[s] = 1;
        }
        if (aptr && aptr < bound && acdef[aptr]) {
            const uint32_t *ac = &w[acdef[aptr]];
            uint32_t acw = ac[0] >> 16, bt = ac[3] < bound ? rty[ac[3]] : 0, t = 0;
            if (bt && bt < bound && tdef[bt] && (w[tdef[bt]] & 0xffff) == 32) t = w[tdef[bt] + 3];
            for (uint32_t k = 4; k < acw && t && t < bound && tdef[t]; k++) {
                const uint32_t *td = &w[tdef[t]];
                uint32_t top = td[0] & 0xffff;
                if (top == 30) {
                    if (!iscon[ac[k]] || cval[ac[k]] + 2 >= (td[0] >> 16)) break;
                    uint32_t mt = td[2 + cval[ac[k]]];
                    // struct のメンバーがベクトルで、その要素で終わる（k の次が最後の添字）
                    if (k + 2 == acw && mt < bound && tdef[mt] && (w[tdef[mt]] & 0xffff) == 23) {
                        int dup = 0;
                        for (int c = 0; c < nc; c++) dup |= cand[c].s == t && cand[c].m == cval[ac[k]];
                        if (!dup && nc < MAXC) { cand[nc].s = t; cand[nc].m = cval[ac[k]]; nc++; }
                        break;
                    }
                    t = mt;
                } else if (top == 28 || top == 29) t = td[2];
                else break;
            }
        }
        i += wc;
    }
    int nok = 0;
    for (int c = 0; c < nc; c++) nok += !bad[cand[c].s];
    if (!nok) goto done;
    // 足す物：（無ければ）uint、候補ごとに 長さの定数・配列の型・ArrayStride の印
    o = malloc((nw + 4 + nok * 12) * sizeof *o);
    if (!o) goto done;
    uint32_t nb = bound, new_u32 = 0;
    if (!t_u32) t_u32 = new_u32 = nb++;
    uint32_t arr[MAXC] = { 0 }, len[MAXC] = { 0 };
    for (int c = 0; c < nc; c++) if (!bad[cand[c].s]) { len[c] = nb++; arr[c] = nb++; }
    memcpy(o, w, 5 * sizeof *o);
    o[3] = nb;
    size_t k = 5;
    int first_type_done = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (i == ann_end) // 印：配列の要素の間
            for (int c = 0; c < nc; c++) {
                if (bad[cand[c].s]) continue;
                const uint32_t *st = &w[tdef[cand[c].s]];
                uint32_t vt = st[2 + cand[c].m], ct = w[tdef[vt] + 2];
                uint32_t bytes = tdef[ct] ? w[tdef[ct] + 2] / 8 : 4;
                o[k++] = (4u << 16) | 71; o[k++] = arr[c]; o[k++] = 6; o[k++] = bytes ? bytes : 4; // ArrayStride
            }
        if (op >= 19 && op <= 39 && !first_type_done && new_u32) { // uint が無ければ最初の型の前に
            o[k++] = (4u << 16) | 21; o[k++] = new_u32; o[k++] = 32; o[k++] = 0;
            first_type_done = 1;
        }
        for (int c = 0; c < nc; c++) {
            if (bad[cand[c].s] || i != tdef[cand[c].s]) continue;
            // その struct の直前に、長さの定数と配列の型
            const uint32_t *st = &w[i];
            uint32_t vt = st[2 + cand[c].m], ct = w[tdef[vt] + 2], cnt = w[tdef[vt] + 3];
            o[k++] = (4u << 16) | 43; o[k++] = t_u32; o[k++] = len[c]; o[k++] = cnt;
            o[k++] = (4u << 16) | 28; o[k++] = arr[c]; o[k++] = ct; o[k++] = len[c];
        }
        memcpy(o + k, w + i, wc * sizeof *o);
        if (op == 30)
            for (int c = 0; c < nc; c++)
                if (!bad[cand[c].s] && i == tdef[cand[c].s]) o[k + 2 + cand[c].m] = arr[c];
        k += wc;
        i += wc;
    }
    *n = k;
done:
    free(tdef); free(rty); free(cval); free(acdef); free(iscon); free(bad);
    if (o && !*n) { free(o); o = NULL; }
    return o;
}

// sparse（S1）：シェーダーの residency の問い合わせを、ふつうの読みと「いつもある」に直す。iPhone の GPU（MoltenVK）は
// SparseResidency の capability を持たないので、残すとシェーダーが組めない。
//   %r = OpImageSparseXxx %S args…  →  %t = OpImageXxx %T args…  ;  %r = OpCompositeConstruct %S %0 %t
//   （%S は { int 状態, %T 値 } の struct。状態は 0）
//   %b = OpImageSparseTexelsResident %bool %code  →  %b = OpCopyObject %bool %true
// OpCapability SparseResidency は外す。直したら新しく確保した並びを返す（*n に語の数）。直す所が無ければ NULL
static uint32_t sparse_plain_op(uint32_t op) {
    // OpImageSparseSampleImplicitLod（305）〜 OpImageSparseDrefGather（315）は、OpImageSampleImplicitLod（87）〜 OpImageDrefGather（97）と同じ並び
    if (op >= 305 && op <= 315) return op - 305 + 87;
    if (op == 320) return 98; // OpImageSparseRead → OpImageRead
    return 0;
}
static uint32_t *spv_sparse(const uint32_t *w, size_t nw, size_t *n) {
    if (!sparse_on() || nw < 5 || w[0] != 0x07230203) return NULL;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return NULL;
    uint32_t *tdef = calloc(bound, sizeof *tdef), *zero = calloc(bound, sizeof *zero), *tru = calloc(bound, sizeof *tru);
    uint8_t *newc = calloc(bound, 1); // その型の 0・true の定数を新しく足す
    uint32_t *o = NULL;
    size_t nsp = 0, ncap = 0, nres = 0, first_func = 0;
    if (!tdef || !zero || !tru || !newc) goto done;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) goto done; // 壊れている：触らない
        const uint32_t *x = &w[i];
        if (op == 17 && wc == 2 && x[1] == 41) ncap++; // OpCapability SparseResidency
        else if (op >= 19 && op <= 39 && wc >= 2 && x[1] < bound) tdef[x[1]] = (uint32_t) i;
        else if (op == 43 && wc == 4 && x[1] < bound && x[2] < bound && x[3] == 0 && tdef[x[1]] && (w[tdef[x[1]]] & 0xffff) == 21 && !zero[x[1]])
            zero[x[1]] = x[2]; // 32 ビットの整数の 0
        else if (op == 41 && wc == 3 && x[1] < bound && x[2] < bound && !tru[x[1]]) tru[x[1]] = x[2]; // OpConstantTrue
        else if (sparse_plain_op(op)) {
            // 結果の型は 2 つのメンバーの struct で、1 つ目は 32 ビットの整数
            if (wc < 5 || x[1] >= bound || !tdef[x[1]]) goto done;
            const uint32_t *st = &w[tdef[x[1]]];
            if ((st[0] & 0xffff) != 30 || (st[0] >> 16) != 4 || st[2] >= bound || !tdef[st[2]] || (w[tdef[st[2]]] & 0xffff) != 21 || w[tdef[st[2]] + 2] != 32) goto done;
            nsp++;
        } else if (op == 316) { // OpImageSparseTexelsResident
            if (wc != 4 || x[1] >= bound) goto done;
            nres++;
        }
        if (op == 54 && !first_func) first_func = i;
        i += wc;
    }
    if (!nsp && !nres && !ncap) goto done;
    if ((nsp || nres) && !first_func) goto done;
    // 足す定数と、読みの値を受ける番号
    uint32_t nb = bound;
    size_t nconst_words = 0;
    for (size_t i = 5; i < nw; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        const uint32_t *x = &w[i];
        if (sparse_plain_op(op)) {
            uint32_t it = w[tdef[x[1]] + 2];
            if (!zero[it]) { zero[it] = nb++; newc[it] |= 1; nconst_words += 4; }
        } else if (op == 316 && !tru[x[1]]) { tru[x[1]] = nb++; newc[x[1]] |= 2; nconst_words += 3; }
    }
    o = malloc((nw + nconst_words + nsp * 5) * sizeof *o);
    if (!o) goto done;
    memcpy(o, w, 5 * sizeof *o);
    size_t k = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        const uint32_t *x = &w[i];
        if (i == first_func) // 型と定数は関数の前に
            for (uint32_t t = 0; t < bound; t++) {
                if (newc[t] & 1) { o[k++] = (4u << 16) | 43; o[k++] = t; o[k++] = zero[t]; o[k++] = 0; }
                if (newc[t] & 2) { o[k++] = (3u << 16) | 41; o[k++] = t; o[k++] = tru[t]; }
            }
        if (op == 17 && wc == 2 && x[1] == 41) { i += wc; continue; }
        uint32_t plain = sparse_plain_op(op);
        if (plain) {
            const uint32_t *st = &w[tdef[x[1]]];
            uint32_t tmp = nb++;
            memcpy(o + k, x, wc * sizeof *o);
            o[k] = (wc << 16) | plain;
            o[k + 1] = st[3]; // 値の型
            o[k + 2] = tmp;
            k += wc;
            o[k++] = (5u << 16) | 80; o[k++] = x[1]; o[k++] = x[2]; o[k++] = zero[st[2]]; o[k++] = tmp; // OpCompositeConstruct
        } else if (op == 316) {
            o[k++] = (4u << 16) | 83; o[k++] = x[1]; o[k++] = x[2]; o[k++] = tru[x[1]]; // OpCopyObject %bool %r %true
        } else {
            memcpy(o + k, x, wc * sizeof *o);
            k += wc;
        }
        i += wc;
    }
    o[3] = nb;
    *n = k;
    {
        static volatile LONG cnt;
        LONG c = InterlockedIncrement(&cnt);
        if (c <= 3 || (c & (c - 1)) == 0)
            say("シェーダーの sparse の読み %zu か所・residency の問い合わせ %zu か所を、ふつうの読みと「いつもある」に直した（%ld 本目）", nsp, nres, c);
    }
done:
    free(tdef); free(zero); free(tru); free(newc);
    if (o && !*n) { free(o); o = NULL; }
    return o;
}

static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void) a;
    size_t nfix = 0;
    uint32_t *fix = spv_fix(ci->pCode, ci->codeSize / 4, &nfix);
    {
        size_t n2 = 0;
        uint32_t *fix2 = spv_vecatomic(fix ? fix : ci->pCode, fix ? nfix : ci->codeSize / 4, &n2);
        if (fix2) { free(fix); fix = fix2; nfix = n2; }
    }
    {
        size_t n3 = 0;
        uint32_t *fix3 = spv_sparse(fix ? fix : ci->pCode, fix ? nfix : ci->codeSize / 4, &n3);
        if (fix3) { free(fix); fix = fix3; nfix = n3; }
    }
    const uint32_t *code = fix ? fix : ci->pCode;
    size_t size = fix ? nfix * 4 : ci->codeSize;
    struct wrinfo *wi = calloc(1, sizeof *wi);
    wr_scan(code, size / 4, wi);
    uint64_t id = new_id();
    // 調べ用：IPHONE_VK_PSB_DBG=1 で、アドレスで読み書きするシェーダーの印を記録に出す（u1）
    {
        static int dbg = -1;
        if (dbg < 0) { const char *e = getenv("IPHONE_VK_PSB_DBG"); dbg = e && *e == '1'; }
        if (dbg && wi && wi->psb)
            say("アドレスで読み書きするシェーダー %llu：印 %d・書く欄 %016llx・読む欄 %016llx", (unsigned long long) id, wi->psb,
                (unsigned long long) wi->pcw, (unsigned long long) wi->pcr);
    }
    wrm_put(id, wi);
    // 調べ用：IPHONE_VK_SPV_DUMP=フォルダ で、受け手に送るシェーダーを番号ごとに書き出す
    const char *dump = getenv("IPHONE_VK_SPV_DUMP");
    if (dump && *dump) {
        char p[1024];
        snprintf(p, sizeof p, "%s" PATH_SEP "%llu%s.spv", dump, (unsigned long long) id, fix ? "_fix" : "");
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(code, 1, size, f); fclose(f); }
    }
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_bytes(&b, code, (uint32_t) size);
    rpc_send(VR_CREATE_SHADER_MODULE, &b);
    free(b.p);
    gs_keep(id, code, size);
    vf_keep(id, code, size);
    free(fix);
    *out = (VkShaderModule) id;
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
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a) { gs_forget_pipe(VK_NULL_HANDLE, ID(h)); vf_forget(ID(h)); wrm_del(ID(h)); destroy_shader_rpc(dv, h, a); }
DESTROY_SIMPLE(destroy_playout_rpc, VkPipelineLayout, VR_DESTROY_PIPELINE_LAYOUT)
static void pl_forget(uint64_t id);
static void VKAPI_CALL destroy_playout(VkDevice dv, VkPipelineLayout h, const VkAllocationCallbacks *a) { gs_forget_layout(dv, ID(h)); pl_forget(ID(h)); destroy_playout_rpc(dv, h, a); }
DESTROY_SIMPLE(destroy_pipeline_rpc, VkPipeline, VR_DESTROY_PIPELINE)
static void VKAPI_CALL destroy_pipeline(VkDevice dv, VkPipeline h, const VkAllocationCallbacks *a) { gs_forget_pipe(dv, ID(h)); wrm_del(ID(h)); destroy_pipeline_rpc(dv, h, a); }

// pipeline の 1 つの段のシェーダーが書く場所を u に足す（module が分からなければ「全部書く」）
static void wri_stage(const VkPipelineShaderStageCreateInfo *st, struct wrinfo *u) {
    if (st->module) {
        AcquireSRWLockShared(&g_wrm_lock);
        wri_merge(u, wrm_find(ID(st->module)));
        ReleaseSRWLockShared(&g_wrm_lock);
        return;
    }
    for (const VkBaseInStructure *x = st->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *mi = (const VkShaderModuleCreateInfo *) x;
            struct wrinfo t = { 0 };
            wr_scan(mi->pCode, mi->codeSize / 4, &t);
            wri_merge(u, &t);
            free(t.sb);
            return;
        }
    u->all = 1;
}

static void wr_pipes(uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *out) {
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        struct wrinfo *u = calloc(1, sizeof *u);
        if (!u) continue;
        if (!ci[i].stageCount) u->all = 1; // pipeline library でつなぐ物は分からない
        for (uint32_t k = 0; k < ci[i].stageCount; k++) wri_stage(&ci[i].pStages[k], u);
        wrm_put(ID(out[i]), u);
    }
}

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
    // binding ごとの flags（descriptor indexing：update after bind・partially bound・数を確保のときに決める）
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *bfl = NULL;
    for (const VkBaseInStructure *x = ci->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) bfl = (const void *) x;
    // update after bind の印（結んだあとに書いた descriptor を、submit のときに見直す。u1）
    if (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) l->uab = 1;
    for (uint32_t i = 0; bfl && i < bfl->bindingCount && i < ci->bindingCount; i++)
        if (bfl->pBindingFlags && (bfl->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)) l->uab = 1;
    if (bfl && (bfl->bindingCount != ci->bindingCount || !vkd3d())) bfl = NULL; // DXVK・zink には今まで通り運ばない
    for (uint32_t i = 0; bfl && i < ci->bindingCount; i++)
        if (bfl->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) {
            l->var = 1;
            l->var_type = ci->pBindings[i].descriptorType;
            l->var_max = ci->pBindings[i].descriptorCount;
        }
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
    // 受け手が読めるときだけ、末尾に flags を付ける（古い受け手は flags の無い形を作る）
    if (bfl && (g_hw.caps & VR_CAP_DESC_FLAGS)) {
        vr_u32(&b, ci->bindingCount);
        for (uint32_t i = 0; i < ci->bindingCount; i++) vr_u32(&b, bfl->pBindingFlags[i]);
    } else if (bfl && l->var) {
        static LONG once;
        if (!InterlockedExchange(&once, 1)) say("受け手が binding の flags を読めない（組み直しが要る）。数を確保のときに決める set は取れないかもしれない");
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
    *out = (VkPipelineLayout) id;
    return VK_SUCCESS;
}

// パイプラインの置き場（受け手には送らない。中身は空のまま）
static VkResult VKAPI_CALL create_pcache(VkDevice dv, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void) dv; (void) ci; (void) a;
    *out = (VkPipelineCache) new_id();
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_pcache(VkDevice dv, VkPipelineCache c, const VkAllocationCallbacks *a) { (void) dv; (void) c; (void) a; }
// 中身は持たないが、決まりどおりの頭（32 バイト）だけは返す。空で返すと Godot などが誤りとみなす
static VkResult VKAPI_CALL pcache_data(VkDevice dv, VkPipelineCache c, size_t *n, void *p) {
    (void) dv; (void) c;
    VkPipelineCacheHeaderVersionOne h = { .headerSize = sizeof h, .headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
                                          .vendorID = g_hw.props.vendorID, .deviceID = g_hw.props.deviceID };
    memcpy(h.pipelineCacheUUID, g_hw.props.pipelineCacheUUID, VK_UUID_SIZE);
    if (!p) { *n = sizeof h; return VK_SUCCESS; }
    if (*n < sizeof h) { *n = 0; return VK_INCOMPLETE; }
    memcpy(p, &h, sizeof h);
    *n = sizeof h;
    return VK_SUCCESS;
}
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
        out[i] = (VkPipeline) id;
        struct wrinfo *u = calloc(1, sizeof *u);
        if (u) wri_stage(&ci[i].stage, u);
        wrm_put(id, u);
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
    for (struct dset *s = p->head, *nx; s; s = nx) { nx = s->next; dsl_unref(s->layout); free(s->e); free(s->hx); free(s); }
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
    // 数を確保のときに決める set（VkDescriptorSetVariableDescriptorCountAllocateInfo）
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vc = NULL;
    for (const VkBaseInStructure *x = ai->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) vc = (const void *) x;
    if (vc && (vc->descriptorSetCount != ai->descriptorSetCount || !vkd3d())) vc = NULL;
    // pool の残りが足りなければ、受け手に頼まずに「足りない」と返す（DXVK などは、それを見て新しい pool を作る）
    uint32_t need[16] = { 0 };
    int short_of = 0;
    AcquireSRWLockExclusive(&g_pools_lock);
    if (p->used_sets + ai->descriptorSetCount > p->max_sets) short_of = 1;
    for (uint32_t i = 0; i < ai->descriptorSetCount && !short_of; i++) {
        const struct dsl *l = (const struct dsl *) ai->pSetLayouts[i];
        for (uint32_t k = 0; k < l->nt; k++) {
            uint32_t cnt = dsl_type_count(l, k, vc ? vc->pDescriptorCounts[i] : 0);
            if (!cnt) continue;
            int j = pool_slot(p, l->t[k].type);
            if (j < 0) { short_of = 1; break; }
            need[j] += cnt;
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
        s->var_count = vc ? vc->pDescriptorCounts[i] : 0;
        InterlockedIncrement(&s->layout->refs);
        AcquireSRWLockExclusive(&g_pools_lock);
        s->next = p->head;
        p->head = s;
        ReleaseSRWLockExclusive(&g_pools_lock);
        out[i] = (VkDescriptorSet) s;
        vr_u64(&b, ID(ai->pSetLayouts[i]));
        vr_u64(&b, ID(s));
    }
    // 数を確保のときに決める set の数は、受け手が読めるときだけ末尾に付ける
    if (vc && (g_hw.caps & VR_CAP_DESC_FLAGS))
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++) vr_u32(&b, vc->pDescriptorCounts[i]);
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
                uint32_t cnt = dsl_type_count(s->layout, k, s->var_count);
                if (j >= 0) p->sz[j].used = p->sz[j].used > cnt ? p->sz[j].used - cnt : 0;
            }
            dsl_unref(s->layout);
        }
        free(s->e);
        free(s->hx);
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

// (binding, 番号) の入る表の場所
static uint32_t *dset_slot(const struct dset *s, uint32_t binding, uint32_t elem) {
    uint32_t h = (binding * 0x9E3779B1u) ^ (elem * 0x85EBCA77u);
    h ^= h >> 15;
    for (uint32_t i = h & (s->hcap - 1);; i = (i + 1) & (s->hcap - 1)) {
        uint32_t k = s->hx[i];
        if (!k || (s->e[k - 1].binding == binding && s->e[k - 1].elem == elem)) return &s->hx[i];
    }
}

static int dset_rehash(struct dset *s, uint32_t cap) {
    uint32_t *nh = calloc(cap, sizeof *nh);
    if (!nh) return 0;
    free(s->hx);
    s->hx = nh;
    s->hcap = cap;
    for (uint32_t i = 0; i < s->n; i++) *dset_slot(s, s->e[i].binding, s->e[i].elem) = i + 1;
    return 1;
}

static void dset_put_full(struct dset *s, struct dent d) {
    if ((!s->hx || (s->n + 1) * 2 > s->hcap) && !dset_rehash(s, s->hcap ? s->hcap * 2 : 16)) return;
    // update after bind の set：書いた順の番号を付ける（submit のとき、結んだあとに書かれた物だけを足す）
    if (s->layout && s->layout->uab) s->gen = d.wg = (uint64_t) InterlockedIncrement64(&g_dgen);
    uint32_t *slot = dset_slot(s, d.binding, d.elem);
    if (*slot) { s->e[*slot - 1] = d; return; }
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2 : 8;
        struct dent *ne = realloc(s->e, nc * sizeof *s->e);
        if (!ne) return;
        s->e = ne;
        s->cap = nc;
    }
    s->e[s->n++] = d;
    *slot = s->n;
}

static void dset_put(struct dset *s, uint32_t binding, uint32_t elem, uint32_t type, struct buf *bf, uint64_t off, uint64_t range) {
    dset_put_full(s, (struct dent) { binding, elem, type, bf, off, range, 0, 0, 0, 0 });
}

static const struct dent *dset_get(const struct dset *s, uint32_t binding, uint32_t elem) {
    if (!s->hx) return NULL;
    uint32_t k = *dset_slot(s, binding, elem);
    return k ? &s->e[k - 1] : NULL;
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
                ii[n] = (VkDescriptorImageInfo) { (VkSampler) e->h0, (VkImageView) e->h1, (VkImageLayout) e->layout };
                w[n].pImageInfo = &ii[n];
            } else if (is_texel_desc(e->type)) {
                tv[n] = (VkBufferView) e->h0;
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

// 空の descriptor を書いたら、種類ごとに 1 回だけ記録に出す（iPhone の GPU は空の descriptor を持たないので、受け手が代わりの物を入れる）
static void note_null_desc(uint32_t type) {
    static volatile LONG seen;
    if (type >= 32 || !vkd3d()) return;
    LONG bit = (LONG) 1 << type;
    if (!(InterlockedOr(&seen, bit) & bit)) say("空の descriptor を書いた（種類 %u）", type);
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
                if (w[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER && !view) note_null_desc(w[i].descriptorType);
                vr_u64(&b, smp);
                vr_u64(&b, view);
                vr_u32(&b, fix_layout(ii->imageLayout));
                dset_put_full(s, (struct dent) { w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, NULL, 0, 0, smp, view, (uint32_t) ii->imageLayout, 0 });
            }
            continue;
        }
        if (is_texel_desc(w[i].descriptorType)) {
            for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
                struct bview *v = (struct bview *) w[i].pTexelBufferView[k];
                if (!v) note_null_desc(w[i].descriptorType);
                vr_u64(&b, ID(v));
                if (v) dset_put_full(s, (struct dent) { w[i].dstBinding, w[i].dstArrayElement + k, w[i].descriptorType, v->b, v->off, v->range, ID(v), 0, 0, 0 });
            }
            continue;
        }
        for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
            const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[k];
            struct buf *bf = (struct buf *) bi->buffer;
            uint64_t range = bi->range == VK_WHOLE_SIZE && bf ? bf->size - bi->offset : bi->range;
            if (!bf) note_null_desc(w[i].descriptorType);
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

// 装置を消したら、その装置の隠しの pool を忘れる。次の装置が同じ番地に作られると、消えた pool を使い回して
// 受け手で set が取れず、そのあとの書き換えで落ちていた（CTS の push_descriptor を続けて流したときだけ）
static void push_forget_dev(VkDevice dv) {
    AcquireSRWLockExclusive(&g_push_lock);
    if (g_push_dev == dv) { g_push_pool = VK_NULL_HANDLE; g_push_dev = VK_NULL_HANDLE; }
    ReleaseSRWLockExclusive(&g_push_lock);
}

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

static void cb_clear(struct cb *c) { push_release(c); c->s.n = 0; c->nlse = 0; c->nr = 0; c->nrd = 0; c->nev = 0; c->nwc = 0; c->nbpl = 0; c->bpl_over = 0; c->qcopy_nowait = 0; c->nqw = 0; c->npv = 0; c->nub = 0; c->psb_done = c->psb_any = c->psb_whole = 0; bct_free(c); c->gs = NULL; c->app_topo_set = 0; gs_cb_clear(c); }

static void cb_free(struct cb *c) { push_release(c); free(c->lse); free(c->ph); free(c->wc); free(c->pv); free(c->ub); gs_cb_free(c); bct_free(c); free(c->s.p); free(c->r); free(c->rd); free(c->ev); free(c); }

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
    for (uint32_t i = 0; i < n; i++) if (cbs[i]) vr_u64(&b, ID(cbs[i]));
    // 番号は入れ物の番地なので、受け手に捨てると送ってから入れ物を解放する。先に解放すると、別の糸がすぐ同じ番地で
    // 新しい command buffer を作って先に受け手に届き、受け手が新しい方を古い pool から捨ててしまう（10/7 SuperTuxKart の 22 本の糸で）
    rpc_send(VR_FREE_COMMAND_BUFFERS, &b);
    free(b.p);
    for (uint32_t i = 0; i < n; i++) {
        if (!cbs[i]) continue;
        struct cb *c = (struct cb *) cbs[i], **pp2 = &p->head;
        while (*pp2 && *pp2 != c) pp2 = &(*pp2)->next;
        if (*pp2) *pp2 = c->next;
        cb_free(c);
    }
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
    if (c->level != VK_COMMAND_BUFFER_LEVEL_SECONDARY && ls_on()) { double ls_t0 = now_ms(); ls_process(c); g_lsst.proc_ms += now_ms() - ls_t0; g_lsst.proc_n++; }
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
static int g_wr_why = 5; // 次の cb_writes の理由（集計のためだけ）
static void cb_writes(struct cb *c, struct buf *bf, uint64_t off, uint64_t len) {
    if (!bf || !bf->m || !bf->m->host) return;
    if (len == VK_WHOLE_SIZE) len = off < bf->size ? bf->size - off : 0;
    int w = g_wr_why;
    g_wr_why = 5;
    InterlockedIncrement64((volatile LONG64 *) &g_stat.wr_n[w]);
    InterlockedAdd64((volatile LONG64 *) &g_stat.wr_bytes[w], (LONG64) len);
    size_t b0 = c->nr;
    rng_add(&c->r, &c->nr, &c->capr, bf->m, bf->memoff + off, len, 0);
    if (c->nr > b0) c->r[b0].lazy = lz_why_ok(w);
}

// つないだ pipeline の番号を覚える
static void cb_note_pipe(struct cb *c, uint64_t id) {
    if (!id) return;
    for (int i = 0; i < c->nbpl; i++) if (c->bpl[i] == id) return;
    if (c->nbpl < 64) c->bpl[c->nbpl++] = id;
    else c->bpl_over = 1;
}

// CPU から見えるメモリに置いた画像（linear）へ GPU が書くとき、その画像のメモリを読み戻す範囲に入れる。
// これまでは buffer にしか付けず、画像に写して CPU で読むアプリ（CTS の draw など）は 0 を読んでいた
static void cb_img_writes(struct cb *c, VkImage h) {
    struct img *im = (struct img *) h;
    if (!im || !im->m || !im->m->host) return;
    rng_add(&c->r, &c->nr, &c->capr, im->m, im->memoff, im->req.size, 0);
}

// storage buffer につないだ物を候補として覚える（書く場所かどうかは submit のときに決める）
static void wr_cand(struct cb *c, uint32_t set, uint32_t binding, struct buf *bf, uint64_t off, uint64_t len) {
    if (!bf || !bf->m || !bf->m->host) return;
    if (c->nwc == c->cwc) {
        size_t cap = c->cwc ? c->cwc * 2 : 16;
        struct wrcand *p = realloc(c->wc, cap * sizeof *p);
        if (!p) { cb_writes(c, bf, off, len); return; }
        c->wc = p;
        c->cwc = cap;
    }
    c->wc[c->nwc++] = (struct wrcand) { bf, off, len, set, binding, 0 };
}

// submit のとき：作られたシェーダーのどれかが書く場所なら、GPU が書くかもしれない範囲にする
// この command buffer でつないだ pipeline のどれかが、この場所に書くか。IPHONE_VK_RO_PIPE=0 で全部のシェーダーで決める前のやり方
static int wr_may_cb(struct cb *c, uint32_t set, uint32_t binding) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("IPHONE_VK_RO_PIPE"); const char *f = getenv("IPHONE_VK_RO_SKIP"); on = !(e && *e == '0') && !(f && *f == '0'); }
    if (!wr_may(set, binding)) return 0;
    if (!on || c->bpl_over || !c->nbpl) return 1;
    int hit = 0;
    AcquireSRWLockShared(&g_wrm_lock);
    for (int i = 0; i < c->nbpl && !hit; i++) {
        struct wrinfo *w = wrm_find(c->bpl[i]);
        if (!w || w->all || wri_has(w, set, binding)) hit = 1;
    }
    ReleaseSRWLockShared(&g_wrm_lock);
    return hit;
}

static void wr_resolve(struct cb *c) {
    for (size_t k = 0; k < c->nwc; k++)
        if (!c->wc[k].done && wr_may_cb(c, c->wc[k].set, c->wc[k].binding)) {
            g_wr_why = 0;
            cb_writes(c, c->wc[k].b, c->wc[k].off, c->wc[k].len);
            c->wc[k].done = 1;
        }
}

// GPU が読むかもしれない範囲として覚える
static void cb_reads(struct cb *c, struct buf *bf, uint64_t off, uint64_t len) {
    if (!bf || !bf->m || !bf->m->host) return;
    if (len == VK_WHOLE_SIZE) len = off < bf->size ? bf->size - off : 0;
    rng_add(&c->rd, &c->nrd, &c->caprd, bf->m, bf->memoff + off, len, 0);
}

// ---- u1：アドレスで読み書きする buffer と、結んだあとに書いた descriptor ----
// push constant に入っていたアドレスを覚える。同じ buffer・同じ欄なら 1 つにまとめる（始まりは手前の方を取る。輪のように進む upload の置き場で増えないように）
static void pv_add(struct cb *c, struct mem *m, uint64_t off, uint64_t end, uint32_t slot) {
    for (size_t i = c->npv; i-- > 0 && i + 16 >= c->npv;)
        if (c->pv[i].m == m && c->pv[i].end == end && c->pv[i].slot == slot) {
            if (off < c->pv[i].off) c->pv[i].off = off;
            return;
        }
    if (c->npv == c->cpv) {
        size_t cap = c->cpv ? c->cpv * 2 : 8;
        struct pva *p = realloc(c->pv, cap * sizeof *p);
        if (!p) return;
        c->pv = p;
        c->cpv = cap;
    }
    c->pv[c->npv++] = (struct pva) { m, off, end, slot };
}

// vkCmdPushConstants の中身から、表にあるアドレス（CPU から見えるメモリの buffer）を探す。8 バイトの値を 4 バイトずつずらして見る
static void pv_scan(struct cb *c, uint32_t off, uint32_t size, const void *v) {
    if (!g_bda_host || !g_nvab || size < 8 || !bda_on()) return;
    const uint8_t *p = v;
    for (uint32_t k = (4 - off % 4) % 4; k + 8 <= size; k += 4) {
        uint64_t x;
        memcpy(&x, p + k, 8);
        if (x < g_vab_lo || x >= g_vab_hi) continue;
        struct mem *m;
        uint64_t mo, me;
        if (vab_find(x, &m, &mo, &me)) { pv_add(c, m, mo, me, (off + k) / 4); InterlockedIncrement64(&g_u1.pv); }
    }
}

// update after bind の set を結んだ：結んだ時点の通し番号を覚える（同じ set を前に結んでいれば、前の番号のまま）
static void ub_note(struct cb *c, struct dset *s, uint32_t set, uint64_t gen) {
    if (!s || !uab_on()) return;
    for (size_t i = 0; i < c->nub; i++)
        if (c->ub[i].s == s && c->ub[i].set == set) {
            if (gen < c->ub[i].gen) c->ub[i].gen = gen;
            return;
        }
    if (c->nub == c->cub) {
        size_t cap = c->cub ? c->cub * 2 : 4;
        struct uabref *p = realloc(c->ub, cap * sizeof *p);
        if (!p) return;
        c->ub = p;
        c->cub = cap;
    }
    c->ub[c->nub++] = (struct uabref) { s, gen, set };
}

// set の 1 つの descriptor が指す buffer を、GPU が読む範囲・書くかもしれない候補にする（cmd_bind_sets と同じ決め方）
static void dent_track(struct cb *c, uint32_t set, const struct dent *e) {
    if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || e->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) wr_cand(c, set, e->binding, e->b, e->off, e->range);
    else if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC && e->b) wr_cand(c, set, e->binding, e->b, 0, e->b->size);
    if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || e->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) cb_reads(c, e->b, e->off, e->range);
    else if (e->b) cb_reads(c, e->b, 0, e->b->size);
}

// submit のとき：結んだあとに書かれた descriptor の buffer を足す（uab_tracking.md の筋書き 1）。
// 受け手の帳簿（結んだ時点の set の中身）には載らないので、そのメモリには「ドライバの番号まで待つ」印を付ける
static void uab_resolve(struct cb *c) {
    for (size_t i = 0; i < c->nub; i++) {
        struct uabref *u = &c->ub[i];
        struct dset *s = u->s;
        uint64_t g = s->gen;
        if (g <= u->gen) continue;
        InterlockedIncrement64(&g_u1.ub_sets);
        for (uint32_t k = 0; k < s->n; k++) {
            const struct dent *e = &s->e[k];
            if (e->wg <= u->gen || !e->b) continue;
            dent_track(c, u->set, e);
            if (e->b->m && e->b->m->host) e->b->m->wide = 1;
            InterlockedIncrement64(&g_u1.ub_ents);
        }
        u->gen = g;
    }
}

// この command buffer でつないだ pipeline のシェーダーが、アドレスで読み書きするか（psb の印）と、そのアドレスの入る push constant の欄
static int cb_psb(struct cb *c, uint64_t *pr, uint64_t *pw) {
    int f = 0;
    *pr = *pw = 0;
    if (c->bpl_over) { *pr = *pw = ~0ull; return 7; }
    AcquireSRWLockShared(&g_wrm_lock);
    for (int i = 0; i < c->nbpl; i++) {
        const struct wrinfo *w = wrm_find(c->bpl[i]);
        if (!w || w->all) { f = 7; *pr = *pw = ~0ull; break; } // 分からない pipeline：アドレスで何でも読み書きするとみなす
        f |= w->psb;
        *pr |= w->pcr;
        *pw |= w->pcw;
    }
    ReleaseSRWLockShared(&g_wrm_lock);
    return f;
}

// submit のとき（記録し直すまでに 1 回）：push constant に入っていたアドレスのうち、シェーダーが読む欄の物は GPU が読む範囲（rd）に、
// 書く欄の物は書くかもしれない範囲（r。CPU が触るまで読み戻しを遅らせる）に足す。どちらも、そのアドレスから buffer の終わりまで
// （D3D12 のリソースの大きさはここからは分からない）。rd・r に入れれば、送る・帳簿・引き取り・合図のときの送り直し（pb_release）は今までの道で済む。
// 出どころの分からないアドレスで読む・書くシェーダーがあれば psb_whole を立てる（queue_submit が、アドレスで使うメモリを全部見る）
static void psb_collect(struct cb *c) {
    if (c->psb_done || !g_bda_host || !bda_on()) return;
    c->psb_done = 1;
    uint64_t pr, pw;
    int f = cb_psb(c, &pr, &pw);
    if (!(f & 1)) return;
    c->psb_any = 1;
    c->psb_whole = (f & 6) != 0;
    for (size_t k = 0; k < c->npv; k++) {
        const struct pva *v = &c->pv[k];
        uint64_t bit = 1ull << (v->slot < 63 ? v->slot : 63);
        // 出どころの分からないアドレス（メモリから読んだアドレス）への書き込みは、push constant のアドレスとは別の所なので、ここでは足さない
        int wr = (pw & bit) != 0, rd = wr || (pr & bit);
        if (!rd) continue;
        rng_add(&c->rd, &c->nrd, &c->caprd, v->m, v->off, v->end - v->off, 0);
        if (wr) {
            size_t b0 = c->nr;
            rng_add(&c->r, &c->nr, &c->capr, v->m, v->off, v->end - v->off, 0);
            if (c->nr > b0) c->r[b0].lazy = lz_why_ok(0); // storage buffer と同じく、CPU が触るまで読み戻しを遅らせる
            InterlockedIncrement64(&g_u1.wr_n);
        }
        InterlockedIncrement64(&g_u1.rd_n);
        v->m->wide = 1;
    }
}

static void VKAPI_CALL cmd_bind_pipeline(VkCommandBuffer cbh, VkPipelineBindPoint bp, VkPipeline p) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_PIPELINE);
    vr_u32(&c->s, bp);
    vr_u64(&c->s, ID(p));
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) { ls_ev(c, LE_PIPE, ID(p), 0, 0, 0, 0, 0); ls_ev(c, LE_ST, 0, 0, 0, 0, 0, 0); }
    gs_bind(c, bp, p);
    if (c->gs) ls_ev(c, LE_RISK, 0, 0, 0, 0, 0, 0);
    cb_note_pipe(c, ID(p));
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
    if (ls_on()) for (uint32_t i = 0; i < n; i++) { ls_ev(c, LE_BIND, (uint64_t) (uintptr_t) sets[i], 0, 0, 0, 0, 0); ls_ev(c, LE_ST, 0, 0, 1 + (first + i < 8 ? first + i : 8), 0, 0, 0); }
    // storage buffer は GPU が書くかもしれない
    for (uint32_t i = 0; i < n; i++) {
        struct dset *s = (struct dset *) sets[i];
        if (!s) continue;
        uint64_t gen = s->gen; // 見る前に取る（見ている間にほかの糸が書いた物は、submit のときに足す）
        for (uint32_t k = 0; k < s->n; k++) dent_track(c, first + i, &s->e[k]);
        if (s->layout && s->layout->uab) ub_note(c, s, first + i, gen);
    }
}

static void VKAPI_CALL cmd_push(VkCommandBuffer cbh, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_ST, 0, 0, 10, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_PUSH_CONSTANTS);
    vr_u64(&c->s, ID(l));
    vr_u32(&c->s, gs_stages(st));
    vr_u32(&c->s, off);
    vr_bytes(&c->s, v, size);
    pv_scan(c, off, size, v); // root の CBV・SRV・UAV のアドレス（vkd3d-proton）を覚える
}

static void VKAPI_CALL cmd_dispatch(VkCommandBuffer cbh, uint32_t x, uint32_t y, uint32_t z) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_DISP, 0, 0, 0, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_DISPATCH);
    vr_u32(&c->s, x);
    vr_u32(&c->s, y);
    vr_u32(&c->s, z);
}

static void VKAPI_CALL cmd_dispatch_base(VkCommandBuffer cbh, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_DISP, 0, 0, 0, 0, 0, 0);
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
    ls_ev(c, LE_DISP, 0, 0, 0, 0, 0, 0);
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
        g_wr_why = 1;
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
    // VK_WHOLE_SIZE の塗りは、残りを 4 の倍数に切り下げた所までしか書かない（そこから先を読み戻しで消さない）
    struct buf *bf = (struct buf *) dst;
    if (size == VK_WHOLE_SIZE && bf) size = off < bf->size ? (bf->size - off) & ~(VkDeviceSize) 3 : 0;
    g_wr_why = 2;
    cb_writes(c, bf, off, size);
}

static void VKAPI_CALL cmd_update(VkCommandBuffer cbh, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void *data) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_UPDATE_BUFFER);
    vr_u64(&c->s, ID(dst));
    vr_u64(&c->s, off);
    vr_bytes(&c->s, data, (uint32_t) size);
    g_wr_why = 2;
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
    size_t ls_off = c->s.n;
    vr_u32(&c->s, VR_CMD_BEGIN_RENDER_PASS);
    if (g_stats > 0) {
        rpl_count(ID(bi->renderPass), &bi->renderArea);
        if (c->rp_last_fb == ID(bi->framebuffer)) g_stat.rp_same++;
        c->rp_last_fb = ID(bi->framebuffer);
    }
    vr_u64(&c->s, ID(bi->renderPass));
    vr_u64(&c->s, ID(bi->framebuffer));
    vr_put(&c->s, &bi->renderArea, sizeof bi->renderArea);
    vr_u32(&c->s, bi->clearValueCount);
    vr_put(&c->s, bi->pClearValues, bi->clearValueCount * sizeof *bi->pClearValues);
    // 2 次の command buffer の中身はここに写して送るので、受け手では常に INLINE
    (void) contents;
    vr_u32(&c->s, VK_SUBPASS_CONTENTS_INLINE);
    ls_ev(c, LE_RPB, ID(bi->renderPass), ID(bi->framebuffer), (uint32_t) ls_off, (uint32_t) c->s.n, 0, 0);
}

static void VKAPI_CALL cmd_end_rp(VkCommandBuffer cbh) { ls_ev((struct cb *) cbh, LE_RPE, 0, 0, (uint32_t) ((struct cb *) cbh)->s.n, 0, 0, 0); vr_u32(&((struct cb *) cbh)->s, VR_CMD_END_RENDER_PASS); }

static void VKAPI_CALL cmd_next_subpass(VkCommandBuffer cbh, VkSubpassContents contents) {
    struct cb *c = (struct cb *) cbh;
    (void) contents;
    ls_ev(c, LE_NEXT, 0, 0, 0, 0, 0, 0);
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
        ls_ev(c, LE_UNK, 0, 0, 0, 0, 0, 0);
        vr_put(&c->s, s->s.p, s->s.n);
        for (size_t k = 0; k < s->nr; k++) rng_add(&c->r, &c->nr, &c->capr, s->r[k].m, s->r[k].off, s->r[k].len, 0);
        for (size_t k = 0; k < s->nrd; k++) rng_add(&c->rd, &c->nrd, &c->caprd, s->rd[k].m, s->rd[k].off, s->rd[k].len, 0);
        for (size_t k = 0; k < s->nev; k++) ev_add(c, s->ev[k].e, s->ev[k].val);
        for (size_t k = 0; k < s->nwc; k++) wr_cand(c, s->wc[k].set, s->wc[k].binding, s->wc[k].b, s->wc[k].off, s->wc[k].len);
        for (int k = 0; k < s->nbpl; k++) cb_note_pipe(c, s->bpl[k]);
        if (s->bpl_over) c->bpl_over = 1;
        for (size_t k = 0; k < s->npv; k++) pv_add(c, s->pv[k].m, s->pv[k].off, s->pv[k].end, s->pv[k].slot);
        for (size_t k = 0; k < s->nub; k++) ub_note(c, s->ub[k].s, s->ub[k].set, s->ub[k].gen);
        if (s->qcopy_nowait) c->qcopy_nowait = 1;
        for (size_t k = 0; k < s->nqw; k++) cb_note_q(c, s->qw[k].qp, s->qw[k].first, s->qw[k].n);
    }
}

static void VKAPI_CALL cmd_draw(VkCommandBuffer cbh, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    if (gs_draw(c, vc, ic, fv, fi)) { ls_ev(c, LE_DRAW, 0, 0, 1, 0, 0, 0); return; }
    ls_ev(c, LE_DRAW, 0, 0, 1, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_DRAW);
    vr_u32(&c->s, vc);
    vr_u32(&c->s, ic);
    vr_u32(&c->s, fv);
    vr_u32(&c->s, fi);
}

static void VKAPI_CALL cmd_draw_indexed(VkCommandBuffer cbh, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_DRAW, 0, 0, 1, 0, 0, 0);
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
    ls_ev(c, LE_DRAW, 0, 0, 1, 0, 0, 0);
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
    ls_ev(c, LE_ST, 0, 0, 11, 0, 0, 0);
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
    ls_ev(c, LE_ST, 0, 0, 12, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_BIND_INDEX_BUFFER);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, off);
    vr_u32(&c->s, t);
    cb_reads(c, (struct buf *) bb, off, VK_WHOLE_SIZE);
    gs_track_ib(c, bb, off, t);
}

static void VKAPI_CALL cmd_set_viewport(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkViewport *v) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_ST, 0, 0, 13, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_SET_VIEWPORT);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_put(&c->s, v, n * sizeof *v);
}

static void VKAPI_CALL cmd_set_scissor(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkRect2D *v) {
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_ST, 0, 0, 14, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_SET_SCISSOR);
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_put(&c->s, v, n * sizeof *v);
}

static void VKAPI_CALL cmd_copy_image_to_buffer(VkCommandBuffer cbh, VkImage im, VkImageLayout l, VkBuffer bb, uint32_t n, const VkBufferImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
    if (im && ((struct img *) im)->bc) { say("BC：ほどいた画像を BC のままバッファへ読み出すことはまだできない（飛ばした）"); return; }
    ls_ev_xfer(c, VK_NULL_HANDLE, im);
    vr_u32(&c->s, VR_CMD_COPY_IMAGE_TO_BUFFER);
    vr_u64(&c->s, ID(im));
    vr_u64(&c->s, ID(bb));
    vr_u32(&c->s, fix_layout(l));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
    g_wr_why = 3;
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
    cb_img_writes(c, im);
    ls_ev_xfer(c, im, VK_NULL_HANDLE);
    struct img *ii = (struct img *) im;
    if (ii && ii->bc) {
        // 全部の範囲をほどいて 1 つの一時バッファに並べ、そこから 1 回で写す（一時バッファを作る往復を 1 回にする）
        const struct buf *src = (const struct buf *) bb;
        if (!src || !src->m || !src->m->shadow) { say("BC：CPU から見えないバッファからは写せない（飛ばした）"); return; }
        lz_settle(src->m, src->memoff, src->size, LZ_OTHER);
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
    for (uint32_t i = 0; i < n; i++) b2i_note(ii ? (uint32_t) ii->ls_fmt : 0, rg[i].imageExtent.width, rg[i].imageExtent.height);
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
    cb_img_writes(c, im);
    {   // 画像全体を消すか：全部の mip と layer を含む範囲が 1 つでもあれば全体
        struct img *ii = (struct img *) im;
        uint32_t whole = 0;
        for (uint32_t k = 0; k < n; k++)
            if (rg[k].baseMipLevel == 0 && (rg[k].levelCount == VK_REMAINING_MIP_LEVELS || rg[k].levelCount >= ii->ls_mips) && rg[k].baseArrayLayer == 0 &&
                (rg[k].layerCount == VK_REMAINING_ARRAY_LAYERS || rg[k].layerCount >= ii->ls_layers) &&
                (rg[k].aspectMask & ls_aspect_of((VkFormat) ii->ls_fmt)) == ls_aspect_of((VkFormat) ii->ls_fmt)) whole = 1;
        ls_ev(c, LE_CLRIMG, ID(im), 0, 1, whole, (uint32_t) c->s.n, n);
    }
    vr_u32(&c->s, VR_CMD_CLEAR_COLOR_IMAGE);
    vr_u64(&c->s, ID(im));
    vr_u32(&c->s, fix_layout(l));
    vr_put(&c->s, col, sizeof *col);
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
}

static void VKAPI_CALL cmd_copy_image(VkCommandBuffer cbh, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
    cb_img_writes(c, d);
    ls_ev_xfer(c, d, s);
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
    cb_img_writes(c, d);
    ls_ev_xfer(c, d, s);
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
    cb_img_writes(c, d);
    ls_ev_xfer(c, d, s);
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
    cb_img_writes(c, im);
    {
        struct img *ii = (struct img *) im;
        uint32_t whole = 0;
        for (uint32_t k = 0; k < n; k++)
            if (rg[k].baseMipLevel == 0 && (rg[k].levelCount == VK_REMAINING_MIP_LEVELS || rg[k].levelCount >= ii->ls_mips) && rg[k].baseArrayLayer == 0 &&
                (rg[k].layerCount == VK_REMAINING_ARRAY_LAYERS || rg[k].layerCount >= ii->ls_layers) &&
                (rg[k].aspectMask & ls_aspect_of((VkFormat) ii->ls_fmt)) == ls_aspect_of((VkFormat) ii->ls_fmt)) whole = 1;
        ls_ev(c, LE_CLRIMG, ID(im), 0, 2, whole, (uint32_t) c->s.n, n);
    }
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
    ls_ev(c, LE_CLRATT, 0, 0, na, nr, (uint32_t) c->s.n, 0);
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
    ls_ev(c, LE_ST, 0, 0, 16 + (uint32_t) st % 40, 0, 0, 0);
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
    ls_ev(c, LE_ST, 0, 0, 11, 0, 0, 0);
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
    vr_u32(s, lso_load((uint32_t) a->loadOp));
    vr_u32(s, lso_store((uint32_t) a->storeOp));
    vr_put(s, &a->clearValue, sizeof a->clearValue);
}

static void VKAPI_CALL cmd_begin_rendering(VkCommandBuffer cbh, const VkRenderingInfo *ri) {
    if (!need_cap(VR_CAP_DYNAMIC_RENDERING, "dynamic rendering")) return;
    struct cb *c = (struct cb *) cbh;
    ls_ev(c, LE_UNK, 0, 0, 0, 0, 0, 0);
    vr_u32(&c->s, VR_CMD_BEGIN_RENDERING);
    if (g_stats > 0) {
        int nl = 0, ns = 0, nc = 0;
        const VkRenderingAttachmentInfo *at[10];
        uint32_t na = 0;
        for (uint32_t i = 0; i < ri->colorAttachmentCount && na < 8; i++) at[na++] = &ri->pColorAttachments[i];
        if (ri->pDepthAttachment) at[na++] = ri->pDepthAttachment;
        if (ri->pStencilAttachment) at[na++] = ri->pStencilAttachment;
        for (uint32_t i = 0; i < na; i++) {
            if (!at[i]->imageView) continue;
            nl += at[i]->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
            nc += at[i]->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
            ns += at[i]->storeOp == VK_ATTACHMENT_STORE_OP_STORE;
        }
        g_stat.rp_begin++;
        g_stat.rp_load += nl;
        g_stat.rp_store += ns;
        g_stat.rp_clear += nc;
        g_stat.rp_px += (double) ri->renderArea.extent.width * ri->renderArea.extent.height * (nl + ns);
    }
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
    if (known && ((struct dev *) dv)->npb) {
        // CPU からの合図を待つ submit の fence なら、合図が来るまで（期限までは）待つ
        struct dev *d = (struct dev *) dv;
        double ms = timeout >= 0x7fffffffffffffffull ? 1e300 : timeout / 1e6, t0 = now_ms();
        if (all) {
            for (uint32_t i = 0; i < n; i++) {
                double left = ms - (now_ms() - t0);
                if (!pb_wait(d, ((struct fence *) fs[i])->q, ((struct fence *) fs[i])->serial, left > 0 ? left : 0)) return VK_TIMEOUT;
            }
        } else {
            // どれか 1 つ：始まれる物が出るまで待ち、それだけを終わったことにする
            for (;;) {
                for (uint32_t i = 0; i < n; i++) {
                    struct fence *fc = (struct fence *) fs[i];
                    if (pb_wait(d, fc->q, fc->serial, 0)) {
                        complete(d, fc->q, fc->serial);
                        d->skipped_wait = 1;
                        g_stat.skipped++;
                        return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
                    }
                }
                if (now_ms() - t0 >= ms) return VK_TIMEOUT;
                Sleep(1);
            }
        }
    }
    if (known && real_fence()) {
        // 受け手の本当の進み具合で待つ（どれか 1 つなら、いちばん早く終わる物＝番号のいちばん小さい物を待てばよい）
        double ms = timeout >= 0x7fffffffffffffffull ? 1e300 : timeout / 1e6;
        uint64_t need = all ? 0 : UINT64_MAX;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t m = ((struct fence *) fs[i])->msg;
            if (all ? m > need : m < need) need = m;
        }
        if (!progress_wait(need, ms)) return VK_TIMEOUT;
    }
    if (known) {
        for (uint32_t i = 0; i < n; i++) complete((struct dev *) dv, ((struct fence *) fs[i])->q, ((struct fence *) fs[i])->serial);
        ((struct dev *) dv)->skipped_wait = 1;
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
        struct dev *d = (struct dev *) dv;
        if (d->npb && !pb_wait(d, fc->q, fc->serial, 0)) return VK_NOT_READY;
        if (real_fence() && fc->msg > g_done_msg) {
            progress_poll();
            if (fc->msg > g_done_msg) return VK_NOT_READY;
        }
        // submit に付けた fence は「終わった」と答える（版 3 の待ち方）
        complete((struct dev *) dv, fc->q, fc->serial);
        ((struct dev *) dv)->skipped_wait = 1;
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
            d->skipped_wait = 1;
            g_stat.skipped++;
            return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        }
    }
    // 受け手は 1 本の流れで命令を順に片づけるので、長く待たせるとほかのスレッドの合図（vkSignalSemaphore）が届かない。
    // 2 ms ずつに区切って聞き直し、そのたびに錠を放す
    VkResult r;
    double t0 = now_ms();
    for (;;) {
        double gone = (now_ms() - t0) * 1e6;
        uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : (gone >= (double) timeout ? 0 : timeout - (uint64_t) gone);
        uint64_t slice = left < 2000000ull ? left : 2000000ull;
        vr_buf b = { 0 };
        vr_u64(&b, ID(dv));
        vr_u32(&b, wi->flags);
        vr_u32(&b, wi->semaphoreCount);
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) { vr_u64(&b, ID(wi->pSemaphores[i])); vr_u64(&b, wi->pValues[i]); }
        vr_u64(&b, slice);
        r = rpc_call(VR_WAIT_SEMAPHORES, &b, NULL);
        free(b.p);
        if (r != VK_TIMEOUT || slice == left) break;
    }
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
    struct dev *d = (struct dev *) dv;
    struct sem *sm = (struct sem *) si->semaphore;
    AcquireSRWLockExclusive(&d->lock);
    if (sm && si->value > sm->reached) sm->reached = si->value;
    struct pbatch *fr = NULL;
    size_t nfr = 0;
    if (d->npb) pb_resolve_locked(d, &fr, &nfr);
    ReleaseSRWLockExclusive(&d->lock);
    pb_release(fr, nfr);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(si->semaphore));
    vr_u64(&b, si->value);
    rpc_send(VR_SIGNAL_SEMAPHORE, &b);
    free(b.p);
    rpc_flush(); // 待っている GPU の仕事があるので、ためずにすぐ送る
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL queue_submit(VkQueue qh, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    struct queue *q = (struct queue *) qh;
    struct dev *d = q->d;
    // u1：結んだあとに書いた descriptor（update after bind）と、アドレスで読み書きする buffer を、command buffer の読む・書く範囲に足す。
    // whole：出どころの分からないアドレスを使うシェーダーがある
    int whole = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
            if (c->nub) uab_resolve(c);
            psb_collect(c);
            whole |= c->psb_whole;
        }
    if (!g_bda_host || !bda_on()) whole = 0;
    // GPU が読む範囲のうち、アプリが書いたページを先に送る
    {
        rng *rd = NULL;
        size_t nrd = 0, crd = 0;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
                for (size_t k = 0; k < c->nrd; k++) rng_add(&rd, &nrd, &crd, c->rd[k].m, c->rd[k].off, c->rd[k].len, 0);
                // GPU が書く範囲でも、アプリが書いたページは先に送る（GPU が書かなかった所を、読み戻しで古い値に戻さないため）
                for (size_t k = 0; k < c->nr; k++) rng_add(&rd, &nrd, &crd, c->r[k].m, c->r[k].off, c->r[k].len, 0);
            }
        // u1：出どころの分からないアドレスがあれば、アドレスで使うメモリを全部見る
        // （この submit が使わないメモリにも触るので、ほかの糸に放されないよう、送り終わるまで錠を持つ）
        if (whole) {
            AcquireSRWLockShared(&d->lock);
            for (struct mem *m = d->mems; m; m = m->next)
                if ((m->aflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) && !m->jpeg && !m->delta) rng_add(&rd, &nrd, &crd, m, 0, m->size, 0);
            InterlockedIncrement64(&g_u1.whole);
        }
        rng_merge(rd, &nrd);
        for (size_t k = 0; k < nrd; k++) send_dirty(rd[k].m, rd[k].off, rd[k].len);
        if (whole) ReleaseSRWLockShared(&d->lock);
        free(rd);
    }
    // WAIT を付けない query の結果の写しは、アプリが前の仕事の終わりを待ってから出す前提で読む。
    // ドライバはその待ちに往復せず答えているので、受け手ではまだ前の仕事が動いていて、写しが 0 を書く
    // （CTS の query_pool.occlusion_query の wait_queue など）。その時だけ、受け手に本当に待たせる
    static int qw = -1;
    if (qw < 0) { const char *e = getenv("IPHONE_VK_QCOPY_WAIT"); qw = !(e && *e == '0'); }
    if (qw && d->skipped_wait && !d->npb) {
        int need = 0;
        for (uint32_t i = 0; i < n && !need; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount && !need; j++)
                need = ((struct cb *) si[i].pCommandBuffers[j])->qcopy_nowait;
        if (need) {
            vr_buf w = { 0 };
            vr_u64(&w, ID(q));
            rpc_call(VR_QUEUE_WAIT_IDLE, &w, NULL);
            free(w.p);
            d->skipped_wait = 0;
        }
    }
    // timeline の値を待つ submit（まだ届いていない値）があるか。あれば 1 つずつ送る。
    // 受け手は送られた 1 回ごとに終わりを追うので、合図を待つ物と待たない物を同じ回に入れると、待たない物の終わりも分からなくなる
    int tlw = 0;
    for (uint32_t i = 0; i < n && !tlw; i++) {
        const VkTimelineSemaphoreSubmitInfo *tl = NULL;
        for (const VkBaseInStructure *x = si[i].pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (const VkTimelineSemaphoreSubmitInfo *) x;
        for (uint32_t j = 0; tl && j < si[i].waitSemaphoreCount && j < tl->waitSemaphoreValueCount; j++) {
            struct sem *sm = (struct sem *) si[i].pWaitSemaphores[j];
            if (sm->timeline && tl->pWaitSemaphoreValues[j] > sm->reached) tlw = 1;
        }
    }
    uint32_t per = tlw ? 1 : 8;
    uint64_t last_msg = 0;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
            ev_fire(c);
            wr_resolve(c);
        }
    // 帳簿は送る前に付け、送り終わるまで錠を持つ（ほかのスレッドが、帳簿の無いまま「届いた」を受け取らないように）
    AcquireSRWLockExclusive(&d->lock);
    // 通し番号は submit の 1 つずつに振る（timeline の値は、その submit とそれより前のものが終わったことだけを知らせる）
    uint64_t base = d->serial + 1, serial = base;
    d->serial += n ? n : 1;
    for (uint32_t i = 0; i < n; i++) {
        serial = base + i;
        for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
            struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
            for (size_t k = 0; k < c->nr; k++) {
                size_t before = d->npend;
                rng_add(&d->pend, &d->npend, &d->cappend, c->r[k].m, c->r[k].off, c->r[k].len, serial);
                if (d->npend > before) { d->pend[before].q = q; d->pend[before].lazy = c->r[k].lazy; }
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
        // まだ届いていない timeline の値を待つ submit は、止まっているかもしれない物として覚える
        if (tlw && tl) {
            semval *w = NULL;
            uint32_t nw = 0;
            for (uint32_t j = 0; j < si[i].waitSemaphoreCount && j < tl->waitSemaphoreValueCount; j++) {
                struct sem *sm = (struct sem *) si[i].pWaitSemaphores[j];
                if (!sm->timeline || tl->pWaitSemaphoreValues[j] <= sm->reached) continue;
                w = realloc(w, (nw + 1) * sizeof *w);
                w[nw++] = (semval) { sm, tl->pWaitSemaphoreValues[j] };
            }
            if (nw) {
                if (d->npb == d->cpb) { d->cpb = d->cpb ? d->cpb * 2 : 16; d->pb = realloc(d->pb, d->cpb * sizeof *d->pb); }
                struct pbatch *pb = &d->pb[d->npb++];
                pb->serial = serial;
                pb->q = q;
                pb->w = w;
                pb->nw = nw;
                pb->ncb = si[i].commandBufferCount;
                pb->cbs = malloc((pb->ncb ? pb->ncb : 1) * sizeof *pb->cbs);
                for (uint32_t j = 0; j < pb->ncb; j++) pb->cbs[j] = (struct cb *) si[i].pCommandBuffers[j];
            }
        }
    }
    serial = base + (n ? n - 1 : 0);
    // 出したばかりで、もう道の分かる物は外す（読むメモリは submit の前に送ってある）
    struct pbatch *fr0 = NULL;
    size_t nfr0 = 0;
    if (d->npb) pb_resolve_locked(d, &fr0, &nfr0);
    // 受け手は 1 回に 8 まで受け取るので、分けて送る（fence は最後にだけ付ける）
    for (uint32_t at = 0; at < n || (n == 0 && at == 0); at += per) {
        uint32_t k = n - at < per ? n - at : per;
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
        vr_u64(&b, at + per >= n ? ID(f) : 0);
        rpc_send(VR_QUEUE_SUBMIT, &b);
        free(b.p);
        last_msg = g_submit_msgs;
        {
            uint64_t m = g_submit_msgs; // いま送った submit の番号（d->lock を持っているので、同じ装置の submit とは混ざらない）
            for (uint32_t i = at; i < at + k; i++)
                for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                    struct cb *c = (struct cb *) si[i].pCommandBuffers[j];
                    for (size_t x = 0; x < c->nqw; x++) qpv_wrote(c->qw[x].qp, c->qw[x].first, c->qw[x].n, m);
                    for (size_t x = 0; x < c->nrd; x++) mu_note(c->rd[x].m, c->rd[x].off, c->rd[x].len, m);
                    for (size_t x = 0; x < c->nr; x++) mu_note(c->r[x].m, c->r[x].off, c->r[x].len, m);
                    // u1：出どころの分からないアドレスを使うなら、アドレスで使うメモリの全部を帳簿に載せる（d->lock を持っている）
                    if (whole && c->psb_whole)
                        for (struct mem *mm = d->mems; mm; mm = mm->next)
                            if ((mm->aflags & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT) && !mm->jpeg && !mm->delta) { mu_note(mm, 0, mm->size, m); mm->wide = 1; }
                }
        }
        if (n == 0) break;
    }
    rpc_flush();
    ReleaseSRWLockExclusive(&d->lock);
    for (size_t i = 0; i < nfr0; i++) { free(fr0[i].w); free(fr0[i].cbs); }
    free(fr0);
    if (f) { ((struct fence *) f)->serial = serial; ((struct fence *) f)->q = q; ((struct fence *) f)->msg = last_msg; }
    if (last_msg) q->last_msg = last_msg;
    return g_dead ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

// ---- vkQueueBindSparse（S1） ----
// 結び付けは中身を動かさない（sparse の物は作ったときに隠しメモリへ丸ごと結んである）。
// 待つ・知らせる semaphore（timeline の値も）と fence だけを、command buffer の無い submit として同じキューに送る
static VkResult VKAPI_CALL queue_bind_sparse(VkQueue qh, uint32_t n, const VkBindSparseInfo *bi, VkFence f) {
    uint32_t nw = 0, nb = 0, no = 0, ni = 0;
    for (uint32_t i = 0; i < n; i++) nw += bi[i].waitSemaphoreCount;
    VkSubmitInfo *si = n ? calloc(n, sizeof *si) : NULL;
    VkTimelineSemaphoreSubmitInfo *tl = n ? calloc(n, sizeof *tl) : NULL;
    VkPipelineStageFlags *st = nw ? malloc(nw * sizeof *st) : NULL;
    if ((n && (!si || !tl)) || (nw && !st)) { free(si); free(tl); free(st); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (uint32_t k = 0; k < nw; k++) st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    for (uint32_t i = 0, at = 0; i < n; i++) {
        si[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si[i].waitSemaphoreCount = bi[i].waitSemaphoreCount;
        si[i].pWaitSemaphores = bi[i].pWaitSemaphores;
        si[i].pWaitDstStageMask = st + at;
        at += bi[i].waitSemaphoreCount;
        si[i].signalSemaphoreCount = bi[i].signalSemaphoreCount;
        si[i].pSignalSemaphores = bi[i].pSignalSemaphores;
        for (const VkBaseInStructure *x = bi[i].pNext; x; x = x->pNext)
            if (x->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                tl[i] = *(const VkTimelineSemaphoreSubmitInfo *) x;
                tl[i].pNext = NULL;
                si[i].pNext = &tl[i];
            }
        nb += bi[i].bufferBindCount;
        no += bi[i].imageOpaqueBindCount;
        ni += bi[i].imageBindCount;
    }
    static volatile LONG calls;
    LONG c = InterlockedIncrement(&calls);
    if (c <= 3 || (c & (c - 1)) == 0)
        say("vkQueueBindSparse %ld 回目：buffer %u・image（opaque）%u・image %u の結び付け。中身は動かさず、合図だけ送る", c, nb, no, ni);
    VkResult r = queue_submit(qh, n, si, f);
    free(si);
    free(tl);
    free(st);
    return r;
}

static VkResult VKAPI_CALL queue_wait_idle(VkQueue qh) {
    struct queue *q = (struct queue *) qh;
    if (real_fence()) progress_wait(q->last_msg, 1e300);
    complete(q->d, q, q->d->serial);
    q->d->skipped_wait = 1;
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
    if (real_fence()) progress_wait(g_submit_msgs, 1e300);
    complete(d, NULL, d->serial);
    d->skipped_wait = 1;
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
    if (bda_on()) vab_add(bf); // CPU から見えるメモリの buffer なら、push constant に入ったアドレスから引けるようにする
    return bf->addr;
}

static uint64_t VKAPI_CALL get_buffer_capture_address(VkDevice dv, const VkBufferDeviceAddressInfo *i) { (void) dv; (void) i; return 0; }
static uint64_t VKAPI_CALL get_memory_capture_address(VkDevice dv, const VkDeviceMemoryOpaqueCaptureAddressInfo *i) { (void) dv; (void) i; return 0; }

// ---- query（GPU に数えさせる：遮蔽・時刻） ----
// 受け手の VR_CREATE_QUERY_POOL などに送る。結果は受け手が出した仕事を全部待ってから読む
// query pool ごとの、1 つの query の値の数（結果を取るときに、終わっていない query の欄を書かないために使う）。0 は分からない
static struct { uint64_t id; int nv; uint32_t count; uint64_t *wmsg; int occ; uint64_t *cw, *cv; } g_qpv[256]; // occ：1＝遮蔽・2＝パイプラインの統計 // wmsg：query ごとに書いた submit の番号
static SRWLOCK g_qpv_lock = SRWLOCK_INIT;

static void qpv_put(uint64_t id, int nv, uint32_t count, int occ) {
    AcquireSRWLockExclusive(&g_qpv_lock);
    int k = -1;
    for (int i = 0; i < 256 && k < 0; i++) if (!g_qpv[i].id) k = i;
    if (k < 0) k = (int) (id % 256); // 満ちていれば押し出す（押し出された pool は前のやり方に戻るだけ）
    g_qpv[k].id = id;
    g_qpv[k].nv = nv;
    g_qpv[k].count = count;
    g_qpv[k].occ = occ;
    free(g_qpv[k].wmsg);
    g_qpv[k].wmsg = calloc(count ? count : 1, sizeof *g_qpv[k].wmsg);
    free(g_qpv[k].cw);
    free(g_qpv[k].cv);
    g_qpv[k].cw = calloc(count ? count : 1, sizeof *g_qpv[k].cw);
    g_qpv[k].cv = calloc((size_t) (count ? count : 1) * (nv > 0 ? nv : 1), sizeof *g_qpv[k].cv);
    ReleaseSRWLockExclusive(&g_qpv_lock);
}

// submit の番号 m で、pool id の first から n 個の query を書いた
static void qpv_wrote(uint64_t id, uint32_t first, uint32_t n, uint64_t m) {
    AcquireSRWLockExclusive(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            for (uint32_t q = first; q < first + n && q < g_qpv[i].count && g_qpv[i].wmsg; q++) g_qpv[i].wmsg[q] = m;
            break;
        }
    ReleaseSRWLockExclusive(&g_qpv_lock);
}

// pool id の first から n 個の query を、最後に書いた submit の番号（覚えていない pool なら UINT64_MAX＝全部待ってもらう）
static uint64_t qpv_wmsg(uint64_t id, uint32_t first, uint32_t n) {
    uint64_t w = UINT64_MAX;
    AcquireSRWLockShared(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            if (!g_qpv[i].wmsg) break;
            w = 0;
            for (uint32_t q = first; q < first + n && q < g_qpv[i].count; q++) if (g_qpv[i].wmsg[q] > w) w = g_qpv[i].wmsg[q];
            break;
        }
    ReleaseSRWLockShared(&g_qpv_lock);
    return w;
}

static int qpv_get(uint64_t id, int del) {
    int nv = 0;
    AcquireSRWLockExclusive(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            nv = g_qpv[i].nv;
            if (del) {
                g_qpv[i].id = 0;
                free(g_qpv[i].wmsg); free(g_qpv[i].cw); free(g_qpv[i].cv);
                g_qpv[i].wmsg = g_qpv[i].cw = g_qpv[i].cv = NULL;
            }
            break;
        }
    ReleaseSRWLockExclusive(&g_qpv_lock);
    return nv;
}

// ---- 終わった query の値を手元に覚える（IPHONE_VK_QUERY_BATCH=0 で切る） ----
// Unreal Engine は 1 コマに数百の遮蔽の query を 1 個ずつ、終わるまで読み回す。query は毎コマ書き直されるので、
// 1 個読むたびに往復していた（実機で 1 コマ約 830 回・1 回約 1 ms で、毎秒 1 コマ前後）。
// 1 個を読む往復で、同じ pool のその先の query もまとめて読み、書いた submit が今回待った番号以下のものだけ覚える
// （受け手はその番号の submit まで待ってから読むので、その query の値はもう変わらない）。
// 覚えた値は、その query を書いた submit の番号が同じあいだだけ使う。書き直し・reset で番号が変わるか、CPU からの reset で消す
#define QRC_SPAN 4096
static uint64_t g_qrc_hits, g_qrc_trips;
static void qrc_print(void) {
    fprintf(stderr, "[iphone-vk] query の値を覚えから答えた回数 %llu・まとめて読んだ往復 %llu\n",
            (unsigned long long) g_qrc_hits, (unsigned long long) g_qrc_trips);
}
static int qrc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("IPHONE_VK_QUERY_BATCH");
        v = !(e && *e == '0');
        const char *st = getenv("IPHONE_VK_STATS");
        if (v && st && *st && *st != '0') atexit(qrc_print);
    }
    return v;
}
// 覚えていれば、query 1 個の値をアプリの形（es バイトが nv 個・頼まれれば「終わったか」）で書いて 1
static int qrc_get(uint64_t id, uint32_t q, size_t es, int want_av, void *data, size_t size) {
    int hit = 0;
    AcquireSRWLockShared(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            int nv = g_qpv[i].nv;
            if (g_qpv[i].cw && q < g_qpv[i].count && g_qpv[i].cw[q] && g_qpv[i].cw[q] == g_qpv[i].wmsg[q]
                && (size_t) (nv + want_av) * es <= size) {
                uint8_t *dst = data;
                for (int k = 0; k < nv; k++) memcpy(dst + (size_t) k * es, &g_qpv[i].cv[(size_t) q * nv + k], es);
                if (want_av) { uint64_t one = 1; memcpy(dst + (size_t) nv * es, &one, es); }
                hit = 1;
            }
            break;
        }
    ReleaseSRWLockShared(&g_qpv_lock);
    return hit;
}
// 受け手から 64 ビット・「終わったか」付きで読んだ first から n 個のうち、書いた submit が upto 以下で終わっているものを覚える
static void qrc_put(uint64_t id, uint32_t first, uint32_t n, const uint8_t *src, size_t ts, uint64_t upto) {
    AcquireSRWLockExclusive(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            int nv = g_qpv[i].nv;
            if (!g_qpv[i].cw) break;
            for (uint32_t j = 0; j < n && first + j < g_qpv[i].count; j++) {
                uint32_t q = first + j;
                uint64_t w = g_qpv[i].wmsg[q], av;
                memcpy(&av, src + (size_t) j * ts + (size_t) nv * 8, 8);
                if (!w || w > upto || !av) continue;
                memcpy(&g_qpv[i].cv[(size_t) q * nv], src + (size_t) j * ts, (size_t) nv * 8);
                g_qpv[i].cw[q] = w;
            }
            break;
        }
    ReleaseSRWLockExclusive(&g_qpv_lock);
}
static void qrc_drop(uint64_t id, uint32_t first, uint32_t n) {
    AcquireSRWLockExclusive(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            for (uint32_t q = first; q < first + n && q < g_qpv[i].count && g_qpv[i].cw; q++) g_qpv[i].cw[q] = 0;
            break;
        }
    ReleaseSRWLockExclusive(&g_qpv_lock);
}
// 読み直しが要る query（書いた submit が upto 以下で、まだ覚えていない物）の番号の範囲。q は必ず入れる。
// DXVK は空いた query の番号を飛び飛びに使い回すので、q から先だけでなく pool 全体から選ぶ
static void qrc_range(uint64_t id, uint32_t q, uint64_t upto, uint32_t *lo, uint32_t *hi) {
    *lo = *hi = q;
    AcquireSRWLockShared(&g_qpv_lock);
    for (int i = 0; i < 256; i++)
        if (g_qpv[i].id == id) {
            if (!g_qpv[i].cw) break;
            for (uint32_t k = 0; k < g_qpv[i].count; k++) {
                uint64_t w = g_qpv[i].wmsg[k];
                if (!w || w > upto || g_qpv[i].cw[k] == w) continue;
                if (k < *lo) *lo = k;
                if (k > *hi) *hi = k;
            }
            break;
        }
    ReleaseSRWLockShared(&g_qpv_lock);
    if (*hi - *lo + 1 > QRC_SPAN) { if (q - *lo < QRC_SPAN) *hi = *lo + QRC_SPAN - 1; else { *lo = q; *hi = q + QRC_SPAN - 1; } }
}

static VkResult VKAPI_CALL create_qpool(VkDevice dv, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void) a;
    if (!(g_hw.caps & VR_CAP_QUERY)) return VK_ERROR_FEATURE_NOT_PRESENT;
    uint64_t id = new_id();
    int nv = 0;
    if (ci->queryType == VK_QUERY_TYPE_OCCLUSION || ci->queryType == VK_QUERY_TYPE_TIMESTAMP) nv = 1;
    else if (ci->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS) nv = __builtin_popcount(ci->pipelineStatistics);
    if (nv > 0) qpv_put(id, nv, ci->queryCount, ci->queryType == VK_QUERY_TYPE_OCCLUSION ? 1 : ci->queryType == VK_QUERY_TYPE_PIPELINE_STATISTICS ? 2 : ci->queryType == VK_QUERY_TYPE_TIMESTAMP ? 3 : 0);
    if (getenv("IPHONE_VK_QUERY_LOG")) say("query pool %llu：種類 %d・%u 個・統計 0x%x", (unsigned long long) id, ci->queryType, ci->queryCount, ci->pipelineStatistics);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->queryType);
    vr_u32(&b, ci->queryCount);
    vr_u32(&b, ci->pipelineStatistics);
    rpc_send(VR_CREATE_QUERY_POOL, &b);
    free(b.p);
    *out = (VkQueryPool) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_qpool(VkDevice dv, VkQueryPool qp, const VkAllocationCallbacks *a) {
    (void) a;
    if (!qp) return;
    qpv_get(ID(qp), 1);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    rpc_send(VR_DESTROY_QUERY_POOL, &b);
    free(b.p);
}

// IPHONE_VK_OCC_VISIBLE=1：遮蔽の query に、往復せず「見えている（大きな数）」と答える。
// 毎コマその場で結果を待つゲーム（Unigine Heaven など）で、CPU と GPU が交互にしか動けなくなるのを避ける。
// 隠れた物も描くので GPU の仕事は増えるが、絵は変わらない。数を正しく数える試験（CTS）は落ちるので既定は切る
// パイプラインの統計の query は、往復せず「0・数え終わった」と答える（受け手が Apple の GPU なら既定で。IPHONE_VK_PSTAT_LOCAL=1 で必ず・0 でしない）。
// iPhone の MoltenVK は統計を正しく数えられない。一方で Unigine Heaven は画面の数字のために毎コマ結果が出るまで読み直し、
// そのたびに USB を往復していた（ふりで 1 コマに 80 回）
// 返り値：1＝遮蔽で「見えている」と答える・2＝統計で 0 と答える・0＝受け手に聞く
static int q_local(uint64_t qp) {
    static int occ_on = -1, ps_on = -1;
    if (occ_on < 0) { const char *e = getenv("IPHONE_VK_OCC_VISIBLE"); occ_on = e && *e == '1'; if (occ_on) say("遮蔽の query は往復せず「見えている」と答える"); }
    if (ps_on < 0) {
        const char *e = getenv("IPHONE_VK_PSTAT_LOCAL");
        ps_on = e && *e ? *e == '1' : g_hw.props.vendorID == 0x106b;
    }
    int kind = 0;
    AcquireSRWLockShared(&g_qpv_lock);
    for (int i = 0; i < 256; i++) if (g_qpv[i].id == qp) { kind = g_qpv[i].occ; break; }
    ReleaseSRWLockShared(&g_qpv_lock);
    if (kind == 1 && occ_on) return 1;
    if (kind == 2 && ps_on) return 2;
    // 調べ用：IPHONE_VK_TS_LOCAL=1 で、timestamp の query に往復せず「終わった・今の時刻」と答える。
    // ANGLE（Chrome）は毎コマ timestamp の query を待たずに読み（flags 0x1）、そのたびに受け手がその submit の終わりまで待つので、
    // 動画のコマの送りが終わるまで次のコマの仕事に進めない。時刻は本物の GPU の物ではない（時間の測り用に使うアプリでは長さが合わない）
    static int ts_on = -1;
    if (ts_on < 0) {
        // 既定は、アプリの名前が ANGLE のときだけ入れる。IPHONE_VK_TS_LOCAL=1 で必ず・=0 で入れない
        const char *e = getenv("IPHONE_VK_TS_LOCAL");
        ts_on = e && *e ? *e == '1' : g_app_angle;
        if (ts_on) say("timestamp の query は往復せず、今の時刻で答える（ANGLE の毎コマの読みで次のコマが止まらないように）");
    }
    if (kind == 3 && ts_on) return 3;
    return 0;
}

static VkResult VKAPI_CALL get_qpool_results(VkDevice dv, VkQueryPool qp, uint32_t first, uint32_t n, size_t size, void *data,
                                             VkDeviceSize stride, VkQueryResultFlags flags) {
    vr_buf b = { 0 }, out = { 0 };
    static int qlog = -1; // 毎回 getenv を呼ばない（query の結果は 1 コマに何十回も読まれる）
    if (qlog < 0) qlog = getenv("IPHONE_VK_QUERY_LOG") != NULL;
    if (qlog) {
        static uint64_t last;
        static unsigned cnt;
        if (ID(qp) != last || ++cnt % 5000 == 0) say("結果を読む pool %llu・%u から %u 個・flags 0x%x（%u 回目）", (unsigned long long) ID(qp), first, n, flags, cnt);
        last = ID(qp);
    }
    int local = q_local(ID(qp));
    if (local) {
        size_t es = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
        int want_av = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
        int nv = qpv_get(ID(qp), 0);
        if (nv < 1) nv = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t at = (uint64_t) i * stride;
            if (at + (uint64_t) (nv + want_av) * es > size) break;
            uint8_t *dst = (uint8_t *) data + at;
            uint64_t v = local == 1 ? (1u << 20) : 0, one = 1;
            if (local == 3) {
                double per = g_hw.props.limits.timestampPeriod > 0 ? g_hw.props.limits.timestampPeriod : 1.0;
                v = (uint64_t) (now_ms() * 1e6 / per);
                if (es == 4) v &= 0xffffffffu;
            }
            for (int k = 0; k < nv; k++) memcpy(dst + (size_t) k * es, &v, es);
            if (want_av) memcpy(dst + (size_t) nv * es, &one, es);
        }
        g_stat.skipped++;
        return VK_SUCCESS;
    }
    // 待たず（WAIT 無し）途中の値も要らない（PARTIAL 無し）なら、終わっていない query の欄は書いてはいけない（Vulkan の決まり）。
    // 前は受け手が 0 で埋めた並びをそのまま写し、アプリの欄を 0 で上書きしていた（CTS の query・timestamp の 65 件）。
    // 受け手には「終わったか」も付けて頼み、終わった query の値だけを写す。「終わったか」はアプリが頼んでいれば書く
    int nv = qpv_get(ID(qp), 0);
    if (nv > 0 && n && !(flags & (VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT))) {
        size_t es = (flags & VK_QUERY_RESULT_64_BIT) ? 8 : 4;
        uint64_t ts = (uint64_t) (nv + 1) * es, tsize = (uint64_t) n * ts;
        int want_av = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
        // 先に書き手の番号と submit の数を読む（読んだあとに submit が来ても、覚えが古くなる向きにしかならない）
        uint64_t wm = qpv_wmsg(ID(qp), first, n), nsub = g_submit_msgs;
        int nrc = n == 1 && qnr_on();
        int qb = n == 1 && qrc_on();
        if (qb && qrc_get(ID(qp), first, es, want_av, data, size)) {
            g_stat.skipped++;
            g_qrc_hits++;
            return VK_SUCCESS;
        }
        if (nrc && qnr_hit(ID(qp), first, wm, nsub)) {
            // まだ。欄の値は書かず、「終わったか」だけ 0 を書く（受け手に聞いたときと同じ）
            if ((uint64_t) (nv + want_av) * es <= size && want_av) memset((uint8_t *) data + (size_t) nv * es, 0, es);
            g_stat.skipped++;
            g_qnr_hits++;
            free(b.p);
            return VK_NOT_READY;
        }
        if (qb && wm != UINT64_MAX) {
            // その先の query もまとめて 64 ビットで読み、終わった物を覚えてから、頼まれた 1 個を覚えから答える
            // IPHONE_VK_QUERY_BATCH=2 なら、それまでに送った submit を全部待ってから読む（往復は減り、1 回の待ちは長くなる）
            static int qall = -1;
            if (qall < 0) { const char *e = getenv("IPHONE_VK_QUERY_BATCH"); qall = e && *e == '2'; }
            if (qall && nsub > wm) wm = nsub;
            uint32_t lo, hi;
            qrc_range(ID(qp), first, wm, &lo, &hi);
            uint32_t span = hi - lo + 1;
            uint64_t ts8 = (uint64_t) (nv + 1) * 8, tsz8 = (uint64_t) span * ts8;
            vr_u64(&b, ID(dv));
            vr_u64(&b, ID(qp));
            vr_u32(&b, lo);
            vr_u32(&b, span);
            vr_u64(&b, tsz8);
            vr_u64(&b, ts8);
            vr_u32(&b, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            vr_u64(&b, wm);
            VkResult r = rpc_call(VR_GET_QUERY_POOL_RESULTS, &b, &out);
            free(b.p);
            g_qrc_trips++;
            if (r >= 0 && out.p && out.n >= tsz8) qrc_put(ID(qp), lo, span, out.p, (size_t) ts8, wm);
            free(out.p);
            if (r < 0) return r;
            if (qrc_get(ID(qp), first, es, want_av, data, size)) return VK_SUCCESS;
            if (nrc) qnr_put(ID(qp), first, wm, nsub);
            if ((uint64_t) (nv + want_av) * es <= size && want_av) memset((uint8_t *) data + (size_t) nv * es, 0, es);
            return VK_NOT_READY;
        }
        vr_u64(&b, ID(dv));
        vr_u64(&b, ID(qp));
        vr_u32(&b, first);
        vr_u32(&b, n);
        vr_u64(&b, tsize);
        vr_u64(&b, ts);
        vr_u32(&b, flags | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        {
            static int qw = -1;
            if (qw < 0) { const char *e = getenv("IPHONE_VK_QUERY_UPTO"); qw = !(e && *e == '0'); }
            vr_u64(&b, qw ? wm : UINT64_MAX);
        }
        VkResult r = rpc_call(VR_GET_QUERY_POOL_RESULTS, &b, &out);
        free(b.p);
        if (nrc && r == VK_NOT_READY) qnr_put(ID(qp), first, wm, nsub);
        if (r >= 0 && out.p && out.n >= tsize)
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *src = (const uint8_t *) out.p + (size_t) i * ts;
                uint64_t av = 0;
                memcpy(&av, src + (size_t) nv * es, es);
                uint64_t at = (uint64_t) i * stride;
                if (at + (uint64_t) (nv + want_av) * es > size) break;
                uint8_t *dst = (uint8_t *) data + at;
                if (av) memcpy(dst, src, (size_t) nv * es);
                if (want_av) memcpy(dst + (size_t) nv * es, &av, es);
            }
        free(out.p);
        return r;
    }
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    vr_u32(&b, first);
    vr_u32(&b, n);
    vr_u64(&b, size);
    vr_u64(&b, stride);
    vr_u32(&b, flags);
    vr_u64(&b, qpv_wmsg(ID(qp), first, n));
    VkResult r = rpc_call(VR_GET_QUERY_POOL_RESULTS, &b, &out);
    free(b.p);
    if (r >= 0 && out.p) memcpy(data, out.p, out.n < size ? out.n : size);
    free(out.p);
    return r;
}

static void VKAPI_CALL reset_qpool(VkDevice dv, VkQueryPool qp, uint32_t first, uint32_t n) {
    vr_buf b = { 0 };
    qrc_drop(ID(qp), first, n);
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(qp));
    vr_u32(&b, first);
    vr_u32(&b, n);
    rpc_send(VR_RESET_QUERY_POOL, &b);
    free(b.p);
}

static void VKAPI_CALL cmd_reset_qpool(VkCommandBuffer cbh, VkQueryPool qp, uint32_t first, uint32_t n) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 1, 0, 0, 0);
    struct cb *c = (struct cb *) cbh;
    cb_note_q(c, ID(qp), first, n);
    vr_u32(&c->s, VR_CMD_RESET_QUERY_POOL);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
}

static void VKAPI_CALL cmd_begin_query(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q, VkQueryControlFlags flags) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 2, 0, 0, 0);
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BEGIN_QUERY);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
    vr_u32(&c->s, flags);
}

static void VKAPI_CALL cmd_end_query(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 3, 0, 0, 0);
    struct cb *c = (struct cb *) cbh;
    cb_note_q(c, ID(qp), q, 1);
    vr_u32(&c->s, VR_CMD_END_QUERY);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
}

// ---- transform feedback（VK_EXT_transform_feedback）：vkd3d-proton のための名乗りだけ ----
// stream output（DirectX の SO）は運ばない。命令は飛ばし、最初の 1 回だけ記録に出す（SO の buffer には何も書かれない）
static void xfb_note(const char *what) {
    static LONG once;
    if (!InterlockedExchange(&once, 1)) say("transform feedback の命令（%s）は飛ばす（名乗りだけ）", what);
}
static void VKAPI_CALL cmd_bind_xfb(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o, const VkDeviceSize *s) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 6, 0, 0, 0);
    (void) cbh; (void) first; (void) n; (void) b; (void) o; (void) s;
    xfb_note("つなぐ");
}
static void VKAPI_CALL cmd_begin_xfb(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 4, 0, 0, 0);
    (void) cbh; (void) first; (void) n; (void) b; (void) o;
    xfb_note("始める");
}
static void VKAPI_CALL cmd_end_xfb(VkCommandBuffer cbh, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 5, 0, 0, 0);
    (void) cbh; (void) first; (void) n; (void) b; (void) o;
    xfb_note("終える");
}
// 番号つきの query：0 番の stream はふつうの query と同じ。ほかの stream は飛ばす
static void VKAPI_CALL cmd_begin_query_indexed(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q, VkQueryControlFlags flags, uint32_t index) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 7, 0, 0, 0);
    if (index == 0) cmd_begin_query(cbh, qp, q, flags);
    else xfb_note("番号つきの query");
}
static void VKAPI_CALL cmd_end_query_indexed(VkCommandBuffer cbh, VkQueryPool qp, uint32_t q, uint32_t index) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 8, 0, 0, 0);
    if (index == 0) cmd_end_query(cbh, qp, q);
}
static void VKAPI_CALL cmd_draw_xfb_count(VkCommandBuffer cbh, uint32_t ninst, uint32_t first, VkBuffer b, VkDeviceSize off, uint32_t coff, uint32_t stride) {
    ls_ev((struct cb *) cbh, LE_DRAW, 0, 0, 1, 0, 0, 0);
    (void) cbh; (void) ninst; (void) first; (void) b; (void) off; (void) coff; (void) stride;
    xfb_note("書いた数で描く");
}

static void VKAPI_CALL cmd_write_timestamp(VkCommandBuffer cbh, VkPipelineStageFlagBits st, VkQueryPool qp, uint32_t q) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 9, 0, 0, 0);
    struct cb *c = (struct cb *) cbh;
    cb_note_q(c, ID(qp), q, 1);
    vr_u32(&c->s, VR_CMD_WRITE_TIMESTAMP);
    vr_u32(&c->s, st);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, q);
}

static void VKAPI_CALL cmd_write_timestamp2(VkCommandBuffer cbh, VkPipelineStageFlags2 st, VkQueryPool qp, uint32_t q) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 10, 0, 0, 0);
    // 2 の stage は、1 の形のいちばん後ろの段にまとめる
    VkPipelineStageFlagBits s1 = (st >> 32) ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : (VkPipelineStageFlagBits) (st ? st : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (st & (st - 1)) s1 = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; // 複数の段なら一番後ろ
    cmd_write_timestamp(cbh, s1, qp, q);
}

static void VKAPI_CALL cmd_copy_qpool_results(VkCommandBuffer cbh, VkQueryPool qp, uint32_t first, uint32_t n, VkBuffer dst,
                                              VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags flags) {
    ls_ev((struct cb *) cbh, LE_RISK, 0, 0, 11, 0, 0, 0);
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_COPY_QUERY_RESULTS);
    vr_u64(&c->s, ID(qp));
    vr_u32(&c->s, first);
    vr_u32(&c->s, n);
    vr_u64(&c->s, ID(dst));
    vr_u64(&c->s, off);
    vr_u64(&c->s, stride);
    vr_u32(&c->s, flags);
    if (!(flags & VK_QUERY_RESULT_WAIT_BIT)) c->qcopy_nowait = 1;
    // GPU が書く範囲として覚える（CPU から見えるメモリなら、待ちのあとに引き取る）
    g_wr_why = 4;
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
    if (create_buffer_ex(dv, i->pCreateInfo, &b, 1) == VK_SUCCESS) { // sparse でも隠しメモリは取らない
        buffer_reqs(dv, b, &r->memoryRequirements);
        destroy_buffer(dv, b, NULL);
    }
}
static void VKAPI_CALL dev_image_reqs(VkDevice dv, const VkDeviceImageMemoryRequirements *i, VkMemoryRequirements2 *r) {
    VkImage im = VK_NULL_HANDLE;
    memset(&r->memoryRequirements, 0, sizeof r->memoryRequirements);
    if (create_image_ex(dv, i->pCreateInfo, &im, 1) == VK_SUCCESS) {
        image_reqs(dv, im, &r->memoryRequirements);
        destroy_image(dv, im, NULL);
    }
}
static void VKAPI_CALL dev_image_sparse_reqs(VkDevice dv, const VkDeviceImageMemoryRequirements *i, uint32_t *n, VkSparseImageMemoryRequirements2 *r) {
    (void) dv;
    struct spimg s = { 0 };
    if (sparse_on() && (i->pCreateInfo->flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)) s = spimg_of(i->pCreateInfo);
    sparse_reqs_out(&s, n, NULL, r);
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
    F("vkInvalidateMappedMemoryRanges", invalidate_ranges);
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
#ifdef _WIN32
    F("vkCreateSwapchainKHR", create_swapchain);
    F("vkDestroySwapchainKHR", destroy_swapchain);
    F("vkGetSwapchainImagesKHR", swapchain_images);
    F("vkAcquireNextImageKHR", acquire_image);
    F("vkAcquireNextImage2KHR", acquire_image2);
    F("vkQueuePresentKHR", queue_present);
#endif
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
    if (sparse_on()) { // sparse（S1）。名乗らないときは前と同じ「まだ中継していない入口」のまま
        F("vkGetImageSparseMemoryRequirements", image_sparse_reqs);
        F("vkGetImageSparseMemoryRequirements2", image_sparse_reqs2);
        F("vkGetImageSparseMemoryRequirements2KHR", image_sparse_reqs2);
        F("vkQueueBindSparse", queue_bind_sparse);
    }
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
    F("vkCmdBindTransformFeedbackBuffersEXT", cmd_bind_xfb);
    F("vkCmdBeginTransformFeedbackEXT", cmd_begin_xfb);
    F("vkCmdEndTransformFeedbackEXT", cmd_end_xfb);
    F("vkCmdBeginQueryIndexedEXT", cmd_begin_query_indexed);
    F("vkCmdEndQueryIndexedEXT", cmd_end_query_indexed);
    F("vkCmdDrawIndirectByteCountEXT", cmd_draw_xfb_count);
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
#ifdef _WIN32
    F("vkGetPhysicalDeviceSurfaceSupportKHR", surface_support);
    F("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", surface_caps);
    F("vkGetPhysicalDeviceSurfaceFormatsKHR", surface_formats);
    F("vkGetPhysicalDeviceSurfacePresentModesKHR", surface_modes);
    F("vkGetPhysicalDeviceWin32PresentationSupportKHR", win32_presentation_support);
#endif
    return NULL;
}

static PFN_vkVoidFunction lookup(const char *pName) {
    F("vkCreateInstance", create_instance);
    F("vkDestroyInstance", destroy_instance);
    F("vkEnumerateInstanceExtensionProperties", enum_instance_ext);
    F("vkEnumerateInstanceVersion", enum_instance_version);
    F("vkEnumeratePhysicalDevices", enum_phys);
#ifdef _WIN32
    F("vkCreateWin32SurfaceKHR", create_win32_surface);
    F("vkDestroySurfaceKHR", destroy_surface);
#endif
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
