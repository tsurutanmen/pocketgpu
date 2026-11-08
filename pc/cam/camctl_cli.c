// camctl2：webcam_rx の PC アプリとの口（名前付きパイプ・1 行 1 つの JSON）を試す小さな道具。
// 受けた行はそのまま標準出力に 1 行ずつ出す（UTF-8）。送った行は標準エラーに「→ 」を付けて出す。
//
//   camctl_cli [--pipe 名前] [--wait ミリ秒] コマンド…
//     get_caps            {"cmd":"get_caps"} を送る
//     get_state           {"cmd":"get_state"} を送る
//     set 番号 値          {"cmd":"set","id":番号,"value":値} を送る（例：camctl_cli set 7 100）
//     raw 'JSON'          その行をそのまま送る（形の変な行を試す）
//     sleep ミリ秒         その間、受けた行を出しながら待つ
//     watch 秒            その間、受けた行を出しながら待つ
//   コマンドを全部送ったあと、--wait（既定 1500 ミリ秒）だけ受けた行を出してから終わる。
//   --pipe の既定は pocketgpu-camctl（webcam_rx の --ctl-pipe と同じ名前にする）。
//   つながらなければ 3 で終わる。
//
//   camctl_cli table      操作の番号の表（wcam_ctl.h）を Markdown で出す（パイプは使わない）
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wcam_ctl.h"
#include "wcam_json.h"

static HANDLE g_h;
static OVERLAPPED g_ro, g_wo;
static int g_reading;
static char g_rbuf[4096];
static char g_line[65536 + 1];
static size_t g_ln;

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

// ms ミリ秒の間、受けた行を出す。切れたら 0
static int pump(double ms) {
    double end = now_ms() + ms;
    for (;;) {
        if (!g_reading) {
            ResetEvent(g_ro.hEvent);
            if (!ReadFile(g_h, g_rbuf, sizeof g_rbuf, NULL, &g_ro) && GetLastError() != ERROR_IO_PENDING) return 0;
            g_reading = 1;
        }
        double left = end - now_ms();
        if (left < 0) left = 0;
        if (WaitForSingleObject(g_ro.hEvent, (DWORD) left) != WAIT_OBJECT_0) return 1;
        DWORD got = 0;
        g_reading = 0;
        if (!GetOverlappedResult(g_h, &g_ro, &got, FALSE)) return 0;
        for (DWORD i = 0; i < got; i++) {
            if (g_rbuf[i] == '\n') {
                g_line[g_ln] = 0;
                fwrite(g_line, 1, g_ln, stdout);
                fputc('\n', stdout);
                fflush(stdout);
                g_ln = 0;
            } else if (g_ln < sizeof g_line - 1) g_line[g_ln++] = g_rbuf[i];
        }
    }
}

static int send_line(const char *s) {
    fprintf(stderr, "→ %s\n", s);
    fflush(stderr);
    size_t n = strlen(s);
    char *b = (char *) malloc(n + 2);
    if (!b) return 0;
    memcpy(b, s, n);
    b[n] = '\n';
    ResetEvent(g_wo.hEvent);
    DWORD wr = 0;
    int ok = WriteFile(g_h, b, (DWORD) (n + 1), NULL, &g_wo) || GetLastError() == ERROR_IO_PENDING;
    if (ok) ok = GetOverlappedResult(g_h, &g_wo, &wr, TRUE) && wr == n + 1;
    free(b);
    return ok;
}

static void print_table(void) {
    printf("| id | name | 名前 | kind | unit | 作り直す | 値と選択肢 | iPhone の設定の鍵 |\n");
    printf("|---|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < WC_CTL_NDEFS; i++) {
        const wc_ctl_def *d = &WC_CTL_DEFS[i];
        const char *kind = d->kind == 3 ? "（読むだけ・state だけ）" : wc_ctl_kind_json(d->kind);
        char ch[512] = "";
        size_t at = 0;
        for (int k = 0; k < d->nlabels && at < sizeof ch - 1; k++) {
            char num[32];
            wc_ctl_num(num, sizeof num, d->labels[k].value);
            int w = snprintf(ch + at, sizeof ch - at, "%s%s %s（%s）", k ? "・" : "", num, d->labels[k].ja, d->labels[k].en);
            if (w > 0) at += (size_t) w;
        }
        printf("| %u | %s | %s | %s | %s | %s | %s%s%s | %s |\n", d->id, d->name, d->ja, kind, d->unit[0] ? d->unit : "", d->rebuild ? "○" : "", d->note,
               ch[0] ? "。選択肢：" : "", ch, d->key);
    }
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *pipe = "pocketgpu-camctl";
    double wait = 1500;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--pipe") && i + 1 < argc) pipe = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
        else break;
    }
    if (i < argc && !strcmp(argv[i], "table")) { print_table(); return 0; }
    if (i >= argc) {
        fprintf(stderr, "使い方：camctl_cli [--pipe 名前] [--wait ミリ秒] get_caps | get_state | set 番号 値 | raw 'JSON' | sleep ミリ秒 | watch 秒 …\n"
                        "        camctl_cli table\n");
        return 2;
    }
    if (!strncmp(pipe, "\\\\.\\pipe\\", 9)) pipe += 9;
    wchar_t wn[256], name[300];
    if (!MultiByteToWideChar(CP_UTF8, 0, pipe, -1, wn, 256)) return 2;
    _snwprintf(name, 299, L"\\\\.\\pipe\\%ls", wn);
    name[299] = 0;
    // つなぐ。口がまだ無い・ほかの相手が使っている間は 5 秒まで待つ
    double t0 = now_ms();
    for (;;) {
        g_h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (g_h != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (now_ms() - t0 > 5000) { fprintf(stderr, "パイプ %s につながらない（%lu）\n", pipe, e); return 3; }
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 500);
        else Sleep(100);
    }
    g_ro.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_wo.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    int ok = 1;
    for (; i < argc && ok; i++) {
        const char *c = argv[i];
        jbuf b = { 0 };
        if (!strcmp(c, "get_caps") || !strcmp(c, "get_state")) {
            jb_f(&b, "{\"cmd\":\"%s\"}", c);
            ok = send_line(b.p);
        } else if (!strcmp(c, "set") && i + 2 < argc) {
            jb_f(&b, "{\"cmd\":\"set\",\"id\":%s,\"value\":%s}", argv[i + 1], argv[i + 2]);
            i += 2;
            ok = send_line(b.p);
        } else if (!strcmp(c, "raw") && i + 1 < argc) {
            ok = send_line(argv[++i]);
        } else if (!strcmp(c, "sleep") && i + 1 < argc) {
            ok = pump(atof(argv[++i]));
        } else if (!strcmp(c, "watch") && i + 1 < argc) {
            ok = pump(atof(argv[++i]) * 1000);
        } else {
            fprintf(stderr, "知らないコマンド：%s\n", c);
            jb_free(&b);
            CloseHandle(g_h);
            return 2;
        }
        jb_free(&b);
        if (ok) ok = pump(0);
    }
    if (ok) ok = pump(wait);
    if (!ok) fprintf(stderr, "パイプが切れた\n");
    CancelIo(g_h);
    CloseHandle(g_h);
    return 0;
}
