#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ha_set.py - 显式把实体设成 on/off（不做翻转），带**回读确认**

为什么需要它：`ha_toggle_once.py` 是翻转语义，验收脚本跑完把设备拨乱了之后，
要"还原到某个确切状态"就只能用它。另外它是"设值 + 回读确认"的最小范例 ——
设备侧实现控制时就是这套：**发命令 → 短轮询回读 → 超时不认**。

用法：
    set HA_TOKEN=<token>
    python ha_set.py switch.a=off switch.b=on switch.c=on
"""

import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http(method, url, token, data=None, timeout=10):
    hdrs = {"Authorization": "Bearer " + token, "User-Agent": "pg-set"}
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        hdrs["Content-Type"] = "application/json"
    r = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    try:
        with OPENER.open(r, timeout=timeout) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode()


def read_state(base, token, eid):
    st, raw = http("GET", "%s/api/states/%s" % (base, eid), token)
    if st != 200:
        return None
    try:
        return json.loads(raw.decode("utf-8", "replace")).get("state")
    except Exception:  # noqa: BLE001
        return None


def main():
    base = os.environ.get("HA_URL", "http://192.168.1.188:8123").rstrip("/")
    token = os.environ.get("HA_TOKEN", "")
    pairs = sys.argv[1:]
    if not token:
        print("需要 HA_TOKEN")
        return 2
    if not pairs:
        print("用法：ha_set.py <entity=on|off> [...]")
        return 2

    print("令牌指纹 %s" % hashlib.sha256(token.encode()).hexdigest()[:8])
    rc = 0
    for p in pairs:
        if "=" not in p:
            print("跳过无法解析的项：%s" % p)
            rc = 2
            continue
        eid, val = p.split("=", 1)
        val = val.strip().lower()
        dom = eid.split(".")[0]
        want = "on" if val in ("on", "1", "true") else "off"
        svc = "turn_on" if want == "on" else "turn_off"

        before = read_state(base, token, eid)
        st, raw = http("POST", "%s/api/services/%s/%s" % (base, dom, svc),
                       token, {"entity_id": eid})
        resp = raw.decode("utf-8", "replace")
        t0 = time.time()
        got = None
        while time.time() - t0 < 5.0:
            cur = read_state(base, token, eid)
            if cur == want:
                got = int((time.time() - t0) * 1000)
                break
            time.sleep(0.15)
        ok = got is not None
        if not ok:
            rc = 1
        print("[%s] %s : %s -> %s  HTTP %s  响应=%s  回读=%s%s"
              % ("OK" if ok else "FAIL", eid, before, want, st, resp[:60],
                 read_state(base, token, eid),
                 ("  确认耗时 %d ms" % got) if ok else "  ❌ 5s 内没变成目标值"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
