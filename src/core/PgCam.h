/*
 * PgCam.h - 局域网摄像头查看（卡片元信息）
 *
 * ⚠️ 界面已独立成 ftu：ui/camera.html -> camera.ftu -> cameraActivity，
 *    逻辑在 src/logic/cameraLogic.cc（扫描复用 platform/PgLan，取流走 platform/PgStream）。
 *
 * 这里只提供主界面卡片要显示的四个字段。
 */
#ifndef PG_CAM_H_
#define PG_CAM_H_

#include "PgGame.h"

namespace pg {

class AppCam : public Game {
 public:
  AppCam() {}

  const char *title() const;  // 摄像头查看
  const char *desc() const;
  const char *tag() const;
  Color theme() const;

  void reset() {}
  void update(int) {}
  void render(Canvas &) {}
  bool onTouch(int, int, int) { return false; }
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return ""; }
};

}  // namespace pg

#endif  // PG_CAM_H_
