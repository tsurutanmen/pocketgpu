"""GPU の計算が失敗しても、受け手のアプリを落とさない。

iPhone では、アプリが後ろに回ると GPU を使えなくなり、Metal の計算が失敗する。
元の ggml-rpc はそこで abort するので、その接続だけ切って次の依頼を待つようにする。
"""
import pathlib
import sys

p = pathlib.Path(sys.argv[1]) / "ggml" / "src" / "ggml-rpc" / "ggml-rpc.cpp"
src = p.read_text(encoding="utf-8")
old = '    GGML_ASSERT(status == GGML_STATUS_SUCCESS && "Unsuccessful graph computations are not supported with RPC");\n'
new = (
    "    if (status != GGML_STATUS_SUCCESS) {\n"
    '        GGML_LOG_ERROR("[%s] graph compute failed (status %d), dropping this client\\n", __func__, (int) status);\n'
    "        return false;\n"
    "    }\n"
)
assert src.count(old) == 2, f"置きかえる場所が {src.count(old)} 個"
p.write_text(src.replace(old, new), encoding="utf-8")
print("patched", p)
