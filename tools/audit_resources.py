#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""audit_resources.py —— `resources/` 目录**全量**审计（专查"黑倒角 / 黑边 / 拉伸 / 白占空间"）

为什么要有它（2026-09-24 用户：「你弄个脚本检讨一下 resources 目录下的资源」）：
  这一类问题**在 PC 设计稿、源码里都看不见**，只有上机才现形：
    · 圆角块的四角渲染成**纯黑**（用户叫"黑色倒角"）；
    · 卡片/行底多出**一圈 1px 黑边**；
    · 图与控件尺寸不符 ⇒ 被框架拉伸 ⇒ 发糊/锯齿；
    · 没人引用的图白占 `/res`（该分区实测只剩 ~3.5MB）。
  已有的几个脚本各管一段（`check_assets.py` 管边缘质量、`check_stretch.py` 管 1:1、
  `check_ha_assets.py` 只管 ha 页、`check_rounded_on_card.py` 只管静态"坐在卡片上"），
  **没有一个总账**，很容易"改完 A 忘了 B"。这个脚本就是那本总账。

★★ 核心判据：**一张图会不会显示成"黑倒角"，取决于「图的角行为」× 「它坐在什么底色上」**
  —— 只看素材本身判不出来，必须两边一起看。

  「图的角行为」（对**内容区**取样，`.9.png` 跳过最外 1px marker 环）：

    ┌ 透明角 ─────── 圆角外 α=0。框架渲染时露出的是**控件自己的 bgColorTab**；
    │                没设色 = -1 = **纯黑** ⇒ 坐在卡片上就是四个黑角。
    │                ⇒ 可救：把 bgColorTab 补成容器色（gen_ui.py 的 CARD_FILL 就是干这个）。
    ├ 死黑角 ─────── 圆角外 α=255 且 RGB≈(0,0,0)（"烘了黑底"的 1:1 图）。
    │                角是**画在图里的像素**，改 bgColorTab **没用** ⇒ 只能换图。
    └ 本色角 ─────── 圆角外的不透明色 ≈ 本体色（或本来就是实心矩形）⇒ 安全。

  「座位底色」的取得顺序（全是**权威来源**，不靠猜）：
    ① 源稿 `data-bg` —— 容器自己声明的底色；
    ② 源稿 `data-round=<面板名>` —— 面板图的中点色（这类容器**没有** data-bg）；
    ③ 容器自己也是个挂图的控件（如 listview item 的行底）⇒ 用**那张图的本体色**；
    ④ 都没有 ⇒ 当作"窗口"（黑）⇒ 角黑看不出来，不算问题。

用法:
  python tools/audit_resources.py              # 全量审计（给人看）
  python tools/audit_resources.py --quiet      # 只列问题
  python tools/audit_resources.py --section 1  # 只跑第 1 节
退出码：0 = 无硬问题，1 = 有（可直接当打包前置检查）
"""
import glob
import json
import os
import re
import sys
from collections import Counter

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG_DIR = os.path.join(ROOT, "resources", "images")
UI_DIR = os.path.join(ROOT, "ui")
SRC_DIR = os.path.join(ROOT, "src")
GEN_UI = os.path.join(ROOT, "tools", "gen_ui.py")

WIN_BG = (0, 0, 0)          # 窗口底色（本工程所有整屏 window 都是黑）
DARK_LUM = 25               # "等于黑"的亮度阈值
ALPHA_OPAQUE = 250          # α >= 它才算"不透明"

hard, soft = [], []         # 硬问题（必须修）/ 软问题（提示）


# ---------------------------------------------------------------- 基础工具
def lum(c):
    return (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000


def near(a, b, tol=10):
    return max(abs(a[i] - b[i]) for i in range(3)) <= tol


def hexs(c):
    return "#%02X%02X%02X" % (c[0], c[1], c[2])


def strip_comments(txt):
    """去掉 C/C++ 注释 —— 注释里经常出现**示意性的文件名**（如 `app_icon_N.png`），
    不剥就会被当成"真实引用"（2026-09-24 实测假报一条）。"""
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
    txt = re.sub(r"//[^\n]*", " ", txt)
    return txt


def hexs_rgb(v):
    """0xRRGGBB → '#RRGGBB'；-1/None → '-1(黑)'（本工程约定 -1 = 不设底色 = 黑）。"""
    return "-1(黑)" if v is None or v < 0 else "#%06X" % (v & 0xFFFFFF)


def parse_genui_table(name):
    """从 gen_ui.py 里**读**声明表（自动同步，避免两处手抄走样）。"""
    try:
        txt = open(GEN_UI, encoding="utf-8").read()
    except OSError:
        return {}
    m = re.search(name + r"\s*=\s*\{(.*?)\n\}", txt, re.S)
    if not m:
        return {}
    out = {}
    for k, v in re.findall(r'"([^"]+)"\s*:\s*"([^"]+)"', m.group(1)):
        out[k] = v
    return out


ROUND_ASSETS = parse_genui_table("ROUND_ASSETS") or {
    "panel": "ios_panel", "panel2": "ios_panel2", "panel2c": "ios_panel2_c"}
# 注：原先的 `CARD_FILL`（手工登记"坐在卡片上的按钮"）已在 2026-09-24 被
#     `gen_ui.parse_caption_fill()` + 几何包含判据取代 —— 白名单必然漏，现在不需要了。
ROUND_FILL = parse_genui_table("ROUND_FILL")        # data-round 面板名 → 面板本体底色


_img_cache = {}


def analyze(name):
    """★ 单张图的"角行为"分析（缓存）。返回 dict 或 None（文件不存在）。"""
    if name in _img_cache:
        return _img_cache[name]
    fp = os.path.join(IMG_DIR, name)
    if not os.path.isfile(fp):
        _img_cache[name] = None
        return None
    im = Image.open(fp)
    src_mode = im.mode
    rg = im.convert("RGBA")
    w, h = rg.size
    px = rg.load()
    is9 = name.endswith(".9.png")
    off = 1 if is9 else 0                     # .9.png 跳过最外 1px marker 环

    # 本体色 = 内容区中心 30% 的不透明众数（不要把"圆角外的黑"算进来）
    cx0, cy0 = int(w * 0.35), int(h * 0.35)
    cx1, cy1 = int(w * 0.65) + 1, int(h * 0.65) + 1
    cc = Counter(px[x, y] for y in range(cy0, cy1) for x in range(cx0, cx1)
                 if px[x, y][3] >= ALPHA_OPAQUE)
    body = cc.most_common(1)[0][0] if cc else None
    body_lum = lum(body) if body else -1

    # 四角（取"角上 3x3 邻域"的最外那点，避开圆角弧上的抗锯齿）
    corners = [px[off, off], px[w - 1 - off, off],
               px[off, h - 1 - off], px[w - 1 - off, h - 1 - off]]
    n_trans = sum(1 for c in corners if c[3] < ALPHA_OPAQUE)
    if n_trans:
        kind = "透明角"
    elif max(lum(c) for c in corners) < DARK_LUM and body_lum > DARK_LUM:
        kind = "死黑角"
    else:
        kind = "本色角"

    # 透明像素占比（>0 就是"带 alpha"，运行时 setBackgroundPic 会丢 alpha ⇒ 白块/黑块）
    total = w * h
    n_alpha = 0
    for y in range(0, h, max(1, h // 64)):
        for x in range(0, w, max(1, w // 64)):
            if px[x, y][3] < ALPHA_OPAQUE:
                n_alpha += 1
    sampled = len(range(0, h, max(1, h // 64))) * len(range(0, w, max(1, w // 64)))

    info = {"name": name, "w": w, "h": h, "mode": src_mode, "is9": is9,
            "body": body, "body_lum": body_lum, "corners": corners,
            "kind": kind, "n_trans": n_trans,
            "alpha_ratio": n_alpha / float(max(1, sampled)),
            "bytes": os.path.getsize(fp)}
    _img_cache[name] = info
    return info


# ------------------------------------------------- 源稿解析（容器底色 + 引用）
def parse_html():
    """返回 {文件: {caption: {"bg": "#RRGGBB"|None, "round": 面板名|None, "tag": 标签文本}}}"""
    out = {}
    for p in sorted(glob.glob(os.path.join(UI_DIR, "*.html"))):
        f = os.path.basename(p)
        txt = open(p, encoding="utf-8").read()
        caps = {}
        for m in re.finditer(r"<[^>]*data-caption=\"([^\"]+)\"[^>]*>", txt):
            a = m.group(0)
            bg = re.search(r'data-bg="(#[0-9A-Fa-f]{6})"', a)
            rd = re.search(r'data-round="([^"]+)"', a)
            caps[m.group(1)] = {"bg": bg.group(1).upper() if bg else None,
                                "round": rd.group(1) if rd else None}
        out[f] = caps
    return out


def collect_ctrls(path):
    """从一份 ui/*.json 里收集控件：顺序 + 窗口 + caption + 矩形 + 用到的图。

    `order` = 源稿定义顺序 —— 判断"谁在谁下面"要用它（后画的在上面）。
    """
    d = json.load(open(path, encoding="utf-8"))
    out = []
    n = [0]

    def walk(node, win, wx, wy):
        if isinstance(node, dict):
            cap = node.get("caption")
            p = node.get("position")
            # listview 的 item 子项另走一条（它们在 node["item"]["subItem"] 下）
            if isinstance(node.get("item"), dict) and node.get("cols") is not None:
                lv = node
                n[0] += 1
                out.append({"order": n[0], "win": win, "cap": lv.get("caption", "?"),
                            "x": lv["position"]["left"] + wx, "y": lv["position"]["top"] + wy,
                            "w": lv["position"]["width"], "h": lv["position"]["height"],
                            "pics": [], "captions": [], "is_list": True})
                for sv in (lv["item"] or {}).get("subItem", []):
                    sp = sv.get("position")
                    if not sp:
                        continue
                    n[0] += 1
                    pics = list((sv.get("picTab") or {}).values())
                    if sv.get("backgroundPic"):
                        pics.append(sv["backgroundPic"])
                    out.append({"order": n[0], "win": win, "cap": sv.get("caption", "?"),
                                "x": sp["left"] + wx, "y": sp["top"] + wy,
                                "w": sp["width"], "h": sp["height"],
                                "pics": [os.path.basename(x) for x in pics],
                                "sub_of": lv.get("caption"), "is_list": False})
                return
            if cap and p:
                n[0] += 1
                pics = list((node.get("picTab") or {}).values())
                if node.get("backgroundPic"):
                    pics.append(node["backgroundPic"])
                out.append({"order": n[0], "win": win, "cap": cap,
                            "x": p["left"] + wx, "y": p["top"] + wy,
                            "w": p["width"], "h": p["height"],
                            "pics": [os.path.basename(x) for x in pics],
                            "bgcolor": (node.get("bgColorTab") or {}).get("color0", -1),
                            "is_list": False})
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


def container_fill(ctrl, ctrls, caps):
    """★ 求控件所坐容器的**底色**（返回 (底色 or None, 容器 caption)）。

    ⚠️ 统一返回 **RGB 3 元组** —— `analyze()` 的 `body` 是 RGBA 4 元组，
       直接拿去 `"%02X%02X%02X" % fill` 会 TypeError（已踩）。


    顺序：源稿 data-bg → data-round 面板图中点色 → 容器自己的图本体色 → None（=窗口黑）。
    """
    cands = []
    for o in ctrls:
        if o is ctrl or o["win"] != ctrl["win"] or o["order"] >= ctrl["order"]:
            continue
        # 同格叠放的两态兄弟（SegXxx / SegXxxOn）不算"容器"
        if (o["x"], o["y"], o["w"], o["h"]) == (ctrl["x"], ctrl["y"], ctrl["w"], ctrl["h"]):
            continue
        if (ctrl["x"] >= o["x"] and ctrl["y"] >= o["y"]
                and ctrl["x"] + ctrl["w"] <= o["x"] + o["w"]
                and ctrl["y"] + ctrl["h"] <= o["y"] + o["h"]):
            cands.append(o)
    if ctrl.get("sub_of"):
        # listview 行内子项：容器 = **同 item 里那个"行底"**
        #   行底的判据 = 面积最大的那个挂图兄弟（SubMyCard 224x145 vs SubMyDic 64x64）。
        # ⚠️ 必须排除自己：`RowCamCard` 自己就是行底，排除后应回落到"几何容器"（= listview）。
        #   （不排除 ⇒ 它会"坐在自己身上"，拿自己的本体色当容器色 ⇒ 假报，2026-09-24 修。）
        sibs = [o for o in ctrls if o is not ctrl and o.get("sub_of") == ctrl["sub_of"] and o["pics"]]
        if sibs:
            bg = max(sibs, key=lambda o: o["w"] * o["h"])
            if ctrl["w"] * ctrl["h"] < bg["w"] * bg["h"]:      # 自己不是行底才用
                info = analyze(bg["pics"][0])
                if info and info["body"]:
                    return info["body"][:3], bg["cap"]
    if not cands:
        return None, None
    cont = min(cands, key=lambda o: o["w"] * o["h"])
    c = caps.get(cont["cap"], {})
    if c.get("bg"):
        h = c["bg"].lstrip("#")
        return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)), cont["cap"]
    rd = c.get("round")
    if rd and ROUND_ASSETS.get(rd):
        info = analyze(os.path.basename(ROUND_ASSETS[rd]) + ".9.png")
        if info and info["body"]:
            return info["body"][:3], cont["cap"]
    for pic in cont["pics"]:
        info = analyze(pic)
        if info and info["body"]:
            return info["body"][:3], cont["cap"]
    return None, cont["cap"]


# ================================================================= 第 1 节
def section1_black_corner():
    print("=" * 96)
    print("【1】★ 黑倒角 / 黑边专项（图角行为 × 座位底色）")
    print("=" * 96)

    html_caps = parse_html()

    # ---- 1a 全量素材的"角行为"普查（先看素材本身有没有"死黑角"）----
    print("\n[1a] 全量素材角行为普查（%d 张 PNG）" % len(glob.glob(os.path.join(IMG_DIR, "*.png"))))
    stat = Counter()
    dead, transparent = [], []
    for fp in sorted(glob.glob(os.path.join(IMG_DIR, "*.png"))):
        name = os.path.basename(fp)
        info = analyze(name)
        if not info:
            continue
        stat[info["kind"]] += 1
        if info["kind"] == "死黑角":
            dead.append(info)
        elif info["kind"] == "透明角" and info["body_lum"] > DARK_LUM:
            transparent.append(info)
    for k in ("本色角", "透明角", "死黑角"):
        print("    %-6s %4d 张" % (k, stat[k]))
    print("    ↑ 说明：透明角/死黑角**本身不是错**（坐在窗口黑底上就看不见），")
    print("      只有\"坐在非黑容器上\"才是事故 —— 见 [1b]。")

    print("\n[1b] 座位分析：坐在**非黑容器**上的挂图控件，圆角外会不会显成黑")
    print("     判据 = 控件的 `bgColorTab`（圆角外露的就是它）**必须等于容器底色**；")
    print("            坐在页面/窗口（黑）上的必须 = -1。")
    print("     容器底色来源：源稿 data-bg → data-round 面板图 → 容器自己的图本体色 → 窗口(黑)")
    print("-" * 96)
    ok_n = 0
    bad_n = 0
    scanned = 0
    seen = set()
    for jp in sorted(glob.glob(os.path.join(UI_DIR, "*.json"))):
        f = os.path.basename(jp)
        caps = html_caps.get(f.replace(".json", ".html"), {})
        try:
            ctrls = collect_ctrls(jp)
        except Exception as e:
            print("    %-14s **读不了**：%s" % (f, e))
            continue
        for c in ctrls:
            if not c["pics"]:
                continue
            fill, cont = container_fill(c, ctrls, caps)
            on_black = (fill is None) or (lum(fill) <= DARK_LUM)
            want = -1 if on_black else int("%02X%02X%02X" % fill, 16)
            for pic in c["pics"]:
                info = analyze(pic)
                if not info or info["kind"] == "本色角":
                    continue          # 本体色烘在四角 ⇒ 无关于底色，安全
                key = (f, c["cap"], pic)
                if key in seen:
                    continue
                seen.add(key)
                scanned += 1
                got = c.get("bgcolor", -1)
                # 近黑 ⇔ -1 视为等价：`#010101` 这种"几乎纯黑"在视觉上与不设底色无差别
                # （源稿刻意用 #010101 绕开 html2json 把"纯黑 0"当未设置的坑）。
                near_black_got = got < 0 or (0 <= got <= 0xFFFFFF
                                             and lum(((got >> 16) & 255, (got >> 8) & 255, got & 255)) <= DARK_LUM)
                if got == want or (want == -1 and near_black_got):
                    ok_n += 1
                    continue
                bad_n += 1
                why = ("圆角外透明 ⇒ 会露 bgColorTab；填的不是容器色" if info["kind"] == "透明角"
                       else "圆角外是**不透明黑**（画在图里的像素）⇒ 换底色也救不回来")
                print("    %-12s %-18s %-30s 坐 %-15s(底 %s)  期望 bgColorTab=%s 实际=%s  ✗ %s"
                      % (f, c["cap"], pic, cont or "?", hexs(fill) if fill else "黑",
                         hexs_rgb(want), hexs_rgb(got), why))
                hard.append(("%s / %s" % (f, c["cap"]),
                             "圆角外会显黑（%s；bgColorTab 应为 %s）" % (info["kind"], hexs_rgb(want))))
    print("    检查 %d 个『坐在非黑容器上的挂图控件』：**正确 %d**，会显黑 %d"
          % (scanned, ok_n, bad_n))
    if bad_n == 0 and scanned:
        print("    ✅ 全部正确：圆角外露的就是它所在容器的底色（弹窗/卡片上的按钮不再有黑倒角）")
    print("    说明：坐在**页面/窗口**（黑）上的控件不在本节范围内 —— 角黑 = 页面色，看不出来。")

    # ---- 1c .9.png 被 data-bgpic 引用（普通贴图路径 **不剥** marker 环）----
    print("\n[1c] 提示：`.9.png` 被 `data-bgpic` 引用（放弃九宫格机制、按普通贴图绘制）")
    print("     这类控件不受 inject_rounded 接管，尺寸/四角/边缘都得自己负责。")
    print("     建议改成 1:1 普通 PNG（尺寸==控件尺寸、圆角外烘容器色）或改用按钮 picTab。")
    print("-" * 96)
    n1c = 0
    for p in sorted(glob.glob(os.path.join(UI_DIR, "*.html"))):
        txt = open(p, encoding="utf-8").read()
        for m in re.finditer(r'data-caption="([^"]+)"[^>]*data-bgpic="([^"]+\.9\.png)"', txt, re.S):
            n1c += 1
            print("    %-14s %-20s %s" % (os.path.basename(p), m.group(1), m.group(2)))
    if n1c == 0:
        print("    （无）")
    else:
        print("    共 %d 处（历史写法，不一定是 bug；`ios_card_row.9.png` 是 1:1 尺寸的"
              "九宫格，贴上去尺寸正好，所以视觉上没问题）" % n1c)

    # ---- 1d .9.png 的 marker 环规范性 ----
    print("\n[1d] `.9.png` marker 规范（拉伸标记必须是**连续一段、居中、不压到圆角**）")
    print("-" * 96)
    n1d = 0
    for name in sorted(os.path.basename(f) for f in glob.glob(os.path.join(IMG_DIR, "*.9.png"))):
        rg = Image.open(os.path.join(IMG_DIR, name)).convert("RGBA")
        w, h = rg.size
        px = rg.load()
        bad = []
        for tag, pts in (("上", [(x, 0) for x in range(w)]),
                         ("下", [(x, h - 1) for x in range(w)]),
                         ("左", [(0, y) for y in range(h)]),
                         ("右", [(w - 1, y) for y in range(h)])):
            op = [i for i, p in enumerate(pts) if px[p][3] >= ALPHA_OPAQUE]
            if not op:
                bad.append("%s环无标记" % tag)
                continue
            if op != list(range(op[0], op[-1] + 1)):
                bad.append("%s环标记不连续" % tag)
                continue
            if op[0] + op[-1] != len(pts) - 1:
                bad.append("%s环标记不居中(%d..%d)" % (tag, op[0], op[-1]))
                continue
            if op[0] < 2 or op[-1] > len(pts) - 3:
                bad.append("%s环标记顶到角(压圆角)" % tag)
                continue
            if any(px[p][:3] != (0, 0, 0) for p in (pts[op[0]], pts[op[-1]])):
                bad.append("%s环标记不是纯黑" % tag)
        if bad:
            n1d += 1
            print("    %-32s %s" % (name, "；".join(bad)))
            soft.append((name, "marker 不规范：" + "；".join(bad)))
    if n1d == 0:
        print("    （%d 张全部合规：标记 = 居中连续一段、纯黑、且两端都离圆角 ≥2px）"
              % len(glob.glob(os.path.join(IMG_DIR, "*.9.png"))))


# ================================================================= 第 2 节
def section2_stretch():
    print()
    print("=" * 96)
    print("【2】1:1 铁律：普通 PNG（非 .9）的尺寸必须 == 控件尺寸，否则被框架拉伸")
    print("=" * 96)
    bad = 0
    ok = 0
    for jp in sorted(glob.glob(os.path.join(UI_DIR, "*.json"))):
        f = os.path.basename(jp)
        try:
            ctrls = collect_ctrls(jp)
        except Exception:
            continue
        for c in ctrls:
            for pic in c["pics"]:
                if pic.endswith(".9.png"):
                    continue
                info = analyze(pic)
                if not info:
                    print("    %-12s %-20s **图缺失** %s" % (f, c["cap"], pic))
                    hard.append((f + " / " + c["cap"], "引用了不存在的图 " + pic))
                    bad += 1
                    continue
                if (info["w"], info["h"]) != (c["w"], c["h"]):
                    print("    %-12s %-20s 图 %dx%d != 控件 %dx%d  ⇒ **会被拉伸**  %s"
                          % (f, c["cap"], info["w"], info["h"], c["w"], c["h"], pic))
                    hard.append((f + " / " + c["cap"],
                                 "1:1 不符：图 %dx%d vs 控件 %dx%d" % (info["w"], info["h"], c["w"], c["h"])))
                    bad += 1
                else:
                    ok += 1
    print("    1:1 正确 %d 处；不符 %d 处" % (ok, bad))


# ================================================================= 第 3 节
def section3_runtime_alpha():
    print()
    print("=" * 96)
    print("【3】运行时换图（setBackgroundPic）用到的图必须**不透明**（框架会丢 alpha ⇒ 白块/黑块）")
    print("=" * 96)
    names = set()
    for p in glob.glob(os.path.join(SRC_DIR, "**", "*.*"), recursive=True):
        if not p.endswith((".cc", ".cpp", ".h")):
            continue
        try:
            txt = strip_comments(open(p, encoding="utf-8", errors="replace").read())
        except OSError:
            continue
        for m in re.finditer(r'setBackgroundPic\s*\(\s*"([^"]+\.png)"', txt):
            names.add(os.path.basename(m.group(1)))
        for m in re.finditer(r'setBackgroundPic\s*\([^)]{0,80}?"([\w./-]+\.png)"', txt):
            names.add(os.path.basename(m.group(1)))
    if not names:
        print("    （源码里没找到字面量的图名；运行时图名多半是拼出来的，跳过）")
        return
    bad = 0
    for n in sorted(names):
        info = analyze(n)
        if not info:
            print("    %-32s **磁盘上没有**" % n)
            hard.append((n, "运行时引用的图不存在"))
            bad += 1
            continue
        if info["alpha_ratio"] > 0.002:
            print("    %-32s 带 alpha（透明像素占比 %.3f）⇒ 运行时换图会出白/黑块"
                  % (n, info["alpha_ratio"]))
            hard.append((n, "运行时换图 + 带 alpha → 白块/黑块"))
            bad += 1
    if bad == 0:
        print("    %d 张运行时图全部不透明 ✓" % len(names))


# ================================================================= 第 4 节
def section4_unused():
    print()
    print("=" * 96)
    print("【4】未被引用的素材（白占 /res；该分区实测只剩 ~3.5MB）")
    print("=" * 96)
    used = set()
    for p in glob.glob(os.path.join(UI_DIR, "*.html")):
        txt = open(p, encoding="utf-8").read()
        for m in re.finditer(r'data-(?:pic0|pic1|bgpic)="([^"]+\.png)"', txt):
            used.add(os.path.basename(m.group(1)))
    for p in glob.glob(os.path.join(UI_DIR, "*.json")):
        try:
            d = json.load(open(p, encoding="utf-8"))
        except Exception:
            continue
        txt = json.dumps(d)
        for m in re.finditer(r'"([\w./-]+\.png)"', txt):
            used.add(os.path.basename(m.group(1)))
    # 源码里的字面量
    src_text = []
    for p in glob.glob(os.path.join(SRC_DIR, "**", "*.*"), recursive=True):
        if p.endswith((".cc", ".cpp", ".h")):
            try:
                src_text.append(open(p, encoding="utf-8", errors="replace").read())
            except OSError:
                pass
    for t in src_text:
        for m in re.finditer(r'"([\w./-]+\.png)"', t):
            used.add(os.path.basename(m.group(1)))
    src_all = "\n".join(strip_comments(t) for t in src_text)
    tools_all = []
    for p in glob.glob(os.path.join(ROOT, "tools", "*.py")):
        try:
            tools_all.append(open(p, encoding="utf-8", errors="replace").read())
        except OSError:
            pass
    tools_all = "\n".join(tools_all)

    unused = []
    runtime_like = []
    total_all = 0
    for fp in sorted(glob.glob(os.path.join(IMG_DIR, "*.png"))):
        n = os.path.basename(fp)
        sz = os.path.getsize(fp)
        total_all += sz
        if n in used:
            continue
        stem = os.path.splitext(n)[0]
        # 运行时拼出来的名字：源码里一般写 `"images/<族>_%s_%d.png"` ⇒ 用**前缀族**判
        # （按 `_` 切段，逐级拼前缀看是否出现在源码里）。
        # 例：ios_rt_blue_106x68_c → "ios_rt_" ∈ 源码 ⇒ 运行时拼的；
        #     batt_amber_100       → "batt_"     ∈ 源码 ⇒ 运行时拼的。
        segs = stem.split("_")
        pref_hit = any(len("_".join(segs[:k])) >= 4 and ("_".join(segs[:k]) + "_") in src_all
                       for k in range(1, len(segs)))
        if pref_hit:
            runtime_like.append((n, sz))
            continue
        unused.append((n, sz))
    print("    素材总数 %d 张，合计 %.1f KB" % (len(glob.glob(os.path.join(IMG_DIR, "*.png"))),
                                          total_all / 1024.0))
    print("    疑似**运行时构造名**（源码里能找到前缀族）：%d 张 —— 不报" % len(runtime_like))
    print("    示例:", ", ".join(n for n, _ in runtime_like[:4]) or "-")
    if unused:
        print("    **未被任何地方引用**：%d 张，合计 %.1f KB" % (
            len(unused), sum(s for _, s in unused) / 1024.0))
        for n, s in unused:
            print("        %-38s %7d B" % (n, s))
        soft.append(("resources/images", "%d 张未引用素材白占 %.1f KB"
                     % (len(unused), sum(s for _, s in unused) / 1024.0)))
    else:
        print("    （没有未引用的素材）")


def main():
    only = 0
    if "--section" in sys.argv:
        only = int(sys.argv[sys.argv.index("--section") + 1])
    print("resources/ 全量审计   " + IMG_DIR)
    print("（判据：图角行为 × 座位底色；详见脚本头部注释）")
    print()
    if only in (0, 1):
        section1_black_corner()
    if only in (0, 2):
        section2_stretch()
    if only in (0, 3):
        section3_runtime_alpha()
    if only in (0, 4):
        section4_unused()

    print()
    print("=" * 96)
    if hard:
        print("硬问题 %d 项（必须修）：" % len(hard))
        for a, b in hard:
            print("    %-34s %s" % (a, b))
    if soft:
        print("软问题 %d 项（提示，可按需处理）：" % len(soft))
        for a, b in soft:
            print("    %-34s %s" % (a, b))
    if not hard and not soft:
        print("全部 PASS：没有黑倒角 / 拉伸 / 运行时 alpha / 白占空间的问题")
    print("=" * 96)
    return 1 if hard else 0


if __name__ == "__main__":
    sys.exit(main())
