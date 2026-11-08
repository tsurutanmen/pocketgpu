// 窓に出すコマを H.264 で運ぶための小さな道具。受け手（server.c）とドライバ（icd.c）の両方で使う。
// 画素の並べ替え（BGRA と NV12）は、受け手で縮める前とドライバでほどいた後で同じ式を使う。
// 色の式は BT.709 の狭い幅（Y は 16〜235）。H.264 の HD でふつうの物
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// BGRA（bgra が 0 なら RGBA）の w×h を NV12 にする。y は Y の面（1 行 ys バイト）、uv は U と V を交互に並べた半分の面。
// 奇数の幅・高さは、最後の列・行を繰り返して偶数にする（NV12 の大きさは偶数に切り上げた物）
static inline void vid_rgb_to_nv12(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra,
                                   uint8_t *y, uint32_t ys, uint8_t *uv, uint32_t uvs) {
    const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    const uint32_t ew = (w + 1) & ~1u, eh = (h + 1) & ~1u;
    for (uint32_t j = 0; j < eh; j += 2) {
        const uint8_t *s0 = px + (size_t) (j < h ? j : h - 1) * stride;
        const uint8_t *s1 = px + (size_t) (j + 1 < h ? j + 1 : h - 1) * stride;
        uint8_t *y0 = y + (size_t) j * ys, *y1 = y0 + ys, *c = uv + (size_t) (j / 2) * uvs;
        for (uint32_t i = 0; i < ew; i += 2) {
            const uint32_t a = i * 4, b = (i + 1 < w ? i + 1 : w - 1) * 4;
            const uint8_t *p0 = s0 + a, *p1 = s0 + b, *p2 = s1 + a, *p3 = s1 + b;
            y0[i] = (uint8_t) (((47 * p0[ri] + 157 * p0[1] + 16 * p0[bi] + 128) >> 8) + 16);
            y0[i + 1] = (uint8_t) (((47 * p1[ri] + 157 * p1[1] + 16 * p1[bi] + 128) >> 8) + 16);
            y1[i] = (uint8_t) (((47 * p2[ri] + 157 * p2[1] + 16 * p2[bi] + 128) >> 8) + 16);
            y1[i + 1] = (uint8_t) (((47 * p3[ri] + 157 * p3[1] + 16 * p3[bi] + 128) >> 8) + 16);
            // 色は 4 つの画素の平均から作る
            const int r = (p0[ri] + p1[ri] + p2[ri] + p3[ri] + 2) >> 2;
            const int g = (p0[1] + p1[1] + p2[1] + p3[1] + 2) >> 2;
            const int bb = (p0[bi] + p1[bi] + p2[bi] + p3[bi] + 2) >> 2;
            c[i] = (uint8_t) ((-26 * r - 86 * g + 112 * bb + 32896) >> 8);
            c[i + 1] = (uint8_t) ((112 * r - 102 * g - 10 * bb + 32896) >> 8);
        }
    }
}

static inline uint8_t vid_clamp10(int v) { return v <= 0 ? 0 : v >= (255 << 10) ? 255 : (uint8_t) (v >> 10); }

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

// 色（U・V）を 1 行ぶん、画素ごとに戻す。色の標本は 2x2 の真ん中にあるので、近い標本と遠い標本を縦横とも 3:1 で混ぜる
// （そのまま 2x2 に広げるより、色の境目がなめらかになる。d3d11cube の 1 枚で 35.4→37.4 dB）。
// ch は色の行の数。vc は作業用（色の 1 行ぶんと前後に 1 組ずつ）。cp に画素ごとの U・V の組（128 を引いた値）を入れる
static inline void vid_chroma_row(const uint8_t *uv, uint32_t uvs, uint32_t ch, uint32_t j, uint32_t w, int16_t *vc, int16_t *cp) {
    const uint32_t m = j / 2, cw = (w + 1) / 2;
    const uint32_t nr = m < ch ? m : ch - 1, fr = (j & 1) ? (m + 1 < ch ? m + 1 : ch - 1) : (m > 0 ? m - 1 : 0);
    const uint8_t *a = uv + (size_t) nr * uvs, *b = uv + (size_t) fr * uvs;
    int16_t *c = vc + 2;
    for (uint32_t x = 0; x < cw * 2; x++) c[x] = (int16_t) (3 * a[x] + b[x]);
    c[-2] = c[0], c[-1] = c[1], c[cw * 2] = c[cw * 2 - 2], c[cw * 2 + 1] = c[cw * 2 - 1]; // 端は同じ標本を繰り返す
    uint32_t k = 0;
#if defined(__SSE2__)
    const __m128i k8 = _mm_set1_epi16(8), k128 = _mm_set1_epi16(128);
    for (; k + 4 <= cw; k += 4) {
        const __m128i cur = _mm_loadu_si128((const __m128i *) (c + 2 * k));
        const __m128i c3 = _mm_add_epi16(cur, _mm_add_epi16(cur, cur));
        const __m128i ev = _mm_sub_epi16(_mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(c3, _mm_loadu_si128((const __m128i *) (c + 2 * k - 2))), k8), 4), k128);
        const __m128i od = _mm_sub_epi16(_mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(c3, _mm_loadu_si128((const __m128i *) (c + 2 * k + 2))), k8), 4), k128);
        _mm_storeu_si128((__m128i *) (cp + 4 * k), _mm_unpacklo_epi32(ev, od));
        _mm_storeu_si128((__m128i *) (cp + 4 * k + 8), _mm_unpackhi_epi32(ev, od));
    }
#endif
    for (; k < cw; k++)
        for (int q = 0; q < 2; q++) {
            cp[4 * k + q] = (int16_t) (((3 * c[2 * k + q] + c[2 * k - 2 + q] + 8) >> 4) - 128);
            cp[4 * k + 2 + q] = (int16_t) (((3 * c[2 * k + q] + c[2 * k + 2 + q] + 8) >> 4) - 128);
        }
}

#if defined(__SSE2__)
// 8 画素ずつ SSE2 で（下の 1 画素ずつの式と同じ値になる）。進めた画素の数を返す
static inline uint32_t vid_row_sse2(const uint8_t *yr, const int16_t *cp, uint32_t w, uint8_t *o) {
    const __m128i z = _mm_setzero_si128(), k16 = _mm_set1_epi16(16), half = _mm_set1_epi32(512), a = _mm_set1_epi8((char) 255);
    const __m128i kb = _mm_set_epi16(2163, 1192, 2163, 1192, 2163, 1192, 2163, 1192);
    const __m128i kr = _mm_set_epi16(1836, 1192, 1836, 1192, 1836, 1192, 1836, 1192);
    const __m128i kg = _mm_set_epi16(-218, 1192, -218, 1192, -218, 1192, -218, 1192), kgv = _mm_set_epi16(0, -546, 0, -546, 0, -546, 0, -546);
    uint32_t i = 0;
    for (; i + 8 <= w; i += 8, o += 32) {
        const __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *) (yr + i)), z), k16);
        const __m128i p0 = _mm_loadu_si128((const __m128i *) (cp + 2 * i)), p1 = _mm_loadu_si128((const __m128i *) (cp + 2 * i + 8));
        const __m128i u = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(p0, 16), 16), _mm_srai_epi32(_mm_slli_epi32(p1, 16), 16));
        const __m128i v = _mm_packs_epi32(_mm_srai_epi32(p0, 16), _mm_srai_epi32(p1, 16));
        const __m128i yul = _mm_unpacklo_epi16(y16, u), yuh = _mm_unpackhi_epi16(y16, u);
        const __m128i yvl = _mm_unpacklo_epi16(y16, v), yvh = _mm_unpackhi_epi16(y16, v);
        const __m128i vl = _mm_unpacklo_epi16(v, z), vh = _mm_unpackhi_epi16(v, z);
#define VID_PACK(lo, hi) _mm_packus_epi16(_mm_packs_epi32(_mm_srai_epi32(_mm_add_epi32(lo, half), 10), _mm_srai_epi32(_mm_add_epi32(hi, half), 10)), z)
        const __m128i B = VID_PACK(_mm_madd_epi16(yul, kb), _mm_madd_epi16(yuh, kb));
        const __m128i G = VID_PACK(_mm_add_epi32(_mm_madd_epi16(yul, kg), _mm_madd_epi16(vl, kgv)), _mm_add_epi32(_mm_madd_epi16(yuh, kg), _mm_madd_epi16(vh, kgv)));
        const __m128i R = VID_PACK(_mm_madd_epi16(yvl, kr), _mm_madd_epi16(yvh, kr));
#undef VID_PACK
        const __m128i bg = _mm_unpacklo_epi8(B, G), ra = _mm_unpacklo_epi8(R, a);
        _mm_storeu_si128((__m128i *) o, _mm_unpacklo_epi16(bg, ra));
        _mm_storeu_si128((__m128i *) (o + 16), _mm_unpackhi_epi16(bg, ra));
    }
    return i;
}
#endif

// NV12 の左上 w×h を、1 行 w*4 バイトの B・G・R・A にする（vid_rgb_to_nv12 の逆）。作業用のメモリが取れなければ 0
static inline int vid_nv12_to_bgra(const uint8_t *y, uint32_t ys, const uint8_t *uv, uint32_t uvs, uint32_t w, uint32_t h, uint8_t *out) {
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    int16_t *tmp = (int16_t *) malloc(((size_t) cw * 2 + 8 + (size_t) cw * 4 + 8) * sizeof(int16_t));
    if (!tmp) return 0;
    int16_t *vc = tmp, *cp = tmp + cw * 2 + 8;
    for (uint32_t j = 0; j < h; j++) {
        const uint8_t *yr = y + (size_t) j * ys;
        uint8_t *o = out + (size_t) j * w * 4;
        vid_chroma_row(uv, uvs, ch, j, w, vc, cp);
        uint32_t i = 0;
#if defined(__SSE2__)
        i = vid_row_sse2(yr, cp, w, o);
        o += (size_t) i * 4;
#endif
        for (; i < w; i++, o += 4) {
            const int Y = (yr[i] - 16) * 1192 + 512, U = cp[2 * i], V = cp[2 * i + 1];
            o[0] = vid_clamp10(Y + 2163 * U);
            o[1] = vid_clamp10(Y - 218 * U - 546 * V);
            o[2] = vid_clamp10(Y + 1836 * V);
            o[3] = 255;
        }
    }
    free(tmp);
    return 1;
}

#ifdef _WIN32
// ---- Windows の Media Foundation ----
// GUID はここに持つ（mfuuid などのライブラリを足さない）。mfplat.dll は使うときに読む。
// Windows の N 版のように Media Foundation が無い PC でも、ドライバと受け手は読み込めて JPEG で動く
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>

#define VID_GUID(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } };
#define VID_FCC(n, f) VID_GUID(n, f, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
VID_FCC(VG_VIDEO, 0x73646976)
VID_FCC(VG_H264, 0x34363248)
VID_FCC(VG_NV12, 0x3231564e)
VID_GUID(VG_MAJOR, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f)
VID_GUID(VG_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5)
VID_GUID(VG_FRAME_SIZE, 0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d)
VID_GUID(VG_FRAME_RATE, 0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0)
VID_GUID(VG_ASPECT, 0xc6376a1e, 0x8d0a, 0x4027, 0xbe, 0x45, 0x6d, 0x9a, 0x0a, 0xd3, 0x9b, 0xb6)
VID_GUID(VG_INTERLACE, 0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd)
VID_GUID(VG_BITRATE, 0x20332624, 0xfb0d, 0x4d9e, 0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e)
VID_GUID(VG_PROFILE, 0xad76a80b, 0x2d5c, 0x4e0b, 0xb3, 0x75, 0x64, 0xe5, 0x20, 0x13, 0x70, 0x36)
VID_GUID(VG_STRIDE, 0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6)
VID_GUID(VG_YUV_MATRIX, 0x3e23d450, 0x2c75, 0x4d25, 0xa0, 0x0e, 0xb9, 0x16, 0x70, 0xd1, 0x23, 0x27)
VID_GUID(VG_NOMINAL_RANGE, 0xc21b8ee5, 0xb956, 0x4071, 0x8d, 0xaf, 0x32, 0x5e, 0xdf, 0x5c, 0xab, 0x11)
VID_GUID(VG_ASYNC, 0xf81a699a, 0x649a, 0x497d, 0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a)
VID_GUID(VG_ASYNC_UNLOCK, 0xe5666d6b, 0x3422, 0x4eb6, 0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07)
VID_GUID(VG_FRIENDLY_NAME, 0x314ffbae, 0x5b41, 0x4c95, 0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3)
VID_GUID(VG_CAT_ENCODER, 0xf79eac7d, 0xe545, 0x4387, 0xbd, 0xee, 0xd6, 0x47, 0xd7, 0xbd, 0xe4, 0x2a)
VID_GUID(VG_CLEAN_POINT, 0x9cdf01d8, 0xa0f0, 0x43ba, 0xb0, 0x77, 0xea, 0xa0, 0x6c, 0xbd, 0x72, 0x8a)
// MF_LOW_LATENCY と CODECAPI_AVLowLatencyMode は同じ値
VID_GUID(VG_LOW_LATENCY, 0x9c27891a, 0xed7a, 0x40e1, 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee)
VID_GUID(VG_RC_MODE, 0x1c0608e9, 0x370c, 0x4710, 0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23)
VID_GUID(VG_MEAN_BITRATE, 0xf7222374, 0x2144, 0x4815, 0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52)
VID_GUID(VG_GOP, 0x95f31b26, 0x95a4, 0x41aa, 0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1)
VID_GUID(VG_BFRAMES, 0x8d390aac, 0xdc5c, 0x4200, 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2)
VID_GUID(VG_FORCE_KEY, 0x398c1b98, 0x8353, 0x475a, 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45)
VID_GUID(VG_QUALITY_SPEED, 0x98332df8, 0x03cd, 0x476b, 0x89, 0xfa, 0x3f, 0x9e, 0x44, 0x2d, 0xec, 0x9f)
VID_GUID(VG_IID_DXGIBUFFER, 0xe7174cfa, 0x1c9e, 0x48b1, 0x88, 0x66, 0x62, 0x62, 0x26, 0xbf, 0xc2, 0x58)
VID_GUID(VG_IID_TRANSFORM, 0xbf94c121, 0x5b05, 0x4e6f, 0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d)
VID_GUID(VG_IID_CODECAPI, 0x901db4c7, 0x31ce, 0x41a2, 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda)
VID_GUID(VG_IID_EVENTGEN, 0x2cd0bd52, 0xbcd5, 0x4b89, 0xb6, 0x2c, 0xea, 0xdc, 0x0c, 0x03, 0x1e, 0x7d)
VID_GUID(VG_IID_2DBUFFER, 0x7dc9d5f9, 0x9ed9, 0x44ec, 0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb)
VID_GUID(VG_CLSID_H264_DEC, 0x62ce7e72, 0x4c71, 0x4d20, 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d)
VID_GUID(VG_CLSID_H264_ENC, 0x6ca50344, 0x051a, 0x4ded, 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35)
#undef VID_FCC
#undef VID_GUID

// mfplat.dll の入口
typedef struct {
    HRESULT (WINAPI *Startup)(ULONG, DWORD);
    HRESULT (WINAPI *CreateSample)(IMFSample **);
    HRESULT (WINAPI *CreateMemoryBuffer)(DWORD, IMFMediaBuffer **);
    HRESULT (WINAPI *CreateMediaType)(IMFMediaType **);
    HRESULT (WINAPI *TEnumEx)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
} vid_mf_api;

static vid_mf_api g_vid_mf;
static BOOL CALLBACK vid_mf_once(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void) o; (void) p; (void) c;
    HMODULE m = LoadLibraryW(L"mfplat.dll");
    if (!m) return TRUE;
    vid_mf_api a;
    a.Startup = (HRESULT (WINAPI *)(ULONG, DWORD)) (void *) GetProcAddress(m, "MFStartup");
    a.CreateSample = (HRESULT (WINAPI *)(IMFSample **)) (void *) GetProcAddress(m, "MFCreateSample");
    a.CreateMemoryBuffer = (HRESULT (WINAPI *)(DWORD, IMFMediaBuffer **)) (void *) GetProcAddress(m, "MFCreateMemoryBuffer");
    a.CreateMediaType = (HRESULT (WINAPI *)(IMFMediaType **)) (void *) GetProcAddress(m, "MFCreateMediaType");
    a.TEnumEx = (HRESULT (WINAPI *)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *))
        (void *) GetProcAddress(m, "MFTEnumEx");
    if (a.Startup && a.CreateSample && a.CreateMemoryBuffer && a.CreateMediaType && a.TEnumEx && SUCCEEDED(a.Startup(MF_VERSION, MFSTARTUP_LITE)))
        g_vid_mf = a;
    return TRUE;
}
// 使えなければ NULL。最初の 1 回だけ読む（MFStartup も 1 回だけ。止めずにプロセスの終わりまで使う）
static inline const vid_mf_api *vid_mf(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, vid_mf_once, NULL, NULL);
    return g_vid_mf.Startup ? &g_vid_mf : NULL;
}

static inline void vid_api_u32(ICodecAPI *c, const GUID *g, UINT32 v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_UI4;
    x.ulVal = v;
    ICodecAPI_SetValue(c, g, &x);
}
static inline void vid_api_bool(ICodecAPI *c, const GUID *g, int v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_BOOL;
    x.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    ICodecAPI_SetValue(c, g, &x);
}

// 動画の形（sub）。fps・bps・profile は 0 なら付けない
static inline IMFMediaType *vid_type(const GUID *sub, uint32_t w, uint32_t h, uint32_t fps, uint32_t bps, uint32_t profile) {
    const vid_mf_api *mf = vid_mf();
    IMFMediaType *t = NULL;
    if (!mf || FAILED(mf->CreateMediaType(&t))) return NULL;
    IMFMediaType_SetGUID(t, &VG_MAJOR, &VG_VIDEO);
    IMFMediaType_SetGUID(t, &VG_SUBTYPE, sub);
    if (w && h) IMFMediaType_SetUINT64(t, &VG_FRAME_SIZE, ((UINT64) w << 32) | h);
    if (fps) IMFMediaType_SetUINT64(t, &VG_FRAME_RATE, ((UINT64) fps << 32) | 1);
    IMFMediaType_SetUINT64(t, &VG_ASPECT, ((UINT64) 1 << 32) | 1);
    IMFMediaType_SetUINT32(t, &VG_INTERLACE, 2); // MFVideoInterlace_Progressive
    if (bps) IMFMediaType_SetUINT32(t, &VG_BITRATE, bps);
    if (profile) IMFMediaType_SetUINT32(t, &VG_PROFILE, profile);
    return t;
}

// 1 つの媒体の中身を n バイト持つ sample
static inline IMFSample *vid_sample(DWORD n) {
    const vid_mf_api *mf = vid_mf();
    IMFSample *s = NULL;
    IMFMediaBuffer *b = NULL;
    if (!mf || FAILED(mf->CreateSample(&s))) return NULL;
    if (FAILED(mf->CreateMemoryBuffer(n ? n : 1, &b))) { IMFSample_Release(s); return NULL; }
    IMFSample_AddBuffer(s, b);
    IMFMediaBuffer_Release(b);
    return s;
}
#endif
