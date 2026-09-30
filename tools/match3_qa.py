#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""match3_qa.py - 消消乐的真机验收辅助：从 QA 的 `gdbg board` 日志里算出"下一步该点哪"

为什么需要它：三消的验收不能靠盲点 —— 8x8 盘上"随便点两个相邻格"绝大多数是无效交换，
盲点 20 次也未必能验到一次连锁/过关分支。所以流程固定成：
    1) 设备侧  echo "gdbg board" > /tmp/pg_autostart
    2) PC 侧   adb logcat -d | grep -E "Match3: (board|power)" | 本脚本
    3) 本脚本输出可直接下发的 `gdbg swap r1 c1 r2 c2` 命令（**走与触摸同一条路径**）

用法：
    adb shell "echo 'gdbg board #n' > /tmp/pg_autostart"
    adb logcat -d | grep -E 'Match3: (board|power)' | tail -16 | python tools/match3_qa.py --cmds

    # 不想打命令、只想看盘面：去掉 --cmds
    # 抓屏逐像素判画面：
    python tools/match3_qa.py --pix docs/shot_xxx.png

输入行形如：
    D/zkgui ( 719): Match3: board[0] 0 4 6 4 6 0 3 6      ← 颜色（- = 空）
    D/zkgui ( 719): Match3: power[0] . . H . . R . .      ← 特殊块（H 横炸弹 / V 竖炸弹 / R 彩虹球）

★ 与游戏逻辑**同口径**的三条（对不上就会"脚本说能消、设备说不行"）：
  1. 3+ 同色横竖连线才算消；
  2. **彩虹球不参与颜色匹配**（它把连线断开，当障碍物）；
  3. 炸弹（H/V）**保留糖果色**，颜色层面照常参与匹配。
"""
import sys

R = C = 8
MIN_RUN = 3


def parse(lines):
    """返回 (grid, power)。grid 里 -1 = 空；power 里 '.' = 普通，H/V/R = 特殊块。"""
    grid = [[-1] * C for _ in range(R)]
    power = [["."] * C for _ in range(R)]
    for ln in lines:
        if "board[" in ln:
            tail = ln.partition("board[")[2]
            idx = int(tail.split("]")[0])
            cells = tail.split("]")[1].split()
            if 0 <= idx < R:
                for c, v in enumerate(cells[:C]):
                    grid[idx][c] = -1 if v == "-" else int(v)
        elif "power[" in ln:
            tail = ln.partition("power[")[2]
            idx = int(tail.split("]")[0])
            cells = tail.split("]")[1].split()
            if 0 <= idx < R:
                for c, v in enumerate(cells[:C]):
                    power[idx][c] = v
    return grid, power


def longest_run(b, pw):
    """盘上最长的同色连线长度（与 collectRuns 同口径：彩虹球隔断、空格不算）。
    用户找"能拼出 4 连/5 连"的交换时要用它 —— 3 连只是普通消，4 连才生成特殊块。"""
    best = 0
    for r in range(R):
        run = 1
        for c in range(1, C):
            ok = (b[r][c] >= 0 and b[r][c] == b[r][c - 1]
                  and pw[r][c] != "R" and pw[r][c - 1] != "R")
            run = run + 1 if ok else 1
            if run > best:
                best = run
    for c in range(C):
        run = 1
        for r in range(1, R):
            ok = (b[r][c] >= 0 and b[r][c] == b[r - 1][c]
                  and pw[r][c] != "R" and pw[r - 1][c] != "R")
            run = run + 1 if ok else 1
            if run > best:
                best = run
    return best


def has_match(b, pw):
    return longest_run(b, pw) >= MIN_RUN


def find_moves(b, pw, min_len=MIN_RUN):
    """所有"交换后能形成 >= min_len 连线"的相邻交换，返回 (r1,c1,r2,c2,连线长度)。

    试算时 **power 也要跟着换**（彩虹球会隔断，只换颜色会算出与设备不一致的结论）。
    min_len=4 就是"能生成特殊块"的那些机会（验收生成分支要用）。
    """
    out = []
    for r in range(R):
        for c in range(C):
            for dr, dc in ((0, 1), (1, 0)):
                r2, c2 = r + dr, c + dc
                if r2 >= R or c2 >= C:
                    continue
                b[r][c], b[r2][c2] = b[r2][c2], b[r][c]
                pw[r][c], pw[r2][c2] = pw[r2][c2], pw[r][c]
                ln = longest_run(b, pw)
                b[r][c], b[r2][c2] = b[r2][c2], b[r][c]
                pw[r][c], pw[r2][c2] = pw[r2][c2], pw[r][c]
                if ln >= min_len:
                    out.append((r, c, r2, c2, ln))
    out.sort(key=lambda m: -m[4])       # 连线越长越优先（便于验 5 连→彩虹球）
    return out


def pix_report(path):
    """判画面（抓屏）：**逐像素**验证"素材真的贴出来了"，而不是靠肉眼看图。

    抓屏是 480x800 的整屏，画布在屏幕 y=160..700（见 ui/main.html 的 GameCanvas）。
    判据（三条，任一不成立就说明贴图/布局坏了）：
      ① 8x8 格中心都能采到"糖果色"（高饱和、且不等于凹槽的浅米白）
      ② 采到的糖果色**分得开**（至少 4 种）—— 全盘一个色说明素材索引错了
      ③ 顶栏木牌区是棕色（(140,96,56) 附近）—— 说明顶栏画在了它该在的位置
    """
    from PIL import Image
    im = Image.open(path).convert("RGB")
    W, H = im.size
    if (W, H) != (480, 800):
        print("!! 抓屏尺寸异常 %dx%d（应为 480x800）" % (W, H))
        return 2

    CANVAS_Y = 160
    BX, BY, CELL = 16, 46, 56

    CANDY = [(200, 60, 60, "红"), (232, 140, 58, "橙"), (232, 190, 52, "黄"),
             (92, 176, 96, "绿"), (78, 146, 214, "蓝"), (150, 96, 190, "紫"),
             (228, 228, 234, "白")]
    SLOT = (238, 240, 230)         # 凹槽色（= M3_SLOT，用来判"这格是不是空的"）

    def avg(cx, cy, r=5):
        n = 0
        s = [0, 0, 0]
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if 0 <= x < W and 0 <= y < H:
                    p = im.getpixel((x, y))
                    s[0] += p[0]
                    s[1] += p[1]
                    s[2] += p[2]
                    n += 1
        return tuple(v // n for v in s)

    def dist(a, b):
        return sum((a[i] - b[i]) ** 2 for i in range(3))

    # 覆盖层（READY / OVER / PAUSED）是**整屏半透明黑遮罩**，会把所有采样色压暗
    # ⇒ 不还原就判不出糖果（实测：压暗 66% 后颜色全被分到"红/绿"）。
    # 基准取顶栏**木牌纯色区**（画布 (200,10) 一带，实测 RGB(140,96,56)）——
    # ⚠️ 别取 (240,22)：那里是进度条的深色底槽 (70,44,24)，会把基准算错、还原过度。
    bar = avg(200, CANVAS_Y + 10)
    shrink = min(1.0, max(0.05, bar[0] / 140.0))
    dim = shrink < 0.90
    k = 1.0 / shrink if dim else 1.0

    hits = []
    counts = {}
    empty = 0
    for r in range(R):
        for c in range(C):
            cx = BX + c * CELL + CELL // 2
            cy = CANVAS_Y + BY + r * CELL + CELL // 2
            col = avg(cx, cy)
            if dim:                       # 还原遮罩压暗
                col = tuple(min(255, int(v * k + 0.5)) for v in col)
            if dist(col, SLOT) < 900:
                empty += 1
                continue
            best = min(CANDY, key=lambda t: dist(col, t[:3]))
            hits.append((r, c, best[3]))
            counts[best[3]] = counts.get(best[3], 0) + 1

    print("棋盘格采样：命中糖果 %d / %d 格（%d 格采到的还是凹槽色）"
          % (len(hits), R * C, empty))
    print("糖果色分布：" + " ".join("%s=%d" % kv for kv in sorted(counts.items())))
    print("顶栏木牌采样 %s：RGB%s%s（原色应为 RGB(140,96,56)；明显偏暗 = 覆盖层遮罩生效）"
          % ((200, CANVAS_Y + 10), bar,
             "  遮罩亮度 %.0f%%" % (shrink * 100) if dim else ""))

    ok = True
    # 覆盖层（过关/失败）会在棋盘上**写大字**，被文字盖住的格子当然采不到糖果色
    # ⇒ 有遮罩时放宽到 75%，无遮罩时只允许 2 格误差。
    allowed = int(R * C * 0.25) if dim else 2
    if len(hits) < R * C - allowed:
        print("!! 有格子没采到糖果（贴图没铺满 / 坐标算错；允许 %d 格）" % allowed)
        ok = False
    if len(counts) < 4:
        print("!! 糖果种类过少（素材索引可能串了）")
        ok = False
    if not (bar[0] > bar[1] > bar[2]):
        print("!! 顶栏颜色不像木牌（可能顶栏没画 / 位置错）")
        ok = False
    print("画面判据：%s" % ("通过 ✓" if ok else "不通过 ✗"))
    return 0 if ok else 1


def main():
    if "--pix" in sys.argv:
        return pix_report(sys.argv[sys.argv.index("--pix") + 1])

    lines = sys.stdin.read().splitlines()
    grid, power = parse(lines)
    if all(v < 0 for row in grid for v in row):
        print("!! 没解析到棋盘（确认 logcat 里有 'Match3: board[N]' 行）")
        return 2

    for r in range(R):
        cells = []
        for c in range(C):
            ch = "." if grid[r][c] < 0 else str(grid[r][c])
            if power[r][c] != ".":
                ch = power[r][c]          # H/V/R 直接盖掉数字（看起来更直观）
            cells.append(ch)
        print("   " + " ".join(cells))
    n_pw = sum(1 for row in power for v in row if v != ".")
    print("特殊块 %d 个（H 横炸弹 / V 竖炸弹 / R 彩虹球）" % n_pw)
    print("初始是否已有三连（应为 False）: %s" % has_match(grid, power))
    # 默认列"能消"的交换；--long 只列"能生成特殊块"的（4 连及以上）
    min_len = 4 if "--long" in sys.argv else MIN_RUN
    moves = find_moves(grid, power, min_len)
    if min_len == 4:
        print("能凑出 4 连及以上的交换 %d 个（4 连→炸弹 / 5 连→彩虹球）" % len(moves))
    else:
        print("可行交换 %d 个（0 个 = 应当触发自动洗牌）" % len(moves))
    if "--cmds" in sys.argv:
        for r, c, r2, c2, ln in moves:
            print("gdbg swap %d %d %d %d      # 形成 %d 连" % (r, c, r2, c2, ln))
    else:
        for r, c, r2, c2, ln in moves[:5]:
            print("  swap %d %d -> %d %d（%d 连）" % (r, c, r2, c2, ln))
    return 0


if __name__ == "__main__":
    sys.exit(main())
