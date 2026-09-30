/*
 * PgSnake.cpp - 贪吃蛇
 *
 * 操作：
 *   触摸：滑动手势转向（上/下/左/右），点一下暂停/继续
 *   按键：A 开始/暂停/继续（结束后重来）  B 返回列表  C 重玩
 *
 * 规则：撞墙或咬到自己 = 结束；吃到food +10 分并变长，速度随长度加快。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgSpriteDraw.h"   // 贴图助手（静态场地走素材，见 docs/game-art-pipeline.md）

namespace pg {

namespace {

const int COLS = 18;   // 格子数（横向）
const int ROWS = 18;   // 格子数（纵向）
const int CELL = 26;   // 每格像素（18*26 = 468，画布 480x540 居中放得下）
const int PAD = 6;     // 网格外框

const int START_LEN = 4;
const int FOOD_SCORE = 10;
const int BASE_INTERVAL = 260;  // 起始每步毫秒
const int MIN_INTERVAL = 95;    // 最快每步毫秒
const int SPEED_STEP = 7;       // 每吃一个加快多少毫秒

Color foodColor() { return rgba(226, 84, 72); }
Color foodHi() { return rgba(255, 150, 130); }
Color headColor() { return rgba(120, 226, 160); }
Color bodyColor(int i, int n) {
  // 身体从头部到尾部逐渐变暗
  int t = (n > 1) ? (i * 110 / (n - 1)) : 0;
  return lerpColor(rgba(72, 196, 128), rgba(30, 96, 70), t);
}

}  // namespace

GameSnake::GameSnake()
    : score_(0),
      dir_(1),
      pending_(1),
      stepAcc_(0),
      interval_(BASE_INTERVAL),
      len_(0),
      food_(0),
      grow_(0),
      over_(false),
      overFlag_(false),
      dragging_(false),
      downX_(0),
      downY_(0),
      moved_(0),
      flashMs_(0) {
  memset(cells_, 0, sizeof(cells_));
}

const char *GameSnake::title() const { return "贪吃蛇"; }
const char *GameSnake::desc() const { return "滑动转向，吃食物变长"; }
const char *GameSnake::tag() const { return "SNAKE"; }
Color GameSnake::theme() const { return rgba(72, 196, 128); }

void GameSnake::reset() {
  memset(cells_, 0, sizeof(cells_));
  // 初始蛇：水平放在中间，头朝右
  int r = ROWS / 2;
  int c = COLS / 2 - START_LEN / 2;
  for (int i = 0; i < START_LEN; ++i) {
    cells_[i].r = r;
    cells_[i].c = c + (START_LEN - 1 - i);  // cells_[0] 是头（最右）
  }
  len_ = START_LEN;
  dir_ = 1;
  pending_ = 1;
  stepAcc_ = 0;
  interval_ = BASE_INTERVAL;
  grow_ = 0;
  score_ = 0;
  over_ = false;
  overFlag_ = false;
  dragging_ = false;
  moved_ = 0;
  flashMs_ = 0;
  // 第一个食物固定放在蛇头正前方几格：开局就能吃到，新手立刻明白规则
  int ahead = cells_[0].c + 5;
  if (ahead >= COLS) ahead = COLS - 1;
  food_ = cells_[0].r * COLS + ahead;
  if (occupied(cells_[0].r, ahead)) spawnFood();
}

void GameSnake::spawnFood() {
  // 收集空格（蛇占的格子不能用），在其中随机取一个
  int r = rand() % ROWS;
  int c = rand() % COLS;
  int guard = 0;
  while (occupied(r, c) && ++guard < 2000) {
    r = rand() % ROWS;
    c = rand() % COLS;
  }
  if (occupied(r, c)) {
    // 极端情况（几乎填满）：线性找第一个空格
    for (int y = 0; y < ROWS && occupied(r, c); ++y)
      for (int x = 0; x < COLS && occupied(r, c); ++x)
        if (!occupied(y, x)) {
          r = y;
          c = x;
        }
  }
  food_ = r * COLS + c;
}

bool GameSnake::occupied(int r, int c) const {
  for (int i = 0; i < len_; ++i) {
    if (cells_[i].r == r && cells_[i].c == c) return true;
  }
  return false;
}

void GameSnake::step() {
  dir_ = pending_;
  int dr = 0, dc = 0;
  switch (dir_) {
    case 0: dr = -1; break;  // 上
    case 1: dc = 1; break;   // 右
    case 2: dr = 1; break;   // 下
    case 3: dc = -1; break;  // 左
  }
  int nr = cells_[0].r + dr;
  int nc = cells_[0].c + dc;

  // 撞墙
  if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) {
    over_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    sfx(SFX_OVER);
    saveBestIfNeeded(score_);
    return;
  }
  // 咬到自己（尾巴那一格这一步会让开，所以最后一段不算）
  int bodyLen = len_ - ((grow_ > 0) ? 0 : 1);
  for (int i = 0; i < bodyLen; ++i) {
    if (cells_[i].r == nr && cells_[i].c == nc) {
      over_ = true;
      overFlag_ = true;
      state_ = GSTATE_OVER;
      sfx(SFX_OVER);
      saveBestIfNeeded(score_);
      return;
    }
  }

  bool eat = (nr * COLS + nc == food_);
  // 身体整体后移一格：从尾往头挪，避免覆盖
  int last = (len_ < MAX_LEN) ? len_ : MAX_LEN - 1;
  for (int i = last; i > 0; --i) cells_[i] = cells_[i - 1];
  cells_[0].r = nr;
  cells_[0].c = nc;

  if (eat) {
    if (len_ < MAX_LEN) ++len_;
    grow_ += 1;
    score_ += FOOD_SCORE;
    flashMs_ = 140;
    if (interval_ > MIN_INTERVAL) interval_ -= SPEED_STEP;
    sfx(SFX_SCORE);
    saveBestIfNeeded(score_);
    spawnFood();
  } else if (grow_ > 0) {
    --grow_;
  }
}

void GameSnake::update(int dtMs) {
  if (flashMs_ > 0) flashMs_ -= dtMs;
  if (!isPlaying() || over_) return;
  stepAcc_ += dtMs;
  // 一帧内可能要跑多步（interval 小 + 帧率低时）
  int guard = 0;
  while (stepAcc_ >= interval_ && !over_ && ++guard < 8) {
    stepAcc_ -= interval_;
    step();
  }
}

void GameSnake::layout(int &bx, int &by, int &cell) const {
  cell = CELL;
  int w = COLS * cell;
  int h = ROWS * cell;
  bx = (vw() - w) / 2;
  by = (vh() - h) / 2;
  if (bx < PAD) {
    bx = PAD;
    cell = (vw() - PAD * 2) / COLS;
    if (cell < 6) cell = 6;
    by = (vh() - ROWS * cell) / 2;
  }
}

void GameSnake::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();

  /* 背景 + 场地 + 细网格 = **一张静态图**（素材化，2026-09-15）。
   * 原来这里是：1 次全屏 fillRect + 2 次大面积 fillRect + 42 条半透明网格线
   * （vline/hline 是逐像素混合）—— 实测这一坨就是本作 40fps 的瓶颈。
   * 底图整图不透明 ⇒ 走 memcpy 快路径。场地布局在 tools/gen_game_art.py 里复刻，
   * **改 COLS/ROWS/CELL/PAD 要两边一起改**。 */
  blit(c, gameart::kSnakeBg, 0, 0);

  int bx, by, cell;
  layout(bx, by, cell);   // 蛇 / 食物的坐标还是要用它（只是场地本身已经画在图里了）

  // food（吃到的瞬间放大闪一下）
  {
    int fr = food_ / COLS;
    int fc = food_ % COLS;
    int cx = bx + fc * cell + cell / 2;
    int cy = by + fr * cell + cell / 2;
    int r = cell / 2 - 3;
    if (flashMs_ > 0) r += 3;
    c.fillCircle(cx, cy, r, foodColor());
    c.fillCircle(cx - r / 3, cy - r / 3, r / 3, foodHi());
    // 小叶子
    c.fillRect(cx + 1, cy - r - 3, 2, 5, rgba(120, 200, 110));
  }
  // 蛇：从尾往头画，头在最上面
  for (int i = len_ - 1; i >= 0; --i) {
    int r = cells_[i].r;
    int cc = cells_[i].c;
    int x = bx + cc * cell;
    int y = by + r * cell;
    int inset = (i == 0) ? 1 : 2;
    Color col = (i == 0) ? headColor() : bodyColor(i, len_);
    c.fillRectRound(x + inset, y + inset, cell - inset * 2, cell - inset * 2,
                    (i == 0) ? 7 : 5, col);
    if (i == 0) {
      // 眼睛：按当前方向朝向
      int ex1, ey1, ex2, ey2;
      int o = cell / 4;
      int m = cell / 2;
      switch (dir_) {
        case 0: ex1 = m - o; ey1 = o; ex2 = m + o; ey2 = o; break;
        case 2: ex1 = m - o; ey1 = cell - o; ex2 = m + o; ey2 = cell - o; break;
        case 3: ex1 = o; ey1 = m - o; ex2 = o; ey2 = m + o; break;
        default: ex1 = cell - o; ey1 = m - o; ex2 = cell - o; ey2 = m + o; break;
      }
      c.fillCircle(x + ex1, y + ey1, 2, rgba(20, 40, 30));
      c.fillCircle(x + ex2, y + ey2, 2, rgba(20, 40, 30));
    }
  }

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 110);
    c.textCenter(W / 2, H / 2 - 110, "贪吃蛇", 3, rgba(120, 226, 160));
    c.textCenter(W / 2, H / 2 - 30, "滑动屏幕转向", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 6, "吃食物变长，别撞墙", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  } else if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 110, "游戏结束", 2, rgba(255, 255, 255));
    char buf[64];
    snprintf(buf, sizeof(buf), "得分 %d", score_);
    c.textCenter(W / 2, H / 2 - 40, buf, 2, rgba(250, 214, 78));
    snprintf(buf, sizeof(buf), "长度 %d", len_);
    c.textCenter(W / 2, H / 2 + 4, buf, 2, rgba(200, 200, 210));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕再来一局", 2,
                 rgba(220, 220, 220));
  } else if (state_ == GSTATE_PAUSED) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 20, "已暂停", 3, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 40, "按暂停键继续", 2, rgba(180, 180, 190));
  }
}

bool GameSnake::onTouch(int action, int x, int y) {
  /*
   * ⚠️ 下面三个「开始 / 继续 / 重来」分支必须挂在 **UP（抬手）** 上，不能挂在 DOWN 上。
   *
   * 原因：一次点击会先后到达 DOWN 和 UP 两个事件。若在 DOWN 时就把状态改成 RUNNING，
   * 紧接着的那个 UP 就会落到下面「没滑动 = 点一下 → 暂停」的逻辑里，于是
   * **点一下想开始、结果变成"已暂停"** —— 2026-09-12 用户报的
   * "触摸可以暂停，不能开始"正是这个（三个分支全中：未开始 / 已暂停 / 结束后重开）。
   * 挂到 UP 上，本次触摸就只发生一次状态切换，不会被同一次手势反转。
   */
  if (over_) {
    if (action == PG_TOUCH_UP) {
      reset();
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_UP) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (state_ == GSTATE_PAUSED) {
    if (action == PG_TOUCH_UP) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;

  if (action == PG_TOUCH_DOWN) {
    dragging_ = true;
    downX_ = x;
    downY_ = y;
    moved_ = 0;
    return true;
  }
  if (action == PG_TOUCH_MOVE) {
    if (!dragging_) return true;
    // 异常跳变（触摸驱动噪声 / 注入抖动）只重同步基准，不产生转向
    // —— 与 2048 / 俄罗斯方块同一套保护（2026-09-13 检讨发现只有那两个有）
    if (abs(x - downX_) > 160 || abs(y - downY_) > 160) {
      downX_ = x;
      downY_ = y;
      return true;
    }
    int dx = x - downX_;
    int dy = y - downY_;
    if (abs(dx) < 22 && abs(dy) < 22) return true;
    int want;
    if (abs(dx) > abs(dy)) {
      want = (dx > 0) ? 1 : 3;
    } else {
      want = (dy > 0) ? 2 : 0;
    }
    turn(want);
    // 以当前位置为新起点，支持连续转向
    downX_ = x;
    downY_ = y;
    ++moved_;
    return true;
  }
  if (action == PG_TOUCH_UP) {
    dragging_ = false;
    // 没滑动 = 点一下 → 暂停
    if (moved_ == 0 && state_ == GSTATE_RUNNING) {
      state_ = GSTATE_PAUSED;
      sfx(SFX_CLICK);
    }
    return true;
  }
  return true;
}

void GameSnake::turn(int want) {
  if (want == dir_) return;
  // 不能 180° 反向（会立刻咬到自己）
  if ((want + 2) % 4 == dir_) return;
  if (want == pending_) return;
  pending_ = want;
  sfx(SFX_MOVE);
}

bool GameSnake::onKey(int key) {
  if (over_ && key == PG_KEY_A) {
    reset();
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (state_ == GSTATE_READY && key == PG_KEY_A) {
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (state_ == GSTATE_PAUSED && key == PG_KEY_A) {
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    return true;
  }
  return false;
}

const char *GameSnake::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", len_);
  return buf;
}

const char *GameSnake::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", interval_);
  return buf;
}

const char *GameSnake::hint() const {
  if (over_) return "撞到了 - 点击屏幕或按暂停键重来";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键或点击继续";
  return "滑动转向，吃食物变长";
}

bool GameSnake::justGameOver() { return overFlag_; }
void GameSnake::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool GameSnake::stillFrame() const {
  return stillUnless(flashMs_);
}

}  // namespace pg
