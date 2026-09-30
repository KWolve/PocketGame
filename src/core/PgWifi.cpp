/*
 * PgWifi.cpp - WiFi（卡片元信息）
 *
 * ⚠️ 界面已搬到**独立 ftu**：ui/wifi.html -> wifi.ftu -> wifiActivity，
 *    逻辑在 src/logic/wifiLogic.cc（直接调 zknet 的 NETMANAGER）。
 *    历史上这里还有一份"画布版 WiFi 状态页"，但那套代码**永远不会被执行**
 *    （`startGame()` 见到 id=="wifi" 就 openActivity 并 return），
 *    属于死代码 —— 2026-09-13 清掉，避免以后有人误走画布路径。
 *
 * 这里保留的只是一个"空壳应用"，用途只有一个：主界面列表要显示这张卡片的
 * title/desc/tag/theme（mainLogic 的列表缓存会对每个 slot 调一次 create() 取元信息）。
 */
#include "PgGames.h"

namespace pg {

AppWifi::AppWifi() {}

const char *AppWifi::title() const { return "WiFi"; }
const char *AppWifi::desc() const { return "状态查看与系统设置入口"; }
const char *AppWifi::tag() const { return "WIFI"; }
Color AppWifi::theme() const { return rgba(92, 168, 226); }

}  // namespace pg
