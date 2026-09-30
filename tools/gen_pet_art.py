#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_pet_art.py - 电子宠物素材生成器（调用 pet3d 离线 PBR 渲染 → 设备用 PNG + 清单）

素材不是 PIL 画的，而是 **headless Chrome + three.js 真渲染**出来的
（见 tools/pet3d/ 与 docs/pet-3d-render.md）。本脚本负责：
  ① 调 pet3d 渲染：8 张**空屏**身体帧 + 1 张阴影 + 8 张"品红屏幕"锚点帧 + 13 张表情层
  ② 量锚点帧里的屏幕四边形 → 得到"每个姿态下表情该贴在哪儿"
  ③ 缩放/自检 → 写 resources/images/game/pet/ + 生成 src/core/PgPetArt.h

★★ 为什么脸要单独一层（而不是烘进姿态帧）：
   烘进去 ⇒ 8 个姿态 × 12 个表情 = 96 张全尺寸贴图（约 20MB）；
   拆出来 ⇒ 8 张身体帧 + 13 张 100x63 的小贴图。
   而且"换表情"不用重烘 3D、不用重刷固件。
   pet3d 里的表情本来就是三层结构（眼睛/嘴/装饰），眨眼只换眼睛层。

用法:
  python tools/gen_pet_art.py                 # 全流程（需要 Chrome，约 50s）
  SKIP_RENDER=1 python tools/gen_pet_art.py   # 复用上次渲染结果，只重出图与清单
"""
import glob
import io
import os
import shutil
import subprocess
import sys

# 本机的 Pillow 装在系统 Python 的 site-packages 里，而受管 Python 的 user-site
# 在某些执行方式下不会被加进 sys.path（实测时好时坏）⇒ 这里显式兜一下。
try:
    from PIL import Image
except ImportError:                                        # pragma: no cover
    for _p in (r'C:\Users\Admin\Python\Python313\site-packages',):
        if os.path.isdir(os.path.join(_p, 'PIL')):
            sys.path.insert(0, _p)
    from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # PocketGame/
PET3D = os.path.join(ROOT, 'tools', 'pet3d')
NODE = 'C:/Users/Admin/.workbuddy/binaries/node/versions/22.22.2-2/node.exe'
OUTDIR = os.path.join(ROOT, 'resources', 'images', 'game', 'pet')
HEADER = os.path.join(ROOT, 'src', 'core', 'PgPetArt.h')

# ★★ 渲染取景尺寸（设备上的最终粘贴尺寸见 kPetW/kPetH）。
#
# ⚠️⚠️ **这个数是被内存卡住的，别随手放大**（2026-09-17 实测）：
#   贴图解码后占堆 = 帧数 × 裁剪后宽 × 高 × 4 字节，**和面积成正比**：
#     280x333（当前）→ 21 帧 ≈ 5.6MB，宠物全载后 MemFree ≈ 9.7MB，占画布宽 44%
#     345x410（试过）→ 21 帧 ≈ 7.9MB，宠物全载后 MemFree ≈ **2.7MB** ← 只剩这么点，太险
#   本板总共 56MB，而且这工程有过 OOM 血案（720p 硬解被内存挤死）。
#   只放大 1.25 倍换来 +8% 的画面占比、却吃掉 7MB 余量 ⇒ **不划算，已回退**。
#   ⇒ 真要把宠物做大，得**先减帧**（sleep/greet/angry/cry 各减一两个姿态），
#     或者接受"按动画分组只保留当前组"（切换时有解码卡顿）。
PET_W, PET_H = 280, 333
VISIBLE_UNITS = 37.9            # 取景里能看到多少个世界单位（= fitCamera 的 box 高 32.6 / padY 0.86）
SS = 2                           # 渲染超采样倍数
JUMP_HEADROOM = 32               # 裁剪时上方必须留的"跳跃净空"（像素）

BODY = ['idle_0', 'idle_1', 'idle_2', 'idle_3',
        'happy_0', 'happy_1', 'happy_2', 'happy_3',
        'sleep_0', 'sleep_1', 'sleep_2',
        'greet_0', 'greet_1', 'greet_2', 'greet_3',
        'angry_0', 'angry_1', 'angry_2',
        'cry_0', 'cry_1', 'cry_2']
EXPR = ['开心', '大笑', '爱心', '哭', '生气', '睡觉', '惊讶', '墨镜', '晕', '思考', '闪光', '害羞']
FACES = EXPR + ['blink']


def render():
    out = os.path.join(PET3D, 'devout')
    if os.path.isdir(out):
        shutil.rmtree(out)
    env = dict(os.environ, Q='mode=device')
    print('== 1/4 pet3d 渲染（headless Chrome + WebGL）==')
    r = subprocess.run([NODE, 'server.js', 'stills.html', 'devout',
                        str(PET_W * SS), str(PET_H * SS), '8752'],
                       cwd=PET3D, env=env, capture_output=True, timeout=600)
    tail = r.stdout.decode('utf-8', 'replace').strip().split('\n')[-3:]
    for line in tail:
        print('   ' + line)
    if r.returncode != 0:
        print('!! 渲染失败：\n' + r.stdout.decode('utf-8', 'replace')[-2000:])
        sys.exit(1)
    return out


def bbox(im, thr=8):
    px = im.load()
    W, H = im.size
    l, t, r, b = W, H, -1, -1
    for y in range(H):
        for x in range(W):
            if px[x, y][3] > thr:
                if x < l: l = x
                if x > r: r = x
                if y < t: t = y
                if y > b: b = y
    return (l, t, r, b) if r >= 0 else None


def check_solid(im, name, min_ratio=0.05):
    """实心（alpha>200）内容的包围盒 + 自检：非空、四边留白（没被裁）"""
    px = im.load()
    W, H = im.size
    l, t, r, b = W, H, -1, -1
    n = 0
    for y in range(H):
        for x in range(W):
            if px[x, y][3] > 200:
                n += 1
                if x < l: l = x
                if x > r: r = x
                if y < t: t = y
                if y > b: b = y
    if n < W * H * min_ratio:
        print('!! %s 几乎是空的（实心像素 %d）' % (name, n))
        sys.exit(1)
    if l < 2 or t < 2 or r > W - 3 or b > H - 3:
        print('!! %s 内容贴到图片边界 (%d,%d)-(%d,%d) —— 已被裁，检查相机构图'
              % (name, l, t, r, b))
        sys.exit(1)
    return l, t, r, b


def main():
    src = os.path.join(PET3D, 'devout')
    if os.environ.get('SKIP_RENDER') != '1':
        src = render()

    if os.path.isdir(OUTDIR):
        shutil.rmtree(OUTDIR)
    os.makedirs(OUTDIR)

    # ---------- 2/4 身体帧：缩放 + 自检 + 统一裁剪 ----------
    print('== 2/4 身体帧 ==')
    info = {}
    for name in BODY:
        f = os.path.join(src, 'pet_%s.png' % name)
        if not os.path.exists(f):
            print('!! 缺帧 %s' % f)
            sys.exit(1)
        im = Image.open(f).convert('RGBA').resize((PET_W, PET_H), Image.LANCZOS)
        l, t, r, b = check_solid(im, name)
        im.save(os.path.join(OUTDIR, '%s.png' % name))
        info[name] = (l, t, r, b)
        print('   %-10s 内容 (%3d,%3d)-(%3d,%3d)  %dx%d' % (name, l, t, r, b, PET_W, PET_H))

    idle_rows = [info[n][3] for n in BODY if n.startswith('idle')]
    if max(idle_rows) - min(idle_rows) > 2:
        print('!! idle 各帧脚底行不一致 %s —— 相机不是固定的，动画会"跳位"' % idle_rows)
        sys.exit(1)
    feet_y = sum(idle_rows) // len(idle_rows)

    # ★ 统一裁剪（8 张共用同一相机 ⇒ 共用同一裁剪矩形，锚点不会漂）。
    #   空白边距实测占 34% 像素，而贴图是逐像素 alpha 混合，每像素都是成本。
    #   ⚠️ 上方必须留够跳跃净空，否则跳起来时天线会被贴图自己的上边界切掉。
    cx0 = max(0, min(v[0] for v in info.values()) - 2)
    cx1 = min(PET_W - 1, max(v[2] for v in info.values()) + 2)
    cy0 = max(0, min(v[1] for v in info.values()) - JUMP_HEADROOM - 2)
    cy1 = min(PET_H - 1, max(v[3] for v in info.values()) + 2)
    crop_w, crop_h = cx1 - cx0 + 1, cy1 - cy0 + 1
    print('   裁剪 (%d,%d)-(%d,%d) -> %dx%d（原 %dx%d，像素 %d%%）'
          % (cx0, cy0, cx1, cy1, crop_w, crop_h, PET_W, PET_H,
             100 * crop_w * crop_h // (PET_W * PET_H)))
    for name in BODY:
        f = os.path.join(OUTDIR, '%s.png' % name)
        Image.open(f).crop((cx0, cy0, cx1 + 1, cy1 + 1)).save(f)

    # ---------- 3/4 阴影 + 锚点（屏幕四边形） + 表情层 ----------
    print('== 3/4 阴影 / 锚点 / 表情层 ==')
    sf = os.path.join(src, 'pet_shadow.png')
    sim = Image.open(sf).convert('RGBA').resize((PET_W, PET_H), Image.LANCZOS)
    bb = bbox(sim)
    if not bb:
        print('!! 阴影图全透明（光照没接上？）')
        sys.exit(1)
    sl, st, sr, sb = bb
    m = 2
    sl, st = max(0, sl - m), max(0, st - m)
    sr, sb = min(PET_W - 1, sr + m), min(PET_H - 1, sb + m)
    sh_w, sh_h = sr - sl + 1, sb - st + 1
    sim.crop((sl, st, sr + 1, sb + 1)).save(os.path.join(OUTDIR, 'shadow.png'))
    print('   %-10s (%3d,%3d)-(%3d,%3d)  %dx%d' % ('shadow', sl, st, sr, sb, sh_w, sh_h))

    # 锚点：品红屏幕的包围盒 = 该姿态下"脸"在画面上的位置
    anchor = {}
    for name in BODY:
        f = os.path.join(src, 'anchor_%s.png' % name)
        aim = Image.open(f).convert('RGBA').resize((PET_W, PET_H), Image.LANCZOS)
        px = aim.load()
        l, t, r, b = PET_W, PET_H, -1, -1
        for y in range(PET_H):
            for x in range(PET_W):
                cr, cg, cb, ca = px[x, y]
                if ca > 128 and cr > 150 and cb > 150 and cg < 110:      # 品红
                    if x < l: l = x
                    if x > r: r = x
                    if y < t: t = y
                    if y > b: b = y
        if r < 0:
            print('!! 锚点帧 %s 里没找到品红屏幕（solid 模式没生效？）' % name)
            sys.exit(1)
        # 存（屏幕中心 x, 屏幕中心 y, 宽, 高）—— 运行时按中心对齐贴表情层
        anchor[name] = ((l + r + 1) // 2, (t + b + 1) // 2, r - l + 1, b - t + 1)
        print('   anchor_%-10s 屏幕 (%3d,%3d)-(%3d,%3d)  %dx%d'
              % (name, l, t, r, b, r - l + 1, b - t + 1))

    # 分组起点：按名字前缀（idle_/happy_/sleep_/greet_）在 BODY 里的首次出现位置
    groups = []
    for name in BODY:
        g = name.split('_')[0]
        if g not in [x[0] for x in groups]:
            groups.append((g, BODY.index(name)))

    ws = sorted(v[2] for v in anchor.values())
    hs = sorted(v[3] for v in anchor.values())
    # ★★ 表情层尺寸取**最大**那个姿态的，锚点用**屏幕中心**：
    #   头一左右转（打招呼 headYaw 到 0.20），屏幕四边形在画面上既平移、宽度也会变
    #   （15 个姿态实测宽 84~96、高 57~67）。画布**不能缩放贴图** ⇒ 尺寸只能取一个常量，
    #   那就取最大的 + 按**中心**对齐 —— 小姿态下多出来的那圈是**透明的**（表情层只有
    #   字形、没有底），不会盖住屏幕边框；中心对齐则保证字形永远落在屏幕正中。
    #   （第一版用"中位数尺寸 + 左上角锚点"，打招呼那几帧字形会明显偏到左上角。）
    fw, fh = max(ws), max(hs)
    print('   表情层尺寸 = %dx%d（各姿态实测宽 %s 高 %s ⇒ 取最大 + 中心对齐）'
          % (fw, fh, ws, hs))

    # 表情层：256x163 的透明贴图缩到屏幕的实际像素尺寸
    for fname in FACES:
        f = os.path.join(src, 'face_%s.png' % fname)
        if not os.path.exists(f):
            print('!! 缺表情层 %s' % f)
            sys.exit(1)
        im = Image.open(f).convert('RGBA').resize((fw, fh), Image.LANCZOS)
        bb2 = bbox(im, 6)
        if not bb2:
            print('!! 表情层 %s 是空的' % fname)
            sys.exit(1)
        l, t, r, b = bb2
        if l < 1 or t < 1 or r > fw - 2 or b > fh - 2:
            print('!! 表情层 %s 的字形贴到边界 (%d,%d)-(%d,%d) —— 尺寸没算对'
                  % (fname, l, t, r, b))
            sys.exit(1)
        im.save(os.path.join(OUTDIR, 'face_%s.png' % fname))
    print('   %d 张表情层（12 表情 + 眨眼），字形均未贴边' % len(FACES))

    # ---------- 4/4 写清单 ----------
    print('== 4/4 写清单 ==')
    L = []
    A = L.append
    A('/*')
    A(' * PgPetArt.h - 电子宠物素材清单（由 tools/gen_pet_art.py 生成，勿手改）')
    A(' *')
    A(' * 素材不是 PIL 画的，是 tools/pet3d/ 用 **headless Chrome + three.js 真 PBR 渲染**出来的')
    A(' * （第一版手绘扁平矢量稿被用户否掉：「我想要的是更接近真实的」），见 docs/pet-3d-render.md。')
    A(' *')
    A(' * ★★ 素材分两层：**身体帧（屏幕是空的）** + **表情层（透明小贴图）**。')
    A(' *    8 个姿态 × 13 个表情 = 104 种组合，内存只要 8 + 13 张 ——')
    A(' *    表情只有 %dx%d，换表情不用重烘 3D、不用重刷固件。' % (fw, fh))
    A(' *    ⚠️ 身体帧的屏幕是空的 ⇒ **必须**在画完身体之后叠一张表情层，')
    A(' *       否则脸是黑的（典型"静默不显示"）。')
    A(' *')
    A(' * ★ 所有身体帧共用**同一个固定相机** ⇒ 贴图原点天然一致，**不需要逐帧锚点**；')
    A(' *   只有"表情层贴哪儿"要逐姿态给（头会点头/抬头，屏幕在画面上会移几个像素）。')
    A(' *')
    A(' * ★ 位移（呼吸起伏 / 跳跃轨迹）**没有烘在帧里**，是运行时算的：')
    A(' *   帧只负责"姿态"（手臂/头/天线的角度，画布算不了）。')
    A(' *')
    A(' * ★ 地面阴影是**单独一张**：否则运行时整体上移（跳跃）会把影子一起带上去。')
    A(' */')
    A('#ifndef PG_PET_ART_H_')
    A('#define PG_PET_ART_H_')
    A('')
    A('#include "core/PgGameArt.h"   // 复用 gameart::Def：贴图助手 blit/blitA 认这个类型')
    A('')
    A('namespace pg {')
    A('namespace petart {')
    A('')
    A('// 与 gameart::Def 同一个类型（ax/ay 一律 0：原点由运行时按原帧算）')
    A('typedef gameart::Def Def;')
    A('')
    A('const int kPetW = %d;        // 身体帧尺寸（已裁掉空白边距）' % crop_w)
    A('const int kPetH = %d;' % crop_h)
    A('const int kPetFrameW = %d;   // 渲染原始取景尺寸 + 裁剪偏移' % PET_W)
    A('const int kPetFrameH = %d;' % PET_H)
    A('const int kPetCropX = %d;' % cx0)
    A('const int kPetCropY = %d;' % cy0)
    A('const int kPetFeetY = %d;   // 脚底行（**原帧坐标系**）：把宠物"放在地上"用' % feet_y)
    A('// ★ 世界单位 -> 像素（= kPetFrameH / 取景里的世界单位数）。')
    A('//   **必须由生成器输出**：运行时拿它算呼吸起伏与跳跃的幅度；')
    A('//   手抄两份的话，改渲染取景（放大/缩小宠物）时会**静默不同步** ——')
    A('//   画面看上去"还能动"，很难联想到是常量没改。')
    A('const float kPetPxPerUnit = %.3ff;' % (PET_H / VISIBLE_UNITS))
    A('')
    A('const Def kPetShadow = {"images/game/pet/shadow.png", %d, %d};' % (sh_w, sh_h))
    A('const int kPetShadowDx = %d;' % sl)
    A('const int kPetShadowDy = %d;' % st)
    A('')
    A('// 表情层：%d 张（前 %d 个是表情，最后一个是眨眼）' % (len(FACES), len(EXPR)))
    A('const int kPetFaceW = %d;' % fw)
    A('const int kPetFaceH = %d;' % fh)
    A('const int kPetFaceN = %d;' % len(EXPR))
    A('const int kPetFaceBlink = %d;   // 眨眼那一张的下标' % len(EXPR))
    A('')
    A('// 每个姿态下**屏幕中心**在原帧坐标系里的位置（顺序同 kPetBody）。')
    A('// ★ 用中心而不是左上角：表情层尺寸是一个常量（取最大姿态），而各姿态的屏幕大小')
    A('//   会随头的转动变几个像素 —— 中心对齐才能保证字形永远落在屏幕正中间。')
    A('const int kPetFaceCX[%d] = {%s};' % (len(BODY), ', '.join(str(anchor[n][0]) for n in BODY)))
    A('const int kPetFaceCY[%d] = {%s};' % (len(BODY), ', '.join(str(anchor[n][1]) for n in BODY)))
    A('')
    A('const Def kPetBody[%d] = {' % len(BODY))
    for name in BODY:
        A('    {"images/game/pet/%s.png", %d, %d},' % (name, crop_w, crop_h))
    A('};')
    A('')
    A('const Def kPetFace[%d] = {' % len(FACES))
    for name in FACES:
        A('    {"images/game/pet/face_%s.png", %d, %d},' % (name, fw, fh))
    A('};')
    A('')
    A('// 表情名（HUD / QA 用；顺序同 kPetFace 的前 kPetFaceN 项）')
    A('const char *const kPetFaceNames[%d] = {%s};'
      % (len(EXPR), ', '.join('"%s"' % e for e in EXPR)))
    A('')
    A('// 身体帧的分组起点（生成器按名字前缀自动算，加姿态不用改运行时代码）')
    for g, i0 in groups:
        A('const int kPet%s0 = %d;' % (g.capitalize(), i0))
    A('const int kPetBodyN = %d;' % len(BODY))
    A('')
    A('}  // namespace petart')
    A('}  // namespace pg')
    A('')
    A('#endif  // PG_PET_ART_H_')
    A('')
    io.open(HEADER, 'w', encoding='utf-8', newline='\n').write('\n'.join(L))

    body_px = len(BODY) * crop_w * crop_h
    face_px = len(FACES) * fw * fh
    total = sum(os.path.getsize(p) for p in glob.glob(os.path.join(OUTDIR, '*.png')))
    print('   写 %s' % os.path.relpath(HEADER, ROOT).replace('\\', '/'))
    print('   %d 张身体帧 (%dx%d) + %d 张表情层 (%dx%d) + 阴影  PNG 合计 %.0fKB'
          % (len(BODY), crop_w, crop_h, len(FACES), fw, fh, total / 1024.0))
    print('   ★ 解码后占堆 %.2fMB（身体 %.2f + 表情 %.2f + 阴影 %.2f）'
          % ((body_px + face_px + sh_w * sh_h) * 4 / 1048576.0,
             body_px * 4 / 1048576.0, face_px * 4 / 1048576.0, sh_w * sh_h * 4 / 1048576.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
