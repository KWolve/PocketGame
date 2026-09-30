#include "core/PgSprite.h"

#include <string.h>

namespace pg {

SpriteBank &SpriteBank::instance() {
  static SpriteBank s;
  return s;
}

const Canvas::Sprite *SpriteBank::get(const char *path) {
  if (!path || !*path) return 0;
  if (strlen(path) >= MAX_PATH) {   // 超长路径：记失败，别截断（截断会张冠李戴）
    ++fail_;
    return 0;
  }

  // 先查缓存（含"已知坏路径"，避免每帧重试把帧率吃掉）
  for (int i = 0; i < count_; ++i) {
    if (strcmp(e_[i].path, path) == 0) {
      return e_[i].failed ? 0 : &e_[i].sp;
    }
  }
  if (!loader_) return 0;           // 没装加载器（PC/hosttest）：静默失败，游戏降级
  if (count_ >= MAX_SPRITES) {      // 表满：同样记失败（说明素材条数超预算了）
    ++fail_;
    return 0;
  }

  Entry &slot = e_[count_];
  strncpy(slot.path, path, MAX_PATH - 1);
  slot.path[MAX_PATH - 1] = 0;
  memset(&slot.sp, 0, sizeof(slot.sp));
  slot.failed = !loader_->load(path, &slot.sp);
  ++count_;
  if (slot.failed) {
    ++fail_;
    return 0;
  }
  /* 算一次"整图不透明"标志（贴图快路径的开关，见 Canvas::Sprite::opaque）。
   * 只做一次、只在加载时做 —— 每帧扫一遍就等于把快路径的收益又吐回去了。 */
  {
    const int n = slot.sp.w * slot.sp.h;
    const uint32_t *p = slot.sp.px;
    slot.sp.opaque = 1;
    for (int i = 0; i < n; ++i) {
      if ((p[i] >> 24) != 0xFFu) {
        slot.sp.opaque = 0;
        break;
      }
    }
  }
  bytes_ += slot.sp.w * slot.sp.h * 4;
  return &slot.sp;
}

void SpriteBank::clear() {
  if (loader_) {
    for (int i = 0; i < count_; ++i) {
      if (!e_[i].failed) loader_->unload(&e_[i].sp);
    }
  }
  count_ = 0;
  bytes_ = 0;
  fail_ = 0;
}

const char *SpriteBank::pathAt(int i) const {
  if (i < 0 || i >= count_) return 0;
  return e_[i].path;
}

}  // namespace pg
