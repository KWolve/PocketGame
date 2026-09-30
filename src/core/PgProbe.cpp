/*
 * PgProbe.cpp - 信号探针（卡片元信息）
 *
 * 见 PgProbe.h 的说明：界面与逻辑都在独立 ftu（ui/probe.html + src/logic/probeLogic.cc），
 * 这里只提供主界面卡片要显示的四个字段。
 */
#include "PgProbe.h"

namespace pg {

const char *AppProbe::title() const { return "信号探针"; }
const char *AppProbe::desc() const { return "扫附近无线信号，找可疑设备"; }
const char *AppProbe::tag() const { return "RF"; }
Color AppProbe::theme() const { return rgba(255, 159, 10); }

}  // namespace pg
