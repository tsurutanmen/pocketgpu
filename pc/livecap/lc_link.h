// USB の iPhone の 50057 番（AppleFeatures.swift の受け口）と話す。Python 版の iphone_link.py と同じ約束。
//   送り [u32 長さ][JSON の頼み][u32 長さ][中身]、返し [u32 長さ][JSON の答え]。答えに "bin_len" があれば、もう 1 枠続く。
//   流しっぱなしの文字起こし（live_asr）は、最初の頼みのあと、そのつなぎが流す形に変わる：
//     PC → iPhone：{"t":"pcm","seq":n,"at":先頭のコマ番号}＋s16le・{"t":"ping","pc":秒}・{"t":"end"}
//     iPhone → PC：{"t":"partial"|"final","text","start","end"}・pong・stats・error・done
//   USB は usbmuxd（Apple Mobile Device Service・127.0.0.1:27015）に「この iPhone のこの番号へ」と頼む（icd.c の usb_connect と同じ）。
//   --addr を渡すと USB を使わずにその番地へ直接つなぐ（iPhone の役 fake_iphone.py に向けるとき）。
#pragma once

#define IPHONE_PORT 50057

static int send_all(SOCKET s, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        int k = send(s, c, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        c += k;
        n -= (size_t) k;
    }
    return 1;
}
static int recv_all(SOCKET s, void *p, size_t n) {
    char *c = p;
    while (n) {
        int k = recv(s, c, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        c += k;
        n -= (size_t) k;
    }
    return 1;
}
static void sock_timeout(SOCKET s, int ms) {
    DWORD t = (DWORD) ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *) &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *) &t, sizeof t);
}
static void sock_nodelay(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof one);
}

// host:port に TCP でつなぐ。ms でつながらなければ諦める
static SOCKET tcp_connect_ms(const char *host, const char *port, int ms) {
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(ai); return s; }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, ai->ai_addr, (int) ai->ai_addrlen);
    freeaddrinfo(ai);
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    FD_ZERO(&ef);
    FD_SET(s, &ef);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int ok = select(0, NULL, &wf, &ef, &tv) == 1 && FD_ISSET(s, &wf);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    if (!ok) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

static void split_hostport(const char *a, char *host, size_t nh, char *port, size_t np, const char *defport) {
    snprintf(host, nh, "127.0.0.1");
    snprintf(port, np, "%s", defport);
    if (!a || !*a) return;
    const char *c = strrchr(a, ':');
    if (c) {
        size_t n = (size_t) (c - a);
        if (n >= nh) n = nh - 1;
        if (n) { memcpy(host, a, n); host[n] = 0; }
        snprintf(port, np, "%s", c + 1);
    } else {
        snprintf(port, np, "%s", a);
    }
}

// ---- usbmuxd ----
static char *mux_call(SOCKET s, const char *type, const char *extra) {
    char body[1024];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>livecap</string><key>ProgName</key><string>livecap</string>%s</dict></plist>\n",
        type, extra);
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    if (!send_all(s, h, sizeof h) || !send_all(s, body, (size_t) n)) return NULL;
    if (!recv_all(s, h, sizeof h) || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = malloc(h[0] - 16 + 1);
    if (!r) return NULL;
    if (!recv_all(s, r, h[0] - 16)) { free(r); return NULL; }
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

// mux：usbmuxd の番地（"host:port" か "port"）。NULL なら USBMUXD_SOCKET_ADDRESS、無ければ 127.0.0.1:27015
static SOCKET usb_connect(int port, const char *mux, Buf *err) {
    char host[256], mport[32], where[300];
    const char *env = getenv("USBMUXD_SOCKET_ADDRESS");
    const char *a = mux && *mux ? mux : env && *env && strncmp(env, "UNIX:", 5) ? env : NULL;
    split_hostport(a, host, sizeof host, mport, sizeof mport, "27015");
    snprintf(where, sizeof where, "%s:%s", host, mport);
    SOCKET s = tcp_connect_ms(host, mport, 3000);
    if (s == INVALID_SOCKET) { buf_printf(err, TT(T_MUX_DOWN), where); return s; }
    sock_timeout(s, 5000);
    char *list = mux_call(s, "ListDevices", "");
    closesocket(s);
    if (!list) { buf_puts(err, TT(T_MUX_NOLIST)); return INVALID_SOCKET; }
    long long dev = -1;  // ConnectionType が USB の機器（Wi-Fi 越しの同期は使わない）
    for (const char *q = list; dev < 0 && (q = strstr(q, "<key>ConnectionType</key>")); q++) {
        const char *v = strstr(q, "<string>");
        const char *end = strstr(q, "</dict>");
        if (v && !strncmp(v + 8, "USB<", 4)) dev = plist_int_after(q, "DeviceID", end);
    }
    free(list);
    if (dev < 0) { buf_puts(err, TT(T_NO_IPHONE)); return INVALID_SOCKET; }
    s = tcp_connect_ms(host, mport, 3000);
    if (s == INVALID_SOCKET) { buf_printf(err, TT(T_MUX_DOWN), where); return s; }
    sock_timeout(s, 5000);
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>",
             dev, (unsigned) htons((u_short) port));
    char *res = mux_call(s, "Connect", extra);
    long long num = res ? plist_int_after(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { closesocket(s); buf_printf(err, TT(T_PORT_REFUSED), num); return INVALID_SOCKET; }
    return s;  // ここから先は iPhone の port 番への通り道
}

// addr があればそこへ、無ければ USB の iPhone の 50057 番へ
static SOCKET link_open(const char *addr, const char *mux, Buf *err) {
    SOCKET s;
    if (addr && *addr) {
        char host[256], port[32];
        split_hostport(addr, host, sizeof host, port, sizeof port, "50057");
        s = tcp_connect_ms(host, port, 10000);
        if (s == INVALID_SOCKET) buf_printf(err, TT(T_ADDR_FAIL), addr);
    } else {
        s = usb_connect(IPHONE_PORT, mux, err);
    }
    if (s != INVALID_SOCKET) sock_nodelay(s);
    return s;
}

// [u32][JSON][u32][中身] をまとめて 1 回で送る
static int send_req(SOCKET s, const char *json, const void *body, uint32_t blen) {
    uint32_t a = (uint32_t) strlen(json);
    size_t n = 8 + (size_t) a + blen;
    char stackbuf[8192], *p = n <= sizeof stackbuf ? stackbuf : malloc(n);
    if (!p) return 0;
    memcpy(p, &a, 4);
    memcpy(p + 4, json, a);
    memcpy(p + 4 + a, &blen, 4);
    if (blen) memcpy(p + 8 + a, body, blen);
    int ok = send_all(s, p, n);
    if (p != stackbuf) free(p);
    return ok;
}
static char *recv_json(SOCKET s) {
    uint32_t n;
    if (!recv_all(s, &n, 4) || n > (64u << 20)) return NULL;
    char *p = malloc((size_t) n + 1);
    if (!p) return NULL;
    if (!recv_all(s, p, n)) { free(p); return NULL; }
    p[n] = 0;
    return p;
}
// 1 つ頼んで 1 つ答えを受ける。2 枠目（bin_len）は読み捨てる
static char *call_req(SOCKET s, const char *json, const void *body, uint32_t blen) {
    if (!send_req(s, json, body, blen)) return NULL;
    char *ans = recv_json(s);
    double bl = 0;
    if (ans && js_get_num(ans, "bin_len", &bl) && bl > 0) {
        uint32_t m;
        if (!recv_all(s, &m, 4)) { free(ans); return NULL; }
        char tmp[4096];
        while (m) {
            uint32_t k = m > sizeof tmp ? (uint32_t) sizeof tmp : m;
            if (!recv_all(s, tmp, k)) { free(ans); return NULL; }
            m -= k;
        }
    }
    return ans;
}

// ---- 流しっぱなしの文字起こし ----
static struct {
    SOCKET s;
    CRITICAL_SECTION cs;
    volatile LONG closed;
    HANDLE reader;
    long long seq;
} LV = { .s = INVALID_SOCKET };

static int live_send(const char *json, const void *body, uint32_t blen) {
    if (LV.s == INVALID_SOCKET) return 0;
    EnterCriticalSection(&LV.cs);
    int ok = send_req(LV.s, json, body, blen);
    LeaveCriticalSection(&LV.cs);
    return ok;
}

static DWORD WINAPI live_reader(LPVOID arg) {
    (void) arg;
    int got_done = 0;
    for (;;) {
        char *j = recv_json(LV.s);
        if (!j) break;
        double t = now_s();
        int done = js_is_t(j, "done");
        q_push(EV_JSON, 0, j, t);
        if (done) { got_done = 1; break; }
    }
    if (!got_done && !LV.closed) {
        Buf b = { 0 };
        buf_puts(&b, "{\"t\":\"error\",\"error\":");
        buf_json_str(&b, TT(T_LINK_LOST));
        buf_puts(&b, "}");
        q_push(EV_JSON, 0, b.p, now_s());
    }
    q_push(EV_CLOSED, 0, NULL, now_s());
    return 0;
}

// 始める。ready の JSON を *ready に（呼んだ側が free）。だめなら err に理由
static int live_start(const char *addr, const char *mux, const char *lang, int rate, int fast, char **ready, Buf *err) {
    InitializeCriticalSection(&LV.cs);
    LV.s = link_open(addr, mux, err);
    if (LV.s == INVALID_SOCKET) return 0;
    sock_timeout(LV.s, 120000);  // 言葉のモデルの初回のダウンロードがあると長くかかる
    Buf q = { 0 };
    buf_puts(&q, "{\"op\":\"live_asr\",\"lang\":");
    buf_json_str(&q, lang);
    buf_printf(&q, ",\"rate\":%d,\"channels\":1,\"format\":\"s16le\",\"volatile\":true,\"fast\":%s,\"timestamps\":true}", rate, fast ? "true" : "false");
    char *ans = call_req(LV.s, q.p, NULL, 0);
    buf_free(&q);
    if (!ans) { buf_puts(err, TT(T_NO_ANSWER)); closesocket(LV.s); LV.s = INVALID_SOCKET; return 0; }
    char *e = js_get_str(ans, "error");
    if (e) {
        buf_printf(err, TT(T_START_FAIL), e);
        free(e);
        free(ans);
        closesocket(LV.s);
        LV.s = INVALID_SOCKET;
        return 0;
    }
    sock_timeout(LV.s, 0);
    *ready = ans;
    LV.reader = CreateThread(NULL, 0, live_reader, NULL, 0, NULL);
    return 1;
}

static int live_send_pcm(const int16_t *pcm, int n, long long at) {
    char h[96];
    snprintf(h, sizeof h, "{\"t\":\"pcm\",\"seq\":%lld,\"at\":%lld}", ++LV.seq, at);
    return live_send(h, pcm, (uint32_t) n * 2);
}
static void live_ping(double pc) {
    char h[96];
    snprintf(h, sizeof h, "{\"t\":\"ping\",\"pc\":%.6f}", pc);
    live_send(h, NULL, 0);
}
static void live_end(void) { live_send("{\"t\":\"end\"}", NULL, 0); }
static void live_close(void) {
    InterlockedExchange(&LV.closed, 1);
    if (LV.s != INVALID_SOCKET) {
        shutdown(LV.s, SD_BOTH);
        if (LV.reader) { WaitForSingleObject(LV.reader, 2000); CloseHandle(LV.reader); LV.reader = NULL; }
        closesocket(LV.s);
        LV.s = INVALID_SOCKET;
    }
}

// ---- 訳す（iPhone の translate）。確定した文を 1 つずつ、別のつなぎで ----
typedef struct TrItem { int fid; char *text; struct TrItem *next; } TrItem;
static struct {
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    TrItem *head, *tail;
    char from[32], to[32];
    const char *addr, *mux;
    volatile LONG pending;
    HANDLE th;
    SOCKET s;
    CRITICAL_SECTION scs;
    int hinted;  // 言語データの案内を出した
} TR = { .s = INVALID_SOCKET };

// 訳せなかった（fix1012）。字幕の訳の行は出さず、理由はコンソール（記録）にだけ書く。
// iPhone の答えの error は短い理由、detail は詳しい理由、code は種類（アプリ 0.7.60 から。前のアプリは error に長い文だけ）
static void tr_report_fail(int prep, const char *ans, Buf *err) {
    char *e = ans ? js_get_str(ans, "error") : NULL;
    char *d = ans ? js_get_str(ans, "detail") : NULL;
    char *code = ans ? js_get_str(ans, "code") : NULL;
    const char *why = e ? e : err->n ? buf_str(err) : ans ? "?" : TT(T_NO_ANSWER);
    warn(prep ? TT(T_TR_PREP_FAIL) : TT(T_TR_FAIL), why);
    if (d && *d) warn(TT(T_TR_DETAIL), d);
    if (code && !strcmp(code, "not_installed") && !TR.hinted) {
        TR.hinted = 1;
        warn(TT(T_TR_HINT), TR.from, TR.to);
    }
    free(e);
    free(d);
    free(code);
}

// 待ち行列の先頭の text が NULL の物は、翻訳の準備の頼み（tr_start が積む。字幕を始めるときに 1 回）。
// 準備の間に確定した文は、この糸が 1 つずつ処理するので、準備が終わってから訳される
static DWORD WINAPI tr_thread(LPVOID arg) {
    (void) arg;
    for (;;) {
        AcquireSRWLockExclusive(&TR.lk);
        while (!TR.head) SleepConditionVariableSRW(&TR.cv, &TR.lk, INFINITE, 0);
        TrItem *it = TR.head;
        TR.head = it->next;
        if (!TR.head) TR.tail = NULL;
        ReleaseSRWLockExclusive(&TR.lk);
        int prep = it->text == NULL;
        double t0 = now_s();
        if (prep) say(TT(T_TR_PREPARING), TR.from, TR.to);
        Buf err = { 0 };
        EnterCriticalSection(&TR.scs);
        if (TR.s == INVALID_SOCKET) {
            TR.s = link_open(TR.addr, TR.mux, &err);
            if (TR.s != INVALID_SOCKET) sock_timeout(TR.s, 60000);
        }
        SOCKET s = TR.s;
        LeaveCriticalSection(&TR.scs);
        char *ans = NULL;
        if (s != INVALID_SOCKET) {
            Buf q = { 0 };
            if (prep) {
                buf_puts(&q, "{\"op\":\"translate_prepare\",\"from\":");
            } else {
                buf_puts(&q, "{\"op\":\"translate\",\"text\":");
                buf_json_str(&q, it->text);
                buf_puts(&q, ",\"from\":");
            }
            buf_json_str(&q, TR.from);
            buf_puts(&q, ",\"to\":");
            buf_json_str(&q, TR.to);
            buf_puts(&q, "}");
            ans = call_req(s, q.p, NULL, 0);
            buf_free(&q);
            if (!ans) {
                EnterCriticalSection(&TR.scs);
                if (TR.s == s) { closesocket(s); TR.s = INVALID_SOCKET; }
                LeaveCriticalSection(&TR.scs);
            }
        }
        if (prep) {
            char *e = ans ? js_get_str(ans, "error") : NULL;
            char *code = ans ? js_get_str(ans, "code") : NULL;
            if (ans && !e) say(TT(T_TR_READY), (now_s() - t0) * 1000.0);
            else if (!ans || code) tr_report_fail(1, ans, &err);
            // code の無い失敗は、translate_prepare を知らない前のアプリ。準備なしで、これまでどおり 1 文ずつ訳す
            free(e);
            free(code);
        } else {
            char *text = ans ? js_get_str(ans, "text") : NULL;
            char *tr = NULL;
            if (text && *text) tr = xstrdup(text);
            else tr_report_fail(0, ans, &err);
            free(text);
            q_push(EV_TRANSLATION, it->fid, tr, now_s()); // 訳せなかったときは NULL。字幕に訳の行を出さない
        }
        free(ans);
        buf_free(&err);
        free(it->text);
        free(it);
    }
    return 0;
}

static void tr_start(const char *addr, const char *mux, const char *lang, const char *to) {
    InitializeCriticalSection(&TR.scs);
    InitializeSRWLock(&TR.lk);
    InitializeConditionVariable(&TR.cv);
    TR.addr = addr;
    TR.mux = mux;
    snprintf(TR.from, sizeof TR.from, "%s", lang);
    char *dash = strchr(TR.from, '-');  // ja-JP → ja
    if (dash) *dash = 0;
    snprintf(TR.to, sizeof TR.to, "%s", to);
    // 最初の文より先に、iPhone で翻訳の準備を済ませる（fix1012。text が NULL の頼み。数える pending には入れない）
    TrItem *prep = calloc(1, sizeof *prep);
    if (prep) {
        prep->fid = -1;
        TR.head = TR.tail = prep;
    }
    TR.th = CreateThread(NULL, 0, tr_thread, NULL, 0, NULL);
}
static void tr_add(int fid, const char *text) {
    TrItem *it = calloc(1, sizeof *it);
    if (!it) return;
    it->fid = fid;
    it->text = xstrdup(text);
    InterlockedIncrement(&TR.pending);
    AcquireSRWLockExclusive(&TR.lk);
    if (TR.tail) TR.tail->next = it; else TR.head = it;
    TR.tail = it;
    ReleaseSRWLockExclusive(&TR.lk);
    WakeConditionVariable(&TR.cv);
}
// 終わるときに、翻訳のつなぎを行儀よく閉じる（閉じずに終わると、相手に「切られた」と見える）
static void tr_close(void) {
    if (!TR.th) return;
    EnterCriticalSection(&TR.scs);
    if (TR.s != INVALID_SOCKET) {
        shutdown(TR.s, SD_SEND);
        closesocket(TR.s);
        TR.s = INVALID_SOCKET;
    }
    LeaveCriticalSection(&TR.scs);
}
