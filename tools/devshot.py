#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
devshot.py - 用数值方式「看」真机截图（AI 无法直接看图的替代手段）

用法:
  python tools/devshot.py <png> [--map] [--regions] [--scale N]

输出:
  - 关键点采样（像素值）
  - 1 字符 = N 像素 的分类图（能看出形状/布局/黑块）
  - 关注区域的墨迹统计（文字是否真的画出来了、暗带位置）
"""
import argparse
import os
import sys

from PIL import Image


def cls_char(rgb, palette):
    """把像素归到最近的调色板类（返回字符）"""
    best, bestd = '.', 1 << 30
    for ch, c in palette:
        d = sum((int(rgb[i]) - c[i]) ** 2 for i in range(3))
        if d < bestd:
            bestd = d
            best = ch
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("png")
    ap.add_argument("--map", action="store_true")
    ap.add_argument("--scale", type=int, default=8, help="1 字符 = N 像素")
    ap.add_argument("--regions", action="store_true")
    ap.add_argument("--points", default="")
    args = ap.parse_args()

    im = Image.open(args.png).convert("RGB")
    W, H = im.size
    px = im.load()
    print("image: %s  %dx%d" % (os.path.basename(args.png), W, H))

    palette = [
        ('.', (11, 15, 20)),     # 页面底色 #0B0F14
        ('-', (22, 32, 44)),     # 面板底 #16202C
        ('+', (37, 49, 63)),     # 选中/浅面板 #25313F
        ('W', (238, 242, 246)),  # 主文字 #EEF2F6
        ('g', (124, 141, 163)),  # 次文字 #7C8DA3
        ('a', (242, 179, 61)),   # 强调黄 #F2B33D
        ('b', (79, 195, 247)),   # 强调蓝 #4FC3F7
        ('r', (239, 83, 80)),    # 红
        ('G', (76, 175, 80)),    # 绿
        ('Y', (237, 194, 46)),   # 2048 黄
        ('x', (0, 0, 0)),        # 纯黑
    ]

    if args.points:
        for token in args.points.split(";"):
            if not token.strip():
                continue
            x, y = [int(v) for v in token.split(",")]
            if 0 <= x < W and 0 <= y < H:
                print("  (%3d,%3d) = #%02X%02X%02X" % ((x, y) + px[x, y]))
            else:
                print("  (%3d,%3d) 越界" % (x, y))

    if args.map:
        s = args.scale
        print("\n--- 分类图 (1 字符 = %dpx) ---  图例: .页面底 -面板 +浅面板 W亮字 g灰字 a黄 b蓝 r红 G绿 Y黄2048 x黑" % s)
        for y in range(0, H, s):
            row = []
            for x in range(0, W, s):
                # 取块内出现最多的"非底色"像素，避免文字被平均掉
                counts = {}
                for yy in range(y, min(y + s, H)):
                    for xx in range(x, min(x + s, W)):
                        c = cls_char(px[xx, yy], palette)
                        counts[c] = counts.get(c, 0) + 1
                # 非背景类优先（让文字/图形显现）
                cand = [k for k in counts if k not in ('.', '-', '+')]
                if cand:
                    ch = max(cand, key=lambda k: counts[k])
                else:
                    ch = max(counts, key=lambda k: counts[k])
                row.append(ch)
            print("  %3d %s" % (y, "".join(row)))

    if args.regions:
        print("\n--- 区域墨迹统计 ---")
        regions = [
            ("标题栏", 0, 0, W, 70),
            ("列表区", 8, 76, 472, 600),
            ("底部提示", 0, 680, W, 120),
        ]
        for name, x0, y0, x1, y1 in regions:
            n = 0
            dark = 0
            rows = {}
            for y in range(max(0, y0), min(H, y1)):
                rc = 0
                for x in range(max(0, x0), min(W, x1)):
                    c = cls_char(px[x, y], palette)
                    n += 1
                    if c in ('W', 'g', 'a', 'b', 'G', 'Y', 'r'):
                        dark += 1
                        rc += 1
                if rc:
                    rows[y] = rc
            print("  %-8s 采样 %6d px, 前景 %5d (%.2f%%)" % (name, n, dark, 100.0 * dark / max(n, 1)))
            if rows:
                ys = sorted(rows)
                # 连续暗行分段（用于看是否有换行/多段文字）
                segs = []
                start = ys[0]
                prev = ys[0]
                for y in ys[1:]:
                    if y - prev > 3:
                        segs.append((start, prev))
                        start = y
                    prev = y
                segs.append((start, prev))
                print("           前景行段: %s" % ", ".join("%d-%d" % s for s in segs[:12]))

    # 列表行色块位置（找非底色列的分布）
    print("\n--- 列表行 x 方向前景分布 (y=90..660) ---")
    colcount = {}
    for y in range(90, min(660, H)):
        for x in range(0, W):
            c = cls_char(px[x, y], palette)
            if c not in ('.',):
                colcount[x] = colcount.get(x, 0) + 1
    if colcount:
        xs = sorted(colcount)
        segs = []
        start = prev = xs[0]
        for x in xs[1:]:
            if x - prev > 4:
                segs.append((start, prev))
                start = x
            prev = x
        segs.append((start, prev))
        for s in segs[:16]:
            print("  x %3d-%3d  像素数 %d" % (s[0], s[1], sum(colcount.get(i, 0) for i in range(s[0], s[1] + 1))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
