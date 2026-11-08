// iPhone のカメラ（H.264）を USB（か試験用の TCP）で受けて、ほどいて、窓に出す PC 側の橋渡し。仮想カメラ（vcam_source.c）へも渡せる。
//
//   webcam_rx.exe --usb                       USB の iPhone（usbmuxd 越しに 50064 番）
//   webcam_rx.exe --usb --device 00008110-0012  iPhone が何台かあるとき、UDID（前の方 6 文字以上でよい）か DeviceID で選ぶ
//   webcam_rx.exe --list                      usbmuxd が見ている iPhone の一覧（DeviceID・UDID・種類）を出して終わる
//   webcam_rx.exe --usb --mux 50520           偽物の usbmuxd に向ける試験
//   webcam_rx.exe --addr 127.0.0.1:50521      TCP（fake_iphone.exe）
//   webcam_rx.exe --addr 192.168.0.10:50064 --code 123456   Wi-Fi の iPhone（アプリで「Wi-Fi でも待つ」を入れて、画面の 6 けたを渡す）
//   そのほか  --w 1280 --h 720 --fps 30 --kbps 0 --gop 0   欲しい形（0 はおまかせ＝ iPhone のアプリの画面の設定）
//             --camera auto|back|front|ultrawide|tele（0〜3 の数でもよい。数は前の版と同じく 0 がおまかせ）
//             --quality auto|standard|high|max   kbps が 0 のときのビットレートの段（新しいアプリだけが読む）
//             --mirror    左右反転して送ってもらう
//             --secs N    N 秒で終わって、遅れとコマ数を書く
//             --nowin     窓を出さない（ほどくところまで測る）
//             --pipe      ほどいた NV12 を \\.\pipe\iphone-webcam-nv12 へ流す（仮想カメラの口）
//             --shm       ほどいた NV12 を共有メモリ Local\pocketgpu-webcam-v1 に書く（DirectShow の仮想カメラ dshow_cam.dll の口。何本でも読める）。
//                         1920x1080 より大きい絵（4K など）は縮めて書く
//             --no-follow --shm のとき、仮想カメラを開いたアプリ（OBS など）が選んだ形を iPhone に頼み直さない
//             --reconnect 切れたら（iPhone のアプリを閉じた・ケーブルを抜いた）つなぎ直し続ける。止めるまで終わらない
//             --swdec     ソフトでほどく（既定は GPU で試して、だめならソフト）。camgpu1015：--decode cpu と同じ。環境変数 POCKETGPU_DECODE=cpu|gpu でも切り替えられる
//             --decode auto|gpu|cpu   camgpu1015：ほどく場所。GPU（3060 の Video Decode）を空けたいなら cpu
//             --adapter 名前   dec1013：GPU でほどくときのアダプタを名前の一部で選ぶ（例 Intel）。無ければ Windows の既定。環境変数 POCKETGPU_ADAPTER でも同じ。選んだ名前は記録に出る
//             --no-clamp  dec1013：--shm のとき、iPhone に頼む大きさを 1920x1080 に収めるのをやめる（4K を頼んで縮める前の動き）。環境変数 POCKETGPU_NO_CLAMP=1 でも同じ
//             --vcam-max 2160   cam4k：--shm のとき、4K の口（共有メモリの名前の後ろに -4k）を作り、仮想カメラが 3840x2160（30・60）も名乗るようにする。
//                               iPhone に頼む大きさの箱が 3840x2160 になり、4K の絵は縮めずに 4K の口へ書く（本体の枠には前の読む側のために 1920x1080 に縮めた絵も書く）。
//                               環境変数 POCKETGPU_VCAM_MAX=2160 でも同じ。無いときは 1080（レジストリ HKCU\Software\PocketGPU\VirtualCamera の MaxHeight を 1080 に戻す）
//             --snap f.png [--snap-at 90]   90 コマ目を PNG に書く
//             --ctl-pipe 名前   camctl2：PC アプリとの口（名前付きパイプ \\.\pipe\名前。既定は pocketgpu-camctl）。1 行 1 つの UTF-8 の JSON
//             --no-ctl    camctl2：PC から操作しない（HELLO で CTRL を名乗らず、パイプも開かない。前の版と同じ動き）
#define COBJMACROS
#include "wcam.h"
#include "video.h"
#include "vdec.h"
#include "wcam_shm.h"
#include "vcam_scale.h"
#include "wcam_ctl.h"
#include "wcam_json.h"
#include <sddl.h>
#include <math.h>
#include <stdarg.h>
#include <mmsystem.h>

// 記録の時計。main の初めに 1 回だけ決める（camctl2：前は最初に書いた糸が決めていたので、2 つの糸が同時に書き始めると時刻がずれた）
static LARGE_INTEGER g_log_f0, g_log_c0;
static void log_clock_init(void) { QueryPerformanceFrequency(&g_log_f0); QueryPerformanceCounter(&g_log_c0); }
static double log_secs(void) {
    LARGE_INTEGER c;
    if (!g_log_f0.QuadPart) log_clock_init();
    QueryPerformanceCounter(&c);
    return (double) (c.QuadPart - g_log_c0.QuadPart) / (double) g_log_f0.QuadPart; // 起動からの秒
}

static int g_test_slow_log; // 試験用（--test-slow-log ミリ秒）：記録を 1 行書くたびに止める。標準エラーの書き込みが止まったときの真似
static void say_(const char *fmt, ...) {
    double t = log_secs();
    if (g_test_slow_log) Sleep((DWORD) g_test_slow_log);
    fprintf(stderr, "[%7.2f] ", t);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#define LOG(...) say_(__VA_ARGS__)

// camctl2：あとで書く記録（PC アプリとの口の糸だけが使う）。標準エラーへの書き込みは、読む側が遅いと止まる。
// 10/12 の試験で、つないだ直後に止まって status・caps・state が 13 秒遅れたので、その糸は書き込みを待たない。書くのは記録の糸
static CRITICAL_SECTION g_alk;
static char *g_alq;
static size_t g_aln, g_alcap;
static HANDLE g_alev;
static void alog(const char *fmt, ...) {
    char line[1024];
    int n = snprintf(line, sizeof line, "[%7.2f] ", log_secs());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t) n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > (int) sizeof line - 2) n = (int) sizeof line - 2;
    line[n++] = '\n';
    EnterCriticalSection(&g_alk);
    if (g_aln + (size_t) n <= 1u << 20) {
        if (g_aln + (size_t) n > g_alcap) {
            size_t c = g_alcap ? g_alcap * 2 : 8192;
            while (c < g_aln + (size_t) n) c *= 2;
            char *q = (char *) realloc(g_alq, c);
            if (q) g_alq = q, g_alcap = c;
        }
        if (g_aln + (size_t) n <= g_alcap) { memcpy(g_alq + g_aln, line, (size_t) n); g_aln += (size_t) n; }
    }
    LeaveCriticalSection(&g_alk);
    SetEvent(g_alev);
}
// 溜まった記録を書く（記録の糸と、終わるときの main）
static void alog_drain(void) {
    EnterCriticalSection(&g_alk);
    char *q = g_alq;
    size_t n = g_aln;
    g_alq = NULL, g_aln = 0, g_alcap = 0;
    LeaveCriticalSection(&g_alk);
    if (n) { fwrite(q, 1, n, stderr); fflush(stderr); }
    free(q);
}

// ---------------------------------------------------------------- 設定
static struct {
    const char *addr, *device;
    int usb, mux, nowin, pipe, shm, swdec, secs, snap_at, follow, reconnect, list;
    const char *snap, *csv;
    int noclamp;            // dec1013：--shm のときの 1920x1080 への頭打ちをやめる（--no-clamp）
    int nodefer;            // cam4k：4K の 60 コマ/秒で読み出しを 1 コマ遅らせるのをやめる（--no-defer。比べる試験用）
    int vcam_max;           // cam4k：仮想カメラが名乗る一番大きい高さ。2160 なら 4K の口を作る（--vcam-max 2160 か環境変数 POCKETGPU_VCAM_MAX）。それ以外は 1080
    const char *adapter;    // dec1013：GPU でほどくときのアダプタ（名前の一部。--adapter。無ければ Windows の既定）
    int ctl;                // camctl2：PC から操作する（HELLO で CTRL を名乗り、PC アプリとのパイプを開く）
    const char *ctl_pipe;   // camctl2：パイプの名前
    wc_hello hello; // コマンドの引数で決めた形（0 はおまかせ）
} g_opt;

// ---------------------------------------------------------------- 記録
typedef struct { double cap, sent, recv, dec, conv, drawn; uint32_t seq; uint32_t bytes; int key; } rec_t;
#define MAXREC 200000
static rec_t *g_rec;
static size_t g_nrec;
static double g_off_us;     // iPhone の時計 − PC の時計（µs）
static double g_best_rtt = 1e18;
static volatile LONG g_done;
static uint64_t g_bad_counter, g_checked, g_gaps, g_decode_fail, g_drawn_n, g_rehello, g_acks, g_sessions;
static double g_t_start;
static size_t g_bytes_total, g_frames_recv, g_frames_dec;

// ---------------------------------------------------------------- 窓に出す絵
static CRITICAL_SECTION g_lk;
static uint8_t *g_front;     // 窓に出す BGRA
static size_t g_front_cap;
static uint32_t g_fw, g_fh;
static long g_front_rec = -1; // g_front の記録の番号
static HWND g_hwnd;
static uint32_t g_ack_w, g_ack_h, g_ack_fps;
static volatile LONG g_connected;

// ---------------------------------------------------------------- 仮想カメラの口（名前つきパイプ）
static CRITICAL_SECTION g_plk;
static uint8_t *g_pbuf;
static size_t g_pcap;
static uint32_t g_pw, g_ph;
static uint64_t g_ppts;
static int g_phas;
static HANDLE g_pev;

static DWORD WINAPI pipe_thread(LPVOID p) {
    (void) p;
    // 作った人（このユーザー）・SYSTEM・Administrators に全部、Local Service（Frame Server の中の media source）に読み書き
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;OW)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;LS)", SDDL_REVISION_1, &sd, NULL)) {
        LOG("パイプの権限の文を作れない（%lu）", GetLastError());
        return 1;
    }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    uint8_t *loc = NULL;
    size_t lcap = 0;
    while (!g_done) {
        HANDLE h = CreateNamedPipeW(WCAM_PIPE_NAME, PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 22, 0, 0, &sa);
        if (h == INVALID_HANDLE_VALUE) { LOG("パイプを作れない（%lu）", GetLastError()); Sleep(500); continue; }
        // 相手を待つ（終わりの合図を見るため、重ねた待ちにして 200 ms ごとに見る）
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        BOOL ok = ConnectNamedPipe(h, &ov);
        DWORD err = ok ? 0 : GetLastError();
        int connected = ok || err == ERROR_PIPE_CONNECTED;
        while (!connected && !g_done && err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 200) == WAIT_OBJECT_0) connected = 1;
        }
        CloseHandle(ov.hEvent);
        if (!connected) { CloseHandle(h); continue; }
        LOG("仮想カメラの口に相手がつながった");
        uint64_t sent = 0;
        while (!g_done) {
            if (WaitForSingleObject(g_pev, 200) != WAIT_OBJECT_0) continue;
            EnterCriticalSection(&g_plk);
            size_t n = (size_t) g_pw * g_ph * 3 / 2;
            wc_pipe_head ph = { WCAM_PIPE_MAGIC, g_pw, g_ph, g_ack_fps, g_ppts, 0, 0 };
            if (g_phas && n) {
                if (lcap < n + sizeof ph) { free(loc); loc = (uint8_t *) malloc(n + sizeof ph); lcap = loc ? n + sizeof ph : 0; }
                if (loc) { memcpy(loc, &ph, sizeof ph); memcpy(loc + sizeof ph, g_pbuf, n); }
                g_phas = 0;
            } else n = 0;
            LeaveCriticalSection(&g_plk);
            if (!n || !loc) continue;
            DWORD wr = 0;
            if (!WriteFile(h, loc, (DWORD) (n + sizeof ph), &wr, NULL) || wr != n + sizeof ph) break;
            sent++;
        }
        LOG("仮想カメラの口：相手が切れた（%llu コマ流した）", (unsigned long long) sent);
        DisconnectNamedPipe(h);
        CloseHandle(h);
    }
    free(loc);
    LocalFree(sd);
    return 0;
}

// ---------------------------------------------------------------- 仮想カメラの口その 2（共有メモリ。DirectShow の dshow_cam.dll が読む）
static HANDLE g_shm_map;
static wcam_shm_head *g_shm;
static int g_shm_big_noted;
// cam4k：4K の口（名前の後ろに -4k の別の共有メモリ）。--vcam-max 2160 のときだけ作る。作れたら、仮想カメラに渡せる一番大きい形（箱）は 3840x2160
static HANDLE g_ext_map;
static wcam_ext_head *g_ext;
static uint32_t g_box_w = WCAM_SHM_MAXW, g_box_h = WCAM_SHM_MAXH;
static int g_names_test; // 試験用の共有メモリの名前（PGPU_TEST_SHM）を使っている＝レジストリも試験用の置き場に書く
static uint64_t g_shm_ext_frames, g_shm_frames; // 記録用
static double g_us_push, g_us_map, g_us_write, g_us_other; // 記録用：1 コマの中の時間の内訳（µs の合計。受ける糸だけが触る）
// dec1013：共有メモリの名前。ふだんは WCAM_SHM_NAME。試験のときだけ環境変数 PGPU_TEST_SHM で別の名前にする（PocketGPU のプレビューと同じ名前の決まり。
// 本物の仮想カメラとプレビューが、試験の絵を読まないようにする）
static WCHAR g_shm_name_buf[128];
static const WCHAR *g_shm_name = WCAM_SHM_NAME;

static int shm_open_writer(void) {
    // 読めるのは、このユーザーのプロセス（整合性レベルが低い物も）と SYSTEM だけ。書けるのは同じユーザーの中くらい以上
    HANDLE tok = NULL;
    char tu[256];
    DWORD n = 0;
    LPWSTR sidstr = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) || !GetTokenInformation(tok, TokenUser, tu, sizeof tu, &n) || !ConvertSidToStringSidW(((TOKEN_USER *) tu)->User.Sid, &sidstr)) {
        LOG("共有メモリ：自分の SID が取れない（%lu）", GetLastError());
        if (tok) CloseHandle(tok);
        return 0;
    }
    CloseHandle(tok);
    wchar_t sddl[512];
    _snwprintf(sddl, 511, L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)S:(ML;;NW;;;LW)", sidstr);
    sddl[511] = 0;
    LocalFree(sidstr);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) { LOG("共有メモリ：権限の文を作れない（%lu）", GetLastError()); return 0; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    const uint64_t total = WCAM_SHM_TOTAL;
    g_shm_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD) (total >> 32), (DWORD) total, g_shm_name);
    DWORD err = GetLastError();
    // cam4k：4K を名乗るなら、4K の口も作る（読む側が前の小さい共有メモリを開いたままでも作れるよう、別の名前）。作れなければ 1080p のままにする
    DWORD err_ext = 0;
    if (g_shm_map && g_opt.vcam_max >= 2160) {
        WCHAR en[200];
        _snwprintf(en, 199, L"%ls%ls", g_shm_name, WCAM_EXT_SUFFIX);
        en[199] = 0;
        const uint64_t et = WCAM_EXT_TOTAL;
        g_ext_map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD) (et >> 32), (DWORD) et, en);
        err_ext = GetLastError();
        if (g_ext_map) {
            g_ext = (wcam_ext_head *) MapViewOfFile(g_ext_map, FILE_MAP_ALL_ACCESS, 0, 0, et);
            MEMORY_BASIC_INFORMATION mbi;
            if (g_ext && err_ext == ERROR_ALREADY_EXISTS && (!VirtualQuery(g_ext, &mbi, sizeof mbi) || mbi.RegionSize < et)) {
                LOG("4K の口: 読む側が前の別の大きさの物を開いたままなので使えない");
                UnmapViewOfFile(g_ext);
                g_ext = NULL;
            }
            if (!g_ext) { CloseHandle(g_ext_map); g_ext_map = NULL; }
        }
        if (!g_ext) LOG("4K の口を作れない（%lu）。仮想カメラは 1080p までにする", err_ext);
    }
    LocalFree(sd);
    if (!g_shm_map) { LOG("共有メモリを作れない（%lu）", err); return 0; }
    g_shm = (wcam_shm_head *) MapViewOfFile(g_shm_map, FILE_MAP_ALL_ACCESS, 0, 0, total);
    if (!g_shm) { LOG("共有メモリを開けない（%lu）", GetLastError()); CloseHandle(g_shm_map); g_shm_map = NULL; return 0; }
    // 読む側が前の分を開いたままなら、同じ物がそのまま返る。形が合っていれば番号と頼み（req）を続ける
    if (!(err == ERROR_ALREADY_EXISTS && g_shm->magic == WCAM_SHM_MAGIC && g_shm->slot_bytes == WCAM_SHM_SLOT_BYTES && g_shm->nslots == WCAM_SHM_SLOTS)) {
        memset(g_shm, 0, sizeof *g_shm);
        g_shm->head_bytes = sizeof(wcam_shm_head), g_shm->slot_bytes = WCAM_SHM_SLOT_BYTES;
        g_shm->nslots = WCAM_SHM_SLOTS, g_shm->max_w = WCAM_SHM_MAXW, g_shm->max_h = WCAM_SHM_MAXH;
        MemoryBarrier();
        g_shm->magic = WCAM_SHM_MAGIC;
    }
    g_shm->version = 3; // 版 2：制御の欄（req・cur）。版 3（cam4k）：4K の口の欄（ext_state・cur_full）。前の読む側は版を見ない
    g_shm->writer_pid = GetCurrentProcessId();
    g_shm->writer_caps = (g_opt.follow ? WCAM_CAP_FOLLOW : 0) | WCAM_CAP_4K;
    g_shm->cur = 0;
    g_shm->cur_full = 0;
    g_shm->ext_state = 0;
    if (g_ext) {
        // 4K の口の頭。読む側が前から開いていて、同じ形ならそのまま続ける（枠の番号は 0 に戻す）
        memset(g_ext, 0, sizeof *g_ext);
        g_ext->head_bytes = sizeof(wcam_ext_head), g_ext->slot_bytes = WCAM_EXT_SLOT_BYTES, g_ext->nslots = WCAM_EXT_SLOTS;
        g_ext->max_w = WCAM_EXT_MAXW, g_ext->max_h = WCAM_EXT_MAXH, g_ext->version = 1, g_ext->writer_pid = GetCurrentProcessId();
        MemoryBarrier();
        g_ext->magic = WCAM_EXT_MAGIC;
        MemoryBarrier();
        g_shm->ext_state = 1;
        g_box_w = WCAM_EXT_MAXW, g_box_h = WCAM_EXT_MAXH;
    }
    g_shm->heartbeat_us = wcam_shm_now_us();
    LOG("共有メモリ %ls を開いた（%s）", g_shm_name, err == ERROR_ALREADY_EXISTS ? "読む側が先に開いていた" : "新しく作った");
    if (g_ext) LOG("  4K の口 %ls%ls を作った（%u x %u まで・%.0f MB）。仮想カメラは 4K も名乗る", g_shm_name, WCAM_EXT_SUFFIX, g_box_w, g_box_h, (double) WCAM_EXT_TOTAL / 1048576.0);
    // 仮想カメラ（dshow_cam）が名乗る一番大きい高さをレジストリに書く。4K でない起動のときは 1080 に戻す（選んでいないときは今までどおり 1080p まで）
    {
        HKEY k;
        const WCHAR *key = g_names_test ? WCAM_VCAM_REGKEY_TEST : WCAM_VCAM_REGKEY;
        DWORD v = g_ext ? 2160 : 1080;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &k, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(k, WCAM_VCAM_REGVAL, 0, REG_DWORD, (const BYTE *) &v, sizeof v);
            RegCloseKey(k);
            LOG("  仮想カメラが名乗る大きさ：%s（HKCU\\%ls の %ls = %lu）", v >= 2160 ? "4K まで" : "1080p まで", key, WCAM_VCAM_REGVAL, (unsigned long) v);
        } else LOG("  仮想カメラが名乗る大きさをレジストリに書けない（%ls）", key);
    }
    return 1;
}

// 共有メモリに収める大きさ（縦横比を保って box の中へ。偶数に揃える）。box は 1920x1080（4K の口があれば 3840x2160）
static void shm_fit_box(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint32_t *ow, uint32_t *oh) {
    if (w <= bw && h <= bh) { *ow = w & ~1u, *oh = h & ~1u; return; }
    uint32_t fw = bw, fh = (uint32_t) ((uint64_t) bw * h / w);
    if (fh > bh) { fh = bh; fw = (uint32_t) ((uint64_t) bh * w / h); }
    *ow = fw & ~1u, *oh = fh & ~1u;
}
static void shm_fit(uint32_t w, uint32_t h, uint32_t *ow, uint32_t *oh) { shm_fit_box(w, h, g_box_w, g_box_h, ow, oh); }

static void shm_write(const uint8_t *y, const uint8_t *uv, uint32_t pitch, uint32_t w, uint32_t h, uint32_t fps, uint64_t pts_us) {
    if (!g_shm) return;
    // 本体の枠（前の読む側も読む）は 1920x1080 まで。それより大きい絵は、4K の口があって入るならそこへ本物を書き、本体の枠には縮めた絵を書く
    uint32_t dw, dh;
    shm_fit_box(w, h, WCAM_SHM_MAXW, WCAM_SHM_MAXH, &dw, &dh);
    const int to_ext = g_ext && (dw != w || dh != h) && w <= WCAM_EXT_MAXW && h <= WCAM_EXT_MAXH;
    if ((dw != w || dh != h) && !g_shm_big_noted++) {
        if (to_ext) LOG("共有メモリ：%u x %u は 4K の口へ書く（本体の枠には前の読む側のために %u x %u に縮めた絵も書く）", w, h, dw, dh);
        else LOG("共有メモリ：%u x %u は入らないので %u x %u に縮めて書く（%u x %u まで）", w, h, dw, dh, WCAM_SHM_MAXW, WCAM_SHM_MAXH);
    }
    int64_t next = g_shm->seq + 1;
    uint32_t i = (uint32_t) (next % WCAM_SHM_SLOTS);
    if (to_ext) {
        // 先に 4K の口へ本物を書く（枠の seq が書き終わりの印）。本体の枠の印（下）はこのあとなので、読む側は本体の枠が新しいと分かれば 4K の口の枠も新しい
        const uint32_t ew = w & ~1u, eh = h & ~1u;
        wcam_shm_slot *es = &g_ext->slot[next % WCAM_EXT_SLOTS];
        es->seq = 0;
        MemoryBarrier();
        uint8_t *e = WCAM_EXT_FRAME(g_ext, next % WCAM_EXT_SLOTS);
        for (uint32_t r = 0; r < eh; r++) memcpy(e + (size_t) r * ew, y + (size_t) r * pitch, ew);
        for (uint32_t r = 0; r < eh / 2; r++) memcpy(e + (size_t) ew * eh + (size_t) r * ew, uv + (size_t) r * pitch, ew);
        es->width = ew, es->height = eh, es->fps = fps, es->flags = 0, es->pts_us = pts_us;
        MemoryBarrier();
        es->seq = next;
        g_shm_ext_frames++;
    }
    g_shm_frames++;
    wcam_shm_slot *sl = &g_shm->slot[i];
    sl->seq = 0; // 書いている間は 0（読む側は使わない）
    MemoryBarrier();
    uint8_t *d = WCAM_SHM_FRAME(g_shm, i);
    if (dw == w && dh == h) {
        for (uint32_t r = 0; r < h; r++) memcpy(d + (size_t) r * w, y + (size_t) r * pitch, w);
        for (uint32_t r = 0; r < h / 2; r++) memcpy(d + (size_t) w * h + (size_t) r * w, uv + (size_t) r * pitch, w);
    } else if (dw * 2 == w && dh * 2 == h) {
        // ちょうど半分（3840x2160 → 1920x1080）：2x2 の平均。色は 2x2 の色の組の平均（SSE2。1 画素ずつの書き方と同じ値）
        vc_half_scale_nv12(y, uv, pitch, dw, dh, d);
    } else {
        // それ以外の大きさ：近い画素を取る
        for (uint32_t r = 0; r < dh; r++) {
            const uint8_t *s = y + (size_t) ((uint64_t) r * h / dh) * pitch;
            uint8_t *o = d + (size_t) r * dw;
            for (uint32_t x = 0; x < dw; x++) o[x] = s[(uint64_t) x * w / dw];
        }
        for (uint32_t r = 0; r < dh / 2; r++) {
            const uint8_t *s = uv + (size_t) ((uint64_t) r * (h / 2) / (dh / 2)) * pitch;
            uint8_t *o = d + (size_t) dw * dh + (size_t) r * dw;
            for (uint32_t x = 0; x < dw / 2; x++) {
                uint32_t sx = (uint32_t) ((uint64_t) x * (w / 2) / (dw / 2)) * 2;
                o[2 * x] = s[sx], o[2 * x + 1] = s[sx + 1];
            }
        }
    }
    sl->width = dw, sl->height = dh, sl->fps = fps, sl->flags = to_ext ? WCAM_SLOT_EXT : 0, sl->pts_us = pts_us;
    MemoryBarrier();
    sl->seq = next;
    g_shm->heartbeat_us = wcam_shm_now_us();
    MemoryBarrier();
    g_shm->seq = next;
}

// iPhone が決めた形を読む側に知らせる。cur は前の読む側のために 1920x1080 に収めた形。cur_full は 4K の口があるときの、収めない本当の形（箱＝3840x2160 まで）
static void shm_set_cur(const wc_ack *a) {
    if (!g_shm) return;
    uint32_t w, h, fw, fh;
    shm_fit_box(a->width, a->height, WCAM_SHM_MAXW, WCAM_SHM_MAXH, &w, &h);
    shm_fit(a->width, a->height, &fw, &fh);
    g_shm->cur_kbps = a->kbps, g_shm->cur_flags = a->flags;
    MemoryBarrier();
    g_shm->cur_full = g_ext ? WCAM_REQ_MAKE(fw, fh, a->fps, a->camera + 1, (uint32_t) g_acks) : 0;
    MemoryBarrier();
    g_shm->cur = WCAM_REQ_MAKE(w, h, a->fps, a->camera + 1, (uint32_t) g_acks);
}

static void shm_close(void) {
    if (g_shm) { g_shm->heartbeat_us = 0; g_shm->cur = 0; g_shm->cur_full = 0; g_shm->ext_state = 0; g_shm->writer_caps = 0; UnmapViewOfFile(g_shm); g_shm = NULL; }
    if (g_shm_map) { CloseHandle(g_shm_map); g_shm_map = NULL; }
    if (g_ext) { UnmapViewOfFile(g_ext); g_ext = NULL; }
    if (g_ext_map) { CloseHandle(g_ext_map); g_ext_map = NULL; }
}

// ---------------------------------------------------------------- camctl2：PC から操作する（CTRL）と、PC アプリとの口
// iPhone とは CTRL_CAPS・CTRL_STATE を受け、CTRL_SET を送る（wcam.h）。PC アプリとは名前付きパイプで 1 行 1 つの UTF-8 の JSON（camctl2/ctl_ids.md）。
//   PC アプリ → ここ：{"cmd":"get_caps"}・{"cmd":"get_state"}・{"cmd":"set","id":番号,"value":数}
//   ここ → PC アプリ：caps・state・event・status・error（つないだ直後に status・caps・state）
// つなぎ手は 1 つ。PC アプリが切れても止まらず、また待つ

// iPhone から受けた物（g_clk で守る。書くのは受ける糸、読むのはパイプの糸）
static CRITICAL_SECTION g_clk;
static int g_c_conn, g_c_ctrl, g_c_have;          // iPhone とつながっている・CTRL を受ける版・CAPS が届いた
static int g_c_thermal_low;                        // 直前の ACK に WC_ACK_THERMAL（熱でコマ数を下げている）
static wc_ctl_item g_c_items[WC_CTL_MAX_ITEMS];
static int g_c_nitems;
static float g_c_stops[WC_CTL_MAX_LIST];
static int g_c_nstops;
static struct { uint16_t id, n; float v[WC_CTL_MAX_LIST]; } g_c_lists[WC_CTL_MAX_ITEMS];
static int g_c_nlists;
static float g_c_val[256];                         // id ごとの今の値（id は 0〜255）
static uint8_t g_c_has[256];
static uint64_t g_c_sets;                          // iPhone へ送った SET の数（記録用）

// PC アプリへ送る行の待ち行列（g_cqlk で守る）。相手がいないときは溜めない
static CRITICAL_SECTION g_cqlk;
static jbuf g_cq;
static HANDLE g_cq_ev;
static int g_c_client;

// iPhone へ送る口。受ける糸（PING・頼み直し）とパイプの糸（SET）の両方が送るので、g_slk の中で送る
static CRITICAL_SECTION g_slk;
static SOCKET g_sock = INVALID_SOCKET;

static int ssend(SOCKET s, uint32_t type, const void *body, uint32_t len) {
    EnterCriticalSection(&g_slk);
    int ok = wc_send_msg(s, type, body, len);
    LeaveCriticalSection(&g_slk);
    return ok;
}

// 1 行を待ち行列に入れる（改行は足す）
static void cq_line(const jbuf *b) {
    if (!g_opt.ctl || !b->n) return;
    EnterCriticalSection(&g_cqlk);
    if (g_c_client) {
        jb_put(&g_cq, b->p, b->n);
        jb_s(&g_cq, "\n");
        SetEvent(g_cq_ev);
    }
    LeaveCriticalSection(&g_cqlk);
}

// ---- JSON を作る（g_clk の中で呼ぶ） ----
static void cj_status(jbuf *b) {
    jb_f(b, "{\"type\":\"status\",\"connected\":%s,\"ctrl\":%s}", g_c_conn ? "true" : "false", g_c_conn && g_c_ctrl ? "true" : "false");
}

// 選ぶ項目の選択肢（表が無ければ min から max を step 刻みで）
static int cj_choice_values(const wc_ctl_item *it, float *out, int max) {
    for (int i = 0; i < g_c_nlists; i++)
        if (g_c_lists[i].id == it->id) {
            int n = g_c_lists[i].n < max ? g_c_lists[i].n : max;
            memcpy(out, g_c_lists[i].v, sizeof(float) * (size_t) n);
            return n;
        }
    int n = 0;
    float st = it->step > 0 ? it->step : 1;
    for (float v = it->min; v <= it->max + st * 0.001f && n < max; v += st) out[n++] = v;
    return n;
}

static void cj_caps(jbuf *b) {
    int on = g_c_conn && g_c_ctrl && g_c_have;
    jb_f(b, "{\"type\":\"caps\",\"iphone\":%s,\"items\":[", on ? "true" : "false");
    int first = 1;
    for (int i = 0; on && i < g_c_nitems; i++) {
        const wc_ctl_item *it = &g_c_items[i];
        const wc_ctl_def *d = wc_ctl_find(it->id);
        // 読むだけの値は state で送る。今は使えない操作と、この webcam_rx が知らない番号は出さない
        if (!d || it->kind > WC_CTL_NUMBER || (it->flags & WC_CTL_UNAVAILABLE)) continue;
        char a[32], z[32], st[32];
        wc_ctl_num(a, sizeof a, it->min);
        wc_ctl_num(z, sizeof z, it->max);
        wc_ctl_num(st, sizeof st, it->step);
        jb_f(b, "%s{\"id\":%u,\"name\":", first ? "" : ",", (unsigned) it->id);
        jb_q(b, d->name);
        jb_f(b, ",\"kind\":\"%s\",\"min\":%s,\"max\":%s,\"step\":%s,\"choices\":[", wc_ctl_kind_json((int) it->kind), a, z, st);
        float vals[WC_CTL_MAX_LIST];
        int nv = 0;
        if (it->kind == WC_CTL_CHOICE) nv = cj_choice_values(it, vals, WC_CTL_MAX_LIST);
        else if (it->id == 10) { nv = g_c_nstops; memcpy(vals, g_c_stops, sizeof(float) * (size_t) nv); } // ズームの choices は倍率のボタン
        for (int k = 0; k < nv; k++) {
            char num[32], ja[64], en[64];
            wc_ctl_num(num, sizeof num, vals[k]);
            wc_ctl_label_of(d, vals[k], ja, en, sizeof ja);
            jb_f(b, "%s{\"value\":%s,\"label_ja\":", k ? "," : "", num);
            jb_q(b, ja);
            jb_s(b, ",\"label_en\":");
            jb_q(b, en);
            jb_s(b, "}");
        }
        jb_s(b, "],\"unit\":");
        jb_q(b, d->unit);
        jb_s(b, "}");
        first = 0;
    }
    jb_s(b, "]}");
}

// ids が NULL なら今の値を全部。あれば、その id だけ
static void cj_state(jbuf *b, const uint16_t *ids, int n) {
    jb_s(b, "{\"type\":\"state\",\"values\":{");
    int first = 1;
    for (int i = 0; i < (ids ? n : 256); i++) {
        unsigned id = ids ? ids[i] : (unsigned) i;
        if (id > 255 || !g_c_has[id]) continue;
        char num[32];
        wc_ctl_num(num, sizeof num, g_c_val[id]);
        jb_f(b, "%s\"%u\":%s", first ? "" : ",", id, num);
        first = 0;
    }
    jb_s(b, "}}");
}

// status・caps・state をまとめて送る（PC アプリがつないだ直後・iPhone の状態が変わったとき）
static void c_send_all(int with_status) {
    jbuf b1 = { 0 }, b2 = { 0 }, b3 = { 0 };
    EnterCriticalSection(&g_clk);
    if (with_status) cj_status(&b1);
    cj_caps(&b2);
    cj_state(&b3, NULL, 0);
    LeaveCriticalSection(&g_clk);
    cq_line(&b1);
    cq_line(&b2);
    cq_line(&b3);
    jb_free(&b1), jb_free(&b2), jb_free(&b3);
}

static void c_clear_locked(void) {
    g_c_have = 0, g_c_nitems = 0, g_c_nstops = 0, g_c_nlists = 0;
    memset(g_c_has, 0, sizeof g_c_has);
}

// ---- iPhone から受けた物（受ける糸） ----
static void c_on_ack(const wc_ack *a, int first) {
    EnterCriticalSection(&g_clk);
    int was_conn = g_c_conn, was_ctrl = g_c_ctrl;
    g_c_conn = 1;
    g_c_ctrl = (a->flags & WC_ACK_CTRL) != 0;
    g_c_thermal_low = (a->flags & WC_ACK_THERMAL) != 0;
    if (!g_c_ctrl) c_clear_locked();
    LeaveCriticalSection(&g_clk);
    if (was_conn != 1 || was_ctrl != g_c_ctrl) {
        // CTRL の iPhone は、このあと CAPS を送ってくる。そのとき caps と state を送る。古いアプリなら、ここで空の caps と state
        if (g_c_ctrl) {
            jbuf b = { 0 };
            EnterCriticalSection(&g_clk);
            cj_status(&b);
            LeaveCriticalSection(&g_clk);
            cq_line(&b);
            jb_free(&b);
        } else c_send_all(1);
    }
    if (first && g_opt.ctl) LOG(g_c_ctrl ? "iPhone は PC からの操作に対応している（CTRL）" : "iPhone のアプリは PC からの操作に対応していない版");
}

static void c_on_disconnect(void) {
    EnterCriticalSection(&g_clk);
    int was = g_c_conn;
    g_c_conn = 0, g_c_ctrl = 0, g_c_thermal_low = 0;
    c_clear_locked();
    LeaveCriticalSection(&g_clk);
    if (was) c_send_all(1);
}

static void c_on_caps(const uint8_t *p, uint32_t len) {
    uint32_t at = 0, n = 0;
    if (len < 4) return;
    memcpy(&n, p, 4);
    at = 4;
    if (n > WC_CTL_MAX_ITEMS || at + n * sizeof(wc_ctl_item) > len) { LOG("iPhone の操作の一覧の形が変（%u 項目・%u バイト）", n, len); return; }
    EnterCriticalSection(&g_clk);
    c_clear_locked();
    memcpy(g_c_items, p + at, n * sizeof(wc_ctl_item));
    g_c_nitems = (int) n;
    at += n * (uint32_t) sizeof(wc_ctl_item);
    for (uint32_t i = 0; i < n; i++)
        if (g_c_items[i].id < 256) { g_c_val[g_c_items[i].id] = g_c_items[i].value; g_c_has[g_c_items[i].id] = 1; }
    // 倍率のボタン（PROTOCOL.md の案）
    uint32_t ns = 0;
    if (at + 4 <= len) {
        memcpy(&ns, p + at, 4);
        at += 4;
        if (ns > WC_CTL_MAX_LIST || at + ns * 4 > len) ns = 0;
        memcpy(g_c_stops, p + at, ns * 4);
        g_c_nstops = (int) ns;
        at += ns * 4;
    }
    // 選択肢の表（camctl2 で足した。無ければ min から max）
    if (at + 4 <= len) {
        uint32_t nl = 0;
        memcpy(&nl, p + at, 4);
        at += 4;
        for (uint32_t i = 0; i < nl && i < WC_CTL_MAX_ITEMS && at + 4 <= len; i++) {
            uint16_t h[2];
            memcpy(h, p + at, 4);
            at += 4;
            if (at + (uint32_t) h[1] * 4 > len) break;
            g_c_lists[g_c_nlists].id = h[0];
            g_c_lists[g_c_nlists].n = h[1] < WC_CTL_MAX_LIST ? h[1] : WC_CTL_MAX_LIST;
            memcpy(g_c_lists[g_c_nlists].v, p + at, (size_t) g_c_lists[g_c_nlists].n * 4);
            at += (uint32_t) h[1] * 4;
            g_c_nlists++;
        }
    }
    g_c_have = 1;
    LeaveCriticalSection(&g_clk);
    // caps だけ送る。値は iPhone が CAPS のすぐあとに全部の STATE を送ってくるので、そこで state にする
    jbuf b = { 0 };
    EnterCriticalSection(&g_clk);
    cj_caps(&b);
    LeaveCriticalSection(&g_clk);
    cq_line(&b);
    jb_free(&b);
    LOG("iPhone の操作の一覧を受けた：%u 項目・倍率のボタン %u 個", n, ns);
}

static void c_on_state(const uint8_t *p, uint32_t len) {
    uint32_t n = len / (uint32_t) sizeof(wc_ctl_val);
    if (!n) return;
    uint16_t ids[512];
    if (n > 512) n = 512;
    EnterCriticalSection(&g_clk);
    for (uint32_t i = 0; i < n; i++) {
        wc_ctl_val v;
        memcpy(&v, p + i * sizeof v, sizeof v);
        ids[i] = v.id;
        if (v.id < 256) { g_c_val[v.id] = v.value; g_c_has[v.id] = 1; }
    }
    jbuf b = { 0 };
    cj_state(&b, ids, (int) n);
    LeaveCriticalSection(&g_clk);
    cq_line(&b);
    jb_free(&b);
}

static void c_on_event(const wc_event *ev) {
    jbuf b = { 0 };
    if (ev->code == 3) jb_f(&b, "{\"type\":\"event\",\"kind\":\"thermal\",\"level\":%u}", ev->arg);
    // 5 は熱でコマ数を変えた。直前の ACK に THERMAL があれば下げた（fps_lowered）、無ければ元に戻した（fps_restored）
    else if (ev->code == 5) jb_f(&b, "{\"type\":\"event\",\"kind\":\"%s\",\"fps\":%u}", g_c_thermal_low ? "fps_lowered" : "fps_restored", ev->arg);
    cq_line(&b);
    jb_free(&b);
}

// ---- PC アプリから受けた行 ----
static void c_error(const char *msg) {
    jbuf b = { 0 };
    jb_s(&b, "{\"type\":\"error\",\"message\":");
    jb_q(&b, msg);
    jb_s(&b, "}");
    cq_line(&b);
    jb_free(&b);
}

// HELLO の土台（g_opt.hello）を守る。PC アプリの set_format と受ける糸の make_hello の両方が触る
static CRITICAL_SECTION g_hlk;
static volatile LONG g_hello_by_app; // set_format で変わった（受ける糸が頼み直す）
static int parse_camera(const char *v, wc_hello *h);
static int parse_quality(const char *v, uint32_t *q);
// camgpu1015：ほどく場所。cpu はソフト（Media Foundation の H.264 デコーダを CPU で）。GPU（3060 など）を空けたいときに使う。--swdec は cpu と同じ
static int parse_decode(const char *v) {
    if (!strcmp(v, "cpu") || !strcmp(v, "sw")) { g_opt.swdec = 1; return 1; }
    if (!strcmp(v, "gpu") || !strcmp(v, "auto")) { g_opt.swdec = 0; return 1; }
    return 0;
}

// {"cmd":"set_format","camera":"front","w":1920,"h":1080,"fps":30,"quality":"high"}（どれも省いてよい）
// camctl2：PC アプリがカメラ・解像度・フレームレート・画質を変えるたびに webcam_rx を起動し直すと、パイプも映像も切れる。
// これを使えば、同じつなぎのまま iPhone に頼み直す（iPhone はカメラを作り直して ACK を返し、caps も送り直す）
static void c_set_format(const char *p) {
    jval v;
    wc_hello h;
    EnterCriticalSection(&g_hlk);
    h = g_opt.hello;
    LeaveCriticalSection(&g_hlk);
    char m[200];
    if (json_get(p, "camera", &v) == 1) {
        char t[32];
        if (v.type == JV_NUM) snprintf(t, sizeof t, "%d", (int) v.num); else snprintf(t, sizeof t, "%s", v.str);
        if (!parse_camera(t, &h)) { c_error("camera は auto・back・front・ultrawide・tele のどれかです。"); return; }
    }
    jval w, hh;
    int hw = json_get(p, "w", &w) == 1, hhh = json_get(p, "h", &hh) == 1;
    if (hw != hhh || (hw && (w.type != JV_NUM || hh.type != JV_NUM))) { c_error("w と h は両方の数で送ってください。"); return; }
    if (hw) {
        if (!((w.num == 0 && hh.num == 0) || (w.num >= 64 && hh.num >= 64 && w.num <= 4096 && hh.num <= 4096))) { c_error("w と h は 64 から 4096 か、両方 0 です。"); return; }
        h.width = (uint32_t) w.num, h.height = (uint32_t) hh.num;
    }
    if (json_get(p, "fps", &v) == 1) {
        if (v.type != JV_NUM || v.num < 0 || v.num > 120) { c_error("fps は 0 から 120 の数です。"); return; }
        h.fps = (uint32_t) v.num;
    }
    if (json_get(p, "quality", &v) == 1) {
        char t[32];
        if (v.type == JV_NUM) snprintf(t, sizeof t, "%d", (int) v.num); else snprintf(t, sizeof t, "%s", v.str);
        if (!parse_quality(t, &h.quality)) { c_error("quality は auto・standard・high・max のどれかです。"); return; }
    }
    EnterCriticalSection(&g_hlk);
    g_opt.hello = h;
    LeaveCriticalSection(&g_hlk);
    InterlockedExchange(&g_hello_by_app, 1);
    snprintf(m, sizeof m, "PC アプリが形を変えた：%ux%u・%u fps・カメラ %u・画質 %u", h.width, h.height, h.fps, h.camera, h.quality);
    alog("%s", m);
}

// set は 1 回の読みの中の分をまとめて 1 つの CTRL_SET にする（同じ id は後の値）
static wc_ctl_val g_c_batch[64];
static int g_c_nbatch;

static void c_flush_set(void) {
    if (!g_c_nbatch) return;
    int ok = 0;
    EnterCriticalSection(&g_slk);
    if (g_sock != INVALID_SOCKET) ok = wc_send_msg(g_sock, WC_CTRL_SET, g_c_batch, (uint32_t) (g_c_nbatch * (int) sizeof(wc_ctl_val)));
    LeaveCriticalSection(&g_slk);
    if (ok) g_c_sets++;
    else c_error("iPhone に送れませんでした。iPhone との接続が切れています。");
    g_c_nbatch = 0;
}

static void c_line(char *line) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return;
    jval cmd;
    int r = json_get(p, "cmd", &cmd);
    if (r < 0) { c_error("JSON を読めません。1 行に 1 つの JSON の object を送ってください。"); return; }
    if (r == 0 || cmd.type != JV_STR) { c_error("cmd がありません。"); return; }
    if (!strcmp(cmd.str, "get_caps") || !strcmp(cmd.str, "get_state")) {
        jbuf b = { 0 };
        EnterCriticalSection(&g_clk);
        if (cmd.str[4] == 'c') cj_caps(&b); else cj_state(&b, NULL, 0);
        LeaveCriticalSection(&g_clk);
        cq_line(&b);
        jb_free(&b);
        return;
    }
    if (!strcmp(cmd.str, "set_format")) { c_set_format(p); return; }
    if (strcmp(cmd.str, "set")) {
        char m[160];
        snprintf(m, sizeof m, "知らない cmd です：%.60s", cmd.str);
        c_error(m);
        return;
    }
    jval id, val;
    if (json_get(p, "id", &id) != 1 || json_get(p, "value", &val) != 1 || id.type != JV_NUM || (val.type != JV_NUM && val.type != JV_BOOL) ||
        !(id.num >= 0 && id.num <= 65535) || id.num != (double) (int) id.num || !(val.num == val.num) || val.num > 1e9 || val.num < -1e9) {
        c_error("set には id と value の数が要ります。");
        return;
    }
    char m[200];
    EnterCriticalSection(&g_clk);
    int conn = g_c_conn, ctrl = g_c_ctrl, have = g_c_have, found = 0;
    for (int i = 0; i < g_c_nitems; i++)
        if (g_c_items[i].id == (int) id.num && g_c_items[i].kind <= WC_CTL_NUMBER && !(g_c_items[i].flags & WC_CTL_UNAVAILABLE) && wc_ctl_find(g_c_items[i].id)) found = 1;
    LeaveCriticalSection(&g_clk);
    if (!conn) { c_error("iPhone が接続されていません。"); return; }
    if (!ctrl) { c_error("iPhone のアプリが古いため、PC から操作できません。iPhone のアプリを新しくしてください。"); return; }
    if (!have) { c_error("iPhone から操作の一覧がまだ届いていません。少し待ってから送ってください。"); return; }
    if (!wc_ctl_find((unsigned) id.num) || id.num >= 100) { snprintf(m, sizeof m, "id %d は知らない操作です。PC から変えられるのは 1 から 29 です。", (int) id.num); c_error(m); return; }
    if (!found) { snprintf(m, sizeof m, "id %d の操作は今のカメラでは使えません。", (int) id.num); c_error(m); return; }
    int k = 0;
    while (k < g_c_nbatch && g_c_batch[k].id != (uint16_t) id.num) k++;
    if (k == g_c_nbatch) {
        if (g_c_nbatch == (int) (sizeof g_c_batch / sizeof g_c_batch[0])) c_flush_set();
        k = g_c_nbatch++;
    }
    g_c_batch[k].id = (uint16_t) id.num, g_c_batch[k].why = 0, g_c_batch[k].value = (float) val.num;
}

// パイプの権限：このユーザーと SYSTEM だけ（ほかのユーザーと低い整合性の物は開けない）
static PSECURITY_DESCRIPTOR c_pipe_sd(void) {
    HANDLE tok = NULL;
    char tu[256];
    DWORD n = 0;
    LPWSTR sidstr = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return NULL;
    BOOL ok = GetTokenInformation(tok, TokenUser, tu, sizeof tu, &n) && ConvertSidToStringSidW(((TOKEN_USER *) tu)->User.Sid, &sidstr);
    CloseHandle(tok);
    if (!ok) return NULL;
    wchar_t sddl[512];
    _snwprintf(sddl, 511, L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)", sidstr);
    sddl[511] = 0;
    LocalFree(sidstr);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, NULL)) return NULL;
    return sd;
}

// 書く。相手が 3 秒読まなければ切る
static int c_write(HANDLE h, OVERLAPPED *wo, const char *d, size_t n) {
    while (n) {
        DWORD wr = 0, chunk = n > 65536 ? 65536 : (DWORD) n;
        ResetEvent(wo->hEvent);
        if (!WriteFile(h, d, chunk, NULL, wo) && GetLastError() != ERROR_IO_PENDING) return 0;
        if (WaitForSingleObject(wo->hEvent, 3000) != WAIT_OBJECT_0) { CancelIo(h); GetOverlappedResult(h, wo, &wr, TRUE); return 0; }
        if (!GetOverlappedResult(h, wo, &wr, FALSE) || !wr) return 0;
        d += wr, n -= wr;
    }
    return 1;
}

static DWORD WINAPI ctl_pipe_thread(LPVOID arg) {
    (void) arg;
    wchar_t name[300];
    const char *nm = g_opt.ctl_pipe;
    if (!strncmp(nm, "\\\\.\\pipe\\", 9)) nm += 9; // \\.\pipe\ を付けて渡されても同じ
    wchar_t wn[256];
    if (!MultiByteToWideChar(CP_UTF8, 0, nm, -1, wn, 256)) wcscpy(wn, L"pocketgpu-camctl");
    _snwprintf(name, 299, L"\\\\.\\pipe\\%ls", wn);
    name[299] = 0;
    PSECURITY_DESCRIPTOR sd = c_pipe_sd();
    SECURITY_ATTRIBUTES sa = { sizeof sa, sd, FALSE };
    if (!sd) alog("PC アプリとの口：権限の文を作れない（%lu）。既定の権限で開く", GetLastError());
    int fail_noted = 0, ready_noted = 0;
    char *line = (char *) malloc(65536 + 1);
    char rbuf[4096];
    while (!g_done && line) {
        // FIRST_PIPE_INSTANCE：同じ名前の口がもうあれば（ほかの webcam_rx が使っている）作らない
        HANDLE h = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, sd ? &sa : NULL);
        if (h == INVALID_HANDLE_VALUE) {
            if (!fail_noted++) alog("PC アプリとの口 %ls を開けない（%lu）。ほかの webcam_rx が同じ名前で開いているかもしれない。2 秒ごとに開き直す", name, GetLastError());
            for (int i = 0; i < 10 && !g_done; i++) Sleep(200);
            continue;
        }
        if (fail_noted || !ready_noted) { alog("PC アプリとの口 %ls で待っている", name); ready_noted = 1; }
        fail_noted = 0;
        OVERLAPPED ro, wo;
        memset(&ro, 0, sizeof ro);
        memset(&wo, 0, sizeof wo);
        ro.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        wo.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        // 相手を待つ（終わりの合図を見るため、200 ms ごとに見る）
        BOOL ok = ConnectNamedPipe(h, &ro);
        DWORD err = ok ? 0 : GetLastError();
        int connected = ok || err == ERROR_PIPE_CONNECTED;
        while (!connected && !g_done && err == ERROR_IO_PENDING)
            if (WaitForSingleObject(ro.hEvent, 200) == WAIT_OBJECT_0) { DWORD x; connected = GetOverlappedResult(h, &ro, &x, FALSE); break; }
        if (!connected) { CancelIo(h); CloseHandle(ro.hEvent); CloseHandle(wo.hEvent); CloseHandle(h); continue; }
        // つないだ直後の status・caps・state は、ほかの事より先に送る（下の初めの回で書く）
        EnterCriticalSection(&g_cqlk);
        g_c_client = 1;
        g_cq.n = 0;
        LeaveCriticalSection(&g_cqlk);
        c_send_all(1);
        alog("PC アプリがつながった");
        size_t ln = 0;
        int reading = 0, alive = 1, overflow = 0;
        while (!g_done && alive) {
            DWORD got = 0;
            if (!reading) {
                ResetEvent(ro.hEvent);
                if (ReadFile(h, rbuf, sizeof rbuf, NULL, &ro) || GetLastError() == ERROR_IO_PENDING) reading = 1;
                else break; // 相手が切れた
            }
            HANDLE ws[2] = { ro.hEvent, g_cq_ev };
            DWORD w = WaitForMultipleObjects(2, ws, FALSE, 200);
            if (w == WAIT_OBJECT_0) {
                reading = 0;
                if (!GetOverlappedResult(h, &ro, &got, FALSE)) break;
                for (DWORD i = 0; i < got; i++) {
                    char c = rbuf[i];
                    if (c == '\n') {
                        line[ln] = 0;
                        if (overflow) c_error("1 行が長すぎます。1 行は 64 KB までです。");
                        else c_line(line);
                        ln = 0, overflow = 0;
                    } else if (ln < 65536) line[ln++] = c;
                    else overflow = 1;
                }
                c_flush_set();
            }
            // 送る物があれば送る
            EnterCriticalSection(&g_cqlk);
            jbuf out = g_cq;
            memset(&g_cq, 0, sizeof g_cq);
            LeaveCriticalSection(&g_cqlk);
            if (out.n && !c_write(h, &wo, out.p, out.n)) alive = 0;
            jb_free(&out);
        }
        EnterCriticalSection(&g_cqlk);
        g_c_client = 0;
        jb_free(&g_cq);
        LeaveCriticalSection(&g_cqlk);
        g_c_nbatch = 0;
        if (reading) { DWORD x; CancelIo(h); GetOverlappedResult(h, &ro, &x, TRUE); } // 読みかけの分を片づける（無いのに待つと止まる）
        DisconnectNamedPipe(h);
        CloseHandle(ro.hEvent);
        CloseHandle(wo.hEvent);
        CloseHandle(h);
        if (!g_done) alog("PC アプリが切れた。また待つ");
    }
    free(line);
    if (sd) LocalFree(sd);
    return 0;
}

// ---------------------------------------------------------------- アプリが選んだ形（共有メモリの req）を HELLO に混ぜる
static int pid_alive(DWORD pid) {
    if (!pid) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED; // 在るが開けない（別の権限）なら、生きていることにする
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

static int64_t g_req_bad;      // iPhone が断った頼み（serial を除いた形）。形が変わるまで頼み直さない
static int64_t g_req_used;     // いまの HELLO に混ぜた頼み
static double g_pid_checked_us;
static int g_pid_ok;

// 今の頼み（無ければ 0）。req_pid のプロセスが終わっていたら 0
static int64_t current_req(double now) {
    if (!g_shm || !g_opt.follow) return 0;
    int64_t r = g_shm->req;
    if (!r) return 0;
    if (now - g_pid_checked_us > 1e6) { g_pid_ok = pid_alive(g_shm->req_pid); g_pid_checked_us = now; }
    if (!g_pid_ok) return 0;
    if (g_req_bad && WCAM_REQ_SAME(r, g_req_bad)) return 0;
    return r;
}

// dec1013：--shm のとき、iPhone に頼む大きさを共有メモリに入る大きさ（1920x1080）に収める（縦横比は保つ）。
// 仮想カメラ（dshow_cam）が出せる形は 1920x1080 まで・共有メモリの枠も PocketGPU のプレビューも 1920x1080 までで、それより大きい絵は誰も使えない。
// 4K を頼むと「GPU でほどく → CPU に読み戻す → 縮めて捨てる」だけの仕事が増える（PC アプリを 4K・60 コマにして、Video Decode が 65% になった）。
// コマ数と画質の段は触らない。大きさが 0（iPhone のおまかせ）のときは頼みようがないので、ACK のあとの警告（note_big）だけ出す。
// 収めたときの元の大きさは g_clamp_w・h に残す（記録用。受ける糸だけが触る）
static uint32_t g_clamp_w, g_clamp_h;
static void clamp_hello(wc_hello *h) {
    g_clamp_w = g_clamp_h = 0;
    if (!g_opt.shm || g_opt.noclamp || !h->width || !h->height) return;
    if (h->width <= g_box_w && h->height <= g_box_h) return; // cam4k：箱は 1920x1080（4K の口があれば 3840x2160）
    uint32_t w, hh;
    shm_fit(h->width, h->height, &w, &hh);
    g_clamp_w = h->width, g_clamp_h = h->height;
    h->width = w, h->height = hh;
}

// dec1013：iPhone が決めた大きさが共有メモリより大きいとき（おまかせで 4K の設定・--no-clamp・iPhone のアプリが PC の指定より自分の設定を優先した）、大きさが変わるたびに 1 回だけ知らせる
static void note_big(const wc_ack *a) {
    static uint32_t last_w, last_h;
    if (!g_opt.shm || (a->width <= g_box_w && a->height <= g_box_h)) { last_w = last_h = 0; return; }
    if (a->width == last_w && a->height == last_h) return;
    last_w = a->width, last_h = a->height;
    uint32_t w, h;
    shm_fit(a->width, a->height, &w, &h);
    LOG("注意：iPhone は %u x %u で送ってくる。仮想カメラと共有メモリは %u x %u までなので、ほどいたあと %u x %u に縮めて渡す。ほどく面積が %.1f 倍になる%s",
        a->width, a->height, g_box_w, g_box_h, w, h, (double) a->width * a->height / ((double) w * h),
        g_opt.noclamp ? "（--no-clamp）" : a->flags & WC_ACK_FIT ? "（iPhone は箱を守ると答えたのに大きい）" : "（古いアプリ。iPhone のアプリを新しくすると、iPhone が箱に収めて送る）");
}

// 送る HELLO を作る：引数の形を土台に、アプリが選んだ形で上書きする
static void make_hello(wc_hello *h, double now, int64_t *used) {
    EnterCriticalSection(&g_hlk);
    *h = g_opt.hello;
    LeaveCriticalSection(&g_hlk);
    h->version = WCAM_VERSION;
    h->flags |= WC_HELLO_LIVE;
    if (g_opt.ctl) h->flags |= WC_HELLO_CTRL; // camctl2
    int64_t r = current_req(now);
    *used = r;
    if (r) {
        if (WCAM_REQ_W(r) && WCAM_REQ_H(r)) h->width = WCAM_REQ_W(r), h->height = WCAM_REQ_H(r);
        if (WCAM_REQ_FPS(r)) h->fps = WCAM_REQ_FPS(r);
        if (WCAM_REQ_CAM1(r)) h->camera = WCAM_REQ_CAM1(r) - 1, h->flags |= WC_HELLO_CAMERA_SET;
    }
    clamp_hello(h); // dec1013
    // camgpu1015：iPhone に「PC が使える大きさは箱まで」と知らせる。新しいアプリは、PC の指定が 0（おまかせ）でも、アプリの画面の設定が 4K でも、
    // 箱に収まる大きさで撮って送る。古いアプリは読まない（clamp_hello が PC の指定だけは収めている）。--no-clamp では立てない
    if (g_opt.shm && !g_opt.noclamp) {
        h->flags |= WC_HELLO_FIT;
        h->max_w = g_box_w, h->max_h = g_box_h; // cam4k：4K を選んだ（4K の口がある）ときは 3840x2160
    } else {
        h->flags &= ~WC_HELLO_FIT;
        h->max_w = h->max_h = 0;
    }
}

static int hello_same(const wc_hello *a, const wc_hello *b) {
    return a->width == b->width && a->height == b->height && a->fps == b->fps && a->kbps == b->kbps && a->gop == b->gop && a->camera == b->camera && a->flags == b->flags &&
           a->quality == b->quality;
}

static const char *cam_name(uint32_t c) { return c == 1 ? "前面" : c == 2 ? "超広角" : c == 3 ? "望遠" : "背面"; }

static void hello_text(const wc_hello *h, char *out, size_t n) {
    char a[32], b[32], c[32];
    if (h->width && h->height) snprintf(a, sizeof a, "%u x %u", h->width, h->height); else snprintf(a, sizeof a, "大きさはおまかせ");
    if (h->fps) snprintf(b, sizeof b, "%u コマ/秒", h->fps); else snprintf(b, sizeof b, "コマ数はおまかせ");
    if (h->camera || (h->flags & WC_HELLO_CAMERA_SET)) snprintf(c, sizeof c, "%s", cam_name(h->camera)); else snprintf(c, sizeof c, "カメラはおまかせ");
    snprintf(out, n, "%s・%s・%s", a, b, c);
}

// ---------------------------------------------------------------- PNG（圧縮なし）で 1 コマ書く
static uint32_t crc_tab[256];
static uint32_t crc32_(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t) (v >> 24), p[1] = (uint8_t) (v >> 16), p[2] = (uint8_t) (v >> 8), p[3] = (uint8_t) v; }
static void png_chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t b[4];
    be32(b, n);
    fwrite(b, 1, 4, f);
    fwrite(t, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = crc32_(0, (const uint8_t *) t, 4);
    c = crc32_(c, d, n);
    be32(b, c);
    fwrite(b, 1, 4, f);
}
// BGRA の w×h を 1/sc に縮めて（平均でなく間引き）書く
static int write_png(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, int sc) {
    uint32_t ow = w / sc, oh = h / sc;
    size_t rowb = 1 + (size_t) ow * 3, raw = rowb * oh;
    uint8_t *img = (uint8_t *) malloc(raw);
    if (!img) return 0;
    for (uint32_t y = 0; y < oh; y++) {
        uint8_t *r = img + y * rowb;
        *r++ = 0;
        const uint8_t *s = bgra + (size_t) y * sc * w * 4;
        for (uint32_t x = 0; x < ow; x++, r += 3) { const uint8_t *q = s + (size_t) x * sc * 4; r[0] = q[2], r[1] = q[1], r[2] = q[0]; }
    }
    // zlib：圧縮なしのブロックを並べる
    size_t nblk = (raw + 65534) / 65535;
    size_t zn = 2 + raw + nblk * 5 + 4;
    uint8_t *z = (uint8_t *) malloc(zn);
    if (!z) { free(img); return 0; }
    size_t at = 0;
    z[at++] = 0x78, z[at++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i += 65535) {
        size_t n = raw - i < 65535 ? raw - i : 65535;
        z[at++] = (i + n >= raw) ? 1 : 0;
        z[at++] = (uint8_t) n, z[at++] = (uint8_t) (n >> 8), z[at++] = (uint8_t) ~n, z[at++] = (uint8_t) (~n >> 8);
        memcpy(z + at, img + i, n);
        at += n;
    }
    for (size_t i = 0; i < raw; i++) { a = (a + img[i]) % 65521; b = (b + a) % 65521; }
    be32(z + at, (b << 16) | a);
    at += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(img); free(z); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, ow), be32(ihdr + 4, oh);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t) at);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(img);
    free(z);
    return 1;
}

// ---------------------------------------------------------------- 受ける
static int read_counter(const uint8_t *y, uint32_t ys, uint32_t w, uint32_t h) {
    const int blk = (int) (h / 18) & ~1;
    if (blk < 4 || (uint32_t) (blk * 16) > w) return -1;
    int v = 0;
    for (int b = 0; b < 16; b++) {
        int px = b * blk + blk / 2, py = blk / 2;
        v = (v << 1) | (y[(size_t) py * ys + px] > 126);
    }
    return v;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *) a, y = *(const double *) b; return x < y ? -1 : x > y; }

static void stat_line(const char *name, double *v, size_t n) {
    if (!n) { LOG("  %-24s （データ無し）", name); return; }
    qsort(v, n, sizeof(double), cmp_d);
    double s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    LOG("  %-24s 平均 %7.2f  中央 %7.2f  95%% %7.2f  最大 %7.2f ms", name, s / n, v[n / 2], v[(size_t) (n * 0.95)], v[n - 1]);
}

static void report(double secs) {
    if (secs <= 0) secs = 1e-6;
    LOG("---- 結果 ----");
    LOG("受けた時間 %.1f 秒・つないだ回数 %llu・受けたコマ %zu（%.1f コマ/秒）・ほどいたコマ %zu・窓に出したコマ %llu（%.1f コマ/秒）", secs, (unsigned long long) g_sessions,
        g_frames_recv, g_frames_recv / secs, g_frames_dec, (unsigned long long) g_drawn_n, g_drawn_n / secs);
    if (g_shm_frames) LOG("共有メモリへ書いたコマ %llu（%.1f コマ/秒。うち 4K の口へ %llu）・最後に iPhone が決めたコマ数 %u", (unsigned long long) g_shm_frames, g_shm_frames / secs, (unsigned long long) g_shm_ext_frames, g_ack_fps);
    LOG("受けたデータ %.2f MB（%.0f kbps・%.2f MB/秒）・コマの番号の飛び %llu・ほどけなかった %llu", g_bytes_total / 1e6, g_bytes_total * 8.0 / 1000 / secs, g_bytes_total / 1e6 / secs,
        (unsigned long long) g_gaps, (unsigned long long) g_decode_fail);
    LOG("形の頼み直し %llu 回・iPhone からの ACK %llu 回", (unsigned long long) g_rehello, (unsigned long long) g_acks);
    LOG("絵の中身の確かめ（上の 16 個の四角が送った番号と同じか）：%llu コマ中 違い %llu", (unsigned long long) g_checked, (unsigned long long) g_bad_counter);
    LOG("時計の差（iPhone − PC）%.0f µs・往復の最短 %.0f µs", g_off_us, g_best_rtt < 1e17 ? g_best_rtt : -1);
    if (g_opt.csv) {
        FILE *f = fopen(g_opt.csv, "w");
        if (f) {
            fprintf(f, "seq,t_s,enc_ms,net_ms,dec_ms,conv_ms,draw_ms,total_ms,bytes,key\n");
            for (size_t i = 0; i < g_nrec; i++) {
                rec_t *r = &g_rec[i];
                double cap = r->cap - g_off_us, sent = r->sent - g_off_us;
                fprintf(f, "%u,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%d\n", r->seq, (r->recv - g_t_start) / 1e6, (sent - cap) / 1000, (r->recv - sent) / 1000, (r->dec - r->recv) / 1000,
                        (r->conv - r->dec) / 1000, r->drawn > 0 ? (r->drawn - r->conv) / 1000 : -1, r->drawn > 0 ? (r->drawn - cap) / 1000 : -1, r->bytes, r->key);
            }
            fclose(f);
        }
    }
    double *a = (double *) malloc(sizeof(double) * (g_nrec + 1));
    if (!a) return;
    struct { const char *n; int k; } items[] = { { "撮る→送り出す(iPhone)", 0 }, { "送り出す→受け終わる", 1 }, { "受け終わる→ほどけた", 2 }, { "ほどけた→渡した", 3 }, { "渡した→窓に描いた", 4 }, { "撮る→窓に描いた(全部)", 5 } };
    for (int it = 0; it < 6; it++) {
        size_t n = 0;
        for (size_t i = 0; i < g_nrec; i++) {
            rec_t *r = &g_rec[i];
            if (it >= 4 && r->drawn <= 0 && !g_opt.nowin) continue;
            if (r->dec <= 0) continue;
            double cap = r->cap - g_off_us, sent = r->sent - g_off_us;
            double v = it == 0 ? sent - cap : it == 1 ? r->recv - sent : it == 2 ? r->dec - r->recv : it == 3 ? r->conv - r->dec : it == 4 ? r->drawn - r->conv : r->drawn - cap;
            if (it == 5 && g_opt.nowin) v = r->conv - cap;
            a[n++] = v / 1000;
        }
        if (it == 4 && g_opt.nowin) continue;
        stat_line(items[it].n, a, n);
    }
    free(a);
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        double cpu = ((double) (((ULONGLONG) k.dwHighDateTime << 32) | k.dwLowDateTime) + (double) (((ULONGLONG) u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e7;
        LOG("このプログラムの CPU 時間 %.2f 秒（%.1f%% of 1 コア）", cpu, cpu / secs * 100);
    }
}

static SOCKET connect_once(int quiet) {
    if (g_opt.usb) return wc_usb_connect_dev((int) WCAM_PORT, g_opt.mux, g_opt.device, quiet);
    char host[128] = "127.0.0.1", port[16] = "50521";
    if (g_opt.addr) {
        const char *c = strrchr(g_opt.addr, ':');
        if (c) { size_t n = (size_t) (c - g_opt.addr); if (n >= sizeof host) n = sizeof host - 1; memcpy(host, g_opt.addr, n); host[n] = 0; snprintf(port, sizeof port, "%s", c + 1); }
    }
    return wc_tcp_connect(host, port);
}

static int ensure_buf(uint8_t **p, size_t *cap, size_t need) {
    if (*cap >= need) return 1;
    free(*p);
    *p = (uint8_t *) malloc(need);
    *cap = *p ? need : 0;
    return *p != NULL;
}

// ほどけた 1 コマを読み出して（GPU なら staging を Map）、窓・パイプ・共有メモリへ渡す。stg：その絵を写した staging（vdec_map_stg）。tag：その絵の札（seq）
struct fctx { uint8_t *back, *packed; size_t back_cap, packed_cap; };
struct dfr_t { int have; wc_frame_head fh; size_t len; double t_recv, t_dec; int stg; uint64_t tag; uint32_t gen; }; // 読み出しを遅らせて持っているコマ
struct fmeta { int have; wc_frame_head fh; size_t len; double t_recv; };

static void emit_frame(struct vdec *dec, int stg, const wc_frame_head *pfh, size_t len, double t_recv, double t_dec, uint64_t tag, struct fctx *c) {
    const wc_frame_head fh = *pfh;
    const uint8_t *y;
    uint32_t pitch, rows;
    const double t_map0 = wc_qpc_us();
    const int mapped_ok = vdec_map_stg(dec, stg, &y, &pitch, &rows);
    const double t_map1 = wc_qpc_us();
    if (!mapped_ok) { vdec_unmap(dec); return; }
    g_us_push += t_dec - t_recv, g_us_map += t_map1 - t_map0;
    g_frames_dec++;
    // 大きさはコマの頭の値。途中で形が変わったとき、デコーダの出す絵の大きさが追いつく前でも、はみ出して読まないように揃える
    uint32_t W = fh.width ? fh.width : g_ack_w, H = fh.height ? fh.height : g_ack_h;
    if (W > pitch) W = pitch;
    if (H > rows) H = rows;
    W &= ~1u, H &= ~1u;
    const uint8_t *uv = y + (size_t) pitch * rows;
    int ctr = read_counter(y, pitch, W, H);
    // ほどけた絵の札は sample の時刻に入れた seq（vdec.tag）。その絵の記録を探す
    uint32_t seq = (uint32_t) tag;
    g_checked++;
    if (ctr >= 0 && (uint32_t) ctr != (seq & 0xffff)) g_bad_counter++;
    const int want_bgra = g_hwnd != NULL || g_opt.snap != NULL; // 窓も PNG も無ければ BGRA は作らない（4K でも軽く）
    if (want_bgra && ensure_buf(&c->back, &c->back_cap, (size_t) W * H * 4)) vid_nv12_to_bgra(y, pitch, uv, pitch, W, H, c->back);
    if (g_opt.pipe && ensure_buf(&c->packed, &c->packed_cap, (size_t) W * H * 3 / 2)) {
        for (uint32_t r = 0; r < H; r++) memcpy(c->packed + (size_t) r * W, y + (size_t) r * pitch, W);
        for (uint32_t r = 0; r < H / 2; r++) memcpy(c->packed + (size_t) W * H + (size_t) r * W, uv + (size_t) r * pitch, W);
        EnterCriticalSection(&g_plk);
        if (ensure_buf(&g_pbuf, &g_pcap, (size_t) W * H * 3 / 2)) { memcpy(g_pbuf, c->packed, (size_t) W * H * 3 / 2); g_pw = W, g_ph = H; g_ppts = (uint64_t) (fh.capture_us - g_off_us); g_phas = 1; }
        LeaveCriticalSection(&g_plk);
        SetEvent(g_pev);
    }
    const double t_w0 = wc_qpc_us();
    if (g_opt.shm) shm_write(y, uv, pitch, W, H, g_ack_fps, (uint64_t) (fh.capture_us - g_off_us));
    double t_conv = wc_qpc_us();
    g_us_write += t_conv - t_w0, g_us_other += t_w0 - t_map1;
    vdec_unmap(dec);
    if (g_nrec < MAXREC) {
        rec_t *r = &g_rec[g_nrec];
        r->cap = (double) fh.capture_us, r->sent = (double) fh.sent_us, r->recv = t_recv, r->dec = t_dec, r->conv = t_conv, r->drawn = 0;
        r->seq = fh.seq, r->bytes = (uint32_t) len, r->key = (fh.flags & WC_FRAME_KEY) != 0;
        if (g_opt.snap && c->back && (int) g_frames_dec == g_opt.snap_at) {
            if (write_png(g_opt.snap, c->back, W, H, W > 1280 ? 2 : 1)) LOG("1 コマを書いた：%s（%u x %u）", g_opt.snap, W, H);
        }
        if (g_hwnd && c->back) {
            EnterCriticalSection(&g_lk);
            if (ensure_buf(&g_front, &g_front_cap, (size_t) W * H * 4)) { memcpy(g_front, c->back, (size_t) W * H * 4); g_fw = W, g_fh = H, g_front_rec = (long) g_nrec; }
            LeaveCriticalSection(&g_lk);
            PostMessageW(g_hwnd, WM_APP, 0, 0);
        }
        g_nrec++;
    }
}

// 1 回つないで、切れるまで受ける。返すのは 1：一度は流れた・0：つなげない／断られた・-1：やめる（ほどけない）
static int session(void) {
    struct vdec dec;
    // デコーダは先に作る（つないだあとに作ると、その間に届いたコマが待たされて、最初の遅れが大きく見える）
    if (!vdec_open(&dec, !g_opt.swdec)) { LOG("H.264 をほどけない"); return -1; }
    if (!g_sessions) {
        LOG("ほどく：%s%s%s", dec.hw ? "GPU（D3D11 の DXVA）" : "ソフト", dec.hw && dec.adapter[0] ? "・アダプタ " : "", dec.hw ? dec.adapter : "");
        if (dec.adapter_missing) LOG("  --adapter %s に合うアダプタが無いので、Windows の既定を使った", g_opt.adapter);
    }
    SOCKET s = INVALID_SOCKET;
    double t_first_fail = 0, t_last_note = 0;
    for (int tries = 0; s == INVALID_SOCKET && !g_done; tries++) {
        int quiet = tries > 0;
        s = connect_once(quiet);
        if (s != INVALID_SOCKET) break;
        double now = wc_qpc_us();
        if (!t_first_fail) t_first_fail = now;
        if (!g_opt.reconnect && tries >= 19) break;
        if (g_opt.reconnect && now - t_last_note > 30e6) { LOG("iPhone を待っています（アプリで「ウェブカメラとして使う」をオンにして、前面に出してください）"); t_last_note = now; }
        for (int i = 0; i < (g_opt.reconnect ? 10 : 5) && !g_done; i++) Sleep(100);
    }
    if (s == INVALID_SOCKET) { if (!g_done) LOG("つなげない"); vdec_close(&dec); return 0; }
    int nd = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &nd, sizeof nd);
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *) &big, sizeof big);
    DWORD to = 30000; // 試験の偽の iPhone はソフトのエンコーダを作るのに 5 秒より長くかかることがある（webcam2 で 5 秒から延ばした）
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    wc_hello sent;
    int64_t used = 0;
    make_hello(&sent, wc_qpc_us(), &used);
    g_req_used = used;
    char ht[128];
    hello_text(&sent, ht, sizeof ht);
    LOG("iPhone に頼む：%s%s", ht, used ? "（仮想カメラを開いたアプリが選んだ形）" : "");
    if (g_clamp_w) LOG("  PC の指定は %u x %u ですが、仮想カメラは %u x %u までしか使えないので、その中に収めて頼む（元に戻すには --no-clamp）", g_clamp_w, g_clamp_h, g_box_w, g_box_h);
    wc_send_msg(s, WC_HELLO, &sent, sizeof sent);
    uint32_t type, len;
    uint8_t *body = NULL;
    wc_ack ack;
    if (!wc_recv_msg(s, &type, &body, &len, 1 << 12) || type != WC_HELLO_ACK || len < sizeof ack) { LOG("iPhone の返事が来ない"); free(body); closesocket(s); vdec_close(&dec); return 0; }
    memcpy(&ack, body, sizeof ack);
    free(body);
    if (ack.status) {
        LOG("iPhone が断った（status %d）", ack.status);
        if (used) g_req_bad = used; // アプリが選んだ形が断られたなら、形が変わるまでその頼みは使わない
        closesocket(s);
        vdec_close(&dec);
        return 0;
    }
    to = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    g_acks++;
    LOG("決まった形：%u x %u・%u コマ/秒・%u kbps・カメラ %u（%s）%s%s", ack.width, ack.height, ack.fps, ack.kbps, ack.camera, cam_name(ack.camera),
        ack.flags & WC_ACK_APP_PRIORITY ? "・アプリの設定を優先した" : ack.flags & WC_ACK_APP_SETTINGS ? "・アプリの設定を使った項目あり" : "",
        ack.flags & WC_ACK_LIVE ? "" : "・途中の切り替えに対応しない版のアプリ");
    if (sent.flags & WC_HELLO_FIT) LOG("  大きさの上限 %u x %u：%s", sent.max_w, sent.max_h, ack.flags & WC_ACK_FIT ? "iPhone が守る（新しいアプリ）" : "iPhone は知らない（古いアプリ。PC の指定だけを収めた）");
    if (sent.fps && ack.fps && ack.fps < sent.fps) LOG("注意：PC は %u コマ/秒を頼みましたが、iPhone は %u コマ/秒で返しました（この大きさでは iPhone が出せない・熱で下げた・iPhone のアプリが古い、など）", sent.fps, ack.fps);
    g_ack_w = ack.width, g_ack_h = ack.height, g_ack_fps = ack.fps;
    note_big(&ack); // dec1013
    shm_set_cur(&ack);
    // camctl2：ここからはパイプの糸も iPhone へ送る（SET）。送るのは g_slk の中で
    EnterCriticalSection(&g_slk);
    g_sock = s;
    LeaveCriticalSection(&g_slk);
    c_on_ack(&ack, 1);
    InterlockedExchange(&g_connected, 1);
    g_sessions++;
    if (!g_t_start) g_t_start = wc_qpc_us();

    struct fctx fc;
    memset(&fc, 0, sizeof fc);
    struct dfr_t dfr;
    memset(&dfr, 0, sizeof dfr);
    struct fmeta hist[32]; // 入れたコマの頭（札＝seq で引く。1 回で出る絵が入力より古いことがあるため）
    memset(hist, 0, sizeof hist);
    uint32_t last_seq = 0;
    int have_last = 0;
    double next_ping = 0, last_key_req = 0;
    double win_t0 = 0, win_up = 0, win_um = 0, win_uw = 0, win_uo = 0;
    uint64_t win_nn = 0, win_nm = 0;
    size_t win_r0 = 0, win_d0 = 0;
    uint64_t win_s0 = 0;
    wc_hello pend;
    double pend_since = 0;
    int pending = 0;
    memset(&pend, 0, sizeof pend);
    // 時計合わせ：最初に 6 回
    for (int i = 0; i < 6; i++) { uint64_t t0 = wc_now_us(); ssend(s, WC_PING, &t0, 8); }
    int ok = 1;
    while (!g_done && ok) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);
        struct timeval tv = { 0, 200000 };
        int sel = select(0, &rf, NULL, NULL, &tv);
        if (sel < 0) break;
        if (sel > 0) {
            if (!wc_recv_msg(s, &type, &body, &len, 16u << 20)) break;
            double t_recv = wc_qpc_us();
            if (type == WC_PONG && len >= 16) {
                uint64_t v[2];
                memcpy(v, body, 16);
                double rtt = t_recv - (double) v[0];
                if (rtt < g_best_rtt) { g_best_rtt = rtt; g_off_us = (double) v[1] - ((double) v[0] + rtt / 2); }
            } else if (type == WC_EVENT && len >= 8) {
                wc_event ev;
                memcpy(&ev, body, 8);
                LOG("iPhone の知らせ：%u（%u）", ev.code, ev.arg);
                c_on_event(&ev);
            } else if (type == WC_CTRL_CAPS) {
                c_on_caps(body, len); // camctl2
            } else if (type == WC_CTRL_STATE) {
                c_on_state(body, len); // camctl2
            } else if (type == WC_HELLO_ACK && len >= sizeof ack) {
                // 流している途中の ACK：PC の頼み直しへの返事か、アプリの画面で設定が変わった
                memcpy(&ack, body, sizeof ack);
                if (ack.status) {
                    LOG("iPhone が頼み直しを断った（status %d）。このあと iPhone が切る", ack.status);
                    if (g_req_used) g_req_bad = g_req_used;
                } else {
                    g_acks++;
                    LOG("形が変わった：%u x %u・%u コマ/秒・%u kbps・カメラ %u（%s）%s", ack.width, ack.height, ack.fps, ack.kbps, ack.camera, cam_name(ack.camera),
                        ack.flags & WC_ACK_APP_PRIORITY ? "・アプリの設定を優先した" : "");
                    if (sent.fps && ack.fps && ack.fps < sent.fps && !(ack.flags & WC_ACK_THERMAL)) LOG("注意：PC は %u コマ/秒を頼みましたが、iPhone は %u コマ/秒で返しました", sent.fps, ack.fps);
                    g_ack_w = ack.width, g_ack_h = ack.height, g_ack_fps = ack.fps;
                    note_big(&ack); // dec1013
                    shm_set_cur(&ack);
                    c_on_ack(&ack, 0);
                }
            } else if (type == WC_FRAME && len > sizeof(wc_frame_head)) {
                wc_frame_head fh;
                memcpy(&fh, body, sizeof fh);
                g_frames_recv++;
                g_bytes_total += len;
                if (have_last && fh.seq != last_seq + 1 && fh.seq > last_seq) g_gaps += fh.seq - last_seq - 1;
                last_seq = fh.seq, have_last = 1;
                { struct fmeta *hm = &hist[fh.seq & 31]; hm->have = 1, hm->fh = fh, hm->len = len, hm->t_recv = t_recv; }
                int k = vdec_push(&dec, body + sizeof fh, len - sizeof fh, fh.seq);
                double t_dec = wc_qpc_us();
                if (k < 0) {
                    g_decode_fail++;
                    if (t_dec - last_key_req > 1e6) { ssend(s, WC_REQ_KEY, NULL, 0); last_key_req = t_dec; }
                } else if (k > 0) {
                    // cam4k：GPU でほどく 4K の 60 コマ/秒は、「ほどけるのを待って読み出す」だけで 1 コマの間隔（16.7 ms）を超える（3060 で 12〜22 ms）。
                    // 読み出し（Map）を次のコマが来るまで 1 コマ遅らせ、次のコマをほどく間に前のコマを共有メモリへ書く。遅れは 1 コマの間隔ぶん（16.7 ms）増える。
                    // 4K でも 30 コマ/秒以下・1080p は今までどおり（遅らせない）
                    if (dec.staged) {
                        // GPU でほどいた絵。1 回の入力で絵が 2 つ以上出ることがあるので（4K で毎秒 2 回ほど）、出た絵を古い順に全部渡す。
                        // どの入力の絵かは札（seq）で hist から探す
                        const int defer = g_ack_fps >= 50 && (uint64_t) dec.fw * dec.fh >= 2500000u && !g_opt.nodefer;
                        int stg;
                        uint64_t tag;
                        while (vdec_out_pop(&dec, &stg, &tag)) {
                            const struct fmeta *m = &hist[tag & 31];
                            const int known = m->have && m->fh.seq == (uint32_t) tag;
                            const wc_frame_head fh2 = known ? m->fh : fh;
                            const size_t len2 = known ? m->len : len;
                            const double tr2 = known ? m->t_recv : t_recv;
                            if (dfr.have) {
                                if (dfr.gen == dec.stg_gen) emit_frame(&dec, dfr.stg, &dfr.fh, dfr.len, dfr.t_recv, dfr.t_dec, dfr.tag, &fc);
                                dfr.have = 0;
                            }
                            if (defer) dfr.have = 1, dfr.fh = fh2, dfr.len = len2, dfr.t_recv = tr2, dfr.t_dec = t_dec, dfr.stg = stg, dfr.tag = tag, dfr.gen = dec.stg_gen;
                            else emit_frame(&dec, stg, &fh2, len2, tr2, t_dec, tag, &fc);
                        }
                    } else {
                        emit_frame(&dec, dec.cur_stg, &fh, len, t_recv, t_dec, dec.tag, &fc);
                    }
                }
            }
            free(body);
        }
        // 遅らせて持っているコマが 40 ms 以上待っている（次のコマが来ない）ときは、読み出して渡す
        if (dfr.have && wc_qpc_us() - dfr.t_dec > 40000) {
            if (dfr.gen == dec.stg_gen) emit_frame(&dec, dfr.stg, &dfr.fh, dfr.len, dfr.t_recv, dfr.t_dec, dfr.tag, &fc);
            dfr.have = 0;
        }
        double now = wc_qpc_us();
        // アプリ（OBS など）が選んだ形が変わったら、少し待って（開き直しの続けざまをまとめる）から iPhone に頼み直す
        // camctl2：PC アプリの set_format で形が変わったときも（--shm が無くても）頼み直す
        if ((g_opt.follow && g_shm) || g_hello_by_app) {
            wc_hello want;
            int64_t u = 0;
            make_hello(&want, now, &u);
            if (hello_same(&want, &sent)) { pending = 0; InterlockedExchange(&g_hello_by_app, 0); }
            else if (!pending || !hello_same(&want, &pend)) { pend = want; pend_since = now; pending = 1; }
            else if (now - pend_since >= 400000) {
                hello_text(&want, ht, sizeof ht);
                int by_app = InterlockedExchange(&g_hello_by_app, 0) != 0;
                LOG("iPhone に頼み直す：%s%s", ht, by_app ? "（PC アプリが選んだ形）" : u ? "（仮想カメラを開いたアプリが選んだ形）" : "（アプリが閉じたので、おまかせに戻す）");
                if (g_clamp_w) LOG("  PC の指定は %u x %u ですが、仮想カメラは %u x %u までしか使えないので、その中に収めて頼む", g_clamp_w, g_clamp_h, g_box_w, g_box_h);
                if (!ssend(s, WC_HELLO, &want, sizeof want)) break;
                sent = want, g_req_used = u, pending = 0;
                g_rehello++;
            }
        }
        if (now >= next_ping) { uint64_t t0 = wc_now_us(); ssend(s, WC_PING, &t0, 8); next_ping = now + 1e6; }
        // cam4k：10 秒ごとに、実際に届いたコマ数を書く（頼んだ数・iPhone が決めた数と並べて見る）
        if (!win_t0) win_t0 = now, win_r0 = g_frames_recv, win_d0 = g_frames_dec, win_s0 = g_shm_frames;
        else if (now - win_t0 >= 10e6) {
            const double dt = (now - win_t0) / 1e6;
            const double rr = (double) (g_frames_recv - win_r0) / dt, dd = (double) (g_frames_dec - win_d0) / dt, ss = (double) (g_shm_frames - win_s0) / dt;
            if (g_frames_recv > win_r0) {
                LOG("実際のコマ数（直近 %.0f 秒）：iPhone から受信 %.1f・ほどいた %.1f・共有メモリへ %.1f コマ/秒（PC が頼んだ %u・iPhone が決めた %u）%s", dt, rr, dd, ss, sent.fps, g_ack_fps,
                    g_ack_fps && rr < g_ack_fps * 0.85 ? "  ← 決めた数に届いていない" : "");
                LOG("  ほどくとき：入力しても絵が出なかった %llu 回・1 回で絵が 2 つ以上出た %llu 回（直近 %.0f 秒。2 つ出たときは前の絵を捨てる）", (unsigned long long) (dec.n_none - win_nn), (unsigned long long) (dec.n_multi - win_nm), dt);
                win_nn = dec.n_none, win_nm = dec.n_multi;
                const double nf = dd * dt > 0 ? dd * dt : 1;
                LOG("  1 コマの時間（平均 ms）：入れる %.2f・ほどけるのを待って読み出す %.2f・共有メモリへ書く %.2f・そのほか %.2f（コマの間隔 %.1f ms）", (g_us_push - win_up) / nf / 1000, (g_us_map - win_um) / nf / 1000,
                    (g_us_write - win_uw) / nf / 1000, (g_us_other - win_uo) / nf / 1000, g_ack_fps ? 1000.0 / g_ack_fps : 0.0);
            }
            win_up = g_us_push, win_um = g_us_map, win_uw = g_us_write, win_uo = g_us_other;
            win_t0 = now, win_r0 = g_frames_recv, win_d0 = g_frames_dec, win_s0 = g_shm_frames;
        }
        if (g_opt.secs && (now - g_t_start) / 1e6 >= g_opt.secs) { InterlockedExchange(&g_done, 1); break; }
    }
    EnterCriticalSection(&g_slk);
    g_sock = INVALID_SOCKET;
    LeaveCriticalSection(&g_slk);
    closesocket(s);
    c_on_disconnect(); // camctl2
    InterlockedExchange(&g_connected, 0);
    if (g_shm) g_shm->cur = 0, g_shm->cur_full = 0;
    free(fc.back);
    free(fc.packed);
    vdec_close(&dec);
    if (!g_done) LOG("iPhone との接続が切れた");
    return 1;
}

static DWORD WINAPI alog_thread(LPVOID p) {
    (void) p;
    while (!g_done) { WaitForSingleObject(g_alev, 200); alog_drain(); }
    return 0;
}

static DWORD WINAPI net_thread(LPVOID p) {
    (void) p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!g_done) {
        int r = session();
        if (r < 0 || !g_opt.reconnect || g_done) break;
        for (int i = 0; i < 10 && !g_done; i++) Sleep(100); // つなぎ直す前に 1 秒
    }
    InterlockedExchange(&g_done, 1);
    // 窓の最後の描き直しを待ってから報告する
    Sleep(100);
    report(g_t_start ? (wc_qpc_us() - g_t_start) / 1e6 : 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

// ---------------------------------------------------------------- 窓
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_APP: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EnterCriticalSection(&g_lk);
        if (g_front && g_fw) {
            RECT rc;
            GetClientRect(h, &rc);
            BITMAPINFO bi;
            memset(&bi, 0, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = (LONG) g_fw, bi.bmiHeader.biHeight = -(LONG) g_fh;
            bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, (int) g_fw, (int) g_fh, g_front, &bi, DIB_RGB_COLORS, SRCCOPY);
            GdiFlush();
            if (g_front_rec >= 0 && g_rec[g_front_rec].drawn == 0) { g_rec[g_front_rec].drawn = wc_qpc_us(); g_drawn_n++; }
        }
        LeaveCriticalSection(&g_lk);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Ctrl+C・ウィンドウを閉じた：受けるのをやめて、共有メモリを片づけてから終わる
static BOOL WINAPI on_ctrl(DWORD t) {
    (void) t;
    InterlockedExchange(&g_done, 1);
    return TRUE;
}

static int list_devices(void) {
    wc_dev d[32];
    int n = wc_usb_list(g_opt.mux, d, 32);
    if (n < 0) { printf("Apple Mobile Device Service（usbmuxd）につながりません\n"); return 1; }
    printf("DeviceID\t種類\tProductID\tUDID\n");
    for (int i = 0; i < n; i++) printf("%lld\t%s\t%lld\t%s\n", d[i].id, d[i].type[0] ? d[i].type : "?", d[i].product, d[i].udid[0] ? d[i].udid : "?");
    if (!n) printf("（つながっている iPhone はありません）\n");
    return 0;
}

static int parse_camera(const char *v, wc_hello *h) {
    struct { const char *n; uint32_t c; int set; } t[] = { { "auto", 0, 0 }, { "back", 0, 1 }, { "wide", 0, 1 }, { "front", 1, 1 }, { "ultrawide", 2, 1 }, { "tele", 3, 1 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!_stricmp(v, t[i].n)) { h->camera = t[i].c; if (t[i].set) h->flags |= WC_HELLO_CAMERA_SET; else h->flags &= ~WC_HELLO_CAMERA_SET; return 1; }
    if (*v >= '0' && *v <= '3' && !v[1]) { h->camera = (uint32_t) (*v - '0'); return 1; } // 前の版と同じ：0 はおまかせ
    return 0;
}
static int parse_quality(const char *v, uint32_t *q) {
    const char *n[] = { "auto", "standard", "high", "max" };
    for (uint32_t i = 0; i < 4; i++) if (!_stricmp(v, n[i])) { *q = i; return 1; }
    if (*v >= '0' && *v <= '3' && !v[1]) { *q = (uint32_t) (*v - '0'); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    log_clock_init();
    g_opt.hello.version = WCAM_VERSION;
    g_opt.snap_at = 90;
    g_opt.follow = 1;
    g_opt.ctl = 1;
    g_opt.ctl_pipe = "pocketgpu-camctl";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--usb")) g_opt.usb = 1;
        else if (!strcmp(a, "--mux") && i + 1 < argc) g_opt.mux = atoi(argv[++i]);
        else if (!strcmp(a, "--device") && i + 1 < argc) g_opt.device = argv[++i];
        else if (!strcmp(a, "--list")) g_opt.list = 1;
        else if (!strcmp(a, "--addr") && i + 1 < argc) g_opt.addr = argv[++i];
        else if (!strcmp(a, "--w") && i + 1 < argc) g_opt.hello.width = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--h") && i + 1 < argc) g_opt.hello.height = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--fps") && i + 1 < argc) g_opt.hello.fps = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--kbps") && i + 1 < argc) g_opt.hello.kbps = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--gop") && i + 1 < argc) g_opt.hello.gop = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--camera") && i + 1 < argc) { if (!parse_camera(argv[++i], &g_opt.hello)) { fprintf(stderr, "--camera は auto・back・front・ultrawide・tele\n"); return 2; } }
        else if (!strcmp(a, "--quality") && i + 1 < argc) { if (!parse_quality(argv[++i], &g_opt.hello.quality)) { fprintf(stderr, "--quality は auto・standard・high・max\n"); return 2; } }
        else if (!strcmp(a, "--mirror")) g_opt.hello.flags |= WC_HELLO_MIRROR;
        else if (!strcmp(a, "--code") && i + 1 < argc) g_opt.hello.code = (uint32_t) atoi(argv[++i]); // Wi-Fi：--addr iPhoneの住所:50064 --code 画面の6けた
        else if (!strcmp(a, "--secs") && i + 1 < argc) g_opt.secs = atoi(argv[++i]);
        else if (!strcmp(a, "--nowin")) g_opt.nowin = 1;
        else if (!strcmp(a, "--pipe")) g_opt.pipe = 1;
        else if (!strcmp(a, "--shm")) g_opt.shm = 1;
        else if (!strcmp(a, "--no-follow")) g_opt.follow = 0;
        else if (!strcmp(a, "--reconnect")) g_opt.reconnect = 1;
        else if (!strcmp(a, "--swdec")) g_opt.swdec = 1;
        else if (!strcmp(a, "--decode") && i + 1 < argc) { if (!parse_decode(argv[++i])) { fprintf(stderr, "--decode は auto・gpu・cpu\n"); return 2; } }
        else if (!strcmp(a, "--no-clamp")) g_opt.noclamp = 1;
        else if (!strcmp(a, "--vcam-max") && i + 1 < argc) g_opt.vcam_max = atoi(argv[++i]);
        else if (!strcmp(a, "--no-defer")) g_opt.nodefer = 1;
        else if (!strcmp(a, "--adapter") && i + 1 < argc) g_opt.adapter = argv[++i];
        else if (!strcmp(a, "--snap") && i + 1 < argc) g_opt.snap = argv[++i];
        else if (!strcmp(a, "--snap-at") && i + 1 < argc) g_opt.snap_at = atoi(argv[++i]);
        else if (!strcmp(a, "--csv") && i + 1 < argc) g_opt.csv = argv[++i];
        else if (!strcmp(a, "--ctl-pipe") && i + 1 < argc) g_opt.ctl_pipe = argv[++i];
        else if (!strcmp(a, "--no-ctl")) g_opt.ctl = 0;
        else if (!strcmp(a, "--test-slow-log") && i + 1 < argc) g_test_slow_log = atoi(argv[++i]);
        else { fprintf(stderr, "知らない引数：%s\n", a); return 2; }
    }
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    if (g_opt.list) return list_devices();
    if (!g_opt.usb && !g_opt.addr) {
        fprintf(stderr, "使い方：webcam_rx --usb [--device UDID] か --addr host:port（--list --secs N --nowin --pipe --shm --reconnect --swdec --snap f.png "
                        "--w --h --fps --camera auto|back|front|ultrawide|tele --quality auto|standard|high|max --ctl-pipe 名前 --no-ctl --no-clamp --vcam-max 2160 --adapter 名前 --decode auto|gpu|cpu）\n");
        return 2;
    }
    // dec1013：環境変数（PocketGPU や試験から、引数を足さずに切り替える）
    {
        char ev[8];
        if (GetEnvironmentVariableA("POCKETGPU_NO_CLAMP", ev, sizeof ev) > 0 && ev[0] == '1') g_opt.noclamp = 1;
        static char adbuf[64];
        if (!g_opt.adapter && GetEnvironmentVariableA("POCKETGPU_ADAPTER", adbuf, sizeof adbuf) > 0 && adbuf[0]) g_opt.adapter = adbuf;
        char dv[16];
        if (GetEnvironmentVariableA("POCKETGPU_DECODE", dv, sizeof dv) > 0 && dv[0] && !g_opt.swdec) parse_decode(dv); // camgpu1015：引数が cpu でなければ環境変数を見る
        DWORD nn = GetEnvironmentVariableW(L"PGPU_TEST_SHM", g_shm_name_buf, 128);
        if (nn > 0 && nn < 128) g_shm_name = g_shm_name_buf, g_names_test = 1;
        // cam4k：4K を仮想カメラが名乗る（PC アプリが 4K を選んだとき環境変数で渡す。古い webcam_rx は環境変数を見ないので、PC アプリは引数でなく環境変数で渡す）
        char vm[16];
        if (!g_opt.vcam_max && GetEnvironmentVariableA("POCKETGPU_VCAM_MAX", vm, sizeof vm) > 0) g_opt.vcam_max = atoi(vm);
    }
    if (g_opt.vcam_max != 2160) g_opt.vcam_max = 1080;
    g_vdec_adapter = g_opt.adapter;
    if (g_opt.shm && !shm_open_writer()) return 1;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    InitializeCriticalSection(&g_lk);
    InitializeCriticalSection(&g_plk);
    InitializeCriticalSection(&g_clk);
    InitializeCriticalSection(&g_cqlk);
    InitializeCriticalSection(&g_slk);
    InitializeCriticalSection(&g_hlk);
    InitializeCriticalSection(&g_alk);
    g_cq_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_alev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE at = CreateThread(NULL, 0, alog_thread, NULL, 0, NULL); // camctl2：パイプの糸の記録を書く糸
    g_pev = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_rec = (rec_t *) calloc(MAXREC, sizeof(rec_t));
    HANDLE pt = g_opt.pipe ? CreateThread(NULL, 0, pipe_thread, NULL, 0, NULL) : NULL;
    HANDLE ct = g_opt.ctl ? CreateThread(NULL, 0, ctl_pipe_thread, NULL, 0, NULL) : NULL; // camctl2：PC アプリとの口
    if (!g_opt.nowin) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc, wc.hInstance = GetModuleHandleW(NULL), wc.lpszClassName = L"WcamRx", wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
        RegisterClassW(&wc);
        g_hwnd = CreateWindowExW(0, L"WcamRx", L"iPhone カメラ（試作）", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 960, 540, NULL, NULL, wc.hInstance, NULL);
    }
    HANDLE nt = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (g_hwnd) {
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        InterlockedExchange(&g_done, 1);
    }
    // webcam2 で直した：もとは 30 秒で打ち切っていたので、--nowin で長く動かすと途中で黙って終わった。
    // 受け終わる（g_done）までは待ち続け、そのあとの片づけ（報告・デコーダを閉じる）は 5 秒まで待つ。デコーダを閉じる所で止まることがあったため
    if (g_opt.nowin)
        while (!g_done && WaitForSingleObject(nt, 200) == WAIT_TIMEOUT) {}
    WaitForSingleObject(nt, g_opt.nowin ? 5000 : 30000);
    InterlockedExchange(&g_done, 1);
    if (pt) { SetEvent(g_pev); WaitForSingleObject(pt, 3000); }
    if (ct) { SetEvent(g_cq_ev); WaitForSingleObject(ct, 3000); }
    if (at) { SetEvent(g_alev); WaitForSingleObject(at, 1000); }
    alog_drain();
    shm_close();
    return 0;
}
