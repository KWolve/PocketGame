# -*- coding: utf-8 -*-
"""独立验证"鼓垫 ↔ 掉落轨"是否对齐（不依赖游戏自己的命中测试）。

判据：对每条掉落轨 i，在鼓垫行里找**与该轨同色**（DRUM_COL[i]）的像素，
      它的 x 质心必须落在**该轨的列中心**附近（容差 8px）。

用法: python drum_align.py <shot.png>
      截图 = 真机 grab.py 抓的 480x800 全屏图（画布 top=160）。
"""
import sys
from PIL import Image

DRUM_COL = [(224, 90, 74), (232, 138, 60), (232, 200, 74),
            (92, 200, 110), (74, 168, 224), (176, 106, 224)]
LANE_W = 80
PAD_X0, PAD_DX, PAD_W = 3, 79, 74
PAD_Y0, PAD_H = 302, 112
CANVAS_TOP = 160
TOL = 26          # 色环是 4px 宽的描边，抗锯齿后颜色会偏一点

p = sys.argv[1]
im = Image.open(p).convert("RGB")
px = im.load()
y0, y1 = PAD_Y0 + CANVAS_TOP, PAD_Y0 + PAD_H + CANVAS_TOP

ok = True
for lane in range(6):
    c = DRUM_COL[lane]
    xs, n = [], 0
    for y in range(y0, y1):
        for x in range(480):
            r, g, b = px[x, y]
            if abs(r - c[0]) <= TOL and abs(g - c[1]) <= TOL and abs(b - c[2]) <= TOL:
                xs.append(x)
                n += 1
    want = LANE_W * lane + LANE_W // 2       # 该轨的列中心
    got = (sum(xs) / float(n)) if n else -1
    diff = abs(got - want) if n else 999
    flag = "✓" if (n > 60 and diff <= 8) else "✗"
    if flag == "✗":
        ok = False
    print("轨 %d  期望列中心 x=%3d  实测色环质心 x=%6.1f（%5d px）  偏差 %6.1f  %s"
          % (lane, want, got, n, diff, flag))
print("结论：%s" % ("6 个鼓垫都与自己的掉落轨对齐 ✓" if ok else
                  "**有鼓垫没对齐**（检查 pad_x0/pad_dx/lane_w）"))
sys.exit(0 if ok else 1)
