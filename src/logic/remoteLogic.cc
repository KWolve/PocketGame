#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif
/*
 * remoteLogic.cc - 蓝牙遥控 独立应用（remote.ftu）逻辑层
 *
 * 架构：**独立功能 = 独立 ftu + 独立 logic**（与 wifiLogic.cc 同构）。
 *   界面：ui/remote.html  -> remote.ftu  -> remoteActivity
 *   能力：platform/PgBt（单例 pg::Bt::instance()），本文件**不 include 任何 btstack 头**。
 *
 * 页面：
 *   遥控页（根层控件，默认可见）：状态卡 4 行 + 3x3 遥控按键 + 已发送提示
 *   学习页（WinRemoteLearn，初始隐藏）：主机状态 + 周边设备列表 + 已学到的报告 + 重扫/断开
 *
 * 线程模型：pg::Bt 内部自己管 BT 线程（rtk_init 20~40 秒，不能放 UI 线程），
 *   这里只做「读状态 → 写控件」，且**先比值再写**（每帧无条件 setText 会触发重绘风暴，
 *   实测 CPU 打到 127%、界面像卡死 —— 见工程记忆第 14 条）。
 *
 * 按键（本板物理键 103=音量- / 105=音量+ / 108=A）：
 *   短按 A/B/C → 发 HID 键（播放暂停 / 音量+ / 音量-）；
 *   长按 ≥700ms → closeActivity 返回主界面（本板 gpio-keys 没有 autorepeat，
 *   框架的 E_KEY_LONG_PRESS 永不触发，必须自己按 DOWN→UP 时长判定 —— 见工程记忆）。
 *
 * ⚠️ BT 只初始化一次：rtk_init 间歇性失败且失败后难恢复（电源域要物理冷启动），
 *   所以 pg::Bt 是单例、start() 幂等；本页反复进出不会重初始化。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgAlarm.h"
#include "platform/PgBt.h"
#include "utils/Log.h"

#ifdef FUN_BUILD
#include "entry/EasyUIContext.h"
#endif

namespace {

/* ==================== 常量 ==================== */
const int TIMER_UI = 1;
const int UI_MS = 300;           // 状态轮询周期（足够跟手，又不至于刷屏）
const int KEY_MS = 100;          // 按键长按轮询周期（300ms 太粗，长按判定会飘到 1s）
const int TIMER_KEY = 2;         // 按键长按轮询用的定时器 id
const long LONG_PRESS_MS = 700;  // 与主界面 KeyRouter 一致
const int TIP_HOLD_MS = 900;     // "已发送：xx" 的停留时长

/* 9 个遥控按键：Consumer Control 位图 + 中文标签（与 PgBt 的报告描述符一一对应） */
const uint16_t kMasks[9] = {
    1u << 0,   // 电源
    1u << 9,   // 播放/暂停
    1u << 8,   // 静音
    1u << 5,   // 上一首
    1u << 12,  // 主页
    1u << 4,   // 下一首
    1u << 6,   // 音量+
    1u << 13,  // 返回
    1u << 7,   // 音量-
};
const char *kLabels[9] = {"电源", "播放", "静音", "上一首", "主页",
                          "下一首", "音量+", "返回", "音量-"};

/* 配色（与 remote.json 保持一致） */
const uint32_t C_KEY_BG = 0xFF2C2C2E;
const uint32_t C_KEY_BG_HIT = 0xFF30D158;
const uint32_t C_ACCENT_READY = 0xFF0A84FF;
const uint32_t C_ACCENT_OK = 0xFF30D158;
const uint32_t C_ACCENT_OFF = 0xFF2C2C2E;
const uint32_t C_ACCENT_WARN = 0xFFC87A3C;
const uint32_t C_TEXT = 0xFFF2F2F7;
const uint32_t C_TEXT_OK = 0xFF30D158;
const uint32_t C_TEXT_DIM = 0xFF78849A;
const uint32_t C_TEXT_HINT = 0xFF9A9AA0;
const uint32_t C_TEXT_WARN = 0xFFFF9F0A;

/* ==================== 运行期状态 ==================== */
pg::Bt *sBt = 0;
int sSent = 0;              // 已发送按键数（打点/统计）
uint16_t sHitMask = 0;      // 最近命中的按键位图（按钮高亮用）
long sHitUntil = 0;         // 高亮截止时刻
int sPage = 0;              // 0 = 遥控页，1 = 学习页
int sDevCount = -1;         // 上次刷新列表时的设备数（变化才 refreshListView）
int sLearnCount = -1;
bool sStarted = false;

/* 文本缓存：只在内容变化时才 setText */
char sLastState[64], sLastName[64], sLastPair[24], sLastScan[32], sLastTip[96];
char sLastSent[64], sLastHost[32], sLastPeer[32], sLastDevHdr[64], sLastLearnHdr[64];
char sLastKeyLabel[9][24];
int sLastKeyStyle[9];

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 只在变化时写控件（避免每帧重绘） */
void setT(ZKTextView *tv, const char *val, char *cache, int cn) {
  if (!tv) return;
  if (!val) val = "";
  if (strncmp(cache, val, (size_t)cn) == 0) return;
  tv->setText(val);
  snprintf(cache, (size_t)cn, "%s", val);
}

/* 改底色必须 setBackgroundColor + setBgStatusColor 一起（只设前者看不到变化） */
void setBg(ZKBase *v, uint32_t color) {
  if (!v) return;
  v->setBackgroundColor(color);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
}

uint32_t bgByState(int st, bool conn, int err) {
  if (err == -1) return C_ACCENT_WARN;
  if (st == 2) return conn ? C_ACCENT_OK : C_ACCENT_READY;
  return C_ACCENT_OFF;
}

/* ==================== 页面切换 ==================== */
void showPage(int p) {
  if (p < 0 || p > 1) return;
  if (sPage == p) return;
  sPage = p;
  LOGD("remoteLogic: 切到%s页", p ? "学习" : "遥控");
  if (mWinRemoteLearnPtr) {
    if (p == 1) mWinRemoteLearnPtr->showWnd();
    else mWinRemoteLearnPtr->hideWnd();
  }
  if (p == 1) {
    /* ⚠️ 不要每次进学习页都清空重扫：实测本板控制器跑一阵会"哑火"
     *   （advCount 冻住、不再上报广播）——清空后就再也填不回来，用户看到恒空的列表。
     *   所以只有列表本来就空时才主动扫一次；用户想刷新就点「重新扫描」。 */
    int n = sBt ? sBt->scannedCount() : 0;
    if (n == 0 && sBt) {
      sBt->hostScanStart();
      LOGD("remoteLogic: 设备列表为空，进页自动扫一次");
    } else {
      LOGD("remoteLogic: 沿用已收集的 %d 个设备", n);
    }
    sDevCount = -1;
    sLearnCount = -1;
  }
}

/* ==================== 发一个遥控键 ==================== */
void fire(int idx) {
  if (idx < 0 || idx >= 9) return;
  if (!sBt) return;
  sBt->sendKey(kMasks[idx]);
  sSent++;
  sHitMask = kMasks[idx];
  sHitUntil = nowMs() + TIP_HOLD_MS;
  LOGD("remoteLogic: 发按键 #%d %s mask=0x%04x (累计 %d)", idx, kLabels[idx],
       kMasks[idx], sSent);
}

/* ==================== 界面同步（先比值再写） ==================== */
void syncSendPage() {
  if (!sBt) return;
  int st = sBt->state();
  bool conn = sBt->connected();
  int err = sBt->errCode();
  char b[96];

  uint32_t accent = bgByState(st, conn, err);
  setBg(mBarRemotePtr, accent);

  setT(mTextRemoteStateValuePtr, sBt->stateText(), sLastState, sizeof(sLastState));
  if (mTextRemoteStateValuePtr) {
    mTextRemoteStateValuePtr->setTextColor(
        err == -1 ? 0xFFFF9F0A : (st == 2 ? (conn ? C_TEXT_OK : C_TEXT) : C_TEXT_DIM));
  }
  setT(mTextRemoteDevValuePtr, sBt->name(), sLastName, sizeof(sLastName));

  bool paired = sBt->paired();
  setT(mTextRemotePairValuePtr, paired ? "已配对" : "未配对", sLastPair,
       sizeof(sLastPair));
  if (mTextRemotePairValuePtr) {
    mTextRemotePairValuePtr->setTextColor(paired ? C_TEXT_OK : C_TEXT_DIM);
  }

  int adv = sBt->advCount();
  snprintf(b, sizeof(b), "%d 条广播", adv);
  setT(mTextRemoteScanValuePtr, b, sLastScan, sizeof(sLastScan));
  if (mTextRemoteScanValuePtr) {
    mTextRemoteScanValuePtr->setTextColor(adv > 0 ? C_TEXT_OK : C_TEXT_DIM);
  }

  /* 状态提示行：失败 > 已连接 > 就绪等手机 > 初始化中 */
  if (err == -1) {
    setT(mTextRemoteTipPtr, "初始化失败：BT 需断电冷启动后重试", sLastTip,
         sizeof(sLastTip));
    if (mTextRemoteTipPtr) mTextRemoteTipPtr->setTextColor(0xFFFF9F0A);
  } else if (conn) {
    setT(mTextRemoteTipPtr, "已连接 —— 按键会直接发给主机", sLastTip, sizeof(sLastTip));
    if (mTextRemoteTipPtr) mTextRemoteTipPtr->setTextColor(C_TEXT_OK);
  } else if (st == 2) {
    snprintf(b, sizeof(b), "用手机蓝牙搜索 \"%s\" 并连接", sBt->name());
    setT(mTextRemoteTipPtr, b, sLastTip, sizeof(sLastTip));
    if (mTextRemoteTipPtr) mTextRemoteTipPtr->setTextColor(0xFF9A9AA0);
  } else {
    setT(mTextRemoteTipPtr, "正在初始化蓝牙（约需 20~40 秒）...", sLastTip,
         sizeof(sLastTip));
    if (mTextRemoteTipPtr) mTextRemoteTipPtr->setTextColor(0xFF9A9AA0);
  }

  /* 最近发送提示 + 命中高亮 */
  bool hit = (sHitUntil > nowMs() && sHitMask);
  if (hit) {
    snprintf(b, sizeof(b), "已发送：%s", pg::Bt::keyName(sHitMask));
    setT(mTextRemoteSentPtr, b, sLastSent, sizeof(sLastSent));
  } else {
    setT(mTextRemoteSentPtr, "", sLastSent, sizeof(sLastSent));
  }

  /* 3x3 按键标签 + 高亮（标签基本不变，一次写完就不再写） */
  for (int i = 0; i < 9; ++i) {
    ZKButton *btn = 0;
    switch (i) {
      case 0: btn = mBtnR0Ptr; break;
      case 1: btn = mBtnR1Ptr; break;
      case 2: btn = mBtnR2Ptr; break;
      case 3: btn = mBtnR3Ptr; break;
      case 4: btn = mBtnR4Ptr; break;
      case 5: btn = mBtnR5Ptr; break;
      case 6: btn = mBtnR6Ptr; break;
      case 7: btn = mBtnR7Ptr; break;
      case 8: btn = mBtnR8Ptr; break;
      default: break;
    }
    if (!btn) continue;
    if (strcmp(sLastKeyLabel[i], kLabels[i]) != 0) {
      snprintf(sLastKeyLabel[i], sizeof(sLastKeyLabel[i]), "%s", kLabels[i]);
      btn->setText(sLastKeyLabel[i]);
    }
    int style = (hit && sHitMask == kMasks[i]) ? 1 : 0;
    if (sLastKeyStyle[i] != style) {
      sLastKeyStyle[i] = style;
      setBg(btn, style ? C_KEY_BG_HIT : C_KEY_BG);
    }
  }
}

void syncLearnPage() {
  if (!sBt) return;
  char b[96];

  int hs = sBt->hostState();
  setT(mTextHostStateValuePtr, sBt->hostStateText(), sLastHost, sizeof(sLastHost));
  if (mTextHostStateValuePtr) {
    mTextHostStateValuePtr->setTextColor(hs == 3 ? C_TEXT_OK : C_TEXT);
  }
  setT(mTextHostPeerValuePtr, sBt->hostPeer(), sLastPeer, sizeof(sLastPeer));

  int dn = sBt->scannedCount();
  if (dn != sDevCount) {
    snprintf(b, sizeof(b), "周边设备 %d 个（点一行连接）", dn);
    setT(mTextRemoteDevHdrPtr, b, sLastDevHdr, sizeof(sLastDevHdr));
    sDevCount = dn;
    if (mListRemoteDevPtr) mListRemoteDevPtr->refreshListView();
  }

  int ln = sBt->learnedCount();
  if (ln != sLearnCount) {
    snprintf(b, sizeof(b), "已学到 %d 条（按对方遥控器）", ln);
    setT(mTextRemoteLearnHdrPtr, b, sLastLearnHdr, sizeof(sLastLearnHdr));
    if (mTextRemoteLearnHdrPtr) {
      mTextRemoteLearnHdrPtr->setTextColor(ln > 0 ? C_TEXT_OK : 0xFF9A9AA0);
    }
    sLearnCount = ln;
    if (mListRemoteLearnedPtr) mListRemoteLearnedPtr->refreshListView();
  }
}

void syncUi() {
  if (sPage == 0) syncSendPage();
  else syncLearnPage();
}

/* ==================== 物理按键（本 ftu 在前台时） ==================== */
/* 本板 gpio-keys 没有 autorepeat，框架 E_KEY_LONG_PRESS 永不触发
 * → 自己记 DOWN 时刻，UP 时算时长（与 wifiLogic / 主界面 KeyRouter 一致）。 */
class RemoteKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键，不顺手调音量/停流）。
     *   ⚠️ 必须在第一句：本页只是 hide、监听器还挂在链上，而框架按键分发是**短路式**的
     *   —— 本页若先返回 true，屏保页自己的监听器可能收不到按键。见 platform/PgSaver.h。 */
    if (pg::wakeSaverByKey(ke)) return true;
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = nowMs();
      sLPFired = false;
      return true;  // 按下先吞掉，避免落到框架默认行为
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_UP) {
      if (ke.mKeyCode != sDownCode) return false;
      int held = (int)(nowMs() - sDownMs);
      sDownCode = -1;
      if (held >= LONG_PRESS_MS && !sLPFired) {
        LOGD("remoteLogic: 长按 %dms -> 返回主界面", held);
        EASYUICONTEXT->closeActivity("remoteActivity");
        return true;
      }
      int idx = -1;
      if (ke.mKeyCode == 108) idx = 1;       // A → 播放/暂停
      else if (ke.mKeyCode == 105) idx = 6;  // B → 音量+
      else if (ke.mKeyCode == 103) idx = 8;  // C → 音量-
      if (idx >= 0) fire(idx);
      return true;
    }
    return false;
  }

  /* ★ 长按达标**立刻**返回，不等按键抬起。
 *   本板 gpio-keys **没有 autorepeat** ⇒ 长按期间内核一个事件都不发；只在 E_KEY_UP 里
 *   判长按的话，用户按满 700ms 还得一直按到松手才切页 —— 手感就是"按了不动、松手才跳"。
 *   所以由页面定时器轮询"按下且已超时"。 */

  static bool longPressReady() {
    if (sDownCode < 0 || sLPFired) return false;      // 有键按着（不分键码：
                                                      // 与本页原来的语义一致 ——
                                                      // 任何键按住 >=700ms 都返回）
    if (nowMs() - sDownMs < LONG_PRESS_MS) return false;
    sLPFired = true;   // 只触发一次
    return true;
  }

 private:
  static int sDownCode;
  static long sDownMs;
  static bool sLPFired;
};
int RemoteKeys::sDownCode = -1;
long RemoteKeys::sDownMs = 0;
bool RemoteKeys::sLPFired = false;

/* 定时器每 100ms 调一次：达标就立刻返回主界面（不等抬手）。 */
void tickKeyLongPress() {
  if (RemoteKeys::longPressReady()) {
    LOGD("remoteLogic: 长按达标（不等抬起）-> 返回主界面");
    EASYUICONTEXT->closeActivity("remoteActivity");
  }
}
RemoteKeys sKeyRouter;

/* ==================== QA 自检通道（免触摸验收） ====================
 * 本板 mt_test 触摸注入时灵时不灵，原生控件又走不了 dispatchCanvasTouch，
 * 所以开一条文件驱动的命令通道：整份内容变化时逐行执行。
 *   send <0..8>    — 发第 N 个遥控键（0 电源 1 播放 2 静音 3 上一首 4 主页
 *                    5 下一首 6 音量+ 7 返回 8 音量-）
 *   page 0|1       — 切页（0 遥控 / 1 学习）
 *   scan           — 重新扫描（学习页）
 *   conn <idx>     — 连接周边设备列表第 idx 个
 *   disc           — 断开对端
 *   learn          — 打印学到的报告
 *   dump           — 打印当前状态
 *   selftest       — 用我们自己的描述符自验证 HID 报告解析器
 *   back           — 关闭本页返回主界面
 * ⚠️ 每行内容必须不同（尾部加序号），相同内容会被去重不执行。
 */
char sCmdLast[512] = {0};

void cmdDump() {
  LOGD("remoteLogic: dump 页=%s state=%d(%s) err=%d conn=%d paired=%d adv=%d "
       "name='%s' sent=%d | host=%d(%s) peer=%s dev=%d learned=%d",
       sPage ? "学习" : "遥控", sBt->state(), sBt->stateText(), sBt->errCode(),
       sBt->connected() ? 1 : 0, sBt->paired() ? 1 : 0, sBt->advCount(), sBt->name(),
       sSent, sBt->hostState(), sBt->hostStateText(), sBt->hostPeer(),
       sBt->scannedCount(), sBt->learnedCount());
}

void cmdRun(char *line) {
  LOGD("remoteLogic: cmd '%s'", line);
  if (!sBt) return;
  if (strncmp(line, "send", 4) == 0) {
    fire(atoi(line + 4));
  } else if (strncmp(line, "page", 4) == 0) {
    showPage(atoi(line + 4));
  } else if (strcmp(line, "scan") == 0) {
    sBt->hostScanStart();
    sDevCount = -1;
  } else if (strncmp(line, "conn", 4) == 0) {
    sBt->hostConnect(atoi(line + 4));
  } else if (strcmp(line, "disc") == 0) {
    sBt->hostDisconnect();
  } else if (strcmp(line, "learn") == 0) {
    int n = sBt->learnedCount();
    LOGD("remoteLogic: learned %d 条", n);
    for (int i = 0; i < n; ++i) {
      LOGD("remoteLogic:   [%d] %s mask=0x%04x", i, sBt->learnedHex(i),
           sBt->learnedMask(i));
    }
  } else if (strcmp(line, "dump") == 0) {
    cmdDump();
  } else if (strcmp(line, "selftest") == 0) {
    sBt->selfTestParse();
  } else if (strcmp(line, "back") == 0) {
    EASYUICONTEXT->closeActivity("remoteActivity");
  } else {
    LOGW("remoteLogic: 未知命令 '%s'", line);
  }
}

void pollCmd() {
  FILE *f = fopen("/tmp/pg_remotecmd", "r");
  if (!f) return;
  char all[512] = {0};
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    if (strlen(all) + (size_t)n + 2 < sizeof(all)) {
      strcat(all, line);
      strcat(all, "\n");
    }
  }
  fclose(f);
  if (!all[0] || strcmp(all, sCmdLast) == 0) return;  // 内容没变 → 不重复执行
  strncpy(sCmdLast, all, sizeof(sCmdLast) - 1);
  char *p = all;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    /* '#' 开头的行当注释跳过 —— 作用：让"内容变化才执行"的去重机制有个
     * 便宜的唯一后缀可加（每轮测试加一行 #xxx 即可重跑同一批命令）。 */
    if (*p && *p != '#') cmdRun(p);
    if (!nl) break;
    p = nl + 1;
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
    {TIMER_UI, UI_MS},    // 状态轮询 + 控件同步
    {TIMER_KEY, KEY_MS},  // 按键长按轮询（达标立刻返回，不等抬手）
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeyRouter);
#endif
  LOGD("remoteLogic: init");
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("remoteLogic: onUI_show");
  /* 独立 ftu 页必须**自己**管屏保：主界面不在前台时 mainLogic 的
   * tickScreensaverPolicy 定时器不跑，屏保会一直保持"开"，30 秒后整屏盖上来。
   * 见 src/logic/wifiLogic.cc §9.8（那边是同一条坑）。只在 show 关、quit 恢复，
   * 别在 onUI_hide 恢复。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  if (!sBt) sBt = pg::Bt::instance();
  if (sBt && !sStarted) {
    sStarted = true;
    sBt->start("/dev/ttyS2");
  }
  sPage = 0;
  if (mWinRemoteLearnPtr) mWinRemoteLearnPtr->hideWnd();
  // 强迫第一帧全量重写
  sLastState[0] = sLastName[0] = sLastPair[0] = sLastScan[0] = sLastTip[0] = 0;
  sLastSent[0] = sLastHost[0] = sLastPeer[0] = sLastDevHdr[0] = sLastLearnHdr[0] = 0;
  for (int i = 0; i < 9; ++i) { sLastKeyLabel[i][0] = 0; sLastKeyStyle[i] = -1; }
  sDevCount = -1;
  sLearnCount = -1;
#endif
}

static void onUI_hide() { LOGD("remoteLogic: onUI_hide"); }

static void onUI_quit() {
  LOGD("remoteLogic: onUI_quit");
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeyRouter);
#endif
  /* ⚠️ 不 stop() 蓝牙：pg::Bt 是单例且 rtk_init 只能成功一次（失败后要物理冷启动），
   *    退出本页只停界面，BT 线程继续跑。 */
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) {
  if (id == TIMER_KEY) {   // 长按达标即返回（不等抬手）
    tickKeyLongPress();
    return true;
  }
  if (id != TIMER_UI) return true;
  /* ★ 闹钟响铃期间**让位**：本页是全屏独立页，留着它就看不到主界面的"闹钟提醒页"
   *   （提醒页在 main.ftu，由主循环 tickAlarmRing() 弹出）⇒ 收掉自己回主界面。
   *   顺带补上老版本的空档：以前在独立页里响铃只有声音、没有界面，用户找不到"停止"。
   *   见 docs/page-split-plan.md §9.1。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("remoteLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    EASYUICONTEXT->closeActivity("remoteActivity");
    return false;
  }
#ifdef FUN_BUILD
  if (!sBt || !mActivityPtr) return true;
  pollCmd();   // QA 自检通道：/tmp/pg_remotecmd
  syncUi();    // 先比值再写
#endif
  return true;
}

static bool onremoteActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;  // 全部交给控件（原生按钮有自己的点击回调）
  return false;
}

/* ---------- 遥控页 ---------- */

static bool onButtonClick_BtnRemoteModeS(ZKButton *pButton) {
  (void)pButton;
  showPage(1);  // 切到学习页
  return true;
}

/* ---------- 学习页 ---------- */

static bool onButtonClick_BtnRemoteModeL(ZKButton *pButton) {
  (void)pButton;
  showPage(0);  // 切回遥控页
  return true;
}

static bool onButtonClick_BtnRemoteRescan(ZKButton *pButton) {
  (void)pButton;
  LOGD("remoteLogic: 手动重新扫描");
  if (sBt) sBt->hostScanStart();
  sDevCount = -1;
  return true;
}

static bool onButtonClick_BtnRemoteDisc(ZKButton *pButton) {
  (void)pButton;
  LOGD("remoteLogic: 断开对端");
  if (sBt) sBt->hostDisconnect();
  return true;
}

/* ---------- 周边设备列表 ---------- */

static int getListItemCount_ListRemoteDev(const ZKListView *pListView) {
  (void)pListView;
  return sBt ? sBt->scannedCount() : 0;
}

static void obtainListItemData_ListRemoteDev(ZKListView *pListView,
                                             ZKListView::ZKListItem *pListItem,
                                             int index) {
  (void)pListView;
  if (!pListItem || !sBt) return;
  /* ★ 2026-09-14 UI 改版：**这里不能再给行设底色** —— 行卡片的圆角底是 item 模板里
   * 的静态子控件 RowCard（九宫格 ios_card_row.9.png）。setBackgroundColor 画的是
   * 直角矩形，而且会把图的四角填成方的（"底色与图片互斥"，见 ui/main.html 的注释）。 */

  ZKListView::ZKListSubItem *addr = pListItem->findSubItemByID(ID_REMOTE_SubDevAddr);
  ZKListView::ZKListSubItem *name = pListItem->findSubItemByID(ID_REMOTE_SubDevName);
  ZKListView::ZKListSubItem *rssi = pListItem->findSubItemByID(ID_REMOTE_SubDevRssi);
  if (addr) addr->setText(sBt->scannedAddr(index));
  if (name) name->setText(sBt->scannedName(index));
  if (rssi) {
    char b[16];
    snprintf(b, sizeof(b), "%d", sBt->scannedRssi(index));
    rssi->setText(b);
  }
}

static void onListItemClick_ListRemoteDev(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  if (!sBt || index < 0 || index >= sBt->scannedCount()) return;
  LOGD("remoteLogic: 连接第 %d 个设备 %s", index, sBt->scannedAddr(index));
  sBt->hostConnect(index);
}

/* ---------- 已学到的报告列表 ---------- */

static int getListItemCount_ListRemoteLearned(const ZKListView *pListView) {
  (void)pListView;
  return sBt ? sBt->learnedCount() : 0;
}

static void obtainListItemData_ListRemoteLearned(ZKListView *pListView,
                                                 ZKListView::ZKListItem *pListItem,
                                                 int index) {
  (void)pListView;
  if (!pListItem || !sBt) return;
  /* ★ 2026-09-14 UI 改版：**这里不能再给行设底色** —— 行卡片的圆角底是 item 模板里
   * 的静态子控件 RowCard（九宫格 ios_card_row.9.png）。setBackgroundColor 画的是
   * 直角矩形，而且会把图的四角填成方的（"底色与图片互斥"，见 ui/main.html 的注释）。 */
  ZKListView::ZKListSubItem *t = pListItem->findSubItemByID(ID_REMOTE_SubLrnText);
  if (t) t->setText(sBt->learnedHex(index));
}

static void onListItemClick_ListRemoteLearned(ZKListView *pListView, int index,
                                              int id) {
  (void)pListView;
  (void)id;
  if (!sBt || index < 0 || index >= sBt->learnedCount()) return;
  /* 真·重放：这条报告在解析时已映射成"我们的"consumer 位图，点一下就用它发出去
   *（对端按键 → 我们的键，闭环）。 */
  uint16_t m = sBt->learnedMask(index);
  if (!m) {
    LOGW("remoteLogic: 第 %d 条没有可用的位图映射", index);
    return;
  }
  LOGD("remoteLogic: 重放学到的报告 #%d mask=0x%04x", index, m);
  sBt->sendKey(m);
  sSent++;
  sHitMask = m;
  sHitUntil = nowMs() + TIP_HOLD_MS;
}

/* ---------- 3x3 遥控按键 ---------- */
/* ⚠️ 回调必须**字面出现**在源码里（fun 按文本匹配；宏拼接出来的名字它认不出来，
 *    会再追加一份空骨架 → 编译报重定义）。所以这里 9 个都展开写死，不用宏。 */
static bool onButtonClick_BtnR0(ZKButton *pButton) { (void)pButton; fire(0); return true; }
static bool onButtonClick_BtnR1(ZKButton *pButton) { (void)pButton; fire(1); return true; }
static bool onButtonClick_BtnR2(ZKButton *pButton) { (void)pButton; fire(2); return true; }
static bool onButtonClick_BtnR3(ZKButton *pButton) { (void)pButton; fire(3); return true; }
static bool onButtonClick_BtnR4(ZKButton *pButton) { (void)pButton; fire(4); return true; }
static bool onButtonClick_BtnR5(ZKButton *pButton) { (void)pButton; fire(5); return true; }
static bool onButtonClick_BtnR6(ZKButton *pButton) { (void)pButton; fire(6); return true; }
static bool onButtonClick_BtnR7(ZKButton *pButton) { (void)pButton; fire(7); return true; }
static bool onButtonClick_BtnR8(ZKButton *pButton) { (void)pButton; fire(8); return true; }
