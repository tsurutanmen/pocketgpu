# 作りどうしを比べる：cmp_all.py 基準の作り 比べる作り [場面 ...]
# 絵：どれかの色が 2 より違う画素の数といちばん大きい差。値：「値」の行を語ごとに比べる（数は 0.006 か 0.1% まで同じと見る）
# 違いの絵は out/<比べる作り>/<場面>_diff_<基準>.png に出す
import os, re, sys
from PIL import Image, ImageChops

F = os.path.dirname(os.path.abspath(__file__))
base, other = sys.argv[1], sys.argv[2]
OUT = os.path.join(F, "out")


def scenes():
    s = sys.argv[3:]
    if s: return s
    return [l.split("\t")[0] for l in open(os.path.join(OUT, base, "summary.tsv"), encoding="utf-8")]


def rc_of(mode):
    r = {}
    p = os.path.join(OUT, mode, "summary.tsv")
    if os.path.exists(p):
        for l in open(p, encoding="utf-8"):
            a = l.rstrip("\n").split("\t")
            r[a[0]] = a[1]
    return r


num = re.compile(r"^-?\d+(\.\d+)?$")


def same_tok(a, b):
    if a == b: return True
    if num.match(a) and num.match(b) and ("." in a or "." in b):
        x, y = float(a), float(b)
        return abs(x - y) <= max(0.006, 1e-3 * max(abs(x), abs(y)))
    return False


def vals(mode, s):
    p = os.path.join(OUT, mode, s + ".txt")
    if not os.path.exists(p): return None, []
    t = open(p, encoding="utf-8", errors="replace").read().splitlines()
    fail = [l for l in t if l.startswith("失敗")]
    return fail, [l for l in t if l.startswith("値 ")]


def img_cmp(s):
    pa, pb = os.path.join(OUT, base, s + ".ppm"), os.path.join(OUT, other, s + ".ppm")
    if not os.path.exists(pb): return "絵なし", None
    if not os.path.exists(pa): return "基準の絵なし", None
    ia, ib = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
    if ia.size != ib.size: return f"大きさ違い {ia.size}→{ib.size}", None
    d = ImageChops.difference(ia, ib)
    px = list(d.getdata())
    n = sum(1 for p in px if max(p) > 2)
    mx = max((max(p) for p in px), default=0)
    if n:
        d.point(lambda v: 255 if v > 2 else 0).save(os.path.join(OUT, other, f"{s}_diff_{base}.png"))
    return n, mx


rb, ro = rc_of(base), rc_of(other)
for s in scenes():
    fo, vo = vals(other, s)
    fb, vb = vals(base, s)
    if ro.get(s) != "0" or fo:
        print(f"{s:10s} 動かない rc={ro.get(s)} {(fo or [''])[0][:150]}")
        continue
    n, mx = img_cmp(s)
    dv = []
    for i in range(max(len(vb), len(vo))):
        a = vb[i] if i < len(vb) else "（無い）"
        b = vo[i] if i < len(vo) else "（無い）"
        ta, tb = a.split(), b.split()
        if len(ta) != len(tb) or not all(same_tok(x, y) for x, y in zip(ta, tb)): dv.append((a, b))
    img = f"絵 {n}" + (f" 画素（最大の差 {mx}）" if isinstance(n, int) and n else "")
    tag = "同じ" if (n == 0 or n is None or n == "基準の絵なし") and not dv else "違う"
    print(f"{s:10s} {tag}  {img}・値の違い {len(dv)} 行")
    for a, b in dv[:12]:
        print(f"      基準: {a[2:]}\n      比較: {b[2:]}")
