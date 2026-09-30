# 信号探针（WiFi / 蓝牙）独立应用

> 2026-09-15。需求：「WiFi 信号探针单独做成一个应用，采用 FlyThings 控件来开发。
> 功能参考 iOS `Find Spy 隐藏设备探测器`」。
>
> 上一轮（2026-09-12）的探针是 **wifi.ftu 里的一个二级 window**（`WinProbe`），
> 只有"AP 列表 + 信道占用"。本轮把它**提升为独立应用**并补齐 Find Spy 的能力。

---

> ## ★ 2026-09-15 第二轮：加了「局域网设备」和「无线嗅探」两页（v1.22）
>
> 第一轮（v1.21）做的是**被动观察**；用户追问"手机 App 是怎么做到的 / 允许顶掉 zknet 是否可行"
> 之后，补上了真正能**主动发现**偷拍设备的两页。要点先列在这里，细节见 **§11**：
>
> | 新页 | 干什么 | 关键机制 | 实测结论 |
> |---|---|---|---|
> | **局域网设备** | 找出**连在同一张网里**的设备，认出 IP 摄像头 | ARP 扫段（UDP 踢 ARP + 读 `/proc/net/arp`）→ OUI → 端口扫 → **RTSP DESCRIBE / HTTP HEAD 指纹** | ✅ 实测扫到 5 台；对假摄像头判出 **level=3「视频流可直读(H264)」** |
> | **无线嗅探** | 看见**平时完全隐身**的 STA、拿到**真 RSSI**、**解出隐藏网络的名字** | 把**闲置的 wlan1** 切成 MONITOR（nl80211 SET_INTERFACE）+ AF_PACKET 抓帧 + radiotap/802.11 解析 | ✅ 实测 CH149、AP rssi=-40、多个随机 MAC 的 STA |
>
> ★★ **不需要"顶掉 zknet"**：`wlan1` 本来就是闲置的（`/res/bin` 里没有 `p2p_supplicant`
> ⇒ 本板 P2P/Miracast 起不来），把它改成 MONITOR 实测返回 0，**wlan0 的 STA 连接全程不受影响**，
> 退出应用会自动切回 STATION。
>
> ★ 为什么这两页**比 iOS 那个 App 强**：见 **§11.6 能力对照**（iOS 连"周边 WiFi 列表"都拿不到，
> 更没有 monitor 模式；我们能看空口、能扫局域网、能解隐藏 SSID、能用真 RSSI 定位**任何**设备）。

## 1. 交付清单

| 文件 | 说明 |
|---|---|
| `ui/probe.html` → `probe.json` → `probe.ftu` | 界面源稿（**全部 FlyThings 原生控件**，零自绘） |
| `src/platform/PgLan.{h,cpp}` ★ | **局域网主动探测**：ARP 扫段 + SSDP + 端口扫 + RTSP/HTTP 指纹 + OUI/风险判定 |
| `src/platform/PgSniff.{h,cpp}` ★ | **无线嗅探**：nl80211 切 monitor（wlan1）+ AF_PACKET 抓帧 + radiotap/802.11 解析 |
| `src/logic/probeLogic.cc` | 该 ftu 的全部逻辑（zknet 扫描 + pg::Bt 扫描 + 追踪 + QA 通道） |
| `src/core/PgProbe.{h,cpp}` | 主界面卡片元信息（title/desc/tag/theme） |
| `ui/main.html` | 新增 `Icon21`（第 22 个图标控件） |
| `tools/gen_icons.py` | 新增 `sym_radar` 符号 + `app_icon_21.png` |
| `tools/gen_ui.py` | `UI_SOURCES` += probe.html；`HIDDEN_PAGE_WINDOWS` += WinProbeBt/WinHunt；`OPAQUE_WINDOWS` += 三页；`HIDDEN_CONTROLS` 图标数 21→22 |
| `src/core/PgGames.cpp` | `createProbe()` + `kAppTable` 追加 `{"probe", …, 21}` |
| `src/logic/mainLogic.cc` | `kIconCount` 21→22、`onButtonClick_Icon21` 桩、`kPageApps` += `probeActivity` |
| `ui/wifi.html` / `src/logic/wifiLogic.cc` | **摘掉** `WinProbe` 整块，底部按钮改成 `openActivity("probeActivity")` |

## 2. 页面结构

```
probe.ftu（一个 Activity = **四个**整屏普通 window 由页签互斥 + 一个整屏浮层）

  WinProbeWifi  ┐
  WinProbeBt    ├─ 页签切换（**每个页面各带一套 4 段页签**，见 §11.5 的坑）
  WinProbeLan   │
  WinProbeSniff ┘
  WinHunt        ← 整屏浮层（写在 html 最后 = 窗口栈最上层；showTab 会先收起它）

旧结构（v1.21）是三页两页签：
├─ WinProbeWifi（默认显示）
│  ├ 顶栏：返回 / 「信号探针」/ 刷新
│  ├ 分段控件：WiFi 探测 | 蓝牙探测（容器 BarSegProbe + 两个动态底色分段）
│  ├ 总览卡：附近 N 个网络 · 2.4G x / 5G y ；可疑 n · 关注 m · 开放 k ；
│  │         信道建议 CH11（1 个占用）· 最挤 CH1（9 个）
│  └ 列表（按 RSSI 降序）：频段 / 信道 / SSID / 加密+BSSID / dBm / 风险标记
├─ WinProbeBt（初始隐藏）
│  ├ 状态卡：BT 状态字 / 广播累计 / 周边设备数 / 提示行
│  └ 列表：名称 / 地址 / dBm / 风险标记      ← 点一行即锁定
└─ WinHunt（初始隐藏）★ 核心
   ├ 目标卡：目标名 + 频段·信道·加密
   ├ 大号 dBm（64px）+ 12 段强度条 + 一行结论（很强/中等/偏弱）
   ├ 最强记录 / 平滑值·采样数
   ├ 蜂鸣开关（越强越密）
   └ 重新锁定 / 停止追踪
```

**为什么三个页面放同一个 ftu**：这是 MCP《页面架构规范》的口径 ——
「跨业务域 / 需独立生命周期 / 大页面 → 独立 ftu；**同一业务域内的页签·二级页·弹窗 →
同 ftu 内多个整窗 window + showWnd/hideWnd**」。这三页是同一个应用的页签关系，
所以同 ftu；而"探针"整体相对 WiFi 设置是**另一个业务域**，所以从 wifi.ftu 拆出来。

> ⚠️ `html2json` 对多整屏 window 会告警「页面级互斥全屏 Window 应拆多 Activity」。
> 这个告警对**页签型**页面是误报（每条规则都有适用边界），本页按项目规范保留同 ftu。

## 3. 功能对照 Find Spy

| Find Spy 的能力 | 本应用怎么落地 | 状态 |
|---|---|---|
| 扫附近全部 WiFi 网络 | `WifiManager::scan()` + `handleWifiScanResult` 监听（与 WiFi 页同一条链路） | ✅ |
| 显示网络信息（信道/加密/信号） | 列表：频段 / CH / 加密 / BSSID / dBm | ✅ |
| **区分安全与可疑设备** | 关键词特征库（高危 3 档）+ 摄像头厂商 OUI 命中（BSSID 前三段） | ✅ |
| **定位/追找设备（扫描指示器）** | 「热点猎手」：锁定目标后每 1.5s 重扫，EMA 平滑 + 峰值保持 + 12 段条 + 越近越密的蜂鸣 | ✅ 本应用最有用的功能 |
| 网络安全建议 | 2.4G 的 1/6/11 占用统计 → 「信道建议 CHx」+「最挤 CHx」；开放网络计数 | ✅ |
| 蓝牙设备扫描 / 追踪 | `pg::Bt` 的 BLE 主机扫描（周边设备 + 实时 RSSI）→ 列表 + 同样可以锁定追踪 | ✅ |
| "已连设备列表 / IP 摄像头识别" | **没做**（要主动探测：连上目标网后 ping 扫段 + 端口扫 80/554/8000/34567 + MAC OUI） | ⛔ 见 §7 |

## 4. 可疑判定（`probeLogic.cc` 的特征库）

**被动观察的边界**：纯扫描只能看到空口广播了什么，**不能证明那是偷拍设备**
（很多智能插座、打印机默认 SSID 里就带 `cam`）。命中只代表"值得走过去看一眼"。

| 等级 | 判据 | 列表里显示 |
|---|---|---|
| 3 高危 | 名称命中 `camera / ipcam / spy / pinhole / 偷拍 / 摄像头 / 监控 / 针孔 / 录像` | `高危 · 名称含摄像头词`（红） |
| 2 可疑 | 名称命中弱词（`cam / dv / eye / v380 / ycc365 / esp32 / ipc / video / cctv / mini / baby`…）**或** BSSID 命中摄像头/IoT 模组 OUI（海康 `44:19:b6`、大华 `3c:ef:8c`、Espressif `24:0a:c4` 等） | `可疑 · …`（金） |
| 1 关注 | 隐藏 SSID（空名字）；BLE 匿名广播且 RSSI ≥ -60 | `关注 · …`（灰） |
| 0 普通 | 其余 | 空 |

⚠️ 关键词判定顺序**必须先高危再弱词**：`camera` 也含 `cam`，反过来判会把高危降成可疑。

## 5. 热点猎手（怎么用它找东西）

1. 在 WiFi / 蓝牙列表里**点一行**（或按底部「热点猎手」→ 自动锁定当前信号最强的那个）。
2. 拿着机器在房间里**慢慢走**，屏幕上的大号 dBm 与 12 段条会实时变化；
   蜂鸣按强度自动加密（≥10 段 400ms 一声 / ≥7 段 800ms / ≥4 段 1600ms）。
3. 走到最强的那一点，看墙、插座、天花板、通风口、烟感、路由器旁。
4. 「重新锁定」清掉基线（换目标方向后重来）；「停止追踪」回列表。

实现要点：

- 追踪期间由 `TIMER_HUNT`（1.5s）触发重扫，但**用 `sScanWaitUntil` 做闸门** ——
  单次扫描 1~3 秒，比周期还长，上一轮没回来就跳过这一拍（堆请求会让 wpa_supplicant 越来越慢）。
- **采样判据**：WiFi 用 `sScanStamp`（每收到一轮结果 +1）——同一轮不重复采样，
  否则 400ms 的轮询会把同一个 RSSI 采样 6 次、均值被灌水；BLE 用"原始 RSSI 变了"。
- `EMA` 平滑（0.6/0.4）压抖动；峰值独立保存，便于"回头再确认"。
- 连续 4 轮扫不到目标 → 显示"-- / 暂时扫不到"，**不让用户对着假信号乱走**。

## 6. QA 自检通道（免触摸验收）

本板 `mt_test` 触摸注入时灵时不灵（`docs/wifi-app.md` §3.2），原生控件又走不了
`dispatchCanvasTouch` ⇒ 文件驱动通道是唯一自动化入口。

```bash
# 打开探针应用（从主界面走索引 21，或直接跳 Activity）
adb shell "echo probe > /tmp/pg_autostart"
# 探针应用自己的命令通道（**内容变了才执行**，每行加序号）
adb shell "echo 'scan 1' > /tmp/pg_probecmd"
adb shell "logcat -c; echo 'dump 2' > /tmp/pg_probecmd; sleep 2; logcat -d | grep probeLogic"
```

⚠️ **命令文件要求"整份内容变了才执行"**，所以每行都要换内容。约定 **`#` 之后是注释**
（写法：`scan #7`）—— 与主界面 `/tmp/pg_autostart` 同一套。**不要**拿序号当参数
（`risk 1` 会被判成"判定名叫 1 的 SSID"，实测踩到）。`pollProbeCmd()` 里统一剥掉 `#` 之后的内容。

| 命令 | 作用 |
|---|---|
| `tab 0..3` | 切页签（0 = WiFi 探测 / 1 = 蓝牙探测 / **2 = 局域网设备 / 3 = 无线嗅探**） |
| `lan` ★ | 开始一轮**完整**局域网扫描（邻居发现 + SSDP + 端口/协议指纹，10~25 秒） |
| `lanquick` ★ | 只做邻居发现 + SSDP（不探端口，快） |
| `lanprobe <ip>` ★ | 只深探某一台（表里没有它会先单独踢一次 ARP，见 §11.5） |
| `lanlist` ★ | 打印局域网设备表（IP/MAC/厂商/类型/端口/等级/原因/指纹/SSDP） |
| `lantest` ★ | **局域网判定自检**：内置 15 条样例跑一遍 classify（不用真摄像头也能验） |
| `sniff on\|off` ★ | 开始 / 停止无线嗅探（把 wlan1 切成 MONITOR 顺听，退出应用自动还原） |
| `snifflist` ★ | 打印发射者表 + 顺听到的 SSID（含从 assoc 解出的隐藏网络名） |
| `oui [mac]` ★ | 查 MAC 厂商（不带参数跑一遍样例表） |
| `scan` | 触发一次 WiFi 扫描 |
| `dump` | 打印 AP 表（序号 / SSID / BSSID / CH / 加密 / RSSI / **风险等级**） |
| `sum` | 打印总览计数 + 信道建议 |
| `hunt <idx>` | 锁定 AP 列表第 idx 个 → 进热点猎手 |
| `huntbest` | 锁「有风险里信号最强的」，没有风险的就锁最强的（**不受序号漂移影响**） |
| `huntssid <名字>` | 按 SSID 锁定（不做 `_`→空格替换） |
| `bt` | 启动蓝牙（`rtk_init`，20~40 秒） |
| `btscan` / `btdump` | 清空重扫 / 打印 BLE 设备表 |
| `huntbt <idx>` | 锁定 BLE 列表第 idx 个 → 进热点猎手 |
| `stop` / `beep 0\|1` | 停止追踪 / 蜂鸣开关 |
| `state` | 打印整体状态（页签 / 目标 / 平滑值 / 峰值 / 段数 / 蜂鸣 / 蓝牙） |
| `risk [名字]` | 可疑判定：带参数判这个名字，**不带参数跑一遍内置样例表**（验证特征库） |
| `btrisk [名字]` | 同上，按 BLE 规则判（RSSI 固定 -50） |
| `back` / `who` | 返回主界面 / 诊断"触摸被谁吃了" |

## 7. 能力边界与下一步

**本应用不做**（都是"被动观察"做不到的事）：

1. **局域网在线设备/IP 摄像头识别** —— 需要主动探测：连上目标网 → ping 扫段 +
   `/proc/net/arp` 取 MAC → OUI 判厂商 → 探 80/554/8000/8080/34567 端口。
   代价是**必须连进那张网**（会打断当前连接，而且"只保留一个 network 条目"的
   zknet 会把原来保存的 SSID 顶掉，见 `docs/wifi-app.md` 末尾的血案）。
2. **红外/镜头反光探测** —— 本板没有 IR 收发硬件、也没有摄像头（是台触摸屏）。
3. **蓝牙/2.4G 私有 RF 的"抓包重放"** —— HID/私有 RF 都在加密链路上，不成立
   （结论见 `docs/wifi-app.md` §7.2）。

**本板 BT 侧的一个既有问题**（不是本应用引入的，已做对照实验）：

LE 扫描会在启动后 **1~3 分钟内自行停止上报**（`advCount` 冻住、设备列表冻住）。
- 对照实验：**不调用任何"重新扫描"也会冻** —— 启动后 adv=4301，之后 48 秒三个读点全是 4301。
- `btTickTimer` 的哑火自愈（每 10s 重起一次 LE 扫描）**救不回来**。
- 恢复方式：重启进程重新走 `rtk_init`（实测连续三次都成功）。
- 对应用的后果：**列表会停在最后一次收到的 16 个设备**（不是空），仍可用，只是不再更新。

**顺手可以做的**（按性价比排序）：

1. 把"风险命中"落一份日志到 `/data`（现场排查"昨天那个可疑 AP 是谁"）；
2. 局域网设备扫描（上面第 1 条，风险是网络切换，建议做成"只扫描不连接"的只读模式）；
3. 把历史扫描结果存成 CSV 供 PC 侧 `tools/wifiprobe.py` 继续分析。

## 8. 真机验收（2026-09-15，adb 20080411）

| 项 | 判据（日志/像素） | 实测 |
|---|---|---|
| 应用注册与跳转 | 日志 | 主界面 QA `probe` → `probeLogic: onUI_show`；WiFi 页 QA `probe` → `wifiLogic: 打开信号探针应用` → `probeLogic: onUI_show` ✅ |
| 主界面图标 | 逐像素 | 工具分类第 8 格 (372..443, 250..320) 有 **#FF9F0A squircle 图标**（4303 像素）✅ |
| WiFi 扫描 | 日志 | `dump 共 34 个 AP`，按 RSSI 降序（-17 → -93），含 2.4G/5G ✅ |
| 信道建议 | 日志 | `sum … ch1=8 ch6=2 ch11=3` → `信道建议 CH6（2 个占用）· 最挤 CH1（8 个）` ✅ |
| **可疑判定特征库**（`risk`） | 日志 | 高危 4/4（IPCamera-4F2A / Wireless Camera / 看家摄像头 / 监控-01）✅；可疑 4/4（ESP_1A2B3C / V380 / HIDDEN-CAM / GC-ABCDEF）✅；**普通 7/7 无假阳性**（TPLink_zkswe / ChinaNet-bsyX / aWiFi / zkswe-soft_5G / DIRECT-01-HP / ADMIN 9959 / 钱多多）✅；隐藏 SSID → `关注 · 隐藏 SSID` ✅ |
| BLE 判定（`btrisk`） | 日志 | 匿名@-50 → `可疑 · 匿名设备且信号很强`；IPC-1234/V380/SmartCam-A1 → 可疑；Mi Band 8 / PocketGame-RC → 普通 ✅ |
| 蓝牙扫描 | 日志 | `state=2 就绪`、`adv=4456 dev=16`（16 = 收集上限）；`btdump` 列出真名字（`LE-Remoter` / `BLE_WIFI_CFG_05E9` / `Ulanzi TC002 1c10`）+ 匿名设备标 `关注/可疑` ✅ |
| 热点猎手（WiFi 目标） | 日志 + 像素 | `锁定 AP 'TPLink_zkswe' key=94:d9:b3:2b:c9:e1 rssi=-17`；**12/12 段**全亮、目标名金色 (FF9F0A) ✅ |
| 热点猎手（BLE 目标） | 日志 + 像素 | `锁定 BLE addr=CC:C4:B2:44:1C:10 rssi=-54` → `avg=-52.5 peak=-50 samples=6 bars=8`；截图 **9/12 段**（红4/金4/绿1）、停止键红、重新锁定灰 ✅ |
| 采样判据 | 日志 | `samples` 每轮扫描只 +1（1→3→5，约 2 秒/轮）——**没有把同一轮 RSSI 重复采样 6 次** ✅ |
| 分段控件 | 逐像素 | 选中块 `#3A3A3C`、未选项与容器同色 `#1C1C1E`（隐形）；两个页签各自正确 ✅ |
| 行卡 / 面板 | 逐像素 | 总览卡 / 状态卡 / 行卡内部 = `#1C1C1E`，圆角干净（(4,252) = 页面黑）✅ |
| 屏保（工作界面禁屏保） | 日志 | 在探针页待了几分钟：`who saverEnable=0 saverOn=0` ✅ |
| 底部按钮配色 | 逐像素 | 左键 `#2C2C2E`、右键（热点猎手）`#0A84FF`、蜂鸣键绿 `#30D158` + 黑字 + 圆角 ✅ |

---

## 9. 能力边界实测（2026-09-15）：隐藏 SSID / STA 摄像头 / 监听模式

用户追问三件事，实测+驱动取证结论如下（**这节是"能不能做到"的权威口径**）。

### 9.1 隐藏 SSID：能看见"它"，但默认拿不到名字

| 问题 | 结论 |
|---|---|
| 扫描能发现隐藏 AP 吗 | ✅ 能 —— beacon 里不带 SSID，但我们照样拿到 **BSSID / 信道 / 加密 / RSSI** |
| 能拿到它的名字吗 | ❌ **默认不能**。隐藏 AP 不响应**广播** probe request；要拿到名字必须发**定向** probe（包里带 SSID），而"不知道名字"就发不出定向包（鸡生蛋）。唯一例外：该 SSID 已存在本机 `wpa_supplicant.conf` 且带 `scan_ssid=1`（= 曾经连过），wpa_supplicant 才会去定向问 |
| 代码怎么处理 | `SSID 为空` → 列表显示 `(隐藏网络)` + 风险标 `关注 · 隐藏 SSID`；**热点猎手照样能用**（锁定键优先用 BSSID，见 `huntLockAp`）⇒ 就算没名字也能"走过去找" |
| 真机验证了吗 | ⚠️ **没有**：本环境连续扫描 30 个 AP **全部广播名字**（`ssid='(隐藏)'` 命中 0）⇒ 这条分支只有代码、没有真机证据 |
| 能连上去吗 | ❌ 基本不能：zknet 写进 `wpa_supplicant.conf` 的网络块**不带 `scan_ssid=1`**（见 10.3 实测原文），所以它对隐藏网络不会定向 probe |
| 对"找摄像头"有用吗 | **基本没用**：偷拍设备不会去隐藏自己的 AP（它多半根本没有 AP 模式，见 10.2） |

### 9.2 ★★ 真正的偷拍设备是 **STA**，这正是当前探针的根本盲区

偷拍设备按组网方式分三类，**只有第一类能被 AP 扫描发现**：

| 类型 | 行为 | 我们的 AP 扫描能发现吗 | 怎么发现 |
|---|---|---|---|
| **softAP 型** | 自己发热点（`IPCamera_xxxx` 之类），多用于初次配网/直连查看 | ✅ 能（就是我们现在做的） | 扫 AP + 关键词/OUI 判定 |
| **STA 型**（**主流**） | 连进房间/酒店的 WiFi，往云端推流 | ❌ **完全看不见**（STA 不发 beacon，不广播任何东西） | 必须**加入同一张网 + 主动探测**：ARP/ping 扫段 → MAC → OUI → 端口（80/554/8000/8080/34567） |
| **4G/蜂窝型** | 插 SIM 卡，根本不在 WiFi 上 | ❌ 看不见 | 只能靠射频/红外（本板无相应硬件） |

第二类还有一层：STA 设备**空闲时不发帧**（只在你睡着时才推流的摄像头，静默期完全不可见）
⇒ 想找它得先"制造流量"（让它上传），或者干脆靠主动探测（ARP 请求是主动发的，能逼它回）。

### 9.3 当前这块板子的 WiFi 到底支持什么（实测）

```
驱动：8733bs（RTL8733BS，Realtek rtw 系全功能驱动）
      版本 v5.14.1-46-gc99202c57.20220113_COEX20211210-2706      ← lsmod + /sys/module/8733bs/version
接口：wlan0（STA，192.168.0.125 已连）+ wlan1（**闲置** —— 见 ① 的说明，P2P 起不来）
      ⇒ /sys/class/ieee80211/phy0 存在 + /sys/class/net/wlan0/phy80211 ⇒ **cfg80211/nl80211 驱动**
模式：STA ✅ / SoftAP ✅（rtw_ap_* 一整套参数）/ P2P ✅（rtw_sel_p2p_iface=1）
      rtw_mp_mode=0（= 普通驱动，不是 MP 测量驱动）、rtw_btcoex_enable=2（WiFi/BT 共存开着）
工具：设备上**只有** ifconfig / ping / wpa_supplicant。
      **没有** iw / iwconfig / iwlist / wpa_cli / hostapd / arp / nc / curl / wget / busybox
控制口：wpa_supplicant 的 ctrl_interface = **/dev/socket/**（zknet 就是走它下 SCAN/CONNECT 的）
       实测有 `/dev/socket/wlan0`（unix socket）= wlan0 的控制口 ⇒ **想下原始命令可以直连它**
       （`SCAN` / `SCAN_RESULTS` / `BSS` / `SET_NETWORK … scan_ssid 1`），不必改 zknet
```

**① 监听模式（monitor mode）：✅ 实测可用，而且「不用顶掉 zknet」**

先看驱动自己怎么声明（`GET_WIPHY` 的 `SUPPORTED_IFTYPES`，工具 `tools/nlprobe.c`）：

```
支持的接口类型: ADHOC(1) STATION(2) AP(3) MONITOR(6) P2P_CLIENT(8) P2P_GO(9)
```

然后逐条实测（2026-09-15，`/tmp/nlprobe`，adb 20080411）：

| 动作 | 结果 |
|---|---|
| `NEW_INTERFACE type=MONITOR`（在 phy0 上建 `mon0`，也试过符合驱动命名模式的 `wlan2`） | ❌ **-22 EINVAL** —— 驱动 **vif 上限 = 2**，而当前已经有 2 个（wlan0/wlan1 都是 STATION） |
| **`SET_INTERFACE wlan1 type=MONITOR`**（复用闲置的 wlan1，改类型而不是新建） | ✅ **返回 0**；`GET_INTERFACE` 复查：`wlan1 type=MONITOR` |
| `SET_CHANNEL wlan1 5745` | ❌ -16 **EBUSY**（单射频，wlan0 已占用该信道）—— 但它**本来就同信道**，不影响收帧 |
| `AF_PACKET` 抓帧 12 秒 | ✅ **138 帧**（管理 119 / 数据 19），**2 个发射 MAC**：`94:d9:b3:2b:c9:e3`（= 我们连的那台 AP 的 BSSID，136 帧）、`9e:01:6e:2b:09:ae`（**另一个 STA**，2 帧数据） |
| 期间 wlan0 的 STA 连接 | ✅ 全程正常（`ping 192.168.0.1` 2/2 通、1.5ms） |
| `SET_INTERFACE wlan1 type=STATION`（还原） | ✅ 返回 0，类型回到 STATION |

⇒ **结论：monitor 可用，且不需要"顶掉"任何东西** —— 拿**闲置的 wlan1** 就行。

**为什么 wlan1 是闲置的**（这条是意外收获）：`/etc/init.rc` 里写着
`service p2p_supplicant /res/bin/p2p_supplicant -iwlan1 -Dnl80211 -c/data/misc/wifi/p2p_supplicant.conf -C/dev/socket/`，
而 **`/res/bin` 里根本没有 `p2p_supplicant`**（只有一个 `firmware/` 目录）⇒ 该服务**永远起不来**
⇒ 本板的 **P2P/Miracast 本来就是坏的**，wlan1 没有进程在用（工程源码与框架配置里也搜不到 `wlan1`）。

**用 monitor 时的三条限制**（实测得出，别指望更多）：
1. **只能停在我们连的那个 AP 的信道**（单射频，`SET_CHANNEL` 返回 EBUSY）。对"找同一网络里的
   摄像头"来说**正好够用**（它必然在同一信道）；但想扫其他信道，得让 wlan0 漫游过去（做不到）。
2. **看不到加密内容**，只能拿到"发射者 MAC + 帧数 + 帧类型"。现代手机普遍用**随机 MAC**
   （本机实测抓到的那个 STA 就是 `9e:…`，本地管理位=1）⇒ OUI 查不到厂商；
   但**廉价的 IPC 模组多数不随机化 MAC**，OUI 查询对它们仍有效。
3. 需要 `CAP_NET_ADMIN`（zkgui 是 uid 0，够用），且**类型切换是全局的** ——
   将来若真有人把 Miracast 修起来，就要和它抢 wlan1（届时得改成"用完立刻切回"）。

**② 局域网主动探测（找 STA 设备）—— 完全可行，已实测**

**② 局域网主动探测（找 STA 设备）—— 完全可行，已实测**

设备连上 `TP-LINK_5G_C9E1`（192.168.0.0/24）后，用 shell 跑了一次 ping 扫段：

```
253 个地址、分 7 批并发 → **7.6 秒**扫完；随后 /proc/net/arp 里拿到 4 台在线设备的真实 MAC：
  192.168.0.1    94:d9:b3:2b:c9:e1   （网关）
  192.168.0.110  34:c9:3d:ad:26:63
  192.168.0.116  32:3d:56:24:5e:cb   （首字节 0x32 = 本地管理位 = 随机 MAC，手机）
  192.168.0.120  0c:cd:d0:7b:cd:91
其余条目 Flags=0x0 = ARP 未应答（= 地址没人）
```

可行性要素（都实测过）：`ping` 可用 ✅ / `/proc/net/arp` 可读 ✅ / zkgui 以 **uid 0** 跑
（`ps` 里 USER=0）⇒ 可开原始套接字、可直接读 ARP。**端口探测要自己写**
（设备没有 `nc`/`curl`/`wget`；用非阻塞 `connect()` + 超时即可，我们已经在跑 civetweb/SSDP，socket 没问题）。

⚠️ 代价（必须提前说清）：**必须连进目标网络**；而 zknet 是"只保留一个 network 条目"
（`update_config=1` + `SET_NETWORK 0`）⇒ 换 SSID 会**顶掉**原来保存的网络（血案见 `docs/wifi-app.md` 末尾）。
酒店场景里"连酒店 WiFi 扫一遍"是可接受的，但**要提醒用户会断开/覆盖原网络**。

### 9.4 结论：现有的探针能做什么、不能做什么

| 目标 | v1.21（AP 扫描 + BLE 扫描） | **v1.22（+ 局域网页 + 嗅探页）** |
|---|---|---|
| softAP 型设备 / 可疑 AP | ✅ 能扫、能判风险、能定位 | ✅ 同上 |
| **STA 型设备 / IP 摄像头** | ❌ 看不见 | ✅ **局域网页**：连网 → ARP 扫段 → MAC 厂商 → 摄像头端口 + **RTSP/HTTP 协议指纹** → 定位（见 §11） |
| 隐藏 SSID 的**名字** | ❌ 拿不到 | ✅ **嗅探页**：顺听客户端的 **assoc request**，SSID 是明文的（见 §11.3） |
| 同信道 STA 活动（不连网） | ⚠️ 只有调试工具 nlprobe 能看 | ✅ **嗅探页**：发射者 MAC + 真 RSSI + 帧数，进页面即可 |
| 全信道 STA / 4G 摄像头 | ❌ | ❌ 单射频只能看一个信道；4G 设备无相应硬件 |

## 10. 踩过的坑（本轮）

1. **`fun.exe build` 不给 window 生成绑定？不，给的，但 `.window` 默认 `visible:true`** ——
   三个整屏 window 若不显式关掉，进页面三页叠在一起。
   ⇒ `HIDDEN_PAGE_WINDOWS` 必须列出 `WinProbeBt` / `WinHunt`（默认页不列）。
2. **`.window` 的 `data-bg="#000000"` 会被 html2json 写成 `-1`（透明）** ⇒
   整屏窗口会透出桌面。⇒ 三页都要进 `OPAQUE_WINDOWS` 白名单（`gen_ui.fix_window_bg`）。
3. **动态底色的按钮不能挂静态九宫格**（`setBackgroundColor` 会清掉 backgroundPic）：
   - 分段控件两个分段、蜂鸣开关 ⇒ 源稿标 `data-noround="1"`，圆角由
     `pg::applyRoundedBg()` 在设底色时一起挂；
   - 12 段强度条 ⇒ 用 **`div.text` 装色块**（textview 没有 picTab，圆角注入整段跳过），
     **只切 `visible`、从不禁改底色**，顺带完全绕开"图与底色互斥"的坑。
4. **坐错底会出脏边**：分段坐在段容器（`#1C1C1E`）上 ⇒ `applyRoundedBg` 必须传
   `ON_CARD`（用烘页面黑底那套图会在胶囊四周留一圈黑）。蜂鸣按钮在页面底上 ⇒ `ON_PAGE`。
5. **列表行视图跨行复用** ⇒ 风险标记为 0 时必须**显式 `setText("")`**，
   否则上一行的"高危"会留在这一行的控件上（工程记忆第 6 条）。
6. **`gen_font.py` 需要 `fontTools`**（当前 shell 的 python 没有）。
   用带它装的解释器跑：`C:/Users/Admin/.workbuddy/binaries/python/envs/default/Scripts/python.exe tools/gen_font.py`。
7. **`cut` 类批量删除脚本的坑**：删除的"终点标记"在文件前面也出现过时，
   `str.find` 会命中前面那个 ⇒ 断言失败/删错。删除必须"从起点之后找终点"。

8. ★★ **切页签必须先把上层整屏窗口收起来**（症状极具误导性）：
   源稿里 `WinHunt` 写在最后 = 窗口栈**最上层**，而 `showTab()` 只切了
   `WinProbeWifi`/`WinProbeBt` ⇒ 猎手页一直盖在上面，点页签"没反应"。
   **判据**：蓝牙页截图在 (60,96) 采到的却是猎手页目标名的**金色**文字（`#E9920B`）。
   ⇒ `showTab()` 开头统一 `mWinHuntPtr->hideWnd(); sHuntOn = false;`（目标不清，底部还能再进）。

9. **`advName()` 不该在"这包没名字"时清空名字**（`src/platform/PgBt.cpp`）：
   BLE 广播包是**交替**发的（一包带名、一包只有 flags/service uuid），原来无条件
   `out[0] = 0` ⇒ 名字列来回闪（同一地址同一秒：`Ulanzi TC002 1c10` ↔ 空），
   连"可疑判定"都跟着抖（匿名 + 强信号 = 可疑）。现改成**只在解析到名字时覆盖**。
   （顺带修了蓝牙遥控的学习页，同一份代码。）

10. **`Bt::hostScanStart()` 不再无条件 `gap_stop_scan(); btStartScan();`**：
    扫描正常时"重新扫描"只**清空重收**、完全不碰控制器状态；只有确实哑火
    （`advCount` 与上一拍相同）才走冷路径 `btMaybeStartAdvScan()` 重起。
    动机见 §7 的观察 —— stop→start 紧邻调用在 btstack/HCI 上是不安全组合
    （"command disallowed" 会让扫描停在关闭态），而 watchdog 本来就在做"哑了就重起"。
    ⚠️ 这条**没有单独证明因果**（对照实验显示不调它也会哑），但"少做一步危险操作"是净收益。

11. **日志缓冲区很小**（实测 `logcat -d` 只有 13~32 行，就被 `PgBt` 心跳 / `PgDlna` SSDP
    通告刷掉）⇒ **读日志要紧跟在写命令之后**，并且**按关键字过滤**；
    "grep 不到"不等于"命令没执行"（坑过一次：以为 `btdump` 没跑，其实日志被挤掉了）。

12. ★★ **端口扫描的 `select` 必须循环**（第二轮最大的坑，症状极具误导性）：
    `select` 是"**有一个** fd 就绪就返回"。写一次就收工的话，**先返回的那个端口会把
    控制权抢回来**，其余端口即使后来也连上了也不会被检查。
    实测判据非常清楚：PC 侧日志明明记着"**80 和 554 都连进来了**"，
    设备却只报 `ports='554'` —— 因为 554 先就绪。改成"循环 select 直到全部有结果或超时"
    后立刻变成 `ports='80 554'`。**端口扫描少报一个口 = 漏掉一台摄像头**，不能含糊。

13. ★★ **`lanprobe <ip>` 会"什么也不做"**：它是"只探一台"，但 `hosts[]` 只有整段扫描才填。
    新进页面直接 `lanprobe` 时表是空的 ⇒ `findIdx` 返回 -1 ⇒ **不报错、不打印、静默返回**
    （日志只有"开始扫描 mode=3"+"本机 …"，看着像卡死）。
    现在会先对这一个地址**单独踢一次 ARP**（发两个 UDP 包 + 等 250ms）再读表，
    并且**找不到也要打一行日志**（"没有 ARP 应答"）—— 静默失败必须消灭。

14. **`LanHost.kind[]` 给小了会静默截断**：中文一个字 3 字节，"路由器/网关"要 19 字节，
    14 字节的缓冲把它截成"路由器/网"。**缓冲区按"最长的那个中文字符串"算，不要凭感觉**。

15. **网关不该被判"关注"**：第一版规则是"未知厂商 + 有开放端口/答了 SSDP ⇒ 关注"，
    结果**每台路由器都带个"关注"噪点**（网关开 80 和管理口、答 UPnP 都是正常的）。
    ⇒ 加 `isGatewayIp()`（末段 == 1）豁免。

16. **SSDP 应答要按 IP 去重再打日志**：同一台设备会对我们的三条 M-SEARCH 各回好几条，
    不去重会连着刷十几行同样的 `SERVER:`。本板日志缓冲只有十几行 —— **刷爆就看不清别的了**
    （这正是第 11 条的放大版）。

17. ★★ **排查"QA 通道停了"时不要只看 logcat**（本轮绕了远路）：
    现象是"连发 5 条 `who`，日志只多 1 行"，看着像**界面线程卡死在 `pthread_join`**
    （线程 wchan 确实是 `futex_wait_queue_me`，另一边还有个线程在 `udp_recvmsg`）。
    真相是**日志被挤掉**：QA 通道一直是好的。**判据要换一个"不依赖日志缓冲"的**：
    发一条会**改界面**的命令（`tab 2`）再**抓屏**看页签亮不亮 —— 一测就知道通道是活的。
    （这条与第 11 条是同一个坑的两种表现，值得单独记：**日志类是易失证据，像素类才是硬证据**。）


---

## 11. 第二轮：局域网设备页 + 无线嗅探页（v1.22）

背景：第一轮只做**被动观察**，而偷拍设备的**主流形态是 STA 型**（连进房间的 WiFi 往云端推流）
—— 它不发 beacon、不广播任何东西，AP 列表里根本没有它（§9.2）。
用户追问"参考的那个手机 App 是怎么做到的 / 允许顶掉 zknet 是否可行"，
于是补上真正能**主动发现**的两页。

### 11.1 局域网设备页（`src/platform/PgLan.{h,cpp}`）—— 找 STA 型设备的正解

四步流水线，跑在**自己的线程**里（只写自己的表 + 加锁，绝不碰控件）：

| 步 | 做法 | 为什么这么做 |
|---|---|---|
| ① 邻居发现 | 往整个 /24 每个地址发一个 **UDP 包**（port 9）⇒ 内核为了发出去**必须做 ARP 解析** ⇒ 读 `/proc/net/arp`（只取 `Flags=0x2` 完整项） | 比 ping 扫段**更灵**：所有端口都关着的设备也会答 ARP；还不需要 ICMP 权限。分两轮扫、每 48 个地址歇 40ms（`unres_qlen` 默认 3，灌太猛会被丢） |
| ② SSDP 顺听 | 发 3 条 `M-SEARCH`（`ssdp:all` / `Basic:1` / `rootdevice`）到 `239.255.255.250:1900`，收 2.6 秒，取应答里的 **`SERVER:` 头** | 很多 IPC / 路由器会把型号写进去（`IPCAM/1.0`、`Hipcam RealServer`…）；按 IP 去重再打日志（§10-16） |
| ③ 端口 + 指纹 | 12 个口**并发非阻塞 connect**（554/80/8000/8080/34567/37777/8554/8001/5000/88/22/9100）。对开着的口做**协议指纹**：<br>• 554/8554 → `RTSP DESCRIBE`<br>• 80/8080/88 → `HTTP HEAD` | **这是本页的核心价值**：<br>RTSP 回 `200 + m=video` ⇒ **确认是一路视频流**（最硬证据）；回 `401/403` ⇒ 确认是 RTSP 设备；<br>`Server: GoAhead-Webs / Boa / Hipcam / V380 / NETSurveillance` ⇒ IPC 常用 Web 栈 |
| ④ 判定 | 端口 + 协议指纹 + SSDP + **MAC 厂商 OUI** + IP 角色（网关 / 随机 MAC）合成 0~3 级 | 分级只认**证据强度**，见 §11.2 |

**风险分级（`Lan::classify`，纯函数，QA 可自检）**

| 级 | 触发条件（按优先级） |
|---|---|
| **3 高危** | `RTSP ... 视频流可直读`（200 + `m=video`，即**没有密码就能拉流**）／任何 RTSP 应答（401/403/200）／端口 34567（雄迈）／37777（大华）／8000（海康 SDK） |
| **2 可疑** | IPC 专有 Web 栈（goahead / boa / hipcam / v380 / ipcam / netsurveillance / xmeye / onvif / dvr / nvr / cam）／SSDP 报安防设备／**安防厂商 OUI**（海康/大华/Axis/Vivotek/Foscam）／8554 / 8001 |
| **1 关注** | 未知厂商 **且 非网关** 且确实暴露了端口或答了 SSDP |
| **0 普通** | 其余（含**网关** —— 开 80/答 UPnP 都是正常行为，不许当噪点） |

**一句话行为**：`摄像头` = level 3；`疑似摄像头` = level 2；`安防设备` = OUI 命中但没探到东西；
`手机/平板` = 随机 MAC；`路由器/网关` = 末段 .1；`打印机` = 9100；`电脑/NAS` = 22/445。

### 11.2 无线嗅探页（`src/platform/PgSniff.{h,cpp}`）—— 把"看不见的"变成看得见

| 步 | 做法 |
|---|---|
| ① 切监听 | nl80211 `SET_INTERFACE` 把**闲置的 wlan1** 改成 `MONITOR`（不碰 wlan0 / wpa_supplicant / zknet） |
| ② 抓帧 | `AF_PACKET` `SOCK_RAW` 绑到 wlan1，`SO_RCVTIMEO=250ms`（让阻塞 recv 定期返回，`stop` 标志才来得及生效） |
| ③ 解析 | radiotap 只取 **位3 CHANNEL** + **位5 DBM_ANTSIGNAL**（必须按位号顺序走 + 各自的对齐，否则信道/RSSI 整体错位）；802.11 取 `addr2`（发射者）+ 管理帧 subtype + **SSID element** |
| ④ 还原 | `stop()` → 停线程 → `SET_INTERFACE wlan1 STATION`；**`onUI_quit` 必调**（唯一收尾点） |

**抓到什么（这才是它比手机 App 强的地方）**

- **同信道所有发射者**：AP 和 **STA** 都算。STA 平时完全隐身，只有空口看得见；
- **radiotap 里的真 RSSI / 信道** ⇒ 可以像"热点猎手"一样定位**任何**设备（不限 AP）；
- **probe request 的 SSID** = 这台设备**正在找**哪个网络（iOS 永远拿不到）；
- ★★ **assoc request 的 SSID 是明文的** ⇒ **隐藏网络的名字就是这么解出来的**。
  实现：beacon 里没有 SSID 的 AP 标记为**隐藏 AP**；之后谁向它发 assoc request，
  就把那个明文 SSID 回填到该 BSSID 上并标 `隐藏AP✓`，同时计入"顺听 SSID"。
  —— 这把 §9.1 里"隐藏 SSID 拿不到名字"的结论**直接翻掉了**。

### 11.3 真机验收（2026-09-15，adb 20080411 / wlan0 192.168.0.125）

**端到端实证**：在 PC（192.168.0.110）上起了一个**假 IP 摄像头**
（554 回 `RTSP/1.0 200` + SDP `m=video` + `a=rtpmap:96 H264/90000`；80 回 `Server: GoaHead-Webs`），
让设备主动去探它：

```
probeLogic: cmd lanprobe 192.168.0.110
PgLan: 表里没有 192.168.0.110 -> 单独踢一次 ARP
PgLan: host ip=192.168.0.110 mac=34:c9:3d:ad:26:63 vendor='-' kind='摄像头'
       ports='80 554' level=3 why='视频流可直读'
       note='RTSP 视频流可直读(H264) · 80:GoAhead-Webs'
```

| 项 | 判据 | 实测 |
|---|---|---|
| 邻居发现 | 日志 | 一轮 5~6 台（**比 shell ping 扫段多**）：路由器 + 2~3 台手机（随机 MAC） + 其他 |
| 端口 + 指纹 | 日志 | 假摄像头 `ports='80 554'`、`RTSP 视频流可直读(H264) · 80:GoAhead-Webs` → `level=3 kind=摄像头` ✅ |
| 顺带发现真实设备 | 日志 | **192.168.0.120 开着 8001** → `疑似摄像头 / IPC 备用端口`（level 2）—— 本网段里真有这么一台 |
| 无假阳性 | 日志 | 路由器 `level=0 kind=路由器/网关`（开 80 + 答 UPnP 都不当风险）；两台手机 `level=0 手机/平板` ✅ |
| 判定自检 | 日志 | `lantest` 15 条：高危 4/4、可疑 5/5、关注 1/1、**普通 4/4 不误报** ✅ |
| OUI 表 | 日志 | `oui` 样例：海康/大华/Espressif/TP-LINK/Intel 命中；`32:3d:…` 正确判为**随机 MAC** ✅ |
| 含判据 | 日志 | `ssdp 192.168.0.1 server='UPnP/1.1 MiniUPnPd/1.8'`（**只打一行**，去重生效）✅ |
| 嗅探切模式 | 日志 | `wlan1 已切成 MONITOR` → `开始抓包`；`SET_INTERFACE` 返回 0 ✅ |
| 嗅探真数据 | 日志 | `信道 149`、`AP 94:d9:b3:2b:c9:e3 rssi=-40 ssid='TP-LINK_5G_C9E1'(beacon)`、多个 **STA probe-req**（含随机 MAC `fa:8d:18:…`）✅ |
| 嗅探还原 | `nlprobe` | `wlan1 type=STATION`（stop 后）、`wlan0` ping 192.168.0.1 **2/2 通 1.7ms**（全程不受影响）✅ |
| 四页渲染 | 逐像素 | 四个页签各自选中态 `#3A3A3C` / 未选 `#1C1C1E`；卡片 `#1C1C1E`；按钮 蓝 `#0A84FF` / 灰 `#2C2C2E` / 红 `#FF453A`；行卡文字墨迹正常 ✅ |
| 退出还原 | 日志 | `onUI_quit` → `PgSniff: 还原 wlan1 -> STATION 成功` ✅ |

截图：`docs/shot_probe_lan.png`（局域网页，5 台设备）、`docs/shot_probe_sniff.png`（嗅探页）、
`docs/shot_probe_wifi.png` / `shot_probe_bt.png`（四页签版）。

### 11.4 用法（界面上怎么走）

- **局域网页**：连上一张网 → `扫描局域网`（邻居 + SSDP + 端口指纹，10~25 秒，进度百分比显示在状态卡）
  → 列表按 IP 排序，每行三行信息：**类型标签 / 开放端口** · **IP / MAC + 厂商** · **协议指纹**；
  点一行 = **只深探这一台**（快）；`深探端口` = 把已发现的都探一遍。
- **嗅探页**：`开始嗅探` → 状态卡显示模式/信道/运行秒数，三格统计（发射者 / 帧数 / 顺听 SSID），
  列表每行 = MAC · 角色（AP / STA / **隐藏AP✓**）· RSSI · 帧数 · `probe '网络名' · 风险 · Ns前`。
- 两个页都是**页签切走不停**（嗅探会继续跑，数据不丢）；**退出应用才会停并还原 wlan1**。

### 11.5 这一轮新增的坑

见 §10 的 12~17（`select` 不循环漏端口 / `lanprobe` 静默返回 / `kind[]` 截断 /
网关误报"关注" / SSDP 刷屏 / **用抓屏而不是日志判"QA 通道是否活着"**）。

额外两条实现纪律：

- **每个整屏页各带一套页签**（4 页 × 4 段 = 16 个控件），**不做"共享浮层页签条"** ——
  整屏浮层"显示着"就会吃掉全系统**控件级**触摸（工程记忆第 17 条，血案是状态栏）。
  代价是加页签要四页一起改（漏一页的症状 = "某页页签不亮/点了不动"）。
- **`wlan1` 的类型切换是全局状态**，所以 `stop()` 一定要还原；`onUI_quit` 是唯一收尾点
  （不要在每个返回按钮里各写一遍，总会漏一个）。

### 11.6 ★ 能力对照：为什么这套东西**强过** iOS 那个 App

先说清参考 App 的真实做法（媒体实锤的是 **Fing** 一系）：iOS **根本不给**"周边 WiFi 列表"，
所以所谓"扫描附近所有 WiFi 网络"是营销话术；它实际靠的是
**局域网主动扫描（ping/ARP + MAC 厂商）** + **BLE RSSI 追踪**。

| 能力 | iOS 那个 App | 本应用（v1.22） | 谁强 |
|---|---|---|---|
| 周边 WiFi 列表（SSID/信道/加密/RSSI） | ❌ 系统不给 | ✅ 完整 AP 表 + 信道占用分析 + **信道建议** | **我们** |
| 局域网设备扫描（IP/MAC/厂商） | ✅（这是它的核心） | ✅ ARP 扫段（实测 5~6 台/一轮） + **精选 OUI 表**（约 240 条，含安防厂商） | 平手 |
| **主动确认"这是一路视频流"** | ❌ 只给"可能有摄像头"的猜测 | ✅ **RTSP DESCRIBE**：回 200+`m=video` 就是**铁证**；还报出编码（H264）+ 是否**未鉴权可直读** | **我们** |
| IPC 私有端口识别（34567/37777/8000/8001/8554） | ❌ | ✅ 按厂商私有端口判型（雄迈/大华/海康） | **我们** |
| HTTP 栈指纹（GoAhead/Boa/Hipcam/V380/NETSurveillance） | ❌ | ✅ 抓 `Server:` 头判 IPC | **我们** |
| SSDP/UPnP 设备型号 | 部分 | ✅ 发 M-SEARCH 收 `SERVER:`（`MiniUPnPd` / `IPCAM/1.0` …） | 平手偏我们 |
| BLE 设备 + RSSI 追踪 | ✅（CoreBluetooth） | ✅ 同样的 BLE 扫描 + **热点猎手**（12 段条 + 蜂鸣 + 峰值/平滑） | 平手 |
| **物理定位"任何"设备**（不限 AP/不限 BLE） | ⚠️ 只能定位 BLE 设备 | ✅ 三种目标都能追：AP（RSSI）/ BLE（RSSI）/ **局域网设备（按 IP 反查它所在的 AP）** | **我们** |
| **看见 STA（不连网也能看空口）** | ❌ iOS 绝对做不到 | ✅ **monitor 模式**顺听同信道：发射者 MAC + **真 RSSI** + 帧数 | **我们**（碾压） |
| **隐藏网络的名字** | ❌（只能看到"有个隐藏网络"） | ✅ 从 **assoc request 的明文 SSID** 解出来，界面标 `隐藏AP✓` | **我们**（碾压） |
| 目标设备**在找哪个网络**（probe request） | ❌ | ✅ 列表里直接写 `probe 'TP-LINK_5G_C9E1'` | **我们**（碾压） |
| 全信道 / 4G / 有线摄像头 | ❌ | ❌（单射频只能看一个信道；4G 无硬件） | 都不行 |

**为什么能做到 iOS 做不到的**：我们是**嵌入式 Linux + root**，
① 可以直接操作网卡（nl80211 切 MONITOR）—— iOS 连原始套接字都不给；
② 能读 `/proc/net/arp`、能开原始套接字、能自己写 TCP/RSTP 客户端；
③ `wlan1` 本来就闲置（P2P 起不来），白送一个监听口。

**一句话**：iOS 那个 App 是"**猜**"（MAC 厂商 + 端口猜类型），
我们这个能"**验**"（RTSP 回 `m=video` 就是视频流）、能"**看**"（monitor 顺听空口）、
能"**解**"（隐藏 SSID 从 assoc 里解名）。**从"猜测"升级到"取证"** —— 这是本质差别。


---

## 12. 「目标网模式」：切网不失联（存盘 + 还原，v1.25）

> 起因：§9/§11 一直挂着一个"代价"——**要主动探测就必须连进目标网络**，
> 而 zknet 的网络配置里**只保留一个 network 条目**（`update_config=1` + `SET_NETWORK 0`），
> 落盘在 `/data/misc/wifi/wpa_supplicant.conf`：
> **连一次别的网，原来那张网的 SSID+密码就被顶掉了**（用户得重新输密码才能回家，
> 血案见 `docs/wifi-app.md` 末尾）。
> 用户提的方案：**切开之前存一份，退出模式之后再恢复**。这一节就是它的落地与实测。

### 12.1 机制：存什么、怎么还原

平台层 `src/platform/PgWifiCfg.{h,cpp}`（跨应用通用：探针和 WiFi 应用都用它）：

| 动作 | 做什么 |
|---|---|
| **存**（`keepHome`） | ① `wpa_supplicant.conf` **整份拷贝** → `/data/misc/wifi/pg_home.conf`；② 解析出的 `ssid=`/`psk=` → `pg_home.meta`（给 UI 显示 + 兜底重连）。**两个文件都 chmod 0600**（里面有明文密码） |
| **还原**（`leaveTargetMode`） | ① `pg_home.conf` 覆盖回 `wpa_supplicant.conf`（**原子写：先写 `.tmp` 再 rename**，避免半截文件把两边都毁掉）② 通过 wpa_supplicant 控制口三步：`RECONFIGURE` → `ENABLE_NETWORK all` → `RECONNECT` |

★★ **为什么存"整份文件"而不是只记 ssid/psk**：原文件里可能还有 `scan_ssid=1`
（连隐藏网络的开关）、`key_mgmt`、`priority` …… 只回写 ssid/psk 会把这些丢掉
（**隐藏网络就再也连不上了**）。整份拷贝回去是**逐字节等价**的。

★★ **板子上没有 `wpa_cli`** ⇒ 控制口客户端自己写：`/dev/socket/wlan0` 是 wpa_supplicant 的
ctrl_interface（unix **datagram** socket），客户端要**自己 bind 一个本地路径**（`/tmp/pg_wpa_ctl`）
再 `sendto` 过去，应答会发回我们。DGRAM 失败会自动退化成 STREAM 再试一次。

★ **"回家"不是切回接口类型，而是"配置 + 让上层重新连接"**：文件还原只是第一步，
address（IP）是 **zknet 的连接事件里 DHCP/静态配置**给的 ⇒ 还原后若 8 秒还没连上，
页面里的 `tickReconnect()` 会用 **zknet `connect(ssid, psk)`** 再兜一次
（psk 从快照里取，用户不用重输）。

### 12.2 两个入口（都做了）

1. **探针 · 局域网页顶栏「目标网 / 恢复原网」**（`BtnLanMode`）：
   - 进：`enterTargetMode()` 存快照 + 打标记；按钮**变绿**、状态卡第二行显示
     `当前网 · 原网 XXX 已存`；第三行提示"去 WiFi 应用连上目标网，回来点扫描"。
   - 出：`leaveTargetMode()` 还原 + 重连（按钮回灰）。
   - ★ 自动收敛：目标网模式期间**真的连过别的网**、之后又回到原网络 ⇒ 自动退出模式
     （否则按钮一直写着"恢复原网"很迷惑）。**必须等"真的连过别的网"**再判，
     不然刚进模式那一刻（还在原网上）就被清掉了。
2. **WiFi 应用 · 任何一次"发起连接"之前**（`keepHomeBeforeSwitch()`，挂在
   `pick` 开放直连 / 密码框确认 / QA `conn` 三条路径上）：自动存一份"当前这张网"。
   再加 QA：`snap` / `snapinfo` / `restore` / `wpa <命令>` —— **不必开探针应用也能回家**。

### 12.3 真机验收（2026-09-15，adb 20080411）

判据全部是**看得见的东西**（配置字节 / IP / ping），不是"日志说成功"：

| 步 | 判据 | 实测 |
|---|---|---|
| ① 进目标网模式 | 快照文件 + 权限 + UI | `pg_home.conf` 102 字节、`pg_home.meta` 0600（`-rw-------`）；按钮 `#30D158` 绿 ✅ |
| ① 快照正确性 | 逐字节比对 | `pg_home.conf` **与原 conf 逐字节相同** ✅ |
| ② 模拟切网（conf 换成目标网 + `RECONFIGURE` + `REASSOCIATE`） | 掉线 | `ifconfig` = `Address not available`、ping 不通 ✅（复现了"被打断"） |
| ③ 退出模式 | 日志 | `已把 pg_home.conf（102 字节）覆盖回 wpa_supplicant.conf（原 101 字节）` → `RECONFIGURE OK` → `ENABLE_NETWORK all OK` → `RECONNECT OK` ✅ |
| ③ 配置还原 | 逐字节比对 | **与原 conf 逐字节相同** ✅ |
| ③ 自动回家 | IP + ping | `wpa_supplicant 自己连上` → `192.168.0.125`、`ping 192.168.0.1` **2/2 通 2.7ms** ✅ |
| WiFi 应用侧自动存盘 | 日志 | `conn PG-TEST-TARGET` 前自动打出 `★ 已保存原网络 'TP-LINK_5G_C9E1'` ✅ |
| WiFi 应用侧回家 | 日志 + ping | `restore` → 还原 + `RECONFIGURE/RECONNECT OK` + 连通 ✅ |
| 跨重启 | 文件 | 快照在 `/data`（**不是 /tmp**）⇒ 掉电/重启后 `wifi state` 仍读得到 `hasHome=1` ✅ |

截图：`docs/shot_probe_lan_target.png`（模式已开，按钮绿）、`docs/shot_probe_lan_mode.png`（还原后，按钮灰）。

### 12.4 这一节踩到的坑（都很值得记）

1. ★★ **`ENABLE_NETWORK all` 必须有，否则"还原成功"是假的**：
   切网过程中如果对目标网**认证失败**，wpa_supplicant 会把那张 network 标成
   **`[TEMP-DISABLED]`**（`LIST_NETWORKS` 里能看到），而这个标记是**跟 network id 绑定**的；
   还原后若不显式启用，原网络可能一直 disabled ⇒ `REASSOCIATE`/`RECONNECT` **只回 `OK` 却
   永远不关联**（症状：`wpa_state=DISCONNECTED`、`ifconfig` 没 IP，看着像"还原失败"）。
2. ★★ **别用"手动 ifconfig 打回 IP"验收**：`address` 来自 zknet 的连接事件，
   打回去的地址**不会被 zknet 认**（它的 `state` 里还是 `connected=0 ip='0.0.0.0'`），
   下一个动作（扫描/切换）就又乱。**要验收就看 IP + ping，要修就让它自己连**。
3. ★ **`wpa DISCONNECT` 会把 wpa_supplicant 置成"主动断开"态**（要 `REASSOCIATE`/`RECONNECT`
   才恢复）—— 排查时随手发它，会把局面搞得更差。诊断优先用只读命令：
   `STATUS` / `LIST_NETWORKS` / `SCAN_RESULTS`。
4. **`LOGD` 是宏，`if/else` 必须带花括号**：`if (...) LOGD(...); else ...` 会编译报
   `'else' without a previous 'if'`（宏展开后跟的 `;` 把 if 语句终结了）。
5. **权限会被"重写"打回去**：`enterTargetMode()` 会在 `keepHome()` 之后再写一次 meta ⇒
   只在 `keepHome` 里 `chmod 0600` 会被那次重写重置回 0666。**权限要收在写函数自己里**。
6. **"模拟切网"最忠实、最安全的做法**：把 conf 换成目标网 + `RECONFIGURE` + `REASSOCIATE`
   —— 与真实切网后的落盘状态**完全一致**，而且**不需要第二台真 AP**。
   ⚠️ 但记得先 `adb push` 一份原 conf 到 PC 做安全网（本轮靠它救回两次）。
7. **搞坏了怎么救**：`adb reboot`。重启后 zknet 会**从 conf 重新走完整流程**（关联 + DHCP），
   网络自己就回来了（本轮实测 40 秒内 `192.168.0.125` + ping 通）。
8. ★ **`kill -9` 重启 zkgui 之后，"WiFi 没网"是常见副作用**（本轮又踩一次）：
   现象是 `ifconfig wlan0` 有 IP、但 flags 里**没有 `running`**、ping 全丢；
   `wpa STATUS` = `wpa_state=DISCONNECTED`、`LIST_NETWORKS` 里 network 0 无 flag。
   ⇒ **发一次 `wpa REASSOCIATE` 立刻就好**（实测 `COMPLETED` + ping 3/3、1.3ms）。
   所以调试循环里的顺序应当是：`kill -9` → 等起来 → **顺手 `wpa REASSOCIATE`（或先 ping 一下确认）**
   → 再做后续验证。**否则会把"网络本身没连上"误判成"新功能把网搞坏了"。**


---

## 13. 全信道跳扫：嗅探能扫别的信道了（v1.27）

> 起因：用户一句反问 —— *「嗅探功能可以断开网络，然后可以扫描到其他通道的才对呀」*。
> 这句话**是对的**，而 §11 里那句"单射频：monitor 只能停在 wlan0 所在的那个信道"是**不完整的**。
> 这一节就是把它做对的过程与证据。

### 13.1 先把约束测清楚：占用信道的是**接口**，不是"连接"

三级对照实验（`tools/nlprobe.c`，同一块板子、同一个 monitor 口 wlan1）：

| # | wlan0 的状态 | `SET_CHANNEL wlan1 6` | 结论 |
|---|---|---|---|
| ① | **关联着**（正常上网） | `-16 EBUSY` | 射频被占 |
| ② | 接口 up，但**没关联**（`Address not available`） | **仍然 `-16 EBUSY`** | ★ 关键：**只发 DISCONNECT 是不够的** |
| ③ | **接口 down**（`ifconfig wlan0 down`） | **返回 0** ✅ | 射频释放了 |

⇒ **必须真的把接口关掉**。③ 之后 12 个信道一次全部切换成功：

```
CH1 (2412MHz): 188 帧 / 20 发射者 / SSID 15  [ADS_APP_Wi-Fi5] [NXJT] [ChinaNet-bsyX] [TPLink_zkswe] …
CH6 (2437MHz):  77 帧 /  7 发射者 / SSID 5   [ChinaNet-xqvW] [aWiFi] …
CH36(5180MHz): 166 帧 /  9 发射者 / SSID 6   [NXJT-5G] [ChinaNet-xqvW-5G] [zkswe-soft_5G] …
CH149(5745MHz): 17 帧 /  2 发射者 / SSID 1   [TP-LINK_5G_C9E1]
CH165(5825MHz):  0 帧
（完整 12 个信道，每信道都抓到了帧；2.4G 与 5G 都能看）
```

### 13.2 实现（`src/platform/PgSniff.{h,cpp}` + `probeLogic.cc` + `probe.html`）

**平台层**（`PgSniff`）：

| 新增 | 说明 |
|---|---|
| `setHop(bool)` | 开/关跳信道。开的时候立刻切到第一个信道，不等 worker 下一拍 |
| `hopCurChan / hopScanned / hopRounds / hopFail` | 状态（UI 与日志用） |
| `hopChanNumAt(i) / hopChanFrames(i) / hopChanTotal()` | 信道表与**每信道帧数**（"空口占用地图"） |
| `SniffPeer.chan` | 每个发射者**最后在哪个信道被看到**（跳扫模式下最有价值的一列） |

- **信道表** = `1..13 + 36/40/44/48 + 149/153/157/161/165`（共 22 个），每信道驻留 **500ms**
  （beacon 间隔 100ms ⇒ 每个 AP 至少 5 个 beacon）。
- **切换时机**在抓包线程里（`recv` 有 120ms 超时，所以切换精度 ≈ 超时值）。
- ★★ **必须按 radiotap 的 freq 过滤**：切信道之后 socket 缓冲里还压着上一信道的帧，
  不过滤就会把两个信道的数据混在一起（症状：CH1 的统计里冒出 CH36 的 AP，设备归属信道全乱）。
- `nl80211SetChannel()` 的 recv **带 select 超时**（400ms）—— 它跑在抓包线程里，
  内核一次不响应就会把线程永久卡死（`stop` 也救不回来）。
- 切失败**也前进**（否则会卡死在一个信道上，看着像"扫描停了"）；失败次数会打在状态卡上。

**应用层**（进出全信道 + 断网/恢复）：

```
进 = ① 与「目标网模式」互斥（先退出它）
     ② **先存快照**（整份 wpa_supplicant.conf + ssid/psk）—— 关掉网卡就不知道原来连的是谁了
     ③ wlan0 down        ← ★ 关键一步
     ④ PgSniff::setHop(true)
出 = ① PgSniff::setHop(false)
     ② ★★ **停嗅探**（`Sniff::stop()` → wlan1 切回 STATION）—— 见 13.4 的坑 10：
        单射频下 monitor 口留着会**干扰 wlan0 收包**（DHCP 拿不到 IP）
     ③ wlan0 up
     ④ 有快照：还原 + RECONFIGURE → ENABLE_NETWORK all → RECONNECT（复用 §12 的机制）
        无快照：**也要** RECONFIGURE → ENABLE_NETWORK all → **REASSOCIATE**（见 13.4 的坑 2）
```

**界面**（嗅探页）：顶栏加「全信道」开关（开=绿底黑字「全信道✓」）；
状态卡 CH 位显示 `CH149 · 128/22`（金色）、第二行显示轮数与失败次数；
提示行在全信道模式下变**红字**：`⚠ WiFi 已断开（原网 'XXX' 已存，退出自动恢复）`；
列表每行第二行前面加 `CH36` 前缀。顶栏位置：`x=326 w=74`（让开标题 260 宽与右侧刷新键 404）。

### 13.3 真机验收（2026-09-15，adb 20080411）

| 项 | 判据 | 实测 |
|---|---|---|
| 进入全信道 | 日志 | `WifiCfg: wlan0 down（原来 up）-> 成功` → `★ 全信道模式 ON（已断开 WiFi，22 个信道轮扫）` ✅ |
| 跳信道真的切了 | 日志 | `跳信道=1 当前CH149 已扫128 **轮5 失败0**`（22 个信道全切成功）✅ |
| 每信道帧数 | 日志 | `CH1=6 CH2=2 CH3=5 CH4=2 CH5=6 CH6=20 CH7=2 CH8=10 CH9=5 CH10=6 CH11=23 CH12=3 CH13=2 CH36=27 CH40=31 CH44=22 CH48=22 CH149=2 …` ✅ |
| 抓到的设备数 | 日志 | 一轮跳扫 **28 个发射者 / 197 帧**（非跳信道时同环境约 10 来个）✅ |
| 扫出的 SSID | nlprobe | 12 个信道共 30+ 个 SSID（含 5G 的 `TP-LINK_5G_C9E1` / `zkswe-soft_5G` / `ChinaNet-xqvW-5G`…）✅ |
| 界面 | 逐像素 | 「全信道」按钮 `#30D158` 绿；CH 位金色（`#D7880D` 抗锯齿边）；提示行**红字 170 px**；行卡 `#1C1C1E` ✅ |
| 退出恢复（无快照路径） | IP + ping | 只 up 不够（见坑 2）⇒ 补上 REASSOCIATE 后：`192.168.0.129` + `ping 192.168.0.1` **2/2 通 1.8ms** ✅ |

**固化版（v1.27）完整闭环实测**（`tools/upgrade_device.sh 1.27.0` 之后、`/proc/PID/maps` 确认跑
`/res/lib/libzkgui.so`、`/tmp/lib` 不存在）—— 一次不间断跑完，**全程无需人工干预**：

```
① 进（网络连着 ⇒ 有快照）
   PgWifiCfg: ★ 已保存原网络 'TP-LINK_5G_C9E1'（conf 102 字节，psk 长度 13）
   WifiCfg: wlan0 down（原来 up）-> 成功
   probeLogic: ★ 全信道模式 ON（已断开 WiFi，22 个信道轮扫）
② 跳扫（约 36 秒）
   PgSniff: dump 共 27 个发射者 / 147 帧 / 跳信道=1 已扫70 轮3 失败0
   信道帧数 CH1=0 CH3=1 CH6=10 CH8=9 CH11=3 CH36=2 CH40=10 CH48=15 …
③ 出
   PgSniff: 跳信道 OFF（停在 CH165，累计切换 66 次 / 2 轮 / 失败 0 次）
   PgSniff: 抓包线程退出（共 131 帧）
   PgSniff: 还原 wlan1 -> STATION 成功 (err=0)          ← ★ 不再留 monitor
   probeLogic: 全信道退出时一并停了嗅探（wlan1 回 STATION）
   PgWifiCfg: 控制口 = /dev/socket/wlan0（候选 0）
   PgWifiCfg: 已把 pg_home.conf（102 字节）覆盖回 wpa_supplicant.conf（原 102 字节）
   PgWifiCfg: RECONFIGURE -> OK / ENABLE_NETWORK all -> OK / RECONNECT -> OK
④ 网络恢复（自动，25 秒内）
   D/DHCP: server 192.168.0.1, lease 7200 seconds → configuring wlan0
   wlan0: ip 192.168.0.129 flags [up broadcast running multicast]
   ping 192.168.0.1 → 3/3 通，avg 2.4ms
```

| 项 | 判据 | 实测 |
|---|---|---|
| 固化后跑的是新版 | `/proc/PID/maps` | `/res/lib/libzkgui.so`（4320580 字节）、`/tmp/lib` 不存在 ✅ |
| 新控件在位 | 逐像素 | 嗅探页顶栏「全信道」按钮 `#30D158`（开）/ `#2C2C2E`（关）✅ |
| 提示行（新增文案） | 逐像素 | 红字 264 px（`⚠ 已断网（原网 … 已存）· 退出会一并停止嗅探并恢复网络`）✅ |
| 跳扫 | 日志 | 累计切换 **66 次 / 2 轮 / 失败 0**；`CH6=10 CH8=9 CH40=10 CH48=15 …` ✅ |
| ★ 退出即恢复 | 日志 + IP + ping | 停嗅探 → up → 三步 OK → `DHCP lease 7200` → **`192.168.0.129` + ping 3/3 2.4ms** ✅（**全自动**） |

### 13.4 这一轮的坑（每条都值得记）

1. ★★ **占用射频的是"接口"不是"关联"**：wlan0 **up 但没关联**照样 `EBUSY`。
   ⇒ 想跳信道只能真 `down` 接口（本板单射频 + 驱动的 chanctx 不支持多信道）。
   别被"断开连接了应该就空出射频了"的直觉骗了（我第一版就是这么想的，实测打脸）。

2. ★★ **接口 down/up 循环之后，wpa_supplicant 不会自己回来**：
   退出全信道时只做 `setStaIfaceUp(true)` 的话 —— 接口是 up 了，但
   **flags 里没有 `running`、没有 IP**（`Address not available`、ping 不通）：
   wpa_supplicant 认为自己是断开的，zknet 也等不到连接事件。
   手动发一次 `RECONFIGURE` + `REASSOCIATE` **立刻恢复**（实测 IP + ping 1.8ms）。
   ⇒ **凡是"接口 down/up 循环"的套路，都要补这一步"唤醒"**。

3. ★★ **`fun launch` 会把设备压垮（本板内存只有 55MB）**：
   `/tmp` 是 **tmpfs（占 RAM）**，一次 launch 要推 **6.8MB**（ui + images + audio + font + lib）。
   MemFree 从 16MB 掉到 ~9MB ⇒ **OOM / 看门狗重启** ⇒ `/tmp` 被清空 ⇒
   **回落 `/res` 固化旧库** —— 现象看起来是"我明明 kill -9 重启了，跑的还是旧库"。
   **判据**：`cat /proc/PID/maps | grep libzkgui` 看是 `/tmp/lib/` 还是 `/res/lib/`
   （`ls /tmp/lib/libzkgui.so` 也能一眼看出 /tmp 是否已被清空）。
   本轮为此绕了好几圈（几次 `adb reboot` 之后都白干了）。
   ⇒ **调试部署后要立刻做验证**；要长期稳定跑，必须**固化**（`fun pack`）。

4. **fun 工具链会自动往 logic 文件追加"未实现回调的桩"**（带 `LOGD_TRACE`）：
   给新控件写自己的回调时，**先删掉那个自动桩**，否则报
   `redefinition of 'bool onButtonClick_Xxx(ZKButton*)'`。

5. **信道必须按 radiotap freq 过滤**（第 13.2 条），否则统计混信道。

6. **全信道模式与「目标网模式」互斥**：一个要"连进目标网扫"，一个要"断开网络扫"，
   同时开没有意义；进全信道时若发现目标网模式开着，先退出它。

7. **`nlprobe` 的教训**：对**非 monitor** 接口调 `SET_CHANNEL` 返回 **`-95 EOPNOTSUPP`**，
   而对 monitor 接口、射频被占时返回 **`-16 EBUSY`** —— 两个错误码要分清，
   否则会把"忘了把 wlan1 切成 monitor"误判成"驱动不支持"（绕了半圈）。

8. ★★ **wpa_supplicant 控制口的路径不是固定的**（收尾时踩到，症状 = "退出全信道后
   网络恢复不了"，而日志只有一行 `控制口命令 'RECONFIGURE' 失败（No such file or directory）`）：
   - `init.rc`：`wpa_supplicant -iwlan0 -Dnl80211 -c… -C/dev/socket/` ⇒ 控制口 = `/dev/socket/wlan0`；
   - 但 **conf 里的 `ctrl_interface=` 会覆盖命令行** —— 写成 `ctrl_interface=wlan0` 时，
     wpa_supplicant 把 `"wlan0"` 当**目录**，控制口就不在 `/dev/socket/wlan0` 了
     （实测那会儿 `/dev/socket/` 里只剩 zknet 自己建的 `wpa_cli/wpa_ctrl_<pid>-*` 客户端 socket）。
   ⇒ `PgWifiCfg` 改成：**多候选路径 + 扫目录（含一层子目录）+ 失败后清缓存重探**
   （`wpaCmd` 现在会在 ENOENT 时自动重探一次，wpa_supplicant 重启/换目录也能自愈）。

10. ★★★ **单射频下 monitor 口会干扰 STA 收包 —— 退出全信道必须连嗅探一起停**（本轮最后一个、
    也是最隐蔽的坑）：退出时只 `setHop(false)` 而**留着 wlan1 在 MONITOR**的话，症状是
    **wlan0 关联成功**（`CTRL-EVENT-CONNECTED`）**但永远拿不到 IP**：

    ```
    D/zknet: [CTRL-EVENT-CONNECTED - Connection to 94:d9:b3:2b:c9:e3 completed]
    E/DHCP : Truncated packet      ← 反复出现（DHCP 收到坏包）
    （ifconfig wlan0 一直 Address not available、ping 不通）
    ```

    停掉嗅探（`Sniff::stop()` → `还原 wlan1 -> STATION 成功`）之后**立刻**：
    ```
    D/DHCP: server 192.168.0.1, lease 7200 seconds → configuring wlan0
    wlan0: ip 192.168.0.129  → ping 3/3 1.8ms
    ```
    ⇒ **"STA 联网"与"monitor 嗅探"在单射频上是互斥的**（monitor 口停在某个信道上会干扰
      STA 的收发）。所以 `leaveFullHop()` 里**必须 `sn->stop()`**（并用 `wasSniffing` 把原因打进日志，
      免得下一个人又以为是 DHCP 坏了）。用户想继续嗅探，再点一次「开始嗅探」即可。

11. ★★ **wpa_supplicant 被 kill 之后，init 不一定把它拉回来**（本项目实测：`kill` 掉后
    `/dev/socket/wlan0` 一直不出现，之后所有 `wpa` 命令都 ENOENT）⇒ **`adb reboot`** 是最快的复位。

9. ★★ **别把"运行中被改写的 conf"当成"用户配置"来还原**（我第一版就栽在这）：
   `update_config=1` ⇒ wpa_supplicant 会自己改写 `wpa_supplicant.conf`。
   我备份到的 94 字节版本（`ctrl_interface=wlan0`）其实是**中间态**，
   还原它 ⇒ 控制口消失 ⇒ 唤醒失败。正确的还原值是**与 `init.rc` 的 `-C` 一致**的那个
   （本项目是 `ctrl_interface=/dev/socket/`）。**改这类文件前先想清楚"哪个是稳定态"**。

### 13.5 顺带给 `tools/nlprobe.c` 加的能力（验证工具）

| 模式 | 作用 |
|---|---|
| `setchan <if> <ch>` | 把接口切到指定信道，打印结果（`0` / `-16 EBUSY` / `-95 EOPNOTSUPP`） |
| `hop <if> <每信道ms> <轮数> [信道...]` | 跳信道轮扫，**每个信道分别打印帧数 / 发射者数 / 顺听到的 SSID** |

⚠️ 用它之前记得先把 wlan1 切成 monitor（`settype wlan1 monitor` + `ifconfig wlan1 up`），
用完切回（`settype wlan1 station`）。

---

## 14. 「扫出一堆数据，但不知道谁是摄像头」—— 可观测性改造（v1.28）

用户原话反馈：**"界面怎么用可以看到有风险的设备。这个扫出来很多数据，但不知道谁是摄像头。"**

这句反馈很重要：功能其实**已经能判**（`risk` 自检 19/19 正确、端到端能认出假摄像头 level=3），
但**界面上看不出来** —— 数据全平铺，判定结果藏在右侧 11px 的小字里。
**"功能能跑"和"用户能用"是两件事。**

### 14.1 三个根因（都不是"判定不准"，而是"呈现不出来"）

| # | 根因 | 原来是什么样 |
|---|---|---|
| ① | **列表顺序与风险无关** | 局域网表按 **IP 升序**（读 ARP 时插入排序）、AP 表按 **RSSI 降序** ⇒ 摄像头埋在十几台手机/路由器中间 |
| ② | **没有筛选** | 43 个 AP / 5+ 台设备全铺在屏幕上，用户得自己一行行找 |
| ③ | **风险表达太弱** | 只有右侧一列 11px 小字，而且只有 100px 宽 —— 装不下"视频流可直读"这类判据（**判据本身就是答案，被截断等于没答案**） |

### 14.2 三处改造

**① 列表按风险排序**（风险高的永远在最上面）

- 局域网：`PgLan::rebuildOrderLocked()` —— **风险降序 → 同风险按 IP 升序**
- WiFi：`apRiskDesc()` 取代原来的 `rssiDesc` —— **风险降序 → 同风险按 RSSI 降序**

★★ **只做"视图映射"，绝不重排底层 `hosts[]`**：
`findIdx(reqIp)` / `startProbeOne()` 都按**原下标**定位设备，重排会让"点某一行深探"
探到**别的设备** —— 而且不报错，是最阴的那种 bug。
`hostCopy(i)` 返回"排序后第 i 台"，列表显示 / QA dump / 点击定位**全走它**，天然一致。

**② 「只看风险设备」筛选条**（放在列表与底部按钮之间那 44px 空隙，不动任何现有坐标）

- 两页各一条（`BtnLanOnly` / `BtnApOnly`）：未开启灰 `#2C2C2E`、开启变蓝 `#0A84FF`，
  文字在「只看风险设备（N）」↔「显示全部（N）」之间切换
- 筛选门槛 = **>= 可疑(2)**（"关注"级**不**进筛选 —— 那是泛词，属噪声）
- 同样只做**视图映射**（`sLanView[]` / `sApView[]`）；⚠️ 点击回调必须**映射回真实下标**，
  否则"深探"会探错 IP

**③ 行首色条 + 汇总卡"点名"**

- 每行左侧 5px 色条：**红=高危 / 金=可疑 / 弱灰=关注 / 卡同色=隐形**
  - ⚠️ 色条是 `div.text`（无 picTab）⇒ **不挂圆角图**（5px 宽的小控件挂九宫格会画出怪边），
    只 `setBackgroundColor` + `setBgStatusColor`
- 汇总卡三行从"统计"改成 **结论 / 点名 / 下一步**：
  - L1：`⚠ 高危 1 台疑似摄像头`（原来写"在线 N 台设备"—— 对用户没意义）
  - L2：`192.168.0.110 · 视频流可直读 · 高危`（**直接点名**，不用滚动列表就知道是谁）
  - L3：`列表已按风险排序 · 点下面「只看风险设备」只看它们`

### 14.3 顺带修掉的关键词分级缺陷（真机暴露）

改造过程中在真实环境里发现 **`BE88U_IoT` 被判 2 级"可疑 · 名称疑似监控设备"** —— 它含 `iot`。

问题：`kWatchKw` 里有 `iot / wireless / mini / dv` 这类**过泛**的词 ⇒
智能插座、灯泡、扫地机全被打成"可疑"，**真正的摄像头反而被这些噪声淹没**
（恰好与用户诉求相反）。

新分级（按**证据强度**，不是按"可能相关"）：

| 级 | 放什么 | 例子 |
|---|---|---|
| **3 高危** | 名字里**明说**是摄像头/监控 | `camera` `ipcam` `spycam` `监控` `偷拍` `针孔` |
| **2 可疑** | **明确**的监控迹象（看到就想走过去看一眼） | `cam` `v380` `camhi` `besder` / `esp_/esp-/esp32` 模组 / 词首 `ipc`、`gc-`、`x5-`、`x6-`、`a4-` / `cctv` `nvr` `dvr` / `kids` `baby` |
| **1 关注** | **泛词** —— 智能家居/普通设备也大量使用 | `iot` `wireless` `mini` `video` `eye` `watch` `hidden` `secret` `smart` |
| **0 普通** | 其余 | — |

★ 另外发现**短词必须词首匹配**：`MiniPC-5G` 含 `ipc`（m-i-n-**i-p-c**）⇒ 被判 2 级。
新增 `matchKwPrefix()`：`ipc / gc- / x5- / x6- / a4-` 一律**只认词首**
（`a4-` 尤其危险 —— HP 打印机热点常叫 `DIRECT-A4-…`）。

### 14.4 可复现的验收手段（本轮新增 QA）

| 命令 | 作用 |
|---|---|
| `riskrank` | 打印两个列表的**当前显示顺序**（前 8 条 + level + 判据） |
| `apinject <名> [rssi]` | **注入假 AP**（QA only）—— 真实环境常常一个可疑 SSID 都没有，那种情况"排序对不对"根本看不出来 |
| `apclear` | 清掉注入项 |
| `lanonly 0\|1` / `aponly 0\|1` | 切筛选态 |

★ `apinject` 有两处必须注意（都踩过）：

1. **名字要在空格处截断** —— 否则 `apinject IPCamera-4F2A -85` 的 SSID 会变成
   `"IPCamera-4F2A -85"`（把 rssi 也吞进名字）。
2. ★★ **注入项要"钉住"** —— `handleWifiScanResult` 是整体赋值 `sScan = *infos`，
   而 zknet 会**周期性上报扫描结果** ⇒ 注入项活不过几秒，抓屏时看到的还是旧列表
   （白排查一轮：以为"注入没生效"，其实是"生效了又被覆盖"）。
   现在注入项存 `sInjectAp`（**生产环境恒空** ⇒ 零影响），每次扫描结果到达后重新挂上；
   并且 `apinject` **自己立刻回读排序后的前 3 名**，一条命令就给判据、不依赖后续时序。

### 14.5 真机验收（2026-09-15，adb 20080411）

**WiFi 页**（注入 1 高危 + 1 可疑）：

```
apinject 'IPCamera-4F2A' -85 → level=3
排序后 #0 IPCamera-4F2A  -85dBm level=3   ← 信号最弱，却排第一
排序后 #1 BE88U_IoT      -92dBm level=2   ← 环境里真实存在
排序后 #2 TPLink_zkswe   -34dBm level=0   ← 信号最强，掉到第三
```
（顺带证明「风险优先于信号强度」）

界面逐像素（`docs/shot_risk_wifi.png` / `shot_risk_only.png`）：

| 项 | 判据 | 实测 |
|---|---|---|
| 行首色条 | RGB | 第0行 `#FF453A` 红 / 第1、2行 `#FF9F0A` 金 / 第3行 `#3A3A3C` 灰 / 第4行 `#1C1C1E` 隐形 ✅ |
| 汇总卡 L1 | 红字像素 | 200 px（`⚠ 1 个高危网络 · 附近共 N 个`）✅ |
| 汇总卡 L2 | 红字像素 | 284 px（点名 `IPCamera-4F2A`）✅ |
| 筛选 `aponly 1` | 行色条 | 第3、4行变 `#000000`（空了）；筛选条 `#2C2C2E` → **`#0A84FF` 蓝** ✅ |

**局域网页**（PC 上起假 IP 摄像头 192.168.0.110：554 回 `200 + m=video H264`、
80 回 `Server: GoAhead-Webs`）：

```
riskrank LAN#0 ip=192.168.0.110 kind=摄像头      level=3 why='视频流可直读'   ← 置顶
riskrank LAN#1 ip=192.168.0.120 kind=疑似摄像头  level=2 why='IPC 备用端口'
riskrank LAN#2 ip=192.168.0.1   kind=路由器/网关 level=0
riskrank LAN#3 ip=192.168.0.100 kind=手机/平板   level=0
riskrank LAN#4 ip=192.168.0.125 kind=在线设备    level=0
```
（192.168.0.110 本来排在 100/120/125 之间 —— 现在按风险重排到第一）

界面（`docs/shot_risk_lan.png` / `shot_risk_lan_only.png`）：
第0行 `#FF453A` / 第1行 `#FF9F0A` / 第2、3行 `#1C1C1E`；
汇总卡 L1 红字 176px、L2 红字 198px；筛选后第2、3行变空 + 筛选条变蓝 ✅

关键词分级回归（**逐条跑**，避免 20 行日志被 zknet 刷掉）：

```
BE88U_IoT → 1 关注    SmartHome-2G → 1 关注    MiniPC-5G → 1 关注     ★ 泛词不再误报 2 级
IPC-1234 → 2 可疑     GC-ABCDEF → 2 可疑                            ★ 词首匹配不漏
IPCamera-4F2A → 3     Wireless Camera → 3                           ★ 高危优先于泛词
ESP_1A2B3C → 2        HIDDEN-CAM → 2          TPLink_zkswe → 0
```

### 14.6 这一轮踩的坑

1. ★ **采样点忘了列表容器偏移**：列表 `data-x="12"` ⇒ item 的 `x=8` 对应**屏幕 x=20**。
   我在屏幕 x=8..14 采样，看到"色条是黑的"，白排查两轮（**色条其实一直是好的**）。
   ⇒ 判像素前先算清「容器偏移 + 行高 + 间距」。
2. ★★ **"用户说不好用"和"功能没做对"是两类问题** —— 判定库 19/19 正确，
   但界面上呈现不出来。优化可观测性（排序/筛选/点名/色条）比继续加判定规则收益大得多。
3. ★★ **注入类 QA 必须"钉住"**（见 14.4-2）。
4. ★★ **短词用子串匹配必然误伤**（`MiniPC` 含 `ipc`）⇒ 短词/型号前缀一律**词首匹配**。
5. ★★ **泛词会淹没信号**：`iot / wireless / mini` 上"可疑"级，会把智能家居全标红，
   真正的摄像头反而找不出来 ⇒ **分级按证据强度，不按"可能相关"**。
6. **日志缓冲在"持续刷屏"场景下更小**：`risk` 一次打 20 行 + zknet 每秒 SCAN 事件
   ⇒ 数据全被挤掉，`grep` 永远是空。**改成"逐条查询"（每次 1 行）就稳了**。
   凡是"一次打很多行"的自检命令，在刷屏环境下都要考虑分流。
