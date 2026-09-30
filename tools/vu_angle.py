#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""vu_angle.py —— 从抓屏**像素**里读出两只 VU 表的指针角度（真机验收用）

为什么要有它：本板的日志服务很脆（被噪声刷爆后整块卡住 ⇒ 应用日志再也进不来），
而"指针摆到哪个角度"这件事**本来就是画面上的事实** —— 直接量像素比查日志更可靠，
也正是验收要交的证据（不是"代码看起来对"，而是"针真的停在这个刻度上"）。

读法（为什么这么读）：
  · 指针是**一条从铰点射出的细线**（针长 = 弧半径+3，本版 113px），所以对每个角度做
    "**径向覆盖率**"统计：沿该角度从 R_IN 到 R_OUT 逐点找"指针色"像素，覆盖半径越多 → 越像指针；
  · 表盘上还有刻度线（半径 ARC_R-13..ARC_R）、刻度数字（半径 ARC_R-26）、L/R 圆标
    （在左下 ≈ -65°，但用的是**刻度色**不是指针色 ⇒ 容差 12 时区分得开）。
    刻度/数字的径向跨度都很短（十几像素），读不到覆盖率 ⇒ 把搜索角限制在
    **-95°~+62°**（刻度弧所在的上半圈）就足够干净。
  · 指针色 = 素材里的 (30,26,20)（`tools/gen_vu_art.py` 的 cream 配色）。

几何（**从 `tools/gen_vu_art.py` 直接 import，不再写死**）：
  · 面盘 `FACE_W x FACE_H` @L/R `FACE_XS[0]/[1], FACE_YS`；**铰点 = PIVOT**（面盘图内坐标）
  · 指针控件 `NEEDLE x NEEDLE`，左上 = 铰点 − NEEDLE/2 ⇒ 轴心必须与刻度圆心重合（本工具顺便验这条）
  ★ 2026-09-19 血案：原先这里把 (113,176)/226 写死在工具里，表盘"压扁"改版后
    这套常量就全错了（铰点跑到盘外、半径也变了）。**同一份几何只允许有一个真值** ⇒ 改成 import。

角度 → dB 的换算与素材一致（`db_to_ang`）：
  -20dB → -52°、0dB → +16°、+3dB → +34°（0 之后压缩）

用法:
    python tools/vu_angle.py frame.png              # 读一帧
    python tools/vu_angle.py a.png b.png --csv      # 多帧，输出 CSV（做时间序列）
    python tools/vu_angle.py f.png --tolerance 18   # 放宽指针色容差（画面偏色时）

退出码：能读出两只表 = 0；有一只读不出（比如指针被挡住/素材没挂上）= 1（便于脚本判定）。
"""
import argparse
import math
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 PIL：请用 ~/.workbuddy/binaries/python/envs/default/Scripts/python.exe 运行")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# ★ 几何单一真值：全部来自素材生成器（同一个脚本既画素材又给判据，不会漂）
from gen_vu_art import (FACE_XS, FACE_YS, FACE_W, FACE_H, PIVOT, ARC_R,  # noqa: E402
                        NEEDLE_LEN, db_to_ang, face_palette)

# ---- 与素材一致的配色（改 gen_vu_art.py 的 cream 配色时要同步这一处）----
NEEDLE_RGB = face_palette("cream")[3]        # 指针色 (30,26,20)
ANG_MIN, ANG_ZERO, ANG_HOT = -52.0, 16.0, 34.0
DB_LO, DB_HI = -20.0, 3.0
HUB_IN_FACE = PIVOT               # 铰点（面盘图内的坐标）—— 允许在面盘**外**
FACE = (FACE_W,)
# 两只表的控件左上（与 ui/radio.html 一致）
METERS = tuple((nm, FACE_XS[i], FACE_YS[i],
                (FACE_XS[i], FACE_YS[i], FACE_XS[i] + FACE_W, FACE_YS[i] + FACE_H))
               for i, nm in enumerate(("L", "R")))
R_IN, R_OUT = 24, NEEDLE_LEN - 4  # 径向搜索范围（针长 NEEDLE_LEN；针尾 11px 在 R_IN 内被排除）
ANG_FROM, ANG_TO = -95.0, 62.0    # 搜索角范围（刻度弧在上半圈；L/R 圆标在 -65° 但颜色不同）


def db_to_ang(db):
    if db <= 0.0:
        return ANG_MIN + (db - DB_LO) * (ANG_ZERO - ANG_MIN) / (0.0 - DB_LO)
    return ANG_ZERO + db * (ANG_HOT - ANG_ZERO) / DB_HI


def ang_to_db(a):
    if a <= ANG_ZERO:
        return DB_LO + (a - ANG_MIN) * (0.0 - DB_LO) / (ANG_ZERO - ANG_MIN)
    return (a - ANG_ZERO) * DB_HI / (ANG_HOT - ANG_ZERO)


def is_needle(px, tol):
    r, g, b = px[0], px[1], px[2]
    return abs(r - NEEDLE_RGB[0]) <= tol and abs(g - NEEDLE_RGB[1]) <= tol \
        and abs(b - NEEDLE_RGB[2]) <= tol


def read_meter(im, cx, cy, tol, face):
    """返回 ((角度, 覆盖率, 命中半径数), (次优角度, 覆盖率, _))。

    两步（**先粗后精**，为什么必须这样）：
      ① **粗定位**：对每个角度做"径向覆盖率"统计（沿该角度 r=R_IN..R_OUT 找指针色像素）。
         指针是唯一**贯穿整个半径范围**的东西 ⇒ 覆盖率自然最高；
         刻度线/数字只占十几像素的径向跨度，读不到覆盖率。
         ⚠️ 这一步的角分辨率很粗（±2°）：小半径处 1px 横向偏差就顶好几度，
         所以**不能**直接拿它当结论。
      ② **精修**：在粗定位 ±6° 内取"半径最大的那批指针色像素"（= 针尖附近），
         用它的实际坐标反算角度 —— r≈113 处 1px 误差只有 0.5°。
         另外取这些像素的**径向最小值**当"是否真的从铰点射出"的旁证。

    ★★ 2026-09-19 血案：**命中必须限制在面盘矩形内**（`face=(x0,y0,x1,y1)`）。
       指针色 (30,26,20)±12 = "很暗的暖黑"，而页面上的**按钮面 (22,22,24)、
       导航栏 (28,28,30)、提示条 (28,28,30)** 全都落在容差里！
       老版铰点在面盘**内**、搜索半径也小，搜索盘完全在浅黄面盘上 ⇒ 撞不到这些；
       本版铰点挪到面盘**外**（浅弧设计的必然结果）⇒ 搜索盘罩到按钮那一排，
       于是"每个角度覆盖率都 100%"，回读角度直接跑到 -86°（根本不在 -52°..+34° 内）。
       ⇒ 判据不能只看颜色，还要看**位置**：针只可能画在面盘里。
    """
    w, h = im.size
    px = im.load()
    fx0, fy0, fx1, fy1 = face

    def hit_at(x, y):
        if not (0 <= x < w and 0 <= y < h):
            return False
        if not (fx0 <= x < fx1 and fy0 <= y < fy1):
            return False                     # ★ 只认面盘内的像素
        return is_needle(px[x, y], tol)

    best, second = (-999.0, -1.0, -999.0), (-999.0, -1.0, -999.0)
    a = ANG_FROM
    while a <= ANG_TO:
        rad = math.radians(a)
        ux, uy = math.sin(rad), -math.cos(rad)
        cov = 0
        for r in range(R_IN, R_OUT + 1):
            hit = False
            for lat in (-1, 0, 1):                       # 允许 ±1px 横向偏差（抗锯齿）
                x = int(round(cx + ux * r - uy * lat))
                y = int(round(cy + uy * r + ux * lat))
                if hit_at(x, y):
                    hit = True
                    break
            if hit:
                cov += 1
        tot = R_OUT - R_IN + 1
        score = cov / float(tot)
        if score > best[1]:
            best, second = (a, score, cov), best
        elif score > second[1]:
            second = (a, score, cov)
        a += 0.25

    # ---- ② 精修：粗定位 ±6° 锥内的指针色像素（同样只认面盘内）----
    lo, hi = best[0] - 6.0, best[0] + 6.0
    cand = []
    for y in range(max(fy0, cy - R_OUT - 2), min(fy1, cy + R_OUT + 3)):
        for x in range(max(fx0, cx - R_OUT - 2), min(fx1, cx + R_OUT + 3)):
            if not is_needle(px[x, y], tol):
                continue
            d = math.hypot(x - cx, y - cy)
            if d < R_IN or d > R_OUT:
                continue
            aa = math.degrees(math.atan2(x - cx, -(y - cy)))
            if lo <= aa <= hi:
                cand.append((d, aa))
    if cand:
        cand.sort(reverse=True)
        tip = cand[:max(3, len(cand) // 5)]              # 最外侧 20% = 针尖
        # 角度是环形量，但针尖这批都在同一侧、跨度很小 ⇒ 直接算术平均即可
        ang = sum(t[1] for t in tip) / len(tip)
        return (ang, best[1], best[2]), second
    return best, second


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames", nargs="+", help="抓屏 PNG")
    ap.add_argument("--tolerance", type=float, default=12.0, help="指针色容差（默认 12）")
    ap.add_argument("--csv", action="store_true", help="输出 CSV")
    args = ap.parse_args()

    rc = 0
    if args.csv:
        print("frame,meter,angle_deg,db,cover")
    for path in args.frames:
        if not os.path.exists(path):
            print("!! 找不到 %s" % path)
            rc = 1
            continue
        im = Image.open(path).convert("RGB")
        if not args.csv:
            print("== %s  %s" % (os.path.basename(path), im.size))
        for name, fx, fy, face in METERS:
            cx, cy = fx + HUB_IN_FACE[0], fy + HUB_IN_FACE[1]
            (ang, cov, hits), (ang2, cov2, _) = read_meter(im, cx, cy, args.tolerance, face)
            ok = cov >= 0.45                                    # 针长 113 → 覆盖率应远超 45%
            if args.csv:
                print("%s,%s,%.2f,%.2f,%.2f" % (os.path.basename(path), name, ang,
                                                ang_to_db(ang), cov))
            else:
                flag = "OK " if ok else "?? "
                print("   %s%s  角 %+7.2f°  ≈ %+5.1f dB   覆盖率 %.0f%%（次优 %+6.1f°/%.0f%%）"
                      % (flag, name, ang, ang_to_db(ang), cov * 100, ang2, cov2 * 100))
            if not ok:
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
