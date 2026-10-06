"""USB の iPhone の Apple Intelligence（Foundation Models の言葉のモデル）に、Windows から頼む。アプリの 50057 番に送る。
アプリは Xcode 26 で組んだ版が要る（Xcode 16 で組んだ版は「入っていない」と答える）。

  python apple_llm.py "日本の四季を三つの文で説明して"
  python apple_llm.py --status                     # 使えるか・使えない理由
  python apple_llm.py "…" --instructions "あなたは高校の先生です" --temperature 0.3 --max-tokens 300
"""
import argparse
import json
import sys
import time

from apple_ocr import call, connect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prompt", nargs="?")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--instructions")
    ap.add_argument("--temperature", type=float)
    ap.add_argument("--max-tokens", type=int)
    ap.add_argument("--addr")
    a = ap.parse_args()
    with connect(a.addr) as s:
        if a.status or not a.prompt:
            print(json.dumps(call(s, {"op": "llm_status"}), ensure_ascii=False))
            if not a.prompt:
                return
        req = {"op": "llm", "prompt": a.prompt}
        if a.instructions:
            req["instructions"] = a.instructions
        if a.temperature is not None:
            req["temperature"] = a.temperature
        if a.max_tokens:
            req["max_tokens"] = a.max_tokens
        t0 = time.perf_counter()
        res = call(s, req)
        dt = (time.perf_counter() - t0) * 1000
    if "error" in res:
        sys.exit(res["error"])
    print(res["text"])
    print(f"iPhone の中で {res['ms']} ms・送って返るまで {dt:.0f} ms", file=sys.stderr)


if __name__ == "__main__":
    main()
