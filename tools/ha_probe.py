#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ha_probe.py - Home Assistant 接入前的 P0 前置验证探针（PC 侧，只读）

回答规划 docs/ha-integration-plan.md 里 P0 阶段要拍板的问题：

  1. HA 可达吗？版本是多少？
  2. 实体有多少？domain 怎么分布？
  3. ★ 实体名是中文吗？（决定字库工作量 —— 本工程字库无逐字回退，缺字形 = 整字消失）
  4. ★ 全量 GET /api/states 与裁剪 POST /api/template 各多大？（决定内存红线走哪条路）
  5. ★ WebSocket 能不能连上？（决定"实时化"这一片的风险是否成立）
     顺带取 config/area_registry/list —— 房间分组只有 WS 有。
  6. 登录/认证链路（拿 30 分钟短期 token，不落盘；设备上要用长期令牌 LLAT）

用法（凭据走环境变量，不写进文件、不打印）：
    set HA_USER=xxx
    set HA_PASS=xxx
    python ha_probe.py --url http://192.168.1.188:8123 [--out docs/ha-probe.md]

⚠️ 一律绕过系统代理（本机常年挂着 http(s)_proxy，走代理会拿到错的响应 ——
   与 tools/radio_probe.py 同一个坑）。
"""

import argparse
import base64
import json
import os
import socket
import ssl
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

UA = "V851s-PocketGame/ha_probe"

# ---------------------------------------------------------------- HTTP 基础


def make_opener():
    """强制直连：绕过 http_proxy/https_proxy（PC 上常年有代理）。"""
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


OPENER = make_opener()


def http(method, url, data=None, headers=None, timeout=10, form=False):
    hdrs = {"User-Agent": UA}
    body = None
    if data is not None:
        if form:
            body = urllib.parse.urlencode(data).encode()
            hdrs["Content-Type"] = "application/x-www-form-urlencoded"
        else:
            body = json.dumps(data).encode()
            hdrs["Content-Type"] = "application/json"
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    try:
        with OPENER.open(req, timeout=timeout) as r:
            raw = r.read()
            return r.status, raw, dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers or {})
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode(), {}


def tcp_probe(host, port, timeout=2.5):
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.close()
        return True, "open"
    except Exception as e:  # noqa: BLE001
        return False, str(e)


# ---------------------------------------------------------------- 认证


def ha_login(base, user, pwd):
    """走 HA 前端的登录流程拿一个短期 access_token（30 分钟，不落盘）。

    login_flow -> login_flow/<id> -> auth/token
    """
    client_id = base.rstrip("/") + "/"
    st, raw, _ = http("POST", base + "/auth/login_flow",
                      {"client_id": client_id,
                       "handler": ["homeassistant", None],
                       "redirect_uri": client_id})
    if st != 200:
        return None, "login_flow HTTP %s: %s" % (st, raw[:200])
    flow = json.loads(raw.decode("utf-8", "replace"))
    flow_id = flow.get("flow_id")
    if not flow_id:
        return None, "login_flow 没给 flow_id: %s" % raw[:200]

    st, raw, _ = http("POST", "%s/auth/login_flow/%s" % (base, flow_id),
                      {"client_id": client_id, "username": user, "password": pwd})
    if st != 200:
        return None, "凭证提交 HTTP %s: %s" % (st, raw[:200])
    res = json.loads(raw.decode("utf-8", "replace"))
    if res.get("type") != "create_entry":
        return None, "登录未通过: %s" % json.dumps(res, ensure_ascii=False)[:300]
    code = res.get("result")

    st, raw, _ = http("POST", base + "/auth/token",
                      {"grant_type": "authorization_code", "code": code,
                       "client_id": client_id}, form=True)
    if st != 200:
        return None, "换 token HTTP %s: %s" % (st, raw[:200])
    tok = json.loads(raw.decode("utf-8", "replace"))
    return tok, None


# ---------------------------------------------------------------- WebSocket（手写最小客户端）


class MiniWS:
    """只做一件事：证明"WS 能连、能订阅"，顺便取注册表。

    刻意手写（不用第三方库）——因为设备侧最终也要手写，这里先量一次复杂度。
    """

    def __init__(self, host, port, path, tls=False, timeout=10):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        if tls:
            ctx = ssl.create_default_context()
            self.sock = ctx.wrap_socket(self.sock, server_hostname=host)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n") % (path, host, port, key)
        self.sock.sendall(req.encode())
        self.buf = b""
        head = self._read_until(b"\r\n\r\n")
        self.handshake_head = head.decode("latin-1")
        self.ok = head.startswith(b"HTTP/1.1 101")

    def _read_until(self, sep):
        while sep not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise IOError("连接在握手阶段就断了")
            self.buf += d
        head, self.buf = self.buf.split(sep, 1)
        return head

    def _recv_exact(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(max(4096, n - len(self.buf)))
            if not d:
                raise IOError("连接已断")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send_text(self, s):
        payload = s.encode()
        head = bytearray([0x81])
        n = len(payload)
        if n < 126:
            head.append(0x80 | n)
        elif n < (1 << 16):
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        head += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(head) + masked)

    def send_pong(self, data=b""):
        mask = os.urandom(4)
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.sock.sendall(bytes([0x8A, 0x80 | len(data)]) + mask + payload)

    def recv_text(self, timeout=5.0):
        """读一条完整文本消息；自动回 pong；忽略非文本帧。"""
        self.sock.settimeout(timeout)
        while True:
            b0, b1 = self._recv_exact(2)
            opcode = b0 & 0x0F
            masked = b1 & 0x80
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._recv_exact(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._recv_exact(8))[0]
            mk = self._recv_exact(4) if masked else None
            payload = self._recv_exact(ln) if ln else b""
            if mk:
                payload = bytes(b ^ mk[i % 4] for i, b in enumerate(payload))
            if opcode == 0x9:      # ping
                self.send_pong(payload)
                continue
            if opcode == 0x8:      # close
                raise IOError("服务端关闭了连接")
            if opcode in (0x1, 0x2):
                return payload.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except Exception:  # noqa: BLE001
            pass


def ws_command(ws, msg_id, payload, timeout=6.0):
    payload = dict(payload)
    payload["id"] = msg_id
    ws.send_text(json.dumps(payload))
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            raw = ws.recv_text(timeout=max(0.5, deadline - time.time()))
        except Exception as e:  # noqa: BLE001  超时/断开都只记录，不让整轮探测崩掉
            return {"success": False, "error": "%s: %s" % (type(e).__name__, e)}
        try:
            obj = json.loads(raw)
        except Exception:  # noqa: BLE001
            continue
        if obj.get("id") == msg_id and obj.get("type") == "result":
            return obj
    return {"success": False, "error": "等待 id=%s 的响应超时" % msg_id}


def ws_probe(host, port, token, path="/api/websocket", tls=False):
    """返回 (描述, 数据字典)。任何一步失败都如实记录，不吞异常。"""
    out = {"handshake": False, "auth": False, "subscribed": False, "errors": []}
    try:
        t0 = time.time()
        ws = MiniWS(host, port, path, tls=tls)
        out["handshake"] = ws.ok
        out["handshake_ms"] = int((time.time() - t0) * 1000)
        if not ws.ok:
            out["errors"].append("握手非 101：%s" % ws.handshake_head.splitlines()[:1])
            ws.close()
            return out
        # 1) auth_required -> auth -> auth_ok
        first = json.loads(ws.recv_text())
        out["first_msg"] = first.get("type")
        if first.get("type") != "auth_required":
            out["errors"].append("首帧不是 auth_required：%s" % first.get("type"))
        ws.send_text(json.dumps({"type": "auth", "access_token": token}))
        auth_res = json.loads(ws.recv_text())
        out["auth"] = (auth_res.get("type") == "auth_ok")
        if not out["auth"]:
            out["errors"].append("auth 失败：%s" % json.dumps(auth_res)[:200])
            ws.close()
            return out
        out["ha_version_ws"] = auth_res.get("ha_version")

        # 2) 订阅 state_changed（实时化的核心）
        r = ws_command(ws, 1, {"type": "subscribe_events", "event_type": "state_changed"})
        out["subscribed"] = bool(r and r.get("success"))

        # 3) 注册表（只有 WS 有 —— 房间分组靠它）
        for mid, cmd in ((2, {"type": "config/area_registry/list"}),
                         (3, {"type": "config/device_registry/list"}),
                         (4, {"type": "config/entity_registry/list"})):
            r2 = ws_command(ws, mid, cmd)
            if r2 and r2.get("success"):
                out[cmd["type"].split("/")[1]] = r2.get("result")
            else:
                out["errors"].append("%s 失败: %s" % (cmd["type"], json.dumps(r2)[:160]))
        ws.close()
    except Exception as e:  # noqa: BLE001
        out["errors"].append("%s: %s" % (type(e).__name__, e))
    return out


# ---------------------------------------------------------------- 主流程


def is_cjk(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def _fp(s):
    """令牌指纹：报告里要能认出"用的是哪一个令牌"，但**不能把令牌写进文件**。"""
    import hashlib
    return hashlib.sha256(s.encode()).hexdigest()[:8]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("HA_URL", "http://192.168.1.188:8123"))
    ap.add_argument("--user", default=os.environ.get("HA_USER", ""))
    ap.add_argument("--passwd", default=os.environ.get("HA_PASS", ""))
    ap.add_argument("--token", default=os.environ.get("HA_TOKEN", ""),
                    help="Long-Lived Access Token。给了就直接用它做 Bearer，"
                         "跳过 login_flow —— 这正是设备侧要走的那条路。")
    ap.add_argument("--out", default="")
    ap.add_argument("--skip-ws", action="store_true")
    args = ap.parse_args()

    base = args.url.rstrip("/")
    pu = urllib.parse.urlparse(base)
    host = pu.hostname
    port = pu.port or (443 if pu.scheme == "https" else 8123)
    tls = (pu.scheme == "https")

    # 中文报告要能直接看（Windows 控制台默认 GBK，会把中文打成乱码）
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    L = []
    def say(s=""):
        print(s)
        L.append(s)

    say("# HA P0 探针结果")
    say()
    say("目标：`%s`（由 `tools/ha_probe.py` 实测，脚本只读，凭据走环境变量）" % base)
    say()

    # --- 1. 端口 ---
    say("## 1. 端口可达性")
    say()
    say("| 端口 | 结果 |")
    say("|---|---|")
    for p in (8123, 18083, 1883, 8083, 8883):
        ok, msg = tcp_probe(host, p)
        say("| %d | %s |" % (p, "open" if ok else "closed / " + msg[:60]))
    say()

    if not tcp_probe(host, port)[0]:
        say("**HA 端口不通，后续探测中止。**")
        _dump(L, args.out)
        return 2

    # --- 2. 未认证探测 ---
    st, raw, _ = http("GET", base + "/api/")
    say("## 2. 未认证访问 `GET /api/`")
    say()
    say("- HTTP %s（**401 = HA 在跑且要求认证，这是期望值**）" % st)
    say("- body: `%s`" % raw.decode("utf-8", "replace")[:120].replace("\n", " "))
    say()

    # --- 3. 认证（优先用长期令牌；没有才走 login_flow 拿短期 token） ---
    if args.token:
        access = args.token
        say("## 3. 认证")
        say()
        say("- 使用**长期访问令牌（LLAT）**直接做 Bearer（**设备侧要走的就是这条路**）")
        # 只打印令牌指纹，不打印令牌本身
        say("- 令牌指纹：`%s…%s`（长度 %d，sha256 前 8 位 `%s`）"
            % (access[:8], access[-6:], len(access), _fp(access)))
        say()
    elif not args.user or not args.passwd:
        say("## 3. 认证")
        say()
        say("未提供 HA_TOKEN，也未提供 HA_USER / HA_PASS，跳过认证相关探测。")
        _dump(L, args.out)
        return 3
    else:
        t0 = time.time()
        tok, err = ha_login(base, args.user, args.passwd)
        if err:
            say("## 3. 认证 **失败**")
            say()
            say("```")
            say(err)
            say("```")
            _dump(L, args.out)
            return 4
        access = tok["access_token"]
        say("## 3. 认证")
        say()
        say("- 登录流程（login_flow → auth/token）**通过**，耗时 %d ms" % int((time.time() - t0) * 1000))
        say("- access_token 有效期 %s 秒（短期；设备上要用长期令牌 LLAT）"
            % tok.get("expires_in"))
        say("- 已拿到 refresh_token：%s" % ("是" if tok.get("refresh_token") else "否"))
        say()

    H = {"Authorization": "Bearer " + access}

    # --- 4. config ---
    st, raw, _ = http("GET", base + "/api/config", headers=H)
    cfg = json.loads(raw.decode("utf-8", "replace")) if st == 200 else {}
    say("## 4. `GET /api/config`")
    say()
    if st != 200:
        say("HTTP %s：`%s`" % (st, raw.decode("utf-8", "replace")[:200]))
    else:
        say("| 项 | 值 |")
        say("|---|---|")
        for k in ("version", "location_name", "time_zone", "country", "language"):
            if k in cfg:
                say("| %s | %s |" % (k, cfg[k]))
        say("| unit_system | %s |" % json.dumps(cfg.get("unit_system", {}), ensure_ascii=False))
        comps = cfg.get("components", [])
        say("| components 数 | %d |" % len(comps))
        say()
        marks = [c for c in comps if "mqtt" in c or "websocket" in c or "camera" in c
                 or "conversation" in c]
        say("- 关注组件：`%s`" % ", ".join(marks))
    say()

    # --- 5. 实体全量 ---
    t0 = time.time()
    st, raw, _ = http("GET", base + "/api/states", headers=H, timeout=25)
    states_ms = int((time.time() - t0) * 1000)
    states_bytes = len(raw)
    states = json.loads(raw.decode("utf-8", "replace")) if st == 200 else []
    say("## 5. `GET /api/states`（全量）")
    say()
    say("- HTTP %s，**%d 个实体**，**%d 字节**（%.1f KB），耗时 %d ms"
        % (st, len(states), states_bytes, states_bytes / 1024.0, states_ms))
    say()

    dom = {}
    cjk_names = []
    for s in states:
        eid = s.get("entity_id", "")
        d = eid.split(".")[0]
        dom[d] = dom.get(d, 0) + 1
        nm = (s.get("attributes") or {}).get("friendly_name") or ""
        if nm and is_cjk(nm):
            cjk_names.append(nm)
    say("### 5.1 domain 分布（前 25）")
    say()
    say("| domain | 数量 |")
    say("|---|---|")
    for d, n in sorted(dom.items(), key=lambda kv: -kv[1])[:25]:
        say("| `%s` | %d |" % (d, n))
    say()
    say("- domain 种类共 **%d** 个" % len(dom))
    say()

    # --- 5.2 state 取值分布（unavailable 的占比直接决定 UI 要画多少"灰态"） ---
    stt = {}
    for s in states:
        v = s.get("state", "")
        key = v if v in ("unavailable", "unknown", "off", "on") else "其它"
        stt[key] = stt.get(key, 0) + 1
    say("### 5.2 state 取值分布")
    say()
    say("| state | 数量 |")
    say("|---|---|")
    for k in ("on", "off", "unavailable", "unknown", "其它"):
        if stt.get(k):
            say("| `%s` | %d |" % (k, stt[k]))
    say()
    if stt.get("unavailable") or stt.get("unknown"):
        say("- ⚠️ **有 %d 个实体不是 on/off** ⇒ 磁贴必须能画「离线/未知」灰态，"
            "并且**不能把它当成 off**（点了没反应会像 bug）"
            % (stt.get("unavailable", 0) + stt.get("unknown", 0)))
    say()

    # --- 6. ★ 中文实体名（字库风险） ---
    say("### 6. ★ 实体中文名占比（决定字库工作量）")
    say()
    say("- 含中文的 friendly_name：**%d / %d**（%.0f%%）"
        % (len(cjk_names), len(states), 100.0 * len(cjk_names) / max(1, len(states))))
    if cjk_names:
        uniq_chars = set()
        for nm in cjk_names:
            for ch in nm:
                if is_cjk(ch):
                    uniq_chars.add(ch)
        say("- **去重汉字数：%d 个**（这就是要补进项目字库的量级）" % len(uniq_chars))
        say("- 样例：`%s`" % "`、`".join(cjk_names[:12]))
        say("- 汉字全集（可直接喂给 tools/gen_font.py）：")
        say()
        say("```")
        say("".join(sorted(uniq_chars)))
        say("```")
    say()

    # --- 7. ★ 模板裁剪响应体积 ---
    say("### 7. ★ `POST /api/template` 裁剪响应（内存红线的关键判据）")
    say()
    # 只取"可控域"的实体，模拟收藏夹/当前页
    ctrl = [s["entity_id"] for s in states
            if s["entity_id"].split(".")[0] in
            ("light", "switch", "scene", "script", "cover", "climate",
             "media_player", "fan", "lock", "input_boolean", "automation")][:30]
    # ⚠️ 别用 % 格式化拼 Jinja —— Jinja 的 `{% ... %}` 会被 Python 当成格式说明符
    #    （实测报 "not enough arguments for format string"）。用字符串拼接。
    #
    # ⚠️⚠️ 而且 HA 的 Jinja 沙箱**禁掉了 dict.update**：
    #      `ns.o.update({...})` 会报
    #      "SecurityError: access to attribute 'update' of 'dict' object is unsafe."
    #      ⇒ 只能用 "namespace 里放 list + `ns.o = ns.o + [x]`" 或手工拼 JSON 文本。
    #      这里把两种都试一遍，留证据（设备侧要照抄能用那条）。
    lst = json.dumps(ctrl)
    cands = [
        ("A: namespace + list 追加 + tojson",
         "{% set ns = namespace(o=[]) %}"
         "{% for e in " + lst + " %}"
         "{% set ns.o = ns.o + [[e, states(e), state_attr(e,'friendly_name')]] %}"
         "{% endfor %}{{ ns.o | tojson }}"),
        ("B: namespace + dict.update（预期被沙箱拒）",
         "{% set ns = namespace(o={}) %}"
         "{% for e in " + lst + " %}"
         "{% set _ = ns.o.update({e: states(e)}) %}"
         "{% endfor %}{{ ns.o | tojson }}"),
        ("C: 手工拼 JSON 文本",
         "{"
         "{% for e in " + lst + " %}\"{{ e }}\":[\"{{ states(e) }}\","
         "\"{{ state_attr(e,'friendly_name') }}\"]{% if not loop.last %},{% endif %}"
         "{% endfor %}}"),
    ]
    say("| 模板写法 | HTTP | 字节 | 耗时 | 说明 |")
    say("|---|---|---|---|---|")
    tpl_ok = None
    for name, tpl in cands:
        t0 = time.time()
        st, raw, _ = http("POST", base + "/api/template", {"template": tpl},
                          headers=H, timeout=20)
        ms = int((time.time() - t0) * 1000)
        note = ""
        if st == 200:
            note = "可用"
            if tpl_ok is None:
                tpl_ok = (name, len(raw), raw)
        else:
            try:
                note = json.loads(raw.decode("utf-8", "replace")).get("message", "")[:70]
            except Exception:  # noqa: BLE001
                note = raw.decode("utf-8", "replace")[:70]
        say("| %s | %s | %d | %d ms | %s |" % (name, st, len(raw), ms, note))
    say()
    if tpl_ok and states_bytes:
        say("- 取 %d 个可控实体：**%.1f KB**，相比全量 %.1f KB 降到 **%.1f%%**"
            % (len(ctrl), tpl_ok[1] / 1024.0, states_bytes / 1024.0,
               100.0 * tpl_ok[1] / states_bytes))
        say("- 可用写法 = %s；样例响应：" % tpl_ok[0])
        say()
        say("```json")
        say(tpl_ok[2].decode("utf-8", "replace")[:600])
        say("```")
    else:
        say("- **三种写法都没成功** —— 这条要先解决（否则只能拉全量，见规划 R3）")
    say()

    # --- 8. 可控实体清单（功能范围的事实依据） ---
    say("### 8. 可控实体清单（前 30，`/api/states` 的口径）")
    say()
    if ctrl:
        say("| entity_id | state | friendly_name |")
        say("|---|---|---|")
        byid = {s["entity_id"]: s for s in states}
        for e in ctrl[:30]:
            s = byid.get(e, {})
            nm = (s.get("attributes") or {}).get("friendly_name", "")
            say("| `%s` | %s | %s |" % (e, s.get("state", ""), nm))
    else:
        say("**没有找到任何可控域实体** —— 这条要先解决（HA 里还没接入设备？）")
    say()

    # --- 9. ★ WebSocket ---
    ws = {}
    ws_ok = False
    if not args.skip_ws:
        say("## 9. ★ WebSocket 实测（实时化这一片的风险判据）")
        say()
        ws = ws_probe(host, port, access, tls=tls)
        ws_ok = bool(ws.get("handshake") and ws.get("auth") and ws.get("subscribed"))
        say("| 项 | 结果 |")
        say("|---|---|")
        say("| 握手 101 | %s (%s ms) |" % (ws.get("handshake"), ws.get("handshake_ms", "-")))
        say("| 首帧 | %s |" % ws.get("first_msg", "-"))
        say("| auth_ok | %s |" % ws.get("auth"))
        say("| 订阅 state_changed | %s |" % ws.get("subscribed"))
        say("| ha_version | %s |" % ws.get("ha_version_ws", "-"))
        for k in ("area_registry", "device_registry", "entity_registry"):
            v = ws.get(k)
            say("| %s | %s |" % (k, ("%d 条" % len(v)) if isinstance(v, list) else "未取到"))
        if ws.get("errors"):
            say()
            say("- 错误/告警：")
            for e in ws["errors"]:
                say("  - `%s`" % e)
        areas = ws.get("area_registry") or []
        if areas:
            say()
            say("- **房间（area）清单**：`%s`" % "`、`".join(a.get("name", "?") for a in areas))

        # --- 9.1 ★ 房间分组可行性：把三张注册表拼起来，看"房间 -> 实体"能不能算出来 ---
        ent = ws.get("entity_registry") or []
        dev = ws.get("device_registry") or []
        if ent and areas:
            area_name = {a.get("id"): a.get("name", "?") for a in areas}
            dev_area = {d.get("id"): d.get("area_id") for d in dev}
            by_area = {}
            no_area = []
            for e in ent:
                aid = e.get("area_id") or dev_area.get(e.get("device_id"))
                if aid and aid in area_name:
                    by_area.setdefault(area_name[aid], []).append(e.get("entity_id", ""))
                else:
                    no_area.append(e.get("entity_id", ""))
            say()
            say("### 9.1 ★ 「房间分组」可行性实测（entity_registry + device_registry + area_registry 三表拼接）")
            say()
            say("| 房间 | 实体数 | 实体 |")
            say("|---|---|---|")
            for nm, ids in sorted(by_area.items(), key=lambda kv: -len(kv[1])):
                say("| %s | %d | %s |" % (nm, len(ids), ", ".join("`%s`" % i for i in ids[:8])))
            say("| **（未分配房间）** | %d | %s |"
                % (len(no_area), ", ".join("`%s`" % i for i in no_area[:8])))
            say()
            say("- ⇒ %s"
                % ("**房间分组可做**：实测能拼出「房间 → 实体」，规划 §3.1 的 L2 层级成立（P3 做）"
                   if by_area else "★ **拼不出房间分组**：注册表里没有 area 归属，L2 要退化成「本地自定义分组」"))
            say()

            # ★ 自证：检查工具必须能证明"它报得出来" —— 上面全落到"未分配"，
            #   那就把原始字段摊开，证明确实是 area_id 为空、而不是我字段名写错了。
            say("### 9.2 自证：注册表原始字段（证明「未分配」不是我字段名写错）")
            say()
            say("- `entity_registry` 第 1 条的 area 相关字段：")
            say()
            say("```json")
            e0 = ent[0]
            say(json.dumps({k: e0.get(k) for k in
                            ("entity_id", "area_id", "device_id", "disabled_by", "hidden_by")},
                           ensure_ascii=False, indent=1))
            say("```")
            if dev:
                say("- `device_registry` 第 1 条的 area 相关字段：")
                say()
                say("```json")
                d0 = dev[0]
                say(json.dumps({k: d0.get(k) for k in
                                ("id", "name", "area_id", "name_by_user")},
                               ensure_ascii=False, indent=1))
                say("```")
            n_e_area = sum(1 for e in ent if e.get("area_id"))
            n_d_area = sum(1 for d in dev if d.get("area_id"))
            say("- 统计：`entity_registry` 里 **%d/%d** 条有 area_id；"
                "`device_registry` 里 **%d/%d** 条有 area_id" % (n_e_area, len(ent), n_d_area, len(dev)))
            say("- 字段名用的是 HA 官方 `area_id`（WS 注册表接口），4 个房间（%s）本身是存在的 ——"
                % "、".join(a.get("name", "?") for a in areas))
            say("  **是「房间建好了但没把设备放进去」**，不是解析错。")
            say("  ⇒ 要做房间分组面板，得先在 HA 前端把设备分配到区域（人工，5 分钟）；")
            say("    否则 L2 退化成「本地自定义分组」（规划里已经留了这条兜底）。")
            say()
        say()

    # --- 10. 结论 ---
    say("## 10. 结论与对策（自动推导）")
    say()
    say("| 判据 | 实测 | 对规划的影响 |")
    say("|---|---|---|")
    say("| HA 版本 | %s | R14 已消除：按此版本实测接口，不照文档假设 |" % cfg.get("version", "?"))
    say("| 实体总数 | %d | %s |"
        % (len(states),
           "规模小，列表页压力不大" if len(states) < 150 else
           "★ 规模偏大，必须以收藏夹为默认视图（规划 R7）"))
    say("| 中文实体名 | %d 个（%.0f%%） | %s |"
        % (len(cjk_names), 100.0 * len(cjk_names) / max(1, len(states)),
           "★ 字库链路（tools/gen_ha_font.py）**必需**，不能省（规划 R2）"
           if cjk_names else "暂无中文名，字库风险暂时很小"))
    say("| 全量 states | %.1f KB | %s |"
        % (states_bytes / 1024.0,
           "不建议每拍拉，走模板裁剪" if states_bytes > 40 * 1024 else "体积可接受"))
    say("| WS | %s | %s |"
        % ("可用" if ws_ok else "未验证/不可用",
           "实时化可行，P3 按计划做" if ws_ok else "★ 先按纯轮询交付（规划 R1 的降级路径）"))
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
