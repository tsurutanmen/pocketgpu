// ipaudio.exe：PC の音を iPhone から出し、iPhone のマイクを PC に取り込む。
//   スピーカー：既定の出力（または名前で選んだ出力）を WASAPI の loopback で取り、TCP で iPhone に流す
//   マイク    ：iPhone から届いた PCM を、WAV に書くか、指定した出力の機器（仮想ケーブルなど）に鳴らす
// つなぎ先：--usb（usbmuxd 越しに iPhone の 50063 番）か --connect host:port（試験用）
// camset で足した：--device DeviceID か UDID（iPhone が何台かつながっているとき。一覧は webcam_rx --list）
// mic2 で足した（work/mic2。約束の追加は proto.h の頭）：
//   --mic-channels 1|2（既定 2。アプリの「ステレオで送る」がオンで入力が 2ch 取れるときだけステレオで来る）
//   --mic-format 16|24|f32（既定 16）・--mic-rate N（既定 48000）・--mic-mode app|raw|standard|voice（既定 app＝アプリの設定）
//   --reconnect：切れたら 2 秒おいてつなぎ直す。マイクを鳴らす出力は開いたままにする（OBS から見て音の流れが続く）
//   --window：小さな状態のウィンドウを出す。OBS の「アプリケーション音声キャプチャ」は、ウィンドウを持つプロセスしか選べないため
//   --latency-test [出力の機器]：遅れを測る。その機器でクリックを鳴らし、iPhone のマイクで拾って、PC に届くまでと PC で鳴るまでを測る
//   --report-sec N：状態の行を N 秒ごとに出す（既定 1）
//   --render-ms N：マイクを鳴らす出力の機器にためる長さ（既定 30。前は 100 で、そのまま遅れになっていた）
//   --no-quiet-skip：たまりすぎを静かな所で飛ばして戻す処理（proto.h の skip_quiet）を止める。比べる試験用
//   引数は UTF-8 で読む（日本語の名前の機器も選べるように）
// fix1012 で足した（work/fix1012。約束の追加は proto.h の頭）：
//   あとから始まったマイク：ACK が mic=no でも、あとで MIC の塊が届いたら「マイクが始まりました」と出し、ウィンドウの行も直す。
//     前から、届いた塊は鳴らしていた。知らせと表示がなかっただけ
//   マイクを待つ見張り（--reconnect のときだけ。マイクの塊が 1 つも届いていない間）：
//     新しいアプリ（STATS に状態がある）：アプリが前面にあるのにマイクが動かないまま 8 秒たったら、つなぎ直す。
//       続けて起きたら 16・32・60 秒と間をあける。アプリが後ろにいる間はつなぎ直さない（つなぎ直しても始まらないため）
//     前のアプリ（0.7.59 まで。ACK に IPA_A_MIC_LATER が無い）：前に出てもマイクが始まらないことがあるので、
//       スピーカーを使っていなければ、15 秒ごとにつなぎ直す。前に出ていれば、つなぎ直したときに mic=ok になる。
//       スピーカーを使っているときは音が途切れるのでつなぎ直さず、案内だけ出す
//     見張りでつなぎ直すときと、その結果が前と同じ mic=no のときは、記録の行を出さない（自動起動の記録が増えすぎないように）
//   --mic-monitor 出力の機器：受けたマイクの音を、--mic-render とは別の出力でも鳴らす（聞くため）。名前の一部か default。
//     --mic-render を付けずに、これだけでもよい。ためる長さは短く（--mic-monitor-ms、既定 20）、出力のバッファは 10 ms。
//     モニターの出力が止まっても、3 秒ごとに開き直すだけで、--mic-render の流れには触らない
//   --mic-monitor-gain dB：モニターの音量（既定 0）。--mic-gain はモニターには掛けない（--mic-render の先の機器に合わせる物なので）
//   --mic-monitor-ms N：モニターでためる長さ（既定 20。5〜200）
//   --mic-wait-front-sec N・--mic-wait-old-sec N：見張りの秒数（既定 8 と 15。0 で切る。試験用）
// 本物の「iPhone」という名前の入力の機器にするには仮想音声ドライバが要る。ここでは作らない（DESIGN.md を見る）
//
//   zig cc -O2 -std=c11 ipaudio.c -o ipaudio.exe -lole32 -luuid -lws2_32 -lavrt -loleaut32 -luser32 -lgdi32 -lshell32
#include "wasapi.h"
#include <shellapi.h>

static SOCKET g_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_send_cs;
static volatile LONG g_stop;   // いまのつなぎを止める
static volatile LONG g_quit;   // 全体を止める（Ctrl+C・ウィンドウを閉じる・--seconds・遅れの測りの終わり）
static volatile uint64_t g_last_rx_us;
static uint64_t g_start_us;   // 起動した時刻（--seconds はつなぎ直しをまたいで数える）
static int g_ja = 1;           // 案内とウィンドウの言葉。Windows の表示の言語が日本語なら日本語

#define TX(ja, en) (g_ja ? (ja) : (en))

typedef struct {
    const char *connect, *spk_dev, *spk_wav, *mic_wav, *mic_render, *tone_dev, *cap_dev, *cap_wav, *lat_dev, *mic_monitor;
    int gen, cap_input, cap_native, usb, mux_port, use_speaker, use_mic, voiceproc, exclusive, keepalive, target_ms, mic_target_ms, chunk_ms, seconds, list;
    int mic_channels, mic_format, mic_rate, mic_mode, reconnect, window, report_sec, latency, clicks, lat_mute, render_ms, no_quiet_skip;
    int monitor_ms, wd_new_sec, wd_old_sec;
    double freq, amp, click_amp, click_freq;
} Opt;

// ---- 数えるもの（スピーカー側は spk 糸だけ、マイク側は受ける糸だけが書く）。つなぐたびに 0 から ----
static struct {
    uint64_t frames, chunks, silent_chunks, disc, timeouts, bytes;
    double age_sum_us, age_max_us, age_min_us;
    uint64_t age_n;
    double send_max_ms, ev_gap_max_ms;
    uint64_t lost_frames, lost_events, dpos_back;
    int rate;
} S;
static struct {
    uint64_t frames, chunks, gap_frames, late_frames, bytes;
    double int_max_ms, int_sum_ms;  // 塊が届く間隔
    uint64_t int_n;
    uint64_t last_arrival_us;
    float peak;                     // 報告の間のいちばん大きい音（0〜1）
} M;
static struct { double last, min, max, sum; int n; } RTT;
static IpaStats g_phone;
static int g_phone_stats_seen;
static IpaAck g_ack;
static int g_mic_ch = 1, g_mic_fmt = IPA_FMT_S16, g_mic_rate = 48000; // いまのつなぎで届く形（ACK から）
// fix1012：アプリの状態（STATS の 33 バイト目。新しいアプリだけ）と、マイクを待つ見張り
static volatile LONG g_phone_state = -1;   // -1 は分からない（前のアプリ）
static volatile LONG g_phone_input_kind;
static volatile LONG g_mic_late_seen;      // ACK が mic=no のつなぎで、MIC の塊が届いた
static volatile LONG g_wd_fired;           // このつなぎは見張りが切った
static int g_wd_quiet;                     // 前のつなぎを見張りが切った。結果が同じ mic=no なら行を出さない
static int g_wd_new_wait = 0;              // 新しいアプリで、前面のままマイクが動かない秒数の上限。続けて起きると延ばす
static int g_wd_hint_done;

// ---- 状態の文（ウィンドウ）----
static CRITICAL_SECTION g_ui_cs;
static wchar_t g_ui_text[2048];
static HWND volatile g_hwnd;
static HFONT g_font;
static char g_ui_state[256], g_ui_mic[512], g_ui_out[400], g_ui_level[128];

static void ui_refresh(void) {
    char all[1400];
    EnterCriticalSection(&g_ui_cs);
    snprintf(all, sizeof all, "%s\n%s\n%s\n%s", g_ui_state, g_ui_mic, g_ui_out, g_ui_level);
    if (!MultiByteToWideChar(CP_UTF8, 0, all, -1, g_ui_text, 2048)) g_ui_text[0] = 0;
    LeaveCriticalSection(&g_ui_cs);
    HWND h = g_hwnd;
    if (h) InvalidateRect(h, NULL, TRUE);
}

static void ui_set(char *slot, size_t cap, const char *text) {
    EnterCriticalSection(&g_ui_cs);
    snprintf(slot, cap, "%s", text);
    LeaveCriticalSection(&g_ui_cs);
    ui_refresh();
}

static void request_quit(void) {
    InterlockedExchange(&g_quit, 1);
    InterlockedExchange(&g_stop, 1);
    SOCKET s = g_sock;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
}

static BOOL WINAPI on_ctrl(DWORD t) {
    (void) t;
    request_quit();
    return TRUE;
}

static LRESULT CALLBACK ui_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        InflateRect(&rc, -14, -10);
        HGDIOBJ old = g_font ? SelectObject(dc, g_font) : NULL;
        SetBkMode(dc, TRANSPARENT);
        wchar_t t[2048];
        EnterCriticalSection(&g_ui_cs);
        wcscpy(t, g_ui_text);
        LeaveCriticalSection(&g_ui_cs);
        DrawTextW(dc, t, -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        if (old) SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        request_quit();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// OBS の「アプリケーション音声キャプチャ」は、見えているウィンドウの持ち主のプロセス（とその子）の音を取る（OBS の win-wasapi.cpp）。
// コンソールのウィンドウは cmd.exe や WindowsTerminal.exe の物になるので、ipaudio.exe 自身のウィンドウを 1 つ出す。
// 題は変えない（OBS は題・クラス・exe の名前で探し直す）。状態はウィンドウの中に書く。最小化しても OBS の一覧には残る
static DWORD WINAPI ui_thread(LPVOID a) {
    (void) a;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ui_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"PocketGPUMicWindow";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR) IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"PocketGPU iPhone Mic", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 560, 200, NULL, NULL, wc.hInstance, NULL);
    if (!h) { fprintf(stderr, "ウィンドウを作れない（%lu）\n", (unsigned long) GetLastError()); return 0; }
    g_hwnd = h;
    ui_refresh();
    ShowWindow(h, SW_SHOWNOACTIVATE); // ゲームの前に出ても、キーボードの入力は取らない
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_hwnd = NULL;
    return 0;
}

// ---- 接続 ----
static SOCKET tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCKET && connect(s, res->ai_addr, (int) res->ai_addrlen) != 0) { closesocket(s); s = INVALID_SOCKET; }
    freeaddrinfo(res);
    return s;
}

// usbmuxd（Apple Mobile Device Service）への 1 回の頼み。icd.c の mux_call と同じ
static char *mux_call(SOCKET s, const char *type, const char *extra) {
    char body[1024];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>ipaudio</string><key>ProgName</key><string>ipaudio</string>%s</dict></plist>\n", type, extra);
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    if (!ipa_send_all(s, h, sizeof h) || !ipa_send_all(s, body, (size_t) n)) return NULL;
    if (!ipa_recv_all(s, h, sizeof h) || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = malloc(h[0] - 16 + 1);
    if (!r || !ipa_recv_all(s, r, h[0] - 16)) { free(r); return NULL; }
    r[h[0] - 16] = 0;
    return r;
}

static long long plist_int_after(const char *at, const char *key, const char *limit) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    if (!q || (limit && q > limit)) return -1;
    q = strstr(q, "<integer>");
    return q ? atoll(q + 9) : -1;
}

// camset で足した：iPhone が何台かつながっているとき、--device で選ぶ（DeviceID の数か、UDID。UDID は大文字・小文字と「-」を見ず、6 文字以上なら前の方の一致でよい）
static const char *g_device;

static int plist_str_after(const char *at, const char *key, const char *limit, char *out, size_t cap) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    out[0] = 0;
    if (!q || (limit && q > limit)) return 0;
    q = strstr(q + strlen(k), "<string>");
    if (!q || (limit && q > limit)) return 0;
    q += 8;
    const char *e = strstr(q, "</string>");
    if (!e) return 0;
    size_t n = (size_t) (e - q);
    if (n >= cap) n = cap - 1;
    memcpy(out, q, n);
    out[n] = 0;
    return 1;
}

static int udid_match(const char *udid, const char *want) {
    size_t i = 0, j = 0, hit = 0;
    for (;;) {
        while (want[j] == '-') j++;
        while (udid[i] == '-') i++;
        if (!want[j]) return hit >= 6;
        if (!udid[i]) return 0;
        char a = udid[i], b = want[j];
        if (a >= 'a' && a <= 'z') a = (char) (a - 32);
        if (b >= 'a' && b <= 'z') b = (char) (b - 32);
        if (a != b) return 0;
        i++, j++, hit++;
    }
}

// quiet：つなぎ直しを待っている間は、同じ失敗を毎回出さない
static SOCKET usb_connect(int port, int mux_port, int quiet) {
    char mp[16];
    snprintf(mp, sizeof mp, "%d", mux_port ? mux_port : 27015);
    SOCKET s = tcp_connect("127.0.0.1", mp);
    if (s == INVALID_SOCKET) { if (!quiet) fprintf(stderr, "Apple Mobile Device Service（127.0.0.1:%s）につながらない\n", mp); return s; }
    char *list = mux_call(s, "ListDevices", "");
    closesocket(s);
    if (!list) { if (!quiet) fprintf(stderr, "usbmuxd が機器の一覧を返さない\n"); return INVALID_SOCKET; }
    long long dev = -1, first = -1;
    int usb = 0, digits = g_device && *g_device;
    for (const char *p = g_device; p && *p; p++) if (*p < '0' || *p > '9') digits = 0;
    for (const char *q = list; dev < 0 && (q = strstr(q, "<key>Properties</key>")); q++) {
        const char *end = strstr(q, "</dict>");
        char type[16], udid[64];
        plist_str_after(q, "ConnectionType", end, type, sizeof type);
        plist_str_after(q, "SerialNumber", end, udid, sizeof udid);
        long long id = plist_int_after(q, "DeviceID", end);
        if (strcmp(type, "USB") || id < 0) continue;
        usb++;
        if (first < 0) first = id;
        if (g_device && *g_device && ((digits && id == atoll(g_device)) || (!digits && udid_match(udid, g_device)))) dev = id;
    }
    free(list);
    if (g_device && *g_device && dev < 0) { if (!quiet) fprintf(stderr, "USB でつながった iPhone の中に「%s」が無い\n", g_device); return INVALID_SOCKET; }
    if (!g_device || !*g_device) {
        dev = first;
        if (usb > 1 && !quiet) fprintf(stderr, "USB の iPhone が %d 台ある。最初の 1 台（DeviceID %lld）を使う。選ぶときは --device DeviceID か UDID\n", usb, first);
    }
    if (dev < 0) { if (!quiet) fprintf(stderr, "USB でつながった iPhone が見えない\n"); return INVALID_SOCKET; }
    s = tcp_connect("127.0.0.1", mp);
    if (s == INVALID_SOCKET) return s;
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>", dev, (unsigned) htons((u_short) port));
    char *res = mux_call(s, "Connect", extra);
    long long num = res ? plist_int_after(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { closesocket(s); if (!quiet) fprintf(stderr, "iPhone の %d 番につながらない（usbmuxd の返事 %lld。アプリで「スピーカーとマイクとして使う」がオンか）\n", port, num); return INVALID_SOCKET; }
    return s;
}

static int send_frame_locked(uint8_t type, uint8_t flags, uint16_t frames, uint64_t index, const void *p, uint32_t len) {
    EnterCriticalSection(&g_send_cs);
    int ok = ipa_send_frame(g_sock, type, flags, frames, index, p, len);
    LeaveCriticalSection(&g_send_cs);
    return ok;
}

// ---- スピーカー：loopback → TCP ----
typedef struct { WaCap *cap; int chunk_frames; IpaWav *wav; } SpkArg;

typedef struct {
    int chunk, staged, all_silent;
    int16_t *stage;
    uint64_t index;
    IpaWav *wav;
} SpkCtx;

// frames コマ（data が NULL か silent なら無音）を 1 塊ずつに揃えて送る
static int spk_feed(SpkCtx *x, const WaCap *c, const BYTE *data, UINT32 fr, int silent) {
    UINT32 done = 0;
    while (done < fr) {
        int take = x->chunk - x->staged;
        if ((UINT32) take > fr - done) take = (int) (fr - done);
        if (silent || !data) memset(x->stage + (size_t) x->staged * 2, 0, (size_t) take * 2 * sizeof(int16_t));
        else wa_to_stereo_s16(c, data + (size_t) done * c->wf->nBlockAlign, take, x->stage + (size_t) x->staged * 2);
        if (!silent && data) x->all_silent = 0;
        x->staged += take;
        done += (UINT32) take;
        if (x->staged == x->chunk) {
            uint64_t t0 = ipa_now_us();
            int ok;
            if (x->all_silent) ok = send_frame_locked(IPA_T_SPK, IPA_FR_SILENT, (uint16_t) x->chunk, x->index, NULL, 0);
            else ok = send_frame_locked(IPA_T_SPK, 0, (uint16_t) x->chunk, x->index, x->stage, (uint32_t) x->chunk * 4);
            double ms = (double) (ipa_now_us() - t0) / 1000.0;
            if (ms > S.send_max_ms) S.send_max_ms = ms;
            if (!ok) return 0;
            if (x->wav) ipa_wav_write(x->wav, x->stage, (size_t) x->chunk * 4);
            S.chunks++;
            if (x->all_silent) S.silent_chunks++; else S.bytes += (uint64_t) x->chunk * 4;
            S.frames += (uint64_t) x->chunk;
            x->index += (uint64_t) x->chunk;
            x->staged = 0;
            x->all_silent = 1;
        }
    }
    return 1;
}

static DWORD WINAPI spk_thread(LPVOID arg) {
    SpkArg *a = (SpkArg *) arg;
    WaCap *c = a->cap;
    SpkCtx x = { a->chunk_frames, 0, 1, NULL, 0, a->wav };
    x.stage = malloc((size_t) x.chunk * 2 * sizeof(int16_t) * 2);
    uint64_t exp_dpos = 0, last_ev_us = 0;
    int have_dpos = 0;
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    S.age_min_us = 1e18;
    if (FAILED(IAudioClient_Start(c->ac))) { fprintf(stderr, "loopback を始められない\n"); InterlockedExchange(&g_stop, 1); free(x.stage); return 0; }
    while (!g_stop) {
        if (WaitForSingleObject(c->ev, 100) != WAIT_OBJECT_0) { S.timeouts++; continue; }
        uint64_t evnow = ipa_now_us();
        if (last_ev_us) { double g = (double) (evnow - last_ev_us) / 1000.0; if (g > S.ev_gap_max_ms) S.ev_gap_max_ms = g; }
        last_ev_us = evnow;
        for (;;) {
            UINT32 n = 0;
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(c->cap, &n)) || !n) break;
            BYTE *data = NULL;
            DWORD flags = 0;
            UINT64 dpos = 0, qpc = 0;
            UINT32 fr = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(c->cap, &data, &fr, &flags, &dpos, &qpc))) break;
            int silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            int disc = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
            if (disc) S.disc++;
            // loopback の時刻は「その音が機器から出る時刻」で、取れた今より先になる。何 µs 先かを数える（出る前に取れている余裕）
            double age = (double) (qpc / 10) - (double) ipa_now_us();
            if (qpc) {
                S.age_sum_us += age; S.age_n++;
                if (age > S.age_max_us) S.age_max_us = age;
                if (age < S.age_min_us) S.age_min_us = age;
            }
            // 取りこぼし（OS の側のバッファがあふれた）：機器の位置が飛んだぶんを無音で埋めて、時間の流れを保つ
            if (have_dpos && dpos != exp_dpos) {
                if (dpos > exp_dpos) {
                    uint64_t lost = dpos - exp_dpos;
                    if (lost > (uint64_t) c->rate * 2) lost = (uint64_t) c->rate * 2;
                    S.lost_frames += lost;
                    S.lost_events++;
                    while (lost) { UINT32 m = lost > 48000 ? 48000 : (UINT32) lost; if (!spk_feed(&x, c, NULL, m, 1)) { InterlockedExchange(&g_stop, 1); break; } lost -= m; }
                } else S.dpos_back++;
            }
            exp_dpos = dpos + fr;
            have_dpos = 1;
            if (!spk_feed(&x, c, data, fr, silent)) InterlockedExchange(&g_stop, 1);
            IAudioCaptureClient_ReleaseBuffer(c->cap, fr);
        }
    }
    IAudioClient_Stop(c->ac);
    IAudioClient_Reset(c->ac); // つなぎ直したとき、前のつなぎの残りを送らない
    if (mm) AvRevertMmThreadCharacteristics(mm);
    free(x.stage);
    return 0;
}

// ---- 試験用：WASAPI を通さず、PC の時計で 440 Hz を 48 kHz で作って送る（OS の取りこぼしと切り分けるため）----
static WAVEFORMATEX g_gen_wf = { WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0 };
static WaCap g_gen_cap;
typedef struct { SpkCtx *x; double amp, freq; } GenArg;
static DWORD WINAPI gen_thread(LPVOID arg) {
    GenArg *g = (GenArg *) arg;
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    int n = 480;
    int16_t *buf = malloc(sizeof(int16_t) * 2 * n);
    double phase = 0, step = 6.283185307179586 * g->freq / 48000.0;
    uint64_t t0 = ipa_now_us();
    uint64_t k = 1;
    while (!g_stop) {
        uint64_t due = t0 + k * 10000;
        for (;;) { uint64_t now = ipa_now_us(); if (now >= due) break; if (due - now > 2500) Sleep(1); else YieldProcessor(); }
        k++;
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t) (g->amp * 32767.0 * sin(phase));
            phase += step;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        if (!spk_feed(g->x, &g_gen_cap, (const BYTE *) buf, (UINT32) n, 0)) break;
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    free(buf);
    return 0;
}

// ---- 鳴らした位置と時刻の記録（遅れの測り）----
// 出力の機器の糸が、塊を書くたびに「この塊の先頭は何コマ目か・その時に鳴っていた位置と時刻（IAudioClock）」を残す。
// 位置 k のコマが鳴った時刻は、k の前後の記録の間を直線でつないで出す
typedef struct {
    double rd0, ratio;   // マイクの輪の、この塊の読み始めの位置（コマ）と、1 コマ出すごとに進む量（鳴らしていなければ rd0 は -1）
    uint64_t w;          // この塊の先頭は、この出力に書いた何コマ目か
    int n;               // この塊のコマ数
    uint64_t pos;        // その時に鳴っていた位置（コマ）
    uint64_t qpc_us;     // その時刻
} PlayRec;
#define PLAYREC_N 1024
typedef struct {
    CRITICAL_SECTION cs;
    PlayRec rec[PLAYREC_N];
    uint64_t count;
} PlayLog;

static void playlog_add(PlayLog *L, const PlayRec *r) {
    EnterCriticalSection(&L->cs);
    L->rec[L->count % PLAYREC_N] = *r;
    L->count++;
    LeaveCriticalSection(&L->cs);
}

// 出力に書いた k コマ目が鳴った（鳴る）時刻。分からなければ 0
static uint64_t playlog_time_of(PlayLog *L, double k, int rate) {
    uint64_t t = 0;
    EnterCriticalSection(&L->cs);
    uint64_t n = L->count < PLAYREC_N ? L->count : PLAYREC_N;
    const PlayRec *bb = NULL, *ba = NULL;
    for (uint64_t j = 0; j < n; j++) {
        const PlayRec *r = &L->rec[(L->count - 1 - j) % PLAYREC_N];
        if (!r->qpc_us || !r->pos) continue;
        if ((double) r->pos <= k) { if (!bb || r->qpc_us > bb->qpc_us) bb = r; }
        else { if (!ba || r->qpc_us < ba->qpc_us) ba = r; }
    }
    if (bb && ba && ba->pos > bb->pos) {
        double f = (k - (double) bb->pos) / (double) (ba->pos - bb->pos);
        t = bb->qpc_us + (uint64_t) (f * (double) (ba->qpc_us - bb->qpc_us));
    } else if (bb) {
        t = bb->qpc_us + (uint64_t) ((k - (double) bb->pos) * 1e6 / rate);
    } else if (ba) {
        double d = ((double) ba->pos - k) * 1e6 / rate;
        t = (double) ba->qpc_us > d ? ba->qpc_us - (uint64_t) d : 0;
    }
    LeaveCriticalSection(&L->cs);
    return t;
}

// マイクの輪の位置 i が、出力の何コマ目として書かれたか。分からなければ -1
static double playlog_out_frame_of(PlayLog *L, double i) {
    double k = -1;
    EnterCriticalSection(&L->cs);
    uint64_t n = L->count < PLAYREC_N ? L->count : PLAYREC_N;
    for (uint64_t j = 0; j < n; j++) {
        const PlayRec *r = &L->rec[(L->count - 1 - j) % PLAYREC_N];
        if (r->rd0 < 0 || r->ratio <= 0) continue;
        if (i >= r->rd0 && i < r->rd0 + r->n * r->ratio) { k = (double) r->w + (i - r->rd0) / r->ratio; break; }
    }
    LeaveCriticalSection(&L->cs);
    return k;
}

static IAudioClock *render_clock(WaRender *r, uint64_t *freq) {
    IAudioClock *c = NULL;
    *freq = 0;
    if (SUCCEEDED(IAudioClient_GetService(r->ac, &IID_IAudioClock, (void **) &c))) {
        UINT64 f = 0;
        if (SUCCEEDED(IAudioClock_GetFrequency(c, &f)) && f) *freq = f;
        else { IAudioClock_Release(c); c = NULL; }
    }
    return c;
}

// この流れだけの音量を 0 にする（Windows の音量や既定の機器は変えない）
static void render_mute_stream(WaRender *r) {
    IAudioStreamVolume *sv = NULL;
    if (FAILED(IAudioClient_GetService(r->ac, &IID_IAudioStreamVolume, (void **) &sv))) return;
    UINT32 n = 0;
    float z[32] = {0};
    if (SUCCEEDED(IAudioStreamVolume_GetChannelCount(sv, &n)) && n <= 32) IAudioStreamVolume_SetAllVolumes(sv, n, z);
    IAudioStreamVolume_Release(sv);
}

// ---- マイク：TCP → 輪 → 機器 ----
// 輪（g_po）は L R の 2ch で持つ。モノラルで届いたら同じ音を両方に入れる。出力の機器は、つなぎ直しても開いたまま
static IpaPlayout g_po;
static int g_po_on;
static double g_po_init_rate = 48000, g_po_init_target = 40;
static int g_render_ms = 30, g_quiet_skip = 1;
static float *g_mic_tmp;
// モニター（--mic-monitor）の輪。--mic-render の輪とは別に持ち、短くためる
static IpaPlayout g_po_mon;
static int g_po_mon_on;
static float *g_mon_tmp;
typedef struct {
    WaRender r;
    IAudioClock *clock;
    uint64_t clock_freq, written;
    int open, mute;
    PlayLog log;
    int log_on;
    char want[256];   // 開いた時の名前（止まったら同じ名前で開き直す）
    // fix1012：--mic-render と --mic-monitor で同じ作りを使う。どの輪から読み、どれだけ掛けるか
    IpaPlayout *po;
    int *po_on;
    float *tmp;       // 48000 コマ × 2ch
    float *gain;
    int render_ms;    // 出力の機器にためる長さ
    double target_ms; // 輪にためる長さ
    int monitor;      // 1 ならモニター（案内の文が変わる）
    uint64_t last_try;
} MicOut;
static MicOut g_mo;   // --mic-render
static MicOut g_mon;  // --mic-monitor
// --mic-gain の倍率。出力の機器で大きくされる所（Steam Streaming Microphone は約 +30 dB）に流すとき、先に下げて音割れを防ぐ
static float g_mic_gain = 1.0f;
static float g_mon_gain = 1.0f; // --mic-monitor-gain

static void mic_fill(void *u, float *out, int frames, int ch) {
    MicOut *mo = (MicOut *) u;
    if (frames > 48000) frames = 48000;
    double rd0 = -1, ratio = 0;
    float *tmp = mo->tmp, g = *mo->gain;
    po_read2(mo->po, tmp, frames, &rd0, &ratio);
    for (int i = 0; i < frames; i++) {
        float l = tmp[(size_t) i * 2] * g, r = tmp[(size_t) i * 2 + 1] * g;
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        for (int c = 0; c < ch; c++) out[(size_t) i * ch + c] = c == 0 ? l : c == 1 ? r : 0.0f;
    }
    if (mo->log_on) {
        PlayRec rec = { rd0, ratio, mo->written, frames, 0, 0 };
        UINT64 pos = 0, qpc = 0;
        if (mo->clock && mo->clock_freq && SUCCEEDED(IAudioClock_GetPosition(mo->clock, &pos, &qpc))) {
            rec.pos = (uint64_t) ((double) pos * mo->r.rate / (double) mo->clock_freq);
            rec.qpc_us = qpc / 10;
        }
        playlog_add(&mo->log, &rec);
    }
    mo->written += (uint64_t) frames;
}

static void list_outputs_for_guide(void) {
    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(g_enum, eRender, DEVICE_STATE_ACTIVE, &col))) return;
    UINT n = 0;
    IMMDeviceCollection_GetCount(col, &n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice *d = NULL;
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        char nm[256];
        wa_name(d, nm, sizeof nm);
        printf("    %s\n", nm);
        IMMDevice_Release(d);
    }
    IMMDeviceCollection_Release(col);
}

static void guide_no_device(const char *name) {
    printf("\n");
    printf("%s\n", TX("出力デバイスが見つかりません：", "Output device not found:"));
    printf("    %s\n\n", name);
    printf("%s\n", TX("iPhone-Mic.cmd の 1 行目の名前か、起動するときの引数を、下の一覧の名前の一部に変えてください。",
                      "Change the name on the first line of iPhone-Mic.cmd, or the argument, to part of a name in this list."));
    printf("%s\n\n", TX("Voicemeeter を使うときは、空いている入力の名前にしてください。例：Voicemeeter AUX Input",
                        "To send it to Voicemeeter, use the name of a free input. Example: Voicemeeter AUX Input"));
    printf("%s\n", TX("使える出力デバイス：", "Available output devices:"));
    list_outputs_for_guide();
    printf("\n");
    fflush(stdout);
}

// 既定の出力なら 1。マイクの音がそのままスピーカーから聞こえるので知らせる
static int is_default_render(IMMDevice *d) {
    int hit = 0;
    LPWSTR id = NULL;
    if (FAILED(IMMDevice_GetId(d, &id))) return 0;
    ERole roles[2] = { eConsole, eMultimedia };
    for (int k = 0; k < 2 && !hit; k++) {
        IMMDevice *def = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, eRender, roles[k], &def))) {
            LPWSTR did = NULL;
            if (SUCCEEDED(IMMDevice_GetId(def, &did))) { hit = !wcscmp(id, did); CoTaskMemFree(did); }
            IMMDevice_Release(def);
        }
    }
    CoTaskMemFree(id);
    return hit;
}

// 輪を用意する。初めの 1 回だけ作り、出力を開き直したときは出す rate だけ合わせる
static void micout_ring(MicOut *mo, int dst_rate) {
    if (!*mo->po_on) {
        po_init(mo->po, 2, g_po_init_rate, (double) dst_rate, mo->target_ms);
        mo->po->skip_quiet = g_quiet_skip; // 録画で声の遅れを一定に保つ（proto.h の po_read2）
        *mo->po_on = 1;
    } else {
        EnterCriticalSection(&mo->po->cs);
        mo->po->dst_rate = dst_rate;
        mo->po->nominal = mo->po->src_rate / mo->po->dst_rate;
        LeaveCriticalSection(&mo->po->cs);
    }
}

// 開いて、輪を用意して、鳴らし始める
static int micout_open(MicOut *mo, const char *name) {
    IMMDevice *md = wa_find(eRender, name);
    if (!md) return 0;
    if (!wa_render_open_ms(&mo->r, md, mic_fill, mo, mo->render_ms)) return 0;
    if (mo->mute) render_mute_stream(&mo->r);
    mo->clock = render_clock(&mo->r, &mo->clock_freq);
    mo->written = 0;
    micout_ring(mo, mo->r.rate);
    if (!wa_render_start(&mo->r)) return 0;
    mo->open = 1;
    if (mo->want != name) snprintf(mo->want, sizeof mo->want, "%s", name);
    return 1;
}

static void micout_close(MicOut *mo) {
    wa_render_stop(&mo->r);
    if (mo->clock) { IAudioClock_Release(mo->clock); mo->clock = NULL; }
    if (mo->r.rc) { IAudioRenderClient_Release(mo->r.rc); mo->r.rc = NULL; }
    if (mo->r.ac) { IAudioClient_Release(mo->r.ac); mo->r.ac = NULL; }
    mo->open = 0;
}

// 出力の機器の糸が止まっていたら（機器が消えた・形が変わった）、同じ名前で開き直す。3 秒に 1 回まで。
// --mic-render とモニターは、それぞれの MicOut の中だけを触る（片方が止まっても、もう片方は止めない）
static void micout_check(MicOut *mo) {
    if (!mo->want[0]) return;
    int dead = !mo->open || (mo->r.th && WaitForSingleObject(mo->r.th, 0) == WAIT_OBJECT_0);
    if (!dead) return;
    uint64_t now = ipa_now_us();
    if (now - mo->last_try < 3000000) return;
    mo->last_try = now;
    if (mo->open) {
        printf("%s\n", mo->monitor ? TX("モニターの出力デバイスが止まりました。もう一度開きます。", "The monitor output device stopped. Opening it again.")
                                   : TX("マイクの音を出す出力デバイスが止まりました。もう一度開きます。", "The output device for the mic stopped. Opening it again."));
        micout_close(mo);
    }
    if (micout_open(mo, mo->want)) {
        printf("%s %s\n", mo->monitor ? TX("モニターの出力デバイスを開きました：", "Opened the monitor output device:") : TX("もう一度開きました：", "Opened again:"), mo->r.name);
    } else {
        micout_close(mo);
        // モニターは、見つからない間は 3 秒ごとに黙って探す（ヘッドホンを挿せば鳴り出す）。案内は起動のときと止まったときに出した
        if (!mo->monitor) printf("%s\n", TX("開けませんでした。3 秒後にもう一度試します。", "Could not open it. Trying again in 3 seconds."));
    }
    fflush(stdout);
}

// ---- 遅れの測り（--latency-test）----
// 測る機器でクリック（2 kHz・5 ms）を 1.0〜1.3 秒おきに鳴らす。鳴った時刻は、その出力の再生位置と時刻（IAudioClock）から出す。
// iPhone のマイクの音の中でクリックの頭を探し、(1) その塊が PC に届いた時刻 (2) PC の出力の機器で鳴った時刻 との差を出す
#define LAT_MAX 200
typedef struct {
    WaRender r;
    IAudioClock *clock;
    uint64_t clock_freq, written;
    PlayLog log;
    double freq, amp;
    int burst;                    // クリックの長さ（コマ）
    uint64_t start[LAT_MAX];      // 各クリックの先頭は、この出力の何コマ目か
    volatile LONG scheduled;      // 書いたクリックの数
    int total;
    uint64_t next;                // 次のクリックの先頭
    unsigned seed;
} Clicker;
static Clicker g_clk;

static void click_fill(void *u, float *out, int frames, int ch) {
    Clicker *k = (Clicker *) u;
    uint64_t w0 = k->written;
    for (int i = 0; i < frames; i++) {
        uint64_t t = w0 + (uint64_t) i;
        if (k->scheduled < k->total && t == k->next) {
            k->start[k->scheduled] = t;
            InterlockedIncrement(&k->scheduled);
            k->seed = k->seed * 1103515245u + 12345u;
            k->next = t + (uint64_t) k->r.rate + (uint64_t) ((k->seed >> 16) % (unsigned) (k->r.rate * 3 / 10)); // 1.0〜1.3 秒おき
        }
        float v = 0;
        LONG s = k->scheduled;
        if (s > 0) {
            uint64_t st = k->start[s - 1];
            if (t >= st && t < st + (uint64_t) k->burst) {
                int j = (int) (t - st);
                int fade = k->r.rate / 2000; // 0.5 ms でなめらかに立ち上げ・下げる
                double g = 1.0;
                if (j < fade) g = 0.5 - 0.5 * cos(3.141592653589793 * j / fade);
                else if (j > k->burst - fade) g = 0.5 - 0.5 * cos(3.141592653589793 * (k->burst - j) / fade);
                v = (float) (k->amp * g * sin(6.283185307179586 * k->freq * j / k->r.rate));
            }
        }
        for (int c = 0; c < ch; c++) out[(size_t) i * ch + c] = c < 2 ? v : 0.0f;
    }
    PlayRec rec = { -1, 0, w0, frames, 0, 0 };
    UINT64 pos = 0, qpc = 0;
    if (k->clock && k->clock_freq && SUCCEEDED(IAudioClock_GetPosition(k->clock, &pos, &qpc))) {
        rec.pos = (uint64_t) ((double) pos * k->r.rate / (double) k->clock_freq);
        rec.qpc_us = qpc / 10;
    }
    playlog_add(&k->log, &rec);
    k->written += (uint64_t) frames;
}

// 届いたマイクの音（ch を混ぜた 1ch）と、塊が届いた時刻
#define LAT_RING (96000 * 12)
#define LAT_CHUNKS 4096
typedef struct { uint64_t index; uint32_t frames; uint64_t arrival_us; } ChunkRec;
static struct {
    CRITICAL_SECTION cs;
    float *x;            // LAT_RING コマ
    uint64_t wr;         // 書いたコマの数（マイクの番号と同じ）
    ChunkRec ch[LAT_CHUNKS];
    uint64_t nch;
    int on;
} LT;

static void lat_store(uint64_t index, const float *f, int frames, int ch, uint64_t arrival) {
    EnterCriticalSection(&LT.cs);
    if (index > LT.wr + LAT_RING) LT.wr = index - LAT_RING;
    while (LT.wr < index) { LT.x[LT.wr % LAT_RING] = 0; LT.wr++; } // 番号が飛んだ所は 0
    for (int i = 0; i < frames; i++) {
        float v = 0;
        if (f) for (int c = 0; c < ch; c++) v += f[(size_t) i * ch + c];
        LT.x[(index + (uint64_t) i) % LAT_RING] = v / (float) ch;
    }
    if (index + (uint64_t) frames > LT.wr) LT.wr = index + (uint64_t) frames;
    LT.ch[LT.nch % LAT_CHUNKS] = (ChunkRec) { index, (uint32_t) frames, arrival };
    LT.nch++;
    LeaveCriticalSection(&LT.cs);
}

typedef struct { double arr_ms, play_ms, mon_ms, level_db; int ok; } LatResult;
static LatResult g_lat_res[LAT_MAX];
static int g_lat_done;

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y;
}

// k 番目のクリックを調べる。まだ早ければ 0、調べたら 1
static int lat_analyze(int k, uint64_t now) {
    uint64_t te = playlog_time_of(&g_clk.log, (double) g_clk.start[k], g_clk.r.rate);
    if (!te) return 0;
    if (now < te + 1500000) return 0; // 鳴ってから 1.5 秒待つ
    LatResult *res = &g_lat_res[k];
    memset(res, 0, sizeof *res);
    int rate = g_mic_rate;
    // 鳴る 0.3 秒前から 0.9 秒後までに届いた塊。クリックの間は 1.0 秒以上あるので、遅れが -0.3〜0.7 秒なら前後のクリックと取り違えない。
    // 前を見るのは、出力の時計が鳴ると言う時刻より先に音が取れる形（loopback をマイクの代わりにした試験）のため
    uint64_t ia = UINT64_MAX, ib = 0, wlo = te > 300000 ? te - 300000 : 0, whi = te + 900000;
    EnterCriticalSection(&LT.cs);
    uint64_t nck = LT.nch < LAT_CHUNKS ? LT.nch : LAT_CHUNKS;
    for (uint64_t j = 0; j < nck; j++) {
        ChunkRec *c = &LT.ch[(LT.nch - 1 - j) % LAT_CHUNKS];
        if (c->arrival_us < wlo || c->arrival_us > whi) continue;
        if (c->index < ia) ia = c->index;
        if (c->index + c->frames > ib) ib = c->index + c->frames;
    }
    int ok = ia != UINT64_MAX && ib > ia && LT.wr - ia < LAT_RING && ib - ia > (uint64_t) (rate / 50);
    double peak = 0, noise = 0;
    uint64_t ipk = 0, ion = 0, t_arr = 0;
    if (ok) {
        uint64_t nn = (uint64_t) rate / 100; // 初めの 10 ms は、まだクリックが届いていない所
        double ss = 0;
        for (uint64_t i = ia; i < ia + nn; i++) { double v = LT.x[i % LAT_RING]; ss += v * v; }
        noise = sqrt(ss / (double) nn);
        for (uint64_t i = ia + nn; i < ib; i++) { double v = fabs(LT.x[i % LAT_RING]); if (v > peak) { peak = v; ipk = i; } }
        ok = peak > 0.002 && peak > noise * 8;
        if (ok) {
            // 頭：いちばん大きい所の 10 ms 前から見て、はじめて 30% を超えた所
            uint64_t from = ipk > ia + nn + (uint64_t) rate / 100 ? ipk - (uint64_t) rate / 100 : ia + nn;
            ion = ipk;
            for (uint64_t i = from; i <= ipk; i++) if (fabs(LT.x[i % LAT_RING]) >= 0.3 * peak) { ion = i; break; }
            for (uint64_t j = 0; j < nck; j++) {
                ChunkRec *c = &LT.ch[(LT.nch - 1 - j) % LAT_CHUNKS];
                if (ion >= c->index && ion < c->index + c->frames) { t_arr = c->arrival_us; break; }
            }
            ok = t_arr != 0;
        }
    }
    LeaveCriticalSection(&LT.cs);
    res->level_db = peak > 0 ? 20 * log10(peak) : -120;
    if (!ok) {
        printf("%s %d：%s。%s %.1f dBFS・%s %.1f dBFS\n", TX("クリック", "Click"), k + 1,
               TX("見つかりませんでした", "not found"), TX("いちばん大きい音", "loudest"), res->level_db, TX("雑音", "noise"), noise > 0 ? 20 * log10(noise) : -120.0);
        fflush(stdout);
        return 1;
    }
    res->ok = 1;
    res->arr_ms = ((double) t_arr - (double) te) / 1000.0;
    res->play_ms = 0;
    res->mon_ms = 0;
    // 輪の位置は、--mic-render の輪とモニターの輪で同じ（つなぐたびに両方を 0 から始め、同じ塊を積む）
    MicOut *outs[2] = { &g_mo, &g_mon };
    for (int j = 0; j < 2; j++) {
        MicOut *mo = outs[j];
        if (!mo->open || !mo->log_on) continue;
        double kf = playlog_out_frame_of(&mo->log, (double) ion);
        if (kf < 0) continue;
        uint64_t tp = playlog_time_of(&mo->log, kf, mo->r.rate);
        if (tp) *(j ? &res->mon_ms : &res->play_ms) = ((double) tp - (double) te) / 1000.0;
    }
    printf("%s %d：%s %.1f ms", TX("クリック", "Click"), k + 1, TX("PC に届くまで", "to the PC"), res->arr_ms);
    if (res->play_ms > 0) printf("・%s %.1f ms", TX("PC の出力で鳴るまで", "to the PC output"), res->play_ms);
    if (res->mon_ms > 0) printf("・%s %.1f ms", TX("モニターで鳴るまで", "to the monitor"), res->mon_ms);
    printf("・%s %.1f dBFS\n", TX("大きさ", "level"), res->level_db);
    fflush(stdout);
    return 1;
}

static void lat_summary(void) {
    double a[LAT_MAX], p[LAT_MAX], q[LAT_MAX];
    int na = 0, np = 0, nq = 0;
    for (int k = 0; k < g_lat_done; k++) {
        if (!g_lat_res[k].ok) continue;
        a[na++] = g_lat_res[k].arr_ms;
        if (g_lat_res[k].play_ms > 0) p[np++] = g_lat_res[k].play_ms;
        if (g_lat_res[k].mon_ms > 0) q[nq++] = g_lat_res[k].mon_ms;
    }
    printf("\n%s：%d / %d\n", TX("測れたクリック", "Clicks measured"), na, g_lat_done);
    if (na) {
        qsort(a, (size_t) na, sizeof(double), cmp_double);
        printf("%s：%s %.1f ms・%s %.1f ms・%s %.1f ms\n", TX("PC に届くまで", "To the PC"), TX("中央値", "median"), a[na / 2], TX("最小", "min"), a[0], TX("最大", "max"), a[na - 1]);
    }
    if (np) {
        qsort(p, (size_t) np, sizeof(double), cmp_double);
        printf("%s：%s %.1f ms・%s %.1f ms・%s %.1f ms\n", TX("PC の出力で鳴るまで", "To the PC output"), TX("中央値", "median"), p[np / 2], TX("最小", "min"), p[0], TX("最大", "max"), p[np - 1]);
    }
    if (nq) {
        qsort(q, (size_t) nq, sizeof(double), cmp_double);
        printf("%s：%s %.1f ms・%s %.1f ms・%s %.1f ms\n", TX("モニターで鳴るまで", "To the monitor"), TX("中央値", "median"), q[nq / 2], TX("最小", "min"), q[0], TX("最大", "max"), q[nq - 1]);
        printf("RESULT latency_monitor measured=%d mon_ms_median=%.1f mon_ms_min=%.1f mon_ms_max=%.1f\n", nq, q[nq / 2], q[0], q[nq - 1]);
    }
    printf("RESULT latency clicks=%d measured=%d arr_ms_median=%.1f arr_ms_min=%.1f arr_ms_max=%.1f play_ms_median=%.1f play_ms_min=%.1f play_ms_max=%.1f in_latency_ms=%.1f io_buffer_ms=%.1f mic_mode=%u\n",
           g_lat_done, na, na ? a[na / 2] : 0.0, na ? a[0] : 0.0, na ? a[na - 1] : 0.0, np ? p[np / 2] : 0.0, np ? p[0] : 0.0, np ? p[np - 1] : 0.0,
           g_ack.in_latency_us / 1000.0, g_ack.io_buffer_us / 1000.0, g_ack.mic_mode);
    fflush(stdout);
}

// ---- 1 秒ごとの報告と PING ----
typedef struct { Opt *o; } RepArg;
static const char *mode_name(int m) {
    switch (m) {
    case IPA_MODE_RAW: return TX("加工なし", "Unprocessed");
    case IPA_MODE_STANDARD: return TX("標準", "Standard");
    case IPA_MODE_VOICE: return TX("雑音とエコーを消す", "Reduce noise and echo");
    default: return TX("不明", "unknown");
    }
}
static const char *kind_name(int k) {
    switch (k) {
    case IPA_IN_BUILTIN: return TX("iPhone のマイク", "iPhone mic");
    case IPA_IN_HEADSET: return TX("有線イヤホン", "Wired earphones");
    case IPA_IN_USB: return TX("USB デバイス", "USB device");
    case IPA_IN_BLUETOOTH: return "Bluetooth";
    default: return TX("ほかのデバイス", "Other device");
    }
}

// マイクを待つ見張り（1 秒ごと）。つなぎ直すなら、知らせる文を返す。返した文を出すかは呼ぶ側が決める
static const char *mic_watch(Opt *o, int *front_sec, int *nomic_sec) {
    if (!o->use_mic || !o->reconnect || o->latency) return NULL;
    if ((g_ack.flags & IPA_A_MIC_OK) || g_mic_late_seen) return NULL;
    LONG st = g_phone_state;
    if (st >= 0) {
        // 新しいアプリ：前面にあるのにマイクが動かない（許可されていないときは除く）秒数を数える。後ろにいる間は待つだけ
        if ((st & IPA_S_FRONT) && !(st & IPA_S_MIC_RUNNING) && !(st & IPA_S_MIC_DENIED)) (*front_sec)++;
        else *front_sec = 0;
        if (o->wd_new_sec > 0 && *front_sec >= g_wd_new_wait) {
            g_wd_new_wait = g_wd_new_wait * 2 > 60 ? 60 : g_wd_new_wait * 2; // 続けて起きたら間をあける
            return TX("iPhone のアプリが前面にあるのに、マイクが開始されません。再接続します。",
                      "The iPhone app is in front, but the mic has not started. Reconnecting.");
        }
        return NULL;
    }
    if (g_ack.flags & IPA_A_MIC_LATER) return NULL; // 新しいアプリで、最初の STATS がまだ
    // 前のアプリ（0.7.59 まで）：前に出てもマイクが始まらないことがある。つなぎ直すと、そのとき前面にあれば始まる
    (*nomic_sec)++;
    if (o->wd_old_sec <= 0 || *nomic_sec < o->wd_old_sec) return NULL;
    if (o->use_speaker) {
        // スピーカーの音が途切れるので、つなぎ直さない。案内だけ 1 回
        if (!g_wd_hint_done) {
            g_wd_hint_done = 1;
            printf("%s\n", TX("マイクが開始されません。iPhone のアプリを前面に表示しても開始されないときは、このプログラムを起動し直してください。",
                              "The mic has not started. If it does not start after you bring the iPhone app to the front, restart this program."));
            fflush(stdout);
        }
        return NULL;
    }
    return TX("マイクが開始されないため、再接続します。iPhone のアプリが前面に表示されていれば、再接続でマイクが開始されます。",
              "The mic has not started, so reconnecting. If the iPhone app is in front, the mic starts when it reconnects.");
}

static DWORD WINAPI report_thread(LPVOID arg) {
    Opt *o = ((RepArg *) arg)->o;
    int sec = 0, front_sec = 0, nomic_sec = 0;
    while (!g_stop) {
        for (int i = 0; i < 10 && !g_stop; i++) Sleep(100);
        if (g_stop) break;
        sec++;
        uint64_t now = ipa_now_us();
        send_frame_locked(IPA_T_PING, 0, 0, now, NULL, 0);
        if (o->use_mic) { micout_check(&g_mo); micout_check(&g_mon); }
        const char *why = mic_watch(o, &front_sec, &nomic_sec);
        if (why) {
            if (!g_wd_quiet) { printf("%s\n", why); fflush(stdout); }
            InterlockedExchange(&g_wd_fired, 1);
            InterlockedExchange(&g_stop, 1);
            shutdown(g_sock, SD_BOTH);
            break;
        }
        // 音の大きさ（ウィンドウ）
        if (o->use_mic) {
            float pk = M.peak;
            M.peak = 0;
            char lv[128];
            if (pk > 0) snprintf(lv, sizeof lv, "%s %.0f dBFS", TX("入力レベル：", "Level:"), 20 * log10(pk));
            else snprintf(lv, sizeof lv, "%s", TX("入力レベル：無音", "Level: silent"));
            ui_set(g_ui_level, sizeof g_ui_level, lv);
        }
        // 止まった相手を見つける：iPhone は 1 秒ごとに STATS を送るので、6 秒何も来なければ切ってつなぎ直す
        uint64_t last_rx = g_last_rx_us, now2 = ipa_now_us(); // 受ける糸が now のあとに書いた値だと、引き算が負になる
        if (o->reconnect && now2 > last_rx && now2 - last_rx > 6000000) {
            printf("%s\n", TX("iPhone から 6 秒間データが届きません。再接続します。", "No data from the iPhone for 6 seconds. Reconnecting."));
            fflush(stdout);
            InterlockedExchange(&g_stop, 1);
            shutdown(g_sock, SD_BOTH);
            break;
        }
        // 遅れの測り：鳴ったクリックを順に調べる
        if (o->latency) {
            while (g_lat_done < g_clk.scheduled && lat_analyze(g_lat_done, now)) g_lat_done++;
            if (g_lat_done >= g_clk.total) {
                lat_summary();
                send_frame_locked(IPA_T_BYE, 0, 0, 0, NULL, 0);
                Sleep(100);
                request_quit();
                break;
            }
        }
        if (sec % (o->report_sec > 0 ? o->report_sec : 1) == 0) {
            printf("t=%2ds", sec);
            if (o->use_speaker)
                printf(" | spk: frames=%llu chunks=%llu silent=%llu disc=%llu lost=%llu ev_gap_max_ms=%.1f ahead_us(avg/max)=%.0f/%.0f send_max_ms=%.2f",
                       (unsigned long long) S.frames, (unsigned long long) S.chunks, (unsigned long long) S.silent_chunks, (unsigned long long) S.disc, (unsigned long long) S.lost_frames, S.ev_gap_max_ms,
                       S.age_n ? S.age_sum_us / (double) S.age_n : 0.0, S.age_max_us, S.send_max_ms);
            if (o->use_mic) {
                printf(" | mic: frames=%llu gaps=%llu late=%llu", (unsigned long long) M.frames, (unsigned long long) M.gap_frames, (unsigned long long) M.late_frames);
                if (g_po_on) printf(" fill_ms=%.1f under=%llu over=%llu", po_fill(&g_po) * 1000.0 / g_po.src_rate, (unsigned long long) g_po.underruns, (unsigned long long) g_po.overruns);
            }
            if (RTT.n) printf(" | rtt_ms last=%.2f min=%.2f avg=%.2f", RTT.last, RTT.min, RTT.sum / RTT.n);
            if (g_phone_stats_seen)
                printf(" | phone: fill=%u/%u under=%u over=%u gap=%u ratio_ppm=%d", g_phone.fill_frames, g_phone.target_frames, g_phone.underruns, g_phone.overruns, g_phone.gap_frames, g_phone.ratio_ppm);
            if (g_phone_state >= 0) printf(" state=0x%02lx", (unsigned long) g_phone_state);
            if (g_po_mon_on && g_mon.open) printf(" | mon: fill_ms=%.1f under=%llu", po_fill(&g_po_mon) * 1000.0 / g_po_mon.src_rate, (unsigned long long) g_po_mon.underruns);
            printf("\n");
            fflush(stdout);
        }
        if (o->seconds > 0 && (ipa_now_us() - g_start_us) / 1000000 >= (uint64_t) o->seconds) {
            send_frame_locked(IPA_T_BYE, 0, 0, 0, NULL, 0);
            Sleep(100);
            request_quit();
        }
    }
    return 0;
}

// ---- 1 回のつなぎ ----
typedef struct {
    WaCap cap;
    int have_cap, spk_rate;
    int conn_no;
} Bridge;

static void print_ack(void) {
    int newapp = g_ack.mic_mode != 0;
    printf("つながった。iPhone：hw_rate=%u io_buffer=%.1fms out_latency=%.1fms in_latency=%.1fms speaker=%s mic=%s%s\n", g_ack.hw_rate,
           g_ack.io_buffer_us / 1000.0, g_ack.out_latency_us / 1000.0, g_ack.in_latency_us / 1000.0,
           (g_ack.flags & IPA_A_SPEAKER_OK) ? "ok" : "no", (g_ack.flags & IPA_A_MIC_OK) ? "ok" : "no",
           (g_ack.flags & IPA_A_MIC_NEEDS_FOREGROUND) ? "（マイクはアプリを前に出すと始まる）" : "");
    printf("マイクの形：%u Hz・%u ch・%s", g_ack.mic_rate, g_ack.mic_channels, ipa_fmt_name(g_ack.mic_format));
    if (newapp)
        printf("・音の種類=%s・入力=%s・入力の機器 %u ch%s%s\n", mode_name(g_ack.mic_mode), kind_name(g_ack.input_kind), g_ack.hw_in_channels,
               (g_ack.flags2 & IPA_A2_STEREO_CAPABLE) ? "・本体のマイクはステレオ対応" : "", (g_ack.flags2 & IPA_A2_STEREO_ON) ? "・ステレオで取っている" : "");
    else
        printf("。前の版のアプリなので、音の種類・ステレオ・ビット数は選べない\n");
    fflush(stdout);
}

// マイクが動いているときのウィンドウの行。kind は入力の種類（0 なら出さない。あとから始まったときは ACK に無い）
static void ui_mic_ok_line(char *a, size_t cap, int kind) {
    int newapp = g_ack.mic_mode != 0;
    const char *sep = TX("・", ", ");
    const char *chs = g_ack.mic_channels >= 2 ? TX("ステレオ", "stereo") : TX("モノラル", "mono");
    const char *bits = g_ack.mic_format == IPA_FMT_F32 ? TX("32 ビット", "32-bit") : g_ack.mic_format == IPA_FMT_S24 ? TX("24 ビット", "24-bit") : TX("16 ビット", "16-bit");
    if (newapp && kind)
        snprintf(a, cap, "%s%s%s%u kHz%s%s%s%s%s%s", TX("マイク：", "Mic: "), kind_name(kind), sep, g_ack.mic_rate / 1000, sep, chs, sep, bits, sep, mode_name(g_ack.mic_mode));
    else if (newapp)
        snprintf(a, cap, "%s%u kHz%s%s%s%s%s%s", TX("マイク：", "Mic: "), g_ack.mic_rate / 1000, sep, chs, sep, bits, sep, mode_name(g_ack.mic_mode));
    else
        snprintf(a, cap, "%s%u kHz%s%s%s%s", TX("マイク：", "Mic: "), g_ack.mic_rate / 1000, sep, chs, sep, bits);
}

static void ui_connected(void) {
    char a[512];
    if (!(g_ack.flags & IPA_A_MIC_OK) && (g_ack.flags & IPA_A_MIC_NEEDS_FOREGROUND))
        snprintf(a, sizeof a, "%s", TX("マイク：iPhone のアプリを前面に表示すると開始します。", "Mic: starts when the iPhone app is in front."));
    else if (!(g_ack.flags & IPA_A_MIC_OK))
        snprintf(a, sizeof a, "%s", TX("マイク：開始できません。iPhone の設定でマイクを許可してください。", "Mic: could not start. Allow the microphone in the iPhone Settings."));
    else
        ui_mic_ok_line(a, sizeof a, g_ack.input_kind);
    ui_set(g_ui_mic, sizeof g_ui_mic, a);
    ui_set(g_ui_state, sizeof g_ui_state, TX("状態：接続中です。", "Status: connected."));
}

// ACK が mic=no のつなぎで、MIC の塊が届いた（アプリが前に出てマイクを始めた）。受ける糸から呼ぶ
static void mic_started_late(Opt *o) {
    InterlockedExchange(&g_mic_late_seen, 1);
    g_wd_quiet = 0;
    g_wd_new_wait = o->wd_new_sec;
    printf("%s\n", TX("iPhone のマイクが開始されました。", "The iPhone mic started."));
    fflush(stdout);
    char a[512];
    ui_mic_ok_line(a, sizeof a, (int) g_phone_input_kind);
    ui_set(g_ui_mic, sizeof g_ui_mic, a);
}

static int run_connection(Opt *o, Bridge *b) {
    memset(&S, 0, sizeof S);
    memset(&M, 0, sizeof M);
    memset(&RTT, 0, sizeof RTT);
    RTT.min = 1e9;
    memset(&g_phone, 0, sizeof g_phone);
    g_phone_stats_seen = 0;
    InterlockedExchange(&g_phone_state, -1);
    InterlockedExchange(&g_phone_input_kind, 0);
    InterlockedExchange(&g_mic_late_seen, 0);
    InterlockedExchange(&g_wd_fired, 0);
    InterlockedExchange(&g_stop, 0);
    int quiet = o->reconnect && b->conn_no > 0;
    SOCKET s;
    if (o->connect) {
        char host[128] = "127.0.0.1", port[16] = "50063";
        const char *cpos = strrchr(o->connect, ':');
        if (cpos) { size_t n = (size_t) (cpos - o->connect); if (n > 127) n = 127; memcpy(host, o->connect, n); host[n] = 0; snprintf(port, sizeof port, "%s", cpos + 1); }
        else snprintf(host, sizeof host, "%s", o->connect);
        s = tcp_connect(host, port);
        if (s == INVALID_SOCKET) { if (!quiet) fprintf(stderr, "%s:%s につながらない\n", host, port); return 1; }
    } else {
        s = usb_connect(IPA_PORT, o->mux_port, quiet);
        if (s == INVALID_SOCKET) return 1;
    }
    g_sock = s;
    if (g_quit) { g_sock = INVALID_SOCKET; closesocket(s); return 1; }
    ipa_nodelay(s);
    int big = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *) &big, sizeof big);
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *) &big, sizeof big);

    IpaHello h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "IPAU", 4);
    h.version = IPA_VERSION;
    int mode = o->voiceproc && o->mic_mode == IPA_MODE_APP ? IPA_MODE_VOICE : o->mic_mode;
    h.flags = (uint16_t) ((o->use_speaker ? IPA_F_SPEAKER : 0) | (o->use_mic ? IPA_F_MIC : 0) | (mode == IPA_MODE_VOICE ? IPA_F_VOICEPROC : 0) | (o->exclusive ? IPA_F_EXCLUSIVE : 0));
    h.spk_rate = (uint32_t) b->spk_rate;
    h.spk_channels = 2;
    h.spk_format = 1;
    h.mic_rate = (uint32_t) o->mic_rate;
    h.mic_channels = (uint16_t) o->mic_channels;
    h.mic_format = (uint16_t) o->mic_format;
    h.mic_mode = (uint8_t) mode;
    h.chunk_frames = (uint16_t) (b->spk_rate * o->chunk_ms / 1000);
    h.target_ms = (uint16_t) o->target_ms;
    DWORD to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    if (!ipa_send_all(s, &h, sizeof h) || !ipa_recv_all(s, &g_ack, sizeof g_ack) || memcmp(g_ack.magic, "IPAK", 4) || g_ack.version != IPA_VERSION) {
        if (!quiet) fprintf(stderr, "あいさつが通らない（iPhone のアプリで音のポートが開いているか。古い受信アプリではないか）\n");
        g_sock = INVALID_SOCKET;
        closesocket(s);
        return 1;
    }
    to = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &to, sizeof to);
    b->conn_no++;
    // 見張りがつなぎ直して、結果がまた mic=no なら、同じ行を出さない
    int wd_was_quiet = g_wd_quiet;
    if (g_ack.flags & IPA_A_MIC_OK) {
        g_wd_quiet = 0;
        g_wd_new_wait = o->wd_new_sec;
    }
    if (!(g_wd_quiet && o->use_mic)) print_ack();
    if ((g_ack.flags & IPA_A_MIC_OK) && wd_was_quiet && o->use_mic) printf("%s\n", TX("iPhone のマイクが開始されました。", "The iPhone mic started."));
    // 届く形。前のアプリは 1ch・16 ビット
    g_mic_ch = g_ack.mic_channels ? g_ack.mic_channels : 1;
    g_mic_fmt = g_ack.mic_format ? g_ack.mic_format : IPA_FMT_S16;
    g_mic_rate = g_ack.mic_rate ? (int) g_ack.mic_rate : 48000;
    if (g_mic_ch > 8 || g_mic_rate > 192000 || (g_mic_fmt != IPA_FMT_S16 && g_mic_fmt != IPA_FMT_S24 && g_mic_fmt != IPA_FMT_F32)) {
        fprintf(stderr, "マイクの形が分からない（%d ch・形 %d・%d Hz）。切る\n", g_mic_ch, g_mic_fmt, g_mic_rate);
        g_sock = INVALID_SOCKET;
        closesocket(s);
        return 1;
    }
    if (o->use_mic) ui_connected();
    else ui_set(g_ui_state, sizeof g_ui_state, TX("状態：接続中です。", "Status: connected."));

    IpaWav micwav = {0}, spkwav = {0};
    if (o->use_mic && o->mic_wav) {
        char path[600];
        if (b->conn_no <= 1) snprintf(path, sizeof path, "%s", o->mic_wav);
        else {
            const char *dot = strrchr(o->mic_wav, '.');
            int n = dot ? (int) (dot - o->mic_wav) : (int) strlen(o->mic_wav);
            snprintf(path, sizeof path, "%.*s_%d%s", n, o->mic_wav, b->conn_no, dot ? dot : "");
        }
        if (!ipa_wav_open_fmt(&micwav, path, (uint32_t) g_mic_rate, (uint16_t) g_mic_ch, g_mic_fmt)) fprintf(stderr, "WAV を開けない: %s\n", path);
    }
    if (o->spk_wav && b->conn_no <= 1) ipa_wav_open(&spkwav, o->spk_wav, (uint32_t) b->spk_rate, 2);
    if (g_po_on) po_restart(&g_po, (double) g_mic_rate);
    if (g_po_mon_on) po_restart(&g_po_mon, (double) g_mic_rate);
    if (LT.on) {
        EnterCriticalSection(&LT.cs);
        LT.wr = 0;
        LT.nch = 0;
        LeaveCriticalSection(&LT.cs);
    }

    g_last_rx_us = ipa_now_us();
    RepArg ra = { o };
    HANDLE rt = CreateThread(NULL, 0, report_thread, &ra, 0, NULL);
    HANDLE st = NULL;
    SpkArg sa = { &b->cap, h.chunk_frames, spkwav.f ? &spkwav : NULL };
    SpkCtx gx = { 480, 0, 1, NULL, 0, spkwav.f ? &spkwav : NULL };
    GenArg ga = { &gx, 0.3, 440.0 };
    if (b->have_cap) st = CreateThread(NULL, 0, spk_thread, &sa, 0, NULL);
    if (o->gen) {
        g_gen_cap.wf = &g_gen_wf; g_gen_cap.ch = 2; g_gen_cap.is_float = 0; g_gen_cap.mask = 3; g_gen_cap.rate = 48000;
        gx.stage = malloc(480 * 2 * sizeof(int16_t) * 2);
        st = CreateThread(NULL, 0, gen_thread, &ga, 0, NULL);
    }

    // 受ける糸（ここ）
    uint8_t *pay = malloc(1 << 20);
    uint64_t mic_next = 0;
    int bps = ipa_fmt_bytes(g_mic_fmt), mch = g_mic_ch;
    uint8_t *zeros = calloc(48000 * 8, 4);
    float *fbuf = malloc(sizeof(float) * 96000 * 8), *f2 = malloc(sizeof(float) * 96000 * 2);
    while (!g_stop) {
        IpaFrame fh;
        if (!ipa_recv_all(s, &fh, sizeof fh)) break;
        if (fh.length > (1u << 20)) { fprintf(stderr, "長さ %u が上限を超えた。形が違うので切る\n", fh.length); break; }
        if (fh.length && !ipa_recv_all(s, pay, fh.length)) break;
        uint64_t now = ipa_now_us();
        g_last_rx_us = now;
        if (fh.type == IPA_T_MIC && o->use_mic) {
            if (!M.chunks && !(g_ack.flags & IPA_A_MIC_OK) && !g_mic_late_seen) mic_started_late(o);
            if (M.last_arrival_us) {
                double d = (double) (now - M.last_arrival_us) / 1000.0;
                M.int_sum_ms += d; M.int_n++;
                if (d > M.int_max_ms) M.int_max_ms = d;
            }
            M.last_arrival_us = now;
            if (fh.index > mic_next) { // 抜けたぶんを無音で埋める
                uint64_t gap = fh.index - mic_next;
                M.gap_frames += gap;
                uint64_t left = gap;
                while (left) {
                    int n = left > 48000 ? 48000 : (int) left;
                    if (micwav.f) ipa_wav_write(&micwav, zeros, (size_t) n * mch * bps);
                    if (g_po_on) po_write_zeros(&g_po, n);
                    if (g_po_mon_on) po_write_zeros(&g_po_mon, n);
                    left -= (uint64_t) n;
                }
                mic_next = fh.index;
            } else if (fh.index < mic_next) { M.late_frames += fh.frames; continue; }
            if (fh.flags & IPA_FR_SILENT) {
                uint32_t left = fh.frames;
                while (left) {
                    int n = left > 48000 ? 48000 : (int) left;
                    if (micwav.f) ipa_wav_write(&micwav, zeros, (size_t) n * mch * bps);
                    if (g_po_on) po_write_zeros(&g_po, n);
                    if (g_po_mon_on) po_write_zeros(&g_po_mon, n);
                    left -= (uint32_t) n;
                }
                if (LT.on) lat_store(fh.index, NULL, fh.frames, 1, now);
            } else {
                if (fh.length != (uint32_t) fh.frames * mch * bps) { fprintf(stderr, "マイクの塊の長さが合わない（%u バイト・%u コマ・%d ch・%d バイト）\n", fh.length, fh.frames, mch, bps); break; }
                if (micwav.f) ipa_wav_write(&micwav, pay, fh.length);
                ipa_pcm_to_f32(g_mic_fmt, pay, fbuf, (size_t) fh.frames * mch);
                float pk = M.peak;
                for (size_t i = 0; i < (size_t) fh.frames * mch; i++) { float a = fabsf(fbuf[i]); if (a > pk) pk = a; }
                M.peak = pk;
                if (g_po_on || g_po_mon_on) {
                    for (uint32_t i = 0; i < fh.frames; i++) {
                        f2[2 * i] = fbuf[(size_t) i * mch];
                        f2[2 * i + 1] = fbuf[(size_t) i * mch + (mch > 1 ? 1 : 0)];
                    }
                    if (g_po_on) po_write(&g_po, f2, fh.frames);
                    if (g_po_mon_on) po_write(&g_po_mon, f2, fh.frames);
                }
                if (LT.on) lat_store(fh.index, fbuf, fh.frames, mch, now);
            }
            mic_next += fh.frames;
            M.frames += fh.frames;
            M.chunks++;
            M.bytes += fh.length;
        } else if (fh.type == IPA_T_PING) {
            send_frame_locked(IPA_T_PONG, 0, 0, fh.index, NULL, 0);
        } else if (fh.type == IPA_T_PONG) {
            double ms = (double) (now - fh.index) / 1000.0;
            RTT.last = ms; RTT.sum += ms; RTT.n++;
            if (ms < RTT.min) RTT.min = ms;
            if (ms > RTT.max) RTT.max = ms;
        } else if (fh.type == IPA_T_STATS && fh.length >= sizeof(IpaStats)) {
            memcpy(&g_phone, pay, sizeof g_phone);
            g_phone_stats_seen = 1;
            if (fh.length >= sizeof(IpaStatsExt)) { // アプリ 0.7.60 から：アプリの状態
                IpaStatsExt ex;
                memcpy(&ex, pay, sizeof ex);
                InterlockedExchange(&g_phone_input_kind, ex.input_kind);
                InterlockedExchange(&g_phone_state, ex.state);
            }
        } else if (fh.type == IPA_T_BYE) {
            break;
        }
    }
    InterlockedExchange(&g_stop, 1);
    shutdown(s, SD_BOTH);
    if (st) WaitForSingleObject(st, 3000);
    WaitForSingleObject(rt, 3000);
    if (spkwav.f) ipa_wav_close(&spkwav);
    if (micwav.f) ipa_wav_close(&micwav);
    g_sock = INVALID_SOCKET;
    closesocket(s);
    free(pay); free(zeros); free(fbuf); free(f2);
    if (gx.stage) free(gx.stage);

    // 見張りが切ったつなぎは、何も届いていないので結果の行を出さない（自動起動の記録が増えすぎないように）
    if (g_wd_fired && o->use_mic) {
        g_wd_quiet = 1;
        return 0;
    }
    g_wd_quiet = 0;
    printf("RESULT spk_frames=%llu spk_chunks=%llu spk_silent_chunks=%llu spk_disc=%llu spk_lost_frames=%llu spk_lost_events=%llu spk_ev_gap_max_ms=%.1f spk_timeouts=%llu spk_ahead_us_min=%.0f spk_ahead_us_avg=%.0f spk_ahead_us_max=%.0f send_max_ms=%.2f\n",
           (unsigned long long) S.frames, (unsigned long long) S.chunks, (unsigned long long) S.silent_chunks, (unsigned long long) S.disc, (unsigned long long) S.lost_frames, (unsigned long long) S.lost_events, S.ev_gap_max_ms, (unsigned long long) S.timeouts,
           S.age_n ? S.age_min_us : 0.0, S.age_n ? S.age_sum_us / (double) S.age_n : 0.0, S.age_max_us, S.send_max_ms);
    printf("RESULT mic_frames=%llu mic_chunks=%llu mic_gap_frames=%llu mic_late_frames=%llu mic_interval_ms_avg=%.2f mic_interval_ms_max=%.2f mic_rate=%d mic_ch=%d mic_fmt=%d",
           (unsigned long long) M.frames, (unsigned long long) M.chunks, (unsigned long long) M.gap_frames, (unsigned long long) M.late_frames,
           M.int_n ? M.int_sum_ms / (double) M.int_n : 0.0, M.int_max_ms, g_mic_rate, g_mic_ch, g_mic_fmt);
    if (g_po_on) printf(" po_under=%llu po_over=%llu po_skips=%llu po_skipped_ms=%.1f po_fill_min_ms=%.1f po_fill_max_ms=%.1f", (unsigned long long) g_po.underruns, (unsigned long long) g_po.overruns,
                        (unsigned long long) g_po.skips, g_po.skipped_frames * 1000.0 / g_po.src_rate, g_po.min_fill * 1000.0 / g_po.src_rate, g_po.max_fill * 1000.0 / g_po.src_rate);
    printf("\n");
    printf("RESULT rtt_n=%d rtt_ms_min=%.2f rtt_ms_avg=%.2f rtt_ms_max=%.2f\n", RTT.n, RTT.n ? RTT.min : 0.0, RTT.n ? RTT.sum / RTT.n : 0.0, RTT.max);
    fflush(stdout);
    return 0;
}

static int run_bridge(Opt *o) {
    Bridge b;
    memset(&b, 0, sizeof b);
    b.spk_rate = 48000;
    WaRender keep;
    memset(&keep, 0, sizeof keep);
    int have_keep = 0;

    // 遅れの測り：マイクを鳴らす出力を決めていなければ、クリックと同じ機器に、この流れだけ音量 0 で鳴らす（PC で鳴るまでを測るため。聞こえない）
    if (o->latency && o->use_mic && !o->mic_render) { o->mic_render = o->lat_dev ? o->lat_dev : "default"; o->lat_mute = 1; }
    // マイクを鳴らす出力：先に確かめる。無ければ案内を出して終わる（iPhone-Mic.cmd が止まって読ませる）
    if (o->use_mic && o->mic_render) {
        IMMDevice *md = wa_find(eRender, o->mic_render);
        if (!md) { guide_no_device(o->mic_render); return 3; }
        if (is_default_render(md) && !o->lat_mute)
            printf("%s\n", TX("注意：この出力は Windows の既定の出力です。iPhone のマイクの音が、そのままスピーカーやヘッドホンから聞こえます。",
                              "Note: this is the Windows default output. You will hear the iPhone mic from your speakers or headphones."));
        IMMDevice_Release(md);
    }
    if (o->use_speaker && o->gen) {
        printf("スピーカー：WASAPI は使わず、PC の時計で 440 Hz を作って送る（試験）\n");
    } else if (o->use_speaker) {
        IMMDevice *sdev = wa_find(eRender, o->spk_dev);
        if (!sdev) { fprintf(stderr, "出力の機器が見つからない: %s\n", o->spk_dev ? o->spk_dev : "既定"); return 1; }
        if (!wa_cap_open(&b.cap, sdev, 1, 48000)) return 1;
        b.have_cap = 1;
        b.spk_rate = b.cap.rate;
        printf("スピーカー: %s を loopback で取る（%d Hz・%d ch・%s）\n", b.cap.name, b.cap.rate, b.cap.ch, b.cap.is_float ? "float32" : "pcm16");
        if (o->keepalive) {
            if (wa_render_open(&keep, sdev, NULL, NULL) && wa_render_start(&keep)) { have_keep = 1; printf("無音の出力を流して、無音のときも loopback が止まらないようにする\n"); }
            else fprintf(stderr, "無音の出力を始められない。何も鳴っていない間は loopback の塊が来ない\n");
        }
    }

    // 機器を確かめたあとでウィンドウを出す（見つからずに終わるときに、ウィンドウが一瞬出ないように）
    if (o->window) CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);

    // 遅れの測り：クリックを鳴らす出力
    if (o->latency) {
        IMMDevice *cd = wa_find(eRender, o->lat_dev);
        if (!cd) { guide_no_device(o->lat_dev ? o->lat_dev : "default"); return 3; }
        memset(&g_clk, 0, sizeof g_clk);
        InitializeCriticalSection(&g_clk.log.cs);
        if (!wa_render_open(&g_clk.r, cd, click_fill, &g_clk)) return 1;
        g_clk.clock = render_clock(&g_clk.r, &g_clk.clock_freq);
        if (!g_clk.clock) { fprintf(stderr, "出力の時計（IAudioClock）が取れない\n"); return 1; }
        g_clk.freq = o->click_freq;
        g_clk.amp = o->click_amp;
        g_clk.burst = g_clk.r.rate * 5 / 1000;
        g_clk.total = o->clicks > LAT_MAX ? LAT_MAX : o->clicks < 1 ? 1 : o->clicks;
        g_clk.next = (uint64_t) g_clk.r.rate * 3; // 始めて 3 秒後から（iPhone のマイクが落ち着くまで）
        g_clk.seed = 2024;
        InitializeCriticalSection(&LT.cs);
        LT.x = calloc(LAT_RING, sizeof(float));
        LT.on = 1;
        printf("%s %s・%d Hz・%d %s・%.0f Hz・%s %.3f\n", TX("遅れを測ります。クリックを鳴らす出力デバイス：", "Measuring delay. Clicks on:"), g_clk.r.name, g_clk.r.rate, g_clk.total,
               TX("回", "clicks"), g_clk.freq, TX("振幅", "amplitude"), g_clk.amp);
        fflush(stdout);
    }

    g_po_init_rate = o->mic_rate;
    g_po_init_target = o->mic_target_ms;
    g_render_ms = o->render_ms;
    g_quiet_skip = !o->no_quiet_skip;
    g_wd_new_wait = o->wd_new_sec;
    if (o->use_mic && o->mic_render) {
        memset(&g_mo, 0, sizeof g_mo);
        InitializeCriticalSection(&g_mo.log.cs);
        g_mo.log_on = o->latency;
        g_mo.mute = o->lat_mute;
        g_mic_tmp = malloc(sizeof(float) * 48000 * 2);
        g_mo.po = &g_po; g_mo.po_on = &g_po_on; g_mo.tmp = g_mic_tmp; g_mo.gain = &g_mic_gain;
        g_mo.render_ms = o->render_ms; g_mo.target_ms = o->mic_target_ms;
        if (!micout_open(&g_mo, o->mic_render)) { fprintf(stderr, "マイクを鳴らす出力を開けない: %s\n", o->mic_render); return 1; }
        printf("マイク：iPhone の音を %s・%d Hz に鳴らす。ため %d ms・出力の機器のバッファ %u コマ%s\n", g_mo.r.name, g_mo.r.rate, o->mic_target_ms, g_mo.r.buf_frames, g_mo.mute ? "。この流れだけ音量 0" : "");
        char t[400];
        snprintf(t, sizeof t, "%s%s", TX("出力：", "Output: "), g_mo.r.name);
        ui_set(g_ui_out, sizeof g_ui_out, t);
    }
    // モニター：聞くための 2 本目の出力。短くためる。開けなくても止めずに、3 秒ごとに探す（report_thread の micout_check）
    if (o->use_mic && o->mic_monitor) {
        memset(&g_mon, 0, sizeof g_mon);
        InitializeCriticalSection(&g_mon.log.cs);
        g_mon.log_on = o->latency;
        g_mon_tmp = malloc(sizeof(float) * 48000 * 2);
        g_mon.po = &g_po_mon; g_mon.po_on = &g_po_mon_on; g_mon.tmp = g_mon_tmp; g_mon.gain = &g_mon_gain;
        g_mon.render_ms = 10; g_mon.target_ms = o->monitor_ms; g_mon.monitor = 1;
        snprintf(g_mon.want, sizeof g_mon.want, "%s", o->mic_monitor);
        micout_ring(&g_mon, 48000); // 出力が開く前から積めるように、輪は先に作る
        if (micout_open(&g_mon, o->mic_monitor)) {
            g_mon.last_try = ipa_now_us();
            printf("%s%s%s%d Hz%s%s %d ms%s%s %u %s%s%s %+.1f dB\n", TX("モニター：", "Monitor: "), g_mon.r.name, TX("・", ", "), g_mon.r.rate, TX("・", ", "),
                   TX("受信バッファ", "receive buffer"), o->monitor_ms, TX("・", ", "), TX("出力バッファ", "output buffer"), g_mon.r.buf_frames, TX("フレーム", "frames"),
                   TX("・", ", "), TX("音量", "volume"), 20 * log10(g_mon_gain));
            if (g_mo.open && !strcmp(g_mo.r.name, g_mon.r.name))
                printf("%s\n", TX("注意：モニターと --mic-render が同じ出力デバイスです。音が二重に聞こえます。",
                                  "Note: the monitor and --mic-render use the same output device. You will hear the sound twice."));
        } else {
            micout_close(&g_mon);
            g_mon.last_try = ipa_now_us();
            printf("%s %s\n", TX("モニターの出力デバイスが見つかりません。見つかるまで 3 秒ごとに探します：", "Monitor output device not found. Looking for it every 3 seconds:"), o->mic_monitor);
        }
        fflush(stdout);
    }
    if (o->latency && !wa_render_start(&g_clk.r)) { fprintf(stderr, "クリックを鳴らせない\n"); return 1; }

    InitializeCriticalSection(&g_send_cs);
    int rc = 0, fails = 0;
    for (;;) {
        ui_set(g_ui_state, sizeof g_ui_state, b.conn_no ? TX("状態：接続が切れました。iPhone を待っています。", "Status: disconnected. Waiting for the iPhone.")
                                                         : TX("状態：iPhone を待っています。", "Status: waiting for the iPhone."));
        int r = run_connection(o, &b);
        if (r == 0) fails = 0;
        if (g_quit || !o->reconnect) { rc = r; break; }
        if (r != 0 && fails++ == 0)
            printf("%s\n", TX("iPhone を待っています。USB でつなぎ、アプリの「スピーカーとマイクとして使う」をオンにしてください。",
                              "Waiting for the iPhone. Connect it over USB and turn on Use as PC speaker and mic in the app."));
        else if (r == 0 && !g_wd_fired)
            printf("%s\n", TX("接続が切れました。再接続します。", "Disconnected. Reconnecting."));
        fflush(stdout);
        for (int i = 0; i < 20 && !g_quit; i++) Sleep(100);
        if (o->seconds > 0 && (ipa_now_us() - g_start_us) / 1000000 >= (uint64_t) o->seconds) request_quit();
        if (g_quit) break;
    }
    if (g_mo.open) micout_close(&g_mo);
    if (g_mon.open) micout_close(&g_mon);
    if (o->latency) wa_render_stop(&g_clk.r);
    if (have_keep) wa_render_stop(&keep);
    {
        FILETIME cr, ex, kr, us;
        GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kr, &us);
        double cpu = ((double) (((ULONGLONG) kr.dwHighDateTime << 32) | kr.dwLowDateTime) + (double) (((ULONGLONG) us.dwHighDateTime << 32) | us.dwLowDateTime)) / 10000.0;
        printf("RESULT cpu_ms=%.0f connections=%d\n", cpu, b.conn_no);
    }
    return rc;
}

// ---- 試験用：決まった音を出力の機器に鳴らす ----
typedef struct { double phase, step, amp; uint64_t frames; } ToneState;
static void tone_fill(void *u, float *out, int frames, int ch) {
    ToneState *t = (ToneState *) u;
    for (int i = 0; i < frames; i++) {
        float v = (float) (t->amp * sin(t->phase));
        t->phase += t->step;
        if (t->phase > 6.283185307179586) t->phase -= 6.283185307179586;
        for (int c = 0; c < ch; c++) out[(size_t) i * ch + c] = c < 2 ? v : 0.0f;
    }
    t->frames += (uint64_t) frames;
}

static int run_tone(Opt *o) {
    IMMDevice *d = wa_find(eRender, o->tone_dev);
    if (!d) { fprintf(stderr, "出力の機器が見つからない: %s\n", o->tone_dev); return 1; }
    WaRender r;
    ToneState t = { 0, 0, o->amp, 0 };
    if (!wa_render_open(&r, d, tone_fill, &t)) return 1;
    t.step = 6.283185307179586 * o->freq / r.rate;
    printf("音を出す: %s（%d Hz・%d ch）%.0f Hz 振幅 %.4f（%.1f dBFS）%d 秒\n", r.name, r.rate, r.ch, o->freq, o->amp, 20 * log10(o->amp), o->seconds);
    if (!wa_render_start(&r)) return 1;
    Sleep((DWORD) o->seconds * 1000);
    wa_render_stop(&r);
    printf("RESULT tone_frames=%llu rate=%d\n", (unsigned long long) t.frames, r.rate);
    return 0;
}

// ---- 試験用：loopback で取った音を WAV にする ----
static int run_capture(Opt *o) {
    IMMDevice *d = wa_find(o->cap_input ? eCapture : eRender, o->cap_dev);
    if (!d) { fprintf(stderr, "出力の機器が見つからない: %s\n", o->cap_dev); return 1; }
    WaCap c;
    WaRender keep;
    memset(&keep, 0, sizeof keep);
    if (!wa_cap_open(&c, d, !o->cap_input, o->cap_native ? 0 : 48000)) return 1;
    int mute = 0;
    float vol = wa_volume(d, &mute);
    printf("%s: %s（%d Hz・%d ch%s）音量 %.2f%s\n", o->cap_input ? "入力を録る" : "loopback", c.name, c.rate, c.ch, c.conv ? "・OS が変換" : "", vol, mute ? " ミュート" : "");
    int have_keep = 0;
    if (!o->cap_input && o->keepalive && wa_render_open(&keep, d, NULL, NULL) && wa_render_start(&keep)) have_keep = 1;
    IpaWav w;
    if (!ipa_wav_open(&w, o->cap_wav, (uint32_t) c.rate, 2)) return 1;
    IAudioClient_Start(c.ac);
    uint64_t t0 = ipa_now_us(), frames = 0, disc = 0, silent = 0, packets = 0;
    int16_t *tmp = malloc(sizeof(int16_t) * 2 * 48000);
    float peak = 0;
    while ((ipa_now_us() - t0) < (uint64_t) o->seconds * 1000000) {
        if (WaitForSingleObject(c.ev, 100) != WAIT_OBJECT_0) continue;
        for (;;) {
            UINT32 n = 0, fr = 0;
            BYTE *data = NULL;
            DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(c.cap, &n)) || !n) break;
            if (FAILED(IAudioCaptureClient_GetBuffer(c.cap, &data, &fr, &flags, NULL, NULL))) break;
            packets++;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) disc++;
            if (fr > 48000) fr = 48000;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { silent++; memset(tmp, 0, (size_t) fr * 4); }
            else wa_to_stereo_s16(&c, data, (int) fr, tmp);
            for (UINT32 i = 0; i < fr * 2; i++) { float a = fabsf((float) tmp[i] / 32768.0f); if (a > peak) peak = a; }
            ipa_wav_write(&w, tmp, (size_t) fr * 4);
            frames += fr;
            IAudioCaptureClient_ReleaseBuffer(c.cap, fr);
        }
    }
    double el = (double) (ipa_now_us() - t0) / 1e6;
    IAudioClient_Stop(c.ac);
    ipa_wav_close(&w);
    if (have_keep) wa_render_stop(&keep);
    printf("RESULT cap_frames=%llu cap_seconds=%.3f cap_rate_measured=%.1f packets=%llu silent_packets=%llu disc=%llu peak=%.5f master_volume=%.3f\n",
           (unsigned long long) frames, el, (double) frames / el, (unsigned long long) packets, (unsigned long long) silent, (unsigned long long) disc, peak, vol);
    return 0;
}

static int parse_mode(const char *s) {
    if (!strcmp(s, "app")) return IPA_MODE_APP;
    if (!strcmp(s, "raw")) return IPA_MODE_RAW;
    if (!strcmp(s, "standard")) return IPA_MODE_STANDARD;
    if (!strcmp(s, "voice")) return IPA_MODE_VOICE;
    fprintf(stderr, "--mic-mode は app・raw・standard・voice のどれか: %s\n", s);
    exit(2);
}

static int parse_format(const char *s) {
    if (!strcmp(s, "16")) return IPA_FMT_S16;
    if (!strcmp(s, "24")) return IPA_FMT_S24;
    if (!strcmp(s, "f32") || !strcmp(s, "32")) return IPA_FMT_F32;
    fprintf(stderr, "--mic-format は 16・24・f32 のどれか: %s\n", s);
    exit(2);
}

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    g_ja = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
    InitializeCriticalSection(&g_ui_cs);
    // 引数は UTF-16 で受けて UTF-8 にする（Shift_JIS の argv では、日本語の機器名が名前の一覧と合わない）
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc((size_t) argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t) n + 1);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    Opt o;
    memset(&o, 0, sizeof o);
    o.mux_port = 0; o.keepalive = 1; o.target_ms = 40; o.mic_target_ms = 40; o.chunk_ms = 10; o.seconds = 0; o.freq = 440; o.amp = 0.01;
    o.mic_channels = 2; o.mic_format = IPA_FMT_S16; o.mic_rate = 48000; o.mic_mode = IPA_MODE_APP; o.report_sec = 1;
    o.clicks = 20; o.click_amp = 0.3; o.click_freq = 2000; o.render_ms = 30;
    o.monitor_ms = 20; o.wd_new_sec = 8; o.wd_old_sec = 15;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s に値が要る\n", a), exit(2), (char *) 0))
        if (!strcmp(a, "--list")) o.list = 1;
        else if (!strcmp(a, "--usb")) o.usb = 1;
        else if (!strcmp(a, "--mux")) o.mux_port = atoi(NEXT());
        else if (!strcmp(a, "--device")) g_device = NEXT();
        else if (!strcmp(a, "--connect")) o.connect = NEXT();
        else if (!strcmp(a, "--speaker")) { o.use_speaker = 1; if (i + 1 < argc && argv[i + 1][0] != '-') o.spk_dev = argv[++i]; }
        else if (!strcmp(a, "--spk-wav")) o.spk_wav = NEXT();
        else if (!strcmp(a, "--gen")) { o.use_speaker = 1; o.gen = 1; }
        else if (!strcmp(a, "--mic")) o.use_mic = 1;
        else if (!strcmp(a, "--mic-wav")) { o.use_mic = 1; o.mic_wav = NEXT(); }
        else if (!strcmp(a, "--mic-render")) { o.use_mic = 1; o.mic_render = NEXT(); }
        else if (!strcmp(a, "--mic-gain")) g_mic_gain = (float) pow(10.0, atof(NEXT()) / 20.0);
        else if (!strcmp(a, "--mic-monitor")) { o.use_mic = 1; o.mic_monitor = NEXT(); }
        else if (!strcmp(a, "--mic-monitor-gain")) g_mon_gain = (float) pow(10.0, atof(NEXT()) / 20.0);
        else if (!strcmp(a, "--mic-monitor-ms")) o.monitor_ms = atoi(NEXT());
        else if (!strcmp(a, "--mic-wait-front-sec")) o.wd_new_sec = atoi(NEXT()); // 試験用：新しいアプリで、前面のままマイクが動かない秒数（既定 8・0 で切る）
        else if (!strcmp(a, "--mic-wait-old-sec")) o.wd_old_sec = atoi(NEXT());   // 試験用：前のアプリで、マイクを待つ秒数（既定 15・0 で切る）
        else if (!strcmp(a, "--mic-channels")) o.mic_channels = atoi(NEXT()) >= 2 ? 2 : 1;
        else if (!strcmp(a, "--mic-format")) o.mic_format = parse_format(NEXT());
        else if (!strcmp(a, "--mic-rate")) o.mic_rate = atoi(NEXT());
        else if (!strcmp(a, "--mic-mode")) o.mic_mode = parse_mode(NEXT());
        else if (!strcmp(a, "--voiceproc")) o.voiceproc = 1;
        else if (!strcmp(a, "--exclusive")) o.exclusive = 1;
        else if (!strcmp(a, "--no-keepalive")) o.keepalive = 0;
        else if (!strcmp(a, "--target-ms")) o.target_ms = atoi(NEXT());
        else if (!strcmp(a, "--mic-target-ms")) o.mic_target_ms = atoi(NEXT());
        else if (!strcmp(a, "--chunk-ms")) o.chunk_ms = atoi(NEXT());
        else if (!strcmp(a, "--seconds")) o.seconds = atoi(NEXT());
        else if (!strcmp(a, "--reconnect")) o.reconnect = 1;
        else if (!strcmp(a, "--window")) o.window = 1;
        else if (!strcmp(a, "--report-sec")) o.report_sec = atoi(NEXT());
        else if (!strcmp(a, "--render-ms")) o.render_ms = atoi(NEXT());
        else if (!strcmp(a, "--no-quiet-skip")) o.no_quiet_skip = 1;
        else if (!strcmp(a, "--latency-test")) { o.latency = 1; o.use_mic = 1; if (i + 1 < argc && argv[i + 1][0] != '-') o.lat_dev = argv[++i]; }
        else if (!strcmp(a, "--clicks")) o.clicks = atoi(NEXT());
        else if (!strcmp(a, "--click-amp")) o.click_amp = atof(NEXT());
        else if (!strcmp(a, "--click-freq")) o.click_freq = atof(NEXT());
        else if (!strcmp(a, "--tone")) o.tone_dev = NEXT();
        else if (!strcmp(a, "--freq")) o.freq = atof(NEXT());
        else if (!strcmp(a, "--amp")) o.amp = atof(NEXT());
        else if (!strcmp(a, "--capture")) o.cap_dev = NEXT();
        else if (!strcmp(a, "--record")) { o.cap_dev = NEXT(); o.cap_input = 1; }
        else if (!strcmp(a, "--native")) o.cap_native = 1;
        else if (!strcmp(a, "--wav")) o.cap_wav = NEXT();
        else { fprintf(stderr, "知らない引数: %s\n使い方は DESIGN.md を見る\n", a); return 2; }
    }
    if (o.mic_rate < 8000 || o.mic_rate > 96000) { fprintf(stderr, "--mic-rate は 8000〜96000\n"); return 2; }
    if (o.render_ms < 3 || o.render_ms > 500) { fprintf(stderr, "--render-ms は 3〜500\n"); return 2; }
    if (o.monitor_ms < 5 || o.monitor_ms > 200) { fprintf(stderr, "--mic-monitor-ms は 5〜200\n"); return 2; }
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    if (!wa_init()) { fprintf(stderr, "COM を始められない\n"); return 1; }
    if (o.list) { wa_list(); return 0; }
    if (o.tone_dev) return run_tone(&o);
    if (o.cap_dev) { if (!o.cap_wav) { fprintf(stderr, "--wav が要る\n"); return 2; } return run_capture(&o); }
    if (!o.connect && !o.usb) { fprintf(stderr, "--usb か --connect host:port が要る\n"); return 2; }
    if (!o.use_speaker && !o.use_mic) { fprintf(stderr, "--speaker か --mic が要る\n"); return 2; }
    if (o.latency && o.reconnect) { fprintf(stderr, "--latency-test と --reconnect は一緒に使えない\n"); return 2; }
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    g_start_us = ipa_now_us();
    snprintf(g_ui_state, sizeof g_ui_state, "%s", TX("状態：始めています。", "Status: starting."));
    int rc = run_bridge(&o);
    HWND hw = g_hwnd;
    if (hw) PostMessageW(hw, WM_CLOSE, 0, 0);
    fflush(stdout);
    return rc;
}
