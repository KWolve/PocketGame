#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_bounds.py —— 画布外的越界体检（**改坐标之后必跑**）

为什么需要它（2026-09-16 血案）：
    给全局状态栏加高、把各页顶栏"整体下移"时，用脚本把 `y >= 某值` 的坐标统一 +22，
    结果**把底部那批按钮也推下去了** —— probe 页 16 个控件的 `y+h` 到了 **806**（屏高 800），
    按钮和标签各露出屏外 6px。肉眼看截图只能看出"有点挤"，**算一遍就立刻抓出来**。

判据（屏幕 480x800）：
    · `x + w > 480` / `y + h > 800` / `x < 0` / `y < 0` ⇒ 越界
    · **只算"顶层"控件**：`class="list"` 内部的子控件 y 是**行内相对坐标**，
      另外 `Win*` 全屏容器（0,0,480,800）本身是容器，都不参与判定。

用法：
    python tools/check_bounds.py            # 扫全部 ui/*.html
    python tools/check_bounds.py camera     # 只看某页（可多个）
退出码：0 = 无越界；1 = 有越界（可直接进 CI / 验收脚本）
"""
import io
import os
import re
import sys

W, H = 480, 800
HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, os.pardir, "ui")

CTRL = re.compile(
    r'data-caption="([^"]+)"[^>]*data-x="(-?\d+)"[^>]*data-y="(-?\d+)"'
    r'[^>]*data-w="(\d+)"[^>]*data-h="(\d+)"')


def scan(path):
    """返回 [(行号, caption, x, y, w, h), ...]，只含顶层控件。"""
    out = []
    list_indent = None
    with io.open(path, encoding="utf-8", errors="replace") as f:
        for idx, ln in enumerate(f, 1):
            st = ln.lstrip()
            ind = len(ln) - len(st)
            if st.startswith("</"):
                if list_indent is not None and ind < list_indent:
                    list_indent = None
                continue
            if re.search(r'class="list"', st):
                list_indent = ind
            m = CTRL.search(st)
            if not m:
                continue
            # 列表内部的子控件 = 行内相对坐标，不判定
            if list_indent is not None and ind > list_indent:
                continue
            cap = m.group(1)
            # 全屏 window 容器（带 data-w=480 data-h=800）是容器本身，跳过
            x, y, w, h = (int(m.group(i)) for i in (2, 3, 4, 5))
            if w >= W and h >= H:
                continue
            out.append((idx, cap, x, y, w, h))
    return out


def main():
    want = sys.argv[1:]
    files = sorted(f for f in os.listdir(UI)
                   if f.endswith(".html") and not f.endswith(".preview.html"))
    if want:
        files = [f for f in files if os.path.splitext(f)[0] in want]

    bad = []
    for fn in files:
        for ln, cap, x, y, w, h in scan(os.path.join(UI, fn)):
            why = []
            if x < 0:
                why.append("x<0")
            if y < 0:
                why.append("y<0")
            if x + w > W:
                why.append("右边越界 x+w=%d>%d" % (x + w, W))
            if y + h > H:
                why.append("底边越界 y+h=%d>%d" % (y + h, H))
            if why:
                bad.append((fn, ln, cap, x, y, w, h, "；".join(why)))

    print("屏幕 %dx%d，扫描 %d 个页面" % (W, H, len(files)))
    if not bad:
        print("★ 全部控件都在屏内（无越界）")
        return 0
    print("!! 越界 %d 处：" % len(bad))
    for fn, ln, cap, x, y, w, h, why in bad:
        print("  %-18s L%-5d %-22s x=%d..%d y=%d..%d  ← %s"
              % (fn, ln, cap, x, x + w, y, y + h, why))
    print()
    print("提示：**批量改坐标之后最容易出这个**（如统一 +22 时把底部控件也推下去）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
