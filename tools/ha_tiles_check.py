#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ha_tiles_check.py - 磁贴"渲染 vs 素材"逐格交叉判据

为什么需要它：抓屏能证明"屏幕上有颜色"，QA dump 能证明"代码认为该显示哪张图"，
但**两者是否一致**（比如图片没部署 / 路径写错 / setBackgroundPic 没生效 ⇒ 画出一张旧图
或纯白块）只有交叉比对才能发现。这正是本工程最怕的"看起来对"的静默失败。

做法：
  1. 从设备 pull 下来的 QA dump（/tmp/pg_ha_list.txt）里读每格的 pic 名；
  2. 抓屏（shot.png）上按磁贴几何取该格**图标区**的主色；
  3. 打开 `resources/images/<pic 名>`，同样取主色；
  4. 逐格比对（量化后容差 24）。

用法：
  adb -s <dev> pull /tmp/pg_ha_list.txt .
  python tools/grab.py shot.png
  python tools/ha_tiles_check.py shot.png pg_ha_list.txt

磁贴几何必须与 ui/ha.html 一致（改布局要同步改这里的常量）。
"""

import os
import re
import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:
    print("需要 PIL")
    sys.exit(2)

# ---- 与 ui/ha.html / tools/shot_stat.py 同一套几何 ----
TILE_X0, TILE_Y0 = 12, 118
TILE_DX, TILE_DY = 232, 156
IC_X, IC_Y, IC_S = 16, 16, 72
CARD_BG = (28, 28, 30)
TOL = 24


def dom(img, x0, y0, x1, y1):
    c = Counter()
    for y in range(y0, min(y1, img.height)):
        for x in range(x0, min(x1, img.width)):
            p = img.getpixel((x, y))[:3]
            if max(abs(p[i] - CARD_BG[i]) for i in range(3)) <= 10:
                continue
            c[(p[0] // 24 * 24, p[1] // 24 * 24, p[2] // 24 * 24)] += 1
    return c


def near(a, b, tol=TOL):
    return max(abs(a[i] - b[i]) for i in range(3)) <= tol


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    shot = Image.open(sys.argv[1]).convert("RGB")
    dump_path = sys.argv[2]
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    imgdir = os.path.join(root, "resources", "images")

    # 解析 dump 里的每格 pic
    pics = {}
    pat = re.compile(r"\[\s*(\d)\] pic=(\S+)")
    with open(dump_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                pics[int(m.group(1))] = m.group(2)

    if not pics:
        print("没从 %s 里解析到磁贴行（格式变了吗？）" % dump_path)
        return 2

    print("逐格比对：抓屏颜色 vs 素材颜色")
    bad = 0
    for i in range(6):
        r, c = divmod(i, 2)
        tx = TILE_X0 + c * TILE_DX
        ty = TILE_Y0 + r * TILE_DY
        ren = dom(shot, tx + IC_X + 4, ty + IC_Y + 4,
                  tx + IC_X + IC_S - 4, ty + IC_Y + IC_S - 4)
        pic = pics.get(i, "")
        if not pic:
            print("  格%d: dump 里没有 pic（脚本可能没跑？）" % i)
            bad += 1
            continue
        p = os.path.join(imgdir, os.path.basename(pic))
        if not os.path.exists(p):
            print("  格%d: 素材不存在 %s" % (i, p))
            bad += 1
            continue
        src = dom(Image.open(p).convert("RGB"), 0, 0, 72, 72)
        if not ren or not src:
            print("  格%d: 取不到主色（渲染=%s 素材=%s）" % (i, bool(ren), bool(src)))
            bad += 1
            continue
        rc, sc = ren.most_common(1)[0][0], src.most_common(1)[0][0]
        ok = near(rc, sc)
        if not ok:
            bad += 1
        print("  格%d %-34s 渲染=%-18s 素材=%-18s %s"
              % (i, os.path.basename(pic), rc, sc, "✓" if ok else "✗ 不一致"))

    print("\n结论：%s（%d 格）" % ("全部一致 ✓" if bad == 0 else "有 %d 格不一致 ✗" % bad, 6))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
