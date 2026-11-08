// webcam_rx（Linux 版）：iPhone のカメラ（H.264）を USB で受けて、ほどいて、仮想のカメラとして出す。Windows 版 webcam_rx.c と同じ約束（wcam_proto.h）。
// ほどくのは FFmpeg の libavcodec（vk/icd/vdec_av.inc。dlopen）。出す先（--out）：
//   auto      v4l2loopback の機器があればそれ（Zoom・Discord・ブラウザ・OBS がふつうのカメラとして選べる）、無ければ PipeWire
//   pipewire  PipeWire の Video/Source（仮想のカメラ。media.role=Camera。OBS の PipeWire のカメラ・ポータル経由のブラウザが選べる）
//   v4l2      v4l2loopback の機器（--v4l2 /dev/videoN。無ければ自分で探す）。作るには root と v4l2loopback（dkms）が要る
//   y4m:パス  YUV4MPEG2 のファイルに書く（試験用）
//   none      ほどくまで（速さを測る）
// どの出し方でも、大きさは最初に決まった形のまま。iPhone の形が途中で変わったら、縦横の比を保って縮め、余りは黒で埋める
//
//   webcam_rx --usb [--device UDID] [--reconnect] [--out auto|pipewire|v4l2|none|y4m:パス] [--v4l2 /dev/videoN] [--name 名前]
//   webcam_rx --addr 127.0.0.1:50064       TCP（試験の偽の iPhone）
//   webcam_rx --list                        usbmuxd が見ている iPhone の一覧
//   webcam_rx --check                       この PC で使える物：AVCODEC OK <ファイル> <版>|NG・V4L2 OK <機器> <名前>|NONE・PIPEWIRE OK <版>|NG|OLD <版>
//   webcam_rx --test-pattern [--w 1280 --h 720 --fps 30] --out ...   iPhone を使わずに、動く縞と番号の絵を出す（仮想のカメラだけを確かめる）
//   そのほか  --w --h --fps --kbps --gop --camera auto|back|front|ultrawide|tele --quality auto|standard|high|max --mirror --code N
//             --secs N（N 秒で終わる）--no-decode（ほどかずに数えるだけ）--dump-h264 パス（受けた H.264 をそのまま書く）
//             --snap パス.ppm [--snap-at 90]（そのコマを PPM に書く）--mux 口（usbmuxd の口。IPHONE_VK_MUX と同じ書き方）
// 記録（stderr）の行は Windows 版と同じ言い回し（start-mic-camera.sh が読む）：「決まった形：…」「iPhone を待っています」「iPhone との接続が切れた」「iPhone が断った（status N）」
#include "lin_common.h"
#include "wcam_proto.h"
#include "pw_lite.h"
#include <linux/videodev2.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

static double g_t0;
static void say_(const char *fmt, ...) {
    if (!g_t0) g_t0 = lc_now_us();
    fprintf(stderr, "[%7.2f] ", (lc_now_us() - g_t0) / 1e6);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
#define LOG(...) say_(__VA_ARGS__)
#define VDEC_SAY(...) say_(__VA_ARGS__)
#include "vdec_av.inc"
#include "video.h"

static struct {
    const char *addr, *device, *out, *v4l2, *name, *dump, *snap;
    int usb, list, secs, reconnect, nodec, pattern, snap_at;
    wc_hello hello;
} g_opt;
static volatile sig_atomic_t g_done;
static void on_sig(int s) { (void) s; g_done = 1; }

// ---------------------------------------------------------------- 出す先
enum { OUT_NONE, OUT_PW, OUT_V4L2, OUT_Y4M };
static struct {
    int kind, ready;
    uint32_t w, h, fps;          // 出す大きさ（最初の形）
    pthread_mutex_t lk;
    uint8_t *nv12;               // 最新の 1 枚（出す大きさの NV12・詰めた物）
    int has;
    uint64_t made, sent, dropped;
    // PipeWire
    void *tl, *stream;
    struct pwl_stream_events ev;
    volatile int state;
    uint32_t fmt, stride, size;  // 決まった形（PWL_VIDEO_FORMAT_*）
    // v4l2loopback
    int fd;
    uint8_t *buf;
    size_t bufsz;
    // y4m
    FILE *y4m;
} V = { .lk = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

// NV12（詰めた・w x h）を、形 fmt の 1 枚（1 行 stride バイト）にする
static void nv12_to(uint32_t fmt, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t stride) {
    const uint8_t *Y = src, *UV = src + (size_t) w * h;
    if (fmt == PWL_VIDEO_FORMAT_YUY2) {
        for (uint32_t r = 0; r < h; r++) {
            const uint8_t *y = Y + (size_t) r * w, *uv = UV + (size_t) (r / 2) * w;
            uint8_t *o = dst + (size_t) r * stride;
            for (uint32_t x = 0; x + 1 < w; x += 2) o[2 * x] = y[x], o[2 * x + 1] = uv[x], o[2 * x + 2] = y[x + 1], o[2 * x + 3] = uv[x + 1];
        }
    } else if (fmt == PWL_VIDEO_FORMAT_I420) {
        uint8_t *u = dst + (size_t) stride * h, *v = u + (size_t) (stride / 2) * (h / 2);
        for (uint32_t r = 0; r < h; r++) memcpy(dst + (size_t) r * stride, Y + (size_t) r * w, w);
        for (uint32_t r = 0; r < h / 2; r++)
            for (uint32_t x = 0; x < w / 2; x++) u[(size_t) r * (stride / 2) + x] = UV[(size_t) r * w + 2 * x], v[(size_t) r * (stride / 2) + x] = UV[(size_t) r * w + 2 * x + 1];
    } else { // NV12
        for (uint32_t r = 0; r < h; r++) memcpy(dst + (size_t) r * stride, Y + (size_t) r * w, w);
        for (uint32_t r = 0; r < h / 2; r++) memcpy(dst + (size_t) stride * h + (size_t) r * stride, UV + (size_t) r * w, w);
    }
}
static size_t fmt_size(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *stride) {
    *stride = fmt == PWL_VIDEO_FORMAT_YUY2 ? w * 2 : w;
    return fmt == PWL_VIDEO_FORMAT_YUY2 ? (size_t) w * h * 2 : (size_t) w * h * 3 / 2;
}

// ---- PipeWire ----
static void pw_on_state(void *d, int old, int st, const char *err) {
    (void) d; (void) old;
    V.state = st;
    static const char *const names[] = { "エラー", "つないでいない", "つないでいる", "止まっている（使うアプリを待っている）", "流している" };
    LOG("PipeWire のカメラ：%s%s%s", st >= -1 && st <= 3 ? names[st + 1] : "?", err ? "・" : "", err ? err : "");
}
static void pw_on_param(void *d, uint32_t id, const void *param) {
    (void) d;
    if (id != PWL_PARAM_Format || !param) return;
    uint32_t f = 0, sz[2] = { 0, 0 };
    if (!pod_find(param, PWL_FORMAT_VIDEO_format, &f, 1) || !pod_find(param, PWL_FORMAT_VIDEO_size, sz, 2)) { LOG("PipeWire のカメラ：決まった形を読めない"); return; }
    if (sz[0] != V.w || sz[1] != V.h || (f != PWL_VIDEO_FORMAT_YUY2 && f != PWL_VIDEO_FORMAT_NV12 && f != PWL_VIDEO_FORMAT_I420)) {
        LOG("PipeWire のカメラ：思っていない形（形 %u・%u x %u）", f, sz[0], sz[1]);
        return;
    }
    V.fmt = f;
    V.size = (uint32_t) fmt_size(f, V.w, V.h, &V.stride);
    LOG("PipeWire のカメラ：使うアプリが %s・%u x %u を選んだ", f == PWL_VIDEO_FORMAT_YUY2 ? "YUY2" : f == PWL_VIDEO_FORMAT_NV12 ? "NV12" : "I420", V.w, V.h);
    pwl_pod p = { .n = 0 };
    uint32_t at = pod_obj_begin(&p, PWL_TYPE_OBJECT_ParamBuffers, PWL_PARAM_Buffers);
    uint32_t nb[3] = { 4, 2, 8 }, dt = (1u << PWL_DATA_MemPtr) | (1u << PWL_DATA_MemFd);
    pod_prop_choice(&p, PWL_PARAM_BUFFERS_buffers, PWL_CHOICE_Range, PWL_TYPE_Int, nb, 3);
    pod_prop_int(&p, PWL_PARAM_BUFFERS_blocks, 1);
    pod_prop_int(&p, PWL_PARAM_BUFFERS_size, (int32_t) V.size);
    pod_prop_int(&p, PWL_PARAM_BUFFERS_stride, (int32_t) V.stride);
    pod_prop_choice(&p, PWL_PARAM_BUFFERS_dataType, PWL_CHOICE_Flags, PWL_TYPE_Int, &dt, 1);
    pod_obj_end(&p, at);
    const void *params[1] = { p.b };
    if (!p.bad) g_pw.update_params(V.stream, params, 1);
}
static void pw_on_process(void *d) {
    (void) d;
    struct pwl_buffer *b = g_pw.dequeue(V.stream);
    if (!b) return;
    struct pwl_data *dd = b->buffer && b->buffer->n_datas ? &b->buffer->datas[0] : NULL;
    if (dd && dd->data && dd->chunk && V.fmt && dd->maxsize >= V.size) {
        pthread_mutex_lock(&V.lk);
        if (V.has) nv12_to(V.fmt, V.nv12, V.w, V.h, (uint8_t *) dd->data, V.stride);
        else if (V.fmt == PWL_VIDEO_FORMAT_YUY2) for (uint32_t i = 0; i + 1 < V.size; i += 2) ((uint8_t *) dd->data)[i] = 16, ((uint8_t *) dd->data)[i + 1] = 128; // 黒
        else memset(dd->data, 16, (size_t) V.stride * V.h), memset((uint8_t *) dd->data + (size_t) V.stride * V.h, 128, V.size - (size_t) V.stride * V.h);
        V.sent++;
        pthread_mutex_unlock(&V.lk);
        dd->chunk->offset = 0, dd->chunk->size = V.size, dd->chunk->stride = (int32_t) V.stride, dd->chunk->flags = 0;
    } else if (dd && dd->chunk) dd->chunk->size = 0;
    g_pw.queue(V.stream, b);
}
static int pw_open(void) {
    if (!pwl_load()) { LOG("PipeWire（libpipewire-0.3.so.0）が無い"); return 0; }
    if (!g_pw.trigger || !pwl_at_least(0, 3, 34)) { LOG("PipeWire %d.%d.%d は古い（0.3.34 から）", g_pw.ver[0], g_pw.ver[1], g_pw.ver[2]); return 0; }
    V.tl = g_pw.tl_new("pocketgpu-camera", NULL);
    if (!V.tl) return 0;
    const char *name = g_opt.name ? g_opt.name : "PocketGPU iPhone Camera";
    void *props = g_pw.props_new("media.class", "Video/Source", "media.role", "Camera", "media.type", "Video", "media.category", "Capture",
                                 "node.name", "pocketgpu_iphone_camera", "node.description", name, "node.nick", name, "media.name", name, NULL);
    V.ev.version = 2;
    V.ev.state_changed = pw_on_state;
    V.ev.param_changed = pw_on_param;
    V.ev.process = pw_on_process;
    V.stream = g_pw.stream_new_simple(g_pw.tl_loop(V.tl), "pocketgpu-camera", props, &V.ev, NULL);
    if (!V.stream) { g_pw.tl_destroy(V.tl); V.tl = NULL; return 0; }
    pwl_pod p = { .n = 0 };
    uint32_t at = pod_obj_begin(&p, PWL_TYPE_OBJECT_Format, PWL_PARAM_EnumFormat);
    pod_prop_id(&p, PWL_FORMAT_mediaType, PWL_MEDIA_TYPE_video);
    pod_prop_id(&p, PWL_FORMAT_mediaSubtype, PWL_MEDIA_SUBTYPE_raw);
    uint32_t fm[4] = { PWL_VIDEO_FORMAT_YUY2, PWL_VIDEO_FORMAT_YUY2, PWL_VIDEO_FORMAT_NV12, PWL_VIDEO_FORMAT_I420 };
    pod_prop_choice(&p, PWL_FORMAT_VIDEO_format, PWL_CHOICE_Enum, PWL_TYPE_Id, fm, 4);
    pod_prop_rect(&p, PWL_FORMAT_VIDEO_size, V.w, V.h);
    pod_prop_frac(&p, PWL_FORMAT_VIDEO_framerate, V.fps, 1);
    pod_obj_end(&p, at);
    const void *params[1] = { p.b };
    if (p.bad || g_pw.stream_connect(V.stream, PWL_DIRECTION_OUTPUT, PWL_ID_ANY, PWL_STREAM_FLAG_DRIVER | PWL_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        LOG("PipeWire のカメラをつなげない");
        g_pw.stream_destroy(V.stream);
        g_pw.tl_destroy(V.tl);
        V.stream = V.tl = NULL;
        return 0;
    }
    if (g_pw.tl_start(V.tl) < 0) return 0;
    LOG("PipeWire（%d.%d.%d）に仮想のカメラ「%s」（%u x %u・%u コマ/秒）を作った", g_pw.ver[0], g_pw.ver[1], g_pw.ver[2], name, V.w, V.h, V.fps);
    return 1;
}
static void pw_close(void) {
    if (V.tl) g_pw.tl_stop(V.tl);
    if (V.stream) g_pw.stream_destroy(V.stream);
    if (V.tl) g_pw.tl_destroy(V.tl);
    V.stream = V.tl = NULL;
}

// ---- v4l2loopback ----
static int v4l2_is_loopback(const char *dev, char *card, size_t cn) {
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return 0;
    struct v4l2_capability c;
    memset(&c, 0, sizeof c);
    int ok = ioctl(fd, VIDIOC_QUERYCAP, &c) == 0 && !strcmp((const char *) c.driver, "v4l2 loopback");
    if (ok && card) snprintf(card, cn, "%s", (const char *) c.card);
    close(fd);
    return ok;
}
// v4l2loopback の機器を探す。名前に PocketGPU を含む物を先に。見つからなければ 0
static int v4l2_find(char *out, size_t n) {
    DIR *d = opendir("/dev");
    if (!d) return 0;
    struct dirent *e;
    char best[64] = "", any[64] = "";
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "video", 5)) continue;
        char path[300], card[64];
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if (!v4l2_is_loopback(path, card, sizeof card)) continue;
        if (strstr(card, "PocketGPU") && !best[0]) snprintf(best, sizeof best, "%s", path);
        if (!any[0]) snprintf(any, sizeof any, "%s", path);
    }
    closedir(d);
    if (!best[0] && !any[0]) return 0;
    snprintf(out, n, "%s", best[0] ? best : any);
    return 1;
}
static int v4l2_open(const char *dev) {
    V.fd = open(dev, O_RDWR | O_CLOEXEC);
    if (V.fd < 0) { LOG("%s を開けない（%s）", dev, strerror(errno)); return 0; }
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = V.w, f.fmt.pix.height = V.h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = V.w * 2;
    f.fmt.pix.sizeimage = V.w * V.h * 2;
    f.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
    if (ioctl(V.fd, VIDIOC_S_FMT, &f) != 0) {
        LOG("%s に形（YUYV・%u x %u）を決められない（%s）。ほかのアプリがカメラを開いていれば閉じる", dev, V.w, V.h, strerror(errno));
        close(V.fd);
        V.fd = -1;
        return 0;
    }
    V.bufsz = (size_t) V.w * V.h * 2;
    V.buf = (uint8_t *) malloc(V.bufsz);
    LOG("v4l2loopback の %s に出す（YUYV・%u x %u）", dev, V.w, V.h);
    return V.buf != NULL;
}

// 出す先を開く（最初の形が決まったときに 1 回）
static int vout_open(uint32_t w, uint32_t h, uint32_t fps) {
    V.w = w & ~1u, V.h = h & ~1u, V.fps = fps ? fps : 30;
    V.nv12 = (uint8_t *) calloc((size_t) V.w * V.h * 3 / 2, 1);
    if (!V.nv12) return 0;
    const char *o = g_opt.out ? g_opt.out : "auto";
    char dev[300] = "";
    if (!strcmp(o, "none")) V.kind = OUT_NONE;
    else if (!strncmp(o, "y4m:", 4)) {
        V.y4m = fopen(o + 4, "wb");
        if (!V.y4m) { LOG("%s を開けない", o + 4); return 0; }
        fprintf(V.y4m, "YUV4MPEG2 W%u H%u F%u:1 Ip A1:1 C420jpeg\n", V.w, V.h, V.fps);
        V.kind = OUT_Y4M;
    } else if (!strcmp(o, "v4l2") || (!strcmp(o, "auto") && (g_opt.v4l2 || v4l2_find(dev, sizeof dev)))) {
        if (g_opt.v4l2) snprintf(dev, sizeof dev, "%s", g_opt.v4l2);
        else if (!dev[0] && !v4l2_find(dev, sizeof dev)) { LOG("v4l2loopback の機器が無い（sudo modprobe v4l2loopback で作る）"); return 0; }
        if (!v4l2_open(dev)) return 0;
        V.kind = OUT_V4L2;
    } else if (!strcmp(o, "pipewire") || !strcmp(o, "auto")) {
        if (!pw_open()) return 0;
        V.kind = OUT_PW;
    } else { LOG("--out が分からない：%s", o); return 0; }
    V.ready = 1;
    return 1;
}

// ほどけた NV12（W x H・1 行 pitch・UV は y + pitch * rows）を、出す大きさに入れる（比を保って縮め、余りは黒）
static void fit_nv12(const uint8_t *y, uint32_t pitch, uint32_t rows, uint32_t W, uint32_t H, uint8_t *dst, uint32_t w, uint32_t h) {
    const uint8_t *uv = y + (size_t) pitch * rows;
    if (W == w && H == h) {
        for (uint32_t r = 0; r < h; r++) memcpy(dst + (size_t) r * w, y + (size_t) r * pitch, w);
        for (uint32_t r = 0; r < h / 2; r++) memcpy(dst + (size_t) w * h + (size_t) r * w, uv + (size_t) r * pitch, w);
        return;
    }
    double s = fmin((double) w / W, (double) h / H);
    uint32_t sw = ((uint32_t) (W * s)) & ~1u, sh = ((uint32_t) (H * s)) & ~1u;
    if (!sw || !sh) return;
    uint32_t ox = ((w - sw) / 2) & ~1u, oy = ((h - sh) / 2) & ~1u;
    memset(dst, 16, (size_t) w * h);
    memset(dst + (size_t) w * h, 128, (size_t) w * h / 2);
    for (uint32_t r = 0; r < sh; r++) {
        const uint8_t *s0 = y + (size_t) ((uint64_t) r * H / sh) * pitch;
        uint8_t *o = dst + (size_t) (oy + r) * w + ox;
        for (uint32_t x = 0; x < sw; x++) o[x] = s0[(uint64_t) x * W / sw];
    }
    for (uint32_t r = 0; r < sh / 2; r++) {
        const uint8_t *s0 = uv + (size_t) ((uint64_t) r * (H / 2) / (sh / 2)) * pitch;
        uint8_t *o = dst + (size_t) w * h + (size_t) (oy / 2 + r) * w + ox;
        for (uint32_t x = 0; x < sw / 2; x++) {
            uint32_t sx = (uint32_t) ((uint64_t) x * (W / 2) / (sw / 2)) * 2;
            o[2 * x] = s0[sx], o[2 * x + 1] = s0[sx + 1];
        }
    }
}

// 1 コマ出す
static void vout_frame(const uint8_t *y, uint32_t pitch, uint32_t rows, uint32_t W, uint32_t H) {
    if (!V.ready) return;
    pthread_mutex_lock(&V.lk);
    fit_nv12(y, pitch, rows, W, H, V.nv12, V.w, V.h);
    V.has = 1;
    V.made++;
    pthread_mutex_unlock(&V.lk);
    if (V.kind == OUT_PW) {
        if (V.state == PWL_STREAM_STATE_STREAMING) {
            g_pw.tl_lock(V.tl);
            g_pw.trigger(V.stream);
            g_pw.tl_unlock(V.tl);
        }
    } else if (V.kind == OUT_V4L2) {
        nv12_to(PWL_VIDEO_FORMAT_YUY2, V.nv12, V.w, V.h, V.buf, V.w * 2);
        if (write(V.fd, V.buf, V.bufsz) != (ssize_t) V.bufsz) V.dropped++;
        else V.sent++;
    } else if (V.kind == OUT_Y4M) {
        size_t n = (size_t) V.w * V.h * 3 / 2;
        if (V.bufsz < n) { free(V.buf); V.buf = (uint8_t *) malloc(n); V.bufsz = V.buf ? n : 0; }
        if (V.buf) {
            nv12_to(PWL_VIDEO_FORMAT_I420, V.nv12, V.w, V.h, V.buf, V.w);
            fputs("FRAME\n", V.y4m);
            fwrite(V.buf, 1, n, V.y4m);
            V.sent++;
        }
    }
}

static void vout_close(void) {
    if (V.kind == OUT_PW) pw_close();
    if (V.fd >= 0) close(V.fd);
    if (V.y4m) fclose(V.y4m);
    if (V.ready && V.kind != OUT_NONE) LOG("出した：%llu コマ（作った %llu・出せなかった %llu）", (unsigned long long) V.sent, (unsigned long long) V.made, (unsigned long long) V.dropped);
    free(V.nv12);
    free(V.buf);
    V.nv12 = V.buf = NULL;
    V.fd = -1, V.y4m = NULL, V.ready = 0;
}

// ---------------------------------------------------------------- 試験の絵（--test-pattern）
// 動く色の縞と、上の 16 個の白黒の四角（コマの番号・Windows の偽の iPhone と同じ読み方）
static void make_pattern(uint8_t *f, uint32_t w, uint32_t h, uint32_t k) {
    uint8_t *Y = f, *UV = f + (size_t) w * h;
    for (uint32_t r = 0; r < h; r++)
        for (uint32_t x = 0; x < w; x++) Y[(size_t) r * w + x] = (uint8_t) (16 + ((x + k * 4) % w) * 219 / w);
    for (uint32_t r = 0; r < h / 2; r++)
        for (uint32_t x = 0; x < w / 2; x++) UV[(size_t) r * w + 2 * x] = (uint8_t) (64 + (r * 128 / (h / 2))), UV[(size_t) r * w + 2 * x + 1] = (uint8_t) (192 - (x * 128 / (w / 2)));
    uint32_t blk = (h / 18) & ~1u;
    if (blk < 4 || blk * 16 > w) return;
    for (int b = 0; b < 16; b++) {
        uint8_t v = ((k & 0xffff) >> (15 - b)) & 1 ? 235 : 16;
        for (uint32_t r = 0; r < blk; r++) memset(Y + (size_t) r * w + (size_t) b * blk, v, blk);
    }
}
static int run_pattern(void) {
    uint32_t w = g_opt.hello.width ? g_opt.hello.width : 1280, h = g_opt.hello.height ? g_opt.hello.height : 720, fps = g_opt.hello.fps ? g_opt.hello.fps : 30;
    w &= ~1u, h &= ~1u;
    if (!vout_open(w, h, fps)) return 1;
    uint8_t *f = (uint8_t *) malloc((size_t) w * h * 3 / 2);
    double t0 = lc_now_us();
    LOG("試験の絵を出す：%u x %u・%u コマ/秒", w, h, fps);
    for (uint32_t k = 0; !g_done; k++) {
        make_pattern(f, w, h, k);
        vout_frame(f, w, h, w, h);
        double due = t0 + (k + 1) * 1e6 / fps, now = lc_now_us();
        if (due > now) lc_sleep_ms((int) ((due - now) / 1000));
        if (g_opt.secs && (lc_now_us() - t0) / 1e6 >= g_opt.secs) break;
    }
    free(f);
    vout_close();
    return 0;
}

// ---------------------------------------------------------------- iPhone から受ける
static int g_sock = -1;
static uint64_t g_frames, g_dec, g_fail, g_bytes, g_sessions, g_gaps, g_checked, g_badctr;

static int send_msg(int s, uint32_t type, const void *body, uint32_t len) {
    uint8_t *b = (uint8_t *) malloc(8 + (size_t) len);
    if (!b) return 0;
    memcpy(b, &type, 4), memcpy(b + 4, &len, 4);
    if (len) memcpy(b + 8, body, len);
    int ok = lc_send_all(s, b, 8 + (size_t) len);
    free(b);
    return ok;
}
static int recv_msg(int s, uint32_t *type, uint8_t **body, uint32_t *len, uint32_t max) {
    uint32_t h[2];
    if (!lc_recv_all(s, h, 8) || h[1] > max) return 0;
    uint8_t *b = (uint8_t *) malloc(h[1] ? h[1] : 1);
    if (!b) return 0;
    if (h[1] && !lc_recv_all(s, b, h[1])) { free(b); return 0; }
    *type = h[0], *body = b, *len = h[1];
    return 1;
}
static const char *cam_name(uint32_t c) { return c == 1 ? "前面" : c == 2 ? "超広角" : c == 3 ? "望遠" : "背面"; }
static int read_counter(const uint8_t *y, uint32_t ys, uint32_t w, uint32_t h) {
    const int blk = (int) (h / 18) & ~1;
    if (blk < 4 || (uint32_t) (blk * 16) > w) return -1;
    int v = 0;
    for (int b = 0; b < 16; b++) v = (v << 1) | (y[(size_t) (blk / 2) * ys + b * blk + blk / 2] > 126);
    return v;
}
static void write_ppm(const char *path, const uint8_t *y, uint32_t pitch, uint32_t rows, uint32_t w, uint32_t h) {
    uint8_t *bgra = (uint8_t *) malloc((size_t) w * h * 4);
    FILE *f = bgra ? fopen(path, "wb") : NULL;
    if (f) {
        vid_nv12_to_bgra(y, pitch, y + (size_t) pitch * rows, pitch, w, h, bgra);
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < (size_t) w * h; i++) { uint8_t rgb[3] = { bgra[4 * i + 2], bgra[4 * i + 1], bgra[4 * i] }; fwrite(rgb, 1, 3, f); }
        fclose(f);
        LOG("1 コマを書いた：%s（%u x %u）", path, w, h);
    }
    free(bgra);
}

static int connect_once(int quiet) {
    if (g_opt.usb) return lc_usb_connect((int) WCAM_PORT, g_opt.device, quiet);
    char host[256], port[16];
    lc_split_hostport(g_opt.addr, host, sizeof host, port, sizeof port, "50064");
    return lc_tcp_connect(host, port);
}

// 1 回つないで、切れるまで受ける。1：一度は流れた・0：つなげない／断られた・-1：やめる
static int session(FILE *dump) {
    struct vdec dec;
    memset(&dec, 0, sizeof dec);
    if (!g_opt.nodec && !vdec_open(&dec, 0)) {
        LOG("H.264 をほどけない（FFmpeg の libavcodec が無い）。Ubuntu・Debian は sudo apt install libavcodec-extra・Fedora は sudo dnf install libavcodec-free・Arch は sudo pacman -S ffmpeg（FFmpeg 4〜8 のどれでもよい）");
        return -1;
    }
    int s = -1;
    double t_note = 0;
    for (int tries = 0; s < 0 && !g_done; tries++) {
        s = connect_once(tries > 0);
        if (s >= 0) break;
        if (!g_opt.reconnect && tries >= 19) break;
        if (g_opt.reconnect && lc_now_us() - t_note > 30e6) { LOG("iPhone を待っています（アプリで「ウェブカメラとして使う」をオンにして、前面に出してください）"); t_note = lc_now_us(); }
        for (int i = 0; i < (g_opt.reconnect ? 10 : 5) && !g_done; i++) lc_sleep_ms(100);
    }
    if (s < 0) { if (!g_done) LOG("つなげない"); vdec_close(&dec); return 0; }
    int big = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    lc_set_timeout(s, 30000);
    wc_hello h = g_opt.hello;
    h.version = WCAM_VERSION;
    h.flags |= WC_HELLO_LIVE;
    send_msg(s, WC_HELLO, &h, sizeof h);
    uint32_t type, len;
    uint8_t *body = NULL;
    wc_ack ack;
    if (!recv_msg(s, &type, &body, &len, 1 << 12) || type != WC_HELLO_ACK || len < sizeof ack) { LOG("iPhone の返事が来ない"); free(body); close(s); vdec_close(&dec); return 0; }
    memcpy(&ack, body, sizeof ack);
    free(body);
    if (ack.status) { LOG("iPhone が断った（status %d）", ack.status); close(s); vdec_close(&dec); return 0; }
    lc_set_timeout(s, 0);
    LOG("決まった形：%u x %u・%u コマ/秒・%u kbps・カメラ %u（%s）%s", ack.width, ack.height, ack.fps, ack.kbps, ack.camera, cam_name(ack.camera),
        ack.flags & WC_ACK_LIVE ? "" : "・途中の切り替えに対応しない版のアプリ");
    if (!V.ready && !g_opt.nodec && !vout_open(ack.width, ack.height, ack.fps)) { close(s); vdec_close(&dec); return -1; }
    g_sock = s;
    g_sessions++;
    uint32_t aw = ack.width, ah = ack.height, last_seq = 0;
    int have_last = 0, wait_key = 1;
    double next_ping = 0, last_keyreq = 0, t0 = lc_now_us();
    while (!g_done) {
        struct pollfd pf = { s, POLLIN, 0 };
        int r = poll(&pf, 1, 200);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            if (!recv_msg(s, &type, &body, &len, 16u << 20)) break;
            if (type == WC_HELLO_ACK && len >= sizeof ack) {
                memcpy(&ack, body, sizeof ack);
                if (!ack.status) {
                    aw = ack.width, ah = ack.height;
                    LOG("形が変わった：%u x %u・%u コマ/秒・%u kbps・カメラ %u（%s）", ack.width, ack.height, ack.fps, ack.kbps, ack.camera, cam_name(ack.camera));
                    wait_key = 1;
                } else LOG("iPhone が頼み直しを断った（status %d）", ack.status);
            } else if (type == WC_EVENT && len >= sizeof(wc_event)) {
                wc_event ev;
                memcpy(&ev, body, sizeof ev);
                LOG("iPhone の知らせ：%u（%u）", ev.code, ev.arg);
            } else if (type == WC_FRAME && len > sizeof(wc_frame_head)) {
                wc_frame_head fh;
                memcpy(&fh, body, sizeof fh);
                g_frames++;
                g_bytes += len;
                if (have_last && fh.seq > last_seq + 1) g_gaps += fh.seq - last_seq - 1;
                last_seq = fh.seq, have_last = 1;
                if (dump) fwrite(body + sizeof fh, 1, len - sizeof fh, dump);
                if (wait_key && !(fh.flags & WC_FRAME_KEY)) { free(body); continue; }
                wait_key = 0;
                if (!g_opt.nodec) {
                    int k = vdec_push(&dec, body + sizeof fh, len - sizeof fh, fh.seq);
                    if (k < 0) {
                        g_fail++;
                        wait_key = 1;
                        if (lc_now_us() - last_keyreq > 1e6) { send_msg(s, WC_REQ_KEY, NULL, 0); last_keyreq = lc_now_us(); }
                    } else if (k > 0) {
                        const uint8_t *y;
                        uint32_t pitch, rows;
                        if (vdec_map(&dec, &y, &pitch, &rows)) {
                            uint32_t W = fh.width ? fh.width : aw, H = fh.height ? fh.height : ah;
                            if (W > dec.fw) W = dec.fw;
                            if (H > dec.fh) H = dec.fh;
                            W &= ~1u, H &= ~1u;
                            g_dec++;
                            int ctr = read_counter(y, pitch, W, H);
                            g_checked++;
                            if (ctr >= 0 && (uint32_t) ctr != ((uint32_t) dec.tag & 0xffff)) g_badctr++;
                            if (g_opt.snap && (int) g_dec == g_opt.snap_at) write_ppm(g_opt.snap, y, pitch, rows, W, H);
                            vout_frame(y, pitch, rows, W, H);
                        }
                    }
                }
            }
            free(body);
        }
        double now = lc_now_us();
        if (now >= next_ping) { uint64_t t = (uint64_t) now; send_msg(s, WC_PING, &t, 8); next_ping = now + 1e6; }
        if (g_opt.secs && (now - t0) / 1e6 >= g_opt.secs) { g_done = 1; break; }
    }
    g_sock = -1;
    close(s);
    vdec_close(&dec);
    if (!g_done) LOG("iPhone との接続が切れた");
    return 1;
}

static int run_check(void) {
    if (vdec_av_loaded()) printf("AVCODEC OK %s %u.%u\n", g_av.path, g_av.version >> 16, (g_av.version >> 8) & 255);
    else printf("AVCODEC NG\n");
    char dev[300], card[64] = "";
    if (v4l2_find(dev, sizeof dev)) { v4l2_is_loopback(dev, card, sizeof card); printf("V4L2 OK %s %s\n", dev, card); }
    else printf("V4L2 NONE\n");
    if (!pwl_load()) printf("PIPEWIRE NG\n");
    else if (!g_pw.trigger || !pwl_at_least(0, 3, 34)) printf("PIPEWIRE OLD %d.%d.%d\n", g_pw.ver[0], g_pw.ver[1], g_pw.ver[2]);
    else printf("PIPEWIRE OK %d.%d.%d\n", g_pw.ver[0], g_pw.ver[1], g_pw.ver[2]);
    return 0;
}

static int parse_camera(const char *v, wc_hello *h) {
    struct { const char *n; uint32_t c; int set; } t[] = { { "auto", 0, 0 }, { "back", 0, 1 }, { "wide", 0, 1 }, { "front", 1, 1 }, { "ultrawide", 2, 1 }, { "tele", 3, 1 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcasecmp(v, t[i].n)) { h->camera = t[i].c; if (t[i].set) h->flags |= WC_HELLO_CAMERA_SET; else h->flags &= ~WC_HELLO_CAMERA_SET; return 1; }
    if (*v >= '0' && *v <= '3' && !v[1]) { h->camera = (uint32_t) (*v - '0'); return 1; }
    return 0;
}
static int parse_quality(const char *v, uint32_t *q) {
    const char *n[] = { "auto", "standard", "high", "max" };
    for (uint32_t i = 0; i < 4; i++) if (!strcasecmp(v, n[i])) { *q = i; return 1; }
    if (*v >= '0' && *v <= '3' && !v[1]) { *q = (uint32_t) (*v - '0'); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    g_opt.snap_at = 90;
    g_opt.hello.version = WCAM_VERSION;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--usb")) g_opt.usb = 1;
        else if (!strcmp(a, "--mux") && more) g_lc_mux = argv[++i];
        else if (!strcmp(a, "--device") && more) g_opt.device = argv[++i];
        else if (!strcmp(a, "--list")) g_opt.list = 1;
        else if (!strcmp(a, "--addr") && more) g_opt.addr = argv[++i];
        else if (!strcmp(a, "--w") && more) g_opt.hello.width = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--h") && more) g_opt.hello.height = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--fps") && more) g_opt.hello.fps = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--kbps") && more) g_opt.hello.kbps = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--gop") && more) g_opt.hello.gop = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--camera") && more) { if (!parse_camera(argv[++i], &g_opt.hello)) { fprintf(stderr, "--camera は auto・back・front・ultrawide・tele\n"); return 2; } }
        else if (!strcmp(a, "--quality") && more) { if (!parse_quality(argv[++i], &g_opt.hello.quality)) { fprintf(stderr, "--quality は auto・standard・high・max\n"); return 2; } }
        else if (!strcmp(a, "--mirror")) g_opt.hello.flags |= WC_HELLO_MIRROR;
        else if (!strcmp(a, "--code") && more) g_opt.hello.code = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(a, "--secs") && more) g_opt.secs = atoi(argv[++i]);
        else if (!strcmp(a, "--reconnect")) g_opt.reconnect = 1;
        else if (!strcmp(a, "--out") && more) g_opt.out = argv[++i];
        else if (!strcmp(a, "--v4l2") && more) g_opt.v4l2 = argv[++i];
        else if (!strcmp(a, "--name") && more) g_opt.name = argv[++i];
        else if (!strcmp(a, "--no-decode")) g_opt.nodec = 1;
        else if (!strcmp(a, "--dump-h264") && more) g_opt.dump = argv[++i];
        else if (!strcmp(a, "--snap") && more) g_opt.snap = argv[++i];
        else if (!strcmp(a, "--snap-at") && more) g_opt.snap_at = atoi(argv[++i]);
        else if (!strcmp(a, "--test-pattern")) g_opt.pattern = 1;
        else if (!strcmp(a, "--check")) return run_check();
        else if (!strcmp(a, "--nowin") || !strcmp(a, "--shm")) {} // Windows 版の引数（start-mic-camera.sh の書き方をそろえるため）。Linux では窓を出さない
        else { fprintf(stderr, "知らない引数：%s\n", a); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    if (g_opt.list) return lc_list_devices();
    if (g_opt.pattern) return run_pattern();
    if (!g_opt.usb && !g_opt.addr) {
        fprintf(stderr, "使い方：webcam_rx --usb [--device UDID] [--reconnect] [--out auto|pipewire|v4l2|none|y4m:パス] か --addr host:port か --test-pattern か --list\n");
        return 2;
    }
    if (!g_opt.nodec && !vdec_av_loaded()) {
        LOG("H.264 をほどけない（FFmpeg の libavcodec が無い）。Ubuntu・Debian は sudo apt install libavcodec-extra・Fedora は sudo dnf install libavcodec-free・Arch は sudo pacman -S ffmpeg（FFmpeg 4〜8 のどれでもよい）");
        return 3;
    }
    if (g_av.ok && g_av.log_set_level) g_av.log_set_level(8); // FFmpeg の記録は致命的な物だけ（壊れたコマの細かい文を出さない）
    if (g_av.ok) LOG("ほどく：%s（FFmpeg の libavcodec %u.%u・ソフト）", g_av.path, g_av.version >> 16, (g_av.version >> 8) & 255);
    FILE *dump = g_opt.dump ? fopen(g_opt.dump, "wb") : NULL;
    double t0 = lc_now_us();
    int rc = 0;
    while (!g_done) {
        int r = session(dump);
        if (r < 0) { rc = 1; break; }
        if (!g_opt.reconnect || g_done) { if (r == 0) rc = 1; break; }
        for (int i = 0; i < 10 && !g_done; i++) lc_sleep_ms(100);
    }
    if (dump) fclose(dump);
    double secs = (lc_now_us() - t0) / 1e6;
    LOG("---- 結果 ----");
    LOG("つないだ回数 %llu・受けたコマ %llu・ほどいたコマ %llu（%.1f コマ/秒）・ほどけなかった %llu・番号の飛び %llu・受けたデータ %.2f MB",
        (unsigned long long) g_sessions, (unsigned long long) g_frames, (unsigned long long) g_dec, secs > 0 ? g_dec / secs : 0.0,
        (unsigned long long) g_fail, (unsigned long long) g_gaps, g_bytes / 1e6);
    if (g_checked) LOG("絵の中身の確かめ（上の 16 個の四角が送った番号と同じか）：%llu コマ中 違い %llu", (unsigned long long) g_checked, (unsigned long long) g_badctr);
    vout_close();
    return rc;
}
