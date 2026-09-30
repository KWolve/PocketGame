#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hls_filter.py - 从公开 IPTV 列表里筛出"本板能播"的频道，生成 resources/iptv/channels.m3u

为什么需要它：
  本板硬解上限 **960x544（约 52 万像素）**，720p/1080p 会直接把解码通道打爆
  （见 PgStream.cpp 的 kMaxDecodePixels）。而公开 IPTV 列表里绝大多数频道是
  720p/1080p —— 直接抄一份 m3u 进来，用户点十个有九个播不了。
  所以必须在**入库前**把分辨率筛一遍。

它做什么（每个频道两步，任一步失败就丢掉）：
  ① 拉 m3u8。若是 master playlist（多档），取它**最低码率的那一档**，
     读该档声明的 RESOLUTION；没有 RESOLUTION 就用带宽粗估。
  ② 真去下**一个 ts 分片**，确认 HTTP 通、且头字节是 TS 同步字 0x47 ——
     避免把"域名能解析但拿不到流"的僵尸源收进来。

用法：
    python tools/hls_filter.py                        # 默认拉 iptv-org 中国频道
    python tools/hls_filter.py -i my.m3u -o out.m3u
    python tools/hls_filter.py --max-pixels 520000     # 放宽/收紧分辨率上限
    python tools/hls_filter.py --keep-hd               # 只筛连通性，不卡分辨率

⚠️ 结果只是"在**跑这个脚本的机器**上当时可用"，源随时会失效。
   所以设备端支持 `/data/iptv.m3u` 覆盖内置表（用户自己更新，不用重刷固件）。

⚠️ 2026-09-14 起**优先用 `tools/iptv_probe.py`**（同一目录）：本脚本只看清单里声明的
   RESOLUTION，**不看编码、不看帧率**，而本板只硬解 H264、帧率要 ≤30 —— 那两条它查不了。
   iptv_probe.py 是"真拉一个分片、自己解 TS/SPS"，宽高与帧率都是实测值。
   本脚本保留作快速连通性预筛。
"""
import argparse
import concurrent.futures as cf
import os
import re
import socket
import sys
import urllib.request
from urllib.parse import urljoin

UA = {
    "User-Agent": "Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36 "
                  "(KHTML, like Gecko) Chrome/120 Mobile Safari/537.36"
}
DEFAULT_SRC = "https://iptv-org.github.io/iptv/countries/cn.m3u"


def http_get(url, limit=200000, timeout=9):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return f.read(limit), f.geturl()


def parse_m3u(text):
    """-> [(name, group, url)]"""
    out, name, group = [], None, ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF"):
            attrs = line
            m = re.search(r'group-title="([^"]*)"', attrs)
            group = m.group(1) if m else ""
            name = line.split(",", 1)[-1].strip() if "," in line else ""
        elif line and not line.startswith("#"):
            if name is not None:
                out.append((name, group, line))
            name = None
    return out


def pick_lowest_variant(text, base):
    """master playlist -> (最低档 url, 分辨率字符串, 带宽)"""
    best = None
    lines = [l.strip() for l in text.splitlines()]
    for i, l in enumerate(lines):
        if not l.startswith("#EXT-X-STREAM-INF:"):
            continue
        attrs = l[len("#EXT-X-STREAM-INF:"):]
        bw_m = re.search(r"BANDWIDTH=(\d+)", attrs)
        rs_m = re.search(r"RESOLUTION=(\d+x\d+)", attrs)
        bw = int(bw_m.group(1)) if bw_m else 0
        res = rs_m.group(1) if rs_m else ""
        url = ""
        for j in range(i + 1, len(lines)):
            if lines[j] and not lines[j].startswith("#"):
                url = urljoin(base, lines[j])
                break
        if not url:
            continue
        if best is None or (bw and bw < best[2]) or (not best[2] and bw):
            best = (url, res, bw)
    return best


def res_pixels(res):
    m = re.match(r"(\d+)x(\d+)", res or "")
    return int(m.group(1)) * int(m.group(2)) if m else 0


def res_from_name(name):
    """iptv-org 的频道名常带 "(720p)" / "(1080p)" / "(576i)" 标注。
    media playlist（单档清单）拿不到 RESOLUTION，只能靠这个先筛一道 ——
    虽然不如实际探测准，但能把绝大多数高清源挡在表外（本板硬解上限 960x544）。"""
    m = re.search(r"\((\d{3,4})[pi]\)", name or "")
    if not m:
        return 0, ""
    h = int(m.group(1))
    if h <= 0:
        return 0, ""
    # 按 16:9 粗估像素（够用来和上限比较）
    return int(h * 16 / 9) * h, "%dx?（按名称 %dp 估）" % (int(h * 16 / 9), h)


def check_channel(item, max_pixels, timeout):
    name, group, url = item
    try:
        body, final = http_get(url, 400000, timeout)
        text = body.decode("utf-8", "replace")
        if "#EXTM3U" not in text:
            return None, "不是 m3u8"

        res, variant = "", ""
        if "#EXT-X-STREAM-INF" in text:
            best = pick_lowest_variant(text, final)
            if not best:
                return None, "master 无档位"
            vurl, res, bw = best
            variant = "%s bw=%d" % (res or "?", bw)
            body2, final2 = http_get(vurl, 400000, timeout)
            text = body2.decode("utf-8", "replace")
            final = final2

        if "#EXT-X-KEY" in text and "METHOD=NONE" not in text:
            return None, "AES-128 加密"
        if "#EXT-X-MAP" in text:
            return None, "fMP4 分片"

        # 分辨率门槛（只有 master 才拿得到精确值；media playlist 靠名称标注估）
        px = res_pixels(res)
        verified = bool(px)
        if not px:
            px, res = res_from_name(name)
        if px and px > max_pixels:
            return None, "分辨率 %s 超上限" % res
        if not px:
            variant = variant or "分辨率未知"

        segs = re.findall(r"^(?!#)(\S+\.ts[^\s]*)$", text, re.M)
        if not segs:
            m = re.findall(r"^(?!#)(\S+\.(?:ts|m4s|aac)[^\s]*)$", text, re.M)
            if not m:
                return None, "清单里没有分片"
            segs = m

        # 真下一片，确认是 TS（同步字 0x47）
        seg = urljoin(final, segs[-1])
        data, _ = http_get(seg, 4096, timeout)
        if not data:
            return None, "分片为空"
        ts_ok = data[0] == 0x47
        if not ts_ok and not data[0:1] == b"\x47":
            # 少数源分片不是 TS（HLS 允许打包成 AAC 等），保守起见只认 TS
            return None, "分片不是 TS（首字节 0x%02x）" % data[0]

        return {
            "name": name, "group": group, "url": url,
            "variant": variant, "seg": seg,
            "verified": verified, "res": res,
        }, "OK"
    except Exception as e:
        return None, "%s: %s" % (type(e).__name__, str(e)[:40])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default=DEFAULT_SRC)
    ap.add_argument("-o", "--output", default="resources/iptv/channels.m3u")
    ap.add_argument("--max-pixels", type=int, default=960 * 544,
                    help="最低档分辨率上限（默认 960x544 = 本板硬解上限）")
    ap.add_argument("--keep-hd", action="store_true", help="不卡分辨率（只筛连通性）")
    ap.add_argument("--timeout", type=int, default=9)
    ap.add_argument("-j", "--jobs", type=int, default=32)
    ap.add_argument("--limit", type=int, default=0, help="只测前 N 个（调试用）")
    args = ap.parse_args()

    src = args.input
    if os.path.isfile(src):
        with open(src, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        text = http_get(src, 20 << 20, args.timeout)[0].decode("utf-8", "replace")

    items = parse_m3u(text)
    if args.limit:
        items = items[:args.limit]
    print("候选频道: %d" % len(items))

    max_px = 1 << 60 if args.keep_hd else args.max_pixels
    ok, why = [], {}
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        futs = {ex.submit(check_channel, it, max_px, args.timeout): it for it in items}
        for fu in cf.as_completed(futs):
            r, msg = fu.result()
            if r:
                ok.append(r)
            else:
                why[msg.split(":")[0]] = why.get(msg.split(":")[0], 0) + 1

    ok.sort(key=lambda x: (not x["verified"], x["group"], x["name"]))
    ver = [c for c in ok if c["verified"]]
    print("\n可用: %d / %d（其中分辨率已确认 %d 个）" % (len(ok), len(items), len(ver)))
    print("淘汰统计: %s" % why)
    for c in ok:
        print("  %s [%s] %-32s %s" % ("✓" if c["verified"] else "?", c["group"][:8],
                                      c["name"][:32], c["variant"]))

    out = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write("#EXTM3U\n")
        f.write("# IPTV 频道表（由 tools/hls_filter.py 自动筛选生成）\n")
        f.write("# 筛选条件：分辨率 <= %d 像素（本板硬解上限 960x544；%s）\n"
                % (args.max_pixels, "已关闭分辨率筛选" if args.keep_hd else "超出会打爆解码通道"))
        f.write("# 分组带「·可播」= 分辨率已确认；「·未验证」= 只有单档清单、分辨率未知，播不出来就跳过\n")
        f.write("# ⚠️ 源会失效；设备上放 /data/iptv.m3u 可覆盖本表（无需重刷固件）\n")
        for c in ok:
            tag = "·可播" if c["verified"] else "·未验证"
            f.write('#EXTINF:-1 group-title="%s%s" tvg-name="%s",%s\n'
                    % (c["group"], tag, c["name"], c["name"]))
            f.write("%s\n" % c["url"])
    print("\n已写出: %s（%d 个频道）" % (out, len(ok)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
