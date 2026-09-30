#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
genfont.py - 生成游戏画布用点阵字体 (src/core/PgFontData.h)

★ 2026-09-15 第二版：**原生多档 + 灰度覆盖率**（不再做整数放大）

旧版的做法：只烘 8x12 / 16x24 / 16x16 三套 1bit 点阵，调用点传 `scale=2..5`，
绘制时把每个原始像素 **放大成 scale×scale 的实心方块**。后果：
  · 中文 16x16 放大 2 倍 → 32x32 里全是 2x2 色块，**笔画边缘是台阶**
    （用户报"整体太粗糙 / 有锯齿"）
  · 放大 3/4/5 倍更糟；1bit 没有抗锯齿，斜边只能是硬边。
  · 直接违反"不能做拉伸"：**放大 = 拉伸**。

新版：
  1. **每一档都按目标像素数原生栅格化**（`N×` 字号直接渲染），绘制时 1:1 贴像素 ——
     不放大、不拉伸。档位 N 仍等于调用点里原来的 `scale` 实参（1..5），
     **调用点一行都不用改**。
  2. **灰度覆盖率**（抗锯齿）：字形按 0..255 覆盖率烘进去（存 4bit/像素，16 级），
     绘制时 `blendPx` 按覆盖率混合 → 边缘平滑。
  3. **字格尺寸严格 = 基准 × N**（8x12→×N、16x16→×N、16x24→×N），
     所以 textW/textH/lineH 的排版结果与旧版一致（零布局漂移）。
  4. **按档位收字**（体积控制）：扫描调用点得出"每个档位真正要画哪些字"；
     字面量直接取字符，**非字面量**（变量/格式化串）回退到"该文件所有字面量"
     这个保守超集。ASCII 在 N<=2 直接给全量（95 个，成本可忽略）。

用法:
  python tools/genfont.py            # 生成 src/core/PgFontData.h + 打印自检
  python tools/genfont.py --check    # 只自检（不写文件）
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT = os.path.join(ROOT, "src", "core", "PgFontData.h")
SRC_DIRS = [os.path.join(ROOT, "src", "core"), os.path.join(ROOT, "src", "logic")]

MAX_N = 5                    # 文本档位上限（text/textCenter 的 scale 实参）
BIG_MAX_N = 3                # 大号数字档位上限：scale 来自 fitBigText(maxScale<=3)；
                             # 再大就是"放大 = 拉伸"，代码里已按 3 封顶
COV_BITS = 4                 # 覆盖率位宽（4 = 16 级；改 8 则体积翻倍、更细腻）

# ---- 三套字形的"基准"（N=1 的参数；N 档 = 全部乘 N）----
CONSOLA = r"C:\Windows\Fonts\consola.ttf"
SIMHEI = r"C:\Windows\Fonts\simhei.ttf"

ASCII = dict(name="ASCII", font=CONSOLA, size=13, cell=(8, 12), oy=-2, center_x=True,
             first=32, last=126)
BIG = dict(name="BIG", font=CONSOLA, size=26, cell=(16, 24), oy=0, center_x=True,
           chars=" 0123456789.:-+")
CJK = dict(name="CJK", font=SIMHEI, size=16, cell=(16, 16), oy=-1, center_x=False,
           chars=None)   # 由扫描决定

# 所有档位都带的 ASCII（数字/符号：分数、时间、比例…运行时拼出来的）
ALWAYS_ASCII = " 0123456789.:-+/%xX()"

# 兜底字符（扫描不到但可能在画布上出现）—— 缺字会画成空心方框，这条只做保险
EXTRA_CJK = "继续重来退出暂停返回确定取消胜利失败得分最高命中关数时间剩余完成恭喜加油"

TEXT_FUNCS = ("text", "textCenter", "textCenterBox",
              "bigText", "bigTextCenter", "bigTextCenterBox")
CALL_RE = re.compile(r'\b(' + "|".join(TEXT_FUNCS) + r')\s*\(')
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def strip_comments(src):
    """剥掉 C/C++ 注释（**字符串感知**：只在字符串字面量之外认 `//` 与 `/* */`）。

    为什么必须做（2026-09-16 实测）：`STR_RE` 不认注释，于是**注释里用英文双引号包起来的中文
    短语**会被当成"字符串字面量"收进画布字库 —— 全 `src/core/` 统计到 **364 条**这样的短语，
    让 CJK@2 涨到 584 字（292KB），而**注释永远不会画到屏上**，纯属白烧 flash/RAM。
    ⚠️ 必须字符串感知：URL（`"http://…"`）、含 `/*` 的路径都可能出现在真实字面量里，
    粗暴 `re.sub` 会把真文案截断 ⇒ 那才会造成"静默漏字"。
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        ch = src[i]
        if ch in ('"', "'"):                       # 字符串 / 字符字面量：整段照抄
            q = ch
            out.append(ch)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == '\\' and i + 1 < n:   # 转义
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if ch == '/' and i + 1 < n and src[i + 1] == '/':      # 行注释
            j = src.find('\n', i)
            i = n if j < 0 else j          # 连换行一起丢（字面量不会跨注释）
            continue
        if ch == '/' and i + 1 < n and src[i + 1] == '*':      # 块注释
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append('\n')               # 保一个换行，别把两行拼成一行
            continue
        out.append(ch)
        i += 1
    return ''.join(out)


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            if ch in '([':
                depth += 1
            elif ch in ')]':
                depth -= 1
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# 扫描时要排除的文件：PgCanvas.cpp 是**实现**（内部 textCenter→text 的转发调用
# 不是"需求"，统计进来会把该文件的全部字面量算成画布文案）。
SKIP_FILES = {"PgCanvas.cpp", "PgFontData.h"}


def source_files():
    out = []
    for d in SRC_DIRS:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith((".c", ".cc", ".cpp", ".h")) and fn not in SKIP_FILES:
                out.append(os.path.join(d, fn))
    return out


_GAME_LITERALS = None


def game_literals():
    """所有游戏实现（src/core/*.cpp）短字面量的并集。

    用在哪：**非字面量实参**的取值来源。画布文案的变量无非两类 ——
      · 本文件 snprintf 出来的（格式串就在本文件）→ 用本文件的短字面量就够；
      · 由宿主代画、值来自别处（例如 mainLogic 画软按钮，标签来自 gGame->softButtons()）
        → 那时用"全部游戏短字面量"这个并集兜住。
    比"文件全部字面量"精确得多（mainLogic 里 345 个中文日志字都会画不到画布上）。
    """
    global _GAME_LITERALS
    if _GAME_LITERALS is None:
        chars = set()
        core = SRC_DIRS[0]
        for fn in sorted(os.listdir(core)):
            if fn.endswith((".c", ".cc", ".cpp")) and fn not in SKIP_FILES:
                chars |= file_literals(os.path.join(core, fn))
        _GAME_LITERALS = chars
    return _GAME_LITERALS


def unescape(s):
    """把 C 字符串字面量里的转义（\\n / \\xNN / \\" 等）剥成"内容里真实出现的字符"。"""
    out = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == 'n':
                out.append('\n')
                i += 2
                continue
            if nxt == 't':
                out.append('\t')
                i += 2
                continue
            if nxt in '\\"\'?':
                out.append(nxt)
                i += 2
                continue
            if nxt == 'x':
                j = i + 2
                while j < len(s) and s[j] in '0123456789abcdefABCDEF':
                    j += 1
                try:
                    out.append(chr(int(s[i + 2:j], 16)))
                except ValueError:
                    pass
                i = j
                continue
            if nxt in '01234567':
                j = i + 1
                while j < len(s) and s[j] in '01234567' and j < i + 4:
                    j += 1
                try:
                    out.append(chr(int(s[i + 1:j], 8)))
                except ValueError:
                    pass
                i = j
                continue
            out.append(nxt)
            i += 2
            continue
        out.append(s[i])
        i += 1
    return ''.join(out)


def file_literals(path, max_len=14):
    # ⚠️ 阈值按档位不同（见调用点）：大字档只画标题/胜负语，用更严的阈值，
    #    能把中文@48/64 的字集砍掉一半（体积 = 字数 × 字格面积）。
    """文件里**短字面量**的字符集 —— 非字面量实参（snprintf 结果 / 表格里的串）的保守超集。

    为什么只取"短"的：画布上会画出来的文案都是短语（"本局得分 %d"、"你赢了！"、
    软按钮标签），而同一个文件里的日志/URL/长提示不会进画布。取"全部字面量"会让
    中文@32 的字集从 ~360 涨到 611（数据量 +130KB）。max_len 是经验阈值，
    真正的保险是 C++ 侧的**跨档回退**（缺字就用别的档 1:1 画，不会变空白）。
    """
    try:
        # ★ 先剥注释：只有**真实字面量**才该进字库 —— 注释里的中文（尤其被英文双引号包起来的
        #   短语）一律不算。见 strip_comments() 的说明（2026-09-16：全工程 364 条这种短语，
        #   让 CJK@2 涨到 584 字/292KB，而它们永远不会画到屏上）。
        src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
    except OSError:
        return set()
    chars = set()
    for m in STR_RE.finditer(src):
        lit = unescape(m.group(1))
        if len(lit) <= max_len:
            chars.update(lit)
    return chars


def scan_requirements():
    """扫描画布文字调用点 → {(kind, N): set(字符)}。"""
    need = {}

    def add(kind, n, chars):
        need.setdefault((kind, n), set()).update(chars)

    stat = {'literal': 0, 'var': 0, 'skipped': 0}
    for path in source_files():
        # ★ 先剥注释：只有真实字面量才该进字库（注释里的引号中文一律不算）
        src = strip_comments(open(path, encoding="utf-8", errors="replace").read())
        flit = None
        for m in CALL_RE.finditer(src):
            name = m.group(1)
            i, depth, j = m.end(), 1, m.end()
            while j < len(src) and depth:
                if src[j] == '(':
                    depth += 1
                elif src[j] == ')':
                    depth -= 1
                j += 1
            args = split_args(src[i:j - 1])
            if any('const char *' in a for a in args):      # 函数定义（形参）
                stat['skipped'] += 1
                continue
            if len(args) < 2:
                continue
            scale_e, payload = args[-2], (args[-3] if len(args) >= 3 else '')
            is_big = name.startswith('big')
            if scale_e.isdigit():
                scales = [int(scale_e)]
            else:
                # 变量档位（fitBigText 算出来的）：只对真正会用变量的那套展开
                scales = range(1, (BIG_MAX_N if is_big else MAX_N) + 1)
            if payload.startswith('"') and payload.endswith('"'):
                stat['literal'] += 1
                text = unescape(payload[1:-1])
            else:
                stat['var'] += 1
                in_core = os.path.dirname(os.path.abspath(path)) == os.path.abspath(SRC_DIRS[0])
                if in_core:
                    if flit is None:
                        # 大字档（N>=3）只画短语，用短阈值收字；小字档放宽
                        lim = 8 if max(scales) >= 3 else 14
                        flit = file_literals(path, lim)
                    text = ''.join(flit)
                else:
                    text = ''.join(game_literals())
            for n in scales:
                if not (1 <= n <= MAX_N):
                    continue
                for ch in text:
                    if ch in '\n\t\r':
                        continue
                    if is_big and ch in BIG['chars']:
                        add('BIG', n, ch)
                    elif ord(ch) < 0x80:
                        add('ASCII', n, ch)
                    elif ord(ch) <= 0xFFFF:
                        add('CJK', n, ch)
    return need, stat


def render_cover(spec, n, ch, dx=0, dy=0):
    """按 N 档**原生**栅格化成 0..255 覆盖率图（cell = 基准 cell × N）。

    dx/dy = **每档的自动对位微调**（见 fit_offsets）：原生栅格化的墨迹会比
    "基准 × N" 略高/略宽（字形放大后 hinting 会多出 1~3px），照搬基准偏移会被字格裁掉。
    """
    cw, chh = spec['cell'][0] * n, spec['cell'][1] * n
    font = ImageFont.truetype(spec['font'], spec['size'] * n)
    scratch = Image.new("L", (cw * 3, chh * 3), 0)
    d = ImageDraw.Draw(scratch)
    l, t, r, b = d.textbbox((0, 0), ch, font=font)
    ox = ((cw - (r - l)) // 2 - l) if spec['center_x'] else 0
    d.text((ox + dx + cw, spec['oy'] * n + dy + chh), ch, fill=255, font=font)
    return scratch.crop((cw, chh, cw * 2, chh * 2))


def ink_box(spec, n, ch, dx=0, dy=0):
    """在大画布上量这个字的**真实墨迹范围**（相对字格左上角，可为负）。"""
    cw, chh = spec['cell'][0] * n, spec['cell'][1] * n
    font = ImageFont.truetype(spec['font'], spec['size'] * n)
    pad = cw
    im = Image.new("L", (cw + 2 * pad, chh + 2 * pad), 0)
    d = ImageDraw.Draw(im)
    l, t, r, b = d.textbbox((0, 0), ch, font=font)
    ox = ((cw - (r - l)) // 2 - l) if spec['center_x'] else 0
    d.text((ox + dx + pad, spec['oy'] * n + dy + pad), ch, fill=255, font=font)
    px = im.load()
    x0, x1, y0, y1 = 10 ** 9, -1, 10 ** 9, -1
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            if px[x, y] > 8:
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    if x1 < 0:
        return None
    return (x0 - pad, x1 - pad, y0 - pad, y1 - pad)


def fit_offsets(spec, n, codes):
    """算这一档的 (dx, dy)：让**所有**字形的墨迹都落在字格里。

    为什么需要（2026-09-15 实测）：原生按 `size = 基准×N` 栅格化后，墨迹高度不是
    严格线性的（例如中文 16px 时墨迹正好 16 行，32px 时要 33 行）⇒ 照搬 `oy*N`
    会把顶部裁掉 1~3px（字号越大越明显，用户看到的是"笔画像被切了"）。
    规则：
      · 竖直：整体下移 `-min(0, 墨迹上沿)`，让最高的字从第 0 行开始
        （旧版整数放大时墨迹上沿也在第 0 行 ⇒ 排版口径不变）。
      · 水平：若能靠平移装下就平移；**装不下就左右各裁一半**（对称裁 → 看不出来），
        绝不单边裁掉一条竖笔。
    """
    cw, chh = spec['cell'][0] * n, spec['cell'][1] * n
    gx0 = gy0 = 10 ** 9
    gx1 = gy1 = -1
    for ch in codes:
        bb = ink_box(spec, n, ch)
        if bb is None:
            continue
        gx0 = min(gx0, bb[0]); gx1 = max(gx1, bb[1])
        gy0 = min(gy0, bb[2]); gy1 = max(gy1, bb[3])
    if gx1 < 0:
        return 0, 0, None
    dy = -min(0, gy0)
    if gy1 + dy > chh - 1:                     # 实在高于字格 → 上下对称留白
        dy = (chh - (gy1 - gy0 + 1)) // 2 - gy0
    dx = -min(0, gx0)
    if gx1 + dx > cw - 1:                      # 实在宽于字格 → 左右对称裁
        dx = (cw - (gx1 - gx0 + 1)) // 2 - gx0
    # 复检：带上 (dx,dy) 后还有多少字形越界（>1px 就该处理，别放过）
    tops, nbad, worst, worst_ch = [], 0, 0, ''
    for ch in codes:
        bb = ink_box(spec, n, ch, dx, dy)
        if bb is None:
            continue
        x0, x1, y0, y1 = bb
        tops.append(y0)
        ov = max(-x0, x1 - (cw - 1), -y0, y1 - (chh - 1), 0)
        if ov > worst:
            worst, worst_ch = ov, ch
        if ov > 0:
            nbad += 1
    return dx, dy, dict(nbad=nbad, worst=worst, worst_ch=worst_ch,
                        top=(min(tops), max(tops)) if tops else (0, 0))


def pack_cov(img, bits):
    """覆盖率图 → 每行 ceil(w*bits/8) 字节，高位在左。返回 (bytes, 贴边行数)。"""
    w, h = img.size
    px = img.load()
    per_row = (w * bits + 7) // 8
    levels = (1 << bits) - 1
    out = bytearray()
    edge = 0
    for y in range(h):
        row = bytearray(per_row)
        acc = accbits = idx = 0
        for x in range(w):
            q = (px[x, y] * levels + 127) // 255
            acc = (acc << bits) | q
            accbits += bits
            if accbits >= 8:
                row[idx] = (acc >> (accbits - 8)) & 0xFF
                idx += 1
                accbits -= 8
                acc &= (1 << accbits) - 1
        if accbits:
            row[idx] = (acc << (8 - accbits)) & 0xFF
        if px[0, y] > 40 or px[w - 1, y] > 40:
            edge += 1
        out += row
    return bytes(out), edge


def art(img):
    px = img.load()
    ramp = ' .:-=+*#%@'
    return [''.join(ramp[min(9, px[x, y] * 10 // 256)] for x in range(img.size[0]))
            for y in range(img.size[1])]


def ink_h(kind, n, data):
    """该档字符集的"高字墨迹高"（数字/大写/中文），供 textH() 用。"""
    codes, masks = data[(kind, n)]
    spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
    cw, chh = spec['cell'][0] * n, spec['cell'][1] * n
    per_row = (cw * COV_BITS + 7) // 8
    top, bot = chh, -1
    for i, ch in enumerate(codes):
        if kind == 'ASCII' and not (ch.isdigit() or ch.isupper()):
            continue
        gbase = i * chh * per_row
        for y in range(chh):
            row = masks[gbase + y * per_row: gbase + (y + 1) * per_row]
            if any(row):
                top = min(top, y)
                bot = max(bot, y)
    return (bot - top + 1) if bot >= 0 else 0


def main():
    check_only = '--check' in sys.argv
    for spec in (ASCII, BIG, CJK):
        if not os.path.isfile(spec['font']):
            print('字体文件缺失: %s' % spec['font'])
            return 1

    need, stat = scan_requirements()
    print('扫描调用点: 字面量 %d 处 / 变量 %d 处 / 跳过函数定义 %d 处'
          % (stat['literal'], stat['var'], stat['skipped']))

    # ---- 每档字符集 ----
    sets = {}
    for n in range(1, MAX_N + 1):
        a = set(need.get(('ASCII', n), set())) | set(ALWAYS_ASCII)
        if n <= 2:
            a |= set(chr(c) for c in range(ASCII['first'], ASCII['last'] + 1))
        a = {c for c in a if ord(c) < 0x80}
        if a:
            sets[('ASCII', n)] = sorted(a)
        if n <= BIG_MAX_N:
            # ★★ 必须 **sorted**（2026-09-15 血案）：
            #   BIG['chars'] = " 0123456789.:-+" —— 按这个原顺序输出码点表就是
            #   [空格, '0'..'9', '.', ':', '-', '+']，其中 '.'/':'/'-'/'+' 排在 '9' **之后**
            #   ⇒ **非升序**；而运行时 `findGlyph` 是二分查找（要求升序）
            #   ⇒ '7' '8' '9' '.' ':' '-' '+' 永远查不到 ⇒ 回退用 ASCII 档画
            #   ⇒ 同一屏里数字大小差一倍（用户报障：数字华容道"字体有大有小"）。
            sets[('BIG', n)] = sorted(BIG['chars'])
        c = set(need.get(('CJK', n), set()))
        if n <= 2:
            c |= set(EXTRA_CJK)
        c = {ch for ch in c if 0x80 <= ord(ch) <= 0xFFFF}
        if c:
            sets[('CJK', n)] = sorted(c)

    # ---- 栅格化 ----
    data, warns, fits = {}, [], {}
    total = 0
    for (kind, n), codes in sorted(sets.items()):
        spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
        # ★ 每档先算"墨迹对位微调"：原生栅格化后墨迹比字格略高/略宽，
        #   照搬 base_y*N 会把顶部裁掉 1~3px（见 fit_offsets 的说明）。
        fdx, fdy, fit = fit_offsets(spec, n, codes)
        fits[(kind, n)] = fit
        masks, kept, blank = bytearray(), [], []
        for ch in codes:
            img = render_cover(spec, n, ch, fdx, fdy)
            packed, edge = pack_cov(img, COV_BITS)
            # ⚠️ 空格/制表符的字形**本来就是空的**，必须保留 —— 丢掉的话运行时
            #    findGlyph 找不到，会给空格画一个"缺字方框"。
            if not any(packed) and ch not in ' \t':
                blank.append(ch)
            if edge and not (kind == 'ASCII' and n == 1):
                warns.append('%s@%d "%s" 墨迹贴到字格边（相邻字会粘）' % (kind, n, ch))
            masks += packed
            kept.append(ch)
        data[(kind, n)] = (kept, bytes(masks))
        total += len(kept) * ((spec['cell'][0] * n * COV_BITS + 7) // 8) * (spec['cell'][1] * n)
        if blank:
            warns.append('%s@%d 空字形（字体里没有）: %s' % (kind, n, ''.join(blank)))

    print()
    print('%-6s %-3s %-10s %-6s %-11s %s'
          % ('字集', '档', '字格', '字数', '数据', '墨迹上沿/越界字形'))
    for kind, n in sorted(data, key=lambda k: (k[0], k[1])):
        codes, masks = data[(kind, n)]
        spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
        f = fits.get((kind, n)) or {}
        fi = ''
        if f:
            fi = '%d..%d / %d' % (f['top'][0], f['top'][1], f['nbad'])
            if f['worst']:
                fi += ' (%r %dpx)' % (f['worst_ch'], f['worst'])
        print('%-6s %-3d %-10s %-6d %7.1f KB  %s'
              % (kind, n, '%dx%d' % (spec['cell'][0] * n, spec['cell'][1] * n),
                 len(codes), len(masks) / 1024.0, fi))
    print('总数据量: %.1f KB' % (total / 1024.0))
    if warns:
        print()
        print('警告 %d 条:' % len(warns))
        for w in warns[:24]:
            print('   ' + w)

    if not check_only:
        print()
        for kind, n, sample in (('CJK', 2, '五子棋'), ('ASCII', 2, 'GAME'), ('BIG', 2, '2048')):
            spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
            print('=== %s N=%d 样例 "%s"（字符画）===' % (kind, n, sample))
            rows = [art(render_cover(spec, n, ch)) for ch in sample]
            for y in range(len(rows[0])):
                print('   ' + '  '.join(r[y] for r in rows))
            print()

    if check_only:
        return 1 if warns else 0

    # ---- 输出头文件 ----
    L = []
    L.append('// Auto-generated by tools/genfont.py -- DO NOT EDIT')
    L.append('//')
    L.append('// ★ 第二版（2026-09-15）：**原生多档 + %dbit 灰度覆盖率**，绘制时 1:1 贴像素。' % COV_BITS)
    L.append('//   · 档位 N=1..%d 对应调用点的 scale 实参（字格 = 基准 × N ⇒ 排版结果与旧版一致）' % MAX_N)
    L.append('//   · 基准：ASCII 8x12(consola %d) / BIG 16x24(consola %d) / CJK 16x16(simhei %d)'
             % (ASCII['size'], BIG['size'], CJK['size']))
    L.append('//   · 覆盖率 %d 级，行内高位在左；绘制用 blendPx 混合 ⇒ 边缘平滑' % (1 << COV_BITS))
    L.append('//   · **不做任何放大/插值**（放大 = 拉伸 = 台阶）')
    L.append('#ifndef PG_FONT_DATA_H_')
    L.append('#define PG_FONT_DATA_H_')
    L.append('')
    L.append('#include <stdint.h>')
    L.append('')
    L.append('namespace pg {')
    L.append('')
    L.append('const int PG_FONT_MAX_N = %d;' % MAX_N)
    L.append('const int PG_FONT_BIG_MAX_N = %d;   // bigText 档位上限（fitBigText 的 maxScale 也按它夹）' % BIG_MAX_N)
    L.append('const int PG_FONT_COV_BITS = %d;' % COV_BITS)
    L.append('')
    L.append('/* 一档字形表。masks 布局：count × cellH × ((cellW*bits+7)/8)，高位在左 */')
    L.append('struct FontSet {')
    L.append('  int cellW;')
    L.append('  int cellH;')
    L.append('  int inkH;              // 高字墨迹高（textH 用）')
    L.append('  int count;')
    L.append('  const uint16_t *codes; // 码点（升序）')
    L.append('  const uint8_t *masks;')
    L.append('};')
    L.append('')

    for kind in ('ASCII', 'BIG', 'CJK'):
        spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
        L.append('// ---------------------------------------------------------------- %s' % kind)
        for n in range(1, MAX_N + 1):
            if (kind, n) not in data:
                continue
            codes, masks = data[(kind, n)]
            cw, chh = spec['cell'][0] * n, spec['cell'][1] * n
            per_row = (cw * COV_BITS + 7) // 8
            L.append('// %s N=%d: %dx%d, %d 字, %d 字节/行' % (kind, n, cw, chh, len(codes), per_row))
            L.append('const uint16_t %s_C%d_CODES[%d] = {%s};'
                     % (kind, n, len(codes), ', '.join('0x%04X' % ord(c) for c in codes)))
            # 点阵用 C 字符串字面量存（每字节 4 字符，相邻字面量自动拼接）：
            # 比 "0xNN, " 省 1/3 体积，编译器解析也更快。
            # ⚠️ 字符串字面量结尾会隐含一个 NUL ⇒ 数组尺寸要 +1，否则报
            #    "initializer-string for array of chars is too long"。
            L.append('const uint8_t %s_C%d_MASKS[%d] =' % (kind, n, len(masks) + 1))
            step = per_row * 8          # 每个字面量 8 行，避免单条字面量过长
            esc = '\\x%02X'
            for i in range(0, len(masks), step):
                L.append('    "' + ''.join(esc % b for b in masks[i:i + step]) + '"')
            L.append('    ;')
            L.append('')

    for kind in ('ASCII', 'BIG', 'CJK'):
        spec = {'ASCII': ASCII, 'BIG': BIG, 'CJK': CJK}[kind]
        L.append('const FontSet %s_FONTS[PG_FONT_MAX_N + 1] = {' % kind)
        for n in range(0, MAX_N + 1):
            if n == 0 or (kind, n) not in data:
                L.append('    {0, 0, 0, 0, 0, 0},')
            else:
                codes, _ = data[(kind, n)]
                L.append('    {%d, %d, %d, %d, %s_C%d_CODES, %s_C%d_MASKS},'
                         % (spec['cell'][0] * n, spec['cell'][1] * n, ink_h(kind, n, data),
                            len(codes), kind, n, kind, n))
        L.append('};')
        L.append('')

    L.append('}  // namespace pg')
    L.append('')
    L.append('#endif  // PG_FONT_DATA_H_')
    # ---- 自检：每档的码点表必须**严格升序**（运行时 findGlyph 用二分查找）----
    # 非升序时二分**会静默漏字**（不报错、只是查不到 ⇒ 回退成别的档 ⇒ 字号/字形错），
    # 所以这里必须显式检查。血案见 sets[('BIG', n)] 处的注释。
    bad = []
    for key in sorted(data.keys()):
        codes = data[key][0]
        if any(codes[i] >= codes[i + 1] for i in range(len(codes) - 1)):
            bad.append('%s@%d' % key)
    if bad:
        print('  ★★ 码点表非升序（二分查找会漏字，必须修）：%s' % ' '.join(bad))
    else:
        print('  码点表升序自检：%d 个档位全部通过 ✓' % len(data))

    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    print('已生成: %s (%.1f KB)' % (OUT, os.path.getsize(OUT) / 1024.0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
