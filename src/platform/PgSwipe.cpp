/*
 * PgSwipe.cpp - 「右滑返回」手势检测（见 PgSwipe.h 的说明）
 */
#include "platform/PgSwipe.h"

#include <stdlib.h>
#include <time.h>

namespace pg {

namespace {

/* 阈值（改动前先看 PgSwipe.h 的「指纹」说明：这几个数是按
 * 「不误伤游戏内横向拖拽」与「不误收竖直列表滚动」定的，真机校准过两轮） */
const int kMinDx = 120;   // 水平位移下限（px，屏宽 25%）
const int kEdgeX = 48;    // ★ 必须从屏幕左侧这么多像素内起手（仿 Android 边缘返回）
const int kMaxDy = 70;    // 纵向偏移上限（px）
const int kMaxMs = 900;   // 手势时长上限（ms）

int sStartX = -1;
int sStartY = 0;
int sCurX = -1;
long sStartMs = 0;
bool sActive = false;
bool sTriggered = false;

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

void swipeDown(int x, int y) {
  sStartX = x;
  sStartY = y;
  sCurX = x;
  sStartMs = nowMs();
  sActive = true;
  sTriggered = false;
}

void swipeMove(int x, int y) {
  (void)y;
  if (sActive) sCurX = x;
}

bool swipeUp(int x, int y) {
  if (!sActive) return false;
  const int dx = x - sStartX;
  const int dy = abs(y - sStartY);
  const long dt = nowMs() - sStartMs;
  sCurX = x;
  sActive = false;
  /* ★ 边缘起手 + 距离 + 纵向容差 + 时长，四个条件全满足才算「右滑返回」。
   *   只判距离会把游戏里的横向拖拽一起吃掉（推箱子里 140px 拖拽实测踩过，见 PgSwipe.h）。 */
  sTriggered = (sStartX <= kEdgeX && dx >= kMinDx && dy <= kMaxDy && dt <= kMaxMs);
  /* ★★ 手势结束必须把**跟手位置**复位（2026-09-16 血案）：
   *   原来只清 `sActive`，`sStartX/sCurX` 留着上一次的值 ⇒ `swipeProgress()` 里
   *   `dx = sCurX - sStartX` 仍是被判过的那一整段位移 ⇒ **右滑过哪怕一次，
   *   progress 就永远返回 255**。后果：navibar 的「返回主页」提示（判 `pr > 0` 显示）
   *   **永久挂在状态栏上、还把页面标题顶掉** —— 用户报的
   *   「状态栏上面显示的返回主页去掉」就是它（提示已整组删除，但这个状态泄漏是真 bug，
   *   留着就是给下一个用 `swipeProgress()` 的人埋雷）⇒ 一并修掉，
   *   与"粘性状态用完必须复位"同一条纪律。
   *   ⚠️ 只复位**位置**；`sTriggered` 按设计保留（"UP 之后仍有效"，
   *      由 swipeClearTriggered() 负责清）。 */
  sStartX = -1;
  sCurX = -1;
  return sTriggered;
}

void swipeCancel() {
  sActive = false;
  sStartX = -1;
  sCurX = -1;
}

int swipeProgress() {
  if (sStartX < 0 || sCurX < 0) return 0;
  const int dx = sCurX - sStartX;
  if (dx <= 0) return 0;
  if (dx >= kMinDx) return 255;
  return dx * 255 / kMinDx;
}

bool swipeTriggered() { return sTriggered; }
void swipeClearTriggered() { sTriggered = false; }

/* 见 PgSwipe.h：主界面每帧同步"在不在画布游戏里"，决定右滑该由谁处理。 */
static bool sGameMode = false;
void setGameMode(bool on) { sGameMode = on; }
bool gameMode() { return sGameMode; }

}  // namespace pg
