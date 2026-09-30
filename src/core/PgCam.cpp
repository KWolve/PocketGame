/*
 * PgCam.cpp - 局域网摄像头查看（卡片元信息）
 *
 * 见 PgCam.h：界面与逻辑都在独立 ftu（ui/camera.html + src/logic/cameraLogic.cc），
 * 这里只提供主界面卡片要显示的四个字段。
 */
#include "PgCam.h"

namespace pg {

const char *AppCam::title() const { return "摄像头查看"; }
const char *AppCam::desc() const { return "扫局域网 RTSP 摄像头并看画面"; }
const char *AppCam::tag() const { return "CAM"; }
Color AppCam::theme() const { return rgba(50, 173, 230); }

}  // namespace pg
