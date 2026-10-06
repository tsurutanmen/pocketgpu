"""絵を比べる。1 枚目を基準に、2 枚目以降がどれだけ違うかを出す。
  python imgdiff.py 基準.bmp 比べる.bmp ...
平均の差（0〜255）・8 より大きく違う画素の割合・PSNR を出す。大きさが違えば、そう書く。Pillow が要る
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageStat


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    base_path = sys.argv[1]
    if not os.path.exists(base_path):
        print(f'基準の絵が無い：{os.path.basename(base_path)}')
        return 1
    base = Image.open(base_path).convert('RGB')
    for p in sys.argv[2:]:
        name = os.path.basename(p)
        if not os.path.exists(p):
            print(f'{name}：絵が無い')
            continue
        img = Image.open(p).convert('RGB')
        if img.size != base.size:
            print(f'{name}：大きさが違う {img.size} と {base.size}')
            continue
        d = ImageChops.difference(base, img)
        mean = sum(ImageStat.Stat(d).mean) / 3
        g = d.convert('L')
        hist = g.histogram()
        big = sum(hist[9:]) / (base.size[0] * base.size[1])
        mse = sum(v * v for v in ImageStat.Stat(d).rms) / 3
        psnr = float('inf') if mse == 0 else 10 * math.log10(255 * 255 / mse)
        print(f'{name}：平均の差 {mean:.2f}・大きく違う画素 {big * 100:.2f}%・PSNR {psnr:.1f}dB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
