---
id: devflow-open-source-stack-integration
title: 开源协议栈/第三方库怎么接进 FlyThings 工程（非 GUI 生态借用）
category: devflow
status: draft
confidence: manual
verified_at: 2026-09-29
stale_days: 180
origin: total
source: 2026-09-29 PocketGame（V851s）实战归纳 + MCP 包卡/packages 实测口径
needs_evidence: true
platforms: [V85X, Z20, Z21, F133, T113]
tags: [开源库, 第三方库, 协议栈, curl, mbedtls, openssl, mqtt, sqlite, libc, musl, glibc, 静态库, dlopen, lib-no-link]
evidence: []
---
# 开源协议栈/第三方库怎么接进 FlyThings 工程（非 GUI 生态借用）

> **检索导引**：FlyThings 怎么用开源库 / 想用某个协议栈（HTTP、TLS、MQTT、WebSocket、JSON、SQLite、
> Modbus、OPC-UA、DNS、压缩）怎么办 / registry 里没有这个包 / 能不能用现成的开源 .a/.so /
> 自己交叉编译的库怎么进工程 / dlopen 找不到库 / 静态库太大 / 库放进 lib-no-link /
> **libc 不匹配 / musl 与 glibc 混用 / 预编译库跑不起来 / undefined reference** /
> 有没有类似的包可以用 / 不用厂家的包行不行。

## 0. 一句话

**FlyThings 应用就是一个普通 Linux 进程**（自绘 framebuffer + 自己的消息循环），
所以"非 GUI 的开源生态基本全都能用"——**但能不能用不取决于功能，取决于四道判据**：
**libc 匹配 → ABI/符号 → 体积预算 → 路径可见性**。跳过判据直接链接，症状往往是"编译过、设备跑不起来"。

---

## 1. 接入路径（按优先级，逐级下降）

```
① registry 包（首选）      flythings_list_packages / packages/<包>/package.yaml（有真机验证卡）
        ↓ 没有
② 本地库目录（自编/自取）   src/dependencies/lib/*.a|*.so  → fun 自动扫描并链接
                           src/dependencies/include/ 、src/ 子目录 → include 路径
        ↓ 只随包不链接
③ lib-no-link/            打包进镜像 lib/ → 设备上 /res/lib（在 LD_LIBRARY_PATH 内）
        ↓ 设备自带
④ 系统 .so + dlopen       ls /lib /res/lib 确认存在 → dlopen（版本随固件，不可控）
        ↓ 纯头文件
⑤ 源码直放                 stb / cJSON / miniz 这类 → src/third_party/，零依赖最省事
```

- ① 的判据：`packages/<包>/package.yaml` 里有 `verified_<日期>` 才算"在这块板上验过"；
  `verified: null` = **只有头文件层面说明，没上过真机**。
- ② 的判据：`grep -c <库名> .fsc/<平台>/CMakeLists.txt`（09-28 前产物目录为 `.fun/`）。
- ③ 的坑：`fun launch`（调试推送）**不推** `lib-no-link`，开发期要手动 push 到 `/data` 或 `/tmp`；
  **但 `/data` 在 ld 路径最前，会遮蔽 `/res/lib` 的固化版** ⇒ "升级了库却跑旧库，日志毫无异常"。
- ④ 的风险：设备上的 `.so` 随固件走，**下个版本可能没有/签名变了** ⇒ 必须包一层失败回退。

---

## 2. ★ 用前四道判据（本篇最有价值的部分）

### 判据 1 · libc 必须匹配（最容易死人的一条）

| 平台 | 架构 / libc | 交叉工具链口径 |
|---|---|---|
| **V85X / T113** | ARMv7（32 位）**musl** | `arm-unknown-linux-musleabihf-*` |
| **Z20 / Z21** | ARMv7（32 位）**glibc** | `arm-pc-linux-gnueabihf-*` |
| **F133 / F135** | RISC-V 64 **musl** | RISC-V 工具链（**严禁混入 glibc**） |

- **预编译二进制必须与目标平台 libc 同源**。拿 glibc 版 `.so` 丢给 V85X = `not found`/段错误；
  反向同理。**同一份库，V85X 与 Z20 要分别编**。
- 静态链接（`-static` + musl）是跨平台最稳的形态；本仓设备端工具就是这么发的
  （`mt_test`：ARMv7 musl 72KB / ARMv7 glibc 4.5MB——**同一份源码，体积差 60 倍**，
  静态带 glibc 会顺带把一堆东西拖进去）。
- 判据命令：`file <lib>`（看 interpreter/架构）、`readelf -d <lib> | grep NEEDED`（看依赖的 libc）。

### 判据 2 · ABI / 符号对得上

- `readelf -h`（ELF class / machine：ARM 32/64、RISC-V）→ `nm -D --defined-only <lib>.so`
  → 对照**头文件里的公开 API**逐个确认（**禁止凭记忆写 API，签名必须能在头文件 grep 到**）。
- C++ 库还要看 `__cplusplus` 与 `-fno-exceptions/-fno-rtti` 口径是否与工程一致（不一致就是链接期爆炸）。

### 判据 3 · 体积预算（`/res` 是硬上限）

- 实测：`/res` 只有 **7,995,392 字节**（V85X 板，2026-09）。而 `/tmp` 是 tmpfs（吃内存，撑爆 OOM 杀 zkgui）。
- **静态库带 DWARF 调试信息会瞬间吃掉一半预算**：本地 ffmpeg 静态库带 4.5MB DWARF，
  必须 `strip -g` 后才进工程（工程自包含、可复现）。
- 判据：`ls -l` 后对比 `/res` 余量；上设备后 `du -sk /res/ui /res/lib`。

### 判据 4 · 运行期路径可见性

- 设备 `/etc/init.rc` 的 `LD_LIBRARY_PATH` 实测含 `/data:/tmp:/res/lib:/res/zkswe:/lib:/lib/eyesee-mpp`
  ⇒ `/res/lib`（固化版）与 `/data`、`/tmp`（调试版）都在内，**顺序决定谁赢**。
- 验收判据：`cat /proc/<pid>/maps` 必须指向 `/res/lib/...`（**不能是 `/tmp/lib/...`**）；
  本板 `fun launch` 后 pid 常不变、跑的还是旧 so，**只看 pid 会被骗**。

---

## 3. 需求 → 库映射（标注验证状态，禁止把未验证当可用）

| 需求 | 推荐 | 接入 | 已知坑 / 状态 |
|---|---|---|---|
| HTTP(S) / 下载 / WebSocket | `curl-cxx`（Z20 已验）/ `civetweb` | registry | WebSocket 回显、Downloader 双任务已验（Z20） |
| TLS | `mbedtls` / `openssl` | registry | **版本按平台分叉**：Z20 的 openssl=`1.1.1-w`，其他平台 `1.1.1-g`；**别照抄** |
| MQTT | `mqtt-cxx` / `paho-mqtt3as` | registry | **Z20 的 paho 必须配 openssl**，否则链接报 `BIO_read/RAND_bytes/SHA1_* undefined`；**Z21 registry 无此包** |
| DNS 直调 | `cares` | registry | 已验（Z20，5 域名 30~88ms） |
| JSON | `rapidjson`（纯头文件）/ `base-json` | registry / 源码 | rapidjson **未上真机**，按 §2 自证 |
| 校时 | `ntp` | registry | **无 RTC 板开机=1970 ⇒ 带证书校验的 HTTPS 会报 `certificate validity starts in the future`，必须先校时** |
| 压缩 | `z`（zlib） | registry | ffmpeg 的 http(gzip)/mov 解封装依赖它 |
| 媒体（播放/解封装） | 官方 `awh264player` / 本地 ffmpeg 静态库 | registry / ② | ffmpeg 走本地库 + `strip -g`；与 MPP 路线**互斥**（同一颗 VE、同一 disp 视频层） |
| 蓝牙（BLE HID / GATT） | `btstack` | registry | 需模组在位（自检可直接判：无 BT 属性/rfkill/hci 节点 = 没插） |
| 数据库 / Modbus / OPC-UA / 组播 / 序列化 | sqlite3 / libmodbus / open62541 / libwebsockets / protobuf-c 等 | **需自编（②或⑤）** | **未验证**：先按 §2 四判据自证，再按 §4 落地 |

---

## 4. 落地纪律（六条）

1. 改 `Manifest.xml` 必须 `fun install`（否则头文件路径不进 CMake）；
2. 本地库放 `src/dependencies/lib/`，**不要手改生成的 `CMakeLists.txt`**；
3. TLS 的 CA 证书只认**资源目录（resPath）**：放别处报
   `not correctly signed by the trusted CA`——**那是没找到 CA，不是证书坏**；
4. 大库先 `strip`，再算 `/res` 预算；
5. 固化后必验 `cat /proc/<pid>/maps`；
6. 结论必须落判据（命令 + 期望值），别用"能跑了"当验收。

---

## 5. 反模式（都是踩过的）

| 反模式 | 症状 | 正解 |
|---|---|---|
| 拿 glibc 预编译库给 musl 平台 | 链接过 / 设备起不来 | 按 §2-1 重新编 |
| 同名 `.so` 长期留在 `/data` | 升级了库却跑旧库，**日志无异常** | 用完删；固化走 `/res/lib` |
| 靠"编译通过"当"能用" | 上机才炸 | 真机判据（maps/fd/回读） |
| 从别的 GUI 框架类推 API | AI 写出不存在的接口 | 见 `devflow/capability-boundaries.md`：**未收录就标未收录，不猜、不类推** |
| 把大库直接丢进 `/res` | 升级包写不下/启动失败 | strip + 算预算，或改 dlopen 按需 |

---

## 6. 本文明说未验证的边界

- 第 3 节标"需自编/未验证"的库（sqlite3 / libmodbus / open62541 / protobuf-c …）**没有真机结论**；
- 第 2 节的判据是**方法论**，已在本工程用过的只是其中一部分（strip/预算/路径/libilc 口径）；
- 真机证据补齐后应把 `status` 从 `draft` 提升为 `verified` 并挂 `evidence`。

---

## 附 · 检索验收问法（入库时登记进 `scripts/check_retrieval.py` 的 GROUPS）

`想用开源库怎么办` / `registry 里没有这个包` / `自己编译的库怎么加进工程` /
`dlopen 找不到库` / `musl 和 glibc 有什么区别` / `静态库太大怎么办` / `SQLite 能用吗` /
`第三方 .so 放哪` / `undefined reference 链接错误` / `ldd 看哪些库`
