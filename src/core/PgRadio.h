/*
 * PgRadio.h - 网络收音机（卡片元信息）
 *
 * ⚠️ 界面已独立成 ftu：ui/radio.html -> radio.ftu -> radioActivity，
 *    逻辑在 src/logic/radioLogic.cc（音频流走 platform/PgStream 的"纯音频"链路）。
 *
 * 这里保留的只是一个"空壳应用"，用途只有一个：主界面列表要显示这张卡片的
 * title/desc/tag/theme（mainLogic 的列表缓存会对每个 slot 调一次 create() 取元信息）。
 * 真正点进卡片时走 openActivity("radioActivity")，本类的 render/onTouch 永不执行。
 */
#ifndef PG_RADIO_H_
#define PG_RADIO_H_

#include "PgGame.h"

namespace pg {

class AppRadio : public Game {
 public:
  AppRadio() {}

  const char *title() const;  // 网络收音机
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

#endif  // PG_RADIO_H_
