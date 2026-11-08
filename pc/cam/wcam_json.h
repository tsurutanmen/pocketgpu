// camctl2：パイプの 1 行の JSON を作る・読む小さな道具（webcam_rx と camctl_cli が使う）。
// 読むのは 1 段の object の中の文字列・数・true/false/null だけ（入れ子の値は読み飛ばす）
#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- 作る ----
typedef struct { char *p; size_t n, cap; } jbuf;

static inline void jb_put(jbuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 1024;
        while (c < b->n + n + 1) c *= 2;
        char *q = (char *) realloc(b->p, c);
        if (!q) return;
        b->p = q, b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static inline void jb_s(jbuf *b, const char *s) { jb_put(b, s, strlen(s)); }
static inline void jb_f(jbuf *b, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) jb_put(b, t, (size_t) n < sizeof t ? (size_t) n : sizeof t - 1);
}
// 引用符つきの文字列（" と \ と制御文字を逃がす。UTF-8 はそのまま）
static inline void jb_q(jbuf *b, const char *s) {
    jb_s(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\') { char e[3] = { '\\', (char) c, 0 }; jb_s(b, e); }
        else if (c < 0x20) jb_f(b, "\\u%04x", c);
        else jb_put(b, (const char *) &c, 1);
    }
    jb_s(b, "\"");
}
static inline void jb_free(jbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

// ---- 読む ----
enum { JV_NONE = 0, JV_STR, JV_NUM, JV_BOOL, JV_NULL, JV_OTHER };
typedef struct { int type; double num; char str[256]; } jval;

static inline const char *js_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

// 文字列を読む（p は " の上）。out が NULL なら読み飛ばすだけ。返すのは閉じた " の次。形が変なら NULL
static inline const char *js_str(const char *p, char *out, size_t cap) {
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            if (!e) return NULL;
            if (e == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    char h = *p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned) (h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned) (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned) (h - 'A' + 10);
                    else return NULL;
                }
                // UTF-8 にする（サロゲートは ? にする。ここで読む鍵と値に日本語の \u は来ない）
                char u[4];
                int k = 0;
                if (v < 0x80) u[k++] = (char) v;
                else if (v < 0x800) { u[k++] = (char) (0xc0 | (v >> 6)); u[k++] = (char) (0x80 | (v & 63)); }
                else if (v >= 0xd800 && v < 0xe000) u[k++] = '?';
                else { u[k++] = (char) (0xe0 | (v >> 12)); u[k++] = (char) (0x80 | ((v >> 6) & 63)); u[k++] = (char) (0x80 | (v & 63)); }
                for (int i = 0; i < k; i++) if (out && n + 1 < cap) out[n++] = u[i];
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        }
        if (out && n + 1 < cap) out[n++] = c;
    }
    if (*p != '"') return NULL;
    if (out) out[n] = 0;
    return p + 1;
}

// 値を 1 つ読む（入れ子の object と array は数えて飛ばす）
static inline const char *js_val(const char *p, jval *v) {
    p = js_ws(p);
    if (v) memset(v, 0, sizeof *v);
    if (*p == '"') { if (v) v->type = JV_STR; return js_str(p, v ? v->str : NULL, sizeof v->str); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = js_str(p, NULL, 0); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) { if (v) v->type = JV_OTHER; return p + 1; } }
            p++;
        }
        return NULL;
    }
    if (!strncmp(p, "true", 4)) { if (v) v->type = JV_BOOL, v->num = 1; return p + 4; }
    if (!strncmp(p, "false", 5)) { if (v) v->type = JV_BOOL, v->num = 0; return p + 5; }
    if (!strncmp(p, "null", 4)) { if (v) v->type = JV_NULL; return p + 4; }
    char *e = NULL;
    double d = strtod(p, &e);
    if (e == p) return NULL;
    if (v) v->type = JV_NUM, v->num = d;
    return e;
}

// 1 段の object から key の値を探す。見つかれば 1・無ければ 0・形が変なら -1
static inline int json_get(const char *s, const char *key, jval *out) {
    const char *p = js_ws(s);
    if (*p != '{') return -1;
    p = js_ws(p + 1);
    if (*p == '}') return 0;
    for (;;) {
        char k[64];
        p = js_str(js_ws(p), k, sizeof k);
        if (!p) return -1;
        p = js_ws(p);
        if (*p != ':') return -1;
        jval v;
        p = js_val(p + 1, &v);
        if (!p) return -1;
        if (!strcmp(k, key) && out) *out = v;
        int hit = !strcmp(k, key);
        p = js_ws(p);
        if (*p == ',') { if (hit) return 1; p++; continue; }
        if (*p == '}') return hit;
        return -1;
    }
}
