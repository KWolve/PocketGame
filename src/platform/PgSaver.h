#ifndef PLATFORM_PG_SAVER_H_
#define PLATFORM_PG_SAVER_H_
/*
 * PgSaver.h - 屏保的"按键唤醒"收口点（2026-09-14 用户要求：屏保界面任意按键退出屏保）
 *
 * ★ 为什么需要这么个函数，而不是"在屏保页里注册个按键监听"就完事：
 *   框架的按键分发是**短路式**的 —— 依次调用**同一条链上**的 IKeyListener，
 *   任何一个返回 true 就停止往下发（工程里就是靠这个机制做"按下先吞掉"）。
 *   而屏保显示时，下面的 Activity（wifi / 工具页 / IPTV…）只是 hide，**它们的
 *   按键监听器还挂在链上**，并且会 return true 把按键吃掉 ⇒ 屏保页自己注册的监听器
 *   可能根本收不到按键（取决于注册顺序，不确定）。
 *
 *   ⇒ 做法：**每个按键监听器的入口都先问一句**"屏保开着吗？开着就由我把它关掉"。
 *     这样无论框架先叫到谁、无论下面挂了多少个监听器，第一个被叫到的就完成了唤醒，
 *     并且把这次按键吞掉（避免"唤醒的同时顺手执行了该键的动作"，例如在 IPTV 页唤醒
 *     顺便把流停了、或者唤醒了却把音量调了一格）。
 *
 * ⚠️ **但"短路"只在一条链内成立**：屏保是 SysApp，实测同一个按键事件会被投递
 *    **4 次（DOWN×2 + UP×2）** —— SysApp 链与下面的 Activity 链**各有一次**。
 *    ⇒ 本入口里做的动作必须**幂等**（"关掉屏保"天然幂等：第二次调用时
 *      `isScreensaverOn()` 已经是 false，直接返回 false）；而**有状态的切换**
 *      （例如 C 键翻转）必须自己去重，见 `platform/PgFlip.cpp` 的 60ms 去重窗。
 *
 * 用法（各监听器 onKeyEvent 的第一句，**传进本次按键**）：
 *     if (pg::wakeSaverByKey(ke)) return true;      // IKeyListener
 *     if (pg::wakeSaverByKey(ke)) return pg::TOOL_KEY_HANDLED;   // ToolPage
 *
 * ⚠️ 只吞"屏保开着"的那一刻；屏保关掉之后后续的 UP 会正常走各页逻辑，
 *    而各页的 keyUp 都有 `if (code != downCode) return false;` 保护（因为 DOWN 被吞了，
 *    downCode 没更新）⇒ 不会出现"唤醒顺手触发一次动作"。
 *
 * ★★ 2026-09-18 起多了"**例外键**"：C 键（108）在屏保里不是唤醒，而是**整屏翻转 180°**
 *    （用户需求「屏保和机器人界面如果按下 C 按键切换成倒 180 度显示」）。
 *    它必须也在**这个统一入口**里判 —— 理由同上：谁先被框架叫到谁就会把它唤醒。
 *    判据与落盘见 platform/PgFlip.h。
 */
#include "entry/EasyUIContext.h"
#include "utils/Log.h"
#include "platform/PgFlip.h"

namespace pg {

/** 屏保开着 → （C 键例外：只翻转不唤醒）其余按键关掉屏保并返回 true；否则 false（正常处理）。 */
inline bool wakeSaverByKey(const KeyEvent &ke) {
  if (!EASYUICONTEXT->isScreensaverOn()) return false;
  /* C 键 = 翻转 180°（连它的抬起事件一起吞掉，见 PgFlip.cpp 的说明） */
  if (flipHandleSaverKey(ke.mKeyCode, ke.mKeyStatus == KeyEvent::E_KEY_DOWN,
                         ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS)) {
    return true;
  }
  LOGD("屏保: 任意键唤醒 -> 退出屏保（本次按键吞掉，不触发页面动作）");
  EASYUICONTEXT->screensaverOff();
  return true;
}

/** 同上，但给"手里只有键码/没有 KeyEvent"的路径用（`ui/ToolPage.cpp` 的 keyDown/keyUp）。 */
inline bool wakeSaverByCode(int keyCode, bool isDown, bool isLongPress) {
  if (!EASYUICONTEXT->isScreensaverOn()) return false;
  if (flipHandleSaverKey(keyCode, isDown, isLongPress)) return true;
  LOGD("屏保: 任意键唤醒 -> 退出屏保（本次按键吞掉，不触发页面动作）");
  EASYUICONTEXT->screensaverOff();
  return true;
}

}  // namespace pg

#endif  // PLATFORM_PG_SAVER_H_
