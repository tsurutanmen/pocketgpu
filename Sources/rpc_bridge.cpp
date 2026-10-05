// llama.cpp の tools/rpc/rpc-server.cpp と同じことを、iPhone のアプリの中でする
#include "rpc_bridge.h"

#include "ggml.h"
#include "ggml-backend.h"
#include "ggml-rpc.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

static std::mutex g_mu;
static std::string g_log;

static void on_log(enum ggml_log_level, const char * text, void *) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_log += text;
    if (g_log.size() > 24000) g_log.erase(0, g_log.size() - 16000);
}

static int copy_out(const std::string & s, char * buf, int len) {
    if (len <= 0) return 0;
    const size_t n = std::min(s.size(), (size_t)len - 1);
    memcpy(buf, s.data() + (s.size() - n), n); // 長すぎるときは後ろを残す
    buf[n] = 0;
    return (int)n;
}

extern "C" void rpc_bridge_init(void) {
    ggml_log_set(on_log, nullptr);
}

extern "C" int rpc_bridge_log(char * buf, int len) {
    std::lock_guard<std::mutex> lk(g_mu);
    return copy_out(g_log, buf, len);
}

extern "C" int rpc_bridge_devices(char * buf, int len) {
    std::string s;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        size_t free = 0, total = 0;
        ggml_backend_dev_memory(dev, &free, &total);
        s += std::string(ggml_backend_dev_name(dev)) + ": " + ggml_backend_dev_description(dev) +
             " (" + std::to_string(total / 1048576) + " MiB, 空き " + std::to_string(free / 1048576) + " MiB)\n";
    }
    return copy_out(s, buf, len);
}

// patches/rpc_no_abort.py が ggml-rpc.cpp に足す
extern "C" void ggml_rpc_set_force_cpu(int on);
extern "C" int ggml_rpc_last_on_cpu(void);

extern "C" void rpc_bridge_set_background(int on) {
    ggml_rpc_set_force_cpu(on);
    on_log(GGML_LOG_LEVEL_INFO, on ? "後ろに回った：CPU で計算する\n" : "前に戻った：GPU で計算する\n", nullptr);
}

extern "C" int rpc_bridge_last_on_cpu(void) {
    return ggml_rpc_last_on_cpu();
}

extern "C" void rpc_bridge_serve(const char * host, int port, int n_threads, int use_gpu) {
    std::vector<ggml_backend_dev_t> devices;
    if (use_gpu) {
        for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
            ggml_backend_dev_t dev = ggml_backend_dev_get(i);
            const auto t = ggml_backend_dev_type(dev);
            if (t != GGML_BACKEND_DEVICE_TYPE_CPU && t != GGML_BACKEND_DEVICE_TYPE_ACCEL) devices.push_back(dev);
        }
    }
    if (devices.empty()) {
        ggml_backend_dev_t cpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
        if (cpu) devices.push_back(cpu);
    }
    const std::string endpoint = std::string(host) + ":" + std::to_string(port);
    on_log(GGML_LOG_LEVEL_INFO, ("受け手を始める: " + endpoint + "\n").c_str(), nullptr);
    ggml_backend_rpc_start_server(endpoint.c_str(), nullptr, (size_t)n_threads, devices.size(), devices.data());
    on_log(GGML_LOG_LEVEL_INFO, "受け手が止まった\n", nullptr);
}
