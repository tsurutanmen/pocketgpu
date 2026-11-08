// DirectShow の仮想カメラ（source filter の COM の DLL）。「PocketGPU iPhone Camera」という名前でカメラの一覧に出る。
// 絵は webcam_rx.exe --shm が共有メモリ Local\pocketgpu-webcam-v1 に書く NV12。来なければ「待っています」の絵を出す。
//
// 入れ方：管理者の権限は要らない。DllRegisterServer は HKCU だけに書く（HKLM には触らない）。
//   64 ビット：%WINDIR%\System32\regsvr32.exe /s dshow_cam64.dll
//   32 ビット：%WINDIR%\SysWOW64\regsvr32.exe /s dshow_cam32.dll   （HKCU\Software\Classes\WOW6432Node に入る）
//   外す：regsvr32 /u /s ...
// 書く所：
//   HKCU\Software\Classes\CLSID\{82069F97-...}\InprocServer32 = この DLL・ThreadingModel = Both
//   HKCU\Software\Classes\CLSID\{860BB310-...（Video Capture Sources）}\Instance\{82069F97-...}：FriendlyName・CLSID・FilterData
// アプリの中の System Device Enumerator は HKEY_CLASSES_ROOT（HKLM と HKCU を合わせた見え方）からこの Instance を読む。
//
// 作り：BaseClasses を使わずに、出口のピン 1 本の source filter を手で書いた。
//   filter：IBaseFilter（IMediaFilter・IPersist）・IAMFilterMiscFlags（source だと名乗る）・IAMVideoControl（大きさごとのコマ数の一覧。Chrome が読む）
//   pin：IPin・IAMStreamConfig（形の一覧と選択）・IKsPropertySet（PIN_CATEGORY_CAPTURE と名乗る）
//   形：1280x720・1920x1080・640x480・640x360 × NV12・YUY2・I420・RGB32・RGB24（30 と 60 コマ/秒。1〜60 を受ける）
//   cam4k：レジストリ HKCU\Software\PocketGPU\VirtualCamera の MaxHeight が 2160 のときだけ、3840x2160（30・60）も足す（webcam_rx --vcam-max 2160 が書く。無ければ今までどおり 1080p まで）。
//        4K の絵は共有メモリの 4K の口（名前の後ろに -4k。wcam_shm.h）から読む。口が無い（前の webcam_rx）なら、1920x1080 の絵を引き伸ばして出す。
//        色の変換は SSE2（vcam_convert.h）。-DVCAM_TEST で組むと、別の CLSID・名前・レジストリの置き場になる（本物の登録に触らない試験用）。
//   Run の間だけ、自分の糸が決めたコマ数でサンプルを作って下流の IMemInputPin::Receive に渡す。
// camset で足した所：
//   既定の形：iPhone がいま送っている形（共有メモリの cur）。まだ分からなければ 1280x720・30。
//   アプリが既定と違う形（大きさかコマ数）を選んで Run したら、その形を共有メモリの req に書き、webcam_rx が iPhone に頼み直す。
//   1280x720 より小さい大きさは頼まない（iPhone からは 720p 以上で受けて、ここで縮める方がきれい）。Stop で自分の頼みを消す。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <strmif.h>
#include <amvideo.h>
#include <vfwmsgs.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include "wcam_shm.h"
#include "textmask.h"
#include "vcam_convert.h"

static_assert(sizeof(wcam_shm_head) == 256, "共有メモリの頭は 256 バイト");
static_assert(sizeof(wcam_ext_head) == 256, "4K の口の頭は 256 バイト");

// ---------------------------------------------------------------- GUID（mingw の uuids.h・strmif.h の値と照らした）
#define G(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } };
#ifndef VCAM_TEST
// この仮想カメラの CLSID（新しく作った）
G(kCLSID_Cam, 0x82069F97, 0x3CA9, 0x4626, 0xAD, 0x74, 0xD2, 0xAA, 0x50, 0xBB, 0xD6, 0x18)
#define CAM_CLSID_STR L"{82069F97-3CA9-4626-AD74-D2AA50BBD618}"
#define CAM_NAME L"PocketGPU iPhone Camera"
#define REGKEY_NAME WCAM_VCAM_REGKEY
#else
// cam4k の試験用（-DVCAM_TEST）：本物と別の CLSID・名前・レジストリの置き場。共有メモリの名前は環境変数 PGPU_TEST_SHM（無ければ Local\pocketgpu-webcam-cam4ktest）。
// 本物の登録（HKCU の本物の CLSID）には一切触らない
G(kCLSID_Cam, 0x82069F97, 0x3CA9, 0x4626, 0xAD, 0x74, 0xD2, 0xAA, 0x50, 0xBB, 0xD6, 0x4B)
#define CAM_CLSID_STR L"{82069F97-3CA9-4626-AD74-D2AA50BBD64B}"
#define CAM_NAME L"PocketGPU CAM4K TEST Camera"
#define REGKEY_NAME WCAM_VCAM_REGKEY_TEST
#endif
#define CAT_STR L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}" // CLSID_VideoInputDeviceCategory
G(kIID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
G(kIID_IClassFactory, 0x00000001, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
G(kIID_IPersist, 0x0000010c, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
G(kIID_IMediaFilter, 0x56a86899, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IBaseFilter, 0x56a86895, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IPin, 0x56a86891, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IEnumPins, 0x56a86892, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IEnumMediaTypes, 0x89c31040, 0x846b, 0x11ce, 0x97, 0xd3, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a)
G(kIID_IMemInputPin, 0x56a8689d, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IMemAllocator, 0x56a8689c, 0x0ad4, 0x11ce, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kIID_IAMStreamConfig, 0xc6e13340, 0x30ac, 0x11d0, 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56)
G(kIID_IKsPropertySet, 0x31efac30, 0x515c, 0x11d0, 0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93)
G(kIID_IAMFilterMiscFlags, 0x2dd74950, 0xa890, 0x11d1, 0xab, 0xe8, 0x00, 0xa0, 0xc9, 0x05, 0xf3, 0x75)
G(kIID_IAMVideoControl, 0x6a2e0670, 0x28e4, 0x11d0, 0xa1, 0x8c, 0x00, 0xa0, 0xc9, 0x11, 0x89, 0x56)
G(kCLSID_MemoryAllocator, 0x1e651cc0, 0xb199, 0x11d0, 0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45)
G(kMEDIATYPE_Video, 0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kFORMAT_VideoInfo, 0x05589f80, 0xc356, 0x11ce, 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a)
G(kSUB_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_YUY2, 0x32595559, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_I420, 0x30323449, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
G(kSUB_RGB32, 0xe436eb7e, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kSUB_RGB24, 0xe436eb7d, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70)
G(kPIN_CATEGORY_CAPTURE, 0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba)
G(kAMPROPSETID_Pin, 0x9b00f101, 0x1567, 0x11d1, 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5)
static const GUID kGUID_NULL = {};

static bool eq(const GUID &a, const GUID &b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

// new と delete は C++ の実行時の物を引き込まないよう、ヒープを直接使う
void *operator new(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
void operator delete(void *p) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }
void operator delete(void *p, size_t) noexcept { if (p) HeapFree(GetProcessHeap(), 0, p); }

static HMODULE g_mod;
static volatile LONG g_objs, g_locks;

// ---------------------------------------------------------------- 記録（環境変数 POCKETGPU_VCAM_LOG にファイル名があるときだけ）
static void logf(const char *fmt, ...) {
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("POCKETGPU_VCAM_LOG", path, sizeof path)) return;
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(b, sizeof b - 3, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int) sizeof b - 3;
    b[n++] = '\r', b[n++] = '\n', b[n] = 0;
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, b, (DWORD) n, &w, NULL); CloseHandle(h); }
}

// ---------------------------------------------------------------- 見せる形
enum Kind { K_NV12, K_YUY2, K_I420, K_RGB32, K_RGB24 };
struct Sub { const GUID *guid; DWORD fourcc; WORD bpp; Kind kind; };
static const Sub SUBS[] = {
    { &kSUB_NV12, MAKEFOURCC('N', 'V', '1', '2'), 12, K_NV12 }, { &kSUB_YUY2, MAKEFOURCC('Y', 'U', 'Y', '2'), 16, K_YUY2 },
    { &kSUB_I420, MAKEFOURCC('I', '4', '2', '0'), 12, K_I420 }, { &kSUB_RGB32, BI_RGB, 32, K_RGB32 }, { &kSUB_RGB24, BI_RGB, 24, K_RGB24 },
};
// 4K（3840x2160）は最後に置く（前の版の形の番号を動かさない）。名乗るのは、レジストリの MaxHeight が 2160 のときだけ（NSIZES_BASE が前までの 4 つ）
static const struct { int w, h; } SIZES[] = { { 1280, 720 }, { 1920, 1080 }, { 640, 480 }, { 640, 360 }, { 3840, 2160 } };
enum { NSUBS = sizeof SUBS / sizeof SUBS[0], NSIZES_ALL = sizeof SIZES / sizeof SIZES[0], NSIZES_BASE = NSIZES_ALL - 1, MAXTYPES = NSUBS * NSIZES_ALL };
static const REFERENCE_TIME AVG_DEFAULT = 333333, AVG_MIN = 166666, AVG_MAX = 10000000; // 30・60・1 コマ/秒

// 名乗る一番大きい高さ（レジストリ）。無い・2160 以外は 1080。フィルタを作るときに 1 回読む（アプリが形の一覧を読む間に変わらないように）
static int read_max_height() {
    HKEY k;
    DWORD v = 0, n = sizeof v, t = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGKEY_NAME, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return 1080;
    LONG r = RegQueryValueExW(k, WCAM_VCAM_REGVAL, NULL, &t, (BYTE *) &v, &n);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && t == REG_DWORD && v >= 2160 ? 2160 : 1080;
}

static int type_w(int i) { return SIZES[i / NSUBS].w; }
static int type_h(int i) { return SIZES[i / NSUBS].h; }
static const Sub &type_sub(int i) { return SUBS[i % NSUBS]; }
static long rgb_stride(int w, int bpp) { return ((w * bpp / 8) + 3) & ~3; }
static long image_size(Kind k, int w, int h) {
    switch (k) {
    case K_NV12:
    case K_I420: return (long) w * h * 3 / 2;
    case K_YUY2: return (long) w * h * 2;
    case K_RGB32: return rgb_stride(w, 32) * h;
    default: return rgb_stride(w, 24) * h;
    }
}

static void free_mt_fields(AM_MEDIA_TYPE *mt) {
    if (mt->cbFormat && mt->pbFormat) CoTaskMemFree(mt->pbFormat);
    mt->cbFormat = 0, mt->pbFormat = NULL;
    if (mt->pUnk) { mt->pUnk->Release(); mt->pUnk = NULL; }
}
static HRESULT copy_mt(AM_MEDIA_TYPE *dst, const AM_MEDIA_TYPE *src) {
    *dst = *src;
    if (src->cbFormat && src->pbFormat) {
        dst->pbFormat = (BYTE *) CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat) { dst->cbFormat = 0; dst->pUnk = NULL; return E_OUTOFMEMORY; }
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    } else dst->cbFormat = 0, dst->pbFormat = NULL;
    if (dst->pUnk) dst->pUnk->AddRef();
    return S_OK;
}
static AM_MEDIA_TYPE *dup_mt(const AM_MEDIA_TYPE *src) {
    AM_MEDIA_TYPE *p = (AM_MEDIA_TYPE *) CoTaskMemAlloc(sizeof *p);
    if (!p) return NULL;
    if (FAILED(copy_mt(p, src))) { CoTaskMemFree(p); return NULL; }
    return p;
}
static void delete_mt(AM_MEDIA_TYPE *p) {
    if (!p) return;
    free_mt_fields(p);
    CoTaskMemFree(p);
}

// i 番の形を作る（pbFormat は CoTaskMemAlloc）
static HRESULT make_mt(int i, REFERENCE_TIME avg, AM_MEDIA_TYPE *mt) {
    memset(mt, 0, sizeof *mt);
    const Sub &s = type_sub(i);
    const int w = type_w(i), h = type_h(i);
    VIDEOINFOHEADER *v = (VIDEOINFOHEADER *) CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!v) return E_OUTOFMEMORY;
    memset(v, 0, sizeof *v);
    const long size = image_size(s.kind, w, h);
    v->AvgTimePerFrame = avg;
    v->dwBitRate = (DWORD) ((double) size * 8 * 10000000.0 / (double) avg);
    v->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    v->bmiHeader.biWidth = w, v->bmiHeader.biHeight = h, v->bmiHeader.biPlanes = 1;
    v->bmiHeader.biBitCount = s.bpp, v->bmiHeader.biCompression = s.fourcc, v->bmiHeader.biSizeImage = (DWORD) size;
    mt->majortype = kMEDIATYPE_Video, mt->subtype = *s.guid, mt->bFixedSizeSamples = TRUE, mt->bTemporalCompression = FALSE;
    mt->lSampleSize = (ULONG) size, mt->formattype = kFORMAT_VideoInfo, mt->cbFormat = sizeof(VIDEOINFOHEADER), mt->pbFormat = (BYTE *) v;
    return S_OK;
}

// 形を受けられるなら、何番の形か（と 1 コマの長さ）を返す。受けられなければ -1
static int match_mt(const AM_MEDIA_TYPE *mt, REFERENCE_TIME *avg, int ntypes) {
    if (!mt || !eq(mt->majortype, kMEDIATYPE_Video) || !eq(mt->formattype, kFORMAT_VideoInfo) || mt->cbFormat < sizeof(VIDEOINFOHEADER) || !mt->pbFormat) return -1;
    const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) mt->pbFormat;
    for (int i = 0; i < ntypes; i++) {
        if (!eq(mt->subtype, *type_sub(i).guid)) continue;
        if (v->bmiHeader.biWidth != type_w(i) || v->bmiHeader.biHeight != type_h(i)) continue;
        REFERENCE_TIME a = v->AvgTimePerFrame ? v->AvgTimePerFrame : AVG_DEFAULT;
        if (a < AVG_MIN || a > AVG_MAX) return -1;
        if (avg) *avg = a;
        return i;
    }
    return -1;
}

// ---------------------------------------------------------------- 絵を作る
// src（sw×sh の NV12・詰めてある）を dst（dw×dh の NV12・行の幅 dpitch）へ。縦横比を保って収め、余りは黒
static void nv12_fit(uint8_t *dy, uint8_t *duv, long dpitch, int dw, int dh, const uint8_t *src, int sw, int sh) {
    const uint8_t *sy = src, *suv = src + (size_t) sw * sh;
    if (sw == dw && sh == dh) {
        for (int y = 0; y < dh; y++) memcpy(dy + (size_t) y * dpitch, sy + (size_t) y * sw, (size_t) dw);
        for (int y = 0; y < dh / 2; y++) memcpy(duv + (size_t) y * dpitch, suv + (size_t) y * sw, (size_t) dw);
        return;
    }
    int fw = dw, fh = (int) ((int64_t) dw * sh / sw);
    if (fh > dh) { fh = dh; fw = (int) ((int64_t) dh * sw / sh); }
    fw &= ~1, fh &= ~1;
    const int ox = ((dw - fw) / 2) & ~1, oy = ((dh - fh) / 2) & ~1;
    for (int y = 0; y < dh; y++) memset(dy + (size_t) y * dpitch, 16, (size_t) dw);
    for (int y = 0; y < dh / 2; y++) memset(duv + (size_t) y * dpitch, 128, (size_t) dw);
    // cam4k：大きさが違うときの最近傍の拡大・縮小。横の対応は 1 行ぶんの表に先に作り（画素ごとの割り算をしない）、同じ元の行から作る行は前の行を写す。
    // 4K に 1080p を引き伸ばす（アプリが 4K を選んで iPhone が切り替わるまでの間・前の webcam_rx との組み合わせ）ときに、4K の 1 コマが 16.7 ms に収まるように
    static const int XM = 4096;
    int xm[XM];
    if (fw > XM) return;
    for (int x = 0; x < fw; x++) xm[x] = (int) ((int64_t) x * sw / fw);
    int prev_sy = -1;
    for (int y = 0; y < fh; y++) {
        const int syi = (int) ((int64_t) y * sh / fh);
        uint8_t *d = dy + (size_t) (oy + y) * dpitch + ox;
        if (syi == prev_sy && y > 0) { memcpy(d, d - dpitch, (size_t) fw); continue; }
        prev_sy = syi;
        const uint8_t *r = sy + (size_t) syi * sw;
        for (int x = 0; x < fw; x++) d[x] = r[xm[x]];
    }
    for (int x = 0; x < fw / 2; x++) xm[x] = (int) ((int64_t) x * (sw / 2) / (fw / 2)) * 2;
    prev_sy = -1;
    for (int y = 0; y < fh / 2; y++) {
        const int syi = (int) ((int64_t) y * (sh / 2) / (fh / 2));
        uint8_t *d = duv + (size_t) (oy / 2 + y) * dpitch + ox;
        if (syi == prev_sy && y > 0) { memcpy(d, d - dpitch, (size_t) fw); continue; }
        prev_sy = syi;
        const uint8_t *r = suv + (size_t) syi * sw;
        for (int x = 0; x < fw / 2; x++) {
            const int sx = xm[x];
            d[2 * x] = r[sx], d[2 * x + 1] = r[sx + 1];
        }
    }
}

// 絵が来ないときの絵：暗い灰色・文字（日本語と英語）・下を左右に動く帯（止まっていないと分かるように）
static void nv12_waiting(uint8_t *dy, uint8_t *duv, long pitch, int w, int h, uint64_t ms) {
    for (int y = 0; y < h; y++) memset(dy + (size_t) y * pitch, 40, (size_t) w);
    for (int y = 0; y < h / 2; y++) memset(duv + (size_t) y * pitch, 128, (size_t) w);
    const int tw = w * 80 / 100, th = (int) ((int64_t) tw * TEXTMASK_H / TEXTMASK_W);
    const int tx = (w - tw) / 2, ty = h * 30 / 100;
    for (int y = 0; y < th && ty + y < h; y++) {
        const unsigned char *row = TEXTMASK + (size_t) ((int64_t) y * TEXTMASK_H / th) * (TEXTMASK_W / 8);
        uint8_t *d = dy + (size_t) (ty + y) * pitch + tx;
        for (int x = 0; x < tw; x++) {
            int mx = (int) ((int64_t) x * TEXTMASK_W / tw);
            if (row[mx >> 3] & (0x80 >> (mx & 7))) d[x] = 225;
        }
    }
    const int bw = w / 12, bh = h / 40 > 2 ? h / 40 : 2, by = h * 80 / 100;
    const int x0 = (int) ((ms / 4) % (uint64_t) (w - bw)) & ~1;
    for (int y = by; y < by + bh && y < h; y++) memset(dy + (size_t) y * pitch + x0, 150, (size_t) bw);
}

static inline uint8_t clip8(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }

// NV12（w×h・詰めてある）を出す形へ。stride は出す側の 1 行のバイト数。RGB は下から上（biHeight が正）。
// cam4k：SIMD（SSE2）の vcam_convert.h に替えた。前の 1 画素ずつの書き方は RGB で 1080p が 28 ms・4K が 84 ms かかった（60 コマ/秒の 1 コマは 16.7 ms）
static void convert_out(Kind k, const uint8_t *src, int w, int h, uint8_t *dst, long stride) {
    if (k == K_NV12) return; // ここには来ない（直接 dst に収める）
    vc_convert_rows((VcKind) k, src, w, h, dst, stride, 0, h);
}

// ---------------------------------------------------------------- 共有メモリから読む
struct ShmReader {
    HANDLE map = NULL;
    wcam_shm_head *head = NULL;
    bool writable = false; // 制御の欄（req）に書けるか。整合性レベルが低いプロセスは読むだけ
    int64_t next_try_us = 0;
    // cam4k：4K の口（名前の後ろに -4k の別の共有メモリ）。頭の ext_state が 1 のときだけ開く。読むだけ
    HANDLE emap = NULL;
    wcam_ext_head *ext = NULL;
    int64_t next_ext_try_us = 0;
    WCHAR name[160] = {};
    void set_name() {
        if (name[0]) return;
        wcscpy(name, WCAM_SHM_NAME);
#ifdef VCAM_TEST
        WCHAR t[128];
        DWORD n = GetEnvironmentVariableW(L"PGPU_TEST_SHM", t, 128);
        wcscpy(name, n > 0 && n < 128 ? t : L"Local\\pocketgpu-webcam-cam4ktest");
#endif
    }
    void close_ext() {
        if (ext) UnmapViewOfFile(ext);
        if (emap) CloseHandle(emap);
        ext = NULL, emap = NULL;
    }
    void close() {
        close_ext();
        if (head) UnmapViewOfFile(head);
        if (map) CloseHandle(map);
        head = NULL, map = NULL, writable = false;
    }
    bool open_if_needed(int64_t now) {
        if (head) return true;
        if (now < next_try_us) return false;
        next_try_us = now + 500000;
        set_name();
        // 頼み（req）を書くために、書ける形で開いてみる。だめなら読むだけ
        DWORD acc = FILE_MAP_READ | FILE_MAP_WRITE;
        map = OpenFileMappingW(acc, FALSE, name);
        if (!map) { acc = FILE_MAP_READ; map = OpenFileMappingW(acc, FALSE, name); }
        if (!map) return false;
        writable = acc != FILE_MAP_READ;
        head = (wcam_shm_head *) MapViewOfFile(map, acc, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mbi;
        if (!head || !VirtualQuery(head, &mbi, sizeof mbi) || mbi.RegionSize < WCAM_SHM_TOTAL || head->magic != WCAM_SHM_MAGIC || head->nslots != WCAM_SHM_SLOTS ||
            head->slot_bytes != WCAM_SHM_SLOT_BYTES) {
            logf("共有メモリの形が違うので使わない");
            close();
            return false;
        }
        logf("共有メモリを開いた（書く側 pid %u・%s・版 %u・4K の口 %s）", head->writer_pid, writable ? "頼みを書ける" : "読むだけ", head->version, head->ext_state ? "あり" : "なし");
        return true;
    }
    // 4K の口を開く（頭の ext_state が 1 のとき。開けなければ 0.5 秒おきに試す）。開いていれば true
    bool ext_ready(int64_t now) {
        if (ext) return head && head->ext_state == 1;
        if (!head || head->ext_state != 1 || now < next_ext_try_us) return false;
        next_ext_try_us = now + 500000;
        WCHAR en[200];
        _snwprintf(en, 199, L"%ls%ls", name, WCAM_EXT_SUFFIX);
        en[199] = 0;
        emap = OpenFileMappingW(FILE_MAP_READ, FALSE, en);
        if (!emap) return false;
        ext = (wcam_ext_head *) MapViewOfFile(emap, FILE_MAP_READ, 0, 0, 0);
        MEMORY_BASIC_INFORMATION mbi;
        if (!ext || !VirtualQuery(ext, &mbi, sizeof mbi) || mbi.RegionSize < WCAM_EXT_TOTAL || ext->magic != WCAM_EXT_MAGIC || ext->nslots != WCAM_EXT_SLOTS ||
            ext->slot_bytes != WCAM_EXT_SLOT_BYTES) {
            logf("4K の口の形が違うので使わない");
            close_ext();
            return false;
        }
        logf("4K の口を開いた");
        return true;
    }
    // 書く側が生きているか（heartbeat が 1 秒より新しい）
    bool alive(int64_t now) const { return head && head->heartbeat_us && now - head->heartbeat_us < 1000000; }
};

static int fps_of(REFERENCE_TIME avg) { return avg > 0 ? (int) ((10000000.0 / (double) avg) + 0.5) : 30; }
static REFERENCE_TIME avg_of(int fps) { return fps >= 1 ? (REFERENCE_TIME) (10000000 / fps) : AVG_DEFAULT; }
static int size_index(int w, int h, int nsizes) {
    for (int i = 0; i < nsizes; i++) if (SIZES[i].w == w && SIZES[i].h == h) return i;
    return -1;
}

// ---------------------------------------------------------------- filter と pin
class CamFilter;

class CamPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    CamFilter *f;
    IPin *peer = NULL;
    IMemInputPin *mip = NULL;
    IMemAllocator *alloc = NULL;
    AM_MEDIA_TYPE mt = {};
    int mt_idx = -1;
    REFERENCE_TIME mt_avg = AVG_DEFAULT;
    long out_stride = 0; // 下流が別の行の幅を頼んできたとき（動的な形の変更）
    int pref = -1;       // SetFormat で決まった形
    REFERENCE_TIME pref_avg = AVG_DEFAULT;
    bool pref_explicit = false; // SetFormat の形が既定と違った（アプリが選んだ）
    bool conn_explicit = false; // いまのつなぎの形をアプリが選んだ
    int ntypes = NSUBS * NSIZES_BASE; // cam4k：名乗る形の数。フィルタを作るときにレジストリの MaxHeight で決まる（2160 なら 4K を足す）

    explicit CamPin(CamFilter *o) : f(o) {}
    // IUnknown（filter と同じ寿命）
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    // IPin
    STDMETHODIMP Connect(IPin *recv, const AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP ReceiveConnection(IPin *, const AM_MEDIA_TYPE *) override { return E_UNEXPECTED; }
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin **pp) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO *pi) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION *d) override {
        if (!d) return E_POINTER;
        *d = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR *id) override {
        if (!id) return E_POINTER;
        *id = (LPWSTR) CoTaskMemAlloc(sizeof(L"Capture"));
        if (!*id) return E_OUTOFMEMORY;
        memcpy(*id, L"Capture", sizeof(L"Capture"));
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *pmt) override { return match_mt(pmt, NULL, ntypes) >= 0 ? S_OK : S_FALSE; }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **pp) override;
    STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return E_UNEXPECTED; }
    STDMETHODIMP BeginFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP EndFlush() override { return E_UNEXPECTED; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }
    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE *pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE **pp) override;
    STDMETHODIMP GetNumberOfCapabilities(int *count, int *size) override {
        if (!count || !size) return E_POINTER;
        *count = ntypes, *size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int i, AM_MEDIA_TYPE **pp, BYTE *caps) override;
    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID set, DWORD id, LPVOID, DWORD, LPVOID data, DWORD cb, DWORD *ret) override {
        if (!eq(set, kAMPROPSETID_Pin)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); // E_PROP_SET_UNSUPPORTED
        if (id != AMPROPERTY_PIN_CATEGORY) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); // E_PROP_ID_UNSUPPORTED
        if (ret) *ret = sizeof(GUID);
        if (!data) return S_OK;
        if (cb < sizeof(GUID)) return E_UNEXPECTED;
        *(GUID *) data = kPIN_CATEGORY_CAPTURE;
        return S_OK;
    }
    STDMETHODIMP QuerySupported(REFGUID set, DWORD id, DWORD *support) override {
        if (!eq(set, kAMPROPSETID_Pin)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (id != AMPROPERTY_PIN_CATEGORY) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
        if (support) *support = 1; // KSPROPERTY_SUPPORT_GET
        return S_OK;
    }

    HRESULT try_connect(IPin *recv, int idx, REFERENCE_TIME avg);
    HRESULT decide_allocator();
    void release_conn();
};

class CamFilter final : public IBaseFilter, public IAMFilterMiscFlags, public IAMVideoControl {
public:
    volatile LONG ref = 1;
    CRITICAL_SECTION lk;
    FILTER_STATE state = State_Stopped;
    IReferenceClock *clock = NULL;
    IFilterGraph *graph = NULL; // 弱い参照（filter は graph を AddRef しない約束）
    WCHAR name[128] = {};
    REFERENCE_TIME t_start = 0;
    int64_t run_qpc_us = 0;
    CamPin pin;
    HANDLE thread = NULL;
    volatile LONG stop = 0;
    volatile LONG rejected = 0;
    int stamp = 0; // 環境変数 POCKETGPU_VCAM_STAMP=1：サンプルの media time に「撮った時刻」を入れる（試験の道具が遅れを測る）
    // 絵
    ShmReader shm;
    uint8_t *fit = NULL; // 出す大きさの NV12
    size_t fit_cap = 0;
    // 数（記録用）
    uint64_t n_sent = 0, n_real = 0, n_wait = 0;
    // cam4k：1 コマの作る時間（µs。流す糸だけが触る）。読む＝共有メモリから写す・変換＝NV12 から出す形へ・全部＝render の始めから終わりまで
    double t_read_sum = 0, t_conv_sum = 0, t_all_sum = 0, t_all_max = 0;
    uint64_t n_ext = 0, n_timed = 0, n_late = 0, n_catchup = 0;
    int64_t rendered_seq = 0; // 最後に出した本物の絵の番号（共有メモリの seq）
    bool catching = false;
    // アプリが選んだ形の頼み（共有メモリの req に書く）
    int64_t my_req = 0;      // 書きたい頼み（0 は無し）
    int64_t my_written = 0;  // 書いた値（Stop で、まだ自分の物なら消す）
    bool req_pending = false;

    CamFilter() : pin(this) {
        InitializeCriticalSection(&lk);
        InterlockedIncrement(&g_objs);
        char v[8];
        stamp = GetEnvironmentVariableA("POCKETGPU_VCAM_STAMP", v, sizeof v) > 0 && v[0] == '1';
        // cam4k：4K を名乗るか（レジストリ）。このフィルタが生きている間は変えない
        const int maxh = read_max_height();
        pin.ntypes = NSUBS * (maxh >= 2160 ? NSIZES_ALL : NSIZES_BASE);
        shm.open_if_needed(wcam_shm_now_us()); // 既定の形（iPhone がいま送っている形）を一覧に出すため、先に開いておく
        logf("filter を作った（pid %lu・名乗る大きさ：%s）", GetCurrentProcessId(), maxh >= 2160 ? "4K まで" : "1080p まで");
    }
    // 既定の形：iPhone がいま送っている形が一覧にあればそれ（NV12）、無ければ 1280x720・30
    // lk の中で呼ぶ。流す糸が動いている間は、共有メモリを開くのはその糸に任せる（二重に開かない）
    void default_type(int *idx, REFERENCE_TIME *avg) {
        *idx = 0, *avg = AVG_DEFAULT;
        const int64_t now = wcam_shm_now_us();
        if (!thread) shm.open_if_needed(now);
        if (!shm.head || !shm.alive(now)) return;
        int64_t c = shm.head->cur;
        // cam4k：4K の口があれば、1920x1080 に収めない本当の形を見る（名乗る形に 4K が無ければ、収めた形のまま）
        if (pin.ntypes > NSUBS * NSIZES_BASE && shm.head->ext_state == 1 && shm.head->cur_full) c = shm.head->cur_full;
        if (!c) return;
        const int si = size_index((int) WCAM_REQ_W(c), (int) WCAM_REQ_H(c), pin.ntypes / NSUBS);
        const int fps = (int) WCAM_REQ_FPS(c);
        if (si < 0 || fps < 1 || fps > 60) return;
        *idx = si * NSUBS, *avg = avg_of(fps);
    }
    // 形 idx・avg が既定と違う（アプリが選んだ）か。色の形の違いは見ない
    bool differs_from_default(int idx, REFERENCE_TIME avg) {
        int di;
        REFERENCE_TIME da;
        default_type(&di, &da);
        return type_w(idx) != type_w(di) || type_h(idx) != type_h(di) || fps_of(avg) != fps_of(da);
    }
    // 頼みを共有メモリに書く・消す（lk の中で呼ぶ）
    void flush_request() {
        if (!req_pending || !shm.head || !shm.writable) return;
        req_pending = false;
        static volatile LONG serial;
        const int64_t r = my_req ? (my_req | ((int64_t) (InterlockedIncrement(&serial) & 0xffff) << 48)) : 0;
        if (r) {
            shm.head->req_pid = GetCurrentProcessId();
            shm.head->req_us = wcam_shm_now_us();
            MemoryBarrier();
            InterlockedExchange64((volatile LONGLONG *) &shm.head->req, r);
            my_written = r;
            logf("形を頼んだ：%ux%u・%u コマ/秒（書く側が%s）", WCAM_REQ_W(r), WCAM_REQ_H(r), WCAM_REQ_FPS(r), shm.head->writer_caps & WCAM_CAP_FOLLOW ? "読む" : "読まない古い版");
        }
    }
    void clear_request() {
        req_pending = false, my_req = 0;
        if (my_written && shm.head && shm.writable) {
            // まだ自分の頼みのままなら消す（ほかのアプリが後から頼んだ物は残す）
            if (InterlockedCompareExchange64((volatile LONGLONG *) &shm.head->req, 0, my_written) == my_written) logf("形の頼みを消した");
        }
        my_written = 0;
    }
    ~CamFilter() {
        EnterCriticalSection(&lk);
        clear_request();
        LeaveCriticalSection(&lk);
        stop_thread();
        pin.release_conn();
        delete_mt(NULL);
        if (clock) clock->Release();
        shm.close();
        if (fit) HeapFree(GetProcessHeap(), 0, fit);
        DeleteCriticalSection(&lk);
        logf("filter を消した（送ったコマ %llu・本物 %llu・待ち %llu）", (unsigned long long) n_sent, (unsigned long long) n_real, (unsigned long long) n_wait);
        InterlockedDecrement(&g_objs);
    }
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (eq(riid, kIID_IUnknown) || eq(riid, kIID_IPersist) || eq(riid, kIID_IMediaFilter) || eq(riid, kIID_IBaseFilter)) *ppv = static_cast<IBaseFilter *>(this);
        else if (eq(riid, kIID_IAMFilterMiscFlags)) *ppv = static_cast<IAMFilterMiscFlags *>(this);
        else if (eq(riid, kIID_IAMVideoControl)) *ppv = static_cast<IAMVideoControl *>(this);
        else { *ppv = NULL; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG) InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return (ULONG) r;
    }
    // IPersist
    STDMETHODIMP GetClassID(CLSID *c) override {
        if (!c) return E_POINTER;
        *c = kCLSID_Cam;
        return S_OK;
    }
    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME t) override;
    STDMETHODIMP GetState(DWORD, FILTER_STATE *st) override {
        if (!st) return E_POINTER;
        *st = state;
        return state == State_Paused ? VFW_S_CANT_CUE : S_OK; // 生の source は止めた絵を出せない
    }
    STDMETHODIMP SetSyncSource(IReferenceClock *c) override {
        EnterCriticalSection(&lk);
        if (c) c->AddRef();
        if (clock) clock->Release();
        clock = c;
        LeaveCriticalSection(&lk);
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock **c) override {
        if (!c) return E_POINTER;
        EnterCriticalSection(&lk);
        *c = clock;
        if (clock) clock->AddRef();
        LeaveCriticalSection(&lk);
        return S_OK;
    }
    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins **pp) override;
    STDMETHODIMP FindPin(LPCWSTR id, IPin **pp) override {
        if (!pp) return E_POINTER;
        if (id && !wcscmp(id, L"Capture")) { *pp = &pin; pin.AddRef(); return S_OK; }
        *pp = NULL;
        return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *fi) override {
        if (!fi) return E_POINTER;
        memcpy(fi->achName, name, sizeof name);
        fi->pGraph = graph;
        if (graph) graph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *g, LPCWSTR n) override {
        EnterCriticalSection(&lk);
        graph = g;
        memset(name, 0, sizeof name);
        if (n) wcsncpy(name, n, 127);
        LeaveCriticalSection(&lk);
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR *) override { return E_NOTIMPL; }
    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override { return 2; } // AM_FILTER_MISC_FLAGS_IS_SOURCE
    // IAMVideoControl：大きさごとのコマ数の一覧（30 と 60）。Chrome はこれが無いと、形の AvgTimePerFrame（30）だけを見る
    STDMETHODIMP GetCaps(IPin *, LONG *flags) override {
        if (!flags) return E_POINTER;
        *flags = 0; // 反転などの切り替えは持たない
        return S_OK;
    }
    STDMETHODIMP SetMode(IPin *, LONG mode) override { return mode == 0 ? S_OK : E_NOTIMPL; }
    STDMETHODIMP GetMode(IPin *, LONG *mode) override {
        if (!mode) return E_POINTER;
        *mode = 0;
        return S_OK;
    }
    STDMETHODIMP GetCurrentActualFrameRate(IPin *, LONGLONG *fr) override {
        if (!fr) return E_POINTER;
        EnterCriticalSection(&lk);
        HRESULT hr = pin.peer ? S_OK : VFW_E_NOT_CONNECTED;
        *fr = pin.peer ? pin.mt_avg : 0;
        LeaveCriticalSection(&lk);
        return hr;
    }
    STDMETHODIMP GetMaxAvailableFrameRate(IPin *, LONG index, SIZE dim, LONGLONG *fr) override {
        if (!fr) return E_POINTER;
        if (index < 0 || index >= pin.ntypes || dim.cx != type_w(index) || dim.cy != type_h(index)) return E_INVALIDARG;
        *fr = AVG_MIN;
        return S_OK;
    }
    STDMETHODIMP GetFrameRateList(IPin *, LONG index, SIZE dim, LONG *n, LONGLONG **list) override {
        if (!n) return E_POINTER;
        if (index < 0 || index >= pin.ntypes || dim.cx != type_w(index) || dim.cy != type_h(index)) return E_INVALIDARG;
        *n = 2;
        if (!list) return S_OK; // 数だけ
        LONGLONG *p = (LONGLONG *) CoTaskMemAlloc(sizeof(LONGLONG) * 2);
        if (!p) return E_OUTOFMEMORY;
        p[0] = AVG_DEFAULT, p[1] = AVG_MIN; // 30・60 コマ/秒（100 ns の単位の間隔）
        *list = p;
        return S_OK;
    }

    void stop_thread();
    static DWORD WINAPI thread_main(LPVOID p);
    void stream();
    long render(BYTE *dst, long cap, uint64_t *pts_us, int *real);
};

// ---- pin
STDMETHODIMP CamPin::QueryInterface(REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    if (eq(riid, kIID_IUnknown) || eq(riid, kIID_IPin)) *ppv = static_cast<IPin *>(this);
    else if (eq(riid, kIID_IAMStreamConfig)) *ppv = static_cast<IAMStreamConfig *>(this);
    else if (eq(riid, kIID_IKsPropertySet)) *ppv = static_cast<IKsPropertySet *>(this);
    else { *ppv = NULL; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}
STDMETHODIMP_(ULONG) CamPin::AddRef() { return f->AddRef(); }
STDMETHODIMP_(ULONG) CamPin::Release() { return f->Release(); }

void CamPin::release_conn() {
    if (alloc) { alloc->Decommit(); alloc->Release(); alloc = NULL; }
    if (mip) { mip->Release(); mip = NULL; }
    if (peer) { peer->Release(); peer = NULL; }
    free_mt_fields(&mt);
    memset(&mt, 0, sizeof mt);
    mt_idx = -1, out_stride = 0, conn_explicit = false;
}

HRESULT CamPin::decide_allocator() {
    HRESULT hr = peer->QueryInterface(kIID_IMemInputPin, (void **) &mip);
    if (FAILED(hr)) return hr;
    ALLOCATOR_PROPERTIES req = {}, got = {};
    mip->GetAllocatorRequirements(&req);
    ALLOCATOR_PROPERTIES want = {};
    want.cBuffers = req.cBuffers > 3 ? req.cBuffers : 3;
    want.cbBuffer = (long) mt.lSampleSize > req.cbBuffer ? (long) mt.lSampleSize : req.cbBuffer;
    want.cbAlign = req.cbAlign > 1 ? req.cbAlign : 1;
    want.cbPrefix = req.cbPrefix;
    IMemAllocator *a = NULL;
    // 下流の allocator を先に試し、だめなら DirectShow の標準の allocator
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0) hr = mip->GetAllocator(&a);
        else hr = CoCreateInstance(kCLSID_MemoryAllocator, NULL, CLSCTX_INPROC_SERVER, kIID_IMemAllocator, (void **) &a);
        if (FAILED(hr) || !a) continue;
        hr = a->SetProperties(&want, &got);
        if (SUCCEEDED(hr) && got.cbBuffer >= (long) mt.lSampleSize) {
            hr = mip->NotifyAllocator(a, FALSE);
            if (SUCCEEDED(hr)) { alloc = a; return S_OK; }
        }
        a->Release();
        a = NULL;
    }
    return FAILED(hr) ? hr : E_FAIL;
}

HRESULT CamPin::try_connect(IPin *recv, int idx, REFERENCE_TIME avg) {
    AM_MEDIA_TYPE t;
    if (FAILED(make_mt(idx, avg, &t))) return E_OUTOFMEMORY;
    HRESULT hr = recv->ReceiveConnection(this, &t);
    if (hr != S_OK) { free_mt_fields(&t); return FAILED(hr) ? hr : E_FAIL; }
    peer = recv;
    peer->AddRef();
    mt = t, mt_idx = idx, mt_avg = avg;
    hr = decide_allocator();
    if (FAILED(hr)) {
        logf("allocator が決まらない（0x%08lx）", (unsigned long) hr);
        recv->Disconnect();
        release_conn();
        return hr;
    }
    logf("つながった：%dx%d・%s・%.1f コマ/秒", type_w(idx), type_h(idx), idx % NSUBS == 0 ? "NV12" : idx % NSUBS == 1 ? "YUY2" : idx % NSUBS == 2 ? "I420" : idx % NSUBS == 3 ? "RGB32" : "RGB24",
         10000000.0 / (double) avg);
    return S_OK;
}

STDMETHODIMP CamPin::Connect(IPin *recv, const AM_MEDIA_TYPE *pmt) {
    if (!recv) return E_POINTER;
    EnterCriticalSection(&f->lk);
    HRESULT hr = VFW_E_NO_ACCEPTABLE_TYPES;
    if (peer) hr = VFW_E_ALREADY_CONNECTED;
    else if (f->state != State_Stopped) hr = VFW_E_NOT_STOPPED;
    else {
        PIN_DIRECTION d;
        if (FAILED(recv->QueryDirection(&d)) || d != PINDIR_INPUT) hr = VFW_E_INVALID_DIRECTION;
        else if (pmt && !eq(pmt->formattype, kGUID_NULL) && pmt->pbFormat) {
            // 全部決まった形：それだけを試す
            REFERENCE_TIME avg;
            int idx = match_mt(pmt, &avg, ntypes);
            bool ex = idx >= 0 && f->differs_from_default(idx, avg);
            hr = idx >= 0 ? try_connect(recv, idx, avg) : VFW_E_TYPE_NOT_ACCEPTED;
            if (hr == S_OK) conn_explicit = ex;
        } else {
            // SetFormat で決まっていればそれだけ。なければ既定の大きさ（iPhone がいま送っている形）の色の形を順に、そのあと一覧の順に
            int cand[MAXTYPES + NSUBS];
            REFERENCE_TIME cavg[MAXTYPES + NSUBS];
            int nc = 0;
            if (pref >= 0) cand[nc] = pref, cavg[nc++] = pref_avg;
            else {
                int di;
                REFERENCE_TIME da;
                f->default_type(&di, &da);
                for (int s = 0; s < NSUBS; s++) cand[nc] = di + s, cavg[nc++] = da;
                for (int k = 0; k < ntypes; k++)
                    if (k < di || k >= di + NSUBS) cand[nc] = k, cavg[nc++] = AVG_DEFAULT;
            }
            for (int c = 0; c < nc; c++) {
                const int idx = cand[c];
                if (pmt && !eq(pmt->majortype, kGUID_NULL) && !eq(pmt->majortype, kMEDIATYPE_Video)) break;
                if (pmt && !eq(pmt->subtype, kGUID_NULL) && !eq(pmt->subtype, *type_sub(idx).guid)) continue;
                if (try_connect(recv, idx, cavg[c]) == S_OK) { hr = S_OK; conn_explicit = pref >= 0 && pref_explicit; break; }
            }
        }
    }
    LeaveCriticalSection(&f->lk);
    if (hr != S_OK) logf("Connect に失敗（0x%08lx）", (unsigned long) hr);
    return hr;
}

STDMETHODIMP CamPin::Disconnect() {
    EnterCriticalSection(&f->lk);
    HRESULT hr;
    if (f->state != State_Stopped) hr = VFW_E_NOT_STOPPED;
    else if (!peer) hr = S_FALSE;
    else { release_conn(); hr = S_OK; }
    LeaveCriticalSection(&f->lk);
    return hr;
}

STDMETHODIMP CamPin::ConnectedTo(IPin **pp) {
    if (!pp) return E_POINTER;
    EnterCriticalSection(&f->lk);
    *pp = peer;
    if (peer) peer->AddRef();
    LeaveCriticalSection(&f->lk);
    return *pp ? S_OK : VFW_E_NOT_CONNECTED;
}

STDMETHODIMP CamPin::ConnectionMediaType(AM_MEDIA_TYPE *pmt) {
    if (!pmt) return E_POINTER;
    EnterCriticalSection(&f->lk);
    HRESULT hr = peer ? copy_mt(pmt, &mt) : (memset(pmt, 0, sizeof *pmt), VFW_E_NOT_CONNECTED);
    LeaveCriticalSection(&f->lk);
    return hr;
}

STDMETHODIMP CamPin::QueryPinInfo(PIN_INFO *pi) {
    if (!pi) return E_POINTER;
    memset(pi, 0, sizeof *pi);
    pi->pFilter = f;
    f->AddRef();
    pi->dir = PINDIR_OUTPUT;
    wcscpy(pi->achName, L"Capture");
    return S_OK;
}

// ---- 形の一覧
class EnumTypes final : public IEnumMediaTypes {
public:
    volatile LONG ref = 1;
    CamPin *pin;
    int only, pos;
    REFERENCE_TIME only_avg;
    int def;                // 既定の大きさの先頭（NV12）の番号。この大きさの色の形を先に並べる
    REFERENCE_TIME def_avg;
    EnumTypes(CamPin *p, int o, REFERENCE_TIME oa, int ps, int d, REFERENCE_TIME da) : pin(p), only(o), pos(ps), only_avg(oa), def(d), def_avg(da) {
        pin->AddRef();
        InterlockedIncrement(&g_objs);
    }
    ~EnumTypes() {
        pin->Release();
        InterlockedDecrement(&g_objs);
    }
    int count() const { return only >= 0 ? 1 : pin->ntypes; }
    // 並べる順の p 番目の形
    int at(int p, REFERENCE_TIME *avg) const {
        if (only >= 0) { *avg = only_avg; return only; }
        if (p < NSUBS) { *avg = def_avg; return def + p; }
        const int k = p - NSUBS;
        *avg = AVG_DEFAULT;
        return k < def ? k : k + NSUBS;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (eq(riid, kIID_IUnknown) || eq(riid, kIID_IEnumMediaTypes)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG) InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return (ULONG) r;
    }
    STDMETHODIMP Next(ULONG n, AM_MEDIA_TYPE **out, ULONG *got) override {
        if (!out || (n > 1 && !got)) return E_POINTER;
        ULONG k = 0;
        while (k < n && pos < count()) {
            AM_MEDIA_TYPE t;
            REFERENCE_TIME avg;
            int idx = at(pos, &avg);
            if (FAILED(make_mt(idx, avg, &t))) break;
            out[k] = dup_mt(&t);
            free_mt_fields(&t);
            if (!out[k]) break;
            k++, pos++;
        }
        if (got) *got = k;
        return k == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) override {
        pos += (int) n;
        if (pos > count()) { pos = count(); return S_FALSE; }
        return S_OK;
    }
    STDMETHODIMP Reset() override {
        pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumMediaTypes **pp) override {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) EnumTypes(pin, only, only_avg, pos, def, def_avg);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }
};

STDMETHODIMP CamPin::EnumMediaTypes(IEnumMediaTypes **pp) {
    if (!pp) return E_POINTER;
    EnterCriticalSection(&f->lk);
    int di;
    REFERENCE_TIME da;
    f->default_type(&di, &da);
    *pp = new (std::nothrow) EnumTypes(this, pref, pref_avg, 0, di, da);
    LeaveCriticalSection(&f->lk);
    return *pp ? S_OK : E_OUTOFMEMORY;
}

// ---- IAMStreamConfig
STDMETHODIMP CamPin::SetFormat(AM_MEDIA_TYPE *pmt) {
    EnterCriticalSection(&f->lk);
    HRESULT hr = S_OK;
    if (!pmt) { pref = -1; pref_avg = AVG_DEFAULT; pref_explicit = false; } // 既定に戻す
    else {
        REFERENCE_TIME avg;
        int idx = match_mt(pmt, &avg, ntypes);
        if (idx >= 0) pref_explicit = f->differs_from_default(idx, avg);
        if (idx < 0) hr = VFW_E_INVALIDMEDIATYPE;
        else if (peer && (idx != mt_idx || avg != mt_avg)) {
            // つながったあとで形が変わる：止まっていて、下流が受けられるなら、graph につなぎ直してもらう
            if (f->state != State_Stopped) hr = VFW_E_NOT_STOPPED;
            else {
                AM_MEDIA_TYPE t;
                make_mt(idx, avg, &t);
                hr = peer->QueryAccept(&t) == S_OK ? S_OK : VFW_E_INVALIDMEDIATYPE;
                free_mt_fields(&t);
                if (hr == S_OK) {
                    pref = idx, pref_avg = avg;
                    IFilterGraph *g = f->graph;
                    LeaveCriticalSection(&f->lk);
                    if (g) g->Reconnect(this);
                    logf("SetFormat（つなぎ直し）：%d 番", idx);
                    return S_OK;
                }
            }
        } else { pref = idx, pref_avg = avg; }
        if (hr == S_OK) logf("SetFormat：%dx%d %d 番 %.1f コマ/秒（%s）", type_w(idx), type_h(idx), idx, 10000000.0 / (double) avg, pref_explicit ? "既定と違う＝アプリが選んだ" : "既定と同じ");
    }
    LeaveCriticalSection(&f->lk);
    return hr;
}

STDMETHODIMP CamPin::GetFormat(AM_MEDIA_TYPE **pp) {
    if (!pp) return E_POINTER;
    EnterCriticalSection(&f->lk);
    AM_MEDIA_TYPE t;
    HRESULT hr;
    if (peer) hr = copy_mt(&t, &mt);
    else if (pref >= 0) hr = make_mt(pref, pref_avg, &t);
    else {
        int di;
        REFERENCE_TIME da;
        f->default_type(&di, &da);
        hr = make_mt(di, da, &t);
    }
    LeaveCriticalSection(&f->lk);
    if (FAILED(hr)) return hr;
    *pp = dup_mt(&t);
    free_mt_fields(&t);
    return *pp ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CamPin::GetStreamCaps(int i, AM_MEDIA_TYPE **pp, BYTE *caps) {
    if (!pp || !caps) return E_POINTER;
    if (i < 0 || i >= ntypes) return S_FALSE;
    AM_MEDIA_TYPE t;
    HRESULT hr = make_mt(i, AVG_DEFAULT, &t);
    if (FAILED(hr)) return hr;
    *pp = dup_mt(&t);
    free_mt_fields(&t);
    if (!*pp) return E_OUTOFMEMORY;
    VIDEO_STREAM_CONFIG_CAPS *c = (VIDEO_STREAM_CONFIG_CAPS *) caps;
    memset(c, 0, sizeof *c);
    const int w = type_w(i), h = type_h(i);
    const long size = image_size(type_sub(i).kind, w, h);
    c->guid = kFORMAT_VideoInfo;
    c->InputSize.cx = c->MinCroppingSize.cx = c->MaxCroppingSize.cx = c->MinOutputSize.cx = c->MaxOutputSize.cx = w;
    c->InputSize.cy = c->MinCroppingSize.cy = c->MaxCroppingSize.cy = c->MinOutputSize.cy = c->MaxOutputSize.cy = h;
    c->CropGranularityX = c->CropGranularityY = c->CropAlignX = c->CropAlignY = 1;
    c->OutputGranularityX = c->OutputGranularityY = 1;
    c->MinFrameInterval = AVG_MIN, c->MaxFrameInterval = AVG_MAX;
    c->MinBitsPerSecond = (LONG) (size * 8), c->MaxBitsPerSecond = (LONG) ((int64_t) size * 8 * 60 > 0x7fffffff ? 0x7fffffff : size * 8 * 60);
    return S_OK;
}

// ---- 1 本のピンの一覧
class EnumPinsImpl final : public IEnumPins {
public:
    volatile LONG ref = 1;
    CamFilter *f;
    int pos;
    EnumPinsImpl(CamFilter *o, int p) : f(o), pos(p) {
        f->AddRef();
        InterlockedIncrement(&g_objs);
    }
    ~EnumPinsImpl() {
        f->Release();
        InterlockedDecrement(&g_objs);
    }
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (eq(riid, kIID_IUnknown) || eq(riid, kIID_IEnumPins)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG) InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return (ULONG) r;
    }
    STDMETHODIMP Next(ULONG n, IPin **out, ULONG *got) override {
        if (!out || (n > 1 && !got)) return E_POINTER;
        ULONG k = 0;
        if (n && pos < 1) { out[0] = &f->pin; f->pin.AddRef(); pos++, k++; }
        if (got) *got = k;
        return k == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) override {
        pos += (int) n;
        if (pos > 1) { pos = 1; return S_FALSE; }
        return S_OK;
    }
    STDMETHODIMP Reset() override {
        pos = 0;
        return S_OK;
    }
    STDMETHODIMP Clone(IEnumPins **pp) override {
        if (!pp) return E_POINTER;
        *pp = new (std::nothrow) EnumPinsImpl(f, pos);
        return *pp ? S_OK : E_OUTOFMEMORY;
    }
};

STDMETHODIMP CamFilter::EnumPins(IEnumPins **pp) {
    if (!pp) return E_POINTER;
    *pp = new (std::nothrow) EnumPinsImpl(this, 0);
    return *pp ? S_OK : E_OUTOFMEMORY;
}

// ---- 状態
void CamFilter::stop_thread() {
    if (!thread) return;
    InterlockedExchange(&stop, 1);
    if (pin.alloc) pin.alloc->Decommit(); // GetBuffer で待っていたら起こす
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) logf("糸が 5 秒で止まらない");
    CloseHandle(thread);
    thread = NULL;
}

STDMETHODIMP CamFilter::Stop() {
    EnterCriticalSection(&lk);
    FILTER_STATE was = state;
    state = State_Stopped;
    clear_request(); // アプリが止めた：自分の頼みを消す（webcam_rx はおまかせの形に戻す）
    LeaveCriticalSection(&lk);
    stop_thread(); // 糸は lk を取るので、外で待つ
    if (pin.alloc) pin.alloc->Decommit();
    if (was != State_Stopped) {
        logf("Stop（送ったコマ %llu・本物 %llu・待ち %llu）", (unsigned long long) n_sent, (unsigned long long) n_real, (unsigned long long) n_wait);
        if (n_timed)
            logf("  1 コマを作る時間（本物の %llu コマ）：平均 %.2f ms（うち色の変換 %.2f ms）・最大 %.2f ms・間隔 %.1f ms より長くかかったコマ %llu・4K の口から読んだコマ %llu・1 コマ遅れから追いついたコマ %llu",
                 (unsigned long long) n_timed, t_all_sum / (double) n_timed / 1000.0, t_conv_sum / (double) n_timed / 1000.0, t_all_max / 1000.0, (double) pin.mt_avg / 10000.0, (unsigned long long) n_late,
                 (unsigned long long) n_ext, (unsigned long long) n_catchup);
    }
    return S_OK;
}

STDMETHODIMP CamFilter::Pause() {
    EnterCriticalSection(&lk);
    HRESULT hr = S_OK;
    if (state == State_Stopped && pin.peer) {
        hr = pin.alloc ? pin.alloc->Commit() : E_FAIL;
        if (SUCCEEDED(hr)) {
            InterlockedExchange(&stop, 0);
            thread = CreateThread(NULL, 0, thread_main, this, 0, NULL);
            if (!thread) hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) state = State_Paused;
    LeaveCriticalSection(&lk);
    return SUCCEEDED(hr) ? VFW_S_CANT_CUE : hr;
}

STDMETHODIMP CamFilter::Run(REFERENCE_TIME t) {
    if (state == State_Stopped) {
        HRESULT hr = Pause();
        if (FAILED(hr)) return hr;
    }
    EnterCriticalSection(&lk);
    t_start = t;
    run_qpc_us = wcam_shm_now_us();
    InterlockedExchange(&rejected, 0);
    const bool was_running = state == State_Running;
    state = State_Running;
    // アプリが選んだ形なら、iPhone に頼む（書くのは流す糸。共有メモリがまだ無ければ、開けたときに書く）。
    // 1280x720 より小さい大きさは頼まない（コマ数だけ頼む）。iPhone からは 720p 以上で受けて、ここで縮める
    if (!was_running && pin.peer && pin.conn_explicit) {
        const int w = type_w(pin.mt_idx), h = type_h(pin.mt_idx), fps = fps_of(pin.mt_avg);
        const bool big = w >= 1280 && h >= 720;
        my_req = WCAM_REQ_MAKE(big ? w : 0, big ? h : 0, fps, 0, 0);
        req_pending = true;
    }
    LeaveCriticalSection(&lk);
    logf("Run%s", pin.conn_explicit ? "（アプリが選んだ形）" : "");
    return S_OK;
}

// ---- 流す糸
DWORD WINAPI CamFilter::thread_main(LPVOID p) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ((CamFilter *) p)->stream();
    CoUninitialize();
    return 0;
}

// 1 コマ作る。返すのは長さ。*real は本物の絵なら 1
long CamFilter::render(BYTE *dst, long cap, uint64_t *pts_us, int *real) {
    const int64_t t_begin = wcam_shm_now_us();
    const int idx = pin.mt_idx;
    const int w = type_w(idx), h = type_h(idx);
    const Sub &s = type_sub(idx);
    const long need = image_size(s.kind, w, h);
    long stride = s.kind == K_YUY2 ? (long) w * 2 : s.kind == K_RGB32 ? rgb_stride(w, 32) : s.kind == K_RGB24 ? rgb_stride(w, 24) : w;
    if (pin.out_stride > stride) stride = pin.out_stride;
    const long total = s.kind == K_NV12 || s.kind == K_I420 ? stride * h * 3 / 2 : stride * h;
    if (cap < total || cap < need) return 0;
    const size_t nv = (size_t) w * h * 3 / 2;
    if (fit_cap < nv) {
        if (fit) HeapFree(GetProcessHeap(), 0, fit);
        fit = (uint8_t *) HeapAlloc(GetProcessHeap(), 0, nv);
        fit_cap = fit ? nv : 0;
        if (!fit) return 0;
    }
    // NV12 なら dst に直接、それ以外は fit（w×h の NV12）に作ってから直す
    uint8_t *ty = s.kind == K_NV12 ? dst : fit, *tuv = s.kind == K_NV12 ? dst + (size_t) stride * h : fit + (size_t) w * h;
    const long tp = s.kind == K_NV12 ? stride : w;
    const int64_t now = wcam_shm_now_us();
    *real = 0;
    bool direct = false; // 共有メモリから直接、出す形に変えた（fit に写していない）
    double t_conv = 0;
    if (shm.open_if_needed(now) && shm.alive(now)) {
        const wcam_shm_head *hd = shm.head;
        for (int tries = 0; tries < 2 && !*real; tries++) {
            int64_t seq = hd->seq;
            MemoryBarrier();
            if (seq <= 0) break;
            // cam4k：書く側の絵が 2 つ続けて出る（デコーダは 1 コマ遅れて 2 つまとめて出すことがある。4K60 で毎秒 2 回ほど）ときは、新しい方だけ取ると 1 つ捨てて、次に同じ絵を 2 回出してしまう。
            // ちょうど 1 コマ遅れているだけなら、1 つ古い方（まだ枠に残っている）を先に出し、追いつくまでは間隔を詰めて出す。2 コマ以上遅れたら最新に飛ぶ
            if (rendered_seq > 0 && seq - rendered_seq == 2) { seq = rendered_seq + 1; catching = true; } else catching = false;
            const wcam_shm_slot *sl = &hd->slot[seq % WCAM_SHM_SLOTS];
            if (sl->seq != seq) continue;
            // cam4k：枠が「本物は 4K の口にある」と言っていて、4K の口を開けるなら、そちらを読む。開けなければ（前の版の書く側・まだ開けない）枠の縮めた絵を使う
            const wcam_shm_slot *esl = NULL;
            const uint8_t *src;
            int sw, sh;
            if ((sl->flags & WCAM_SLOT_EXT) && shm.ext_ready(now)) {
                esl = &shm.ext->slot[seq % WCAM_EXT_SLOTS];
                if (esl->seq != seq) continue;
                sw = (int) esl->width, sh = (int) esl->height;
                if (sw < 2 || sh < 2 || sw > (int) WCAM_EXT_MAXW || sh > (int) WCAM_EXT_MAXH) break;
                src = WCAM_EXT_FRAME(shm.ext, seq % WCAM_EXT_SLOTS);
            } else {
                sw = (int) sl->width, sh = (int) sl->height;
                if (sw < 2 || sh < 2 || sw > (int) WCAM_SHM_MAXW || sh > (int) WCAM_SHM_MAXH) break;
                src = WCAM_SHM_FRAME(hd, seq % WCAM_SHM_SLOTS);
            }
            const uint64_t pts = sl->pts_us;
            if (s.kind != K_NV12 && sw == w && sh == h) {
                // 出す大きさと同じ：fit に写さず、共有メモリから直接 YUY2・I420・RGB に変える（4K で 12 MB の写しを 1 回減らす）
                const int64_t c0 = wcam_shm_now_us();
                convert_out(s.kind, src, w, h, dst, stride);
                t_conv = (double) (wcam_shm_now_us() - c0);
                direct = true;
            } else {
                nv12_fit(ty, tuv, tp, w, h, src, sw, sh);
                direct = false;
            }
            MemoryBarrier();
            // 写している間に書き換わっていなければ使う
            if (sl->seq == seq && (!esl || esl->seq == seq)) { *real = 1, *pts_us = pts, rendered_seq = seq; if (esl) n_ext++; if (catching) n_catchup++; }
        }
    } else if (shm.head && !shm.alive(now)) {
        // 書く側が終わった：開いたままにしておく（また始まれば同じ物が使われる）
    }
    if (!*real) { nv12_waiting(ty, tuv, tp, w, h, (uint64_t) (now / 1000)); direct = false; }
    if (s.kind != K_NV12 && !direct) {
        const int64_t c0 = wcam_shm_now_us();
        convert_out(s.kind, fit, w, h, dst, stride);
        t_conv = (double) (wcam_shm_now_us() - c0);
    }
    if (*real) {
        // 記録用の時間（本物の絵のときだけ）
        const double all = (double) (wcam_shm_now_us() - t_begin);
        t_all_sum += all, t_conv_sum += t_conv, n_timed++;
        if (all > t_all_max) t_all_max = all;
        if (all > (double) pin.mt_avg / 10.0) n_late++; // 1 コマの間隔より長くかかった
    }
    return total;
}

void CamFilter::stream() {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    int64_t last_us = 0, last_seq = 0;
    uint64_t n = 0;
    logf("流し始め");
    while (!stop) {
        if (state != State_Running || rejected) {
            LARGE_INTEGER due;
            due.QuadPart = -100000; // 10 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 50);
            continue;
        }
        if (req_pending) {
            EnterCriticalSection(&lk);
            if (!shm.head) shm.open_if_needed(wcam_shm_now_us());
            flush_request();
            LeaveCriticalSection(&lk);
        }
        const int64_t iv = pin.mt_avg / 10; // µs
        const int64_t now = wcam_shm_now_us();
        const int64_t since = now - last_us;
        // 新しいコマが来ていれば早めに出す（遅れを減らす）。来なくても間が空きすぎないように同じ絵か待ちの絵を出す
        int64_t seq = (shm.head && shm.alive(now)) ? shm.head->seq : 0;
        bool fresh = seq > last_seq;
        bool due_now = last_us == 0 || (fresh && since >= iv * 8 / 10) || since >= (seq ? iv * 3 / 2 : iv);
        if (!due_now) {
            LARGE_INTEGER due;
            due.QuadPart = -20000; // 2 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 20);
            if (!shm.head) shm.open_if_needed(wcam_shm_now_us());
            continue;
        }
        IMediaSample *smp = NULL;
        HRESULT hr = pin.alloc ? pin.alloc->GetBuffer(&smp, NULL, NULL, 0) : E_FAIL;
        if (FAILED(hr) || !smp) {
            if (stop) break;
            Sleep(5);
            continue;
        }
        AM_MEDIA_TYPE *nmt = NULL;
        if (smp->GetMediaType(&nmt) == S_OK && nmt) {
            // 下流が行の幅などを変えてきた（動的な形の変更）。同じ形の中で幅だけ受ける
            if (eq(nmt->formattype, kFORMAT_VideoInfo) && nmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                const VIDEOINFOHEADER *v = (const VIDEOINFOHEADER *) nmt->pbFormat;
                const Sub &s = type_sub(pin.mt_idx);
                long st = s.kind == K_YUY2 ? v->bmiHeader.biWidth * 2 : s.kind == K_RGB32 || s.kind == K_RGB24 ? rgb_stride(v->bmiHeader.biWidth, s.bpp) : v->bmiHeader.biWidth;
                pin.out_stride = st;
                logf("下流が形を変えた：幅 %ld", (long) v->bmiHeader.biWidth);
            }
            delete_mt(nmt);
        }
        BYTE *p = NULL;
        smp->GetPointer(&p);
        uint64_t pts = 0;
        int real = 0;
        long len = p ? render(p, smp->GetSize(), &pts, &real) : 0;
        if (real) last_seq = rendered_seq > last_seq ? rendered_seq : last_seq;
        smp->SetActualDataLength(len);
        REFERENCE_TIME rs = 0, now_rt = 0;
        EnterCriticalSection(&lk);
        IReferenceClock *c = clock;
        if (c) c->AddRef();
        REFERENCE_TIME ts = t_start;
        int64_t rq = run_qpc_us;
        LeaveCriticalSection(&lk);
        if (c && SUCCEEDED(c->GetTime(&now_rt))) rs = now_rt - ts;
        else rs = (REFERENCE_TIME) (wcam_shm_now_us() - rq) * 10;
        if (c) c->Release();
        if (rs < 0) rs = 0;
        REFERENCE_TIME re = rs + pin.mt_avg;
        smp->SetTime(&rs, &re);
        if (stamp && real) {
            LONGLONG ms0 = (LONGLONG) pts, ms1 = (LONGLONG) pts + 1;
            smp->SetMediaTime(&ms0, &ms1);
        } else {
            LONGLONG ms0 = (LONGLONG) n, ms1 = (LONGLONG) n + 1;
            smp->SetMediaTime(&ms0, &ms1);
        }
        smp->SetSyncPoint(TRUE);
        smp->SetDiscontinuity(n == 0);
        smp->SetPreroll(FALSE);
        hr = len ? pin.mip->Receive(smp) : S_OK;
        smp->Release();
        last_us = now;
        n++, n_sent++;
        if (real) n_real++;
        else n_wait++;
        if (hr != S_OK) {
            logf("下流が受けない（0x%08lx）。次の Run まで止める", (unsigned long) hr);
            InterlockedExchange(&rejected, 1);
        }
    }
    CloseHandle(timer);
    logf("流し終わり（%llu コマ）", (unsigned long long) n);
}

// ---------------------------------------------------------------- COM の入口
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (eq(riid, kIID_IUnknown) || eq(riid, kIID_IClassFactory)) { *ppv = this; return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        CamFilter *f = new (std::nothrow) CamFilter();
        if (!f) return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL l) override {
        if (l) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }
};
static Factory g_factory;

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) { g_mod = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

extern "C" HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID c, REFIID riid, void **ppv) {
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!eq(c, kCLSID_Cam)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

extern "C" HRESULT STDAPICALLTYPE DllCanUnloadNow(void) { return g_objs == 0 && g_locks == 0 ? S_OK : S_FALSE; }

// ---------------------------------------------------------------- 登録（HKCU だけ。管理者の権限は要らない）
#if defined(_WIN64)
#define VIEW KEY_WOW64_64KEY
#else
#define VIEW KEY_WOW64_32KEY
#endif
#define K_CLSID L"Software\\Classes\\CLSID\\" CAM_CLSID_STR
#define K_CAT L"Software\\Classes\\CLSID\\" CAT_STR
#define K_INST K_CAT L"\\Instance\\" CAM_CLSID_STR

// FilterData：REGFILTER2 の版 2。merit = MERIT_DO_NOT_USE・出口のピン 1 本・形は video（細かい形は書かない）。
// 既にある仮想カメラ（OBS・Unity Capture）が IFilterMapper2 で書いた物と同じ並び
static const BYTE FILTER_DATA[] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 版 2・merit 0x200000・ピン 1・予約
    '0', 'p', 'i', '3', 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,     // ピン：REG_PINFLAG_B_OUTPUT・形 1
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                                 // medium 0・category なし
    '0', 't', 'y', '3', 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00,     // 形：major は 0x38・minor は 0x48
    0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71, // MEDIATYPE_Video
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // GUID_NULL
};
static_assert(sizeof FILTER_DATA == 0x58, "FilterData は 88 バイト");

static LONG set_str(HKEY k, const wchar_t *name, const wchar_t *v) { return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *) v, (DWORD) ((wcslen(v) + 1) * sizeof(wchar_t))); }

static LONG del_if_empty(const wchar_t *path) {
    HKEY k;
    LONG r = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | VIEW, &k);
    if (r != ERROR_SUCCESS) return r;
    DWORD nsub = 0, nval = 0;
    r = RegQueryInfoKeyW(k, NULL, NULL, NULL, &nsub, NULL, NULL, &nval, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || nsub || nval) return ERROR_DIR_NOT_EMPTY;
    return RegDeleteKeyExW(HKEY_CURRENT_USER, path, VIEW, 0);
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
    HKEY base;
    // RegDeleteTree は view を受けないので、view を付けて開いた親から消す
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID", 0, KEY_ALL_ACCESS | VIEW, &base) == ERROR_SUCCESS) {
        RegDeleteTreeW(base, CAM_CLSID_STR);
        RegDeleteTreeW(base, CAT_STR L"\\Instance\\" CAM_CLSID_STR);
        RegCloseKey(base);
    }
    // HKCU に元から無かった親（Video Capture Sources の CLSID と Instance）は、空なら消す
    del_if_empty(K_CAT L"\\Instance");
    del_if_empty(K_CAT);
    return S_OK;
}

extern "C" HRESULT STDAPICALLTYPE DllRegisterServer(void) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_mod, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return E_FAIL;
    HKEY k = NULL;
    LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, K_CLSID, 0, NULL, 0, KEY_WRITE | VIEW, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    set_str(k, NULL, CAM_NAME);
    RegCloseKey(k);
    r = RegCreateKeyExW(HKEY_CURRENT_USER, K_CLSID L"\\InprocServer32", 0, NULL, 0, KEY_WRITE | VIEW, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) { DllUnregisterServer(); return HRESULT_FROM_WIN32(r); }
    set_str(k, NULL, path);
    set_str(k, L"ThreadingModel", L"Both");
    RegCloseKey(k);
    r = RegCreateKeyExW(HKEY_CURRENT_USER, K_INST, 0, NULL, 0, KEY_WRITE | VIEW, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) { DllUnregisterServer(); return HRESULT_FROM_WIN32(r); }
    set_str(k, L"FriendlyName", CAM_NAME);
    set_str(k, L"CLSID", CAM_CLSID_STR);
    RegSetValueExW(k, L"FilterData", 0, REG_BINARY, FILTER_DATA, sizeof FILTER_DATA);
    RegCloseKey(k);
    return S_OK;
}
