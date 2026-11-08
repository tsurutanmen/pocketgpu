// 仮想カメラをいまのユーザーだけに入れる・外す道具。管理者の権限は要らない（HKCU だけに書く）。
//   camera_setup install     64 ビット版は自分で DllRegisterServer を呼び、32 ビット版は SysWOW64 の regsvr32 に頼む
//   camera_setup uninstall   同じく DllUnregisterServer
//   camera_setup status      登録があるか（64・32 ビットの両方）を見る
// DLL（dshow_cam64.dll・dshow_cam32.dll）はこの exe と同じ場所から使う。場所を動かしたら入れ直す。
// 画面の文は WriteConsoleW で出す（cmd のコードページに関係なく日本語が出る）。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define CAM_KEY L"Software\\Classes\\CLSID\\{82069F97-3CA9-4626-AD74-D2AA50BBD618}\\InprocServer32"
// cam4k：仮想カメラが名乗る一番大きい高さの置き場（wcam_shm.h の WCAM_VCAM_REGKEY と同じ）。環境変数 PGPU_TEST_SHM があるとき（試験）は試験用の置き場
static const wchar_t *vcam_regkey(void) {
    wchar_t t[8];
    return GetEnvironmentVariableW(L"PGPU_TEST_SHM", t, 8) > 0 ? L"Software\\PocketGPU\\VirtualCamera-Test" : L"Software\\PocketGPU\\VirtualCamera";
}
#define VCAM_REGKEY vcam_regkey()

static void say(const wchar_t *s) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n, mode;
    if (GetConsoleMode(h, &mode)) WriteConsoleW(h, s, (DWORD) wcslen(s), &n, NULL);
    else {
        char b[2048];
        int k = WideCharToMultiByte(CP_UTF8, 0, s, -1, b, sizeof b, NULL, NULL);
        if (k > 1) WriteFile(h, b, (DWORD) k - 1, &n, NULL);
    }
    if (GetConsoleMode(h, &mode)) WriteConsoleW(h, L"\r\n", 2, &n, NULL);
    else WriteFile(h, "\r\n", 2, &n, NULL);
}

static int registered(REGSAM view, wchar_t *path, DWORD cb) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CAM_KEY, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return 0;
    DWORD t;
    LONG r = RegQueryValueExW(k, NULL, NULL, &t, (BYTE *) path, &cb);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int is_elevated(void) {
    HANDLE t;
    TOKEN_ELEVATION e = { 0 };
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    GetTokenInformation(t, TokenElevation, &e, sizeof e, &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}

// 64 ビット版：DLL を読んで、入口を直接呼ぶ
static HRESULT call64(const wchar_t *dll, const char *fn) {
    HMODULE m = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT(STDAPICALLTYPE * f)(void) = (HRESULT(STDAPICALLTYPE *)(void)) (void *) GetProcAddress(m, fn);
    HRESULT hr = f ? f() : E_NOINTERFACE;
    FreeLibrary(m);
    return hr;
}

// 32 ビット版：32 ビットの regsvr32 に頼む（64 ビットのプロセスは 32 ビットの DLL を読めない）
static DWORD call32(const wchar_t *dll, int un) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH * 2 + 64];
    GetSystemWow64DirectoryW(exe, MAX_PATH);
    wcscat(exe, L"\\regsvr32.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return 0; // 32 ビットの仕組みが無い Windows
    _snwprintf(cmd, sizeof cmd / sizeof cmd[0] - 1, L"\"%ls\" /s %ls\"%ls\"", exe, un ? L"/u " : L"", dll);
    cmd[sizeof cmd / sizeof cmd[0] - 1] = 0;
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return GetLastError();
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

int wmain(int argc, wchar_t **argv) {
    wchar_t dir[MAX_PATH], d64[MAX_PATH], d32[MAX_PATH], buf[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (sl) sl[1] = 0;
    _snwprintf(d64, MAX_PATH - 1, L"%lsdshow_cam64.dll", dir);
    _snwprintf(d32, MAX_PATH - 1, L"%lsdshow_cam32.dll", dir);
    d64[MAX_PATH - 1] = d32[MAX_PATH - 1] = 0;
    const wchar_t *what = argc >= 2 ? argv[1] : L"status";
    if (!wcscmp(what, L"install")) {
        if (is_elevated()) {
            say(L"管理者として動いています。いまのユーザーの設定に入れますが、管理者として動くアプリにはこのカメラは出ません。");
            say(L"Running as administrator. The camera is added for the current user, but apps running as administrator will not see it.");
        }
        HRESULT hr = call64(d64, "DllRegisterServer");
        DWORD r32 = SUCCEEDED(hr) ? call32(d32, 0) : 0;
        if (FAILED(hr) || r32) {
            call64(d64, "DllUnregisterServer");
            call32(d32, 1);
            say(L"カメラを登録できませんでした。");
            say(L"Could not register the camera.");
            return 1;
        }
        say(L"「PocketGPU iPhone Camera」をカメラの一覧に追加しました。カメラを使うアプリを開き直してください。");
        say(L"Added \"PocketGPU iPhone Camera\" to the camera list. Restart the apps that use the camera.");
        return 0;
    }
    if (!wcscmp(what, L"uninstall")) {
        call64(d64, "DllUnregisterServer");
        call32(d32, 1);
        int left = registered(KEY_WOW64_64KEY, buf, sizeof buf) || registered(KEY_WOW64_32KEY, buf, sizeof buf);
        say(left ? L"一部の登録が残っています。もう一度試してください。" : L"「PocketGPU iPhone Camera」をカメラの一覧から外しました。");
        say(left ? L"Some registration is still left. Please try again." : L"Removed \"PocketGPU iPhone Camera\" from the camera list.");
        return left ? 1 : 0;
    }
    // cam4k：仮想カメラが名乗る一番大きい高さ。webcam_rx が起動のたびに書く（--vcam-max 2160 なら 2160・そうでなければ 1080）。手で決めるときはこちら
    //   camera_setup vcam-max 2160    4K（3840x2160）も名乗る（アプリを開き直すと一覧に出る）
    //   camera_setup vcam-max 1080    1080p まで（ふつう）
    if (!wcscmp(what, L"vcam-max")) {
        if (argc < 3 || (wcscmp(argv[2], L"2160") && wcscmp(argv[2], L"1080"))) {
            say(L"使い方：camera_setup vcam-max 2160 ／ camera_setup vcam-max 1080");
            return 2;
        }
        HKEY k;
        DWORD v = wcscmp(argv[2], L"2160") ? 1080 : 2160;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, VCAM_REGKEY, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &k, NULL) != ERROR_SUCCESS) { say(L"レジストリに書けませんでした。"); return 1; }
        RegSetValueExW(k, L"MaxHeight", 0, REG_DWORD, (const BYTE *) &v, sizeof v);
        RegCloseKey(k);
        say(v == 2160 ? L"仮想カメラが 4K（3840x2160）も名乗るようにしました。カメラを使うアプリを開き直してください。" : L"仮想カメラは 1080p までにしました。カメラを使うアプリを開き直してください。");
        return 0;
    }
    wchar_t line[MAX_PATH + 64];
    int a = registered(KEY_WOW64_64KEY, buf, sizeof buf);
    _snwprintf(line, MAX_PATH + 63, L"64 ビットのアプリ向け：%ls%ls", a ? L"登録されています　" : L"登録されていません", a ? buf : L"");
    line[MAX_PATH + 63] = 0;
    say(line);
    int b = registered(KEY_WOW64_32KEY, buf, sizeof buf);
    _snwprintf(line, MAX_PATH + 63, L"32 ビットのアプリ向け：%ls%ls", b ? L"登録されています　" : L"登録されていません", b ? buf : L"");
    line[MAX_PATH + 63] = 0;
    say(line);
    {
        HKEY k;
        DWORD v = 1080, n = sizeof v, t = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, VCAM_REGKEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExW(k, L"MaxHeight", NULL, &t, (BYTE *) &v, &n) != ERROR_SUCCESS || t != REG_DWORD) v = 1080;
            RegCloseKey(k);
        }
        say(v >= 2160 ? L"名乗る大きさ：4K（3840x2160）まで" : L"名乗る大きさ：1080p まで");
    }
    say(L"使い方：camera_setup install ／ camera_setup uninstall ／ camera_setup vcam-max 2160|1080");
    return 0;
}
