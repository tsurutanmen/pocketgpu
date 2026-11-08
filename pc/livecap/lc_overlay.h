// 画面の上に重ねる字幕のウィンドウ（Win32 と GDI）。Python 版の Overlay（tkinter）と同じふるまい：
//   いつも手前（HWND_TOPMOST）・クリックは下のウィンドウに通り抜ける（WS_EX_TRANSPARENT）・タスクバーと Alt+Tab に出ない。
//   確定した文は白、途中の文は黄色。下に揃えて、あふれた古い行は上に押し出す。
//   Ctrl+Alt+C で動かすモード（ドラッグで移動・右下の角で大きさ・ホイールで文字）、もう一度押すか右クリックで固定して位置を覚える。
//   位置と文字の大きさは {"geometry": "幅x高さ+X+Y", "font": N} の JSON に残す（Python 版と同じ形）。
#pragma once

static void app_hotkey(int id);
static void app_request_quit(void);

typedef struct { const wchar_t *p; int n; } WLine;

static struct {
    HWND hwnd;
    HFONT font, small_font;
    int font_pt, dpi;
    int unlocked, hidden, drag;  // drag：1 動かす・2 大きさ
    POINT drag_pt;
    RECT drag_rc;
    wchar_t *fin, *vol;
    int has_text;
    BYTE alpha;
    char *settings, *settings_alt;
    const wchar_t *hint, *waiting;
} OV;

typedef BOOL(WINAPI *PFN_SPDAC)(HANDLE);
typedef UINT(WINAPI *PFN_GDFW)(HWND);
static PFN_GDFW p_GetDpiForWindow;

// 文字をぼかさない（モニターごとの DPI に合わせる）。ウィンドウを作る前に呼ぶ
static void dpi_init(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SPDAC spdac = (PFN_SPDAC) (void *) GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (!spdac || !spdac((HANDLE) -4)) SetProcessDPIAware();  // -4 = PER_MONITOR_AWARE_V2
    p_GetDpiForWindow = (PFN_GDFW) (void *) GetProcAddress(u, "GetDpiForWindow");
}
static int ov_dpi(HWND h) {
    if (p_GetDpiForWindow && h) {
        UINT d = p_GetDpiForWindow(h);
        if (d) return (int) d;
    }
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
}
static HFONT ov_make_font(int pt, int dpi, int weight) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Yu Gothic UI");
}
static void ov_fonts(void) {
    if (OV.font) DeleteObject(OV.font);
    if (OV.small_font) DeleteObject(OV.small_font);
    OV.font = ov_make_font(OV.font_pt, OV.dpi, FW_BOLD);
    OV.small_font = ov_make_font(11, OV.dpi, FW_NORMAL);
}

// ---- 行の折り返し。英語は語の切れ目で、日本語は文字ごとに。句読点を行の頭に置かない ----
static int ov_is_word(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'\'' || c == L'-' || c == L'_';
}
static int ov_no_line_start(wchar_t c) {
    return c && wcschr(L"、。，．,.)）」』】!?！？:;：；ー々…ゃゅょっぁぃぅぇぉャュョッァィゥェォ", c) != NULL;
}
static void ov_push_line(WLine *out, int *n, int max, const wchar_t *p, int len) {
    if (*n == max) {  // 多すぎるときは古い（上の）行を捨てる
        memmove(out, out + 1, sizeof(WLine) * (size_t) (max - 1));
        (*n)--;
    }
    out[(*n)++] = (WLine) { p, len };
}
static int ov_wrap(HDC dc, const wchar_t *s, int maxw, WLine *out, int max) {
    int n = 0;
    if (!s || !*s) return 0;
    const wchar_t *p = s;
    for (;;) {
        const wchar_t *e = wcschr(p, L'\n');
        int rem = e ? (int) (e - p) : (int) wcslen(p);
        if (rem == 0) ov_push_line(out, &n, max, p, 0);
        const wchar_t *q = p;
        while (rem > 0) {
            int fit = 0;
            SIZE sz;
            GetTextExtentExPointW(dc, q, rem, maxw, &fit, NULL, &sz);
            if (fit >= rem) fit = rem;
            else {
                if (fit < 1) fit = 1;
                if (fit > 1 && IS_HIGH_SURROGATE(q[fit - 1])) fit--;
                if (ov_is_word(q[fit]) && ov_is_word(q[fit - 1])) {
                    int k = fit - 1;
                    while (k > 0 && q[k] != L' ') k--;
                    if (k > 0) fit = k + 1;
                } else if (fit > 1 && ov_no_line_start(q[fit])) {
                    fit--;
                }
            }
            ov_push_line(out, &n, max, q, fit);
            q += fit;
            rem -= fit;
            while (rem > 0 && *q == L' ') { q++; rem--; }
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

static void ov_paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hh = rc.bottom;
    if (w <= 0 || hh <= 0) { EndPaint(h, &ps); return; }
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, hh);
    HGDIOBJ obm = SelectObject(m, bm);
    HBRUSH bg = CreateSolidBrush(RGB(0x0b, 0x0b, 0x0b));
    FillRect(m, &rc, bg);
    DeleteObject(bg);
    SetBkMode(m, TRANSPARENT);
    HGDIOBJ ofont = SelectObject(m, OV.font);
    TEXTMETRICW tm;
    GetTextMetricsW(m, &tm);
    int lh = tm.tmHeight + tm.tmExternalLeading;
    int pad = MulDiv(OV.font_pt, OV.dpi, 96) / 2;
    if (pad < 8) pad = 8;
    int maxw = w - 2 * pad > 50 ? w - 2 * pad : 50;
    static WLine lv[256], lf[256];
    int nv = ov_wrap(m, OV.vol, maxw, lv, 256);
    int nf = ov_wrap(m, OV.fin, maxw, lf, 256);
    int y = hh - pad;
    SetTextColor(m, RGB(0xff, 0xe6, 0x80));  // 途中の文
    for (int i = nv - 1; i >= 0 && y > 0; i--) {
        y -= lh;
        TextOutW(m, pad, y, lv[i].p, lv[i].n);
    }
    SetTextColor(m, RGB(0xff, 0xff, 0xff));  // 確定した文
    for (int i = nf - 1; i >= 0 && y > 0; i--) {
        y -= lh;
        TextOutW(m, pad, y, lf[i].p, lf[i].n);
    }
    SelectObject(m, OV.small_font);
    GetTextMetricsW(m, &tm);
    int slh = tm.tmHeight + tm.tmExternalLeading;
    if (!OV.has_text && OV.waiting) {
        static WLine lw[16];
        int nw = ov_wrap(m, OV.waiting, maxw, lw, 16);
        SetTextColor(m, RGB(0x9a, 0x9a, 0x9a));
        int yy = hh - pad - nw * slh;
        for (int i = 0; i < nw; i++) TextOutW(m, pad, yy + i * slh, lw[i].p, lw[i].n);
    }
    if (OV.unlocked) {
        int bw = MulDiv(3, OV.dpi, 96);
        HPEN pen = CreatePen(PS_SOLID, bw, RGB(0x3a, 0xa0, 0xff));
        HGDIOBJ op = SelectObject(m, pen), ob = SelectObject(m, GetStockObject(NULL_BRUSH));
        Rectangle(m, bw / 2 + 1, bw / 2 + 1, w - bw / 2, hh - bw / 2);
        int g = MulDiv(18, OV.dpi, 96);  // 右下の角の印
        for (int k = 1; k <= 3; k++) {
            MoveToEx(m, w - bw - 2, hh - bw - 2 - k * g / 3, NULL);
            LineTo(m, w - bw - 2 - k * g / 3, hh - bw - 2);
        }
        SelectObject(m, op);
        SelectObject(m, ob);
        DeleteObject(pen);
        static WLine lh2[16];
        int nh = ov_wrap(m, OV.hint, w - 20 > 50 ? w - 20 : 50, lh2, 16);
        SetTextColor(m, RGB(0x7f, 0xd1, 0xff));
        for (int i = 0; i < nh; i++) TextOutW(m, 10, MulDiv(8, OV.dpi, 96) + i * slh, lh2[i].p, lh2[i].n);
    }
    SelectObject(m, ofont);
    BitBlt(dc, 0, 0, w, hh, m, 0, 0, SRCCOPY);
    SelectObject(m, obm);
    DeleteObject(bm);
    DeleteDC(m);
    EndPaint(h, &ps);
}

// ---- 位置の保存（Python 版と同じ JSON の形）----
static void ov_geometry(char *out, size_t n) {
    RECT r;
    GetWindowRect(OV.hwnd, &r);
    snprintf(out, n, "%ldx%ld+%ld+%ld", r.right - r.left, r.bottom - r.top, r.left, r.top);
}
static void ov_save(void) {
    char g[96];
    ov_geometry(g, sizeof g);
    Buf b = { 0 };
    buf_printf(&b, "{\"geometry\": \"%s\", \"font\": %d}", g, OV.font_pt);
    if (OV.settings) mkdirs_parent_u8(OV.settings);
    if (!(OV.settings && write_replace_u8(OV.settings, b.p, b.n)) && OV.settings_alt) {
        mkdirs_parent_u8(OV.settings_alt);  // 実行ファイルの横に書けないとき（Program Files など）
        write_replace_u8(OV.settings_alt, b.p, b.n);
    }
    buf_free(&b);
}

// "幅x高さ+X+Y"・"幅x高さ"・"+X+Y"。"+-1700" は負の位置、"-10" は右（下）の端からの距離（Tk と同じ）
static const char *geo_num(const char *p, long *v) {
    char *e;
    *v = strtol(p, &e, 10);
    return e == p ? NULL : e;
}
static int parse_geometry(const char *g, long *w, long *h, long *x, long *y, int *has_size, int *has_pos, const RECT *scr) {
    *has_size = *has_pos = 0;
    if (!g || !*g) return 0;
    const char *p = g;
    if (*p == '=') p++;
    if (*p >= '0' && *p <= '9') {
        if (!(p = geo_num(p, w)) || (*p != 'x' && *p != 'X') || !(p = geo_num(p + 1, h))) return 0;
        *has_size = 1;
    }
    if (*p == '+' || *p == '-') {
        long a, b;
        char sa = *p++;
        if (!(p = geo_num(p, &a))) return 0;
        if (*p != '+' && *p != '-') return 0;
        char sb = *p++;
        if (!(p = geo_num(p, &b))) return 0;
        *x = sa == '+' ? a : scr->right - *w - a;
        *y = sb == '+' ? b : scr->bottom - *h - b;
        *has_pos = 1;
    }
    return *p == 0 && (*has_size || *has_pos);
}

static void ov_set_clickthrough(int on) {
    LONG st = GetWindowLongW(OV.hwnd, GWL_EXSTYLE);
    st |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (on) st |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else st &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongW(OV.hwnd, GWL_EXSTYLE, st);
    SetWindowPos(OV.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
static void ov_toggle_unlock(void) {
    OV.unlocked = !OV.unlocked;
    ov_set_clickthrough(!OV.unlocked);
    if (!OV.unlocked) ov_save();
    InvalidateRect(OV.hwnd, NULL, FALSE);
    UpdateWindow(OV.hwnd);
}
static void ov_toggle_hide(void) {
    OV.hidden = !OV.hidden;
    if (OV.hidden) ShowWindow(OV.hwnd, SW_HIDE);
    else {
        ShowWindow(OV.hwnd, SW_SHOWNOACTIVATE);
        ov_set_clickthrough(!OV.unlocked);
    }
}
static void ov_keep_on_top(void) {
    if (OV.hwnd && !OV.hidden && !OV.drag)
        SetWindowPos(OV.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static int ov_in_grip(HWND h, int x, int y) {
    RECT rc;
    GetClientRect(h, &rc);
    int g = MulDiv(28, OV.dpi, 96);
    return x > rc.right - g && y > rc.bottom - g;
}

static LRESULT CALLBACK ov_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: ov_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_HOTKEY: app_hotkey((int) wp); return 0;
    case WM_MOUSEACTIVATE:
        if (!OV.unlocked) return MA_NOACTIVATE;
        break;
    case WM_LBUTTONDOWN:
        if (OV.unlocked) {
            OV.drag = ov_in_grip(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) ? 2 : 1;
            OV.drag_pt.x = GET_X_LPARAM(lp);  // 画面の座標で覚える（メッセージの座標から。GetCursorPos と同じ点）
            OV.drag_pt.y = GET_Y_LPARAM(lp);
            ClientToScreen(h, &OV.drag_pt);
            GetWindowRect(h, &OV.drag_rc);
            SetCapture(h);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (OV.drag) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(h, &p);
            long dx = p.x - OV.drag_pt.x, dy = p.y - OV.drag_pt.y;
            RECT r = OV.drag_rc;
            if (OV.drag == 1) SetWindowPos(h, NULL, r.left + dx, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            else {
                long nw = r.right - r.left + dx, nh = r.bottom - r.top + dy;
                SetWindowPos(h, NULL, 0, 0, nw < 200 ? 200 : nw, nh < 60 ? 60 : nh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (OV.drag) { OV.drag = 0; ReleaseCapture(); }
        return 0;
    case WM_CAPTURECHANGED: OV.drag = 0; return 0;
    case WM_RBUTTONUP:
        if (OV.unlocked) ov_toggle_unlock();
        return 0;
    case WM_MOUSEWHEEL:
        if (OV.unlocked) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            OV.font_pt += d > 0 ? 2 : -2;
            if (OV.font_pt < 10) OV.font_pt = 10;
            if (OV.font_pt > 96) OV.font_pt = 96;
            ov_fonts();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (OV.unlocked && LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            SetCursor(LoadCursorW(NULL, ov_in_grip(h, p.x, p.y) ? IDC_SIZENWSE : IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case 0x02E0: {  // WM_DPICHANGED：モニターをまたいだ
        RECT *r = (RECT *) lp;
        OV.dpi = HIWORD(wp);
        ov_fonts();
        if (!OV.drag) SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE: app_request_quit(); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static ULONGLONG file_mtime_u8(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t *w = path ? u2w(path) : NULL;
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &fa);
    free(w);
    return ok ? ((ULONGLONG) fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) : 0;
}
// 2 つの置き場のうち、新しい方を読む（実行ファイルの横に書けなかったときは、もう 1 つの方が新しい）
static char *settings_read(const char *a, const char *b) {
    const char *first = file_mtime_u8(b) > file_mtime_u8(a) ? b : a, *second = first == a ? b : a;
    char *s = first ? read_file_u8(first, NULL) : NULL;
    if (!s && second) s = read_file_u8(second, NULL);
    return s;
}

// font_given：--font-size を書いたか（書いたらそれ、無ければ覚えた大きさ、それも無ければ既定）
static int ov_create(const char *geometry, int font_pt, int font_given, int lines, double opacity, const char *title) {
    OV.hint = u2w(TT(T_HINT));
    OV.waiting = u2w(TT(T_WAITING));
    char *st = settings_read(OV.settings, OV.settings_alt);
    char *saved_geo = st ? js_get_str(st, "geometry") : NULL;
    double saved_font = 0;
    if (st) js_get_num(st, "font", &saved_font);
    free(st);
    OV.font_pt = font_given || saved_font < 1 ? font_pt : (int) saved_font;
    if (OV.font_pt < 10) OV.font_pt = 10;
    if (OV.font_pt > 96) OV.font_pt = 96;
    if (opacity < 0.1) opacity = 0.1;
    if (opacity > 1.0) opacity = 1.0;
    OV.alpha = (BYTE) lrint(opacity * 255);

    // 既定の位置：主のモニターの下の方、幅 7 割、確定した文の行数＋1 行の高さ
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    POINT origin = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT mon = mi.rcMonitor;
    int sysdpi = ov_dpi(NULL);
    HDC sdc = GetDC(NULL);
    HFONT tf = ov_make_font(OV.font_pt, sysdpi, FW_BOLD);
    HGDIOBJ of = SelectObject(sdc, tf);
    TEXTMETRICW tm;
    GetTextMetricsW(sdc, &tm);
    SelectObject(sdc, of);
    DeleteObject(tf);
    ReleaseDC(NULL, sdc);
    int pad = MulDiv(OV.font_pt, sysdpi, 96) / 2;
    if (pad < 8) pad = 8;
    long mw = mon.right - mon.left, mh = mon.bottom - mon.top;
    long w = mw * 7 / 10, hgt = (tm.tmHeight + tm.tmExternalLeading) * (lines + 1) + 2 * pad;
    long x = mon.left + (mw - w) / 2, y = mon.bottom - hgt - mh * 8 / 100;
    const char *gs[2] = { geometry, saved_geo };
    for (int i = 0; i < 2; i++) {
        long gw = w, gh = hgt, gx = x, gy = y;
        int hs, hp;
        if (!gs[i] || !parse_geometry(gs[i], &gw, &gh, &gx, &gy, &hs, &hp, &mon)) continue;
        if (gw < 200) gw = 200;
        if (gh < 60) gh = 60;
        RECT want = { gx, gy, gx + gw, gy + gh };
        if (!MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) continue;  // 外したモニターの上だった
        w = gw; hgt = gh; x = gx; y = gy;
        break;
    }
    free(saved_geo);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = ov_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"LivecapCaption";
    RegisterClassExW(&wc);
    wchar_t *wt = u2w(title);
    OV.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                              wc.lpszClassName, wt, WS_POPUP, x, y, w, hgt, NULL, NULL, wc.hInstance, NULL);
    free(wt);
    if (!OV.hwnd) return 0;
    OV.dpi = ov_dpi(OV.hwnd);
    ov_fonts();
    SetLayeredWindowAttributes(OV.hwnd, 0, OV.alpha, LWA_ALPHA);
    ShowWindow(OV.hwnd, SW_SHOWNOACTIVATE);
    ov_set_clickthrough(1);
    UpdateWindow(OV.hwnd);
    static const char keys[3] = { 'C', 'H', 'Q' };
    for (int i = 0; i < 3; i++)
        if (!RegisterHotKey(OV.hwnd, i + 1, MOD_CONTROL | MOD_ALT | 0x4000 /* MOD_NOREPEAT */, keys[i])) warn(TT(T_HOTKEY_FAIL), keys[i]);
    return 1;
}

// 変わったときだけ描き直す。描き終えてから返る
static int ov_show(const char *fin, const char *vol) {
    wchar_t *f = u2w(fin), *v = u2w(vol);
    if (OV.fin && OV.vol && !wcscmp(f, OV.fin) && !wcscmp(v, OV.vol)) {
        free(f);
        free(v);
        return 0;
    }
    free(OV.fin);
    free(OV.vol);
    OV.fin = f;
    OV.vol = v;
    if (*f || *v) OV.has_text = 1;
    InvalidateRect(OV.hwnd, NULL, FALSE);
    UpdateWindow(OV.hwnd);
    return 1;
}

static void ov_pump(void) {
    MSG m;
    for (int i = 0; i < 100 && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE); i++) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
static int ov_hit_self(void) {
    RECT r;
    GetWindowRect(OV.hwnd, &r);
    POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    HWND h = WindowFromPoint(p);
    return h && GetAncestor(h, GA_ROOT) == OV.hwnd;
}
// いつも手前か・クリックが通り抜けるか（WindowFromPoint が自分を返さないか）・動かすモードで掴めるか・固定に戻るか
static void ov_selfcheck(Buf *b) {
    LONG st = GetWindowLongW(OV.hwnd, GWL_EXSTYLE);
    char g[96];
    ov_geometry(g, sizeof g);
    int hit = ov_hit_self();
    buf_printf(b, "{\"topmost\": %s, \"click_through\": %s, \"layered\": %s, \"hit_test_reaches_window\": %s, \"geometry\": \"%s\"",
               st & WS_EX_TOPMOST ? "true" : "false", st & WS_EX_TRANSPARENT ? "true" : "false", st & WS_EX_LAYERED ? "true" : "false",
               hit ? "true" : "false", g);
    ov_toggle_unlock();
    ov_pump();
    st = GetWindowLongW(OV.hwnd, GWL_EXSTYLE);
    hit = ov_hit_self();
    buf_printf(b, ", \"unlocked_click_through\": %s, \"unlocked_hit_test_reaches_window\": %s", st & WS_EX_TRANSPARENT ? "true" : "false", hit ? "true" : "false");
    ov_toggle_unlock();
    ov_pump();
    st = GetWindowLongW(OV.hwnd, GWL_EXSTYLE);
    buf_printf(b, ", \"relocked_click_through\": %s}", st & WS_EX_TRANSPARENT ? "true" : "false");
}

static void ov_destroy(void) {
    if (!OV.hwnd) return;
    for (int i = 1; i <= 3; i++) UnregisterHotKey(OV.hwnd, i);
    DestroyWindow(OV.hwnd);
    OV.hwnd = NULL;
}
