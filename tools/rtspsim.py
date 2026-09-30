#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rtspsim.py —— 合成 RTSP 摄像头（台架用，不需要真 IPC）

为什么需要它（PocketGame/docs/camera-radio-app.md §4）：
  现场扫遍 LAN 一台 RTSP 摄像头都没有，只能自己造一个源。工程里已验过
  HTTP 源（tools/range_http.py），但 **RTSP 传输层本身一直没验过**。
  本脚本就是补这一环：一个能跑 DESCRIBE / SETUP / PLAY 的最小 RTSP 服务端，
  把 H.264 裸流（Annex-B）按 RTP 发出去。

用法：
  python rtspsim.py --bind 192.168.1.19 --port 8554 --file hd720.h264 --fps 25

支持两种 transport（真机侧 TCP 优先、失败回落 UDP，两种都要能接）：
  - RTP/AVP/TCP  → interleaved 模式（$<ch><len> 嵌在 RTSP 连接里）
  - RTP/AVP      → UDP 模式（服务端自己开 UDP 口往 client_port 发）

⚠️ Windows 防火墙：绑具体网卡 IP，别绑 0.0.0.0（沿用 range_http.py 的实测经验）。
"""
import argparse
import base64
import random
import re
import socket
import struct
import sys
import threading
import time

MTU_SAFE = 1400          # 单个 RTP 包最大负载（含 12 字节 RTP 头）
TCP_ONLY = False         # --tcp-only：只接受 interleaved TCP
CLOCK = 90000            # H.264 时钟 90kHz
PT = 96                  # 动态负载类型
SSRC = random.randint(1, 0x7FFFFFFF)


# ---------------------------------------------------------------- NAL 解析
def parse_annexb(path):
    """把 Annex-B 裸流切成 [(nal_type, bytes)]，去掉起始码。"""
    d = open(path, "rb").read()
    # 找所有起始码位置（3 字节 000001；4 字节 00000001 的最后一个 1 也会被找到）
    offs = []
    i = 0
    while True:
        j = d.find(b"\x00\x00\x01", i)
        if j < 0:
            break
        offs.append(j)
        i = j + 3
    nals = []
    for k, o in enumerate(offs):
        end = offs[k + 1] if k + 1 < len(offs) else len(d)
        nal = d[o + 3:end]
        # 去掉可能残留的尾部 0（4 字节起始码的前导 0 会落进上一个 NAL 末尾）
        nal = nal.rstrip(b"\x00") if nal else nal
        if not nal:
            continue
        nals.append((nal[0] & 0x1F, nal))
    return nals


def split_frames(nals):
    """按「每个 VCL NAL（1/5）算一帧」分组，把前面的参数集/SET 挂到帧上。

    返回 [(ts_nal_list)]，每个元素是一帧要发的 NAL 列表（含 AUD/SPS/PPS/SEI）。
    裸流里每个 IDR 前都有 SPS/PPS，所以每一帧都自带参数集 —— 客户端中途接入也能解。
    """
    frames = []
    cur = []
    for t, nal in nals:
        if t in (1, 5):
            cur.append(nal)
            frames.append(cur)
            cur = []
        else:
            cur.append(nal)
    return frames


# ---------------------------------------------------------------- RTP 打包
def rtp_packets(nal, seq, ts, ssrc):
    """把一个 NAL 打成 1..n 个 RTP 包（单包 / FU-A 分片）。

    ⚠️ RTP 头 12 字节的布局（这里写错过一次，记下来）：
        byte0 = V(2b)|P|X|CC(4b) —— 只有版本位，**PT 不在这里**
        byte1 = M(1b)|PT(7b)     —— 负载类型在这里；M=1 表示"本帧最后一个包"
    把 PT 写进 byte0 会让客户端把流认成 PT=0（PCMU），SDP 里的 96/H264 对不上。
    """
    out = []
    v0 = 0x80                      # V=2, P=0, X=0, CC=0
    if len(nal) <= MTU_SAFE - 12:
        hdr = struct.pack("!BBHII", v0, 0x80 | PT, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        out.append((hdr + nal, seq & 0xFFFF))
        return out, (seq + 1) & 0xFFFF
    # FU-A
    ind = (nal[0] & 0xE0) | 28
    typ = nal[0] & 0x1F
    body = nal[1:]
    chunk = MTU_SAFE - 12 - 2
    first = True
    i = 0
    while i < len(body):
        piece = body[i:i + chunk]
        i += len(piece)
        last = (i >= len(body))
        fu = ((0x80 if first else 0) | (0x40 if last else 0) | typ)
        # 分片的最后一个包才置 M 位（"一帧结束"）
        b1 = (PT | (0x80 if last else 0))
        hdr = struct.pack("!BBHII", v0, b1, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        out.append((hdr + bytes([ind, fu]) + piece, seq & 0xFFFF))
        seq = (seq + 1) & 0xFFFF
        first = False
    return out, seq


def _req(conn, timeout):
    """读一条 RTSP 请求（含 Content-Length 指定的 body）。"""
    conn.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = conn.recv(4096)
        if not c:
            return None
        buf += c
    head, _, rest = buf.partition(b"\r\n\r\n")
    m = re.search(rb"Content-Length:\s*(\d+)", head, re.I)
    need = int(m.group(1)) if m else 0
    while len(rest) < need:
        c = conn.recv(4096)
        if not c:
            break
        rest += c
    lines = head.decode("utf-8", "replace").split("\r\n")
    method, uri, _ver = (lines[0].split(" ") + ["", ""])[:3]
    hdrs = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
    return method.upper(), uri, hdrs, rest[:need]


def sdp(sps, pps, fps=25):
    sp = base64.b64encode(sps).decode()
    pp = base64.b64encode(pps).decode()
    return ("v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=CamSim\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            # ★ 真机摄像头（海康等）都带 sprop-parameter-sets，台架要跟真机一致
            "a=fmtp:96 packetization-mode=1;profile-level-id=%s;"
            "sprop-parameter-sets=%s,%s\r\n"
            "a=control:trackID=0\r\n"
            "a=framerate:%d\r\n" % (sps[1:4].hex(), sp, pp, fps))


class Session(threading.Thread):
    """一个客户端会话：先握手，PLAY 之后按 fps 推流。"""

    def __init__(self, conn, addr, frames, fps, timeout, log):
        super().__init__(daemon=True)
        self.conn = conn
        self.addr = addr
        self.frames = frames
        self.fps = fps
        self.timeout = timeout
        self.log = log
        self.seq = random.randint(0, 0xFFFF)
        self.stopped = False
        self.transport = None      # 'tcp' / 'udp'
        self.rtp_ch = 0
        self.rtcp_ch = 1
        self.udp = None
        self.udp_dst = None
        self.playing = False

    # ---------- 发送 ----------
    def send_rtp(self, pkt):
        if self.transport == "tcp":
            head = b"$" + bytes([self.rtp_ch]) + struct.pack("!H", len(pkt))
            self.conn.sendall(head + pkt)
        else:
            self.udp.sendto(pkt, self.udp_dst)

    def send_rtcp(self, pkts):
        """最小 RTCP SR —— 有些客户端靠它推进 npt / 算抖动。

        ⚠️ **UDP 模式也必须发**（踩过）：SDP 里声明了 `a=range:npt=0-`，
        ffmpeg 的 rtsp 解封装会等 RTCP 建立时基再返回；UDP 上不发就干等，
        实测 `avformat_open_input` 卡了 **16.6 秒**（设备侧日志
        `PgStream: 打开成功 用时 16641ms`），直接超过摄像头页 15s 的等首帧超时。
        """
        #  SR: 0x80|PT=200, length(=6), ssrc, ntp(8), rtptime, pktcount, octetcount
        #  总长 28 字节 ⇒ 头部 4 + 6 个 32bit 字 = BBH + 6×I
        sr = struct.pack("!BBHIIIIII", 0x80, 200, 6, SSRC, 0, 0, 0, 0, 0)
        try:
            if self.transport == "tcp":
                head = b"$" + bytes([self.rtcp_ch]) + struct.pack("!H", len(sr))
                self.conn.sendall(head + sr)
            elif self.transport == "udp" and self.udp and self.udp_dst:
                self.udp.sendto(sr, (self.udp_dst[0], self.udp_dst[1] + 1))
        except Exception:
            pass

    # ---------- 主循环 ----------
    def run(self):
        rd = self.conn
        try:
            while not self.stopped:
                r = _req(rd, self.timeout)
                if r is None:
                    break
                method, uri, h, body = r
                cseq = h.get("cseq", "0")
                if method == "OPTIONS":
                    self.resp(cseq, 'Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, '
                                    'TEARDOWN, GET_PARAMETER')
                elif method == "DESCRIBE":
                    s = sdp(self.sps, self.pps, self.fps)
                    self.resp(cseq, "Content-Type: application/sdp", s)
                    self.log("DESCRIBE -> 200 (SDP %d 字节)" % len(s))
                elif method == "SETUP":
                    tr = h.get("transport", "")
                    if TCP_ONLY and "RTP/AVP/TCP" not in tr.upper():
                        # 对照实验用：只认 TCP。客户端第一次（UDP）开流会立刻失败，
                        # 正好逼出播放侧的 UDP→TCP 回落那条路（PgStream::openInput）。
                        self.conn.sendall(("RTSP/1.0 461 Unsupported Transport\r\n"
                                           "CSeq: %s\r\nSession: 1\r\n\r\n" % cseq).encode())
                        self.log("SETUP  %s -> 461（本台架 --tcp-only）" % tr)
                        continue
                    if "RTP/AVP/TCP" in tr.upper():
                        self.transport = "tcp"
                        m = re.search(r"interleaved=(\d+)-(\d+)", tr)
                        if m:
                            self.rtp_ch, self.rtcp_ch = int(m.group(1)), int(m.group(2))
                        else:
                            self.rtp_ch, self.rtcp_ch = 0, 1
                        tr_out = ("RTP/AVP/TCP;unicast;interleaved=%d-%d"
                                  % (self.rtp_ch, self.rtcp_ch))
                    else:
                        self.transport = "udp"
                        m = re.search(r"client_port=(\d+)-(\d+)", tr)
                        cp = int(m.group(1)) if m else 5000
                        if self.udp is None:
                            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                            self.udp.bind(("0.0.0.0", 0))
                        self.udp_dst = (self.addr[0], cp)
                        sp = self.udp.getsockname()[1]
                        tr_out = ("RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d"
                                  % (cp, cp + 1, sp, sp + 1))
                    self.resp(cseq, "Transport: " + tr_out)
                    self.log("SETUP  transport=%s  %s" % (self.transport, tr_out))
                elif method == "PLAY":
                    self.playing = True
                    self.resp(cseq, "Range: npt=0.000-\r\nSession: 1")
                    self.log("PLAY   -> 开始推流")
                    self.pump()
                    self.playing = False
                elif method in ("TEARDOWN", "PAUSE"):
                    self.resp(cseq)
                    self.log("%s -> 收尾" % method)
                    if method == "TEARDOWN":
                        break
                elif method == "GET_PARAMETER":
                    self.resp(cseq)
                else:
                    self.resp(cseq)
        except Exception as e:
            self.log("会话结束：%s" % e)
        finally:
            self.stopped = True
            try:
                self.conn.close()
            except Exception:
                pass
            if self.udp:
                self.udp.close()

    def resp(self, cseq, extra="", body=""):
        if isinstance(body, str):
            body = body.encode()
        hdr = ("RTSP/1.0 200 OK\r\nCSeq: %s\r\nSession: 1\r\n" % cseq)
        if body:
            hdr += "Content-Length: %d\r\n" % len(body)
        if extra:
            hdr += extra + "\r\n"
        self.conn.sendall(hdr.encode() + b"\r\n" + body)

    def pump(self):
        """按 fps 循环推流。帧间隔用绝对时间对齐，防止漂移累积。"""
        dt = 1.0 / self.fps
        t0 = time.time()
        n = 0
        while not self.stopped:
            f = self.frames[n % len(self.frames)]
            ts = (n * CLOCK // self.fps) & 0xFFFFFFFF
            for nal in f:
                pkts, self.seq = rtp_packets(nal, self.seq, ts, SSRC)
                for pkt, _sq in pkts:
                    self.send_rtp(pkt)
            n += 1
            if n % self.fps == 0:      # 每秒一个 SR（真机摄像头也是这个节奏）
                self.send_rtcp(n)
            target = t0 + n * dt
            sleep = target - time.time()
            if sleep > 0:
                time.sleep(sleep)
            elif sleep < -1.0:
                t0 = time.time() - n * dt      # 落后太多就重新对齐
            # 顺便检查客户端是否断开（TCP 模式读不到会抛异常）
            if self.transport == "tcp":
                try:
                    self.conn.setblocking(False)
                    d = self.conn.recv(4096, socket.MSG_PEEK)
                    if not d:
                        raise ConnectionError("客户端关闭")
                    # 客户端可能会发 TEARDOWN / keepalive，交给主循环
                    self.conn.setblocking(True)
                    self.conn.settimeout(2.0)
                    r = _req(self.conn, 2.0)
                    self.conn.setblocking(True)
                    if r and r[0] == "TEARDOWN":
                        self.stopped = True
                        return
                except BlockingIOError:
                    self.conn.setblocking(True)
                    self.conn.settimeout(self.timeout)
                except socket.timeout:
                    pass
                except ConnectionError:
                    self.stopped = True
                    return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--file", required=True)
    ap.add_argument("--fps", type=int, default=25)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--tcp-only", action="store_true",
                    help="只认 RTP/AVP/TCP（逼客户端走 TCP 回落，做对照实验用）")
    a = ap.parse_args()
    global TCP_ONLY
    TCP_ONLY = a.tcp_only

    nals = parse_annexb(a.file)
    sps = next(n for t, n in nals if t == 7)
    pps = next(n for t, n in nals if t == 8)
    frames = split_frames(nals)
    print("[rtspsim] 载入 %s：%d 个 NAL / %d 帧，SPS %d 字节，PPS %d 字节"
          % (a.file, len(nals), len(frames), len(sps), len(pps)), flush=True)

    lock = threading.Lock()

    def log(m):
        with lock:
            print("[rtspsim %s] %s" % (time.strftime("%H:%M:%S"), m), flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.bind, a.port))
    srv.listen(8)
    log("监听 rtsp://%s:%d/  已就绪（Ctrl+C 退出）" % (a.bind, a.port))

    while True:
        conn, addr = srv.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s = Session(conn, addr, frames, a.fps, a.timeout, log)
        s.sps, s.pps = sps, pps
        log("新连接 %s:%d" % addr)
        s.start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
