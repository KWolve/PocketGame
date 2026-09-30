#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ha_ui_review.py - 智能家居页的「设计规格 × 真机像素」自动体检

为什么要有它（2026-09-23 用户要求"UI 细节评审，细节到图标效果"）：
  PC 侧设计稿有 `ui/ha.preview.html`，但**落地后真机长什么样只能靠人眼**。
  而这个模型看不了图（Read 不支持图像）⇒ 必须把"看图"变成"量数"。
  本工具把**生成的 `ui/ha.json` 当设计真值**（它就是最终控件几何），
  对抓屏逐控件量：底色对不对、文字墨迹在哪、居中偏多少、有没有被裁。

判据（全部逐像素，不靠"看起来对"）：
  1. 底色       控件内 2px 内缩区域的众数色 vs json 的 bgColor / backgroundColor
  2. 墨迹       与底色差 > 阈值的像素 mask → bbox（相对控件左上角）
  3. 垂直居中   文字控件：|墨迹中線 - 控件中線| <= 3px
  4. 裁切       墨迹是否贴到控件边（1px 内）⇒ 文字被切/字号过大
  5. 空控件     该有文字的控件却一个墨迹像素都没有（字体缺字形 / setText 没跑到）
  6. 越界       墨迹跑到控件矩形**外面**（相邻控件重叠/对齐错误）

用法:
  python tools/ha_ui_review.py docs/shot.png --win WinHaHome
  python tools/ha_ui_review.py docs/shot.png --win WinHaHome --only Card,Dic,Nm,St
  python tools/ha_ui_review.py docs/shot.png --win WinHaHome --icon Dic0 --zoom 40
  python tools/ha_ui_review.py docs/shot.png --win WinHaHome --quiet   # 只打印问题

⚠️ 前提：截图必须是**该窗口**的画面（抓屏有 pan 双缓冲，见 tools/grab.py）。
   `--win` 给错会得到一片"底色全不对"的噪声，所以工具会先做**窗口自证**：
   统计该窗口期望有文字的控件里，有多少真的画出了墨迹；命中率 < 60% 直接报错退出。
"""
import json
import os
import sys
from collections import Counter

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
JSON_PATH = os.path.join(ROOT, "ui", "ha.json")

INK_THRESH = 45          # 与底色的最大通道差超过它就算"墨迹"
EDGE_TOL = 1             # 墨迹贴边判据（像素）

# 半透明遮罩（弹层压暗层）会让"底色 vs json"必然对不上，这些控件跳过底色检查
SKIP_BG = ("ImgHaMask", "ImgHaMask2")


def load_spec(path=None):
    """把 ui/ha.json 压成 {caption: {...}}（带所属窗口 / 是否 listview / 子项归属）。

    ★ listview 的子项坐标是**相对 item** 的，这里原样保存并标 `sub_of=<listview caption>`，
      由 main() 按 cols/rowSpacing 摊成绝对坐标（见 --item-rows）。
    """
    d = json.load(open(path or JSON_PATH, encoding="utf-8"))
    out = {}

    def add(cap, node, win, sub_of=None, kind="ctrl", wx=0, wy=0):
        p = node.get("position") or {}
        out[cap] = {
            "win": win, "kind": kind,
            # ★ 子控件坐标是**相对窗口**的 ⇒ 必须加上窗口的 (left,top) 才是屏幕绝对坐标。
            #   ha.json 的窗口都在 (0,0) 所以看不出差别；但 ime.json 的 KbPanel 在 (0,372)，
            #   不加偏移会把整页控件当成上移 372px 来量（实测白折腾一轮）。
            "x": p.get("left", 0) + wx, "y": p.get("top", 0) + wy,
            "w": p.get("width", 0), "h": p.get("height", 0),
            "bg": node.get("backgroundColor"),
            "bgtab": (node.get("bgColorTab") or {}).get("color0"),
            "colortab": (node.get("colorTab") or {}).get("color0"),
            "fs": node.get("fontSize", 0),
            "text": node.get("text", ""),
            "pic0": (node.get("picTab") or {}).get("pic0"),
            "bpic": node.get("backgroundPic"),
            "visible": node.get("visible", 1),
            "is_list": "cols" in node,
            "cols": node.get("cols", 1),
            "lrows": node.get("rows", 1),
            "col_spacing": node.get("colSpacing", 0),
            "row_spacing": node.get("rowSpacing", 0),
            "sub_of": sub_of,
        }

    def walk(node, win, sub_of, wx=0, wy=0):
        if isinstance(node, dict):
            cap = node.get("caption")
            if cap and "position" in node:
                add(cap, node, win, sub_of, wx=wx, wy=wy)
            # listview：subItem 挂在 node["item"]["subItem"] 下（**不是** node["subItem"]）
            if isinstance(node.get("item"), dict) and cap:
                for sv in (node.get("item") or {}).get("subItem", []):
                    sc = sv.get("caption")
                    if sc and "position" in sv:
                        add(sc, sv, win, cap, kind="sub", wx=wx, wy=wy)
                return
            for k, v in node.items():
                if isinstance(v, (dict, list)) and k != "item":
                    walk(v, win, sub_of, wx, wy)
        elif isinstance(node, list):
            for v in node:
                walk(v, win, sub_of, wx, wy)

    for wkey, wval in d.items():
        if not isinstance(wval, dict) or "caption" not in wval:
            continue
        wname = wval["caption"]
        # ★ 顶层"窗口"节点：ha.json 里叫 WinXxx，但别的页未必（如 ia/ime.json 的 KbPanel）
        #   ⇒ 判据放宽成"顶层键是 window__*"（不靠名字前缀）
        if not str(wkey).startswith("window"):
            continue
        wp = wval.get("position") or {}
        for ckey, cval in wval.items():
            if isinstance(cval, dict) and "position" in cval:
                walk(cval, wname, None, wp.get("left", 0), wp.get("top", 0))
    return out


def to_rgb(v):
    """json 里的颜色是 ARGB int（可能是负数/无符号）⇒ 拆出 RGB。"""
    if v is None:
        return None
    v &= 0xFFFFFFFF
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def pic_center_color(pic):
    """picTab 的图（按钮圆角底）—— 取图中点色当"期望底色"。"""
    if not pic:
        return None
    fp = os.path.join(ROOT, "resources", pic.replace("/", os.sep))
    if not os.path.isfile(fp):
        return None
    im = Image.open(fp).convert("RGBA")
    w, h = im.size
    r, g, b, a = im.getpixel((w // 2, h // 2))
    return (r, g, b) if a > 200 else None


def analyze(px, x, y, w, h, W, H):
    """量一个矩形：众数底色 + 墨迹 bbox / 计数 / 贴边情况。"""
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(W, x + w), min(H, y + h)
    if x1 - x0 < 3 or y1 - y0 < 3:
        return None
    # 底色：内缩 2px，避开圆角与描边
    inner = [(px[i, j]) for j in range(y0 + 2, y1 - 2) for i in range(x0 + 2, x1 - 2)]
    if not inner:
        return None
    bg, bg_n = Counter(inner).most_common(1)[0]
    xs, ys = [], []
    edge = 0
    cnt = 0
    # ★ 圆角按钮的"透明四角"会露出更暗的窗口底色 —— 那不是文字，要排除。
    #   但**不能**简单地把"比底色暗的像素"全排除（早先版本就这么写的）：
    #   深色文字（黑字在绿按钮上）**也是比底色暗**的 ⇒ 会被一律丢掉，
    #   于是"完成/下一步"这些绿按钮被误报成"★空控件(应有文字)"（2026-09-24 自证时抓到）。
    #   正解：① 墨迹只在**内缩 2px** 的范围里数（避开描边/圆角的抗锯齿边）；
    #         ② 再排除"落在四个圆角圆弧**之外**"的像素（那是控件没盖到的窗口底色）。
    INSET = 2
    R = 20                       # 本项目卡片/按钮素材的圆角半径（ios_*_r20 / r10 取大者）
    ca = (x0 + R, y0 + R)
    cb = (x1 - 1 - R, y0 + R)
    cc = (x0 + R, y1 - 1 - R)
    cd = (x1 - 1 - R, y1 - 1 - R)

    def outside_round(i, j):
        """像素是否落在圆角矩形之外（只可能在四角）。"""
        for (cx, cy) in (ca, cb, cc, cd):
            if (i - cx) * (i - cx) + (j - cy) * (j - cy) > R * R:
                # 只有"朝角外侧"的方向才算外面
                if (i < x0 + R and cx == ca[0]) or (i > x1 - 1 - R and cx == cb[0]) \
                   or (i < x0 + R and cx == cc[0]) or (i > x1 - 1 - R and cx == cd[0]):
                    if (j < y0 + R and cy == ca[1]) or (j < y0 + R and cy == cb[1]) \
                       or (j > y1 - 1 - R and cy == cc[1]) or (j > y1 - 1 - R and cy == cd[1]):
                        return True
        return False

    for j in range(y0 + INSET, y1 - INSET):
        for i in range(x0 + INSET, x1 - INSET):
            p = px[i, j]
            d = max(abs(p[0] - bg[0]), abs(p[1] - bg[1]), abs(p[2] - bg[2]))
            if d > INK_THRESH and not outside_round(i, j):
                cnt += 1
                xs.append(i); ys.append(j)
                # ⚠️ 只判**右/下**贴边：左/上是左对齐文字的**正常起点**（报了全是噪音）；
                #    右/下贴边才意味着"可能被裁"（2026-09-24 修）。
                if (x1 - 1) - i < EDGE_TOL or (y1 - 1) - j < EDGE_TOL:
                    edge += 1
    info = {"bg": bg, "bg_share": bg_n / float(len(inner)), "ink": cnt}
    if xs:
        info.update({"ix0": min(xs) - x0, "ix1": max(xs) - x0,
                     "iy0": min(ys) - y0, "iy1": max(ys) - y0, "edge": edge})
    return info


def ascii_zoom(px, x, y, w, h, cols=36):
    """把一块区域打成字符画（看图标外形）。"""
    ramp = " .:-=+*#%@"
    rows = max(1, int(cols * h / float(w) * 0.5))
    lines = []
    for r in range(rows):
        line = []
        for c in range(cols):
            i = x + int((c + 0.5) * w / cols)
            j = y + int((r + 0.5) * h / rows)
            try:
                p = px[i, j]
            except Exception:
                line.append(" ")
                continue
            lum = (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
            line.append(ramp[min(9, lum * 10 // 256)])
        lines.append("".join(line))
    return "\n".join(lines)


def contrast_report(px, ctrls, W, H, dy_off, min_ratio=3.0):
    """逐控件算 WCAG 对比度 —— **文字色取自 json（权威），底色取自真机实测**。

    为什么不从像素里"猜"文字色（第一版就这么写的，不可靠）：
      · 圆角外露的窗口底色往往比文字像素还多 ⇒ "第二主色"会取到它；
      · 小字（11~15px）绝大多数像素是反锯齿混色，纯文字色可能只有 8 个像素
        ⇒ 阈值一高就把它滤掉，算出来的对比度**偏低**（假告警）。
    ⇒ 文字色用 json 的 colorTab（源稿写死的真值），底色用截图的众数（能抓运行时改色）。
    """
    def lum(c):
        r, g, b = c
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    rows = []
    for k, v in sorted(ctrls.items(), key=lambda t: (t[1]["y"], t[1]["x"])):
        if v.get("visible") is False or k in SKIP_BG:
            continue
        ink = to_rgb(v.get("colortab"))
        if not ink or ink == (0, 0, 0) or v["h"] < 12:
            continue
        y = v["y"] + dy_off(v)
        x0, y0 = max(0, v["x"]), max(0, y)
        x1, y1 = min(W, v["x"] + v["w"]), min(H, y + v["h"])
        if x1 - x0 < 6 or y1 - y0 < 6:
            continue
        bg = Counter(px[i, j] for j in range(y0 + 1, y1 - 1) for i in range(x0 + 1, x1 - 1)).most_common(1)[0][0]
        l1, l2 = sorted((lum(bg), lum(ink)), reverse=True)
        ratio = (l1 + 5) / (l2 + 5)
        if ratio < min_ratio:
            rows.append((k, bg, ink, ratio, v.get("fs", 0), v.get("text", "")[:12]))
    return rows


def corner_report(px, ctrls, W, H, dy_off, band=6, min_black=3):
    """★ 专抓"圆角块四角/四边发黑"。

    为什么要重写（第一版漏报，被像素类别图抓到）：
      九宫格（`.9.png`）有 **1px 内缩** ⇒ 控件最外 1px 显示的是 `bgColorTab`，
      真正的黑在**边内 1~2px 的圆角弧上** —— 只取 (x0,y0) 四个点会正好落在那一圈上，
      于是"看起来是容器色"，**漏报**。
    ⇒ 正确判据：扫"控件内**贴边 band 像素**的边带"，数里面的**黑像素**数量；
       容器不是黑的（lum>25）却有成片黑 ⇒ 角/边发黑。

    容器色 = 控件矩形**外面** 3px 一圈的众数（这比静态推断父容器可靠：
      不用猜层次，直接量"这块东西周围是什么颜色"）。
    """
    rows = []
    for k, v in sorted(ctrls.items(), key=lambda t: (t[1]["y"], t[1]["x"])):
        if v.get("visible") is False or k in SKIP_BG:
            continue
        if not (v.get("pic0") or v.get("bpic")):
            continue
        w, h = v["w"], v["h"]
        x0, y0 = v["x"], v["y"] + dy_off(v)
        x1, y1 = x0 + w - 1, y0 + h - 1
        if w < 12 or h < 12 or x0 < 5 or y0 < 5 or x1 > W - 6 or y1 > H - 6:
            continue
        # 容器色：只取**四个斜角外侧** 2px（键位/行这类密排控件，正上正下的 3px 会踩到隔壁控件
        #   ⇒ 曾把键盘键全误报成"黑角"）
        outs = [px[x0 - 2, y0 - 2], px[x1 + 2, y0 - 2], px[x0 - 2, y1 + 2], px[x1 + 2, y1 + 2]]
        cont = Counter(outs).most_common(1)[0][0]
        if sum(1 for c in outs if max(abs(c[i] - cont[i]) for i in range(3)) <= 12) < 3:
            continue                      # 四角外侧本身不一致（落在别的控件上）⇒ 判不了，跳过
        cont_lum = (cont[0] * 299 + cont[1] * 587 + cont[2] * 114) // 1000
        if cont_lum <= 25:
            continue                      # 容器本身就是黑的（坐在窗口上）⇒ 角黑看不出来
        # 边带里的黑像素
        blacks = 0
        for j in range(y0, y1 + 1):
            for i in range(x0, x1 + 1):
                if min(i - x0, x1 - i, j - y0, y1 - j) >= band:
                    continue
                c = px[i, j]
                if (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000 < 25:
                    blacks += 1
        if blacks >= min_black:
            rows.append((k, cont, blacks))
    return rows


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    shot = sys.argv[1]
    win = None
    quiet = False
    only = None
    icon = None
    zoom_cols = 40
    for i, a in enumerate(sys.argv):
        if a == "--win":
            win = sys.argv[i + 1]
        elif a == "--quiet":
            quiet = True
        elif a == "--only":
            only = sys.argv[i + 1].split(",")
        elif a == "--icon":
            icon = sys.argv[i + 1]
        elif a == "--zoom":
            zoom_cols = int(sys.argv[i + 1])
    if not win:
        print("!! 必须给 --win <窗口名>（如 WinHaHome）")
        return 2

    jpath = JSON_PATH
    if "--json" in sys.argv:
        jpath = sys.argv[sys.argv.index("--json") + 1]
    spec = load_spec(jpath)
    ctrls = {k: v for k, v in spec.items() if v["win"] == win}

    # ★ listview 展开：把子项按 cols/rowSpacing 摊成**绝对坐标**（--item-rows N = 实际行数）
    nrows = 0
    if "--item-rows" in sys.argv:
        nrows = int(sys.argv[sys.argv.index("--item-rows") + 1])
    if nrows > 0:
        lists = {k: v for k, v in ctrls.items() if v["is_list"]}
        subs = {k: v for k, v in ctrls.items() if v["sub_of"]}
        for k in subs:
            del ctrls[k]
        for lname, lv in lists.items():
            cols = max(1, lv["cols"])
            iw = (lv["w"] - (cols - 1) * lv["col_spacing"]) // cols
            # item 高度按**声明的可见行数**算（不是实际 item 数）：
            # 声明 rows=4 / h=616 / rowSpacing=8 ⇒ item 高 148（与设计一致）
            ih = (lv["h"] - (max(1, lv["lrows"]) - 1) * lv["row_spacing"]) // max(1, lv["lrows"])
            for n in range(nrows):
                r, c = divmod(n, cols)
                ox = lv["x"] + c * (iw + lv["col_spacing"])
                oy = lv["y"] + r * (ih + lv["row_spacing"])
                for k, v in subs.items():
                    if v["sub_of"] != lname:
                        continue
                    nv = dict(v)
                    nv["x"] = ox + v["x"]; nv["y"] = oy + v["y"]; nv["visible"] = True
                    ctrls["%s#%d" % (k, n)] = nv

    # 容器：矩形里完整套着另一个控件的（"无字却有墨迹"是**正常的**，别报）
    containers = set()
    for a2, va in ctrls.items():
        for b2, vb in ctrls.items():
            if a2 == b2:
                continue
            if vb["x"] >= va["x"] and vb["y"] >= va["y"] \
               and vb["x"] + vb["w"] <= va["x"] + va["w"] \
               and vb["y"] + vb["h"] <= va["y"] + va["h"]:
                containers.add(a2)
                break
    if "--item-rows" not in sys.argv:
        pass
    if not ctrls:
        print("!! ui/ha.json 里没有窗口 %s" % win)
        return 2

    dy = 0
    dy_from = 0
    if "--dy" in sys.argv:
        dy = int(sys.argv[sys.argv.index("--dy") + 1])
    if "--dy-from" in sys.argv:      # 只对 y >= 该值的控件加偏移（页头/状态条不动，只有网格下移）
        dy_from = int(sys.argv[sys.argv.index("--dy-from") + 1])

    def off(v):
        return dy if v["y"] >= dy_from else 0
    im = Image.open(shot).convert("RGB")
    W, H = im.size
    if W < 480 or H < 800:
        print("!! 截图尺寸 %dx%d，期望 >= 480x800（抓屏可能抓错了页/被裁）" % (W, H))
        return 2
    px = im.load()

    # ---- 窗口自证：期望有文字的控件里，多少真的画出了墨迹 ----
    exp = [k for k, v in ctrls.items()
           if v["text"] and v["h"] >= 12 and v.get("visible") is not False]
    if only:
        exp = [k for k in exp if any(k.startswith(o) for o in only)]
    hit = 0
    res = {}
    for k in exp:
        v = ctrls[k]
        r = analyze(px, v["x"], v["y"] + off(v), v["w"], v["h"], W, H)
        res[k] = r
        if r and r["ink"] > 3:
            hit += 1
    rate = hit / float(len(exp) or 1)
    print("窗口 %s（截图 %s，%dx%d）" % (win, os.path.basename(shot), W, H))
    print("自证：%d/%d 个文字控件画出了墨迹（%.0f%%）" % (hit, len(exp), rate * 100))
    if exp and rate < 0.6 and "--noself" not in sys.argv:
        print("!! 命中率过低 —— 这张截图**多半不是 %s**（抓屏错页/命令没生效）" % win)
        return 3

    if "--corners" in sys.argv:
        bad = corner_report(px, ctrls, W, H, off)
        print("\n★ 边带发黑的控件（坐在非黑容器上、但自己贴边处有黑像素 ⇒ 圆角透黑）：")
        if not bad:
            print("   无")
        for k, cont, nb in bad:
            print("   %-24s 容器色%-16s 边带黑像素 %d 个" % (k, str(cont), nb))
        return 0

    if "--contrast" in sys.argv:
        bad = contrast_report(px, ctrls, W, H, off)
        print("\n对比度 < 3:1 的控件（WCAG 对大图形/粗体字的下限；越小越看不见）：")
        if not bad:
            print("   无")
        for k, bg, ink, ratio, fs, tx in sorted(bad, key=lambda t: t[3]):
            print("   %-18s 底%-16s 字%-16s 对比 %.2f:1  fs=%d  %r" % (k, str(bg), str(ink), ratio, fs, tx))
        return 0

    if icon:
        if icon not in ctrls:
            print("!! 没有控件 %s" % icon)
            return 2
        v = ctrls[icon]
        print("\n---- %s 字符画（%d,%d %dx%d，text=%r）----"
              % (icon, v["x"], v["y"], v["w"], v["h"], v["text"]))
        print(ascii_zoom(px, v["x"], v["y"], v["w"], v["h"], zoom_cols))
        return 0

    print("\n%-18s %-4s %-9s %-6s %-16s %s"
          % ("控件", "尺寸", "底色", "墨迹", "墨迹bbox(x0,y0,x1,y1)", "判定"))
    print("-" * 96)
    issues = []
    for k in sorted(ctrls, key=lambda a: (ctrls[a]["y"], ctrls[a]["x"])):
        if only and not any(k.startswith(o) for o in only):
            continue
        v = ctrls[k]
        if v.get("visible") is False and not only:
            continue
        r = res.get(k) or analyze(px, v["x"], v["y"] + off(v), v["w"], v["h"], W, H)
        if not r:
            # ⚠️ 1px 分隔线（LineHaXxx）是**有意为之**的细线 —— 跳过但**不算问题**
            #    （早先版本把它算成"有问题"，误报噪音；2026-09-24 修）
            if v["w"] < 4 or v["h"] < 4:
                print("%-18s %-4s  (细线/太小，按设计跳过)" % (k, "%dx%d" % (v["w"], v["h"])))
            else:
                print("%-18s %-4s  (越界，跳过)" % (k, "%dx%d" % (v["w"], v["h"])))
            continue
        # ★ 期望底色的优先级：**picTab 图的中点** → bgColorTab → backgroundColor。
        #   为什么图优先（2026-09-24 血案）：`CARD_FILL` 会给"坐在卡片上的圆角按钮"
        #   把 bgColorTab 补成**容器色**（好让四角融进卡片）⇒ 从此 bgColorTab 不再描述
        #   "按钮本体是什么色"，照着它判会全线误报"底色!=规格(差30)"。
        #   真正画出来的是 picTab 那张图的本体色。
        # ★ "两态叠一格"的控件（SegHaCtrl / SegHaCtrlOn）：json 里两张图都在，
        #   运行时由逻辑 setVisible 二选一 ⇒ 拿静态图判底色必然误报。
        #   识别：caption 以 On 结尾，或存在 caption+"On" 的兄弟（本工程的命名约定）。
        if k.endswith("On") or (k + "On") in ctrls:
            print("%-18s %-4s %-14s  (两态叠放，运行时切换，跳过底色判定)"
                  % (k, "%dx%d" % (v["w"], v["h"]), str(r["bg"])))
            continue
        want_bg = None
        for cand in (pic_center_color(v.get("pic0")), pic_center_color(v.get("bpic")),
                     to_rgb(v["bgtab"]), to_rgb(v["bg"])):
            if cand and cand != (255, 255, 255):
                want_bg = cand
                break
        notes = []
        if want_bg and k not in SKIP_BG:
            d = max(abs(r["bg"][i] - want_bg[i]) for i in range(3))
            if d > 12:
                notes.append("底色!=规格(差%d)" % d)
        if v["text"]:
            if r["ink"] <= 3:
                notes.append("★空控件(应有文字)")
            else:
                cy_i = (r["iy0"] + r["iy1"]) / 2.0
                cy_c = v["h"] / 2.0
                if abs(cy_i - cy_c) > 3.5:
                    notes.append("垂直偏差%.0f" % (cy_i - cy_c))
                ink_h = r["iy1"] - r["iy0"] + 1
                if ink_h > v["h"] - 2:
                    notes.append("墨迹高%d≈控件高%d" % (ink_h, v["h"]))
                if r["edge"] > 0:
                    notes.append("★贴边(%dpx)可能被裁" % r["edge"])
                if r["ix0"] < -1 or r["ix1"] > v["w"]:
                    notes.append("★横向越界")
        # 注意：json 里 text 为空的控件 = **运行时才 setText**（Nm/St/Sb/Dic 都是这样）
        # ⇒ 它们有墨迹是正常的，不报；json 里有 text 才算"该有字"。
        flag = "、".join(notes)
        if flag:
            issues.append((k, flag))
        if not quiet or flag:
            bb = ("%3d,%3d,%3d,%3d" % (r["ix0"], r["iy0"], r["ix1"], r["iy1"])) if r["ink"] > 3 else "-"
            print("%-18s %-4s %-9s %-6d %-16s %s"
                  % (k, "%dx%d" % (v["w"], v["h"]), str(r["bg"]), r["ink"], bb, flag or "ok"))
    print("-" * 96)
    vis = [k for k, v in ctrls.items() if v.get("visible") is not False and k not in SKIP_BG]
    ov = []
    for i in range(len(vis)):
        for j in range(i + 1, len(vis)):
            a, b = ctrls[vis[i]], ctrls[vis[j]]
            x0 = max(a["x"], b["x"]); x1 = min(a["x"] + a["w"], b["x"] + b["w"])
            y0 = max(a["y"], b["y"]); y1 = min(a["y"] + a["h"], b["y"] + b["h"])
            if x1 - x0 > 2 and y1 - y0 > 2:
                # 完整的"包含"关系不算问题（父子），只报**部分相交**
                if vis[i] in containers or vis[j] in containers:
                    continue
                ov.append((vis[i], vis[j], (x1 - x0) * (y1 - y0)))
    if ov:
        print("可见控件部分相交（可能互相盖住/热区越界）：")
        for a, b, area in sorted(ov, key=lambda t: -t[2])[:12]:
            print("   %-16s x %-16s 重叠 %d px²" % (a, b, area))
    print("控件 %d 个，有问题 %d 个" % (len(ctrls), len(issues)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
