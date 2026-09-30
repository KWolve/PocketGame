#include "PgDisplay.h"

#include <stdlib.h>
#include <string.h>

#include "utils/Log.h"

#ifdef FUN_BUILD
#include "control/ZKTextView.h"
#include "utils/BitmapHelper.h"
#endif

namespace pg {

DeviceDisplay::DeviceDisplay() : view_(0), bmp_(0), frameCount_(0) {}

DeviceDisplay::~DeviceDisplay() { detach(); }

bool DeviceDisplay::attach(void *textView, int w, int h) {
  if (!textView) {
    LOGE("PgDisplay: textView is NULL");
    return false;
  }
  detach();  // 若还在挂载状态，先解绑（旧缓冲所有权已归框架）

  // 每次挂载都重新分配：上一块的 bitmap 与数据一旦交给控件，
  // 框架会在 setBackgroundBmp(NULL) 时把它们释放掉（实测），不能复用。
  if (!canvas_.init(w, h)) {
    LOGE("PgDisplay: canvas init failed (%dx%d)", w, h);
    return false;
  }

  view_ = textView;

#ifdef FUN_BUILD
  bitmap_t *b = (bitmap_t *)calloc(1, sizeof(bitmap_t));
  if (!b) {
    LOGE("PgDisplay: bitmap_t alloc failed");
    view_ = 0;
    canvas_.abandon();
    return false;
  }
  // 32bpp ARGB8888：小端内存里字节序 B,G,R,A，与 Canvas 的 uint32 0xAARRGGBB 一致
  b->type = 0;
  b->bits = 32;
  b->bytes = 4;
  b->alpha = 1;
  b->ck = 0;
  b->width = (uint32_t)w;
  b->height = (uint32_t)h;
  b->pitch = (uint32_t)(w * 4);
  b->data = (uint8_t *)canvas_.pixels();  // 零拷贝：直接挂画布缓冲
  b->am = 0;
  b->ap = 0;
  b->phy_addr = 0;
  bmp_ = b;

  ZKTextView *tv = (ZKTextView *)textView;
  tv->setBackgroundBmp(b);  // <= 从此这块内存归控件/框架所有，我们不再释放
  LOGD("PgDisplay: attach canvas %dx%d ok (bmp=%p data=%p)", w, h, (void *)b,
       (void *)b->data);
#else
  LOGD("PgDisplay: host build, no real display attached");
#endif

  canvas_.clear(0xFF000000u);
  present();
  return true;
}

void DeviceDisplay::present() {
  if (!view_) return;
  ++frameCount_;
#ifdef FUN_BUILD
  ZKTextView *tv = (ZKTextView *)view_;
  // 官方做法：交替 setInvalid 触发重绘（渲染层在 draw 时读 bmp->data）
  tv->setInvalid(!tv->isInvalid());
#endif
}

void DeviceDisplay::detach() {
  if (!view_) return;
#ifdef FUN_BUILD
  // 解绑即"归还所有权"：框架在这里释放 bitmap_t 及其 data，
  // 所以之后既不能 free 也不能再用那块缓冲（否则 segv 打死进程）。
  ((ZKTextView *)view_)->setBackgroundBmp(NULL);
#endif
  view_ = 0;
  bmp_ = 0;
  canvas_.abandon();  // 忘掉缓冲，等下次 attach 重新分配
}

}  // namespace pg
