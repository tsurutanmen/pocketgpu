// 仮想カメラ（dshow_cam.dll）の中の色の変換：NV12（詰めてある w×h）→ YUY2・I420・RGB32・RGB24。SSE2（x86-64 は必ずある。32 ビット版は -msse2 で組む）。
// 行の範囲 [y0, y1) だけを受け持つ（y0・y1 は偶数。色は 2 行で 1 組なので）。何本かの糸で分けるときに使える。
// 式は前の版（1 画素ずつの書き方）と同じ BT.601 の制限範囲：C=(Y-16)*298, R=(C+409V')>>8, G=(C-100U'-208V')>>8, B=(C+516U')>>8（U'=U-128・V'=V-128）。
// SIMD 版は 16 ビットの掛け算の上半分（mulhi）で 6 ビットの小数を持って計算するので、前の版と ±1 まで違うことがある（試験 vcam_bench が測る）。
#pragma once
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>

enum VcKind { VC_NV12, VC_YUY2, VC_I420, VC_RGB32, VC_RGB24 };

static inline uint8_t vc_clip8(int v) { return (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v); }

// ---- 1 行ぶんの BGRA（SIMD。w は 16 の倍数の部分だけ SIMD、残りは 1 画素ずつ）
static inline void vc_row_bgra(const uint8_t *yr, const uint8_t *cr, int w, uint8_t *o) {
    const __m128i zero = _mm_setzero_si128(), c1192 = _mm_set1_epi16(1192), c128 = _mm_set1_epi16(128), rnd = _mm_set1_epi16(32), mask = _mm_set1_epi16(0x00ff);
    const __m128i k_y = _mm_set1_epi16((short) 19072);      // 298*64
    const __m128i k_vr = _mm_set1_epi16((short) 26176);     // 409/256*16384
    const __m128i k_ug = _mm_set1_epi16((short) -6400);     // -100/256*16384
    const __m128i k_vg = _mm_set1_epi16((short) -13312);    // -208/256*16384
    const __m128i k_ub = _mm_set1_epi16((short) (33024 - 65536)); // 516/256*16384 = 33024（符号つきに入らないので -32512 + 1.0 に分ける）
    const __m128i alpha = _mm_set1_epi8((char) 0xff);
    int x = 0;
    for (; x + 16 <= w; x += 16) {
        const __m128i y16 = _mm_loadu_si128((const __m128i *) (yr + x));
        const __m128i uv = _mm_loadu_si128((const __m128i *) (cr + x)); // u0 v0 u1 v1 ... u7 v7（16 画素ぶん）
        const __m128i U8 = _mm_slli_epi16(_mm_sub_epi16(_mm_and_si128(uv, mask), c128), 8);   // (U-128)<<8
        const __m128i V8 = _mm_slli_epi16(_mm_sub_epi16(_mm_srli_epi16(uv, 8), c128), 8);     // (V-128)<<8
        const __m128i vr = _mm_mulhi_epi16(V8, k_vr);
        const __m128i ug = _mm_mulhi_epi16(U8, k_ug);
        const __m128i vg = _mm_mulhi_epi16(V8, k_vg);
        const __m128i ub = _mm_add_epi16(_mm_mulhi_epi16(U8, k_ub), U8);
        __m128i rr[2], gg[2], bb[2];
        for (int half = 0; half < 2; half++) {
            const __m128i yw = half ? _mm_unpackhi_epi8(y16, zero) : _mm_unpacklo_epi8(y16, zero);
            // Y*74.5（6 ビットの小数）から 16*74.5=1192 を引く。(Y-16) が負でも前の版と同じ値になる
            const __m128i y64 = _mm_adds_epi16(_mm_sub_epi16(_mm_mulhi_epu16(_mm_slli_epi16(yw, 8), k_y), c1192), rnd);
            // 色は 2 画素で 1 組：この 8 画素ぶんの 4 組を 2 つずつに増やす
            const __m128i vrh = half ? _mm_unpackhi_epi16(vr, vr) : _mm_unpacklo_epi16(vr, vr);
            const __m128i ugh = half ? _mm_unpackhi_epi16(ug, ug) : _mm_unpacklo_epi16(ug, ug);
            const __m128i vgh = half ? _mm_unpackhi_epi16(vg, vg) : _mm_unpacklo_epi16(vg, vg);
            const __m128i ubh = half ? _mm_unpackhi_epi16(ub, ub) : _mm_unpacklo_epi16(ub, ub);
            rr[half] = _mm_srai_epi16(_mm_adds_epi16(y64, vrh), 6);
            gg[half] = _mm_srai_epi16(_mm_adds_epi16(_mm_adds_epi16(y64, ugh), vgh), 6);
            bb[half] = _mm_srai_epi16(_mm_adds_epi16(y64, ubh), 6);
        }
        const __m128i R = _mm_packus_epi16(rr[0], rr[1]), G = _mm_packus_epi16(gg[0], gg[1]), B = _mm_packus_epi16(bb[0], bb[1]);
        const __m128i bg_lo = _mm_unpacklo_epi8(B, G), bg_hi = _mm_unpackhi_epi8(B, G);
        const __m128i ra_lo = _mm_unpacklo_epi8(R, alpha), ra_hi = _mm_unpackhi_epi8(R, alpha);
        uint8_t *p = o + (size_t) x * 4;
        _mm_storeu_si128((__m128i *) (p + 0), _mm_unpacklo_epi16(bg_lo, ra_lo));
        _mm_storeu_si128((__m128i *) (p + 16), _mm_unpackhi_epi16(bg_lo, ra_lo));
        _mm_storeu_si128((__m128i *) (p + 32), _mm_unpacklo_epi16(bg_hi, ra_hi));
        _mm_storeu_si128((__m128i *) (p + 48), _mm_unpackhi_epi16(bg_hi, ra_hi));
    }
    for (; x < w; x++) {
        const int C = (yr[x] - 16) * 298 + 128, D = cr[x & ~1] - 128, E = cr[(x & ~1) + 1] - 128;
        uint8_t *q = o + (size_t) x * 4;
        q[0] = vc_clip8((C + 516 * D) >> 8), q[1] = vc_clip8((C - 100 * D - 208 * E) >> 8), q[2] = vc_clip8((C + 409 * E) >> 8), q[3] = 255;
    }
}

// ---- 行の範囲 [y0, y1) を変える。dst の行の幅は stride バイト（RGB は下から上に並べる。I420 は幅 w の 3 つの面）
static inline void vc_convert_rows(VcKind k, const uint8_t *src, int w, int h, uint8_t *dst, long stride, int y0, int y1) {
    const uint8_t *sy = src, *suv = src + (size_t) w * h;
    switch (k) {
    case VC_NV12: break;
    case VC_I420: {
        uint8_t *u = dst + (size_t) w * h, *v = u + (size_t) (w / 2) * (h / 2);
        const __m128i mask = _mm_set1_epi16(0x00ff);
        for (int y = y0; y < y1; y++) memcpy(dst + (size_t) y * w, sy + (size_t) y * w, (size_t) w);
        for (int y = y0 / 2; y < y1 / 2; y++) {
            const uint8_t *r = suv + (size_t) y * w;
            uint8_t *ur = u + (size_t) y * (w / 2), *vr = v + (size_t) y * (w / 2);
            int x = 0;
            for (; x + 16 <= w / 2; x += 16) {
                const __m128i a = _mm_loadu_si128((const __m128i *) (r + 2 * x)), b = _mm_loadu_si128((const __m128i *) (r + 2 * x + 16));
                _mm_storeu_si128((__m128i *) (ur + x), _mm_packus_epi16(_mm_and_si128(a, mask), _mm_and_si128(b, mask)));
                _mm_storeu_si128((__m128i *) (vr + x), _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8)));
            }
            for (; x < w / 2; x++) ur[x] = r[2 * x], vr[x] = r[2 * x + 1];
        }
        break;
    }
    case VC_YUY2:
        for (int y = y0; y < y1; y++) {
            const uint8_t *yr = sy + (size_t) y * w, *cr = suv + (size_t) (y / 2) * w;
            uint8_t *o = dst + (size_t) y * stride;
            int x = 0;
            for (; x + 16 <= w; x += 16) {
                const __m128i a = _mm_loadu_si128((const __m128i *) (yr + x)), c = _mm_loadu_si128((const __m128i *) (cr + x));
                _mm_storeu_si128((__m128i *) (o + 2 * x), _mm_unpacklo_epi8(a, c));
                _mm_storeu_si128((__m128i *) (o + 2 * x + 16), _mm_unpackhi_epi8(a, c));
            }
            for (; x < w; x += 2) o[2 * x] = yr[x], o[2 * x + 1] = cr[x], o[2 * x + 2] = yr[x + 1], o[2 * x + 3] = cr[x + 1];
        }
        break;
    case VC_RGB32:
        for (int y = y0; y < y1; y++) vc_row_bgra(sy + (size_t) y * w, suv + (size_t) (y / 2) * w, w, dst + (size_t) (h - 1 - y) * stride);
        break;
    case VC_RGB24: {
        // 1 行ぶんを BGRA で作ってから 3 バイトに詰める（SSE2 には 3 バイトに詰める命令が無い）。1 行は最大 4K で 16 KB
        uint8_t tmp[4096 * 4 + 64];
        if (w > 4096) break;
        for (int y = y0; y < y1; y++) {
            vc_row_bgra(sy + (size_t) y * w, suv + (size_t) (y / 2) * w, w, tmp);
            uint8_t *o = dst + (size_t) (h - 1 - y) * stride;
            int x = 0;
            for (; x + 1 < w; x++) { uint32_t p; memcpy(&p, tmp + (size_t) x * 4, 4); memcpy(o + (size_t) x * 3, &p, 4); } // 4 バイト書いて 3 バイトだけ進む（次の画素が上書きする）
            o[(size_t) x * 3] = tmp[(size_t) x * 4], o[(size_t) x * 3 + 1] = tmp[(size_t) x * 4 + 1], o[(size_t) x * 3 + 2] = tmp[(size_t) x * 4 + 2];
        }
        break;
    }
    }
}

// ---- 前の版と同じ 1 画素ずつの書き方（試験の照らし合わせ用）
static inline void vc_convert_ref(VcKind k, const uint8_t *src, int w, int h, uint8_t *dst, long stride) {
    const uint8_t *sy = src, *suv = src + (size_t) w * h;
    switch (k) {
    case VC_NV12: break;
    case VC_I420: {
        uint8_t *u = dst + (size_t) w * h, *v = u + (size_t) (w / 2) * (h / 2);
        memcpy(dst, sy, (size_t) w * h);
        for (int y = 0; y < h / 2; y++) {
            const uint8_t *r = suv + (size_t) y * w;
            uint8_t *ur = u + (size_t) y * (w / 2), *vr = v + (size_t) y * (w / 2);
            for (int x = 0; x < w / 2; x++) ur[x] = r[2 * x], vr[x] = r[2 * x + 1];
        }
        break;
    }
    case VC_YUY2:
        for (int y = 0; y < h; y++) {
            const uint8_t *yr = sy + (size_t) y * w, *cr = suv + (size_t) (y / 2) * w;
            uint8_t *o = dst + (size_t) y * stride;
            for (int x = 0; x < w; x += 2, o += 4) o[0] = yr[x], o[1] = cr[x], o[2] = yr[x + 1], o[3] = cr[x + 1];
        }
        break;
    case VC_RGB32:
    case VC_RGB24: {
        const int bpp = k == VC_RGB32 ? 4 : 3;
        for (int y = 0; y < h; y++) {
            const uint8_t *yr = sy + (size_t) y * w, *cr = suv + (size_t) (y / 2) * w;
            uint8_t *o = dst + (size_t) (h - 1 - y) * stride;
            for (int x = 0; x < w; x++, o += bpp) {
                const int C = (yr[x] - 16) * 298 + 128, D = cr[x & ~1] - 128, E = cr[(x & ~1) + 1] - 128;
                o[0] = vc_clip8((C + 516 * D) >> 8);
                o[1] = vc_clip8((C - 100 * D - 208 * E) >> 8);
                o[2] = vc_clip8((C + 409 * E) >> 8);
                if (bpp == 4) o[3] = 255;
            }
        }
        break;
    }
    }
}
