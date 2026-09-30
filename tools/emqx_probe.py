#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
emqx_probe.py - EMQX（MQTT Broker）侧 P0 探针（只读）

为什么需要它：HA 实机上装了 `mqtt.*` 组件（mqtt.light / mqtt.switch / mqtt.sensor …），
说明**公司这台 HA 的设备是通过 MQTT 接进来的**。于是多出一条可选通道：

    设备(屏幕) --MQTT--> EMQX <--HA(同一批 topic)
    设备(屏幕) --REST/WS--> HA

必须先把 broker 的真实情况量清楚，否则"M×QTT 当实时通道"这个选项没法评估。

本脚本只做**只读**侦察：
  1. EMQX Dashboard API（18083）拿版本 / 监听器 / 在线客户端 / 订阅表
  2. 用原生 socket 手写 MQTT CONNECT + SUBSCRIBE，短时间嗅探 `homeassistant/#`
     （HA MQTT 发现的固定前缀），看设备到底挂在哪些 topic 上

用法（凭据走环境变量）：
    set EMQX_USER=xxx
    set EMQX_PASS=xxx
    python emqx_probe.py --url http://192.168.1.188:18083 --mqtt-host 192.168.1.188 [--out docs/emqx-probe.md]
"""

import argparse
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
UA = "V851s-PocketGame/emqx_probe"


def http(method, url, data=None, headers=None, timeout=10):
    hdrs = {"User-Agent": UA}
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        hdrs["Content-Type"] = "application/json"
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    try:
        with OPENER.open(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode()


# ------------------------------------------------------------------ 原生 MQTT

def mqtt_str(s):
    b = s.encode()
    return struct.pack(">H", len(b)) + b


def mqtt_varint(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        d = sock.recv(n - len(buf))
        if not d:
            raise IOError("连接已断（还差 %d 字节）" % (n - len(buf)))
        buf += d
    return buf


def mqtt_connect(sock, client_id, user="", pwd="", keepalive=30):
    """MQTT 3.1.1 CONNECT。返回 (return_code, session_present)。"""
    flags = 0x02  # clean session
    payload = mqtt_str(client_id)
    if user:
        flags |= 0x80
        payload += mqtt_str(user)
    if pwd:
        flags |= 0x40
        payload += mqtt_str(pwd)
    body = mqtt_str("MQTT") + bytes([4, flags]) + struct.pack(">H", keepalive) + payload
    sock.sendall(bytes([0x10]) + mqtt_varint(len(body)) + body)
    hdr = recv_exact(sock, 2)
    if hdr[0] >> 4 != 2:
        raise IOError("CONNACK 包头不对：0x%02x" % hdr[0])
    ack = recv_exact(sock, hdr[1] if hdr[1] else 2)
    return ack[-1], bool(ack[0] & 0x01)


def mqtt_subscribe(sock, topic, qos=0, mid=1):
    body = struct.pack(">H", mid) + mqtt_str(topic) + bytes([qos])
    sock.sendall(bytes([0x82]) + mqtt_varint(len(body)) + body)


def mqtt_read_packets(sock, seconds, max_pkts=400):
    """收 PUBLISH，返回 (msgs, subacks)。
    msgs = [(topic, payload)]；subacks = [(mid, [granted_qos...])]（0x80 = 被 ACL 拒）。"""
    msgs, subacks = [], []
    end = time.time() + seconds
    buf = b""
    while time.time() < end and len(msgs) < max_pkts:
        sock.settimeout(max(0.2, end - time.time()))
        try:
            d = sock.recv(8192)
        except socket.timeout:
            break
        except Exception:  # noqa: BLE001
            break
        if not d:
            break
        buf += d
        while len(buf) >= 2:
            ptype = buf[0] >> 4
            ln, mult, idx, need = 0, 1, 1, False
            while True:
                if idx >= len(buf):
                    need = True
                    break
                b = buf[idx]
                ln += (b & 0x7F) * mult
                mult *= 128
                idx += 1
                if not (b & 0x80):
                    break
                if mult > 128 ** 3:
                    need = True
                    break
            if need or len(buf) < idx + ln:
                break
            pkt = buf[idx:idx + ln]
            buf = buf[idx + ln:]
            if ptype == 3 and len(pkt) >= 2:
                tl = struct.unpack(">H", pkt[:2])[0]
                if 2 + tl <= len(pkt):
                    msgs.append((pkt[2:2 + tl].decode("utf-8", "replace"),
                                 pkt[2 + tl:].decode("utf-8", "replace")))
            elif ptype == 9 and len(pkt) >= 3:
                mid = struct.unpack(">H", pkt[:2])[0]
                subacks.append((mid, list(pkt[2:])))
    return msgs, subacks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("EMQX_URL", "http://192.168.1.188:18083"))
    ap.add_argument("--user", default=os.environ.get("EMQX_USER", ""))
    ap.add_argument("--passwd", default=os.environ.get("EMQX_PASS", ""))
    ap.add_argument("--mqtt-host", default="192.168.1.188")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--sniff", type=int, default=12, help="嗅探秒数")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    base = args.url.rstrip("/")
    host = urllib.parse.urlparse(base).hostname

    L = []
    def say(s=""):
        print(s)
        L.append(s)

    say("# EMQX（MQTT Broker）P0 探针结果")
    say()
    say("目标：`%s`（由 `tools/emqx_probe.py` 实测，只读）" % base)
    say()

    # --- 1. Dashboard API ---
    say("## 1. Dashboard API")
    say()
    tok = None
    if args.user:
        st, raw = http("POST", base + "/api/v5/login",
                       {"username": args.user, "password": args.passwd})
        say("- `POST /api/v5/login` → HTTP %s" % st)
        if st == 200:
            j = json.loads(raw.decode("utf-8", "replace"))
            tok = j.get("token")
            say("- EMQX 版本：**%s**" % j.get("version"))
            say("- 已取得 API token：%s" % ("是" if tok else "否"))
        else:
            say("- body：`%s`" % raw.decode("utf-8", "replace")[:200])
        say()
    else:
        say("未提供 EMQX_USER/EMQX_PASS，跳过需鉴权的 API。")
        say()

    H = {"Authorization": "Bearer " + tok} if tok else None

    def api(path, label, keys=(), timeout=10):
        """EMQX v5 的列表接口是分页对象 {"data":[...],"meta":{...}}，要拆开。"""
        if not H:
            return None
        st, raw = http("GET", base + "/api/v5/" + path, headers=H, timeout=timeout)
        if st != 200:
            say("- `%s` → HTTP %s `%s`" % (label, st, raw.decode("utf-8", "replace")[:120]))
            return None
        try:
            j = json.loads(raw.decode("utf-8", "replace"))
        except Exception:  # noqa: BLE001
            say("- `%s` → 非 JSON" % label)
            return None
        rows = j.get("data") if isinstance(j, dict) and isinstance(j.get("data"), list) else j
        meta = j.get("meta") if isinstance(j, dict) else None
        n = len(rows) if isinstance(rows, list) else 1
        say("- `%s` → HTTP 200（%d 条%s）"
            % (label, n, ("，共 %s" % meta.get("count")) if isinstance(meta, dict) and meta.get("count") is not None else ""))
        if isinstance(rows, list) and rows and keys:
            say()
            say("| " + " | ".join(keys) + " |")
            say("|" + "---|" * len(keys))
            for it in rows[:20]:
                say("| " + " | ".join(str(it.get(k, "")) for k in keys) + " |")
            say()
        elif isinstance(rows, list) and rows:
            say()
            say("```json")
            say(json.dumps(rows[:6], ensure_ascii=False)[:900])
            say("```")
            say()
        return rows if isinstance(rows, list) else [rows]

    if H:
        nodes = api("nodes", "GET /nodes", ("node", "version", "uptime"))
        api("listeners", "GET /listeners", ("id", "type", "current_connections", "running", "bind"))
        say()
        cl = api("clients?limit=50", "GET /clients", ("clientid", "username", "ip_address", "connected"))
        say()
        subs = api("subscriptions?limit=200", "GET /subscriptions", ("clientid", "topic", "qos")) or []
        say()
        if subs:
            topics = sorted({s.get("topic", "") for s in subs if isinstance(s, dict)})
            say("- **全部订阅 topic 去重（%d 个）**：" % len(topics))
            for t in topics[:60]:
                say("  - `%s`" % t)
            say()

    # --- 2. 原生 MQTT 探测 ---
    say("## 2. 原生 MQTT 连接 + 嗅探（手写 CONNECT/SUBSCRIBE）")
    say()
    try:
        s = socket.create_connection((args.mqtt_host, args.mqtt_port), timeout=6)
        t0 = time.time()
        rc, sp = mqtt_connect(s, "pg-probe-%d" % int(time.time()),
                              args.user, args.passwd)
        say("- `CONNECT` → return code **%d**（0 = 接受；4 = 用户名/密码错；5 = 未授权）"
            % rc)
        say("- 握手耗时 %d ms" % int((time.time() - t0) * 1000))
        if rc == 0:
            # 两个订阅一起发：#（全量，可能被 ACL 拒）与 homeassistant/#（HA MQTT 发现前缀）
            mqtt_subscribe(s, "#", mid=1)
            mqtt_subscribe(s, "homeassistant/#", mid=2)
            pkts, subacks = mqtt_read_packets(s, args.sniff)
            if subacks:
                say("- `SUBACK`：%s（granted qos 里 **0x80 = 该主题被 ACL 拒绝**）"
                    % "; ".join("mid=%d -> %s" % (m, [hex(x) for x in q]) for m, q in subacks))
            say("- 嗅探 %d 秒，收到 **%d 条**消息" % (args.sniff, len(pkts)))
            if pkts:
                say()
                say("| topic | payload 摘要 |")
                say("|---|---|")
                for tp, pl in pkts[:30]:
                    say("| `%s` | %s |" % (tp, pl.replace("\n", " ")[:110]))
                say()
                uniq = sorted({tp for tp, _ in pkts})
                say("- **去重 topic 数：%d**" % len(uniq))
                say()
                say("### 2.1 去重 topic 全量（前 60）")
                say()
                say("```")
                for t in uniq[:60]:
                    say(t)
                say("```")
            else:
                say()
                say("- **没嗅到消息** —— 这条要人工触发一次：去 HA 里点一下开关，然后重跑本脚本。")
        s.close()
    except Exception as e:  # noqa: BLE001
        say("- 连接/嗅探失败：`%s: %s`" % (type(e).__name__, e))
    say()

    _dump(L, args.out)
    return 0


def _dump(lines, out):
    if not out:
        return
    try:
        d = os.path.dirname(os.path.abspath(out))
        if d and not os.path.isdir(d):
            os.makedirs(d)
        with open(out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("\n[已写入] %s" % out)
    except Exception as e:  # noqa: BLE001
        print("[写文件失败] %s" % e)


if __name__ == "__main__":
    sys.exit(main() or 0)
