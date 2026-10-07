# 2 つのフォルダの同じ名前の PPM を比べる：cmp.py 基準のフォルダ 比べるフォルダ [場面 ...]
# 違う画素（どれかの色が 2 より違う）の数・いちばん大きい差・基準で色のついた画素と比べる側で色のついた画素の数
import os, sys
from PIL import Image, ImageChops
a, b = sys.argv[1], sys.argv[2]
names = sys.argv[3:] or ["pidx", "pinst", "tri", "cut", "layer", "adj"]
for n in names:
    pa, pb = os.path.join(a, n + ".ppm"), os.path.join(b, n + ".ppm")
    if not (os.path.exists(pa) and os.path.exists(pb)):
        print(f"{n}: 絵が無い"); continue
    ia, ib = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
    if ia.size != ib.size:
        print(f"{n}: 大きさが違う {ia.size} {ib.size}"); continue
    pa_, pb_ = ia.load(), ib.load()
    w, h = ia.size
    diff = mx = ca = cb = 0
    for y in range(h):
        for x in range(w):
            p, q = pa_[x, y], pb_[x, y]
            d = max(abs(p[i] - q[i]) for i in range(3))
            if d > 2: diff += 1
            mx = max(mx, d)
            ca += max(p) > 60
            cb += max(q) > 60
    print(f"{n}: 違う画素 {diff} / {w*h}・最大の差 {mx}・色のついた画素 基準 {ca} 比べる側 {cb}")
    if diff:
        d = ImageChops.difference(ia, ib).point(lambda v: 255 if v > 2 else 0)
        d.save(os.path.join(b, n + "_diff.png"))
