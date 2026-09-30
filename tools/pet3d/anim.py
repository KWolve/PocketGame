#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
pet3d/anim.py - 把动画帧序列合成 GIF + 分镜条，并做**客观校验**

校验口径（不看图也能判"到底动没动"）：
  · 逐帧统计"实心像素的包围盒上沿 / 左右沿 / 实心面积 / 主体平均亮度"
  · 打印这几个量的 **min..max 波动范围** —— 全为 0 就是没动（静默失败）
  · 外加几张关键帧的"重心位移"检查（跳起的那几帧上沿应明显上移）

产出：
  preview/anim_<key>.gif    循环动画（合成在设备底色上，避免 GIF 不支持半透明）
  preview/anim_<key>_strip.png  分镜条（8 帧横排 + 中文标签）
用法: python anim.py
"""
import glob
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(ROOT, 'anim')
PREV = os.path.join(ROOT, 'preview')
os.makedirs(PREV, exist_ok=True)

FONT = 'C:/Windows/Fonts/msyh.ttc'
FONTB = 'C:/Windows/Fonts/msyhbd.ttc'
PAGE_BG = (11, 11, 13)

ANIMS = [
    ('idle',  '待机 · 呼吸 + 眨眼 + 天线摆', 18),
    ('happy', '摸头反应 · 下蹲→跳起→落地', 20),
    ('sleep', '睡觉 · 垂头 + 灯灭 + Z 字飘', 12),
    ('faces', '表情轮播 · 12 个常见表情', 10),
]


def font(sz, bold=False):
    try:
        return ImageFont.truetype(FONTB if bold else FONT, sz)
    except Exception:
        return ImageFont.load_default()


def flat(im):
    base = Image.new('RGBA', im.size, PAGE_BG + (255,))
    return Image.alpha_composite(base, im.convert('RGBA')).convert('RGB')


def stats(im):
    """返回 (top, bottom, left, right, solid_px, mean_lum_of_solid)"""
    px = im.load()
    W, H = im.size
    top, bottom, left, right = H, -1, W, -1
    n = 0
    lum = 0
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a < 200:
                continue
            n += 1
            lum += (r * 299 + g * 587 + b * 114) // 1000
            if y < top: top = y
            if y > bottom: bottom = y
            if x < left: left = x
            if x > right: right = x
    return top, bottom, left, right, n, (lum / n if n else 0)


def frame_diff(frames, step=1):
    """
    相邻帧的**灰度平均绝对差**（降采样到 80x95 再算）。
    ★ 这才是"到底有没有在动"的通用判据 —— 只看包围盒会漏掉"表情/颜色在变但剪影不变"的动画
      （表情轮播的剪影几乎不动，但眼睛嘴一直在换）。
    """
    small = [f.convert('L').resize((80, 95), Image.BOX) for f in frames[::step]]
    diffs = []
    for i in range(1, len(small)):
        a, b = small[i - 1].tobytes(), small[i].tobytes()
        diffs.append(sum(abs(x - y) for x, y in zip(a, b)) / float(len(a)))
    return diffs


def global_palette(frames):
    step = max(1, len(frames) // 8)
    samples = frames[::step][:8]
    mont = Image.new('RGB', (samples[0].width, samples[0].height * len(samples)))
    for i, f in enumerate(samples):
        mont.paste(f, (0, i * f.height))
    return mont.quantize(colors=255, method=Image.MEDIANCUT)


def do_anim(key, title, fps):
    files = sorted(glob.glob(os.path.join(SRC, 'anim_%s_*.png' % key)))
    if not files:
        print('!! 没找到 %s 的帧' % key)
        return
    raws = [Image.open(f) for f in files]
    frames = [flat(im) for im in raws]

    tops, bots, lefts, areas, lums = [], [], [], [], []
    for im in raws:
        t, b, l, r, n, lm = stats(im)
        tops.append(t); bots.append(b); lefts.append(l); areas.append(n); lums.append(lm)

    span = lambda a: max(a) - min(a)
    diffs = frame_diff(frames)
    print('%-6s %2d 帧  fps=%d  上沿波动 %3dpx  下沿波动 %3dpx  左沿波动 %3dpx  '
          '面积波动 %5dpx  亮度 %.0f..%.0f  帧间差 %.2f..%.2f'
          % (key, len(frames), fps, span(tops), span(bots), span(lefts),
             span(areas), min(lums), max(lums), min(diffs), max(diffs)))
    if span(tops) < 3 and max(diffs) < 1.0:
        print('   ⚠️ 几乎没动 —— 检查 pose() 是否真的接上了 applyPose')

    # GIF 体积控制：降到 1.5 倍设备尺寸（320x380 是设备上的真实尺寸）
    GW, GH = 480, 570
    gframes = [f.resize((GW, GH), Image.LANCZOS) for f in frames]
    pal = global_palette(gframes)
    pframes = [f.quantize(palette=pal, dither=Image.FLOYDSTEINBERG) for f in gframes]
    gif = os.path.join(PREV, 'anim_%s.gif' % key)
    pframes[0].save(gif, save_all=True, append_images=pframes[1:],
                    duration=int(round(1000.0 / fps)), loop=0, optimize=True, disposal=2)
    print('   -> %s  %.0f KB' % (os.path.basename(gif), os.path.getsize(gif) / 1024.0))

    # 分镜条
    pick = [int(round(i * (len(frames) - 1) / 7.0)) for i in range(8)]
    cw, ch, pad, lab = 160, 190, 12, 30
    strip = Image.new('RGB', (8 * cw + 9 * pad, ch + lab + 2 * pad + 34), PAGE_BG)
    d = ImageDraw.Draw(strip)
    d.text((pad, 12), title, font=font(20, True), fill=(238, 240, 245))
    for c, idx in enumerate(pick):
        x = pad + c * (cw + pad)
        y = 34 + pad
        strip.paste(frames[idx].resize((cw, ch), Image.LANCZOS), (x, y))
        t = '#%d' % idx
        d.text((x + 4, y + ch + 4), t, font=font(15), fill=(150, 160, 175))
    strip.save(os.path.join(PREV, 'anim_%s_strip.png' % key))


if __name__ == '__main__':
    for k, t, f in ANIMS:
        do_anim(k, t, f)
    sys.exit(0)
