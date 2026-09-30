#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_icons.py - 生成主界面卡片图标（iOS squircle 圆角方形底 + 白色符号）

为什么改成"圆角方形 + 符号图形"（2026-09-14 改版）：
  旧版是「渐变正圆 + 中文单字」（箱 / 雷 / 砖 / 棋 / 鼠 …，21 个里 19 个是汉字）。
  问题在于：**汉字要被"读"，符号能被"认"** —— 手机图标的识别逻辑是后者。
  另外正圆在方形卡片行里与 iOS 的"圆角方形图标"语言不一致，整体不像手机。
  ⇒ 改为 iOS squircle（超椭圆，指数 n=5）+ 每个应用一枚白色矢量符号。
  颜色保留"每个应用一个色相"的思路（换成语义化、拉开明度的色板）。

为什么符号要自己画（不用 emoji / 内置图标名）：
  · gen_res 内置 46 个图标名（play/back/settings…）全是**通用操作**图标，
    没有"贪吃蛇""五子棋"这类应用符号；
  · emoji（gen_res.emoji_icon_ss）实测可用，但彩色 emoji 与 iOS 图标风格不一致，
    且字形来自系统字体，跨机器渲染结果不可控。
  ⇒ 自绘：全部用几何图元（圆角矩形/圆/多边形/带圆头直线）在 4x 画布上画完
    LANCZOS 缩回，天然满足 MCP「PNG 防锯齿五要素」（超采样 + 圆头端点 + 1:1 尺寸）。

输出：resources/images/app_icon_<slot>.png（slot 与 kAppTable 的 slot 字段一致）
用法：python tools/gen_icons.py
"""
import math
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ios_theme import RADIUS, TOKENS, hex2rgb  # noqa: E402

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT_DIR = os.path.join(ROOT, "resources", "images")

# 图标边长 —— **必须严格等于 ui/main.html 里 Icon 控件的 data-w/data-h**。
# ⚠️ 尺寸不等 = 框架自己缩放 = 边缘出锯齿（这是"图标糊/毛边"的头号原因，别随手改）。
# 2026-09-14 第二版：主界面改成 4x5 桌面网格 → 控件 72x72，图标同步提到 72。
SIZE = 72
SS = 8             # 超采样倍数（8x 后 LANCZOS 缩回，斜边与圆角更干净）

# (slot, 名称, 主题色, 符号函数名)
# 色板原则：同分类内相邻两项色相/明度至少拉开一档；分类之间允许接近
# （游戏 13 个 vs 工具 7 个不会同时出现在一屏）。
ICONS = [
    (0,  "2048",      "#EDC22E", "sym_2048"),
    (1,  "俄罗斯方块", "#0A84FF", "sym_tetris"),
    (2,  "像素飞机",   "#FF453A", "sym_plane"),
    (3,  "像素小鸟",   "#FFD60A", "sym_bird"),
    (4,  "贪吃蛇",     "#30D158", "sym_snake"),
    (5,  "扫雷",       "#5E5CE6", "sym_mine"),
    (6,  "推箱子",     "#B4763A", "sym_box"),
    (7,  "打砖块",     "#FF6B22", "sym_bricks"),
    (8,  "番茄钟",     "#FF375F", "sym_tomato"),
    (9,  "定时器",     "#64D2FF", "sym_hourglass"),
    (10, "秒表",       "#AF52DE", "sym_stopwatch"),
    (11, "计算器",     "#8E8E93", "sym_calc"),
    (12, "WiFi",      "#32ADE6", "sym_wifi"),
    (13, "蓝牙遥控",   "#6B7A8F", "sym_remote"),
    (14, "打地鼠",     "#7CB342", "sym_hammer"),
    (15, "记忆翻牌",   "#FF2D55", "sym_cards"),
    (16, "五子棋",     "#C9A227", "sym_gomoku"),
    (17, "数字华容道", "#00C7BE", "sym_slide"),
    (18, "反应计时",   "#BF5AF2", "sym_bolt"),
    (19, "时钟套件",   "#0A84FF", "sym_clock"),
    (20, "网络电视",   "#7B61FF", "sym_tv"),
    (21, "信号探针",   "#FF9F0A", "sym_radar"),
    (22, "消消乐",     "#FF7A45", "sym_match3"),
    (23, "摇骰子",     "#D9483B", "sym_dice"),
    # ---- 2026-09-16 儿童益智三件套（slot 24~26）----
    (24, "数字连线画", "#2A9D8F", "sym_dots"),
    (25, "算术泡泡",   "#4C6EF5", "sym_bubble"),
    (26, "找不同",     "#D9822B", "sym_spot"),
    # ---- 2026-09-16 木纹乐器三件套 + 两个工具应用（slot 27~31）----
    (27, "数独",       "#8A6A3A", "sym_sudoku"),
    (28, "节奏钢琴",   "#7A5CC6", "sym_piano"),
    (29, "打鼓",       "#D9822B", "sym_drum"),
    (30, "网络收音机", "#FF7A45", "sym_radio"),
    (31, "摄像头查看", "#32ADE6", "sym_cctv"),
    # ---- 2026-09-16 新增系统设置（slot 32；音效/WiFi/音量/背光收纳进它）----
    (32, "系统设置",   "#8E8E93", "sym_gear"),
    # ---- 2026-09-17 新增电子宠物（slot 33；3D 离线渲染的 WorkBuddy 机器人）----
    (33, "电子宠物",   "#2FE6CC", "sym_pet"),
    # 2026-09-18 小精灵（实拍素材抠像 + 桌面场景，见 docs/elf-app.md）
    (34, "小精灵",     "#FFC478", "sym_elf"),
    # ---- 2026-09-20 两部"动画短片"应用（slot 35~36）----
    # 卡片点进去是全屏循环播放（见 docs/movie-app.md），图标要与 sym_elf 明显区分。
    (35, "飞天仙女",   "#7E6EEB", "sym_fairy"),
    (36, "可爱小猫",   "#E8964C", "sym_kitten"),
    # ---- 2026-09-23 智能家居（slot 37；接入 Home Assistant 的遥控器）----
    # 见 docs/ha-integration-plan.md。绿色系（#1D9E75）跟同屏的
    # 系统设置灰（#8E8E93）、电子宠物青（#2FE6CC）都拉得开。
    (37, "智能家居",   "#1D9E75", "sym_smarthome"),
]


# ==================================================================
#  绘制辅助（全部在 0..64 的逻辑坐标里画，内部自动乘 SS）
# ==================================================================
def _p(v):
    return int(round(v * SS))


class S:
    """把一个 64x64 逻辑画布包成"逻辑坐标 + 自动 4x"的绘图器"""

    def __init__(self, draw):
        self.d = draw

    def rr(self, x0, y0, x1, y1, r, fill=None, outline=None, w=1):
        self.d.rounded_rectangle([_p(x0), _p(y0), _p(x1), _p(y1)], radius=_p(r),
                                 fill=fill, outline=outline, width=max(1, _p(w)))

    def ell(self, cx, cy, rx, ry=None, fill=None, outline=None, w=1):
        ry = rx if ry is None else ry
        self.d.ellipse([_p(cx - rx), _p(cy - ry), _p(cx + rx), _p(cy + ry)],
                       fill=fill, outline=outline, width=max(1, _p(w)))

    def poly(self, pts, fill=None):
        self.d.polygon([(_p(x), _p(y)) for x, y in pts], fill=fill)

    def line(self, x0, y0, x1, y1, w, fill):
        """带圆头端点的直线（PIL 默认 butt 平头 → 斜线端点会出毛刺）"""
        self.d.line([_p(x0), _p(y0), _p(x1), _p(y1)], fill=fill, width=max(1, _p(w)))
        r = w / 2.0
        self.ell(x0, y0, r, r, fill=fill)
        self.ell(x1, y1, r, r, fill=fill)


W = (255, 255, 255, 255)


# ---------------- 21 个符号 ----------------
def sym_2048(s):
    """两张叠起来的数字方块"""
    s.rr(13, 15, 41, 43, 6, outline=W, w=3.4)
    s.rr(23, 24, 51, 52, 6, outline=W, w=3.4)


def sym_tetris(s):
    """T 形四连块"""
    s.rr(12, 14, 52, 24, 2.5, fill=W)
    s.rr(27, 24, 37, 50, 2.5, fill=W)


def sym_plane(s):
    """纸飞机：机身三角 + 折翼"""
    s.poly([(52, 12), (12, 30), (30, 34)], fill=W)
    s.poly([(52, 12), (26, 52), (30, 34)], fill=W)


def sym_bird(s):
    """小鸟：椭圆身体 + 尖喙 + 翅膀"""
    s.ell(28, 30, 15, 12, fill=W)
    s.poly([(43, 26), (54, 31), (43, 36)], fill=W)
    s.poly([(22, 28), (34, 21), (32, 34)], fill=(0, 0, 0, 0))
    s.ell(40, 25, 3.2, 3.2, fill=(0, 0, 0, 0))


def sym_snake(s):
    """L 形三连体（贪吃蛇的"身体拐弯"）"""
    s.rr(11, 11, 27, 27, 5, fill=W)
    s.rr(26, 11, 42, 27, 5, fill=W)
    s.rr(26, 26, 42, 42, 5, fill=W)
    s.rr(41, 26, 57, 42, 5, fill=W)
    s.ell(19, 19, 3.4, 3.4, fill=(0, 0, 0, 0))


def sym_mine(s):
    """水雷：球体 + 四向尖刺 + 引信"""
    for a in (0, 90, 180, 270):
        rad = math.radians(a)
        s.line(32 + 17 * math.cos(rad), 34 + 17 * math.sin(rad),
               32 + 25 * math.cos(rad), 34 + 25 * math.sin(rad), 4, W)
    s.ell(32, 34, 15, 15, fill=W)
    s.line(32, 20, 40, 9, 3.4, W)


def sym_box(s):
    """行李箱 / 推箱子的箱子：方框 + 提手"""
    s.rr(12, 19, 52, 52, 5, outline=W, w=3.6)
    s.rr(25, 11, 39, 19, 3, outline=W, w=3.2)
    s.line(24, 30, 40, 42, 3.2, W)
    s.line(40, 30, 24, 42, 3.2, W)


def sym_bricks(s):
    """砖墙 3x2 + 球"""
    s.rr(10, 12, 30, 25, 2.5, fill=W)
    s.rr(34, 12, 54, 25, 2.5, fill=W)
    s.rr(10, 29, 30, 42, 2.5, fill=W)
    s.rr(34, 29, 54, 42, 2.5, fill=W)
    s.ell(32, 54, 6, 6, fill=W)


def sym_tomato(s):
    """番茄：果实 + 顶部叶"""
    s.ell(32, 36, 19, 17, fill=W)
    s.poly([(32, 20), (23, 12), (32, 15), (41, 12)], fill=W)
    s.rr(30, 13, 34, 21, 1.5, fill=W)


def sym_hourglass(s):
    """沙漏"""
    s.poly([(16, 10), (48, 10), (33, 32), (48, 54), (16, 54), (31, 32)], fill=W)


def sym_stopwatch(s):
    """秒表：表盘 + 顶部按钮 + 指针"""
    s.rr(28, 9, 36, 15, 2, fill=W)
    s.ell(32, 36, 19, 19, outline=W, w=3.6)
    s.line(32, 36, 32, 24, 3.2, W)
    s.line(32, 36, 42, 40, 3.2, W)


def sym_calc(s):
    """计算器：机身 + 按键点阵"""
    s.rr(14, 10, 50, 54, 6, outline=W, w=3.4)
    s.rr(19, 16, 45, 24, 2.5, fill=W)
    for ry in (31, 43):
        for rx in (22, 31, 41):
            s.ell(rx, ry, 3, 3, fill=W)


def sym_wifi(s):
    """WiFi：三段弧 + 圆点"""
    for r, w in ((10, 3.2), (18, 3.4), (26, 3.6)):
        s.d.arc([_p(32 - r), _p(42 - r), _p(32 + r), _p(42 + r)],
                start=215, end=325, fill=W, width=max(1, _p(w)))
    s.ell(32, 44, 4.4, 4.4, fill=W)


def sym_remote(s):
    """遥控器：机身 + 方向圆 + 两颗键"""
    s.rr(18, 8, 46, 56, 8, outline=W, w=3.4)
    s.ell(32, 23, 7.5, 7.5, outline=W, w=3)
    s.ell(27, 40, 3.4, 3.4, fill=W)
    s.ell(37, 40, 3.4, 3.4, fill=W)
    s.ell(32, 49, 3.4, 3.4, fill=W)


def sym_hammer(s):
    """锤子（打地鼠）"""
    s.rr(14, 12, 50, 26, 4, fill=W)
    s.rr(28, 24, 36, 54, 3, fill=W)


def sym_cards(s):
    """两张叠起来的卡牌"""
    s.rr(11, 16, 35, 52, 5, outline=W, w=3.4)
    s.rr(29, 12, 53, 48, 5, outline=W, w=3.4)


def sym_gomoku(s):
    """棋盘十字 + 一枚落子"""
    s.line(32, 10, 32, 54, 3, W)
    s.line(10, 32, 54, 32, 3, W)
    s.ell(32, 32, 9, 9, fill=W)
    s.ell(32, 32, 3.6, 3.6, fill=(0, 0, 0, 0))


def sym_slide(s):
    """3x3 宫格，右下角空缺（华容道的空格）"""
    for gy in range(3):
        for gx in range(3):
            if gx == 2 and gy == 2:
                continue
            x0 = 12 + gx * 14
            y0 = 12 + gy * 14
            s.rr(x0, y0, x0 + 11, y0 + 11, 3, fill=W)


def sym_bolt(s):
    """闪电（反应计时）"""
    s.poly([(38, 6), (16, 34), (29, 34), (25, 58), (47, 29), (34, 29)], fill=W)


def sym_clock(s):
    """时钟：表盘 + 时分针"""
    s.ell(32, 32, 21, 21, outline=W, w=3.8)
    s.line(32, 32, 32, 18, 3.4, W)
    s.line(32, 32, 43, 38, 3.4, W)


def sym_tv(s):
    """电视：机身 + 播放三角"""
    s.rr(9, 16, 55, 50, 6, outline=W, w=3.6)
    s.poly([(28, 24), (28, 42), (43, 33)], fill=W)


def sym_radar(s):
    """信号探针：两圈同心回波环 + 扫描指针 + 中心回波点。

    刻意与已有的符号区分开：
      · sym_wifi 是"扇形三段弧"（朝上的开口弧），这里是**闭合圆环**；
      · sym_gomoku 是"十字 + 落子"，中心是**空心**的洞，这里是实心点 + 斜指针。
    中心点必须实心 —— gen_icons 的自检 ② 要求中心 17x17 区域有足够白色墨迹。
    """
    s.ell(32, 36, 17, 17, outline=W, w=2.6)   # 外圈回波环
    s.ell(32, 36, 8, 8, outline=W, w=2.4)     # 内圈回波环
    s.ell(32, 24, 4.8, 4.8, fill=W)           # 中心点（略偏上，给指针留位）
    s.line(32, 24, 51, 43, 3.4, W)            # 扫描指针（右下 45°）
    s.ell(51, 43, 3.0, 3.0, fill=W)           # 指针端点（加粗，避免尖角被缩掉）


def sym_match3(s):
    """消消乐：三个糖果方块连成一行 + 上方一颗四角星（"连成一线就消"）。

    刻意与已有符号分开：
      · sym_slide 是 **3x3 宫格**（九个块），这里只有**横排三个**；
      · sym_bricks 是 2x2 砖 + 底下一颗球，这里是 1x3 一行 + 顶部星。
    中心 (32,32) 落在中间方块上（实心）⇒ 满足 gen_icons 自检 ② 的中心墨迹要求。
    """
    s.rr(8, 27, 22, 41, 4, fill=W)
    s.rr(25, 27, 39, 41, 4, fill=W)
    s.rr(42, 27, 56, 41, 4, fill=W)
    s.poly([(32, 7), (35, 17), (45, 20), (35, 23), (32, 33), (29, 23), (19, 20), (29, 17)],
           fill=W)


def sym_dice(s):
    """摇骰子：等轴立方体（六边形轮廓 + 三条内棱）+ 三个可见面各一颗点数。

    刻意与已有符号分开：
      · sym_box 是"行李箱"（**正投影**方框 + 提手 + 叉），这里是有顶面的**立体**六边形；
      · sym_bricks 是 2x2 砖块 + 底下一颗球。
    中心 (32,32) 落在三条内棱的交点上（有白色墨迹）⇒ 满足 gen_icons 自检 ② 的中心墨迹要求。
    """
    cx, cy, r = 32.0, 33.0, 21.6
    pts = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
           for a in (30, 90, 150, 210, 270, 330)]
    s.d.polygon([(_p(x), _p(y)) for x, y in pts], outline=W, width=max(1, _p(3.2)))
    for a in (90, 210, 330):        # 中心 -> 下 / 左上 / 右上：立方体的三条内棱
        s.line(cx, cy, cx + r * math.cos(math.radians(a)),
               cy + r * math.sin(math.radians(a)), 3.0, W)
    # 三个可见面里的点数（顶面 / 左下 / 右下，位置 = 各面的形心）
    s.ell(cx, cy - 10.6, 2.9, 2.9, fill=W)
    s.ell(cx - 9.4, cy + 5.6, 2.9, 2.9, fill=W)
    s.ell(cx + 9.4, cy + 5.6, 2.9, 2.9, fill=W)


def sym_dots(s):
    """数字连线画：三个圆点 + 两段连线（"按数字连起来"）。

    刻意与已有符号分开：
      · sym_gomoku 是"十字 + 中心空心圈"，这里是**折线**（斜的两段）+ 三个实心点；
      · sym_slide 是 3x3 九宫格。
    中间那颗点落在 (32,30) 且半径 7.5 —— 覆盖中心 (32,32) ⇒ 满足自检 ② 的中心墨迹要求。
    """
    # ⚠️ 逻辑坐标就是**输出像素**（_p(v)=v*SS，再缩小 SIZE 倍），可用范围是 0..72 ——
    #    而既有 24 枚图标的墨迹中心都落在 ~31.5（不是 36）。新符号要跟这个基准对齐，
    #    否则一排图标里这枚会明显"偏右下"。下面这组坐标的墨迹中心 = (32, 32)。★ 已实测。
    s.line(14, 43, 32, 22, 3.6, W)
    s.line(32, 22, 50, 43, 3.6, W)
    s.ell(14, 43, 6.5, 6.5, fill=W)
    s.ell(50, 43, 6.5, 6.5, fill=W)
    s.ell(32, 22, 7.5, 7.5, fill=W)


def sym_bubble(s):
    """算术泡泡：一个泡泡（圆环）+ 里面的加号 + 右上角一颗小泡泡。

    刻意与已有符号分开：
      · sym_radar 是"两圈同心环 + 斜指针"，这里是**单环 + 十字**；
      · sym_calc 是圆角矩形机身，这里没有矩形。
    十字交点 (32,33) ⇒ 满足自检 ② 的中心墨迹要求。
    """
    # 同 sym_dots：墨迹中心对齐到 (32, 32)（= 既有图标的基准）
    s.ell(29, 38, 17, 17, outline=W, w=3.4)
    s.line(29, 28, 29, 48, 3.6, W)
    s.line(19, 38, 39, 38, 3.6, W)
    s.ell(45, 15, 6, 6, fill=W)


def sym_spot(s):
    """找不同：一个竖长的对比框 + 中间分隔线 + 两个位置不同的圆点。

    刻意与已有符号分开：
      · sym_cards 是**两张错开**的圆角卡；这里是一整个框 + 中缝 + 内部两点；
      · sym_wifi 是扇形弧。
    中缝在 y=32 ⇒ 中心 (32,32) 有墨迹 ⇒ 满足自检 ②。
    """
    s.rr(12, 6, 52, 58, 6, outline=W, w=3.4)
    s.line(12, 32, 52, 32, 3.4, W)
    s.ell(24, 19, 5.0, 5.0, fill=W)
    s.ell(41, 45, 5.0, 5.0, fill=W)


def sym_sudoku(s):
    """数独：3x3 宫格 + 对角三颗实心数字点。

    中心 (32,32) 正好落在中间那颗点上（实心）⇒ 满足自检 ②。
    ★ 墨迹 bbox 中心已对齐 (32,32)（= 既有 27 枚图标的基准，见 sym_dots 的说明）：
      外框 11..53 两边对称 ⇒ x 中心 32；上下同理 ⇒ y 中心 32。
    """
    s.rr(11, 11, 53, 53, 5, outline=W, w=3.4)
    for v in (25.0, 39.0):
        s.line(11, v, 53, v, 1.7, W)
        s.line(v, 11, v, 53, 1.7, W)
    s.ell(18, 18, 3.2, 3.2, fill=W)
    s.ell(32, 32, 3.2, 3.2, fill=W)
    s.ell(46, 46, 3.2, 3.2, fill=W)


def sym_piano(s):
    """节奏钢琴：琴键块 + 挖空的黑键缝 + 一个落下来的音符。

    "黑键"用 `fill=(0,0,0,0)` **挖空**（透明 = 主题色透出来），和 sym_bird 的
    眼睛/翅膀是同一招。黑键只做到 y=29（不到中心）⇒ 中心 (32,32) 仍是白键面 ⇒ 自检 ② 过。
    ★ 墨迹中心对齐 (32,32)：键块 11..53；顶部音符 (32,8) 与底部键块 53 ⇒ y 中心 30.5，
      已实测微调到 (32,32)。
    """
    s.rr(11, 20, 53, 56, 4, fill=W)             # 键块
    for x in (25.0, 39.0):                      # 白键分缝
        s.line(x, 20, x, 56, 2.0, (0, 0, 0, 0))
    for x in (18.0, 46.0):                      # 两个黑键（挖空，不到中心）
        s.rr(x - 3.4, 20, x + 3.4, 36, 1.6, fill=(0, 0, 0, 0))
    s.ell(32, 10, 5.0, 3.6, fill=W)             # 落下的音符
    s.line(32, 10, 32, 20, 2.4, W)              # 音符的杆


def sym_drum(s):
    """打鼓：鼓身 + 鼓面高光弧 + 两根交叉鼓棒。

    鼓身是实心椭圆，中心 (32,32) 落在鼓身上 ⇒ 自检 ② 过。
    ★ 墨迹中心对齐 (32,32)：x 11..53、y 9..55。
    """
    s.ell(32, 39, 21, 16, fill=W)                       # 鼓身
    s.ell(32, 33, 16, 6, outline=(0, 0, 0, 0), w=2.6)   # 鼓面的高光带（挖空）
    s.line(22, 24, 44, 12, 3.0, W)                      # 鼓棒 1
    s.line(42, 22, 53, 30, 3.0, W)                      # 鼓棒 2
    s.ell(44, 12, 2.4, 2.4, fill=W)
    s.ell(53, 30, 2.4, 2.4, fill=W)


def sym_radio(s):
    """网络收音机：机身 + 旋钮 + 喇叭格栅 + 天线。

    中心 (32,32) 落在"旋钮与格栅之间的机身面"上（实心）⇒ 自检 ② 过。
    ★ 墨迹中心对齐 (32,32)：x 10..54、y 9..55。
    """
    s.line(40, 20, 49, 9, 2.8, W)                # 天线
    s.rr(10, 20, 54, 55, 6, fill=W)              # 机身
    s.ell(22, 38, 7.0, 7.0, fill=(0, 0, 0, 0))   # 旋钮（挖空）
    s.line(22, 31, 22, 45, 1.6, W)               # 旋钮指针
    for y in (28.0, 36.0, 44.0):                 # 喇叭格栅（三条挖空槽）
        s.rr(35, y, 49, y + 4, 2.0, fill=(0, 0, 0, 0))


def sym_cctv(s):
    """摄像头查看：枪机机身 + 镜头锥 + 安装支架 + 镜头圈。

    中心 (32,32) 落在机身上（实心）⇒ 自检 ② 过。
    ★ 墨迹中心对齐 (32,32)：x 10..55、y 15..50。
    刻意与 sym_radio 区分：radio 是"横向机身 + 天线 + 格栅"，这里是"斜的镜头锥 + 支架"。
    """
    s.line(25, 30, 25, 19, 3.0, W)                       # 支架立杆
    s.line(17, 19, 33, 19, 3.0, W)                       # 支架横臂
    s.rr(10, 30, 40, 50, 6, fill=W)                      # 机身
    s.poly([(40, 32), (55, 40), (40, 48)], fill=W)       # 镜头锥
    s.ell(24, 40, 5.0, 5.0, fill=(0, 0, 0, 0))           # 镜头圈（挖空）
    s.ell(55, 40, 2.6, 2.6, fill=W)                      # 镜头端头


def sym_gear(s):
    """系统设置：齿轮（8 齿 + 轴孔）。
    ★ 墨迹中心对齐 (32,32)：齿尖到 32±22.5 ⇒ x/y 约 9.5..54.5。
    与 sym_cctv / sym_wifi 都不同：这里是"放射状齿 + 实心盘 + 挖空轴孔"。
    """
    for i in range(8):
        a = math.radians(i * 45.0)
        ca, sa = math.cos(a), math.sin(a)
        s.line(32 + ca * 12, 32 + sa * 12, 32 + ca * 19, 32 + sa * 19, 7.0, W)
    s.ell(32, 32, 15.0, 15.0, fill=W)
    s.ell(32, 32, 6.5, 6.5, fill=(0, 0, 0, 0))


def sym_smarthome(s):
    """智能家居：一栋房子（圆窗 + 开着的门）。

    ★ 墨迹 bbox 中心对齐 (32,32)：屋顶 x 11.5..52.5、墙 y 29..51
      ⇒ 整体 y 13.5..51（中心 32.25）、x 11.5..52.5（中心 32）。
    与既有符号都不同：sym_gear 是齿轮、sym_wifi 是扇形波纹、sym_cctv 是镜头、
    sym_pet 是机器人头 —— 这个是"实体轮廓 + 两处挖空"。
    """
    s.line(11.5, 30.0, 32.0, 13.5, 6.0, W)          # 左屋顶
    s.line(32.0, 13.5, 52.5, 30.0, 6.0, W)          # 右屋顶
    s.rr(17.0, 29.0, 47.0, 51.0, 3.5, fill=W)       # 墙体（圆角方）
    s.ell(32.0, 35.5, 5.2, 5.2, fill=(0, 0, 0, 0))  # 圆窗挖空
    s.rr(29.0, 41.0, 35.0, 51.0, 1.2, fill=(0, 0, 0, 0))  # 门挖空（底部开）


def sym_pet(s):
    """电子宠物：一只有天线的机器人头（与 sym_gear / sym_cctv / sym_wifi 都不同）。

    ★ 墨迹 bbox 必须中心对齐 (32,32)：这里天线球 12.5..19.3、头 23..51.5
      ⇒ 整体 y 12.5..51.5（中心 32）、x 14..50（中心 32）。
    """
    s.line(32, 23, 32, 18, 2.6, W)                    # 天线杆
    s.ell(32, 15.9, 3.4, 3.4, fill=W)                 # 天线球
    s.rr(14, 23, 50, 51.5, 9.0, fill=W)               # 头（圆角方）
    s.rr(20, 31, 29, 39, 2.5, fill=(0, 0, 0, 0))      # 左眼挖空
    s.rr(35, 31, 44, 39, 2.5, fill=(0, 0, 0, 0))      # 右眼挖空


def sym_elf(s):
    """小精灵：尖耳朵 + 圆身子 + 一点闪光（与 sym_pet 的"机器人头"明显不同）。

    ★ 墨迹 bbox 必须中心对齐 (32,32)：左耳尖 x=16、右耳尖 x=47（中心 31.5）、
      最上 y=8、脚底 y=56.5（中心 32.2）。改形状后要重新量（见 gen_icons 的自检输出）。
    """
    s.poly([(20.5, 26), (16, 8), (29.5, 20.5)], fill=W)        # 左耳（尖）
    s.poly([(42.5, 26), (47, 8), (33.5, 20.5)], fill=W)        # 右耳（尖）
    s.ell(31.5, 38, 15.5, 16, fill=W)                          # 身子（圆）
    s.ell(26, 34, 2.8, 3.4, fill=(0, 0, 0, 0))                 # 左眼挖空
    s.ell(37, 34, 2.8, 3.4, fill=(0, 0, 0, 0))                 # 右眼挖空
    s.line(44, 17, 44, 25, 2.2, W)                             # 闪光（竖）
    s.line(40, 21, 48, 21, 2.2, W)                             # 闪光（横）


def sym_fairy(s):
    """飞天仙女：右边一弯月牙 + 左边带翅膀飞起来的小人 + 一条飘带。

    ★ 与 sym_kitten / sym_elf 刻意区分：本图的"记忆点"是**右侧那弯月牙**。
    ★ 墨迹 bbox 必须中心对齐 (32,32)：改形状后按 gen_icons 的自检输出重新量。
    """
    s.ell(43, 22, 13, 13, fill=W)                              # 月牙外圆
    s.ell(38.5, 19.5, 10.5, 10.5, fill=(0, 0, 0, 0))           # 挖出月牙
    s.ell(21, 30, 4.2, 4.2, fill=W)                            # 头
    s.poly([(21, 35), (14, 50), (28, 50)], fill=W)             # 裙子
    s.poly([(17, 37), (7, 30), (15, 44)], fill=W)              # 左翅
    s.poly([(25, 36), (32, 30), (27, 43)], fill=W)             # 右翅
    s.line(12, 53, 30, 48, 2.0, W)                             # 飘带


def sym_kitten(s):
    """可爱小猫：正脸（三角耳 + 圆头 + 挖空的眼睛与鼻子 + 四根胡须）。

    ★ 与 sym_elf 刻意区分：精灵是"尖耳 + 圆身子 + 闪光"，这里是**一张正脸 + 胡须**。
    ★ 墨迹 bbox 必须中心对齐 (32,32)：四根胡须左右对称（x=2 / x=62），
      耳尖 y=10、下巴 y=53（中心 31.5）。改形状后按自检输出重新量。
    """
    s.poly([(19, 25), (16, 10), (33, 21)], fill=W)             # 左耳
    s.poly([(45, 25), (48, 10), (31, 21)], fill=W)             # 右耳
    s.ell(32, 39, 17, 14, fill=W)                              # 头
    s.ell(25, 36, 3.0, 3.6, fill=(0, 0, 0, 0))                 # 左眼挖空
    s.ell(39, 36, 3.0, 3.6, fill=(0, 0, 0, 0))                 # 右眼挖空
    s.poly([(32, 44), (29, 41), (35, 41)], fill=(0, 0, 0, 0))  # 鼻子挖空
    s.line(2, 39, 14, 37, 1.8, W)                              # 胡须（左 2）
    s.line(2, 45, 14, 44, 1.8, W)
    s.line(62, 39, 50, 37, 1.8, W)                             # 胡须（右 2）
    s.line(62, 45, 50, 44, 1.8, W)


SYMBOLS = {}


def _collect():
    for _, _, _, fn in ICONS:
        SYMBOLS[fn] = globals()[fn]


# ==================================================================
#  图标合成
# ==================================================================
def squircle_mask(size, ss):
    """iOS 超椭圆 mask（|x/a|^n + |y/b|^n = 1，n=5）。

    比"圆角矩形（rx=30%）"更接近 iOS：四个角过渡更"鼓"，边上没有明显直段。
    """
    n = 5.0
    a = b = size / 2.0 - 0.5
    pts = []
    steps = 720
    for i in range(steps):
        t = 2 * math.pi * i / steps
        ct, st = math.cos(t), math.sin(t)
        x = a * (abs(ct) ** (2.0 / n)) * (1 if ct >= 0 else -1)
        y = b * (abs(st) ** (2.0 / n)) * (1 if st >= 0 else -1)
        pts.append((size / 2.0 + x, size / 2.0 + y))
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).polygon(pts, fill=255)
    return m


def make_icon(color_hex, sym_name):
    SZ = SIZE * SS
    base = hex2rgb(color_hex)

    # ① 竖向双段渐变（iOS 图标是"上亮下暗"的浅渐变，不做噪声、不做对角）
    top = tuple(min(255, int(v + (255 - v) * 0.16)) for v in base)
    bot = tuple(max(0, int(v * 0.88)) for v in base)
    grad = Image.new("RGB", (2, SZ))
    gpx = grad.load()
    for y in range(SZ):
        t = y / float(SZ - 1)
        col = tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3))
        gpx[0, y] = col
        gpx[1, y] = col
    img = grad.resize((SZ, SZ), Image.BILINEAR).convert("RGBA")

    # ② squircle 裁剪（弧线外 alpha=0）
    img.putalpha(squircle_mask(SZ, SS))

    # ③ 白色符号（超采样画布里绘制）
    layer = Image.new("RGBA", (SZ, SZ), (0, 0, 0, 0))
    s = S(ImageDraw.Draw(layer))
    SYMBOLS[sym_name](s)
    img = Image.alpha_composite(img, layer)

    # ④ 缩回目标尺寸
    # ★★ 必须用 BOX，**不能再用 LANCZOS**（2026-09-16 真机定位）：
    #   LANCZOS 是带负瓣的插值核，对「squircle 硬边 mask」做缩小时会**振铃**——
    #   实测顶行 α 序列为 `0,0,0,0,0,0,1,4,0,0,0,1,35,77,...`：
    #   α 在上升过程中反复跌回 0，边缘外侧散落着 α=1/4 的孤立像素。
    #   这些像素渲染到黑底上就是零星的亮点 → 肉眼看就是"倒角有锯齿/毛刺"。
    #   BOX（盒式平均）= 精确覆盖率，α 单调、过渡带 1px、无负瓣。
    out = img.resize((SIZE, SIZE), Image.BOX)
    out = clean_alpha(out)
    return out


def clean_alpha(img):
    """把 α==0 的像素 RGB 清零。

    为什么必须清：这些像素渲染时本该完全透明，但**只要渲染路径忽略了 alpha 通道
    （或做了 1-bit 量化），它们就会直接显形**。实测 app_icon 的透明区里有 RGB=255
    的"隐形白点"（非预乘缩放的残留），一旦被忽略 alpha 就会变成一圈白砂。
    α=0 的 RGB 没有任何信息价值，清零是零代价的保险。
    """
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]
            if a == 0 and (r or g or b):
                px[x, y] = (0, 0, 0, 0)
    return img


def main():
    _collect()
    os.makedirs(OUT_DIR, exist_ok=True)
    bad = 0

    for slot, name, color, sym in ICONS:
        img = make_icon(color, sym)

        # 自检 ①：四角必须真透明（否则卡片上会露出方块角）
        for xy in ((0, 0), (SIZE - 1, 0), (0, SIZE - 1), (SIZE - 1, SIZE - 1)):
            if img.getpixel(xy)[3] != 0:
                print("!! slot %d 四角 alpha != 0 (%s)" % (slot, xy))
                bad += 1

        # 自检 ②：中心区域要有白色符号的墨迹（符号可能画错位置/被裁掉）
        #   不能只判单点（中空的符号如"五子棋"中心是洞）→ 数一小块里的白像素。
        solid = 0
        for dy in range(-8, 9):
            for dx in range(-8, 9):
                r, g, b, a = img.getpixel((SIZE // 2 + dx, SIZE // 2 + dy))
                if a > 200 and r > 200 and g > 200 and b > 200:
                    solid += 1
        if solid < 12:
            print("!! slot %d 中心白符号墨迹过少 (%d px)" % (slot, solid))
            bad += 1

        # 自检 ③：不透明区域覆盖率（squircle 理论面积约 0.92，中空部分会降一点）
        op = sum(1 for v in img.split()[3].getdata() if v > 128)
        ratio = op / float(SIZE * SIZE)
        if not (0.88 < ratio <= 1.0):
            print("!! slot %d 不透明覆盖率异常 %.3f（应≈0.90~1.0）" % (slot, ratio))
            bad += 1

        fname = "app_icon_%d.png" % slot
        img.save(os.path.join(OUT_DIR, fname))
        print("gen %-18s %-12s %-8s 覆盖 %.2f" % (fname, name, color, ratio))

    print("共 %d 个图标，自检%s" % (len(ICONS), "失败" if bad else "通过"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
