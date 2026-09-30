/*
 * PgBubble.cpp - 算术泡泡（点破答案正确的那个泡泡）
 *
 * 教育目标：**心算与数感**（5-9 岁）。顶部出一题，下方 4 个泡泡各带一个数字慢慢上浮，
 *   点中答案正确的那个就破掉、进入下一题。
 *
 * 设计取舍（对齐 MEMORY.md 的"儿童游戏设计红线"）：
 *   · **无失败惩罚**：点错只是那个泡泡晃一下 + "再数一数"，分数不掉、题目不变、不限次数。
 *   · **不限时**：泡泡只是**缓慢上浮**（约 30px/s，一个身位约 3 秒），不是"限时下落" ——
 *     速度只是为了画面有生气，不构成时间压力。
 *   · **难度分三档**，按答对数自动升：10 以内加法 → 20 以内加减 → 100 以内加减。
 *   · **不识字也能玩**：题目是阿拉伯数字与加减号，选项是数字。
 *   · **手指友好**：泡泡半径 44px（直径 88），连通后直径远超需要。
 *
 * 视觉：蜡笔童趣风，共用件在 PgKids.h（零素材）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "PgKids.h"
#include "PgLog.h"

namespace pg {

namespace {

using kids::wax;
using kids::ink;
using kids::inkText;
using kids::card;

// BUBBLE_N（候选个数）= 类里的 enum，因为数组要声明在头文件里
const int BUBBLE_R = 44;     // 泡泡半径
const int DRIFT_VY = 30;     // 上浮速度（px/s）—— 刻意慢，不是限时机制
const int POP_MS = 430;      // 答对后泡泡炸开的展示时长，之后换题
const int WRONG_MS = 560;    // 答错时泡泡晃动时长
const int MSG_MS = 1500;     // "再数一数"提示时长
const int PRAISE_MS = 1000;  // 连对赞语时长
const int LEVEL_STEP = 6;    // 每答对 6 题升一档

}  // namespace

GameBubble::GameBubble()
    : a_(1),
      qb_(1),
      ans_(2),
      op_(0),
      score_(0),
      qno_(1),
      combo_(0),
      level_(0),
      popMs_(0),
      popIdx_(-1),
      msgMs_(0),
      praiseMs_(0),
      elapsedMs_(0) {
  memset(bub_, 0, sizeof(bub_));
}

const char *GameBubble::title() const { return "算术泡泡"; }
const char *GameBubble::desc() const { return "点破答案正确的那颗泡泡"; }
const char *GameBubble::tag() const { return "MATH"; }
Color GameBubble::theme() const { return wax(5); }

int GameBubble::colX(int i) const { return 14 + (vw() - 28) * (2 * i + 1) / 8; }
int GameBubble::playTop() const { return 138; }  // 与 render() 里题目卡的下沿对齐
int GameBubble::playBottom() const { return vh() - 10; }

// ---------------- 出题 ----------------
void GameBubble::nextQuestion() {
  level_ = score_ / LEVEL_STEP;
  if (level_ > 2) level_ = 2;

  if (level_ == 0) {
    // 10 以内加法（5-6 岁的主场）
    op_ = 0;
    a_ = 1 + rand() % 5;
    qb_ = 1 + rand() % 5;
  } else if (level_ == 1) {
    // 20 以内加减
    op_ = rand() % 2;
    if (op_ == 0) {
      a_ = 2 + rand() % 10;
      qb_ = 2 + rand() % 10;
      if (a_ + qb_ > 20) qb_ = 20 - a_;
    } else {
      a_ = 6 + rand() % 15;
      qb_ = 1 + rand() % a_;  // 保证不出现负数
    }
  } else {
    // 100 以内加减（两位数）
    op_ = rand() % 2;
    if (op_ == 0) {
      a_ = 10 + rand() % 60;
      qb_ = 10 + rand() % 30;
      if (a_ + qb_ > 100) qb_ = 100 - a_;
    } else {
      a_ = 30 + rand() % 70;
      qb_ = 5 + rand() % 25;
    }
  }
  ans_ = (op_ == 0) ? (a_ + qb_) : (a_ - qb_);
  if (ans_ < 0) ans_ = 0;

  // 三个干扰项：答案 ±1..5，去重、不为负
  int opts[BUBBLE_N];
  opts[0] = ans_;
  int n = 1;
  int guard = 0;
  while (n < BUBBLE_N && guard++ < 200) {
    int d = 1 + rand() % 5;
    if (rand() % 2) d = -d;
    int v = ans_ + d;
    if (v < 0) v = ans_ + (d < 0 ? -d : d);
    if (v == ans_) continue;
    bool dup = false;
    for (int k = 0; k < n; ++k)
      if (opts[k] == v) dup = true;
    if (dup) continue;
    opts[n++] = v;
  }
  while (n < BUBBLE_N) opts[n++] = ans_ + n;  // 兜底（几乎不可能走到）

  for (int i = n - 1; i > 0; --i) {
    int j = rand() % (i + 1);
    int t = opts[i];
    opts[i] = opts[j];
    opts[j] = t;
  }

  // 放进 4 列泡泡（起点错开，看起来像一批新泡泡浮上来）
  for (int i = 0; i < BUBBLE_N; ++i) {
    bub_[i].x = colX(i);
    bub_[i].y = playTop() + 64 + i * 74;
    bub_[i].val = opts[i];
    bub_[i].wobbleMs = 0;
  }
  popMs_ = 0;
  popIdx_ = -1;
}

void GameBubble::reset() {
  score_ = 0;
  qno_ = 1;
  combo_ = 0;
  level_ = 0;
  msgMs_ = 0;
  praiseMs_ = 0;
  elapsedMs_ = 0;
  nextQuestion();
}

// ---------------- 推进 ----------------
void GameBubble::update(int dtMs) {
  if (msgMs_ > 0) msgMs_ -= dtMs;
  if (praiseMs_ > 0) praiseMs_ -= dtMs;

  if (popMs_ > 0) {
    popMs_ -= dtMs;
    if (popMs_ <= 0) {
      ++qno_;
      nextQuestion();
    }
    return;  // 换题动画期间泡泡不动（画面更干净）
  }

  if (state_ != GSTATE_RUNNING) return;
  elapsedMs_ += dtMs;

  const int top = playTop(), bot = playBottom();
  for (int i = 0; i < BUBBLE_N; ++i) {
    if (bub_[i].wobbleMs > 0) bub_[i].wobbleMs -= dtMs;
    bub_[i].y -= DRIFT_VY * dtMs / 1000;
    if (bub_[i].y + BUBBLE_R < top) bub_[i].y = bot + BUBBLE_R;  // 从下面再浮上来
  }
}

// ---------------- 绘制 ----------------
void GameBubble::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  kids::paperBg(c);

  // ① 题目卡
  const int cx0 = 22, cw = W - 44, cy0 = 10, ch = 118;
  kids::panel(c, cx0, cy0, cw, ch, 20, card(), 3);

  /* ★★ 题目**不用 bigText(拼好的串)**，改用 `bigNumber()` + 字面量运算符拼起来。
   * 原因（实测过，代价很大）：genfont.py 对"非字面量实参"的 text/bigText 调用会退化成
   * "**本文件全部短字面量**"这个保守超集 —— 一旦档位还是变量，就会把中文灌进
   * 48/64/80px 三档（每枚字形上千字节），字库从 2.6MB 涨到 4.6MB。
   * 现在：数字走 bigNumber（不在扫描名单，靠 BIG 恒带的 0-9），运算符是**字面量** ⇒ 零新增字形。
   * 版式：`a op b`，三段各自量宽后整体居中；卡内宽 412px 足够放 "99 - 29"（264px）。 */
  const int yq = cy0 + 6;
  const int gap = 12;
  const int w1 = c.bigNumberW(a_, 3);
  const int w2 = c.bigNumberW(qb_, 3);
  int wop = c.bigTextW("+", 3);
  if (c.bigTextW("-", 3) > wop) wop = c.bigTextW("-", 3);
  const int xq = cx0 + (cw - (w1 + wop + w2 + 2 * gap)) / 2;
  c.bigNumber(xq, yq, a_, 3, inkText());
  const int xo = xq + w1 + gap;
  if (op_ == 0)
    c.bigText(xo, yq, "+", 3, inkText());
  else
    c.bigText(xo, yq, "-", 3, inkText());
  c.bigNumber(xo + wop + gap, yq, qb_, 3, inkText());

  /* 卡片底部那一行是"共用槽"：赞语 / 提示 / 默认问句 三选一（都写**字面量**）。
   * 为什么不做成三个位置：泡泡区从 y=138 起，另起一行会压到泡泡上。 */
  const int yb = cy0 + 80;
  if (praiseMs_ > 0) {
    if (combo_ >= 8)
      c.textCenter(W / 2, yb, "太强了！", 2, wax(1));
    else if (combo_ >= 5)
      c.textCenter(W / 2, yb, "好厉害！", 2, wax(1));
    else
      c.textCenter(W / 2, yb, "真棒！", 2, wax(1));
  } else if (msgMs_ > 0) {
    c.textCenter(W / 2, yb, "再数一数", 2, wax(0));
  } else {
    c.textCenter(W / 2, yb, "等于几？", 2, ink());
  }

  // ② 泡泡
  for (int i = 0; i < BUBBLE_N; ++i) {
    int x = bub_[i].x, y = bub_[i].y;
    if (bub_[i].wobbleMs > 0) x += (int)(7 * sinf((float)bub_[i].wobbleMs / 26.0f));

    int r = BUBBLE_R;
    if (popMs_ > 0 && i == popIdx_) {
      // 答对：向外胀一圈再收（用剩余时间反向插值，不引入缩放贴图）
      int t = POP_MS - popMs_;
      r = BUBBLE_R + t * 14 / POP_MS;
    }
    if (y + r < playTop() - 4 || y - r > playBottom() + 4) continue;  // 出界不画

    Color fill = lerpColor(wax(i + 2), rgba(255, 255, 255), 140);  // 粉彩：蜡笔涂色感
    kids::disc(c, x, y, r, fill, 3);
    // 左上高光（不透明浅色，不用 alpha —— 画布 fillCircle 是覆写，不做混合）
    c.fillCircle(x - r / 3, y - r / 3, r / 7, lerpColor(fill, rgba(255, 255, 255), 190));

    /* 泡泡上的数字走 `bigNumber()`（单一整数），**不走 bigText(buf)**：
     *   bigNumber 不在 genfont 的扫描名单里 ⇒ 数字只靠 BIG 恒带的 " 0123456789.:-+"，
     *   一个字库字形都不加。档位由 fitBigNumber 现算（位数多、泡小时自动降档）。 */
    int bs = c.fitBigNumber(2 * r - 26, 2 * r - 26, bub_[i].val, 3);
    c.bigNumberCenter(x, y - c.bigTextH(bs) / 2, bub_[i].val, bs, inkText());
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 130);
    c.textCenter(W / 2, H / 2 - 92, "算术泡泡", 5, wax(5));
    /* ★ 说明行必须是**档 2**：11 个中文 × 档 3(48px) = 528px > 画布宽 480px，
     *   会被屏幕左右各裁掉 24px（用户报障"文字太大超出屏幕"）。
     *   档 2 的 32px ⇒ 352px，两侧各留 64px。改之前先跑 `tools/check_textwidth.py`。 */
    c.textCenter(W / 2, H / 2 - 16, "点破答案正确的那颗泡泡", 2, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 28, "点击屏幕开始", 2, rgba(200, 200, 205));
  }
}

// ---------------- 输入 ----------------
bool GameBubble::onTouch(int action, int x, int y) {
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;
  if (action != PG_TOUCH_DOWN) return true;
  if (popMs_ > 0) return true;  // 换题动画期间不接受输入

  for (int i = 0; i < BUBBLE_N; ++i) {
    long long dx = x - bub_[i].x, dy = y - bub_[i].y;
    if (dx * dx + dy * dy > (long long)BUBBLE_R * BUBBLE_R) continue;

    if (bub_[i].val == ans_) {
      ++score_;
      ++qno_;
      ++combo_;
      sfx(SFX_SCORE);
      popMs_ = POP_MS;
      popIdx_ = i;
      msgMs_ = 0;
      saveBestIfNeeded(score_);
      if (combo_ >= 3) praiseMs_ = PRAISE_MS;  // 文案按连对数在 render 里选（用字面量）
    } else {
      bub_[i].wobbleMs = WRONG_MS;
      msgMs_ = MSG_MS;
      combo_ = 0;
      sfx(SFX_HIT);
    }
    return true;
  }
  return true;
}

bool GameBubble::onKey(int key) {
  if (key == PG_KEY_A) {
    if (state_ == GSTATE_READY) state_ = GSTATE_RUNNING;
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

// ---------------- HUD ----------------
const char *GameBubble::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", qno_);
  return buf;
}

const char *GameBubble::info2Value(char *buf, int n) const {
  if (combo_ >= 2) {
    snprintf(buf, n, "%d 连", combo_);
    return buf;
  }
  return "";  // 空串 = 该行留空（不是"—"：多一个字形就多一处可能的缺字点）
}

const char *GameBubble::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点破答案正确的那颗泡泡（不限时，点错不要紧）";
}

// QA 钩子：只加"让状态前进"的，不加"直接改分"的
void GameBubble::debugCmd(const char *rest) {
  char cmd[24];
  int k = 0;
  while (rest[k] && rest[k] != ' ' && k < 23) {
    cmd[k] = rest[k];
    ++k;
  }
  cmd[k] = 0;

  if (strcmp(cmd, "q") == 0) {
    // 打印当前题目与选项（验收用："答案到底在哪一列"必须可复现，不能靠盲点）
    char line[160];
    int off = snprintf(line, sizeof(line), "qa q level=%d %d%c%d=%d ans=%d |", level_,
                       a_, op_ == 0 ? '+' : '-', qb_, ans_, ans_);
    for (int i = 0; i < BUBBLE_N; ++i)
      off += snprintf(line + off, sizeof(line) - off, " c%d=%d", i + 1, bub_[i].val);
    pg::logInfo("%s", line);
    return;
  }
  if (strcmp(cmd, "answer") == 0) {
    // 直接点破"正确泡泡"那一列 —— 走的是与触摸**同一条**判定路径
    for (int i = 0; i < BUBBLE_N; ++i) {
      if (bub_[i].val == ans_) {
        pg::logInfo("qa answer -> tap correct bubble");
        onTouch(PG_TOUCH_DOWN, bub_[i].x, bub_[i].y);
        return;
      }
    }
    pg::logInfo("qa answer: correct bubble not found");
    return;
  }
  if (strcmp(cmd, "wrong") == 0) {
    for (int i = 0; i < BUBBLE_N; ++i) {
      if (bub_[i].val != ans_) {
        pg::logInfo("qa wrong -> tap a wrong bubble");
        onTouch(PG_TOUCH_DOWN, bub_[i].x, bub_[i].y);
        return;
      }
    }
    return;
  }
  if (strcmp(cmd, "next") == 0) {
    nextQuestion();
    pg::logInfo("qa next");
    return;
  }
  pg::logInfo("qa: unknown cmd");
}

}  // namespace pg
