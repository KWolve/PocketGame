#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
games_smoke.py - 全游戏**真机冒烟回归**（逐款进入 → 抓屏 → 判"这一款真的画出来了"）

什么时候必须跑它：
  · **重跑过 `tools/gen_game_art.py`**（它一次生成全部 79 张游戏素材 + `PgGameArt.h`）——
    只要生成器或素材被打过补丁，就要确认"别的游戏没被顺手改坏"；
  · 改过 `PgGameArt.h` / `PgSprite` / 画布尺寸这类**共用件**；
  · 发版前最后一遍。

每一款做四件事：
  1. **先"长按暂停键"回到列表**（`pginj key /dev/input/event3 108 800`）——
     独立 ftu 页（react/probe/iptv/wifi/…）是独立 Activity，主界面的 QA 通道管不到它，
     不先退出来就会出现"命令进了、画面还是上一页"的假象（实测踩过，见 README v1.30.8）；
  2. `enter <分类内下标>`（等价于点卡片）；
  3. 抓屏（复用 `tools/grab.py`，按 pan 对齐半页），量画布区（屏幕 y=160..700）的
     "墨迹率" = 与**众数颜色**不同的像素占比；
  4. 读日志确认**真正进了哪一页**（`enter game[N]` / `enter tool[N]` / `独立页面 X -> Y`）
     并抓素材类报错关键字（`尺寸不符` / `载入失败` / `拼图失败`）。

判据：① 每轮都必须能读出"进了哪个页面"；② 画面必须**与上一轮不同**（说明真的切过去了）；
      ③ 墨迹率 > 1%（不是一整块纯色）。

用法:
    python tools/games_smoke.py             # 游戏分类全部 21 款
    python tools/games_smoke.py 12 13 20    # 只跑指定下标
截图落 `out/smoke/`（中转文件两侧都会清掉）。有异常则返回码非 0。
"""
import hashlib
import os
import re
import subprocess
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grab import grab                                    # noqa: E402

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow（本机用 Python314 解释器）")

CANVAS_TOP, CANVAS_BOT = 160, 700
# 每个分类的卡片数（与 PgGames.cpp 的 kAppTable 同源：游戏 21 / 工具 10 / 系统 1）
CAT_SIZES = {0: 21, 1: 10, 2: 2}   # 2026-09-16：系统分类加了"系统设置"（slot 32）
                                  #   ⇒ 应用总数 33（游戏 21 + 工具 10 + 系统 2）
OUT_DIR = "out/smoke"
KEY_NODE = "/dev/input/event3"       # gpio-keys（103/105/108 三键）
KEY_PAUSE = 108                      # 长按 = 各页通用的"返回列表"
ERR_PAT = re.compile(r"尺寸不符|载入失败|拼图失败|素材缺失", re.I)
ENTRY_PAT = re.compile(r"(enter game\[\d+\] \S+|enter tool\[\d+\] \S+|独立页面 \S+ -> \S+)")


def sh(args, timeout=60):
    """★ 按**字节**收、再 `errors="replace"` 解码：`adb logcat -d` 里会混进**非 UTF-8 字节**
    （实测：进 iptv 页那轮直接 `UnicodeDecodeError: 0xc5 in position 217476`，
    而 `text=True` 一抛异常返回值就成了 None ⇒ 后面 AttributeError 把整趟冒烟打断）。
    日志里有二进制内容这件事本身不奇怪（SSID/裸帧回显都可能），检具不该因此崩。"""
    b = subprocess.run(["adb"] + args, capture_output=True, timeout=timeout).stdout or b""
    return b.decode("utf-8", "replace")


def back_to_list():
    """长按暂停键 ≈800ms：所有独立页与游戏页都认（gpio-keys 无 BACK 键，只能这样退）。"""
    sh(["shell", "/data/pginj key %s %d 800" % (KEY_NODE, KEY_PAUSE)])
    time.sleep(0.6)


def ink_ratio(img):
    px = img.load()
    w, h = img.size
    cnt = Counter()
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            cnt[px[x, y]] += 1
    if not cnt:
        return 0.0, (0, 0, 0)
    top, tn = cnt.most_common(1)[0]
    return 1.0 - tn / float(sum(cnt.values())), top


def main():
    """默认扫**全部 32 个应用**（游戏 21 + 工具 10 + 系统 1）；
    给了参数就只扫那些下标（并把分类固定为 0，便于单点复现）。
    `--glyph`：顺便做**字库覆盖体检** —— 每页先 `glyphmiss reset`，进页面（画布游戏再点一下开局）
    后读 `glyphmiss` 的 miss/fallback。**字库是"按档位收字的子集"，缺字是静默的**（画方框或干脆消失），
    只有这个计数能抓到。改过 `tools/genfont.py` / 加过画布文案之后必须跑一遍。"""
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    glyph_mode = "--glyph" in sys.argv
    if argv:
        todo = [(0, int(a)) for a in argv]
    else:
        todo = [(cat, i) for cat, n in sorted(CAT_SIZES.items()) for i in range(n)]
    os.makedirs(OUT_DIR, exist_ok=True)

    print("%-5s %-4s %-26s %-8s %-9s %-9s %s"
          % ("分类", "下标", "进入的页面", "墨迹率", "画面变化", "缺字", "报错"))
    print("-" * 92)
    bad, prev_md5, cur_cat, glyph_bad = [], None, None, []
    for cat, i in todo:
        if cat != cur_cat:
            sh(["shell", "echo 'cat %d #smoke%d' > /tmp/pg_autostart" % (cat, cat)])
            time.sleep(1.0)
            cur_cat = cat
        back_to_list()
        tag = "sm%d%d" % (cat, i)
        if glyph_mode:
            sh(["shell", "echo 'glyphmiss reset #gr%d%d' > /tmp/pg_autostart" % (cat, i)])
            time.sleep(0.3)
        sh(["shell", "echo 'enter %d #%s' > /tmp/pg_autostart" % (i, tag)])
        time.sleep(2.0)
        jtext = "—"
        if glyph_mode:
            # 画布游戏再点一下开局（READY 遮罩之外的文案才会被画到 ⇒ 才会计数）
            sh(["shell", "/data/pginj tap /dev/input/event0 240 430"])
            time.sleep(1.2)
            sh(["shell", "echo 'glyphmiss #gn%d%d' > /tmp/pg_autostart" % (cat, i)])
            time.sleep(0.5)
            gm = re.search(r"glyphmiss miss=(\d+) fallback=(\d+)", sh(["logcat", "-d"]))
            if gm:
                m, f = int(gm.group(1)), int(gm.group(2))
                jtext = "%d/%d" % (m, f)
                if m or f:
                    glyph_bad.append((cat, i, m, f))
            else:
                jtext = "?"
        # ① 日志：真正进了哪一页 + 素材报错
        log = sh(["logcat", "-d"])
        entry, errs = "", []
        for line in log.splitlines():
            m = ENTRY_PAT.search(line)
            if m:
                entry = m.group(1)
            if ERR_PAT.search(line):
                errs.append(line.split("): ", 1)[-1].strip()[:44])
        # ② 抓屏 + 判据
        # ★ 抓屏要**重试**：实测偶发失败（一次 21 轮里连续 8 轮拿不到帧，重跑就过）——
        #   60fps 重绘期间 `cat /dev/fb0` 偶尔读短、pull 也随之失败。台架本身不可复现
        #   会让"通过/不通过"失去意义，所以宁可重试 3 次再判失败。
        png = os.path.join(OUT_DIR, "c%d_%02d.png" % (cat, i))
        ratio, top, changed = 0.0, (0, 0, 0), False
        for attempt in range(3):
            try:
                grab(png, keep_raw=True)      # ★ 不删 PC 侧中转文件（见 grab.grab 的说明）
                img = Image.open(png).convert("RGB")
                md5 = hashlib.md5(open(png, "rb").read()).hexdigest()
                changed = (prev_md5 is None) or (md5 != prev_md5)
                prev_md5 = md5
                # ★ 量墨迹的区域要分两类：**画布类游戏**量画布区（y=160..700）；
                #   **独立 ftu 页**量**全屏** —— 它们的原生控件在 y<160 那边，而空列表页
                #   中间那块本来就是纯色（实测摄像头页在"局域网里没有摄像头"时画布区墨迹 = 0%，
                #   看着像"没画出来"，其实标题栏与状态行都在全屏范围内）。
                box = (0, 0, 480, 800) if entry.startswith("独立页面") else (0, CANVAS_TOP, 480, CANVAS_BOT)
                ratio, top = ink_ratio(img.crop(box))
                break
            except Exception as e:                      # noqa: BLE001
                if attempt == 2:
                    errs.append("抓屏失败(%s): %s" % (type(e).__name__, e))
                else:
                    time.sleep(0.5)

        flag = ""
        if not entry:
            flag = " ← 没读到进入记录"
        elif not changed:
            flag = " ← 画面与上一轮相同（没真的切过去？）"
        elif ratio < 0.01:
            flag = " ← 画面几乎纯色"
        if flag or errs:
            bad.append((cat, i, entry, ratio, errs, flag))
        print("%-5d %-4d %-26s %6.1f%%  %-9s %-9s %s%s"
              % (cat, i, entry or "?", ratio * 100.0, "是" if changed else "否", jtext,
                 ";".join(errs[:1]), flag))
    print("-" * 92)
    if bad or glyph_bad:
        if bad:
            print("画面异常 %d 项：" % len(bad))
            for cat, i, e, r, err, f in bad:
                print("  分类%d #%d %s 墨迹率=%.1f%%%s%s" % (cat, i, e or "?", r * 100, f,
                                                         ("  报错=" + str(err)) if err else ""))
        if glyph_bad:
            print("**缺字** %d 项（跑 tools/genfont.py 补字后重验）：" % len(glyph_bad))
            for cat, i, m, f in glyph_bad:
                print("  分类%d #%d miss=%d fallback=%d" % (cat, i, m, f))
        return 1
    print("全部 %d 项：都能进入、画面都切过去了、日志无素材类报错%s ✓"
          % (len(todo), "、字库无缺字" if glyph_mode else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
