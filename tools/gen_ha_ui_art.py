#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_ha_ui_art.py —— 智能家居（ha.ftu）交互改版需要的 3 张素材

为什么需要（2026-09-23 落地 ui/ha.preview.html 设计稿时发现）：
  ① **勾图标必须用图**：字库（font/pocketgame.ttf，4,209 个字形）里
     ✓(U+2713)/✔(U+2714)/√(U+221A) **全都没有字形** ⇒ 直接用字符会**整字消失**。
     （彩蛋：●(U+25CF) 有字形但在 html2json 黑名单里，也不能用；最后圆点用了 •(U+2022)。）
  ② **半透明遮罩必须用带 alpha 的 PNG**：菜单/确认弹层的压暗靠它。
     ⚠️ 运行时 `setBackgroundPic` **不保留 alpha**（PgSkin.h 血案），
        所以这类图只能在源稿里静态挂（data-pic0/data-pic1 或 data-bgpic），
        不能在代码里动态换。

产物（写进 resources/images/）：
    ha_done.png      96x96   绿圆 + 白勾（向导③完成页）
    sheet_mask.png   480x748 黑 alpha=204（半屏菜单的压暗层 = 80% 不透明）
    dialog_mask.png  480x748 黑 alpha=204（确认对话框的压暗层，同一档）

★ 2026-09-24 用户口径：「弹框全屏半透 80% 的透明度遮住」+「背景的列表让它在那里显示」
  ⇒ 遮罩 480x748（**导航栏之下满屏**）× **alpha=204（不透明度 80%）**。
  配合"弹层改 modal + 窗口透明"，效果 = 背景的我的设备列表被压暗到 20%，
  还能看清轮廓，前景的菜单/确认框完整显示。
  ⚠️ 想改浓淡只动这一个数：80% 透明（列表很清楚）= 51；折中 = 128；全遮 = 255。
    ha_domchip_64.png  64x64 域标识底块（我的设备 listview 的行内）
    ha_domchip_40.png  40x40 域标识底块（添加向导选择页的行内）
    ha_card_my.png   224x145 我的设备·卡片底（1:1，圆角外烘列表黑底；行距 12）
    ha_row_pick.png  456x72  选择设备·行底（1:1，同上）

★★ 2026-09-24 补：卡片/行底为什么必须换成 **1:1 非九宫格** PNG（用户报"列表背面有两条横线"）：
   原来挂的是 `ios_btn_dark_r20.9.png` / `ios_card_row.9.png`（经 `data-bgpic`）。
   `.9.png` 的**最外 1px 是九宫格 marker**，而这两张图的 marker 是**不透明黑 (0,0,0,255)**；
   经 `data-bgpic` 走的是普通贴图路径、**不剥 marker** ⇒ 真机上每张卡片四周多出一圈
   1px 黑边（实测 x=12 / x=235 是 (0,0,0)，卡片本体从 x=13 起）。
   两张卡片上下相邻时，两个 marker 行叠成 **2px 黑缝** —— 那就是用户看到的"横线"
   （原来还叠加了 8px 的行距 ⇒ 10px 黑带）。
   而"素材 1:1、不拉伸"本来就是本工程的规矩 ⇒ 直接出**和 item 等尺寸**的普通 PNG，
   圆角外烘列表底色（黑），marker 问题、拉伸问题、黑角问题一起消失。

★★ 2026-09-23 补：域标识底块为什么必须是**不透明 + 四角烘行底色**（用户报"图标角落都是黑的"）：
   底块原来是圆角九宫格（透明四角），而 `inject_rounded` 挂图后会把 `bgColorTab` 清成 -1
   ⇒ 四角透出的是**窗口黑底**，而底块坐在 #1C1C1E 的卡片/行上 ⇒ **四个黑角**（真机实测）。
   而 listview 的 subItem 又不能挂圆角图 ⇒ 正解 = 一张**不透明**的 1:1 PNG，
   圆角外直接烘上行底色（CARD_BG）。所有行共用同一张 ⇒ 不违反"subItem 图片不按行区分"。
   ⚠️ 行底色/卡片底色/卡片尺寸一变，这里必须跟着改并重跑（CARD_BG 是唯一真值）。
   尺寸跟着 `data-row-spacing` 走：行距 r ⇒ item 高 = (列表高 - (行数-1)*r) / 行数。

自检：尺寸 / alpha 通道 / 关键像素（圆心是绿的、勾上是白的、遮罩四角是半透明黑）。
      跑完打印 PASS / FAIL，失败非零退出（别让"图不对"留到真机上才发现）。
"""
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "resources", "images")

GREEN = (48, 209, 88, 255)
WHITE = (255, 255, 255, 255)
BLACK = (0, 0, 0, 255)

# ★ 唯一真值：行/卡片底色。域标识底块的**四角**填它，才能"骗过眼睛"当成圆角块。
#   改 ui/ha.html 里卡片底（或选择页行底 ios_card_row）时，这里必须同步改 + 重跑。
CARD_BG = (28, 28, 30)      # #1C1C1E（卡片/行本体色）
LIST_BG = (0, 0, 0)         # 列表与窗口的底色（圆角外烘它）
CHIP_BG = (44, 44, 46)      # #2C2C2E 底块本体


def make_card_bg(w, h, radius):
    """1:1 卡片/行底图：**不透明 RGB**，圆角外直接烘列表底色（黑）。

    ⚠️ 一定不要用 `.9.png` + `data-bgpic`：那条路**不剥 marker**，
       而这两张老素材的 marker 是**不透明黑** ⇒ 每张卡片会多出一圈 1px 黑边。
    ⚠️ 也不要留 alpha：本工程实测"运行时换图会丢 alpha、透明处变白块"，
       静态挂图虽然没这个问题，但统一成不透明更省心（四角本来就是黑的）。
    """
    ss = 4
    big = Image.new("RGBA", (w * ss, h * ss), LIST_BG + (255,))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((0, 0, w * ss - 1, h * ss - 1), radius=radius * ss, fill=CARD_BG + (255,))
    return big.resize((w, h), Image.BOX).convert("RGB")


def make_domchip(size, radius):
    """域标识底块：**不透明**圆角方块（四角烘行底色）。

    ⚠️ 一定要 RGB（不带 alpha）：带 alpha 的图在运行时换图会露白块，
       静态挂图时四角又会透出窗口黑底 —— 两种都是"黑/白角"事故。
    """
    ss = 4
    big = Image.new("RGBA", (size * ss, size * ss), CARD_BG + (255,))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle((0, 0, size * ss - 1, size * ss - 1), radius=radius * ss, fill=CHIP_BG + (255,))
    return big.resize((size, size), Image.BOX).convert("RGB")


def make_done_tick():
    """96x96 绿圆 + 白色勾。"""
    im = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((0, 0, 95, 95), fill=GREEN)
    # 勾：两段粗线，joint="curve" 让拐角圆滑（不然像个折角符号）
    d.line([(26, 50), (42, 66), (70, 32)], fill=WHITE, width=9, joint="curve")
    # 线端点补圆（PIL 的 line 端点是平的）
    for cx, cy in ((26, 50), (42, 66), (70, 32)):
        d.ellipse((cx - 4, cy - 4, cx + 4, cy + 4), fill=WHITE)
    return im


MASK_ALPHA = 204      # 80% 不透明（见文件头说明；改浓淡只动这个数）


def make_mask(alpha):
    # ⚠️ 高度 748（不是 800）：整屏窗口顶部 52px 是**常显导航栏**，
    #    遮罩盖到那里会被 gen_ui 的 SLIVER 检查判为"被浮层切成半截"。
    #    从 y=52 起铺，视觉无差别（导航栏本来就压在最上层）。
    return Image.new("RGBA", (480, 748), (0, 0, 0, alpha))


def check(path, expect_size, test):
    im = Image.open(path).convert("RGBA")
    ok = im.size == expect_size
    msg = "%s %s" % ("OK " if ok else "**FAIL**", os.path.basename(path))
    if not ok:
        print("   %s 尺寸 %s != 期望 %s" % (msg, im.size, expect_size))
    else:
        detail = test(im)
        print("   %s 尺寸 %s %s" % (msg, im.size, detail))
    return ok


def main():
    os.makedirs(OUT, exist_ok=True)
    print("生成 ha.ftu 交互改版素材 -> %s" % OUT)

    p_done = os.path.join(OUT, "ha_done.png")
    p_chip64 = os.path.join(OUT, "ha_domchip_64.png")
    p_chip40 = os.path.join(OUT, "ha_domchip_40.png")
    p_cardmy = os.path.join(OUT, "ha_card_my.png")
    p_rowpick = os.path.join(OUT, "ha_row_pick.png")
    make_done_tick().save(p_done)
    p_sheet = os.path.join(OUT, "sheet_mask.png")
    make_mask(MASK_ALPHA).save(p_sheet)
    p_dialog = os.path.join(OUT, "dialog_mask.png")
    make_mask(MASK_ALPHA).save(p_dialog)
    make_card_bg(224, 145, 20).save(p_cardmy)     # 我的设备：item 224x145（行距 12 ⇒ (616-36)/4）
    make_card_bg(456, 72, 20).save(p_rowpick)     # 选择设备：item 456x72
    make_domchip(64, 14).save(p_chip64)
    make_domchip(40, 9).save(p_chip40)

    print("自检：")
    n_ok = 0
    n_ok += check(p_done, (96, 96), lambda im: "圆心=%s 勾上=%s" % (
        im.getpixel((20, 48)),                     # 圆内左侧（非勾）
        im.getpixel((42, 66))))                    # 勾拐点
    for p, a in ((p_sheet, MASK_ALPHA), (p_dialog, MASK_ALPHA)):
        n_ok += check(p, (480, 748), lambda im, a=a: "四角 alpha=%d(期望%d) 中心 alpha=%d" % (
            im.getpixel((0, 0))[3], a, im.getpixel((240, 374))[3]))

    # 关键像素判据（不能只看尺寸）
    im = Image.open(p_done).convert("RGBA")
    c_ok = im.getpixel((20, 48))[:3] == GREEN[:3]      # 圆是绿的
    t_ok = im.getpixel((42, 66))[:3] == WHITE[:3]      # 勾是白的
    a_ok = im.getpixel((2, 2))[3] == 0                 # 四角在圆外 ⇒ 透明
    print("   勾图标：圆绿=%s 勾白=%s 圆外透明=%s" % (c_ok, t_ok, a_ok))
    for p, want in ((p_sheet, MASK_ALPHA), (p_dialog, MASK_ALPHA)):
        im = Image.open(p).convert("RGBA")
        if im.getpixel((0, 0))[3] != want:
            print("   **FAIL** %s alpha 不对" % os.path.basename(p)); a_ok = False

    # ---- 域标识底块（不透明 + 四角烘行底色）----
    chip_ok = True
    for size, rad, path in ((64, 14, p_chip64), (40, 9, p_chip40)):
        im = Image.open(path)
        w, h = im.size
        rgb = im.convert("RGB")
        corners = [rgb.getpixel((0, 0)), rgb.getpixel((w - 1, 0)),
                   rgb.getpixel((0, h - 1)), rgb.getpixel((w - 1, h - 1))]
        mid = rgb.getpixel((w // 2, h // 2))
        ncol = len(set(rgb.getdata()))
        ok = (im.mode == "RGB" and (w, h) == (size, size)
              and all(c == CARD_BG for c in corners) and mid == CHIP_BG and ncol > 3)
        chip_ok = chip_ok and ok
        n_ok += 1
        print("   %-20s %dx%d mode=%s 四角=%s 中点=%s 颜色数=%d %s"
              % (os.path.basename(path), w, h, im.mode, str(corners[0]), str(mid), ncol,
                 "OK" if ok else "**FAIL**"))
        if ncol <= 3:
            print("      ↑ 颜色数<=3 ⇒ **圆角没画出来**（变成直角方块了）")
        if im.mode != "RGB":
            print("      ↑ 带 alpha ⇒ 会露黑角/白块")

    # ---- 卡片/行底（1:1、不透明、圆角外 = 列表底色）----
    card_ok = True
    for w, h, rad, path in ((224, 145, 20, p_cardmy), (456, 72, 20, p_rowpick)):
        im = Image.open(path)
        rgb = im.convert("RGB")
        corners = [rgb.getpixel(p) for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
        mid = rgb.getpixel((w // 2, h // 2))
        ncol = len(set(rgb.getdata()))
        ok = (im.mode == "RGB" and im.size == (w, h)
              and all(c == LIST_BG for c in corners) and mid == CARD_BG and ncol > 3)
        card_ok = card_ok and ok
        n_ok += 1
        print("   %-18s %dx%d mode=%s 四角=%s 中点=%s 颜色数=%d %s"
              % (os.path.basename(path), im.size[0], im.size[1], im.mode,
                 str(corners[0]), str(mid), ncol, "OK" if ok else "**FAIL**"))
        if ncol <= 3:
            print("      ↑ 颜色数<=3 ⇒ **圆角没画出来**（直角方块）")

    if n_ok == 7 and c_ok and t_ok and a_ok and chip_ok and card_ok:
        print("PASS（7/7 素材）")
        return 0
    print("FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
