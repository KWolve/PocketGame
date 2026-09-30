#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_instr_art.py —— 三款「木纹实体乐器风」游戏的 PNG 素材 + 几何常量

**不单独运行**，由 `tools/gen_game_art.py` 的 main() 调用（见那里的 `instr.bake_all(...)`），
复用它的 `save()` / `check()` / 清单头拼装，产出一份 `src/core/PgInstrArt.h`。

产物
----
    resources/images/game/sudoku/{bg,num_btn,act_btn}.png
    resources/images/game/piano/{bg,note_0..7,key_0..7}.png
    resources/images/game/drum/{bg,note_0..5,pad_0..5}.png

★ 三条纪律（与工程其它素材生成器一致，违反会在 check() 里报出来）
  1. **1:1**：图尺寸 == 屏幕上贴的尺寸（`Canvas::Sprite` 不缩放不插值）。
  2. **缩小一律 `Image.BOX`**，禁用 LANCZOS/BILINEAR —— 带负瓣的核会在硬边**外**
     漏"幽灵像素"（2026-09-16 资源体检的三个根因之一）。本文件全程用 BOX。
  3. **静态/大块元素整图不透明**（把"所坐的底"烘进图里）⇒ 命中 `memcpy` 快路径。
     只有"小面积、数量少"的（下落音符）才用带 alpha 的图 —— 逐像素混合的代价
     跟"面积 × 张数"成正比，音符只有十来张 56x28，占屏面积 2%，可以接受。
     ⚠️ 反例（血案）：消消乐 64 格**透明**糖果贴凹槽 ⇒ render 17.7ms / **33fps**；
        把格子底烘进糖果图 ⇒ 4.6ms / **58fps**。

★ 为什么底图要留一块"**平的琴托色区**"（piano/drum 的 y>=KEYY / y>=PADY）：
  贴图要"整图不透明"就得把周围的底烘进图里，而**烘进去的底必须与真实底图一致**，
  否则按键/鼓垫周围会有一圈颜色对不上的方框。木纹是逐块随机的 ⇒ 没办法让每张小图
  各自烘对。所以底图在这些区域画成**均匀色**，小图烘同一个色即可严丝合缝。
"""

import math

from PIL import Image, ImageDraw, ImageFilter

SS = 4                      # 超采样倍数（几何图形放大 SS 倍画，再 BOX 缩回 ⇒ 精确覆盖率）
W, H = 480, 540             # 画布可视区（逻辑层 setViewport 给的就是这个）

NONE = (0, 0, 0, 0)

# ---- 木色令牌（唯一真值；底图与小图都从这里取）----
WOOD_D = (56, 36, 20)       # 深胡桃（阴影 / 缝）
WOOD_M = (104, 70, 40)      # 胡桃
WOOD_L = (146, 102, 60)     # 浅胡桃
WOOD_H = (186, 142, 92)     # 橡木高光
CONSOLE = (74, 48, 28)      # **平的琴托/鼓架色**（键与垫所坐的底，必须与小图烘的底一致）
IVORY = (242, 236, 222)     # 象牙键面
IVORY_D = (206, 196, 176)
INK = (34, 22, 12)

# 8 条音轨/6 条鼓轨的主题色
LANE_COL = [(224, 90, 74), (232, 138, 60), (232, 200, 74), (92, 200, 110),
            (74, 168, 224), (122, 122, 232), (176, 106, 224), (232, 106, 176)]
DRUM_COL = [(224, 90, 74), (232, 138, 60), (232, 200, 74),
            (92, 200, 110), (74, 168, 224), (176, 106, 224)]

# ---- 几何常量（与 C++ 共用；会一并写进 PgInstrArt.h）----
SUDOKU = dict(cell=44, bx=42, by=6,
              num_y=410, num_w=46, num_h=46, num_gap=4, num_x=17,
              act_y=464, act_w=88, act_h=58, act_gap=4, act_x=12, act_n=5)
PIANO = dict(key_w=60, key_h=144, key_y=392, judge_y=388, note_w=56, note_h=28)
# ★★ 打鼓的鼓垫几何（2026-09-16 改）：从"3 列 x 2 行 / 150px 宽"改成
#    **单排 6 个、各自正对上方那条掉落轨**（74px 宽 + 5px 缝 ⇒ x = 3 + i*79）。
#
#    为什么必须改：原布局的垫子是按**读序**排的（先填上行三格、再填下行三格：
#    `(lane%3, lane/3)`），而 6 条掉落轨是一整条**左→右横扫**（lane_w=80）——
#    两套坐标里"第 n 个"含义不同，实测 **4/6 条轨的 x 偏差 ≥120px（最大 200px = 屏宽 42%）**，
#    L3~L5 还要多往下挪一行。快档音符可见时间只有约 1.0s、鼓点间隔 680ms，
#    要求玩家在这点时间里"查表该敲哪个垫"必然掉分（唯一的颜色线索"轨头色标"还被
#    顶部信息条 y=8..40 盖住）。对齐之后 = "音符掉到哪个垫子上就敲哪个"，零学习成本，
#    这也是太鼓/吉他英雄/osu!mania 的通用映射。
#    尺寸取舍：74px 仍比"真触摸实测可用"的钢琴键（60px）宽，且不再需要上下跨行移动。
DRUM = dict(pad_w=74, pad_h=112, pad_x0=3, pad_dx=79, pad_y0=302,
            note_w=72, note_h=26, lane_w=80, fall_h=300)


# ==================================================================
#  基础工具
# ==================================================================

def h32(x):
    """与 C++ 侧同名的确定性哈希（纹理不闪；这里只用来定木纹的随机位）。"""
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def mix(a, b, t):
    """t: 0..256（与 Canvas::lerpColor 同语义）。"""
    if t <= 0:
        return tuple(a)
    if t >= 256:
        return tuple(b)
    return tuple(int(a[i] + (b[i] - a[i]) * t / 256.0) for i in range(3))


def shrink(im, w, h):
    """超采样缩回 —— **一律 BOX**（见文件头纪律 2）。"""
    return im.resize((w, h), Image.BOX)


def rr(w, h, r, fill, ss=SS, box=(0, 0, 0, 0)):
    """圆角矩形（抗锯齿）。box = (l,t,r,b) 内缩（像素）。"""
    im = Image.new("RGBA", (w * ss, h * ss), NONE)
    d = ImageDraw.Draw(im)
    l, t, ri, b = [v * ss for v in box]
    d.rounded_rectangle([l, t, w * ss - 1 - ri, h * ss - 1 - b],
                        radius=r * ss, fill=fill)
    return shrink(im, w, h)


def circ(w, h, cx, cy, rad, fill, ss=SS):
    im = Image.new("RGBA", (w * ss, h * ss), NONE)
    d = ImageDraw.Draw(im)
    d.ellipse([(cx - rad) * ss, (cy - rad) * ss, (cx + rad) * ss, (cy + rad) * ss],
              fill=fill)
    return shrink(im, w, h)


def wood(w, h, seed, base=WOOD_L, dark=WOOD_D, br=0.55, vig=0.0, vertical=False):
    """木纹底（**不透明 RGB**，返回 RGBA 且 alpha 全 255）。

    三层叠加：① 沿纹理方向的明暗带（模拟纹理起伏）② 若干条随机深色纹路
    ③ 细噪声（`Image.effect_noise` 是 C 实现，比逐像素 Python 快两个数量级）。
    再按需叠一层暗角。
    """
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    n = h if not vertical else w
    for i in range(n):
        # 用哈希做"不规则条带"而不是正弦 —— 木纹不是周期性的
        k = (h32(seed * 7919 + i) & 0xFF) / 255.0
        k = k * 0.6 + (h32(seed * 104729 + i // 3) & 0xFF) / 255.0 * 0.4
        c = mix(base, dark, int(k * br * 256))
        if vertical:
            d.line([(i, 0), (i, h)], fill=c)
        else:
            d.line([(0, i), (w, i)], fill=c)

    # 深色纹路：随机位置、随机宽度、随机浓度
    ov = Image.new("RGBA", (w, h), NONE)
    od = ImageDraw.Draw(ov)
    nline = max(6, n // 14)
    for j in range(nline):
        p = h32(seed * 31337 + j) % n
        ln = 20 + (h32(seed * 6151 + j) % max(20, n // 2))
        a = 26 + (h32(seed * 2749 + j) % 46)
        th = 1 + (h32(seed * 9973 + j) % 2)
        col = mix(dark, INK, 90) + (a,)
        if vertical:
            od.line([(p, 0), (p, h)], fill=col, width=th)
            od.line([(p + 3, 0), (p + 3, h)], fill=col, width=1)
        else:
            od.line([(0, p), (w, p)], fill=col, width=th)
            od.line([(0, p + 3), (w, p + 3)], fill=col, width=1)
        _ = ln
    im = Image.alpha_composite(im.convert("RGBA"), ov).convert("RGB")

    # 细噪声（±8 级）
    noise = Image.effect_noise((w, h), 10).convert("RGB")
    im = Image.blend(im, noise, 0.07)

    out = im.convert("RGBA").copy()
    if vig > 0:
        m = Image.new("L", (max(2, w // 4), max(2, h // 4)), 0)
        md = ImageDraw.Draw(m)
        md.ellipse([-m.width * 0.25, -m.height * 0.25, m.width * 1.25, m.height * 1.25],
                   fill=255)
        m = m.filter(ImageFilter.GaussianBlur(6)).resize((w, h), Image.BOX)
        dark = Image.new("RGBA", (w, h), INK + (255,))
        tmp = Image.composite(out, dark, m)
        out = Image.blend(out, tmp, vig)
    out.putalpha(Image.new("L", (w, h), 255))
    return out


def plate(w, h, r, base, light, edge, seed, face=None, inset=0):
    """一块"实木面板"：底 = 面板木色，四周一圈暗缝 + 顶部一道高光。

    face 给定时把它（RGB）当面板正面纹理，否则用 wood()。
    inset>0 时最外 inset 圈留 **back**（调用方自己先铺底），用于"控件坐在更深的色上"。
    """
    im = Image.new("RGBA", (w, h), NONE)
    body = rr(w, h, r, base + (255,), box=(2, 2, 2, 2))
    if face is not None:
        f = face.resize((w, h), Image.BOX).convert("RGBA") if face.size != (w, h) else \
            face.convert("RGBA")
        f.putalpha(body.split()[3])
        body = f
    im = Image.alpha_composite(im, body)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([2, 2, w - 3, h - 3], radius=r, outline=edge + (200,), width=1)
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=r + 1, outline=(0, 0, 0, 90), width=1)
    d.line([(r, 3), (w - r, 3)], fill=light + (110,))
    _ = seed
    return im


# ==================================================================
#  数独
# ==================================================================

def bake_sudoku_bg():
    s = SUDOKU
    cs, bx, by = s["cell"], s["bx"], s["by"]
    side = cs * 9
    im = wood(W, H, 11, base=WOOD_M, dark=WOOD_D, br=0.5, vig=0.35)
    d = ImageDraw.Draw(im)

    # 棋盘外框（双层，模拟实木棋盘)
    d.rectangle([bx - 8, by - 8, bx + side + 7, by + side + 7], fill=WOOD_D)
    d.rectangle([bx - 6, by - 6, bx + side + 5, by + side + 5], fill=WOOD_L)

    # 81 个浅色凹槽（格子）
    for r in range(9):
        for c in range(9):
            x, y = bx + c * cs, by + r * cs
            d.rectangle([x + 1, y + 1, x + cs - 2, y + cs - 2], fill=(228, 216, 194))
            d.line([(x + 1, y + 1), (x + cs - 2, y + 1)], fill=(246, 240, 226))
            d.line([(x + 1, y + 1), (x + 1, y + cs - 2)], fill=(246, 240, 226))
            d.line([(x + cs - 2, y + cs - 2), (x + 1, y + cs - 2)], fill=(196, 184, 164))
    # 细格线 + 3x3 宫分隔（画在格上）
    for i in range(1, 9):
        heavy = (i % 3 == 0)
        c = (0, 0, 0, 150) if heavy else (0, 0, 0, 70)
        wd = 3 if heavy else 1
        d.line([(bx + i * cs, by), (bx + i * cs, by + side)], fill=c, width=wd)
        d.line([(bx, by + i * cs), (bx + side, by + i * cs)], fill=c, width=wd)
    d.rectangle([bx, by, bx + side, by + side], outline=(0, 0, 0, 170), width=2)

    # 底部"平的琴托色区"：数字键/动作键所坐的底必须与小图烘的底**完全一致**
    d.rectangle([0, s["num_y"] - 12, W, H], fill=CONSOLE)
    d.line([(0, s["num_y"] - 12), (W, s["num_y"] - 12)], fill=INK, width=2)

    # 数字键与动作键的"凹槽"（小图坐进来正好齐平）
    for i in range(9):
        x = s["num_x"] + i * (s["num_w"] + s["num_gap"])
        d.rectangle([x, s["num_y"], x + s["num_w"] - 1, s["num_y"] + s["num_h"] - 1],
                    fill=mix(CONSOLE, INK, 110))
    for i in range(s.get("act_n", 5)):
        x = s["act_x"] + i * (s["act_w"] + s["act_gap"])
        d.rectangle([x, s["act_y"], x + s["act_w"] - 1, s["act_y"] + s["act_h"] - 1],
                    fill=mix(CONSOLE, INK, 110))
    im.putalpha(Image.new("L", (W, H), 255))
    return im


def _btn(w, h, r, seed, top, bot, label_free=True):
    """一枚木键（不透明；底烘 CONSOLE 色 —— 必须与底图 y>=num_y 的色一致）。"""
    im = Image.new("RGBA", (w, h), CONSOLE + (255,))
    face = wood(w, h, seed, base=top, dark=bot, br=0.5)
    body = rr(w, h, r, (0, 0, 0, 255), box=(6, 5, 6, 5))
    face.putalpha(body.split()[3])
    im = Image.alpha_composite(im, face)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([6, 5, w - 7, h - 6], radius=r, outline=INK + (170,), width=1)
    d.line([(r + 6, 7), (w - r - 7, 7)], fill=WOOD_H + (130,))
    d.line([(r + 6, h - 6), (w - r - 7, h - 6)], fill=(0, 0, 0, 120))
    _ = label_free
    im.putalpha(Image.new("L", (w, h), 255))
    return im


# ==================================================================
#  节奏钢琴
# ==================================================================

def bake_piano_bg():
    p = PIANO
    kw, kh, ky = p["key_w"], p["key_h"], p["key_y"]
    # 舞台 = 深胡桃木（**保留木纹**：掉落区叠的是半透明暗槽，纹理要透出来）
    im = wood(W, H, 23, base=mix(WOOD_M, WOOD_D, 70), dark=(30, 18, 14),
              br=0.72, vig=0.4)
    d = ImageDraw.Draw(im)

    # 掉落区：8 条**半透明暗槽**（不是实心黑 —— 否则木纹全被盖掉，看着像塑料）
    ov = Image.new("RGBA", (W, H), NONE)
    od = ImageDraw.Draw(ov)
    for i in range(8):
        x = i * 60
        od.rectangle([x + 1, 0, x + 59, ky - 1], fill=(14, 8, 16, 96))
        od.line([(x, 0), (x, ky)], fill=(8, 5, 9, 230), width=1)
        od.line([(x + 59, 0), (x + 59, ky)], fill=(0, 0, 0, 120), width=1)
    im = Image.alpha_composite(im, ov)
    d = ImageDraw.Draw(im)
    for i in range(8):   # 轨头色标（一眼看出每键管哪条轨）
        d.rectangle([i * 60 + 20, 6, i * 60 + 40, 14], fill=LANE_COL[i] + (235,))
        d.rectangle([i * 60 + 20, 6, i * 60 + 40, 14], outline=INK + (200,))
    d.rectangle([0, 0, W, 5], fill=WOOD_L)
    d.line([(0, 5), (W, 5)], fill=INK)

    # 判定线（亮木条 + 下方暗槽）
    d.rectangle([0, p["judge_y"] - 4, W, p["judge_y"] - 1], fill=WOOD_H)
    d.line([(0, p["judge_y"] - 4), (W, p["judge_y"] - 4)], fill=(255, 246, 226))
    d.rectangle([0, p["judge_y"], W, p["judge_y"] + 5], fill=mix(WOOD_D, INK, 120))

    # 琴托：**严格均匀色**（键图烘的就是这个色 —— 有木纹就对不齐了）
    d.rectangle([0, ky - 6, W, H], fill=CONSOLE)
    d.line([(0, ky - 6), (W, ky - 6)], fill=INK, width=2)
    d.line([(0, ky - 4), (W, ky - 4)], fill=WOOD_H + (150,))
    for i in range(8):   # 键槽（键图坐进来正好齐平）
        x = i * kw
        d.rectangle([x, ky, x + kw - 1, ky + kh - 1], fill=mix(CONSOLE, (0, 0, 0), 105))
    im.putalpha(Image.new("L", (W, H), 255))
    return im


def bake_piano_key(i):
    """一枚琴键（不透明；底烘 CONSOLE 色 —— 与底图 y>=key_y 完全一致）。"""
    p = PIANO
    w, h = p["key_w"], p["key_h"]
    im = Image.new("RGBA", (w, h), CONSOLE + (255,))
    face = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(face)
    for y in range(h):                       # 键面竖向渐变（上暗下亮 ⇒ 有厚度）
        t = y / float(h - 1)
        d.line([(0, y), (w - 1, y)], fill=mix(IVORY_D, IVORY, int(t * 200)))
    body = rr(w, h, 5, (0, 0, 0, 255), box=(4, 3, 4, 3))
    face.putalpha(body.split()[3])
    im = Image.alpha_composite(im, face)
    d = ImageDraw.Draw(im)
    # 底部彩帽（= 音轨色，一眼看出点哪根手指）
    col = LANE_COL[i]
    cap = rr(w, h, 5, col + (255,), box=(6, h - 34, 6, 5))
    im = Image.alpha_composite(im, cap)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([4, 3, w - 5, h - 4], radius=5, outline=INK + (160,), width=1)
    d.line([(7, h - 36), (w - 8, h - 36)], fill=(0, 0, 0, 110))
    d.line([(5, 2), (w - 6, 2)], fill=(255, 255, 255, 150))
    # 键面小圆点（音名位）
    dot = circ(14, 14, 7, 7, 3.2, mix(col, (255, 255, 255), 150) + (255,))
    im.alpha_composite(dot, (w // 2 - 7, 16))
    im.putalpha(Image.new("L", (w, h), 255))
    return im


def bake_piano_note(i):
    """下落音符（**带 alpha**：小面积、数量少 ⇒ 逐像素混合可接受）。"""
    p = PIANO
    w, h = p["note_w"], p["note_h"]
    im = Image.new("RGBA", (w, h), NONE)
    col = LANE_COL[i]
    body = rr(w, h, 6, col + (255,), box=(1, 1, 1, 1))
    im = Image.alpha_composite(im, body)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=6,
                        outline=mix(col, (0, 0, 0), 150) + (255,), width=1)
    d.line([(6, 3), (w - 7, 3)], fill=mix(col, (255, 255, 255), 190) + (255,))
    d.line([(6, h - 4), (w - 7, h - 4)], fill=mix(col, (0, 0, 0), 120) + (200,))
    return im


# ==================================================================
#  打鼓
# ==================================================================

def bake_drum_bg():
    p = DRUM
    # 舞台 = 深枫木（同样**保留木纹**）
    im = wood(W, H, 37, base=mix(WOOD_M, WOOD_D, 80), dark=(28, 20, 14),
              br=0.75, vig=0.45)
    d = ImageDraw.Draw(im)

    # 掉落区：6 条半透明暗槽
    ov = Image.new("RGBA", (W, H), NONE)
    od = ImageDraw.Draw(ov)
    for i in range(6):
        x = i * p["lane_w"]
        od.rectangle([x + 2, 0, x + p["lane_w"] - 3, p["fall_h"] - 1],
                     fill=(12, 10, 8, 100))
        od.line([(x, 0), (x, p["fall_h"])], fill=(8, 6, 5, 235), width=1)
    im = Image.alpha_composite(im, ov)
    d = ImageDraw.Draw(im)
    for i in range(6):   # 轨头色标
        d.rectangle([i * p["lane_w"] + 26, 6, i * p["lane_w"] + 54, 14],
                    fill=DRUM_COL[i] + (235,))
        d.rectangle([i * p["lane_w"] + 26, 6, i * p["lane_w"] + 54, 14],
                    outline=INK + (200,))
    d.rectangle([0, 0, W, 5], fill=WOOD_L)
    d.line([(0, 5), (W, 5)], fill=INK)

    # 判定线
    d.rectangle([0, p["fall_h"] - 4, W, p["fall_h"] - 1], fill=WOOD_H)
    d.line([(0, p["fall_h"] - 4), (W, p["fall_h"] - 4)], fill=(255, 246, 226))
    d.rectangle([0, p["fall_h"], W, p["fall_h"] + 5], fill=mix(WOOD_D, INK, 120))

    # 鼓架：**严格均匀色**（鼓垫图烘的就是这个色）
    d.rectangle([0, p["fall_h"] + 6, W, H], fill=CONSOLE)
    d.line([(0, p["fall_h"] + 6), (W, p["fall_h"] + 6)], fill=INK, width=2)
    # 单排 6 个垫槽：**每个槽正对上方那条掉落轨**（见 DRUM 常量的说明）
    for i in range(6):
        x = p["pad_x0"] + i * p["pad_dx"]
        y = p["pad_y0"]
        d.rectangle([x, y, x + p["pad_w"] - 1, y + p["pad_h"] - 1],
                    fill=mix(CONSOLE, (0, 0, 0), 105))
    # 垫子下方那条"键位名牌"底：给画布上的鼓位名一个稳的底（不是纯色块，压一点暗）
    d.rectangle([0, p["pad_y0"] + p["pad_h"] + 4, W, p["pad_y0"] + p["pad_h"] + 34],
                fill=mix(CONSOLE, (0, 0, 0), 40))
    d.line([(0, p["pad_y0"] + p["pad_h"] + 4), (W, p["pad_y0"] + p["pad_h"] + 4)],
           fill=INK, width=1)
    im.putalpha(Image.new("L", (W, H), 255))
    return im


def bake_drum_pad(i):
    """一面鼓垫（不透明；底烘 CONSOLE 色）。

    ★ 2026-09-16：垫子从 150px 宽改成 **74px 宽**（单排 6 个、各自正对掉落轨，见 DRUM 的说明），
      于是"鼓皮 + 主题色环"不能再按整张图拉伸着画 —— 74x112 上那个椭圆会被拉成一条竖长条，
      既不像鼓也不好认色。改成**居中的正圆鼓皮**（直径 = 宽度方向留白后的 60px）+ 木框，
      颜色环仍在圆心 —— 它是"该敲哪个垫"的身份标识（与掉落的音符同色）。
    """
    p = DRUM
    w, h = p["pad_w"], p["pad_h"]
    col = DRUM_COL[i]
    im = Image.new("RGBA", (w, h), CONSOLE + (255,))
    d = ImageDraw.Draw(im)
    # 木框（鼓腔）
    ring = rr(w, h, 12, WOOD_M + (255,), box=(2, 2, 2, 2))
    im = Image.alpha_composite(im, ring)
    d = ImageDraw.Draw(im)
    # 鼓皮：居中的正圆（直径 = 宽度方向的可画区），纵向做明暗渐变
    dia = w - 14
    cx, cy = w // 2, h // 2
    box = [cx - dia // 2, cy - dia // 2, cx + dia // 2, cy + dia // 2]
    skin = Image.new("RGBA", (w, h), NONE)
    sd = ImageDraw.Draw(skin)
    for y in range(max(0, box[1]), min(h, box[3] + 1)):
        t = abs(y - cy) / float(max(1, dia // 2))
        c = mix((244, 241, 234), (166, 156, 146), int(min(1.0, t) * 235))
        sd.line([(0, y), (w - 1, y)], fill=tuple(c) + (255,))
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).ellipse(box, fill=255)
    skin.putalpha(m)
    im = Image.alpha_composite(im, skin)
    d = ImageDraw.Draw(im)
    # 主题色环（身份标识：与该轨掉落的音符同色）+ 上方高光
    d.ellipse(box, outline=col + (240,), width=4)
    d.arc([box[0] + 3, box[1] + 3, box[2] - 3, box[3] - 3], 200, 340,
          fill=(255, 255, 255, 140), width=3)
    im.putalpha(Image.new("L", (w, h), 255))
    return im


def bake_drum_note(i):
    w, h = DRUM["note_w"], DRUM["note_h"]
    im = Image.new("RGBA", (w, h), NONE)
    col = DRUM_COL[i]
    body = rr(w, h, h // 2, col + (255,), box=(1, 1, 1, 1))
    im = Image.alpha_composite(im, body)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=h // 2,
                        outline=mix(col, (0, 0, 0), 150) + (255,), width=1)
    d.line([(h, 3), (w - h, 3)], fill=mix(col, (255, 255, 255), 190) + (255,))
    return im


# ==================================================================
#  入口（由 gen_game_art.main 调用）
# ==================================================================

def bake_all(save, check, items):
    """返回自检失败数。save/check/items 由 gen_game_art.main 传入（共用同一套输出逻辑）。"""
    bad = 0

    # ---------------- 数独 ----------------
    G = "sudoku"
    im = bake_sudoku_bg()
    save(im, "%s/bg.png" % G)
    bad += check(im, "sudoku/bg", (0, 0), opaque_full=True)
    items.append(("sudoku", "kSudokuBg", "%s/bg.png" % G, W, H, 0, 0,
                  "木纹棋盘（81 格凹槽 + 3x3 宫线）+ 平的琴托色区（静态整屏，整图不透明）"))

    s = SUDOKU
    im = _btn(s["num_w"], s["num_h"], 8, 71, WOOD_H, WOOD_M)
    save(im, "%s/num_btn.png" % G)
    bad += check(im, "sudoku/num_btn", (0, 0), opaque_full=True)
    items.append(("sudoku", "kSudokuNumBtn", "%s/num_btn.png" % G,
                  s["num_w"], s["num_h"], 0, 0,
                  "数字键（不透明，底烘琴托色；9 个键共用同一张，数字由画布字库写）"))

    im = _btn(s["act_w"], s["act_h"], 10, 97, WOOD_M, WOOD_D)
    save(im, "%s/act_btn.png" % G)
    bad += check(im, "sudoku/act_btn", (0, 0), opaque_full=True)
    items.append(("sudoku", "kSudokuActBtn", "%s/act_btn.png" % G,
                  s["act_w"], s["act_h"], 0, 0,
                  "动作键（擦除/笔记/难度/提示/新局，5 个共用同一张）"))

    # ---------------- 节奏钢琴 ----------------
    G = "piano"
    im = bake_piano_bg()
    save(im, "%s/bg.png" % G)
    bad += check(im, "piano/bg", (0, 0), opaque_full=True)
    items.append(("piano", "kPianoBg", "%s/bg.png" % G, W, H, 0, 0,
                  "木纹舞台 + 8 条掉落轨 + 判定线 + 平的琴托色区（静态整屏，整图不透明）"))

    p = PIANO
    for i in range(8):
        im = bake_piano_key(i)
        save(im, "%s/key_%d.png" % (G, i))
        bad += check(im, "piano/key_%d" % i, (0, 0), opaque_full=True)
        items.append(("piano", "kPianoKey%d" % i, "%s/key_%d.png" % (G, i),
                      p["key_w"], p["key_h"], 0, 0,
                      "第 %d 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）" % i))
    for i in range(8):
        im = bake_piano_note(i)
        save(im, "%s/note_%d.png" % (G, i))
        bad += check(im, "piano/note_%d" % i, (0, 0))
        items.append(("piano", "kPianoNote%d" % i, "%s/note_%d.png" % (G, i),
                      p["note_w"], p["note_h"], 0, 0,
                      "第 %d 号下落音符（带 alpha，贴合轨）" % i))

    # ---------------- 打鼓 ----------------
    G = "drum"
    im = bake_drum_bg()
    save(im, "%s/bg.png" % G)
    bad += check(im, "drum/bg", (0, 0), opaque_full=True)
    items.append(("drum", "kDrumBg", "%s/bg.png" % G, W, H, 0, 0,
                  "木纹舞台 + 6 条掉落轨 + 平的鼓架色区（静态整屏，整图不透明）"))

    d_ = DRUM
    for i in range(6):
        im = bake_drum_pad(i)
        save(im, "%s/pad_%d.png" % (G, i))
        bad += check(im, "drum/pad_%d" % i, (0, 0), opaque_full=True)
        items.append(("drum", "kDrumPad%d" % i, "%s/pad_%d.png" % (G, i),
                      d_["pad_w"], d_["pad_h"], 0, 0,
                      "第 %d 号鼓垫（不透明，底烘鼓架色）" % i))
    for i in range(6):
        im = bake_drum_note(i)
        save(im, "%s/note_%d.png" % (G, i))
        bad += check(im, "drum/note_%d" % i, (0, 0))
        items.append(("drum", "kDrumNote%d" % i, "%s/note_%d.png" % (G, i),
                      d_["note_w"], d_["note_h"], 0, 0,
                      "第 %d 号下落音符（带 alpha，贴合轨）" % i))

    # ---------------- 预览拼图（人看） ----------------
    try:
        import os
        sheet = ["sudoku/bg.png", "sudoku/num_btn.png", "sudoku/act_btn.png",
                 "piano/bg.png"] + ["piano/key_%d.png" % i for i in range(4)] + \
                ["piano/note_%d.png" % i for i in range(4)] + \
                ["drum/bg.png"] + ["drum/pad_%d.png" % i for i in range(3)] + \
                ["drum/note_%d.png" % i for i in range(3)]
        # 由调用方决定路径，这里只返回清单
        PREVIEW_PATHS.extend(sheet)
    except Exception:  # noqa: BLE001
        pass
    _ = math
    return bad


PREVIEW_PATHS = []
