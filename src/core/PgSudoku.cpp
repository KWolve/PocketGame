/*
 * PgSudoku.cpp - 数独（9x9 · 木纹棋盘 · 唯一解生成 · 三档难度）
 *
 * 玩法：点格子选中 → 点下方数字键填数；冲突的格子标红；填满且全对即完成。
 *   · 「笔记」模式：数字键变成"候选数"开关（一格可记多个），像纸上做题那样。
 *   · 「提示」给一格正确答案（扣分），「难度」三档循环（简单 40 提示数 / 中等 34 / 困难 28）。
 *   · 不设倒计时、不做失败惩罚（填错只是标红，随时可擦）。
 *
 * 视觉：**木纹实体乐器风**（与节奏钢琴/打鼓同一套令牌）。静态部分全在
 *   `resources/images/game/sudoku/` 的三张 PNG 里（改图只换文件），
 *   只有"高亮层 / 数字 / 候选数 / 覆盖层文字"是运行时绘制 —— 见
 *   docs/wood-instruments.md 的"哪些东西该烘成图"。
 *
 * ★ 为什么高亮用 `blendRect` 而不是再烘几张"选中格"的图：
 *   高亮格的位置**每帧都变**（选不同的格子），张数 = 81 × 4 种状态，烘图不划算；
 *   而 blendRect 是纯像素 lerp（没有 sqrt、没有 AA 分支），一帧最多 25 格 × 44² ≈
 *   4.8 万像素 —— 实测远低于 1ms。**"反复绘制的固定图形才出图"** 这条纪律反过来说
 *   就是："位置每帧变的、小面积的，代码画"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PgGames.h"
#include "core/PgLog.h"
#include "core/PgSpriteDraw.h"

namespace pg {

namespace {

using gameart::kSudokuActBtn;
using gameart::kSudokuActGap;
using gameart::kSudokuActH;
using gameart::kSudokuActN;
using gameart::kSudokuActW;
using gameart::kSudokuActX;
using gameart::kSudokuActY;
using gameart::kSudokuBg;
using gameart::kSudokuBX;
using gameart::kSudokuBY;
using gameart::kSudokuCell;
using gameart::kSudokuNumBtn;
using gameart::kSudokuNumGap;
using gameart::kSudokuNumH;
using gameart::kSudokuNumW;
using gameart::kSudokuNumX;
using gameart::kSudokuNumY;

// 配色（与素材生成器对齐；**格底色也要写在这里**，因为高亮是画在格上的半透明层）
const Color kCellBg = rgb(0xE4D8C2);
const Color kDigitGiven = rgb(0x2B1B0E);
const Color kDigitUser = rgb(0x1A4FA0);
const Color kDigitBad = rgb(0xC02020);
const Color kMarkCol = rgb(0x6B5A44);
const Color kBtnText = rgb(0xF6EFE2);

enum { ACT_ERASE = 0, ACT_NOTE, ACT_DIFF, ACT_HINT, ACT_NEW };
const char *const kActLabels[5] = {"擦除", "笔记", "难度", "提示", "新局"};

const char *const kDiffNames[3] = {"简单", "中等", "困难"};

// 每档难度挖掉多少格（题面 = 81 - 挖掉）
const int kHoles[3] = {41, 47, 53};
const int kCelebrateMs = 2200;
const int kFlashMs = 420;

/* ---- 确定性随机（xorshift32）：QA 给了种子就能**逐字复现**同一局 ---- */
inline unsigned rndNext(unsigned &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
inline int rndInt(unsigned &s, int n) { return n > 0 ? (int)(rndNext(s) % (unsigned)n) : 0; }

/* ---- 数独求解/数解 ---- */
inline bool canPut(const int *b, int idx, int v) {
  int r = idx / 9, c = idx % 9;
  for (int i = 0; i < 9; ++i) {
    if (b[r * 9 + i] == v) return false;
    if (b[i * 9 + c] == v) return false;
  }
  int br = (r / 3) * 3, bc = (c / 3) * 3;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (b[(br + i) * 9 + bc + j] == v) return false;
    }
  }
  return true;
}

/* 随机填满一个完整合法盘面（就是"标准答案"）。 */
bool fillFull(int *b, int idx, unsigned &rng) {
  if (idx >= 81) return true;
  if (b[idx]) return fillFull(b, idx + 1, rng);
  int ord[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  for (int i = 8; i > 0; --i) {
    int j = rndInt(rng, i + 1);
    int t = ord[i];
    ord[i] = ord[j];
    ord[j] = t;
  }
  for (int k = 0; k < 9; ++k) {
    if (!canPut(b, idx, ord[k])) continue;
    b[idx] = ord[k];
    if (fillFull(b, idx + 1, rng)) return true;
    b[idx] = 0;
  }
  return false;
}

/* 数解（最多数到 limit 就收工）。用 MRV（优先选候选最少的空格）⇒ 快一个量级。
 * nodes 上限兜底：真遇到病态盘面就判成"不唯一"（保留那个提示数，宁可题面多一点
 * 也不能让生成卡住 —— 这是**静默失败必须消灭**的反面：宁可保守也不能超时）。 */
int countSol(int *b, int limit, long &nodes, long nodeCap) {
  if (++nodes > nodeCap) return limit;
  int best = -1, bestN = 10;
  for (int i = 0; i < 81; ++i) {
    if (b[i]) continue;
    int n = 0;
    for (int v = 1; v <= 9; ++v) {
      if (canPut(b, i, v)) ++n;
    }
    if (n == 0) return 0;
    if (n < bestN) {
      bestN = n;
      best = i;
      if (n == 1) break;
    }
  }
  if (best < 0) return 1;   // 填满了 = 一个解
  int found = 0;
  for (int v = 1; v <= 9; ++v) {
    if (!canPut(b, best, v)) continue;
    b[best] = v;
    found += countSol(b, limit - found, nodes, nodeCap);
    b[best] = 0;
    if (found >= limit) return found;
  }
  return found;
}

}  // namespace

// ---------------- 卡片元信息 ----------------
const char *GameSudoku::title() const { return "数独"; }
const char *GameSudoku::desc() const { return "9x9 数独，三档难度，每局唯一解"; }
const char *GameSudoku::tag() const { return "SUDOKU"; }
Color GameSudoku::theme() const { return rgb(0xB4763A); }

// ---------------- 出题 ----------------
void GameSudoku::setDiff(int d) {
  if (d < 0) d = 0;
  if (d > 2) d = 2;
  diff_ = d;
}

bool GameSudoku::genPuzzle(unsigned seed, int diff) {
  if (seed == 0) seed = 0x1234ABCDu;
  unsigned rng = seed | 1u;
  for (int i = 0; i < 81; ++i) sol_[i] = 0;
  if (!fillFull(sol_, 0, rng)) {
    /* 理论上不可能失败（空盘一定可填）。真失败就打日志 —— 静默失败是要消灭的东西。 */
    logInfo("Sudoku: 生成完整盘面失败（seed=%u）", seed);
    return false;
  }

  int holes = kHoles[diff < 0 ? 0 : (diff > 2 ? 2 : diff)];
  for (int i = 0; i < 81; ++i) cur_[i] = sol_[i];
  int order[81];
  for (int i = 0; i < 81; ++i) order[i] = i;
  for (int i = 80; i > 0; --i) {
    int j = rndInt(rng, i + 1);
    int t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  int dug = 0;
  for (int k = 0; k < 81 && dug < holes; ++k) {
    int idx = order[k];
    int save = cur_[idx];
    cur_[idx] = 0;
    int probe[81];
    memcpy(probe, cur_, sizeof(probe));
    long nodes = 0;
    if (countSol(probe, 2, nodes, 60000L) != 1) {
      cur_[idx] = save;   // 挖了就不唯一 ⇒ 退回（保留这个提示数）
    } else {
      ++dug;
    }
  }
  for (int i = 0; i < 81; ++i) {
    given_[i] = cur_[i];
    mark_[i] = 0;
  }
  logInfo("Sudoku: 新局 seed=%u 难度=%d 挖掉 %d 格（目标 %d）", seed, diff_, dug, holes);
  return true;
}

// ---------------- 生命周期 ----------------
void GameSudoku::reset() {
  sel_ = -1;
  noteMode_ = false;
  score_ = 0;
  hints_ = 0;
  celebMs_ = 0;
  flashMs_ = 0;
  flashIdx_ = -1;
  scaleMs_ = 0;
  scaleIdx_ = -1;
  solved_ = false;
  /* 种子：不指定时用时间（每次"新局"都不一样），指定时用于 QA 复现。
   * ⚠️ 用 `seed_ * 2654435761u` 而不是直接 +1：连着开两局若只差 1，
   *    xorshift 的前几个输出会高度相关（**同一局会重复出现**，玩家一眼看出"这局我做过"）。 */
  seed_ = (unsigned)time(0) * 2654435761u + (unsigned)resetCount_++;
  unsigned long t0 = (unsigned long)clock();
  genPuzzle(seed_, diff_);
  genMs_ = (int)(((unsigned long)clock() - t0) * 1000UL / (unsigned long)CLOCKS_PER_SEC);
  recomputeFilled();
  saveBestIfNeeded(score_);
}

void GameSudoku::recomputeFilled() {
  int n = 0;
  for (int i = 0; i < 81; ++i) {
    if (cur_[i]) ++n;
  }
  filled_ = n;
}

void GameSudoku::update(int dtMs) {
  if (flashMs_ > 0) flashMs_ -= dtMs;
  if (scaleMs_ > 0) {
    scaleMs_ -= dtMs;
    if (scaleMs_ <= 0) {
      scaleIdx_ = -1;
    }
  }
  if (celebMs_ > 0) {
    celebMs_ -= dtMs;
    if (celebMs_ <= 0) {
      /* 完成一局：保持难度不变，换一局新的（难度由玩家自己按） */
      solved_ = false;
      reset();
      state_ = GSTATE_RUNNING;
    }
  }
  if (state_ != GSTATE_RUNNING) return;
}

// ---------------- 绘制 ----------------
namespace {

int cellX(int idx) { return kSudokuBX + (idx % 9) * kSudokuCell; }
int cellY(int idx) { return kSudokuBY + (idx / 9) * kSudokuCell; }

int btnX(int i, int x0, int w, int gap) { return x0 + i * (w + gap); }

}  // namespace

void GameSudoku::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  blit(c, kSudokuBg, 0, 0);   // 整屏木纹棋盘（不透明 ⇒ memcpy 快路径）

  /* 冲突掩码**本帧算一次**（81 格 × 27 次比较；在数字着色里还要再用一次，
   * 重复算会让每帧多出 2000+ 次比较 —— 先算好，别偷懒）。 */
  unsigned char bad[81];
  for (int i = 0; i < 81; ++i) bad[i] = (unsigned char)(conflictsAt(i) > 0);

  /* ---- 高亮层（画在数字**下面**，否则会把数字染上一层色）---- */
  const int selRow = sel_ >= 0 ? sel_ / 9 : -1;
  const int selCol = sel_ >= 0 ? sel_ % 9 : -1;
  const int selVal = sel_ >= 0 ? cur_[sel_] : 0;
  const int selBR = selRow >= 0 ? (selRow / 3) * 3 : -1;
  const int selBC = selCol >= 0 ? (selCol / 3) * 3 : -1;
  for (int i = 0; i < 81; ++i) {
    const int r = i / 9, col = i % 9;
    int a = 0;
    Color hc = rgb(0xFFFFFF);
    if (i == sel_) {
      a = 96;
      hc = rgb(0xFFD98A);
    } else if (sel_ >= 0 && r >= selBR && r < selBR + 3 && col >= selBC && col < selBC + 3) {
      a = 52;   // 同宫
    } else if (sel_ >= 0 && (r == selRow || col == selCol)) {
      a = 40;   // 同行同列
    }
    if (selVal && cur_[i] == selVal && i != sel_) {
      a = 74;
      hc = rgb(0x9FD6FF);   // 相同数字：一眼看清"这个数还能放哪"
    }
    if (flashMs_ > 0 && i == flashIdx_) {
      a = 120;
      hc = rgb(0xA8FFB0);
    }
    if (a > 0) c.blendRect(cellX(i), cellY(i), kSudokuCell, kSudokuCell, hc, a);
  }

  /* ---- 冲突标红（在数字下面画一层淡红）---- */
  for (int i = 0; i < 81; ++i) {
    if (cur_[i] && bad[i]) {
      c.blendRect(cellX(i), cellY(i), kSudokuCell, kSudokuCell, rgb(0xFF3B30), 86);
    }
  }

  /* ---- 数字与候选数 ---- */
  const int gh = c.textH(3);
  for (int i = 0; i < 81; ++i) {
    const int x = cellX(i), y = cellY(i);
    const int v = cur_[i];
    if (v) {
      const Color col = bad[i] ? kDigitBad : (given_[i] ? kDigitGiven : kDigitUser);
      const int w = c.numberW(v, 3);
      c.number(x + (kSudokuCell - w) / 2, y + (kSudokuCell - gh) / 2, v, 3, col);
    } else if (mark_[i]) {
      for (int k = 0; k < 9; ++k) {
        if (!(mark_[i] & (1 << k))) continue;
        const int mx = x + (k % 3) * 14 + 3;
        const int my = y + (k / 3) * 14 + 1;
        c.number(mx, my, k + 1, 1, kMarkCol);
      }
    }
  }

  /* ---- 数字键行 ---- */
  for (int i = 0; i < 9; ++i) {
    const int x = btnX(i, kSudokuNumX, kSudokuNumW, kSudokuNumGap);
    blit(c, kSudokuNumBtn, x, kSudokuNumY);
    const Color tc = noteMode_ ? rgb(0xFFE9A8) : kBtnText;
    const int w = c.numberW(i + 1, 3);
    c.number(x + (kSudokuNumW - w) / 2, kSudokuNumY + (kSudokuNumH - gh) / 2,
             i + 1, 3, tc);
  }

  /* ---- 动作键行 ---- */
  for (int i = 0; i < kSudokuActN; ++i) {
    const int x = btnX(i, kSudokuActX, kSudokuActW, kSudokuActGap);
    blit(c, kSudokuActBtn, x, kSudokuActY);
    const char *lab = kActLabels[i];
    Color tc = kBtnText;
    if (i == ACT_NOTE && noteMode_) tc = rgb(0xFFD24A);   // 笔记开着：亮黄
    if (i == ACT_HINT) tc = rgb(0xBFE8FF);
    int tw = c.textW(lab, 2);
    int extra = 0;
    if (i == ACT_DIFF) extra = c.numberW(diff_ + 1, 2);
    int y = kSudokuActY + (kSudokuActH - c.textH(2)) / 2;
    c.text(x + (kSudokuActW - tw - extra) / 2, y, lab, 2, tc);
    if (i == ACT_DIFF) {
      c.number(x + (kSudokuActW - tw - extra) / 2 + tw, y, diff_ + 1, 2, rgb(0xFFD24A));
    }
  }

  /* ---- 覆盖层 ---- */
  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 110, "数独", 5, rgb(0xFFD98A));
    /* ★ 文案宽度：画布 480、两侧各留 ≥16 ⇒ 上限 448px。档 2 的中文 = 32px/字
     *   ⇒ 最多 13 字。下面两行分别是 10 字与 9 字。改文案后必跑
     *   `tools/check_textwidth.py`（超宽会被屏裁，而功能断言全绿）。 */
    c.textCenter(W / 2, H / 2 - 24, "点格子，再点数字填数", 2, rgb(0xF2F2F7));
    c.textCenter(W / 2, H / 2 + 18, "笔记可记候选数", 2, rgb(0xC7C7CC));
    c.textCenter(W / 2, H / 2 + 60, "点屏幕开始", 2, rgb(0xFFD98A));
  } else if (celebMs_ > 0) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 130);
    c.textCenter(W / 2, H / 2 - 70, "完成！", 5, rgb(0x8CF0A8));
    /* 手工排版（不用 snprintf 拼串传给 text —— 那会让 genfont 退化成
     * "本文件全部短字面量"，见 MEMORY.md 规则 9）。 */
    const int wA = c.textW("提示", 2);
    const int wB = c.numberW(hints_, 2);
    const int wC = c.textW("次", 2);
    int x = (W - (wA + wB + wC)) / 2;
    const int y = H / 2 + 7;
    c.text(x, y, "提示", 2, rgb(0xF2F2F7));
    x += wA;
    c.number(x, y, hints_, 2, rgb(0xFFD24A));
    x += wB;
    c.text(x, y, "次", 2, rgb(0xF2F2F7));
  }
}

// ---------------- 输入 ----------------
int GameSudoku::cellAt(int x, int y) const {
  const int side = kSudokuCell * 9;
  if (x < kSudokuBX || x >= kSudokuBX + side) return -1;
  if (y < kSudokuBY || y >= kSudokuBY + side) return -1;
  const int col = (x - kSudokuBX) / kSudokuCell;
  const int row = (y - kSudokuBY) / kSudokuCell;
  return row * 9 + col;
}

int GameSudoku::numAt(int x, int y) const {
  if (y < kSudokuNumY || y >= kSudokuNumY + kSudokuNumH) return -1;
  for (int i = 0; i < 9; ++i) {
    const int bx = btnX(i, kSudokuNumX, kSudokuNumW, kSudokuNumGap);
    if (x >= bx && x < bx + kSudokuNumW) return i;
  }
  return -1;
}

int GameSudoku::actAt(int x, int y) const {
  if (y < kSudokuActY || y >= kSudokuActY + kSudokuActH) return -1;
  for (int i = 0; i < kSudokuActN; ++i) {
    const int bx = btnX(i, kSudokuActX, kSudokuActW, kSudokuActGap);
    if (x >= bx && x < bx + kSudokuActW) return i;
  }
  return -1;
}

int GameSudoku::conflictsAt(int idx) const {
  const int v = cur_[idx];
  if (!v) return 0;
  const int r = idx / 9, c = idx % 9;
  int n = 0;
  for (int i = 0; i < 9; ++i) {
    if (i != c && cur_[r * 9 + i] == v) ++n;
    if (i != r && cur_[i * 9 + c] == v) ++n;
  }
  const int br = (r / 3) * 3, bc = (c / 3) * 3;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      const int k = (br + i) * 9 + bc + j;
      if (k != idx && cur_[k] == v) ++n;
    }
  }
  return n;
}

void GameSudoku::placeSelected(int v) {
  if (sel_ < 0 || v < 1 || v > 9) return;
  if (given_[sel_]) return;                 // 题面格不可改
  if (noteMode_) {
    mark_[sel_] ^= (unsigned short)(1 << (v - 1));
    /* 记候选数也给个声音反馈（**音高跟着数字走** —— 从钢琴那 8 个音里借，
     * 顺手让"数感"这件事多一个听觉通道；SFX_PNO1..8 在枚举里是连续的）。 */
    sfx(SFX_PNO1 + ((v - 1) & 7));
    return;
  }
  if (cur_[sel_] == v) {
    cur_[sel_] = 0;                         // 再点一次同一个数字 = 擦掉（少按一次"擦除"）
  } else {
    cur_[sel_] = v;
    mark_[sel_] = 0;
    flashIdx_ = sel_;
    flashMs_ = kFlashMs;
    sfx(SFX_CLICK);
    if (v == sol_[sel_]) {
      score_ += 10;
      saveBestIfNeeded(score_);
      sfx(SFX_SCORE);
    }
  }
  recomputeFilled();

  /* 完成判定：填满 + 与解完全一致 */
  if (!solved_ && filled_ >= 81) {
    bool ok = true;
    for (int i = 0; i < 81; ++i) {
      if (cur_[i] != sol_[i]) ok = false;
    }
    if (ok) {
      solved_ = true;
      celebMs_ = kCelebrateMs;
      sfx(SFX_CLEAR);
      logInfo("Sudoku: 完成（难度 %d，提示 %d 次，得分 %d）", diff_, hints_, score_);
    }
  }
}

void GameSudoku::eraseSelected() {
  if (sel_ < 0 || given_[sel_]) return;
  cur_[sel_] = 0;
  mark_[sel_] = 0;
  recomputeFilled();
  sfx(SFX_MOVE);
}

void GameSudoku::hintOne() {
  /* 提示优先给"**选中的格子**"，没选就给第一个空格 —— 玩家的意图总是"我卡在这格"。 */
  int target = -1;
  if (sel_ >= 0 && !given_[sel_] && cur_[sel_] != sol_[sel_]) {
    target = sel_;
  } else {
    for (int i = 0; i < 81; ++i) {
      if (!given_[i] && cur_[i] != sol_[i]) {
        target = i;
        break;
      }
    }
  }
  if (target < 0) {
    logInfo("Sudoku: 提示无格可给（盘面已与解一致）");
    return;
  }
  sel_ = target;
  cur_[target] = sol_[target];
  mark_[target] = 0;
  ++hints_;
  score_ -= 15;
  if (score_ < 0) score_ = 0;
  recomputeFilled();
  sfx(SFX_MERGE);
  logInfo("Sudoku: 提示格 %d,%d = %d（第 %d 次）", target / 9, target % 9, sol_[target],
          hints_);
  if (!solved_ && filled_ >= 81) {
    bool ok = true;
    for (int i = 0; i < 81; ++i) {
      if (cur_[i] != sol_[i]) ok = false;
    }
    if (ok) {
      solved_ = true;
      celebMs_ = kCelebrateMs;
      sfx(SFX_CLEAR);
      logInfo("Sudoku: 完成（难度 %d，提示 %d 次，得分 %d）", diff_, hints_, score_);
    }
  }
}

bool GameSudoku::onTouch(int action, int x, int y) {
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (!isPlaying()) return true;
  if (action != PG_TOUCH_DOWN) return true;
  if (celebMs_ > 0) return true;

  const int act = actAt(x, y);
  if (act >= 0) {
    logInfo("Sudoku: 动作键 %d（%s）", act, kActLabels[act]);
    switch (act) {
      case ACT_ERASE: eraseSelected(); break;
      case ACT_NOTE:
        noteMode_ = !noteMode_;
        sfx(SFX_ROTATE);
        logInfo("Sudoku: 笔记模式 %s", noteMode_ ? "开" : "关");
        break;
      case ACT_DIFF:
        setDiff(diff_ + 1 > 2 ? 0 : diff_ + 1);
        logInfo("Sudoku: 难度 -> %d", diff_ + 1);
        reset();
        state_ = GSTATE_RUNNING;
        break;
      case ACT_HINT: hintOne(); break;
      case ACT_NEW:
        reset();
        state_ = GSTATE_RUNNING;
        break;
      default: break;
    }
    return true;
  }

  const int n = numAt(x, y);
  if (n >= 0) {
    logInfo("Sudoku: 数字键 %d", n + 1);
    placeSelected(n + 1);
    return true;
  }

  const int cell = cellAt(x, y);
  if (cell >= 0) {
    sel_ = (sel_ == cell) ? -1 : cell;
    scaleIdx_ = cell;
    scaleMs_ = 900;
    sfx(SFX_MOVE);
    logInfo("Sudoku: 选中格 %d,%d（题面=%d 当前=%d）", cell / 9, cell % 9, given_[cell],
            cur_[cell]);
    return true;
  }
  return true;
}

bool GameSudoku::onKey(int key) {
  if (key == PG_KEY_A) {
    if (state_ == GSTATE_READY) state_ = GSTATE_RUNNING;
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

// ---------------- HUD ----------------
const char *GameSudoku::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%s", kDiffNames[diff_ < 0 ? 0 : (diff_ > 2 ? 2 : diff_)]);
  return buf;
}

const char *GameSudoku::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", 81 - filled_);
  return buf;
}

const char *GameSudoku::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点格子再点数字 · 笔记可记候选数";
}

// ---------------- QA ----------------
void GameSudoku::debugCmd(const char *rest) {
  char cmd[24];
  int k = 0;
  while (rest[k] && rest[k] != ' ' && k < 23) {
    cmd[k] = rest[k];
    ++k;
  }
  cmd[k] = 0;
  const char *arg = rest + k;
  while (*arg == ' ') ++arg;

  if (strcmp(cmd, "q") == 0) {
    char line[220];
    int off = snprintf(line, sizeof(line), "qa sudoku diff=%d sel=%d filled=%d hints=%d "
                                          "score=%d seed=%u genms=%d |",
                       diff_ + 1, sel_, filled_, hints_, score_, seed_, genMs_);
    for (int r = 0; r < 9; ++r) {
      for (int c2 = 0; c2 < 9; ++c2) {
        const int i = r * 9 + c2;
        const char ch = (char)('0' + (cur_[i] ? cur_[i] : 0));
        line[off++] = (given_[i] && cur_[i]) ? ch : (cur_[i] ? ch : '.');
      }
      line[off++] = ' ';
    }
    line[off] = 0;
    logInfo("%s", line);
    return;
  }
  if (strcmp(cmd, "new") == 0) {
    if (*arg) seed_ = (unsigned)strtoul(arg, 0, 10) | 1u;
    reset();
    logInfo("qa sudoku new seed=%u diff=%d", seed_, diff_ + 1);
    return;
  }
  if (strcmp(cmd, "level") == 0) {
    setDiff(atoi(arg) - 1);
    reset();
    logInfo("qa sudoku level=%d", diff_ + 1);
    return;
  }
  if (strcmp(cmd, "cell") == 0) {
    int r = 0, c = 0;
    sscanf(arg, "%d %d", &r, &c);
    if (r >= 0 && r < 9 && c >= 0 && c < 9) {
      sel_ = r * 9 + c;
      logInfo("qa sudoku cell=%d,%d", r, c);
    } else {
      logInfo("qa sudoku cell: 越界 %d,%d", r, c);
    }
    return;
  }
  if (strcmp(cmd, "put") == 0) {
    int r = 0, c = 0, v = 0;
    sscanf(arg, "%d %d %d", &r, &c, &v);
    if (r < 0 || r > 8 || c < 0 || c > 8 || v < 0 || v > 9) {
      logInfo("qa sudoku put: 参数越界 %d %d %d", r, c, v);
      return;
    }
    sel_ = r * 9 + c;
    if (v == 0) {
      eraseSelected();
    } else {
      placeSelected(v);
    }
    logInfo("qa sudoku put %d %d %d -> filled=%d score=%d", r, c, v, filled_, score_);
    return;
  }
  if (strcmp(cmd, "one") == 0) {
    hintOne();
    return;
  }
  if (strcmp(cmd, "solve") == 0) {
    /* 把空格全部按正确值填完 —— 走的是**与"提示"同一条**路径（hintOne），
     * 所以验收到的完成流程与实际玩到的一致（不为验收单开一条捷径）。 */
    for (int i = 0; i < 200 && filled_ < 81 && celebMs_ <= 0; ++i) {
      const int before = filled_;
      hintOne();
      if (filled_ == before) break;
    }
    logInfo("qa sudoku solve -> filled=%d celeb=%d", filled_, celebMs_);
    return;
  }
  logInfo("qa sudoku: 未知命令（q/new [seed]/level n/cell r c/put r c v/one/solve）");
}

}  // namespace pg
