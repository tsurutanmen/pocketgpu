// 録画（VR_CAP_RECORD・OBS の録画を iPhone で・道 A）のドライバ側。icd.c の中に取り込む（struct swapchain の後ろ）。
// 絵は iPhone の中にあるので送らない。送るのは「録れ・止めろ」と、窓に出すコマの印（memory と時刻）だけ。
// PC の音は、スピーカーの係の道（work/audio：ipaudio.exe が WASAPI の loopback で取り、口 50063 で iPhone へ）をそのまま使う。
// iPhone の受け手（AudioBridge）が届いた PCM を録画にも渡す（vkr_record_pcm）。ドライバは音を取らない
//
//   IPHONE_VK_RECORD=秒        最初の present から録り始め、その秒数で止める（試す用）
//   IPHONE_VK_RECORD_FLAG=場所  そのファイルがある間だけ録る（OBS の台本が録画の始め・終わりで作る・消す。250 ms ごとに見る）
//   IPHONE_VK_RECORD_CODEC=h264 で H.264（標準は HEVC。受け手が HEVC を作れなければ H.264）
//   IPHONE_VK_RECORD_KBPS=数    映像のビットレート（標準は受け手が大きさから決める）
//   IPHONE_VK_RECORD_AUDIO=0    PC の音（口 50063 で届く物）を入れない
//   IPHONE_VK_RECORD_FETCH=場所 止めたら、そのフォルダに mp4 を持ち帰る（標準は iPhone に置いたまま）
//   IPHONE_VK_RECORD_KEEP=0     持ち帰ったら iPhone から消す
//   IPHONE_VK_RECORD_PHOTOS=1   書き終えたら iPhone の「写真」に入れる
//   IPHONE_VK_RECORD_NAME=名前  ファイルの名前（英数字と - _。標準は日時）
static struct {
    int ready;               // 設定を読んだ
    double secs;             // IPHONE_VK_RECORD
    char flag[MAX_PATH];     // IPHONE_VK_RECORD_FLAG
    char fetch[MAX_PATH];    // IPHONE_VK_RECORD_FETCH
    char name[64];
    uint32_t codec, kbps, dest;
    int audio, keep;
    int on, by_flag, auto_done;
    double t0, stop_at, next_check;
    uint64_t frames;
    LONG live;               // 生きている swapchain の数（0 になったら止める）
} g_rc;

static void rc_settings(void) {
    if (g_rc.ready) return;
    g_rc.ready = 1;
    const char *e = getenv("IPHONE_VK_RECORD");
    g_rc.secs = e ? atof(e) : 0;
    e = getenv("IPHONE_VK_RECORD_FLAG");
    if (e) snprintf(g_rc.flag, sizeof g_rc.flag, "%s", e);
    e = getenv("IPHONE_VK_RECORD_FETCH");
    if (e) snprintf(g_rc.fetch, sizeof g_rc.fetch, "%s", e);
    e = getenv("IPHONE_VK_RECORD_NAME");
    if (e) snprintf(g_rc.name, sizeof g_rc.name, "%s", e);
    e = getenv("IPHONE_VK_RECORD_CODEC");
    g_rc.codec = e && (!_stricmp(e, "h264") || !_stricmp(e, "avc")) ? VR_REC_H264 : VR_REC_HEVC;
    e = getenv("IPHONE_VK_RECORD_KBPS");
    g_rc.kbps = e && atoi(e) > 0 ? (uint32_t) atoi(e) : 0;
    e = getenv("IPHONE_VK_RECORD_AUDIO");
    g_rc.audio = !(e && *e == '0');
    e = getenv("IPHONE_VK_RECORD_KEEP");
    g_rc.keep = !(e && *e == '0');
    e = getenv("IPHONE_VK_RECORD_PHOTOS");
    g_rc.dest = VR_REC_KEEP | (e && *e == '1' ? VR_REC_PHOTOS : 0);
}

// ---- 受け手に頼む ----
// 返事：状態・コマ・落としたコマ・音の標本・長さ（µs）・大きさ・幅・高さ・場所
typedef struct { uint32_t state, w, h; uint64_t frames, dropped, asamples, dur_us, bytes; char path[512]; } rc_status;

static VkResult rc_ctl(uint32_t act, rc_status *st, uint32_t fps, uint32_t w, uint32_t h) {
    vr_buf b = { 0 }, out = { 0 };
    vr_u32(&b, act);
    vr_u32(&b, g_rc.codec);
    vr_u32(&b, g_rc.kbps);
    vr_u32(&b, fps);
    vr_u32(&b, w);
    vr_u32(&b, h);
    vr_u32(&b, g_rc.audio ? 48000 : 0); // 音を入れる（口 50063 で届く PC の音。届いていなければ受け手が音なしにする）
    vr_u32(&b, 2);
    vr_u32(&b, 192);
    vr_u32(&b, g_rc.dest);
    vr_bytes(&b, g_rc.name, (uint32_t) strlen(g_rc.name));
    VkResult r = rpc_call(VR_RECORD_CTL, &b, &out);
    free(b.p);
    memset(st, 0, sizeof *st);
    if (r == VK_SUCCESS) {
        vr_rd rd = { out.p, out.n, 0, 0 };
        st->state = vr_ru32(&rd);
        st->frames = vr_ru64(&rd);
        st->dropped = vr_ru64(&rd);
        st->asamples = vr_ru64(&rd);
        st->dur_us = vr_ru64(&rd);
        st->bytes = vr_ru64(&rd);
        st->w = vr_ru32(&rd);
        st->h = vr_ru32(&rd);
        uint32_t n = 0;
        const char *p = (const char *) vr_rbytes(&rd, &n);
        if (p && n) snprintf(st->path, sizeof st->path, "%.*s", (int) n, p);
    }
    free(out.p);
    return r;
}

static void rc_start(struct swapchain *sc, const char *why) {
    rc_status st;
    // コマ/秒の目安は present の区切り（区切らなければ 60）。大きさを伝えると、受け手はコマを待たずに書き手を作れる
    uint32_t fps = sc && sc->gap > 0 ? (uint32_t) (1000.0 / sc->gap + 0.5) : 60;
    VkResult r = rc_ctl(VR_REC_START, &st, fps, sc ? sc->w : 0, sc ? sc->h : 0);
    if (r != VK_SUCCESS || st.state != 1) {
        say("録画を始められない（%d・状態 %u）", r, st.state);
        return;
    }
    g_rc.on = 1;
    g_rc.t0 = now_ms();
    g_rc.frames = 0;
    g_rc.stop_at = g_rc.secs > 0 && !g_rc.by_flag ? g_rc.t0 + g_rc.secs * 1000 : 0;
    say("iPhone で録画を始めた（%s・%s・音 %s）", why, g_rc.codec == VR_REC_HEVC ? "HEVC" : "H.264", g_rc.audio ? "口 50063 の PC の音" : "なし");
}

// 受け手から最後のファイルを持ち帰る（1 MB ずつ。持ち帰る間はアプリが止まるので、短い録画か終わるときに使う）
static void rc_fetch(const char *rpath) {
    const char *base = strrchr(rpath, '\\');
    const char *b2 = strrchr(rpath, '/');
    if (!base || (b2 && b2 > base)) base = b2;
    base = base ? base + 1 : rpath;
    char dst[MAX_PATH * 2];
    snprintf(dst, sizeof dst, "%s" PATH_SEP "%s", g_rc.fetch, base);
    FILE *f = fopen(dst, "wb");
    if (!f) { say("録画：%s に書けない", dst); return; }
    uint64_t at = 0, total = 1;
    double t0 = now_ms();
    vr_buf b = { 0 }, out = { 0 };
    while (at < total) {
        vr_u64(&b, at);
        vr_u32(&b, 1u << 20);
        if (rpc_call(VR_RECORD_FETCH, &b, &out) != VK_SUCCESS || out.n < 8) break;
        memcpy(&total, out.p, 8);
        size_t n = out.n - 8;
        if (!n) break;
        fwrite(out.p + 8, 1, n, f);
        at += n;
    }
    free(b.p);
    free(out.p);
    fclose(f);
    double ms = now_ms() - t0;
    if (at == total) say("録画：%s に持ち帰った（%.1f MB・%.0f ms・%.1f MB/秒）", dst, at / 1048576.0, ms, ms > 0 ? at / 1048576.0 / (ms / 1000) : 0);
    else say("録画：持ち帰りが途中で切れた（%llu / %llu バイト）", (unsigned long long) at, (unsigned long long) total);
    if (at == total && !g_rc.keep) {
        rc_status st;
        rc_ctl(VR_REC_DELETE, &st, 0, 0, 0);
    }
}

static void rc_stop(const char *why) {
    if (!g_rc.on) return;
    g_rc.on = 0;
    rc_status st;
    VkResult r = rc_ctl(VR_REC_STOP, &st, 0, 0, 0);
    say("iPhone の録画を止めた（%s）：%s・%u x %u・%llu コマ（落とした %llu）・%.1f 秒・音 %.1f 秒・%.1f MB・%s", why, r == VK_SUCCESS && st.state == 0 ? "書けた" : "失敗",
        st.w, st.h, (unsigned long long) st.frames, (unsigned long long) st.dropped, st.dur_us / 1e6, st.asamples / 48000.0, st.bytes / 1048576.0, st.path);
    if (r == VK_SUCCESS && st.state == 0 && g_rc.fetch[0] && st.path[0]) rc_fetch(st.path);
}

// present ごとに 1 回：始める・止める時か見る
static void rc_tick(struct swapchain *sc) {
    if (!sc || !(g_hw.caps & VR_CAP_RECORD) || g_dead) return;
    rc_settings();
    double t = now_ms();
    if (g_rc.secs > 0 && !g_rc.flag[0] && !g_rc.on && !g_rc.auto_done) {
        g_rc.auto_done = 1;
        rc_start(sc, "IPHONE_VK_RECORD");
    }
    if (g_rc.on && g_rc.stop_at > 0 && t >= g_rc.stop_at) rc_stop("決めた秒数が過ぎた");
    if (g_rc.flag[0] && t >= g_rc.next_check) {
        g_rc.next_check = t + 250;
        int exists = GetFileAttributesA(g_rc.flag) != INVALID_FILE_ATTRIBUTES;
        if (exists && !g_rc.on) {
            g_rc.by_flag = 1;
            rc_start(sc, "印のファイルができた");
        } else if (!exists && g_rc.on && g_rc.by_flag) {
            rc_stop("印のファイルが消えた");
        }
    }
}

// 窓に出すコマを受け手に録ってもらう（絵は送らない。memory の番号と時刻だけ）
static void rc_frame(struct swapchain *sc, uint32_t j, uint64_t serial) {
    if (!g_rc.on || g_dead) return;
    vr_buf b = { 0 };
    vr_u64(&b, ID(sc->rbmem[j]));
    vr_u64(&b, 0);
    vr_u32(&b, sc->w);
    vr_u32(&b, sc->h);
    vr_u32(&b, sc->w * 4);
    vr_u32(&b, sc->fmt == VK_FORMAT_B8G8R8A8_UNORM || sc->fmt == VK_FORMAT_B8G8R8A8_SRGB ? 1 : 0);
    double tp = serial ? sc->pres_t[serial % 64] : now_ms();
    vr_u64(&b, (uint64_t) (int64_t) ((tp - g_rc.t0) * 1000.0));
    rpc_send(VR_RECORD_FRAME, &b);
    free(b.p);
    g_rc.frames++;
}

static void rc_swapchain_made(void) { InterlockedIncrement(&g_rc.live); }
static void rc_swapchain_gone(void) {
    if (InterlockedDecrement(&g_rc.live) <= 0) rc_stop("swapchain を全部片づけた");
}
