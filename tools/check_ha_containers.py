#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_ha_containers.py —— 静态普查：**哪些按钮坐在"非窗口"的容器上**

为什么需要它（2026-09-24 用户报"添加设备界面/内部弹层图标有黑角"）：
  真机实测坐实：**带圆角九宫格（picTab）的控件，其"透明四角"渲染出来是控件自己
  填的一层黑**（不是"透出下面的兄弟控件"）。所以
    · 坐在**窗口**（黑）上 → 角是黑 = 与背景同色，看不出来（正常）
    · 坐在**卡片/面板**（#1C1C1E 等）上 → 角是黑的、周围是卡片色 ⇒ **四个黑角**（事故）
  正解：把该控件的 `data-bg` 改成**容器色**（于是 bgColorTab 也跟着变），
        并用 `data-pic0/pic1` **显式**指定原来的圆角图 ——
        `inject_rounded()` 见到 pic0 已存在就"尊重"，不会再把它清成 -1。
        ⇒ 圆的形状仍由九宫格提供（#2C2C2E 之类），而角上填的是容器色，看不出来。

用法: python tools/check_ha_containers.py [--fix]   # --fix 只打印建议，不代改
"""
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
JSON = os.path.join(ROOT, "ui", "ha.json")


def to_hex(v):
    return None if v is None else "#%06X" % (v & 0xFFFFFF)


def load():
    d = json.load(open(JSON, encoding="utf-8"))
    ctrls = []

    def walk(node, win):
        if isinstance(node, dict):
            if "position" in node and node.get("caption"):
                ctrls.append((win, node))
            if isinstance(node.get("item"), dict):
                for sv in node["item"].get("subItem", []):
                    if "position" in sv:
                        ctrls.append((win, sv))
                return
            for v in node.values():
                if isinstance(v, (dict, list)):
                    walk(v, win)
        elif isinstance(node, list):
            for v in node:
                walk(v, win)

    for k, v in d.items():
        if isinstance(v, dict) and "__" in k and str(v.get("caption", "")).startswith("Win"):
            for ck, cv in v.items():
                if isinstance(cv, dict) and "position" in cv:
                    walk(cv, v["caption"])
    return ctrls


def rect(n):
    p = n["position"]
    return p["left"], p["top"], p["width"], p["height"]


def inside(a, b):
    """a 完全落在 b 内（且不是同一个）"""
    ax, ay, aw, ah = rect(a); bx, by, bw, bh = rect(b)
    return ax >= bx and ay >= by and ax + aw <= bx + bw and ay + ah <= by + bh


def main():
    ctrls = load()
    # 只关心"挂了圆角图"的控件
    withpic = [(w, n) for w, n in ctrls if (n.get("picTab") or {}).get("pic0")]
    print("挂了圆角图（picTab.pic0）的控件：%d 个" % len(withpic))
    print()
    groups = {}
    for w, n in withpic:
        # 找**最小**的外层容器（不含自己）
        outer = [(w2, n2) for w2, n2 in ctrls if n2 is not n and inside(n, n2)]
        if not outer:
            continue
        # 排除"纯装饰的透明面板"：用它的 bgColorTab 判断
        outer.sort(key=lambda t: rect(t[1])[2] * rect(t[1])[3])
        cw, cn = outer[0]
        cont_bg = (cn.get("bgColorTab") or {}).get("color0")
        groups.setdefault(cn.get("caption", "?"), []).append((w, n, cont_bg, cn))

    n_bad = 0
    for cont, items in sorted(groups.items()):
        for w, n, cont_bg, cn in items:
            own_pic = (n.get("picTab") or {}).get("pic0")
            body = None
            fp = os.path.join(ROOT, "resources", "images", own_pic)
            if os.path.isfile(fp):
                from PIL import Image
                im = Image.open(fp).convert("RGBA")
                body = im.getpixel((im.size[0] // 2, im.size[1] // 2))[:3]
            print("   %-18s 坐在 %-16s（容器底 %s）上" % (n.get("caption"), cont, to_hex(cont_bg)))
            print("        自己的图 %s（本体色 %s）" % (own_pic, str(body)))
            n_bad += 1
    print()
    print("⇒ 共 %d 个控件坐在【非窗口】容器上：这些的圆角会渲染成**纯黑角**。" % n_bad)
    print("  修法：把它们的 data-bg 改成容器色，并**显式**写 data-pic0/data-pic1 指向原图。")
    print("  涉及容器色：" + "、".join(sorted({str(to_hex(cb)) for _, _, cb, _ in sum(groups.values(), []) if cb is not None})))
    return 0


if __name__ == "__main__":
    sys.exit(main())
