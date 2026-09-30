#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_textblack.py - 抓出**会被 html2json 静默吃掉**的字符（文案黑名单）。

为什么需要它（2026-09-16 用户报"音量条右侧两个黑块"）：
    转换器 `ui_tools/html2json.py` 里有一张内置黑名单：

        _TEXT_BLACKLIST = set('⌫℃■●‹－＋–…→★◆▶▷①')
        def _clean_text(s):
            for ch in s:
                if ch in _TEXT_BLACKLIST or _is_emoji(ch):
                    continue        # ← 直接丢弃，**不报错**

    后果：源稿里写了 `＋`/`－` 的按钮，生成的 json 里 `text` 变成空串
    ⇒ 控件只剩一个底色块（用户看到的"黑块"），编译/静态检查/日志全都不报。
    这与"缺字形导致 5800 只显示 800"是同一类**静默失败**。

    ⚠️ 黑名单在 MCP 仓库里，工程侧不能改（约定：不代改 MCP）。
       所以本工具只做**拦截**：在生成前/后扫一遍，把会被吃掉的字符报出来，
       源稿要改用等价的可显示字符（如 ASCII `+` / `-`）。

用法:
    python tools/check_textblack.py           # 扫 ui/*.html 的文案
    python tools/check_textblack.py -v        # 连"上下文"一起打印
    python tools/check_textblack.py --json    # 扫 ui/*.json 的 text 字段（反查已生成物）
退出码: 0 = 干净；1 = 有会被吃掉的字符
"""
import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ⚠️ 必须与 D:/zkswe/flythings-mcp-open/ui_tools/html2json.py 的 _TEXT_BLACKLIST **保持同步**。
#    从那边源码里抄过来的（那边改了要跟着改；本工具会在启动时尝试自动读取真值）。
_FALLBACK_BLACKLIST = "⌫℃■●‹－＋–…→★◆▶▷①"

# 源稿里**允许**保留黑名单字符的地方：html 注释（<!-- -->）。
# 注释不进 text，写说明时用到这些符号是安全的。
COMMENT_RE = re.compile(r"<!--.*?-->", re.S)


def load_blacklist():
    """尽量从转换器源码读真值，读不到就用兜底常量。"""
    p = os.environ.get(
        "PG_HTML2JSON", r"D:\zkswe\flythings-mcp-open\ui_tools\html2json.py")
    try:
        s = open(p, encoding="utf-8").read()
        m = re.search(r"_TEXT_BLACKLIST\s*=\s*set\((['\"])(.*?)\1\)", s, re.S)
        if m:
            return set(m.group(2)), p
    except Exception:  # noqa: BLE001
        pass
    return set(_FALLBACK_BLACKLIST), None


def strip_comments(s):
    """把 html 注释换成等长空白，保持行号可读。"""
    return COMMENT_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), s)


def text_nodes(html):
    """粗取 html 里"会被当成文案"的片段：标签之间的文本 + 常见 data-* 文案属性。"""
    body = strip_comments(html)
    out = []
    for m in re.finditer(r">([^<>]+)<", body, re.S):
        out.append((m.start(), m.group(1)))
    for m in re.finditer(r'data-(?:text|title|hint|label|value)="([^"]*)"', body):
        out.append((m.start(), m.group(1)))
    return out


def line_of(s, pos):
    return s.count("\n", 0, pos) + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--json", action="store_true", help="改扫 ui/*.json 的 text 字段")
    args = ap.parse_args()

    bad, src = load_blacklist()
    print("黑名单（会被静默丢弃的字符）：%s" % "".join(sorted(bad)))
    print("  来源：%s" % (src or "内置兜底常量（未读到转换器源码）"))
    print()

    total = 0
    if args.json:
        for p in sorted(glob.glob(os.path.join(ROOT, "ui", "*.json"))):
            name = os.path.basename(p)
            if name.endswith(".preview.json"):
                continue
            try:
                data = json.load(open(p, encoding="utf-8"))
            except Exception:  # noqa: BLE001
                continue

            def walk(node, ctx=""):
                nonlocal total
                if isinstance(node, dict):
                    cap = node.get("caption", ctx)
                    t = node.get("text")
                    if isinstance(t, str):
                        hit = sorted({c for c in t if c in bad})
                        if hit:
                            total += 1
                            print("  ★ %s / %s : text=%r 命中 %s"
                                  % (name, cap, t, " ".join("U+%04X" % ord(c) for c in hit)))
                    for k, v in node.items():
                        if isinstance(v, (dict, list)):
                            walk(v, cap)
                elif isinstance(node, list):
                    for v in node:
                        walk(v, ctx)
            walk(data)
        if total == 0:
            print("  （json 的 text 字段里没有黑名单字符 ✓ —— 说明源稿已经干净）")
        return 0

    for p in sorted(glob.glob(os.path.join(ROOT, "ui", "*.html"))):
        name = os.path.basename(p)
        if name.endswith(".preview.html"):
            continue          # 预览稿不参与打包
        raw = open(p, encoding="utf-8").read()
        for pos, frag in text_nodes(raw):
            hit = sorted({c for c in frag if c in bad})
            if not hit:
                continue
            total += 1
            ln = line_of(raw, pos)
            ctx = re.sub(r"\s+", " ", frag).strip()
            print("  ★ %s:%d  命中 %s" % (name, ln, " ".join("U+%04X" % ord(c) for c in hit)))
            if args.verbose:
                print("        文案: %r" % ctx[:80])
            else:
                print("        文案: %r" % (ctx[:60] + ("…" if len(ctx) > 60 else "")))

    print()
    if total:
        print("结论：%d 处会被静默丢弃（界面上这些字符**直接消失**）—— 源稿请改用等价字符"
              "（`＋`→ ASCII `+`，`－`→ ASCII `-`，其余按字形可用性替换）" % total)
        return 1
    print("结论：源稿干净 ✓")
    return 0


if __name__ == "__main__":
    sys.exit(main())
