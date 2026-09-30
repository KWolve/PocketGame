#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_elf_from_video.py - 把 `pet_video/desk-pet-seg*.mp4` 里的**小精灵**抠成画布贴图

素材来源不是画的，是**用户拍的/生成的实拍片段**（黑底、固定机位）：
  seg1 = 小精灵从画面左边"飘/走进来"，平移约 400px 后站定
  seg2 = 站定之后的待机（几乎不动，只有轻微起伏）

本脚本负责整条链路：
  ① 抽帧（缓存到 out/petv/，缺失才调 ffmpeg）
  ② **抠像**：黑底按亮度阈值抠 → 抹掉右下角固定水印 → 填内部洞 → 内缩防黑边 → 羽化
  ③ 标定：用"站定"的帧量出静止态尺寸 ⇒ 得**统一缩放比**（全帧同一个比例，绝不做逐帧归一化）
  ④ 选帧：待机 = seg2 里运动最连贯的一小段；移动 = seg1 平移段等间隔取样
  ⑤ 烘焙：统一缩放 → 按**脚底水平中心**对齐放进固定画布 → RGBA PNG
  ⑥ 桌面场景：木纹桌面 + 台灯暖光 + 暗背景（整屏 480x540，**不透明**⇒ 走 memcpy 快路径）
  ⑦ 自检 + 预览拼版 + 生成 src/core/PgElfArt.h

★★ 三条关键约束（都是本项目反复吃过的亏）：
  1. **1:1 贴图、不许拉伸**：贴图尺寸 == 屏幕上贴的尺寸，只允许"一次统一缩放"把源尺寸缩到目标尺寸
     （`Image.BOX`，面积平均，不会产生振铃）。
  2. **统一缩放 + 固定画布 + 固定锚点**：逐帧各自归一化会让"呼吸"变成"整体缩放"（看着像在喘大气），
     而固定画布 + 固定锚点保证换帧**绝不跳位**。
  3. **背景必须不透明**：`Canvas` 对"整图 alpha 全 255"走逐行 memcpy，否则逐像素混合（差 3~4 倍）。

用法:
  python tools/gen_elf_from_video.py            # 全流程（需要 ffmpeg 抽帧；已缓存则不需要）
  python tools/gen_elf_from_video.py --measure  # 只量不写（打印标定表，用于调参）
"""
import glob
import os
import shutil
import subprocess
import sys

try:
    from PIL import Image, ImageFilter, ImageDraw
except ImportError:                                        # pragma: no cover
    for _p in (r'C:\Users\Admin\Python\Python313\site-packages',):
        if os.path.isdir(os.path.join(_p, 'PIL')):
            sys.path.insert(0, _p)
    from PIL import Image, ImageFilter, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))    # PocketGame/
VIDEO_DIR = os.path.normpath(os.path.join(ROOT, '..', 'pet_video'))
CACHE = os.path.normpath(os.path.join(ROOT, '..', 'out', 'petv'))
OUTDIR = os.path.join(ROOT, 'resources', 'images', 'game', 'elf')
HEADER = os.path.join(ROOT, 'src', 'core', 'PgElfArt.h')
PREVIEW = os.path.normpath(os.path.join(ROOT, '..', 'out', 'elf_preview.png'))
FFMPEG_CANDIDATES = [os.environ.get('FFMPEG', ''), 'ffmpeg',
                     r'E:\AICODE\F102\ffmpeg.exe', r'E:\AICODE\jiaqian\bin\ffmpeg.exe']

# ---------------- 抠像参数（都来自实测，见 docs/elf-app.md 的"量"一节）----------------
THR = 18              # 黑底判定：背景亮度 0~6（中位 0.1），主体大多 >120；取 18 很安全
WM = (630, 1110, 720, 1280)   # 右下角固定水印（圆角框徽标）区域 (y0, x0, y1, x1) —— 一律抹掉
ERODE = 2             # 边缘内缩：源片主体边缘混了黑底，不内缩会留一圈黑边（很显眼）
FEATHER = 1.8         # 羽化半径（在源分辨率上做，缩放后就是天然的抗锯齿边）

# ---------------- 目标尺寸 ----------------
# ★ 贴图解码后占堆 = 张数 × 宽 × 高 × 4，和面积成正比。本板 56MB（宠物 5.6MB 时 MemFree 9.7MB）。
#   这里把"静止态主体高度"定成 210px（宠物是 289px）⇒ 20 张 ≈ 4.1MB，比宠物还省一点。
REST_H = 210
MARGIN_X = 6          # 画布左右余量（像素，缩放后）
MARGIN_TOP = 6
MARGIN_BOTTOM = 12    # 脚底之下留一点：给阴影用

# ---------------- 选帧 ----------------
IDLE_N = 10           # 待机帧数（运行时**乒乓播放** ⇒ 首尾天然无缝）
IDLE_WIN = 26         # 在 seg2 里滑窗找"最活"的一段（源帧数）

# ★★ 走路帧（2026-09-18 修，用户反馈"身体缺半边"）：
#   源帧 16~34 的主体 bbox 左沿 **x0 = 0** ⇒ 被**视频画面左边缘切掉**了
#   （那几帧正是"小精灵从画面外走进来"的姿态）。走路循环里播到它们 = 缺半边的身体。
#   ⇒ 走路帧只从 **38** 取到 **60**（x0 = 59~352，主体完整在画面内、且仍在平移）。
MOVE_N = 10
MOVE_FROM, MOVE_TO = 38, 60

# ★★ 进场帧（"从屏幕最左边走出来"）：就用上面那批"被画面切掉"的源帧。
#   烘焙时水平锚点取 **推算的完整身体中心**（原片里它是负的 ⇒ 画布里偏右），
#   于是运行时把精灵放在"身体中心 ≈ 屏幕左边缘外"的**负坐标**上时，
#   缺掉的那半边正好落在屏幕外 ⇒ 看着是完整的身体从边上探进来。
#   ⚠️ 这套帧**只能**在"身体中心还在屏幕左边缘附近/以外"时用（见 PgElf.cpp 的 currentFrame）。
ENTER_N = 5
ENTER_FROM, ENTER_TO = 17, 33


def log(*a):
    print(*a)


def find_ffmpeg():
    for c in FFMPEG_CANDIDATES:
        if not c:
            continue
        if os.path.sep in c:
            if os.path.isfile(c):
                return c
        else:
            try:
                subprocess.run([c, '-version'], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, check=True)
                return c
            except Exception:
                continue
    return None


def ensure_frames(seg):
    """返回该段的所有源帧路径（升序）。缓存缺失才调 ffmpeg 抽帧。"""
    d = os.path.join(CACHE, 'f%d' % seg)
    fs = sorted(glob.glob(os.path.join(d, '*.png')))
    if len(fs) >= 100:
        return fs
    exe = find_ffmpeg()
    if not exe:
        log('!! 找不到 ffmpeg：请手工抽帧到 %s' % d)
        log('   ffmpeg -i %s/desk-pet-seg%d.mp4 -vsync 0 %s/%%03d.png' % (VIDEO_DIR, seg, d))
        sys.exit(1)
    os.makedirs(d, exist_ok=True)
    subprocess.run([exe, '-hide_banner', '-loglevel', 'error', '-y',
                    '-i', os.path.join(VIDEO_DIR, 'desk-pet-seg%d.mp4' % seg),
                    '-vsync', '0', os.path.join(d, '%03d.png')], check=True)
    return sorted(glob.glob(os.path.join(d, '*.png')))


def cut(path):
    """黑底抠像 → (RGB 图, L 通道 alpha 图)。返回源分辨率的结果。"""
    im = Image.open(path).convert('RGB')
    g = im.convert('L')
    # ① 抹水印（把它当背景）
    d = ImageDraw.Draw(g)
    d.rectangle([WM[1], WM[0], WM[3] - 1, WM[2] - 1], fill=0)
    # ② 阈值
    a = g.point(lambda v: 255 if v > THR else 0)
    # ③ 填**内部**洞：先从画布四边泛洪标出"外面"，没被标到且非主体的就是洞
    inv = a.point(lambda v: 0 if v > 0 else 255).convert('L')
    ImageDraw.floodfill(inv, (0, 0), 128)
    import numpy as np
    outside = (np.asarray(inv) == 128)
    m = np.asarray(a) > 0
    holes = (~m) & (~outside)
    filled = m | holes
    a = Image.fromarray((filled * 255).astype('uint8'))
    # ④ 内缩防黑边（对本板这种"主体边缘混了黑底"的实拍素材是必须的）
    for _ in range(ERODE):
        a = a.filter(ImageFilter.MinFilter(3))
    # ⑤ 羽化（放源分辨率上做 ⇒ 缩放后就是连续边）
    a = a.filter(ImageFilter.GaussianBlur(FEATHER))
    return im, a


def anchor_of(a):
    """给一张 alpha 图，返回（bbox, 脚底行, 水平中心）"""
    import numpy as np
    arr = np.asarray(a) > 40
    ys, xs = np.where(arr)
    if not len(ys):
        return None
    return (xs.min(), ys.min(), xs.max(), ys.max()), int(ys.max()), int((xs.min() + xs.max()) / 2)


def measure(frame_paths, tag, lo=None, hi=None):
    """量一段帧的静止态尺寸（用于标定统一缩放比）"""
    import numpy as np
    lo = lo or 0
    hi = hi or len(frame_paths)
    hs, ws, feet = [], [], []
    for p in frame_paths[lo:hi]:
        _, a = cut(p)
        r = anchor_of(a)
        if not r:
            continue
        (x0, y0, x1, y1), f, cx = r
        hs.append(y1 - y0 + 1)
        ws.append(x1 - x0 + 1)
        feet.append(f)
    log('  [%s] 帧数 %d：主体高 中位 %d（min %d / max %d）；宽 中位 %d；脚底行 中位 %d'
        % (tag, len(hs), int(np.median(hs)), min(hs), max(hs), int(np.median(ws)), int(np.median(feet))))
    return int(np.median(hs)), int(np.median(ws)), int(np.median(feet)), max(hs), max(ws)


def motion_curve(frame_paths):
    """逐帧运动量（alpha 掩码的异或占比）——用来挑"最活"的一段"""
    import numpy as np
    ms = []
    for p in frame_paths:
        _, a = cut(p)
        ms.append(np.asarray(a) > 40)
    return np.array([(ms[i] ^ ms[i - 1]).sum() / max(1, ms[i].sum()) for i in range(1, len(ms))])


def pick_idle(frame_paths):
    """挑一段"运动连贯、没有大跳"的待机窗口（首尾相接 ⇒ 运行时乒乓播放天然无缝）"""
    import numpy as np
    mo = motion_curve(frame_paths)
    best, bi = -1, 0
    for i in range(0, len(mo) - IDLE_WIN):
        w = mo[i:i + IDLE_WIN]
        if w.max() > 3.0 * max(1e-6, float(np.median(w))):     # 有大跳（换姿态）就跳过
            continue
        s = w.sum()
        if s > best:
            best, bi = s, i
    log('  待机窗口：源帧 %d~%d（运动量合计 %.2f，最大 %.2f，中位 %.3f）'
        % (bi + 1, bi + IDLE_WIN, best, mo[bi:bi + IDLE_WIN].max(), np.median(mo[bi:bi + IDLE_WIN])))
    idx = [bi + int(round(k * (IDLE_WIN - 1) / (IDLE_N - 1))) for k in range(IDLE_N)]
    return idx


def bake(frame_paths, idx, s, canvas, ax, ay, prefix, out, check_h, complete_w=None):
    """把选中的帧烘焙成固定画布 RGBA。
    s   = 统一缩放比
    ax/ay = 画布里的锚点应落在哪（常规帧 = 可见部分的"脚底水平中心"）
    check_h = 这一组允许的高度波动（相对中位的比例），超了报错
    complete_w = ★ 进场帧专用：源片里**被画面左边缘切掉**的那种帧，水平锚点改用
                 "推算的完整身体中心"（= 右沿 - 完整宽/2）。这样运行时把它放在
                 **负坐标**（身体中心在屏幕左边缘外）时，"缺掉的那半边"正好在屏幕外。
    返回 (hs, ats)：每帧的主体高（源尺度）、每帧"正确"所在的画布 x
                    （ats[k] = 该帧应该画在 elfX_=ats[k] 处；进场帧的这个值可能是负的）
    """
    import numpy as np
    W, H = canvas
    hs, ats = [], []
    for n, i in enumerate(idx):
        rgb, a = cut(frame_paths[i])
        r = anchor_of(a)
        if not r:
            log('!! 第 %d 帧抠出来是空的' % (i + 1))
            sys.exit(1)
        (x0, y0, x1, y1), feet, cx = r
        if complete_w:
            # ★ 前提自检：进场帧必须是"真被画面左边缘切过"的（否则这套锚点没有意义）
            if x0 > 2:
                log('!! 进场帧 %d 的主体左沿 x0=%d（没被画面切过）—— 选帧区间错了' % (i + 1, x0))
                sys.exit(1)
            cx = x1 - (complete_w - 1) / 2.0      # 推算的"完整身体水平中心"（原片里是负的）
        ats.append(int(round(cx * s)))
        # 统一缩放（BOX = 面积平均；先缩再放，不拉伸）
        rgb = rgb.resize((int(rgb.width * s + 0.5), int(rgb.height * s + 0.5)), Image.BOX)
        a = a.resize(rgb.size, Image.BOX)
        sx = lambda v: int(v * s + 0.5)          # noqa: E731
        # 画布内位置：锚点（脚底水平中心）对齐到 (ax, ay)
        px = ax - sx(cx)
        py = ay - sx(feet)
        canvas_im = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        body = Image.merge('RGBA', rgb.split() + (a,))
        canvas_im.paste(body, (px, py))
        canvas_im.save(os.path.join(out, '%s_%02d.png' % (prefix, n)))
        bb = canvas_im.getbbox()
        hs.append(sx(y1 - y0 + 1))
        # 自检：不能被画布裁掉（顶/左右都要留白；底部留给阴影）
        if bb[0] < 1 or bb[1] < 1 or bb[2] > W - 1 or bb[3] > H - 1:
            log('!! %s_%02d 贴到画布边（bbox=%s canvas=%s）' % (prefix, n, bb, (W, H)))
            sys.exit(1)
    med = float(np.median(hs))
    spread = (max(hs) - min(hs)) / med
    log('  %-5s %2d 帧  缩放 %.3f  主体高 %d~%d（波动 %.1f%%）'
        % (prefix, len(idx), s, min(hs), max(hs), 100 * spread))
    if spread > check_h:
        log('  !! %s 主体高度波动 %.1f%% 超过 %.1f%% —— 逐帧归一化会让它"喘大气"，检查选帧'
            % (prefix, 100 * spread, 100 * check_h))
        sys.exit(1)
    return hs, ats


def make_desk_scene(path):
    """桌面场景：暗背景 + 木纹桌面 + 台灯暖光（整屏 480x540，**不透明**）。"""
    import numpy as np
    W, H = 480, 540
    img = Image.new('RGB', (W, H), (16, 13, 12))
    d = ImageDraw.Draw(img)
    import math
    # 背景：上暗下暖的垂直渐变
    for y in range(H):
        t = y / (H - 1.0)
        r = int(20 + 46 * t * t)
        g = int(15 + 30 * t * t)
        b = int(14 + 22 * t * t)
        d.line([(0, y), (W, y)], fill=(r, g, b))
    # 桌面：木纹（自上而下三条色带 + 板缝），从 y=210 起
    desk_y = 210
    plank_h = (H - desk_y) // 3
    base = [(92, 56, 34), (86, 51, 30), (78, 46, 27)]
    for k in range(3):
        y0 = desk_y + k * plank_h
        y1 = min(H, y0 + plank_h)
        for y in range(y0, y1):
            t = (y - y0) / max(1.0, (y1 - y0))
            c = base[k]
            # 木纹：横向细纹（用确定性的伪随机，保证可复现）
            grain = 6 * math.sin(y * 1.7 + k * 2.3) + 3 * math.sin(y * 5.1)
            shade = 1.0 - 0.18 * t
            d.line([(0, y), (W, y)], fill=(int((c[0] + grain) * shade),
                                          int((c[1] + grain * 0.7) * shade),
                                          int((c[2] + grain * 0.5) * shade)))
        d.line([(0, y0), (W, y0)], fill=(44, 27, 17))     # 板缝（暗线）
    # 台灯暖光：左上角大范围柔光（叠亮）
    light = Image.new('L', (W, H), 0)
    dl = ImageDraw.Draw(light)
    for r in range(240, 0, -6):
        v = int(60 * (1 - r / 240.0) ** 1.6)
        dl.ellipse([70 - r, 40 - r * 0.75, 70 + r, 40 + r * 0.75], fill=v)
    light = light.filter(ImageFilter.GaussianBlur(28))
    warm = Image.new('RGB', (W, H), (255, 196, 120))
    img = Image.composite(warm, img, light.point(lambda v: min(255, int(v * 1.1))))
    # 桌面高光（台灯打到木桌上的椭圆亮斑）
    glow = Image.new('L', (W, H), 0)
    dg = ImageDraw.Draw(glow)
    dg.ellipse([110, 250, 470, 470], fill=64)
    glow = glow.filter(ImageFilter.GaussianBlur(60))
    img = Image.composite(Image.new('RGB', (W, H), (255, 214, 150)), img, glow)
    img.save(path)
    return img


def make_shadow(path, w=140, h=30):
    """脚底阴影（单独一张；运行时整体上移时不会跟着飞）"""
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for i in range(14, 0, -1):
        t = i / 14.0
        d.ellipse([w / 2 - w / 2 * t, h / 2 - h / 2 * t, w / 2 + w / 2 * t, h / 2 + h / 2 * t],
                  fill=(0, 0, 0, int(90 * (1 - t) + 20)))
    im = im.filter(ImageFilter.GaussianBlur(6))
    im.save(path)
    return im


def spread_idx(lo, hi, n):
    """在源帧区间 [lo, hi]（1-based，闭区间）里**等间隔**取 n 帧，返回 0-based 下标。
    ⚠️ 别再用"固定步距"：区间一改，固定步距就会取不满/越界（走路帧上一版就是这么
       误取了"被画面左边缘切掉"的帧 ⇒ 走路循环里闪"缺半边"的身体）。"""
    if n <= 1:
        return [lo - 1]
    return [lo - 1 + int(round(k * (hi - lo) / float(n - 1))) for k in range(n)]


def ink_boxes(frame_paths, idx):
    """每个取样帧的主体 bbox（排掉右下角水印）——用于自检"完整 / 被切"。"""
    import numpy as np
    out = []
    for i in idx:
        _, a = cut(frame_paths[i])
        r = anchor_of(a)
        if not r:
            log('!! 第 %d 帧抠出来是空的' % (i + 1))
            sys.exit(1)
        out.append(r[0])
    return out


def translation_per_frame(frame_paths, idx):
    """相邻取样帧之间主体**整体平移**了多少（源 px）。
    ★ 用"列占用曲线的互相关"而不是 bbox 左沿：bbox 左沿会随姿态（手臂/腿）抖
      （实测相邻帧 4~28px），互相关取到的是**整体位移**，才是走路的真实速度。
    ★ 为什么先取列 profile 再做 1D 互相关：逐像素 np.roll 是 401 次 × 92 万元素的全图
      运算（Python 里要跑几十秒），列曲线只有 1280 点，瞬间出结果，且对竖直形变免疫。"""
    import numpy as np
    profs = []
    for i in idx:
        _, a = cut(frame_paths[i])
        profs.append((np.asarray(a) > 40).sum(axis=0).astype(np.float32))
    ds = []
    for k in range(1, len(profs)):
        a0, a1 = profs[k - 1], profs[k]
        best, bd = -1e18, 0
        for d in range(-200, 201):
            v = float((np.roll(a0, d) * a1).sum())
            if v > best:
                best, bd = v, d
        ds.append(bd)
    per_sample = sum(ds) / float(len(ds)) if ds else 0.0
    per_src = per_sample / ((idx[-1] - idx[0]) / float(len(idx) - 1)) if len(idx) > 1 else 0.0
    return per_sample, per_src


def main():
    measure_only = '--measure' in sys.argv
    import numpy as np
    log('=== 小精灵素材生成（源：%s）===' % VIDEO_DIR)
    f1 = ensure_frames(1)
    f2 = ensure_frames(2)
    log('  源帧：seg1 %d 帧 / seg2 %d 帧（1280x720 @24fps）' % (len(f1), len(f2)))

    # ---- 标定：静止态（用 seg2 末尾 20 帧）----
    log('标定（seg2 末尾 20 帧 = 站定态）：')
    rest_h, rest_w, feet, max_h2, max_w2 = measure(f2, 'seg2 末段', len(f2) - 20, len(f2))
    s = REST_H / float(rest_h)
    log('  静止态主体 %dx%d（脚底行 %d）⇒ 统一缩放比 %.4f（目标高 %d）'
        % (rest_w, rest_h, feet, s, REST_H))

    # ---- 移动段的最大尺寸（决定画布）----
    log('标定（seg1 平移段 %d~%d）：' % (MOVE_FROM, MOVE_TO))
    mh, mw, mf, max_h1, max_w1 = measure(f1, 'seg1 平移段', MOVE_FROM - 1, MOVE_TO)
    max_h = max(max_h1, max_h2)
    max_w = max(max_w1, max_w2)
    W = int(max_w * s + 0.5) + MARGIN_X * 2
    H = int(max_h * s + 0.5) + MARGIN_TOP + MARGIN_BOTTOM
    ax, ay = W // 2, H - MARGIN_BOTTOM
    log('  画布 %dx%d（主体最大 %dx%d × %.3f）锚点=(%d,%d) 脚底' % (W, H, max_w, max_h, s, ax, ay))

    # ---- 选帧 ----
    log('选帧：')
    idle_idx = pick_idle(f2)
    move_idx = spread_idx(MOVE_FROM, MOVE_TO, MOVE_N)
    enter_idx = spread_idx(ENTER_FROM, ENTER_TO, ENTER_N)
    log('  待机源帧号 %s' % [i + 1 for i in idle_idx])
    log('  走路源帧号 %s（区间 %d~%d 等间隔）'
        % ([i + 1 for i in move_idx], MOVE_FROM, MOVE_TO))
    log('  进场源帧号 %s（区间 %d~%d 等间隔；这批是"被画面左边缘切过"的姿态）'
        % ([i + 1 for i in enter_idx], ENTER_FROM, ENTER_TO))

    # ---- 自检：走路帧必须"完整"（没被画面左边缘切过）；进场帧必须"被切" ----
    box_m = ink_boxes(f1, move_idx)
    box_e = ink_boxes(f1, enter_idx)
    x0_m = [b[0] for b in box_m]
    x0_e = [b[0] for b in box_e]
    if min(x0_m) < 30:
        log('!! 走路帧里第 %d 源帧的主体左沿 x0=%d —— 被画面左边缘切过，'
            '走路循环里会闪"缺半边"的身体（选帧区间错了）'
            % (move_idx[x0_m.index(min(x0_m))] + 1, min(x0_m)))
        sys.exit(1)
    if max(x0_e) > 2:
        log('!! 进场帧里第 %d 源帧的主体左沿 x0=%d —— 没被画面切过，它就不是'
            '"从边上走进来"的姿态（选帧区间错了）'
            % (enter_idx[x0_e.index(max(x0_e))] + 1, max(x0_e)))
        sys.exit(1)
    log('  自检：走路帧 x0=%d~%d（都完整在画面内）✓；进场帧 x0=%d~%d（都被左边缘切过）✓'
        % (min(x0_m), max(x0_m), min(x0_e), max(x0_e)))
    # "完整身体"的宽度：取走路帧的中位宽（走路姿态下的完整宽，比站定态更贴）
    complete_w = int(np.median([b[2] - b[0] + 1 for b in box_m]))
    log('  完整身体宽（走路帧中位）= %d 源px ⇒ 进场帧的水平锚点按它推算"完整身体中心"' % complete_w)

    if measure_only:
        log('（--measure：只量不写）')
        return

    # ---- 出图 ----
    if os.path.isdir(OUTDIR):
        shutil.rmtree(OUTDIR)
    os.makedirs(OUTDIR)
    log('烘焙：')
    h_idle, _ = bake(f2, idle_idx, s, (W, H), ax, ay, 'idle', OUTDIR, 0.06)
    h_move, at_move = bake(f1, move_idx, s, (W, H), ax, ay, 'move', OUTDIR, 0.10)
    h_enter, at_enter = bake(f1, enter_idx, s, (W, H), ax, ay, 'enter', OUTDIR, 0.10,
                             complete_w=complete_w)
    log('  进场帧"正确"位置 kElfEnterAt = %s（画布 x；**负 = 身体中心还在屏幕左边缘外**）'
        % at_enter)
    make_desk_scene(os.path.join(OUTDIR, 'desk.png'))
    make_shadow(os.path.join(OUTDIR, 'shadow.png'))
    log('  桌面场景 desk.png 480x540（不透明，memcpy 快路径）')
    log('  脚底阴影 shadow.png 140x30')

    # ---- 配套的运行时常量（**由素材反推**，写进头文件 ⇒ C++ 里不再手抄）----
    step_src = (MOVE_TO - MOVE_FROM) / float(MOVE_N - 1)
    move_frame_ms = int(round(step_src / 24.0 * 1000.0))
    per_sample, per_src = translation_per_frame(f1, move_idx)
    walk_speed = int(round(per_src * 24.0 * s))
    if per_sample < 3:
        log('!! 走路帧之间的整体平移只有 %.1f 源px —— 这批帧可能不是"平移段"，检查选帧' % per_sample)
        sys.exit(1)
    log('  配套常量（由素材反推）：每帧 %.2f 源帧 = %dms；平移 %.1f 源px/取样帧'
        % (step_src, move_frame_ms, per_sample))
    log('    ⇒ kElfMoveFrameMs = %d，kElfWalkSpeed = %d px/s'
        '（%.2f 源px/源帧 × 24fps × %.4f 缩放）' % (move_frame_ms, walk_speed, per_src, s))
    log('    ⚠️ 时基与走速**必须成对**（帧间隔 × 走速 == 素材位移）⇒ 脚才不打滑')

    # ---- 自检：背景必须不透明；贴图必须有透明区（外轮廓非矩形）----
    bg = Image.open(os.path.join(OUTDIR, 'desk.png')).convert('RGB')
    assert bg.size == (480, 540)
    n_solid = 0
    for n in range(IDLE_N):
        im = Image.open(os.path.join(OUTDIR, 'idle_%02d.png' % n))
        na = sum(1 for p in im.getdata() if p[3] < 255)
        if na < 500:
            log('!! idle_%02d 几乎没有透明像素（%d）—— 抠像阈值可能失效' % (n, na))
            sys.exit(1)
        n_solid += 1
    log('  自检：%d 张 idle 帧都有透明区（外轮廓非矩形）✓' % n_solid)

    # ---- 预览拼版（给人眼确认；本脚本作者看不了图，靠数值自检）----
    #   ⚠️ 进场帧那几张**故意**是"缺半边"的（源片里主体被画面左边缘切掉）——
    #      看拼版时别以为抠坏了：它们只该用在"身体中心在屏幕左边缘外"的位置上。
    groups = [('idle', IDLE_N), ('move', MOVE_N), ('enter', ENTER_N)]
    cols = 6
    tw, th = W // 2, H // 2
    rows = (IDLE_N + MOVE_N + ENTER_N + cols - 1) // cols
    sheet = Image.new('RGB', (cols * tw, rows * th), (28, 28, 32))
    k = 0
    for prefix, cnt in groups:
        for i in range(cnt):
            if k >= cols * rows:
                break
            im = Image.open(os.path.join(OUTDIR, '%s_%02d.png' % (prefix, i))).resize((tw, th), Image.BOX)
            sheet.paste(im, ((k % cols) * tw, (k // cols) * th), im)
            k += 1
    sheet.save(PREVIEW)
    log('  预览：%s（顺序：idle_00..%02d、move_00..%02d、enter_00..%02d）'
        % (PREVIEW, IDLE_N - 1, MOVE_N - 1, ENTER_N - 1))

    # ---- 头文件 ----
    def emit(paths, n, prefix):
        return '\n'.join('    {"images/game/elf/%s_%02d.png", %d, %d, %d, %d},'
                         % (prefix, i, W, H, ax, ay) for i in range(n))

    with open(HEADER, 'w', encoding='utf-8') as fp:
        fp.write('''/*
 * PgElfArt.h - 小精灵素材清单（由 tools/gen_elf_from_video.py 生成，勿手改）
 *
 * 素材**不是画的**：由用户提供的实拍片段 `pet_video/desk-pet-seg*.mp4` 抠像而来
 * （黑底 + 固定机位；seg1 = 从左走进来并站定，seg2 = 站定后的待机）。
 * 生成链路：亮度阈值抠底 → 抹右下角固定水印 → 填内部洞 → 内缩防黑边 → 羽化
 *          → **一次统一缩放**（%0.4f，绝不逐帧归一化）→ 对齐进固定画布。
 *
 * ★★ 四条硬约束：
 *   1. 贴图尺寸 == 屏幕上贴的尺寸（1:1，不拉伸；具体值见 kElfW/kElfH）；
 *   2. 同一组内的帧**共用同一个缩放比与同一个锚点** ⇒ 换帧绝不跳位（锚点 = 脚底水平中心）；
 *   3. 桌面场景与阴影是**另外两张**：场景不透明（走 memcpy 快路径），阴影单独一张
 *      （这样角色整体上移时影子不会跟着飞）；
 *   4. ★ 时基（kElfMoveFrameMs）与走速（kElfWalkSpeed）**必须成对** —— 它们都由素材反推，
 *      帧间隔 × 走速 == 素材里的实际位移 ⇒ 脚才不打滑。改一个必须一起改（重跑生成器即可）。
 *
 * ★ 动画播放方式：**乒乓**（0→N-1→0）。源片段首尾并不重合（实测 seg2 首末 IoU 0.75），
 *   乒乓能保证接缝处不跳；走的动画则配合运行时的水平位移一起用。
 *
 * ★★ kElfEnter（"从屏幕最左边走出来"）：这套帧取自"主体被**视频画面左边缘切掉**"的那几帧
 *   （源帧 %d~%d）—— 它们的身体**天生缺半边**。烘焙时水平锚点取的是**推算的完整身体中心**
 *   （原片里是负的），所以：
 *     · kElfEnterAt[k] = 这张帧"正确"所在的画布 x（**负值 = 身体中心还在屏幕左边缘外**）；
 *     · 只有当精灵的 x ≈ kElfEnterAt[k] 时，"缺掉的那半边"才正好落在屏幕外 ⇒ 看着是完整的身体。
 *   ⇒ 运行时**按位置选帧**（不是按时间），见 PgElf.cpp 的 currentFrame()/enterFrameFor()。
 *   ⚠️ 绝不要把这套帧当普通走路帧用（那样会在屏幕中间露出一具"缺半边"的身体）。
 */
#ifndef PG_ELF_ART_H_
#define PG_ELF_ART_H_

#include "core/PgGameArt.h"

namespace pg {
namespace elfart {

typedef gameart::Def Def;

const int kElfW = %d;      // 单帧贴图尺寸（== 屏幕上的绘制尺寸）
const int kElfH = %d;
const int kElfAnchorX = %d;   // 锚点：脚底水平中心（图内坐标）
const int kElfAnchorY = %d;
const float kElfScale = %0.4ff;   // 源 -> 贴图的统一缩放比（仅供文档/换素材时参考）

const int kElfIdleN = %d;
const int kElfMoveN = %d;
const int kElfEnterN = %d;

// ★ 走路的时基与速度（由素材反推：每帧 %.2f 源帧 / 平移 %.1f 源px每取样帧）
const int kElfMoveFrameMs = %d;
const int kElfWalkSpeed = %d;     // px/s（画布尺度）

// 桌面场景（整屏 480x540；**不透明**）
const Def kElfDesk = {"images/game/elf/desk.png", 480, 540, 0, 0};
// 脚底阴影（锚点=中心）
const Def kElfShadow = {"images/game/elf/shadow.png", 140, 30, 70, 15};

// 待机（乒乓播放）
const Def kElfIdle[%d] = {
%s
};
// 走路（主体完整在画面内；运行时同时做水平位移）
const Def kElfMove[%d] = {
%s
};
// 进场：身体中心在屏幕左边缘附近/以外时用（缺的半边靠屏幕边缘"挡住"）
const Def kElfEnter[%d] = {
%s
};
// 每张进场帧"正确"所在的画布 x（负 = 身体中心在屏幕左边缘外）
const int kElfEnterAt[%d] = {%s};

}  // namespace elfart
}  // namespace pg

#endif  // PG_ELF_ART_H_
''' % (s, ENTER_FROM, ENTER_TO, W, H, ax, ay, s, IDLE_N, MOVE_N, ENTER_N,
       step_src, per_sample, move_frame_ms, walk_speed,
       IDLE_N, emit(f2, IDLE_N, 'idle'),
       MOVE_N, emit(f1, MOVE_N, 'move'),
       ENTER_N, emit(f1, ENTER_N, 'enter'),
       ENTER_N, ', '.join(str(v) for v in at_enter)))
    log('清单头：%s' % HEADER)
    log('=== 完成：%d 张角色帧（待机 %d + 走路 %d + 进场 %d）+ 场景 + 阴影 ==='
        % (IDLE_N + MOVE_N + ENTER_N, IDLE_N, MOVE_N, ENTER_N))


if __name__ == '__main__':
    main()
