/*
 * PgHaApp.cpp - 智能家居（卡片元信息）
 *
 * ★ 2026-09-23 新增。需求原文：「在屏幕产品中新增一个智能家居功能，接入 Home Assistant（HA），
 *   实现通过屏幕查看并控制 HA 中的面板与设备，最终把这个产品变成一个 HA 遥控器」。
 *
 * ⚠️ 界面在**独立 ftu**：ui/ha.html -> ha.ftu -> haActivity，
 *    逻辑在 src/logic/haLogic.cc，网络层在 src/platform/PgHa.{h,cpp}
 *    （配置 /data/ha.conf；QA 通道 /tmp/pg_hacmd）。
 *    本文件只提供主界面卡片要的 title/desc/tag/theme —— 与 AppSettings 同一套做法。
 *
 * slot = 37（**追加在 kAppTable 表尾**，绝不插中间 —— 存档按 slot 索引）。
 * 规划与真机实测：docs/ha-integration-plan.md（§11）、docs/ha-probe.md、docs/ha-verify.md。
 */
#include "PgGames.h"

namespace pg {

AppHa::AppHa() {}

const char *AppHa::title() const { return "智能家居"; }
const char *AppHa::desc() const { return "Home Assistant · 设备状态与控制"; }
const char *AppHa::tag() const { return "HA"; }
Color AppHa::theme() const { return rgba(29, 158, 117); }   // 与 app_icon_37 的 #1D9E75 同色

}  // namespace pg
