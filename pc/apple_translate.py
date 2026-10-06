"""USB の iPhone の翻訳（iOS 26 の Translation）で文を訳す。アプリの 50057 番に送る。
使う言語は iPhone に入っている必要がある（設定 → アプリ → 翻訳 → ダウンロード済みの言語）。

  python apple_translate.py "今日はいい天気です。" --from ja --to en
"""
import argparse
import sys

from apple_ocr import call, connect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--from", dest="src", default="ja")
    ap.add_argument("--to", default="en")
    ap.add_argument("--addr")
    a = ap.parse_args()
    with connect(a.addr) as s:
        res = call(s, {"op": "translate", "text": a.text, "from": a.src, "to": a.to})
    if "error" in res:
        sys.exit(res["error"])
    print(res["text"])
    print(f"iPhone の中で {res.get('ms')} ms", file=sys.stderr)


if __name__ == "__main__":
    main()
