/*
 * ToolPage.cpp - "原生工具页"通用外壳实现。设计说明见 ToolPage.h。
 */
#include "ui/ToolPage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "control/ZKButton.h"
#include "control/ZKTextView.h"
#include "entry/EasyUIContext.h"

#include "core/PgGame.h"
#include "core/PgGames.h"
#include "platform/PgAlarm.h"
#include "platform/PgAudio.h"
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgSkin.h"      // pg::applyRoundedBg / toolBtnColor（按钮配色唯一真值）
#include "utils/Log.h"

namespace pg {

namespace {

/* 长按阈值：与主界面 KeyRouter 一致（本板无 autorepeat，只能按时长判） */
const long kLongPressMs = 700;

/* 单调时钟：长按轮询要与 keyDown/keyUp 用**同一条时间轴**，所以这里自带一份
 * （keyDown/keyUp 的时刻由页面传入，页面用的也是 clock_gettime(MONOTONIC)）。 */
long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* 键码 -> 音量±（与 mainLogic 的三个键码定义保持一致） */
const int kCodeVolDown = 103;
const int kCodeVolUp = 105;
const int kCodePause = 108;

/* 按钮配色：**不再在这里放第二份色表**（改版时就是这里漏了，工具页整片还是旧配色）。
 * 唯一真值 = platform/PgSkin.h 的 pg::toolBtnColor() / toolBtnFg()。 */

/* 把强调色提亮（状态行直接用强调色太暗，深底上几乎看不见） */
uint32_t brighten(uint32_t c, int add) {
  int a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
  r += add;
  g += add;
  b += add;
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

bool setTextIfChanged(ZKTextView *tv, char *cache, int cap, const char *val) {
  if (!tv) return false;
  if (!val) val = "";
  if (strncmp(cache, val, (size_t)cap - 1) == 0) return false;  // 没变不写
  snprintf(cache, (size_t)cap, "%s", val);
  tv->setText(cache);
  return true;
}

}  // namespace

ToolPage::ToolPage()
    : game_(0), active_(false), downCode_(-1), downMs_(0), longFired_(false),
      lastAccent_(0) {
  memset(cTitle_, 0, sizeof(cTitle_));
  memset(cMain_, 0, sizeof(cMain_));
  memset(cPhase_, 0, sizeof(cPhase_));
  memset(cSub_, 0, sizeof(cSub_));
  memset(cHint_, 0, sizeof(cHint_));
  memset(cKeyBar_, 0, sizeof(cKeyBar_));
  memset(btnText_, 0, sizeof(btnText_));
  for (int i = 0; i < 20; ++i) {
    btnStyle_[i] = -1;
    btnShown_[i] = -1;
  }
}

bool ToolPage::init(const ToolPageBinds &b) {
  b_ = b;
  if (b_.slot < 0 || b_.slot >= appCount()) {
    LOGW("ToolPage: 槽位 %d 非法", b_.slot);
    return false;
  }
  game_ = gameEntry(b_.slot).create();
  if (!game_) {
    LOGW("ToolPage: 槽位 %d 建不出 Game", b_.slot);
    return false;
  }
  game_->bind(pgHost(), b_.slot);
  game_->setViewport(480, 540);   // 工具页不用画布，但保留统一初值
  game_->reset();
  game_->setState(GSTATE_RUNNING);

  // 清缓存 → 第一帧全量写一遍控件
  for (int i = 0; i < 20; ++i) {
    btnStyle_[i] = -1;
    btnShown_[i] = -1;
    btnText_[i][0] = 0;
  }
  cTitle_[0] = cMain_[0] = cPhase_[0] = cSub_[0] = cHint_[0] = cKeyBar_[0] = 0;
  lastAccent_ = 0;
  /* 顶部整条 8px 色带：改版后**不要** —— iOS 页面里没有"整条饱和色"这种东西，
   * 它是工程软件的观感（也是"不够简洁"的来源之一）。页面识别改由状态行文字色承担。 */
  if (b_.accentBar) b_.accentBar->setVisible(false);
  syncAll();
  LOGD("ToolPage: 就绪 slot=%d %s（%s，按钮 %d）", b_.slot, game_->title(),
       b_.appName ? b_.appName : "?", b_.btnCount);
  return true;
}

void ToolPage::quit() {
  if (game_) {
    delete game_;
    game_ = 0;
  }
  active_ = false;
}

void ToolPage::show() {
  active_ = true;
  /* 工作界面禁屏保。⚠️ 主界面的 tickScreensaverPolicy 会**让位**（它在前台之外
   * 时不碰这个开关），所以这里关掉就是最终态（见 mainLogic 里那段注释）。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
}

void ToolPage::hide() {
  active_ = false;
  downCode_ = -1;
  bool allow = EASYUICONTEXT->getScreensaverTimeOut() > 0;
  EASYUICONTEXT->setScreensaverEnable(allow);
  EASYUICONTEXT->resetScreensaverTimeOut();
}

void ToolPage::tick(int dtMs) {
  if (!active_ || !game_) return;
  /* ★ 闹钟响铃期间**让位**：本页是全屏独立页，留着它就看不到主界面的"闹钟提醒页"
   *   （提醒页在 main.ftu，由主循环 tickAlarmRing() 弹出）⇒ 收掉自己回主界面。
   *   顺带补上老版本的空档：以前在独立页里响铃只有声音、没有界面，用户找不到"停止"。
   *   见 docs/page-split-plan.md §9.1。 */
  if (pg::Alarm::instance().ringing() && b_.appName) {
    LOGD("ToolPage: 响铃中 -> 让位（回主界面弹提醒页）");
    EASYUICONTEXT->closeActivity(b_.appName);
    active_ = false;
    return;
  }
  /* ★ 长按达标就**立刻**退出，不等按键抬起（本板 gpio-keys 无 autorepeat，
   *   长按期间没有任何按键事件 ⇒ 只在 keyUp 里判的话，用户按满 700ms 还得一直
   *   按到松手才切页，手感是"按了不动、松手才跳"）。工具页定时器 50ms 一拍，
   *   判定精度足够，所以放这儿做；8 个工具页共用这一段，各自不用改。
   *   ⚠️ 必须**先**判这个再 update：否则会白跑一帧再关页。 */
  if (keyLongPressReady(nowMs())) {
    LOGD("ToolPage: 长按达标（不等抬起）-> 返回列表");
    if (b_.appName) EASYUICONTEXT->closeActivity(b_.appName);
    active_ = false;
    return;
  }
  game_->update(dtMs);
  syncAll();
}

/* 当前"按下且已超过长按阈值"？是则标记已触发并返回 true（只返回一次）。
 * nowMs 由调用方给（与 keyDown/keyUp 用同一条时间轴）。 */
bool ToolPage::keyLongPressReady(long nowMs) {
  if (downCode_ < 0 || longFired_) return false;   // 有键按着（不分键码，与原语义一致）
  if (nowMs - downMs_ < kLongPressMs) return false;
  longFired_ = true;
  return true;
}

void ToolPage::button(int i) {
  if (!active_ || !game_) return;
  if (i < 0 || i >= b_.btnCount) return;
  bool used = game_->onUiButton(i);
  if (used && pgHost()) pgHost()->playSfx(SFX_CLICK);
  // 状态可能刚变（开始/暂停）→ 让文本与配色立刻跟上，不等下一帧
  syncAll();
  LOGD("ToolPage: 按钮 %d '%s' -> %s", i, btnText_[i], used ? "已处理" : "无动作");
}

ToolKeyResult ToolPage::keyDown(int keyCode, long nowMs) {
  /* ★ 屏保开着：任意键只唤醒（放在 active_ 检查之前 —— 屏保时本页是 hide 状态、
   *   active_ 已经是 false，否则第一句就 return IGNORE、根本轮不到唤醒）。 */
  if (pg::wakeSaverByCode(keyCode, true, false)) return TOOL_KEY_HANDLED;
  if (!active_) return TOOL_KEY_IGNORE;
  if (keyCode != kCodeVolDown && keyCode != kCodeVolUp && keyCode != kCodePause)
    return TOOL_KEY_IGNORE;
  downCode_ = keyCode;
  downMs_ = nowMs;
  longFired_ = false;
  LOGD("ToolPage: 键按下 code=%d", keyCode);
  return TOOL_KEY_HANDLED;   // 按下先吞掉，动作统一在抬起时判
}

ToolKeyResult ToolPage::keyUp(int keyCode, long nowMs) {
  if (pg::wakeSaverByCode(keyCode, false, false)) return TOOL_KEY_HANDLED;
  if (!active_) return TOOL_KEY_IGNORE;
  if (keyCode != downCode_) return TOOL_KEY_IGNORE;
  int held = (int)(nowMs - downMs_);
  int code = downCode_;
  downCode_ = -1;

  if (code == kCodeVolDown || code == kCodeVolUp) {
    int pct = pg::volumeStepGlobal(code == kCodeVolUp ? 1 : -1);
    LOGD("ToolPage: 音量%s -> %d%%", code == kCodeVolUp ? "+" : "-", pct);
    return TOOL_KEY_HANDLED;   // 音量条 OSD 在主界面/本页各自处理，这里只调音量
  }

  /* 108：长按 = 返回主列表（工具页没有触摸返回按钮，这是唯一出口） */
  if (held >= kLongPressMs && !longFired_) {
    longFired_ = true;
    LOGD("ToolPage: 长按 %dms -> 返回列表", held);
    return TOOL_KEY_EXIT;
  }
  if (game_) {
    game_->onKey(PG_KEY_A);
    syncAll();
    LOGD("ToolPage: 短按 %dms -> 开始/暂停", held);
  }
  return TOOL_KEY_HANDLED;
}

void ToolPage::syncAll() {
  if (!game_) return;
  char buf[128];

  // 标题
  setTextIfChanged(b_.title, cTitle_, sizeof(cTitle_), game_->title());
  // 主数值 / 状态行 / 副行
  setTextIfChanged(b_.mainText, cMain_, sizeof(cMain_),
                   game_->uiText(0, buf, sizeof(buf)));
  setTextIfChanged(b_.phase, cPhase_, sizeof(cPhase_),
                   game_->uiText(1, buf, sizeof(buf)));
  setTextIfChanged(b_.sub, cSub_, sizeof(cSub_),
                   game_->uiText(2, buf, sizeof(buf)));
  setTextIfChanged(b_.hint, cHint_, sizeof(cHint_), game_->hint());
  setTextIfChanged(b_.keyBar, cKeyBar_, sizeof(cKeyBar_), game_->keyBar());

  // 强调色只有一处用：状态行文字（顶部色条已隐藏，见 init 里的说明）
  uint32_t accent = game_->uiAccent();
  if (accent && accent != lastAccent_) {
    lastAccent_ = accent;
    if (b_.phase) b_.phase->setTextColor(brighten(accent, 55));
  }

  // 按钮：标签 / 样式 / 显隐
  for (int i = 0; i < b_.btnCount && i < 20; ++i) {
    ZKButton *btn = b_.btns ? b_.btns[i] : 0;
    if (!btn) continue;
    const char *label = game_->uiButton(i);
    if (!label) label = "";
    int show = (label[0] != 0) ? 1 : 0;
    if (btnShown_[i] != show) {
      btnShown_[i] = show;
      btn->setVisible(show != 0);
    }
    if (!show) continue;
    if (strcmp(btnText_[i], label) != 0) {
      snprintf(btnText_[i], sizeof(btnText_[i]), "%s", label);
      btn->setText(btnText_[i]);
    }
    int style = game_->uiButtonStyle(i);
    if (style < 0 || style > 3) style = 0;
    if (btnStyle_[i] != style) {
      btnStyle_[i] = style;
      /* ⚠️ 顺序很重要：**先设底色，再尝试挂圆角图**。
       *   applyRoundedBg 在某些情况下会"什么都不做"（该 (样式,尺寸) 没有资源、或控件太薄），
       *   那时必须留下**正确的底色**（直角但颜色对），而不是沿用上一次的颜色。
       *   （踩过：只调 applyRoundedBg ⇒ 计算器的橙/绿键因为缺 108x64 的图而整片变灰。） */
      uint32_t c = pg::toolBtnColor(style);
      btn->setBackgroundColor(c);
      btn->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, c);
      pg::applyRoundedBg(btn, c);   // 有对应资源就升级成圆角版（内部会清底色）
      btn->setTextColor(pg::toolBtnFg(style));
    }
  }
}

}  // namespace pg
