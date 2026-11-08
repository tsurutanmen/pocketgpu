// iPhone のカメラを PC のウェブカメラにする約束（ポート 50064）と、PC 側の小さな道具。DESIGN.md の「約束」と同じ中身。
// 数はすべてリトルエンディアン。メッセージは [u32 型][u32 長さ][中身] の繰り返し（両方向）。
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define WCAM_PORT 50064u            // iPhone の中の 127.0.0.1:50064（USB の usbmuxd 越しに PC が届く）
#define WCAM_VERSION 1u

// メッセージの型
enum {
    WC_HELLO = 1,       // PC→iPhone：こういうカメラが欲しい（wc_hello）。つないだ直後に PC から送る
    WC_HELLO_ACK = 2,   // iPhone→PC：決まった形（wc_ack）。status が 0 以外なら、このあと iPhone が切る
    WC_FRAME = 3,       // iPhone→PC：1 コマ（wc_frame_head ＋ Annex B の NAL の並び。キーフレームには SPS・PPS が付く）
    WC_EVENT = 4,       // iPhone→PC：状態の知らせ（wc_event）
    WC_REQ_KEY = 16,    // PC→iPhone：次のコマをキーフレームにして（中身なし）。ほどけなくなったとき
    WC_SET_KBPS = 17,   // PC→iPhone：ビットレートを変える（u32 kbps）
    WC_PING = 20,       // PC→iPhone：時計合わせ（u64 PC の時刻 µs）
    WC_PONG = 21,       // iPhone→PC：（u64 PC の時刻 µs・u64 iPhone の時刻 µs）。iPhone の時刻は単調な時計
    // camctl2：PC から操作する（CTRL）。PC が HELLO で WC_HELLO_CTRL を立てたつなぎだけ。形は下の wc_ctl_* と camctl2/ctl_ids.md
    WC_CTRL_CAPS = 5,   // iPhone→PC：使える操作と範囲。ACK のあと（作り直すたび）と GET のあと
    WC_CTRL_STATE = 6,  // iPhone→PC：値（wc_ctl_val × n）。値が変わったとき
    WC_CTRL_SET = 18,   // PC→iPhone：値を変える（wc_ctl_val × n。why の所は flags で今は 0）
    WC_CTRL_GET = 19,   // PC→iPhone：CAPS と全部の値を送り直して（中身なし）
};

// 欲しいカメラ（0 は「おまかせ」＝ iPhone のアプリの画面の設定を使う）。camera：0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠
// camera の 0 は「おまかせ」と「背面の広角」の両方の意味になるので、PC が背面の広角を選ぶときは WC_HELLO_CAMERA_SET を立てる
typedef struct {
    uint32_t version, width, height, fps, kbps, gop, camera, flags;
    uint32_t code;          // Wi-Fi でつなぐときの合言葉（アプリの画面の 6 けた）。USB では見ない。古い iPhone は先頭の 32 バイトだけ読む
    uint32_t quality;       // camset で足した。0 おまかせ・1 標準・2 高画質・3 最高（kbps が 0 のときのビットレートの段）。古い iPhone は読まない
    uint32_t max_w, max_h;  // camgpu1015 で足した。WC_HELLO_FIT を立てたときだけ読まれる「PC が使える最大の大きさ」（48 バイト目まで）。古い iPhone は 40 バイトの後ろを読まない
} wc_hello;
#define WC_HELLO_MIRROR 1u      // 映像を左右反転して送る（ふつうは反転しない。反転して見せるのは使う側のアプリ）
#define WC_HELLO_CAMERA_SET 2u  // camset：camera の値を PC が決めた（0 なら背面の広角）。立っていなくて camera が 0 なら「おまかせ」
#define WC_HELLO_LIVE 4u        // camset：PC は流している途中の WC_HELLO_ACK（形が変わった知らせ）を受けられる。
                                //   立っていれば、iPhone はアプリの画面で設定を変えたとき、その場でカメラとエンコーダを作り直し、ACK を送ってキーフレームから流す。
                                //   立っていなければ（古い webcam_rx は大きさが変わると壊れる）、大きさとコマ数の変更は次の接続まで待つ
                                // PC は同じつなぎのまま HELLO をもう一度送ってよい（形の頼み直し）。iPhone はカメラを作り直して ACK を返す（前の版のアプリも同じ）
#define WC_HELLO_CTRL 8u        // camctl2：PC は CTRL のメッセージを受け、送れる。立てる PC は WC_HELLO_LIVE も立てる
#define WC_HELLO_FIT 16u        // camgpu1015：PC が使える絵は max_w x max_h の箱（縦横どちらの向きでもよい）まで。iPhone は、PC の指定も画面の設定も、この箱に収まる大きさで撮って送る。
                                //   iPhone 側で小さい形を撮る（撮れなければ、エンコーダの前に縮める）ので、PC は 4K を GPU でほどいて縮める仕事をしなくてよくなる。
                                //   古いアプリはこの印も max_w・max_h も読まない（ACK に WC_ACK_FIT が立たない）

typedef struct {
    uint32_t version;
    int32_t status;         // 0 できた・1 カメラの許可が無い・2 そのカメラが無い・3 アプリが前にいない（iOS は後ろでカメラを使えない）・4 エンコーダを作れない・5 頼みが変・6 Wi-Fi の合言葉が違う
    uint32_t width, height, fps, kbps, camera, flags;
} wc_ack;
// ACK の flags（camset で足した。古いアプリは 0）
#define WC_ACK_APP_SETTINGS 1u  // アプリの画面の設定を使った項目がある（PC の頼みが 0 だった・または下の優先の切り替え）
#define WC_ACK_APP_PRIORITY 2u  // アプリの画面で「PC からの指定を優先する」が切られていて、PC の指定よりアプリの設定を使った
#define WC_ACK_LIVE 4u          // この iPhone は流している途中で形を変えられる（WC_HELLO_LIVE を受ける版）
#define WC_ACK_CHANGED 8u       // 流している途中の ACK（アプリの画面で設定が変わった・PC の頼み直し）
#define WC_ACK_THERMAL 16u      // camctl：熱のためにコマ数を下げている（fps が下げたコマ数）。camctl2 から、熱で下げて始めた最初の ACK にも立つ（CTRL の PC だけ）
#define WC_ACK_CTRL 32u         // camctl2：この iPhone は CTRL を受ける（PC が WC_HELLO_CTRL を立てたときだけ立つ）。無ければ古いアプリ
#define WC_ACK_FIT 64u          // camgpu1015：この iPhone は WC_HELLO_FIT を守った（PC が立てたときだけ立つ）。無ければ古いアプリ（PC の指定が箱に収まっていても、画面の設定は箱を超えることがある）

// camctl2：CTRL の中身。CAPS は {u32 数}{wc_ctl_item × 数}{u32 倍率のボタンの数}{f32 × 数}{u32 選択肢の表の数}{u16 id・u16 数・f32 × 数 を表の数だけ}
typedef struct {
    uint16_t id, kind;      // kind：WC_CTL_TOGGLE など
    float min, max, step, value;
    uint32_t flags;         // WC_CTL_REBUILD など
} wc_ctl_item;              // 24 バイト
typedef struct {
    uint16_t id, why;       // why：0 全部（CAPS のあと・GET）・1 iPhone の画面で変えた・2 iOS が変えた・8 SET への返事。SET では 0
    float value;
} wc_ctl_val;               // 8 バイト
#define WC_CTL_TOGGLE 0u        // 0 と 1
#define WC_CTL_CHOICE 1u        // 選択肢の表の値のどれか
#define WC_CTL_NUMBER 2u        // min から max（step 刻み）
#define WC_CTL_READONLY 3u      // 読むだけ（100 から）
#define WC_CTL_REBUILD 1u       // 変えるとカメラを作り直す（ACK の CHANGED が来て、キーフレームから）
#define WC_CTL_UNAVAILABLE 2u   // 今は使えない（背景ぼかしの間のピント・前面カメラで iOS が背面のライトを許さない）
#define WC_CTL_LOG 4u           // 対数で見せる（ズーム・ISO）
#define WC_CTL_MAX_ITEMS 128
#define WC_CTL_MAX_LIST 64

// 1 コマの頭（32 バイト）。時刻は iPhone の単調な時計（µs）。PC は WC_PING で差を測る
typedef struct {
    uint64_t capture_us;    // カメラが撮った時刻（CMSampleBuffer の presentationTimeStamp）
    uint64_t sent_us;       // 送り始める直前の時刻
    uint32_t seq;
    uint32_t flags;         // 1：キーフレーム
    uint32_t width, height;
} wc_frame_head;
#define WC_FRAME_KEY 1u

typedef struct { uint32_t code, arg; } wc_event;
// code：1 カメラが止まった（アプリが後ろに回った・他のアプリが使った・電話）・2 カメラが戻った・3 熱の状態（arg が ProcessInfo.thermalState）
//       5 熱でコマ数を変えた（arg が新しいコマ数。上げて戻したときも送る。下げたのか戻したのかは直前の ACK の WC_ACK_THERMAL で分かる）

#include "wcam_pipe.h"

// ---- 時刻 ----
static inline double wc_qpc_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1e6 / (double) f.QuadPart;
}
static inline uint64_t wc_now_us(void) { return (uint64_t) wc_qpc_us(); }

// ---- 送る・受ける ----
static inline int wc_send_all(SOCKET s, const void *d, size_t n) {
    const char *p = (const char *) d;
    while (n) {
        int k = send(s, p, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= (size_t) k;
    }
    return 1;
}
static inline int wc_recv_all(SOCKET s, void *d, size_t n) {
    char *p = (char *) d;
    while (n) {
        int k = recv(s, p, n > (1u << 30) ? (1 << 30) : (int) n, 0);
        if (k <= 0) return 0;
        p += k;
        n -= (size_t) k;
    }
    return 1;
}
// 頭（型・長さ）と中身を 1 回の send に。小さな send を 2 回に分けると Nagle と遅延 ACK で 40 ms 待たされることがあるので、まとめて送る
static inline int wc_send_msg(SOCKET s, uint32_t type, const void *body, uint32_t len) {
    uint8_t *b = (uint8_t *) malloc(8 + (size_t) len);
    if (!b) return 0;
    memcpy(b, &type, 4);
    memcpy(b + 4, &len, 4);
    if (len) memcpy(b + 8, body, len);
    int ok = wc_send_all(s, b, 8 + (size_t) len);
    free(b);
    return ok;
}
// 1 つ受ける。*body は malloc（呼ぶ側が free）。上限は max バイト。切れたら 0
static inline int wc_recv_msg(SOCKET s, uint32_t *type, uint8_t **body, uint32_t *len, uint32_t max) {
    uint32_t h[2];
    if (!wc_recv_all(s, h, 8)) return 0;
    if (h[1] > max) return 0;
    uint8_t *b = (uint8_t *) malloc(h[1] ? h[1] : 1);
    if (!b) return 0;
    if (h[1] && !wc_recv_all(s, b, h[1])) { free(b); return 0; }
    *type = h[0], *body = b, *len = h[1];
    return 1;
}

// ---- つなぐ ----
static inline SOCKET wc_tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCKET && connect(s, res->ai_addr, (int) res->ai_addrlen) != 0) { closesocket(s); s = INVALID_SOCKET; }
    freeaddrinfo(res);
    return s;
}

// usbmuxd（Apple Mobile Device Service・127.0.0.1:27015）。icd.c の usb_connect と同じやり方。mux_port は試験用に偽物へ向けられる
static inline char *wc_mux_call(SOCKET s, const char *type, const char *extra) {
    char body[1024];
    int n = snprintf(body, sizeof body,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>%s</string>"
        "<key>ClientVersionString</key><string>iphone-webcam</string><key>ProgName</key><string>iphone-webcam</string>%s</dict></plist>\n",
        type, extra);
    uint32_t h[4] = { 16 + (uint32_t) n, 1, 8, 1 };
    if (!wc_send_all(s, h, sizeof h) || !wc_send_all(s, body, (size_t) n)) return NULL;
    if (!wc_recv_all(s, h, sizeof h) || h[0] < 16 || h[0] > (1u << 20)) return NULL;
    char *r = (char *) malloc(h[0] - 16 + 1);
    if (!r) return NULL;
    if (!wc_recv_all(s, r, h[0] - 16)) { free(r); return NULL; }
    r[h[0] - 16] = 0;
    return r;
}
static inline long long wc_plist_int_after(const char *at, const char *key, const char *limit) {
    char k[64];
    snprintf(k, sizeof k, "<key>%s</key>", key);
    const char *q = strstr(at, k);
    if (!q || (limit && q > limit)) return -1;
    q = strstr(q, "<integer>");
    return q ? atoll(q + 9) : -1;
}
// 鍵の後ろの <string> の中身を out に写す（limit より後ろは見ない）。無ければ 0
static inline int wc_plist_str_after(const char *at, const char *key, const char *limit, char *out, size_t cap) {
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

// usbmuxd が見ている機器（camset で足した。iPhone が何台つながっていても選べるように）
typedef struct {
    long long id;        // usbmuxd の DeviceID（つなぎ直すと変わる）
    long long product;   // USB の ProductID
    char udid[64];       // SerialNumber（UDID。変わらない）
    char type[16];       // USB か Network
} wc_dev;

// 一覧を読む。返すのは数（最大 max）。usbmuxd につながらなければ -1
static inline int wc_usb_list(int mux_port, wc_dev *out, int max) {
    char mport[16];
    snprintf(mport, sizeof mport, "%d", mux_port > 0 ? mux_port : 27015);
    SOCKET s = wc_tcp_connect("127.0.0.1", mport);
    if (s == INVALID_SOCKET) return -1;
    char *list = wc_mux_call(s, "ListDevices", "");
    closesocket(s);
    if (!list) return -1;
    int n = 0;
    // 機器ごとに Properties の dict がある。その中の ConnectionType・DeviceID・ProductID・SerialNumber を読む
    for (const char *q = list; n < max && (q = strstr(q, "<key>Properties</key>")); q++) {
        const char *end = strstr(q, "</dict>");
        wc_dev d;
        memset(&d, 0, sizeof d);
        d.id = wc_plist_int_after(q, "DeviceID", end);
        d.product = wc_plist_int_after(q, "ProductID", end);
        wc_plist_str_after(q, "ConnectionType", end, d.type, sizeof d.type);
        wc_plist_str_after(q, "SerialNumber", end, d.udid, sizeof d.udid);
        if (d.id >= 0) out[n++] = d;
    }
    free(list);
    return n;
}

// UDID が want と合うか。大文字・小文字と「-」は見ない。6 文字以上なら前の方の一致でよい（画面に出した短い形でも選べる）
static inline int wc_udid_match(const char *udid, const char *want) {
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

// 使う機器の DeviceID を決める。want：NULL か空なら USB の最初の 1 台・数字だけなら DeviceID・それ以外は UDID。無ければ -1
static inline long long wc_usb_pick(int mux_port, const char *want, int quiet) {
    wc_dev d[32];
    int n = wc_usb_list(mux_port, d, 32);
    if (n < 0) { if (!quiet) fprintf(stderr, "Apple Mobile Device Service（usbmuxd）につながらないか、一覧を返さない\n"); return -1; }
    int all_digits = want && *want;
    for (const char *p = want; p && *p; p++) if (*p < '0' || *p > '9') all_digits = 0;
    int usb = 0;
    long long first = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(d[i].type, "USB")) continue;
        usb++;
        if (first < 0) first = d[i].id;
        if (want && *want && ((all_digits && d[i].id == atoll(want)) || (!all_digits && wc_udid_match(d[i].udid, want)))) return d[i].id;
    }
    if (want && *want) { if (!quiet) fprintf(stderr, "USB でつながった iPhone の中に「%s」が無い（--list で一覧を見る）\n", want); return -1; }
    if (first < 0) { if (!quiet) fprintf(stderr, "USB でつながった iPhone が見えない\n"); return -1; }
    if (usb > 1 && !quiet) fprintf(stderr, "USB の iPhone が %d 台ある。最初の 1 台（DeviceID %lld）を使う。選ぶときは --device DeviceID か UDID\n", usb, first);
    return first;
}

static inline SOCKET wc_usb_connect_dev(int port, int mux_port, const char *want, int quiet) {
    char mport[16];
    snprintf(mport, sizeof mport, "%d", mux_port > 0 ? mux_port : 27015);
    long long dev = wc_usb_pick(mux_port, want, quiet);
    if (dev < 0) return INVALID_SOCKET;
    SOCKET s = wc_tcp_connect("127.0.0.1", mport);
    if (s == INVALID_SOCKET) return s;
    char extra[160];
    snprintf(extra, sizeof extra, "<key>DeviceID</key><integer>%lld</integer><key>PortNumber</key><integer>%u</integer>", dev, (unsigned) htons((u_short) port));
    char *res = wc_mux_call(s, "Connect", extra);
    long long num = res ? wc_plist_int_after(res, "Number", NULL) : -1;
    free(res);
    if (num != 0) { closesocket(s); if (!quiet) fprintf(stderr, "iPhone の %d 番につながらない（usbmuxd の返事 %lld）\n", port, num); return INVALID_SOCKET; }
    return s;
}
// 前の形（USB の最初の 1 台）
static inline SOCKET wc_usb_connect(int port, int mux_port) { return wc_usb_connect_dev(port, mux_port, NULL, 0); }
