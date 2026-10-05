// Swift から呼ぶ、llama.cpp の計算の受け手（ggml-rpc）の入口
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// ggml の記録を手元にためる。最初に1回だけ呼ぶ
void rpc_bridge_init(void);
// ためた記録の後ろの部分を buf に書く。書いた長さを返す
int rpc_bridge_log(char * buf, int len);
// 計算に使える装置（Metal の GPU・CPU）と、そのメモリを buf に書く
int rpc_bridge_devices(char * buf, int len);
// 受け手を始める。戻ってこない（アプリを閉じるまで待ち受け続ける）ので、別のスレッドで呼ぶ
void rpc_bridge_serve(const char * host, int port, int n_threads, int use_gpu);
// アプリが後ろに回ったら 1。後ろの間は GPU が使えないので CPU で計算する
void rpc_bridge_set_background(int on);
// 最後の計算を CPU でしたら 1
int rpc_bridge_last_on_cpu(void);

#ifdef __cplusplus
}
#endif
