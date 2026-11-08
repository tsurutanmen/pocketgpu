// 小道具：時計・UTF-8 と UTF-16・画面への出力・文字列の組み立て・JSON の読み書き・ファイル・出来事の待ち行列
#pragma once

#define LC_PI 3.14159265358979323846

// ---- 時計（QueryPerformanceCounter。Python の time.perf_counter と同じ時計）----
static double g_qpf;
static void clock_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpf = (double) f.QuadPart;
}
static double now_s(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart / g_qpf;
}

// ---- UTF-8 と UTF-16 ----
static wchar_t *u2w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t) (n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0) w[0] = 0;
    else MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n);
    return w;
}
static char *w2u(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t) (n > 0 ? n : 1));
    if (!s) return NULL;
    if (n <= 0) s[0] = 0;
    else WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static char *xstrdup(const char *s) {
    size_t n = strlen(s ? s : "");
    char *p = malloc(n + 1);
    if (p) memcpy(p, s ? s : "", n + 1);
    return p;
}

// ---- 伸びる文字列 ----
typedef struct { char *p; size_t n, cap; } Buf;
static void buf_need(Buf *b, size_t add) {
    if (b->n + add + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + add + 1) c *= 2;
    char *q = realloc(b->p, c);
    if (!q) abort();
    b->p = q;
    b->cap = c;
}
static void buf_add(Buf *b, const char *s, size_t n) {
    buf_need(b, n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_puts(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n > 0) {
        buf_need(b, (size_t) n);
        vsnprintf(b->p + b->n, (size_t) n + 1, fmt, ap2);
        b->n += (size_t) n;
    } else if (!b->p) {
        buf_need(b, 0);
        b->p[0] = 0;
    }
    va_end(ap2);
}
static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}
static void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}
static const char *buf_str(Buf *b) {
    if (!b->p) buf_need(b, 0), b->p[0] = 0;
    return b->p;
}

static void buf_json_str(Buf *b, const char *s) {
    buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *) (s ? s : ""); *p; p++) {
        if (*p == '"') buf_add(b, "\\\"", 2);
        else if (*p == '\\') buf_add(b, "\\\\", 2);
        else if (*p == '\n') buf_add(b, "\\n", 2);
        else if (*p == '\r') buf_add(b, "\\r", 2);
        else if (*p == '\t') buf_add(b, "\\t", 2);
        else if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
        else buf_add(b, (const char *) p, 1);
    }
    buf_add(b, "\"", 1);
}

// Python の str(round(x, d)) と同じ見た目（1.0・6.085・44.7）
static void buf_num(Buf *b, double x, int d) {
    char t[64];
    if (!isfinite(x)) x = 0;
    snprintf(t, sizeof t, "%.*f", d, x);
    char *dot = strchr(t, '.');
    if (dot) {
        char *e = t + strlen(t) - 1;
        while (e > dot + 1 && *e == '0') *e-- = 0;
    } else {
        strcat(t, ".0");
    }
    if (!strcmp(t, "-0.0")) strcpy(t, "0.0");
    buf_puts(b, t);
}

// ---- 画面への出力。コンソールなら UTF-16 で、ファイルやパイプなら UTF-8 のまま ----
static CRITICAL_SECTION g_out_cs;
static void out_raw(DWORD which, const char *s) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode, wr;
    EnterCriticalSection(&g_out_cs);
    if (GetConsoleMode(h, &mode)) {
        wchar_t *w = u2w(s);
        if (w) {
            WriteConsoleW(h, w, (DWORD) wcslen(w), &wr, NULL);
            free(w);
        }
    } else {
        // FlushFileBuffers は呼ばない：パイプでは読む側が読み終えるまで待つので、字幕の表示が止まる（10/11 に 0.8 秒止まった）
        WriteFile(h, s, (DWORD) strlen(s), &wr, NULL);
    }
    LeaveCriticalSection(&g_out_cs);
}
static void vsay(DWORD which, const char *fmt, va_list ap) {
    Buf b = { 0 };
    buf_vprintf(&b, fmt, ap);
    buf_puts(&b, "\n");
    out_raw(which, b.p);
    buf_free(&b);
}
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(STD_OUTPUT_HANDLE, fmt, ap);
    va_end(ap);
}
static void warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(STD_ERROR_HANDLE, fmt, ap);
    va_end(ap);
}

// ---- ファイル（名前は UTF-8 で受けて、Windows には UTF-16 で渡す）----
static FILE *fopen_u8(const char *path, const wchar_t *mode) {
    wchar_t *w = u2w(path);
    FILE *f = w ? _wfopen(w, mode) : NULL;
    free(w);
    return f;
}
static void mkdirs_u8(const char *path) {
    wchar_t *w = u2w(path);
    if (!w) return;
    for (wchar_t *p = w; *p; p++) {
        if ((*p == L'\\' || *p == L'/') && p > w && p[-1] != L':' && p[-1] != L'\\' && p[-1] != L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(w, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(w, NULL);
    free(w);
}
static void mkdirs_parent_u8(const char *path) {
    char *d = xstrdup(path);
    char *a = strrchr(d, '\\'), *b = strrchr(d, '/');
    char *c = a > b ? a : b;
    if (c && c > d) {
        *c = 0;
        mkdirs_u8(d);
    }
    free(d);
}
// 一時ファイルに書いてから置き換える（読む側が半端な中身を見ない）
static int write_replace_u8(const char *path, const char *data, size_t n) {
    Buf tmp = { 0 };
    buf_printf(&tmp, "%s.tmp", path);
    FILE *f = fopen_u8(tmp.p, L"wb");
    if (!f) { buf_free(&tmp); return 0; }
    fwrite(data, 1, n, f);
    fclose(f);
    wchar_t *wt = u2w(tmp.p), *wp = u2w(path);
    int ok = 0;
    for (int i = 0; i < 5 && !ok; i++) {  // OBS が読んでいる瞬間は置き換えに失敗することがある
        ok = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) Sleep(10);
    }
    if (!ok) DeleteFileW(wt);
    free(wt);
    free(wp);
    buf_free(&tmp);
    return ok;
}
static char *read_file_u8(const char *path, size_t *len) {
    FILE *f = fopen_u8(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1L << 30)) { fclose(f); return NULL; }
    char *p = malloc((size_t) n + 1);
    if (!p) { fclose(f); return NULL; }
    size_t got = fread(p, 1, (size_t) n, f);
    fclose(f);
    p[got] = 0;
    if (len) *len = got;
    return p;
}

// ---- JSON を読む。木は作らず、上の段のキーを探して値の位置を返す ----
static const char *js_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}
static const char *js_skip_str(const char *p) {
    p++;
    while (*p && *p != '"') p += (*p == '\\' && p[1]) ? 2 : 1;
    return *p ? p + 1 : NULL;
}
static const char *js_skip(const char *p) {
    p = js_ws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') return js_skip_str(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = js_skip_str(p);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p;
}
static int js_hex4(const char *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        *v = *v * 16 + (unsigned) d;
    }
    return 1;
}
static void js_put_utf8(Buf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = (char) cp; buf_add(b, t, 1); }
    else if (cp < 0x800) { t[0] = (char) (0xC0 | (cp >> 6)); t[1] = (char) (0x80 | (cp & 0x3F)); buf_add(b, t, 2); }
    else if (cp < 0x10000) { t[0] = (char) (0xE0 | (cp >> 12)); t[1] = (char) (0x80 | ((cp >> 6) & 0x3F)); t[2] = (char) (0x80 | (cp & 0x3F)); buf_add(b, t, 3); }
    else { t[0] = (char) (0xF0 | (cp >> 18)); t[1] = (char) (0x80 | ((cp >> 12) & 0x3F)); t[2] = (char) (0x80 | ((cp >> 6) & 0x3F)); t[3] = (char) (0x80 | (cp & 0x3F)); buf_add(b, t, 4); }
}
// v が文字列なら、ほどいて UTF-8 で b に足し、1 を返す
static int js_str_buf(const char *v, Buf *b) {
    v = js_ws(v);
    if (!v || *v != '"') return 0;
    const char *p = v + 1;
    buf_need(b, 0);
    while (*p && *p != '"') {
        if (*p != '\\') {
            const char *q = p;
            while (*q && *q != '"' && *q != '\\') q++;
            buf_add(b, p, (size_t) (q - p));
            p = q;
            continue;
        }
        p++;
        char c = *p ? *p++ : 0;
        switch (c) {
        case 'n': buf_add(b, "\n", 1); break;
        case 'r': buf_add(b, "\r", 1); break;
        case 't': buf_add(b, "\t", 1); break;
        case 'b': buf_add(b, "\b", 1); break;
        case 'f': buf_add(b, "\f", 1); break;
        case 'u': {
            unsigned cp, lo;
            if (!js_hex4(p, &cp)) break;
            p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u' && js_hex4(p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            js_put_utf8(b, cp);
            break;
        }
        default:
            if (c) buf_add(b, &c, 1);
        }
    }
    return 1;
}
static const char *js_get(const char *obj, const char *key) {
    const char *p = js_ws(obj);
    if (!p || *p != '{') return NULL;
    p++;
    for (;;) {
        p = js_ws(p);
        if (!*p || *p != '"') return NULL;
        const char *e = js_skip_str(p);
        if (!e) return NULL;
        Buf k = { 0 };
        js_str_buf(p, &k);
        int hit = !strcmp(buf_str(&k), key);
        buf_free(&k);
        p = js_ws(e);
        if (*p != ':') return NULL;
        p = js_ws(p + 1);
        if (hit) return p;
        p = js_skip(p);
        if (!p) return NULL;
        p = js_ws(p);
        if (*p == ',') p++;
    }
}
// キーの文字列を新しく取る（無ければ NULL）
static char *js_get_str(const char *obj, const char *key) {
    const char *v = js_get(obj, key);
    Buf b = { 0 };
    if (!v || !js_str_buf(v, &b)) { buf_free(&b); return NULL; }
    buf_str(&b);
    return b.p;
}
static int js_get_num(const char *obj, const char *key, double *out) {
    const char *v = js_get(obj, key);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtod(v, NULL);
    return 1;
}
static int js_is_t(const char *obj, const char *want) {
    char *t = js_get_str(obj, "t");
    int r = t && !strcmp(t, want);
    free(t);
    return r;
}

// ---- 出来事の待ち行列（受ける糸・訳す糸 → 本体の糸）----
enum { EV_JSON = 1, EV_TRANSLATION, EV_CLOSED };
typedef struct Ev {
    int kind, fid;
    double t;
    char *s;
    struct Ev *next;
} Ev;
static CRITICAL_SECTION g_q_cs;
static Ev *g_q_head, *g_q_tail;
static HANDLE g_q_event;
static void q_init(void) {
    InitializeCriticalSection(&g_q_cs);
    g_q_event = CreateEventW(NULL, FALSE, FALSE, NULL);
}
static void q_push(int kind, int fid, char *s, double t) {
    Ev *e = calloc(1, sizeof *e);
    if (!e) return;
    e->kind = kind;
    e->fid = fid;
    e->s = s;
    e->t = t;
    EnterCriticalSection(&g_q_cs);
    if (g_q_tail) g_q_tail->next = e; else g_q_head = e;
    g_q_tail = e;
    LeaveCriticalSection(&g_q_cs);
    SetEvent(g_q_event);
}
static Ev *q_take_all(void) {
    EnterCriticalSection(&g_q_cs);
    Ev *e = g_q_head;
    g_q_head = g_q_tail = NULL;
    LeaveCriticalSection(&g_q_cs);
    return e;
}
