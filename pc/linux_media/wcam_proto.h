// iPhone のカメラを PC のウェブカメラにする約束（ポート 50064）の型と番号だけ。work/camctl2/pc/wcam.h の約束の所の写し（Windows の物を読まない）。
// webcam_rx の Linux 版（webcam_rx_linux.c）が使う。数はすべてリトルエンディアン。メッセージは [u32 型][u32 長さ][中身] の繰り返し（両方向）。
// 約束を変えるときは wcam.h と一緒に直す
#pragma once
#include <stdint.h>

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
} wc_hello;
#define WC_HELLO_MIRROR 1u      // 映像を左右反転して送る（ふつうは反転しない。反転して見せるのは使う側のアプリ）
#define WC_HELLO_CAMERA_SET 2u  // camset：camera の値を PC が決めた（0 なら背面の広角）。立っていなくて camera が 0 なら「おまかせ」
#define WC_HELLO_LIVE 4u        // camset：PC は流している途中の WC_HELLO_ACK（形が変わった知らせ）を受けられる。
                                //   立っていれば、iPhone はアプリの画面で設定を変えたとき、その場でカメラとエンコーダを作り直し、ACK を送ってキーフレームから流す。
                                //   立っていなければ（古い webcam_rx は大きさが変わると壊れる）、大きさとコマ数の変更は次の接続まで待つ
                                // PC は同じつなぎのまま HELLO をもう一度送ってよい（形の頼み直し）。iPhone はカメラを作り直して ACK を返す（前の版のアプリも同じ）
#define WC_HELLO_CTRL 8u        // camctl2：PC は CTRL のメッセージを受け、送れる。立てる PC は WC_HELLO_LIVE も立てる

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
