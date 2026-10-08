// subwin.exe が起動したアプリの中で動く知らせの受け口（WINEVENT_INCONTEXT）。上の窓ができた瞬間（まだ見せる前）に、
// アプリの糸の中でその場でサブモニターへ動かす。外から知らせを受けて動かすと、見せてから 1 コマほど主のモニターに出るため。
// 場所は subwin.exe が環境変数 SUBWIN_X・SUBWIN_Y・SUBWIN_R・SUBWIN_B で渡す（アプリはその環境を受け継ぐ）
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

static int g_x = -1, g_y, g_r, g_b, g_ok;

static int envi(const char *k, int *v) {
    char b[32];
    if (!GetEnvironmentVariableA(k, b, sizeof b)) return 0;
    *v = atoi(b);
    return 1;
}

__declspec(dllexport) void CALLBACK subwin_ev(HWINEVENTHOOK h, DWORD ev, HWND w, LONG obj, LONG child, DWORD tid, DWORD ms) {
    (void) h; (void) ev; (void) tid; (void) ms;
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !w) return;
    if (g_x == -1) g_ok = envi("SUBWIN_X", &g_x) && envi("SUBWIN_Y", &g_y) && envi("SUBWIN_R", &g_r) && envi("SUBWIN_B", &g_b);
    if (!g_ok) return;
    if (GetAncestor(w, GA_ROOT) != w || GetWindow(w, GW_OWNER)) return; // 上の窓だけ
    if (GetWindowLongW(w, GWL_STYLE) & WS_CHILD) return;
    RECT r;
    if (!GetWindowRect(w, &r)) return;
    if (r.left >= g_x - 40 && r.left < g_r && r.top >= g_y - 40 && r.top < g_b) return; // もう向こう
    SetWindowPos(w, NULL, g_x, g_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
