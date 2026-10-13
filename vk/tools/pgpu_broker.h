// usbmuxd の受け渡し役（pgpu-probe broker）と、口をもらう側（ドライバ icd.c・pgpu-probe）の間のあいさつ。Linux だけ。両方が読む。
//
// 受け渡し役は名前だけの Unix socket（@名前）で待つ。名前だけの socket はファイルの権限で守れず、同じ網の名前空間の誰でもつなげる。
// 同じ利用者の Flatpak のアプリ（網を共有する物）もつなげる。そこで、次の 3 つを両方の側で確かめる：
//   1. つないだ相手の uid（SO_PEERCRED）が自分と同じか。別の利用者が先に名前を取っても、口を渡さない・もらわない
//   2. 合言葉を両方が知っているか。合言葉は run-on-iphone-gpu.sh が起動ごとに作り、そのゲームと受け渡し役にだけ環境変数で渡す
//      （PGPU_BROKER_TOKEN・16 進 32 けた）。合言葉そのものは送らない。互いの乱数に合言葉で印（SipHash-2-4）を付けて返し合う。
//      偽の受け渡し役には合言葉が漏れず、合言葉を知らない相手には口を渡さない
//   3. もらった fd が socket（SOCK_STREAM・AF_UNIX か AF_INET か AF_INET6）か（fstat と getsockopt）
// 名前：run-on-iphone-gpu.sh が起動ごとに作る名前を PGPU_BROKER で渡す（pocketgpu-usbmuxd-<uid>-<乱数>）。
//       無ければ pocketgpu-usbmuxd-<uid>（./run-on-iphone-gpu.sh --broker を手で動かすとき。Flatpak の Steam 用）
// 流れ（どちらも 1 回だけ。呼ぶ側が socket に 5 秒の時間切れを付けておく）：
//   頼む側 → 役：  "PGB2"・頼み 1 バイト（'G' usbmuxd の口・'P' 生きているかだけ）・乱数 Nc（16）
//   役 → 頼む側：  乱数 Nb（16）・印 MAC(鍵, 'B' 頼み Nc Nb)（8）
//   頼む側 → 役：  印 MAC(鍵, 'C' 頼み Nb Nc)（8）。役の印が合わなければ、送らずに切る
//   役 → 頼む側：  答え 1 バイト（'K' 口を渡す・SCM_RIGHTS つき／'P' 生きている／'E' usbmuxd につながらない／'A' 断った）
#pragma once
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // struct ucred（SO_PEERCRED）。取り込む側が先に system の頭を読んでいれば、そちらで決めておく
#endif
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#define PGB_FN static __attribute__((unused))
#define PGB_MAGIC "PGB2"
#define PGB_TOKEN_ENV "PGPU_BROKER_TOKEN"
#define PGB_NAME_ENV "PGPU_BROKER"

// ---- SipHash-2-4（鍵 16 バイト・出力 8 バイト） ----
PGB_FN uint64_t pgb_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
PGB_FN uint64_t pgb_rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
#define PGB_ROUND                                                                                           \
    do {                                                                                                    \
        v0 += v1; v1 = pgb_rotl(v1, 13); v1 ^= v0; v0 = pgb_rotl(v0, 32);                                   \
        v2 += v3; v3 = pgb_rotl(v3, 16); v3 ^= v2;                                                          \
        v0 += v3; v3 = pgb_rotl(v3, 21); v3 ^= v0;                                                          \
        v2 += v1; v1 = pgb_rotl(v1, 17); v1 ^= v2; v2 = pgb_rotl(v2, 32);                                   \
    } while (0)
PGB_FN uint64_t pgb_siphash(const uint8_t key[16], const uint8_t *in, size_t n) {
    uint64_t k0 = pgb_le64(key), k1 = pgb_le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    size_t full = n - n % 8;
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m = pgb_le64(in + i);
        v3 ^= m;
        PGB_ROUND;
        PGB_ROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t) n << 56;
    for (size_t i = 0; i < n % 8; i++) b |= (uint64_t) in[full + i] << (8 * i);
    v3 ^= b;
    PGB_ROUND;
    PGB_ROUND;
    v0 ^= b;
    v2 ^= 0xff;
    PGB_ROUND;
    PGB_ROUND;
    PGB_ROUND;
    PGB_ROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
#undef PGB_ROUND

// 印：MAC(鍵, 向き 頼み a b)
PGB_FN uint64_t pgb_mac(const uint8_t key[16], char dir, char op, const uint8_t a[16], const uint8_t b[16]) {
    uint8_t m[34];
    m[0] = (uint8_t) dir, m[1] = (uint8_t) op;
    memcpy(m + 2, a, 16);
    memcpy(m + 18, b, 16);
    return pgb_siphash(key, m, sizeof m);
}

PGB_FN int pgb_random(uint8_t *p, size_t n) {
#ifdef SYS_getrandom
    if (syscall(SYS_getrandom, p, n, 0) == (long) n) return 1;
#endif
    int f = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (f < 0) return 0;
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(f, p + got, n - got);
        if (k <= 0) break;
        got += (size_t) k;
    }
    close(f);
    return got == n;
}

// 合言葉（16 進 32 けた）を鍵にする。無いか形が違えば 0
PGB_FN int pgb_key_parse(const char *s, uint8_t key[16]) {
    if (!s || strlen(s) != 32) return 0;
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
            else return 0;
        }
        key[i] = (uint8_t) v;
    }
    return 1;
}
PGB_FN int pgb_key_from_env(uint8_t key[16]) { return pgb_key_parse(getenv(PGB_TOKEN_ENV), key); }

PGB_FN int pgb_same8(uint64_t a, const uint8_t *b) {
    uint8_t x[8];
    for (int i = 0; i < 8; i++) x[i] = (uint8_t) (a >> (8 * i));
    volatile uint8_t d = 0;
    for (int i = 0; i < 8; i++) d |= (uint8_t) (x[i] ^ b[i]);
    return d == 0;
}
PGB_FN void pgb_put8(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i));
}

PGB_FN int pgb_send(int s, const void *p, size_t n) {
    const uint8_t *c = (const uint8_t *) p;
    while (n) {
        ssize_t k = send(s, c, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        c += k, n -= (size_t) k;
    }
    return 1;
}
PGB_FN int pgb_recv(int s, void *p, size_t n) {
    uint8_t *c = (uint8_t *) p;
    while (n) {
        ssize_t k = recv(s, c, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        c += k, n -= (size_t) k;
    }
    return 1;
}

// つないだ相手が同じ利用者か（uid はこちらの利用者の名前空間で見た値。写していない uid は 65534 になり、合わない）
PGB_FN int pgb_peer_is_me(int s) {
    struct ucred cr;
    socklen_t cl = sizeof cr;
    return getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cl == sizeof cr && cr.uid == getuid();
}

// もらった fd が、usbmuxd の口になりうる socket か
PGB_FN int pgb_fd_is_stream_socket(int fd) {
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode)) return 0;
    int type = 0, dom = 0;
    socklen_t l = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &l) != 0 || type != SOCK_STREAM) return 0;
#ifdef SO_DOMAIN
    l = sizeof dom;
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &dom, &l) != 0) return 0;
    if (dom != AF_UNIX && dom != AF_INET && dom != AF_INET6) return 0;
#endif
    return 1;
}

// 名前だけの socket につなぐ（名前は先頭の @ を付けない）
PGB_FN int pgb_connect(const char *name) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (!n || n + 1 > sizeof a.sun_path) { errno = ENAMETOOLONG; return -1; }
    memcpy(a.sun_path + 1, name, n);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (connect(s, (struct sockaddr *) &a, (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n)) != 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    return s;
}

// 頼む側：あいさつして頼む。返すのは役の答え（'K'・'P'・'E'・'A'）か、確かめられなければ 0（why に理由）。
// 'K' なら *fd にもらった口（socket であることを確かめた物）
PGB_FN char pgb_ask(int s, const uint8_t key[16], char op, int *fd, const char **why) {
    *fd = -1;
    *why = "";
    if (!pgb_peer_is_me(s)) { *why = "相手が別の利用者"; return 0; }
    uint8_t req[21], nc[16], resp[24], mine[8], ans = 0;
    memcpy(req, PGB_MAGIC, 4);
    req[4] = (uint8_t) op;
    if (!pgb_random(nc, 16)) { *why = "乱数が取れない"; return 0; }
    memcpy(req + 5, nc, 16);
    if (!pgb_send(s, req, sizeof req) || !pgb_recv(s, resp, sizeof resp)) { *why = "あいさつが通らない（受け渡し役の版が違う）"; return 0; }
    if (!pgb_same8(pgb_mac(key, 'B', op, nc, resp), resp + 16)) { *why = "相手が合言葉を知らない"; return 0; }
    pgb_put8(mine, pgb_mac(key, 'C', op, resp, nc));
    if (!pgb_send(s, mine, 8)) { *why = "あいさつの途中で切れた"; return 0; }
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct iovec iov = { &ans, 1 };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof u.buf;
    ssize_t r;
    do r = recvmsg(s, &m, MSG_CMSG_CLOEXEC); while (r < 0 && errno == EINTR);
    int got = -1;
    if (r == 1 && !(m.msg_flags & MSG_CTRUNC))
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len == CMSG_LEN(sizeof(int))) memcpy(&got, CMSG_DATA(c), sizeof got);
    if (r != 1) { if (got >= 0) close(got); *why = "答えが来ない"; return 0; }
    if (ans != 'K') {
        if (got >= 0) close(got);
        if (ans == 'A') *why = "受け渡し役に断られた";
        return (char) ans;
    }
    if (got < 0) { *why = "口が付いていない"; return 0; }
    if (!pgb_fd_is_stream_socket(got)) { close(got); *why = "もらった物が socket でない"; return 0; }
    *fd = got;
    return 'K';
}

// 受け渡し役の側：相手を確かめる。1 通った（*op に頼み）・0 断る
PGB_FN int pgb_serve_auth(int c, const uint8_t key[16], char *op) {
    if (!pgb_peer_is_me(c)) return 0;
    uint8_t req[21], nb[16], resp[24], theirs[8];
    if (!pgb_recv(c, req, sizeof req) || memcmp(req, PGB_MAGIC, 4)) return 0;
    char o = (char) req[4];
    if (o != 'G' && o != 'P') return 0;
    if (!pgb_random(nb, 16)) return 0;
    memcpy(resp, nb, 16);
    pgb_put8(resp + 16, pgb_mac(key, 'B', o, req + 5, nb));
    if (!pgb_send(c, resp, sizeof resp) || !pgb_recv(c, theirs, 8)) return 0;
    if (!pgb_same8(pgb_mac(key, 'C', o, nb, req + 5), theirs)) return 0;
    *op = o;
    return 1;
}

// 受け渡し役の側：答えを送る（fd が 0 以上なら一緒に渡す）
PGB_FN int pgb_reply(int c, char ans, int fd) {
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    memset(&u, 0, sizeof u);
    struct iovec iov = { &ans, 1 };
    struct msghdr m;
    memset(&m, 0, sizeof m);
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
    ssize_t r;
    do r = sendmsg(c, &m, MSG_NOSIGNAL); while (r < 0 && errno == EINTR);
    return r == 1;
}
#endif
