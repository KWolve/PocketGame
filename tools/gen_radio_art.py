#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_radio_art.py - 网络收音机**播放页**的素材（1:1 硬边、烘底，不拉伸）

为什么单独一个生成器：这批图有一套自己的"语义"（暖橙渐变 + 黑底 + 圆盘），
塞进 gen_ui.py 会和 iOS 主题那套（`ios_*`/`app_*`）混在一起。

工程铁律（都体现在下面的实现里）：
  · **图尺寸必须 == 控件盒尺寸**（`tools/check_stretch.py` 会抓）——
    频谱柱故意例外：它的初始盒就是 12x132，运行时**只改高度**（等于"同一张图按盒缩放"，
    属于"运行时改尺寸"那一类，检查器不覆盖；见文件末尾注释）。
  · **烘底**：圆角/圆弧的羽化像素要与"它坐的那张底"匹配，这里全是**页面黑底**（#000000）
    ⇒ 四角烘黑，屏幕上无缝（`tools/check_assets.py` 第 ④⑤ 项）。
  · 抗锯齿用 **4x 超采样 + LANCZOS 缩回**，不用高斯羽化（会把边缘抹开 3~4px）。
  · 细线不参与圆角（<24px 的条直接直角）。

产出（resources/images/）：
  rp_bar_12x132.png      频谱柱（竖渐变，圆角顶）
  rp_disc_240.png        大圆盘（黑胶片 + 暖橙标签 + 音符）
  rp_floor_456x12.png    频谱基线（刻度）
  rp_level_320x8.png     总电平条（运行时改宽度）
  rp_btn_prev_104.png / rp_btn_stop_104.png / rp_btn_next_104.png   圆形控制键
"""
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "resources", "images")

PAGE = (0, 0, 0, 255)          # 页面黑底（所有图都烘它）
C_DEEP = (242, 84, 45)         # 深橙（柱底）
C_MID = (255, 122, 69)         # 主题橙
C_TOP = (255, 206, 74)         # 琥珀（柱顶）
C_INK = (242, 242, 247)        # 前景白
C_DIM = (48, 48, 52)
C_BODY = (18, 18, 20)
SS = 4                         # 超采样倍数


def _lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def vgrad(w, h, stops):
    """竖直线性渐变（stops = [(0.0, color), ...]，0 在上）。"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        for i in range(len(stops) - 1):
            t0, c0 = stops[i]
            t1, c1 = stops[i + 1]
            if t0 <= t <= t1:
                k = 0.0 if t1 == t0 else (t - t0) / (t1 - t0)
                px_col = _lerp(c0, c1, k)
                break
        else:
            px_col = stops[-1][1]
        for x in range(w):
            px[x, y] = px_col
    return img


def rounded_mask(w, h, radius, corners="all"):
    big = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(big)
    r = radius * SS
    if corners == "top":
        d.rectangle([0, r, w * SS - 1, h * SS - 1], fill=255)
        d.rectangle([0, 0, r, h * SS - 1], fill=255)
        d.rectangle([w * SS - 1 - r, 0, w * SS - 1, h * SS - 1], fill=255)
        d.ellipse([0, 0, 2 * r, 2 * r], fill=255)
        d.ellipse([w * SS - 1 - 2 * r, 0, w * SS - 1, 2 * r], fill=255)
    else:
        d.rounded_rectangle([0, 0, w * SS - 1, h * SS - 1], radius=r, fill=255)
    return big.resize((w, h), Image.BOX)


def make_bar(w=12, h=132, radius=6):
    """频谱柱：底部深橙 → 顶部琥珀，圆角顶，四角烘页面黑。"""
    grad = vgrad(w, h, [(0.0, C_TOP), (0.45, C_MID), (1.0, C_DEEP)])
    img = Image.new("RGBA", (w, h), PAGE)
    img.paste(grad, (0, 0), rounded_mask(w, h, radius, "top"))
    # 顶部再压一道亮边（"灯管"感），只有 2px，不参与圆角
    d = ImageDraw.Draw(img)
    d.rectangle([2, 0, w - 3, 1], fill=C_TOP + (255,))
    return img


def make_disc(size=240):
    """大圆盘：黑胶片 + 同心纹 + 暖橙标签 + 白音符。"""
    img = Image.new("RGBA", (size, size), PAGE)
    big = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    c = size * SS / 2.0
    # 外圈光晕（3 层，越外越淡）——烘进黑底，所以直接叠色不用 alpha
    for i, (rr, col) in enumerate([((c - 2 * SS), (58, 26, 16)),
                                   ((c - 5 * SS), (34, 18, 12)),
                                   ((c - 8 * SS), (22, 13, 10))]):
        d.ellipse([c - rr, c - rr, c + rr, c + rr], fill=col + (255,))
    # 盘体
    r_body = c - 11 * SS
    d.ellipse([c - r_body, c - r_body, c + r_body, c + r_body], fill=C_BODY + (255,))
    # 同心纹（唱片沟槽）
    r = r_body - 6 * SS
    while r > c * 0.42:
        d.ellipse([c - r, c - r, c + r, c + r], outline=(30, 30, 34, 255), width=max(1, SS // 2))
        r -= 7 * SS
    # 标签盘（暖橙渐变：这里用"上亮下深"的两段圆做近似）
    r_lab = c * 0.40
    lab = vgrad(size, size, [(0.0, C_TOP), (0.5, C_MID), (1.0, C_DEEP)]).convert("RGBA")
    mask = Image.new("L", (size * SS, size * SS), 0)
    ImageDraw.Draw(mask).ellipse([c - r_lab, c - r_lab, c + r_lab, c + r_lab], fill=255)
    lab = lab.resize((size * SS, size * SS), Image.LANCZOS)
    big.paste(lab, (0, 0), mask)
    # 音符（白）：符头 + 符干 + 符尾
    hx, hy = c - 0.10 * c, c + 0.14 * c
    hr = 0.105 * c
    d.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=C_INK + (255,))
    lw = max(2, int(0.035 * c))
    d.rectangle([hx + hr - lw, hy - 0.42 * c, hx + hr, hy], fill=C_INK + (255,))
    d.polygon([(hx + hr, hy - 0.42 * c), (hx + hr + 0.26 * c, hy - 0.28 * c),
               (hx + hr + 0.26 * c, hy - 0.16 * c), (hx + hr, hy - 0.30 * c)],
              fill=C_INK + (255,))
    img.alpha_composite(big.resize((size, size), Image.LANCZOS))
    return img


def make_floor(w=456, h=12, step=16, x0=0):
    """频谱基线：一条 2px 底线 + 每根柱中心一根刻度。"""
    img = Image.new("RGBA", (w, h), PAGE)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, 1], fill=(44, 44, 48, 255))
    x = x0
    while x < w:
        d.rectangle([x, 0, x, 5], fill=C_DIM + (255,))
        x += step
    # 中线压一道暖色（整屏看是"频谱的底"）
    d.rectangle([0, 2, w - 1, 2], fill=(70, 34, 22, 255))
    return img


def make_level(w=320, h=8, radius=4):
    """总电平条：横向渐变（运行时按电平改宽度）。"""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for x in range(w):
        t = x / max(1, w - 1)
        c = _lerp(C_DEEP, C_TOP, t)
        for y in range(h):
            px[x, y] = c
    out = Image.new("RGBA", (w, h), PAGE)
    out.paste(img, (0, 0), rounded_mask(w, h, radius))
    return out


def make_ctrl_btn(size=104, kind="prev"):
    """圆形控制键（图 = 整个按钮面，含图形；运行时不做任何拼接）。"""
    img = Image.new("RGBA", (size, size), PAGE)
    big = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    c = size * SS / 2.0
    r = c - 2 * SS
    ring = C_MID if kind == "stop" else (54, 54, 58)
    d.ellipse([c - r, c - r, c + r, c + r], fill=(22, 22, 24, 255),
              outline=ring + (255,), width=2 * SS)
    ink = C_INK + (255,)
    half = 0.17 * size * SS
    cy = c
    if kind == "prev":
        d.rectangle([c - 0.30 * size * SS, cy - half, c - 0.30 * size * SS + 0.055 * size * SS,
                     cy + half], fill=ink)
        d.polygon([(c + 0.30 * size * SS, cy - half), (c + 0.30 * size * SS, cy + half),
                   (c - 0.20 * size * SS, cy)], fill=ink)
    elif kind == "next":
        d.rectangle([c + 0.30 * size * SS - 0.055 * size * SS, cy - half,
                     c + 0.30 * size * SS, cy + half], fill=ink)
        d.polygon([(c - 0.30 * size * SS, cy - half), (c - 0.30 * size * SS, cy + half),
                   (c + 0.20 * size * SS, cy)], fill=ink)
    else:  # stop
        s = 0.16 * size * SS
        d.rounded_rectangle([c - s, cy - s, c + s, cy + s], radius=int(0.03 * size * SS),
                            fill=C_MID + (255,))
    img.alpha_composite(big.resize((size, size), Image.LANCZOS))
    return img


def save(img, name):
    p = os.path.join(OUT, name)
    img.convert("RGBA").save(p)
    print("  %-26s %dx%d  %d B" % (name, img.size[0], img.size[1], os.path.getsize(p)))


def main():
    os.makedirs(OUT, exist_ok=True)
    print("生成网络收音机播放页素材 -> %s" % OUT)
    save(make_bar(12, 132), "rp_bar_12x132.png")
    save(make_disc(240), "rp_disc_240.png")
    save(make_floor(456, 12), "rp_floor_456x12.png")
    save(make_level(320, 8), "rp_level_320x8.png")
    save(make_ctrl_btn(104, "prev"), "rp_btn_prev_104.png")
    save(make_ctrl_btn(104, "stop"), "rp_btn_stop_104.png")
    save(make_ctrl_btn(104, "next"), "rp_btn_next_104.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
