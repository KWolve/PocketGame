#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""flip_qa.py - 「挂绳倒挂」整屏翻转的真机验收（逐像素，不靠"看着对"）

背景：用户需求「因为我的屏幕挂绳在底部，屏保和机器人界面按下 C 按键切换成倒 180 度显示」。
     生效范围（用户选定）= **只这两页**：屏保 + 机器人（电子宠物）；其它页面永远正向。

用法:
    python tools/flip_qa.py            # 全流程验收（约 60~90s，自己会 push pginj / 清 QA）

核心判据（比"读日志"可靠 —— 本板日志缓冲只有十几行，会被 SSDP 之类刷掉）：
    同一页面抓"翻转关/开"两帧 A、B，比两个差异：
        d_direct  = diff(A, B)            # 变了多少
        d_rot     = diff(rot180(A), B)    # 把 A 转 180° 后再比
    · **倒挂** ⇒ B ≈ rot180(A)  ⇒ d_rot 远小于 d_direct（实测差 3 个数量级）
    · **正向** ⇒ B ≈ A         ⇒ d_direct ≈ 0
    · 另有辅助判据：屏保帧的"黑 HUD 在顶还是底"（正向=顶、倒挂=底）。

覆盖的 6 个场景：
  ① 屏保：C 键翻转 → 单帧证明屏幕真的转了
  ② 屏保：C 键翻转**不唤醒**屏保（帧仍然是屏保画面，且不是主界面）
  ③ 屏保：再按 C 翻回来
  ④ 屏保：按别的键唤醒 → 屏幕自动恢复正向（离开环境页就还原）
  ⑤ 菜单：意愿开着也不翻转（范围门控）+ 帧逐像素不变
  ⑥ 机器人页：自动套用倒挂；短按 C 翻回正向；长按 C 仍是返回列表
"""
import os
import subprocess
import sys

from PIL import Image, ImageChops

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ADB = os.environ.get("PG_ADB", "D:/zkswe/FlyThingsPreview/sdk/platform-tools/adb/adb.exe")
OUT = os.path.join(ROOT, "out")
PY = sys.executable

fails = []
steps = []


def sh(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout).stdout


def dev(cmd):
    """在设备的 shell 里执行一句（⚠️ 本板 /bin/sh 没有 grep/head，过滤一律放 PC 侧）"""
    return sh([ADB, "shell", cmd])


def qa_saver(cmd):
    dev("echo '%s' > /tmp/pg_savercmd" % cmd)


def qa_main(cmd):
    dev("echo '%s' > /tmp/pg_autostart" % cmd)


def logcat(tail=200):
    return sh([ADB, "shell", "logcat -d -t %d" % tail], timeout=60)


def grab(name):
    p = os.path.join(OUT, name)
    subprocess.run([PY, os.path.join(ROOT, "tools", "grab.py"), p, "--tries", "2"],
                   capture_output=True, timeout=120)
    if not os.path.exists(p):
        raise SystemExit("抓屏失败：%s" % name)
    return Image.open(p).convert("RGB")


def diff(a, b):
    d = ImageChops.difference(a, b).convert("L")
    px = list(d.getdata())
    return sum(1 for v in px if v > 40)


def hud_at_top(im):
    """屏保帧：黑 HUD 在顶还是在底？返回 ('top'|'bottom', 上亮度, 下亮度)。"""
    def lum(box):
        c = im.crop(box)
        px = list(c.getdata())
        return sum(sum(p) / 3.0 for p in px) / len(px)
    up = lum((0, 0, 480, 40))
    dn = lum((0, 760, 480, 800))
    return ("top" if up < dn else "bottom"), round(up, 1), round(dn, 1)


def record(name, ok, detail):
    steps.append((name, ok, detail))
    print("  %s %s —— %s" % ("[PASS]" if ok else "[FAIL]", name, detail))
    if not ok:
        fails.append(name)


def main():
    os.makedirs(OUT, exist_ok=True)
    print("=== 挂绳倒挂 · 整屏翻转验收 ===")
    dev("chmod 755 /tmp/pginj 2>/dev/null")
    subprocess.run([ADB, "push", os.path.join(ROOT, "tools", "pginj"), "/tmp/pginj"],
                   capture_output=True)
    dev("chmod 755 /tmp/pginj")
    qa_main("dlna off #q0")      # 少点 SSDP 噪声（本板日志缓冲很小）
    qa_saver("#idle0")

    # ---------------- ① 屏保：C 键翻转 ----------------
    print("\n① 屏保里按 C 键 -> 倒 180°")
    qa_main("saver on #q2")          # ⚠️ 先进屏保：屏保没显示时 pg_savercmd 根本不被轮询
    subprocess.run(["sleep", "3.5"])
    qa_saver("flip off #q1")         # 确保从"正向"开始（否则抓到的参照帧本身就是倒的）
    subprocess.run(["sleep", "1.5"])
    a = grab("_flip_a.png")                      # 正向屏保
    dev("/tmp/pginj key /dev/input/event3 108 80")
    subprocess.run(["sleep", "0.6"])
    # ⚠️ 先读日志再抓屏：本板日志缓冲只有十几行，而抓屏要 1~2s，晚读这条就没了
    lg_after_c = logcat(60)
    b = grab("_flip_b.png")                      # 按 C 后的屏保
    d_direct, d_rot = diff(a, b), diff(a.rotate(180), b)
    at, up, dn = hud_at_top(b)
    record("① C 键把屏保转过去", d_rot * 5 < d_direct and at == "bottom",
           "d_direct=%d d_rot=%d（越小越像）；黑 HUD 在 %s（上 %.1f / 下 %.1f）"
           % (d_direct, d_rot, at, up, dn))

    # ---------------- ② 翻转不唤醒 ----------------
    print("\n② 翻转那一下不能把屏保唤醒")
    lg = lg_after_c
    i_flip = lg.rfind("C 键(108) -> 整屏翻转")
    tail = lg[i_flip:] if i_flip >= 0 else ""
    woke = ("任意键唤醒" in tail) or ("performScreensaverOff" in tail)
    # ⚠️ 判"画面仍是屏保"必须拿**转过 180° 的那一帧**去比（b 是倒着的屏保）：
    #    倒着的屏保 ≈ rot180(正向屏保)，而它跟正向屏保本身差得很远。
    like_saver = diff(b, a.rotate(180)) < diff(b, a)
    record("② 不唤醒", i_flip >= 0 and (not woke) and like_saver,
           "日志有翻转=%s；翻转后出现唤醒/收起=%s（应 False）；画面是倒着的屏保=%s"
           % (i_flip >= 0, woke, like_saver))

    # ---------------- ③ 再按 C 翻回来 ----------------
    print("\n③ 再按一次 C -> 翻回正向")
    dev("/tmp/pginj key /dev/input/event3 108 80")
    subprocess.run(["sleep", "1.5"])
    c = grab("_flip_c.png")
    record("③ 翻回正向", diff(a, c) * 5 < diff(a.rotate(180), c) and hud_at_top(c)[0] == "top",
           "与正向帧 diff=%d、与倒挂帧 diff=%d；黑 HUD 在 %s"
           % (diff(a, c), diff(a.rotate(180), c), hud_at_top(c)[0]))

    # ---------------- ④ 换个键唤醒 -> 自动恢复正向 ----------------
    print("\n④ 屏保处于倒挂时按音量键唤醒 -> 屏幕自动恢复正向")
    qa_saver("flip on #q4")          # 先让屏保真的倒着（否则这一步什么都没验到）
    subprocess.run(["sleep", "1.5"])
    d = grab("_flip_d.png")
    assert hud_at_top(d)[0] == "bottom", "准备阶段失败：屏保没有处于倒挂"
    dev("/tmp/pginj key /dev/input/event3 103 80")
    subprocess.run(["sleep", "0.7"])
    lg_wake = logcat(60)             # 唤醒后立刻读一次（缓冲小，晚了就没了）
    subprocess.run(["sleep", "1.3"])
    m = grab("_flip_m.png")          # 唤醒后的主界面（此刻还不知道屏幕正不正）
    qa_main("flip off #q7")          # 若它还倒着，这一步会把屏幕转回正向 -> 帧会变
    subprocess.run(["sleep", "1.5"])
    mref = grab("_flip_mref.png")    # 确定的"正向主界面"
    # 判据：唤醒后的帧 == 确定的"正向主界面"，而不像它转 180°（若唤醒了还倒着则相反）
    # ⚠️ 日志只作**参考信息**不当判据：本板缓冲十几行，晚读必丢（踩过两次假失败）。
    #    像素判据本身是自洽的：唤醒后若还倒着，mref 的"flip off"就会把屏幕转过来
    #    ⇒ 两帧必然对不上，这条就会 FAIL。
    ok_log = "环境页(saver=0 pet=0) 意愿=1" in lg_wake and "-> 实际 0°" in lg_wake
    d_same, d_rot = diff(m, mref), diff(m, mref.rotate(180))
    record("④ 退出屏保并恢复正向", d_same < d_rot,
           "唤醒帧 vs 正向主界面 diff=%d、vs 倒挂主界面 diff=%d；日志留痕=%s（仅参考）"
           % (d_same, d_rot, ok_log))

    # ---------------- ⑤ 菜单：意愿开着也不翻转 ----------------
    print("\n⑤ 菜单里（意愿=倒挂）屏幕必须保持正向")
    qa_main("flip on #q3")
    subprocess.run(["sleep", "1.5"])
    m2 = grab("_flip_m2.png")
    # ⚠️ 用容差而不是 ==0：导航栏上有分钟时钟，跨分钟那一下会跳字（几百像素），
    #    而"真被翻转"会差 **十几万** 像素 ⇒ 差一个数量级足够区分。
    d_same, d_rot = diff(mref, m2), diff(mref.rotate(180), m2)
    record("⑤ 范围门控（只这两页）", d_same < 3000 and d_same * 10 < d_rot,
           "flip on 前后两帧不同像素 %d（应≈0，仅导航栏时钟可能跳字）、vs 转 180° diff=%d"
           % (d_same, d_rot))

    # ---------------- ⑥ 机器人页 ----------------
    print("\n⑥ 机器人页：自动倒挂 + 短按 C 翻回 + 长按 C 返回")
    qa_main("33 1 #q4")          # slot 33 = 电子宠物
    subprocess.run(["sleep", "3.5"])
    qa_main("flip off #q5")      # 让机器人页回到正向，抓参照帧
    subprocess.run(["sleep", "1.5"])
    p0 = grab("_flip_p0.png")    # 正向机器人页
    qa_main("flip on #q6")
    subprocess.run(["sleep", "1.5"])
    p1 = grab("_flip_p1.png")    # 倒挂机器人页
    d_direct, d_rot = diff(p0, p1), diff(p0.rotate(180), p1)
    record("⑥a 机器人页自动套用倒挂", d_rot * 3 < d_direct,
           "d_direct=%d d_rot=%d（越小越像）" % (d_direct, d_rot))
    dev("/tmp/pginj key /dev/input/event3 108 80")   # 短按 C = 翻转
    subprocess.run(["sleep", "1.5"])
    p2 = grab("_flip_p2.png")
    record("⑥b 机器人页短按 C 翻回正向", diff(p0, p2) * 3 < diff(p0.rotate(180), p2),
           "与正向 diff=%d、与倒挂 diff=%d" % (diff(p0, p2), diff(p0.rotate(180), p2)))
    dev("/tmp/pginj key /dev/input/event3 108 900")  # 长按 C = 返回列表（>=700ms）
    subprocess.run(["sleep", "2.5"])
    p3 = grab("_flip_p3.png")
    record("⑥c 机器人页长按 C 仍回列表", diff(p3, mref) < diff(p3, p0),
           "当前画面更像主界面=%s（与正向主界面 diff=%d、与机器人页 diff=%d）"
           % (diff(p3, mref) < diff(p3, p0), diff(p3, mref), diff(p3, p0)))

    # ---------------- ⑦ 闹钟提醒页：必须正着显示 ----------------
    print("\n⑦ 屏保倒挂中响铃 -> 闹钟提醒页必须正着（抑制器）")
    qa_main("saver on #q8")          # 回到屏保
    subprocess.run(["sleep", "3"])
    qa_main("flip off #q8b")
    subprocess.run(["sleep", "1.2"])
    qa_main("alarm test #q8c")       # ① 正向下的闹钟页 = 参照帧
    subprocess.run(["sleep", "2.5"])
    u = grab("_flip_u.png")
    qa_main("ring stop #q8d")
    subprocess.run(["sleep", "1.5"])
    qa_main("flip on #q8e")          # 让屏保处于倒挂，再响一次
    subprocess.run(["sleep", "1.5"])
    qa_main("alarm test #q8f")       # ② 倒挂意愿 + 屏保显示中，闹钟页应把画面**拉回正向**
    subprocess.run(["sleep", "2.5"])
    s = grab("_flip_s.png")
    lg = logcat(400)
    ok_sup = "抑制=1" in lg and "-> 实际 0°" in lg
    record("⑦ 闹钟页抑制翻转（正着显示）", ok_sup and diff(s, u) < diff(s, u.rotate(180)),
           "抑制日志=%s；闹钟帧 vs 正向闹钟帧 diff=%d、vs 其转 180° diff=%d"
           % (ok_sup, diff(s, u), diff(s, u.rotate(180))))
    qa_main("ring stop #q8g")
    subprocess.run(["sleep", "1.8"])
    r = grab("_flip_r.png")
    record("⑦b 停铃后恢复倒挂（屏保仍在）", hud_at_top(r)[0] == "bottom",
           "停铃后黑 HUD 在 %s（应 bottom）" % hud_at_top(r)[0])

    # ---------------- 收尾 ----------------
    qa_main("flip off #q9")
    subprocess.run(["sleep", "1"])
    print("\n=== 结论 ===")
    for n, ok, _ in steps:
        print("  %s %s" % ("✓" if ok else "✗", n))
    print("通过 %d/%d" % (len(steps) - len(fails), len(steps)))
    if fails:
        print("失败项：%s" % "、".join(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
