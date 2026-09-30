/*
 * PgSprite.h - 游戏贴图仓库：**运行时从 PNG 加载 + 缓存**（2026-09-15 加）
 *
 * 为什么要有它（用户 2026-09-15 定）：
 *   之前游戏画面全靠几何原语程序化绘制（打地鼠的草地就是 ~60 行 fillRect/fillEllipse），
 *   两个问题：① **开发效率低** —— 每加一个元素就要写一段绘图代码 + 调色 + 调位置；
 *   ② **效果上限低** —— 木纹/毛绒/手绘笔触这些几何画不出来。
 *   ⇒ 静态元素（地面、洞口、地鼠、图标…）改用**PNG 素材**，改图只换文件、
 *     不重编不刷机；只有**动态部分**（位移、弹出、淡出、命中闪光）才自己绘制。
 *
 * ★ 与"编进 .so 的常量表"（打地鼠最初的 PgWhackArt.h 那套，**已废弃删除**）的区别：
 *     常量表    改图 = 重跑生成器 + 重编 + 重新刷机；**不占堆**，但 .so 变大
 *               （实测：三帧 120x120 的锤子就是 580KB 的 C 数组）
 *     本仓库    改图 = 覆盖 PNG + `fun launch` 推资源；**占堆**（≈ w*h*4 字节/张），
 *               所以退出游戏要 clear() 把内存还回去（本板只有 56MB 内存，
 *               720p 硬解就是被内存挤死的 —— 见 docs/online-media.md）
 *   两种方式**共用 Canvas::Sprite**，游戏侧写法一样，可以按"改图频率 vs 内存压力"挑。
 *
 * ★ 三条纪律（与"所有东西不能做拉伸"一致，别破坏）：
 *   ① **1:1**：图尺寸 == 屏幕上的绘制尺寸。要别的尺寸就多烘一张，绝不在运行时缩放
 *      （缩放 = 重采样 = 边缘发糊，用户明确禁止）。
 *   ② 素材是**带透明背景**的 PNG（逐像素 α 混合），不是烘底图。
 *   ③ **失败只试一次**：解不开的路径记成坏路径，不再重试 —— 否则每帧重试会
 *      把帧率直接吃掉（QA `spr` 可以看失败计数）。
 */
#ifndef PG_SPRITE_H_
#define PG_SPRITE_H_

#include "core/PgCanvas.h"

namespace pg {

/*
 * 贴图加载器：把一张 PNG 文件解成画布能直接 1:1 贴的像素缓冲。
 *
 * 为什么是接口而不是直接调框架 API：**core 层不依赖 easyui**（工程纪律，见 PgGame.h
 * 与 PgDisplay.h）—— 设备侧的实现走框架自带的 `BitmapHelper::loadBitmapFromFile`
 * （见 platform/PgSprite.cpp，它自带 PNG/GIF/WebP/QOI 解码），PC/hosttest 侧不装加载器，
 * `get()` 一律返回 0，**游戏必须能降级**（画个占位几何或跳过，别崩）。
 */
class SpriteLoader {
 public:
  virtual ~SpriteLoader() {}
  /* 成功时填好 out->px/w/h/stride（px 指向**堆**，由 unload 释放）。
   * path 是**资源相对路径**，如 "images/game/whack/mole.png"。 */
  virtual bool load(const char *path, Canvas::Sprite *out) = 0;
  virtual void unload(Canvas::Sprite *sp) = 0;
};

/*
 * 按路径缓存贴图（惰性加载）。
 *
 * 典型用法：
 *   // 画一帧（顶层 render 里）
 *   const Canvas::Sprite *mole = pg::sprites().get("images/game/whack/mole.png");
 *   if (mole) c.drawSprite(*mole, x, y);
 *
 *   // 退出游戏（宿主 exitGameToMenu 里已统一调）→ 把堆还回去
 *   pg::sprites().clear();
 *
 * ⚠️ 返回的指针在 clear() 之后失效；**别跨帧缓存 const Sprite***（每帧 get 一次，
 *    命中缓存只是查一张小表，开销可忽略）。
 */
class SpriteBank {
 public:
  static SpriteBank &instance();

  void setLoader(SpriteLoader *l) { loader_ = l; }
  bool ready() const { return loader_ != 0; }

  /* 取图（首次调用触发解码 + 缓存）。失败返回 0，并记一次失败（不再重试）。 */
  const Canvas::Sprite *get(const char *path);

  /* 释放全部缓存（退出游戏时调）。下次 get 会重新加载。 */
  void clear();

  int count() const { return count_; }        // 已缓存张数
  int bytes() const { return bytes_; }        // 已占堆字节（内存预算，QA 打印）
  int failCount() const { return fail_; }     // 加载失败次数（含"坏路径"去重后的）
  const char *pathAt(int i) const;            // QA 列表用；i 越界返回 0

  // 缓存容量：贴图张数上限（超出后新路径不再加载并计入 fail）
  enum { MAX_SPRITES = 40, MAX_PATH = 104 };

 private:
  SpriteBank() : loader_(0), count_(0), bytes_(0), fail_(0) {}
  struct Entry {
    char path[MAX_PATH];
    Canvas::Sprite sp;
    bool failed;
  };
  Entry e_[MAX_SPRITES];
  SpriteLoader *loader_;
  int count_;
  int bytes_;
  int fail_;
};

inline SpriteBank &sprites() { return SpriteBank::instance(); }

}  // namespace pg

#endif  // PG_SPRITE_H_
