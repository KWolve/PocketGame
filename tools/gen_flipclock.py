# -*- coding: utf-8 -*-
"""生成屏保翻页钟的卡片图（每位数字 = 上半张 + 下半张）。

为什么把数字画进图片、而不直接用矢量字控件：
    翻页效果需要把数字**切成上下两半**分别折叠。原生控件没有"裁剪文本"的能力
    （ZKTextView 只能整字居中/对齐），而图片天然可切 —— 把整卡画好再 crop 两半即可。
    折叠动画用 `setPosition()` 压缩控件高度实现：上半张 y 下移 + 高度归零 = "折下去"。

    ⚠️ 这不违反「非游戏禁止自定义绘图」铁律：产物是**静态图片资源**，
       运行时是**原生控件**（btn + 背景图 + setPosition），没有一行画布绘制代码。

输出（resources/images/）：
    flip_u{d}.png   上半张（88x70），d = 0..9
    flip_d{d}.png   下半张（88x70）
    flip_colon_on.png / flip_colon_off.png   冒号（闪烁两态）

用法：python tools/gen_flipclock.py
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

# ---- 尺寸与配色（与 ui/screensaver.html 必须一致）----
CARD_W, CARD_H = 88, 140
HALF = CARD_H // 2           # 70
RADIUS = 12
SS = 4                       # 超采样倍数（先画大图再缩，边缘无锯齿）

NUM_H = 100                  # 数字墨迹目标高度（跨中线，上下各显示 50px）

# 卡片底色：上亮下暗（光源在上），形成翻牌钟的立体感
UP_TOP = (0x26, 0x33, 0x44)   # 上半张顶部
UP_BOT = (0x1A, 0x24, 0x31)   # 上半张底部（靠近中轴）
DN_TOP = (0x19, 0x23, 0x30)   # 下半张顶部（靠近中轴）
DN_BOT = (0x13, 0x1B, 0x24)   # 下半张底部
FG = (0xF0, 0xF4, 0xF8)       # 数字
SHADOW = (0x07, 0x0B, 0x10)   # 数字投影
SEAM = (0x08, 0x0C, 0x11)     # 中缝（上半张底 3px）
AXIS_HI = (0x35, 0x48, 0x60)  # 中轴高光（下半张顶 1px）
EDGE_HI = (0x30, 0x42, 0x58)  # 上半张顶边高光 1px
COLON_ON = (0xF2, 0xB3, 0x3D)  # 冒号（琥珀）
COLON_OFF = (0x3A, 0x46, 0x55)  # 冒号暗态

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\consolab.ttf",   # Consolas Bold（数字等宽、饱满）
    r"C:\Windows\Fonts\consola.ttf",
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\simhei.ttf",
]

OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "resources", "images")


def pick_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("找不到可用字体")


def fit_font(path, text="0123456789", target_h=NUM_H, lo=20, hi=220):
    """二分找到墨迹高度最接近 target_h 的字号。"""
    best = None
    for _ in range(24):
        mid = (lo + hi) // 2
        f = ImageFont.truetype(path, mid)
        img = Image.new("L", (600, 400), 0)
        d = ImageDraw.Draw(img)
        l, t, r, b = d.textbbox((0, 0), text, font=f)
        h = b - t
        if best is None or abs(h - target_h) < abs(best[1] - target_h):
            best = (mid, h)
        if h < target_h:
            lo = mid + 1
        else:
            hi = mid - 1
        if lo > hi:
            break
    return best[0]


def vgrad(w, h, top, bot):
    """垂直渐变（RGB）。"""
    img = Image.new("RGB", (1, h))
    px = img.load()
    for y in range(h):
        k = y / float(max(h - 1, 1))
        px[0, y] = tuple(int(round(top[i] + (bot[i] - top[i]) * k)) for i in range(3))
    return img.resize((w, h), Image.BOX)


def round_mask(w, h, radius):
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    return img


def make_card(font, ch):
    """画一整张卡（含数字），返回 RGB 图。超采样后缩小。"""
    W, H = CARD_W * SS, CARD_H * SS
    R = RADIUS * SS

    # 底色：上半 + 下半 各一段渐变（中轴处最深，突出"两张卡叠着"的观感）
    up = vgrad(W, H // 2, UP_TOP, UP_BOT)
    dn = vgrad(W, H // 2, DN_TOP, DN_BOT)
    card = Image.new("RGB", (W, H))
    card.paste(up, (0, 0))
    card.paste(dn, (0, H // 2))

    d = ImageDraw.Draw(card)

    # 数字（居中，跨中线）：先画投影再画本体
    bbox = d.textbbox((0, 0), ch, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    tx = (W - tw) // 2 - bbox[0]
    ty = (H - th) // 2 - bbox[1]
    d.text((tx + 2 * SS, ty + 3 * SS), ch, font=font, fill=SHADOW)
    d.text((tx, ty), ch, font=font, fill=FG)

    # 上半张顶边高光 + 中缝；下半张中轴高光
    d.rectangle([0, 0, W - 1, SS - 1], fill=EDGE_HI)
    d.rectangle([0, H // 2 - 3 * SS, W - 1, H // 2 - 1], fill=SEAM)
    d.rectangle([0, H // 2, W - 1, H // 2 + SS - 1], fill=AXIS_HI)

    # 圆角
    card.putalpha(round_mask(W, H, R))
    card = card.resize((CARD_W, CARD_H), Image.BOX)
    return card


def make_colon(on):
    """冒号图：两块圆点（宽 26，高 140，与卡片同高）。"""
    W, H = 26, CARD_H
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = COLON_ON if on else COLON_OFF
    r = 7 * SS
    cx = W * SS // 2
    for cy in (H * SS // 2 - 26 * SS, H * SS // 2 + 26 * SS):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=c)
    return img.resize((W, H), Image.BOX)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    font_path = pick_font()
    size = fit_font(font_path)
    font = ImageFont.truetype(font_path, size * SS)

    print("字体 %s  字号 %d（超采样 %dx）" % (os.path.basename(font_path), size, SS))
    bad = 0
    for i in range(10):
        card = make_card(font, str(i))
        up = card.crop((0, 0, CARD_W, HALF))
        dn = card.crop((0, HALF, CARD_W, CARD_H))
        up.save(os.path.join(OUT_DIR, "flip_u%d.png" % i))
        dn.save(os.path.join(OUT_DIR, "flip_d%d.png" % i))

        # 自检：数字必须真的跨过中线（上半张底部、下半张顶部都要有墨迹）
        ua = up.convert("L").point(lambda v: 255 if v > 200 else 0)
        da = dn.convert("L").point(lambda v: 255 if v > 200 else 0)
        # 只看中部 40px 宽（避开卡片圆角/边缘高光）
        ub = ua.crop((24, HALF - 12, CARD_W - 24, HALF)).getbbox()
        db = da.crop((24, 0, CARD_W - 24, 12)).getbbox()
        if not ub or not db:
            print("  !! %d 未跨中线（上=%s 下=%s）" % (i, ub, db))
            bad += 1
        print("  flip_u%d / flip_d%d  %dx%d" % (i, i, CARD_W, HALF))

    make_colon(True).save(os.path.join(OUT_DIR, "flip_colon_on.png"))
    make_colon(False).save(os.path.join(OUT_DIR, "flip_colon_off.png"))
    print("  flip_colon_on / flip_colon_off  26x%d" % CARD_H)

    print("共 %d 张卡片图 + 2 张冒号图，自检%s" % (20, "失败 %d 个" % bad if bad else "通过"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
