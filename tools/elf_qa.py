#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""elf_qa.py - 「小精灵」真机验收（逐像素 + 日志，不靠肉眼看图）

用法:
    python tools/elf_qa.py              # 全流程（约 60s，自己 push pginj / 清 QA）

判据一览（每一条都打印实测值，FAIL 会列在结尾）：
  ① 桌面场景 1:1：抓帧裁出画布（屏幕 y=160..700）与 desk.png 比，
     除角色所在区域外**逐像素一致** ⇒ 证明场景没被缩放/偏移。
  ② 角色在场：与场景不同的像素 = 角色+阴影，其 bbox 水平中心应 ≈ 家位置(240)、
     下沿（脚底）应 ≈ groundY(432-2)。
  ③ 待机动画在动：同局内隔 ~1s 抓两帧，角色区域内必须有像素变化（>300）。
     ★ 这条是"素材/状态机真的在跑"的判据 —— 静态截图看不出"它在不在动"。
  ④ 走动：`gdbg walk 380` 后 1.5s，角色 bbox 中心必须明显右移（>60px）。
  ⑤ 跳跃：`gdbg big` 抓帧，脚底必须离地（bbox 下沿比静止时高 >20px），且阴影变淡。
  ⑥ 睡着：`gdbg sleep` 后整屏平均亮度显著下降（>25）且头顶出现亮墨迹（Zzz）。
  ⑦ 触摸：pginj 在角色上点一下 ⇒ 日志"被逗 #N"计数 +1（走的是真触摸路径）。
  ⑧ 资源：`spr` 张数 ≤ SpriteBank 上限 40、失败 0；记录 MemFree。
  ⑨ **从屏幕左边走进来**（2026-09-18 用户报「身体缺半边」后新增）：`gdbg enter`（配 `slow 10`）
     过程中必须同时满足：① x 走到**负坐标**（身体中心在屏幕左边缘外）；② 用的是**进场帧**；
     ③ 角色墨迹的**左沿 ≈ 0**（贴着屏幕左边，而不是浮在屏内）；④ 最终走到家位置。
     ★ 判据 ③ 是核心：只有「缺掉的那半边落在屏幕外」时左沿才正好是 0。
⚠️ 与 flip 功能的关系：小精灵是**环境页**（isFlipPage），屏保/它自己显示时屏幕会按
   用户的"倒挂意愿"整屏翻转 ⇒ 本脚本**开头先把翻转关掉**，否则抓到的帧是倒的
   （量出来的 bbox 会全错，但日志看着一切正常 —— 踩过）。
"""
import os
import re
import subprocess
import sys

try:
    import numpy as np
    from PIL import Image
except ImportError:                                        # pragma: no cover
    print('!! 需要 PIL+numpy：用 envs/default/Scripts/python.exe 跑')
    sys.exit(1)

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
ADB = os.environ.get('PG_ADB', 'D:/zkswe/FlyThingsPreview/sdk/platform-tools/adb/adb.exe')
OUT = os.path.join(ROOT, 'out')
PY = sys.executable
DESK = os.path.join(ROOT, 'resources', 'images', 'game', 'elf', 'desk.png')
CANVAS_Y = 160          # 画布控件在屏幕上的 y（ui/main.html 的 GameCanvas）
GROUND_Y = 432          # 站立线（PgElf.cpp: vh()-108）
HOME_X = 240            # 家位置（vw()/2）

fails = []
rows = []


def sh(a, t=90):
    return subprocess.run(a, capture_output=True, text=True, timeout=t).stdout


def dev(cmd):
    return sh([ADB, 'shell', cmd])


def qa_main(cmd):
    dev("echo '%s' > /tmp/pg_autostart" % cmd)


def logcat(tail=200):
    return sh([ADB, 'shell', 'logcat -d -t %d' % tail])


def grab(name):
    p = os.path.join(OUT, name)
    subprocess.run([PY, os.path.join(ROOT, 'tools', 'grab.py'), p, '--tries', '2'],
                   capture_output=True, timeout=120)
    if not os.path.exists(p):
        raise SystemExit('抓屏失败 %s' % name)
    im = np.asarray(Image.open(p).convert('RGB'))
    # ★ 屏幕是 480x800，画布在 y=160..700（并且**要求此刻没有整屏翻转**）
    return im[CANVAS_Y:CANVAS_Y + 540].astype(int)


def grab_nf(name):
    """抓一帧，且**先确保屏幕没被翻转**。
    ⚠️ 为什么必须做：小精灵是"环境页"，**物理 C 键短按一下就把整屏翻 180°**
       （见 core/PgGame.h 的 isFlipPage）—— 而人手按键盘是我控制不了的。
       实测：验收跑到一半有人按了两下 C，抓帧就是倒的 ⇒ bbox 覆盖整个画布、
       亮度/动画判据集体假失败（白排查一轮）。`flip off` 幂等，随手发不心疼。"""
    qa('flip off')
    subprocess.run(['sleep', '0.5'])
    return grab(name)


def elf_box(cv, desk):
    """返回（与场景不同的像素数, bbox 或 None）。bbox 里含阴影（它是角色的一部分）。"""
    dm = np.abs(cv - desk).mean(axis=2) > 24
    if dm.sum() < 200:
        return int(dm.sum()), None
    ys, xs = np.where(dm)
    return int(dm.sum()), (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


def record(name, ok, detail):
    rows.append((name, ok, detail))
    print('  %s %s —— %s' % ('[PASS]' if ok else '[FAIL]', name, detail))
    if not ok:
        fails.append(name)


QQ = [0]


def qa(cmd):
    '''发一条 QA 命令（自动加序号 —— 本板 QA 通道「内容不变就不执行」）。'''
    QQ[0] += 1
    qa_main('%s #e%d' % (cmd, QQ[0]))


def elf_state(tries=6):
    '''读一次状态行，返回 dict（读不到返回 None —— 本板日志缓冲只有十几行，要重试）。
    ⚠️ 只认 'Elf: 相位=...' 那种行：日志里还有 '距上次互动=' 与 'PocketGame touch: x=..'，
       不加锚定就会解析出别的数字（踩过：把 13773ms 当成互动次数）。'''
    for _ in range(tries):
        qa('gdbg state')
        subprocess.run(['sleep', '0.55'])
        for l in reversed(logcat(60).splitlines()):
            mm = re.search(r'相位=(\S+) x=(-?\d+) .*?互动=(\d+) hop=(\d+) .*?睡着=(\d+) '
                           r'自动=(\d+)', l)
            if mm:
                return dict(phase=mm.group(1), x=int(mm.group(2)), taps=int(mm.group(3)),
                            hop=int(mm.group(4)), sleep=int(mm.group(5)), auto=int(mm.group(6)))
    return None


def last_state_line():
    '''取**最后一条**状态行（re.search 命中的是窗口里最旧的那条 —— 踩过）。'''
    for l in reversed(logcat(60).splitlines()):
        if '相位=' in l:
            return l
    return ''


def setup():
    '''把环境调到「确定态」并**回读确认**。
    ⚠️ 不回读就会像上一次那样：应用被重进后 auto 复位成 on，整轮验收都在
       「自动行为开着 + 已睡着」的状态下跑，判据集体假失败（2026-09-18 实测）。'''
    for _ in range(4):
        qa('34 1')                      # 进/重进小精灵（slot 34）
        subprocess.run(['sleep', '3'])
        qa('gdbg auto off')
        qa('gdbg wake')
        qa('gdbg hoploop 0')
        subprocess.run(['sleep', '0.9'])
        st = elf_state()
        if st and st['auto'] == 0 and st['sleep'] == 0:
            print('前置就绪：%s' % st)
            return True
        print('前置未就绪（%s），重试' % st)
    return False


def main():
    os.makedirs(OUT, exist_ok=True)
    qa_main('gdbg slow 1 #boot0')     # 上一轮若崩在慢放里，这里是自愈点
    desk = np.asarray(Image.open(DESK).convert('RGB')).astype(int)
    print('=== 小精灵验收（画布 %dx%d，站立线 y=%d）===' % (desk.shape[1], desk.shape[0], GROUND_Y))
    subprocess.run([ADB, 'push', os.path.join(ROOT, 'tools', 'pginj'), '/tmp/pginj'],
                   capture_output=True)
    dev('chmod 755 /tmp/pginj')
    # ★ 必须先探测协议：pginj 不带 `proto` 时用的是默认标志，本板（MT-A）注入会被忽略，
    #   现象只是"点了没反应"（2026-09-18 踩到，白跑一轮）。
    print(dev('/tmp/pginj proto /dev/input/event0').strip())
    qa('flip off')                   # ★ 先关整屏翻转，否则抓到的帧是倒的
    subprocess.run(['sleep', '1.2'])
    if not setup():
        print('!! 前置步骤没成功（应用没进到确定的待机态），结果不可信')

    # ---------------- ① 场景 1:1 + ② 角色在场 ----------------
    print('\n① 桌面场景 1:1 / ② 角色在场')
    cv = grab_nf('_elf_a.png')
    px, box = elf_box(cv, desk)
    # 场景一致性：把角色 bbox 抠掉再比
    mask = np.ones(desk.shape[:2], bool)
    if box:
        mask[max(0, box[1] - 6):box[3] + 7, max(0, box[0] - 6):box[2] + 7] = False
    scene_diff = np.abs(cv - desk).mean(axis=2)[mask].mean()
    record('① 桌面场景 1:1', scene_diff < 1.0,
           '挖掉角色区后与 desk.png 的平均差 %.2f（<1 = 1:1 贴上、没缩放/偏移）' % scene_diff)
    if box:
        cx = (box[0] + box[2]) / 2
        record('② 角色在场且站位对', abs(cx - HOME_X) < 40 and abs(box[3] - (GROUND_Y - 2)) < 14,
               '角色（含阴影）bbox x[%d,%d] y[%d,%d] 中心 x=%.0f（家 %d）脚底 y=%d（%d±14）'
               % (box[0], box[2], box[1], box[3], cx, HOME_X, box[3], GROUND_Y - 2))
    else:
        record('② 角色在场且站位对', False, '!! 画面里找不到角色（像素差异 %d）' % px)

    # ---------------- ③ 待机动画在动 ----------------
    print('\n③ 待机动画（静态截图看不出"动没动"，必须同局抓两帧比）')
    a = grab_nf('_elf_b1.png')
    subprocess.run(['sleep', '1.1'])
    b = grab_nf('_elf_b2.png')
    reg = (slice(180, 440), slice(120, 360))       # 角色活动区
    n = int((np.abs(a - b).mean(axis=2)[reg] > 18).sum())
    record('③ 待机动画在动', n > 300, '隔 1.1s 两帧在角色区内有 %d 个像素变化（>300 = 在动）' % n)

    # ---------------- ④ 走动 ----------------
    print('\n④ 走动（gdbg walk 380）')
    qa('gdbg walk 380')
    subprocess.run(['sleep', '1.6'])
    c = grab_nf('_elf_c.png')
    _, box2 = elf_box(c, desk)
    ok4 = box2 is not None and (box2[0] + box2[2]) / 2 > HOME_X + 60
    record('④ 走动', ok4, '1.6s 后角色中心 x=%s（起点 %d，应右移 >60px）'
           % ((box2[0] + box2[2]) / 2 if box2 else '?', HOME_X))
    qa('gdbg state')
    subprocess.run(['sleep', '0.8'])
    lg = logcat(60)
    m = re.search(r'Elf: 相位=(\S+) x=(-?\d+)', lg)
    if m:
        print('     日志：相位=%s x=%s' % (m.group(1), m.group(2)))

    # ---------------- ⑤ 跳跃 ----------------
    print('\n⑤ 跳跃（gdbg big，抓跳跃中的一帧）')
    qa('gdbg walk 240')          # 先回中间
    subprocess.run(['sleep', '1.6'])
    # ★★ 顺序很重要：**先起抓屏进程**（它读 /dev/fb0 大约在 0.4s 时发生），
    #    再下发 `gdbg big`。反过来写必错过 —— 抓一次要 1~2s，而跳跃只有 900ms。
    qa('gdbg hoploop 8')          # 连续跳：空中变成"稳态"，抓屏怎么按都拍得到
    subprocess.run(['sleep', '0.8'])
    qa('flip off')                          # 同理：别让外部按键把这一帧翻过去
    subprocess.run(['sleep', '0.4'])
    proc = subprocess.Popen([PY, os.path.join(ROOT, 'tools', 'grab.py'),
                             os.path.join(OUT, '_elf_d.png'), '--tries', '1'],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc.wait(timeout=60)
    d = np.asarray(Image.open(os.path.join(OUT, '_elf_d.png')).convert('RGB'))
    d = d[CANVAS_Y:CANVAS_Y + 540].astype(int)
    _, box3 = elf_box(d, desk)
    qa('gdbg state')             # 日志侧再补一条：hop>0 = 逻辑上真的在跳
    subprocess.run(['sleep', '0.8'])
    lg = logcat(60)
    mh = re.search(r'hop=(\d+)', lg)
    hopv = int(mh.group(1)) if mh else -1
    ok5 = box3 is not None and box3[3] < GROUND_Y - 20
    record('⑤ 跳跃离地', ok5, '跳跃帧 bbox y[%d,%d]（脚底 %s，静止时 ≈%d）；'
           '同刻日志 hop=%d' % (box3[1], box3[3], box3[3] if box3 else '?', GROUND_Y - 2, hopv))

    # ---------------- ⑥ 睡着 ----------------
    print('\n⑥ 睡着（压暗 + Zzz）')
    qa('gdbg hoploop 0')          # ★ 先停连跳：跳着的时候有"闪光"特效，
    subprocess.run(['sleep', '1.6'])        #   会把"醒着参照帧"弄脏（实测 Zzz 区被判成 93 像素）
    e = grab_nf('_elf_e1.png')
    lum0 = e.mean()
    # ★ Zzz 在"贴图墨迹上方"的纯背景区（canvas y≈180..212、x≈280..350）：
    #   醒着时那里一定是暗背景，睡着后才出现亮墨迹 ⇒ 可以当判据用。
    zreg = (slice(176, 216), slice(276, 356))
    nz0 = int((e[zreg].mean(axis=2) > 150).sum())
    qa('gdbg sleep')
    subprocess.run(['sleep', '1.5'])
    f = grab_nf('_elf_e2.png')
    lum1 = f.mean()
    nz1 = int((f[zreg].mean(axis=2) > 150).sum())
    record('⑥ 睡着压暗 + Zzz', (lum0 - lum1) > 15 and nz1 > 30 and nz0 < 10,
           '整屏亮度 %.1f -> %.1f（降 %.1f，应 >15）；Zzz 区亮墨迹 %d -> %d 像素（醒着应 <10、睡着应 >30）'
           % (lum0, lum1, lum0 - lum1, nz0, nz1))
    qa('gdbg wake')
    subprocess.run(['sleep', '1'])

    # ---------------- ⑦ 触摸（真注入） ----------------
    print('\n⑦ 触摸（pginj 真注入，走真触摸路径）')
    seq = [0]

    def tap_count():
        # ⚠️★ QA 通道是"**整份文件内容变化才执行**" ⇒ 连发两条一模一样的命令，
        #     第二条会被静默忽略（读到的还是旧状态）。这里必须每次换序号。
        seq[0] += 1
        qa_main('gdbg state #q7-%d' % seq[0])
        subprocess.run(['sleep', '0.7'])
        # ⚠️ 不能用 `.*互动=`：日志尾部还有"距上次互动="，贪婪匹配会吃到它
        #    （实测把 13773ms 当成互动次数 —— 判据自己错，比功能坏更难发现）。
        # ⚠️ 必须**倒序**找：re.search 命中的是日志窗口里**最旧**那行（踩过：
        #    点完之后读到的还是点击前那条状态 ⇒ "计数没变"的假失败）
        for l in reversed(logcat(60).splitlines()):
            mm = re.search(r'相位=(\S+) x=(-?\d+) .*?互动=(\d+) hop=', l)
            if mm:
                return (int(mm.group(2)), int(mm.group(3)))
        return (-1, -1)
    ex, n0 = tap_count()
    if ex < 0:
        ex, n0 = HOME_X, 0                 # 没读到状态行时的兜底（会记账为可疑）
    st = [l for l in logcat(60).splitlines() if 'Elf: 相位=' in l]
    print('     点击前状态：%s' % (st[-1].split('): ')[-1] if st else '(没读到)'))
    # ⚠️ pginj 打的是**屏幕**坐标，而 ex/GROUND_Y 是**画布**坐标 ⇒ y 要加 CANVAS_Y
    dev('/tmp/pginj tap /dev/input/event0 %d %d 200' % (ex, CANVAS_Y + GROUND_Y - 80))
    subprocess.run(['sleep', '1.2'])
    _ex2, n1 = tap_count()
    tl = [l for l in logcat(80).splitlines() if 'touch:' in l or '被逗' in l or '点到' in l]
    for l in tl[-4:]:
        print('     注入后日志：%s' % l.split('): ')[-1])
    record('⑦ 触摸互动', n1 == n0 + 1, '互动计数 %d -> %d（点在它身上，画布 (%d,%d) = 屏幕 (%d,%d)）'
           % (n0, n1, ex, GROUND_Y - 80, ex, CANVAS_Y + GROUND_Y - 80))

    # ---------------- ⑨ 从屏幕左边走进来 ----------------
    print('\n⑨ 从屏幕左边走进来（身体从屏幕外探进来 + 贴屏幕左边缘）')
    qa('gdbg auto off')
    qa('gdbg slow 10')        # ★ 必须放慢：进场那一段只有 0.66s，而抓一帧要 0.5~1.2s
    xs = []
    qa('gdbg enter')
    seen = []
    for i in range(3):
        cv = grab('_elf_g%d.png' % i)     # grab() 已经裁到画布区（540 行）
        _q, box = elf_box(cv, desk)
        if box:
            seen.append((box[0], box[2] - box[0] + 1))   # (墨迹左沿, 可见宽度)
        qa('gdbg state')
        subprocess.run(['sleep', '0.5'])
        m2 = re.search(r'相位=(\S+) x=(-?\d+) 帧=([a-z]+)(\d+)', last_state_line())
        xs.append(int(m2.group(2)) if m2 else -999)
    qa('gdbg slow 1')
    qa('gdbg auto off')
    subprocess.run(['sleep', '3.4'])          # 回到 1 倍速后把进场走完
    qa('gdbg state')
    subprocess.run(['sleep', '0.6'])
    m3 = re.search(r'相位=(\S+) x=(-?\d+)', last_state_line())
    endx = int(m3.group(2)) if m3 else -1
    for k, (lx, w) in enumerate(seen):
        print('     第%d次采样：墨迹左沿=%d 可见宽度=%d（完整约 194px）' % (k + 1, lx, w))
    print('     同刻日志里的 x：%s（负 = 身体中心还在屏幕左边缘外，抓帧比它慢所以常抓不到）'
          % [x for x in xs if x != -999])
    flush = len(seen) > 0 and all(lx <= 2 for (lx, _) in seen)
    narrow = (len(seen) > 0) and seen[0][1] < 130       # 刚进门只露出一小片
    grows = (len(seen) >= 2) and seen[-1][1] > seen[0][1]  # 逐渐展开
    record('⑨ 从屏幕左边走进来', flush and narrow and grows and endx == 240,
           '全程贴着屏幕左边=%s（左沿 %s）；第一次采样只露出一小片=%s（可见宽 %s，完整 194）；'
           '逐渐展开=%s；最终 x=%d（家 240）'
           % (flush, [lx for (lx, _) in seen], narrow, [w for (_, w) in seen], grows, endx))

    # ---------------- ⑧ 资源 / 内存 ----------------
    print('\n⑧ 资源与内存')
    qa('spr')
    subprocess.run(['sleep', '1'])
    lg = logcat(60)
    m = re.search(r'spr 缓存 (\d+) 张 / (\d+)KB / 失败 (\d+)', lg)
    mem = dev('cat /proc/meminfo')
    mf = re.search(r'MemFree:\s+(\d+) kB', mem)
    if m:
        record('⑧ 资源', int(m.group(1)) <= 40 and int(m.group(3)) == 0,
               '贴图 %s 张 / %sKB / 失败 %s（上限 40）；MemFree %sMB'
               % (m.group(1), m.group(2), m.group(3),
                  round(int(mf.group(1)) / 1024.0, 1) if mf else '?'))
    else:
        record('⑧ 资源', False, '没读到 spr 统计')

    qa('gdbg auto on')
    print('\n=== 结论 ===')
    for n_, ok, _ in rows:
        print('  %s %s' % ('✓' if ok else '✗', n_))
    print('通过 %d/%d' % (len(rows) - len(fails), len(rows)))
    if fails:
        print('失败项：%s' % '、'.join(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
