"""USB の iPhone の読み上げ（AVSpeechSynthesizer）で、文を音声の WAV にする。アプリの 50057 番に送る。

  python apple_tts.py "こんにちは。今日はいい天気です。" -o out.wav
  python apple_tts.py --voices              # iPhone にある声の一覧（日本語だけ：--voices ja）
  python apple_tts.py "…" --voice 声のid --rate 0.5 -o out.wav
"""
import argparse
import base64
import json
import sys

from apple_ocr import call, connect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?")
    ap.add_argument("-o", "--out", default="tts.wav")
    ap.add_argument("--lang", default="ja-JP")
    ap.add_argument("--voice")
    ap.add_argument("--rate", type=float)
    ap.add_argument("--voices", nargs="?", const="")
    ap.add_argument("--addr")
    a = ap.parse_args()
    with connect(a.addr) as s:
        if a.voices is not None:
            for v in call(s, {"op": "voices"})["voices"]:
                if v["lang"].startswith(a.voices):
                    print(v["lang"], v["quality"], v["name"], v["id"])
            return
        req = {"op": "tts", "text": a.text or "", "lang": a.lang}
        if a.voice:
            req["voice"] = a.voice
        if a.rate is not None:
            req["rate"] = a.rate
        res = call(s, req)
    if "error" in res:
        sys.exit(res["error"])
    data = base64.b64decode(res["wav_b64"])
    open(a.out, "wb").write(data)
    print(f"{a.out}：{len(data)} バイト・iPhone の中で {res['ms']} ms・声 {res['voice']}")


if __name__ == "__main__":
    main()
