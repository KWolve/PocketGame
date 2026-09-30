#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * wifiLogic.cc - WiFi 独立应用（wifi.ftu）逻辑层
 *
 * 页面：WiFi 主页（开关行卡 + iOS 分组列表） + WinWifiPwd 密码弹窗。
 * ★ 2026-09-15 第二版（参考手机「设置 > 无线局域网」重排）：
 *   · 开关换成 **滑轨两态图**（ZKCheckBox 的 picTab{pic0,pic2}，图由
 *     tools/ios_theme.py 的 gen_switch() 生成）—— 逻辑这边不用管造型，
 *     只保留"用户点开关 → enableWifi"与"按真实状态回写勾选"这两件事；
 *     老版那句 setText("开"/"关") 已删（开关不该再有文字）。
 *   · 原来那张"当前连接信息卡"删了，信息压进开关卡副标题 + 右侧 IP；
 *     TextWifiSsid 控件已不存在（逻辑里不再有 mTextWifiSsidPtr）。
 *   · 列表改成 iOS 分组卡：四张行底图（RowCardTop/Mid/Bot/Solo）叠在同一位置，
 *     由 obtainListItemData_ListWifiAp 按 index 切 visible。
 *   · 行右侧去掉 dBm 数字，改成"档位词 + 加密锁图标"。
 * ⚠️ 原来的「信号探针」二级页（WinProbe）已于 2026-09-15 **提升为独立应用**
 *    （ui/probe.html -> probe.ftu -> probeActivity，逻辑在 probeLogic.cc）；
 *    本页底部的「信号探针」入口行改成 openActivity 跳过去。
 * 能力：zknet WifiManager（扫描/连接/监听器）。线程模型：
 *   - WifiManager 回调来自它的内部线程 → 回调里只记状态标志，UI 一律在 onUI_Timer 里刷；
 *   - 列表/文本遵循「先比值，只在变化时写」防重绘风暴（见工程记忆 14 条）。
 *
 * 按键：本板实体键 103=音量- 105=音量+ 108=暂停。wifi 页在前台时 mainActivity 已
 * onUI_hide，主 KeyRouter 让路（gActivityActive=false），这里注册自己的监听器：
 *   音量键 → pg::volumeStepGlobal（钩子转发回 mainLogic 的 Host 实例）+ 本 ftu 的
 *   WinVolume OSD；任意键长按 ≥700ms → 返回主界面（closeActivity）。
 *
 * ⚠️ CbWifiOn 是 ZKCheckBox，fun 生成器不为 checkbox 生成绑定 → 手动
 *   findControlByID（json 里 id=21001）+ setCheckedChangeListener。
 *   ⚠️ 这个 id 是**按控件出现顺序**分配的（第一个 checkbox = 21001），
 *      所以 CbWifiOn 必须继续是 ui/wifi.html 里唯一的/第一个 checkbox。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgWifiCfg.h"  // 切网前的"回家快照"（见该文件头：zknet 只留一个 network 条目）
#include "platform/PgAlarm.h"
#include "platform/PgAudio.h"
#include "core/PgCanvas.h"
#include "utils/Log.h"

#ifdef FUN_BUILD
#include "entry/EasyUIContext.h"
#include "control/ZKCheckBox.h"
#include "net/NetManager.h"
#include "net/WifiManager.h"
#include "net/WifiInfo.h"
#endif

class ZKButton;   // 前向声明（无条件，PC 侧编译也不会报错）

// 密码框的两个动作定义在文件后部的「UI 回调」区，但匿名 namespace 里的 QA 通道
// 要提前调用它们 —— 所以在这里（namespace 之外）先声明，避免"未声明"编译错。
static bool onButtonClick_BtnPwdOk(ZKButton *pButton);
static bool onButtonClick_BtnPwdCancel(ZKButton *pButton);

namespace {

/* ==================== 常量 ==================== */
const int TIMER_UI = 1;
const int UI_MS = 400;            // 状态轮询周期
const int KEY_MS = 100;           // 按键长按轮询周期（400ms 太粗，长按判定会飘到 1.1s）
const int TIMER_KEY = 2;          // 按键长按轮询用的定时器 id
const long SCAN_WAIT_MS = 12000;  // "正在扫描" 最长显示时长（超时自动收）
const long LONG_PRESS_MS = 700;   // 与主 KeyRouter 一致

// checkbox 的控件 id（ui/wifi.json 里 data-caption=CbWifiOn 分到的 id）
const int ID_WIFI_CbWifiOn = 21001;

// 配色（与 wifi.json 一致）
const uint32_t C_BG_ROW = 0xFF1C1C1E;
const uint32_t C_BG_ROW_PRESSED = 0xFF3A3A3C;
const uint32_t C_TEXT = 0xFFF2F2F7;
const uint32_t C_TEXT_DIM = 0xFF9A9AA0;
const uint32_t C_TEXT_BLUE = 0xFF64D2FF;
const uint32_t C_TEXT_GREEN = 0xFF30D158;
const uint32_t C_TEXT_GOLD = 0xFFFF9F0A;

/* ==================== 运行期状态 ==================== */
WifiManager *sWm = 0;
bool sListenerAdded = false;

std::vector<WifiInfo> sScan;      // 最近一次扫描结果（缓存副本）
volatile bool sScanDirty = true;  // 列表需要重建
volatile long sScanWaitUntil = 0; // >0 = 正在扫描（到点自动收）
volatile int sConnEvent = -1;     // E_WIFI_CONNECT 事件（-1 无）
volatile bool sPwdError = false;  // 密码错误回调

char sSelSsid[64] = {0};   // 密码弹窗对应的网络
char sSelSec[32] = {0};
bool sSelOpen = false;

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool sCbSync = false;       // 程序性 setChecked 时的防回环标志
long sCbHoldUntil = 0;      // 用户刚点过开关：暂停"按真实状态回写勾选"一小会儿
ZKCheckBox *sCbOn = 0;      // checkbox 无生成绑定，onUI_show 时解析

// 开关点击 → 真正开关 WiFi（生成器不给 checkbox 生成绑定，手动挂监听）
class CbChanged : public ZKCheckBox::ICheckedChangeListener {
 public:
  void onCheckedChanged(ZKCheckBox *p, bool on) override {
    if (sCbSync) return;  // 程序性回写，忽略
    (void)p;
    LOGD("wifiLogic: 用户开关 WiFi -> %d", on ? 1 : 0);
    if (sWm) sWm->enableWifi(on);
    if (on) {
      sScanWaitUntil = nowMs() + SCAN_WAIT_MS + 4000;  // 起卡+扫描要几秒
    } else {
      sScanWaitUntil = 0;
      sScan.clear();
      sScanDirty = true;
    }
    sCbHoldUntil = nowMs() + 8000;  // 8s 内不让轮询把勾选态改回去
  }
};
CbChanged sCbChanged;

// 上次写入控件的文本缓存（先比值再写）
char sLastState[64] = {0};
char sLastIp[32] = {0};
uint32_t sLastStateColor = 0;   // 副标题颜色缓存（同"先比值再写"，避免每帧 setTextColor）

/* ==================== 工具 ==================== */

// freq(MHz) -> 信道号；2.4G 1~14，5G (f-5000)/5；未知返回 0
int chanOfFreq(int f) {
  if (f >= 5000) return (f - 5000) / 5;
  if (f == 2484) return 14;
  if (f >= 2412 && f <= 2472) return (f - 2412) / 5 + 1;
  return 0;
}

// 加密方式归一化：去括号，取 WPA/WPA2/WEP/开放
void secText(const std::string &enc, char *buf, int n) {
  if (!buf || n <= 0) return;
  const char *e = enc.c_str();
  if (!e[0] || strcmp(e, "[ESS]") == 0 || strcmp(e, "NONE") == 0) {
    snprintf(buf, (size_t)n, "开放");
    return;
  }
  if (strstr(e, "WPA2") || strstr(e, "RSN")) {
    snprintf(buf, (size_t)n, "WPA2");
  } else if (strstr(e, "WPA")) {
    snprintf(buf, (size_t)n, "WPA");
  } else if (strstr(e, "WEP")) {
    snprintf(buf, (size_t)n, "WEP");
  } else {
    // 未知格式：截前 12 个字符兜底
    snprintf(buf, (size_t)n, "%.12s", e);
  }
}

// rssi -> 档位词（**只剩 QA / 日志用**：列表行右侧 2026-09-15 起改成三段弧线图标，
// 不再在界面上显示文字 —— 用户反馈"信号强度图标太抽象"，见 obtainListItemData_ListWifiAp）
const char *rssiWord(int rssi) {
  if (rssi >= -55) return "满格";
  if (rssi >= -65) return "强";
  if (rssi >= -75) return "中";
  if (rssi >= -85) return "弱";
  return "很差";
}

bool scanByIdRssiDesc(const WifiInfo &a, const WifiInfo &b) {
  return a.getRssi() > b.getRssi();
}

// 是否当前已连接的 ssid
bool isConnectedSsid(const WifiInfo &info) {
  if (!sWm || !sWm->isConnected()) return false;
  WifiInfo *cur = sWm->getConnectionInfo();
  if (!cur) return false;
  const std::string &a = info.getSsid();
  const std::string &b = cur->getSsid();
  return !a.empty() && a == b;
}

/* ==================== WifiManager 监听（线程侧只设标志） ==================== */
class WifiEvents : public WifiManager::IWifiListener {
 public:
  void handleWifiScanResult(std::vector<WifiInfo> *infos) override {
    if (!infos) return;
    sScan = *infos;
    std::sort(sScan.begin(), sScan.end(), scanByIdRssiDesc);
    sScanDirty = true;
    sScanWaitUntil = 0;
  }
  void handleWifiConnect(E_WIFI_CONNECT event, int args) override {
    (void)args;
    sConnEvent = (int)event;
  }
  void handleWifiErrorCode(E_WIFI_ERROR_CODE code) override {
    if (code == E_WIFI_ERROR_CODE_PASSWORD_INCORRECT) sPwdError = true;
  }
};
WifiEvents sEvents;

/* ==================== 自定义输入法（IME）对接 ====================
 * 输入法不在本文件 —— 它是独立的 SysApp：`src/logic/imeApp.cc`
 * （REGISTER_SYSAPP(APP_TYPE_SYS_IME) + 布局 ui/ime.html -> ui/ime.ftu）。
 *
 * 为什么这样做（2026-09-13 返工）：
 *   第一版把键盘**自绘**在画布上（挂 CvPwdKb），功能可用但**违反"非游戏禁止自定义绘图"
 *   铁律**，而且画布点阵字的观感与其余原生控件页不一致。官方正解（见知识库
 *   devflow/activity-code-skeleton.md §6 + 范本 ImeDemo）是做一个 IME SysApp：
 *   系统在**任何 ZKEditText 聚焦时自动拉起**，业务页一行代码都不用写。
 *   ⇒ 密码框恢复成原生 `ZKEditText`（EditPwd），键盘交给 imeApp.cc。
 *
 * 数据流：点密码框 → 框架 showIME() → 我们的 IME 收字
 *   → 「完成」doneIMETextUpdate(整串) → 框架写回 EditText 并触发本文件的
 *     onEditTextChanged_EditPwd() → 这里直接发起连接（等价于点「连接」）。
 */
class PwdImeListener : public IMEContext::IIMETextUpdateListener {
 public:
  void onIMETextUpdate(const std::string &text) override;
};
PwdImeListener sPwdImeListener;

/* 程序化写 EditText 时用它挡掉 onEditTextChanged_EditPwd 的"自动连接"
 * （打开弹窗要清空输入框，那不是用户提交） */
bool sPwdSilentSet = false;

/* QA 用：showIME 需要 info 活到 IME 关闭（框架只存指针），所以必须是静态的 */
IMEContext::SIMETextInfo sQaImeInfo;

void PwdImeListener::onIMETextUpdate(const std::string &text) {
  LOGD("wifiLogic: IME 交回 %d 字符", (int)text.size());
  if (!mEditPwdPtr) return;
  sPwdSilentSet = false;   // 这是"用户提交"，允许触发连接
  mEditPwdPtr->setText(text);
}

/* ★ 音量 OSD **已收归全局状态栏**（2026-09-14）：本 ftu 不再自带 WinVolume。
 *  原来 WiFi 页有一份自己的面板 —— 结果工具页/套件/IPTV 上按音量键只有声音没界面。
 *  现在：按音量键只调音量（走 pg::volumeStepGlobal），状态栏负责弹面板（见 statusbarLogic.cc）。 */

/* ==================== 物理按键（wifi 页前台） ==================== */
class WifiKeys : public EasyUIContext::IKeyListener {
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
      return true;  // 按下先吞掉
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_UP) {
      if (ke.mKeyCode != sDownCode) return false;
      int held = (int)(nowMs() - sDownMs);
      sDownCode = -1;
      if (held >= LONG_PRESS_MS && !sLPFired) {
        // 长按 = 返回主界面
        LOGD("wifiLogic: 长按 %dms -> 返回主界面", held);
        EASYUICONTEXT->closeActivity("wifiActivity");
        return true;
      }
      if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
        int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
        if (pct >= 0)
          LOGD("wifiLogic: 音量%s -> %d%%（OSD 由全局状态栏显示）",
               ke.mKeyCode == 105 ? "+" : "-", pct);
      }
      return true;  // 其余按键吞掉，避免落到框架默认行为
    }
    return false;
  }

  /* ★ 长按达标**立刻**返回，不等按键抬起。
 *   本板 gpio-keys **没有 autorepeat** ⇒ 长按期间内核一个事件都不发；只在 E_KEY_UP 里
 *   判长按的话，用户按满 700ms 还得一直按到松手才切页 —— 手感就是"按了不动、松手才跳"。
 *   所以由页面定时器轮询"按下且已超时"。（定时器周期要够细：页面自身的状态轮询
 *   300~400ms 太粗，判定会飘到 1.1s，所以这几页另开 100ms 的按键定时器。） */
  static bool longPressReady() {
    if (sDownCode < 0 || sLPFired) return false;      // 有键按着（不分键码：
                                                      // 与本页原来的语义一致 ——
                                                      // 任何键按住 >=700ms 都返回）
    if (nowMs() - sDownMs < LONG_PRESS_MS) return false;
    sLPFired = true;   // 只触发一次；抬起时 UP 分支也不会再判
    return true;
  }

 private:
  static int sDownCode;
  static long sDownMs;
  static bool sLPFired;
};
int WifiKeys::sDownCode = -1;
long WifiKeys::sDownMs = 0;
bool WifiKeys::sLPFired = false;

/* 定时器每 100ms 调一次：达标就立刻返回主界面（不等抬手）。 */
void tickKeyLongPress() {
  if (WifiKeys::longPressReady()) {
    LOGD("wifiLogic: 长按达标（不等抬起）-> 返回主界面");
    EASYUICONTEXT->closeActivity("wifiActivity");
  }
}
WifiKeys sKeyRouter;

/* ==================== QA 自检通道（免触摸验收） ====================
 * 本板 mt_test 触摸注入时灵时不灵（见工程记忆），原生控件又走不了 dispatchCanvasTouch，
 * 所以这里再开一条文件驱动的命令通道：整份内容变化时逐行执行。
 *   on / off        — 开/关 WiFi
 *   scan            — 触发一次扫描
 *   dump            — 把当前扫描结果（序号/SSID/频率/信道/加密/信号）打到日志
 *   pick <idx>      — 等价于点列表第 idx 行（开放网络直连，加密网络弹密码框）
 *   conn <ssid> <pw>— 按 SSID 直连（确定性；SSID 原样，含空格请用 pick）
 *   pw <文字>       — 填入密码（含空格；用 _ 代表空格）
 *   ok / cancel     — 密码框的 连接/取消
 *   probe           — 跳到独立的信号探针应用（probeActivity）
 *   snap / snapinfo — 把当前网络存成"回家快照" / 打印快照状态
 *   restore         — 把快照里的原网络还原回去（配置 + 重读 + 重连）
 *   wpa <命令>      — 直连 wpa_supplicant 控制口发一条命令（诊断用）
 *   state           — 打印当前状态（开关/连接/SSID/IP/列表条数）
 * ⚠️ 每行内容必须不同（尾部加序号），相同内容会被去重不执行。
 */
char sCmdLast[512] = {0};

/* ==================== 切网前的"回家快照" ====================
 * 为什么必须有（血案见 docs/wifi-app.md 末尾）：zknet 的配置里**只保留一个 network 条目**
 * （`update_config=1` + `SET_NETWORK 0`）⇒ 只要连一次别的网络，**原来那张网的 SSID+密码就被
 * 顶掉了**，用户得重新输密码。
 * 所以任何"发起连接"的入口都先存一份：只要用户在探针里进了「目标网模式」，
 * 之后就能一键切回来（配置整份还原 + 自动重连）。
 */
void keepHomeBeforeSwitch(const char *newSsid) {
  if (!sWm || !sWm->isConnected()) return;
  pg::WifiCfg *wc = pg::WifiCfg::instance();
  if (wc->targetMode()) return;   // 目标网模式里：**不许**把目标网当成"家"
  if (!wc->hasHome() || newSsid == 0 || strcmp(wc->homeSsid(), newSsid) != 0) {
    WifiInfo *cur = sWm->getConnectionInfo();
    if (!cur) return;
    std::string cs = cur->getSsid();
    if (cs.empty()) return;
    if (newSsid && cs == newSsid) return;   // 连的就是当前这张，不用存
    wc->keepHome(cs.c_str());
  }
}

void wifiCmdDump() {
  LOGD("wifiLogic: dump 共 %d 个 AP", (int)sScan.size());
  for (size_t i = 0; i < sScan.size(); ++i) {
    char sec[24];
    secText(sScan[i].getEncryption(), sec, sizeof(sec));
    LOGD("wifiLogic: AP#%d ssid='%s' freq=%d ch=%d sec=%s rssi=%d", (int)i,
         sScan[i].getSsid().c_str(), sScan[i].getFreq(),
         chanOfFreq(sScan[i].getFreq()), sec, sScan[i].getRssi());
  }
}

void wifiCmdState() {
  if (!sWm) {
    LOGD("wifiLogic: state sWm=NULL");
    return;
  }
  const char *ip = sWm->getIp();
  WifiInfo *cur = sWm->getConnectionInfo();
  LOGD("wifiLogic: state enable=%d connected=%d ssid='%s' ip='%s' ap=%d",
       sWm->isWifiEnable() ? 1 : 0, sWm->isConnected() ? 1 : 0,
       cur ? cur->getSsid().c_str() : "", ip ? ip : "", (int)sScan.size());
}

volatile bool sPwdDlgOpen = false;   // 密码弹窗是否在显示（触摸白名单要用）

/* 显式拉起我们的 IME（键盘）。
 *
 * 为什么不让框架"点 ZKEditText 自动拉起"就完事：实测**不稳定**——
 * 同一个密码框，有时点一下就弹键盘，有时点两三下都没反应（真机 + pginj 注入都复现过）。
 * 而 QA 通道 `imeshow` 走同一条 showIME() 调用，**每次必成功**。
 * ⇒ 结论：控件聚焦那条路有竞态，改成"点密码框时我们自己调 showIME"，
 *   IME 面板照样覆盖 y>=424 的下半屏，行为与自动拉起完全一致（甚至更确定）。
 *
 * ⚠️ sQaImeInfo 必须静态：框架只存指针，要活到 IME 关闭。 */
void showPwdIme() {
  sQaImeInfo.isPassword = true;
  sQaImeInfo.passwordChar = '*';
  sQaImeInfo.imeTextType = IMEContext::E_IME_TEXT_TYPE_ALL;
  sQaImeInfo.text = mEditPwdPtr ? mEditPwdPtr->getText() : std::string();
  LOGD("wifiLogic: 拉起键盘（弹窗=%d，输入框已有 %d 字符）", sPwdDlgOpen ? 1 : 0,
       (int)sQaImeInfo.text.size());
  EASYUICONTEXT->showIME(&sQaImeInfo, &sPwdImeListener);
}

/* 打开密码弹窗：清空输入框再 showWnd。
 * ⚠️ 清空是"程序化写 EditText"，会触发 onEditTextChanged_EditPwd —— 那不是用户提交，
 *    所以用 sPwdSilentSet 挡一下（setText 是同步回调，设完立刻复位）。 */
void openPwdDialog() {
  if (mTextPwdSsidPtr) mTextPwdSsidPtr->setText(sSelSsid);
  if (mTextPwdMsgPtr) mTextPwdMsgPtr->setText("");
  if (mEditPwdPtr) {
    sPwdSilentSet = true;
    mEditPwdPtr->setText("");
    sPwdSilentSet = false;
  }
  if (mWinWifiPwdPtr) mWinWifiPwdPtr->showWnd();
  sPwdDlgOpen = true;
  LOGD("wifiLogic: 打开密码弹窗 '%s'（键盘交给 imeApp）", sSelSsid);
}

// 等价于点列表第 idx 行（复用 onListItemClick_ListWifiAp 的逻辑）
void wifiCmdPick(int idx) {
  if (idx < 0 || idx >= (int)sScan.size() || !sWm) {
    LOGD("wifiLogic: pick %d 越界（共 %d 个）", idx, (int)sScan.size());
    return;
  }
  const WifiInfo &ap = sScan[idx];
  snprintf(sSelSsid, sizeof(sSelSsid), "%s", ap.getSsid().c_str());
  secText(ap.getEncryption(), sSelSec, sizeof(sSelSec));
  sSelOpen = (strcmp(sSelSec, "开放") == 0);
  LOGD("wifiLogic: pick %d -> '%s' (%s) open=%d", idx, sSelSsid, sSelSec,
       sSelOpen ? 1 : 0);
  if (sSelOpen) {
    keepHomeBeforeSwitch(sSelSsid);
    sWm->connect(sSelSsid, "");
  } else {
    openPwdDialog();
  }
}

void wifiCmdRun(const char *cmd) {
  if (!cmd || !cmd[0]) return;
  if (strncmp(cmd, "on", 2) == 0 && sWm) {
    LOGD("wifiLogic: cmd on");
    sWm->enableWifi(true);
    sScanWaitUntil = nowMs() + SCAN_WAIT_MS + 4000;
    sCbHoldUntil = nowMs() + 8000;
  } else if (strncmp(cmd, "off", 3) == 0 && sWm) {
    LOGD("wifiLogic: cmd off");
    sWm->enableWifi(false);
    sScanWaitUntil = 0;
    sScan.clear();
    sScanDirty = true;
    sCbHoldUntil = nowMs() + 8000;
  } else if (strncmp(cmd, "scan", 4) == 0 && sWm) {
    LOGD("wifiLogic: cmd scan");
    sScanWaitUntil = nowMs() + SCAN_WAIT_MS;
    sWm->scan();
  } else if (strncmp(cmd, "dump", 4) == 0) {
    wifiCmdDump();
  } else if (strncmp(cmd, "state", 5) == 0) {
    wifiCmdState();
  } else if (strncmp(cmd, "pickopen", 8) == 0) {
    // 列表随扫描在变、序号会失效 → 直接按"第一个开放网络"做确定性自检
    for (size_t i = 0; i < sScan.size(); ++i) {
      char sec[24];
      secText(sScan[i].getEncryption(), sec, sizeof(sec));
      if (strcmp(sec, "开放") == 0) {
        LOGD("wifiLogic: pickopen -> #%d '%s'", (int)i, sScan[i].getSsid().c_str());
        wifiCmdPick((int)i);
        return;
      }
    }
    LOGD("wifiLogic: pickopen 没找到开放网络");
  } else if (strncmp(cmd, "conn ", 5) == 0) {
    /* conn <ssid> [密码] —— 按 SSID 直连。
     * 为什么要它：`pick <idx>` 走列表序号，而扫描结果随时会把列表刷掉（序号漂移），
     * 自检脚本没法稳定复现（踩过：pick 10 打到了别的 AP）。按名字连就天然确定。
     * SSID 原样传（**不做 `_`→空格替换**：真实 SSID 里就有下划线，如 TPLink_zkswe，
     * 替换会把名字改掉导致连不上 —— 踩过）。含空格的 SSID 请改用 pick <idx>。 */
    if (!sWm) {
      LOGD("wifiLogic: conn 失败 sWm=NULL");
      return;
    }
    std::string rest = cmd + 5;
    size_t sp = rest.find(' ');
    std::string ssid = (sp == std::string::npos) ? rest : rest.substr(0, sp);
    std::string pw = (sp == std::string::npos) ? std::string() : rest.substr(sp + 1);
    LOGD("wifiLogic: cmd conn ssid='%s' pw=%s", ssid.c_str(), pw.empty() ? "(空)" : "(有)");
    keepHomeBeforeSwitch(ssid.c_str());
    sWm->connect(ssid, pw);
  } else if (strncmp(cmd, "pick ", 5) == 0) {
    wifiCmdPick(atoi(cmd + 5));
  } else if (strncmp(cmd, "who", 3) == 0) {
    /* 诊断"触摸被谁吃了"：键盘是否在显示 / 弹窗是否在显示 / 屏保开关状态 */
    LOGD("wifiLogic: who ime=%d dlg=%d wnd=%d saverEnable=%d saverOn=%d",
         EASYUICONTEXT->isIMEShow() ? 1 : 0, sPwdDlgOpen ? 1 : 0,
         (mWinWifiPwdPtr && mWinWifiPwdPtr->isWndShow()) ? 1 : 0,
         EASYUICONTEXT->isScreensaverEnable() ? 1 : 0,
         EASYUICONTEXT->isScreensaverOn() ? 1 : 0);
  } else if (strncmp(cmd, "imeshow", 7) == 0) {
    /* 程序化拉起 IME（等价于用户点密码框）—— 本板注入不了真实触摸，
     * 这是脚本化验收 IME 的唯一入口。 */
    showPwdIme();
  } else if (strncmp(cmd, "imehide", 7) == 0) {
    LOGD("wifiLogic: cmd imehide");
    EASYUICONTEXT->hideIME();
  } else if (strncmp(cmd, "ok", 2) == 0) {
    LOGD("wifiLogic: cmd ok");
    onButtonClick_BtnPwdOk(0);
  } else if (strncmp(cmd, "cancel", 6) == 0) {
    LOGD("wifiLogic: cmd cancel");
    onButtonClick_BtnPwdCancel(0);
  } else if (strncmp(cmd, "vol ", 4) == 0) {
    // 走的就是按键那条路：跨 activity 钩子 -> mainLogic 的 Host 音频 -> 本 ftu 的 OSD
    // 第三个参数可指定 OSD 停留毫秒（抓帧验收用，正常是 1600ms）
    int d = atoi(cmd + 4);
    int hold = 0;
    const char *sp = strchr(cmd + 4, ' ');
    if (sp) hold = atoi(sp + 1);
    int pct = pg::volumeStepGlobal(d);
    /* hold 参数已无用（面板归状态栏）；抓帧请用状态栏通道 /tmp/pg_statusbarcmd */
    LOGD("wifiLogic: cmd vol %d -> %d%% (hold=%d 已忽略)", d, pct, hold);
  } else if (strncmp(cmd, "snapinfo", 8) == 0) {
    pg::WifiCfg *wc = pg::WifiCfg::instance();
    WifiInfo *cur = (sWm && sWm->isConnected()) ? sWm->getConnectionInfo() : 0;
    LOGD("wifiLogic: snapinfo home='%s' hasHome=%d target=%d pskLen=%d cur='%s' ctrl=%s",
         wc->homeSsid(), wc->hasHome() ? 1 : 0, wc->targetMode() ? 1 : 0, wc->homePskLen(),
         cur ? cur->getSsid().c_str() : "", wc->ctrlPath());
  } else if (strncmp(cmd, "restore", 7) == 0) {
    /* 在 WiFi 应用里也能"回家"：万一用户在别处把网切掉了，不必开探针应用 */
    pg::WifiCfg *wc = pg::WifiCfg::instance();
    char ssid[64] = {0}, psk[96] = {0};
    bool ok = wc->leaveTargetMode(ssid, sizeof(ssid), psk, sizeof(psk));
    LOGD("wifiLogic: restore -> %d 原网络='%s'", ok ? 1 : 0, ssid);
    if (ok && ssid[0] && sWm) {
      char resp[256] = {0};
      int r = wc->wpaCmd("STATUS", resp, sizeof(resp));
      LOGD("wifiLogic: restore 后 STATUS=%d", r);
      /* 控制口重连不保证成功 ⇒ 这里直接 zknet 直连一次（有密码就不用用户再输） */
      if (!sWm->isConnected()) sWm->connect(ssid, psk);
    }
  } else if (strncmp(cmd, "snap", 4) == 0) {
    pg::WifiCfg *wc = pg::WifiCfg::instance();
    WifiInfo *cur = (sWm && sWm->isConnected()) ? sWm->getConnectionInfo() : 0;
    bool ok = wc->keepHome(cur ? cur->getSsid().c_str() : 0);
    LOGD("wifiLogic: snap -> %d 原网络='%s'", ok ? 1 : 0, wc->homeSsid());
  } else if (strncmp(cmd, "wpa ", 4) == 0) {
    char resp[600];
    int r = pg::WifiCfg::instance()->wpaCmd(cmd + 4, resp, sizeof(resp));
    LOGD("wifiLogic: wpa '%s' -> %d / '%s'", cmd + 4, r, resp);
  } else if (strncmp(cmd, "probe", 5) == 0) {
    LOGD("wifiLogic: cmd probe -> 跳到独立的信号探针应用");
    onButtonClick_BtnProbe(0);
  } else {
    LOGD("wifiLogic: cmd 未识别 '%s'", cmd);
  }
}

void pollWifiCmd() {
  FILE *f = fopen("/tmp/pg_wificmd", "r");
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
  LOGD("wifiLogic: ---- 执行 QA 命令块 ----");
  char *p = all;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (*p) wifiCmdRun(p);
    if (!nl) break;
    p = nl + 1;
  }
}

/* ==================== UI 同步（先比值再写） ==================== */
void setTextIfChanged(ZKTextView *tv, const char *text, char *cache, int cn) {
  if (!tv || !text) return;
  if (strncmp(cache, text, (size_t)cn) == 0) return;
  tv->setText(text);
  snprintf(cache, (size_t)cn, "%s", text);
}

void syncStatusCard() {
  if (!sWm) return;
  bool en = sWm->isWifiEnable();
  bool conn = sWm->isConnected();
  char b[96];

  /* ★ 2026-09-15 改版：原来那张"当前连接信息卡"（大字 SSID + IP + dBm）已删，
   *   信息压进「无线局域网」开关卡的副标题 + 右侧 IP —— 与手机设置页一致
   *   （手机设置里当前网络就是列表里带勾的那行，没有单独的详情卡）。 */
  if (conn) {
    WifiInfo *info = sWm->getConnectionInfo();
    const char *ssid = (info && !info->getSsid().empty()) ? info->getSsid().c_str() : "";
    snprintf(b, sizeof(b), "已连接 %s", ssid);
    setTextIfChanged(mTextWifiStatePtr, b, sLastState, sizeof(sLastState));
    const char *ip = sWm->getIp();
    snprintf(b, sizeof(b), "%s", (ip && ip[0]) ? ip : "获取IP中");
    setTextIfChanged(mTextWifiIpPtr, b, sLastIp, sizeof(sLastIp));
  } else if (en) {
    snprintf(b, sizeof(b), "%s",
             sScanWaitUntil ? "正在扫描附近网络" : "未连接");
    setTextIfChanged(mTextWifiStatePtr, b, sLastState, sizeof(sLastState));
    setTextIfChanged(mTextWifiIpPtr, "", sLastIp, sizeof(sLastIp));
  } else {
    snprintf(b, sizeof(b), "已关闭");
    setTextIfChanged(mTextWifiStatePtr, b, sLastState, sizeof(sLastState));
    setTextIfChanged(mTextWifiIpPtr, "", sLastIp, sizeof(sLastIp));
  }
  // 副标题颜色：已连接绿、其余灰（先比值再写，避免每帧 setTextColor 触发重绘）
  if (mTextWifiStatePtr) {
    uint32_t want = conn ? C_TEXT_GREEN : C_TEXT_DIM;
    if (want != sLastStateColor) {
      mTextWifiStatePtr->setTextColor(want);
      sLastStateColor = want;
    }
  }

  // 开关勾选态（防回环；用户刚点过开关时先别回写）
  if (sCbOn && !sCbSync && nowMs() > sCbHoldUntil) {
    if (sCbOn->isChecked() != en) {
      sCbSync = true;
      sCbOn->setChecked(en);
      sCbSync = false;
    }
  }
  /* ★ 开关上的"开/关"文字已删除（2026-09-15）：新版开关是**滑轨两态图**
   *   （ui/wifi.html 的 data-pic / data-pic2，图由 ios_theme.gen_switch 生成），
   *   开关状态由滑块位置表达，再写字就是画蛇添足 —— 也正好避免"字压在图上"。 */

  if (sScanWaitUntil && nowMs() > sScanWaitUntil) sScanWaitUntil = 0;
}


}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

/**
 * 注册定时器
 */
static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_UI, UI_MS},    // 状态轮询
    {TIMER_KEY, KEY_MS},  // 按键长按轮询（达标立刻返回，不等抬手）
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeyRouter);
#endif
  LOGD("wifiLogic: init");
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("wifiLogic: onUI_show");
  /* 工作界面禁屏保（官方推荐用法，见 EasyUIContext.h:159）。
   * ⚠️ 本页必须关：屏保是整屏 SysApp，30 秒无操作就会盖上来 ——
   *    实测（本机）它盖住 WiFi 页后，**连键盘一起盖掉**，用户第一下触摸只能"唤醒"，
   *    表现就是"点了没反应"。而本页恰恰是"交互慢"的页（扫网 36 个 AP、看列表、
   *    输密码），几乎必然踩到。
   * ⚠️ 只在 onUI_show 关、onUI_quit 恢复，**不要在 onUI_hide 恢复**：
   *    键盘（IME SysApp）弹出时本页会 hide，一恢复屏保就又回来了。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  if (!sWm) {
    NetManager *nm = NETMANAGER;
    sWm = nm ? nm->getWifiManager() : 0;
  }
  if (sWm && !sListenerAdded) {
    sWm->addWifiListener(&sEvents);
    sListenerAdded = true;
  }
  if (!sCbOn && mActivityPtr) {
    sCbOn = (ZKCheckBox *)mActivityPtr->findControlByID(ID_WIFI_CbWifiOn);
    if (sCbOn) sCbOn->setCheckedChangeListener(&sCbChanged);
  }
  if (sWm) {
    bool en = sWm->isWifiEnable();
    if (sCbOn) {
      sCbSync = true;
      sCbOn->setChecked(en);
      sCbSync = false;
    }
    if (en) {
      // 进页先扫一次
      sScanWaitUntil = nowMs() + SCAN_WAIT_MS;
      sWm->scan();
    }
  }
  sScanDirty = true;
  sConnEvent = -1;
  sLastState[0] = 0;      // 清文本缓存：进页强制重写副标题/IP
  sLastIp[0] = 0;
  sLastStateColor = 0;    // 颜色缓存同上
#endif
}

static void onUI_hide() { LOGD("wifiLogic: onUI_hide"); }

static void onUI_quit() {
  LOGD("wifiLogic: onUI_quit");
  /* 恢复屏保（回主界面后 mainLogic 的 tickScreensaverPolicy 也会按模式收敛，
   * 这里显式恢复只是不留"离页到收敛"之间的空档）。 */
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeyRouter);
  if (sWm && sListenerAdded) {
    sWm->removeWifiListener(&sEvents);
    sListenerAdded = false;
  }
#endif
  sWm = 0;
  sCbOn = 0;
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
    LOGD("wifiLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    EASYUICONTEXT->closeActivity("wifiActivity");
    return false;
  }
#ifdef FUN_BUILD
  if (!sWm || !mActivityPtr) return true;

  // WiFi 连接事件（来自 wifi 线程）
  if (sConnEvent >= 0) {
    int ev = sConnEvent;
    sConnEvent = -1;
    LOGD("wifiLogic: 连接事件 %d", ev);
    sScanDirty = true;
  }
  // 密码错误
  if (sPwdError) {
    sPwdError = false;
    if (mWinWifiPwdPtr && mTextPwdMsgPtr) mTextPwdMsgPtr->setText("密码错误，请重试");
  }

  pollWifiCmd();  // QA 自检通道：/tmp/pg_wificmd
  syncStatusCard();

  if (sScanDirty) {
    sScanDirty = false;
    if (mListWifiApPtr) mListWifiApPtr->refreshListView();
  }
#endif
  return true;
}

static bool onwifiActivityTouchEvent(const MotionEvent &ev) {
  /* ★★ 键盘（IME）显示期间**一个触摸都不能吞** —— 这就是"wifi 键盘敲不进字"的真凶。
   *
   * 原来的白名单只放行输入框 + 两个按钮（y ≤ 394），而**键盘面板铺在 y=424..800**：
   * 点键位落进"弹窗空白处"分支被 `return true` 吞掉，键位永远收不到 DOWN。
   * 真机判据（决定性）：**同一个 IME 实例**，长按实体键离开本页后立刻能输入
   * （`IME 键 '1' -> 1 字符`），一回到本页又不行 ⇒ 不是键盘坏，是本页把触摸吃了。
   * ⚠️ IME 是独立 SysApp、整屏在最上层，它的触摸不经过本 Activity 的窗口树；
   *    本 Activity 的触摸回调只要"吞"了，键盘就永远收不到。 */
  if (sPwdDlgOpen)
    LOGD("wifiLogic: touch action=%d (%d,%d) ime=%d", (int)ev.mActionStatus, ev.mX, ev.mY,
         EASYUICONTEXT->isIMEShow() ? 1 : 0);

  /* ⚠️ 这里**不许再按坐标吞触摸**了（原来那段白名单已删除，血案见函数头注释）：
   *    键盘（IME SysApp）整屏在最上层，它的触摸经过本 Activity 的触摸回调；
   *    只要这里 `return true`，按键就永远收不到 DOWN。
   *
   * 原来那段白名单的**唯一目的**是"别让点弹窗空白处穿透到下层列表、误连别的网络"。
   * 那个风险改在**下层**挡（`onListItemClick_ListWifiAp` 开头判 sPwdDlgOpen）——
   * 在"真正做事的地方"挡，比在坐标上猜要可靠得多，也不会再伤到键盘。
   *
   * 只剩一件事要在这里做：点**密码输入框**时抬手**显式拉起键盘**（不依赖控件聚焦，
   * 框架的自动拉起是间歇性的，见 showPwdIme 的说明）。 */
  if (sPwdDlgOpen) {
    /* ★★ 为什么"取消 / 连接"要在这里**按坐标手动处理**（2026-09-15 真机实测，老缺陷）：
     *   弹窗窗口 WinWifiPwd 是 `touchable=false`，而这条**不能改** —— 整屏窗口一旦可触摸，
     *   连键盘区域（y≥424）的触摸也会被本 Activity 吃掉，IME 就再也收不到按键
     *   （"wifi 键盘敲不进字"的老血案，见本函数开头那段说明）。
     *   代价是：**窗口不可触摸 ⇒ 窗口里的按钮收不到点击**。实测（pginj 注入）：
     *     点「取消」(145,364) / 「连接」(335,364) → 本回调**收到了** action=1/2，坐标也对，
     *     但 `onButtonClick_BtnPwdCancel/Ok` **一次都没被触发**；
     *     而点「输入框」有反应 —— 因为那是这里手动调的 showPwdIme()。
     *   ⇒ 所以这三块可交互区全部改成"手动判坐标 + 直接调回调"。
     *
     * ⚠️⚠️ **只处理 y < 424**：y≥424 是键盘的地盘，必须原样放行（`return false`），
     *    这是当年血案的根因（老白名单没有下限，把键盘区域的触摸也吞了）。
     * ⚠️ 下面这些数字必须与 ui/wifi.html 的控件盒**逐个对齐**，改布局就得回来改。 */
    static const int kKbTopY = 424;                                   // 键盘面板顶边
    static const int kEditX = 60, kEditY = 238, kEditW = 360, kEditH = 60;
    static const int kCardX = 40, kCardY = 150, kCardW = 400, kCardH = 260;
    static const int kBtnY = 336, kBtnH = 56, kBtnW = 170;
    static const int kBtnCancelX = 60, kBtnOkX = 250;

    if (ev.mY >= kKbTopY) return false;   // ← 键盘区域：整块放行，别吞

    if (ev.mActionStatus == MotionEvent::E_ACTION_UP) {
      if (ev.mX >= kEditX && ev.mX < kEditX + kEditW &&
          ev.mY >= kEditY && ev.mY < kEditY + kEditH) {
        showPwdIme();
        return true;
      }
      if (ev.mX >= kBtnCancelX && ev.mX < kBtnCancelX + kBtnW &&
          ev.mY >= kBtnY && ev.mY < kBtnY + kBtnH) {
        LOGD("wifiLogic: 弹窗内点「取消」（手动分发）");
        onButtonClick_BtnPwdCancel(0);
        return true;
      }
      if (ev.mX >= kBtnOkX && ev.mX < kBtnOkX + kBtnW &&
          ev.mY >= kBtnY && ev.mY < kBtnY + kBtnH) {
        LOGD("wifiLogic: 弹窗内点「连接」（手动分发）");
        onButtonClick_BtnPwdOk(0);
        return true;
      }
    }
    // 卡片范围内的其余触摸：吞掉（不然会穿透到下层列表，虽然列表自己也挡了一层）
    if (ev.mX >= kCardX && ev.mX < kCardX + kCardW &&
        ev.mY >= kCardY && ev.mY < kCardY + kCardH) {
      return true;
    }
  }
  return false;  // 卡片外 / 键盘区域：全部放行（按钮/键盘/列表各归各的）
}

/* ---------- 主页 ---------- */

static bool onButtonClick_BtnWifiBack(ZKButton *pButton) {
  (void)pButton;
  /* ★ 这行日志是**故意的**（2026-09-16）：用户报「左上角返回图标不可用」，
   *   而排查时"点完画面不变"有两种可能 —— ① 回调根本没被调用（触摸没到按钮）、
   *   ② 回调被调用了但 `closeActivity` 没生效。没有这行日志两者**从外部完全无法区分**
   *   （回调里原本一个 LOGD 都没有，我为此白排查一轮）。 */
  LOGD("wifiLogic: 顶栏返回被点击 -> closeActivity(wifiActivity)");
  EASYUICONTEXT->closeActivity("wifiActivity");
  return false;
}

static bool onButtonClick_BtnRescan(ZKButton *pButton) {
  (void)pButton;
  if (!sWm) return false;
  if (!sWm->isWifiEnable()) {
    if (mTextWifiStatePtr) mTextWifiStatePtr->setText("无线局域网已关闭");
    return false;
  }
  LOGD("wifiLogic: 手动扫描");
  sScanWaitUntil = nowMs() + SCAN_WAIT_MS;
  sWm->scan();
  return false;
}

/* 底部「信号探针」按钮：2026-09-15 起信号探针是**独立应用**（probe.ftu），
 * 这里只负责跳过去 —— 本页不再自带探针窗口（原 WinProbe 已删）。
 * ⚠️ 跳转前**不用**关本页：openActivity 把本页压到后台，返回时还在 WiFi 页。 */
static bool onButtonClick_BtnProbe(ZKButton *pButton) {
  (void)pButton;
  LOGD("wifiLogic: 打开信号探针应用（probeActivity）");
  EASYUICONTEXT->openActivity("probeActivity");
  return false;
}


/* ---------- 扫描列表（主页） ---------- */

static int getListItemCount_ListWifiAp(const ZKListView *pListView) {
  (void)pListView;
  return (int)sScan.size();
}

/* 只显示/隐藏某一行里的某个子项（没有变化就不写 —— obtainListItemData 在列表滚动时
 * 会被反复调用，避免无谓的 setVisible 造成重绘）。 */
static void subVisible(ZKListView::ZKListItem *item, int id, bool on) {
  if (!item) return;
  ZKListView::ZKListSubItem *si = item->findSubItemByID(id);
  if (si && si->isVisible() != on) si->setVisible(on);
}

static void obtainListItemData_ListWifiAp(ZKListView *pListView,
                                          ZKListView::ZKListItem *pListItem,
                                          int index) {
  (void)pListView;
  if (!pListItem || index < 0 || index >= (int)sScan.size()) return;
  const WifiInfo &ap = sScan[index];

  /* ★ iOS 分组卡（2026-09-15 改版）：整组列表是**一整张卡** —— 首行上圆角、末行下圆角、
   *   中间行上下都直角，行与行之间一条 1px 分隔线。
   *   但 listview 的 item 模板只有一份、做不出"按位置换底图"，所以 ui/wifi.html 里把
   *   四张角图（RowCardTop/Mid/Bot/Solo）叠在同一位置，这里按 index 只显示一张
   *   —— 与主界面 Icon0..23 按 slot 切图是同一手法（见 mainLogic.cc 的 syncRowIcon）。
   *   分隔线画在底图里（末行那张没有线），所以这里不用管线，只切图。 */
  const int n = (int)sScan.size();
  const bool isTop = (index == 0);
  const bool isBot = (index == n - 1);
  const bool isSolo = (n == 1);
  subVisible(pListItem, ID_WIFI_RowCardTop, isTop && !isSolo);
  subVisible(pListItem, ID_WIFI_RowCardBot, isBot && !isSolo);
  subVisible(pListItem, ID_WIFI_RowCardSolo, isSolo);
  subVisible(pListItem, ID_WIFI_RowCardMid, !isTop && !isBot);

  ZKListView::ZKListSubItem *ssid = pListItem->findSubItemByID(ID_WIFI_SubApSsid);
  ZKListView::ZKListSubItem *info = pListItem->findSubItemByID(ID_WIFI_SubApInfo);
  ZKListView::ZKListSubItem *lock = pListItem->findSubItemByID(ID_WIFI_SubApLock);
  if (ssid) {
    std::string s = ap.getSsid();
    ssid->setText(s.empty() ? "隐藏网络" : s);
    ssid->setTextColor(C_TEXT);
  }
  char sec[24];
  secText(ap.getEncryption(), sec, sizeof(sec));
  if (info) {
    int c = chanOfFreq(ap.getFreq());
    bool mine = isConnectedSsid(ap);
    char b[96];
    if (c > 0) {
      snprintf(b, sizeof(b), "信道 %d · %s%s", c, sec, mine ? " · 已连接" : "");
    } else {
      snprintf(b, sizeof(b), "%s%s", sec, mine ? " · 已连接" : "");
    }
    info->setText(b);
    info->setTextColor(mine ? C_TEXT_GREEN : C_TEXT_DIM);
  }
  /* ★ 信号强度：2026-09-15 从"档位词文字"改成**三段弧线图标**。
   *   用户原话："信号强度和加密方式图标有点太过于抽象了" —— 文字要"读"，
   *   而手机上是**扫一眼**就知道强弱。四张图叠在同一位置，按 rssi 只显示一张
   *   —— 与行底图（RowCardTop/Mid/Bot/Solo）同一手法。
   *   分档与配色（图里已经烘好）：满格/强 3 段绿 · 中 2 段青 · 弱 1 段青 · 很差 1 段灰。
   *   ⚠️ 原来的 dBm 数字 2026-09-15 已去掉（要精确值用 QA `dump`）。 */
  const int rssi = ap.getRssi();
  const bool s3 = (rssi >= -65);
  const bool s2 = (!s3 && rssi >= -75);
  const bool s1 = (!s3 && !s2 && rssi >= -85);
  subVisible(pListItem, ID_WIFI_SubApSig3, s3);
  subVisible(pListItem, ID_WIFI_SubApSig2, s2);
  subVisible(pListItem, ID_WIFI_SubApSig1, s1);
  subVisible(pListItem, ID_WIFI_SubApSig0, (!s3 && !s2 && !s1));
  /* 锁图标只在**加密网络**上出现（开放网络挂锁是错的）。 */
  if (lock) {
    bool locked = (strcmp(sec, "开放") != 0);
    if (lock->isVisible() != locked) lock->setVisible(locked);
  }
}

static void onListItemClick_ListWifiAp(ZKListView *pListView, int index,
                                       int id) {
  (void)pListView;
  (void)id;
  /* 密码弹窗显示期间**不响应列表点击**。
   * 这是原来"触摸坐标白名单"要解决的同一件事（弹窗空白处穿透会误连别的网络），
   * 但改在**真正发起连接的地方**挡：比在触摸坐标上猜可靠，也不会误吞键盘的按键
   * （血案：白名单只到 y=394，键盘在 y=424..800 ⇒ 键盘整个被吞，敲不进字）。 */
  if (sPwdDlgOpen) {
    LOGD("wifiLogic: 弹窗显示中，忽略列表点击 #%d（防误连）", index);
    return;
  }
  if (index < 0 || index >= (int)sScan.size() || !sWm) return;
  const WifiInfo &ap = sScan[index];
  snprintf(sSelSsid, sizeof(sSelSsid), "%s", ap.getSsid().c_str());
  secText(ap.getEncryption(), sSelSec, sizeof(sSelSec));
  sSelOpen = (strcmp(sSelSec, "开放") == 0);
  LOGD("wifiLogic: 选网 %s (%s) open=%d", sSelSsid, sSelSec, sSelOpen ? 1 : 0);

  if (sSelOpen) {
    // 免密直连
    sWm->connect(sSelSsid, "");
    if (mTextWifiStatePtr) mTextWifiStatePtr->setText("正在连接...");
  } else {
    // 弹密码框（**半屏软键盘**输入 —— kbOpen 会挂画布、清缓冲、刷显示）
    openPwdDialog();
  }
}


/* ---------- 密码弹窗 ---------- */

/* 输入法在我们的 IME 应用里（src/logic/imeApp.cc）；这里只有三个入口：
 *   ① 点「取消」   -> 关弹窗
 *   ② 点「连接」   -> 拿 EditText 当前文本发起连接
 *   ③ IME 按「完成」-> 框架把整串写回 EditText -> onEditTextChanged_EditPwd -> 等价于点①
 *      （用户因此不用打完密码再回头点一次按钮） */

static bool onButtonClick_BtnPwdCancel(ZKButton *pButton) {
  (void)pButton;
  LOGD("wifiLogic: 密码框取消");
  if (mWinWifiPwdPtr) mWinWifiPwdPtr->hideWnd();
  sPwdDlgOpen = false;
  return false;
}

static bool onButtonClick_BtnPwdOk(ZKButton *pButton) {
  (void)pButton;
  if (!sWm) return false;
  std::string pw = mEditPwdPtr ? mEditPwdPtr->getText() : "";
  LOGD("wifiLogic: 连接 %s (pw len=%d)", sSelSsid, (int)pw.size());
  keepHomeBeforeSwitch(sSelSsid);
  sWm->connect(sSelSsid, pw);
  if (mWinWifiPwdPtr) mWinWifiPwdPtr->hideWnd();
  sPwdDlgOpen = false;
  if (mTextWifiStatePtr) mTextWifiStatePtr->setText("正在连接...");
  return false;
}

static void onEditTextChanged_EditPwd(const std::string &text) {
  if (sPwdSilentSet) {   // 打开弹窗时的程序化清空：不是用户提交
    LOGD("wifiLogic: 密码框程序化置空（忽略，不连接）");
    return;
  }
  if (text.empty()) {   // 空提交（例如 IME 被清空后误提交）：不能拿空密码去连
    LOGW("wifiLogic: 密码框收到空提交，忽略（不发起连接）");
    return;
  }
  LOGD("wifiLogic: 密码框收到提交（%d 字符）-> 直接连接", (int)text.size());
  onButtonClick_BtnPwdOk(0);
}


static bool onButtonClick_BtnProbeBg(ZKButton* pButton) {
  LOGD_TRACE("BtnProbeBg click");
  return false;
}


