#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_whack_art.py —— 打地鼠**锤子**的绘制函数库（不再单独产出文件）

⚠️ 2026-09-15 起本文件**只提供函数**，被 `tools/gen_game_art.py` import 复用
（`draw_hammer` / `make_frames` / 配色常量），它产出的 PNG 与清单都由
`gen_game_art.py` 统一写出（`resources/images/game/whack/hammer_*.png` +
`src/core/PgGameArt.h`）。

**为什么改**：原来它把三帧像素**写成 C 数组编进 .so**（`src/core/PgWhackArt.h`，
580KB），改一张图要重跑生成器 + 重编 + 重新刷机。现在走"PNG 运行时加载"，
改图只要覆盖 PNG —— 见 `docs/game-art-pipeline.md`。
（旧的 `src/core/PgWhackArt.h` 已无人引用，可删。）

保留在此的绘制逻辑（离线烘图，运行时只 1:1 贴像素）：
  · 运行时做旋转/缩放 = 重采样 = 边缘发糊/锯齿，用户明确禁止（"所有东西不能做拉伸"）。
  · 离线用 LANCZOS 超采样旋转，质量高；运行时只是逐像素 α 混合。
  · 三帧动作（举起 / 挥下 / 砸中）**锚点必须各自记录**（都绕握把转，
    但锚点取的是**锤头中心** —— 这样三帧的锤头钉在触点、只有柄在摆），
    否则运行时换帧会"跳"。

尺寸约束：SPRITE W x H = 120x120，锚点 ANCHOR = (60, 112)（握把底端）。
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

W, H = 120, 120          # sprite 尺寸（== 屏幕上贴的尺寸，1:1）
ANCHOR = (60, 112)       # 旋转中心 = 握把底端（手握住的位置）
SS = 4                   # 超采样倍数（画和旋转都在放大后的画布上做）
K = 0.90                 # 绕握把缩放（留边，见 draw_hammer 的说明）

# ---- 配色（卡通木槌：暖木色 + 深描边，和草地场景协调）----
C_EDGE = (74, 42, 18, 255)        # 描边（深棕）
C_HEAD = (208, 139, 82, 255)      # 槌头正面
C_HEAD_HI = (238, 182, 124, 255)  # 槌头高光（左上）
C_HEAD_LO = (168, 100, 47, 255)   # 槌头暗部（右下）
C_HANDLE = (190, 126, 74, 255)    # 柄
C_HANDLE_HI = (222, 158, 102, 255)
C_BAND = (140, 152, 164, 255)     # 金属箍
C_BAND_HI = (198, 208, 218, 255)


def _rr(d, box, r, fill):
    d.rounded_rectangle(box, radius=r, fill=fill)


def draw_hammer(ss, k=1.0):
    """在 (W*ss, H*ss) 画布上画一把竖直的木槌（握把底端 = ANCHOR*ss）。

    k = **绕握把的缩放**（不是整图缩放）。为什么需要它：三帧是绕握把旋转出来的，
    角度越大内容越往两侧探；k=1.0 时 up/hit 两帧会**贴到画布边**（自检会警告），
    再调角度就会被裁。留 10% 边距最省事（也让锤子在 480x540 的画面里比例更合适）。
    """
    im = Image.new("RGBA", (W * ss, H * ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    ax, ay = ANCHOR[0] * ss, ANCHOR[1] * ss
    ow = 3 * ss * k                                  # 描边宽度

    # 相对锚点的偏移量（向上为负）—— 全部乘 k，所以"绕握把缩放"
    def X(dx):
        return ax + dx * ss * k

    def Y(dy):
        return ay + dy * ss * k

    # ---------- 柄（从槌头一直延伸到握把底）----------
    hx0, hx1 = X(-7), X(8)
    hy0, hy1 = Y(-84), Y(0)
    _rr(d, [hx0 - ow, hy0, hx1 + ow, hy1], 8 * ss, C_EDGE)      # 描边
    _rr(d, [hx0, hy0, hx1, hy1], 6 * ss, C_HANDLE)              # 填充
    _rr(d, [X(-5.5), Y(-82), X(-3), Y(-2)], 2 * ss, C_HANDLE_HI)  # 圆柱感高光
    d.ellipse([hx0 - ow, Y(-8), hx1 + ow, Y(4)], fill=C_EDGE)   # 握把圆头
    d.ellipse([hx0, Y(-6.5), hx1, Y(2)], fill=C_HANDLE)

    # ---------- 槌头（横放的圆柱，圆角矩形）----------
    bx0, bx1 = X(-36), X(36)
    by0, by1 = Y(-104), Y(-66)
    _rr(d, [bx0 - ow, by0 - ow, bx1 + ow, by1 + ow], 14 * ss, C_EDGE)
    _rr(d, [bx0, by0, bx1, by1], 11 * ss, C_HEAD)
    _rr(d, [X(-32), Y(-101), X(32), Y(-92)], 5 * ss, C_HEAD_HI)  # 上沿高光
    _rr(d, [X(-32), Y(-77), X(32), Y(-69)], 5 * ss, C_HEAD_LO)   # 下沿暗部
    for dx in (-28, 16):                                         # 两端金属箍
        _rr(d, [X(dx), Y(-103), X(dx + 12), Y(-67)], 4 * ss, C_BAND)
        _rr(d, [X(dx + 2), Y(-101), X(dx + 5), Y(-69)], 2 * ss, C_BAND_HI)

    return im


BIG = 3   # 旋转用的放大画布倍数（见下面对"为什么必须在大画布上转"的说明）

# HEAD_DY = 锤头中心距握把的距离（源像素；draw_hammer 里锤头上下边是 Y(-104)/Y(-66)）。
# ★ 导出的锚点就用**锤头中心**，不是握把 —— 见 make_frames 的说明。
HEAD_DY = 85.0


def _rot_point(pt, ang, size, center, mark=1):
    """点 pt 经过与 `Image.rotate(ang, center=...)` **完全一致**的变换后的坐标。

    为什么不手推旋转矩阵：PIL 的 rotate 是**视觉逆时针**，而图像坐标 y 轴向下 ——
    自己推公式极容易把符号搞反（症状是旋转后的锚点偏到另一侧）。
    这里用"画一个小方块 + NEAREST 旋转"来**实算**，和真正用的那步变换同源，不会错。
    （mark=1 画 3x3：单个像素会被 NEAREST 采样漏掉，实测 -26° 时直接消失。）
    """
    m = Image.new("L", size, 0)
    for dy in range(-mark, mark + 1):
        for dx in range(-mark, mark + 1):
            x, y = int(round(pt[0])) + dx, int(round(pt[1])) + dy
            if 0 <= x < size[0] and 0 <= y < size[1]:
                m.putpixel((x, y), 255)
    r = m.rotate(ang, resample=Image.NEAREST, center=center)
    bb = r.getbbox()
    if not bb:
        return None
    return ((bb[0] + bb[2] - 1) / 2.0, (bb[1] + bb[3] - 1) / 2.0)


def make_frames():
    """三帧：举起（蓄力）/ 挥下（中位）/ 砸中（贴地）。

    角度约定：PIL `rotate(a)` 逆时针为正。握把在下、槌头在上时，
      · **负角度 = 顺时针** ⇒ 槌头向右倒 ⇒ 举起的蓄力姿态
      · **正角度 = 逆时针** ⇒ 槌头向左倒 ⇒ 砸下去的姿态

    ★★ 必须在**放大的大画布**上旋转（2026-09-15 修，用户报"锤子缺了一个角落"）：
      原来直接在 120x120（SS 尺度 480x480）的画布上 `rotate` —— PIL 的 rotate
      **不会自动扩展画布**，转到画布外的部分**直接丢失**，事后"居中"也救不回来。
      实测（SS 尺度）：up 帧右边丢 39px、hit 帧左边丢 55px ⇒ 最终图上缺 10~14 像素的角
      （症状就是"锤头少了一块"，而且**只在某几帧**出现，很容易被当成美术风格）。
      现在：先把锤子贴进 3 倍大的画布（锚点对齐到画布中心），在那里旋转（绝不裁），
      再降到最终尺度、取"刚好装下内容"的居中窗口。

      窗口装不下时报错退出（而不是默默裁掉）—— 那是"该调小 K 或加大 W/H"的信号。

    ★★ 导出的锚点是**锤头中心**（不是握把）（2026-09-15 用户要求"点击时锤头落点
      要直接到老鼠头上"）：三帧是绕**握把**转出来的，所以锤头中心在各帧的位置不同；
      把每帧的锤头中心作为贴图锚点 ⇒ 屏幕上**锤头钉在触点不动、只有柄在摆**，
      看起来就是"照着鼠标砸下去"。若用握把当锚点，锤头会飘到触点的斜上方约 70px。

      旋转中心仍然是握把（挥动的支点在那里，视觉才自然）—— 锚点与旋转中心是两件事。

    返回 [(名字, Image, 角度, (anchorX, anchorY)), ...]，锚点 = 锤头中心在图内的坐标
    """
    base = draw_hammer(SS, K)
    bw, bh = base.size

    big = Image.new("RGBA", (bw * BIG, bh * BIG), (0, 0, 0, 0))
    big.paste(base, (bw, bh))                      # 居中放置（四周各留 1 倍余量）
    acx, acy = bw + ANCHOR[0] * SS, bh + ANCHOR[1] * SS   # 握把（= 旋转中心）在大画布里的位置
    head_big = (bw + ANCHOR[0] * SS, bh + ANCHOR[1] * SS - HEAD_DY * SS * K)  # 锤头中心

    out = []
    for name, ang in (("up", -26.0), ("mid", -4.0), ("hit", 30.0)):
        r = big.rotate(ang, resample=Image.BICUBIC, center=(acx, acy))
        bb = r.split()[3].getbbox()                # 内容包围盒（None = 全透明，异常）
        if not bb:
            raise SystemExit("帧 %s 旋转后是空图（画错或角度过大）" % name)
        if bb[0] < 2 or bb[1] < 2 or bb[2] > bw * BIG - 2 or bb[3] > bh * BIG - 2:
            raise SystemExit("帧 %s 旋转后顶到大画布边（BIG=%d 不够大）" % (name, BIG))

        rs = r.resize((bw * BIG // SS, bh * BIG // SS), Image.LANCZOS)  # 降到最终像素尺度
        b2 = rs.split()[3].getbbox()
        cw, ch = b2[2] - b2[0], b2[3] - b2[1]
        if cw > W or ch > H:
            raise SystemExit("帧 %s 内容 %dx%d 装不进 %dx%d —— 调小 K 或加大 W/H"
                             % (name, cw, ch, W, H))

        # 开一个 W x H 的窗口把内容居中；锚点按窗口左上的偏移换算
        left = b2[0] - (W - cw) // 2
        top = b2[1] - (H - ch) // 2
        small = rs.crop((left, top, left + W, top + H))

        hp = _rot_point(head_big, ang, big.size, (acx, acy))   # 锤头中心旋转后的位置
        if hp is None:
            raise SystemExit("帧 %s 锤头中心算不出来（旋转标记丢了）" % name)
        # 大画布坐标 → 最终素材内的坐标
        hx = int(round(hp[0] / SS)) - left
        hy = int(round(hp[1] / SS)) - top
        if not (0 <= hx < W and 0 <= hy < H):
            raise SystemExit("帧 %s 锤头中心 (%d,%d) 落在 %dx%d 素材外 —— 锚点会失效"
                             % (name, hx, hy, W, H))
        out.append((name, small, ang, (hx, hy)))
    return out
