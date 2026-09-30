#ifndef PLATFORM_PG_FLIP_H_
#define PLATFORM_PG_FLIP_H_
/*
 * PgFlip.h - 整屏 180° 翻转（2026-09-18 用户需求）
 *
 * 需求原文：「因为我的屏幕挂绳在底部，屏保和机器人界面如果按下 C 按键切换成倒 180 度显示。」
 * 用户当面选定的三条（2026-09-18）：
 *   ① 生效范围 = **只这两页**（屏保 / 机器人）；其它页面永远正向，手持操作时不别扭。
 *   ② 机器人页里 **短按 C = 翻转**（长按仍是返回列表）；代价只有"按键摸头/按键换表情"，
 *      而这两个功能触摸全都有（点屏幕=摸头、左右滑=换表情、长按屏幕=表情面板）。
 *   ③ 翻转状态**落盘记住**，重启/刷机后仍是上次那面。
 *
 * ---- 事实基础（反汇编 + 真机实测，都不是猜的）----
 *   `ConfigManager::setScreenRotate(int)` 存完静态量后**尾调 `zk_disp_set_rotate`** ⇒ 实时生效、
 *   不用重启；`setTouchRotate(int)` 同理尾调 `zk_event_set_touch_rotate` ⇒ **触摸坐标一起转**。
 *   真机实测：设 180 后抓屏，画面恰好是原来的 180° 旋转（384000 像素 **0 差异**）。
 *   ⚠️ 它是"把渲染结果转过去再写 fb"，所以 `/dev/fb0` 读到的是**转过之后**的画面 ——
 *     QA 的抓屏/比对工具要跟着转 180° 才能用（见 `tools/saver_qa.py --rot180`）。
 *   ⚠️ EasyUI.cfg 里的 `rotateScreen/rotateTouch` 只在**开机**读一次；进程跑起来之后
 *     由本模块托管（两个值永远一起改，别只改一个 —— 只转画面不转触摸 = 点哪儿都不对）。
 *   ⚠️ 视频走的是**另一个 disp 层**（ch0/lyl0，见 docs/disp-layer.md），本翻转不保证它跟着转；
 *     本功能只作用在屏保/机器人这两个纯 UI 页，不涉及视频。
 *
 * ---- 自愈与幂等（本工程的纪律）----
 *   `flipTick()` 每拍可随便调：它按"意愿 + 有没有环境页在显示"算出目标角度，**只在变化时**才
 *   下发一次。所以各页只要老实上报自己"在不在显示"，掉帧/漏报一拍都不会把屏幕留在错的角度。
 */
#include "manager/ConfigManager.h"
#include "utils/Log.h"

namespace pg {

/* 本板三个物理键（`docs/audio-and-input-hardware.md` §2.4；`/proc/bus/input/devices` 能力位实测）。
 * ⚠️ 只在这一个地方定义键码：翻转是**全机**行为，屏保页和主界面必须用同一个真值。 */
enum {
  PG_KEY_CODE_VOL_DOWN = 103,  // KEY_UP    -> 音量 -
  PG_KEY_CODE_VOL_UP = 105,    // KEY_LEFT  -> 音量 +
  PG_KEY_CODE_C = 108,         // KEY_DOWN  -> 物理键"C"（现语义：暂停/互动；本功能：翻转）
};

/** 会"整屏倒过来看"的环境页。谁显示谁负责上报（见 flipSetPageOn）。 */
enum FlipPage {
  FLIP_PAGE_SAVER = 0,  // 屏保（砖块钟）
  FLIP_PAGE_PET = 1,    // 机器人（电子宠物）
  FLIP_PAGE_COUNT = 2,
};

/** 开机调用（Main.cpp 里已调）；重复调用等于什么都没做。 */
void flipInit();

/** 用户意愿：C 键切出来的那个状态（落盘保存，与"当前屏幕角度"是两回事）。 */
bool flipWanted();

/** 切换意愿：落盘 + 立刻按当前页面情况下发。返回切换后的意愿。 */
bool flipToggleWanted();

/** 显式设置意愿（QA `flip on|off`）。 */
void flipSetWanted(bool on);

/** 各页上报"我在不在显示"（幂等，每拍调都行）。 */
void flipSetPageOn(FlipPage page, bool on);
bool flipPageOn(FlipPage page);

/*
 * 抑制器：**必须正着读**的浮层（目前只有"闹钟到点提醒页"）显示时，强制不翻转。
 *
 * 为什么需要它（2026-09-18 实测到的真问题）：闹钟提醒页是 main 里的一个 window，
 * 它可以在**屏保还显示着**的时候弹出来（日志实测 `提醒页=1` + `环境页 saver=1` +
 * `实际 180°`）⇒ 挂着看时钟时闹钟是**倒着**的，用户根本没法读。
 * 所以规则是：**"要人读/要人点"的页面永远正着显示**，环境页（拿来看的）才允许倒。
 */
void flipSetSuppress(bool on);
bool flipSuppressed();

/** 每拍/每帧调用：按"意愿 + 有没有环境页在显示"算出目标角度，**变化才下发**（自愈、幂等）。 */
void flipTick();

/** 当前实际已下发的角度（0 / 180）。 */
int flipDeg();

/*
 * 屏保显示中的按键统一入口（由 `pg::wakeSaverByKey()` 调用）。
 * 返回 true = 这次按键已处理完，调用方应**吞掉**它（不要拿去唤醒屏保）。
 *
 * ★ 为什么放在这里而不是屏保页自己的监听器里：框架的按键分发是**短路式**的，屏保显示时
 *   下面挂着的 wifi/iptv/工具页监听器还在链上，**谁先被叫到谁就把它唤醒了**（取决于注册顺序）
 *   —— 所以"C 键不唤醒、改成翻转"必须在所有监听器共用的那个入口里判（同 PgSaver.h 的理由）。
 */
bool flipHandleSaverKey(int keyCode, bool isDown, bool isLongPress);

}  // namespace pg

#endif  // PLATFORM_PG_FLIP_H_
