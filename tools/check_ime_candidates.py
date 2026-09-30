#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
check_ime_candidates.py - 核对输入法候选表(imePinyinData.h)与**当前**字库是否一致

为什么需要它（本工程最容易静默失败的地方）：
  imePinyinData.h 是"某一时刻的 font/pocketgame.ttf + 工程文案"的产物。
  而 font/pocketgame.ttf 会随 ui/*.html 改动被 tools/gen_font.py 重新生成
  ⇒ 只要**字库在生成候选表之后被重跑过**，候选表里就可能残留"字库里已经没有
     字形"的字 —— 那个字在屏幕上**整个消失**（不是方框），
     用户看到的现象就是"输入法能打出来的字变少了 / 候选里一片空白"。

判据：
  · 词组表/单字表里的**每一个汉字**都必须能在当前 ttf 的 cmap 里找到；
  · 报出受影响的拼音，明确指出"必须重跑 tools/gen_ime_pinyin.py"。

用法:
  python tools/check_ime_candidates.py                     # 默认 font/pocketgame.ttf
  python tools/check_ime_candidates.py <ttf>
  python tools/check_ime_candidates.py --example keting    # 打印某拼音的候选展开
"""
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
H_PATH = os.path.join(ROOT, "src", "logic", "imePinyinData.h")
ENT_RE = re.compile(r'\{"([a-z]+)", "([^"]*)"\}')


def load_cmap(ttf_path):
    from fontTools.ttLib import TTFont
    f = TTFont(ttf_path)
    return set(f.getBestCmap().keys())


def load_tables():
    src = open(H_PATH, encoding="utf-8").read()
    mw = re.search(r"kImePyWords\[\] = \{(.*?)\n\};", src, re.S)
    mc = re.search(r"kImePyChars\[\] = \{(.*?)\n\};", src, re.S)
    return (ENT_RE.findall(mw.group(1)) if mw else [],
            ENT_RE.findall(mc.group(1)) if mc else [])


def candidates(value):
    """一个条目的值 -> 候选列表（词组按空格分；单字串按字符分）"""
    if " " in value:
        return [w for w in value.split() if w.strip()]
    return list(value)


def main():
    args = [a for a in sys.argv[1:]]
    ttf = os.path.join(ROOT, "font", "pocketgame.ttf")
    if args and not args[0].startswith("--"):
        ttf = args[0]

    cmap = load_cmap(ttf)
    words, chars = load_tables()
    print("字库: %s（%d 个码点）" % (os.path.relpath(ttf, ROOT), len(cmap)))
    print("候选表: %s" % os.path.relpath(H_PATH, ROOT))
    print("  词组条目 %d 条；单字条目 %d 条" % (len(words), len(chars)))

    def scan(ents, label):
        bad = {}          # py -> [缺失字符]
        total = 0
        for py, val in ents:
            for cand in candidates(val):
                for ch in cand:
                    total += 1
                    if ord(ch) not in cmap:
                        bad.setdefault(py, []).append(ch)
        n_miss = sum(len(v) for v in bad.values())
        print("  %s: 汉字出现次数 %d，缺字形 %d 处（%.2f%%）"
              % (label, total, n_miss, 100.0 * n_miss / max(1, total)))
        return bad

    if "--example" in sys.argv:
        key = sys.argv[sys.argv.index("--example") + 1]
        for label, ents in (("词组", words), ("单字", chars)):
            hit = [v for k, v in ents if k == key]
            print("  [%s] %s -> %s" % (label, key, " | ".join(hit) if hit else "(无)"))

    bad = {}
    for label, ents in (("词组表", words), ("单字表", chars)):
        b = scan(ents, label)
        for k, v in b.items():
            bad.setdefault(k, []).extend(v)

    if not bad:
        print("\nOK：候选表里的每个字当前字库都有字形，不需要重跑生成器。")
        return 0

    print("\n!! 有 %d 个拼音的候选里含**当前字库没有字形**的字 —— "
          "用户选上去后那个字会在屏幕上整个消失：" % len(bad))
    for k in sorted(bad):
        print("   %-12s %s" % (k, "".join(dict.fromkeys(bad[k]))))
    print("\n修法：重跑 `python tools/gen_ime_pinyin.py`（它会按当前 cmap 重新过滤），"
          "\n      再重新构建、固化。原因是字库在候选表生成之后被重跑过。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
