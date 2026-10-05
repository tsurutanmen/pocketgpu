"""vklist.exe を本当に実行して、その出力をターミナル風の画像にする（手で書いた文字は使わない）。

  python snap_vklist.py <vkbuild のフォルダ> <出力の png>
"""
import os
import subprocess
import sys
from datetime import datetime

from PIL import Image, ImageDraw, ImageFont

build, out = sys.argv[1], sys.argv[2]
exe = os.path.join(build, "vklist.exe")
icd = os.path.join(build, "iphone_icd.json")


def run(extra_env):
    env = dict(os.environ, **extra_env)
    r = subprocess.run([exe], capture_output=True, env=env)
    return r.stdout.decode("utf-8", "replace").strip().splitlines()


lines = [("dim", f"# {datetime.now():%Y-%m-%d %H:%M}  Windows 11 / vulkan-1.dll"), ("dim", "")]
lines += [("cmd", "> vklist.exe"), *[("out", l) for l in run({})], ("dim", "")]
lines += [("cmd", f"> set VK_ADD_DRIVER_FILES={icd}"), ("cmd", "> vklist.exe")]
lines += [("hit" if "iPhone" in l else "out", l) for l in run({"VK_ADD_DRIVER_FILES": icd})]

mono = ImageFont.truetype("C:/Windows/Fonts/consola.ttf", 22)
jp = ImageFont.truetype("C:/Windows/Fonts/YuGothM.ttc", 22)
colors = {"dim": (120, 128, 140), "cmd": (130, 200, 255), "out": (225, 228, 232), "hit": (120, 230, 140)}
pad, lh = 28, 34
w = max(int(mono.getlength(t)) for _, t in lines) + pad * 2
img = Image.new("RGB", (max(w, 900), pad * 2 + lh * len(lines) + 40), (22, 24, 28))
d = ImageDraw.Draw(img)
d.rectangle([0, 0, img.width, 36], fill=(40, 43, 50))
for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]):
    d.ellipse([16 + i * 24, 11, 30 + i * 24, 25], fill=c)
for i, (k, t) in enumerate(lines):
    f = jp if any(ord(ch) > 127 for ch in t) else mono
    d.text((pad, 36 + pad + i * lh), t, font=f, fill=colors[k])
img.save(out)
print(out)
