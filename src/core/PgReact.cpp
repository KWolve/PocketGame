/*
 * PgReact.cpp - 反应计时（看谁反应快，记录毫秒）
 *
 * 全部用原生控件渲染（nativePage = 2 → 窗口 WinReact）：本游戏的核心就是那个"毫秒数"，
 * 画布点阵字体放到 96px 会糊，原生矢量字库才清晰。
 *
 * 流程：点"开始" → 按钮变蓝"等待…"（随机 1.2~3.6 秒）→ 变绿"点这里！"
 *      → 玩家点击，差值就是反应时间 → 显示毫秒，再点一次进入下一轮。
 *      在等待期间抢点 = 抢跑，本轮作废（这是反应测试类游戏的标配规则）。
 *
 * 精度：靠主循环步长累加（约 16ms/帧），对"人类反应 200~400ms"完全够用。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

/* ★ 2026-09-14 UI 改版：换成 iOS 深色令牌（ACCENT / SUCCESS / DATA / DANGER）。
 * ⚠️ 这四个色**同时**用于阶段色条、阶段文字与反应区大按钮 —— 改一处三处同步生效。 */
Color accentIdle() { return rgba(10, 132, 255); }    // 蓝：待机 / 等待
Color accentGo() { return rgba(48, 209, 88); }       // 绿：就是现在
Color accentResult() { return rgba(255, 159, 10); }  // 橙：出成绩
Color accentEarly() { return rgba(255, 69, 58); }    // 红：抢跑

}  // namespace

GameReact::GameReact()
    : phase_(PH_IDLE),
      waitMs_(0),
      elapsedMs_(0),
      lastMs_(0),
      bestMs_(0),
      sumMs_(0),
      rounds_(0),
      overFlag_(false) {}

const char *GameReact::title() const { return "反应计时"; }
const char *GameReact::desc() const { return "看到变绿立刻点，测反应毫秒"; }
const char *GameReact::tag() const { return "REACT"; }
Color GameReact::theme() const { return rgba(46, 186, 112); }

void GameReact::reset() {
  phase_ = PH_IDLE;
  waitMs_ = 0;
  elapsedMs_ = 0;
  lastMs_ = 0;
  sumMs_ = 0;
  rounds_ = 0;
  overFlag_ = false;
  // bestMs_ 跨局保留（那是"你的最好成绩"，重来不该抹掉）
}

void GameReact::armWait() {
  phase_ = PH_WAIT;
  waitMs_ = 1200 + rand() % 2400;  // 1.2 ~ 3.6 秒，避免被节奏猜到
  elapsedMs_ = 0;
}

void GameReact::tap() {
  switch (phase_) {
    case PH_IDLE:
      armWait();
      sfx(SFX_CLICK);
      break;
    case PH_WAIT:
      // 抢跑：等待期间就点了
      phase_ = PH_EARLY;
      sfx(SFX_HIT);
      break;
    case PH_NOW:
      lastMs_ = elapsedMs_;
      if (lastMs_ < 1) lastMs_ = 1;
      if (bestMs_ <= 0 || lastMs_ < bestMs_) bestMs_ = lastMs_;
      sumMs_ += lastMs_;
      ++rounds_;
      phase_ = PH_RESULT;
      sfx(SFX_SCORE);
      break;
    case PH_RESULT:
    case PH_EARLY:
    default:
      armWait();
      sfx(SFX_CLICK);
      break;
  }
}

void GameReact::update(int dtMs) {
  if (phase_ == PH_WAIT) {
    waitMs_ -= dtMs;
    if (waitMs_ <= 0) {
      phase_ = PH_NOW;
      elapsedMs_ = 0;
      sfx(SFX_JUMP);
    }
  } else if (phase_ == PH_NOW) {
    elapsedMs_ += dtMs;
    if (elapsedMs_ > 2500) {  // 2.5 秒没点 → 当作放弃
      phase_ = PH_IDLE;
      sfx(SFX_OVER);
    }
  }
}

bool GameReact::onKey(int key) {
  if (key == PG_KEY_A) {
    tap();  // 物理键也能测（只是比触摸慢一点）
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *GameReact::uiText(int slot, char *buf, int n) const {
  switch (slot) {
    case 0:  // 主大字：毫秒数 / 状态大字
      switch (phase_) {
        case PH_WAIT: return "···";
        case PH_NOW: return "点！";
        case PH_RESULT:
          snprintf(buf, n, "%d", lastMs_);
          return buf;
        case PH_EARLY: return "抢跑";
        default: return "--";
      }
    case 1:  // 状态行
      switch (phase_) {
        case PH_WAIT: return "等待变绿…";
        case PH_NOW: return "就是现在！";
        case PH_RESULT: return "毫秒";
        case PH_EARLY: return "太早了，等变绿再点";
        default: return "点击下方按钮开始";
      }
    case 2: {  // 副行：最好成绩 / 平均
      if (rounds_ <= 0) return "共测 5 轮，取平均更准";
      int avg = sumMs_ / rounds_;
      snprintf(buf, n, "最好 %d · 平均 %d（%d 次）", bestMs_, avg, rounds_);
      return buf;
    }
    default:
      return "";
  }
}

const char *GameReact::uiButton(int i) const {
  if (i != 0) return "";  // 只用一个按钮
  switch (phase_) {
    case PH_WAIT: return "等待…";
    case PH_NOW: return "点这里！";
    case PH_RESULT: return "再来一次";
    case PH_EARLY: return "重新开始";
    default: return "开始";
  }
}

int GameReact::uiButtonStyle(int i) const {
  if (i != 0) return 0;
  switch (phase_) {
    case PH_WAIT: return 3;    // 蓝（强调）
    case PH_NOW: return 1;     // 绿（主行动）—— 与等待的蓝形成强对比
    case PH_EARLY: return 2;   // 暗红（失败）
    case PH_RESULT: return 3;  // 蓝
    default: return 1;         // 绿
  }
}

bool GameReact::onUiButton(int i) {
  if (i != 0) return false;
  tap();
  return true;
}

uint32_t GameReact::uiAccent() const {
  switch (phase_) {
    case PH_WAIT: return accentIdle();
    case PH_NOW: return accentGo();
    case PH_RESULT: return accentResult();
    case PH_EARLY: return accentEarly();
    default: return accentIdle();
  }
}

const char *GameReact::hint() const {
  switch (phase_) {
    case PH_WAIT: return "忍住 —— 等按钮变绿再点";
    case PH_NOW: return "点！";
    case PH_RESULT: return "再点一次继续下一轮";
    case PH_EARLY: return "抢跑了，本轮作废 —— 再点一次重来";
    default: return "点下方按钮开始（多测几轮看平均）";
  }
}

const char *GameReact::keyBar() const {
  return "暂停键 开始/下一轮 · 长按 返回列表";
}

bool GameReact::justGameOver() { return overFlag_; }
void GameReact::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
