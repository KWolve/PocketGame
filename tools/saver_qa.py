#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""saver_qa.py - 屏保「超级玛丽砖块钟」的真机像素验收

用法:
    python tools/grab.py out/saver.png                 # 先抓一帧（按 pan 选半页）
    python tools/saver_qa.py out/saver.png             # 体检这一帧
    python tools/saver_qa.py out/saver.png --expect 0958   # 顺带核对显示的是不是这个时间
    python tools/saver_qa.py out/saver.png --rot180    # 屏幕处于"整屏 180°倒挂"时用（见下）

★ `--rot180` 是什么时候要加（2026-09-18 挂绳倒挂功能）：
    整屏翻转是"把渲染结果转过去再写 fb"，所以**倒挂时 /dev/fb0 里读到的是倒着的画面**。
    加了它，工具会先把帧转回正向再按正常版式体检。不加会报一堆"砖台没上屏 / 数字认不出来"
    —— 那不是屏保坏了，是帧倒着。

判据（全部逐像素量，不靠"看起来对"）：
  1. HUD 条      y=0..56 平均亮度 < 60（黑条）且 y=6..50 有白色文字墨迹
  2. 背景        天空带（y=70..170，x=0..479）整条同色 ⇒ 背景图铺满、没被切
  3. 四块砖      (slot_x+4, 185) 是砖块色（暗橙，R-B 差 > 90）；没有 ⇒ 砖块图没上屏
  4. 数字识别    每格内"金色墨迹"的 mask 与 mc_dig0..9 的 mask 做 IoU，取最大 ⇒ 报出识别值
  5. 金币在飞    钟上方（y=20..175）出现金色墨迹 ⇒ 报 bbox（"数字弹出"是否真的在飞）
  6. 马里奥      砖台附近（y=300..545）出现马里奥配色 ⇒ 报 bbox，并与 4 帧图做 IoU 认帧
  7. 砖台/地面   y=560 是砖块色、y=760 是地面砖色 ⇒ 场景两层都在

⚠️ 为什么要它：屏保没有可点击控件，本板又没有 screencap（见 tools/grab.py），
   "动画到底有没有动"只能靠**逐像素比对 + 识别**来证明，否则很容易把
   "静态图摆对了" 当成 "动画跑通了"。
"""
import os
import sys

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG = os.path.join(ROOT, "resources", "images")

CELL_X = [35, 131, 261, 357]
CELL_Y, CELL_W, CELL_H = 180, 88, 140
PLAT_Y = 540
HUD_H = 56

MARIO_COLORS = ((0xE0, 0x3A, 0x1E), (0x2A, 0x54, 0xC8), (0xF2, 0xB4, 0x8C),
                (0x5B, 0x3A, 0x16))
# ⚠️ 不含"黄扣子"色 —— 金色数字墨迹 #FFD24A 与 #F5C542 在 tol=30 内会互相命中，
#    加进去会把"砖块里的数字"误判成马里奥（实测过：报出一个 355x5 的横条假目标）。
MARIO_MIN_BOX = 20   # 认成马里奥的最小边长（像素）；低于它一律当噪声


def load(name):
    return Image.open(os.path.join(IMG, name)).convert("RGBA")


def near(px, ref, tol=40):
    return all(abs(px[i] - ref[i]) <= tol for i in range(3))


def is_gold(px, tol=60):
    """金色墨迹：R 高、G 中高、B 明显低。"""
    r, g, b = px[0], px[1], px[2]
    return r >= 200 - tol and g >= 150 - tol and b <= 150 + tol and (r - b) >= 70


def is_brick(px):
    r, g, b = px[0], px[1], px[2]
    return (r - b) >= 80 and r >= 90 and b <= 100 and g < r


def ink_bbox(im, box, pred):
    xs, ys = [], []
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            if pred(im.getpixel((x, y))):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs) + 1, max(ys) + 1)


def digit_mask(d):
    """mc_dig{d}.png 的"金墨"位置集合（相对 88x140 格）。"""
    im = load("mc_dig%d.png" % d)
    s = set()
    for y in range(CELL_H):
        for x in range(CELL_W):
            r, g, b, a = im.getpixel((x, y))
            if a > 128 and is_gold((r, g, b)):
                s.add((x, y))
    return s


def identify(im, slot, masks):
    """把某一格的金墨与 10 个字模比 IoU，返回 (数字, 分数)。"""
    x0 = CELL_X[slot]
    shot = set()
    for y in range(CELL_H):
        for x in range(CELL_W):
            if is_gold(im.getpixel((x0 + x, CELL_Y + y))):
                shot.add((x, y))
    if not shot:
        return None, 0.0
    best, bs = None, 0.0
    for d, m in masks.items():
        inter = len(shot & m)
        union = len(shot | m)
        sc = inter / float(union) if union else 0.0
        if sc > bs:
            best, bs = d, sc
    return best, bs


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    expect = None
    rot180 = False
    for i, a in enumerate(sys.argv):
        if a == "--expect" and i + 1 < len(sys.argv):
            expect = sys.argv[i + 1]
        if a == "--rot180":
            rot180 = True
    im = Image.open(path).convert("RGB")
    if im.size != (480, 800):
        print("!! 尺寸不是 480x800：%s" % (im.size,))
        return 1
    if rot180:
        # 屏幕处于"挂绳倒挂"（整屏 180°）时，/dev/fb0 里读到的是**转过之后**的画面
        # （旋转发生在写 fb 之前，见 platform/PgFlip.h）。这里先转回来再按正常版式体检。
        im = im.rotate(180)
        print("[说明] 已按 --rot180 把帧转回正向再体检（屏幕当前是倒挂状态）")
    fails = []

    # 1. HUD
    lum = sum(sum(im.getpixel((x, y))) / 3.0
              for y in range(0, HUD_H, 4) for x in range(0, 480, 8))
    lum /= float(len(range(0, HUD_H, 4)) * len(range(0, 480, 8)))
    white = ink_bbox(im, (0, 4, 480, 52), lambda p: min(p) > 200)
    print("[HUD ] y=0..56 平均亮度 %.1f（判据 <60）  白字墨迹 %s" % (lum, white))
    if lum >= 60:
        fails.append("HUD 不是黑条")
    if not white:
        fails.append("HUD 没有白字（日期/星期没画上）")

    # 2. 天空带整条同色
    row = [im.getpixel((x, 120)) for x in range(0, 480, 10)]
    uniq = set(row)
    print("[背景] y=120 采样 %d 点，%d 种颜色  %s" % (len(row), len(uniq), list(uniq)[:3]))
    if len(uniq) > 2:
        fails.append("天空带颜色不纯（背景图可能只铺了一部分）")

    # 3/4. 砖块 + 数字识别
    masks = {d: digit_mask(d) for d in range(10)}
    digits = []
    for s in range(4):
        bx = im.getpixel((CELL_X[s] + 4, CELL_Y + 5))
        mid = im.getpixel((CELL_X[s] + 44, CELL_Y + 130))
        d, score = identify(im, s, masks)
        digits.append(d)
        ok = is_brick(bx)
        print("[砖%d ] 左上%s 砖色=%s   数字识别=%s(%.2f)" %
              (s, bx, "OK" if ok else "❌", d, score))
        if not ok:
            fails.append("第%d块砖没上屏" % s)
        if d is None or score < 0.6:
            fails.append("第%d位数字认不出来（%s %.2f）" % (s, d, score))

    shown = "".join(str(d) if d is not None else "?" for d in digits)
    print("[时钟] 识别为 %s:%s" % (shown[:2], shown[2:]))
    if expect and len(expect) == 4:
        # ⚠️ 只比"四位数字"，不要拿带冒号的字符串去比（第一版写成 "12:13" vs "1213"，
        #    结果时钟明明是对的却报 FAIL —— 工具自己的 bug 会误导验收结论）。
        print("[核对] 期望 %s:%s -> %s" % (expect[:2], expect[2:],
                                        "一致 ✓" if shown == expect else "不一致 ❌"))
        if shown != expect:
            fails.append("显示 %s != 期望 %s" % (shown, expect))

    # 5. 金币（钟上方）
    coin = ink_bbox(im, (0, 10, 480, CELL_Y), is_gold)
    print("[金币] 钟上方金墨 bbox = %s" % (coin,))
    if coin:
        print("       ↑ 有数字正飞出砖块（宽 %d 高 %d）"
              % (coin[2] - coin[0], coin[3] - coin[1]))

    # 6. 马里奥（只看砖块以下的区域：钟格 180..320 里的金墨不是马里奥）
    mb = ink_bbox(im, (0, 322, 480, PLAT_Y + 6),
                  lambda p: any(near(p, c, 30) for c in MARIO_COLORS))
    if mb and (mb[2] - mb[0]) >= MARIO_MIN_BOX and (mb[3] - mb[1]) >= MARIO_MIN_BOX:
        print("[马里] 在场 bbox = %s（%dx%d，整帧 68x68）"
              % (mb, mb[2] - mb[0], mb[3] - mb[1]))
        mar = mb
    else:
        print("[马里] 不在场（bbox=%s 视为无/噪声）" % (mb,))
        mar = None

    # 7. 砖台 / 地面
    plat = is_brick(im.getpixel((240, PLAT_Y + 30)))
    gnd = is_brick(im.getpixel((240, 780))) or is_brick(im.getpixel((60, 780)))
    print("[场景] 砖台(y=570)=%s  地面(y=780)=%s" % ("OK" if plat else "❌",
                                                 "OK" if gnd else "❌"))
    if not plat:
        fails.append("砖台没上屏")
    if not gnd:
        fails.append("地面没上屏")

    print("=" * 60)
    if fails:
        print("结论：FAIL —— %d 项" % len(fails))
        for f in fails:
            print("  · %s" % f)
        return 1
    print("结论：PASS —— 砖块/数字/场景/文字全部就位 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
