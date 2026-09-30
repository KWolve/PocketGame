/*
 * PgFlip.cpp - 整屏 180° 翻转（实现；接口与理由见 PgFlip.h）
 */
#include "PgFlip.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace pg {

/* 单调毫秒时钟（与 mainLogic 的 nowMs 同一套；用不到系统时间，不怕校时跳变） */
static long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* 落盘路径：与屏保的"用户亮度凭据"同一套写法（/data 是 jffs2 闪存，重启不丢；
 * 没有 /data 时退回 /tmp，那时只在本次开机有效）。 */
static const char *kFlipPaths[2] = {"/data/pocketgame_uiflip.dat",
                                    "/tmp/pocketgame_uiflip.dat"};

static bool sInited = false;
static bool sWanted = false;                  // 用户意愿（C 键切出来的）
static bool sPageOn[FLIP_PAGE_COUNT] = {false, false};
static int sApplied = 0;                      // 已下发的角度；0 = 框架开机时的值（EasyUI.cfg）
static bool sSuppressed = false;              // "必须正着读"的浮层（闹钟提醒页）显示中
static long long sCLastDownMs = 0;            // 最近一次处理"C 键按下"的时刻（去重用）
static int sCCallSeq = 0;                     // 诊断：C 键被投递的次数（识别重复投递）

/* ---------------- 落盘 ---------------- */

static void flipPersist() {
  for (int i = 0; i < 2; ++i) {
    FILE *fp = fopen(kFlipPaths[i], "w");
    if (!fp) continue;
    fprintf(fp, "%d\n", sWanted ? 1 : 0);
    fclose(fp);
    return;
  }
  LOGD("屏幕翻转: 落盘失败（/data 与 /tmp 都写不了）—— 重启后会回到正向");
}

static void flipLoad() {
  for (int i = 0; i < 2; ++i) {
    FILE *fp = fopen(kFlipPaths[i], "r");
    if (!fp) continue;
    int v = 0;
    if (fscanf(fp, "%d", &v) != 1) v = 0;
    fclose(fp);
    sWanted = (v != 0);
    LOGD("屏幕翻转: 读到落盘状态 %s（%s）", sWanted ? "倒 180°" : "正向", kFlipPaths[i]);
    return;
  }
  sWanted = false;   // 从没设过 = 正向（这也是老设备升级上来的默认）
}

/* ---------------- 下发 ---------------- */

/* ★ 两个值必须**一起**改：只转画面不转触摸 = 点哪儿都不对；只转触摸不转画面同理。
 * 回读一次是为了把"框架实际值"打出来（万一它内部归一化成别的角度，日志里看得见）。 */
static void flipApply(int deg) {
  CONFIGMANAGER->setScreenRotate(deg);
  CONFIGMANAGER->setTouchRotate(deg);
  sApplied = CONFIGMANAGER->getScreenRotate();
  LOGD("屏幕翻转: 下发 %d°（回读 screen=%d touch=%d）", deg,
       CONFIGMANAGER->getScreenRotate(), CONFIGMANAGER->getTouchRotate());
}

static bool flipAnyPageOn() {
  for (int i = 0; i < FLIP_PAGE_COUNT; ++i) {
    if (sPageOn[i]) return true;
  }
  return false;
}

/* ---------------- 对外接口 ---------------- */

void flipInit() {
  if (sInited) return;
  sInited = true;
  /* 开机时框架已按 EasyUI.cfg 的 rotateScreen 设好（本工程 = 0）⇒ 先认它当"当前值"，
   * 这样后面 flipTick() 只在真的需要 180° 时才下发一次，不会开机就白转一下。 */
  sApplied = CONFIGMANAGER->getScreenRotate();
  flipLoad();
  LOGD("屏幕翻转: 就绪（意愿=%s，当前 %d°，落盘路径 %s）", sWanted ? "倒 180°" : "正向",
       sApplied, kFlipPaths[0]);
}

bool flipWanted() { return sWanted; }

void flipSetWanted(bool on) {
  flipInit();
  if (sWanted == on) {
    LOGD("屏幕翻转: 意愿已是 %s，不重复落盘", on ? "倒 180°" : "正向");
  } else {
    sWanted = on;
    flipPersist();
    LOGD("屏幕翻转: 意愿 -> %s（已落盘，重启后仍是这面）", on ? "倒 180°" : "正向");
  }
  flipTick();   // 立刻按当前页面情况下发（在屏保里按 C 时这一下就转过去了）
}

bool flipToggleWanted() {
  flipInit();
  flipSetWanted(!sWanted);
  return sWanted;
}

void flipSetPageOn(FlipPage page, bool on) {
  if (page < 0 || page >= FLIP_PAGE_COUNT) return;
  sPageOn[page] = on;
}

bool flipPageOn(FlipPage page) {
  if (page < 0 || page >= FLIP_PAGE_COUNT) return false;
  return sPageOn[page];
}

void flipSetSuppress(bool on) { sSuppressed = on; }

bool flipSuppressed() { return sSuppressed; }

void flipTick() {
  flipInit();   // 幂等：万一有页面比 Main.cpp 更早调到（自愈，不依赖调用顺序）
  /* ★ 抑制器优先：闹钟提醒页这类"要人读/要人点"的浮层显示时，**永远正着** ——
   *   它会弹在屏保之上（实测 `提醒页=1` 时 `环境页 saver=1`），不抑制的话
   *   挂着看时钟的人会看到**倒着的闹钟**，根本没法规读。 */
  const int want = (sWanted && !sSuppressed && flipAnyPageOn()) ? 180 : 0;
  if (want == sApplied) return;
  flipApply(want);
  LOGD("屏幕翻转: 环境页(saver=%d pet=%d) 意愿=%d 抑制=%d -> 实际 %d°",
       sPageOn[FLIP_PAGE_SAVER] ? 1 : 0, sPageOn[FLIP_PAGE_PET] ? 1 : 0, sWanted ? 1 : 0,
       sSuppressed ? 1 : 0, want);
}

int flipDeg() { return sApplied; }

bool flipHandleSaverKey(int keyCode, bool isDown, bool isLongPress) {
  if (keyCode != PG_KEY_CODE_C) return false;
  /* 诊断：**每次投递都打一行**。留着有实际价值 —— 2026-09-18 就是靠它发现
   * "同一个按键事件会被投递多次"（屏保是 SysApp，与下面压着的 Activity **各有一次**；
   * 实测**一次物理按下 = 4 次投递：DOWN×2 + UP×2，间隔 1ms**）。
   * 不去重的话：DOWN 被切两次（翻转来回一次、净效果为零），抬起那侧还会被当成
   * "任意键唤醒"把屏保关掉。 */
  const long long now = nowMs();
  LOGD("屏保: C 键投递#%d down=%d long=%d（t=%lldms）", ++sCCallSeq, isDown ? 1 : 0,
       isLongPress ? 1 : 0, now);

  if (!isDown) {
    /* ★★ 抬起（以及长按 repeat）**一律吞掉**，而且**不"用一次就清标志"**：
     *   第一版写成"看见第一次抬起就把标志清掉"，结果第二次投递的抬起放行了
     *   ⇒ 被当成"任意键唤醒"把屏保关掉了（实测现象：按一下 C，屏保就没了）。
     *   这里改成"C 键在屏保里从不唤醒、也不透传"—— 它本来就没有别的语义。 */
    return true;
  }
  /* 同一次按下被投递两遍时，只按第一遍算（60ms 去重窗；人手不可能 60ms 内连按两次）。 */
  if (now - sCLastDownMs < 60) return true;
  sCLastDownMs = now;
  const bool on = flipToggleWanted();
  LOGD("屏保: C 键(108) -> 整屏翻转 -> %s（**不唤醒屏保**；长按/按其它键即可唤醒）",
       on ? "倒 180°" : "正向");
  return true;
}

}  // namespace pg
