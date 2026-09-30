/*
 * PgSokoban.cpp - 推箱子（5 关，关卡已用 BFS 求解器验证可解）
 *
 * 操作：
 *   滑动屏幕 = 移动（滑动方向即移动方向）
 *   A = 撤销一步        C = 重开本关        B = 返回
 *   画布底部还有一排按钮：上一关 / 下一关 / 撤销 / 重来
 *
 * 关卡数据用经典字符表示：'#' 墙、' ' 地板、'.' 目标点、'$' 箱子、'@' 玩家、
 * '*' 箱子已在目标点、'+' 玩家站在目标点。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

const char *kLevels[] = {
    // 1: 一步入门
    "########\n"
    "#      #\n"
    "#  .   #\n"
    "#  $   #\n"
    "#  @   #\n"
    "#      #\n"
    "########",
    // 2: 两个箱子（最短 5 步）
    "#########\n"
    "#       #\n"
    "#  . .  #\n"
    "#  $ $  #\n"
    "#  @    #\n"
    "#       #\n"
    "#########",
    // 3: 竖直通道（最短 3 步）
    "#########\n"
    "#       #\n"
    "#  ###  #\n"
    "#  #.#  #\n"
    "#  # #  #\n"
    "#  #$#  #\n"
    "#  # #  #\n"
    "#   @   #\n"
    "#########",
    // 4: 绕行取位（最短 14 步）
    "##########\n"
    "#        #\n"
    "#  $  .  #\n"
    "#  #  #  #\n"
    "#  $  .  #\n"
    "#   @    #\n"
    "#        #\n"
    "##########",
    // 5: 进房间里推（最短 8 步）
    "##########\n"
    "#        #\n"
    "#  ####  #\n"
    "#  #  #  #\n"
    "#  # $#  #\n"
    "#  #  #  #\n"
    "#  # .#  #\n"
    "#  @     #\n"
    "##########",
};

const int kLevelCount = (int)(sizeof(kLevels) / sizeof(kLevels[0]));

const int MAX_CELL = 52;

}  // namespace

GameSokoban::GameSokoban()
    : w_(0),
      h_(0),
      px_(0),
      py_(0),
      level_(0),
      moves_(0),
      boxesTotal_(0),
      boxesDone_(0),
      undoTop_(0),
      win_(false),
      overFlag_(false),
      dragging_(false),
      downX_(0),
      downY_(0),
      moved_(0),
      cell_(MAX_CELL),
      bx_(0),
      by_(0) {
  memset(wall_, 0, sizeof(wall_));
  memset(goal_, 0, sizeof(goal_));
  memset(box_, 0, sizeof(box_));
}

const char *GameSokoban::title() const { return "推箱子"; }
const char *GameSokoban::desc() const { return "滑动把箱子推到目标点"; }
const char *GameSokoban::tag() const { return "BOX"; }
Color GameSokoban::theme() const { return rgba(198, 148, 92); }

void GameSokoban::reset() { loadLevel(level_); }

bool GameSokoban::loadLevel(int level) {
  if (level < 0) level = 0;
  if (level >= kLevelCount) level = kLevelCount - 1;
  level_ = level;
  memset(wall_, 0, sizeof(wall_));
  memset(goal_, 0, sizeof(goal_));
  memset(box_, 0, sizeof(box_));
  w_ = h_ = 0;
  px_ = py_ = 0;
  moves_ = 0;
  undoTop_ = 0;
  boxesTotal_ = boxesDone_ = 0;
  win_ = false;
  overFlag_ = false;

  const char *s = kLevels[level];
  int r = 0, c = 0;
  for (const char *p = s; *p; ++p) {
    char ch = *p;
    if (ch == '\n') {
      if (c > w_) w_ = c;
      ++r;
      c = 0;
      continue;
    }
    if (r >= MAXH || c >= MAXW) continue;
    switch (ch) {
      case '#':
        wall_[r][c] = 1;
        break;
      case '.':
        goal_[r][c] = 1;
        break;
      case '$':
        box_[r][c] = 1;
        ++boxesTotal_;
        break;
      case '*':
        goal_[r][c] = 1;
        box_[r][c] = 1;
        ++boxesTotal_;
        break;
      case '@':
        px_ = c;
        py_ = r;
        break;
      case '+':
        goal_[r][c] = 1;
        px_ = c;
        py_ = r;
        break;
      default:
        break;
    }
    ++c;
  }
  if (c > w_) w_ = c;
  h_ = r + 1;
  if (boxesDone_ == 0) {
    // 统计已在目标点上的箱子
    boxesDone_ = 0;
    for (int y = 0; y < h_; ++y)
      for (int x = 0; x < w_; ++x)
        if (box_[y][x] && goal_[y][x]) ++boxesDone_;
  }
  cell_ = MAX_CELL;
  return true;
}

bool GameSokoban::canWalk(int r, int c) const {
  if (r < 0 || r >= h_ || c < 0 || c >= w_) return false;
  return !wall_[r][c];
}

bool GameSokoban::boxAt(int r, int c) const {
  if (r < 0 || r >= h_ || c < 0 || c >= w_) return false;
  return box_[r][c] != 0;
}

int GameSokoban::boxCount() const {
  int n = 0;
  for (int y = 0; y < h_; ++y)
    for (int x = 0; x < w_; ++x)
      if (box_[y][x]) ++n;
  return n;
}

void GameSokoban::pushUndo(int dr, int dc) {
  if (undoTop_ >= MAXUNDO) {
    // 满了就整体前移一格，丢掉最老的一步
    memmove(&undo_[0], &undo_[1], sizeof(Pos) * (MAXUNDO - 1));
    undoTop_ = MAXUNDO - 1;
  }
  undo_[undoTop_].r = (int8_t)dr;
  undo_[undoTop_].c = (int8_t)dc;
  ++undoTop_;
}

bool GameSokoban::move(int dr, int dc) {
  int nr = py_ + dr;
  int nc = px_ + dc;
  if (!canWalk(nr, nc)) return false;
  int pushed = 0;
  if (boxAt(nr, nc)) {
    int br = nr + dr;
    int bc = nc + dc;
    if (!canWalk(br, bc) || boxAt(br, bc)) return false;  // 推不动
    box_[nr][nc] = 0;
    box_[br][bc] = 1;
    pushed = 1;
    bool wasOn = goal_[nr][nc] != 0;
    bool nowOn = goal_[br][bc] != 0;
    if (wasOn && !nowOn) --boxesDone_;
    if (!wasOn && nowOn) ++boxesDone_;
    sfx(nowOn ? SFX_SCORE : SFX_DROP);
  }
  (void)pushed;
  // 记录撤销：只记玩家方向 + 是否推了箱子（箱子位置可由方向反推）
  pushUndo(dr, dc);
  px_ = nc;
  py_ = nr;
  ++moves_;
  if (!boxAt(nr, nc)) sfx(SFX_MOVE);

  if (solved()) {
    win_ = true;
    overFlag_ = true;
    sfx(SFX_CLEAR);
  }
  return true;
}

bool GameSokoban::undo() {
  if (undoTop_ <= 0) return false;
  --undoTop_;
  int dr = undo_[undoTop_].r;
  int dc = undo_[undoTop_].c;
  // 玩家退回，若身后那格有箱子则把它拉回来
  int prevR = px_ - dr;
  int prevC = px_ - dc;
  int boxR = px_ + dr;
  int boxC = px_ + dc;
  if (boxAt(boxR, boxC)) {
    box_[boxR][boxC] = 0;
    box_[px_][py_] = 1;
    if (goal_[boxR][boxC] && !goal_[px_][py_]) --boxesDone_;
    if (!goal_[boxR][boxC] && goal_[px_][py_]) ++boxesDone_;
  }
  px_ = prevC;
  py_ = prevR;
  if (moves_ > 0) --moves_;
  win_ = false;
  sfx(SFX_CLICK);
  return true;
}

bool GameSokoban::solved() const {
  for (int y = 0; y < h_; ++y) {
    for (int x = 0; x < w_; ++x) {
      if (goal_[y][x] && !box_[y][x]) return false;
    }
  }
  return true;
}

void GameSokoban::update(int dtMs) { (void)dtMs; }

void GameSokoban::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.fillRect(0, 0, W, H, rgba(24, 20, 16));

  // 计算格子尺寸（底部留 60px 给按钮）
  int availH = H - 66;
  int cell = MAX_CELL;
  int cw = (W - 8) / (w_ > 0 ? w_ : 1);
  int chh = (availH - 8) / (h_ > 0 ? h_ : 1);
  if (cw < cell) cell = cw;
  if (chh < cell) cell = chh;
  if (cell < 10) cell = 10;
  int gw = w_ * cell, gh = h_ * cell;
  int bx = (W - gw) / 2;
  int by = (availH - gh) / 2;
  if (by < 4) by = 4;
  cell_ = cell;
  bx_ = bx;
  by_ = by;

  for (int y = 0; y < h_; ++y) {
    for (int x = 0; x < w_; ++x) {
      int px = bx + x * cell;
      int py = by + y * cell;
      if (wall_[y][x]) {
        c.fillRect(px, py, cell, cell, rgba(96, 84, 70));
        c.fillRect(px + 2, py + 2, cell - 4, cell / 3, rgba(122, 108, 90));
        continue;
      }
      // 地板（棋盘格）
      bool dark = ((x + y) & 1) != 0;
      c.fillRect(px, py, cell, cell, dark ? rgba(58, 50, 42) : rgba(66, 57, 48));
      if (goal_[y][x]) {
        int r2 = cell / 5;
        c.fillCircle(px + cell / 2, py + cell / 2, r2, rgba(96, 168, 110));
        c.fillCircle(px + cell / 2, py + cell / 2, r2 / 2, rgba(30, 40, 32));
      }
      if (box_[y][x]) {
        bool on = goal_[y][x] != 0;
        int pad = cell / 8;
        Color body = on ? rgba(110, 196, 120) : rgba(206, 152, 78);
        Color edge = on ? rgba(72, 150, 84) : rgba(160, 112, 52);
        c.fillRectRound(px + pad, py + pad, cell - pad * 2, cell - pad * 2,
                        cell / 6, body);
        c.fillRect(px + pad + 3, py + pad + 3, cell - pad * 2 - 6,
                   (cell - pad * 2) / 3, edge);
        c.strokeRect(px + pad, py + pad, cell - pad * 2, cell - pad * 2, 1,
                     rgba(40, 30, 20));
      }
    }
  }

  // 玩家
  {
    int cx = bx + px_ * cell + cell / 2;
    int cy = by + py_ * cell + cell / 2;
    int r2 = cell / 3;
    c.fillCircle(cx, cy, r2, rgba(238, 238, 244));
    c.fillCircle(cx, cy - r2 / 3, r2 / 2, rgba(90, 160, 235));
    c.fillCircle(cx - r2 / 3, cy - r2 / 3, 2, rgba(30, 30, 40));
    c.fillCircle(cx + r2 / 3, cy - r2 / 3, 2, rgba(30, 30, 40));
    c.fillRect(cx - r2 / 2, cy + r2 / 4, r2, 3, rgba(200, 80, 80));
  }

  // 底部按钮
  int bw = (W - 5 * 8) / 4;
  int byy = H - 54;
  struct Btn {
    const char *t;
    int x;
  } btns[4] = {{"上一关", 8},
               {"下一关", 8 + bw + 8},
               {"撤销", 8 + (bw + 8) * 2},
               {"重来", 8 + (bw + 8) * 3}};
  for (int i = 0; i < 4; ++i) {
    c.fillRectRound(btns[i].x, byy, bw, 44, 8, rgba(52, 44, 38));
    c.textCenterBox(btns[i].x, byy, bw, 44, btns[i].t, 2, rgba(232, 226, 214));
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 120, "推箱子", 3, rgba(226, 178, 108));
    c.textCenter(W / 2, H / 2 - 40, "滑动屏幕移动小人", 2, rgba(232, 232, 232));
    c.textCenter(W / 2, H / 2 - 4, "把箱子推到绿色目标点", 2, rgba(232, 232, 232));
    c.textCenter(W / 2, H / 2 + 50, "点击屏幕或按暂停键开始", 2, rgba(170, 170, 180));
  } else if (win_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 130);
    c.textCenter(W / 2, H / 2 - 90, "过关！", 3, rgba(120, 226, 160));
    char buf[64];
    snprintf(buf, sizeof(buf), "用了 %d 步", moves_);
    c.textCenter(W / 2, H / 2 - 20, buf, 2, rgba(250, 214, 78));
    if (level_ + 1 < kLevelCount) {
      c.textCenter(W / 2, H / 2 + 40, "点击屏幕进入下一关", 2,
                   rgba(224, 224, 224));
    } else {
      c.textCenter(W / 2, H / 2 + 40, "全部通关，厉害！", 2, rgba(224, 224, 224));
    }
  } else if (state_ == GSTATE_PAUSED) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 20, "已暂停", 3, rgba(255, 255, 255));
  }
}

bool GameSokoban::onTouch(int action, int x, int y) {
  if (win_) {
    if (action == PG_TOUCH_DOWN) {
      if (level_ + 1 < kLevelCount) {
        loadLevel(level_ + 1);
        sfx(SFX_CLICK);
      } else {
        loadLevel(0);
        state_ = GSTATE_RUNNING;
      }
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
  if (state_ == GSTATE_PAUSED) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;

  // 底部按钮
  if (action == PG_TOUCH_DOWN) {
    const int bw = (vw() - 5 * 8) / 4;
    int byy = vh() - 54;
    if (y >= byy && y <= byy + 44) {
      for (int i = 0; i < 4; ++i) {
        int bxp = 8 + (bw + 8) * i;
        if (x >= bxp && x <= bxp + bw) {
          if (i == 0 && level_ > 0) loadLevel(level_ - 1);
          else if (i == 1 && level_ + 1 < kLevelCount) loadLevel(level_ + 1);
          else if (i == 2) undo();
          else if (i == 3) loadLevel(level_);
          sfx(SFX_CLICK);
          return true;
        }
      }
    }
  }

  if (action == PG_TOUCH_DOWN) {
    dragging_ = true;
    downX_ = x;
    downY_ = y;
    moved_ = 0;
    return true;
  }
  if (action == PG_TOUCH_MOVE) {
    if (!dragging_) return true;
    // 异常跳变（触摸驱动噪声 / 注入抖动）只重同步基准，不产生位移
    // —— 与 2048 / 俄罗斯方块同一套保护（2026-09-13 检讨发现只有那两个有）
    if (abs(x - downX_) > 160 || abs(y - downY_) > 160) {
      downX_ = x;
      downY_ = y;
      return true;
    }
    int dx = x - downX_;
    int dy = y - downY_;
    int th = cell_ / 2;
    if (th < 22) th = 22;
    if (abs(dx) < th && abs(dy) < th) return true;
    if (abs(dx) > abs(dy)) {
      move(0, dx > 0 ? 1 : -1);
    } else {
      move(dy > 0 ? 1 : -1, 0);
    }
    downX_ = x;
    downY_ = y;
    ++moved_;
    return true;
  }
  if (action == PG_TOUCH_UP) {
    // 轻点 = 朝点击方向走一步（方便单指操作）
    if (dragging_ && moved_ == 0) {
      int cc = (x - bx_) / (cell_ > 0 ? cell_ : 1);
      int rr = (y - by_) / (cell_ > 0 ? cell_ : 1);
      int dx = cc - px_;
      int dy = rr - py_;
      if (abs(dx) + abs(dy) == 1) {
        move(dy, dx);
        sfx(SFX_CLICK);
      }
    }
    dragging_ = false;
    return true;
  }
  return true;
}

bool GameSokoban::onKey(int key) {
  if (key == PG_KEY_A) {
    if (win_) {
      if (level_ + 1 < kLevelCount) loadLevel(level_ + 1);
      return true;
    }
    if (state_ == GSTATE_READY) {
      state_ = GSTATE_RUNNING;
      return true;
    }
    undo();
    return true;
  }
  if (key == PG_KEY_C) {
    loadLevel(level_);
    return true;
  }
  return false;
}

const char *GameSokoban::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", moves_);
  return buf;
}

const char *GameSokoban::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", level_ + 1, kLevelCount);
  return buf;
}

const char *GameSokoban::hint() const {
  if (win_) return "过关 - 点击屏幕继续";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  return "滑动移动，把箱子推到绿色点";
}

const char *GameSokoban::keyBar() const {
  return "点底部按钮操作 · 长按 返回列表";
}

bool GameSokoban::justGameOver() { return overFlag_; }
void GameSokoban::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
