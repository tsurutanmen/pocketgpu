// 色の変換の試験：vcam_convert.h の SIMD 版を、前の版（1 画素ずつ）と照らし合わせ、1 コマにかかる時間を測る。
//   vcam_bench [幅 高さ [回数 [糸の数]]]      既定は 3840 2160 40 1
// 照らし合わせ：乱数の絵と、色の境目が多い絵で、各形式の出力の最大の差と、差のある画素の割合を出す。
// 時間：1 コマ（NV12 の入力は 1 コマ分を使い回す。出力は毎回同じ場所＝使うアプリの allocator の buffer を使い回す形）の平均・中央・最大（ms）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "vcam_convert.h"
#include "vcam_scale.h"

static double now_ms() {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

struct Job {
    VcKind k;
    const uint8_t *src;
    int w, h;
    uint8_t *dst;
    long stride;
    int y0, y1;
    HANDLE go, done;
    volatile LONG quit;
};
static DWORD WINAPI worker(LPVOID p) {
    Job *j = (Job *) p;
    for (;;) {
        WaitForSingleObject(j->go, INFINITE);
        if (j->quit) return 0;
        vc_convert_rows(j->k, j->src, j->w, j->h, j->dst, j->stride, j->y0, j->y1);
        SetEvent(j->done);
    }
}

static const char *NAMES[] = { "NV12", "YUY2", "I420", "RGB32", "RGB24" };
static long stride_of(VcKind k, int w) { return k == VC_YUY2 ? w * 2L : k == VC_RGB32 ? w * 4L : k == VC_RGB24 ? ((w * 3L) + 3) & ~3L : w; }
static size_t size_of(VcKind k, int w, int h) { return k == VC_I420 || k == VC_NV12 ? (size_t) w * h * 3 / 2 : (size_t) stride_of(k, w) * h; }

int main(int argc, char **argv) {
    int w = argc > 2 ? atoi(argv[1]) : 3840, h = argc > 2 ? atoi(argv[2]) : 2160, n = argc > 3 ? atoi(argv[3]) : 40, nt = argc > 4 ? atoi(argv[4]) : 1;
    printf("vcam_bench %dx%d・%d 回・糸 %d 本（%d ビット）\n", w, h, n, nt, (int) sizeof(void *) * 8);
    std::vector<uint8_t> src((size_t) w * h * 3 / 2);
    // 絵 1：乱数。絵 2：なめらかな勾配と境目
    unsigned s = 12345;
    for (auto &b : src) { s = s * 1664525u + 1013904223u; b = (uint8_t) (s >> 24); }
    std::vector<uint8_t> src2(src.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) src2[(size_t) y * w + x] = (uint8_t) (16 + ((x * 3 + y) % 220));
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w; x++) src2[(size_t) w * h + (size_t) y * w + x] = (uint8_t) ((x & 1) ? 128 + ((y * 5) % 100) - 50 : 128 + ((x / 2 * 3) % 100) - 50);
    VcKind kinds[] = { VC_YUY2, VC_I420, VC_RGB32, VC_RGB24 };
    int bad = 0;
    for (VcKind k : kinds) {
        for (int pic = 0; pic < 2; pic++) {
            const uint8_t *in = pic ? src2.data() : src.data();
            std::vector<uint8_t> a(size_of(k, w, h) + 64), b(size_of(k, w, h) + 64);
            vc_convert_ref(k, in, w, h, a.data(), stride_of(k, w));
            vc_convert_rows(k, in, w, h, b.data(), stride_of(k, w), 0, h);
            size_t nd = 0, total = size_of(k, w, h);
            int mx = 0;
            for (size_t i = 0; i < total; i++) {
                int d = abs((int) a[i] - (int) b[i]);
                if (d) nd++;
                if (d > mx) mx = d;
            }
            // RGB24 の行の詰め物（4 の倍数にする余り）は見ない
            printf("  %-5s 絵%d：最大の差 %d・違うバイト %zu / %zu（%.4f%%）%s\n", NAMES[k], pic + 1, mx, nd, total, 100.0 * (double) nd / (double) total, mx > 2 ? "  ← 大きい" : "");
            if (mx > 2) bad++;
        }
    }
    // webcam_rx が 4K を 1920x1080 に縮める SSE2（vcam_scale.h）：照らし合わせと時間
    if (w % 32 == 0 && h % 4 == 0) {
        const uint32_t dw = (uint32_t) w / 2, dh = (uint32_t) h / 2;
        std::vector<uint8_t> a((size_t) dw * dh * 3 / 2), b(a.size());
        vc_half_scale_nv12_ref(src.data(), src.data() + (size_t) w * h, (uint32_t) w, dw, dh, a.data());
        vc_half_scale_nv12(src.data(), src.data() + (size_t) w * h, (uint32_t) w, dw, dh, b.data());
        const int same = memcmp(a.data(), b.data(), a.size()) == 0;
        double s1 = 0, s2 = 0;
        for (int i = -2; i < 20; i++) {
            double t0 = now_ms();
            vc_half_scale_nv12(src.data(), src.data() + (size_t) w * h, (uint32_t) w, dw, dh, b.data());
            double t1 = now_ms();
            vc_half_scale_nv12_ref(src.data(), src.data() + (size_t) w * h, (uint32_t) w, dw, dh, a.data());
            double t2 = now_ms();
            if (i >= 0) s1 += t1 - t0, s2 += t2 - t1;
        }
        printf("  縮め（%dx%d → %ux%u）：SSE2 と 1 画素ずつは%s。SSE2 %.2f ms・1 画素ずつ %.2f ms\n", w, h, dw, dh, same ? "完全に同じ" : "違う（バグ）", s1 / 20, s2 / 20);
        if (!same) bad++;
    }
    // 時間
    std::vector<Job> jobs(nt);
    std::vector<HANDLE> th(nt);
    for (int t = 0; t < nt; t++) {
        jobs[t].go = CreateEventW(NULL, FALSE, FALSE, NULL);
        jobs[t].done = CreateEventW(NULL, FALSE, FALSE, NULL);
        jobs[t].quit = 0;
        if (nt > 1) th[t] = CreateThread(NULL, 0, worker, &jobs[t], 0, NULL);
    }
    printf("1 コマの時間（ms）：\n");
    for (VcKind k : { VC_NV12, VC_YUY2, VC_I420, VC_RGB32, VC_RGB24 }) {
        std::vector<uint8_t> dst(size_of(k, w, h) + 64);
        std::vector<double> t(n);
        for (int i = -3; i < n; i++) {
            const double t0 = now_ms();
            if (k == VC_NV12) {
                for (int y = 0; y < h; y++) memcpy(dst.data() + (size_t) y * w, src.data() + (size_t) y * w, (size_t) w);
                for (int y = 0; y < h / 2; y++) memcpy(dst.data() + (size_t) w * h + (size_t) y * w, src.data() + (size_t) w * h + (size_t) y * w, (size_t) w);
            } else if (nt <= 1) vc_convert_rows(k, src.data(), w, h, dst.data(), stride_of(k, w), 0, h);
            else {
                int per = ((h / nt) + 1) & ~1;
                for (int q = 0; q < nt; q++) {
                    jobs[q].k = k, jobs[q].src = src.data(), jobs[q].w = w, jobs[q].h = h, jobs[q].dst = dst.data(), jobs[q].stride = stride_of(k, w);
                    jobs[q].y0 = q * per, jobs[q].y1 = q == nt - 1 ? h : std::min(h, (q + 1) * per);
                    if (q) SetEvent(jobs[q].go);
                }
                vc_convert_rows(k, src.data(), w, h, dst.data(), stride_of(k, w), jobs[0].y0, jobs[0].y1);
                for (int q = 1; q < nt; q++) WaitForSingleObject(jobs[q].done, INFINITE);
            }
            if (i >= 0) t[i] = now_ms() - t0;
        }
        std::sort(t.begin(), t.end());
        double sum = 0;
        for (double v : t) sum += v;
        printf("  %-5s 平均 %6.2f  中央 %6.2f  最大 %6.2f ms   （60 コマ/秒の 1 コマ 16.7 ms の %.0f%%）\n", NAMES[k], sum / n, t[n / 2], t[n - 1], 100.0 * (sum / n) / 16.667);
    }
    printf("（比べる）前の版の 1 画素ずつの書き方（同じコンパイラの -O2）：\n");
    for (VcKind k : { VC_YUY2, VC_I420, VC_RGB32, VC_RGB24 }) {
        std::vector<uint8_t> dst(size_of(k, w, h) + 64);
        double sum = 0;
        const int m = 8;
        for (int i = -2; i < m; i++) {
            const double t0 = now_ms();
            vc_convert_ref(k, src.data(), w, h, dst.data(), stride_of(k, w));
            if (i >= 0) sum += now_ms() - t0;
        }
        printf("  %-5s 平均 %6.2f ms\n", NAMES[k], sum / m);
    }
    for (int t = 1; t < nt; t++) { jobs[t].quit = 1; SetEvent(jobs[t].go); WaitForSingleObject(th[t], 1000); }
    return bad ? 1 : 0;
}
