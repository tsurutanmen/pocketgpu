// MoltenVK の設定を PC から渡すための、許した名前の表と読み取り（受け手 server.c と試験が使う。Vulkan には頼らない）。
// PC は IPHONE_VK_MVK="SYNCHRONOUS_QUEUE_SUBMITS=0,FAST_MATH_ENABLED=1" の形で書く。受け手はこの表にある名前と範囲だけを受ける。
// 何でも MoltenVK に渡さない。表に無い名前・数でない値・範囲の外は、断って理由を返す。
// MoltenVK の設定の値は、VK_EXT_layer_settings の pValues を型を確かめずに読まれる（MoltenVK 1.4.2 の MVKInstance::initMVKConfig）。
// そのため値の大きさは MoltenVK の側の型に合わせる必要がある。下の表の物はどれも 4 バイト（VkBool32・uint32_t・enum）。
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MVKCFG_MAX 8            // 1 回に渡せる設定の数
#define MVKCFG_TEXT_MAX 512     // 設定の文字列の長さの上限（終わりの 0 を含まない）

typedef struct {
    const char *name;           // MVK_CONFIG_ を除いた名前（大文字）
    int is_bool;                // 1 なら VkBool32（layer settings の型は BOOL32）。0 なら uint32（UINT32）
    uint32_t lo, hi;            // 許す値の範囲
} mvkcfg_def;

// 許した設定。速さに効くと調べ物（research1015/moltenvk_tuning.md）で挙がった物だけ。
// 入れない物：PREFILL_METAL_COMMAND_BUFFERS（0 以外はメモリが増え update-after-bind と合わない）・VK_SEMAPHORE_SUPPORT_STYLE（遅い道に変わる）・
//             METAL_COMPILE_TIMEOUT（64 ビット）・ログや GPU キャプチャ（調べ用）・API_VERSION_TO_ADVERTISE と ADVERTISE_EXTENSIONS（PC に見せる性質が変わる）
static const mvkcfg_def mvkcfg_defs[] = {
    { "SYNCHRONOUS_QUEUE_SUBMITS",                  1, 0, 1 },
    { "FAST_MATH_ENABLED",                          0, 0, 2 },   // 0 なし・1 常に・2 シェーダーごとに許可（既定）
    { "USE_METAL_ARGUMENT_BUFFERS",                 1, 0, 1 },
    { "MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE", 0, 1, 1024 },
    { "USE_MTLHEAP",                                0, 0, 2 },
    { "USE_COMMAND_POOLING",                        1, 0, 1 },
};
#define MVKCFG_NDEFS ((int) (sizeof mvkcfg_defs / sizeof mvkcfg_defs[0]))

typedef struct {
    int n;
    uint8_t idx[MVKCFG_MAX];    // mvkcfg_defs の番号
    uint32_t val[MVKCFG_MAX];
} mvkcfg_set;

static inline void mvkcfg_clear(mvkcfg_set *s) { memset(s, 0, sizeof *s); }

static inline int mvkcfg_equal(const mvkcfg_set *a, const mvkcfg_set *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++) if (a->idx[i] != b->idx[i] || a->val[i] != b->val[i]) return 0;
    return 1;
}

static inline char mvkcfg_up(char c) { return c >= 'a' && c <= 'z' ? (char) (c - 32) : c; }

// 書き出し先（rej）に断った理由を足す。cap を超えたら黙って切る
static inline void mvkcfg_note(char *rej, size_t cap, const char *name, size_t nlen, const char *why) {
    if (!rej || !cap) return;
    size_t at = strlen(rej);
    char tmp[96];
    size_t k = 0;
    for (size_t i = 0; i < nlen && k < 40; i++) {
        unsigned char c = (unsigned char) name[i];
        tmp[k++] = c >= 0x20 && c < 0x7f ? (char) c : '?';   // 制御文字・8 ビットの文字は記録に出さない
    }
    tmp[k] = 0;
    size_t wl = strlen(why);
    if (at + k + wl + 6 >= cap) return;
    if (at) rej[at++] = ' ';
    memcpy(rej + at, tmp, k); at += k;
    rej[at++] = '(';
    memcpy(rej + at, why, wl); at += wl;
    rej[at++] = ')';
    rej[at] = 0;
}

// 読む。"名前=値" を ,（か ; か空白）で並べた文字列。名前は MVK_CONFIG_ を付けても付けなくてもよく、大文字小文字は問わない。値は 10 進数の整数。
// 同じ名前が 2 回あれば後のが勝つ。戻り値は断った数（理由は rej に "名前(理由) …" の形で入る。rej は呼ぶ側が最初に 0 で埋める）。
// 長すぎる文字列・数が多すぎる指定は、全部断る
static inline int mvkcfg_parse(const char *s, size_t len, mvkcfg_set *out, char *rej, size_t rejcap) {
    int bad = 0;
    mvkcfg_clear(out);
    if (rej && rejcap) rej[0] = 0;
    if (!s) return 0;
    if (len > MVKCFG_TEXT_MAX) { mvkcfg_note(rej, rejcap, "text", 4, "too long"); return 1; }
    size_t i = 0;
    while (i < len) {
        while (i < len && (s[i] == ',' || s[i] == ';' || s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == 0)) i++;
        if (i >= len) break;
        size_t t0 = i;
        while (i < len && !(s[i] == ',' || s[i] == ';' || s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == 0)) i++;
        const char *tk = s + t0;
        size_t tl = i - t0, eq = 0;
        while (eq < tl && tk[eq] != '=') eq++;
        if (eq == tl) { mvkcfg_note(rej, rejcap, tk, tl, "no =value"); bad++; continue; }
        const char *nm = tk;
        size_t nl = eq;
        if (nl > 11 && !strncmp(nm, "MVK_CONFIG_", 11)) { nm += 11; nl -= 11; }
        else if (nl > 11) {
            int pre = 1;
            for (int k = 0; k < 11; k++) if (mvkcfg_up(nm[k]) != "MVK_CONFIG_"[k]) { pre = 0; break; }
            if (pre) { nm += 11; nl -= 11; }
        }
        int di = -1;
        for (int d = 0; d < MVKCFG_NDEFS && di < 0; d++) {
            size_t dl = strlen(mvkcfg_defs[d].name);
            if (dl != nl) continue;
            int same = 1;
            for (size_t k = 0; k < nl; k++) if (mvkcfg_up(nm[k]) != mvkcfg_defs[d].name[k]) { same = 0; break; }
            if (same) di = d;
        }
        if (di < 0) { mvkcfg_note(rej, rejcap, tk, eq, "not allowed"); bad++; continue; }
        const char *v = tk + eq + 1;
        size_t vl = tl - eq - 1;
        if (vl == 0 || vl > 10) { mvkcfg_note(rej, rejcap, tk, eq, "bad value"); bad++; continue; }
        uint64_t x = 0;
        int digits = 1;
        for (size_t k = 0; k < vl; k++) { if (v[k] < '0' || v[k] > '9') { digits = 0; break; } x = x * 10 + (uint64_t) (v[k] - '0'); }
        if (!digits) { mvkcfg_note(rej, rejcap, tk, eq, "not a number"); bad++; continue; }
        if (x < mvkcfg_defs[di].lo || x > mvkcfg_defs[di].hi) { mvkcfg_note(rej, rejcap, tk, eq, "out of range"); bad++; continue; }
        int at = -1;
        for (int k = 0; k < out->n; k++) if (out->idx[k] == di) at = k;
        if (at < 0) {
            if (out->n >= MVKCFG_MAX) { mvkcfg_note(rej, rejcap, tk, eq, "too many"); bad++; continue; }
            at = out->n++;
        }
        out->idx[at] = (uint8_t) di;
        out->val[at] = (uint32_t) x;
    }
    return bad;
}

// "名前=値,名前=値" に書き出す（記録とドライバへの返事に使う）。書いた長さを返す
static inline size_t mvkcfg_format(const mvkcfg_set *s, char *buf, size_t cap) {
    size_t at = 0;
    if (!cap) return 0;
    buf[0] = 0;
    for (int i = 0; i < s->n; i++) {
        const char *nm = mvkcfg_defs[s->idx[i]].name;
        size_t nl = strlen(nm);
        char num[16];
        int k = 0;
        uint32_t v = s->val[i];
        char rev[12];
        int rl = 0;
        do { rev[rl++] = (char) ('0' + v % 10); v /= 10; } while (v);
        while (rl) num[k++] = rev[--rl];
        num[k] = 0;
        if (at + nl + 1 + (size_t) k + 2 >= cap) break;
        if (i) buf[at++] = ',';
        memcpy(buf + at, nm, nl); at += nl;
        buf[at++] = '=';
        memcpy(buf + at, num, (size_t) k); at += (size_t) k;
        buf[at] = 0;
    }
    return at;
}
