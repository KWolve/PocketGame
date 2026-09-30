#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ios_theme.py - PocketGame「iOS 深色」设计令牌 + 九宫格资源生成

为什么单独一个文件：
  每个页面各自写死色值 → 多页面必然漂移（本工程改版前就是：130 处按钮共用
  #26313F，底色 6 档亮度差只有 6~10 级）。这里把**颜色、圆角、字号**收成唯一真值，
  ui/*.html 只准引用这里的值，gen_ui.py 也只按这里的色值去挂图。

为什么圆角要靠"图"而不是 CSS：
  FlyThings 没有 CSS 引擎。实测（2026-09-14）：
    · div.card/window 上的 border-radius → html2json **能自动转图**
    · div.btn 上的 border-radius → **不转**，会输出 warning 让你自己切图
  本工程按钮有 285 个，逐个切图不现实。正解 = **按语义色生成九宫格（.9.png）两态图**，
  一种颜色一张图，任意尺寸都能拉伸复用；再由 gen_ui.py 按控件的底色自动挂上去。
  这样 HTML 里一个 data-pic 都不用写，285 个按钮一次到位。

九宫格规范见 MCP `knowledge/uicontrols/nine-patch-rule.md` 五条规则
（marker 纯黑不透明 / top·left 只画拉伸段 / right·bottom 同宽 / 1px 贴边 / 最后绘制）。
本文件直接用 gen_res.to_9patch 生成，不自己实现 marker。

用法：
  python tools/ios_theme.py            # 生成全部九宫格 + 电池图到 resources/images/
  python tools/ios_theme.py --check    # 只校验已有资源（尺寸/marker）
  from ios_theme import TOKENS, btn_asset_for   # 供 gen_ui.py 引用
"""
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT_DIR = os.path.join(ROOT, "resources", "images")

# gen_res（圆角九宫格等素材生成器）在 FlyThings 工具目录里（工程不复制一份，避免双份漂移）。
# 用环境变量 PG_MCP_UI_TOOLS 指向它；未设置时按常见安装位置兜底。
MCP_UI_TOOLS = os.environ.get(
    "PG_MCP_UI_TOOLS", r"D:\zkswe\flythings-mcp-open\ui_tools")


# ==================================================================
#  1. 设计令牌（唯一真值）
# ==================================================================
#
# 底色/面：iOS 深色系统色。层次靠"底色深浅"拉开（深底上 1px 描边会被抗锯齿糊掉）。
TOKENS = {
    # ---- 面（从底到顶，亮度递增）----
    "BG":        "#000000",   # 页面底（iOS 深色系统底，纯黑）
    "SURFACE":   "#1C1C1E",   # 卡片 / 面板（iOS secondarySystemBackground dark）
    "SURFACE2":  "#2C2C2E",   # 分段控件容器 / 次级按钮 / 卡片按下态
    "SURFACE3":  "#3A3A3C",   # 选中胶囊 / 强按下
    "SURFACE4":  "#48484A",   # 按下高亮

    # ---- 文字（iOS label 四档）----
    # ⚠️ 主文字**不用纯白 #FFFFFF**：小字号在深底上会发光发胀
    #    （见 docs/ui-design-baseline.md §2）。iOS 的 systemGray6 正好是"有温度的近白"。
    "T1":        "#F2F2F7",   # 主文字：标题 / 卡片名 / 按钮文字
    "T2":        "#9A9AA0",   # 次文字：标签 / 描述
    "T3":        "#636366",   # 提示：操作说明
    "T4":        "#48484A",   # 最弱：版本号 / 版权

    # ---- 强调（语义固定，全工程不许串用）----
    "ACCENT":    "#0A84FF",   # 系统蓝：链接性 / 可交互指示 / 主行动
    "DATA":      "#FF9F0A",   # 数据橙：得分 / 需要被看见的数值
    "SUCCESS":   "#30D158",   # 成功绿：确认 / 开始（一屏只能有一个）
    "DANGER":    "#FF453A",   # 危险红：停止 / 删除
    "DANGER_SOFT": "#3A1F1F",  # 危险暗底：破坏性动作降权
    "WARN":      "#FFD60A",   # 告警黄：低电 / 注意
    "TEAL":      "#64D2FF",   # 次强调青：次要数值 / 状态
    "SEP":       "#38383A",   # 分隔线（iOS dark separator）：列表行之间那条 1px 细线
}

# ---- 圆角档（像素）----
RADIUS = {
    "card":   26,   # 列表行卡片（行高 110）
    "panel":  20,   # 弹窗 / 面板
    "seg":    14,   # 分段控件容器
    "segsel": 11,   # 选中胶囊
    "btnL":   14,   # 大按钮（高 ≥ 48）
    "btnM":   10,   # 中按钮（高 30..47）
    "btnS":   8,    # 小按钮（高 < 30）
    "key":    12,   # 键盘键
    "icon":   19,   # 应用图标 squircle（边长 64 → 30%）
}

# ---- 字号档（沿用工程既有阶梯，只调语义）----
FONTSIZE = {
    "display": 88,  # 显示级：时钟
    "value":   64,  # 数值级：计算器结果
    "score":   32,  # 游戏得分
    "title":   26,  # 页面标题
    "wintitle": 22,  # 窗口标题
    "card":    26,  # 卡片名
    "btn":     18,  # 按钮文字
    "tab":     15,  # tab
    "label":   14,  # 标签
    "desc":    14,  # 卡片描述
    "hint":    13,  # 提示
    "foot":    11,  # 脚注（下限）
}


# ==================================================================
#  2. 九宫格资源表
# ==================================================================
#
# (资源名, 常态色, 按下色, 圆角档)
# ⚠️ 一种颜色一张图 → 任意尺寸复用。圆角档按控件高度选（见 btn_asset_for）。
BTN_STYLES = [
    # 次级按钮（原 #26313F / #2A3B4F / #1A222E / #1E2A38 一律收归这一档）
    ("ios_btn_gray",     "SURFACE2",   "SURFACE4",   None),
    # 卡片上的次级块（贴在 SURFACE 上，比按钮暗一档）
    ("ios_btn_dark",     "SURFACE",    "SURFACE2",   None),
    # 分段控件选中胶囊
    ("ios_btn_select",   "SURFACE3",   "SURFACE4",   None),
    # 主行动（蓝）
    ("ios_btn_blue",     "ACCENT",     None,         None),
    # 成功 / 开始（绿）
    ("ios_btn_green",    "SUCCESS",    None,         None),
    # 危险（红）
    ("ios_btn_red",      "DANGER",     None,         None),
    # 危险降权（暗红底）
    ("ios_btn_redsoft",  "DANGER_SOFT", None,        None),
    # 数据 / 告警（橙黄）
    ("ios_btn_amber",    "DATA",       None,         None),
    # 高对比（近白底 + 深色字，用于"唯一主按钮"）
    ("ios_btn_white",    "T1",         None,         None),
]

# 圆角档 → 后缀（gen_ui.py 按控件高度选）
#   r8  → 高 < 30（小开关 / 小芯片）
#   r10 → 高 30..47（中按钮）
#   r14 → 高 48..79（大按钮 / 分段胶囊）
#   r20 → 高 ≥ 80（弹窗里的整宽大按钮）
RADIUS_VARIANTS = [("r8", 8), ("r10", 10), ("r14", 14), ("r20", 20)]

# ---- 「烘底」：圆角图的四周必须烘上**它实际坐落的底色**，不能留透明 ----
#
# ⚠️⚠️ 2026-09-14 真机实测（血案）：**运行时 setBackgroundPic() 这条路径不保留 alpha**
#    —— 图里 alpha=0 的像素在屏上渲染成**纯白**，于是每个圆角按钮的四周多出一圈 1px 白描边
#    （圆角的羽化过渡也会白→实色的糊边）。
#    实测对比（同一张 ios_btn_select_r14.9.png）：
#      · JSON 里写 picTab / backgroundPic  → alpha 正确（列表行卡片四角干净、无白边）
#      · 运行时 setBackgroundPic()          → 透明渲染成白（tab 胶囊、音效按钮一圈白）
#    所以：**凡是会被运行时挂图的资源，都必须烘底**。静态的也一起烘，保持一套资源一种行为。
#
# 两种底：
#   page  页面底 #000000  —— 绝大多数按钮（直接坐在页面上）
#   panel 面板底 #1C1C1E  —— 坐在弹窗/面板里的按钮（如暂停弹窗那三个按钮）
# 资源名：`ios_btn_gray_r14.9.png`（page）/ `ios_btn_gray_r14_c.9.png`（panel，c = on-card）
#
# ★★ 2026-09-24：`BAKES` 现在**只给运行时的那套（ios_rt_*）用**。
#    静态九宫格套改烘"透明底"（`STATIC_BAKE = None`），并**不再生成 `_c` 变体** ——
#    圆角外透出的是控件的 `bgColorTab`，由 `gen_ui.inject_rounded()` 按"容器色"填。
#    原因见 `_bake_and_round()` 的长注释（用户报的"黑色倒角"）。
BAKES = {
    "":    "BG",        # 页面底
    "_c":  "SURFACE",   # 面板底
}

# 静态九宫格套的烘底：None = 透明（圆角外交给控件的 bgColorTab）
STATIC_BAKE = None

# 面板 / 卡片类（无按下态，单独一张图）
SURFACE_STYLES = [
    ("ios_card_row",  "SURFACE",  "card"),    # 列表行卡片
    ("ios_panel",     "SURFACE",  "panel"),   # 弹窗 / 面板
    ("ios_panel2",    "SURFACE2", "panel"),   # 次级面板（贴在 SURFACE 上）
    ("ios_seg",       "SURFACE",  "seg"),     # 分段控件容器
    ("ios_seg_sel",   "SURFACE3", "segsel"),  # 选中胶囊
]

# ★ 「面板套面板」用的那张（2026-09-15 新增）
# ⚠️ 上面那批一律烘**页面黑底**（见 gen_surfaces）；而"输入框坐在弹窗卡片里"
#    这种嵌套场合，圆角外的烘底必须是**它坐着的那张面板的色**，否则四角露出一圈黑。
#    命名后缀 `_c`（on-card）与按钮资源的约定一致。
SURFACE_STYLES_ON_CARD = [
    ("ios_panel2_c", "SURFACE2", "panel"),    # #2C2C2E 的次级面板，烘 SURFACE #1C1C1E
]

# 按下态色（比常态提亮；不用纯白，避免"发白光")
PRESS_LIFT = {
    "SURFACE":  "SURFACE2",
    "SURFACE2": "SURFACE4",
    "SURFACE3": "SURFACE4",
}


def hex2rgb(h):
    h = h.lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def tok(name):
    """令牌名 → (r,g,b)"""
    return hex2rgb(TOKENS[name])


def lift(name, amount=46):
    """把颜色向白提亮（按下态用）。返回 (r,g,b)"""
    c = tok(name)
    return tuple(min(255, int(v + (255 - v) * amount / 255.0)) for v in c)


# ==================================================================
#  3. 生成
# ==================================================================
def _import_gen_res():
    if MCP_UI_TOOLS not in sys.path:
        sys.path.insert(0, MCP_UI_TOOLS)
    import gen_res  # noqa: E402
    return gen_res


# ★★ 本文件所有"超采样画完再缩回"一律用 Image.BOX（盒式平均 = 精确覆盖率）。
#    **禁用 LANCZOS / BILINEAR**（2026-09-16 全目录体检发现的系统性瑕疵）：
#    它们是带负瓣/带过冲的重采样核，在硬边两侧会"振铃"——实测 batt_bolt.png 的
#    闪电边缘外侧散落着 α≈17 的一串幽灵像素、app_icon 顶行 α 出现 1,4,0,0,0,1 的抖动。
#    这些像素在深色底上就是隐约的毛刺/锯齿（作者："倒角有锯齿"）。BOX 没有负瓣。
AA_SS = 4   # 抗锯齿超采样倍数（见 _aa_round）


def _aa_round(w, h, radius, fill, ss=None):
    """圆角矩形 → RGBA，边缘为**精确覆盖率**（0..255），过渡带严格 1px。

    ★★ 为什么不用 gen_res.rounded_rect（2026-09-16 真机定位）：
      gen_res 的实现是「1x 直画硬边 + α 通道高斯羽化（σ=0.5）」。高斯不是覆盖率，
      它把硬边**向内向外都抹开**，实测圆角边缘的 RGB 要跨 3~4 个像素才从底色过渡到面色
      —— 肉眼看就是圆角"发虚 + 一圈脏边/黑框"，正是作者反馈的"倒角有锯齿、还有黑框"。

    ★★ 正确做法 = 超采样 + 区域平均：在 ss 倍画布上画**硬边**，再缩回，
      每个像素拿到的就是"该像素被形状覆盖的面积比"（数学精确），过渡带 = 1px。

    ⚠️ 缩回**必须用 BOX，绝不能用 LANCZOS**：LANCZOS 是带负瓣（负权重）的插值核，
      在硬边两侧会产生振铃过冲。实测 `app_icon` 顶行 α 出现 `1, 4, 0, 0, 0, 1, 35` 的
      非单调抖动 —— 那些孤立的小 α 像素渲染到黑底上就是零星的亮点/毛刺，
      视觉上正是"锯齿"。BOX（盒式平均）没有负瓣，等价于精确覆盖率。
    """
    from PIL import Image, ImageDraw
    if ss is None:
        ss = AA_SS
    fill = tuple(fill)
    if radius <= 0:
        return Image.new("RGBA", (w, h), fill + (255,))
    big = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(big).rounded_rectangle(
        [0, 0, w * ss - 1, h * ss - 1], radius=radius * ss, fill=255)
    cov = big.resize((w, h), Image.BOX)
    out = Image.new("RGBA", (w, h), fill + (0,))
    out.putalpha(cov)
    return out


def _bake_and_round(gen_res, size, radius, fill, bake_key):
    """圆角图 → 再烘一层底（返回 RGBA）。`bake_key=None` = **烘透明底**。

    ★★ 2026-09-24 改：**静态九宫格套不再烘黑底，改成烘"透明底"**（用户报"黑色倒角"）。
      原来烘黑底是把"圆角外"的像素**烧成不透明黑**，于是：
        · 图挂在**页面**上（黑）→ 正好，看不出来（所以一直没被发现）；
        · 图挂在**卡片/面板/弹窗**上 → 圆角外那一圈还是黑的 ⇒ **四个黑倒角**。
      而"透明底"把圆角外交给**控件的 bgColorTab**（本工程 `inject_rounded` 现在会把它
      设成**容器色**）⇒ 一种素材适配任意容器色，**不需要为每种容器色各出一套 `_c` 变体**。

      为什么这条路成立（实测证据，不是推测）：
        ① `ui/main.html` 的 `GridCard`/桌面图标（`app_icon_*.png`，圆角外 α=0）走 JSON
           路径渲染，角上是**纯黑**=底下的页面色，不是白 —— 说明 alpha 被正确合成；
        ② ha 完成页的 `ha_done.png`（绿圆，圆角外 α=0）同结论；
        ③ 最强的证据：半透明遮罩 `sheet_mask.png`（α=204）挂在 picTab 上，
           实测背景被压暗成 `原色 × 0.2` —— **这就是 alpha 合成在工作**。
      ⚠️ 只有**运行时 `setBackgroundPic()`** 那条路不保留 alpha（渲染成白）⇒
         `ios_rt_*.png`（gen_runtime_buttons）**必须继续烘不透明底**，见那里的注释。
      ⚠️ 九宫格 marker 边不受影响：它由 `gen_res.to_9patch()` 后加，恒为纯黑不透明
         （`tools/ios_theme.py` 的 main() 有自检）。
    """
    from PIL import Image
    body = _aa_round(size, size, radius, fill)
    base = (0, 0, 0, 0) if bake_key is None else (hex2rgb(TOKENS[bake_key]) + (255,))
    canvas = Image.new("RGBA", (size, size), base)
    canvas.alpha_composite(body)
    return canvas


def gen_buttons(gen_res, out_dir):
    """按钮九宫格两态图（静态套）。

    命名：`ios_btn_<style>_<rN>.9.png` / `..._p.9.png`
    ★★ 2026-09-24：**只出一套（烘透明底）**，不再出 `_c`（面板底）变体 ——
      圆角外是透明的，透出控件的 `bgColorTab`；`gen_ui.inject_rounded()` 会把它
      按"按钮坐在什么容器上"填成**容器色**。这样任意容器色都不用各出一套图。
      （原来 `_c` 存在的原因就是"烘死黑底后换个容器就露黑角"，现在从根上没有了。）
    """
    n = 0
    for name, normal, pressed, _ in BTN_STYLES:
        for suffix, radius in RADIUS_VARIANTS:
            # 内容尺寸要 > 2*radius，否则圆角区退化（拉伸段为负）
            size = max(48, radius * 4)
            gen_res.to_9patch(
                _bake_and_round(gen_res, size, radius, tok(normal), STATIC_BAKE),
                radius, out_dir, "%s_%s.9.png" % (name, suffix))
            n += 1
            # 按下态：iOS 是"变亮"而不是"变暗"
            pc = tok(pressed) if pressed else lift(normal)
            gen_res.to_9patch(
                _bake_and_round(gen_res, size, radius, pc, STATIC_BAKE),
                radius, out_dir, "%s_%s_p.9.png" % (name, suffix))
            n += 1
    return n


def gen_surfaces(gen_res, out_dir):
    """面板/卡片类圆角图（一次静态尺寸即可，九宫格保证可拉伸）。

    ★★ 2026-09-24：一律烘 **透明底**（原来烘页面黑底 / 面板底）。
      两张"坐错底就露一圈别的颜色"的老血案（`ios_panel2_c` 烘 SURFACE、
      `ios_card_row` 烘黑底）因此从根上消失：圆角外透出的是**控件的 bgColorTab**。
      `ios_panel2_c` 保留（`data-round="panel2c"` 仍指向它），内容已与 `ios_panel2`
      等价 —— 命名留着是为了不动源稿与 `ROUND_ASSETS`。
    """
    n = 0
    for name, color_key, rkey in SURFACE_STYLES:
        r = RADIUS[rkey]
        size = max(64, r * 4)
        gen_res.to_9patch(
            _bake_and_round(gen_res, size, r, tok(color_key), STATIC_BAKE),
            r, out_dir, name + ".9.png")
        n += 1
    for name, color_key, rkey in SURFACE_STYLES_ON_CARD:
        r = RADIUS[rkey]
        size = max(64, r * 4)
        gen_res.to_9patch(
            _bake_and_round(gen_res, size, r, tok(color_key), STATIC_BAKE),
            r, out_dir, name + ".9.png")
        print("  %s.9.png（烘透明底）" % name)
        n += 1
    return n


# ==================================================================
#  运行时按钮：普通 PNG（**不是**九宫格）
# ==================================================================
def gen_runtime_buttons(gen_res, out_dir, sizes):
    """给"运行时会被改底色"的按钮生成普通圆角 PNG（尺寸与控件严格相等）。

    ⚠️⚠️ 为什么这批不能用九宫格（2026-09-14 真机定位）：
      `.9.png` 四周必须有 1px 的 marker 边（编码拉伸区，见 nine-patch-rule.md）。
      **JSON 路径**（picTab / backgroundPic 字段）会正确消费这条边：内容内缩 1px、边不画。
      但**运行时 `setBackgroundPic()` 路径会把这条 1px 透明边渲染成白线**
      —— 实测：段容器（JSON picTab）边缘干净；tab 胶囊 / 音效按钮（运行时挂图）
      最外一圈恒为 #FFFFFF，无论图里烘什么底色都盖不住（它画在内容之外）。
      ⇒ 运行时挂的图必须是**没有 marker 边的普通 PNG**，控件尺寸 == 图尺寸。
      代价：一种 (样式, 尺寸) 要一张图。尺寸由 gen_ui 从源稿里扫 `data-noround` 收集，
      所以**不会漏**（新增运行时按钮时源稿一标，这里就自动多出一张）。

    命名：`ios_rt_<style>_<W>x<H><bake>.png` / `..._p.png`
      bake = ""（页面黑底，直接坐在页面上）/ "_c"（面板底 #1C1C1E）

    ★ 为什么同一 (样式,尺寸) 要两套（2026-09-14 第二版）：
      烘底把"圆角的抗锯齿像素"与原底色绑死了。图坐错底，边缘就会露出一圈别的颜色
      —— 肉眼看就是**锯齿/脏边**（血案：tab 胶囊烘的是页面黑底，却坐在
      `#1C1C1E` 的段容器上 ⇒ 胶囊四周一圈黑）。所以调用方必须说清"我坐在什么上"。
    """
    n = 0
    if not sizes:
        return 0
    for style, _normal, _pressed, _ in BTN_STYLES:
        short = style.replace("ios_btn_", "")   # 与 PgSkin.cpp 的 kStyles 短名一致
        for (w, h) in sorted(sizes):
            radius = RADIUS["btnS"] if h < 30 else (RADIUS["btnM"] if h < 48
                                                   else (RADIUS["btnL"] if h < 80
                                                         else RADIUS["panel"]))
            radius = min(radius, min(w, h) // 2)
            normal = tok(_normal)
            pc = tok(_pressed) if _pressed else lift(_normal)
            for bake, bake_key in BAKES.items():
                for suffix, color in (("", normal), ("_p", pc)):
                    img = _aa_rounded_on_bake(gen_res, w, h, radius, color, bake_key)
                    name = "ios_rt_%s_%dx%d%s%s.png" % (short, w, h, suffix, bake)
                    img.save(os.path.join(out_dir, name))
                    n += 1
    return n


def _aa_rounded_on_bake(gen_res, w, h, radius, fill, bake_key):
    """圆角矩形（任意宽高，非正方）→ 烘上不透明底色。AA 用 _aa_round（精确覆盖率）。"""
    from PIL import Image
    body = _aa_round(w, h, radius, fill)
    canvas = Image.new("RGBA", (w, h), hex2rgb(TOKENS[bake_key]) + (255,))
    canvas.alpha_composite(body)
    return canvas


# ---- 主界面网格：整格点击区（尺寸必须 == ui/main.html 的 GridCard 控件）----
GRID_CELL_W, GRID_CELL_H = 112, 107

# ---- 电池：**一枚整图 = 一个完整状态（外壳 + 电量条 + 充电闪电）** ----
# ★★ 2026-09-16 改版（用户原话：「电池图标不对。做成对应的图片直接贴就不存在错位了」）：
#   上一版把电池拆成 **3 个控件**（外壳 52x26 + 电量条 40x20 + 闪电 22x26），
#   电量条的位置由 `navibar.cc` 的常量在**运行时** setPosition 决定 —— 而 html 里
#   声明的是 (381,16)，常量却写成了 y=5 ⇒ **电量条比外壳高 11px、顶出壳外**。
#   这正是"错位"的来源：**同一份几何有两个来源（声明的 + 运行时常量），必然漂**。
#   ⇒ 现在**烘成一张 52x26 的整图**，控件与图 1:1，运行时只 `setBackgroundPic`
#     **一图一状态，永不 setPosition** ⇒ 结构上不可能错位。
#
#   状态空间（刻意收敛到 10% 一档 = 11 档，够用且不至于几百张图）：
#     色   blue=正常 / red=低电 / amber=充电（闪电**烘在 amber 里**，因为
#          "充电 ⇒ amber" 是同一件事的两种表现，分开两个控件才会又出现"对不齐"）
#     档   0,10,20,...,100 → 电量条宽 0,4,8,...,40（正好 4px 一档，不出现"半像素"）
#   ⇒ 3 × 11 = 33 张，替换掉旧的 122 张（外壳 1 + 电量条 120 + 闪电 1），/res 反而更小。
BATT_W, BATT_H = 52, 26          # == ui/navibar.html 的 ImgNbBatt 控件盒（1:1，不能改）
BATT_FILL_W, BATT_FILL_H = 40, 20   # 电量条满格尺寸（外壳内缘，52x26 里左右各留 3px）
BATT_INSET = 3                   # 电量条在外壳内的左上内缩（描边 2px + 1px 间隙）
BATT_LEVELS = tuple(range(0, 101, 10))
BATT_FILL_COLORS = {
    "blue":   "ACCENT",   # 正常
    "red":    "DANGER",   # 低电
    "amber":  "DATA",     # 充电 / 已充满（这一档烘了闪电）
}
BATT_BOLT_IN_AMBER = True        # amber 档把闪电烘进图里


def gen_grid_cell(gen_res, out_dir):
    """主界面网格的"整格点击区"底图：112x107 的**不透明纯黑**。

    为什么需要它（2026-09-15 修"主界面点不动"）：
      网格里图标只有 72x72，格的其余部分（名称/留白）没有任何可点控件 ⇒ 点那儿像"没触摸"。
      补一个与格同尺寸的按钮当命中区，但 html2json 对**子项**按钮会丢掉 `data-bg`
      （子项只认 data-pic/data-bgpic/src）⇒ 只能给它一张图。
      图用 #010101（不是 #000000 —— 那个会被当成"没设置"变透明）：
      在纯黑页面底上肉眼不可见，但**不透明**，确保框架把它当正常控件绘制与命中。
    """
    from PIL import Image
    w, h = GRID_CELL_W, GRID_CELL_H
    im = Image.new("RGB", (w * 4, h * 4), hex2rgb(TOKENS["BG"]))
    gen_res.save(im.resize((w, h), Image.BOX), out_dir, "grid_cell.png")
    return 1


def _batt_bolt_polygon(ss):
    """充电闪电的顶点（在 22x26 的局部坐标里，乘上超采样倍率）。"""
    return [(13 * ss, 1 * ss), (4 * ss, 14 * ss), (10 * ss, 14 * ss),
            (8 * ss, 25 * ss), (18 * ss, 11 * ss), (12 * ss, 11 * ss)]


def gen_battery(gen_res, out_dir):
    """**一枚整图 = 一个完整电池状态**：`batt_<色>_<电量档>.png`（52x26，3 色 × 11 档）。

    ⚠️ 都**烘到页面底色 #000000 上**（不透明）：
       MCP `devflow/pixel-analysis-ai.md` 的实测坑——半透明图贴纯色底会发脏，
       需要真透明装饰件要用 button+picTab。电池就在标题栏的纯黑底上，烘黑最稳。
    ⚠️ 图尺寸必须 **== 控件尺寸（52x26）**：不等时框架会缩放 ⇒ 圆角描边发糊/锯齿
      （工程铁律，`tools/check_stretch.py` 会抓）。
    ⚠️ 旧文件要**主动清掉**（下面的 stale 清理）：留着只是白占 /res，
       而且会让"到底哪张在生效"变得可疑。
    """
    from PIL import Image, ImageDraw

    SS = 4
    n = 0
    stale = []
    for short in BATT_FILL_COLORS:
        for lv in BATT_LEVELS:
            fn = "batt_%s_%d.png" % (short, lv)
            # 电量条宽度：与档位严格对应（10% → 4px），窄条圆角收敛避免退化成尖角
            w = BATT_FILL_W * lv // 100
            r = min(5, w // 2, BATT_FILL_H // 2) if w > 0 else 0

            im = Image.new("RGB", (BATT_W * SS, BATT_H * SS), hex2rgb(TOKENS["BG"]))
            d = ImageDraw.Draw(im)
            # ① 外壳：圆角描边 + 正极头（与旧版逐参数一致，观感不变）
            d.rounded_rectangle([1 * SS, 1 * SS, 45 * SS, 25 * SS], radius=9 * SS,
                                outline=hex2rgb(TOKENS["T2"]), width=2 * SS)
            d.rounded_rectangle([47 * SS, 9 * SS, 51 * SS, 17 * SS], radius=2 * SS,
                                fill=hex2rgb(TOKENS["T2"]))
            # ② 电量条：外壳内缘内缩 2px（旧版靠"另一个控件的偏移"达成，现在烘进图里）
            # ⚠️ `rounded_rectangle` 的右下角坐标是**排他**的（实测：给 [3,3,42,22] 只画出
            #    39x19）⇒ 要"正好 w x 20"必须给 `[3, 3, 3+w, 23]`。
            if w > 0:
                d.rounded_rectangle([BATT_INSET * SS, BATT_INSET * SS,
                                     (BATT_INSET + w) * SS,
                                     (BATT_INSET + BATT_FILL_H) * SS],
                                    radius=r * SS, fill=tok(BATT_FILL_COLORS[short]))
            # ③ 充电闪电：烘在 amber 档里，**白色不透明**（直接盖在电量条上）
            if short == "amber" and BATT_BOLT_IN_AMBER:
                d.polygon(_batt_bolt_polygon(SS), fill=(255, 255, 255))

            im.resize((BATT_W, BATT_H), Image.BOX).save(os.path.join(out_dir, fn))
            # ★ 生成器自检：**图尺寸必须 == 控件尺寸**（不等时框架缩放 ⇒ 描边发糊）。
            #   这条工程铁律已经栽过多次，写在这里比指望 check_stretch 更早发现。
            im2 = Image.open(os.path.join(out_dir, fn))
            if im2.size != (BATT_W, BATT_H):
                raise SystemExit("电池图尺寸错：%s = %s，应为 %dx%d"
                                 % (fn, im2.size, BATT_W, BATT_H))
            n += 1

    # ---- 清理旧素材（拆成 3 控件的时代产物）----
    import glob
    for pat in ("batt_shell.png", "batt_bolt.png", "batt_fill_*_*.png"):
        for p in glob.glob(os.path.join(out_dir, pat)):
            stale.append(p)
    for p in stale:
        try:
            os.remove(p)
        except OSError:
            pass
    if stale:
        print("    清掉旧电池素材 %d 张（外壳/电量条/闪电三控件时代）" % len(stale))
    return n


# ==================================================================
#  iOS 风格开关（UISwitch）+ 列表「整组卡」行底图
# ==================================================================
# 开关控件盒（必须与 ui/wifi.html 里的 CbWifiOn 严格 1:1 —— 图会被 check_stretch 查）：
SWITCH_W, SWITCH_H = 72, 44
SWITCH_PAD = 3                      # 圆钮与滑轨边缘的间隙（iOS 是 2pt）
ROW_CARD_R = 20                     # 整组卡圆角（与 RADIUS["panel"] 同档）
ROW_CARD_H = 88                     # 行高。⚠️ **必须等于 listview 算出来的 item 高**：
                                    #   item 高 = (列表控件高 440 - 行距 0×(rows-1)) / rows(5) = 88。
                                    #   对不上的话每行底部会漏一条黑缝（行卡比行矮）。
ROW_CARD_W = 448                    # 行宽 == ui/wifi.html 的列表/item 宽（非九宫格，图必须 1:1）
ROW_LINE_X0 = 16                    # 行内分隔线左端（与行内文字 x 对齐）


def gen_switch(gen_res, out_dir):
    """iOS 风格开关的两态图（switch_off.png / switch_on.png，72x44）。

    为什么做成"位图两态"而不是自己画：ZKCheckBox 原生支持
    `picTab{pic0: 未选中, pic2: 选中}`（见 MCP `ui_tools/html2json.py` 的 checkbox 分支），
    框架按 checked 自己切图 —— 点按判定、选中回调、防回环全都不用在应用层操心。
    画布自绘则违反「非游戏禁止自定义绘图」铁律。

    ⚠️ 烘底用的是**卡片色**（SURFACE #1C1C1E）而不是页面黑：开关坐在
       「无线局域网」那行卡片里，烘错底会在四个圆角外圈露一圈别的颜色（脏边）。
    """
    from PIL import Image, ImageDraw
    n = 0
    SS = 4
    W, H = SWITCH_W * SS, SWITCH_H * SS
    knob_r = (SWITCH_H - 2 * SWITCH_PAD) // 2
    for name, color_key, knob_left in (
            ("off", "SURFACE3", SWITCH_PAD),
            ("on", "SUCCESS", SWITCH_W - SWITCH_PAD - 2 * knob_r)):
        im = Image.new("RGB", (W, H), hex2rgb(TOKENS["SURFACE"]))
        d = ImageDraw.Draw(im)
        d.rounded_rectangle([0, 0, W - 1, H - 1], radius=H // 2, fill=tok(color_key))
        cx, cy, r = (knob_left + knob_r) * SS, H // 2, knob_r * SS
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255))
        im.resize((SWITCH_W, SWITCH_H), Image.BOX).save(
            os.path.join(out_dir, "switch_%s.png" % name))
        print("  switch_%s.png (%dx%d)" % (name, SWITCH_W, SWITCH_H))
        n += 1
    return n


def _row_card_img(w, h, radius, top, bottom):
    """整组卡的一段角：上圆角 / 下圆角 / 上下都直角 / 四角圆角，并烘页面黑底。

    ⚠️ 分隔线**画进图里**（不单独做控件）：listview 的 subItem 分支只认 data-bgpic /
       data-icon，`data-bg` 会被忽略（见 html2json.py 的 subItem 分支），
       所以"用 1px 控件画线"这条路走不通。画进底图反而更省：一张图管一件事。
      分隔线画在**缩放之后**的 1px 上（图高 == 行高，所以它正好落在行底最后一行）。
    """
    from PIL import Image, ImageDraw
    SS = 4
    W, H = w * SS, h * SS
    r = radius * SS
    fill = tok("SURFACE")
    body = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    if top and bottom:
        d.rounded_rectangle([0, 0, W - 1, H - 1], radius=r, fill=fill)
    elif top:
        d.rounded_rectangle([0, 0, W - 1, 2 * r - 1], radius=r, fill=fill)
        d.rectangle([0, r, W - 1, H - 1], fill=fill)
    elif bottom:
        d.rounded_rectangle([0, H - 2 * r, W - 1, H - 1], radius=r, fill=fill)
        d.rectangle([0, 0, W - 1, H - r - 1], fill=fill)
    else:
        d.rectangle([0, 0, W - 1, H - 1], fill=fill)
    body = body.resize((w, h), Image.BOX)
    canvas = Image.new("RGBA", (w, h), hex2rgb(TOKENS["BG"]) + (255,))
    canvas.alpha_composite(body)
    if not bottom:   # 末行不画分隔线（iOS 分组里最后一行底下没有线）
        ImageDraw.Draw(canvas).rectangle(
            [ROW_LINE_X0, h - 1, w - 1, h - 1], fill=hex2rgb(TOKENS["SEP"]) + (255,))
    return canvas


def gen_row_cards(gen_res, out_dir):
    """列表「整组卡」的四段角（ios_row_top / mid / bot / solo）——**普通 PNG，不是九宫格**。

    为什么是四张图：iOS 设置的分组列表是**一整张卡**（首行上圆角、中间行直角、
    末行下圆角、行间 1px 分隔线），而 listview 的 item 模板只有一份、做不出"按位置换底"。
    所以出四张图，由 wifiLogic 在 obtainListItemData 里按 index 切 visible
    —— 与主界面 Icon0..23 按 slot 切图是同一手法（见 mainLogic.cc 的 syncRowIcon）。

    ⚠️⚠️ 为什么**不能**用九宫格（2026-09-15 真机实测，血案）：九宫格那圈 1px marker 边
       会被算进缩放 —— 图内容 88 高、控件也 88 高时，实际只画到 86，**每行底下留 2px 黑**
       （真机像素扫描：行高 88 / 面板 85px + 分隔线 1px + 黑 2px），行与行之间成了一道黑缝。
       行底图本来就是"宽度由列表定死、不需要横向拉伸"的东西 ⇒ 直接出
       **图尺寸 == 控件尺寸（ROW_CARD_W x ROW_CARD_H）** 的普通 PNG：
       1:1 贴图没有解析不确定性，还顺带走"整图不透明 → memcpy"的快路径。
    ⚠️ 代价：列表宽度改了要回来改 ROW_CARD_W 重跑（check_stretch 会查出来）。
    """
    n = 0
    for name, top, bottom in (("top", True, False), ("mid", False, False),
                              ("bot", False, True), ("solo", True, True)):
        r = 0 if (not top and not bottom) else ROW_CARD_R
        img = _row_card_img(ROW_CARD_W, ROW_CARD_H, r, top, bottom)
        img.save(os.path.join(out_dir, "ios_row_%s.png" % name))
        print("  ios_row_%s.png (%dx%d)" % (name, ROW_CARD_W, ROW_CARD_H))
        n += 1
    return n


# ---- 列表行的两枚状态图标：三段弧信号强度 + 加粗锁 ----
AP_ICON = 28          # 图标边长。⚠️ 必须 == ui/wifi.html 里那两个 subItem 的宽高
                      #   （非九宫格图，check_stretch.py 会查"图尺寸 == 控件尺寸"）

# (资源名, 点亮段数, 颜色令牌) —— 按 rssi 分档，运行时只显示一张
SIG_TIERS = (
    ("sig_3", 3, "SUCCESS"),    # rssi >= -65：满格 / 强
    ("sig_2", 2, "TEAL"),       # rssi >= -75：中
    ("sig_1", 1, "TEAL"),       # rssi >= -85：弱
    ("sig_0", 1, "SURFACE4"),   # 更差：只剩最内一段、灰
)


def gen_ap_icons(gen_res, out_dir):
    """WiFi 列表行右侧的两枚状态图标（28x28，普通 PNG，图尺寸 == 控件尺寸）。

    ★ 为什么自己画、而不是用 `data-icon="lock"` / 文字（2026-09-15 用户反馈
      "信号强度和加密方式图标有点太过于抽象了"）：
        · `gen_res` 的 `lock` glyph 是 24 单位画布上的**细线框**，渲染到 22px 只剩
          12x14 的墨迹、线宽 1~2px —— 在 480x800 上就是"看不清的小图案"；
        · 信号强度原来干脆是**文字**（"满格/强/中/弱"），要"读"而不是"扫一眼"。
      改成：① **三段弧线**点亮 1/2/3 段 = 信号强弱（与顶栏 WiFi 图标、信号探针
      同一套符号语言）；② **锁加粗成实心块 + 粗锁环**（一眼看到"这是加密网络"）。

    ⚠️ 烘卡片底（SURFACE）：图标坐在行卡片（#1C1C1E）上，烘错底四角会露一圈脏边。
    ⚠️ 四档信号图叠在同一位置，运行时按 rssi 只显示一张（见 wifiLogic 的 subVisible）。
    """
    import math
    from PIL import Image, ImageDraw
    n = 0
    SS = 6                      # 超采样：弧线与圆弧端点要干净
    S = AP_ICON * SS
    cx, cy = 14.0, 20.5         # 弧线的圆心（底部圆点所在的中心）
    radii = (5.0, 9.5, 14.0)    # 内→外三段
    a0, a1 = 210.0, 330.0       # 上半弧的张角（避开画布边缘）
    lw = 2.6                    # 弧线宽（比 gen_res 的细线框粗一倍多）

    def arc_ends(r):
        p0 = (cx + r * math.cos(math.radians(a0)), cy + r * math.sin(math.radians(a0)))
        p1 = (cx + r * math.cos(math.radians(a1)), cy + r * math.sin(math.radians(a1)))
        return p0, p1

    for name, lit, color_key in SIG_TIERS:
        im = Image.new("RGB", (S, S), hex2rgb(TOKENS["SURFACE"]))
        d = ImageDraw.Draw(im)
        col = tok(color_key)
        for i, r in enumerate(radii):       # i=0 是最内那一段
            if i >= lit:                    # 未点亮的段：留空（不是画灰，避免糊成一团）
                continue
            d.arc([(cx - r) * SS, (cy - r) * SS, (cx + r) * SS, (cy + r) * SS],
                  a0, a1, fill=col, width=int(lw * SS))
            for p in arc_ends(r):           # 圆头端点（PIL 的 arc 只有平头）
                d.ellipse([(p[0] - lw / 2) * SS, (p[1] - lw / 2) * SS,
                           (p[0] + lw / 2) * SS, (p[1] + lw / 2) * SS], fill=col)
        rd = 2.2                            # 底部圆点
        d.ellipse([(cx - rd) * SS, (cy - rd) * SS, (cx + rd) * SS, (cy + rd) * SS], fill=col)
        im.resize((AP_ICON, AP_ICON), Image.BOX).save(
            os.path.join(out_dir, name + ".png"))
        print("  %s.png (%dx%d)" % (name, AP_ICON, AP_ICON))
        n += 1

    # 加粗实心锁：粗锁环（半圆描边）+ 实心锁体 + 卡片色挖出的钥匙孔
    im = Image.new("RGB", (S, S), hex2rgb(TOKENS["SURFACE"]))
    d = ImageDraw.Draw(im)
    col = tok("T2")
    body_top, body_bot = 12.0, 23.0
    d.arc([(14 - 4.5) * SS, (12 - 4.5) * SS, (14 + 4.5) * SS, (12 + 4.5) * SS],
          180, 360, fill=col, width=int(2.6 * SS))
    d.rounded_rectangle([6 * SS, body_top * SS, 22 * SS, body_bot * SS],
                        radius=3 * SS, fill=col)
    d.ellipse([(14 - 1.6) * SS, (17 - 1.6) * SS, (14 + 1.6) * SS, (17 + 1.6) * SS],
              fill=hex2rgb(TOKENS["SURFACE"]))
    d.rectangle([(14 - 0.8) * SS, 17 * SS, (14 + 0.8) * SS, 20.5 * SS],
                fill=hex2rgb(TOKENS["SURFACE"]))
    im.resize((AP_ICON, AP_ICON), Image.BOX).save(os.path.join(out_dir, "lock.png"))
    print("  lock.png (%dx%d)" % (AP_ICON, AP_ICON))
    n += 1
    return n


def btn_asset_for(hexcolor, height, on_card=False):
    """按控件底色 + 高度，返回该挂哪个九宫格（gen_ui.py 用）。

    返回 (normal, pressed) 资源名（相对 resources/images/），找不到返回 None。

    ★★ 2026-09-24：`on_card` 参数**保留但不再影响结果**（只出一套图，烘透明底）。
      原来靠 `_c` 后缀区分"坐在页面上 / 坐在面板上"，但那只覆盖 #1C1C1E 一种容器色；
      现在圆角外是透明的，透出控件 `bgColorTab`（由 gen_ui 按**容器实际底色**填）
      ⇒ 任意容器色都正确，也就不再需要两套图。
      保留参数是为了不打断调用方（gen_ui / 老脚本），语义写在这里以免误用。
    """
    if not hexcolor:
        return None
    want = hexcolor.upper().lstrip("#")
    for name, normal, _pressed, _ in BTN_STYLES:
        if TOKENS[normal].upper().lstrip("#") == want:
            break
    else:
        return None
    # 圆角档按高度选（与 RADIUS_VARIANTS 对应）
    if height >= 80:
        suffix = "r20"
    elif height >= 48:
        suffix = "r14"
    elif height >= 30:
        suffix = "r10"
    else:
        suffix = "r8"
    return ("%s_%s.9.png" % (name, suffix),
            "%s_%s_p.9.png" % (name, suffix))


def gen_all(out_dir, runtime_sizes=None):
    """生成全套资源。runtime_sizes = 运行时按钮的 (宽,高) 集合（gen_ui 从源稿扫出来）。"""
    gen_res = _import_gen_res()
    os.makedirs(out_dir, exist_ok=True)
    print("iOS 深色令牌 → 资源（输出 %s）" % out_dir)
    n = 0
    n += gen_buttons(gen_res, out_dir)
    n += gen_surfaces(gen_res, out_dir)
    n += gen_runtime_buttons(gen_res, out_dir, runtime_sizes or set())
    n += gen_battery(gen_res, out_dir)
    n += gen_grid_cell(gen_res, out_dir)
    n += gen_switch(gen_res, out_dir)      # WiFi 页的滑轨开关（两态）
    n += gen_row_cards(gen_res, out_dir)   # WiFi 页列表的整组卡四段角
    n += gen_ap_icons(gen_res, out_dir)    # WiFi 列表行右侧：信号弧线 ×4 + 加粗锁
    print("共生成 %d 个资源" % n)
    return n


def main():
    n = gen_all(OUT_DIR, set())

    # 自检：九宫格四边 marker 必须是**纯黑不透明**（nine-patch-rule 规则 1），
    # 且圆角区不能落 marker（规则 2）。
    from PIL import Image
    bad = 0
    for f in sorted(os.listdir(OUT_DIR)):
        if not f.endswith(".9.png"):
            continue
        im = Image.open(os.path.join(OUT_DIR, f))
        w, h = im.size
        px = im.load()
        if px[w // 2, 0][:3] != (0, 0, 0) or px[w // 2, 0][3] != 255:
            print("  !! %s top marker 不是纯黑不透明" % f)
            bad += 1
        if px[0, h // 2][:3] != (0, 0, 0) or px[0, h // 2][3] != 255:
            print("  !! %s left marker 不是纯黑不透明" % f)
            bad += 1
    print("九宫格 marker 自检：%s" % ("失败 %d 处" % bad if bad else "通过"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
