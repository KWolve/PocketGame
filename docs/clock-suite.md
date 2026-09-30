# 时钟套件 · 能力核查（先过 MCP，再动手）

> 工作方式（作者 2026-09-13 定）：**下一步的功能，一律先从 MCP 过一道，尤其是涉及硬件平台的**。
> 本文就是"时钟套件（世界时钟 + 闹钟 + 校时）"开工前的核查记录 —— 每条结论都标了**来源**
> （MCP 工具 / 设备实测），没有"我觉得"。
>
> **关联**：响铃方案另见 **`docs/audio-output.md`**（三条音频路径的实测对比 + 参考工程"官方音频播放器" `zk_audio_player` 的接入方式）—— 作者 2026-09-13 定：**闹钟走音频播放器，不用蜂鸣器**。

## 0. 一句话结论

**能做**：世界时钟、闹钟（到点**用官方音频播放器出声** + 亮屏）、自动校时、手动设时间、亮度调节。
**做不了**：掉电保持时间（**无 RTC**）。**要靠联网校时**，这也是在校时上不能省的原因。

## 1. 核查了哪些（可复现的命令）

| 想确认什么 | 用了什么 | 结果 |
|---|---|---|
| 本板硬件规格 | `flythings_hardware_info(platform=v85x)` | 型号库只收录 `PocketDisplay4`，**本板不在库** ⇒ 按铁律翻 S 盘参考工程（`V851ExtendedScreen_ap_p2p`） |
| 有没有时钟/闹钟组件 | `flythings_knowledge_search` ×3 | 只有 `digitalclock` 控件与 `system_time` 用法；**无现成闹钟组件**（相关查询命中低置信，知识库明示"未收录，禁止用别的 GUI 框架类推"） |
| 有没有 RTC | `flythings_package_search(rtc)` / `(time)` | **空**（`rtc` 只命中无关的 webrtc-aec） |
| 平台包全集 | `flythings_list_packages(v85x)` | 25 个包；`zkhardware` = 硬件能力（串口/GPIO/按键） |
| 硬件能力清单 | `flythings_get_package_api(zkhardware)` | **查到了两个原本不知道的能力**（见下） |
| 背光/蜂鸣器/时区数据 | 设备实查 | 见 §3 |

## 2. ★ 这次"过 MCP"最大的收获：两个没查就不会知道的能力

`zkhardware` 的 API 里有：

```cpp
// 蜂鸣器（hw/HardwareManager.h）—— 闹钟响铃的省电选择，不用去折腾音频流
HARDWAREMANAGER->beep();
HARDWAREMANAGER->setBeepPWM(uint32_t freq, uint8_t duty);   // 可调频率/占空比
HARDWAREMANAGER->setCustomBeep(cb);                          // 自定义鸣叫

// 背光/屏（utils/BrightnessHelper.h）—— 闹钟亮屏、屏保调暗都靠它
BRIGHTNESSHELPER->setBrightness(0~100);   // ⚠️ 0 不等于关屏
BRIGHTNESSHELPER->getBrightness(); BRIGHTNESSHELPER->getMaxBrightness();
BRIGHTNESSHELPER->screenOff(); screenOn(); backlightOff(); backlightOn();
BRIGHTNESSHELPER->isScreenOn(); setLCDEnable(bool);
BRIGHTNESSHELPER->setLuminance(); setContrast(); setSaturation(); setHue();
```

> 这两个是我**之前完全不知道**的（屏保那轮只用了 `EasyUI.cfg` 的 `defBrightness` 配置项）。
> 如果按老习惯直接写代码，我会去自己写 PWM sysfs 或再折腾一遍 ALSA —— 白费一天。

## 3. 设备实测（API 在 ≠ 硬件在）

| 项 | 结论 | 证据 |
|---|---|---|
| 背光**可读** | ✅ | `bright` → `亮度 cur=72 max=100 屏亮=1` |
| 背光**可调** | ✅ | `bright 40` → `setBrightness(40) -> 回读 40`（回读校验一致） |
| 蜂鸣器 | ✅ API 可用、无崩溃（**但不用它**） | `beep` → `beep() 已调用`；`beepfreq 2000 128` → 生效且进程存活（pid 不变）。作者定：**闹钟响铃走音频播放器**，蜂鸣器只留作诊断 |
| ⚠️ **蜂鸣器是否真的出声** | **待耳朵确认** | 软件侧无法判定（`beep` 不走 `/sys/class/pwm/pwmchip0`，那个设备 12 通道但无额外导出） |
| ⚠️ **亮度变化是否肉眼可见** | **待肉眼确认** | fb 像素数据**不受背光影响**（背光是 PWM 调光，不是改像素）⇒ 截图无法验证 |
| RTC | ❌ **没有** | 无 rtc 包、设备无 `/dev/rtc*`、无 `/sys/class/rtc` |
| 时区数据（tzdata） | ❌ **没有** | 设备无 `/usr/share/zoneinfo` ⇒ 世界时钟**必须内置时区表** |
| 标准背光节点 | ❌ 没有 | 无 `/sys/class/backlight`、`/sys/class/leds` ⇒ 背光走私有驱动（所以只能用 `BRIGHTNESSHELPER`） |

## 4. 时间相关的官方 API（校时/设时间用）

```cpp
// 读时间
struct tm *t = TimeHelper::getDateTime();
base::DateTime now = base::DateTime::now();

// 设时间（两种写法，DateDemo/DigitalClockDemo 实测）
TimeHelper::setDateTime(&tm);                      // tm 字段已填好
TimeHelper::setDateTime("2026-09-13 22:30:00");
base::setSystemDateTime(dt);

// 校时
ntp::syncTime(ntp::defaultServerList(), 5000);     // 我们已封装成 pg::TimeSync
```

> ⚠️ 本工程里 `TimeHelper::setDateTime` 设的是**本地时间**，而本地时间已由
> `PgTime::ensureTimezone()` 定成北京时间（`TZ=UTC-8`）——两者必须一致，否则手动设的时间会偏 8 小时。

## 5. 设计（据此定案）

| 子功能 | 实现 | 依赖 |
|---|---|---|
| **世界时钟** | 内置**固定偏移时区表**（约 20 个常用城市）；不做夏令时（设备无 tzdata，且目标市场无夏令时需求） | 纯计算 |
| **闹钟** | 多个闹钟存 `/data`（jffs2，重启不丢）；到点：**官方音频播放器**（`zk_audio_player`，见 `docs/audio-output.md`）循环播铃声 + `BRIGHTNESSHELPER->screenOn()` 亮屏 + 全屏提醒页 | `libzkmedia.a` + 背光 ⚠️ 响铃前要先 `PgAudio::releasePcm()` 让出 card0 |
| **闹钟的"全局"性** | 闹钟检查放**主界面（mainLogic）**而不是时钟页 —— 这样用户在玩游戏/看别的页时也会响 | 已有主循环 |
| **自动校时** | 复用 `pg::TimeSync`（已实现，`ntp` 包 + 内置 IP 列表） | 已有 |
| **手动设时间** | `TimeHelper::setDateTime(...)`；界面用我们自己的半屏键盘（IME）输入 | 已有 |
| **时间显示** | 时钟页内用 `digitalclock` 控件（⚠️ 它不支持居中，需要居中的地方用 textview 自刷）；屏保沿用已有的翻页钟 | 已有 |

## 6. 一键自检命令（已上机、可复用）

`/tmp/pg_autostart`：

| 命令 | 作用 |
|---|---|
| `beep` | 蜂鸣器短鸣 |
| `beepfreq <hz> <duty>` | 自定义 PWM 鸣叫（d=0~255，`0 0` = 停） |
| `bright` | 打印 `cur / max / 屏亮` |
| `bright <0-100>` | 设亮度（**带回读校验**） |
| `screen off` / `screen on` | 熄屏 / 亮屏 |
| `saver on` / `saver off` / `saver` | 屏保进出 / 查状态 |
| `zkwav [<相对路径>]` | **官方音频播放器**自检（`zk_wav_play`，后台线程）。测前先 `pcmfree` |
| `zkplay` / `zkstop` | 框架 `ZKMediaPlayer` 探针（**反例**：实测走 card1 = 静音） |
| `pcmfree` / `pcmopen` | 让出 / 拿回 `card0`（两个播放器互斥，测之前要让出） |

> ⚠️ **命令后面可以写 `#注释`**（QA 文件靠"内容必须变"去重，所以要加编号）——
> 分发入口已统一剥掉 `#` 之后的内容。**踩过的坑**：`bright #p1` 曾因参数解析被注释干扰而
> **静默什么都不做**（现象极像"功能没实现"）。

## 7. 本次踩的坑（排查过程比结论更值钱）

1. ⚠️ **QA 命令的行内注释会干扰参数解析** —— `strncmp` 只看前缀的命令（`saver off #x`）没事，
   但"后面必须结束"（`*arg == 0`）和 `sscanf("%d")` 那类会**静默失败**。
   ⇒ 已在 `runAutoCmd` 入口统一剥离注释，并加了一条"收到 '%s'"的诊断日志。
2. ⚠️ **`strings | grep` 查不了中文**（只提 ASCII）⇒ 查二进制里的中文字符串要用 `grep -a`。
3. ⚠️ **`logcat -d | head -N` 取的是最旧的行** —— 音频线程会刷屏，新日志在后面。
   查日志一律 `grep` 全量，不要用 head。
4. ⚠️ **`fun launch` 不一定重启已在跑的应用** —— 推了新 so 但进程还是旧的（pid 没变），
   现象就是"代码明明改了、二进制里也有新字符串，命令却不生效"。
   ⇒ 改 C++ 代码后，部署完**确认 pid 变化**（或直接 `killall zkgui` 让 init 拉起）。

---

## 8. 闹钟：引擎已实现（2026-09-13，真机验收通过）

> 本节对应代码：`src/platform/PgAlarm.{h,cpp}`（引擎）+ `src/platform/PgRingtone.{h,cpp}`（响铃）。
> 响铃走**官方音频播放器**（见 `docs/audio-output.md`），铃声 `resources/audio/alarm.wav`。

### 8.1 ★ 为什么用独立守护线程，而不是主界面定时器

**主界面的 `onUI_Timer` 只在主界面在前台时跑** —— 用户进 `wifi.ftu` / `remote.ftu` 这类
独立页面后就不跑了（同一个根因早就暴露过：主界面的 QA 文件 `/tmp/pg_autostart`
在独立页面上失效）。而闹钟必须**任何页面都会响** ⇒ `PgAlarm` 起一个 pthread 守护线程，
它不依赖任何 activity 的生命周期。

守护线程每 400ms 轮询一次（< 1s，不会漏掉分钟边界），用"年月日时分"压缩值去重，
**同一分钟只触发一次**。`mask` 语义：`0` = 仅一次（响完自动关闭并存盘）；`bit0..bit6` = 周日..周六
（`127` = 每天）。

### 8.2 到点动作（顺序固定）

1. **响铃**：`Ringtone::start("audio/alarm.wav")` —— 自动 `releasePcm()` 让出 `card0`
   （官方播放器与 PgAudio 常开流互斥），循环推帧；
2. **亮屏**：`BRIGHTNESSHELPER->screenOn()`（熄灭状态才会有效果）；
3. **自动关闭**：`mask == 0` 的闹钟响完即关并存盘（否则明天还会响）；
4. **通知 UI**：`setFireCb()` 注册的回调（当前未接 UI，见 §8.4）。

**响铃有 60 秒自动超时**（没人按停也不会响一整天）。

停铃有三条路径，任一皆可：**任意物理键**（`handleLogicalKey` 开头，响铃优先于一切操作）、
**触摸**（真实触摸与 QA 注入两条入口都判）、**QA `alarm stop`**。停铃后自动
`acquirePcm()` 把声卡还给音效系统。

### 8.3 验收记录（真机，日志判定）

| 项 | 证据 |
|---|---|
| 守护线程 | 到点日志 `PgAlarm: ★ 闹钟到点 #0 23:24 -> 响铃 + 亮屏` |
| 响铃走对声卡 | `PgRingtone: 让出 PgAudio 声卡 -> 1` → `zk_audio_player_init(1, 22050) -> 0` → `/proc/<pid>/fd` = **`pcmC0D0p`** |
| 循环 | `已循环 25 次（仍在响铃）`（35 秒后仍在） |
| 按键停铃 | `按键停铃（logical=0）` → `已停止响铃` → `PgAudio: 已拿回声卡 hw:0,0` |
| QA 停铃 | `alarm stop -> 仍在响=0（已停止响铃）` |
| 持久化 | 文件 `/data/pocketgame_alarms.txt`；`killall zkgui` 后重启仍 `alarm list -> 共 1 个` |
| 真实链路 | 设"下一分钟"的每日闹钟 → 到点自动响（不是只测 `alarm test`） |

### 8.4 未做（下一步）

- **闹钟提醒页**（全屏 UI，"停止/贪睡"按钮）—— 属 UI 改动，按规矩要先出预览稿确认；
  引擎侧已留好 `setFireCb()` 与 `stopRing()` 接口。
- **贪睡（snooze）**：暂未做（需要提醒页上的按钮）。
- **时钟套件页**（世界时钟 + 闹钟列表/编辑）—— 同样等 UI 确认。

### 8.5 QA 命令（`/tmp/pg_autostart`）

| 命令 | 作用 |
|---|---|
| `alarm list` | 列出（序号/开关/时间/mask）+ 打印存储路径 |
| `alarm add <hh> <mm> [mask]` | 新增（mask 省略 = 仅一次；127 = 每天） |
| `alarm on\|off\|del <序号>` / `alarm clear` | 开关 / 删除 / 清空 |
| `alarm in <秒>` | N 秒后响一次（**验收用，不落盘**） |
| `alarm test` | 立刻响一次（不落盘） |
| `alarm stop` | 停铃并归还声卡 |
| `ring` / `ring off` / `ring <文件>` | 直接控制铃声（循环播放器自检，不经闹钟） |

---

## 9. 时钟套件界面 + 响铃提醒页（2026-09-13 完成，真机验收通过）

> 用户 2026-09-13 定：**这部分 UI 不需要审核**（直接做）。
> 代码：`ui/main.html`（三个窗口）+ `src/logic/mainLogic.cc`（同步与回调）+ `PgGames.{h,cpp}`（注册 slot 19）。

### 9.1 三个窗口（都用普通 `window`，不用 modal）

| 窗口 | 作用 | 布局要点 |
|---|---|---|
| `WinClockSuite` | 主页 | 色条 / 标题 / **当前大钟（fs=64）** / 日期+星期 / **闹钟 5 行** / 编辑区 / 操作区 / 底栏 |
| `WinWorld` | 世界时钟 | **10 个城市**（左城市名 fs=18、右当地时间 fs=28）+ 返回；**固定偏移时区表**，不做夏令时（本板无 tzdata） |
| `WinAlarmRing` | 响铃提醒 | 琥珀条 / 标签 / **96px 大字** / 日期 / 「停止」（红）/「贪睡 5 分钟」/ 提示 |

闹钟行 = 4 个控件：**时间（点击=载入编辑）** + 重复说明 + 开关（绿=开/灰=关）+ 删除。
空行显示暗色 `--:--` 且**开关/删除自动隐藏**（`setVisible`）—— 一眼看出哪几行是空的。

### 9.2 为什么单独一套同步（不复用 `syncToolUi` 的 8 键/20 键模板）

时钟套件的按钮是 **27 个**（5 行 × 3 + 编辑区 6 + 操作 2 + 世界页 1 + 提醒页 2…），
远超"8 键/20 键"模板的表达能力 ⇒ 走 `nativePage() == 3` 的**专门分支**：
`syncToolUi()` 一进来就 `if (page == 3) { syncClockSuite(); syncWorldClock(); return; }`。

所有文本都**带变化检测**（`setToolText` 的 cache），且主页同步有"**一秒只做一次**"的闸
（`gSuiteLastSec`）—— 每帧无条件 `setText` 会触发重绘风暴（硬规则）。

### 9.3 ⚠️ 跨线程：守护线程只置标志

闹钟到点的回调来自**守护线程**（`PgAlarm` 的 pthread）。回调 `onAlarmFireFromGuard()`
**只置 `gRingPending` 标志**，真正的"填控件 + `showWnd`"放在主循环的 `tickAlarmRing()` 里
—— 跨线程操作 FlyThings 控件不安全。

`tickAlarmRing()` 放在 `TIMER_LOOP` 里（**任何模式都判**），所以玩游戏时到点也会弹提醒页。

### 9.4 真机验收（日志 + 数值化读屏）

| 项 | 证据 |
|---|---|
| 应用注册 | `enter tool[19] 时钟套件 (native page 3)`（主界面第 20 个卡片，图标 slot 19） |
| 主页渲染 | 色条 `#F2B33D` / 标题栏 `#16202C` / 大钟白字 / 日期 / **5 行 `#26313F` 按钮** / 编辑区 / 保存+新建+世界时钟 / 底栏，逐层对上 |
| 新建 | `suite new` → 编辑态 `-1`，起点 = **当前时间 +1 分钟**（23:37） |
| 调时/调分 | `suite min 1` ×3 → `23:40` |
| 重复循环 | `suite rep` → mask `127 → 62 → 65`（每天 → 工作日 → 周末） |
| 保存 | `PgAlarm: 新增 23:40 mask=65（共 1 个）`，编辑态回 `-2`，提示"已保存 23:40（周末）" |
| 列表渲染 | 行0：时间墨迹 1098 + 重复「周末」+ **开关色 `(46,125,91)` 绿** + 删除按钮；行1~4：暗色 `--:--` 且开关/删除墨迹 = 0（已隐藏） |
| 世界时钟页 | 色条 `#4A90D9`，**10 行**左右都有墨迹（城市 60~109 / 时间 106~121），返回按钮在位 |
| 提醒页弹出 | `闹钟提醒页已弹出（闹钟（手动测试））`；`ring state 响铃中=1 提醒页=1` |
| 提醒页渲染 | 琥珀条 / 标签墨迹 322 / **大字区墨迹 1147** / 日期 181 / **停止钮底 `(228,87,76)` 红** / 贪睡钮 `#26313F` |
| 贪睡 | `贪睡 5 分钟` → `响铃中=0 提醒页=0`（收起） |
| 按键停铃（在套件页） | `按键停铃（logical=0）` → `已停止响铃` → `PgAudio: 已拿回声卡 hw:0,0` |

### 9.5 QA 命令（新增，均需先进入时钟套件页）

| 命令 | 作用 |
|---|---|
| `suite new` / `ok` / `cancel` / `rep` | 新建 / 保存 / 取消 / 切换重复 |
| `suite hr -1\|1` / `min -1\|1` | 调时 / 调分 |
| `suite edit <i>` / `sw <i>` / `del <i>` | 编辑第 i 行 / 开关 / 删除 |
| `suite world` / `back` | 进 / 出世界时钟页 |
| `suite state` | 打印编辑态与闹钟数（自检） |
| `ring stop` / `snooze` / `state` | 等价于点提醒页的「停止」/「贪睡」；查提醒页状态 |
| `saver to <秒>` | **临时延长屏保超时**（自动化验收必备：QA 操作不算"用户活动"，30 秒必被屏保打断） |

### 9.6 踩的坑

1. ⚠️ **截图要按 `fb0/pan` 的当前值传 `offset_y`**：设备双缓冲 + 静止时不重绘，
   传错 offset 会读到**上一屏**（这次先读到套件主页、改传 0 才拿到世界时钟页）。
   做法：先 `cat /sys/class/graphics/fb0/pan`，再按它抓。
2. ⚠️ **`isShow()` 是 SysApp（BaseApp）的**，窗口类要用 **`isWndShow()`**（编译期才发现）。
3. ⚠️ **函数声明的作用域**：QA 命令写在**匿名 namespace 内**，而回调实现写在 namespace **外**
   ⇒ 若把前置声明也放进匿名 namespace，会出现 `(anonymous)::f` 与 `::f` 两个候选，
   报 `call of overloaded 'f(int)' is ambiguous`。**声明必须放在全局作用域**。
4. ⚠️ **屏保会打断自动化验收**：QA 命令是程序化的，框架空闲检测**不认**它（只有真触摸/按键算活动），
   所以 30 秒一到必然进屏保 ⇒ 加了 `saver to <秒>` 专治这个。

---

## 拆页记录：时钟套件已独立成 `clocksuite.ftu`（2026-09-14）

原来是主界面里的整屏窗口（`WinClockSuite` + `WinWorld`），现按拆分口径
（「游戏不动，其余功能界面全部独立成 ftu」）搬成独立页：

| 项 | 内容 |
|---|---|
| 布局 | `ui/clocksuite.html`（root `div.screen` + 两个 window：`WinClockSuite` 默认显示 / `WinWorld` 初始隐藏） |
| 逻辑 | `src/logic/clocksuiteLogic.cc`（约 750 行；原来的 `syncClockSuite/syncWorldClock/suite*/maskText/…` 全搬过来） |
| 路由 | `mainLogic.cc` 的 `kPageApps` 表加 `{"clocksuite","clocksuiteActivity"}`；`kAppTable` 的 slot 19 **不动**（存档索引） |
| 控件 | caption 一个都没改（`TextSuiteTime` / `BtnAlTime0..4` / `BtnHrUp` …），所以逻辑搬运近乎零改名 |
| 按键 | 108 短按 = 世界钟页返回套件主页（主页内无动作）；**108 长按 ≥700ms = 返回应用列表**；103/105 = 音量± |
| 自检通道 | **`/tmp/pg_clocksuitecmd`**（独立页必须有自己的通道 —— 后台 Activity 的定时器不跑，`/tmp/pg_autostart` 在这里失效）：`suite new\|ok\|cancel\|rep\|hr <d>\|min <d>\|edit <i>\|sw <i>\|del <i>\|world\|back\|state`、`dump` |
| 屏保 | 进页关、退出恢复（独立页必须自己管，主界面不在前台时它的策略不跑） |

### ⚠️ 唯一一个"故意没搬"的东西：响铃提醒页 `WinAlarmRing`

闹钟是**跨页面**的（`pg::Alarm` 守护线程在任何页面都会响），提醒页由**主循环**
`tickAlarmRing()` 弹出 —— 它属于"整屏浮层"（与音量 OSD 同类）。把它塞进某一个应用页，
就会出现"在别的应用里响铃时看不到提醒"。所以：

- `WinAlarmRing` **留在 `main.ftu`**；
- 本页在**响铃期间自动收掉自己**（`onUI_Timer` 里判 `pg::Alarm::instance().ringing()` →
  `closeActivity`），回到主界面后由主循环把提醒页弹出来。
- 这**顺带补上了老版本的一个空档**：以前在 wifi / 工具页里响铃**只有声音、没有界面**，
  用户找不到"停止"（只能等 60s 自动停）。

验证（2026-09-14 真机）：
- `alarm test`（主界面）→ `闹钟提醒页已弹出（闹钟（手动测试））` ✅
- 独立页打开时到点：`时钟套件: 响铃中 -> 让位（回主界面弹提醒页）` → `onUI_show (mode=0)`
  → `闹钟提醒页已弹出（闹钟 18:18）` ✅（抓屏确认提醒页可见）
