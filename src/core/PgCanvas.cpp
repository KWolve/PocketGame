#include "PgCanvas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgFontData.h"

namespace pg {

Color lerpColor(Color a, Color b, int t) {
  if (t <= 0) return a;
  if (t >= 256) return b;
  int ar = colorR(a), ag = colorG(a), ab = colorB(a), aa = colorA(a);
  int br = colorR(b), bg = colorG(b), bb = colorB(b), ba = colorA(b);
  return rgba(ar + (br - ar) * t / 256, ag + (bg - ag) * t / 256,
              ab + (bb - ab) * t / 256, aa + (ba - aa) * t / 256);
}

Color shade(Color c, int delta) {
  int r = colorR(c) + delta, g = colorG(c) + delta, b = colorB(c) + delta;
  if (r < 0) r = 0;
  if (r > 255) r = 255;
  if (g < 0) g = 0;
  if (g > 255) g = 255;
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  return rgba(r, g, b, colorA(c));
}

Canvas::Canvas() : w_(0), h_(0), buf_(0) {}

// 缓冲所有权可能已被交给框架（见 PgCanvas.h），析构不做释放，交给进程退出回收
Canvas::~Canvas() {}

bool Canvas::init(int w, int h) {
  if (w <= 0 || h <= 0) return false;
  if (buf_ && w_ == w && h_ == h) return true;  // 尺寸没变且缓冲还在自己手里 -> 复用
  size_t n = (size_t)w * (size_t)h;
  uint32_t *nb = (uint32_t *)malloc(n * 4);
  if (!nb) return false;
  memset(nb, 0, n * 4);
  buf_ = nb;
  w_ = w;
  h_ = h;
  return true;
}

void Canvas::release() {
  // 只有确定所有权还在自己手里才能调用（设备侧通常不适用，见 PgDisplay.h）
  if (buf_) {
    free(buf_);
    buf_ = 0;
  }
  w_ = 0;
  h_ = 0;
}

void Canvas::abandon() {
  // 缓冲已经被框架释放/接管：只把指针忘掉，绝不 free（否则二次释放打死进程）
  buf_ = 0;
  w_ = 0;
  h_ = 0;
}

void Canvas::clear(Color c) {
  if (!buf_) return;
  size_t n = (size_t)w_ * (size_t)h_;
  for (size_t i = 0; i < n; ++i) buf_[i] = c;
}

void Canvas::px(int x, int y, Color c) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
  buf_[(size_t)y * w_ + x] = c;
}

void Canvas::blendPx(int x, int y, Color c, int a) {
  if (!buf_ || x < 0 || y < 0 || x >= w_ || y >= h_ || a <= 0) return;
  uint32_t *p = buf_ + (size_t)y * w_ + x;
  if (a >= 255) { *p = c | 0xFF000000u; return; }
  uint32_t d = *p;
  int ia = 255 - a;
  int r = (colorR(c) * a + (int)((d >> 16) & 0xFF) * ia) / 255;
  int g = (colorG(c) * a + (int)((d >> 8) & 0xFF) * ia) / 255;
  int b = (colorB(c) * a + (int)(d & 0xFF) * ia) / 255;
  *p = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

namespace {
/* 覆盖率 a 的像素：满覆盖直写（快），边缘才混合 */
inline void putAA(Canvas *cv, int x, int y, Color c, float cov) {
#if PG_CANVAS_AA
  if (cov >= 1.0f) cv->px(x, y, c);
  else if (cov > 0.0f) cv->blendPx(x, y, c, (int)(cov * 255.0f + 0.5f));
#else
  if (cov > 0.0f) cv->px(x, y, c);
#endif
}
}  // namespace

/*
 * ★★ 抗锯齿（2026-09-15）—— 这段是全工程"游戏素材有锯齿"的根因所在。
 *
 * 原来：`dx = (int)(0.5 + sqrtf(...))` 取整后 `fillRect` ⇒ 圆的斜边是 1px 台阶。
 * 现在：**距离场覆盖率** —— 像素中心到圆心的距离 d，覆盖率 = r + 0.5 - d（clamp 0..1）。
 *      只有"边缘那一圈"（cov ∈ (0,1)）才做混合，圆内部仍走 `fillRect` 快路径 ⇒ 开销可控
 *      （一颗半径 17 的棋子约 900 像素，其中边缘仅约 100 个）。
 *
 * ⚠️ 覆盖率近似的口径：像素当作 1x1 方块、以中心采样，边界落在 [中心-0.5, 中心+0.5] 内
 *    就按比例混合。这比"2x2 超采样"快，肉眼看不出差别。
 */
void Canvas::fillCircle(int cx, int cy, int r, Color c) {
  if (r <= 0 || !buf_) return;
  const float rf = (float)r;
  const float cxf = (float)cx + 0.5f;
  const float cyf = (float)cy + 0.5f;
  for (int y = cy - r - 1; y <= cy + r + 1; ++y) {
    float dy = (float)y + 0.5f - cyf;
    float t = rf * rf - dy * dy;
    if (t <= 0) continue;
    float dxf = sqrtf(t);
    int xl = (int)floorf(cxf - dxf);
    int xr = (int)floorf(cxf + dxf);
    if (xr < xl) continue;
    // 快路径：整行（含两侧 AA 余量）都在圆内 → 直接填，不做逐像素距离
    float maxdx = fabsf((float)xl + 0.5f - cxf);
    float m2 = fabsf((float)xr + 0.5f - cxf);
    if (m2 > maxdx) maxdx = m2;
    if (maxdx * maxdx + dy * dy <= (rf - 0.5f) * (rf - 0.5f)) {
      fillRect(xl, y, xr - xl + 1, 1, c);
      continue;
    }
    for (int x = xl; x <= xr; ++x) {
      float pdx = (float)x + 0.5f - cxf;
      putAA(this, x, y, c, rf + 0.5f - sqrtf(pdx * pdx + dy * dy));
    }
  }
}

void Canvas::fillRect(int x, int y, int w, int h, Color c) {
  if (!buf_ || w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w;
  int y1 = y + h;
  if (x1 > w_) x1 = w_;
  if (y1 > h_) y1 = h_;
  if (x0 >= x1 || y0 >= y1) return;
  int iw = x1 - x0;
  for (int yy = y0; yy < y1; ++yy) {
    uint32_t *row = buf_ + (size_t)yy * w_ + x0;
    for (int i = 0; i < iw; ++i) row[i] = c;
  }
}

void Canvas::hline(int x0, int x1, int y, Color c) {
  if (x0 > x1) {
    int t = x0;
    x0 = x1;
    x1 = t;
  }
  fillRect(x0, y, x1 - x0 + 1, 1, c);
}

void Canvas::vline(int x, int y0, int y1, Color c) {
  if (y0 > y1) {
    int t = y0;
    y0 = y1;
    y1 = t;
  }
  fillRect(x, y0, 1, y1 - y0 + 1, c);
}

void Canvas::fillRectRound(int x, int y, int w, int h, int r, Color c) {
  if (r <= 0 || w <= 2 * r || h <= 2 * r) {
    fillRect(x, y, w, h, c);
    return;
  }
  fillRect(x + r, y, w - 2 * r, h, c);          // 中间竖条
  fillRect(x, y + r, r, h - 2 * r, c);          // 左
  fillRect(x + w - r, y + r, r, h - 2 * r, c);  // 右
  /* 四角：1/4 圆，**距离场抗锯齿**（原来用整数圆判定 ⇒ 圆角是一圈台阶）。
   * 角心在局部坐标 (r, r)；像素 (dx,dy) 中心到角心的距离 d ⇒ 覆盖率 = r + 0.5 - d。 */
  const float rf = (float)r;
  for (int dy = 0; dy < r; ++dy) {
    float oy = (float)(r - dy) - 0.5f;
    for (int dx = 0; dx < r; ++dx) {
      float ox = (float)(r - dx) - 0.5f;
      float cov = rf + 0.5f - sqrtf(ox * ox + oy * oy);
      if (cov <= 0.0f) continue;
      putAA(this, x + dx, y + dy, c, cov);
      putAA(this, x + w - 1 - dx, y + dy, c, cov);
      putAA(this, x + dx, y + h - 1 - dy, c, cov);
      putAA(this, x + w - 1 - dx, y + h - 1 - dy, c, cov);
    }
  }
}

void Canvas::strokeRect(int x, int y, int w, int h, int t, Color c) {
  fillRect(x, y, w, t, c);
  fillRect(x, y + h - t, w, t, c);
  fillRect(x, y, t, h, c);
  fillRect(x + w - t, y, t, h, c);
}

/*
 * 圆环：**逐行只画环带**，不能靠"画外圆 + 用透明色擦内圆"。
 *
 * ★★ 2026-09-14 血案（用户报"五子棋 AI 落子后红圈里面是黑的"）：
 *   原来的实现是
 *       fillCircle(cx, cy, r, c);
 *       fillCircle(cx, cy, r - t, 0x00000000);   // 以为"透明 = 擦除"
 *   但 `fillRect` 是**直接赋值**（`row[i] = c`，不做 alpha 混合）⇒ 内圈被**真的写成
 *   alpha=0 的像素**；画布合成到屏幕时 alpha=0 就**透出下层页面底（纯黑）**。
 *   结果：**环内部永远是纯黑，把底下的棋子/卡片内容盖掉**。
 *   为什么一直没发现：它盖的正好是深色内容（五子棋黑子 #1C1E24 vs 纯黑 #000000），
 *   肉眼分不出；只有**盖住白子**（AI 落子）时才暴露。
 *   ⇒ 需要"擦除"语义时，**不要用 alpha=0 写像素**；要挖空就自己算环带/环框。
 */
void Canvas::strokeCircle(int cx, int cy, int r, int t, Color c) {
  if (r <= 0 || t <= 0 || !buf_) return;
  const float rf = (float)r;
  const float inf = (float)(r - t);            // 内缘半径（<=0 时退化成实心圆）
  const float cxf = (float)cx + 0.5f;
  const float cyf = (float)cy + 0.5f;
  for (int y = cy - r - 1; y <= cy + r + 1; ++y) {
    float dy = (float)y + 0.5f - cyf;
    float t2 = rf * rf - dy * dy;
    if (t2 <= 0) continue;
    float dxf = sqrtf(t2);
    int xl = (int)floorf(cxf - dxf), xr = (int)floorf(cxf + dxf);
    for (int x = xl; x <= xr; ++x) {
      float pdx = (float)x + 0.5f - cxf;
      float d = sqrtf(pdx * pdx + dy * dy);
      float cov = rf + 0.5f - d;               // 外缘覆盖率
      if (cov > 1.0f) cov = 1.0f;
      if (inf > 0.0f) {
        float covIn = d - (inf - 0.5f);        // 内缘覆盖率
        if (covIn < cov) cov = covIn;
      }
      putAA(this, x, y, c, cov);
    }
  }
}

void Canvas::fillEllipse(int cx, int cy, int rx, int ry, Color c) {
  if (rx <= 0 || ry <= 0 || !buf_) return;
  const float rxf = (float)rx, ryf = (float)ry;
  const float cxf = (float)cx + 0.5f, cyf = (float)cy + 0.5f;
  for (int y = cy - ry - 1; y <= cy + ry + 1; ++y) {
    float dy = (float)y + 0.5f - cyf;
    float vy = ryf + 0.5f - fabsf(dy);            // 该行在竖直方向的覆盖率
    if (vy <= 0.0f) continue;
    if (vy > 1.0f) vy = 1.0f;
    float k = 1.0f - (dy * dy) / (ryf * ryf);
    if (k <= 0.0f) continue;
    float dxf = rxf * sqrtf(k);
    int xl = (int)floorf(cxf - dxf), xr = (int)floorf(cxf + dxf);
    for (int x = xl; x <= xr; ++x) {
      float pdx = (float)x + 0.5f - cxf;
      float cov = (dxf + 0.5f - fabsf(pdx)) * vy;   // 水平覆盖 × 竖直覆盖
      putAA(this, x, y, c, cov);
    }
  }
}

/** 椭圆环 —— **只画环带**（理由见 strokeCircle 上的血案注释）+ 距离场抗锯齿。
 *  椭圆没有"逐像素精确距离"的闭式解，这里用**归一化距离的梯度**近似：
 *     f = sqrt((ox/rx)^2 + (oy/ry)^2)   （=1 是边界）
 *     |grad f| = sqrt((ox/rx^2)^2 + (oy/ry^2)^2)
 *     到边界的像素距离 ≈ (1 - f) / |grad f|   ⇒ 外缘覆盖率 = 距离 + 0.5 */
void Canvas::strokeEllipse(int cx, int cy, int rx, int ry, int t, Color c) {
  if (rx <= 0 || ry <= 0 || t <= 0 || !buf_) return;
  const float rxf = (float)rx, ryf = (float)ry;
  const float inf = (float)(rx - t);            // 内缘（按 x 半径算比值）
  const float cxf = (float)cx + 0.5f, cyf = (float)cy + 0.5f;
  /* ★★ 2026-09-15 修一个**自己引入的回归**：环被画成了**实心椭圆**。
   *
   * 错在哪（上一轮加 AA 时写的版本）：
   *   float cov = (1-f)/gl + 0.5;  if (cov > 1) cov = 1;      ← 先 clamp
   *   float ci  = 0.5 - (f - inf/rxf)/gl;  if (ci < cov) ...
   * 椭圆**内部深处** gl（梯度模）→ 0，于是 (1-f)/gl 爆成几万 ⇒ cov 被 clamp 成 1（填实）；
   * 而内边界那项符号写反了（洞中心方向算出的是**正**距离 ⇒ 永远 "ci > cov"，起不到约束）。
   * ⇒ 打地鼠的命中环变成了一个盖住半个屏幕的实心亮绿椭圆（29743 像素）。
   *
   * 正确做法：**两个边界各算一个"到边界的像素距离"，取较小的覆盖率，最后才 clamp**。
   *   到外边界的有符号距离 = (f - 1) / |∇f|      （正 = 在外侧）
   *   到内边界的有符号距离 = (inf/rxf - f) / |∇f|（正 = 在洞里）
   * 注意内边界那项是 **(inf_n - f)** 而不是 (f - inf_n) —— 差一个负号就完全失效。
   * （圆的版本 strokeCircle 用的是真实距离，没有这个问题，可对照。） */
  for (int y = cy - ry - 1; y <= cy + ry + 1; ++y) {
    float oy = (float)y + 0.5f - cyf;
    for (int x = cx - rx - 1; x <= cx + rx + 1; ++x) {
      float ox = (float)x + 0.5f - cxf;
      float nx = ox / rxf, ny = oy / ryf;
      float f = sqrtf(nx * nx + ny * ny);
      if (f <= 0.0001f) {
        // 椭圆正中心：在洞里（inf>0）⇒ 不画；实心（inf<=0）⇒ 填
        putAA(this, x, y, c, inf > 0.0f ? 0.0f : 1.0f);
        continue;
      }
      float gx = ox / (rxf * rxf), gy = oy / (ryf * ryf);
      float gl = sqrtf(gx * gx + gy * gy);
      if (gl <= 0.0001f) continue;
      float cov = 0.5f - (f - 1.0f) / gl;              // 外边界：内侧为大正数
      if (inf > 0.0f) {
        float ci = 0.5f - (inf / rxf - f) / gl;        // 内边界：洞中心方向为大负数
        if (ci < cov) cov = ci;
      }
      if (cov > 1.0f) cov = 1.0f;                      // ★ clamp 放在取 min **之后**
      putAA(this, x, y, c, cov);
    }
  }
}

void Canvas::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                          Color c) {
  int minY = y0 < y1 ? y0 : y1;
  if (y2 < minY) minY = y2;
  int maxY = y0 > y1 ? y0 : y1;
  if (y2 > maxY) maxY = y2;
  if (minY < 0) minY = 0;
  if (maxY >= h_) maxY = h_ - 1;
  /* ★ 抗锯齿：交点保持**浮点**，逐像素按"水平覆盖率"混合。
   *   slanted 边的台阶就来自"交点取整" ⇒ 用浮点区间与像素区间求重叠即可消掉。 */
  const int ax[3] = {x0, x1, x2};
  const int ay[3] = {y0, y1, y2};
  for (int y = minY; y <= maxY; ++y) {
    float xs[3];
    int n = 0;
    float fy = (float)y + 0.5f;              // 该行中心线
    for (int e = 0; e < 3; ++e) {
      int i0 = e, i1 = (e + 1) % 3;
      int ya = ay[i0], yb = ay[i1];
      if (ya == yb) continue;
      if ((fy >= (float)ya && fy < (float)yb) ||
          (fy >= (float)yb && fy < (float)ya)) {
        float t = (fy - (float)ya) / (float)(yb - ya);
        xs[n++] = (float)ax[i0] + t * (float)(ax[i1] - ax[i0]);
      }
    }
    if (n < 2) continue;
    float lo = xs[0], hi = xs[0];
    for (int i = 1; i < n; ++i) {
      if (xs[i] < lo) lo = xs[i];
      if (xs[i] > hi) hi = xs[i];
    }
    int xl = (int)floorf(lo), xr = (int)floorf(hi);
    for (int x = xl; x <= xr; ++x) {
      float left = (float)x > lo ? (float)x : lo;        // 覆盖区间 = [max(x,lo), min(x+1,hi))
      float right = (float)(x + 1) < hi ? (float)(x + 1) : hi;
      putAA(this, x, y, c, right - left);
    }
  }
}

void Canvas::drawSpriteA(const Sprite &sp, int x, int y, int alpha) {
  if (!buf_ || !sp.px || sp.w <= 0 || sp.h <= 0 || alpha <= 0) return;
  // 越界裁剪（只算需要的那几行/列，别做全图循环）
  int sx0 = x < 0 ? -x : 0;
  int sy0 = y < 0 ? -y : 0;
  int sx1 = (x + sp.w > w_) ? (w_ - x) : sp.w;
  int sy1 = (y + sp.h > h_) ? (h_ - y) : sp.h;
  if (sx1 <= sx0 || sy1 <= sy0) return;
  const bool fade = (alpha < 255);
  // stride == 0 ⇒ 行间距就是宽度（编进 .so 的常量表都是紧凑的；运行时解码的图
  // 可能因硬件对齐而更宽，见 Sprite::stride 的说明）
  const int step = sp.stride > 0 ? sp.stride : sp.w;

  /* ★ 快路径：**整图不透明 + 不做整体淡出** ⇒ 只做搬运，不算 alpha、不混合。
   * 收益很大：整屏底图（480x540）每帧重贴，慢路径是 259200 次"取像素/判 alpha/写"，
   * 快路径是 memcpy（编译器会向量化）。打地鼠实测把帧率从 49 拉到 60 靠的就是它。
   * 逐行 memcpy 而不是一次 memcpy：目标缓冲的行距是画布宽度 w_，与图宽一般不等。 */
  if (sp.opaque && !fade) {
    const int bytes = (size_t)(sx1 - sx0) * 4;
    for (int sy = sy0; sy < sy1; ++sy) {
      const uint32_t *row = sp.px + (size_t)sy * (size_t)step;
      memcpy(buf_ + (size_t)(y + sy) * (size_t)w_ + (x + sx0), row + sx0, bytes);
    }
    return;
  }

  for (int sy = sy0; sy < sy1; ++sy) {
    const uint32_t *row = sp.px + (size_t)sy * (size_t)step;
    uint32_t *dst = buf_ + (size_t)(y + sy) * (size_t)w_;
    for (int sx = sx0; sx < sx1; ++sx) {
      const uint32_t c = row[sx];
      int a = (int)((c >> 24) & 0xFF);
      if (!a) continue;                       // 全透明像素：跳过（sprite 大部分是空的）
      if (fade) a = a * alpha / 255;
      if (a >= 255) {
        dst[x + sx] = c;                      // 不透明：直接写（快路径）
      } else {
        blendPx(x + sx, y + sy, c, a);
      }
    }
  }
}

void Canvas::drawSprite(const Sprite &sp, int x, int y) {
  drawSpriteA(sp, x, y, 255);
}

void Canvas::blendRect(int x, int y, int w, int h, Color c, int alpha) {
  if (!buf_ || w <= 0 || h <= 0 || alpha <= 0) return;
  if (alpha >= 255) {
    fillRect(x, y, w, h, c);
    return;
  }
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w;
  int y1 = y + h;
  if (x1 > w_) x1 = w_;
  if (y1 > h_) y1 = h_;
  int ia = 255 - alpha;
  int sr = colorR(c), sg = colorG(c), sb = colorB(c);

  /* ★★ 用**查表**代替逐像素整数除法（2026-09-15 实测优化，收益很大）。
   *
   * 为什么：Cortex-A7 **没有硬件整数除法**，`x / 255` 是一次软件子程序调用
   * （几十个周期）。全屏遮罩 = 259200 像素 × 3 次除法 ⇒ 实测**单帧 8.5ms**，
   * 足以把所有游戏从 60fps 拖到 44fps（READY / OVER / PAUSED 遮罩都走这里，
   * 见 docs/game-art-pipeline.md §9 —— 这是当时用 QA `bench` 挖出来的）。
   *
   * 为什么可以查表：源色是**常量**（同一个 c 和 alpha），混合结果只取决于目标分量
   * （0..255 共 256 种）。花 768 次除法预建 3 张 256 项表，循环里就只剩
   * 移位 + 查表 + 组装 —— 每像素除法次数从 3 降到 0。 */
  uint8_t tr[256], tg[256], tb[256];
  for (int i = 0; i < 256; ++i) {
    tr[i] = (uint8_t)((sr * alpha + i * ia) / 255);
    tg[i] = (uint8_t)((sg * alpha + i * ia) / 255);
    tb[i] = (uint8_t)((sb * alpha + i * ia) / 255);
  }
  for (int yy = y0; yy < y1; ++yy) {
    uint32_t *row = buf_ + (size_t)yy * w_ + x0;
    const int n = x1 - x0;
    for (int i = 0; i < n; ++i) {
      const uint32_t d = row[i];
      row[i] = 0xFF000000u | ((uint32_t)tr[(d >> 16) & 0xFF] << 16) |
               ((uint32_t)tg[(d >> 8) & 0xFF] << 8) | (uint32_t)tb[d & 0xFF];
    }
  }
}

// ---------------- 文本 ----------------

namespace {

// UTF-8 解码：返回码点，*len 写出消耗字节数（非法字节按 1 字节跳过）
uint32_t utf8Next(const char *s, int *len) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char c = p[0];
  if (c < 0x80) {
    *len = 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    *len = 2;
    return ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
  }
  if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    *len = 3;
    return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
           (uint32_t)(p[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
      (p[3] & 0xC0) == 0x80) {
    *len = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
  }
  *len = 1;
  return 0;
}

/* ---------------- 字形表访问（PgFontData.h：原生多档 + 4bit 覆盖率） ----------------
 * ★ 2026-09-15 第二版的关键约定（改这里之前先读）：
 *   · 档位 n（1..5）= 调用点原来的 scale 实参，现在**就是一个像素档**
 *     （字格 = 基准 × n），绘制 1:1 —— 没有任何放大/插值，
 *     所以不存在"放大成 scale×scale 色块"的台阶（那是第一版的病根）。
 *   · 覆盖率 4bit/像素，边缘像素走 blendPx 混合 ⇒ 抗锯齿。
 *   · 某档没收这个字时**跨档回退**：用别的档 1:1 画在本档字格里（居中、不缩放），
 *     推进宽度仍按本档算 —— 宁可字号不对，也不要空白或拉伸。
 */
const FontSet *setFor(int n, bool ascii) {
  static const FontSet kEmpty = {0, 0, 0, 0, 0, 0};
  if (n < 1 || n > PG_FONT_MAX_N) return &kEmpty;
  return ascii ? &ASCII_FONTS[n] : &CJK_FONTS[n];
}

/** 档位夹取：调用点传 0/负数或超过上限时收敛到合法档（生成器只烘到 MAX_N 档） */
inline int clampN(int n) {
  if (n < 1) return 1;
  return (n > PG_FONT_MAX_N) ? PG_FONT_MAX_N : n;
}

inline int clampBigN(int n) {
  if (n < 1) return 1;
  return (n > PG_FONT_BIG_MAX_N) ? PG_FONT_BIG_MAX_N : n;
}

/** 在字集里按码点查找：先二分（要求 codes 升序），失败再线性兜底。
 *
 * ★★ 为什么必须有"线性兜底"（2026-09-15 血案）：
 *   二分查找**只在 codes 升序时才正确**，而生成器一度**没排序**就输出 BIG 档的码点表
 *   （按 `chars=" 0123456789.:-+"` 的原字符串顺序）⇒ 数组长这样：
 *     [0x20, 0x30..0x39, 0x2E, 0x3A, 0x2D, 0x2B]     ← '.'/':'/'-'/'+' 排在 '9' 之后
 *   于是 `7` `8` `9` `.` `:` `-` `+` 这 7 个字符**二分永远查不到** ⇒ 走
 *   `drawBigGlyph` 的\"未收录回退\"，改用 **ASCII 档**画 ⇒ 同一个画面里数字大小差一倍
 *   （用户报障：数字华容道\"字体有大有小\"；实测 BIG@3 墨迹 32x50 vs 回退后 16x25）。
 *
 * 三条纪律：
 *   ① 生成器负责让 codes **升序**（`tools/genfont.py` 已排序）；
 *   ② 这里保留兜底 —— 数据错了也不会静默画错，只是慢一点（异常路径才有开销）；
 *      正常（升序）路径命中二分，**零额外开销**；
 *   ③ 二分"命中就返回"意味着返回的索引**一定正确**，不会因非升序而返回错字形；
 *      非升序只会\"漏\"，所以线性扫一遍就能补齐。 */
int findGlyph(const FontSet *fs, uint32_t cp) {
  if (!fs || fs->count <= 0) return -1;
  int lo = 0, hi = fs->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t c = fs->codes[mid];
    if (c == cp) return mid;
    if (c < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  // 兜底：codes 非升序时会走到这里（也包含"确实没收录"的常见情形，代价可忽略）
  for (int i = 0; i < fs->count; ++i) {
    if (fs->codes[i] == cp) return i;
  }
  return -1;
}

/** 取点阵里第 (gx,gy) 个像素的覆盖率（0..255） */
inline int covAt(const uint8_t *row, int gx) {
#if PG_FONT_COV_BITS == 4
  return (gx & 1) ? (row[gx >> 1] & 0x0F) * 17 : (row[gx >> 1] >> 4) * 17;
#elif PG_FONT_COV_BITS == 8
  return row[gx];
#else
  // 通用回退（非 4/8 位时按位段取）
  int bits = PG_FONT_COV_BITS;
  int per = 8 / bits;
  int idx = gx / per, off = gx % per;
  int shift = 8 - bits * (off + 1);
  int q = (row[idx] >> shift) & ((1 << bits) - 1);
  return q * 255 / ((1 << bits) - 1);
#endif
}

/** 把一个字形（1:1，不缩放）画到 (x,y) */
void blitGlyph(Canvas *cv, int x, int y, const FontSet *fs, int idx, Color c) {
  const int perRow = (fs->cellW * PG_FONT_COV_BITS + 7) / 8;
  const uint8_t *g = fs->masks + (size_t)idx * fs->cellH * perRow;
  for (int gy = 0; gy < fs->cellH; ++gy) {
    const uint8_t *row = g + (size_t)gy * perRow;
    for (int gx = 0; gx < fs->cellW; ++gx) {
      int a = covAt(row, gx);
      if (!a) continue;
      if (a >= 255) cv->px(x + gx, y + gy, c);
      else cv->blendPx(x + gx, y + gy, c, a);
    }
  }
}

/** 该码点在"某一档"是否可画；能画就画并返回 true（供跨档回退用） */
bool tryDrawAt(Canvas *cv, uint32_t cp, bool ascii, int wantN, int x, int y, Color c) {
  const FontSet *fs = setFor(wantN, ascii);
  int idx = findGlyph(fs, cp);
  if (idx < 0) return false;
  blitGlyph(cv, x, y, fs, idx, c);
  return true;
}

/* ---------------- 缺字统计（定义在文件末尾，见 PgCanvas.h 的声明） ----------------
 * 为什么要有：字库是"按档位收字的子集"，没收到的字过去是**静默**的
 * （画个方框或者消失），验收时很容易漏。计数 + 记录码点后，QA 一句 `glyphmiss`
 * 就能确认"该画的字全都在字库里"。 */
int gSizeFallback = 0;
int gMissing = 0;
uint32_t gMissingCps[16] = {0};
int gMissingN = 0;

void noteMissing(uint32_t cp) {
  ++gMissing;
  if (gMissingN < 16) {
    for (int i = 0; i < gMissingN; ++i) {
      if (gMissingCps[i] == cp) return;      // 同一个字只记一次
    }
    gMissingCps[gMissingN++] = cp;
  }
}

}  // namespace

int Canvas::glyphW(uint32_t cp, int scale) const {
  if (cp == '\n') return 0;
  const int n = clampN(scale);
  const FontSet *fs = setFor(n, cp < 0x80);
  if (fs->cellW > 0) return fs->cellW;
  return (cp < 0x80) ? 8 * n : 16 * n;   // 该档没烘（理论上不会）：退回基准宽度
}

int Canvas::drawGlyph(int x, int y, uint32_t cp, int scale, Color c) {
  const int n = clampN(scale);
  const bool ascii = (cp < 0x80);
  const FontSet *fs = setFor(n, ascii);
  if (!fs->cellW) return 16 * n;
  if (tryDrawAt(this, cp, ascii, n, x, y, c)) return fs->cellW;

  // 本档没收这个字：向两侧由近及远找有它的档，**1:1 画在本档字格正中**
  //（不缩放 —— 拉伸出来的字形正是这次要消灭的东西）
  for (int d = 1; d <= PG_FONT_MAX_N; ++d) {
    int m = n - d;
    if (m >= 1 && tryDrawAt(this, cp, ascii, m, x + (fs->cellW - setFor(m, ascii)->cellW) / 2,
                            y + (fs->cellH - setFor(m, ascii)->cellH) / 2, c)) {
      ++gSizeFallback;
      return fs->cellW;
    }
    m = n + d;
    if (m <= PG_FONT_MAX_N &&
        tryDrawAt(this, cp, ascii, m, x + (fs->cellW - setFor(m, ascii)->cellW) / 2,
                  y + (fs->cellH - setFor(m, ascii)->cellH) / 2, c)) {
      ++gSizeFallback;
      return fs->cellW;
    }
  }
  // 全档都没有：画一个空心方块占位（宽度稳定，且一眼能看出缺字）
  if (fs->cellW > 4 * n) {
    strokeRect(x + n, y + n, fs->cellW - 2 * n, fs->cellH - 2 * n, n, c);
  }
  noteMissing(cp);
  return fs->cellW;
}

int Canvas::textW(const char *s, int scale) const {
  if (!s) return 0;
  int total = 0;
  int cur = 0;
  const char *p = s;
  while (*p) {
    if (*p == '\n') {
      if (cur > total) total = cur;
      cur = 0;
      ++p;
      continue;
    }
    int len = 1;
    uint32_t cp = utf8Next(p, &len);
    p += len;
    cur += glyphW(cp, scale);
  }
  if (cur > total) total = cur;
  return total;
}

// 墨迹高（居中排版用）：取 ASCII 档的实测量（与第一版的 FONT_INK_H*scale 口径一致，
// 但现在是**量出来的**，不再是写死的 10×N）。CJK 的墨迹更高，但调用点一直按 ASCII
// 口径居中，保持同口径 ⇒ 排版位置不变。
int Canvas::textH(int scale) const {
  const FontSet *fs = setFor(clampN(scale), true);
  return fs->inkH > 0 ? fs->inkH : 10 * clampN(scale);
}

int Canvas::textLineH(int scale) const {
  const int n = clampN(scale);
  const FontSet *fs = setFor(n, true);
  return (fs->cellH > 0 ? fs->cellH : 12 * n) + 4 * n;
}

void Canvas::text(int x, int y, const char *s, int scale, Color c) {
  if (!s || !buf_ || scale <= 0) return;
  int cx = x;
  const char *p = s;
  while (*p) {
    if (*p == '\n') {
      cx = x;
      y += textLineH(scale);
      ++p;
      continue;
    }
    int len = 1;
    uint32_t cp = utf8Next(p, &len);
    p += len;
    cx += drawGlyph(cx, y, cp, scale, c);
  }
}

void Canvas::textCenter(int cx, int y, const char *s, int scale, Color c) {
  text(cx - textW(s, scale) / 2, y, s, scale, c);
}

void Canvas::textCenterBox(int x, int y, int w, int h, const char *s, int scale,
                           Color c) {
  int tw = textW(s, scale);
  int th = (strchr(s, '\n') ? 2 : 1) * textH(scale);
  text(x + (w - tw) / 2, y + (h - th) / 2, s, scale, c);
}

void Canvas::number(int x, int y, int value, int scale, Color c) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  text(x, y, buf, scale, c);
}

int Canvas::numberW(int value, int scale) const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  return textW(buf, scale);
}

void Canvas::numberCenter(int cx, int y, int value, int scale, Color c) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  textCenter(cx, y, buf, scale, c);
}

// ---------------------------------------------------------------- 大号数字

int Canvas::bigGlyphW(uint32_t cp, int scale) const {
  if (cp == '\n') return 0;
  // 大号是等宽字格（中文也只占一格，见 drawBigGlyph）
  const FontSet *fs = &BIG_FONTS[clampBigN(scale)];
  return fs->cellW > 0 ? fs->cellW : 16 * clampBigN(scale);
}

int Canvas::drawBigGlyph(int x, int y, uint32_t cp, int scale, Color c) {
  const int n = clampBigN(scale);
  const FontSet *fs = &BIG_FONTS[n];
  int idx = findGlyph(fs, cp);
  if (idx < 0) {
    // 未收录（字母、中文…）：回退到原字形，但**按大号字格居中**。
    // ⚠️ 占位宽度仍返回大号字格宽，否则 bigTextW 与实际绘制不一致，
    //    居中/换行会整体偏掉。
    const bool ascii = (cp < 0x80);
    const FontSet *as = setFor(n, ascii);
    drawGlyph(x + (fs->cellW - as->cellW) / 2, y + (fs->cellH - as->cellH) / 2,
              cp, n, c);
    return fs->cellW;
  }
  blitGlyph(this, x, y, fs, idx, c);
  return fs->cellW;
}

int Canvas::bigTextW(const char *s, int scale) const {
  if (!s) return 0;
  int total = 0, cur = 0;
  const char *p = s;
  while (*p) {
    if (*p == '\n') {
      if (cur > total) total = cur;
      cur = 0;
      ++p;
      continue;
    }
    int len = 1;
    uint32_t cp = utf8Next(p, &len);
    p += len;
    cur += bigGlyphW(cp, scale);
  }
  if (cur > total) total = cur;
  return total;
}

// 绘制时真正占用的垂直空间是**整个字格**（数字墨迹在格内居中，上下留白也是它的地盘）
// —— fitBigText / bigTextCenterBox 必须用这个，用墨迹高会让字形顶到框边。
int Canvas::bigTextH(int scale) const { return BIG_FONTS[clampBigN(scale)].cellH; }

int Canvas::bigTextInkH(int scale) const { return BIG_FONTS[clampBigN(scale)].inkH; }

int Canvas::bigTextLineH(int scale) const {
  return BIG_FONTS[clampBigN(scale)].cellH + 4 * clampBigN(scale);
}

void Canvas::bigText(int x, int y, const char *s, int scale, Color c) {
  if (!s || !buf_ || scale <= 0) return;
  int cx = x;
  const char *p = s;
  while (*p) {
    if (*p == '\n') {
      cx = x;
      y += bigTextLineH(scale);
      ++p;
      continue;
    }
    int len = 1;
    uint32_t cp = utf8Next(p, &len);
    p += len;
    cx += drawBigGlyph(cx, y, cp, scale, c);
  }
}

void Canvas::bigTextCenter(int cx, int y, const char *s, int scale, Color c) {
  bigText(cx - bigTextW(s, scale) / 2, y, s, scale, c);
}

void Canvas::bigTextCenterBox(int x, int y, int w, int h, const char *s,
                              int scale, Color c) {
  int tw = bigTextW(s, scale);
  int th = bigTextH(scale);
  bigText(x + (w - tw) / 2, y + (h - th) / 2, s, scale, c);
}

void Canvas::bigNumber(int x, int y, int value, int scale, Color c) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  bigText(x, y, buf, scale, c);
}

int Canvas::bigNumberW(int value, int scale) const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  return bigTextW(buf, scale);
}

void Canvas::bigNumberCenter(int cx, int y, int value, int scale, Color c) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  bigTextCenter(cx, y, buf, scale, c);
}

int Canvas::fitBigText(int w, int h, const char *s, int maxScale) const {
  if (!s || maxScale < 1) return 1;
  // 档位上限由字库决定（PG_FONT_BIG_MAX_N）；超过就是"放大 = 拉伸"，不允许
  if (maxScale > PG_FONT_BIG_MAX_N) maxScale = PG_FONT_BIG_MAX_N;
  for (int sc = maxScale; sc > 1; --sc) {
    if (bigTextW(s, sc) <= w && bigTextH(sc) <= h) return sc;
  }
  return 1;
}

int Canvas::fitBigNumber(int w, int h, int value, int maxScale) const {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", value);
  return fitBigText(w, h, buf, maxScale);
}

// ---- 缺字统计（QA 用；见 PgCanvas.h 的说明）----
int Canvas::sizeFallbackCount() { return gSizeFallback; }
int Canvas::missingGlyphCount() { return gMissing; }
uint32_t Canvas::missingGlyphAt(int i) {
  if (i < 0 || i >= gMissingN) return 0;
  return gMissingCps[i];
}
void Canvas::resetGlyphStats() {
  gSizeFallback = 0;
  gMissing = 0;
  gMissingN = 0;
  for (int i = 0; i < 16; ++i) gMissingCps[i] = 0;
}

}  // namespace pg
