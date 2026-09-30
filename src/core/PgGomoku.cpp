/*
 * PgGomoku.cpp - 五子棋（点一下落子，内置轻量评分 AI）
 *
 * 为什么加它：原有 8 款里**没有棋类**；而"点一下落子"是触摸屏上最自然的交互之一。
 *
 * AI 策略：对每个"已有棋子附近 2 格内"的空点做**单点评分** ——
 *   value = 如果 AI 落这里形成的价值 + 玩家落这里的价值（拦他）
 * 连子数按 5/4/3/2 分级，端头是否被封决定是否减档。不做深搜（棋盘小、回合快，
 * 单层评分已经能挡住绝大多数菜鸟攻势，且每步只算一次、不会拖慢主循环）。
 *
 * 棋盘 13 路、每格约 37px；落子吸附到最近的交叉点（小屏上必须这样才好点）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

Color boardColor() { return rgba(198, 164, 110); }
Color lineColor() { return rgba(92, 72, 48); }
Color starColor() { return rgba(70, 54, 36); }
Color blackStone() { return rgba(28, 30, 36); }
Color whiteStone() { return rgba(244, 246, 250); }
Color lastMark() { return rgba(226, 76, 66); }

const int DX[4] = {1, 0, 1, 1};
const int DY[4] = {0, 1, 1, -1};

}  // namespace

GameGomoku::GameGomoku()
    : lastX_(-1),
      lastY_(-1),
      turns_(0),
      winner_(0),
      winCount_(0),
      thinkMs_(0),
      aiX_(-1),
      aiY_(-1),
      overFlag_(false) {
  memset(b_, 0, sizeof(b_));
}

const char *GameGomoku::title() const { return "五子棋"; }
const char *GameGomoku::desc() const { return "点一下落子，五子连珠获胜"; }
const char *GameGomoku::tag() const { return "GOMOKU"; }
Color GameGomoku::theme() const { return rgba(198, 164, 110); }

void GameGomoku::layout(int &cell, int &bx, int &by) const {
  const int margin = 16;
  int avail = vw() - 2 * margin;
  cell = avail / (N - 1);
  int total = cell * (N - 1);
  bx = (vw() - total) / 2;
  // 垂直方向：上方留 40px 给"开始/结束"提示，其余居中
  int top = 40;
  by = top + (vh() - top - total) / 2;
  if (by < top) by = top;
}

int GameGomoku::at(int x, int y, int &gx, int &gy) const {
  int cell, bx, by;
  layout(cell, bx, by);
  gx = (x - bx + cell / 2) / cell;
  gy = (y - by + cell / 2) / cell;
  if (gx < 0 || gx >= N || gy < 0 || gy >= N) return 0;
  // 离交叉点太远（> 0.55 格）视为误触，不落子
  int cx = bx + gx * cell, cy = by + gy * cell;
  int dx = x - cx, dy = y - cy;
  if (dx * dx + dy * dy > (cell * 55 / 100) * (cell * 55 / 100)) return 0;
  return 1;
}

bool GameGomoku::win(int x, int y, int who) const {
  for (int d = 0; d < 4; ++d) {
    int cnt = 1;
    for (int s = -1; s <= 1; s += 2) {
      for (int i = 1; i < 5; ++i) {
        int nx = x + DX[d] * i * s;
        int ny = y + DY[d] * i * s;
        if (nx < 0 || nx >= N || ny < 0 || ny >= N) break;
        if (b_[ny][nx] != who) break;
        ++cnt;
      }
    }
    if (cnt >= 5) return true;
  }
  return false;
}

int GameGomoku::lineScore(int x, int y, int who) const {
  int total = 0;
  for (int d = 0; d < 4; ++d) {
    int cnt = 1;
    int open = 0;  // 两端各是否有空位
    for (int s = -1; s <= 1; s += 2) {
      for (int i = 1; i < 5; ++i) {
        int nx = x + DX[d] * i * s;
        int ny = y + DY[d] * i * s;
        if (nx < 0 || nx >= N || ny < 0 || ny >= N) break;
        if (b_[ny][nx] == who) {
          ++cnt;
        } else {
          if (b_[ny][nx] == 0) ++open;
          break;
        }
      }
    }
    if (cnt >= 5) total += 1000000;
    else if (cnt == 4) total += (open == 2) ? 120000 : (open == 1 ? 15000 : 0);
    else if (cnt == 3) total += (open == 2) ? 9000 : (open == 1 ? 900 : 0);
    else if (cnt == 2) total += (open == 2) ? 700 : (open == 1 ? 90 : 0);
    else total += open * 14;
  }
  return total;
}

void GameGomoku::reset() {
  memset(b_, 0, sizeof(b_));
  lastX_ = lastY_ = -1;
  turns_ = 0;
  winner_ = 0;
  thinkMs_ = 0;
  aiX_ = aiY_ = -1;
  overFlag_ = false;
}

void GameGomoku::aiTurn() {
  // 玩家第一手若下在中心附近，AI 就贴着下；否则走通用评分
  int bestX = -1, bestY = -1;
  int bestV = -1;
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      if (b_[y][x]) continue;
      // 只看已有棋子附近，空盘心区域也能开局
      bool near = false;
      for (int dy = -2; dy <= 2 && !near; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
          if (b_[ny][nx]) {
            near = true;
            break;
          }
        }
      }
      if (!near && turns_ > 1) continue;

      int atk = lineScore(x, y, 2);  // 自己落这里的价值
      int def = lineScore(x, y, 1);  // 挡玩家的价值
      // 略偏进攻（11:10），但对方成四/成五时必须优先拦
      int v = atk * 11 / 10 + def;
      if (v > bestV) {
        bestV = v;
        bestX = x;
        bestY = y;
      }
    }
  }
  if (bestX < 0) {  // 兜底：找第一个空位
    for (int y = 0; y < N && bestX < 0; ++y)
      for (int x = 0; x < N; ++x)
        if (!b_[y][x]) {
          bestX = x;
          bestY = y;
          break;
        }
  }
  if (bestX < 0) return;

  b_[bestY][bestX] = 2;
  lastX_ = bestX;
  lastY_ = bestY;
  ++turns_;
  sfx(SFX_DROP);
  if (win(bestX, bestY, 2)) {
    winner_ = 2;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    sfx(SFX_OVER);
  } else if (turns_ >= N * N) {
    winner_ = 3;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    sfx(SFX_OVER);
  }
}

void GameGomoku::update(int dtMs) {
  if (state_ != GSTATE_RUNNING || winner_) return;
  if (thinkMs_ > 0) {
    thinkMs_ -= dtMs;
    if (thinkMs_ <= 0) {
      thinkMs_ = 0;
      aiTurn();
    }
  }
}

void GameGomoku::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.clear(rgba(26, 30, 38));

  int cell, bx, by;
  layout(cell, bx, by);
  int total = cell * (N - 1);

  // 棋盘底
  c.fillRectRound(bx - cell / 2, by - cell / 2, total + cell, total + cell, 8,
                  boardColor());
  // 网格
  for (int i = 0; i < N; ++i) {
    c.hline(bx, bx + total, by + i * cell, lineColor());
    c.vline(bx + i * cell, by, by + total, lineColor());
  }
  // 星位（13 路的常见 5 个点）
  const int stars[5][2] = {{3, 3}, {9, 3}, {3, 9}, {9, 9}, {6, 6}};
  for (int i = 0; i < 5; ++i) {
    c.fillCircle(bx + stars[i][0] * cell, by + stars[i][1] * cell, 4,
                 starColor());
  }

  // 棋子
  int r = cell * 45 / 100;
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      int v = b_[y][x];
      if (!v) continue;
      int cx = bx + x * cell, cy = by + y * cell;
      Color col = (v == 1) ? blackStone() : whiteStone();
      c.fillCircle(cx, cy, r, col);
      // 高光（左上偏亮），让棋子有立体感
      c.fillCircle(cx - r / 3, cy - r / 3, r / 3,
                   (v == 1) ? rgba(96, 104, 118) : rgba(255, 255, 255));
    }
  }

  // 最后一手标记
  if (lastX_ >= 0) {
    c.strokeCircle(bx + lastX_ * cell, by + lastY_ * cell, r + 5, 3, lastMark());
  }

  if (winner_ || state_ == GSTATE_READY) {
    if (state_ == GSTATE_READY && !winner_) {
      c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
      c.textCenter(W / 2, H / 2 - 96, "五子棋", 5, rgba(198, 164, 110));
      c.textCenter(W / 2, H / 2 - 20, "你先手（黑子）", 2, rgba(230, 230, 230));
      c.textCenter(W / 2, H / 2 + 16, "点交叉点落子", 2, rgba(150, 170, 190));
      c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
    } else if (winner_) {
      c.blendRect(0, 0, W, H, rgba(0, 0, 0), 165);
      const char *t = (winner_ == 1) ? "你赢了！" : (winner_ == 2 ? "AI 获胜" : "和棋");
      Color col = (winner_ == 1) ? rgba(120, 226, 160) : rgba(226, 96, 88);
      c.textCenter(W / 2, H / 2 - 96, t, 4, col);
      char b[64];
      snprintf(b, sizeof(b), "共 %d 手", turns_);
      c.textCenter(W / 2, H / 2 - 20, b, 2, rgba(210, 218, 230));
      snprintf(b, sizeof(b), "累计胜局 %d", winCount_);
      c.textCenter(W / 2, H / 2 + 16, b, 2, rgba(198, 164, 110));
      c.textCenter(W / 2, H / 2 + 60, "点击屏幕再来一局", 2, rgba(160, 160, 170));
    }
  }
}

bool GameGomoku::onTouch(int action, int x, int y) {
  if (winner_) {
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
  if (thinkMs_ > 0) return true;  // AI 思考中不接受落子

  int gx, gy;
  if (!at(x, y, gx, gy)) return true;
  if (b_[gy][gx]) {
    sfx(SFX_HIT);
    return true;
  }

  b_[gy][gx] = 1;
  lastX_ = gx;
  lastY_ = gy;
  ++turns_;
  sfx(SFX_DROP);

  if (win(gx, gy, 1)) {
    winner_ = 1;
    ++winCount_;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    sfx(SFX_OVER);
  } else if (turns_ >= N * N) {
    winner_ = 3;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    sfx(SFX_OVER);
  } else {
    thinkMs_ = 280;  // AI "思考"一下再落子，比瞬间落子更像对手
  }
  return true;
}

bool GameGomoku::onKey(int key) {
  if (winner_ && key == PG_KEY_A) {
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

const char *GameGomoku::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", turns_);
  return buf;
}

const char *GameGomoku::info2Value(char *buf, int n) const {
  if (winner_ == 1) return "你赢了";
  if (winner_ == 2) return "AI 胜";
  if (winner_ == 3) return "和棋";
  if (thinkMs_ > 0) return "AI 思考";
  if (state_ == GSTATE_READY) return "待开始";
  return "轮到你";
}

const char *GameGomoku::hint() const {
  if (winner_ == 1) return "五子连珠，你赢了 - 点击屏幕再来一局";
  if (winner_ == 2) return "AI 连成五子 - 点击屏幕再来一局";
  if (winner_ == 3) return "棋盘已满，和棋 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  if (thinkMs_ > 0) return "AI 正在思考…";
  return "点交叉点落子，先连成五子者胜";
}

bool GameGomoku::justGameOver() { return overFlag_; }
void GameGomoku::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
