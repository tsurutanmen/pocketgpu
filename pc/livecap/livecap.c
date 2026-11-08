// livecap.exe：PC で鳴っている音（またはマイク）を、USB の iPhone の文字起こし（iOS 26 の SpeechAnalyzer）に流しっぱなしにして字幕にする。
// work/ai_apps/live_caption.py（Python 版）を、Python と numpy なしの小さな exe に作り直した物。オプションと設定の JSON は Python 版と同じ。
//   音を取る（WASAPI の loopback・マイク・WAV）→ 16 kHz・モノラル・s16 → 100 ms ずつ iPhone へ → 途中の文（黄）と確定した文（白）
//   → 字幕のウィンドウ・OBS 用のテキストファイル・時刻つきの .txt と .srt。--translate で iPhone の翻訳も。
// 遅れの測り方も Python 版と同じ：その文の最後の音を PC が取った時刻から、その文がウィンドウに出た時刻まで。終わりに中央値を出す。
//
//   組み方は build.sh（zig cc）。要る DLL は Windows に入っている物だけ。
#define COBJMACROS
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#include "lc_text.h"
#include "lc_util.h"
#include "lc_resample.h"
#include "lc_audio.h"
#include "lc_link.h"
#include "lc_overlay.h"

// ---- オプション（Python 版と同じ名前）----
typedef struct {
    const char *source, *lang, *translate, *addr, *mux, *obs, *minutes, *geometry, *latency_csv, *save_audio, *settings, *ui;
    int accurate, no_window, no_keepalive, obs_lines, lines, font_size, font_given, chunk_ms, quiet, selfcheck, list, help;
    double opacity, seconds, end_wait;
} Opt;
static Opt O;

static int parse_args(int argc, char **argv) {
    O.source = "loopback";
    O.lang = "ja-JP";
    O.obs_lines = 2;
    O.lines = 2;
    O.font_size = 28;
    O.opacity = 0.82;
    O.chunk_ms = 100;
    O.end_wait = 5.0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i], *val = NULL;
        char *eq = strncmp(a, "--", 2) == 0 ? strchr(a, '=') : NULL;
        if (eq) { *eq = 0; val = eq + 1; }
#define FLAG(name, field) else if (!strcmp(a, name)) { O.field = 1; }
#define VAL(name) (!strcmp(a, name) && (val || (i + 1 < argc ? (val = argv[++i], 1) : (warn(TT(T_NEED_VALUE), name), exit(2), 0))))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) O.help = 1;
        FLAG("--accurate", accurate)
        FLAG("--no-window", no_window)
        FLAG("--no-keepalive", no_keepalive)
        FLAG("--quiet", quiet)
        FLAG("--selfcheck", selfcheck)
        FLAG("--list", list)
        else if (VAL("--source")) O.source = val;
        else if (VAL("--lang")) O.lang = val;
        else if (VAL("--translate")) O.translate = val;
        else if (VAL("--addr")) O.addr = val;
        else if (VAL("--mux")) O.mux = val;
        else if (VAL("--obs")) O.obs = val;
        else if (VAL("--obs-lines")) O.obs_lines = atoi(val);
        else if (VAL("--minutes")) O.minutes = val;
        else if (VAL("--lines")) O.lines = atoi(val);
        else if (VAL("--font-size")) { O.font_size = atoi(val); O.font_given = 1; }
        else if (VAL("--opacity")) O.opacity = atof(val);
        else if (VAL("--geometry")) O.geometry = val;
        else if (VAL("--settings")) O.settings = val;
        else if (VAL("--chunk-ms")) O.chunk_ms = atoi(val);
        else if (VAL("--seconds")) O.seconds = atof(val);
        else if (VAL("--end-wait")) O.end_wait = atof(val);
        else if (VAL("--latency-csv")) O.latency_csv = val;
        else if (VAL("--save-audio")) O.save_audio = val;
        else if (VAL("--ui")) O.ui = val;
        else { warn(TT(T_UNKNOWN_ARG), a); return 0; }
#undef FLAG
#undef VAL
    }
    if (O.lines < 1) O.lines = 1;
    if (O.obs_lines < 1) O.obs_lines = 1;
    if (O.chunk_ms < 10) O.chunk_ms = 10;
    if (O.chunk_ms > 1000) O.chunk_ms = 1000;
    return 1;
}

// ---- 音のコマ番号 → 取った時刻（送る糸が足し、本体の糸が読む）----
static struct {
    SRWLOCK lk;
    long long *ends;  // 塊の最後のコマ番号（の次）
    double *times;    // その塊の最後のコマを取った時刻
    int n, cap;
} CM;
static void cm_add(long long end, double t) {
    AcquireSRWLockExclusive(&CM.lk);
    if (CM.n >= 400000) {  // 11 時間ぶん。古い半分を捨てる
        memmove(CM.ends, CM.ends + 200000, sizeof(long long) * (size_t) (CM.n - 200000));
        memmove(CM.times, CM.times + 200000, sizeof(double) * (size_t) (CM.n - 200000));
        CM.n -= 200000;
    }
    if (CM.n == CM.cap) {
        int c = CM.cap ? CM.cap * 2 : 1024;
        long long *e = realloc(CM.ends, sizeof(long long) * (size_t) c);
        double *t2 = e ? realloc(CM.times, sizeof(double) * (size_t) c) : NULL;
        if (e) CM.ends = e;
        if (t2) CM.times = t2;
        if (e && t2) CM.cap = c;
    }
    if (CM.n < CM.cap) {
        CM.ends[CM.n] = end;
        CM.times[CM.n] = t;
        CM.n++;
    }
    ReleaseSRWLockExclusive(&CM.lk);
}
static int cm_time_of(long long idx, double *out) {
    AcquireSRWLockShared(&CM.lk);
    int ok = CM.n > 0;
    if (ok) {
        int lo = 0, hi = CM.n;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (CM.ends[mid] < idx) lo = mid + 1; else hi = mid;
        }
        if (lo >= CM.n) lo = CM.n - 1;
        *out = CM.times[lo] - (double) (CM.ends[lo] - idx) / OUT_RATE;
    }
    ReleaseSRWLockShared(&CM.lk);
    return ok;
}

// ---- 送る：取った音を chunk_ms の塊にまとめて iPhone へ。2 秒ごとに ping も ----
static struct {
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    int16_t *buf;
    size_t len, cap;
    double t_last;  // 最後に積んだコマを取った時刻
    int chunk;
    volatile LONG finishing, error;
    long long sent, dropped;
    HANDLE th;
    FILE *wav;
    long long wav_frames;
} SND;

static void snd_push(const int16_t *pcm, int n, double t_end) {
    AcquireSRWLockExclusive(&SND.lk);
    if (SND.len + (size_t) n > SND.cap) {
        size_t c = SND.cap ? SND.cap * 2 : (size_t) OUT_RATE * 4;
        while (c < SND.len + (size_t) n) c *= 2;
        int16_t *q = c <= (size_t) OUT_RATE * 3600 ? realloc(SND.buf, sizeof(int16_t) * c) : NULL;  // 1 時間ぶんまで
        if (q) { SND.buf = q; SND.cap = c; }
    }
    if (SND.len + (size_t) n <= SND.cap) {
        memcpy(SND.buf + SND.len, pcm, sizeof(int16_t) * (size_t) n);
        SND.len += (size_t) n;
        SND.t_last = t_end;
    } else {
        SND.dropped += n;
    }
    int wake = SND.len >= (size_t) SND.chunk;
    ReleaseSRWLockExclusive(&SND.lk);
    if (wake) WakeConditionVariable(&SND.cv);
}

static void wav_header(FILE *f, long long frames) {
    uint32_t data = (uint32_t) (frames * 2), riff = 36 + data, fmt = 16, rate = OUT_RATE, br = OUT_RATE * 2;
    uint16_t pcm = 1, ch = 1, ba = 2, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static DWORD WINAPI snd_thread(LPVOID arg) {
    (void) arg;
    int16_t *part = malloc(sizeof(int16_t) * (size_t) SND.chunk);
    double next_ping = now_s() + 2.0;
    for (;;) {
        AcquireSRWLockExclusive(&SND.lk);
        while (SND.len < (size_t) SND.chunk && !SND.finishing) {
            double w = next_ping - now_s();
            if (w <= 0) break;
            SleepConditionVariableSRW(&SND.cv, &SND.lk, (DWORD) (w * 1000) + 1, 0);
        }
        int take = SND.len >= (size_t) SND.chunk ? SND.chunk : SND.finishing ? (int) SND.len : 0;
        double t_end = 0;
        if (take) {
            memcpy(part, SND.buf, sizeof(int16_t) * (size_t) take);
            memmove(SND.buf, SND.buf + take, sizeof(int16_t) * (SND.len - (size_t) take));
            SND.len -= (size_t) take;
            t_end = SND.t_last - (double) SND.len / OUT_RATE;  // まだ送っていない分だけ前にずらす
        }
        int fin = SND.finishing && SND.len == 0;
        ReleaseSRWLockExclusive(&SND.lk);
        if (take) {
            long long at = SND.sent;
            SND.sent += take;
            cm_add(SND.sent, t_end);
            if (SND.wav) {
                fwrite(part, sizeof(int16_t), (size_t) take, SND.wav);
                SND.wav_frames += take;
            }
            if (!live_send_pcm(part, take, at)) InterlockedExchange(&SND.error, 1);
        }
        if (now_s() >= next_ping) {
            live_ping(now_s());
            next_ping = now_s() + 2.0;
        }
        if (fin) break;
    }
    free(part);
    return 0;
}

static void snd_start(void) {
    InitializeSRWLock(&SND.lk);
    InitializeConditionVariable(&SND.cv);
    SND.chunk = OUT_RATE * O.chunk_ms / 1000;
    if (O.save_audio) {
        mkdirs_parent_u8(O.save_audio);
        SND.wav = fopen_u8(O.save_audio, L"wb");
        if (SND.wav) wav_header(SND.wav, 0);
        else warn(TT(T_FILE_FAIL), O.save_audio);
    }
    SND.th = CreateThread(NULL, 0, snd_thread, NULL, 0, NULL);
}
// 残りを送ってから止める（2 回呼んでも 1 回）
static void snd_finish(void) {
    if (!SND.th) return;
    InterlockedExchange(&SND.finishing, 1);
    WakeConditionVariable(&SND.cv);
    WaitForSingleObject(SND.th, 5000);
    CloseHandle(SND.th);
    SND.th = NULL;
    if (SND.wav) {
        wav_header(SND.wav, SND.wav_frames);
        fclose(SND.wav);
        SND.wav = NULL;
    }
}

// ---- 字幕の中身 ----
#define NFIN 50
typedef struct {
    double start, end;
    char *text, *tr;
    int fid, pending;  // pending：訳を待っていて、まだ議事録に書いていない
    double wall;
} Fin;
static struct {
    Fin f[NFIN];
    int n, head;
    char *vol;
    double vol_start;
    int has_vs, next_id;
    long long total;
} CAP;
static Fin *cap_at(int i) { return &CAP.f[(CAP.head + i) % NFIN]; }  // 0 がいちばん古い
static Fin *cap_find(int fid) {
    for (int i = CAP.n - 1; i >= 0; i--)
        if (cap_at(i)->fid == fid) return cap_at(i);
    return NULL;
}

// ---- 議事録：[時:分:秒] 文 の .txt と、音の始まりからの時間の .srt ----
static struct { FILE *txt, *srt; int n; char *stem; } MIN;

static void fput_crlf(FILE *f, const char *s) {  // Python 版（Windows の文字のモード）と同じく改行は CRLF
    for (; *s; s++) {
        if (*s == '\n') fputc('\r', f);
        fputc(*s, f);
    }
}
static void srt_time(char *out, size_t n, double s) {
    long long ms = llround(s * 1000);
    if (ms < 0) ms = 0;
    snprintf(out, n, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
}
static int min_open(const char *folder) {
    mkdirs_u8(folder);
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    Buf b = { 0 };
    size_t fl = strlen(folder);
    buf_printf(&b, "%s%scaption_%04d%02d%02d_%02d%02d%02d", folder, fl && (folder[fl - 1] == '\\' || folder[fl - 1] == '/') ? "" : "\\",
               lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
    MIN.stem = b.p;
    Buf t = { 0 }, s = { 0 };
    buf_printf(&t, "%s.txt", MIN.stem);
    buf_printf(&s, "%s.srt", MIN.stem);
    MIN.txt = fopen_u8(t.p, L"ab");
    MIN.srt = fopen_u8(s.p, L"ab");
    if (!MIN.txt || !MIN.srt) warn(TT(T_FILE_FAIL), MIN.txt ? s.p : t.p);
    buf_free(&t);
    buf_free(&s);
    return MIN.txt && MIN.srt;
}
// wall：その文の音の始まりの時刻（FILETIME の 100 ns 単位）
static void min_add(double wall, double start, double end, const char *text, const char *tr) {
    if (!MIN.txt || !MIN.srt) return;
    MIN.n++;
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG) wall;
    FILETIME ft = { u.LowPart, u.HighPart };
    SYSTEMTIME su, lt;
    FileTimeToSystemTime(&ft, &su);
    SystemTimeToTzSpecificLocalTime(NULL, &su, &lt);
    Buf b = { 0 };
    buf_printf(&b, "[%02d:%02d:%02d] %s\n", lt.wHour, lt.wMinute, lt.wSecond, text);
    if (tr) buf_printf(&b, "           → %s\n", tr);
    fput_crlf(MIN.txt, b.p);
    fflush(MIN.txt);
    buf_free(&b);
    char a[32], z[32];
    srt_time(a, sizeof a, start);
    srt_time(z, sizeof z, end > start + 0.5 ? end : start + 0.5);
    buf_printf(&b, "%d\n%s --> %s\n%s%s%s\n\n", MIN.n, a, z, text, tr ? "\n" : "", tr ? tr : "");
    fput_crlf(MIN.srt, b.p);
    fflush(MIN.srt);
    buf_free(&b);
}
static double wall_of(double t_perf) {  // QPC の時刻 → 壁の時計（FILETIME の 100 ns 単位）
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u = { { ft.dwLowDateTime, ft.dwHighDateTime } };
    return (double) u.QuadPart - (now_s() - t_perf) * 1e7;
}

// ---- 遅れの記録 ----
typedef struct { char kind; double start, end, ms; char *text; } LRow;  // kind：p 途中・f 確定・t 話し始めから最初の文字
static struct { LRow *r; int n, cap; long long *seen; int nseen, capseen; } LAT;

static void lat_row(char kind, double start, double end, double ms, const char *text) {
    if (LAT.n == LAT.cap) {
        int c = LAT.cap ? LAT.cap * 2 : 256;
        LRow *q = realloc(LAT.r, sizeof(LRow) * (size_t) c);
        if (!q) return;
        LAT.r = q;
        LAT.cap = c;
    }
    LAT.r[LAT.n++] = (LRow) { kind, round(start * 1000) / 1000, round(end * 1000) / 1000, round(ms * 10) / 10, O.latency_csv ? xstrdup(text) : NULL };
}
static double lat_add(char kind, double start, double end, const char *text, double t_disp) {
    double end_t;
    if (!cm_time_of((long long) (end * OUT_RATE), &end_t)) return NAN;
    double lat = t_disp - end_t;
    lat_row(kind, start, end, lat * 1000, text);
    if (kind == 'p') {
        long long key = llround(start * 100);
        int seen = 0;
        for (int i = 0; i < LAT.nseen && !seen; i++) seen = LAT.seen[i] == key;
        if (!seen) {
            if (LAT.nseen == LAT.capseen) {
                int c = LAT.capseen ? LAT.capseen * 2 : 64;
                long long *q = realloc(LAT.seen, sizeof(long long) * (size_t) c);
                if (q) { LAT.seen = q; LAT.capseen = c; }
            }
            if (LAT.nseen < LAT.capseen) LAT.seen[LAT.nseen++] = key;
            double st;
            if (cm_time_of((long long) (start * OUT_RATE), &st)) lat_row('t', start, end, (t_disp - st) * 1000, text);
        }
    }
    return lat;
}
static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}
static void lat_summary(Buf *b) {
    static const char kinds[3] = { 'p', 'f', 't' };
    static const char *names[3] = { "partial", "final", "first_text" };
    double *v = malloc(sizeof(double) * (size_t) (LAT.n ? LAT.n : 1));
    int first = 1;
    buf_puts(b, "{");
    for (int k = 0; k < 3; k++) {
        int n = 0;
        for (int i = 0; i < LAT.n; i++)
            if (LAT.r[i].kind == kinds[k]) v[n++] = LAT.r[i].ms;
        if (!n) continue;
        qsort(v, (size_t) n, sizeof(double), cmp_double);
        double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
        int i90 = (int) (n * 0.9);
        if (i90 > n - 1) i90 = n - 1;
        buf_printf(b, "%s\"%s\": {\"n\": %d, \"p50_ms\": ", first ? "" : ", ", names[k], n);
        buf_num(b, med, 1);
        buf_puts(b, ", \"p90_ms\": ");
        buf_num(b, v[i90], 1);
        buf_puts(b, ", \"max_ms\": ");
        buf_num(b, v[n - 1], 1);
        buf_puts(b, "}");
        first = 0;
    }
    buf_puts(b, "}");
    free(v);
}
static void csv_field(Buf *b, const char *s) {
    if (!strpbrk(s, ",\"\r\n")) { buf_puts(b, s); return; }
    buf_puts(b, "\"");
    for (; *s; s++) {
        if (*s == '"') buf_puts(b, "\"\"");
        else buf_add(b, s, 1);
    }
    buf_puts(b, "\"");
}
static void lat_save(void) {
    if (!O.latency_csv) return;
    static const char *names[128] = { ['p'] = "partial", ['f'] = "final", ['t'] = "first_text" };
    Buf b = { 0 };
    buf_puts(&b, "kind,start_s,end_s,latency_ms,text\r\n");
    for (int i = 0; i < LAT.n; i++) {
        LRow *r = &LAT.r[i];
        buf_printf(&b, "%s,", names[(int) r->kind]);
        buf_num(&b, r->start, 3);
        buf_puts(&b, ",");
        buf_num(&b, r->end, 3);
        buf_puts(&b, ",");
        buf_num(&b, r->ms, 1);
        buf_puts(&b, ",");
        csv_field(&b, r->text ? r->text : "");
        buf_puts(&b, "\r\n");
    }
    mkdirs_parent_u8(O.latency_csv);
    FILE *f = fopen_u8(O.latency_csv, L"wb");
    if (f) {
        fwrite(b.p, 1, b.n, f);
        fclose(f);
    } else {
        warn(TT(T_FILE_FAIL), O.latency_csv);
    }
    buf_free(&b);
}

// ---- OBS の「テキスト（GDI+）」の「ファイルから読み取り」用。UTF-8（BOM なし）・改行は LF。変わったときだけ、0.1 秒に 1 回まで ----
static struct { char *last, *pending; double t; } OBS;
static void obs_write(const char *text, int force) {
    double now = now_s();
    if (OBS.last && !strcmp(text, OBS.last)) {
        free(OBS.pending);
        OBS.pending = NULL;
        return;
    }
    if (!force && now - OBS.t < 0.1) {  // 間に合わない分は、あとで書く（最後の中身を取りこぼさない）
        free(OBS.pending);
        OBS.pending = xstrdup(text);
        return;
    }
    write_replace_u8(O.obs, text, strlen(text));
    free(OBS.last);
    OBS.last = xstrdup(text);
    OBS.t = now;
    free(OBS.pending);
    OBS.pending = NULL;
}
static void obs_tick(void) {
    if (OBS.pending && now_s() - OBS.t >= 0.1) {
        char *p = OBS.pending;
        OBS.pending = NULL;
        obs_write(p, 1);
        free(p);
    }
}

// ---- 本体 ----
static Source SRC;
static struct {
    double t_start, ending, quit_at;
    volatile LONG end_requested, quit;
    char *last_stats, *window_check;
    double *rtts;
    int nrtt, caprtt;
    int window;
    double next_top, selfcheck_at;
    HANDLE finished;
} A;

static void app_quit_soon(void) {  // 訳が残っていれば少し待つ
    if (TR.th && TR.pending > 0) {
        if (A.quit_at == 0) A.quit_at = now_s() + 3.0;
    } else {
        A.quit = 1;
    }
}
static void app_request_quit(void) {
    if (A.end_requested) {
        A.quit = 1;
        return;
    }
    InterlockedExchange(&A.end_requested, 1);
    if (A.window && OV.hwnd && !OV.hidden) ov_toggle_hide();
    SetEvent(g_q_event);
}
static void app_hotkey(int id) {
    if (id == 1) ov_toggle_unlock();
    else if (id == 2) ov_toggle_hide();
    else if (id == 3) app_request_quit();
}

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        A.quit = 1;
        SetEvent(g_q_event);
        WaitForSingleObject(A.finished, 4500);
        return TRUE;
    }
    if (A.end_requested) A.quit = 1;
    else InterlockedExchange(&A.end_requested, 1);
    SetEvent(g_q_event);
    return TRUE;
}

// 前後の空白を取る（ASCII と全角の空白）
static void trim(char *s) {
    char *p = s;
    for (;;) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        else if ((unsigned char) p[0] == 0xE3 && (unsigned char) p[1] == 0x80 && (unsigned char) p[2] == 0x80) p += 3;
        else break;
    }
    memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    for (;;) {
        if (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
        else if (n >= 3 && (unsigned char) s[n - 3] == 0xE3 && (unsigned char) s[n - 2] == 0x80 && (unsigned char) s[n - 1] == 0x80) s[n -= 3] = 0;
        else break;
    }
}

static void cap_push(double start, double end, char *text, int fid) {
    if (CAP.n == NFIN) {  // いちばん古い文を落とす。訳を待っていたら、訳なしで議事録に書く
        Fin *o = cap_at(0);
        if (o->pending) min_add(o->wall, o->start, o->end, o->text, NULL);
        free(o->text);
        free(o->tr);
        CAP.head = (CAP.head + 1) % NFIN;
        CAP.n--;
    }
    Fin *f = cap_at(CAP.n++);
    memset(f, 0, sizeof *f);
    f->start = start;
    f->end = end;
    f->text = text;
    f->fid = fid;
    CAP.total++;
}

typedef struct { char kind; double start, end; char *text; int fid; } Applied;

static void app_render(int any_final, Applied *ap, int na) {
    // 確定した文の行（最後の lines 行）。途中の文があるときは 1 行減らす。最後の文の訳があれば下に
    const char *fin[NFIN];
    int nf = 0;
    int from = CAP.n - O.lines < 0 ? 0 : CAP.n - O.lines;
    for (int i = from; i < CAP.n; i++) fin[nf++] = cap_at(i)->text;
    const char *vol = CAP.vol ? CAP.vol : "";
    int fstart = 0;
    if (*vol) {
        int keep = O.lines > 1 ? O.lines - 1 : 0;
        if (nf > keep) fstart = nf - keep;
    }
    const char *tr = NULL;
    if (CAP.n && nf - fstart > 0 && cap_at(CAP.n - 1)->tr && !strcmp(fin[nf - 1], cap_at(CAP.n - 1)->text)) tr = cap_at(CAP.n - 1)->tr;
    const char *shown[NFIN + 2];
    int ns = 0;
    for (int i = fstart; i < nf; i++) shown[ns++] = fin[i];
    if (tr) shown[ns++] = tr;
    Buf b = { 0 };
    for (int i = 0; i < ns; i++) {
        if (i) buf_puts(&b, "\n");
        buf_puts(&b, shown[i]);
    }
    if (A.window) ov_show(buf_str(&b), vol);
    if (O.obs) {
        const char *ol[NFIN + 3];
        int no = 0;
        for (int i = 0; i < ns; i++) ol[no++] = shown[i];
        if (*vol) ol[no++] = vol;
        int k = O.obs_lines + (tr ? 1 : 0), s0 = no - k < 0 ? 0 : no - k;
        Buf o = { 0 };
        for (int i = s0; i < no; i++) {
            if (i > s0) buf_puts(&o, "\n");
            buf_puts(&o, ol[i]);
        }
        obs_write(buf_str(&o), any_final);
        buf_free(&o);
    }
    buf_free(&b);
    double t_disp = now_s();
    for (int i = 0; i < na; i++) {
        double lat = lat_add(ap[i].kind, ap[i].start, ap[i].end, ap[i].text, t_disp);
        if (ap[i].kind != 'f') continue;
        if (!O.quiet) say(TT(T_FINAL_LINE), ap[i].start, ap[i].end, ap[i].text, isnan(lat) ? 0.0 : lat * 1000);
        Fin *f = cap_find(ap[i].fid);
        if (MIN.txt && f) {
            double st;
            f->wall = wall_of(cm_time_of((long long) (ap[i].start * OUT_RATE), &st) ? st : t_disp);
            if (TR.th) f->pending = 1;
            else min_add(f->wall, f->start, f->end, f->text, NULL);
        }
        if (TR.th && f) tr_add(f->fid, f->text);
    }
}

static void app_drain(void) {
    Ev *e = q_take_all();
    if (!e) return;
    Applied *ap = NULL;
    int na = 0, cap = 0, any_final = 0, changed = 0;
    while (e) {
        Ev *nx = e->next;
        if (e->kind == EV_TRANSLATION) {
            InterlockedDecrement(&TR.pending);
            Fin *f = cap_find(e->fid);
            if (f) {
                free(f->tr);
                f->tr = e->s ? xstrdup(e->s) : NULL; // 訳せなかったときは NULL（fix1012。訳の行を出さない。理由は tr_thread がコンソールに書いた）
                if (f->pending) {
                    min_add(f->wall, f->start, f->end, f->text, f->tr);
                    f->pending = 0;
                }
                changed = 1;
            }
        } else if (e->kind == EV_CLOSED) {
            if (A.ending) app_quit_soon();
            else {
                warn("%s", TT(T_LINK_LOST));
                A.quit = 1;
            }
        } else if (e->kind == EV_JSON && e->s) {
            char *t = js_get_str(e->s, "t");
            if (t && (!strcmp(t, "partial") || !strcmp(t, "final"))) {
                char *text = js_get_str(e->s, "text");
                double st = 0, en = 0;
                int has_st = js_get_num(e->s, "start", &st);
                js_get_num(e->s, "end", &en);
                if (!text) text = xstrdup("");
                if (t[0] == 'p') {
                    free(CAP.vol);
                    CAP.vol = xstrdup(text);
                    CAP.has_vs = has_st;
                    CAP.vol_start = st;
                    if (na == cap) { cap = cap ? cap * 2 : 16; ap = realloc(ap, sizeof(Applied) * (size_t) cap); }
                    ap[na++] = (Applied) { 'p', st, en, text, -1 };
                } else {
                    // 確定した文と範囲が重なる途中の文だけ消す（確定より後ろの音の途中の文は残す）
                    if (!CAP.has_vs || CAP.vol_start < en + 0.01) {
                        free(CAP.vol);
                        CAP.vol = NULL;
                        CAP.has_vs = 0;
                    }
                    trim(text);
                    if (*text) {
                        int fid = CAP.next_id++;
                        cap_push(st, en, xstrdup(text), fid);
                        if (na == cap) { cap = cap ? cap * 2 : 16; ap = realloc(ap, sizeof(Applied) * (size_t) cap); }
                        ap[na++] = (Applied) { 'f', st, en, text, fid };
                        any_final = 1;
                    } else {
                        free(text);
                    }
                }
                changed = 1;
            } else if (t && !strcmp(t, "pong")) {
                double pc;
                if (js_get_num(e->s, "pc", &pc) && pc > 0) {
                    if (A.nrtt == A.caprtt) {
                        A.caprtt = A.caprtt ? A.caprtt * 2 : 64;
                        A.rtts = realloc(A.rtts, sizeof(double) * (size_t) A.caprtt);
                    }
                    A.rtts[A.nrtt++] = (e->t - pc) * 1000;
                }
            } else if (t && !strcmp(t, "stats")) {
                free(A.last_stats);
                A.last_stats = e->s;
                e->s = NULL;
            } else if (t && !strcmp(t, "error")) {
                char *er = js_get_str(e->s, "error");
                warn(TT(T_IPHONE_ERROR), er ? er : "?");
                free(er);
            } else if (t && !strcmp(t, "done")) {
                if (A.ending) app_quit_soon();
            }
            free(t);
        }
        free(e->s);
        free(e);
        e = nx;
    }
    if (changed) app_render(any_final, ap, na);
    for (int i = 0; i < na; i++) free(ap[i].text);
    free(ap);
}

static void app_check_end(void) {
    double now = now_s();
    int wav_done = SRC.kind == SRC_WAV && SRC.done;
    if ((O.seconds > 0 && now - A.t_start >= O.seconds) || wav_done || A.end_requested) {
        if (!A.ending) {
            A.ending = now;
            if (A.end_requested && !O.quiet) say("%s", TT(T_STOPPING));
            src_stop(&SRC);
            snd_finish();
            live_end();  // 残りを確定させて done を待つ
        } else if (now - A.ending > O.end_wait) {
            A.quit = 1;
        }
    }
    if (A.quit_at > 0 && (now >= A.quit_at || TR.pending <= 0)) A.quit = 1;
}

static int cmp_d(const void *a, const void *b) { return cmp_double(a, b); }

static void app_finish(void) {
    src_stop(&SRC);
    if (!A.ending) {
        snd_finish();
        live_end();
    }
    live_close();
    tr_close();
    if (MIN.txt) {
        for (int i = 0; i < CAP.n; i++)
            if (cap_at(i)->pending) {
                Fin *f = cap_at(i);
                min_add(f->wall, f->start, f->end, f->text, f->tr);
                f->pending = 0;
            }
        fclose(MIN.txt);
        fclose(MIN.srt);
        say(TT(T_MINUTES), MIN.stem, MIN.stem);
    }
    if (OBS.pending) obs_write(OBS.pending, 1);
    lat_save();
    Buf b = { 0 };
    buf_puts(&b, "{\"latency\": ");
    lat_summary(&b);
    buf_puts(&b, ", \"link_rtt_ms_p50\": ");
    if (A.nrtt) {
        qsort(A.rtts, (size_t) A.nrtt, sizeof(double), cmp_d);
        double med = A.nrtt % 2 ? A.rtts[A.nrtt / 2] : (A.rtts[A.nrtt / 2 - 1] + A.rtts[A.nrtt / 2]) / 2;
        buf_num(&b, med, 1);
    } else {
        buf_puts(&b, "null");
    }
    buf_puts(&b, ", \"audio_sent_s\": ");
    buf_num(&b, (double) SND.sent / OUT_RATE, 2);
    buf_puts(&b, ", \"silence_filled_s\": ");
    buf_num(&b, SRC.filled_s, 2);
    buf_printf(&b, ", \"finals\": %lld, \"iphone_stats\": %s, \"capture_device_latency_ms\": ", CAP.total, A.last_stats ? A.last_stats : "{}");
    if (SRC.kind == SRC_WAV) buf_puts(&b, "null");
    else buf_num(&b, SRC.latency_ms, 1);
    buf_printf(&b, ", \"window_check\": %s", A.window_check ? A.window_check : "null");
    if (SND.dropped) buf_printf(&b, ", \"dropped_s\": %.2f", (double) SND.dropped / OUT_RATE);
    if (SND.error) buf_puts(&b, ", \"send_error\": true");
    buf_puts(&b, "}");
    say(TT(T_SUMMARY), b.p);
    buf_free(&b);
    if (A.window) ov_destroy();
}

static char *exe_dir_file(const char *name) {
    wchar_t p[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return NULL;
    wchar_t *s = wcsrchr(p, L'\\');
    if (s) s[1] = 0;
    char *d = w2u(p);
    Buf b = { 0 };
    buf_printf(&b, "%s%s", d, name);
    free(d);
    return b.p;
}
static char *appdata_file(const char *name) {
    const char *ad = getenv("APPDATA");
    if (!ad || !*ad) return NULL;
    Buf b = { 0 };
    buf_printf(&b, "%s\\PocketGPU\\%s", ad, name);
    return b.p;
}

int main(void) {
    InitializeCriticalSection(&g_out_cs);
    clock_init();
    q_init();
    A.finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    int argc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc((size_t) argc + 1, sizeof(char *));
    const char *ui = NULL;
    for (int i = 0; i < argc; i++) {
        argv[i] = w2u(wargv[i]);
        if (!strcmp(argv[i], "--ui") && i + 1 < argc) { char *u = w2u(wargv[i + 1]); ui = u; }
        else if (!strncmp(argv[i], "--ui=", 5)) ui = argv[i] + 5;
    }
    text_choose(ui);
    if (!parse_args(argc, argv)) return 2;
    if (O.help) {
        out_raw(STD_OUTPUT_HANDLE, TT(T_USAGE));
        return 0;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    int com_ok = wa_init();
    if (!com_ok && (O.list || strncmp(O.source, "wav:", 4))) {
        warn("%s", TT(T_COM_FAIL));
        return 1;
    }
    if (O.list) {
        wa_list();
        return 0;
    }
    if (!src_open(&SRC, O.source)) return 1;
    if (O.no_keepalive) SRC.keepalive = 0;
    say(TT(T_AUDIO), SRC.name, SRC.rate, SRC.ch, OUT_RATE);
    A.window = !O.no_window;
    if (A.window) dpi_init();  // ウィンドウを作る前に

    say("%s", TT(T_CONNECTING));
    double t0 = now_s();
    char *ready = NULL;
    Buf err = { 0 };
    if (!live_start(O.addr, O.mux, O.lang, OUT_RATE, !O.accurate, &ready, &err)) {
        warn("%s", buf_str(&err));
        return 1;
    }
    char *eng = js_get_str(ready, "engine"), *loc = js_get_str(ready, "locale"), *fmt = js_get_str(ready, "analyzer_format");
    say(TT(T_READY), eng ? eng : "?", loc ? loc : "?", fmt ? fmt : "?", (now_s() - t0) * 1000);
    free(eng);
    free(loc);
    free(fmt);
    free(ready);

    if (O.obs) mkdirs_parent_u8(O.obs);
    if (O.minutes) min_open(O.minutes);
    if (O.translate) tr_start(O.addr, O.mux, O.lang, O.translate);
    InitializeSRWLock(&CM.lk);
    snd_start();
    if (!src_start(&SRC, snd_push)) {
        live_close();
        return 1;
    }
    A.t_start = now_s();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    if (A.window) {
        if (O.settings) OV.settings = xstrdup(O.settings);
        else {
            OV.settings = exe_dir_file("livecap_settings.json");
            OV.settings_alt = appdata_file("livecap_settings.json");
        }
        if (!ov_create(O.geometry, O.font_size, O.font_given, O.lines, O.opacity, TT(T_TITLE))) A.window = 0;
        A.next_top = now_s() + 2.0;
        if (O.selfcheck) A.selfcheck_at = now_s() + 2.5;
    }
    while (!A.quit) {
        if (A.window) {
            MsgWaitForMultipleObjectsEx(1, &g_q_event, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            ov_pump();
        } else {
            WaitForSingleObject(g_q_event, 50);
        }
        app_drain();
        app_check_end();
        obs_tick();
        if (A.window) {
            double now = now_s();
            if (A.selfcheck_at > 0 && now >= A.selfcheck_at) {
                A.selfcheck_at = 0;
                Buf c = { 0 };
                ov_selfcheck(&c);
                A.window_check = c.p;
                say(TT(T_WINCHECK), c.p);
            }
            if (now >= A.next_top) {
                ov_keep_on_top();
                A.next_top = now + 2.0;
            }
        }
    }
    app_finish();
    SetEvent(A.finished);
    return 0;
}
