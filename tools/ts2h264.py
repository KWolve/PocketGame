#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ts2h264.py - 从 MPEG-TS（本地文件或 HLS 分片 URL）里抽出 H264 裸流（Annex-B）

为什么需要它：
  要验证 `zk_h264_player`（设备硬件 H264 解码器）能不能用、能不能靠
  `FLAG_SCALE_DOWN_2/4` 解 720p，就得先有一段**没被解码**的 H264 ES 喂进去。
  而 IPTV 流是 TS 封装，所以中间需要一次"TS → H264 ES"的提取。
  设备上有 ffmpeg 能做这件事，但为了喂给**另一套**解码器做对照实验，
  在 PC 上离线抽一段出来更干净（不干扰设备上正在跑的链路）。

做什么：
  ① 解析 PAT → 找到 PMT 的 PID
  ② 解析 PMT → 找到 stream_type=0x1B（H264）的 elementary PID
  ③ 收集该 PID 的 TS 载荷，按 PES 头切分，剥掉 PES 头，把 ES 数据顺序写出
  （TS 里的 H264 本来就是 Annex-B，剥掉 PES 头后直接可用）

用法：
    python tools/ts2h264.py -i seg.ts -o out.h264
    python tools/ts2h264.py -i https://host/x.m3u8 -o out.h264 --seconds 6
    python tools/ts2h264.py -i https://host/seg.ts -o out.h264
"""
import argparse
import re
import socket
import sys
import urllib.request
from urllib.parse import urljoin

UA = {"User-Agent": "Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36"}
TS_PKT = 188


def fetch(url, limit=1 << 24, timeout=15):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=timeout) as f:
        return f.read(limit)


def load_input(src, seconds, timeout):
    """返回 (ts_bytes, 描述)。src 可以是本地文件、.ts 直链，或 .m3u8（会挑最低档后顺序拉分片）。"""
    if src.startswith("http://") or src.startswith("https://"):
        data = fetch(src, 1 << 20, timeout)
        text = data.decode("utf-8", "replace")
        if "#EXTM3U" not in text:
            return data, "直链 ts"
        # m3u8：挑最低带宽档 → 顺序拉分片，凑够 seconds 秒
        final = src
        if "#EXT-X-STREAM-INF" in text:
            best = None
            lines = [l.strip() for l in text.splitlines()]
            for i, l in enumerate(lines):
                if not l.startswith("#EXT-X-STREAM-INF:"):
                    continue
                m = re.search(r"BANDWIDTH=(\d+)", l)
                bw = int(m.group(1)) if m else 10 ** 9
                for j in range(i + 1, len(lines)):
                    if lines[j] and not lines[j].startswith("#"):
                        if best is None or bw < best[0]:
                            best = (bw, urljoin(final, lines[j]))
                        break
            if not best:
                raise SystemExit("master playlist 里没有可用档位")
            sub = best[1]
            sub_text = fetch(sub, 1 << 20, timeout).decode("utf-8", "replace")
            text, final = sub_text, sub
        segs = re.findall(r"^(?!#)(\S+\.ts[^\s]*)$", text, re.M)
        if not segs:
            raise SystemExit("清单里没有 .ts 分片")
        target = re.search(r"#EXT-X-TARGETDURATION:(\d+)", text)
        seg_sec = int(target.group(1)) if target else 6
        need = max(1, int(seconds / seg_sec) + 1)
        out = bytearray()
        for u in segs[-need:]:
            try:
                out += fetch(urljoin(final, u), 1 << 22, timeout)
            except Exception as e:
                print("  分片失败 %s: %s" % (u, e), file=sys.stderr)
        return bytes(out), "m3u8（拉了 %d 个分片）" % need
    with open(src, "rb") as f:
        return f.read(), "本地文件"


def section_at(d):
    """TS 载荷里取 PSI section。
    ⚠️ 踩过的坑：payload_unit_start_indicator=1 的包，**第一个字节是 pointer_field**
    （指向 section 起点相对偏移），必须先跳过它再从 table_id 开始读。
    不跳的话整段偏移错 1 字节 —— 实测表现为"PAT 解出来的 PMT PID 是个不存在的值
    （0x10E2），实际视频 PID 0x0100 永远找不到"。"""
    if not d:
        return d
    skip = 1 + d[0]
    return d[skip:] if skip < len(d) else b""


def parse_pat(pkt_data):
    """PAT: table_id(1) section_length(2) tsid(2) ver(1) sec(1) last(1) 然后
    program_number(2)+reserved/pid(2) 列表，末尾 CRC32(4)"""
    if len(pkt_data) < 12 or pkt_data[0] != 0x00:
        return None
    slen = ((pkt_data[1] & 0x0F) << 8) | pkt_data[2]
    body = pkt_data[8:3 + slen - 4]      # 跳过 CRC
    pmt_pids = []
    for i in range(0, len(body) - 3, 4):
        prog = (body[i] << 8) | body[i + 1]
        pid = ((body[i + 2] & 0x1F) << 8) | body[i + 3]
        if prog != 0:
            pmt_pids.append(pid)
    return pmt_pids


def parse_pmt(pkt_data):
    """PMT: 找 stream_type=0x1B(H264) / 0x24(HEVC) 的 elementary PID

    结构（从 table_id 起）：
      [0] table_id(0x02) [1..2] section_length [3..4] program_number [5] ver
      [6] section_number [7] last_section_number
      [8..9] PCR_PID  [10..11] program_info_length
      [12 .. 12+program_info_length-1] program_info 描述符   ← ★ 必须整段跳过
      [之后] 循环：stream_type(1) reserved/PID(2) ES_info_length(2) ES_info(...)
      [末尾] CRC32(4)

    ⚠️ 踩过的坑：忘了跳过 program_info 描述符（`o=12` 而不是 `12+plen`），
    就会在描述符里乱读 —— 实测表现为"解出一个 stream_type=0x25、pid 等于 PMT 自己"
    这种明显不合理的值，然后永远找不到视频 PID。
    """
    if len(pkt_data) < 12 or pkt_data[0] != 0x02:
        return None
    slen = ((pkt_data[1] & 0x0F) << 8) | pkt_data[2]
    pcr_pid = ((pkt_data[8] & 0x1F) << 8) | pkt_data[9]
    plen = ((pkt_data[10] & 0x0F) << 8) | pkt_data[11]
    off = 12 + plen                      # ★ 跳过 program_info
    end = min(3 + slen - 4, len(pkt_data))   # 去掉 CRC32
    vids = []
    while off + 4 < end:
        stype = pkt_data[off]
        pid = ((pkt_data[off + 1] & 0x1F) << 8) | pkt_data[off + 2]
        elen = ((pkt_data[off + 3] & 0x0F) << 8) | pkt_data[off + 4]
        if stype in (0x1B, 0x24):
            vids.append((stype, pid))
        off += 5 + elen
    return (pcr_pid, vids)


def extract(ts, verbose=True):
    pmt_pid = None
    video_pid = None
    stype = None
    es = bytearray()
    pes_buf = bytearray()
    n_pkt = len(ts) // TS_PKT
    pending_start = False

    for i in range(n_pkt):
        p = ts[i * TS_PKT:(i + 1) * TS_PKT]
        if len(p) < TS_PKT or p[0] != 0x47:
            continue
        pid = ((p[1] & 0x1F) << 8) | p[2]
        payload_start = (p[1] & 0x40) != 0
        afc = (p[3] >> 4) & 0x03
        if afc == 0 or afc == 2:
            continue
        off = 4
        if afc == 3:
            off = 4 + 1 + p[4]

        if pid == 0 and pmt_pid is None:
            pids = parse_pat(section_at(p[off:]) if payload_start else p[off:])
            if pids:
                pmt_pid = pids[0]
                if verbose:
                    print("  PAT → PMT pid=0x%X" % pmt_pid)
            continue

        if pmt_pid is not None and pid == pmt_pid and video_pid is None:
            r = parse_pmt(section_at(p[off:]) if payload_start else p[off:])
            if r:
                pcr_pid, vids = r
                if vids:
                    stype, video_pid = vids[0]
                    if verbose:
                        print("  PMT → 视频 pid=0x%X stream_type=0x%02X (%s)"
                              % (video_pid, stype,
                                 "H264" if stype == 0x1B else "HEVC"))
            continue

        if video_pid is None or pid != video_pid:
            continue

        d = p[off:]
        if payload_start:
            if len(pes_buf) > 9:
                es += pes_buf
            # 解析 PES 头：00 00 01 <stream_id> <len2> <flags2> <hdrlen> [可选头]
            if len(d) >= 9 and d[0] == 0 and d[1] == 0 and d[2] == 1:
                hdr_len = d[8]
                pes_buf = bytearray(d[9 + hdr_len:])
            else:
                pes_buf = bytearray(d)
        else:
            pes_buf += d

    if len(pes_buf) > 9:
        es += pes_buf
    return bytes(es), stype, n_pkt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", required=True)
    ap.add_argument("-o", "--output", default="test.h264")
    ap.add_argument("--seconds", type=int, default=6, help="m3u8 时拉多少秒")
    ap.add_argument("--timeout", type=int, default=15)
    args = ap.parse_args()

    print("输入: %s" % args.input)
    ts, desc = load_input(args.input, args.seconds, args.timeout)
    print("  已取 %d 字节（%s）" % (len(ts), desc))

    es, stype, n_pkt = extract(ts)
    if not es:
        raise SystemExit("没抽到视频 ES —— 检查源是不是 H264/mpegts")
    if stype == 0x24:
        print("⚠️ 这是 HEVC（0x24）—— 本板硬件**只解 H264**，喂进去没用")

    with open(args.output, "wb") as f:
        f.write(es)

    # 统计 NAL：看有没有 SPS/PPS/IDR（解码器起播必须要有 IDR）
    nals = {}
    i = 0
    while i + 4 < len(es):
        if es[i] == 0 and es[i + 1] == 0 and es[i + 2] == 1:
            t = es[i + 3] & 0x1F
            nals[t] = nals.get(t, 0) + 1
            i += 3
        else:
            i += 1
    names = {1: "非IDR切片", 5: "IDR(关键帧)", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}
    print("\n抽出 H264 ES: %d 字节，TS 包 %d 个" % (len(es), n_pkt))
    print("NAL 统计: %s" % ", ".join(
        "%s=%d" % (names.get(k, str(k)), v) for k, v in sorted(nals.items())))
    if 5 not in nals:
        print("⚠️ 没有 IDR 帧 —— 解码器起播会一直等关键帧，建议多拉几个分片（--seconds 加大）")
    if 7 not in nals:
        print("⚠️ 没有 SPS —— 有些流把 SPS/PPS 放在别处，解码器可能解不出来")
    print("已写出: %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
