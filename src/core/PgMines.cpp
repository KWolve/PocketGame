/*
 * PgMines.cpp - 扫雷（9x9 / 10 雷）
 *
 * 操作：
 *   点格子 = 挖开（插旗模式下 = 插旗/取消）
 *   长按格子（>450ms）= 插旗/取消（不用切模式）
 *   A = 切换「插旗 / 挖开」模式      C = 重新开始      B = 返回
 *
 * 首点安全：第一次挖开的那格及其 8 邻域都不会有雷（点开即一片，体验好）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

// 数字 1..8 的配色（经典扫雷色）
Color numColor(int n) {
  switch (n) {
    case 1: return rgba(90, 150, 255);
    case 2: return rgba(70, 190, 120);
    case 3: return rgba(235, 95, 95);
    case 4: return rgba(150, 110, 230);
    case 5: return rgba(220, 150, 60);
    case 6: return rgba(70, 195, 200);
    case 7: return rgba(220, 220, 220);
    default: return rgba(190, 190, 200);
  }
}

const int LONG_PRESS_MS = 450;

}  // namespace

GameMines::GameMines()
    : opened_(0),
      flags_(0),
      elapsedMs_(0),
      boomMs_(0),
      downR_(-1),
      downC_(-1),
      downMs_(-1),
      over_(false),
      overFlag_(false),
      win_(false),
      firstClick_(true),
      flagMode_(false),
      cell_(CELL),
      bx_(0),
      by_(0) {
  memset(cells_, 0, sizeof(cells_));
}

const char *GameMines::title() const { return "扫雷"; }
const char *GameMines::desc() const { return "点开格子，避开 10 颗雷"; }
const char *GameMines::tag() const { return "MINES"; }
Color GameMines::theme() const { return rgba(120, 150, 210); }

void GameMines::reset() {
  memset(cells_, 0, sizeof(cells_));
  opened_ = 0;
  flags_ = 0;
  elapsedMs_ = 0;
  boomMs_ = 0;
  downR_ = downC_ = -1;
  downMs_ = -1;
  over_ = false;
  overFlag_ = false;
  win_ = false;
  firstClick_ = true;
  flagMode_ = false;
}

int GameMines::minesLeft() const { return MINES - flags_; }

void GameMines::generate(int safeR, int safeC) {
  int placed = 0;
  while (placed < MINES) {
    int r = rand() % ROWS;
    int c = rand() % COLS;
    // 首点安全区：3x3
    if (abs(r - safeR) <= 1 && abs(c - safeC) <= 1) continue;
    Cell &cl = cells_[idx(r, c)];
    if (cl.mine) continue;
    cl.mine = 1;
    ++placed;
  }
  // 计算邻域雷数
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      int n = 0;
      for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
          if (!dr && !dc) continue;
          int rr = r + dr, cc = c + dc;
          if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) continue;
          if (cells_[idx(rr, cc)].mine) ++n;
        }
      }
      cells_[idx(r, c)].near = (uint8_t)n;
    }
  }
}

void GameMines::reveal(int r, int c) {
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
  Cell &cl = cells_[idx(r, c)];
  if (cl.open || cl.flag) return;
  cl.open = 1;
  ++opened_;
  if (cl.near != 0) return;
  // 空白格：递归展开 8 邻域（用显式栈，深度可控）
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <= 1; ++dc) {
      if (!dr && !dc) continue;
      reveal(r + dr, c + dc);
    }
  }
}

void GameMines::openAll() {
  for (int i = 0; i < MAXCELL; ++i) {
    if (cells_[i].mine) cells_[i].open = 1;
  }
}

bool GameMines::checkWin() {
  // 非雷格全开 = 胜
  return opened_ >= (MAXCELL - MINES);
}

void GameMines::layout(int &bx, int &by, int &cell) const {
  cell = CELL;
  int w = COLS * cell;
  int h = ROWS * cell;
  if (w > vw() - 8) {
    cell = (vw() - 8) / COLS;
    w = COLS * cell;
  }
  if (h > vh() - 8) {
    int c2 = (vh() - 8) / ROWS;
    if (c2 < cell) {
      cell = c2;
      w = COLS * cell;
      h = ROWS * cell;
    }
  }
  bx = (vw() - w) / 2;
  by = (vh() - h) / 2;
}

void GameMines::update(int dtMs) {
  if (boomMs_ > 0) boomMs_ -= dtMs;
  if (over_) return;
  if (!isPlaying()) return;
  elapsedMs_ += dtMs;
  // 长按判定（按住 450ms 没松手 = 插旗）
  if (downMs_ >= 0) {
    downMs_ += dtMs;
    if (downMs_ >= LONG_PRESS_MS && downR_ >= 0) {
      Cell &cl = cells_[idx(downR_, downC_)];
      if (!cl.open) {
        cl.flag = cl.flag ? 0 : 1;
        flags_ += cl.flag ? 1 : -1;
        sfx(SFX_MOVE);
      }
      downR_ = downC_ = -1;  // 消费掉这次按下
      downMs_ = -1;
    }
  }
}

void GameMines::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.fillRect(0, 0, W, H, rgba(16, 20, 28));

  int bx, by, cell;
  layout(bx, by, cell);
  bx_ = bx;
  by_ = by;
  cell_ = cell;

  // 底板
  int gw = COLS * cell, gh = ROWS * cell;
  c.fillRect(bx - 4, by - 4, gw + 8, gh + 8, rgba(28, 36, 48));

  for (int r = 0; r < ROWS; ++r) {
    for (int cc = 0; cc < COLS; ++cc) {
      const Cell &cl = cells_[idx(r, cc)];
      int x = bx + cc * cell;
      int y = by + r * cell;
      int pad = 2;
      int iw = cell - pad * 2;
      if (!cl.open) {
        // 未挖开：凸起方块
        c.fillRectRound(x + pad, y + pad, iw, iw, 6, rgba(72, 92, 122));
        c.fillRectRound(x + pad + 2, y + pad + 2, iw - 4, iw / 3, 4,
                        rgba(96, 120, 156));
        if (cl.flag) {
          int cx = x + cell / 2;
          int cy = y + cell / 2;
          c.fillRect(cx - 2, cy - 11, 3, 22, rgba(230, 230, 235));  // 杆
          c.fillTriangle(cx + 1, cy - 11, cx + 13, cy - 6, cx + 1, cy - 1,
                         rgba(228, 78, 72));  // 旗
          c.fillRect(cx - 8, cy + 9, 17, 4, rgba(120, 130, 150));  // 底座
        }
      } else {
        // 已挖开：凹陷格子
        c.fillRectRound(x + pad, y + pad, iw, iw, 6, rgba(40, 50, 64));
        if (cl.mine) {
          int cx = x + cell / 2;
          int cy = y + cell / 2;
          int rr = cell / 5;
          bool hit = (boomMs_ > 0);
          Color mc = hit ? rgba(255, 120, 100) : rgba(210, 90, 84);
          c.fillCircle(cx, cy, rr, mc);
          c.fillRect(cx - rr - 3, cy - 1, (rr + 3) * 2, 3, mc);
          c.fillRect(cx - 1, cy - rr - 3, 3, (rr + 3) * 2, mc);
          c.fillCircle(cx - rr / 3, cy - rr / 3, rr / 4, rgba(255, 255, 255));
        } else if (cl.near) {
          // 大号点阵：数字只有 1 位，按格子自动选档。
          // ⚠️ 高度约束给到 `cell`（而不是 cell-8）：bigTextH 是**字格**高（24*scale），
          //    而数字墨迹在格内居中、只占其中 3/4 —— 按字格卡会把 50px 格子里的 2 档挤掉。
          char b[4];
          snprintf(b, sizeof(b), "%d", cl.near);
          int sc = c.fitBigText(cell - 8, cell, b, 2);
          int tw = c.bigTextW(b, sc);
          int th = c.bigTextH(sc);
          c.bigText(x + (cell - tw) / 2, y + (cell - th) / 2, b, sc,
                    numColor(cl.near));
        }
      }
    }
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 110);
    c.textCenter(W / 2, H / 2 - 110, "扫雷", 3, rgba(150, 185, 245));
    c.textCenter(W / 2, H / 2 - 30, "点开格子，避开 10 颗雷", 2,
                 rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 6, "长按格子插旗标记", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  } else if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    char buf[64];
    if (win_) {
      c.textCenter(W / 2, H / 2 - 90, "全部排完，通关！", 2, rgba(120, 226, 160));
    } else {
      c.textCenter(W / 2, H / 2 - 90, "踩到雷了", 2, rgba(255, 130, 120));
    }
    snprintf(buf, sizeof(buf), "用时 %d 秒", elapsedMs_ / 1000);
    c.textCenter(W / 2, H / 2 - 30, buf, 2, rgba(250, 214, 78));
    c.textCenter(W / 2, H / 2 + 20, "点击屏幕再来一局", 2,
                 rgba(220, 220, 220));
  } else if (!isPlaying() && state_ == GSTATE_PAUSED) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 20, "已暂停", 3, rgba(255, 255, 255));
  }
}

bool GameMines::onTouch(int action, int x, int y) {
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

  // 命中格子
  int cc = (x - bx_) / (cell_ > 0 ? cell_ : 1);
  int rr = (y - by_) / (cell_ > 0 ? cell_ : 1);
  bool in = (rr >= 0 && rr < ROWS && cc >= 0 && cc < COLS) &&
            (x >= bx_ && y >= by_);

  if (action == PG_TOUCH_DOWN) {
    if (!in) return true;
    downR_ = rr;
    downC_ = cc;
    downMs_ = 0;
    return true;
  }
  if (action == PG_TOUCH_UP) {
    int wasR = downR_, wasC = downC_;
    downR_ = downC_ = -1;
    downMs_ = -1;
    if (!in || wasR < 0 || wasR != rr || wasC != cc) return true;
    Cell &cl = cells_[idx(rr, cc)];
    if (flagMode_) {
      if (!cl.open) {
        cl.flag = cl.flag ? 0 : 1;
        flags_ += cl.flag ? 1 : -1;
        sfx(SFX_MOVE);
      }
      return true;
    }
    if (cl.flag) return true;  // 已插旗，先取消再挖
    if (firstClick_) {
      firstClick_ = false;
      generate(rr, cc);
    }
    if (cl.mine) {
      cl.open = 1;
      openAll();
      over_ = true;
      overFlag_ = true;
      win_ = false;
      boomMs_ = 400;
      state_ = GSTATE_OVER;
      sfx(SFX_OVER);
      return true;
    }
    reveal(rr, cc);
    sfx(SFX_MOVE);
    if (checkWin()) {
      over_ = true;
      overFlag_ = true;
      win_ = true;
      state_ = GSTATE_OVER;
      sfx(SFX_SCORE);
    }
    return true;
  }
  return true;
}

bool GameMines::onKey(int key) {
  if (key == PG_KEY_A) {
    if (over_) {
      reset();
      state_ = GSTATE_RUNNING;
      return true;
    }
    flagMode_ = !flagMode_;
    sfx(SFX_CLICK);
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    if (state_ == GSTATE_OVER) state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

const char *GameMines::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", minesLeft());
  return buf;
}

const char *GameMines::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", elapsedMs_ / 1000);
  return buf;
}

const char *GameMines::hint() const {
  if (over_) return win_ ? "通关 - 点击屏幕再来一局" : "踩雷 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (flagMode_) return "插旗模式 - 点格子插旗";
  return "点格子挖开，长按插旗";
}

bool GameMines::justGameOver() { return overFlag_; }
void GameMines::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GameMines::stillFrame() const {
  return stillUnless(boomMs_);
}

}  // namespace pg
