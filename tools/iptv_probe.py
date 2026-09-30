#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
iptv_probe.py - 在 **PC 侧** 实测 IPTV 源能不能播、多大分辨率、多少帧率（不依赖 ffmpeg）

为什么单独写这个（而不是用 hls_filter.py）：
  hls_filter.py 只看 m3u8 里声明的 RESOLUTION（media playlist 常常根本没有），
  也**不看帧率、不看编码**。而本板：
    · 只硬解 **H264**（HEVC/H265 一律解不了，zk_h264_player 与 MPP 都只认 H264）；
    · 硬解上限定在 **720p**（>960x544 的走 zk_h264_player 1/2 缩放，见 PgStream.cpp:963）；
    · 帧率要 ≤30（源大多是 25/30，>30 的在 480x800 上没意义还更吃内存）。
  所以入库前必须**真去拉一段码流**，自己解出 编码/宽/高/帧率。

怎么做到"不用 ffmpeg"：
  ① 拉 m3u8 → master 挑最低码率档 → media 清单拿分片 URL；
  ② 真下最后一个分片（限 1.5MB），首字节必须是 TS 同步字 0x47；
  ③ 自己解 TS：PAT → PMT → 视频 PID + **stream_type**（0x1B=H264 / 0x24=HEVC）；
  ④ 从 PES 里攒出 H264 ES，找 SPS（NAL 0x67），按 H.264 规范解出
     宽/高（含 frame_cropping）与 VUI 里的 num_units_in_tick/time_scale → **精确帧率**；
  ⑤ VUI 没写 timing 的源，退化成"数 AUD(9)/首片(1|5, first_mb=0) 的个数 ÷ 分片时长"估帧率。

用法：
    python tools/iptv_probe.py                                   # 默认探 iptv-org 中国频道
    python tools/iptv_probe.py -i cand.m3u -o resources/iptv/channels.m3u
    python tools/iptv_probe.py --max-h 720 --max-fps 30 --jobs 32
    python tools/iptv_probe.py --report out.md                    # 写一份可读的实测报告
"""
import argparse
import concurrent.futures as cf
import os
import re
import socket
import sys
import time
import urllib.request
from urllib.parse import urljoin

DEFAULT_SRC = ("https://raw.githubusercontent.com/iptv-org/iptv/master/streams/cn.m3u"
               "|https://iptv-org.github.io/iptv/countries/cn.m3u"
               "|https://cdn.jsdelivr.net/gh/iptv-org/iptv@master/streams/cn.m3u")
# ⚠️ 必须与设备端 `PgHls.cpp` 的 kUserAgent **完全一致**：很多源按 UA 放行/403，
#    改了 UA 就可能"PC 通、设备不通"（或反过来），那样这次实测就白做了。
UA = {
    "User-Agent": "Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36 "
                  "(KHTML, like Gecko) Chrome/120 Mobile",
    "Accept": "*/*",
}
TS_PKT = 188


# ---------------------------------------------------------------- HTTP

def http_get(url, limit=200000, timeout=8):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as f:
        return f.read(limit)


# ---------------------------------------------------------------- m3u8

def parse_m3u(text):
    """-> [(name, group, url)]"""
    out, name, group = [], None, ""
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXTINF"):
            m = re.search(r'group-title="([^"]*)"', line)
            group = m.group(1) if m else ""
            name = line.split(",", 1)[-1].strip() if "," in line else ""
        elif line and not line.startswith("#"):
            if name is not None:
                out.append((name, group, line))
            name = None
    return out


def pick_lowest_variant(text, base):
    lines = [l.strip() for l in text.splitlines()]
    best = None
    for i, l in enumerate(lines):
        if not l.startswith("#EXT-X-STREAM-INF:"):
            continue
        attrs = l[len("#EXT-X-STREAM-INF:"):]
        bw = int(re.search(r"BANDWIDTH=(\d+)", attrs).group(1)) if "BANDWIDTH=" in attrs else 0
        url = ""
        for j in range(i + 1, len(lines)):
            if lines[j] and not lines[j].startswith("#"):
                url = urljoin(base, lines[j])
                break
        if not url:
            continue
        if best is None or (bw and bw < best[1]) or (not best[1] and bw):
            best = (url, bw)
    return best


def resolve_media_playlist(url, timeout):
    """-> (media_playlist_text, final_base, variant_bw, variant_url)"""
    body = http_get(url, 400000, timeout)
    text = body.decode("utf-8", "replace")
    if "#EXTM3U" not in text:
        raise ValueError("不是 m3u8")
    final, bw, vurl = url, 0, url
    if "#EXT-X-STREAM-INF" in text:
        best = pick_lowest_variant(text, url)
        if not best:
            raise ValueError("master 无档位")
        vurl, bw = best
        text = http_get(vurl, 400000, timeout).decode("utf-8", "replace")
        final = vurl
    return text, final, bw, vurl


# ---------------------------------------------------------------- bit reader

class Bits:
    def __init__(self, data):
        self.d = data
        self.pos = 0

    def u(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.pos >> 3] if (self.pos >> 3) < len(self.d) else 0
            v = (v << 1) | ((byte >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0 and z < 32:
            z += 1
        return (1 << z) - 1 + (self.u(z) if z else 0)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)


def unescape_rbsp(b):
    """去掉 emulation prevention 字节 00 00 03 -> 00 00"""
    out, i = bytearray(), 0
    while i < len(b):
        if i + 2 < len(b) and b[i] == 0 and b[i + 1] == 0 and b[i + 2] == 3:
            out += b[i:i + 2]
            i += 3
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def parse_sps(nal):
    """nal = 以 0x67 开头的 SPS NAL（含 header）。-> dict(w,h,fps,profile,level) 或 None"""
    try:
        rbsp = unescape_rbsp(nal[1:])
        br = Bits(rbsp)
        profile = br.u(8)
        br.u(8)                      # constraint flags
        level = br.u(8)
        br.ue()                      # seq_parameter_set_id
        chroma = 1
        if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
            chroma = br.ue()
            if chroma == 3:
                br.u(1)
            br.ue(); br.ue()         # bit_depth_luma/chroma_minus8
            br.u(1)                  # qpprime
            if br.u(1):              # seq_scaling_matrix_present
                n = 8 if chroma != 3 else 12
                for i in range(n):
                    if br.u(1):
                        size = 16 if i < 6 else 64
                        last, nextv = 8, 8
                        for _ in range(size):
                            if nextv:
                                nextv = (last + br.se() + 256) % 256
                            last = nextv if nextv else last
        br.ue()                      # log2_max_frame_num_minus4
        poc = br.ue()
        if poc == 0:
            br.ue()
        elif poc == 1:
            br.u(1); br.se(); br.se()
            for _ in range(br.ue()):
                br.se()
        br.ue()                      # max_num_ref_frames
        br.u(1)                      # gaps_in_frame_num
        w_mbs = br.ue() + 1
        h_map = br.ue() + 1
        frame_only = br.u(1)
        if not frame_only:
            br.u(1)                  # mb_adaptive_frame_field
        br.u(1)                      # direct_8x8
        w, h = w_mbs * 16, h_map * 16 * (2 if not frame_only else 1)
        if br.u(1):                  # frame_cropping_flag
            l, r, t, b = br.ue(), br.ue(), br.ue(), br.ue()
            sx = 1
            if chroma == 0:
                sx = 1
            elif chroma == 1:
                sx = 2
            elif chroma == 2:
                sx = 2
            elif chroma == 3:
                sx = 1
            # SubWidthC/SubHeightC: mono=1/1, 420=2/2, 422=2/1, 444=1/1
            subw, subh = (1, 1) if chroma == 0 else ((2, 2) if chroma == 1 else ((2, 1) if chroma == 2 else (1, 1)))
            hh = h_map * 16 * (2 if not frame_only else 1)
            crop_unit_x = subw
            crop_unit_y = subh * (2 if not frame_only else 1)
            w -= (l + r) * crop_unit_x
            h = hh - (t + b) * crop_unit_y
        fps = 0.0
        if br.u(1):                  # vui_parameters_present_flag
            if br.u(1):              # aspect_ratio_info
                if br.u(8) == 255:
                    br.u(16); br.u(16)
            if br.u(1):
                br.u(1)              # overscan_appropriate
            if br.u(1):              # video_signal_type
                br.u(3); br.u(1)
                if br.u(1):
                    br.u(8); br.u(8); br.u(8)
            if br.u(1):              # chroma_loc_info
                br.ue(); br.ue()
            if br.u(1):              # timing_info
                tick = br.u(32)
                scale = br.u(32)
                br.u(1)              # fixed_frame_rate
                if tick:
                    fps = scale / (2.0 * tick)
        return {"w": w, "h": h, "fps": fps, "profile": profile, "level": level}
    except Exception:
        return None


# ---------------------------------------------------------------- TS

def _section(p, pay):
    """从 PUSI 包取出 PSI section（**先跳 pointer_field**，血的教训：
    少跳这 1 字节会让 table_id/seclen 全体错位，症状是"PAT 解不出 PMT PID、
    PMT 里找不到视频流"，而分片本身是好 TS —— 极易误判成源坏了）。"""
    s = p[pay:]
    if not s:
        return None
    ptr = s[0]
    if 1 + ptr + 4 > len(s):
        return None
    return s[1 + ptr:]


def ts_streams(data):
    """解 PAT/PMT，返回 (video_pid, stream_type) 或 (None, None)

    ⚠️ 两个坑（都踩过，症状都是"PMT 里找不到视频流"、而分片其实是好 TS）：
      ① PSI section 前面有 pointer_field，必须先跳过再读 table_id；
      ② section_length 里**含末尾 4 字节 CRC** —— program/ES 循环要在 `3+seclen-4`
         处收尾。不收的话 CRC 会被当成一条 program 表项，把刚解出的 PMT PID 覆盖掉。
    另外 PAT/PMT 在分片里的先后顺序不保证，所以先扫一遍 PAT、再扫一遍 PMT。"""
    pmt_pid = None
    for off in range(0, len(data) - TS_PKT + 1, TS_PKT):
        p = data[off:off + TS_PKT]
        if p[0] != 0x47:
            continue
        if ((p[1] & 0x1F) << 8) | p[2] != 0 or not (p[1] & 0x40):
            continue
        pay = 4 + (1 + p[4] if (p[3] >> 4) & 0x2 else 0)
        if ((p[3] >> 4) & 0x1) == 0 or pay >= TS_PKT:
            continue
        sec = _section(p, pay)
        if sec is None or len(sec) < 8 or sec[0] != 0x00:
            continue
        seclen = ((sec[1] & 0x0F) << 8) | sec[2]
        body = sec[8:3 + seclen - 4]
        for i in range(0, len(body) - 3, 4):
            prog = (body[i] << 8) | body[i + 1]
            ppid = ((body[i + 2] & 0x1F) << 8) | body[i + 3]
            if prog != 0:
                pmt_pid = ppid
                break
        if pmt_pid is not None:
            break
    if pmt_pid is None:
        return None, None

    for off in range(0, len(data) - TS_PKT + 1, TS_PKT):
        p = data[off:off + TS_PKT]
        if p[0] != 0x47 or (((p[1] & 0x1F) << 8) | p[2]) != pmt_pid or not (p[1] & 0x40):
            continue
        pay = 4 + (1 + p[4] if (p[3] >> 4) & 0x2 else 0)
        if ((p[3] >> 4) & 0x1) == 0 or pay >= TS_PKT:
            continue
        sec = _section(p, pay)
        if sec is None or len(sec) < 12 or sec[0] != 0x02:
            continue
        seclen = ((sec[1] & 0x0F) << 8) | sec[2]
        pil = ((sec[10] & 0x0F) << 8) | sec[11]
        i = 12 + pil
        end = min(3 + seclen, len(sec)) - 4
        while i + 4 < end:
            st = sec[i]
            epid = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2]
            eil = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4]
            if st in (0x1B, 0x24, 0x02, 0x10):
                return epid, st
            i += 5 + eil
        break
    return None, None


def extract_es(data, vpid):
    """拼出视频 PID 的 PES 负载（ES 字节）"""
    out = bytearray()
    for off in range(0, len(data) - TS_PKT + 1, TS_PKT):
        p = data[off:off + TS_PKT]
        if p[0] != 0x47:
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        if pid != vpid:
            continue
        afc = (p[3] >> 4) & 0x3
        pay = 4
        if afc & 0x2:
            pay += 1 + p[4]
        if afc & 0x1 == 0 or pay >= TS_PKT:
            continue
        if p[1] & 0x40:   # PUSI：跳过 PES 头
            s = p[pay:]
            if len(s) > 9 and s[0] == 0 and s[1] == 0 and s[2] == 1:
                hdr = 9 + s[8]
                s = s[hdr:]
            out += s
        else:
            out += p[pay:]
    return bytes(out)


def scan_es(es):
    """扫 H264 NAL：-> dict(sps, aud, frames)"""
    res = {"sps": None, "aud": 0, "frames": 0, "nal": {}}
    i = 0
    n = len(es)
    while True:
        j = es.find(b"\x00\x00\x01", i)
        if j < 0 or j + 3 >= n:
            break
        t = es[j + 3] & 0x1F
        if (es[j + 3] & 0x80) != 0:   # forbidden bit：多半是伪同步
            i = j + 3
            continue
        k = es.find(b"\x00\x00\x01", j + 3)
        nal = es[j + 3: k if k > 0 else n]
        res["nal"][t] = res["nal"].get(t, 0) + 1
        if t == 7 and res["sps"] is None:
            res["sps"] = nal
        elif t == 9:
            res["aud"] += 1
        elif t in (1, 5):
            try:
                br = Bits(unescape_rbsp(nal[1:]))
                if br.ue() == 0:
                    res["frames"] += 1
            except Exception:
                pass
        i = j + 3
    return res


# ---------------------------------------------------------------- 单条探测

def probe_one(item, max_w, max_h, max_fps, timeout):
    name, group, url = item
    t0 = time.time()
    r = {"name": name, "group": group, "url": url, "ok": False, "why": "",
         "w": 0, "h": 0, "fps": 0.0, "codec": "", "variant": "", "kbps": 0, "t": 0.0}
    try:
        text, final, bw, vurl = resolve_media_playlist(url, timeout)
        if bw:
            r["variant"] = "bw=%d" % bw

        if "#EXT-X-KEY" in text and "METHOD=NONE" not in text:
            r["why"] = "AES-128 加密"
            return r
        if "#EXT-X-MAP" in text:
            r["why"] = "fMP4 分片（非 TS）"
            return r

        # 拿分片（最后两个，后一个没 SPS 就用前一个）
        ent = re.findall(r"#EXTINF:([\d.]+)[^\n]*\n([^#\n][^\n]*)", text)
        if not ent:
            segs = re.findall(r"^(?!#)(\S+)$", text, re.M)
            ent = [(0, s) for s in segs]
        if not ent:
            r["why"] = "清单里没有分片"
            return r
        dur, seg = float(ent[-1][0] or 0), ent[-1][1].strip()
        segurl = urljoin(final, seg)

        data = http_get(segurl, 1500000, timeout)
        if not data:
            r["why"] = "分片为空"
            return r
        if data[0] != 0x47:
            r["why"] = "分片首字节 0x%02X（不是 TS）" % data[0]
            return r
        if dur:
            r["kbps"] = int(len(data) * 8 / dur / 1000)

        vpid, st = ts_streams(data)
        if vpid is None:
            r["why"] = "PMT 里没找到视频流"
            return r
        r["codec"] = {0x1B: "H264", 0x24: "HEVC", 0x02: "MPEG2", 0x10: "MPEG4"}.get(st, "0x%X" % st)
        if st != 0x1B:
            r["why"] = "编码 %s（本板只硬解 H264）" % r["codec"]
            return r

        info = None
        for cand in ([data] + ([http_get(urljoin(final, ent[-2][1].strip()), 1500000, timeout)]
                               if len(ent) >= 2 else [])):
            es = extract_es(cand, vpid)
            sc = scan_es(es)
            if sc["sps"]:
                info = parse_sps(sc["sps"])
                aud, frames = sc["aud"], sc["frames"]
                break
        if not info:
            r["why"] = "这一片里没有 SPS（无法判定分辨率）"
            return r

        r["w"], r["h"] = info["w"], info["h"]
        fps = info["fps"]
        src = "VUI"
        if not fps:                       # VUI 没写 timing → 靠数帧估
            cnt = aud or frames
            if cnt and dur:
                fps = cnt / dur
                src = "估(AUD%d)" % aud if aud else "估(首片%d)" % frames
        r["fps"] = round(fps, 3) if fps else 0
        r["fps_src"] = src

        if max_w and r["w"] > max_w:
            r["why"] = "宽 %d > %d" % (r["w"], max_w)
            return r
        if max_h and r["h"] > max_h:
            r["why"] = "高 %d > %d" % (r["h"], max_h)
            return r
        if max_fps and fps and fps > max_fps + 0.6:
            r["why"] = "帧率 %.2f > %d" % (fps, max_fps)
            return r
        if not fps:
            r["why"] = "帧率无法判定（不满足 ≤%dfps）" % max_fps
            return r

        r["ok"] = True
        r["why"] = "OK"
    except Exception as e:
        r["why"] = "%s: %s" % (type(e).__name__, str(e)[:48])
    finally:
        r["t"] = round(time.time() - t0, 1)
    return r


# ---------------------------------------------------------------- main

def load_src(src, timeout, tries=3):
    """拉候选清单。`src` 可以是本地文件，或 `URL1|URL2|...`（依次退避重试）。

    ⚠️ GitHub raw 在国内时通时不通（实测同一次会话里 0.5s 成功、几分钟后 8s 超时），
    所以这里必须**多源 + 重试**，否则整个探测会死在第一步。"""
    if os.path.isfile(src):
        with open(src, encoding="utf-8", errors="replace") as f:
            return f.read()
    last = None
    for url in src.split("|"):
        for i in range(tries):
            try:
                return http_get(url, 20 << 20, max(timeout, 20)).decode("utf-8", "replace")
            except Exception as e:
                last = e
                time.sleep(1.0 + i)
    raise last


# 分组归类：iptv-org 的 streams/cn.m3u **不带 group-title**，直接抄会全落到"未分组"，
# 选台页就没法按类找台。这几条规则够用（命中不了的一律"地方"）。
GROUP_RULES = [
    (re.compile(r"^CCTV[- ]?\d"), "央视"),
    (re.compile(r"^(CGTN|VOA|Angel TV|Home Plus|Dragon TV International|"
                r"Discovering China|China Travel)"), "国际"),
    (re.compile(r"卫视"), "卫视"),
]


# 旧表里的英文分组 → 中文（选台页统一显示中文）
GROUP_ALIAS = {
    "News": "新闻", "Movies": "影视", "Religious": "其他",
    "Documentary": "纪实", "Culture": "文化", "General": "综合",
    "Sports": "体育", "Kids": "少儿", "Music": "音乐", "Undefined": "其他",
}


def classify(name, group):
    """给出的分组一律规整成中文短名；没有就按频道名归类。"""
    for tag in ("·可播", "·未验证"):
        if group and tag in group:
            group = group.split(tag)[0]
    group = GROUP_ALIAS.get((group or "").split(";")[0], group)
    if group:
        return group
    for rx, g in GROUP_RULES:
        if rx.search(name or ""):
            return g
    return "地方"


def write_outputs(args, ok, bad, n_all):
    out = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write("#EXTM3U\n")
        f.write("# IPTV 频道表（由 tools/iptv_probe.py 在 PC 侧逐条**实测码流**生成）\n")
        f.write("# 筛选条件：H264 编码；宽<=%d 高<=%d；帧率<=%g（都是从真实分片里解出来的实测值）\n"
                % (args.max_w, args.max_h, args.max_fps))
        f.write("# 每条上面那行 # 注释 = 实测的 分辨率@帧率 与码率（设备端解析器会跳过 # 行）\n")
        f.write("# ⚠️ 源会失效；设备上放 /data/iptv.m3u 可覆盖本表（无需重刷固件）\n")
        for r in sorted(ok, key=lambda x: (classify(x["name"], x["group"]), x["name"])):
            g = classify(r["name"], r["group"])
            f.write("# 实测 %dx%d@%.2ffps %s %dkbps\n"
                    % (r["w"], r["h"], r["fps"], r.get("fps_src", ""), r["kbps"]))
            f.write('#EXTINF:-1 group-title="%s·可播" tvg-name="%s",%s\n'
                    % (g, r["name"], r["name"]))
            f.write("%s\n" % r["url"])
    print("已写出: %s（%d 个频道）" % (out, len(ok)))

    if args.json:
        import json
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"ok": ok, "bad": bad,
                       "criteria": {"max_w": args.max_w, "max_h": args.max_h,
                                    "max_fps": args.max_fps}}, f, ensure_ascii=False, indent=1)
        print("JSON: %s" % os.path.abspath(args.json))

    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write("# IPTV 源 PC 侧实测报告\n\n")
            f.write("探测机: %s｜候选 %d｜条件: H264 / 宽<=%d / 高<=%d / 帧率<=%g\n\n"
                    % (socket.gethostname(), n_all, args.max_w, args.max_h, args.max_fps))
            f.write("## 可用 %d\n\n| 频道 | 分组 | 实测分辨率@帧率 | 帧率来源 | 码率 | 耗时 |\n"
                    "|---|---|---|---|---|---|\n" % len(ok))
            for r in sorted(ok, key=lambda x: (classify(x["name"], x["group"]), x["name"])):
                f.write("| %s | %s | %dx%d@%g | %s | %d kbps | %.1fs |\n"
                        % (r["name"], classify(r["name"], r["group"]), r["w"], r["h"], r["fps"],
                           r.get("fps_src", ""), r["kbps"], r["t"]))
            f.write("\n## 淘汰 %d\n\n| 频道 | 原因 |\n|---|---|\n" % len(bad))
            for r in sorted(bad, key=lambda x: x["name"]):
                f.write("| %s | %s |\n" % (r["name"], r["why"]))
        print("报告: %s" % os.path.abspath(args.report))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default=DEFAULT_SRC,
                    help="候选清单（本地文件，或 URL，多个用 | 分隔）")
    ap.add_argument("-o", "--output", default="resources/iptv/channels.m3u")
    ap.add_argument("--report", default="")
    ap.add_argument("--json", default="", help="把完整记录（含 URL）写成 JSON，便于事后换格式/复筛")
    ap.add_argument("--max-w", type=int, default=1280)
    ap.add_argument("--max-h", type=int, default=720)
    ap.add_argument("--max-fps", type=float, default=30.0)
    ap.add_argument("--timeout", type=int, default=8)
    ap.add_argument("-j", "--jobs", type=int, default=32)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--keep", default="", help="额外一起探测的清单（本地文件或 URL）")
    ap.add_argument("--from-json", default="",
                    help="跳过探测，直接拿上次的 JSON 重出 m3u/报告（只改格式时用，省一次全量探测）")
    args = ap.parse_args()

    if args.from_json:
        import json
        d = json.load(open(args.from_json, encoding="utf-8"))
        return write_outputs(args, d["ok"], d["bad"], len(d["ok"]) + len(d["bad"]))

    items = parse_m3u(load_src(args.input, args.timeout))
    if args.keep:
        items += parse_m3u(load_src(args.keep, args.timeout))
    seen, uniq = set(), []
    for it in items:
        if it[2] in seen:
            continue
        seen.add(it[2])
        uniq.append(it)
    items = uniq[:args.limit] if args.limit else uniq
    print("候选频道: %d（去重后）" % len(items))

    ok, bad = [], []
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(probe_one, it, args.max_w, args.max_h, args.max_fps, args.timeout)
                for it in items]
        for i, fu in enumerate(cf.as_completed(futs), 1):
            r = fu.result()
            (ok if r["ok"] else bad).append(r)
            print("  [%3d/%3d] %s %-30s %s" % (
                i, len(futs), "✓" if r["ok"] else "×", r["name"][:30],
                ("%dx%d@%.2ffps %s" % (r["w"], r["h"], r["fps"], r["fps_src"])) if r["ok"] else r["why"]),
                flush=True)

    print("\n=== 可用: %d / %d ===" % (len(ok), len(items)))
    by = {}
    for r in bad:
        k = r["why"].split(":")[0]
        by[k] = by.get(k, 0) + 1
    print("淘汰: %s" % by)

    return write_outputs(args, ok, bad, len(items))


if __name__ == "__main__":
    sys.exit(main())
