#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_ha_tiles.py - 智能家居页的「类型控制图」（域 × 状态）

为什么要有它（2026-09-23 用户需求）：
  「扫描出来的不同类型比如 switch、light 这些就做成不同的控制图片效果」
  ⇒ 每个 domain 一套自己的图形，并且**同一套图形有三种状态外观**（亮/灭/灰）。

★★ 三条硬约束（都是本项目踩出来的，别按直觉改）：

  ① **图片必须是不透明的（不能有 alpha 通道）**。
     `ZKButton::setBackgroundPic()` 在**运行时**走的渲染路径**不保留 alpha**
     ——透明像素会被渲染成纯白。图标若是透明底，换图后就是一整块白方块
     （statusbar.cc 里"两个按钮叠一格"的写法就是被这条逼出来的）。
     所以这里画的是 **RGB 图**，四角用 `CARD_BG` 填死，靠**四角填卡片底色**来"假装圆角"。
     ⚠️ 因此 **卡片底色一变，这里必须跟着改并重跑**（CARD_BG 是唯一真值）。

  ② **状态只有三档**：on / off / flat（离线 unavailable|unknown、以及非开关态的数值）。
     实测这台 HA 34 个实体里只有 4 个是真 on/off ⇒ 三档之外的信息全部交给**文字**。
     如果给每个 state 都烘一张图，图档会爆 `/res` 分区
     （实测分区 7,995,392 字节，现在只剩 ~330KB 余量）。

  ③ **必须用 BOX 缩小，不能用 LANCZOS**。LANCZOS 带负瓣，对硬边图形缩小时会"振铃"
     （app_icon 的血案：α 上升过程中反复跌回 0，边缘散落孤立亮点 = 肉眼可见毛刺）。
     这里虽然是不透明图，但圆角/线端同样会被振铃影响，所以照抄 BOX。

输出：resources/images/ha_tile_<域>_<on|off|flat>.png（13 域 × 3 档 = 39 张）
     + docs/ha-tiles-sheet.png（联络表：39 张拼成一张，方便一眼看全，不参与打包）

用法：python tools/gen_ha_tiles.py
"""

import math
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT_DIR = os.path.join(ROOT, "resources", "images")
SHEET = os.path.join(ROOT, "docs", "ha-tiles-sheet.png")

SIZE = 72          # 与控件尺寸 1:1（本工程铁律：素材不拉伸）
SS = 4             # 超采样倍率：圆角/线端要精确覆盖率
ART = 40           # 图形所在的正方形边长（居中）
OX = (SIZE - ART) // 2.0

# ★ 唯一真值：卡片底色。图标四角填它，才能骗过眼睛显示成"圆角图形"。
#   改 ui/ha.html 里磁贴的 data-bg 时，这里要同步改 + 重跑本脚本。
CARD_BG = (28, 28, 30)          # #1C1C1E

# 三种状态的底板色 / 图形色
STATE_STYLE = {
    #            底板              图形
    "on":   ((58, 58, 60), None),              # 底板用域色（压暗后），图形白
    "off":  ((58, 58, 60), (110, 110, 116)),   # #3A3A3C 底 + 中灰图形
    "flat": ((36, 36, 38), (85, 85, 90)),      # #242426 底 + 暗灰图形
}

# ★ on 态底板用"域色 × 这个系数"。
#   为什么不直接用满色（第一版就是这么做的）：满色底 + **白**图形的灰度对比只有
#   1.6~1.8:1（琥珀/青 这些亮色尤其糟），72px 的小图上图形会"糊"在底里。
#   压暗到 ~0.72 后白图形跳到 2.2:1 以上，同时"亮"的感觉还在（实测平均亮度仍
#   比 off 态高 60+，脚本的自检 ④ 就是量这个比例）。
ON_DARKEN = 0.72

# 域 → (颜色, 画法)。顺序 = /api 里域的常见程度，也决定联络表的行序。
DOMAINS = [
    ("light",         "#FFB100", "g_light"),
    ("switch",        "#0A84FF", "g_switch"),
    ("sensor",        "#64D2FF", "g_sensor"),
    ("binary_sensor", "#5E5CE6", "g_binary_sensor"),
    ("scene",         "#BF5AF2", "g_scene"),
    ("button",        "#FF9F0A", "g_button"),
    ("climate",       "#40C8E0", "g_climate"),
    ("cover",         "#A2845E", "g_cover"),
    ("fan",           "#30D158", "g_fan"),
    ("lock",          "#FF453A", "g_lock"),
    ("camera",        "#8E8E93", "g_camera"),
    ("media_player",  "#FF375F", "g_media"),
    ("other",         "#8E8E93", "g_other"),
]

STATES = ["on", "off", "flat"]


# ==================================================================
#  画法：每个函数在 (x, y, s) 给出的正方形里用**归一化比例**作图
#  d = ImageDraw；坐标换算全在 _pt/_box 里，画法本身只认比例
# ==================================================================

def _pt(x, y, s, fx, fy):
    return (x + fx * s, y + fy * s)


def _box(x, y, s, fx0, fy0, fx1, fy1):
    return [x + fx0 * s, y + fy0 * s, x + fx1 * s, y + fy1 * s]


def g_light(d, x, y, s):
    """灯泡：泡 + 螺旋颈 + 光芒。"""
    d.ellipse(_box(x, y, s, 0.28, 0.16, 0.72, 0.60), fill=255)
    d.rectangle(_box(x, y, s, 0.42, 0.58, 0.58, 0.72), fill=255)
    d.rectangle(_box(x, y, s, 0.38, 0.74, 0.62, 0.80), fill=255)
    for ang in (0, 45, 90, 135, 180, 225, 270, 315):
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        if abs(dy) > 0.7:      # 上下方向的光芒和灯泡本体挤在一起，画短一点
            r0, r1 = 0.48, 0.56
        else:
            r0, r1 = 0.44, 0.56
        d.line([_pt(x, y, s, 0.5 + dx * r0, 0.40 + dy * r0),
                _pt(x, y, s, 0.5 + dx * r1, 0.40 + dy * r1)],
               fill=255, width=int(0.055 * s))


def g_switch(d, x, y, s):
    """拨钮：胶囊 + 滑块（开关最具辨识度的形状）。"""
    d.rounded_rectangle(_box(x, y, s, 0.12, 0.32, 0.88, 0.68),
                        radius=0.18 * s, outline=255, width=int(0.075 * s))
    d.ellipse(_box(x, y, s, 0.56, 0.38, 0.82, 0.62), fill=255)
    d.line([_pt(x, y, s, 0.24, 0.50), _pt(x, y, s, 0.40, 0.50)],
           fill=255, width=int(0.075 * s))


def g_sensor(d, x, y, s):
    """温度计：管 + 球 + 刻度（比"水滴水"更明确是"读数"）。"""
    d.line([_pt(x, y, s, 0.42, 0.14), _pt(x, y, s, 0.42, 0.62)],
           fill=255, width=int(0.12 * s))
    d.ellipse(_box(x, y, s, 0.24, 0.58, 0.60, 0.90), fill=255)
    for fy in (0.26, 0.38, 0.50):
        d.line([_pt(x, y, s, 0.56, fy), _pt(x, y, s, 0.74, fy)],
               fill=255, width=int(0.06 * s))


def g_binary_sensor(d, x, y, s):
    """门磁/触点：空心环 + 中心实点 + 四个尖角（"检测到/未检测到"）。"""
    d.ellipse(_box(x, y, s, 0.26, 0.26, 0.74, 0.74), outline=255,
              width=int(0.075 * s))
    d.ellipse(_box(x, y, s, 0.42, 0.42, 0.58, 0.58), fill=255)
    for ang in (0, 90, 180, 270):
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        d.line([_pt(x, y, s, 0.5 + dx * 0.34, 0.5 + dy * 0.34),
                _pt(x, y, s, 0.5 + dx * 0.44, 0.5 + dy * 0.44)],
               fill=255, width=int(0.06 * s))


def g_scene(d, x, y, s):
    """场景：四角星（"一键切换氛围"）+ 右上小点。"""
    pts = [(0.50, 0.10), (0.60, 0.38), (0.88, 0.48), (0.60, 0.58),
           (0.50, 0.86), (0.40, 0.58), (0.12, 0.48), (0.40, 0.38)]
    d.polygon([_pt(x, y, s, a, b) for a, b in pts], fill=255)
    d.ellipse(_box(x, y, s, 0.76, 0.10, 0.90, 0.24), fill=255)


def g_button(d, x, y, s):
    """按钮：外环 + 中心点（"按一下就执行"）。"""
    d.ellipse(_box(x, y, s, 0.16, 0.16, 0.84, 0.84), outline=255,
              width=int(0.085 * s))
    d.ellipse(_box(x, y, s, 0.38, 0.38, 0.62, 0.62), fill=255)


def g_climate(d, x, y, s):
    """空调：雪花（3 条主线 + 每端两撇）。"""
    for ang in (0, 60, 120):
        a = math.radians(ang)
        dx, dy = math.cos(a), math.sin(a)
        d.line([_pt(x, y, s, 0.5 - dx * 0.32, 0.5 - dy * 0.32),
                _pt(x, y, s, 0.5 + dx * 0.32, 0.5 + dy * 0.32)],
               fill=255, width=int(0.07 * s))
        for side in (-1, 1):
            for t in (0.16, 0.24):      # 主线上两处各出一撇
                bx, by = 0.5 + dx * side * t, 0.5 + dy * side * t
                for da in (-40, 40):
                    b = math.radians(ang + da)
                    ex, ey = bx + math.cos(b) * side * 0.11, by + math.sin(b) * side * 0.11
                    d.line([_pt(x, y, s, bx, by), _pt(x, y, s, ex, ey)],
                           fill=255, width=int(0.055 * s))


def g_cover(d, x, y, s):
    """窗帘：顶杆 + 三幅垂帘。"""
    d.line([_pt(x, y, s, 0.10, 0.16), _pt(x, y, s, 0.90, 0.16)],
           fill=255, width=int(0.07 * s))
    for fx in (0.26, 0.50, 0.74):
        d.line([_pt(x, y, s, fx, 0.22), _pt(x, y, s, fx, 0.78)],
               fill=255, width=int(0.075 * s))
    d.line([_pt(x, y, s, 0.10, 0.80), _pt(x, y, s, 0.90, 0.80)],
           fill=255, width=int(0.07 * s))


def g_fan(d, x, y, s):
    """风扇：三片扇叶（pieslice 楔形）+ 轮毂。"""
    box = _box(x, y, s, 0.06, 0.06, 0.94, 0.94)
    for k in range(3):
        st = -90 + k * 120 + 12
        d.pieslice(box, st, st + 76, fill=255)
    # 轮毂：**在图形图层上挖空**（fill=0），不是填卡片色 ——
    # 图层是单通道 mask，挖空才有"透过孔看到底板"的效果（填 CARD_BG 会报类型错）。
    d.ellipse(_box(x, y, s, 0.40, 0.40, 0.60, 0.60), fill=0)


def g_lock(d, x, y, s):
    """锁：锁梁 + 锁体。"""
    d.arc(_box(x, y, s, 0.28, 0.10, 0.72, 0.58), start=180, end=360,
          fill=255, width=int(0.085 * s))
    d.rounded_rectangle(_box(x, y, s, 0.20, 0.46, 0.80, 0.88),
                        radius=0.08 * s, fill=255)
    d.ellipse(_box(x, y, s, 0.45, 0.60, 0.55, 0.70), fill=0)   # 锁孔（挖空）


def g_camera(d, x, y, s):
    """摄像头：机身 + 镜头 + 顶部小凸起。"""
    d.rounded_rectangle(_box(x, y, s, 0.08, 0.26, 0.92, 0.78),
                        radius=0.10 * s, outline=255, width=int(0.07 * s))
    d.ellipse(_box(x, y, s, 0.34, 0.34, 0.66, 0.66), outline=255,
              width=int(0.065 * s))
    d.rectangle(_box(x, y, s, 0.34, 0.16, 0.52, 0.26), fill=255)


def g_media(d, x, y, s):
    """音箱/播放器：喇叭 + 两道声波。"""
    d.polygon([_pt(x, y, s, 0.14, 0.38), _pt(x, y, s, 0.30, 0.38),
               _pt(x, y, s, 0.50, 0.18), _pt(x, y, s, 0.50, 0.82),
               _pt(x, y, s, 0.30, 0.62), _pt(x, y, s, 0.14, 0.62)], fill=255)
    for r in (0.60, 0.74):
        d.arc(_box(x, y, s, 0.5 - r + 0.14, 0.5 - r + 0.14, 0.5 + r + 0.14, 0.5 + r + 0.14),
              start=-52, end=52, fill=255, width=int(0.06 * s))


def g_other(d, x, y, s):
    """其它/未知域：方框 + 2x2 点（保持中性，不假装有具体语义）。"""
    d.rounded_rectangle(_box(x, y, s, 0.10, 0.10, 0.90, 0.90),
                        radius=0.14 * s, outline=255, width=int(0.07 * s))
    for fx in (0.34, 0.66):
        for fy in (0.34, 0.66):
            d.ellipse(_box(x, y, s, fx - 0.09, fy - 0.09, fx + 0.09, fy + 0.09), fill=255)


GLYPHS = {}


def _collect():
    for _, _, fn in DOMAINS:
        GLYPHS[fn] = globals()[fn]


# ==================================================================
#  合成
# ==================================================================

def hex2rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def make_tile(domain_color, glyph_name, state):
    """画一张 SIZE×SIZE 的**不透明**图（mode=RGB，没有 alpha 通道）。"""
    sz = SIZE * SS
    bg_hex, fg = STATE_STYLE[state]
    if state == "on":
        # ★ 开态：底板 = 域色压暗一档（理由见 ON_DARKEN），图形近白。
        bg = tuple(int(v * ON_DARKEN) for v in hex2rgb(domain_color))
        fg = (255, 255, 255)
    elif state == "off":
        bg = bg_hex
        fg = (110, 110, 116)
    else:
        bg = bg_hex
        fg = (85, 85, 90)

    img = Image.new("RGB", (sz, sz), CARD_BG)
    d = ImageDraw.Draw(img)
    # 圆角底板（半径 22% ≈ iOS 的圆角方图观感）
    pad = int(0.04 * sz)
    d.rounded_rectangle([pad, pad, sz - 1 - pad, sz - 1 - pad],
                        radius=0.22 * sz, fill=bg)

    # 图形画在独立图层上再贴回去 —— 这样"图形颜色"和"底板颜色"可以分开控制，
    # 也顺手避免了 fg=CARD_BG 的挖空画法（挖空会把底板抠漏，露出四角底色）。
    layer = Image.new("L", (sz, sz), 0)
    gd = ImageDraw.Draw(layer)
    GLYPHS[glyph_name](gd, OX * SS, OX * SS, ART * SS)
    img.paste(Image.new("RGB", (sz, sz), fg), (0, 0), layer)

    # ★ 必须 BOX：LANCZOS 对硬边振铃（见文件头 ③）
    out = img.resize((SIZE, SIZE), Image.BOX)
    return out


def mean_luma(img):
    px = list(img.getdata())
    n = len(px)
    return sum(0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px) / n


def ink_ratio(img, fg_like):
    """中心区域里"接近图形色"的像素占比 —— 用来证明**图形真的画上去了**。"""
    w, h = img.size
    x0, y0 = int(w * 0.20), int(h * 0.20)
    x1, y1 = int(w * 0.80), int(h * 0.80)
    px = img.load()
    hit = 0
    tot = 0
    for yy in range(y0, y1):
        for xx in range(x0, x1):
            r, g, b = px[xx, yy]
            tot += 1
            if (abs(r - fg_like[0]) < 42 and abs(g - fg_like[1]) < 42
                    and abs(b - fg_like[2]) < 42):
                hit += 1
    return hit / float(tot or 1)


def main():
    _collect()
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(SHEET), exist_ok=True)
    bad = 0
    total = 0
    rows = []

    for dom, color, glyph in DOMAINS:
        row = []
        for st in STATES:
            img = make_tile(color, glyph, st)

            # 自检 ①：**绝不能有 alpha 通道**（运行时换图不保留 alpha ⇒ 会变白块）
            if img.mode != "RGB":
                print("!! %s/%s mode=%s（必须 RGB）" % (dom, st, img.mode))
                bad += 1

            # 自检 ②：图形真的画上去了（中心区有图形色像素）
            fg = (255, 255, 255) if st == "on" else (
                (110, 110, 116) if st == "off" else (85, 85, 90))
            r = ink_ratio(img, fg)
            if r < 0.05:
                print("!! %s/%s 中心区图形色占比过低 %.3f（图形可能没画上）" % (dom, st, r))
                bad += 1

            # 自检 ③：四角必须是**卡片底色**（否则磁贴四角会露方块角）
            for xy in ((0, 0), (SIZE - 1, 0), (0, SIZE - 1), (SIZE - 1, SIZE - 1)):
                p = img.getpixel(xy)
                if max(abs(p[i] - CARD_BG[i]) for i in range(3)) > 6:
                    print("!! %s/%s 四角不是卡片底色 (%s) = %s" % (dom, st, xy, p))
                    bad += 1

            fname = "ha_tile_%s_%s.png" % (dom, st)
            p = os.path.join(OUT_DIR, fname)
            img.save(p, optimize=True)
            sz = os.path.getsize(p)
            total += sz
            row.append((fname, img, sz))
        rows.append((dom, row))
        print("gen %-14s on/off/flat  %d+%d+%d B"
              % (dom, row[0][2], row[1][2], row[2][2]))

    # 自检 ④：**状态真的被视觉编码了**（on 必须明显比 off 亮）
    #   这条最重要 —— 它是"图片能表达状态"这件事的机器判据，不靠肉眼。
    for dom, row in rows:
        _, on_img, _ = row[0]
        _, off_img, _ = row[1]
        _, flat_img, _ = row[2]
        lo, lf_, lfl = mean_luma(on_img), mean_luma(off_img), mean_luma(flat_img)
        if not (lo > lf_ + 25 and lf_ > lfl - 6):
            print("!! %s 三档亮度差不成立：on=%.1f off=%.1f flat=%.1f" % (dom, lo, lf_, lfl))
            bad += 1

    # 联络表（给人看的，不进包）
    cols, rows_n = 3, len(rows)
    gap, cell = 6, SIZE
    sheet = Image.new("RGB", (cols * cell + (cols + 1) * gap,
                              rows_n * cell + (rows_n + 1) * gap), (16, 16, 18))
    for ri, (dom, row) in enumerate(rows):
        for ci, (fname, img, _) in enumerate(row):
            sheet.paste(img, (gap + ci * (cell + gap), gap + ri * (cell + gap)))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(SHEET)

    print("共 %d 张，合计 %d 字节（%.1f KB），自检%s"
          % (len(DOMAINS) * len(STATES), total, total / 1024.0, "失败" if bad else "通过"))
    print("联络表（看一眼 39 张长什么样）: %s" % SHEET)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
