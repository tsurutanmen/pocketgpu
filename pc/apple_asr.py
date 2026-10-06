"""USB の iPhone の音声認識（SFSpeechRecognizer）で、音声のファイルを文字にする。アプリの 50057 番に送る。
初めてのときは iPhone の画面に「音声認識を許可」が出るので、押してもらう。

  python apple_asr.py 音声.wav                       # 日本語で文字にする
  python apple_asr.py 音声.wav --lang en-US
  python apple_asr.py 音声.wav --compare 基準.txt    # 基準の文（whisper など）と、何文字違うかを出す
  python apple_asr.py 音声.wav --engine legacy       # 古い SFSpeechRecognizer で（--server で Apple のサーバーも使ってよい）
"""
import argparse
import difflib
import os
import sys
import time

from apple_ocr import call, connect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--lang", default="ja-JP")
    ap.add_argument("--server", action="store_true")
    ap.add_argument("--engine", default="new", help="new：iOS 26 の SpeechAnalyzer（標準）・legacy：SFSpeechRecognizer")
    ap.add_argument("--compare")
    ap.add_argument("--addr")
    a = ap.parse_args()
    data = open(a.audio, "rb").read()
    ext = os.path.splitext(a.audio)[1].lstrip(".") or "wav"
    with connect(a.addr) as s:
        t0 = time.perf_counter()
        res = call(s, {"op": "asr", "lang": a.lang, "ext": ext, "on_device": not a.server, "engine": a.engine}, data)
        dt = (time.perf_counter() - t0) * 1000
    if "error" in res:
        sys.exit(res["error"])
    print(res["text"])
    print(f"iPhone の中で {res['ms']} ms・送って返るまで {dt:.0f} ms・{res.get('engine')}・区切り {res.get('segments', '-')}", file=sys.stderr)
    if a.compare:
        ref = "".join(open(a.compare, encoding="utf-8").read().split())
        got = "".join(res["text"].split())
        r = difflib.SequenceMatcher(None, ref, got).ratio()
        print(f"基準と比べて：一致率 {r * 100:.1f}%・基準 {len(ref)} 文字・iPhone {len(got)} 文字", file=sys.stderr)


if __name__ == "__main__":
    main()
