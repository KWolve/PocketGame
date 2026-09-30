#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * statusbar.cc - 全局状态栏（SysApp：statusbar.ftu -> statusbar，APP_TYPE_SYS_STATUSBAR）
 *
 * ★ 为什么有这个页（2026-09-14 用户要求）：
 *   「音量弹框做成全局的，可以用状态栏实现。现在的音量弹框在其他界面看不到。」
 *   原来主界面（main.ftu）和 WiFi 页（wifi.ftu）各有一份 WinVolume，工具页/时钟套件/IPTV
 *   上按音量键**只有声音、没有界面**。现在收成**一份全局 OSD**放进状态栏 ——
 *   状态栏是 APP_TYPE_SYS_STATUSBAR 的 SysApp，**悬浮在所有页面之上**，
 *   所以任何 ftu 里按音量键都看得见。
 *
 * ⚠️ 文件名必须叫 `statusbar.cc`（**不是** statusbarLogic.cc）：fun 对 **sysapp 页**
 *    （screensaver / navibar / statusbar）约定的逻辑文件就是 `<页名>.cc`（见 screensaver.cc）。
 *    名字放错会有两个后果：fun 会再生成一份模板 `statusbar.cc`，而**两份都定义 onUI_init**
 *    时由注册顺序决定谁生效 —— 实测模板那份赢了，自己写的逻辑完全不执行（踩过）。
 *
 * 数据流（**唯一收口点**，不各页各写一套）：
 *   任意页面按音量键 / QA → `pg::volumeStepGlobal(±1)`
 *        → （mainLogic 注入的钩子）真改音量
 *        → PgAudio **广播** `setVolumeNotifyHook(fn)` 注册的回调   ← 本页注册的就是它
 *        → 本页定时器里更新控件 + showWnd，1.6s 后自动隐藏
 *
 * ⚠️ 为什么回调里只记一个数、控件在定时器里更新：回调可能来自非 UI 线程，
 *    而"改控件"必须在 UI 线程（工程既有铁律："控件操作只能在主线程"）。
 *    记一个 volatile int 成本可忽略，换来的是不用关心调用方是谁。
 *
 * ★ 显隐策略（2026-09-15 定稿）：**按需显隐** —— 平时 `hideStatusBar()`（窗口栈里没有它
 *   ⇒ 不挡任何触摸），音量变化才 `showStatusBar()` + 显面板；超时 / **点面板外** 立刻收起。
 *   ⚠️ 不要改成"常显 + 触摸穿透"：实测整屏 topmost 窗口设 `touchable=false` 甚至
 *      `setTouchPass(true)` 都放不过触摸（整机点不动、控件级回调零输出）—— 血案见 showOsd。
 *   ⚠️ 状态栏被加载后框架默认把它标记为**显示** ⇒ 必须靠 `onUI_Timer` 里的"收敛收起"
 *      把它按住（onUI_init 太早、onUI_show 不会被回调 —— 都是实测结论）。
 *
 * ⚠️ 常显由主界面主循环收敛保证（mainLogic 的 TIMER_LOOP 里每约 1 秒查一次
 *    `isStatusBarShow()`）——**不要**在 `Main.cpp::onEasyUIInit` 里调 `showStatusBar()`，
 *    那时框架的 SysApp 工厂还没就绪，调了不生效（实测）。
 *
 * 自检通道：/tmp/pg_statusbarcmd（独立 SysApp 必须有自己的通道 —— 主界面的
 *   /tmp/pg_autostart 在这里读不到）：
 *   vol <0-100> [holdMs]   手动弹出音量面板（holdMs 用于抓帧验收，默认 1600ms）
 *   hide                   立刻收起
 *   dump                   打印当前显示状态与数值
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "entry/EasyUIContext.h"
#include "platform/PgAudio.h"   // pg::setVolumeNotifyHook（音量变化广播）
#include "utils/Log.h"

namespace {

const int TIMER_TICK = 1;
const int TICK_MS = 100;        // 100ms：OSD 显隐够跟手，又不至于刷屏
const int kOsdHoldMs = 1600;    // 与改造前主界面/WiFi 页的停留时长一致

volatile int sPendingPct = -1;  // 有新音量待显示（-1 = 无）；见文件头"为什么只记一个数"
long sHideMs = 0;               // >0 = 面板显示中，到点隐藏
int sCurPct = -2;               // 面板上当前显示的值（-2 = 未知，变化检测用）
char sQaTag[512] = {0};

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 把数值写到控件上（变化检测：值没变就不写，避免重绘） */
void updatePanel(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (sCurPct != pct) {
    sCurPct = pct;
    if (mBarSbVolPtr) {
      mBarSbVolPtr->setMax(100);
      mBarSbVolPtr->setProgress(pct);
    }
    if (mTextSbVolPctPtr) {
      char b[16];
      snprintf(b, sizeof(b), "%d%%", pct);
      mTextSbVolPctPtr->setText(b);
    }
  }
}

/* 静音状态的视觉同步（2026-09-17 用户需求改版）：
 *   · 未静音 → 显示「喇叭」图标（`BtnSbVolIcon`）、标题「音量」；
 *   · 静音中 → 显示「喇叭带斜杠」图标（`BtnSbVolIconMute`）、标题「静音」。
 *
 * ★★ 为什么是**两个按钮叠在同一格、按状态显示其一**，而不是一个按钮换图：
 *   `platform/PgSkin.h` 实测记着 —— **运行时 `setBackgroundPic()` 这条路径不保留 alpha**
 *   （透明像素会被渲染成纯白）。图标是透明底的线框 PNG，用那条路换图会得到一整块白方块。
 *   所以照主界面应用网格图标的老办法（mainLogic 的 syncRowIcon）：两个都建好、只显示一个。
 *
 * ⚠️ 真静音走 `pg::setMutedGlobal()`（codec 输出开关，可逆），不是把音量调到 0 ——
 *   原因见 PgAudio.h；"调整音量自动解除静音"也在那边
 *   （按键走 volumeStepGlobal、拖动走 setVolumePercentGlobal，两条路都会先解除）。 */
void updateMuteVisual() {
  static int sLastMute = -1;
  const int m = pg::isMutedGlobal() ? 1 : 0;
  if (m == sLastMute) return;     // 变化检测：不写控件就不重绘
  sLastMute = m;
  /* 显隐 + 触摸一起切：藏起来那枚**必须同时关掉 touchable**，
   * 否则它会继续吃掉这一格上的点击（两个控件同位置，框架按 z 序命中）。 */
  if (mBtnSbVolIconPtr) {
    mBtnSbVolIconPtr->setVisible(!m);
    mBtnSbVolIconPtr->setTouchable(!m);
  }
  if (mBtnSbVolIconMutePtr) {
    mBtnSbVolIconMutePtr->setVisible(m);
    mBtnSbVolIconMutePtr->setTouchable(m);
  }
  if (mTextSbVolTitlePtr) mTextSbVolTitlePtr->setText(m ? "静音" : "音量");
}

/* ★★ 音量 OSD 的显隐 = **整个状态栏的显隐**（2026-09-15 定稿，用户指出的正解）
 *
 * 用框架接口 `EASYUICONTEXT->showStatusBar()/hideStatusBar()` 切换，而不是只显隐面板窗口。
 *
 * 为什么必须这样（血案：整机所有页面点不动）：
 *   状态栏根窗口是 **480x800 整屏 + topmost**，它只要"显示着"就盖在所有页面之上。
 *   实测（含注入 + 手指）：只设 `touchable=false`、甚至再补 `setTouchPass(true)`，
 *   **都挡不住** —— 触摸只到页面级（全局监听能打印坐标），控件级一条都不来
 *   （控件级探针 `PGSPY` 零输出）。⇒ 唯一可靠的办法是**平时不让它显示**。
 *
 * 为什么按需显隐可行（实测）：
 *   `hideStatusBar()` 之后 `isStatusBarShow()=0`，但**页面、定时器、QA 通道都还活着**
 *   （推 `dump` 立刻有响应），音量变化的广播回调也照样触发 ⇒ 闭环成立：
 *     音量变化 → showOsd() → showStatusBar() + 显面板 → 超时/点外面 → hideOsd()。
 */
static bool sWantOsd = false;   // 是否"该显示音量 OSD"（= 状态栏该显示）

void showOsd(int pct, int holdMs) {
  updatePanel(pct);
  sWantOsd = true;
  EASYUICONTEXT->showStatusBar();                    // ① 先把整屏浮层显示出来
  if (mWinSbVolumePtr) mWinSbVolumePtr->showWnd();   // ② 再显面板
  sHideMs = nowMs() + (holdMs > 0 ? holdMs : kOsdHoldMs);
  LOGD("状态栏: 音量 OSD %d%%（%dms 后自动收起）", pct, holdMs > 0 ? holdMs : kOsdHoldMs);
}

void hideOsd(const char *why) {
  sWantOsd = false;
  if (mWinSbVolumePtr) mWinSbVolumePtr->hideWnd();
  EASYUICONTEXT->hideStatusBar();   // ★ 收起整屏浮层 —— 从此不再挡任何触摸
  sHideMs = 0;
  LOGD("状态栏: 收起音量 OSD（%s）", why ? why : "");
}

/* PgAudio 的音量变化广播 → 这里只记数（线程安全考虑，见文件头） */
void onVolumeChanged(int pct) { sPendingPct = pct; }

/* -------------------- 自检通道 /tmp/pg_statusbarcmd -------------------- */
void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_statusbarcmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_statusbarcmd", "r");
  if (!f) return;
  char buf[512];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (n == 0) return;
  if (strncmp(buf, sQaTag, sizeof(sQaTag) - 1) == 0) return;   // 整份内容去重
  strncpy(sQaTag, buf, sizeof(sQaTag) - 1);
  sQaTag[sizeof(sQaTag) - 1] = 0;

  char *save = 0;
  for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(0, "\r\n", &save)) {
    while (*line == ' ' || *line == '\t') ++line;
    char *h = strchr(line, '#');     // 行内注释（习惯写 `vol 60 #1` 保证内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;

    if (strncmp(line, "vol ", 4) == 0) {
      int pct = atoi(line + 4);
      int hold = 0;
      const char *sp = strchr(line + 4, ' ');
      if (sp) hold = atoi(sp + 1);
      LOGD("状态栏 QA: vol %d hold=%d", pct, hold);
      showOsd(pct, hold);
    } else if (strncmp(line, "hide", 4) == 0) {
      LOGD("状态栏 QA: hide");
      hideOsd("QA hide");
    } else if (strncmp(line, "dump", 4) == 0) {
      LOGD("状态栏 QA: 面板=%s 值=%d 剩余=%ldms 状态栏=%s",
           (mWinSbVolumePtr && mWinSbVolumePtr->isWndShow()) ? "显示" : "隐藏", sCurPct,
           sHideMs ? (sHideMs - nowMs()) : 0,
           EASYUICONTEXT->isStatusBarShow() ? "显示" : "隐藏");
    } else {
      LOGD("状态栏 QA: 未知命令 '%s'", line);
    }
  }
}

}  // namespace

/**
 * 注册定时器
 */
static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_TICK, TICK_MS},
};

/**
 * @brief 当界面构造时触发
 */
static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
#endif
  /* 默认隐藏：状态栏**不放常驻内容**（否则挡住下面页面的标题栏），只在音量变化时弹面板 */
  if (mWinSbVolumePtr) mWinSbVolumePtr->hideWnd();

  /* ★★ 显隐策略 = **按需显隐**（2026-09-15 定稿，理由见 showOsd 上的注释）：
   *   平时隐藏（窗口栈里没有状态栏 ⇒ 不挡任何触摸），音量变化才显示。
   *   ⚠️ 不要回到"常显 + 触摸穿透"那条路：整屏 topmost 窗口设 `touchable=false`、
   *      甚至 `setTouchPass(true)` 都放不过触摸（实测整机点不动、控件级回调零输出）。 */
  sWantOsd = false;
  sCurPct = -2;
  /* ★★ 显式注册全局触摸监听（2026-09-15 实测必需）：
   *   本页是 SysApp（`EventApp<BaseApp>`）—— 与普通 Activity 不同，**它收不到全局触摸**
   *   （实测：面板显示时点屏幕，`onstatusbarActivityTouchEvent` 里的探针一条都不打印，
   *     而 mainActivity 的同类回调照常有坐标）。而"点面板外收起"正好依赖这个回调
   *   （用户要求 2026-09-15：「statusbar 显示的时候，触摸音量条外面就直接隐藏掉」）。
   *   生成的 `ui_statusbar.h` 只在 DESTROY 时 unregister（没有配对 register）⇒ 这里补上。
   *   幂等说明：即使基类某条路径也注册过，重复注册只会让回调多跑一次 ——
   *   我们的判断是"点外面才收"，重复调用无副作用。 */
  if (mstatusbarPtr) {
    EASYUICONTEXT->registerGlobalTouchListener(mstatusbarPtr);
    LOGD("状态栏: 已注册全局触摸监听（点面板外收起用）");
  }
  /* ★ 注册音量变化广播：全工程音量操作都走 pg::volumeStepGlobal，所以这一处就够 */
  pg::setVolumeNotifyHook(onVolumeChanged);
  qaSyncTag();   // 自检基线：只执行"本次启动之后新推的"命令
  LOGD("状态栏就绪（全局音量 OSD；任何 ftu 里按音量键都走这里）");
}

static void onUI_intent(const Intent *intent) {
  (void)intent;
}

static void onUI_show() {
  LOGD("状态栏: onUI_show（sWantOsd=%d）", sWantOsd ? 1 : 0);
  /* 兜底：状态栏只该在"要显示音量 OSD"时出现。
   * 框架可能在启动/其它时机把它 show 出来（那时它是整屏浮层 ⇒ 挡住所有页面触摸），
   * 这里立刻收掉。⚠️ sWantOsd 必须**先于** showStatusBar() 置位（见 showOsd 的顺序）。 */
  if (!sWantOsd) EASYUICONTEXT->hideStatusBar();
}

static void onUI_hide() { LOGD("状态栏: onUI_hide"); }

/**
 * @brief 当界面完全退出时触发
 */
static void onUI_quit() {
  LOGD("状态栏: onUI_quit");
  pg::setVolumeNotifyHook(0);   // 反注册（hideStatusBar 后不该再往已销毁的控件写）
}

/**
 * @brief 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) {
  (void)data;
}

/**
 * @brief 定时器回调函数, 不要在此函数中写耗时操作, 否则将影响UI刷新
 * @param id 当前所触发定时器的id, 与注册时的id相同
 * @return true 继续运行当前定时器
 */
static bool onUI_Timer(int id) {
  if (id != TIMER_TICK) return true;

  /* ★★ 收敛：**没有音量 OSD 要显示 ⇒ 状态栏必须是隐藏的**（2026-09-15 定稿）
   *
   * 为什么必须放在这里（实测）：
   *   · 状态栏被 `loadStatusBar()` 加载后，框架把它标记为**显示**
   *     （QA `dump` 实测：启动后 `isStatusBarShow()=1`）；
   *   · `onUI_init()` 里 hide 太早（框架的"显示"标记还在后面）；
   *   · `onUI_show()` 实测**根本没被回调**（日志里一条都没有）。
   *   ⇒ 只有定时器能兜住 —— 它在页面加载后就在跑，而且 `hideStatusBar()` 之后**照样在跑**
   *     （实测：hide 后推 `dump` 立刻有响应，音量广播回调也照样触发）。
   *   代价：加载后 ~100ms 内状态栏还是显示的（这 100ms 会吃掉触摸，可忽略）。
   *
   * ⚠️ 方向别写反：以前是"每秒保证它显示"（常显），那会让**整机所有页面的控件都点不动**
   *    （实测：状态栏显示时注入/手指都只到页面级，`PGSPY` 控件级探针零输出；
   *      一旦 hideStatusBar() 立刻恢复 `list click`）。 */
  if (!sWantOsd && EASYUICONTEXT->isStatusBarShow()) {
    LOGD("状态栏: 收敛收起（当前无音量 OSD 要显示）");
    EASYUICONTEXT->hideStatusBar();
  }

  if (sPendingPct >= 0) {          // 音量变了 → 弹出面板（写控件在 UI 线程做，见文件头）
    int p = sPendingPct;
    sPendingPct = -1;
    showOsd(p, 0);
  }
  if (sHideMs && nowMs() >= sHideMs) hideOsd("1.6s 超时");
  updateMuteVisual();   // 静音状态可能被别处改（设置页 / 长按音量键）
  qaPoll();
  return true;
}

/**
 * @brief 有新的触摸事件时触发
 *
 * ★ 用户要求（2026-09-15）：「statusbar 显示的时候，触摸音量条外面就直接隐藏掉」。
 *   实现依据：这个回调是**全局触摸监听**（ITouchListener），它在任何页面/浮层都会收到
 *   事件（不受窗口命中影响 —— 血案排查时实测：控件级收不到时，页面级照常有坐标）。
 *   所以这里能可靠地判断"触点是否落在音量面板之外"。
 *   ⚠️ 只在 DOWN 处理（MOVE 会连续触发，没必要）。
 *   ⚠️ 返回 false：不要去吞事件（该不该传给下层由窗口命中决定，不由这里决定）。
 */
static bool onstatusbarActivityTouchEvent(const MotionEvent &ev) {
  /* 诊断（2026-09-16）：面板上的"静音"按钮点了没反应，先确认触摸到底有没有派发到本页。
   * ⚠️ 排查完可以删；留着也无害（只在**首次**收到触摸时打一条）。 */
  static int sTouchSeen = 0;
  if (!sTouchSeen) {
    sTouchSeen = 1;
    LOGD("状态栏: 首次收到触摸 action=%d x=%d y=%d osd=%d", (int)ev.mActionStatus, ev.mX,
         ev.mY, sWantOsd ? 1 : 0);
  }
  if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN && sWantOsd && mWinSbVolumePtr &&
      mWinSbVolumePtr->isWndShow()) {
    const LayoutPosition &p = mWinSbVolumePtr->getPosition();
    /* ★★ 坐标必须换算成**屏幕**坐标再比（2026-09-16 血案）：
     *   控件 `getPosition()` 给的是**窗口内**坐标（面板现在是 (0,0,240,160)），
     *   而 `MotionEvent` 给的是**屏幕**坐标。窗口收成 240x160 之后，
     *   拿 0..160 去比屏幕上的 y=646 ⇒ 面板内的触摸被误判成"面板外"，
     *   面板立刻收起、按钮更是永远点不着（现象："点了没反应"，日志却是
     *   `触摸面板外 (186,646) → 收起`）。
     *   所以这里加上窗口在屏幕上的位置 = gen_ui.py 给 statusbar 根节点头的
     *   position（两处必须一致，下面两个常量就是那份值）。
     *   ⚠️ 2026-09-16 第二轮：面板 240x160 → **240x112**，位置 (120,520) → **(120,544)**。 */
    const int kPanelWinLeft = 120, kPanelWinTop = 544;   // == tools/gen_ui.py 的 statusbar patch
    const int l = kPanelWinLeft + p.mLeft;
    const int t = kPanelWinTop + p.mTop;
    bool inside = (ev.mX >= l && ev.mX < l + p.mWidth &&
                   ev.mY >= t && ev.mY < t + p.mHeight);
    if (!inside) {
      LOGD("状态栏: 触摸面板外 (%d,%d) → 收起", ev.mX, ev.mY);
      hideOsd("点面板外");
    }
  }
  return false;
}

/**
 * @brief 音量条拖动回调。
 *   ⚠️ 状态栏根节点 touchable=false（整屏触摸穿透），所以这个回调**实际不会被触发** ——
 *      但 fun 按控件（BarSbVol）生成了绑定，必须给个定义，否则链接期 undefined reference。
 *      （语义沿用改造前 WiFi 页那份：只更新百分比字样 + 刷新自动收起计时。
 *        音量本身仍由音量键/全局钩子改 —— 这个面板是"显示"，不是"调节器"。）
 */
/**
 * @brief 音量图标 = 静音开关（2026-09-17 用户需求）
 *
 * 用户原话：「进度条左侧放一个音量图标，点一下图标变成静音的样子，再点一下解除静音
 *   （= 现在的音效开关），调整音量自动解除静音」。
 *   ⇒ 点图标 = 在"静音 / 有声"之间切；两态图标由 updateMuteVisual() 二选一显示。
 *
 * ★ 图标按钮**能点**，是因为 2026-09-16 把 statusbar 的窗口从 480x800 收成了 240x160
 *   （整屏浮层会吃掉全机控件级触摸，连自己的按钮都点不到；见 ui/statusbar.html 的说明）。
 */
static bool toggleMuteFromOsd(const char *who) {
  const bool want = !pg::isMutedGlobal();
  const bool ok = pg::setMutedGlobal(want);
  LOGD("状态栏: %s -> %s（%s）", who, want ? "静音" : "解除静音", ok ? "成功" : "失败");
  updateMuteVisual();
  /* 面板继续显示一会儿，让用户看到状态变化（1.6s 自动收起） */
  sHideMs = nowMs() + kOsdHoldMs;
  return true;
}

static bool onButtonClick_BtnSbVolIcon(ZKButton *p) {
  (void)p;
  return toggleMuteFromOsd("点音量图标");
}

static bool onButtonClick_BtnSbVolIconMute(ZKButton *p) {
  (void)p;
  return toggleMuteFromOsd("点静音图标");
}

static void onProgressChanged_BarSbVol(ZKSeekBar *pSeekBar, int progress) {
  (void)pSeekBar;
  if (mTextSbVolPctPtr) {
    char b[16];
    snprintf(b, sizeof(b), "%d%%", progress);
    mTextSbVolPctPtr->setText(b);
  }
  sCurPct = progress;
  sHideMs = nowMs() + kOsdHoldMs;
}static bool onButtonClick_BtnSbPanelBg(ZKButton* pButton) {
  LOGD_TRACE("BtnSbPanelBg click");
  return false;
}


