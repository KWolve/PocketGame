#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""画布文字宽度体检：把「画布文案会不会超出屏幕」变成一条可复现的静态检查。

为什么需要这个（血案）：算术泡泡 READY 页的说明行「点破答案正确的那颗泡泡」是 11 个
中文 × 档 3 的 48px 字格 = **528px > 屏宽 480px** ⇒ 左右各被裁掉 24px，用户报"文字
超出屏幕"。这类问题**编译/QA 功能断言都抓不到**（动作逻辑全对），只能靠量宽度。

字档宽度与 `src/core/PgFontData.h` 的 `ASCII_FONTS` / `CJK_FONTS` / `BIG_FONTS`
一一对应（改生成器时记得改这里）：

    text 系列   ASCII : 8  16 24 32  40      CJK : 16 32 48 64 80
    bigText 系列 BIG  : 16 32 48（只有 3 档，且 ASCII/数字共用 BIG 档）

规矩：**画布宽 480**，留边 ≥ MARGIN ⇒ 文案宽必须 ≤ 480 - 2*MARGIN。

用法：
    python tools/check_textwidth.py            # 全部画布源文件
    python tools/check_textwidth.py -v         # 连"贴着边"的也列出来
"""
import os
import re
import sys

W = 480            # 画布宽（c.width()）
MARGIN = 16        # 单侧安全留边
LIMIT = W - 2 * MARGIN

TEXT_W = {1: 8, 2: 16, 3: 24, 4: 32, 5: 40}      # ASCII_FONTS[n].cellW
CJK_W = {1: 16, 2: 32, 3: 48, 4: 64, 5: 80}      # CJK_FONTS[n].cellW
BIG_W = {1: 16, 2: 32, 3: 48}                    # BIG_FONTS[n].cellW

# 只扫这些目录（画布游戏的实现都在 src/core）
SRC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "core")

# 匹配 `c.textCenter(expr, expr, "字面量", 档位, ...)` —— 只认**字面量档位**，
# 变量档位（如 PgSpot 的 sc）单列出来提醒人工核算。
CALL = re.compile(
    r'\.(?P<fn>textCenterBox|textCenter|bigTextCenter|bigText|text)\s*\(\s*'
    r'(?P<args>(?:[^;()]|\([^()]*\))*?)\)\s*;'
)
STR = re.compile(r'^\s*"((?:[^"\\]|\\.)*)"\s*$')
# 文件内的 `const int sc = 2;` —— 调用点写 `sc` 时也能算出档位（否则全进 NOTE，噪声大）
CONST_INT = re.compile(r'\bconst\s+int\s+(\w+)\s*=\s*(\d+)\s*;')


def esc(s):
    return (s.replace("\\n", "\n").replace("\\t", "\t")
             .replace('\\"', '"').replace("\\\\", "\\"))


def split_args(s):
    """按逗号切参数（不处理嵌套逗号，够用：调用点都是简单实参）。"""
    out, depth, cur, in_str = [], 0, "", False
    i = 0
    while i < len(s):
        ch = s[i]
        if in_str:
            cur += ch
            if ch == "\\":
                if i + 1 < len(s):
                    cur += s[i + 1]
                    i += 2
                    continue
            elif ch == '"':
                in_str = False
        else:
            if ch == '"':
                in_str = True
                cur += ch
            elif ch in "([":
                depth += 1
                cur += ch
            elif ch in ")]":
                depth -= 1
                cur += ch
            elif ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def width_of(s, scale_is_big, n):
    if not s or n < 1:
        return 0, 0.0
    total = 0
    odd = 0
    for ch in s:
        o = ord(ch)
        if o == 0x0A:      # 换行：另起一行，不累宽
            continue
        if scale_is_big:
            total += BIG_W.get(n, 16)
        elif o < 0x80:
            total += TEXT_W.get(n, 8)
        else:
            total += CJK_W.get(n, 16)
            odd += 1
    return total, 0.0


def scan():
    rows = []
    for name in sorted(os.listdir(SRC_DIR)):
        if not name.startswith("Pg") or not name.endswith(".cpp"):
            continue
        path = os.path.join(SRC_DIR, name)
        src = open(path, encoding="utf-8", errors="replace").read()
        consts = dict(CONST_INT.findall(src))
        for ln, line in enumerate(src.splitlines(), 1):
            for m in CALL.finditer(line):
                args = split_args(m.group("args"))
                if len(args) < 3:
                    continue
                lit = STR.match(args[2])
                if not lit:
                    continue
                fn = m.group("fn")
                big = fn.startswith("big")
                raw = args[3] if len(args) > 3 else "?"
                raw = consts.get(raw, raw)   # 文件内常量（如 `const int sc = 2;`）就地展开
                if not raw.isdigit():
                    rows.append((name, ln, fn, esc(lit.group(1)), raw, None, True))
                    continue
                x = width_of(esc(lit.group(1)), big, int(raw))
                rows.append((name, ln, fn, esc(lit.group(1)), raw, x[0], False))
    return rows


def main():
    verbose = "-v" in sys.argv
    rows = scan()
    bad, tight, var = [], [], []
    for name, ln, fn, text, raw, w, is_var in rows:
        if is_var:
            var.append((name, ln, fn, text, raw))
        elif w > LIMIT:
            bad.append((name, ln, fn, text, raw, w))
        elif w > LIMIT - 32:
            tight.append((name, ln, fn, text, raw, w))

    print("画布文字宽度体检（画布宽 %d，安全上限 %d，%s）" % (W, LIMIT, "含贴边" if verbose else "只报超出"))
    print("=" * 78)
    if bad:
        print("\n[FAIL] 超出安全宽度（会被屏幕裁掉两侧）：")
        for name, ln, fn, text, raw, w in bad:
            print("  %s:%d  %s(..., %r, 档位 %s)  宽 %d > %d  溢出 %dpx"
                  % (name, ln, fn, text, raw, w, LIMIT, w - LIMIT))
    if tight and verbose:
        print("\n[WARN] 贴着边（留边 < 16px）：")
        for name, ln, fn, text, raw, w in tight:
            print("  %s:%d  %s(..., %r, 档位 %s)  宽 %d（剩余 %dpx）"
                  % (name, ln, fn, text, raw, w, W - w))
    if var:
        print("\n[NOTE] 档位是变量（脚本无法核算，需人工确认目标档位）：")
        for name, ln, fn, text, raw in var:
            print("  %s:%d  %s(..., %r, %s)" % (name, ln, fn, text, raw))

    print("\n结果：%d 处超宽 / %d 处贴边 / %d 处变量档位" % (len(bad), len(tight), len(var)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
