#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""resources/images 资源体检：**重复**与**无人引用**（孤儿）。

回答的问题：
  · 有没有内容完全相同的图？（改了配色/尺寸后旧图没清，或不同主题色撞车）
  · 有没有"烘出来了但没人引用"的图？（真能删的）
  · 各族的引用命中率如何？

为什么不能只扫文件名（★ 本次踩的坑）：
  `resources/images` 里有 **两套加载机制**，只做静态字符串匹配会把 792 个**动态引用**误判成孤儿：
    ① **JSON 静态路径**：`ui/*.json` 的 `picTab`/`backgroundPic` 字段直接写文件名（`.9.png` 九宫格）；
   ② **运行时拼名字**：`snprintf("images/ios_rt_%s_%dx%d%s.png", style, w, h, suf)`
      （`src/platform/PgSkin.cpp`）、`snprintf("images/batt_%s_%d.png", ...)`
      （`src/logic/navibar.cc` —— 电池整图：3 色 × 11 档，见 tools/ios_theme.py）。
  ⇒ 必须把 ② 的**格式串**转成正则当作"动态引用模式"，否则 `ios_rt_*`（540 张）与
  `batt_*`（33 张）全会被误报。
  ⚠️ 换过名字的族要注意：电池在 2026-09-16 从"外壳 + 电量条 + 闪电"三控件
     （`batt_shell/batt_fill_%s_%d/batt_bolt`，122 张）改成**一枚整图**
     （`batt_%s_%d.png`，33 张）。**改了拼名格式就要同步这个模型的来源**
     —— 旧格式串只留在注释里时，本脚本仍会把它当一条模板（匹配不到任何文件，无害但会误导）。

另一个坑：`re.escape()` 在 **Python 3.7+ 不转义 `%`** ⇒ 替换 `r'\\%s'` 永远匹配不到，
正则会带着裸 `%s` 去匹配文件名 ⇒ 同样得到"全都没被引用"的假结论。按裸 `%s` 替换才对。

用法：
    python tools/check_res_usage.py        # 汇总 + 孤儿清单
    python tools/check_res_usage.py -v     # 连重复组、仅在生成器里出现的也列出来
退出码：恒为 0（孤儿是**提醒**不是错误 —— 也可能是刚烘好还没接线的图）。
"""
import collections
import hashlib
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMGDIR = os.path.join(ROOT, "resources", "images")


def fam(name):
    """把文件名归到"族"：去掉尺寸/变体后缀，便于看"一族烘了多少张"。"""
    b = name[:-4]
    if b.endswith(".9"):
        b = b[:-2]
    b = re.sub(r'_\d+x\d+.*$', '', b)
    b = re.sub(r'_r\d+.*$', '', b)
    b = re.sub(r'_\d+$', '', b)
    return b


def conv(tmpl):
    """格式串 → 匹配文件名的正则（见文件头关于 re.escape 的坑）。"""
    t = re.escape(tmpl)
    t = (t.replace('%dx%d', r'\d+x\d+')
          .replace('%02d', r'\d+')
          .replace('%s', r'[A-Za-z0-9_]*')
          .replace('%d', r'\d+')
          .replace('%u', r'\d+')
          .replace('%x', r'[0-9a-f]+'))
    return re.compile('^' + t + '$')


def collect_refs(files):
    """返回 (字面量引用 -> 来源集合, [(正则, 来源, 模板)]) 。"""
    lit = collections.defaultdict(set)
    dyn = []

    def add(tmpl, src):
        base = tmpl.split("images/", 1)[1] if "images/" in tmpl else tmpl
        if "%" in base:
            dyn.append((conv(base), src, base))
        else:
            lit[base].add(src)

    def scan(path, label):
        try:
            b = open(path, "rb").read()
        except OSError:
            return
        for m in re.finditer(rb'images/[A-Za-z0-9_%\./\-]+\.png', b):
            add(m.group(0).decode(), label)

    # 运行时引用：src/ 源码 + ui/ 的 json/html/ftu（ftu 是二进制，但字符串在）
    for root, _, fs in os.walk(os.path.join(ROOT, "src")):
        for f in fs:
            if f.endswith((".cc", ".cpp", ".h")):
                scan(os.path.join(root, f), "src")
    for root, _, fs in os.walk(os.path.join(ROOT, "ui")):
        for f in fs:
            scan(os.path.join(root, f), "ui")
    # 生成器清单（单独标记：只有它引用的图 = 生成器自己列的，未必接线）
    for root, _, fs in os.walk(os.path.join(ROOT, "tools")):
        for f in fs:
            if f.endswith(".py"):
                scan(os.path.join(root, f), "tools")
    return lit, dyn


def main():
    verbose = "-v" in sys.argv
    files = sorted(f for f in os.listdir(IMGDIR) if f.lower().endswith(".png"))
    total_bytes = sum(os.path.getsize(os.path.join(IMGDIR, f)) for f in files)
    print("resources/images：%d 张 PNG，合计 %.2f MB" % (len(files), total_bytes / 1048576))

    # ---------- 1) 重复 ----------
    by_md5 = collections.defaultdict(list)
    for f in files:
        body = open(os.path.join(IMGDIR, f), "rb").read()
        by_md5[hashlib.md5(body).hexdigest()].append(f)
    dups = {k: v for k, v in by_md5.items() if len(v) > 1}
    saved = sum(os.path.getsize(os.path.join(IMGDIR, v[0])) * (len(v) - 1) for v in dups.values())
    print("\n[1] 内容完全相同（字节级 md5）：%d 组，涉及 %d 张，理论可省 %.1f KB"
          % (len(dups), sum(len(v) for v in dups.values()), saved / 1024))
    if verbose:
        for v in sorted(dups.values(), key=lambda x: -len(x)):
            print("     ", " == ".join(v))
    else:
        print("       （-v 看全部组；多为不同主题色撞车，见 README 的说明）")

    # ---------- 2) 引用 ----------
    lit, dyn = collect_refs(files)
    orphan, only_tools = [], []
    per = collections.defaultdict(lambda: [0, 0])
    for f in files:
        srcs = set(lit.get(f, ()))
        hit = bool(srcs) or any(r.match(f) for r, _, _ in dyn)
        if hit and srcs and srcs <= {"tools"}:
            only_tools.append(f)
        if not hit:
            orphan.append(f)
        k = fam(f)
        per[k][0] += 1
        per[k][1] += 1 if hit else 0
    print("\n[2] 引用模型：字面量 %d 个 + 动态模板 %d 条（%s）"
          % (len(lit), len(dyn), ", ".join(sorted({t for _, _, t in dyn}))))

    print("\n[3] 各族命中率（总数 / 命中）：")
    for k, (n, u) in sorted(per.items(), key=lambda x: -x[1][0]):
        if u != n or verbose:
            print("     %-22s %4d / %4d%s" % (k, n, u, "" if n == u else "   <== 未命中 %d" % (n - u)))

    print("\n[4] 无人引用（静态与动态都覆盖不到）：%d 张，合计 %.1f KB"
          % (len(orphan), sum(os.path.getsize(os.path.join(IMGDIR, f)) for f in orphan) / 1024))
    for f in orphan:
        print("     %-46s %6d B" % (f, os.path.getsize(os.path.join(IMGDIR, f))))
    if only_tools:
        print("\n[5] 只有生成器清单里提到（运行时未见引用）：%d 张" % len(only_tools))
        if verbose:
            for f in only_tools:
                print("     ", f)

    print("\n结论：重复 %d 组 / 孤儿 %d 张。**本脚本只诊断，不删文件** —— 删之前先确认"
          "（两张同内容的图可能被不同主题色按名字引用，删了要改代码做别名）。" % (len(dups), len(orphan)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
