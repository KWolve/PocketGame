/*
 * PgDisplay.h - 设备侧显示层：把软件帧缓冲挂到 ZKTextView 上当画布
 *
 * 渲染链路（与 easyui 官方能力一致）：
 *   软渲染写入 Canvas 像素缓冲 -> bitmap_t 直接指向该缓冲（零拷贝）
 *   -> ZKTextView::setBackgroundBmp() 挂画布
 *   -> 每帧 setInvalid(!isInvalid()) 触发刷新（控件->屏幕走硬件加速）
 *
 * !! bitmap 所有权（踩坑换来的规则，别改）!!
 *   `ZKTextView::setBackgroundBmp(bmp)` 之后，**这块 bitmap 就归控件/框架所有**：
 *   框架在替换或清除它时会自己 free(bitmap->data) 以及 free(bitmap)。
 *   实测后果（都表现为"进程莫名其妙退出"，而 /etc/init.rc 里 zkgui 是
 *   `class main` 服务，会被 init 立刻重启，所以现象是"按返回键后整机重开一遍"）：
 *     - detach 时自己再 free(bmp) / free(data) -> 二次释放，直接 segv；
 *     - 不 free 但复用同一块缓冲 -> 写已被框架释放的内存，clear() 处 segv
 *       （pc = pg::Canvas::clear，lr = pg::DeviceDisplay::attach）。
 *   所以这里的策略是：**每次 attach 重新分配 bitmap + 缓冲，交给控件；
 *   detach 只做 setBackgroundBmp(NULL) + abandon()，绝不 free、绝不复用。**
 *
 * 本文件不 include easyui 头文件（用 void* 传控件），因此 PC 上也能编译（tools/hosttest）。
 */
#ifndef PG_DISPLAY_H_
#define PG_DISPLAY_H_

#include "core/PgCanvas.h"

namespace pg {

class DeviceDisplay {
 public:
  DeviceDisplay();
  ~DeviceDisplay();

  // textView: ZKTextView*；返回是否挂载成功
  bool attach(void *textView, int w, int h);
  bool attached() const { return view_ != 0; }

  Canvas &canvas() { return canvas_; }
  const Canvas &canvas() const { return canvas_; }

  // 每帧调用：把像素推上屏
  void present();

  // 解绑控件（不释放任何缓冲，原因见文件头）
  void detach();

 private:
  Canvas canvas_;
  void *view_;
  void *bmp_;   // bitmap_t*，仅作记录；所有权已交给控件，我们不释放
  int frameCount_;
};

}  // namespace pg

#endif  // PG_DISPLAY_H_
