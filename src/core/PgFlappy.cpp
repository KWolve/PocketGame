/*
 * PgFlappy.cpp - 小鸟过水管（点击屏幕让小鸟上升）
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgSpriteDraw.h"   // 贴图助手（静态天空/地面走素材，见 docs/game-art-pipeline.md）

namespace pg {

namespace {

const float GRAVITY = 1500.0f;    // px/s^2
const float FLAP_VY = -450.0f;    // 振翅瞬时速度
const float MAX_FALL = 700.0f;    // 最大下落速度
const float PIPE_SPEED = 155.0f;  // px/s
const int PIPE_W = 78;
const int GAP_H = 172;
const int PIPE_SPACING = 250;   // px
const int BIRD_R = 16;
const int GROUND_H = 46;

/* 天空配色已烘进 images/game/flappy/bg.png（见 tools/gen_game_art.py 的
 * FLAPPY_SKY_TOP/BOT —— 改色要两边一起改），代码里不再需要。 */
Color pipeColor() { return rgba(72, 176, 92); }
Color pipeDark() { return rgba(46, 126, 62); }

}  // namespace

GameFlappy::GameFlappy()
    : by_(0),
      vy_(0),
      birdX_(0),
      score_(0),
      spawnCd_(0),
      groundOffset_(0),
      birdRot_(0),
      over_(false),
      overFlag_(false),
      started_(false),
      lastFlapMs_(0),
      shakeMs_(0) {
  memset(pipes_, 0, sizeof(pipes_));
}

const char *GameFlappy::title() const { return "小鸟过水管"; }
const char *GameFlappy::desc() const { return "点击屏幕振翅，穿过水管"; }
const char *GameFlappy::tag() const { return "BIRD"; }
Color GameFlappy::theme() const { return rgba(76, 175, 80); }

void GameFlappy::reset() {
  memset(pipes_, 0, sizeof(pipes_));
  birdX_ = vw() * 0.30f;
  by_ = vh() * 0.40f;
  vy_ = 0;
  score_ = 0;
  spawnCd_ = 500;
  groundOffset_ = 0;
  birdRot_ = 0;
  over_ = false;
  overFlag_ = false;
  started_ = false;
  lastFlapMs_ = 0;
  shakeMs_ = 0;
}

void GameFlappy::flap() {
  vy_ = FLAP_VY;
  birdRot_ = -0.38f;
  sfx(SFX_JUMP);
}

void GameFlappy::spawnPipe() {
  for (int i = 0; i < MAX_PIPE; ++i) {
    if (pipes_[i].alive) continue;
    Pipe &p = pipes_[i];
    p.alive = true;
    p.x = (float)vw() + 20.0f;
    p.gapH = GAP_H;
    int minY = 60;
    int maxY = vh() - GROUND_H - p.gapH - 60;
    if (maxY < minY + 1) maxY = minY + 1;
    p.gapY = minY + rand() % (maxY - minY);
    p.passed = false;
    return;
  }
}

void GameFlappy::update(int dtMs) {
  float dt = (float)dtMs / 1000.0f;
  if (shakeMs_ > 0) shakeMs_ -= dtMs;

  if (!isPlaying() || over_) return;

  groundOffset_ = (groundOffset_ + (int)(PIPE_SPEED * dt)) % 48;

  if (!started_) {
    // 待机：小鸟悬停（不落）
    return;
  }
  // 物理
  vy_ += GRAVITY * dt;
  if (vy_ > MAX_FALL) vy_ = MAX_FALL;
  by_ += vy_ * dt;
  birdRot_ += (vy_ > 0 ? 3.0f : -3.0f) * dt;
  if (birdRot_ > 1.15f) birdRot_ = 1.15f;
  if (birdRot_ < -0.45f) birdRot_ = -0.45f;

  // 天花板
  if (by_ < BIRD_R + 2) {
    by_ = BIRD_R + 2;
    if (vy_ < 0) vy_ = 0;
  }

  // 水管
  for (int i = 0; i < MAX_PIPE; ++i) {
    Pipe &p = pipes_[i];
    if (!p.alive) continue;
    p.x -= PIPE_SPEED * dt;
    if (!p.passed && p.x + PIPE_W < birdX_ - BIRD_R) {
      p.passed = true;
      ++score_;
      saveBestIfNeeded(score_);
      sfx(SFX_SCORE);
    }
    if (p.x + PIPE_W < -10) p.alive = false;
  }

  // 生成
  spawnCd_ -= dtMs;
  if (spawnCd_ <= 0) {
    spawnCd_ = (int)(PIPE_SPACING / PIPE_SPEED * 1000.0f);
    spawnPipe();
  }

  // 碰撞（用内缩后的鸟盒，手感宽松一点）
  int bx = (int)birdX_ - BIRD_R + 4;
  int byy = (int)by_ - BIRD_R + 4;
  int bw = BIRD_R * 2 - 8;
  int bh = BIRD_R * 2 - 8;

  if (by_ + BIRD_R >= vh() - GROUND_H) {
    by_ = vh() - GROUND_H - BIRD_R;
    over_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    sfx(SFX_OVER);
    saveBestIfNeeded(score_);
    return;
  }

  for (int i = 0; i < MAX_PIPE; ++i) {
    const Pipe &p = pipes_[i];
    if (!p.alive) continue;
    int px = (int)p.x;
    if (bx < px + PIPE_W && bx + bw > px) {
      if (byy < p.gapY || byy + bh > p.gapY + p.gapH) {
        over_ = true;
        overFlag_ = true;
        state_ = GSTATE_OVER;
        shakeMs_ = 300;
        sfx(SFX_OVER);
        saveBestIfNeeded(score_);
        return;
      }
    }
  }
}

void GameFlappy::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();

  /* 天空渐变 + 地面 = **一张静态图**（素材化，2026-09-15）。
   * 原来天空是**逐 2 行 fillRect 共 270 次**（全屏扫一遍），是本作 44fps 的主要瓶颈；
   * 地面也是每次 2 条全宽 fillRect。都烘进底图 ⇒ 一次 memcpy 快路径。
   * 云 / 水管 / 地面滚动条纹是动的，仍留在代码里画。 */
  blit(c, gameart::kFlappyBg, 0, 0);

  // 云
  for (int i = 0; i < 4; ++i) {
    int cx = (i * 149 + 40 - (groundOffset_ * (i % 3 + 1)) / 4) % (W + 120);
    if (cx < -80) cx += W + 120;
    int cy = 50 + (i * 67) % 150;
    c.fillEllipse(cx, cy, 42, 15, rgba(235, 242, 250, 60));
    c.fillEllipse(cx + 26, cy - 8, 28, 12, rgba(235, 242, 250, 50));
  }

  int shakeY = (shakeMs_ > 0) ? ((rand() % 7) - 3) : 0;

  // 水管
  for (int i = 0; i < MAX_PIPE; ++i) {
    const Pipe &p = pipes_[i];
    if (!p.alive) continue;
    int px = (int)p.x;
    // 上管
    c.fillRect(px, 0, PIPE_W, p.gapY, pipeColor());
    c.fillRect(px, 0, 10, p.gapY, shade(pipeColor(), 40));
    c.fillRect(px + PIPE_W - 12, 0, 12, p.gapY, pipeDark());
    // 上管口
    c.fillRect(px - 5, p.gapY - 22, PIPE_W + 10, 22, pipeColor());
    c.fillRect(px - 5, p.gapY - 22, 10, 22, shade(pipeColor(), 40));
    c.fillRect(px + PIPE_W - 5, p.gapY - 22, 10, 22, pipeDark());
    // 下管
    int gy = p.gapY + p.gapH;
    c.fillRect(px, gy, PIPE_W, H - GROUND_H - gy, pipeColor());
    c.fillRect(px, gy, 10, H - GROUND_H - gy, shade(pipeColor(), 40));
    c.fillRect(px + PIPE_W - 12, gy, 12, H - GROUND_H - gy, pipeDark());
    c.fillRect(px - 5, gy, PIPE_W + 10, 22, pipeColor());
    c.fillRect(px - 5, gy, 10, 22, shade(pipeColor(), 40));
    c.fillRect(px + PIPE_W - 5, gy, 10, 22, pipeDark());
  }

  // 地面**滚动的条纹**（地面本体已在底图里；条纹位置随 groundOffset_ 变，所以留在代码里）
  int gy = H - GROUND_H;
  for (int x = -48; x < W + 48; x += 48) {
    int xx = x - groundOffset_;
    c.fillRect(xx, gy + 10, 24, GROUND_H - 10, rgba(78, 62, 42));
  }

  // 小鸟
  {
    int bx = (int)birdX_;
    int byy = (int)by_ + shakeY;
    // 身体
    c.fillEllipse(bx, byy, BIRD_R + 2, BIRD_R, rgba(250, 214, 78));
    c.fillEllipse(bx, byy + 2, BIRD_R - 2, BIRD_R - 5, rgba(255, 236, 150));
    // 翅膀（随速度摆动）
    int wa = (int)(vy_ / 14.0f);
    c.fillEllipse(bx - 5, byy + 2 + wa, 10, 7, rgba(240, 176, 48));
    // 眼睛
    c.fillCircle(bx + 7, byy - 6, 4, rgba(255, 255, 255));
    c.fillCircle(bx + 9, byy - 6, 2, rgba(20, 20, 20));
    // 喙
    c.fillTriangle(bx + 14, byy - 2, bx + 24, byy + 2, bx + 14, byy + 7,
                   rgba(250, 130, 40));
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 110);
    c.textCenter(W / 2, H / 2 - 110, "小鸟过水管", 3, rgba(250, 214, 78));
    c.textCenter(W / 2, H / 2 - 30, "点击屏幕振翅上升", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 6, "穿过水管得分", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  } else if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 110, "游戏结束", 2, rgba(255, 255, 255));
    char buf[64];
    snprintf(buf, sizeof(buf), "得分 %d", score_);
    c.textCenter(W / 2, H / 2 - 40, buf, 2, rgba(250, 214, 78));
    c.textCenter(W / 2, H / 2 + 10, "点击屏幕再来一局", 2, rgba(220, 220, 220));
  }
}

bool GameFlappy::onTouch(int action, int x, int y) {
  (void)x;
  (void)y;
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
      started_ = true;
      vy_ = 0;
      flap();
    }
    return true;
  }
  if (!isPlaying()) return true;
  if (action == PG_TOUCH_DOWN) {
    if (!started_) started_ = true;
    flap();
  }
  return true;
}

bool GameFlappy::onKey(int key) {
  if (over_ && key == PG_KEY_A) {
    reset();
    state_ = GSTATE_RUNNING;
    started_ = true;
    flap();
    return true;
  }
  if (state_ == GSTATE_READY && key == PG_KEY_A) {
    state_ = GSTATE_RUNNING;
    started_ = true;
    flap();
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *GameFlappy::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", score_);
  return buf;
}

const char *GameFlappy::hint() const {
  if (over_) return "游戏结束 - 点击屏幕或按暂停键重来";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  if (!started_) return "点击屏幕振翅起飞";
  return "点击屏幕让小鸟上升";
}

bool GameFlappy::justGameOver() { return overFlag_; }
void GameFlappy::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GameFlappy::stillFrame() const {
  return stillUnless(shakeMs_);
}

}  // namespace pg
