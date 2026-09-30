/*
 * PgDice.cpp - 摇骰子（3 颗骰子 · 3D 翻滚 · 触摸优先）
 *
 * 用户 2026-09-15 需求：「摇骰子的游戏，基于 FlyThings UI 框架，3 个骰子，要模拟
 * 3D 效果和声音」，参考产品 = App Store《简易骰子 - 朋友聚会摇色子模拟器》。
 *
 * ★★ 3D 是**离线烘出来**的（tools/gen_game_art.py 的 dice 段）：正交投影 + 背面剔除
 *    + 面法线光照，渲出 6 个静止面（值为 v 的面朝上）与 12 帧翻滚（绕斜轴转满一圈）。
 *    为什么不在运行时画立方体：① 画布**不做缩放**、也没有多边形贴图，
 *    "把点数纹理贴到斜面"要靠仿射映射，画布给不了；② 每帧现画 3 个面 ×
 *    若干个图元是**逐像素填充**（打地鼠/消消乐都在这上面掉过帧）。
 *    ⇒ 立体感在 PC 侧算完、真机只做 1:1 贴图，是本工程一贯的做法。
 *
 * 画面里只有**动态**部分留在代码里：翻滚帧的切换 + 三颗的位移抖动、落定的下沉回弹、
 * 接触阴影的浓淡、结果板上的文字（和值/评语每局在变）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgLog.h"
#include "core/PgSpriteDraw.h"   // 贴图助手（blit / blitA）+ 素材清单

namespace pg {

namespace {

/* 翻滚帧数（**必须 == GameDice::ROLL_FRAMES == 素材 roll0..rollN-1 的张数**）。
 * 为什么在这里再写一遍：匿名命名空间里的素材表要在编译期定长，
 * 而那个枚举在类里是 private。改了帧数的话，这三处一起改（生成器 → 枚举 → 这里）。 */
const int kRollN = 12;

/* 素材表（清单见 core/PgGameArt.h，尺寸/锚点由 tools/gen_game_art.py 生成） */
const gameart::Def *kFace[6] = {
    &gameart::kDiceFace1, &gameart::kDiceFace2, &gameart::kDiceFace3,
    &gameart::kDiceFace4, &gameart::kDiceFace5, &gameart::kDiceFace6,
};
const gameart::Def *kRoll[kRollN] = {
    &gameart::kDiceRoll0, &gameart::kDiceRoll1, &gameart::kDiceRoll2,
    &gameart::kDiceRoll3, &gameart::kDiceRoll4, &gameart::kDiceRoll5,
    &gameart::kDiceRoll6, &gameart::kDiceRoll7, &gameart::kDiceRoll8,
    &gameart::kDiceRoll9, &gameart::kDiceRoll10, &gameart::kDiceRoll11,
};

/* 颜色：桌面与结果板都在素材里，这里只留"文字"用的三档 */
Color cText() { return rgba(178, 192, 214); }
Color cSum() { return rgba(255, 226, 128); }
Color cOver() { return rgba(255, 120, 90); }
Color cSeq() { return rgba(140, 220, 255); }
Color cDim() { return rgba(150, 164, 186); }

}  // namespace

GameDice::GameDice()
    : phase_(PH_IDLE), animMs_(0), clatterMs_(0), stopSfx_(0), rolls_(0), sum_(0) {
  for (int i = 0; i < DICE_N; ++i) {
    val_[i] = 1;
    force_[i] = 0;
  }
}

const char *GameDice::title() const { return "摇骰子"; }
const char *GameDice::desc() const { return "3 颗骰子 · 摇一摇比大小"; }
const char *GameDice::tag() const { return "DICE"; }
Color GameDice::theme() const { return rgba(238, 236, 232); }

void GameDice::reset() {
  for (int i = 0; i < DICE_N; ++i) {
    val_[i] = 1 + rand() % 6;   // 开机就先摆三个点数（不结算、不出音）
    force_[i] = 0;
  }
  phase_ = PH_IDLE;
  animMs_ = 0;
  clatterMs_ = 0;
  stopSfx_ = 0;
  rolls_ = 0;
  sum_ = 0;
  state_ = GSTATE_READY;
}

/* ★ 摇动的**唯一**入口：触摸、按键、QA `roll` 全走这里
 *（项目纪律：QA 必须和真实操作同源，否则验出来的是另一回事）。 */
void GameDice::startRoll() {
  for (int i = 0; i < DICE_N; ++i) {
    if (force_[i] >= 1 && force_[i] <= 6) {
      val_[i] = force_[i];      // QA `force` 指定过：用指定值，并立刻消费掉
      force_[i] = 0;
    } else {
      val_[i] = 1 + rand() % 6;
    }
  }
  sum_ = 0;                     // 结果板先清空（停定后才写）
  phase_ = PH_ROLL;
  animMs_ = 0;
  clatterMs_ = 0;
  stopSfx_ = 0;
  ++rolls_;
  sfx(SFX_DICE);
  logInfo("Dice: roll #%d 目标 %d %d %d", rolls_, val_[0], val_[1], val_[2]);
}

void GameDice::update(int dtMs) {
  if (phase_ == PH_IDLE) return;
  animMs_ += dtMs;

  if (phase_ == PH_ROLL) {
    /* 翻滚声：按固定节拍重复播（骰子在杯里"咔啦咔啦"）。
     * 用 SFX_DICE（新音效，见 resources/audio/dice.wav / tools/gensfx.py）。 */
    clatterMs_ += dtMs;
    if (clatterMs_ >= CLATTER_MS) {
      clatterMs_ = 0;
      sfx(SFX_DICE);
    }
    /* 依次停定：从左到右，每跨过一颗的停定时刻补一次"落地"声。
     * 用"已停定颗数"而不是三个 bool 标记 —— 少三个成员，也不会漏播。 */
    int stopped = 0;
    for (int i = 0; i < DICE_N; ++i) {
      if (animMs_ >= stopMs(i)) ++stopped;
    }
    if (stopped > stopSfx_) {
      stopSfx_ = stopped;
      sfx(SFX_DROP);
    }
    if (animMs_ >= stopMs(DICE_N - 1)) {
      phase_ = PH_SETTLE;
      animMs_ = 0;
      sum_ = val_[0] + val_[1] + val_[2];
      saveBestIfNeeded(sum_);
      const char *v = verdict();
      if (v[0] == '豹') sfx(SFX_SCORE);          // 豹子：拔高的"得分"音
      else if (v[0] == '顺') sfx(SFX_MERGE);     // 顺子：上行双音
      logInfo("Dice: settle %d+%d+%d=%d %s", val_[0], val_[1], val_[2], sum_,
              v[0] ? v : "-");
    }
    return;
  }

  if (phase_ == PH_SETTLE && animMs_ >= SETTLE_MS) {
    animMs_ = 0;
    phase_ = PH_IDLE;
  }
}

const char *GameDice::verdict() const {
  if (sum_ <= 0) return "";
  int a = val_[0], b = val_[1], c = val_[2];
  if (a == b && b == c) return "豹子!";
  int mn = a < b ? a : b;
  int mx = a > b ? a : b;
  mn = mn < c ? mn : c;
  mx = mx > c ? mx : c;
  // 顺子 = 三颗互不相同且连续（123/234/345/456）
  if (mx - mn == 2 && a != b && b != c && a != c) return "顺子!";
  if (sum_ >= 11) return "大";
  return "小";
}

/* ==================== 绘制 ==================== */

void GameDice::drawDie(Canvas &c, int i) {
  const int x = cx(i);
  int y = CY;
  const gameart::Def *def = 0;

  if (rolling(i)) {
    /* 翻滚帧：每颗加一个**相位偏移**（i*4），否则三颗动作一模一样，像复制粘贴。
     * 帧号从 animMs_ 直接算 —— 不存"当前第几帧"，少一个可能与时间不同步的状态。 */
    def = kRoll[(animMs_ / ROLL_FRAME_MS + i * 4) % kRollN];
    // 跳动：翻滚时骰子在"跳"，位移每颗错开（不做缩放 —— 画布 1:1 铁律）
    y += (int)(7.0f * sinf((float)(animMs_ + i * 110) * 0.028f));
  } else {
    def = kFace[val_[i] - 1];
    if (phase_ == PH_SETTLE) {
      /* 落定：轻轻下沉再归位（起始多沉 4px ⇒ 有"砸下来"的收势）。
       * 注意只有**位移**，没有缩放 —— 缩放会重采样发糊，工程明令禁止。 */
      y += 4 - 4 * animMs_ / SETTLE_MS;
    }
  }

  /* 接触阴影（**先画**，压在骰子下面）：翻滚中淡一些（骰子离地），停定后实一些。
   * 半透明 ⇒ 走的是逐像素 alpha 混合路径，所以只画这一张、别铺满。 */
  blitA(c, gameart::kDiceShadow, x, CY + 62, rolling(i) ? 120 : 190);
  blit(c, *def, x, y);
}

void GameDice::drawBoard(Canvas &c) {
  const int cx0 = (BOARD_X0 + BOARD_X1) / 2;
  const int w = BOARD_X1 - BOARD_X0;
  const int h = BOARD_Y1 - BOARD_Y0;

  if (sum_ <= 0) {
    // 还没摇过（或正在摇）：板里给一句提示 —— 别留一块空黑板
    const char *t = (phase_ == PH_IDLE) ? "点击屏幕摇骰子" : "摇骰子…";
    c.textCenterBox(BOARD_X0, BOARD_Y0, w, h, t, 2, cDim());
    return;
  }

  char b[48];
  snprintf(b, sizeof(b), "%d   %d   %d", val_[0], val_[1], val_[2]);
  c.textCenter(cx0, BOARD_Y0 + 12, b, 2, cText());
  c.bigNumberCenter(cx0, BOARD_Y0 + 34, sum_, 3, cSum());

  const char *v = verdict();
  if (v[0]) {
    Color col = cSum();
    if (v[0] == '豹') col = cOver();
    else if (v[0] == '顺') col = cSeq();
    c.textCenter(cx0, BOARD_Y0 + 108, v, 2, col);
  }
}

void GameDice::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();

  /* 桌面 + 木框 + 暗角 + 结果板烘在同一张里，**整图不透明** ⇒ 走 memcpy 快路径
   *（三消的教训：一张图里有半透明像素就退回逐像素混合，帧率直接腰斩）。 */
  blit(c, gameart::kDiceBg, 0, 0);

  for (int i = 0; i < DICE_N; ++i) drawDie(c, i);
  drawBoard(c);

  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    c.textCenter(W / 2, H / 2 - 132, "摇骰子", 4, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 - 56, "3 颗骰子 · 点击屏幕摇一摇", 2, rgba(228, 232, 240));
    c.textCenter(W / 2, H / 2 - 18, "豹子 / 顺子有彩头", 2, cSum());
    c.textCenter(W / 2, H / 2 + 44, "点屏幕开始", 2, cDim());
  }
}

/* ==================== 输入 ==================== */

bool GameDice::onTouch(int action, int x, int y) {
  (void)x;
  (void)y;
  if (action != PG_TOUCH_DOWN) return true;   // 抬手/滑动不做事（避免误触发第二局）
  if (state_ == GSTATE_READY) {
    state_ = GSTATE_RUNNING;
    startRoll();
    return true;
  }
  if (phase_ != PH_IDLE) return true;         // 正在摇：忽略，免得连点把动画打乱
  startRoll();
  return true;
}

bool GameDice::onKey(int key) {
  if (key == PG_KEY_A) {                      // 确定 / 开始
    if (state_ == GSTATE_READY) state_ = GSTATE_RUNNING;
    if (phase_ == PH_IDLE) startRoll();
    return true;
  }
  if (key == PG_KEY_C) {                      // 重玩：清掉次数，回到初始态
    reset();
    state_ = GSTATE_RUNNING;
    sfx(SFX_CLICK);
    return true;
  }
  return false;
}

/* 停定后（PH_IDLE）画面一动不动 ⇒ 让宿主跳过重绘；
 * 翻滚/落定期间必须返回 false，否则骰子会冻在半空。 */
bool GameDice::stillFrame() const { return phase_ == PH_IDLE; }

const char *GameDice::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", rolls_);
  return buf;
}

const char *GameDice::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d", best());
  return buf;
}

const char *GameDice::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (phase_ != PH_IDLE) return "摇骰子…";
  const char *v = verdict();
  if (v[0] == '豹') return "豹子！三点同数，彩头最大";
  if (v[0] == '顺') return "顺子！三颗连号";
  if (v[0]) return "再点一次继续摇 · 离开按 B 键";
  return "点击屏幕摇骰子";
}

/* ==================== QA 通道 ====================
 * `gdbg roll` / `gdbg force 5 5 5` / `gdbg state`
 *（宿主 mainLogic 把 `gdbg <...>` 原样转发到 Game::debugCmd）
 *
 * ⚠️ 刻意**不加"直接改分数"**的钩子：`force` 只决定下一轮骰子点数，
 *    从摇动到结算仍然走完整的 startRoll → update → 结算流程，
 *    所以 QA 验到的现象与真机上摇出来的一致（见 docs/touch-inject.md §7）。 */
void GameDice::debugCmd(const char *rest) {
  if (!rest) return;
  if (strncmp(rest, "roll", 4) == 0) {
    if (state_ == GSTATE_READY) state_ = GSTATE_RUNNING;
    if (phase_ != PH_IDLE) {
      logInfo("Dice: QA roll 被忽略（动画中：phase=%d）", phase_);
      return;
    }
    startRoll();
    return;
  }
  if (strncmp(rest, "force ", 6) == 0) {
    int a = 0, b = 0, c = 0;
    if (sscanf(rest + 6, "%d %d %d", &a, &b, &c) == 3 && a >= 1 && a <= 6 &&
        b >= 1 && b <= 6 && c >= 1 && c <= 6) {
      force_[0] = a;
      force_[1] = b;
      force_[2] = c;
      logInfo("Dice: QA force 下一轮 = %d %d %d", a, b, c);
    } else {
      logInfo("Dice: QA force 参数无效（要三个 1..6）");
    }
    return;
  }
  if (strncmp(rest, "state", 5) == 0) {
    logInfo("Dice: state 次数=%d 点数=%d/%d/%d 和值=%d 评语=%s phase=%d best=%d",
            rolls_, val_[0], val_[1], val_[2], sum_,
            verdict()[0] ? verdict() : "-", phase_, best());
    return;
  }
  logInfo("Dice: 未知 QA 命令 '%s'（可用：roll / force a b c / state）", rest);
}

}  // namespace pg
