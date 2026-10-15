// 速さを測る口（bw.c）を、受け手（server.c）の中から開ける。
// iPhone は、アプリが 127.0.0.1:50058 で既定で待つ（Sources/RpcServerApp.swift）。取り決めは bw.c の上の説明のとおりで、PC は pc/usb_bw.py でつなぐ。
// Android と PC の代役は server.c 1 本だけで組むので、bw.c をここで取り込み、同じ取り決めの口を受け手が自分で開ける。iOS は今までどおり bw.c を別に組むので、この物は使わない。
//   Android：localabstract:pocketgpu-bw で既定で待つ（GPU の命令が通る localabstract:pocketgpu-gpu と同じ形の USB の口）。PC は adb forward tcp:口 localabstract:pocketgpu-bw
//            つないでよいのは adb（uid 2000・0）と受け手の自分の uid だけ。同じ端末のほかのアプリは断る
//   PGPU_BW_UNIX=名前   抽象名を替える。空か 0 で待たない
//   PGPU_BW_TCP=住所:口   TCP でも待つ（iPhone と同じ 127.0.0.1:50058 の形）。既定では待たない。PC の代役では、これで試す
// つなぎごとに別の糸で受けるので、受け手の主の仕事（accept・serve_one）は邪魔しない。受けるのは 12 バイトの頼みだけで、Vulkan には触らない。
// 使う側（server.c）は vlog・JE を先に定義しておく
#pragma once
#include "bw.c"
#include <errno.h>
#include <stdio.h>
#if defined(__ANDROID__) || defined(VR_BW_UNIX)
#define BWL_UNIX 1
#include <sys/un.h>
#endif

typedef struct { vr_sock ls; int check_peer; } bwl_arg;

static void bwl_spawn(vr_sock c) {
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, bw_thread, (LPVOID) (uintptr_t) c, 0, NULL);
    if (t) CloseHandle(t); else BW_CLOSE(c);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, bw_thread, (void *) (intptr_t) c) == 0) pthread_detach(t); else BW_CLOSE(c);
#endif
}

#ifdef _WIN32
static DWORD WINAPI bwl_accept_thread(LPVOID p) {
#else
static void *bwl_accept_thread(void *p) {
#endif
    bwl_arg a = *(bwl_arg *) p;
    free(p);
    for (;;) {
        vr_sock c = accept(a.ls, NULL, NULL);
        if (c == VR_BAD_SOCK) {
#ifdef _WIN32
            Sleep(100);
#else
            if (errno != EINTR) poll(NULL, 0, 100); // fd が足りないなどで失敗し続けても、空回りしない
#endif
            continue;
        }
#ifdef BWL_UNIX
        if (a.check_peer) {
            struct ucred cr;
            socklen_t l = sizeof cr;
            int uid = getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 ? (int) cr.uid : -1;
            if (uid != 2000 && uid != 0 && uid != (int) getuid()) {
                vlog(JE("速さの口：adb ではない接続（uid %d）を断った", "Speed socket: refused a connection that is not from adb (uid %d)"), uid);
                BW_CLOSE(c);
                continue;
            }
        }
#endif
        bwl_spawn(c);
    }
    return 0;
}

static int bwl_run(vr_sock ls, int check_peer) {
    bwl_arg *a = (bwl_arg *) malloc(sizeof *a);
    if (!a) return 0;
    a->ls = ls;
    a->check_peer = check_peer;
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, bwl_accept_thread, a, 0, NULL);
    if (t) { CloseHandle(t); return 1; }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, bwl_accept_thread, a) == 0) { pthread_detach(t); return 1; }
#endif
    free(a);
    return 0;
}

// "住所:口" か "口" で待つ
static void bwl_start_tcp(const char *spec) {
    char host[64] = "127.0.0.1";
    int port = 0;
    const char *colon = strrchr(spec, ':');
    if (colon) {
        size_t n = (size_t) (colon - spec);
        if (n >= sizeof host) n = sizeof host - 1;
        memcpy(host, spec, n);
        host[n] = 0;
        port = atoi(colon + 1);
    } else {
        port = atoi(spec);
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        vlog(JE("速さの口 %s で待てない：住所が読めない", "Cannot listen on the speed socket %s: the address cannot be read"), spec);
        return;
    }
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (ls == VR_BAD_SOCK) return;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 16) != 0 || !bwl_run(ls, 0)) {
        vlog(JE("速さの口 %s:%d で待てない", "Cannot listen on the speed socket %s:%d"), host, port);
        BW_CLOSE(ls);
        return;
    }
    vlog(JE("速さの口：%s:%d で待っている", "Speed socket: listening on %s:%d"), host, port);
}

#ifdef BWL_UNIX
// 抽象名の UNIX の口で待つ（先頭の 0 が抽象名のしるし。Android の LocalServerSocket と adb の localabstract と同じ）
static void bwl_start_unix(const char *name) {
    struct sockaddr_un u;
    size_t n = strlen(name);
    if (!n || n + 1 > sizeof u.sun_path) {
        vlog(JE("速さの口 localabstract:%s で待てない：名前が長すぎる", "Cannot listen on the speed socket localabstract:%s: the name is too long"), name);
        return;
    }
    memset(&u, 0, sizeof u);
    u.sun_family = AF_UNIX;
    memcpy(u.sun_path + 1, name, n);
    socklen_t ul = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *) &u, ul) != 0 || listen(s, 8) != 0 || !bwl_run(s, 1)) {
        vlog(JE("速さの口 localabstract:%s で待てない：%s", "Cannot listen on the speed socket localabstract:%s: %s"), name, strerror(errno));
        if (s >= 0) close(s);
        return;
    }
    vlog(JE("速さの口：localabstract:%s で待っている", "Speed socket: listening on localabstract:%s"), name);
}
#endif

// 受け手が待ち始めたあとに 1 回呼ぶ。待てなくても受け手は続ける（速さを測れないだけ）
static void bwl_start(void) {
    static int started;
    if (started) return;
    started = 1;
    const char *t = getenv("PGPU_BW_TCP");
    if (t && *t && strcmp(t, "0")) bwl_start_tcp(t);
#ifdef BWL_UNIX
    const char *un = getenv("PGPU_BW_UNIX");
    if (!un) un = "pocketgpu-bw";
    if (*un && strcmp(un, "0")) bwl_start_unix(un);
#endif
}
