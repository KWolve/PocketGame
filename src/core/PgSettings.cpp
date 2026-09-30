/*
 * PgSettings.cpp - 系统设置（卡片元信息）
 *
 * 用户需求（2026-09-16）：「把音效开关，wifi，音量调节，背光调节都做到系统设置里面」。
 *
 * ⚠️ 界面在**独立 ftu**：ui/settings.html -> settings.ftu -> settingsActivity，
 *    逻辑在 src/logic/settings.cc（调 zknet / zkhardware / 本工程的 Host）。
 *    这里只留一个"空壳应用"：主界面列表要对这个 slot 取 title/desc/tag/theme
 *    （与 AppWifi 同一套做法，见 PgWifi.cpp 的说明）。
 *
 * slot = 32（**追加在 kAppTable 表尾**，绝不插中间 —— 存档按 slot 索引）。
 */
#include "PgGames.h"

namespace pg {

AppSettings::AppSettings() {}

const char *AppSettings::title() const { return "系统设置"; }
const char *AppSettings::desc() const { return "音效 / WiFi / 音量 / 背光"; }
const char *AppSettings::tag() const { return "SET"; }
Color AppSettings::theme() const { return rgba(142, 142, 147); }

}  // namespace pg
