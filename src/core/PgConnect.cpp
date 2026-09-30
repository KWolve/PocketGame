/*
 * PgConnect.cpp - 数字连线画（按 1 2 3 … 顺序连点成图）
 *
 * 教育目标：**数序 + 点数**（5-8 岁最硬的刚需）。玩法是"连点成画"——
 *   每连对一个点，笔迹就往前走一格，最后画出一个完整的图案。
 *   小孩不需要识字、不需要读规则，看数字按顺序点就行。
 *
 * 设计取舍（对齐 MEMORY.md 的"儿童游戏设计红线"）：
 *   · **无失败惩罚**：连错不扣分、不重置，只晃动一下并提示"先连 N 哦"（纯正向提示）。
 *   · **不限时**：没有倒计时，慢一点没有代价。
 *   · **不识字也能玩**：圆点上就是阿拉伯数字；文字只做锦上添花。
 *   · **手指友好**：点半径 18px（直径 36，远超 5-10 岁的容错需求），
 *     且"拖过去"和"一个个点"两种手势都认（见 onTouch）。
 *   · **不依赖多点/物理键**：单指可玩。
 *
 * 视觉：蜡笔童趣风，共用件在 PgKids.h（本作零素材、零 PNG）。
 */
#include <stdio.h>
#include <string.h>

#include "PgGames.h"
#include "PgKids.h"

namespace pg {

namespace {

using kids::wax;
using kids::ink;
using kids::inkSoft;
using kids::inkText;
using kids::card;

// 图案顶点表上限
enum { MAXP = 16 };

/* 图案（坐标是 0..100 的归一化方格，y 向下）。
 * 顺序 = 连线顺序 = 圆点上的数字；**按点数从少到多**排，让小孩从 3 个点起步。
 * 名字会在画完后打在屏幕下方（"太棒了！小鱼"）——这是唯一的文字奖励。 */
struct Shape {
  const char *name;
  int n;
  int xs[MAXP];
  int ys[MAXP];
};

const Shape kShapes[] = {
    {"三角形", 3,
     {50, 86, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {12, 84, 84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"正方形", 4,
     {16, 84, 84, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {16, 16, 84, 84, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"小房子", 5,
     {50, 88, 88, 12, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
     {8, 46, 86, 86, 46, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"小鱼", 8,
     {8, 26, 50, 86, 74, 86, 50, 26, 0, 0, 0, 0, 0, 0, 0, 0},
     {50, 30, 26, 16, 50, 84, 74, 68, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"星星", 10,
     {50, 61, 92, 67, 76, 50, 24, 33, 8, 39, 0, 0, 0, 0, 0, 0},
     {6, 35, 36, 56, 86, 68, 86, 56, 36, 35, 0, 0, 0, 0, 0, 0}},
    {"小花", 12,
     {50, 63, 86, 76, 86, 63, 50, 37, 14, 24, 14, 37, 0, 0, 0, 0},
     {8, 27, 29, 50, 71, 73, 92, 73, 71, 50, 29, 27, 0, 0, 0, 0}},
};
const int kShapeCount = (int)(sizeof(kShapes) / sizeof(kShapes[0]));

// 圆点半径（手指容错）：5-10 岁实测建议 ≥ 16，这里给 18。
const int DOT_R = 18;
// 判定半径：比可视半径再放宽 6px —— "差不多点到了"就算对（低龄容错）
const int HIT_R = DOT_R + 8;
// 完成一个图案后的庆祝时长（自动进入下一个）
const int CELEBRATE_MS = 1700;
// 连错时的提示展示时长
const int MSG_MS = 1700;

}  // namespace

GameConnect::GameConnect()
    : level_(0),
      next_(0),
      completed_(0),
      drag_(false),
      fx_(0),
      fy_(0),
      wobbleIdx_(-1),
      wobbleMs_(0),
      msgMs_(0),
      celebrateMs_(0),
      allDone_(false),
      overFlag_(false) {}

const char *GameConnect::title() const { return "数字连线画"; }
const char *GameConnect::desc() const { return "按 1 2 3 的顺序连点，画出图案"; }
const char *GameConnect::tag() const { return "DOTS"; }
Color GameConnect::theme() const { return wax(4); }

// ---------------- 几何 ----------------
void GameConnect::layout(int &bx, int &by, int &side) const {
  int s = vw() - 60;
  int s2 = vh() - 190;
  if (s2 < s) s = s2;
  if (s > 420) s = 420;
  if (s < 180) s = 180;
  side = s;
  bx = (vw() - s) / 2;
  by = 76;  // 上方留出"当前图案/进度"的空间
}

void GameConnect::dotPos(int i, int &x, int &y) const {
  int bx, by, side;
  layout(bx, by, side);
  const Shape &s = kShapes[level_];
  x = bx + s.xs[i] * side / 100;
  y = by + s.ys[i] * side / 100;
}

int GameConnect::shapeN() const { return kShapes[level_].n; }

int GameConnect::dotAt(int x, int y) const {
  int n = shapeN();
  for (int i = 0; i < n; ++i) {
    int dx = 0, dy = 0;
    dotPos(i, dx, dy);
    long long ex = x - dx, ey = y - dy;
    if (ex * ex + ey * ey <= (long long)HIT_R * HIT_R) return i;
  }
  return -1;
}

// ---------------- 生命周期 ----------------
void GameConnect::startShape(int idx) {
  if (idx < 0) idx = 0;
  if (idx >= kShapeCount) idx = kShapeCount - 1;
  level_ = idx;
  next_ = 0;
  drag_ = false;
  wobbleIdx_ = -1;
  wobbleMs_ = 0;
  msgMs_ = 0;
  celebrateMs_ = 0;
}

void GameConnect::reset() {
  completed_ = 0;
  allDone_ = false;
  overFlag_ = false;
  startShape(0);
}

void GameConnect::update(int dtMs) {
  if (wobbleMs_ > 0) wobbleMs_ -= dtMs;
  if (msgMs_ > 0) msgMs_ -= dtMs;
  if (celebrateMs_ <= 0) return;

  celebrateMs_ -= dtMs;
  if (celebrateMs_ > 0) return;

  // 庆祝结束：自动进下一个图案（儿童游戏不该要求"点一下继续"）
  if (level_ + 1 < kShapeCount) {
    startShape(level_ + 1);
  } else {
    allDone_ = true;
    overFlag_ = true;
    state_ = GSTATE_OVER;
  }
}

// ---------------- 绘制 ----------------
/* 图案名：**按关卡 switch 成字面量**，而不是传 kShapes[i].name。
 * 原因：非字面量实参会让 genfont.py 退化成"本文件全部短字面量"，把中文灌进大字档。
 * 六个 case 换来的是字库只多这 18 个字。 */
void GameConnect::shapeName(Canvas &c, int y) const {
  const int cx = c.width() / 2;
  switch (level_) {
    case 0: c.textCenter(cx, y, "三角形", 3, inkText()); break;
    case 1: c.textCenter(cx, y, "正方形", 3, inkText()); break;
    case 2: c.textCenter(cx, y, "小房子", 3, inkText()); break;
    case 3: c.textCenter(cx, y, "小鱼", 3, inkText()); break;
    case 4: c.textCenter(cx, y, "星星", 3, inkText()); break;
    default: c.textCenter(cx, y, "小花", 3, inkText()); break;
  }
}

void GameConnect::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  kids::paperBg(c);

  int bx, by, side;
  layout(bx, by, side);
  int n = shapeN();
  const Shape &sh = kShapes[level_];
  bool finished = (next_ >= n);

  // 顶点像素坐标（每帧算一次，后面复用）
  int px[MAXP], py[MAXP];
  for (int i = 0; i < n; ++i) dotPos(i, px[i], py[i]);

  // ① 完成后：给图案填一层淡色（"图画"浮出来）。
  //    质心用顶点平均值 —— 这几个图案从质心都看得见所有顶点，扇形填充成立。
  if (finished) {
    int cx = 0, cy = 0;
    for (int i = 0; i < n; ++i) {
      cx += sh.xs[i];
      cy += sh.ys[i];
    }
    cx = bx + (cx / n) * side / 100;
    cy = by + (cy / n) * side / 100;
    Color pale = lerpColor(kids::paper(), wax(level_ + 2), 96);
    kids::fanFill(c, cx, cy, px, py, n, pale);
  }

  // ② 已连的线段（完成后多一条收口段）
  int seg = finished ? n : (next_ - 1);
  for (int i = 0; i < seg; ++i) {
    int j = (i + 1) % n;
    kids::thickLine(c, px[i], py[i], px[j], py[j], 5, ink());
  }

  // ③ 正在拖的那一段（跟随手指，浅色虚线感）
  if (drag_ && next_ > 0 && !finished) {
    kids::thickLine(c, px[next_ - 1], py[next_ - 1], fx_, fy_, 4, inkSoft());
  }

  // ④ 圆点
  // ⚠️ 缓冲区按"本函数最长的中文字串"算（"一共画好 %d 个图案"），别按数字位数估 —— 会截断成乱码
  /* 圆点数字一律走 `number()`，**不用 text(buf)**：
   *   · `number()` 不在 genfont.py 的扫描名单里 ⇒ 数字只靠 ALWAYS_ASCII（0-9 恒在），
   *     **不给字库加任何字形**；
   *   · 反例：如果写成 `textCenter(x, y, buf, 2, c)`，genfont 会因为 scale 变量/非字面量
   *     把**整个文件的短字面量**塞进字库（实测中文档位从 79 涨到 174 = 字库 +2MB）。
   * 档位 2 是**字面量**（否则 genfont 会把 1..5 五档全展开）。 */
  const int th = c.textH(2);
  for (int i = 0; i < n; ++i) {
    int x = px[i], y = py[i];
    if (wobbleIdx_ == i && wobbleMs_ > 0) {
      // 连错时左右晃：幅度 5px，周期 ~180ms（不是"抖动惩罚"，只是"这个不对哦"）
      x += (int)(5 * sinf((float)wobbleMs_ / 28.0f));
    }
    bool doneDot = finished || i < next_;
    bool isNext = (!finished && i == next_);
    Color fill = doneDot ? wax(i + 2) : (isNext ? wax(i + 2) : card());
    Color numc = (doneDot || isNext) ? rgba(255, 255, 255) : inkSoft();

    if (isNext) {
      // 当前目标：外圈加一道粗环（静态即可 —— 不做脉动，省掉每帧重绘）
      kids::ring(c, x, y, DOT_R + 7, 4, wax(i + 2));
    }
    kids::disc(c, x, y, DOT_R, fill, 3);
    c.numberCenter(x, y - th / 2, i + 1, 2, numc);
  }

  // ⑤ 底部一行字（唯一的文字区）
  const int ty = by + side + 18;
  /* ⚠️ 这一整段**只能用字面量**（不要 snprintf 出来的串）：
   *   genfont.py 对非字面量实参的调用会退化成"本文件全部短字面量"，
   *   档位≥3 时中文每枚字形上千字节 ⇒ 字库白涨几百 KB。见 PgBubble.cpp 的同类说明。 */
  if (allDone_) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    /* ★ 档 4 而不是 5：6 个中文 × 档 5(80px) = 480px = 画布宽，两侧零留边、看着像被裁。
     *   档 4(64px) ⇒ 384px，两侧各留 48px。`tools/check_textwidth.py` 会拦住这类。 */
    c.textCenter(W / 2, H / 2 - 92, "全部画完啦！", 4, wax(3));
    c.textCenter(W / 2, H / 2 - 16, "每个图案都画好了", 3, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 28, "点击屏幕再来一局", 2, rgba(200, 200, 205));
  } else if (celebrateMs_ > 0) {
    c.textCenter(W / 2, ty, "太棒了！", 4, wax(1));
    shapeName(c, ty + 52);  // 图案名按关卡 switch 成字面量，见该函数注释
  } else if (msgMs_ > 0) {
    c.textCenter(W / 2, ty, "先连前面的数字哦", 3, wax(0));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 130);
    c.textCenter(W / 2, H / 2 - 92, "数字连线画", 5, wax(1));
    // ★ 档 2：原文案档 3 时 9 中文 + 2 空格 + "1" = 504px > 480 屏宽（同 PgBubble 那处）
    c.textCenter(W / 2, H / 2 - 16, "从 1 开始，顺着数字连", 2, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 28, "点击屏幕开始", 2, rgba(200, 200, 205));
  } else {
    c.textCenter(W / 2, ty, "顺着数字连起来", 3, inkSoft());
  }
}

// ---------------- 输入 ----------------
void GameConnect::connectNext() {
  ++next_;
  sfx(SFX_MOVE);
  drag_ = true;
  if (next_ >= shapeN()) {
    // 图案完成
    ++completed_;
    celebrateMs_ = CELEBRATE_MS;
    drag_ = false;
    msgMs_ = 0;
    wobbleIdx_ = -1;
    sfx(SFX_CLEAR);
  }
}

bool GameConnect::onTouch(int action, int x, int y) {
  if (allDone_) {
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
  if (celebrateMs_ > 0) return true;  // 庆祝中不接受输入（马上自动切下一个）

  const int n = shapeN();

  if (action == PG_TOUCH_DOWN) {
    fx_ = x;
    fy_ = y;
    int id = dotAt(x, y);
    if (id == next_) {
      connectNext();
    } else if (id >= 0 && id < n) {
      // 点到了别的点：温和提示，不惩罚
      wobbleIdx_ = id;
      wobbleMs_ = 620;
      msgMs_ = MSG_MS;
      sfx(SFX_HIT);
    } else {
      drag_ = true;  // 从空白处起手也行 —— 小孩常是"拖着画"
    }
    return true;
  }

  if (action == PG_TOUCH_MOVE) {
    fx_ = x;
    fy_ = y;
    if (drag_) {
      int id = dotAt(x, y);
      if (id == next_) connectNext();
    }
    return true;
  }

  // UP
  drag_ = false;
  return true;
}

bool GameConnect::onKey(int key) {
  if (key == PG_KEY_A) {
    if (allDone_) {
      reset();
      state_ = GSTATE_RUNNING;
    } else if (state_ == GSTATE_READY) {
      state_ = GSTATE_RUNNING;
    }
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
const char *GameConnect::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", level_ + 1, kShapeCount);
  return buf;
}

const char *GameConnect::info2Value(char *buf, int n) const {
  if (allDone_) return "完成";
  if (next_ >= shapeN()) return "完成";
  snprintf(buf, n, "%d/%d", next_, shapeN());
  return buf;
}

const char *GameConnect::hint() const {
  if (allDone_) return "全部画完 - 点击屏幕再来一局";
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  if (celebrateMs_ > 0) return "画好啦 - 下一个图案马上来";
  return "按数字顺序连点（可以拖着画）";
}

bool GameConnect::stillFrame() const {
  if (state_ != GSTATE_RUNNING) {
    // 覆盖层上还有东西在动时必须重绘：晃动 / 提示 / 庆祝
    return wobbleMs_ <= 0 && msgMs_ <= 0 && celebrateMs_ <= 0;
  }
  return !drag_ && wobbleMs_ <= 0 && msgMs_ <= 0 && celebrateMs_ <= 0;
}

bool GameConnect::justGameOver() { return overFlag_; }
void GameConnect::clearGameOverFlag() { overFlag_ = false; }

}  // namespace pg
