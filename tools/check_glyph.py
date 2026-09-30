#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_glyph.py - 检查项目字库（font/pocketgame.ttf）里**是否真有某个字符的字形**。

为什么需要它（2026-09-16 用户报"音量条右侧两个黑块"）：
    设置页的 ＋/－ 按钮写的是**全角** U+FF0B / U+FF0D。字库是从系统字体裁的子集
    （见 tools/gen_font.py），裁的时候如果字符集来源没覆盖到这两个码点，
    它们就**静默不画** —— 控件只剩一个底色块（就是用户看到的"黑块"），
    日志、编译、静态检查全都不会报错。这与"缺 ASCII 数字导致 5800 只显示 800"
    是同一个坑（见 tools/gen_font.py 文件头）。

用法:
    python tools/check_glyph.py                      # 检查内置的"易缺字符"清单
    python tools/check_glyph.py --text "＋－×÷"      # 检查指定字符串
    python tools/check_glyph.py --scan-json           # 扫描 ui/*.json 里所有 text 字段
    python tools/check_glyph.py --scan-src            # 扫描 src/ 里的字符串字面量
退出码: 0 = 全部有字形；1 = 有缺失
"""
import argparse
import glob
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "font", "pocketgame.ttf")

# 工程里"最容易缺、缺了又最难发现"的一批字符（全角标点 + 数学符 + 常用符号）
SUSPECT = "＋－×÷…—·°％（）「」《》、，。：；！？～　←→↑↓√≈≠≤≥○●□■★☆♦♥"


def load_cmap(path):
    """读 TTF 的 cmap，返回 {码点: 字形名}。手写解析（不依赖 fontTools）。"""
    with open(path, "rb") as fh:
        data = fh.read()
    tag, num_tables = struct.unpack(">4sH", data[0:6])
    if tag not in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"ttcf"):
        raise ValueError("不是 TTF/OTF：%r" % tag)
    tables = {}
    for i in range(num_tables):
        off = 12 + i * 16
        name, _checksum, offset, length = struct.unpack(">4sIII", data[off:off + 16])
        tables[name] = (offset, length)
    if b"cmap" not in tables:
        raise ValueError("没有 cmap 表")
    cm_off = tables[b"cmap"][0]
    _ver, n_sub = struct.unpack(">HH", data[cm_off:cm_off + 4])
    best = None
    for i in range(n_sub):
        rec = cm_off + 4 + i * 8
        plat, enc, sub_off = struct.unpack(">HHI", data[rec:rec + 8])
        sub = cm_off + sub_off
        fmt = struct.unpack(">H", data[sub:sub + 2])[0]
        # 优先级：3/10(格式12) > 3/1(格式4) > 0/x
        rank = {(3, 10): 3, (3, 1): 2, (0, 4): 1, (0, 3): 1}.get((plat, enc), 0)
        if rank and (best is None or rank > best[0]):
            best = (rank, fmt, sub)
    if best is None:
        raise ValueError("找不到可用的 cmap 子表")
    _rank, fmt, sub = best
    out = {}
    if fmt == 4:
        seg_x2 = struct.unpack(">H", data[sub + 6:sub + 8])[0]
        seg = seg_x2 // 2
        ends = struct.unpack(">%dH" % seg, data[sub + 14:sub + 14 + seg_x2])
        starts = struct.unpack(">%dH" % seg, data[sub + 16 + seg_x2:sub + 16 + seg_x2 * 2])
        deltas = struct.unpack(">%dh" % seg, data[sub + 16 + seg_x2 * 2:sub + 16 + seg_x2 * 3])
        rng_off_base = sub + 16 + seg_x2 * 3
        ranges = struct.unpack(">%dH" % seg, data[rng_off_base:rng_off_base + seg_x2])
        for i in range(seg):
            for cp in range(starts[i], min(ends[i], 0xFFFF) + 1):
                if ranges[i] == 0:
                    gid = (cp + deltas[i]) & 0xFFFF
                else:
                    gi = rng_off_base + i * 2 + ranges[i] + (cp - starts[i]) * 2
                    if gi + 2 > len(data):
                        continue
                    gid = struct.unpack(">H", data[gi:gi + 2])[0]
                    if gid:
                        gid = (gid + deltas[i]) & 0xFFFF
                if gid:
                    out[cp] = gid
    elif fmt == 12:
        _f, _r, _l, n_groups = struct.unpack(">HHII", data[sub:sub + 12])
        for i in range(n_groups):
            g = sub + 16 + i * 12
            s, e, gid = struct.unpack(">III", data[g:g + 12])
            for cp in range(s, min(e, 0x10FFFF) + 1):
                out[cp] = gid + (cp - s)
    else:
        raise ValueError("暂不支持的 cmap 格式 %d" % fmt)
    return out


def collect_from_json():
    """ui/*.json 里所有 text 字段（JSON 是 gen_ui.py 从 html 生成的，含全部文案）"""
    chars = set()
    for p in sorted(glob.glob(os.path.join(ROOT, "ui", "*.json"))):
        if os.path.basename(p).endswith(".preview.json"):
            continue
        try:
            raw = open(p, encoding="utf-8").read()
        except Exception:  # noqa: BLE001
            continue

        def walk(node):
            if isinstance(node, dict):
                for k, v in node.items():
                    if k == "text" and isinstance(v, str):
                        chars.update(v)
                    else:
                        walk(v)
            elif isinstance(node, list):
                for v in node:
                    walk(v)
        try:
            walk(json.loads(raw))
        except Exception:  # noqa: BLE001
            pass
    return chars


def collect_from_src():
    """src/ 下的字符串字面量（游戏标题/提示会 setText 到控件）"""
    chars = set()
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for ext in ("cc", "cpp", "h"):
        for p in glob.glob(os.path.join(ROOT, "src", "**", "*." + ext), recursive=True):
            try:
                s = open(p, encoding="utf-8", errors="ignore").read()
            except Exception:  # noqa: BLE001
                continue
            for m in pat.finditer(s):
                # 跳过带格式符/转义的（%s、\n 这类不是文案）
                body = m.group(1)
                if "\\" in body or "%" in body:
                    continue
                if any("\u4e00" <= c <= "\u9fff" or ord(c) > 0x2000 for c in body):
                    chars.update(body)
    return chars


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default=None, help="要检查的字符串")
    ap.add_argument("--scan-json", action="store_true")
    ap.add_argument("--scan-src", action="store_true")
    ap.add_argument("--font", default=FONT)
    args = ap.parse_args()

    if not os.path.exists(args.font):
        print("!! 找不到字库 %s" % args.font)
        return 1
    cmap = load_cmap(args.font)
    print("字库 %s：%d 个字形" % (os.path.relpath(args.font, ROOT), len(cmap)))

    if args.text is not None:
        charsets = [("命令行 --text", set(args.text))]
    elif args.scan_json or args.scan_src:
        charsets = []
        if args.scan_json:
            charsets.append(("ui/*.json 的 text 字段", collect_from_json()))
        if args.scan_src:
            charsets.append(("src/ 字符串字面量", collect_from_src()))
    else:
        charsets = [("内置易缺字符清单", set(SUSPECT))]

    bad_total = 0
    for name, chars in charsets:
        chars = {c for c in chars if c.strip() and ord(c) > 0x1F}
        miss = sorted(c for c in chars if ord(c) not in cmap)
        print("  [%s] 检查 %d 个字符 → 缺 %d 个" % (name, len(chars), len(miss)))
        if miss:
            bad_total += len(miss)
            for c in miss:
                print("      ★ 缺 U+%04X %r" % (ord(c), c))
    if bad_total:
        print("结论：有缺字（在界面上会**静默不画**，控件只剩底色块）")
        return 1
    print("结论：全部有字形 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
