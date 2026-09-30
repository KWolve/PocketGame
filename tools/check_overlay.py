#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_overlay.py - 「常显浮层盖住页面控件」检讨（navibar / 音量面板）

## 为什么需要它（2026-09-16，用户需求原文）
「把 Ui 目录下的 json 布局全部检讨一遍…navibar 是盖在 UI 上面的，
  检讨有 UI 区域覆盖的情况下告警并修复」。

## 判据的几何依据（不要凭感觉改这几个数）
· **navibar**：`ui/navibar.json` 的窗口是 **480x52 @(0,0)、topmost、常显**
  ⇒ 它压住 **屏幕 y ∈ [0,52)** 这一条。只在**屏保**与**视频播放页**隐藏
  （见 src/logic/navibar.cc 的定时器）。
· **statusbar（音量 OSD）**：**240x160 @(120,520)、topmost**，但**按需显隐**
  （音量变化弹出、1.6s 自动收起）⇒ "弹出来盖住底下"是它的设计语义，
  **只统计、不判错**（否则会误报一大片，第一版就栽在这里）。

## 分档（关键：区分"露残边"与"整块被盖"，以及"根本不画"）
| 档 | 条件 | 含义 | 处理 |
|---|---|---|---|
| `SLIVER` | 常显浮层只盖住它**一部分**，且它**确实会画东西** | 被切成半截：**肉眼可见的瑕疵** —— 实测就是它在状态栏下沿留了一条 8px 的 #1C1C1E 横带 | **告警，必修** |
| `BURIED` | 常显浮层把它**整块**罩住 | 不是残影，但可能丢信息（投屏状态字被整块盖死那类） | 提示，人工判断 |
| `PAGE_BASE` | 是整屏窗口（页底） | 被盖的只是底色 | 不报 |
| `NOSHOW` | 既无文字也无图、底色是 -1 | 不画任何像素（透明背板） | 不报 |
| `PANEL_HIT` | 只与本脚本登记的**按需**浮层相交 | 音量 OSD 的既定行为 | 只在 `-v` 下统计 |

## 已知并接受的 `BURIED`（别重复"发现"）
1. 各页**顶部 8px 强调色条**（`BarCalc`/`BarClock`/…）：全在 0..8，被整块盖。
   其中 ToolPage 系已被 `ToolPage::init()` 显式隐藏（改版决定"iOS 页面里没有整条饱和色"）。
2. **与 navibar 标题重复的页面标题**（camera/radio/wifi/probe/suite）：
   整块被盖、无残影，信息由 navibar 代显。
3. **11 处页面标题**（`Text*Title`，盒 8..52）：2026-09-16 已**有意收进**浮层覆盖区
   （`data-h=52→44`），只为消掉那条 8px 残带 —— 标题由 navibar 显示（见 ui/navibar.html）。
4. **视频区**（`Caster`/`IptvVideo`/`CamVideo` 480x700 @0,0）：顶部 52px 是真丢画面，
   已由"进播放页隐藏 navibar"解决（`pg::setVideoPage`）。

用法：
  python tools/check_overlay.py            # 全部 ui/*.json
  python tools/check_overlay.py -v         # 连 PANEL_HIT / 已定案项一起列
  python tools/check_overlay.py main.json  # 单个文件
退出码：0 = 无 SLIVER；1 = 有 SLIVER（可进 CI / 打包前置检查）
"""
import glob
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
UI_DIR = os.path.join(ROOT, "ui")

# 常显浮层：与 ui/navibar.json 一致（改 navibar 尺寸时这里必须同步）
NAVI = {"name": "Navibar", "left": 0, "top": 0, "width": 480, "height": 52, "always": True}
# 按需浮层：只统计不判错
PANELS = [{"name": "VolPanel", "left": 120, "top": 520, "width": 240, "height": 160,
           "always": False}]

SKIP_FILES = {"navibar.json", "statusbar.json", "screensaver.json"}
# 视频区（videoview 透明窗口）：被 navibar 盖 52px 是"真丢画面"，
# 但修法不是挪盒子（那是硬件 disp 层的显示坐标，不能动），而是"进播放页把 navibar
# 收起来"（`pg::setVideoPage(true)`）⇒ 归到已定案，不算 SLIVER。
# ⚠️ **新增任何视频播放页都要把它的 videoview caption 补到这里**，
#    否则会报一条看着很吓人的 SLIVER（其实是设计如此）。
#    ★ 2026-09-23：`VideoMain`（fairy.ftu / kitten.ftu 的全屏播放区）**已删** ——
#      那两个应用下线了，控件与页都不存在。留着只在"报告里列出已删页"这一点上有害。
VIDEO_VIEWS = {"Caster", "IptvVideo", "CamVideo"}

# 「顶栏背板」：源稿上是 `data-bg="#000000"` 的整条占位块，**不画任何像素**
# （html2json 把这个"合法的纯黑 0"写成了 -1=透明，见 tools/gen_ui.py 的
#   OPAQUE_WINDOWS 注释；页面底色本来就是黑，所以看不出差别）。
# ⇒ 它们被盖 52/64 也没有残影，不该进 SLIVER。**新增同类背板时在这里补一行。**
NO_PAINT = {"BarCamTop", "BarRadioTop", "BarWifiTop", "BarBtTop", "BarLanTop",
            "BarSnTop", "BarHuntTop"}

# 2026-09-16 已定案（有意为之）的 BURIED，打印时标出来，避免下次被当新问题
SETTLED = {
    "BarCalc", "BarPm", "BarSw", "BarTm", "BarRc", "BarSet", "BarRemote", "BarSuite",
    "BarWorld", "BarClock", "BarReact", "BarRing", "BarIptv", "BarRadioPlayTop",
    "BarRemoteLearn", "BarCastBottom",
    "TextCalcTitle", "TextClockTitle", "TextReactTitle", "TextPmTitle", "TextRcTitle",
    "TextSwTitle", "TextTmTitle", "TextSetTitle", "TextRemoteTitle",
    "TextRemoteLearnTitle", "TextCamTitle", "TextRadioTitle", "TextWifiTitle",
    "TextSuiteTitle", "TextGameTitle", "TextWorldTitle", "TextCastMsg",
    # 视频区：被盖 52px 是真丢画面，已由"进播放页隐藏 navibar"解决（pg::setVideoPage）
    "Caster", "IptvVideo", "CamVideo",
}


def walk(node, ox, oy, vis, ctx, out):
    """摊平控件树。

    ⚠️ 两件事必须做对（第一版都踩过）：
      ① **可见性按祖先链与**，但**整屏窗口另算**：`WinGame` 里的 `TextGameTitle`
         自身 vis=true、`WinGame` 隐藏 —— 若直接判祖先，"进游戏后才会显示"的那几个
         窗口就会被整个漏掉（第一版只报出 7 处、漏了 main.json 里的 3 处）。
         所以把 `window__` 节点当"**将来会显示**"处理（它只是初始隐藏的整页）；
      ② **listview 的 `item` 模板不是屏幕坐标**（相对 item 自身），必须跳过。
    """
    for key, val in node.items():
        if not isinstance(val, dict) or "position" not in val:
            continue
        p = val["position"]
        x, y = ox + p.get("left", 0), oy + p.get("top", 0)
        w, h = p.get("width", 0), p.get("height", 0)
        cap = val.get("caption", key)
        bg = val.get("backgroundColor")
        pic = val.get("backgroundPic") or val.get("picTab") or val.get("iconPosition")
        is_win = key.startswith("window__") or key.startswith("modal__")   # 整页/弹窗容器
        own = val.get("visible", True) is not False
        # 整页/弹窗容器按"将来会显示"处理（它只是初始隐藏），其余按自身 visible
        vis2 = (True if is_win else own) and vis
        out.append({
            "caption": cap, "key": key, "x": x, "y": y, "w": w, "h": h,
            "visible": vis2,
            "isWindow": is_win,
            # 「会不会画东西」：有文字 / 有图 / 底色不是 -1(None 表示没有该字段)，
            # 或者有 fontSize/colorTab（= 文字控件，内容由逻辑层运行时写，如 TextCastMsg）。
            "paints": (bool(val.get("text")) or bool(pic) or bg not in (None, -1)
                       or "fontSize" in val or "colorTab" in val)
                      and cap not in NO_PAINT,
            "touchable": val.get("touchable", False),
            "text": val.get("text") or "",
            "ctx": ctx,
        })
        nctx = cap if not ctx else ctx
        if "item" in val and isinstance(val["item"], dict):
            continue
        walk(val, x, y, vis2, nctx, out)


def overlap(a, r):
    x0, y0 = max(a["x"], r["left"]), max(a["y"], r["top"])
    x1 = min(a["x"] + a["w"], r["left"] + r["width"])
    y1 = min(a["y"] + a["h"], r["top"] + r["height"])
    if x1 <= x0 or y1 <= y0:
        return None
    return {"x0": x0, "y0": y0, "x1": x1, "y1": y1, "w": x1 - x0, "h": y1 - y0}


def classify(item, ov, layer):
    if not layer["always"]:
        return "PANEL_HIT"
    if item["isWindow"] and item["h"] >= 600:      # 整屏窗口 = 页底，被盖的是底色
        return "PAGE_BASE"
    if not item["paints"]:                         # 透明背板：不画像素
        return "NOSHOW"
    # 视频区：被盖 52px 是**真丢画面**，但修法不是挪盒子（那是硬件 disp 层的显示坐标），
    # 而是"进播放页把 navibar 收起来"（pg::setVideoPage）⇒ 归到已定案，不算 SLIVER。
    if item["caption"] in VIDEO_VIEWS:
        return "BURIED"
    fully = ov["h"] >= item["h"] and ov["w"] >= item["w"]
    return "BURIED" if fully else "SLIVER"


def check_file(path):
    fn = os.path.basename(path)
    if fn in SKIP_FILES:
        return []
    try:
        data = json.load(open(path, encoding="utf-8"))
    except Exception as e:                                     # noqa: BLE001
        print("  !! 读取失败 %s: %s" % (fn, e))
        return []
    items = []
    walk(data, 0, 0, True, "", items)
    res = []
    for it in items:
        if not it["visible"] or it["w"] <= 0 or it["h"] <= 0:
            continue
        for layer in [NAVI] + PANELS:
            ov = overlap(it, layer)
            if ov:
                res.append(dict(file=fn, layer=layer, item=it, ov=ov,
                                kind=classify(it, ov, layer)))
    return res


def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    files = []
    for a in args:
        if os.path.isdir(a):
            files += sorted(glob.glob(os.path.join(a, "*.json")))
        elif os.path.isfile(a):
            files.append(a)
    if not files:
        files = sorted(glob.glob(os.path.join(UI_DIR, "*.json")))
    if not files:
        print("没找到 ui/*.json —— 先跑 gen_ui.py")
        return 1

    hits = []
    for f in files:
        hits += check_file(f)

    def pick(*kinds):
        return [h for h in hits if h["kind"] in kinds]

    sliver = pick("SLIVER")
    buried = pick("BURIED")
    pagebase = pick("PAGE_BASE")
    noshow = pick("NOSHOW")
    panel = pick("PANEL_HIT")

    print("=" * 78)
    print("  浮层覆盖检讨   [常显] Navibar %dx%d @(%d,%d) ⇒ 压住屏幕 y ∈ [%d,%d)"
          % (NAVI["width"], NAVI["height"], NAVI["left"], NAVI["top"],
             NAVI["top"], NAVI["top"] + NAVI["height"]))
    print("                 [按需] VolPanel %dx%d @(%d,%d)（音量 OSD，只统计）"
          % (PANELS[0]["width"], PANELS[0]["height"], PANELS[0]["left"], PANELS[0]["top"]))
    print("=" * 78)

    if sliver:
        print("\n[SLIVER] 被常显浮层切成半截、下沿露出可见残边 —— **必修** —— %d 处"
              % len(sliver))
        for h in sliver:
            it, ov, L = h["item"], h["ov"], h["layer"]
            print("   %-16s %-20s 盒(%d,%d,%dx%d)  被 %s 盖 %dpx → **露 %dpx**"
                  % (h["file"], it["caption"][:20], it["x"], it["y"], it["w"], it["h"],
                     L["name"], min(ov["h"], it["h"]), it["h"] - ov["h"]))
            print("        修法：盒收进 y<%d（`data-h` 改为 %d）或整体下移到 y>=%d"
                  % (L["top"] + L["height"], L["top"] + L["height"] - it["y"],
                     L["top"] + L["height"]))
    else:
        print("\n[SLIVER] 无 —— 没有被切成半截露残边的控件 ✓")

    new_buried = [h for h in buried if h["item"]["caption"] not in SETTLED]
    print("\n[BURIED] 整块落在常显浮层内 —— 确认是否丢信息 —— %d 处（其中 %d 处已定案）"
          % (len(buried), len(buried) - len(new_buried)))
    for h in (buried if verbose else new_buried or buried[:8]):
        it = h["item"]
        flag = "已定案" if it["caption"] in SETTLED else "★待确认"
        print("   %-16s %-20s 盒(%d,%d,%dx%d) %-4s vis=%s txt=%r"
              % (h["file"], it["caption"][:20], it["x"], it["y"], it["w"], it["h"],
                 flag, it["visible"], it["text"][:10]))
    if not verbose and len(buried) > 8:
        print("   …（还有 %d 处，用 -v 看全）" % (len(buried) - 8))

    if verbose:
        print("\n[PAGE_BASE] 整屏窗口被盖（只是页底底色）—— %d 处" % len(pagebase))
        for h in pagebase:
            print("   %-16s %-20s" % (h["file"], h["item"]["caption"][:20]))
        print("\n[NOSHOW] 透明背板被盖（不画像素，无残影）—— %d 处" % len(noshow))
        for h in noshow:
            print("   %-16s %-20s 盒(%d,%d,%dx%d)"
                  % (h["file"], h["item"]["caption"][:20], h["item"]["x"],
                     h["item"]["y"], h["item"]["w"], h["item"]["h"]))
        print("\n[PANEL_HIT] 只与按需浮层（音量 OSD）相交 —— 设计语义，不判错 —— %d 处"
              % len(panel))

    print("\n" + "=" * 78)
    if sliver:
        print("  结论：有 %d 处**可见残边**（SLIVER）✗" % len(sliver))
    elif new_buried:
        print("  结论：无可见残边 ✓；另有 %d 处整块被盖**待确认**（见上）" % len(new_buried))
    else:
        print("  结论：PASS —— 无可见残边，整块被盖项均已定案 ✓")
    print("=" * 78)
    return 1 if sliver else 0


if __name__ == "__main__":
    sys.exit(main())
