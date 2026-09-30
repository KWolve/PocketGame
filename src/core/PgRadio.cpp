/*
 * PgRadio.cpp - 网络收音机（卡片元信息）
 *
 * 见 PgRadio.h：界面与逻辑都在独立 ftu（ui/radio.html + src/logic/radioLogic.cc），
 * 这里只提供主界面卡片要显示的四个字段。
 */
#include "PgRadio.h"

namespace pg {

const char *AppRadio::title() const { return "网络收音机"; }
const char *AppRadio::desc() const { return "在线电台，联网即听"; }
const char *AppRadio::tag() const { return "RADIO"; }
Color AppRadio::theme() const { return rgba(255, 122, 69); }

}  // namespace pg
