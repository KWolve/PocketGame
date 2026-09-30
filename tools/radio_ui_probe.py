#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""radio_ui_probe.py - 电台页"布局/触摸"的逐像素探针（给抓屏分析用，不依赖日志）

为什么不用日志：本固件**没有 `logd`**（`/bin/sh: logd: not found`），
`adb logcat` 流又会卡死 ⇒ 判据只能落在**画面上**。（2026-09-19 实测）

本探针输出：
  · `face`  ：浅黄表盘像素的**外接框** + 每行宽度（判"表盘盖到哪了"）
  · `bands` ：关键横带的平均色（判"时间/总电平条有没有被盖"）
  · `name`  ：台名带的指纹（判"换台按钮点了有没有反应"）
  · `digits`：某带里"青色数字像素"计数（时间/读数是否真的画出来了）

用法：
    python tools/radio_ui_probe.py shot D:/Temp/x.png      # 分析已有帧
    python tools/radio_ui_probe.py live                    # 抓一帧再分析
"""
import io
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PY = sys.executable

# 关键横带（y0, y1, 名字）—— 全部来自 ui/radio.html 的控件盒
BANDS = [
    (348, 400, "台名"),
    (400, 424, "分组信息"),
    (424, 454, "★已播时长"),
    (454, 462, "★总电平条"),
    (468, 616, "仪表区"),
    (620, 724, "★控制按钮"),
]


def is_face(p):
    """浅黄面盘（米黄 cream：上 (255,250,226) → 下 (238,220,168)）。"""
    r, g, b = p
    return r > 195 and g > 185 and b < 225 and (r - b) > 18


def is_cyan(p):
    """已播时长/电平数字色 #64D2FF。"""
    r, g, b = p
    return b > 150 and b - r > 40 and g > 120


def analyze(path):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    W, H = im.size
    px = im.load()
    out = {"size": (W, H)}

    rows = {}
    for y in range(52, H):
        c = sum(1 for x in range(W) if is_face(px[x, y]))
        if c > 6:
            rows[y] = c
    if rows:
        ys = sorted(rows)
        out["face"] = (ys[0], ys[-1], max(rows.values()))
    else:
        out["face"] = None

    bands = []
    for y0, y1, nm in BANDS:
        rs = gs = bs = 0
        n = 0
        cy = 0
        for y in range(y0, min(y1, H)):
            for x in range(0, W, 3):
                r, g, b = px[x, y]
                rs += r
                gs += g
                bs += b
                n += 1
                if is_cyan((r, g, b)):
                    cy += 1
        bands.append((nm, y0, y1, rs / n, gs / n, bs / n, cy))
    out["bands"] = bands

    # 台名带指纹（换台会变）
    fp = 0
    for y in range(352, 396):
        for x in range(16, 464, 2):
            r, g, b = px[x, y]
            fp = (fp * 31 + (r * 3 + g * 5 + b * 7) // 16) & 0xFFFFFFFF
    out["name"] = fp
    return out


def fmt(a):
    L = []
    f = a["face"]
    L.append("  表盘浅黄像素：%s" % ("y=%d..%d  最宽 %d" % f if f else "无（非表盘视图）"))
    for nm, y0, y1, r, g, b, cy in a["bands"]:
        L.append("  %-12s y=%3d..%3d  均色(%3.0f,%3.0f,%3.0f)  青数字 %4d" % (nm, y0, y1, r, g, b, cy))
    L.append("  台名指纹 0x%08X" % a["name"])
    return "\n".join(L)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "live"
    if mode == "live":
        tmp = "D:/Temp/pgqa/probe.png"
        os.makedirs(os.path.dirname(tmp), exist_ok=True)
        subprocess.run([PY, os.path.join(ROOT, "tools", "grab.py"), tmp],
                       capture_output=True)
        path = tmp
    else:
        path = sys.argv[2]
    if not os.path.isfile(path):
        raise SystemExit("没有帧文件：%s" % path)
    print("帧 %s" % path)
    print(fmt(analyze(path)))


if __name__ == "__main__":
    main()
