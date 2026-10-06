"""iPhone の受け手で、GPU が使えないときは CPU で計算し、それでもだめならアプリを落とさない。

iPhone では、アプリが後ろに回ると GPU を使えなくなり、Metal の計算が失敗する。
- アプリが後ろにいる間（ggml_rpc_set_force_cpu(1)）は、はじめから CPU で計算する
- 前にいても GPU が失敗したら、CPU でやり直す
- iPhone の Metal のメモリは CPU からもそのまま読めるので、モデルを送り直さずに切り替えられる
  ただし Metal の「共有」バッファは is_host に false と答えるので、Metal 自身に共有かを尋ねて見分ける
  （0.6.0 まではここを is_host だけで見ていて、CPU への切り替えが一度も効いていなかった）
- Metal は一度失敗すると、作り直すまで失敗を返し続ける。失敗したら計算係（backend）だけ作り直す。
  バッファは装置の物なので、作り直しても消えない
- 元の ggml-rpc は失敗で abort するので、その接続だけ切って次の依頼を待つようにする
"""
import pathlib
import sys

p = pathlib.Path(sys.argv[1]) / "ggml" / "src" / "ggml-rpc" / "ggml-rpc.cpp"
src = p.read_text(encoding="utf-8")

helpers = r'''
// ---- iPhone の受け手：GPU が使えないときは CPU で計算する ----
#include "ggml-metal.h"
// ggml-metal-device.m にある（Metal の共有バッファなら CPU からそのまま読める）
extern "C" bool ggml_metal_buffer_is_shared(void * buf);
static std::atomic<int> g_rpc_force_cpu{0};
extern "C" void ggml_rpc_set_force_cpu(int on) { g_rpc_force_cpu.store(on); }
extern "C" int ggml_rpc_last_on_cpu(void);
static std::atomic<int> g_rpc_last_on_cpu{0};
extern "C" int ggml_rpc_last_on_cpu(void) { return g_rpc_last_on_cpu.load(); }

static ggml_backend_t rpc_cpu_backend() {
    static ggml_backend_t cpu = [] {
        ggml_backend_dev_t dev = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
        ggml_backend_t b = dev ? ggml_backend_dev_init(dev, nullptr) : nullptr;
        if (b) {
            auto set_threads = (ggml_backend_set_n_threads_t) ggml_backend_reg_get_proc_address(ggml_backend_dev_backend_reg(dev), "ggml_backend_set_n_threads");
            if (set_threads) {
                set_threads(b, std::max(1, (int) std::thread::hardware_concurrency() - 1));
            }
        }
        return b;
    }();
    return cpu;
}

static bool rpc_tensor_on_host(const ggml_tensor * t) {
    if (t == nullptr) {
        return true;
    }
    const ggml_tensor * base = t->view_src ? t->view_src : t;
    ggml_backend_buffer_t buf = base->buffer;
    if (buf == nullptr || ggml_backend_buffer_is_host(buf)) {
        return true;
    }
    ggml_backend_dev_t dev = ggml_backend_buft_get_device(ggml_backend_buffer_get_type(buf));
    if (dev != nullptr && ggml_backend_dev_backend_reg(dev) == ggml_backend_metal_reg()) {
        return ggml_metal_buffer_is_shared(buf->context);
    }
    return false;
}

static bool rpc_graph_on_host(ggml_cgraph * graph) {
    for (int i = 0; i < ggml_graph_n_nodes(graph); i++) {
        ggml_tensor * t = ggml_graph_node(graph, i);
        if (!rpc_tensor_on_host(t)) {
            return false;
        }
        for (int j = 0; j < GGML_MAX_SRC; j++) {
            if (!rpc_tensor_on_host(t->src[j])) {
                return false;
            }
        }
    }
    return true;
}

static ggml_status rpc_compute_with_fallback(ggml_backend_t & backend, ggml_cgraph * graph) {
    if (!g_rpc_force_cpu.load()) {
        ggml_status st = ggml_backend_graph_compute(backend, graph);
        if (st == GGML_STATUS_SUCCESS) {
            g_rpc_last_on_cpu.store(0);
            return st;
        }
        GGML_LOG_WARN("[%s] GPU failed (status %d), retrying on CPU\n", __func__, (int) st);
        // Metal は失敗を覚えていて、作り直すまで失敗を返し続けるので、計算係だけ作り直す
        ggml_backend_dev_t dev = ggml_backend_get_device(backend);
        ggml_backend_t fresh = dev ? ggml_backend_dev_init(dev, nullptr) : nullptr;
        if (fresh != nullptr) {
            ggml_backend_free(backend);
            backend = fresh;
            GGML_LOG_INFO("[%s] recreated the GPU backend\n", __func__);
        }
    }
    ggml_backend_t cpu = rpc_cpu_backend();
    if (cpu == nullptr || !rpc_graph_on_host(graph)) {
        GGML_LOG_ERROR("[%s] some tensors are not readable from the CPU, cannot fall back\n", __func__);
        return GGML_STATUS_FAILED;
    }
    g_rpc_last_on_cpu.store(1);
    return ggml_backend_graph_compute(cpu, graph);
}

'''
anchor = "bool rpc_server::graph_compute(const std::vector<uint8_t> & input) {\n"
assert src.count(anchor) == 1, "graph_compute の場所が変わった"
src = src.replace(anchor, helpers + anchor)

old = (
    "    ggml_status status = ggml_backend_graph_compute(backends[device], graph);\n"
    '    GGML_ASSERT(status == GGML_STATUS_SUCCESS && "Unsuccessful graph computations are not supported with RPC");\n'
)
new = (
    "    ggml_status status = rpc_compute_with_fallback(backends[device], graph);\n"
    "    if (status != GGML_STATUS_SUCCESS) {\n"
    '        GGML_LOG_ERROR("[%s] graph compute failed (status %d), dropping this client\\n", __func__, (int) status);\n'
    "        return false;\n"
    "    }\n"
)
assert src.count(old) == 2, f"置きかえる場所が {src.count(old)} 個"
src = src.replace(old, new)
p.write_text(src, encoding="utf-8")
print("patched", p)
