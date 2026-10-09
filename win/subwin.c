// アプリを起動し、その窓を作られたらすぐサブモニター（主でないモニター）へ動かす。主のモニターに一瞬も出さないため。
//   subwin.exe アプリ [引数 ...]
// IPHONE_VK_MONITOR が sub か番号（1 から）のときだけ動かす。それ以外はそのまま起動して終わるのを待つ。
// 1. 起動のときに位置を渡す（CW_USEDEFAULT で窓を作るアプリはそこに出る）。窓は前に出さない
//    アプリの中には subwin_hook.dll の知らせの受け口（WINEVENT_INCONTEXT）を差し込み、窓ができた瞬間、見せる前に動かす
// 2. 窓ができた知らせ（EVENT_OBJECT_CREATE・SHOW）を受けて、起動したアプリとその子の上の窓を動かす。大きさは変えない
// 終わりの番号はアプリのものを返す
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

static RECT g_work;
static DWORD g_root;
static int g_on;

typedef struct { HMONITOR m[16]; int n; } mons;

static BOOL CALLBACK collect(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void) dc;
    (void) r;
    mons *l = (mons *) lp;
    if (l->n < 16) l->m[l->n++] = m;
    return TRUE;
}

// ドライバの move_to_monitor と同じ選び方：sub なら主でない最初のモニター、番号ならその番目
static int pick_monitor(void) {
    const char *e = getenv("IPHONE_VK_MONITOR");
    if (!e || !*e) return 0;
    mons l = { 0 };
    EnumDisplayMonitors(NULL, NULL, collect, (LPARAM) &l);
    MONITORINFO mi = { .cbSize = sizeof mi };
    if (!strcmp(e, "sub")) {
        for (int i = 0; i < l.n; i++)
            if (GetMonitorInfoW(l.m[i], &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
                g_work = mi.rcWork;
                return 1;
            }
        return 0;
    }
    int k = atoi(e);
    if (k < 1 || k > l.n || !GetMonitorInfoW(l.m[k - 1], &mi)) return 0;
    g_work = mi.rcWork;
    return 1;
}

// pid が起動したアプリか、その子孫か。毎回プロセスの一覧を取ると重いので、答えを覚えておく
static DWORD g_seen[64];
static int g_ans[64], g_nseen;

static int ours_slow(DWORD pid);

static int ours(DWORD pid) {
    if (pid == g_root) return 1;
    for (int i = 0; i < g_nseen; i++)
        if (g_seen[i] == pid) return g_ans[i];
    int a = ours_slow(pid);
    int k = g_nseen < 64 ? g_nseen++ : (int) (pid % 64);
    g_seen[k] = pid;
    g_ans[k] = a;
    return a;
}

static int ours_slow(DWORD pid) {
    for (int depth = 0; depth < 8 && pid; depth++) {
        if (pid == g_root) return 1;
        DWORD parent = 0;
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (s == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe = { .dwSize = sizeof pe };
        for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
            if (pe.th32ProcessID == pid) { parent = pe.th32ParentProcessID; break; }
        CloseHandle(s);
        if (parent == pid) return 0;
        pid = parent;
    }
    return 0;
}

static void CALLBACK on_event(HWINEVENTHOOK h, DWORD ev, HWND w, LONG obj, LONG child, DWORD tid, DWORD ms) {
    (void) h; (void) ev; (void) tid; (void) ms;
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !w) return;
    if (GetAncestor(w, GA_ROOT) != w || GetWindow(w, GW_OWNER)) return; // 上の窓だけ
    LONG st = GetWindowLongW(w, GWL_STYLE);
    if (st & WS_CHILD) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!ours(pid)) return;
    RECT r;
    if (!GetWindowRect(w, &r)) return;
    if (r.left >= g_work.left && r.left < g_work.right && r.top >= g_work.top && r.top < g_work.bottom) return; // もう向こう
    SetWindowPos(w, NULL, g_work.left + 40, g_work.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        fwprintf(stderr, L"subwin.exe アプリ [引数 ...]\n");
        return 2;
    }
    // コマンドラインから自分の名前を除いた残りをそのまま渡す（引用符もそのまま）
    wchar_t *cl = GetCommandLineW();
    int q = 0;
    while (*cl && (q || (*cl != L' ' && *cl != L'\t'))) { if (*cl == L'"') q = !q; cl++; }
    while (*cl == L' ' || *cl == L'\t') cl++;
    wchar_t *line = _wcsdup(cl);

#ifdef _WIN64
    // 32 ビットのアプリには、64 ビットの受け口（subwin_hook.dll）を差し込めない。横の x86\subwin.exe（32 ビット版）があれば任せる
    {
        wchar_t exe[MAX_PATH];
        const wchar_t *p = line;
        size_t k = 0;
        if (*p == L'"') { p++; while (*p && *p != L'"' && k < MAX_PATH - 1) exe[k++] = *p++; }
        else while (*p && *p != L' ' && *p != L'\t' && k < MAX_PATH - 1) exe[k++] = *p++;
        exe[k] = 0;
        int is32 = 0;
        FILE *f = _wfopen(exe, L"rb");
        if (f) {
            unsigned char h[1024];
            size_t got = fread(h, 1, sizeof h, f);
            fclose(f);
            if (got >= 0x40) {
                DWORD pe = *(DWORD *) (h + 0x3c);
                if (pe + 6 <= got && !memcmp(h + pe, "PE\0\0", 4)) is32 = *(WORD *) (h + pe + 4) == 0x14c;
            }
        }
        wchar_t me[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, me, MAX_PATH);
        while (n && me[n - 1] != L'\\' && me[n - 1] != L'/') n--;
        wcscpy(me + n, L"x86\\subwin.exe");
        if (is32 && GetFileAttributesW(me) != INVALID_FILE_ATTRIBUTES) {
            size_t len = wcslen(me) + wcslen(line) + 8;
            wchar_t *cl2 = malloc(len * sizeof *cl2);
            swprintf(cl2, len, L"\"%ls\" %ls", me, line);
            STARTUPINFOW s2 = { .cb = sizeof s2 };
            PROCESS_INFORMATION p2;
            if (CreateProcessW(NULL, cl2, NULL, NULL, TRUE, 0, NULL, NULL, &s2, &p2)) {
                WaitForSingleObject(p2.hProcess, INFINITE);
                DWORD code = 1;
                GetExitCodeProcess(p2.hProcess, &code);
                CloseHandle(p2.hThread);
                CloseHandle(p2.hProcess);
                return (int) code;
            }
            free(cl2);
        }
    }
#endif

    g_on = pick_monitor();
    STARTUPINFOW si = { .cb = sizeof si };
    if (g_on) {
        si.dwFlags = STARTF_USEPOSITION | STARTF_USESHOWWINDOW;
        si.dwX = (DWORD) (g_work.left + 40);
        si.dwY = (DWORD) (g_work.top + 40);
        si.wShowWindow = SW_SHOWNOACTIVATE;
    }
    PROCESS_INFORMATION pi;
    // 知らせは起動より先に受け始める（窓をすぐ作るアプリに間に合わせる）
    HWINEVENTHOOK h1 = NULL, h2 = NULL;
    if (g_on) {
        h1 = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, on_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        h2 = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, on_event, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
    HWINEVENTHOOK hin = NULL;
    if (g_on) {
        char b[32];
        sprintf(b, "%ld", g_work.left + 40);
        SetEnvironmentVariableA("SUBWIN_X", b);
        sprintf(b, "%ld", g_work.top + 40);
        SetEnvironmentVariableA("SUBWIN_Y", b);
        sprintf(b, "%ld", g_work.right);
        SetEnvironmentVariableA("SUBWIN_R", b);
        sprintf(b, "%ld", g_work.bottom);
        SetEnvironmentVariableA("SUBWIN_B", b);
    }
    if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, g_on ? CREATE_SUSPENDED : 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"subwin：起動できない（%lu）：%ls\n", GetLastError(), line);
        return 1;
    }
    g_root = pi.dwProcessId;
    if (g_on) {
        // アプリの中で知らせを受ける口を差し込んでから動かす（窓ができた瞬間、見せる前にその場で動かす）。
        // 32 ビットのアプリなどで差し込めなければ、外で知らせを受けて動かすだけにする
        wchar_t dll[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, dll, MAX_PATH);
        while (n && dll[n - 1] != L'\\' && dll[n - 1] != L'/') n--;
        wcscpy(dll + n, L"subwin_hook.dll");
        HMODULE hm = LoadLibraryW(dll);
        WINEVENTPROC fn = hm ? (WINEVENTPROC) GetProcAddress(hm, "subwin_ev") : NULL;
        if (hm && !fn) fn = (WINEVENTPROC) GetProcAddress(hm, "subwin_ev@28"); // 32 ビットの stdcall は @数字 の付いた名前で書き出される
        if (fn) hin = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, hm, fn, pi.dwProcessId, 0, WINEVENT_INCONTEXT);
        if (getenv("SUBWIN_DBG"))
            fprintf(stderr, "subwin: dll %ls hm %p fn %p hook %p err %lu\n", dll, (void *) hm, (void *) fn, (void *) hin, GetLastError());
        ResumeThread(pi.hThread);
    }
    CloseHandle(pi.hThread);
    // 終わるまで知らせを受け続ける
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (hin) UnhookWinEvent(hin);
    if (h1) UnhookWinEvent(h1);
    if (h2) UnhookWinEvent(h2);
    free(line);
    return (int) code;
}
