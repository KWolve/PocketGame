/*
 * PgSpriteDevice.h - 设备侧贴图加载器的安装入口
 *
 * 实现见 platform/PgSprite.cpp（core/PgSprite.h 只放接口与缓存，不依赖 easyui）。
 * 宿主（mainLogic）在 onUI_init 里调一次 installSpriteLoader()，之后
 * `pg::sprites().get("images/xxx.png")` 就能直接用了。
 */
#ifndef PG_SPRITE_DEVICE_H_
#define PG_SPRITE_DEVICE_H_

namespace pg {

/* 把设备侧加载器（BitmapHelper 解 PNG）装进全局 SpriteBank。重复调用无害。 */
void installSpriteLoader();

/* QA 用：解码一张图并打印尺寸 + 前 8 个像素的 ARGB。
 * 为什么需要：解码出来的**字节序 / 是否预乘 alpha** 只能实测 —— 猜错的表现是
 * "颜色整体偏暗"或"红蓝互换"，肉眼看抓屏不一定分得清。`spr <path>` 打出来最直观。 */
void spriteProbe(const char *path);

}  // namespace pg

#endif  // PG_SPRITE_DEVICE_H_
