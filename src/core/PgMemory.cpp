/*
 * PgMemory.cpp - 记忆翻牌（4x4，8 对图案）
 *
 * 设计取舍：**牌面不写字，用 8 种"形状 + 颜色"组合**。
 * 原因有两条：① 触摸屏上图形比文字更好认，老人小孩都能玩；
 * ② 画布用的是点阵字体，放大后边缘会糊（见 docs/ui-design-baseline.md §8），
 *    能不用文字就不用 —— 本作的全部文字都在顶部 HUD（原生矢量控件）里。
 *
 * 玩法：点一张翻开、再点一张；相同则消除，不同则延时翻回。全部消除即通关。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

Color bgColor() { return rgba(24, 26, 34); }
Color backColor() { return rgba(46, 56, 74); }
Color backEdge() { return rgba(78, 96, 124); }
Color backMark() { return rgba(96, 116, 148); }
Color doneColor() { return rgba(38, 66, 52); }
Color doneEdge() { return rgba(110, 206, 140); }

// 8 种图案配色（同时用形状+颜色区分，弱视/色觉差异也能靠形状认）
Color symColor(int s) {
  switch (s) {
    case 0: return rgba(228, 87, 76);    // 红
    case 1: return rgba(92, 168, 226);   // 蓝
    case 2: return rgba(120, 226, 160);  // 绿
    case 3: return rgba(242, 179, 61);   // 琥珀
    case 4: return rgba(168, 140, 232);  // 紫
    case 5: return rgba(94, 214, 206);   // 青
    case 6: return rgba(240, 148, 78);   // 橙
    default: return rgba(228, 120, 170); // 粉
  }
}

// 画一个图案（纯几何，无文字）
void drawSymbol(Canvas &c, int sym, int cx, int cy, int r) {
  Color col = symColor(sym);
  switch (sym) {
    case 0:  // 圆
      c.fillCircle(cx, cy, r, col);
      break;
    case 1:  // 圆角方块
      c.fillRectRound(cx - r, cy - r, 2 * r, 2 * r, 10, col);
      break;
    case 2:  // 正三角
      c.fillTriangle(cx, cy - r, cx - r, cy + r * 4 / 5, cx + r, cy + r * 4 / 5,
                     col);
      break;
    case 3:  // 菱形
      c.fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, col);
      c.fillTriangle(cx, cy + r, cx - r, cy, cx + r, cy, col);
      break;
    case 4:  // 圆环
      c.strokeCircle(cx, cy, r * 3 / 4, 7, col);
      break;
    case 5:  // 十字
      c.fillRect(cx - r / 3, cy - r, 2 * r / 3, 2 * r, col);
      c.fillRect(cx - r, cy - r / 3, 2 * r, 2 * r / 3, col);
      break;
    case 6:  // 双圆
      c.fillCircle(cx - r * 9 / 20, cy, r * 3 / 5, col);
      c.fillCircle(cx + r * 9 / 20, cy, r * 3 / 5, col);
      break;
    default:  // 扁椭圆
      c.fillEllipse(cx, cy, r, r * 5 / 9, col);
      break;
  }
}

}  // namespace

GameMemory::GameMemory()
    : first_(-1),
      second_(-1),
      hideMs_(0),
      steps_(0),
      matched_(0),
      elapsedMs_(0),
      over_(false),
      overFlag_(false) {
  memset(c_, 0, sizeof(c_));
}

const char *GameMemory::title() const { return "记忆翻牌"; }
const char *GameMemory::desc() const { return "翻开两张相同图案配对消除"; }
const char *GameMemory::tag() const { return "MEMO"; }
Color GameMemory::theme() const { return rgba(168, 140, 232); }

void GameMemory::layout(int &bx, int &by, int &cw, int &ch) const {
  const int gap = 12;
  int avail = vw() - 2 * 22;
  cw = (avail - (COLS - 1) * gap) / COLS;
  ch = cw;
  bx = (vw() - (COLS * cw + (COLS - 1) * gap)) / 2;
  by = 46;
}

void GameMemory::shuffle() {
  int deck[CARDS];
  for (int i = 0; i < PAIRS; ++i) {
    deck[i * 2] = i;
    deck[i * 2 + 1] = i;
  }
  for (int i = CARDS - 1; i > 0; --i) {
    int j = rand() % (i + 1);
    int t = deck[i];
    deck[i] = deck[j];
    deck[j] = t;
  }
  for (int i = 0; i < CARDS; ++i) {
    c_[i].sym = deck[i];
    c_[i].open = false;
    c_[i].done = false;
  }
}

void GameMemory::reset() {
  shuffle();
  first_ = second_ = -1;
  hideMs_ = 0;
  steps_ = 0;
  matched_ = 0;
  elapsedMs_ = 0;
  over_ = false;
  overFlag_ = false;
}

int GameMemory::at(int x, int y) const {
  int bx, by, cw, ch;
  layout(bx, by, cw, ch);
  const int gap = 12;
  if (x < bx || y < by) return -1;
  int col = (x - bx) / (cw + gap);
  int row = (y - by) / (ch + gap);
  if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return -1;
  // 落在间隙里不算命中
  int lx = (x - bx) - col * (cw + gap);
  int ly = (y - by) - row * (ch + gap);
  if (lx >= cw || ly >= ch) return -1;
  return row * COLS + col;
}

void GameMemory::update(int dtMs) {
  if (over_) return;
  if (state_ != GSTATE_RUNNING) return;
  elapsedMs_ += dtMs;

  if (hideMs_ > 0) {
    hideMs_ -= dtMs;
    if (hideMs_ <= 0) {
      hideMs_ = 0;
      Card &a = c_[first_];
      Card &b = c_[second_];
      if (a.sym == b.sym) {
        a.done = b.done = true;
        ++matched_;
        sfx(SFX_MERGE);
        if (matched_ >= PAIRS) {
          over_ = true;
          overFlag_ = true;
          state_ = GSTATE_OVER;
          sfx(SFX_OVER);
        }
      } else {
        a.open = b.open = false;
        sfx(SFX_HIT);
      }
      first_ = second_ = -1;
    }
  }
}

void GameMemory::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.clear(bgColor());

  int bx, by, cw, ch;
  layout(bx, by, cw, ch);
  const int gap = 12;

  for (int i = 0; i < CARDS; ++i) {
    int col = i % COLS, row = i / COLS;
    int x = bx + col * (cw + gap);
    int y = by + row * (ch + gap);
    const Card &cd = c_[i];

    if (cd.done) {
      // 已配对：淡绿底 + 绿边 + 图案（半透明感用暗一档的颜色表达）
      c.fillRectRound(x, y, cw, ch, 12, doneColor());
      c.strokeRect(x, y, cw, ch, 2, doneEdge());
      drawSymbol(c, cd.sym, x + cw / 2, y + ch / 2, cw / 4);
    } else if (cd.open) {
      // 翻开：亮底 + 图案
      c.fillRectRound(x, y, cw, ch, 12, rgba(240, 244, 250));
      drawSymbol(c, cd.sym, x + cw / 2, y + ch / 2, cw / 4);
    } else {
      // 背面：深色 + 中央菱形（不写字）
      c.fillRectRound(x, y, cw, ch, 12, backColor());
      c.strokeRect(x, y, cw, ch, 2, backEdge());
      int cx = x + cw / 2, cy = y + ch / 2, r = cw / 5;
      c.fillTriangle(cx, cy - r, cx - r, cy, cx + r, cy, backMark());
      c.fillTriangle(cx, cy + r, cx - r, cy, cx + r, cy, backMark());
    }
  }

  if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 165);
    c.textCenter(W / 2, H / 2 - 96, "全部配对完成", 4, rgba(255, 255, 255));
    char b[64];
    snprintf(b, sizeof(b), "共 %d 步", steps_);
    c.textCenter(W / 2, H / 2 - 20, b, 2, rgba(168, 140, 232));
    int sec = elapsedMs_ / 1000;
    snprintf(b, sizeof(b), "用时 %d 分 %d 秒", sec / 60, sec % 60);
    c.textCenter(W / 2, H / 2 + 16, b, 2, rgba(200, 210, 226));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕再来一局", 2, rgba(160, 160, 170));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 96, "记忆翻牌", 5, rgba(168, 140, 232));
    c.textCenter(W / 2, H / 2 - 20, "翻开两张相同图案", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 16, "全部配对即通关", 2, rgba(150, 170, 190));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool GameMemory::onTouch(int action, int x, int y) {
  if (over_) {
    if (action == PG_TOUCH_DOWN) {
      reset();
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;
  if (action != PG_TOUCH_DOWN) return true;
  if (hideMs_ > 0) return true;  // 正在判定，忽略新输入

  int idx = at(x, y);
  if (idx < 0) return true;
  Card &cd = c_[idx];
  if (cd.done || cd.open) return true;

  cd.open = true;
  sfx(SFX_MOVE);
  if (first_ < 0) {
    first_ = idx;
  } else {
    second_ = idx;
    ++steps_;
    hideMs_ = 760;  // 给玩家看清两张牌的时间
  }
  return true;
}

bool GameMemory::onKey(int key) {
  if (over_ && key == PG_KEY_A) {
    reset();
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (state_ == GSTATE_READY && key == PG_KEY_A) {
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    if (state_ == GSTATE_OVER) state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

const char *GameMemory::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", steps_);
  return buf;
}

const char *GameMemory::info2Value(char *buf, int n) const {
  int sec = elapsedMs_ / 1000;
  snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
  return buf;
}

const char *GameMemory::hint() const {
  if (over_) return "全部配对完成 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点两张相同的图案即可消除";
}

bool GameMemory::justGameOver() { return overFlag_; }
void GameMemory::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
