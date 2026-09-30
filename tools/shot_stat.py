#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""shot_stat.py - 对抓屏 PNG 做客观统计（本项目不靠"肉眼看着像"）

★ 2026-09-23 改版：智能家居页从**列表**改成了**2x3 磁贴网格**（用户要求"不同类型做不同
  控制图片效果"，而 listview 的行图片不按行区分 ⇒ 只能每格独立控件）。
  所以这里也从"按行找墨迹"改成"按格取主色"，用来证明：

    ① 每格的图标底色 == 该格 `images/ha_tile_<域>_<on|off|flat>.png` 的底色
       （拿 tools/show_tiles.py 或直接读那张 PNG 的主色来对照）；
    ② 状态文字的颜色真的按状态分档（开=绿 / 关=灰 / 离线=橙 / 数值=青 / 在途=蓝）。

用法: python tools/shot_stat.py <png> [--tiles]

磁贴几何（与 ui/ha.html 严格对应，改布局要同步改这里）：
  格 (c,r) 左上角 = (12 + 232*c, 118 + 156*r)，格 224x148
  图标按钮 ≈ 格内 +16,+16 的 72x72
  状态文字 ≈ 格内 +96,+54 的 120x26
"""

import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("需要 PIL")
    sys.exit(2)

# ---- 磁贴几何（唯一真值；改 ui/ha.html 的 data-x/data-y 要同步改这三行）----
TILE_X0, TILE_Y0 = 12, 118
TILE_W, TILE_H, TILE_DX, TILE_DY = 224, 148, 232, 156
IC_X, IC_Y, IC_S = 16, 16, 72
ST_X, ST_Y, ST_W, ST_H = 96, 54, 120, 26
TILE_BG = (28, 28, 30)          # #1C1C1E 卡片底色（图标四角填的就是它）


def band(img, y0, y1, x0=0, x1=480, bg=(0, 0, 0), tol=24):
    n = 0
    for y in range(y0, min(y1, img.height)):
        for x in range(x0, min(x1, img.width)):
            p = img.getpixel((x, y))[:3]
            if abs(p[0] - bg[0]) > tol or abs(p[1] - bg[1]) > tol or abs(p[2] - bg[2]) > tol:
                n += 1
    return n


def dom_color(img, x0, y0, x1, y1, skip_bg=True):
    """区域内的主色（量化到 32 一档，避免抗锯齿带来的碎色）。"""
    c = Counter()
    for y in range(y0, min(y1, img.height)):
        for x in range(x0, min(x1, img.width)):
            p = img.getpixel((x, y))[:3]
            if skip_bg and max(abs(p[i] - TILE_BG[i]) for i in range(3)) <= 10:
                continue          # 卡片底色/图标四角，不算
            c[(p[0] // 24 * 24, p[1] // 24 * 24, p[2] // 24 * 24)] += 1
    return c


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return 2
    img = Image.open(args[0]).convert("RGB")
    print("分辨率: %dx%d" % (img.size))

    print("\n--- 各带非背景像素（bg=黑）---")
    for name, y0, y1 in [("导航栏 0..26", 0, 26), ("标题 26..52", 26, 52),
                         ("状态行 58..88", 58, 88), ("提示行 86..112", 86, 112),
                         ("磁贴区 118..578", 118, 578),
                         ("翻页 590..634", 590, 634), ("按钮 646..696", 646, 696),
                         ("页脚 708..734", 708, 734)]:
        print("  %-16s %6d" % (name, band(img, y0, y1)))

    print("\n--- 磁贴逐格主色（对照 images/ha_tile_<域>_<状态>.png）---")
    for r in range(3):
        for c in range(2):
            i = r * 2 + c
            tx = TILE_X0 + c * TILE_DX
            ty = TILE_Y0 + r * TILE_DY
            icon = dom_color(img, tx + IC_X + 4, ty + IC_Y + 4,
                             tx + IC_X + IC_S - 4, ty + IC_Y + IC_S - 4)
            st = dom_color(img, tx + ST_X, ty + ST_Y, tx + ST_X + ST_W, ty + ST_Y + ST_H)
            name_ink = band(img, ty + 20, ty + 50, tx + 96, tx + 216)
            ic_top = icon.most_common(1)
            print("  格%d (x=%3d,y=%3d) 图标主色=%-22s 状态字主色=%-22s 名字墨迹=%4d"
                  % (i, tx, ty, ic_top[0] if ic_top else "-",
                     st.most_common(1)[0] if st.most_common(1) else "-", name_ink))

    print("\n--- 底部三键主色（应依次是 绿/深灰/深灰）---")
    for name, x0, x1 in [("添加设备", 20, 150), ("刷新", 174, 304), ("管理模式", 328, 458)]:
        print("  %-8s %s" % (name, Counter(img.getpixel((x, 670))[:3]
                                          for x in range(x0, x1)).most_common(2)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
