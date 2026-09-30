#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_rounded_on_card.py —— 全工程普查：**哪些"圆角块"坐在卡片上**（会渲成黑角）

⚠️⚠️ 2026-09-24：**本脚本的判据已被 `tools/audit_resources.py` 取代，仅留作历史参考。**
   原因：它假设"圆角素材的圆角外是透明的、所以露 bgColorTab ⇒ 坐在卡片上就是黑角"，
   并据此输出"要给这些控件补容器色"的清单（那张清单曾以 `gen_ui.CARD_FILL` 的形式落地）。
   后来实测发现两件事，判据因此重构：
     ① 当时的素材其实是**圆角外烘不透明黑**的（不是透明），所以"补 bgColorTab"只能修
        最外 1px 那一圈、修不掉里面的黑 —— 真正的解法是**素材改烘透明底**；
     ② 本工程控件树是**平铺**的，"包着谁"只能靠**几何包含**判断（本脚本用的是几何，
        这点是对的），而新的 `audit_resources.py` 把这套判据与"图角行为"合在一起，
        并且直接**校验落地结果**（控件的 bgColorTab 是否 == 容器色）。
   ⇒ 现在查"黑倒角"请用：`python tools/audit_resources.py`（第【1】节）。

背景（2026-09-24 用户报"图标角落都是黑的"）：
  真机实测坐实：带圆角九宫格（`picTab`）的控件，其"透明四角"渲染出来是**控件自己填的一层黑**，
  **不是**"透出下面的兄弟控件"。所以
    · 坐在**窗口**（黑）上 → 角黑 = 与背景同色，看不出来（正常，占绝大多数）
    · 坐在**卡片/面板**上 → 角黑 + 周围是卡片色 ⇒ **四个黑角 / 一圈黑边**（事故）

判据（纯静态，不用上机）：对每个挂了 picTab 的控件，
  找**同窗口内、绘制在它之前、且矩形完整覆盖它**的那个"最小容器"；
  若该容器的填充色不是黑（且不是窗口本身）⇒ 判定为"坐在卡片上"，会显黑角。

用法:
  python tools/check_rounded_on_card.py               # 全工程 ui/*.json
  python tools/check_rounded_on_card.py ui/ha.json    # 只看一页
"""
import glob
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import re

# data-round 的取值 → 面板图（与 tools/gen_ui.py 的 ROUND_ASSETS 保持一致）
ROUND_ASSETS = {"panel": "ios_panel", "panel2": "ios_panel2", "panel2c": "ios_panel2_c"}


def pic_center(name, is_nine=True):
    """读素材中点色 → #RRGGBB（用来当"容器的底色"）。"""
    from PIL import Image
    fp = os.path.join(ROOT, "resources", "images", name + (".9.png" if is_nine else ".png"))
    if not os.path.isfile(fp):
        return None
    im = Image.open(fp).convert("RGBA")
    r, g, b, a = im.getpixel((im.size[0] // 2, im.size[1] // 2))
    return None if a < 200 else "#%02X%02X%02X" % (r, g, b)


def src_round(html_path):
    """caption -> data-round 的值（这类容器**没有 data-bg**，靠面板图当底）。"""
    try:
        txt = open(html_path, encoding="utf-8").read()
    except OSError:
        return {}
    out = {}
    for m in re.finditer(r"<[^>]*>", txt):
        tag = m.group(0)
        cap = re.search(r'data-caption="([^"]+)"', tag)
        rd = re.search(r'data-round="([^"]+)"', tag)
        if cap and rd:
            out[cap.group(1)] = rd.group(1)
    return out


def src_bg(html_path):
    """从源稿读 caption -> data-bg/#hex —— 这是**权威**（json 里的 -1 是"未设置/透明"，
    直接 to_hex 会变成 #FFFFFF 造成一堆假阳性，实测踩过）。"""
    try:
        txt = open(html_path, encoding="utf-8").read()
    except OSError:
        return {}
    out = {}
    for m in re.finditer(r"<[^>]*>", txt):
        tag = m.group(0)
        cap = re.search(r'data-caption="([^"]+)"', tag)
        bg = re.search(r'data-bg="(#[0-9A-Fa-f]{6})"', tag)
        if cap and bg:
            out[cap.group(1)] = bg.group(1)
    return out


def to_hex(v):
    return None if v is None else "#%06X" % (v & 0xFFFFFF)


def luma(v):
    if v is None:
        return -1
    v &= 0xFFFFFF
    r, g, b = (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
    return (r * 299 + g * 587 + b * 114) // 1000


def collect(path):
    """返回 [(order, window, caption, rect, fill, pic0)]，order = 源稿定义顺序。"""
    d = json.load(open(path, encoding="utf-8"))
    out = []
    n = [0]

    def walk(node, win, wx, wy):
        if isinstance(node, dict):
            cap = node.get("caption")
            p = node.get("position")
            if cap and p:
                n[0] += 1
                # 填充色：优先 bgColorTab，其次 backgroundColor
                bg = (node.get("bgColorTab") or {}).get("color0")
                if bg in (None, -1):
                    bg = node.get("backgroundColor")
                out.append({
                    "order": n[0], "win": win, "cap": cap,
                    "x": p["left"] + wx, "y": p["top"] + wy,
                    "w": p["width"], "h": p["height"],
                    "fill": bg, "pic0": (node.get("picTab") or {}).get("pic0"),
                })
            if isinstance(node.get("item"), dict):
                for sv in node["item"].get("subItem", []):
                    sp = sv.get("position")
                    if sp:
                        n[0] += 1
                        out.append({"order": n[0], "win": win, "cap": sv.get("caption", "?"),
                                    "x": sp["left"] + wx, "y": sp["top"] + wy,
                                    "w": sp["width"], "h": sp["height"],
                                    "fill": None, "pic0": (sv.get("picTab") or {}).get("pic0")})
                return
            for v in node.values():
                if isinstance(v, (dict, list)):
                    walk(v, win, wx, wy)
        elif isinstance(node, list):
            for v in node:
                walk(v, win, wx, wy)

    for k, v in d.items():
        if not str(k).startswith("window") or not isinstance(v, dict):
            continue
        wp = v.get("position") or {}
        walk(v, v.get("caption"), wp.get("left", 0), wp.get("top", 0))
    return out


def luma_hex(h):
    h = h.lstrip("#")
    r, g, b = int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)
    return (r * 299 + g * 587 + b * 114) // 1000


def main():
    files = sys.argv[1:] if len(sys.argv) > 1 else sorted(glob.glob(os.path.join(ROOT, "ui", "*.json")))
    total_bad = 0
    for f in files:
        if os.path.basename(f) in ("ha.preview.json",):
            continue
        ctrls = collect(f)
        hp = os.path.join(ROOT, "ui", os.path.basename(f).replace(".json", ".html"))
        bgmap = src_bg(hp)
        rmap = src_round(hp)

        def fill_of(cap):
            """容器的底色：data-bg → data-round 的面板图 → 自己的 pic0 图；都没有则当作窗口（黑）。"""
            h = bgmap.get(cap)
            if h:
                return h
            r = rmap.get(cap)
            if r and ROUND_ASSETS.get(r):
                return pic_center(ROUND_ASSETS[r])
            return None
        # listview 不能当容器：它的 item 只画"实际有数据的行"，空列表时它什么都不画
        lists = set()
        try:
            dj = json.load(open(f, encoding="utf-8"))
            for k, v in dj.items():
                if not str(k).startswith("window") or not isinstance(v, dict):
                    continue
                for kk, vv in v.items():
                    if isinstance(vv, dict) and "cols" in vv and vv.get("caption"):
                        lists.add(vv["caption"])
        except Exception:
            pass
        bad = []
        for c in ctrls:
            if not c["pic0"]:
                continue
            cands = []
            for o in ctrls:
                if o is c or o["win"] != c["win"] or o["order"] >= c["order"]:
                    continue
                if o["w"] == c["w"] and o["h"] == c["h"] and o["x"] == c["x"] and o["y"] == c["y"]:
                    continue                     # 同格叠放的兄弟（段控两态）不算容器
                if (c["x"] >= o["x"] and c["y"] >= o["y"]
                        and c["x"] + c["w"] <= o["x"] + o["w"]
                        and c["y"] + c["h"] <= o["y"] + o["h"]):
                    cands.append(o)
            if not cands:
                continue
            cont = min(cands, key=lambda o: o["w"] * o["h"])
            if cont["cap"] in lists:
                continue                          # listview 不是"有色块"容器
            fl = fill_of(cont["cap"])
            if not fl:
                continue                          # 容器没有实色底 ⇒ 当作窗口 ⇒ 角黑看不出来
            if luma_hex(fl) <= 25:
                continue                          # 容器本身是黑的 ⇒ 角黑看不出来
            bad.append((c["cap"], cont["cap"], fl, bgmap.get(c["cap"]), c["pic0"]))
        if bad:
            print("### %s：%d 个控件坐在卡片上（会显黑角）" % (os.path.basename(f), len(bad)))
            for cap, cont, fl, own, pic in bad:
                print("    %-20s 坐在 %-18s(底 %s) 上   自己的图 %s" % (cap, cont, fl, pic))
            total_bad += len(bad)
    print()
    print("== 全工程合计 %d 个" % total_bad)
    return 0


if __name__ == "__main__":
    sys.exit(main())
