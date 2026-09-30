# 蓝牙（BT）启用需求清单 —— 给模组/方案厂商（Zkswe）

> 更新：2026-09-12 第二轮（用 **btstack 用户态 H5/H4 探针**实测之后）
> 目标：V851s 口袋掌机要做 **BLE 智能遥控器**（本机当 BLE HID 外设发键 + 当 BLE HID 主机读真实遥控器）。
> 软件栈已就位（`btstack 1.8.0` + BLE/GATT/HIDS），**卡在硬件通路点亮这一步**。

---

## 一、结论先说

**这颗板子的 BT 部分从当前固件里"接触不到"**：两路可用串口（ttyS0/ttyS2）上，
用 btstack 的 H5 和 H4 两种传输、115200 与 1500000 两档波特率都试过，
**芯片侧零回应**（只有 btstack 本地"已发出"事件在刷）。

所以需要贵方补齐下面 4 项；补齐后我们这边 1~2 天就能把 BLE 遥控器跑起来。

## 二、实测证据（可复现）

### 2.1 软件侧：我们有可用的用户态 BT 通路（不是软件问题）

`btstack 1.8.0`（v85x 包）自带：

| 能力 | 头/符号 | 说明 |
|---|---|---|
| H5 传输（Realtek 常用） | `hci_transport_h5_instance()` | SLIP + 同步握手 |
| H4 传输 | `hci_transport_h4_instance()` | 无握手，最省事 |
| POSIX 串口实现 | `btstack_uart_posix_instance()` | 直接开 `/dev/ttySx` |
| POSIX run loop | `btstack_run_loop_posix_get_instance()` | 事件循环 |
| BLE 外设栈 | `ble/hids_device.h`、`att_server.h`、`sm.h` | 将来做 HID 遥控器的现成实现 |
| 配置 | 包内 `btstack_config.h` 已开 `ENABLE_H5` / `ENABLE_BLE` / POSIX | 开箱即用 |

我们已写好探针工程 `bt_probe/`（独立 bin 工程）：

```bash
./fun.exe build                       # 产出 .fun/v85x/BtBringup
adb push .fun/v85x/BtBringup /tmp/ && adb shell chmod 755 /tmp/BtBringup
adb shell "/tmp/BtBringup /dev/ttyS2 115200 h5"   # 10 秒后自动给结论
adb shell "/tmp/BtBringup /dev/ttyS2 115200 h4"
```

实测输出：

```
[BT] 串口=/dev/ttyS2 波特率=115200 传输=H5
[BT] ==== 结论 ====
[BT] 只收到 btstack 自己的状态事件，**芯片侧零回应**
```

（ttyS0 @115200 H4、ttyS2 @1500000 H4 同样零回应。）

### 2.2 固件侧：缺什么（逐项实测）

| 检查 | 命令 | 结果 |
|---|---|---|
| 实际无线芯片 | `lsmod` | 只有 `8733bs`（**RTL8733BS** WiFi+BT 组合） |
| BT 射频是否被期待使用 | `cat /sys/module/8733bs/parameters/rtw_btcoex_enable` | **2**（WiFi 驱动开了 BT 共存） |
| 串口 pinmux | `/proc/device-tree/soc@03000000/uart@2500000`（ttyS0）<br>`.../uart@2500800`（ttyS2） | `status=okay`，`pinctrl-names="default","sleep"` 都有 → **串口本身是好的** |
| 串口是否被禁用 | `.../uart@2500400`（ttyS1）<br>`.../uart@2500c00`（ttyS3） | `status=disb`（非 okay = 禁用）→ `/dev` 里没有这两个节点<br>⚠️ 而出厂 `EasyUI.cfg` 里写的正是 **`"uart":"ttyS1"`** |
| BT 设备节点 | `ls /dev \| grep hci`、`ls /sys/class/bluetooth` | **不存在** |
| BT 使能/复位 GPIO | `find /proc/device-tree -iname '*bt*' -o -iname '*bluetooth*' -o -iname '*reg_on*'`<br>`ls /sys/class/gpio` | **DT 里没有任何 BT 节点**；已导出 GPIO 一个都没有 |
| BT 固件 | `ls /lib/firmware`、全盘 `find` | 只有 `aic8800DC`（**AIC8800** 那颗芯片的），**没有任何 RTL8733 BT 固件** |
| 内核 BT 子系统 | `/sys/class/bluetooth`、`/dev/hci*` | 没有（我们走 btstack 用户态，不需要内核） |
| init.rc 里的 BT 服务 | `cat /etc/init.rc` | `service hciattach /res/bin/hciattach -n ttyS2 aic`（**面向 AIC8800**）<br>`service bt /tmp/bt.sh` —— 但 **`/res/bin/` 是空的、`/tmp/bt.sh` 不存在**，服务都是 `disabled` |

**结论**：这套固件是按"BT 走 AIC8800"配的（`/lib/firmware/aic8800DC`、`hciattach ... aic`），
但这块板实际贴的是 **RTL8733BS** → 于是 BT 这条链上一个环节都没有：
没有固件、没有使能脚控制、DT 里没有任何 BT 描述。

## 三、需要贵方提供（按优先级）

1. **BT 使能 / 复位 GPIO 与时序**（最可能的根因，优先级最高）
   - BT_EN / BT_REG_ON / BT_RST_N 分别是**哪个 GPIO**、有效电平、上电到可通信的延时；
   - **BT 与 WiFi 是否共用一路电源/晶振**、WiFi 起来后 BT 是否还需要单独拉一个脚。
   - 现象对照：现在串口发出去的东西没有任何回应，和"BT 核在复位态/没上电"完全一致。

2. **BT 走哪一路 UART**
   - 候选：`ttyS0`(uart@2500000) 或 `ttyS2`(uart@2500800) —— 这两路目前都不回应；
     而 `ttyS1`(uart@2500400) 被 DT 标成 `disb` 禁用，偏偏出厂 cfg 里写的是 `uart=ttyS1`。
   - 如果 BT 就是接在 UART1 上，**需要贵方改设备树把那路 enable**（我们改不了 DT）；
   - 若走 ttyS2/ttyS0，请确认是否需要先用 GPIO 给 BT 上电（即第 1 项）。

3. **RTL8733B 的 BT 补丁固件**（fw + config，Realtek 命名一般类似 `rtl8733b_fw`/`rtl8733b_config`）
   - 用途：Realtek 的 BT 需要主机下载补丁固件（btstack 的 chipset 流程 / 内核 btrtl 都靠它）。
   - ⚠️ 注意：ROM 模式下的芯片**本该能回 HCI Reset**（我们连这个都没收到），
     所以固件缺失是"第二层问题"——第一层是上面 1/2 的通路与上电。

4. **（可选、但最省时间）贵方已跑通的 BT 启动方式**
   - 比如一段 `bt.sh`、`hciattach` 参数、或一份把 BT 点亮的 dts 片段；
   - `init.rc` 里已经留了 `service bt /tmp/bt.sh` 的位置，给我们脚本我们就能直接塞进去。

## 四、拿到之后我们这边的工作（不需要贵方参与）

- btstack 起 HCI 通路（H5 或 H4）→ `blehid` 暴露成 **BLE HID 外设**（键盘/媒体遥控），
  界面按键映射成标准 HID 键码发给 Android TV/盒子/手机；
- 同时做 **BLE HID 主机**（btstack central + gatt），连真实 BLE 遥控器读 HID report，
  在本机建"物理键 → 动作"的映射表。

## 五、一个必须提前对齐的技术前提

**"学习别人的蓝牙遥控器按键再原样发出去"在 Bluetooth 上做不到**：

- BLE/经典 BT 的 HID 按键在**已加密的链路层连接**里传，每条连接有独立会话密钥（还要配对/bonding），
  抓不到明文、也没法换一台设备重放；
- 如果那个遥控器其实是 **2.4G 私有 RF**（很多电视盒子/投影遥控是这种）或**红外**，
  用 BT 芯片根本无法兼容（物理层都不一样）。

所以"智能遥控器"的落地形态是：**本机当 BLE 遥控器**（发标准键码给目标设备）
+ **本机当 BLE 主机**去读真实遥控器的按键，把"哪个键 → 干什么"记成本机映射表。

---

_相关落点_：
- 探针工程：`bt_probe/`（`fun.json` 里已声明 `btstack ^1.8.0`）
- 依赖：主工程加 `btstack` / `blehid` 时用 `./fun.exe add btstack`
- 界面：建议独立 `remote.ftu`（一个 Activity 一个 ftu），参考已落地的 `wifi.ftu`
- 免触摸调试：主工程已有文件驱动 QA 通道（`/tmp/pg_autostart`、`/tmp/pg_wificmd`）

---

## 补充（2026-09-12 晚，参数已按规格确认后的第二轮）

**主机侧已排除干净**（细节见 `docs/bt-bringup.md`）：

- 串口：`/dev/ttyS2`（DT 里 `okay` 的两路 ttyS0/ttyS2 都试过；ttyS1/ttyS3 是 `disb`）；
- 参数：**115200 初始化 → 1500000，无流控，偶校验（8E1）** —— 完全按 RTL8733BS 规格；
- 传输：**H4 与 H5 都试**（H5 的 SLIP 帧已在自研 uart 里实现，不再有空指针崩溃）；
- 命令：标准 **HCI Reset** + Realtek 厂商命令 **0xFC6D（Read_ROM_Version，rtk_hciattach 的第一步）**；
- 结果：**以上全部组合，芯片侧零回应**（连 0xFC6D 都不回）。

**⇒ 这不再是"缺补丁固件"一句话能解释的：BT 部分很可能根本没有上电/没被使能。**
请优先确认第 1 项，再谈固件：

1. **BT 使能（BT_EN）/复位 GPIO 的编号与上电时序** —— 本板设备树里**没有任何
   bt/bluetooth/\*reg_on 节点**，`/sys/class/gpio` 也没有已导出的 GPIO，
   我们无法猜 GPIO。请给：GPIO 编号 + 有效电平 + 上电/复位/释放的延时顺序
   （即 `rtk_init(dev)` 里那一段）。
2. **RTL8733B 的 BT 补丁固件**（`rtl8733b_fw` + `rtl8733b_config` 之类）。
3. **你们已跑通的 `rtk_init(dev)` / `rtk_hciattach` 实现或参数**（含厂商命令集与下载分包大小）——
   有这份我们就能补齐固件下载与波特率切换，直接复用现有探针工程验证。
4. 顺带确认：**BT 到底挂在哪路 UART**（我们按 V85X 惯例用的是 ttyS2；若实际是 ttyS1，
   需要你们改设备树 enable，我们改不了 DT）。

拿到 1（+2/3）后，用 `bt_probe/`（已支持 H4/H5、115200/1500000、8E1 偶校验、run loop 线程化）
可以当天验证到 `HCI_STATE_WORKING`。

---

## ✅ 说明（2026-09-12 深夜更新）

**BT 已经跑通，本清单里要的东西不再需要向厂家索取**：参考工程
`S:/projects/LearningProject/V851ExtendedScreen_ap_p2p` 里已有完整实现与固件
（`src/ble/rtk/` + `src/dependencies/bin/firmware/rtlbt/`），已移植进本仓库。

根因回顾：① BT 电源开关节点 `state_bt` 当时是 `off`（写 1 才上电）；
② RTL8733BS 必须先跑 hciattach（H5 同步 + 下载补丁固件 + 切 1500000）。
详见 `docs/bt-bringup.md` §七。**保留本文档仅作排查记录。**
