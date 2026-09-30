#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ha_toggle_once.py - 拨一次开关，**每 200ms 打一行**观察状态怎么变（诊断用）

背景：ha_verify.py 批量跑时报"未变成 on"，但 MQTT 侧明明看到 HA 发了 command、
设备回了 state。本脚本把"单次切换 + 逐步轮询"摊开，定位到底是：
  · 服务调用没生效 → 状态一直不变
  · 生效了但**比我等的慢** → 状态在我放弃之后才变（= 等待窗口设置错误）
  · 生效了但我**读错了**（读的不是同一个实体 / 读到了缓存）

用法：
    set HA_TOKEN=<token>
    python ha_toggle_once.py --entity switch.z20_smart_panel_ke_ting_deng --watch-ms 6000
"""

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def req(method, url, token, data=None, timeout=10):
    hdrs = {"Authorization": "Bearer " + token, "User-Agent": "pg-toggle"}
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        hdrs["Content-Type"] = "application/json"
    r = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    t0 = time.time()
    try:
        with OPENER.open(r, timeout=timeout) as resp:
            return resp.status, resp.read(), int((time.time() - t0) * 1000)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), int((time.time() - t0) * 1000)
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode(), int((time.time() - t0) * 1000)


def get_state(base, token, eid):
    st, raw, ms = req("GET", "%s/api/states/%s" % (base, eid), token)
    if st != 200:
        return None, st, ms, raw.decode("utf-8", "replace")[:120]
    try:
        j = json.loads(raw.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001
        return None, st, ms, "非 JSON"
    return j, st, ms, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("HA_URL", "http://192.168.1.188:8123"))
    ap.add_argument("--token", default=os.environ.get("HA_TOKEN", ""))
    ap.add_argument("--entity", required=True)
    ap.add_argument("--watch-ms", type=int, default=6000)
    ap.add_argument("--step-ms", type=int, default=200)
    ap.add_argument("--no-restore", action="store_true")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass
    if not args.token:
        print("需要 HA_TOKEN")
        return 2

    base = args.url.rstrip("/")
    fp = hashlib.sha256(args.token.encode()).hexdigest()[:8]
    eid = args.entity
    dom = eid.split(".")[0]

    L = []
    def say(s=""):
        print(s)
        L.append(s)

    say("# 单次切换逐步观察：`%s`（令牌指纹 `%s`）" % (eid, fp))
    say()
    cur, st, ms, err = get_state(base, token=args.token, eid=eid)
    if cur is None:
        say("- ❌ `GET /api/states/%s` 失败：HTTP %s `%s`" % (eid, st, err))
        _dump(L, args.out)
        return 3
    orig = cur.get("state")
    say("- 调用前：state = **%s**，`last_changed` = %s，`last_updated` = %s"
        % (orig, cur.get("last_changed"), cur.get("last_updated")))
    say("- 属性：`%s`" % json.dumps(cur.get("attributes", {}), ensure_ascii=False)[:300])
    say()

    want = "off" if orig == "on" else "on"
    svc = "turn_off" if want == "off" else "turn_on"
    say("## 发命令 `%s/%s`" % (dom, svc))
    say()
    t0 = time.time()
    stc, rawc, rt = req("POST", "%s/api/services/%s/%s" % (base, dom, svc),
                        args.token, {"entity_id": eid})
    say("- HTTP **%s**，往返 %d ms" % (stc, rt))
    say("- 响应体：`%s`" % rawc.decode("utf-8", "replace")[:400])
    say()

    say("## 逐步轮询（每 %d ms 一行）" % args.step_ms)
    say()
    say("| t(ms) | HTTP | state | last_updated | 说明 |")
    say("|---|---|---|---|---|")
    changed_at = None
    deadline = time.time() + args.watch_ms / 1000.0
    while time.time() < deadline:
        c, s2, ms2, e2 = get_state(base, args.token, eid)
        t = int((time.time() - t0) * 1000)
        if c is None:
            say("| %d | %s | — | — | `%s` |" % (t, s2, e2))
        else:
            v = c.get("state")
            note = ""
            if v == want and changed_at is None:
                changed_at = t
                note = "★ 变了（命令 → 观测到 = %d ms）" % t
            say("| %d | %s | **%s** | %s | %s |" % (t, s2, v, c.get("last_updated"), note))
        time.sleep(args.step_ms / 1000.0)
    say()
    if changed_at is None:
        say("- ❌ **在 %d ms 窗口内没观测到 `%s`**" % (args.watch_ms, want))
        say("- ⇒ 要么命令真的没生效，要么生效得比窗口还慢 —— 把 `--watch-ms` 调大再试一次。")
    else:
        say("- ✅ **确认生效**：命令 → 观测到 `%s` 用了 **%d ms**" % (want, changed_at))
    say()

    if not args.no_restore:
        back = "turn_on" if orig == "on" else "turn_off"
        stc2, rawc2, rt2 = req("POST", "%s/api/services/%s/%s" % (base, dom, back),
                               args.token, {"entity_id": eid})
        say("## 还原 -> `%s`（HTTP %s，%d ms）" % (back, stc2, rt2))
        say()
        t1 = time.time()
        ok_at = None
        while time.time() - t1 < args.watch_ms / 1000.0:
            c, _, _, _ = get_state(base, args.token, eid)
            if c and c.get("state") == orig:
                ok_at = int((time.time() - t1) * 1000)
                break
            time.sleep(args.step_ms / 1000.0)
        say("- 还原观测：%s" % ("✅ %s 用了 %d ms" % (orig, ok_at) if ok_at is not None
                              else "❌ 没回到 `%s`" % orig))
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
