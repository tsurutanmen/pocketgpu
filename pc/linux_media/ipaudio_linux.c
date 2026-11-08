// ipaudio（Linux 版）：iPhone のマイクを PC の仮想のマイクにし、PC の音を iPhone から出す。Windows 版 ipaudio.c と同じ約束（proto.h）。
//   マイク    ：iPhone から届いた PCM を、仮想のマイクに流す。
//               PipeWire：Audio/Source の流れ「PocketGPU iPhone Mic」（通話アプリのマイクの一覧に出る）
//               PulseAudio：--pulse-mic-sink の出力（start-mic-camera.sh が module-null-sink と module-remap-source で作る）に流す
//   スピーカー：PipeWire：Audio/Sink の流れ「PocketGPU iPhone Speaker」（出力の一覧に出る。そこに出した音が iPhone から鳴る）。
//               --speaker-from default なら、今の出力の音（モニター）を取る（Windows 版の loopback と同じ。PC からも鳴る）
//               PulseAudio：--pulse-spk-source（null-sink のモニター）を取る
// 時計のずれは proto.h の IpaPlayout（Windows 版と同じ）で吸収する。マイクの流れは iPhone が切れても残す（アプリから見て機器が消えない）
//
//   ipaudio --usb [--device UDID] [--mic] [--speaker] [--reconnect] [--backend auto|pipewire|pulse|none]
//   ipaudio --connect 127.0.0.1:50063 --mic --mic-wav m.wav --gen --spk-wav s.wav --backend none --seconds 10   （試験）
//   そのほか  --mic-name 名前・--spk-name 名前・--speaker-from sink|default・--pulse-mic-sink 名前・--pulse-spk-source 名前
//             --mic-channels 1|2・--mic-format 16|24|f32・--mic-rate N・--mic-mode app|raw|standard|voice・--mic-target-ms N（既定 40）
//             --target-ms N（iPhone がためる長さ・既定 40）・--chunk-ms N（既定 10）・--seconds N・--report-sec N・--gen（440 Hz を PC の音の代わりに送る）
//             --mux 口（usbmuxd の口。IPHONE_VK_MUX と同じ書き方）・--list
//   ipaudio --check   この PC で使える物：PIPEWIRE OK <版>|NG・PULSE OK|NG
// 記録の行は Windows 版と同じ言い回し（start-mic-camera.sh が読む）：「つながった。iPhone：hw_rate=… speaker=ok|no mic=ok|no」など
#include "lin_common.h"
#include "proto.h"
#include "pw_lite.h"
#include <math.h>
#include <strings.h>

typedef struct {
    const char *connect, *device, *mic_wav, *spk_wav, *backend, *mic_name, *spk_name, *spk_from, *pa_mic_sink, *pa_spk_src;
    int usb, list, use_speaker, use_mic, gen, reconnect, seconds, report_sec, target_ms, mic_target_ms, chunk_ms;
    int mic_channels, mic_format, mic_rate, mic_mode;
} Opt;
static Opt o;

static volatile sig_atomic_t g_quit;
static volatile int g_stop;      // いまのつなぎを止める
static volatile int g_connected; // iPhone とつながっている（スピーカーの音をためる）
static int g_sock = -1;
static pthread_mutex_t g_send_lk = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_start_us;
static volatile uint64_t g_last_rx_us;
static IpaAck g_ack;
static int g_mic_ch = 1, g_mic_fmt = IPA_FMT_S16, g_mic_rate = 48000;
static volatile int g_phone_state = -1;
static IpaStats g_phone;
static int g_phone_stats_seen;
static const char *g_backend = "none"; // 実際に使っている物

static void on_sig(int s) { (void) s; g_quit = 1; g_stop = 1; }

static struct { uint64_t frames, chunks, silent_chunks, bytes, dropped; double send_max_ms; } S;
static struct { uint64_t frames, chunks, gap_frames, late_frames, bytes; float peak; } M;
static struct { double last, min, max, sum; int n; } RTT;

static int send_frame_locked(uint8_t type, uint8_t flags, uint16_t frames, uint64_t index, const void *p, uint32_t len) {
    pthread_mutex_lock(&g_send_lk);
    int ok = g_sock >= 0 && ipa_send_frame(g_sock, type, flags, frames, index, p, len);
    pthread_mutex_unlock(&g_send_lk);
    return ok;
}

// ---------------------------------------------------------------- マイク：iPhone → 仮想のマイク
static IpaPlayout g_po; // 2ch の float。読むのは出す側（PipeWire・PulseAudio）の時計
static int g_po_on;
#define OUT_RATE 48000

static inline int16_t clip16(float v) {
    float x = v * 32768.0f;
    return x >= 32767.0f ? 32767 : x <= -32768.0f ? -32768 : (int16_t) lrintf(x);
}

// ---------------------------------------------------------------- スピーカー：PC → iPhone（ためる輪。書くのは取る側、読むのは送る糸）
#define SPK_CAP (OUT_RATE * 2)
static struct {
    pthread_mutex_t lk;
    pthread_cond_t cv;
    int16_t buf[SPK_CAP * 2];
    uint64_t wr, rd;
} R = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, { 0 }, 0, 0 };

static void spk_push(const int16_t *x, uint32_t frames) {
    if (!g_connected || !o.use_speaker) return;
    pthread_mutex_lock(&R.lk);
    for (uint32_t i = 0; i < frames; i++) {
        if (R.wr - R.rd >= SPK_CAP) { R.rd++; S.dropped++; } // 送るのが追いつかない：古い方を捨てる
        size_t at = (size_t) (R.wr % SPK_CAP) * 2;
        R.buf[at] = x[2 * i], R.buf[at + 1] = x[2 * i + 1];
        R.wr++;
    }
    pthread_cond_signal(&R.cv);
    pthread_mutex_unlock(&R.lk);
}

// ---------------------------------------------------------------- PipeWire
static struct {
    void *tl, *mic, *spk;
    struct pwl_stream_events mic_ev, spk_ev;
    int has_requested;
    float *tmp;
    volatile int mic_state, spk_state;
} P;

static void pw_mic_state(void *d, int old, int st, const char *err) {
    (void) d; (void) old;
    P.mic_state = st;
    if (st == PWL_STREAM_STATE_ERROR) fprintf(stderr, "PipeWire のマイク：エラー %s\n", err ? err : "");
}
static void pw_spk_state(void *d, int old, int st, const char *err) {
    (void) d; (void) old;
    P.spk_state = st;
    if (st == PWL_STREAM_STATE_ERROR) fprintf(stderr, "PipeWire のスピーカー：エラー %s\n", err ? err : "");
}
static void pw_mic_process(void *d) {
    (void) d;
    struct pwl_buffer *b = g_pw.dequeue(P.mic);
    if (!b) return;
    struct pwl_data *dd = b->buffer && b->buffer->n_datas ? &b->buffer->datas[0] : NULL;
    if (dd && dd->data && dd->chunk) {
        uint32_t n = dd->maxsize / 4;
        if (P.has_requested && b->requested && b->requested < n) n = (uint32_t) b->requested;
        if (n > 8192) n = 8192;
        int16_t *out = (int16_t *) dd->data;
        if (g_po_on) po_read(&g_po, P.tmp, (int) n);
        else memset(P.tmp, 0, sizeof(float) * 2 * n);
        for (uint32_t i = 0; i < 2 * n; i++) out[i] = clip16(P.tmp[i]);
        dd->chunk->offset = 0, dd->chunk->stride = 4, dd->chunk->size = n * 4, dd->chunk->flags = 0;
        b->size = n;
    }
    g_pw.queue(P.mic, b);
}
static void pw_spk_process(void *d) {
    (void) d;
    struct pwl_buffer *b = g_pw.dequeue(P.spk);
    if (!b) return;
    struct pwl_data *dd = b->buffer && b->buffer->n_datas ? &b->buffer->datas[0] : NULL;
    if (dd && dd->data && dd->chunk) {
        uint32_t off = dd->chunk->offset % (dd->maxsize ? dd->maxsize : 1), sz = dd->chunk->size;
        if (off + sz > dd->maxsize) sz = dd->maxsize - off;
        spk_push((const int16_t *) ((const uint8_t *) dd->data + off), sz / 4);
    }
    g_pw.queue(P.spk, b);
}
static void *pw_audio_format(pwl_pod *p) {
    uint32_t at = pod_obj_begin(p, PWL_TYPE_OBJECT_Format, PWL_PARAM_EnumFormat);
    pod_prop_id(p, PWL_FORMAT_mediaType, PWL_MEDIA_TYPE_audio);
    pod_prop_id(p, PWL_FORMAT_mediaSubtype, PWL_MEDIA_SUBTYPE_raw);
    pod_prop_id(p, PWL_FORMAT_AUDIO_format, PWL_AUDIO_FORMAT_S16_LE);
    pod_prop_int(p, PWL_FORMAT_AUDIO_rate, OUT_RATE);
    pod_prop_int(p, PWL_FORMAT_AUDIO_channels, 2);
    uint32_t pos[2] = { PWL_AUDIO_CHANNEL_FL, PWL_AUDIO_CHANNEL_FR };
    pod_prop_array_id(p, PWL_FORMAT_AUDIO_position, pos, 2);
    pod_obj_end(p, at);
    return p->bad ? NULL : p->b;
}
static int pw_open(void) {
    if (!pwl_load()) return 0;
    P.has_requested = pwl_at_least(0, 3, 49);
    P.tmp = (float *) calloc(8192 * 2, sizeof(float));
    P.tl = g_pw.tl_new("pocketgpu-audio", NULL);
    if (!P.tl || !P.tmp) return 0;
    void *loop = g_pw.tl_loop(P.tl);
    uint32_t fl = PWL_STREAM_FLAG_AUTOCONNECT | PWL_STREAM_FLAG_MAP_BUFFERS | PWL_STREAM_FLAG_RT_PROCESS;
    char lat[32];
    snprintf(lat, sizeof lat, "%d/%d", OUT_RATE / 100, OUT_RATE); // 10 ms
    if (o.use_mic) {
        const char *name = o.mic_name ? o.mic_name : "PocketGPU iPhone Mic";
        void *props = g_pw.props_new("media.class", "Audio/Source", "node.name", "pocketgpu_iphone_mic", "node.description", name, "node.nick", name,
                                     "media.name", name, "node.latency", lat, NULL);
        P.mic_ev.version = 2, P.mic_ev.state_changed = pw_mic_state, P.mic_ev.process = pw_mic_process;
        P.mic = g_pw.stream_new_simple(loop, "pocketgpu-mic", props, &P.mic_ev, NULL);
        pwl_pod p = { .n = 0 };
        const void *params[1] = { pw_audio_format(&p) };
        if (!P.mic || !params[0] || g_pw.stream_connect(P.mic, PWL_DIRECTION_OUTPUT, PWL_ID_ANY, fl, params, 1) < 0) { fprintf(stderr, "PipeWire のマイクを作れない\n"); return 0; }
        printf("マイク：PipeWire に仮想のマイク「%s」（48 kHz・ステレオ）を作った。通話アプリのマイクでこれを選んでください\n", name);
    }
    if (o.use_speaker && !o.gen) {
        int monitor = o.spk_from && !strcmp(o.spk_from, "default");
        const char *name = o.spk_name ? o.spk_name : "PocketGPU iPhone Speaker";
        void *props = monitor ? g_pw.props_new("media.type", "Audio", "media.category", "Capture", "stream.capture.sink", "true", "node.name", "pocketgpu_speaker_capture",
                                               "node.description", "PocketGPU speaker capture", "node.latency", lat, NULL)
                              : g_pw.props_new("media.class", "Audio/Sink", "node.name", "pocketgpu_iphone_speaker", "node.description", name, "node.nick", name,
                                               "media.name", name, "node.latency", lat, NULL);
        P.spk_ev.version = 2, P.spk_ev.state_changed = pw_spk_state, P.spk_ev.process = pw_spk_process;
        P.spk = g_pw.stream_new_simple(loop, "pocketgpu-speaker", props, &P.spk_ev, NULL);
        pwl_pod p = { .n = 0 };
        const void *params[1] = { pw_audio_format(&p) };
        if (!P.spk || !params[0] || g_pw.stream_connect(P.spk, PWL_DIRECTION_INPUT, PWL_ID_ANY, fl, params, 1) < 0) { fprintf(stderr, "PipeWire のスピーカーを作れない\n"); return 0; }
        if (monitor) printf("スピーカー：PipeWire で、今の出力の音を取って iPhone に送る\n");
        else printf("スピーカー：PipeWire に仮想のスピーカー「%s」を作った。音を iPhone から出すには、出力でこれを選んでください\n", name);
    }
    if (g_pw.tl_start(P.tl) < 0) return 0;
    fflush(stdout);
    return 1;
}
static void pw_close(void) {
    if (P.tl) g_pw.tl_stop(P.tl);
    if (P.mic) g_pw.stream_destroy(P.mic);
    if (P.spk) g_pw.stream_destroy(P.spk);
    if (P.tl) g_pw.tl_destroy(P.tl);
    P.tl = P.mic = P.spk = NULL;
}

// ---------------------------------------------------------------- PulseAudio（pa_simple）
static void *g_pa_mic, *g_pa_spk;
static void *pa_mic_thread(void *a) {
    (void) a;
    float f[480 * 2];
    int16_t s[480 * 2];
    while (!g_quit) {
        po_read(&g_po, f, 480);
        for (int i = 0; i < 960; i++) s[i] = clip16(f[i]);
        int e = 0;
        if (g_pa.write(g_pa_mic, s, sizeof s, &e) < 0) { fprintf(stderr, "PulseAudio のマイクに書けない（%s）\n", pa_errtext(e)); break; }
    }
    return NULL;
}
static void *pa_spk_thread(void *a) {
    (void) a;
    int16_t s[480 * 2];
    while (!g_quit) {
        int e = 0;
        if (g_pa.read(g_pa_spk, s, sizeof s, &e) < 0) { fprintf(stderr, "PulseAudio のスピーカーから読めない（%s）\n", pa_errtext(e)); break; }
        spk_push(s, 480);
    }
    return NULL;
}
static int pa_open(void) {
    if (!pa_load()) return 0;
    pwl_pa_spec ss = { PWL_PA_SAMPLE_S16LE, OUT_RATE, 2 };
    int e = 0;
    if (o.use_mic) {
        if (!o.pa_mic_sink) { fprintf(stderr, "PulseAudio では --pulse-mic-sink（仮想のマイクにつながる出力）が要る。start-mic-camera.sh から起動する\n"); return 0; }
        pwl_pa_attr at = { (uint32_t) -1, OUT_RATE * 4 * 40 / 1000, (uint32_t) -1, (uint32_t) -1, (uint32_t) -1 };
        g_pa_mic = g_pa.open(NULL, "PocketGPU", PWL_PA_STREAM_PLAYBACK, o.pa_mic_sink, "iPhone mic", &ss, NULL, &at, &e);
        if (!g_pa_mic) { fprintf(stderr, "PulseAudio の %s を開けない（%s）\n", o.pa_mic_sink, pa_errtext(e)); return 0; }
        pthread_t t;
        pthread_create(&t, NULL, pa_mic_thread, NULL);
        pthread_detach(t);
        printf("マイク：PulseAudio の %s に流す\n", o.pa_mic_sink);
    }
    if (o.use_speaker && !o.gen) {
        const char *src = o.pa_spk_src ? o.pa_spk_src : "@DEFAULT_MONITOR@";
        pwl_pa_attr at = { (uint32_t) -1, (uint32_t) -1, (uint32_t) -1, (uint32_t) -1, OUT_RATE * 4 * 10 / 1000 };
        g_pa_spk = g_pa.open(NULL, "PocketGPU", PWL_PA_STREAM_RECORD, src, "iPhone speaker", &ss, NULL, &at, &e);
        if (!g_pa_spk) { fprintf(stderr, "PulseAudio の %s を開けない（%s）\n", src, pa_errtext(e)); return 0; }
        pthread_t t;
        pthread_create(&t, NULL, pa_spk_thread, NULL);
        pthread_detach(t);
        printf("スピーカー：PulseAudio の %s の音を iPhone に送る\n", src);
    }
    fflush(stdout);
    return 1;
}

// ---------------------------------------------------------------- 試験：440 Hz を PC の音の代わりに（PC の時計で 10 ms ごと）
static void *gen_thread(void *a) {
    (void) a;
    int16_t s[480 * 2];
    double ph = 0, step = 2 * M_PI * 440.0 / OUT_RATE;
    double t0 = lc_now_us();
    for (uint64_t k = 1; !g_quit; k++) {
        for (int i = 0; i < 480; i++) { int16_t v = (int16_t) (sin(ph) * 0.3 * 32767); s[2 * i] = s[2 * i + 1] = v; ph += step; }
        if (ph > 2 * M_PI * 1000) ph = fmod(ph, 2 * M_PI);
        spk_push(s, 480);
        double due = t0 + (double) k * 10000.0, now = lc_now_us();
        if (due > now) lc_sleep_ms((int) ((due - now) / 1000.0));
    }
    return NULL;
}

// ---------------------------------------------------------------- 送る糸（スピーカー）：ためた音を 1 塊ずつ送る。音が来ない間は無音の塊を 10 ms ごとに送る
typedef struct { int chunk; IpaWav *wav; } SpkArg;
static void *spk_send_thread(void *arg) {
    SpkArg *a = (SpkArg *) arg;
    int n = a->chunk;
    int16_t *stage = (int16_t *) calloc((size_t) n * 2, sizeof(int16_t));
    uint64_t index = 0;
    double idle_since = lc_now_us(), next_silent = 0;
    pthread_mutex_lock(&R.lk);
    R.rd = R.wr; // つないだ時より前の音は送らない
    pthread_mutex_unlock(&R.lk);
    while (!g_stop) {
        pthread_mutex_lock(&R.lk);
        if (R.wr - R.rd < (uint64_t) n) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000000L;
            if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&R.cv, &R.lk, &ts);
        }
        int have = R.wr - R.rd >= (uint64_t) n;
        if (have) {
            for (int i = 0; i < n; i++) {
                size_t at = (size_t) (R.rd % SPK_CAP) * 2;
                stage[2 * i] = R.buf[at], stage[2 * i + 1] = R.buf[at + 1];
                R.rd++;
            }
        }
        pthread_mutex_unlock(&R.lk);
        double now = lc_now_us();
        int silent = 0;
        if (have) {
            idle_since = now;
            silent = 1;
            for (int i = 0; i < 2 * n; i++) if (stage[i]) { silent = 0; break; }
        } else {
            // 50 ms 何も来ない（何も鳴っていない・機器が止まっている）：無音の塊を時計で送り、iPhone の側の流れを保つ
            if (now - idle_since < 50000 || now < next_silent) continue;
            next_silent = (next_silent && now - next_silent < 50000 ? next_silent : now) + (double) n * 1e6 / OUT_RATE;
            memset(stage, 0, sizeof(int16_t) * 2 * (size_t) n);
            silent = 1;
        }
        double t0 = lc_now_us();
        int ok = silent ? send_frame_locked(IPA_T_SPK, IPA_FR_SILENT, (uint16_t) n, index, NULL, 0)
                        : send_frame_locked(IPA_T_SPK, 0, (uint16_t) n, index, stage, (uint32_t) n * 4);
        double ms = (lc_now_us() - t0) / 1000.0;
        if (ms > S.send_max_ms) S.send_max_ms = ms;
        if (!ok) { g_stop = 1; break; }
        if (a->wav) ipa_wav_write(a->wav, stage, (size_t) n * 4);
        S.chunks++, S.frames += (uint64_t) n;
        if (silent) S.silent_chunks++; else S.bytes += (uint64_t) n * 4;
        index += (uint64_t) n;
    }
    free(stage);
    return NULL;
}

// ---------------------------------------------------------------- 様子を見る糸：1 秒ごとに PING・状態の行・止まった相手の見つけ
static void *report_thread(void *arg) {
    (void) arg;
    int sec = 0;
    while (!g_stop) {
        for (int i = 0; i < 10 && !g_stop; i++) lc_sleep_ms(100);
        if (g_stop) break;
        sec++;
        send_frame_locked(IPA_T_PING, 0, 0, ipa_now_us(), NULL, 0);
        uint64_t last = g_last_rx_us, now = ipa_now_us();
        if (o.reconnect && now > last && now - last > 6000000) {
            printf("iPhone から 6 秒間データが届きません。再接続します。\n");
            fflush(stdout);
            g_stop = 1;
            shutdown(g_sock, SHUT_RDWR);
            break;
        }
        if (o.report_sec > 0 && sec % o.report_sec == 0) {
            printf("t=%2ds", sec);
            if (o.use_speaker) printf(" | spk: frames=%llu chunks=%llu silent=%llu dropped=%llu send_max_ms=%.2f", (unsigned long long) S.frames, (unsigned long long) S.chunks,
                                      (unsigned long long) S.silent_chunks, (unsigned long long) S.dropped, S.send_max_ms);
            if (o.use_mic) {
                printf(" | mic: frames=%llu gaps=%llu late=%llu", (unsigned long long) M.frames, (unsigned long long) M.gap_frames, (unsigned long long) M.late_frames);
                if (g_po_on) printf(" fill_ms=%.1f under=%llu over=%llu", po_fill(&g_po) * 1000.0 / g_po.src_rate, (unsigned long long) g_po.underruns, (unsigned long long) g_po.overruns);
                printf(" level=%s", M.peak > 0 ? "" : "silent");
                if (M.peak > 0) printf("%.0fdBFS", 20 * log10(M.peak));
                M.peak = 0;
            }
            if (RTT.n) printf(" | rtt_ms last=%.2f min=%.2f avg=%.2f", RTT.last, RTT.min, RTT.sum / RTT.n);
            if (g_phone_stats_seen) printf(" | phone: fill=%u/%u under=%u over=%u gap=%u ratio_ppm=%d", g_phone.fill_frames, g_phone.target_frames, g_phone.underruns, g_phone.overruns, g_phone.gap_frames, g_phone.ratio_ppm);
            if (g_phone_state >= 0) printf(" state=0x%02x", (unsigned) g_phone_state);
            printf("\n");
            fflush(stdout);
        }
        if (o.seconds > 0 && (ipa_now_us() - g_start_us) / 1000000 >= (uint64_t) o.seconds) {
            send_frame_locked(IPA_T_BYE, 0, 0, 0, NULL, 0);
            lc_sleep_ms(100);
            g_quit = 1, g_stop = 1;
            shutdown(g_sock, SHUT_RDWR);
        }
    }
    return NULL;
}

static const char *mode_name(int m) { return m == IPA_MODE_RAW ? "加工なし" : m == IPA_MODE_STANDARD ? "標準" : m == IPA_MODE_VOICE ? "雑音とエコーを消す" : "不明"; }

// ---------------------------------------------------------------- 1 回のつなぎ
static int run_connection(int conn_no) {
    memset(&S, 0, sizeof S);
    memset(&M, 0, sizeof M);
    memset(&RTT, 0, sizeof RTT);
    RTT.min = 1e9;
    g_phone_stats_seen = 0, g_phone_state = -1, g_stop = 0;
    int quiet = o.reconnect && conn_no > 0;
    int s;
    if (o.connect) {
        char host[256], port[16];
        lc_split_hostport(o.connect, host, sizeof host, port, sizeof port, "50063");
        s = lc_tcp_connect(host, port);
        if (s < 0) { if (!quiet) fprintf(stderr, "%s:%s につながらない\n", host, port); return 1; }
    } else {
        s = lc_usb_connect(IPA_PORT, o.device, quiet);
        if (s < 0) return 1;
    }
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    IpaHello h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "IPAU", 4);
    h.version = IPA_VERSION;
    h.flags = (uint16_t) ((o.use_speaker ? IPA_F_SPEAKER : 0) | (o.use_mic ? IPA_F_MIC : 0) | (o.mic_mode == IPA_MODE_VOICE ? IPA_F_VOICEPROC : 0));
    h.spk_rate = OUT_RATE, h.spk_channels = 2, h.spk_format = 1;
    h.mic_rate = (uint32_t) o.mic_rate, h.mic_channels = (uint16_t) o.mic_channels, h.mic_format = (uint16_t) o.mic_format, h.mic_mode = (uint8_t) o.mic_mode;
    h.chunk_frames = (uint16_t) (OUT_RATE * o.chunk_ms / 1000);
    h.target_ms = (uint16_t) o.target_ms;
    lc_set_timeout(s, 5000);
    if (!ipa_send_all(s, &h, sizeof h) || !ipa_recv_all(s, &g_ack, sizeof g_ack) || memcmp(g_ack.magic, "IPAK", 4) || g_ack.version != IPA_VERSION) {
        if (!quiet) fprintf(stderr, "あいさつが通らない（iPhone のアプリで音のポートが開いているか。古い受信アプリではないか）\n");
        close(s);
        return 1;
    }
    lc_set_timeout(s, 0);
    printf("つながった。iPhone：hw_rate=%u io_buffer=%.1fms out_latency=%.1fms in_latency=%.1fms speaker=%s mic=%s%s\n", g_ack.hw_rate, g_ack.io_buffer_us / 1000.0,
           g_ack.out_latency_us / 1000.0, g_ack.in_latency_us / 1000.0, (g_ack.flags & IPA_A_SPEAKER_OK) ? "ok" : "no", (g_ack.flags & IPA_A_MIC_OK) ? "ok" : "no",
           (g_ack.flags & IPA_A_MIC_NEEDS_FOREGROUND) ? "（マイクはアプリを前に出すと始まる）" : "");
    g_mic_ch = g_ack.mic_channels ? g_ack.mic_channels : 1;
    g_mic_fmt = g_ack.mic_format ? g_ack.mic_format : IPA_FMT_S16;
    g_mic_rate = g_ack.mic_rate ? (int) g_ack.mic_rate : 48000;
    printf("マイクの形：%d Hz・%d ch・%s・音の種類=%s\n", g_mic_rate, g_mic_ch, ipa_fmt_name(g_mic_fmt), g_ack.mic_mode ? mode_name(g_ack.mic_mode) : "（前の版のアプリ）");
    fflush(stdout);
    if (g_mic_ch > 8 || g_mic_rate < 8000 || g_mic_rate > 192000 || (g_mic_fmt != IPA_FMT_S16 && g_mic_fmt != IPA_FMT_S24 && g_mic_fmt != IPA_FMT_F32)) {
        fprintf(stderr, "マイクの形が分からない（%d ch・形 %d・%d Hz）。切る\n", g_mic_ch, g_mic_fmt, g_mic_rate);
        close(s);
        return 1;
    }
    IpaWav micwav = { 0 }, spkwav = { 0 };
    if (o.use_mic && o.mic_wav) {
        char path[600];
        if (conn_no <= 0) snprintf(path, sizeof path, "%s", o.mic_wav);
        else {
            const char *dot = strrchr(o.mic_wav, '.');
            int n = dot ? (int) (dot - o.mic_wav) : (int) strlen(o.mic_wav);
            snprintf(path, sizeof path, "%.*s_%d%s", n, o.mic_wav, conn_no + 1, dot ? dot : "");
        }
        if (!ipa_wav_open_fmt(&micwav, path, (uint32_t) g_mic_rate, (uint16_t) g_mic_ch, g_mic_fmt)) fprintf(stderr, "WAV を開けない: %s\n", path);
    }
    if (o.spk_wav && conn_no <= 0) ipa_wav_open(&spkwav, o.spk_wav, OUT_RATE, 2);
    if (g_po_on) po_restart(&g_po, (double) g_mic_rate);
    pthread_mutex_lock(&g_send_lk);
    g_sock = s;
    pthread_mutex_unlock(&g_send_lk);
    g_last_rx_us = ipa_now_us();
    g_connected = 1;
    pthread_t rt, st;
    pthread_create(&rt, NULL, report_thread, NULL);
    SpkArg sa = { h.chunk_frames, spkwav.f ? &spkwav : NULL };
    int have_st = o.use_speaker && (g_ack.flags & IPA_A_SPEAKER_OK) && pthread_create(&st, NULL, spk_send_thread, &sa) == 0;

    uint8_t *pay = (uint8_t *) malloc(1 << 20);
    float *fbuf = (float *) malloc(sizeof(float) * 96000 * 8), *f2 = (float *) malloc(sizeof(float) * 96000 * 2);
    uint8_t *zeros = (uint8_t *) calloc(48000 * 8, 4);
    uint64_t mic_next = 0;
    int bps = ipa_fmt_bytes(g_mic_fmt), mch = g_mic_ch;
    int late_noted = (g_ack.flags & IPA_A_MIC_OK) != 0;
    while (!g_stop) {
        IpaFrame fh;
        if (!ipa_recv_all(s, &fh, sizeof fh)) break;
        if (fh.length > (1u << 20)) { fprintf(stderr, "長さ %u が上限を超えた。形が違うので切る\n", fh.length); break; }
        if (fh.length && !ipa_recv_all(s, pay, fh.length)) break;
        g_last_rx_us = ipa_now_us();
        if (fh.type == IPA_T_MIC && o.use_mic) {
            if (!late_noted) { late_noted = 1; printf("iPhone のマイクが開始されました。\n"); fflush(stdout); }
            if (fh.index > mic_next) { // 抜けたぶんを無音で埋める
                uint64_t left = fh.index - mic_next;
                M.gap_frames += left;
                while (left) {
                    int n = left > 48000 ? 48000 : (int) left;
                    if (micwav.f) ipa_wav_write(&micwav, zeros, (size_t) n * mch * bps);
                    if (g_po_on) po_write_zeros(&g_po, n);
                    left -= (uint64_t) n;
                }
                mic_next = fh.index;
            } else if (fh.index < mic_next) { M.late_frames += fh.frames; continue; }
            if (fh.flags & IPA_FR_SILENT) {
                uint32_t left = fh.frames;
                while (left) {
                    int n = left > 48000 ? 48000 : (int) left;
                    if (micwav.f) ipa_wav_write(&micwav, zeros, (size_t) n * mch * bps);
                    if (g_po_on) po_write_zeros(&g_po, n);
                    left -= (uint32_t) n;
                }
            } else {
                if (fh.length != (uint32_t) fh.frames * mch * bps) { fprintf(stderr, "マイクの塊の長さが合わない（%u バイト・%u コマ・%d ch・%d バイト）\n", fh.length, fh.frames, mch, bps); break; }
                if (micwav.f) ipa_wav_write(&micwav, pay, fh.length);
                ipa_pcm_to_f32(g_mic_fmt, pay, fbuf, (size_t) fh.frames * mch);
                float pk = M.peak;
                for (size_t i = 0; i < (size_t) fh.frames * mch; i++) { float v = fabsf(fbuf[i]); if (v > pk) pk = v; }
                M.peak = pk;
                if (g_po_on) {
                    for (uint32_t i = 0; i < fh.frames; i++) f2[2 * i] = fbuf[(size_t) i * mch], f2[2 * i + 1] = fbuf[(size_t) i * mch + (mch > 1 ? 1 : 0)];
                    po_write(&g_po, f2, fh.frames);
                }
            }
            mic_next += fh.frames;
            M.frames += fh.frames, M.chunks++, M.bytes += fh.length;
        } else if (fh.type == IPA_T_PING) {
            send_frame_locked(IPA_T_PONG, 0, 0, fh.index, NULL, 0);
        } else if (fh.type == IPA_T_PONG) {
            double ms = (double) (ipa_now_us() - fh.index) / 1000.0;
            RTT.last = ms, RTT.sum += ms, RTT.n++;
            if (ms < RTT.min) RTT.min = ms;
            if (ms > RTT.max) RTT.max = ms;
        } else if (fh.type == IPA_T_STATS && fh.length >= sizeof(IpaStats)) {
            memcpy(&g_phone, pay, sizeof g_phone);
            g_phone_stats_seen = 1;
            if (fh.length >= sizeof(IpaStatsExt)) { IpaStatsExt ex; memcpy(&ex, pay, sizeof ex); g_phone_state = ex.state; }
        } else if (fh.type == IPA_T_BYE) break;
    }
    g_stop = 1;
    g_connected = 0;
    shutdown(s, SHUT_RDWR);
    pthread_cond_broadcast(&R.cv);
    if (have_st) pthread_join(st, NULL);
    pthread_join(rt, NULL);
    pthread_mutex_lock(&g_send_lk);
    g_sock = -1;
    pthread_mutex_unlock(&g_send_lk);
    close(s);
    if (spkwav.f) ipa_wav_close(&spkwav);
    if (micwav.f) ipa_wav_close(&micwav);
    free(pay), free(fbuf), free(f2), free(zeros);
    printf("RESULT spk_frames=%llu spk_chunks=%llu spk_silent_chunks=%llu spk_dropped=%llu send_max_ms=%.2f\n", (unsigned long long) S.frames, (unsigned long long) S.chunks,
           (unsigned long long) S.silent_chunks, (unsigned long long) S.dropped, S.send_max_ms);
    printf("RESULT mic_frames=%llu mic_chunks=%llu mic_gap_frames=%llu mic_late_frames=%llu mic_rate=%d mic_ch=%d mic_fmt=%d", (unsigned long long) M.frames,
           (unsigned long long) M.chunks, (unsigned long long) M.gap_frames, (unsigned long long) M.late_frames, g_mic_rate, g_mic_ch, g_mic_fmt);
    if (g_po_on) printf(" po_under=%llu po_over=%llu po_skips=%llu", (unsigned long long) g_po.underruns, (unsigned long long) g_po.overruns, (unsigned long long) g_po.skips);
    printf("\nRESULT rtt_n=%d rtt_ms_min=%.2f rtt_ms_avg=%.2f rtt_ms_max=%.2f backend=%s\n", RTT.n, RTT.n ? RTT.min : 0.0, RTT.n ? RTT.sum / RTT.n : 0.0, RTT.max, g_backend);
    fflush(stdout);
    return 0;
}

static int parse_mode(const char *s) {
    if (!strcasecmp(s, "raw")) return IPA_MODE_RAW;
    if (!strcasecmp(s, "standard")) return IPA_MODE_STANDARD;
    if (!strcasecmp(s, "voice")) return IPA_MODE_VOICE;
    return IPA_MODE_APP;
}
static int parse_format(const char *s) { return !strcmp(s, "24") ? IPA_FMT_S24 : !strcasecmp(s, "f32") || !strcmp(s, "32") ? IPA_FMT_F32 : IPA_FMT_S16; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    o.target_ms = 40, o.mic_target_ms = 40, o.chunk_ms = 10, o.report_sec = 1;
    o.mic_channels = 2, o.mic_format = IPA_FMT_S16, o.mic_rate = 48000, o.mic_mode = IPA_MODE_APP;
    o.backend = "auto";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s に値が要る\n", a), exit(2), (char *) 0))
        if (!strcmp(a, "--list")) o.list = 1;
        else if (!strcmp(a, "--check")) {
            if (pwl_load()) printf("PIPEWIRE OK %d.%d.%d\n", g_pw.ver[0], g_pw.ver[1], g_pw.ver[2]);
            else printf("PIPEWIRE NG\n");
            printf(pa_load() ? "PULSE OK\n" : "PULSE NG\n");
            return 0;
        }
        else if (!strcmp(a, "--usb")) o.usb = 1;
        else if (!strcmp(a, "--mux")) g_lc_mux = NEXT();
        else if (!strcmp(a, "--device")) o.device = NEXT();
        else if (!strcmp(a, "--connect")) o.connect = NEXT();
        else if (!strcmp(a, "--speaker")) o.use_speaker = 1;
        else if (!strcmp(a, "--speaker-from")) o.spk_from = NEXT();
        else if (!strcmp(a, "--spk-wav")) o.spk_wav = NEXT();
        else if (!strcmp(a, "--gen")) o.use_speaker = 1, o.gen = 1;
        else if (!strcmp(a, "--mic")) o.use_mic = 1;
        else if (!strcmp(a, "--mic-wav")) o.use_mic = 1, o.mic_wav = NEXT();
        else if (!strcmp(a, "--mic-name")) o.mic_name = NEXT();
        else if (!strcmp(a, "--spk-name")) o.spk_name = NEXT();
        else if (!strcmp(a, "--backend")) o.backend = NEXT();
        else if (!strcmp(a, "--pulse-mic-sink")) o.pa_mic_sink = NEXT();
        else if (!strcmp(a, "--pulse-spk-source")) o.pa_spk_src = NEXT();
        else if (!strcmp(a, "--mic-channels")) o.mic_channels = atoi(NEXT()) >= 2 ? 2 : 1;
        else if (!strcmp(a, "--mic-format")) o.mic_format = parse_format(NEXT());
        else if (!strcmp(a, "--mic-rate")) o.mic_rate = atoi(NEXT());
        else if (!strcmp(a, "--mic-mode")) o.mic_mode = parse_mode(NEXT());
        else if (!strcmp(a, "--mic-target-ms")) o.mic_target_ms = atoi(NEXT());
        else if (!strcmp(a, "--target-ms")) o.target_ms = atoi(NEXT());
        else if (!strcmp(a, "--chunk-ms")) o.chunk_ms = atoi(NEXT());
        else if (!strcmp(a, "--seconds")) o.seconds = atoi(NEXT());
        else if (!strcmp(a, "--reconnect")) o.reconnect = 1;
        else if (!strcmp(a, "--report-sec")) o.report_sec = atoi(NEXT());
        else { fprintf(stderr, "知らない引数: %s\n", a); return 2; }
    }
    if (o.mic_rate < 8000 || o.mic_rate > 96000) { fprintf(stderr, "--mic-rate は 8000〜96000\n"); return 2; }
    if (o.chunk_ms < 2 || o.chunk_ms > 100) { fprintf(stderr, "--chunk-ms は 2〜100\n"); return 2; }
    if (o.list) return lc_list_devices();
    if (!o.connect && !o.usb) { fprintf(stderr, "--usb か --connect host:port が要る\n"); return 2; }
    if (!o.use_speaker && !o.use_mic) { fprintf(stderr, "--speaker か --mic が要る\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    g_start_us = ipa_now_us();
    // 出す先：PipeWire → PulseAudio。none は WAV と --gen だけ（試験）
    int want_out = (o.use_mic && !o.mic_wav) || (o.use_speaker && !o.gen);
    if (o.use_mic) {
        po_init(&g_po, 2, o.mic_rate, OUT_RATE, o.mic_target_ms);
        g_po.skip_quiet = 1;
    }
    if (strcmp(o.backend, "none") && want_out) {
        if ((!strcmp(o.backend, "auto") || !strcmp(o.backend, "pipewire")) && pw_open()) g_backend = "pipewire";
        else if (strcmp(o.backend, "pipewire") && pa_open()) g_backend = "pulse";
        else {
            fprintf(stderr, "PipeWire（libpipewire-0.3.so.0）にも PulseAudio（libpulse-simple.so.0）にもつながらないので、仮想のマイクとスピーカーを作れない\n");
            return 3;
        }
        g_po_on = o.use_mic;
    }
    if (o.gen) {
        pthread_t t;
        pthread_create(&t, NULL, gen_thread, NULL);
        pthread_detach(t);
        printf("スピーカー：PC の音の代わりに 440 Hz を送る（試験）\n");
    }
    int conn = 0, fails = 0, rc = 0;
    for (;;) {
        int r = run_connection(conn);
        if (r == 0) conn++, fails = 0;
        if (g_quit || !o.reconnect) { rc = r; break; }
        if (r != 0 && fails++ == 0) printf("iPhone を待っています。USB でつなぎ、アプリの「スピーカーとマイクとして使う」をオンにしてください。\n");
        else if (r == 0) printf("接続が切れました。再接続します。\n");
        fflush(stdout);
        for (int i = 0; i < 20 && !g_quit; i++) lc_sleep_ms(100);
        if (o.seconds > 0 && (ipa_now_us() - g_start_us) / 1000000 >= (uint64_t) o.seconds) break;
    }
    g_quit = 1;
    if (!strcmp(g_backend, "pipewire")) pw_close();
    printf("RESULT connections=%d\n", conn);
    return rc;
}
