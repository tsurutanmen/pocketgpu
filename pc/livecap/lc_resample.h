// 音の速さを変える（例 48 kHz → 16 kHz）。numpy の無い、塊ごとに続けて使える作り。
// 作り：Kaiser 窓の sinc で低域だけを通す FIR を、比 L/M の位相ごとに前もって並べておく（ポリフェーズ）。
//   出す 1 コマごとに、入りの 2K コマとその位相の係数の積の和を取る。
//   通す所は 7.0 kHz まで、8.0 kHz から上は 70 dB 落とす（16 kHz にしたときに折り返す音が 8 kHz より下に入らない）。
//   44.1 kHz のように割り切れない比も、L/M の整数で正確に回るのでずれがたまらない。
//   中身は K コマ（どの rate でも約 2.2 ms）遅れるが、届いた分だけで出せる（先を待たない）。
#pragma once

typedef struct {
    int fin, fout, L, M, K, taps, pass;
    float *coef;           // L 個の位相 × taps
    float *buf;            // 入りの音。buf[0] の通し番号が base
    int len, cap;
    long long base, next_num, total;
} Rs;

static double rs_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 200; k++) {
        double h = x / (2.0 * k);
        t *= h * h;
        s += t;
        if (t < 1e-14 * s) break;
    }
    return s;
}
static long long rs_gcd(long long a, long long b) {
    while (b) { long long t = a % b; a = b; b = t; }
    return a;
}

static __attribute__((unused)) void rs_free(Rs *r) {
    free(r->coef);
    free(r->buf);
    memset(r, 0, sizeof *r);
}

static int rs_init(Rs *r, int fin, int fout) {
    memset(r, 0, sizeof *r);
    r->fin = fin;
    r->fout = fout;
    if (fin == fout) { r->pass = 1; return 1; }
    long long g = rs_gcd(fin, fout), L = fout / g, M = fin / g;
    if (L > 1024) {  // 半端な rate（例 47999）は 1024 位相で近く回す（ずれは 100 万分の 1 の桁）
        M = llround((double) fin * 1024.0 / fout);
        L = 1024;
    }
    r->L = (int) L;
    r->M = (int) M;
    double lo = fin < fout ? fin : fout;
    double tw = lo / 16.0;  // 移る幅（通す所の端から落とす所の端まで）
    if (tw > 1000) tw = 1000;
    double fc = lo / 2 - tw / 2;  // 半分に落ちる所。16 kHz なら 7.5 kHz
    const double A = 70.0, beta = 0.1102 * (A - 8.7);
    int K = (int) ceil((A - 7.95) * fin / (14.36 * tw) / 2.0);
    if (K < 4) K = 4;
    r->K = K;
    r->taps = 2 * K;
    r->coef = malloc(sizeof(float) * (size_t) r->L * (size_t) r->taps);
    double *tmp = malloc(sizeof(double) * (size_t) r->taps);
    if (!r->coef || !tmp) { free(tmp); return 0; }
    double fcn = fc / fin, i0b = rs_i0(beta);
    for (int ph = 0; ph < r->L; ph++) {
        double sum = 0;
        for (int j = 0; j < r->taps; j++) {
            double tau = (double) (j - K + 1) - (double) ph / r->L;  // 入りのコマと、出すコマの位置との差（入りのコマの単位）
            double x = tau / K;
            double w = fabs(x) >= 1.0 ? 0.0 : rs_i0(beta * sqrt(1.0 - x * x)) / i0b;
            double s = fabs(tau) < 1e-12 ? 2.0 * fcn : sin(2.0 * LC_PI * fcn * tau) / (LC_PI * tau);
            tmp[j] = s * w;
            sum += tmp[j];
        }
        float *c = r->coef + (size_t) ph * r->taps;
        for (int j = 0; j < r->taps; j++) c[j] = (float) (tmp[j] / sum);  // 位相ごとに直流の利得を 1 に
    }
    free(tmp);
    r->cap = r->taps + 8192;
    r->buf = calloc((size_t) r->cap, sizeof(float));
    if (!r->buf) return 0;
    // 先を待たない形にする：入りの頭に K コマの無音を足したことにして、出す位置を K コマ（約 2.2 ms）後ろにずらす。
    // 前後対称のまま出す位置を合わせると、出す 1 コマごとに先の K コマを待つので、WASAPI の 10 ms の塊の最後の 2.2 ms が
    // 次の塊まで出られず、送る塊が毎回 10 ms 遅れていた（10/11 に loopback で測った）
    r->len = 2 * K - 1;
    r->base = -(K - 1);
    r->total = K;
    return 1;
}

// 1 回に渡せる入りのコマ数 n に対して、出る数の上限
static int rs_out_max(const Rs *r, int n) {
    return (int) ((double) n * r->fout / r->fin) + 4;
}

// x を足して、出せるだけ out に出す。出した数を返す
static int rs_run(Rs *r, const float *x, int n, float *out) {
    if (r->pass) {
        memcpy(out, x, sizeof(float) * (size_t) n);
        return n;
    }
    if (r->len + n > r->cap) {
        int c = r->cap;
        while (r->len + n > c) c *= 2;
        float *q = realloc(r->buf, sizeof(float) * (size_t) c);
        if (!q) return 0;
        r->buf = q;
        r->cap = c;
    }
    memcpy(r->buf + r->len, x, sizeof(float) * (size_t) n);
    r->len += n;
    r->total += n;
    int k = 0;
    const int taps = r->taps;
    for (;;) {
        long long i = r->next_num / r->L;
        int ph = (int) (r->next_num % r->L);
        if (i + r->K > r->total - 1) break;
        const float *s = r->buf + (i - r->K + 1 - r->base);
        const float *c = r->coef + (size_t) ph * taps;
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        int j = 0;
        for (; j + 4 <= taps; j += 4) {
            a0 += s[j] * c[j];
            a1 += s[j + 1] * c[j + 1];
            a2 += s[j + 2] * c[j + 2];
            a3 += s[j + 3] * c[j + 3];
        }
        for (; j < taps; j++) a0 += s[j] * c[j];
        out[k++] = (a0 + a1) + (a2 + a3);
        r->next_num += r->M;
    }
    long long drop = (r->next_num / r->L - r->K + 1) - r->base;  // もう使わない頭を捨てる
    if (drop > 0) {
        if (drop > r->len) drop = r->len;
        memmove(r->buf, r->buf + drop, sizeof(float) * (size_t) (r->len - drop));
        r->len -= (int) drop;
        r->base += drop;
    }
    return k;
}

static void f32_to_s16(const float *x, int16_t *y, int n) {
    for (int i = 0; i < n; i++) {
        float v = x[i] * 32767.0f;
        y[i] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int16_t) lrintf(v);
    }
}
