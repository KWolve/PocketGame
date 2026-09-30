#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
kids_qa.py - 儿童益智三件套（数字连线画 / 算术泡泡 / 找不同）的真机验收

为什么必须是脚本（而不是手点）：
  · **验收手段本身要可复现**（MEMORY.md 规则 14）。这三款的"该点哪里"都要先算出来 ——
    连线画要按 1..N 的顺序点、泡泡要算出答案在哪一列、找不同要知道差异的屏幕坐标；
  · 庆祝动画只有 1.7s，而抓屏要 2.7s —— 手工抢拍必失败，得并发抓。
  · 返回码 0 = 全绿；非 0 = 有断言失败（可直接进 CI/回归）。

用法：
  python tools/kids_qa.py                 # 全跑（connect / bubble / spot / ready 四段）
  python tools/kids_qa.py --only connect  # 只跑一款
  python tools/kids_qa.py --only ready    # 只查"READY 页文案会不会超出屏幕"
  ADB=adb python tools/kids_qa.py         # 指定 adb
"""
import os
import subprocess
import sys
import time

ADB = os.environ.get("ADB", "adb")
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "out")
PY = sys.executable

# 画布 480x540；屏幕 480x800，画布顶边在屏幕 y=160（实测）
CANVAS_TOP = 160
W_SCREEN = 480
CANVAS_H = 544

# 连线画的图案顶点，**与 src/core/PgConnect.cpp 的 kShapes 一致**（0..100 归一化，y 向下）
SHAPES = [
    ("三角形", [(50, 12), (86, 84), (14, 84)]),
    ("正方形", [(16, 16), (84, 16), (84, 84), (16, 84)]),
    ("小房子", [(50, 8), (88, 46), (88, 86), (12, 86), (12, 46)]),
    ("小鱼", [(8, 50), (26, 30), (50, 26), (86, 16), (74, 50), (86, 84), (50, 74), (26, 68)]),
    ("星星", [(50, 6), (61, 35), (92, 36), (67, 56), (76, 86), (50, 68), (24, 86), (33, 56),
              (8, 36), (39, 35)]),
    ("小花", [(50, 8), (63, 27), (86, 29), (76, 50), (86, 71), (63, 73), (50, 92), (37, 73),
              (14, 71), (24, 50), (14, 29), (37, 27)]),
]

fails = []
checks = 0

_seq = 0


def gdbg(word):
    """把 `gdbg <word>` 变成**每次都不同**的串。

    ⚠️⚠️ QA 通道的规矩是「**内容变化才执行**」⇒ 连发两条一模一样的 `gdbg answer`
    只有第一条会跑（本次实测：连对 3 次拿不到 combo，看着像"赞语没实现"）。
    游戏侧只取第一个词，所以尾随的序号是无害的。
    """
    global _seq
    _seq += 1
    return "gdbg %s %d" % (word, _seq)




def run(cmd, shell=False):
    return subprocess.run(cmd, shell=shell, capture_output=True, text=True)


def qa(cmd, wait=0.45):
    qa_nowait(cmd)
    time.sleep(wait)


def qa_nowait(cmd):
    """发一条 QA 命令（**自动加唯一序号**）。

    ★ 为什么必须唯一化：通道是"**内容变化才执行**"，连发同样内容只跑第一条 ⇒
      同一份脚本连跑两次、或同一款游戏连进两次时，第二条会被**静默忽略**，
      表现成"命令没生效"（像素类断言整片误报）。本脚本原来发的是裸 `enter 15`。
    """
    global _seq
    _seq += 1
    run([ADB, "shell", "echo '%s #q%d' > /tmp/pg_autostart" % (cmd, _seq)])


def back_to_list():
    """先"长按暂停键 ≈800ms"归一次状态，再进游戏。

    ★ 为什么必须：独立 ftu 页（wifi / camera / radio / iptv / probe / …）是**独立 Activity，
      会盖在主界面上层**，而主界面那条 `/tmp/pg_autostart` **照收命令、日志照样打
      `enter game[N] …`** ⇒ 出现"**命令进了、截图却是别人的页面**"。
      实测（2026-09-16）：本脚本在 wifi 页没退出的情况下跑出 **9 项颜色断言失败**
      （而功能类断言全过，非常有迷惑性）；长按返回归位后重跑 = **42/42 全绿**。
      本板 gpio-keys 只有 103/105/108（**没有 BACK 键**），但**每个独立页都实现了
      "长按 ≥700ms → closeActivity"**，所以这一下对任何页面都有效。"""
    run([ADB, "shell", "/data/pginj key /dev/input/event3 108 800"])
    time.sleep(1.2)


def logs(n=60):
    """取最近 n 行日志。

    ⚠️ **不要按 "PocketGame" 过滤**：游戏侧走 `pg::logInfo` 打出来的行（如 `qa q level=...`）
    没有那个前缀，滤掉就"永远查不到"，会被误判成"QA 钩子坏了"（本次实测踩过）。
    """
    out = run([ADB, "logcat", "-d", "-t", str(n)]).stdout.replace("\r", "")
    return out.splitlines()


def grab(name):
    path = os.path.join(OUT, name)
    run([PY, os.path.join(ROOT, "tools", "grab.py"), path])
    return path


def grab_nowait(name):
    path = os.path.join(OUT, name)
    return subprocess.Popen([PY, os.path.join(ROOT, "tools", "grab.py"), path]), path


def check(cond, what, detail=""):
    global checks
    checks += 1
    if cond:
        print("  [OK]   %s" % what)
    else:
        print("  [FAIL] %s  %s" % (what, detail))
        fails.append(what)


def px(path, x, y):
    from PIL import Image
    im = Image.open(path).convert("RGB")
    return im.load()[x, y]


def gscore():
    """取当前分数（走 QA，返回 (title, score, state)）"""
    qa("gscore")
    for l in reversed(logs(40)):
        if "gscore title=" in l:
            seg = l.split("gscore title=", 1)[1].split()
            d = {}
            # 游戏名是**位置参数**（`title=找不同 score=0`）—— 它没有 `=`，
            # 只按 `=` 切会把它整段丢掉（实测 d 里只剩 score/state）。
            if seg:
                d["title"] = seg[0]
            for s in seg[1:]:
                if "=" in s:
                    k, v = s.split("=", 1)
                    d[k] = v
            return d
    return {}



def count_color(path, cx, cy, color, rad=16, tol=8):
    """在半径 rad 的圆内数"接近 color"的像素个数。

    为什么不看单点：圆点中心画着数字，**单点探针很容易打在字形或抗锯齿边缘上**
    （实测把 wax(2) 打成了 50% 混合色）。数面积才能稳定判定"这块是不是这个颜色"。
    """
    from PIL import Image
    im = Image.open(path).convert("RGB")
    p = im.load()
    n = 0
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            if dx * dx + dy * dy > rad * rad:
                continue
            c = p[cx + dx, cy + dy]
            if all(abs(c[i] - color[i]) <= tol for i in range(3)):
                n += 1
    return n

def canvas_xy(u, v):
    """归一化 (u,v) 0..100 → 画布坐标（与 PgConnect::layout 一致）"""
    side = min(480 - 60, 540 - 190)
    if side > 420:
        side = 420
    bx = (480 - side) // 2
    by = 76
    return bx + u * side // 100, by + v * side // 100


# ------------------------------------------------------------------ 数字连线画
def test_connect():
    print("\n=== 数字连线画（slot 24，游戏分类第 15 项）===")
    back_to_list()
    qa("enter 15")
    time.sleep(1.2)
    check(any("数字连线画" in l for l in logs()), "能进入游戏且标题正确")

    qa("gscore")
    t = gscore()
    check(t.get("score") == "0", "开局分数为 0", str(t))

    # READY 覆盖层要压暗整屏（纸底被压到中灰）；点击后恢复亮纸底
    p_ready = grab("qa_kids_connect_ready.png")
    c_ready = px(p_ready, 30, 200)
    check(max(c_ready) < 200, "READY 覆盖层压暗画面", str(c_ready))

    qa("tap 240 270")  # 开始
    p_run = grab("qa_kids_connect_run.png")
    c_run = px(p_run, 30, 200)
    check(c_run == (253, 246, 227), "开始后是米黄纸底 #FDF6E3", str(c_run))

    # 第 1 个点（当前目标）应是蜡笔色 + 白字；其余点是"未连"卡色
    x1, y1 = canvas_xy(*SHAPES[0][1][0])
    x3, y3 = canvas_xy(*SHAPES[0][1][2])
    # 探针要**避开圆点中间的数字字形**（字形占 ~32x24，直接打中心会打到白字/深字）⇒ 数面积
    n1 = count_color(p_run, x1, y1 + CANVAS_TOP, (255, 209, 102))
    n3 = count_color(p_run, x3, y3 + CANVAS_TOP, (255, 251, 240))
    check(n1 > 150, "当前目标点已上色 wax(2)", "命中 %d px" % n1)
    check(n3 > 150, "未连点是卡色 #FFFBF0", "命中 %d px" % n3)

    # 连错：先点第 2 个点 → 分数不变、不重置（无惩罚）。
    # ⚠️ 提示只显示 1.7s 而抓屏要 2.7s ⇒ **必须并发抓**，先起抓屏再下发点击。
    x2, y2 = canvas_xy(*SHAPES[0][1][1])
    proc_w, p_wrong = grab_nowait("qa_kids_connect_wrong.png")
    qa_nowait("tap %d %d" % (x2, y2))
    proc_w.wait()
    t = gscore()
    check(t.get("score") == "0", "连错不扣分（score 仍 0）", str(t))
    # 提示文案应是红色 wax(0)=#FF6B6B（"先连前面的数字哦"）
    red = any(px(p_wrong, x, y) == (255, 107, 107)
              for y in range(600, 660) for x in range(100, 380))
    check(red, "连错时给出红色正向提示")

    # 按顺序点完第 1 个图案 → 分数 +1，且庆祝动画里有淡色填充
    qa("tap %d %d" % (x1, y1))
    qa("tap %d %d" % (x2, y2))
    proc, path = grab_nowait("qa_kids_connect_celeb.png")  # 并发抓，抢 1.7s 的庆祝
    qa_nowait("tap %d %d" % (x3, y3))
    proc.wait()
    t = gscore()
    check(t.get("score") == "1", "连完第 1 个图案 → 完成数 1", str(t))
    inner = px(path, 240, 300 + CANVAS_TOP)
    check(inner != (253, 246, 227), "完成时图案内部被填色", str(inner))

    # 一路点完 6 个图案 → 进"全部画完"覆盖层（这一条同时验证 6 组顶点数据都对）
    time.sleep(2.0)
    for name, pts in SHAPES[1:]:
        for (u, v) in pts:
            x, y = canvas_xy(u, v)
            qa("tap %d %d" % (x, y), wait=0.4)
        time.sleep(2.0)  # 等庆祝结束、自动切下一个
    t = gscore()
    check(t.get("score") == str(len(SHAPES)), "6 个图案全部连完", str(t))
    check(t.get("state") == "3", "进入 OVER（全部完成覆盖层）", str(t))
    p_all = grab("qa_kids_connect_alldone.png")
    check(px(p_all, 30, 200) != (253, 246, 227), "全部完成覆盖层压暗画面")

    # 大标题「全部画完啦！」是蜡笔绿 wax(3)（**彩色** ⇒ 中性色判据抓不到，得按颜色扫）。
    # 它原本是档 5：6 中文 × 80px = 480px = 满宽零留边 ⇒ 降档 4（384px）后左右各留 48px。
    from PIL import Image
    _im = Image.open(p_all).convert("RGB")
    _pp = _im.load()
    g = [(x, y) for y in range(320, 430) for x in range(W_SCREEN)
         if abs(_pp[x, y][0] - 123) <= 26 and abs(_pp[x, y][1] - 211) <= 26
         and abs(_pp[x, y][2] - 137) <= 26]
    if g:
        gx0 = min(t[0] for t in g)
        gx1 = max(t[0] for t in g)
        check(gx0 >= 24 and W_SCREEN - 1 - gx1 >= 24, "完成页大标题左右各留边 >= 24px",
              "x %d~%d（左 %d / 右 %d）" % (gx0, gx1, gx0, W_SCREEN - 1 - gx1))
    else:
        check(False, "完成页大标题（wax(3) 绿）可见", "没扫到")

    qa("glyphmiss")
    miss = [l for l in logs(40) if "glyphmiss" in l]
    check(any("0/0" in l or "miss=0" in l.replace(" ", "") for l in miss),
          "画布无缺字", str(miss[-1] if miss else "无输出"))


# ------------------------------------------------------------------ 算术泡泡
def test_bubble():
    print("\n=== 算术泡泡（slot 25，游戏分类第 16 项）===")
    back_to_list()
    qa("enter 16")
    time.sleep(1.2)
    check(any("算术泡泡" in l for l in logs()), "能进入游戏且标题正确")
    qa("tap 240 270")
    qa(gdbg("q"))
    line = [l for l in logs(40) if "qa q level=" in l]
    check(bool(line), "QA 能打印题目与选项", str(line))
    t0 = gscore()
    qa(gdbg("answer"))   # 点破正确泡泡那一列（与触摸同一条判定路径）
    t1 = gscore()
    check(int(t1.get("score", "0")) == int(t0.get("score", "0")) + 1,
          "点对 → 答对数 +1", "%s -> %s" % (t0, t1))
    # 连对到第 3 次应出赞语（文案是字面量）。赞语只显示 1s ⇒ **并发抓**
    time.sleep(0.8)
    qa(gdbg("answer"))
    time.sleep(0.8)
    # 赞语只显示 1s，而 QA 通道是**轮询执行**的（下发到执行有延迟）⇒ 抓一次不可靠。
    # 但 combo 已经 >=3，**之后每次答对都会再弹一次** ⇒ 重试抓拍，命中即通过。
    orange = 0
    for attempt in range(4):
        proc, p = grab_nowait("qa_kids_bubble_praise.png")  # 先起抓屏（cat 在 ~0.4s 发生）
        qa_nowait(gdbg("answer"))                          # 点击在 ~0.3s 被处理 ⇒ 落在 1s 窗口内
        proc.wait()
        # 赞语是橙色 wax(1)=#FF9F43，位于题目卡底部那一行（画布 y≈90 → 屏幕 ≈250）
        orange = sum(count_color(p, x, y, (255, 159, 67), 4, 12) > 0
                     for y in range(238, 285, 3) for x in range(140, 340, 3))
        if orange > 8:
            break
        time.sleep(0.5)
    check(orange > 8, "连对 3 次出现橙色赞语", "命中 %d 采样点（试了 %d 次）" % (orange, attempt + 1))

    # 点错分支：不扣分 + 红色提示（提示 1.5s ⇒ 并发抓）。
    # ⚠️ 单次抓拍会**偶发失败**（全量回归时设备更忙，实测同一条断言单跑过、全量跑挂）⇒
    # 点错没有代价，重试几次直到抓到；"不扣分"只判第一次（重试本身也点错，分数都该不变）。
    time.sleep(1.2)
    red = 0
    for attempt in range(4):
        ta = gscore()
        proc_w, p2 = grab_nowait("qa_kids_bubble_wrong.png")
        qa_nowait(gdbg("wrong"))
        proc_w.wait()
        tb = gscore()
        if attempt == 0:
            check(ta.get("score") == tb.get("score"), "点错不扣分", "%s -> %s" % (ta, tb))
        red = sum(count_color(p2, x, y, (255, 107, 107), 4, 12) > 0
                  for y in range(238, 285, 3) for x in range(140, 340, 3))
        if red > 8:
            break
        time.sleep(0.4)
    check(red > 8, "点错出现红色提示（再数一数）", "命中 %d 采样点（试了 %d 次）" % (red, attempt + 1))

    qa("glyphmiss")
    miss = [l for l in logs(40) if "glyphmiss" in l]
    check(any("0/0" in l or "miss=0" in l.replace(" ", "") for l in miss),
          "画布无缺字", str(miss[-1] if miss else "无输出"))


# ------------------------------------------------------------------ 找不同
def test_spot():
    print("\n=== 找不同（slot 26，游戏分类第 17 项）===")
    back_to_list()
    qa("enter 17")
    time.sleep(1.2)
    check(any("找不同" in l for l in logs()), "能进入游戏且标题正确")
    qa("tap 240 270")
    qa(gdbg("q"))
    q = [l for l in logs(40) if "qa q scene=" in l]
    check(bool(q), "QA 能打印本关差异坐标", str(q))
    p0 = grab("qa_kids_spot_start.png")
    # 两幅面板的底色应不同（上=天空 0xCFE8F7，下=草 0xC9E8B8 在底部）
    sky = px(p0, 240, 60 + CANVAS_TOP)
    grass = px(p0, 240, 460 + CANVAS_TOP)
    check(sky != grass, "上下两幅图底色不同", "%s vs %s" % (sky, grass))

    t0 = gscore()
    qa(gdbg("find"))
    t1 = gscore()
    check(int(t1.get("score", "0")) == int(t0.get("score", "0")) + 1, "找到一处 → 计数 +1",
          "%s -> %s" % (t0, t1))
    p1 = grab("qa_kids_spot_found.png")
    # 找到的差异会被红圈（wax(0)=#FF6B6B）圈出
    red = any(px(p1, x, y) == (255, 107, 107)
              for y in range(CANVAS_TOP, CANVAS_TOP + 480, 2)
              for x in range(0, 480, 2))
    check(red, "找到的差异被红圈标出")
    # 点空白：只有灰圈反馈，不扣分
    ta = gscore()
    qa("tap 470 20")
    tb = gscore()
    check(ta.get("score") == tb.get("score"), "点空不扣分", "%s -> %s" % (ta, tb))

    # 全部找完 → 庆祝并自动进下一关
    for _ in range(6):
        qa(gdbg("find"), wait=0.35)  # ★ 必须带唯一序号：QA 通道"内容变化才执行"
    time.sleep(2.2)
    qa(gdbg("q"))
    q2 = [l for l in logs(40) if "qa q scene=" in l]
    check(any("scene=1" in l for l in q2), "找全后自动进入第 2 关", str(q2[-1] if q2 else "无输出"))

    qa("glyphmiss")
    miss = [l for l in logs(40) if "glyphmiss" in l]
    check(any("0/0" in l or "miss=0" in l.replace(" ", "") for l in miss),
          "画布无缺字", str(miss[-1] if miss else "无输出"))


# ------------------------------------------------------------------ READY 页文案宽度
def ready_text_rows(path):
    """扫出 READY 画面里的浅色文字行段 → [(y0, y1, x0, x1)]。

    判据：**近中性（max-min ≤ 8）且亮（max ≥ 185）**。为什么是这个判据：
      · READY 的文字是**在压暗层之后再画的** ⇒ 是原色（白 255 / 浅灰 200,200,205）；
      · 被压暗的纸底是 (124,120,111) —— B 通道比 R 低 13，**不是中性色**，不会误命中；
      · 标题是蜡笔色（wax(n)，饱和度高）也不会命中。
    ⚠️ 别用"亮度阈值"硬扫：纸底 (124,120,111) 与白字压暗值 (125,125,125) 只差 14，
    本次实测第一版判据就是这么写错的（扫不到任何文字行）。
    """
    from PIL import Image
    im = Image.open(path).convert("RGB")
    p = im.load()
    W, H = im.size
    segs = []
    cur = None
    for y in range(CANVAS_TOP + 96, 700):     # 跳过 HUD 区
        xs = [x for x in range(W)
              if (max(p[x, y]) - min(p[x, y])) <= 8 and max(p[x, y]) >= 185]
        if len(xs) >= 4:
            if cur and y - cur[1] <= 4:
                cur[1] = y
                cur[2] = min(cur[2], min(xs))
                cur[3] = max(cur[3], max(xs))
            else:
                if cur:
                    segs.append(cur)
                cur = [y, y, min(xs), max(xs)]
        elif cur and y - cur[1] > 4:
            segs.append(cur)
            cur = None
    if cur:
        segs.append(cur)
    return segs


def test_ready():
    """READY 页文案**不许超出屏幕**。

    为什么单列一条：文案超宽时功能断言**全绿**（点击/计分都正常），只有肉眼看得出被裁。
    血案：算术泡泡说明行 11 中文 × 档 3(48px) = 528px > 画布宽 480 ⇒ 左右各裁 24px，
    用户报"开机页面文字太大超出了屏幕"。静态侧靠 `tools/check_textwidth.py` 拦，
    这里补**真机像素**侧的证据（两道闸，缺一不可）。
    """
    print("\n=== READY 页文案宽度（防\"文字超出屏幕\"）===")
    for idx, name in ((16, "算术泡泡"), (15, "数字连线画"), (17, "找不同"), (13, "消消乐")):
        back_to_list()
        qa("enter %d" % idx)
        time.sleep(1.3)
        t = gscore()
        check(name in t.get("title", ""), "%s：进的确实是这款（title=%s）" % (name, t.get("title")),
              str(t))
        p = grab("qa_kids_ready_%d.png" % idx)
        segs = ready_text_rows(p)
        check(len(segs) >= 2, "%s：READY 页认到 >=2 行文字" % name, str(segs))
        if not segs:
            continue
        left = min(s[2] for s in segs)
        right = W_SCREEN - 1 - max(s[3] for s in segs)
        check(left >= 24 and right >= 24, "%s：READY 文案左右各留边 >= 24px" % name,
              "左 %d / 右 %d；行段 %s" % (left, right, segs))


def main():
    only = None
    if "--only" in sys.argv:
        only = sys.argv[sys.argv.index("--only") + 1]
    os.makedirs(OUT, exist_ok=True)
    # ★ 先切到"游戏"分类：`enter <n>` 用的是**分类内下标**，不切就可能落在别的分类上
    #   （实测踩过：设备停在"系统"分类时 `enter 0` 直接进了 WiFi 页）。
    qa("cat 0")
    if only in (None, "connect"):
        test_connect()
    if only in (None, "bubble"):
        test_bubble()
    if only in (None, "spot"):
        test_spot()
    if only in (None, "ready"):
        test_ready()
    print("\n================  共 %d 项断言，失败 %d 项  ================" % (checks, len(fails)))
    for f in fails:
        print("  FAILED: %s" % f)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
