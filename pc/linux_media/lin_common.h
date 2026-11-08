// webcam_rx・ipaudio の Linux 版が共に使う小さな道具：時計・記録・TCP・usbmuxd（iPhone の一覧と、iPhone の番号につなぐ）。
// usbmuxd の口はドライバ（vk/icd/icd.c の mux_open）と同じ順に決める：
//   --mux か IPHONE_VK_MUX（番号なら 127.0.0.1 の TCP・/パス か unix:パス なら Unix socket）→ USBMUXD_SOCKET_ADDRESS → /var/run/usbmuxd
// このプログラムは外（ホスト）で動くので、受け渡し役（pgpu-probe broker）は使わない
#pragma once
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static inline double lc_now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec * 1e6 + (double) t.tv_nsec / 1e3;
}

static inline void lc_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

static inline void lc_set_timeout(int s, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static inline int lc_send_all(int s, const void *p, size_t n) {
    const char *c = (const char *) p;
    while (n) {
        ssize_t k = send(s, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        c += k, n -= (size_t) k;
    }
    return 1;
}

static inline int lc_recv_all(int s, void *p, size_t n) {
    char *c = (char *) p;
    while (n) {
        ssize_t k = recv(s, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        c += k, n -= (size_t) k;
    }
    return 1;
}

static inline int lc_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) != 0) { close(s); s = -1; }
    freeaddrinfo(res);
    if (s >= 0) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); }
    return s;
}

static inline int lc_unix_connect(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path));
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *) &a, sizeof a) != 0) { close(s); return -1; }
    return s;
}

// "住所:番号" を分ける。番号だけなら住所は 127.0.0.1
static inline void lc_split_hostport(const char *a, char *host, size_t hn, char *port, size_t pn, const char *dport) {
    snprintf(host, hn, "127.0.0.1");
    snprintf(port, pn, "%s", dport);
    if (!a || !*a) return;
    const char *c = strrchr(a, ':');
    if (c) {
        size_t n = (size_t) (c - a);
        if (n >= hn) n = hn - 1;
        if (n) { memcpy(host, a, n); host[n] = 0; }
        snprintf(port, pn, "%s", c + 1);
    } else {
        int digits = 1;
        for (const char *p = a; *p; p++) if (*p < '0' || *p > '9') digits = 0;
        if (digits) snprintf(port, pn, "%s", a);
        else snprintf(host, hn, "%s", a);
    }
}

// ---- usbmuxd ----
static const char *g_lc_mux; // --mux（無ければ環境変数）

static inline int lc_mux_open(char *where, size_t nw) {
    const char *a = g_lc_mux && *g_lc_mux ? g_lc_mux : getenv("IPHONE_VK_MUX");
    if (!a || !*a) a = getenv("USBMUXD_SOCKET_ADDRESS");
    if (!a || !*a) a = "/var/run/usbmuxd";
    const char *path = !strncmp(a, "UNIX:", 5) || !strncmp(a, "unix:", 5) ? a + 5 : a[0] == '/' ? a : NULL;
    if (path) {
        snprintf(where, nw, "%s", path);
        return lc_unix_connect(path);
    }
    char host[256], port[16];
    lc_split_hostport(a, host, sizeof host, port, sizeof port, "27015");
    snprintf(where, nw, "%s:%s", host, port);
    return lc_tcp_connect(host, port);
}

static inline char *lc_mux_call(int s, const char *type, const char *extra) {
    char body[1024];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>pocketgpu-media</string><key>ProgName</key><string>pocketgpu-media</string>%s</dict></plist>\n",
        type, extra);
    if (n <= 0 || n >= (int) sizeof body) return NULL;
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    lc_set_timeout(s, 5000);
    if (!lc_send_all(s, h, sizeof h) || !lc_send_all(s, body, (size_t) n)) return NULL;
    if (!lc_recv_all(s, h, sizeof h) || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = (char *) malloc(h[0] - 16 + 1);
    if (!r) return NULL;
    if (!lc_recv_all(s, r, h[0] - 16)) { free(r); return NULL; }
    r[h[0] - 16] = 0;
    return r;
}

static inline long long lc_plist_int(const char *at, const char *key, const char *limit) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    if (!q || (limit && q > limit)) return -1;
    q = strstr(q, "<integer>");
    return q && (!limit || q < limit) ? atoll(q + 9) : -1;
}
static inline int lc_plist_str(const char *at, const char *key, const char *limit, char *out, size_t cap) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    out[0] = 0;
    if (!q || (limit && q > limit)) return 0;
    q = strstr(q + strlen(k), "<string>");
    if (!q || (limit && q > limit)) return 0;
    q += 8;
    const char *e = strstr(q, "</string>");
    if (!e) return 0;
    size_t n = (size_t) (e - q);
    if (n >= cap) n = cap - 1;
    memcpy(out, q, n);
    out[n] = 0;
    return 1;
}

typedef struct { long long id, product; char udid[64], type[16]; } lc_dev;

static inline int lc_mux_list(lc_dev *out, int max, char *where, size_t nw) {
    int s = lc_mux_open(where, nw);
    if (s < 0) return -1;
    char *list = lc_mux_call(s, "ListDevices", "");
    close(s);
    if (!list) return -1;
    int n = 0;
    for (const char *q = list; n < max && (q = strstr(q, "<key>Properties</key>")); q++) {
        const char *end = strstr(q, "</dict>");
        lc_dev d;
        memset(&d, 0, sizeof d);
        d.id = lc_plist_int(q, "DeviceID", end);
        d.product = lc_plist_int(q, "ProductID", end);
        lc_plist_str(q, "ConnectionType", end, d.type, sizeof d.type);
        lc_plist_str(q, "SerialNumber", end, d.udid, sizeof d.udid);
        if (d.id >= 0) out[n++] = d;
    }
    free(list);
    return n;
}

// UDID が want と合うか。大文字・小文字と「-」は見ない。6 文字以上なら前の方の一致でよい（webcam_rx・ipaudio の Windows 版と同じ決まり）
static inline int lc_udid_match(const char *udid, const char *want) {
    size_t i = 0, j = 0, hit = 0;
    for (;;) {
        while (want[j] == '-') j++;
        while (udid[i] == '-') i++;
        if (!want[j]) return hit >= 6;
        if (!udid[i]) return 0;
        char a = udid[i], b = want[j];
        if (a >= 'a' && a <= 'z') a = (char) (a - 32);
        if (b >= 'a' && b <= 'z') b = (char) (b - 32);
        if (a != b) return 0;
        i++, j++, hit++;
    }
}

// USB の iPhone の port 番につなぐ。want：NULL か空なら USB の最初の 1 台・数字だけなら DeviceID・それ以外は UDID。だめなら -1
static inline int lc_usb_connect(int port, const char *want, int quiet) {
    lc_dev d[32];
    char where[300];
    int n = lc_mux_list(d, 32, where, sizeof where);
    if (n < 0) { if (!quiet) fprintf(stderr, "usbmuxd（%s）につながらないか、一覧を返さない\n", where); return -1; }
    int digits = want && *want;
    for (const char *p = want; p && *p; p++) if (*p < '0' || *p > '9') digits = 0;
    long long dev = -1, first = -1;
    int usb = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(d[i].type, "USB")) continue;
        usb++;
        if (first < 0) first = d[i].id;
        if (want && *want && dev < 0 && ((digits && d[i].id == atoll(want)) || (!digits && lc_udid_match(d[i].udid, want)))) dev = d[i].id;
    }
    if (want && *want && dev < 0) { if (!quiet) fprintf(stderr, "USB でつながった iPhone の中に「%s」が無い（--list で一覧を見る）\n", want); return -1; }
    if (!want || !*want) {
        dev = first;
        if (usb > 1 && !quiet) fprintf(stderr, "USB の iPhone が %d 台ある。最初の 1 台（DeviceID %lld）を使う。選ぶときは --device DeviceID か UDID\n", usb, first);
    }
    if (dev < 0) { if (!quiet) fprintf(stderr, "USB でつながった iPhone が見えない\n"); return -1; }
    int s = lc_mux_open(where, sizeof where);
    if (s < 0) return -1;
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>", dev, (unsigned) htons((uint16_t) port));
    char *res = lc_mux_call(s, "Connect", extra);
    long long num = res ? lc_plist_int(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { close(s); if (!quiet) fprintf(stderr, "iPhone の %d 番につながらない（usbmuxd の返事 %lld）\n", port, num); return -1; }
    lc_set_timeout(s, 0);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); // Unix socket なら効かないだけ
    return s;
}

// --list：usbmuxd が見ている iPhone の一覧
static inline int lc_list_devices(void) {
    lc_dev d[32];
    char where[300];
    int n = lc_mux_list(d, 32, where, sizeof where);
    if (n < 0) { printf("usbmuxd（%s）につながりません\n", where); return 1; }
    printf("DeviceID\t種類\tProductID\tUDID\n");
    for (int i = 0; i < n; i++) printf("%lld\t%s\t%lld\t%s\n", d[i].id, d[i].type[0] ? d[i].type : "?", d[i].product, d[i].udid[0] ? d[i].udid : "?");
    if (!n) printf("（つながっている iPhone はありません）\n");
    return 0;
}
