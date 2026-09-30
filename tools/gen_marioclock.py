#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gen_marioclock.py - 屏保时钟的「超级玛丽砖块」素材（2026-09-18 需求）

需求原文：
  「时钟的格式改成超级玛丽的砖块风格，时钟的背景是砖块，然后当时间要变的时候整个前面 3 秒，
   做一个超级玛丽的动画从屏幕左边走出来，然后到了分钟的小数据的位置跳起来顶一下砖块，
   然后砖块的数字像金币一样弹出，再出来一个新的数字。整个屏保时钟的诗词、喜好按键都删掉，
   背景风格采用超级马里奥游戏背景风格。」

产物（resources/images/，全部 1:1 硬边、不重采样 ⇒ 无锯齿/无幽灵像素）：
  saver_bg_day.png    480x800  RGB   白天地面关（蓝天白云绿丘 + 地面砖 + 悬空砖台 + 顶部黑条）
  saver_bg_night.png  480x800  RGB   夜空关（深蓝 + 星月 + 剪影 + 砖台，夜里不刺眼）
  mc_brick.png         88x140  RGB   数字底板（砖块）
  mc_dig{d}.png        88x140  RGBA  金色像素数字（0..9）
  mc_colon_on/off.png   26x140 RGBA  冒号两态
  mc_mario_w0/1/2.png   68x68  RGBA  马里奥走路 3 帧
  mc_mario_jump.png     68x68  RGBA  马里奥跳起（顶砖块那一帧）

为什么数字是**图片**而不是字控件：
  金币弹出要把数字"飞出砖块 + 横向压扁成旋转的金币"，原生控件没有裁剪/旋转文本的能力，
  而图片可以（控件 setPosition 改宽高即压缩）。这与翻页钟原来把数字烘进图片是同一个理由。
  ⚠️ 仍然是"静态图片资源 + 原生控件"，没有一行画布绘制代码（符合"非游戏禁止自绘"铁律）。

为什么数字自己当金币（而不是另做一套金币图）：
  「砖块的数字像金币一样弹出」= 旧数字本身就是那个金币 ⇒ 直接把**当前数字控件**
  从砖块位置往上飞 + 横向压扁，飞完隐藏、换成新数字控件落下。少 40 个控件、少 10 张图。

用法：python tools/gen_marioclock.py
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "resources", "images")

# ---------------- 几何（必须与 ui/screensaver.html 逐字一致）----------------
SCR_W, SCR_H = 480, 800
HUD_H = 56                     # 顶部黑条（SMB 计分板风）高
CELL_W, CELL_H = 88, 140       # 数字砖块
CELL_Y = 180                   # 砖块顶边
COLON_W, COLON_H = 26, 140
PLAT_Y, PLAT_H = 540, 60       # 悬空砖台（马里奥走在这上面）
GND_Y = 690                    # 地面砖顶边
MARIO_W, MARIO_H = 68, 68      # 马里奥（16x16 像素格 x4 + 2px 描边）
MARIO_SCALE = 4
MARIO_PAD = 2                  # 描边外扩

# ---------------- 配色（白天/夜空共用一套砖块，保证两种背景下都好看）----------------
BRICK_BODY = (0xA8, 0x42, 0x0C)
BRICK_HI = (0xC8, 0x6A, 0x24)
BRICK_SH = (0x7A, 0x2E, 0x06)
MORTAR = (0x2E, 0x0C, 0x02)
DIGIT = (0xFF, 0xD2, 0x4A)     # 金币金
DIGIT_EDGE = (0x2E, 0x0C, 0x02)

DAY_SKY = (0x5C, 0x94, 0xFC)
DAY_CLOUD = (0xFF, 0xFF, 0xFF)
DAY_GRASS = (0x00, 0xA8, 0x00)
DAY_GRASS_DARK = (0x00, 0x6C, 0x00)
DAY_GND_BODY = (0xC0, 0x70, 0x30)
DAY_GND_HI = (0xE0, 0xA0, 0x60)
DAY_GND_MORTAR = (0x8B, 0x4A, 0x18)
DAY_PIPE = (0x00, 0xA8, 0x00)
DAY_PIPE_HI = (0x58, 0xD8, 0x58)
HUD_DAY = (0x00, 0x00, 0x00)

NIGHT_SKY = (0x10, 0x1A, 0x3A)
NIGHT_HILL = (0x0A, 0x10, 0x24)
NIGHT_GND_BODY = (0x1E, 0x2E, 0x52)
NIGHT_GND_HI = (0x33, 0x4A, 0x78)
NIGHT_GND_MORTAR = (0x08, 0x0E, 0x1C)
NIGHT_MOON = (0xF5, 0xE9, 0xA0)
NIGHT_STAR = (0xF5, 0xF8, 0xFF)
NIGHT_PIPE = (0x1A, 0x6B, 0x3A)
NIGHT_PIPE_HI = (0x2E, 0x8F, 0x54)
HUD_NIGHT = (0x05, 0x08, 0x0F)

# ---------------- 马里奥像素造型（16x16 格，字符 = 色块）----------------
# . 透明  R 红帽/上衣  S 皮肤  B 蓝背带裤  Y 黄扣  N 深棕（胡子/鞋）  K 黑眼
MARIO_PAL = {
    ".": None,
    "R": (0xE0, 0x3A, 0x1E),
    "S": (0xF2, 0xB4, 0x8C),
    "B": (0x2A, 0x54, 0xC8),
    "Y": (0xF5, 0xC5, 0x42),
    "N": (0x5B, 0x3A, 0x16),
    "K": (0x2A, 0x1A, 0x0A),
}

# 每帧：矩形 (x, y, w, h, 色)。走 3 帧靠**腿部/手臂**区别，跳跃帧手臂上举。
_M_BASE = [
    (4, 0, 8, 3, "R"),    # 帽顶
    (3, 3, 10, 2, "R"),   # 帽檐
    (4, 4, 8, 3, "S"),    # 脸
    (9, 4, 2, 2, "K"),    # 眼
    (4, 7, 8, 2, "N"),    # 胡子
    (4, 9, 8, 4, "B"),    # 背带裤
    (6, 10, 1, 1, "Y"),   # 扣子
    (10, 10, 1, 1, "Y"),
]
MARIO_FRAMES = {
    "w0": _M_BASE + [
        (2, 9, 2, 3, "S"), (13, 9, 2, 3, "S"),
        (3, 13, 3, 2, "B"), (10, 13, 3, 2, "B"),
        (2, 15, 4, 1, "N"), (10, 15, 4, 1, "N"),
    ],
    "w1": _M_BASE + [
        (2, 10, 2, 3, "S"), (13, 10, 2, 3, "S"),
        (5, 13, 3, 2, "B"), (9, 13, 3, 2, "B"),
        (4, 15, 4, 1, "N"), (9, 15, 4, 1, "N"),
    ],
    "w2": _M_BASE + [
        (1, 9, 2, 3, "S"), (13, 10, 2, 3, "S"),
        (4, 13, 3, 2, "B"), (9, 13, 3, 2, "B"),
        (3, 15, 4, 1, "N"), (10, 15, 4, 1, "N"),
    ],
    "jump": _M_BASE + [
        (1, 6, 2, 4, "S"), (13, 6, 2, 4, "S"),   # 双手上举（顶砖块）
        (2, 13, 4, 2, "B"), (10, 13, 4, 2, "B"),
        (1, 15, 4, 1, "N"), (11, 15, 4, 1, "N"),
    ],
}

# ---------------- 金色像素数字（6 宽 x 10 高，scale 11 -> 66x110）----------------
DIGIT_SCALE = 11
FONT_6x10 = {
    0: [".####.", "#....#", "#....#", "#....#", "#....#",
        "#....#", "#....#", "#....#", "#....#", ".####."],
    1: ["..##..", ".###..", "..##..", "..##..", "..##..",
        "..##..", "..##..", "..##..", "..##..", ".####."],
    2: [".####.", "#....#", ".....#", ".....#", "....##",
        "..##..", ".##...", "##....", "#.....", "######"],
    3: [".####.", "#....#", ".....#", ".....#", "..###.",
        ".....#", ".....#", ".....#", "#....#", ".####."],
    4: ["....##", "...###", "..#.##", ".#..##", "#...##",
        "######", "....##", "....##", "....##", "....##"],
    5: ["######", "#.....", "#.....", "#.....", "#####.",
        ".....#", ".....#", ".....#", "#....#", ".####."],
    6: ["..####", ".#....", "#.....", "#.....", "#####.",
        "#....#", "#....#", "#....#", "#....#", ".####."],
    7: ["######", ".....#", ".....#", "....##", "....#.",
        "...##.", "...##.", "...#..", "...#..", "...#.."],
    8: [".####.", "#....#", "#....#", "#....#", ".####.",
        "#....#", "#....#", "#....#", "#....#", ".####."],
    9: [".####.", "#....#", "#....#", "#....#", "#....#",
        ".#####", ".....#", ".....#", "....#.", ".####."],
}


def _rects(img, rects, scale, off=(0, 0)):
    d = ImageDraw.Draw(img)
    for (x, y, w, h, c) in rects:
        col = MARIO_PAL[c]
        if col is None:
            continue
        d.rectangle([off[0] + x * scale, off[1] + y * scale,
                     off[0] + (x + w) * scale - 1, off[1] + (y + h) * scale - 1],
                    fill=col)


def make_mario(frame):
    """68x68 马里奥（16x16 格 x4 = 64，外加 2px 深色描边）。"""
    body = Image.new("RGBA", (16 * MARIO_SCALE, 16 * MARIO_SCALE), (0, 0, 0, 0))
    _rects(body, MARIO_FRAMES[frame], MARIO_SCALE)
    mask = body.getchannel("A").point(lambda v: 255 if v > 0 else 0)
    # 描边：把轮廓外扩 2px（MaxFilter(5) = 半径 2），垫在身体下面
    edge = mask.filter(ImageFilter.MaxFilter(2 * MARIO_PAD + 1))
    out = Image.new("RGBA", (MARIO_W, MARIO_H), (0, 0, 0, 0))
    ec = Image.new("L", (MARIO_W, MARIO_H), 0)
    ec.paste(edge, (MARIO_PAD, MARIO_PAD))
    out.paste(Image.new("RGBA", (MARIO_W, MARIO_H), (0x1A, 0x0E, 0x06, 255)),
              (0, 0), ec)
    out.paste(body, (MARIO_PAD, MARIO_PAD), body)
    return out


def make_digit(ch):
    """88x140 金色像素数字（含 3px 深色描边），居中。"""
    rows = FONT_6x10[ch]
    w6, h10 = len(rows[0]), len(rows)
    ink = Image.new("L", (w6 * DIGIT_SCALE, h10 * DIGIT_SCALE), 0)
    d = ImageDraw.Draw(ink)
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c == "#":
                d.rectangle([x * DIGIT_SCALE, y * DIGIT_SCALE,
                             (x + 1) * DIGIT_SCALE - 1, (y + 1) * DIGIT_SCALE - 1],
                            fill=255)
    edge = ink.filter(ImageFilter.MaxFilter(7))          # 外扩 3px
    iw, ih = ink.size
    pad = 3
    ox, oy = (CELL_W - iw) // 2, (CELL_H - ih) // 2
    out = Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0))
    ec = Image.new("L", (iw + 2 * pad, ih + 2 * pad), 0)
    ec.paste(edge, (pad, pad))
    out.paste(Image.new("RGBA", (iw + 2 * pad, ih + 2 * pad), DIGIT_EDGE + (255,)),
              (ox - pad, oy - pad), ec)
    out.paste(Image.new("RGBA", (iw, ih), DIGIT + (255,)), (ox, oy), ink)
    return out


def make_colon(on):
    out = Image.new("RGBA", (COLON_W, COLON_H), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    col = DIGIT if on else (0x6A, 0x4A, 0x18)
    cx = COLON_W // 2
    for cy in (COLON_H // 2 - 26, COLON_H // 2 + 26):
        d.ellipse([cx - 11, cy - 11, cx + 11, cy + 11], fill=DIGIT_EDGE)
        d.ellipse([cx - 8, cy - 8, cx + 8, cy + 8], fill=col)
    return out


def brick_row(d, x0, y0, w, h, body, hi, sh, phase):
    """一段砖：body + 顶部高光 + 底部阴影，按 phase 决定竖缝错位。"""
    x = x0 - (22 if phase else 0)
    while x < x0 + w:
        d.rectangle([x, y0, x + 41, y0 + h - 1], fill=body)
        d.rectangle([x, y0, x + 41, y0 + 2], fill=hi)
        d.rectangle([x, y0 + h - 3, x + 41, y0 + h - 1], fill=sh)
        x += 44


def brick_panel(img, x0, y0, w, h, body=BRICK_BODY, hi=BRICK_HI, sh=BRICK_SH,
                mortar=MORTAR):
    """在 img 上画一块"跑砖"面板（44x35 一皮，奇偶皮错缝）。"""
    d = ImageDraw.Draw(img)
    d.rectangle([x0, y0, x0 + w - 1, y0 + h - 1], fill=mortar)
    y = y0
    row = 0
    while y < y0 + h:
        hh = min(35, y0 + h - y)
        brick_row(d, x0, y, w, hh - 3, body, hi, sh, row % 2)
        y += 35
        row += 1


def ground_panel(img, x0, y0, w, h, body, hi, mortar):
    d = ImageDraw.Draw(img)
    d.rectangle([x0, y0, x0 + w - 1, y0 + h - 1], fill=mortar)
    y = y0
    row = 0
    while y < y0 + h:
        hh = min(39, y0 + h - y)
        x = x0 - (20 if row % 2 else 0)
        while x < x0 + w:
            d.rectangle([x, y, x + 37, y + hh - 3], fill=body)
            d.rectangle([x, y, x + 37, y + 2], fill=hi)
            x += 40
        y += 40
        row += 1


def cloud(img, cx, cy, s=1.0):
    d = ImageDraw.Draw(img)
    for (dx, dy, r) in ((-42, 8, 22), (-8, -6, 30), (30, 4, 24), (0, 12, 20)):
        rr = int(r * s)
        d.ellipse([cx + int(dx * s) - rr, cy + int(dy * s) - rr,
                   cx + int(dx * s) + rr, cy + int(dy * s) + rr],
                  fill=DAY_CLOUD)


def pipe(img, x, y, w, h, body, hi, dark):
    d = ImageDraw.Draw(img)
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=body)
    d.rectangle([x + 4, y, x + 14, y + h - 1], fill=hi)
    d.rectangle([x + w - 12, y, x + w - 5, y + h - 1], fill=dark)
    lip_h = 18
    d.rectangle([x - 5, y, x + w + 4, y + lip_h - 1], fill=body)
    d.rectangle([x - 5, y, x - 1, y + lip_h - 1], fill=hi)
    d.rectangle([x + w, y, x + w + 4, y + lip_h - 1], fill=dark)
    d.rectangle([x + 4, y, x + 14, y + lip_h - 1], fill=hi)


def bush(img, cx, cy, w, h):
    d = ImageDraw.Draw(img)
    b = h // 2
    d.rectangle([cx - w // 2, cy - b, cx + w // 2, cy + b], fill=DAY_GRASS)
    for dx in (-w // 3, 0, w // 3):
        d.ellipse([cx + dx - b, cy - b - b // 2, cx + dx + b, cy - b + b // 2],
                  fill=DAY_GRASS)


def bg_day():
    im = Image.new("RGB", (SCR_W, SCR_H), DAY_SKY)
    d = ImageDraw.Draw(im)
    # ⚠️ 云必须放在 y >= 330（时钟下缘 320 以下）：数字"金币弹出"要往上飞最多 150px
    #    （钟顶 180 -> 30），那块天空是**飞行走廊**，压一朵白云上去金币就没对比度了。
    cloud(im, 74, 392, 1.0)
    cloud(im, 348, 446, 0.85)
    cloud(im, 206, 352, 0.6)
    # 山丘（先画深色描边再叠亮色，做出层次）
    for (cx, cy, rx, ry, col) in ((96, 664, 158, 96, DAY_GRASS_DARK),
                                  (418, 686, 176, 108, DAY_GRASS_DARK)):
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=col)
    for (cx, cy, rx, ry) in ((96, 656, 148, 88), (418, 678, 166, 100)):
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=DAY_GRASS)
    pipe(im, 14, 596, 46, GND_Y - 596, DAY_PIPE, DAY_PIPE_HI, DAY_GRASS_DARK)
    ground_panel(im, 0, GND_Y, SCR_W, SCR_H - GND_Y,
                 DAY_GND_BODY, DAY_GND_HI, DAY_GND_MORTAR)
    bush(im, 250, GND_Y - 4, 96, 34)
    bush(im, 396, GND_Y - 4, 72, 26)
    brick_panel(im, 0, PLAT_Y, SCR_W, PLAT_H)
    d.rectangle([0, 0, SCR_W - 1, HUD_H - 1], fill=HUD_DAY)
    return im


def bg_night():
    im = Image.new("RGB", (SCR_W, SCR_H), NIGHT_SKY)
    d = ImageDraw.Draw(im)
    stars = [(38, 96, 2), (86, 74, 2), (132, 128, 1), (176, 88, 2), (214, 150, 2),
             (258, 104, 1), (300, 76, 2), (338, 132, 2), (432, 92, 1), (462, 148, 2),
             (58, 206, 1), (120, 240, 2), (196, 214, 1), (268, 262, 2), (330, 226, 1),
             (406, 250, 2), (452, 320, 1), (84, 344, 2), (158, 386, 1), (236, 348, 2),
             (312, 402, 1), (388, 372, 2), (60, 452, 1), (440, 442, 2)]
    for (x, y, r) in stars:
        d.rectangle([x, y, x + r, y + r], fill=NIGHT_STAR)
    # 月亮压到山丘上（y>=330）：钟上方那条是金币飞行走廊，月亮搁那儿会被金币穿过
    d.ellipse([350, 384, 442, 476], fill=NIGHT_MOON)
    for (dx, dy, r) in ((-18, -12, 7), (12, 6, 9), (-4, 20, 5)):
        d.ellipse([396 + dx - r, 430 + dy - r, 396 + dx + r, 430 + dy + r],
                  fill=(0xE8, 0xD9, 0x8A))
    for (cx, cy, rx, ry) in ((96, 664, 158, 96), (418, 686, 176, 108)):
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=NIGHT_HILL)
    pipe(im, 14, 596, 46, GND_Y - 596, NIGHT_PIPE, NIGHT_PIPE_HI, (0x0E, 0x40, 0x22))
    ground_panel(im, 0, GND_Y, SCR_W, SCR_H - GND_Y,
                 NIGHT_GND_BODY, NIGHT_GND_HI, NIGHT_GND_MORTAR)
    brick_panel(im, 0, PLAT_Y, SCR_W, PLAT_H)
    d.rectangle([0, 0, SCR_W - 1, HUD_H - 1], fill=HUD_NIGHT)
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    bad = 0

    def save(im, name):
        p = os.path.join(OUT, name)
        im.save(p)
        print("  %-24s %dx%d %s" % (name, im.size[0], im.size[1], im.mode))
        return p

    save(bg_day(), "saver_bg_day.png")
    save(bg_night(), "saver_bg_night.png")
    brick = Image.new("RGB", (CELL_W, CELL_H))
    brick_panel(brick, 0, 0, CELL_W, CELL_H)
    save(brick, "mc_brick.png")

    for i in range(10):
        rows = FONT_6x10[i]
        if len(rows) != 10 or any(len(r) != 6 for r in rows):
            print("  !! 数字 %d 的字模尺寸不对" % i)
            bad += 1
        dig = make_digit(i)
        # 自检：墨迹必须落在画布内且四角透明
        bb = dig.getchannel("A").getbbox()
        if not bb or bb[0] < 0 or bb[2] > CELL_W or bb[3] > CELL_H:
            print("  !! 数字 %d 墨迹越界 %s" % (i, bb))
            bad += 1
        save(dig, "mc_dig%d.png" % i)

    save(make_colon(True), "mc_colon_on.png")
    save(make_colon(False), "mc_colon_off.png")

    feet = []
    for f in ("w0", "w1", "w2", "jump"):
        m = make_mario(f)
        bb = m.getchannel("A").getbbox()
        feet.append(bb[3] if bb else -1)
        save(m, "mc_mario_%s.png" % f)
    # 自检：4 帧的"脚底"必须同一行 —— 否则走/跳之间人物会上下跳一下
    if len(set(feet)) != 1:
        print("  !! 4 帧脚底不在同一行 %s" % feet)
        bad += 1
    else:
        print("  马里奥 4 帧脚底 y=%d（画布高 %d）" % (feet[0], MARIO_H))

    print("共 19 张素材，自检%s" % ("失败 %d 项" % bad if bad else "通过"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
