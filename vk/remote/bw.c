// USB の通り道そのものの速さを測るための受け口。Vulkan とは関係なく、ただバイトを流す。
// PC は app/pc/usb_bw.py でつなぐ。つなぎごとに別のスレッドで受けるので、何本並べても測れる。
//   PC → [u32 向き][u64 バイト数]
//   向き 0：受け手が そのバイト数を送る（iPhone → PC）
//   向き 1：PC が そのバイト数を送り、受け手は受け取り終えたら [u64 受け取った数] を返す（PC → iPhone）
// 一つのつなぎで何回でも続けられる。PC が切ったら終わる。
#include "wire.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define BW_CLOSE closesocket
#else
#include <pthread.h>
#include <poll.h>
#define BW_CLOSE close
#endif

static void bw_client(vr_sock c) {
    enum { CHUNK = 1 << 20 };
    uint8_t *buf = calloc(1, CHUNK);
    int big = 4 << 20, one = 1;
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *) &big, sizeof big);
    setsockopt(c, SOL_SOCKET, SO_RCVBUF, (const char *) &big, sizeof big);
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    for (int i = 0; buf && i < CHUNK; i++) buf[i] = (uint8_t) (i * 131 + 7);
    for (;;) {
        uint8_t h[12];
        if (!buf || !vr_recv_all(c, h, sizeof h)) break;
        uint32_t dir;
        uint64_t n;
        memcpy(&dir, h, 4);
        memcpy(&n, h + 4, 8);
        if (dir == 0) {
            int ok = 1;
            for (uint64_t left = n; left && ok;) {
                size_t k = left < CHUNK ? (size_t) left : CHUNK;
                ok = vr_send_all(c, buf, k);
                left -= k;
            }
            if (!ok) break;
        } else if (dir == 1) {
            uint64_t got = 0;
            while (got < n) {
                size_t want = n - got < CHUNK ? (size_t) (n - got) : CHUNK;
                int k = (int) recv(c, (char *) buf, (int) want, 0);
                if (k <= 0) break;
                got += (uint64_t) k;
            }
            if (got < n || !vr_send_all(c, &got, 8)) break;
        } else {
            break;
        }
    }
    free(buf);
    BW_CLOSE(c);
}

#ifdef _WIN32
static DWORD WINAPI bw_thread(LPVOID p) { bw_client((vr_sock) (uintptr_t) p); return 0; }
#else
static void *bw_thread(void *p) { bw_client((vr_sock) (intptr_t) p); return NULL; }
#endif

// ---- インターネット共有の口（合言葉つき） ----
static uint8_t g_bw_token[16];
static int g_bw_token_ok;
static char g_bw_extra[64];
static volatile vr_sock g_bw_extra_ls = VR_BAD_SOCK;
void vkr_bw_set_token(const char *hex) { g_bw_token_ok = vr_token_parse(hex, g_bw_token); }

#ifdef _WIN32
static DWORD WINAPI bw_extra_thread(LPVOID p) {
#else
static void *bw_extra_thread(void *p) {
#endif
    vr_sock ls = (vr_sock) (uintptr_t) p;
    for (;;) {
#ifndef _WIN32
        // 口を替えた・閉じたときは、この糸が自分の口を閉じて終わる（別の糸が close しても accept が戻らず、口が bind されたまま残ることがあるので、0.5 秒ごとの poll で見る）
        if (ls != g_bw_extra_ls) {
            BW_CLOSE(ls);
            break;
        }
        struct pollfd pf = { .fd = ls, .events = POLLIN, .revents = 0 };
        if (poll(&pf, 1, 500) <= 0) continue;
#endif
        vr_sock c = accept(ls, NULL, NULL);
        if (c == VR_BAD_SOCK) {
#ifdef _WIN32
            break;
#else
            continue;
#endif
        }
        if (!g_bw_token_ok || !vr_link_auth(c, g_bw_token)) { BW_CLOSE(c); continue; }
#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, bw_thread, (LPVOID) (uintptr_t) c, 0, NULL);
        if (t) CloseHandle(t); else BW_CLOSE(c);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, bw_thread, (void *) (intptr_t) c) == 0) pthread_detach(t); else BW_CLOSE(c);
#endif
    }
    return 0;
}

// インターネット共有の口（host）でも待ち始める。同じ口なら何もしない。空の住所なら口を閉じる
int vkr_bw_listen_extra(const char *host, int port) {
    if (!host || !*host) {
#ifdef _WIN32
        if (g_bw_extra_ls != VR_BAD_SOCK) BW_CLOSE(g_bw_extra_ls);
#endif
        // Windows 以外は、待っている糸が気づいて口を閉じる
        g_bw_extra_ls = VR_BAD_SOCK;
        g_bw_extra[0] = 0;
        return 0;
    }
    if (!strcmp(host, g_bw_extra)) return 0;
#ifdef _WIN32
    if (g_bw_extra_ls != VR_BAD_SOCK) BW_CLOSE(g_bw_extra_ls);
#endif
    g_bw_extra_ls = VR_BAD_SOCK;
    g_bw_extra[0] = 0;
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 16) != 0) { BW_CLOSE(ls); return 1; }
    snprintf(g_bw_extra, sizeof g_bw_extra, "%s", host);
    g_bw_extra_ls = ls;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, bw_extra_thread, (LPVOID) (uintptr_t) ls, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, bw_extra_thread, (void *) (uintptr_t) ls) == 0) {
        pthread_detach(t);
    } else {
        BW_CLOSE(ls);
        g_bw_extra_ls = VR_BAD_SOCK;
        g_bw_extra[0] = 0;
        return 1;
    }
#endif
    return 0;
}

// 戻ってこない。別のスレッドで呼ぶ
int vkr_bw_serve(const char *host, int port) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 16) != 0) return 1;
    for (;;) {
        vr_sock c = accept(ls, NULL, NULL);
        if (c == VR_BAD_SOCK) continue;
#ifdef _WIN32
        HANDLE t = CreateThread(NULL, 0, bw_thread, (LPVOID) (uintptr_t) c, 0, NULL);
        if (t) CloseHandle(t); else BW_CLOSE(c);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, bw_thread, (void *) (intptr_t) c) == 0) pthread_detach(t); else BW_CLOSE(c);
#endif
    }
}

#ifdef VR_BW_MAIN
int main(int argc, char **argv) {
    printf("速さを測る受け口：%s:%d\n", argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? atoi(argv[2]) : 50058);
    fflush(stdout);
    // 調べ用：VKR_EXTRA_LISTEN=住所 でインターネット共有の口の真似（VKR_LINK_TOKEN=16 進 32 文字の合言葉）
    const char *ex = getenv("VKR_EXTRA_LISTEN");
    if (ex && *ex) {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        vkr_bw_set_token(getenv("VKR_LINK_TOKEN"));
        vkr_bw_listen_extra(ex, argc > 2 ? atoi(argv[2]) : 50058);
    }
    return vkr_bw_serve(argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? atoi(argv[2]) : 50058);
}
#endif
