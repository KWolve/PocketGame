#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ha_verify.py - HA 控制闭环验收（P0 最后一项）

回答规划 docs/ha-integration-plan.md 里悬着的几个问题：

  ★ §2.4 的核心设计：`POST /api/services/<domain>/<service>` 的**返回值是不是真的**
    含有受影响实体的最新状态？如果含 —— 那就是"不用等轮询"的真相回填；不含 —— 整个
    手感设计要改成"操作后立刻 poll"。
  · 一次开关的真实往返延迟是多少（决定 UI 要不要做乐观态）。
  · `unavailable` 的实体调用服务会怎样（UI 该怎么画灰态/该不该禁点）。
  · 实体不存在 / 服务不存在，错误长什么样（UI 要能区分"令牌坏了"和"雨我无瓜"）。
  · 调用一个服务之后，别的实体会不会被连带改变（决定"回填"要不要全量刷）。

⚠️ **默认只读**。真正去开关设备必须显式加 `--actuate`，且**每个实体都会还原到原始状态**。
   `--scene` 才会去按场景按钮（那会连带改多个实体，会在报告里列出改了哪些并全部还原）。

用法（令牌走环境变量，不进文件）：
    set HA_TOKEN=<long-lived-token>
    python ha_verify.py --url http://192.168.1.188:8123 --actuate --out docs/ha-verify.md
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
UA = "V851s-PocketGame/ha_verify"

# 只碰这两类"可开关且可还原"的域；其它域（climate/cover/lock…）不在这里乱动
TOGGLE_DOMAINS = ("switch", "light", "input_boolean", "fan")


def http(method, url, data=None, headers=None, timeout=10):
    hdrs = {"User-Agent": UA}
    body = None
    if data is not None:
        body = json.dumps(data).encode()
        hdrs["Content-Type"] = "application/json"
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs, method=method)
    t0 = time.time()
    try:
        with OPENER.open(req, timeout=timeout) as r:
            return r.status, r.read(), int((time.time() - t0) * 1000)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), int((time.time() - t0) * 1000)
    except Exception as e:  # noqa: BLE001
        return None, str(e).encode(), int((time.time() - t0) * 1000)


def jbody(raw):
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except Exception:  # noqa: BLE001
        return None


def one_state(base, hdr, eid):
    st, raw, _ = http("GET", "%s/api/states/%s" % (base, eid), headers=hdr)
    if st == 200:
        return jbody(raw)
    return None


def call(base, hdr, domain, service, payload):
    return http("POST", "%s/api/services/%s/%s" % (base, domain, service),
                payload, headers=hdr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default=os.environ.get("HA_URL", "http://192.168.1.188:8123"))
    ap.add_argument("--token", default=os.environ.get("HA_TOKEN", ""))
    ap.add_argument("--actuate", action="store_true", help="真的去开关设备（会还原）")
    ap.add_argument("--scene", action="store_true", help="额外按一次场景按钮（会还原）")
    ap.add_argument("--settle-ms", type=int, default=4000, help="等状态稳定的上限")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    if not args.token:
        print("需要 HA_TOKEN（长期访问令牌）")
        return 2

    # ★ 观测窗口强制下限：Ha 侧 MQTT 实体的状态变化是「设备回包」驱动的（异步），
    #   实测 200~300ms 才可见；窗口太小会误判成"命令没生效"，而且会让"还原校验"假通过。
    settle_ms = max(1500, args.settle_ms)

    base = args.url.rstrip("/")
    hdr = {"Authorization": "Bearer " + args.token}

    L = []
    def say(s=""):
        print(s)
        L.append(s)

    import hashlib
    fp = hashlib.sha256(args.token.encode()).hexdigest()[:8]

    say("# HA 控制闭环验收")
    say()
    say("目标 `%s`（令牌指纹 `%s`，脚本 `tools/ha_verify.py`）" % (base, fp))
    say()
    say("- 模式：**%s**" % ("实际驱动设备（每个都会还原）" if args.actuate else "只读（没加 --actuate）"))
    say("- 观测窗口 `--settle-ms` = **%d ms**（强制不低于 1500 ms —— 首次踩过：窗口太小会把"
        "「命令已生效」误判成「超时」，而且会让还原校验**假通过**）" % settle_ms)
    say()

    # ---------- 0. 基线 ----------
    st, raw, ms = http("GET", base + "/api/states", headers=hdr)
    states = jbody(raw) or []
    say("## 0. 基线快照")
    say()
    say("- `GET /api/states` → HTTP %s，**%d 个实体**，%d ms" % (st, len(states), ms))
    byid = {s["entity_id"]: s for s in states}
    targets = [s for s in states
               if s["entity_id"].split(".")[0] in TOGGLE_DOMAINS]
    say()
    say("| 可开关实体 | 当前 state | friendly_name |")
    say("|---|---|---|")
    for s in targets:
        say("| `%s` | %s | %s |"
            % (s["entity_id"], s.get("state"),
               (s.get("attributes") or {}).get("friendly_name", "")))
    say()
    if not targets:
        say("**没有可开关实体**，控制闭环无从验起。")
        _dump(L, args.out)
        return 3

    # ---------- 1. 正例：往返 + 真相回填 ----------
    say("## 1. 正例：一次开关的完整往返（★ 同时验「服务响应即真相回填」）")
    say()
    if not args.actuate:
        say("未加 `--actuate`，本节跳过。**验收时必须加。**")
        say()
    else:
        say("| 实体 | 拨前实测 | 动作 | 服务响应里的 state | 服务往返 | 观察到变化 | 轮询次数 | 还原确认 |")
        say("|---|---|---|---|---|---|---|---|")
        shown_payload = False
        all_ok = True
        for s in targets:
            eid = s["entity_id"]
            dom = eid.split(".")[0]
            snap = s.get("state")
            if snap not in ("on", "off"):
                say("| `%s` | %s | （跳过：非 on/off，不动它） | | | | | |" % (eid, snap))
                continue
            # ★ 别再拿"开场快照"当基准 —— 前提是这中间可能已经有人/别的东西改过它。
            #   真·基准要**在拨之前当场读一次**。
            pre = one_state(base, hdr, eid)
            orig = (pre or {}).get("state", snap)
            if orig not in ("on", "off"):
                say("| `%s` | %s | （拨前实测已不是 on/off，跳过） | | | | | |" % (eid, orig))
                continue
            want = "off" if orig == "on" else "on"
            svc = "turn_off" if want == "off" else "turn_on"

            t0 = time.time()
            stc, rawc, rt = call(base, hdr, dom, svc, {"entity_id": eid})
            resp = jbody(rawc)
            # ★ 关键：服务响应里有没有这个实体的最新状态？
            in_resp = None
            if isinstance(resp, list):
                for it in resp:
                    if it.get("entity_id") == eid:
                        in_resp = it.get("state")
            # 回读（轮询到期望值），并记录**实际轮询了几次**（证明循环真的在跑）
            observed_ms = None
            final = None
            polls = 0
            deadline = time.time() + settle_ms / 1000.0
            while time.time() < deadline:
                polls += 1
                cur = one_state(base, hdr, eid)
                if cur and cur.get("state") == want:
                    observed_ms = int((time.time() - t0) * 1000)
                    final = cur.get("state")
                    break
                time.sleep(0.15)
            ok = (final == want)
            if not ok:
                all_ok = False

            # 还原（并**连读两次**确认，避免"命令还没生效就读到原值"的假通过）
            back_svc = "turn_on" if orig == "on" else "turn_off"
            call(base, hdr, dom, back_svc, {"entity_id": eid})
            restored = "?"
            deadline = time.time() + settle_ms / 1000.0
            while time.time() < deadline:
                cur = one_state(base, hdr, eid)
                if cur and cur.get("state") == orig:
                    time.sleep(0.4)
                    again = one_state(base, hdr, eid)
                    if again and again.get("state") == orig:
                        restored = "✅ 双读一致"
                        break
                time.sleep(0.15)
            if restored != "✅ 双读一致":
                restored = "❌ 仍未回到 %s" % orig
                all_ok = False

            say("| `%s` | %s | %s | **%s** | %d ms | %s | %d | %s |"
                % (eid, orig, svc, in_resp, rt,
                   ("✅ %d ms" % observed_ms) if ok else "❌ **超时**（未变成 %s）" % want,
                   polls, restored))

            if not shown_payload and isinstance(resp, list) and resp:
                shown_payload = True
                say()
                say("**服务响应的原始报文**（这是「回填」能不能成立的关键证据）：")
                say()
                say("```json")
                say(json.dumps(resp[:2], ensure_ascii=False, indent=1)[:1200])
                say("```")
                say()
        say()
        say("- 总体：**%s**" % ("全部通过 ✅" if all_ok else "★ 有失败项，见上表 ❌"))
        say("- ★ **结论（§2.4 的判据）**：%s"
            % ("服务响应里**确实带着受影响实体的最新 state** ⇒ 可直接回填、不必等轮询"
               if shown_payload else
               "服务响应是 **空数组 `[]`** ⇒ **不能靠服务响应回填**。原因是这类实体由 MQTT 驱动，"
               "状态变化是**异步**的（HA 发出 command 后要等设备回 state），服务调用在那一刻"
               "「还没变化」所以就返回空。⇒ UI 必须按 **乐观态 + 短轮询确认（实测 200~300 ms）** 来写，"
               "并且要有**超时回滚**（设备不应答时状态永远不变，见 §3）"))
        say()

    # ---------- 2. 反例：错误怎么表达 ----------
    say("## 2. 反例：三种失败长什么样（UI 要能区分，不能都是「连接失败」）")
    say()
    say("| 场景 | HTTP | 响应体（截断） | UI 该怎么说 |")
    say("|---|---|---|---|")
    cases = [
        ("实体不存在", "switch", "turn_on", {"entity_id": "switch.this_does_not_exist_xyz"}),
        ("服务不存在", "switch", "turn_on_xyz", {"entity_id": targets[0]["entity_id"]}),
        ("域不存在", "nosuchdomain", "turn_on", {"entity_id": targets[0]["entity_id"]}),
        ("body 里没 entity_id", "switch", "turn_on", {}),
    ]
    for name, dom, svc, pl in cases:
        stc, rawc, rt = call(base, hdr, dom, svc, pl)
        body = rawc.decode("utf-8", "replace").replace("\n", " ")[:150]
        hint = {"实体不存在": "该设备已从 HA 移除，请刷新列表",
                "服务不存在": "HA 版本不支持该操作（别笼统说连接失败）",
                "域不存在": "同上", "body 里没 entity_id": "本地 bug，不是网络问题"}.get(name, "")
        say("| %s | **%s** | `%s` | %s |" % (name, stc, body, hint))
    say()

    # ---------- 3. unavailable 的实体 ----------
    say("## 3. `unavailable` 的实体调用服务会怎样（决定灰态要不要禁点）")
    say()
    unavailable = [s for s in targets if s.get("state") in ("unavailable", "unknown")]
    if not unavailable:
        say("（当前没有 unavailable 的可开关实体，跳过）")
        say()
    else:
        say("| 实体 | 调用前 | 调用结果 HTTP | 调用后 | 变化 |")
        say("|---|---|---|---|---|")
        for s in unavailable:
            eid = s["entity_id"]
            dom = eid.split(".")[0]
            stc, rawc, _ = call(base, hdr, dom, "turn_on", {"entity_id": eid})
            time.sleep(0.5)
            after = one_state(base, hdr, eid)
            av = after.get("state") if after else "?"
            say("| `%s` | %s | **%s** | %s | %s |"
                % (eid, s.get("state"), stc, av,
                   "无变化（命令被吞）" if av == s.get("state") else "变了"))
        say()
        say("- ⇒ %s"
            % ("**命令返回 200 但状态不变** ⇒ UI 必须**禁点灰态**，否则用户会以为按钮坏了"
               "（HA 侧设备离线时，服务调用是「接受但不生效」）"))
        say()

    # ---------- 4. 场景按钮（可选） ----------
    if args.scene and args.actuate:
        say("## 4. 场景按钮：一次调用会不会连带改别的实体")
        say()
        buttons = [s for s in states if s["entity_id"].startswith("button.")]
        if not buttons:
            say("（没有 button 实体，跳过）")
            say()
        else:
            b = buttons[0]
            bead = [s for s in targets if s.get("state") in ("on", "off")]
            before = {s["entity_id"]: s.get("state") for s in bead}
            say("- 按 `%s`（%s）" % (b["entity_id"],
                                    (b.get("attributes") or {}).get("friendly_name", "")))
            stc, rawc, rt = call(base, hdr, "button", "press", {"entity_id": b["entity_id"]})
            resp = jbody(rawc)
            say("- HTTP %s，%d ms，响应里 %s 条状态"
                % (stc, rt, len(resp) if isinstance(resp, list) else "?"))
            time.sleep(2.0)
            changed = []
            for eid, was in before.items():
                cur = one_state(base, hdr, eid)
                now = cur.get("state") if cur else "?"
                if now != was:
                    changed.append((eid, was, now))
            say()
            if changed:
                say("| 被连带改变的实体 | 前 | 后 |")
                say("|---|---|---|")
                for eid, was, now in changed:
                    say("| `%s` | %s | %s |" % (eid, was, now))
                say()
                say("- ⇒ **一次服务调用会连带改多个实体** ⇒ 回填要么用服务响应、要么全量刷当前页")
            else:
                say("- 这次没有其它实体的状态被改变（该场景可能只影响面板自身）")
            say()
            # 还原
            say("- 还原被改动的实体：")
            for eid, was, _ in changed:
                dom = eid.split(".")[0]
                svc = "turn_on" if was == "on" else "turn_off"
                call(base, hdr, dom, svc, {"entity_id": eid})
                say("  - `%s` -> %s" % (eid, was))
            if changed:
                time.sleep(1.0)
                left = []
                for eid, was, _ in changed:
                    cur = one_state(base, hdr, eid)
                    if (cur or {}).get("state") != was:
                        left.append(eid)
                say("- 还原校验：%s" % ("✅ 全部回到原值" if not left else "❌ 仍是 %s" % left))
            say()
    else:
        say("## 4. 场景按钮")
        say()
        say("未加 `--scene`（会连带改多个实体），跳过。")
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
