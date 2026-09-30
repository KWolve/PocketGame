/*
 * PgGames.h - 全部游戏类声明 + 游戏注册表
 *
 * 扩展方式（新增一款游戏只需两步）：
 *   1) 写一个 pg::Game 子类（.h 里声明、.cpp 里实现，仿 Pg2048.cpp）
 *   2) 在 PgGames.cpp 的 kGameTable[] 里加一行（工厂函数）
 * 主界面的游戏列表由注册表驱动，自动多出卡片，无需改 UI。
 */
#ifndef PG_GAMES_H_
#define PG_GAMES_H_

#include "PgGame.h"
#include "PgMusic.h"

namespace pg {

// ---------------- 2048 ----------------
class Game2048 : public Game {
 public:
  Game2048();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "最大块"; }
  const char *info1Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  struct Slide {
    int fr, fc, tr, tc;
    int value;
    bool merged;
  };

  bool moveTiles(int dir);  // 0=上 1=右 2=下 3=左
  void spawnTile();
  void addPop(int r, int c);
  bool canMove() const;
  int maxTile() const;
  void layout(int &bx, int &by, int &cell, int &gap) const;
  int tileX(int c, int cell, int gap, int bx) const;
  int tileY(int r, int cell, int gap, int by) const;

  int grid_[4][4];
  int next_[4][4];
  int score_;
  bool over_;
  bool overFlag_;
  bool won_;

  // 滑动动画
  Slide slides_[32];
  int slideCount_;
  int animMs_;       // <0 表示无动画
  bool pendingEnd_;  // 动画结束时需要落子
  // 弹跳表现（合成出的格 + 新生成的格）
  int popCells_[8];
  int popCount_;
  bool popOn_;
  int popMs_;
  int mergeCount_;  // 本步合成出的格数（>0 时播合成音）

  // 滑动手势
  bool dragging_;
  int downX_, downY_;
  int lastX_, lastY_;
  bool consumed_;
};

// ---------------- 俄罗斯方块 ----------------
class GameTetris : public Game {
 public:
  GameTetris();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);
  // 底部软按钮（左 / 下 / 右 / 转）：把原先"四种手势挤一处"的冲突彻底消掉
  const SoftButton *softButtons(int &n) const;
  bool onSoftButton(int id);

  int score() const { return score_; }
  const char *info1Label() const { return "消除行"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "等级"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { CW = 10, CH = 20 };
  void newPiece();
  bool fits(int type, int rot, int px, int py) const;
  bool testMove(int dx, int dy, int drot);
  void lockPiece();
  void applyClear();
  void cellOf(int type, int rot, int idx, int &dx, int &dy) const;
  int dropInterval() const;

  uint8_t board_[CH][CW];  // 0 空，1..7 颜色索引
  int curType_, curRot_, curX_, curY_;
  int nextType_;
  int bag_[7];
  int bagIdx_;
  int score_, lines_, level_;
  int dropAcc_;
  bool over_;
  bool overFlag_;
  int flashLines_[4];
  int flashCount_;
  int flashMs_;
  bool pendingClear_;
  int lockDelay_;
  int lastClearScore_;

  // 手势
  bool dragging_;
  int downX_, downY_;
  int lastX_, lastY_;
  long pressMs_;
  bool consumed_;
  int moveStep_;
  int softDropAcc_;

  // 底部软按钮（id 独立编号，避免与 PG_KEY_* 语义混淆）
  enum { SOFT_LEFT = 1, SOFT_DOWN, SOFT_RIGHT, SOFT_ROT };
  mutable SoftButton soft_[4];  // mutable：坐标需在 const 的 softButtons() 里惰性算
  mutable int softN_;
};

// ---------------- 打飞机 ----------------
class GamePlane : public Game {
 public:
  GamePlane();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "生命"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "关卡"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { MAX_BULLET = 40, MAX_ENEMY = 12, MAX_EBULLET = 24, MAX_EXPL = 16 };

  struct Bullet { bool alive; float x, y, vy; };
  struct EBullet { bool alive; float x, y, vy; };
  struct Enemy {
    bool alive;
    float x, y, vy, phase;
    int hp;
    int kind;  // 0 小机 1 中机 2 精英
    int w, h;
    float shootCd;
  };
  struct Expl { bool alive; float x, y; int ms; int maxMs; float r; };

  void spawnEnemy();
  void fire();
  void addExpl(float x, float y, float r, int ms);
  void killPlayer();

  Bullet bullets_[MAX_BULLET];
  EBullet ebullets_[MAX_EBULLET];
  Enemy enemies_[MAX_ENEMY];
  Expl expls_[MAX_EXPL];
  float px_, py_, targetX_, targetY_;
  int lives_;
  int score_;
  int level_;
  int fireCd_;
  int spawnCd_;
  int invincibleMs_;
  int shakeMs_;
  bool over_;
  bool overFlag_;
  bool dragging_;
};

// ---------------- 小鸟过水管 ----------------
// ---------------- 贪吃蛇 ----------------
class GameSnake : public Game {
 public:
  GameSnake();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "长度"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "步长"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { MAX_LEN = 400 };
  struct Cell {
    int r, c;
  };

  void step();                  // 走一格（含吃food/碰撞判定）
  void spawnFood();
  bool occupied(int r, int c) const;
  void turn(int want);          // 0=上 1=右 2=下 3=左
  void layout(int &bx, int &by, int &cell) const;

  Cell cells_[MAX_LEN];  // cells_[0] 是头
  int score_;
  int dir_, pending_;
  int stepAcc_, interval_;
  int len_;
  int food_;   // r * COLS + c
  int grow_;   // 还要长长几节
  bool over_, overFlag_;
  bool dragging_;
  int downX_, downY_;
  int moved_;
  int flashMs_;
};

class GameFlappy : public Game {
 public:
  GameFlappy();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "水管"; }
  const char *info1Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { MAX_PIPE = 4 };
  struct Pipe {
    bool alive;
    float x;
    int gapY;
    int gapH;
    bool passed;
  };

  void flap();
  void spawnPipe();

  float by_, vy_;
  float birdX_;
  Pipe pipes_[MAX_PIPE];
  int score_;
  int spawnCd_;
  int groundOffset_;
  float birdRot_;
  bool over_;
  bool overFlag_;
  bool started_;
  int lastFlapMs_;
  // 粒子（撞管/落地）
  int shakeMs_;
};

// ---------------- 注册表 ----------------
// ---------------- 扫雷 ----------------
class GameMines : public Game {
 public:
  GameMines();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return opened_; }
  bool showBest() const { return false; }  // 扫雷看"剩余雷/用时"，不是最高分
  const char *info1Label() const { return "剩余雷"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "用时"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { COLS = 9, ROWS = 9, CELL = 50, MINES = 10, MAXCELL = COLS * ROWS };
  struct Cell {
    uint8_t mine, open, flag, near;
  };

  void layout(int &bx, int &by, int &cell) const;
  void generate(int safeR, int safeC);  // 首点安全：避开首点及其 8 邻域
  void reveal(int r, int c);            // 递归展开
  void openAll();                       // 踩雷后亮出所有雷
  bool checkWin();
  int idx(int r, int c) const { return r * COLS + c; }
  int minesLeft() const;

  Cell cells_[MAXCELL];
  int opened_, flags_, elapsedMs_, boomMs_;
  int downR_, downC_, downMs_;
  bool over_, overFlag_, win_, firstClick_, flagMode_;
  int cell_, bx_, by_;  // 最后一帧的网格几何（触摸命中用）
};

// ---------------- 推箱子 ----------------
class GameSokoban : public Game {
 public:
  GameSokoban();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return moves_; }
  bool showBest() const { return false; }
  const char *info1Label() const { return "步数"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "关卡"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { MAXW = 12, MAXH = 12, MAXUNDO = 512 };
  struct Pos {
    int8_t r, c;
  };

  bool loadLevel(int level);
  bool canWalk(int r, int c) const;
  bool boxAt(int r, int c) const;
  bool move(int dr, int dc);   // 返回是否真的动了
  bool undo();
  void pushUndo(int dr, int dc);
  bool solved() const;
  int boxCount() const;

  char wall_[MAXH][MAXW];
  char goal_[MAXH][MAXW];
  char box_[MAXH][MAXW];
  int w_, h_, px_, py_, level_, moves_, boxesTotal_, boxesDone_;
  Pos undo_[MAXUNDO];
  int undoTop_;
  bool win_, overFlag_, dragging_;
  int downX_, downY_, moved_, cell_, bx_, by_;
};

// ---------------- 打砖块 ----------------
class GameBreakout : public Game {
 public:
  GameBreakout();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  /* 覆盖层静止判定：见 Game::stillFrame / stillUnless（把还在播的特效列进来）。 */
  bool stillFrame() const;
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "命"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "关卡"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { COLS = 7, ROWS = 5 };
  void layout();
  void buildLevel();
  void resetBall();
  void loseLife();
  bool brickAlive(int r, int c) const;
  int bricksLeft() const;

  float bx_, by_, vx_, vy_;
  float px_, targetX_;
  int lives_, level_, score_, bricks_;
  bool launched_, over_, overFlag_, win_, dragging_;
  uint8_t brick_[ROWS][COLS];
  int rowColor_[ROWS];
  int cellW_, cellH_, brickTop_, fieldW_, fieldH_;
  int hitMs_, shakeMs_;
};

// ---------------- 打地鼠（触摸专属：靠快速点击反应） ----------------
class GameWhack : public Game {
 public:
  GameWhack();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);
  /* 覆盖层静止，但**锤子回落与连击弹出还在播**时画面仍在动 ⇒ 那几帧不能跳。
   * 见 Game::stillFrame 的说明（跳过重绘能省掉整屏遮罩的 8ms）。 */
  bool stillFrame() const;

  int score() const { return score_; }
  const char *info1Label() const { return "剩余"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "连击"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

  /* 调试命令（QA `gdbg` 转发）：`hit <i>` 强制判定第 i 个洞命中（**连击赞赏的验收手段**
   * —— 盲点必然断连，靠手点复现不出连击）、`wake <i>` 强制第 i 个洞冒头、`state` 打印状态。 */
  void debugCmd(const char *rest);

 private:
  enum { HOLES = 9, GAME_MS = 30000 };
  struct Mole {
    bool up;     // 是否冒头中
    bool hit;    // 本次是否已被打中
    int upMs;    // 已冒出时长
    int upDur;   // 本次冒出总时长
    int waitMs;  // 未冒头时的等待
    int fxMs;    // 命中特效剩余
  };
  int cxi(int i) const;   // 第 i 个洞的中心 x
  int cyi(int i) const;
  void wakeOne();         // 让一个洞随机冒头
  int hitAt(int x, int y) const;  // 命中哪个洞（-1 无）
  void hitHole(int i);    // ★ 判定"打中第 i 个洞"的**唯一**实现（触摸与 QA 都走它）

  Mole m_[HOLES];
  int score_, combo_, maxCombo_, leftMs_;
  int spawnMs_;  // 下一次唤醒的节拍倒计时（随游戏时长递减 = 难度递增）
  bool over_, overFlag_;

  /* ---- 锤子（三帧贴图，清单见 core/PgGameArt.h）----
   * 锤子是**手指的替身**：**锤头中心落在触点上**（= 打击点就是落点，
   * 用户 2026-09-15 定："点击的时候锤子锤头落点要直接到老鼠头上"）。
   * 三帧动作（举起/挥下/砸中）由 tools/gen_game_art.py 离线绕握把旋转得到，
   * **每帧锚点不同**（旋转后做过平移居中），所以贴图时必须用该帧自己的锚点。 */
  int hammerX_, hammerY_;   // 锤子握把位置（画布坐标）；<0 表示还没按过（不画）
  int swingMs_;             // 挥手动作剩余（>0 = 正在"砸中"帧）
  int idleMs_;              // 距上次挥手的时间（用来决定回落到哪一帧）

  /* ---- 连击赞赏（COMBO 弹出）---- */
  int comboFxMs_;           // 特效剩余时间（0 = 不显示）
  int comboFxN_;            // 触发时的连击数（定格，不随后续连击变）
  int comboFxX_, comboFxY_; // 弹出位置（= 命中处，让"夸赞"出现在打中那里）

  void renderHoleFront(Canvas &c, int i);  // 洞口前沿（盖住地鼠下半身 → "从洞里钻出来"）
  void renderMole(Canvas &c, int i);    // 地鼠本体
  void renderHammer(Canvas &c);         // 锤子（最上层）
  void renderCombo(Canvas &c);          // 连击赞赏图
};

// ---------------- 记忆翻牌（图案配对，牌面纯几何图形不写字） ----------------
class GameMemory : public Game {
 public:
  GameMemory();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return matched_; }
  const char *scoreLabel() const { return "已配对"; }
  bool showBest() const { return false; }
  const char *info1Label() const { return "步数"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "用时"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { COLS = 4, ROWS = 4, CARDS = COLS * ROWS, PAIRS = CARDS / 2 };
  struct Card {
    int sym;    // 0..PAIRS-1 图案编号
    bool open;  // 当前是否翻开
    bool done;  // 已配对
  };
  int at(int x, int y) const;  // 屏幕坐标 → 卡片下标（-1 无）
  void shuffle();
  void layout(int &bx, int &by, int &cw, int &ch) const;

  Card c_[CARDS];
  int first_, second_;  // 待比较的两张（-1 无）
  int hideMs_;          // 第二张翻开后"翻回去"的倒计时
  int steps_, matched_, elapsedMs_;
  bool over_, overFlag_;
};

// ---------------- 五子棋（点一下落子，内置轻量 AI） ----------------
class GameGomoku : public Game {
 public:
  GameGomoku();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return winCount_; }
  const char *scoreLabel() const { return "胜局"; }
  bool showBest() const { return false; }
  const char *info1Label() const { return "手数"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "状态"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  // 13 路（不是标准 15 路）：480px 宽上 15 路每格只有 32px，手指点不准；
  // 13 路每格 37px，配合"吸附到最近交叉点"手感可接受。
  enum { N = 13 };
  void layout(int &cell, int &bx, int &by) const;
  int at(int x, int y, int &gx, int &gy) const;  // 屏幕坐标 → 棋盘坐标
  bool win(int x, int y, int who) const;
  int lineScore(int x, int y, int who) const;    // 某点对 who 的价值（AI 用）
  void aiTurn();

  uint8_t b_[N][N];     // 0 空 / 1 黑（玩家）/ 2 白（AI）
  int lastX_, lastY_; // 最后落子（画高亮圈）
  int turns_;
  int winner_;        // 0 进行中 / 1 玩家胜 / 2 AI 胜 / 3 和棋
  int winCount_;
  int thinkMs_;       // AI "思考"延时，避免瞬间落子
  int aiX_, aiY_;     // 待落子位置（-1 = 无）
  bool overFlag_;
};

// ---------------- 数字华容道（点相邻块滑动） ----------------
class GameSlide : public Game {
 public:
  GameSlide();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return steps_; }
  const char *scoreLabel() const { return "步数"; }
  bool showBest() const { return false; }
  const char *info1Label() const { return "用时"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "状态"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  enum { N = 4, CELLS = N * N };
  void layout(int &cell, int &bx, int &by) const;
  int at(int x, int y) const;        // 屏幕坐标 → 格子下标（-1 无）
  void newBoard();                   // 生成必定有解的盘面
  bool canMove(int idx) const;       // 该格是否与空格相邻
  bool solved() const;

  int t_[CELLS];   // 0 = 空格
  int empty_;
  int steps_, elapsedMs_;
  bool won_, overFlag_;
  int fxMs_;       // 完成时高亮动画
};

// ---------------- 反应计时（看谁反应快 · 原生控件大字） ----------------
//
// 为什么用原生控件而不是画布：**这个游戏的全部内容就是一个"毫秒数"** ——
// 画布只有 8x12 点阵 ASCII，放到 96px 是 8 倍放大，数字边缘会糊成一团；
// 原生 ZKTextView 走设备矢量字库，任意字号都清晰（见 docs/ui-design-baseline.md §8）。
// 本类因此一行画布代码都没有，只提供状态机 + 原生 UI 描述（与番茄钟同一套路）。
class GameReact : public Game {
 public:
  GameReact();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &) {}                       // 原生 UI：不碰画布
  bool onTouch(int, int, int) { return false; }  // 反应区是原生按钮
  bool onKey(int key);

  int score() const { return bestMs_ > 0 ? bestMs_ : 0; }
  bool showBest() const { return false; }        // "最好成绩"自己显示在副行
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

  // ---- 原生 UI（nativePage = 2，窗口 WinReact）----
  bool nativeUi() const { return true; }
  int nativePage() const { return 2; }
  const char *uiText(int slot, char *buf, int n) const;
  const char *uiButton(int i) const;
  int uiButtonStyle(int i) const;
  bool onUiButton(int i);
  uint32_t uiAccent() const;

 private:
  enum { PH_IDLE = 0, PH_WAIT, PH_NOW, PH_RESULT, PH_EARLY };
  void armWait();  // 进入等待（随机 1.2~3.6 秒后变绿）
  void tap();      // 反应区被按下

  int phase_;
  int waitMs_;     // WAIT 剩余毫秒
  int elapsedMs_;  // NOW 已过毫秒（= 反应时间）
  int lastMs_;     // 本轮反应时间
  int bestMs_;     // 历史最好
  int sumMs_;      // 累计（算平均）
  int rounds_;     // 有效轮次
  bool overFlag_;
};

// ---------------- 时钟套件（原生控件整屏窗口 WinClockSuite） ----------------
/*
 * 世界时钟 + 闹钟（增删改/开关/存储）+ 到点响铃提醒。
 *
 * 为什么**不复用** uiText/uiButton 那套模板：它有 5 行闹钟 × 4 个控件 + 一整块编辑区
 * （时/分 ± 、重复、保存/删除/取消）+ 独立的世界时钟页，按钮远多于"8 键/20 键"模板，
 * 所以由 mainLogic 的 `syncClockSuite()` 直接驱动（见 mainLogic.cc 的"时钟套件"区）。
 * 本类只负责列表信息（标题/描述/图标标识/配色）与 `nativePage() = 3`。
 *
 * 闹钟数据与响铃在 `src/platform/PgAlarm.*`：**独立守护线程**，不依赖任何页面，
 * 所以用户在任何应用里都会响（含 wifi / 蓝牙遥控这类独立 ftu）。
 */
class GameClockSuite : public Game {
 public:
  GameClockSuite() {}

  const char *title() const { return "时钟套件"; }
  const char *desc() const { return "世界时钟 · 闹钟 · 自动校时"; }
  const char *tag() const { return "钟"; }
  Color theme() const { return 0xFF2C5FD9; }

  void reset() {}
  void update(int dtMs) { (void)dtMs; }
  void render(Canvas &) {}                       // 原生 UI：不碰画布
  bool onTouch(int, int, int) { return false; }  // 全部由原生按钮接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return "B 键 返回列表 · 到点自动响铃并亮屏"; }
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}

  // ---- 原生 UI（nativePage = 3，窗口 WinClockSuite / WinWorld / WinAlarmRing）----
  bool nativeUi() const { return true; }
  int nativePage() const { return 3; }
};

// ---------------- IPTV 网络电视 ----------------
/**
 * IPTV：把 HLS（m3u8）直播频道做成"选台 → 播放"两页。
 *
 * 为什么自己实现 HLS：本板 ffmpeg **没编 HLS 解封装**（见 docs/iptv.md），
 * 而 IPTV 源基本都是 m3u8。所以由 `src/platform/PgHls.*` 自己
 * "拉清单 → 顺序下 .ts 分片 → 拼成连续 TS 流"，再挂一个本地 HTTP 中继，
 * 交给**已验收过的** `pg::StreamPlayer`（ffmpeg mpegts + 设备硬解）去播。
 *
 * ⚠️ **本板硬约束：只能硬解 H264，且解码上限 960x544（约 52 万像素）**。
 *    720p（92 万像素）会直接把解码通道打爆（见 PgStream.cpp 的 kMaxDecodePixels）。
 *    所以选台页会**自动挑 master playlist 里最低码率的档位**（往往是 240p/360p），
 *    频道表也应当优先收录 ≤480p 的源。
 *
 * 频道表：`resources/iptv/channels.m3u`（随固件走），
 *         `/data/iptv.m3u` 存在时**优先用它**（用户自己更新，不怕源失效）。
 *
 * 本类只负责列表信息与 `nativePage() = 4`；两个页面的内容由 mainLogic 的
 * `syncIptv()` / `syncIptvPlay()` 驱动（与其他原生控件应用同一套路）。
 */
class GameIptv : public Game {
 public:
  GameIptv() {}

  const char *title() const { return "网络电视"; }
  const char *desc() const { return "IPTV · HLS 直播频道"; }
  const char *tag() const { return "视"; }
  Color theme() const { return 0xFF8C3FC7; }

  void reset() {}
  void update(int dtMs) { (void)dtMs; }
  void render(Canvas &) {}                       // 原生 UI：不碰画布
  bool onTouch(int, int, int) { return false; }  // 全部由原生按钮接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return "B 键 返回频道列表"; }
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}

  // ---- 原生 UI（nativePage = 4，窗口 WinIptv / WinIptvPlay）----
  bool nativeUi() const { return true; }
  int nativePage() const { return 4; }
};

// ---------------- 番茄时钟 ----------------
class ToolPomodoro : public Game {
 public:
  ToolPomodoro();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &) {}                       // 原生 UI：不画布
  bool onTouch(int, int, int) { return false; }  // 按钮由原生 ZKButton 接管
  bool onKey(int key);

  int score() const { return done_; }
  bool showBest() const { return false; }
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

  // ---- 原生 UI（见 PgGame.h 的说明）----
  bool nativeUi() const { return true; }
  const char *uiText(int slot, char *buf, int n) const;
  const char *uiButton(int i) const;
  int uiButtonStyle(int i) const;
  bool onUiButton(int i);
  uint32_t uiAccent() const;

 private:
  void nextPhase(bool countDone);
  int phaseTotalMs() const;

  int workMin_, breakMin_, remainMs_, phase_, done_;
  bool running_, overFlag_;
  int flashMs_, lastBeepMs_;
};

// ---------------- 定时器 ----------------
class ToolTimer : public Game {
 public:
  ToolTimer();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &) {}
  bool onTouch(int, int, int) { return false; }
  bool onKey(int key);

  int score() const { return setMs_ / 1000; }
  bool showBest() const { return false; }
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver();
  void clearGameOverFlag();

  // ---- 原生 UI ----
  bool nativeUi() const { return true; }
  const char *uiText(int slot, char *buf, int n) const;
  const char *uiButton(int i) const;
  int uiButtonStyle(int i) const;
  bool onUiButton(int i);
  uint32_t uiAccent() const;

 private:
  void addTime(int deltaMs);
  void startStop();

  int setMs_, leftMs_, addMs_;
  bool running_, finished_, overFlag_;
  int flashMs_, lastBeepMs_;
};

// ---------------- 秒表 ----------------
class ToolStopwatch : public Game {
 public:
  ToolStopwatch();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &) {}
  bool onTouch(int, int, int) { return false; }
  bool onKey(int key);

  int score() const { return elapsedMs_ / 100; }
  bool showBest() const { return false; }
  const char *hint() const;
  const char *keyBar() const;

  // ---- 原生 UI ----
  bool nativeUi() const { return true; }
  const char *uiText(int slot, char *buf, int n) const;
  const char *uiButton(int i) const;
  int uiButtonStyle(int i) const;
  bool onUiButton(int i);
  uint32_t uiAccent() const;

 private:
  enum { MAXLAP = 3 };
  int elapsedMs_, lapCount_, lastLapMs_;
  bool running_;
  int lapMs_[MAXLAP];
};

// ---------------- 计算器 ----------------
class ToolCalc : public Game {
 public:
  ToolCalc();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs) { (void)dtMs; }
  void render(Canvas &) {}
  bool onTouch(int, int, int) { return false; }
  bool onKey(int key);

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const;
  const char *keyBar() const;
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}

  // ---- 原生 UI ----
  bool nativeUi() const { return true; }
  int nativePage() const { return 1; }  // 1 = 计算器模板
  const char *uiText(int slot, char *buf, int n) const;
  const char *uiButton(int i) const;
  int uiButtonStyle(int i) const;
  bool onUiButton(int i);

 private:
  void press(char key);   // 数字/运算符/功能键
  void compute();

  char entry_[24];   // 当前输入
  int entryLen_;
  double acc_;       // 累加值
  char op_;          // 待执行运算符
  double result_;    // 上次结果
  char status_[40];  // 状态行（表达式）
  int pressedKey_, pressedMs_;
};

// ---------------- 消消乐（糖果三消 · 触摸优先） ----------------
/*
 * 参考产品 = 开心消消乐（用户 2026-09-15 指定"UI 效果参考这个"）。
 * 素材见 tools/gen_game_art.py 的 match3 段（糖果/选中框/爆花 + 整屏花园棋盘底图），
 * 能力对照表与验收记录见 docs/match3.md。
 *
 * 玩法：8x8 棋盘 + 7 种糖果，滑动（或点两下）交换相邻两格，
 *       三个及以上同色连成一线即消除；消除后上方糖果下落、顶部补新糖并可**连锁**。
 * 关卡制：每关 25 步，达到目标分即过关（目标随关卡递增），步数用尽未达标则重试本关。
 */
class GameMatch3 : public Game {
 public:
  GameMatch3();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);
  /* 交换/消除/下落/洗牌/分数弹出期间画面在动 ⇒ 那几帧不能跳（见 Game::stillFrame）。 */
  bool stillFrame() const;

  int score() const { return totalScore_; }
  const char *info1Label() const { return "关卡"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "步数"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

  /* QA 通道（宿主 `gdbg <...>` 转发）。只暴露"让状态前进"的钩子（见 Game::debugCmd）：
   *   board              打印棋盘（每格一个数字，- 表示空格）+ **特殊块单独一行**
   *                      （`.` 无 / `H` 横炸弹 / `V` 竖炸弹 / `R` 彩虹球；
   *                       刻意不改 board 行格式，否则 PC 侧解析脚本要跟着改）
   *   state              打印关卡/步数/本关分/总分/阶段/**已生成的特殊块数**
   *   swap r1 c1 r2 c2   走 touch 的**同一条**交换路径（三消验收的主力手段）
   *   shuffle            强制洗牌（验"无解自动重排"的分支）
   *   power r c <H|V|R|N> 直接把某格设成特殊块（**验收钩子**：4 连/5 连在真机上要凑很久，
   *                      而"炸弹引爆""彩虹球交换"这两条分支必须验；只改棋盘状态、不动分数）
   *   goal <n>           把本关目标分改成 n（**验收钩子**：不改分数，只改过关线，
   *                      否则验一次过关要连消二十几次）
   *   moves <n>          把剩余步数改成 n（**验收钩子**：验"步数用完"分支用，
   *                      同样不改分数 —— 25 步全打光太慢）
   *   level <n>          跳到第 n 关（重置棋盘/步数，不改分数） */
  void debugCmd(const char *rest);

 private:
  enum { COLS = 8, ROWS = 8, TYPES = 7, CELL = 56, BX = 16, BY = 46 };
  // 动画阶段（与 GSTATE_* 正交：GSTATE 表示"这局在干嘛"，本枚举表示"这一帧在演什么"）
  enum Phase { PH_PLAY = 0, PH_SWAP, PH_REVERT, PH_CLEAR, PH_FALL };

  /* ---- 特殊块（对齐参考产品的 4 连/5 连奖励）----
   * 存法与糖果**正交**：`cell_` 仍然是糖果颜色，`power_` 记"它是不是特殊块"。
   * 为什么不用"特殊块 = 几个新种类值"：那样 `cell_` 的类型空间会与颜色混在一起，
   * 匹配扫描、下落、洗牌、彩虹球判色全都要额外特判 —— 分成两个数组反而更少分支。
   *   PW_ROW/PW_COL：4 连生成，**保留糖果色**（还能继续被同色匹配），消除时清整行/整列
   *   PW_RAINBOW   ：5 连生成，**无色**（不参与颜色匹配，`collectRuns` 把它当隔断），
   *                  与它交换 ⇒ 清掉"对方那颗糖"的全部同色；被波及 ⇒ 清掉场上最多的颜色
   */
  enum { PW_NONE = 0, PW_ROW, PW_COL, PW_RAINBOW };

  enum {
    SWAP_MS = 170,    // 交换/回退动画
    CLEAR_MS = 240,   // 消除动画（糖果淡出 + 爆花）
    POP_MS = 900,     // 得分弹出（上飘 + 淡出）
    SHUF_MS = 800,    // "重新排列"提示
    MOVES_PER_LEVEL = 25,
    TARGET_BASE = 600,   // 第 1 关目标分
    TARGET_STEP = 450,   // 每关递增
  };

  // ---- 棋盘 ----
  bool inGrid(int r, int c) const { return r >= 0 && r < ROWS && c >= 0 && c < COLS; }
  int idx(int r, int c) const { return r * COLS + c; }
  void newBoard();                                  // 生成"无初始三连"的棋盘
  bool findMatches(bool out[ROWS][COLS]) const;     // 标记所有 3+ 连线，返回有没有
  /* 扫描所有 run：标记消除集合 out；`spawn`/`kind` 非空时顺带记下">=4 的 run"要生成什么特殊块。
   * findMatches() 就是它传空指针的薄封装（避免两套扫描逻辑各写一遍）。 */
  bool collectRuns(bool out[ROWS][COLS], unsigned char kind[ROWS][COLS]) const;
  void expandSpecials();                            // 展开特殊块效果（会级联扩散）
  void buildRainbowClear(int rr, int rc, bool out[ROWS][COLS]) const;
  bool hasAnyMatch() const;                         // 只判有没有（洗牌/死局用）
  bool hasAnyMove();                                // 有没有"能消的相邻交换"（死局检测）
  void shuffleBoard();                              // 重排到"无匹配且有解"
  void trySwap(int r1, int c1, int r2, int c2);     // ★ 唯一交换入口（触摸与 QA 同源）
  void startClear(const bool mark[ROWS][COLS]);
  void startFall();
  void afterAnim();                                 // 动画收尾（状态机的核心）
  void settleAfterFall();                           // 过关/失败/死局
  void nextLevel();
  void retryLevel();
  void resetLevelState();
  void addScore(int gain, int cells, int combo);
  void popScore(int px, int py, int value);
  int cellX(int c) const { return BX + c * CELL; }
  int cellY(int r) const { return BY + r * CELL; }
  void cellDrawPos(int r, int c, int &x, int &y) const;  // 含动画偏移
  void renderBoard(Canvas &c);
  void renderPowerMark(Canvas &c, int x, int y, int kind);  // 炸弹上的方向标记（代码画）
  void renderTopBar(Canvas &c);
  void renderScorePop(Canvas &c);
  void renderOverlay(Canvas &c);

  signed char cell_[ROWS][COLS];   // 糖果种类 0..TYPES-1，-1 = 空
  unsigned char power_[ROWS][COLS];  // 特殊块类型（PW_*），0 = 普通糖果
  unsigned char trig_[ROWS][COLS];   // 本轮消除中该格特殊块是否已展开（防重复扩散）
  unsigned char spawn_[ROWS][COLS];  // 本轮要在这些格子**生成**的特殊块（PW_*）
  signed char spawnCol_[ROWS][COLS]; // 生成时保留的糖果颜色
  int fallFrom_[ROWS][COLS];       // 本格糖果"从上方落下多少格"（像素 = 值 × CELL）
  bool marked_[ROWS][COLS];        // 本轮要消除的格

  int level_, movesLeft_, target_, levelScore_, totalScore_;
  int phase_, animMs_, animTotal_;
  int swapA_, swapB_;              // 交换的两格（线性下标），渲染时据此插值滑动
  int combo_;                      // 本次移动内的连锁次数（1 = 第一次消除）
  int maxCombo_;
  int clearCells_;                 // 本轮消除的糖果数（拿去算分/弹窗）
  int powerMade_;                  // 本局生成的炸弹/彩虹球数（info2 显示，也是验收判据）
  int popMs_, popVal_, popX_, popY_;
  int shufFxMs_;
  bool won_;
  bool overFlag_;

  // 触摸
  int selR_, selC_;                // 已选中的格（-1 = 无）
  int dragR_, dragC_, dragX_, dragY_;  // 拖拽起点（用于"滑动交换"）
};

// ---------------- 摇骰子（3 颗骰子 · 离线 3D 烘帧 · 触摸优先） ----------------
/*
 * 用户 2026-09-15 需求：「摇骰子的游戏，基于 FlyThings UI 框架，3 个骰子，
 * 要模拟 3D 效果和声音」，参考产品 = App Store《简易骰子 - 朋友聚会摇色子模拟器》。
 *
 * **3D 是离线算好的**：tools/gen_game_art.py 用正交投影 + 背面剔除 + 面法线光照，
 * 把"6 个静止面（值为 v 的面朝上）"和"12 帧翻滚（绕斜轴转满一圈 ⇒ 可无缝循环）"
 * 渲成 PNG；运行时只做 1:1 贴图。理由：画布**不做运行时缩放**（工程铁律），
 * 立体感只能离线烘完 —— 顺带也避开了每帧画立方体多边形的开销。
 * 细节与坑见 docs/dice-game.md。
 *
 * 玩法：点屏幕（或按暂停键）摇一次；三颗骰子翻滚后**从左到右依次停定**，
 *       和值写进结果板；豹子（三同）/ 顺子 / 大（≥11）/ 小（≤10）会给评语和音效。
 *       没有输赢，可以一直摇（这就是"聚会比大小"的用法）。
 */
class GameDice : public Game {
 public:
  GameDice();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);
  /* 翻滚/落定期间画面每帧在变 ⇒ false；停定后（PH_IDLE）⇒ true，
   * 宿主会跳过重绘（省掉"整屏桌面 + 3 颗骰子"的重贴）。 */
  bool stillFrame() const;

  int score() const { return sum_; }        // 本次和值 3..18
  const char *scoreLabel() const { return "点数"; }
  const char *info1Label() const { return "第几次"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "最佳"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;

  /* QA 通道（宿主 `gdbg <...>` 转发）。只暴露"让状态前进"的钩子：
   *   roll           摇一次 —— **走与触摸完全相同的入口** startRoll()
   *   force a b c    指定**下一次**摇出的点数（各 1..6）。豹子/顺子这类分支
   *                  靠随机摇几乎复现不出来，用它把分支固定下来
   *                  （只定骰子、不改分数，所以验收仍然有意义）
   *   state          打印 三颗点数 / 和值 / 评语 / 阶段 / 次数 / 最佳 */
  void debugCmd(const char *rest);

 private:
  enum {
    DICE_N = 3,
    IMG = 152,              // 骰子帧图边长（== 素材尺寸，1:1）
    IMG_R = IMG / 2,        // 锚点 = 图中心（素材已按立方体中心对齐）
    ROLL_FRAMES = 12,       // 翻滚帧数（与素材一致）
    ROLL_FRAME_MS = 55,     // 换帧间隔（12 帧转一圈 ≈ 660ms）
    ROLL_MS = 900,          // 第 0 颗（最左）停定的时刻
    STOP_GAP_MS = 150,      // 相邻两颗停定的间隔（左→右依次落定）
    SETTLE_MS = 300,        // 最后一颗停定后的落定弹跳 / 结果板出现
    CLATTER_MS = 120,       // 翻滚中"骰子碰撞"音效的节拍
    CY = 190,               // 骰子中心 y
    CX0 = 88,               // 第 0 颗中心 x（三颗间距 152，左右各留 12）
  };
  // 动画相位（与 GSTATE_* 正交：GSTATE 说"这局在干嘛"，本枚举说"这一帧在演什么"）
  enum Phase { PH_IDLE = 0, PH_ROLL, PH_SETTLE };

  // 结果板（与素材 dice/bg.png 里烘的那块板严格对齐）
  enum { BOARD_X0 = 40, BOARD_Y0 = 320, BOARD_X1 = 440, BOARD_Y1 = 455 };

  void startRoll();                              // ★ 唯一入口（触摸/按键/QA 同源）
  int stopMs(int i) const { return ROLL_MS + i * STOP_GAP_MS; }
  bool rolling(int i) const { return phase_ == PH_ROLL && animMs_ < stopMs(i); }
  int cx(int i) const { return CX0 + i * IMG; }
  void drawDie(Canvas &c, int i);
  void drawBoard(Canvas &c);
  const char *verdict() const;                   // 豹子 / 顺子 / 大 / 小 / 空

  int val_[DICE_N];      // 本次摇出的点数（1..6）
  int force_[DICE_N];    // QA 指定的点数（0 = 没指定）
  int phase_;
  int animMs_;           // PH_ROLL：已翻滚时长；PH_SETTLE：已落定时长
  int clatterMs_;        // 下次"骰子碰撞"音效的倒计时
  int stopSfx_;          // 已经播过几次"落定"音（= 已停定的颗数）
  int rolls_;            // 累计摇动次数
  int sum_;              // 本次和值（0 = 还没摇过）
};

/* ==================== 儿童益智三件套（2026-09-16）====================
 *
 * 用户需求原话：「有没有适合 5-10 岁小朋友玩的触屏游戏，最好是益智学习类型的」。
 * 现有 21 款里适龄的只有 4 款（消消乐/记忆翻牌/打地鼠/数字华容道），
 * 而**数感 / 识字 / 观察力 / 计算思维**四块完全空白 ⇒ 这三款专门补前三块。
 *
 * 三款共用的四条设计红线（写在这里，改任何一个都要回头看一遍）：
 *   ① **无失败惩罚**：错了不扣分、不重置、不限次数（低龄被"失败"一击就劝退）。
 *   ② **不限时**：一律没有倒计时；节奏慢是特性不是缺点。
 *   ③ **不识字也能玩**：靠数字与图形，文字只做锦上添花。
 *   ④ **难度分档**：5 岁与 10 岁不能吃同一套数值（各自在 .cpp 里解决）。
 *
 * 视觉：统一的「蜡笔童趣风」，共用件在 **core/PgKids.h**（零 PNG 素材）。
 * 三款都是**画布类游戏** ⇒ 不新建 ftu，只加 Game 子类 + 注册 + 图标。
 * ==================================================================== */

// ---------------- 数字连线画（按 1 2 3 的顺序连点成图） ----------------
// 教：数序 + 点数。6 个图案（3~12 个点），画完自动进下一个。
class GameConnect : public Game {
 public:
  GameConnect();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);
  /* 拖动中 / 晃动中 / 提示中 / 庆祝中都要重绘，其余时候画面是死的 ⇒ 让宿主跳过。 */
  bool stillFrame() const;

  int score() const { return completed_; }
  const char *scoreLabel() const { return "完成"; }
  const char *info1Label() const { return "图案"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "连点"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

 private:
  void layout(int &bx, int &by, int &side) const;
  void dotPos(int i, int &x, int &y) const;
  int dotAt(int x, int y) const;
  int shapeN() const;
  void startShape(int idx);
  void connectNext();                          // ★ 唯一连点入口（触摸与 QA 同源）
  void shapeName(Canvas &c, int y) const;      // 图案名（switch 字面量，别改成传表里的串）

  int level_;                  // 当前图案下标
  int next_;                   // 下一个该连的点（== shapeN() 表示已连完）
  int completed_;              // 画好的图案数（= HUD 得分 / 最高分）
  bool drag_;                  // 手指是否按着（拖动画那一段）
  int fx_, fy_;                // 手指位置
  int wobbleIdx_, wobbleMs_;   // 连错时晃动的那个点
  int msgMs_;                  // "先连 N 哦"
  int celebrateMs_;            // 画完后的庆祝（结束后自动下一个）
  bool allDone_, overFlag_;
};

// ---------------- 算术泡泡（点破答案正确的那颗泡泡） ----------------
// 教：心算与数感。三档难度（10 以内加法 → 20 以内加减 → 100 以内加减），按答对数自动升。
class GameBubble : public Game {
 public:
  GameBubble();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *scoreLabel() const { return "答对"; }
  const char *info1Label() const { return "第几题"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "连对"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;

  /* QA 通道（`gdbg <...>`）：
   *   q       打印当前题目与 4 个候选（含正确答案在哪一列）—— 验收不靠盲点
   *   answer  点破"正确泡泡"那一列（走与触摸同一条判定路径）
   *   wrong   点一个错误泡泡（验收"答错不惩罚"这条分支）
   *   next    直接出下一题（按对数不变，所以验收仍然有意义） */
  void debugCmd(const char *rest);

 private:
  enum { BUBBLE_N = 4 };
  struct Bubble {
    int x, y;        // 中心（x 由列号定，y 随上浮推进）
    int val;         // 泡泡上的数字
    int wobbleMs;    // 答错时该泡泡的晃动计时
  };
  int colX(int i) const;
  int playTop() const;
  int playBottom() const;
  void nextQuestion();

  Bubble bub_[BUBBLE_N];
  int a_, qb_, ans_, op_;  // 题目（op_ 0 = 加 1 = 减）
  int score_, qno_, combo_, level_;
  int popMs_, popIdx_;    // 答对后泡泡"胀开"的动画
  int msgMs_, praiseMs_;
  int elapsedMs_;
};

// ---------------- 找不同（上下两幅图点差异） ----------------
// 教：观察力与专注力。4 个程序化场景（零素材），每关 5 处差异；卡住 16 秒给提示。
class GameSpot : public Game {
 public:
  GameSpot() : scene_(0), foundMask_(0), found_(0), totalFound_(0), idleMs_(0),
               hintMs_(0), hintIdx_(-1), celebMs_(0), tapMs_(0), tapX_(0), tapY_(0),
               elapsedMs_(0) {}

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return totalFound_; }
  const char *scoreLabel() const { return "找到"; }
  const char *info1Label() const { return "第几关"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "本关"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;

  /* QA 通道（`gdbg <...>`）：
   *   q      打印本关差异列表（类型 / 图形 / 屏幕坐标 / 是否已找到）
   *   find   找出"下一处还没找到"的差异并点它（走与触摸同一条路径）
   *   next   直接进下一关（已找到数不变） */
  void debugCmd(const char *rest);

 private:
  int sceneN() const;
  int diffN() const;
  void startScene(int idx);
  int hitDiff(int x, int y, int &whichPanel);  // 命中检测（两幅图都能点）

  int scene_;       // 当前场景下标
  int foundMask_;   // 已找到的差异位图（DIFF_MAX <= 8，一个 int 够）
  int found_;       // 本关已找到数
  int totalFound_;  // 累计（= HUD 得分 / 最高分）
  int idleMs_;      // 距离上次找到过去了多久（到点给提示）
  int hintMs_, hintIdx_;
  int celebMs_;     // 找全后的庆祝（结束后自动下一关）
  int tapMs_, tapX_, tapY_;  // 点空时的灰圈反馈
  int elapsedMs_;
};

// ---------------- 数独（木纹棋盘 · 9x9 · 唯一解生成） ----------------
// 界面：整屏木纹棋盘 + 数字键行 1..9 + 动作键行（擦除/笔记/难度/提示/新局）。
// 素材（tools/gen_instr_art.py）：sudoku/bg.png（含 81 个格凹槽）、num_btn、act_btn。
// 三档难度只改"挖掉多少格"；**每一局都是唯一解**（生成器自带解数计数）。
class GameSudoku : public Game {
 public:
  GameSudoku() : sel_(-1), noteMode_(false), diff_(1), score_(0), filled_(0),
                 hints_(0), celebMs_(0), flashMs_(0), flashIdx_(-1), genMs_(0),
                 scaleMs_(0), scaleDir_(1), scaleIdx_(-1), seed_(0), solved_(false),
                 resetCount_(0) {}

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "难度"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "剩余"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}

  /* QA 通道（`gdbg <...>`）—— 只加"让状态前进"的钩子：
   *   q            打印盘面（81 个字符 + 选格 + 难度 + 已填数）
   *   new [seed]   重开一局（给定种子 ⇒ **验收可复现**）
   *   level n      切难度 1..3 并重开
   *   cell r c     选中某格
   *   put r c v    在 (r,c) 落子 v（走与触摸**同一条**代码路径）
   *   one          给"第一个空格"填上正确值（等价于点"提示"）
   *   solve        把剩下的空格全部按正确值填完（用来验收"完成即庆祝"）*/
  void debugCmd(const char *rest);

 private:
  bool genPuzzle(unsigned seed, int diff);
  void placeSelected(int v);
  void eraseSelected();
  void hintOne();
  void recomputeFilled();
  int conflictsAt(int idx) const;   // 该格与同行/列/宫重复的个数（0 = 无冲突）
  int cellAt(int x, int y) const;   // 屏幕坐标 → 格下标（-1 = 不在棋盘上）
  int numAt(int x, int y) const;    // 数字键 0..8（-1 未命中）
  int actAt(int x, int y) const;    // 动作键 0..4（-1 未命中）
  void setDiff(int d);

  int given_[81];      // 题面（0 = 空；**不可改**，也是"是否玩家填入"的判据）
  int cur_[81];        // 玩家当前盘面
  int sol_[81];        // 唯一解
  unsigned short mark_[81];  // 候选数位图（bit v-1）
  int sel_;
  bool noteMode_;
  int diff_;           // 0 简单 / 1 中等 / 2 困难
  int score_;
  int filled_;         // 已填（含题面）
  int hints_;          // 用了多少次提示
  int celebMs_;        // 完成后的庆祝（结束后自动下一难度/新局）
  int flashMs_;        // 落子后的格子闪光
  int flashIdx_;
  int genMs_;          // 上一局生成耗时（QA 看性能）
  int scaleMs_;        // 提示格的呼吸高亮
  int scaleDir_;
  int scaleIdx_;
  unsigned seed_;      // 本局种子（QA 复现用）
  bool solved_;
  unsigned resetCount_;  // 第几局（跟 seed 一起搅进随机数，防止连开两局盘面雷同）
};

// ---------------- 节奏钢琴（下落式跟弹 · 8 键 · 木纹立式琴） ----------------
// 内核（判定窗口/计分）在 core/PgMusic.h；谱面与发声在本类。
// 音符落到底线(y=388)时点对应琴键：Perfect/Good/OK/Miss 三档 + 连击。
class GamePiano : public Game {
 public:
  GamePiano() : song_(0), diff_(1), score_(0), combo_(0), maxCombo_(0),
                nNotes_(0), next_(0), nowMs_(0), startDelayMs_(1500),
                lastJudge_(0), lastJudgeMs_(0), lastJudgeLane_(-1),
                msAcc_(0), overFlag_(false), celebMs_(0), metronomeMs_(0) {
    for (int i = 0; i < 5; ++i) cnt_[i] = 0;
    for (int i = 0; i < 8; ++i) {
      keyFlash_[i] = 0;
      hitMask_[i] = 0;
      keyDown_[i] = false;
    }
    for (int i = 0; i < music::NOTE_MAX; ++i) done_[i] = false;
  }

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "连击"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "进度"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

  /* QA 通道：q（打印谱面与游标）/ key n（按键 n，走与触摸同一条路径）/
   *           auto N（自动把接下来 N 个音符按应击时刻"点对"）/ song n / diff n */
  void debugCmd(const char *rest);

 private:
  void loadSong(int idx);
  bool tapLane(int lane, bool fromAuto);
  int laneAt(int x, int y) const;
  int judgeSummary(char *buf, int n) const;

  int song_, diff_;
  int score_, combo_, maxCombo_;
  music::Note notes_[music::NOTE_MAX];
  int nNotes_;
  int next_;          // 下一个"还没结算"的音符（谱面按 atMs 升序）
  int nowMs_;         // 曲子已进行的毫秒
  int startDelayMs_;  // 起播倒计时（给玩家准备时间）
  int lastJudge_;     // 最近一次判定结果（music::Judge）
  int lastJudgeMs_;
  int lastJudgeLane_;
  int cnt_[5];        // [J_MISS..J_PERFECT] 计数
  int keyFlash_[8];   // 琴键高亮剩余时间
  int hitMask_[8];    // 每个音轨一个位图，标"该音符已结算"
  int msAcc_;         // 累计毫秒（HUD 显示用时）
  bool overFlag_;
  int celebMs_;       // 结算页
  int metronomeMs_;   // 节拍器闪烁相位
  bool keyDown_[8];   // 当前按下的琴键（画压下态）
  bool done_[music::NOTE_MAX];   // 音符是否已结算（命中或漏掉）
};

// ---------------- 打鼓（下落式跟打 · 6 鼓位 · 木纹鼓架 · **四档速度含「入门」**） ----------------
// 速度档：入门 / 慢 / 中 / 快（定义在 core/PgMusic.h 的 `music::drumDiff()`）——
// 「入门」= 下落 3600ms + 判定窗口放宽到 100/200/300ms，给小孩与第一次玩的人；
// **钢琴不共用这张表**（钢琴仍是三档标准窗口，见 PgMusic.h 的说明）。
class GameDrum : public Game {
 public:
  /* 默认档 = music::DRUM_DIFF_DEFAULT（"中"）—— 与加「入门」档之前的手感一致。
   * 档位不落盘：每次进游戏都从这里开始（所以加档不会影响任何历史状态）。 */
  GameDrum() : chart_(0), diff_(music::DRUM_DIFF_DEFAULT), score_(0), combo_(0),
               maxCombo_(0),
               nNotes_(0), next_(0), nowMs_(0), startDelayMs_(1500),
               lastJudge_(0), lastJudgeMs_(0), lastJudgeLane_(-1),
               overFlag_(false), celebMs_(0) {
    for (int i = 0; i < 5; ++i) cnt_[i] = 0;
    for (int i = 0; i < 6; ++i) {
      padFlash_[i] = 0;
      padDown_[i] = false;
    }
    for (int i = 0; i < music::NOTE_MAX; ++i) done_[i] = false;
  }

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset();
  void update(int dtMs);
  void render(Canvas &c);
  bool onTouch(int action, int x, int y);
  bool onKey(int key);

  int score() const { return score_; }
  const char *info1Label() const { return "连击"; }
  const char *info1Value(char *buf, int n) const;
  const char *info2Label() const { return "进度"; }
  const char *info2Value(char *buf, int n) const;
  const char *hint() const;
  bool justGameOver();
  void clearGameOverFlag();

  /* QA 通道：q / pad n / auto N / chart n / diff n */
  void debugCmd(const char *rest);

 private:
  void loadChart(int idx);
  bool hitPad(int lane, bool fromAuto);
  int padAt(int x, int y) const;

  int chart_, diff_;
  int score_, combo_, maxCombo_;
  music::Note notes_[music::NOTE_MAX];
  int nNotes_;
  int next_;
  int nowMs_;
  int startDelayMs_;
  int lastJudge_, lastJudgeMs_, lastJudgeLane_;
  int cnt_[5];
  int padFlash_[6];
  int hitMask_[6];
  bool overFlag_;
  int celebMs_;
  bool padDown_[6];
  bool done_[music::NOTE_MAX];
};

// ---------------- WiFi（卡片元信息） ----------------
// ⚠️ 界面与逻辑都在**独立 ftu**（ui/wifi.html + src/logic/wifiLogic.cc）。
//    卡片点进去 = `openActivity("wifiActivity")`，所以本类的 render/onTouch
//    **永远不会被执行**；留着只是为了主界面列表能取到 title/desc/tag/theme。
class AppSettings : public Game {
 public:
  AppSettings();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset() {}
  void update(int) {}
  void render(Canvas &) {}                        // 界面在 settings.ftu 里，不画布
  bool onTouch(int, int, int) { return false; }    // 触摸由原生控件接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return ""; }
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}
};

/* 2026-09-23 新增：智能家居（Home Assistant 遥控器，slot 37，系统分类）。
 *
 * 与 AppSettings/AppRadio/AppCamera 同类：**只为主界面卡片提供元信息**
 * （title/desc/tag/theme），界面与逻辑全在独立 ftu（ui/ha.html -> ha.ftu -> haActivity，
 * 逻辑 src/logic/haLogic.cc，网络层 src/platform/PgHa.{h,cpp}）。
 * 卡片点进去走 openActivity —— mainLogic 的 kPageApps 表里加了 {"ha","haActivity"}。
 *
 * ⚠️ 加应用必须四处同步（**少一处就是静默失效**）：
 *   ① ui/main.html 的 Icon37  ② tools/gen_icons.py 的 ICONS  ③ mainLogic.cc 的 kIconCount
 *   ④ tools/gen_ui.py 的 HIDDEN_CONTROLS（range(38)）
 * 另外 ScoreStore::MAX_GAMES(=40) 要 > 37 —— 本页 showBest=false 不写最高分，
 * 但同表别的应用靠它，别再让它贴上限。 */
class AppHa : public Game {
 public:
  AppHa();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset() {}
  void update(int) {}
  void render(Canvas &) {}                        // 界面在 ha.ftu 里，不画布
  bool onTouch(int, int, int) { return false; }    // 触摸由原生控件接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return ""; }
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}
};

class AppWifi : public Game {
 public:
  AppWifi();

  const char *title() const;
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset() {}
  void update(int) {}
  void render(Canvas &) {}                        // 界面在 wifi.ftu 里，不画布
  bool onTouch(int, int, int) { return false; }    // 触摸由原生控件接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return ""; }
  bool justGameOver() { return false; }
  void clearGameOverFlag() {}
};


enum AppCategory {
  APP_GAME = 0,  // 游戏
  APP_TOOL,      // 工具（番茄钟/定时器/秒表/计算器）
  APP_SYSTEM,    // 系统（WiFi 状态与设置入口）
  APP_CATEGORY_COUNT
};

struct GameEntry {
  const char *id;                                  // 稳定 id
  Game *(*create)();                               // 工厂
  int category;                                    // AppCategory
  int slot;                                        // 全局稳定槽位（存档索引 0..N-1）
};

// 全部应用数（= kAppTable 的**行数**，不是最大 slot）
int appCount();
/* ⚠️ 参数是**表下标**，不是 slot（两者历史上相等，现在已经脱钩）。
 *   越界会被**夹紧**到有效范围（保证不返回野引用），但这会**掩盖**"拿 slot 当下标"
 *   这类错误 —— 按 slot 查请一律用下面的 appBySlot()。 */
const GameEntry &appEntry(int index);

/* ★ 2026-09-24 新增：slot（稳定存档索引）↔ 表下标 的显式转换。
 *   为什么必须有：kAppTable 里 `slot` 是"永不变的位置"，而下标是"第几行"。
 *   删过应用之后两者**已经脱钩** —— 现在的空洞是 slot 33/34/35/36，
 *   于是“智能家居”slot=37 却只排在第 34 行（下标 33）。
 *   ⇒ 拿 `slot >= appCount()` 当边界、拿 `appEntry(slot)` 当查表，都会**静默**判错：
 *     实测现象 = 启动器上那一格**整格空白** + 点了毫无反应（用户报"程序入口不见了"）。 */
int appIndexForSlot(int slot);                 // slot -> 表下标；找不到返回 -1
const GameEntry *appBySlot(int slot);          // slot -> 表项；找不到返回 nullptr
/* 表里**最大的 slot**。图标控件是 Icon<slot>、存档也是按 slot 索引的
 * ⇒ "图标够不够 / 存档槽够不够"必须跟它比，不能跟 appCount() 比
 *   （删过应用后 slot 会跳号：现在个数 34、最大 slot 37）。 */
int maxAppSlot();

// 分类视图：某分类下第 k 个应用对应的全局槽位（越界返回 -1）
int categoryCount(int category);
int categorySlot(int category, int k);
const char *categoryName(int category);

// 兼容旧调用名（等于 appCount/appEntry）
int gameCount();
const GameEntry &gameEntry(int index);

// 初始化随机种子（进程内只生效一次），logic 层启动时调一次
void seedRandom();

}  // namespace pg

#endif  // PG_GAMES_H_
