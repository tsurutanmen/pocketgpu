// H.264 を FFmpeg の libavcodec でほどく（Linux）。icd.c（窓の絵）と webcam_rx（カメラ）が取り込む。
// libavcodec は動くときに dlopen する。組むときに FFmpeg のヘッダは要らず、無い PC でも動く（呼ぶ側が JPEG などに戻す）。
// 形は Windows の Media Foundation の物（icd.c の struct vdec）と同じ：vdec_open・vdec_push・vdec_map・vdec_unmap・vdec_close。
// ほどけた絵は NV12（Y の面のすぐ後ろに UV の面・1 行の長さは同じ）で返す（vid_nv12_to_bgra にそのまま渡せる）。
//
// 読む libavcodec：IPHONE_VK_AVCODEC=パス があればそれ、無ければ libavcodec.so.63・62・61・60・59・58（FFmpeg 9・8・7・6・5・4）の順。
// 63（FFmpeg 9。Arch は 2026 年 10 月に 9.0）も、下の AVPacket・AVFrame の並びは同じ（n9.0 の packet.h・frame.h で確かめた）
// FFmpeg の版で中の並びが変わる所は使わない：
//   AVCodecContext の中は触らない。設定は avcodec_open2 の AVDictionary で渡す（threads・flags=+low_delay・thread_type）
//   AVPacket は data（24 バイト目）と size（32 バイト目）だけ。FFmpeg 4 から 9 まで同じ
//   AVFrame は data[8]・linesize[8]・width・height・format（0・64・104・108・116 バイト目）だけ。FFmpeg 1 から同じ
//   絵の形は番号でなく名前（av_get_pix_fmt_name）で見る（yuv420p・yuvj420p・nv12）
//   札（何回目の present の物か）は、AVFrame の pts を読まずに、入れた順の待ち行列で付ける（遅れの少ない設定では入れた順に出る）
// ソフトでほどく。GPU（VA-API）でほどく道はまだ無い（AVCodecContext の hw_device_ctx を触る必要があり、版で並びが変わるため）。
// IPHONE_VK_VIDEO_THREADS=N でほどく糸の数（既定 1。コマを重ねる糸は遅れが増えるので、1 コマを分けて受け持つ thread_type=slice にする）
//
// 取り込む側が先に決めておく物：VDEC_SAY(...)（記録に 1 行）・uint8_t など
#ifndef VDEC_AV_INC
#define VDEC_AV_INC
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VDEC_AV_PKT_DATA 24
#define VDEC_AV_PKT_SIZE 32
#define VDEC_AV_FRM_LINESIZE 64
#define VDEC_AV_FRM_WIDTH 104
#define VDEC_AV_FRM_HEIGHT 108
#define VDEC_AV_FRM_FORMAT 116

static struct {
    pthread_once_t once;
    int ok;
    unsigned version; // avcodec_version()
    char path[64];
    const void *(*find_by_name)(const char *);
    void *(*alloc_ctx)(const void *);
    int (*open2)(void *, const void *, void **);
    void (*free_ctx)(void **);
    void *(*pkt_alloc)(void);
    void (*pkt_free)(void **);
    int (*new_packet)(void *, int);
    void (*pkt_unref)(void *);
    int (*send_packet)(void *, const void *);
    int (*receive_frame)(void *, void *);
    void (*flush)(void *);
    void *(*frame_alloc)(void);
    void (*frame_free)(void **);
    void (*frame_unref)(void *);
    int (*dict_set)(void **, const char *, const char *, int);
    void (*dict_free)(void **);
    const char *(*pix_fmt_name)(int);
    unsigned (*codec_version)(void);
    void (*log_set_level)(int);
} g_av = { PTHREAD_ONCE_INIT, 0, 0, "", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static void vdec_av_load(void) {
    static const char *const names[] = { "libavcodec.so.63", "libavcodec.so.62", "libavcodec.so.61", "libavcodec.so.60", "libavcodec.so.59", "libavcodec.so.58" };
    const char *alt = getenv("IPHONE_VK_AVCODEC");
    void *h = NULL;
    if (alt && *alt) {
        h = dlopen(alt, RTLD_NOW | RTLD_LOCAL);
        snprintf(g_av.path, sizeof g_av.path, "%s", alt);
    }
    for (size_t i = 0; !h && !(alt && *alt) && i < sizeof names / sizeof names[0]; i++) {
        h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (h) snprintf(g_av.path, sizeof g_av.path, "%s", names[i]);
    }
    if (!h) return;
    // libavutil の関数（av_frame_alloc など）は、libavcodec が読み込んだ物から引く（dlsym はつながる先も探す）
#define AVS(f, n) (*(void **) &g_av.f = dlsym(h, n))
    int ok = AVS(find_by_name, "avcodec_find_decoder_by_name") && AVS(alloc_ctx, "avcodec_alloc_context3") && AVS(open2, "avcodec_open2") &&
             AVS(free_ctx, "avcodec_free_context") && AVS(pkt_alloc, "av_packet_alloc") && AVS(pkt_free, "av_packet_free") &&
             AVS(new_packet, "av_new_packet") && AVS(pkt_unref, "av_packet_unref") && AVS(send_packet, "avcodec_send_packet") &&
             AVS(receive_frame, "avcodec_receive_frame") && AVS(flush, "avcodec_flush_buffers") && AVS(frame_alloc, "av_frame_alloc") &&
             AVS(frame_free, "av_frame_free") && AVS(frame_unref, "av_frame_unref") && AVS(dict_set, "av_dict_set") && AVS(dict_free, "av_dict_free") &&
             AVS(pix_fmt_name, "av_get_pix_fmt_name") && AVS(codec_version, "avcodec_version");
    AVS(log_set_level, "av_log_set_level");
#undef AVS
    if (!ok) { dlclose(h); return; }
    g_av.version = g_av.codec_version();
    // FFmpeg 4（58）より古い物は、send_packet と av_packet_alloc の並びが違うことがあるので使わない
    if ((g_av.version >> 16) < 58) { dlclose(h); return; }
    g_av.ok = 1;
}

// libavcodec を読めたか（1 回だけ読む）
static int vdec_av_loaded(void) {
    pthread_once(&g_av.once, vdec_av_load);
    return g_av.ok;
}

struct vdec {
    void *ctx, *pkt, *frm;
    int hw;                  // いつも 0（ソフト）
    uint8_t *nv12;           // 最後にほどけた絵（NV12）
    size_t cap;
    uint32_t npitch, nrows;  // nv12 の 1 行のバイトと Y の面の行数
    uint32_t fw, fh;         // 最後にほどけた絵の大きさ
    int has;
    uint64_t tag;            // 最後にほどけた絵の札
    uint64_t tags[64];       // 入れたコマの札（入れた順）
    unsigned th, tt;
    int bad_fmt_noted;
};

static void vdec_close(struct vdec *d) {
    if (g_av.ok) {
        if (d->frm) g_av.frame_free(&d->frm);
        if (d->pkt) g_av.pkt_free(&d->pkt);
        if (d->ctx) g_av.free_ctx(&d->ctx);
    }
    free(d->nv12);
    memset(d, 0, sizeof *d);
}

// hw は今は見ない（ソフトだけ）
static int vdec_open(struct vdec *d, int hw) {
    (void) hw;
    memset(d, 0, sizeof *d);
    if (!vdec_av_loaded()) return 0;
    const void *codec = g_av.find_by_name("h264");
    if (!codec) return 0;
    d->ctx = g_av.alloc_ctx(codec);
    d->pkt = g_av.pkt_alloc();
    d->frm = g_av.frame_alloc();
    if (!d->ctx || !d->pkt || !d->frm) { vdec_close(d); return 0; }
    void *opt = NULL;
    const char *e = getenv("IPHONE_VK_VIDEO_THREADS");
    char th[16];
    snprintf(th, sizeof th, "%d", e && atoi(e) > 0 && atoi(e) <= 16 ? atoi(e) : 1);
    g_av.dict_set(&opt, "threads", th, 0);
    g_av.dict_set(&opt, "thread_type", "slice", 0);
    g_av.dict_set(&opt, "flags", "+low_delay", 0);
    int r = g_av.open2(d->ctx, codec, &opt);
    g_av.dict_free(&opt);
    if (r < 0) { vdec_close(d); return 0; }
    return 1;
}

// ほどけた 1 枚（AVFrame）を nv12 に写す。1 写した・0 使えない形
static int vdec_take(struct vdec *d) {
    const uint8_t *f = (const uint8_t *) d->frm;
    uint8_t *const *data = (uint8_t *const *) f;
    const int *ls = (const int *) (f + VDEC_AV_FRM_LINESIZE);
    int w = *(const int *) (f + VDEC_AV_FRM_WIDTH), h = *(const int *) (f + VDEC_AV_FRM_HEIGHT), fmt = *(const int *) (f + VDEC_AV_FRM_FORMAT);
    const char *name = g_av.pix_fmt_name(fmt);
    int planar = name && (!strcmp(name, "yuv420p") || !strcmp(name, "yuvj420p"));
    int semi = name && !strcmp(name, "nv12");
    if ((!planar && !semi) || w <= 0 || h <= 0 || w > 16384 || h > 16384 || !data[0] || !data[1] || (planar && !data[2])) {
        if (!d->bad_fmt_noted++) VDEC_SAY("H.264 のほどけた絵の形 %s（%d x %d）は使えない", name ? name : "?", w, h);
        return 0;
    }
    uint32_t pitch = ((uint32_t) w + 31) & ~31u, rows = (uint32_t) h;
    size_t need = (size_t) pitch * rows + (size_t) pitch * ((rows + 1) / 2);
    if (d->cap < need) {
        free(d->nv12);
        d->nv12 = (uint8_t *) malloc(need);
        d->cap = d->nv12 ? need : 0;
        if (!d->nv12) return 0;
    }
    uint8_t *y = d->nv12, *uv = d->nv12 + (size_t) pitch * rows;
    for (int r = 0; r < h; r++) memcpy(y + (size_t) r * pitch, data[0] + (ptrdiff_t) r * ls[0], (size_t) w);
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (semi) {
        for (int r = 0; r < ch; r++) memcpy(uv + (size_t) r * pitch, data[1] + (ptrdiff_t) r * ls[1], (size_t) cw * 2);
    } else {
        for (int r = 0; r < ch; r++) {
            const uint8_t *u = data[1] + (ptrdiff_t) r * ls[1], *v = data[2] + (ptrdiff_t) r * ls[2];
            uint8_t *o = uv + (size_t) r * pitch;
            for (int x = 0; x < cw; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
        }
    }
    d->npitch = pitch, d->nrows = rows, d->fw = (uint32_t) w, d->fh = (uint32_t) h, d->has = 1;
    return 1;
}

// 出てきた絵を全部受け取る。1 受け取った・0 無い・-1 失敗
static int vdec_drain(struct vdec *d) {
    int got = 0;
    for (;;) {
        int r = g_av.receive_frame(d->ctx, d->frm);
        if (r == -EAGAIN || r == -(int) 0x20464f45 /* AVERROR_EOF：'EOF ' */) return got;
        if (r < 0) return -1;
        if (vdec_take(d)) {
            got = 1;
            d->tag = d->th != d->tt ? d->tags[d->th++ % 64] : 0;
        } else if (d->th != d->tt) d->th++;
        g_av.frame_unref(d->frm);
    }
}

// 1 コマぶんの NAL の並び（Annex B）を入れて、出てきた絵を受け取る。1 ほどけた絵がある・0 まだ無い・-1 失敗
static int vdec_push(struct vdec *d, const uint8_t *au, size_t n, uint64_t tag) {
    if (!d->ctx || !n || n > (64u << 20)) return -1;
    if (g_av.new_packet(d->pkt, (int) n) < 0) return -1;
    uint8_t *pd = *(uint8_t **) ((uint8_t *) d->pkt + VDEC_AV_PKT_DATA);
    if (!pd || *(int *) ((uint8_t *) d->pkt + VDEC_AV_PKT_SIZE) != (int) n) { g_av.pkt_unref(d->pkt); return -1; }
    memcpy(pd, au, n);
    if (d->tt - d->th >= 64) d->th = d->tt - 63; // 出てこないコマが溜まった（壊れた流れ）。古い札を捨てる
    d->tags[d->tt++ % 64] = tag;
    int got = 0;
    int r = g_av.send_packet(d->ctx, d->pkt);
    if (r == -EAGAIN) {
        // 先に出てきた絵を受け取ってから入れ直す
        int k = vdec_drain(d);
        if (k < 0) { g_av.pkt_unref(d->pkt); return -1; }
        got |= k;
        r = g_av.send_packet(d->ctx, d->pkt);
    }
    g_av.pkt_unref(d->pkt);
    if (r < 0) {
        // 壊れたコマ：中の状態を捨てる（呼ぶ側はキーフレームから始め直す）
        g_av.flush(d->ctx);
        d->th = d->tt;
        return -1;
    }
    int k = vdec_drain(d);
    if (k < 0) return -1;
    return got | k;
}

// ほどけた最後の絵の NV12 を指す（UV の面は Y の面のすぐ後ろ。1 行は同じ長さ）
static int vdec_map(struct vdec *d, const uint8_t **y, uint32_t *pitch, uint32_t *rows) {
    if (!d->has) return 0;
    *y = d->nv12, *pitch = d->npitch, *rows = d->nrows;
    return 1;
}

__attribute__((unused)) static void vdec_unmap(struct vdec *d) { (void) d; }

// この PC で H.264 をほどけるか（libavcodec があり、h264 のデコーダを作れるか）。最初の 1 回だけ試す
__attribute__((unused)) static int vdec_available(void) {
    static int v = -1;
    if (v < 0) {
        struct vdec d;
        int ok = vdec_open(&d, 0);
        vdec_close(&d);
        if (ok) VDEC_SAY("H.264 は %s（FFmpeg の libavcodec %u.%u）でほどく", g_av.path, g_av.version >> 16, (g_av.version >> 8) & 255);
        else VDEC_SAY("この PC には H.264 をほどく libavcodec（FFmpeg）が無いので、窓の絵は JPEG で受け取る");
        v = ok;
    }
    return v > 0;
}
#endif
