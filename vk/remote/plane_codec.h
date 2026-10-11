// 動画のコマのような「1 画素 1 バイト（か 2・4 バイト）の平面」を、画質を落とさずに縮める・ほどく（VR_MEM_WRITE_CODED の中身）。
// ドライバ（icd.c）が縮め、受け手（server.c）がほどく。どちらもこの 1 つのファイルを取り込む。ほかの部品は使わない（C99 と OS の糸だけ）。
//
// 方式 1（PC_CODEC_1）：
//   予測  左 a・上 b・左上 c から a + b - c（256 で回す。0..255 に切り詰めないので、ほどくのは行ごとの足し算の積み上げだけになる。
//         切り詰めた場合との差は、実際の動画で 0.03 % だけ大きくなる程度）。残差は 8 ビットで回し、0,-1,1,-2… の順の番号 z にする。
//         段の最初の行は左だけ（行の頭は 0）。ほかの行の頭は上。bpp が 2・4 なら、左は bpp バイト前（色ごとに予測する）
//   文脈  上の行だけから決める 8 つ：|b-c| + |d-b| + 上の z + (左上の z + 右上の z)/2 を 0,1,2-3,4-7,…,64 以上に分ける（d は右上）。
//         上の行だけなので、ほどく側は 1 行ぶんの文脈を先にまとめて計算でき、番号を読んでから画素に戻せる
//   符号  rANS（32 ビットの状態・16 ビットずつ読み書き・確率は 4096 分の）。行の中の x を 4 で割った余りごとに、別の状態と別の流れ（4 本）。
//         流れが別なので、4 つの状態はほどくときに待ち合わせない。頻度の表は文脈ごとに 1 つで、全部の段で共通（面全体の数から作る）
//   段    行をいくつかの段に分け、段ごとに別の流れにする（段の最初の行は上を見ない）。ほどくのも縮めるのも段ごとに別の糸でできる
//
// 中身の並び（どれも little endian）：
//   u32 方式（1）・u32 bpp（1/2/4）・u32 幅（1 行のバイト）・u32 行の数・u32 段の数・u32 表のバイト・表・
//   段ごとに（u32 行の数・u32 段のバイト）・段を順に
// 表：文脈 8 つぶん。それぞれ「記号の数 n（0 なら使わない文脈）」と、記号 0..n-1 の頻度（合わせて 4096、1 つは 4095 まで）。どの数も 7 ビットずつの可変長
// 段：u32 流れのバイト ×4・u32 状態の初め ×4・流れ 4 本（16 ビットの語）。ほどき終わったら、状態はどれも 65536 に戻り、
//     どの流れもちょうど使い切る（壊れていないかの確かめ）
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#endif

#define PC_CODEC_1 1u
#define PC_NCTX 8
#define PC_SB 12
#define PC_M (1u << PC_SB)
#define PC_L (1u << 16)
#define PC_MAX_W (1u << 20)        // 1 行は 1 MB まで
#define PC_MAX_BYTES (1ull << 31)  // 1 回で 2 GB まで
#define PC_HEAD 24u
#include <stddef.h>
// 試験のときだけ、縮める・ほどく所の時間を分けて数える（-DPC_PROF・x86 の clang か gcc だけ）
#if defined(PC_PROF) && (defined(__x86_64__) || defined(__i386__))
static uint64_t g_pc_prof[6];
#define PC_TICK(v) uint64_t v = __builtin_ia32_rdtsc()
#define PC_ACC(i, a, b) (g_pc_prof[i] += (b) - (a))
#else
#define PC_TICK(v)
#define PC_ACC(i, a, b)
#endif

// PC_NO_SIMD を決めて組むと、まとめて計算する所を使わない（答えは同じ。調べ用）
#if defined(PC_NO_SIMD)
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PC_NEON 1
#elif defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#define PC_SSE2 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PC_INLINE static inline __attribute__((always_inline))
#define PC_UNUSED __attribute__((unused))
#else
#define PC_INLINE static inline
#define PC_UNUSED
#endif

// ---- 小さな道具 ----
static inline uint32_t pc_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void pc_wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint32_t pc_ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void pc_st16(uint8_t *p, uint32_t v) { uint16_t w = (uint16_t) v; memcpy(p, &w, 2); }
static inline uint8_t pc_zig(uint8_t r) { return (uint8_t) ((uint8_t) (r << 1) ^ (uint8_t) ((int8_t) r >> 7)); }
static inline uint8_t pc_unzig(uint8_t z) { return (uint8_t) ((z >> 1) ^ (uint8_t) (0u - (z & 1u))); }
static inline int pc_abs(int v) { return v < 0 ? -v : v; }

// ---- 糸の溜まり（縮める・ほどくのを段ごとに分ける） ----
#ifdef _WIN32
typedef CRITICAL_SECTION pc_mutex;
typedef CONDITION_VARIABLE pc_cond;
static void pc_mx_init(pc_mutex *m) { InitializeCriticalSection(m); }
static void pc_mx_lock(pc_mutex *m) { EnterCriticalSection(m); }
static void pc_mx_unlock(pc_mutex *m) { LeaveCriticalSection(m); }
static void pc_cv_init(pc_cond *c) { InitializeConditionVariable(c); }
static void pc_cv_wait(pc_cond *c, pc_mutex *m) { SleepConditionVariableCS(c, m, INFINITE); }
static void pc_cv_all(pc_cond *c) { WakeAllConditionVariable(c); }
static void pc_yield(void) { SwitchToThread(); }
static int pc_ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int) si.dwNumberOfProcessors; }
#else
typedef pthread_mutex_t pc_mutex;
typedef pthread_cond_t pc_cond;
static void pc_mx_init(pc_mutex *m) { pthread_mutex_init(m, NULL); }
static void pc_mx_lock(pc_mutex *m) { pthread_mutex_lock(m); }
static void pc_mx_unlock(pc_mutex *m) { pthread_mutex_unlock(m); }
static void pc_cv_init(pc_cond *c) { pthread_cond_init(c, NULL); }
static void pc_cv_wait(pc_cond *c, pc_mutex *m) { pthread_cond_wait(c, m); }
static void pc_cv_all(pc_cond *c) { pthread_cond_broadcast(c); }
static void pc_yield(void) { sched_yield(); }
static int pc_ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int) n : 1; }
#endif

typedef void (*pc_fn)(void *arg, int i);
typedef struct { pc_fn fn; void *arg; int n, next, done, active; } pc_job;
static struct {
    volatile int state; // 0 まだ・1 作っている・2 できた
    pc_mutex mu, call;
    pc_cond work, fin;
    pc_job *cur;
    unsigned gen;
    int nthr;
} g_pcp;
// 溜まりの糸の数。最初に使う前に決める（-1 なら CPU の数 - 1・7 まで）
static int g_pc_threads_want = -1;

static void pc_run(pc_job *j) {
    for (;;) {
        int i = __atomic_fetch_add(&j->next, 1, __ATOMIC_ACQ_REL);
        if (i >= j->n) break;
        j->fn(j->arg, i);
        __atomic_fetch_add(&j->done, 1, __ATOMIC_ACQ_REL);
    }
}

#ifdef _WIN32
static DWORD WINAPI pc_worker(LPVOID unused)
#else
static void *pc_worker(void *unused)
#endif
{
    (void) unused;
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    unsigned seen = 0;
    pc_mx_lock(&g_pcp.mu);
    for (;;) {
        while (!(g_pcp.cur && g_pcp.gen != seen)) pc_cv_wait(&g_pcp.work, &g_pcp.mu);
        seen = g_pcp.gen;
        pc_job *j = g_pcp.cur;
        j->active++;
        pc_mx_unlock(&g_pcp.mu);
        pc_run(j);
        pc_mx_lock(&g_pcp.mu);
        j->active--;
        pc_cv_all(&g_pcp.fin);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void pc_pool_init(void) {
    int st = 0;
    if (__atomic_compare_exchange_n(&g_pcp.state, &st, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pc_mx_init(&g_pcp.mu);
        pc_mx_init(&g_pcp.call);
        pc_cv_init(&g_pcp.work);
        pc_cv_init(&g_pcp.fin);
        int want = g_pc_threads_want;
        if (want < 0) { want = pc_ncpu() - 1; if (want > 7) want = 7; }
        if (want > 31) want = 31;
        int made = 0;
        for (int i = 0; i < want; i++) {
#ifdef _WIN32
            HANDLE t = CreateThread(NULL, 256 << 10, pc_worker, NULL, 0, NULL);
            if (!t) break;
            CloseHandle(t);
#else
            pthread_t t;
            if (pthread_create(&t, NULL, pc_worker, NULL) != 0) break;
            pthread_detach(t);
#endif
            made++;
        }
        g_pcp.nthr = made;
        __atomic_store_n(&g_pcp.state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&g_pcp.state, __ATOMIC_ACQUIRE) != 2) pc_yield();
    }
}

// fn(arg, 0..n-1) を溜まりの糸と呼んだ糸で分けて行い、全部終わってから戻る。いくつもの糸から同時に呼ばれたら順にする
static void pc_par(int n, pc_fn fn, void *arg) {
    if (n <= 0) return;
    if (__atomic_load_n(&g_pcp.state, __ATOMIC_ACQUIRE) != 2) pc_pool_init();
    if (n == 1 || g_pcp.nthr == 0) {
        for (int i = 0; i < n; i++) fn(arg, i);
        return;
    }
    pc_mx_lock(&g_pcp.call);
    pc_job j = { fn, arg, n, 0, 0, 0 };
    pc_mx_lock(&g_pcp.mu);
    g_pcp.cur = &j;
    g_pcp.gen++;
    pc_cv_all(&g_pcp.work);
    pc_mx_unlock(&g_pcp.mu);
    pc_run(&j);
    pc_mx_lock(&g_pcp.mu);
    g_pcp.cur = NULL; // これより後には誰も加わらない
    while (j.active > 0 || __atomic_load_n(&j.done, __ATOMIC_ACQUIRE) < n) pc_cv_wait(&g_pcp.fin, &g_pcp.mu);
    pc_mx_unlock(&g_pcp.mu);
    pc_mx_unlock(&g_pcp.call);
}

// ---- 1 行の文脈（上の行 up と、その z の zp から） ----
// 0→0・1→1・2-3→2・4-7→3・8-15→4・16-31→5・32-63→6・64 以上→7。比べた数を足すだけにして、まとめて計算できる形にする
#define PC_Q(a) ((uint8_t) (((a) >= 1) + ((a) >= 2) + ((a) >= 4) + ((a) >= 8) + ((a) >= 16) + ((a) >= 32) + ((a) >= 64)))
PC_INLINE void pc_row_ctx(const uint8_t *restrict up, const uint8_t *restrict zp, uint8_t *restrict ctx, uint32_t W, const uint32_t B) {
    uint32_t x = 0;
    // 左の端（左上が無い）
    for (; x < B && x < W; x++) {
        int b = up[x], d = x + B < W ? up[x + B] : b;
        int zr = x + B < W ? zp[x + B] : 0;
        int a = pc_abs(d - b) + zp[x] + (zr >> 1);
        ctx[x] = PC_Q(a);
    }
    // 真ん中（左上も右上もある）。16 画素ずつまとめて計算する（文脈は 64 で頭打ちなので、和は 255 で止めてよい）
    uint32_t mid = W > B ? W - B : 0;
#if defined(PC_NEON)
    for (; x + 16 <= mid; x += 16) {
        uint8x16_t b = vld1q_u8(up + x), c = vld1q_u8(up + x - B), d = vld1q_u8(up + x + B);
        uint8x16_t zu = vld1q_u8(zp + x), zl = vld1q_u8(zp + x - B), zr = vld1q_u8(zp + x + B);
        uint8x16_t act = vqaddq_u8(vqaddq_u8(vabdq_u8(b, c), vabdq_u8(d, b)), vqaddq_u8(zu, vhaddq_u8(zl, zr)));
        // 0→0・1→1・2-3→2…：8 - 先頭の 0 の数（7 で止める）
        vst1q_u8(ctx + x, vminq_u8(vsubq_u8(vdupq_n_u8(8), vclzq_u8(act)), vdupq_n_u8(7)));
    }
#elif defined(PC_SSE2)
    for (; x + 16 <= mid; x += 16) {
        __m128i b = _mm_loadu_si128((const __m128i *) (up + x)), c = _mm_loadu_si128((const __m128i *) (up + x - B)), d = _mm_loadu_si128((const __m128i *) (up + x + B));
        __m128i zu = _mm_loadu_si128((const __m128i *) (zp + x)), zl = _mm_loadu_si128((const __m128i *) (zp + x - B)), zr = _mm_loadu_si128((const __m128i *) (zp + x + B));
        __m128i a1 = _mm_or_si128(_mm_subs_epu8(b, c), _mm_subs_epu8(c, b));
        __m128i a2 = _mm_or_si128(_mm_subs_epu8(d, b), _mm_subs_epu8(b, d));
        __m128i h = _mm_sub_epi8(_mm_avg_epu8(zl, zr), _mm_and_si128(_mm_xor_si128(zl, zr), _mm_set1_epi8(1))); // 切り捨ての半分
        __m128i act = _mm_adds_epu8(_mm_adds_epu8(a1, a2), _mm_adds_epu8(zu, h));
        __m128i q = _mm_setzero_si128();
        // act >= t なら 0xFF（-1）。その数を数える
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(1)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(2)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(4)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(8)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(16)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(32)), act));
        q = _mm_sub_epi8(q, _mm_cmpeq_epi8(_mm_max_epu8(act, _mm_set1_epi8(64)), act));
        _mm_storeu_si128((__m128i *) (ctx + x), q);
    }
#endif
    for (; x < mid; x++) {
        int b = up[x], c = up[x - B], d = up[x + B];
        int a = pc_abs(b - c) + pc_abs(d - b) + zp[x] + ((zp[x - B] + zp[x + B]) >> 1);
        ctx[x] = PC_Q(a);
    }
    // 右の端（右上が無い）
    for (; x < W; x++) {
        int b = up[x], c = x >= B ? up[x - B] : b;
        int zl = x >= B ? zp[x - B] : 0;
        int a = pc_abs(b - c) + zp[x] + (zl >> 1);
        ctx[x] = PC_Q(a);
    }
}

// 状態 j（0..3）が 1 行で受け持つ記号の数（x ≡ j mod 4）
static inline uint32_t pc_cnt(uint32_t W, uint32_t j) { return W > j ? (W - j + 3) / 4 : 0; }

// ---- 縮める ----
// 縮める側の表（文脈と記号ごとに 16 バイト）。割り算は掛け算に直す：x / f = (t + ((x - t) >> sh1)) >> sh2・t = x * m >> 32
typedef struct { uint32_t xmax, m; uint16_t st, cmpl; uint8_t sh1, sh2, pad[2]; } pc_es;
typedef struct { uint32_t f[PC_NCTX][256]; pc_es e[PC_NCTX][256]; } pc_etab;

// f（1..4095）で割るための掛ける数と右へずらす数（32 ビットの x のどれでも割り算と同じ答え）
static void pc_recip(uint32_t f, uint32_t *m, uint32_t *sh) {
    uint32_t l = 0;
    while ((1u << l) < f) l++;
    *m = (uint32_t) ((((uint64_t) 1 << 32) * (((uint64_t) 1 << l) - f)) / f + 1);
    *sh = (l ? 1u : 0u) | ((l ? l - 1 : 0) << 8);
}
static inline uint32_t pc_div(uint32_t x, uint32_t m, uint32_t sh) {
    uint32_t t = (uint32_t) (((uint64_t) x * m) >> 32);
    return (t + ((x - t) >> (sh & 255))) >> (sh >> 8);
}
static void pc_es_set(pc_es *e, uint32_t f, uint32_t st) {
    uint32_t m = 0, sh = 0;
    if (f) pc_recip(f, &m, &sh);
    e->xmax = f << 20;
    e->m = m;
    e->st = (uint16_t) st;
    e->cmpl = (uint16_t) (PC_M - f);
    e->sh1 = (uint8_t) (sh & 255);
    e->sh2 = (uint8_t) (sh >> 8);
    e->pad[0] = e->pad[1] = 0;
}

// 1 行の残差の番号 z（cur は今の行・up は上の行で NULL なら段の最初の行）。残差は (cur - up) の左との差
PC_INLINE void pc_row_resid(const uint8_t *restrict cur, const uint8_t *restrict up, uint8_t *restrict z, uint32_t W, const uint32_t B) {
    uint32_t x = 0;
    if (!up) {
        for (; x < B && x < W; x++) z[x] = pc_zig(cur[x]);
        for (; x < W; x++) z[x] = pc_zig((uint8_t) (cur[x] - cur[x - B]));
        return;
    }
    for (; x < B && x < W; x++) z[x] = pc_zig((uint8_t) (cur[x] - up[x]));
    for (; x < W; x++) z[x] = pc_zig((uint8_t) (cur[x] - up[x] - cur[x - B] + up[x - B]));
}

// 頻度 cnt を 4096 に合わせる。使う記号はどれも 1 以上、1 つは 4095 まで。記号の数（最大の記号 + 1）を返す。空なら 0
static uint32_t pc_norm(const uint32_t *cnt, uint32_t *f) {
    uint64_t tot = 0;
    uint32_t distinct = 0, last = 0;
    for (uint32_t s = 0; s < 256; s++) {
        f[s] = 0;
        if (cnt[s]) { tot += cnt[s]; distinct++; last = s; }
    }
    if (!tot) return 0;
    if (distinct == 1) {
        f[last] = PC_M - 1;
        f[(last + 1) & 255] = 1;
        return last == 255 ? 256 : last + 2;
    }
    uint32_t sum = 0;
    for (uint32_t s = 0; s < 256; s++)
        if (cnt[s]) {
            uint64_t v = ((uint64_t) cnt[s] * PC_M + tot / 2) / tot;
            f[s] = v < 1 ? 1 : v > PC_M - 1 ? PC_M - 1 : (uint32_t) v;
            sum += f[s];
        }
    while (sum != PC_M) {
        uint32_t best = 0, bf = 0;
        if (sum < PC_M) {
            for (uint32_t s = 0; s < 256; s++) if (f[s] && f[s] < PC_M - 1 && f[s] > bf) { bf = f[s]; best = s; }
            uint32_t add = PC_M - sum, room = PC_M - 1 - f[best];
            if (add > room) add = room;
            f[best] += add;
            sum += add;
        } else {
            for (uint32_t s = 0; s < 256; s++) if (f[s] > 1 && f[s] > bf) { bf = f[s]; best = s; }
            uint32_t sub = sum - PC_M, room = f[best] - 1;
            if (sub > room) sub = room;
            f[best] -= sub;
            sum -= sub;
        }
    }
    return last + 1;
}

static uint8_t *pc_vput(uint8_t *o, uint32_t v) {
    while (v >= 128) { *o++ = (uint8_t) (v | 128); v >>= 7; }
    *o++ = (uint8_t) v;
    return o;
}

// 段の数（seg_rows 行ずつ）
PC_UNUSED static uint32_t pc_nseg(uint32_t rows, uint32_t seg_rows) {
    if (!seg_rows || seg_rows > rows) seg_rows = rows;
    return (rows + seg_rows - 1) / seg_rows;
}

// 縮めた物の大きさの上限（段の数 nseg のとき）。流れ 1 つは記号 1 つに多くて 2 バイト
PC_UNUSED static uint64_t pc_bound(uint32_t width, uint32_t rows, uint32_t nseg) {
    return PC_HEAD + PC_NCTX * (3 + 256 * 2) + (uint64_t) nseg * (8 + 32) + 2ull * width * rows + 8ull * nseg;
}

// 縮めるときの一時の場所の大きさ
PC_UNUSED static uint64_t pc_enc_scratch(uint32_t width, uint32_t rows, uint32_t nseg) {
    // 並べ方：揃え・全画素の番号（2 バイト）・揃え・表・段ごとの数・段の場所と長さ・揃え・段ごとの出口（頭 32・流れ 4 本に 2 バイト/画素 + 8 ずつ・8 に揃える）
    return 4ull * width * rows + sizeof(pc_etab) + (uint64_t) nseg * (PC_NCTX * 256 * 4 + 12 + 80) + 512;
}

typedef struct {
    const uint8_t *src;
    uint32_t W, rows, B, seg_rows, nseg;
    uint16_t *I;         // 全画素の「文脈 × 256 + z」（表の何番目か）
    uint32_t *hist;      // 段ごとの数（nseg × 8 × 256）
    uint8_t *out;        // 段ごとの出口（段 i は out + seg_off[i] から。流れ 4 つを後ろから書き、最後に頭と並べて前に詰める）
    uint64_t *seg_off;
    uint32_t *seg_len;   // 段の頭（32 バイト）と流れを合わせたバイト
    const pc_etab *et;
    int bad;             // 一時の場所が取れなかった
} pc_ejob;

PC_INLINE void pc_enc_a_impl(pc_ejob *J, int i, const uint32_t B) {
    uint32_t W = J->W, y0 = (uint32_t) i * J->seg_rows, y1 = y0 + J->seg_rows < J->rows ? y0 + J->seg_rows : J->rows;
    uint32_t *h = J->hist + (size_t) i * PC_NCTX * 256;
    uint32_t h2[PC_NCTX * 256]; // 奇数の x の数（同じ所を続けて足すと待ちができるので 2 つに分けて、最後に合わせる）
    memset(h, 0, PC_NCTX * 256 * 4);
    memset(h2, 0, sizeof h2);
    uint8_t *zz = (uint8_t *) malloc((size_t) W * 3); // 上の行の z・今の行の z・文脈
    if (!zz) { J->bad = 1; return; }
    uint8_t *zp = zz, *z = zz + W, *c = zz + 2 * (size_t) W;
    for (uint32_t y = y0; y < y1; y++) {
        const uint8_t *cur = J->src + (size_t) y * W, *up = y > y0 ? cur - W : NULL;
        uint16_t *I = J->I + (size_t) y * W;
        pc_row_resid(cur, up, z, W, B);
        if (up) pc_row_ctx(up, zp, c, W, B);
        else memset(c, 0, W);
        for (uint32_t x = 0; x < W; x++) I[x] = (uint16_t) ((uint32_t) c[x] << 8 | z[x]);
        uint32_t x = 0;
        for (; x + 2 <= W; x += 2) {
            h[I[x]]++;
            h2[I[x + 1]]++;
        }
        if (x < W) h[I[x]]++;
        uint8_t *t = zp; zp = z; z = t;
    }
    for (uint32_t k = 0; k < PC_NCTX * 256; k++) h[k] += h2[k];
    free(zz);
}
static void pc_enc_a(void *arg, int i) {
    pc_ejob *J = (pc_ejob *) arg;
    switch (J->B) {
    case 1: pc_enc_a_impl(J, i, 1); break;
    case 2: pc_enc_a_impl(J, i, 2); break;
    default: pc_enc_a_impl(J, i, 4); break;
    }
}

// 1 つの記号を足す。あふれそうなら 16 ビットを出す。分岐にせず、出す語はいつも O - 2 に書き、出したときだけ O を動かす
// （出さないときに書いた所は、まだ使っていない所なので害が無い）
#define PC_PUT(S, O, IX)                                                    \
    do {                                                                    \
        const pc_es *e_ = es + (IX);                                        \
        uint32_t big_ = S >= e_->xmax;                                      \
        pc_st16(O - 2, S);                                                  \
        O -= big_ << 1;                                                     \
        S = big_ ? S >> 16 : S;                                             \
        uint32_t t_ = (uint32_t) (((uint64_t) S * e_->m) >> 32);            \
        uint32_t q_ = (t_ + ((S - t_) >> e_->sh1)) >> e_->sh2;              \
        S += e_->st + q_ * e_->cmpl;                                        \
    } while (0)

static void pc_enc_b(void *arg, int i) {
    pc_ejob *J = (pc_ejob *) arg;
    const pc_es *es = &J->et->e[0][0];
    uint32_t W = J->W, y0 = (uint32_t) i * J->seg_rows, y1 = y0 + J->seg_rows < J->rows ? y0 + J->seg_rows : J->rows, nr = y1 - y0;
    // 流れ j の出口：beg_j から cap_j バイト（後ろから書く）
    uint8_t *base = J->out + J->seg_off[i];
    uint8_t *beg[4], *o0, *o1, *o2, *o3;
    uint64_t at = 32;
    for (uint32_t j = 0; j < 4; j++) {
        beg[j] = base + at;
        at += 2ull * pc_cnt(W, j) * nr + 8;
    }
    o0 = beg[1]; o1 = beg[2]; o2 = beg[3]; o3 = base + at;
    uint32_t s0 = PC_L, s1 = PC_L, s2 = PC_L, s3 = PC_L;
    uint32_t W4 = W & ~3u;
    for (uint32_t y = y1; y-- > y0;) {
        const uint16_t *I = J->I + (size_t) y * W;
        for (uint32_t x = W; x-- > W4;) {
            switch (x & 3) {
            case 0: PC_PUT(s0, o0, I[x]); break;
            case 1: PC_PUT(s1, o1, I[x]); break;
            default: PC_PUT(s2, o2, I[x]); break;
            }
        }
        for (uint32_t x = W4; x >= 4;) {
            x -= 4;
            PC_PUT(s3, o3, I[x + 3]);
            PC_PUT(s2, o2, I[x + 2]);
            PC_PUT(s1, o1, I[x + 1]);
            PC_PUT(s0, o0, I[x]);
        }
    }
    // 段の頭：流れのバイト ×4・状態の初め ×4。そのあとに流れを詰める
    uint8_t *ends[4] = { beg[1], beg[2], beg[3], base + at }, *os[4] = { o0, o1, o2, o3 };
    uint32_t st[4] = { s0, s1, s2, s3 };
    uint8_t head[32];
    for (uint32_t j = 0; j < 4; j++) {
        pc_wr32(head + j * 4, (uint32_t) (ends[j] - os[j]));
        pc_wr32(head + 16 + j * 4, st[j]);
    }
    uint8_t *w = base + 32;
    for (uint32_t j = 0; j < 4; j++) {
        size_t len = (size_t) (ends[j] - os[j]);
        memmove(w, os[j], len); // 後ろの流れほど右にあるので、前から順に詰めれば重ならない所を壊さない
        w += len;
    }
    memcpy(base, head, 32);
    J->seg_len[i] = (uint32_t) (w - base);
}
#undef PC_PUT

// src（width × rows バイト、隙間なし）を縮めて out に書き、書いたバイトを返す（out は pc_bound 以上、scratch は pc_enc_scratch 以上）。
// bpp は 1・2・4。seg_rows 行ごとに段を分ける（0 なら 1 つ）。いくつもの糸から同時に呼んでよい（場所は呼ぶ側が別々に渡す）
PC_UNUSED static uint64_t pc_encode(const uint8_t *src, uint32_t width, uint32_t rows, uint32_t bpp, uint32_t seg_rows, uint8_t *out, uint8_t *scratch) {
    if (!width || !rows || (bpp != 1 && bpp != 2 && bpp != 4) || width > PC_MAX_W || (uint64_t) width * rows > PC_MAX_BYTES) return 0;
    if (!seg_rows || seg_rows > rows) seg_rows = rows;
    uint32_t nseg = pc_nseg(rows, seg_rows);
    uint64_t npx = (uint64_t) width * rows;
    pc_ejob J;
    memset(&J, 0, sizeof J);
    J.src = src; J.W = width; J.rows = rows; J.B = bpp; J.seg_rows = seg_rows; J.nseg = nseg;
    uint8_t *sp = (uint8_t *) (((uintptr_t) scratch + 63) & ~(uintptr_t) 63);
    J.I = (uint16_t *) sp; sp += 2 * npx;
    sp = (uint8_t *) (((uintptr_t) sp + 63) & ~(uintptr_t) 63);
    pc_etab *et = (pc_etab *) sp; sp += sizeof *et;
    J.hist = (uint32_t *) sp; sp += (size_t) nseg * PC_NCTX * 256 * 4;
    J.seg_off = (uint64_t *) sp; sp += (size_t) nseg * 8;
    J.seg_len = (uint32_t *) sp; sp += (size_t) nseg * 4;
    sp = (uint8_t *) (((uintptr_t) sp + 63) & ~(uintptr_t) 63);
    J.out = sp;
    uint64_t at = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        uint32_t r = i + 1 < nseg ? seg_rows : rows - seg_rows * (nseg - 1);
        J.seg_off[i] = at;
        at += 32 + 2ull * width * r + 32;
        at = (at + 7) & ~7ull;
    }
    PC_TICK(e0);
    pc_par((int) nseg, pc_enc_a, &J);
    if (J.bad) return 0;
    PC_TICK(e1);
    PC_ACC(3, e0, e1);
    // 数をまとめて表にする
    uint32_t cnt[256];
    uint8_t *o = out + PC_HEAD;
    for (uint32_t k = 0; k < PC_NCTX; k++) {
        for (uint32_t s = 0; s < 256; s++) {
            uint32_t v = 0;
            for (uint32_t i = 0; i < nseg; i++) v += J.hist[((size_t) i * PC_NCTX + k) * 256 + s];
            cnt[s] = v;
        }
        uint32_t n = pc_norm(cnt, et->f[k]);
        o = pc_vput(o, n);
        uint32_t st = 0;
        for (uint32_t s = 0; s < 256; s++) {
            pc_es_set(&et->e[k][s], et->f[k][s], st);
            st += et->f[k][s];
            if (s < n) o = pc_vput(o, et->f[k][s]);
        }
    }
    uint32_t tab_len = (uint32_t) (o - (out + PC_HEAD));
    J.et = et;
    PC_TICK(e2);
    PC_ACC(4, e1, e2);
    pc_par((int) nseg, pc_enc_b, &J);
    PC_TICK(e3);
    PC_ACC(5, e2, e3);
    pc_wr32(out, PC_CODEC_1);
    pc_wr32(out + 4, bpp);
    pc_wr32(out + 8, width);
    pc_wr32(out + 12, rows);
    pc_wr32(out + 16, nseg);
    pc_wr32(out + 20, tab_len);
    for (uint32_t i = 0; i < nseg; i++) {
        uint32_t r = i + 1 < nseg ? seg_rows : rows - seg_rows * (nseg - 1);
        pc_wr32(o, r);
        pc_wr32(o + 4, J.seg_len[i]);
        o += 8;
    }
    for (uint32_t i = 0; i < nseg; i++) {
        memcpy(o, J.out + J.seg_off[i], J.seg_len[i]);
        o += J.seg_len[i];
    }
    return (uint64_t) (o - out);
}

// ---- ほどく ----
// 表：文脈と場所 → 記号（8 ビット）と、文脈と記号 → 頻度（下 16 ビット）・始まり（上 16 ビット）。合わせて 40 KB（L1 に入る）。
// ほどく所では 1 つの番地から両方を引く（x86 の少ないレジスタで、読む所の番地を退避させないため）
typedef struct { uint8_t sym[PC_NCTX][PC_M]; uint32_t fs[PC_NCTX][256]; } pc_dtab;
#define PC_FS_AT (PC_NCTX * PC_M)

static int pc_vget(const uint8_t **p, const uint8_t *end, uint32_t *v) {
    uint32_t r = 0;
    for (int sh = 0; sh < 28; sh += 7) {
        if (*p >= end) return 0;
        uint8_t b = *(*p)++;
        r |= (uint32_t) (b & 127) << sh;
        if (!(b & 128)) { *v = r; return 1; }
    }
    return 0;
}

// 中身の頭を読む。ほどいたときの大きさ（幅 × 行の数）を返す。読めなければ 0
PC_UNUSED static uint64_t pc_blob_bytes(const uint8_t *b, size_t n) {
    if (n < PC_HEAD) return 0;
    uint32_t codec = pc_rd32(b), bpp = pc_rd32(b + 4), W = pc_rd32(b + 8), rows = pc_rd32(b + 12);
    if (codec != PC_CODEC_1 || (bpp != 1 && bpp != 2 && bpp != 4) || !W || W > PC_MAX_W || !rows || (uint64_t) W * rows > PC_MAX_BYTES) return 0;
    return (uint64_t) W * rows;
}

typedef struct {
    const pc_dtab *dt;
    uint8_t *dst;
    uint32_t W, B;
    const uint8_t **sp;  // 段ごとの始まり（段の頭 32 バイトから）
    const uint32_t *slen, *srow, *srows;
    const uint8_t *bend; // 中身の終わり（これより先は読まない）
    int *bad;
} pc_djob;

// 1 つの記号をほどく。S は状態・P は読む所。zc[X] は文脈を読んで、ほどいた番号 z に置き換える
#define PC_DEC(S, P, X)                                                      \
    do {                                                                     \
        uint32_t sl_ = S & (PC_M - 1);                                       \
        uint32_t k_ = zc[X];                                                 \
        uint32_t sy_ = tb[(k_ << PC_SB) + sl_];                              \
        uint32_t fs_;                                                        \
        memcpy(&fs_, tb + PC_FS_AT + (k_ << 10) + (sy_ << 2), 4);            \
        zc[X] = (uint8_t) sy_;                                               \
        S = (fs_ & 0xffff) * (S >> PC_SB) + sl_ - (fs_ >> 16);              \
        uint32_t nd_ = S < PC_L, w_ = pc_ld16(P);                            \
        S = nd_ ? (S << 16) | w_ : S;                                        \
        P += nd_ << 1;                                                       \
    } while (0)

PC_INLINE void pc_dec_seg_impl(pc_djob *J, int i, const uint32_t B) {
    const uint32_t W = J->W, rows = J->srows[i];
    const uint8_t *tb = (const uint8_t *) J->dt;
    uint8_t *dst = J->dst + (size_t) J->srow[i] * W;
    const uint8_t *seg = J->sp[i], *send = seg + J->slen[i];
    // 段の頭：流れのバイト ×4・状態の初め ×4
    const uint8_t *ps[4], *pe[4], *lim[4];
    uint32_t cnt[4], cmax = pc_cnt(W, 0);
    uint64_t at = 32;
    for (uint32_t j = 0; j < 4; j++) {
        uint32_t len = pc_rd32(seg + j * 4);
        cnt[j] = pc_cnt(W, j);
        if (len & 1 || at + len > J->slen[i]) { __atomic_store_n(J->bad, 1, __ATOMIC_RELAXED); return; }
        ps[j] = seg + at;
        at += len;
        pe[j] = seg + at;
        lim[j] = J->bend; // 読んでよいのは中身の終わりまで（となりの流れまで読んでも外には出ない）
    }
    if (at != J->slen[i]) { __atomic_store_n(J->bad, 1, __ATOMIC_RELAXED); return; }
    (void) send;
    uint32_t s0 = pc_rd32(seg + 16), s1 = pc_rd32(seg + 20), s2 = pc_rd32(seg + 24), s3 = pc_rd32(seg + 28);
    const uint8_t *p0 = ps[0], *p1 = ps[1], *p2 = ps[2], *p3 = ps[3];
    // 行の場所：上の行・今の行・上の z・今の文脈（ほどくと z に置き換わる）。終わり近くの流れは詰め物つきの写しから読む
    size_t tcap = (size_t) cmax * 4 + 64;
    uint8_t *mem = (uint8_t *) malloc((size_t) W * 4 + tcap * 4);
    if (!mem) { __atomic_store_n(J->bad, 1, __ATOMIC_RELAXED); return; }
    uint8_t *up = mem, *cur = up + W, *zp = cur + W, *zc = zp + W, *tails = zc + W;
    int in_tail[4] = { 0, 0, 0, 0 }, bad = 0;
    for (uint32_t y = 0; y < rows && !bad; y++) {
        // 1 行で流れ j から読むのは多くて 2 × cnt[j] バイト（と、読み飛ばす 2 バイト）。足りなければ、その流れの残りを詰め物つきの写しに移す
        const uint8_t *pa[4] = { p0, p1, p2, p3 };
        for (uint32_t j = 0; j < 4; j++) {
            const uint8_t *p = pa[j];
            if (lim[j] - p >= (ptrdiff_t) (2 * cnt[j] + 2)) continue;
            if (in_tail[j] || p > pe[j]) { bad = 1; break; }
            uint8_t *t = tails + tcap * j;
            size_t left = (size_t) (pe[j] - p);
            if (left > tcap - (2 * cnt[j] + 2)) { bad = 1; break; }
            memset(t, 0, tcap);
            memcpy(t, p, left);
            pa[j] = t;
            pe[j] = t + left;
            lim[j] = t + tcap;
            in_tail[j] = 1;
        }
        if (bad) break;
        p0 = pa[0]; p1 = pa[1]; p2 = pa[2]; p3 = pa[3];
        PC_TICK(c0);
        if (y) pc_row_ctx(up, zp, zc, W, B);
        else memset(zc, 0, W);
        PC_TICK(c1);
        PC_ACC(0, c0, c1);
        uint32_t x = 0;
        const uint32_t W4 = W & ~3u;
        for (; x < W4; x += 4) {
            PC_DEC(s0, p0, x);
            PC_DEC(s1, p1, x + 1);
            PC_DEC(s2, p2, x + 2);
            PC_DEC(s3, p3, x + 3);
        }
        for (; x < W; x++) {
            switch (x & 3) {
            case 0: PC_DEC(s0, p0, x); break;
            case 1: PC_DEC(s1, p1, x); break;
            default: PC_DEC(s2, p2, x); break;
            }
        }
        PC_TICK(c2);
        PC_ACC(1, c1, c2);
        // 画素に戻す：(今の行 - 上の行) は残差を左から積み上げた物（bpp ごとに別に積む）。段の最初の行は上を 0 とする
        if (B == 1) {
            uint32_t acc = 0;
            if (y) for (x = 0; x < W; x++) { acc += pc_unzig(zc[x]); cur[x] = (uint8_t) (up[x] + acc); }
            else for (x = 0; x < W; x++) { acc += pc_unzig(zc[x]); cur[x] = (uint8_t) acc; }
        } else {
            for (x = 0; x < B && x < W; x++) cur[x] = (uint8_t) ((y ? up[x] : 0) + pc_unzig(zc[x]));
            if (y) for (; x < W; x++) cur[x] = (uint8_t) (cur[x - B] - up[x - B] + up[x] + pc_unzig(zc[x]));
            else for (; x < W; x++) cur[x] = (uint8_t) (cur[x - B] + pc_unzig(zc[x]));
        }
        memcpy(dst + (size_t) y * W, cur, W);
        PC_TICK(c3);
        PC_ACC(2, c2, c3);
        uint8_t *t = up; up = cur; cur = t;
        t = zp; zp = zc; zc = t;
    }
    if (bad || p0 != pe[0] || p1 != pe[1] || p2 != pe[2] || p3 != pe[3] || s0 != PC_L || s1 != PC_L || s2 != PC_L || s3 != PC_L)
        __atomic_store_n(J->bad, 1, __ATOMIC_RELAXED);
    free(mem);
}
#undef PC_DEC
static void pc_dec_seg(void *arg, int i) {
    pc_djob *J = (pc_djob *) arg;
    switch (J->B) {
    case 1: pc_dec_seg_impl(J, i, 1); break;
    case 2: pc_dec_seg_impl(J, i, 2); break;
    default: pc_dec_seg_impl(J, i, 4); break;
    }
}

// b（n バイト）をほどいて dst に書く（dst は pc_blob_bytes バイト以上）。うまくいけば 0。
// 壊れた中身でも dst の外には書かず、b の外は読まない（dst の中身はでたらめになる）。err に理由
PC_UNUSED static int pc_decode(const uint8_t *b, size_t n, uint8_t *dst, const char **err) {
    static const char *e_head = "頭が読めない", *e_tab = "表が壊れている", *e_seg = "段の並びが壊れている", *e_mem = "メモリが足りない", *e_data = "流れが壊れている";
    if (!pc_blob_bytes(b, n)) { *err = e_head; return -1; }
    uint32_t bpp = pc_rd32(b + 4), W = pc_rd32(b + 8), rows = pc_rd32(b + 12), nseg = pc_rd32(b + 16), tab_len = pc_rd32(b + 20);
    if (!nseg || nseg > rows || tab_len > n - PC_HEAD) { *err = e_seg; return -1; }
    const uint8_t *p = b + PC_HEAD, *tend = p + tab_len;
    pc_dtab *dt = (pc_dtab *) malloc(sizeof *dt);
    const uint8_t **sp = (const uint8_t **) malloc(nseg * sizeof *sp);
    uint32_t *slen = (uint32_t *) malloc(nseg * 4 * 3);
    if (!dt || !sp || !slen) { free(dt); free(sp); free(slen); *err = e_mem; return -1; }
    uint32_t *srow = slen + nseg, *srows = srow + nseg;
    int ok = 1;
    for (uint32_t k = 0; k < PC_NCTX && ok; k++) {
        uint32_t ns, f, st = 0;
        if (!pc_vget(&p, tend, &ns) || ns > 256) { ok = 0; break; }
        memset(dt->fs[k], 0, sizeof dt->fs[k]);
        if (!ns) {
            // 使わない文脈：でたらめな流れでも表の外を読まないよう、仮の表を入れる（記号 0 が 4095・記号 1 が 1）
            memset(dt->sym[k], 0, PC_M - 1);
            dt->sym[k][PC_M - 1] = 1;
            dt->fs[k][0] = PC_M - 1;
            dt->fs[k][1] = 1u | ((PC_M - 1) << 16);
            continue;
        }
        for (uint32_t s = 0; s < ns; s++) {
            if (!pc_vget(&p, tend, &f) || f > PC_M - 1 || st + f > PC_M) { ok = 0; break; }
            dt->fs[k][s] = f | (st << 16);
            memset(dt->sym[k] + st, (int) s, f);
            st += f;
        }
        if (ok && st != PC_M) ok = 0;
    }
    if (!ok || p != tend) { free(dt); free(sp); free(slen); *err = e_tab; return -1; }
    // 段の並び
    p = tend;
    if ((size_t) (b + n - p) < (size_t) nseg * 8) { free(dt); free(sp); free(slen); *err = e_seg; return -1; }
    const uint8_t *data = p + (size_t) nseg * 8;
    uint64_t at = 0, rsum = 0;
    for (uint32_t i = 0; i < nseg; i++) {
        srows[i] = pc_rd32(p + i * 8);
        slen[i] = pc_rd32(p + i * 8 + 4);
        srow[i] = (uint32_t) rsum;
        rsum += srows[i];
        sp[i] = data + at;
        at += slen[i];
        if (!srows[i] || rsum > rows || slen[i] < 32) ok = 0;
    }
    if (!ok || rsum != rows || at != (uint64_t) (b + n - data)) { free(dt); free(sp); free(slen); *err = e_seg; return -1; }
    int bad = 0;
    pc_djob J = { dt, dst, W, bpp, sp, slen, srow, srows, b + n, &bad };
    pc_par((int) nseg, pc_dec_seg, &J);
    free(dt);
    free(sp);
    free(slen);
    if (bad) { *err = e_data; return -1; }
    return 0;
}
