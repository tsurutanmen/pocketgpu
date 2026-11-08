// fake_iphone.exe：本物の iPhone の代わりに、PC の中で「音の受け手」の役をする試験用の相手。
// AudioBridge.swift と同じ約束・同じ「ため」と補正（proto.h の IpaPlayout）で動く。
//   スピーカー側：届いた PCM を輪にため、iPhone の機器の時計（--ppm でずらせる）で読み出して WAV に書く
//   マイク側    ：既定は正弦波（L は --mic-freq、R はその 1.5 倍）を 10 ms ごとに送る。
//                --mic-loopback 出力の機器名 なら、その機器の loopback をマイクの代わりにする（遅れの測りの試験）
// mic2 で足した：
//   新しいアプリのまね（既定）：HELLO の mic_channels・mic_format・mic_mode を読み、ACK の 32〜35 バイト目を書く
//   --old：前の版（0.7.56）のアプリのまね。新しい所を読まず、1ch・16 ビットで送る
//   --app-stereo 0|1（既定 1）：アプリの「ステレオで送る」・--app-mode 1|2|3（既定 2）：アプリの音の種類
//   --mono-input：入力の機器が 1ch しか取れない（ステレオを頼まれてもモノラル）
//   --mic-delay-ms N：マイクの塊を N ms 遅らせて送る（遅れの測りが、足した遅れを測れるかを見る）
//   --accept-loop：つなぎが切れても次のつなぎを待つ・--drop-after S：つないで S 秒で切る（つなぎ直しの試験）
// fix1012 で足した（アプリが後ろにいる間につながったときの試験）：
//   --bg-until S：代役が起きてから S 秒はアプリが後ろにいる（-1 でずっと後ろ。既定 0 は初めから前面）。
//     後ろにいる間につながると、ACK は mic=no と IPA_A_MIC_NEEDS_FOREGROUND
//   --late new|bug|fail（既定 new）：前に出たときのふるまい
//     new ：アプリ 0.7.60 のまね。ACK に IPA_A_MIC_LATER を立て、前に出たら、このつなぎのままマイクを始める。STATS は 36 バイトで状態つき
//     bug ：アプリ 0.7.59 のまね（10/12 の不具合）。前に出ても、このつなぎではマイクを始めない。STATS は 32 バイト。つなぎ直すと mic=ok
//     fail：0.7.60 で、前に出たのに始めるのに失敗し続けるまね。STATS は前面・待っている。つなぎ直すと mic=ok
//   zig cc -O2 -std=c11 fake_iphone.c -o fake_iphone.exe -lws2_32 -lwinmm -lole32 -luuid -lavrt -loleaut32
#include "wasapi.h"

typedef struct {
    int port, seconds, io_frames, no_mic, no_adapt;
    double ppm, mic_freq, mic_amp;
    int mic_drop_every, mic_jitter_ms, spk_stall_ms_at_s, spk_stall_ms;
    const char *spk_wav, *play_wav, *mic_loopback;
    int old, app_stereo, app_mode, mono_input, mic_delay_ms, accept_loop, drop_after, freeze_after, mic_pulse, stall_at_s, stall_ms;
    int bg_until, late;
} Opt;
enum { LATE_NEW = 0, LATE_BUG = 1, LATE_FAIL = 2 };

static SOCKET g_c = INVALID_SOCKET;
static CRITICAL_SECTION g_cs;
static volatile LONG g_stop;
static Opt O;
static IpaHello g_h;
static IpaPlayout g_po;
static int g_po_on;
static IpaWav g_spkwav, g_playwav;
static uint64_t g_spk_next, g_spk_frames, g_spk_chunks, g_spk_gap, g_spk_late, g_spk_silent_chunks;
static uint64_t g_played_frames, g_silence_after_start;
static double g_fill_min_ms = 1e9, g_fill_max_ms = 0;
static uint64_t g_mic_sent, g_mic_dropped;
static double g_arrival_int_max_ms, g_arrival_int_sum_ms;
static uint64_t g_arrival_n, g_last_arrival_us;
static int g_mic_rate = 48000, g_mic_ch = 1, g_mic_fmt = IPA_FMT_S16;
static uint64_t g_t0_us;               // 代役が起きた時刻（--bg-until はここから数える）
static volatile LONG g_mic_running;    // このつなぎでマイクを送っている
static volatile LONG g_late_pending;   // 前に出たら、このつなぎのままマイクを始める（new）・始めたいが失敗し続ける（fail）
static HANDLE g_late_th;
static DWORD WINAPI mic_thread(LPVOID a);
static DWORD WINAPI mic_loop_thread(LPVOID a);
static int fake_front(void) {
    if (O.bg_until < 0) return 0;
    return ipa_now_us() - g_t0_us >= (uint64_t) O.bg_until * 1000000;
}

static volatile LONG g_frozen; // --freeze-after：つないだまま何も送らなくなる（止まった iPhone のまね）
static int send_locked(uint8_t t, uint8_t fl, uint16_t fr, uint64_t idx, const void *p, uint32_t len) {
    if (g_frozen) return 1;
    EnterCriticalSection(&g_cs);
    int ok = ipa_send_frame(g_c, t, fl, fr, idx, p, len);
    LeaveCriticalSection(&g_cs);
    return ok;
}

static void sleep_until(uint64_t t_us) {
    for (;;) {
        uint64_t n = ipa_now_us();
        if (n >= t_us) return;
        if (t_us - n > 2500) Sleep(1);
        else YieldProcessor();
    }
}

// iPhone の機器の読み出し：io_frames コマごとに、ppm だけずれた時計で呼ぶ
static DWORD WINAPI play_thread(LPVOID a) {
    (void) a;
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task); // 本物の iPhone の音の糸は実時間の優先度。それに近づける
    int n = O.io_frames;
    float *f = malloc(sizeof(float) * 2 * n);
    int16_t *s = malloc(sizeof(int16_t) * 2 * n);
    double period = (double) n / (48000.0 * (1.0 + O.ppm * 1e-6)) * 1e6;
    uint64_t t0 = ipa_now_us();
    double k = 1;
    while (!g_stop) {
        sleep_until(t0 + (uint64_t) (period * k));
        k += 1;
        int was_running = g_po.running;
        int got = po_read(&g_po, f, n);
        if (was_running && got < n) g_silence_after_start += (uint64_t) (n - got);
        g_played_frames += (uint64_t) n;
        double fm = po_fill(&g_po) * 1000.0 / 48000.0;
        if (g_po.running) {
            if (fm < g_fill_min_ms) g_fill_min_ms = fm;
            if (fm > g_fill_max_ms) g_fill_max_ms = fm;
        }
        for (int i = 0; i < n * 2; i++) {
            float v = f[i] * 32768.0f;
            s[i] = (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (g_playwav.f) ipa_wav_write(&g_playwav, s, (size_t) n * 4);
    }
    free(f);
    free(s);
    return 0;
}

// ---- マイクの塊を、ACK で決めた形にして送る（--mic-delay-ms のときは、届いた順に遅らせて送る）----
#define MQ_N 4096
typedef struct { uint64_t due, index; uint32_t frames; uint32_t len; uint8_t *data; } MicItem;
static MicItem g_mq[MQ_N];
static volatile LONG g_mq_w, g_mq_r;

// L R の float（frames コマ）を、決めた ch と形にする
static uint32_t pack_mic(const float *lr, int frames, uint8_t *out) {
    int bps = ipa_fmt_bytes(g_mic_fmt);
    uint8_t *p = out;
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < g_mic_ch; c++) {
            float v = g_mic_ch == 1 ? 0.5f * (lr[2 * i] + lr[2 * i + 1]) : lr[2 * i + c];
            if (g_mic_fmt == IPA_FMT_F32) { memcpy(p, &v, 4); }
            else if (g_mic_fmt == IPA_FMT_S24) {
                float x = v > 1 ? 1 : v < -1 ? -1 : v;
                int32_t q = (int32_t) (x * 8388607.0f);
                p[0] = (uint8_t) q; p[1] = (uint8_t) (q >> 8); p[2] = (uint8_t) (q >> 16);
            } else {
                float x = v > 1 ? 1 : v < -1 ? -1 : v;
                int16_t q = (int16_t) (x * 32767.0f);
                memcpy(p, &q, 2);
            }
            p += bps;
        }
    }
    return (uint32_t) (p - out);
}

static void mic_emit(const float *lr, int frames, uint64_t index) {
    uint8_t *buf = malloc((size_t) frames * 2 * 4);
    uint32_t len = pack_mic(lr, frames, buf);
    if (!O.mic_delay_ms) {
        send_locked(IPA_T_MIC, 0, (uint16_t) frames, index, buf, len);
        free(buf);
        return;
    }
    LONG w = g_mq_w;
    if (w - g_mq_r >= MQ_N) { free(buf); g_mic_dropped += (uint64_t) frames; return; }
    MicItem *it = &g_mq[w % MQ_N];
    it->due = ipa_now_us() + (uint64_t) O.mic_delay_ms * 1000;
    it->index = index; it->frames = (uint32_t) frames; it->len = len; it->data = buf;
    InterlockedIncrement(&g_mq_w);
}

static DWORD WINAPI delay_thread(LPVOID a) {
    (void) a;
    while (!g_stop) {
        if (g_mq_r == g_mq_w) { Sleep(1); continue; }
        MicItem *it = &g_mq[g_mq_r % MQ_N];
        if (ipa_now_us() < it->due) { Sleep(1); continue; }
        send_locked(IPA_T_MIC, 0, (uint16_t) it->frames, it->index, it->data, it->len);
        free(it->data);
        InterlockedIncrement(&g_mq_r);
    }
    return 0;
}

static DWORD WINAPI mic_thread(LPVOID a) {
    (void) a;
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    int chunk = g_mic_rate / 100; // 10 ms
    float *lr = malloc(sizeof(float) * 2 * chunk);
    double phase = 0, step = 6.283185307179586 * O.mic_freq / g_mic_rate;
    double phase2 = 0, step2 = step * 1.5;
    double period = (double) chunk / ((double) g_mic_rate * (1.0 + O.ppm * 1e-6)) * 1e6;
    uint64_t t0 = ipa_now_us(), index = 0;
    double k = 1;
    uint64_t count = 0;
    unsigned seed = 12345;
    while (!g_stop) {
        sleep_until(t0 + (uint64_t) (period * k));
        k += 1;
        // --mic-pulse：200 ms 鳴らして 300 ms 黙る（話し声のように、間に静かな所がある音）
        double gate = O.mic_pulse && ((uint64_t) (k - 2) % 50) >= 20 ? 0.0 : 1.0;
        // --stall S:MS：S 秒の所で MS ms 止まり、そのあとまとめて送る（PC か USB が詰まったまね）
        if (O.stall_ms && (uint64_t) (k - 2) == (uint64_t) O.stall_at_s * 100) Sleep((DWORD) O.stall_ms);
        for (int i = 0; i < chunk; i++) {
            lr[2 * i] = (float) (gate * O.mic_amp * sin(phase));
            lr[2 * i + 1] = (float) (gate * O.mic_amp * sin(phase2));
            phase += step; phase2 += step2;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            if (phase2 > 6.283185307179586) phase2 -= 6.283185307179586;
        }
        count++;
        if (O.mic_drop_every && count % (uint64_t) O.mic_drop_every == 0) { // 抜けを作る：番号だけ進めて送らない
            g_mic_dropped += (uint64_t) chunk;
            index += (uint64_t) chunk;
            continue;
        }
        if (O.mic_jitter_ms) { seed = seed * 1103515245u + 12345u; Sleep((seed >> 16) % (unsigned) (O.mic_jitter_ms + 1)); }
        mic_emit(lr, chunk, index);
        index += (uint64_t) chunk;
        g_mic_sent += (uint64_t) chunk;
    }
    free(lr);
    return 0;
}

// --mic-loopback：出力の機器の loopback を、iPhone のマイクの代わりにする。機器の時計で塊ができたら送る
static DWORD WINAPI mic_loop_thread(LPVOID a) {
    (void) a;
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    IMMDevice *d = wa_find(eRender, O.mic_loopback);
    if (!d) { fprintf(stderr, "loopback の機器が見つからない: %s\n", O.mic_loopback); return 0; }
    WaCap c;
    if (!wa_cap_open(&c, d, 1, g_mic_rate)) return 0;
    WaRender keep;
    memset(&keep, 0, sizeof keep);
    if (wa_render_open(&keep, d, NULL, NULL)) wa_render_start(&keep); // 何も鳴っていないときも塊が来るように
    int chunk = g_mic_rate / 100;
    float *lr = malloc(sizeof(float) * 2 * (chunk + 48000));
    int16_t *tmp = malloc(sizeof(int16_t) * 2 * 48000);
    int staged = 0;
    uint64_t index = 0;
    IAudioClient_Start(c.ac);
    while (!g_stop) {
        if (WaitForSingleObject(c.ev, 100) != WAIT_OBJECT_0) continue;
        for (;;) {
            UINT32 n = 0, fr = 0;
            BYTE *data = NULL;
            DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(c.cap, &n)) || !n) break;
            if (FAILED(IAudioCaptureClient_GetBuffer(c.cap, &data, &fr, &flags, NULL, NULL))) break;
            if (fr > 48000) fr = 48000;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset(tmp, 0, (size_t) fr * 4);
            else wa_to_stereo_s16(&c, data, (int) fr, tmp);
            IAudioCaptureClient_ReleaseBuffer(c.cap, fr);
            for (UINT32 i = 0; i < fr; i++) {
                lr[2 * staged] = tmp[2 * i] / 32768.0f;
                lr[2 * staged + 1] = tmp[2 * i + 1] / 32768.0f;
                if (++staged == chunk) {
                    mic_emit(lr, chunk, index);
                    index += (uint64_t) chunk;
                    g_mic_sent += (uint64_t) chunk;
                    staged = 0;
                }
            }
        }
    }
    IAudioClient_Stop(c.ac);
    wa_render_stop(&keep);
    free(lr);
    free(tmp);
    return 0;
}

static DWORD WINAPI stats_thread(LPVOID a) {
    (void) a;
    uint64_t t0 = ipa_now_us();
    while (!g_stop) {
        Sleep(1000);
        // 前に出た：new なら、このつなぎのままマイクを始める（アプリ 0.7.60 の startMicIfWaiting）
        if (g_late_pending && fake_front() && O.late == LATE_NEW && !g_late_th) {
            printf("代役：前に出たので、このつなぎのままマイクを始める\n");
            fflush(stdout);
            InterlockedExchange(&g_mic_running, 1);
            InterlockedExchange(&g_late_pending, 0);
            g_late_th = CreateThread(NULL, 0, O.mic_loopback ? mic_loop_thread : mic_thread, NULL, 0, NULL);
        }
        IpaStatsExt ex;
        memset(&ex, 0, sizeof ex);
        IpaStats *stp = &ex.base;
        if (g_po_on) {
            stp->fill_frames = (uint32_t) po_fill(&g_po);
            stp->target_frames = (uint32_t) g_po.target;
            stp->underruns = (uint32_t) g_po.underruns;
            stp->overruns = (uint32_t) g_po.overruns;
            stp->ratio_ppm = (int32_t) ((g_po.ratio / g_po.nominal - 1.0) * 1e6);
        }
        stp->gap_frames = (uint32_t) g_spk_gap;
        stp->mic_sent_frames = (uint32_t) g_mic_sent;
        stp->mic_dropped_frames = (uint32_t) g_mic_dropped;
        ex.state = (uint8_t) ((fake_front() ? IPA_S_FRONT : 0) | (g_mic_running ? IPA_S_MIC_RUNNING : 0) | (g_late_pending ? IPA_S_MIC_WAITING : 0));
        ex.input_kind = g_mic_running ? IPA_IN_BUILTIN : 0;
        // 前のアプリ（--old・--late bug）は 32 バイト。新しいアプリは 36 バイト
        send_locked(IPA_T_STATS, 0, 0, 0, &ex, (O.old || O.late == LATE_BUG) ? (uint32_t) sizeof(IpaStats) : (uint32_t) sizeof ex);
        uint64_t el = (ipa_now_us() - t0) / 1000000;
        if (O.freeze_after && el >= (uint64_t) O.freeze_after && !g_frozen) { printf("代役：%d 秒たったので、つないだまま止まる\n", O.freeze_after); fflush(stdout); InterlockedExchange(&g_frozen, 1); }
        if (O.drop_after && el >= (uint64_t) O.drop_after) { printf("代役：%d 秒たったので切る\n", O.drop_after); fflush(stdout); InterlockedExchange(&g_stop, 1); shutdown(g_c, SD_BOTH); }
        if (O.seconds && el >= (uint64_t) O.seconds + 2) { InterlockedExchange(&g_stop, 1); shutdown(g_c, SD_BOTH); }
    }
    return 0;
}

static int serve_one(SOCKET ls) {
    g_c = accept(ls, NULL, NULL);
    if (g_c == INVALID_SOCKET) return 1;
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_frozen, 0);
    g_spk_next = g_spk_frames = g_spk_chunks = g_spk_gap = g_spk_late = g_spk_silent_chunks = 0;
    g_mic_sent = g_mic_dropped = 0;
    InterlockedExchange(&g_mic_running, 0);
    InterlockedExchange(&g_late_pending, 0);
    g_late_th = NULL;
    ipa_nodelay(g_c);
    if (!ipa_recv_all(g_c, &g_h, sizeof g_h) || memcmp(g_h.magic, "IPAU", 4)) { fprintf(stderr, "あいさつが違う\n"); closesocket(g_c); return 1; }
    printf("HELLO: version=%u flags=0x%04x spk=%uHz/%uch mic=%uHz/%uch/fmt%u mode=%u chunk=%u target=%ums\n", g_h.version, g_h.flags, g_h.spk_rate, g_h.spk_channels,
           g_h.mic_rate, g_h.mic_channels, g_h.mic_format, g_h.mic_mode, g_h.chunk_frames, g_h.target_ms);
    IpaAck ack;
    memset(&ack, 0, sizeof ack);
    memcpy(ack.magic, "IPAK", 4);
    ack.version = IPA_VERSION;
    if (g_h.flags & IPA_F_SPEAKER) ack.flags |= IPA_A_SPEAKER_OK;
    int want_mic = (g_h.flags & IPA_F_MIC) && !O.no_mic;
    int mic_on = want_mic && fake_front();
    if (mic_on) ack.flags |= IPA_A_MIC_OK;
    else if (want_mic) {
        // アプリが後ろにいる。マイクは前に出たら始まる
        ack.flags |= IPA_A_MIC_NEEDS_FOREGROUND;
        if (O.late != LATE_BUG && !O.old) ack.flags |= IPA_A_MIC_LATER;
        InterlockedExchange(&g_late_pending, O.late != LATE_BUG);
    }
    uint32_t r = g_h.mic_rate ? g_h.mic_rate : 48000;
    if (O.old) {
        // 前の版のアプリ：rate は 8000〜48000、1ch・16 ビット。新しい所は 0 のまま
        g_mic_rate = (int) (r < 8000 ? 8000 : r > 48000 ? 48000 : r);
        g_mic_ch = 1;
        g_mic_fmt = IPA_FMT_S16;
    } else {
        g_mic_rate = (int) (r < 8000 ? 8000 : r > 48000 ? 48000 : r); // 代役の機器は 48 kHz まで
        g_mic_ch = g_h.mic_channels >= 2 && O.app_stereo && !O.mono_input ? 2 : 1;
        g_mic_fmt = g_h.mic_format == IPA_FMT_S24 || g_h.mic_format == IPA_FMT_F32 ? g_h.mic_format : IPA_FMT_S16;
        int mode = g_h.mic_mode >= 1 && g_h.mic_mode <= 3 ? g_h.mic_mode : (g_h.flags & IPA_F_VOICEPROC) ? IPA_MODE_VOICE : O.app_mode;
        ack.mic_mode = (uint8_t) mode;
        ack.input_kind = IPA_IN_BUILTIN;
        ack.flags2 = (uint8_t) (IPA_A2_STEREO_CAPABLE | (g_mic_ch == 2 ? IPA_A2_STEREO_ON : 0));
        ack.hw_in_channels = (uint8_t) (O.mono_input ? 1 : g_mic_ch);
    }
    ack.mic_rate = (uint32_t) g_mic_rate;
    ack.mic_channels = (uint16_t) g_mic_ch;
    ack.mic_format = (uint16_t) g_mic_fmt;
    ack.hw_rate = 48000;
    ack.io_buffer_us = (uint32_t) (O.io_frames * 1e6 / 48000);
    ack.out_latency_us = 4000;
    ack.in_latency_us = 3000;
    if (!ipa_send_all(g_c, &ack, sizeof ack)) return 1;
    printf("ACK: mic=%dHz/%dch/fmt%d mode=%u %s\n", g_mic_rate, g_mic_ch, g_mic_fmt, ack.mic_mode, O.old ? "（前の版のまね）" : "");
    printf("ACK flags=0x%04x%s\n", ack.flags, mic_on ? "" : want_mic ? "（後ろにいるので mic=no）" : "");
    fflush(stdout);

    int spk_on = (g_h.flags & IPA_F_SPEAKER) != 0;
    HANDLE th[4] = {0};
    int nth = 0;
    if (spk_on) {
        po_init(&g_po, 2, (double) g_h.spk_rate, 48000.0, (double) g_h.target_ms);
        g_po.adapt = !O.no_adapt;
        g_po_on = 1;
        if (O.spk_wav) ipa_wav_open(&g_spkwav, O.spk_wav, g_h.spk_rate, 2);
        if (O.play_wav) ipa_wav_open(&g_playwav, O.play_wav, 48000, 2);
        th[nth++] = CreateThread(NULL, 0, play_thread, NULL, 0, NULL);
    }
    if (mic_on) { InterlockedExchange(&g_mic_running, 1); th[nth++] = CreateThread(NULL, 0, O.mic_loopback ? mic_loop_thread : mic_thread, NULL, 0, NULL); }
    if (want_mic && O.mic_delay_ms) th[nth++] = CreateThread(NULL, 0, delay_thread, NULL, 0, NULL);
    th[nth++] = CreateThread(NULL, 0, stats_thread, NULL, 0, NULL);

    uint8_t *pay = malloc(1 << 20);
    int16_t *zeros = calloc(48000, sizeof(int16_t) * 2);
    float *fb = malloc(sizeof(float) * 48000 * 2);
    uint64_t pings = 0;
    while (!g_stop) {
        IpaFrame fh;
        if (!ipa_recv_all(g_c, &fh, sizeof fh)) break;
        if (fh.length > (1u << 20) || (fh.length && !ipa_recv_all(g_c, pay, fh.length))) break;
        uint64_t now = ipa_now_us();
        if (fh.type == IPA_T_SPK && spk_on) {
            if (g_last_arrival_us) {
                double d = (double) (now - g_last_arrival_us) / 1000.0;
                g_arrival_int_sum_ms += d; g_arrival_n++;
                if (d > g_arrival_int_max_ms) g_arrival_int_max_ms = d;
            }
            g_last_arrival_us = now;
            if (fh.index > g_spk_next) {
                uint64_t gap = fh.index - g_spk_next;
                g_spk_gap += gap;
                for (uint64_t left = gap; left;) { int n = left > 48000 ? 48000 : (int) left; if (g_spkwav.f) ipa_wav_write(&g_spkwav, zeros, (size_t) n * 4); po_write_zeros(&g_po, n); left -= (uint64_t) n; }
                g_spk_next = fh.index;
            } else if (fh.index < g_spk_next) { g_spk_late += fh.frames; continue; }
            if (fh.flags & IPA_FR_SILENT) {
                g_spk_silent_chunks++;
                for (uint32_t left = fh.frames; left;) { int n = left > 48000 ? 48000 : (int) left; if (g_spkwav.f) ipa_wav_write(&g_spkwav, zeros, (size_t) n * 4); po_write_zeros(&g_po, n); left -= (uint32_t) n; }
            } else {
                if (fh.length != (uint32_t) fh.frames * 4) { fprintf(stderr, "スピーカーの塊の長さが合わない\n"); break; }
                if (g_spkwav.f) ipa_wav_write(&g_spkwav, pay, fh.length);
                ipa_s16_to_f32((const int16_t *) pay, fb, (size_t) fh.frames * 2);
                po_write(&g_po, fb, fh.frames);
            }
            g_spk_next += fh.frames;
            g_spk_frames += fh.frames;
            g_spk_chunks++;
        } else if (fh.type == IPA_T_PING) {
            send_locked(IPA_T_PONG, 0, 0, fh.index, NULL, 0);
            pings++;
        } else if (fh.type == IPA_T_BYE) {
            break;
        }
    }
    InterlockedExchange(&g_stop, 1);
    send_locked(IPA_T_BYE, 0, 0, 0, NULL, 0);
    for (int i = 0; i < nth; i++) { WaitForSingleObject(th[i], 2000); CloseHandle(th[i]); }
    if (g_late_th) { WaitForSingleObject(g_late_th, 2000); CloseHandle(g_late_th); g_late_th = NULL; }
    while (g_mq_r != g_mq_w) { free(g_mq[g_mq_r % MQ_N].data); InterlockedIncrement(&g_mq_r); }
    if (g_spkwav.f) ipa_wav_close(&g_spkwav);
    if (g_playwav.f) ipa_wav_close(&g_playwav);
    closesocket(g_c);
    g_c = INVALID_SOCKET;
    free(pay); free(zeros); free(fb);
    printf("RESULT fake spk_frames=%llu spk_chunks=%llu spk_silent_chunks=%llu spk_gap=%llu spk_late=%llu arrival_ms_avg=%.2f arrival_ms_max=%.2f\n",
           (unsigned long long) g_spk_frames, (unsigned long long) g_spk_chunks, (unsigned long long) g_spk_silent_chunks, (unsigned long long) g_spk_gap,
           (unsigned long long) g_spk_late, g_arrival_n ? g_arrival_int_sum_ms / (double) g_arrival_n : 0.0, g_arrival_int_max_ms);
    if (g_po_on)
        printf("RESULT fake playout ppm=%+.0f adapt=%d under=%llu over=%llu silence_after_start_frames=%llu fill_ms_min=%.1f fill_ms_max=%.1f target_ms=%.1f final_ratio_ppm=%+.0f played_frames=%llu\n",
               O.ppm, !O.no_adapt, (unsigned long long) g_po.underruns, (unsigned long long) g_po.overruns, (unsigned long long) g_silence_after_start,
               g_fill_min_ms, g_fill_max_ms, g_po.target * 1000.0 / 48000.0, (g_po.ratio / g_po.nominal - 1.0) * 1e6, (unsigned long long) g_played_frames);
    printf("RESULT fake mic_sent=%llu mic_dropped=%llu pings=%llu mic_ch=%d mic_fmt=%d mic_rate=%d\n", (unsigned long long) g_mic_sent, (unsigned long long) g_mic_dropped,
           (unsigned long long) pings, g_mic_ch, g_mic_fmt, g_mic_rate);
    fflush(stdout);
    g_po_on = 0;
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    O.port = 50381; O.io_frames = 256; O.mic_freq = 440; O.mic_amp = 0.3; O.app_stereo = 1; O.app_mode = IPA_MODE_STANDARD;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s に値が要る\n", a), exit(2), (char *) 0))
        if (!strcmp(a, "--port")) O.port = atoi(NEXT());
        else if (!strcmp(a, "--ppm")) O.ppm = atof(NEXT());
        else if (!strcmp(a, "--io-frames")) O.io_frames = atoi(NEXT());
        else if (!strcmp(a, "--no-adapt")) O.no_adapt = 1;
        else if (!strcmp(a, "--no-mic")) O.no_mic = 1;
        else if (!strcmp(a, "--mic-freq")) O.mic_freq = atof(NEXT());
        else if (!strcmp(a, "--mic-amp")) O.mic_amp = atof(NEXT());
        else if (!strcmp(a, "--mic-drop-every")) O.mic_drop_every = atoi(NEXT());
        else if (!strcmp(a, "--mic-jitter-ms")) O.mic_jitter_ms = atoi(NEXT());
        else if (!strcmp(a, "--mic-loopback")) O.mic_loopback = NEXT();
        else if (!strcmp(a, "--mic-delay-ms")) O.mic_delay_ms = atoi(NEXT());
        else if (!strcmp(a, "--old")) O.old = 1;
        else if (!strcmp(a, "--app-stereo")) O.app_stereo = atoi(NEXT());
        else if (!strcmp(a, "--app-mode")) O.app_mode = atoi(NEXT());
        else if (!strcmp(a, "--mono-input")) O.mono_input = 1;
        else if (!strcmp(a, "--accept-loop")) O.accept_loop = 1;
        else if (!strcmp(a, "--drop-after")) O.drop_after = atoi(NEXT());
        else if (!strcmp(a, "--freeze-after")) O.freeze_after = atoi(NEXT());
        else if (!strcmp(a, "--mic-pulse")) O.mic_pulse = 1;
        else if (!strcmp(a, "--stall")) { const char *v = NEXT(); O.stall_at_s = atoi(v); const char *c = strchr(v, ':'); O.stall_ms = c ? atoi(c + 1) : 0; }
        else if (!strcmp(a, "--spk-wav")) O.spk_wav = NEXT();
        else if (!strcmp(a, "--play-wav")) O.play_wav = NEXT();
        else if (!strcmp(a, "--seconds")) O.seconds = atoi(NEXT());
        else if (!strcmp(a, "--bg-until")) O.bg_until = atoi(NEXT());
        else if (!strcmp(a, "--late")) { const char *v = NEXT(); O.late = !strcmp(v, "bug") ? LATE_BUG : !strcmp(v, "fail") ? LATE_FAIL : LATE_NEW; }
        else { fprintf(stderr, "知らない引数: %s\n", a); return 2; }
    }
    g_t0_us = ipa_now_us();
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    if (O.mic_loopback && !wa_init()) { fprintf(stderr, "COM を始められない\n"); return 1; }
    InitializeCriticalSection(&g_cs);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short) O.port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *) &sa, sizeof sa) != 0 || listen(ls, 1) != 0) { fprintf(stderr, "127.0.0.1:%d を開けない\n", O.port); return 1; }
    printf("iPhone の代役：127.0.0.1:%d で待つ（時計のずれ %+.0f ppm・機器の塊 %d コマ・補正 %s・%s）\n", O.port, O.ppm, O.io_frames, O.no_adapt ? "なし" : "あり",
           O.old ? "前の版のアプリのまね" : "新しいアプリのまね");
    fflush(stdout);
    int rc;
    do { rc = serve_one(ls); } while (O.accept_loop);
    return rc;
}
