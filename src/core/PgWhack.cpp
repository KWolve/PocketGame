/*
 * PgWhack.cpp - 打地鼠（触摸专属：靠"快速点到冒头的地鼠"取胜）
 *
 * 为什么加它：原有 8 款全是策略/动作类，**没有一款是纯反应类**；而且它是**触摸屏独占**的玩法 ——
 * 靠快速、随机的点击反应，键盘做不出等价体验（这正是"适合触摸屏"的典型）。
 *
 * 玩法：30 秒内，地鼠从 9 个洞里随机冒头（每次停留 0.6~1.4 秒），点中即得分；
 *       连击每 5 次多给 1 分；点到空处断连。难度随时间递增（冒头节拍从 820ms 压到 260ms）。
 *
 * ★ 2026-09-15 视觉改版（用户要求"结合实际场景 + 锤子切图 + 连击赞赏"）。
 * ★★ 2026-09-15 第二版：**渲染全面素材化**（用户："绘制效率太低，通用图标直接做成 PNG
 *    存起来直接加载；只有动态的才自己绘制"）。
 *
 *   静态元素 → **PNG 素材，1:1 贴图**（清单见 core/PgGameArt.h，生成器 tools/gen_game_art.py）：
 *     草地底图（原来每帧 135 次 fillRect + 110 个草簇 + 泥土斑/花/石头 ≈ 60 行绘图代码）
 *     洞口 / 洞口前沿 / 地鼠（普通 + 被击中）/ 锤子三帧 / 连击放射线
 *   动态部分 → 仍由代码绘制：
 *     地鼠的**升起与缩回**（贴图时的 y 位移）、连击赞赏的**弹出与淡出**（位移 + 整体
 *     alpha，走 drawSpriteA）、命中扩散环（半径和透明度每帧在变）、文字（连击数会变）
 *
 *   收益有两头：**开发效率**（改图只换 PNG，不重编不刷机）与**帧率**（贴图是顺序写 +
 *   跳过透明像素，几何绘制则有逐像素覆盖率/开方；草地那一大坨全屏重绘尤其贵）。
 *
 * 画面里的文字只在两处：顶部 HUD（原生控件，见宿主）与**连击赞赏**。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgSpriteDraw.h"   // 贴图助手（blit / blitA）+ 素材清单 + 日志钩子

namespace pg {

namespace {

/* 场景配色已经全部烘进 PNG（见 tools/gen_game_art.py），代码里只留**动态元素**的颜色 */
Color burstCol() { return rgba(180, 240, 150); }   // 命中扩散环（半径/透明度每帧在变）
/* 草地近处的近似色。只用于**文字淡出**：画布文字没有 alpha 参数，只能把颜色
 * 往背景色插值来假装"淡出"（贴图走 drawSpriteA 的真 alpha，文字走这条）。
 * ⚠️ 它必须接近 bg.png 下部的平均色，否则淡出会"变出另一种颜色"。 */
Color grassNear() { return rgba(104, 180, 96); }
Color comboGold() { return rgba(255, 214, 64); }   // 连击赞赏文字主色（低阶）
Color comboHot() { return rgba(255, 138, 48); }    // 高阶（连击 ≥8）
Color comboEdge() { return rgba(88, 48, 8); }      // 文字描边

// 洞的中心：3x3 等分布满画布可视区
const int CX[3] = {80, 240, 400};

/* 连击赞赏的时间线（毫秒） */
const int COMBO_MS = 900;
const int COMBO_IN = 150;    // 弹出阶段（靠位移 + 整体 alpha 渐显，不做真缩放）

}  // namespace

GameWhack::GameWhack()
    : score_(0),
      combo_(0),
      maxCombo_(0),
      leftMs_(GAME_MS),
      spawnMs_(0),
      over_(false),
      overFlag_(false),
      hammerX_(-1),
      hammerY_(-1),
      swingMs_(0),
      idleMs_(9999),
      comboFxMs_(0),
      comboFxN_(0),
      comboFxX_(0),
      comboFxY_(0) {
  memset(m_, 0, sizeof(m_));
}

const char *GameWhack::title() const { return "打地鼠"; }
const char *GameWhack::desc() const { return "点冒头的地鼠，30 秒连击计分"; }
const char *GameWhack::tag() const { return "WHACK"; }
Color GameWhack::theme() const { return rgba(206, 150, 86); }

int GameWhack::cxi(int i) const { return CX[i % 3]; }

int GameWhack::cyi(int i) const {
  // 3 行平分可视高度（底部留一点余量，避免贴着画面边）
  int h = vh();
  return h / 4 + (i / 3) * (h / 4);
}

void GameWhack::reset() {
  memset(m_, 0, sizeof(m_));
  for (int i = 0; i < HOLES; ++i) m_[i].waitMs = 200 + rand() % 600;
  score_ = 0;
  combo_ = 0;
  maxCombo_ = 0;
  leftMs_ = GAME_MS;
  spawnMs_ = 0;
  over_ = false;
  overFlag_ = false;
  swingMs_ = 0;
  idleMs_ = 9999;
  comboFxMs_ = 0;
}

void GameWhack::wakeOne() {
  int pool[HOLES];
  int n = 0;
  for (int i = 0; i < HOLES; ++i) {
    if (!m_[i].up) pool[n++] = i;
  }
  if (n == 0) return;
  int k = pool[rand() % n];
  Mole &m = m_[k];
  m.up = true;
  m.hit = false;
  m.upMs = 0;
  m.upDur = 620 + rand() % 780;  // 0.62 ~ 1.40 秒
}

int GameWhack::hitAt(int x, int y) const {
  for (int i = 0; i < HOLES; ++i) {
    int dx = x - cxi(i);
    int dy = y - (cyi(i) - 40);  // 完全冒头时地鼠圆心大约在这里
    if (dx * dx + dy * dy <= 58 * 58) return i;
  }
  return -1;
}

void GameWhack::update(int dtMs) {
  for (int i = 0; i < HOLES; ++i) {
    if (m_[i].fxMs > 0) m_[i].fxMs -= dtMs;
  }
  // 锤子/连击动画的计时（**放在 over 判定之前** —— 结束时锤子还要落回去）
  if (swingMs_ > 0) swingMs_ -= dtMs;
  if (idleMs_ < 100000) idleMs_ += dtMs;
  if (comboFxMs_ > 0) comboFxMs_ -= dtMs;
  if (over_) return;
  if (state_ != GSTATE_RUNNING) return;  // READY / PAUSED 时不计时不出鼠

  leftMs_ -= dtMs;
  if (leftMs_ <= 0) {
    leftMs_ = 0;
    over_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
    sfx(SFX_OVER);
    saveBestIfNeeded(score_);
    return;
  }

  int active = 0;
  for (int i = 0; i < HOLES; ++i) {
    Mole &m = m_[i];
    if (m.up) {
      ++active;
      m.upMs += dtMs;
      if (m.upMs >= m.upDur) {
        m.up = false;
        m.waitMs = 240 + rand() % 700;
      }
    } else if (m.waitMs > 0) {
      m.waitMs -= dtMs;
    }
  }

  // 唤醒节拍：难度递增（30 秒内从 ~820ms 压到 ~260ms），同时最多 4 只
  spawnMs_ -= dtMs;
  if (spawnMs_ <= 0) {
    if (active < 4) wakeOne();
    int elapsed = GAME_MS - leftMs_;
    int base = 820 - elapsed / 40;
    if (base < 260) base = 260;
    spawnMs_ = base + rand() % 260;
  }
}

/* ==================== 场景（贴图）====================
 * 草地底图 / 洞口 / 地鼠 / 锤子 / 放射线全部走素材（PNG 运行时加载），
 * 绘图代码从 ~140 行降到几次 blit —— 这是"素材化"要的**开发效率**。 */

/* 洞口前沿：**画在地鼠之后**，把地鼠下半身挡住 ⇒ "从洞里钻出来"的观感。
 * 素材 hole_front.png 里只保留下半部分（上半透明），所以直接贴即可 —— */
void GameWhack::renderHoleFront(Canvas &c, int i) {
  blit(c, gameart::kWhackHoleFront, cxi(i), cyi(i));
}

void GameWhack::renderMole(Canvas &c, int i) {
  const Mole &m = m_[i];
  int cx = cxi(i), cy = cyi(i);

  /* 冒出动画：前 25% 升起、后 25% 缩回。
   * **位移是动态的**，所以留在代码里（素材只提供两张状态图：普通 / 被击中）。 */
  float t = (float)m.upMs / (float)(m.upDur > 0 ? m.upDur : 1);
  float lift = 1.0f;
  if (t < 0.25f) lift = t / 0.25f;
  else if (t > 0.75f) lift = (1.0f - t) / 0.25f;
  int rise = (int)(lift * 46.0f);
  if (rise < 0) rise = 0;

  blit(c, m.hit ? gameart::kWhackMoleHit : gameart::kWhackMole, cx, cy - rise);
}

/* ==================== 锤子（三帧贴图）==================== */

void GameWhack::renderHammer(Canvas &c) {
  if (hammerX_ < 0) return;   // 还没按过：不画（免得开机就杵一把锤子在屏幕上）

  /* 取哪一帧：
   *   swingMs_ > 0        → 砸中（hit）—— 触摸刚发生，最"用力"的一帧
   *   idleMs_ < 260ms     → 挥下（mid）—— 收势
   *   更久                → 举起（up）—— 待命
   * 三帧的**锚点不同**（离线绕握把转出来，锤头中心在各帧的位置不一样，
   * 见 tools/gen_game_art.py 的 make_frames），按帧取各自的锚点 ⇒
   * 屏幕上**锤头钉在触点不动、只有柄在摆**，看起来就是"照着点下去"。
   * （锚点用的是**锤头中心**，不是握把 —— 用户 2026-09-15：
   *  "点击的时候锤子锤头落点要直接到老鼠头上"。） */
  static const gameart::Def *kHammer[3] = {&gameart::kWhackHammerUp,
                                           &gameart::kWhackHammerMid,
                                           &gameart::kWhackHammerHit};
  int idx = 2;
  if (swingMs_ <= 0) idx = (idleMs_ < 260) ? 1 : 0;
  blit(c, *kHammer[idx], hammerX_, hammerY_);
}

/* ==================== 连击赞赏 ==================== */

void GameWhack::renderCombo(Canvas &c) {
  if (comboFxMs_ <= 0) return;
  const int W = c.width();
  const int H = c.height();
  const int elapsed = COMBO_MS - comboFxMs_;          // 0..COMBO_MS
  const int in = (elapsed < COMBO_IN) ? elapsed : COMBO_IN;
  const int out = (comboFxMs_ < 200) ? (200 - comboFxMs_) : 0;  // 末尾 200ms 淡出

  // 弹出：靠位移 + 放射线伸长做"弹"，不用真缩放（缩放会重采样 = 糊）
  int cy = comboFxY_ - (in * 22 / COMBO_IN) - (out * 10 / 200);
  int cx = comboFxX_;
  // 夹到画面内（赞赏别跑到屏幕外）
  if (cx < 110) cx = 110;
  if (cx > W - 110) cx = W - 110;
  if (cy < 96) cy = 96;
  if (cy > H - 96) cy = H - 96;

  const bool hot = (comboFxN_ >= 8);
  Color main = hot ? comboHot() : comboGold();
  Color edge = comboEdge();
  if (out) {   // 淡出：把颜色向草地色插值（画布文字没有 alpha 参数，只能这样淡）
    main = lerpColor(main, grassNear(), out * 256 / 200);
    edge = lerpColor(edge, grassNear(), out * 256 / 200);
  }

  /* ① 放射线：**贴图 + 整体 alpha**（弹出渐显 → 末端淡出，走 drawSpriteA）。
   * 原来 12 条楔形每帧现画（fillTriangle ×12），现在一张图搞定；
   * "伸长/脉动"这类逐帧形变不做 —— 换成渐显，观感差别不大但省一大截。 */
  int a = 255;
  if (in < COMBO_IN) a = 70 + in * 185 / COMBO_IN;
  if (out) a = a * (200 - out) / 200;
  blitA(c, hot ? gameart::kWhackRaysHot : gameart::kWhackRays, cx, cy, a);

  // ② 文字：先 4 方向画深色描边，再画亮色（草地底色花，没描边会看不清）
  char nb[16];
  snprintf(nb, sizeof(nb), "x%d", comboFxN_);
  const char *word = "COMBO";
  const char *praise = 0;
  if (comboFxN_ >= 12) praise = "无敌!";
  else if (comboFxN_ >= 8) praise = "太强了!";
  else if (comboFxN_ >= 5) praise = "厉害!";
  else if (comboFxN_ >= 3) praise = "不错!";

  int wy2 = cy - 22;   // "COMBO" 基线上移一点，给下面的数字让位
  /* ⚠️ 描边原为 8 方向（3×3 网格 − 中心 = 8 次），三段文字 ⇒ 27 次/帧；
   *    这里减到 **4 方向**（上下左右）⇒ 15 次/帧。连击弹出是短时高频特效，帧率敏感，
   *    而 4 方向描边在花哨的草地底色上已经够清楚。 */
  static const int OX4[4] = {-2, 2, 0, 0};
  static const int OY4[4] = {0, 0, -2, 2};
  for (int pass = 0; pass < 2; ++pass) {
    const Color col = pass ? main : edge;
    const int cnt = pass ? 1 : 4;
    for (int k = 0; k < cnt; ++k) {
      const int ox = pass ? 0 : OX4[k];
      const int oy = pass ? 0 : OY4[k];
      if (praise) c.textCenter(cx + ox, cy - 58 + oy, praise, 2, col);
      c.textCenter(cx + ox, wy2 + oy, word, 2, col);
      c.bigTextCenter(cx + ox, cy + 2 + oy, nb, 2, col);
    }
  }
}

/* ==================== 主绘制 ==================== */

void GameWhack::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();

  /* ① 底图 = **草地 + 9 个洞口**（静态部分全在这一张里，锚点 (0,0) = 左上角对齐）。
   *   一张图顶掉旧版 60 行绘图（135 次 fillRect 全屏 + 110 个草簇 + 泥土斑/花/石头），
   *   而且洞口已合成进去，省掉每帧 9 次单独贴图（≈10 万像素的扫描）。
   *   整图不透明 ⇒ Canvas 走 memcpy 快路径（见 Canvas::Sprite::opaque）。 */
  blit(c, gameart::kWhackBg, 0, 0);

  // ③ 命中特效 + 地鼠（先画鼠，再用"洞口前沿"盖住下半身 ⇒ 从洞里钻出来）
  for (int i = 0; i < HOLES; ++i) {
    const Mole &m = m_[i];
    if (m.fxMs <= 0) continue;
    int cx = cxi(i), cy = cyi(i);
    int p = 260 - m.fxMs;            // 0..260
    int r = 40 + p / 3;              // 40 -> 126
    int a = 200 - (p * 200 / 260);   // 200 -> 0
    if (a > 0) c.strokeEllipse(cx, cy - 40, r, r * 3 / 5, 3, rgba(colorR(burstCol()),
                                                                  colorG(burstCol()),
                                                                  colorB(burstCol()), a));
  }
  for (int i = 0; i < HOLES; ++i) {
    if (m_[i].up) renderMole(c, i);
  }
  for (int i = 0; i < HOLES; ++i) {
    if (m_[i].up) renderHoleFront(c, i);
  }

  renderCombo(c);                        // ④ 连击赞赏
  renderHammer(c);                       // ⑤ 锤子（最上层，跟手）

  // ⑥ 开始 / 结束遮罩（盖住一切，包括锤子）
  if (over_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 165);
    c.textCenter(W / 2, H / 2 - 96, "时间到", 4, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 - 26, "点击屏幕再来一局", 2, rgba(230, 230, 230));
    char b[64];
    snprintf(b, sizeof(b), "本局 %d 分", score_);
    c.textCenter(W / 2, H / 2 + 20, b, 2, rgba(206, 150, 86));
    snprintf(b, sizeof(b), "最高连击 %d", maxCombo_);
    c.textCenter(W / 2, H / 2 + 54, b, 2, rgba(150, 170, 190));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 140);
    c.textCenter(W / 2, H / 2 - 96, "打地鼠", 5, rgba(206, 150, 86));
    c.textCenter(W / 2, H / 2 - 20, "点冒头的地鼠", 2, rgba(230, 230, 230));
    c.textCenter(W / 2, H / 2 + 16, "连击每 5 次多 1 分", 2, rgba(150, 170, 190));
    c.textCenter(W / 2, H / 2 + 60, "点击屏幕开始", 2, rgba(160, 160, 170));
  }
}

bool GameWhack::onTouch(int action, int x, int y) {
  /* 锤子跟随手指：**任何动作都先更新位置**（原来在 DOWN 之后有一句
   * `if (action != PG_TOUCH_DOWN) return true;` 会提前返回，MOVE 就更新不到）。 */
  hammerX_ = x;
  hammerY_ = y;
  if (action == PG_TOUCH_DOWN) {
    swingMs_ = 200;   // 切到"砸中"帧
    idleMs_ = 0;      // 计时归零（松手后靠它回落：260ms 内=挥下帧，之后=举起帧）
  } else if (action == PG_TOUCH_UP) {
    idleMs_ = 0;
  }

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
  if (action != PG_TOUCH_DOWN) return true;  // 按下即判定（反应类要越快越准）

  int i = hitAt(x, y);
  if (i >= 0 && m_[i].up && !m_[i].hit) {
    hitHole(i);
  } else {
    combo_ = 0;  // 点空断连
    sfx(SFX_HIT);
  }
  return true;
}

/* 判定"打中第 i 个洞"的**唯一**实现 —— 触摸与 QA 都走它。
 * 抽出来的原因：连击赞赏的验收要靠 QA 连续命中（盲点必然断连，手点复现不出），
 * 而"QA 走另一条路径"就会验出与真实玩不一样的东西（项目纪律：QA 必须同源）。 */
void GameWhack::hitHole(int i) {
  if (i < 0 || i >= HOLES) return;
  Mole &m = m_[i];
  if (!m.up || m.hit) return;      // 没冒头 / 本次已被打过：忽略
  m.hit = true;
  m.up = false;
  m.waitMs = 300 + rand() % 560;
  m.fxMs = 260;
  ++combo_;
  if (combo_ > maxCombo_) maxCombo_ = combo_;
  score_ += 1 + (combo_ / 5);      // 连击加成
  sfx(SFX_SCORE);
  saveBestIfNeeded(score_);
  /* 连击赞赏：≥2 才弹（第 1 下没必要"夸"），位置取**命中处**并上移一点
   * （别被锤子和地鼠挡），越连越夸张（见 renderCombo 的颜色/形容词分级）。 */
  if (combo_ >= 2) {
    comboFxMs_ = COMBO_MS;
    comboFxN_ = combo_;
    comboFxX_ = cxi(i);
    comboFxY_ = cyi(i) - 74;
  }
}

/* 调试命令（QA `gdbg <...>` 转发）。只暴露"让状态前进"的钩子：
 *   wake <i>  强制第 i 个洞冒头（把停留时间拉到 4s，方便逐条下发命令）
 *   hit  <i>  打中第 i 个洞（没冒头就先补冒头）—— 连击赞赏的验收手段
 *   reset     重开一局
 * ⚠️ 不加"直接改分"的钩子（那会让验收失去意义）。 */
void GameWhack::debugCmd(const char *rest) {
  if (!rest) return;
  if (strncmp(rest, "reset", 5) == 0) {
    reset();
    state_ = GSTATE_RUNNING;
    return;
  }
  const bool isWake = (strncmp(rest, "wake ", 5) == 0);
  const bool isHit = (strncmp(rest, "hit ", 4) == 0);
  if (!isWake && !isHit) return;
  int i = atoi(rest + (isWake ? 5 : 4));
  if (i < 0 || i >= HOLES) return;
  if (!m_[i].up) {                 // 顺带补冒头（QA 一条命令就能推进连击）
    m_[i].up = true;
    m_[i].hit = false;
    m_[i].upMs = 0;
    m_[i].upDur = 4000;            // 拉长停留，免得下一条命令到时它已经缩回去了
  }
  if (isHit) hitHole(i);
}

/* 静止态判定：覆盖层（READY/OVER）本身不动，但**锤子和连击特效还在播**时画面在变，
 * 那几帧必须照常重绘（否则锤子会卡在半空、连击字样冻住）。
 * 三个计时器的含义见 update()：swingMs_ 挥手动作、idleMs_ 距上次挥手的时间
 *（< 300ms 时锤子还在"挥下→举起"之间）、comboFxMs_ 连击特效剩余。 */
bool GameWhack::stillFrame() const {
  if (swingMs_ > 0 || comboFxMs_ > 0) return false;
  if (idleMs_ < 300) return false;
  return state_ != GSTATE_RUNNING;
}

bool GameWhack::onKey(int key) {
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
    if (state_ == GSTATE_OVER) state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

const char *GameWhack::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d 秒", (leftMs_ + 999) / 1000);
  return buf;
}

const char *GameWhack::info2Value(char *buf, int n) const {
  snprintf(buf, n, "x%d", combo_);
  return buf;
}

const char *GameWhack::hint() const {
  if (over_) return "时间到 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点冒头的地鼠 · 连击有加分";
}

bool GameWhack::justGameOver() { return overFlag_; }
void GameWhack::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
