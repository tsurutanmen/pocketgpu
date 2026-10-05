"""whisper-cli に、計算の受け手（ggml-rpc）を GPU として足す口をつける。

  WHISPER_RPC=127.0.0.1:50053 whisper-cli -m … -f …

ggml-rpc の装置は GPU の種類で登録されるので、whisper はそれを最初の GPU として使う。
"""
import pathlib
import sys

cli = pathlib.Path(sys.argv[1]) / "examples" / "cli" / "cli.cpp"
src = cli.read_text(encoding="utf-8")

anchor = "    ggml_backend_load_all();\n"
assert src.count(anchor) == 1, "ggml_backend_load_all の場所が変わった"
src = src.replace(anchor, anchor + """
    // add an RPC server (e.g. an iPhone) as a GPU device: WHISPER_RPC=host:port
    if (const char * rpc = getenv("WHISPER_RPC")) {
        ggml_backend_reg_t reg = ggml_backend_rpc_add_server(rpc);
        if (reg) {
            ggml_backend_register(reg);
        } else {
            fprintf(stderr, "WHISPER_RPC: cannot reach %s\\n", rpc);
        }
    }
""")

inc = '#include "whisper.h"\n'
assert inc in src
src = src.replace(inc, inc + '#include "ggml-backend.h"\n#include "ggml-rpc.h"\n', 1)

cli.write_text(src, encoding="utf-8")
print("patched", cli)
