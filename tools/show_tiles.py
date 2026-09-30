#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
show_tiles.py - 把 ha_tile_*.png 降采样成**字符画**打印出来

用途：本机（或当前会话）**读不了图片**时，仍然能肉眼核对图形画得对不对。
  · 图形是"形状"问题 ⇒ 用亮度映射成 ASCII 就够看清轮廓；
  · 底板颜色/状态编码是"数值"问题 ⇒ 顺便打印每个状态的平均亮度。

用法：
  python tools/show_tiles.py                 # 全部域 × on 态
  python tools/show_tiles.py light switch    # 只看某几个域
  python tools/show_tiles.py --state off light
"""

import os
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG_DIR = os.path.join(ROOT, "resources", "images")

RAMP = " .:-=+*#%@"      # 由暗到亮


def ascii_of(path, cols=26):
    img = Image.open(path).convert("L")
    w, h = img.size
    rows = max(1, int(cols * h / float(w) * 0.5))     # 0.5：字符比像素高
    small = img.resize((cols, rows), Image.BOX)
    px = small.load()
    out = []
    for y in range(rows):
        line = ""
        for x in range(cols):
            v = px[x, y]
            line += RAMP[min(len(RAMP) - 1, v * len(RAMP) // 256)]
        out.append(line)
    return out, img


def main():
    args = [a for a in sys.argv[1:]]
    state = "on"
    if "--state" in args:
        i = args.index("--state")
        state = args[i + 1]
        del args[i:i + 2]
    names = args or ["light", "switch", "sensor", "binary_sensor", "scene", "button",
                     "climate", "cover", "fan", "lock", "camera", "media_player", "other"]

    for n in names:
        p = os.path.join(IMG_DIR, "ha_tile_%s_%s.png" % (n, state))
        if not os.path.exists(p):
            print("!(缺) %s" % p)
            continue
        art, img = ascii_of(p)
        px = list(img.getdata())
        luma = sum(px) / float(len(px))
        print("=== %-14s (%s) 平均亮度 %.1f ===" % (n, state, luma))
        for line in art:
            print("   |" + line + "|")
        print()


if __name__ == "__main__":
    main()
