# 蓝牙 bring-up：参数确认 + 实测矩阵（RTL8733BS / V85X）

> 2026-09-12。目的：在**不依赖厂家固件**的前提下，把"能不能通、通不了卡在哪"测清楚。
> 探针工程：`bt_probe/`（独立 bin 工程，`fun add btstack` 装的 btstack 1.8.0）。

## 一、先确认的三件事（本次已全部确认）

| 要确认 | 本板结论 | 依据 |
|---|---|---|
| **挂哪路串口** | **`/dev/ttyS2`**（V85X；F133 是 ttyS1） | DT：`uart@2500000`(ttyS0)/`uart@2500800`(ttyS2) = `okay` 且 pinmux 齐全；`uart@2500400`(ttyS1)/`uart@2500c00`(ttyS3) = **`disb`**。⚠️ 出厂 `/res/etc/EasyUI.cfg` 里写的 `uart:"ttyS1"` 在本板**不存在**，别照着用 |
| **波特率 / 流控 / 校验** | **初始化 115200 → 工作 1500000，无流控，偶校验（8E1）** | 按 Realtek RTL8733BS 规格；探针已实现 `set_baudrate` + `set_parity(EVEN)`（自研 termios uart，见下） |
| **是否需预初始化** | **需要**：`persist.wifi.module = 8733bs` → 走 Realtek 专用 `rtk_init(dev)`（上电时序 + 下载补丁固件 + 改波特率），不能当普通 HCI 直接用 | 设备 `getprop persist.wifi.module` = `8733bs`；这也解释了"标准 HCI Reset 无人应答" |

## 二、探针怎么用

```bash
# 在设备上
/tmp/BtBringup [串口] [波特率] [h4|h5] [none|even] [0|1(流控)]
/tmp/BtBringup /dev/ttyS2 115200  h4 even    # RTL8733BS 初始化口（推荐先跑这条）
/tmp/BtBringup /dev/ttyS2 1500000 h4 even    # 工作口
```
10 秒自动收尾并打印结论（脚本可判）。退出用 `btstack_run_loop_trigger_exit()`，不是 `exit()`。

## 三、实测矩阵（2026-09-12，全部：芯片侧零回应）

| 串口 | 波特率 | 传输 | 校验 | 结果 |
|---|---|---|---|---|
| ttyS2 | 115200 | H4 | 偶(8E1) | 零回应（含 Realtek VS 0xFC6D 无应答） |
| ttyS2 | 115200 | H4 | 无(8N1) | 零回应 |
| ttyS2 | 1500000 | H4 | 偶(8E1) | 零回应 |
| ttyS2 | 1500000 | H5 | 偶(8E1) | 零回应 |
| ttyS2 | 115200 | H5 | 偶(8E1) | 零回应 |
| ttyS0 | 115200 | H4 | 偶(8E1) | 零回应 |

判定依据（不是"没日志就算失败"）：
- 只看 **HCI 事件码 0x01~0x5F** 算"芯片回话"；`0x60+` 是 btstack 本地事件
  （`0x6E = TRANSPORT_PACKET_SENT` 是**本地"已发出"**，会被它刷屏误判，踩过）。
- 额外发 **Realtek 厂商命令 `0xFC6D`（Read_ROM_Version）** ——
  这是 `rtk_hciattach` 的第一步；芯片只要**上电且在听**就会回 `COMMAND_COMPLETE`。
  **它也不回 ⇒ 不是"缺补丁固件"那么简单，而是 BT 部分没被使能/没上电。**

## 四、结论：主机侧已无问题，卡在模块侧

- 主机侧：串口参数已按规格（8E1/115200/1500000/无流控）、H4 与 H5 都试过、
  两路可用 UART 都试过、厂商命令也发过 —— **能做的都做了，没有任何字节回来**。
- 模块侧缺三样（按优先级，都要模组厂/同事的 V85X 工程提供）：
  1. **BT 使能/复位 GPIO 的编号与时序**（`rtk_init()` 的第一步；本板 DT 里
     **没有任何 bt/bluetooth/\*reg_on 节点**，`/sys/class/gpio` 也没有已导出 GPIO）；
  2. **RTL8733B 的 BT 补丁固件**（`rtl8733b_fw` / `rtl8733b_config`；全盘只有 aic8800DC，
     那是 AIC8800 的，不是这颗芯片）；
  3. **可参考的 `rtk_init(dev)` / `rtk_hciattach` 实现**（同事 V85X 工程里有；
     本机没有，无法照抄时序与厂商命令集）。
- 顺序上是"先 ① 上电，才谈 ② 固件"：**0xFC6D 不回 = 先查 ①**。

## 五、btstack 集成的五条硬规矩（都是实测踩出来的）

1. **一条线程专跑 run loop**（`btstack_run_loop_execute()`），**所有 btstack API 都在该线程调**；
   别的线程干活用 `btstack_run_loop_execute_on_main_thread(&registration)`
   （`btstack_context_callback_registration_t{ item, callback, context }`）。
   串口读就注册成 run loop data source（本探针的 `uart_termios.c` 即此模式）。
2. **`hci_add_event_handler()` 必须在 `hci_init()` 之后**（前者往 `hci_stack` 的链表加节点，
   `hci_stack` 由后者分配；顺序反了 = 空指针段错误）。
3. **上电后别立刻发命令**：等 `hci_get_state() == HCI_STATE_WORKING`；
   没到就挂 **100ms 定时器**轮询（本探针顺带每秒重发 HCI Reset + 厂商命令）。
   状态枚举：`HCI_STATE_OFF/INITIALIZING/WORKING/HALTING/SLEEPING`。
4. **H5 要求 uart 提供 frame 级接口**（`set_frame_received/set_frame_sent/receive_frame/send_frame`）：
   留 NULL 会在 `hci_power_control()` 里**空指针段错误**（实测：H4 正常、H5 一上电就崩）。
   不想自己写 SLIP 就用 btstack 自带的 `btstack_uart_posix_instance()`（但它**不支持偶校验**）。
5. **退出**用 `btstack_run_loop_trigger_exit()`（POSIX run loop 没有别的 stop 接口）；
   实在不管也行（`pthread_detach`，随进程退出）。

## 六、本探针的串口实现（为什么不用 btstack 自带的）

`bt_probe/src/uart_termios.c` 自己实现 `btstack_uart_t`：

- 原因 ①：**Realtek 要 8E1 偶校验**，`btstack_uart_posix_instance()` 只做 `cfmakeraw`（8N1）；
- 原因 ②：需要 115200 → 1500000 的**运行中切换**（`set_baudrate`）、RTS/CTS 可选；
- 原因 ③：读用 run loop data source → 回调落在 run loop 线程，符合规矩 1；
- 顺带实现了 **SLIP 帧**（0xC0 帧界 / 0xDB 转义），满足规矩 4。

以后接真实 BT 模块时，这个 uart 可以直接复用（补上 GPIO 上电与固件下载即可）。

---

## 七、✅ 已解决（2026-09-12 深夜，找到参考工程后）

**根因两条**（之前所有"零回应"都是它们）：

1. **BT 没上电**：本板 BT 的电源开关是一个 sysfs 节点
   **`/sys/devices/platform/soc/soc@03000000:netRF/state_bt`**，实测当时是 **`off`**。
   写 `1` 才给 BT 供电（参考工程 `rtk_init()` 里的 `bt_enable()`：`0` → 50ms → `1` → 300ms，跑两轮）。
2. **缺 Realtek 预初始化**：RTL8733BS **不做 hciattach 流程就不会应答标准 HCI**。
   必须先做 **H5 同步握手 → 读 ROM 版本 → 下载补丁固件（rtl8733bs_fw + _config）→ 切到 1500000**，
   之后才能当普通 HCI 用。

**参考实现来源**：`S:/projects/LearningProject/V851ExtendedScreen_ap_p2p/src/ble/rtk/`
（`hciattach.c` + `hciattach_h4.c` + `rtb_fwc.c`，Realtek hciattach 3.1.3 移植版）；
固件：同工程 `src/dependencies/bin/firmware/rtlbt/`。

**已移植进本仓库**：
- 探针：`bt_probe/src/rtk/`（+ `rtk_log.h` 日志 shim；固件目录改为候选 `/res/bin` → `/data` → `/tmp`）
- 主工程：`PocketGame/src/platform/bt/rtk/`（同一份，`private.h` 条件包含 `utils/Log.h`）
- 固件：`PocketGame/src/dependencies/bin/firmware/rtlbt/` → **`fun pack` 会自动打进
  `/res/bin/firmware/rtlbt/`**（已实测：镜像里可见，包体 420.6 → 472.6 KB）

### 跑通的命令与证据

```bash
# 设备上（探针带 rtk 前置关键字 = 先 rtk_init 再交给 btstack）
/tmp/BtBringup rtk /dev/ttyS2 1500000 h5 even
```
```
[rtk] Realtek hciattach version 3.1.3be84a4...
[rtk] [SYNC] Get SYNC Resp Pkt          ← H5 同步握手成功
[rtk] H5 init finished / Realtek H5 IC
[rtk] Read ROM version 02
[rtk] IC: RTL8733BS, chip_type 0x76
[rtk] Load FW /data/firmware/rtlbt/rtl8733bs_fw OK, size 55580
[rtk] Config baudrate: 04928002 / Vendor baud ...
[BT]  BTSTACK state = 2 (WORKING)        ← ★ HCI 通路建立
[BT]  COMMAND_COMPLETE opcode=0x1001 status=0x00
[BT]  -> HCI 版本 11, 厂商 0x005d (Realtek), 子版本 0x6fec
[BT] ==== 结论 ==== HCI 状态到 WORKING —— 同步握手通过，HCI 通路建立
```

### 最终确认的工作参数（与参考工程一致）

| 阶段 | 串口 | 传输 | 波特率 | 校验 | 流控 |
|---|---|---|---|---|---|
| 预初始化（hciattach，下载固件） | ttyS2 | H5 | **115200** | **偶 8E1** | 无 |
| 之后（btstack 正常使用） | ttyS2 | **H5** | **1500000** | **偶 8E1** | 无 |

> 也就是说：**H5 + 偶校验从头到尾**；波特率由 rtk_init 从 115200 切到 1500000。
> （之前按 H4/8N1 测必然零回应 —— 参数和上电两个都错着。）

### 自检清单当前状态

| # | 自检项 | 状态 |
|---|---|---|
| 1 | 编译无未定义符号（libbtstack.a 已链上） | ✅ |
| 2 | `hci_power_control` 后 `BTSTACK_EVENT_STATE → HCI_STATE_WORKING` | ✅ **已通** |
| 3 | 收到 `GAP_EVENT_ADVERTISING_REPORT`（扫描） | ⏳ 待实现（探针加 scan 模式） |
| 4 | HIDS/ATT 连接事件、手机可发现（BLE HID 外设） | ⏳ 待实现（需 HID 描述符 + 广播） |
| 5 | 配对后生成 TLV 文件（如 `/data/bttlv.db`） | ⏳ 待实现（依赖 4） |
