#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pet3d/sheet.py - 把渲染出来的 PNG 合成成"给人看"的预览图

产出（都在 tools/pet3d/preview/）：
  hero.png      大图 3/4 视角（合成在设备底色上）
  faces.png     12 个表情特写 + 中文标签
  device.png    设备实机版面 480x800（含导航栏 / HUD 区 / 键位提示），宠物按**真实尺寸**摆
用法: python sheet.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(ROOT, 'out')
PREV = os.path.join(ROOT, 'preview')
os.makedirs(PREV, exist_ok=True)

FONT = 'C:/Windows/Fonts/msyh.ttc'
FONTB = 'C:/Windows/Fonts/msyhbd.ttc'
PAGE_BG = (11, 11, 13)
CHIP = (28, 28, 30)

EXPR = ['开心', '大笑', '爱心', '哭', '生气', '睡觉',
        '惊讶', '墨镜', '晕', '思考', '闪光', '害羞']


def font(sz, bold=False):
    try:
        return ImageFont.truetype(FONTB if bold else FONT, sz)
    except Exception:
        return ImageFont.load_default()


def flat(im, bg=PAGE_BG):
    base = Image.new('RGBA', im.size, bg + (255,))
    return Image.alpha_composite(base, im.convert('RGBA')).convert('RGB')


def do_hero():
    im = Image.open(os.path.join(OUT, 'hero.png'))
    bg = flat(im)
    bg.save(os.path.join(PREV, 'hero.png'))
    print('hero', bg.size)


def do_faces():
    cw, ch = 320, 200          # 单格渲染尺寸
    pad, lab, cols = 18, 34, 4
    rows = (len(EXPR) + cols - 1) // cols
    W = cols * cw + (cols + 1) * pad
    H = rows * (ch + lab) + (rows + 1) * pad + 44
    sheet = Image.new('RGB', (W, H), PAGE_BG)
    d = ImageDraw.Draw(sheet)
    d.text((pad, 12), 'WorkBuddy 电子宠物 · 12 个表情（脸部屏幕内容）',
           font=font(21, True), fill=(238, 240, 245))
    for i, e in enumerate(EXPR):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = 44 + pad + r * (ch + lab + pad)
        f = os.path.join(OUT, 'face_%s.png' % e)
        if not os.path.exists(f):
            continue
        cell = flat(Image.open(f))
        sheet.paste(cell, (x, y))
        tw = d.textlength(e, font=font(18, True))
        d.text((x + (cw - tw) / 2, y + ch + 6), e, font=font(18, True), fill=(200, 208, 220))
    sheet.save(os.path.join(PREV, 'faces.png'))
    print('faces', sheet.size)


POSES = ['待机', '开心跳', '打招呼', '睡觉', '生气', '哭']


def do_poses():
    cw, ch = 320, 380
    pad, lab, cols = 18, 34, 3
    rows = (len(POSES) + cols - 1) // cols
    W = cols * cw + (cols + 1) * pad
    H = rows * (ch + lab) + (rows + 1) * pad + 44
    sheet = Image.new('RGB', (W, H), PAGE_BG)
    d = ImageDraw.Draw(sheet)
    d.text((pad, 12), '同一个机器人 · 6 组姿态（动画就是把这些姿态之间的过渡帧烘出来）',
           font=font(21, True), fill=(238, 240, 245))
    for i, e in enumerate(POSES):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = 44 + pad + r * (ch + lab + pad)
        f = os.path.join(OUT, 'pose_%s.png' % e)
        if not os.path.exists(f):
            continue
        cell = flat(Image.open(f)).resize((cw, ch), Image.LANCZOS)
        sheet.paste(cell, (x, y))
        tw = d.textlength(e, font=font(18, True))
        d.text((x + (cw - tw) / 2, y + ch + 6), e, font=font(18, True), fill=(200, 208, 220))
    sheet.save(os.path.join(PREV, 'poses.png'))
    print('poses', sheet.size)


def do_device():
    """480x800 实机版面：导航栏 52 + HUD 108 + 画布 540 + 键位提示 100"""
    S = 2
    scr = Image.new('RGB', (480, 800), (0, 0, 0))

    hero = Image.open(os.path.join(OUT, 'hero.png')).convert('RGBA')
    sprite = hero.resize((320, 380), Image.LANCZOS)      # 设备上真实的宠物资材尺寸

    # 画布区底色（游戏页画布是 480x540 @ y=160）
    canvas_bg = Image.new('RGB', (480, 540), (8, 10, 14))
    scr.paste(canvas_bg, (0, 160))
    scr.paste(sprite, (80, 160 + 80), sprite)

    d = ImageDraw.Draw(scr)
    d.rectangle([0, 0, 479, 51], fill=(28, 28, 30))
    d.polygon([(22, 26), (34, 17), (34, 35)], fill=(210, 216, 226))
    t = '电子宠物'
    d.text(((480 - d.textlength(t, font=font(21, True))) / 2, 13), t, font=font(21, True), fill=(238, 240, 245))

    d.rectangle([0, 700, 479, 799], fill=(0, 0, 0))
    d.text((16, 716), '按「暂停键」摸头 · 连按两次换表情 · 长按返回列表',
           font=font(17), fill=(120, 132, 150))
    d.text((16, 748), '也可以直接点它：点头顶=摸头  点肚子=逗它  左右滑动=翻表情',
           font=font(17), fill=(96, 106, 122))

    scr = scr.resize((480 * S, 800 * S), Image.LANCZOS)
    scr.save(os.path.join(PREV, 'device.png'))
    print('device', scr.size)


if __name__ == '__main__':
    do_hero()
    do_faces()
    do_poses()
    do_device()
    sys.exit(0)
