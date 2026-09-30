# 硬件基准工程（板级事实的唯一来源）

> ## ⚠️ 硬规则
> **本工程的硬件与下面这个工程是同一套硬件。凡涉及硬件（GPIO / ADC / 充电 / 背光 /
> 显示屏 / 按键 / 电源）的任何疑问，一律以该工程的代码为准 —— 不要自己臆测、不要自己造。**
>
> ```text
> S:/projects/LearningProject/V851ExtendedScreen_ap_p2p
> ```
>
> - 它是 ZKSWE 给的**官方参考工程**（手机副屏 / 投屏盒子），跑在**同一块 V851s 板子**上；
> - 板级参数（分压电阻、电压区间、GPIO 号、ADC 通道）**只在那里有**，内核 DT 里查不到；
> - 遇到"某个 GPIO 是几号""某个 ADC 通道对应什么""某个引脚高有效还是低有效"这类问题，
>   **先去那个工程搜，搜不到再找人确认，不要自己试出来就写进代码**。

## 1. 为什么确定是同一套硬件（实测证据）

在本板（PocketGame 目标机）上逐项核对，与参考工程用到的节点一一对上：

| 参考工程用到的 | 本板实测 | 结论 |
|---|---|---|
| `POWER_OFF_NODE = /sys/devices/platform/soc/soc@03000000:sysKeys/sys5v` | **该节点存在**，读回 `1` | ✅ 同款 `zkswe,syskeys` 驱动 |
| `AdcHelper` → `/sys/class/gpadc/{adc_ch,adc_enable,adc_val}` | **三个节点都在**，ch0 读回 `4072` | ✅ 同款 `sunxi-gpadc` |
| `GpioHelper` → `"GPIO_<N>"` → `/sys/class/gpio/gpio<N>` | `/sys/class/gpio/` 机制一致；`GPIO_135`/`GPIO_5` 均可读 | ✅ 同一 GPIO 命名法 |
| `libzkhardware.so`（`AdcHelper`/`GpioHelper` 的实现所在） | `/lib/libzkhardware.so` **已在本板**（38368 B），导出符号一致 | ✅ 同一个 SDK 包 |

本板 `soc@03000000` 下的 zkswe 私有节点（可用来认板子）：
`sysKeys`（电源/按键）、`gpio-keys`、`lcd0@05461000`、`codec_mach`、`netRF`、
`rt-media@01c0e000`、`daudio0_mach`。

## 2. 参考工程里"硬件相关"的代码索引

| 想查什么 | 去这个文件 |
|---|---|
| **电池电压 / 电量 / 充电状态 / 低电 LED / 关机 / 按键** | `src/system/hardware.cpp` + `src/system/hardware.h` |
| 显示屏图层（disp 层开关、`/dev/disp` + `DISP_LAYER_GET/SET_CONFIG`） | `src/system/hardware.cpp` 的 `_release_layer()` |
| GPIO / ADC 的**实现**（引脚名怎么解析、走哪个节点） | `libzkhardware.so`（`AdcHelper` / `GpioHelper`）；头文件在 SDK 包 `zkhardware` 的 `include/utils/` 下 |
| 音频（声卡、通道、音量） | `src/system/`、`resources/alsa/` |
| 客户/OEM 差异（不同板子的小差别） | `doc/oem.ini`、`doc/oem_make.ini`、`doc/oem_zhiying.ini`、`src/system/setting.cpp` |
| 蓝牙（RTL8733BS 踩坑） | `doc/蓝牙踩坑记录-RTL8733BS-V851s.md` |

## 3. 从参考工程抄下来的板级事实（**照抄，不要改**）

### 3.1 电池（`src/system/hardware.cpp`）

```cpp
#define CHARGED_GPIO  "GPIO_135"   // 充满（高有效）
#define CHARGING_GPIO "GPIO_5"     // 充电中（高有效）
#define LED_GPIO      "GPIO_136"   // 低电指示 LED
#define KEY4_GPIO     "GPIO_3"
```

```cpp
static const float ADC_REF_VOL    = 1800;  // ADC 参考电压 1.8V
static const float ADC_DIVIDER_R1 = 200.0; // 上分压电阻 200Ω
static const float ADC_DIVIDER_R2 = 150.0; // 下分压电阻 150Ω（采样点）
static const float BATTERY_V_MAX  = 4150;  // 满电电压 mV
static const float BATTERY_V_MIN  = 3380;  // 没电电压 mV

// 电压：value 是 12bit ADC 值（0..4096）
voltage = value / 4096.0 * ADC_REF_VOL * (ADC_DIVIDER_R2 + ADC_DIVIDER_R1) / ADC_DIVIDER_R2;

// 电量百分比
voltage_ratio = (voltage - BATTERY_V_MIN) / (BATTERY_V_MAX - BATTERY_V_MIN);
percent = clamp(voltage_ratio, 0, 1) * 100;   // 原文用 (voltage_ratio + 0.005) * 100 取整
```

- **ADC 通道 = `0`**（`AdcHelper::setChannel(0)`、`AdcHelper::setEnable(true)`、`AdcHelper::getVal()`）；
- **滤波 = 累计 5 次采样取平均**，而且**每 30 秒才更新一次**电量（`timer.elapsed() < 30*1000` 直接返回）；
- 充电状态判定顺序：`charged==1` → 充满；`charging==1` → 充电中；否则 → 未充电；
- **低电阈值 `BATTERY_LOW = 20`（%）**，客户模式（`CUSTOMER_MAKE`）下是 `9`；
- 低电且有电时才动 LED：`GpioHelper::output(LED_GPIO, blink)` 与 `GpioHelper::input(LED_GPIO)`
  （后者是"设为输入模式、悬空"＝关灯）；检测线程 `DELAY(50)` 一轮、每 10 轮（≈500ms）做一次电池检测。

### 3.2 电源 / 关机

```cpp
#define POWER_OFF_NODE "/sys/devices/platform/soc/soc@03000000:sysKeys/sys5v"

bool get_power_off_support() { return base::exists(POWER_OFF_NODE); }   // 节点在 = 支持
void power_off() { base::writeFile(POWER_OFF_NODE, "0", 1); }           // 写 "0" 即断电
```

- `sys5v` **可读可写**：读回 `1` 表示系统 5V 自锁供电在；写 `0` = 切断自锁 = **关机**；
- 顺带：它**不是**充电检测（充电检测用 `GPIO_135/GPIO_5`），别拿它当"插着充电器"。

### 3.3 GPIO / ADC 的底层走法（`libzkhardware.so`）

| 接口 | 底层 |
|---|---|
| `GpioHelper::input("GPIO_<N>")` | `/sys/class/gpio/export` → `gpio<N>/direction=in` → 读 `gpio<N>/value` |
| `GpioHelper::output("GPIO_<N>", v)` | 同上，`direction=out` → 写 `gpio<N>/value` |
| `AdcHelper::setChannel(ch)` | 写 `/sys/class/gpadc/adc_ch` |
| `AdcHelper::setEnable(b)` | 写 `/sys/class/gpadc/adc_enable` |
| `AdcHelper::getVal()` | 读 `/sys/class/gpadc/adc_val`（12bit：0~4096） |

⇒ 二者都**已经在本工程的 Manifest 依赖里**（`zkhardware` 包），
所以直接 `#include "utils/AdcHelper.h"` / `#include "utils/GpioHelper.h"` 即可，
**不需要**自己写 sysfs 读写代码。

## 4. 使用姿势（写代码时）

1. 先在本页 §2 的表里找到对应文件，去 `S:/projects/LearningProject/V851ExtendedScreen_ap_p2p` 读原码；
2. **常量、GPIO 号、公式、判定顺序一律照抄**；确实需要偏离时，把"为什么偏离 + 实测依据"写进注释；
3. 本板特有的实测结论（比如"这个脚在本板读回 1"）写进本工程 `docs/`，不要污染对参考工程的理解。

---

## 附：本工程已按此实现的模块

| 模块 | 文件 | 依据 |
|---|---|---|
| 电池电量 / 充电状态 | `src/platform/PgBattery.{h,cpp}` | 本文 §3.1 |
