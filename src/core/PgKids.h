/*
 * PgKids.h - 「蜡笔童趣风」儿童游戏共用视觉件
 *
 * 使用者：PgConnect（数字连线画）/ PgBubble（算术泡泡）/ PgSpot（找不同）。
 * 2026-09-16 用户从三套方案里选定 A「蜡笔童趣风」：米黄纸底 + 粗勾边 + 圆胖形 + 高饱和蜡笔色。
 *
 * 为什么单独一个头：三款要"看起来是一套"。色板与画法放一处，改配色只改这个文件。
 *
 * ★ 三条纪律（别破坏）：
 *   ① 本文件**不调用任何文字 API**（text/textCenter/…）。画布字库由 tools/genfont.py
 *      扫 **.cpp 里** `text()/textCenter()` 调用点上的字面量生成 —— 文字调用留在 .cpp 才收得到，
 *      挪进这里会**静默漏字**（画成空心方框/回退别的档）。见 MEMORY.md 规则 1。
 *   ② **零素材**：全部几何图元画出来（与 PgMemory 同一取舍）。本工程铁律"不许拉伸"因此天然成立。
 *   ③ 尖角图形一律"先描边层、再填充层"（两层同形状、描边层外扩 edgeW）——
 *      比"沿边逐段画圆"便宜一个量级（见下 thickLine 的注释）。
 */
#ifndef PG_KIDS_H_
#define PG_KIDS_H_

#include <math.h>

#include "PgCanvas.h"

namespace pg {
namespace kids {

// ---------------- 蜡笔调色板 ----------------
inline Color paper() { return rgba(253, 246, 227); }      // 米黄纸底 #FDF6E3
inline Color paperLine() { return rgba(243, 231, 203); }  // 纸纹（极浅，别调深）
inline Color ink() { return rgba(138, 90, 43); }          // 勾边棕 #8A5A2B
inline Color inkSoft() { return rgba(186, 146, 100); }    // 次级描边 / 未连点
inline Color inkText() { return rgba(90, 58, 24); }       // 纸上正文 #5A3A18
inline Color card() { return rgba(255, 251, 240); }       // 卡片底（比纸更白）

/* 8 色蜡笔：红 橙 黄 绿 青 蓝 紫 粉。
 * 高饱和、彼此可辨；**颜色不是唯一区分手段** —— 数字/形状要同时能认。 */
inline Color wax(int i) {
  switch (i & 7) {
    case 0: return rgba(255, 107, 107);
    case 1: return rgba(255, 159, 67);
    case 2: return rgba(255, 209, 102);
    case 3: return rgba(123, 211, 137);
    case 4: return rgba(78, 205, 196);
    case 5: return rgba(91, 155, 213);
    case 6: return rgba(167, 139, 250);
    default: return rgba(255, 143, 177);
  }
}
inline int waxCount() { return 8; }

// ---------------- 画法 ----------------

/* 纸底：米黄 + 每 26px 一条极浅横纹（"练习本"的暗示，零素材）。
 * 用 fillRect 分档画，不逐行 —— 逐行是 540 次调用（性能纪律见 docs/game-art-pipeline.md §附）。 */
inline void paperBg(Canvas &c) {
  const int W = c.width(), H = c.height();
  c.clear(paper());
  for (int y = 22; y < H; y += 26) c.fillRect(0, y, W, 1, paperLine());
}

/* 蜡笔块：圆角填充 + 粗勾边。edgeW 画在框**内**，所以 w/h 就是最终占位。 */
inline void panel(Canvas &c, int x, int y, int w, int h, int r, Color fill, int edgeW = 3) {
  c.fillRectRound(x, y, w, h, r, fill);
  c.strokeRect(x, y, w, h, edgeW, ink());
}

/* 圆形蜡笔块：填充 + 粗勾边。 */
inline void disc(Canvas &c, int cx, int cy, int r, Color fill, int edgeW = 3) {
  c.fillCircle(cx, cy, r, fill);
  if (edgeW > 0) c.strokeCircle(cx, cy, r - edgeW / 2, edgeW, ink());
}

/* 粗折线。**为什么不用 fillRect 拼**：斜线用水平/垂直矩形拼出来是"阶梯"，
 * 蜡笔风对这种台阶特别敏感。用沿路径撒小圆（步长 = 半径 ⇒ 无缝隙）画面平滑，
 * 代价是 n/r 次 fillCircle —— 只在动画帧上用，静止帧被 stillFrame() 挡掉。 */
inline void thickLine(Canvas &c, int x0, int y0, int x1, int y1, int t, Color col) {
  int dx = x1 - x0, dy = y1 - y0;
  int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int n = adx > ady ? adx : ady;
  int r = t / 2;
  if (r < 1) r = 1;
  if (n <= 0) {
    c.fillCircle(x0, y0, r, col);
    return;
  }
  int step = r;
  if (step < 1) step = 1;
  for (int i = 0; i <= n; i += step) {
    int x = x0 + (int)((long long)dx * i / n);
    int y = y0 + (int)((long long)dy * i / n);
    c.fillCircle(x, y, r, col);
  }
  c.fillCircle(x1, y1, r, col);  // 收尾补一颗，保证末端不掉
}

/* 从内部点向各顶点扇形填充 —— 星形/花形这类"从中心看得见所有顶点"的图形都够用。
 * （Canvas 只有 fillTriangle，没有任意多边形填充。） */
inline void fanFill(Canvas &c, int cx, int cy, const int *xs, const int *ys, int n,
                    Color col) {
  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;
    c.fillTriangle(cx, cy, xs[i], ys[i], xs[j], ys[j], col);
  }
}

/* 多边形蜡笔块：先画"外扩 edgeW 的描边层"，再画填充层 ⇒ 得到一圈均匀勾边。
 * 顶点必须**从中心看得见**（凸形，或星形/花形的交替内外径）。 */
inline void polyBlock(Canvas &c, int cx, int cy, const int *xs, const int *ys, int n,
                      Color col, int edgeW, float expand = 1.0f) {
  enum { KIDS_MAXP = 24 };
  if (n < 3 || n > KIDS_MAXP) return;
  if (edgeW > 0) {
    int ox[KIDS_MAXP], oy[KIDS_MAXP];
    for (int i = 0; i < n; ++i) {
      ox[i] = cx + (int)((xs[i] - cx) * expand + (xs[i] >= cx ? edgeW : -edgeW) + 0.5f);
      oy[i] = cy + (int)((ys[i] - cy) * expand + (ys[i] >= cy ? edgeW : -edgeW) + 0.5f);
    }
    fanFill(c, cx, cy, ox, oy, n, ink());
  }
  fanFill(c, cx, cy, xs, ys, n, col);
}

/* 星形 / 花瓣形：外径 ro 与内径 ri 交替，共 2*petals 个顶点。
 * petals=5 且 ri/ro 小 = 尖角星；petals=5..6 且 ri/ro 大 = 圆胖花瓣。 */
inline void spikeShape(Canvas &c, int cx, int cy, int ro, int ri, int petals,
                       float rotDeg, Color col, int edgeW) {
  enum { KIDS_MAXV = 24 };
  int n = petals * 2;
  if (n > KIDS_MAXV) return;
  int xs[KIDS_MAXV], ys[KIDS_MAXV];
  const float DEG = 3.14159265f / 180.0f;
  for (int i = 0; i < n; ++i) {
    float r = (i % 2 == 0) ? (float)ro : (float)ri;
    float a = (rotDeg + 360.0f * i / n) * DEG;
    xs[i] = cx + (int)(r * cosf(a) + 0.5f);
    ys[i] = cy + (int)(r * sinf(a) + 0.5f);
  }
  polyBlock(c, cx, cy, xs, ys, n, col, edgeW);
}

/* 圆环（蜡笔圈）：strokeCircle 已带 AA，直接用。 */
inline void ring(Canvas &c, int cx, int cy, int r, int t, Color col) {
  c.strokeCircle(cx, cy, r, t, col);
}

}  // namespace kids
}  // namespace pg

#endif  // PG_KIDS_H_
