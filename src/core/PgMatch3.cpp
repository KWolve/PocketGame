/*
 * PgMatch3.cpp - 消消乐（糖果三消 · 触摸优先）
 *
 * 为什么加它（用户 2026-09-15）：「游戏里面再帮我加一个消消乐，UI 效果要参考
 * 《开心消消乐》」—— 参考产品是全民级三消，玩法本身极简（滑动交换 → 三个同色连成
 * 一线就消），但**视觉语言**很具体：
 *   · 清新花园底 + **木质棋盘框** + 浅色糖果凹槽（不是深色科技风）
 *   · 糖果 = 高饱和多彩 + 顶部高光 + 深色描边（果冻/塑料质感），**形状也各不相同**
 *   · 顶栏是木牌进度条（关卡 / 本关目标进度 / 剩余步数）
 * ⇒ 静态部分（底图/糖果/选中框/爆花）全部烘成 PNG（tools/gen_game_art.py 的 match3 段），
 *   本文件只写**动态部分**：交换滑动、消除淡出、下落位移、分数弹出、进度条填充。
 *   这条分工与 docs/game-art-pipeline.md 一致（静态素材化，动态才写代码）。
 *
 * 玩法与关卡（对齐参考产品的"目标 + 步数"两层）：
 *   8x8 棋盘 / 7 种糖果；滑动或点两下交换**相邻**两格；3+ 同色连线消除；
 *   消除后上方糖果下落、顶部补新糖，若能再消则**连锁**（combo 越高分越多）；
 *   每关 25 步，本关分达到目标即过关（目标随关卡递增），步数用尽未达标则重试本关。
 *
 * 三个"必须做对否则很难玩"的细节（都在下面有专门注释）：
 *   ① 开局**不能自带三连**（否则一进游戏就白送分）
 *   ② 无解时必须**自动重排**（否则棋盘卡死，玩家只能干瞪眼）
 *   ③ 无效交换（交换后不产生消除）**不扣步数**，并且要滑回去而不是瞬移
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgSpriteDraw.h"   // blit / blitA + 素材清单 + 日志钩子

namespace pg {

/* ==================== 糖果素材 ====================
 * 索引与 M3_CANDY（tools/gen_game_art.py）**逐位对应**：改颜色/形状只改生成器，
 * 重跑后 PNG 覆盖、本文件不用动（见 docs/game-art-pipeline.md）。
 * 图尺寸 == 格尺寸（56x56）⇒ 锚点 (0,0)，直接按格左上角贴，不涉及任何缩放。 */
static const gameart::Def *candyDef(int t) {
  static const gameart::Def *const kTab[7] = {
      &gameart::kMatch3Candy0, &gameart::kMatch3Candy1, &gameart::kMatch3Candy2,
      &gameart::kMatch3Candy3, &gameart::kMatch3Candy4, &gameart::kMatch3Candy5,
      &gameart::kMatch3Candy6,
  };
  if (t < 0 || t > 6) return 0;
  return kTab[t];
}

namespace {

/* 棋盘几何（必须与 PgGames.h 的 GameMatch3 常量、tools/gen_game_art.py 的 M3_* 一致） */
const int kRows = 8;
const int kCols = 8;
const int kCell = 56;
const int kBy = 46;                                   // 棋盘左上角 y
const int kBoardBottom = kBy + kRows * kCell;         // 494

/* 分数弹出文字的"淡出色"：画布文字没有 alpha 参数，只能把颜色往**它底下的颜色**
 * 插值来假装淡出（与打地鼠的做法一致）。棋盘区域与花园区域的底色差别很大，
 * 所以按 y 分档取近似底色 —— 否则弹出字会在某片区域"越淡越突兀"。 */
Color fadeTargetAt(int y) {
  if (y < kBy) return rgba(46, 90, 62);               // 上方花园
  if (y > kBoardBottom) return rgba(60, 108, 72);     // 下方花园
  return rgba(210, 200, 182);                         // 棋盘（木框 + 浅色凹槽的平均感）
}

int iabs(int v) { return v < 0 ? -v : v; }

}  // namespace

/* ==================== 构造 / 元信息 ==================== */

GameMatch3::GameMatch3() {
  memset(cell_, -1, sizeof(cell_));
  memset(power_, 0, sizeof(power_));
  memset(trig_, 0, sizeof(trig_));
  memset(spawn_, 0, sizeof(spawn_));
  memset(spawnCol_, -1, sizeof(spawnCol_));
  memset(fallFrom_, 0, sizeof(fallFrom_));
  memset(marked_, 0, sizeof(marked_));
  level_ = 1;
  movesLeft_ = MOVES_PER_LEVEL;
  target_ = TARGET_BASE;
  levelScore_ = 0;
  totalScore_ = 0;
  phase_ = PH_PLAY;
  animMs_ = animTotal_ = 0;
  swapA_ = swapB_ = -1;
  combo_ = 0;
  maxCombo_ = 0;
  clearCells_ = 0;
  powerMade_ = 0;
  popMs_ = popVal_ = popX_ = popY_ = 0;
  shufFxMs_ = 0;
  won_ = false;
  overFlag_ = false;
  selR_ = selC_ = -1;
  dragR_ = dragC_ = -1;
  dragX_ = dragY_ = 0;
}

const char *GameMatch3::title() const { return "消消乐"; }
const char *GameMatch3::desc() const { return "滑动交换相邻糖果，三个同色就能消"; }
const char *GameMatch3::tag() const { return "MATCH"; }
Color GameMatch3::theme() const { return rgba(255, 146, 40); }

const char *GameMatch3::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d 关", level_);
  return buf;
}

const char *GameMatch3::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", movesLeft_);
  return buf;
}

const char *GameMatch3::hint() const {
  if (state_ == GSTATE_OVER) return won_ ? "过关 - 点击进入下一关" : "步数用完 - 点击重试本关";
  if (state_ == GSTATE_READY) return "滑动交换相邻糖果 · 3 个同色即消";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  if (shufFxMs_ > 0) return "没有能消的组合 - 已重新排列";
  return "滑动或点两下交换 · 滑动键 重排棋盘";
}

/* ==================== 生命周期 ==================== */

void GameMatch3::reset() {
  level_ = 1;
  totalScore_ = 0;
  powerMade_ = 0;
  memset(marked_, 0, sizeof(marked_));
  memset(power_, 0, sizeof(power_));
  memset(trig_, 0, sizeof(trig_));
  memset(spawn_, 0, sizeof(spawn_));
  resetLevelState();
  phase_ = PH_PLAY;
  animMs_ = animTotal_ = 0;
  swapA_ = swapB_ = -1;
  popMs_ = shufFxMs_ = 0;
  overFlag_ = false;
  won_ = false;
  selR_ = selC_ = dragR_ = dragC_ = -1;
  newBoard();
}

void GameMatch3::resetLevelState() {
  movesLeft_ = MOVES_PER_LEVEL;
  levelScore_ = 0;
  combo_ = 0;
  target_ = TARGET_BASE + (level_ - 1) * TARGET_STEP;
}

void GameMatch3::nextLevel() {
  ++level_;
  resetLevelState();
  newBoard();
  phase_ = PH_PLAY;
  selR_ = selC_ = -1;
}

void GameMatch3::retryLevel() {
  resetLevelState();
  newBoard();
  phase_ = PH_PLAY;
  selR_ = selC_ = -1;
}

/* ==================== 棋盘生成 / 匹配 / 死局 ==================== */

/*
 * 开局棋盘：**绝不能自带三连**。
 * 做法是"逐格填 + 避开同色"（填之前看左边两格和上边两格），而不是"随机填完再重来"：
 * 后者在 8x8 上命中率很低（每次随机都带三连的概率 > 80%），会白白烧掉上百次重试。
 */
void GameMatch3::newBoard() {
  memset(power_, 0, sizeof(power_));      // 新棋盘不留任何特殊块（炸弹/彩虹球只在消除中生成）
  for (int attempt = 0; attempt < 24; ++attempt) {
    for (int r = 0; r < ROWS; ++r) {
      for (int c = 0; c < COLS; ++c) {
        int t = 0;
        for (int guard = 0; guard < 60; ++guard) {
          t = rand() % TYPES;
          bool bad = (c >= 2 && cell_[r][c - 1] == t && cell_[r][c - 2] == t) ||
                     (r >= 2 && cell_[r - 1][c] == t && cell_[r - 2][c] == t);
          if (!bad) break;
        }
        cell_[r][c] = (signed char)t;
      }
    }
    memset(fallFrom_, 0, sizeof(fallFrom_));
    if (!hasAnyMatch() && hasAnyMove()) return;   // 既没有白送的三连，又确实有的玩
  }
  /* 兜底：24 次都不满足说明随机数出了问题（正常概率极低）——
   * 摆一个"交错无三连"的确定性棋盘，保证游戏一定能开始，不把玩家卡在空棋盘上。 */
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) cell_[r][c] = (signed char)((r + c) % TYPES);
  }
  logInfo("Match3: 随机棋盘 24 次都不满足（无三连且有解），已回退到交错棋盘");
}

/* 扫描所有 3+ 连线（run），标记到 out；`kind` 非空时顺带记下 **>=4 的 run 要生成什么特殊块**。
 *
 * 与"只看有没有匹配"那版的两点区别：
 *   ① **彩虹球不参与颜色匹配** —— 它把 run 断开（当障碍物），否则"无色块"会跟任何颜色连成片；
 *   ② 长度 ≥4 的 run 记一个生成点：4 连 → 方向炸弹（该 run 的方向）、5 连及以上 → 彩虹球。
 * 生成位置取 run 的**中间格**：可预期（不随"玩家从哪头交换"而变），也便于 QA 复现。
 */
bool GameMatch3::collectRuns(bool out[ROWS][COLS], unsigned char kind[ROWS][COLS]) const {
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) out[r][c] = false;
  }
  bool any = false;

  // ---- 横向 ----
  for (int r = 0; r < ROWS; ++r) {
    int c = 0;
    while (c < COLS) {
      const int v = cell_[r][c];
      if (v < 0 || power_[r][c] == PW_RAINBOW) { ++c; continue; }   // 空格/彩虹球 = 隔断
      int e = c + 1;
      while (e < COLS && cell_[r][e] == v && power_[r][e] != PW_RAINBOW) ++e;
      const int len = e - c;
      if (len >= 3) {
        for (int k = c; k < e; ++k) out[r][k] = true;
        any = true;
        if (kind && len >= 4) {
          const int pos = c + len / 2;
          if (kind[r][pos] == PW_NONE) kind[r][pos] = (len >= 5) ? PW_RAINBOW : PW_ROW;
        }
      }
      c = e;
    }
  }
  // ---- 纵向 ----
  for (int c = 0; c < COLS; ++c) {
    int r = 0;
    while (r < ROWS) {
      const int v = cell_[r][c];
      if (v < 0 || power_[r][c] == PW_RAINBOW) { ++r; continue; }
      int e = r + 1;
      while (e < ROWS && cell_[e][c] == v && power_[e][c] != PW_RAINBOW) ++e;
      const int len = e - r;
      if (len >= 3) {
        for (int k = r; k < e; ++k) out[k][c] = true;
        any = true;
        if (kind && len >= 4) {
          const int pos = r + len / 2;
          if (kind[pos][c] == PW_NONE) kind[pos][c] = (len >= 5) ? PW_RAINBOW : PW_COL;
        }
      }
      r = e;
    }
  }
  return any;
}

bool GameMatch3::findMatches(bool out[ROWS][COLS]) const {
  return collectRuns(out, 0);   // 只问"有没有匹配"的调用方（hasAnyMatch/hasAnyMove）不需要生成信息
}

/*
 * ★★ 特殊块的效果展开（三消里最容易写错的一块）：
 *   把"被消除集合"逐步扩张到"它波及的格子"，而且**允许级联**——
 *   炸弹清掉一行后，那一行里可能还有另一个炸弹/彩虹球，它们也要触发。
 * 所以是一层 while（每轮把所有"还没展开过的、已在集合里的"特殊块展开一遍），
 * 直到不再有新增格子。`trig_` 保证每个特殊块只展开一次（否则两个炸弹互指会死循环）。
 */
void GameMatch3::expandSpecials() {
  memset(trig_, 0, sizeof(trig_));
  for (int pass = 0; pass < 16; ++pass) {          // 上限只是防御（正常 2~3 轮就收敛）
    bool changed = false;
    for (int r = 0; r < ROWS; ++r) {
      for (int c = 0; c < COLS; ++c) {
        if (!marked_[r][c] || trig_[r][c]) continue;
        const int pw = power_[r][c];
        if (pw == PW_NONE) continue;
        trig_[r][c] = 1;
        if (pw == PW_ROW) {
          int n = 0;
          for (int k = 0; k < COLS; ++k) {
            if (!marked_[r][k] && cell_[r][k] >= 0) { marked_[r][k] = true; ++n; changed = true; }
          }
          logInfo("Match3: 横向炸弹 (%d,%d) 引爆 → 第 %d 行 +%d 格", r, c, r, n);
        } else if (pw == PW_COL) {
          int n = 0;
          for (int k = 0; k < ROWS; ++k) {
            if (!marked_[k][c] && cell_[k][c] >= 0) { marked_[k][c] = true; ++n; changed = true; }
          }
          logInfo("Match3: 纵向炸弹 (%d,%d) 引爆 → 第 %d 列 +%d 格", r, c, c, n);
        } else if (pw == PW_RAINBOW) {
          // 被别人的爆炸"波及"的彩虹球：清掉场上**最多的那种颜色**（而不是随机 —— 随机会让
          // 同一局面每次结果不同，QA 就没法复现了）
          int cnt[TYPES] = {0};
          for (int a = 0; a < ROWS; ++a) {
            for (int b = 0; b < COLS; ++b) {
              if (cell_[a][b] >= 0 && power_[a][b] != PW_RAINBOW) ++cnt[cell_[a][b]];
            }
          }
          int best = -1, bestN = 0;
          for (int t = 0; t < TYPES; ++t) {
            if (cnt[t] > bestN) { bestN = cnt[t]; best = t; }
          }
          int n = 0;
          if (best >= 0) {
            for (int a = 0; a < ROWS; ++a) {
              for (int b = 0; b < COLS; ++b) {
                if (cell_[a][b] == best && power_[a][b] != PW_RAINBOW && !marked_[a][b]) {
                  marked_[a][b] = true;
                  ++n;
                  changed = true;
                }
              }
            }
          }
          logInfo("Match3: 彩虹球 (%d,%d) 被波及 → 清色 %d（场上共 %d 颗，+%d）", r, c, best,
                  bestN, n);
        }
      }
    }
    if (!changed) break;
  }
}

/* 彩虹球被**玩家主动交换**时：清掉"交换过去那一格"的全部同色（这是它的主要用法）。 */
void GameMatch3::buildRainbowClear(int rr, int rc, bool out[ROWS][COLS]) const {
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) out[r][c] = false;
  }
  const int other = (idx(rr, rc) == swapA_) ? swapB_ : swapA_;
  const int col = cell_[other / COLS][other % COLS];
  int n = 0;
  if (col >= 0) {
    for (int r = 0; r < ROWS; ++r) {
      for (int c = 0; c < COLS; ++c) {
        if (cell_[r][c] == col && power_[r][c] != PW_RAINBOW) { out[r][c] = true; ++n; }
      }
    }
  }
  out[rr][rc] = true;                       // 彩虹球自己也消耗掉
  logInfo("Match3: 彩虹球 (%d,%d) 交换 → 清掉颜色 %d 的 %d 颗", rr, rc, col, n);
}

bool GameMatch3::hasAnyMatch() const {
  bool m[ROWS][COLS];
  return findMatches(m);
}

/*
 * 死局检测：**必须做**，否则棋盘会出现"怎么换都消不掉"的僵局，
 * 玩家只会觉得"这游戏坏了"（参考产品在这里会自动重排并提示）。
 * 64 格 × 2 个方向 × 一次全盘扫描（128 格）≈ 16k 次比较，只在每次落定后跑一次，
 * 放在 A7 上也是微秒级，不需要更聪明的算法。
 */
bool GameMatch3::hasAnyMove() {
  /* ⚠️ 两个容易写漏的点：
   *   ① 试探交换时要**连 power_ 一起换** —— 特殊块是"跟着糖果走的物件"，只换 cell_
   *      会让"彩虹球在哪"与真实交换后不一致（它会把 run 断开），死局判定就对不上现实；
   *   ② **特殊块参与的交换本身就是有效操作**（彩虹球与任意糖、两个炸弹互换都能炸），
   *      所以"只剩特殊块可玩"时不能判成死局 —— 那会让棋盘被无谓地重排掉。 */
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      const int nbr[2][2] = {{r, c + 1}, {r + 1, c}};
      for (int k = 0; k < 2; ++k) {
        const int r2 = nbr[k][0], c2 = nbr[k][1];
        if (r2 >= ROWS || c2 >= COLS) continue;
        if (power_[r][c] == PW_RAINBOW || power_[r2][c2] == PW_RAINBOW) return true;
        if (power_[r][c] != PW_NONE && power_[r2][c2] != PW_NONE) return true;

        signed char t = cell_[r][c];
        cell_[r][c] = cell_[r2][c2];
        cell_[r2][c2] = t;
        unsigned char p = power_[r][c];
        power_[r][c] = power_[r2][c2];
        power_[r2][c2] = p;
        const bool ok = hasAnyMatch();
        t = cell_[r][c];
        cell_[r][c] = cell_[r2][c2];
        cell_[r2][c2] = t;
        p = power_[r][c];
        power_[r][c] = power_[r2][c2];
        power_[r2][c2] = p;
        if (ok) return true;
      }
    }
  }
  return false;
}

void GameMatch3::shuffleBoard() {
  for (int attempt = 0; attempt < 80; ++attempt) {
    // 收集现有糖果 → Fisher-Yates 打乱 → 写回（保留"玩家场上的这些糖"，只挪位置）
    signed char pool[ROWS * COLS];
    int n = 0;
    for (int r = 0; r < ROWS; ++r) {
      for (int c = 0; c < COLS; ++c) {
        if (cell_[r][c] >= 0) pool[n++] = cell_[r][c];
      }
    }
    for (int i = n - 1; i > 0; --i) {
      int j = rand() % (i + 1);
      signed char t = pool[i];
      pool[i] = pool[j];
      pool[j] = t;
    }
    int k = 0;
    for (int r = 0; r < ROWS; ++r) {
      for (int c = 0; c < COLS; ++c) cell_[r][c] = pool[k++];
    }
    if (!hasAnyMatch() && hasAnyMove()) {
      shufFxMs_ = SHUF_MS;
      logInfo("Match3: 死局 → 第 %d 次重排成功", attempt + 1);
      return;
    }
  }
  newBoard();          // 兜底（概率极低）
  shufFxMs_ = SHUF_MS;
  logInfo("Match3: 死局重排 80 次未果，已重建棋盘");
}

/* ==================== 交换 / 消除 / 下落 ==================== */

/* ★ 唯一的交换入口：触摸与 QA（`gdbg swap`）都走它 ——
 * 验收必须和真实操作同源，否则验到的东西和玩家玩到的不是一回事
 * （这条纪律见 docs/touch-inject.md §7）。 */
void GameMatch3::trySwap(int r1, int c1, int r2, int c2) {
  if (phase_ != PH_PLAY || !isPlaying()) return;
  if (!inGrid(r1, c1) || !inGrid(r2, c2)) return;
  int d = iabs(r1 - r2) + iabs(c1 - c2);
  if (d != 1) return;                       // 只允许相邻（对角不行）
  if (cell_[r1][c1] < 0 || cell_[r2][c2] < 0) return;

  signed char t = cell_[r1][c1];
  cell_[r1][c1] = cell_[r2][c2];
  cell_[r2][c2] = t;
  unsigned char p = power_[r1][c1];       // 特殊块要跟着糖果一起走（它是挂在糖上的）
  power_[r1][c1] = power_[r2][c2];
  power_[r2][c2] = p;
  swapA_ = idx(r1, c1);
  swapB_ = idx(r2, c2);
  combo_ = 0;
  phase_ = PH_SWAP;
  animMs_ = animTotal_ = SWAP_MS;
  sfx(SFX_MOVE);
}

void GameMatch3::startClear(const bool mark[ROWS][COLS]) {
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) marked_[r][c] = mark[r][c];
  }

  /* ① 展开特殊块效果（会往 marked_ 里加格子，并且**级联**）。
   *    必须在统计颗数之前 —— 否则分数与实际消掉的格数对不上。 */
  expandSpecials();

  /* ② 生成点**不参与消除**：它要留在原格变成炸弹/彩虹球。
   *    颜色先存下来（下一步把 cell_ 置空后就取不到了）。 */
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      if (spawn_[r][c] != PW_NONE) {
        spawnCol_[r][c] = cell_[r][c];
        marked_[r][c] = false;
      }
    }
  }

  int n = 0;
  int longBonus = 0;
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      if (marked_[r][c]) ++n;
    }
  }
  /* 长连加成：单独扫一遍"≥4 的连续段"给奖励（交叉处会重复计一次 ——
   * 分数宁松不严，玩家感受到的是"连得越长越爽"）。 */
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      if (!marked_[r][c]) continue;
      if (c == 0 || !marked_[r][c - 1]) {           // 段首
        int len = 0;
        while (c + len < COLS && marked_[r][c + len]) ++len;
        if (len >= 4) longBonus += 20 * (len - 3);
      }
    }
  }
  for (int c = 0; c < COLS; ++c) {
    for (int r = 0; r < ROWS; ++r) {
      if (!marked_[r][c]) continue;
      if (r == 0 || !marked_[r - 1][c]) {
        int len = 0;
        while (r + len < ROWS && marked_[r + len][c]) ++len;
        if (len >= 4) longBonus += 20 * (len - 3);
      }
    }
  }

  ++combo_;                                     // 连锁计数（第 1 次 = 1）
  if (combo_ > maxCombo_) maxCombo_ = combo_;
  const int gain = n * 10 * combo_ + longBonus;
  clearCells_ = n;
  addScore(gain, n, combo_);

  // 弹出位置 = 被消格子的重心（"分数就从我消掉的地方冒出来"）
  int sx = 0, sy = 0;
  for (int r = 0; r < ROWS; ++r) {
    for (int c = 0; c < COLS; ++c) {
      if (marked_[r][c]) { sx += cellX(c) + CELL / 2; sy += cellY(r) + CELL / 2; }
    }
  }
  if (n > 0) popScore(sx / n, sy / n, gain);

  phase_ = PH_CLEAR;
  animMs_ = animTotal_ = CLEAR_MS;
  sfx(combo_ >= 2 ? SFX_MERGE : SFX_CLEAR);
}

void GameMatch3::startFall() {
  int maxDrop = 0;
  for (int c = 0; c < COLS; ++c) {
    signed char keepV[ROWS];
    unsigned char keepP[ROWS];                     // ★ 特殊块要跟着一起落下
    int keepR[ROWS];
    int n = 0;
    for (int r = ROWS - 1; r >= 0; --r) {          // 从底往上收集（还活着的糖）
      if (cell_[r][c] >= 0) {
        keepV[n] = cell_[r][c];
        keepP[n] = power_[r][c];
        keepR[n] = r;
        ++n;
      }
    }
    const int missing = ROWS - n;
    for (int i = 0; i < missing; ++i) {            // 顶部补新糖
      keepV[n + i] = (signed char)(rand() % TYPES);
      keepP[n + i] = PW_NONE;                      // 新掉进来的都是普通糖
      keepR[n + i] = -(i + 1);                     // 负数 = 从棋盘上方"掉进来"
    }
    for (int idx2 = 0; idx2 < ROWS; ++idx2) {
      const int r = ROWS - 1 - idx2;               // keep[idx2] 落到这一行
      cell_[r][c] = keepV[idx2];
      power_[r][c] = keepP[idx2];
      const int drop = r - keepR[idx2];
      fallFrom_[r][c] = drop > 0 ? drop : 0;
      if (fallFrom_[r][c] > maxDrop) maxDrop = fallFrom_[r][c];
    }
  }
  // 下落时长按最大落差给（每格 ~55ms，夹在 170..420ms）
  int ms = maxDrop * 55;
  if (ms < 170) ms = 170;
  if (ms > 420) ms = 420;
  phase_ = PH_FALL;
  animMs_ = animTotal_ = ms;
}

void GameMatch3::afterAnim() {
  switch (phase_) {
    case PH_SWAP: {
      memset(spawn_, 0, sizeof(spawn_));       // 本轮要生成的特殊块（由 collectRuns 填）
      const int ar = swapA_ / COLS, ac = swapA_ % COLS;
      const int br = swapB_ / COLS, bc = swapB_ % COLS;
      bool m[ROWS][COLS];

      // ① 彩虹球参与的交换**永远有效**：清掉"换过去那一格"的全部同色
      int rr = -1, rc = -1;
      if (power_[ar][ac] == PW_RAINBOW) { rr = ar; rc = ac; }
      else if (power_[br][bc] == PW_RAINBOW) { rr = br; rc = bc; }
      if (rr >= 0) {
        --movesLeft_;
        if (movesLeft_ < 0) movesLeft_ = 0;
        /* ★ 进清除流程前先把这一格的"彩虹球"标记摘掉。
         * 不摘会怎样（2026-09-15 实测踩到）：彩虹球自己也在被消除集合里 ⇒
         * `expandSpecials()` 又按"**被波及**的彩虹球"再触发一次（清掉场上最多的颜色）
         * ⇒ 一次交换清了 **7 颗（对方颜色）+ 14 颗（最多色）**，比参考产品强一倍。
         * 主动交换时它的效果**已经**由 buildRainbowClear 用掉了，不能再算一次。 */
        power_[rr][rc] = PW_NONE;
        buildRainbowClear(rr, rc, m);
        combo_ = 0;
        startClear(m);
        break;
      }
      // ② 正常：交换后形成 3 连及以上
      if (collectRuns(m, spawn_)) {
        --movesLeft_;                  // ★ 只有**有效**交换才扣步数
        if (movesLeft_ < 0) movesLeft_ = 0;
        startClear(m);
        break;
      }
      // ③ 两个特殊块互换 = 玩家刻意组合（参考产品的"连炸"），也算有效，直接引爆
      if (power_[ar][ac] != PW_NONE && power_[br][bc] != PW_NONE) {
        --movesLeft_;
        if (movesLeft_ < 0) movesLeft_ = 0;
        bool m2[ROWS][COLS];
        memset(m2, 0, sizeof(m2));
        m2[ar][ac] = true;
        m2[br][bc] = true;
        combo_ = 0;
        logInfo("Match3: 两个特殊块互换 (%d,%d)<->(%d,%d) → 引爆", ar, ac, br, bc);
        startClear(m2);
        break;
      }
      // ④ 无效交换：滑回去（**不是瞬移** —— 瞬移会让玩家以为"点了没反应"）
      {
        const int a = swapA_, b = swapB_;
        signed char t = cell_[a / COLS][a % COLS];
        cell_[a / COLS][a % COLS] = cell_[b / COLS][b % COLS];
        cell_[b / COLS][b % COLS] = t;
        unsigned char p = power_[a / COLS][a % COLS];
        power_[a / COLS][a % COLS] = power_[b / COLS][b % COLS];
        power_[b / COLS][b % COLS] = p;
        phase_ = PH_REVERT;
        animMs_ = animTotal_ = SWAP_MS;
        sfx(SFX_HIT);
      }
      break;
    }
    case PH_REVERT:
      swapA_ = swapB_ = -1;
      phase_ = PH_PLAY;
      break;
    case PH_CLEAR: {
      for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
          if (marked_[r][c]) {
            cell_[r][c] = -1;
            power_[r][c] = PW_NONE;
            marked_[r][c] = false;
          }
        }
      }
      /* 清除完立刻把特殊块**放回生成点**（保留原糖果色）：
       * 这样紧接着的下落会把它当普通物块一起压下去 —— 与参考产品一致（炸弹会往下掉）。 */
      for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
          if (spawn_[r][c] == PW_NONE) continue;
          cell_[r][c] = (spawnCol_[r][c] >= 0) ? spawnCol_[r][c]
                                               : (signed char)(rand() % TYPES);
          power_[r][c] = spawn_[r][c];
          ++powerMade_;
          logInfo("Match3: 生成 %s 于 (%d,%d)（本局第 %d 个）",
                  spawn_[r][c] == PW_RAINBOW ? "彩虹球"
                                             : (spawn_[r][c] == PW_ROW ? "横向炸弹" : "纵向炸弹"),
                  r, c, powerMade_);
          spawn_[r][c] = PW_NONE;
          spawnCol_[r][c] = -1;
        }
      }
      startFall();
      break;
    }
    case PH_FALL: {
      bool m[ROWS][COLS];
      memset(spawn_, 0, sizeof(spawn_));       // 连锁同样能生成特殊块（参考产品也是如此）
      if (collectRuns(m, spawn_)) {
        startClear(m);                   // 连锁：落定后又凑成新的三连
      } else {
        memset(fallFrom_, 0, sizeof(fallFrom_));
        phase_ = PH_PLAY;
        combo_ = 0;
        settleAfterFall();
      }
      break;
    }
    default:
      phase_ = PH_PLAY;
      break;
  }
}

void GameMatch3::settleAfterFall() {
  if (levelScore_ >= target_) {                    // 达标即过关（不必用完步数）
    won_ = true;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    sfx(SFX_OVER);
    logInfo("Match3: 过关 level=%d 本关分=%d 总分=%d 步数余=%d", level_, levelScore_,
            totalScore_, movesLeft_);
    return;
  }
  if (movesLeft_ <= 0) {
    won_ = false;
    state_ = GSTATE_OVER;
    overFlag_ = true;
    saveBestIfNeeded(totalScore_);
    sfx(SFX_OVER);
    logInfo("Match3: 步数用尽 level=%d 本关分=%d 目标=%d 总分=%d", level_, levelScore_,
            target_, totalScore_);
    return;
  }
  if (!hasAnyMove()) {
    shuffleBoard();
    logInfo("Match3: 检测到死局，自动重排");
  }
}

void GameMatch3::addScore(int gain, int cells, int combo) {
  levelScore_ += gain;
  totalScore_ += gain;
  saveBestIfNeeded(totalScore_);
  logInfo("Match3: +%d（%d 颗, 连锁 x%d）本关=%d/%d 总分=%d", gain, cells, combo,
          levelScore_, target_, totalScore_);
}

void GameMatch3::popScore(int px, int py, int value) {
  popMs_ = POP_MS;
  popVal_ = value;
  popX_ = px;
  popY_ = py;
}

/* ==================== 主循环 ==================== */

void GameMatch3::update(int dtMs) {
  if (popMs_ > 0) popMs_ -= dtMs;
  if (shufFxMs_ > 0) shufFxMs_ -= dtMs;

  if (phase_ != PH_PLAY) {
    animMs_ -= dtMs;
    if (animMs_ <= 0) afterAnim();
    return;
  }
  // PH_PLAY：等玩家操作，没有自主推进的计时器
}

bool GameMatch3::stillFrame() const {
  /* 覆盖层（READY / OVER / PAUSED）本身不动，但**交换/消除/下落/洗牌/分数弹出**
   * 期间画面在动 ⇒ 那几帧必须照常重绘，否则动画会冻在半路。 */
  if (phase_ != PH_PLAY || popMs_ > 0 || shufFxMs_ > 0) return false;
  return state_ != GSTATE_RUNNING;
}

/* ==================== 绘制 ==================== */

void GameMatch3::cellDrawPos(int r, int c, int &x, int &y) const {
  x = cellX(c);
  y = cellY(r);
  const int id = idx(r, c);

  // ① 交换 / 回退：两格沿直线**插值滑动**（t: 1 → 0）
  if ((phase_ == PH_SWAP || phase_ == PH_REVERT) && animTotal_ > 0 &&
      (id == swapA_ || id == swapB_)) {
    const int other = (id == swapA_) ? swapB_ : swapA_;
    const int ox = cellX(other % COLS), oy = cellY(other / COLS);
    const int t = animMs_ * 256 / animTotal_;
    x = x + (ox - x) * t / 256;
    y = y + (oy - y) * t / 256;
  }
  // ② 下落：从上方掉到位（偏移 = 落差像素 × 剩余比例）
  if (phase_ == PH_FALL && animTotal_ > 0 && fallFrom_[r][c] > 0) {
    y -= fallFrom_[r][c] * CELL * animMs_ / animTotal_;
  }
}

void GameMatch3::renderBoard(Canvas &c) {
  /* ⚠️ 循环变量不能叫 `c` —— 会把画布参数 `Canvas &c` 遮蔽掉，
   *    于是 `blitA(c, ...)` 传进去的是个 int（-Werror=format 之外还容易看漏，实测踩过）。 */
  for (int r = 0; r < ROWS; ++r) {
    for (int col = 0; col < COLS; ++col) {
      const int t = cell_[r][col];
      if (t < 0) continue;
      const gameart::Def *d = candyDef(t);
      if (!d) continue;
      int x, y;
      cellDrawPos(r, col, x, y);

      int alpha = 255;
      if (phase_ == PH_CLEAR && marked_[r][col] && animTotal_ > 0) {
        alpha = animMs_ * 255 / animTotal_;        // 消除：糖果淡出（1 → 0）
      }
      const int pw = power_[r][col];
      if (pw == PW_RAINBOW) {
        blitA(c, gameart::kMatch3Rainbow, x, y, alpha);   // 彩虹球有自己的图（无色）
      } else {
        blitA(c, *d, x, y, alpha);
        if (pw != PW_NONE && alpha == 255) renderPowerMark(c, x, y, pw);
      }

      // 消除特效：爆花贴在被消格子上（先快速显现、再淡出），锚点 = 中心
      if (phase_ == PH_CLEAR && marked_[r][col] && animTotal_ > 0) {
        const int p = animTotal_ - animMs_;         // 已播放时长
        int ba = (p < 90) ? (p * 255 / 90) : (255 - (p - 90) * 255 / (animTotal_ - 90));
        if (ba > 0) {
          blitA(c, gameart::kMatch3Burst, cellX(col) + CELL / 2, cellY(r) + CELL / 2, ba);
        }
      }
    }
  }
  // 选中框画在糖果之上（否则被糖果盖住看不见）
  if (selR_ >= 0 && selC_ >= 0 && phase_ == PH_PLAY) {
    blit(c, gameart::kMatch3Sel, cellX(selC_), cellY(selR_));
  }
}

/* 炸弹上的方向标记：贴一张**中性色的小图**（mark_h / mark_v），叠在糖果之上。
 *
 * ★ 曾经是"代码画图元"（圆角条 + 4 个三角），实测带特殊块时帧率掉到 52fps（render 5.1ms）：
 *   每格 6 次图元调用是**逐像素填充**，而贴图版大部分像素 alpha=0 会被直接跳过
 *   （`Canvas::drawSpriteA` 的慢路径有 `if (!a) continue;`）⇒ 几乎免费。
 *   改回贴图后帧率回到 57+。
 * 为什么只烘 2 张（而不是 7 色 × 2）：标记是中性色的，与糖果图叠加即可 —— 见 gen_game_art.py。 */
void GameMatch3::renderPowerMark(Canvas &c, int x, int y, int kind) {
  blit(c, kind == PW_ROW ? gameart::kMatch3MarkH : gameart::kMatch3MarkV, x, y);
}

void GameMatch3::renderTopBar(Canvas &c) {
  const int W = c.width();
  c.fillRectRound(8, 4, W - 16, 36, 13, rgba(88, 56, 30, 220));
  c.fillRectRound(10, 6, W - 20, 32, 11, rgba(140, 96, 56));

  char b[24];
  snprintf(b, sizeof(b), "第 %d 关", level_);
  c.textCenterBox(14, 6, 104, 32, b, 1, rgba(255, 240, 200));

  // 本关目标进度条（填充比例 = 本关分 / 目标分）
  const int px = 124, pw = 196;
  c.fillRectRound(px, 14, pw, 16, 8, rgba(70, 44, 24));
  int fill = 0;
  if (target_ > 0) {
    fill = levelScore_ * pw / target_;
    if (fill > pw) fill = pw;
  }
  if (fill > 0) {
    const bool done = (levelScore_ >= target_);
    c.fillRectRound(px, 14, fill, 16, 8, done ? rgba(120, 226, 120) : rgba(255, 205, 70));
  }
  c.strokeRect(px, 14, pw, 16, 1, rgba(255, 240, 200, 90));

  snprintf(b, sizeof(b), "步 %d", movesLeft_);
  c.textCenterBox(326, 6, 140, 32, b, 1, movesLeft_ <= 5 ? rgba(255, 150, 130)
                                                        : rgba(255, 240, 200));
}

void GameMatch3::renderScorePop(Canvas &c) {
  if (popMs_ <= 0) return;
  const int elapsed = POP_MS - popMs_;
  const int rise = elapsed * 34 / POP_MS;              // 0 → 34px 上飘
  const int out = (popMs_ < 300) ? (300 - popMs_) : 0;  // 末尾 300ms 淡出
  char b[16];
  snprintf(b, sizeof(b), "+%d", popVal_);
  const int y = popY_ - rise;
  Color col = rgba(255, 236, 140);
  Color edge = rgba(120, 62, 12);
  if (out) {
    const Color bg = fadeTargetAt(y);
    col = lerpColor(col, bg, out * 256 / 300);
    edge = lerpColor(edge, bg, out * 256 / 300);
  }
  // 4 方向描边（棋盘底色花，没有描边小字会看不清）
  static const int OX[4] = {-2, 2, 0, 0};
  static const int OY[4] = {0, 0, -2, 2};
  for (int k = 0; k < 4; ++k) c.bigTextCenter(popX_ + OX[k], y + OY[k], b, 1, edge);
  c.bigTextCenter(popX_, y, b, 1, col);
}

void GameMatch3::renderOverlay(Canvas &c) {
  const int W = c.width(), H = c.height();
  if (state_ == GSTATE_OVER) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 168);
    char b[64];
    if (won_) {
      c.textCenter(W / 2, H / 2 - 104, "过关", 5, rgba(255, 214, 64));
      snprintf(b, sizeof(b), "第 %d 关  %d 分", level_, levelScore_);
      c.textCenter(W / 2, H / 2 - 24, b, 2, rgba(255, 255, 255));
      snprintf(b, sizeof(b), "总得分 %d", totalScore_);
      c.textCenter(W / 2, H / 2 + 12, b, 2, rgba(200, 230, 200));
      snprintf(b, sizeof(b), "点击进入第 %d 关", level_ + 1);
      c.textCenter(W / 2, H / 2 + 62, b, 2, rgba(160, 220, 160));
    } else {
      c.textCenter(W / 2, H / 2 - 104, "步数用完", 4, rgba(255, 140, 120));
      snprintf(b, sizeof(b), "本关 %d 分  目标 %d", levelScore_, target_);
      c.textCenter(W / 2, H / 2 - 24, b, 2, rgba(255, 255, 255));
      snprintf(b, sizeof(b), "总得分 %d", totalScore_);
      c.textCenter(W / 2, H / 2 + 12, b, 2, rgba(200, 210, 220));
      c.textCenter(W / 2, H / 2 + 62, "点击重试本关", 2, rgba(255, 200, 140));
    }
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 148);
    c.textCenter(W / 2, H / 2 - 132, "消消乐", 5, rgba(255, 168, 60));
    c.textCenter(W / 2, H / 2 - 52, "滑动交换相邻糖果", 2, rgba(240, 240, 240));
    c.textCenter(W / 2, H / 2 - 16, "3 个同色连成一线就消除", 2, rgba(210, 220, 210));
    /* ★ 原文案「4 连出方向炸弹 · 5 连出彩虹球」档 2 时正好 480px = 满宽贴边，
     *   缩短成 8 中文 + 2 数字 = 320px（两侧各留 80px）。改前先跑 check_textwidth.py。 */
    c.textCenter(W / 2, H / 2 + 24, "4 连炸弹 · 5 连彩虹球", 2, rgba(255, 214, 64));
    c.textCenter(W / 2, H / 2 + 62, "25 步内达到目标分即过关", 2, rgba(180, 200, 180));
    c.textCenter(W / 2, H / 2 + 116, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

void GameMatch3::render(Canvas &c) {
  // ① 静态底图：花园 + 木框 + 8x8 凹槽（一张贴图顶掉几百行几何绘制）
  blit(c, gameart::kMatch3Bg, 0, 0);
  // ② 糖果（含交换滑动 / 下落位移 / 消除淡出 / 爆花）
  renderBoard(c);
  // ③ 分数弹出（在糖果之上，但在顶栏之下）
  renderScorePop(c);
  // ④ 顶栏（关卡 / 目标进度 / 步数）
  renderTopBar(c);
  // ⑤ 洗牌提示
  if (shufFxMs_ > 0) {
    c.fillRectRound(48, c.height() - 96, c.width() - 96, 40, 12, rgba(24, 40, 28, 210));
    c.textCenterBox(48, c.height() - 96, c.width() - 96, 40, "重新排列棋盘", 1,
                    rgba(255, 230, 160));
  }
  // ⑥ 覆盖层（盖住一切）
  renderOverlay(c);
}

/* ==================== 输入 ==================== */

bool GameMatch3::onTouch(int action, int x, int y) {
  if (state_ == GSTATE_OVER) {
    if (action == PG_TOUCH_DOWN) {
      if (won_) nextLevel(); else retryLevel();
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
  if (phase_ != PH_PLAY) return true;          // 动画中不接受输入（避免状态错乱）

  const int gx = (x - BX) / CELL;
  const int gy = (y - BY) / CELL;
  const bool inside = (x >= BX && y >= BY && gx >= 0 && gx < COLS && gy >= 0 && gy < ROWS);

  if (action == PG_TOUCH_DOWN) {
    if (!inside) {
      selR_ = selC_ = -1;
      return true;
    }
    // 已选中且新点在**相邻格** ⇒ 直接交换（"点两下"的第二次点击）
    if (selR_ >= 0 && selC_ >= 0 && iabs(selR_ - gy) + iabs(selC_ - gx) == 1) {
      trySwap(selR_, selC_, gy, gx);
      selR_ = selC_ = -1;
      dragR_ = dragC_ = -1;
      return true;
    }
    if (selR_ == gy && selC_ == gx) {          // 再点同一格 = 取消选择
      selR_ = selC_ = -1;
      return true;
    }
    selR_ = gy;
    selC_ = gx;
    dragR_ = gy;
    dragC_ = gx;
    dragX_ = x;
    dragY_ = y;
    sfx(SFX_CLICK);
  } else if (action == PG_TOUCH_MOVE) {
    if (dragR_ < 0) return true;
    const int dx = x - dragX_, dy = y - dragY_;
    if (iabs(dx) > 13 || iabs(dy) > 13) {      // 滑动阈值：太灵敏会误判成"点了就走"
      int tr = dragR_, tc = dragC_;
      if (iabs(dx) >= iabs(dy)) tc += (dx > 0 ? 1 : -1);
      else tr += (dy > 0 ? 1 : -1);
      dragR_ = dragC_ = -1;
      if (inGrid(tr, tc)) {
        trySwap(selR_, selC_, tr, tc);
        selR_ = selC_ = -1;
      }
    }
  } else if (action == PG_TOUCH_UP) {
    dragR_ = dragC_ = -1;                      // 保留 sel：等第二次点击
  }
  return true;
}

bool GameMatch3::onKey(int key) {
  if (state_ == GSTATE_OVER && key == PG_KEY_A) {
    if (won_) nextLevel(); else retryLevel();
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (state_ == GSTATE_READY && key == PG_KEY_A) {
    state_ = GSTATE_RUNNING;
    return true;
  }
  if (key == PG_KEY_C) {                       // 重玩 / 手动重排
    if (state_ == GSTATE_OVER) {
      retryLevel();
      state_ = GSTATE_RUNNING;
    } else {
      shuffleBoard();
    }
    return true;
  }
  return false;
}

bool GameMatch3::justGameOver() { return overFlag_; }
void GameMatch3::clearGameOverFlag() { overFlag_ = false; }

/* ==================== QA 调试通道 ==================== */

void GameMatch3::debugCmd(const char *rest) {
  if (!rest) return;
  if (strncmp(rest, "board", 5) == 0) {
    for (int r = 0; r < ROWS; ++r) {
      char line[COLS * 2 + 1];
      char pl[COLS * 2 + 1];
      int k = 0;
      for (int c = 0; c < COLS; ++c) {
        const int v = cell_[r][c];
        line[k] = (v < 0) ? '-' : (char)('0' + v);
        // 特殊块单独一行打印（**不改 board 行格式** —— 否则 PC 侧解析脚本要跟着改）
        pl[k] = (power_[r][c] == PW_ROW) ? 'H'
                : (power_[r][c] == PW_COL) ? 'V'
                : (power_[r][c] == PW_RAINBOW) ? 'R' : '.';
        ++k;
        line[k] = ' ';
        pl[k] = ' ';
        ++k;
      }
      line[k ? k - 1 : 0] = 0;
      pl[k ? k - 1 : 0] = 0;
      logInfo("Match3: board[%d] %s", r, line);
      logInfo("Match3: power[%d] %s", r, pl);
    }
    return;
  }
  if (strncmp(rest, "state", 5) == 0) {
    logInfo("Match3: level=%d moves=%d levelScore=%d/%d total=%d phase=%d combo=%d "
            "maxCombo=%d state=%d powerMade=%d", level_, movesLeft_, levelScore_, target_,
            totalScore_, phase_, combo_, maxCombo_, (int)state_, powerMade_);
    return;
  }
  /* `power r c k` —— 直接把某格设成特殊块（k = H 横炸弹 / V 竖炸弹 / R 彩虹球 / N 清除）。
   * **验收钩子**：4 连/5 连在真机上要凑很久（还要靠运气），而炸弹引爆、彩虹球交换
   * 这两条分支必须验；它只改"棋盘状态"，**不动分数**（合规，见 Game::debugCmd 的约定）。 */
  if (strncmp(rest, "power ", 6) == 0) {
    int r = 0, c = 0;
    char k = 0;
    if (sscanf(rest + 6, "%d %d %c", &r, &c, &k) != 3) {
      logInfo("Match3: power 参数应为 <r> <c> <H|V|R|N>");
      return;
    }
    if (!inGrid(r, c) || cell_[r][c] < 0) {
      logInfo("Match3: power 目标格无效 (%d,%d)", r, c);
      return;
    }
    const int pw = (k == 'H' || k == 'h') ? PW_ROW
                   : (k == 'V' || k == 'v') ? PW_COL
                   : (k == 'R' || k == 'r') ? PW_RAINBOW : PW_NONE;
    power_[r][c] = (unsigned char)pw;
    logInfo("Match3: (%d,%d) 设为 %s（验收钩子）", r, c,
            pw == PW_ROW ? "横向炸弹" : pw == PW_COL ? "纵向炸弹"
            : pw == PW_RAINBOW ? "彩虹球" : "普通糖果");
    return;
  }
  if (strncmp(rest, "shuffle", 7) == 0) {
    shuffleBoard();
    logInfo("Match3: 手动触发重排");
    return;
  }
  if (strncmp(rest, "goal ", 5) == 0) {
    target_ = atoi(rest + 5);
    if (target_ < 1) target_ = 1;
    logInfo("Match3: 本关目标分改为 %d（验收钩子）", target_);
    return;
  }
  if (strncmp(rest, "moves ", 6) == 0) {
    movesLeft_ = atoi(rest + 6);
    if (movesLeft_ < 0) movesLeft_ = 0;
    logInfo("Match3: 剩余步数改为 %d（验收钩子）", movesLeft_);
    return;
  }
  if (strncmp(rest, "level ", 6) == 0) {
    const int lv = atoi(rest + 6);
    if (lv < 1) return;
    level_ = lv;
    resetLevelState();
    newBoard();
    phase_ = PH_PLAY;
    /* ★ 必须把状态也拉回 RUNNING：否则"上一局刚结束（OVER）时跳关"会让棋盘是新的、
     * 但整局仍卡在 OVER —— 后续 swap 会被 trySwap 的 isPlaying() 静默拦截
     * （2026-09-15 实测：跳关后 QA swap 完全没效果，moves/分数都不动）。 */
    state_ = GSTATE_RUNNING;
    overFlag_ = false;
    won_ = false;
    selR_ = selC_ = -1;
    logInfo("Match3: 跳到第 %d 关（目标 %d，状态已置 RUNNING）", level_, target_);
    return;
  }
  if (strncmp(rest, "swap ", 5) == 0) {
    int a = 0, b = 0, c1 = 0, d = 0;
    if (sscanf(rest + 5, "%d %d %d %d", &a, &b, &c1, &d) != 4) {
      logInfo("Match3: swap 参数应为 r1 c1 r2 c2");
      return;
    }
    logInfo("Match3: QA swap (%d,%d)<->(%d,%d) phase=%d", a, b, c1, d, phase_);
    trySwap(a, b, c1, d);
    return;
  }
  logInfo("Match3: 未知调试命令 '%s'（可用 board/state/swap/shuffle/goal/level）", rest);
}

}  // namespace pg
