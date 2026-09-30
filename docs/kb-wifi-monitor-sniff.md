# 嵌入式 WiFi 监控与嗅探（monitor mode / 跳信道 / 隐藏 AP / 局域网主动探测）——V85X 实测

> 🔍 **检索导引（命中条件）**：用户问「**怎么嗅探 WiFi / 抓空口包 / 抓 802.11 帧**」
> 「**WiFi 监听模式 / monitor mode 怎么开 / monitor 接口**」「**怎么扫到所有信道 / 只能听一个信道 / 换不了信道**」
> 「**SET_CHANNEL 返回 EBUSY / EOPNOTSUPP**」「**怎么找隐藏 SSID / 隐藏网络拿不到名字 / 解出隐藏网络**」
> 「**怎么找隐藏摄像头 / 偷拍设备 / 排查可疑 WiFi 设备 / 防偷拍**」「**局域网里有哪些设备 / ARP 扫段**」
> 「**找 IP 摄像头 / 探摄像头端口 / RTSP 探测**」「**没有 wpa_cli / wpa_supplicant 控制口怎么用**」
> 「**wlan1 有什么用 / 两个 wlan 接口**」「**改完接口类型后网络没了 / 联网失败 DHCP 拿不到 IP**」
> 且平台为 **全志 V851s / V853 / V85X（FlyThings / EasyUI）** → **本篇就是答案**。
> 其中 1~7 节的方法（nl80211 / wpa_supplicant / ARP 探测）**不依赖厂商私有 API，其他嵌入式 Linux 同样适用**，
> 只是实测值来自 V85X；8 节给出换板必须重测的清单。
> 相关：`../devflow/busybox-debug-library.md`（板子上工具极简时怎么补工具）、`../v85x/usb-otg-mode-switch.md`（接口模式切换同类思路）

## 一句话

嵌入式板子上做 WiFi「嗅探/防偷拍」，**只靠 AP 扫描是错的**（偷拍设备主流是 STA 型，根本不发 beacon）；
要看得见必须走 **monitor 模式**，而**单射频下唯一能让 monitor 换信道的前提是把 STA 接口 `down` 掉**
（只断开关联没用，`SET_CHANNEL` 照样 `-16 EBUSY`）；**换完信道抓包必须按 radiotap 的 freq 过滤**；
**退出时必须把 STA 接口 `up` 回来 + 唤醒 wpa_supplicant**（`RECONFIGURE` → `ENABLE_NETWORK all` → `REASSOCIATE`），
并且**必须同时关掉 monitor 口** —— 否则 STA 会「关联成功但 DHCP 永远失败」（`E/DHCP: Truncated packet`）。

## 0. 实测环境（换板逐项重测）

| 项 | 实测值 | 怎么读 |
|---|---|---|
| 板子 / 平台 | 全志 V851s / FlyThings(EasyUI) | — |
| WiFi 模组 | RTL8733BS（Realtek rtw 系） | `lsmod` / `/sys/module/*/version` |
| 驱动 | `8733bs`，`v5.14.1-46-gc…20220113_COEX20211210-2706` | `cat /sys/module/8733bs/version` |
| 驱动类型 | **cfg80211/nl80211** | `ls /sys/class/ieee80211/` 有 `phy0` |
| 接口 | `wlan0`（STA）+ `wlan1`（**实测闲置**） | `ifconfig` / `/sys/class/net` |
| vif 上限 | **2**（再建第 3 个返回 `-22 EINVAL`） | 见 §1 |
| wpa_supplicant | 在跑；启动参数 `-iwlan0 -Dnl80211 -c… -C/dev/socket/` | `/etc/init.rc` |
| 板载无线工具 | **只有** `ifconfig` / `ping` / `wpa_supplicant` | `ls /bin /sbin /usr/bin /usr/sbin` |
| **没有** | `iw` / `iwconfig` / `iwlist` / `wpa_cli` / `hostapd` / `arp` / `nc` / `curl` / `wget` / `busybox` | 同上 |
| 权限 | 应用进程 **uid 0**（可开原始套接字、可读 `/proc/net/arp`） | `ps` 的 USER 列 |

⚠️ **`wlan1` 为什么是闲置的**：`/etc/init.rc` 里有
`service p2p_supplicant … -iwlan1 -Dnl80211 -c/data/misc/wifi/p2p_supplicant.conf -C/dev/socket/`，
但 **`/res/bin` 里根本没有 `p2p_supplicant` 这个二进制**（只有一个 `firmware/` 目录）⇒ 该服务起不来
⇒ **本板的 P2P/Miracast 本来就是坏的**，`wlan1` 没有任何进程在用。
==> 这就是"白得一个监听口"的来源。**换成别的板子要重新确认这一点。**

## 1. 开监控口：只有一条路（新建不行，改类型才行）

```bash
# 先问驱动：它自己声明支持哪些接口类型（只读，安全）
./nlprobe                       # GET_WIPHY 的 SUPPORTED_IFTYPES
#   → ADHOC STATION AP MONITOR P2P_CLIENT P2P_GO     ← 声明支持 MONITOR

./nlprobe mon phy0 mon0         # 新建一个 monitor 口
#   → -22 EINVAL       ← 驱动 vif 上限 = 2，而 wlan0/wlan1 已占满

./nlprobe settype wlan1 monitor # ★ 改**已有**闲置口的类型
#   → 0                ← 成功（GET_INTERFACE 复查 type=MONITOR）
ifconfig wlan1 up
```

**结论**：声明支持 ≠ 能新建。**vif 上限满了就"复用闲置接口"**（改类型而不是新建）。
用完务必切回：`./nlprobe settype wlan1 station`。

⚠️ 对**非 monitor** 接口调 `SET_CHANNEL` 会返回 **`-95 EOPNOTSUPP`**，
别把它当成"驱动不支持"（它只是说"这个接口类型不能设信道"）。

## 2. ★★ 跳信道的真相：占住信道的是"接口"，不是"关联状态"

三级对照实验（同一 monitor 口，只改 STA 口的状态）：

| # | wlan0 的状态 | `SET_CHANNEL wlan1 <freq>` | 说明 |
|---|---|---|---|
| ① | **关联着**（正常上网） | `-16 EBUSY` | 射频被占 |
| ② | 接口 **up 但没关联**（刚 `DISCONNECT`） | **仍然 `-16 EBUSY`** | ★ 只断开不够！ |
| ③ | **接口 down**（`ifconfig wlan0 down`） | **返回 0** ✅ | 射频释放了 |

⇒ **想扫别的信道，必须真把 STA 接口 `down` 掉**。
（"断开连接就空出射频了"是错误直觉 —— 占住信道的是接口所在的 channel context；
单射频驱动 `num_different_channels = 1`，两个接口不在同一信道就不允许。）

③ 之后 12 个信道一次全部切换成功（`nlprobe hop`，每信道 1.5s）：

```
CH1 (2412MHz): 188 帧 / 20 发射者 / SSID 15  [ADS_APP_Wi-Fi5] [NXJT] [ChinaNet-bsyX] [TPLink_zkswe] …
CH6 (2437MHz):  77 帧 /  7 发射者 / SSID 5   [aWiFi] …
CH36(5180MHz): 166 帧 /  9 发射者 / SSID 6   [NXJT-5G] [zkswe-soft_5G] …
CH149(5745MHz): 17 帧 /  2 发射者 / SSID 1   [<自家 5G SSID>]
CH165(5825MHz):  0 帧
```

## 3. 跳信道实现要点（四条，缺一条就出"看着像坏了"的症状）

1. **信道表 + 驻留时间**：本项目用 `1..13 + 36/40/44/48 + 149/153/157/161/165`（22 个），
   每信道 **500ms**（beacon 间隔 100ms ⇒ 每个 AP 至少 5 个 beacon）。切换放在**抓包线程**里按时间片做
   （`recv` 用 120ms 超时，切换精度 ≈ 超时值）。
2. ★★ **必须按 radiotap 的 freq 过滤帧**：切信道后 socket 缓冲里**还压着上一信道的帧**
   （radiotap 里的 freq 是发送时的真实频率）。不过滤的症状：`CH1` 的统计里冒出 `CH36` 的 AP、
   设备归属信道全乱 —— 而"谁在哪个信道"恰恰是跳扫的核心价值。
3. ★★ **`SET_CHANNEL` 的 ACK `recv` 必须带超时**（`select` 400ms）：它跑在抓包线程里，
   内核一次不响应就把线程**永久卡死**（`stop` 标志也读不到、线程退不出）。
4. **切失败也要前进**：否则卡死在一个信道上，看着像"扫描停了"。失败次数要显示/打印出来。

实测一轮（22 信道 × 500ms）：

```
跳信道=1 当前CH149 已扫128 轮5 失败0
信道帧数 CH1=6 CH2=2 CH6=20 CH8=10 CH11=23 CH36=27 CH40=31 CH44=22 CH48=22 CH149=2 …
```

## 4. ★★★ monitor 口会干扰 STA 收包 —— 退出必须连它一起关

**症状（极具误导性）**：退出跳扫后 wlan0 **关联成功**，但**永远拿不到 IP**：

```
D/zknet: [CTRL-EVENT-CONNECTED - Connection to <BSSID> completed]
E/DHCP : Truncated packet      ← 反复出现
（ifconfig wlan0 一直是 Address not available、ping 不通）
```

**根因**：`wlan1` 还留在 **MONITOR**（只停了跳扫、没停嗅探）。单射频下 monitor 口停在某个信道上
会**干扰 STA 的收发**（DHCP 收到坏包）。

**验证与解法**：把监听口切回 STATION（停抓包）后**立刻**：

```
D/DHCP: server <网关>, lease 7200 seconds → configuring wlan0
wlan0: ip <本机 IP>  → ping <网关> 3/3 通 1.8ms
```

⇒ **"STA 联网"与"monitor 嗅探"在单射频上是互斥的**。
凡是有"进监控模式"的功能，**退出路径必须同时停掉嗅探**（并把原因打进日志，免得下一个人去查 DHCP）。

## 5. 接口 down/up 之后必须"唤醒"（否则有接口、没 running、没 IP）

`down` 过 STA 接口再 `up` 回来，**光 up 是不够的**：

| 现象 | 原因 |
|---|---|
| 接口 `up` 但没有 `running` 标志、`Address not available`、ping 不通 | wpa_supplicant 认为自己是断开的，上层（zknet 等）也等不到连接事件 |

**唤醒三步**（顺序不能变）：

```
RECONFIGURE            # 重读配置文件（这之后文件里的 network 才是当前的）
ENABLE_NETWORK all     # ★ 不能少：认证失败过的网络会被标 [TEMP-DISABLED]，不显式启用则后面只回 OK 却永不关联
REASSOCIATE            # 关联回去（有具体 network 时也可用 RECONNECT）
```

- **IP 不是文件给的**：`address` 来自上层框架的连接事件（DHCP/静态）⇒ 唤醒后要等上层跑 DHCP；
  实测 25 秒内自动拿到 IP + ping 通。
- 若能拿到凭据（ssid/psk），**兜底再调一次上层的 `connect(ssid, psk)`** 更稳。

## 6. 没有 `wpa_cli` 时：自己写控制口客户端 + 路径不是固定的

板子上没有 `wpa_cli`，但 wpa_supplicant 的控制口一直开着，可以自己连：

1. **控制口是 unix socket**：客户端要**自己 `bind` 一个本地路径**（如 `/tmp/xx_wpa_ctl`）再 `sendto` 过去，
   应答会发回我们。`SOCK_DGRAM` 失败就退化成 `SOCK_STREAM` 再试一次。
2. ★★ **路径不固定，别写死**：
   - `init.rc` 用 `-C/dev/socket/` 启动 ⇒ 控制口 = `/dev/socket/<ifname>`（`/dev/socket/wlan0`）；
   - 但 **conf 里的 `ctrl_interface=` 会覆盖命令行** —— 写成 `ctrl_interface=wlan0` 时，
     wpa_supplicant 会把 `"wlan0"` 当**目录**，控制口就不在 `/dev/socket/wlan0` 了
     （此时 `/dev/socket/` 里只剩别的进程自建的 `wpa_cli/wpa_ctrl_<pid>-*` 客户端 socket）。
   ⇒ 正确做法：**多候选路径 + 扫目录（含一层子目录） + 失败后清缓存重探**。
3. **排查用的只读命令优先**：`STATUS` / `LIST_NETWORKS` / `SCAN_RESULTS`。
   ⚠️ `DISCONNECT` 会把 wpa_supplicant 置成"主动断开"态（要 `REASSOCIATE`/`RECONNECT` 才恢复），
   随手发它会把局面搞得更差。
4. **`kill` 掉 wpa_supplicant 后，init 不一定把它拉回来**（实测：控制口一直不出现、之后所有 wpa 命令 ENOENT）
   ⇒ **`reboot` 是最快的复位**。

## 7. 只做 AP 扫描是不够的：先分清设备类型，再选手段

| 设备类型 | 行为 | AP 扫描能发现吗 | 正确手段 |
|---|---|---|---|
| **softAP 型** | 自己发热点（多为配网/直连） | ✅ 能 | 扫 AP + 关键词/OUI 判定 + 追 RSSI 定位 |
| **STA 型（主流）** | 连进房间/酒店的 WiFi 往云端推流 | ❌ **看不见**（不发 beacon、不广播任何东西） | **加入同一张网 + 主动探测**（见下） |
| 4G/蜂窝型 | 插 SIM 卡 | ❌ | 只能靠射频/红外（一般板子无此硬件） |

**隐藏 SSID 的准确口径**（很多人会想当然）：

- ✅ **能发现"它"**：beacon 里不带 SSID，但照样拿到 **BSSID / 信道 / 加密 / RSSI**；用 RSSI 能物理定位。
- ❌ **默认拿不到名字**：隐藏 AP **不响应广播 probe request**；要拿名字必须发**定向** probe（包里带 SSID），
  而不知道名字就发不出（鸡生蛋）。
- ✅ **有解**：顺听客户端的 **assoc request** —— 客户端连隐藏 AP 时 SSID 是**明文**的，
  抓到就能把名字回填到那个 BSSID 上（同时也能看到"某设备正在找哪个网络"= probe request 里的 SSID）。
- ❌ 连接：配置里若没有 `scan_ssid=1`，对隐藏网络不会定向 probe ⇒ **连不上**。

**局域网主动探测（找 STA 型设备的四步，实测可用）**

| 步 | 做法 | 要点 |
|---|---|---|
| ① 邻居发现 | 往整个 /24 每个地址发一个 **UDP 包**（内核为发出去**必须做 ARP**）⇒ 读 `/proc/net/arp` | 比 ping 扫段更灵（端口全关的设备也答 ARP）；只取 `Flags=0x2` 完整项；分两轮扫、每 48 个地址歇 40ms（`unres_qlen` 默认 3，灌太猛会被丢）。实测 253 个地址 **7.6 秒** |
| ② SSDP 顺听 | 发 `M-SEARCH`（`ssdp:all` 等）到 `239.255.255.250:1900`，收 2.6s，取 `SERVER:` 头 | 很多 IPC/路由器把型号写进去（`IPCAM/1.0` 等）；**按 IP 去重再打日志**（否则刷屏） |
| ③ 端口 + 协议指纹 | 12 个摄像头端口并发非阻塞 `connect`；对开着的口做指纹：`554/8554` 发 **RTSP DESCRIBE**、`80/8080` 发 **HTTP HEAD** | ★ `RTSP` 回 `200 + m=video` = **确认是一路视频流**（最硬的证据，还能看出编码与"是否未鉴权可直读"）；`Server: GoAhead-Webs / Boa / Hipcam / V380` 等 = IPC 常用 web 栈 |
| ④ 判定 | 端口 + 协议指纹 + SSDP + **MAC 厂商 OUI** + IP 角色 合成风险等级 | 只认**证据强度**；**网关要豁免**（它开 80/答 UPnP 是正常的）；随机 MAC（首字节本地管理位置 1）说明是手机/平板类 |

⚠️ **端口扫描的 `select` 必须循环**：`select` 是"有一个就绪就返回"，写一次就收工会让
**先返回的端口把控制权抢走**，其余端口即使连上了也不被检查 —— 实测症状：对端日志明明记着两个端口都连进来了，
本机只报一个。**少报一个口 = 漏掉一台摄像头。**

## 8. 一页速查（现象 | 第一嫌疑 | 立即验证）

| 现象 | 第一嫌疑 | 立即验证 |
|---|---|---|
| `SET_CHANNEL` 返回 `-16 EBUSY` | STA 接口没 down（up 但未关联也算） | `ifconfig wlan0 down` 再试 |
| `SET_CHANNEL` 返回 `-95 EOPNOTSUPP` | 目标接口不是 monitor / 没 up | `GET_INTERFACE` 看 type；`ifconfig <mon> up` |
| 新建 monitor 口 `-22 EINVAL` | vif 上限满了 | 改成"复用闲置接口"（`settype <if> monitor`） |
| 信道统计里混进别的信道设备 | 没按 radiotap freq 过滤 | 按 freq 丢弃非当前信道的帧 |
| 抓包线程退不出 / 界面卡住 | `SET_CHANNEL` 的 ACK recv 没超时 | 加 `select` 超时 |
| STA **关联成功但没 IP** | ★ monitor 口还开着干扰收发 | 停抓包（监听口回 STATION）后看 DHCP |
| 接口 up 了却没 IP / 无 `running` | 没唤醒 wpa_supplicant | `RECONFIGURE` → `ENABLE_NETWORK all` → `REASSOCIATE` |
| wpa 命令全部 ENOENT | 控制口路径不对（或被 kill 后未重启） | 扫 `/dev/socket/**` 找 `wlan0` socket；不行就 `reboot` |
| 只看到自家 AP、找不到可疑设备 | 偷拍设备是 STA 型 | 走 §7 的局域网主动探测 |
| 隐藏 SSID 没名字 | 隐藏 AP 不答广播 probe | 顺听 **assoc request**（明文 SSID） |

## 9. 换板对照清单（禁止照抄量值）

- [ ] 驱动名/版本、是否 **cfg80211**（`/sys/class/ieee80211/`）、是否声明 **MONITOR**（`SUPPORTED_IFTYPES`）
- [ ] **vif 上限**：新建第 N 个返回什么；有没有**闲置接口**可复用（查 `/etc/init.rc` 的 service 与二进制是否真的存在）
- [ ] STA 接口 `down` 后 `SET_CHANNEL` 是否返回 0（**这决定"全信道"功能能不能做**）
- [ ] 支持的信道范围（2.4G / 5G 高信道是否可用、是否受法规域限制）
- [ ] monitor 口是否干扰 STA（跑一次 DHCP 看有没有 `Truncated packet`）
- [ ] wpa_supplicant 启动参数（`-C` 指向哪）+ conf 里 `ctrl_interface` 是否与之一致
- [ ] 板载有哪些网络工具（有没有 `iw`/`wpa_cli`；没有就得自己写 nl80211 / 控制口客户端）
- [ ] 应用进程权限（能否开 `AF_PACKET` 原始套接字、能否读 `/proc/net/arp`）
- [ ] 内存：`monitor` + 跳扫是长驻线程，`/tmp` 若是 tmpfs 要注意部署方式（见 `deploy-scene-map.md`）

## 10. 参考实现（工程内相对路径）

| 文件 | 内容 |
|---|---|
| `tools/nlprobe.c` | **独立静态可执行**（交叉编译后 push 到设备）：只读列接口/类型/信道、`settype`、`setchan`、`hop` 跳信道轮扫（每信道打印帧数/发射者/SSID）、`sniff` 抓帧统计 |
| `src/platform/PgSniff.{h,cpp}` | monitor 开关 + 跳信道 + radiotap/802.11 解析（真 RSSI、信道、发射者、probe/assoc 里的明文 SSID） |
| `src/platform/PgLan.{h,cpp}` | 局域网主动探测（UDP 踢 ARP + 读 `/proc/net/arp` + SSDP + 端口 + RTSP/HTTP 指纹 + OUI 表） |
| `src/platform/PgWifiCfg.{h,cpp}` | 控制口客户端（自写、多候选路径）+ 配置快照/还原（切网不失联） |

**顺手提两个 MCP 工具名歧义**（本轮踩到，容易让人绕开 MCP 手搓脚本）：
`flythings_build_ui_flow` 名字像"UI 构建流程"，**实际是"编译 + 全量部署到真机"**；
`flythings_pack_upgrade` 听起来像"打包 UI"，**实际是"固化成 `update.img`"**。
建议在 docstring 首行加场景别名（如"实为编译+部署，不限于 UI"）。
