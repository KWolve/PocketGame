#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
rhythm_tap.py - 「下落式节奏」的**真触摸**按时敲键验收（节奏钢琴 / 打鼓）

为什么要它：`gdbg auto N` 虽然走的是同一条 tapLane/hitPad 路径，但那是"游戏自己点"。
本工具走的是**真实触摸注入**（`pginj tap /dev/input/event<n> x y`），证明"玩家手点也能命中"。

做法：轮询 QA（`gdbg q`）拿到**下一个待结算音符的屏幕 y**（v1.30.6 起 `disp` 行会打印它），
      按当前档位的下落速度算出"还有多少毫秒到判定线"，减掉 adb 往返（约 200ms）后落指。

用法:
    python tools/rhythm_tap.py <次> piano|drum [触摸节点]
例:
    python tools/rhythm_tap.py 3 piano /dev/input/event0

判据：PocketGame 日志出现 `Piano: 命中 #x lane=… -> 完美/不错/还行`（或 `Drum: 命中 …`）。
⚠️ 前置：游戏必须已经在 RUNNING（`enter <下标>` + `key a`），且**已过起播留白 1.5s**。
⚠️ 触摸节点不固定（本机 gt9xx→event0 / axs_ts→event4），用
   `grep -A6 -i gt9xx /proc/bus/input/devices` 确认。
"""
import re
import subprocess
import sys
import time

ADB = ["adb"]
CANVAS_TOP = 160          # 画布在屏幕里的 top（ui/main.json 的 GameCanvas.position.top）
TRAVEL = {"piano": (388, 416), "drum": (296, 322)}   # (判定线 y, 判定线+音符高)
# 下落时长表（毫秒）——必须与 `music::fallMs()` / `music::drumDiff()` 同源：
#   钢琴三档 {慢,中,快}；打鼓四档 {入门,慢,中,快}（入门 3600ms 见 core/PgMusic.h）
FALLS = {
    "piano": [2600, 1800, 1250],
    "drum": [3600, 2600, 1800, 1250],
}


def sh(args):
    return subprocess.run(ADB + args, capture_output=True, text=True, timeout=30).stdout


def qa(kind):
    """问一次 QA，返回 (diff1based, [(idx, lane, y), ...])（y 为画布坐标，diff 从 1 起）。"""
    tag = "q%d" % int(time.time() * 1000)
    sh(["shell", "echo 'gdbg q #%s' > /tmp/pg_autostart" % tag])
    time.sleep(0.35)
    out = sh(["logcat", "-d"])
    diff, notes = 0, []
    for line in out.splitlines():
        m = re.search(r"qa %s .*?diff=(\d+)" % kind, line)
        if m:                      # ★ 取最后一条（logcat -d 是整段历史）
            diff = int(m.group(1))
        m = re.search(r"qa %s disp\(mtAll=-?\d+\):(.*)" % kind, line)
        if m:
            notes = [(int(a), int(b), int(c)) for a, b, c in
                     re.findall(r"#(\d+) (?:lane|pad)=(\d+) y=(-?\d+)", m.group(1))]
    return diff, notes


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    want = int(sys.argv[1])
    kind = sys.argv[2].lower()
    dev = sys.argv[3] if len(sys.argv) > 3 else "/dev/input/event0"
    # 可选：**故意迟敲**这么多毫秒（用来验"某档判定窗口有没有放宽"：
    # 入门档 ok=300ms 时 +250ms 仍算「还行」；中档 ok=200ms 就变成"空敲/漏"）
    late_ms = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    if kind not in TRAVEL:
        print("kind 只能是 piano / drum")
        return 2
    judge_y, travel = TRAVEL[kind]

    got = 0
    seen = set()
    for _ in range(want * 6):
        if got >= want:
            break
        diff, notes = qa(kind)
        if not notes:
            time.sleep(0.3)
            continue
        idx, lane, y = max(notes, key=lambda t: t[2])       # 最靠下的那个音符
        if y >= judge_y or y < 180:
            time.sleep(0.15)
            continue
        px = lane * 60 + 30 if kind == "piano" else 3 + lane * 79 + 37   # 键/垫中心
        py = CANVAS_TOP + (430 if kind == "piano" else 56 + 302)
        # ★ 下落时长要跟着**当前档位**走（打鼓的入门档是 3600ms，用 1800 会算错落指时刻）
        falls = FALLS[kind]
        d = diff - 1 if 1 <= diff <= len(falls) else len(falls) - 1
        remain = (judge_y - y) * falls[d] / float(travel)
        time.sleep(max(0.0, remain / 1000.0 - 0.20 + late_ms / 1000.0))
        sh(["shell", "/data/pginj tap %s %d %d" % (dev, px, py)])
        time.sleep(0.45)
        out = sh(["logcat", "-d"])
        hit = None
        for line in out.splitlines():
            if ("Piano: 命中 #" in line or "Drum: 命中 #" in line) and line not in seen:
                hit = line
                seen.add(line)
        if hit:
            got += 1
            print("命中 #%d: note#%d lane=%d y=%d 预算剩余%.0fms -> tap(%d,%d)"
                  % (got, idx, lane, y, remain, px, py))
            print("        %s" % hit.split("): ", 1)[-1].strip())
        else:
            print("  未命中: note#%d lane=%d y=%d 预算剩余%.0fms -> tap(%d,%d)"
                  % (idx, lane, y, remain, px, py))
    print("真触摸按时命中一共 %d 次（目标 %d）" % (got, want))
    return 0 if got else 1


if __name__ == "__main__":
    sys.exit(main())
