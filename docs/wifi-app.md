# WiFi 独立应用 + 信号探针 + 音量 OSD（PocketGame）

本轮把原来的"画布版 WiFi 页"换成了 **FlyThings 原生控件**做的独立应用（Android 风格），
并补了音量弹窗、WiFi 信号探针工具包。DLNA / 蓝牙遥控的可行性结论在文末。

---

> ⚠️ **2026-09-15 更新：「信号探针」已从本应用拆出，成为独立应用**（`probe.ftu` /
> `probeActivity` / `src/logic/probeLogic.cc`），功能扩到 WiFi + 蓝牙 + 热点猎手。
> 本文下面凡是写着 `WinProbe` / `ui/wifi.html 的探针页` / `pg_wificmd` 的 `probe` 命令的地方，
> 都请对照 `docs/wifi-probe-app.md` 读 —— 那些控件与回调**已经不在 `wifi.ftu` 里了**；
> `wifi.ftu` 底部的「信号探针」按钮现在只做 `openActivity("probeActivity")`。
> （本文其余部分保留为 WiFi 应用自身的历史记录。）

## 1. 交付了什么

| 文件 | 说明 |
|---|---|
| `ui/wifi.html` | WiFi 应用布局源稿（→ `ui/wifi.json` → `ui/wifi.ftu`） |
| `src/logic/wifiLogic.cc` | WiFi 应用逻辑层（扫描/连接/开关/探针/密码框/音量 OSD） |
| `ui/main.html` | 主界面：新增 `WinVolume` 音量 OSD 窗口 |
| `tools/gen_ui.py` | 多 ftu 生成（main + wifi）；新增进度条图生成 |
| `tools/wifiprobe.py` | **WiFi 信号探针工具包**（PC 侧驱动，出表格 + 信道占用报告） |
| `docs/wifi_probe_report.md` / `.csv` | 探针报告样例（本次真机实测产出） |
| `docs/shot_wifi_*.png` | 真机验收截图 |

### 1.1 独立 ftu（需求③）

一个 Activity 一个 ftu：`wifiActivity` ↔ `wifi.ftu`，`fun build` 自动生成绑定
（`EventApp<Activity>("wifi")` + `INIT_UI_EVENT_BINDINGS`）。
主界面"系统"分类里的 WiFi 卡片用 `EASYUICONTEXT->openActivity("wifiActivity")` 进入，
**不再走画布**（`kAppTable[]` 里那一行保留，slot 12 = 存档索引稳定）。

### 1.2 WiFi 页结构

```
WiFi 主页
├─ 顶栏：返回 / WLAN 标题 / 右上角总开关（ZKCheckBox，选中变绿 + 显示"开/关"）
├─ 状态卡：当前 SSID（已连接变绿）+ 信号/信道 + IP
├─ 扫描列表（6 行/屏）：SSID / 信道+加密（已连接标注）/ 信号档位 / dBm
└─ 底部：刷新 · 信号探针（图标在上、文字在下）
WinProbe（整屏普通 window）
├─ 顶栏：返回 / WiFi 信号探针 / 刷新
├─ 信道占用概览：共 N 个网络（2.4G:x / 5G:y）+ 占用最多的前 8 个信道
└─ 列表（按信号强度排序）：CH xx / SSID / 频段+加密 / dBm
WinWifiPwd（modal）
└─ 标题 / SSID / 密码输入框（**系统软键盘 IME**）/ 错误提示 / 取消·连接
WinVolume（普通 window，1.6s 自动消失）
└─ 音量图标 + 音量 + 百分比 + 进度条（ZKSeekBar）
```

加密网络点一下弹密码框；开放网络直接连。

### 1.3 音量 OSD（需求②）

- 主界面按音量键 → `WinVolume` 弹出（进度条 + 百分比 + 提示音），1.6s 自动消失。
- WiFi 页在前台时，`wifiLogic` 自己注册按键监听器（`EASYUICONTEXT->registerKeyListener`），
  音量键走 **跨 activity 钩子** `pg::volumeStepGlobal()` 转给 mainLogic 那份唯一的音频实例，
  再显示 **wifi.ftu 自己的** `WinVolume`（两个 ftu 各有一份，互不干扰）。
- 长按任意键 ≥700ms = 返回主界面（本板 gpio-keys 无 autorepeat，长按只能按时长判定）。

---

## 2. 信号探针工具包（需求④）

### 2.1 设备侧：应用里的"信号探针"页
点一次刷新重新扫描，看现场实况（信道 + 频段 + 加密 + dBm，按信号强度排序）。

### 2.2 PC 侧：`tools/wifiprobe.py`
把设备扫到的结果拉回 PC 做表格/统计/报告：

```bash
python tools/wifiprobe.py                                  # 打印表格
python tools/wifiprobe.py --out docs/probe.md --csv docs/ap.csv
python tools/wifiprobe.py --device 20080411 --wait 8
```

它做的事：adb 打开 WiFi 应用 → 触发一次扫描 → 让设备把结果 dump 到日志 →
解析成「AP 表 + 2.4G 信道占用 + 5G 信道占用 + 加密分布」，并对 1/6/11 给出选信道建议。

真机实测（本办公室环境）：24 个 AP，CH1 上有 9 个（含旁边 -24dBm 的实验室 AP）→
工具直接给出"建议用 CH11"。完整输出见 `docs/wifi_probe_report.md`。

---

## 3. 免触摸自检（本板关键手段）

### 3.1 工程内的两条 QA 通道
主界面（`mainLogic.cc`）：`/tmp/pg_autostart`，整份内容变化时逐行执行。
```bash
adb shell "echo wifiapp1 > /tmp/pg_autostart"     # 打开 WiFi 应用
adb shell "echo 'vol 1 6000' > /tmp/pg_autostart" # 音量 +1（OSD 停留 6s，便于抓帧）
```

WiFi 应用（`wifiLogic.cc`）：`/tmp/pg_wificmd`，内容变化才执行。
```
on / off          开/关 WiFi
scan              触发一次扫描
dump              把扫描结果打到日志（AP#i ssid/freq/ch/sec/rssi）
state             打印 enable/connected/ssid/ip/ap 条数
pick <i>          等价于点列表第 i 行
pickopen          挑第一个开放网络连接（序号会随扫描变化，这个才是确定性的）
pw <文字>         填密码（用 _ 代表空格）
ok / cancel       密码框的 连接 / 取消
probe / back      显示/隐藏信号探针页
vol <±1> [停留ms] 走按键同一条路：跨 activity 钩子 + 本 ftu 的 OSD
```

⚠️ 两条通道都要求**每行内容不同**（尾部加序号），相同内容会被去重不执行。
⚠️ 读日志必须"同一次 adb 调用内"完成，否则日志会被 zknet 的 WiFi 事件刷爆：
```bash
adb shell "logcat -c; <动作>; /tmp/busybox sleep 0.4; logcat -d"
```

### 3.2 为什么不用触摸注入
`mt_test` 注入在本板**时灵时不灵**（同一个点，前面能触发后面就不响应；
重启设备、重启应用都没恢复），而 `dispatchCanvasTouch`（画布坐标注入）只对游戏
（`MODE_GAME`）生效、进不了原生控件。所以原生控件的验收改用上面的文件 QA 通道。

`mt_test` 可用时的两个注意点：
- 坐标是 **1:1 屏幕坐标**（虽然 `ABS_MT_POSITION_Y` 量程是 960、屏幕只有 800，
  实测按原值传即可，不要按量程缩放）；
- 触摸设备 `/dev/input/event4`。

---

## 4. 踩坑记录（本轮新增）

1. **ZKSeekBar 不给图就什么都不画。** `div.bar` 只认 `data-track` / `data-fill`
   指向的图片文件（html2json 不会从颜色合成）。缺图时症状是：音量面板的卡片、
   图标、百分比、提示文字都正常，唯独进度条位置一片空白。
   → 轨道/填充图由 `tools/gen_ui.py` 的 `gen_volume_bar_assets()` 生成到
   `resources/images/vol_track.png` / `vol_fill.png`（200x20，竖向渐变，
   因为填充图会按进度裁切，横向渐变会被压扁）。
2. **`fun build` 不给 checkbox 生成绑定。** 告警
   `the version of easyui does not support ZKCheckbox xxx`，且 `ui_wifi.h` 里
   既没有 `ID_WIFI_CbWifiOn` 也没有 `mCbOnPtr`。但 `libeasyui.so` 里
   `ZKCheckBox` 符号是齐的 —— 是生成器的支持表太保守。
   → 逻辑层自己定 id（`const int ID_WIFI_CbWifiOn = 21001;`，值取自 json）
   + `findControlByID()` + `setCheckedChangeListener()`。
3. **模态窗口 + 系统 IME**：密码框用 `div.input`（ZKEditText）能直接拉起框架软键盘，
   不用自绘键盘；`WinWifiPwd` 保持 modal（弹窗语义），`WinVolume`/`WinProbe`
   必须是普通 window（modal 是输入黑洞）。
4. **触摸注入以外，还要盯 pid**：`fun launch` 之后 pid 会变；用 `ls -l /proc/<pid>/fd`
   可以确认应用是不是真的加载了 `/tmp/lib/libzkgui.so`（vs 出厂 `/res/lib/...`）。
5. **列表序号会随扫描漂移**：`pick <i>` 这种按序号的命令不可靠（我踩过：想连 #10 的
   开放网络，结果列表已刷新成别的网络，弹出了密码框）。自检要用 `pickopen` 这类
   "按条件选"的命令。

---

## 5. 真机验收记录（2026-09-12）

| 项 | 证据 |
|---|---|
| WiFi 页渲染 | `docs/shot_wifi_main2.png`（开关绿底"开"、状态卡、6 行 AP、底部图标+文字） |
| 关 WiFi | 点开关 → 日志 `wifiLogic: 用户开关 WiFi -> 0`；截图开关变灰显示"关"、列表清空 |
| 开 WiFi + 扫描 | 28~35 个 AP，含 2.4G/5G、信道 1~149、加密 WPA2/开放 |
| 连接（开放网） | 日志 `SET_NETWORK 0 ssid "aWiFi"` → `key_mgmt NONE` → `Associated` → 
`CTRL-EVENT-CONNECTED` → `dhcpRequestIp` → `state connected=1 ip='192.168.2.8'` |
| 连接状态界面 | `docs/shot_wifi_connected.png`（SSID 绿色大字 + "已连接 · 强 · -68 dBm" + IP） |
| 密码框 + 软键盘 | `docs/shot_pwd_dialog.png` |
| ~~信号探针页~~ | **已迁出**为独立应用 ⇒ `docs/shot_probe_wifi.png` / `shot_probe_bt.png` / `shot_probe_hunt.png` |
| 音量 OSD（主界面） | `docs/shot_osd2.png`，进度条 68% 填充 + 轨道 |
| 音量 OSD（WiFi 页） | `docs/shot_osd_wifi2.png` |
| 探针报告 | `docs/wifi_probe_report.md`（24 AP / CH1 挤 9 个 / 建议 CH11） |

> 注：本轮测试结束时设备**连在 aWiFi（开放热点）上、IP 192.168.2.8**。
> 不想留着就在 WiFi 页点右上角开关关掉，或用 `echo off > /tmp/pg_wificmd`。

---

## 6. 需求⑤ DLNA 投屏器 —— 可行性结论

**能做，但要分期，且不是一个小功能。**

设备侧能力已确认：
- 在线包仓库 v85x 有 `ffmpeg 4.1.9`、`av`、`aw-mpp 3.0.0-pre2`（硬解）、`civetweb`（HTTP 服务）、
  `rtsp/rtp/srt`、`mp4v2`、`openh264`；
- 硬件是 V851s，有视频解码单元（VDEC）和显示层（disp），我们已经在用 `/dev/fb0`。

DLNA 渲染端（DMR）要做四件事，缺一不可：
1. **SSDP 发现 + 描述**：监听 239.255.255.250:1900 的 M-SEARCH，回 NOTIFY/响应，
   提供 device description XML（civetweb 起 HTTP 服务）。
2. **SOAP 控制**：实现 `AVTransport`（SetAVTransportURI / Play / Pause / Stop /
   GetTransportInfo…）和 `RenderingControl`（音量）+ `ConnectionManager`。
3. **拉流解码上屏**：`http://<phone>:port/xxx.mp4` 或手机推的流 → ffmpeg 解封装/解码
   （或交给 aw-mpp 硬解）→ 送 disp 层。**第 3 步是风险最大的一步**，第一步实验建议先做
   "PC 起个 HTTP 服务放一个 MP4，设备端写个最小程序用 av 拉流解码 + 上屏"，
   跑通了再包 SSDP/SOAP。
4. 音频同步（本板音频已经打通：ALSA 常开流 + codec 输出开关已修好）。

工作量估计：第 3 步 1~2 天（含调通硬解/上屏），1+2 两天左右，整体 3~5 天。

## 7. 需求⑥ 蓝牙学习遥控器 —— **有一个硬阻塞 + 一个概念上的坑**

### 7.1 硬阻塞：本板蓝牙没起来（缺固件/缺工具）

实测（2026-09-12）：

| 检查项 | 结果 |
|---|---|
| WiFi 芯片 | `lsmod` 只有 `8733bs`（**RTL8733BS**，WiFi+BT 组合芯片） |
| BT 固件 | `/lib/firmware` 只有 `aic8800DC`；全盘 `find` 找不到任何 rtl/bt 固件 |
| BT 设备节点 | 没有 `/dev/hci*`、没有 `/sys/class/bluetooth`、没有 rfkill |
| 框架里的 BT 服务 | `/etc/init.rc`: `service hciattach /res/bin/hciattach -n ttyS2 aic`（面向 **AIC8800**）<br>`service bt /tmp/bt.sh` —— 但 **`/res/bin/` 是空的、`/tmp/bt.sh` 不存在**，且这些服务都是 `disabled` |
| 内核模块 | `8733bs.ko` + `aic8800_bsp.ko` + `aic8800_fdrv.ko`（AIC 的只是躺在 `/lib/modules` 里没加载） |
| 在线包 | 有 `btstack 1.8.0` / `blehid 1.1.3` / `gatt`（软件栈够用） |

结论：**要么本板用的 RTL8733BS，BT 需要 Realtek 的固件 + 上电/复位时序；
要么板子其实也贴了 AIC8800（则要 aic 固件 + hciattach 二进制）。这两样都不在固件里。**

需要向模组/方案厂商（Zkswe）要：
1. RTL8733BS 的 BT 固件（`rtl8733b_fw` / config）+ **BT 使能 GPIO、复位 GPIO、走哪路 UART、波特率**；
   （或者确认板上有 AIC8800、给 `/res/bin/hciattach` 与 `aic` 的 BT 固件）
2. 内核是否编了 Bluetooth 子系统（`hci_uart`/`bthci`）——没有的话就走 btstack 的 H5 transport
   （btstack 自带 h5，但要自己按厂商时序初始化芯片）。

拿到 1+2 之后，`btstack` + `blehid` 就能干活，接下来的开发是常规工作量。

### 7.2 概念上的坑：**"学习别人的蓝牙键再发出去"在 BT 上行不通**

这个必须提前说清楚，否则方向会错：

- BLE HID / 经典 BT HID 遥控器的按键是**在已加密的链路层连接里**传的 HID report，
  每条连接有独立的会话密钥（还要配对/bonding）。你**抓不到"空口明文"、也没法换一台设备重放**。
- 如果那个遥控器其实是 **2.4G 私有 RF**（很多电视盒子/投影遥控是这种，不是蓝牙），
  那就更不可能用 BT 收发器兼容 —— 物理层都不是一回事。
- 如果是 **红外遥控**，那才有"学习"的经典做法（记载波频率 + 时序），但本板没有 IR 收发硬件。

**可行的"智能遥控器"做法**（建议按这个方向做）：
- **(a) 把本机做成 BLE HID 外设（遥控器）**：用 `blehid` 暴露成 BLE 键盘/媒体遥控，
  自定义界面映射成标准 HID 键码（方向键/确认/音量/播放暂停…），发给 Android TV/盒子/手机。
  这是"智能遥控器"的正解，也是仓库里 `blehid` 包的用途。
- **(b) 把本机做成 BLE HID 主机**：连上真实的 BLE 遥控器，读它的 HID report，
  **在本机建立"物理键 → 我想干什么"的映射表**（这才是"学习"的落地含义），
  再由 (a) 转发出去或驱动本地功能。
- 需要"学习"的场合基本都是**这个键要触发什么动作**，而不是复制空口报文。

所以需求⑥我建议改名成 **「BLE 智能遥控器（HID 外设 + HID 主机映射）」**，
前提是先把 7.1 的固件问题解决。

---

## 8. 怎么跑

```bash
# 生成 UI（含进度条图）+ 打包 ftu
python tools/gen_ui.py

# 构建 + 部署（工程根目录）
./fun.exe build
./fun.exe launch

# 真机进 WiFi 应用（免触摸）
adb shell "echo wifiapp1 > /tmp/pg_autostart"

# 探针报告
python tools/wifiprobe.py --out docs/probe.md --csv docs/ap.csv
```

> `fun launch` 会清空 `/tmp`（含 `mt_test`/`busybox`），要用注入工具得重新推：
> `adb push D:\zkswe\flythings-mcp-open\bin_tools\v85x\{mt_test,busybox} /tmp/ && adb shell "chmod 755 /tmp/mt_test /tmp/busybox"`

---

## ⚠️ 换 SSID 会**覆盖**掉原来保存的网络（2026-09-13 实测）

`zknet` 走的是"**只保留一个 network 条目**"的方式：每次连接都是
`doCommand: SET_NETWORK 0 ssid "…" / SET_NETWORK 0 psk "…"` —— **固定复用 network id 0**，
并且 `wpa_supplicant.conf` 里 `update_config=1`，于是新 SSID 直接把旧条目**顶掉**。

实测：设备原来连 `TP-LINK_5G_C9E1`（192.168.0.x），我们用 QA 连了一次 `zkswe-soft_5G` 之后，
`/data/misc/wifi/wpa_supplicant.conf` 里**只剩 `zkswe-soft_5G` 一条**，
原始网络的名字和口令在设备侧**已经找不回来**了（没有历史、没有第二份配置）。

**要切回去怎么办**（本次的做法）：口令不在设备上，就从**已连过该网络的 PC** 上取 ——
Windows 会保存 WLAN 配置，`netsh wlan show profile name="<SSID>" key=clear` 能读到口令
（字段"关键内容 / Key Content"），再用 QA 连回去：

```bash
# 1) 设备侧打开我们的 WiFi 应用（/tmp/pg_wificmd 只有它在轮询）
echo wifiapp > /tmp/pg_autostart
# 2) 直连（SSID 原样传，不做下划线替换）
echo 'conn TP-LINK_5G_C9E1 <口令>' > /tmp/pg_wificmd
```
实测回切成功：`Trying to associate with 94:d9:b3:2b:c9:e3 (SSID='TP-LINK_5G_C9E1' freq=5745 MHz)`
→ `CTRL-EVENT-CONNECTED` → `DHCP ip 192.168.0.125`（**拿回了原来那个 IP**），
随后 DLNA 投屏端到端复测通过（`在线流直连` → PLAYING → `音频就绪 aac 48000Hz`）。

**教训**：设备侧"多网络记忆"是没有的；换过 SSID 之后再想回去，只能从别处拿口令，
或者一开始就把要常连的网络**先记在项目文档/口令库里**。
（如果以后要在产测/现场频繁切换，应该先给 zknet 加"多 network 条目"的能力，而不是靠 QA 打补丁。）

---

## 9. 自定义输入法（IME）替代超出屏幕边界的系统 IME

> 2026-09-13 **返工**：第一版是「画布自绘半屏键盘」（挂 `CvPwdKb`，逻辑在 `wifiLogic.cc`）。
> 功能验收是通过的，但**方向错了** —— 违反用户当日立下的铁律
> 「**非游戏功能禁止自定义绘图，一律用 UI 控件**」，而且点阵字放大后观感与其余原生控件页不一致。
> 现在改成**框架正解：自定义 IME 应用**（45 个原生 `ZKButton`）。

### 9.1 为什么要做 IME（而不是自绘 / 换系统键盘）

- 系统内置 IME 在本板 480×800 竖屏下**超出屏幕边界**（用户实测）。
- `ZKEditText` 被点击后**必然**调起 IME，头文件里**没有"关掉"的开关**（只有 `setPassword`）。
- 框架的 IME 是**应用类型**（`APP_TYPE_SYS_IME = 4`，`IMEBaseApp : BaseApp, IMEContext`），
  而且**可以自己注册**：`REGISTER_SYSAPP(APP_TYPE_SYS_IME, 类名)`。
  注册之后，**系统在任何 ZKEditText 聚焦时自动拉起它** —— 业务页一行代码都不用写。
- 依据（都是知识库明文）：`devflow/activity-code-skeleton.md` §6 + 官方范本
  `S:/projects/LearningProject/basedemo-new_z20_1024_600/ImeDemo-New/`。

### 9.2 组成

| 文件 | 作用 |
|---|---|
| `src/logic/imeApp.cc` | **手写的 IME SysApp**：`REGISTER_SYSAPP(APP_TYPE_SYS_IME, PgImeApp)`，`getAppName()` 返回 `ime.ftu`；键位靠 `pBase->getID()` 查 `kKeyIds[]` 表；`onInitIME()` 拿 `isPassword/imeTextType/现有文本` |
| `ui/ime.html` → `ui/ime.ftu` | 键盘布局：**45 个原生 ZKButton**（4×10 常规键 + 底部 `_ # 空格 退格 完成`）+ 顶部回显条 / 清空 / 收起 / 提示行 |
| `ui/wifi.html` 的 `WinWifiPwd` | 输入框恢复成**原生 `EditText`**（`data-password="1"`）；按钮上移到 y<424（下半屏是键盘的地盘） |
| `src/logic/wifiLogic.cc` | 只留三个入口：取消 / 连接 / `onEditTextChanged_EditPwd`（IME 按「完成」→ 框架回写 → 直接发起连接） |

布局要点（480×800）：
- 键盘面板 `KbPanel` 在 `y=424..800`（`#141C26`）；根节点**故意不写 `data-bg`** ⇒
  `html2json` 不设 `backgroundColor` ⇒ **上方 424px 透明**，能透出下面的密码弹窗。
- 键位：`x = 8 + 47*i`（键 41 宽、间距 6），`y(行) = 90 + 58*行`（键 52 高）。
- 底行不等宽：`_`(41) `#`(41) 空格(229) `退格`(41) `完成`(88)。
- 字库：`⇧`/`⌫` 在 `simhei` 里**没有字形**（会显示成方框）→ 改用「大写」「退格」；
  掩码用 `●`（U+25CF，simhei 有）。

### 9.3 五个坑（都实测踩过）

1. ⚠️ **fun 生成器不认 `ime` 这个 sysapp 页名**。它只认 `screensaver`/`statusbar`/`navibar`
   （源码里 `APP_TYPE_SYS_{{ .Name | upper }}` 的判定表不含 `ime`）。所以对 `ui/ime.html`
   它会额外生成一个**普通 activity**（`REGISTER_ACTIVITY(imeActivity)`）+ 逻辑脚手架
   `src/logic/imeLogic.cc` —— **那个是惰性的**（没人 `openActivity("ime")`），
   真正生效的是手写的 `src/logic/imeApp.cc`。ID 宏从生成的 `ui_ime.h` 取（单一来源）。
2. ⚠️ **`html2json` 生成 `EditText` 时漏写 `touchable`** ⇒ **点输入框毫无反应**
   （现象像"IME 没生效"，其实控件根本收不到触摸）。已在 `tools/gen_ui.py::patch_edittexts()` 补齐。
3. ⚠️ **IME 实例每次显示都可能重建** ⇒ 若 QA 文件不做基线，会把上次留的命令**重放**
   （实测：重开后自动执行了上一条 `k 45`，用空密码发起了一次连接）。
   现在 `onInitIME()` 里先把当前文件内容记成基线，且**只在键盘显示期间**执行 QA。
4. ⚠️ **空提交要挡住**：IME 若交回空串，`onEditTextChanged_EditPwd` 不能拿空密码去连（已加判断）。
5. ⚠️ **弹窗空白处要吞掉触摸**：`WinWifiPwd` 是整屏 window 且 `touchable:false`，触摸会穿透到
   下面的 WiFi 列表（可能误连别的网络）。现在 `onwifiActivityTouchEvent` 用**白名单**
   （输入框 + 取消 + 连接三个矩形）放行，其余一律吞掉；矩形坐标必须与 `ui/wifi.html` 同步。

### 9.4 真机验收（2026-09-13）

| 项 | 结果 / 证据（日志判定优先） |
|---|---|
| IME 注册与取控件 | `IME 键盘就绪：45 键中 0 个未取到；回显 ok，提示 ok` |
| 拉起路径（程序化） | `imeshow` → `IME 打开：password=1 类型=0 初始 0 字符` |
| 键盘渲染 | 45 键**全部有文字墨迹**（非底色像素 6~272/键）；面板/键/完成键底色与设计值一致 |
| 输入 | `k 11`→`q`、`k 21`→`a`、大小写 `k 31`+`k 11`→`Q`、`k 30`→`-`、`k 41`→`_`、`k 43`→空格 |
| 掩码回显 | 输入 10 字符 → 回显条 `●`×10（墨迹 63×20） |
| 退格 / 清空 | `k 44` 3→2 字符；`清空` 归零 |
| 完成（提交） | `IME 完成：交回 2 字符` → 框架回写 → `密码框收到提交（2 字符）-> 直接连接` → `连接 <ssid> (pw len=2)` |
| 收起（不保存） | `IME 收起：不保存`，弹窗保留、**不发起连接** |
| ID→键映射 | `btn <idx>` 走真实 `onClick(控件指针)`：`btn 14→'r'`、`btn 30→'-'`、`btn 41→'_'`、`btn 43→空格`、`btn 31→大写`、`btn 32→'Z'`、`btn 44→退格` |
| 键盘自动收起 | 提交后画面回到 WiFi 列表（键盘区无键盘色） |

**未验证（要说清楚）**：**"用手指点输入框 → 框架拉起 IME"这一跳没能自动验**。
原因是本板触摸注入不通（见 `docs/MCP-待改清单.md` G2：`axs_ts` 驱动 evdev 不标准，
`touch` 探测失败；`ui_test`/`mt_test` 事件进了内核但框架不响应）。
上面验的是"IME 收到命令后的全链路" + "程序化 showIME 能拉起 IME"，**控件命中那一步需要真手指确认**。

### 9.5 QA 命令（免触摸验收）

| 通道 / 命令 | 作用 |
|---|---|
| `/tmp/pg_wificmd` → `imeshow` / `imehide` | 程序化拉起 / 收起 IME（等价于点输入框） |
| `/tmp/pg_imecmd` → `k <1..45>` | 按键（直接走 `handleKey`） |
| `/tmp/pg_imecmd` → `btn <1..45>` | 拿**真实控件指针**喂 `onClick`（验 ID→键映射） |
| `/tmp/pg_imecmd` → `shift` / `del` / `clear` / `done` / `hide` / `lbl` | 功能键；`lbl` 把缓冲打进日志 |

> ⚠️ IME 的 QA 只在**键盘显示期间**执行，且 `onInitIME` 会先把当前文件内容记成基线
> （防重放，见 9.3-3）。`fun launch` 会清 `/tmp`，命令文件要重写。

### 9.6 键盘复查（2026-09-14）：**IME 本身是好的，坏的是"没重启进程"**

用户反馈「键盘功能不可用」。真机逐条复查（`pginj` 真触摸注入 + QA）结论：

| 步骤 | 判据 | 实测 |
|---|---|---|
| 点列表里的加密网络 | 日志 | `wifiLogic: 选网 TPLink_zkswe (WPA2)` → `打开密码弹窗` ✅ |
| 点密码框 | 日志 | `IME 键盘就绪：45 键中 0 个未取到` → `IME 打开：password=1` ✅ |
| 点数字键 1/2/3 | 日志 | `IME 键 '1' -> 1 字符` … `'3' -> 3 字符` ✅ |
| 键盘渲染 | 逐像素 | 键底 `#26313F`、字形近白 `(225,229,234)`、完成键 `#2E7D5B` ✅ |
| 键盘上方仍是弹窗 | 逐像素 | (240,150) = `#0B0F14`（弹窗底色）⇒ IME 上半屏是**透明**的 ✅ |
| 点「完成」 | 日志 | `IME 完成：交回 3 字符` → `密码框收到提交 -> 直接连接` ✅ |

**那"不可用"是怎么来的**：设备当时跑的是**上一次启动时载入内存的 libzkgui.so**
（`/res/lib/libzkgui.so` 已换新，但 zkgui 进程没重启）。
判据很硬：同一份源码，**重启 zkgui 后** `imeshow` 立刻能拉起 IME，重启前一行日志都没有。
⇒ **结论：换库/换 ftu 之后必须让 zkgui 重新起一次**（`ps` 取 pid → `kill -9` → init 自动拉起），
否则"改了没反应"极易误判成功能坏了。

#### 顺带修掉的一个不稳点：点输入框**不保证**拉起键盘

`docs/wifi-app.md` §9.4 早就承认"**用手指点输入框 → 框架拉起 IME 这一跳没能自动验**"。
这次用真触摸注入反复点，确认它**是间歇性的**：同一个密码框，有时一击即弹，有时点两三下没反应
（自动拉起依赖控件聚焦，有竞态）。

修法：**不再依赖框架的自动拉起** —— `wifiLogic.cc` 抽 `showPwdIme()`，
在触摸白名单命中密码框、**抬手**时自己调 `EASYUICONTEXT->showIME(...)`
（与 QA `imeshow` 走同一条调用，那条路每次必成功）。

真机复验：`/tmp/pginj tap /dev/input/event0 240 246` →
`wifiLogic: 拉起键盘（弹窗=1，输入框已有 0 字符）` → `IME 打开：password=1` ✅

> ⚠️ 键盘显示期间，**弹窗上的「取消 / 连接」点不到**（IME 应用整屏在最上层，
> 触摸归它）。这是框架 IME 的正常行为，所以设计的正路是键盘上的「完成」
> （提交即连接）——`ui/wifi.html` 的提示行也是这么写的。

### 9.7 ★★ 「WiFi 键盘敲不进字」真因（2026-09-14 二次定位，一次改对）

**现象**：密码弹窗和键盘**都正常显示**（键盘键位、完成后提交都对），但点键位
**一点反应都没有**，日志里连一条触摸记录都没有。

**真因：wifiLogic **自己的触摸回调**按坐标把键盘的触摸"吞"了。**

```cpp
// 改前（节选）：弹窗显示期间只放行 3 块区域，其余一律 return true
static bool onwifiActivityTouchEvent(const MotionEvent &ev) {
  if (sPwdDlgOpen) {
    // 白名单：EditPwd(32,210,416,72) / BtnPwdCancel(32,330,196,64) / BtnPwdOk(252,330,196,64)
    ...
    return true;   // ← 弹窗"空白处"吞掉
  }
}
```

- 白名单最高只到 **y=394**（连接按钮底边），而**键盘面板铺在 y=424..800** ⇒
  **每一个键位都落进"空白处"分支被吞掉**。
- 责任链：IME 是独立 SysApp、整屏在最上层，但**它的触摸仍然经过当前 Activity 的
  触摸回调**（框架先给 Activity，Activity 不吞才往下发）。回调返回 `true` = 键位
  永远收不到 DOWN。

**决定性判据（对照实验，一次就能分清"键盘坏"还是"宿主页吞"）**：

| 操作 | 实测 |
|---|---|
| WiFi 页 + 键盘显示 → 点键位 | **0 条日志**（`IME 键` 一条不出） |
| 长按实体键离开本页 → **键盘还开着** → 点同一个键位 | `IME 键 '1' -> 1 字符` ✅ |
| 再回本页 → 点键位 | 又没反应 |

⇒ **同一个 IME 实例**（离开后那一次没有重新 `IME 打开`）**在同一坐标上时而可用时而不可用**
⇒ 键盘、IME 注册、坐标、协议**全部无辜**，是**宿主页把触摸吃了**。

**修法：删掉坐标白名单，改在"真正做事的地方"挡。**

| 原来要防的事 | 现在怎么防 |
|---|---|
| 点弹窗空白处穿透到下层列表 → **误连别的网络** | `onListItemClick_ListWifiAp()` 开头判 `sPwdDlgOpen` 直接 return（附录文日志） |
| 点输入框 → 拉键盘 | 保留唯一一处坐标判断：命中 `EditPwd` 时抬手显式 `showIME()`（见 §9.6） |

> **通用规则（已进 docs/ui-design-baseline.md 自测清单）**：
> **Activity 的触摸回调里不许用"坐标白名单"吞触摸。** 要挡就在"真正发起动作的地方"挡
> （列表点击/按钮回调）——在坐标上猜，必然和后来的浮层（键盘、OSD、弹窗）打架。
> 症状还极具误导性：**界面上什么都正常，就是点不动，而且一条日志都没有**。

真机复验（完整链路，全是真实触摸）：

```
点列表第 2 行        → wifiLogic: 选网 zkswe-soft_5G (WPA2) open=0
                     → wifiLogic: 打开密码弹窗 'zkswe-soft_5G'（键盘交给 imeApp）
点密码框             → wifiLogic: touch action=1/2 (240,246) ime=0
                     → wifiLogic: 拉起键盘（弹窗=1，输入框已有 0 字符）→ IME 打开：password=1
点键位 1/8/9/0       → IME 键 '1' -> 1 字符 / '8' -> 2 / '9' -> 3 / '0' -> 4   ✅
点「完成」           → IME 完成：交回 4 字符
                     → wifiLogic: 密码框收到提交（4 字符）-> 直接连接 → performConnectWifi...
```

### 9.8 顺带修掉：WiFi 页会被**屏保**盖住（30 秒必踩）

**现象**：在 WiFi 页静置 30 秒，**屏保（整屏 SysApp）盖上来，连键盘一起盖**。
用户第一下触摸只能"唤醒"屏保 —— 表现同样像"点了没反应"。
本次抓屏取证实锤：点列表前的截图是**屏保**（4 位翻页钟 + 喜欢/不喜欢按钮），
点一下才露出 WiFi 页。

**真因**：屏保开关原来**只有主界面** `mainLogic::tickScreensaverPolicy()` 在管，
而**主界面不在前台时它的定时器不跑** ⇒ 进独立 ftu（wifi/remote）后屏保一直保持"开"。

**修法**（官方推荐用法，`easyui/include/entry/EasyUIContext.h:159` 明确写了这个场景）：

```cpp
// wifiLogic.cc
onUI_show():  setScreensaverEnable(false); if (isScreensaverOn()) screensaverOff();
onUI_quit():  setScreensaverEnable(getScreensaverTimeOut() > 0);
```
⚠️ **不要在 `onUI_hide` 恢复** —— 键盘（IME SysApp）弹出时本页会 hide，
一恢复屏保就又回来了。

**配套修掉一个更隐蔽的坑**：`tickScreensaverPolicy()` 的收敛判据。

```cpp
// 改前：缓存"模式"，模式没变就直接 return
static int sWork = -1;
if ((int)work == sWork) return;
// 改后：跟"框架的实际开关"收敛
const bool want = !work && (EASYUICONTEXT->getScreensaverTimeOut() > 0);
if (EASYUICONTEXT->isScreensaverEnable() == want) return;
```
为什么必须改：独立页把屏保关了，**回主界面时模式还是 `MODE_MENU`**（和进之前一样）
⇒ 判"没变化"直接返回 ⇒ **屏保再也回不来**（实测：主界面 QA `saver` 报 `enable=0`，
改后 `enable=1`、等 35 秒正常进屏保）。

**真机验收**：

| 场景 | 判据 | 实测 |
|---|---|---|
| WiFi 页静置 38 秒 | 屏幕内容 | **仍是 WiFi 列表页**（没有屏保） ✅ |
| 同上，QA `who` | 开关状态 | `saverEnable=0 saverOn=0` ✅ |
| 回主界面后 QA `saver` | 开关状态 | `enable=1` ✅ |
| 主界面静置 35 秒 | 日志 | `performScreensaver` 出现 ⇒ 屏保正常回来了 ✅ |

### 9.9 本次新增的 QA 命令

| 命令 | 作用 |
|---|---|
| `who` | 打印 `ime=` / `dlg=`（弹窗）/ `wnd=`（弹窗窗口是否 show）/ `saverEnable=` / `saverOn=` —— **"触摸被谁吃了"一眼定位** |
| `snap` / `snapinfo` | 把**当前网络**存成"回家快照" / 打印快照状态（原网络、是否在目标网模式、控制口） |
| `restore` | 把快照里的原网络**还原回去**（配置 + 重读 + 重连） |
| `wpa <命令>` | 直连 wpa_supplicant 控制口（`/dev/socket/wlan0`）发一条命令，诊断用（`STATUS`/`LIST_NETWORKS`…） |

> ★ **2026-09-15 更新：「换 SSID 会顶掉原保存网络」这件事已经有解了（v1.25）** ——
> 新增「回家快照」：`src/platform/PgWifiCfg.{h,cpp}` 在**任何一次发起连接之前**把
> `/data/misc/wifi/wpa_supplicant.conf` **整份**存到 `/data/misc/wifi/pg_home.conf`（+ `pg_home.meta`），
> 需要时一键还原（覆盖回文件 + 控制口 `RECONFIGURE` → `ENABLE_NETWORK all` → `RECONNECT`，
> 配置**逐字节等价**、自动重连并重新拿 IP）。
> 入口两处：探针应用局域网页顶栏「目标网 / 恢复原网」；以及本应用的 QA `snap`/`restore`。
> 实现细节、实测与 7 条坑见 **`docs/wifi-probe-app.md` §12**（含"还原成功但连不上"的真凶
> `[TEMP-DISABLED]`）。**下面这段血案的代价从此不再存在**。

---

## 10. 2026-09-15 第二版 UI：改成手机「设置 > 无线局域网」的样子

用户原话：「WiFi 界面的开关和 UI 布局不合适，参考手机设置界面的效果修改」。

### 10.1 旧版哪里"不合适"

| 位置 | 旧版 | 问题 |
|---|---|---|
| 总开关 | 顶栏右侧一个 **108x36 的方块**（`ZKCheckBox` 无图模式：选中变绿 + 文字写「开/关」） | 那是"按钮"不是"开关"。手机上的 WiFi 开关是**滑轨**（iOS UISwitch / Android Switch），靠滑块位置表达状态，不写字 |
| 当前连接 | 一张 92 高的独立状态卡（大字 SSID + 状态行 + IP） | 手机设置里没有"详情卡"，当前网络就是**列表里带勾的那行** |
| 列表 | 每行一张独立圆角卡（行距 6），右侧两行文字（档位词 + dBm） | 手机设置是**一整组卡片 + 行分隔线**；dBm 数字在列表里属于工程师视角 |
| 底部 | 两个 222x88 的大按钮（刷新 / 信号探针） | 手机设置的动作在**顶栏右上角**或**列表尾部的跳转行**，不在底部放大按钮 |

### 10.2 改成了什么

```
┌────────────────────────────────┐  ← 无线局域网        🔄  顶栏：标题居中 + 右上刷新图标
│ ┌────────────────────────────┐ │
│ │ 无线局域网        ●──────  │ │  开关卡（h=88）：iOS 滑轨开关 72x44
│ │ 已连接 TPLink_zkswe   1.2.3│ │  副标题（左）+ IP（右）
│ └────────────────────────────┘ │
│ 可用网络                        │  小节标题（14px / T2）
│ ┌────────────────────────────┐ │
│ │ TPLink_zkswe       🔒 满格  │ │  ← 整组卡：首行上圆角
│ │ 信道 6 · WPA2 · 已连接      │ │
│ ├────────────────────────────┤ │  ← 1px 分隔线（左缩进 16，与文字对齐）
│ │ Home-2.4G          🔒 强    │ │
│ └────────────────────────────┘ │  ← 末行下圆角
│ ┌────────────────────────────┐ │
│ │ 📡 信号探针             ›  │ │  跳转行（整行可点）
│ └────────────────────────────┘ │
└────────────────────────────────┘
```

要点：
- **开关是原生 checkbox 挂两态位图**，不是画布自绘：
  `ui/wifi.html` 写 `data-pic="switch_off.png" data-pic2="switch_on.png"` +
  `data-icon-w/h = 控件尺寸`，图由 `tools/ios_theme.py` 的 `gen_switch()` 生成（72x44）。
  `ZKCheckBox` 自己按 `checked` 切图 ⇒ **点按判定、选中回调、防回环逻辑一行都没改**。
  ⚠️ 逻辑层那句 `setText("开"/"关")` 必须删掉（开关上不该有字，也会压在图上）。
- **列表是"整组卡"**：`ios_row_top / mid / bot / solo` 四张行底图叠在同一位置，
  由 `wifiLogic.cc` 的 `obtainListItemData_ListWifiAp` 按 index 切 `visible`
  （首/中/末/独）—— 与主界面 `Icon0..23` 按 slot 切图同一手法。
  分隔线**画在底图里**（subItem 不认 `data-bg`，只认 `data-bgpic`）。
- 行右侧 = **档位词（满格/强/中/弱）+ 加密锁图标**；dBm 数字去掉（要 dBm 用 QA `dump`）。
- 一键入口：顶栏右上 🔄 = 原「刷新」按钮（caption `BtnRescan` 不变，回调没动）。

### 10.3 踩坑（两条，都是"看图看不出来、查代码也查不出来"的）

**① 行底图**不能**用九宫格 —— 图内容高 == 控件高时会被吃掉 2px。**
  第一版把 `ios_row_*.9.png` 做成九宫格（图内容 88x88）。真机像素扫描：
  行高 88 里只有 **85px 面板 + 1px 分隔线 + 2px 黑**，行与行之间一道黑缝。
  原因：九宫格那圈 1px marker 边被算进了缩放（等于把"图 90px"映射到"控件 88px"）。
  ⇒ 行底图这种"宽度由列表定死、不需要横向拉伸"的东西，一律出
  **普通 PNG（图尺寸 == 控件尺寸）**：1:1 贴图、无解析不确定性，还顺带走
  "整图不透明 → memcpy" 快路径。**判据**：`devshot.py` 扫一列像素的颜色分段。

**② item 高不是 `data-h`，是 `(列表高 - 行距×(rows-1)) / rows`。**
  列表 440 高、rows=5、rowSpacing=0 ⇒ item 高 **88**（不是我在 HTML 里给行卡写的 88 就完事，
  而是行卡**必须**等于这个算出来的值）。行卡比 item 矮 ⇒ 每行底部漏黑缝；
  比 item 高 ⇒ 相邻行互相盖住。改列表高度/行数前先算一遍。

### 10.4 本次真机验收判据（可复现）

```bash
python tools/grab.py .shots/x.png && python tools/devshot.py .shots/x.png --map
# 再看像素：
#   (400,120)==(48,209,88) 且 (426,120)==(255,255,255)  → 开关"开"（绿轨+钮靠右）
#   (382,120) 灰 (#3A3A3C) 且 (398,120)==白              → 开关"关"（灰轨+钮靠左）
#   x=240 列从 210 起：PANEL×87 → LINE(56,56,58) → PANEL… 每 88px 重复 → 整组卡+分隔线正常
#   首行上圆角：y=210 时 x=16..30 全黑、x=40 起面板色
```
⚠️ QA `off` / `on` 会设 `sCbHoldUntil = now()+8s`（防回环），**8 秒内不回写勾选态** ——
   验证"关态图"必须等过 8 秒再抓屏，否则会误判成"开关不跟着变"。

### 10.5 顺带记录：`gen_font.py` 缺依赖

`python tools/gen_font.py` 报 `缺少 fontTools：python -m pip install fonttools`
（收集字符那步正常，裁剪 ttf 那步需要 fontTools）。本次改动的新文案
（"无线局域网 / 可用网络 / 信号探针 / 已连接 / 隐藏网络"）**实测真机全部有字形**
（抓标题区放大后 5 个字齐全），所以本次没受影响。
但**下次新增生僻字前要先把这个依赖装上**，否则会静默漏字（FlyThings 是整字库替换、
没有逐字回退）。

---

## 11. 第二版补丁（v1.28.2）：状态图标 + 密码弹窗 + 键盘

用户反馈两条：**「信号强度和加密方式图标有点太过于抽象了」**、**「密码输入和键盘界面一起修改」**。

### 11.1 列表行右侧：文字/细线框 → 图形

| 项 | 改前 | 改后 |
|---|---|---|
| 信号强度 | 文字档位词「满格 / 强 / 中 / 弱」（15px） | **三段弧线图标**（28x28）：点亮 1/2/3 段 = 弱/中/强；配色 强=绿 `#30D158` · 中/弱=青 `#64D2FF` · 很差=只剩内段灰 `#48484A` |
| 加密 | `gen_res` 的 `lock` glyph（24 单位画布上的**细线框**，渲染到 22px 只剩 12x14 墨迹、线宽 1~2px） | **加粗实心锁**（28x28）：粗锁环 + 实心锁体 + 卡片色挖出的钥匙孔 |

* 四档信号图**叠在同一位置**，逻辑层按 `rssi` 只显示一张（`subVisible`）——与行底图同一手法。
* 图由 `tools/ios_theme.py` 的 `gen_ap_icons()` 生成（PIL 画，6x 超采样；**烘卡片底** `#1C1C1E`）。
* dBm 数字之前已去掉（要精确值用 QA `dump`）。

### 11.2 密码弹窗：散控件 → 居中卡片

改前是"标题/输入框/按钮散在全黑底上"；改成**一张 400x260 的居中弹窗卡片**（`CardPwd`，`data-round="panel"`），
里面按内边距对齐：标题（20px）→ 网络名（14px 灰）→ 输入框 → 错误提示（红 13px）→ 两个等宽按钮（取消灰 / 连接绿）。
* 卡片底 `y=410 < 424` —— 下半屏是键盘的地盘，**压过去就点不到**（老约束，别忘）。
* ⚠️ 逻辑层 `onwifiActivityTouchEvent` 里那份"输入框触摸白名单"坐标**必须跟着改**（32/210/416/72 → 60/238/360/60）。
* 删掉了原来那句"键盘在下半屏 · 点「完成」可直接连接"（iOS 弹窗没有这种说明，键盘自己会讲）。

### 11.3 键盘：配色分层（更像手机键盘）

* 面板保持纯黑；**普通键 `#2C2C2E` → `#3A3A3C`**（与黑面板拉开对比）。
* 功能键（清空 / 收起 / 大写 / 退格）`#1C1C1E` → **`#2C2C2E`**（比普通键**暗**一档 —— iOS 键盘就是"字母键浅、功能键深"）。
* 顶部**回显条改成圆角次级面板**（`data-round="panel2"`，`#2C2C2E`）。
* ⚠️ **修掉一个老 bug**：完成键原来 `data-bg="#30D158"` + `data-color="#30D158"` = **绿底绿字，字看不见**；文字改 `#000000`。
* 键位几何、captions 全部未动（`imeApp.cc` 按 ID 查表，一行没改）。

### 11.4 ★★ 新坑：listview 子项（subItem）的背景图**水平多 16px 缩进**

真机第一版里，锁和信号图标整体**右移约 16px**：控件盒写 `x=366`，图的左上角实际落在 **382**。

* **标定法定位**（推荐照抄）：临时生成两张"标定图"——`calib_a`（左上红块 8x8 + 右下黄块 8x8）、
  `calib_b`（整圈 2px 绿边框），分别替换两个 subItem 的 `data-bgpic`，打包上机抓屏量色块边界。
  实测：红块落在 `x=382..389`（图内 0..7）⇒ **图左边界 382 = 控件 left + 16**；
  绿框落在 `x=420..447`（28 宽，1:1）⇒ 同样的 +16；**y 方向无偏移**。
* **结论**：subItem 渲染 `backgroundPic` 时水平固定 +16px，竖直不加、尺寸 1:1。
* **修法**：源稿里把 x 写成"设计位置 − 16"（锁 350→实际 366、信号 388→实际 404），
  并在源稿注释里写清原因 —— 否则后人看到 350 会以为写错了，一改就整体右移压到行边缘。
* ⚠️ `RowCard`（448x88 整行底图）**没有这个偏移**（图从 item x=0 铺满）—— 偏移只在小尺寸图上暴露出来，
  因为它一旦偏 16px 就肉眼可见，而整行底图偏 16px 会被当成"行内边距"。

### 11.5 ★★ 顺带修掉一个老缺陷：弹窗里的「取消 / 连接」**从来就点不动**

真机实测（`pginj` 注入 + logcat）发现的：

```
点「取消」(145,364) → logcat 有  wifiLogic: touch action=1/2 (145,364) ime=1
                      但 onButtonClick_BtnPwdCancel **一次都没触发**
点「输入框」(240,268) → 正常触发 showPwdIme()   ← 因为那段是手动判坐标调用的
```

**根因**：弹窗窗口 `WinWifiPwd` 是 `touchable=false`，而这条**不能改** ——
整屏窗口一旦可触摸，连键盘区域（y≥424）的触摸也会被本 Activity 吃掉，
IME 就再也收不到按键（"wifi 键盘敲不进字"的老血案，见 §10 与 `onwifiActivityTouchEvent` 的注释）。
代价就是：**窗口不可触摸 ⇒ 窗口里的按钮收不到点击**。
（用户此前一直靠键盘上的「完成」提交，那条路走的是 `onEditTextChanged` → 不依赖按钮，所以没暴露。）

**修法**：在 `onwifiActivityTouchEvent` 里手动判坐标 + 直接调回调，**只处理 y < 424**：

| 区域 | 行为 |
|---|---|
| `y ≥ 424`（键盘） | **原样放行 `return false`** —— ⚠️ 这一条是命根子，老白名单就是把键盘区域的触摸也吞了才出事 |
| 输入框 (60,238,360,60) | 抬手 → `showPwdIme()`，吞掉 |
| 取消 (60,336,170,56) / 连接 (250,336,170,56) | 抬手 → 直接调 `onButtonClick_BtnPwdCancel/Ok` |
| 卡片其余 (40,150,400,260) | 吞掉（防穿透到下层列表） |

验证（都过了）：
```
「取消」→ logcat: 弹窗内点「取消」（手动分发）→ 密码框取消 ；抓屏卡片位置变黑 ✓
「连接」→ logcat: 弹窗内点「连接」（手动分发）→ 连接 TPLink_zkswe (pw len=0) ✓
键盘「1」「2」→ logcat: IME 键 '1' -> 1 字符 / '2' -> 2 字符 ；回显条 393 个白像素 ✓  ← 不能回归的那条
```
⚠️ 改弹窗布局时，这 5 组坐标（键盘顶 424 / 输入框 / 卡片 / 两个按钮）要跟着改，否则"看着能点、实际点不动"。

---

## 12. v1.28.5：开关卡的 IP 从"与状态行并列"改成"独立第三行"

用户反馈：**「无线局域网已连接的ip地址放到已连接的ssid底部。现在被开关按键覆盖到了。」**

**问题**（量出来的，不是看出来的）：

```
TextWifiIp   x=280..432   ← 盒子的右端
CbWifiOn     x=376..448   ← 滑轨开关
⇒ 重叠区 x=376..432（56px）：IP 文字右侧被开关压住
```

**改法**：开关卡从"两行 + 右侧 IP"改成**三行**（标题 / 状态·SSID / IP），IP 左对齐、与上面两行同一个左边距：

| 控件 | 改前 | 改后 |
|---|---|---|
| `TextWifiSwitchLabel` | x=32 y=88 w=280 h=32 fs=22 | x=32 y=84 w=**328** h=28 fs=22 |
| `TextWifiState` | x=32 y=122 w=240 h=26 fs=13 | x=32 y=114 w=**328** h=20 fs=13 |
| `TextWifiIp` | x=**280** y=122 w=152 h=26 align=**right** | x=**32** y=**136** w=**328** h=20 align=**left** |
| `CbWifiOn` | x=376 y=98 w=72 h=44 | 不动 |

**★ 布局约束（写进源稿注释了）**：三行文字的宽度统一 **328（= x 32..360）**，
**必须在 x<376 结束** —— 那是滑轨开关的左边界，越界就又被盖住。

**真机验收判据**：

```
逐行扫开关卡（字符画，每 3px 采样）：
  y= 88..106  第一行「无线局域网」
  y=118..128  第二行 状态/SSID
  y=140..148  第三行 IP（绿色 #30D158，宽度 ≈120px ≈ 13 字符）
  三行文字全部止于 x≈181；开关在 x≈373..448 ⇒ 零重叠 ✓
```

⚠️ 与 §11.4「subItem 背景图水平多 16px」是**两码事**：那条是 listview 子项的偏移，
本条是**普通控件之间的盒模型重叠**。改任何一行的 `data-w` 前先算一遍"右端 < 376"。

### 12.1 ★ 顺手加了自动化把关：`tools/check_overlap.py`

这类"IP 被开关压住"的问题**抓屏查不出来** —— 被盖住的内容在屏幕上就是"没有"，
你不知道本该有什么；只有"知道这里应该有一段字"的人才能发现。⇒ 必须在**生成阶段**用盒模型算。

新增 `tools/check_overlap.py`，已接进 `gen_ui.py` 的流程（与 `check_stretch` 并列，写完 json 后跑）。

**判据的两个关键点（都踩过）**：

① **「完整包含」= 正常，「部分重叠」才是问题**。
   卡片/面板底图罩住内容、四档信号图叠在同一位置靠 `visible` 切换 —— 这些都是完整包含/完全重合。
   只有**互不包含的部分重叠**才一定互相遮挡。

② **文字要按「实占宽度」算，不能按控件盒**。
   居中标题的盒常常是整屏宽（`TextWifiTitle` 盒 `x=0..480`），
   按盒宽判会把"标题 vs 左右按钮"全报成问题（第一版实测 **2 处误报**）。
   实测标定：**中文 ≈ fontSize、ASCII ≈ fontSize × 0.5**
   （验证：「无线局域网」fs=22 → 估 110px，真机墨迹实测 **108px**；
   `"192.168.1.100"` fs=13 → 估 85px，实测 **89px**）。
   对齐方式按 `alignment` 码算：**36=左 / 37=中 / 38=右**（html2json 的映射）。

**工具有效性验证**（`python tools/check_overlap.py <目录>` 可指定路径，便于这类回归）：

```
把 IP 还原到改前的 x=280..432 / align=right：
  [高] TextWifiIp(text) x=346..432   ← 右对齐的**实占区**（85px），不是盒子
       CbWifiOn(solid)  x=376..448
       → 重叠区 x=376..432（336 px²）   ✓ 准确报出
全工程 15 个 json 现状：PASS（无部分重叠）
```

⚠️ 内容由**逻辑层运行时写**的文字（json 里 `text` 为空，如 `TextWifiIp`）无法估算实占宽度，
工具会退回"按盒宽"保守判定 —— 宁可多报也不错漏。

---

## 13. v1.28.6：左上角返回按钮点不动（+ 连带查出「信号探针」行也点不动）

用户反馈：**「wifi界面左上角返回图标不可用。」**

### 13.1 根因：**装饰控件的盒会占住命中区**（`touchable=false` 不是穿透）

框架按「**后声明 = 更上层**」做触摸命中测试，而后声明的控件**即使 `touchable=false`
也照样占住命中区** —— 它只是"自己不响应"，**并不把命中让给下层**。
（与记忆里"整屏 SysApp 浮层不吃穿透"是同一机制。）

顶栏原来的声明顺序：

```html
#1 BtnWifiBack     ← 返回按钮（要点的）
#2 TextWifiTitle   ← 标题，盒 x=0..480（整屏宽），**声明在后 ⇒ 盖在按钮上**
#3 BtnRescan       ← 刷新按钮，在标题之上 ⇒ 不受影响
```

⇒ **返回按钮被整屏宽的标题盖掉**；而刷新按钮（声明在标题之后）正常 ——
这个"同容器两个按钮表现不同"的现象，正是定位的关键线索。

### 13.2 连带查出：底部「信号探针 ›」整行也点不动（同一机制）

`tools/check_overlap.py` 的**触摸遮挡**判据（第 ② 类）一次报出 3 处：

```
BtnProbe（整行可点）← 被声明在它之后的三个装饰盖住
  ImgProbeIcon   x=32..76    重叠  1936 px²
  TextProbeLabel x=92..332   重叠 10560 px²
  ImgProbeArrow  x=404..436  重叠  1024 px²
```

真机确认：**BtnProbe 的回调自带 `LOGD`，点行中心零日志** ⇒ 回调根本没被调用。

### 13.3 修法

**① 顶栏标题：把盒缩到不压按钮**（这是最小改法）

```html
TextWifiTitle  data-x="60" data-w="360"   ← 原 x=0 w=480（整屏宽）
```
中心仍是 240（居中效果不变），两侧给两个按钮各留出空间。

**② 「信号探针」行：拆三层**（因为装饰必然在按钮内部，缩不开）

```html
<!-- ① 底图（纯装饰） -->
<div class="btn" data-caption="BtnProbeBg" data-bgpic="ios_panel.9.png" ... data-untouchable="1"></div>
<!-- ② 装饰（icon/text 默认 touchable=false） -->
<div class="icon" data-caption="ImgProbeIcon" ...></div>
<div class="text" data-caption="TextProbeLabel" ...>信号探针</div>
<div class="icon" data-caption="ImgProbeArrow" ...></div>
<!-- ③ 命中层：无图/无文字/无 data-bg ⇒ html2json 不设 bgColorTab（透明），声明最后 ⇒ 最上层 -->
<div class="btn" data-caption="BtnProbe" data-x="16" data-y="690" data-w="448" data-h="72"></div>
```

★ ③ 的关键：**"无图 + 无文字 + 无 `data-bg`"的按钮是透明的**
（html2json 1373 行 `if bgc or text:` 才设 `bgColorTab`；否则不设 ⇒ 透明）。
给 ③ 挂 `data-bgpic` 或写文字都会被填上底色，把下面两层盖住。

### 13.4 真机判据（都过）

```
返回按钮   → logcat: wifiLogic: 顶栏返回被点击 -> closeActivity(wifiActivity)
                     wifiLogic: onUI_quit
信号探针行 → logcat: wifiLogic: 打开信号探针应用（probeActivity）
                     probeLogic: init（信号探针独立应用）
tools/check_overlap.py → PASS（无部分重叠、无触摸遮挡）
```

### 13.5 ⚠️ 本次排查踩的四个坑（都很有欺骗性）

**① 回调里没有日志 ⇒ "点完没反应"无法定位。**
`onButtonClick_BtnWifiBack` 原本一行 `LOGD` 都没有，导致"触摸没到按钮"和
"回调调用了但 `closeActivity` 失败"**从外部完全无法区分**（表现都是"画面不变"）。
⇒ 给交互回调加一行日志，成本极低、价值极高。已加并保留。

**② `LOGD` 被条件包住 ⇒ "没有日志"被误读成"事件没到达"。**
`wifiLogic` 的 `touch` 日志写在 `if (sPwdDlgOpen)` 里 —— 只有密码弹窗打开时才打。
我据此判定"触摸没到 Activity"，**白排查一轮**。
⇒ 判据要看清它**在什么条件下才输出**。

**③ 抓屏的 `pan` ⇒ 两张图来自不同半页，"画面没变"是假象。**
`grab.py` 输出里的 `pan_y=800` / `pan_y=0` 是**不同半页**。
我曾据"点击前后画面相同"判定"按钮点不动"，实际是两次抓到了不同半页的残留帧。
⇒ **同一次对比必须确认 pan 一致**，或用 logcat 这类与显示无关的判据。

**④ QA 通道的命令重放会把测试环境搞脏。**
`/tmp/pg_wificmd` 里上一轮留下的 `pick 0` 在**进程重启后被重放**（打开密码弹窗），
弹窗会把 `onwifiActivityTouchEvent` 里的触摸吞掉 ⇒ 点哪儿都没反应。
⇒ **测完清空 QA 通道**（工程既定规矩），**测试前也要先清一次**。

### 13.6 新增的自动把关

`tools/check_overlap.py` 加了**第 ② 类判据「触摸遮挡」**（按**控件盒**判，
与第 ① 类"视觉重叠"按文字实占区判互补）：

> 下层的**可触摸**控件，若被**上层（后声明）控件的盒**覆盖 ⇒ 它的点击收不到。

这条判据在当前工程上跑出的 3 处，**全部是真问题**（已修）。
