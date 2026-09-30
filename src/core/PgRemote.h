/*
 * PgRemote.h - 蓝牙遥控（卡片元信息）
 *
 * ⚠️ 界面已迁到**独立 ftu**：ui/remote.html -> remote.ftu -> remoteActivity，
 *    逻辑在 src/logic/remoteLogic.cc（该文件直接调 platform/PgBt 的单例）。
 *
 * 这里保留的只是一个"空壳应用"，用途只有一个：主界面列表要显示这张卡片的
 * title/desc/tag/theme（mainLogic 的列表缓存会对每个 slot 调一次 create() 取元信息）。
 * 真正点进卡片时，mainLogic 会走 openActivity("remoteActivity")，本类的
 * render/onTouch **永远不会被执行**。
 */
#ifndef PG_REMOTE_H_
#define PG_REMOTE_H_

#include "PgGame.h"

namespace pg {

class AppRemote : public Game {
 public:
  AppRemote() {}

  const char *title() const;  // 蓝牙遥控
  const char *desc() const;   // 把本机变成 BLE HID 遥控器
  const char *tag() const;    // 卡片短标签（ASCII）
  Color theme() const;

  // 生命周期（只有 reset 有意义：空壳不持有任何状态）
  void reset() {}
  void update(int) {}
  void render(Canvas &) {}                       // 界面在 remote.ftu 里，不画布
  bool onTouch(int, int, int) { return false; }   // 触摸由原生控件接管
  bool onKey(int) { return false; }

  int score() const { return 0; }
  bool showBest() const { return false; }
  const char *hint() const { return ""; }
  const char *keyBar() const { return ""; }
};

}  // namespace pg

#endif  // PG_REMOTE_H_
