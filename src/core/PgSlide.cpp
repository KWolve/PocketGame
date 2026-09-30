/*
 * PgSlide.cpp - 数字华容道（15 格推盘）
 *
 * 为什么加它：**零学习成本的触摸玩法** —— 点与空格相邻的块就滑过去，
 * 不需要教程、不需要手势约定，老人小孩都能上手。
 *
 * 盘面生成：从"已完成"状态开始做 200 次随机合法移动 —— 这样得到的状态
 *   **必定有解**（直接随机打乱有一半概率是无解盘面，是这类游戏最常见的 bug）。
 *
 * 数字用画布点阵字体渲染：cell 约 102px，scale 取 4（32x48），
 * 后续"大号数字点阵"落地后可换成更清晰的一档（见 docs/games-review.md）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

Color bgColor() { return rgba(22, 26, 34); }
Color tileColor() { return rgba(58, 72, 94); }
Color tileEdge() { return rgba(84, 104, 134); }
Color tileHi() { return rgba(76, 94, 120); }
Color doneColor() { return rgba(46, 112, 84); }
Color doneEdge() { return rgba(110, 214, 152); }
Color numColor() { return rgba(240, 246, 252); }

}  // namespace

GameSlide::GameSlide()
    : empty_(CELLS - 1),
      steps_(0),
      elapsedMs_(0),
      won_(false),
      overFlag_(false),
      fxMs_(0) {
  memset(t_, 0, sizeof(t_));
}

const char *GameSlide::title() const { return "数字华容道"; }
const char *GameSlide::desc() const { return "点相邻数字滑动，排成 1 到 15"; }
const char *GameSlide::tag() const { return "SLIDE"; }
Color GameSlide::theme() const { return rgba(94, 152, 214); }

void GameSlide::layout(int &cell, int &bx, int &by) const {
  const int gap = 10;
  const int margin = 20;
  int avail = vw() - 2 * margin;
  cell = (avail - (N - 1) * gap) / N;
  int total = N * cell + (N - 1) * gap;
  bx = (vw() - total) / 2;
  by = 50;
}

void GameSlide::newBoard() {
  int guard = 0;
  do {
    for (int i = 0; i < CELLS - 1; ++i) t_[i] = i + 1;
    t_[CELLS - 1] = 0;
    empty_ = CELLS - 1;
    // 随机合法移动打乱（保证可解）
    for (int k = 0; k < 240; ++k) {
      int r = empty_ / N, c = empty_ % N;
      int cand[4], n = 0;
      if (r > 0) cand[n++] = empty_ - N;
      if (r < N - 1) cand[n++] = empty_ + N;
      if (c > 0) cand[n++] = empty_ - 1;
      if (c < N - 1) cand[n++] = empty_ + 1;
      if (n == 0) break;
      int pick = cand[rand() % n];
      t_[empty_] = t_[pick];
      t_[pick] = 0;
      empty_ = pick;
    }
    ++guard;
  } while (solved() && guard < 8);  // 极小概率打回原样，避免开局即通关
}

void GameSlide::reset() {
  newBoard();
  steps_ = 0;
  elapsedMs_ = 0;
  won_ = false;
  overFlag_ = false;
  fxMs_ = 0;
}

bool GameSlide::solved() const {
  for (int i = 0; i < CELLS - 1; ++i)
    if (t_[i] != i + 1) return false;
  return t_[CELLS - 1] == 0;
}

int GameSlide::at(int x, int y) const {
  int cell, bx, by;
  layout(cell, bx, by);
  const int gap = 10;
  if (x < bx || y < by) return -1;
  int col = (x - bx) / (cell + gap);
  int row = (y - by) / (cell + gap);
  if (col < 0 || col >= N || row < 0 || row >= N) return -1;
  int lx = (x - bx) - col * (cell + gap);
  int ly = (y - by) - row * (cell + gap);
  if (lx >= cell || ly >= cell) return -1;
  return row * N + col;
}

bool GameSlide::canMove(int idx) const {
  int r1 = idx / N, c1 = idx % N;
  int r2 = empty_ / N, c2 = empty_ % N;
  return (r1 == r2 && (c1 - c2 == 1 || c2 - c1 == 1)) ||
         (c1 == c2 && (r1 - r2 == 1 || r2 - r1 == 1));
}

void GameSlide::update(int dtMs) {
  if (fxMs_ > 0) fxMs_ -= dtMs;
  if (won_ || state_ != GSTATE_RUNNING) return;
  elapsedMs_ += dtMs;
}

void GameSlide::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.clear(bgColor());

  int cell, bx, by;
  layout(cell, bx, by);
  const int gap = 10;

  for (int i = 0; i < CELLS; ++i) {
    int col = i % N, row = i / N;
    int x = bx + col * (cell + gap);
    int y = by + row * (cell + gap);
    int v = t_[i];
    if (v == 0) continue;  // 空格不画

    c.fillRectRound(x, y, cell, cell, 12, tileColor());
    c.strokeRect(x, y, cell, cell, 2, tileEdge());
    // 顶部一条高光，给出"块"的厚度感
    c.fillRectRound(x + 6, y + 6, cell - 12, 5, 3, tileHi());

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", v);
    // 大号点阵：显示同样大小只需放大 2~3 倍（原来 8x12 要放大 4 倍，台阶 4px）。
    // 留边只给 6px：15-puzzle 的数字本来就该撑满格子（cell=102 -> 2 位数正好 96 宽）。
    int sc = c.fitBigText(cell - 6, cell - 6, buf, 3);
    int tw = c.bigTextW(buf, sc);
    int th = c.bigTextH(sc);
    c.bigText(x + (cell - tw) / 2, y + (cell - th) / 2, buf, sc, numColor());
  }

  if (won_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 160);
    c.textCenter(W / 2, H / 2 - 96, "排好了！", 4, rgba(120, 226, 160));
    char b[64];
    snprintf(b, sizeof(b), "共 %d 步", steps_);
    c.textCenter(W / 2, H / 2 - 20, b, 2, rgba(210, 218, 230));
    int sec = elapsedMs_ / 1000;
    snprintf(b, sizeof(b), "用时 %d 分 %d 秒", sec / 60, sec % 60);
    c.textCenter(W / 2, H / 2 + 16, b, 2, rgba(150, 170, 190));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕再来一局", 2, rgba(160, 160, 170));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 96, "数字华容道", 5, rgba(94, 152, 214));
    c.textCenter(W / 2, H / 2 - 20, "点空格旁边的数字", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 16, "排成 1 到 15 即通关", 2, rgba(150, 170, 190));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool GameSlide::onTouch(int action, int x, int y) {
  if (won_) {
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

  int idx = at(x, y);
  if (idx < 0) return true;
  if (idx == empty_ || !canMove(idx)) {
    sfx(SFX_HIT);
    return true;
  }

  t_[empty_] = t_[idx];
  t_[idx] = 0;
  empty_ = idx;
  ++steps_;
  sfx(SFX_MOVE);

  if (solved()) {
    won_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    fxMs_ = 900;
    sfx(SFX_OVER);
  }
  return true;
}

bool GameSlide::onKey(int key) {
  if (won_ && key == PG_KEY_A) {
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

const char *GameSlide::info1Value(char *buf, int n) const {
  int sec = elapsedMs_ / 1000;
  snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
  return buf;
}

const char *GameSlide::info2Value(char *buf, int n) const {
  if (won_) return "已完成";
  if (state_ == GSTATE_READY) return "待开始";
  int right = 0;
  for (int i = 0; i < CELLS - 1; ++i)
    if (t_[i] == i + 1) ++right;
  snprintf(buf, n, "对 %d/15", right);
  return buf;
}

const char *GameSlide::hint() const {
  if (won_) return "已排好 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点与空格相邻的数字即可滑动";
}

bool GameSlide::justGameOver() { return overFlag_; }
void GameSlide::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
