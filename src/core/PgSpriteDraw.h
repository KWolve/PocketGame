/*
 * PgSpriteDraw.h - 贴图助手：按素材清单（PgGameArt.h）贴到画布上
 *
 * 为什么单独一个头：**每个游戏都要这两句**，写进各自的 .cpp 就会复制粘贴，
 * 而"锚点对齐 + 尺寸自检 + 失败降级"这三件事漏一件都会变成难查的静默故障：
 *   · 忘了减锚点   → 贴歪（多帧动作还会"换帧跳位"）
 *   · 忘了尺寸自检 → 素材换错尺寸只是差几像素，肉眼看不出
 *   · 忘判空指针   → PC/离屏验收（没有解码器）直接崩
 *
 * 用法：
 *   #include "core/PgSpriteDraw.h"
 *   blit(c, gameart::kWhackMole, cx, cy);            // 静态贴
 *   blitA(c, gameart::kWhackRays, cx, cy, alpha);    // 带整体透明度（淡出）
 */
#ifndef PG_SPRITE_DRAW_H_
#define PG_SPRITE_DRAW_H_

#include "core/PgGameArt.h"
#include "core/PgLog.h"
#include "core/PgSprite.h"

namespace pg {

/* 按清单贴图：锚点对齐 + 尺寸自检 + 缺素材时静默跳过。
 * 尺寸不符只报**一次**（同一条路径），否则每帧刷日志比不报还糟。 */
inline void blit(Canvas &c, const gameart::Def &d, int x, int y) {
  const Canvas::Sprite *s = sprites().get(d.path);
  if (!s) return;   // 没素材（PC 端 / 加载失败）：跳过这个元素，别崩
  if (s->w != d.w || s->h != d.h) {
    static const char *warned[16] = {0};
    static int nwarn = 0;
    bool seen = false;
    for (int i = 0; i < nwarn; ++i) {
      if (warned[i] == d.path) seen = true;
    }
    if (!seen && nwarn < 16) {
      warned[nwarn++] = d.path;
      logInfo("PgArt: 素材尺寸与清单不符 %s 实际 %dx%d 清单 %dx%d（改了图要重跑 "
              "tools/gen_game_art.py）", d.path, s->w, s->h, d.w, d.h);
    }
  }
  c.drawSprite(*s, x - d.ax, y - d.ay);
}

/* 带整体透明度（0..255）的贴图 —— 淡出/残影用（连击赞赏就是这么淡出的）。 */
inline void blitA(Canvas &c, const gameart::Def &d, int x, int y, int alpha) {
  if (alpha <= 0) return;
  const Canvas::Sprite *s = sprites().get(d.path);
  if (!s) return;
  c.drawSpriteA(*s, x - d.ax, y - d.ay, alpha);
}

}  // namespace pg

#endif  // PG_SPRITE_DRAW_H_
