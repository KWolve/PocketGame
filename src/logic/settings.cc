#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * settings.cc - 系统设置（独立 ftu：settings.ftu -> settingsActivity）
 *
 * ★ 2026-09-16 用户需求：「把音效开关，wifi，音量调节，背光调节都做到系统设置里面」，
 *   并明确「音效开关从主界面移除」（主界面那颗 BtnSound 已删）。
 *
 * 四项各自的数据来源（**都不是新造的**，所以行为与原来一致）：
 *   静音 —— `pg::setMutedGlobal()`（真静音 = codec 输出开关；2026-09-17 用户明确
 *           「音效开关就是等于静音按键了，就不存在音效开关的说法」⇒ 不再有"音效开关"）
 *   WiFi —— `pg::WifiService`（zknet 封装：enabled/connected/ssid/setEnabled/openSystemSettings）
 *   音量 —— `pg::volumeStepGlobal(±1)` 走全局收口（状态栏 OSD 也会跟着弹）；
 *           "选网"按钮把音量写成一个具体值时要落盘 + `noteVolumePercent`
 *   背光 —— `BRIGHTNESSHELPER`（zkhardware 的 BrightnessHelper，0..100）
 *           ★ 这是**真背光**：设备上没有 /sys/class/backlight、PWM 通道也没人用，
 *             只有这条 SDK 通路（`BRIGHTNESSHELPER->setBrightness()`，实测 cur/max 可读）。
 *
 * 为什么本页**不用** ToolPage 外壳：ToolPage 是给"一个大数值 + 8 个功能键"的时钟类页面用的；
 * 本页是四项设置的原生控件页，形态更接近 wifi/radio 那两页，所以照它们的骨架写
 * （按键监听 + 长按返回 + 关屏保 + 自己的 QA 通道）。
 *
 * 自检（QA 通道 `/tmp/pg_settingscmd`）：
 *   dump                        打印四项当前值
 *   sound 0|1 / wifi 0|1        切开关
 *   vol <0-100> / bright <0-100> 直接设值（会自动落盘）
 *   quit                        退出本页（与点返回同一条路）
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/PgGame.h"          // pg::Host（store / audio）
#include "entry/EasyUIContext.h"
#include "platform/PgAudio.h"     // pg::volumeStepGlobal / noteVolumePercent
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgWifi.h"
#include "ui/ToolPage.h"          // pg::pgHost()
#include "utils/BrightnessHelper.h"
#include "utils/Log.h"

namespace {

const int TIMER_TICK = 1;
const int TICK_MS = 500;       // WiFi 状态/音量可能被别处改（按键、状态栏），半秒对齐一次

int sVolPct = -1;              // 画在屏上的值（-1 = 未知，变化检测用）
int sBrightPct = -1;
int sMuted = -1;              /* 静音态（2026-09-17：原来的"音效开关"改成真静音，见文件头） */
char sWifiTxt[64] = {0};
char sQaTag[512] = {0};

/* 拖动状态（2026-09-17：两条进度条改成**直接拖动**，见文件头与 html 的说明）。
 *   · sLastDrag*Ms：节流用。拖动回调是高频的（手指每动一点就来一次），
 *     而底层写的是 codec（每次 open/close ALSA 控制句柄）⇒ 必须限流。
 *   · sBarWriteUntilMs：**回写抑制窗**。syncAll 每 500ms 按真值回写条，
 *     用户还在拖时被拽回去会像"抢"，所以拖动后 1.2s 内不回写（时间窗到期自动恢复，
 *     不是粘性标志 —— 本工程栽过粘性状态的亏）。 */
int sLastDragVol = -1, sLastDragVolMs = 0;
int sLastDragBright = -1, sLastDragBrightMs = 0;
long sBarWriteUntilMs = 0;
/* ★★ `setProgress()` **也会触发 onProgressChanged**（2026-09-17 真机实测）：
 *   fun 生成的绑定是 `disp.on(..., UI_EVENT_TYPE_PROGRESS, ...)` —— **事件队列异步派发**，
 *   所以 syncAll 每 500ms 回写一次条，就会"假拖动"一次回调，
 *   于是再反向写一遍 codec（日志里能看到没人碰屏幕却打出
 *   `拖动音量 -> 请求 88% 实际 87%`）。
 *   ⇒ 记下"最近一次程序回写"的时刻，回调在这之后一个小窗口内直接忽略。
 *     窗口期间不会有真实拖动被误伤 —— 因为用户一拖 `sBarWriteUntilMs` 就把它推后了。 */
long sBarProgSetMs = 0;
const int kProgEchoMs = 300;       // 程序回写后的"回声"忽略窗
const int kDragMinGapMs = 80;      // 两次写入的最小间隔
const int kDragMinStep = 2;        // 进度至少变这么多才写
const int kBarHoldMs = 1200;       // 拖动后多久内不回写条
const int kBrightMinPct = 10;      // 背光下限（全黑会让用户以为死机）

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 取值 ---------------- */
int readVolume() {
  pg::Host *h = pg::pgHost();
  if (!h) return -1;
  return h->volumePercent();           // 落盘值就是"当前音量"（音量键每次都会写它）
}

int readBright() { return BRIGHTNESSHELPER->getBrightness(); }

void wifiText(char *buf, int n) {
  /* 直接用平台层（不用 Host 转发）：本页已经把四项能力都摆在眼前，
   * 多一层转发只多一处要同步的地方。 */
  if (!pg::WifiService::enabled()) {
    snprintf(buf, n, "已关闭");
    return;
  }
  if (!pg::WifiService::connected()) {
    snprintf(buf, n, "未连接");
    return;
  }
  char ssid[40] = {0};
  pg::WifiService::ssid(ssid, sizeof(ssid));
  snprintf(buf, n, "%s", ssid[0] ? ssid : "已连接");
}

/* ---------------- 写控件（全部做变化检测：框架 setText 会重绘） ---------------- */
void syncAll(bool force) {
  pg::Host *h = pg::pgHost();
  if (!h) return;
  /* ★ 静音态（2026-09-17 起这一行是"静音 开/关"）：值直接问 pg 层的唯一真值，
   *   所以音量面板里点图标、或按键解除静音，本页下一拍就会跟上。 */
  if (force || sMuted != (pg::isMutedGlobal() ? 1 : 0)) {
    sMuted = pg::isMutedGlobal() ? 1 : 0;
    if (mTextSetSoundValPtr) mTextSetSoundValPtr->setText(sMuted ? "开" : "关");
  }
  const int vol = readVolume();
  if (vol >= 0 && (force || vol != sVolPct)) {
    sVolPct = vol;
    if (mTextSetVolValPtr) {
      char b[32];
      snprintf(b, sizeof(b), "音量  %d%%", vol);
      mTextSetVolValPtr->setText(b);
    }
    // ★ 进度条**正在被拖时不要回写**（否则 500ms 的定时器会把手指位置拽回去）
    if (mBarSetVolPtr && nowMs() > sBarWriteUntilMs) {
      sBarProgSetMs = nowMs();          // 记下"这次是程序写的"，回调里要忽略它的回声
      mBarSetVolPtr->setProgress(vol);
    }
  }
  const int br = readBright();
  if (br >= 0 && (force || br != sBrightPct)) {
    sBrightPct = br;
    if (mTextSetBrightValPtr) {
      char b[32];
      snprintf(b, sizeof(b), "背光  %d%%", br);
      mTextSetBrightValPtr->setText(b);
    }
    if (mBarSetBrightPtr && nowMs() > sBarWriteUntilMs) {
      sBarProgSetMs = nowMs();
      mBarSetBrightPtr->setProgress(br);
    }
  }
  char w[64] = {0};
  wifiText(w, sizeof(w));
  if (force || strcmp(w, sWifiTxt) != 0) {
    snprintf(sWifiTxt, sizeof(sWifiTxt), "%s", w);
    if (mTextSetWifiValPtr) mTextSetWifiValPtr->setText(w);
  }
}

void quitPage() {
  LOGD("settings: 退出该页（= 点返回按钮）");
  EASYUICONTEXT->closeActivity("settingsActivity");
}

/* ---------------- QA 自检通道 /tmp/pg_settingscmd ---------------- */
void qaPoll() {
  FILE *f = fopen("/tmp/pg_settingscmd", "rb");
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

  pg::Host *h = pg::pgHost();
  if (strncmp(line, "dump", 4) == 0) {
    LOGD("qa settings: muted=%d vol=%d bright=%d wifi=%s", sMuted, sVolPct, sBrightPct,
         sWifiTxt);
  } else if (strncmp(line, "sound ", 6) == 0) {
    /* 命令字面沿用 `sound 0|1`（1 = 有声、0 = 静音），但落地已经是**真静音**开关。
     * 另有 `mute 0|1` 同义（1 = 静音），给新脚本用。 */
    const bool on = (line[6] == '1');
    pg::setMutedGlobal(!on);
    LOGD("qa settings: sound -> %s（回读 muted=%d）", on ? "开" : "关",
         pg::isMutedGlobal() ? 1 : 0);
    syncAll(true);
  } else if (strncmp(line, "mute ", 5) == 0) {
    /* 与 `sound` 同义的**正向**写法（1 = 静音），新脚本用它更直观 */
    const bool m = (line[5] == '1');
    const bool ok = pg::setMutedGlobal(m);
    LOGD("qa settings: mute -> %d（%s，回读 muted=%d）", m ? 1 : 0,
         ok ? "成功" : "失败", pg::isMutedGlobal() ? 1 : 0);
    syncAll(true);
  } else if (strncmp(line, "wifi ", 5) == 0) {
    const bool on = (line[5] == '1');
    pg::WifiService::setEnabled(on);
    LOGD("qa settings: wifi -> %s（回读 enabled=%d）", on ? "开" : "关",
         pg::WifiService::enabled() ? 1 : 0);
    syncAll(true);
  } else if (strncmp(line, "vol ", 4) == 0) {
    int v = atoi(line + 4);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (h) {
      h->setVolumePercent(v);             // 内部：写 codec + 落盘 + noteVolumePercent
      LOGD("qa settings: vol %d -> %d%%", v, h->volumePercent());
    }
    syncAll(true);
  } else if (strncmp(line, "bright ", 7) == 0) {
    int v = atoi(line + 7);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    BRIGHTNESSHELPER->setBrightness(v);
    LOGD("qa settings: bright %d -> 回读 %d", v, BRIGHTNESSHELPER->getBrightness());
    syncAll(true);
  } else if (strncmp(line, "quit", 4) == 0) {
    LOGD("settings: QA quit");
    quitPage();
  } else if (line[0]) {
    LOGD("qa settings: 未知命令 '%s'", line);
  }
}

/* ==================== 物理按键 ==================== */
class SettingsKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键）。必须在第一句 —— 框架按键分发是
     *   短路式的，本页若先返回 true，屏保页自己的监听器可能收不到按键。 */
    if (pg::wakeSaverByKey(ke)) return true;
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = t;
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;
    const int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= 700) {
      LOGD("settings: 长按 %dms -> 退出", held);
      quitPage();
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {   // 音量±（全局收口 → OSD 也会弹）
      const int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("settings: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      syncAll(true);
      return true;
    }
    if (ke.mKeyCode == 108) {                          // 暂停键短按 = 返回
      quitPage();
      return true;
    }
    return true;
  }

 private:
  static int sDownCode;
  static long sDownMs;
};
int SettingsKeys::sDownCode = -1;
long SettingsKeys::sDownMs = 0;
SettingsKeys sKeys;

}  // namespace

/* ==================================================================
 *  回调（这些函数名由 fun 生成的头文件声明，必须在**全局作用域**定义）
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
#endif
  /* 本页要一直在前台操作：关掉屏保（与 ToolPage / wifi 页同一条纪律） */
  EASYUICONTEXT->setScreensaverEnable(false);
  EASYUICONTEXT->registerKeyListener(&sKeys);
  /* ★ 2026-09-16：两个"行内数值"文字压在各自的整行按钮上（设计如此：数值显示在行右端），
   *   而 `touchable=false` **不等于触摸穿透** —— 它照样吃掉矩形范围内的触摸，
   *   于是 BtnSetSound / BtnSetWifi 的右半部分点不到（tools/check_overlap.py 一直在报）。
   *   按 MCP `uicontrols/touch-events.md` 的实测结论补 `setTouchPass(true)`：
   *   装饰文字放行，事件落到下层的行按钮上。⚠️ touchPass 没有 json 字段，只能代码设。 */
  if (mTextSetSoundValPtr) {
    mTextSetSoundValPtr->setTouchable(false);
    mTextSetSoundValPtr->setTouchPass(true);
  }
  if (mTextSetWifiValPtr) {
    mTextSetWifiValPtr->setTouchable(false);
    mTextSetWifiValPtr->setTouchPass(true);
  }
  syncAll(true);
  LOGD("系统设置页就绪（音效/WiFi/音量/背光；背光走 BRIGHTNESSHELPER）");
}

static void onUI_quit() {
  EASYUICONTEXT->unregisterKeyListener(&sKeys);
  EASYUICONTEXT->setScreensaverEnable(true);   // 离页恢复屏保
  LOGD("系统设置页: onUI_quit");
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

/**
 * @brief 页面被唤起时（独立 ftu 必须自己管屏保：主界面不在前台时它的屏保策略不会跑）
 */
static void onUI_show() {
  LOGD("settings: onUI_show");
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  syncAll(true);
}

static void onUI_intent(const Intent *intent) { (void)intent; }

/** 页面被盖住时（进别的页面）—— 与 onUI_show 成对，必须实现（否则链接期缺符号） */
static void onUI_hide() { LOGD("settings: onUI_hide"); }

/* ==================== 两条进度条的拖动回调（2026-09-17 用户需求） ====================
 * 用户原话：「进度条不需要按键，直接拖动就好了」⇒ ＋/－ 按钮已从源稿删除，
 * 条本身就是操作面。三个必须做对的地方：
 *
 * ① **节流**：回调是高频的（手指每移动一点触发一次），而 `setVolumePercent` 内部是
 *    open→probe→write→read→close 一整套 ALSA 控制操作 ⇒ 不节流会拖出卡顿。
 *    判据：进度变化 ≥ kDragMinStep 或距上次 ≥ kDragMinGapMs 才写。
 * ② **不弹全局音量 OSD**：用 `pg::suppressVolumeOsd()` 开一个时间窗
 *    （用户正看着本页这条，再弹一个 OSD 是噪音；窗到期自动失效）。
 * ③ **不让定时器抢**：`sBarWriteUntilMs` 让 syncAll 在拖动后一段时间内不回写条。
 *
 * ⚠️ 音量值一律以**回读**为准：codec 把 0..100 量化过（实测 90% → 88%），
 *    所以文字显示的是 `setVolumePercentGlobal()` 的返回值，不是手指请求值。
 */
static void onProgressChanged_BarSetVol(ZKSeekBar *view, int progress) {
  (void)view;
  const long t = nowMs();
  if (t - sBarProgSetMs < kProgEchoMs) return;   // 这是 syncAll 回写引发的"回声"，不是用户拖的
  if (sLastDragVol >= 0 && progress != sLastDragVol &&
      (progress - sLastDragVol < kDragMinStep && sLastDragVol - progress < kDragMinStep) &&
      (t - sLastDragVolMs) < kDragMinGapMs) {
    return;                                    // 小步 + 密集 ⇒ 丢掉这次
  }
  sLastDragVol = progress;
  sLastDragVolMs = t;
  sBarWriteUntilMs = t + kBarHoldMs;
  pg::suppressVolumeOsd(kBarHoldMs);
  const int got = pg::setVolumePercentGlobal(progress);
  if (got < 0) return;
  LOGD("settings: 拖动音量 -> 请求 %d%% 实际 %d%%", progress, got);
  sVolPct = got;
  if (mTextSetVolValPtr) {
    char b[32];
    snprintf(b, sizeof(b), "音量  %d%%", got);
    mTextSetVolValPtr->setText(b);
  }
}

static void onProgressChanged_BarSetBright(ZKSeekBar *view, int progress) {
  (void)view;
  const long t = nowMs();
  if (t - sBarProgSetMs < kProgEchoMs) return;   // 同上：忽略程序回写的回声
  if (sLastDragBright >= 0 && progress != sLastDragBright &&
      (progress - sLastDragBright < kDragMinStep &&
       sLastDragBright - progress < kDragMinStep) &&
      (t - sLastDragBrightMs) < kDragMinGapMs) {
    return;
  }
  sLastDragBright = progress;
  sLastDragBrightMs = t;
  sBarWriteUntilMs = t + kBarHoldMs;
  int want = progress;
  if (want < kBrightMinPct) {
    want = kBrightMinPct;                      // 下限 10%（与旧版按 － 的行为一致）
    if (mBarSetBrightPtr) {
      sBarProgSetMs = t;                       // 吸附也是"程序写的"，别让它再回声一次写 codec
      mBarSetBrightPtr->setProgress(want);     // 滑块吸回下限
    }
  }
  BRIGHTNESSHELPER->setBrightness(want);
  const int got = BRIGHTNESSHELPER->getBrightness();
  LOGD("settings: 拖动背光 -> 请求 %d%% 实际 %d%%", progress, got);
  sBrightPct = got;
  if (mTextSetBrightValPtr) {
    char b[32];
    snprintf(b, sizeof(b), "背光  %d%%", got);
    mTextSetBrightValPtr->setText(b);
  }
}

/**
 * @brief 定时器：半秒对齐一次（音量/背光可能被按键或别处改）
 */
static bool onUI_Timer(int id) {
  if (id != TIMER_TICK) return true;
  syncAll(false);
  qaPoll();
  return true;
}

static bool onsettingsActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;    // 不吞事件：本页控件要正常收触摸
}

/* ---------------- 按钮回调 ---------------- */
static bool onButtonClick_BtnSetBack(ZKButton *p) {
  (void)p;
  LOGD("settings: 点返回");
  quitPage();
  return true;
}

/* ★ 2026-09-17 用户：「音效开关就是等于静音按键了，就不存在音效开关的说法」
 *   ⇒ 这一行改成**真静音**开关（`pg::setMutedGlobal`，= codec 输出开关），
 *     与全局音量面板左侧那个可点图标是**同一个开关**，两处状态天然一致。
 *     原来走的 `Host::setSoundOn()` 只管游戏音效 SFX（日志写着"流照旧，只改是否写音效"），
 *     语义不同，已经不再从这里暴露。 */
static bool onButtonClick_BtnSetSound(ZKButton *p) {
  (void)p;
  const bool want = !pg::isMutedGlobal();
  const bool ok = pg::setMutedGlobal(want);
  LOGD("settings: 静音 -> %s（%s）", want ? "开" : "关", ok ? "成功" : "失败");
  syncAll(true);
  return true;
}

static bool onButtonClick_BtnSetWifi(ZKButton *p) {
  (void)p;
  const bool want = !pg::WifiService::enabled();
  pg::WifiService::setEnabled(want);
  LOGD("settings: WiFi -> %s（回读 enabled=%d）", want ? "开" : "关",
       pg::WifiService::enabled() ? 1 : 0);
  syncAll(true);
  return true;
}

static bool onButtonClick_BtnSetWifiPage(ZKButton *p) {
  (void)p;
  LOGD("settings: 打开系统 WiFi 设置页");
  pg::WifiService::openSystemSettings();
  return true;
}

/* ★ 2026-09-17：`BtnSetVolDown/Up`、`BtnSetBrightDown/Up` 四个按钮回调与 `stepBright()`
 *   已随源稿里那四个按钮一起删除 —— 用户要求「进度条不需要按键，直接拖动就好了」。
 *   上面的 onProgressChanged_* 就是唯一的操作入口。 */
