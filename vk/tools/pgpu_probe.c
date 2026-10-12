// PocketGPU の Linux キットの道具。start-pocketgpu.sh（順に確かめる）と run-on-iphone-gpu.sh（1 つのアプリを iPhone の GPU で起動）が使う。
// Windows のキットの tools\pgpu_common.ps1 と同じことを、Python も PowerShell も無い Linux（SteamOS を含む）でするため C で書いた。
// 依存は libc だけ。libvulkan.so.1 は loader・vklist のときだけ dlopen する。
// 組み方（vk/build_linux.sh probe か make_linux_kit.py）：
//   zig cc -target x86_64-linux-gnu.2.31 -O2 -I<Vulkan-Headers>/include tools/pgpu_probe.c -o pgpu-probe -ldl
//
//   pgpu-probe mux                     usbmuxd の口につながるか            MUX OK <どこ> / MUX NG <どこ> <理由>
//   pgpu-probe devices                 usbmuxd に見えている機器            DEV <番号> <USB|Network> speed=<bps> product=<番号> の行と DEVICES <数>
//   pgpu-probe pair <番号>             usbmuxd にペアリングの記録があるか  PAIR OK / PAIR NONE <返事の番号> / PAIR NG <理由>
//   pgpu-probe hello <番号> [proto] [ms] [port]   受け手（iPhone のアプリ）にあいさつ   HELLO OK mem_mb=<数> name=<名前> / HELLO NG <理由> [<返事>]
//   pgpu-probe bw <番号> [MB]          USB の太さ（iPhone の 50058 番）    BW OK down=<MB/s> up=<MB/s> rtt_ms=<ms> / BW NG <理由>
//   pgpu-probe loader                  Vulkan のローダーの版               LOADER OK <版> <ファイル> / LOADER NG <理由>
//   pgpu-probe vklist                  Vulkan の装置の一覧（vklist.exe と同じ形）  GPU0: 名前  (vendor 0x…, type …, Vulkan 1.x, … MiB)
//   pgpu-probe ww                      ドライバの書き込み監視（userfaultfd の非同期の書き込み禁止と PAGEMAP_SCAN）が使えるか  WW OK / WW NG <理由>
//   pgpu-probe usb                     この PC の USB に Apple の iPhone・iPad（05ac:12xx）が見えるか（/sys を読む）  USB <数> と USBDEV <番号> <速さ Mbps> の行
//   pgpu-probe broker [--quiet]        usbmuxd の受け渡し役。Steam の入れ物（pressure-vessel）・Flatpak の中のドライバに、
//                                      usbmuxd につないだ口を渡す。親（起こしたシェル）が終わると一緒に終わる
//   pgpu-probe broker-check            受け渡し役が動いているか            BROKER OK / BROKER NONE
// usbmuxd の口はドライバ（icd.c の mux_open）と同じ順に決める：IPHONE_VK_MUX → USBMUXD_SOCKET_ADDRESS → /var/run/usbmuxd → 受け渡し役
// 終わりの番号：0 は OK、1 は NG、2 は使い方の誤り
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/userfaultfd.h>
#include <vulkan/vulkan.h>

#define RECV_PORT 50060 // Vulkan の受け手（vk/remote/proto.h の VR_DEFAULT_PORT）
#define BW_PORT 50058   // USB の太さを測る受け口（vk/remote/bw.c）

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void set_timeout(int s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

// 全部送る・全部受ける。切れたら 0、時間切れなら -1
static int send_all(int s, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t k = send(s, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
        c += k;
        n -= (size_t) k;
    }
    return 1;
}
static int recv_all(int s, void *p, size_t n) {
    char *c = p;
    while (n) {
        ssize_t k = recv(s, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k == 0) return 0;
        if (k < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? -1 : 0;
        c += k;
        n -= (size_t) k;
    }
    return 1;
}

static int unix_connect(const char *path, int abstract) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n + 1 > sizeof a.sun_path) { errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path + (abstract ? 1 : 0), path, n);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    socklen_t len = abstract ? (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n) : (socklen_t) sizeof a;
    if (connect(s, (struct sockaddr *) &a, len) != 0) { int e = errno; close(s); errno = e; return -1; }
    return s;
}

static int tcp_connect(const char *host, const char *port, int ms) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) { errno = EHOSTUNREACH; return -1; }
    int s = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (s < 0) { freeaddrinfo(ai); return -1; }
    int r = connect(s, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd pf = { s, POLLOUT, 0 };
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&pf, 1, ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) { close(s); errno = err ? err : ETIMEDOUT; return -1; }
    } else if (r != 0) { int e = errno; close(s); errno = e; return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

// 受け渡し役の名前に使う uid（icd.c の mux_outer_uid と同じ）。利用者の名前空間の中なら /proc/self/uid_map で外の uid に戻す
static unsigned outer_uid(void) {
    unsigned u = (unsigned) getuid();
    FILE *f = fopen("/proc/self/uid_map", "r");
    if (f) {
        unsigned long in, out, cnt;
        while (fscanf(f, "%lu %lu %lu", &in, &out, &cnt) == 3)
            if (u >= in && u - in < cnt) { u = (unsigned) (out + (u - in)); break; }
        fclose(f);
    }
    return u;
}
static void broker_name(char *out, size_t n) { snprintf(out, n, "pocketgpu-usbmuxd-%u", outer_uid()); }

// 受け渡し役から usbmuxd につないだ口をもらう（icd.c の broker_get と同じ）
static int broker_get(const char *name) {
    int s = unix_connect(name, 1);
    if (s < 0) return -1;
    set_timeout(s, 5000);
    char b = 0;
    struct iovec iov = { &b, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { 0 };
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof u.buf;
    ssize_t r = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
    close(s);
    int fd = -1;
    if (r == 1)
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof fd);
    if (r != 1 || b != 'K') { if (fd >= 0) close(fd); errno = ECONNREFUSED; return -1; }
    return fd;
}

// usbmuxd の口を開く。where にどこか、なぜだめかは errno。use_broker が 0 なら受け渡し役は試さない（受け渡し役が自分で使う）
static int mux_open(char *where, size_t nw, int use_broker) {
    const char *mp = getenv("IPHONE_VK_MUX");
    const char *env = mp && *mp ? mp : getenv("USBMUXD_SOCKET_ADDRESS");
    char a[300] = "";
    if (env && *env) snprintf(a, sizeof a, "%s", env);
    int dflt = !a[0];
    if (!a[0]) snprintf(a, sizeof a, "/var/run/usbmuxd");
    if (a[0] == '@') {
        snprintf(where, nw, "%s", a);
        return use_broker ? broker_get(a + 1) : -1;
    }
    const char *path = !strncmp(a, "UNIX:", 5) || !strncmp(a, "unix:", 5) ? a + 5 : a[0] == '/' ? a : NULL;
    if (path) {
        snprintf(where, nw, "%s", path);
        int s = unix_connect(path, 0);
        const char *bk = getenv("IPHONE_VK_MUX_BROKER");
        if (s < 0 && dflt && use_broker && !(bk && *bk == '0')) {
            int e = errno;
            char name[64];
            broker_name(name, sizeof name);
            s = broker_get(name);
            if (s >= 0) snprintf(where, nw, "@%s", name);
            else errno = e;
        }
        if (s >= 0) set_timeout(s, 3000);
        return s;
    }
    char host[256] = "127.0.0.1", port[16];
    const char *c = strrchr(a, ':');
    if (c) {
        size_t n = (size_t) (c - a) < sizeof host - 1 ? (size_t) (c - a) : sizeof host - 1;
        memcpy(host, a, n);
        host[n] = 0;
        snprintf(port, sizeof port, "%s", c + 1);
    } else snprintf(port, sizeof port, "%s", a);
    snprintf(where, nw, "%s:%s", host, port);
    int s = tcp_connect(host, port, 1500);
    if (s >= 0) set_timeout(s, 3000);
    return s;
}

// usbmuxd に 1 つ頼んで返事（XML の plist）をもらう。だめなら NULL
static char *mux_call(int s, const char *type, const char *extra) {
    char body[2048];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>pocketgpu-start</string><key>ProgName</key><string>pocketgpu-start</string>%s</dict></plist>\n",
        type, extra);
    if (n <= 0 || n >= (int) sizeof body) return NULL;
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    if (send_all(s, h, sizeof h) != 1 || send_all(s, body, (size_t) n) != 1) return NULL;
    if (recv_all(s, h, sizeof h) != 1 || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = malloc(h[0] - 16 + 1);
    if (!r) return NULL;
    if (recv_all(s, r, h[0] - 16) != 1) { free(r); return NULL; }
    r[h[0] - 16] = 0;
    if (!strncmp(r, "bplist", 6)) { free(r); return NULL; } // 2 進の plist は読まない（XML で頼めば XML で返る）
    return r;
}

// at から limit までの <key>名前</key> の次の <integer> か <string>
static long long plist_int(const char *at, const char *key, const char *limit) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    if (!q || (limit && q > limit)) return -1;
    q += strlen(k);
    while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
    return strncmp(q, "<integer>", 9) ? -1 : atoll(q + 9);
}
static int plist_str(const char *at, const char *key, const char *limit, char *out, size_t n) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    out[0] = 0;
    if (!q || (limit && q > limit)) return 0;
    q += strlen(k);
    while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
    if (strncmp(q, "<string>", 8)) return 0;
    q += 8;
    const char *e = strstr(q, "</string>");
    if (!e) return 0;
    size_t l = (size_t) (e - q) < n - 1 ? (size_t) (e - q) : n - 1;
    memcpy(out, q, l);
    out[l] = 0;
    return 1;
}

struct dev { long long id, speed, product; char type[16], serial[80]; };

// 機器の一覧。数を返す（usbmuxd につながらなければ -1）
static int list_devices(struct dev *d, int max, char *where, size_t nw) {
    int s = mux_open(where, nw, 1);
    if (s < 0) return -1;
    char *r = mux_call(s, "ListDevices", "");
    close(s);
    if (!r) return -1;
    int n = 0;
    for (const char *q = r; n < max && (q = strstr(q, "<key>Properties</key>")); q++) {
        const char *end = strstr(q, "</dict>");
        struct dev *x = &d[n];
        memset(x, 0, sizeof *x);
        x->id = plist_int(q, "DeviceID", end);
        x->speed = plist_int(q, "ConnectionSpeed", end);
        x->product = plist_int(q, "ProductID", end);
        plist_str(q, "ConnectionType", end, x->type, sizeof x->type);
        plist_str(q, "SerialNumber", end, x->serial, sizeof x->serial);
        if (x->id >= 0) n++;
    }
    free(r);
    return n;
}

// iPhone のその番号につなぐ。だめなら -1 と why
static int connect_phone(long long dev, int port, const char **why, long long *num) {
    char where[320];
    int s = mux_open(where, sizeof where, 1);
    if (s < 0) { *why = "mux"; return -1; }
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>",
             dev, (unsigned) htons((uint16_t) port));
    char *r = mux_call(s, "Connect", extra);
    long long nb = r ? plist_int(r, "Number", NULL) : -1;
    free(r);
    if (nb != 0) { close(s); *why = r ? "refused" : "mux"; if (num) *num = nb; return -1; }
    return s;
}

static int cmd_mux(void) {
    char where[320];
    int s = mux_open(where, sizeof where, 1);
    if (s < 0) {
        printf("MUX NG %s %s\n", where, errno == ENOENT ? "missing" : errno == EACCES ? "denied" : errno == ECONNREFUSED ? "refused" : "error");
        return 1;
    }
    close(s);
    printf("MUX OK %s\n", where);
    return 0;
}

static int cmd_devices(void) {
    struct dev d[16];
    char where[320];
    int n = list_devices(d, 16, where, sizeof where);
    if (n < 0) { printf("DEVICES NG %s\n", where); return 1; }
    for (int i = 0; i < n; i++)
        printf("DEV %lld %s speed=%lld product=%lld\n", d[i].id, d[i].type[0] ? d[i].type : "?", d[i].speed, d[i].product);
    printf("DEVICES %d\n", n);
    return 0;
}

static int cmd_pair(long long id) {
    struct dev d[16];
    char where[320];
    int n = list_devices(d, 16, where, sizeof where);
    if (n < 0) { printf("PAIR NG mux\n"); return 1; }
    const char *udid = NULL;
    for (int i = 0; i < n; i++) if (d[i].id == id) udid = d[i].serial;
    if (!udid || !udid[0]) { printf("PAIR NG nodevice\n"); return 1; }
    int s = mux_open(where, sizeof where, 1);
    if (s < 0) { printf("PAIR NG mux\n"); return 1; }
    char extra[200];
    snprintf(extra, sizeof extra, "<key>PairRecordID</key><string>%s</string>", udid);
    char *r = mux_call(s, "ReadPairRecord", extra);
    close(s);
    if (!r) { printf("PAIR NG mux\n"); return 1; }
    int ok = strstr(r, "<key>PairRecordData</key>") != NULL;
    long long num = plist_int(r, "Number", NULL);
    free(r);
    if (ok) { printf("PAIR OK\n"); return 0; }
    printf("PAIR NONE %lld\n", num);
    return 1;
}

// 受け手にあいさつ（VR_HELLO）して、GPU の名前とメモリを聞く（tools\pgpu_common.ps1 の Invoke-Hello と同じ読み方）
static int cmd_hello(long long id, unsigned proto, int ms, int port) {
    const char *why = NULL;
    long long num = 0;
    int s = connect_phone(id, port, &why, &num);
    if (s < 0) { printf("HELLO NG %s %lld\n", why, num); return 1; }
    set_timeout(s, ms);
    uint32_t req[3] = { 1, 4, proto };
    uint32_t h[2];
    int k = send_all(s, req, sizeof req);
    if (k == 1) k = recv_all(s, h, sizeof h);
    if (k != 1) { close(s); printf("HELLO NG %s\n", k < 0 ? "timeout" : "closed"); return 1; }
    int32_t res = (int32_t) h[0];
    if (res != 0) { close(s); printf("HELLO NG result %d\n", res); return 1; }
    if (h[1] < 276 || h[1] > (16u << 20)) { close(s); printf("HELLO NG short\n"); return 1; }
    uint8_t *b = malloc(h[1]);
    if (!b || recv_all(s, b, h[1]) != 1) { close(s); free(b); printf("HELLO NG closed\n"); return 1; }
    close(s);
    char name[257];
    memcpy(name, b + 20, 256);
    name[256] = 0;
    unsigned long long mb = 0;
    if (h[1] >= 1316) {
        uint32_t nh;
        memcpy(&nh, b + 1304, 4);
        for (uint32_t i = 0; i < nh && i < 16 && 1316 + 16 * i + 4 <= h[1]; i++) {
            uint64_t size;
            uint32_t flags;
            memcpy(&size, b + 1308 + 16 * i, 8);
            memcpy(&flags, b + 1316 + 16 * i, 4);
            if (flags & 1) mb += size >> 20;
        }
    }
    free(b);
    printf("HELLO OK mem_mb=%llu name=%s\n", mb, name);
    return 0;
}

// USB の太さ（app/pc/usb_bw.py と同じ約束）。頼み：向き（u32。0 は iPhone→PC・1 は PC→iPhone）と量（u64）
static int bw_xfer(int s, uint32_t dir, uint64_t n, char *buf, size_t bn) {
    uint8_t h[12];
    memcpy(h, &dir, 4);
    memcpy(h + 4, &n, 8);
    if (send_all(s, h, 12) != 1) return 0;
    uint64_t left = n;
    while (left) {
        size_t k = left < bn ? (size_t) left : bn;
        if ((dir == 0 ? recv_all(s, buf, k) : send_all(s, buf, k)) != 1) return 0;
        left -= k;
    }
    if (dir == 1) {
        uint64_t back;
        if (recv_all(s, &back, 8) != 1 || back != n) return 0;
    }
    return 1;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *) a, y = *(const double *) b; return x < y ? -1 : x > y; }
static int cmd_bw(long long id, int mb) {
    const char *why = NULL;
    int s = connect_phone(id, BW_PORT, &why, NULL);
    if (s < 0) { printf("BW NG %s\n", why); return 1; }
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    set_timeout(s, 20000);
    size_t bn = 1 << 20;
    char *buf = calloc(1, bn);
    double t[20];
    int ok = buf != NULL;
    for (int i = 0; ok && i < 20; i++) {
        double t0 = now_ms();
        ok = bw_xfer(s, 0, 1, buf, bn);
        t[i] = now_ms() - t0;
    }
    ok = ok && bw_xfer(s, 0, 2 << 20, buf, bn) && bw_xfer(s, 1, 2 << 20, buf, bn);
    double down = 0, up = 0;
    if (ok) {
        uint64_t n = (uint64_t) mb << 20;
        double t0 = now_ms();
        ok = bw_xfer(s, 0, n, buf, bn);
        down = mb / ((now_ms() - t0) / 1000.0);
        t0 = now_ms();
        ok = ok && bw_xfer(s, 1, n, buf, bn);
        up = mb / ((now_ms() - t0) / 1000.0);
    }
    close(s);
    free(buf);
    if (!ok) { printf("BW NG dropped\n"); return 1; }
    qsort(t, 20, sizeof *t, cmp_d);
    printf("BW OK down=%.0f up=%.0f rtt_ms=%.2f\n", down, up, t[10]);
    return 0;
}

static void *open_vulkan(void) {
    const char *alt = getenv("PGPU_LIBVULKAN"); // 試験用
    void *h = dlopen(alt && *alt ? alt : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return h;
}

static int cmd_loader(void) {
    void *h = open_vulkan();
    if (!h) { printf("LOADER NG missing\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkEnumerateInstanceVersion ev = gipa ? (PFN_vkEnumerateInstanceVersion) gipa(NULL, "vkEnumerateInstanceVersion") : NULL;
    uint32_t v = VK_API_VERSION_1_0;
    if (ev) ev(&v);
    Dl_info di;
    const char *file = gipa && dladdr((void *) gipa, &di) && di.dli_fname ? di.dli_fname : "libvulkan.so.1";
    printf("LOADER OK %u.%u.%u %s\n", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), file);
    return 0;
}

static int cmd_vklist(void) {
    void *h = open_vulkan();
    if (!h) { printf("libvulkan.so.1 が無い\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "pgpu-probe", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = create(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { printf("vkCreateInstance %d\n", r); return 1; }
    PFN_vkEnumeratePhysicalDevices enumd = (PFN_vkEnumeratePhysicalDevices) gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties) gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memp = (PFN_vkGetPhysicalDeviceMemoryProperties) gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance) gipa(inst, "vkDestroyInstance");
    uint32_t n = 0;
    r = enumd(inst, &n, NULL);
    if (r != VK_SUCCESS) { printf("vkEnumeratePhysicalDevices %d\n", r); destroy(inst, NULL); return 1; }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    enumd(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceMemoryProperties m;
        props(devs[i], &p);
        memp(devs[i], &m);
        unsigned long long heap = 0;
        for (uint32_t k = 0; k < m.memoryHeapCount; k++)
            if (m.memoryHeaps[k].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap += m.memoryHeaps[k].size;
        printf("GPU%u: %s  (vendor 0x%04x, type %d, Vulkan %u.%u, %llu MiB)\n", i, p.deviceName, p.vendorID, p.deviceType,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), heap >> 20);
    }
    fflush(stdout);
    destroy(inst, NULL);
    return n ? 0 : 1;
}

// ---- 書き込み監視が使えるか（icd.c の ww_init と同じ確かめ方） ----
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
struct ww_region { uint64_t start, end, categories; };
struct ww_scan {
    uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages;
    uint64_t category_inverted, category_mask, category_anyof_mask, return_mask;
};
#define WW_PAGEMAP_SCAN _IOWR('f', 16, struct ww_scan)
static int cmd_ww(void) {
    long pg = sysconf(_SC_PAGESIZE);
    int u = (int) syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (u < 0 && errno == EINVAL) u = (int) syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (u < 0) { printf("WW NG %s\n", errno == EPERM ? "userfaultfd-denied" : "no-userfaultfd"); return 1; }
    struct uffdio_api api = { .api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED };
    if (ioctl(u, UFFDIO_API, &api) != 0 || !(api.features & UFFD_FEATURE_WP_ASYNC)) { close(u); printf("WW NG no-wp-async\n"); return 1; }
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0) { close(u); printf("WW NG no-pagemap\n"); return 1; }
    uint8_t *t = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const char *why = NULL;
    if (t == MAP_FAILED) why = "mmap";
    else {
        struct uffdio_register rg = { .range = { (uint64_t) (uintptr_t) t, 4 * pg }, .mode = UFFDIO_REGISTER_MODE_WP };
        struct uffdio_writeprotect wp = { .range = { (uint64_t) (uintptr_t) t, 4 * pg }, .mode = UFFDIO_WRITEPROTECT_MODE_WP };
        if (ioctl(u, UFFDIO_REGISTER, &rg) != 0 || ioctl(u, UFFDIO_WRITEPROTECT, &wp) != 0) why = "register";
        else {
            t[pg + 5] = 1;
            struct ww_region reg[8];
            struct ww_scan a = { .size = sizeof a, .flags = 3, .start = (uint64_t) (uintptr_t) t, .end = (uint64_t) (uintptr_t) (t + 4 * pg),
                                 .vec = (uint64_t) (uintptr_t) reg, .vec_len = 8, .category_mask = 2, .return_mask = 2 };
            long n = ioctl(pm, WW_PAGEMAP_SCAN, &a);
            if (n < 0) why = "no-pagemap-scan";
            else if (n != 1 || reg[0].start != (uint64_t) (uintptr_t) (t + pg) || reg[0].end != (uint64_t) (uintptr_t) (t + 2 * pg)) why = "wrong-pages";
        }
        munmap(t, 4 * pg);
    }
    close(pm);
    close(u);
    if (why) { printf("WW NG %s\n", why); return 1; }
    printf("WW OK\n");
    return 0;
}

// ---- この PC の USB に Apple の iPhone・iPad が見えるか（usbmuxd が見ていなくても、つないであるかを分けるため） ----
static int read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(out, (int) n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}
static int cmd_usb(void) {
    const char *root = getenv("PGPU_SYS_USB"); // 試験用
    if (!root || !*root) root = "/sys/bus/usb/devices";
    DIR *d = opendir(root);
    int n = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[512], v[32], pr[32], sp[32];
            snprintf(p, sizeof p, "%s/%s/idVendor", root, e->d_name);
            if (!read_line(p, v, sizeof v) || strcmp(v, "05ac")) continue;
            snprintf(p, sizeof p, "%s/%s/idProduct", root, e->d_name);
            if (!read_line(p, pr, sizeof pr) || strncmp(pr, "12", 2)) continue;
            snprintf(p, sizeof p, "%s/%s/speed", root, e->d_name);
            if (!read_line(p, sp, sizeof sp)) snprintf(sp, sizeof sp, "?");
            printf("USBDEV 05ac:%s %s\n", pr, sp);
            n++;
        }
        closedir(d);
    }
    printf("USB %d\n", n);
    return n ? 0 : 1;
}

// ---- 受け渡し役 ----
static volatile sig_atomic_t g_quit;
static void on_term(int sig) { (void) sig; g_quit = 1; }

static void broker_serve(int c, int quiet) {
    struct ucred cr;
    socklen_t cl = sizeof cr;
    char ok = 'E';
    int fd = -1;
    // 同じ利用者のプロセスにだけ渡す（名前だけの socket には持ち主の権限が無いため）
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.uid == getuid()) {
        char where[320];
        fd = mux_open(where, sizeof where, 0);
        if (fd >= 0) ok = 'K';
        if (!quiet) fprintf(stderr, "[pgpu-probe] pid %d に %s\n", (int) cr.pid, fd >= 0 ? "usbmuxd の口を渡した" : "usbmuxd につながらないと返した");
    } else if (!quiet) fprintf(stderr, "[pgpu-probe] 別の利用者からの頼みを断った\n");
    struct iovec iov = { &ok, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct msghdr m = { 0 };
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    if (fd >= 0) {
        m.msg_control = u.buf;
        m.msg_controllen = sizeof u.buf;
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level = SOL_SOCKET;
        h->cmsg_type = SCM_RIGHTS;
        h->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof fd);
    }
    set_timeout(c, 2000);
    sendmsg(c, &m, MSG_NOSIGNAL);
    if (fd >= 0) close(fd); // 渡した側の写しは閉じる（もらった側の口は残る）
    close(c);
}

static int cmd_broker(int quiet) {
    char name[64];
    broker_name(name, sizeof name);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, name, strlen(name));
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { printf("BROKER NG socket\n"); return 1; }
    if (bind(s, (struct sockaddr *) &a, (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))) != 0) {
        printf(errno == EADDRINUSE ? "BROKER BUSY @%s\n" : "BROKER NG bind\n", name);
        close(s);
        return errno == EADDRINUSE ? 0 : 1;
    }
    listen(s, 16);
    // 起こした親（run-on-iphone-gpu.sh やターミナルのシェル）が終わったら一緒に終わる
    pid_t parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    printf("BROKER READY @%s\n", name);
    fflush(stdout);
    while (!g_quit) {
        struct pollfd pf = { s, POLLIN, 0 };
        int r = poll(&pf, 1, 1000);
        if (r <= 0) continue;
        int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
        if (c >= 0) broker_serve(c, quiet);
    }
    close(s);
    return 0;
}

static int cmd_broker_check(void) {
    char name[64];
    broker_name(name, sizeof name);
    int s = unix_connect(name, 1);
    if (s < 0) { printf("BROKER NONE @%s\n", name); return 1; }
    close(s); // 何も頼まずに閉じる（受け渡し役は 1 回分の usbmuxd の口を作って閉じる）
    printf("BROKER OK @%s\n", name);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "使い方：pgpu-probe mux|devices|pair <番号>|hello <番号> [proto] [ms] [port]|bw <番号> [MB]|loader|vklist|ww|usb|broker [--quiet]|broker-check\n");
    return 2;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "mux")) return cmd_mux();
    if (!strcmp(c, "devices")) return cmd_devices();
    if (!strcmp(c, "pair") && argc >= 3) return cmd_pair(atoll(argv[2]));
    if (!strcmp(c, "hello") && argc >= 3)
        return cmd_hello(atoll(argv[2]), argc >= 4 ? (unsigned) atoi(argv[3]) : 3, argc >= 5 ? atoi(argv[4]) : 3000, argc >= 6 ? atoi(argv[5]) : RECV_PORT);
    if (!strcmp(c, "bw") && argc >= 3) return cmd_bw(atoll(argv[2]), argc >= 4 ? atoi(argv[3]) : 32);
    if (!strcmp(c, "loader")) return cmd_loader();
    if (!strcmp(c, "vklist")) return cmd_vklist();
    if (!strcmp(c, "ww")) return cmd_ww();
    if (!strcmp(c, "usb")) return cmd_usb();
    if (!strcmp(c, "broker")) return cmd_broker(argc >= 3 && !strcmp(argv[2], "--quiet"));
    if (!strcmp(c, "broker-check")) return cmd_broker_check();
    return usage();
}
