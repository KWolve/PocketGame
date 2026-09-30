#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
rhythm_qa.py - 「下落式节奏」游戏的画面验收（节奏钢琴 / 打鼓）

为什么需要它：这类游戏的画面**每帧都在动**，"抓一张图看看"根本判不出对错 ——
2026-09-16 的血案就是"音符确实被 draw 了，但 y 恒等于判定线（整数除法把
每毫秒像素算成 0），而且被上层的琴键/鼓垫盖住"，静态截图看不出任何异常，
只有"**同一局内两次采样必须不同**"这个判据能抓到。

用法：
    python tools/rhythm_qa.py a.png b.png piano
    python tools/rhythm_qa.py a.png b.png drum

判据（两个，都要满足）：
  1) **舞台区差分**：两帧之间舞台区应有大量变化像素（修复前钢琴是 0）；
  2) **音符像素分布**：舞台上应能测到"音轨主题色"的像素，且两帧的质心 y 不同。

截图要求：真机 `tools/grab.py` 抓的 480x800 全屏图，**同一局运行中**相隔约 1 秒抓两张。
画布在屏幕上的位置：left=0 top=160（与 ui/main.json 的 GameCanvas 一致）。
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：换一个带 PIL 的解释器（本机用 Python314）")

CANVAS_TOP = 160          # 画布在屏幕里的 top（ui/main.json 的 GameCanvas.position.top）
STAGE_TOP = 46            # 画布内：顶部两条信息条占 0..45
# 音轨主题色（与 tools/gen_instr_art.py 的 LANE_COL / DRUM_COL 同源）
LANE_COL = [(224, 90, 74), (232, 138, 60), (232, 200, 74), (92, 200, 110),
            (74, 168, 224), (122, 122, 232), (176, 106, 224), (232, 106, 176)]
DRUM_COL = [(224, 90, 74), (232, 138, 60), (232, 200, 74),
            (92, 200, 110), (74, 168, 224), (176, 106, 224)]
TOL = 14
DIFF_TH = 12


def stage_rect(kind):
    """舞台（画布坐标）-> 屏幕坐标。判定线 = 钢琴 388 / 打鼓 kDrumFallH-4 = 296。"""
    judge = 388 if kind == "piano" else 296
    return STAGE_TOP + CANVAS_TOP, judge + CANVAS_TOP


def near(px, cols):
    r, g, b = px[0], px[1], px[2]
    for c in cols:
        if abs(r - c[0]) <= TOL and abs(g - c[1]) <= TOL and abs(b - c[2]) <= TOL:
            return True
    return False


def note_stats(im, kind):
    cols = LANE_COL if kind == "piano" else DRUM_COL
    y0, y1 = stage_rect(kind)
    px = im.load()
    total, rows = 0, []
    for y in range(y0, y1):
        n = 0
        for x in range(im.size[0]):
            if near(px[x, y], cols):
                n += 1
        if n:
            total += n
            rows.append(y - CANVAS_TOP)      # 报画布坐标，便于与 QA 日志对照
    if not rows:
        return 0, 0, -1, -1, -1.0
    cy = sum(r * 1 for r in rows) / float(len(rows))
    return total, len(rows), min(rows), max(rows), cy


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    a, b, kind = sys.argv[1], sys.argv[2], sys.argv[3].lower()
    if kind not in ("piano", "drum"):
        print("kind 只能是 piano / drum")
        return 2
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    pa, pb = ia.load(), ib.load()
    W, H = ia.size
    y0, y1 = stage_rect(kind)

    chg = 0
    rows = set()
    for y in range(y0, y1):
        for x in range(W):
            p, q = pa[x, y], pb[x, y]
            if (abs(p[0] - q[0]) > DIFF_TH or abs(p[1] - q[1]) > DIFF_TH or
                    abs(p[2] - q[2]) > DIFF_TH):
                chg += 1
                rows.add(y - CANVAS_TOP)

    ta, ra, loa, hia, cya = note_stats(ia, kind)
    tb, rb, lob, hib, cyb = note_stats(ib, kind)
    print("[%s] A=%s  B=%s" % (kind, a, b))
    print("  ① 舞台区差分：变化像素 = %d（涉及 %d 行，画布 y %s..%s）"
          % (chg, len(rows), min(rows) if rows else -1, max(rows) if rows else -1))
    print("  ② 音符像素：A=%d 像素/%d 行 y=%d..%d 质心=%d ；B=%d 像素/%d 行 y=%d..%d 质心=%d"
          % (ta, ra, loa, hia, int(cya), tb, rb, lob, hib, int(cyb)))
    ok = chg > 300 and ta > 300 and tb > 300
    print("  结论：%s" % ("通过（音符在舞台上且画面在动）" if ok else
                        "**不通过** —— 舞台区没动或没有音符（检查下落映射与层级）"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
