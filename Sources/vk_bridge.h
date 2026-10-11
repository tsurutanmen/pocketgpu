// Swift から呼ぶ、Vulkan の命令の受け手（vk/remote/server.c）の入口
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// 受け手を始める。戻ってこない（アプリを閉じるまで待ち受け続ける）ので、別のスレッドで呼ぶ
int vkr_serve(const char * host, int port);
// 窓に出す絵を広げる関数（Swift の MetalFX）を登録する。受け手の糸から呼ばれる。成功で 1。vkr_serve の前に呼ぶ
typedef int (*vkr_upscale_fn)(const uint8_t * src, uint32_t w, uint32_t h, uint32_t stride, uint8_t * dst, uint32_t dw, uint32_t dh, uint32_t dstride);
void vkr_set_upscaler(vkr_upscale_fn fn);
// アプリが前にいないときは 1。その間は GPU に新しい仕事を出さずに待つ（出すと GPU を失う）
void vkr_set_paused(int on);
// iPhone の熱の状態（ProcessInfo.thermalState の 0〜3）。PC のドライバが聞いて、熱いときはコマ数を落とす
void vkr_set_thermal(int state);
// 受け手が処理した命令の数（つないでから増え続ける）と、いま PC がつながっているか。画面のファンを回すのに使う
uint64_t vkr_ops_total(void);
int vkr_clients(void);
// 窓に出すコマとして PC に返した数（つないでから増え続ける）。1 秒ごとの差で毎秒のコマ数を出す
uint64_t vkr_frames_total(void);
// アプリの画面に絵を出すか。1 の間だけ、PC に返した最新の JPEG を 1 枚写しておく。0 にすると写すのをやめ、持っていた 1 枚も捨てる
void vkr_set_preview(int on);
// 写しておいた最新の JPEG を buf に写し、大きさを返す。まだ無いときと cap が足りないときは 0。
// serial はコマが変わるたびに 1 増える（buf が NULL でも書く）。番号が変わったときだけ読み直す
int vkr_latest_frame(uint8_t * buf, int cap, uint64_t * serial);
// 受け手の記録に 1 行書く。熱・電池・低電力の移り変わりを残す
void vkr_note(const char * line);
// USB の通り道の速さを測る受け口（vk/remote/bw.c）。戻ってこないので別のスレッドで呼ぶ
int vkr_bw_serve(const char * host, int port);
// インターネット共有の口（USB を有線のネットワークとして使う道）。合言葉は 16 進 32 文字で、起動のたびに作る。
// 住所で待ち始め、同じ住所なら何もしない。空の住所なら口を閉じる。PC のドライバは USB の口で住所と合言葉を聞いてからつなぐ
void vkr_set_token(const char * hex);
int vkr_listen_extra(const char * host);
// アプリから 1 秒ごとに呼ぶ。on はアプリの設定（インターネット共有でも待ち受けるか）。受け手が、名前が bridge で始まり IPv4 が 172.20.10.0/28 の口を探して待ち、
// 消えたら閉じる（0.0.0.0 や Wi-Fi の en0 では待たない）。out に今待っている住所を書く（無ければ空）。Vulkan がまだ起きていなければ 2・待てなければ 1・ほかは 0
int vkr_hotspot_tick(int on, char * out, int cap);
void vkr_bw_set_token(const char * hex);
int vkr_bw_listen_extra(const char * host, int port);
// マイクラのサーバー（mc/pumpkin-ios・Pumpkin・GPL-3.0）。PUMPKIN を付けて組んだときだけ入っている。止まるまで戻らない
int pumpkin_ios_run(const char * dir);

#ifdef __cplusplus
}
#endif
