/*
 * Pg2048.cpp - 2048（触摸滑动操作）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"

namespace pg {

namespace {

const int GAP = 8;
const int MARGIN = 14;
const int ANIM_MS = 110;   // 滑动时长
const int POP_MS = 150;    // 合成/新块弹跳时长

Color boardBg() { return rgba(58, 58, 66); }
Color emptyBg() { return rgba(76, 76, 86); }
Color bgColor() { return rgba(26, 28, 34); }

Color tileBg(int v) {
  switch (v) {
    case 0: return emptyBg();
    case 2: return rgba(238, 228, 218);
    case 4: return rgba(237, 224, 200);
    case 8: return rgba(242, 177, 121);
    case 16: return rgba(245, 149, 99);
    case 32: return rgba(246, 124, 95);
    case 64: return rgba(246, 94, 59);
    case 128: return rgba(237, 207, 114);
    case 256: return rgba(237, 204, 97);
    case 512: return rgba(237, 200, 80);
    case 1024: return rgba(237, 197, 63);
    case 2048: return rgba(237, 194, 46);
    default: return rgba(60, 58, 50);
  }
}

Color tileFg(int v) {
  if (v <= 4) return rgba(119, 110, 101);
  return rgba(249, 246, 242);
}

int digitsOf(int v) {
  int n = 1;
  while (v >= 10) {
    v /= 10;
    ++n;
  }
  return n;
}

}  // namespace

Game2048::Game2048()
    : score_(0),
      over_(false),
      overFlag_(false),
      won_(false),
      slideCount_(0),
      animMs_(-1),
      pendingEnd_(false),
      popCount_(0),
      popOn_(false),
      popMs_(0),
      mergeCount_(0),
      dragging_(false),
      downX_(0),
      downY_(0),
      lastX_(0),
      lastY_(0),
      consumed_(false) {
  memset(grid_, 0, sizeof(grid_));
  memset(next_, 0, sizeof(next_));
  memset(popCells_, 0, sizeof(popCells_));
}

const char *Game2048::title() const { return "2048"; }
const char *Game2048::desc() const { return "滑动合并数字，凑出 2048"; }
const char *Game2048::tag() const { return "2048"; }
Color Game2048::theme() const { return rgba(237, 194, 46); }

void Game2048::layout(int &bx, int &by, int &cell, int &gap) const {
  gap = GAP;
  int avail = vw() - 2 * MARGIN;
  cell = (avail - 5 * gap) / 4;
  int total = 4 * cell + 5 * gap;
  bx = (vw() - total) / 2 + gap;
  by = (vh() - total) / 2 + gap;
}

int Game2048::tileX(int c, int cell, int gap, int bx) const {
  return bx + c * (cell + gap);
}

int Game2048::tileY(int r, int cell, int gap, int by) const {
  return by + r * (cell + gap);
}

void Game2048::reset() {
  memset(grid_, 0, sizeof(grid_));
  score_ = 0;
  over_ = false;
  overFlag_ = false;
  won_ = false;
  slideCount_ = 0;
  animMs_ = -1;
  pendingEnd_ = false;
  popCount_ = 0;
  popOn_ = false;
  popMs_ = 0;
  mergeCount_ = 0;
  dragging_ = false;
  consumed_ = false;
  spawnTile();
  spawnTile();
}

void Game2048::addPop(int r, int c) {
  if (popCount_ < 8) popCells_[popCount_++] = r * 4 + c;
}

void Game2048::spawnTile() {
  int empty[16];
  int n = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (grid_[r][c] == 0) empty[n++] = r * 4 + c;
  if (n == 0) return;
  int k = empty[rand() % n];
  int r = k / 4, c = k % 4;
  grid_[r][c] = ((rand() % 10) == 0) ? 4 : 2;
  popCount_ = 0;
  addPop(r, c);
  popOn_ = true;
  popMs_ = 0;
}

bool Game2048::moveTiles(int dir) {
  memset(next_, 0, sizeof(next_));
  slideCount_ = 0;
  int added = 0;
  bool moved = false;
  int mergedDest[16];
  int mergedAt = 0;

  for (int line = 0; line < 4; ++line) {
    int vals[4], sr[4], sc[4];
    int n = 0;
    for (int pos = 0; pos < 4; ++pos) {
      int r, c;
      switch (dir) {
        case 0: r = pos; c = line; break;      // 上
        case 2: r = 3 - pos; c = line; break;  // 下
        case 3: r = line; c = pos; break;      // 左
        default: r = line; c = 3 - pos; break; // 右
      }
      if (grid_[r][c]) {
        vals[n] = grid_[r][c];
        sr[n] = r;
        sc[n] = c;
        ++n;
      }
    }
    // 压缩 + 合并
    int outV[4];
    int outSrc[4][2];
    int outSrcN[4];
    int outIsMerge[4];
    int outN = 0;
    int i = 0;
    while (i < n) {
      if (i + 1 < n && vals[i] == vals[i + 1]) {
        outV[outN] = vals[i] * 2;
        outSrc[outN][0] = i;
        outSrc[outN][1] = i + 1;
        outSrcN[outN] = 2;
        outIsMerge[outN] = 1;
        added += outV[outN];
        if (outV[outN] >= 2048) won_ = true;
        ++outN;
        i += 2;
      } else {
        outV[outN] = vals[i];
        outSrc[outN][0] = i;
        outSrcN[outN] = 1;
        outIsMerge[outN] = 0;
        ++outN;
        ++i;
      }
    }
    // 写回 + 记录动画
    for (int k = 0; k < outN; ++k) {
      int r, c;
      switch (dir) {
        case 0: r = k; c = line; break;
        case 2: r = 3 - k; c = line; break;
        case 3: r = line; c = k; break;
        default: r = line; c = 3 - k; break;
      }
      next_[r][c] = outV[k];
      for (int s = 0; s < outSrcN[k]; ++s) {
        int si = outSrc[k][s];
        if (slideCount_ < 32) {
          Slide &sl = slides_[slideCount_++];
          sl.fr = sr[si];
          sl.fc = sc[si];
          sl.tr = r;
          sl.tc = c;
          sl.value = vals[si];
          sl.merged = (outIsMerge[k] && s == 0);
        }
      }
      if (outIsMerge[k]) {
        mergedDest[mergedAt++] = r * 4 + c;
      }
    }
  }

  // 是否发生变化
  for (int r = 0; r < 4 && !moved; ++r)
    for (int c = 0; c < 4; ++c)
      if (next_[r][c] != grid_[r][c]) {
        moved = true;
        break;
      }
  if (!moved) {
    slideCount_ = 0;
    return false;
  }

  score_ += added;
  mergeCount_ = mergedAt;
  if (mergedAt > 0) sfx(SFX_MERGE);
  else sfx(SFX_MOVE);

  // 待弹跳的合成格（动画结束后生效；新生成的格由 spawnTile 追加）
  popCount_ = 0;
  for (int i = 0; i < mergedAt && popCount_ < 8; ++i) {
    addPop(mergedDest[i] / 4, mergedDest[i] % 4);
  }
  animMs_ = 0;
  pendingEnd_ = true;
  return true;
}

bool Game2048::canMove() const {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (grid_[r][c] == 0) return true;
      if (c < 3 && grid_[r][c] == grid_[r][c + 1]) return true;
      if (r < 3 && grid_[r][c] == grid_[r + 1][c]) return true;
    }
  return false;
}

int Game2048::maxTile() const {
  int m = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (grid_[r][c] > m) m = grid_[r][c];
  return m;
}

void Game2048::update(int dtMs) {
  if (popOn_) {
    popMs_ += dtMs;
    if (popMs_ >= POP_MS) {
      popOn_ = false;
      popMs_ = 0;
    }
  }
  if (animMs_ < 0) return;

  animMs_ += dtMs;
  if (animMs_ < ANIM_MS) return;

  // 动画结束：把滑动结果落盘，然后生新块
  animMs_ = -1;
  memcpy(grid_, next_, sizeof(grid_));
  slideCount_ = 0;
  if (pendingEnd_) {
    pendingEnd_ = false;
    int keep = popCount_;
    int keepCells[8];
    for (int i = 0; i < keep && i < 8; ++i) keepCells[i] = popCells_[i];
    spawnTile();  // 内部会重置 popCount_ 并加入新格
    for (int i = 0; i < keep && popCount_ < 8; ++i) {
      int k = keepCells[i];
      bool dup = false;
      for (int j = 0; j < popCount_; ++j)
        if (popCells_[j] == k) dup = true;
      if (!dup) popCells_[popCount_++] = k;
    }
    popOn_ = true;
    popMs_ = 0;
    if (!canMove()) {
      over_ = true;
      overFlag_ = true;
      state_ = GSTATE_OVER;
      sfx(SFX_OVER);
      saveBestIfNeeded(score_);
    }
  }
}

void Game2048::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  c.clear(bgColor());

  int bx, by, cell, gap;
  layout(bx, by, cell, gap);
  int boardW = 4 * cell + 3 * gap;
  c.fillRectRound(bx - gap, by - gap, boardW + 2 * gap, boardW + 2 * gap, 10,
                  boardBg());

  // 底格
  for (int r = 0; r < 4; ++r)
    for (int cc = 0; cc < 4; ++cc)
      c.fillRectRound(tileX(cc, cell, gap, bx), tileY(r, cell, gap, by), cell,
                      cell, 6, emptyBg());

  float t = 1.0f;
  if (animMs_ >= 0) {
    t = (float)animMs_ / (float)ANIM_MS;
    if (t > 1.0f) t = 1.0f;
    // ease-out
    t = 1.0f - (1.0f - t) * (1.0f - t);
  }

  if (animMs_ >= 0) {
    // 动画中：只画滑动的块
    for (int i = 0; i < slideCount_; ++i) {
      const Slide &sl = slides_[i];
      int fx = tileX(sl.fc, cell, gap, bx);
      int fy = tileY(sl.fr, cell, gap, by);
      int tx = tileX(sl.tc, cell, gap, bx);
      int ty = tileY(sl.tr, cell, gap, by);
      int x = fx + (int)((tx - fx) * t);
      int y = fy + (int)((ty - fy) * t);
      c.fillRectRound(x, y, cell, cell, 6, tileBg(sl.value));
    }
  } else {
    // 静止：画棋盘 + 弹跳
    for (int r = 0; r < 4; ++r)
      for (int cc = 0; cc < 4; ++cc) {
        int v = grid_[r][cc];
        if (!v) continue;
        int x = tileX(cc, cell, gap, bx);
        int y = tileY(r, cell, gap, by);
        int w = cell, h = cell;
        bool popping = false;
        if (popOn_) {
          for (int i = 0; i < popCount_; ++i)
            if (popCells_[i] == r * 4 + cc) popping = true;
        }
        if (popping) {
          float p = (float)popMs_ / (float)POP_MS;
          float bump = (p < 0.5f) ? (p * 2.0f) : ((1.0f - p) * 2.0f);
          int grow = (int)(cell * 0.16f * bump);
          x -= grow / 2;
          y -= grow / 2;
          w = cell + grow;
          h = cell + grow;
        }
        c.fillRectRound(x, y, w, h, 6, tileBg(v));
        // 数字：大号点阵 + 按格子自动选档。
        // 原来是手写的 scale = 11/位数，会出现"3 位数反而比 2 位数小"的跳变；
        // fitBigText 是按真实可用宽高算的，位数越多自动降一档。
        // 基准用 cell（不是弹跳时的 w/h），否则合成动画里字号会来回跳。
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", v);
        int sc = c.fitBigText(cell - 4, cell - 8, buf, 3);
        int tw = c.bigTextW(buf, sc);
        int th = c.bigTextH(sc);
        c.bigText(x + (w - tw) / 2, y + (h - th) / 2, buf, sc, tileFg(v));
      }
  }

  // 顶部/底部覆盖提示
  if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    c.textCenter(W / 2, H / 2 - 70, "GAME OVER", 4, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 10, "点击屏幕再来一局", 2, rgba(230, 230, 230));
    char b[64];
    snprintf(b, sizeof(b), "本局得分 %d", score_);
    c.textCenter(W / 2, H / 2 + 50, b, 2, rgba(242, 179, 61));
  } else if (won_) {
    c.blendRect(0, 0, W, 60, rgba(237, 194, 46), 90);
    c.textCenter(W / 2, 14, "达成 2048 !", 2, rgba(255, 255, 255));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 120);
    c.textCenter(W / 2, H / 2 - 40, "2048", 5, rgba(237, 194, 46));
    c.textCenter(W / 2, H / 2 + 40, "手指滑动合并数字", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 76, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool Game2048::onTouch(int action, int x, int y) {
  if (over_) {
    if (action == PG_TOUCH_DOWN) {
      reset();
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
      return true;
    }
    return true;
  }
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
      return true;
    }
    return true;
  }
  if (!isPlaying()) return true;

  if (action == PG_TOUCH_DOWN) {
    dragging_ = true;
    consumed_ = false;
    downX_ = lastX_ = x;
    downY_ = lastY_ = y;
    return true;
  }
  if (action == PG_TOUCH_MOVE) {
    if (!dragging_) return true;
    // 异常跳变（触摸驱动/注入噪声）只做基准重同步，不产生动作
    if (abs(x - lastX_) > 160 || abs(y - lastY_) > 160) {
      downX_ = lastX_ = x;
      downY_ = lastY_ = y;
      return true;
    }
    lastX_ = x;
    lastY_ = y;
    if (animMs_ >= 0) return true;  // 动画期间不收新输入
    int dx = x - downX_;
    int dy = y - downY_;
    const int TH = 26;
    if (abs(dx) >= TH || abs(dy) >= TH) {
      int dir;
      if (abs(dx) > abs(dy)) dir = (dx > 0) ? 1 : 3;
      else dir = (dy > 0) ? 2 : 0;
      moveTiles(dir);
      // 以当前位置为新基准，支持连续滑动
      downX_ = x;
      downY_ = y;
    }
    return true;
  }
  if (action == PG_TOUCH_UP) {
    dragging_ = false;
    if (!consumed_ && animMs_ < 0) {
      int dx = x - downX_;
      int dy = y - downY_;
      const int TH = 18;
      if (abs(dx) >= TH || abs(dy) >= TH) {
        int dir;
        if (abs(dx) > abs(dy)) dir = (dx > 0) ? 1 : 3;
        else dir = (dy > 0) ? 2 : 0;
        moveTiles(dir);
      }
    }
    return true;
  }
  return true;
}

bool Game2048::onKey(int key) {
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

const char *Game2048::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", maxTile());
  return buf;
}

const char *Game2048::hint() const {
  if (over_) return "游戏结束 - 点击屏幕或按暂停键重来";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  if (animMs_ >= 0) return " ";
  return "滑动屏幕合并相同数字";
}

bool Game2048::justGameOver() { return overFlag_; }
void Game2048::clearGameOverFlag() { overFlag_ = false; }

/* 覆盖层静止判定（见 Game::stillFrame）：把**覆盖层上还会动的特效**列进来，
 * 否则它会被冻结在半路（撞机抖屏 / 踩雷爆炸 / 消行闪烁）。 */
bool Game2048::stillFrame() const {
  return stillUnless(animMs_ < 0 ? 0 : animMs_, popMs_);
}

}  // namespace pg
