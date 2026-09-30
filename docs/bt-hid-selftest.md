# BLE HID 自检（HID 外设 / 扫描 / TLV 持久化）

> 目标：把"智能遥控器"的**后段流程**跑通 —— 即 BT HCI 通路建立之后的四件事：
> BLE 扫描、HID 外设（手机可发现/连接）、配对与 TLV 持久化。
> 上游（BT 上电 + Realtek 预初始化）见 `bt-bringup.md`。

工程：`bt_probe/`（独立可执行，不依赖 FlyThings UI）
入口：`/tmp/BtBringup selftest [串口] [波特率] [even|none] [窗口秒] [scan 0|1] [rtk 0|1] [hid_adv 0|1]`

```
/tmp/BtBringup selftest /dev/ttyS2 1500000 even 1800 1 1 1
```

---

## 一、自检清单与当前状态

| # | 自检项 | 状态 | 证据 |
|---|---|---|---|
| 1 | 编译无未定义符号（libbtstack.a 已链上） | ✅ | `fun build` 链接通过（HIDS/GAP/TLV 全部解析） |
| 2 | `hci_power_control` → `BTSTACK_EVENT_STATE == HCI_STATE_WORKING` | ✅ | `[BLE] BTSTACK state = 2 (WORKING)` |
| 3 | 扫描能收到 `GAP_EVENT_ADVERTISING_REPORT` | ✅ | 单次自检收到 **1812 条**广播（含 SSID/名字/RSSI） |
| 4 | HIDS/ATT 连接事件 + 手机可发现 | **部分**：连接事件 ✅ / 订阅待手机验证 | 设备侧 `★ LE 连接建立 handle=0x0018`；广播含 HID UUID+Appearance+Name，PC 扫描可发现 |
| 5 | 配对后 TLV 文件生成 | ✅ | `/data/bttlv.db` 已生成（69 字节）+ 回读校验通过 |

第 4 条的"手机可发现"已由 PC 侧独立验证（扫描到 `PocketGame-RC`，广播含 `0x1812`）；
**"HIDS INPUT_REPORT_ENABLE"需要手机/主机真正订阅 HID 报告特征**，见 §四。

---

## 二、架构（btstack 硬规矩，全踩过）

```
主线程                                run loop 线程（唯一能调 btstack API 的线程）
  │                                        │
  ├─ rtk_init(dev)   ← 不涉及 btstack       │
  ├─ socketpair(fds)                        │
  └─ pthread_create ──────────────────────► btstack_memory_init
                                            btstack_run_loop_init(posix)
                                            socketpair[0] 注册成 data source   ← 跨线程投递
                                            TLV 初始化（/data/bttlv.db）
                                            hci_init(H5 + 8E1 + 1500000)
                                            le_hid_setup()（l2cap/sm/att/gatt_client/hids_device）
                                            注册 boot timer → run_loop_execute()
                                                  │
   postMsg(SEND_KEY/STOP) ──写 socketpair────► postHandler（run loop 线程）→ sendKey()
```

三条**必须遵守**的规则（违反会静默卡死或崩）：

1. **所有 btstack API 只能在跑 run loop 的那条线程里调**。
   我最初把 `btstack_run_loop_add_data_source` / `add_timer` 放在主线程 → run loop 起来后
   永远停在 `HCI_STATE_INITIALIZING`，**连 5 秒一次的 tick 日志都没有**（线程阻塞在 `poll()`）。
   参考工程 `server.cpp` 的 `ble_thread` 就是把这一切都放在同一条线程里。
2. **`hci_add_event_handler()` 必须在 `hci_init()` 之后**（它往 `hci_stack` 的链表加节点）。
3. **判定"芯片有没有回应"只能看 HCI 事件码 `0x01~0x5F`**；
   `0x6E` 是 btstack 本地 `HCI_EVENT_TRANSPORT_PACKET_SENT`（"已发出"），会刷屏误导。

其它已落地的设计：
- **HID 报告描述符**原样取自参考工程（Report ID 1 = Consumer Control 位图，ID 2 = Digitizer 触摸）。
- HID 服务在 **GATT 数据库**（`gatt_profile_data.h`，编译期生成的 ATT 表）里，`hids_device_init()`
  在 `att_server_init()` 之后挂上报告描述符。
- **TLV**：`tlv_posix_init_instance("/data/bttlv.db")` + `btstack_tlv_set_instance` +
  `le_device_db_tlv_configure`（配对信息由 `le_device_db_tlv` 写进同一个 TLV）。
- **socketpair + data source** 做跨线程命令投递（后续主工程里 UI 线程 → BT 线程就走这条）。

---

## 三、工作参数（与 `bt-bringup.md` 一致）

| 阶段 | 串口 | 传输 | 波特率 | 校验 | 流控 |
|---|---|---|---|---|---|
| rtk_init（上电时序 + 下补丁固件） | ttyS2 | H5 | 115200 → 1500000 | 偶 8E1 | 无 |
| btstack 正常工作 | ttyS2 | H5 | 1500000 | 偶 8E1 | 无 |

`btstack` 版本：**1.7.2**（与参考工程一致；`gatt_profile_data.h` 就是为它生成的）。
工程里 `fun.json` 显式锁 `"btstack": "1.7.2"`。

---

## 四、⚠️ 第 4 条为什么只能用手机验证（Windows 的限制）

设备侧 GATT 数据库经反复核实**完全正确**：

```
用 WinRT 原生 API 列服务:
  0000180a-...  handle=0x0008   Device Information
  00001812-...  handle=0x001b   HID            ← 存在！
对照枚举特征:
  0000180a 特征枚举 status=0 数量=9   ✓ 9 个特征全部枚举出来
  00001812 特征枚举 status=3 数量=0   ✗ PROTOCOL_ERROR
```

**结论：Windows 把 HID 服务保留给系统 HID 驱动，不给应用层访问其特征**（所以
`bleak`/WinRT 都拿不到报告特征、也就无法写 CCCD 启用通知）。
`status=0/9` vs `status=3/0` 这个对照说明设备侧 ATT 机制没问题。

顺带排除的两个干扰项：
- `bleak` 在 Windows 上"列出全部服务"会漏（只列 Device Information）→ 用 WinRT 原生 API 才能看到 HID；
  如果必须用 bleak，可试 `BleakClient(dev, winrt={"use_cached_services": False})`（强制 UNCACHED）。
- 广播里**去掉 HID UUID/Appearance**（`hid_adv=0`）也不能让 Windows 放开 HID 特征 → 不是广播导致的。

**手机验证步骤**（Android/iOS 均可）：

1. 设备上跑：`/tmp/BtBringup selftest /dev/ttyS2 1500000 even 1800 1 1 1`
2. 手机蓝牙里搜索 **PocketGame-RC** 并连接（若系统提示配对，确认即可）
3. 设备日志应出现：
   ```
   ★ LE 连接建立 handle=0x....
   SM Just Works 请求 → 自动确认
   ★ 自检④通过：HIDS INPUT_REPORT_ENABLE handle=0x.... enable=1
   → 发 HID 按键报告 mask=0x0200 → 返回 0
   ★ SM 配对完成（配对信息写入 TLV → /data/bttlv.db）
   ```
4. `adb shell /tmp/busybox ls -l /data/bttlv.db` 看到文件变大 = 配对信息已持久化。

---

## 五、已知问题：rtk_init 成功率不稳定（调试期约束）

现象：**同一块板上反复跑，`rtk_init` 有时成功、有时失败**，失败表现为
`OP_H5_SYNC Transmission timeout`（有时走到 `[CONFIG]` 阶段才失败）。
失败后 btstack 也起不来（一直 `HCI_STATE_INITIALIZING`）。
**一旦进入失败状态，后续无论怎么重试/重启都难恢复**，而"刚上电第一次"通常成功。

已经排除的（都实测过）：

| 假设 | 实测 | 结论 |
|---|---|---|
| 参数不对 | H5 + 偶校验 + 1500000 已验证可成功 | ❌ 排除 |
| 断电时序不对 | 脚本里显式 `state_bt` 0 → 3s → 1 也一样 | ❌ 排除 |
| 内存不足 | 下载期间 MemFree 稳定 15.9MB | ❌ 排除 |
| **WiFi 共存** | **`ifconfig wlan0 down` 之后仍三次失败** | ❌ 排除 |
| 软件 reboot 能复位 | **连续 3 次 `reboot` + 立即跑，全部失败** | ❌ 排除 |

**⇒ 最可能的解释：BT 的电源域/复位只有"物理冷启动"才能回到 ROM 状态，
`reboot`（软重启）不断它的电。** 09-12 深夜第一次跑通、以及后来几次跑通，
都发生在板子"刚上电/冷启动后不久"的窗口内。

**调试期对策**：
1. **能一次跑通就不要重复跑** —— 自检窗口设长一点（`1800` 秒），跑通后留在窗口里做各种测试，
   别退出重来。
2. 必须重来时，优先**物理断电重上电**（拔插电源/复位键），而不是 `reboot`。
3. `tools/btretry.sh`（PC 侧）会"reboot → 跑 → 查 WORKING"，但如上所述软重启可能不够，
   仅在"板子刚从冷启动起来"时有效。
4. 新版 `selftest` 内置了 **rtk_init 失败自动断电重试 3 次**（写 `state_bt` 0→2s→1），
   轻量但同样受"软复位不彻底"限制。

**产品化结论**：主工程里 BT 只在**开机后进入遥控器功能时初始化一次**，
不做反复重初始化 —— 这样天然规避该问题（参考工程也是这个做法）。

---

## 六、工具

| 工具 | 位置 | 用途 |
|---|---|---|
| `btup.sh` | `bt_probe/tools/` | 设备端：自动重试到 `WORKING` |
| `ble_central.py` | `bt_probe/tools/` | PC 侧 BLE 主机：扫描/连接/枚举/订阅（bleak） |
| `winrt_gatt_probe.py` | `bt_probe/tools/` | PC 侧按 UUID 精确查服务（绕开 bleak 的列表漏项） |
| `winrt_adv_scan.py` | `bt_probe/tools/` | 纯 WinRT：扫描 → 取设备 → 列全部服务（能看到 HID） |
| `winrt_hid_connect.py` | `bt_probe/tools/` | 纯 WinRT：配对 + 订阅 HID 报告（Windows 上会被系统拒绝，留作记录） |

依赖：`pip install bleak`（含 winrt 3.2.1）。

---

## 七、下一步

1. **手机实测第 4 条**（§四的步骤）—— 这是唯一还没自动化覆盖的一环。
2. 把 `ble_selftest.cpp` 的骨架（线程模型 + socketpair 投递 + TLV + HID setup）移植进
   `PocketGame`，做成"智能遥控器"应用：
   - 设备侧按键（A/B/C）→ `hids_device_send_input_report_for_id()` 发 HID 键码；
   - 做 BLE HID 主机读取真遥控器按键（btstack `hids_client`），在**本机**建"物理键 → 动作"映射
     （注意：**"抓包重放学遥控器"在 BT 上不成立** —— HID 按键走加密链路）。

---

## 八、主工程集成（PocketGame）：蓝牙遥控应用

验证过的骨架已移植进主工程，做成**独立 ftu**的功能页（一个功能 = 一个 ftu + 一个 logic）。

### 文件清单

| 文件 | 作用 |
|---|---|
| `ui/remote.html` → `remote.ftu` | 界面源稿（遥控页 + 学习页），`gen_ui.py` 打包 |
| `src/logic/remoteLogic.cc` | 该 ftu 的全部逻辑：调 `pg::Bt::instance()`，不 include btstack |
| `src/core/PgRemote.{h,cpp}` | 主界面卡片的元信息（title/desc/tag/theme）；**不再画布** |
| `src/platform/PgBt.{h,cpp}` | BLE HID 外设 + 主机（btstack + rtk_init），单例 |
| `src/platform/bt/` | `rtk/`（hciattach/rtb_fwc）、`uart_termios.c`（8E1 串口）、`tlv_posix.*`、`gatt_profile_data.h` |
| `Manifest.xml` | `btstack 1.7.2` |

> ⚠️ 蓝牙的状态/按键**不再经 `Host` 往 core 转发**（那 22 个 `bt*` 虚函数已删除）：
> 独立 ftu 的 logic 直接调平台层单例，主界面不必为别的应用挂一层转发。

### 按键映射

| 输入 | 发出的 HID 键（Consumer Control 位图） |
|---|---|
| 触摸按钮（3x3） | 电源 / 播放 / 静音 / 上一首 / 主页 / 下一首 / 音量+ / 返回 / 音量- |
| 物理键 A（raw 108） | 播放 / 暂停 |
| 物理键 B（raw 105） | 音量 + |
| 物理键 C（raw 103） | 音量 - |
| 任意键长按 ≥700ms | 返回主界面（`closeActivity`） |

发报告后 **120ms 自动补一个"全松开"**（否则主机会以为键一直按着）。

### 怎么进

- 界面：主界面「工具」页 → **蓝牙遥控** 卡片
- 脚本：`printf 'remote\n#x\n' > /tmp/cmd && adb push /tmp/cmd /tmp/pg_autostart`
- 页面内自动化：`/tmp/pg_remotecmd`（`send/page/scan/conn/disc/learn/dump/selftest/back`）

### ⚠️ 集成时踩的坑（已写进工程记忆）

1. **`KEY_POWER` / `KEY_MENU` / `KEY_NEXT`… 是 `linux/input.h` 的宏**。
   把它们放进枚举 → 一旦有文件 include 了 input 相关头，裸名会被宏替换成数字，
   直接编译报 `expected identifier before numeric constant`。
   **自定义枚举一律加前缀**（本项目统一 `RC_`）。
2. **`kAppTable[]` 的 slot 必须稳定**：`startGame()`/QA 用**数组下标**，表里的 `slot` 是**存档索引**，
   两者不是一回事。新应用一律追加到表尾。
3. **独立 ftu 的 logic 要自己注册按键监听**（本 ftu 在前台时 `mainActivity` 已 `onUI_hide`）。
4. **列表回调要字面出现**：`getListItemCount_Xxx` / `obtainListItemData_Xxx` / `onListItemClick_Xxx`
   必须与 caption 严格同名（`fun` 按文本匹配生成绑定）。
5. **`gap_stop_scan()` 而不是 `gap_scan_stop()`**（btstack 里 start/stop 的构词不一致）。

### 预期现象（BT 未冷启动时）

进应用后 **UI 不卡**（`rtk_init` 已挪到 BT 线程，它要 20~40 秒）：

```
PgBt: rtk_init 第 1/2/3 次 → -1
PgBt: rtk_init 三次都失败（BT 需物理冷启动；见 docs/bt-hid-selftest.md）
PgBt: TLV 就绪（/data/bttlv.db）        ← 这部分照常工作
PgBt: le_hid_setup 完成
PgBt: HCI_POWER_ON
```

界面显示「初始化失败」+ 底部提示"BT 需断电冷启动后重试"。

**板子物理断电重上电后再进应用**，就会看到：状态变"就绪（可被发现）"、扫描条数开始涨、
手机能搜到 **PocketGame-RC**；连上后状态变"已连接"，按键即遥控。

---

## 九、主机侧：学习真实遥控器的按键

需求原文的"学习其他遥控器的蓝牙 key"——**"抓包重放"在 BT 上不成立**（HID 按键走加密链路、
每条连接独立会话密钥）。正解是**本机当 BLE HID 主机**去连对方、把对方发的报告读进来，
在本机建"物理键 → 动作"映射表。这一侧已实现。

### 能力

在 `PgBt` 里（与"外设侧"共存，**同一颗芯片同时做 peripheral + central**）：

1. **扫描并缓存周边 BLE 设备**（最多 16 个，按地址去重）：地址 / 广播名 / RSSI / 地址类型
2. **主动连接**指定设备：`gap_connect(addr, addr_type)`
3. 连上后 **`hids_client_connect()`** 自动发现对端 HID 服务 + 报告映射，
   并**自动启用输入报告通知**
4. **收到 HID 报告** → 解析 report id + payload → 存进"学习表"（滚动 8 条）

### 脚本化验证命令（QA）

```
btscan        清空并重新收集扫描结果（等几秒）
btlist        打印扫到的设备（地址 / 名字 / RSSI）
btconn N      连接列表里第 N 个（走 hids_client 发现 HID 服务）
btlearn       打印主机状态 + 学到的 HID 报告
```

典型流程：`btscan` → 等 5s → `btlist` → `btconn 0` → 按对方遥控器的键 → `btlearn`

### 实测证据（2026-09-13）

```
PgBt: btlist —— 共 14 个设备（累计广播 956 条，扫描中=有数据）
PgBt:   [0] FA:A8:9A:CE:60:D9  ""                  RSSI=-80
PgBt:   [3] 06:43:50:23:2D:A8  "Dao Charger 150W"  RSSI=-90
PgBt:   [13] B9:42:FB:00:1D:1F "BR"                RSSI=-94
PgBt: gap_connect(0D:E0:E6:46:69:42 type=1) -> 0     ← 发起连接成功
PgBt: btlearn —— hostState=连接中 peer=0D:E0:E6:46:69:42 学到 0 条
```

> 停在"连接中"是正常的：周边多数是随机地址的 BLE 外设/信标，不可连接或不应答。
> **要验证完整的"学习"，需要一个真实的 BLE 遥控器/键盘**（连上后会看到
> `HID 服务已连接 cid=… status=0x00` → `★ 已订阅输入报告` → `★ 学到 HID 报告 #1 rid=… xx xx`）。

### ⚠️ 踩到的坑：按需收集不生效 → 改成始终收集

原设计用一个 `scanCollect` 开关（`btscan` 命令打开），但实测**开了开关也收不到**：
`btlist` 显示 `共 0 个设备`，而同一时刻 `advCount=1153`（扫描明明在工作）。
在收集分支里加打点后确认"一条都没进收集代码"。
**改成"始终收集"（不再看开关）后立刻正常**。
根因未彻底查清（怀疑与该标志跨线程读写/优化有关），但**始终收集的开销只是每收到一条广播
做一次 ≤16 次的线性查重**，可以忽略，所以就这样定了。

### 学习页（界面化，2026-09-13 完成）

右上角一个「遥控中 / 学习中」切换按钮，学习页布局：

```
┌ 蓝牙遥控                    [ 学习中 ] ┐   ← 右上角切换（绿色=学习页）
│ 主机状态   空闲 / 连接中 / 已连接（发现服务）/ 学习中（按对方按键）
│ 对端       AA:BB:CC:DD:EE:FF | -
│ 周边设备 N 个（点一行连接）
│   0 FA:A8:9A:CE:60:D9 -80      ← 点一行 = hostConnect(i)
│   1 06:43:50:23:2D:A8 Dao Charger 150W -90
│   …（最多 6 行，列表最长 16 个）
│ 已学到 0 条（按对方遥控器）
│   id1 e9 00                    ← 点一条 = 把学到的报告"重放"出去（演示通路）
│ [ 重新扫描 ]  [ 断开 ]
└──────────────────────────────────────────┘
```

- 进学习页会自动 `btHostScan()`（清空并重新收集周边设备）
- 触摸走 `onTouch` 的分页分支；数据全部经 `Host` 的 `bt*` 接口（core 层不碰 btstack）
- 重放的当前实现是**把学到报告的 payload 首字节当 consumer 位图发出去**（演示通路可用）；
  **精确映射**（对端每个 usage → 我们发的键）需要解析对端报告描述符，
  可用 `hids_client_descriptor_storage_get_descriptor_data()` + `btstack_hid_parser`，列为后续项。

### 报告解析 + 重放闭环（2026-09-13 完成，**不需要第二台设备就能自验证**）

"学习"的关键是把对端报告的**原始字节**变成"对方按了哪个键"。用 btstack 的
`btstack_hid_parser`（**纯函数**，可在 UI 线程安全调用）。

**自验证设计**（本机自己就是 HID 外设，格式已知 → 不需要真遥控器就能验解析器）：

```bash
btselfparse     # QA：用我们自己的 139 字节描述符解析 5 个构造报告
```
```
PgBt: [自验证] btstack 算出的 input 报告字节数: id1=2 id2=4 无id=0   ← 描述符解析正常
PgBt: [自验证] 全 0（无按键）   -> 识别 0 项: (无按键)
PgBt: [自验证] bit9 -> 期望播放暂停 -> 识别 1 项: 播放暂停     ✓
PgBt: [自验证] bit6 -> 期望音量加   -> 识别 1 项: 音量加       ✓
PgBt: [自验证] bit7 -> 期望音量减   -> 识别 1 项: 音量减       ✓
PgBt: [自验证] bit1 -> 期望菜单     -> 识别 1 项: 菜单         ✓
```

**⚠️ 踩到的坑（值得记）：`btstack_hid_parser_init` 必须传【含 report id 的完整报告】**
（BLE HID 通知的数据本身就带 report id）。我一开始"跳过首字节只传位图"→ 一项都解析不出来；
先以为是描述符问题，用 `btstack_hid_get_report_size_for_id()` 一测（id1=2 / id2=4 字节全对）
才定位到是传参错。**排错顺序：先验描述符能不能被解析（算 report size），再看传参。**

**重放闭环**：收到对端报告时，解析出 usage → `usageToMask()` 映射成**我们自己的**
consumer 位图 → 存进学习表。学习页点某一条 = `btSendKey(该位图)` —— 即
**"对端按了什么 → 我们也能发什么"**。

映射表（Consumer Page，对端 usage → 我们的位图）：
```
0x30 电源→bit0   0x40 菜单→bit1   0xB5 下一首→bit4   0xB6 上一首→bit5
0xE9 音量加→bit6  0xEA 音量减→bit7  0xE2 静音→bit8   0xCD 播放暂停→bit9
0x223 主页→bit12  0x224 返回→bit13
```
（键盘 Page 0x07 的键也做了名字识别，但暂未映射成我们的位图 —— 需要时在 `usageToMask` 里加。）

---

## 十、⚠️ 已知问题：BLE 扫描会"哑火"（本板/本会话实测）

**现象**：进入蓝牙遥控后，学习页的"周边设备"先是能收集到（实测收集到 15 个），
过一阵（几十秒~几分钟）**advCount 冻在某个值不再增长**，列表从此恒空；
此时**BT 线程的定时器也不再回调**（心跳日志停在最后一次）。

**已确认的事实**（都有日志）：
- 同一时刻 UI 线程仍活着（`/tmp/pg_remotecmd` 的 `dump` 正常响应），BT 线程也活着
  （它还能处理 socketpair 投递的 `CMD_HOST_SCAN` 并打印"开始收集扫描结果"）；
- 只是 **GAP_EVENT_ADVERTISING_REPORT 不再到达**，且**定时器不再回调**；
- `advCount` 冻结后重新 `gap_stop_scan()+gap_start_scan()` 也**不一定**能恢复。

**不是这次 UI 改造引入的**（改造前就有：09-13 那轮 canvas 版本同样是"扫到 15 个后冻结"）。

**工程上的对策**（已落地，尽量让界面自洽）：
1. **进学习页不清空已有列表** —— 只有列表为空时才主动扫一次；
   用户想刷新就点「重新扫描」（幂等，顺手重起 LE 扫描）。
2. **扫描哑火自愈**：BT 线程每 2s 一拍，连续 5 拍没有新广播就自动重起 LE 扫描。
3. **低频心跳日志**（每 200 条广播 / 每 30s 一拍）：一眼看出"是控制器不报了"还是"进了没收集"。

**待查**：怀疑与 BT/WiFi 共存（同一颗 RTL8733BS 组合芯片）或固件扫描超时有关；
参考工程只在开机后初始化一次 BT 并长时间只做 HID 外设，未跑这种"长时间扫描 + 主机侧"组合。
