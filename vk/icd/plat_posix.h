// icd.c が Windows の API をそのまま呼んでいる所のうち、Linux では薄い置き換えで済む物（Windows では読み込まない）。
//   錠（SRWLOCK）・条件変数・糸・不可分の足し算・待つ・socket の名前・書き出しの印・型の名前・ファイルがあるか
//
// ---- icd.c の Windows に依る所と、Linux 版での扱い ----
//  つなぐ所      Winsock（WSAStartup・WSAPoll・ioctlsocket・closesocket・DWORD の SO_RCVTIMEO・select(0, ...)）
//                → 名前はこのファイル、受信の待ち・non-blocking・connect の確かめは icd.c の sock_rcvtimeo・sock_nonblock・tcp_connect で分ける
//                usbmuxd：Windows は 127.0.0.1:27015 の TCP（Apple Mobile Device Service）、Linux は Unix socket（/var/run/usbmuxd）。icd.c の mux_open で分ける
//  錠と糸        SRWLOCK・Interlocked → pthread_rwlock と __atomic。窓に描く糸の CreateThread・CONDITION_VARIABLE・WaitForSingleObject → このファイル（pthread）
//  時計          QueryPerformanceCounter（now_ms）→ clock_gettime。Sleep → nanosleep。GetProcessTimes → getrusage。糸の CPU 時間 → CLOCK_THREAD_CPUTIME_ID
//                timeBeginPeriod は何もしない（Linux の nanosleep はもともと細かい）・YieldProcessor → sched_yield
//  メモリの写し  VirtualAlloc(MEM_WRITE_WATCH)・GetWriteWatch・ResetWriteWatch → mmap と userfaultfd の非同期の書き込み禁止＋PAGEMAP_SCAN（icd.c の ww_）。
//                使えない核では、全ページを「書いたかもしれない」とし、受け手の中身の写し（m->sent）との違いだけを送る。
//                読み戻しの遅延（VirtualProtect・PAGE_NOACCESS・例外で読む。約 530 行）は外す
//  窓と絵を出す  Win32 の surface・GDI・WIC の JPEG → X11（VK_KHR_xcb_surface・VK_KHR_xlib_surface）と libturbojpeg か stb_image（wsi_x11.inc）。
//                窓のバナー・モニターへ動かす・広げた絵の別窓は Windows だけ
//  動画          Media Foundation の H.264 とそれを GPU でほどく D3D11（約 330 行）・video.h の Windows の部分 → 外す（窓の絵は JPEG）
//  ファイル      シェーダーを書き出す調べ用（IPHONE_VK_SPV_DUMP）と録画の持ち帰りの区切りの文字（PATH_SEP）。MAX_PATH・DeleteFileA・GetFileAttributesA は調べ用の制御ファイルを使う所のため
//  環境変数      getenv はそのまま。VKD3D_FEATURE_LEVEL の取得と設定だけ Windows の API → getenv・setenv
//  dll の読み込み  __declspec(dllexport) → visibility("default")（組むとき -fvisibility=hidden）。
//                GetModuleHandleEx(PIN) → dladdr と dlopen(RTLD_NODELETE)。32 ビットの stdcall 名の .def は Linux に無い
//  例外の記録係  AddVectoredExceptionHandler と dbghelp → sigaction と backtrace（IPHONE_VK_LOG=1 のとき）
#pragma once
#ifndef _WIN32
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <wchar.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
// 窓（swapchain）だけが使う関数は、Linux では使われない
#define LINUX_UNUSED __attribute__((unused))
#define PATH_SEP "/"

typedef int32_t LONG;
typedef int64_t LONG64;
typedef uint32_t DWORD;
typedef void *HWND;

// ---- 錠：SRWLOCK は pthread の読み書き錠で置き換える ----
typedef pthread_rwlock_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void InitializeSRWLock(SRWLOCK *l) { pthread_rwlock_init(l, NULL); }
static inline void AcquireSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_wrlock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *l) { pthread_rwlock_unlock(l); }
static inline void AcquireSRWLockShared(SRWLOCK *l) { pthread_rwlock_rdlock(l); }
static inline void ReleaseSRWLockShared(SRWLOCK *l) { pthread_rwlock_unlock(l); }

// ---- 不可分の演算（Windows の Interlocked* と同じく、返すのは Exchange・Or が前の値、Increment・Decrement・Add が後の値） ----
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedAdd64(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)

// ---- 待つ ----
static inline void Sleep(DWORD ms) {
    struct timespec ts = { (time_t) (ms / 1000), (long) (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// ---- ファイル：パスの長さと、ファイルを消す（調べ用の制御ファイルを使う所が呼ぶ） ----
#define MAX_PATH 4096
static inline int DeleteFileA(const char *path) { return unlink(path) == 0; }

// ---- socket：Winsock の名前を POSIX の物に読み替える ----
#define INVALID_SOCKET VR_BAD_SOCK
#define closesocket close
#define WSAPOLLFD struct pollfd
#define WSAPoll(fds, n, ms) poll((fds), (nfds_t) (n), (ms))

// ---- 窓に描く糸のための、糸と条件変数（Windows の CreateThread・CONDITION_VARIABLE の形のまま使う） ----
typedef void *HANDLE;
typedef void *LPVOID;
#define WINAPI
#define INFINITE 0xffffffffu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u

// 条件変数：SRWLOCK（pthread の読み書き錠）と組むので、pthread_cond を中の mutex と回数で包む。
// 待つ側は、錠を持ったまま中の mutex を取ってから錠を放すので、起こす側（錠を持って Wake する）との間で知らせを落とさない
typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned seq; } CONDITION_VARIABLE;
static inline void InitializeConditionVariable(CONDITION_VARIABLE *cv) {
    pthread_mutex_init(&cv->m, NULL);
    pthread_cond_init(&cv->c, NULL);
    cv->seq = 0;
}
static inline int SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, unsigned long flags) {
    (void) flags;
    pthread_mutex_lock(&cv->m);
    unsigned s = cv->seq;
    pthread_rwlock_unlock(l);
    int r = 0;
    if (ms == INFINITE) {
        while (cv->seq == s && r == 0) r = pthread_cond_wait(&cv->c, &cv->m);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long) (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (cv->seq == s && r == 0) r = pthread_cond_timedwait(&cv->c, &cv->m, &ts);
    }
    pthread_mutex_unlock(&cv->m);
    pthread_rwlock_wrlock(l);
    return r == 0;
}
static inline void WakeConditionVariable(CONDITION_VARIABLE *cv) {
    pthread_mutex_lock(&cv->m);
    cv->seq++;
    pthread_cond_signal(&cv->c);
    pthread_mutex_unlock(&cv->m);
}

// 糸：CreateThread の形で pthread を作る。終わるときに自分の CPU 時間を覚える（thread_cpu_ms で読む）。
// CloseHandle は、待ち終えていれば片づけ、まだ走っていれば切り離して、糸が終わるときに自分で片づけさせる
struct plat_thread {
    pthread_t t;
    DWORD (*fn)(LPVOID);
    LPVOID arg;
    double cpu_ms;
    int joined;
    volatile int state; // 0 走っている・1 終わった・2 CloseHandle が先に来た（終わったら自分で片づける）
};
static void *plat_thread_main(void *p) {
    struct plat_thread *h = (struct plat_thread *) p;
    h->fn(h->arg);
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) h->cpu_ms = (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
    if (__atomic_exchange_n(&h->state, 1, __ATOMIC_SEQ_CST) == 2) free(h);
    return NULL;
}
static inline HANDLE CreateThread(void *sa, size_t stack, DWORD (*fn)(LPVOID), LPVOID arg, DWORD flags, DWORD *id) {
    (void) sa; (void) stack; (void) flags; (void) id;
    struct plat_thread *h = (struct plat_thread *) calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fn = fn;
    h->arg = arg;
    if (pthread_create(&h->t, NULL, plat_thread_main, h) != 0) { free(h); return NULL; }
    return h;
}
// 糸の終わりを待つ（糸のハンドルにだけ使う）
static inline DWORD WaitForSingleObject(HANDLE hh, DWORD ms) {
    struct plat_thread *h = (struct plat_thread *) hh;
    if (!h || h->joined) return WAIT_OBJECT_0;
    if (ms == INFINITE) {
        pthread_join(h->t, NULL);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += ms / 1000;
        ts.tv_nsec += (long) (ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_timedjoin_np(h->t, NULL, &ts) != 0) return WAIT_TIMEOUT;
    }
    h->joined = 1;
    return WAIT_OBJECT_0;
}
static inline int CloseHandle(HANDLE hh) {
    struct plat_thread *h = (struct plat_thread *) hh;
    if (!h) return 0;
    if (h->joined) { free(h); return 1; }
    pthread_detach(h->t);
    if (__atomic_exchange_n(&h->state, 2, __ATOMIC_SEQ_CST) == 1) free(h);
    return 1;
}
// 終わった糸が使った CPU 時間（ms）。WaitForSingleObject で待ち終えてから読む
static inline double thread_cpu_ms(HANDLE hh) {
    struct plat_thread *h = (struct plat_thread *) hh;
    return h && h->joined ? h->cpu_ms : 0;
}

// ---- 時計の細かさ（Linux の nanosleep はもともと細かいので何もしない）・待つ間の一休み ----
static inline unsigned timeBeginPeriod(unsigned ms) { (void) ms; return 0; }
#define YieldProcessor() sched_yield()

// ---- 録画の印のファイルがあるか・大文字小文字を区別しない比べ ----
#define INVALID_FILE_ATTRIBUTES ((DWORD) -1)
static inline DWORD GetFileAttributesA(const char *p) { return access(p, F_OK) == 0 ? 0x80u : INVALID_FILE_ATTRIBUTES; }
#define _stricmp strcasecmp

// ---- 環境変数（VKD3D_FEATURE_LEVEL を聞く・入れる所が Windows の名前で呼ぶ） ----
static inline DWORD GetEnvironmentVariableA(const char *n, char *buf, DWORD size) {
    const char *v = getenv(n);
    if (!v) return 0;
    size_t l = strlen(v);
    if (buf && size > l) { memcpy(buf, v, l + 1); return (DWORD) l; }
    return (DWORD) (l + 1);
}
static inline int SetEnvironmentVariableA(const char *n, const char *v) { return v ? setenv(n, v, 1) == 0 : unsetenv(n) == 0; }
#endif
