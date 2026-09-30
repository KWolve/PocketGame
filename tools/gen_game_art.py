#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_game_art.py —— 游戏 PNG 素材 + 轻量清单头

为什么改这套（用户 2026-09-15）：
  原来游戏画面全靠几何原语**程序化绘制**（打地鼠的草地就是 60 行 fillRect/fillEllipse），
  开发效率低（每加一个元素都要写绘图代码 + 调色 + 对位置），效果上限也低
  （木纹/毛绒/手绘笔触画不出来）。现在：**静态元素 = PNG 素材，改图只换文件**，
  只有动态部分（位移 / 弹出 / 淡出 / 命中闪光）才保留代码绘制。

产物：
  ① resources/images/game/<游戏>/*.png   —— 运行时按路径加载的素材（1:1 贴，带透明通道）
  ② src/core/PgGameArt.h                 —— **轻量清单**（路径 + 尺寸 + 锚点，无像素数据，
                                             只有几 KB；像素全在 PNG 里）
  ③ docs/shot_game_art_<游戏>.png        —— 拼图预览（给人看，也当"素材有没有画出来"的判据）

★ 三条纪律（生成器里一律自检，违反就报错退出）：
  1. **1:1**：素材尺寸 == 屏幕上贴的尺寸（Canvas::Sprite 不缩放不插值）。
  2. **锚点**：每张图声明自己在图内的参考点，贴图时 `x - ax, y - ay`
     —— 换帧/换尺寸都不会错位（打地鼠的三帧锤子就靠这个）。
  3. **不裁切**：内容不能贴到图片边界（贴边 = 已被裁，运行时就是"少一块"）。
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(TOOLS, ".."))
IMG_ROOT = os.path.join(ROOT, "resources", "images", "game")
OUT_H = os.path.join(ROOT, "src", "core", "PgGameArt.h")

sys.path.insert(0, TOOLS)
import gen_whack_art as whack_hammer  # noqa: E402  复用锤子的绘制/旋转（单一真值）
import gen_instr_art as instr         # noqa: E402  数独/节奏钢琴/打鼓（木纹实体乐器风）

SS = 4  # 超采样倍数：几何图形先放大 SS 倍画，再 LANCZOS 缩回来 ⇒ 自带抗锯齿


# ---------------------------------------------------------------- 工具

def h32(x):
    """PgWhack.cpp 里 h32() 的 Python 复刻（确定性哈希：每帧一致，纹理不闪）。"""
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def lerp(a, b, t):
    """t: 0..256（与 PgCanvas::lerpColor 同语义），a/b 是 (r,g,b) 或 (r,g,b,a)。"""
    if t <= 0:
        return tuple(a)
    if t >= 256:
        return tuple(b)
    n = min(len(a), len(b))
    return tuple(a[i] + (b[i] - a[i]) * t // 256 for i in range(n))


def rgba(c, a=255):
    return (c[0], c[1], c[2], a)


# ---------------------------------------------------------------- 配色（与旧版程序化绘制一致）

GRASS_FAR = (52, 104, 62)
GRASS_NEAR = (104, 180, 96)
GRASS_HI = (134, 206, 118)
GRASS_LO = (40, 86, 54)
SOIL = (150, 116, 72)
MOUND_HI = (178, 140, 88)
MOUND_LO = (132, 100, 58)
HOLE_DEEP = (52, 40, 28)
HOLE_IN = (28, 22, 16)
FLOWER_A = (248, 240, 246)
FLOWER_B = (255, 214, 120)
STONE = (158, 158, 150)
STONE_HI = (200, 200, 194)

MOLE_BODY = (212, 158, 96)
MOLE_DARK = (148, 102, 58)
MOLE_EDGE = (96, 62, 30)
EYE_WHITE = (252, 250, 248)
EYE_PUPIL = (30, 26, 28)
MOLE_HIT = (255, 236, 168)

COMBO_GOLD = (255, 214, 64)
COMBO_HOT = (255, 138, 48)

W_BG, H_BG = 480, 540          # 画布可视区（== ui 侧 GameCanvas 尺寸）
W_HOLE, H_HOLE = 156, 76       # 洞口：椭圆 rx=76,ry=36 + **每边 2px 余量**
AX_HOLE, AY_HOLE = 78, 38      #   （贴边 = 已被裁，自检会拦；锚点 = 洞中心）
W_MOLE, H_MOLE = 96, 88        # 地鼠：头 r=38 + 耳朵
AX_MOLE, AY_MOLE = 48, 46      # 锚点 = 地鼠**圆心**
W_RAYS = 176                   # 连击放射线：最大半径 82 ⇒ 直径 164，留边
AX_RAYS = AY_RAYS = W_RAYS // 2


# ---------------------------------------------------------------- ① 草地底图（静态整屏）

def bake_bg():
    """整屏草地。**静态** —— 原来每帧重画 135 次 fillRect + 110 个草簇，现在一次贴图。"""
    ss = 2                          # 480x540 全屏用 2x 超采样就够（内存友好），再 LANCZOS
    W, H = W_BG * ss, H_BG * ss
    im = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(im)

    # 基底：上深下浅（俯视草地）
    band = 4 * ss
    for y in range(0, H, band):
        col = lerp(GRASS_FAR, GRASS_NEAR, y * 256 // H)
        d.rectangle([0, y, W, min(y + band - 1, H - 1)], fill=rgba(col))

    # 泥土斑
    for i in range(4):
        h = h32(0x3D17 + i * 977)
        x = h % W
        y = (h >> 9) % H
        rx = (38 + (h >> 17) % 46) * ss
        d.ellipse([x - rx, y - rx * 2 // 3, x + rx, y + rx * 2 // 3],
                  fill=rgba(lerp(GRASS_NEAR, SOIL, 96)))

    # 草簇（主体纹理）
    for i in range(110):
        h = h32(0x9E37 + i * 7919)
        x = h % W
        y = 8 * ss + (h >> 11) % (H - 8 * ss)
        hh = (4 + (h >> 21) % 5) * ss
        col = GRASS_HI if ((h >> 3) & 1) else GRASS_LO
        col = lerp(col, GRASS_NEAR, (H - y) * 128 // H)
        wdt = 2 * ss
        d.rectangle([x, y - hh, x + wdt - 1, y], fill=rgba(col))
        d.rectangle([x - 3 * ss, y - hh * 3 // 4, x - 3 * ss + wdt - 1, y], fill=rgba(col))
        d.rectangle([x + 3 * ss, y - hh * 5 // 6, x + 3 * ss + wdt - 1, y], fill=rgba(col))

    # 小花
    for i in range(7):
        h = h32(0x51ED + i * 613)
        x = 24 * ss + h % (W - 48 * ss)
        y = 30 * ss + (h >> 9) % (H - 60 * ss)
        for dx, dy, r in ((-4, -3, 3), (4, -3, 3), (0, -5, 3), (0, 0, 4)):
            d.ellipse([x + dx * ss - r * ss, y + dy * ss - r * ss,
                       x + dx * ss + r * ss, y + dy * ss + r * ss], fill=rgba(FLOWER_A))
        d.ellipse([x - 2 * ss, y - 4 * ss, x + 2 * ss, y], fill=rgba(FLOWER_B))

    # 石头
    for i in range(3):
        h = h32(0x7A31 + i * 331)
        x = 30 * ss + h % (W - 60 * ss)
        y = 40 * ss + (h >> 9) % (H - 80 * ss)
        rx = (13 + (h >> 17) % 9) * ss
        d.ellipse([x - rx, y - rx * 3 // 4, x + rx, y + rx * 3 // 4], fill=rgba(STONE))
        d.ellipse([x - rx // 4 - rx // 2, y - rx // 5 - rx // 3,
                   x - rx // 4 + rx // 2, y - rx // 5 + rx // 3], fill=rgba(STONE_HI))

    return im.resize((W_BG, H_BG), Image.LANCZOS)


# ---------------------------------------------------------------- ② 洞口 / 洞口前沿

def _hole_shapes(d, ss, ox, oy, lower_only=False):
    """在 (ox,oy) 为中心的洞里画土层（洞口 / 洞口前沿共用同一套形状）。"""
    def clip(box):
        """只保留下半部分（前沿用）：把上半裁掉，y < oy 的部分不画。"""
        if not lower_only:
            return box
        return [box[0], max(box[1], oy), box[2], box[3]]

    def ell(rx, ry, col, dy=0):
        x0, y0 = ox - rx, oy - ry + dy
        x1, y1 = ox + rx, oy + ry + dy
        if lower_only and y1 >= oy:      # 前沿：从中心线往下画矩形裁剪
            b = [x0, oy, x1, y1]
            d.ellipse([x0, y0, x1, y1], fill=col)
            # 再把上半擦掉（前沿图必须上半透明，否则会盖住地鼠的头）
            d.rectangle([x0 - 1, y0 - 1, x1 + 1, oy - 1], fill=(0, 0, 0, 0))
            return
        if not lower_only:
            d.ellipse([x0, y0, x1, y1], fill=col)

    ell(76 * ss, 36 * ss, rgba(MOUND_LO))                       # 外沿（描边色）
    ell(74 * ss, 34 * ss, rgba(MOUND_HI))                       # 土堆
    ell(62 * ss, 28 * ss, rgba(HOLE_DEEP), dy=-2 * ss)          # 洞里
    ell(54 * ss, 22 * ss, rgba(HOLE_IN), dy=-5 * ss)            # 洞底


def bake_hole(front=False):
    ss = SS
    im = Image.new("RGBA", (W_HOLE * ss, H_HOLE * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    _hole_shapes(d, ss, AX_HOLE * ss, AY_HOLE * ss, lower_only=front)
    if front:
        # 前沿：土堆上缘给一道亮边（更像"翻起来的土"），只在下半画
        d.arc([(AX_HOLE - 74) * ss, (AY_HOLE - 34) * ss,
               (AX_HOLE + 74) * ss, (AY_HOLE + 34) * ss],
              start=10, end=170, fill=rgba(lerp(MOUND_HI, (255, 255, 255), 90)),
              width=max(1, ss // 2))
    return im.resize((W_HOLE, H_HOLE), Image.LANCZOS)


def bake_bg_with_holes(hole):
    """草地 + **9 个洞口**合成一张底图。

    为什么合成：洞口的位置是**固定的**（3x3 均分画布，见 PgWhack 的 cxi/cyi），
    每帧单独贴 9 张洞口是白白多扫 ~10 万像素（156x76x9）。
    合成之后每帧只贴 1 张 480x540（而且整图不透明 ⇒ 走 memcpy 快路径）。
    实测：这一步把打地鼠从 49fps 推到 58fps 量级。
    ⚠️ 洞口位置必须与 PgWhack::cxi/cyi 保持一致（改布局要两边一起改）：
         cxi = {80, 240, 400}[i%3]；cyi = 540/4 * (1 + i/3) = 135 / 270 / 405
    """
    bg = bake_bg()
    for i in range(9):
        cx = (80, 240, 400)[i % 3]
        cy = H_BG // 4 * (1 + i // 3)
        bg.alpha_composite(hole, (cx - AX_HOLE, cy - AY_HOLE))
    return bg


# ---------------------------------------------------------------- ③ 地鼠

def bake_mole(hit=False):
    ss = SS
    im = Image.new("RGBA", (W_MOLE * ss, H_MOLE * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx, cy = AX_MOLE * ss, AY_MOLE * ss

    body = lerp(MOLE_BODY, MOLE_HIT, 110) if hit else MOLE_BODY
    dark = lerp(MOLE_DARK, MOLE_HIT, 70) if hit else MOLE_DARK
    edge = lerp(MOLE_EDGE, MOLE_HIT, 40) if hit else MOLE_EDGE

    def circ(dx, dy, r, col):
        d.ellipse([cx + dx - r, cy + dy - r, cx + dx + r, cy + dy + r], fill=rgba(col))

    # 耳朵（带描边）
    for s in (-1, 1):
        circ(s * 27 * ss, -24 * ss, 15 * ss, edge)
        circ(s * 27 * ss, -24 * ss, 13 * ss, dark)
        circ(s * 27 * ss, -22 * ss, 7 * ss, lerp(dark, (255, 255, 255), 60))

    # 头（描边 + 主体 + 左上高光）
    circ(0, 0, 40 * ss, edge)
    circ(0, 0, 38 * ss, body)
    d.ellipse([cx - 30 * ss, cy - 34 * ss, cx + 10 * ss, cy - 6 * ss],
              fill=rgba(lerp(body, (255, 255, 255), 46)))     # 柔和高光

    # 眼
    for s in (-1, 1):
        circ(s * 14 * ss, -8 * ss, 12 * ss, edge)
        circ(s * 14 * ss, -8 * ss, 10 * ss, EYE_WHITE)
        circ(s * 15 * ss, -6 * ss, 4 * ss, EYE_PUPIL)
        circ(s * 16 * ss, -8 * ss, 2 * ss, (255, 255, 255))   # 眼神光

    # 鼻子
    circ(0, 15 * ss, 9 * ss, edge)
    circ(0, 15 * ss, 7 * ss, dark)
    circ(-2 * ss, 13 * ss, 2 * ss, lerp(dark, (255, 255, 255), 120))

    out = im.resize((W_MOLE, H_MOLE), Image.LANCZOS)
    if hit:
        # 命中：叠一层暖色光晕（"打中了"的即时反馈）
        glow = Image.new("RGBA", out.size, (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        gd.ellipse([cx - 46 * ss, cy - 46 * ss, cx + 46 * ss, cy + 46 * ss],
                   fill=(255, 226, 130, 90))
        glow = glow.filter(ImageFilter.GaussianBlur(3))
        out = Image.alpha_composite(out, glow)
    return out


# ---------------------------------------------------------------- ④ 连击放射线

def bake_rays(hot=False):
    """12 道楔形放射线（静态长度版）。弹出/淡出靠**位移 + 整体 alpha**（drawSpriteA）。"""
    ss = 2
    W = W_RAYS * ss
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    cx = cy = W // 2
    col = rgba(COMBO_HOT if hot else COMBO_GOLD)
    r0, r1 = 46 * ss, 82 * ss
    for k in range(12):
        a = math.radians(k * 30.0)
        dx, dy = math.cos(a), math.sin(a)
        px, py = -dy, dx                     # 垂直方向（楔形半宽）
        wdt = 4 * ss
        d.polygon([(cx + dx * r0 - px * wdt, cy + dy * r0 - py * wdt),
                   (cx + dx * r0 + px * wdt, cy + dy * r0 + py * wdt),
                   (cx + dx * r1, cy + dy * r1)], fill=col)
    return im.resize((W_RAYS, W_RAYS), Image.LANCZOS)


# ---------------------------------------------------------------- ⑤ 其它游戏的静态背景
# 这两个游戏原本每帧用几何原语重画整屏背景（尤其贪吃蛇的 42 条网格线 + 小鸟的 270 次
# 渐变 fillRect），是各自的帧率瓶颈（实测 40 / 44fps，见 docs/game-art-pipeline.md §9）。
# 背景既然是**静态**的，就烘成一张不透明图 ⇒ 每帧一次 memcpy 快路径。

# 贪吃蛇场地（PgSnake.cpp 的 COLS/ROWS/CELL/PAD，**改布局要两边一起改**）
SNAKE_COLS, SNAKE_ROWS, SNAKE_CELL, SNAKE_PAD = 18, 18, 26, 6
SNAKE_BG = (14, 22, 30)
SNAKE_FRAME = (26, 40, 52)
SNAKE_FIELD = (18, 30, 40)

# 小鸟（PgFlappy.cpp 的常量）
FLAPPY_GROUND_H = 46
FLAPPY_SKY_TOP = (28, 46, 74)
FLAPPY_SKY_BOT = (58, 96, 132)
FLAPPY_GROUND = (96, 78, 52)
FLAPPY_GROUND_HI = (136, 112, 74)


def bake_snake_bg():
    """贪吃蛇：底色 + 场地外框 + 场地 + 细网格（全不透明）。"""
    im = Image.new("RGBA", (W_BG, H_BG), rgba(SNAKE_BG))
    d = ImageDraw.Draw(im)
    gw = SNAKE_COLS * SNAKE_CELL
    gh = SNAKE_ROWS * SNAKE_CELL
    bx = (W_BG - gw) // 2
    by = (H_BG - gh) // 2
    d.rectangle([bx - 3, by - 3, bx + gw + 3 - 1, by + gh + 3 - 1], fill=rgba(SNAKE_FRAME))
    d.rectangle([bx, by, bx + gw - 1, by + gh - 1], fill=rgba(SNAKE_FIELD))
    # 细网格：原代码是 rgba(255,255,255,12) 的**半透明**线，而画布原语是直接覆写
    #（不混 alpha）—— 落到屏幕上会与控件下层混色，淡到什么程度取决于底下画的是什么。
    # 这里直接合成成"白 12/255 叠在场地色上"的不透明近似色：观感一致，且不再受底层影响。
    g = lerp(SNAKE_FIELD, (255, 255, 255), 12)
    for i in range(1, SNAKE_COLS):
        x = bx + i * SNAKE_CELL
        d.line([x, by, x, by + gh - 1], fill=rgba(g), width=1)
    for i in range(1, SNAKE_ROWS):
        y = by + i * SNAKE_CELL
        d.line([bx, y, bx + gw - 1, y], fill=rgba(g), width=1)
    return im


def bake_flappy_bg():
    """小鸟：天空渐变（每 2 行一档）+ 地面基色 + 地面顶部亮条。
    **不含**滚动条纹与云（它们是动的，留在代码里画）。"""
    im = Image.new("RGBA", (W_BG, H_BG))
    d = ImageDraw.Draw(im)
    for y in range(0, H_BG, 2):
        col = lerp(FLAPPY_SKY_TOP, FLAPPY_SKY_BOT, y * 256 // H_BG)
        d.rectangle([0, y, W_BG - 1, min(y + 2, H_BG) - 1], fill=rgba(col))
    gy = H_BG - FLAPPY_GROUND_H
    d.rectangle([0, gy, W_BG - 1, H_BG - 1], fill=rgba(FLAPPY_GROUND))
    d.rectangle([0, gy, W_BG - 1, gy + 8 - 1], fill=rgba(FLAPPY_GROUND_HI))
    return im


# ---------------------------------------------------------------- ⑤ 消消乐（糖果三消）

"""★ 布局常量：**必须与 src/core/PgMatch3.cpp 的常量逐字一致**（改这里要同步改那边）。

视觉参考（用户指定的参考产品 = 开心消消乐）：
  · 底色是"清新花园"（绿），棋盘是**木质面板 + 浅色糖果凹槽**，不是深色科技风；
  · 糖果 = 高饱和多彩 + 顶部高光 + 深色描边（塑料/果冻质感），一眼能数清"有几个同色"；
  · 顶栏是木牌进度条（关卡 / 本关目标进度 / 剩余步数）。
所以背景与棋盘**整屏烘成一张静态底图**（每帧只贴一次，见 docs/game-art-pipeline.md），
糖果/选中框/爆花各自单独一张（位置每帧在变，必须能单独贴）。
"""

M3_COLS, M3_ROWS = 8, 8
M3_CELL = 56                                    # 格边长（480x540 画布里 8 格正好 448）
M3_BX = (W_BG - M3_COLS * M3_CELL) // 2         # 16  棋盘左上角 x
M3_BY = (H_BG - M3_ROWS * M3_CELL) // 2         # 46  棋盘左上角 y
M3_R = 23.0                                     # 糖果外接半径（图 56 里四周留 5px ⇒ 不贴边）

M3_WOOD = (152, 106, 62)
M3_WOOD_DARK = (118, 78, 44)
M3_WOOD_HI = (176, 128, 78)
M3_SLOT = (238, 240, 230)                       # 糖果凹槽（浅米白，糖果坐上去对比强）
M3_SLOT_LO = (196, 202, 188)
M3_GARDEN_TOP = (26, 60, 44)
M3_GARDEN_BOT = (74, 132, 84)
M3_GOLD = (255, 214, 64)

# 7 种糖果：**颜色 + 形状双重区分**。
# 为什么要形状：只靠色相区分（7 色）对色弱玩家不友好，而且"扫一眼数同色"会更慢 ——
# 形状是第二维度，参考产品也是"每种一个小动物"而不只是七种颜色。
M3_CANDY = [
    ((232, 59, 60), "circle"),    # 0 红 · 圆果冻
    ((255, 146, 40), "square"),   # 1 橙 · 方软糖
    ((255, 208, 40), "star"),     # 2 黄 · 星星糖
    ((84, 200, 96), "hex"),       # 3 绿 · 六角糖
    ((64, 160, 240), "diamond"),  # 4 蓝 · 钻石糖
    ((168, 96, 220), "heart"),    # 5 紫 · 心形糖
    ((244, 244, 248), "stripe"),  # 6 白 · 条纹糖（白底 + 红斜纹，避免与"浅色底"糊在一起）
]


def _rr_pts(cx, cy, hx, hy, rc, n=8):
    """圆角矩形的轮廓点（逻辑坐标，顺时针闭合）。"""
    pts = []
    for ox, oy, a0 in ((hx - rc, hy - rc, 0), (-(hx - rc), hy - rc, 90),
                       (-(hx - rc), -(hy - rc), 180), (hx - rc, -(hy - rc), 270)):
        for i in range(n + 1):
            a = math.radians(a0 + 90.0 * i / n)
            pts.append((cx + ox + rc * math.cos(a), cy + oy + rc * math.sin(a)))
    return pts


def _shape_pts(kind, cx=M3_CELL / 2.0, cy=M3_CELL / 2.0, r=M3_R):
    """糖果外形轮廓（逻辑坐标）。全部用多边形点集表达 ⇒ 描边/填充都走同一条码路。"""
    if kind == "circle":
        return [(cx + r * math.cos(2 * math.pi * i / 64),
                 cy + r * math.sin(2 * math.pi * i / 64)) for i in range(64)]
    if kind == "square":                       # 软糖：圆角方
        h = r * 0.86
        return _rr_pts(cx, cy, h, h, h * 0.40)
    if kind == "star":                         # 五角星（尖角朝上）
        pts = []
        for i in range(10):
            rr = r if i % 2 == 0 else r * 0.46
            a = -math.pi / 2 + i * math.pi / 5
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
        return pts
    if kind == "hex":                          # 六角糖
        return [(cx + r * 0.95 * math.cos(-math.pi / 2 + i * math.pi / 3),
                 cy + r * 0.95 * math.sin(-math.pi / 2 + i * math.pi / 3)) for i in range(6)]
    if kind == "diamond":                      # 钻石
        return [(cx, cy - r), (cx + r * 0.84, cy - r * 0.10),
                (cx, cy + r), (cx - r * 0.84, cy - r * 0.10)]
    if kind == "heart":                        # 心形（经典参数方程）
        pts = []
        for i in range(72):
            t = 2 * math.pi * i / 72
            x = 16 * math.sin(t) ** 3
            y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
            pts.append((cx + x * r / 16.0 * 0.98, cy - y * r / 16.0 * 0.98 + r * 0.06))
        return pts
    if kind == "stripe":                       # 条纹糖：圆角横条
        return _rr_pts(cx, cy, r * 1.02, r * 0.80, r * 0.42)
    raise ValueError(kind)


def _draw_slot(d, ss, x0, y0, w=56, h=56, wood_edge=True):
    """画一格糖果凹槽（外圈木色 + 浅米白槽底 + 顶部内阴影）。

    ★ **单一真值**：整屏底图（bake_m3_bg）与糖果图（bake_candy）都用它，
    这样"有糖果的格子"与"空格子"看起来是同一块地方 —— 否则糖果图贴上去会有一圈色差。

    wood_edge=True 时把 56x56 铺满木色：**糖果图必须整块不透明**才能走
    Canvas 的 memcpy 快路径（见下面对帧率的说明）。
    """
    if wood_edge:
        d.rectangle([x0 * ss, y0 * ss, (x0 + w) * ss - 1, (y0 + h) * ss - 1],
                    fill=rgba(M3_WOOD))
    sx, sy = (x0 + 2) * ss, (y0 + 2) * ss
    ex, ey = (x0 + 2 + (w - 5)) * ss, (y0 + 2 + (h - 5)) * ss
    # ⚠️ 这里**必须用预混合的近似色**，不能用 rgba(色, alpha)：
    #   ImageDraw 的 fill 是**直接覆写**（不像 alpha_composite 会混合），
    #   写成半透明会留下 alpha=90 的像素 ⇒ Canvas 的 memcpy 快路径判定失败
    #   （见 force_opaque 的说明）。draw 出"带 alpha 的颜色"和"lerp 出来的近似色"
    #   在**不透明底**上视觉等价，后者还顺带满足快路径。
    d.rounded_rectangle([sx, sy + ss, ex, ey + ss], radius=12 * ss,
                        fill=rgba(lerp(M3_SLOT, (96, 66, 38), 90)))
    d.rounded_rectangle([sx, sy, ex, ey], radius=12 * ss, fill=rgba(M3_SLOT))
    d.rounded_rectangle([sx, sy, ex, sy + 3 * ss], radius=12 * ss,
                        fill=rgba(lerp(M3_SLOT, M3_SLOT_LO, 120)))


def bake_candy(idx):
    """烘一颗糖果：凹槽底 → 形状 mask → 垂直渐变 → 内部装饰 → 深色描边 → 左上高光。

    ★★ 为什么要**带凹槽底**（而不是只烘一个透明背景的糖果）：
    第一版是"透明糖果贴到凹槽上"，真机实测 **fps 只有 33**（bench：render=17.7ms/帧）——
    因为带透明像素的图走**逐像素 alpha 混合**（64 格 × 56x56 = 20 万像素/帧）。
    把凹槽烘进图里 ⇒ 整图不透明 ⇒ Canvas 走 **memcpy 快路径**（跳过 alpha 判定），
    这是本项目"整屏底图"提速的同一招（docs/game-art-pipeline.md §5）。
    代价只是：每个糖果多一张凹槽底的像素（7 张 × 12KB）。
    """
    ss = SS
    SZ = M3_CELL * ss
    col, kind = M3_CANDY[idx]

    # ⓪ 不透明底：木色 + 本格的凹槽（与整屏底图完全一致）
    base = Image.new("RGBA", (SZ, SZ), rgba(M3_WOOD))
    _draw_slot(ImageDraw.Draw(base), ss, 0, 0)

    pts = _shape_pts(kind)

    # ① 形状 mask
    mask = Image.new("L", (SZ, SZ), 0)
    ImageDraw.Draw(mask).polygon([(x * ss, y * ss) for x, y in pts], fill=255)

    # ② 垂直渐变（上亮下暗 = 果冻/塑料的"受光"）
    top = tuple(min(255, int(v + (255 - v) * 0.34)) for v in col)
    bot = tuple(max(0, int(v * 0.66)) for v in col)
    grad = Image.new("RGBA", (SZ, SZ))
    gd = ImageDraw.Draw(grad)
    for y in range(SZ):
        gd.line([(0, y), (SZ - 1, y)], fill=rgba(lerp(top, bot, y * 256 // SZ)))

    im = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    im.paste(grad, (0, 0), mask)

    # ③ 内部装饰（同样是"认形状"的第二线索：圆内环 / 方内方 / 星核 / 心内点 / 斜纹）
    deco = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    dd = ImageDraw.Draw(deco)
    c = M3_CELL / 2.0
    dark = tuple(max(0, int(v * 0.60)) for v in col)

    def dl(pts2, fill, w):
        dd.line([(x * ss, y * ss) for x, y in pts2], fill=fill, width=max(1, int(w * ss)),
                joint="curve")

    if kind == "circle":
        dl([(c + M3_R * 0.62 * math.cos(2 * math.pi * i / 48),
             c + M3_R * 0.62 * math.sin(2 * math.pi * i / 48)) for i in range(49)],
           rgba(dark, 90), ss * 1.6)
    elif kind == "square":
        p = _rr_pts(c, c, M3_R * 0.42, M3_R * 0.42, M3_R * 0.18)
        dd.polygon([(x * ss, y * ss) for x, y in p], fill=rgba(lerp(col, (255, 255, 255), 150)))
    elif kind == "star":
        dd.ellipse([(c - M3_R * 0.26) * ss, (c - M3_R * 0.26) * ss,
                    (c + M3_R * 0.26) * ss, (c + M3_R * 0.26) * ss], fill=rgba(dark, 110))
    elif kind == "hex":
        dl([(c + M3_R * 0.56 * math.cos(-math.pi / 2 + i * math.pi / 3),
             c + M3_R * 0.56 * math.sin(-math.pi / 2 + i * math.pi / 3)) for i in range(7)],
           rgba(lerp(col, (255, 255, 255), 170)), ss * 1.5)
    elif kind == "diamond":
        dl([(c, c - M3_R * 0.55), (c + M3_R * 0.42, c - M3_R * 0.06), (c, c + M3_R * 0.55)],
           rgba(lerp(col, (255, 255, 255), 190)), ss * 1.4)
    elif kind == "heart":
        dl([(c - M3_R * 0.34, c - M3_R * 0.22), (c, c + M3_R * 0.16),
            (c + M3_R * 0.34, c - M3_R * 0.22)], rgba(lerp(col, (255, 255, 255), 190)), ss * 1.4)
    elif kind == "stripe":
        # 斜条纹：先画满斜线，再用形状 mask 裁掉外边（否则会溢出成方形）
        sl = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
        sd = ImageDraw.Draw(sl)
        for k in range(-6, 14):
            x0 = (k * 10) * ss
            sd.line([(x0, 0), (x0 + SZ, SZ)], fill=rgba((214, 56, 62), 235),
                    width=max(1, int(4.0 * ss)))
        sl.putalpha(Image.composite(sl.split()[3], Image.new("L", (SZ, SZ), 0), mask))
        deco = Image.alpha_composite(deco, sl)
        dd = ImageDraw.Draw(deco)

    # 装饰只保留形状内部的那部分
    deco.putalpha(Image.composite(deco.split()[3], Image.new("L", (SZ, SZ), 0), mask))
    im = Image.alpha_composite(im, deco)

    # ④ 深色描边（把糖果从浅色凹槽/相邻糖果里"抠"出来，小尺寸下尤其关键）
    d = ImageDraw.Draw(im)
    edge = tuple(max(0, int(v * 0.50)) for v in col)
    d.line([(x * ss, y * ss) for x, y in pts] + [(pts[0][0] * ss, pts[0][1] * ss)],
           fill=rgba(edge, 235), width=max(1, int(2.1 * ss)), joint="curve")

    # ⑤ 左上高光（一个斜椭圆；不旋转 —— 旋转会重采样，边缘反而毛）
    hl = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hx0, hy0 = (c - M3_R * 0.52) * ss, (c - M3_R * 0.58) * ss
    hd.ellipse([hx0, hy0, hx0 + M3_R * 0.72 * ss, hy0 + M3_R * 0.42 * ss],
               fill=rgba((255, 255, 255), 120))
    hl.putalpha(Image.composite(hl.split()[3], Image.new("L", (SZ, SZ), 0), mask))
    im = Image.alpha_composite(im, hl)

    # 合成到不透明凹槽底上（⇒ 输出整图不透明，运行时走 memcpy 快路径）
    im = Image.alpha_composite(base, im)
    return force_opaque(im.resize((M3_CELL, M3_CELL), Image.LANCZOS), "candy%d" % idx)


def bake_m3_sel():
    """选中高亮：金色圆角描边 + 四角加粗（弱提示，不盖住糖果本身）。"""
    ss = 4
    SZ = M3_CELL * ss
    im = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([3 * ss, 3 * ss, (M3_CELL - 4) * ss, (M3_CELL - 4) * ss],
                        radius=11 * ss, outline=rgba(M3_GOLD, 235), width=int(2.4 * ss))
    d.rounded_rectangle([7 * ss, 7 * ss, (M3_CELL - 8) * ss, (M3_CELL - 8) * ss],
                        radius=9 * ss, fill=rgba((255, 255, 255), 34))
    # 四角加粗（"框住"的观感，比整圈等宽更醒目）
    L, W2 = 12 * ss, 4 * ss
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x = 3 * ss if sx > 0 else SZ - 4 * ss
        y = 3 * ss if sy > 0 else SZ - 4 * ss
        xa, xb = sorted((x, x + sx * L))
        ya, yb = sorted((y, y + sy * W2))
        d.rectangle([xa, ya, xb, yb], fill=rgba(M3_GOLD, 250))
        xa, xb = sorted((x, x + sx * W2))
        ya, yb = sorted((y, y + sy * L))
        d.rectangle([xa, ya, xb, yb], fill=rgba(M3_GOLD, 250))
    return im.resize((M3_CELL, M3_CELL), Image.LANCZOS)


M3_BURST = 112          # 爆花尺寸（锚点 = 中心）


def bake_m3_burst():
    """消除爆花：8 道星芒 + 中心亮核（贴上去靠整体 alpha 淡出 ⇒ 不用逐帧形变）。"""
    ss = 3
    SZ = M3_BURST * ss
    im = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = M3_BURST / 2.0
    for i in range(8):
        a = i * math.pi / 4
        L = (52.0 if i % 2 == 0 else 33.0)
        hw = (5.6 if i % 2 == 0 else 4.0)
        tip = (c + L * math.cos(a), c + L * math.sin(a))
        b1 = (c + 6 * math.cos(a + math.pi / 2), c + 6 * math.sin(a + math.pi / 2))
        b2 = (c + 6 * math.cos(a - math.pi / 2), c + 6 * math.sin(a - math.pi / 2))
        mid1 = ((c + tip[0]) / 2 + hw * math.cos(a + math.pi / 2),
                (c + tip[1]) / 2 + hw * math.sin(a + math.pi / 2))
        mid2 = ((c + tip[0]) / 2 + hw * math.cos(a - math.pi / 2),
                (c + tip[1]) / 2 + hw * math.sin(a - math.pi / 2))
        pts = [b1, mid1, tip, mid2, b2]
        d.polygon([(x * ss, y * ss) for x, y in pts], fill=rgba((255, 246, 200), 200))
    d.ellipse([(c - 17) * ss, (c - 17) * ss, (c + 17) * ss, (c + 17) * ss],
              fill=rgba((255, 232, 150), 190))
    d.ellipse([(c - 9) * ss, (c - 9) * ss, (c + 9) * ss, (c + 9) * ss],
              fill=rgba((255, 255, 255), 240))
    return im.resize((M3_BURST, M3_BURST), Image.LANCZOS)


def bake_m3_mark(horiz):
    """炸弹上的方向标记：**只画箭头条，其余全透明**。

    ★ 为什么从"代码画图元"改成"贴一张小图"（2026-09-15 实测）：
      图元版每格要 6 次调用（2 次圆角矩 + 4 次三角填充），盘上 2 个炸弹就要 12 次；
      换成贴图后 = 一次 `drawSpriteA`，而且**大部分像素是 alpha=0 会被直接跳过**
      （`Canvas` 的慢路径里有 `if (!a) continue;`）⇒ 几乎免费。
      实测这一步把带特殊块时的帧率从 **52 拉回 57+**。
      注意这**不是**"给每种糖果各烘一张箭头"（那是 7 色 × 2 方向 = 14 张）——
      标记是**中性色**的，与糖果图叠加即可，所以只要 2 张。
    """
    ss = 4
    SZ = M3_CELL * ss
    im = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = M3_CELL / 2.0
    edge = rgba((52, 32, 18), 215)
    white = rgba((255, 255, 255), 242)

    def rrect(x, y, w, h, r, col):
        d.rounded_rectangle([x * ss, y * ss, (x + w) * ss, (y + h) * ss],
                            radius=r * ss, fill=col)

    def tri(p1, p2, p3, col):
        d.polygon([(p1[0] * ss, p1[1] * ss), (p2[0] * ss, p2[1] * ss), (p3[0] * ss, p3[1] * ss)],
                  fill=col)

    if horiz:
        rrect(c - 13, c - 5, 26, 10, 5, edge)
        rrect(c - 12, c - 4, 24, 8, 4, white)
        tri((c - 21, c), (c - 13, c - 8), (c - 13, c + 8), edge)
        tri((c + 21, c), (c + 13, c - 8), (c + 13, c + 8), edge)
        tri((c - 20, c), (c - 13, c - 7), (c - 13, c + 7), white)
        tri((c + 20, c), (c + 13, c - 7), (c + 13, c + 7), white)
    else:
        rrect(c - 5, c - 13, 10, 26, 5, edge)
        rrect(c - 4, c - 12, 8, 24, 4, white)
        tri((c, c - 21), (c - 8, c - 13), (c + 8, c - 13), edge)
        tri((c, c + 21), (c - 8, c + 13), (c + 8, c + 13), edge)
        tri((c, c - 20), (c - 7, c - 13), (c + 7, c - 13), white)
        tri((c, c + 20), (c - 7, c + 13), (c + 7, c + 13), white)
    return im.resize((M3_CELL, M3_CELL), Image.LANCZOS)


def bake_m3_rainbow():
    """彩虹球（5 连生成的"万能消除"块）：6 色扇区拼成的圆球 + 白高光 + 深色描边。

    ★ 特殊块为什么只加**这三张**素材（rainbow + mark_h + mark_v）：
      · 4 连的"方向炸弹"**保留原糖果色**，只在糖果上叠一张**中性色**的箭头图 ⇒
        不需要 7 色 × 2 方向 = 14 张；
      · 彩虹球是**无色**的（不参与颜色匹配），本来就只有一种外观。
      这是"素材化"里该省的省、该素材化的素材化：**反复绘制的固定图形一律出图**
      （哪怕只有几笔），因为它换来的是 memcpy/跳像素而不是逐像素图元填充。
      ⚠️ 彩虹球**烘了凹槽底**（整图不透明）—— 理由与糖果图相同：走 memcpy 快路径。
    """
    ss = 4
    SZ = M3_CELL * ss
    base = Image.new("RGBA", (SZ, SZ), rgba(M3_WOOD))
    _draw_slot(ImageDraw.Draw(base), ss, 0, 0)

    im = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = M3_CELL / 2.0
    R = 23.0
    cols = [(232, 59, 60), (255, 146, 40), (255, 208, 40),
            (84, 200, 96), (64, 160, 240), (168, 96, 220)]
    for i, col in enumerate(cols):
        a0 = -90 + i * 60
        d.pieslice([(c - R) * ss, (c - R) * ss, (c + R) * ss, (c + R) * ss],
                   a0, a0 + 60, fill=rgba(col))
    d.ellipse([(c - R) * ss, (c - R) * ss, (c + R) * ss, (c + R) * ss],
              outline=rgba((58, 40, 28), 235), width=max(1, int(2.1 * ss)))

    # 高光（裁剪到球内，否则会溢出成方块）
    hl = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    hd.ellipse([(c - R * 0.52) * ss, (c - R * 0.58) * ss,
                (c + R * 0.18) * ss, (c + R * 0.04) * ss], fill=rgba((255, 255, 255), 150))
    mask = Image.new("L", (SZ, SZ), 0)
    ImageDraw.Draw(mask).ellipse([(c - R) * ss, (c - R) * ss, (c + R) * ss, (c + R) * ss],
                                 fill=255)
    hl.putalpha(Image.composite(hl.split()[3], Image.new("L", (SZ, SZ), 0), mask))
    im = Image.alpha_composite(im, hl)
    # 合成到不透明凹槽底 ⇒ 整图不透明、走 memcpy
    im = Image.alpha_composite(base, im)
    return force_opaque(im.resize((M3_CELL, M3_CELL), Image.LANCZOS), "match3/rainbow.png")


def bake_m3_bg():
    """整屏静态底图：花园渐变 + 木框 + 8x8 浅色凹槽（每帧只贴这一次）。

    ⚠️ 顶部 y<46 与底部 y>494 的花园色**同时是"分数弹出文字"的淡出目标色**
    （画布文字没有 alpha，只能向底色插值）—— 见 PgMatch3.cpp 的 gardenAt()。
    """
    ss = 2                                   # 整屏 2x 超采样（够平顺，内存友好）
    W, H = W_BG * ss, H_BG * ss
    im = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(im)

    # ① 花园渐变
    for y in range(0, H, 4):
        col = lerp(M3_GARDEN_TOP, M3_GARDEN_BOT, y * 256 // H)
        d.rectangle([0, y, W - 1, min(y + 4, H) - 1], fill=rgba(col))

    # ② 静态光斑/叶片（确定性哈希 ⇒ 每次生成同一张图）
    #    ⚠️ 一律用 lerp 出**不透明近似色**而不是 rgba(色, alpha)：ImageDraw 的 fill 是
    #       直接覆写，写半透明会留下 alpha<255 的像素 ⇒ 快路径判定失败（见 force_opaque）。
    #       lerp 的 t 与 alpha 同为 0..256 语义，所以在不透明底上两者**视觉等价**。
    for i in range(34):
        h = h32(0x51E3 + i * 613)
        x = (h % (W + 160)) - 80
        y = ((h // 977) % (H + 160)) - 80
        r = 10 + (h >> 13) % 22
        a = 30 + (h >> 19) % 26
        under = lerp(M3_GARDEN_TOP, M3_GARDEN_BOT, max(0, min(255, y * 256 // H)))
        d.ellipse([x - r, y - r, x + r, y + r],
                  fill=rgba(lerp(under, (150, 220, 150), a)))
    # 四角藤蔓感：几条暗绿弧线（近角处更密）
    for i, (ax, ay, rr) in enumerate([(-30, -20, 150), (W + 30, -30, 180),
                                      (-40, H + 40, 210), (W + 40, H + 30, 170)]):
        d.arc([ax - rr, ay - rr, ax + rr, ay + rr], 0, 360,
              fill=rgba(lerp(M3_GARDEN_TOP, (20, 48, 34), 90)), width=6 * ss)

    # ③ 木质棋盘面板（圆角 + 内圈亮边 + 木纹）
    x0, y0 = (M3_BX - 7) * ss, (M3_BY - 7) * ss
    x1, y1 = (M3_BX + M3_COLS * M3_CELL + 6) * ss, (M3_BY + M3_ROWS * M3_CELL + 6) * ss
    d.rounded_rectangle([x0, y0 + 3 * ss, x1, y1 + 4 * ss], radius=20 * ss,
                        fill=rgba(lerp(lerp(M3_GARDEN_TOP, M3_GARDEN_BOT, 170),
                                       (52, 34, 20), 120)))
    d.rounded_rectangle([x0, y0, x1, y1], radius=20 * ss, fill=rgba(M3_WOOD_DARK))
    d.rounded_rectangle([x0 + 3 * ss, y0 + 3 * ss, x1 - 3 * ss, y1 - 3 * ss],
                        radius=17 * ss, fill=rgba(M3_WOOD))
    for i in range(26):
        h = h32(0x7C11 + i * 331)
        yy = y0 + ((h >> 5) % (y1 - y0))
        d.line([x0 + 6 * ss, yy, x1 - 6 * ss, yy],
               fill=rgba(lerp(M3_WOOD, lerp(M3_WOOD, M3_WOOD_HI, 150), 150)),
               width=1 * ss)

    # ④ 8x8 糖果凹槽（与糖果图**共用** _draw_slot ⇒ 贴上去没有色差）
    for gy in range(M3_ROWS):
        for gx in range(M3_COLS):
            _draw_slot(d, ss, M3_BX + gx * M3_CELL, M3_BY + gy * M3_CELL, wood_edge=False)
    # 整屏底图必须**严格**不透明，否则 Canvas 的 memcpy 快路径判定失败（见 force_opaque）
    return force_opaque(im.resize((W_BG, H_BG), Image.LANCZOS), "match3/bg.png")


# ---------------------------------------------------------------- ⑦ 骰子（3D 立方体）
#
# 用户 2026-09-15 需求：「摇骰子的游戏，3 个骰子，要模拟 3D 效果和声音」，
# 参考产品 = App Store《简易骰子 - 朋友聚会摇色子模拟器》。
#
# 做法：**离线做真 3D 渲染**（正交投影 + 背面剔除 + 面法线光照），把"翻滚动画帧"
# 与"6 个静止面"烘成 PNG，运行时只做 1:1 贴图 —— 与本工程"不许拉伸/逐帧素材化"的
# 铁律一致（画布不做运行时缩放，所以立体感必须在**离线**算出来）。
#
# 为什么用**正交**投影（不是透视）：正交投影下立方的每个面投影是**平行四边形**，
# 于是"把方形点数纹理贴到该面上"是一条精确的**仿射**变换（PIL 的 Image.AFFINE 可以直接算），
# 不需要解 8 元单应矩阵；视觉上等轴方块也正是骰子游戏常见的观感。

DICE_L = 78                      # 立方体边长（像素，1:1）
DICE_SS = 4                      # 超采样倍数（棱边/点数圆靠它抗锯齿）
DICE_IMG = 152                   # 帧图边长（== 屏幕上的贴图尺寸）
DICE_ANCHOR = DICE_IMG // 2      # 锚点 = 图中心（= 立方体中心投影，**所有帧一致**）
DICE_ROLL_N = 12                 # 翻滚帧数：绕固定轴转满一圈 ⇒ 首尾相接可无缝循环
DICE_ROLL_STEP = 360.0 / DICE_ROLL_N

# (值, 外法线, 面内 u 轴, 面内 v 轴)；恒有 u × v == 法线
# 对面和 = 7 是标准骰子的约定：1-6 / 2-5 / 3-4
DICE_FACES = [
    (1, (0, 0, 1), (1, 0, 0), (0, 1, 0)),
    (2, (0, 1, 0), (1, 0, 0), (0, 0, -1)),
    (3, (1, 0, 0), (0, 0, -1), (0, 1, 0)),
    (4, (-1, 0, 0), (0, 0, 1), (0, 1, 0)),
    (5, (0, -1, 0), (1, 0, 0), (0, 0, 1)),
    (6, (0, 0, -1), (-1, 0, 0), (0, 1, 0)),
]

# 点数布局（u,v 归一化，v 向上；中式骰子 1 和 4 用红点 —— 参考产品也是红的）
DICE_PIPS = {
    1: [(0.50, 0.50)],
    2: [(0.30, 0.70), (0.70, 0.30)],
    3: [(0.26, 0.74), (0.50, 0.50), (0.74, 0.26)],
    4: [(0.30, 0.70), (0.70, 0.70), (0.30, 0.30), (0.70, 0.30)],
    5: [(0.28, 0.72), (0.72, 0.72), (0.50, 0.50), (0.28, 0.28), (0.72, 0.28)],
    6: [(0.32, 0.76), (0.68, 0.76), (0.32, 0.50), (0.68, 0.50),
        (0.32, 0.24), (0.68, 0.24)],
}

DICE_FACE_RGB = (250, 249, 246)
DICE_FACE_EDGE_RGB = (206, 202, 206)   # 倒角处的过渡色（比面稍暗）
DICE_PIP_DARK = (44, 40, 46)
DICE_PIP_RED = (198, 48, 44)
DICE_EDGE = (62, 50, 40)

# 世界坐标里的固定光源（左上上前）—— 面法线点乘它得到亮度，所以转动时明暗会跟着变
DICE_LIGHT = (-0.33, 0.80, 0.52)

DICE_TABLE_TOP = (40, 98, 70)    # 桌面（绿毡）上
DICE_TABLE_BOT = (20, 56, 44)    # 桌面下
DICE_BOARD_FILL = (28, 34, 44)   # 结果板
DICE_BOARD_EDGE = (104, 118, 140)
DICE_FRAME = (52, 36, 24)        # 木框

DICE_BOARD = (40, 320, 440, 455)  # 结果板矩形（左, 上, 右, 下）
DICE_CY = 190                     # 骰子中心 y（三颗横排，x 由 _dice_pos 算）


# ---- 3x3 矩阵小工具（纯 python，避免为一个 3D 骰子引 numpy）----
def _d3_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _d3_norm(a):
    n = math.sqrt(_d3_dot(a, a))
    return (a[0] / n, a[1] / n, a[2] / n) if n > 1e-9 else (0.0, 0.0, 0.0)


def _d3_apply(M, v):
    return (_d3_dot(M[0], v), _d3_dot(M[1], v), _d3_dot(M[2], v))


def _d3_mm(A, B):
    return tuple(tuple(sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3))
                 for i in range(3))


def _d3_eye():
    return ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _d3_rx(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return ((1.0, 0.0, 0.0), (0.0, c, -s), (0.0, s, c))


def _d3_ry(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return ((c, 0.0, s), (0.0, 1.0, 0.0), (-s, 0.0, c))


def _d3_rz(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return ((c, -s, 0.0), (s, c, 0.0), (0.0, 0.0, 1.0))


def _d3_axis_angle(axis, deg):
    """Rodrigues：绕任意轴转 deg 度（翻滚动画用 —— 让骰子看起来在乱滚）。"""
    x, y, z = _d3_norm(axis)
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    t = 1.0 - c
    return ((t * x * x + c, t * x * y - s * z, t * x * z + s * y),
            (t * x * y + s * z, t * y * y + c, t * y * z - s * x),
            (t * x * z - s * y, t * y * z + s * x, t * z * z + c))


def dice_view():
    """观察旋转：镜头在**右上前方**俯视 ⇒ 同时看到顶面/前面/右面（立体感的来源）。"""
    return _d3_mm(_d3_rx(25.0), _d3_ry(-35.0))


def dice_local_up(val):
    """把"值为 val 的面"转到 +Y（朝上）的 **90° 步进**旋转。

    刻意只用 90° 的整数倍：这样点数方块始终与骰子棱平行（像真骰子），
    任意角度会把点数转成歪的。
    """
    return {
        2: _d3_eye(),
        5: _d3_rz(180.0),
        1: _d3_rx(-90.0),
        6: _d3_rx(90.0),
        3: _d3_rz(90.0),
        4: _d3_rz(-90.0),
    }[val]


_DICE_TEX_CACHE = {}


def dice_face_tex(val, ts):
    """单个面的**正方形点数纹理**（不透明 RGB）—— 会被仿射变换贴到投影后的面上。"""
    key = (val, ts)
    if key in _DICE_TEX_CACHE:
        return _DICE_TEX_CACHE[key]
    im = Image.new("RGB", (ts, ts), DICE_FACE_RGB)
    d = ImageDraw.Draw(im)
    r = ts * 0.112
    for (u, v) in DICE_PIPS[val]:
        cx, cy = u * ts, (1.0 - v) * ts        # v 向上 ⇒ 屏幕 y 要翻
        col = DICE_PIP_RED if val in (1, 4) else DICE_PIP_DARK
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=col)

    # 倒角：四周一圈**渐变压暗**（模糊的内缩圆角矩形当 mask）。
    # 没有它，三个面拼起来就是"三块平板"，看着像魔方而不是骰子；
    # 有了它，棱线附近有明暗过渡，观感接近实体骰子的圆角棱。
    edge = Image.new("L", (ts, ts), 0)
    m = ts * 0.075
    ImageDraw.Draw(edge).rounded_rectangle([m, m, ts - m, ts - m],
                                           radius=ts * 0.10, fill=255)
    edge = edge.filter(ImageFilter.GaussianBlur(ts * 0.05))
    im = Image.composite(im, Image.new("RGB", (ts, ts), DICE_FACE_EDGE_RGB), edge)
    _DICE_TEX_CACHE[key] = im
    return im


def dice_quad(R, val, S):
    """某个面投影到屏幕后的四角（输出像素空间，y 向下）+ 亮度系数 k。

    ★ 抽成独立函数是**为了自检**（见 check_dice_face）：只有拿得到"顶面到底落在哪几个像素上"，
    才能验证"顶面画的点数 == 面值"这件容易静默出错的事（视角一改、u/v 轴一写反，
    渲染出来照样是"一个挺像骰子的东西"，只是点数错了）。
    """
    cen = S / 2.0
    half = S * DICE_L / (2.0 * DICE_IMG)     # S 与 DICE_IMG 成比例，所以这里换算出像素半径
    for _v, n, u, v in DICE_FACES:
        if _v != val:
            continue
        n2 = _d3_apply(R, n)

        def scr(s, t):
            a = (2.0 * s - 1.0) * half
            b = (2.0 * t - 1.0) * half
            p = (u[0] * a + v[0] * b + n[0] * half,
                 u[1] * a + v[1] * b + n[1] * half,
                 u[2] * a + v[2] * b + n[2] * half)
            w = _d3_apply(R, p)
            return (cen + w[0], cen - w[1])

        k = 0.50 + 0.50 * max(0.0, _d3_dot(n2, DICE_LIGHT))
        return [scr(0, 0), scr(1, 0), scr(1, 1), scr(0, 1)], n2[2], k
    return None, 0.0, 0.0


def dice_visible_count(R):
    """当前朝向下**可见（面向观察者）的面数**。等轴视角下静态帧恒为 3。"""
    return sum(1 for _v, n, _u, _w in DICE_FACES if _d3_apply(R, n)[2] > 1e-4)


def check_dice_face(im, val, R):
    """自检：**朝上的那个面，画出来的点数必须正好等于面值**。

    做法：把"顶面"投影四边形内缩后取出来，数里面的深色连通块（= 点数）。
    为什么值得写：面值画错/点数布局表写反，肉眼扫一眼"还挺像骰子"，
    真机上却会出现"摇出 3 却显示 4 个点"——这属于必须被机器抓住的静默故障。
    """
    q, nz, k = dice_quad(R, val, DICE_IMG)
    if q is None:
        print("  !! 面值 %d 不在 DICE_FACES 表里" % val)
        return 1
    if nz <= 0:
        print("  !! face%d 该面朝上的帧里它不可见（视角错？）" % val)
        return 1
    # 四边形内缩（避开棱边描边）：沿形心方向收 6px
    cx = sum(p[0] for p in q) / 4.0
    cy = sum(p[1] for p in q) / 4.0
    inset = []
    for (x, y) in q:
        dx, dy = x - cx, y - cy
        dd = math.hypot(dx, dy) or 1.0
        inset.append((x - dx / dd * 6.0, y - dy / dd * 6.0))
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).polygon(inset, fill=255)
    mp = mask.load()
    ip = im.convert("RGBA").load()
    seen = set()
    blobs = 0
    W, H = im.size
    for y in range(H):
        for x in range(W):
            if mp[x, y] < 128 or (x, y) in seen:
                continue
            r, g, b, a = ip[x, y]
            if a < 200:
                continue
            pip = (r < 150 and g < 150 and b < 160) or (r > 140 and g < 110)
            if not pip:
                continue
            n = 0
            stack = [(x, y)]
            seen.add((x, y))
            while stack:
                px_, py_ = stack.pop()
                n += 1
                for nx, ny in ((px_ + 1, py_), (px_ - 1, py_), (px_, py_ + 1), (px_, py_ - 1)):
                    if 0 <= nx < W and 0 <= ny < H and mp[nx, ny] >= 128 and (nx, ny) not in seen:
                        rr, gg, bb, aa = ip[nx, ny]
                        p2 = (rr < 150 and gg < 150 and bb < 160) or (rr > 140 and gg < 110)
                        if aa >= 200 and p2:
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            if n >= 6:          # 抗锯齿碎屑不算点数
                blobs += 1
    ok = (blobs == val)
    print("  face%-2d 顶面 亮度%.2f 内缩后点数 %d（应 %d）%s"
          % (val, k, blobs, val, "✓" if ok else "★ 点数和面值不符！"))
    return 0 if ok else 1


def dice_frame(R):
    """渲染一帧立方体（R = 3x3 世界旋转），返回 DICE_IMG x DICE_IMG 的 RGBA 图。

    流程：背面剔除（法线 z<=0）→ 按面质心深度排序 → 每面用 **AFFINE 逆映射**
    把方格纹理贴到投影后的平行四边形 → 描棱边。
    """
    S = DICE_IMG * DICE_SS
    out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ts = int(round(DICE_L * DICE_SS))
    vis = []

    for val, n, u, v in DICE_FACES:
        n2 = _d3_apply(R, n)
        if n2[2] <= 1e-4:
            continue                      # 背面（观察者在 +z 无穷远）
        q, nz, k = dice_quad(R, val, S)    # 几何只有一处实现（单一真值）
        vis.append((nz, val, n2, q, k))

    # 远的先画（凸多面体的可见面互不遮挡，排序只为万无一失）
    vis.sort(key=lambda f: f[0])

    for _zc, val, n2, (p00, p10, p11, p01), k in vis:
        du = (p10[0] - p00[0], p10[1] - p00[1])
        dv = (p01[0] - p00[0], p01[1] - p00[1])
        det = du[0] * dv[1] - dv[0] * du[1]
        if abs(det) < 1e-6:
            continue
        ia, ib = dv[1] / det, -dv[0] / det          # A^-1 第一行
        ic, idd = -du[1] / det, du[0] / det         # A^-1 第二行
        # PIL AFFINE 的 data 语义：input_x = a*out_x + b*out_y + c（即"输出 -> 输入"）
        ca, cb = ts * ia, ts * ib
        cc = -ts * (ia * p00[0] + ib * p00[1])
        cd, ce = ts * ic, ts * idd
        cf = -ts * (ic * p00[0] + idd * p00[1])

        tex = ImageEnhance.Brightness(dice_face_tex(val, ts)).enhance(k)
        layer = tex.transform((S, S), Image.AFFINE, (ca, cb, cc, cd, ce, cf),
                              resample=Image.BILINEAR)
        mask = Image.new("L", (S, S), 0)
        ImageDraw.Draw(mask).polygon([p00, p01, p11, p10], fill=255)
        out.paste(layer, (0, 0), mask)
        ImageDraw.Draw(out).line([p00, p01, p11, p10, p00], fill=rgba(DICE_EDGE),
                                 width=max(1, int(DICE_SS * 1.15)))

    return out.resize((DICE_IMG, DICE_IMG), Image.LANCZOS)


def bake_dice_faces():
    """6 个静止帧：值为 v 的面朝上（落定后展示的就是它）。"""
    V = dice_view()
    out = []
    for v in (1, 2, 3, 4, 5, 6):
        R = _d3_mm(V, dice_local_up(v))
        out.append(dice_frame(R))
    return out


def bake_dice_roll():
    """翻滚帧序列：绕一根斜轴匀速转满一圈（12 帧）⇒ 可无缝循环播放。"""
    V = dice_view()
    axis = (1.0, 1.0, 0.35)
    return [dice_frame(_d3_mm(V, _d3_axis_angle(axis, i * DICE_ROLL_STEP)))
            for i in range(DICE_ROLL_N)]


def bake_dice_shadow():
    """骰子底部的椭圆接触阴影（半透明 —— 走逐像素 alpha 混合，别指望快路径）。"""
    W, H = 116, 40
    S2 = 3
    m = Image.new("L", (W * S2, H * S2), 0)
    px = m.load()
    cx, cy = W * S2 / 2.0, H * S2 / 2.0
    rx, ry = (W * S2 / 2.0 - S2), (H * S2 / 2.0 - S2)
    for y in range(H * S2):
        for x in range(W * S2):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            r = math.sqrt(dx * dx + dy * dy)
            if r < 1.0:
                px[x, y] = int(168 * (1.0 - r) ** 1.4)
    out = Image.new("RGBA", (W, H), (8, 20, 14, 0))
    out.putalpha(m.resize((W, H), Image.LANCZOS))
    return out


def bake_dice_bg():
    """整屏桌面（绿毡 + 木框 + 暗角）+ 下方的结果板。**静态**，每帧只贴一次。

    结果板是**空的**：和值/评语由运行时用画布点阵字写上去（文字每帧在变）。
    """
    ss = 2
    W, H = W_BG * ss, H_BG * ss
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)

    # ① 竖渐变毡面
    band = 3 * ss
    for y in range(0, H, band):
        col = lerp(DICE_TABLE_TOP, DICE_TABLE_BOT, y * 256 // H)
        d.rectangle([0, y, W, min(y + band - 1, H - 1)], fill=col)

    # ② 绒面颗粒（小尺寸噪声放大 ⇒ 平滑的毛毡感，不是刺点）
    nw, nh = W // 8, H // 8
    nse = Image.new("RGB", (nw, nh))
    np_ = nse.load()
    for y in range(nh):
        for x in range(nw):
            v = 104 + (h32(0x51CE + x * 7919 + y * 104729) % 48)
            np_[x, y] = (v, v, v)
    im = Image.blend(im, nse.resize((W, H), Image.BILINEAR), 0.09)

    # ③ 中心受光 + 四角压暗（暗角）
    vw, vh = 64, 72
    vig = Image.new("L", (vw, vh))
    vp = vig.load()
    for y in range(vh):
        for x in range(vw):
            dx = (x / (vw - 1.0) - 0.5) * 2.0
            dy = (y / (vh - 1.0) - 0.5) * 2.0
            r = min(1.0, math.sqrt(dx * dx * 0.90 + dy * dy * 0.55))
            vp[x, y] = int(150 * (r ** 2.4))
    dark = Image.new("RGB", (W, H), (6, 16, 12))
    im = Image.composite(dark, im, vig.resize((W, H), Image.BILINEAR)).convert("RGBA")
    d = ImageDraw.Draw(im)

    # ④ 木框（外深内亮两道）
    d.rectangle([0, 0, W - 1, H - 1], outline=DICE_FRAME, width=13 * ss)
    d.rectangle([13 * ss, 13 * ss, W - 13 * ss, H - 13 * ss],
                outline=(96, 70, 44), width=2 * ss)

    # ⑤ 结果板（圆角 + 亮描边 + 内圈暗线）
    #    ⚠️ 描边必须够亮够粗：板色(深蓝灰)与暗角背景天生接近，
    #       第一版 2px 的 (104,118,140) 缩到整屏看几乎分不出边界。
    bx0, by0, bx1, by1 = DICE_BOARD
    d.rounded_rectangle([bx0 * ss, by0 * ss, bx1 * ss, by1 * ss], radius=18 * ss,
                        fill=(22, 28, 38), outline=(126, 142, 168), width=3 * ss)
    d.rounded_rectangle([(bx0 + 6) * ss, (by0 + 6) * ss, (bx1 - 6) * ss, (by1 - 6) * ss],
                        radius=13 * ss, outline=(52, 62, 80), width=ss)

    return force_opaque(im.resize((W_BG, H_BG), Image.LANCZOS), "dice/bg.png")


# ---------------------------------------------------------------- ⑥ 格式探针

def bake_probe():
    """格式探针：8x1 的已知颜色，用来确认设备端解码出来的**字节序 / 是否预乘 alpha**。

    像素（PNG 里存的）：
      0 不透明红      (255,0,0,255)
      1 不透明绿      (0,255,0,255)
      2 不透明蓝      (0,0,255,255)
      3 半透明白      (255,255,255,128)
      4 全透明        (0,0,0,0)
      5 25% 红        (255,0,0,64)
      6 不透明中灰    (128,128,128,255)
      7 全透明带 RGB  (200,100,50,0)

    判读（QA `spr images/game/_probe.png`）：
      · px0 应是 A=255 R=255 G=0 B=0 ⇒ 布局为 **0xAARRGGBB**（= BGRA 内存序），与画布一致
      · px3 若 R=G=B=255 A=128 ⇒ **非预乘**（正确，可直接 alpha 混合）
             若 R=G=B=128 A=128 ⇒ 预乘 ⇒ 混合会偏暗，素材要另作处理
      · px4/px7 若 A=0 ⇒ 解码器保留 alpha（好）
    """
    px = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 128),
          (0, 0, 0, 0), (255, 0, 0, 64), (128, 128, 128, 255), (200, 100, 50, 0)]
    im = Image.new("RGBA", (len(px), 1))
    for i, c in enumerate(px):
        im.putpixel((i, 0), c)
    return im


# ---------------------------------------------------------------- 输出

def force_opaque(im, name="", warn=True):
    """把整图 alpha 强制为 255（只用于**设计上就该铺满**的素材）。

    ★★ 为什么必须有这一步（2026-09-15 消消乐实测）：

    `Canvas` 的 memcpy 快路径要求 **全部** alpha == 0xFF（见 core/PgSprite.cpp
    加载时算的 `Sprite::opaque`）。一张"看着完全不透明"的图只要有**一个** alpha<255
    的像素，就判定失败 ⇒ 每帧老老实实逐像素混合（消消乐第一版：render 17.7ms / fps 33）。

    那 alpha<255 从哪来？—— **`ImageDraw` 的半透明 `fill`**：
    `d.ellipse(..., fill=rgba(色, 40))` 是**直接覆写**像素（PIL 的 draw 不做 alpha 合成），
    于是图上就出现了 alpha=40 的像素（实测一块半透明椭圆缩回后 **112240** 个）。

    ⚠️ **不要把它归罪于 `resize(LANCZOS)`**（2026-09-15 做过对照实验）：
    整图全不透明的地图缩回后 alpha<255 的像素是 **0**（LANCZOS 不会凭空造出半透明）。

    ⚠️ 所以本函数是**兜底**而不是主修法：主修法是把 `rgba(色, a)` 改写成
    `rgba(lerp(底色, 叠加色, a))`（视觉等价的不透明近似色）。否则本函数把那个 alpha
    抹成 255 之后，半透明叠加会变成**实心色块** —— 那是个**视觉 bug**，比性能问题更难查。
    因此这里在强制之前先**告警**：告诉调用者"你这张图原本有半透明像素"。
    """
    if warn:
        n = sum(im.split()[3].histogram()[:255])
        if n:
            print("  ⚠️ %s 强制不透明前有 %d 个 alpha<255 的像素 —— 多半是用了 "
                  "rgba(色, alpha) 的 fill（ImageDraw 直接覆写、不混合）。"
                  "抹平后视觉上会变成实心色块，应改成 lerp(底色, 叠加色, alpha)。"
                  % (name or "素材", n))
    im.putalpha(Image.new("L", im.size, 255))
    return im


def save(im, rel):
    path = os.path.join(IMG_ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.save(path)
    return path, os.path.getsize(path)


def check(im, name, anchor, opaque_full=False):
    """自检：① 有内容 ② 内容不贴边（贴边 = 已被裁）。

    opaque_full=True 用于**故意铺满整张图**的素材（消消乐糖果：含凹槽底 ⇒ 整图不透明，
    贴边是设计而不是"被裁"）。这类只查①。
    """
    a = im.split()[3]
    ink = sum(a.histogram()[25:])
    w, h = im.size
    px = a.load()
    edge = 0
    for x in range(w):
        edge += px[x, 0] > 8
        edge += px[x, h - 1] > 8
    for y in range(h):
        edge += px[0, y] > 8
        edge += px[w - 1, y] > 8
    bad = (ink < 100) or (edge > 0 and not opaque_full)
    print("  %-22s %3dx%-3d 墨迹 %6d px (%4.1f%%)  锚点 %-11s 贴边像素 %-3s %s"
          % (name, w, h, ink, 100.0 * ink / (w * h), str(anchor),
             edge, "（整图不透明·预期）✓" if opaque_full and edge else
                   ("★ 有内容贴边！" if edge else "✓")))
    return bad


def main():
    print("生成游戏素材 → resources/images/game/")
    items = []      # (游戏, 变量名, 相对路径, w, h, ax, ay, 说明)
    bad = 0

    # ---- 探针 ----
    p, n = save(bake_probe(), "_probe.png")
    print("  探针 %s（%d 字节）" % (os.path.basename(p), n))

    # ---- 打地鼠 ----
    G = "whack"
    print("[whack]")

    hole = bake_hole(False)
    save(hole, "%s/hole.png" % G)
    bad += check(hole, "hole", (AX_HOLE, AY_HOLE))

    im = bake_bg_with_holes(hole)
    save(im, "%s/bg.png" % G)
    items.append(("whack", "kWhackBg", "%s/bg.png" % G, W_BG, H_BG, 0, 0,
                  "草地 + 9 个洞口（静态整屏，合成版）"))

    im = bake_hole(True)
    save(im, "%s/hole_front.png" % G)
    items.append(("whack", "kWhackHoleFront", "%s/hole_front.png" % G, W_HOLE, H_HOLE,
                  AX_HOLE, AY_HOLE, "洞口前沿（只保留下半，用来盖住地鼠下半身）"))

    im = bake_mole(False)
    save(im, "%s/mole.png" % G)
    bad += check(im, "mole", (AX_MOLE, AY_MOLE))
    items.append(("whack", "kWhackMole", "%s/mole.png" % G, W_MOLE, H_MOLE,
                  AX_MOLE, AY_MOLE, "地鼠（锚点=圆心）"))

    im = bake_mole(True)
    save(im, "%s/mole_hit.png" % G)
    items.append(("whack", "kWhackMoleHit", "%s/mole_hit.png" % G, W_MOLE, H_MOLE,
                  AX_MOLE, AY_MOLE, "被击中的地鼠（亮）"))

    # 锤子：复用既有生成器（三帧绕握把旋转 + 每帧自己的锚点）
    frames = whack_hammer.make_frames()
    for name, fr, ang, anchor in frames:
        save(fr, "%s/hammer_%s.png" % (G, name))
        print("  hammer_%-15s %3dx%-3d 锚点 %s（旋转 %.0f°）"
              % (name + ".png", whack_hammer.W, whack_hammer.H, anchor, ang))
        items.append(("whack", "kWhackHammer" + name.capitalize(),
                      "%s/hammer_%s.png" % (G, name), whack_hammer.W, whack_hammer.H,
                      anchor[0], anchor[1], "锤子 %s 帧（**锚点=锤头中心**：贴图时锤头落在触点上）" % name))

    for hot in (False, True):
        im = bake_rays(hot)
        nm = "rays_hot" if hot else "rays"
        save(im, "%s/%s.png" % (G, nm))
        items.append(("whack", "kWhack" + ("RaysHot" if hot else "Rays"),
                      "%s/%s.png" % (G, nm), W_RAYS, W_RAYS, AX_RAYS, AY_RAYS,
                      "连击放射线（%s）" % ("高阶" if hot else "普通")))

    # ---- 贪吃蛇：静态场地 ----
    print("[snake]")
    im = bake_snake_bg()
    save(im, "snake/bg.png")
    items.append(("snake", "kSnakeBg", "snake/bg.png", W_BG, H_BG, 0, 0,
                  "场地底图（底色+外框+场地+细网格，静态整屏）"))

    # ---- 小鸟：静态天空与地面 ----
    print("[flappy]")
    im = bake_flappy_bg()
    save(im, "flappy/bg.png")
    items.append(("flappy", "kFlappyBg", "flappy/bg.png", W_BG, H_BG, 0, 0,
                  "天空渐变+地面（静态整屏；云与滚动条纹仍是代码绘制）"))

    # ---- 消消乐（糖果三消）----
    # ⚠️ 这里刻意用 G3 而不是 G：后面 whack 的拼图预览段还在用 G（历史代码），
    #    复用同一个变量名会让那段去 match3/ 目录找 hole.png（踩过）。
    G3 = "match3"
    print("[match3]")

    im = bake_m3_bg()
    save(im, "%s/bg.png" % G3)
    items.append(("match3", "kMatch3Bg", "%s/bg.png" % G3, W_BG, H_BG, 0, 0,
                  "花园渐变 + 木质棋盘 + 8x8 浅色糖果凹槽（静态整屏，每帧只贴一次）"))

    for i, (col, kind) in enumerate(M3_CANDY):
        im = bake_candy(i)
        save(im, "%s/candy%d.png" % (G3, i))
        # 整图不透明（含凹槽底）是**故意的**：走 memcpy 快路径，见 bake_candy 的说明
        bad += check(im, "candy%d(%s)" % (i, kind), (M3_CELL // 2, M3_CELL // 2),
                     opaque_full=True)
        items.append(("match3", "kMatch3Candy%d" % i, "%s/candy%d.png" % (G3, i),
                      M3_CELL, M3_CELL, 0, 0,
                      "糖果 %d：%s（**含凹槽底的不透明整图** ⇒ memcpy 快路径；"
                      "锚点=左上角，图尺寸 == 格尺寸）" % (i, kind)))

    im = bake_m3_sel()
    save(im, "%s/sel.png" % G3)
    bad += check(im, "sel", (M3_CELL // 2, M3_CELL // 2))
    items.append(("match3", "kMatch3Sel", "%s/sel.png" % G3, M3_CELL, M3_CELL, 0, 0,
                  "选中高亮框（金色圆角框 + 四角加粗）"))

    im = bake_m3_burst()
    save(im, "%s/burst.png" % G3)
    bad += check(im, "burst", (M3_BURST // 2, M3_BURST // 2))
    items.append(("match3", "kMatch3Burst", "%s/burst.png" % G3, M3_BURST, M3_BURST,
                  M3_BURST // 2, M3_BURST // 2, "消除爆花（锚点=中心；靠整体 alpha 淡出）"))

    im = bake_m3_rainbow()
    save(im, "%s/rainbow.png" % G3)
    bad += check(im, "rainbow", (M3_CELL // 2, M3_CELL // 2), opaque_full=True)
    items.append(("match3", "kMatch3Rainbow", "%s/rainbow.png" % G3, M3_CELL, M3_CELL, 0, 0,
                  "彩虹球（5 连生成；**含凹槽底的不透明整图** ⇒ memcpy 快路径）"))

    for horiz, nm in ((True, "h"), (False, "v")):
        im = bake_m3_mark(horiz)
        save(im, "%s/mark_%s.png" % (G3, nm))
        bad += check(im, "mark_%s" % nm, (M3_CELL // 2, M3_CELL // 2))
        items.append(("match3", "kMatch3Mark" + nm.upper(), "%s/mark_%s.png" % (G3, nm),
                      M3_CELL, M3_CELL, 0, 0,
                      "%s向炸弹的**方向标记**（叠在糖果上；只烘 2 张 —— 标记是中性色，"
                      "不必按糖果色铺 14 张）" % ("横" if horiz else "纵")))

    # match3 预览拼图（人看；也是"素材有没有画出来"的判据）
    sheet = ["%s/candy%d.png" % (G3, i) for i in range(len(M3_CANDY))]
    sheet += ["%s/sel.png" % G3, "%s/burst.png" % G3, "%s/rainbow.png" % G3,
              "%s/mark_h.png" % G3, "%s/mark_v.png" % G3]
    cell, pad = 150, 8
    cols = 5
    rows = (len(sheet) + cols - 1) // cols
    sh = Image.new("RGB", (cols * (cell + pad) + pad, rows * (cell + pad) + pad), (30, 40, 34))
    for i, rel in enumerate(sheet):
        s = Image.open(os.path.join(IMG_ROOT, rel)).convert("RGBA")
        s.thumbnail((cell, cell), Image.LANCZOS)
        bg2 = Image.new("RGBA", (cell, cell), (60, 60, 66, 255))
        bg2.paste(s, ((cell - s.width) // 2, (cell - s.height) // 2), s)
        sh.paste(bg2.convert("RGB"), (pad + (i % cols) * (cell + pad),
                                      pad + (i // cols) * (cell + pad)))
    out_png = os.path.join(ROOT, "docs", "shot_game_art_match3.png")
    sh.save(out_png)
    print("写出 %s（拼图预览）" % os.path.relpath(out_png, ROOT))

    # ---- 摇骰子（3D 立方体：6 个静止面 + 12 帧翻滚 + 桌面 + 接触阴影）----
    GD = "dice"
    print("[dice]")

    im = bake_dice_bg()
    save(im, "%s/bg.png" % GD)
    items.append(("dice", "kDiceBg", "%s/bg.png" % GD, W_BG, H_BG, 0, 0,
                  "绿毡桌面 + 木框 + 暗角 + 结果板（静态整屏，每帧只贴一次；"
                  "板上的和值/评语由运行时写字）"))

    faces = bake_dice_faces()
    DV = dice_view()
    for i, im in enumerate(faces):
        save(im, "%s/face%d.png" % (GD, i + 1))
        rl = _d3_mm(DV, dice_local_up(i + 1))
        bad += check(im, "face%d" % (i + 1), (DICE_ANCHOR, DICE_ANCHOR))
        bad += check_dice_face(im, i + 1, rl)      # ★ 顶面点数必须 == 面值
        items.append(("dice", "kDiceFace%d" % (i + 1), "%s/face%d.png" % (GD, i + 1),
                      DICE_IMG, DICE_IMG, DICE_ANCHOR, DICE_ANCHOR,
                      "静止帧：值为 %d 的面朝上（锚点=图中心=立方体中心投影）" % (i + 1)))

    rolls = bake_dice_roll()
    axis = (1.0, 1.0, 0.35)
    for i, im in enumerate(rolls):
        save(im, "%s/roll%d.png" % (GD, i))
        rl = _d3_mm(DV, _d3_axis_angle(axis, i * DICE_ROLL_STEP))
        nv = dice_visible_count(rl)
        if not (2 <= nv <= 3):
            print("  !! roll%d 可见面数 %d（应 2~3）—— 视角/轴算错了" % (i, nv))
            bad += 1
        # 翻滚帧**不做点数自检**：这一帧里"朝上的面"是斜的、点数会相互贴边，
        # 连通块计数不具备判据效力（会误报）。翻滚帧的观感由真机抓屏判。
        bad += check(im, "roll%d" % i, (DICE_ANCHOR, DICE_ANCHOR))
        items.append(("dice", "kDiceRoll%d" % i, "%s/roll%d.png" % (GD, i),
                      DICE_IMG, DICE_IMG, DICE_ANCHOR, DICE_ANCHOR,
                      "翻滚第 %d 帧（绕斜轴转 %.0f°；%d 帧转满一圈 ⇒ 可无缝循环）"
                      % (i, i * DICE_ROLL_STEP, DICE_ROLL_N)))

    im = bake_dice_shadow()
    save(im, "%s/shadow.png" % GD)
    items.append(("dice", "kDiceShadow", "%s/shadow.png" % GD, 116, 40, 58, 20,
                  "接触阴影（半透明椭圆；贴图走逐像素 alpha 混合）"))

    # dice 预览拼图（人看；也是"3D 是否真的渲出来"的判据）
    sheet = ["%s/bg.png" % GD] + ["%s/face%d.png" % (GD, v) for v in (1, 2, 3, 4, 5, 6)]
    sheet += ["%s/roll%d.png" % (GD, i) for i in (0, 3, 6, 9)]
    cell, pad = 200, 8
    cols = 6
    rows = (len(sheet) + cols - 1) // cols
    sh = Image.new("RGB", (cols * (cell + pad) + pad, rows * (cell + pad) + pad),
                   (18, 30, 24))
    for i, rel in enumerate(sheet):
        s = Image.open(os.path.join(IMG_ROOT, rel)).convert("RGBA")
        s.thumbnail((cell, cell), Image.LANCZOS)
        b2 = Image.new("RGBA", (cell, cell), (52, 54, 60, 255))
        b2.paste(s, ((cell - s.width) // 2, (cell - s.height) // 2), s)
        sh.paste(b2.convert("RGB"), (pad + (i % cols) * (cell + pad),
                                    pad + (i // cols) * (cell + pad)))
    out_png = os.path.join(ROOT, "docs", "shot_game_art_dice.png")
    sh.save(out_png)
    print("写出 %s（拼图预览）" % os.path.relpath(out_png, ROOT))

    # ---- 数独 / 节奏钢琴 / 打鼓（木纹实体乐器风；实现在 tools/gen_instr_art.py）----
    print("[sudoku/piano/drum]")
    bad += instr.bake_all(save, check, items)
    if instr.PREVIEW_PATHS:
        cell, pad = 200, 8
        cols = 6
        rows = (len(instr.PREVIEW_PATHS) + cols - 1) // cols
        sh = Image.new("RGB", (cols * (cell + pad) + pad, rows * (cell + pad) + pad),
                       (28, 20, 12))
        for i, rel in enumerate(instr.PREVIEW_PATHS):
            s2 = Image.open(os.path.join(IMG_ROOT, rel)).convert("RGBA")
            s2.thumbnail((cell, cell), Image.BOX)
            b3 = Image.new("RGBA", (cell, cell), (60, 42, 26, 255))
            b3.paste(s2, ((cell - s2.width) // 2, (cell - s2.height) // 2), s2)
            sh.paste(b3.convert("RGB"), (pad + (i % cols) * (cell + pad),
                                        pad + (i // cols) * (cell + pad)))
        out_png = os.path.join(ROOT, "docs", "shot_game_art_instr.png")
        sh.save(out_png)
        print("写出 %s（拼图预览）" % os.path.relpath(out_png, ROOT))

    # ---- 清单头（只有路径/尺寸/锚点，几 KB；像素全在 PNG 里）----
    L = []
    A = L.append
    A("/*")
    A(" * PgGameArt.h - 游戏素材**清单**（由 tools/gen_game_art.py 生成，勿手改）")
    A(" *")
    A(" * 这里只有「路径 + 尺寸 + 锚点」，**没有像素数据** —— 像素在")
    A(" * resources/images/game/<游戏>/*.png 里，运行时由 pg::sprites().get()")
    A(" * 解码加载（见 core/PgSprite.h）。所以：**改图只要覆盖 PNG**（尺寸不变时")
    A(" * 连这个头都不用重生成），不用重编、不用重新刷机。")
    A(" *")
    A(" * 用法：")
    A(" *   const Canvas::Sprite *s = sprites().get(kWhackMole.path);")
    A(" *   if (s) c.drawSprite(*s, px - kWhackMole.ax, py - kWhackMole.ay);")
    A(" *        // 锚点 = 图内的参考点（洞中心 / 地鼠圆心 / 锤子握把…）")
    A(" *")
    A(" * ⚠️ 尺寸必须与素材严格一致（Canvas 是 1:1 贴图，不做缩放）——")
    A(" *    check() 里对不上会打日志，QA `spr` 也能看到实际解出来的尺寸。")
    A(" */")
    A("#ifndef PG_GAME_ART_H_")
    A("#define PG_GAME_ART_H_")
    A("")
    A("namespace pg {")
    A("namespace gameart {")
    A("")
    A("struct Def {")
    A("  const char *path;   // 资源相对路径")
    A("  int w, h;           // 素材尺寸（== 屏幕上贴的尺寸，1:1）")
    A("  int ax, ay;         // 锚点：图内的参考点坐标")
    A("};")
    A("")
    cur = None
    for game, var, rel, w, h, ax, ay, desc in items:
        if game != cur:
            A("// ---- %s ----" % game)
            cur = game
        A("// %s" % desc)
        A("const Def %s = {\"images/game/%s\", %d, %d, %d, %d};" % (var, rel, w, h, ax, ay))
    A("")
    A("// ==================== 布局常量（数独 / 节奏钢琴 / 打鼓）====================")
    A("// 由 tools/gen_instr_art.py 的常量表生成 —— **素材尺寸与布局必须同源**，")
    A("// 否则就会出现「图上画在一处、代码判在另一处」（缩进 1px 的错位肉眼看不出来）。")
    import gen_instr_art as _gi
    _s, _p, _d = _gi.SUDOKU, _gi.PIANO, _gi.DRUM
    A("// ---- 数独：9x9 棋盘 + 数字键行 + 动作键行 ----")
    A("const int kSudokuCell = %d;" % _s["cell"])
    A("const int kSudokuBX = %d, kSudokuBY = %d;" % (_s["bx"], _s["by"]))
    A("const int kSudokuNumX = %d, kSudokuNumY = %d, kSudokuNumW = %d, kSudokuNumH = %d,"
      % (_s["num_x"], _s["num_y"], _s["num_w"], _s["num_h"]))
    A("    kSudokuNumGap = %d;" % _s["num_gap"])
    A("const int kSudokuActX = %d, kSudokuActY = %d, kSudokuActW = %d, kSudokuActH = %d,"
      % (_s["act_x"], _s["act_y"], _s["act_w"], _s["act_h"]))
    A("    kSudokuActGap = %d, kSudokuActN = %d;" % (_s["act_gap"], _s.get("act_n", 5)))
    A("// ---- 节奏钢琴：8 键（键宽 = 画布宽 / 8）----")
    A("const int kPianoKeyW = %d, kPianoKeyH = %d, kPianoKeyY = %d;"
      % (_p["key_w"], _p["key_h"], _p["key_y"]))
    A("const int kPianoJudgeY = %d;" % _p["judge_y"])
    A("const int kPianoNoteW = %d, kPianoNoteH = %d;" % (_p["note_w"], _p["note_h"]))
    A("// ---- 打鼓：6 鼓位（**单排，各自正对上方那条掉落轨**）+ 6 条掉落轨 ----")
    A("const int kDrumPadW = %d, kDrumPadH = %d;" % (_d["pad_w"], _d["pad_h"]))
    A("const int kDrumPadX0 = %d, kDrumPadDX = %d;" % (_d["pad_x0"], _d["pad_dx"]))
    A("const int kDrumPadY0 = %d;" % _d["pad_y0"])
    A("const int kDrumNoteW = %d, kDrumNoteH = %d;" % (_d["note_w"], _d["note_h"]))
    A("const int kDrumLaneW = %d, kDrumFallH = %d;" % (_d["lane_w"], _d["fall_h"]))
    A("")
    A("}  // namespace gameart")
    A("}  // namespace pg")
    A("")
    A("#endif  // PG_GAME_ART_H_")
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("写出 %s (%d 张素材)" % (os.path.relpath(OUT_H, ROOT), len(items)))

    # ---- 拼图预览（人看）----
    sheet_paths = ["%s/bg.png" % G, "%s/hole.png" % G, "%s/hole_front.png" % G,
                   "%s/mole.png" % G, "%s/mole_hit.png" % G,
                   "%s/rays.png" % G, "%s/rays_hot.png" % G]
    sheet_paths += ["%s/hammer_%s.png" % (G, n) for n, _, _, _ in frames]
    cell, pad = 180, 8
    cols = 5
    rows = (len(sheet_paths) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (cell + pad) + pad, rows * (cell + pad) + pad),
                      (26, 32, 28))
    for i, rel in enumerate(sheet_paths):
        im = Image.open(os.path.join(IMG_ROOT, rel)).convert("RGBA")
        im.thumbnail((cell, cell), Image.LANCZOS)
        bg = Image.new("RGBA", (cell, cell), (60, 60, 66, 255))
        bg.paste(im, ((cell - im.width) // 2, (cell - im.height) // 2), im)
        sheet.paste(bg.convert("RGB"),
                    (pad + (i % cols) * (cell + pad), pad + (i // cols) * (cell + pad)))
    out_png = os.path.join(ROOT, "docs", "shot_game_art_whack.png")
    sheet.save(out_png)
    print("写出 %s（拼图预览）" % os.path.relpath(out_png, ROOT))

    print("自检：%s" % ("通过 ✓" if not bad else "失败 ✗（%d 张贴边/空图）" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
