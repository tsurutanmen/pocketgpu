// 録画（VR_CAP_RECORD・OBS の録画を iPhone で・道 A）。server.c の中に取り込む（vlog・t_ms・sleep_ms・drain_upto などを使う）。
// ゲームを iPhone の GPU で描いているとき、絵はもう iPhone の中にある。窓に出すコマ（持ち帰り用の buffer）を、
// 受け手が自分で高画質に符号化してファイルに書く。USB には絵を流さない（流すのは「録れ」「止めろ」とコマの印だけ）。
// PC の音は、スピーカーの係の道（work/audio：PC の ipaudio.exe が WASAPI の loopback で取り、口 50063 で iPhone の AudioBridge へ）を使う。
// AudioBridge が届いた塊を vkr_record_pcm で録画にも渡す。PC の代役では、同じ約束の小さな受け口をここに持つ（VKR_RECORD_AUDIO_LISTEN）
//
// 受け手の中の流れ：
//   VR_RECORD_CTL（始める）→ 印を立てるだけ。中身を書くのは最初のコマが来てから（そのコマの大きさで）
//   VR_RECORD_FRAME → その memory を使う仕事が終わるのを待ち（drain_upto）、絵を写して録画の糸に渡す（糸が詰まっていたら落とす）
//   vkr_record_pcm（口 50063 の受け手の糸から）→ PC の音（16 ビットの PCM）を録画の糸に渡す（落とさない）
//   録画の糸 → 機械ごとの書き手（PC の代役は Media Foundation の Sink Writer・iPhone はアプリが登録する AVAssetWriter）
//   VR_RECORD_CTL（止める）→ 糸に止めろと言い、書き終えるまで待って答える
//   VR_RECORD_FETCH → 最後に書き終えたファイルを PC に返す（場所は受け手が決めた物だけ。PC からは場所を指せない）
#pragma once

// ---- 機械ごとの書き手 ----
// start：name はファイルの名前（受け手が決めて渡す。英数字と - _ . だけ）。codec は VR_REC_H264/HEVC。arate が 0 なら音なし。
//        dest は VR_REC_* の印。書く場所を path（UTF-8）に入れて、作業用の印を返す。作れなければ NULL
// video：BGRA（bgra が 0 なら RGBA）の w×h。時刻は µs（録画の最初のコマを 0 にした値）
// audio：16 ビットの PCM（ch 個ずつ交互）を frames 組。時刻は µs
// stop：書き終えるまで待つ。1 なら書けた
typedef void *(*vkr_rec_start_fn)(const char *name, uint32_t w, uint32_t h, uint32_t fps, uint32_t codec, uint32_t kbps,
                                  uint32_t arate, uint32_t ach, uint32_t akbps, uint32_t dest, char *path, uint32_t path_cap);
typedef int (*vkr_rec_video_fn)(void *ctx, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int64_t pts_us);
typedef int (*vkr_rec_audio_fn)(void *ctx, const int16_t *pcm, uint32_t frames, uint32_t rate, uint32_t ch, int64_t pts_us);
typedef int (*vkr_rec_stop_fn)(void *ctx);

static struct {
    vkr_rec_start_fn start;
    vkr_rec_video_fn video;
    vkr_rec_audio_fn audio;
    vkr_rec_stop_fn stop;
} g_recb;

// 口 50063 で届いた PC の音を録画に入れる（下の本体の説明を見る）。iPhone は AudioBridge.swift の受ける糸から呼ぶ
void vkr_record_pcm(const int16_t *pcm, uint32_t frames, uint32_t rate, uint32_t ch, uint64_t index);

// iPhone のアプリ（Swift）が呼ぶ。4 つとも揃っていれば caps で VR_CAP_RECORD を名乗る（vkr_serve より前に呼ぶ）
void vkr_set_recorder(vkr_rec_start_fn start, vkr_rec_video_fn video, vkr_rec_audio_fn audio, vkr_rec_stop_fn stop) {
    g_recb.start = start, g_recb.video = video, g_recb.audio = audio, g_recb.stop = stop;
}

// ---- 糸と錠（Windows と POSIX） ----
#ifdef _WIN32
typedef SRWLOCK rec_mutex;
typedef CONDITION_VARIABLE rec_cond;
#define REC_LOCK(m) AcquireSRWLockExclusive(m)
#define REC_UNLOCK(m) ReleaseSRWLockExclusive(m)
#define REC_WAIT(c, m) SleepConditionVariableSRW(c, m, INFINITE, 0)
#define REC_WAKE(c) WakeAllConditionVariable(c)
#else
typedef pthread_mutex_t rec_mutex;
typedef pthread_cond_t rec_cond;
#define REC_LOCK(m) pthread_mutex_lock(m)
#define REC_UNLOCK(m) pthread_mutex_unlock(m)
#define REC_WAIT(c, m) pthread_cond_wait(c, m)
#define REC_WAKE(c) pthread_cond_broadcast(c)
#endif

// 2 GB を超えるファイルも読めるように（Windows の long は 32 ビット）
#ifdef _WIN32
#define REC_FSEEK _fseeki64
#define REC_FTELL _ftelli64
#else
#define REC_FSEEK fseeko
#define REC_FTELL ftello
#endif

#define REC_SLOTS 3 // 絵を待たせておける数。1080p で 1 枚 8 MB。iPhone のメモリを考えて少なめ
struct rec_audio_chunk {
    struct rec_audio_chunk *next;
    uint64_t index;         // 口 50063 の塊の番号（その向きの最初のコマから数えたコマ）
    uint32_t rate, ch, frames;
    int16_t pcm[];
};

// 2 つの時計を合わせる：「届いた時刻 − 相手の時刻」のいちばん小さい値（待たされた分を除いた物）を、5 秒ごとに入れ替える窓で覚える。
// 窓を入れ替えるので、時計がわずかにずれていっても追いかける
typedef struct { double cur, prev, t0; int has_cur, has_prev; } rec_minwin;
static void minwin_put(rec_minwin *m, double now, double v) {
    if (!m->has_cur && !m->has_prev) m->t0 = now;
    if (now - m->t0 > 5000) { m->prev = m->cur, m->has_prev = m->has_cur, m->has_cur = 0, m->t0 = now; }
    if (!m->has_cur || v < m->cur) m->cur = v, m->has_cur = 1;
}
static int minwin_get(const rec_minwin *m, double *v) {
    if (m->has_cur && m->has_prev) { *v = m->cur < m->prev ? m->cur : m->prev; return 1; }
    if (m->has_cur) { *v = m->cur; return 1; }
    if (m->has_prev) { *v = m->prev; return 1; }
    return 0;
}

static struct {
    rec_mutex mu;
    rec_cond cv;
    int inited;
    int state;      // 0 止まっている・1 録画中（糸が動いている）・2 失敗した
    int stop_req;   // 止めろと言った
    int thr_on;
#ifdef _WIN32
    HANDLE thr;
#else
    pthread_t thr;
#endif
    // 始めるときの設定
    uint32_t codec, kbps, fps, arate, ach, akbps, dest;
    char name[96];
    // 絵の待ち行列（受け手の主の糸が入れ、録画の糸が出す）
    struct { uint8_t *px; size_t cap; uint32_t w, h, stride; int bgra; int64_t pts; int full; } v[REC_SLOTS];
    unsigned vin, vout;
    struct rec_audio_chunk *ahead, **atail;
    // 数
    uint64_t frames, dropped, asamples, bytes;
    int64_t first_pts, last_pts;
    uint32_t w, h;
    uint32_t pre_w, pre_h; // 始めろで伝えられた大きさ（0 なら最初のコマで決める）
    char path[512];      // 書いている（書いた）ファイル
    char last_path[512]; // 最後に書き終えたファイル（VR_RECORD_FETCH・VR_REC_DELETE はこれだけ）
    double busy_ms;      // 書き手に渡すのにかかった時間（コマの合計）
    // 時計合わせ（どちらも受け手の時計の ms。錠 mu の中で読み書きする）
    rec_minwin vwin;     // コマ：届いた時刻 − PC の時刻（PC の時計 → 受け手の時計）
    rec_minwin awin;     // 音：届いた時刻 − 塊の番号の時刻（音の番号 → 受け手の時計）
    uint64_t a_last_index;
    double a_seen_ms;    // 音が最後に届いた時刻（録画していなくても数える。書き手を作るときに音の道があるかを見る）
    uint32_t a_rate, a_ch; // 届いている音の形
} g_rec;

static void rec_init_once(void) {
    if (g_rec.inited) return;
#ifdef _WIN32
    InitializeSRWLock(&g_rec.mu);
    InitializeConditionVariable(&g_rec.cv);
#else
    pthread_mutex_init(&g_rec.mu, NULL);
    pthread_cond_init(&g_rec.cv, NULL);
#endif
    g_rec.atail = &g_rec.ahead;
    g_rec.inited = 1;
}

// 名前は英数字と - _ . だけを残す（PC から場所を指させない）
static void rec_clean_name(const char *in, uint32_t n, char *out, size_t cap) {
    size_t k = 0;
    for (uint32_t i = 0; i < n && in[i] && k + 1 < cap; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out[k++] = c;
    }
    out[k] = 0;
    if (!k) {
        time_t t = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        strftime(out, cap, "iphone-vk-%Y%m%d-%H%M%S", &tmv);
    }
}

// 録画の大きさと違う絵（窓の大きさが変わった）は、左上を合わせて切るか、黒で埋める
static const uint8_t *rec_fit(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, uint32_t rw, uint32_t rh, uint8_t **tmp, size_t *tcap) {
    if (w == rw && h == rh) return px;
    size_t need = (size_t) rw * rh * 4;
    if (*tcap < need) { free(*tmp); *tmp = malloc(need); *tcap = *tmp ? need : 0; }
    if (!*tmp) return NULL;
    memset(*tmp, 0, need);
    uint32_t cw = w < rw ? w : rw, ch = h < rh ? h : rh;
    for (uint32_t y = 0; y < ch; y++) memcpy(*tmp + (size_t) y * rw * 4, px + (size_t) y * stride, (size_t) cw * 4);
    return *tmp;
}

// 書き手を作る（大きさは g_rec.w・g_rec.h）。作れなければ *failed を立てる
static void *rec_open(int *failed) {
    uint32_t kbps = g_rec.kbps;
    if (!kbps) {
        // 決めていなければ、1 画素 1 コマあたり HEVC 0.10・H.264 0.16 ビット（1080p60 で HEVC 約 12 Mbps・H.264 約 20 Mbps）
        double bpp = g_rec.codec == VR_REC_HEVC ? 0.10 : 0.16;
        kbps = (uint32_t) ((double) g_rec.w * g_rec.h * (g_rec.fps ? g_rec.fps : 60) * bpp / 1000.0);
        if (kbps < 2000) kbps = 2000;
    }
    double t0 = t_ms();
    // 音は、頼まれていて（arate が 0 でない）、口 50063 の音がいま届いているときだけ入れる（届いていないのに音の道を作ると、空の音の道ができる）
    uint32_t ar = g_rec.arate && g_rec.a_seen_ms > 0 && t0 - g_rec.a_seen_ms < 1000 ? g_rec.a_rate : 0, ach = g_rec.a_ch ? g_rec.a_ch : 2;
    if (g_rec.arate && !ar) vlog(JE("録画：口 50063 の PC の音が届いていないので、音なしで録る（スピーカーの係の ipaudio を動かすと入る）", "Recording: no PC audio is arriving on port 50063, so recording without audio (it starts when the speaker helper ipaudio runs)"));
    g_rec.arate = ar, g_rec.ach = ach;
    void *ctx = g_recb.start(g_rec.name, g_rec.w, g_rec.h, g_rec.fps ? g_rec.fps : 60, g_rec.codec, kbps, ar, ach, g_rec.akbps,
                             g_rec.dest, g_rec.path, sizeof g_rec.path);
    if (!ctx) {
        *failed = 1;
        vlog(JE("録画：書き手を作れない（%u x %u）", "Recording: cannot create the writer (%u x %u)"), g_rec.w, g_rec.h);
    } else {
        vlog(JE("録画：%s・%u x %u・%s %u kbps・音 %u Hz %u ch・作るのに %.0f ms", "Recording: %s, %u x %u, %s %u kbps, audio %u Hz %u ch, took %.0f ms to create"), g_rec.path, g_rec.w, g_rec.h,
             g_rec.codec == VR_REC_HEVC ? "HEVC" : "H.264", kbps, g_rec.arate, g_rec.ach, t_ms() - t0);
    }
    return ctx;
}

#ifdef _WIN32
static DWORD WINAPI rec_thread(LPVOID arg)
#else
static void *rec_thread(void *arg)
#endif
{
    (void) arg;
#ifdef _WIN32
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
#endif
    void *ctx = NULL;
    int failed = 0, have_first = 0;
    uint8_t *fit = NULL;
    size_t fitcap = 0;
    struct rec_audio_chunk *apend = NULL, **aptail = &apend; // 最初のコマを書くまで待たせておく音
    // 大きさが分かっていれば（ドライバが始めろで伝えた）、コマを待たずに書き手を作る（PC の代役の HEVC は作るのに約 1 秒かかる）
    if (g_rec.w && g_rec.h) {
        ctx = rec_open(&failed);
        // 作っている間にたまったコマは古いので捨てる（最初に数コマだけ書いて、作るのにかかった時間ぶん絵が止まるのを避ける）
        REC_LOCK(&g_rec.mu);
        for (; g_rec.vout != g_rec.vin; g_rec.vout++) g_rec.v[g_rec.vout % REC_SLOTS].full = 0, g_rec.dropped++;
        REC_UNLOCK(&g_rec.mu);
    }
    for (;;) {
        REC_LOCK(&g_rec.mu);
        while (!g_rec.v[g_rec.vout % REC_SLOTS].full && !g_rec.ahead && !g_rec.stop_req) REC_WAIT(&g_rec.cv, &g_rec.mu);
        // 音は先にまとめて取る（落とさない）。絵は 1 枚ずつ
        *aptail = g_rec.ahead;
        while (*aptail) aptail = &(*aptail)->next;
        g_rec.ahead = NULL;
        g_rec.atail = &g_rec.ahead;
        int has_v = g_rec.v[g_rec.vout % REC_SLOTS].full;
        int stop = g_rec.stop_req && !has_v;
        REC_UNLOCK(&g_rec.mu);
        unsigned slot = g_rec.vout % REC_SLOTS;
        // 大きさが分からなかったときは、最初のコマの大きさで作る
        if (has_v && !ctx && !failed) {
            g_rec.w = (g_rec.v[slot].w + 1) & ~1u, g_rec.h = (g_rec.v[slot].h + 1) & ~1u;
            ctx = rec_open(&failed);
        }
        if (has_v) {
            if (ctx && !failed) {
                // 最初に書けたコマの時刻を 0 にする（作っている間に落としたコマの分、音も切る）
                if (!have_first) g_rec.first_pts = g_rec.v[slot].pts, have_first = 1;
                double t0 = t_ms();
                const uint8_t *px = rec_fit(g_rec.v[slot].px, g_rec.v[slot].w, g_rec.v[slot].h, g_rec.v[slot].stride, g_rec.w, g_rec.h, &fit, &fitcap);
                int64_t pts = g_rec.v[slot].pts - g_rec.first_pts;
                if (pts < 0) pts = 0;
                if (!px || !g_recb.video(ctx, px, g_rec.w, g_rec.h, px == g_rec.v[slot].px ? g_rec.v[slot].stride : g_rec.w * 4, g_rec.v[slot].bgra, pts)) {
                    failed = 1;
                    vlog(JE("録画：コマを書けない（%llu コマ目）", "Recording: cannot write a frame (frame #%llu)"), (unsigned long long) g_rec.frames);
                }
                g_rec.busy_ms += t_ms() - t0;
                g_rec.frames++;
                g_rec.last_pts = pts;
            }
            REC_LOCK(&g_rec.mu);
            g_rec.v[slot].full = 0;
            g_rec.vout++;
            REC_UNLOCK(&g_rec.mu);
        }
        // 音は最初のコマを書いてから（それより前の音は切る）。書き手が無ければ捨てる
        if (have_first || stop || failed) {
            struct rec_audio_chunk *al = apend;
            apend = NULL, aptail = &apend;
            while (al) {
                struct rec_audio_chunk *n = al->next;
                double voff = 0, aoff = 0;
                REC_LOCK(&g_rec.mu);
                int clk = minwin_get(&g_rec.vwin, &voff) && minwin_get(&g_rec.awin, &aoff);
                REC_UNLOCK(&g_rec.mu);
                if (ctx && !failed && have_first && clk && g_rec.arate) {
                    // 塊の頭の時刻：受け手の時計で aoff + 番号/rate、PC の時計ではそこから voff を引く
                    double srv_ms = aoff + (double) al->index * 1000.0 / al->rate;
                    int64_t pts = (int64_t) ((srv_ms - voff) * 1000.0) - g_rec.first_pts;
                    int64_t dur = (int64_t) al->frames * 1000000 / (al->rate ? al->rate : 48000);
                    if (pts + dur > 0) {
                        uint32_t skip = pts < 0 ? (uint32_t) ((-pts) * al->rate / 1000000) : 0;
                        if (skip < al->frames) {
                            if (!g_recb.audio(ctx, al->pcm + (size_t) skip * al->ch, al->frames - skip, al->rate, al->ch, pts < 0 ? 0 : pts)) failed = 1;
                            g_rec.asamples += al->frames - skip;
                        }
                    }
                }
                free(al);
                al = n;
            }
        }
        if (stop) break;
    }
    int ok = 0;
    if (ctx) {
        double t0 = t_ms();
        ok = g_recb.stop(ctx) && !failed;
        vlog(JE("録画：書き終えた（%s・%.0f ms）・%llu コマ・落とした %llu・音 %.1f 秒・書き手に渡す 1 コマ %.2f ms", "Recording: finished writing (%s, %.0f ms), %llu frames, dropped %llu, audio %.1f s, %.2f ms per frame handed to the writer"), ok ? JE("成功", "ok") : JE("失敗", "failed"), t_ms() - t0,
             (unsigned long long) g_rec.frames, (unsigned long long) g_rec.dropped, g_rec.arate ? (double) g_rec.asamples / g_rec.arate : 0.0,
             g_rec.frames ? g_rec.busy_ms / g_rec.frames : 0.0);
        FILE *f = fopen(g_rec.path, "rb");
        if (f) {
            REC_FSEEK(f, 0, SEEK_END);
            g_rec.bytes = (uint64_t) REC_FTELL(f);
            fclose(f);
        }
        if (ok) snprintf(g_rec.last_path, sizeof g_rec.last_path, "%s", g_rec.path);
    }
    free(fit);
    REC_LOCK(&g_rec.mu);
    g_rec.state = ok || !ctx ? 0 : 2;
    if (failed && ctx) g_rec.state = 2;
    REC_UNLOCK(&g_rec.mu);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void rec_join(void) {
    if (!g_rec.thr_on) return;
#ifdef _WIN32
    WaitForSingleObject(g_rec.thr, INFINITE);
    CloseHandle(g_rec.thr);
#else
    pthread_join(g_rec.thr, NULL);
#endif
    g_rec.thr_on = 0;
}

static int rec_begin(void) {
    rec_init_once();
    if (g_rec.state == 1) return 1;
    rec_join();
    g_rec.state = 1;
    g_rec.stop_req = 0;
    g_rec.frames = g_rec.dropped = g_rec.asamples = g_rec.bytes = 0;
    g_rec.first_pts = g_rec.last_pts = 0;
    g_rec.w = g_rec.pre_w, g_rec.h = g_rec.pre_h;
    g_rec.path[0] = 0;
    g_rec.busy_ms = 0;
    g_rec.vin = g_rec.vout = 0;
    for (int i = 0; i < REC_SLOTS; i++) g_rec.v[i].full = 0;
#ifdef _WIN32
    g_rec.thr = CreateThread(NULL, 0, rec_thread, NULL, 0, NULL);
    g_rec.thr_on = g_rec.thr != NULL;
#else
    g_rec.thr_on = pthread_create(&g_rec.thr, NULL, rec_thread, NULL) == 0;
#endif
    if (!g_rec.thr_on) g_rec.state = 2;
    return g_rec.thr_on;
}

static void rec_end(void) {
    if (!g_rec.inited || !g_rec.thr_on) return;
    REC_LOCK(&g_rec.mu);
    g_rec.stop_req = 1;
    REC_WAKE(&g_rec.cv);
    REC_UNLOCK(&g_rec.mu);
    rec_join();
}

// 主の糸から：絵を写して渡す。糸が詰まっていれば落とす（ゲームの速さを録画のために落とさない）
static void rec_push_video(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int64_t pts) {
    REC_LOCK(&g_rec.mu);
    unsigned slot = g_rec.vin % REC_SLOTS;
    int busy = g_rec.v[slot].full;
    REC_UNLOCK(&g_rec.mu);
    if (busy) { g_rec.dropped++; return; }
    size_t need = (size_t) w * h * 4;
    if (g_rec.v[slot].cap < need) {
        free(g_rec.v[slot].px);
        g_rec.v[slot].px = malloc(need);
        g_rec.v[slot].cap = g_rec.v[slot].px ? need : 0;
        if (!g_rec.v[slot].px) { g_rec.dropped++; return; }
    }
    for (uint32_t y = 0; y < h; y++) memcpy(g_rec.v[slot].px + (size_t) y * w * 4, px + (size_t) y * stride, (size_t) w * 4);
    g_rec.v[slot].w = w, g_rec.v[slot].h = h, g_rec.v[slot].stride = w * 4, g_rec.v[slot].bgra = bgra, g_rec.v[slot].pts = pts;
    REC_LOCK(&g_rec.mu);
    g_rec.v[slot].full = 1;
    g_rec.vin++;
    REC_WAKE(&g_rec.cv);
    REC_UNLOCK(&g_rec.mu);
}

static void rec_push_audio(const int16_t *pcm, uint32_t frames, uint32_t rate, uint32_t ch, uint64_t index) {
    if (!frames || !ch || ch > 8 || frames > 48000 * 4) return;
    struct rec_audio_chunk *c = malloc(sizeof *c + (size_t) frames * ch * 2);
    if (!c) return;
    c->next = NULL, c->index = index, c->rate = rate, c->ch = ch, c->frames = frames;
    if (pcm) memcpy(c->pcm, pcm, (size_t) frames * ch * 2);
    else memset(c->pcm, 0, (size_t) frames * ch * 2); // 無音の塊（中身なし）
    REC_LOCK(&g_rec.mu);
    *g_rec.atail = c;
    g_rec.atail = &c->next;
    REC_WAKE(&g_rec.cv);
    REC_UNLOCK(&g_rec.mu);
}

// VR_RECORD_CTL の返事の中身
static void rec_status(vr_buf *o) {
    vr_u32(o, (uint32_t) g_rec.state);
    vr_u64(o, g_rec.frames);
    vr_u64(o, g_rec.dropped);
    vr_u64(o, g_rec.asamples);
    vr_u64(o, (uint64_t) (g_rec.last_pts > 0 ? g_rec.last_pts : 0));
    vr_u64(o, g_rec.bytes);
    vr_u32(o, g_rec.w);
    vr_u32(o, g_rec.h);
    const char *p = g_rec.state == 1 ? g_rec.path : g_rec.last_path[0] ? g_rec.last_path : g_rec.path;
    vr_bytes(o, p, (uint32_t) strlen(p));
}

// 受け手が切れたとき（ゲームが終わった・落ちた）：録画中なら書き終える。ファイルは残す
static void rec_on_disconnect(void) {
    if (g_rec.inited && g_rec.state == 1) {
        vlog(JE("録画：PC が切れたので書き終える", "Recording: the PC disconnected, so finishing the file"));
        rec_end();
    }
}

// ---- PC の代役：Media Foundation の Sink Writer（mp4 に H.264/HEVC と AAC を書く） ----
#ifdef _WIN32
#include <mfreadwrite.h>
#define REC_GUID(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } };
#define REC_FCC(n, f) REC_GUID(n, f, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
REC_FCC(RG_AUDIO, 0x73647561)
REC_FCC(RG_PCM, 0x00000001)
REC_FCC(RG_AAC, 0x00001610)
REC_FCC(RG_HEVC, 0x43564548)
REC_GUID(RG_A_CH, 0x37e48bf5, 0x645e, 0x4c5b, 0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a)
REC_GUID(RG_A_RATE, 0x5faeeae7, 0x0290, 0x4c31, 0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba)
REC_GUID(RG_A_BITS, 0xf2deb57f, 0x40fa, 0x4764, 0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69)
REC_GUID(RG_A_BYTES, 0x1aab75c8, 0xcfef, 0x451c, 0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31)
REC_GUID(RG_A_ALIGN, 0x322de230, 0x9eeb, 0x43bd, 0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d)
REC_GUID(RG_HW, 0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0)
REC_GUID(RG_NO_THROTTLE, 0x08b845d8, 0x2b74, 0x4afe, 0x9d, 0x53, 0xbe, 0x16, 0xd2, 0xd5, 0xae, 0x4f)
REC_GUID(RG_CONTAINER, 0x150ff23f, 0x4abc, 0x478b, 0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca)
REC_GUID(RG_MP4, 0xdc6cd05d, 0xb9d0, 0x40ef, 0xbd, 0x35, 0xfa, 0x62, 0x2c, 0x1a, 0xb2, 0x8a)
#undef REC_FCC
#undef REC_GUID

typedef HRESULT (WINAPI *rec_sw_create_fn)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
typedef HRESULT (WINAPI *rec_attr_create_fn)(IMFAttributes **, UINT32);
static rec_sw_create_fn g_rec_sw_create;
static rec_attr_create_fn g_rec_attr_create;

typedef struct {
    IMFSinkWriter *w;
    DWORD vs, as;
    uint32_t ew, eh, fps;
    int has_audio;
    int64_t a_next; // 音の次の時刻（100 ns）。届いた時刻がずれても音は詰めて並べる
} mf_rec;

static int mf_rec_probe(void) {
    if (g_rec_sw_create) return 1;
    if (!vid_mf()) return 0;
    HMODULE rw = LoadLibraryW(L"mfreadwrite.dll"), pl = GetModuleHandleW(L"mfplat.dll");
    if (!rw || !pl) return 0;
    g_rec_sw_create = (rec_sw_create_fn) (void *) GetProcAddress(rw, "MFCreateSinkWriterFromURL");
    g_rec_attr_create = (rec_attr_create_fn) (void *) GetProcAddress(pl, "MFCreateAttributes");
    if (!g_rec_attr_create) g_rec_sw_create = NULL;
    return g_rec_sw_create != NULL;
}

static IMFMediaType *mf_rec_atype(const GUID *sub, uint32_t rate, uint32_t ch, uint32_t bytes_per_sec) {
    const vid_mf_api *mf = vid_mf();
    IMFMediaType *t = NULL;
    if (!mf || FAILED(mf->CreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &VG_MAJOR, &RG_AUDIO);
    IMFMediaType_SetGUID(t, &VG_SUBTYPE, sub);
    IMFMediaType_SetUINT32(t, &RG_A_BITS, 16);
    IMFMediaType_SetUINT32(t, &RG_A_RATE, rate);
    IMFMediaType_SetUINT32(t, &RG_A_CH, ch);
    IMFMediaType_SetUINT32(t, &RG_A_BYTES, bytes_per_sec);
    if (IsEqualGUID(sub, &RG_PCM)) IMFMediaType_SetUINT32(t, &RG_A_ALIGN, ch * 2);
    return t;
}

static void mf_rec_free(mf_rec *r) {
    if (!r) return;
    if (r->w) IMFSinkWriter_Release(r->w);
    free(r);
}

static mf_rec *mf_rec_open(const wchar_t *path, uint32_t w, uint32_t h, uint32_t fps, const GUID *codec, uint32_t kbps, uint32_t arate, uint32_t ach,
                           uint32_t akbps, int hw) {
    mf_rec *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->ew = w, r->eh = h, r->fps = fps;
    IMFAttributes *at = NULL;
    if (FAILED(g_rec_attr_create(&at, 4))) { free(r); return NULL; }
    IMFAttributes_SetUINT32(at, &RG_HW, hw ? 1 : 0); // GPU の符号化（NVENC など）を使ってよい
    IMFAttributes_SetGUID(at, &RG_CONTAINER, &RG_MP4);
    // 待たせない：音と絵の時刻が離れると WriteSample が止まって待つ（録画の糸が 1 本なので、そこで詰まる）
    IMFAttributes_SetUINT32(at, &RG_NO_THROTTLE, 1);
    HRESULT hr = g_rec_sw_create(path, NULL, at, &r->w);
    IMFAttributes_Release(at);
    if (FAILED(hr)) { vlog(JE("録画：Sink Writer を作れない（0x%08lx）", "Recording: cannot create the Sink Writer (0x%08lx)"), (unsigned long) hr); mf_rec_free(r); return NULL; }
    // 映像：出す形（H.264 は High）→ 入れる形（NV12）
    IMFMediaType *o = vid_type(codec, w, h, fps, kbps * 1000, IsEqualGUID(codec, &VG_H264) ? 100 : 0);
    hr = o ? IMFSinkWriter_AddStream(r->w, o, &r->vs) : E_FAIL;
    if (o) IMFMediaType_Release(o);
    if (FAILED(hr)) { vlog(JE("録画：映像の形を受け付けない（0x%08lx）", "Recording: the video format is not accepted (0x%08lx)"), (unsigned long) hr); mf_rec_free(r); return NULL; }
    IMFMediaType *in = vid_type(&VG_NV12, w, h, fps, 0, 0);
    hr = in ? IMFSinkWriter_SetInputMediaType(r->w, r->vs, in, NULL) : E_FAIL;
    if (in) IMFMediaType_Release(in);
    if (FAILED(hr)) { vlog(JE("録画：NV12 を入れられない（0x%08lx）", "Recording: cannot set NV12 as the input (0x%08lx)"), (unsigned long) hr); mf_rec_free(r); return NULL; }
    // 音：AAC（毎秒のバイトは 12000・16000・20000・24000 のどれか）← 16 ビットの PCM
    if (arate == 44100 || arate == 48000) {
        uint32_t bps = akbps * 1000 / 8;
        bps = bps >= 24000 ? 24000 : bps >= 20000 ? 20000 : bps >= 16000 ? 16000 : 12000;
        IMFMediaType *ao = mf_rec_atype(&RG_AAC, arate, ach, bps), *ai = mf_rec_atype(&RG_PCM, arate, ach, arate * ach * 2);
        hr = ao && ai ? IMFSinkWriter_AddStream(r->w, ao, &r->as) : E_FAIL;
        if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(r->w, r->as, ai, NULL);
        if (ao) IMFMediaType_Release(ao);
        if (ai) IMFMediaType_Release(ai);
        if (FAILED(hr)) { vlog(JE("録画：音の形を受け付けない（0x%08lx）", "Recording: the audio format is not accepted (0x%08lx)"), (unsigned long) hr); mf_rec_free(r); return NULL; }
        r->has_audio = 1;
    } else if (arate) {
        vlog(JE("録画：音の標本/秒 %u は AAC にできないので、音なしで録る", "Recording: the audio sample rate %u cannot be encoded as AAC, so recording without audio"), arate);
    }
    hr = IMFSinkWriter_BeginWriting(r->w);
    if (FAILED(hr)) { vlog(JE("録画：書き始められない（0x%08lx）", "Recording: cannot begin writing (0x%08lx)"), (unsigned long) hr); mf_rec_free(r); return NULL; }
    return r;
}

static void *mf_rec_start(const char *name, uint32_t w, uint32_t h, uint32_t fps, uint32_t codec, uint32_t kbps, uint32_t arate, uint32_t ach,
                          uint32_t akbps, uint32_t dest, char *path, uint32_t path_cap) {
    (void) dest;
    const char *dir = getenv("VKR_RECORD_DIR");
    snprintf(path, path_cap, "%s\\%s.mp4", dir && *dir ? dir : ".", name);
    wchar_t wp[600];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 600)) return NULL;
    // VKR_VIDEO_HW=0 ならソフトの符号化だけ。HEVC が作れなければ H.264 に戻す
    const char *hwe = getenv("VKR_VIDEO_HW");
    int hw = !(hwe && *hwe == '0');
    mf_rec *r = NULL;
    if (codec == VR_REC_HEVC) {
        r = mf_rec_open(wp, w, h, fps, &RG_HEVC, kbps, arate, ach, akbps, hw);
        if (!r) { vlog(JE("録画：HEVC は使えないので H.264 で録る", "Recording: HEVC is not available, so recording with H.264")); DeleteFileW(wp); }
    }
    if (!r) r = mf_rec_open(wp, w, h, fps, &VG_H264, kbps, arate, ach, akbps, hw);
    return r;
}

static int mf_rec_video(void *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int64_t pts_us) {
    mf_rec *r = (mf_rec *) p;
    const DWORD n = r->ew * r->eh * 3 / 2;
    IMFSample *s = vid_sample(n);
    if (!s) return 0;
    IMFMediaBuffer *b = NULL;
    BYTE *q = NULL;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b)) || FAILED(IMFMediaBuffer_Lock(b, &q, NULL, NULL))) {
        if (b) IMFMediaBuffer_Release(b);
        IMFSample_Release(s);
        return 0;
    }
    vid_rgb_to_nv12(px, w, h, stride, bgra, q, r->ew, q + (size_t) r->ew * r->eh, r->ew);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, n);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, (LONGLONG) pts_us * 10);
    IMFSample_SetSampleDuration(s, (LONGLONG) (10000000ull / r->fps));
    HRESULT hr = IMFSinkWriter_WriteSample(r->w, r->vs, s);
    IMFSample_Release(s);
    if (FAILED(hr)) vlog(JE("録画：WriteSample（映像）0x%08lx", "Recording: WriteSample (video) 0x%08lx"), (unsigned long) hr);
    return SUCCEEDED(hr);
}

static int mf_rec_audio(void *p, const int16_t *pcm, uint32_t frames, uint32_t rate, uint32_t ch, int64_t pts_us) {
    mf_rec *r = (mf_rec *) p;
    if (!r->has_audio) return 1;
    const DWORD n = frames * ch * 2;
    IMFSample *s = vid_sample(n);
    if (!s) return 0;
    IMFMediaBuffer *b = NULL;
    BYTE *q = NULL;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b)) || FAILED(IMFMediaBuffer_Lock(b, &q, NULL, NULL))) {
        if (b) IMFMediaBuffer_Release(b);
        IMFSample_Release(s);
        return 0;
    }
    memcpy(q, pcm, n);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, n);
    IMFMediaBuffer_Release(b);
    // 音の時刻は届いた時刻ではなく、標本の数で詰めて並べる（隙間や重なりを作らない）。大きくずれたら（20 ms 超）合わせ直す
    int64_t t = (int64_t) pts_us * 10;
    if (r->a_next && llabs(t - r->a_next) < 200000) t = r->a_next;
    LONGLONG dur = (LONGLONG) frames * 10000000 / rate;
    r->a_next = t + dur;
    IMFSample_SetSampleTime(s, t);
    IMFSample_SetSampleDuration(s, dur);
    HRESULT hr = IMFSinkWriter_WriteSample(r->w, r->as, s);
    IMFSample_Release(s);
    if (FAILED(hr)) vlog(JE("録画：WriteSample（音）0x%08lx", "Recording: WriteSample (audio) 0x%08lx"), (unsigned long) hr);
    return SUCCEEDED(hr);
}

static int mf_rec_stop(void *p) {
    mf_rec *r = (mf_rec *) p;
    HRESULT hr = IMFSinkWriter_Finalize(r->w);
    if (FAILED(hr)) vlog(JE("録画：Finalize 0x%08lx", "Recording: Finalize 0x%08lx"), (unsigned long) hr);
    mf_rec_free(r);
    return SUCCEEDED(hr);
}

// ---- PC の代役の、口 50063 の約束の小さな受け口（試す用。iPhone では AudioBridge.swift がこの役） ----
// 約束はスピーカーの係の work/audio/DESIGN.md の 3 章：HELLO（40 バイト・"IPAU"）→ ACK（40 バイト・"IPAK"）→ [頭 16 バイト][中身] の繰り返し。
// 鳴らさずに録画に渡すだけ。VKR_RECORD_AUDIO_LISTEN=127.0.0.1:番号 で開ける（ipaudio.exe --connect 127.0.0.1:番号 --speaker でつなぐ）
#pragma pack(push, 1)
typedef struct { char magic[4]; uint16_t version, flags; uint32_t spk_rate; uint16_t spk_ch, spk_fmt; uint32_t mic_rate; uint16_t mic_ch, mic_fmt, chunk, target_ms; uint8_t rsv[12]; } ra_hello;
typedef struct { char magic[4]; uint16_t version, flags; uint32_t mic_rate; uint16_t mic_ch, mic_fmt; uint32_t hw_rate, io_us, out_us, in_us; uint8_t rsv[8]; } ra_ack;
typedef struct { uint8_t type, flags; uint16_t frames; uint32_t length; uint64_t index; } ra_frame;
#pragma pack(pop)

static DWORD WINAPI ra_thread(LPVOID p) {
    SOCKET ls = (SOCKET) (uintptr_t) p;
    uint8_t *buf = malloc(1u << 20);
    for (; buf;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        ra_hello h;
        if (!vr_recv_all(c, &h, sizeof h) || memcmp(h.magic, "IPAU", 4) || h.version != 1 || h.spk_fmt != 1) { closesocket(c); continue; }
        ra_ack a;
        memset(&a, 0, sizeof a);
        memcpy(a.magic, "IPAK", 4);
        a.version = 1, a.flags = (h.flags & 1) ? 1 : 0, a.mic_rate = 48000, a.mic_ch = 1, a.mic_fmt = 1, a.hw_rate = 48000;
        if (!vr_send_all(c, &a, sizeof a)) { closesocket(c); continue; }
        vlog(JE("録画の音の受け口：つながった（%u Hz・%u ch・1 塊 %u コマ）", "Recording audio port: connected (%u Hz, %u ch, %u frames per chunk)"), h.spk_rate, h.spk_ch, h.chunk);
        uint64_t got = 0;
        for (;;) {
            ra_frame f;
            if (!vr_recv_all(c, &f, sizeof f) || f.length > (1u << 20)) break;
            if (f.length && !vr_recv_all(c, buf, f.length)) break;
            if (f.type == 1) { // SPK：無音の印なら中身なし
                int silent = (f.flags & 1) || f.length < (uint32_t) f.frames * h.spk_ch * 2;
                vkr_record_pcm(silent ? NULL : (const int16_t *) buf, f.frames, h.spk_rate, h.spk_ch, f.index);
                got += f.frames;
            } else if (f.type == 3) { // PING → PONG（番号はそのまま）
                ra_frame q = { 4, 0, 0, 0, f.index };
                if (!vr_send_all(c, &q, sizeof q)) break;
            } else if (f.type == 6) {
                break; // BYE
            }
        }
        closesocket(c);
        vlog(JE("録画の音の受け口：切れた（%.1f 秒受けた）", "Recording audio port: disconnected (received %.1f s)"), h.spk_rate ? (double) got / h.spk_rate : 0.0);
    }
    return 0;
}

static void ra_listen(const char *where) {
    char host[64] = "127.0.0.1";
    const char *c = strrchr(where, ':');
    int port = c ? atoi(c + 1) : atoi(where);
    if (c && (size_t) (c - where) < sizeof host) { memcpy(host, where, (size_t) (c - where)); host[c - where] = 0; }
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short) port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *) &sa, sizeof sa) || listen(ls, 1)) {
        vlog(JE("録画の音の受け口：%s:%d を開けない", "Recording audio port: cannot open %s:%d"), host, port);
        if (ls != INVALID_SOCKET) closesocket(ls);
        return;
    }
    vlog(JE("録画の音の受け口：%s:%d で待っている（口 50063 の約束）", "Recording audio port: listening on %s:%d (the port 50063 convention)"), host, port);
    HANDLE t = CreateThread(NULL, 0, ra_thread, (LPVOID) (uintptr_t) ls, 0, NULL);
    if (t) CloseHandle(t);
}

// 代役は Media Foundation が使えれば録画を名乗る（VKR_RECORD_OFF=1 で名乗らない）
static void rec_platform_init(void) {
    const char *off = getenv("VKR_RECORD_OFF");
    if (off && *off == '1') return;
    if (!g_recb.start && mf_rec_probe()) vkr_set_recorder(mf_rec_start, mf_rec_video, mf_rec_audio, mf_rec_stop);
    static int ra_done;
    const char *ra = getenv("VKR_RECORD_AUDIO_LISTEN");
    if (!ra_done && ra && *ra) { ra_done = 1; ra_listen(ra); }
}
#else
static void rec_platform_init(void) {}
#endif

static int rec_available(void) {
    rec_init_once();
    rec_platform_init();
    return g_recb.start && g_recb.video && g_recb.audio && g_recb.stop;
}

// ---- 命令（handle の中から呼ぶ） ----
static int rec_cmd_ctl(vr_sock s, vr_rd *r) {
    rec_init_once();
    uint32_t act = vr_ru32(r), codec = vr_ru32(r), kbps = vr_ru32(r), fps = vr_ru32(r), w = vr_ru32(r), h = vr_ru32(r);
    uint32_t arate = vr_ru32(r), ach = vr_ru32(r), akbps = vr_ru32(r), dest = vr_ru32(r);
    uint32_t nn = 0;
    const char *nm = (const char *) vr_rbytes(r, &nn);
    if (r->bad) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
    if (!rec_available()) return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
    switch (act) {
    case VR_REC_START:
        if (g_rec.state != 1) {
            g_rec.codec = codec == VR_REC_HEVC ? VR_REC_HEVC : VR_REC_H264;
            g_rec.kbps = kbps > 500000 ? 500000 : kbps;
            g_rec.fps = fps && fps <= 240 ? fps : 60;
            g_rec.arate = arate <= 192000 ? arate : 0;
            g_rec.ach = ach >= 1 && ach <= 2 ? ach : 2;
            g_rec.akbps = akbps ? akbps : 192;
            g_rec.dest = dest;
            // 大きさが分かれば先に書き手を作る（0 なら最初のコマの大きさ）
            g_rec.pre_w = w && w <= 8192 ? (w + 1) & ~1u : 0, g_rec.pre_h = h && h <= 8192 ? (h + 1) & ~1u : 0;
            rec_clean_name(nm ? nm : "", nm ? nn : 0, g_rec.name, sizeof g_rec.name);
            if (!rec_begin()) vlog(JE("録画：糸を作れない", "Recording: cannot create the thread"));
            else vlog(JE("録画：始める（%s・%u コマ/秒の目安・音 %u Hz）", "Recording: starting (%s, about %u fps, audio %u Hz)"), g_rec.codec == VR_REC_HEVC ? "HEVC" : "H.264", g_rec.fps, g_rec.arate);
        }
        break;
    case VR_REC_STOP:
        if (g_rec.state == 1) rec_end();
        break;
    case VR_REC_DELETE:
        if (g_rec.state != 1 && g_rec.last_path[0]) {
            if (remove(g_rec.last_path) == 0) vlog(JE("録画：%s を消した", "Recording: removed %s"), g_rec.last_path);
            g_rec.last_path[0] = 0;
            g_rec.path[0] = 0;
            g_rec.bytes = 0;
        }
        break;
    default:
        break;
    }
    rec_status(&g_out);
    return reply(s, 0);
}

static int rec_cmd_frame(vr_rd *r) {
    uint64_t mid = vr_ru64(r), off = vr_ru64(r);
    uint32_t w = vr_ru32(r), h = vr_ru32(r), stride = vr_ru32(r), bgra = vr_ru32(r);
    int64_t pts = (int64_t) vr_ru64(r);
    if (r->bad || !g_rec.inited || g_rec.state != 1) return 1;
    // 届いた時刻（GPU を待つ前）と PC の時刻の差を覚える。音の時刻を PC の時計に直すのに使う
    double now = t_ms();
    REC_LOCK(&g_rec.mu);
    minwin_put(&g_rec.vwin, now, now - pts / 1000.0);
    REC_UNLOCK(&g_rec.mu);
    drain_upto(last_with_mem(mid));
    if (g_err != VK_SUCCESS) return 1;
    ent *e = find(mid);
    if (!e || !e->map || w == 0 || h == 0 || w > 8192 || h > 8192 || stride < (uint64_t) w * 4 || off > e->size || (uint64_t) stride * h > e->size - off)
        return 1;
    rec_push_video((const uint8_t *) e->map + off, w, h, stride, (int) bgra, pts);
    return 1;
}

// 口 50063（スピーカーの係の約束）で届いた PC の音を録画に入れる。iPhone は AudioBridge の受ける糸から、PC の代役は下の受け口から呼ぶ。
// pcm は 16 ビットの交互（NULL なら無音の塊）。index はその塊の頭のコマの番号（つなぎ直すと 0 に戻る）
void vkr_record_pcm(const int16_t *pcm, uint32_t frames, uint32_t rate, uint32_t ch, uint64_t index) {
    if (!g_rec.inited || !frames || (rate != 44100 && rate != 48000) || ch < 1 || ch > 2) return;
    double now = t_ms();
    REC_LOCK(&g_rec.mu);
    if (index < g_rec.a_last_index) memset(&g_rec.awin, 0, sizeof g_rec.awin); // つなぎ直した：番号が 0 に戻ったので覚え直す
    g_rec.a_last_index = index;
    minwin_put(&g_rec.awin, now, now - (double) index * 1000.0 / rate);
    g_rec.a_seen_ms = now, g_rec.a_rate = rate, g_rec.a_ch = ch;
    int on = g_rec.state == 1 && g_rec.arate;
    REC_UNLOCK(&g_rec.mu);
    if (on) rec_push_audio(pcm, frames, rate, ch, index);
}

static int rec_cmd_fetch(vr_sock s, vr_rd *r) {
    uint64_t at = vr_ru64(r);
    uint32_t max = vr_ru32(r);
    if (r->bad) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
    if (!g_rec.inited || g_rec.state == 1 || !g_rec.last_path[0]) return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
    FILE *f = fopen(g_rec.last_path, "rb");
    if (!f) return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
    REC_FSEEK(f, 0, SEEK_END);
    uint64_t total = (uint64_t) REC_FTELL(f);
    if (max > (8u << 20)) max = 8u << 20;
    uint64_t n = at < total ? total - at : 0;
    if (n > max) n = max;
    vr_u64(&g_out, total);
    if (n) {
        vr_reserve(&g_out, (size_t) n);
        REC_FSEEK(f, (int64_t) at, SEEK_SET);
        size_t got = fread(g_out.p + g_out.n, 1, (size_t) n, f);
        g_out.n += got;
    }
    fclose(f);
    return reply(s, 0);
}
