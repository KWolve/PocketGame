#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pet3d/preview.py - 把带 alpha 的渲染图"合成到设备底色上"再用字符画看

为什么需要它：工程自带的 asciiview.py 把透明像素当**纸白**，看带 alpha 的 3D 出图
就像"白纸上一个亮块"，读不出明暗与配色。这里改成：
  · 先按给定底色（默认 #1C1C1E，与设备页面底一致）做 alpha 合成
  · --mode gray  出亮度字符画（判曝光/明暗分布）
  · --mode hue   出**色相字母画**（R/G/B/C/M/Y/W/K），判配色是否如设计
用法:
  python preview.py out/hero.png 76
  python preview.py out/hero.png 76 --mode hue
"""
import os
import sys

from PIL import Image

GRAY = " .:-=+*#%@"
BG = (28, 28, 30)


def hue_char(r, g, b):
    mx, mn = max(r, g, b), min(r, g, b)
    if mx < 46:
        return "."
    if mx - mn < 22:
        return "W" if mx > 190 else ("w" if mx > 110 else "k")
    if r >= g and r >= b:
        return "R" if g < b + 30 and b < g + 60 else ("M" if b > g else "Y")
    if g >= r and g >= b:
        return "C" if b > r + 30 else "G"
    return "B" if r < g else "M"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 76
    mode = "gray"
    if "--mode" in sys.argv:
        mode = sys.argv[sys.argv.index("--mode") + 1]
    bg = BG
    if "--bg" in sys.argv:
        h = sys.argv[sys.argv.index("--bg") + 1].lstrip("#")
        bg = tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

    im = Image.open(path).convert("RGBA")
    base = Image.new("RGBA", im.size, bg + (255,))
    im = Image.alpha_composite(base, im).convert("RGB")

    w, h = im.size
    rows = max(1, int(cols * h / float(w) * 0.5))
    im = im.resize((cols, rows), Image.BOX)
    px = im.load()

    print("== %s  %dx%d -> %d x %d  [%s]" % (os.path.basename(path), w, h, cols, rows, mode))
    for y in range(rows):
        out = []
        for x in range(cols):
            r, g, b = px[x, y]
            if mode == "hue":
                out.append(hue_char(r, g, b))
            else:
                v = (r * 299 + g * 587 + b * 114) // 1000
                out.append(GRAY[v * (len(GRAY) - 1) // 255])
        print("".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
