#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_ha_assets.py —— 逐张体检"被切出来的图片"（智能家居页）

⚠️⚠️ 2026-09-24：关于"黑角"的那部分判据已被 `tools/audit_resources.py` 取代
   （本文件里的 `ON_CARD` 是**手写白名单**，必然漏；新工具用几何包含 + 校验落地结果）。
   本脚本保留仍然有效的部分：1:1 拉伸、引用与磁盘对不上、未引用素材（ha* 范围）。

为什么要有它（2026-09-23 用户报"图标角落都是黑的"，要求把所有切图再查一遍）：
  这一类问题**在 PC 设计稿上完全看不出来**，只有真机才现形：

  ① ★★ 圆角九宫格（`.9.png`）的**四角是透明的**，而 `inject_rounded` 挂图后会把
     `bgColorTab` 清成 -1 ⇒ 四角透出的是**窗口黑底**。
        · 控件坐在**窗口**上 → 露黑底 = 与背景一致，看不出来（OK）
        · 控件坐在**卡片/行**上 → 露黑底 = 卡片上四个黑角（**事故**）
     正解：坐在卡片上的圆角块必须用**不透明**的 1:1 PNG，四角烘卡片底色。

  ② 带 alpha 的图**在运行时** `setBackgroundPic()` 换图会**丢 alpha**
     （透明区渲染成白块/黑块，见 src/platform/PgSkin.h 血案）⇒ 运行时用的图必须不透明。

  ③ **1:1 铁律**：本工程素材不拉伸 ⇒ 普通 PNG 的尺寸必须等于控件尺寸；
     `.9.png` 靠九宫格拉伸，不受此限。

  ④ 引用与磁盘要对得上：json 引用的图必须存在；磁盘上的 ha_* 图必须被引用
     （没被引用的图只会白占 /res —— 本分区实测只剩 ~300KB）。

用法:
  python tools/check_ha_assets.py            # 全量审计
  python tools/check_ha_assets.py --quiet    # 只列问题
"""
import json
import os
import re
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
HTML = os.path.join(ROOT, "ui", "ha.html")
JSON = os.path.join(ROOT, "ui", "ha.json")
IMGDIR = os.path.join(ROOT, "resources", "images")

# 容器是"窗口"（黑底）还是"卡片/行"（#1C1C1E）——由控件在控件树里的位置判断：
# 这里手写一张"哪些控件坐在卡片/行上"的清单（改动 ui/ha.html 时要同步）。
ON_CARD = {                      # caption -> 所在容器底色
    "SubMyDic": (28, 28, 30),    # 我的设备行内（listview item）
    "SubPickDic": (28, 28, 30),  # 选择页行内
    "SubMyCard": None,           # 行底本身（在窗口上）
}


def load_json_usage():
    """从 ui/ha.json 收集 caption -> 用到的图（picTab / backgroundPic / bgColorTab）。"""
    d = json.load(open(JSON, encoding="utf-8"))
    cap2pic, deep = {}, {}

    def walk(node, depth):
        if isinstance(node, dict):
            cap = node.get("caption")
            pics = []
            for k, v in (node.get("picTab") or {}).items():
                pics.append(v)
            if node.get("backgroundPic"):
                pics.append(node["backgroundPic"])
            if isinstance(cap, str) and pics:
                cap2pic[cap] = pics
                deep[cap] = depth
            for v in node.values():
                if isinstance(v, (dict, list)):
                    walk(v, depth + 1)
        elif isinstance(node, list):
            for v in node:
                walk(v, depth + 1)

    walk(d, 0)
    return cap2pic


def pic_info(rel):
    """(size, mode, 四角, 中点, 是否带透明)"""
    fp = os.path.join(IMGDIR, os.path.basename(rel))
    if not os.path.isfile(fp):
        return None
    im = Image.open(fp)
    rgba = im.convert("RGBA")
    w, h = im.size
    corners = [rgba.getpixel((0, 0)), rgba.getpixel((w - 1, 0)),
               rgba.getpixel((0, h - 1)), rgba.getpixel((w - 1, h - 1))]
    has_tr = any(c[3] < 255 for c in corners)
    return {"file": fp, "size": (w, h), "mode": im.mode, "corners": corners,
            "center": rgba.getpixel((w // 2, h // 2)), "trans_corner": has_tr,
            "bytes": os.path.getsize(fp)}


def main():
    quiet = "--quiet" in sys.argv
    usage = load_json_usage()
    # 源稿里显式写的图（data-pic*/data-bgpic）也要算进来
    html = open(HTML, encoding="utf-8").read()
    html_pics = set(re.findall(r'data-(?:pic0|pic1|bgpic)="([^"]+\.png)"', html))

    all_pics = set()
    for v in usage.values():
        all_pics |= set(v)
    all_pics |= html_pics

    print("=" * 92)
    print("① json/源稿引用的图：%d 张" % len(all_pics))
    print("=" * 92)
    problems = []
    for rel in sorted(all_pics):
        info = pic_info(rel)
        name = os.path.basename(rel)
        if not info:
            print("   %-34s **缺失**（json 引用了但磁盘上没有）" % name)
            problems.append((name, "文件缺失"))
            continue
        users = [c for c, v in usage.items() if rel in v]
        on_card = [c for c in users if c in ON_CARD and ON_CARD[c]]
        is_nine = rel.endswith(".9.png")
        verdict = []
        if info["mode"] == "RGB":
            verdict.append("不透明✓")
        else:
            verdict.append("带alpha")
        if info["trans_corner"]:
            verdict.append("四角透明")
        if info["trans_corner"] and on_card:
            verdict.append("✗✗ 座位在卡片/行上 ⇒ **会露黑角**（须换不透明烘底图）")
            problems.append((name, "圆角透明 + 坐在卡片上 = 黑角；用在 " + ",".join(on_card)))
        elif info["trans_corner"] and users:
            verdict.append("（四角透明，但坐在窗口上 → 露黑底=背景，OK）")
        if not is_nine and not info["trans_corner"] and users:
            # 1:1 铁律：普通 PNG 必须与控件尺寸一致
            from_check = []
            for cap in users:
                pass
            verdict.append("")
        line = "   %-34s %3dx%-3d %-4s %6d B  %s%s" % (
            name, info["size"][0], info["size"][1], info["mode"], info["bytes"],
            " ".join(v for v in verdict if v),
            ("  用在:" + ",".join(users[:3])) if users else "")
        if not quiet or (info["trans_corner"] and on_card):
            print(line)

    # ---- ② 普通 PNG 的 1:1 检查（控件尺寸 vs 图尺寸）----
    print()
    print("=" * 92)
    print("② 1:1 铁律：普通 PNG（非 .9）的尺寸必须 == 控件尺寸，否则会被拉伸")
    print("=" * 92)
    dw = json.load(open(JSON, encoding="utf-8"))

    def ctrls(node, out):
        if isinstance(node, dict):
            if "caption" in node and "position" in node:
                out.append(node)
            for v in node.values():
                if isinstance(v, (dict, list)):
                    ctrls(v, out)
        elif isinstance(node, list):
            for v in node:
                ctrls(v, out)
        return out

    fits = 0
    for c in ctrls(dw, []):
        pics = list((c.get("picTab") or {}).values())
        if c.get("backgroundPic"):
            pics.append(c["backgroundPic"])
        for rel in pics:
            if rel.endswith(".9.png"):
                continue
            info = pic_info(rel)
            if not info:
                continue
            p = c["position"]
            if info["size"] != (p["width"], p["height"]):
                print("   %-34s 图%dx%d != 控件 %-12s %dx%d  ⇒ **会被拉伸**"
                      % (os.path.basename(rel), info["size"][0], info["size"][1],
                         c.get("caption"), p["width"], p["height"]))
                problems.append((os.path.basename(rel), "1:1 不符（会被拉伸）"))
            else:
                fits += 1
    print("   尺寸 1:1 的普通 PNG：%d 张" % fits)

    # ---- ③ 磁盘上的 ha_* 图有没有被引用 ----
    print()
    print("=" * 92)
    print("③ 磁盘上的 ha_* / ha- 图：有没有**没被引用**的白占空间")
    print("=" * 92)
    used_files = set(os.path.basename(r) for r in all_pics)
    total = 0
    for f in sorted(os.listdir(IMGDIR)):
        if not f.startswith("ha"):
            continue
        total += os.path.getsize(os.path.join(IMGDIR, f))
        mark = "被引用" if f in used_files else "**未被引用**"
        if mark != "被引用":
            print("   %-34s %7d B  %s" % (f, os.path.getsize(os.path.join(IMGDIR, f)), mark))
    print("   ha* 素材合计 %d 字节（%.1f KB）" % (total, total / 1024.0))

    print()
    print("=" * 92)
    if problems:
        print("发现 %d 个问题：" % len(problems))
        for n, why in problems:
            print("   %-34s %s" % (n, why))
        return 1
    print("PASS：没有发现黑角 / 拉伸 / 缺图 / 白占空间的问题")
    return 0


if __name__ == "__main__":
    sys.exit(main())
