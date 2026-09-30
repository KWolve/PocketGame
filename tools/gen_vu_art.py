#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_vu_art.py - 网络收音机"双指针 VU 表"：**真机素材 + 样张**（一份几何两处用）

为什么同一个脚本既出素材又出样张：表盘的几何（圆心/半径/刻度角/红区）**必须逐像素一致** ——
样张是给用户看效果的、素材是上真机的，两套代码画出来对不上就会"看图挺好、上机变形"。
⇒ 一律用 `draw_vu()` 这一份实现。

★★ 2026-09-19 "压扁"改版（用户报「表盘盖住了其他控件：控制按键、电台时间都被盖住」）
   原版是 **226x226 的方盘 @y=408..634**，把"已播时长"(424..454) 与"总电平条"(454..462)
   整条压在下面，下沿还伸进控制键(620..724)那一排。布局预算（`ui/radio.html`）：

       已播时长 424..454 ┊ 总电平条 454..462 ┊ **仪表区 464..620** ┊ 控制键 620..724

   ⇒ 面盘压成 **226 x 128 @y=468**（468..596），读数窗挪到面盘**下方**(597..618)。

   ⚠️⚠️ **压扁不能把圆压成椭圆**：指针是"整张图绕铰点转"，刻度弧必须是**正圆**，
   否则弧上各点到铰点的距离随角度变，针尖会离开刻度（这是"旋转式"仪表的硬约束）。
   真表"扁"的做法是**大半径浅弧** —— 把铰点挪到面盘**下边缘之外**(`PIVOT_Y=142 > FACE_H=128`)，
   半径加大到 120 ⇒ 盘内只露出弧的顶部一段，观感是"扁宽"，而几何仍然严格。
   两条不能破的等式：**圆心 == 铰点**、**针长 == 弧半径(+3)**。

   自检：`python tools/gen_vu_art.py --check`（不产图，只验几何不越界/不压别的控件）。

产出的**素材**（`resources/images/`，直接给 `ui/radio.html` 的 `div.pointer` / `class="icon"` 用）：
  vu_face_l_226x128.png / vu_face_r_226x128.png  表盘底（浅黄面盘 + 刻度弧 + 红区 + VU 字样 + L/R 圆标）
  vu_needle_234.png                              指针（**254x254、铰点 = 图片正中 (127,127)**、针尖朝上）
  vu_tab_meter_on/off_84x36.png                  视图切换页签"表盘"（亮/暗两态）
  vu_tab_spec_on/off_84x36.png                   视图切换页签"频谱"（亮/暗两态）

产出的**样张**（`docs/`，坐标**按真机控件盒**画，可直接当"布局证据"）：
  mock_vu_page.png      整页样张（480x800 实机尺寸）
  mock_vu_spec.png      同一页的频谱视图
  mock_vu_variants.png  三种面盘风格并排（米黄经典 / 深棕暖光 / 黑面浅黄弧）
  mock_vu_needles.png   同一只表在 -20/-10/-3/0/+3 dB 下的指针位置

真机实现口径（**来自 MCP 知识库 `uicontrols/pointer-fields.md`，不是猜的**）：
  · `rotationPoint` = **控件系**旋转圆心；`fixedPoint` = **指针图系**铰点；两者配套否则绕错圆心；
  · 唯一 API `setTargetAngle(度)`（目标角，不是增量）；`animatable=false` 时由调用方定时器驱动；
  · `startAngle` 支持负值（表盘 0 位校准），`clockwise=1` 时正角 = 顺时针。
  ⇒ 本工程用法：指针图 254x254、铰点在正中 ⇒ `fixedPoint=(127,127)`、
     `rotationPoint=(127,127)`、控件放在"铰点 − 127"的位置；角度直接用"相对垂直"的度数
     （左边负、右边正），与 `db_to_ang()` 的输出一致。
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DOCS = os.path.join(ROOT, "docs")
IMGS = os.path.join(ROOT, "resources", "images")

PAGE = (0, 0, 0)
NAVI = (28, 28, 30)
INK = (242, 242, 247)
DIM = (154, 154, 160)
CYAN = (100, 210, 255)

# ============================ 表盘几何（唯一真值） ============================
# ⚠️ 改这里必须同步 `ui/radio.html` 的控件盒；`--check` 会验证一致性。
FACE_W, FACE_H = 226, 128        # 面盘控件盒 == 素材尺寸
FACE_YS = (468, 468)             # L / R 面盘的 y（468..596）
FACE_XS = (8, 246)               # L / R 面盘的 x
PIVOT = (117, 142)               # 铰点（面盘局部坐标）—— **PIVOT[1] > FACE_H ⇒ 在盘外**
ARC_R = 110                      # 刻度弧半径（圆心 == 铰点 ⇒ 针尖恒在弧上）
NEEDLE_LEN = ARC_R + 3           # 针长（略过弧线一点，像真表）
NEEDLE = 2 * (NEEDLE_LEN + 4)    # 指针图边长（铰点 = 图片正中）；234
DB_TXT_Y, DB_TXT_H = 597, 21     # 读数窗（面盘下方 597..618，控制键顶 620）
DB_TXT_W = 90
BTN_TOP = 620                    # 控制键那一排的顶（布局硬边界）
BAR_BOT = 462                    # 总电平条的底（布局硬边界）
CORNER = 14                      # ★ 面盘**圆倒角半径**：既画边框也裁 alpha（两处必须同一个值）
LAST_ANGLE = -9999               # 与 radioLogic.cc 一致

# ★★ 为什么半径只能是 110（2026-09-19 实测，`check_bounds` 抓的）：
#   指针是"254 见方的方图绕正中转" ⇒ **指针盒宽 = 2×(弧半径+7)**，比面盘(226)还宽。
#   两只表并排时，两个盒的总跨距 = 2×NEEDLE + 面盘间距。要让"左盒不出左沿、
#   右盒不出右沿"（`data-x` 负数会被框架钳成 0 ⇒ 指针错位！），必须：
#       PIVOT[0] ≥ NEEDLE/2 − FACE_XS[0]   且   PIVOT[0] ≤ 480 − FACE_XS[1] − NEEDLE/2
#   代入 FACE_XS=(8,246)： NEEDLE ≤ 242。取 NEEDLE=234 ⇒ PIVOT[0] ∈ [109,117]。
#   半径再大一点（如 120/指针图 254）右盒就会到 x=500 > 480，被 `check_bounds` 判越界。
#   ⇒ 这四条不等式就是"半径上限"的来历，改任何一条都要重算，不要凭手感调。


def needle_box(i):
    """第 i 只表（0=L,1=R）的**指针控件盒**左上角。

    ⚠️ `data-x` **不能是负数**（框架会把负值钳成 0，指针整体错位）——`check()` 里有断言。
    """
    return (FACE_XS[i] + PIVOT[0] - NEEDLE // 2, FACE_YS[i] + PIVOT[1] - NEEDLE // 2)


def db_txt_box(i):
    """第 i 只表的 dB 读数窗控件盒（水平居中于该表）。"""
    cx = FACE_XS[i] + FACE_W // 2
    return (cx - DB_TXT_W // 2, DB_TXT_Y, DB_TXT_W, DB_TXT_H)


def font(sz, bold=False):
    for p in (r"C:\Windows\Fonts\simhei.ttf", r"C:\Windows\Fonts\msyh.ttc",
              r"C:\Windows\Fonts\arial.ttf"):
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:                                   # noqa: BLE001
                continue
    return ImageFont.load_default()


def mono(sz):
    for p in (r"C:\Windows\Fonts\consola.ttf", r"C:\Windows\Fonts\cour.ttf"):
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:                                   # noqa: BLE001
                continue
    return font(sz)


F_TITLE = font(20)
F_NAME = font(30)
F_SMALL = font(14)
F_TINY = font(11)
F_BIG = font(22)
F_LBL = font(15, True)
F_DB = mono(17)
F_DB_S = mono(12)


# ---------------- 几何：dB → 指针角度（度，0 = 垂直向上） ----------------
# ⚠️ `radioLogic.cc::vuDbToAngle()` 必须与本函数**逐字一致**（改一个就要改另一个）。
DB_MIN, DB_MAX_HOT = -20.0, 3.0
ANG_MIN, ANG_ZERO, ANG_HOT = -52.0, 16.0, 34.0


def db_to_ang(db):
    db = max(DB_MIN, min(DB_MAX_HOT, db))
    if db <= 0.0:
        return ANG_MIN + (db - DB_MIN) * (ANG_ZERO - ANG_MIN) / (0.0 - DB_MIN)
    return ANG_ZERO + (db - 0.0) * (ANG_HOT - ANG_ZERO) / (DB_MAX_HOT - 0.0)


def polar(cx, cy, ang, r):
    a = math.radians(ang)
    return (cx + r * math.sin(a), cy - r * math.cos(a))


SCALE_TICKS = [(-20, True), (-10, True), (-7, False), (-5, True), (-3, True),
               (-2, False), (-1, True), (0, True), (1, False), (2, False), (3, True)]
SCALE_LABELS = [-20, -10, -5, -3, -1, 0, 3]


def face_palette(style):
    """三种风格：返回 (面盘上色, 面盘下色, 刻度色, 指针色, 文字色, 红区, 光晕)。"""
    if style == "cream":          # 米黄经典（默认推荐）
        return ((255, 250, 226), (238, 220, 168), (60, 48, 30), (30, 26, 20),
                (70, 56, 34), (196, 48, 40), (255, 202, 90))
    if style == "brown":          # 深棕面 + 暖黄光（老台式机）
        return ((92, 66, 34), (58, 40, 22), (255, 226, 150), (255, 214, 120),
                (252, 232, 176), (226, 96, 62), (255, 190, 80))
    # "dark"：黑面 + 浅黄刻度（最省亮度、夜里不刺眼）
    return ((30, 28, 26), (18, 17, 16), (250, 226, 150), (255, 236, 170),
            (252, 236, 190), (232, 92, 64), (255, 206, 96))


def round_mask(w, h, radius, ss=4):
    """**圆倒角 alpha 遮罩**（4x 超采样再缩回，边缘带抗锯齿）。

    ★ 为什么必须"真的裁"（2026-09-19 用户反馈：**四角是方块，不好看**）：
      原先面盘是一张**不透明矩形**，圆角只是**画上去的一圈边框** ——
      边框之外的四个角仍然是面盘底色 ⇒ 在黑色页面上就是四块"方角料"。
      ⇒ 面盘必须以 **RGBA** 出图、角上 alpha=0，让页面底色透出来。
      ⚠️ 用 `putalpha()` 而不是 `paste` 一张黑图：后者会把角**涂黑**，
        在非黑背景的页面上照样看得出方块。
      ⚠️ 半径必须与画边框时的 `rounded_rectangle(radius=...)` **同一个值**，
        否则会出现"边框弧在透明区内"或"透明区切进面盘"的双弧。
    ⚠️ 缩回必须用 **`Image.BOX`（面积平均）而不是 LANCZOS**：后者会在硬边两侧
       产生**振铃**（形状外飘出 alpha=3 这类淡像素），被 `tools/check_assets.py`
       的"幽灵像素"判据判 FAIL（本次实测 32 处）。BOX 只做平均，没有过冲。
    """
    m = Image.new("L", (w * ss, h * ss), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w * ss - 1, h * ss - 1],
                                        radius=radius * ss, fill=255)
    return m.resize((w, h), Image.BOX)


def face_layer(w, h, style, label, txt_db=None):
    """表盘底（不含指针）：浅黄面盘 + 刻度弧 + 红区 + 刻度数字 + VU 字样 + L/R 圆标。

    ★ 压扁版：**所有刻画都由 `PIVOT / ARC_R` 决定**（圆心 == 铰点、弧是正圆），
      面盘尺寸只决定"裁出多大一块" ⇒ 换尺寸不用重算刻度。
    """
    top, bot, sc, nd, tc, red, glow = face_palette(style)
    # ---- 外光晕（样张用；真机不用，见文件尾注释）----
    halo = Image.new("RGBA", (w + 24, h + 24), (0, 0, 0, 0))
    hd = ImageDraw.Draw(halo)
    for i, a in ((0, 50), (3, 66), (6, 82)):
        hd.rounded_rectangle([6 - i, 6 - i, w + 18 + i, h + 18 + i],
                             radius=14 + i, outline=glow + (a,), width=3)
    halo = Image.alpha_composite(Image.new("RGBA", halo.size, PAGE + (255,)), halo).convert("RGB")
    # ---- 面盘：上亮下暗的浅黄渐变 ----
    face = Image.new("RGB", (w, h), top)
    px = face.load()
    for yy in range(h):
        t = (yy / (h - 1.0)) ** 1.15
        col = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        for xx in range(w):
            px[xx, yy] = col
    hi = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(hi).ellipse([w * 0.14, h * 0.02, w * 0.86, h * 0.94], fill=glow + (34,))
    face = Image.alpha_composite(face.convert("RGBA"), hi).convert("RGB")
    # ---- 表内所有刻画都画在一个 4x 超采样层上，最后缩回（抗锯齿） ----
    SS = 4
    lay = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    cx, cy = PIVOT[0] * SS, PIVOT[1] * SS
    R = ARC_R * SS
    d.arc([cx - R, cy - R, cx + R, cy + R], ANG_MIN - 90, ANG_HOT - 90,
          fill=sc + (255,), width=int(1.8 * SS))
    # 红区（0VU → +3dB）：真表这里是**一圈更粗的红弧**，比红刻度线好认
    d.arc([cx - R, cy - R, cx + R, cy + R], ANG_ZERO - 90, ANG_HOT - 90,
          fill=red + (255,), width=int(6.0 * SS))
    for db, major in SCALE_TICKS:
        a = db_to_ang(db)
        d.line([polar(cx, cy, a, R - 1 * SS),
                polar(cx, cy, a, R - ((13 if major else 7) * SS))],
               fill=sc + (255,), width=int((1.8 if major else 1.0) * SS))
    fnum = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 11 * SS)
    for db in SCALE_LABELS:
        p = polar(cx, cy, db_to_ang(db), R - 26 * SS)
        t = str(db)
        wt = d.textlength(t, font=fnum)
        d.text((p[0] - wt / 2, p[1] - 7 * SS), t, font=fnum,
               fill=(red if db >= 0 else sc) + (255,))
    fvu = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", 13 * SS)
    wt = d.textlength("VU", font=fvu)
    d.text((cx - wt / 2, cy - 46 * SS), "VU", font=fvu, fill=sc + (150,))
    # L/R 圆标：放在**扫描扇区之外**的左下角（铰点在盘外 ⇒ 扇区覆盖盘内下半部，
    #   左下角是唯一安稳的位置；放中间会被针扫过去）。
    lx, ly, lr = 28 * SS, 100 * SS, 14 * SS
    d.ellipse([lx - lr, ly - lr, lx + lr, ly + lr], fill=sc + (255,))
    fl = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", int(lr * 1.15))
    wt = d.textlength(label, font=fl)
    d.text((lx - wt / 2, ly - lr * 0.72), label, font=fl, fill=tc + (255,))
    if txt_db:
        fdb = ImageFont.truetype(r"C:\Windows\Fonts\consola.ttf", 17 * SS)
        wt = d.textlength(txt_db, font=fdb)
        d.text((cx + 30 * SS - wt / 2, 96 * SS), txt_db, font=fdb,
               fill=(255, 214, 120, 255))
    face = Image.alpha_composite(face.convert("RGBA"),
                                lay.resize((w, h), Image.BOX)).convert("RGB")
    fr = ImageDraw.Draw(face)
    fr.rounded_rectangle([0, 0, w - 1, h - 1], radius=CORNER, outline=(58, 56, 52), width=3)
    fr.rounded_rectangle([3, 3, w - 4, h - 4], radius=CORNER - 2,
                         outline=(16, 15, 14), width=2)
    # ★ 背光内圈：真机**不做外光晕控件**（外扩会被屏幕边缘裁掉、`data-x` 负值还会被钳成 0），
    #   改成把暖光烘在**边框内侧** —— 观感一样是"面盘在发光"，且严格 1:1 不出界。
    glow_ring = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow_ring)
    for i, a in ((6, 70), (10, 40), (14, 22)):
        gd.rounded_rectangle([i, i, w - 1 - i, h - 1 - i], radius=max(2, CORNER - i + 4),
                             outline=glow + (a,), width=2)
    face = Image.alpha_composite(face.convert("RGBA"), glow_ring)
    # ★★ 最后一步：把**圆角之外**的四个角裁成透明（见 round_mask 的注释）
    mask = round_mask(w, h, CORNER)
    # ⚠️ 还必须把**全透明像素的 RGB 也清成 0**：`check_assets.py` 的
    #   「透明区污染（α=0 却带 RGB —— 渲染忽略 alpha 时会显形）」就是查这个。
    #   半透明（0<α<255）的边缘像素**保留**面盘色，否则合成时会出一圈黑边。
    solid = mask.point(lambda v: 0 if v == 0 else 255)
    face.paste((0, 0, 0), mask=Image.eval(solid, lambda v: 255 - v))
    face.putalpha(mask)
    return face, halo


def needle_layer(size, style, angle=0.0, needle_len=None):
    """指针（**独立一层**、透明背景、铰点在图片正中）。
    ⚠️ `angle` 是**相对垂直的旋转角**（0 = 针尖朝上 = 图片原样）—— 真机 `ZKPointer`
       旋转的就是"图片原样"那个基准，所以**素材必须用 0° 画**；
       样张要的是"某个 dB 下的姿态"，调用方自己 `db_to_ang(db)` 传进来。
       （**血案**：一开始这里收的是 dB，`needle_layer(..., db=0.0)` 按 0VU=+16° 画，
         于是"0° 素材"其实歪了 16° —— 指针图必须与旋转基准对齐。）"""
    top, bot, sc, nd, tc, red, glow = face_palette(style)
    c = size / 2.0
    L = needle_len if needle_len else int(size * 0.44)
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    SS = 4
    lay = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    C = c * SS
    a = math.radians(angle)
    ux, uy = math.sin(a), -math.cos(a)      # 针的方向
    pxv, pyv = -uy, ux                      # 垂直方向
    tip = (C + ux * L * SS, C + uy * L * SS)
    # 针尖也要留 ~1.6px 宽（**不收成一个点**）：点状针尖的 alpha 必然很淡，
    # `check_assets.py` 的"幽灵像素"判据会把整根针报成悬空淡像素（细线/尖端误报）。
    tipw = 0.8 * SS
    tail = (C - ux * 11 * SS, C - uy * 11 * SS)
    # ⚠️ 针体要有**实心核**（宽度 ≥2px 才有 α=255 的连续像素）——太细的话
    #    `tools/check_assets.py` 的"幽灵像素"判据会把整根针当成悬空淡像素（细线误报）。
    w_hub = 2.6 * SS
    d.polygon([(C + pxv * w_hub, C + pyv * w_hub),
               (tip[0] + pxv * tipw, tip[1] + pyv * tipw),
               (tip[0] - pxv * tipw, tip[1] - pyv * tipw),
               (C - pxv * w_hub, C - pyv * w_hub)], fill=nd + (255,))
    d.polygon([(C + pxv * w_hub * 0.8, C + pyv * w_hub * 0.8),
               (tail[0] + pxv * tipw * 0.7, tail[1] + pyv * tipw * 0.7),
               (tail[0] - pxv * tipw * 0.7, tail[1] - pyv * tipw * 0.7),
               (C - pxv * w_hub * 0.8, C - pyv * w_hub * 0.8)], fill=nd + (255,))
    d.ellipse([C - 3.4 * SS, C - 3.4 * SS, C + 3.4 * SS, C + 3.4 * SS], fill=nd + (255,))
    d.ellipse([C - 1.4 * SS, C - 1.4 * SS, C + 1.4 * SS, C + 1.4 * SS], fill=glow + (255,))
    return Image.alpha_composite(img, lay.resize((size, size), Image.BOX))


def tab_layer(w, h, text, on):
    """视图切换页签（文字烘进图里，省一个 text 控件、也不会被上层控件挡住点击）。"""
    img = Image.new("RGB", (w, h), PAGE)
    SS = 4
    lay = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(lay)
    d.rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=h * SS // 2,
                        fill=((255, 122, 69) if on else (46, 46, 48)) + (255,))
    f = ImageFont.truetype(r"C:\Windows\Fonts\simhei.ttf", int(h * 0.44 * SS))
    tw = d.textlength(text, font=f)
    d.text(((w * SS - tw) / 2, h * SS * 0.24), text, font=f,
           fill=((16, 12, 8, 255) if on else (206, 206, 210, 255)))
    img.paste(Image.alpha_composite(img.convert("RGBA"),
                                    lay.resize((w, h), Image.BOX)).convert("RGB"), (0, 0))
    return img


def draw_vu(dst, x, y, style, label, needle_db, txt_db=None):
    """样张用：表盘 + 指针按**真机口径**叠起来（指针盒放"铰点 − 图边长/2"）。

    ⚠️ 面盘现在是 **RGBA（圆角外透明）** ⇒ 必须带 mask 贴（`paste(im, box, im)`）；
       直接 `paste(im, box)` 会把 alpha 丢掉、把四个角当成面盘色贴上去。
    """
    face, halo = face_layer(FACE_W, FACE_H, style, label, txt_db)
    dst.paste(halo, (x - 12, y - 12))
    dst.paste(face, (x, y), face)
    n = needle_layer(NEEDLE, style, db_to_ang(needle_db), needle_len=NEEDLE_LEN)
    nx = x + PIVOT[0] - NEEDLE // 2
    ny = y + PIVOT[1] - NEEDLE // 2
    box = dst.crop((nx, ny, nx + NEEDLE, ny + NEEDLE)).convert("RGBA")
    dst.paste(Image.alpha_composite(box, n).convert("RGB"), (nx, ny))
    if txt_db:
        F = mono(14)
        dd = ImageDraw.Draw(dst)
        cx = x + FACE_W // 2
        tw = dd.textlength(txt_db, font=F)
        dd.rectangle([cx - DB_TXT_W // 2, DB_TXT_Y, cx + DB_TXT_W // 2, DB_TXT_Y + DB_TXT_H],
                     fill=(14, 13, 12))
        dd.text((cx - tw / 2, DB_TXT_Y + 3), txt_db, font=F, fill=(255, 214, 120))


def page(style="cream", l_db=-3.0, r_db=-9.5, meters=True):
    """整页样张（480x800）：**坐标全部照抄 ui/radio.html 的控件盒** ⇒ 可直接当布局证据。"""
    im = Image.new("RGB", (480, 800), PAGE)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 480, 52], fill=NAVI)
    d.text((240 - d.textlength("网络收音机", font=F_TITLE) / 2, 14), "网络收音机",
           font=F_TITLE, fill=INK)
    d.line([(38, 62), (26, 74), (38, 86)], fill=INK, width=3)
    d.text((64, 58), "播放中", font=F_SMALL, fill=DIM)
    # 视图切换页签（表盘 | 频谱），当前的那枚是亮橙
    for i, (t, on) in enumerate((("表盘", meters), ("频谱", not meters))):
        im.paste(tab_layer(84, 36, t, on), (300 + i * 88, 58))
    # 大圆盘（与工程一致：黑胶片 + 暖橙标签 + 音符）—— 控件盒 (120,100,240x240)
    d.ellipse([120, 100, 360, 340], fill=(18, 18, 20), outline=(58, 36, 24), width=3)
    for rr in range(110, 60, -8):
        d.ellipse([240 - rr, 220 - rr, 240 + rr, 220 + rr], outline=(30, 30, 34), width=1)
    d.ellipse([200, 180, 280, 260], fill=(255, 122, 69))
    d.ellipse([232, 212, 252, 232], fill=(255, 255, 255))
    d.rectangle([248, 196, 252, 224], fill=(255, 255, 255))
    # 台名 348 / 信息 400 / 已播时长 424 / 总电平条 454（照抄控件盒）
    d.text((240 - d.textlength("怀集音乐之声", font=F_NAME) / 2, 348), "怀集音乐之声",
           font=F_NAME, fill=INK)
    s = "音乐 · lhttp.qtfm.cn · 64k MP3"
    d.text((240 - d.textlength(s, font=F_SMALL) / 2, 400), s, font=F_SMALL, fill=DIM)
    d.text((240 - d.textlength("02:14", font=F_BIG) / 2, 424), "02:14", font=F_BIG, fill=CYAN)
    d.rectangle([80, 454, 400, 462], fill=(44, 44, 46))
    d.rectangle([80, 454, 310, 462], fill=(255, 206, 74))
    if meters:
        draw_vu(im, FACE_XS[0], FACE_YS[0], style, "L", l_db, "%+.1f" % l_db)
        draw_vu(im, FACE_XS[1], FACE_YS[1], style, "R", r_db, "%+.1f" % r_db)
    else:
        for i in range(30):
            x = 2 + i * 16
            hh = 8 + int(120 * abs(math.sin(i * 0.55)) * 0.9) + 6
            d.rectangle([x, 596 - hh, x + 11, 596], fill=(255, 122, 69))
            d.rectangle([x, 596 - hh, x + 11, 596 - hh + 3], fill=(255, 206, 74))
        d.rectangle([12, 596, 468, 608], fill=(28, 28, 30))
    for cx, kind in ((100, "prev"), (240, "stop"), (380, "next")):
        d.ellipse([cx - 52, BTN_TOP, cx + 52, BTN_TOP + 104], fill=(22, 22, 24),
                  outline=(255, 122, 69) if kind == "stop" else (54, 54, 58), width=2)
        cy = BTN_TOP + 52
        if kind == "prev":
            d.rectangle([cx - 22, cy - 12, cx - 16, cy + 12], fill=INK)
            d.polygon([(cx + 14, cy - 12), (cx + 14, cy + 12), (cx - 8, cy)], fill=INK)
        elif kind == "next":
            d.rectangle([cx + 8, cy - 12, cx + 14, cy + 12], fill=INK)
            d.polygon([(cx - 22, cy - 12), (cx - 22, cy + 12), (cx, cy)], fill=INK)
        else:
            d.rounded_rectangle([cx - 12, cy - 12, cx + 12, cy + 12], radius=3,
                                fill=(255, 122, 69))
    d.text((240 - d.textlength("音量键调音量 · 长按 B 返回列表", font=F_TINY) / 2, 732),
           "音量键调音量 · 长按 B 返回列表", font=F_TINY, fill=(99, 99, 102))
    d.rectangle([0, 758, 480, 800], fill=(28, 28, 30))
    kb = "第 31 / 61 · 音乐 · 音量键调音量 · 长按 B 返回"
    d.text((240 - d.textlength(kb, font=F_SMALL) / 2, 766), kb, font=F_SMALL, fill=DIM)
    return im


def save(img, path):
    img.save(path)
    print("  %-32s %-12s %7d B" % (os.path.basename(path), "%dx%d" % img.size,
                                   os.path.getsize(path)))


def check():
    """几何自检：不产图，只验"不越界 / 不压别的控件 / 两条等式不破"。

    为什么要有它（2026-09-19 血案）：上一版把面盘放在 y=408..634，**盖住了"已播时长"
    与"总电平条"**，而当时的检查只看了"表盘自己画得对不对"，没有把新控件与页面上
    **原有控件的矩形**做相交 —— 用户先在真机上发现的。⇒ 把布局预算写成断言。
    """
    bad = []
    for i in (0, 1):
        fx, fy = FACE_XS[i], FACE_YS[i]
        face = (fx, fy, fx + FACE_W, fy + FACE_H)
        if face[1] < BAR_BOT + 2:
            bad.append("表%d 面盘顶 %d 压到总电平条(底 %d)" % (i, face[1], BAR_BOT))
        if face[3] > BTN_TOP - 2:
            bad.append("表%d 面盘底 %d 压到控制键(顶 %d)" % (i, face[3], BTN_TOP))
        # 读数窗：必须在面盘下方、按钮上方
        tx, ty, tw, th = db_txt_box(i)
        if ty < face[3]:
            bad.append("表%d 读数窗顶 %d 与面盘重叠" % (i, ty))
        if ty + th > BTN_TOP - 1:
            bad.append("表%d 读数窗底 %d 压到控制键" % (i, ty + th))
        if tx < face[0] or tx + tw > face[2]:
            bad.append("表%d 读数窗没在表盘正下方" % i)
        # 指针控件盒：**必须整只在屏内**。x 不能为负（框架会把负值钳成 0 ⇒ 指针错位），
        #   x+w 也不能超过屏宽（否则 `check_bounds.py` 判越界，且会被画布裁掉）。
        bx, by = needle_box(i)
        if bx < 0:
            bad.append("表%d 指针盒 x=%d < 0（会被钳成 0 导致错位）" % (i, bx))
        if bx + NEEDLE > 480:
            bad.append("表%d 指针盒右边 %d > 480（`check_bounds.py` 会判越界）"
                       % (i, bx + NEEDLE))
        if by < 0 or by + NEEDLE > 800:
            bad.append("表%d 指针盒纵向 %d..%d 出屏" % (i, by, by + NEEDLE))
        # ★ 两条不能破的等式
        if NEEDLE_LEN != ARC_R + 3:
            bad.append("针长(%d) != 弧半径(%d)+3 ⇒ 针尖会离开刻度弧" % (NEEDLE_LEN, ARC_R))
        if NEEDLE < 2 * NEEDLE_LEN:
            bad.append("指针图(%d) 装不下针长(%d)" % (NEEDLE, NEEDLE_LEN))
        if NEEDLE % 2:
            bad.append("指针图边长必须是偶数（铰点在正中要整除）")
        # 铰点允许在面盘**外**（这正是"浅弧"的做法），但弧顶必须在盘内
        if PIVOT[1] - ARC_R < 4:
            bad.append("弧顶 y=%d 贴到面盘上边缘（要留 ≥4px）" % (PIVOT[1] - ARC_R))
        # 弧的左右端点必须在盘内
        for a in (ANG_MIN, ANG_HOT):
            for r in (ARC_R, ARC_R - 26):
                p = polar(PIVOT[0], PIVOT[1], a, r)
                if not (2 <= p[0] <= FACE_W - 2 and 2 <= p[1] <= FACE_H - 2):
                    bad.append("表%d 角度%.0f° 半径%d 的点 (%.0f,%.0f) 出盘"
                               % (i, a, r, p[0], p[1]))
        # 指针的**实画范围**（含针尾 11px）不许伸进控制键那一排
        tail_y = PIVOT[1] + 11 * math.cos(math.radians(abs(ANG_MIN)))
        if fy + tail_y > BTN_TOP:
            bad.append("表%d 针尾 y=%.0f 伸进控制键区（顶 %d）" % (i, fy + tail_y, BTN_TOP))
    # ★ 圆倒角必须**真的透明**（用户 2026-09-19 反馈"四角是方块，不好看"）：
    #   只画一圈圆角边框是不够的，四个角还得 alpha=0。
    tf, _th = face_layer(64, 36, "cream", "L")
    ta = tf.getchannel("A").load()
    for px_, py_, nm in ((0, 0, "左上"), (63, 0, "右上"), (0, 35, "左下"), (63, 35, "右下")):
        if ta[px_, py_] > 8:
            bad.append("面盘%s角没透明（alpha=%d）⇒ 会露出方角料" % (nm, ta[px_, py_]))
    if ta[32, 18] < 250:
        bad.append("面盘中心不该透明（alpha=%d）" % ta[32, 18])
    # 与 ui/radio.html 对账
    html = os.path.join(ROOT, "ui", "radio.html")
    if os.path.isfile(html):
        import re
        src = open(html, encoding="utf-8").read()
        for cap, want in (("VuFaceL", "226x128"), ("VuFaceR", "226x128")):
            m = re.search(r'data-caption="%s"[^>]*data-w="(\d+)" data-h="(\d+)"' % cap, src)
            if m and "%sx%s" % (m.group(1), m.group(2)) != want:
                bad.append("radio.html 的 %s 是 %sx%s，与本脚本的 %sx%s 不一致"
                           % (cap, m.group(1), m.group(2), FACE_W, FACE_H))
        m = re.search(r'data-caption="VuNeedleL"[^>]*data-x="(-?\d+)" data-y="(-?\d+)"', src)
        if m and (int(m.group(1)), int(m.group(2))) != needle_box(0):
            bad.append("radio.html 的 VuNeedleL 是 (%s,%s)，应为 %s"
                       % (m.group(1), m.group(2), needle_box(0)))
    print("== 几何自检 ==")
    print("  面盘 %dx%d @L%s / R%s   铰点%s  弧半径 %d  针长 %d  指针图 %d"
          % (FACE_W, FACE_H, (FACE_XS[0], FACE_YS[0]), (FACE_XS[1], FACE_YS[1]),
             PIVOT, ARC_R, NEEDLE_LEN, NEEDLE))
    print("  指针盒 L%s R%s   读数窗 L%s R%s"
          % (needle_box(0), needle_box(1), db_txt_box(0)[:2], db_txt_box(1)[:2]))
    print("  布局预算：总电平条底 %d < 面盘 %d..%d < 读数 %d..%d < 控制键顶 %d"
          % (BAR_BOT, FACE_YS[0], FACE_YS[0] + FACE_H, DB_TXT_Y, DB_TXT_Y + DB_TXT_H,
             BTN_TOP))
    if bad:
        print("  ✗ %d 项不合格：" % len(bad))
        for b in bad:
            print("     - %s" % b)
        return 1
    print("  ✓ 全部通过")
    return 0


def main():
    if "--check" in sys.argv:
        return check()
    os.makedirs(IMGS, exist_ok=True)
    os.makedirs(DOCS, exist_ok=True)
    rc = check()
    if rc:
        print("!! 几何自检没过，先修几何再产图")
        return rc
    print("① 真机素材 -> %s" % IMGS)
    save(face_layer(FACE_W, FACE_H, "cream", "L")[0],
         os.path.join(IMGS, "vu_face_l_226x128.png"))
    save(face_layer(FACE_W, FACE_H, "cream", "R")[0],
         os.path.join(IMGS, "vu_face_r_226x128.png"))
    # 指针素材：254x254、0° 针尖朝上、铰点 = 图片正中；针长要**够到刻度弧**
    save(needle_layer(NEEDLE, "cream", 0.0, needle_len=NEEDLE_LEN),
         os.path.join(IMGS, "vu_needle_234.png"))
    for key, txt in (("meter", "表盘"), ("spec", "频谱")):
        for on in (True, False):
            save(tab_layer(84, 36, txt, on),
                 os.path.join(IMGS, "vu_tab_%s_%s_84x36.png" % (key, "on" if on else "off")))
    print("② 样张 -> %s" % DOCS)
    save(page(), os.path.join(DOCS, "mock_vu_page.png"))
    save(page(meters=False), os.path.join(DOCS, "mock_vu_spec.png"))
    gap, lbl = 10, 20
    out = Image.new("RGB", (FACE_W * 3 + gap * 4, FACE_H + lbl + 30), PAGE)
    dd = ImageDraw.Draw(out)
    for i, (st, name) in enumerate([("cream", "A cream 米黄经典"),
                                    ("brown", "B brown 深棕暖光"),
                                    ("dark", "C dark 黑面浅黄弧")]):
        x = gap + i * (FACE_W + gap)
        draw_vu(out, x, 24, st, "L", -3.0, "-3.0")
        dd.text((x + 4, 6), name, font=F_TINY, fill=DIM)
    save(out, os.path.join(DOCS, "mock_vu_variants.png"))
    out = Image.new("RGB", (FACE_W * 5 + 10 * 6, FACE_H + 30 + 26), PAGE)
    dd = ImageDraw.Draw(out)
    for i, v in enumerate((-20.0, -10.0, -3.0, 0.0, 3.0)):
        x = 10 + i * (FACE_W + 10)
        draw_vu(out, x, 20, "cream", "L", v, "%+.1f" % v)
        dd.text((x + 4, 2), "%+.0f dB" % v, font=F_TINY, fill=DIM)
    save(out, os.path.join(DOCS, "mock_vu_needles.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
