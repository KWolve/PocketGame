/*
 * PgTetris.cpp - 俄罗斯方块（触摸：左右拖动移动 / 下拖加速 / 上滑直落 / 轻点旋转）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

const int BOARD_W = 10;
const int BOARD_H = 20;

/* ---- 手势阈值（都不是随便定的，改之前先读注释）----
 * SOFT_DROP_DY  下拖多少像素算"软降一步"
 * HARD_DROP_DY  上滑多少像素算"直落"
 * HARD_DROP_DX  直落允许的横向漂移上限（超过就不算"真的往上滑"）
 * TAP_DX/TAP_DY 轻点（= 旋转）允许的最大位移
 */
const int SOFT_DROP_DY = 40;
const int HARD_DROP_DY = 90;
const int HARD_DROP_DX = 40;
const int TAP_DX = 14;
const int TAP_DY = 14;

// 7 种方块的颜色（I O T S Z J L）
const Color PIECE_COLORS[8] = {
    0,
    rgba(79, 195, 247),   // I 青
    rgba(242, 179, 61),   // O 黄
    rgba(186, 104, 200),  // T 紫
    rgba(76, 175, 80),    // S 绿
    rgba(239, 83, 80),    // Z 红
    rgba(66, 133, 244),   // J 蓝
    rgba(255, 138, 101),  // L 橙
};

// 每种方块的 4 个旋转态，每个旋转态 4 个格子的 (dx,dy)
// 用 4x4 位图描述更稳妥：0..6 号方块，每块 4 个旋转
// 这里用坐标表（标准 Super Rotation System 近似）
struct Cell {
  int x, y;
};
const Cell SHAPES[7][4][4] = {
    // I
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
     {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
     {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
     {{1, 0}, {1, 1}, {1, 2}, {1, 3}}},
    // O
    {{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}}},
    // T
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
     {{1, 0}, {0, 1}, {1, 1}, {1, 2}}},
    // S
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
     {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
     {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
     {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
    // Z
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
     {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
     {{1, 0}, {0, 1}, {1, 1}, {0, 2}}},
    // J
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
     {{1, 0}, {1, 1}, {0, 2}, {1, 2}}},
    // L
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
     {{0, 0}, {1, 0}, {1, 1}, {1, 2}}},
};

Color bgColor() { return rgba(18, 20, 26); }
Color fieldBg() { return rgba(28, 32, 40); }
Color gridColor() { return rgba(38, 44, 54); }
Color panelBg() { return rgba(24, 28, 36); }

}  // namespace

GameTetris::GameTetris()
    : curType_(0),
      curRot_(0),
      curX_(0),
      curY_(0),
      nextType_(0),
      bagIdx_(7),
      score_(0),
      lines_(0),
      level_(1),
      dropAcc_(0),
      over_(false),
      overFlag_(false),
      flashCount_(0),
      flashMs_(0),
      pendingClear_(false),
      lockDelay_(0),
      lastClearScore_(0),
      dragging_(false),
      downX_(0),
      downY_(0),
      lastX_(0),
      lastY_(0),
      pressMs_(0),
      softN_(0),
      consumed_(false),
      moveStep_(0),
      softDropAcc_(0) {
  memset(board_, 0, sizeof(board_));
  memset(bag_, 0, sizeof(bag_));
  memset(flashLines_, 0, sizeof(flashLines_));
}

const char *GameTetris::title() const { return "俄罗斯方块"; }
const char *GameTetris::desc() const { return "拖动方块，填满整行消除"; }
const char *GameTetris::tag() const { return "TETRIS"; }
Color GameTetris::theme() const { return rgba(79, 195, 247); }

void GameTetris::reset() {
  memset(board_, 0, sizeof(board_));
  score_ = 0;
  lines_ = 0;
  level_ = 1;
  dropAcc_ = 0;
  over_ = false;
  overFlag_ = false;
  flashCount_ = 0;
  flashMs_ = 0;
  pendingClear_ = false;
  lockDelay_ = 0;
  bagIdx_ = 7;
  dragging_ = false;
  consumed_ = false;
  moveStep_ = 0;
  softDropAcc_ = 0;
  nextType_ = rand() % 7;
  newPiece();
}

void GameTetris::newPiece() {
  curType_ = nextType_;
  nextType_ = rand() % 7;
  curRot_ = 0;
  curX_ = 3;
  curY_ = -1;
  dropAcc_ = 0;
  lockDelay_ = 0;
  if (!fits(curType_, curRot_, curX_, curY_)) {
    curY_ = 0;
    if (!fits(curType_, curRot_, curX_, curY_)) {
      over_ = true;
      overFlag_ = true;
      state_ = GSTATE_OVER;
      sfx(SFX_OVER);
      saveBestIfNeeded(score_);
    }
  }
}

void GameTetris::cellOf(int type, int rot, int idx, int &dx, int &dy) const {
  dx = SHAPES[type][rot & 3][idx].x;
  dy = SHAPES[type][rot & 3][idx].y;
}

bool GameTetris::fits(int type, int rot, int px, int py) const {
  for (int i = 0; i < 4; ++i) {
    int dx, dy;
    cellOf(type, rot, i, dx, dy);
    int x = px + dx;
    int y = py + dy;
    if (x < 0 || x >= CW) return false;
    if (y >= CH) return false;
    if (y >= 0 && board_[y][x]) return false;
  }
  return true;
}

bool GameTetris::testMove(int dx, int dy, int drot) {
  int nr = (curRot_ + drot) & 3;
  int nx = curX_ + dx;
  int ny = curY_ + dy;
  if (fits(curType_, nr, nx, ny)) {
    curRot_ = nr;
    curX_ = nx;
    curY_ = ny;
    return true;
  }
  // 旋转贴墙微调
  if (drot != 0) {
    static const int kick[4] = {-1, 1, -2, 2};
    for (int k = 0; k < 4; ++k) {
      if (fits(curType_, nr, nx + kick[k], ny)) {
        curRot_ = nr;
        curX_ = nx + kick[k];
        curY_ = ny;
        return true;
      }
    }
  }
  return false;
}

void GameTetris::lockPiece() {
  for (int i = 0; i < 4; ++i) {
    int dx, dy;
    cellOf(curType_, curRot_, i, dx, dy);
    int x = curX_ + dx;
    int y = curY_ + dy;
    if (y >= 0 && y < CH && x >= 0 && x < CW) {
      board_[y][x] = (uint8_t)(curType_ + 1);
    }
  }
  // 找满行（先闪一下，150ms 后在 update 里真正消掉）
  flashCount_ = 0;
  for (int y = 0; y < CH; ++y) {
    bool ok = true;
    for (int x = 0; x < CW; ++x)
      if (!board_[y][x]) {
        ok = false;
        break;
      }
    if (ok && flashCount_ < 4) flashLines_[flashCount_++] = y;
  }
  if (flashCount_ > 0) {
    flashMs_ = 0;
    pendingClear_ = true;
    sfx(SFX_CLEAR);
  } else {
    sfx(SFX_DROP);
    newPiece();
  }
}

void GameTetris::applyClear() {
  int n = flashCount_;
  for (int i = 0; i < n; ++i) {
    int y = flashLines_[i];
    for (int yy = y; yy > 0; --yy)
      for (int x = 0; x < CW; ++x) board_[yy][x] = board_[yy - 1][x];
    for (int x = 0; x < CW; ++x) board_[0][x] = 0;
  }
  static const int table[5] = {0, 100, 300, 500, 800};
  int gained = table[n] * level_;
  score_ += gained;
  lastClearScore_ = gained;
  lines_ += n;
  int newLevel = lines_ / 10 + 1;
  if (newLevel != level_) {
    level_ = newLevel;
    sfx(SFX_SCORE);
  }
  saveBestIfNeeded(score_);
  flashCount_ = 0;
  flashMs_ = 0;
}

int GameTetris::dropInterval() const {
  int v = 800 - (level_ - 1) * 70;
  if (v < 110) v = 110;
  return v;
}

void GameTetris::update(int dtMs) {
  if (!isPlaying() || over_) {
    return;
  }
  // 消行闪烁期间冻结重力
  if (pendingClear_) {
    flashMs_ += dtMs;
    if (flashMs_ >= 150) {
      pendingClear_ = false;
      applyClear();
      newPiece();
    }
    return;
  }

  dropAcc_ += dtMs;
  int iv = dropInterval();
  while (dropAcc_ >= iv) {
    dropAcc_ -= iv;
    if (fits(curType_, curRot_, curX_, curY_ + 1)) {
      ++curY_;
    } else {
      lockPiece();
      break;
    }
  }
}

void GameTetris::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.clear(bgColor());

  // 布局：左侧棋盘，右侧信息板
  // ⚠️ 底部要给软按钮留条带（本游戏实现了 softButtons，见下）——
  //    不留的话 20 行棋盘会一路顶到画布底、被按钮压住最后两行。
  const int availH = H - PG_SOFT_BAR_H;
  int pad = 10;
  int cell = (availH - 2 * pad) / BOARD_H;   // 高度受限
  int fw = cell * CW;
  int fh = cell * BOARD_H;
  int fx = 12;
  int fy = (availH - fh) / 2;
  if (fx + fw + 12 > W - 150) {
    // 右侧留给面板
  }
  int panelX = fx + fw + 12;
  int panelW = W - panelX - 12;
  if (panelW < 100) {
    cell = (W - 160 - 2 * pad) / CW;
    if (cell * BOARD_H > availH - 2 * pad) cell = (availH - 2 * pad) / BOARD_H;
    fw = cell * CW;
    fh = cell * BOARD_H;
    fy = (availH - fh) / 2;
    panelX = fx + fw + 12;
    panelW = W - panelX - 12;
  }

  // 棋盘底
  c.fillRectRound(fx - 4, fy - 4, fw + 8, fh + 8, 6, rgba(40, 46, 58));
  c.fillRect(fx, fy, fw, fh, fieldBg());
  for (int x = 1; x < CW; ++x) c.vline(fx + x * cell, fy, fy + fh - 1, gridColor());
  for (int y = 1; y < CH; ++y) c.hline(fx, fx + fw - 1, fy + y * cell, gridColor());

  // 已固定的块
  for (int y = 0; y < CH; ++y)
    for (int x = 0; x < CW; ++x) {
      if (!board_[y][x]) continue;
      Color col = PIECE_COLORS[board_[y][x]];
      c.fillRectRound(fx + x * cell + 1, fy + y * cell + 1, cell - 2, cell - 2,
                      3, col);
      c.fillRect(fx + x * cell + 2, fy + y * cell + 2, cell - 4, 2,
                 shade(col, 45));
    }

  // 消行闪烁
  if (flashCount_ > 0) {
    int alpha = 200 - (flashMs_ * 200 / 160);
    if (alpha < 0) alpha = 0;
    for (int i = 0; i < flashCount_; ++i) {
      c.blendRect(fx, fy + flashLines_[i] * cell, fw, cell,
                  rgba(255, 255, 255), alpha);
    }
  }

  // 幽灵落点
  if (isPlaying() && !over_) {
    int gy = curY_;
    while (fits(curType_, curRot_, curX_, gy + 1)) ++gy;
    for (int i = 0; i < 4; ++i) {
      int dx, dy;
      cellOf(curType_, curRot_, i, dx, dy);
      int x = curX_ + dx;
      int y = gy + dy;
      if (y >= 0) {
        c.strokeRect(fx + x * cell + 1, fy + y * cell + 1, cell - 2, cell - 2, 1,
                     rgba(120, 140, 165));
      }
    }
    // 当前块
    Color col = PIECE_COLORS[curType_ + 1];
    for (int i = 0; i < 4; ++i) {
      int dx, dy;
      cellOf(curType_, curRot_, i, dx, dy);
      int x = curX_ + dx;
      int y = curY_ + dy;
      if (y < 0) continue;
      c.fillRectRound(fx + x * cell + 1, fy + y * cell + 1, cell - 2, cell - 2, 3,
                      col);
      c.fillRect(fx + x * cell + 2, fy + y * cell + 2, cell - 4, 2,
                 shade(col, 45));
    }
  }

  // 右侧面板
  c.fillRectRound(panelX, fy - 4, panelW, fh + 8, 6, panelBg());
  int px = panelX + 10;
  int py = fy + 8;
  c.text(px, py, "NEXT", 2, rgba(140, 155, 175));
  py += 30;
  // 下一个方块预览（4x4 网格居中）
  int pv = cell < 18 ? cell : 18;
  int boxW = 4 * pv;
  int boxX = panelX + (panelW - boxW) / 2;
  c.fillRect(boxX, py, boxW, boxW, rgba(16, 18, 24));
  for (int i = 0; i < 4; ++i) {
    int dx, dy;
    cellOf(nextType_, 0, i, dx, dy);
    Color col = PIECE_COLORS[nextType_ + 1];
    c.fillRectRound(boxX + dx * pv + 1, py + dy * pv + 1, pv - 2, pv - 2, 2, col);
  }
  py += boxW + 22;

  char buf[32];
  c.text(px, py, "SCORE", 1, rgba(120, 135, 155));
  py += 14;
  snprintf(buf, sizeof(buf), "%d", score_);
  c.bigText(px, py, buf, 1, rgba(242, 179, 61));   // 大号点阵：同样 16px 宽/字符，笔画更细
  py += 32;
  c.text(px, py, "LEVEL", 1, rgba(120, 135, 155));
  py += 14;
  snprintf(buf, sizeof(buf), "%d", level_);
  c.bigText(px, py, buf, 1, rgba(79, 195, 247));
  py += 32;
  c.text(px, py, "LINES", 1, rgba(120, 135, 155));
  py += 14;
  snprintf(buf, sizeof(buf), "%d", lines_);
  c.bigText(px, py, buf, 1, rgba(200, 210, 225));
  py += 34;

  // 操作提示（面板底部）
  int hy = fy + fh - 78;
  c.text(px, hy, "轻点 旋转", 1, rgba(110, 125, 145));
  c.text(px, hy + 16, "拖动 左右移", 1, rgba(110, 125, 145));
  c.text(px, hy + 32, "下拖 加速", 1, rgba(110, 125, 145));
  c.text(px, hy + 48, "上滑 直落", 1, rgba(110, 125, 145));

  if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    c.textCenter(W / 2, H / 2 - 70, "GAME OVER", 4, rgba(255, 255, 255));
    snprintf(buf, sizeof(buf), "得分 %d", score_);
    c.textCenter(W / 2, H / 2 + 10, buf, 2, rgba(242, 179, 61));
    c.textCenter(W / 2, H / 2 + 48, "点击屏幕再来一局", 2, rgba(220, 220, 220));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 50, "俄罗斯方块", 4, rgba(79, 195, 247));
    c.textCenter(W / 2, H / 2 + 10, "点下方按钮操作", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 44, "也可轻点旋转 / 上滑直落", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 90, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool GameTetris::onTouch(int action, int x, int y) {
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

  if (action == PG_TOUCH_DOWN) {
    dragging_ = true;
    consumed_ = false;
    downX_ = lastX_ = x;
    downY_ = lastY_ = y;
    pressMs_ = 0;
    moveStep_ = 0;
    softDropAcc_ = 0;
    return true;
  }
  if (action == PG_TOUCH_MOVE) {
    if (!dragging_) return true;
    // 异常跳变（触摸驱动/注入噪声）只做基准重同步，不产生动作
    if (abs(x - lastX_) > 160 || abs(y - lastY_) > 160) {
      downX_ = lastX_ = x;
      downY_ = lastY_ = y;
      moveStep_ = 0;
      return true;
    }
    int dx = x - downX_;
    int dy = y - downY_;
    lastX_ = x;
    lastY_ = y;
    // 水平移动：每 24px 一格
    const int STEP = 24;
    int want = dx / STEP;
    while (moveStep_ < want) {
      if (testMove(1, 0, 0)) {
        sfx(SFX_MOVE);
      }
      ++moveStep_;
    }
    while (moveStep_ > want) {
      if (testMove(-1, 0, 0)) {
        sfx(SFX_MOVE);
      }
      --moveStep_;
    }
    // 下拖加速
    if (dy > SOFT_DROP_DY) {
      softDropAcc_ += 1;
      if (softDropAcc_ >= 2) {
        softDropAcc_ = 0;
        if (testMove(0, 1, 0)) {
          score_ += 1;
          dropAcc_ = 0;
        }
      }
    }
    /* 上滑 = 直落。
     * ★ 2026-09-15 加大阈值（用户反馈"容易误触发"）：
     *   原来只判 `dy < -50` ⇒ 横移时手指略微向上带、或轨迹是斜的，就会被判成直落，
     *   而直落是**立刻锁定方块**（不可逆）—— 误一次就毁一手，代价太高。
     *   现在两条都要满足：
     *     ① 上滑位移 ≥ HARD_DROP_DY(90px)（原 50px，约 4 格高）；
     *     ② 横向漂移 ≤ HARD_DROP_DX(40px) —— 必须是"真的往上滑"，
     *        斜着拖（本意多半是横移）不算。
     *   ⚠️ 阈值调大后不会影响正常手感：要直落时人会明显往上甩一下。
     *      但别把 HARD_DROP_DY 调小回 50 —— 那正是这次要修的误触。 */
    if (dy < -HARD_DROP_DY && abs(dx) <= HARD_DROP_DX && !consumed_) {
      consumed_ = true;
      int n = 0;
      while (testMove(0, 1, 0)) ++n;
      score_ += n * 2;
      sfx(SFX_DROP);
      lockPiece();
      saveBestIfNeeded(score_);
    }
    return true;
  }
  if (action == PG_TOUCH_UP) {
    dragging_ = false;
    int dx = x - downX_;
    int dy = y - downY_;
    if (!consumed_ && abs(dx) < TAP_DX && abs(dy) < TAP_DY) {
      // 轻点 = 旋转
      if (testMove(0, 0, 1)) {
        sfx(SFX_ROTATE);
      } else {
        sfx(SFX_HIT);
      }
    }
    return true;
  }
  return true;
}

/* ---- 底部软按钮：左 / 下 / 右 / 转 ------------------------------------
 * 起因（2026-09-13 游戏功能检讨）：原先四种操作全挤在**同一个触摸区域**里靠位移区分 ——
 * 水平拖 24px=移动、下拖>40=软降、上滑<-50=直落、轻点(<14px)=旋转。
 * 想横移时手抖一下会被判成旋转；想软降时拖斜了会变成横移。
 * 现在底部给 4 个实体按钮（**绘制与命中由 logic 层统一负责**，见 mainLogic 的
 * drawSoftButtons / dispatchCanvasTouch），手感确定、无歧义；
 * 手势只保留"上滑直落"（方向差异最大，最不容易误触）。
 */
const SoftButton *GameTetris::softButtons(int &n) const {
  if (softN_ != 4) {
    layoutSoftBar(soft_, 4, vw(), vh());
    soft_[0].label = "左";
    soft_[0].id = SOFT_LEFT;
    soft_[1].label = "下";
    soft_[1].id = SOFT_DOWN;
    soft_[2].label = "右";
    soft_[2].id = SOFT_RIGHT;
    soft_[3].label = "转";
    soft_[3].id = SOFT_ROT;
    softN_ = 4;
  }
  n = softN_;
  return soft_;
}

bool GameTetris::onSoftButton(int id) {
  if (over_) return false;
  if (state_ == GSTATE_READY) {
    state_ = GSTATE_RUNNING;
    sfx(SFX_CLICK);
    return true;
  }
  if (!isPlaying()) return true;
  switch (id) {
    case SOFT_LEFT:
      if (testMove(-1, 0, 0)) sfx(SFX_MOVE);
      return true;
    case SOFT_RIGHT:
      if (testMove(1, 0, 0)) sfx(SFX_MOVE);
      return true;
    case SOFT_DOWN:  // 软降：每格 1 分（与手势软降的计分一致）
      if (testMove(0, 1, 0)) {
        score_ += 1;
        dropAcc_ = 0;
      } else {
        sfx(SFX_HIT);
      }
      return true;
    case SOFT_ROT:
      if (testMove(0, 0, 1)) sfx(SFX_ROTATE);
      else sfx(SFX_HIT);
      return true;
  }
  return false;
}

bool GameTetris::onKey(int key) {
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
    return true;
  }
  return false;
}

const char *GameTetris::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", lines_);
  return buf;
}

const char *GameTetris::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", level_);
  return buf;
}

const char *GameTetris::hint() const {
  if (over_) return "游戏结束 - 点击屏幕或按暂停键重来";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点下方按钮操作 · 也可轻点旋转 / 上滑直落";
}

const char *GameTetris::keyBar() const {
  return "暂停键 暂停/继续 · 长按 返回列表 · 音量键 调音量";
}

bool GameTetris::justGameOver() { return overFlag_; }
void GameTetris::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GameTetris::stillFrame() const {
  return stillUnless(flashMs_);
}

}  // namespace pg
