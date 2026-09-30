#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * clocksuiteLogic.cc - 时钟套件（独立 ftu：clocksuite.ftu -> clocksuiteActivity）
 *
 * 为什么独立：主界面拆分口径 =「游戏不动，其余功能界面全部独立成 ftu」
 * （见 docs/page-split-plan.md）。本页装三样东西里的两样：
 *   WinClockSuite（主页：大钟/日期/5 行闹钟/编辑区）、WinWorld（世界时钟）。
 *
 * ⚠️⚠️ **响铃提醒页 WinAlarmRing 不在这里，仍然留在 main.ftu**：
 *   闹钟是**跨页面**的（`pg::Alarm` 守护线程在任何页面都会响），提醒页由主循环的
 *   `tickAlarmRing()` 弹出 —— 它属于"整屏浮层"（和音量 OSD 同类），
 *   放进某个应用页会让"在别的应用里响铃时看不到提醒"。
 *   本页只做一件事配合它：**响铃期间自动收掉自己**（见 onUI_Timer 里的让位），
 *   回到主界面后由主循环把提醒页弹出来。
 *
 * ⚠️ 闹钟数据与守护线程在 `platform/PgAlarm.*`，**跟页面无关**，不要搬。
 *
 * 按键（本板实体键）：103 音量- / 105 音量+ / 108 暂停键。
 *   108 短按 = 世界钟页返回主页面（在主页则无动作）；108 长按 ≥700ms = 返回应用列表。
 *   ⚠️ 本板 gpio-keys **无 autorepeat** ⇒ 长按只能应用层按时长判。
 *
 * 自检通道：/tmp/pg_clocksuitecmd（独立页必须有自己的通道 —— 后台 Activity 的
 * 定时器不跑，主界面的 /tmp/pg_autostart 在这里失效，wifi/remote/工具页都踩过）：
 *   suite new|ok|cancel|rep|hr <d>|min <d>|edit <i>|sw <i>|del <i>|world|back|state
 *   （子命令与 docs/clock-suite.md 保持一致，脚本不用改）
 *   dump    打印当前相位（编辑态/闹钟数/提示）
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/PgGame.h"          // pg::SFX_CLICK / pg::SFX_SCORE
#include "entry/EasyUIContext.h"
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgAudio.h"     // pg::volumeStepGlobal（音量键）
#include "platform/PgAlarm.h"
#include "platform/PgNavi.h"      // 导航栏标题覆盖（"世界时钟"子页名要用）
#include "ui/ToolPage.h"          // pgHost()：复用主界面的音效/存档宿主
#include "utils/Log.h"

namespace {

/* 文本写入：只在变化时写控件（每帧无条件 setText 会重绘风暴）。
 * 与主界面的同名实现一致，但本页自带一份 —— 独立 ftu 独立逻辑，不跨文件依赖。 */
void setToolText(ZKTextView *tv, char *cache, int n, const char *val) {
  if (!tv) return;
  if (!val) val = "";
  if (strncmp(cache, val, (size_t)n - 1) == 0) return;
  snprintf(cache, (size_t)n, "%s", val);
  tv->setText(cache);
}

/* 音效：走主界面的宿主（音效与存档是**进程级单例**，各页自己 new 一份会重复 init，
 * 见 src/ui/ToolPage.h 的说明）。宿主在 mainActivity 的 onUI_init 里就绪，恒非空（仍判一下）。 */
void playSfx(int id) {
  pg::Host *h = pg::pgHost();
  if (h) h->playSfx(id);
}

/* 控件底色：只 setBackgroundColor 看不到变化，要配 setBgStatusColor（本工程硬规则） */
void setCtrlBg(ZKBase *v, uint32_t color) {
  if (!v) return;
  v->setBackgroundColor(color);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
}

/* 本页内部函数的前置声明：handleSuiteCmd 在文件靠前，而这些实现写在后面
 * （QA 通道 / suite 子命令要用它们，顺序反了会 'was not declared'）。 */
void syncClockSuite();
void syncWorldClock();
const pg::AlarmItem *suiteItem(int i);
void suiteTouchEdit();
void suiteEditExisting(int i);
void suiteAddNew();
void suiteSave();
void suiteCancel();
void suiteStepHr(int d);
void suiteStepMin(int d);
void suiteCycleRepeat();

/* 底部钥匙栏文案（原来是 gGame->keyBar()；独立后固定写死，与 GameClockSuite 里那句一致） */
const char *kKeyBar = "B 键 返回列表 · 到点自动响铃并亮屏";

const char *const WEEK_CN[7] = {"日", "一", "二", "三", "四", "五", "六"};

const int kSuiteRows = 5;
const int kWorldCount = 10;

/* 编辑态：-2 = 未在编辑（显示 --）；-1 = 新建中；>=0 = 编辑第 index 个闹钟 */
struct SuiteEdit {
  int index;
  int hour, minute, mask;
};
SuiteEdit gSuiteEdit = {-2, 7, 30, 127};

/* 文本缓存：只在变化时写控件（每帧无条件 setText 会造成重绘风暴，见 §硬规则） */
char gSuiteTime[16], gSuiteDate[40], gSuiteHint[80], gSuiteEditHdr[40];
char gSuiteRowTime[kSuiteRows][8], gSuiteRowRep[kSuiteRows][28], gSuiteRowSw[kSuiteRows][8];
char gSuiteEHr[8], gSuiteEMin[8], gSuiteERep[28], gSuiteKeyBarCache[64];
char gWorldCity[kWorldCount][24], gWorldTime[kWorldCount][8];

long gSuiteLastSec = -1;

const pg::AlarmItem *suiteItem(int i) {
  const pg::Alarm &al = pg::Alarm::instance();
  return (i >= 0 && i < al.count()) ? &al.items()[i] : 0;
}

/* 重复说明：mask 0=仅一次；127=每天；62=工作日（周一~周五）；65=周末 */

const char *maskText(int mask) {
  switch (mask) {
    case 0: return "仅一次";
    case 127: return "每天";
    case 62: return "工作日";
    case 65: return "周末";
    default: return "自定义";
  }
}


ZKButton *suiteTimeBtn(int i) {
  switch (i) {
    case 0: return mBtnAlTime0Ptr; case 1: return mBtnAlTime1Ptr;
    case 2: return mBtnAlTime2Ptr; case 3: return mBtnAlTime3Ptr;
    default: return mBtnAlTime4Ptr;
  }
}
ZKTextView *suiteRepText(int i) {
  switch (i) {
    case 0: return mTextAlRep0Ptr; case 1: return mTextAlRep1Ptr;
    case 2: return mTextAlRep2Ptr; case 3: return mTextAlRep3Ptr;
    default: return mTextAlRep4Ptr;
  }
}
ZKButton *suiteSwBtn(int i) {
  switch (i) {
    case 0: return mBtnAlSw0Ptr; case 1: return mBtnAlSw1Ptr;
    case 2: return mBtnAlSw2Ptr; case 3: return mBtnAlSw3Ptr;
    default: return mBtnAlSw4Ptr;
  }
}
ZKButton *suiteDelBtn(int i) {
  switch (i) {
    case 0: return mBtnAlDel0Ptr; case 1: return mBtnAlDel1Ptr;
    case 2: return mBtnAlDel2Ptr; case 3: return mBtnAlDel3Ptr;
    default: return mBtnAlDel4Ptr;
  }
}
ZKTextView *worldCityText(int i) {
  switch (i) {
    case 0: return mTextWorldCity0Ptr; case 1: return mTextWorldCity1Ptr;
    case 2: return mTextWorldCity2Ptr; case 3: return mTextWorldCity3Ptr;
    case 4: return mTextWorldCity4Ptr; case 5: return mTextWorldCity5Ptr;
    case 6: return mTextWorldCity6Ptr; case 7: return mTextWorldCity7Ptr;
    case 8: return mTextWorldCity8Ptr; default: return mTextWorldCity9Ptr;
  }
}
ZKTextView *worldTimeText(int i) {
  switch (i) {
    case 0: return mTextWorldTime0Ptr; case 1: return mTextWorldTime1Ptr;
    case 2: return mTextWorldTime2Ptr; case 3: return mTextWorldTime3Ptr;
    case 4: return mTextWorldTime4Ptr; case 5: return mTextWorldTime5Ptr;
    case 6: return mTextWorldTime6Ptr; case 7: return mTextWorldTime7Ptr;
    case 8: return mTextWorldTime8Ptr; default: return mTextWorldTime9Ptr;
  }
}


struct WorldCity {
  const char *name;
  int off;   // 相对 UTC 的小时偏移
};
const WorldCity kWorldCities[kWorldCount] = {
    {"北京", 8},  {"东京", 9},  {"首尔", 9},   {"新加坡", 8}, {"迪拜", 4},
    {"莫斯科", 3}, {"伦敦", 0}, {"巴黎", 1},   {"纽约", -5},  {"洛杉矶", -8},
};


void syncWorldClock() {
  static long lastMin = -1;
  time_t now = time(0);
  long m = (long)(now / 60);
  if (m == lastMin) return;
  lastMin = m;
  char b[24];
  for (int i = 0; i < kWorldCount; ++i) {
    time_t t = now + (time_t)kWorldCities[i].off * 3600;
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(b, sizeof(b), "%02d:%02d", g.tm_hour, g.tm_min);
    setToolText(worldTimeText(i), gWorldTime[i], sizeof(gWorldTime[i]), b);
    setToolText(worldCityText(i), gWorldCity[i], sizeof(gWorldCity[i]),
                kWorldCities[i].name);
  }
}

/* ==================== 世界时钟子页的"在/不在"统一入口 ====================
 * ★ 2026-09-16「检讨 UI 覆盖」修正 ①（用户已审批）：
 *   导航栏只会按 Activity 名显示"时钟套件"，而子页自己的标题 TextWorldTitle
 *   （"世界时钟"，y=8..52）**整块被 navibar（0..52 常显浮层）盖死** ⇒ 用户切到
 *   世界时钟页后无从得知自己在哪一页。页面里也没有第二个位置挪它
 *   （10 行城市从 y=60 起、每行 64px，已经铺到 y=700，下面就是提示行与返回键）。
 *   ⇒ 把名字直接写到状态栏上（pg::setNaviTitle），退出子页传 0 恢复自动映射。
 *   见 src/platform/PgNavi.h。
 *
 * ⚠️ 必须**所有**切页路径都走这个函数（按钮 / 短按 108 / QA 通道）——
 *    漏一条就会出现"标题卡在世界时钟上但人在主页"这种粘性状态。 */
void suiteSetWorldPage(bool on) {
  if (on) {
    if (mWinClockSuitePtr) mWinClockSuitePtr->hideWnd();
    if (mWinWorldPtr) mWinWorldPtr->showWnd();
    syncWorldClock();
    pg::setNaviTitle("世界时钟");
  } else {
    if (mWinWorldPtr) mWinWorldPtr->hideWnd();
    if (mWinClockSuitePtr) mWinClockSuitePtr->showWnd();
    pg::setNaviTitle(0);
  }
}


/* suite 子命令（QA 通道；命令名与 docs/clock-suite.md 一致，脚本不用改）。
 * 调用的就是界面回调**内部那一批函数**（suiteAddNew/suiteSave/...）⇒ 验的是同一条逻辑。 */
void handleSuiteCmd(const char *a) {
  pg::Alarm &al = pg::Alarm::instance();
  const char *arg = a;
  while (*arg == ' ') ++arg;
  if (strncmp(arg, "new", 3) == 0) {
    suiteAddNew();
  } else if (strncmp(arg, "ok", 2) == 0) {
    suiteSave();
  } else if (strncmp(arg, "cancel", 6) == 0) {
    suiteCancel();
  } else if (strncmp(arg, "rep", 3) == 0) {
    suiteCycleRepeat();
  } else if (strncmp(arg, "hr ", 3) == 0) {
    suiteStepHr(atoi(arg + 3));
  } else if (strncmp(arg, "min ", 4) == 0) {
    suiteStepMin(atoi(arg + 4));
  } else if (strncmp(arg, "edit ", 5) == 0) {
    suiteEditExisting(atoi(arg + 5));
  } else if (strncmp(arg, "sw ", 3) == 0) {
    int i = atoi(arg + 3);
    const pg::AlarmItem *it = suiteItem(i);
    if (it) {
      al.setEnabled(i, !it->enabled);
      LOGD("PocketGame: suite sw %d -> enabled=%d", i, it->enabled ? 0 : 1);
    }
    gSuiteLastSec = -1;
    syncClockSuite();
  } else if (strncmp(arg, "del ", 4) == 0) {
    int i = atoi(arg + 4);
    const pg::AlarmItem *it = suiteItem(i);
    if (it) {
      LOGD("PocketGame: suite del %d（原 %02d:%02d）", i, it->hour, it->minute);
      al.removeAt(i);
      if (gSuiteEdit.index == i) gSuiteEdit.index = -2;
      else if (gSuiteEdit.index > i) gSuiteEdit.index -= 1;
    }
    gSuiteLastSec = -1;
    syncClockSuite();
  } else if (strncmp(arg, "world", 5) == 0) {
    suiteSetWorldPage(true);
    LOGD("PocketGame: suite world -> 世界时钟页");
  } else if (strncmp(arg, "back", 4) == 0) {
    suiteSetWorldPage(false);
    LOGD("PocketGame: suite back -> 主页");
  } else if (strncmp(arg, "state", 5) == 0) {
    LOGD("PocketGame: suite state 编辑态=%d(%02d:%02d mask=%d) 闹钟数=%d 提示='%s'",
         gSuiteEdit.index, gSuiteEdit.hour, gSuiteEdit.minute, gSuiteEdit.mask,
         al.count(), gSuiteHint);
  } else {
    LOGD("时钟套件 QA: suite 子命令未知：'%s'", arg);
  }
}

void syncClockSuite() {
  time_t now = time(0);
  if ((long)now == gSuiteLastSec) return;   // 一秒只做一次
  gSuiteLastSec = (long)now;
  struct tm lt;
  localtime_r(&now, &lt);
  char b[64];

  snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
  setToolText(mTextSuiteTimePtr, gSuiteTime, sizeof(gSuiteTime), b);
  snprintf(b, sizeof(b), "%04d-%02d-%02d  星期%s", lt.tm_year + 1900, lt.tm_mon + 1,
           lt.tm_mday, WEEK_CN[(lt.tm_wday % 7 + 7) % 7]);
  setToolText(mTextSuiteDatePtr, gSuiteDate, sizeof(gSuiteDate), b);

  const pg::Alarm &al = pg::Alarm::instance();
  for (int i = 0; i < kSuiteRows; ++i) {
    const pg::AlarmItem *it = suiteItem(i);
    ZKButton *bt = suiteTimeBtn(i);
    ZKTextView *rp = suiteRepText(i);
    ZKButton *sw = suiteSwBtn(i);
    ZKButton *dl = suiteDelBtn(i);
    const bool has = (it != 0);
    if (bt) {
      if (has) snprintf(b, sizeof(b), "%02d:%02d", it->hour, it->minute);
      else snprintf(b, sizeof(b), "--:--");
      if (strcmp(gSuiteRowTime[i], b) != 0) {
        snprintf(gSuiteRowTime[i], sizeof(gSuiteRowTime[i]), "%s", b);
        bt->setText(gSuiteRowTime[i]);
      }
      /* 有闹钟的行用亮字，空行用暗字（一眼看出哪几行是空的） */
      bt->setTextColor(has ? 0xFFF2F2F7 : 0xFF636366);
      /* 正在编辑的那行高亮（琥珀） */
      setCtrlBg(bt, (gSuiteEdit.index == i) ? 0xFF3A3A3C : 0xFF2C2C2E);
    }
    if (rp) setToolText(rp, gSuiteRowRep[i], sizeof(gSuiteRowRep[i]),
                        has ? maskText(it->mask) : "");
    if (sw) {
      const char *t = !has ? "" : (it->enabled ? "开" : "关");
      if (strcmp(gSuiteRowSw[i], t) != 0) {
        snprintf(gSuiteRowSw[i], sizeof(gSuiteRowSw[i]), "%s", t);
        sw->setText(gSuiteRowSw[i]);
      }
      setCtrlBg(sw, !has ? 0xFF1C1C1E : (it->enabled ? 0xFF30D158 : 0xFF2C2C2E));
      sw->setVisible(has);
      if (dl) dl->setVisible(has);
    }
  }

  /* 编辑区 */
  const bool editing = (gSuiteEdit.index != -2);
  setToolText(mTextEditHdrPtr, gSuiteEditHdr, sizeof(gSuiteEditHdr),
              editing ? (gSuiteEdit.index < 0 ? "新建闹钟（调好时间后点「保存」）"
                                              : "编辑闹钟（改好后点「保存」）")
                      : "编辑（先点一行，或点「新建」）");
  if (editing) {
    snprintf(b, sizeof(b), "%02d", gSuiteEdit.hour);
    setToolText(mTextEditHrPtr, gSuiteEHr, sizeof(gSuiteEHr), b);
    snprintf(b, sizeof(b), "%02d", gSuiteEdit.minute);
    setToolText(mTextEditMinPtr, gSuiteEMin, sizeof(gSuiteEMin), b);
    /* 「重复」是按钮（不是 textview）：文字直接写它，带变化检测 */
    snprintf(b, sizeof(b), "重复：%s", maskText(gSuiteEdit.mask));
    if (mBtnRepeatPtr && strcmp(gSuiteERep, b) != 0) {
      snprintf(gSuiteERep, sizeof(gSuiteERep), "%s", b);
      mBtnRepeatPtr->setText(gSuiteERep);
    }
  } else {
    setToolText(mTextEditHrPtr, gSuiteEHr, sizeof(gSuiteEHr), "--");
    setToolText(mTextEditMinPtr, gSuiteEMin, sizeof(gSuiteEMin), "--");
    if (mBtnRepeatPtr && strcmp(gSuiteERep, "重复：--") != 0) {
      snprintf(gSuiteERep, sizeof(gSuiteERep), "重复：--");
      mBtnRepeatPtr->setText(gSuiteERep);
    }
  }
  setToolText(mTextSuiteKeyBarPtr, gSuiteKeyBarCache, sizeof(gSuiteKeyBarCache),
              kKeyBar);
}


/* ==================== 时钟套件：编辑辅助 + 按钮回调 ====================
 * ⚠️ 每个回调函数名必须**字面出现在源码里**（fun 按文本匹配生成绑定，宏拼接会被判为缺失）。
 * ⚠️ 编辑态变化后要把 gSuiteLastSec 置 -1 强制刷新
 *    （syncClockSuite 有"一秒只做一次"的闸，不然点了按钮界面要等一秒才变）。
 */
void suiteTouchEdit() { gSuiteLastSec = -1; syncClockSuite(); }

/* 点某一行 → 把该闹钟载入编辑区 */
void suiteEditExisting(int i) {
  const pg::AlarmItem *it = suiteItem(i);
  if (!it) return;   // 空行：忽略（要新增请点「新建闹钟」）
  gSuiteEdit.index = i;
  gSuiteEdit.hour = it->hour;
  gSuiteEdit.minute = it->minute;
  gSuiteEdit.mask = it->mask;
  LOGD("PocketGame: 时钟套件 编辑 #%d %02d:%02d", i, it->hour, it->minute);
  suiteTouchEdit();
}

/* 点「新建闹钟」：以"当前时间 +1 分钟"为起点（比固定 07:30 更符合直觉） */
void suiteAddNew() {
  time_t now = time(0);
  struct tm lt;
  localtime_r(&now, &lt);
  int mm = lt.tm_min + 1, hh = lt.tm_hour;
  if (mm >= 60) { mm = 0; hh = (hh + 1) % 24; }
  gSuiteEdit.index = -1;
  gSuiteEdit.hour = hh;
  gSuiteEdit.minute = mm;
  gSuiteEdit.mask = 127;
  LOGD("PocketGame: 时钟套件 新建闹钟（起点 %02d:%02d）", hh, mm);
  suiteTouchEdit();
}

/* 保存：新建 → add；编辑 → setItem */
void suiteSave() {
  pg::Alarm &al = pg::Alarm::instance();
  bool ok = false;
  if (gSuiteEdit.index < 0) {
    ok = al.add(gSuiteEdit.hour, gSuiteEdit.minute, gSuiteEdit.mask);
  } else {
    ok = al.setItem(gSuiteEdit.index, gSuiteEdit.hour, gSuiteEdit.minute,
                    gSuiteEdit.mask);
  }
  if (ok) playSfx(pg::SFX_SCORE);
  snprintf(gSuiteHint, sizeof(gSuiteHint), ok ? "已保存 %02d:%02d（%s）" : "保存失败（最多 5 个）",
           gSuiteEdit.hour, gSuiteEdit.minute, maskText(gSuiteEdit.mask));
  if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
  gSuiteEdit.index = -2;   // 退出编辑态
  suiteTouchEdit();
}

void suiteCancel() {
  gSuiteEdit.index = -2;
  snprintf(gSuiteHint, sizeof(gSuiteHint), "已取消");
  if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
  suiteTouchEdit();
}

/* 时/分 ± （小时 0..23 环绕；分钟 0..59 环绕） */
void suiteStepHr(int d) {
  if (gSuiteEdit.index == -2) return;
  gSuiteEdit.hour = (gSuiteEdit.hour + d + 24) % 24;
  suiteTouchEdit();
}
void suiteStepMin(int d) {
  if (gSuiteEdit.index == -2) return;
  gSuiteEdit.minute = (gSuiteEdit.minute + d + 60) % 60;
  suiteTouchEdit();
}

/* 重复：仅一次 → 每天 → 工作日 → 周末 → 仅一次（循环） */
void suiteCycleRepeat() {
  if (gSuiteEdit.index == -2) return;
  switch (gSuiteEdit.mask) {
    case 0:   gSuiteEdit.mask = 127; break;   // 仅一次 -> 每天
    case 127: gSuiteEdit.mask = 62;  break;   // 每天 -> 工作日（周一~周五）
    case 62:  gSuiteEdit.mask = 65;  break;   // 工作日 -> 周末（周日+周六）
    default:  gSuiteEdit.mask = 0;   break;   // 其它 -> 仅一次
  }
  LOGD("PocketGame: 时钟套件 重复 -> %s（mask=%d）", maskText(gSuiteEdit.mask),
       gSuiteEdit.mask);
  suiteTouchEdit();
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

const int TIMER_TICK = 1;
const int TICK_MS = 300;   // 大钟按秒走；300ms 一拍够用，QA 响应也快
const int KEY_MS = 100;    // 按键长按轮询周期（300ms 太粗，长按判定会飘到 1s）
const int TIMER_KEY = 2;   // 按键长按轮询用的定时器 id

/**
 * 注册定时器
 */
static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_TICK, TICK_MS},
    {TIMER_KEY, KEY_MS},   // 按键长按轮询（达标立刻返回，不等抬手）
};

/* -------------------- 自检通道 /tmp/pg_clocksuitecmd -------------------- */
char sQaTag[512] = {0};

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_clocksuitecmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_clocksuitecmd", "r");
  if (!f) return;
  char buf[1024];
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
    char *h = strchr(line, '#');      // 行内注释（习惯写 `suite new #1` 保证内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    if (strncmp(line, "dump", 4) == 0) {
      LOGD("时钟套件 QA: 编辑态=%d(%02d:%02d mask=%d) 闹钟数=%d 提示='%s' 世界钟页=%d",
           gSuiteEdit.index, gSuiteEdit.hour, gSuiteEdit.minute, gSuiteEdit.mask,
           pg::Alarm::instance().count(), gSuiteHint,
           (mWinWorldPtr && mWinWorldPtr->isWndShow()) ? 1 : 0);
      continue;
    }
    if (strncmp(line, "suite", 5) == 0) {
      handleSuiteCmd(line + 5);
      continue;
    }
    LOGD("时钟套件 QA: 未知命令 '%s'", line);
  }
}

/* -------------------- 物理按键 -------------------- */
class SuiteKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键，不顺手调音量/停流）。
     *   ⚠️ 必须在第一句：本页只是 hide、监听器还挂在链上，而框架按键分发是**短路式**的
     *   —— 本页若先返回 true，屏保页自己的监听器可能收不到按键。见 platform/PgSaver.h。 */
    if (pg::wakeSaverByKey(ke)) return true;
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = t;
      sLPFired = false;
      return true;   // 按下先吞掉
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;
    int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= 700 && !sLPFired) {
      LOGD("时钟套件: 长按 %dms -> 返回列表", held);
      EASYUICONTEXT->closeActivity("clocksuiteActivity");
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
      int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("时钟套件: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      return true;
    }
    if (ke.mKeyCode == 108) {
      /* 世界钟页里短按 = 返回套件主页（与页面里的〈返回 按钮同义）。
       * 主页里短按无动作（原来套件在主界面时也一样）。 */
      if (mWinWorldPtr && mWinWorldPtr->isWndShow()) {
        suiteSetWorldPage(false);        // 窗口 + 导航栏标题一起收（见函数注释）
        LOGD("时钟套件: 短按 -> 回套件主页");
      }
      return true;
    }
    return true;
  }

  /* ★ 长按达标**立刻**返回，不等按键抬起。
 *   本板 gpio-keys **没有 autorepeat** ⇒ 长按期间内核一个事件都不发；只在 E_KEY_UP 里
 *   判长按的话，用户按满 700ms 还得一直按到松手才切页 —— 手感就是"按了不动、松手才跳"。
 *   所以由页面定时器轮询"按下且已超时"。 */

  static bool longPressReady() {
    if (sDownCode < 0 || sLPFired) return false;      // 有键按着（不分键码：
                                                      // 与本页原来的语义一致 ——
                                                      // 任何键按住 >=700ms 都返回）
    if (nowMs() - sDownMs < 700) return false;
    sLPFired = true;   // 只触发一次
    return true;
  }

 private:
  static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
  }
  static int sDownCode;
  static long sDownMs;
  static bool sLPFired;
};
int SuiteKeys::sDownCode = -1;
long SuiteKeys::sDownMs = 0;
bool SuiteKeys::sLPFired = false;
SuiteKeys sKeys;

/* 定时器每 100ms 调一次：达标就立刻返回应用列表（不等抬手）。 */
void tickKeyLongPress() {
  if (SuiteKeys::longPressReady()) {
    LOGD("时钟套件: 长按达标（不等抬起）-> 返回列表");
    EASYUICONTEXT->closeActivity("clocksuiteActivity");
  }
}

/**
 * @brief 当界面构造时触发
 */
static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  gSuiteLastSec = -1;     // 强制首帧全量刷新
  qaSyncTag();            // 自检基线：只执行"进页之后新推的"命令
  LOGD("时钟套件页就绪（独立 ftu）");
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("时钟套件: onUI_show");
  /* 工作界面禁屏保（官方推荐用法，见 EasyUIContext.h）。本页是"看一眼钟/改闹钟"的页，
   * 但一样是独立 ftu —— 主界面不在前台时它的屏保策略不跑，必须自己关。
   * ⚠️ 只在 show 关、quit 恢复，别在 hide 恢复（否则回主界面到收敛之间有空档）。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
  gSuiteLastSec = -1;
  syncClockSuite();
  syncWorldClock();
}

static void onUI_hide() {
  LOGD("时钟套件: onUI_hide");
  /* ★ 粘性标志复位（2026-09-16）：本页退后台时可能正停在世界时钟子页上，
   *   不复位的话 navibar 会继续显示"世界时钟"（它在别的页上也常显）。 */
  pg::setNaviTitle(0);
}

static void onUI_quit() {
  LOGD("时钟套件: onUI_quit");
  pg::setNaviTitle(0);           // 同上：长按返回是 closeActivity，不走子页切页函数
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeys);
#endif
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

/**
 * @brief 定时器回调：不要在这里做耗时操作
 */
static bool onUI_Timer(int id) {
  if (id == TIMER_KEY) {   // 长按达标即返回（不等抬手）
    tickKeyLongPress();
    return true;
  }
  if (id != TIMER_TICK) return false;
  /* ★ 闹钟让位：响铃期间本页收掉自己，回主界面让主循环把"闹钟提醒页"弹出来
   *   （提醒页在 main.ftu；本页是全屏页，留着它就看不到提醒页，用户只听见响、找不到停止）。
   *   这同时补上了老版本的空档：以前在 wifi/工具页里响铃也只有声音、没有界面。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("时钟套件: 响铃中 -> 让位（回主界面弹提醒页）");
    EASYUICONTEXT->closeActivity("clocksuiteActivity");
    return false;
  }
  syncClockSuite();
  syncWorldClock();
  qaPoll();
  return true;
}

/**
 * @brief 触摸事件（本页控件都是原生按钮，只在世界钟页需要一点点额外处理）
 */
static bool onclocksuiteActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;
}

/* ==================================================================
 *                   按钮回调（名字必须字面出现，fun 按文本匹配）
 * ================================================================== */
/* ---- 5 行：时间（点击=编辑）/ 开关 / 删除 ---- */
static bool onButtonClick_BtnAlTime0(ZKButton *pButton) {
  (void)pButton;
  suiteEditExisting(0);
  return true;
}
static bool onButtonClick_BtnAlTime1(ZKButton *pButton) {
  (void)pButton;
  suiteEditExisting(1);
  return true;
}
static bool onButtonClick_BtnAlTime2(ZKButton *pButton) {
  (void)pButton;
  suiteEditExisting(2);
  return true;
}
static bool onButtonClick_BtnAlTime3(ZKButton *pButton) {
  (void)pButton;
  suiteEditExisting(3);
  return true;
}
static bool onButtonClick_BtnAlTime4(ZKButton *pButton) {
  (void)pButton;
  suiteEditExisting(4);
  return true;
}
static bool onButtonClick_BtnAlSw0(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(0);
  if (it) {
    pg::Alarm::instance().setEnabled(0, !it->enabled);
    snprintf(gSuiteHint, sizeof(gSuiteHint), "%02d:%02d 已%s", it->hour, it->minute,
             it->enabled ? "关闭" : "开启");
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_CLICK);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlSw1(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(1);
  if (it) {
    pg::Alarm::instance().setEnabled(1, !it->enabled);
    snprintf(gSuiteHint, sizeof(gSuiteHint), "%02d:%02d 已%s", it->hour, it->minute,
             it->enabled ? "关闭" : "开启");
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_CLICK);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlSw2(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(2);
  if (it) {
    pg::Alarm::instance().setEnabled(2, !it->enabled);
    snprintf(gSuiteHint, sizeof(gSuiteHint), "%02d:%02d 已%s", it->hour, it->minute,
             it->enabled ? "关闭" : "开启");
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_CLICK);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlSw3(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(3);
  if (it) {
    pg::Alarm::instance().setEnabled(3, !it->enabled);
    snprintf(gSuiteHint, sizeof(gSuiteHint), "%02d:%02d 已%s", it->hour, it->minute,
             it->enabled ? "关闭" : "开启");
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_CLICK);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlSw4(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(4);
  if (it) {
    pg::Alarm::instance().setEnabled(4, !it->enabled);
    snprintf(gSuiteHint, sizeof(gSuiteHint), "%02d:%02d 已%s", it->hour, it->minute,
             it->enabled ? "关闭" : "开启");
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_CLICK);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlDel0(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(0);
  if (it) {
    snprintf(gSuiteHint, sizeof(gSuiteHint), "已删除 %02d:%02d", it->hour, it->minute);
    pg::Alarm::instance().removeAt(0);
    /* 删掉之后后面的行会前移：正在编辑的索引要跟着修正，否则会指错行 */
    if (gSuiteEdit.index == 0) gSuiteEdit.index = -2;
    else if (gSuiteEdit.index > 0) gSuiteEdit.index -= 1;
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_DROP);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlDel1(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(1);
  if (it) {
    snprintf(gSuiteHint, sizeof(gSuiteHint), "已删除 %02d:%02d", it->hour, it->minute);
    pg::Alarm::instance().removeAt(1);
    /* 删掉之后后面的行会前移：正在编辑的索引要跟着修正，否则会指错行 */
    if (gSuiteEdit.index == 1) gSuiteEdit.index = -2;
    else if (gSuiteEdit.index > 1) gSuiteEdit.index -= 1;
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_DROP);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlDel2(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(2);
  if (it) {
    snprintf(gSuiteHint, sizeof(gSuiteHint), "已删除 %02d:%02d", it->hour, it->minute);
    pg::Alarm::instance().removeAt(2);
    /* 删掉之后后面的行会前移：正在编辑的索引要跟着修正，否则会指错行 */
    if (gSuiteEdit.index == 2) gSuiteEdit.index = -2;
    else if (gSuiteEdit.index > 2) gSuiteEdit.index -= 1;
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_DROP);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlDel3(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(3);
  if (it) {
    snprintf(gSuiteHint, sizeof(gSuiteHint), "已删除 %02d:%02d", it->hour, it->minute);
    pg::Alarm::instance().removeAt(3);
    /* 删掉之后后面的行会前移：正在编辑的索引要跟着修正，否则会指错行 */
    if (gSuiteEdit.index == 3) gSuiteEdit.index = -2;
    else if (gSuiteEdit.index > 3) gSuiteEdit.index -= 1;
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_DROP);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
static bool onButtonClick_BtnAlDel4(ZKButton *pButton) {
  (void)pButton;
  const pg::AlarmItem *it = suiteItem(4);
  if (it) {
    snprintf(gSuiteHint, sizeof(gSuiteHint), "已删除 %02d:%02d", it->hour, it->minute);
    pg::Alarm::instance().removeAt(4);
    /* 删掉之后后面的行会前移：正在编辑的索引要跟着修正，否则会指错行 */
    if (gSuiteEdit.index == 4) gSuiteEdit.index = -2;
    else if (gSuiteEdit.index > 4) gSuiteEdit.index -= 1;
    if (mTextSuiteHintPtr) mTextSuiteHintPtr->setText(gSuiteHint);
    playSfx(pg::SFX_DROP);
    gSuiteLastSec = -1;
    syncClockSuite();
  }
  return true;
}
/* ---- 编辑区 ---- */
static bool onButtonClick_BtnHrDown(ZKButton *pButton) {
  (void)pButton; suiteStepHr(-1); return true;
}
static bool onButtonClick_BtnHrUp(ZKButton *pButton) {
  (void)pButton; suiteStepHr(1); return true;
}
static bool onButtonClick_BtnMinDown(ZKButton *pButton) {
  (void)pButton; suiteStepMin(-1); return true;
}
static bool onButtonClick_BtnMinUp(ZKButton *pButton) {
  (void)pButton; suiteStepMin(1); return true;
}
static bool onButtonClick_BtnRepeat(ZKButton *pButton) {
  (void)pButton; suiteCycleRepeat(); return true;
}
static bool onButtonClick_BtnSave(ZKButton *pButton) {
  (void)pButton;
  suiteSave();
  return true;
}
static bool onButtonClick_BtnCancel(ZKButton *pButton) {
  (void)pButton; suiteCancel(); return true;
}
static bool onButtonClick_BtnAdd(ZKButton *pButton) {
  (void)pButton;
  suiteAddNew();
  return true;
}

/* ---- 世界时钟页切换 ---- */
static bool onButtonClick_BtnWorld(ZKButton *pButton) {
  (void)pButton;
  suiteSetWorldPage(true);
  return true;
}
static bool onButtonClick_BtnWorldBack(ZKButton *pButton) {
  (void)pButton;
  suiteSetWorldPage(false);
  return true;
}

