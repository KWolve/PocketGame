#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * timerLogic.cc - 秒表（独立 ftu：timer.ftu -> timerActivity）
 *
 * 为什么独立：主界面拆分口径 =「游戏不动，其余功能界面全部独立成 ftu」
 * （见 docs/page-split-plan.md）。本页是拆分后的第一个功能页。
 *
 * 本文件刻意保持"薄"：定时器的状态机在 src/core/PgTools.cpp 的 ToolStopwatch
 * （Game 子类，对外只暴露 uiText/uiButton/onUiButton/uiButtonStyle/uiAccent），
 * 页面侧只做四件事：建外壳、每帧转发、按钮转发、按键转发。
 * 这些样板全在 src/ui/ToolPage.h（8 个工具页共用一份，避免各自漂）。
 *
 * 按键（本板实体键）：103 音量- / 105 音量+ / 108 暂停；108 长按 ≥700ms = 返回列表。
 * ⚠️ 本板 gpio-keys 无 autorepeat ⇒ 长按必须在应用层按时长判（ToolPage 里做了）。
 *
 * 自检通道：/tmp/pg_timercmd（独立页必须有自己的通道 —— 后台 Activity 的
 * 定时器不跑，主界面的 /tmp/pg_autostart 在这里是失效的，wifi/remote 已踩过）：
 *   btn <0..7>   点第 i 个按钮（走真实 onClick 路径）
 *   key a        等价按一下暂停键（短按）
 *   dump         打印当前主数值/状态/按钮标签
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/PgGame.h"
#include "core/PgGames.h"
#include "entry/EasyUIContext.h"
#include "ui/ToolPage.h"
#include "utils/Log.h"

namespace {

const int TIMER_TICK = 1;
const int TICK_MS = 50;  // 20fps：秒表要百分秒精度，50ms 一刷足够（显示 0.01s）

pg::ToolPage sPage;

/* 8 个按钮的指针表。⚠️ 不能写成 `ZKButton *const t[8] = {mBtnTm0Ptr, ...}`：
 * 那些 mXxxPtr 是 fun 在 onCreate 里才赋值的，静态初始化期全是 NULL（屏保那边
 * 用的是"存指针的地址、运行时再解引用"，这里等价地由 fillBtns() 在 onUI_init 时取）。 */
ZKButton *sBtns[8] = {0, 0, 0, 0, 0, 0, 0, 0};

void fillBtns() {
  ZKButton **p[8] = {&mBtnTm0Ptr, &mBtnTm1Ptr, &mBtnTm2Ptr, &mBtnTm3Ptr,
                     &mBtnTm4Ptr, &mBtnTm5Ptr, &mBtnTm6Ptr, &mBtnTm7Ptr};
  for (int i = 0; i < 8; ++i) sBtns[i] = *p[i];
}

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 按 id 找槽位：**不要写死数字** —— kAppTable 的 slot 是存档索引（最高分/分类），
 * 以后加应用时行号会变，写死就会串到别的应用上。 */
int slotById(const char *id) {
  for (int i = 0; i < pg::appCount(); ++i)
    if (strcmp(pg::gameEntry(i).id, id) == 0) return i;
  return -1;
}

/* 按键监听：交给外壳判（长按/音量/短按），本文件只负责"关页"这个动作 */
class TmKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN)
      return sPage.keyDown(ke.mKeyCode, t) != pg::TOOL_KEY_IGNORE;
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;  // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    pg::ToolKeyResult r = sPage.keyUp(ke.mKeyCode, t);
    if (r == pg::TOOL_KEY_EXIT) {
      EASYUICONTEXT->closeActivity("timerActivity");
      return true;
    }
    return r != pg::TOOL_KEY_IGNORE;
  }
};
TmKeys sKeys;

/* -------------------- 自检通道 /tmp/pg_timercmd -------------------- */
char sQaTag[512] = {0};

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_timercmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_timercmd", "r");
  if (!f) return;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (n == 0) return;
  if (strncmp(buf, sQaTag, sizeof(sQaTag) - 1) == 0) return;  // 整份内容去重
  strncpy(sQaTag, buf, sizeof(sQaTag) - 1);
  sQaTag[sizeof(sQaTag) - 1] = 0;

  char *save = 0;
  for (char *line = strtok_r(buf, "\r\n", &save); line;
       line = strtok_r(0, "\r\n", &save)) {
    while (*line == ' ' || *line == '\t') ++line;
    char *h = strchr(line, '#');  // 行内注释（习惯写 `btn 0 #1` 保证内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    if (strncmp(line, "btn ", 4) == 0) {
      /* 除了点按钮，还**核对 caption->ID 映射**（最容易错的就是它：
       * 页面 caption 改动后 ID 会重新分配，回调名对不上就静默无反应）。 */
      int i = atoi(line + 4);
      static const int kExpect[8] = {ID_TIMER_BtnTm0, ID_TIMER_BtnTm1,
                                     ID_TIMER_BtnTm2, ID_TIMER_BtnTm3,
                                     ID_TIMER_BtnTm4, ID_TIMER_BtnTm5,
                                     ID_TIMER_BtnTm6, ID_TIMER_BtnTm7};
      if (i < 0 || i >= 8) {
        LOGD("定时器 QA: 按钮 %d 越界", i);
        continue;
      }
      LOGD("定时器 QA: 按钮 %d id=%d（期望 %d）", i, sBtns[i] ? sBtns[i]->getID() : -1,
           kExpect[i]);
      sPage.button(i);
    } else if (strncmp(line, "key", 3) == 0) {
      LOGD("定时器 QA: 短按暂停键");
      sPage.keyDown(108, nowMs());
      sPage.keyUp(108, nowMs() + 60);
    } else if (strncmp(line, "dump", 4) == 0) {
      char b0[64], b1[64], b2[64];
      pg::Game *g = sPage.game();
      LOGD("定时器 QA: 主='%s' 状态='%s' 副='%s' 按钮=[%s|%s|%s]",
           g ? g->uiText(0, b0, sizeof(b0)) : "-",
           g ? g->uiText(1, b1, sizeof(b1)) : "-",
           g ? g->uiText(2, b2, sizeof(b2)) : "-", g ? g->uiButton(0) : "-",
           g ? g->uiButton(1) : "-", g ? g->uiButton(2) : "-");
    } else {
      LOGD("定时器 QA: 未知命令 '%s'", line);
    }
  }
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

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
  fillBtns();
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  pg::ToolPageBinds b;
  b.slot = slotById("timer");
  b.appName = "timerActivity";
  b.title = mTextTmTitlePtr;
  b.mainText = mTextTmMainPtr;
  b.phase = mTextTmPhasePtr;
  b.sub = mTextTmSubPtr;
  b.hint = mTextTmHintPtr;
  b.keyBar = mTextTmKeyBarPtr;
  b.accentBar = mBarTmPtr;
  b.btns = sBtns;
  b.btnCount = 8;
  sPage.init(b);
  sPage.show();       // 本页就是前台（onUI_show 也会调，重复调用是幂等的）
  qaSyncTag();        // 自检基线：只执行"进页之后新推的"命令
  LOGD("定时器页就绪（slot=%d）", b.slot);
}

static void onUI_intent(const Intent *intent) { (void)intent; }
static void onUI_show() {
  LOGD("定时器: onUI_show");
  sPage.show();
}
static void onUI_hide() {
  LOGD("定时器: onUI_hide");
  sPage.hide();
}
static void onUI_quit() {
  LOGD("定时器: onUI_quit");
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeys);   // 定时器由框架随页面销毁，无需手动反注册
#endif
  sPage.quit();
}
static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

/**
 * @brief 定时器回调：不要在这里做耗时操作
 */
static bool onUI_Timer(int id) {
  if (id != TIMER_TICK) return false;
  sPage.tick(TICK_MS);
  qaPoll();
  return true;
}

/**
 * @brief 触摸事件（本页没有需要拦截的触摸：按钮都是原生控件）
 */
static bool ontimerActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;
}

/* ---- 8 个按钮：统一转发到外壳（标签/显隐/配色都由 ToolPage 按 Game 的状态算）---- */
static bool onButtonClick_BtnTm0(ZKButton *p) { (void)p; sPage.button(0); return true; }
static bool onButtonClick_BtnTm1(ZKButton *p) { (void)p; sPage.button(1); return true; }
static bool onButtonClick_BtnTm2(ZKButton *p) { (void)p; sPage.button(2); return true; }
static bool onButtonClick_BtnTm3(ZKButton *p) { (void)p; sPage.button(3); return true; }
static bool onButtonClick_BtnTm4(ZKButton *p) { (void)p; sPage.button(4); return true; }
static bool onButtonClick_BtnTm5(ZKButton *p) { (void)p; sPage.button(5); return true; }
static bool onButtonClick_BtnTm6(ZKButton *p) { (void)p; sPage.button(6); return true; }
static bool onButtonClick_BtnTm7(ZKButton *p) { (void)p; sPage.button(7); return true; }
