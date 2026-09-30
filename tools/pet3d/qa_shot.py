#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pet3d/qa_shot.py - 渲染结果的**客观判据**（不靠人看原图）

对每张 PNG 输出：
  · 尺寸 / alpha 覆盖率 / 内容包围盒（判"有没有出画、留白合不合理"）
  · 主体的亮度分位（p05/p50/p95）—— 判"有没有过曝/发黑"
  · 高光占比（>=250 的像素比例）与死黑占比 —— 判曝光是否失衡
  · 主体平均饱和度 —— 判"是不是一片灰"
用法: python qa_shot.py out/*.png
"""
import glob
import os
import sys

from PIL import Image, ImageStat


def analyze(path):
    im = Image.open(path).convert("RGBA")
    W, H = im.size
    px = im.load()

    a_vals = []
    lum = []
    sat = []
    xs, ys = [], []
    hi = lo = 0
    soft = 0
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a <= 16:
                continue
            a_vals.append(a)
            xs.append(x); ys.append(y)
            if a < 200:
                soft += 1
                continue
            L = (r * 299 + g * 587 + b * 114) // 1000
            lum.append(L)
            mx, mn = max(r, g, b), min(r, g, b)
            sat.append(0 if mx == 0 else (mx - mn) * 100 // mx)
            if L >= 250:
                hi += 1
            if L <= 6:
                lo += 1
    n = len(lum)
    if n == 0:
        return f"{os.path.basename(path):22s} {W}x{H}  !! 全透明（没渲染出东西）"
    lum.sort()
    sat.sort()
    p = lambda arr, q: arr[min(len(arr) - 1, int(len(arr) * q))]
    bbox = (min(xs), min(ys), max(xs), max(ys))
    fill = n * 100.0 / (W * H)
    hi235 = sum(1 for v in lum if v >= 235) * 100.0 / n
    lo60 = sum(1 for v in lum if v < 60) * 100.0 / n
    mids = [v for v in lum if 60 <= v < 235]
    return (
        f"{os.path.basename(path):22s} {W:4d}x{H:<4d} "
        f"实心覆盖 {n*100.0/(W*H):5.1f}%  半透明(阴影) {soft*100.0/(W*H):4.1f}%  "
        f"bbox=({bbox[0]:3d},{bbox[1]:3d})-({bbox[2]:3d},{bbox[3]:3d})  "
        f"亮度 p05={p(lum,0.05):3d} p50={p(lum,0.5):3d} p95={p(lum,0.95):3d}  "
        f"亮部≥235 {hi235:4.1f}%  暗部<60 {lo60:4.1f}%  "
        f"中间调中位 {(p(mids,0.5) if mids else 0):3d}  饱和中位 {p(sat,0.5):3d}"
    )


def main():
    args = sys.argv[1:]
    files = []
    for a in args:
        files.extend(sorted(glob.glob(a)))
    if not files:
        print(__doc__)
        return 1
    for f in files:
        print(analyze(f))
    return 0


if __name__ == "__main__":
    sys.exit(main())
