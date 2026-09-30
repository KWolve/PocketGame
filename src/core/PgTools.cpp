/*
 * PgTools.cpp - 四个工具应用：番茄时钟 / 定时器 / 秒表 / 计算器
 *
 * 与游戏共用 pg::Game 抽象，但**渲染方式不同**：这些是"界面型"应用，用 FlyThings
 * 原生控件（ZKTextView/ZKButton）渲染 —— 矢量字体任意字号都清晰、按钮有原生按下态、
 * 布局调整只需要改 ui/main.html 再重新生成。
 *
 * 本文件因此只有三部分：
 *   1) 状态机（倒计时 / 计时 / 表达式求值）
 *   2) 原生 UI 描述（uiText 文本槽 / uiButton 按钮标签 / uiButtonStyle 配色 / uiAccent 强调色）
 *   3) 交互入口（onUiButton 原生按钮、onKey 物理键）
 * 一行画布代码都没有 —— 绘制由 logic 层同步到控件完成。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

void fmtMmSs(char *b, int n, int ms) {
  if (ms < 0) ms = 0;
  int total = ms / 1000;
  snprintf(b, n, "%02d:%02d", total / 60, total % 60);
}

// 数字格式化：去掉多余的 0，太大/太小用科学计数（错误值给中文提示）
void fmtNum(char *out, int n, double v) {
  if (isnan(v) || isinf(v)) {
    snprintf(out, n, "错误");
    return;
  }
  double av = fabs(v);
  if (av != 0 && (av >= 1e12 || av < 1e-6)) {
    snprintf(out, n, "%.6g", v);
    return;
  }
  snprintf(out, n, "%.10g", v);
}

}  // namespace

/* ==================================================================
 * 番茄时钟
 * ================================================================== */

ToolPomodoro::ToolPomodoro()
    : workMin_(25),
      breakMin_(5),
      remainMs_(25 * 60 * 1000),
      phase_(0),
      done_(0),
      running_(false),
      overFlag_(false),
      flashMs_(0),
      lastBeepMs_(0) {}

const char *ToolPomodoro::title() const { return "番茄时钟"; }
const char *ToolPomodoro::desc() const { return "工作 25 分 / 休息 5 分循环"; }
const char *ToolPomodoro::tag() const { return "TOMATO"; }
Color ToolPomodoro::theme() const { return rgba(226, 92, 82); }

int ToolPomodoro::phaseTotalMs() const {
  return (phase_ == 0 ? workMin_ : breakMin_) * 60 * 1000;
}

void ToolPomodoro::reset() {
  phase_ = 0;
  done_ = 0;
  running_ = false;
  remainMs_ = phaseTotalMs();
  flashMs_ = 0;
  lastBeepMs_ = 0;
}

void ToolPomodoro::nextPhase(bool countDone) {
  if (phase_ == 0) {
    if (countDone) ++done_;
    phase_ = 1;
  } else {
    phase_ = 0;
  }
  remainMs_ = phaseTotalMs();
  flashMs_ = 1200;  // 状态行短时提示"阶段切换"
  sfx(SFX_OVER);
}

void ToolPomodoro::update(int dtMs) {
  if (flashMs_ > 0) flashMs_ -= dtMs;
  if (!running_) return;
  remainMs_ -= dtMs;
  if (remainMs_ <= 0) nextPhase(true);
}

bool ToolPomodoro::onKey(int key) {
  if (key == PG_KEY_A) {
    running_ = !running_;
    sfx(SFX_CLICK);
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *ToolPomodoro::hint() const {
  if (running_) return (phase_ == 0) ? "专注中 · 按暂停键" : "休息中 · 按暂停键";
  return "按暂停键或点「开始」进入专注";
}

const char *ToolPomodoro::keyBar() const {
  return "暂停键 开始/暂停 · 长按 返回列表";
}

bool ToolPomodoro::justGameOver() { return overFlag_; }
void ToolPomodoro::clearGameOverFlag() { overFlag_ = false; }

/* ---------------- 原生 UI 描述 ---------------- */

const char *ToolPomodoro::uiText(int slot, char *buf, int n) const {
  switch (slot) {
    case 0: {  // 主大字：剩余时间
      int left = remainMs_ < 0 ? 0 : remainMs_;
      snprintf(buf, n, "%02d:%02d", left / 60000, (left / 1000) % 60);
      return buf;
    }
    case 1: {  // 状态行
      if (flashMs_ > 0) return "阶段切换 · 响铃提醒";
      if (running_) return (phase_ == 0) ? "专注中" : "休息中";
      return (phase_ == 0) ? "专注 · 已暂停" : "休息 · 已暂停";
    }
    case 2: {  // 副行：配置摘要 + 完成数
      snprintf(buf, n, "工作 %d 分 · 休息 %d 分 · 已完成 %d 个", workMin_,
               breakMin_, done_);
      return buf;
    }
    default:
      return "";
  }
}

const char *ToolPomodoro::uiButton(int i) const {
  switch (i) {
    case 0: return running_ ? "暂停" : "开始";
    case 1: return "跳过";
    case 2: return "重置";
    case 3: return "工作 -1";
    case 4: return "工作 +1";
    case 5: return "休息 -1";
    case 6: return "休息 +1";
    default: return "";
  }
}

int ToolPomodoro::uiButtonStyle(int i) const {
  if (i == 0) return running_ ? 2 : 1;  // 开始=主(绿)，暂停=次(暗)
  return 0;
}

bool ToolPomodoro::onUiButton(int i) {
  switch (i) {
    case 0:
      running_ = !running_;
      sfx(SFX_CLICK);
      return true;
    case 1:
      nextPhase(false);
      return true;
    case 2:
      reset();
      sfx(SFX_CLICK);
      return true;
    case 3:
    case 4:
    case 5:
    case 6: {
      int delta = (i == 3 || i == 5) ? -1 : 1;
      if (i <= 4) {
        workMin_ += delta;
        if (workMin_ < 1) workMin_ = 1;
        if (workMin_ > 90) workMin_ = 90;
      } else {
        breakMin_ += delta;
        if (breakMin_ < 1) breakMin_ = 1;
        if (breakMin_ > 60) breakMin_ = 60;
      }
      // 时长变化要立刻反映到当前阶段（否则按了没反应）
      if (phase_ == (i <= 4 ? 0 : 1)) {
        int t = phaseTotalMs();
        if (!running_ || remainMs_ > t) remainMs_ = t;
      }
      sfx(SFX_MOVE);
      return true;
    }
    default:
      return false;
  }
}

uint32_t ToolPomodoro::uiAccent() const {
  // ★ 2026-09-14 UI 改版：换成 iOS 深色令牌（DANGER / SUCCESS）
  return (phase_ == 0) ? 0xFFFF453Au : 0xFF30D158u;  // 专注红 / 休息绿
}

/* ==================================================================
 * 定时器
 * ================================================================== */

ToolTimer::ToolTimer()
    : setMs_(5 * 60 * 1000),
      leftMs_(5 * 60 * 1000),
      addMs_(10 * 1000),
      running_(false),
      finished_(false),
      overFlag_(false),
      flashMs_(0),
      lastBeepMs_(0) {}

const char *ToolTimer::title() const { return "定时器"; }
const char *ToolTimer::desc() const { return "倒计时提醒（到点响铃）"; }
const char *ToolTimer::tag() const { return "TIMER"; }
Color ToolTimer::theme() const { return rgba(96, 168, 226); }

void ToolTimer::reset() {
  running_ = false;
  finished_ = false;
  leftMs_ = setMs_;
  flashMs_ = 0;
  lastBeepMs_ = 0;
}

void ToolTimer::startStop() {
  if (finished_) {
    reset();
  } else {
    running_ = !running_;
  }
  sfx(SFX_CLICK);
}

void ToolTimer::addTime(int deltaMs) {
  setMs_ += deltaMs;
  if (setMs_ < 10 * 1000) setMs_ = 10 * 1000;
  if (setMs_ > 99 * 60 * 1000) setMs_ = 99 * 60 * 1000;
  if (!running_) reset();
  sfx(SFX_MOVE);
}

void ToolTimer::update(int dtMs) {
  if (flashMs_ > 0) flashMs_ -= dtMs;
  if (finished_) {
    // 到点后每 1.5 秒提醒一次（直到用户清零/重开）
    lastBeepMs_ += dtMs;
    if (lastBeepMs_ >= 1500) {
      lastBeepMs_ = 0;
      if (host_) host_->playSfx(SFX_OVER);
    }
    return;
  }
  if (!running_) return;
  leftMs_ -= dtMs;
  if (leftMs_ <= 0) {
    leftMs_ = 0;
    running_ = false;
    finished_ = true;
    overFlag_ = true;
    flashMs_ = 3000;
    sfx(SFX_OVER);
  }
}

bool ToolTimer::onKey(int key) {
  if (key == PG_KEY_A) {
    startStop();
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *ToolTimer::hint() const {
  if (finished_) return "时间到 · 按暂停键或点「清零」重新计时";
  if (running_) return "倒计时中 · 按暂停键";
  return "选预设或微调时长，按暂停键开始";
}

const char *ToolTimer::keyBar() const {
  return "暂停键 开始/暂停 · 长按 返回列表";
}

bool ToolTimer::justGameOver() { return overFlag_; }
void ToolTimer::clearGameOverFlag() { overFlag_ = false; }

/* ---------------- 原生 UI 描述 ---------------- */

const char *ToolTimer::uiText(int slot, char *buf, int n) const {
  switch (slot) {
    case 0: {
      int remain = (running_ || finished_) ? leftMs_ : setMs_;
      snprintf(buf, n, "%02d:%02d", remain / 60000, (remain / 1000) % 60);
      return buf;
    }
    case 1:
      if (finished_) return "时间到！";
      if (running_) return "倒计时中";
      return "已暂停";
    case 2:
      snprintf(buf, n, "设定 %02d:%02d", setMs_ / 60000, (setMs_ / 1000) % 60);
      return buf;
    default:
      return "";
  }
}

const char *ToolTimer::uiButton(int i) const {
  switch (i) {
    case 0: return running_ ? "暂停" : (finished_ ? "再来" : "开始");
    case 1: return "清零";
    case 2: return "1 分";
    case 3: return "3 分";
    case 4: return "5 分";
    case 5: return "10 分";
    case 6: return "-10 秒";
    case 7: return "+10 秒";
    default: return "";
  }
}

int ToolTimer::uiButtonStyle(int i) const {
  if (i == 0) return running_ ? 2 : 1;
  if (i == 1) return 2;
  return 0;
}

bool ToolTimer::onUiButton(int i) {
  switch (i) {
    case 0:
      startStop();
      return true;
    case 1:
      reset();
      sfx(SFX_CLICK);
      return true;
    case 2: setMs_ = 60 * 1000; reset(); sfx(SFX_CLICK); return true;
    case 3: setMs_ = 3 * 60 * 1000; reset(); sfx(SFX_CLICK); return true;
    case 4: setMs_ = 5 * 60 * 1000; reset(); sfx(SFX_CLICK); return true;
    case 5: setMs_ = 10 * 60 * 1000; reset(); sfx(SFX_CLICK); return true;
    case 6: addTime(-10 * 1000); return true;
    case 7: addTime(10 * 1000); return true;
    default:
      return false;
  }
}

uint32_t ToolTimer::uiAccent() const {
  // ★ 2026-09-14 UI 改版：iOS 令牌（DATA 橙 / ACCENT 蓝 / SURFACE3 暗）
  if (finished_) return 0xFFFF9F0Au;            // 到点：橙
  return running_ ? 0xFF0A84FFu : 0xFF3A3A3Cu;  // 运行蓝 / 空闲暗
}

/* ==================================================================
 * 秒表
 * ================================================================== */

ToolStopwatch::ToolStopwatch()
    : elapsedMs_(0), lapCount_(0), lastLapMs_(0), running_(false) {
  memset(lapMs_, 0, sizeof(lapMs_));
}

const char *ToolStopwatch::title() const { return "秒表"; }
const char *ToolStopwatch::desc() const { return "计时 / 计次（分:秒.百分秒）"; }
const char *ToolStopwatch::tag() const { return "WATCH"; }
Color ToolStopwatch::theme() const { return rgba(150, 130, 226); }

void ToolStopwatch::reset() {
  elapsedMs_ = 0;
  lapCount_ = 0;
  lastLapMs_ = 0;
  running_ = false;
  memset(lapMs_, 0, sizeof(lapMs_));
}

void ToolStopwatch::update(int dtMs) {
  if (running_) elapsedMs_ += dtMs;
}

bool ToolStopwatch::onKey(int key) {
  if (key == PG_KEY_A) {
    running_ = !running_;
    sfx(SFX_CLICK);
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *ToolStopwatch::hint() const {
  return running_ ? "计时中 · 按暂停键，点「计次」记一圈"
                  : "按暂停键或点「开始」开始计时";
}

const char *ToolStopwatch::keyBar() const {
  return "暂停键 开始/暂停 · 长按 返回列表";
}

/* ---------------- 原生 UI 描述 ---------------- */

const char *ToolStopwatch::uiText(int slot, char *buf, int n) const {
  switch (slot) {
    case 0: {
      int ms = elapsedMs_;
      snprintf(buf, n, "%02d:%02d.%02d", ms / 60000, (ms / 1000) % 60,
               (ms / 10) % 100);
      return buf;
    }
    case 1:
      if (running_) return "计时中";
      return elapsedMs_ > 0 ? "已暂停" : "就绪 · 按开始计时";
    case 2: {
      if (lapCount_ == 0) return "还没有计次";
      snprintf(buf, n, "计次 %d · 最近 %02d:%02d.%02d", lapCount_, lapMs_[0] / 60000,
               (lapMs_[0] / 1000) % 60, (lapMs_[0] / 10) % 100);
      return buf;
    }
    default:
      return "";
  }
}

const char *ToolStopwatch::uiButton(int i) const {
  switch (i) {
    case 0: return running_ ? "暂停" : "开始";
    case 1: return "计次";
    case 2: return "清零";
    default: return "";
  }
}

int ToolStopwatch::uiButtonStyle(int i) const {
  if (i == 0) return running_ ? 2 : 1;
  if (i == 2) return 2;
  return 0;
}

bool ToolStopwatch::onUiButton(int i) {
  switch (i) {
    case 0:
      running_ = !running_;
      sfx(SFX_CLICK);
      return true;
    case 1:
      if (running_ || elapsedMs_ > 0) {
        int lap = elapsedMs_ - lastLapMs_;
        for (int k = MAXLAP - 1; k > 0; --k) lapMs_[k] = lapMs_[k - 1];
        lapMs_[0] = lap;
        lastLapMs_ = elapsedMs_;
        ++lapCount_;
        sfx(SFX_MOVE);
      }
      return true;
    case 2:
      reset();
      sfx(SFX_CLICK);
      return true;
    default:
      return false;
  }
}

uint32_t ToolStopwatch::uiAccent() const {
  // ★ 2026-09-14 UI 改版：iOS 令牌（ACCENT 蓝 / SURFACE3 暗）
  return running_ ? 0xFF0A84FFu : 0xFF3A3A3Cu;
}

/* ==================================================================
 * 计算器（原生 20 键键盘）
 * ================================================================== */

namespace {
// 键盘布局：与 ui/main.html 里 BtnK0..BtnK19 的顺序一一对应
const char kCalcKeys[20] = {'C', '<', '%', '/', '7', '8', '9', '*',
                            '4', '5', '6', '-', '1', '2', '3', '+',
                            '0', '.', 's', '='};
}  // namespace

ToolCalc::ToolCalc()
    : entryLen_(0), acc_(0), op_(0), result_(0), pressedKey_(-1), pressedMs_(0) {
  memset(entry_, 0, sizeof(entry_));
  memset(status_, 0, sizeof(status_));
}

const char *ToolCalc::title() const { return "计算器"; }
const char *ToolCalc::desc() const { return "四则运算（原生键盘）"; }
const char *ToolCalc::tag() const { return "CALC"; }
Color ToolCalc::theme() const { return rgba(120, 200, 190); }

void ToolCalc::reset() {
  entryLen_ = 0;
  entry_[0] = 0;
  acc_ = 0;
  op_ = 0;
  result_ = 0;
  status_[0] = 0;
  pressedKey_ = -1;
}

void ToolCalc::compute() {
  double cur = 0;
  if (entryLen_ > 0) cur = atof(entry_);
  switch (op_) {
    case '+': acc_ += cur; break;
    case '-': acc_ -= cur; break;
    case '*': acc_ *= cur; break;
    case '/': acc_ = (cur == 0.0) ? NAN : acc_ / cur; break;
    default: acc_ = cur; break;
  }
  op_ = 0;
  entryLen_ = 0;
  entry_[0] = 0;
}

void ToolCalc::press(char key) {
  char shown[24];
  if (key >= '0' && key <= '9') {
    if (entryLen_ < 12) {
      entry_[entryLen_++] = key;
      entry_[entryLen_] = 0;
    }
    sfx(SFX_MOVE);
    return;
  }
  switch (key) {
    case '.':
      if (entryLen_ == 0 || strchr(entry_, '.') == 0) {
        if (entryLen_ < 12) {
          if (entryLen_ == 0) entry_[entryLen_++] = '0';
          entry_[entryLen_++] = '.';
          entry_[entryLen_] = 0;
        }
      }
      sfx(SFX_MOVE);
      return;
    case 'C':
      reset();
      sfx(SFX_CLICK);
      return;
    case '<':  // 退格
      if (entryLen_ > 0) entry_[--entryLen_] = 0;
      sfx(SFX_MOVE);
      return;
    case 's':  // 正负号
      if (entryLen_ > 0) {
        if (entry_[0] == '-') {
          memmove(entry_, entry_ + 1, (size_t)entryLen_);
          --entryLen_;
        } else if (entryLen_ < 12) {
          memmove(entry_ + 1, entry_, (size_t)entryLen_ + 1);
          entry_[0] = '-';
          ++entryLen_;
        }
      }
      sfx(SFX_MOVE);
      return;
    case '%':
      if (entryLen_ > 0) {
        double v = atof(entry_) / 100.0;
        fmtNum(shown, sizeof(shown), v);
        snprintf(entry_, sizeof(entry_), "%s", shown);
        entryLen_ = (int)strlen(entry_);
      }
      sfx(SFX_MOVE);
      return;
    case '+':
    case '-':
    case '*':
    case '/': {
      if (entryLen_ > 0) {
        compute();
      } else if (op_ == 0) {
        acc_ = result_;  // 首次按运算符：把上次结果当作左值
      }
      op_ = key;
      fmtNum(shown, sizeof(shown), acc_);
      snprintf(status_, sizeof(status_), "%s %c", shown, op_);
      sfx(SFX_CLICK);
      return;
    }
    case '=': {
      double right = (entryLen_ > 0) ? atof(entry_) : 0.0;
      double left = acc_;
      double res;
      switch (op_) {
        case '+': res = left + right; break;
        case '-': res = left - right; break;
        case '*': res = left * right; break;
        case '/': res = (right == 0.0) ? NAN : left / right; break;
        default: res = (entryLen_ > 0) ? right : left; break;
      }
      if (op_ != 0 && entryLen_ > 0) {
        char lhs[24], rhs[24];
        fmtNum(lhs, sizeof(lhs), left);
        fmtNum(rhs, sizeof(rhs), right);
        snprintf(status_, sizeof(status_), "%s %c %s =", lhs, op_, rhs);
      } else {
        status_[0] = 0;
      }
      acc_ = res;
      result_ = res;
      op_ = 0;
      entryLen_ = 0;
      entry_[0] = 0;
      sfx(SFX_SCORE);
      return;
    }
    default:
      return;
  }
}

bool ToolCalc::onKey(int key) {
  if (key == PG_KEY_A) {  // A = 退格（最常用）
    if (entryLen_ > 0) {
      entry_[--entryLen_] = 0;
    } else {
      reset();
    }
    sfx(SFX_MOVE);
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    sfx(SFX_CLICK);
    return true;
  }
  return false;
}

const char *ToolCalc::hint() const { return "点键盘输入 · 长按暂停键返回"; }
const char *ToolCalc::keyBar() const {
  return "点键盘输入 · 长按 返回列表";
}

/* ---------------- 原生 UI 描述 ---------------- */

const char *ToolCalc::uiText(int slot, char *buf, int n) const {
  switch (slot) {
    case 0: {  // 主显示
      if (entryLen_ > 0) {
        snprintf(buf, n, "%s", entry_);
      } else if (op_ != 0) {
        fmtNum(buf, n, acc_);
      } else {
        fmtNum(buf, n, result_);
      }
      return buf;
    }
    case 1:  // 表达式状态行
      return status_;
    default:
      return "";
  }
}

const char *ToolCalc::uiButton(int i) const {
  if (i < 0 || i >= 20) return "";
  static char one[2];
  switch (kCalcKeys[i]) {
    case '<': return "<-";
    case 's': return "+/-";
    case '*': return "x";
    default:
      one[0] = kCalcKeys[i];
      one[1] = 0;
      return one;
  }
}

int ToolCalc::uiButtonStyle(int i) const {
  if (i < 0 || i >= 20) return 0;
  char k = kCalcKeys[i];
  if (k == '=') return 1;                                       // 等号：主
  if (k == 'C' || k == '<' || k == '%' || k == 's') return 2;    // 功能键：次
  if (k == '+' || k == '-' || k == '*' || k == '/') return 3;    // 运算符：强调
  return 0;                                                      // 数字
}

bool ToolCalc::onUiButton(int i) {
  if (i < 0 || i >= 20) return false;
  pressedKey_ = i;
  pressedMs_ = 120;
  press(kCalcKeys[i]);
  return true;
}

}  // namespace pg
