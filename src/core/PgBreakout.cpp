/*
 * PgBreakout.cpp - 打砖块
 *
 * 操作：拖动屏幕移动挡板；点屏幕发射球；A 开始/暂停；C 重来；B 返回。
 * 清完一屏加一层（球更快），三条命用完结束。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

const int PADDLE_W = 92;
const int PADDLE_H = 14;
const int PADDLE_BOTTOM = 52;  // 挡板中心距底边
const float BALL_R = 8.5f;
const float BASE_SPEED = 300.0f;
const float LEVEL_SPEEDUP = 42.0f;

Color rowColorFn(int row) {
  switch (row) {
    case 0: return rgba(232, 92, 86);
    case 1: return rgba(240, 152, 68);
    case 2: return rgba(240, 214, 78);
    case 3: return rgba(110, 200, 110);
    default: return rgba(96, 152, 236);
  }
}

}  // namespace

GameBreakout::GameBreakout()
    : bx_(0),
      by_(0),
      vx_(0),
      vy_(0),
      px_(0),
      targetX_(0),
      lives_(3),
      level_(1),
      score_(0),
      bricks_(0),
      launched_(false),
      over_(false),
      overFlag_(false),
      win_(false),
      dragging_(false),
      cellW_(0),
      cellH_(0),
      brickTop_(0),
      fieldW_(0),
      fieldH_(0),
      hitMs_(0),
      shakeMs_(0) {
  memset(brick_, 0, sizeof(brick_));
  memset(rowColor_, 0, sizeof(rowColor_));
}

const char *GameBreakout::title() const { return "打砖块"; }
const char *GameBreakout::desc() const { return "拖动挡板，弹球打砖"; }
const char *GameBreakout::tag() const { return "BRICK"; }
Color GameBreakout::theme() const { return rgba(232, 122, 92); }

void GameBreakout::layout() {
  fieldW_ = vw();
  fieldH_ = vh();
  cellW_ = fieldW_ / COLS;
  cellH_ = 34;
  brickTop_ = 26;
  for (int r = 0; r < ROWS; ++r) rowColor_[r] = rowColorFn(r);
}

void GameBreakout::buildLevel() {
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) brick_[r][c] = 1;
  }
  bricks_ = ROWS * COLS;
}

void GameBreakout::resetBall() {
  launched_ = false;
  px_ = vw() * 0.5f;
  targetX_ = px_;
  bx_ = px_;
  by_ = vh() - PADDLE_BOTTOM - PADDLE_H - BALL_R - 2;
  vx_ = 0;
  vy_ = 0;
}

void GameBreakout::reset() {
  layout();
  buildLevel();
  lives_ = 3;
  level_ = 1;
  score_ = 0;
  over_ = false;
  overFlag_ = false;
  win_ = false;
  dragging_ = false;
  hitMs_ = 0;
  shakeMs_ = 0;
  resetBall();
}

int GameBreakout::bricksLeft() const {
  int n = 0;
  for (int r = 0; r < ROWS; ++r)
    for (int c = 0; c < COLS; ++c)
      if (brick_[r][c]) ++n;
  return n;
}

bool GameBreakout::brickAlive(int r, int c) const {
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return false;
  return brick_[r][c] != 0;
}

void GameBreakout::loseLife() {
  --lives_;
  sfx(lives_ > 0 ? SFX_HIT : SFX_OVER);
  if (lives_ <= 0) {
    over_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    saveBestIfNeeded(score_);
    return;
  }
  resetBall();
}

void GameBreakout::update(int dtMs) {
  float dt = (float)dtMs / 1000.0f;
  if (hitMs_ > 0) hitMs_ -= dtMs;
  if (shakeMs_ > 0) shakeMs_ -= dtMs;
  if (!isPlaying() || over_) return;

  // 挡板跟随（插值，手感柔一点）
  float k = 1.0f - powf(1.0f - 0.45f, dt * 60.0f);
  px_ += (targetX_ - px_) * k;
  if (px_ < PADDLE_W / 2) px_ = PADDLE_W / 2;
  if (px_ > fieldW_ - PADDLE_W / 2) px_ = fieldW_ - PADDLE_W / 2;

  if (!launched_) {
    bx_ = px_;
    by_ = fieldH_ - PADDLE_BOTTOM - PADDLE_H - BALL_R - 2;
    return;
  }

  float speed = BASE_SPEED + (level_ - 1) * LEVEL_SPEEDUP;
  float len = sqrtf(vx_ * vx_ + vy_ * vy_);
  if (len > 1.0f) {  // 归一化到目标速度
    vx_ = vx_ / len * speed;
    vy_ = vy_ / len * speed;
  }

  // 分步推进，避免高速穿透
  int steps = 1 + (int)(speed * dt / 8.0f);
  float sdt = dt / (float)steps;
  for (int i = 0; i < steps && !over_; ++i) {
    bx_ += vx_ * sdt;
    by_ += vy_ * sdt;

    // 左右墙
    if (bx_ < BALL_R) {
      bx_ = BALL_R;
      vx_ = -vx_;
      sfx(SFX_HIT);
    } else if (bx_ > fieldW_ - BALL_R) {
      bx_ = fieldW_ - BALL_R;
      vx_ = -vx_;
      sfx(SFX_HIT);
    }
    // 顶部
    if (by_ < BALL_R) {
      by_ = BALL_R;
      vy_ = -vy_;
      sfx(SFX_HIT);
    }

    // 挡板
    float py = fieldH_ - PADDLE_BOTTOM;
    if (vy_ > 0 && by_ + BALL_R >= py - PADDLE_H / 2 &&
        by_ + BALL_R <= py + PADDLE_H / 2 + 6 && bx_ >= px_ - PADDLE_W / 2 &&
        bx_ <= px_ + PADDLE_W / 2) {
      by_ = py - PADDLE_H / 2 - BALL_R;
      // 命中位置决定反弹角（越靠边越斜）
      float t = (bx_ - px_) / (PADDLE_W / 2);
      if (t < -1.0f) t = -1.0f;
      if (t > 1.0f) t = 1.0f;
      float ang = t * 1.05f;  // ±60°
      float speed2 = BASE_SPEED + (level_ - 1) * LEVEL_SPEEDUP;
      vx_ = sinf(ang) * speed2;
      vy_ = -cosf(ang) * speed2;
      hitMs_ = 120;
      sfx(SFX_MOVE);
    }

    // 砖块
    int c = (int)(bx_ / (float)cellW_);
    int r = (int)((by_ - brickTop_) / (float)cellH_);
    if (r >= 0 && r < ROWS && c >= 0 && c < COLS && brick_[r][c]) {
      brick_[r][c] = 0;
      --bricks_;
      score_ += 10 * level_;
      hitMs_ = 90;
      sfx(SFX_CLEAR);
      saveBestIfNeeded(score_);
      // 从哪边撞进去的：比较穿入深度
      float cx = (c + 0.5f) * cellW_;
      float cy = brickTop_ + (r + 0.5f) * cellH_;
      float dx = fabsf(bx_ - cx) / (cellW_ * 0.5f);
      float dy = fabsf(by_ - cy) / (cellH_ * 0.5f);
      if (dx > dy) {
        vx_ = -vx_;
        bx_ += (bx_ > cx ? 1 : -1) * 2.0f;
      } else {
        vy_ = -vy_;
        by_ += (by_ > cy ? 1 : -1) * 2.0f;
      }
      if (bricks_ <= 0) {
        // 进下一层
        ++level_;
        score_ += 100;
        sfx(SFX_SCORE);
        saveBestIfNeeded(score_);
        buildLevel();
        resetBall();
        state_ = GSTATE_RUNNING;
        return;
      }
    }

    // 掉底
    if (by_ - BALL_R > fieldH_) {
      shakeMs_ = 260;
      loseLife();
      return;
    }
  }
}

void GameBreakout::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  layout();

  // 背景
  for (int y = 0; y < H; y += 3) {
    Color col = lerpColor(rgba(18, 22, 34), rgba(34, 30, 52), y * 256 / H);
    c.fillRect(0, y, W, 3, col);
  }
  // 网格点
  for (int y = 16; y < H; y += 32) {
    for (int x = 16; x < W; x += 32) {
      c.px(x, y, rgba(255, 255, 255, 24));
    }
  }

  int shakeY = (shakeMs_ > 0) ? ((rand() % 7) - 3) : 0;

  // 砖块
  for (int r = 0; r < ROWS; ++r) {
    for (int cc = 0; cc < COLS; ++cc) {
      if (!brick_[r][cc]) continue;
      int x = cc * cellW_ + 2;
      int y = brickTop_ + r * cellH_ + 2 + shakeY;
      int w = cellW_ - 4;
      int h = cellH_ - 5;
      Color base = rowColor_[r];
      c.fillRectRound(x, y, w, h, 5, base);
      c.fillRect(x + 2, y + 2, w - 4, h / 3, shade(base, 34));
      c.fillRect(x + 2, y + h - 4, w - 4, 2, shade(base, -50));
    }
  }

  // 挡板
  {
    int py = H - PADDLE_BOTTOM;
    int x = (int)px_ - PADDLE_W / 2;
    int y = py - PADDLE_H / 2 + shakeY;
    Color pc = (hitMs_ > 0) ? rgba(255, 255, 255) : rgba(226, 232, 240);
    c.fillRectRound(x, y, PADDLE_W, PADDLE_H, 7, pc);
    c.fillRectRound(x + 3, y + 3, PADDLE_W - 6, 4, 2, rgba(160, 190, 230));
  }

  // 球
  {
    int bx = (int)bx_, by = (int)by_ + shakeY;
    c.fillCircle(bx, by, (int)BALL_R, rgba(250, 226, 120));
    c.fillCircle(bx - 2, by - 2, (int)BALL_R / 2, rgba(255, 255, 255));
    if (!launched_) {  // 待发射提示
      c.textCenter(W / 2, H - PADDLE_BOTTOM - 74, "点屏幕发射", 2,
                   rgba(220, 220, 230));
    }
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 110, "打砖块", 3, rgba(240, 150, 120));
    c.textCenter(W / 2, H / 2 - 30, "拖动屏幕移动挡板", 2, rgba(232, 232, 232));
    c.textCenter(W / 2, H / 2 + 6, "点屏幕发射球", 2, rgba(232, 232, 232));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(170, 170, 180));
  } else if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 110, "游戏结束", 2, rgba(255, 255, 255));
    char buf[64];
    snprintf(buf, sizeof(buf), "得分 %d", score_);
    c.textCenter(W / 2, H / 2 - 40, buf, 2, rgba(250, 214, 78));
    snprintf(buf, sizeof(buf), "到第 %d 层", level_);
    c.textCenter(W / 2, H / 2 + 4, buf, 2, rgba(200, 200, 210));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕再来一局", 2, rgba(220, 220, 220));
  }
}

bool GameBreakout::onTouch(int action, int x, int y) {
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
  if (state_ == GSTATE_PAUSED) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;

  (void)y;
  if (action == PG_TOUCH_DOWN) {
    dragging_ = true;
    targetX_ = (float)x;
    if (!launched_) {
      launched_ = true;
      float ang = ((rand() % 60) - 30) * 0.01745f;
      vx_ = sinf(ang) * BASE_SPEED;
      vy_ = -cosf(ang) * BASE_SPEED;
      sfx(SFX_JUMP);
    }
    return true;
  }
  if (action == PG_TOUCH_MOVE) {
    if (!dragging_) return true;
    targetX_ = (float)x;
    return true;
  }
  if (action == PG_TOUCH_UP) {
    dragging_ = false;
    return true;
  }
  return true;
}

bool GameBreakout::onKey(int key) {
  if (key == PG_KEY_A) {
    if (over_) {
      reset();
      state_ = GSTATE_RUNNING;
      return true;
    }
    if (state_ == GSTATE_READY) {
      state_ = GSTATE_RUNNING;
      return true;
    }
    if (isPlaying() && !launched_) {  // 直接发射
      launched_ = true;
      float ang = ((rand() % 60) - 30) * 0.01745f;
      vx_ = sinf(ang) * BASE_SPEED;
      vy_ = -cosf(ang) * BASE_SPEED;
      sfx(SFX_JUMP);
      return true;
    }
    return false;  // 其余情况交给框架做暂停
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *GameBreakout::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", lives_);
  return buf;
}

const char *GameBreakout::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", level_);
  return buf;
}

const char *GameBreakout::hint() const {
  if (over_) return "游戏结束 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (!launched_) return "拖动移动挡板，点屏幕发射";
  return "拖动挡板接球，打光砖块过关";
}

bool GameBreakout::justGameOver() { return overFlag_; }
void GameBreakout::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GameBreakout::stillFrame() const {
  return stillUnless(hitMs_, shakeMs_);
}

}  // namespace pg
