/*
 * PgBattery.h - 电池电量 / 充电状态
 *
 * ⚠️ 板级参数（GPIO 号、ADC 通道、分压电阻、电压区间）**全部照抄官方参考工程**：
 *      S:/projects/LearningProject/V851ExtendedScreen_ap_p2p
 *      → src/system/hardware.cpp（本工程 docs/hardware-reference.md §3.1 有摘录）
 *    本工程与它是**同一套硬件**（同一颗 V851s 板、同一个 zkswe syskeys 驱动、
 *    同一个 sunxi-gpadc、同一个 libzkhardware.so），所以常量不要改。
 *
 * 数据来源（都是 zkhardware 包提供的接口，Manifest 里已有该依赖）：
 *   · 电量/电压：AdcHelper（底层 /sys/class/gpadc/{adc_ch,adc_enable,adc_val}），**通道 0**
 *   · 充电状态：GpioHelper（底层 /sys/class/gpio/gpio<N>）
 *       CHARGED  = "GPIO_135"（高有效 → 已充满）
 *       CHARGING = "GPIO_5"  （高有效 → 充电中）
 *
 * 刷新节奏也照抄：**约每 30 秒**更新一次电量，且取 **5 次采样的平均**
 *   （ADC 有噪声，单次抖动会让电量百分比跳变）；充电状态约每 500ms 查一次。
 *   启动时先立刻读一次，避免开机前 30 秒没有值。
 *
 * 用法：mainLogic 的 TIMER_LOOP 里调 tick()（幂等 + 内部节流，随便调）。
 */
#ifndef PG_BATTERY_H_
#define PG_BATTERY_H_

namespace pg {

/* 充电状态 —— 与参考工程 hardware.h 的 E_CHARGE_STATE 一一对应，序号也一致 */
enum ChargeState {
  CHARGE_NONE = 0,      /* 参考 E_CHARGE_NO_ACTIVE：没在充电 */
  CHARGE_ACTIVE = 1,    /* 参考 E_CHARGING       ：充电中 */
  CHARGE_FULL = 2,      /* 参考 E_FULL_CHARGED   ：已充满 */
  CHARGE_ABNORMAL = 3,  /* 参考 E_CHARGEE_ABNORMAL：异常（参考工程也没实现判定，保留占位） */
};

class Battery {
 public:
  /* 幂等：初始化 ADC（设通道 + 使能）并立刻读一次，开机就有值 */
  static void init();

  /* 主循环调用：内部按 500ms 节流（照参考工程"每 10 轮×50ms 检测一次"的节奏） */
  static void tick();

  /* 当前电量 %（0~100）；一次都没读到时返回 -1 */
  static int percent();

  /* 电池电压 mV；读不到返回 -1 */
  static int voltageMv();

  /* 最近一次原始 ADC 值（0~4096）；读不到返回 -1。现场标定用 */
  static int rawAdc();

  static ChargeState state();
  static const char *stateText();       /* 中文短文案，直接给 UI 用 */
  static bool charging();               /* 充电中或已充满，都算"接着电" */
  static bool low();                    /* 低电（阈值见 kLowPercent） */

  /* QA：立刻重新采样并发布（跳过 30s 节流；滤波仍取平均但只采 1 次） */
  static void refresh();

  /* QA `battraw`：直接读一次 ADC（不动全局状态），用于现场标定分压/通道 */
  static int readAdcOnce();

  /** ⚠️ **仅供自检**：伪造电量/充电状态，用来在真机上验收界面图标的各个状态
   *  （设备常年插着 USB 时没法等到低电/未充电，只能伪造）。
   *  `pct` < 0 = 清除伪造，回到真实值。**只影响对外读数，不影响真实 ADC 采样与日志。** */
  static void setFake(int pct, int state);
  static bool fakeActive();
};

}  // namespace pg

#endif  // PG_BATTERY_H_
