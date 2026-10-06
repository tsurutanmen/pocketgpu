"""USB の iPhone の文字認識（Vision）で、画像の文字を読む。アプリの 50057 番（Sources/AppleFeatures.swift）に送る。

  python apple_ocr.py 画像.png               # 読んだ文字を出す
  python apple_ocr.py 画像.png --json        # 行ごとの位置と確からしさも出す
  python apple_ocr.py --hello                 # iPhone ができることを聞く
  python apple_ocr.py --make-test test.png    # 日本語の試しの画像を作る（PC の字体で描く）
  python apple_ocr.py 画像.png --addr 127.0.0.1:50057   # USB を使わずに番地へ直接
"""
import argparse
import json
import socket
import struct
import sys
import time

import usb_forward as uf


def frame(b):
    return struct.pack("<I", len(b)) + b


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        c = s.recv(n - len(out))
        if not c:
            raise ConnectionError("途中で切れた")
        out += c
    return out


def call(s, req, body=b""):
    s.sendall(frame(json.dumps(req).encode()) + frame(body))
    n = struct.unpack("<I", recv_exact(s, 4))[0]
    return json.loads(recv_exact(s, n))


def connect(addr):
    if addr:
        h, p = addr.rsplit(":", 1)
        return socket.create_connection((h, int(p)))
    return uf.connect_device(uf.usb_device_id(), 50057)


def make_test(path):
    from PIL import Image, ImageDraw, ImageFont
    lines = [
        "iPhone の文字認識を Windows から使う試し",
        "日本の四季：春は桜、夏は花火、秋は紅葉、冬は雪。",
        "2026年10月6日（火）20:58　USB で 90MB/s",
        "The quick brown fox jumps over the lazy dog.",
    ]
    font = ImageFont.truetype("C:/Windows/Fonts/meiryo.ttc", 40)
    img = Image.new("RGB", (1400, 80 + 70 * len(lines)), "white")
    d = ImageDraw.Draw(img)
    for i, t in enumerate(lines):
        d.text((40, 40 + 70 * i), t, fill="black", font=font)
    img.save(path)
    print("\n".join(lines))
    print(f"→ {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", nargs="?")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--hello", action="store_true")
    ap.add_argument("--fast", action="store_true", help="速さ優先の読み方")
    ap.add_argument("--langs", default="ja-JP,en-US")
    ap.add_argument("--addr")
    ap.add_argument("--make-test")
    a = ap.parse_args()
    if a.make_test:
        make_test(a.make_test)
        return
    with connect(a.addr) as s:
        if a.hello or not a.image:
            print(json.dumps(call(s, {"op": "hello"}), ensure_ascii=False))
            if not a.image:
                return
        data = open(a.image, "rb").read()
        t0 = time.perf_counter()
        res = call(s, {"op": "ocr", "langs": a.langs.split(","), "fast": a.fast}, data)
        dt = (time.perf_counter() - t0) * 1000
    if "error" in res:
        sys.exit(res["error"])
    if a.json:
        print(json.dumps(res, ensure_ascii=False, indent=1))
    else:
        print(res["text"])
    print(f"iPhone の中で {res['ms']} ms・送って返るまで {dt:.0f} ms・{res['width']}x{res['height']}", file=sys.stderr)


if __name__ == "__main__":
    main()
