// Swift から呼ぶ、Vulkan の命令の受け手（vk/remote/server.c）の入口
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// 受け手を始める。戻ってこない（アプリを閉じるまで待ち受け続ける）ので、別のスレッドで呼ぶ
int vkr_serve(const char * host, int port);
// アプリが前にいないときは 1。その間は GPU に新しい仕事を出さずに待つ（出すと GPU を失う）
void vkr_set_paused(int on);
// USB の通り道の速さを測る受け口（vk/remote/bw.c）。戻ってこないので別のスレッドで呼ぶ
int vkr_bw_serve(const char * host, int port);
// マイクラのサーバー（mc/pumpkin-ios・Pumpkin・GPL-3.0）。PUMPKIN を付けて組んだときだけ入っている。止まるまで戻らない
int pumpkin_ios_run(const char * dir);

#ifdef __cplusplus
}
#endif
