// camctl2 の試験用：work/camset/pc/fake_iphone.c（CTRL を知らない古いアプリの真似）の写し。変えたのはポートの決まり（50760〜50780）だけ
// 「iPhone の役」を PC の中でやる試験用のプログラム。本物の iPhone（ios/CameraBridge.swift）と同じ約束（wcam.h）で話す。
// カメラの代わりに、動く色の縞・跳ねる四角・時計・コマの番号（上の 16 個の白黒の四角）を NV12 で作り、
// Media Foundation の H.264 エンコーダ（GPU の物を先に・無ければソフト）で縮めて、コマごとに送る。
//
//   fake_iphone.exe --port 50521 [--sw] [--once] [--app 1920x1080@30] [--app-camera 0] [--app-quality 1] [--prefer-app]
//                   [--app-change-after 8 --app-change 1280x720@60]
// camset で足した：アプリの画面の設定の真似・流している途中の HELLO（頼み直し）・途中の ACK。ポートは 50520〜50529
//
// 本物の iPhone と違う所：時計は PC の QueryPerformanceCounter（受け手と同じ時計なので、遅れを正しく測れる）。
// エンコーダは VideoToolbox でなく Media Foundation。コマは 1 コマずつ縮めて、出てくるまで待ってから送る（server.c の vid_encode と同じ）。
#define COBJMACROS
#include "wcam.h"
#include "video.h"
#include <mmsystem.h>
#include <math.h>
#include <stdarg.h>

static void logf_(const char *fmt, ...);
#define LOG(...) logf_(__VA_ARGS__)
static void logf_(const char *fmt, ...) {
    static double t0;
    if (!t0) t0 = wc_qpc_us();
    fprintf(stderr, "[%7.2f] ", (wc_qpc_us() - t0) / 1e6); // 起動からの秒（試験の記録で順を追うため）
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// ---------------------------------------------------------------- 絵を作る（NV12）
static const uint8_t FONT[12][7] = { // 5x7。0〜9、':'、'.'
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e }, { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e }, { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e }, { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 }, { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e }, { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c }, { 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c } };

// 縞の色（Y・U・V。BT.709 の狭い幅）：白・黄・水色・緑・赤紫・赤・青・黒
static const uint8_t BARS[8][3] = { { 235, 128, 128 }, { 219, 16, 138 }, { 188, 154, 16 }, { 173, 42, 26 }, { 78, 214, 230 }, { 63, 102, 240 }, { 32, 240, 118 }, { 16, 128, 128 } };

typedef struct { uint8_t *y, *uv; uint32_t w, h, ys; } nv12_t;
static int g_noise; // カメラの雑音に似せて、Y に ±g_noise 程度のばらつきを足す（--noise N。エンコーダの仕事を本物に近づける）

static void fill_rect(nv12_t *f, int x0, int y0, int x1, int y1, uint8_t Y, uint8_t U, uint8_t V) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int) f->w) x1 = (int) f->w;
    if (y1 > (int) f->h) y1 = (int) f->h;
    x0 &= ~1, y0 &= ~1, x1 &= ~1, y1 &= ~1; // 色は 2x2 ごと
    for (int y = y0; y < y1; y++) memset(f->y + (size_t) y * f->ys + x0, Y, (size_t) (x1 - x0));
    for (int y = y0 / 2; y < y1 / 2; y++) {
        uint8_t *c = f->uv + (size_t) y * f->ys;
        for (int x = x0; x < x1; x += 2) c[x] = U, c[x + 1] = V;
    }
}

static void draw_text(nv12_t *f, int x, int y, const char *s, int sc) {
    for (; *s; s++, x += 6 * sc) {
        int g = *s >= '0' && *s <= '9' ? *s - '0' : *s == ':' ? 10 : *s == '.' ? 11 : -1;
        if (g < 0) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (FONT[g][r] & (1 << (4 - c))) fill_rect(f, x + c * sc, y + r * sc, x + (c + 1) * sc, y + (r + 1) * sc, 235, 128, 128);
    }
}

// 1 コマを作る。seq：コマの番号（上の 16 個の四角に 2 進で入れる）
static void draw_frame(nv12_t *f, uint32_t seq, double t_s) {
    const int w = (int) f->w, h = (int) f->h;
    // 縞：1 秒に画面の幅の 1/4 だけ流れる。縦は同じなので 1 行作って写す
    const int bw = w / 8 ? w / 8 : 1;
    const int scroll = (int) (t_s * w / 4) % (bw * 8);
    static uint8_t *row_y, *row_uv;
    static int row_w;
    if (row_w != w) {
        free(row_y); free(row_uv);
        row_y = (uint8_t *) malloc((size_t) w * 2 + 8);
        row_uv = (uint8_t *) malloc((size_t) w * 2 + 8);
        row_w = w;
    }
    for (int x = 0; x < w * 2; x += 2) { // 2 画素ごとに色を決める（幅の 2 倍まで作って、ずらして写す）
        int b = ((x + 0) / bw) % 8;
        row_y[x] = row_y[x + 1] = BARS[b][0];
        row_uv[x] = BARS[b][1], row_uv[x + 1] = BARS[b][2];
    }
    const int sh = scroll & ~1;
    for (int y = 0; y < h; y++) memcpy(f->y + (size_t) y * f->ys, row_y + sh, (size_t) w);
    for (int y = 0; y < h / 2; y++) memcpy(f->uv + (size_t) y * f->ys, row_uv + sh, (size_t) w);
    // 跳ねる四角（1 秒で左右に 1 往復・上下に 1.5 往復）
    const int sq = h / 8;
    double px = fabs(fmod(t_s * 1.0, 2.0) - 1.0), py = fabs(fmod(t_s * 1.5, 2.0) - 1.0);
    int qx = (int) (px * (w - sq)), qy = (int) (py * (h - sq));
    fill_rect(f, qx, qy, qx + sq, qy + sq, 235, 128, 128);
    fill_rect(f, qx + sq / 4, qy + sq / 4, qx + sq * 3 / 4, qy + sq * 3 / 4, 16, 128, 128);
    // 上：コマの番号（16 個の四角）
    const int blk = (h / 18) & ~1;
    for (int b = 0; b < 16; b++) {
        int on = (seq >> (15 - b)) & 1;
        fill_rect(f, b * blk, 0, (b + 1) * blk, blk, on ? 235 : 16, 128, 128);
    }
    if (g_noise > 0) {
        // 乱数の表（4 MB）を、コマごとに違う所から読んで足す。1 画素ずつ乱数を作ると 1080p で 20 ms 以上かかり、エンコーダの試験にならない
        static int8_t *tab;
        static uint32_t rng = 2463534242u;
        const size_t TN = 1u << 22;
        if (!tab) {
            tab = (int8_t *) malloc(TN + 8192);
            const uint32_t m = (uint32_t) g_noise * 2 + 1;
            for (size_t i = 0; i < TN + 8192; i++) { rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5; tab[i] = (int8_t) ((int) (rng % m) - g_noise); }
        }
        rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
        size_t off = rng % (TN - (size_t) w * 4);
        for (int y = 0; y < h; y++) {
            uint8_t *r = f->y + (size_t) y * f->ys;
            const int8_t *t = tab + (off + (size_t) y * 37) % (TN - (size_t) w);
            for (int x = 0; x < w; x++) {
                int v = r[x] + t[x];
                r[x] = (uint8_t) (v < 16 ? 16 : v > 235 ? 235 : v);
            }
        }
    }
    // 時計
    SYSTEMTIME st;
    GetLocalTime(&st);
    char tc[32];
    snprintf(tc, sizeof tc, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    const int sc = h / 90 > 0 ? h / 90 : 1;
    fill_rect(f, 0, blk, 12 * 6 * sc + 2 * sc, blk + 9 * sc, 24, 128, 128);
    draw_text(f, sc, blk + sc, tc, sc);
}

// ---------------------------------------------------------------- H.264 エンコーダ（Media Foundation。server.c の mf_* と同じやり方）
typedef struct {
    IMFActivate *act;
    IMFTransform *t;
    ICodecAPI *api;
    IMFMediaEventGenerator *ev;
    int need, have, provides;
    DWORD outsz;
    uint32_t w, h, fps;
    int hw;
    char name[96];
} enc_t;

static void enc_free(enc_t *e) {
    if (!e) return;
    if (e->t) {
        IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (e->ev) IMFMediaEventGenerator_Release(e->ev);
    if (e->api) ICodecAPI_Release(e->api);
    if (e->t) IMFTransform_Release(e->t);
    if (e->act) { IMFActivate_ShutdownObject(e->act); IMFActivate_Release(e->act); }
    free(e);
}

static int enc_pump(enc_t *e, int want) {
    double t0 = wc_qpc_us();
    while (want ? e->have <= 0 : e->need <= 0) {
        IMFMediaEvent *ev = NULL;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(e->ev, MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            double dt = (wc_qpc_us() - t0) / 1000;
            if (dt > 1000) return 0;
            if (dt > 2) Sleep(1);
            else SwitchToThread();
            continue;
        }
        if (FAILED(hr) || !ev) return 0;
        MediaEventType ty = 0;
        IMFMediaEvent_GetType(ev, &ty);
        IMFMediaEvent_Release(ev);
        if (ty == METransformNeedInput) e->need++;
        else if (ty == METransformHaveOutput) e->have++;
    }
    return 1;
}

static int enc_setup(enc_t *e, uint32_t kbps, uint32_t fps, uint32_t gop) {
    IMFAttributes *at = NULL;
    UINT32 async = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(e->t, &at)) && at) {
        IMFAttributes_GetUINT32(at, &VG_ASYNC, &async);
        if (async) IMFAttributes_SetUINT32(at, &VG_ASYNC_UNLOCK, TRUE);
        IMFAttributes_SetUINT32(at, &VG_LOW_LATENCY, TRUE);
        IMFAttributes_Release(at);
    }
    if (async && FAILED(IMFTransform_QueryInterface(e->t, &VG_IID_EVENTGEN, (void **) &e->ev))) return 0;
    if (SUCCEEDED(IMFTransform_QueryInterface(e->t, &VG_IID_CODECAPI, (void **) &e->api)) && e->api) {
        vid_api_bool(e->api, &VG_LOW_LATENCY, 1);
        vid_api_u32(e->api, &VG_RC_MODE, 0); // CBR
        vid_api_u32(e->api, &VG_MEAN_BITRATE, kbps * 1000);
        vid_api_u32(e->api, &VG_GOP, gop);
        vid_api_u32(e->api, &VG_BFRAMES, 0);
        if (!e->hw) vid_api_u32(e->api, &VG_QUALITY_SPEED, 0);
    }
    static const uint32_t prof[] = { 100, 77, 66 };
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 3 && FAILED(hr); i++) {
        IMFMediaType *o = vid_type(&VG_H264, e->w, e->h, fps, kbps * 1000, prof[i]);
        if (!o) return 0;
        hr = IMFTransform_SetOutputType(e->t, 0, o, 0);
        IMFMediaType_Release(o);
    }
    if (FAILED(hr)) return 0;
    IMFMediaType *in = vid_type(&VG_NV12, e->w, e->h, fps, 0, 0);
    if (!in) return 0;
    hr = IMFTransform_SetInputType(e->t, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr)) return 0;
    MFT_OUTPUT_STREAM_INFO si;
    memset(&si, 0, sizeof si);
    if (FAILED(IMFTransform_GetOutputStreamInfo(e->t, 0, &si))) return 0;
    e->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    e->outsz = si.cbSize > e->w * e->h * 3 / 2 ? si.cbSize : e->w * e->h * 3 / 2 + 65536;
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

static enc_t *enc_open(uint32_t w, uint32_t h, uint32_t kbps, uint32_t fps, uint32_t gop, int sw_only) {
    const vid_mf_api *mf = vid_mf();
    if (!mf) return NULL;
    MFT_REGISTER_TYPE_INFO in = { VG_VIDEO, VG_NV12 }, out = { VG_VIDEO, VG_H264 };
    enc_t *got = NULL;
    for (int pass = sw_only ? 1 : 0; pass < 2 && !got; pass++) {
        IMFActivate **act = NULL;
        UINT32 n = 0;
        UINT32 fl = (pass == 0 ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(mf->TEnumEx(VG_CAT_ENCODER, fl, &in, &out, &act, &n))) n = 0;
        for (UINT32 i = 0; i < n; i++) {
            enc_t *e = got ? NULL : (enc_t *) calloc(1, sizeof *e);
            if (e) {
                e->w = w, e->h = h, e->fps = fps, e->hw = pass == 0;
                WCHAR wn[96] = { 0 };
                if (SUCCEEDED(IMFActivate_GetString(act[i], &VG_FRIENDLY_NAME, wn, 96, NULL)))
                    WideCharToMultiByte(CP_UTF8, 0, wn, -1, e->name, sizeof e->name - 1, NULL, NULL);
                if (SUCCEEDED(IMFActivate_ActivateObject(act[i], &VG_IID_TRANSFORM, (void **) &e->t))) {
                    e->act = act[i];
                    IMFActivate_AddRef(act[i]);
                }
                if (e->t && enc_setup(e, kbps, fps, gop)) got = e;
                else {
                    LOG("エンコーダ %s は使えない", e->name[0] ? e->name : "?");
                    enc_free(e);
                }
            }
            IMFActivate_Release(act[i]);
        }
        if (act) CoTaskMemFree(act);
    }
    if (got) LOG("H.264 のエンコーダ：%s（%s）・%u x %u・%u kbps・%u コマ/秒・キーフレームは %u コマごと", got->name, got->hw ? "GPU" : "ソフト", w, h, kbps, fps, gop);
    return got;
}

typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static void buf_put(buf_t *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 65536;
        while (c < b->n + n) c *= 2;
        b->p = (uint8_t *) realloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

// 1 コマ縮める。draw が NV12 の入力の中に絵を作る（カメラが絵を渡すのと同じ位置づけ）。縮めた物が out に入る。出なければ 0
typedef void (*draw_fn)(nv12_t *, void *);
static int enc_encode(enc_t *e, draw_fn draw, void *arg, uint64_t idx, int key, buf_t *out, int *iskey) {
    const DWORD n = e->w * e->h * 3 / 2;
    IMFSample *s = vid_sample(n);
    if (!s) return 0;
    IMFMediaBuffer *b = NULL;
    BYTE *p = NULL;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b)) || FAILED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        if (b) IMFMediaBuffer_Release(b);
        IMFSample_Release(s);
        return 0;
    }
    nv12_t f = { p, p + (size_t) e->w * e->h, e->w, e->h, e->w };
    draw(&f, arg);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, n);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, (LONGLONG) (idx * 10000000ull / e->fps));
    IMFSample_SetSampleDuration(s, (LONGLONG) (10000000ull / e->fps));
    if (key && e->api) vid_api_u32(e->api, &VG_FORCE_KEY, 1);
    HRESULT hr = e->ev && !enc_pump(e, 0) ? E_FAIL : IMFTransform_ProcessInput(e->t, 0, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) return 0;
    if (e->ev) e->need--;
    int got = 0;
    for (;;) {
        if (e->ev && !enc_pump(e, 1)) break;
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!e->provides && !(ob.pSample = vid_sample(e->outsz))) break;
        DWORD st = 0;
        hr = IMFTransform_ProcessOutput(e->t, 0, 1, &ob, &st);
        if (e->ev) e->have--;
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType *t = NULL;
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (SUCCEEDED(IMFTransform_GetOutputAvailableType(e->t, 0, 0, &t)) && t) {
                IMFTransform_SetOutputType(e->t, 0, t, 0);
                IMFMediaType_Release(t);
            }
            continue;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            return 0;
        }
        IMFMediaBuffer *mb = NULL;
        BYTE *q = NULL;
        DWORD len = 0;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(ob.pSample, &mb)) && SUCCEEDED(IMFMediaBuffer_Lock(mb, &q, NULL, &len))) {
            buf_put(out, q, len);
            IMFMediaBuffer_Unlock(mb);
        }
        if (mb) IMFMediaBuffer_Release(mb);
        UINT32 clean = 0;
        if (SUCCEEDED(IMFSample_GetUINT32(ob.pSample, &VG_CLEAN_POINT, &clean)) && clean) *iskey = 1;
        IMFSample_Release(ob.pSample);
        got = 1;
        if (e->ev) break;
    }
    return got;
}

// ---------------------------------------------------------------- 1 人の相手
// camset：アプリの画面の設定（--app-*）と、PC の頼みの混ぜ方を、本物のアプリ（CameraBridge.swift の resolve）と同じ決まりで真似る。
//   PC の値が 0 の項目はアプリの設定。PC が値を決めた項目は、--prefer-app が無ければ PC を使う
//   流している途中の HELLO（頼み直し）を受けたら、エンコーダを作り直して ACK を返し、キーフレームから流す
//   --app-change-after N --app-change 1920x1080@60：N 秒後に「アプリの画面で設定を変えた」ことにする（PC が WC_HELLO_LIVE を立てていれば、その場で切り替える）
static struct { uint32_t w, h, fps, cam, quality; int prefer_app; double change_after; uint32_t cw, ch, cfps; } g_app = { 1280, 720, 30, 0, 1, 0, 0, 0, 0, 0 };

typedef struct {
    SOCKET s;
    CRITICAL_SECTION send_lk; // 送るのは 2 本の糸（コマとPONG）。頭と中身の間に別の物が挟まらないように
    volatile LONG want_key, new_kbps, stop, new_hello;
    wc_hello hello;           // new_hello のときの中身（send_lk で守る）
} client_t;

static int send_locked(client_t *c, uint32_t type, const void *body, uint32_t len) {
    EnterCriticalSection(&c->send_lk);
    int ok = wc_send_msg(c->s, type, body, len);
    LeaveCriticalSection(&c->send_lk);
    return ok;
}

static void log_hello(const wc_hello *h, uint32_t len) {
    LOG("HELLO を受けた：w=%u h=%u fps=%u kbps=%u camera=%u flags=0x%x quality=%u（%u バイト）", h->width, h->height, h->fps, h->kbps, h->camera, h->flags, len >= 40 ? h->quality : 0, len);
}

static DWORD WINAPI ctl_thread(LPVOID p) {
    client_t *c = (client_t *) p;
    for (;;) {
        uint32_t type, len;
        uint8_t *body = NULL;
        if (!wc_recv_msg(c->s, &type, &body, &len, 1 << 16)) break;
        if (type == WC_REQ_KEY) InterlockedExchange(&c->want_key, 1);
        else if (type == WC_SET_KBPS && len >= 4) { uint32_t k; memcpy(&k, body, 4); InterlockedExchange(&c->new_kbps, (LONG) k); }
        else if (type == WC_HELLO && len >= 32) {
            wc_hello h;
            memset(&h, 0, sizeof h);
            memcpy(&h, body, len < sizeof h ? len : sizeof h);
            log_hello(&h, len);
            EnterCriticalSection(&c->send_lk);
            c->hello = h;
            LeaveCriticalSection(&c->send_lk);
            InterlockedExchange(&c->new_hello, 1);
        } else if (type == WC_PING && len >= 8) {
            uint64_t v[2];
            memcpy(&v[0], body, 8);
            v[1] = wc_now_us();
            send_locked(c, WC_PONG, v, 16);
        }
        free(body);
    }
    InterlockedExchange(&c->stop, 1);
    return 0;
}

typedef struct { uint32_t seq; double t0_us; double t_ready_us; } drawarg_t;
static void draw_cb(nv12_t *f, void *a) {
    drawarg_t *d = (drawarg_t *) a;
    draw_frame(f, d->seq, (wc_qpc_us() - d->t0_us) / 1e6);
    d->t_ready_us = wc_qpc_us(); // 絵ができた時刻。本物のカメラでは「カメラが絵を渡した時刻」にあたる
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *) a, y = *(const double *) b; return x < y ? -1 : x > y; }

// 画質の段とビットレート（CameraBridge.swift の qualityKbps と同じ表）
static uint32_t quality_kbps(uint32_t q, uint32_t w, uint32_t h, uint32_t fps) {
    const int k4 = w * h >= 3840 * 2160, k1080 = w * h >= 1920 * 1080, hi = fps >= 50;
    static const uint32_t T[3][3][2] = { // [段][720・1080・4K][30・60]
        { { 4000, 6000 }, { 8000, 12000 }, { 20000, 30000 } },
        { { 6000, 9000 }, { 12000, 18000 }, { 30000, 45000 } },
        { { 12000, 18000 }, { 24000, 36000 }, { 50000, 60000 } } };
    uint32_t qi = q >= 1 && q <= 3 ? q - 1 : 0;
    return T[qi][k4 ? 2 : k1080 ? 1 : 0][hi];
}

// HELLO とアプリの設定から、撮る形を決める
static void resolve(const wc_hello *hi, uint32_t *w, uint32_t *h, uint32_t *fps, uint32_t *cam, uint32_t *kbps, uint32_t *flags) {
    const int pc_size = hi->width && hi->height, pc_fps = hi->fps != 0, pc_cam = hi->camera != 0 || (hi->flags & WC_HELLO_CAMERA_SET);
    const int pc_q = hi->kbps != 0 || hi->quality != 0;
    *flags = WC_ACK_LIVE;
    int app_used = !pc_size || !pc_fps || !pc_cam || !pc_q;
    if (g_app.prefer_app && (pc_size || pc_fps || pc_cam || pc_q)) *flags |= WC_ACK_APP_PRIORITY;
    *w = pc_size && !g_app.prefer_app ? hi->width : g_app.w;
    *h = pc_size && !g_app.prefer_app ? hi->height : g_app.h;
    *fps = pc_fps && !g_app.prefer_app ? hi->fps : g_app.fps;
    *cam = pc_cam && !g_app.prefer_app ? hi->camera : g_app.cam;
    uint32_t q = hi->quality && !g_app.prefer_app ? hi->quality : g_app.quality;
    *kbps = hi->kbps && !g_app.prefer_app ? hi->kbps : quality_kbps(q, *w, *h, *fps);
    if (app_used || g_app.prefer_app) *flags |= WC_ACK_APP_SETTINGS;
    *w &= ~1u, *h &= ~1u;
}

static void serve_client(SOCKET s, int sw_only) {
    client_t c;
    memset(&c, 0, sizeof c);
    c.s = s;
    InitializeCriticalSection(&c.send_lk);
    int nd = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &nd, sizeof nd);
    uint32_t type, len;
    uint8_t *body = NULL;
    DWORD to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    if (!wc_recv_msg(s, &type, &body, &len, 1 << 12) || type != WC_HELLO || len < 32) { LOG("あいさつが来ない"); free(body); DeleteCriticalSection(&c.send_lk); return; }
    to = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    wc_hello hi;
    memset(&hi, 0, sizeof hi);
    memcpy(&hi, body, len < sizeof hi ? len : sizeof hi);
    free(body);
    log_hello(&hi, len);
    HANDLE ct = NULL;
    const double t0 = wc_qpc_us();
    drawarg_t da = { 0, t0, 0 };
    buf_t out = { 0 };
    double *encms = (double *) malloc(sizeof(double) * 200000);
    size_t nenc = 0, nsent = 0, nkey = 0, bytes = 0, skipped = 0;
    enc_t *enc = NULL;
    int live = (hi.flags & WC_HELLO_LIVE) != 0, changed_by_app = 0, first = 1;
    uint32_t w = 0, h = 0, fps = 30;
    uint64_t k = 0;
    for (;;) {
        // ---- 形を決めてエンコーダを作る（最初・頼み直し・アプリの設定が変わった）
        uint32_t cam, kbps, fl;
        resolve(&hi, &w, &h, &fps, &cam, &kbps, &fl);
        if (!first) fl |= WC_ACK_CHANGED;
        uint32_t gop = hi.gop ? hi.gop : fps * 2;
        wc_ack ack;
        memset(&ack, 0, sizeof ack);
        ack.version = WCAM_VERSION;
        if (hi.version != WCAM_VERSION || w < 64 || h < 64 || w > 4096 || h > 2304 || fps < 1 || fps > 120) ack.status = 5;
        if (hi.camera > 3) ack.status = 2;
        enc_free(enc);
        enc = ack.status ? NULL : enc_open(w, h, kbps, fps, gop, sw_only);
        if (!ack.status && !enc) ack.status = 4;
        ack.width = w, ack.height = h, ack.fps = fps, ack.kbps = kbps, ack.camera = cam, ack.flags = fl;
        send_locked(&c, WC_HELLO_ACK, &ack, sizeof ack);
        if (ack.status) { LOG("頼みを断った（status %d）", ack.status); Sleep(200); break; }
        LOG("%s：%u x %u・%u コマ/秒・%u kbps・カメラ %u・ACK flags 0x%x", first ? "始める" : "切り替えた", w, h, fps, kbps, cam, fl);
        first = 0;
        if (!ct) ct = CreateThread(NULL, 0, ctl_thread, &c, 0, NULL);
        const double period = 1e6 / fps;
        double next = wc_qpc_us();
        int restart = 0;
        for (int fk = 0; !c.stop && !restart; k++, fk++) {
            // コマの時刻まで待つ（遅れていれば待たない。1 コマ以上遅れたら、飛ばして今に合わせる）
            for (;;) {
                double now = wc_qpc_us();
                if (now >= next) break;
                if (next - now > 2000) Sleep(1);
                else SwitchToThread();
            }
            double now = wc_qpc_us();
            while (now - next > period) { next += period; skipped++; }
            next += period;
            // 頼み直し（PC から）・アプリの設定の変更（--app-change-after）
            if (InterlockedExchange(&c.new_hello, 0)) {
                EnterCriticalSection(&c.send_lk);
                hi = c.hello;
                LeaveCriticalSection(&c.send_lk);
                live = (hi.flags & WC_HELLO_LIVE) != 0;
                restart = 1;
                break;
            }
            if (g_app.change_after > 0 && !changed_by_app && (now - t0) / 1e6 >= g_app.change_after) {
                changed_by_app = 1;
                g_app.w = g_app.cw, g_app.h = g_app.ch, g_app.fps = g_app.cfps;
                if (live) { LOG("アプリの画面で設定が変わった：%u x %u・%u。その場で切り替える", g_app.w, g_app.h, g_app.fps); restart = 1; break; }
                LOG("アプリの画面で設定が変わった：PC が途中の切り替えに対応しないので、次の接続まで待つ");
            }
            da.seq = (uint32_t) k;
            LONG nk = InterlockedExchange(&c.new_kbps, 0);
            if (nk > 0 && enc->api) vid_api_u32(enc->api, &VG_MEAN_BITRATE, (uint32_t) nk * 1000);
            int wk = InterlockedExchange(&c.want_key, 0);
            out.n = 0;
            wc_frame_head fh;
            memset(&fh, 0, sizeof fh);
            uint32_t hdr0[2] = { 0, 0 };
            buf_put(&out, hdr0, 8);          // メッセージの頭の分
            buf_put(&out, &fh, sizeof fh);   // コマの頭の分
            int key = 0;
            if (!enc_encode(enc, draw_cb, &da, (uint64_t) fk, wk || fk == 0, &out, &key)) { LOG("縮められない"); c.stop = 1; break; }
            double te1 = wc_qpc_us();
            if (out.n <= 8 + sizeof fh) continue;
            fh.capture_us = (uint64_t) da.t_ready_us, fh.sent_us = wc_now_us(), fh.seq = (uint32_t) k, fh.flags = key ? WC_FRAME_KEY : 0, fh.width = w, fh.height = h;
            uint32_t hdr[2] = { WC_FRAME, (uint32_t) (out.n - 8) };
            memcpy(out.p, hdr, 8);
            memcpy(out.p + 8, &fh, sizeof fh);
            EnterCriticalSection(&c.send_lk);
            int okk = wc_send_all(s, out.p, out.n); // 頭と中身を 1 回で
            LeaveCriticalSection(&c.send_lk);
            if (!okk) { c.stop = 1; break; }
            if (nenc < 200000) encms[nenc++] = (te1 - da.t_ready_us) / 1000; // エンコードだけの時間（絵を作る時間は除く）
            nsent++, bytes += out.n, nkey += key ? 1 : 0;
        }
        if (!restart) break;
    }
    double secs = (wc_qpc_us() - t0) / 1e6;
    shutdown(s, SD_BOTH);
    if (ct) { WaitForSingleObject(ct, 2000); CloseHandle(ct); }
    qsort(encms, nenc, sizeof(double), cmp_d);
    double sum = 0;
    for (size_t i = 0; i < nenc; i++) sum += encms[i];
    LOG("終わり：%.1f 秒・送ったコマ %zu（%.1f コマ/秒）・キーフレーム %zu・飛ばした %zu・%.0f kbps・エンコード 平均 %.2f ms / 95%% %.2f ms / 最大 %.2f ms",
        secs, nsent, secs > 0 ? nsent / secs : 0, nkey, skipped, secs > 0 ? bytes * 8.0 / 1000 / secs : 0, nenc ? sum / nenc : 0, nenc ? encms[(size_t) (nenc * 0.95)] : 0, nenc ? encms[nenc - 1] : 0);
    free(encms);
    free(out.p);
    enc_free(enc);
    DeleteCriticalSection(&c.send_lk);
}

static int parse_mode(const char *v, uint32_t *w, uint32_t *h, uint32_t *fps) { return sscanf(v, "%ux%u@%u", w, h, fps) == 3; }

int main(int argc, char **argv) {
    int port = 50521, sw = 0, once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sw")) sw = 1;
        else if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--noise") && i + 1 < argc) g_noise = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) { if (!parse_mode(argv[++i], &g_app.w, &g_app.h, &g_app.fps)) return 2; }
        else if (!strcmp(argv[i], "--app-camera") && i + 1 < argc) g_app.cam = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app-quality") && i + 1 < argc) g_app.quality = (uint32_t) atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prefer-app")) g_app.prefer_app = 1;
        else if (!strcmp(argv[i], "--app-change-after") && i + 1 < argc) g_app.change_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--app-change") && i + 1 < argc) { if (!parse_mode(argv[++i], &g_app.cw, &g_app.ch, &g_app.cfps)) return 2; }
    }
    if (port < 50760 || port > 50780) { fprintf(stderr, "ポートは 50760〜50780 だけ\n"); return 2; } // camctl2：ポートの決まりだけ変えた
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET, a.sin_port = htons((u_short) port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 1) != 0) { fprintf(stderr, "127.0.0.1:%d で待てない\n", port); return 1; }
    // この PC では Media Foundation のエンコーダを最初に作るのに 1 分近くかかることがあった。待つ前に 1 回作っておき、頼まれてからは速く作れるようにする
    // （待ち受けは先に始めるので、PC がつないで HELLO を送っても、ここが終わるまで溜まっているだけ）
    LOG("エンコーダの下ごしらえ…");
    enc_free(enc_open(g_app.w, g_app.h, 4000, g_app.fps, g_app.fps * 2, sw));
    LOG("iPhone の役：127.0.0.1:%d で待っている（pid %lu）。アプリの設定 %u x %u・%u コマ/秒・カメラ %u・画質 %u%s", port, GetCurrentProcessId(), g_app.w, g_app.h, g_app.fps, g_app.cam,
        g_app.quality, g_app.prefer_app ? "・アプリの設定を優先" : "");
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        LOG("PC がつながった");
        serve_client(c, sw);
        closesocket(c);
        if (once) break;
    }
    closesocket(ls);
    return 0;
}
