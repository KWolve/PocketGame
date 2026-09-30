#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD

/* ============================================================================
 *  屏保大时钟：**超级玛丽砖块钟**（2026-09-18 用户需求）
 *
 *  需求原文：
 *    「时钟的格式改成超级玛丽的砖块风格，时钟的背景是砖块，然后当时间要变的时候整个
 *      前面 3 秒，做一个超级玛丽的动画从屏幕左边走出来，然后到了分钟的小数据的位置
 *      跳起来顶一下砖块，然后砖块的数字像金币一样弹出，再出来一个新的数字。
 *      整个屏保时钟的诗词、喜好按键都删掉，背景风格采用超级马里奥游戏背景风格。」
 *
 *  版式：ui/screensaver.html（唯一来源）  素材：tools/gen_marioclock.py（19 张）
 *
 *  动画时间轴（每个整分一次）：
 *    T-3.0s  马里奥从屏幕左外走进来（走 3 帧循环）
 *    T-0.7s  走到第 4 块砖（分个位）正下方 → 起跳
 *    T-0.0s  头撞到砖块 → 砖块上顶 10px + 旧数字当金币飞出（横向压扁=旋转）
 *    T+0.15s 新数字从上方落下
 *    T+0.7s  马里奥落地 → 继续往右走出屏幕
 *    T+1.9s  马里奥隐藏，屏面回到静止
 *
 *  ★★ 为什么整条时间轴是**由绝对时钟推导**的，而不是"每帧累加 dt / 帧计数器状态机"：
 *     本项目在宠物页栽过两次 —— 定时器被挂起或掉帧时，"累加式"动画的相位会**永久漂**，
 *     而且"帧指纹跳帧"那套优化会把 update 一起跳掉，最后表现为**永久冻帧**。
 *     这里改成：每一帧都从 `gettimeofday()` 算出"距离下一个整分还有多少毫秒"(msToFlip)，
 *     所有位置/高度/帧号都是 msToFlip 的**纯函数** ⇒ 掉多少帧都自己对齐，不会漂。
 *     只有"金币弹出"这一小段（540ms）用帧计数器，因为它必须与"整分那一刻"严格对齐，
 *     而它的起点本身就是由整分事件触发的。
 *
 *  ⚠️ 与旧版（翻页钟）的关系：翻页那套（每位 20 张上下半张卡片 + setPosition 压缩）
 *     已整体删除，数字改成**每格一张整图**；砖块与数字**分成两层**，所以砖块能单独上顶、
 *     数字能单独飞出去当金币。
 * ========================================================================== */
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <math.h>   // cosf()：金币旋转的连续曲线（musl 把 libm 合在 libc，不用 -lm）

#include "core/PgGame.h"             // pg::SFX_HIT / pg::SFX_SCORE（撞砖"咚" + 金币"叮"）
#include "ui/ToolPage.h"             // pg::pgHost()：复用主界面的音效宿主（进程级单例）
#include "utils/BrightnessHelper.h"  // BRIGHTNESSHELPER：屏保轻微调暗（本板唯一背光通路）
#include "manager/ConfigManager.h"   // CONFIGMANAGER->setScreenRotate/setTouchRotate：整屏 180° 翻转
#include "platform/PgSaver.h"        // pg::wakeSaverByKey(ke)：任意键唤醒（C 键例外=翻转）
#include "platform/PgFlip.h"         // pg::flipSetPageOn / flipTick / flipToggleWanted（挂绳倒挂）

/* ==================== 音效（2026-09-18 用户选"要，跟静音键走"） ====================
 * 撞砖 = `SFX_HIT`（hit.wav，低声"咚"）、金币飞出 = `SFX_SCORE`（score.wav，"叮"）；
 * 都复用现成素材，**不新增 wav**。
 *
 * ★ "跟静音键走"是**自动成立**的，这里不需要判静音：设备的静音是写 codec 的
 *   `SPK/LINEOUT Switch`（见 settings.cc 的说明），静音后喇叭整个不响 —— 用不着我们判。
 * ★ 每次"撞砖事件"只响一次，**不是每个数字响一次**：4 位同时变（如 09:59 -> 10:00）
 *   也只"咚-叮"一下。金币音**延后 2 拍（100ms）**再响，听感才对（咚→叮）。
 * QA `sfx off` 可彻底关掉（默认开）。 */
static bool sSfxOn = true;
static int sCoinSfxDelay = 0;   // >0 = 还有几拍才播金币音

static void saverPlayHit() {
  if (!sSfxOn) return;
  pg::Host *h = pg::pgHost();
  /* ⚠️ 宿主为空要**打出来**（否则就是"静默无声"，属于本工程明令消灭的静默失败）。
   * ⚠️⚠️ `LOGD` 是**宏**，带 if/else 时**必须加花括号**（本工程铁律，这里刚踩过一次：
   *     `if (!h) LOGD(..); else ...` 会报 `'else' without a previous 'if'`）。 */
  if (!h) {
    LOGD("屏保音效: 宿主未就绪（pgHost() 为空）—— 本次撞砖无声");
  } else {
    h->playSfx(pg::SFX_HIT);
  }
  sCoinSfxDelay = 2;            // 100ms 之后"叮"
}

static void saverTickSfx() {
  if (sCoinSfxDelay > 0 && --sCoinSfxDelay == 0 && sSfxOn) {
    pg::Host *h = pg::pgHost();
    if (h) h->playSfx(pg::SFX_SCORE);
  }
}

/* ==================== 屏保亮度联动（2026-09-18 用户选"轻微调暗"） ====================
 * 进屏保 -> 降到用户亮度的 70%（下限 45）；退出屏保 -> 立刻恢复。
 * ★ 只有 `BRIGHTNESSHELPER` 这一条路（本板没有 `/sys/class/backlight`，见 settings.cc）。
 * ★ 下限的写法有讲究：`lo = min(DIM_MIN, 用户亮度)`，再夹一次"绝不比用户设的更亮" ——
 *   否则用户本来就把亮度调到 40 时，会被"下限 45"给**调亮**，那就荒谬了。
 *
 * ★★ 为什么必须把"进屏保前的用户亮度"**落盘**（这是本节唯一的坑）：
 *   应用若在**屏保显示中**被自愈重启（本工程有 `_exit(0)` 自愈，见 PgSaver 相关说明），
 *   内存里的 `sSavedBright` 就没了 —— 新进程会把"已经调暗的值"当成用户亮度记下来，
 *   于是退出屏保后**永远暗着**。落盘之后，重启后的第一拍就能把它恢复回去。
 */
#define DIM_PCT 70     /* 调暗到用户亮度的百分比 */
#define DIM_MIN 45     /* 调暗后的最低值（再低就看不见时间了） */
static const char *kDimPaths[2] = {"/data/pocketgame_saverdim.dat",
                                   "/tmp/pocketgame_saverdim.dat"};
static int sSavedBright = -1;    /* >=0 = 当前处于调暗态，这就是用户亮度 */
static bool sDimEnabled = true;  /* QA `dim off` 可关掉亮度联动 */

static int saverDimValue(int cur) {
  if (cur <= 0) return -1;                    /* 读不到就别动 */
  int v = cur * DIM_PCT / 100;
  int lo = DIM_MIN < cur ? DIM_MIN : cur;      /* 下限不许超过用户设的 */
  if (v < lo) v = lo;
  if (v > cur) v = cur;                        /* 兜底：绝不比用户设的更亮 */
  return v;
}

static void saverDimPersist(int v) {
  for (int i = 0; i < 2; ++i) {
    FILE *fp = fopen(kDimPaths[i], "w");
    if (!fp) continue;
    fprintf(fp, "%d\n", v);
    fclose(fp);
    return;
  }
  LOGD("屏保亮度: 落盘失败（/data 与 /tmp 都写不了）—— 应用若在屏保里重启，亮度可能停在调暗值");
}

/* 读回用户亮度并**顺手删掉**文件（一次性凭据） */
static int saverDimRead() {
  for (int i = 0; i < 2; ++i) {
    FILE *fp = fopen(kDimPaths[i], "r");
    if (!fp) continue;
    int v = -1;
    if (fscanf(fp, "%d", &v) != 1) v = -1;
    fclose(fp);
    remove(kDimPaths[i]);
    if (v > 0) return v;
  }
  return -1;
}

/* 进屏保：调暗。**注意入口有两个可能的时机**（实测 2026-09-18）：
 *   `performScreensaverOn` -> `registerTimerListener` -> `onUI_init` -> 之后才有 tick。
 * 所以"进屏保调暗"写在 onUI_init 里最稳（tick 里做也行，但没必要多绕一圈）。 */
static void saverDimEnter() {
  if (!sDimEnabled) {
    sSavedBright = -1;
    LOGD("屏保亮度: 联动被 QA 关掉了（dim off），不调暗");
    return;
  }
  /* ★ 上次若"调暗没收尾"（应用在屏保显示中被自愈重启过），文件里那份才是**用户亮度**，
   *   而当前读到的值是调暗后的 —— 必须先恢复再用它当基准，否则亮度基准会被越调越低。 */
  int stale = saverDimRead();
  if (stale > 0) {
    BRIGHTNESSHELPER->setBrightness(stale);
    LOGD("屏保亮度: 发现上次调暗没收尾 -> 先恢复用户亮度 %d 再重新调暗", stale);
  }
  int cur = BRIGHTNESSHELPER->getBrightness();
  int dv = saverDimValue(cur);
  if (dv < 0) {
    sSavedBright = -1;
    LOGD("屏保亮度: 读不到当前亮度，本次不调暗");
    return;
  }
  if (dv == cur) {
    sSavedBright = -1;
    LOGD("屏保亮度: 用户亮度 %d 已经够暗（或低于下限），不动", cur);
    return;
  }
  sSavedBright = cur;
  saverDimPersist(cur);
  BRIGHTNESSHELPER->setBrightness(dv);
  LOGD("屏保亮度: 调暗 %d -> %d（退出恢复 %d）", cur, dv, cur);
}

/* 退屏保：恢复用户亮度。
 * ★★ 为什么必须挂在 `onUI_quit`：**屏保一退出，框架就把本页的定时器停掉**
 *    （实测日志顺序：`performScreensaverOff` -> `unregisterKeyListener` -> `onUI_quit`
 *      之后就再没有本页的 tick 了）—— 所以"每拍自愈"那条路在**退出这一侧根本走不到**。
 *    我第一版就是这么写的，结果唤醒后亮度**永远停在暗的那一档**。 */
static void saverDimExit() {
  int mem = sSavedBright;                 /* 内存里那份（正常路径） */
  int fromFile = saverDimRead();          /* 顺手把凭据删掉 */
  sSavedBright = -1;
  int back = mem > 0 ? mem : fromFile;    /* 内存丢了（屏保里被重启过）→ 用落盘的那份 */
  if (back > 0) {
    BRIGHTNESSHELPER->setBrightness(back);
    LOGD("屏保亮度: 恢复 %d（来源=%s）", back, mem > 0 ? "内存" : "落盘");
  }
}

/* 开机自愈：应用启动时若发现"上次调暗没收尾"，把用户亮度恢复回去（文件即凭据）。
 * ⚠️ 只在**当前不在屏保里**时做 —— 若开机就在屏保里，交给 saverDimEnter 处理。 */
static void saverDimBootCheck() {
  if (EASYUICONTEXT->isScreensaverOn()) return;
  int stale = saverDimRead();
  if (stale > 0) {
    BRIGHTNESSHELPER->setBrightness(stale);
    LOGD("屏保亮度: 开机发现上次调暗没收尾 -> 恢复 %d", stale);
  } else {
    LOGD("屏保亮度: 无未收尾的调暗（当前亮度 %d）", BRIGHTNESSHELPER->getBrightness());
  }
}

/* ==================== 几何：**以 ui/screensaver.html 为唯一来源** ====================
 * 2026-09-14 的血案：几何在 .cc 里手抄一份、html 里再写一份，改了 html 而 .cc 里的那份
 * 被 setPosition 覆盖 ⇒ 屏幕上"数字没动、冒号跑了"（看起来就是"点偏了"）。
 * 所以这里**开机从 ftu 读回来**当基准，下面的常量只作"读不到时"的兜底。
 * 读数只取 **Brick（静态砖块）**：数字图层会被金币动画改宽高，读到中间态会把整块钟弄变形。 */
static int sGeomX[4] = {35, 131, 261, 357};
static int sGeomY = 180;
static int sGeomW = 88;
static int sGeomH = 140;
static int sColonX = 227;

#define PLAT_Y 540        /* 悬空砖台顶边（马里奥脚底）—— 与 gen_marioclock.py 的 PLAT_Y */
#define MARIO_W 68
#define MARIO_H 68
#define MARIO_FEET_ROW 66 /* 4 帧墨迹"脚底"所在行（gen_marioclock.py 有自检保证一致） */
#define MARIO_HEAD_ROW 2  /* 墨迹"头顶"所在行（= 描边外扩 2px） */
/* 站立时控件盒的 y：让"墨迹脚底"正好落在砖台顶边（+1 是让脚踩进去 1px，不悬空） */
#define MARIO_STAND_Y (PLAT_Y - MARIO_FEET_ROW + 1)

/* ---- 时间轴常量（毫秒）---- */
#define WALK_TOTAL_MS 2300  /* 走进来的耗时（3s 里前 2.3s 走，剩 0.7s 跳） */
#define JUMP_MS 700         /* 起跳到撞砖：msToFlip 从 700 到 0 */
#define JUMP_H 160          /* 脚底抬升高度（540 -> 380，头顶正好顶到砖块下缘 320） */
#define EXIT_MS 1900        /* 撞砖之后：0..700 落回，700..1900 往右走出屏幕 */
/* ★★ 金币弹出用**"动画时间"（毫秒）**驱动，每一步推进 SAVER_MS：
 *   为什么不用"帧号"：用户报"4 个数字变化时页面很卡"，实测**逻辑侧毫无问题**
 *   （节拍稳定 50/51ms、本页单拍 ≤1ms、进程 CPU 4%），但面板是 60Hz 而动画只有 20fps
 *   ⇒ 真正的问题是**视觉台阶感**：金币每 50ms 跳一档宽度、一帧位移 16px，看着就是"卡"。
 *   所以把时基提到 40fps(SAVER_MS=25) 并把曲线做平滑（上升 ease-out、旋转连续余弦）。
 *   ⚠️ 每步仍只推进固定的一小段 ⇒ QA `slow n` 依然是"拉开步距"，可以逐档抓帧。 */
#define POP_FLY_MS 440      /* 旧数字飞出去（含旋转） */
#define POP_DROP_MS 170     /* 新数字落下 */
#define POP_TOTAL_MS (POP_FLY_MS + POP_DROP_MS)
#define POP_SPIN_MIN 18     /* 旋转压到最扁时的宽度（像素） */
#define POP_SPIN_HALF 1.7   /* 飞行期间转几个半圈（1.7 -> 约 3.4 个半圈，看着在转） */
#define POP_RISE_MAX 130    /* 金币往上飞的总高度 */
#define POP_DROP_MAX 26     /* 新数字从上方多少像素落下来 */
#define BRICK_BUMP 10       /* 砖块被顶起来的高度 */
#define BRICK_BUMP_FRAMES 2 /* 顶起来保持几帧（100ms） */

/* ==================== 任意按键退出屏保（2026-09-14 用户要求） ====================
 * 本板物理键只有三个（103 音量- / 105 音量+ / 108 暂停），屏保里按任意一个都应当"唤醒"，
 * 而不是去调音量、也不该触发下面页面的动作 ⇒ 整次按键（DOWN/UP/LONG）全部吞掉。
 * ⚠️ 光靠这个监听器不够：框架按键分发是**短路式**的，各页监听器入口也各加了一句
 *    `pg::wakeSaverByKey()`（platform/PgSaver.h），谁先被叫到谁完成唤醒。 */
class SaverKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★★ 统一走 `pg::wakeSaverByKey(ke)`（platform/PgSaver.h）—— 别在这里自己判：
     *   框架的按键分发是**短路式**的，屏保显示时下面挂着的 wifi/iptv/工具页监听器还在链上，
     *   谁先被叫到谁就把它唤醒了（取决于注册顺序，不确定）。
     *   这个统一入口里有两件事：
     *     ① 任意键 → 唤醒屏保（屏保页只是"看时间"，不该顺带触发页面动作）；
     *     ② **C 键(108) 例外**：它是"整屏翻转 180°"（用户 2026-09-18 需求 ——
     *        挂绳在底部、设备倒挂着，按一下就把画面转过来看时间）。翻转**不唤醒屏保**，
     *        连它的抬起事件也一起吞掉（否则抬起那一下又会被当成"任意键"唤醒）。见 PgFlip.h。 */
    if (pg::wakeSaverByKey(ke)) return true;
    return true;   // 屏保在场时整次按键都吞掉：别让"唤醒"顺带触发页面动作
  }
};
SaverKeys sSaverKeys;

#define TIMER_SAVER 0
#define SAVER_MS 25 /* 40fps：走/跳/金币/冒号闪烁的时基（面板 60Hz，20fps 会有台阶感） */

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_SAVER, SAVER_MS},
};

/* ==================== 控件指针表 ====================
 * [位][数字] -> 数字图层控件；砖块 4 个；马里奥 4 帧。
 * 指针本身由 INIT_UI_EVENT_BINDINGS 声明，这里取地址建表。 */
#define PG_DIG(s, d) (&mDig##s##_##d##Ptr)
#define PG_ROW(M, s)                                                          \
  M(s, 0), M(s, 1), M(s, 2), M(s, 3), M(s, 4), M(s, 5), M(s, 6), M(s, 7),     \
      M(s, 8), M(s, 9)
static ZKTextView *const *const sDigTbl[4][10] = {{PG_ROW(PG_DIG, 0)},
                                                  {PG_ROW(PG_DIG, 1)},
                                                  {PG_ROW(PG_DIG, 2)},
                                                  {PG_ROW(PG_DIG, 3)}};
static ZKTextView *saverDig(int slot, int d) { return *sDigTbl[slot][d]; }

static ZKTextView *saverBrick(int slot) {
  switch (slot) {
    case 0: return mBrick0Ptr;
    case 1: return mBrick1Ptr;
    case 2: return mBrick2Ptr;
    default: return mBrick3Ptr;
  }
}
/* 马里奥帧：0/1/2 = 走，3 = 跳（顶砖块） */
static ZKTextView *saverMarioFrame(int f) {
  switch (f) {
    case 0: return mMarioW0Ptr;
    case 1: return mMarioW1Ptr;
    case 2: return mMarioW2Ptr;
    default: return mMarioJumpPtr;
  }
}

/* ---- 慢帧计量（2026-09-18 加：用户报"4 个数字变化的时候页面很卡"）-----------------
 * 屏保是"常显 + 20fps"的页，一旦 UI 线程被别的东西占住（或本页一次改太多控件），
 * 表现就是"数字变化那一下卡一下"。光靠眼睛判断不了是"本页太重"还是"别处在抢 CPU"，
 * 所以这里把**本页 tick 的实际周期**量出来：正常应当 ≈ SAVER_MS(50ms)，
 * 每个统计窗（1s）打印一次 平均/最大 —— 差值就是被卡住的毫秒数。
 * QA `dumpslow` 可直接读；日志里每 1 秒一条（只有超阈值才打，避免刷屏）。 */
static long long sLastTickMs2 = 0;
static int sSlowMax = 0;
static int sSlowSum = 0;
static int sSlowN = 0;
static long long sTickT0 = 0;
static int sCostMax = 0;      /* 本页单拍最大耗时（含 setPosition/setVisible 的同步成本） */
static int sMutMax = 0;       /* 单拍最多改动了几个控件 */
static int sMutNow = 0;       /* 本拍已改动的控件数 */
#define SLOW_LOG_MS 60   /* 单拍超过它才算"卡了一下"（正常 ≈25ms，留足余量） */
#define COST_LOG_MS 10   /* 本页单拍耗时超过它才报（40fps 下 10ms = 占了近半拍） */

static long long saverMsNow() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* 每拍开头调用：算与上一拍的间隔 */
static void saverTickBegin() {
  long long now = saverMsNow();
  if (sLastTickMs2 > 0) {
    int d = (int)(now - sLastTickMs2);
    sSlowSum += d;
    ++sSlowN;
    if (d > sSlowMax) sSlowMax = d;
  }
  sLastTickMs2 = now;
  sTickT0 = now;
  sMutNow = 0;
}

/* 每拍结尾调用：算本页自己的耗时；每 20 拍（1s）汇总打印一次（只在有异常时打） */
static void saverTickEnd() {
  int cost = (int)(saverMsNow() - sTickT0);
  if (cost > sCostMax) sCostMax = cost;
  if (sMutNow > sMutMax) sMutMax = sMutNow;
  if (sSlowN >= 20) {
    if (sSlowMax > SLOW_LOG_MS || sCostMax > COST_LOG_MS) {
      LOGD("屏保慢帧: 20 拍 间隔平均 %dms 最大 %dms | 本页单拍最大耗时 %dms（最多改动 %d 个控件）",
           sSlowSum / (sSlowN ? sSlowN : 1), sSlowMax, sCostMax, sMutMax);
    }
    sSlowMax = 0;
    sSlowSum = 0;
    sSlowN = 0;
    sCostMax = 0;
    sMutMax = 0;
  }
}

/* ---- 状态 ---- */
static int sCur[4] = {-1, -1, -1, -1};  /* 已提交（或正在提交）的数字 */
static bool sPopOn[4] = {false, false, false, false}; /* 该位是否在弹金币 */
static int sPopFrom[4] = {0, 0, 0, 0};  /* 正在飞出去的那个旧数字 */
static int sPopMs[4] = {0, 0, 0, 0};      /* 金币动画已播的"动画时间"(ms) */
static bool sPopSwapped[4] = {false, false, false, false}; /* 本位的旧/新数字是否已切换 */
static int sBump[4] = {0, 0, 0, 0};     /* 砖块上顶剩余帧数 */
static int sBrickDy[4] = {0, 0, 0, 0};  /* 砖块当前位移（只为"变化才 setPosition"） */
static int sMarioShown = -1;            /* 当前显示的马里奥帧；-1 = 隐藏 */
static int sMarioX = 0, sMarioY = 0;    /* 马里奥当前位置（只为"变化才 setPosition"） */
static int sFrameCnt = 0;               /* 全局帧计数（只用于走路换帧） */
static long sEnterPhase = -1;           /* 进屏保那一拍的 msToFlip（决定能不能完整走一段） */
static bool sMarioSkip = false;         /* 本条时间轴不让马里奥出场（见下方"入场时机"说明） */

/* ---- 入场时机（2026-09-18 补）----
 * 若"进屏保"发生在**本条动画窗口之内**，这一分钟就不让马里奥出场：
 *   · 正处在"前 3 秒"里进来 → 按剩余时间**压缩走路**（仍从屏幕左外走进来）；剩得太少（<0.9s）就不出场；
 *   · 正处在"撞砖后 1.9 秒"里进来 → 他本该正在往右走掉，半路出现比不出场更怪 ⇒ 不出场。
 * ⚠️ 补这条之前的表现：进屏保晚了会把马里奥**凭空画在屏幕中间**往右走，一眼就是 bug。 */
#define MIN_WALK_MS 900   /* 压缩后走路的最短时长（再短就不像"走"了） */
static bool sColonOn = true;
static int sQaDiv = 0;
static int sLastSec = -1;               /* 秒变化判据（日期/星期只在秒变化时刷） */
static int sWarm = 0;                   /* 刚进屏保的前几拍：强制重设文本 + 对齐 */
static bool sReady = false;             /* 第一次 tick */
static char sQaTag[256] = {0};

/* ---- QA（免触摸验收通道 /tmp/pg_savercmd，只在屏保显示期间执行）---- */
static int sForceHHMM = -1;  /* 强制时间 hhmm；-1 = 用系统时间 */
static int sAnimDiv = 1;     /* 动画分频（1 = 每帧 50ms）；QA `slow n` 放慢，便于抓中间帧 */
static int sAnimDivCnt = 0;
static bool sQaAnim = false; /* QA `mario`：强制播一遍完整动画 */
static long sQaPhase = 0;    /* 合成相位（毫秒，从 3000 往下走，撞砖后变负数） */
static bool sQaBumped = false;
static int sThemeForce = -1; /* -1 自动（6:00-18:00 白天）/ 0 夜空 / 1 白天 */
static int sBgShown = -1;    /* 当前上屏的背景：0 夜空 / 1 白天 */

/* ==================== 几何回读 ==================== */
static void saverReadGeom() {
  ZKTextView *b0 = saverBrick(0);
  if (b0) {
    const LayoutPosition &p = b0->getPosition();
    if (p.mWidth >= 40 && p.mHeight >= 30 && p.mTop >= 0 && p.mTop < 800) {
      sGeomY = p.mTop;
      sGeomW = p.mWidth;
      sGeomH = p.mHeight;
    }
  }
  for (int i = 0; i < 4; ++i) {
    ZKTextView *b = saverBrick(i);
    if (b) {
      const LayoutPosition &p = b->getPosition();
      if (p.mLeft >= 0 && p.mLeft < 480) sGeomX[i] = p.mLeft;
    }
  }
  if (mColonOnPtr) {
    const LayoutPosition &p = mColonOnPtr->getPosition();
    if (p.mLeft >= 0 && p.mLeft < 480) sColonX = p.mLeft;
  }
  LOGD("屏保几何(取自 ftu): y=%d w=%d h=%d x=[%d,%d,%d,%d] 冒号x=%d",
       sGeomY, sGeomW, sGeomH, sGeomX[0], sGeomX[1], sGeomX[2], sGeomX[3], sColonX);
}

/* ==================== 基础操作 ==================== */
static void saverShow(int slot, int d) {
  for (int k = 0; k < 10; ++k) {
    ZKTextView *t = saverDig(slot, k);
    if (t) {
      ++sMutNow;
      t->setVisible(k == d);
    }
  }
}

/* 只在本位数字的几何与目标不一致时才 setPosition（每帧无脑写 = 重绘风暴） */
static void saverDigGeom(int slot, int d, int x, int y, int w, int h) {
  ZKTextView *t = saverDig(slot, d);
  if (!t) return;
  const LayoutPosition &p = t->getPosition();
  if (p.mLeft == x && p.mTop == y && p.mWidth == w && p.mHeight == h) return;
  ++sMutNow;
  t->setPosition(LayoutPosition(x, y, w, h));
}

/* 砖块位移（撞砖时上顶 BRICK_BUMP 像素） */
static void saverBrickGeom(int slot, int dy) {
  if (sBrickDy[slot] == dy) return;
  sBrickDy[slot] = dy;
  ZKTextView *b = saverBrick(slot);
  if (b) {
    ++sMutNow;
    b->setPosition(LayoutPosition(sGeomX[slot], sGeomY - dy, sGeomW, sGeomH));
  }
}

static void saverBrickReset() {
  for (int i = 0; i < 4; ++i) saverBrickGeom(i, 0);
}

/* ==================== 马里奥 ==================== */
static void saverMarioHide() {
  for (int f = 0; f < 4; ++f) {
    ZKTextView *t = saverMarioFrame(f);
    if (t) t->setVisible(false);
  }
  sMarioShown = -1;
}

static void saverMarioShow(int frame, int x, int y) {
  if (frame != sMarioShown) {
    for (int f = 0; f < 4; ++f) {
      ZKTextView *t = saverMarioFrame(f);
      if (t) t->setVisible(f == frame);
    }
    sMarioShown = frame;
  }
  if (x != sMarioX || y != sMarioY) {
    sMarioX = x;
    sMarioY = y;
    ZKTextView *t = saverMarioFrame(frame);
    if (t) {
      ++sMutNow;
      t->setPosition(LayoutPosition(x, y, MARIO_W, MARIO_H));
    }
  }
}

/* 目标站立点：第 4 块砖（分个位）的水平中心 */
static int saverMarioTargetX() {
  return sGeomX[3] + sGeomW / 2 - MARIO_W / 2;
}

/* 纯函数：给定"距下一个整分的毫秒"，算出马里奥此刻该在哪一帧、什么位置。
 * 返回 false = 此刻不该显示（大部分时间）。 */
static bool saverMarioPose(long msToFlip, int *frame, int *x, int *y) {
  const long since = 60000 - msToFlip;   /* 距上一次整分的毫秒 */
  const int standY = MARIO_STAND_Y;
  if (msToFlip <= JUMP_MS) {             /* 撞砖前 0.7s：跳起来 */
    if (sMarioSkip) return false;
    long t = msToFlip;
    long h = (long)JUMP_H * (JUMP_MS * JUMP_MS - t * t) / (JUMP_MS * JUMP_MS);
    *frame = 3;
    *x = saverMarioTargetX();
    *y = standY - (int)h;
    return true;
  }
  if (since <= JUMP_MS) {                /* 撞砖后 0.7s：落回 */
    if (sMarioSkip) return false;
    long t = since;
    long h = (long)JUMP_H * (JUMP_MS * JUMP_MS - t * t) / (JUMP_MS * JUMP_MS);
    *frame = 3;
    *x = saverMarioTargetX();
    *y = standY - (int)h;
    return true;
  }
  if (since <= EXIT_MS) {                /* 撞砖后 0.7..1.9s：往右走出屏幕 */
    if (sMarioSkip) return false;
    long p = since - JUMP_MS;
    long span = (480 + MARIO_W) - saverMarioTargetX();
    *frame = ((sFrameCnt / 4) + 2) % 3;
    *x = saverMarioTargetX() + (int)(span * p / (EXIT_MS - JUMP_MS));
    *y = standY;
    return true;
  }
  if (msToFlip <= 3000) {                /* 前面 3 秒：从屏幕左外走进来 */
    /* 起点时刻：正常是 T-3.0s；若 3 秒窗口内才进屏保，就按进屏保那一刻起走（压缩走路）。 */
    long wStart = (sEnterPhase > JUMP_MS && sEnterPhase < 3000) ? sEnterPhase : 3000;
    long span = wStart - JUMP_MS;
    if (span < MIN_WALK_MS) {            /* 剩得太少：这一分钟不出场（别勉强挤一段"瞬移"） */
      if (!sMarioSkip) LOGD("屏保: 进屏保太晚（剩 %ldms）-> 这一分钟马里奥不出场", span);
      sMarioSkip = true;
      return false;
    }
    long p = wStart - msToFlip;          /* 已经走了多久 */
    *frame = (sFrameCnt / 4) % 3;
    *x = -(MARIO_W + 12) +
         (int)(((long)(saverMarioTargetX() + MARIO_W + 12) * p) / span);
    *y = standY;
    return true;
  }
  sMarioSkip = false;   /* 真正的空闲段（一条时间轴结束）→ 下一分钟重新评估 */
  return false;
}

/* ==================== 金币弹出 ==================== */
static void saverPopFinish(int slot) {
  if (!sPopOn[slot]) return;
  sPopOn[slot] = false;
  ZKTextView *old = saverDig(slot, sPopFrom[slot]);
  if (old) old->setVisible(false);
  saverDigGeom(slot, sPopFrom[slot], sGeomX[slot], sGeomY, sGeomW, sGeomH);
  saverShow(slot, sCur[slot]);
  saverDigGeom(slot, sCur[slot], sGeomX[slot], sGeomY, sGeomW, sGeomH);
}

/* 该位数字要变了：旧数字当金币飞出去，新数字随后落下。
 * ⚠️ sCur 在**这里**就更新成新值：显示会晚 550ms（金币飞完 + 新数字落下），
 *    但"时间的真值"立刻跟系统对齐 —— 否则下一次比较会再触发一次弹出。 */
static void saverPopStart(int slot, int to) {
  if (sPopOn[slot]) saverPopFinish(slot);
  int from = sCur[slot];
  sCur[slot] = to;
  if (from < 0 || from == to) {   /* 没得弹（首次对齐/数字没变）→ 直接就位 */
    saverShow(slot, to);
    saverDigGeom(slot, to, sGeomX[slot], sGeomY, sGeomW, sGeomH);
    return;
  }
  sPopOn[slot] = true;
  sPopFrom[slot] = from;
  sPopMs[slot] = 0;
  sPopSwapped[slot] = false;
  sBump[slot] = BRICK_BUMP_FRAMES;   /* 撞砖：砖块上顶一下 */
  saverShow(slot, from);
  saverDigGeom(slot, from, sGeomX[slot], sGeomY, sGeomW, sGeomH);
  LOGD("屏保: 位%d 撞砖 %d -> %d（金币弹出）", slot, from, to);
}

/* 缓动/旋转都改成连续函数（原来 8 档离散宽度 = 每 50ms 跳一档，正是"看着卡"的来源）。
 * ⚠️ 这里用 cos()：本工程工具链是 musl（libm 合进 libc），不需要额外 -lm。 */
static inline float saverEaseOut(float p) { return 1.0f - (1.0f - p) * (1.0f - p); }
static inline float saverCosine01(float p) {
  return 0.5f - 0.5f * cosf(6.2831853f * POP_SPIN_HALF * p);
}

static void saverPopAdvance() {
  for (int i = 0; i < 4; ++i) {
    if (!sPopOn[i]) continue;
    sPopMs[i] += SAVER_MS;                    /* 每步推进一小段动画时间（slow 只拉大步距） */
    int t = sPopMs[i];
    if (t < POP_FLY_MS) {                     /* 旧数字：一边上升一边"转"（横向压扁） */
      float p = (float)t / (float)POP_FLY_MS;
      int rise = (int)(POP_RISE_MAX * saverEaseOut(p));
      int w = POP_SPIN_MIN + (int)((sGeomW - POP_SPIN_MIN) * saverCosine01(p));
      int x = sGeomX[i] + (sGeomW - w) / 2;   /* 压扁时中轴不动 */
      saverDigGeom(i, sPopFrom[i], x, sGeomY - rise, w, sGeomH);
    } else {
      if (!sPopSwapped[i]) {                  /* 飞完（已到顶）：换新数字，从上方落下 */
        sPopSwapped[i] = true;
        saverDigGeom(i, sPopFrom[i], sGeomX[i], sGeomY, sGeomW, sGeomH);
        ZKTextView *old = saverDig(i, sPopFrom[i]);
        if (old) old->setVisible(false);
        saverShow(i, sCur[i]);
      }
      int dt = t - POP_FLY_MS;
      if (dt > POP_DROP_MS) dt = POP_DROP_MS;
      float p = (float)dt / (float)POP_DROP_MS;
      int dy = (int)(POP_DROP_MAX * (1.0f - p * p));   /* 落下：越落越快（ease-in） */
      saverDigGeom(i, sCur[i], sGeomX[i], sGeomY - dy, sGeomW, sGeomH);
    }
    if (sPopMs[i] > POP_TOTAL_MS) saverPopFinish(i);
  }
  for (int i = 0; i < 4; ++i) {
    if (sBump[i] > 0) {
      saverBrickGeom(i, BRICK_BUMP);
      --sBump[i];
      if (sBump[i] == 0) saverBrickGeom(i, 0);
    }
  }
}

/* ==================== 时间 / 日期 / 北京时间 ==================== */
/* 读出"距下一个整分的毫秒"与当前时分秒。
 * ★ 为什么需要**毫秒级**相位：马里奥要在"整分那一刻"正好顶到砖块，
 *   而 20fps 的定时器只有 50ms 分辨率，用"秒"根本对不齐（会早/晚最多 1 秒）。
 * ⚠️ 面板无 RTC，未校时前是 1970 —— 但"秒"照样在走，动画不受影响。 */
static void saverNow(long *msToFlip, int *hh, int *mm, int *ss) {
  if (sForceHHMM >= 0) {   /* QA 强制时间：没有真实毫秒相位 */
    *hh = (sForceHHMM / 100) % 100;
    *mm = sForceHHMM % 100;
    *ss = 59;
    if (sQaAnim) {
      long ph = sQaPhase > 0 ? sQaPhase : sQaPhase + 60000;
      *msToFlip = ph > 0 ? ph : 57000;
    } else {
      *msToFlip = 57000;   /* 安全区：马里奥一定是隐藏的 */
    }
    return;
  }
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t t = tv.tv_sec;
  struct tm lt;
  localtime_r(&t, &lt);
  *hh = lt.tm_hour;
  *mm = lt.tm_min;
  *ss = lt.tm_sec;
  long ms = (long)(60 - lt.tm_sec) * 1000 - (long)(tv.tv_usec / 1000);
  if (ms < 0) ms = 0;
  if (ms > 60000) ms = 60000;
  *msToFlip = ms;
}

/* 背景按小时切：6:00-18:00 白天（地上关），其余夜空关。
 * 为什么要有夜空版：屏保大多在夜里跑，整屏白天蓝底在凌晨会很刺眼（用户选的就是"自动切"）。 */
static void saverApplyBg(int hour) {
  int day = (sThemeForce >= 0) ? sThemeForce : ((hour >= 6 && hour < 18) ? 1 : 0);
  if (day == sBgShown) return;
  sBgShown = day;
  if (mSaverBgDayPtr) mSaverBgDayPtr->setVisible(day == 1);
  if (mSaverBgNightPtr) mSaverBgNightPtr->setVisible(day == 0);
  LOGD("屏保背景: %s（hour=%d 强制=%d）", day ? "白天·地上关" : "夜空关", hour,
       sThemeForce);
}

/* ---- 日期 / 星期（自己格式化，见 ui/screensaver.html 里为什么不用 digitalclock）---- */
static const char *WEEK_CN[7] = {"星期日", "星期一", "星期二", "星期三",
                                 "星期四", "星期五", "星期六"};
static char sDateText[16] = {0};
static char sWeekText[16] = {0};

static void saverUpdateDate() {
  time_t t = time(NULL);
  struct tm lt;
  localtime_r(&t, &lt);
  char dbuf[16];
  snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", lt.tm_year + 1900, lt.tm_mon + 1,
           lt.tm_mday);
  const char *wk = WEEK_CN[(lt.tm_wday % 7 + 7) % 7];
  /* 只在变化时 setText（每帧无条件写控件 = 重绘风暴） */
  if (strcmp(dbuf, sDateText) != 0) {
    strncpy(sDateText, dbuf, sizeof(sDateText) - 1);
    LOGD("屏保日期: '%s'", dbuf);
    if (mTextSaverDatePtr) mTextSaverDatePtr->setText(dbuf);
  }
  if (strcmp(wk, sWeekText) != 0) {
    strncpy(sWeekText, wk, sizeof(sWeekText) - 1);
    LOGD("屏保星期: '%s'", wk);
    if (mTextSaverWeekPtr) mTextSaverWeekPtr->setText(wk);
  }
}

/* ==================== 对齐 / 复位 ==================== */
static void saverSnap(int *now) {
  for (int i = 0; i < 4; ++i) {
    sCur[i] = now[i];
    sPopOn[i] = false;
    sPopFrom[i] = now[i];
    sPopMs[i] = 0;
    sPopSwapped[i] = false;
    sBump[i] = 0;
    saverShow(i, now[i]);
    saverDigGeom(i, now[i], sGeomX[i], sGeomY, sGeomW, sGeomH);
  }
  saverBrickReset();
  saverMarioHide();
  sMarioX = sMarioY = -999;   /* 让下一次 setPosition 一定生效 */
}

/* QA 文件当前内容设为基线（不执行）。
 * ⚠️ 必须做：屏保 App 实例会被销毁/重建，而 sQaTag 一重建就没了 —— 若不设基线，
 *    上一次留下的命令会被**重放**（实测踩到：残留的 `off` 让屏保刚进就自己退出）。 */
static void saverQaSyncTag() {
  FILE *fp = fopen("/tmp/pg_savercmd", "r");
  if (!fp) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, fp);
  fclose(fp);
  sQaTag[n] = 0;
}

static int saverNextMinute(int hhmm) {
  int hh = (hhmm / 100) % 100, mm = hhmm % 100;
  if (++mm >= 60) {
    mm = 0;
    hh = (hh + 1) % 24;
  }
  return hh * 100 + mm;
}

static void saverPollQa() {
  FILE *fp = fopen("/tmp/pg_savercmd", "r");
  if (!fp) return;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  buf[n] = 0;
  if (n == 0) return;
  if (strncmp(buf, sQaTag, sizeof(sQaTag) - 1) == 0) return; /* 整份内容去重 */
  strncpy(sQaTag, buf, sizeof(sQaTag) - 1);
  sQaTag[sizeof(sQaTag) - 1] = 0;

  char *save = NULL;
  for (char *line = strtok_r(buf, "\r\n", &save); line;
       line = strtok_r(NULL, "\r\n", &save)) {
    while (*line == ' ' || *line == '\t') ++line;
    if (*line == 0 || *line == '#') continue;
    /* 剥掉行内注释（`#` 起）：命令文件习惯写 `mario #k1`（内容必须变，
     * 否则整份去重不执行），不剥的话会被当成子命令。 */
    {
      char *h = strchr(line, '#');
      if (h) *h = 0;
      int ln = (int)strlen(line);
      while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
      if (line[0] == 0) continue;
    }
    if (strncmp(line, "set ", 4) == 0) {
      sForceHHMM = atoi(line + 4);
      sQaAnim = false;
      LOGD("屏保 QA: 强制时间 %04d", sForceHHMM);
    } else if (strncmp(line, "auto", 4) == 0) {
      sForceHHMM = -1;
      sQaAnim = false;
      LOGD("屏保 QA: 恢复系统时间");
    } else if (strncmp(line, "next", 4) == 0) {
      int hh, mm, ss;
      long ms;
      saverNow(&ms, &hh, &mm, &ss);
      sForceHHMM = saverNextMinute(hh * 100 + mm);
      sQaAnim = false;
      LOGD("屏保 QA: next -> %04d", sForceHHMM);
    } else if (strncmp(line, "mario", 5) == 0) {
      /* ★ 立刻播一遍完整动画：走进来 -> 跳 -> 撞砖 -> 金币弹出 -> 新数字 -> 走掉。
       *   为什么要它：真实动画只在整分出现，等一分钟没法做验收；
       *   配合 `slow n` 还能把整段放慢，抓中间帧（截图往返 ~1.2-2.4s，动画才 0.6s）。
       *   可选参数 <ms> = 从"距整分多少毫秒"开始演，用来复现"进场晚了"的各种情形：
       *     `mario 2400`  = 3 秒窗口内进场（走路会被压缩）
       *     `mario 1200`  = 进场太晚（剩 <0.9s）→ 这一分钟马里奥不出场
       *     `enter 59500` + `mario` = 正好在"撞砖后的出场窗口"里进场 → 也不出场
       *   ⚠️ 为什么需要这套：设备会周期性校时（实测两次读到的相位对不上 PC 时钟），
       *      靠算秒表卡"整分前 1 秒进屏保"根本卡不准 ⇒ 必须能在 QA 里**指定相位**。 */
      const char *a = line + 5;
      while (*a == ' ') ++a;
      if (sForceHHMM < 0) {
        int hh, mm, ss;
        long ms;
        saverNow(&ms, &hh, &mm, &ss);
        sForceHHMM = hh * 100 + mm;
      }
      sQaAnim = true;
      sQaPhase = (*a >= '0' && *a <= '9') ? atol(a) : 3000;
      if (sQaPhase < JUMP_MS + 50) sQaPhase = JUMP_MS + 50;
      if (sQaPhase > 60000) sQaPhase = 3000;
      sQaBumped = false;
      LOGD("屏保 QA: mario 演示开始（相位 %ldms；撞砖时时间前进 1 分钟）", sQaPhase);
    } else if (strncmp(line, "enter", 5) == 0) {
      /* 复现"进屏保那一刻的相位"：与真实进场走**同一个判断**（见 saverMarioPose 说明）。
       * `enter` 无参数 = 复位成"正常进场"（-1）。 */
      const char *a = line + 5;
      while (*a == ' ') ++a;
      if (*a >= '0' && *a <= '9') {
        sEnterPhase = atol(a);
        sMarioSkip = ((60000 - sEnterPhase) <= EXIT_MS);
        /* 走路时间不足时"会出场"是假的（真正判定在 saverMarioPose 里）—— 日志说清楚 */
        int tight = (!sMarioSkip && sEnterPhase > JUMP_MS &&
                     sEnterPhase - JUMP_MS < MIN_WALK_MS);
        LOGD("屏保 QA: 模拟进场相位 %ldms -> 马里奥%s出场%s", sEnterPhase,
             sMarioSkip ? "不" : "会",
             tight ? "（但走路时间不足，实际也不出场）" : "");
      } else {
        sEnterPhase = -1;
        sMarioSkip = false;
        LOGD("屏保 QA: 模拟进场相位复位（正常 3 秒进场）");
      }
    } else if (strncmp(line, "theme", 5) == 0) {
      const char *a = line + 5;
      while (*a == ' ') ++a;
      if (strncmp(a, "day", 3) == 0) sThemeForce = 1;
      else if (strncmp(a, "night", 5) == 0) sThemeForce = 0;
      else sThemeForce = -1;
      sBgShown = -1;   /* 强制下一次重算 */
      LOGD("屏保 QA: 背景 theme=%s", sThemeForce < 0 ? "auto" : (sThemeForce ? "day" : "night"));
    } else if (strncmp(line, "off", 3) == 0) {
      LOGD("屏保 QA: 退出屏保");
      EASYUICONTEXT->screensaverOff();
    } else if (strncmp(line, "dump", 4) == 0) {
      int hh, mm, ss;
      long ms;
      saverNow(&ms, &hh, &mm, &ss);
      LOGD("屏保 QA: 显示 %d%d:%d%d | 距整分 %ldms | 背景=%s | 马里奥帧=%d pos=(%d,%d) | "
           "金币[%d%d%d%d] 位移[%d%d%d%d]",
           sCur[0], sCur[1], sCur[2], sCur[3], ms, sBgShown == 1 ? "白天" : "夜空",
           sMarioShown, sMarioX, sMarioY, sPopOn[0] ? 1 : 0, sPopOn[1] ? 1 : 0,
           sPopOn[2] ? 1 : 0, sPopOn[3] ? 1 : 0, sBrickDy[0], sBrickDy[1], sBrickDy[2],
           sBrickDy[3]);
    } else if (strncmp(line, "sfx", 3) == 0) {
      const char *a = line + 3;
      while (*a == ' ') ++a;
      if (strncmp(a, "off", 3) == 0) sSfxOn = false;
      else if (strncmp(a, "on", 2) == 0) sSfxOn = true;
      LOGD("屏保 QA: 音效 sfx=%s（撞砖 hit.wav / 金币 score.wav；静音键仍照常生效）",
           sSfxOn ? "on" : "off");
    } else if (strncmp(line, "dim", 3) == 0) {
      const char *a = line + 3;
      while (*a == ' ') ++a;
      if (strncmp(a, "off", 3) == 0) sDimEnabled = false;
      else if (strncmp(a, "on", 2) == 0) sDimEnabled = true;
      int cur = BRIGHTNESSHELPER->getBrightness();
      LOGD("屏保 QA: 亮度联动 dim=%s | 当前亮度 %d | 处于调暗态 %s（记住的用户亮度 %d）",
           sDimEnabled ? "on" : "off", cur, sSavedBright >= 0 ? "是" : "否", sSavedBright);
    } else if (strncmp(line, "flip", 4) == 0) {
      /* 整屏翻转（挂绳倒挂）：`flip` = 切换、`flip on|off` = 指定。
       * 与主界面 QA 的 `flip` 同源（都走 pg::flipSetWanted）—— 验收时必须"与真实操作同一条路"。
       * 屏保显示中下发，屏幕会**立刻**转过去（flipTick 在同一拍就下发）。 */
      const char *a = line + 4;
      while (*a == ' ') ++a;
      if (strncmp(a, "on", 2) == 0) pg::flipSetWanted(true);
      else if (strncmp(a, "off", 3) == 0) pg::flipSetWanted(false);
      else pg::flipToggleWanted();
      LOGD("屏保 QA: 翻转 意愿=%s | 屏幕实际 %d° | 环境页 saver=%d pet=%d | 抑制=%d",
           pg::flipWanted() ? "倒 180°" : "正向", pg::flipDeg(),
           pg::flipPageOn(pg::FLIP_PAGE_SAVER) ? 1 : 0,
           pg::flipPageOn(pg::FLIP_PAGE_PET) ? 1 : 0, pg::flipSuppressed() ? 1 : 0);
    } else if (strncmp(line, "rot", 3) == 0) {
      /* 【底层探针，正常用不到】直接调框架的 ConfigManager 旋转接口，绕过 PgFlip 的意愿/落盘。
       * 只用来验证"框架本身能不能实时转"（本次就是这么验的：转完抓屏，384000 像素 0 差异）。
       * ⚠️ 它会把屏幕转到与 PgFlip 记录不一致的角度 —— 玩完记得 `flip off` 复位。 */
      int v = atoi(line + 3);
      CONFIGMANAGER->setScreenRotate(v);
      CONFIGMANAGER->setTouchRotate(v);
      LOGD("屏保 QA: [底层探针] rot=%d -> 回读 screen=%d touch=%d", v,
           CONFIGMANAGER->getScreenRotate(), CONFIGMANAGER->getTouchRotate());
    } else if (strncmp(line, "slowstat", 8) == 0) {
      /* 按需查"节拍与耗时"（用户说"卡"时先跑它）：
       *   间隔平均/最大  —— 正常应 ≈ SAVER_MS(25ms)，明显大 = UI 线程被占；
       *   本页单拍耗时   —— 明显大 = 本页自己太重；
       *   最多改动控件数 —— 一拍照改了多少个控件（工程上"一帧别改太多控件"的经验值）。 */
      LOGD("屏保节拍: 最近 %d 拍 间隔平均 %dms 最大 %dms（请求周期 %dms；实测框架给 ~20ms）| "
           "本页单拍最大耗时 %dms | 单拍最多改动 %d 个控件",
           sSlowN, sSlowN ? sSlowSum / sSlowN : 0, sSlowMax, SAVER_MS, sCostMax, sMutMax);
    } else if (strncmp(line, "slow", 4) == 0) {
      /* 放慢动画（抓中间帧用）：动画每 n x 50ms 推进一步。
       * 为什么需要它：金币飞出只有 400ms，而一次截屏往返就 ~450ms。 */
      sAnimDiv = atoi(line + 4);
      if (sAnimDiv < 1) sAnimDiv = 1;
      LOGD("屏保 QA: 动画放慢 x%d（每步 %d ms）", sAnimDiv, SAVER_MS * sAnimDiv);
    } else {
      LOGD("屏保 QA: 未知命令 '%s'", line);
    }
  }
}

/**
 * @brief 当界面构造时触发
 */
static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sSaverKeys);   // 任意键唤醒（见上面 SaverKeys 说明）
#endif
  sForceHHMM = -1;
  sQaAnim = false;
  sThemeForce = -1;
  sBgShown = -1;
  sLastSec = -1;
  sColonOn = true;
  sReady = false;
  sWarm = 12;   /* 进屏保后前 12 拍（300ms）持续重设文本：首次进入时窗口还没上屏，setText 会丢 */
  if (mColonOnPtr) mColonOnPtr->setVisible(true);
  if (mColonOffPtr) mColonOffPtr->setVisible(false);
  saverReadGeom();       // 必须先读：后面的几何都按它算
  int t[4];
  long ms;
  int hh, mm, ss;
  saverNow(&ms, &hh, &mm, &ss);
  t[0] = (hh / 10) % 10;
  t[1] = hh % 10;
  t[2] = (mm / 10) % 10;
  t[3] = mm % 10;
  saverSnap(t);
  saverUpdateDate();
  saverApplyBg(hh);
  saverQaSyncTag();      // 本会话的 QA 基线：只执行"进入屏保之后新推的"命令
  (void)mTextSaverHintPtr;   // 提示文案是静态的，logic 不用它（引用一下避免"未使用"告警）

  /* ★ 亮度联动：进屏保调暗（退出在 onUI_quit 里恢复，见 saverDimExit 的说明）。
   *   onUI_init 有两个可能的时机（app 启动 / 进屏保），按当前状态分流。 */
  if (EASYUICONTEXT->isScreensaverOn()) {
    saverDimEnter();
  } else {
    saverDimBootCheck();
  }

  LOGD("屏保砖块钟就绪：%d%d:%d%d（距整分 %ldms；进入屏保直接对齐，不播马里奥）",
       sCur[0], sCur[1], sCur[2], sCur[3], ms);
}

/**
 * @brief 当界面完全退出时触发
 */
static void onUI_quit() {
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sSaverKeys);
#endif
  /* ★ 亮度恢复必须在这里做（不是在 tick 里）—— 框架退出屏保后就停了本页定时器，
   *   见 saverDimExit 的注释：第一版写在 tick 里，唤醒后亮度永远停在暗档。 */
  saverDimExit();
  /* ★ 屏幕翻转同理：退出这一侧只能挂在这里。屏保关掉 = 本页不再是"环境页"，
   *   于是屏幕立刻恢复正向（若底下还压着机器人页，PgFlip 会保持 180° —— 它按"谁在显示"算）。 */
  pg::flipSetPageOn(pg::FLIP_PAGE_SAVER, false);
  pg::flipTick();
  LOGD_TRACE("");
}

/**
 * @brief 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) {
  LOGD_TRACE("");
}

/**
 * @brief 定时器回调函数, 不要在此函数中写耗时操作, 否则将影响UI刷新
 * @param id 当前所触发的定时器的id, 与注册时的id相同
 * @return true  继续运行当前定时器
 *         false 停止运行当前定时器
 */
static bool onUI_Timer(int id) {
  if (id != TIMER_SAVER) return true;
  saverTickBegin();   /* 慢帧计量：放在最前，屏保没显示时也量（那是"定时器本底"） */

  /* 屏保没显示时什么都不做（省电）：SysApp 实例在进程里常驻，定时器一旦注册就一直跑。
   * ⚠️ 判据用 `EASYUICONTEXT->isScreensaverOn()` 而**不是** `mscreensaverPtr->isShow()` ——
   *    实测后者在屏保已经显示时仍返回 false（用它会把整个 tick 挡掉：数字停在初值、
   *    日期一直显示占位符 `--`）。 */
  const bool saverOn = EASYUICONTEXT->isScreensaverOn();

  /* 0) 挂绳倒挂：把"屏保在不在显示"报给 PgFlip，再由它决定该不该把屏幕倒过来。
   *    ★★ 必须放在下面那句 `if (!saverOn) return true;` **之前**，而且退出那一侧还要
   *       额外挂 `onUI_quit`（屏保一退出框架就把本页定时器停掉，这里根本轮不到）。
   *       这条与"亮度恢复只能挂 onUI_quit"是同一个坑，2026-09-18 一次性踩明白。 */
  pg::flipSetPageOn(pg::FLIP_PAGE_SAVER, saverOn);
  pg::flipTick();

  /* 0) 亮度联动的**兜底**（正常路径走 onUI_init/onUI_quit）：
   *    万一某条路径没触发 onUI_quit（例如框架直接销毁页面），这里每拍补一次恢复。
   *    ⚠️ 屏保退出后框架会停掉本页定时器 ⇒ 这条兜底只在"定时器还在但状态已经变了"时有用。 */
  if (!saverOn && sSavedBright >= 0) saverDimExit();

  if (!saverOn) {
    sReady = false;
    sWarm = 12;     /* 下次进屏保重新走一遍"上屏后补设" */
    saverTickEnd();
    return true;
  }

  long msToFlip;
  int hh, mm, ss;
  saverNow(&msToFlip, &hh, &mm, &ss);

  /* 0) 本拍是否推进"动画步"（QA `slow` 可以放慢；正常 = 每拍都推进） */
  bool animStep = (++sAnimDivCnt >= sAnimDiv);
  if (animStep) sAnimDivCnt = 0;

  /* 1) 刚进屏保 / 首次 tick：对齐 + 补设文本（首次进入时窗口还没真正上屏，setText 会丢） */
  if (!sReady) {
    sReady = true;
    sWarm = 6;
    /* ★ 亮度调暗挂在这里（不是 onUI_init）：SysApp 实例是**跨"进/退屏保"复用**的，
     *   `onUI_init` 只在进程里第一次显示屏保时跑一次 —— 第二次进屏保不会再触发它
     *   （实测：第二次进屏保只走了本分支，`onUI_init` 一声不响）。
     *   而"刚进屏保的首拍"是**每次都会经过**的确定性时机。 */
    saverDimEnter();
    /* ★ 记下"进屏保时处在时间轴的哪一段"，供 saverMarioPose 决定这一分钟怎么演（见其说明）：
     *   正在"撞砖后 1.9 秒"的出场窗口里进来 -> 这一分钟干脆不让他出场。 */
    sEnterPhase = msToFlip;
    sMarioSkip = ((60000 - msToFlip) <= EXIT_MS);
    if (sMarioSkip) LOGD("屏保: 进屏保时正在撞砖后的出场窗口内 -> 这一分钟马里奥不出场");
    LOGD("屏保: 进入（对齐到 %02d:%02d，距整分 %ldms%s）", hh, mm, msToFlip,
         (msToFlip <= 3000) ? " · 处于前 3 秒窗口（走路会压缩）" : "");
  }
  saverApplyBg(hh);
  if (sWarm > 0) {
    --sWarm;
    int t[4] = {(hh / 10) % 10, hh % 10, (mm / 10) % 10, mm % 10};
    saverSnap(t);
    sDateText[0] = 0;
    sWeekText[0] = 0;
    saverUpdateDate();
  }

  /* 2) 秒变化：刷日期/星期（顺手做背景的自动切换） */
  if (ss != sLastSec) {
    sLastSec = ss;
    if (sForceHHMM < 0) saverUpdateDate();
  }

  /* 3) 时间变化 → 撞砖 + 金币弹出（每个数字位各弹各的；但"咚"只响一次） */
  {
    int t[4] = {(hh / 10) % 10, hh % 10, (mm / 10) % 10, mm % 10};
    bool hit = false;
    for (int i = 0; i < 4; ++i) {
      if (t[i] != sCur[i] && sWarm == 0) {
        saverPopStart(i, t[i]);
        hit = true;
      }
    }
    if (hit) saverPlayHit();
  }

  /* 4) 动画推进（金币 / 砖块位移 / 马里奥 / 金币音效）—— 只在"动画步"那一拍做 */
  if (animStep) {
    saverPopAdvance();
    saverTickSfx();
    ++sFrameCnt;
    int frame, x, y;
    if (saverMarioPose(msToFlip, &frame, &x, &y)) {
      saverMarioShow(frame, x, y);
    } else {
      saverMarioHide();
    }
    /* QA `mario`：合成相位往下走；走到 0 = 撞砖（时间前进 1 分钟，数字真变、金币真弹） */
    if (sQaAnim) {
      sQaPhase -= SAVER_MS;
      if (sQaPhase <= 0 && !sQaBumped) {
        sQaBumped = true;
        sForceHHMM = saverNextMinute(sForceHHMM);
        LOGD("屏保 QA: mario 撞砖 -> 强制时间前进到 %04d", sForceHHMM);
      }
      if (sQaPhase <= -EXIT_MS) {
        sQaAnim = false;
        LOGD("屏保 QA: mario 演示结束");
      }
    }
  }

  /* 5) 冒号闪烁：**用绝对时钟定相位**（500ms 亮/暗），不数拍。
   * ⚠️ 为什么不数拍：实测框架给 25ms 的定时器**实际约 20ms 一跳**（timer 分辨率），
   *    数拍的话闪烁周期会跟着这个偏差漂（实测会变成 400ms）。绝对时钟就没有这个问题。 */
  {
    bool on = ((saverMsNow() / 500) & 1) == 0;
    if (on != sColonOn) {
      sColonOn = on;
      if (mColonOnPtr) mColonOnPtr->setVisible(sColonOn);
      if (mColonOffPtr) mColonOffPtr->setVisible(!sColonOn);
    }
  }

  /* 6) QA 轮询：每 10 拍（500ms）读一次 */
  if (++sQaDiv >= 10) {
    sQaDiv = 0;
    saverPollQa();
  }
  saverTickEnd();   /* 本页本拍耗时（setPosition/setVisible 的同步成本都算在里面） */
  return true;
}

/**
 * @brief 有新的触摸事件时触发
 * @param ev 触摸事件
 * @return true 表示该触摸事件在此被拦截，系统不再将此触摸事件传递到控件上
 *         false 触摸事件将继续传递到控件上
 */
static bool onscreensaverActivityTouchEvent(const MotionEvent &ev) {
  /* ★ 这条日志是**判定"唤醒淡出能不能做"的实验**留下的判据：框架是"先收起屏保、再把触摸
   *   分发给应用"（实测：导航栏的全局监听收到的 x/y 就在 performScreensaverOff 之后），
   *   所以**唤醒那一次触摸根本不会走到本回调** ⇒ 想在"收起之前"播一段淡出动画拿不到时机。
   *   留着它：以后再想加唤醒动效，先看这条日志有没有出现。 */
  LOGD("屏保: 触摸 action=%d x=%d y=%d（能收到说明这次的触摸没被框架先收走屏保）",
       (int)ev.mActionStatus, ev.mX, ev.mY);
  switch (ev.mActionStatus) {
  case MotionEvent::E_ACTION_DOWN: // 触摸按下
    break;
  case MotionEvent::E_ACTION_MOVE: // 触摸滑动
    break;
  case MotionEvent::E_ACTION_UP: // 触摸抬起
    break;
  default:
    break;
  }
  return false;
}
