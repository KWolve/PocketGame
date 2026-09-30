#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * navibar.cc - 全局导航栏（SysApp：navibar.ftu -> navibar，APP_TYPE_SYS_NAVIBAR）
 *
 * 用户需求（2026-09-16）：
 *   ① 「状态栏显示 wifi 连接状态」；问到形态时定为「可以做成导航栏，navibar」。
 *   ② 「应用界面内模仿 android 手机右滑有个动效图片后返回主页」。
 *   ③ 「状态栏显示不完整，直接把电池，页面 title 都放到状态栏这样子改动最小」
 *      ⇒ 本页扩成完整状态栏（标题 + 电量 + WiFi 三件套），见 refreshTitle / refreshBatt。
 *
 * ★★ 为什么这两件事都落在本文件：它的触摸回调是**全局监听**
 *   （`onnavibarActivityTouchEvent`，任何页面/浮层都会收到，实测见 statusbar.cc），
 *   所以"右滑返回"在这里做一次就覆盖**所有独立 ftu 页**——不用改 16 个页面。
 *   （主界面/画布游戏不在其列：游戏是主界面内的整屏 window，见 mainLogic.cc 的处理。）
 *
 * ★★ 与 statusbar（音量 OSD）最关键的差别：**它是常显的**。
 *   整屏浮层只要"显示着"就吃掉全机控件级触摸（血案见 statusbar.cc 的注释），
 *   statusbar 靠"按需显隐"回避了这个坑；导航栏要一直在，回避不了 ⇒
 *   解法是在 ui/navibar.html 里把 **root 的 data-res 写成 480x30**：
 *   窗口只占屏幕顶部那一条，挡住的也只有那 30px。
 *
 * ★★ 内容（2026-09-16 第三次需求后）= **状态栏三件套：页面标题 + 电量 + WiFi**。
 *   用户原话：「状态栏显示不完整，直接把电池，页面 title 都放到状态栏这样子改动最小」
 *   —— 各页自己的标题画在 y=8~18，会被本条（不透明浮层）盖成"半截"；
 *   统一到这里显示就不必逐页下移（那是 15 个文件）。
 *   ⚠️ **所以别再往各个页面加回"标题/电量"** —— 会与本条重复/被盖。
 *   ★ 验收判据（改完必验）：导航栏显示着时，主界面 tab / 列表项照常能点。
 *
 * ⚠️ 它自己**不放任何可点控件**（常显浮层上的按钮会被自己吃掉触摸，见 statusbar），
 *    只放状态与"右滑返回"的动效提示（那组提示不可点，只跟手移动）。
 *
 * 数据来源：`pg::WifiService`（zknet 封装，见 src/platform/PgWifi.h）。
 *
 * ⚠️ 文件命名：本文件是 **navibar.cc**（不是 navibarLogic.cc）—— 与 statusbar.cc 同构。
 *    踩过的坑：`fun build` 发现 `src/logic/<ftu名>.cc` 不存在时会**自动生成一个桩**，
 *    于是手写的 navibarLogic.cc 与桩 navibar.cc 同时存在 ⇒ 同一套回调注册两遍、
 *    链接期还报 undefined reference。**一个 ftu 只留一个 .cc**。
 *
 * ⚠️ 回调函数必须定义在**全局作用域**（不能放进匿名命名空间，否则链接不上）。
 *
 * 自检（QA 通道 `/tmp/pg_navibarcmd`）：
 *   dump          打印 WiFi 状态与导航栏显隐
 *   show / hide   手动切显隐
 *   back          手动调一次 `EASYUICONTEXT->goBack()`（验"右滑返回"到底会不会退）
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "entry/EasyUIContext.h"
#include "platform/PgBattery.h"
#include "platform/PgNavi.h"
#include "platform/PgSwipe.h"
#include "platform/PgWifi.h"
#include "utils/Log.h"

namespace {

const int TIMER_TICK = 1;
const int TICK_MS = 60;     // 16fps；WiFi/标题/电量用计数节流（原来还要给右滑动效供帧）
const int WIFI_EVERY_TICKS = 16;   // 16 * 60ms ≈ 1s
const int BATT_EVERY_TICKS = 16;   // 电量：1s 查一次（Battery 内部还按 30s 节流实际采样）
const int kSsidMax = 40;
const int kTitleMax = 48;
const int kActMax = 48;

int sTick = 0;
int sLastKind = -1;                 // -1 未知 / 0 关 / 1 未连 / 2 已连（变化检测用）
char sLastSsid[kSsidMax] = {0};
char sQaTag[512] = {0};

/* 把状态写到控件上（只在变化时写）
 * ⚠️ 框架的 setText/setVisible 都会触发重绘 —— 定时器每拍无条件写就是白烧 CPU
 *   （与 ToolPage 的"必须变化检测"同一条纪律：实测无条件写会到 127% CPU）。 */
void applyState(int kind, const char *ssid) {
  if (mImgNbWifiOnPtr) mImgNbWifiOnPtr->setVisible(kind == 2);
  if (mImgNbWifiOffPtr) mImgNbWifiOffPtr->setVisible(kind != 2);

  const char *txt = (kind == 0) ? "WiFi 已关闭" : ((kind == 2) ? ssid : "未连接");
  if (strcmp(sLastSsid, txt) != 0) {
    snprintf(sLastSsid, sizeof(sLastSsid), "%s", txt);
    if (mTextNbSsidPtr) mTextNbSsidPtr->setText(txt);
  }
}

void refreshWifi(bool force) {
  const bool on = pg::WifiService::enabled();
  const bool conn = on && pg::WifiService::connected();
  const int kind = !on ? 0 : (conn ? 2 : 1);
  char ssid[kSsidMax] = {0};
  if (conn) pg::WifiService::ssid(ssid, sizeof(ssid));
  const bool ssidChanged = conn && strcmp(ssid, sLastSsid) != 0;
  if (!force && kind == sLastKind && !ssidChanged) return;
  sLastKind = kind;
  applyState(kind, ssid);
  LOGD("导航栏: WiFi 状态 kind=%d（0关/1未连/2已连）ssid=%s", kind, conn ? ssid : "-");
}

/* ---------------- 页面标题（2026-09-16 用户需求） ----------------
 * 用户原话：「状态栏显示不完整，直接把电池，页面 title 都放到状态栏这样子改动最小」。
 *
 * 为什么标题必须由导航栏来显示：各页自己的标题画在 y=8~18（fs=20~22，墨迹约 10~42），
 * 会被本页（0..30 的不透明浮层）**盖成半截** —— 统一到这里显示才是干净解，
 * 而且顺带省掉"逐页把标题下移 30px"的 15 个文件改动（用户要的正是"改动最小"）。
 *
 * 数据来源：`EASYUICONTEXT->currentAppName()` = 当前 Activity 名（如 "settingsActivity"），
 * 查下面这张**本地映射表**换中文。为什么不从 `kAppTable` 取 title()：那要先 create()
 * 一个 Game 才能问标题（工厂函数有副作用/开销），而标题本来就是纯 UI 文案。
 * ⚠️ **加新应用（独立 ftu）时要在这里补一行** —— 这是继"图标四处"之后的**第 5 个同步点**；
 *    忘了的表现：状态栏标题停在旧页/显示兜底值。QA `dump` 会打出当前 Activity 名，便于核对。
 * ⚠️ 画布游戏（主界面内的 window）没有独立 Activity ⇒ currentAppName() 仍是 mainActivity，
 *    所以游戏里标题就是"口袋游戏机"，符合预期。 */
struct TitleMap { const char *act; const char *title; };
const TitleMap kTitleMap[] = {
    {"mainActivity",       "口袋游戏机"},
    {"wifiActivity",       "无线局域网"},
    {"iptvActivity",       "网络电视"},
    {"probeActivity",      "信号探针"},
    {"timerActivity",      "定时器"},
    {"stopwatchActivity",  "秒表"},
    {"calcActivity",       "计算器"},
    {"pomodoroActivity",   "番茄钟"},
    {"clocksuiteActivity", "时钟套件"},
    {"remoteActivity",     "蓝牙遥控"},
    {"reactActivity",      "反应计时"},
    {"radioActivity",      "网络收音机"},
    {"cameraActivity",     "局域网摄像头"},
    /* 2026-09-23：智能家居（Home Assistant 遥控器，slot 37）。
     * ⚠️ 就是上面那段注释说的"**第 5 个同步点**" —— 我第一版漏了它，
     *    实测现象正是注释里写的：状态栏标题显示兜底值「口袋游戏机」
     *    （logcat 里 `导航栏: 标题 act=haActivity -> 口袋游戏机`）。 */
    {"haActivity",         "智能家居"},
    {"settingsActivity",   "系统设置"},
};
char sLastTitle[kTitleMax] = {0};
char sLastAct[kActMax] = {0};
char sOvrAct[kActMax] = {0};   // ★ 覆盖是哪个 Activity 设的（过期判定用，见 refreshTitle）

/* ★ 2026-09-16「检讨 UI 覆盖」修正 ①：页面级名字走**覆盖**通道。
 *   上面那张 kTitleMap 只能给 Activity 级的名字；而游戏页的"当前游戏名"
 *   （TextGameTitle）与世界时钟子页的"世界时钟"（TextWorldTitle）都是**页面级**的，
 *   它们的控件整块躺在 y<52（被本条盖死），页面里也没有第二个位置能放
 *   （游戏 HUD 下面就是 540 高的画布；WinWorld 的 10 行城市已铺到 y=700）。
 *   ⇒ 由调用方 `pg::setNaviTitle(...)` 直接写到本条上，见 src/platform/PgNavi.h。 */
void refreshTitle(bool force) {
  const char *act = EASYUICONTEXT->currentAppName();
  if (!act) act = "";
  const char *title = 0;
  const char *ovr = pg::naviTitleOverride();
  /* ★★ 覆盖必须**跟着设置它的那个 Activity 走**（2026-09-16 真机验收抓到）：
   *   验收时 `enter 12`（进 WiFi 应用）之后，状态栏标题**仍停在"俄罗斯方块"** ——
   *   因为设置覆盖的是 mainLogic 的定时器，它在本页退到后台后就不再跑了，
   *   而 onUI_hide 里的复位**不一定来得及**（框架切 Activity 与定时器尾拍有竞争）。
   *   ⇒ 由本条（常显、定时器一直在跑）自己判：**Activity 一变，覆盖立刻作废**。
   *   这条比"指望每个页面记得清"可靠得多 —— 页面漏清只会少显示一次覆盖，
   *   不会把别人的名字粘在状态栏上。 */
  if (ovr) {
    if (!sOvrAct[0]) {
      snprintf(sOvrAct, sizeof(sOvrAct), "%s", act);   // 记下"是谁设的"
    } else if (strcmp(sOvrAct, act) != 0) {
      LOGD("导航栏: 标题覆盖过期（%s 设的，现在 act=%s）-> 作废", sOvrAct, act);
      pg::setNaviTitle(0);
      ovr = 0;
      sOvrAct[0] = 0;
    }
  } else {
    sOvrAct[0] = 0;
  }
  if (ovr) {
    title = ovr;                        // 覆盖优先（页面专属名字）
  } else {
    for (unsigned i = 0; i < sizeof(kTitleMap) / sizeof(kTitleMap[0]); ++i) {
      if (strcmp(kTitleMap[i].act, act) == 0) { title = kTitleMap[i].title; break; }
    }
  }
  if (!title) title = "口袋游戏机";      // 兜底：未知 Activity / 名字还没更新
  const bool actChanged = (strcmp(sLastAct, act) != 0);
  /* ⚠️ 覆盖生效时**不按 act 变化短路**：同一个 Activity 里切换子页（如进游戏）时
   *    act 不变、只有覆盖值变，若沿用原来的 `!actChanged && title 相同就返回`
   *    会永远刷不出来（那是"页面级名字永远停在旧值"的死法）。 */
  if (!force && strcmp(sLastTitle, title) == 0 && (ovr || !actChanged)) return;
  snprintf(sLastAct, sizeof(sLastAct), "%s", act);
  snprintf(sLastTitle, sizeof(sLastTitle), "%s", title);
  if (mTextNbTitlePtr) mTextNbTitlePtr->setText(title);
  LOGD("导航栏: 标题 act=%s -> %s%s", act[0] ? act : "(空)", title,
       ovr ? "（页面覆盖）" : "");
}

/* ---------------- 视频播放页（2026-09-16「检讨 UI 覆盖」修正 ②） ----------------
 * 三个播放页（IPTV 播放页 / 摄像头查看页 / 投屏页）的视频区是 **480x700 @y=0**，
 * 逻辑层按"等比铺满"下发 set_pos ⇒ 本条会永久压住画面最上面 52px
 * （= 画面的 7.4%，**真丢内容**）。视频层是硬件 disp 层，坐标不能动
 * （见 docs/iptv.md 的 set_pos/set_crop 说明）⇒ 走"屏保"那条老路：
 * **播放页里把本条收起来**，退出即恢复。
 *
 * ⚠️ 自愈兜底（必须有）：标志是全局粘性的，万一某个页面异常退出没清，
 *    本条就会**永远不回来**（用户看到的是"状态栏没了"）。所以这里额外校一次
 *    "当前 Activity 是不是那三个播放页之一"，不是就**就地清掉标志**。
 *    宁可少藏一次，也不能永久消失。 */
bool videoPageActive() {
  if (!pg::videoPage()) return false;
  const char *act = EASYUICONTEXT->currentAppName();
  /* ⚠️ **加视频播放页必须往这里加一行**（2026-09-20 补的教训）：
   *   下面这个白名单是"自愈兜底"用的 —— 不在名单里的 Activity 会被判定成
   *   "视频页标志残留"，随即 `pg::setVideoPage(false)`。于是新加的播放页
   *   表现成**导航栏又冒出来、把画面顶部 52px 压掉**（而页面代码看着完全正确）。
   *   新增：fairyActivity（飞天仙女）/ kittenActivity（可爱小猫）—— 见 docs/movie-app.md。
   *   ★ 2026-09-23：fairy / kitten **两个应用已下线**（用户要求"去掉电子宠物/小精灵/飞天仙女/
   *     可爱小猫这 4 个 APP 省空间"）⇒ 名单里也一并删掉。留着它们的名字不会出错（那些
   *     Activity 已经不存在了，永远匹配不上），但"名单里有已删页"会误导下一个人以为还在。 */
  const bool ok = act && (strcmp(act, "mainActivity") == 0 ||
                          strcmp(act, "iptvActivity") == 0 ||
                          strcmp(act, "cameraActivity") == 0);
  if (!ok) {
    pg::setVideoPage(false);
    LOGD("导航栏: 视频页标志残留（当前 act=%s）-> 自愈清掉",
         (act && act[0]) ? act : "(空)");
    return false;
  }
  return true;
}

/* ---------------- 电量（2026-09-16 用户需求：电池也搬到状态栏） ----------------
 * ★★ 2026-09-16 第二版（用户原话：「电池图标不对。做成对应的图片直接贴就不存在错位了」）：
 *   上一版是**三个控件拼**（外壳 + 电量条 + 闪电），电量条的 y 由这里的常量在运行时
 *   `setPosition` 决定 —— 而源稿声明的是 y=16、这里的常量却写成了 **y=5**
 *   ⇒ 电量条比外壳高 11px、顶出壳外，就是用户看到的"电池图标不对"。
 *   根因不是"常量填错了"，而是**同一份几何有两个来源**（源稿 + 运行时常量），必然漂。
 *   ⇒ 现在：**一个控件 + 一张整图**（`batt_<色>_<档>.png`，52x26，外壳/电量条/闪电都烘在图里），
 *     这里只 `setBackgroundPic` 一次，**永不 setPosition** ⇒ 结构上不可能错位。
 *   图由 `tools/ios_theme.py` 的 `gen_battery()` 生成（3 色 × 11 档 = 33 张，
 *   档位 0,10,...,100 与图名严格对应；闪电烘在 amber 档里，因为"充电 ⇒ amber"是同一件事）。
 *
 * ⚠️ 顺手调 `pg::Battery::tick()`：它内部 30s 节流采样。导航栏也要调的原因 ——
 *    主界面的 TIMER_LOOP 在独立 ftu 页（独立 Activity）里不保证还在跑，
 *    那样电量会永远停在进页前的值。tick() 幂等，多处调用无害。 */
const int kBattStep = 10;      // 图档粒度（%）：与 ios_theme.BATT_LEVELS 必须一致

void refreshBatt(bool force) {
  pg::Battery::tick();
  const int pct = pg::Battery::percent();
  const bool charging = pg::Battery::charging();
  const bool low = pg::Battery::low() && !charging;
  /* 低电红 / 充电琥珀 / 正常蓝（与 tools/ios_theme.py 的 BATT_FILL_COLORS 一致） */
  static const char *kFillColor[3] = {"blue", "red", "amber"};
  const int ci = low ? 1 : (charging ? 2 : 0);
  const int shown = (pct < 0) ? 0 : pct;      // 读不到电量 → 画空电池
  /* 落到 10% 一档的图（图就是按档烘的）。向下取档：45% 显示 40% 那档，
   * 宁可少画一点也不虚报（电量显示的通行做法）。 */
  const int lvl = (shown / kBattStep) * kBattStep;

  static int sLastLvl = -2, sLastCi = -1;
  if (!force && lvl == sLastLvl && ci == sLastCi) return;

  if (mImgNbBattPtr && (lvl != sLastLvl || ci != sLastCi)) {
    /* 名字放 static 缓冲：setBackgroundPic 只收路径指针，指向栈上临时串会悬垂。 */
    static char sBattPic[48];
    snprintf(sBattPic, sizeof(sBattPic), "images/batt_%s_%d.png", kFillColor[ci], lvl);
    mImgNbBattPtr->setBackgroundPic(sBattPic);
  }

  /* 百分比数字（读不到就显示 --%） */
  static int sLastShown = -2;
  if (shown != sLastShown) {
    sLastShown = shown;
    if (mTextNbBattPtr) {
      char buf[16];
      if (pct < 0) snprintf(buf, sizeof(buf), "--%%");
      else snprintf(buf, sizeof(buf), "%d%%", shown);
      mTextNbBattPtr->setText(buf);
    }
  }

  sLastLvl = lvl;
  sLastCi = ci;
}

/* ---------------- 「右滑返回」----------------
 * ⛔ 2026-09-16 用户要求**删掉视觉提示**（原话：「状态栏上面显示的返回主页去掉」）：
 *   原来这里跑一套"跟手滑入 + 停留 400ms"的动效（ImgNbBackHome + TextNbBackHome
 *   两个控件，还要在显示期间把页面标题藏掉避免叠字）。那一组控件与相关状态
 *   （sVisBack / sVisDx / sBackFlashMs）已随 ui/navibar.html 一起删除。
 *
 * ★ **手势本身保留**：判定仍在 onnavibarActivityTouchEvent 里（喂给 pg::swipe*），
 *   达标照样 goBack() —— 只是不再有任何视觉反馈。
 * ⚠️ 别因为"没反馈"就把手势也去掉：这是用户当初明确要的功能（见本文件头部需求 ②）。 */
void triggerBack() {
  /* ★ 画布游戏由主界面负责退出（它不是 Activity，goBack() 对它无效）——
   *   这里不抢着 goBack，否则两边都判、谁都不动。见 platform/PgSwipe.h 的说明。 */
  if (pg::gameMode()) {
    LOGD("导航栏: 右滑达标（在画布游戏里 → 交给主界面退游戏，本页不 goBack）");
    return;
  }
  LOGD("导航栏: 右滑达标 -> goBack()");
  /* ★ 用框架的 goBack()，而不是自己记"当前是哪个页"：
   *   它由框架按窗口栈处理（关闭当前 Activity / 回上一层），
   *   导航栏是 SysApp，不该关心上层是谁。 */
  EASYUICONTEXT->goBack();
}

/* ---------------- QA 自检通道 /tmp/pg_navibarcmd ---------------- */
void qaPoll() {
  FILE *f = fopen("/tmp/pg_navibarcmd", "rb");
  if (!f) return;
  char buf[512] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (n == 0 || strcmp(buf, sQaTag) == 0) return;   // 通道纪律：内容变化才执行
  snprintf(sQaTag, sizeof(sQaTag), "%s", buf);

  char *line = buf;
  char *nl = strchr(line, '\n');
  if (nl) *nl = 0;
  char *hash = strchr(line, '#');
  if (hash) *hash = 0;               // `#` 后是注释

  if (strncmp(line, "dump", 4) == 0) {
    /* 先强制刷一遍再打，否则读到的是上一拍的值（尤其在刚切页时） */
    refreshWifi(true);
    refreshTitle(true);
    refreshBatt(true);
    /* ⚠️ 标题/电量的字段是"状态栏三件套"的验收判据 —— 有它就不用抓屏 */
    LOGD("qa navibar: show=%d | wifi kind=%d ssid=%s | act=%s title=%s | batt=%d%% %s low=%d",
         EASYUICONTEXT->isNaviBarShow() ? 1 : 0, sLastKind, sLastSsid,
         sLastAct[0] ? sLastAct : "(空)", sLastTitle[0] ? sLastTitle : "(空)",
         pg::Battery::percent(), pg::Battery::stateText(), pg::Battery::low() ? 1 : 0);
  } else if (strncmp(line, "show", 4) == 0) {
    EASYUICONTEXT->showNaviBar();
    LOGD("qa navibar: showNaviBar()");
  } else if (strncmp(line, "hide", 4) == 0) {
    EASYUICONTEXT->hideNaviBar();
    LOGD("qa navibar: hideNaviBar()");
  } else if (strncmp(line, "back", 4) == 0) {
    LOGD("qa navibar: 手动 goBack()");
    EASYUICONTEXT->goBack();
  } else if (line[0]) {
    LOGD("qa navibar: 未知命令 '%s'", line);
  }
}

}  // namespace

/**
 * 注册定时器
 * ⚠️ 必须自己定义这张表 + 在 onUI_init 里展开 `INIT_UI_TIMERS`，
 *    否则 onUI_Timer 根本不会被调用（2026-09-16 实测：不展开 ⇒ dump 命令零响应）。
 */
static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_TICK, TICK_MS},
};

/**
 * @brief 界面初始化：显示导航栏 + 首刷状态 + 藏起"返回"动效
 * ⚠️ 不要在这里 hideNaviBar —— 它是**常显**的（与 statusbar 的"按需显隐"相反）。
 */
static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
#endif
  /* ★★ 必须**显式注册全局触摸监听**（与 statusbar 同一条实测结论）：
   *   SysApp（`EventApp<BaseApp>`）与普通 Activity 不同 —— **它收不到全局触摸**，
   *   而生成的宏只在 DESTROY 时 unregister，**没有配对的 register**。
   *   本页的"右滑返回"完全依赖这个回调 ⇒ 漏了它就整个手势都不触发
   *   （2026-09-16 实测：不注册时右滑零日志、零反应，注册后即通）。 */
  if (mnavibarPtr) {
    EASYUICONTEXT->registerGlobalTouchListener(mnavibarPtr);
    LOGD("导航栏: 已注册全局触摸监听（右滑返回用）");
  }
  /* 状态栏三件套首刷：标题（按当前 Activity）/ WiFi / 电量 */
  pg::Battery::init();     // 幂等：开机就有电量（否则第一条要等 30s 才采样）
  refreshTitle(true);
  refreshWifi(true);
  refreshBatt(true);
  EASYUICONTEXT->showNaviBar();
  LOGD("导航栏就绪（标题 + 电量 + WiFi + 右滑返回；窗口 480x52，见 ui/navibar.html）");
}

/**
 * @brief 当界面完全退出时触发
 */
static void onUI_quit() { LOGD("导航栏: onUI_quit"); }

/**
 * @brief 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

/**
 * @brief 定时器：兜住显隐 + 刷新 WiFi 状态 + 跑右滑动效
 */
static bool onUI_Timer(int id) {
  if (id != TIMER_TICK) return true;
  /* 每拍确认它该显示 / 该隐藏。
   * ⚠️ 方向别写反：导航栏**平时要显示**（statusbar 才是"必须收敛成隐藏"）。
   * ★★ 2026-09-16 用户需求：「屏保界面隐藏导航栏」——
   *   屏保是**整屏**画面（翻页钟 + 诗词），屏顶再压一条状态栏既难看也没意义。
   *   为什么在导航栏**自己**这里判、而不是去改 screensaver.cc：
   *   本页的定时器一直在跑（常显浮层），自己判 `isScreensaverOn()` 最省 ——
   *   少改一个文件、也不会漏掉别的进屏保路径（`saver on` QA / 超时 / 按键唤醒）。
   *   ⚠️ 顺序要紧：屏保时**不能**再走下面那条"收敛重新显示"，否则会和隐藏打架。 */
  /* ★★ 2026-09-16「检讨 UI 覆盖」修正 ②：视频播放页隐藏本条。
   *   三个播放页的视频区是 480x700 @y=0（等比铺满）⇒ 本条会永久压掉画面顶部 52px。
   *   与屏保同一条路子：本页自己判、自己收 —— 不改 3 个播放页的窗口几何
   *   （视频层是硬件 disp 层，动坐标风险高，见 docs/iptv.md）。
   *   ⚠️ 三条判断**互斥且有顺序**：视频页 → 屏保 → 收敛重新显示。
   *      顺序反了它会和"收敛重新显示"打架（和下面屏保那条同一个坑）。 */
  if (videoPageActive()) {
    if (EASYUICONTEXT->isNaviBarShow()) {
      EASYUICONTEXT->hideNaviBar();
      LOGD("导航栏: 视频播放页 -> 隐藏（把画面顶部 52px 让出来）");
    }
  } else if (EASYUICONTEXT->isScreensaverOn()) {
    if (EASYUICONTEXT->isNaviBarShow()) {
      EASYUICONTEXT->hideNaviBar();
      LOGD("导航栏: 屏保中 -> 隐藏");
    }
  } else if (!EASYUICONTEXT->isNaviBarShow()) {
    EASYUICONTEXT->showNaviBar();
    LOGD("导航栏: 收敛重新显示（框架把它标记成隐藏了）");
  }
  if (++sTick >= WIFI_EVERY_TICKS) {
    sTick = 0;
    /* 状态栏三件套：标题 / WiFi / 电量（各函数内部都有变化检测，没变就不写控件） */
    refreshTitle(false);
    refreshWifi(false);
    refreshBatt(false);
    qaPoll();
  }
  return true;
}

/**
 * @brief 触摸事件（**全局监听**：任何页面/浮层都会收到）
 *
 * 只做一件事：喂给 `pg::swipe*` 做"右滑返回"手势判定（达标在 triggerBack() 里 goBack）。
 * ⛔ 2026-09-16 起**不再画跟手动效**（用户要求删掉「返回主页」提示，见 triggerBack 的说明）。
 *
 * ⚠️ 返回 false = **不吞事件**：导航栏自己不放可点控件，
 *   能不能点到下面的控件由窗口命中决定，不由这里决定（与 statusbar 同一条纪律）。
 */
static bool onnavibarActivityTouchEvent(const MotionEvent &ev) {
  /* 诊断：第一次收到触摸时打一条（确认"全局监听真的生效"；之后不再刷屏） */
  static int sTouchSeen = 0;
  if (!sTouchSeen) {
    sTouchSeen = 1;
    LOGD("导航栏: 首次收到触摸 action=%d x=%d y=%d（全局监听生效）",
         (int)ev.mActionStatus, ev.mX, ev.mY);
  }
  switch (ev.mActionStatus) {
    case MotionEvent::E_ACTION_DOWN:
      pg::swipeDown(ev.mX, ev.mY);
      break;
    case MotionEvent::E_ACTION_MOVE:
      pg::swipeMove(ev.mX, ev.mY);
      break;
    case MotionEvent::E_ACTION_UP:
      if (pg::swipeUp(ev.mX, ev.mY)) triggerBack();
      break;
    case MotionEvent::E_ACTION_CANCEL:
      pg::swipeCancel();
      break;
    default:
      break;
  }
  return false;
}
