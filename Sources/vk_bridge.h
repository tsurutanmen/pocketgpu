// Swift から呼ぶ、Vulkan の命令の受け手（vk/remote/server.c）の入口
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// 受け手を始める。戻ってこない（アプリを閉じるまで待ち受け続ける）ので、別のスレッドで呼ぶ
int vkr_serve(const char * host, int port);

#ifdef __cplusplus
}
#endif
