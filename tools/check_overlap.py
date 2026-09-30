#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_overlap.py - 控件盒重叠检查（专抓"文字被实心件压住"这类看不见的遮挡）

为什么需要它（2026-09-16）：
    用户反馈「**IP 地址被开关按键覆盖到了**」—— 量的结果是 `TextWifiIp` 盒
    `x=280..432` 与滑轨开关 `CbWifiOn`（`x=376..448`）**部分重叠** 56px。
    这类问题的麻烦在于：**抓屏查不出来**。被盖住的内容在屏幕上就是"没有"，
    你不知道本该有什么；只有"知道这里应该有一段字"的人才能发现。
    ⇒ 必须在**生成阶段**用盒模型算出来。

判据（关键：区分"正常叠放"与"意外遮挡"）：
    · **完整包含**（一方把另一方整个罩住，或两者完全重合）→ **正常**。
      卡片/面板底图、四档信号图叠在同一位置靠 visible 切换，都是这种。
    · **部分重叠**（有交集但互不包含）→ **一定互相遮挡，报出来**。
      这正是"IP 被开关压住"的形态。

优先级：
    [高] 文字 × 实心件（有 backgroundPic / picTab / backgroundColor）的部分重叠
         —— 文字会被实心件盖住，用户看不到。
    [高] 文字 × 文字 的部分重叠 —— 两段字糊在一起。
    [低] 实心件 × 实心件 的部分重叠 —— 可能是刻意的图形拼接，供人工判断。

用法：
    python tools/check_overlap.py            # 扫 ui/*.json
    python tools/check_overlap.py -v         # 列出全部（默认只列前若干）
退出码：0 = 无问题，1 = 有"高"优先级问题（可直接进 CI / 打包前置检查）

⚠️ 2026-09-19：递归 bug 修好后，**之前从来没被检查过的层**一下子全暴露了
   （本工程 140 处"高"优先级），其中有一大类是**设计如此、不是 bug**：
   · **列表行卡片**：`RowCard`（可点的行底图，声明在前）上面叠着
     `SubStaName/Info/Note`（行内文字，声明在后）—— 这正是"整行可点"的写法，
     文字在 `onUI_init` 里补了 `setTouchPass(true)`
     （本脚本**只读 json，看不到运行期行为**）。
   · **满屏视频/画布 + 置于其上的提示文字**（camera 页）。
   ⇒ 判据：**顺着它报出的名字去看那一屏图**，能看见就是设计，看不见才是 bug。
   （修前本脚本对 radio 页**整页零检查**却输出"没问题"、退出码 0。）

⚠️ 已知豁免（**别按提示去缩盒**）：`[触摸]` 那类里有 2 处是 settings 的行内数值文本
   （`TextSetSoundVal` / `TextSetWifiVal`）压在整行按钮上。**设计如此**（数值显示在行右端），
   正确修法是运行期给它们 `setTouchable(false)+setTouchPass(true)`（见 settings.cc::onUI_init
   与 MCP `uicontrols/touch-events.md`）——而本脚本只读 json，**看不到运行期行为**，
   所以这两条会一直在。判据：**json 里缩不动、又是装饰文字的，就看代码有没有补 touchPass**。
"""
import os
import sys
import glob
import json

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
UI_DIR = os.path.join(ROOT, "ui")


def _rect(n):
    p = n.get("position") or {}
    try:
        x, y = int(p.get("left", 0)), int(p.get("top", 0))
        w, h = int(p.get("width", 0)), int(p.get("height", 0))
    except (TypeError, ValueError):
        return None
    return (x, y, x + w, y + h)


def _is_visible(n):
    return n.get("visible", True) is not False


def _kind(n):
    """控件分类：text（纯文字）/ solid（有图或底色）/ empty（啥都没有）"""
    has_text = bool(n.get("text"))
    has_pic = bool(n.get("picTab")) or bool(n.get("backgroundPic")) or bool(n.get("iconPosition"))
    has_bg = n.get("backgroundColor") is not None
    if has_pic or has_bg:
        return "solid"
    if has_text:
        return "text"
    # 没有 text 字段但有 colorTab/fontSize 的 textview（内容是逻辑层运行时写的）
    if "fontSize" in n or "colorTab" in n:
        return "text"
    return "empty"


def _inter(a, b):
    x0, y0 = max(a[0], b[0]), max(a[1], b[1])
    x1, y1 = min(a[2], b[2]), min(a[3], b[3])
    if x1 <= x0 or y1 <= y0:
        return None
    return (x0, y0, x1, y1)


def _contains(outer, inner):
    return outer[0] <= inner[0] and outer[1] <= inner[1] and \
           outer[2] >= inner[2] and outer[3] >= inner[3]


# 对齐码（html2json.py: left/center/right → 36/37/38）
ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT = 36, 37, 38


def _text_rect(n, r):
    """按「文本内容 + 字号 + 对齐方式」估算文字**实际占据**的矩形（而不是控件盒）。

    为什么必须这样（2026-09-16 实测）：**居中标题的控件盒常常是整屏宽**
    （`TextWifiTitle` 盒 `x=0..480`，文字却只占中间 ~110px）。
    直接拿盒宽判重叠，会把"标题 vs 左右按钮"全报成问题（第一版实测 2 处误报）。

    实测标定（真机墨迹实测反推）：
      · 中文字符宽 ≈ fontSize（方框字：中文 1:1 占位）
      · ASCII 字符宽 ≈ fontSize × 0.5
      验证：「无线局域网」fs=22 → 估 110px，真机墨迹实测 **108px** ✓
            "192.168.1.100" fs=13 → 估 85px，实测 **89px** ✓

    返回 None 表示"内容由逻辑层运行时写、无法估算" ⇒ 调用方退回盒宽（保守）。
    """
    txt = n.get("text") or ""
    if not txt:
        return None
    fs = int(n.get("fontSize") or 14)
    w = 0.0
    for ch in txt:
        w += fs * (1.0 if ord(ch) > 0x2E80 else 0.5)
    w = int(w) + 2
    x0, y0, x1, y1 = r
    if x1 - x0 <= w:
        return r                      # 文字比盒子还宽（会被裁）⇒ 用盒宽
    al = n.get("alignment")
    if al == ALIGN_CENTER:
        cx = (x0 + x1) // 2
        return (cx - w // 2, y0, cx + w // 2, y1)
    if al == ALIGN_RIGHT:
        return (x1 - w, y0, x1, y1)
    return (x0, y0, x0 + w, y1)


def _eff_rect(n):
    """判重叠用的有效矩形：文字控件用**估算的文字实占区**，其余用控件盒。"""
    r = _rect(n)
    if r is None:
        return None
    if _kind(n) == "text":
        return _text_rect(n, r) or r
    return r


def collect_sibling_groups(node, out, path="root"):
    """递归收集"每个容器下的直接子控件"分组。

    ⚠️ 只比**同一父级**的兄弟：跨层级的遮挡（弹窗盖住下层列表）是框架的正常行为。

    ★★★ 2026-09-19 血案（本函数曾经**整页漏查**）：
        `ui/*.json` 的层级是 `root → window__N → button__M / textview__K ...`，
        而 **window 自己也是"带 position + caption 的控件"**。旧版是这样写的：

            for k, v in node.items():
                if isinstance(v, dict) and "position" in v and "caption" in v:
                    kids.append(v)                       # ← window 被当成"兄弟控件"
                elif isinstance(v, (dict, list)):
                    collect_sibling_groups(v, ...)
            ...
            for k, v in node.items():
                if ... and not (isinstance(v, dict) and "position" in v):
                    collect_sibling_groups(v, ...)       # ← 带 position 的**永不递归**

        ⇒ window 收进 kids 之后就**再也没往下走过**，window 里的几十个控件
          （radio 页 54 个）**一个都没进过比较**。工具只报 root 那一层，
          **整页零检查**，而且退出码还是 0 —— 看着"检查通过"，实际什么都没查。
          表现：VU 表盘把"电台时间/总电平条"整条盖住，脚本却说没问题（用户先发现的）。

        判据：**"容器自身是控件"不等于"它没有子控件"** ⇒ 递归与"它是不是控件"必须解耦。
        自检：本工程 radio.json 现在应产出 ≥1 个兄弟组，且**组内成员数 == 54**（旧版只有 1 组 2 个）。
    """
    if isinstance(node, dict):
        kids = [v for v in node.values()
                if isinstance(v, dict) and "position" in v and "caption" in v]
        if len(kids) >= 2:
            out.append((path, kids))
        # ★ 无论本节点自身是不是控件，都要继续往下找"下一层的兄弟组"
        for k, v in node.items():
            if k == "position":
                continue
            if isinstance(v, (dict, list)):
                collect_sibling_groups(v, out, path + "/" + str(k))
    elif isinstance(node, list):
        # 列表形式的一层子控件也是兄弟组（listview 的 item 模板等）
        kids = [v for v in node
                if isinstance(v, dict) and "position" in v and "caption" in v]
        if len(kids) >= 2:
            out.append((path, kids))
        for i, v in enumerate(node):
            collect_sibling_groups(v, out, path + "[%d]" % i)


def check_file(path, verbose):
    try:
        data = json.load(open(path, encoding="utf-8"))
    except Exception as e:                                     # noqa: BLE001
        print("  !! 读取失败 %s: %s" % (os.path.basename(path), e))
        return []
    groups = []
    collect_sibling_groups(data, groups)
    found = []
    for gpath, kids in groups:
        live = [k for k in kids if _is_visible(k) and _rect(k)]
        # ---------- ① 视觉重叠（按"文字实占区"判，否则居中标题会误报）----------
        for i in range(len(live)):
            for j in range(i + 1, len(live)):
                a, b = live[i], live[j]
                ra, rb = _eff_rect(a), _eff_rect(b)
                if not ra or not rb:
                    continue
                it = _inter(ra, rb)
                if not it:
                    continue
                # 完整包含 / 完全重合 → 正常叠放，跳过
                if _contains(ra, rb) or _contains(rb, ra):
                    continue
                ka, kb = _kind(a), _kind(b)
                if ka == "empty" and kb == "empty":
                    continue
                sev = "高" if "text" in (ka, kb) else "低"
                area = (it[2] - it[0]) * (it[3] - it[1])
                found.append(dict(
                    kind="视觉", file=os.path.basename(path), group=gpath, sev=sev, area=area,
                    a=a.get("caption"), ka=ka, ra=ra,
                    b=b.get("caption"), kb=kb, rb=rb, inter=it))

        # ---------- ② 触摸遮挡（按**控件盒**判，与视觉无关）----------
        # ★★ 为什么必须单独查（2026-09-16 血案）：用户报「左上角返回图标不可用」。
        #    根因 = 整屏宽的居中标题 **声明在返回按钮之后**（= 在更上层），
        #    它的盒 (0..480) 盖住了返回按钮 (8..52) ⇒ 按钮收不到点击。
        #    ⚠️ `touchable=false` **只让它不响应触摸，并不让出命中区**
        #      （与"整屏 SysApp 浮层不吃穿透"同一机制）⇒ 纯装饰控件也必须缩盒。
        #    ⚠️⚠️ 这类问题**视觉上完全看不出来**：文字实占区在正中间，看着谁也没挡；
        #      而且同容器的刷新按钮（声明在标题**之后**）还是好的 —— 极易误判成"按钮坏了"。
        #    判据：下层的**可触摸**控件，被上层（后声明）控件的**盒**覆盖 → 它的点击收不到。
        for i in range(len(live)):
            for j in range(i + 1, len(live)):
                low, up = live[i], live[j]          # 后声明 = 更上层
                if low.get("touchable", True) is False:
                    continue                        # 下层本来就不可触摸 ⇒ 无所谓
                rl, ru = _rect(low), _rect(up)
                it = _inter(rl, ru)
                if not it:
                    continue
                area = (it[2] - it[0]) * (it[3] - it[1])
                found.append(dict(
                    kind="触摸", file=os.path.basename(path), group=gpath, sev="高", area=area,
                    a=low.get("caption"), ka="可点", ra=rl,
                    b=up.get("caption"), kb="上层", rb=ru, inter=it))
    return found


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    # 支持指定路径（目录或单个 json）—— 便于"拿改前的 json 验证工具能不能抓到"这类回归
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if args:
        files = []
        for a in args:
            if os.path.isdir(a):
                files += sorted(glob.glob(os.path.join(a, "*.json")))
            elif os.path.isfile(a):
                files.append(a)
    else:
        files = sorted(glob.glob(os.path.join(UI_DIR, "*.json")))
    if not files:
        print("没找到 ui/*.json —— 先跑 gen_ui.py")
        return 1

    allhits = []
    for f in files:
        allhits += check_file(f, verbose)

    print("=" * 78)
    print("  控件盒重叠检查   %s" % UI_DIR)
    print("=" * 78)
    print("  两类检查：① 视觉重叠（按**文字实占区**判，居中标题不算）")
    print("            ② 触摸遮挡（按**控件盒**判：下层可点控件被上层盒覆盖）")

    touch = [h for h in allhits if h["kind"] == "触摸"]
    vis = [h for h in allhits if h["kind"] == "视觉"]
    high = [h for h in vis if h["sev"] == "高"]
    low = [h for h in vis if h["sev"] == "低"]

    if not allhits:
        print("\n  PASS  没有发现重叠问题 ✓")
        print("=" * 78)
        return 0

    print("\n[触摸] 下层可点控件被上层盒覆盖 —— 它的点击收不到（视觉上看不出来）—— %d 处" % len(touch))
    for h in (touch if verbose else touch[:12]):
        print("   %-14s %-22s" % (h["file"], h["group"][:22]))
        print("      %s 盒 x=%d..%d y=%d..%d   ← 可点，但被下面这个盖住了"
              % (h["a"], h["ra"][0], h["ra"][2], h["ra"][1], h["ra"][3]))
        print("      %s 盒 x=%d..%d y=%d..%d   （声明在后 = 更上层）"
              % (h["b"], h["rb"][0], h["rb"][2], h["rb"][1], h["rb"][3]))
        print("      → 重叠 %d px²   修法：把上层那个的盒缩到不压它" % h["area"])
    if not verbose and len(touch) > 12:
        print("   …（还有 %d 处，用 -v 看全）" % (len(touch) - 12))

    print("\n[视觉] 涉及文字的部分重叠（文字会被盖住）—— %d 处" % len(high))
    for h in (high if verbose else high[:10]):
        print("   %-14s %s × %s  重叠 %d px²" % (h["file"], h["a"], h["b"], h["area"]))

    print("\n[低] 图形件之间的部分重叠（可能是刻意的拼接，人工判断）—— %d 处" % len(low))
    for h in (low if verbose else low[:8]):
        print("   %-14s %s × %s  重叠 %d px²" % (h["file"], h["a"], h["b"], h["area"]))

    bad = len(touch) + len(high)
    print("\n" + "=" * 78)
    print("  结论：%s" % ("有 %d 处**高**优先级问题 ✗（触摸 %d + 视觉 %d）" % (bad, len(touch), len(high))
                        if bad else "无高优先级问题 ✓"))
    print("=" * 78)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
