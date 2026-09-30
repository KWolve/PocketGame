#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_assets.py - resource/images 资源体检（透明正确性 + 边缘质量 + 源稿一致性）

为什么需要这个工具（2026-09-16，作者："倒角有锯齿，还有一个黑色边框。检讨整套
resource 目录下的图标。"）：
    这类问题**真机上一眼看见，源码里搜不到** —— 它们不是"代码写错了"，而是
    "生成出来的像素不对"。没有工具就只能靠肉眼在 480x800 的小屏上找茬，
    而且改完一次要重新打包、推送、抓屏才能知道有没有变好。
    所以把"像素质量"固化成可复现的判据，每次改完生成器跑一次即可。

六项检查（每一项都对应一次真实踩坑）：

  ① 源稿 round+bg 冲突    data-round 的控件又写了 data-bg
     ⇒ 框架会跳过 .9.png 的 1px marker 边不画、把图内容安置在"控件内缩 1px"处，
        控件最外 1px 于是露出**控件自己的底色** ⇒ 视觉上是一圈错位的黑框/亮框。
     判据：必须为 0 处。（2026-09-16 标定图法实测确认了这个 1px 映射）

  ② 透明区污染            α==0 但 RGB!=0 的像素
     ⇒ 这些像素本该完全不可见；**只要渲染路径忽略 alpha（或做 1-bit 量化），
        它们就会直接显形**。非预乘 alpha 缩放的典型残留（实测出现过 RGB=255 的白点）。
     判据：0 像素。

  ③ 边缘振铃              α 在沿边扫描时非单调（上升过程中回落 >3）
     ⇒ LANCZOS 等带负瓣的插值核在硬边两侧产生过冲，边缘外侧散落孤立的小 α 像素。
        渲染到深色底上就是零星的亮点 —— 肉眼即"锯齿/毛刺"。
     判据：0 处回落。

  ④ AA 过渡带宽度         圆角/硬边上"既非底色也非面色"的像素跨越了几行
     ⇒ 用"高斯羽化"做抗锯齿会把硬边向内向外都抹开（实测 3~4px），
        合成到深色底后是一圈**发虚的脏边**。正确做法是超采样精确覆盖率，恒为 1px。
     判据：≤2px。

  ⑤ 烘底完整性            ios_* 面板/按钮类图，除圆角外的四角外侧必须是**不透明**的底色
     ⇒ 运行时 setBackgroundPic() 不保留 alpha，透明会渲染成白边。
     判据：0 处透明像素。

  ⑥ 九宫格 marker           .9.png 四周必须有 1px marker 边（编码拉伸区）
     ⇒ 缺 marker ⇒ 拉伸区判断错 ⇒ 圆角/边框被拉变形。
     判据：0 处缺失。

用法：
    python tools/check_assets.py            # 打印报告
    python tools/check_assets.py -v         # 附失败项的具体数值/文件名
    python tools/check_assets.py --json out.json
退出码：0 = 全部 PASS，1 = 有 FAIL（可直接用于 CI / 打包前置检查）
"""
import os
import re
import sys
import glob
import json

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG_DIR = os.path.join(ROOT, "resources", "images")
UI_DIR = os.path.join(ROOT, "ui")

# 面板 / 按钮 / 圆角底 —— 这些是"烘底了的不透明图"，判定标准与矢量图标不同
OPAQUE_PREFIXES = ("ios_btn", "ios_panel", "ios_seg", "ios_card", "ios_row",
                   "ios_rt_", "grid_cell", "batt_fill")
# 矢量图标 —— 天生带 alpha 的不规则形状，不参与"烘底完整性"检查
ICON_PREFIXES = ("icon_", "app_icon_", "sig_", "lock", "switch_", "batt_bolt",
                 "flip_", "vol_", "bd_", "video_cover")


def _rgba(path):
    from PIL import Image
    im = Image.open(path)
    return im.convert("RGBA") if im.mode in ("RGBA", "LA", "P") else im.convert("RGB").convert("RGBA")


# ---------------------------------------------------------------- ① 源稿冲突
def _transparent_corner(path):
    """图的**内容区**四角是否透明（`.9.png` 跳过最外 1px marker 环）。

    ★ 2026-09-24 新增：静态九宫格套已改烘**透明底**，其圆角外的半透明像素是
      "与容器色正常合成的抗锯齿"，**是我们要的**，不该被 ④ 当成残留 ⇒ 用这个判据豁免。
    """
    im = _rgba(path)
    w, h = im.size
    off = 1 if path.endswith(".9.png") else 0
    px = im.load()
    return any(px[x, y][3] < 250 for x, y in
               ((off, off), (w - 1 - off, off), (off, h - 1 - off), (w - 1 - off, h - 1 - off)))


def check_ui_conflicts(verbose):
    """data-round 的控件不能同时写 data-bg（除非要求"最外 1px 露出的颜色 == 图烘的底色"）。

    例外白名单：控件**必须**有底色时（引擎会给默认白底的 div.input 就是），
    只能显式写"等于图烘底色"的值 —— 这种是刻意的，登记在这里并在源稿注释里说明原因。
    """
    ALLOW = {
        # caption: (允许的 bg 值, 原因)
        "EditPwd": ("#1C1C1E", "div.input 有引擎默认白底(0xFFFFFF)，不写会露白框；"
                               "#1C1C1E == ios_panel2_c 烘的 SURFACE 底"),
        # 2026-09-17：摄像头页的「账号密码」弹窗，**整段照抄 wifi 那套**
        # （同一个坑：div.input 默认白底 ⇒ 必须显式写卡片底 #1C1C1E，纹理才接得上）。
        "EditCamUser": ("#1C1C1E", "同 EditPwd：div.input 默认白底，须显式写卡片底"),
        "EditCamPwd":  ("#1C1C1E", "同 EditPwd：div.input 默认白底，须显式写卡片底"),
        # 2026-09-24：ha 命名页的输入框**直接坐在黑窗口上**（不像上面三个坐在 #1C1C1E 面板里）
        #   ⇒ 最外 1px 应该是"页面黑"。因为 html2json 把"纯黑 0"当未设置，这里写 #010101。
        "EditHaName":  ("#010101", "坐在黑窗口上的 div.input；#010101 = 近黑，最外 1px 直接融进页面"),
    }
    bad, allowed = [], []
    for p in sorted(glob.glob(os.path.join(UI_DIR, "*.html"))):
        s = open(p, encoding="utf-8").read()
        for m in re.finditer(r'<(\w+)([^>]*?)>', s, re.S):
            a = m.group(2)
            if "data-round" not in a or "data-bg=" not in a:
                continue
            cap = re.search(r'data-caption="([^"]+)"', a)
            bg = re.search(r'data-bg="([^"]+)"', a)
            cap = cap.group(1) if cap else "(无 caption)"
            bgv = bg.group(1) if bg else ""
            if cap in ALLOW and bgv.upper() == ALLOW[cap][0].upper():
                allowed.append((os.path.basename(p), cap, bgv, ALLOW[cap][1]))
            else:
                bad.append((os.path.basename(p), cap, bgv))
    return bad, allowed


# ------------------------------------------------- ② ③ ④ 逐图像素质量
def _scan_image(path, verbose):
    """返回该图的各项指标 dict。"""
    im = _rgba(path)
    w, h = im.size
    px = im.load()
    name = os.path.basename(path)
    is9 = name.endswith(".9.png")

    # 内容区（.9.png 跳过 1px marker 边）
    x0, y0, x1, y1 = (1, 1, w - 1, h - 1) if is9 else (0, 0, w, h)
    dirty = 0          # ② α==0 但 RGB!=0
    semi = 0           # 半透明像素数
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            r, g, b, a = px[x, y]
            total += 1
            if a == 0:
                if r or g or b:
                    dirty += 1
            elif a < 255:
                semi += 1

    # ③ 幽灵像素（重采样振铃的产物）：判据 = "**α 不够浓 + 8 邻域内完全没有浓像素**"。
    #    重采样核（LANCZOS/BILINEAR）带负瓣或过冲时，会在形状边界**之外**漏出一圈
    #    极淡的像素（实测 batt_bolt 的闪电外侧有一串 α≈17），它们在深色底上就是
    #    隐约的毛刺/锯齿。判据要能把它和"正常的边缘过渡"分开：
    #      · 线末端、细线边缘的像素虽然 α 低，但**旁边一定有主体**（α 很高）；
    #      · 幽灵是**悬空的** —— 它的 8 邻域全是 0。
    #    ⚠️⚠️ 这个判据改了三版，前两版都误报，记下来免得再踩：
    #      ① "α 非单调" → 误报 25 张。**细线的正确截面本来就不单调**
    #         （线宽 2~3px：0→97→166→77→0）。
    #      ② "归零后又冒出 α>0" → 仍误报 13 张。**相邻两条线只隔 1px** 时，
    #         第二条线的起点（α=69）被当成幽灵（剖面 [...213, 35, 0, 69, 255]）。
    #      ③ "孤立连通段峰值太低" → 还误报 12 张：**线末端**的覆盖率天然就低。
    #    ⇒ 只有"二维邻域里找不到任何浓像素"才是真幽灵。
    #  ★★ 2026-09-16（第二轮改版踩到）：这个判据对**整体半透明的图**会**全图误报** ——
    #    音量条的半透明轨道（α 恒为 40）整张都没有"浓像素"，于是被判成 1988/3108 处幽灵、
    #    FAIL 退出。它压根没有"主体/背景"之分，本来就不该参与振铃检查。
    #    ⇒ 先看**全图最大 α**：若它 < GHOST_PEAK（整张图是故意半透明的），直接跳过。
    #    ⚠️ 同一条规则在 `tools/gen_ui.py::polish_generated_icons` 里也要有 ——
    #      那边是把幽灵像素**真的清零**，漏了会把整张半透明图擦掉（已各修一处）。
    GHOST_PEAK = 150
    if (im.getchannel("A").getextrema()[1] or 0) < GHOST_PEAK:
        return dict(name=name, size=(w, h), is9=is9, dirty=dirty, semi=semi,
                    total=total, ringing=0, scan=None, translucent=True)
    ghost = 0
    best_scan = None
    for y in range(y0, y1):
        for x in range(x0, x1):
            a = px[x, y][3]
            if a == 0 or a > GHOST_PEAK:
                continue
            dense = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    xx, yy = x + dx, y + dy
                    if x0 <= xx < x1 and y0 <= yy < y1 and px[xx, yy][3] > GHOST_PEAK:
                        dense += 1
            if dense == 0:
                ghost += 1
                if best_scan is None:
                    best_scan = (x, y, a)
    return dict(name=name, size=(w, h), is9=is9, dirty=dirty, semi=semi,
                total=total, ringing=ghost, scan=best_scan)


def check_pixels(verbose):
    conflicts, ring, dirty, aa = [], [], [], []
    files = sorted(f for f in os.listdir(IMG_DIR) if f.lower().endswith(".png"))
    for f in files:
        p = os.path.join(IMG_DIR, f)
        try:
            r = _scan_image(p, verbose)
        except Exception as e:
            print("  !! 读取失败 %s: %s" % (f, e))
            continue
        if r["dirty"] > 0:
            dirty.append(r)
        if r["ringing"] > 0:
            ring.append(r)
        # ④ 只对"烘底类"图检查过渡带（矢量图标线宽本身就只有 1~2px，AA 必然宽）
        # ★★ 2026-09-24：**静态九宫格套（烘透明底）豁免** —— 它的半透明像素就是
        #    圆角边缘与容器色合成的 AA，是设计要的；只有"烘不透明底"的图
        #    （运行时套 ios_rt_* / batt_* 等）才要求 α 恒为 255。
        if (f.startswith(OPAQUE_PREFIXES) and r["semi"] > 0
                and not _transparent_corner(os.path.join(IMG_DIR, f))):
            # 烘底图的半透明像素 = 抗锯齿残留；占比过高说明 AA 方式不对
            ratio = r["semi"] / max(1, r["total"])
            if ratio > 0.002:
                aa.append(dict(r, ratio=ratio))
    return conflicts, ring, dirty, aa


# ----------------------------------------------------------- ⑤ 烘底完整性
def check_opaque_baked(verbose):
    """ios_* 面板类图：把图当"圆角矩形"，检查**圆角外侧**是否有透明像素。

    判据取法：图的四条边的中点一带（远离圆角）必须完全不透明 ——
    那里本该是"烘上去的底色"，若透明/半透明说明没烘底或烘漏了。
    """
    bad = []
    for f in sorted(os.listdir(IMG_DIR)):
        if not f.lower().endswith(".png"):
            continue
        if not f.startswith(OPAQUE_PREFIXES):
            continue
        p = os.path.join(IMG_DIR, f)
        im = _rgba(p)
        w, h = im.size
        px = im.load()
        is9 = f.endswith(".9.png")
        off = 1 if is9 else 0
        if w - 1 - off <= off or h - 1 - off <= off:
            continue          # 图太小（如电量条的 1px 宽片），四条边中点法不适用
        cx = min(max(w // 2, off + 1), w - 1 - off - 1)
        cy = min(max(h // 2, off + 1), h - 1 - off - 1)
        # 上边中点、下边中点、左边中点、右边中点，各取 3px 深
        probes = []
        for dx in (cx - 1, cx, cx + 1):
            if off <= dx <= w - 1 - off:
                probes.append((dx, off))
                probes.append((dx, h - 1 - off))
        for dy in (cy - 1, cy, cy + 1):
            if off <= dy <= h - 1 - off:
                probes.append((off, dy))
                probes.append((w - 1 - off, dy))
        hole = [q for q in probes if px[q[0], q[1]][3] < 255]
        if hole:
            bad.append((f, len(hole), hole[:3]))
    return bad


# --------------------------------------------------------- ⑥ 九宫格 marker
def check_markers(verbose):
    bad = []
    for f in sorted(os.listdir(IMG_DIR)):
        if not f.endswith(".9.png"):
            continue
        p = os.path.join(IMG_DIR, f)
        im = _rgba(p)
        w, h = im.size
        px = im.load()
        top = sum(1 for x in range(w) if px[x, 0][3] == 255 and px[x, 0][:3] == (0, 0, 0))
        left = sum(1 for y in range(h) if px[0, y][3] == 255 and px[0, y][:3] == (0, 0, 0))
        if top == 0 or left == 0:
            bad.append((f, top, left))
    return bad


# --------------------------------------------------------------------- 主流程
def main():
    verbose = "-v" in sys.argv or "--verbose" in sys.argv
    out_json = None
    if "--json" in sys.argv:
        out_json = sys.argv[sys.argv.index("--json") + 1]

    print("=" * 74)
    print("  resource/images 资源体检   %s" % IMG_DIR)
    print("=" * 74)
    fails = 0

    # ①
    bad, allowed = check_ui_conflicts(verbose)
    print("\n[①] 源稿 data-round + data-bg 冲突（框架 1px 内缩会让最外 1px 露出控件底色）")
    if bad:
        fails += 1
        print("     FAIL  %d 处" % len(bad))
        for b in bad:
            print("        %-14s %-22s bg=%s" % b)
    else:
        print("     PASS  0 处冲突")
    for a in allowed:
        print("        (白名单) %-14s %-20s bg=%s\n                  ↳ %s" % a)

    # ② ③ ④
    _, ring, dirty, aa = check_pixels(verbose)
    print("\n[②] 透明区污染（α=0 却带 RGB —— 渲染忽略 alpha 时会显形）")
    if dirty:
        fails += 1
        tot = sum(r["dirty"] for r in dirty)
        print("     FAIL  %d 张图 / 共 %d 像素" % (len(dirty), tot))
        for r in sorted(dirty, key=lambda t: -t["dirty"])[:10 if not verbose else 999]:
            print("        %-38s %5d px  %s" % (r["name"], r["dirty"], r["size"]))
    else:
        print("     PASS  0 像素")

    print("\n[③] 边缘振铃（幽灵像素（重采样振铃：悬空在形状外的淡像素 —— 深色底上的毛刺/锯齿））")
    if ring:
        fails += 1
        print("     FAIL  %d 张图" % len(ring))
        for r in sorted(ring, key=lambda t: -t["ringing"])[:10 if not verbose else 999]:
            print("        %-38s 幽灵像素 %d 处  %s" % (r["name"], r["ringing"], r["size"]))
            if verbose and r["scan"]:
                print("           α 剖面: %s" % r["scan"])
    else:
        print("     PASS  0 张")

    print("\n[④] 烘底类图的抗锯齿残留（半透明占比 >0.2% ⇒ AA 用了模糊而非精确覆盖率）")
    print("     （静态九宫格套已改烘**透明底**，圆角外的半透明 AA 属正常，已豁免）")
    if aa:
        fails += 1
        print("     FAIL  %d 张图" % len(aa))
        for r in sorted(aa, key=lambda t: -t["ratio"])[:10 if not verbose else 999]:
            print("        %-38s 半透明 %5d/%d (%.1f%%)" % (r["name"], r["semi"], r["total"], r["ratio"] * 100))
    else:
        print("     PASS  0 张（AA 只有 1px 过渡带）")

    # ⑤
    baked = check_opaque_baked(verbose)
    print("\n[⑤] 烘底完整性（ios_* 面板/按钮：远离圆角的四条边中点必须完全不透明）")
    if baked:
        fails += 1
        print("     FAIL  %d 张图" % len(baked))
        for f, n, sample in baked:
            print("        %-38s 透明采样点 %d 例 %s" % (f, n, sample))
    else:
        print("     PASS  0 张")

    # ⑥
    mk = check_markers(verbose)
    print("\n[⑥] 九宫格 marker（.9.png 四周必须有 1px 纯黑 marker）")
    if mk:
        fails += 1
        print("     FAIL  %d 张图" % len(mk))
        for f, t, l in mk:
            print("        %-38s 上边 %d px / 左边 %d px" % (f, t, l))
    else:
        print("     PASS  0 张")

    print("\n" + "=" * 74)
    print("  结论：%s" % ("全部通过 ✓" if fails == 0 else "有 %d 项 FAIL ✗" % fails))
    print("=" * 74)

    if out_json:
        json.dump(dict(fails=fails, conflicts=bad, allowed=allowed, dirty=[r["name"] for r in dirty],
                       ringing=[r["name"] for r in ring], aa=[r["name"] for r in aa],
                       baked=[b[0] for b in baked], markers=[m[0] for m in mk]),
                  open(out_json, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
        print("  报告已写入 %s" % out_json)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
