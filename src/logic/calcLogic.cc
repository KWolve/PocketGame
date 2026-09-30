#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * calcLogic.cc - 计算器（独立 ftu：calc.ftu -> calcActivity）
 *
 * 拆分口径见 docs/page-split-plan.md：游戏不动，其余功能界面全部独立成 ftu。
 * 本页只做四件事：建外壳（pg::ToolPage）、每帧同步、按钮转发、按键转发；
 * 状态机在 src/core/PgTools.cpp 的 Game 子类里，页面本身不含业务逻辑。
 *
 * 按键：108 短按 = 开始/下一步；108 长按 ≥700ms = 返回列表；103/105 = 音量±。
 * ⚠️ 本板 gpio-keys 无 autorepeat ⇒ 长按只能应用层按时长判（ToolPage 里做的）。
 * 自检通道：/tmp/pg_calccmd（btn <i> / key / dump）
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
const int TICK_MS = 50;

pg::ToolPage sPage;

/* 按钮指针表。⚠️ mXxxPtr 由 fun 在 onCreate 里赋值，静态初始化期全是 NULL，
 * 所以这里在 onUI_init 时再取（屏保那边同样是"存地址、运行时解引用"）。 */
const int kBtnCount = 20;
ZKButton *sBtns[kBtnCount] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void fillBtns() {
  ZKButton **p[kBtnCount] = {&mBtnK0Ptr, &mBtnK1Ptr, &mBtnK2Ptr, &mBtnK3Ptr, &mBtnK4Ptr, &mBtnK5Ptr, &mBtnK6Ptr, &mBtnK7Ptr, &mBtnK8Ptr, &mBtnK9Ptr, &mBtnK10Ptr, &mBtnK11Ptr, &mBtnK12Ptr, &mBtnK13Ptr, &mBtnK14Ptr, &mBtnK15Ptr, &mBtnK16Ptr, &mBtnK17Ptr, &mBtnK18Ptr, &mBtnK19Ptr};
  for (int i = 0; i < kBtnCount; ++i) sBtns[i] = *p[i];
}

const int kBtnIds[kBtnCount] = {ID_CALC_BtnK0, ID_CALC_BtnK1, ID_CALC_BtnK2, ID_CALC_BtnK3, ID_CALC_BtnK4, ID_CALC_BtnK5, ID_CALC_BtnK6, ID_CALC_BtnK7, ID_CALC_BtnK8, ID_CALC_BtnK9, ID_CALC_BtnK10, ID_CALC_BtnK11, ID_CALC_BtnK12, ID_CALC_BtnK13, ID_CALC_BtnK14, ID_CALC_BtnK15, ID_CALC_BtnK16, ID_CALC_BtnK17, ID_CALC_BtnK18, ID_CALC_BtnK19};

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 按 id 找槽位（不写死数字：slot 是存档索引，加应用时行号会变） */
int slotById(const char *id) {
  for (int i = 0; i < pg::appCount(); ++i)
    if (strcmp(pg::gameEntry(i).id, id) == 0) return i;
  return -1;
}

class PageKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN)
      return sPage.keyDown(ke.mKeyCode, t) != pg::TOOL_KEY_IGNORE;
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;  // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    pg::ToolKeyResult r = sPage.keyUp(ke.mKeyCode, t);
    if (r == pg::TOOL_KEY_EXIT) {
      EASYUICONTEXT->closeActivity("calcActivity");
      return true;
    }
    return r != pg::TOOL_KEY_IGNORE;
  }
};
PageKeys sKeys;

char sQaTag[512] = {0};

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_calccmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_calccmd", "r");
  if (!f) return;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (n == 0) return;
  if (strncmp(buf, sQaTag, sizeof(sQaTag) - 1) == 0) return;
  strncpy(sQaTag, buf, sizeof(sQaTag) - 1);
  sQaTag[sizeof(sQaTag) - 1] = 0;

  char *save = 0;
  for (char *line = strtok_r(buf, "\r\n", &save); line;
       line = strtok_r(0, "\r\n", &save)) {
    while (*line == ' ' || *line == '\t') ++line;
    char *h = strchr(line, '#');
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    if (strncmp(line, "btn ", 4) == 0) {
      /* 除了点按钮，还核对 caption->ID 映射（最容易错的就是它） */
      int i = atoi(line + 4);
      if (i < 0 || i >= kBtnCount) {
        LOGD("计算器 QA: 按钮 %d 越界", i);
        continue;
      }
      LOGD("计算器 QA: 按钮 %d id=%d（期望 %d）", i, sBtns[i] ? sBtns[i]->getID() : -1,
           kBtnIds[i]);
      sPage.button(i);
    } else if (strncmp(line, "key", 3) == 0) {
      LOGD("计算器 QA: 短按暂停键");
      sPage.keyDown(108, nowMs());
      sPage.keyUp(108, nowMs() + 60);
    } else if (strncmp(line, "dump", 4) == 0) {
      char b0[64], b1[64], b2[64];
      pg::Game *g = sPage.game();
      LOGD("计算器 QA: 主='%s' 状态='%s' 副='%s' 按钮0='%s'",
           g ? g->uiText(0, b0, sizeof(b0)) : "-",
           g ? g->uiText(1, b1, sizeof(b1)) : "-",
           g ? g->uiText(2, b2, sizeof(b2)) : "-", g ? g->uiButton(0) : "-");
    } else {
      LOGD("计算器 QA: 未知命令 '%s'", line);
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
  fillBtns();
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  pg::ToolPageBinds b;
  b.slot = slotById("calc");
  b.appName = "calcActivity";
  b.title = mTextCalcTitlePtr;
  b.mainText = mTextCalcMainPtr;
  b.phase = mTextCalcPhasePtr;
  b.sub = mTextCalcSubPtr;
  b.hint = mTextCalcHintPtr;
  b.keyBar = mTextCalcKeyBarPtr;
  b.accentBar = mBarCalcPtr;
  b.btns = sBtns;
  b.btnCount = kBtnCount;
  sPage.init(b);
  sPage.show();
  qaSyncTag();   // 自检基线：只执行"进页之后新推的"命令
  LOGD("计算器页就绪（slot=%d）", b.slot);
}

static void onUI_intent(const Intent *intent) { (void)intent; }
static void onUI_show() { sPage.show(); }
static void onUI_hide() { sPage.hide(); }
static void onUI_quit() {
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeys);   // 定时器由框架随页面销毁
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
 * @brief 触摸事件（本页按钮都是原生控件，无需拦截）
 */
static bool oncalcActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;
}

static bool onButtonClick_BtnK0(ZKButton *p) { (void)p; sPage.button(0); return true; }
static bool onButtonClick_BtnK1(ZKButton *p) { (void)p; sPage.button(1); return true; }
static bool onButtonClick_BtnK2(ZKButton *p) { (void)p; sPage.button(2); return true; }
static bool onButtonClick_BtnK3(ZKButton *p) { (void)p; sPage.button(3); return true; }
static bool onButtonClick_BtnK4(ZKButton *p) { (void)p; sPage.button(4); return true; }
static bool onButtonClick_BtnK5(ZKButton *p) { (void)p; sPage.button(5); return true; }
static bool onButtonClick_BtnK6(ZKButton *p) { (void)p; sPage.button(6); return true; }
static bool onButtonClick_BtnK7(ZKButton *p) { (void)p; sPage.button(7); return true; }
static bool onButtonClick_BtnK8(ZKButton *p) { (void)p; sPage.button(8); return true; }
static bool onButtonClick_BtnK9(ZKButton *p) { (void)p; sPage.button(9); return true; }
static bool onButtonClick_BtnK10(ZKButton *p) { (void)p; sPage.button(10); return true; }
static bool onButtonClick_BtnK11(ZKButton *p) { (void)p; sPage.button(11); return true; }
static bool onButtonClick_BtnK12(ZKButton *p) { (void)p; sPage.button(12); return true; }
static bool onButtonClick_BtnK13(ZKButton *p) { (void)p; sPage.button(13); return true; }
static bool onButtonClick_BtnK14(ZKButton *p) { (void)p; sPage.button(14); return true; }
static bool onButtonClick_BtnK15(ZKButton *p) { (void)p; sPage.button(15); return true; }
static bool onButtonClick_BtnK16(ZKButton *p) { (void)p; sPage.button(16); return true; }
static bool onButtonClick_BtnK17(ZKButton *p) { (void)p; sPage.button(17); return true; }
static bool onButtonClick_BtnK18(ZKButton *p) { (void)p; sPage.button(18); return true; }
static bool onButtonClick_BtnK19(ZKButton *p) { (void)p; sPage.button(19); return true; }
