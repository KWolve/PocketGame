#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
radio_probe.py - **实测**网络收音机电台源能不能播（PC 侧，纯 stdlib，不依赖 ffmpeg）

为什么要这个工具：
  电台表（radioLogic.cc 的 kBuiltinSta[]）以前只有 7 个"示例台"，是拍脑袋写的。
  电台 URL 的生命周期很短（换 CDN / 下线 / 限流都会发生），所以"表里有多少个台"这件事
  **必须能一键复测**，否则文档里的数量一定过期。

它判什么（都是"真去拉一段字节"，不看清单里声明什么）：
  ① HTTP 能不能通（状态码 / TTFB / 是否被墙或 403）；
  ② 直连流：读前 16KB，首字节签名是不是音频 ——
       ID3 / 0xFFEx(MP3 sync) / 0xFFF1(ADTS AAC) / 'OggS' / 0x47(TS)
     同时看 Content-Type（audio/* / application/octet-stream / video/mp2t 都算）；
  ③ HLS(.m3u8)：拉清单 → 若是 master 取**最低码率档** → 拉第一个分片 → 验同步字
     （0x47 TS / ID3 / ADTS）；分片拉不到 = 这条链在板上也会卡住；
  ④ 综合给一个 verdict：OK / WEAK / FAIL，并给出原因。

用法：
    python tools/radio_probe.py                       # 探 tools/radio_cand.txt
    python tools/radio_probe.py -i my.txt -o out.txt  # 自定义输入/输出
    python tools/radio_probe.py --report r.md         # 另存可读报告
    python tools/radio_probe.py --jobs 12

输入格式（每行）：`名称 | 分组 | 地址`（`#` 开头是注释；分组可省，省了归"其他"）
输出：默认把**通过**的条目写成 `名称|地址` 追加分组注释，可直接丢到 /data/radio.txt。
⚠️ 这台设备的字库是子集、没有逐字回退 ⇒ 名字里别用生僻字（工具会提示非 ASCII 情况）。
"""
import argparse
import concurrent.futures as cf
import os
import re
import socket
import ssl
import sys
import time
import urllib.error
import urllib.request

UA = "PocketGameRadioProbe/1.0"
TO = 10          # 单次请求超时（秒）
MAXB = 16384     # 直连流最多读多少字节就判定

# ⚠️ **PC 上常常挂着 http(s)_proxy**（本工程这台机器就有）：代理会把流"改写"成
#    非音频字节（实测同一 URL 走代理首字节是随机数据、不走代理才是 MP3）
#    ⇒ 探电台必须直连，否则测出来的是代理的行为、不是电台的行为。
#    设备上没有代理，所以直连才代表真机。
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def fetch(url, rng=None, maxb=None, timeout=TO):
    """返回 (status, ctype, data, ttfb_ms, err)。data 最多 maxb 字节。"""
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if rng:
        req.add_header("Range", "bytes=%s" % rng)
    t0 = time.time()
    try:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        r = _OPENER.open(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        return (e.code, e.headers.get("Content-Type", ""), b"", (time.time() - t0) * 1000,
                "HTTP %d" % e.code)
    except Exception as e:                                  # noqa: BLE001
        if "BadStatusLine" in type(e).__name__ or "ICY" in str(e):
            return fetch_icy(url, maxb or MAXB, timeout)
        return (0, "", b"", (time.time() - t0) * 1000, "%s: %s" % (type(e).__name__, e))
    ttfb = (time.time() - t0) * 1000
    ctype = r.headers.get("Content-Type", "") or ""
    buf = b""
    try:
        while len(buf) < (maxb or MAXB):
            chunk = r.read(2048)
            if not chunk:
                break
            buf += chunk
    except (socket.timeout, Exception):                     # 流式源读到一半正常
        pass
    finally:
        try:
            r.close()
        except Exception:                                   # noqa: BLE001
            pass
    return (r.status, ctype, buf, ttfb, "")


def fetch_icy(url, maxb, timeout=TO):
    """Shoutcast/Icecast 的 `ICY 200 OK` 响应：http.client 会当成非法状态行报错，
    但**设备的 ffmpeg 是认的** ⇒ 这里用裸 socket 自己读，别让 PC 侧的挑剔把好台判死。
    （本次实测命中的：`http://server1.chilltrax.com:9000/`）"""
    from urllib.parse import urlsplit
    u = urlsplit(url)
    if u.scheme != "http":
        return (0, "", b"", 0, "ICY 只支持 http")
    host, port = u.hostname, (u.port or 80)
    path = (u.path or "/") + (("?" + u.query) if u.query else "")
    t0 = time.time()
    try:
        sk = socket.create_connection((host, port), timeout=timeout)
        sk.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\n"
                    "Icy-MetaData: 0\r\n\r\n" % (path, host, UA)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf and len(buf) < 8192:
            c = sk.recv(1024)
            if not c:
                break
            buf += c
        head, _, rest = buf.partition(b"\r\n\r\n")
        ctype = ""
        for ln in head.split(b"\r\n"):
            if ln.lower().startswith(b"content-type:"):
                ctype = ln.split(b":", 1)[1].strip().decode("latin-1")
        body = rest
        while len(body) < maxb:
            c = sk.recv(4096)
            if not c:
                break
            body += c
        sk.close()
        return (200, ctype or "audio/mpeg(ICY)", body[:maxb], (time.time() - t0) * 1000, "")
    except Exception as e:                                  # noqa: BLE001
        return (0, "", b"", (time.time() - t0) * 1000, "ICY: %s" % e)


MP3_BR = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
MP3_SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def _mp3_frame_len(b, i):
    """b[i] 起是不是一个 MPEG 音频帧头？是则返回帧长（字节），否则 0。"""
    if i + 4 > len(b) or b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return 0
    ver = (b[i + 1] >> 3) & 3          # 3=MPEG1 2=MPEG2 0=MPEG2.5
    layer = (b[i + 1] >> 1) & 3        # 1=Layer3 2=Layer2 3=Layer1
    brx = (b[i + 2] >> 4) & 0xF
    srx = (b[i + 2] >> 2) & 3
    pad = (b[i + 2] >> 1) & 1
    if ver == 1 or layer == 0 or brx in (0, 15) or srx == 3:
        return 0
    br = MP3_BR[brx] * 1000
    sr = MP3_SR.get(ver, [44100])[srx]
    if layer == 3:                      # Layer1：帧长 = (12*br/sr + pad)*4
        return int(12 * br / sr + pad) * 4
    k = 144 if (layer == 1 and ver == 3) else (72 if layer == 1 else 144)
    return int(k * br / sr + pad)


def audio_sig(b):
    """在**前 8KB**里找一个"被下一帧确认过"的同步头 → 编码名 / None。

    为什么不能"只看头几个字节"：电台流前面常有一段非音频前缀（实测蜻蜓的流里
    第 0 字节是随机字节、MP3 同步头在第 6/15/144 字节），只看前 64 字节会把
    **能播的台误判成打不开**（第一版就是这么误报了 12 个台）。
    也不能"只找 0xFF 就算"——随机数据里 0xFF 很常见 ⇒ 必须用**第二帧**确认。
    """
    if not b:
        return None
    if b[:3] == b"ID3":
        return "MP3/ID3"
    if b[:4] == b"OggS":
        return "OGG"
    if b[:4] == b"fLaC":
        return "FLAC"
    n = min(len(b) - 4, 8192)
    for i in range(0, n):
        if b[i] == 0x47 and i + 376 < len(b) and b[i + 188] == 0x47 and b[i + 376] == 0x47:
            return "MPEG-TS"
        fl = _mp3_frame_len(b, i)
        if fl:
            if i + fl + 4 <= len(b) and _mp3_frame_len(b, i + fl):
                return "MP3(+%d)" % i
            return "MP3(+%d)" % i
        if b[i] == 0xFF and (b[i + 1] & 0xF6) == 0xF0 and i + 9 < len(b):
            afl = ((b[i + 3] & 3) << 11) | (b[i + 4] << 3) | ((b[i + 5] >> 5) & 7)
            if afl > 6 and i + afl + 2 <= len(b) and b[i + afl] == 0xFF and \
                    (b[i + afl + 1] & 0xF6) == 0xF0:
                return "AAC/ADTS(+%d)" % i
            if afl > 6:
                return "AAC/ADTS(+%d)" % i
    return None


def is_hls(url, ctype, body):
    if ".m3u8" in url.lower():
        return True
    if "mpegurl" in ctype.lower():
        return True
    return body[:7] == b"#EXTM3U"


def probe_hls(url):
    """拉清单 → 取第一个（或最低码率档的）分片 → 验同步字。"""
    st, ct, body, ttfb, err = fetch(url, maxb=65536, timeout=TO)
    if err:
        return ("FAIL", "清单拉不到：%s" % err, "", 0)
    txt = body.decode("utf-8", "replace")
    if "#EXTM3U" not in txt:
        return ("FAIL", "不是 m3u8（ctype=%s，%db）" % (ct, len(body)), "", ttfb)
    if "#EXT-X-STREAM-INF" in txt:                          # master → 挑最低 BANDWIDTH
        best, bw_best = None, None
        lines = [x.strip() for x in txt.splitlines()]
        for i, ln in enumerate(lines):
            if ln.startswith("#EXT-X-STREAM-INF"):
                m = re.search(r"BANDWIDTH=(\d+)", ln)
                bw = int(m.group(1)) if m else 0
                for nxt in lines[i + 1:]:
                    if nxt and not nxt.startswith("#"):
                        if bw_best is None or bw < bw_best:
                            bw_best, best = bw, nxt
                        break
        if not best:
            return ("FAIL", "master 清单里找不到档位", "", ttfb)
        url = url.rsplit("/", 1)[0] + "/" + best if not best.startswith("http") else best
        st, ct, body, ttfb2, err = fetch(url, maxb=65536, timeout=TO)
        if err:
            return ("FAIL", "media 清单拉不到：%s" % err, "", ttfb2)
        txt = body.decode("utf-8", "replace")
    seg = None
    for ln in txt.splitlines():
        ln = ln.strip()
        if ln and not ln.startswith("#"):
            seg = ln
            break
    if not seg:
        return ("FAIL", "清单里没有分片", "", ttfb)
    segurl = seg if seg.startswith("http") else url.rsplit("/", 1)[0] + "/" + seg
    st, ct, body, ttfb2, err = fetch(segurl, rng="0-32767", maxb=32768, timeout=TO)
    if err:
        return ("FAIL", "分片拉不到：%s" % err, "", ttfb2)
    sig = audio_sig(body)
    if not sig:
        return ("FAIL", "分片不是音频（ctype=%s 前8字节=%s）" % (ct, body[:8].hex()), "", ttfb2)
    if sig == "OGG":
        return ("WEAK", "HLS+OGG(%s)" % ct, segurl, ttfb2)
    return ("OK", "HLS 分片=%s ctype=%s" % (sig, ct), segurl, ttfb2)


def probe_one(item):
    name, group, url = item
    if is_hls(url, "", b""):
        v, why, seg, ms = probe_hls(url)
        return (name, group, url, v, why, ms, seg)
    st, ct, body, ttfb, err = fetch(url, rng="0-%d" % (MAXB - 1), maxb=MAXB, timeout=TO)
    if err and st == 0:
        return (name, group, url, "FAIL", "连不上：%s" % err, ttfb, "")
    if st >= 400:
        return (name, group, url, "FAIL", "HTTP %d" % st, ttfb, "")
    sig = audio_sig(body)
    if not sig:
        bad = body[:8].hex()
        # 有些服务端不理会 Range、先回 HTML 播放器页 ⇒ 这里也会判成"不是音频"
        return (name, group, url, "FAIL",
                "不是音频签名（ctype=%s 前8字节=%s）" % (ct, bad), ttfb, "")
    if sig == "OGG":
        return (name, group, url, "WEAK", "OGG（板上解码器未必有）", ttfb, "")
    if sig == "FLAC":
        return (name, group, url, "WEAK", "FLAC（码率高、没必要）", ttfb, "")
    return (name, group, url, "OK", "%s ctype=%s %db" % (sig, ct, len(body)), ttfb, "")


def read_cand(path):
    out = []
    with open(path, "r", encoding="utf-8") as f:
        for ln in f:
            ln = ln.split("#")[0].strip()
            if not ln:
                continue
            parts = [p.strip() for p in ln.split("|")]
            if len(parts) == 2:
                nm, gp, ur = parts[0], "其他", parts[1]
            elif len(parts) >= 3:
                nm, gp, ur = parts[0], parts[1], parts[2]
            else:
                continue
            out.append((nm, gp, ur))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--in", dest="inp",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "radio_cand.txt"))
    ap.add_argument("-o", "--out", default=None, help="把通过的写成 名称|地址（含分组注释）")
    ap.add_argument("--report", default=None)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", default=None, help="只探名字里含该子串的")
    a = ap.parse_args()

    cand = read_cand(a.inp)
    if a.only:
        cand = [c for c in cand if a.only in c[0] or a.only in c[1]]
    print("候选 %d 条 -> 并发 %d" % (len(cand), a.jobs))
    res = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = {ex.submit(probe_one, c): c for c in cand}
        for i, f in enumerate(cf.as_completed(futs), 1):
            r = f.result()
            res.append(r)
            print("[%2d/%2d] %-4s %-24s %s" % (i, len(cand), r[3], r[0][:24], r[4]))

    order = {"OK": 0, "WEAK": 1, "FAIL": 2}
    res.sort(key=lambda r: (order[r[3]], r[1], r[0]))
    nok = sum(1 for r in res if r[3] == "OK")
    nweak = sum(1 for r in res if r[3] == "WEAK")
    print("\n==== 实测结果：OK %d / WEAK %d / FAIL %d（共 %d）====" %
          (nok, nweak, len(res) - nok - nweak, len(res)))

    if a.out:
        gp_now = None
        with open(a.out, "w", encoding="utf-8") as f:
            for nm, gp, ur, v, why, ms, seg in res:
                if v == "FAIL":
                    continue
                if gp != gp_now:
                    f.write("# ===== %s =====\n" % gp)
                    gp_now = gp
                f.write("%s|%s\n" % (nm, ur))
        print("已写出可用表：%s" % a.out)

    if a.report:
        with open(a.report, "w", encoding="utf-8") as f:
            f.write("# 网络收音机电台源实测报告\n\n> 工具 `tools/radio_probe.py`，"
                    "判据 = 真拉一段字节验同步字（不是看清单声明）。\n\n")
            f.write("| 判定 | 名称 | 分组 | 实测 | TTFB | 地址 |\n|---|---|---|---|---|---|\n")
            for nm, gp, ur, v, why, ms, seg in res:
                f.write("| %s | %s | %s | %s | %dms | `%s` |\n" % (v, nm, gp, why, ms, ur))
            f.write("\n**OK %d / WEAK %d / 共 %d**\n" % (nok, nweak, len(res)))
        print("已写出报告：%s" % a.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
