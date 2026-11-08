// 試験の道具（64 ビット版と 32 ビット版を組む）。アプリと同じやり方で DirectShow のカメラの一覧を読み、絵を取る。
//   dscam_tool list                 DirectShow の Video Capture Sources の一覧（フィルタは作らない。名前を読むだけ）
//   dscam_tool mf                   Media Foundation のカメラの一覧（Windows のカメラアプリなどが見る方。カメラは開かない）
//   dscam_tool grab "名前" [--fmt NV12|YUY2|I420|RGB32|RGB24] [--size 1280x720] [--fps 30] [--secs 5] [--png f.png]
//                                   その名前のフィルタだけを作り、SampleGrabber と NullRenderer につないで取る
// grab の数：受けたコマ・本物の絵（共有メモリから来た）と「待っています」の絵の数・上の 16 個の四角（偽の iPhone のコマ番号）の種類・遅れ
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <strmif.h>
#include <control.h>
#include <amvideo.h>
#include <mfapi.h>
#include <mfidl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wcam_shm.h"

#define G(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } };
G(kIID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
G(kCLSID_SystemDeviceEnum, 0x62BE5D10, 0x60EB, 0x11d0, 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86)
G(kCLSID_VideoInputDeviceCategory, 0x860BB310, 0x5D01, 0x11d0, 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86)
G(kIID_ICreateDevEnum, 0x29840822, 0x5b84, 0x11d0, 0xbd, 0x3b, 0x00, 0xa0, 0xc9, 0x11, 0xce, 0x86)
G(kIID_IPropertyBag, 0x55272a00, 0x42cb, 0x11ce, 0x81, 0x35, 0x00, 0xaa, 0x00, 0x4b, 0xb8, 0x51)
G(kIID_IBaseFilter, 0x56a86895, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kCLSID_FilterGraph, 0xe436ebb3, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IGraphBuilder, 0x56a868a9, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IMediaControl, 0x56a868b1, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IAMStreamConfig, 0xc6e13340, 0x30ac, 0x11d0, 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56)
G(kIID_IKsPropertySet, 0x31efac30, 0x515c, 0x11d0, 0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93)
G(kAMPROPSETID_Pin, 0x9b00f101, 0x1567, 0x11d1, 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5)
G(kPIN_CATEGORY_CAPTURE, 0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba)
G(kCLSID_SampleGrabber, 0xc1f400a0, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37)
G(kCLSID_NullRenderer, 0xc1f400a4, 0x3f08, 0x11d3, 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37)
G(kIID_ISampleGrabber, 0x6b652fff, 0x11fe, 0x4fce, 0x92, 0xad, 0x02, 0x66, 0xb5, 0xd7, 0xc7, 0x8f)
G(kIID_ISampleGrabberCB, 0x0579154a, 0x2b53, 0x4994, 0xb0, 0xd0, 0xe7, 0x73, 0x14, 0x8e, 0xff, 0x85)
G(kMEDIATYPE_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_YUY2, 0x32595559, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_I420, 0x30323449, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_RGB32, 0xe436eb7e, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kSUB_RGB24, 0xe436eb7d, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kMF_SRC_TYPE, 0xc60ac5fe, 0x252a, 0x478f, 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3)
G(kMF_SRC_VIDCAP, 0x8ac3587a, 0x4ae7, 0x42d8, 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f)
G(kMF_FRIENDLY, 0x60d0e559, 0x52f8, 0x4fa2, 0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01)
G(kMF_SYMLINK, 0x58f0aad8, 0x22bf, 0x4f8a, 0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f)
static bool eq(const GUID &a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

struct ISGCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double t, IMediaSample *s) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double t, BYTE *b, LONG n) = 0;
};
struct ISG : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(LONG *, LONG *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISGCB *, LONG) = 0;
};

static void u8(const wchar_t *w, char *out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

static int read_prop(IPropertyBag *bag, const wchar_t *name, char *out, int n) {
    VARIANT v;
    VariantInit(&v);
    out[0] = 0;
    if (FAILED(bag->Read(name, &v, NULL))) return 0;
    if (v.vt == VT_BSTR) u8(v.bstrVal, out, n);
    VariantClear(&v);
    return 1;
}

// ---------------------------------------------------------------- 一覧
static int cmd_list(const char *want, IBaseFilter **found) {
    ICreateDevEnum *de = NULL;
    IEnumMoniker *em = NULL;
    HRESULT hr = CoCreateInstance(kCLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, kIID_ICreateDevEnum, (void **) &de);
    if (FAILED(hr)) { printf("System Device Enumerator を作れない 0x%08lx\n", (unsigned long) hr); return -1; }
    hr = de->CreateClassEnumerator(kCLSID_VideoInputDeviceCategory, &em, 0);
    int n = 0, hit = 0;
    if (hr == S_OK && em) {
        IMoniker *m = NULL;
        while (em->Next(1, &m, NULL) == S_OK) {
            IPropertyBag *bag = NULL;
            char name[256], clsid[64], devpath[512], disp[600] = "";
            LPOLESTR dn = NULL;
            if (SUCCEEDED(m->GetDisplayName(NULL, NULL, &dn)) && dn) { u8(dn, disp, sizeof disp); CoTaskMemFree(dn); }
            if (SUCCEEDED(m->BindToStorage(NULL, NULL, kIID_IPropertyBag, (void **) &bag))) {
                read_prop(bag, L"FriendlyName", name, sizeof name);
                read_prop(bag, L"CLSID", clsid, sizeof clsid);
                int hasdp = read_prop(bag, L"DevicePath", devpath, sizeof devpath);
                if (!want) printf("  %d. %s  CLSID=%s%s\n     %s\n", ++n, name, clsid[0] ? clsid : "-", hasdp ? "  DevicePath あり" : "", disp);
                else if (!strcmp(name, want) && found && !*found) {
                    hr = m->BindToObject(NULL, NULL, kIID_IBaseFilter, (void **) found);
                    printf("「%s」のフィルタを作った：0x%08lx\n", name, (unsigned long) hr);
                    hit = 1;
                }
                bag->Release();
            }
            m->Release();
        }
        em->Release();
    } else printf("  （Video Capture Sources は空：0x%08lx）\n", (unsigned long) hr);
    de->Release();
    return want ? hit : n;
}

static int cmd_mf(void) {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFAttributes *a = NULL;
    MFCreateAttributes(&a, 1);
    a->SetGUID(kMF_SRC_TYPE, kMF_SRC_VIDCAP);
    IMFActivate **acts = NULL;
    UINT32 n = 0;
    HRESULT hr = MFEnumDeviceSources(a, &acts, &n);
    printf("Media Foundation のカメラ：%u 個（0x%08lx）\n", n, (unsigned long) hr);
    for (UINT32 i = 0; i < n; i++) {
        WCHAR *nm = NULL, *sl = NULL;
        UINT32 len;
        char b1[256] = "", b2[512] = "";
        if (SUCCEEDED(acts[i]->GetAllocatedString(kMF_FRIENDLY, &nm, &len))) { u8(nm, b1, sizeof b1); CoTaskMemFree(nm); }
        if (SUCCEEDED(acts[i]->GetAllocatedString(kMF_SYMLINK, &sl, &len))) { u8(sl, b2, sizeof b2); CoTaskMemFree(sl); }
        printf("  %u. %s\n     %s\n", i + 1, b1, b2);
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    a->Release();
    MFShutdown();
    return (int) n;
}

// ---------------------------------------------------------------- PNG（圧縮なし）
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
static int write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    size_t rowb = 1 + (size_t) w * 3, raw = rowb * h;
    uint8_t *img = (uint8_t *) malloc(raw);
    for (uint32_t y = 0; y < h; y++) { img[y * rowb] = 0; memcpy(img + y * rowb + 1, rgb + (size_t) y * w * 3, (size_t) w * 3); }
    size_t nblk = (raw + 65534) / 65535, at = 0;
    uint8_t *z = (uint8_t *) malloc(2 + raw + nblk * 5 + 4);
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
    if (!f) return 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w), be32(ihdr + 4, h);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t) at);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(img);
    free(z);
    return 1;
}

// ---------------------------------------------------------------- 取った絵を読む
enum Kind { K_NV12, K_YUY2, K_I420, K_RGB32, K_RGB24 };
static inline uint8_t clip8(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }
static void yuv_rgb(int Y, int U, int V, uint8_t *o) {
    const int C = (Y - 16) * 298 + 128, D = U - 128, E = V - 128;
    o[0] = clip8((C + 409 * E) >> 8), o[1] = clip8((C - 100 * D - 208 * E) >> 8), o[2] = clip8((C + 516 * D) >> 8);
}
// 1 画素の (Y, RGB) を返す
static int pix(Kind k, const uint8_t *b, int w, int h, int x, int y, uint8_t *rgb) {
    int Y, U, V;
    switch (k) {
    case K_NV12: Y = b[(size_t) y * w + x]; U = b[(size_t) w * h + (size_t) (y / 2) * w + (x & ~1)]; V = b[(size_t) w * h + (size_t) (y / 2) * w + (x & ~1) + 1]; break;
    case K_I420: Y = b[(size_t) y * w + x]; U = b[(size_t) w * h + (size_t) (y / 2) * (w / 2) + x / 2]; V = b[(size_t) w * h * 5 / 4 + (size_t) (y / 2) * (w / 2) + x / 2]; break;
    case K_YUY2: { const uint8_t *p = b + (size_t) y * w * 2 + (size_t) (x & ~1) * 2; Y = p[(x & 1) ? 2 : 0]; U = p[1]; V = p[3]; break; }
    default: {
        const int bpp = k == K_RGB32 ? 4 : 3, st = ((w * bpp) + 3) & ~3;
        const uint8_t *p = b + (size_t) (h - 1 - y) * st + (size_t) x * bpp;
        rgb[0] = p[2], rgb[1] = p[1], rgb[2] = p[0];
        return ((66 * p[2] + 129 * p[1] + 25 * p[0] + 128) >> 8) + 16;
    }
    }
    yuv_rgb(Y, U, V, rgb);
    return Y;
}
// 偽の iPhone の上の 16 個の四角（コマ番号）。webcam_rx の read_counter と同じ
static int read_counter(Kind k, const uint8_t *b, int w, int h) {
    const int blk = (h / 18) & ~1;
    if (blk < 4 || blk * 16 > w) return -1;
    int v = 0;
    uint8_t rgb[3];
    for (int i = 0; i < 16; i++) v = (v << 1) | (pix(k, b, w, h, i * blk + blk / 2, blk / 2, rgb) > 126);
    return v;
}

// ---------------------------------------------------------------- grab
struct Grab : public ISGCB {
    CRITICAL_SECTION lk;
    Kind kind;
    int w, h, stamp;
    long n = 0, n_bytes_bad = 0;
    double t_first = 0, t_last = 0;
    uint8_t *last = NULL;
    long last_len = 0;
    int counters[4096];
    int nctr = 0, distinct = 0, last_ctr = -2, ctr_none = 0;
    int per_sec[600] = {}; // 1 秒ごとの「新しい番号のコマ」の数（0 なら待ちの絵か止まった絵）
    double lat[20000];
    int nlat = 0;
    // cam4k：本物の絵（共有メモリから来た。media time が撮った時刻）の数と、新しい絵（撮った時刻が前と違う）の数・1 秒ごと。コールバックの間隔
    int want_copy = 0;
    long n_real = 0, n_uniq = 0;
    LONGLONG last_m0 = -1;
    int uniq_sec[600] = {};
    double gap[40000];
    int ngap = 0;
    double t_prev = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (eq(riid, kIID_IUnknown) || eq(riid, kIID_ISampleGrabberCB)) { *ppv = this; return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double t, IMediaSample *s) override {
        const double now = (double) wcam_shm_now_us();
        BYTE *p = NULL;
        s->GetPointer(&p);
        long len = s->GetActualDataLength();
        LONGLONG m0 = 0, m1 = 0;
        int has_mt = s->GetMediaTime(&m0, &m1) == S_OK;
        EnterCriticalSection(&lk);
        if (!n) t_first = now;
        else if (ngap < 40000) gap[ngap++] = (now - t_prev) / 1000.0;
        t_prev = now;
        t_last = now, n++;
        if (want_copy) { // PNG を書くときだけ写す（4K の RGB32 は 1 コマ 33 MB。毎コマ写すと試験の道具が重くなって測りが濁る）
            if (!last) last = (uint8_t *) malloc((size_t) w * h * 4 + 64);
            if (len > (long) w * h * 4) n_bytes_bad++;
            else if (n % 8 == 0) { memcpy(last, p, (size_t) len); last_len = len; }
        }
        int c = read_counter(kind, p, w, h);
        if (c < 0) ctr_none++;
        else if (c != last_ctr) { distinct++; last_ctr = c; int sec = (int) ((now - t_first) / 1e6); if (sec >= 0 && sec < 600 && c != 0) per_sec[sec]++; }
        if (stamp && has_mt && m0 > 100000000LL) {
            n_real++;
            if (m0 != last_m0) { n_uniq++; last_m0 = m0; int sec = (int) ((now - t_first) / 1e6); if (sec >= 0 && sec < 600) uniq_sec[sec]++; }
        }
        // stamp のときは media time が「撮った時刻」（QPC µs）。大きい値なら遅れとして記録する
        if (stamp && has_mt && m0 > 1000000000LL && nlat < 20000) lat[nlat++] = (now - (double) m0) / 1000.0;
        LeaveCriticalSection(&lk);
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE *, LONG) override { return E_NOTIMPL; }
};

static int cmp_d(const void *a, const void *b) { double x = *(const double *) a, y = *(const double *) b; return x < y ? -1 : x > y; }

static IPin *get_pin(IBaseFilter *f, PIN_DIRECTION want) {
    IEnumPins *ep = NULL;
    IPin *p = NULL, *r = NULL;
    if (FAILED(f->EnumPins(&ep))) return NULL;
    while (!r && ep->Next(1, &p, NULL) == S_OK) {
        PIN_DIRECTION d;
        if (SUCCEEDED(p->QueryDirection(&d)) && d == want) r = p;
        else p->Release();
    }
    ep->Release();
    return r;
}

static int cmd_grab(const char *name, const char *fmt, int W, int H, int fps, int secs, const char *png) {
    IBaseFilter *cam = NULL;
    if (cmd_list(name, &cam) <= 0 || !cam) { printf("「%s」が一覧に無いか、作れない\n", name); return 1; }
    const GUID *sub = &kSUB_NV12;
    Kind kind = K_NV12;
    if (!strcmp(fmt, "YUY2")) sub = &kSUB_YUY2, kind = K_YUY2;
    else if (!strcmp(fmt, "I420")) sub = &kSUB_I420, kind = K_I420;
    else if (!strcmp(fmt, "RGB32")) sub = &kSUB_RGB32, kind = K_RGB32;
    else if (!strcmp(fmt, "RGB24")) sub = &kSUB_RGB24, kind = K_RGB24;
    IPin *out = get_pin(cam, PINDIR_OUTPUT);
    // ピンの種類（PIN_CATEGORY_CAPTURE と名乗るか）
    IKsPropertySet *ks = NULL;
    if (out && SUCCEEDED(out->QueryInterface(kIID_IKsPropertySet, (void **) &ks))) {
        GUID cat = {};
        DWORD got = 0;
        HRESULT hr = ks->Get(kAMPROPSETID_Pin, 0, NULL, 0, &cat, sizeof cat, &got);
        printf("ピンの種類：%s（0x%08lx）\n", SUCCEEDED(hr) && eq(cat, kPIN_CATEGORY_CAPTURE) ? "PIN_CATEGORY_CAPTURE" : "違う", (unsigned long) hr);
        ks->Release();
    }
    // 形を選ぶ（アプリと同じく IAMStreamConfig の一覧から）
    IAMStreamConfig *sc = NULL;
    int chosen = -1;
    if (out && SUCCEEDED(out->QueryInterface(kIID_IAMStreamConfig, (void **) &sc))) {
        int cnt = 0, sz = 0;
        sc->GetNumberOfCapabilities(&cnt, &sz);
        printf("IAMStreamConfig：形 %d 個\n", cnt);
        for (int i = 0; i < cnt && chosen < 0; i++) {
            AM_MEDIA_TYPE *mt = NULL;
            BYTE caps[256];
            if (sc->GetStreamCaps(i, &mt, caps) != S_OK || !mt) continue;
            const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) mt->pbFormat;
            if (eq(mt->subtype, *sub) && v->bmiHeader.biWidth == W && v->bmiHeader.biHeight == H) {
                ((VIDEOINFOHEADER *) mt->pbFormat)->AvgTimePerFrame = 10000000LL / fps;
                HRESULT hr = sc->SetFormat(mt);
                printf("SetFormat %s %dx%d %d コマ/秒：0x%08lx\n", fmt, W, H, fps, (unsigned long) hr);
                chosen = i;
            }
            if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
            if (mt->pUnk) mt->pUnk->Release();
            CoTaskMemFree(mt);
        }
        sc->Release();
    }
    if (chosen < 0) { printf("その形は一覧に無い\n"); return 1; }
    IGraphBuilder *gb = NULL;
    IBaseFilter *sgf = NULL, *nul = NULL;
    ISG *sg = NULL;
    IMediaControl *mc = NULL;
    HRESULT hr = CoCreateInstance(kCLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, kIID_IGraphBuilder, (void **) &gb);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(kCLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER, kIID_IBaseFilter, (void **) &sgf);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(kCLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER, kIID_IBaseFilter, (void **) &nul);
    if (FAILED(hr)) { printf("graph の部品を作れない 0x%08lx\n", (unsigned long) hr); return 1; }
    sgf->QueryInterface(kIID_ISampleGrabber, (void **) &sg);
    AM_MEDIA_TYPE want = {};
    want.majortype = kMEDIATYPE_Video, want.subtype = *sub;
    sg->SetMediaType(&want);
    sg->SetBufferSamples(FALSE);
    sg->SetOneShot(FALSE);
    static Grab cb;
    InitializeCriticalSection(&cb.lk);
    cb.kind = kind, cb.w = W, cb.h = H;
    char sv[8];
    cb.stamp = GetEnvironmentVariableA("POCKETGPU_VCAM_STAMP", sv, sizeof sv) > 0;
    cb.want_copy = png != NULL;
    sg->SetCallback(&cb, 0);
    gb->AddFilter(cam, L"cam");
    gb->AddFilter(sgf, L"grab");
    gb->AddFilter(nul, L"null");
    IPin *sgin = get_pin(sgf, PINDIR_INPUT), *sgout = get_pin(sgf, PINDIR_OUTPUT), *nin = get_pin(nul, PINDIR_INPUT);
    hr = gb->ConnectDirect(out, sgin, NULL);
    printf("カメラ → SampleGrabber を直接つなぐ：0x%08lx\n", (unsigned long) hr);
    if (FAILED(hr)) return 1;
    hr = gb->ConnectDirect(sgout, nin, NULL);
    printf("SampleGrabber → NullRenderer：0x%08lx\n", (unsigned long) hr);
    AM_MEDIA_TYPE cmt = {};
    if (SUCCEEDED(out->ConnectionMediaType(&cmt))) {
        const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) cmt.pbFormat;
        printf("つながった形：%ldx%ld・%.2f コマ/秒・1 コマ %lu バイト\n", (long) v->bmiHeader.biWidth, (long) v->bmiHeader.biHeight, 1e7 / (double) v->AvgTimePerFrame, (unsigned long) cmt.lSampleSize);
        if (cmt.cbFormat) CoTaskMemFree(cmt.pbFormat);
    }
    gb->QueryInterface(kIID_IMediaControl, (void **) &mc);
    FILETIME ct0, et0, kt0, ut0, ct1, et1, kt1, ut1;
    GetProcessTimes(GetCurrentProcess(), &ct0, &et0, &kt0, &ut0);
    const double w0 = (double) wcam_shm_now_us();
    hr = mc->Run();
    printf("Run：0x%08lx。%d 秒取る\n", (unsigned long) hr, secs);
    Sleep((DWORD) secs * 1000);
    mc->Stop();
    const double w1 = (double) wcam_shm_now_us();
    GetProcessTimes(GetCurrentProcess(), &ct1, &et1, &kt1, &ut1);
    EnterCriticalSection(&cb.lk);
    double dur = (cb.t_last - cb.t_first) / 1e6;
    printf("受けたコマ %ld（%.1f コマ/秒）・上の四角の番号の種類 %d・読めないコマ %d\n", cb.n, dur > 0 ? (cb.n - 1) / dur : 0, cb.distinct, cb.ctr_none);
    if (cb.stamp) {
        printf("本物の絵のコマ %ld・うち新しい絵（撮った時刻が前と違う）%ld（%.1f コマ/秒）\n", cb.n_real, cb.n_uniq, dur > 0 ? (cb.n_uniq - 1) / dur : 0);
        int last_sec = (int) dur;
        printf("1 秒ごとの新しい絵の数：");
        for (int i = 0; i <= last_sec && i < 600; i++) printf("%d ", cb.uniq_sec[i]);
        printf("\n");
    }
    if (cb.ngap > 10) {
        qsort(cb.gap, (size_t) cb.ngap, sizeof(double), cmp_d);
        double s = 0;
        int over = 0;
        const double nominal = 1000.0 / fps;
        for (int i = 0; i < cb.ngap; i++) { s += cb.gap[i]; if (cb.gap[i] > nominal * 1.5) over++; }
        printf("受け取る間隔（ms。目標 %.1f）：平均 %.2f・中央 %.2f・95%% %.2f・最大 %.2f・目標の 1.5 倍より長い %d / %d\n", nominal, s / cb.ngap, cb.gap[cb.ngap / 2], cb.gap[(int) (cb.ngap * 0.95)], cb.gap[cb.ngap - 1], over, cb.ngap);
    }
    {
        auto ft = [](const FILETIME &f) { return (double) (((ULONGLONG) f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
        const double cpu = (ft(kt1) - ft(kt0)) + (ft(ut1) - ft(ut0)), wall = (w1 - w0) / 1e6;
        printf("この道具（仮想カメラの DLL が動く糸を含む）の CPU：%.2f 秒 / %.2f 秒（1 コア = 100%% で %.1f%%）\n", cpu, wall, wall > 0 ? cpu / wall * 100 : 0);
    }
    {
        int last_sec = (int) dur;
        printf("1 秒ごとの新しいコマの数：");
        for (int i = 0; i <= last_sec && i < 600; i++) printf("%d ", cb.per_sec[i]);
        printf("\n");
    }
    if (cb.nlat) {
        qsort(cb.lat, (size_t) cb.nlat, sizeof(double), cmp_d);
        double s = 0;
        for (int i = 0; i < cb.nlat; i++) s += cb.lat[i];
        printf("撮った→この道具が受けた：平均 %.1f・中央 %.1f・95%% %.1f・最大 %.1f ms（%d コマ）\n", s / cb.nlat, cb.lat[cb.nlat / 2], cb.lat[(int) (cb.nlat * 0.95)], cb.lat[cb.nlat - 1], cb.nlat);
    }
    if (png && cb.last && cb.last_len) {
        uint8_t *rgb = (uint8_t *) malloc((size_t) W * H * 3);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) pix(kind, cb.last, W, H, x, y, rgb + ((size_t) y * W + x) * 3);
        printf("最後のコマを書いた：%s（%s）\n", png, write_png(png, rgb, (uint32_t) W, (uint32_t) H) ? "できた" : "失敗");
        int c = read_counter(kind, cb.last, W, H);
        printf("最後のコマの上の四角：%d\n", c);
        free(rgb);
    }
    LeaveCriticalSection(&cb.lk);
    mc->Release();
    out->Release(), sgin->Release(), sgout->Release(), nin->Release();
    sg->Release();
    gb->Release(), sgf->Release(), nul->Release(), cam->Release();
    return cb.n > 0 ? 0 : 1;
}

// camset：アプリが形を選ぶ前に見る物（既定の形・IAMVideoControl のコマ数の一覧）を出す。Chrome と OBS が読む所
G(kIID_IAMVideoControl, 0x6a2e0670, 0x28e4, 0x11d0, 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56)
static int cmd_caps(const char *name) {
    IBaseFilter *cam = NULL;
    if (cmd_list(name, &cam) <= 0 || !cam) { printf("「%s」が一覧に無いか、作れない\n", name); return 1; }
    IPin *out = get_pin(cam, PINDIR_OUTPUT);
    IAMStreamConfig *sc = NULL;
    IAMVideoControl *vc = NULL;
    cam->QueryInterface(kIID_IAMVideoControl, (void **) &vc);
    printf("IAMVideoControl：%s\n", vc ? "あり" : "なし");
    if (out && SUCCEEDED(out->QueryInterface(kIID_IAMStreamConfig, (void **) &sc))) {
        AM_MEDIA_TYPE *mt = NULL;
        if (SUCCEEDED(sc->GetFormat(&mt)) && mt) {
            const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) mt->pbFormat;
            printf("既定の形（GetFormat）：%ldx%ld・%.2f コマ/秒\n", (long) v->bmiHeader.biWidth, (long) v->bmiHeader.biHeight, 1e7 / (double) v->AvgTimePerFrame);
            if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
            CoTaskMemFree(mt);
        }
        IEnumMediaTypes *em = NULL;
        if (SUCCEEDED(out->EnumMediaTypes(&em)) && em) {
            AM_MEDIA_TYPE *t = NULL;
            if (em->Next(1, &t, NULL) == S_OK && t) {
                const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) t->pbFormat;
                printf("一覧の最初の形（EnumMediaTypes）：%ldx%ld・%.2f コマ/秒\n", (long) v->bmiHeader.biWidth, (long) v->bmiHeader.biHeight, 1e7 / (double) v->AvgTimePerFrame);
                if (t->cbFormat) CoTaskMemFree(t->pbFormat);
                CoTaskMemFree(t);
            }
            em->Release();
        }
        int cnt = 0, sz = 0;
        sc->GetNumberOfCapabilities(&cnt, &sz);
        for (int i = 0; i < cnt; i++) {
            BYTE caps[256];
            if (sc->GetStreamCaps(i, &mt, caps) != S_OK || !mt) continue;
            const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) mt->pbFormat;
            const VIDEO_STREAM_CONFIG_CAPS *c = (const VIDEO_STREAM_CONFIG_CAPS *) caps;
            char rates[128] = "-";
            if (vc) {
                LONG n = 0;
                LONGLONG *list = NULL;
                SIZE d = { v->bmiHeader.biWidth, v->bmiHeader.biHeight };
                if (SUCCEEDED(vc->GetFrameRateList(out, i, d, &n, &list)) && list) {
                    int at = 0;
                    for (LONG k = 0; k < n; k++) at += snprintf(rates + at, sizeof rates - (size_t) at, "%s%.0f", k ? "・" : "", 1e7 / (double) list[k]);
                    CoTaskMemFree(list);
                }
            }
            char fcc[5] = { 0 };
            memcpy(fcc, &mt->subtype.Data1, 4);
            if (v->bmiHeader.biCompression == 0) strcpy(fcc, v->bmiHeader.biBitCount == 32 ? "RGB32" : "RGB24");
            printf("  %2d. %-5s %4ldx%-4ld 形の %.0f コマ/秒・範囲 %.0f〜%.0f・IAMVideoControl %s\n", i, fcc, (long) v->bmiHeader.biWidth, (long) v->bmiHeader.biHeight, 1e7 / (double) v->AvgTimePerFrame,
                   1e7 / (double) c->MaxFrameInterval, 1e7 / (double) c->MinFrameInterval, rates);
            if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
            CoTaskMemFree(mt);
        }
        sc->Release();
    }
    if (vc) vc->Release();
    if (out) out->Release();
    cam->Release();
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    printf("dscam_tool（%d ビット）\n", (int) sizeof(void *) * 8);
    int rc = 0;
    if (argc >= 3 && !strcmp(argv[1], "caps")) {
        rc = cmd_caps(argv[2]);
    } else if (argc >= 2 && !strcmp(argv[1], "list")) {
        printf("DirectShow の Video Capture Sources：\n");
        rc = cmd_list(NULL, NULL) >= 0 ? 0 : 1;
    } else if (argc >= 2 && !strcmp(argv[1], "mf")) {
        cmd_mf();
    } else if (argc >= 3 && !strcmp(argv[1], "grab")) {
        const char *fmt = "NV12", *png = NULL;
        int w = 1280, h = 720, fps = 30, secs = 5;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--fmt") && i + 1 < argc) fmt = argv[++i];
            else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
            else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--secs") && i + 1 < argc) secs = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        }
        rc = cmd_grab(argv[2], fmt, w, h, fps, secs, png);
    } else {
        printf("使い方：dscam_tool list | mf | caps \"名前\" | grab \"名前\" [--fmt NV12] [--size 1280x720] [--fps 30] [--secs 5] [--png f.png]\n");
        rc = 2;
    }
    CoUninitialize();
    return rc;
}
