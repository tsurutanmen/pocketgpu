// icd.c が Windows の API をそのまま呼んでいる所のうち、Linux では薄い置き換えで済む物（Windows では読み込まない）。
//   錠（SRWLOCK）・不可分の足し算・待つ・socket の名前・書き出しの印・型の名前
//
// ---- icd.c の Windows に依る所と、Linux 版での扱い（元の icd.c の約 1 万行のうち、Linux 版で外れるのは約 2000 行） ----
//  つなぐ所      Winsock（WSAStartup・WSAPoll・ioctlsocket・closesocket・DWORD の SO_RCVTIMEO・select(0, ...)）
//                → 名前はこのファイル、受信の待ち・non-blocking・connect の確かめは icd.c の sock_rcvtimeo・sock_nonblock・tcp_connect で分ける
//                usbmuxd は 127.0.0.1:27015 の TCP（Apple Mobile Device Service）。Linux の usbmuxd は Unix socket なので、そのままでは届かない
//  錠と糸        SRWLOCK 17 個（約 160 回の取得・解放）・Interlocked 数種 → このファイル（pthread_rwlock と __atomic）
//                CreateThread 3 本・CONDITION_VARIABLE・InitOnce・WaitForSingleObject は全部、窓と動画の中
//  時計          QueryPerformanceCounter（now_ms）→ clock_gettime。Sleep → nanosleep。GetProcessTimes → getrusage
//                timeBeginPeriod・YieldProcessor は窓に出す間隔の調整だけ
//  メモリの写し  VirtualAlloc(MEM_WRITE_WATCH)・GetWriteWatch・ResetWriteWatch → mmap。書き込み監視が無いので、全ページを「書いたかもしれない」とし、
//                受け手の中身の写し（m->sent）との違いだけを送る。読み戻しの遅延（VirtualProtect・PAGE_NOACCESS・例外で読む。約 530 行）は外す
//  窓と絵を出す  Win32 の surface・GDI・窓・モニター・WIC の JPEG・窓のバナー（約 1100 行）→ 外す。VK_KHR_surface と swapchain を名乗らない
//  動画          Media Foundation の H.264 とそれを GPU でほどく D3D11（約 330 行）・video.h の Windows の部分 → 外す
//  ファイル      シェーダーを書き出す調べ用（IPHONE_VK_SPV_DUMP）の区切りの文字だけ（PATH_SEP）。MAX_PATH・DeleteFileA は調べ用の制御ファイルを使う所のため
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
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
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
#endif
