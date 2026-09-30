/*
 * PgPlane.cpp - 打飞机（触摸拖动控制战机，自动开火）
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

const float PLAYER_MAX_SPEED = 1150.0f;  // px/s，跟随手指的最大速度
const float BULLET_VY = -560.0f;
const float PLAYER_W = 46.0f;
const float PLAYER_H = 46.0f;
const int FIRE_INTERVAL = 170;        // ms
const int INVINCIBLE_MS = 1600;

Color bgTop() { return rgba(10, 14, 24); }
Color bgBot() { return rgba(18, 26, 40); }

bool hitRect(float ax, float ay, float aw, float ah, float bx, float by, float bw,
             float bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

}  // namespace

GamePlane::GamePlane()
    : px_(0),
      py_(0),
      targetX_(0),
      targetY_(0),
      lives_(3),
      score_(0),
      level_(1),
      fireCd_(0),
      spawnCd_(0),
      invincibleMs_(0),
      shakeMs_(0),
      over_(false),
      overFlag_(false),
      dragging_(false) {
  memset(bullets_, 0, sizeof(bullets_));
  memset(ebullets_, 0, sizeof(ebullets_));
  memset(enemies_, 0, sizeof(enemies_));
  memset(expls_, 0, sizeof(expls_));
}

const char *GamePlane::title() const { return "打飞机"; }
const char *GamePlane::desc() const { return "拖动战机自动开火，躲弹幕"; }
const char *GamePlane::tag() const { return "PLANE"; }
Color GamePlane::theme() const { return rgba(239, 83, 80); }

void GamePlane::reset() {
  memset(bullets_, 0, sizeof(bullets_));
  memset(ebullets_, 0, sizeof(ebullets_));
  memset(enemies_, 0, sizeof(enemies_));
  memset(expls_, 0, sizeof(expls_));
  px_ = vw() * 0.5f;
  py_ = vh() - 90.0f;
  targetX_ = px_;
  targetY_ = py_;
  lives_ = 3;
  score_ = 0;
  level_ = 1;
  fireCd_ = 0;
  spawnCd_ = 400;
  invincibleMs_ = 0;
  shakeMs_ = 0;
  over_ = false;
  overFlag_ = false;
  dragging_ = false;
}

void GamePlane::addExpl(float x, float y, float r, int ms) {
  for (int i = 0; i < MAX_EXPL; ++i) {
    if (!expls_[i].alive) {
      expls_[i].alive = true;
      expls_[i].x = x;
      expls_[i].y = y;
      expls_[i].ms = 0;
      expls_[i].maxMs = ms;
      expls_[i].r = r;
      return;
    }
  }
}

void GamePlane::fire() {
  for (int i = 0; i < MAX_BULLET; ++i) {
    if (!bullets_[i].alive) {
      bullets_[i].alive = true;
      bullets_[i].x = px_;
      bullets_[i].y = py_ - 24.0f;
      bullets_[i].vy = BULLET_VY;
      sfx(SFX_SHOOT);
      return;
    }
  }
}

void GamePlane::spawnEnemy() {
  for (int i = 0; i < MAX_ENEMY; ++i) {
    if (enemies_[i].alive) continue;
    Enemy &e = enemies_[i];
    int roll = rand() % 100;
    if (roll < 62) {
      e.kind = 0;
      e.hp = 1;
      e.w = 34;
      e.h = 32;
    } else if (roll < 90) {
      e.kind = 1;
      e.hp = 3;
      e.w = 48;
      e.h = 44;
    } else {
      e.kind = 2;
      e.hp = 8;
      e.w = 64;
      e.h = 58;
    }
    e.alive = true;
    e.x = 24.0f + (float)(rand() % (int)(vw() - 48 - e.w));
    e.y = -e.h - 10.0f;
    e.vy = 55.0f + (float)(rand() % 45) + level_ * 7.0f;
    if (e.vy > 190.0f) e.vy = 190.0f;
    e.phase = (float)(rand() % 628) / 100.0f;
    e.shootCd = 900.0f + (float)(rand() % 900);
    return;
  }
}

void GamePlane::killPlayer() {
  if (invincibleMs_ > 0) return;
  --lives_;
  addExpl(px_, py_, 40.0f, 420);
  shakeMs_ = 320;
  invincibleMs_ = INVINCIBLE_MS;
  sfx(SFX_HIT);
  if (lives_ <= 0) {
    over_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    sfx(SFX_OVER);
    saveBestIfNeeded(score_);
  }
}

void GamePlane::update(int dtMs) {
  if (!isPlaying() || over_) return;
  float dt = (float)dtMs / 1000.0f;

  if (invincibleMs_ > 0) invincibleMs_ -= dtMs;
  if (shakeMs_ > 0) shakeMs_ -= dtMs;

  // 玩家跟随手指（限速直线趋近，手感干脆）
  {
    float dxs = targetX_ - px_;
    float dys = targetY_ - py_;
    float dist = sqrtf(dxs * dxs + dys * dys);
    float maxStep = PLAYER_MAX_SPEED * dt;
    if (dist > 0.01f) {
      float step = dist < maxStep ? dist : maxStep;
      px_ += dxs / dist * step;
      py_ += dys / dist * step;
    }
    if (px_ < PLAYER_W / 2) px_ = PLAYER_W / 2;
    if (px_ > vw() - PLAYER_W / 2) px_ = vw() - PLAYER_W / 2;
    if (py_ < PLAYER_H / 2 + 10) py_ = PLAYER_H / 2 + 10;
    if (py_ > vh() - PLAYER_H / 2 - 4) py_ = vh() - PLAYER_H / 2 - 4;
  }

  // 自动开火
  fireCd_ -= dtMs;
  if (fireCd_ <= 0) {
    fireCd_ = FIRE_INTERVAL;
    fire();
  }

  // 生成敌机
  spawnCd_ -= dtMs;
  if (spawnCd_ <= 0) {
    int base = 900 - level_ * 70;
    if (base < 280) base = 280;
    spawnCd_ = base + rand() % 400;
    spawnEnemy();
  }

  // 我方子弹
  for (int i = 0; i < MAX_BULLET; ++i) {
    if (!bullets_[i].alive) continue;
    bullets_[i].y += bullets_[i].vy * dt;
    if (bullets_[i].y < -20) bullets_[i].alive = false;
  }

  // 敌机
  for (int i = 0; i < MAX_ENEMY; ++i) {
    Enemy &e = enemies_[i];
    if (!e.alive) continue;
    e.y += e.vy * dt;
    if (e.kind != 2) {
      e.phase += dt * 2.0f;
      e.x += sinf(e.phase) * 26.0f * dt;
      if (e.x < 4) e.x = 4;
      if (e.x > vw() - e.w - 4) e.x = vw() - e.w - 4;
    }
    // 射击
    if (e.kind >= 1) {
      e.shootCd -= dtMs;
      if (e.shootCd <= 0 && e.y > 0 && e.y < vh() * 0.72f) {
        e.shootCd = 1200.0f + (float)(rand() % 900);
        for (int k = 0; k < MAX_EBULLET; ++k) {
          if (!ebullets_[k].alive) {
            ebullets_[k].alive = true;
            ebullets_[k].x = e.x + e.w / 2;
            ebullets_[k].y = e.y + e.h;
            ebullets_[k].vy = e.kind == 2 ? 260.0f : 210.0f;
            break;
          }
        }
      }
    }
    if (e.y > vh() + 20) e.alive = false;

    // 撞机
    if (invincibleMs_ <= 0 &&
        hitRect(px_ - PLAYER_W / 2 + 8, py_ - PLAYER_H / 2 + 6, PLAYER_W - 16,
                PLAYER_H - 14, e.x, e.y, (float)e.w, (float)e.h)) {
      e.alive = false;
      addExpl(e.x + e.w / 2, e.y + e.h / 2, 34, 380);
      killPlayer();
      continue;
    }
    // 我方子弹命中
    for (int b = 0; b < MAX_BULLET; ++b) {
      if (!bullets_[b].alive) continue;
      if (hitRect(bullets_[b].x - 4, bullets_[b].y - 12, 8, 18, e.x, e.y,
                  (float)e.w, (float)e.h)) {
        bullets_[b].alive = false;
        --e.hp;
        if (e.hp <= 0) {
          e.alive = false;
          int gain = e.kind == 0 ? 100 : (e.kind == 1 ? 300 : 800);
          score_ += gain;
          saveBestIfNeeded(score_);
          addExpl(e.x + e.w / 2, e.y + e.h / 2,
                  e.kind == 0 ? 26.0f : (e.kind == 1 ? 38.0f : 54.0f),
                  e.kind == 0 ? 300 : 460);
          sfx(SFX_SCORE);
          int newLevel = score_ / 1500 + 1;
          if (newLevel > level_) level_ = newLevel;
        } else {
          sfx(SFX_MOVE);
        }
        break;
      }
    }
  }

  // 敌弹
  for (int i = 0; i < MAX_EBULLET; ++i) {
    if (!ebullets_[i].alive) continue;
    ebullets_[i].y += ebullets_[i].vy * dt;
    if (ebullets_[i].y > vh() + 20) {
      ebullets_[i].alive = false;
      continue;
    }
    if (invincibleMs_ <= 0 &&
        hitRect(px_ - PLAYER_W / 2 + 12, py_ - PLAYER_H / 2 + 10,
                PLAYER_W - 24, PLAYER_H - 20, ebullets_[i].x - 4,
                ebullets_[i].y - 8, 8, 16)) {
      ebullets_[i].alive = false;
      killPlayer();
    }
  }

  // 爆炸动画
  for (int i = 0; i < MAX_EXPL; ++i) {
    if (!expls_[i].alive) continue;
    expls_[i].ms += dtMs;
    if (expls_[i].ms >= expls_[i].maxMs) expls_[i].alive = false;
  }
}

void GamePlane::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();

  // 背景：深色渐变（按行插值）+ 星点
  for (int y = 0; y < H; y += 2) {
    Color col = lerpColor(bgTop(), bgBot(), y * 256 / H);
    c.fillRect(0, y, W, 2, col);
  }
  // 星空（确定性伪随机，避免每帧抖动）
  static const int STAR_N = 42;
  for (int i = 0; i < STAR_N; ++i) {
    int sx = (i * 97 + 31) % W;
    int sy = (i * 173 + 59) % H;
    int bright = 60 + (i * 37) % 120;
    c.fillRect(sx, sy, 2, 2, rgba(bright, bright, bright));
  }

  int shakeX = 0, shakeY = 0;
  if (shakeMs_ > 0) {
    shakeX = (rand() % 5) - 2;
    shakeY = (rand() % 5) - 2;
  }

  // 敌机
  for (int i = 0; i < MAX_ENEMY; ++i) {
    const Enemy &e = enemies_[i];
    if (!e.alive) continue;
    int x = (int)e.x + shakeX;
    int y = (int)e.y + shakeY;
    int w = e.w, h = e.h;
    Color body = e.kind == 0 ? rgba(200, 90, 90)
                             : (e.kind == 1 ? rgba(230, 140, 70)
                                            : rgba(180, 80, 200));
    // 机翼
    c.fillTriangle(x + w / 2, y + h, x, y + h / 3, x + w, y + h / 3, body);
    c.fillRectRound(x + w / 3, y, w / 3, h * 2 / 3, 4, shade(body, 30));
    // 座舱
    c.fillCircle(x + w / 2, y + h / 3, w / 8, rgba(250, 250, 255));
    if (e.kind == 2) {
      c.fillCircle(x + w / 2, y + h * 2 / 3, w / 6, rgba(255, 120, 200));
    }
  }

  // 我方子弹
  for (int i = 0; i < MAX_BULLET; ++i) {
    if (!bullets_[i].alive) continue;
    int x = (int)bullets_[i].x + shakeX;
    int y = (int)bullets_[i].y + shakeY;
    c.fillRect(x - 2, y - 12, 4, 18, rgba(120, 240, 255));
    c.fillRect(x - 1, y - 12, 2, 18, rgba(255, 255, 255));
  }

  // 敌弹
  for (int i = 0; i < MAX_EBULLET; ++i) {
    if (!ebullets_[i].alive) continue;
    c.fillCircle((int)ebullets_[i].x + shakeX, (int)ebullets_[i].y + shakeY, 5,
                 rgba(255, 210, 90));
    c.fillCircle((int)ebullets_[i].x + shakeX, (int)ebullets_[i].y + shakeY, 2,
                 rgba(255, 255, 255));
  }

  // 我方战机
  if (!over_ && lives_ > 0) {
    bool blink = (invincibleMs_ > 0) && ((invincibleMs_ / 90) % 2 == 0);
    if (!blink) {
      int x = (int)px_ + shakeX;
      int y = (int)py_ + shakeY;
      // 尾焰
      int flame = 10 + rand() % 10;
      c.fillTriangle(x - 8, y + 16, x + 8, y + 16, x, y + 16 + flame,
                     rgba(255, 180, 60));
      c.fillTriangle(x - 4, y + 16, x + 4, y + 16, x, y + 14 + flame / 2,
                     rgba(255, 240, 160));
      // 机身
      c.fillTriangle(x, y - 26, x - 16, y + 20, x + 16, y + 20,
                     rgba(80, 190, 235));
      c.fillRectRound(x - 6, y - 6, 12, 24, 4, rgba(200, 235, 250));
      // 机翼
      c.fillTriangle(x - 16, y + 12, x - 24, y + 24, x - 6, y + 18,
                     rgba(60, 150, 200));
      c.fillTriangle(x + 16, y + 12, x + 24, y + 24, x + 6, y + 18,
                     rgba(60, 150, 200));
      // 座舱
      c.fillCircle(x, y + 2, 6, rgba(255, 255, 255));
      c.fillCircle(x, y + 2, 3, rgba(90, 200, 255));
    }
  }

  // 爆炸
  for (int i = 0; i < MAX_EXPL; ++i) {
    const Expl &ex = expls_[i];
    if (!ex.alive) continue;
    float t = (float)ex.ms / (float)ex.maxMs;
    int r = (int)(ex.r * (0.35f + 0.65f * t));
    int alpha = (int)(220 * (1.0f - t));
    c.blendRect((int)ex.x - r, (int)ex.y - r, r * 2, r * 2, rgba(255, 170, 40),
                alpha / 3);
    c.blendRect((int)ex.x - r * 2 / 3, (int)ex.y - r * 2 / 3, r * 4 / 3,
                r * 4 / 3, rgba(255, 90, 40), alpha);
  }

  // 生命图标
  for (int i = 0; i < lives_; ++i) {
    int hx = 16 + i * 26;
    int hy = 16;
    c.fillTriangle(hx, hy - 10, hx - 9, hy + 10, hx + 9, hy + 10,
                   rgba(80, 190, 235));
  }

  if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    c.textCenter(W / 2, H / 2 - 80, "游戏结束", 2, rgba(255, 255, 255));
    char buf[64];
    snprintf(buf, sizeof(buf), "得分 %d", score_);
    c.textCenter(W / 2, H / 2 - 10, buf, 2, rgba(242, 179, 61));
    c.textCenter(W / 2, H / 2 + 40, "点击屏幕再来一局", 2, rgba(220, 220, 220));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 80, "打飞机", 3, rgba(239, 83, 80));
    c.textCenter(W / 2, H / 2 - 10, "手指拖动战机", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 26, "自动开火", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 76, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool GamePlane::onTouch(int action, int x, int y) {
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
      targetX_ = px_;
      targetY_ = py_;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;
  if (action == PG_TOUCH_DOWN || action == PG_TOUCH_MOVE) {
    dragging_ = true;
    // 手指上方留出 46px，避免挡住战机
    targetX_ = (float)x;
    targetY_ = (float)y - 46.0f;
    if (targetX_ < PLAYER_W / 2) targetX_ = PLAYER_W / 2;
    if (targetX_ > vw() - PLAYER_W / 2) targetX_ = vw() - PLAYER_W / 2;
    if (targetY_ < PLAYER_H / 2 + 10) targetY_ = PLAYER_H / 2 + 10;
    if (targetY_ > vh() - PLAYER_H / 2 - 4) targetY_ = vh() - PLAYER_H / 2 - 4;
    return true;
  }
  if (action == PG_TOUCH_UP) {
    dragging_ = false;
    return true;
  }
  return true;
}

bool GamePlane::onKey(int key) {
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

const char *GamePlane::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", lives_ > 0 ? lives_ : 0);
  return buf;
}

const char *GamePlane::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", level_);
  return buf;
}

const char *GamePlane::hint() const {
  if (over_) return "游戏结束 - 点击屏幕或按暂停键重来";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "拖动屏幕移动战机，自动开火";
}

const char *GamePlane::keyBar() const {
  return "暂停键 暂停/继续 · 长按 返回列表 · 音量键 调音量";
}

bool GamePlane::justGameOver() { return overFlag_; }
void GamePlane::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GamePlane::stillFrame() const {
  return stillUnless(invincibleMs_, shakeMs_);
}

}  // namespace pg
