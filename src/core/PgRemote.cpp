/*
 * PgRemote.cpp - 蓝牙遥控（卡片元信息）
 *
 * 见 PgRemote.h 的说明：界面与逻辑都在独立 ftu（ui/remote.html + src/logic/remoteLogic.cc），
 * 这里只提供主界面卡片要显示的四个字段。
 */
#include "PgRemote.h"

namespace pg {

const char *AppRemote::title() const { return "蓝牙遥控"; }
const char *AppRemote::desc() const { return "把本机变成 BLE HID 遥控器"; }
const char *AppRemote::tag() const { return "BT"; }
Color AppRemote::theme() const { return rgba(70, 130, 200); }

}  // namespace pg
