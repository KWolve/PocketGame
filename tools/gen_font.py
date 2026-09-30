#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_font.py - 生成项目自带字库（font/pocketgame.ttf）

背景（很重要）：
  设备系统内置的是**裁剪版** fzcircle.ttf，只含原固件用到的字形。本项目新加的中文
  在系统字库里没有 → 界面上的中文全部渲染成缺字方框（实测：主界面标题 5 个字
  形完全一样）。FlyThings 的 fun 流程规定：项目根目录放 font/*.ttf 并
  在 package.properties 里开 enable.font.location=true，就会**完全改用项目字体**。

做法：
  从系统汉字字体（simhei.ttf）裁出「本项目实际用到的字符」子集 —— 既能覆盖全部文案，
  又能把体积压到几十 KB（整字库 10MB 级别，不适合随资源推送）。

  字符集来源（自动扫描，避免漏字）：
    1) **ASCII 可见字符全量（0x20~0x7E）** —— 数字/字母必须全给！
       缺字形的表现**不是方框而是"整个字消失"**（实测：最高分 5800 只显示 "800"、
       167 只显示 "1"），比中文缺字更难发现。全量 ASCII 只有 95 个字形，体积可忽略。
    2) ui/ 目录下**所有** *.json 里所有 text 字段（一个 ftu 一个 json：main/wifi/remote…）
    3) src/**/*.cc|*.cpp|*.h 里所有字符串字面量
       （游戏标题/描述/提示/按键条等最终都会 setText 到控件上，用设备字体渲染）

  ★ 2026-09-23 改：基底字体与字符集下限**全部改用 MCP 裁好的基础中文字体**
    （`flythings-mcp-open/components/fonts/fonts/`，思源黑体 Source Han Sans / SIL OFL）：
      · `zkswe-hans-common.ttf`  872KB = GB2312 一级 3755 + 标点 + 全角 + ASCII  ← **字符集下限**
        ⚠️ 2026-09-24 起：工程自己的 `common_chinese()` 已经扩到 GB2312 **全集 6763 字**，
           比 MCP common（一级）更大 ⇒ 真正生效的下限是**全集**；这一条只剩
           "MCP 换字体/换路径时兜底"的意义（并集用，不会覆盖）。
      · `zkswe-hans-multi.ttf`   10.5MB = 全中文 + 扩展B + 拉丁/希腊/西里尔/假名/谚文 ← **取字形**
    为什么**不能直接把 MCP 字体整个拷进 `font/`**（MCP README 的默认做法）：
      · `common` 缺 35 个**本项目真在显示**的字（`骰`/诗词的 `茱萸貂裘`/频道名 `嵊圳浏`/`①`…）
        ⇒ 直接换会**静默少字**（缺字形 = 整字消失，不是方框，极难定位）。
      · `full`  缺 `°·×■●◆↑` 等符号；`multi` 才会齐。
      · `full`/`multi` 整个拷进去是 7.4~10.5MB，而 **/res 分区只有 7,995,392 字节**（MEMORY §42）。
    ⇒ 所以：**用 MCP 的 multi 取字形、用 MCP 的 common 定下限、输出仍是子集**
      （几十~一百多 KB），既吃上 MCP 裁好的字体、又零丢字。

用法:
    python tools/gen_font.py                 # 默认用 MCP multi
    python tools/gen_font.py --src <ttf>     # 换基底字体（应急/对比用）
    python tools/gen_font.py --no-mcp-floor  # 不加 MCP common 的字符集下限
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT_DIR = os.path.join(ROOT, "font")
OUT_TTF = os.path.join(OUT_DIR, "pocketgame.ttf")

# ---- 裁好的基础中文字体（思源黑体 / SIL OFL）----
# ⚠️ 本仓库不附带这两个字体文件（它们属于 FlyThings 工具链的组件）。
#    用环境变量 ZKSWE_MCP_FONTS（或 PG_MCP_FONTS）指向存放它们的目录。
MCP_FONTS_DIR = os.environ.get(
    "ZKSWE_MCP_FONTS",
    os.environ.get("PG_MCP_FONTS",
                   r"D:\zkswe\flythings-mcp-open\components\fonts\fonts"))
MCP_MULTI = os.path.join(MCP_FONTS_DIR, "zkswe-hans-multi.ttf")    # 取字形（覆盖最全）
MCP_COMMON = os.path.join(MCP_FONTS_DIR, "zkswe-hans-common.ttf")  # 定字符集下限
BASE_FONT = MCP_MULTI
FALLBACK_FONT = r"C:\Windows\Fonts\simhei.ttf"   # MCP 不在时才用
# ASCII 可见字符全量：分数/最高分/等级等运行时拼出来的数字都靠它
ASCII_PRINTABLE = "".join(chr(c) for c in range(0x20, 0x7F))
# ⚠️ 这里只放"确实会显示"的标点。2026-09-23 移除了 `℃`：
#     · 它原本是硬塞进来的，实际全工程只有 `src/dependencies/lib/libavcodec.a` 里出现过
#       （二进制库，不属于本工程的文案）⇒ 本就不显示；
#     · 而 **MCP 三档字体都没有 ℃ 字形**，留着它会让出包自检永远报赤字。
#     要重新用 ℃ 的话，得先换一个带该字形的基底字体（见文件头 BAT 说明）。
CJK_PUNCT = "，。、；：？！（）【】《》“”‘’—…·"

# ★ 2026-09-24 新增：**GB2312 收不进、但现实里常用**的汉字白名单。
#   为什么需要：GB2312 只有 6763 字，它**不含**一批很常见的人名/网名用字 ——
#   实测 `玥 / 珺 / 喆 / 垚 / 犇 / 骉` 这 6 个字（人名高频）在 GB2312 **全集**里都查不到，
#   于是输入法候选里永远没有它们（用户想给房间起个带"玥"的名字就做不到）。
#   它们属于 GBK/GB18030 独有字，要单独列。
# ⚠️ 加字只需往这里追加 —— 会**并集**进字符集，重复的字无害（set 去重）。
# ⚠️ 加之前确认**基底字体**（MCP zkswe-hans-multi.ttf）里有该字形，否则 gen_font.py 的
#    出包自检会当场报"缺 N 个字符"。误加的字会让构建失败，不会静默丢失。
# ⚠️ 这里**只收简体**：繁体字（鈺/瑩…）用不上，白占体积。
EXTRA_HANZI = (
    # 人名高频（GB2312 全集都不收的那批）
    "玥珺喆垚犇骉頔翀玙琤琬璆珩琀琚瑀璠璟珉瑄瑢璁璋瑗玢玦玠琋玧"
    "祎祺禛祾姮婳姽嫮嬿媖"
    # 网名/网络用字
    "囧槑烎巭嫑兲靐燚朤㵘尛歘恏羴猋麤龘"
    # 其它常见（人名/地名/品牌）
    "旸旻昶昱昉晞晅暐曌焓焜堃埜壵沄泠洢筱箐芃苒苪"
)

# ★ 只出现在**日志/QA dump 文本**里的字符：不上屏幕 ⇒ 不进字库。
#   为什么必须显式列出来：本文件"扫字符串字面量"无法区分 `LOGD("...")` 与
#   `setText("...")`，而 MCP 三档字体全都不含这几个符号 ⇒ 不排除就会让出包自检
#   报"缺 N 个字符"的**假警报**（真因是收集集不干净，不是字体不行）。
#   证据（2026-09-23 逐个 grep 过，命中处全是 LOGD/LOGW 或写 QA dump 文件）：
#     ★  U+2605  PgAlarm/PgBt/PgDns/PgH264/PgHls/haLogic 的 LOGD + QA dump 前缀
#     ≈  U+2248  mainLogic.cc 的 LOGD（帧间隔≈…us）
#     ⚠  U+26A0  各文件的 LOGD/LOGW（**界面上原来有两处**：probeLogic 的
#                「⚠ %d 个高危网络」与「⚠ 高危/⚠ 发现」—— 2026-09-23 已改成「！」，所以现在只剩日志）
#     \uFE0F     变体选择符（跟 `⚠️` 一起来的，本身不可见）
#   ⚠️ 若哪天要把它们放到界面上：**把它从这张表里删掉**，自检会立刻报赤字提醒你换字体。
NOT_DISPLAYED = "★⚠≈\ufe0f"


def common_chinese():
    """**GB2312 全集**（一级 3755 + 二级 3008 = 6763 字）+ 常用中文标点
    —— 「联网文本 / 用户输入」型界面的兜底字集。

    为什么必须有它（2026-09-14 屏保"每日诗词"引入）：
      诗词是**运行时从网络抓来的**，字集在打包时无法枚举；而本项目的 font/*.ttf 是
      **完全替换**系统字体的子集字库、没有逐字回退 —— 字库里缺哪个字，
      那个字在屏面上就**整个消失**（不是方框，更难发现）。
      ⇒ 凡是"内容来自网络/用户输入"的界面，字库就必须覆盖常用字全集，
        否则会出现"诗句缺字"这种看起来像 bug 的现象。

    ★ 2026-09-24 改：**一级 → 全集**（多了二级 3008 字）。
      触发原因是**输入法**（ime.ftu）——它是最典型的"用户输入"型界面：
      候选表按字库 cmap 过滤（见 tools/gen_ime_pinyin.py），字库只有一级字的话，
      用户想打「鑫 / 淼 / 婷 / 妍 / 怡 / 璇 / 瑜 / 瑾 / 喆 / 玥」这类**人名高频字**
      就一个字都出不来（实测：常用人名 30 字里 19 个打不出）。
      二级字同时惠及：HA 实体名、WiFi SSID、屏保诗词（来自网络）。
      代价（实测，不是估计）：ttf **944KB → 1743KB（+818KB）**；
        res 分区 7,995,392 字节，当时升级包 4.11MB → 4.93MB，余量仍 >2.6MB；
        基底字体 zkswe-hans-multi.ttf 里二级字字形**齐全（缺 0 个）**。
      若哪天闪存吃紧要退回去：把 hi 的上界从 0xF8 改回 0xD8 即可（一级字）。"""
    out = []
    for hi in range(0xB0, 0xF8):          # GB2312 全集：0xB0..0xD7 一级 / 0xD8..0xF7 二级
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    out.extend(chr(c) for c in range(0x3000, 0x3040))   # CJK 标点/符号
    out.extend(chr(c) for c in range(0xFF01, 0xFF5F))   # 全角 ASCII
    return out


def collect_from_channels(path, chars):
    """把 IPTV 频道表里**会显示的字**全部收进字库。

    为什么单独有这一条（2026-09-14）：频道表由 tools/gen_channels.py 生成，
    中文频道名 / 分组 / 备注（"仅限国内"）/ 实测行（"720x404@25.60fps"）都会显示在
    选台列表里，而这张表**随时会换**。只靠 RUNTIME_TEXT 白名单的话，换一个地名就
    静默丢一个字 —— 实测踩到：`嵊州综合` 的"嵊"不在 GB2312 **一级**字库（3755 字）里，
    列表里直接显示成"州综合"。
    整表扫一遍最省事：多收几十个字，对 ttf 体积影响可忽略。
    """
    if not os.path.isfile(path):
        print("!! 频道表不存在，跳过收字：%s" % path)
        return
    try:
        # ⚠️ 用内建 open（本文件没有 import io；上一版就是踩了这个，异常被吞掉、
        #    字库静默少了几十个字，检查 cmap 才发现"嵊"仍然缺）
        with open(path, encoding="utf-8") as f:
            chars.update(ch for ch in f.read() if ord(ch) > 0x7F)
        print("频道表收字：%s" % os.path.basename(path))
    except Exception as e:  # noqa: BLE001
        print("!! 读频道表失败（跳过收字）：%s" % e)


def collect_from_json(path, chars):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return
    stack = [data]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "text" and isinstance(v, str):
                    chars.update(v)
                else:
                    stack.append(v)
        elif isinstance(node, list):
            stack.extend(node)


def strip_code(text):
    """去掉 C/C++ 的注释，返回剩余代码（保留字符串字面量原样）。

    为什么必须做（2026-09-23 换 MCP 字体时暴露）：
      本文件的"扫字符串字面量"用正则跑**整份源文件**，把**注释里**引号包起来的
      说明文字也当成了要显示的内容。本工程的注释风格恰好大量使用：
        · `// 实测建议 ≥ 16`        → 收进 `≥`
        · `/* ★★ 抗锯齿 ... */`      → 收进 `★`
        · `// ⚠️ 注意 ...`           → 收进 `⚠` 和变体选择符 U+FE0F
        · `/* ... 约 ≈ 1e7 ... */`
      这些字**永远不会显示**，却让字库白白背上"缺字形"的假警报 ——
      实测 MCP 三档字体全部缺 `⚠ ★ ≥ ≈ ℃`，于是自检报"输出缺 8 个字符"，
      真因是**收集集本身不干净**，不是字体不行。

    ⚠️ 必须用状态机，不能正则/按行切：字符串里的 `//`（如 `"http://"`）会被误判成注释、
      注释里的 `"` 会把后面整段代码吞掉。字符字面量 `'/'`、转义 `\\"` 都要处理。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        # ---- 字符串字面量：整段原样保留 ----
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        # ---- 字符字面量 ----
        if c == "'":
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == '\\' and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        # ---- 行注释 ----
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                i += 1
            continue
        # ---- 块注释 ----
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            i += 2
            while i + 1 < n and not (text[i] == '*' and text[i + 1] == '/'):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def collect_from_sources(dirs, chars):
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith((".c", ".cc", ".cpp", ".h")):
                continue
            with open(os.path.join(d, fn), encoding="utf-8") as f:
                src = strip_code(f.read())     # ★ 先剥注释：注释里的字不显示，不该进字库
            for m in pat.finditer(src):
                chars.update(m.group(1))


def font_cmap_text(path):
    """读一个 ttf 的 cmap，返回它**能显示的全部字符**（字符串）。读不到就返回 ""。

    ⚠️ 静默失败必须消灭：读不到要**打印原因**，不能悄悄返回空 —— 空 cmap 会让
       "字符集下限"整条失效，而输出看着完全正常（就是少字）。
    """
    if not os.path.isfile(path):
        print("!! 字体不存在，跳过取 cmap：%s" % path)
        return ""
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("!! 没有 fontTools，无法取 cmap：%s" % path)
        return ""
    try:
        f = TTFont(path, lazy=True)
        cmap = f.getBestCmap()
        chars = "".join(chr(cp) for cp in cmap.keys())
        f.close()
        return chars
    except Exception as e:  # noqa: BLE001
        print("!! 读 cmap 失败（%s）：%s" % (os.path.basename(path), e))
        return ""


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--src", default="", help="基底字体（默认 MCP zkswe-hans-full.ttf）")
    ap.add_argument("--no-mcp-floor", action="store_true",
                    help="不加 MCP common 的字符集下限（默认加）")
    args = ap.parse_args()

    base = args.src or BASE_FONT
    if not os.path.isfile(base):
        print("!! 基底字体不存在：%s" % base)
        if not args.src and os.path.isfile(FALLBACK_FONT):
            print("   退回本机 simhei：%s（临时应急；MCP 字体在的话请修路径）" % FALLBACK_FONT)
            base = FALLBACK_FONT
        else:
            return 1
    print("基底字体：%s" % base)

    chars = set(ASCII_PRINTABLE) | set(CJK_PUNCT) | set(EXTRA_HANZI)
    # 多 ftu：扫描 ui/ 下所有 *.json（main/wifi/...）
    ui_dir = os.path.join(ROOT, "ui")
    for fn in sorted(os.listdir(ui_dir)):
        if fn.endswith(".json"):
            collect_from_json(os.path.join(ui_dir, fn), chars)
    collect_from_sources(
        [os.path.join(ROOT, "src", "core"), os.path.join(ROOT, "src", "logic"),
         os.path.join(ROOT, "src", "platform")], chars)

    # ⚠️ 运行时由**框架**生成的文本：源码与 json 里都查不到，必须白名单，否则显示方框。
    #    digitalclock 的 data-format="EEEE" → 框架按 locale 输出「星期日/星期一/…」
    #    （屏保页用了它，见 ui/screensaver.html）。
    #    这与"snprintf 拼出来的分数不在字面量里"是同一类问题。
    RUNTIME_TEXT = "星期一二三四五六日天"
    chars.update(RUNTIME_TEXT)
    # 联网/用户输入型文本（屏保诗词）→ 必须覆盖常用字全集，见 common_chinese() 说明
    chars.update(common_chinese())
    # IPTV 频道表（中文名/分组/备注/实测行都会显示在选台列表里）→ 整表收字
    collect_from_channels(os.path.join(ROOT, "resources", "iptv", "channels.m3u"), chars)

    # ★ 控制字符不属于"字形"：换行/制表是排版指令，字库里没有也不需要它们的字形
    #   （实测收集集里混进了 U+000A —— 来自 ui/*.json 的多行 text 字段，会让自检报假缺口）。
    #   NOT_DISPLAYED = 只在日志/dump 里出现的符号，见该常量的说明。
    junk = sorted(c for c in chars if ord(c) < 0x20 or c in NOT_DISPLAYED)
    if junk:
        chars.difference_update(junk)
        print("排除 %d 个「无字形 / 不上屏幕」的字符：%s"
              % (len(junk), " ".join("U+%04X" % ord(c) for c in junk)))

    # ★ MCP common 的 cmap = **字符集下限**：MCP 裁好的"常用中文"该有的字一个不能少。
    #   ⚠️ 必须在 common_chinese() 之后并集（不是替换）—— 上面那几条是"本项目实际用字"，
    #      比 common 多（实测多 35 个）。
    if not args.no_mcp_floor:
        floor = font_cmap_text(MCP_COMMON)
        if floor:
            before = len(chars)
            chars.update(floor)
            print("MCP common 字符集下限：%s（%d 字符；并集后新增 %d 个）"
                  % (os.path.basename(MCP_COMMON), len(floor), len(chars) - before))
        else:
            print("!! MCP common 读不到 ⇒ **本轮的字符集下限没生效**（输出仍可用，"
                  "只是不保证覆盖 MCP 常用字全集）")

    cjk = sorted(c for c in chars if ord(c) > 0x7F)
    if not cjk:
        print("没有收集到中文字符")
        return 1
    print("收集字符: ASCII %d 个, 非 ASCII %d 个" %
          (len([c for c in chars if ord(c) <= 0x7F]), len(cjk)))
    print("  非 ASCII: %s" % "".join(cjk))

    try:
        from fontTools import subset
    except ImportError:
        # ⚠️ 2026-09-15：这台机器上 fontTools 装在**隔离环境**里（不污染全局 python），
        #    所以直接 `python tools/gen_font.py` 会走到这里 —— 用下面那个解释器跑即可。
        print("缺少 fontTools（裁字库要它）。两种跑法：")
        print("  ① 用已装好 fontTools 的隔离环境：")
        print("     C:\\Users\\Admin\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe"
              " tools/gen_font.py")
        print("  ② 或给当前解释器装：python -m pip install fonttools")
        return 1

    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    text = "".join(sorted(chars))
    args = [base, "--text=%s" % text, "--output-file=%s" % OUT_TTF,
            "--layout-features=", "--no-hinting", "--desubroutinize",
            "--drop-tables+=DSIG"]
    subset.main(args)

    # ★ 出包后自检：把输出字体的 cmap 与"收集到的字符集"对一遍，**缺一个就报**。
    #   理由：缺字形是**整字消失**（不是方框），现场表现为"某个字莫名其妙没了"，
    #   极难定位。这里当场证明"要的字都在"。
    got = set(font_cmap_text(OUT_TTF))
    missing = sorted(c for c in chars if c not in got)
    if missing:
        # ⚠️ 这里**必须打码点**（U+XXXX），不能只打字符：本机控制台是 GBK，
        #    中文/符号打出去会变成乱码（实测 `°·×` 在日志里成了 `掳路脳`），
        #    那样等于没有可读的判据。
        print("!! ★ 自检失败：输出字体缺 %d 个字符（基底字体 %s 里就没有字形）"
              % (len(missing), os.path.basename(base)))
        for c in missing:
            print("     U+%04X  %s" % (ord(c), c))
        print("   ⇒ 要么给 --src 换一个覆盖更全的基底字体，要么把这些字从文案里换掉。")
        return 2
    print("自检：输出 cmap 覆盖全部 %d 个字符 ✓" % len(chars))

    size = os.path.getsize(OUT_TTF)
    print("已生成: %s (%d 字节 / %.1f KB)" % (OUT_TTF, size, size / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
