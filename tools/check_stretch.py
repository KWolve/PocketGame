#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_stretch.py - 检查"图尺寸 != 控件尺寸"的控件（= 框架会缩放 = 拉伸 = 发糊/出锯齿）。

用户要求（2026-09-15）：**所有东西都不能做拉伸**。这个脚本是那条要求的执行者 ——
由 tools/gen_ui.py 在生成后自动跑，有问题会打印表格。

规则：
  · 非九宫格的 backgroundPic / picTab 图片，尺寸必须 **严格等于** 控件尺寸；
  · `.9.png`（九宫格）是**故意**可拉伸的（marker 边），跳过；
  · 图找不到也报（路径写错是最常见的低级错）。

⚠️ 只覆盖 JSON 里的**静态**引用。**运行时改尺寸**的控件不在覆盖范围：
   电量条 BattFill 的图由 tools/ios_theme.py 按宽度各烘一张
   （`batt_fill_<色>_<宽>.png`，3 色 × 40 宽），自己保证 1:1。
"""
import glob
import json
import os

from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
IMG_DIR = os.path.join(ROOT, "resources", "images")
_cache = {}


def img_size(name):
    if name in _cache:
        return _cache[name]
    p = os.path.join(IMG_DIR, name)
    if not os.path.isfile(p):
        _cache[name] = None
        return None
    with Image.open(p) as im:
        _cache[name] = im.size
    return _cache[name]


def walk(node, cb):
    if isinstance(node, dict):
        cb(node)
        for v in node.values():
            walk(v, cb)
    elif isinstance(node, list):
        for v in node:
            walk(v, cb)


def main():
    bad = []
    for f in sorted(glob.glob(os.path.join(ROOT, "ui", "*.json"))):
        if '.preview' in f:
            continue
        data = json.load(open(f, encoding='utf-8'))

        def check(node, _f=f):
            cap = node.get('caption')
            if not isinstance(cap, str):
                return
            pos = node.get('position') or {}
            w, h = pos.get('width'), pos.get('height')
            refs = []
            bg = node.get('backgroundPic')
            if isinstance(bg, str):
                refs.append(('backgroundPic', bg))
            pt = node.get('picTab') or {}
            for k in ('pic0', 'pic1'):
                if isinstance(pt.get(k), str):
                    refs.append(('picTab.' + k, pt[k]))
            for kind, ref in refs:
                name = ref.split('/')[-1]
                if not name or name.endswith('.9.png'):
                    continue
                sz = img_size(name)
                if not sz:
                    bad.append((os.path.basename(_f), cap, kind, name, '图不存在', ''))
                    continue
                if (sz[0], sz[1]) != (w, h):
                    bad.append((os.path.basename(_f), cap, kind, name,
                                '%dx%d' % sz, '%sx%s' % (w, h)))

        walk(data, check)

    if bad:
        print('  [拉伸检查] 发现 %d 处"图尺寸 != 控件尺寸"（框架会缩放 ⇒ 拉伸）：' % len(bad))
        print('  %-18s %-14s %-14s %-34s %-12s %s'
              % ('页面', '控件', '来源', '图片', '图尺寸', '控件尺寸'))
        for row in bad:
            print('  %-18s %-14s %-14s %-34s %-12s %s' % row)
    else:
        print('  [拉伸检查] 通过：%d 张非九宫格背景图全部与控件 1:1' % len(_cache))
    return 1 if bad else 0


if __name__ == '__main__':
    raise SystemExit(main())
