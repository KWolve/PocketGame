#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
ime_qa.py - 真机竣工验收：给自研输入法（ime.ftu）喂拼音，把候选**读回来**

为什么要有它：输入法的正确性在 PC 侧**证明不了** ——
  · 候选表是生成物（src/logic/imePinyinData.h），PC 侧只能证明"表里有这个字"；
  · 真机上还可能被 `kMaxCand`（运行时候选上限）/ `kCandPerPage`（一层几格）截掉，
    或者字库缺字形导致"选中后整个字消失"。
  ⇒ 必须走到设备上、让**真键盘**吃一遍键、再把候选 dump 出来对。

实现方式：复用 imeApp.cc 自带的 QA 通道（/tmp/pg_imecmd → /tmp/pg_ime_dump.txt），
键盘输入全程不模拟触摸（只有"拉起输入法"那一下点了输入框）。与触摸共用 handleKey。

⚠️ IME 的 QA 通道**不剥行尾 `#注释`**（与 HA 的 /tmp/pg_hacmd 不同）：
   `type keting #12` 会把 `#12` 也当键喂进去（'#'、'1'、'2' 都是键盘上有的键）。
   ⇒ 去重标记必须写成**单独一行**（整行以 # 开头会被跳过）。

用法:
  python tools/ime_qa.py xin                  # 喂 "xin"，打印候选
  python tools/ime_qa.py keting --shot        # 顺便抓屏（供像素级验收）
  python tools/ime_qa.py xin --serial 20080411
  python tools/ime_qa.py --raw $'clear\ntype yue\ndump\n#x'   # 直接发多行 QA

退出码：0 = 拿到了候选；2 = 中途失败。
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ENV = dict(os.environ, MSYS_NO_PATHCONV="1")
CMD_FILE = os.path.join(ROOT, ".shots", "_imeqa.txt")
HA_NAME_PAGE = 2          # haLogic.cc 的 `enum Page { PG_HOME=0, PG_PICK, PG_NAME, ... }`


def pick_device(serial):
    """★ 与 tools/grab.py 同一条纪律：**绝不"随便挑一台"** ——
    现场常常同时连着目标板和另一台面板，挑错就是把别人的机器当验收对象。"""
    p = subprocess.run(["adb", "devices"], capture_output=True, text=True, env=ENV,
                       encoding="utf-8", errors="replace")
    devs = [ln.split()[0] for ln in p.stdout.splitlines()[1:] if ln.strip().endswith("device")]
    if serial:
        if serial not in devs:
            print("!! 指定的设备 %s 不在 adb 列表里：%s" % (serial, devs))
            return None
        return serial
    if len(devs) == 1:
        return devs[0]
    print("!! adb 上有 %d 台设备（%s）—— 必须显式指定：--serial <设备> 或 PG_SERIAL=<设备>"
          % (len(devs), ", ".join(devs) or "无"))
    return None


class Dev:
    def __init__(self, serial):
        self.s = serial
        self.A = ["adb", "-s", serial]

    def sh(self, cmd, t=30):
        p = subprocess.run(self.A + ["shell", cmd], capture_output=True, text=True,
                           env=ENV, timeout=t, encoding="utf-8", errors="replace")
        return (p.stdout or "") + (p.stderr or "")

    def push(self, src, dst):
        subprocess.run(self.A + ["push", src, dst], capture_output=True, env=ENV)

    def dump(self):
        return self.sh("cat /tmp/pg_ime_dump.txt")

    def qa(self, body, wait=1.5):
        """把整份 QA 内容推上去（内容变化才执行），等一会儿，把 dump 读回来。"""
        os.makedirs(os.path.dirname(CMD_FILE), exist_ok=True)
        with open(CMD_FILE, "w", encoding="utf-8") as f:
            f.write(body.rstrip("\n") + "\n")
        self.push(CMD_FILE, "/tmp/pg_imecmd")
        time.sleep(wait)
        return self.dump()


def show_ime(d):
    """拉起输入法：确保在 HA 页 → 切命名页 → 点输入框（ZKEditText 聚焦时系统自动 showIME）。

    ⚠️ 三个坑（都实测踩过）：
      ① **必须先确认应用停在哪个页**。固化/重启后前台是**启动器（main.ftu）**，
         此时 HA 的 QA 通道 `/tmp/pg_hacmd` 与心跳 `/tmp/pg_ha_tick.txt` 都是**不存在**的
         （页面没跑，timer 就不会轮询）—— 直接发 `win name` 会石沉大海。
         入口是启动器自己的通道：`echo 'haapp' > /tmp/pg_autostart`。
      ② **屏保会抢前台**（screensaverTimeout=30s）。屏保中 HA 的 timer 停 ⇒ QA 也不执行。
         这里靠 `pg_ha_tick.txt` 存在 + 每次点击输入框前先点一下屏幕来规避。
      ③ 通道内容**必须每轮不同**才执行（整份去重）⇒ 用时间戳当序号，别写死 `#1`。
    """
    # 1) 打开 HA 页（启动器通道）
    d.sh("echo 'haapp #%d' > /tmp/pg_autostart" % int(time.time()))
    time.sleep(2.5)
    # 2) HA → 命名页
    d.sh("echo 'win name #%d' > /tmp/pg_hacmd" % int(time.time()))
    # ★ 必须**等页面真的切过去**再点 —— 固定 sleep 会踩这个坑：
    #   切页是异步的（QA 轮询 250ms 一拍 + 窗口动画），上次实测 sleep(1.2) 不够，
    #   那一 tap 落在 HA 主页上（只点了一下卡片），于是"输入框没聚焦 → IME 没起来"，
    #   而症状是"dump 读不到"，看起来像 IME 坏了。
    #   心跳文件 /tmp/pg_ha_tick.txt 里有 `win=N`，拿它当判据（**比 sleep 可靠**）。
    for _ in range(25):
        t = d.sh("cat /tmp/pg_ha_tick.txt")
        if ("win=%d" % HA_NAME_PAGE) in t:
            break
        time.sleep(0.4)
    else:
        print("!! 没能切到命名页（/tmp/pg_ha_tick.txt 始终没有 win=%d）—— "
              "当前内容：%r" % (HA_NAME_PAGE, t.strip()))
    time.sleep(0.5)
    # 3) 点输入框聚焦（焦点在 ZKEditText 上时框架自动 showIME）
    d.push(os.path.join(ROOT, "tools", "pginj"), "/tmp/pginj")
    d.sh("chmod 755 /tmp/pginj")
    # 触摸面板是 gt9xx（/dev/input/event0），但**节点名会变** ⇒ 每次现查，别写死
    dev_info = d.sh("cat /proc/bus/input/devices")
    m = re.search(r'Name="gt9xx".*?Handlers=(event\d+)', dev_info, re.S)
    node = "/dev/input/" + (m.group(1) if m else "event0")
    d.sh("/tmp/pginj tap %s 240 195 80" % node)   # 命名页输入框中心
    time.sleep(1.5)
    return node


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("pinyin", nargs="?", default="", help="要喂的拼音（不含声调）")
    ap.add_argument("--serial", default=os.environ.get("PG_SERIAL", ""))
    ap.add_argument("--shot", action="store_true", help="顺便抓屏到 .shots/ime_qa_<拼音>.png")
    ap.add_argument("--raw", default="", help="直接发这段多行 QA（替代 pinyin）")
    ap.add_argument("--no-ime", action="store_true", help="不拉起输入法（假设已在前台）")
    args = ap.parse_args()

    dev = pick_device(args.serial)
    if not dev:
        return 2
    d = Dev(dev)

    if args.raw:
        out = d.qa(args.raw)
        print(out.rstrip() or "!! dump 是空的 —— 输入法没在前台？")
        return 0 if out.strip() else 2

    if not args.pinyin:
        ap.print_help()
        return 2

    node = ""
    if not args.no_ime:
        node = show_ime(d)
        print("(触摸节点 %s；已点命名页输入框拉起输入法)" % node)

    # ★ 用 `mode cn` **显式设置**（2026-09-24 给 QA 通道加的，见 imeApp.cc 的 pollQa）。
    #   在此之前只有 toggle 版 `mode`：必须先 dump 读一次"现在是什么模式"才知道要不要翻，
    #   多一次往返，而且读到的模式和真正执行时可能已经不一样了。
    #   ⚠️ 若设备上跑的还是加这个命令之前的库，`mode cn` 会被当成 toggle ⇒ 模式可能翻反，
    #      此时 dump 里会看到 py 正常但 mode=en（字上不了屏）。先固化新库再跑本脚本。
    body = "mode cn\nclear\ntype %s\ndump\n#probe" % args.pinyin
    out = ""
    for attempt in (1, 2):
        out = d.qa(body) or ""
        if ("py=" in out) and ("mode=cn" in out):
            break
        # IME 有时没被第一下点起来（触摸注入与窗口焦点是两件事）⇒ 重试一次
        if attempt == 1 and not args.no_ime and node:
            d.sh("/tmp/pginj tap %s 240 195 80" % node)
            time.sleep(1.8)
    if "py=" not in out:
        print("!! 读不到 dump（两次都没起来）：%r" % out[:200])
        print("   检查：① 设备是否在 HA **命名页**（不是主页/启动器）；② 是否被屏保盖住；")
        print("        ③ `cat /tmp/pg_ha_tick.txt` 有没有 `win=%d`。" % HA_NAME_PAGE)
        return 2
    print("---- 真机 dump ----")
    print(out.rstrip())
    if "mode=cn" not in out:
        print("   ⚠️ 模式不是中文 —— 设备上的库可能还不认 `mode cn`（先固化新库）")

    if args.shot:
        shot = os.path.join(ROOT, ".shots", "ime_qa_%s.png" % args.pinyin)
        rc = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "grab.py"), shot,
                             "--serial", dev], capture_output=True, text=True, env=ENV)
        print("截图: %s" % (os.path.relpath(shot, ROOT) if rc.returncode == 0 else "(失败)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
