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
};
static struct {
    uint64_t sends[VR_OP_COUNT], calls[VR_OP_COUNT], bytes[VR_OP_COUNT];
    double wait_ms[VR_OP_COUNT];
    uint64_t bytes_in, flushes;
    uint64_t ww_calls, ww_pages;   // 書き込み監視を見た回数・見つかった書いたページ
    uint64_t skipped;              // 往復せずに済ませた待ち
    double ww_ms, rb_ms;           // 書き込み監視にかかった時間・引き取りにかかった時間
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
}

// ---- 受け手との通り道 ----
static SRWLOCK g_lock = SRWLOCK_INIT;
static vr_sock g_sock = VR_BAD_SOCK;
static int g_dead;
static vr_buf g_q; // まだ送っていない命令
static volatile LONG64 g_next_id;

// 数字だけの物（shader module など）の番号。ポインタと重ならないように上の方の桁を使う
static uint64_t new_id(void) { return (1ull << 62) | (uint64_t) InterlockedIncrement64(&g_next_id); }

static int flush_locked(void) {
    if (g_dead || g_sock == VR_BAD_SOCK) { g_q.n = 0; return 0; }
    if (g_q.n) g_stat.flushes++;
    if (g_q.n && !vr_send_all(g_sock, g_q.p, g_q.n)) { g_dead = 1; say("送れない：切れた"); }
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
static VkResult rpc_call(uint32_t op, vr_buf *b, vr_buf *out) {
    VkResult res = VK_ERROR_DEVICE_LOST;
    AcquireSRWLockExclusive(&g_lock);
    if (op < VR_OP_COUNT) g_stat.calls[op]++;
    double t0 = now_ms();
    put_msg_locked(op, b);
    if (flush_locked()) {
        uint32_t h[2];
        if (vr_recv_all(g_sock, h, 8)) {
            vr_buf tmp = { 0 }, *dst = out ? out : &tmp;
            dst->n = 0;
            vr_reserve(dst, h[1]);
            if (h[1] == 0 || vr_recv_all(g_sock, dst->p, h[1])) {
                dst->n = h[1];
                res = (VkResult) (int32_t) h[0];
                g_stat.bytes_in += 8 + h[1];
            } else {
                g_dead = 1;
            }
            free(tmp.p);
        } else {
            g_dead = 1;
            say("返事が来ない：切れた（命令 %u）", op);
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
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t nq;
    VkQueueFamilyProperties q[16];
    uint32_t ne;
    VkExtensionProperties ext[64];
    uint32_t caps; // 受け手ができること（VR_CAP_*）
} g_hw;
static SRWLOCK g_hw_lock = SRWLOCK_INIT;

// 中継するだけで済む（新しい入口が要らない）拡張だけを見せる
static const char *const ALLOWED_EXT[] = {
    "VK_KHR_16bit_storage", "VK_KHR_8bit_storage", "VK_KHR_shader_float16_int8", "VK_KHR_storage_buffer_storage_class",
    "VK_EXT_subgroup_size_control", "VK_KHR_shader_integer_dot_product", "VK_KHR_shader_non_semantic_info",
    "VK_KHR_driver_properties", "VK_KHR_spirv_1_4", "VK_KHR_shader_float_controls", "VK_KHR_shader_subgroup_extended_types",
    "VK_KHR_variable_pointers", "VK_KHR_uniform_buffer_standard_layout", "VK_EXT_scalar_block_layout",
    "VK_KHR_vulkan_memory_model", "VK_KHR_shader_atomic_int64", "VK_KHR_dedicated_allocation",
    "VK_KHR_get_memory_requirements2", "VK_KHR_bind_memory2", "VK_KHR_maintenance1", "VK_KHR_maintenance3",
    "VK_KHR_relaxed_block_layout", "VK_KHR_shader_draw_parameters", "VK_KHR_timeline_semaphore",
};

static int allowed_ext(const char *name) {
    for (size_t i = 0; i < sizeof ALLOWED_EXT / sizeof *ALLOWED_EXT; i++)
        if (!strcmp(name, ALLOWED_EXT[i])) return 1;
    return 0;
}

static void mask_features(VkPhysicalDeviceFeatures *f) {
    // 中継していない機能は「無い」と答える
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = f->sparseResidency4Samples = f->sparseResidency8Samples = f->sparseResidency16Samples = VK_FALSE;
    f->sparseResidencyAliased = f->pipelineStatisticsQuery = f->inheritedQueries = f->occlusionQueryPrecise = VK_FALSE;
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
            vr_rcopy(&rd, &g_hw.mem, sizeof g_hw.mem);
            uint32_t nq = vr_ru32(&rd);
            for (uint32_t i = 0; i < nq; i++) {
                VkQueueFamilyProperties q;
                vr_rcopy(&rd, &q, sizeof q);
                if (i < 16) g_hw.q[g_hw.nq++] = q;
            }
            uint32_t ne = vr_ru32(&rd);
            for (uint32_t i = 0; i < ne; i++) {
                VkExtensionProperties x;
                vr_rcopy(&rd, &x, sizeof x);
                if (allowed_ext(x.extensionName) && g_hw.ne < 64) g_hw.ext[g_hw.ne++] = x;
            }
            if (g_hw.ne < 64) g_hw.ext[g_hw.ne++] = (VkExtensionProperties) { "VK_KHR_swapchain", 70 };
            if (rd.at + 4 <= rd.n) g_hw.caps = vr_ru32(&rd); // 古い受け手は付けない
            const char *dl = getenv("IPHONE_VK_DELTA");
            if (dl && *dl == '0') g_hw.caps &= ~VR_CAP_DELTA;
            if (!rd.bad) {
                char real[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
                memcpy(real, g_hw.props.deviceName, sizeof real);
                real[sizeof real - 1] = 0;
                snprintf(g_hw.props.deviceName, sizeof g_hw.props.deviceName, "%.200s (iPhone-RPC)", real);
                if (g_hw.props.apiVersion > VK_API_VERSION_1_2)
                    g_hw.props.apiVersion = VK_MAKE_API_VERSION(0, 1, 2, VK_API_VERSION_PATCH(g_hw.props.apiVersion));
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
    struct mem *prev, *next;
};

struct buf { uint64_t size; VkMemoryRequirements req; struct mem *m; uint64_t memoff; };

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

struct dent { uint32_t binding, elem, type; struct buf *b; uint64_t off, range; };
struct dpool;
struct dset { struct dpool *pool; struct dset *next; uint32_t n, cap; struct dent *e; };
struct dpool { struct dset *head; };

struct cpool { uint32_t fam; struct cb *head; };
struct cb {
    VK_LOADER_DATA loader;
    struct dev *d;
    struct cpool *pool;
    struct cb *next;
    vr_buf s;          // 記録した命令
    uint32_t flags;
    rng *r;            // この command buffer で GPU が書くかもしれない範囲
    size_t nr, capr;
    rng *rd;           // GPU が読むかもしれない範囲（submit の前に、ここだけ送る）
    size_t nrd, caprd;
};

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

// off から len バイトのうち、アプリが書いたページを受け手へ送る
static void send_dirty(struct mem *m, uint64_t off, uint64_t len) {
    if (off >= m->size) return;
    if (len > m->size - off) len = m->size - off;
    size_t p0 = (size_t) (off / PAGE), p1 = (size_t) ((off + len + PAGE - 1) / PAGE);
    if (p1 > m->npages) p1 = m->npages;
    collect_dirty(m, p0, p1);
    const size_t chunk = (16u << 20) / PAGE;
    vr_buf b = { 0 };
    for (size_t i = p0; i < p1;) {
        if (!m->dirty[i]) { i++; continue; }
        size_t j = i;
        while (j < p1 && m->dirty[j] && j - i < chunk) m->dirty[j++] = 0;
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
    for (size_t i = 0; i < n && ok; i++)
        for (uint64_t at = r[i].off; at < r[i].off + r[i].len && ok; at += piece) {
            uint64_t k = r[i].off + r[i].len - at < piece ? r[i].off + r[i].len - at : piece;
            uint32_t h[2];
            if (!vr_recv_all(g_sock, h, 8)) { ok = 0; g_dead = 1; break; }
            if ((int32_t) h[0] < 0) {
                if (!g_dead) say("受け手が失敗を返した (%d)。この先は全部失敗にする", (int32_t) h[0]);
                g_dead = 1;
            }
            if ((int32_t) h[0] == VK_SUCCESS && r[i].m->delta && (g_hw.caps & VR_CAP_DELTA)) {
                // 形（0 そのまま・1 違い）を読んでから中身
                uint8_t *buf = malloc(h[1] ? h[1] : 1);
                if (!buf || (h[1] && !vr_recv_all(g_sock, buf, h[1]))) { free(buf); ok = 0; g_dead = 1; break; }
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
                free(buf);
                continue;
            }
            if ((int32_t) h[0] == VK_SUCCESS && h[1] == k) {
                if (!vr_recv_all(g_sock, r[i].m->shadow + at, k)) { ok = 0; g_dead = 1; break; }
            } else {
                // 読めなかった：中身は捨てる
                uint8_t tmp[4096];
                for (uint32_t left = h[1]; left;) {
                    uint32_t c = left < sizeof tmp ? left : (uint32_t) sizeof tmp;
                    if (!vr_recv_all(g_sock, tmp, c)) { ok = 0; g_dead = 1; break; }
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
        if (d->pend[i].serial <= upto && (!q || d->pend[i].q == q)) rng_add(&take, &nt, &ct, d->pend[i].m, d->pend[i].off, d->pend[i].len, d->pend[i].serial);
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
    (void) ci; (void) a;
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

static VkResult VKAPI_CALL enum_instance_version(uint32_t *v) {
    *v = VK_API_VERSION_1_2;
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
};

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
}

static void VKAPI_CALL get_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    get_features(pd, &f->features);
    query_chain(VR_GET_FEATURES2, f->pNext, FEAT_ST, sizeof FEAT_ST / sizeof *FEAT_ST, &f->features, sizeof f->features);
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

static void VKAPI_CALL get_format(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p) {
    (void) pd;
    vr_buf b = { 0 }, out = { 0 };
    vr_u32(&b, (uint32_t) f);
    memset(p, 0, sizeof *p);
    if (rpc_call(VR_GET_FORMAT_PROPS, &b, &out) == VK_SUCCESS && out.n >= sizeof *p) memcpy(p, out.p, sizeof *p);
    // 線形（linear）の画像を CPU から直接書かれると、写しの同期で追えないので見せない（アプリは受け渡し用のバッファから写す）
    p->linearTilingFeatures = 0;
    free(b.p);
    free(out.p);
}

static void VKAPI_CALL get_format2(VkPhysicalDevice pd, VkFormat f, VkFormatProperties2 *p) {
    get_format(pd, f, &p->formatProperties);
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
    vr_buf b = { 0 }, out = { 0 };
    vr_u32(&b, f);
    vr_u32(&b, t);
    vr_u32(&b, ti);
    vr_u32(&b, u);
    vr_u32(&b, fl);
    memset(p, 0, sizeof *p);
    VkResult r = rpc_call(VR_GET_IMAGE_FORMAT_PROPS, &b, &out);
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
    if (base) vr_put(&b, base, sizeof *base);
    vr_u32(&b, nc);
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t sz = st_size(FEAT_ST, sizeof FEAT_ST / sizeof *FEAT_ST, chain[i]->sType);
        vr_u32(&b, chain[i]->sType);
        vr_bytes(&b, BODY(chain[i]), sz - sizeof(VkBaseOutStructure));
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
    vr_u32(&b, ci->usage);
    VkResult r = rpc_call(VR_CREATE_BUFFER, &b, &o);
    if (r == VK_SUCCESS && o.n >= sizeof bf->req) memcpy(&bf->req, o.p, sizeof bf->req);
    free(b.p);
    free(o.p);
    if (r != VK_SUCCESS) { free(bf); return r; }
    *out = (VkBuffer) bf;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_buffer(VkDevice dv, VkBuffer bb, const VkAllocationCallbacks *a) {
    (void) a;
    if (!bb) return;
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
struct img { VkMemoryRequirements req; struct mem *m; uint64_t memoff; };

// 受け手の装置では swapchain を使わないので、PRESENT_SRC の形は GENERAL に読み替える
static VkImageLayout fix_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR || l == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR ? VK_IMAGE_LAYOUT_GENERAL : l;
}

static VkResult VKAPI_CALL create_image(VkDevice dv, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void) a;
    struct img *im = calloc(1, sizeof *im);
    if (!im) return VK_ERROR_OUT_OF_HOST_MEMORY;
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
    VkResult r = rpc_call(VR_CREATE_IMAGE, &b, &o);
    if (r == VK_SUCCESS && o.n >= sizeof im->req) memcpy(&im->req, o.p, sizeof im->req);
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
    vr_u32(&b, ci->format);
    vr_put(&b, &ci->components, sizeof ci->components);
    vr_put(&b, &ci->subresourceRange, sizeof ci->subresourceRange);
    rpc_send(VR_CREATE_IMAGE_VIEW, &b);
    free(b.p);
    *out = (VkImageView) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_view(VkDevice dv, VkImageView h, const VkAllocationCallbacks *a) { (void) a; if (h) send_destroy(VR_DESTROY_IMAGE_VIEW, dv, ID(h)); }

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

// ---- 描画用のパイプライン ----
static void put_stage(vr_buf *b, const VkPipelineShaderStageCreateInfo *st) {
    vr_u32(b, st->stage);
    vr_u32(b, st->flags);
    vr_u64(b, ID(st->module));
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
        uint64_t id = new_id();
        vr_buf b = { 0 };
        vr_u64(&b, ID(dv));
        vr_u64(&b, id);
        vr_u32(&b, c->flags);
        vr_u64(&b, ID(c->layout));
        vr_u64(&b, ID(c->renderPass));
        vr_u32(&b, c->subpass);
        vr_u32(&b, c->stageCount);
        for (uint32_t k = 0; k < c->stageCount; k++) put_stage(&b, &c->pStages[k]);
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
            vr_u32(&b, rs->depthClampEnable);
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
        VkResult r = rpc_call(VR_CREATE_GRAPHICS_PIPELINE, &b, NULL);
        free(b.p);
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
    c->minImageExtent.width = c->minImageExtent.height = 1;
    c->maxImageExtent.width = c->maxImageExtent.height = 4096;
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

static void VKAPI_CALL destroy_swapchain(VkDevice dv, VkSwapchainKHR h, const VkAllocationCallbacks *a) {
    (void) a;
    struct swapchain *sc = (struct swapchain *) h;
    if (!sc) return;
    if (sc->pend >= 0 && sc->fence[sc->pend]) wait_fences(dv, 1, &sc->fence[sc->pend], VK_TRUE, UINT64_MAX);
    if (sc->pool) destroy_cpool(dv, sc->pool, NULL);
    for (uint32_t i = 0; i < sc->n; i++) {
        if (sc->fence[i]) destroy_fence(dv, sc->fence[i], NULL);
        if (sc->img[i]) destroy_image(dv, sc->img[i], NULL);
        if (sc->imem[i]) free_memory(dv, sc->imem[i], NULL);
        if (sc->rb[i]) destroy_buffer(dv, sc->rb[i], NULL);
        if (sc->rbmem[i]) free_memory(dv, sc->rbmem[i], NULL);
    }
    if (sc->frames) say("swapchain：%llu コマ・%.1f コマ/秒", (unsigned long long) sc->frames, sc->frames * 1000.0 / (now_ms() - sc->t0));
    free(sc);
}

static VkResult VKAPI_CALL create_swapchain(VkDevice dv, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    (void) a;
    struct swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->d = (struct dev *) dv;
    sc->hwnd = surface_hwnd(ci->surface);
    sc->w = ci->imageExtent.width;
    sc->h = ci->imageExtent.height;
    sc->fmt = ci->imageFormat;
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
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
            .memoryTypeIndex = pick_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        if ((r = alloc_memory(dv, &mai, NULL, &sc->rbmem[i])) != VK_SUCCESS) break;
        ((struct mem *) sc->rbmem[i])->delta = 1;
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
static VkResult blit(struct swapchain *sc, uint32_t j) {
    if (!sc->hwnd) return VK_SUCCESS;
    uint8_t *px = sc->px[j];
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = (LONG) sc->w;
    bmi.bmiHeader.biHeight = -(LONG) sc->h; // 上から下へ
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    if (sc->fmt == VK_FORMAT_R8G8B8A8_UNORM || sc->fmt == VK_FORMAT_R8G8B8A8_SRGB)
        for (size_t p = 0; p < (size_t) sc->w * sc->h; p++) { uint8_t t = px[p * 4]; px[p * 4] = px[p * 4 + 2]; px[p * 4 + 2] = t; }
    RECT rc;
    GetClientRect(sc->hwnd, &rc);
    HDC dc = GetDC(sc->hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, 0, 0, (int) sc->w, (int) sc->h, px, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(sc->hwnd, dc);
    sc->frames++;
    // IPHONE_VK_DUMP=コマ番号:ファイル名 で、そのコマを PPM に書く（絵が正しいかを確かめる用）
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
    // 窓の大きさが変わったら、作り直してもらう
    if ((uint32_t) (rc.right - rc.left) != sc->w || (uint32_t) (rc.bottom - rc.top) != sc->h) return VK_SUBOPTIMAL_KHR;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL queue_present(VkQueue qh, const VkPresentInfoKHR *pi) {
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct swapchain *sc = (struct swapchain *) pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        VkDevice dv = (VkDevice) sc->d;
        // 描き終わりを待ってから画像を持ち帰り用に写す仕事を頼む（待たない）
        VkPipelineStageFlags st[16];
        for (uint32_t k = 0; k < 16; k++) st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = i == 0 ? (pi->waitSemaphoreCount < 16 ? pi->waitSemaphoreCount : 16) : 0,
            .pWaitSemaphores = pi->pWaitSemaphores, .pWaitDstStageMask = st, .commandBufferCount = 1, .pCommandBuffers = &sc->cmd[idx] };
        VkResult r = queue_submit(qh, 1, &si, sc->fence[idx]);
        // 1 つ前に頼んだコマを受け取って窓に描く（GPU はその間にさっき頼んだ分を進める）
        if (r == VK_SUCCESS && sc->pend >= 0) {
            uint32_t j = (uint32_t) sc->pend;
            r = wait_fences(dv, 1, &sc->fence[j], VK_TRUE, UINT64_MAX);
            reset_fences(dv, 1, &sc->fence[j]);
            if (r == VK_SUCCESS) r = blit(sc, j);
        }
        sc->pend = (int) idx;
        if (pi->pResults) pi->pResults[i] = r;
        if (r < 0 || (worst == VK_SUCCESS && r != VK_SUCCESS)) worst = r;
    }
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
DESTROY_SIMPLE(destroy_shader, VkShaderModule, VR_DESTROY_SHADER_MODULE)
DESTROY_SIMPLE(destroy_dsl, VkDescriptorSetLayout, VR_DESTROY_DSL)
DESTROY_SIMPLE(destroy_playout, VkPipelineLayout, VR_DESTROY_PIPELINE_LAYOUT)
DESTROY_SIMPLE(destroy_pipeline, VkPipeline, VR_DESTROY_PIPELINE)

static VkResult VKAPI_CALL create_dsl(VkDevice dv, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void) a;
    uint64_t id = new_id();
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, id);
    vr_u32(&b, ci->flags);
    vr_u32(&b, ci->bindingCount);
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        vr_u32(&b, ci->pBindings[i].binding);
        vr_u32(&b, ci->pBindings[i].descriptorType);
        vr_u32(&b, ci->pBindings[i].descriptorCount);
        vr_u32(&b, ci->pBindings[i].stageFlags);
    }
    rpc_send(VR_CREATE_DSL, &b);
    free(b.p);
    *out = (VkDescriptorSetLayout) (uintptr_t) id;
    return VK_SUCCESS;
}

static void VKAPI_CALL dsl_support(VkDevice dv, const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayoutSupport *s) {
    (void) dv; (void) ci;
    s->supported = VK_TRUE;
}

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
        vr_u32(&b, ci->pPushConstantRanges[i].stageFlags);
        vr_u32(&b, ci->pPushConstantRanges[i].offset);
        vr_u32(&b, ci->pPushConstantRanges[i].size);
    }
    rpc_send(VR_CREATE_PIPELINE_LAYOUT, &b);
    free(b.p);
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
        vr_u64(&b, ID(st->module));
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
        if (r != VK_SUCCESS) { say("compute pipeline を作れない (%d)", r); out[i] = VK_NULL_HANDLE; worst = r; continue; }
        out[i] = (VkPipeline) (uintptr_t) id;
    }
    return worst;
}

// ---- descriptor ----
static VkResult VKAPI_CALL create_dpool(VkDevice dv, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void) a;
    struct dpool *p = calloc(1, sizeof *p);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
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
    for (struct dset *s = p->head, *nx; s; s = nx) { nx = s->next; free(s->e); free(s); }
    p->head = NULL;
}

static void VKAPI_CALL destroy_dpool(VkDevice dv, VkDescriptorPool pp, const VkAllocationCallbacks *a) {
    (void) a;
    if (!pp) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(pp));
    rpc_send(VR_DESTROY_DESCRIPTOR_POOL, &b);
    free(b.p);
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

static VkResult VKAPI_CALL alloc_dsets(VkDevice dv, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    struct dpool *p = (struct dpool *) ai->descriptorPool;
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    vr_u64(&b, ID(p));
    vr_u32(&b, ai->descriptorSetCount);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        struct dset *s = calloc(1, sizeof *s);
        s->pool = p;
        s->next = p->head;
        p->head = s;
        out[i] = (VkDescriptorSet) s;
        vr_u64(&b, ID(ai->pSetLayouts[i]));
        vr_u64(&b, ID(s));
    }
    rpc_send(VR_ALLOC_DESCRIPTOR_SETS, &b);
    free(b.p);
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
        while (*pp2 && *pp2 != s) pp2 = &(*pp2)->next;
        if (*pp2) *pp2 = s->next;
        free(s->e);
        free(s);
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

static void dset_put(struct dset *s, uint32_t binding, uint32_t elem, uint32_t type, struct buf *bf, uint64_t off, uint64_t range) {
    for (uint32_t i = 0; i < s->n; i++)
        if (s->e[i].binding == binding && s->e[i].elem == elem) { s->e[i] = (struct dent) { binding, elem, type, bf, off, range }; return; }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->e = realloc(s->e, s->cap * sizeof *s->e); }
    s->e[s->n++] = (struct dent) { binding, elem, type, bf, off, range };
}

static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void) c;
    if (nc) say("descriptor のコピーはまだ扱わない（%u 件を飛ばした）", nc);
    vr_buf b = { 0 };
    vr_u64(&b, ID(dv));
    uint32_t real = 0;
    for (uint32_t i = 0; i < nw; i++) if (is_buffer_desc(w[i].descriptorType) || is_image_desc(w[i].descriptorType)) real++;
    vr_u32(&b, real);
    for (uint32_t i = 0; i < nw; i++) {
        if (!is_buffer_desc(w[i].descriptorType) && !is_image_desc(w[i].descriptorType)) { say("この種類の descriptor（%d）はまだ扱わない", w[i].descriptorType); continue; }
        struct dset *s = (struct dset *) w[i].dstSet;
        vr_u64(&b, ID(s));
        vr_u32(&b, w[i].dstBinding);
        vr_u32(&b, w[i].dstArrayElement);
        vr_u32(&b, w[i].descriptorType);
        vr_u32(&b, w[i].descriptorCount);
        if (is_image_desc(w[i].descriptorType)) {
            for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
                const VkDescriptorImageInfo *ii = &w[i].pImageInfo[k];
                vr_u64(&b, w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? ID(ii->sampler) : 0);
                vr_u64(&b, w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ? 0 : ID(ii->imageView));
                vr_u32(&b, fix_layout(ii->imageLayout));
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

static void cb_clear(struct cb *c) { c->s.n = 0; c->nr = 0; c->nrd = 0; }

static void cb_free(struct cb *c) { free(c->s.p); free(c->r); free(c->rd); free(c); }

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
    // storage buffer は GPU が書くかもしれない
    for (uint32_t i = 0; i < n; i++) {
        struct dset *s = (struct dset *) sets[i];
        if (!s) continue;
        for (uint32_t k = 0; k < s->n; k++) {
            struct dent *e = &s->e[k];
            if (e->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) cb_writes(c, e->b, e->off, e->range);
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
    vr_u32(&c->s, st);
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
    vr_u32(&c->s, contents);
}

static void VKAPI_CALL cmd_end_rp(VkCommandBuffer cbh) { vr_u32(&((struct cb *) cbh)->s, VR_CMD_END_RENDER_PASS); }

static void VKAPI_CALL cmd_next_subpass(VkCommandBuffer cbh, VkSubpassContents contents) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_NEXT_SUBPASS);
    vr_u32(&c->s, contents);
}

static void VKAPI_CALL cmd_draw(VkCommandBuffer cbh, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_DRAW);
    vr_u32(&c->s, vc);
    vr_u32(&c->s, ic);
    vr_u32(&c->s, fv);
    vr_u32(&c->s, fi);
}

static void VKAPI_CALL cmd_draw_indexed(VkCommandBuffer cbh, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_DRAW_INDEXED);
    vr_u32(&c->s, n);
    vr_u32(&c->s, ic);
    vr_u32(&c->s, first);
    vr_u32(&c->s, (uint32_t) vo);
    vr_u32(&c->s, fi);
}

static void draw_indirect(VkCommandBuffer cbh, uint32_t op, VkBuffer bb, VkDeviceSize off, uint32_t n, uint32_t stride) {
    struct cb *c = (struct cb *) cbh;
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
}

static void VKAPI_CALL cmd_bind_ib(VkCommandBuffer cbh, VkBuffer bb, VkDeviceSize off, VkIndexType t) {
    struct cb *c = (struct cb *) cbh;
    vr_u32(&c->s, VR_CMD_BIND_INDEX_BUFFER);
    vr_u64(&c->s, ID(bb));
    vr_u64(&c->s, off);
    vr_u32(&c->s, t);
    cb_reads(c, (struct buf *) bb, off, VK_WHOLE_SIZE);
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
    vr_u32(&c->s, VR_CMD_COPY_IMAGE_TO_BUFFER);
    vr_u64(&c->s, ID(im));
    vr_u64(&c->s, ID(bb));
    vr_u32(&c->s, fix_layout(l));
    vr_u32(&c->s, n);
    vr_put(&c->s, rg, n * sizeof *rg);
    cb_writes(c, (struct buf *) bb, 0, VK_WHOLE_SIZE);
}

static void VKAPI_CALL cmd_copy_buffer_to_image(VkCommandBuffer cbh, VkBuffer bb, VkImage im, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    struct cb *c = (struct cb *) cbh;
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
    int known = timeout == UINT64_MAX;
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
static VkResult VKAPI_CALL not_yet(void) {
    say("まだ中継していない入口が呼ばれた");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static const char *const NOT_YET[] = {
    "vkQueueBindSparse", "vkGetImageSparseMemoryRequirements",
    "vkCreateEvent", "vkDestroyEvent", "vkGetEventStatus", "vkSetEvent", "vkResetEvent",
    "vkCreateQueryPool", "vkDestroyQueryPool", "vkGetQueryPoolResults", "vkCreateBufferView", "vkDestroyBufferView",
    "vkGetImageSubresourceLayout", 
    
    
    "vkCmdSetLineWidth", "vkCmdSetDepthBias", "vkCmdSetBlendConstants",
    "vkCmdSetDepthBounds", "vkCmdSetStencilCompareMask", "vkCmdSetStencilWriteMask", "vkCmdSetStencilReference",
    
    
    "vkCmdClearDepthStencilImage", "vkCmdClearAttachments", "vkCmdResolveImage",
    "vkCmdSetEvent", "vkCmdResetEvent", "vkCmdWaitEvents", "vkCmdBeginQuery", "vkCmdEndQuery", "vkCmdResetQueryPool",
    "vkCmdWriteTimestamp", "vkCmdCopyQueryPoolResults", 
    "vkCmdExecuteCommands",
    // 1.1
    "vkGetDeviceGroupPeerMemoryFeatures", "vkCmdSetDeviceMask", 
    "vkGetImageSparseMemoryRequirements2", "vkCreateSamplerYcbcrConversion", "vkDestroySamplerYcbcrConversion",
    "vkCreateDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplate", "vkUpdateDescriptorSetWithTemplate",
    // 1.2
    "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount", "vkCreateRenderPass2", "vkCmdBeginRenderPass2",
    "vkCmdNextSubpass2", "vkCmdEndRenderPass2", "vkResetQueryPool", "vkGetBufferDeviceAddress", "vkGetBufferOpaqueCaptureAddress", "vkGetDeviceMemoryOpaqueCaptureAddress",
};

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
    for (size_t i = 0; i < sizeof NOT_YET / sizeof *NOT_YET; i++) F(NOT_YET[i], not_yet);
    return NULL;
}

static PFN_vkVoidFunction lookup_phys(const char *pName) {
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
