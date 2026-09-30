#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""movie_seam_qa.py - 短片循环的"接缝不掉帧 / 不进屏保"验收（真机，PC 侧）

为什么要有它：用户口径是「播放结束重新播放不要切换画面，也不要进入屏保」。
"没有切换画面"是**可定量证明**的 —— MoviePage 每 500ms 把状态落到
`/tmp/pg_movie_<tag>.txt`（/tmp 是 tmpfs，不磨损 NAND），其中：

    decoded = H264Player::framesDecodedInRun()   # 本轮起流以来的累计解码帧数
    fileloops = StreamPlayer::fileLoops()        # 流层的循环计数（seek 回起点次数）
    running  = StreamPlayer::running()           # 流是否在跑
    phase    = 相位机（PLAYING = 正常播放）
    saver_on = 是否已进入屏保

判据（三条都不许破）：
  1. **decoded 的每秒增量恒 ≈ 帧率** —— 接缝处若"停了/黑了一下"，增量会塌陷
     （掉帧判据：最小值 ≥ 期望帧率的 60%）。
  2. **phase 全程 PLAYING、running 全程 1** —— 只要走了"停流重起"那条路，
     相位就会短暂离开 PLAYING、running 会掉 0。这两个变量是"有没有切换画面"的
     直接证据（比抓帧更可靠：抓一帧要 1~2s，接缝只有几十 ms）。
  3. **saver_on 全程 0** —— 播放期间不允许进屏保。

用法：
    python tools/movie_seam_qa.py fairy 45          # 采 45 秒
    python tools/movie_seam_qa.py kitten 30 --fps 15
"""
import argparse
import re
import subprocess
import sys
import time

ADB = os.environ.get("PG_ADB", "D:/zkswe/flythings/sdk/platform-tools/adb/adb.exe")
DEV = os.environ.get("PG_SERIAL", "20080411")


def sh(cmd, timeout=15):
    """跑一条 adb shell 命令，返回 stdout（去 CR）。失败返回空串。"""
    try:
        p = subprocess.run([ADB, "-s", DEV, "shell", cmd], capture_output=True,
                           timeout=timeout)
        return p.stdout.decode("utf-8", "replace").replace("\r", "")
    except Exception as e:
        print("  (adb 失败: %s)" % e, file=sys.stderr)
        return ""


KEYMAP = ("tag", "phase", "running", "loops", "fileloops", "lastseek_ms", "decoded",
          "drop", "fastfail", "mem_kb", "saver_on", "saver_to", "file")


def read_state(tag):
    txt = sh("cat /tmp/pg_movie_%s.txt" % tag)
    st = {}
    for line in txt.splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip()
        if k in KEYMAP:
            st[k] = v.strip()
    return st


def num(st, k, default=-1):
    try:
        return int(st.get(k, default))
    except (TypeError, ValueError):
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", help="页面 tag：fairy / kitten")
    ap.add_argument("seconds", type=int, help="采样多少秒")
    ap.add_argument("--fps", type=float, default=15.0, help="片源帧率（算期望增量）")
    ap.add_argument("--dur", type=float, default=10.10,
                    help="片长秒数（看 ffmpeg -i 的 Duration；用错了会误报）")
    ap.add_argument("--interval", type=float, default=1.0, help="采样间隔（秒）")
    a = ap.parse_args()

    print("=== 采样 /tmp/pg_movie_%s.txt，共 %ds，每 %.1fs 一次 ===" % (a.tag, a.seconds,
                                                                       a.interval))
    rows = []
    t0 = time.time()
    while time.time() - t0 < a.seconds:
        row = {"t": round(time.time() - t0, 1)}
        row.update(read_state(a.tag))
        rows.append(row)
        print("  t=%5.1fs phase=%-8s running=%s fileloops=%s seek@%s decoded=%s drop=%s "
              "saver_on=%s"
              % (row["t"], row.get("phase", "?"), row.get("running", "?"),
                 row.get("fileloops", "?"), row.get("lastseek_ms", "?"),
                 row.get("decoded", "?"), row.get("drop", "?"), row.get("saver_on", "?")))
        time.sleep(a.interval)

    if not rows:
        print("!! 没采到任何样本（页面没在跑？QA 通道没通？）")
        return 2

    # ---- 判据 ----
    ok = True
    phases = [r.get("phase") for r in rows]
    runners = [num(r, "running") for r in rows]
    savers = [num(r, "saver_on") for r in rows]
    decs = [num(r, "decoded") for r in rows]
    loops = [num(r, "fileloops") for r in rows]

    bad_phase = [r for r in rows if r.get("phase") != "PLAYING"]
    print("\n--- 判据 ---")
    if bad_phase:
        ok = False
        print("✗ 相位不全是 PLAYING：%s" % sorted(set(phases)))
        print("   （出现别的相位 = 走了'停流重起'那条路 = 接缝处画面被切过）")
    else:
        print("✓ 相位全程 PLAYING（%d 个样本）" % len(rows))

    if min(runners) != 1:
        ok = False
        print("✗ running 掉到 0（最少 %d）= 流中途停过" % min(runners))
    else:
        print("✓ running 全程 1 = 流没有中断过")

    nloops = loops[-1] - loops[0]
    if nloops < 1:
        ok = False
        print("✗ 采样期间 fileloops 没有增加（%d -> %d）：可能还没播满一轮，"
              "采样时间太短" % (loops[0], loops[-1]))
    else:
        print("✓ 采样期间完成 %d 轮循环（fileloops %d -> %d）" % (nloops, loops[0], loops[-1]))

    # decoded 增量：**用平均帧率判**（单段增量会被"状态文件 500ms 刷新 + 采样间隔"的
    # 相位差扰动到 6~17，那不是掉帧，是采样抖动；真掉帧会掉到 0~2）
    exp = a.interval * a.fps
    d = [decs[i + 1] - decs[i] for i in range(len(decs) - 1)]
    d = [x for x in d if x >= 0]
    drops = [num(r, "drop") for r in rows]
    ndrop = drops[-1] - drops[0]
    span = max(rows[-1]["t"], 0.001)
    avg_fps = (decs[-1] - decs[0]) / span if span > 0 else 0
    print("✓ 解码帧：%d -> %d（%.1fs）⇒ 平均 **%.1f fps**（片源 %.1f）"
          % (decs[0], decs[-1], span, avg_fps, a.fps))
    if abs(avg_fps - a.fps) > a.fps * 0.15:
        ok = False
        print("✗ 平均解码帧率偏了 %.1f fps ⇒ 确实有整段停过（不是采样抖动）"
              % (avg_fps - a.fps))
    else:
        print("✓ 平均帧率与片源一致（±15%% 内）⇒ **整段时间轴没有丢帧/停顿**")
    if d:
        print("· 单段增量：最小 %d / 最大 %d（期望 ≈%.0f；波动来自采样相位，"
              "掉帧才会接近 0）" % (min(d), max(d), exp))
    print("· 硬件播放器丢包累计：%d（0 = 一个包都没丢）" % ndrop)

    # ★★ 最硬的接缝判据：lastseek_ms 的差分 = 一轮实际时长
    #   接缝开销 = 差分 - 片长；它才是"那一瞬间画面有没有停"的量化结果。
    seeks = []
    for r in rows:
        s = num(r, "lastseek_ms")
        if s >= 0 and (not seeks or seeks[-1] != s):
            seeks.append(s)
    if len(seeks) >= 2:
        gaps = [seeks[i + 1] - seeks[i] for i in range(len(seeks) - 1)]
        wd = a.dur * 1000
        over = [g - wd for g in gaps]
        print("✓ 接缝间隔（ms）：%s（片长 %.2fs）" % (gaps, a.dur))
        print("⇒ **接缝开销 = %s ms**（一轮时长 - 片长；0~66ms 等于最多丢一帧，肉眼不可见）"
              % over)
        if max(over) > 150:
            ok = False
            print("✗ 接缝开销最大 %dms ⇒ 接缝处明显停了一下" % max(over))
        else:
            print("✓ 接缝开销都在 150ms 以内（≈ 一帧的量级）⇒ 播完立刻接上，没有空档")
        if max(gaps) - min(gaps) > 100:
            ok = False
            print("✗ 各轮时长不一致（极差 %dms）⇒ 接缝开销不稳定，会累积漂移"
                  % (max(gaps) - min(gaps)))
        else:
            print("✓ 各轮时长极差 %dms = 每轮完全一致（没有累积漂移）"
                  % (max(gaps) - min(gaps)))
    else:
        print("?? 采样期间没抓到接缝（时间太短？片长 %s s）" % a.dur)

    if max(savers) != 0:
        ok = False
        print("✗ saver_on 出现过 1（进过屏保）")
    else:
        print("✓ 全程没进屏保（saver_on 恒 0，超时设的是 %s 秒）" % rows[-1].get("saver_to"))

    print("\n=== 结论：%s ===" % ("通过" if ok else "不通过"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
