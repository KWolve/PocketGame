#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gen_ime_pinyin.py - 生成输入法的拼音索引（src/logic/imePinyinData.h）

为什么需要它：
  本板没有系统级中文输入法（工程自带的 IME 只有 45 个 ASCII 键，见 ui/ime.html）。
  用户要"改名时能自己输入中文" ⇒ 给自研 IME 加**全拼**输入：打 `ketingdeng`
  就能出「客厅灯」。需要一个 拼音 -> 字/词 的表。

两条硬约束（都来自本项目已踩过的血案）：
  1. ★ **只能用字库里真有字形的字**。本工程 font/*.ttf 是**完全替换**系统字体的子集，
     没有逐字回退 ⇒ 候选里放一个缺字形的字，用户选中后那个字**在屏幕上整个消失**
     （不是方框）。所以候选集 = 读 `font/pocketgame.ttf` 的 cmap 得到"可显示字"，
     再与拼音表取交集。
  2. ★ **候选顺序决定好不好用**。同一个拼音（如 `shi`）有几十个常用字，
     排在第一位的最容易被点到 ⇒ 顺序必须是「设备改名常用」优先：
       ① 人工排的常用字优先级串（房间/家电/方位/数量）
       ② 项目自身文案里出现过的字
       ③ 其余按码点（保证确定性：同样的输入永远同样的顺序）

用法（需要 pypinyin + fontTools）：
    python tools/gen_ime_pinyin.py
    # 本机用隔离环境（fontTools 装在里面）：
    # C:\\Users\\Admin\\.workbuddy\\binaries\\python\\envs\\default\\Scripts\\python.exe \\
    #     tools/gen_ime_pinyin.py

==================== 2026-09-24：候选"太少"的整改 ====================
用户反馈「输入法的候选词数量太少、缺少候选字」。逐项量过之后的**真因**（都是硬数字）：
  · 字库只裁了 GB2312 **一级** 3755 字 ⇒ 二级字一个都进不了候选表：
    常用人名 30 字里 **19 个打不出来**（鑫/淼/婷/妍/怡/璇/瑜/瑾/喆/玥…）。
    修法在 gen_font.py 的 common_chinese()（一级 → 全集 6763 字），
    **本文件不用改就自动跟上**（候选表按字库 cmap 过滤）。
  · `MAX_WORDS=1500` 把自动词砍掉 3525 条；`MAX_CHARS_PER_PY=60` 会把
    `yi`(149)/`ji`(130)/`yu`(122) 这类高频同音字截掉一半。
  · 人工词表实际只生效 167 条 —— 太少，而自动抽的 4858 条多是 n-gram 碎片。

⚠️ **两个上限必须成对改**（否则改了等于没改）：
   本文件的 `MAX_CHARS_PER_PY` ↔ `src/logic/imeApp.cc` 的 `kMaxCand`（运行时候选总数）。
   生成器放宽了、运行时还卡在 60，用户看到的仍然只有 60 个候选。

⚠️ 生成顺序：**先 gen_font.py（出字库）→ 再本文件（按 cmap 过滤）**。
   反过来跑会用旧字库过滤，二级字又被丢掉。
"""
import io
import importlib.util
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
FONT_TTF = os.path.join(ROOT, "font", "pocketgame.ttf")
OUT_H = os.path.join(ROOT, "src", "logic", "imePinyinData.h")

MAX_CHARS_PER_PY = 150    # 单字每拼音上限。★ 2026-09-24 由 60 提到 150：
                          #   GB2312 全集里同音字最多的是 `yi` = **149 字**（一级 60 → 全集 149），
                          #   150 是"一个字都不砍"的最小值（实测：>150 的拼音 0 个）。
                          #   注意改它必须同步改 imeApp.cc 的 `kMaxCand`（运行时候选总数上限），
                          #   否则运行时还是会被截到旧值 —— 那样改了等于没改。
MAX_WORDS_PER_PY = 10     # 同一拼音下最多放几个**词**。★ 2026-09-24 由写死的 `[:6]` 提到 10：
                          #   原来 6 是拍脑袋定的；实测全集下没有拼音超 6（瓶颈是 MAX_WORDS），
                          #   提到 10 是给"房间×设备"组合词留余量（如 `keting` 有 8 个词）。
                          #   别调太大：候选条一页只有 5 格，词太多会把单字挤到后面几页。
MAX_WORDS = 2500          # 词组总上限（控 .so 体积）；**人工词表永不截断**，只截自动抽取的。
                          # ★ 2026-09-24 由 1500 提到 2500：1500 时自动词被砍掉 3525 条，
                          #   连界面文案里的词（关于/上页/下页/最高分…）都进不来。
MIN_WORD_LEN = 2
MAX_WORD_LEN = 4          # 上限 4 是**版面约束**：候选条一格 92px @ 字号 22 只放得下 4 个汉字
                          #   （5 字 = 110px 会被裁）。要收 5 字词得先改 ui/ime.html 的候选格。

# ---- 常用字优先级串：越靠前越优先（按"设备名/房间/家电/方位/数量"排）----
PRIORITY = (
    "灯客厅卧室厨房餐厅卫生间浴室阳台书房玄关走廊楼梯主次儿童房老人"
    "吊吸顶落地台壁射筒氛围带条串彩暖白开关插座面板"
    "空调风扇电视音响投影窗帘门锁猫眼传感温湿度人体光照水浸烟燃气"
    "扫地净化加湿热水壶饭煲微波炉烤冰箱洗衣烘干饮水路由摄像铃"
    "上下左右前后里外中间大小多少新旧高低东西南北"
    "一二三四五六七八九十百千万零两半个这那些"
    "关销模式场景回家离睡影院阅读派对夜起床工作休息"
    "机电风火水气声光亮暗热冷干湿"
    "房厅室厨卫台梯门窗墙地天花"
    "网线路由信号频道"
    "个只双套把张块条件层位"
    # ---- ★ 2026-09-24 新增：GB2312 **二级**字里的高频字 ----
    # 为什么必须显式列：单字候选的排序键是「PRIORITY 下标 → 工程文案里出现过 → 码点」，
    #   而二级字的 Unicode 码点天然比一级字大 ⇒ 不列进来就**永远排在候选最后几页**，
    #   用户要翻 20 多页去找（`yi` 全集下 149 个字）。列进来它们就落到第 1~2 页。
    #   ⚠️ 只列**别人名/家居常用**的：字库已含全部二级字，这里纯是排序优化，
    #     列多无害（不在字库里的字会被 cmap 遍历自然跳过），但太长会稀释前面的权重。
    # ⚠️ 别把上面已有的字在这里再写一遍：PRIORITY 用 dict 建索引，**后出现的会覆盖前面的**，
    #    重复等于把该字的优先级降到最后。
    "鑫淼喆玥瑾瑜璇珂玮琛煜烨炜璐琦珺婷妍怡嫣婧媛婵娟嫒"
    "萌蕾蕊薇茜蕴芸萱涵淇沛柏楷森磊垚焱犇骉"
    "彤晗曦熙宸轩豪"
    "榻帷幔凳熨"
    # GBK 独有、但用于人名/网名很常见的字（GB2312 全集不含，见 gen_font.py 的 EXTRA_HANZI）
    "玥珺喆垚犇骉頔翀玙琤琬璆珩琀琚瑀璠璟珉瑄瑢璁璋瑗玢玦玠琋玧"
    "祎祺禛祾姮婳姽嫮嬿媖囧槑烎巭嫑兲靐燚朤㵘尛歘恏羴猋麤龘"
    "旸旻昶昱昉晞晅暐曌焓焜堃埜壵沄泠洢筱箐芃苒苪"
)

# ---- 人工词组表：改名/命名最常用的组合（全拼直出，一次点中）----
WORDS_EXTRA = """
客厅 卧室 厨房 餐厅 卫生间 洗手间 浴室 阳台 书房 玄关 走廊 楼梯 车库 储藏室 衣帽间
主卧 次卧 儿童房 老人房 客房 婴儿房 保姆房 宠物房 健身房 影音室 娱乐室 工作间
主灯 客厅灯 卧室灯 厨房灯 餐厅灯 阳台灯 书房灯 玄关灯 走廊灯 楼梯灯 卫生间灯
吊灯 吸顶灯 台灯 落地灯 壁灯 射灯 筒灯 灯带 灯条 氛围灯 夜灯 床头灯 镜前灯 小夜灯
彩色灯 冷暖灯 前灯 后灯 顶灯 侧灯 阅读灯
开关 插座 面板 开关面板 智能开关 智能插座 双联开关 单联开关 三联开关
空调 立式空调 挂式空调 中央空调 风扇 风扇灯 吊扇 落地扇 排气扇 新风 加湿器 除湿机
净化器 空气净化器 扫地机 扫地机器人 吸尘器 热水器 热水壶 电饭煲 微波炉 烤箱 冰箱
洗衣机 烘干机 洗碗机 饮水机 消毒柜 抽油烟机 燃气灶 电磁炉
电视 电视机 投影仪 音响 音箱 功放 机顶盒 播放器 摄像头 门铃 门锁 智能门锁 猫眼
窗帘 电动窗帘 纱帘 遮阳帘 卷帘 百叶窗 天窗 大门 前门 后门 房门 阳台门 车库门
温度 湿度 人体 光照 水浸 烟雾 燃气 门磁 窗磁 传感器 探测 报警 告警
回家 离家 睡眠 影院 阅读 派对 夜晚 起床 工作 休息 用餐 会客 清洁 全部关闭 全部打开
场景 模式 自动化 定时 倒计时
# ---- ★ 2026-09-24 扩充：人工词表是候选质量的主力，自动抽的是 n-gram 碎片 ----
# 为什么必须人工列（实测结论）：`auto` 那 4858 条里有 "关的设备/出现在下/到点会自"
#   这种碎片，**只能是填充料**；用户真正会打的词（房间×设备组合）必须显式列。
#   加词只受两条约束：① 2~4 字（MAX_WORD_LEN，候选格放得下 4 字）；
#                     ② 字在字库里（扩到 GB2312 全集后基本都满足）。
# ---- 房间 × 设备（命名最高频，全拼一次点中）----
厨房空调 厨房灯带 餐厅空调 餐厅灯带 书房空调 书房窗帘 玄关窗帘 走廊灯带
卫生间排气扇 卫生间浴霸 阳台窗帘 阳台空调 主卧空调 主卧窗帘 次卧空调 次卧窗帘
儿童房空调 儿童房窗帘 老人房空调 客房空调 客房灯 客房窗帘 洗衣房 洗衣房灯
储物间 设备间 配电间 地下室 阁楼 露台 阳光房 茶水间 办公室 会议室
# ---- 更多家电 ----
浴霸 地暖 浴缸 马桶 晾衣架 净水器 软水机 蒸箱 电陶炉 咖啡机 面包机 豆浆机
破壁机 空气炸锅 气炸锅 电压力锅 电暖器 油汀 取暖器 暖风机 冷风扇 空调扇
电蚊香 灭蚊灯 香薰机 电水壶 电蒸锅 电饼铛 厨师机 榨汁机 消毒机 干衣机 挂烫机
熨烫机 除螨仪 吸顶扇 换气扇 新风机 浴帘 花洒 水龙头 地漏
# ---- 网络 / 智能家居 ----
路由器 光猫 交换机 网线 网关 中继 无线 信号 频道 密码 账号 面板 场景卡
自动化 定时器 倒计时 传感器 执行器 联动 触发器 状态 开关状态
# ---- 门窗 / 安防 ----
推拉门 纱窗 卷帘门 防盗门 指纹锁 密码锁 门磁 窗磁 红外 报警器 监控 摄像 可视门铃
# ---- 方位 / 描述（"左边那盏""后面那台"）----
左边 右边 上面 下面 里面 外面 中间 前面 后面 旁边 上边 下边 前边 后边 左侧 右侧
# ---- 场所（面板用于民宿/酒店/办公室时）----
公寓 别墅 商铺 办公室 会议室 教室 病房 民宿 酒店 宾馆 前台 大堂 包间 大厅
# ---- 数量 / 状态 ----
一个 两个 三个 四个 五个 全部 所有 单个 多个 打开 关闭 开启 停止 暂停 继续 恢复
# ---- 房间 × 设备（原有组合，保留）----
客厅主灯 客厅灯带 客厅空调 客厅窗帘 客厅电视 卧室主灯 卧室灯带 卧室空调 卧室窗帘
主卧灯 次卧灯 儿童房灯 阳台灯 玄关灯 书房灯
""".split()


def load_gen_font():
    """复用 gen_font 的收集/剥注释实现（同一套口径，避免两处标准漂移）。"""
    spec = importlib.util.spec_from_file_location(
        "gf", os.path.join(ROOT, "tools", "gen_font.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def font_cmap():
    """字库里**真有字形**的码点集合（只有这些字能出现在候选里）。"""
    from fontTools.ttLib import TTFont
    f = TTFont(FONT_TTF, lazy=True)
    s = set(f.getBestCmap().keys())
    f.close()
    return s


def project_blob():
    """项目自身文案（ui/*.json 的 text + src 里剥过注释的字符串字面量）。"""
    gf = load_gen_font()
    blob = []
    ui = os.path.join(ROOT, "ui")
    for fn in sorted(os.listdir(ui)):
        if not fn.endswith(".json"):
            continue
        try:
            data = json.load(io.open(os.path.join(ui, fn), encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            print("!! 读 %s 失败（跳过）：%s" % (fn, e))
            continue
        stack = [data]
        while stack:
            node = stack.pop()
            if isinstance(node, dict):
                for k, v in node.items():
                    if k == "text" and isinstance(v, str):
                        blob.append(v)
                    else:
                        stack.append(v)
            elif isinstance(node, list):
                stack.extend(node)
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for d in ("core", "logic", "platform"):
        dd = os.path.join(ROOT, "src", d)
        if not os.path.isdir(dd):
            continue
        for fn in sorted(os.listdir(dd)):
            if fn.endswith((".c", ".cc", ".cpp", ".h")):
                src = gf.strip_code(io.open(os.path.join(dd, fn), encoding="utf-8").read())
                blob.extend(m.group(1) for m in pat.finditer(src))
    return blob


def build_words(avail, blob):
    """词组候选，分两类返回：`(curated, auto)`。

    ★ 为什么必须分开：`curated` 是人工挑的（客厅灯/窗帘/卫生间…），**永远不截断**；
      `auto` 是从工程文案里抽的 n-gram，量大且噪声多，只按频率填满剩余额度。
      踩过的坑：2026-09-23 第一版把两者合成一个 dict 后统一截断，
      结果 `窗帘`/`客厅灯` 因为拼音较长被截掉了 —— 正好是最该有的词。
    """
    curated = []
    for w in WORDS_EXTRA:
        if MIN_WORD_LEN <= len(w) <= MAX_WORD_LEN and all(ord(c) in avail for c in w):
            if w not in curated:
                curated.append(w)
    auto = {}
    text = "\n".join(blob)
    for m in re.finditer(r"[\u4e00-\u9fff]{%d,%d}" % (MIN_WORD_LEN, MAX_WORD_LEN), text):
        w = m.group(0)
        if not all(ord(c) in avail for c in w):
            continue      # 词里有一个字没字形就不收（否则用户选中后少字）
        if w in curated:
            continue
        auto[w] = auto.get(w, 0) + 1
    return curated, auto


def main():
    try:
        from pypinyin import pinyin, Style
    except ImportError:
        print("缺少 pypinyin（拼音数据源）。装一个：python -m pip install pypinyin")
        return 1
    if not os.path.isfile(FONT_TTF):
        print("字库不存在：%s（请先跑 tools/gen_font.py）" % FONT_TTF)
        return 1

    cmap = font_cmap()
    blob = project_blob()
    allowed = {c for c in set("".join(blob)) if ord(c) in cmap}

    # ---------------- 单字表 ----------------
    pidx = {c: i for i, c in enumerate(PRIORITY)}
    by_py = {}
    for cp in sorted(cmap):
        ch = chr(cp)
        if not ("\u4e00" <= ch <= "\u9fff"):
            continue
        try:
            # heteronym=True：多音字拿全部读音（「行」要同时有 xing / hang）
            rs = pinyin(ch, style=Style.NORMAL, heteronym=True)
        except Exception:  # noqa: BLE001
            continue
        if not rs:
            continue
        for py in {r.strip().lower() for r in rs[0] if r and r.strip()}:
            if not re.fullmatch(r"[a-z]+", py):
                continue
            by_py.setdefault(py, []).append((pidx.get(ch, 9999), cp))
    chars_out = []
    for py in sorted(by_py):
        # 排序键：人工优先级 -> 工程文案里出现过 -> 码点（确定性）
        lst = sorted(by_py[py], key=lambda t: (t[0], 0 if chr(t[1]) in allowed else 1, t[1]))
        seen = []
        for _, cp in lst:
            ch = chr(cp)
            if ch not in seen:
                seen.append(ch)
            if len(seen) >= MAX_CHARS_PER_PY:
                break
        chars_out.append((py, "".join(seen)))

    # ---------------- 词组表 ----------------
    curated, auto = build_words(cmap, blob)
    # 人工词给高权重（保证它在同一拼音的候选里排前面），自动词用出现次数
    budget = MAX_WORDS - len(curated)
    if budget < 0:
        print("!! 人工词表 %d 条已超上限 %d —— 要么删词，要么调大 MAX_WORDS"
              % (len(curated), MAX_WORDS))
        budget = 0
    picked = list(curated) + [w for w, _ in
                              sorted(auto.items(), key=lambda t: (-t[1], len(t[0]), t[0]))[:budget]]
    weight = {w: 50 for w in curated}
    weight.update({w: auto[w] for w in picked if w in auto})

    w_by_py = {}
    for w in picked:
        try:
            rs = pinyin(w, style=Style.NORMAL, heteronym=False)
        except Exception:  # noqa: BLE001
            continue
        py = "".join((r[0] if r else "") for r in rs).lower()
        if not re.fullmatch(r"[a-z]+", py):
            continue
        w_by_py.setdefault(py, []).append((weight[w], w))
    words_out = []
    for py in sorted(w_by_py):
        lst = sorted(w_by_py[py], key=lambda t: (-t[0], t[1]))
        words_out.append((py, " ".join(w for _, w in lst[:MAX_WORDS_PER_PY])))

    # ---------------- 落盘 ----------------
    out_dir = os.path.dirname(OUT_H)
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    L = []
    A = L.append
    A("/* imePinyinData.h - 输入法拼音索引（**自动生成，勿手改**）")
    A(" *")
    A(" * 生成器：tools/gen_ime_pinyin.py")
    A(" * 数据源：pypinyin（读音）+ font/pocketgame.ttf 的 cmap（**只收有字形的字**）")
    A(" *         + 工程自身文案（决定候选顺序）")
    A(" *")
    A(" * 候选顺序为什么自定义：改名场景下打 `shi` 最该先出「室」这类常在设备名里")
    A(" * 出现的字，而不是字典序第一位的字。")
    A(" *")
    A(" * ⚠️ 表按 py **升序**（查找靠二分 + 前缀扫），生成器已保证，不要手工插行。")
    A(" */")
    A("#ifndef PG_IME_PINYIN_DATA_H_")
    A("#define PG_IME_PINYIN_DATA_H_")
    A("")
    A("struct ImePyEnt {")
    A('  const char *py;    // 全拼（小写、无音调），如 "keting"')
    A("  const char *text;  // 词组：多个候选用**空格**分隔；单字表：紧凑串")
    A("};")
    A("")
    A("// 词组表（先匹配它 —— 一次点中一个词）")
    A("static const ImePyEnt kImePyWords[] = {")
    for py, txt in words_out:
        A('    {"%s", "%s"},' % (py, txt))
    A("};")
    A("static const int kImePyWordCount = %d;" % len(words_out))
    A("")
    A("// 单字表（词组没匹配上时用；每格最多 %d 个字）" % MAX_CHARS_PER_PY)
    A("static const ImePyEnt kImePyChars[] = {")
    for py, txt in chars_out:
        A('    {"%s", "%s"},' % (py, txt))
    A("};")
    A("static const int kImePyCharCount = %d;" % len(chars_out))
    A("")
    A("#endif  // PG_IME_PINYIN_DATA_H_")
    io.open(OUT_H, "w", encoding="utf-8", newline="\n").write("\n".join(L) + "\n")

    # ---------------- 自检：几个必须能打出来的词 ----------------
    wmap = dict(words_out)
    cmap_tab = dict(chars_out)
    probes = [("keting", "客厅"), ("woshi", "卧室"), ("deng", "灯"),
              ("ketingdeng", "客厅灯"), ("diaodeng", "吊灯"), ("chuanglian", "窗帘")]
    ok = True
    for py, want in probes:
        pool = wmap.get(py, "") + cmap_tab.get(py, "")
        hit = all(c in pool for c in want)
        ok = ok and hit
        print("   自检 %-12s -> %-16s %s"
              % (py, pool[:16] or "（空）", "OK" if hit else "!! 缺 " + want))
    print("拼音表：词组 %d 条 / 单字 %d 条（可显示汉字 %d 个），已写 %s（%d 字节）"
          % (len(words_out), len(chars_out),
             len([c for c in cmap if 0x4E00 <= c <= 0x9FFF]), OUT_H,
             os.path.getsize(OUT_H)))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
