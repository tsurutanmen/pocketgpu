// iPhone 音声の約束（DESIGN.md の「約束」と同じ）。PC の ipaudio.c と、iPhone の代役 fake_iphone.c が使う。
// mic2 で足した物（どれも前は予約の 0 だった所か、前の PC が 1 だけ入れていた所。前の PC と前のアプリは読まないので、組み合わせても壊れない）：
//   HELLO の mic_channels：2 を頼むと、アプリの「ステレオで送る」がオンで入力が 2ch 取れるときだけ 2ch で来る。来る ch は ACK を見る
//   HELLO の mic_format：1 s16le・2 s24le（3 バイト）・3 float32le。来る形は ACK の mic_format を見る（前のアプリは 1）
//   HELLO の mic_rate：48000 より上は、iPhone の機器がその rate で動いたときだけ。来る rate は ACK を見る
//   HELLO の 28 バイト目 mic_mode：0 アプリの設定・1 加工なし・2 標準・3 雑音とエコーを消す。3 のときは flags の 0x4 も立てる（前のアプリ向け）
//   ACK の 32〜35 バイト目：使った音の種類・入力の種類・ステレオの印・入力の機器の ch 数。前のアプリは 0
// fix1012 で足した物（アプリ 0.7.60 から。前の PC は読まないので、組み合わせても壊れない）：
//   ACK の flags の 0x0008（IPA_A_MIC_LATER）：いまはマイクが無いが、アプリが前に出たら、このつなぎのまま始める。
//     始まったら、ACK に書いた形で MIC の塊が届き出す（前のアプリも、前に出てマイクを始められたときは同じ）
//   STATS の 33〜36 バイト目（IpaStatsExt）：アプリの状態（IPA_S_*）・いまの入力の種類・予約 2。前のアプリは 32 バイトで送る
// iPhone 側の AudioBridge.swift は同じ並びを手で読み書きする。数はすべてリトルエンディアン。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define IPA_PORT 50063          // iPhone の中の 127.0.0.1 で待つ番号
#define IPA_VERSION 1

// HELLO の flags
#define IPA_F_SPEAKER 0x0001    // PC の音を iPhone から出したい（PC → iPhone）
#define IPA_F_MIC 0x0002        // iPhone のマイクを PC に送ってほしい（iPhone → PC）
#define IPA_F_VOICEPROC 0x0004  // iPhone の音声処理（エコー消し）を使う
#define IPA_F_EXCLUSIVE 0x0008  // 他のアプリの音と混ぜない（短いバッファを通しやすい）
// ACK の flags
#define IPA_A_SPEAKER_OK 0x0001
#define IPA_A_MIC_OK 0x0002
#define IPA_A_MIC_NEEDS_FOREGROUND 0x0004  // マイクは前に出ているときしか始められない。アプリを前に出すと始まる
#define IPA_A_MIC_LATER 0x0008             // fix1012：このつなぎのまま、あとでマイクを始める。つなぎ直さずに待てる
// STATS の 33 バイト目（IpaStatsExt.state）
#define IPA_S_FRONT 0x01        // アプリが前面にある
#define IPA_S_MIC_RUNNING 0x02  // マイクが動いている
#define IPA_S_MIC_WAITING 0x04  // マイクを始めるのを待っている
#define IPA_S_MIC_DENIED 0x08   // マイクが許可されていない
// マイクの PCM の形（HELLO と ACK の mic_format）
#define IPA_FMT_S16 1
#define IPA_FMT_S24 2
#define IPA_FMT_F32 3
// 音の種類（HELLO の mic_mode・ACK の mic_mode）
#define IPA_MODE_APP 0
#define IPA_MODE_RAW 1
#define IPA_MODE_STANDARD 2
#define IPA_MODE_VOICE 3
// ACK の flags2
#define IPA_A2_STEREO_CAPABLE 0x01  // 本体のマイクがステレオにできる
#define IPA_A2_STEREO_ON 0x02       // いまステレオで取っている
// ACK の input_kind
enum { IPA_IN_UNKNOWN = 0, IPA_IN_BUILTIN = 1, IPA_IN_HEADSET = 2, IPA_IN_USB = 3, IPA_IN_BLUETOOTH = 4, IPA_IN_OTHER = 5 };

#pragma pack(push, 1)
typedef struct {            // PC → iPhone。40 バイト
    char magic[4];          // "IPAU"
    uint16_t version;       // 1
    uint16_t flags;         // IPA_F_*
    uint32_t spk_rate;      // スピーカー側の 1 秒あたりのコマ数（例 48000）
    uint16_t spk_channels;  // 2
    uint16_t spk_format;    // 1 = s16le
    uint32_t mic_rate;      // 欲しいマイクの rate（例 48000）
    uint16_t mic_channels;  // 1
    uint16_t mic_format;    // 1 = s16le
    uint16_t chunk_frames;  // 送り手が揃える 1 塊のコマ数（例 480 = 10 ms）
    uint16_t target_ms;     // iPhone がためておく長さ（スピーカー側）
    uint8_t mic_mode;       // IPA_MODE_*。前は予約
    uint8_t reserved[11];
} IpaHello;

typedef struct {            // iPhone → PC。40 バイト
    char magic[4];          // "IPAK"
    uint16_t version;
    uint16_t flags;         // IPA_A_*
    uint32_t mic_rate;      // 実際に送るマイクの rate
    uint16_t mic_channels;
    uint16_t mic_format;
    uint32_t hw_rate;       // iPhone の音の機器の rate
    uint32_t io_buffer_us;  // AVAudioSession.ioBufferDuration（マイクロ秒）
    uint32_t out_latency_us;
    uint32_t in_latency_us;
    uint8_t mic_mode;       // 使った音の種類。前のアプリは 0
    uint8_t input_kind;     // IPA_IN_*
    uint8_t flags2;         // IPA_A2_*
    uint8_t hw_in_channels; // iPhone の入力の機器の ch 数
    uint8_t reserved[4];
} IpaAck;

enum { IPA_T_SPK = 1, IPA_T_MIC = 2, IPA_T_PING = 3, IPA_T_PONG = 4, IPA_T_STATS = 5, IPA_T_BYE = 6 };
#define IPA_FR_SILENT 0x01  // 中身は無音。length は 0 で、frames のぶん進める

typedef struct {            // 16 バイト。このあとに length バイトの PCM
    uint8_t type;
    uint8_t flags;
    uint16_t frames;        // PCM のコマ数（PING・PONG・STATS は 0）
    uint32_t length;
    uint64_t index;         // この塊の先頭のコマの番号（その向きの数え始めから）。PING・PONG は送り手の時計（マイクロ秒）
} IpaFrame;

typedef struct {            // IPA_T_STATS の中身。iPhone が 1 秒ごとに送る。32 バイト
    uint32_t fill_frames;
    uint32_t target_frames;
    uint32_t underruns;
    uint32_t overruns;
    uint32_t gap_frames;
    int32_t ratio_ppm;      // 読む速さの補正（100 万分率）
    uint32_t mic_sent_frames;
    uint32_t mic_dropped_frames;
} IpaStats;

typedef struct {            // fix1012：アプリ 0.7.60 からの STATS。36 バイト。初めの 32 バイトは IpaStats と同じ
    IpaStats base;
    uint8_t state;          // IPA_S_*
    uint8_t input_kind;     // マイクが動いているときの入力の種類（IPA_IN_*）。止まっていれば 0
    uint8_t reserved[2];
} IpaStatsExt;
#pragma pack(pop)

_Static_assert(sizeof(IpaHello) == 40, "hello");
_Static_assert(sizeof(IpaAck) == 40, "ack");
_Static_assert(sizeof(IpaFrame) == 16, "frame");
_Static_assert(sizeof(IpaStats) == 32, "stats");
_Static_assert(sizeof(IpaStatsExt) == 36, "stats ext");

// ---- 時計 ----
static inline uint64_t ipa_now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t) ((double) c.QuadPart * 1e6 / (double) f.QuadPart);
}

// ---- 送る・受ける ----
static inline int ipa_send_all(SOCKET s, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        int r = send(s, b, n > (1u << 20) ? (1 << 20) : (int) n, 0);
        if (r <= 0) return 0;
        b += r;
        n -= (size_t) r;
    }
    return 1;
}

static inline int ipa_recv_all(SOCKET s, void *p, size_t n) {
    char *b = p;
    while (n) {
        int r = recv(s, b, n > (1u << 20) ? (1 << 20) : (int) n, 0);
        if (r <= 0) return 0;
        b += r;
        n -= (size_t) r;
    }
    return 1;
}

static inline void ipa_nodelay(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof one);
}

// 区切りの 1 塊を送る（頭と中身を 1 回の send にまとめる）
static inline int ipa_send_frame(SOCKET s, uint8_t type, uint8_t flags, uint16_t frames, uint64_t index, const void *pcm, uint32_t len) {
    uint8_t tmp[16 + 4096];
    uint8_t *buf = len <= 4096 ? tmp : (uint8_t *) malloc(16 + len);
    if (!buf) return 0;
    IpaFrame h = { type, flags, frames, len, index };
    memcpy(buf, &h, 16);
    if (len) memcpy(buf + 16, pcm, len);
    int ok = ipa_send_all(s, buf, 16 + len);
    if (buf != tmp) free(buf);
    return ok;
}

// ---- 形 ----
static inline int ipa_fmt_bytes(int fmt) { return fmt == IPA_FMT_F32 ? 4 : fmt == IPA_FMT_S24 ? 3 : 2; }
static inline const char *ipa_fmt_name(int fmt) { return fmt == IPA_FMT_F32 ? "float32" : fmt == IPA_FMT_S24 ? "24bit" : "16bit"; }

// 届いた形の PCM を float（交互のまま）にする。n はサンプルの数（コマ数 × ch）
static inline void ipa_pcm_to_f32(int fmt, const uint8_t *p, float *f, size_t n) {
    if (fmt == IPA_FMT_F32) { memcpy(f, p, n * 4); return; }
    if (fmt == IPA_FMT_S24) {
        for (size_t i = 0; i < n; i++) {
            int32_t v = (int32_t) ((uint32_t) p[3 * i] << 8 | (uint32_t) p[3 * i + 1] << 16 | (uint32_t) p[3 * i + 2] << 24) >> 8;
            f[i] = (float) v * (1.0f / 8388608.0f);
        }
        return;
    }
    for (size_t i = 0; i < n; i++) { int16_t v; memcpy(&v, p + 2 * i, 2); f[i] = (float) v * (1.0f / 32768.0f); }
}

// ---- WAV（16 ビット・24 ビット・float32）----
typedef struct { FILE *f; uint32_t rate; uint16_t ch; uint64_t bytes; int fmt; int head; } IpaWav;

static inline int ipa_wav_open_fmt(IpaWav *w, const char *path, uint32_t rate, uint16_t ch, int fmt) {
    w->f = fopen(path, "wb");
    w->rate = rate;
    w->ch = ch;
    w->bytes = 0;
    w->fmt = fmt;
    w->head = fmt == IPA_FMT_F32 ? 58 : 44; // float は fmt を 18 バイトにして fact を足す
    if (!w->f) return 0;
    uint8_t h[64] = {0};
    fwrite(h, 1, (size_t) w->head, w->f);
    return 1;
}

static inline int ipa_wav_open(IpaWav *w, const char *path, uint32_t rate, uint16_t ch) { return ipa_wav_open_fmt(w, path, rate, ch, IPA_FMT_S16); }

static inline void ipa_wav_write(IpaWav *w, const void *pcm, size_t bytes) {
    if (w->f && bytes) { fwrite(pcm, 1, bytes, w->f); w->bytes += bytes; }
}

static inline void ipa_wav_close(IpaWav *w) {
    if (!w->f) return;
    uint32_t v;
    uint16_t u;
    int bps = ipa_fmt_bytes(w->fmt), fl = w->fmt == IPA_FMT_F32;
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f); v = (uint32_t) ((uint64_t) w->head - 8 + w->bytes); fwrite(&v, 4, 1, w->f);
    fwrite("WAVEfmt ", 1, 8, w->f); v = fl ? 18 : 16; fwrite(&v, 4, 1, w->f);
    u = fl ? 3 : 1; fwrite(&u, 2, 1, w->f); // 3 = WAVE_FORMAT_IEEE_FLOAT
    u = w->ch; fwrite(&u, 2, 1, w->f);
    v = w->rate; fwrite(&v, 4, 1, w->f);
    v = w->rate * w->ch * (uint32_t) bps; fwrite(&v, 4, 1, w->f);
    u = (uint16_t) (w->ch * bps); fwrite(&u, 2, 1, w->f);
    u = (uint16_t) (bps * 8); fwrite(&u, 2, 1, w->f);
    if (fl) {
        u = 0; fwrite(&u, 2, 1, w->f); // cbSize
        fwrite("fact", 1, 4, w->f); v = 4; fwrite(&v, 4, 1, w->f);
        v = (uint32_t) (w->bytes / ((uint64_t) w->ch * (uint64_t) bps)); fwrite(&v, 4, 1, w->f);
    }
    fwrite("data", 1, 4, w->f); v = (uint32_t) w->bytes; fwrite(&v, 4, 1, w->f);
    fclose(w->f);
    w->f = NULL;
}

// ---- 読み出し側の「ため」と、時計のずれの補正 ----
// 受け側（iPhone のスピーカー・PC のマイク出力）が共通で使う。AudioBridge.swift の PlayoutRing と同じ中身。
//  - 届いた塊を float で輪に積む。読み出しは受け側の機器の時計で呼ばれる
//  - たまった量（fill）をなだらかにしたものが目標からずれたら、読む速さを最大 ±0.5% 変えて戻す（時計のずれの吸収）
//  - 足りなくなったら無音で埋め、目標までたまり直すのを待つ。多すぎたら目標まで古い方を捨てる
typedef struct {
    int ch;
    int cap;                // コマ数（2 のべき乗でなくてよい）
    float *buf;
    uint64_t wr;            // 積んだコマの総数
    double rd;              // 読む位置（コマ・小数）
    double nominal;         // 元の rate ÷ 出す rate
    double target;          // ためる目標（コマ）
    double fs;              // fill のなだらかな値
    double integ;           // 補正の積分の項（時計のずれの推定）
    double src_rate, dst_rate;
    double ratio;           // いま使っている読む速さ（nominal に補正をかけた値）
    int running;            // 目標までたまって鳴らしている間 1
    int adapt;              // 0 なら補正しない（比べる用）
    int skip_quiet;         // 1 なら、たまりすぎを静かな所だけ飛ばして早く戻す（mic2。PC のマイクの側だけ）
    uint64_t skips, skipped_frames;
    uint64_t underruns, overruns, gap_frames, in_frames, out_frames;
    double min_fill, max_fill;
    CRITICAL_SECTION cs;
} IpaPlayout;

static inline void po_init(IpaPlayout *p, int ch, double src_rate, double dst_rate, double target_ms) {
    memset(p, 0, sizeof *p);
    p->ch = ch;
    p->cap = (int) (src_rate * 2); // 2 秒
    p->buf = (float *) calloc((size_t) p->cap * ch, sizeof(float));
    p->nominal = src_rate / dst_rate;
    p->src_rate = src_rate;
    p->dst_rate = dst_rate;
    p->ratio = p->nominal;
    p->target = src_rate * target_ms / 1000.0;
    p->adapt = 1;
    p->min_fill = 1e18;
    InitializeCriticalSection(&p->cs);
}

// 中身を捨てて、ためなおしから始める。rate が変わったら輪も作り直す（読む糸と同じ錠の中で）
static inline void po_restart(IpaPlayout *p, double src_rate) {
    EnterCriticalSection(&p->cs);
    if (src_rate != p->src_rate) {
        free(p->buf);
        p->cap = (int) (src_rate * 2);
        p->buf = (float *) calloc((size_t) p->cap * p->ch, sizeof(float));
        p->target = p->target / p->src_rate * src_rate;
        p->src_rate = src_rate;
        p->nominal = src_rate / p->dst_rate;
    }
    p->wr = 0;
    p->rd = 0;
    p->running = 0;
    p->fs = 0;
    p->integ = 0;
    p->ratio = p->nominal;
    LeaveCriticalSection(&p->cs);
}

// 積む。interleaved の float。frames コマ
static inline void po_write(IpaPlayout *p, const float *x, int frames) {
    EnterCriticalSection(&p->cs);
    for (int i = 0; i < frames; i++) {
        size_t at = (size_t) (p->wr % (uint64_t) p->cap) * p->ch;
        for (int c = 0; c < p->ch; c++) p->buf[at + c] = x[(size_t) i * p->ch + c];
        p->wr++;
    }
    p->in_frames += (uint64_t) frames;
    double fill = (double) p->wr - p->rd;
    if (fill > p->target * 2 + 0.05 * p->src_rate) { // 多すぎる（止まりのあとの追いつき）。目標まで古い方を捨てる
        p->rd = (double) p->wr - p->target;
        p->overruns++;
    }
    LeaveCriticalSection(&p->cs);
}

static inline void po_write_zeros(IpaPlayout *p, int frames) {
    float z[960 * 2] = {0};
    while (frames > 0) {
        int n = frames > 960 ? 960 : frames;
        po_write(p, z, n);
        frames -= n;
    }
}

static inline float po_at(const IpaPlayout *p, int64_t i, int c) {
    int64_t m = i % p->cap;
    if (m < 0) m += p->cap;
    return p->buf[(size_t) m * p->ch + c];
}

// 受け側の機器の時計で frames コマを読む。足りない所は無音。鳴らせた（無音でない）コマ数を返す
static inline int po_read2(IpaPlayout *p, float *out, int frames, double *rd0, double *ratio_out);
static inline int po_read(IpaPlayout *p, float *out, int frames) { return po_read2(p, out, frames, NULL, NULL); }

// rd0 と ratio_out が NULL でなければ、読み始めの位置（コマ）と、1 コマ出すごとに進む量を返す（鳴らしていなければ rd0 は -1）
static inline int po_read2(IpaPlayout *p, float *out, int frames, double *rd0, double *ratio_out) {
    EnterCriticalSection(&p->cs);
    if (rd0) *rd0 = -1;
    double fill = (double) p->wr - p->rd;
    if (fill < p->min_fill && p->running) p->min_fill = fill;
    if (fill > p->max_fill) p->max_fill = fill;
    if (!p->running) {
        if (fill >= p->target) { p->running = 1; p->fs = fill; }
        else {
            memset(out, 0, sizeof(float) * (size_t) frames * p->ch);
            LeaveCriticalSection(&p->cs);
            return 0;
        }
    }
    p->fs += 0.02 * (fill - p->fs);
    // たまりすぎ（止まりのあとにまとめて届いた）が 20 ms を超えて続き、飛ばす所が静か（-45 dBFS より小さい）なら、そこを飛ばす。
    // 速さの補正だけだと 1 秒に 5 ms しか戻らず、録画の声の遅れが十数秒ずれたままになるため。音のある所は飛ばさない
    if (p->skip_quiet && p->adapt) {
        double excess = p->fs - p->target;
        if (excess > 0.02 * p->src_rate && fill - excess > p->target * 0.5) {
            int64_t a = (int64_t) floor(p->rd), n = (int64_t) excess;
            float pk = 0;
            for (int64_t i = 0; i < n; i++)
                for (int c = 0; c < p->ch; c++) { float v = fabsf(po_at(p, a + i, c)); if (v > pk) pk = v; }
            if (pk < 0.0056f) {
                p->rd += (double) n;
                p->fs -= (double) n;
                fill -= (double) n;
                p->skips++;
                p->skipped_frames += (uint64_t) n;
            }
        }
    }
    if (p->adapt) {
        // 比例（4 秒で戻す速さ）＋積分（20 秒の時定数でずれを覚える）。どちらも ±0.5% で止める
        double err = p->fs - p->target;                    // コマ
        double dt = (double) frames / p->dst_rate;         // 秒
        double adj_p = err / (4.0 * p->src_rate);
        p->integ += adj_p * dt / 20.0;
        if (p->integ > 0.005) p->integ = 0.005;
        if (p->integ < -0.005) p->integ = -0.005;
        double adj = adj_p + p->integ;
        if (adj > 0.005) adj = 0.005;
        if (adj < -0.005) adj = -0.005;
        p->ratio = p->nominal * (1.0 + adj);
    } else {
        p->ratio = p->nominal;
    }
    int got = 0;
    if (rd0) *rd0 = p->rd;
    if (ratio_out) *ratio_out = p->ratio;
    for (int i = 0; i < frames; i++) {
        int64_t i0 = (int64_t) floor(p->rd);
        if ((double) (i0 + 1) >= (double) p->wr) { // 足りない。残りは無音にして、たまり直しを待つ
            memset(out + (size_t) i * p->ch, 0, sizeof(float) * (size_t) (frames - i) * p->ch);
            p->running = 0;
            p->underruns++;
            break;
        }
        double fr = p->rd - (double) i0;
        for (int c = 0; c < p->ch; c++) {
            float a = po_at(p, i0, c), b = po_at(p, i0 + 1, c);
            out[(size_t) i * p->ch + c] = (float) (a + (b - a) * fr);
        }
        p->rd += p->ratio;
        got++;
    }
    p->out_frames += (uint64_t) frames;
    LeaveCriticalSection(&p->cs);
    return got;
}

static inline double po_fill(IpaPlayout *p) {
    EnterCriticalSection(&p->cs);
    double f = (double) p->wr - p->rd;
    LeaveCriticalSection(&p->cs);
    return f;
}

// s16le → float（interleaved）
static inline void ipa_s16_to_f32(const int16_t *s, float *f, size_t n) {
    for (size_t i = 0; i < n; i++) f[i] = (float) s[i] * (1.0f / 32768.0f);
}
