// ドライバ（PC）と受け手（iPhone・試験では PC）の間で、Vulkan の命令を運ぶための小さな道具。
// 送るもの：[u32 命令][u32 長さ][中身]。返事のある命令だけ [i32 結果][u32 長さ][中身] が返る。
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET vr_sock;
#define VR_BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int vr_sock;
#define VR_BAD_SOCK (-1)
#endif

// ---- 書く側 ----
typedef struct { uint8_t *p; size_t n, cap; } vr_buf;

static inline void vr_reserve(vr_buf *b, size_t more) {
    if (b->n + more <= b->cap) return;
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + more) c *= 2;
    b->p = (uint8_t *) realloc(b->p, c);
    b->cap = c;
}
static inline void vr_put(vr_buf *b, const void *d, size_t n) { vr_reserve(b, n); if (n) memcpy(b->p + b->n, d, n); b->n += n; }
static inline void vr_u32(vr_buf *b, uint32_t v) { vr_put(b, &v, 4); }
static inline void vr_u64(vr_buf *b, uint64_t v) { vr_put(b, &v, 8); }
static inline void vr_bytes(vr_buf *b, const void *d, uint32_t n) { vr_u32(b, n); vr_put(b, d, n); }
static inline void vr_str(vr_buf *b, const char *s) { vr_bytes(b, s, s ? (uint32_t) strlen(s) + 1 : 0); }

// ---- 読む側 ----
typedef struct { const uint8_t *p; size_t n, at; int bad; } vr_rd;

static inline const void *vr_take(vr_rd *r, size_t n) {
    if (r->bad || r->at + n > r->n) { r->bad = 1; return NULL; }
    const void *q = r->p + r->at;
    r->at += n;
    return q;
}
static inline uint32_t vr_ru32(vr_rd *r) { const void *q = vr_take(r, 4); uint32_t v = 0; if (q) memcpy(&v, q, 4); return v; }
static inline uint64_t vr_ru64(vr_rd *r) { const void *q = vr_take(r, 8); uint64_t v = 0; if (q) memcpy(&v, q, 8); return v; }
static inline const void *vr_rbytes(vr_rd *r, uint32_t *n) { *n = vr_ru32(r); return vr_take(r, *n); }
static inline void vr_rcopy(vr_rd *r, void *dst, size_t n) { const void *q = vr_take(r, n); if (q) memcpy(dst, q, n); else memset(dst, 0, n); }

// ---- つなぐ ----
static inline int vr_send_all(vr_sock s, const void *d, size_t n) {
    const char *p = (const char *) d;
    while (n) {
        int k = send(s, p, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        p += k; n -= (size_t) k;
    }
    return 1;
}
static inline int vr_recv_all(vr_sock s, void *d, size_t n) {
    char *p = (char *) d;
    while (n) {
        int k = recv(s, p, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        p += k; n -= (size_t) k;
    }
    return 1;
}
static inline void vr_nodelay(vr_sock s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof one);
}
