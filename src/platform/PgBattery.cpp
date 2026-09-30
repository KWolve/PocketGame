/*
 * PgBattery.cpp - 电池电量 / 充电状态实现，见 PgBattery.h 的说明。
 *
 * 本文件是**照抄**官方参考工程 src/system/hardware.cpp 的逻辑：
 *   · 同一个 ADC 通道（0）、同一组分压电阻（R1=200 / R2=150）、同一段电压区间（3380~4150mV）
 *   · 同样的滤波与节奏（5 次采样取平均 + 约 30 秒更新一次电量、500ms 查一次充电状态）
 *   · 同样的充电状态判定顺序（先判"充满"，再判"充电中"）
 * 只把参考工程里"回调通知 + 线程"的写法换成本工程的"主循环 tick"写法，
 * 数值与判定顺序一个字不改（改了就偏离板级事实了）。
 */
#include "platform/PgBattery.h"

#ifdef FUN_BUILD

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "utils/Log.h"
#include "utils/AdcHelper.h"
#include "utils/GpioHelper.h"

namespace pg {

namespace {

/* ===== 板级常量：与参考工程 src/system/hardware.cpp 完全一致（不要改） ===== */
const float kAdcRefVol = 1800.0f;      /* ADC 参考电压 1.8V */
const float kDividerR1 = 200.0f;       /* 上分压电阻 200Ω */
const float kDividerR2 = 150.0f;       /* 下分压电阻 150Ω（ADC 采样点） */
const float kBatteryVMax = 4150.0f;    /* 电池满电电压 mV */
const float kBatteryVMin = 3380.0f;    /* 电池没电电压 mV */
const int kAdcFullScale = 4096;        /* 12bit */

const char *kChargedGpio = "GPIO_135";   /* 高有效 = 已充满 */
const char *kChargingGpio = "GPIO_5";    /* 高有效 = 充电中 */

const int kLowPercent = 20;              /* BATTERY_LOW（客户模式为 9，本工程用默认值） */
const int kSamplesPerPublish = 5;        /* 参考工程：累计 5 次采样后取平均 */
const long long kLevelPeriodMs = 30000;  /* 参考工程：电量至少隔 30 秒才更新一次 */
const long long kTickPeriodMs = 500;     /* 参考工程：每 10 轮 × 50ms ≈ 500ms 检测一次 */

/* ===== 状态 ===== */
volatile int sInited = 0;
volatile int sPercent = -1;     /* -1 = 还没测到 */
volatile int sVoltageMv = -1;
volatile int sRawAdc = -1;
volatile int sState = CHARGE_NONE;
/* 自检用伪造值（QA battfake；-1 = 关） */
volatile int sFakePct = -1;
volatile int sFakeState = -1;

long long sNextTickMs = 0;
long long sNextLevelMs = 0;     /* 到点才开始收下一组 5 个采样 */
int sSum = 0;
int sCount = 0;
int sPublishCount = -1;         /* -1 = 还没发布过 */

long long monoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ADC 原始值 → 电池电压（mV）。公式照参考工程 _calculate_voltage()：
 *   voltage = value / 4096 * 参考电压 * (R2 + R1) / R2 */
float voltageFromAdc(int value) {
  return (float)value / (float)kAdcFullScale * kAdcRefVol * (kDividerR2 + kDividerR1) / kDividerR2;
}

/* 电池电压 → 电量（%）。逻辑照参考工程 _calculate_battery()：线性映射并钳制到 0~100，
 * 最后 +0.005 再取整（原文如此，作用是四舍五入到整数百分比）。 */
int percentFromVoltage(float voltage) {
  float ratio = (voltage - kBatteryVMin) / (kBatteryVMax - kBatteryVMin);
  if (ratio < 0.0f) return 0;
  if (ratio > 1.0f) return 100;
  return (int)((ratio + 0.005f) * 100.0f);
}

/* 充电状态。判定顺序照参考工程 _det_charge_state()：先"充满"，再"充电中"。 */
int detectChargeState() {
  int charged = GpioHelper::input(kChargedGpio);
  int charging = GpioHelper::input(kChargingGpio);
  if (charged == 1) return CHARGE_FULL;
  if (charging == 1) return CHARGE_ACTIVE;
  return CHARGE_NONE;
}

void publishLevel() {
  if (sCount <= 0) return;
  int avg = sSum / sCount;
  float v = voltageFromAdc(avg);
  int pct = percentFromVoltage(v);
  sRawAdc = avg;
  sVoltageMv = (int)(v + 0.5f);
  sPercent = pct;
  sSum = 0;
  sCount = 0;
  long long now = monoMs();
  sNextLevelMs = now + kLevelPeriodMs;   /* 下一组采样至少 30 秒后再开始（照参考工程） */
  LOGD("PgBattery: 电量 %d%%（电压 %dmV，ADC 平均 %d，第 %d 次发布）", pct, sVoltageMv, avg,
       ++sPublishCount);
}

}  // namespace

void Battery::init() {
  if (sInited) return;
  sInited = 1;
  /* 照参考工程：设通道 0 并使能（底层写 /sys/class/gpadc/adc_ch、adc_enable） */
  AdcHelper::setChannel(0);
  AdcHelper::setEnable(true);
  /* 参考工程在检测线程启动时**立刻读一次**并发布，这样开机就有值。
   * 这里同样处理：单次采样直接发布（sCount 会是 1，平均即本身）。 */
  int first = AdcHelper::getVal();
  if (first > 0) {
    sSum = first;
    sCount = 1;
    publishLevel();
    sNextLevelMs = monoMs() + kLevelPeriodMs;
  } else {
    LOGW("PgBattery: 开机首次读 ADC 失败（%d），等下一轮", first);
    sNextLevelMs = monoMs();
  }
  sState = detectChargeState();
  LOGD("PgBattery: 初始化完成 —— 充电状态 %s（GPIO_135=%d GPIO_5=%d）", stateText(), 
       GpioHelper::input(kChargedGpio), GpioHelper::input(kChargingGpio));
}

void Battery::tick() {
  if (!sInited) init();
  long long now = monoMs();
  if (now < sNextTickMs) return;
  sNextTickMs = now + kTickPeriodMs;

  /* ---- 电量（约 30 秒一组、每组 5 次采样取平均；照参考工程 _detcet_battery_level）---- */
  if (now >= sNextLevelMs) {
    int value = AdcHelper::getVal();
    if (value > 0) {
      sSum += value;
      ++sCount;
      if (sCount >= kSamplesPerPublish) publishLevel();
    } else if (sPublishCount < 0) {
      /* 一次都没成功过：把下一次采样提前一点再试（否则要再等 30 秒） */
      sNextLevelMs = now + 1000;
    }
  }

  /* ---- 充电状态（每 500ms 查一次，变了才打日志；照参考工程）---- */
  int st = detectChargeState();
  if (st != sState) {
    LOGD("PgBattery: 充电状态 %s → %s", stateText(), 
         st == CHARGE_FULL ? "已充满" : (st == CHARGE_ACTIVE ? "充电中" : "未充电"));
    sState = st;
  }
}

int Battery::percent() { return sFakePct >= 0 ? sFakePct : sPercent; }
int Battery::voltageMv() { return sVoltageMv; }
int Battery::rawAdc() { return sRawAdc; }
ChargeState Battery::state() { return (ChargeState)(sFakeState >= 0 ? sFakeState : sState); }

const char *Battery::stateText() {
  switch (state()) {
    case CHARGE_FULL: return "已充满";
    case CHARGE_ACTIVE: return "充电中";
    case CHARGE_ABNORMAL: return "充电异常";
    default: return "未充电";
  }
}

bool Battery::charging() {
  ChargeState st = state();
  return st == CHARGE_ACTIVE || st == CHARGE_FULL;
}

bool Battery::low() {
  int p = percent();
  return p >= 0 && p <= kLowPercent;
}

void Battery::setFake(int pct, int state) {
  sFakePct = (pct < 0 || pct > 100) ? -1 : pct;
  sFakeState = (state < 0 || state > CHARGE_ABNORMAL) ? -1 : state;
  LOGD("PgBattery: 伪造读数 %s（电量 %d%% 状态 %d）—— 仅自检用，真实采样未受影响",
       fakeActive() ? "开启" : "清除", sFakePct, sFakeState);
}

bool Battery::fakeActive() { return sFakePct >= 0 || sFakeState >= 0; }

void Battery::refresh() {
  if (!sInited) init();
  int value = AdcHelper::getVal();
  if (value > 0) {
    sSum = value;
    sCount = 1;
    publishLevel();
  }
  sState = detectChargeState();
  LOGD("PgBattery: 手动刷新 —— 电量 %d%% 电压 %dmV ADC %d 状态 %s", sPercent, sVoltageMv, sRawAdc,
       stateText());
}

int Battery::readAdcOnce() {
  if (!sInited) init();
  return AdcHelper::getVal();   /* 直接读，不动全局状态 */
}

}  // namespace pg

#endif  // FUN_BUILD
