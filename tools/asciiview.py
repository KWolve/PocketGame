#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
asciiview.py - 把 PNG 降采样成 ASCII 字符画（"看不了图也能验画面"的通用手段）

为什么要有它：验收经常在没有图形界面的环境里做（远程会话 / 纯文本工具链）。
把图降采样成字符画后，**布局、明暗、元素位置、有没有铺满**一眼就能判，
而且判据可以写进脚本（摇骰子"顶面点数 == 面值"就是这么在真机核对的）。

用法:
  python tools/asciiview.py shot.png                # 亮度字符画（默认 96 列）
  python tools/asciiview.py icon.png 48 --alpha     # 按 alpha 出画（看图标外形/透明区）
  python tools/asciiview.py shot.png 96 --region 0 60 240 400   # 只看某一块（x y w h）

字符表: " .:-=+*#%@" —— 由暗到亮。前三档（空格/./:）适合看深色底，
浅底截图建议直接用默认（浅色会打成 # / % / @）。
"""
import os
import sys

from PIL import Image

RAMP = " .:-=+*#%@"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else 96
    use_alpha = "--alpha" in sys.argv

    region = None
    if "--region" in sys.argv:
        i = sys.argv.index("--region")
        region = [int(v) for v in sys.argv[i + 1:i + 5]]

    if not os.path.isfile(path):
        print("!! 找不到文件: %s" % path)
        return 1

    im = Image.open(path).convert("RGBA")
    if region:
        im = im.crop((region[0], region[1], region[0] + region[2], region[1] + region[3]))

    w, h = im.size
    # 字符本身是"高瘦"的，按 0.5 压缩行数，画出来比例才不失真
    rows = max(1, int(cols * h / float(w) * 0.5))
    im = im.resize((cols, rows), Image.BOX)
    px = im.load()

    print("== %s  %dx%d -> %d cols x %d rows%s" %
          (os.path.basename(path), w, h, cols, rows, "  [alpha]" if use_alpha else ""))
    for y in range(rows):
        line = []
        for x in range(cols):
            r, g, b, a = px[x, y]
            if use_alpha:
                v = a
            else:
                # 透明像素按"纸白"处理，否则 PNG 的透明区会变成黑块
                if a == 0:
                    v = 255
                else:
                    v = (r * 299 + g * 587 + b * 114) // 1000
            idx = v * (len(RAMP) - 1) // 255
            line.append(RAMP[idx])
        print("".join(line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
