#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_watch.py - 在 MQTT 上"看着"一次 HA 服务调用到底发生了什么（只读诊断）

为什么需要它：验收时发现 `POST /api/services/switch/turn_on` 返回 **200 但 service
response 是空数组、实体状态也没变**。这有三种可能，必须用 MQTT 侧的证据把它们区分开：

  ① HA 根本没发命令（服务调用没落到 mqtt 集成上）
  ② HA 发了命令，但**设备没回 state** ⇒ 状态不变（MQTT 实体非 optimistic 时就是这样）
  ③ 设备回了 state，但值 HA 不认

做法：订阅 topic → 在指定时刻打一次 HA 服务 → 把期间**所有** MQTT 报文原样打出来。
能看到 `.../command` 就说明 ① 不成立；能看到 `.../state` 回包就说明 ② 不成立。

用法：
    set HA_TOKEN=<token>
    python mqtt_watch.py --mqtt-host 192.168.1.188 --topic "smartpanel/#" \
        --ha-url http://192.168.1.188:8123 --call switch/turn_on \
        --entity switch.z20_smart_panel_ke_ting_deng --seconds 15
"""

import argparse
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


# ---------------- 最小 MQTT 客户端（与 emqx_probe.py 同源） ----------------

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
            raise IOError("连接已断")
        buf += d
    return buf


def mqtt_connect(sock, client_id, user="", pwd="", keepalive=30):
    flags = 0x02
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
    ack = recv_exact(sock, hdr[1] if hdr[1] else 2)
    return ack[-1]


def mqtt_subscribe(sock, topic, qos=0, mid=1):
    body = struct.pack(">H", mid) + mqtt_str(topic) + bytes([qos])
    sock.sendall(bytes([0x82]) + mqtt_varint(len(body)) + body)


def mqtt_loop(sock, seconds, out, stop_evt):
    end = time.time() + seconds
    buf = b""
    while time.time() < end and not stop_evt.is_set():
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
                    out.append((round(time.time() - T0, 2),
                                pkt[2:2 + tl].decode("utf-8", "replace"),
                                pkt[2 + tl:].decode("utf-8", "replace")))
            elif ptype == 9 and len(pkt) >= 3:
                out.append((round(time.time() - T0, 2), "SUBACK",
                            "mid=%d granted=%s" % (struct.unpack(">H", pkt[:2])[0],
                                                   [hex(x) for x in pkt[2:]])))
            elif ptype == 13:
                out.append((round(time.time() - T0, 2), "PINGRESP", ""))


T0 = time.time()


def ha_call(base, token, domain_service, entity, extra):
    url = "%s/api/services/%s" % (base.rstrip("/"), domain_service)
    payload = {"entity_id": entity}
    payload.update(extra)
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Authorization": "Bearer " + token, "Content-Type": "application/json"},
        method="POST")
    try:
        with OPENER.open(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode()


def ha_state(base, token, entity):
    req = urllib.request.Request(
        "%s/api/states/%s" % (base.rstrip("/"), entity),
        headers={"Authorization": "Bearer " + token})
    try:
        with OPENER.open(req, timeout=10) as r:
            return json.loads(r.read().decode())
    except Exception:  # noqa: BLE001
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mqtt-host", default="192.168.1.188")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--mqtt-user", default=os.environ.get("EMQX_USER", ""))
    ap.add_argument("--mqtt-pass", default=os.environ.get("EMQX_PASS", ""))
    ap.add_argument("--topic", default="homeassistant/#")
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--ha-url", default=os.environ.get("HA_URL", "http://192.168.1.188:8123"))
    ap.add_argument("--token", default=os.environ.get("HA_TOKEN", ""))
    ap.add_argument("--call", default="", help="如 switch/turn_on")
    ap.add_argument("--entity", default="", help="如 switch.xxx")
    ap.add_argument("--call-at", type=float, default=3.0, help="第几秒打这次调用")
    ap.add_argument("--extra", default="", help="额外 JSON，如 {\"brightness\":128}")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    L = []
    def say(s=""):
        print(s)
        L.append(s)

    say("# MQTT 侧观察：一次 HA 服务调用到底发出了什么")
    say()
    say("- 订阅 `%s`，观察 %d 秒" % (args.topic, args.seconds))
    if args.call:
        say("- 在第 %.1f 秒调用 `%s` → `%s`" % (args.call_at, args.call, args.entity))
    say()

    msgs = []
    stop = threading.Event()

    sock = socket.create_connection((args.mqtt_host, args.mqtt_port), timeout=6)
    rc = mqtt_connect(sock, "pg-watch-%d" % int(time.time() * 1000),
                      args.mqtt_user, args.mqtt_pass)
    say("- MQTT CONNECT 返回码 **%d**（0 = 接受）" % rc)
    if rc != 0:
        say("**连不上 broker，终止。**")
        _dump(L, args.out)
        return 2

    mqtt_subscribe(sock, args.topic, mid=1)
    th = threading.Thread(target=mqtt_loop, args=(sock, args.seconds, msgs, stop),
                          daemon=True)
    th.start()

    call_result = None
    if args.call and args.token:
        time.sleep(args.call_at)
        before = ha_state(args.ha_url, args.token, args.entity) if args.entity else None
        extra = json.loads(args.extra) if args.extra else {}
        say("- 调用前 `%s` = **%s**" % (args.entity, (before or {}).get("state", "?")))
        st, raw = ha_call(args.ha_url, args.token, args.call, args.entity, extra)
        call_result = (st, raw.decode("utf-8", "replace"))
        say("- 服务调用 HTTP **%s**，响应体：`%s`" % (st, call_result[1][:300]))
        time.sleep(3.0)
        after = ha_state(args.ha_url, args.token, args.entity) if args.entity else None
        say("- 调用后 `%s` = **%s**" % (args.entity, (after or {}).get("state", "?")))
        say()

    th.join(timeout=args.seconds + 5)
    stop.set()
    try:
        sock.close()
    except Exception:  # noqa: BLE001
        pass

    say("## MQTT 报文时序（time / topic / payload）")
    say()
    if not msgs:
        say("**一条都没收到** —— 说明这段时间该前缀下没有任何报文（含保留报文）。")
    else:
        say("| t(s) | topic | payload |")
        say("|---|---|---|")
        for t, tp, pl in msgs:
            say("| %.2f | `%s` | `%s` |" % (t, tp, pl.replace("\n", " ")[:180]))
    say()

    # ---------------- 自动判定 ----------------
    say("## 判定（自动推导）")
    say()
    cmds = [m for m in msgs if m[1].endswith("/command") or m[1].endswith("/set")]
    sts = [m for m in msgs if m[1].endswith("/state")]
    say("| 观察到的 | 数量 | 推论 |")
    say("|---|---|---|")
    say("| 命令类 topic（`*/command`、`*/set`） | %d | %s |"
        % (len(cmds),
           "HA 的命令**确实发出去了** ⇒ 不该怪 HA 的服务调用链路" if cmds
           else "★ **没看到命令** ⇒ HA 可能没把它下发到 MQTT（查 mqtt 集成/实体是否真由 mqtt 提供）"))
    say("| 状态类 topic（`*/state`） | %d | %s |"
        % (len(sts),
           "设备**回了状态** ⇒ 若 HA 仍不变，是 HA 侧映射问题" if sts
           else "★ **设备没回状态** ⇒ 状态不变是「设备没应答」，不是 HA 的问题"))
    say()
    say("- ⇒ %s"
        % ("**结论：命令发得出去、状态没人回** —— 设备侧（面板固件/继电器）没响应；"
           "HA 的 MQTT 实体在非 optimistic 模式下状态只跟 `stat_t` 走，所以必然不变。"
           "UI 侧要按「命令已发 / 未确认」三态来画（待确认 → 超时回滚），不能假设点完就变。"
           if cmds and not sts else
           "见上表两行推论的组合。"))
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
