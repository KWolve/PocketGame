#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_font_coverage.py - 核对一个字库里"本工程要用到的字"覆盖了多少

为什么需要它（**这是本工程最容易静默失败的地方**）：
  项目 `font/*.ttf` 是**完全替换**系统字体的子集字库，**没有逐字回退** ——
  字库里缺哪个字，那个字在屏幕上就**整个消失**（不是方框！更难发现）。
  所以"换字库"这个动作必须先量覆盖，不能凭体积/来源判断。

用法:
  python tools/check_font_coverage.py <ttf> [--list-missing]

字符集来源与 tools/gen_font.py **完全一致**（ASCII 全量 + GB2312 一级 + 中文标点 +
全角 + ui/*.json 的全部文本 + src 下所有字符串字面量 + IPTV 频道表），
所以这个工具报的"缺字"就是屏幕上真会消失的字。
"""

import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASCII_PRINTABLE = "".join(chr(c) for c in range(0x20, 0x7F))
CJK_PUNCT = "，。、；：？！（）【】《》“”‘’—…·℃"


def common_chinese():
    out = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    out.extend(chr(c) for c in range(0x3000, 0x3040))
    out.extend(chr(c) for c in range(0xFF01, 0xFF5F))
    return out


def collect_strings(dirs):
    """扫 src/**/*.{cc,cpp,h} 里的字符串字面量（与 gen_font.py 同口径）。"""
    out = set()
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for d in dirs:
        for dirpath, _dirnames, filenames in os.walk(d):
            for fn in filenames:
                if not fn.endswith((".cc", ".cpp", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    txt = open(p, "r", encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for m in pat.finditer(txt):
                    out.add(m.group(1))
    return out


def collect_json_text(path, chars):
    try:
        d = json.load(open(path, "r", encoding="utf-8"))
    except Exception:  # noqa: BLE001
        return

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k in ("text", "caption") and isinstance(v, str):
                    chars.update(v)
                else:
                    walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)
    walk(d)


def collect_channels(path, chars):
    if not os.path.exists(path):
        return
    try:
        txt = open(path, "r", encoding="utf-8", errors="replace").read()
    except OSError:
        return
    chars.update(txt)


def needed_chars():
    chars = set(ASCII_PRINTABLE) | set(CJK_PUNCT) | set(common_chinese())
    ui_dir = os.path.join(ROOT, "ui")
    if os.path.isdir(ui_dir):
        for fn in sorted(os.listdir(ui_dir)):
            if fn.endswith(".json"):
                collect_json_text(os.path.join(ui_dir, fn), chars)
    src_dir = os.path.join(ROOT, "src")
    for s in collect_strings([src_dir]):
        chars.update(s)
    collect_channels(os.path.join(ROOT, "resources", "iptv", "channels.m3u"), chars)
    return chars


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ttf = sys.argv[1]
    want_missing = "--list-missing" in sys.argv
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("需要 fontTools")
        return 2
    if not os.path.exists(ttf):
        print("字体不存在: %s" % ttf)
        return 2

    f = TTFont(ttf, fontNumber=0)
    cmap = set()
    for t in f["cmap"].tables:
        cmap.update(t.cmap.keys())
    f.close()

    chars = needed_chars()
    cps = sorted({ord(c) for c in chars if len(c) == 1})
    missing = [c for c in cps if c not in cmap]

    print("字体: %s（%d 字节）" % (ttf, os.path.getsize(ttf)))
    print("字库字形数: %d" % len(cmap))
    print("工程需要码点: %d" % len(cps))
    print("缺失: %d 个  ⇒ 覆盖率 %.2f%%"
          % (len(missing), 100.0 * (len(cps) - len(missing)) / max(1, len(cps))))
    if missing:
        s = "".join(chr(c) for c in missing)
        print("\n缺字清单（这些字在屏幕上会**整个消失**）：")
        print("  %s" % s)
        cjk = [chr(c) for c in missing if 0x4E00 <= c <= 0x9FFF]
        print("\n  其中汉字 %d 个；非汉字 %d 个" % (len(cjk), len(missing) - len(cjk)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
