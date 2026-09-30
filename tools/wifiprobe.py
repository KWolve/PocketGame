#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
wifiprobe.py - WiFi 信号探针工具包（PC 侧驱动，真机取数）

思路：设备上的 WiFi 应用（wifi.ftu）自带一条文件驱动的 QA 通道
（/tmp/pg_wificmd），本脚本通过 adb 让它扫一次、把结果 dump 到日志，
再把日志解析成表格 + 信道占用统计 + 报告文件。

为什么不让设备直接出报告：设备端字体是裁剪子集、屏幕只有 480x800，
  真正需要"读数据"的场合（选信道、看干扰）在 PC 上做更合适。
设备端也保留了一个「信号探针」页（点右下角"信号探针"）看现场实况。

用法:
  python tools/wifiprobe.py                     # 扫描并打印表格
  python tools/wifiprobe.py --out docs/probe.md # 额外输出 Markdown 报告
  python tools/wifiprobe.py --csv docs/ap.csv   # 额外输出 CSV
  python tools/wifiprobe.py --device 20080411 --wait 8

前置: 项目已 fun launch 过（/tmp/ui/wifi.ftu 在设备上），adb 能连上设备。
"""
import argparse
import os
import re
import subprocess
import sys
import time
from collections import Counter

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ADB_DEFAULT = os.environ.get(
    "PG_ADB", r"D:\zkswe\FlyThingsPreview\sdk\platform-tools\adb\adb.exe")

# wifiLogic.cc 里 dump 出去的行格式
RE_AP = re.compile(
    r"AP#(\d+) ssid='(.*?)' freq=(\d+) ch=(-?\d+) sec=(\S+) rssi=(-?\d+)")
RE_STATE = re.compile(
    r"state enable=(\d) connected=(\d) ssid='(.*?)' ip='(.*?)' ap=(\d+) probe=(\d)")
# 2.4G 常用不重叠信道
NON_OVERLAP_24G = [1, 6, 11]


def adb(dev, args, timeout=30):
    cmd = [ADB_DEFAULT, "-s", dev] + args
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return (r.stdout or "") + (r.stderr or "")


def sh(dev, script, timeout=30):
    return adb(dev, ["shell", script], timeout=timeout)


def fetch(dev, wait):
    """打开 WiFi 应用 -> 扫描 -> 读日志 -> 解析出 AP 列表"""
    ts = str(int(time.time()))

    # 1) 确认应用在跑（不在跑就没法接收命令）
    ps = sh(dev, "ps")
    if "zkgui" not in ps:
        print("!! 设备上 zkgui 没在运行 → 先执行 ./fun.exe launch")
        return None, []

    # 2) 打开 WiFi 应用（内容每行都不同，避免被去重）
    sh(dev, "echo wifiprobe%s > /tmp/pg_autostart" % ts)
    time.sleep(2.5)

    # 3) 清日志 + 下发 scan/dump
    sh(dev, "logcat -c")
    sh(dev, "echo scan > /tmp/pg_wificmd")
    sh(dev, "echo dump >> /tmp/pg_wificmd")
    sh(dev, "echo '#%s' >> /tmp/pg_wificmd" % ts)
    print("已下发扫描命令，等 %ds 让设备扫完..." % wait)
    time.sleep(wait)

    log = sh(dev, "logcat -d", timeout=60)
    aps = []
    for line in log.splitlines():
        m = RE_AP.search(line)
        if m:
            aps.append({
                "ssid": m.group(2),
                "freq": int(m.group(3)),
                "ch": int(m.group(4)),
                "sec": m.group(5),
                "rssi": int(m.group(6)),
            })
        if "wifiLogic: state " in line:
            pass
    # 4) 状态行（连接情况）另取一次，用于报告头
    state = None
    for line in log.splitlines():
        m = RE_STATE.search(line)
        if m:
            state = {
                "enable": m.group(1) == "1",
                "connected": m.group(2) == "1",
                "ssid": m.group(3),
                "ip": m.group(4),
                "total": int(m.group(5)),
            }
    # 去重（同一 SSID+信道可能被重复扫到，保留最强）
    best = {}
    for a in aps:
        k = (a["ssid"], a["ch"])
        if k not in best or a["rssi"] > best[k]["rssi"]:
            best[k] = a
    return state, sorted(best.values(), key=lambda x: -x["rssi"])


def band(a):
    return "5G" if a["freq"] >= 5000 else "2.4G"


def bars(v, lo=-95, hi=-25, width=24):
    """RSSI -> 条形图（越满越强）"""
    t = max(0.0, min(1.0, (v - lo) / float(hi - lo)))
    n = int(round(t * width))
    return "#" * n + "." * (width - n)


def report(aps, state):
    lines = []
    add = lines.append
    add("== WiFi 信号探针结果 ==")
    if state:
        add("状态: WiFi %s / %s%s%s" % (
            "开" if state["enable"] else "关",
            "已连接 " + state["ssid"] + " (" + state["ip"] + ")"
            if state["connected"] else "未连接",
            "" if state["total"] == len(aps) else
            "  [设备共扫到 %d 条，本报告去重后 %d 条]" % (state["total"], len(aps)),
            ""))
    add("共 %d 个 AP（2.4G %d / 5G %d）" % (
        len(aps), sum(1 for a in aps if band(a) == "2.4G"),
        sum(1 for a in aps if band(a) == "5G")))
    add("")
    add("%-4s %-6s %-30s %-8s %-6s %s" % ("#", "信道", "SSID", "加密", "RSSI", "强度"))
    for i, a in enumerate(aps):
        add("%-4d %-6s %-30s %-8s %-6d %s" % (
            i, ("CH%d" % a["ch"]) if a["ch"] > 0 else ("%dM" % a["freq"]),
            a["ssid"][:30], a["sec"], a["rssi"], bars(a["rssi"])))

    # 信道占用（2.4G 重点看 1/6/11）
    add("")
    add("-- 2.4G 信道占用 --")
    c24 = Counter(a["ch"] for a in aps if band(a) == "2.4G" and a["ch"] > 0)
    for ch in range(1, 15):
        if c24.get(ch):
            add("  CH%-3d %2d 个  %s" % (ch, c24[ch], "#" * c24[ch]))
    if not c24:
        add("  （现场没有 2.4G 网络）")
    add("  不重叠信道占用: " + "  ".join(
        "CH%d=%d" % (c, c24.get(c, 0)) for c in NON_OVERLAP_24G))
    free = [c for c in NON_OVERLAP_24G if c24.get(c, 0) == 0]
    if free:
        add("  --> 建议用 CH%d（1/6/11 里最空）" % free[0])
    else:
        bestch = min(NON_OVERLAP_24G, key=lambda c: c24.get(c, 0))
        add("  --> 1/6/11 都被占用，CH%d 相对最空（%d 个）" % (bestch, c24.get(bestch, 0)))

    # 5G 信道
    c5 = Counter(a["ch"] for a in aps if band(a) == "5G" and a["ch"] > 0)
    if c5:
        add("")
        add("-- 5G 信道占用 --")
        for ch in sorted(c5):
            add("  CH%-4d %2d 个  %s" % (ch, c5[ch], "#" * c5[ch]))

    # 加密分布
    add("")
    add("-- 加密方式 --")
    for k, v in Counter(a["sec"] for a in aps).most_common():
        add("  %-8s %d" % (k, v))
    return "\n".join(lines)


def markdown(aps, state, text):
    head = "# WiFi 信号探针报告\n\n"
    if state:
        head += ("- 设备 WiFi：%s\n- 连接：%s\n- IP：%s\n- 扫描到的 AP 条数：%d\n\n" % (
            "开" if state["enable"] else "关",
            (state["ssid"] if state["connected"] else "未连接"),
            state["ip"], state["total"]))
    head += "| # | 信道 | SSID | 加密 | RSSI | 频段 |\n|---|---|---|---|---|---|\n"
    for i, a in enumerate(aps):
        head += "| %d | %s | %s | %s | %d | %s |\n" % (
            i, a["ch"] if a["ch"] > 0 else "-", a["ssid"].replace("|", "\\|"),
            a["sec"], a["rssi"], band(a))
    head += "\n```\n" + text + "\n```\n"
    return head


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="20080411")
    ap.add_argument("--wait", type=int, default=7, help="扫描等待秒数")
    ap.add_argument("--out", help="输出 Markdown 报告路径")
    ap.add_argument("--csv", help="输出 CSV 路径")
    args = ap.parse_args()

    state, aps = fetch(args.device, args.wait)
    if not aps:
        print("没解析到 AP —— 检查：① 应用是否 fun launch 过 ② WiFi 开关是否打开")
        return 1

    text = report(aps, state)
    print(text)

    if args.out:
        path = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(markdown(aps, state, text))
        print("\n已写出 Markdown 报告: %s" % path)

    if args.csv:
        path = args.csv if os.path.isabs(args.csv) else os.path.join(ROOT, args.csv)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write("index,ssid,freq,channel,band,encryption,rssi\n")
            for i, a in enumerate(aps):
                f.write("%d,%s,%d,%d,%s,%s,%d\n" % (
                    i, a["ssid"], a["freq"], a["ch"], band(a), a["sec"], a["rssi"]))
        print("已写出 CSV: %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
