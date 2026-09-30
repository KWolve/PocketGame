/*
 * PgSpot.cpp - 找不同（上下两幅图，点出不一样的地方）
 *
 * 教育目标：**观察力与专注力**（5-10 岁）。上下两幅图，点中"不一样的那处"就圈出来。
 *
 * 设计取舍（对齐 MEMORY.md 的"儿童游戏设计红线"）：
 *   · **无失败惩罚**：点空只是画个灰圈（"我收到了"），不掉分、不扣时间。
 *   · **不限时**：没有倒计时；卡住 16 秒会自己给一处提示（黄圈），不催不骂。
 *   · **差距可辨**：差异只有三类 —— 颜色不同 / 明显大小不同 / 有和没有；不做细微色差。
 *   · **不识字也能玩**：全靠看图，文字只有底部一行与 HUD。
 *
 * ★ 为什么**零 PNG 素材**：两幅图的差异必须由"同一份场景数据 + 一张差异表"算出来。
 *   若改用两张几乎一样的图，改一处忘一处就是**不可见的 bug**（而且要多占几百 KB）。
 *   场景 = item 表（12 条），差异 = diff 表（5 条，改色/改大小/B 图去掉）。
 *
 * 视觉：蜡笔童趣风，共用件在 PgKids.h。
 *
 * ★★ **本类刻意不覆盖 `stillFrame()`** —— 踩过一次，记下来：
 *   "静态场景（只有点中差异才变）⇒ 静止时就跳过重绘" 看着很划算，实测帧率也确实从 49 涨到 99。
 *   但**宿主在 MODE_GAME 里的调用顺序是「先判 stillFrame，返回 true 就直接 break」，
 *   而 `gGame->update()` 在那之后**（mainLogic.cc 的 3403~3415 行）⇒ 报"静止"等于**连 update 一起跳过**。
 *   本关有两处状态靠 update 推进：`idleMs_`（卡住 16s 给提示）与 `celebMs_`（找全后自动进下一关）。
 *   结果 = 找全后画面永远停在"庆祝"，**永不进入下一关**（tools/kids_qa.py 的最后一条断言抓到）。
 *   ⇒ 判据：**只要有"靠时间推进的状态"（倒计时/自动切换/提示计时），就不能报静止**。
 *     对照组：PgConnect 的 stillFrame 只在"所有计时器都归零"时返回 true，所以它是安全的。
 */
#include <stdio.h>
#include <string.h>

#include "PgGames.h"
#include "PgKids.h"
#include "PgLog.h"

namespace pg {

namespace {

using kids::wax;
using kids::ink;
using kids::inkSoft;
using kids::inkText;

enum { ITEM_MAX = 14, DIFF_MAX = 8 };

// 图形种类
enum SpotKind {
  K_DOT = 0,  // 实心圆
  K_FLOWER,   // 五瓣花（外径/内径差别小 = 圆胖）
  K_STAR,     // 五角星（内径小 = 尖）
  K_TRI,      // 三角
  K_SQUARE,   // 方
  K_RING,     // 环
};

// 差异种类
enum SpotDiffType {
  D_COLOR = 0,  // B 图换成另一个颜色
  D_SIZE,       // B 图变大/变小
  D_GONE,       // 只在 A 图里有
};

/* 场景条目：坐标是 0..100 的**归一化方格**（映射到正方形，保证圆不被压扁）。
 * 用 unsigned char 存，一条 5 字节 —— 4 个场景全表也就几百字节，静态常量、不占堆。 */
struct SpotItem {
  unsigned char kind;
  unsigned char color;  // 0..7，见 kids::wax
  unsigned char x, y;
  unsigned char r;
};

struct SpotDiff {
  unsigned char idx;    // 指向 items 的下标
  unsigned char type;   // SpotDiffType
  unsigned char color;  // type==D_COLOR 时 B 图用的颜色
  unsigned char r;      // type==D_SIZE 时 B 图用的半径（同一 0..100 标度）
};

struct SpotScene {
  const char *name;
  unsigned int bg[3];   // 3 段横向底色（0xRRGGBB）
  unsigned char bgY[2]; // 两处分界（0..100）
  int n;
  SpotItem items[ITEM_MAX];
  int nd;
  SpotDiff diffs[DIFF_MAX];
};

/* 4 个场景，每关 5 处差异（5-10 岁的注意力带宽；再多容易烦）。
 * ⚠️ 坐标都要落在 [r, 100-r] 内，否则图形会被面板边裁掉 —— 见 check 项。 */
const SpotScene kScenes[] = {
    {"小花园",
     {0xCFE8F7, 0xE4F2D5, 0xC9E8B8},
     {48, 62},
     12,
     {{K_DOT, 2, 20, 22, 8},   {K_FLOWER, 0, 42, 18, 6},  {K_FLOWER, 6, 66, 20, 7},
      {K_DOT, 4, 84, 30, 5},   {K_DOT, 3, 28, 46, 7},     {K_DOT, 1, 52, 44, 6},
      {K_DOT, 5, 78, 46, 7},   {K_FLOWER, 0, 18, 66, 11}, {K_DOT, 2, 44, 64, 9},
      {K_DOT, 6, 70, 66, 9},   {K_TRI, 5, 32, 86, 7},     {K_TRI, 3, 60, 86, 7}},
     5,
     {{1, D_COLOR, 5, 0}, {5, D_SIZE, 0, 10}, {7, D_COLOR, 5, 0}, {9, D_COLOR, 1, 0},
      {10, D_GONE, 0, 0}}},

    {"海底世界",
     {0xBFE3F0, 0xA8D8EA, 0xF3E3BE},
     {68, 84},
     12,
     {{K_RING, 4, 18, 16, 6},   {K_RING, 5, 42, 12, 5},    {K_RING, 4, 66, 18, 6},
      {K_RING, 5, 86, 14, 5},   {K_FLOWER, 0, 26, 42, 10}, {K_STAR, 2, 54, 40, 10},
      {K_FLOWER, 6, 80, 46, 10}, {K_DOT, 3, 30, 68, 8},    {K_DOT, 1, 58, 66, 9},
      {K_DOT, 5, 82, 70, 8},    {K_TRI, 1, 26, 88, 7},     {K_TRI, 6, 62, 88, 7}},
     5,
     {{1, D_COLOR, 2, 0}, {5, D_SIZE, 0, 13}, {9, D_COLOR, 2, 0}, {10, D_GONE, 0, 0},
      {3, D_SIZE, 0, 9}}},

    {"果子园",
     {0xEAF6DC, 0xD8EFC4, 0xC6E7AD},
     {40, 78},
     12,
     {{K_DOT, 0, 18, 18, 8},  {K_DOT, 1, 44, 14, 7},   {K_DOT, 0, 70, 18, 8},
      {K_DOT, 6, 86, 32, 6},  {K_DOT, 2, 24, 42, 7},   {K_DOT, 0, 52, 40, 9},
      {K_DOT, 1, 78, 46, 7},  {K_RING, 5, 20, 64, 7},  {K_RING, 6, 48, 62, 8},
      {K_RING, 4, 74, 64, 7}, {K_TRI, 1, 32, 86, 7},   {K_TRI, 3, 62, 86, 7}},
     5,
     {{1, D_COLOR, 4, 0}, {5, D_SIZE, 0, 13}, {8, D_COLOR, 1, 0}, {11, D_GONE, 0, 0},
      {3, D_SIZE, 0, 10}}},

    {"小沙滩",
     {0xFDF0CF, 0xBFE3F0, 0xF3E3BE},
     {40, 64},
     12,
     {{K_DOT, 2, 18, 16, 9},   {K_SQUARE, 0, 44, 14, 6},  {K_SQUARE, 6, 70, 18, 7},
      {K_RING, 4, 86, 30, 6},  {K_TRI, 5, 26, 38, 7},     {K_TRI, 1, 52, 36, 8},
      {K_TRI, 3, 78, 46, 7},   {K_RING, 4, 18, 58, 7},    {K_FLOWER, 2, 44, 60, 9},
      {K_FLOWER, 0, 68, 62, 8}, {K_STAR, 5, 30, 82, 9},   {K_STAR, 3, 66, 82, 8}},
     5,
     {{1, D_COLOR, 3, 0}, {5, D_SIZE, 0, 11}, {9, D_COLOR, 5, 0}, {10, D_GONE, 0, 0},
      {2, D_SIZE, 0, 10}}},
};
const int kSceneCount = (int)(sizeof(kScenes) / sizeof(kScenes[0]));

// 面板几何：两块 224 高的画布上下排，下面留一行字
const int PANEL_H = 224;
const int PANEL_GAP = 8;
const int PANEL_TOP = 14;

const int HINT_AFTER_MS = 16000;  // 卡住多久给提示
const int HINT_SHOW_MS = 5200;    // 提示展示多久
const int CELEBRATE_MS = 1800;    // 找全后的庆祝（之后自动下一关）
const int TAP_MS = 520;           // 点空时的灰圈

// ---------------- 几何（自由函数：不依赖游戏实例，便于复用到两幅图） ----------------
void panelRect(int vw, int which, int &px, int &py, int &pw, int &ph) {
  pw = vw - 28;
  ph = PANEL_H;
  px = 14;
  py = PANEL_TOP + which * (PANEL_H + PANEL_GAP);
}

void itemAt(int vw, int which, const SpotItem &it, int &cx, int &cy, int &r) {
  int px, py, pw, ph;
  panelRect(vw, which, px, py, pw, ph);
  const int side = ph;  // 正方形映射区：保证圆不被压扁（面板是横的，左右留白）
  const int ox = px + (pw - side) / 2;
  cx = ox + it.x * side / 100;
  cy = py + it.y * side / 100;
  r = it.r * side / 100;
}

void drawShape(Canvas &c, const SpotItem &it, int cx, int cy, int r, Color col) {
  if (r < 2) r = 2;
  switch (it.kind) {
    case K_FLOWER:
      kids::spikeShape(c, cx, cy, r, r * 55 / 100, 5, -90.0f, col, 2);
      break;
    case K_STAR:
      kids::spikeShape(c, cx, cy, r, r * 42 / 100, 5, -90.0f, col, 2);
      break;
    case K_TRI: {
      int xs[3] = {cx, cx - r * 9 / 10, cx + r * 9 / 10};
      int ys[3] = {cy - r, cy + r * 3 / 4, cy + r * 3 / 4};
      kids::polyBlock(c, cx, cy, xs, ys, 3, col, 2);
      break;
    }
    case K_SQUARE: {
      int d = r * 8 / 10;
      int xs[4] = {cx - d, cx + d, cx + d, cx - d};
      int ys[4] = {cy - d, cy - d, cy + d, cy + d};
      kids::polyBlock(c, cx, cy, xs, ys, 4, col, 2);
      break;
    }
    case K_RING: {
      int th = r / 3;
      if (th < 3) th = 3;
      if (th > 6) th = 6;
      kids::ring(c, cx, cy, r, th, col);
      break;
    }
    default:
      kids::disc(c, cx, cy, r, col, 2);
      break;
  }
}

/* 画一块面板。withDiffs=true 时按差异表改写（B 图）——
 * 这是"两幅图只有差异不同"的**唯一保证点**：任何新差异都只能加在 diff 表里。 */
void drawPanel(Canvas &c, int vw, int scene, int foundMask, int hintMs, int hintIdx,
               int which, bool withDiffs) {
  int px, py, pw, ph;
  panelRect(vw, which, px, py, pw, ph);
  const SpotScene &sc = kScenes[scene];

  // ① 底色：3 段横向色带
  int y0 = py;
  for (int band = 0; band < 3; ++band) {
    int y1 = (band < 2) ? py + ph * sc.bgY[band] / 100 : py + ph;
    c.fillRect(px, y0, pw, y1 - y0, rgb(sc.bg[band]));
    y0 = y1;
  }
  c.strokeRect(px, py, pw, ph, 3, ink());

  // ② 条目
  for (int i = 0; i < sc.n; ++i) {
    const SpotItem &it = sc.items[i];
    Color col = wax(it.color);
    int rOver = -1;  // -1 = 用条目自带半径；-2 = 本图不画
    if (withDiffs) {
      for (int d = 0; d < sc.nd; ++d) {
        const SpotDiff &df = sc.diffs[d];
        if (df.idx != i) continue;
        if (df.type == D_GONE)
          rOver = -2;
        else if (df.type == D_COLOR)
          col = wax(df.color);
        else if (df.type == D_SIZE)
          rOver = df.r * ph / 100;  // 像素半径
        break;
      }
    }
    if (rOver == -2) continue;
    int cx = 0, cy = 0, r = 0;
    itemAt(vw, which, it, cx, cy, r);
    if (rOver >= 0) r = rOver;
    drawShape(c, it, cx, cy, r, col);
  }

  // ③ 已找到的差异：两幅图都圈出来（让小孩看到"这一对"）
  for (int d = 0; d < sc.nd; ++d) {
    if (!(foundMask & (1 << d))) continue;
    const SpotItem &it = sc.items[sc.diffs[d].idx];
    int cx = 0, cy = 0, r = 0;
    itemAt(vw, which, it, cx, cy, r);
    kids::ring(c, cx, cy, r + 12, 5, wax(0));
  }

  // ④ 提示：只给在 A 图上，黄色一圈，不闪不吵
  if (hintMs > 0 && hintIdx >= 0 && which == 0) {
    const SpotItem &it = sc.items[sc.diffs[hintIdx % sc.nd].idx];
    int cx = 0, cy = 0, r = 0;
    itemAt(vw, which, it, cx, cy, r);
    kids::ring(c, cx, cy, r + 18, 4, wax(2));
  }
}

}  // namespace

// ---------------- 卡片元信息 ----------------
const char *GameSpot::title() const { return "找不同"; }
const char *GameSpot::desc() const { return "上下两幅图，点出不一样的地方"; }
const char *GameSpot::tag() const { return "SPOT"; }
Color GameSpot::theme() const { return wax(6); }

// ---------------- 场景数据访问 ----------------
int GameSpot::sceneN() const { return kScenes[scene_].n; }
int GameSpot::diffN() const { return kScenes[scene_].nd; }

// ---------------- 生命周期 ----------------
void GameSpot::startScene(int idx) {
  const int n = kSceneCount;
  if (n <= 0) return;
  idx %= n;
  if (idx < 0) idx += n;
  scene_ = idx;
  foundMask_ = 0;
  found_ = 0;
  idleMs_ = 0;
  hintMs_ = 0;
  hintIdx_ = -1;
  celebMs_ = 0;
  tapMs_ = 0;
}

void GameSpot::reset() {
  totalFound_ = 0;
  elapsedMs_ = 0;
  startScene(0);
}

void GameSpot::update(int dtMs) {
  if (tapMs_ > 0) tapMs_ -= dtMs;
  if (hintMs_ > 0) hintMs_ -= dtMs;

  if (celebMs_ > 0) {
    celebMs_ -= dtMs;
    if (celebMs_ <= 0) startScene(scene_ + 1);  // 循环关卡（不做"通关即结束"）
    return;
  }
  if (state_ != GSTATE_RUNNING) return;
  elapsedMs_ += dtMs;

  idleMs_ += dtMs;
  if (idleMs_ >= HINT_AFTER_MS && hintIdx_ < 0) {
    for (int i = 0; i < diffN(); ++i) {
      if (!(foundMask_ & (1 << i))) {
        hintIdx_ = i;
        hintMs_ = HINT_SHOW_MS;
        break;
      }
    }
    idleMs_ = 0;
  }
}

// ---------------- 绘制 ----------------
void GameSpot::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  kids::paperBg(c);

  drawPanel(c, vw(), scene_, foundMask_, hintMs_, hintIdx_, 0, false);
  drawPanel(c, vw(), scene_, foundMask_, hintMs_, hintIdx_, 1, true);

  if (tapMs_ > 0) kids::ring(c, tapX_, tapY_, 26, 4, inkSoft());  // 点空的反馈（不惩罚）

  const int ty = PANEL_TOP + 2 * PANEL_H + PANEL_GAP + 12;
  if (celebMs_ > 0) {
    c.textCenter(W / 2, ty, "全找到啦！", 4, wax(3));
  } else if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 130);
    c.textCenter(W / 2, H / 2 - 92, "找不同", 5, wax(6));
    /* ★ 档 2 已是下限（档 1 只有 16px，太小），所以只能**缩短文案**：
     *   原文案 14 字 × 32 = 448px = 安全上限（两侧仅剩 16px，汉字墨迹几乎贴边）。 */
    c.textCenter(W / 2, H / 2 - 16, "上下两图，找出不同处", 2, rgba(255, 255, 255));
    c.textCenter(W / 2, H / 2 + 28, "点击屏幕开始", 2, rgba(200, 200, 205));
  } else {
    /* 进度条手工排版："找到" + n + "/" + m + "处"。
     * 为什么不用 snprintf 拼整串再 textCenter：那会让 genfont.py 把本文件的短字面量
     * 灌进该档字库（档位越高越贵，中文 48px 一枚上千字节）。
     * 这里字面量只有 "找到" "/" "处" 三个短串，数字走 number() ⇒ 零新增字形。 */
    const int sc = 2;
    const int wl = c.textW("找到", sc);
    const int w1 = c.numberW(found_, sc);
    const int ws = c.textW("/", sc);
    const int w2 = c.numberW(diffN(), sc);
    const int wend = c.textW("处", sc);
    int x = (W - (wl + w1 + ws + w2 + wend)) / 2;
    c.text(x, ty, "找到", sc, inkText());
    x += wl;
    c.number(x, ty, found_, sc, inkText());
    x += w1;
    c.text(x, ty, "/", sc, inkText());
    x += ws;
    c.number(x, ty, diffN(), sc, inkText());
    x += w2;
    c.text(x, ty, "处", sc, inkText());
  }
}

// ---------------- 输入 ----------------
int GameSpot::hitDiff(int x, int y, int &whichPanel) {
  for (int which = 0; which < 2; ++which) {
    int px, py, pw, ph;
    panelRect(vw(), which, px, py, pw, ph);
    if (x < px || x >= px + pw || y < py || y >= py + ph) continue;
    whichPanel = which;
    const SpotScene &sc = kScenes[scene_];
    for (int d = 0; d < sc.nd; ++d) {
      const SpotItem &it = sc.items[sc.diffs[d].idx];
      int cx = 0, cy = 0, r = 0;
      itemAt(vw(), which, it, cx, cy, r);
      int hit = r + 16;  // 判定比图形大一圈（低龄手指容错）
      if (hit < 26) hit = 26;
      long long dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= (long long)hit * hit) return d;
    }
    return -1;  // 落在这一块图里，但不是差异处
  }
  return -1;
}

bool GameSpot::onTouch(int action, int x, int y) {
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

  int which = -1;
  int d = hitDiff(x, y, which);
  if (d < 0) {
    tapX_ = x;
    tapY_ = y;
    tapMs_ = TAP_MS;  // 点在差异之外：灰圈表示"收到了"，但不扣分
    return true;
  }
  if (foundMask_ & (1 << d)) return true;  // 已经找到过

  foundMask_ |= (1 << d);
  ++found_;
  ++totalFound_;
  idleMs_ = 0;
  hintMs_ = 0;
  hintIdx_ = -1;
  sfx(SFX_SCORE);
  saveBestIfNeeded(totalFound_);

  if (found_ >= diffN()) {
    celebMs_ = CELEBRATE_MS;
    sfx(SFX_CLEAR);
  }
  return true;
}

bool GameSpot::onKey(int key) {
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
const char *GameSpot::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", scene_ + 1, kSceneCount);
  return buf;
}

const char *GameSpot::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", found_, diffN());
  return buf;
}

const char *GameSpot::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "点出两幅图里不一样的地方（不限时）";
}

// QA 钩子：只加"让状态前进"的，不加"直接改分"的
void GameSpot::debugCmd(const char *rest) {
  char cmd[24];
  int k = 0;
  while (rest[k] && rest[k] != ' ' && k < 23) {
    cmd[k] = rest[k];
    ++k;
  }
  cmd[k] = 0;

  if (strcmp(cmd, "q") == 0) {
    // 打印本关的差异列表（第几处 / 类型 / 图形 / 归一化位置 / 是否已找到）
    // 验收不能靠肉眼找差异 —— 这条让脚本能算出"该点哪里"
    char line[240];
    int off = snprintf(line, sizeof(line), "qa q scene=%d items=%d diffs=%d |", scene_,
                       sceneN(), diffN());
    for (int d = 0; d < diffN(); ++d) {
      const SpotItem &it = kScenes[scene_].items[kScenes[scene_].diffs[d].idx];
      int cx = 0, cy = 0, r = 0;
      itemAt(vw(), 0, it, cx, cy, r);
      off += snprintf(line + off, sizeof(line) - off, " d%d(k%d %d,%d r%d found%d)", d,
                      (int)it.kind, cx, cy, r, (foundMask_ >> d) & 1);
    }
    logInfo("%s", line);
    return;
  }
  if (strcmp(cmd, "find") == 0) {
    // 找下一处未找到的差异 —— 走的是与触摸**同一条**判定路径（否则验的与玩到的不是一回事）
    const SpotScene &sc = kScenes[scene_];
    for (int d = 0; d < sc.nd; ++d) {
      if (foundMask_ & (1 << d)) continue;
      const SpotItem &it = sc.items[sc.diffs[d].idx];
      int cx = 0, cy = 0, r = 0;
      itemAt(vw(), 0, it, cx, cy, r);
      logInfo("qa find -> tap diff %d at (%d,%d)", d, cx, cy);
      onTouch(PG_TOUCH_DOWN, cx, cy);
      return;
    }
    logInfo("qa find: all found");
    return;
  }
  if (strcmp(cmd, "next") == 0) {
    startScene(scene_ + 1);
    logInfo("qa next -> scene %d", scene_);
    return;
  }
  logInfo("qa: unknown cmd");
}

}  // namespace pg
