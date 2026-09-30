/*
 * PgCanvas.h - 软件帧缓冲 + 绘图原语（不依赖 easyui，可在 PC 上编译做离屏验收）
 *
 * 像素格式：uint32_t，语义 0xAARRGGBB。小端内存里字节序为 B,G,R,A
 * （= 32bpp BMP / Android ARGB8888 的常见内存布局），可直接交给
 * bitmap_t->data 使用（bitmap_t.bits=32, bytes=4, pitch=width*4）。
 */
#ifndef PG_CANVAS_H_
#define PG_CANVAS_H_

#include <stdint.h>
#include <stddef.h>

namespace pg {

/* 画布抗锯齿总开关（2026-09-15 加）。
 * 1 = 开（默认，边缘按覆盖率混合，消除圆/圆角/椭圆的台阶）
 * 0 = 关（退回硬边，用于**性能对比**：软渲染在 480x540 上每帧重绘，AA 的 sqrt
 *     开销需要可量化 —— 把这里改成 0 重编一次即可 A/B）
 * 为什么不写成运行时开关：这是编译期常量，能省掉每像素的分支。 */
#ifndef PG_CANVAS_AA
#define PG_CANVAS_AA 1
#endif

typedef uint32_t Color;

inline Color rgba(int r, int g, int b, int a = 255) {
  return ((uint32_t)(a & 0xFF) << 24) | ((uint32_t)(r & 0xFF) << 16) |
         ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}
// 0xRRGGBB -> 不透明颜色
inline Color rgb(uint32_t hex) {
  return 0xFF000000u | (hex & 0x00FFFFFFu);
}
inline int colorA(Color c) { return (int)((c >> 24) & 0xFF); }
inline int colorR(Color c) { return (int)((c >> 16) & 0xFF); }
inline int colorG(Color c) { return (int)((c >> 8) & 0xFF); }
inline int colorB(Color c) { return (int)(c & 0xFF); }

Color lerpColor(Color a, Color b, int t);  // t: 0..256
Color shade(Color c, int delta);           // delta>0 变亮, <0 变暗

class Canvas {
 public:
  Canvas();
  ~Canvas();

  // 分配像素缓冲（尺寸不变则复用）。w,h 必须是正数。
  // 注意：设备侧把这块缓冲零拷贝挂给控件后，**所有权就归框架了**
  // （框架替换/清除 bitmap 时会 free(data)），所以那之后必须 abandon()，
  // 由 DeviceDisplay 在下次挂载时重新分配。详见 platform/PgDisplay.h。
  bool init(int w, int h);
  void release();   // 真正释放（仅限确认所有权还在自己手里时用）
  void abandon();   // 只忘掉指针，不释放（所有权已交给别人时用）

  int width() const { return w_; }
  int height() const { return h_; }
  size_t pitch() const { return (size_t)w_ * 4; }
  uint32_t *pixels() const { return buf_; }
  bool valid() const { return buf_ != 0; }

  void clear(Color c);
  void px(int x, int y, Color c);
  /**
   * 按覆盖率把颜色混进已有像素（a: 0..255）。**抗锯齿的底层操作**。
   *
   * 为什么需要它：`px/fillRect` 都是**直接覆写**（写什么就是什么），所以圆的斜边、
   * 圆角、椭圆的边界都是"硬边台阶"（用户 2026-09-15 报"游戏素材好多锯齿"）。
   * 抗锯齿的做法就是：只在**边缘那一圈**按覆盖率调用本函数混合，内部仍走快速填充。
   */
  void blendPx(int x, int y, Color c, int a);
  void fillRect(int x, int y, int w, int h, Color c);
  void fillRectRound(int x, int y, int w, int h, int r, Color c);
  void strokeRect(int x, int y, int w, int h, int t, Color c);
  void fillCircle(int cx, int cy, int r, Color c);
  void strokeCircle(int cx, int cy, int r, int t, Color c);
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c);
  void hline(int x0, int x1, int y, Color c);
  void vline(int x, int y0, int y1, Color c);
  // alpha: 0(全透) ~ 255(不透明)
  void blendRect(int x, int y, int w, int h, Color c, int alpha);
  // 以 (x,y) 为圆心画椭圆环（管道/舱体用）
  void fillEllipse(int cx, int cy, int rx, int ry, Color c);
  void strokeEllipse(int cx, int cy, int rx, int ry, int t, Color c);

  /* ---- 贴图（sprite）----
   * 为什么需要：几何原语画不出"木纹 / 金属质感 / 手绘笔触"。打地鼠的锤子
   * （用户 2026-09-15：「锤子样式可以弄几个切图切换动作」）就要它。
   *
   * 三条约定（与"所有东西不能做拉伸"一致，别破坏）：
   *   ① **1:1 贴图：不缩放、不插值**。要多大就离线做多大
   *      （`tools/gen_game_art.py`）；需要别的尺寸就多出一张，绝不在运行时拉。
   *   ② 像素格式与画布一致（0xAARRGGBB），逐像素 alpha 混合；alpha=0 直接跳过。
   *      所以图是**带透明背景**的 PNG 烘成的（不是烘底图）。
   *   ③ 图数据来源有两处，共用本结构：
   *      · **运行时加载的 PNG**（推荐，见 core/PgSprite.h）—— 改图只换文件、
   *        不重编不刷机；代价是占堆（≈ w×h×4/张），退出游戏要 clear()
   *      · **编在 .so 里的 const 常量表**（更早的写法，如打地鼠最初的
   *        `PgWhackArt.h`：三帧锤子 580KB 编进 .so）—— .so 变大、改图要重编，
   *        但完全不占堆。2026-09-15 已全面换成上面的 PNG 方案，那个头文件已删。
   *   ④ `stride` 见字段说明：运行时解码的缓冲可能因**硬件对齐**比宽度宽。
   */
  struct Sprite {
    const uint32_t *px;
    int w;
    int h;
    /* 行间距（像素）。**0 = 等于 w**（紧凑排布）—— 老代码的聚合初始化
     * `{px, w, h}` 依然成立，行为不变。
     * 为什么需要它：运行时从 PNG 解码出来的缓冲受**硬件对齐**约束（V85X 的 G2D
     * 要求 16 字节对齐的 stride），宽度不是 4 的倍数时 pitch != w*4；
     * 这时按 w 取行会**斜切**。见 platform/PgSprite.cpp 的 stride 处理。 */
    int stride;
    /* 1 = **整图没有半透明/全透明像素**（如整屏底图）。贴图时可以走 memcpy 快路径。
     * 0 = 未知/有透明像素 ⇒ 逐像素 alpha 混合（安全路径）。
     * 这个标志由加载侧算一次（SpriteBank::get 里扫一遍），**别每帧扫**。 */
    int opaque;
  };
  /** 把 sprite 贴到 (x,y)（左上角对齐）。越界自动裁剪。 */
  void drawSprite(const Sprite &sp, int x, int y);
  /** 带整体透明度（0..255）的贴图 —— 淡出/残影用。 */
  void drawSpriteA(const Sprite &sp, int x, int y, int alpha);

  // ---- 文本（内嵌原生多档字库，见 PgFontData.h）----
  // ★ 2026-09-15 第二版：`scale`(档位 N) **不再做整数放大** —— 每一档都是
  //   "按目标像素数原生栅格化 + 4bit 灰度覆盖率"的点阵，绘制 1:1 贴像素，
  //   边缘走 blendPx 混合 ⇒ 无拉伸、有抗锯齿。
  //   档位与像素：ASCII 8x12 / 16x24 / 24x36 / 32x48 / 40x60（N=1..5），
  //              中文 16x16 / 32x32 / 48x48 / 64x64 / 80x80（N=1..5）。
  //   调用点的 `scale` 实参含义不变（1..5），所以**排版宽度/高度与第一版一致**。
  //   s 为 UTF-8；某档没收的字会跨档回退（1:1 画、不缩放），不会变成空白或方框。
  int textW(const char *s, int scale) const;
  int textH(int scale) const;      // 字形墨迹高度（该档实测值）
  int textLineH(int scale) const;  // 推荐行高
  void text(int x, int y, const char *s, int scale, Color c);
  void textCenter(int cx, int y, const char *s, int scale, Color c);
  void textCenterBox(int x, int y, int w, int h, const char *s, int scale, Color c);
  void number(int x, int y, int value, int scale, Color c);
  int numberW(int value, int scale) const;
  void numberCenter(int cx, int y, int value, int scale, Color c);

  // ---- 大号数字（原生 16x24/32x48/48x72，同属 PgFontData.h）----
  // 与上面同一套机制：档位 N=1..PG_FONT_BIG_MAX_N 都是原生栅格化 + 抗锯齿，
  // 不放大。换算关系：bigText(scale=2) 的显示尺寸 == text(scale=4)。
  //
  // ⚠️ 字符集只有 0-9 与 . : - + 空格（见 BIG）。其它字符（字母/中文）会回退到
  //    普通字形，但**占位宽度仍按大号字格**，所以居中/换行不会错位。
  int bigTextW(const char *s, int scale) const;
  int bigTextH(int scale) const;      // **字格**高度（绘制占用的垂直空间）
  int bigTextInkH(int scale) const;   // 数字**墨迹**高度（比字格矮，仅供精确定位用）
  int bigTextLineH(int scale) const;  // 推荐行高
  void bigText(int x, int y, const char *s, int scale, Color c);
  void bigTextCenter(int cx, int y, const char *s, int scale, Color c);
  void bigTextCenterBox(int x, int y, int w, int h, const char *s, int scale,
                        Color c);
  void bigNumber(int x, int y, int value, int scale, Color c);
  int bigNumberW(int value, int scale) const;
  void bigNumberCenter(int cx, int y, int value, int scale, Color c);

  // 在 w x h 的框里挑**放得下的最大 scale**（1..maxScale），返回实际用的档位。
  // 用来替掉以前手写的 "scale = 11 / 位数" 这类经验值 —— 位数多、格子小时自动降档。
  int fitBigText(int w, int h, const char *s, int maxScale) const;
  int fitBigNumber(int w, int h, int value, int maxScale) const;

  // ---- 缺字统计（QA 用；缺字是"静默"故障，必须可验收）----
  // 画布字库是**按档位收字的子集**（见 tools/genfont.py）。如果某个字没进字库：
  //   ① 该档没有但有别的档 → 跨档回退（字会偏大/偏小，但不消失）→ sizeFallback
  //   ② 全档都没有 → 画空心方框占位 → missing
  // 正常两者都恒为 0；QA 命令 `glyphmiss` 会打印它们。
  static int sizeFallbackCount();
  static int missingGlyphCount();
  static uint32_t missingGlyphAt(int i);   // 最近 16 个缺失码点（0 = 空）
  static void resetGlyphStats();

 private:
  // 绘制单字形；返回推进宽度
  int drawGlyph(int x, int y, uint32_t cp, int scale, Color c);
  int glyphW(uint32_t cp, int scale) const;
  int drawBigGlyph(int x, int y, uint32_t cp, int scale, Color c);
  int bigGlyphW(uint32_t cp, int scale) const;

  int w_;
  int h_;
  uint32_t *buf_;
};

}  // namespace pg

#endif  // PG_CANVAS_H_
