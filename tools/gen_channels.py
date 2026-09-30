#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_channels.py - 把 iptv_probe.py 探测出来的原始频道表**规范化**成标准格式（带中文）

为什么要这一层（而不是直接手改 channels.m3u）：
    `tools/iptv_probe.py` 是**实测**生成器，它只认码流，不认识中文，产出的表长这样：
        # 实测 640x360@30.00fps VUI 1120kbps
        #EXTINF:-1 group-title="国际·可播" tvg-name="CGTN (1080p)",CGTN (1080p)
    直接拿它当正式表有三个问题（用户报"描述很乱"）：
      ① 名字是英文 + 分辨率括号 + `[Geo-blocked]` 这类标记，一屏字看不懂；
      ② 同一个频道有多个源（CGTN 就有 4 个），列表里**重复出现**，用户以为卡了；
      ③ 分组是"国际·可播"这种带内部后缀的，且没有频道号。

本脚本做的事（全部可重跑、幂等）：
      ① 名字：拆出 `[...]` 标记 → 备注字段；去掉 `(...)` 分辨率后缀；
              查 `NAME_MAP` 换成中文名（未命中就保留原名并打印提示，方便补映射）；
      ② 去重：同一个中文名只留**一条**源（默认取像素数最大的；`PREFER_URL` 可显式指定）；
      ③ 排序：按分组（央视/地方/国际/新闻/影视/其他）→ 组内按像素数降序；
      ④ 输出标准 `#EXTINF`：`tvg-id` / `tvg-name` / `tvg-chno` / `group-title` / `tvg-note`
              + 逗号后跟**中文名**（设备端解析器读的就是"最后一个逗号之后"那截，
              见 src/logic/iptvLogic.cc 的 iptvLoadChannels）；
              + 保留 `# 实测 <宽x高@帧率> · <码率>` 注释行（列表副行显示它）。

用法：
    # 探测（PC 侧实测码流，慢）→ 规范化（就地改写）
    python tools/iptv_probe.py -i cand.m3u -o resources/iptv/channels.m3u
    python tools/gen_channels.py
    # 指定输入/输出（比如拿旧表试跑）
    python tools/gen_channels.py -i /tmp/raw.m3u -o /tmp/out.m3u
"""
import argparse
import io
import os
import re
import sys

# ---------------------------------------------------------------- 中文名映射
# 左边 = 去掉 `(...)` 分辨率后缀与 `[...]` 标记后的**原始名**（探测表里的写法）
# 右边 = 列表里显示的中文名
NAME_MAP = {
    "Chifeng Comprehensive News Chanel": "赤峰新闻综合",
    "Jilin City Channel": "吉林市新闻综合",
    "Jilin Lifestyle Channel": "吉林生活",
    "Jilin Movie Channel": "吉林影视",
    "Jilin Rural Channel": "吉林乡村",
    "Kangba TV": "康巴卫视",
    "Lanzhou Comprehensive News Channel": "兰州新闻综合",
    "Lanzhou Culture & Tourism Channel": "兰州文旅",
    "TV BRICS Chinese": "金砖电视中文台",
    "Tonghua TV": "通化新闻",
    "Xinjiang TV 3": "新疆电视台 3",
    "Zhejiang TV International": "浙江国际",
    # ---- 央视 ----
    "CCTV-1": "CCTV-1 综合",
    "CCTV-3": "CCTV-3 综艺",
    "CCTV-10": "CCTV-10 科教",
    "CCTV-12": "CCTV-12 社会与法",
    "CCTV+ 1": "CCTV+ 1 新闻",
    "CCTV+ 2": "CCTV+ 2 新闻",
    # ---- 国际 ----
    "CGTN": "CGTN 英语",
    "CGTN Documentary": "CGTN 纪录",
    "CGTN Global Biz": "CGTN 财经",
    "China Travel": "中国旅游",
    "Discovering China": "发现中国",
    "Dragon TV International": "东方卫视国际",
    "VOA美国之音": "VOA 美国之音",
    "Home Plus": "Home Plus 影视",
    "Angel TV Chinese": "Angel TV 中文台",
    # ---- 地方 ----
    "Chuxiong News Channel": "楚雄新闻综合",
    "Hebei TV": "河北卫视",
    "Jiangsu Public & News Channel": "江苏公共·新闻",
    "Siping TV": "四平新闻综合",          # 与"四平新闻综合"同频道不同源 → 合并
    "Xinjiang TV 2": "新疆电视台 2",
    "浙江 Ⅰ 绍兴影视": "绍兴影视",         # 去掉"浙江 Ⅰ "这种来源前缀
    "浙江 Ⅰ 绍兴综合": "绍兴综合",
    "蕭山生活頻道": "萧山生活",            # 繁体 → 简体
    "靖江新闻綜合": "靖江新闻综合",
    # 其余本来就是中文的（余姚姚江文化 / 嵊州综合 / 武进新闻 …）不用映射，原样保留
}

# 同频道多源时的取舍：默认"像素数最大"，这里可显式钉住某条（按 url 子串匹配）
PREFER_URL = {
    "CGTN 英语": "amg00405",     # 4 个源里选 Rakuten 官方 CDN（360p@30，实测能出画）
    "CGTN 纪录": "amg00405",     # 2 个源里同理
}

# 分组：探测表里是 `国际·可播` 这种，取 `·` 之前那段当中文分组名
GROUP_ORDER = ["央视", "地方", "国际", "新闻", "影视", "其他"]

# `[...]` 标记 → 中文备注（列表里会显示，提醒用户"这个源有时打不开"）
NOTE_MAP = {
    "Geo-blocked": "仅限国内",
    "Not 24/7": "非全天播出",
}

# ---------------------------------------------------------------- 解析 / 规范化

# 实测行有两种写法，都要能吃：
#   探测表：`# 实测 640x360@30.00fps VUI 1120kbps`
#   本脚本输出：`# 实测 640x360@30.00fps · 1120kbps`
# ⚠️ 只认一种的话，脚本重跑一次就把实测数据洗没了（踩过）。
RE_MEASURE = re.compile(r"实测\s+(\S+?)[\s·]+(?:(?:VUI|估\(\w+\))[\s·]*)?(\d+kbps)")
RE_TAG = re.compile(r"\[([^\]]+)\]")
RE_PAREN = re.compile(r"\s*\(([^)]*)\)\s*$")
RE_GROUP = re.compile(r'group-title="([^"]*)"')
# ⚠️ 也要认 `tvg-note="..."`：脚本的输出里带着它，输入解析不认的话，
#    **重跑一次备注就被洗掉了**（2026-09-14 实测踩到：列表里"非全天播出"整列消失）。
RE_NOTE = re.compile(r'tvg-note="([^"]*)"')


def parse_raw(path):
    """读探测表 → [{name, group, url, size, fps, kbps, note}]"""
    rows, note_txt = [], ""
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith("#"):
                if s.startswith("#EXTINF"):
                    g = RE_GROUP.search(s)
                    raw_name = s.rsplit(",", 1)[-1].strip()
                    m = RE_MEASURE.search(note_txt)
                    nt = RE_NOTE.search(s)
                    rows.append({
                        "raw": raw_name,
                        "group": (g.group(1) if g else "").split("·")[0] or "其他",
                        "size": m.group(1) if m else "",
                        "kbps": m.group(2) if m else "",
                        "note": nt.group(1) if nt else "",
                        "url": "",
                    })
                    note_txt = ""
                else:
                    m = RE_MEASURE.search(s)
                    if m:
                        note_txt = s
                continue
            if rows and not rows[-1]["url"]:
                rows[-1]["url"] = s
    return rows


def norm_name(raw):
    """原始名 → (中文名, 备注)。未命中映射表时保留原名并打印提示。"""
    note = []
    for tag in RE_TAG.findall(raw):
        note.append(NOTE_MAP.get(tag.strip(), tag.strip()))
    base = RE_TAG.sub("", raw).strip()
    base = RE_PAREN.sub("", base).strip()          # 去掉尾部 (720p) / (1080p) 这类
    base = re.sub(r"\s+", " ", base)
    cn = NAME_MAP.get(base)
    if cn is None:
        cn = base
        if re.search(r"[A-Za-z]", base):
            sys.stderr.write("!! 没有中文名映射，原样保留：%r（补进 NAME_MAP 即可）\n" % base)
    return cn, " · ".join(note)


def pixels(size):
    m = re.match(r"(\d+)x(\d+)", size or "")
    return int(m.group(1)) * int(m.group(2)) if m else 0


def build(rows):
    """规范化 + 去重 + 排序 → 输出行列表"""
    items = []
    for r in rows:
        cn, note = norm_name(r["raw"])
        if not note:
            note = r.get("note", "")   # 已经是规范化过的表（重跑场景）：沿用已有的备注
        r["cn"], r["note"] = cn, note
        items.append(r)

    # 去重：同一中文名只留一条
    best = {}
    for r in items:
        k = r["cn"]
        if k not in best:
            best[k] = r
            continue
        cur = best[k]
        pref = PREFER_URL.get(k)
        if pref:
            # 显式钉住的那条优先；两条都命中/都不命中时按像素数比
            if pref in r["url"] and pref not in cur["url"]:
                best[k] = r
                continue
            if pref in cur["url"]:
                continue
        if pixels(r["size"]) > pixels(cur["size"]):
            best[k] = r

    out = sorted(best.values(), key=lambda r: (
        GROUP_ORDER.index(r["group"]) if r["group"] in GROUP_ORDER else len(GROUP_ORDER),
        -pixels(r["size"]),
        r["cn"],
    ))
    return out


def slug(cn):
    """中文名 → tvg-id（英文/数字/连字符；纯中文的用拼音式占位没必要，直接用 remove 后）
    这里只做"稳定可读"：CCTV-1 综合 → cctv-1；CGTN 英语 → cgtn；
    中文名 → 用 `cn-<序号>` 形式（tvg-id 只作程序匹配用，不显示）。"""
    m = re.match(r"([A-Za-z0-9+\-\. ]+)", cn)
    if m:
        s = re.sub(r"[^a-z0-9]+", "-", m.group(1).strip().lower()).strip("-")
        if s:
            return s
    return ""


def emit(items):
    lines = []
    lines.append("#EXTM3U")
    lines.append("#")
    lines.append("# IPTV 频道表 —— **标准 m3u 格式 + 中文名**（列表里显示的就是逗号后那截名字）")
    lines.append("#")
    lines.append("# 字段说明：")
    lines.append('#   tvg-id      程序匹配用的英文标识（同名频道的稳定 key）')
    lines.append('#   tvg-name    频道名（与逗号后的显示名一致）')
    lines.append('#   tvg-chno    频道号（列表里的序号）')
    lines.append('#   group-title 分组：央视 / 地方 / 国际 / 新闻 / 影视 / 其他')
    lines.append('#   tvg-note    源的限制（"仅限国内" = Geo-blocked；"非全天播出" = Not 24/7）')
    lines.append("#")
    lines.append("# 上面那行 `# 实测 宽x高@帧率 · 码率` = 由 tools/iptv_probe.py 真拉码流解出来的实测值，")
    lines.append("#   列表副行会显示（本板硬解上限 720p，来源见 docs/iptv.md）。")
    lines.append("#")
    lines.append("# ⚠️ 本文件由 **tools/gen_channels.py 生成**，别手改（下次探测会被覆盖）。")
    lines.append("#    重新生成：python tools/iptv_probe.py -i cand.m3u -o resources/iptv/channels.m3u")
    lines.append("#              python tools/gen_channels.py")
    lines.append("#    临时换源（不改固件）：设备上放 /data/iptv.m3u 覆盖本表。")
    lines.append("#")

    group_now = None
    used_ids = {}
    for i, r in enumerate(items, 1):
        if r["group"] != group_now:
            group_now = r["group"]
            lines.append("")
            lines.append("# ==================== %s ====================" % group_now)
        lines.append("# 实测 %s · %s" % (r["size"] or "未测", r["kbps"] or "未测"))
        sid = slug(r["cn"]) or ("ch%02d" % i)
        # tvg-id 必须唯一（CCTV-1 综合 与 CCTV+ 1 新闻 会都推出 `cctv-1`）
        if sid in used_ids:
            used_ids[sid] += 1
            sid = "%s-%d" % (sid, used_ids[sid])
        else:
            used_ids[sid] = 1
        fields = [
            'tvg-id="%s"' % sid,
            'tvg-name="%s"' % r["cn"],
            'tvg-chno="%d"' % i,
            'group-title="%s"' % r["group"],
        ]
        if r["note"]:
            fields.append('tvg-note="%s"' % r["note"])
        lines.append("#EXTINF:-1 %s,%s" % (" ".join(fields), r["cn"]))
        lines.append(r["url"])
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("-i", "--in", dest="src",
                    default=os.path.join(here, "resources", "iptv", "channels.m3u"))
    ap.add_argument("-o", "--out", dest="dst", default=None,
                    help="默认就地覆盖 -i")
    args = ap.parse_args()
    dst = args.dst or args.src

    rows = parse_raw(args.src)
    if not rows:
        sys.stderr.write("没有解析到任何频道（%s）\n" % args.src)
        return 1
    items = build(rows)
    text = emit(items)
    with io.open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("%s: %d 条 → 规范化后 %d 条（去重 %d）" %
          (os.path.basename(args.src), len(rows), len(items), len(rows) - len(items)))
    for r in items:
        print("   %2d  %-16s %-12s %s" %
              (items.index(r) + 1, r["cn"], r["group"], r["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
