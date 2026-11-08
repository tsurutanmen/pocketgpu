// NV12 をちょうど半分に縮める（3840x2160 → 1920x1080）。2x2 の平均（色は 2x2 の色の組の平均）。SSE2。C でも C++ でも使える。
// webcam_rx が、4K の絵を前の読む側のために 1920x1080 に縮めて共有メモリの本体の枠へ書くときに使う（4K の口が無い読む側・プレビューの絵）。
// src は Y が pitch バイトの行で h 行、そのあと UV が pitch バイトの行で h/2 行。dst は詰めた（幅 dw）NV12。w＝dw*2・h＝dh*2。
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <emmintrin.h>

static inline void vc_half_scale_nv12_ref(const uint8_t *y, const uint8_t *uv, uint32_t pitch, uint32_t dw, uint32_t dh, uint8_t *d) {
    for (uint32_t r = 0; r < dh; r++) {
        const uint8_t *a = y + (size_t) (2 * r) * pitch, *b = a + pitch;
        uint8_t *o = d + (size_t) r * dw;
        for (uint32_t x = 0; x < dw; x++) o[x] = (uint8_t) ((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
    for (uint32_t r = 0; r < dh / 2; r++) {
        const uint8_t *a = uv + (size_t) (2 * r) * pitch, *b = a + pitch;
        uint8_t *o = d + (size_t) dw * dh + (size_t) r * dw;
        for (uint32_t x = 0; x < dw / 2; x++) {
            o[2 * x] = (uint8_t) ((a[4 * x] + a[4 * x + 2] + b[4 * x] + b[4 * x + 2] + 2) >> 2);
            o[2 * x + 1] = (uint8_t) ((a[4 * x + 1] + a[4 * x + 3] + b[4 * x + 1] + b[4 * x + 3] + 2) >> 2);
        }
    }
}

static inline void vc_half_scale_nv12(const uint8_t *y, const uint8_t *uv, uint32_t pitch, uint32_t dw, uint32_t dh, uint8_t *d) {
    const __m128i mask = _mm_set1_epi16(0x00ff), two = _mm_set1_epi16(2), ones = _mm_set1_epi16(1), two32 = _mm_set1_epi32(2);
    for (uint32_t r = 0; r < dh; r++) {
        const uint8_t *a = y + (size_t) (2 * r) * pitch, *b = a + pitch;
        uint8_t *o = d + (size_t) r * dw;
        uint32_t x = 0;
        for (; x + 16 <= dw; x += 16) {
            __m128i s[2];
            for (int k = 0; k < 2; k++) {
                const __m128i va = _mm_loadu_si128((const __m128i *) (a + 2 * x + 16 * k)), vb = _mm_loadu_si128((const __m128i *) (b + 2 * x + 16 * k));
                __m128i t = _mm_add_epi16(_mm_add_epi16(_mm_and_si128(va, mask), _mm_srli_epi16(va, 8)), _mm_add_epi16(_mm_and_si128(vb, mask), _mm_srli_epi16(vb, 8)));
                s[k] = _mm_srli_epi16(_mm_add_epi16(t, two), 2);
            }
            _mm_storeu_si128((__m128i *) (o + x), _mm_packus_epi16(s[0], s[1]));
        }
        for (; x < dw; x++) o[x] = (uint8_t) ((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
    for (uint32_t r = 0; r < dh / 2; r++) {
        const uint8_t *a = uv + (size_t) (2 * r) * pitch, *b = a + pitch;
        uint8_t *o = d + (size_t) dw * dh + (size_t) r * dw;
        uint32_t x = 0; // 出力のバイト（2 バイトで U と V の 1 組）
        for (; x + 16 <= dw; x += 16) {
            __m128i du[2], dv[2];
            for (int k = 0; k < 2; k++) {
                const __m128i va = _mm_loadu_si128((const __m128i *) (a + 2 * x + 16 * k)), vb = _mm_loadu_si128((const __m128i *) (b + 2 * x + 16 * k));
                const __m128i eu = _mm_add_epi16(_mm_and_si128(va, mask), _mm_and_si128(vb, mask)), ev = _mm_add_epi16(_mm_srli_epi16(va, 8), _mm_srli_epi16(vb, 8));
                du[k] = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(eu, ones), two32), 2);
                dv[k] = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(ev, ones), two32), 2);
            }
            const __m128i u = _mm_packs_epi32(du[0], du[1]), v = _mm_packs_epi32(dv[0], dv[1]);
            _mm_storeu_si128((__m128i *) (o + x), _mm_or_si128(u, _mm_slli_epi16(v, 8)));
        }
        for (; x < dw; x += 2) {
            o[x] = (uint8_t) ((a[2 * x] + a[2 * x + 2] + b[2 * x] + b[2 * x + 2] + 2) >> 2);
            o[x + 1] = (uint8_t) ((a[2 * x + 1] + a[2 * x + 3] + b[2 * x + 1] + b[2 * x + 3] + 2) >> 2);
        }
    }
}
