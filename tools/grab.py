#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""抓真机帧 → PNG（**按 pan 对齐半页**）。

为什么需要它：/dev/fb0 是 480x1600 双缓冲（每页 480x800），框架在 y=0 / y=800 两页之间翻。
固定读第 0 半页会拿到**上一页/上一帧** —— 症状是"两页截图字节级相同"或"日志说进了 A 页、
图上却是 B 页"，极易误判成"命令没生效"。所以每次都要读 pan 并按它选偏移。

用法:
    python tools/grab.py out.png            # 抓一帧存 PNG
    python tools/grab.py out.png --tries 4  # 自定义重试次数（默认 12）
    python tools/grab.py out.png --serial 20080411   # ★ 多设备时必须指定（或设 PG_SERIAL）
    python tools/grab.py out.png --keep-raw # 保留 3MB 中转文件（连抓几十帧的批量脚本用）

★★ 2026-09-23 修：以前内部调 `adb shell` **没带 `-s`** —— adb 上同时挂着两台设备时
   （本项目的常态：`20080411` + 现场的 `192.168.1.177:5555` Z20 面板），
   `adb shell` 一律失败，而症状只是**"adb pull 没落下文件"**（那个中转文件根本没生成），
   看起来像"设备盘满/没权限"，极难定位。
   ⇒ 现在：**自动挑唯一设备**；有两台以上就必须用 `--serial` 或环境变量 `PG_SERIAL`
     显式指定（否则**当场拒**并说明，不做"随便挑一台"这种静默行为）。

⚠️ **路径要用 Windows 风格**（`D:/Temp/x.png`）。传 MSYS 风格的 `/d/Temp/x.png` 时，
   Python 会把它当"**当前盘**的绝对路径"⇒ 落到 `E:/d/Temp/...` 这种地方，**而且会成功**
   （2026-09-19 踩到：先报 `FileNotFoundError: '/d/Temp\\_raw.bin'` 查半天；加了"自动建目录"
   之后反而变成**静默写到错地方** —— 比报错更坏）。⇒ 本工具现在**直接拒绝** MSYS 风格路径
   并给出应改成的写法（"静默失败必须消灭"，宁可当场拒）。

⚠️ **动画/屏保页的 pan 一直在翻**：本工具默认重试 12 次、**每次之间 sleep 0.15s**
   （2026-09-18 补：不加间隔时"连抓必失败"，因为两次读 pan 之间正好够翻一页；
     加上间隔后能稳定落在某一页上）。仍然抓不到时**必须看那行 `!! pan 一直在变` 警告** ——
   那一帧可能是撕裂的/错页的，别拿它当验收证据（本次就差点把一个错页当成"界面坏了"）。

偏移公式: offset = pan_y * stride(1920)   # stride = 480 * 4(BGRA)
"""
import os
import re
import subprocess
import sys
import time

DEV = "/tmp/pg_fb.bin"
STRIDE = 1920          # 480 * 4 字节
PAGE = 480 * 800 * 4

SERIAL = None          # None = 还没定；由 resolve_serial() 填


def resolve_serial(explicit=None):
    """定下要用哪台设备（**必须显式**，不能"随便挑"）。"""
    global SERIAL
    s = explicit or os.environ.get("PG_SERIAL") or os.environ.get("ANDROID_SERIAL")
    if s:
        SERIAL = s
        return s
    out = subprocess.run(["adb", "devices"], capture_output=True,
                         timeout=30).stdout.decode("utf-8", "replace")
    devs = [ln.split()[0] for ln in out.splitlines()[1:]
            if len(ln.split()) >= 2 and ln.split()[1] == "device"]
    if len(devs) == 1:
        SERIAL = devs[0]
        print("（自动选中唯一设备 %s）" % SERIAL)
        return SERIAL
    raise SystemExit(
        "adb 上有 %d 台设备（%s）—— 必须显式指定哪一台：\n"
        "   python tools/grab.py out.png --serial <设备>\n"
        "   或设环境变量 PG_SERIAL=<设备>（本项目：本板 20080411；现场 Z20 面板 192.168.1.177:5555）\n"
        "   ⚠️ 故意**不**做「随便挑一台」——那会把别人的板子当成自己的验收对象。"
        % (len(devs), ", ".join(devs)))


def windows_style_hint(path):
    """`/d/Temp/x.png` 这种 MSYS 路径在 Windows 上不是盘符路径 ⇒ 返回修正建议，否则 None。"""
    m = re.match(r"^/([a-zA-Z])/(.*)$", path)
    return "%s:/%s" % (m.group(1).upper(), m.group(2)) if m else None


def reject_msys_path(path):
    """★ MSYS 风格路径**直接拒绝**（不能只靠"自动建目录"兜底）。

    为什么必须拒绝：`/d/Temp/x.png` 在 Windows 上被解释成"当前盘（E:）的 \\d\\Temp\\x.png"，
    而"自动建目录"会让它**真的写成功** —— 于是图存到了错地方、验收时找不到，
    比直接报 FileNotFoundError 更坏。本项目的纪律是**消灭静默失败**，所以这里宁可当场拒。"""
    hint = windows_style_hint(path)
    if hint:
        raise SystemExit("路径 %s 不能用：这是 MSYS/Cygwin 风格，Windows 上会被写成"
                         "「当前盘:%s」。\n   ⇒ 改用：%s" % (path, path, hint))


def sh(cmd, timeout=30):
    # ★ 必须带 -s：多设备时 `adb shell` 会失败（见文件头的修订说明）
    base = ["adb"] + (["-s", SERIAL] if SERIAL else []) + ["shell", cmd]
    return subprocess.run(base, capture_output=True,
                          timeout=timeout).stdout.decode("utf-8", "replace").strip()


def pan_y():
    txt = sh("cat /sys/class/graphics/fb0/pan")
    # 形如 "0,800" / "0,0"
    try:
        return int(txt.split(",")[1])
    except Exception:
        return 0


def grab(out_path, tries=12, keep_raw=False, gap=0.15):
    """抓一帧。**两侧的中转文件都会清掉**（见 finally 里的注释）。

    keep_raw=True：PC 侧的 `_raw.bin` **保留**（下一轮直接覆盖）。给"连抓几十帧"的批量脚本
    用 —— 每轮都删会触发宿主环境的"批量删除"保护（实测：21 轮冒烟跑到一半被拦，命令直接失败）。
    设备侧那份仍然每轮清掉（那是 tmpfs 上的 3MB，不能留）。

    gap：每次重试之间的间隔（秒）。**必须有间隔** —— 动画页 60fps 翻页时，
    "读 pan → dump 3MB → 再读 pan" 之间几乎必然跨过一次翻页，不加间隔会一直判"pan 在变"。"""
    reject_msys_path(out_path)
    raw_path = os.path.join(os.path.dirname(out_path) or ".", "_raw.bin")
    # ★ 目标目录不存在就建（合法的"目录还没建"场景）；失败时给出可读的原因。
    d = os.path.dirname(raw_path)
    if d and not os.path.isdir(d):
        try:
            os.makedirs(d)
            print("（已创建输出目录 %s）" % d)
        except OSError as e:
            raise SystemExit("输出目录建不出来：%s\n   ⇒ 检查路径 %s 与磁盘剩余空间" % (e, out_path))
    off = None
    try:
        for i in range(tries):
            p1 = pan_y()
            sh("cat /dev/fb0 > " + DEV)
            p2 = pan_y()
            if p1 == p2:                      # 抓的过程中没翻页 → 有效
                off = p2
                break
            time.sleep(gap)
        if off is None:
            off = p2
            print("!! pan 一直在变（用了最后一次 pan=%d）—— 这一帧可能撕裂/错页，别当验收证据" % off)
        subprocess.run(["adb"] + (["-s", SERIAL] if SERIAL else []) + ["pull", DEV, raw_path],
                       capture_output=True)
        if not os.path.exists(raw_path):
            raise SystemExit("adb pull 没落下文件：%s\n   ⇒ 检查设备里 %s 是否生成（设备上执行 "
                             "`cat /dev/fb0 > %s` 看能不能写）、PC 磁盘是否有空间"
                             % (raw_path, DEV, DEV))
        raw = open(raw_path, "rb").read()
        if len(raw) < PAGE * 2:
            raise SystemExit("帧缓冲太小: %d 字节（期望 >= %d）—— **PC 侧磁盘是不是满了？**"
                             "（3MB 中转文件写不进去时表现就是这个）" % (len(raw), PAGE * 2))
        from PIL import Image
        im = Image.frombytes("RGBA", (480, 800), raw[off * STRIDE: off * STRIDE + PAGE],
                             "raw", "BGRA").convert("RGB")
        im.save(out_path)
        print("%s  pan_y=%d  %s" % (out_path, off, im.size))
        return off
    finally:
        # ★ 中转文件必须两边都删（2026-09-15 实测踩到）：
        #   ① 设备 /tmp 是 **tmpfs**（本板 56MB 内存）——3MB 的 /tmp/pg_fb.bin 留一次就吃 3MB，
        #      抓几张图就能把 Shmem 顶起来（实测 1.2MB → 4.2MB，帧率跟着掉）；
        #   ② PC 侧那份 _raw.bin 同样 3MB，会一直堆在 docs//out/ 里（除非 keep_raw）。
        sh("rm -f " + DEV)
        if not keep_raw:
            try:
                os.remove(raw_path)
            except OSError:
                pass


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    tries = 12                       # 与文档串一致（原先是 6，文档写 12 —— 已对齐）
    if "--tries" in sys.argv:
        tries = int(sys.argv[sys.argv.index("--tries") + 1])
    explicit = None
    if "--serial" in sys.argv:
        explicit = sys.argv[sys.argv.index("--serial") + 1]
    resolve_serial(explicit)
    grab(out, tries, keep_raw=("--keep-raw" in sys.argv))


if __name__ == "__main__":
    main()
