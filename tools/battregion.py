#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""battregion.py - 把真机截图里"电池图标"那一小块做 1:1 分类渲染（自检用）

用法: python tools/battregion.py <png> [x0 x1 y0 y1]
默认区域 = 主界面电池图标 (282..345, 5..40)。
图例: .=页面底 S=外壳灰 A=琥珀(充电/充满) B=蓝(正常放电) R=红(低电) W=闪电(浅色) ?=其它
"""
import sys
from collections import Counter

from PIL import Image

PAGE, SHELL = (11, 15, 20), (143, 160, 181)
AMBER, BLUE, RED, BOLT = (242, 179, 61), (92, 168, 226), (228, 87, 76), (238, 242, 246)


def cls(p):
    r, g, b = p
    if abs(r - PAGE[0]) < 14 and abs(g - PAGE[1]) < 14 and abs(b - PAGE[2]) < 16:
        return "."
    if abs(r - SHELL[0]) < 30 and abs(g - SHELL[1]) < 30 and abs(b - SHELL[2]) < 30:
        return "S"
    if abs(r - AMBER[0]) < 26 and abs(g - AMBER[1]) < 30 and abs(b - AMBER[2]) < 40:
        return "A"
    if abs(r - BLUE[0]) < 34 and abs(g - BLUE[1]) < 34 and abs(b - BLUE[2]) < 34:
        return "B"
    if abs(r - RED[0]) < 30 and abs(g - RED[1]) < 30 and abs(b - RED[2]) < 30:
        return "R"
    if r > 200 and g > 210 and b > 215:
        return "W"
    return "?"


def main():
    png = sys.argv[1]
    x0, x1, y0, y1 = (int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (282, 345, 5, 40)
    im = Image.open(png).convert("RGB")
    px = im.load()
    print("=== 电池区域 1:1（x=%d..%d, y=%d..%d）%s ===" % (x0, x1 - 1, y0, y1 - 1, png))
    for y in range(y0, y1):
        print("%3d|%s|" % (y, "".join(cls(px[x, y]) for x in range(x0, x1))))
    c = Counter(cls(px[x, y]) for y in range(y0, y1) for x in range(x0, x1))
    # 电量条宽度：数"有色的填充像素"最多的一行
    best = (0, "")
    for y in range(y0, y1):
        row = "".join(cls(px[x, y]) for x in range(x0, x1))
        n = sum(1 for ch in row if ch in "ABRW")
        if n > best[0]:
            best = (n, row)
    kind = {"A": "琥珀(充电/已充满)", "B": "蓝(未充电)", "R": "红(低电)", "": "空", "W": "?"}
    fill = ""
    for ch in best[1]:
        if ch in "ABRW":
            fill = ch
            break
    print("统计:", dict(c))
    print("电量条最长一行 = %d px（满格 40）→ 约 %d%%   颜色=%s   闪电=%s"
          % (best[0], round(best[0] * 100.0 / 40), kind.get(fill, fill),
             "有" if c.get("W", 0) > 20 else "无"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
