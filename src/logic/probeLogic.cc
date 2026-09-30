#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif

/**
 * @brief 当界面隐藏时触发
 */
static void onUI_hide() {
  LOGD_TRACE("");
}
/*
 * probeLogic.cc - 信号探针独立应用（probe.ftu）逻辑层
 *
 * 功能参考 iOS「Find Spy 隐藏设备探测器」，**全部用 FlyThings 原生控件**实现。
 * 五个页面（同一 ftu 内的整屏普通 window；前四个由页签切换，第五个是整屏浮层）：
 *
 *   WinProbeWifi （默认可见）  WiFi 探测：附近 AP 总览 + 可疑判定 + 信道建议 + 排序列表
 *   WinProbeBt   （初始隐藏）  蓝牙探测：BLE 设备列表（复用 pg::Bt 主机扫描）
 *   WinProbeLan  （初始隐藏）★ 局域网设备：**主动探测**（邻居发现 + MAC 厂商 + 端口 +
 *                             RTSP/HTTP 协议指纹）⇒ 找 STA 型偷拍设备的正解
 *   WinProbeSniff（初始隐藏）★ 无线嗅探：把闲置的 wlan1 切成 MONITOR 顺听空口
 *                             ⇒ 同信道所有发射者（含隐身的 STA）+ 真 RSSI +
 *                               **从 assoc 帧里解出隐藏网络的真名**
 *   WinHunt      （整屏浮层）  热点猎手：锁定目标实时追踪 RSSI（大号 dBm + 12 段条 + 蜂鸣）
 *
 * ⚠️ 加页签/加页必须**四个页面一起改**（每个整屏页各带一套页签：浮层会盖住别页的页签），
 *    并且 tools/gen_ui.py 的 HIDDEN_PAGE_WINDOWS / OPAQUE_WINDOWS 要跟着加（踩过）。
 *
 * 线程模型与 wifiLogic 一致：
 *   - zknet 的 WifiManager 回调来自它自己的线程 ⇒ 回调里**只置标志 / 拷数据**，
 *     控件一律在 onUI_Timer 里改；
 *   - 蓝牙侧 pg::Bt 已经把 btstack 关在它自己的线程里（见 PgBt.h），
 *     这里只调它公开的扫描查询接口（scannedCount/Addr/Name/Rssi），**不碰 btstack**。
 *   - 所有控件写入都先"比值"，只在变化时写（防重绘风暴，工程记忆第 14 条）。
 *
 * ⚠️ 触摸回调里**不许按坐标吞触摸**（血案见 docs/wifi-app.md §9.7）：
 *    整屏浮层（IME / 屏保 / OSD）的触摸会经过本 Activity，`return true` 会把它全废掉。
 *    要挡就在"真正发起动作的地方"挡（列表点击回调开头判状态）。
 *
 * ⚠️ 扫描/追踪都是**长动作**：WiFi 单次扫描 1~3 秒。所以每个动作都带
 *    "进度标志 + 上限超时"（工程记忆第 10 条：判进展看增量、超时用"停滞 + 上限"）。
 *
 * ⚠️ **能力边界**（必须说清楚，别让人误以为"一定能扫出摄像头"）：
 *    ① WiFi 探测页 = **被动观察**（只看空口广播了什么），命中关键词只代表"值得看一眼"；
 *    ② 局域网设备页 = **主动探测**：只有"连进同一张网 + 设备会应答"才看得见；
 *       端口 + RTSP 指纹能**坐实"这是一路视频流"**，但"它是不是偷拍用的"仍要人判断；
 *    ③ 无线嗅探页 = 只能看**当前这一个信道**（单射频），且看不到加密内容。
 *    细节与实测证据见 docs/wifi-probe-app.md §9。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <string>
#include <vector>

#include "platform/PgSaver.h"    // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgAlarm.h"
#include "platform/PgAudio.h"
#include "platform/PgBt.h"       // BLE 主机扫描（周边设备 + RSSI）
#include "platform/PgSkin.h"     // pg::applyRoundedBg（动态底色按钮的圆角）
#include "platform/PgLan.h"      // 局域网设备扫描（邻居发现 + 端口 + 协议指纹）
#include "platform/PgSniff.h"    // 无线嗅探（monitor 顺听）
#include "platform/PgWifiCfg.h"  // WiFi「回家快照」：切网前存盘、退出目标网模式后还原
#include "utils/Log.h"

#ifdef FUN_BUILD
#include "entry/EasyUIContext.h"
#include "net/NetManager.h"
#include "net/WifiManager.h"
#include "net/WifiInfo.h"
#endif

namespace {

/* ==================== 常量 ==================== */
const int TIMER_UI = 1;      // 常规刷新（400ms）
const int TIMER_HUNT = 2;    // 追踪页重扫（1500ms）
const int TIMER_KEY = 3;     // 按键长按轮询（100ms）
const long UI_MS = 400;
const long HUNT_MS = 1500;
const int KEY_MS = 100;
const long SCAN_WAIT_MS = 12000;  // "正在扫描" 最长显示时长（超时自动收）
const long LONG_PRESS_MS = 700;   // 与主 KeyRouter / wifiLogic 一致

// 配色（与 ui/probe.html 一致；动态底色的那几个必须与 PgSkin.kStyles 的值对得上）
const uint32_t C_TEXT = 0xFFF2F2F7;
const uint32_t C_TEXT_DIM = 0xFF9A9AA0;
const uint32_t C_TEXT_BLUE = 0xFF64D2FF;
const uint32_t C_TEXT_GREEN = 0xFF30D158;
const uint32_t C_TEXT_GOLD = 0xFFFF9F0A;
const uint32_t C_TEXT_RED = 0xFFFF453A;
const uint32_t C_BG_GRAY = 0xFF2C2C2E;   // SURFACE2 —— 次级按钮
const uint32_t C_BG_SEGOFF = 0xFF1C1C1E;  // SURFACE —— 未选中分段：与段容器同色 = **隐形**
                                          //   （ui/main.html 的分段控件就是这个观感：
                                          //    容器 + 一枚"只看得见选中那一块"的胶囊）
const uint32_t C_BG_SEL = 0xFF3A3A3C;    // SURFACE3 —— 分段选中胶囊
const uint32_t C_BG_GREEN = 0xFF30D158;  // SUCCESS
const uint32_t C_BG_BLUE = 0xFF0A84FF;   // ACCENT —— 「只看风险」筛选开启态
                                         //   （蓝=已激活的筛选器，比红更中性：
                                         //    红要留给"高危"本身，别抢它的语义）

/* ==================== 运行期状态 ==================== */
WifiManager *sWm = 0;
bool sListenerAdded = false;

std::vector<WifiInfo> sScan;       // 最近一次扫描结果（按 RSSI 降序）
volatile bool sScanDirty = true;   // 列表需要重建
/* ★ QA 注入的假 AP（"钉住"，只给 riskrank/apinject 验收用；生产环境恒空）。
 * 为什么必须钉住：zknet 会**周期性上报扫描结果**，而 `handleWifiScanResult` 是
 * 整体赋值 `sScan = *infos` ⇒ 注入项活不过几秒，抓屏时看到的还是旧列表
 * （实测浪费了两轮排查：以为"注入没生效"，其实是"生效了又被覆盖"）。 */
std::vector<WifiInfo> sInjectAp;

volatile int  sScanSeq = 0;        // 扫描结果**次数**：内容变了就 +1。
                                   // ⚠️ 不能只看 size —— 重扫后数量相同（很常见）时
                                   //    筛选视图不会重建，列表就"看着没变"。
volatile long sScanWaitUntil = 0;  // >0 = 正在扫描（到点自动收）
volatile long sScanStamp = 0;      // 每收到一轮结果 +1（追踪页据此判"新数据"）

pg::Bt *sBt = 0;                   // 蓝牙单例（懒取，见 btGet()）
int sBtDevCount = -1;              // 蓝牙设备数缓存（变了才 refreshListView）

int sTab = 0;                      // 0=WiFi / 1=蓝牙 / 2=局域网 / 3=嗅探
const char *kTabName[4] = {"WiFi 探测", "蓝牙探测", "局域网设备", "无线嗅探"};

/* ---- 追踪目标（热点猎手） ---- */
enum { TG_NONE = 0, TG_AP = 1, TG_BLE = 2 };
int sTgtKind = TG_NONE;
char sTgtKey[20] = {0};      // AP: BSSID（找不到就退化为 "#SSID"）/ BLE: 地址
char sTgtLabel[64] = {0};    // 显示名
char sTgtSub[80] = {0};      // 副标题（频段/信道/加密 或 地址）
int sTgtRssi = 0;            // 最近一次原始 RSSI
double sTgtAvg = 0;          // 平滑值（首次直接取原始值）
int sTgtPeak = -127;         // 最强记录
int sTgtSamples = 0;         // 采样次数
int sTgtMiss = 0;            // 连续没扫到目标的次数
long sTgtStamp = -1;         // 上次采样用的 sScanStamp
bool sHuntOn = false;        // 追踪中
bool sBeep = true;           // 蜂鸣开关
int sBeepTick = 0;           // 蜂鸣节拍计数（TIMER_UI 累加）
int sLastBars = -1;          // 上次的强度条段数

/* 上次写入控件的文本缓存（先比值再写） */
char sLastSum1[160] = {0};
char sLastSum2[160] = {0};
char sLastSum3[160] = {0};
char sLastBtState[64] = {0};
char sLastBtAdv[64] = {0};
char sLastBtCount[32] = {0};
char sLastBtTip[128] = {0};
char sLastHuntTgt[64] = {0};
char sLastHuntSub[80] = {0};
char sLastHuntDbm[24] = {0};
char sLastHuntMeta[64] = {0};
char sLastHuntLevel[64] = {0};
char sLastHuntPeak[24] = {0};
char sLastHuntAvg[32] = {0};
char sLastHuntTip[160] = {0};

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

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
  if (strstr(e, "WPA2") || strstr(e, "WPA3")) snprintf(buf, (size_t)n, "WPA2");
  else if (strstr(e, "WPA")) snprintf(buf, (size_t)n, "WPA");
  else if (strstr(e, "WEP")) snprintf(buf, (size_t)n, "WEP");
  else snprintf(buf, (size_t)n, "%s", e);
}

/* 只在变化时写控件 */
void setTextIfChanged(ZKTextView *tv, const char *val, char *cache, int cn) {
  if (!tv) return;
  if (!val) val = "";
  if (cn > 0 && strncmp(cache, val, (size_t)cn) == 0) return;
  snprintf(cache, (size_t)cn, "%s", val);
  tv->setText(cache);
}

/* 动态底色按钮：底色 + 圆角图一起设（顺序不能反，见 platform/PgSkin.h）。
 * ⚠️ 圆角图是**烘底**的 ⇒ on 必须传对：
 *    分段坐在段容器（#1C1C1E）上 → ON_CARD；页面上的按钮 → ON_PAGE。 */
void setCtrlBg(ZKBase *v, uint32_t color, pg::OnSurface on, int *cache) {
  if (!v) return;
  if (cache && *cache == (int)color) return;
  if (cache) *cache = (int)color;
  v->setBackgroundColor(color);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
  pg::applyRoundedBg(v, color, on);
}

pg::Bt *btGet() {
  if (!sBt) sBt = pg::Bt::instance();
  return sBt;
}

/* ==================== 可疑设备判定（特征库） ==================== */
enum { RISK_NONE = 0, RISK_WATCH = 1, RISK_SUSPECT = 2, RISK_HIGH = 3 };

// 高危关键词（命中即 3）
const char *kHighKw[] = {
    "camera", "ipcam", "ipcamera", "spycam", "spy", "pinhole", "hidcam",
    "偷拍", "摄像头", "摄像机", "监控", "针孔", "录像",
};
/* 「可疑」(2)：**明确的**监控/摄像头迹象（命中即 2，但要放在高危之后判 ——
 * "cam" 比 "camera" 松，先判高危才不会把 "IPCamera" 降级成 2）。
 *
 * ⚠️⚠️ 分级纪律（2026-09-15 真机踩到，见 docs/wifi-probe-app.md §14）：
 *    这里**只放"看到就想走过去看一眼"的专有词**。
 *    凡是"智能设备/消费电子也普遍会用"的泛词（iot / wireless / mini / video /
 *    dv …）**一律降到 kLowKw(1)** —— 否则智能插座、灯泡、扫地机会全被打成
 *    "可疑"，把真正的摄像头淹没掉，恰好与"帮我找出摄像头"的诉求相反。
 *    （血案：环境里 `BE88U_IoT` 因含 "iot" 被判 2 级"疑似监控设备"。） */
const char *kWatchKw[] = {
    // 通用线索（4 字母以上，子串匹配不会误伤）
    "cam", "cctv", "nvr", "dvr",
    // 具体 IPC 品牌 / 模组名（廉价摄像头最爱用这些）
    "v380", "ycc365", "camhi", "besder", "espcam", "wifcam", "smartcam", "icam",
    // 芯片模组命名（ESP 系做摄像头极多）
    "esp_", "esp-", "esp32",
    // 用途线索（婴儿/儿童监视器）
    "kids", "baby",
};

/* 「可疑」(2) 但**只认词首**的短词/型号前缀 —— 子串匹配会误伤（见 matchKwPrefix）。 */
const char *kPrefKw[] = {"ipc", "gc-", "gc_", "x5-", "x6-", "a4-"};

/* 「关注」(1)：**泛词** —— 可能相关，但智能家居/普通设备也大量使用。
 * 命中只给 1 级 ⇒ 列表里带一条弱灰色条 + "关注"，**但不进「只看风险」筛选**
 * （筛选门槛是 >= 可疑）。这样既不漏、又不吵。 */
const char *kLowKw[] = {
    "iot", "wireless", "mini", "video", "eye", "watch", "hidden", "secret", "smart",
};
// 摄像头/IoT 模组常见 OUI（BSSID 前三段）—— 命中给 2
const char *kCamOui[] = {
    "44:19:b6", "bc:ad:28", "c0:56:e3",                                     // 海康威视
    "3c:ef:8c", "4c:11:bf", "e0:50:8b",                                     // 大华
    "24:0a:c4", "3c:71:bf", "84:f3:eb", "a4:cf:12", "7c:df:a1", "48:3f:da",  // Espressif
    "b0:c5:54", "50:02:91",                                                 // 通用 IPCam 模组
};

// 把名字转小写（ASCII），中文原样留（关键词里也有中文，直接 strstr 即可）
void lowerCopy(const char *src, char *dst, int n) {
  int i = 0;
  for (; src && src[i] && i < n - 1; ++i) {
    char c = src[i];
    dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
  }
  dst[i] = 0;
}

int matchKw(const char *lower, const char *const *tab, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (strstr(lower, tab[i])) return (int)i;
  return -1;
}

/**
 * **词首匹配**：要求词出现在 SSID 开头。
 *
 * 为什么必须分开：2~3 个字母的短词用子串匹配一定误伤 ——
 * 血案 `MiniPC-5G` 含 "ipc"（m-i-n-**i-p-c**）被判"疑似监控设备"；
 * `a4-` 更险（HP 打印机热点常叫 "DIRECT-A4-…"）。
 * 而真正的 IPC 设备命名恰恰都是**词首**（`IPC-1234` / `GC-ABCDEF` / `A4-…`）。
 */
bool matchKwPrefix(const char *lower, const char *const *tab, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (strncmp(lower, tab[i], strlen(tab[i])) == 0) return true;
  return false;
}

/** 判定一个 WiFi AP 的风险；why 出参带原因串（直接显示在列表右侧）。 */
int riskOfAp(const WifiInfo &ap, char *why, int wn) {
  char name[96];
  lowerCopy(ap.getSsid().c_str(), name, sizeof(name));
  const bool hidden = ap.getSsid().empty();

  if (!hidden) {
    if (matchKw(name, kHighKw, sizeof(kHighKw) / sizeof(kHighKw[0])) >= 0) {
      snprintf(why, (size_t)wn, "高危 · 名称含摄像头词");
      return RISK_HIGH;
    }
    if (matchKw(name, kWatchKw, sizeof(kWatchKw) / sizeof(kWatchKw[0])) >= 0 ||
        matchKwPrefix(name, kPrefKw, sizeof(kPrefKw) / sizeof(kPrefKw[0]))) {
      snprintf(why, (size_t)wn, "可疑 · 名称疑似监控设备");
      return RISK_SUSPECT;
    }
  }
  {  // OUI 命中（BSSID 前三段，小写比对）
    char bssid[32];
    lowerCopy(ap.getBssid().c_str(), bssid, sizeof(bssid));
    for (size_t i = 0; i < sizeof(kCamOui) / sizeof(kCamOui[0]); ++i) {
      if (strncmp(bssid, kCamOui[i], 8) == 0) {
        snprintf(why, (size_t)wn, "可疑 · 厂商 OUI 像是摄像头模组");
        return RISK_SUSPECT;
      }
    }
  }
  if (hidden) {
    snprintf(why, (size_t)wn, "关注 · 隐藏 SSID");
    return RISK_WATCH;
  }
  /* 泛词放最后判（在 OUI 之后）：OUI 是硬件级证据，比"名字里有个 iot"硬得多，
   * 所以 OUI 命中时已经在上一步返回 2 级了。 */
  if (matchKw(name, kLowKw, sizeof(kLowKw) / sizeof(kLowKw[0])) >= 0) {
    snprintf(why, (size_t)wn, "关注 · 名称像智能/影像设备");
    return RISK_WATCH;
  }
  why[0] = 0;
  return RISK_NONE;
}

/** 判定一个 BLE 设备的风险。name 可能为空（匿名广播）。 */
int riskOfBle(const char *name, int rssi, char *why, int wn) {
  char low[96];
  lowerCopy(name, low, sizeof(low));
  if (!low[0]) {
    /* 匿名 + 就在身边 = 最像"藏在房间里的东西"（很多针孔摄像头 BLE 不广播名字） */
    if (rssi >= -60) {
      snprintf(why, (size_t)wn, "可疑 · 匿名设备且信号很强");
      return RISK_SUSPECT;
    }
    snprintf(why, (size_t)wn, "关注 · 匿名广播");
    return RISK_WATCH;
  }
  if (matchKw(low, kHighKw, sizeof(kHighKw) / sizeof(kHighKw[0])) >= 0) {
    snprintf(why, (size_t)wn, "高危 · 名称含摄像头词");
    return RISK_HIGH;
  }
  if (matchKw(low, kWatchKw, sizeof(kWatchKw) / sizeof(kWatchKw[0])) >= 0) {
    snprintf(why, (size_t)wn, "可疑 · 名称疑似监控设备");
    return RISK_SUSPECT;
  }
  why[0] = 0;
  return RISK_NONE;
}

uint32_t riskColor(int lv) {
  if (lv >= RISK_HIGH) return C_TEXT_RED;
  if (lv == RISK_SUSPECT) return C_TEXT_GOLD;
  return C_TEXT_DIM;
}

/* ==================== WiFi 扫描（zknet 监听器） ==================== */
/* ★★ 列表排序 = **风险优先**，同风险再按信号强度降序。
 *
 * 为什么不能继续用"纯 RSSI"（原来的 rssiDesc）：用户实测反馈
 * **"扫出来很多数据，但不知道谁是摄像头"** —— 本环境一轮扫到 30+ 个网络，
 * 可疑 AP 被淹没在邻居的 ChinaNet / aWiFi 里，得逐行看右侧小字才找得到。
 * 现在风险高的排最前，进页第一屏就是该看的那些。
 *
 * （局域网设备表的排序在 platform/PgLan.cpp 的 rebuildOrderLocked 里，
 *   同样是"风险降序 → 同风险按 IP 升序"，两页规则一致。） */
bool apRiskDesc(const WifiInfo &a, const WifiInfo &b) {
  char wa[64], wb[64];
  const int la = riskOfAp(a, wa, sizeof(wa));
  const int lb = riskOfAp(b, wb, sizeof(wb));
  if (la != lb) return la > lb;
  if (a.getRssi() != b.getRssi()) return a.getRssi() > b.getRssi();
  return a.getSsid() < b.getSsid();
}

class ProbeWifiEvents : public WifiManager::IWifiListener {
 public:
  void handleWifiEnable(E_WIFI_ENABLE event, int args) override {
    (void)event;
    (void)args;
    sScanDirty = true;
  }
  void handleWifiScanResult(std::vector<WifiInfo> *infos) override {
    /* ⚠️ 这里跑在 WifiManager 的内部线程 —— 只做"拷数据 + 排序 + 置标志"，
     *    一个控件都不要碰（跨线程改控件会偶发花屏/卡死）。 */
    if (!infos) return;
    sScan = *infos;
    /* QA 注入项"钉住"：扫描结果整体覆盖后重新挂上（生产环境 sInjectAp 恒空 ⇒ 无影响） */
    for (size_t k = 0; k < sInjectAp.size(); ++k) sScan.push_back(sInjectAp[k]);
    std::sort(sScan.begin(), sScan.end(), apRiskDesc);   // ★ 风险优先，不是纯 RSSI
    ++sScanSeq;
    sScanDirty = true;
    sScanWaitUntil = 0;
    sScanStamp++;
  }
  void handleWifiConnect(E_WIFI_CONNECT event, int args) override {
    (void)event;
    (void)args;
    sScanDirty = true;
  }
  void handleWifiErrorCode(E_WIFI_ERROR_CODE code) override { (void)code; }
};
ProbeWifiEvents sEvents;

bool startScan() {
  if (!sWm) return false;
  if (!sWm->isWifiEnable()) {
    LOGD("probeLogic: WiFi 未开 -> 先打开再扫");
    sWm->enableWifi(true);
    sScanWaitUntil = nowMs() + SCAN_WAIT_MS + 4000;   // 起卡要几秒
    return true;
  }
  sScanWaitUntil = nowMs() + SCAN_WAIT_MS;
  sWm->scan();
  return true;
}

/* ==================== 信道占用 / 总览 ==================== */
int occCount(int ch) {
  int c = 0;
  for (size_t i = 0; i < sScan.size(); ++i)
    if (chanOfFreq(sScan[i].getFreq()) == ch) ++c;
  return c;
}

/* ==================== 「只看风险」筛选 + 风险色条 ====================
 * 解决"扫出一堆数据但不知道谁是摄像头"的最后一步：
 * 把手机 / 路由器 / 邻居的 WiFi 滤掉，**剩下的就是嫌疑设备**。
 *
 * 只做**视图映射**（int 下标数组），不动底层数据：
 *   • LAN 的顺序由 PgLan::hostCopy 负责（风险降序 → IP 升序）
 *   • AP 的顺序由 sScan 的排序负责（apRiskDesc）
 * 筛选只是再滤掉 level < 可疑 的那些。
 */
bool sLanOnly = false, sApOnly = false;
int sLanView[96], sLanViewN = 0, sLanViewStamp = -1;
int sApView[96], sApViewN = 0, sApViewStamp = -1;
int sBgLanOnly = -1, sBgApOnly = -1;
char sLastLanOnlyBtn[40] = {0};
char sLastApOnlyBtn[40] = {0};

void rebuildLanView() {
  pg::Lan *l = pg::Lan::instance();
  const int stamp = (l->generation() << 1) | (sLanOnly ? 1 : 0);
  if (stamp == sLanViewStamp) return;
  sLanViewStamp = stamp;
  sLanViewN = 0;
  const int n = l->hostCount();
  for (int i = 0; i < n && sLanViewN < 96; ++i) {
    pg::LanHost h;
    if (!l->hostCopy(i, &h)) continue;
    if (sLanOnly && h.level < RISK_SUSPECT) continue;
    sLanView[sLanViewN++] = i;
  }
}

void rebuildApView() {
  const int stamp = (sScanSeq << 1) | (sApOnly ? 1 : 0);
  if (stamp == sApViewStamp) return;
  sApViewStamp = stamp;
  sApViewN = 0;
  char why[64];
  for (size_t i = 0; i < sScan.size() && sApViewN < 96; ++i) {
    if (sApOnly && riskOfAp(sScan[i], why, sizeof(why)) < RISK_SUSPECT) continue;
    sApView[sApViewN++] = (int)i;
  }
}

/** 风险等级 → 行首色条颜色（普通设备用行卡底色 ⇒ 视觉上隐形，不干扰）。 */
uint32_t riskBarColor(int lv) {
  if (lv >= RISK_HIGH) return C_TEXT_RED;      // 高危：红
  if (lv == RISK_SUSPECT) return C_TEXT_GOLD;  // 可疑：金
  if (lv == RISK_WATCH) return 0xFF3A3A3C;     // 关注：弱灰（SURFACE3）
  return 0xFF1C1C1E;                           // 普通：与行卡同色 ⇒ 看不见
}

/* ⚠️ 色条**不挂圆角图**：setCtrlBg 会 setBackgroundPic(.9.png)，
 *    而 5px 宽的小控件挂九宫格会画出奇怪的边。色条本来就是直角，正好。
 *    （⚠️ 只 setBackgroundColor 是**看不到变化**的，必须配 setBgStatusColor
 *      —— 工程硬规则，见 clocksuiteLogic.cc / imeApp.cc 的注释。） */
void setRiskBar(ZKListView::ZKListSubItem *v, int lv) {
  if (!v) {
    /* ⚠️ 找不到色条控件时**要出声**（只打前两次，避免刷屏）—— 否则
     *    "色条不显示"会变成一个查不出原因的静默失败（踩过）。 */
    static int warned = 0;
    if (warned < 2) { ++warned; LOGD("probeLogic: 找不到行首色条控件（ID 写错？）"); }
    return;
  }
  const uint32_t c = riskBarColor(lv);
  v->setBackgroundColor(c);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, c);
}

void syncWifiSummary() {
  int n24 = 0, n5 = 0, nSus = 0, nWatch = 0, nOpen = 0, nHigh = 0;
  char why[64], sec[24];
  char topSsid[64] = {0}, topWhy[48] = {0};
  int topRssi = 0, topLv = 0;
  /* ⚠️ sScan 已按 **风险降序** 排好（apRiskDesc）⇒ 循环里第一台 level>=2 的
   *    就是最可疑的那个，直接记下来做"点名"。 */
  for (size_t i = 0; i < sScan.size(); ++i) {
    if (sScan[i].getFreq() >= 5000) ++n5;
    else ++n24;
    secText(sScan[i].getEncryption(), sec, sizeof(sec));
    if (strcmp(sec, "开放") == 0) ++nOpen;
    int lv = riskOfAp(sScan[i], why, sizeof(why));
    if (lv >= RISK_SUSPECT) {
      ++nSus;
      if (lv >= RISK_HIGH) ++nHigh;
      if (!topSsid[0]) {
        snprintf(topSsid, sizeof(topSsid), "%s",
                 sScan[i].getSsid().empty() ? "(隐藏网络)" : sScan[i].getSsid().c_str());
        snprintf(topWhy, sizeof(topWhy), "%s", why);
        topRssi = sScan[i].getRssi();
        topLv = lv;
      }
    } else if (lv == RISK_WATCH) {
      ++nWatch;
    }
  }

  char b[256];

  /* ---- 筛选条（放在 empty 早退**之前**，任何状态下按钮文案/配色都对）---- */
  if (sApOnly) snprintf(b, sizeof(b), "显示全部（%d 个）", (int)sScan.size());
  else if (nSus) snprintf(b, sizeof(b), "只看风险设备（%d 个）", nSus);
  else snprintf(b, sizeof(b), "只看风险设备");
  setCtrlBg(mBtnApOnlyPtr, sApOnly ? C_BG_BLUE : C_BG_GRAY, pg::ON_PAGE, &sBgApOnly);
  if (mBtnApOnlyPtr) mBtnApOnlyPtr->setTextColor(sApOnly ? 0xFFFFFFFF : C_TEXT);
  setTextIfChanged(mBtnApOnlyPtr, b, sLastApOnlyBtn, sizeof(sLastApOnlyBtn));

  /* ---- 第一行：有可疑就直接报可疑（不再报"附近 N 个网络"这种对用户无意义的数）---- */
  if (sScanWaitUntil) snprintf(b, sizeof(b), "正在扫描附近网络...");
  else if (sScan.empty()) snprintf(b, sizeof(b), "没有扫描到网络（WiFi 是否已打开？）");
  else if (sApOnly && !nSus) snprintf(b, sizeof(b), "筛选下没有可疑网络（附近共 %d 个）",
                                      (int)sScan.size());
  else if (nHigh) snprintf(b, sizeof(b), "！%d 个高危网络 · 附近共 %d 个", nHigh, (int)sScan.size());
  else if (nSus) snprintf(b, sizeof(b), "！%d 个可疑网络 · 附近共 %d 个", nSus, (int)sScan.size());
  else snprintf(b, sizeof(b), "附近 %d 个网络 · 未发现可疑（2.4G %d / 5G %d）",
                (int)sScan.size(), n24, n5);
  setTextIfChanged(mTextSumL1Ptr, b, sLastSum1, sizeof(sLastSum1));
  if (mTextSumL1Ptr)
    mTextSumL1Ptr->setTextColor(nHigh ? C_TEXT_RED : (nSus ? C_TEXT_GOLD
                                                 : (sScan.empty() ? C_TEXT : C_TEXT_GREEN)));

  if (sScan.empty()) {
    setTextIfChanged(mTextSumL2Ptr, "", sLastSum2, sizeof(sLastSum2));
    setTextIfChanged(mTextSumL3Ptr, "", sLastSum3, sizeof(sLastSum3));
    return;
  }

  /* ---- 第二行：**点名最可疑的那一个**（"谁是摄像头"的直接答案）---- */
  if (topSsid[0]) {
    snprintf(b, sizeof(b), "%s · %s · %d dBm · %s", topSsid, topWhy, topRssi,
             topLv >= RISK_HIGH ? "高危" : "可疑");
  } else {
    snprintf(b, sizeof(b), "可疑 %d 个 · 关注 %d 个 · 开放 %d 个", nSus, nWatch, nOpen);
  }
  setTextIfChanged(mTextSumL2Ptr, b, sLastSum2, sizeof(sLastSum2));
  if (mTextSumL2Ptr)
    mTextSumL2Ptr->setTextColor(topSsid[0] ? (topLv >= RISK_HIGH ? C_TEXT_RED : C_TEXT_GOLD)
                                           : C_TEXT_DIM);

  /* 信道建议：只在 2.4G 的 1/6/11 里挑（这三个互不重叠，是最现实的"换信道"选项）。
   * 都挤的话给相对最空的；顺带报最挤的那个，用户能看懂"为什么建议它"。 */
  const int cand[3] = {1, 6, 11};
  int best = cand[0], bestN = occCount(cand[0]);
  int crowded = cand[0], crowdedN = bestN;
  for (int i = 1; i < 3; ++i) {
    int c = occCount(cand[i]);
    if (c < bestN) {
      bestN = c;
      best = cand[i];
    }
    if (c > crowdedN) {
      crowdedN = c;
      crowded = cand[i];
    }
  }
  snprintf(b, sizeof(b), "信道建议 CH%d（%d 个占用）· 最挤 CH%d（%d 个）", best, bestN,
           crowded, crowdedN);
  setTextIfChanged(mTextSumL3Ptr, b, sLastSum3, sizeof(sLastSum3));
}

/* ==================== 蓝牙页 ==================== */
const char *btTipOf(pg::Bt *bt) {
  if (!bt) return "";
  if (bt->errCode() == -1) return "初始化失败：BT 需断电冷启动后重试";
  int st = bt->state();
  if (st == 0) return "未启动 · 点「启动蓝牙」开始";
  if (st == 1) return "正在初始化蓝牙（约需 20~40 秒）...";
  if (bt->scannedCount() == 0) return "蓝牙已就绪，正在等广播（保持本页几秒）";
  return "点一行即可锁定它并进入热点猎手";
}

void syncBtPage() {
  pg::Bt *bt = btGet();
  if (!bt) return;
  char b[96];
  setTextIfChanged(mTextBtStateVPtr, bt->stateText(), sLastBtState, sizeof(sLastBtState));
  if (mTextBtStateVPtr) {
    mTextBtStateVPtr->setTextColor(bt->errCode() == -1 ? C_TEXT_GOLD
                                  : (bt->state() == 2 ? C_TEXT_GREEN : C_TEXT));
  }
  snprintf(b, sizeof(b), "广播累计 %d 条", bt->advCount());
  setTextIfChanged(mTextBtAdvPtr, b, sLastBtAdv, sizeof(sLastBtAdv));

  int n = bt->scannedCount();
  snprintf(b, sizeof(b), "%d 个设备", n);
  setTextIfChanged(mTextBtCountPtr, b, sLastBtCount, sizeof(sLastBtCount));
  if (mTextBtCountPtr) mTextBtCountPtr->setTextColor(n > 0 ? C_TEXT_BLUE : C_TEXT_DIM);

  setTextIfChanged(mTextBtTipPtr, btTipOf(bt), sLastBtTip, sizeof(sLastBtTip));
  if (mTextBtTipPtr) {
    mTextBtTipPtr->setTextColor(bt->errCode() == -1 ? C_TEXT_GOLD : C_TEXT_DIM);
  }

  if (n != sBtDevCount) {
    sBtDevCount = n;
    if (mListProbeBtPtr) mListProbeBtPtr->refreshListView();
  }
}

/* ==================== 局域网设备页（主动探测） ====================
 * 平台层在 platform/PgLan.h：邻居发现（UDP 触发内核 ARP + 读 /proc/net/arp）
 * → SSDP 顺听设备型号 → 摄像头端口扫描 + **RTSP/HTTP 协议指纹** → 风险判定。
 * 这是"找 STA 型偷拍设备"的正解 —— AP 扫描根本看不见它们（见 docs/wifi-probe-app.md §9）。
 *
 * ⚠️ 一轮完整扫描 10~25 秒，跑在 worker 线程里：本页只读它的表 + 按 generation 刷控件。
 */
/* ---- 目标网模式（切网扫描的"回家快照"，见 platform/PgWifiCfg.h）----
 * 为什么要有它：zknet 只保留**一个** network 条目 ⇒ 连一次目标网就把原网络的
 * SSID+密码顶掉（血案见 docs/wifi-app.md 末尾）。所以"切之前存、退出时还原"。
 */
int sBgLanMode = -1;
char sLastLanModeBtn[24] = {0};
bool sModeSawOther = false;   // 目标网模式期间**是否真的连过别的网**
                              // （刚进模式时还在原网上，不能马上判定"已经回家了"）
/* 还原后的重连兜底：wpa_supplicant 的 RECONFIGURE+RECONNECT 不保证一定成
 * ⇒ 8 秒没连上就交给 zknet 直连（用快照里存下来的 ssid/psk），再等 15 秒判成败。 */
int sReconnectStage = 0;      // 0 空闲 / 1 等控制口生效 / 2 已交 zknet
long sReconnectAt = 0;
char sReconnectSsid[64] = {0};
char sReconnectPsk[96] = {0};

pg::WifiCfg *wcGet() { return pg::WifiCfg::instance(); }

std::string connectedSsid() {
#ifdef FUN_BUILD
  if (sWm && sWm->isConnected()) {
    WifiInfo *cur = sWm->getConnectionInfo();
    if (cur) return cur->getSsid();
  }
#endif
  return std::string();
}

void tickReconnect() {
  if (!sReconnectStage) return;
  const std::string cur = connectedSsid();
  if (cur == sReconnectSsid && !cur.empty()) {
    LOGD("probeLogic: ★ 原网络 '%s' 已恢复连接（%s）", sReconnectSsid,
         sReconnectStage == 1 ? "wpa_supplicant 自己连上" : "zknet 直连成功");
    sReconnectStage = 0;
    return;
  }
  if (sReconnectStage == 1) {
    if (nowMs() - sReconnectAt > 8000) {
      LOGD("probeLogic: 8 秒还没连上原网络 -> 交给 zknet 直连 '%s'", sReconnectSsid);
      sReconnectStage = 2;
      sReconnectAt = nowMs();
#ifdef FUN_BUILD
      if (sWm) sWm->connect(sReconnectSsid, sReconnectPsk);
#endif
    }
    return;
  }
  if (sReconnectStage == 2 && nowMs() - sReconnectAt > 15000) {
    LOGD("probeLogic: 原网络 '%s' 仍未连上（大概不在覆盖范围）—— 配置已还原，等它自己回来",
         sReconnectSsid);
    sReconnectStage = 0;
  }
}

pg::Lan *sLan = 0;
int sLanGen = -1;
int sLanHosts = -1;
char sLastLan1[200] = {0};
char sLastLan2[200] = {0};
char sLastLan3[200] = {0};
char sLastLanBtn[32] = {0};

pg::Lan *lanGet() {
  if (!sLan) sLan = pg::Lan::instance();
  return sLan;
}

const char *lanPhaseText(int ph) {
  switch (ph) {
    case 1: return "正在找邻居（ARP 扫段）";
    case 2: return "正在发 SSDP 找设备型号";
    case 3: return "正在探端口 / 协议指纹";
    default: return "正在收尾";
  }
}

int portCountOf(const char *p) {
  if (!p || !p[0]) return 0;
  int n = 1;
  for (const char *q = p; *q; ++q)
    if (*q == ' ') ++n;
  return n;
}

void syncLanPage() {
  pg::Lan *l = lanGet();
  char b[220], ip[24] = {0}, mask[24] = {0};
  const bool hasNet = l->netInfo(ip, sizeof(ip), mask, sizeof(mask));
  const int n = l->hostCount();
  const bool run = l->running();

  /* ---- 先统计风险，并记下"第一台风险设备"的点名信息 ----
   * ⚠️ hostCopy 已按 **风险降序** 返回（见 PgLan::rebuildOrderLocked）
   *    ⇒ 第一台 level>=2 的就是最可疑的那台，直接写进汇总卡第二行，
   *      用户不用滚动列表就知道"谁是摄像头"。 */
  int nCam = 0, nWithPort = 0, nHigh = 0;
  char topIp[20] = {0}, topWhy[52] = {0};
  int topLv = 0;
  for (int i = 0; i < n; ++i) {
    pg::LanHost h;
    if (!l->hostCopy(i, &h)) continue;
    if (h.ports[0]) ++nWithPort;
    if (h.level >= RISK_SUSPECT) {
      ++nCam;
      if (h.level >= RISK_HIGH) ++nHigh;
      if (!topIp[0]) {
        snprintf(topIp, sizeof(topIp), "%s", h.ip);
        snprintf(topWhy, sizeof(topWhy), "%s", h.why[0] ? h.why : h.kind);
        topLv = h.level;
      }
    }
  }

  /* ---- 第一行：**结论**（有风险就直接报，而不是报"在线 N 台设备"）---- */
  if (run) snprintf(b, sizeof(b), "%s %d%%", lanPhaseText(l->phase()), l->progress());
  else if (!hasNet) snprintf(b, sizeof(b), "WiFi 没连上 —— 先连进目标网络");
  else if (sLanOnly && !nCam) snprintf(b, sizeof(b), "筛选下没有风险设备（共 %d 台在线）", n);
  else if (nCam) snprintf(b, sizeof(b), "%s %d 台疑似摄像头", nHigh ? "！高危" : "！发现", nCam);
  else if (n) snprintf(b, sizeof(b), "在线 %d 台设备 · 未发现可疑", n);
  else snprintf(b, sizeof(b), "还没有扫描");
  setTextIfChanged(mTextLanL1Ptr, b, sLastLan1, sizeof(sLastLan1));
  if (mTextLanL1Ptr) {
    mTextLanL1Ptr->setTextColor((run || !hasNet) ? C_TEXT_GOLD
                                 : (nCam ? (nHigh ? C_TEXT_RED : C_TEXT_GOLD)
                                         : (n ? C_TEXT_GREEN : C_TEXT)));
  }

  /* ---- 目标网模式：状态收敛 + 文案 ---- */
  pg::WifiCfg *wc = wcGet();
  std::string cur = connectedSsid();
  if (wc->targetMode()) {
    if (!cur.empty() && wc->homeSsid()[0] && cur != wc->homeSsid()) sModeSawOther = true;
    /* 用户自己又切回原网络了 ⇒ 自动退出模式（否则按钮一直写着"恢复原网"，很迷惑）。
     * ⚠️ 必须等"真的连过别的网"之后再判，否则**刚进模式那一刻**（还在原网上）就被清掉了。 */
    else if (sModeSawOther && !cur.empty() && cur == wc->homeSsid()) {
      wc->clearTargetMode();
      sModeSawOther = false;
      LOGD("probeLogic: 当前已是原网络 '%s' -> 自动退出目标网模式", wc->homeSsid());
    }
  } else {
    sModeSawOther = false;
  }

  /* ---- 第二行：有风险就**点名**，否则报网络信息 ---- */
  if (nCam && topIp[0]) {
    snprintf(b, sizeof(b), "%s · %s · %s", topIp, topWhy, topLv >= RISK_HIGH ? "高危" : "可疑");
  } else if (hasNet) {
    const char *curName = cur.empty() ? l->ifName() : cur.c_str();
    if (wc->targetMode() && wc->homeSsid()[0]) {
      snprintf(b, sizeof(b), "%s · 原网 %s 已存", curName, wc->homeSsid());
    } else if (wc->hasHome() && wc->homeSsid()[0] && cur != wc->homeSsid()) {
      snprintf(b, sizeof(b), "%s · %s · 可切回 %s", curName, l->subnetText(), wc->homeSsid());
    } else if (wc->hasHome() && wc->homeSsid()[0]) {
      snprintf(b, sizeof(b), "%s · %s · 原网已存", curName, l->subnetText());
    } else {
      snprintf(b, sizeof(b), "%s · %s", curName, l->subnetText());
    }
  } else {
    snprintf(b, sizeof(b), "没有拿到 IP（wlan0 未连接）");
  }
  setTextIfChanged(mTextLanL2Ptr, b, sLastLan2, sizeof(sLastLan2));
  if (mTextLanL2Ptr) {
    mTextLanL2Ptr->setTextColor(nCam ? (topLv >= RISK_HIGH ? C_TEXT_RED : C_TEXT_GOLD)
                                     : C_TEXT_DIM);
  }

  /* ---- 目标网按钮：进模式变绿 + 文字变"恢复原网" ---- */
  setCtrlBg(mBtnLanModePtr, wc->targetMode() ? C_BG_GREEN : C_BG_GRAY, pg::ON_PAGE, &sBgLanMode);
  if (mBtnLanModePtr) mBtnLanModePtr->setTextColor(wc->targetMode() ? 0xFF000000 : C_TEXT);
  setTextIfChanged(mBtnLanModePtr, wc->targetMode() ? "恢复原网" : "目标网", sLastLanModeBtn,
                   sizeof(sLastLanModeBtn));

  /* ---- 第三行：下一步动作 ---- */
  if (run) snprintf(b, sizeof(b), "扫描中：已发现 %d 台（探端口会主动连接对端）", n);
  else if (nCam) snprintf(b, sizeof(b), "列表已按风险排序 · 点下面「只看风险设备」只看它们");
  else if (n) snprintf(b, sizeof(b), "有开放端口 %d 台 · 点「深探端口」看协议指纹", nWithPort);
  else if (wc->targetMode())
    snprintf(b, sizeof(b), "目标网模式：去 WiFi 应用连上目标网，回来点「扫描局域网」");
  else snprintf(b, sizeof(b), "点下面「扫描局域网」开始（主动探测，对端会留记录）");
  setTextIfChanged(mTextLanL3Ptr, b, sLastLan3, sizeof(sLastLan3));
  if (mTextLanL3Ptr) mTextLanL3Ptr->setTextColor(nCam ? C_TEXT_GOLD : C_TEXT_BLUE);

  /* ---- 「只看风险」筛选条 ---- */
  if (sLanOnly) snprintf(b, sizeof(b), "显示全部（%d 台）", n);
  else if (nCam) snprintf(b, sizeof(b), "只看风险设备（%d 台）", nCam);
  else snprintf(b, sizeof(b), "只看风险设备");
  setCtrlBg(mBtnLanOnlyPtr, sLanOnly ? C_BG_BLUE : C_BG_GRAY, pg::ON_PAGE, &sBgLanOnly);
  if (mBtnLanOnlyPtr) mBtnLanOnlyPtr->setTextColor(sLanOnly ? 0xFFFFFFFF : C_TEXT);
  setTextIfChanged(mBtnLanOnlyPtr, b, sLastLanOnlyBtn, sizeof(sLastLanOnlyBtn));

  setTextIfChanged(mTextLanScanLblPtr, run ? "扫描中..." : "扫描局域网", sLastLanBtn,
                   sizeof(sLastLanBtn));

  /* 表变了才刷列表（worker 每完成一台就 +1 generation，400ms 轮询足够跟手） */
  const int g = l->generation();
  if (g != sLanGen || n != sLanHosts) {
    sLanGen = g;
    sLanHosts = n;
    if (mListProbeLanPtr) mListProbeLanPtr->refreshListView();
  }
}

/* ==================== 无线嗅探页（monitor 顺听） ====================
 * 平台层在 platform/PgSniff.h：把**闲置的 wlan1** 切成 MONITOR（不动 wlan0/zknet），
 * AF_PACKET 抓帧 → radiotap 取信道/RSSI → 802.11 取发射者 + 管理帧里的 SSID。
 */
pg::Sniff *sSn = 0;
int sSnTick = 0;
char sLastSnState[64] = {0};
char sLastSnChan[24] = {0};
char sLastSnInfo[200] = {0};
char sLastSnTip[220] = {0};
char sLastSnPeersV[16] = {0};
char sLastSnFramesV[24] = {0};
char sLastSnSsidsV[16] = {0};

/* ==================== 全信道嗅探（跳信道）====================
 * ★★ 为什么必须断网（实测，见 docs/wifi-probe-app.md §13）：
 *    单射频驱动下**占用信道的是 wlan0 这个接口本身**，不是"关联状态"——
 *      wlan0 关联着 / up 但没关联  => SET_CHANNEL 返回 -16 EBUSY
 *      **wlan0 down**               => SET_CHANNEL 返回 0，全信道都能切
 *    ⇒ 想扫别的信道只能真把 wlan0 关掉。代价：这段时间**设备没有网络**
 *      （局域网页也用不了，所以两者互斥）。
 * ★ 复用「回家快照」把代价消掉：
 *      进 = 存快照（整份 conf + ssid/psk）→ wlan0 down → 开跳信道
 *      出 = 关跳信道 → wlan0 up → 还原 + 重连（8 秒没上再交给 zknet 直连兜底）
 * ⚠️ 退出应用（onUI_quit）必须调 leaveFullHop —— 否则接口留在 down 状态，
 *    用户一出去就是"WiFi 坏了"（这是本功能最大的风险点）。
 */
/* ⚠️ 前置声明：这两个取单例的函数定义在下面（snGet 在嗅探段） */
pg::Sniff *snGet();
pg::WifiCfg *wcGet();

bool sHopMode = false;               // 全信道模式开着的标记（= 我们关过 wlan0）
int sBgSnHop = -1;
char sLastSnHopLbl[24] = {0};

bool enterFullHop() {
  pg::Sniff *sn = snGet();
  if (!sn->active()) {
    LOGD("probeLogic: 全信道需要先「开始嗅探」（monitor 口还没起）");
    return false;
  }
  pg::WifiCfg *wc = wcGet();
  /* 与「目标网模式」互斥：一个要连进目标网扫、一个要断网扫，同时开没意义 */
  if (wc->targetMode()) {
    char s2[64] = {0}, p2[96] = {0};
    wc->leaveTargetMode(s2, sizeof(s2), p2, sizeof(p2));
    LOGD("probeLogic: 全信道与目标网模式互斥 -> 已退出目标网模式（原网 '%s'）", s2);
  }
  /* ★ 顺序不能反：**先存快照，再关网卡** —— 关掉之后就不知道原来连的是谁了 */
  const std::string cur = connectedSsid();
  if (!cur.empty()) {
    if (wc->keepHome(cur.c_str()))
      LOGD("probeLogic: 已保存原网络 '%s'（退出全信道时自动恢复）", wc->homeSsid());
  } else {
    LOGD("probeLogic: 当前没连网络 -> 跳过快照，只做跳信道");
  }
  wc->setStaIfaceUp(false);        /* ★ 关键一步：wlan0 down，射频才释放 */
  sn->setHop(true);
  sHopMode = true;
  LOGD("probeLogic: ★ 全信道模式 ON（已断开 WiFi，%d 个信道轮扫）", sn->hopChanTotal());
  return true;
}

void leaveFullHop(const char *why) {
  if (!sHopMode) return;
  pg::Sniff *sn = snGet();
  if (sn->active()) sn->setHop(false);
  /* ★★ 必须**连嗅探一起停掉**（`Sniff::stop()` 会把 wlan1 切回 STATION）。
   *   为什么（2026-09-15 真机实测，绕了一大圈）：退出全信道后 wlan1 若**还留在 MONITOR**，
   *   单射频下它会**干扰 wlan0 的收发** —— 症状极具误导性：
   *     wlan0 **关联是成功的**（`CTRL-EVENT-CONNECTED`），但 DHCP 反复失败：
   *         `E/DHCP: Truncated packet` × N  ⇒ **永远拿不到 IP**（ping 不通）。
   *   停掉嗅探（wlan1 → STATION）后**立刻**：
   *         `D/DHCP: server 192.168.0.1, lease 7200` + `configuring wlan0` ⇒ IP 回来、ping 通。
   *   ⇒ **单射频上"STA 联网"与"monitor 嗅探"是互斥的**，退出全信道必须一起停。
   *     用户想继续嗅探，再点一次「开始嗅探」即可。 */
  const bool wasSniffing = sn->active();
  if (wasSniffing) sn->stop();
  pg::WifiCfg *wc = wcGet();
  wc->setStaIfaceUp(true);         /* ★ 必须 up 回来 */
  sHopMode = false;
  if (wasSniffing)
    LOGD("probeLogic: 全信道退出时一并停了嗅探（wlan1 回 STATION）—— 否则 monitor 口会"
         "干扰 wlan0 收包，DHCP 拿不到 IP");
  if (wc->hasHome() && wc->homeSsid()[0]) {
    char ssid[64] = {0}, psk[96] = {0};
    bool ok = wc->leaveTargetMode(ssid, sizeof(ssid), psk, sizeof(psk));
    LOGD("probeLogic: 全信道模式 OFF（%s）：配置还原 %s，原网络 '%s'", why,
         ok ? "成功" : "失败", ssid);
    if (ok && ssid[0]) {
      snprintf(sReconnectSsid, sizeof(sReconnectSsid), "%s", ssid);
      snprintf(sReconnectPsk, sizeof(sReconnectPsk), "%s", psk);
      sReconnectStage = 1;         /* 复用 LAN 段的"8 秒没上就交给 zknet"兜底 */
      sReconnectAt = nowMs();
    }
  } else {
    /* ★★ 没有快照也必须"叫醒" wpa_supplicant（2026-09-15 真机踩到）：
     *    全信道模式里接口是 down 的 ⇒ 退出来只 up 的话，接口是 up 了但
     *    **没有 running、没有 IP**（`Address not available`、ping 不通）：
     *    wpa_supplicant 认为自己是断开的，zknet 也等不到连接事件。
     *    手动发一次 RECONFIGURE + REASSOCIATE 立刻恢复（实测 IP + ping 1.8ms）。
     *    ⇒ 以后"接口 down/up 循环"这个套路，都要记得补这一步。 */
    usleep(300000);              /* 等接口 up 到底再发命令 */
    char resp[256] = {0};
    int r1 = wc->wpaCmd("RECONFIGURE", resp, sizeof(resp));
    LOGD("probeLogic: 无快照 -> RECONFIGURE %d '%s'", r1, resp);
    usleep(200000);
    resp[0] = 0;
    int r0 = wc->wpaCmd("ENABLE_NETWORK all", resp, sizeof(resp));
    LOGD("probeLogic: 无快照 -> ENABLE_NETWORK all %d", r0);
    usleep(200000);
    resp[0] = 0;
    int r2 = wc->wpaCmd("REASSOCIATE", resp, sizeof(resp));
    LOGD("probeLogic: 全信道模式 OFF（%s）：无快照 -> 已唤醒原网络（REASSOCIATE %d）", why, r2);
  }
}

pg::Sniff *snGet() {
  if (!sSn) sSn = pg::Sniff::instance();
  return sSn;
}

/** 嗅探页的风险判定 —— 只用"空口上真能拿到的东西"：SSID 名 + MAC 厂商 + 是否隐藏 AP。 */
int riskOfSniff(const pg::SniffPeer &p, char *why, int wn) {
  why[0] = 0;
  char low[40];
  lowerCopy(p.ssid, low, sizeof(low));
  if (low[0]) {
    if (matchKw(low, kHighKw, sizeof(kHighKw) / sizeof(kHighKw[0])) >= 0) {
      snprintf(why, (size_t)wn, "名称含摄像头词");
      return RISK_HIGH;
    }
    if (matchKw(low, kWatchKw, sizeof(kWatchKw) / sizeof(kWatchKw[0])) >= 0) {
      snprintf(why, (size_t)wn, "名称疑似监控设备");
      return RISK_SUSPECT;
    }
  }
  if (p.flags & pg::SNIFF_F_HIDDEN_AP) {
    if (p.flags & pg::SNIFF_F_RESOLVED) {
      snprintf(why, (size_t)wn, "隐藏AP(名字已解出)");
      return RISK_SUSPECT;
    }
    snprintf(why, (size_t)wn, "隐藏AP");
    return RISK_WATCH;
  }
  if (pg::pgOuiIsCam(p.mac)) {
    snprintf(why, (size_t)wn, "安防厂商设备");
    return RISK_SUSPECT;
  }
  return RISK_NONE;
}

void syncSniffPage() {
  pg::Sniff *s = snGet();
  char b[240];
  const bool on = s->active();

  setTextIfChanged(mTextSnStateVPtr, s->stateText(), sLastSnState, sizeof(sLastSnState));
  if (mTextSnStateVPtr) mTextSnStateVPtr->setTextColor(on ? C_TEXT_GREEN : C_TEXT_GOLD);

  const int ch = s->channel();
  const bool hop = s->hopActive();
  if (hop) snprintf(b, sizeof(b), "CH%d · %d/%d", s->hopCurChan(), s->hopScanned() % 99,
                    s->hopChanTotal());
  else if (ch > 0) snprintf(b, sizeof(b), "CH %d", ch);
  else if (on) snprintf(b, sizeof(b), "CH 同步中");
  else snprintf(b, sizeof(b), "CH --");
  setTextIfChanged(mTextSnChanPtr, b, sLastSnChan, sizeof(sLastSnChan));
  if (mTextSnChanPtr) mTextSnChanPtr->setTextColor(hop ? C_TEXT_GOLD : C_TEXT_BLUE);

  if (on && hop) {
    snprintf(b, sizeof(b), "全信道跳扫 · 运行 %d 秒 · 已扫 %d 轮（失败 %ld 次）",
             s->elapsedSec(), s->hopRounds(), s->hopFail());
  } else if (on) {
    snprintf(b, sizeof(b), "%s 监听中 · 运行 %d 秒 · 数据帧 %ld", s->ifName(),
             s->elapsedSec(), s->dataFrames());
  } else {
    snprintf(b, sizeof(b), "点「开始嗅探」把闲置的 %s 切成监听口", s->ifName());
  }
  setTextIfChanged(mTextSnInfoPtr, b, sLastSnInfo, sizeof(sLastSnInfo));

  if (sHopMode) {
    /* 全信道模式：这是"设备暂时没网"的状态，必须说清楚 + 说明会自动恢复 */
    snprintf(b, sizeof(b), "⚠ 已断网（原网 '%s' 已存）· 退出会一并停止嗅探并恢复网络",
             wcGet()->homeSsid()[0] ? wcGet()->homeSsid() : "-");
  } else if (on && s->hiddenResolved() > 0) {
    snprintf(b, sizeof(b), "★ 已从关联帧解出 %d 个隐藏网络的名字", s->hiddenResolved());
  } else if (on) {
    snprintf(b, sizeof(b), "顺听中：点「全信道」可轮扫 %d 个信道（会暂时断网）",
             s->hopChanTotal());
  } else {
    snprintf(b, sizeof(b), "退出本应用会自动把 %s 切回 station", s->ifName());
  }
  setTextIfChanged(mTextSnTipPtr, b, sLastSnTip, sizeof(sLastSnTip));
  if (mTextSnTipPtr)
    mTextSnTipPtr->setTextColor(sHopMode ? C_TEXT_RED
                                         : (s->hiddenResolved() > 0 ? C_TEXT_GOLD : C_TEXT_DIM));

  /* 「全信道」开关按钮（动态底色 ⇒ 圆角由 setCtrlBg 挂；文字跟着状态变） */
  setCtrlBg(mBtnSnHopPtr, hop ? C_BG_GREEN : C_BG_GRAY, pg::ON_PAGE, &sBgSnHop);
  if (mBtnSnHopPtr) mBtnSnHopPtr->setTextColor(hop ? 0xFF000000 : C_TEXT);
  setTextIfChanged(mBtnSnHopPtr, hop ? "全信道●" : "全信道", sLastSnHopLbl,
                   sizeof(sLastSnHopLbl));

  snprintf(b, sizeof(b), "%d", s->peerCount());
  setTextIfChanged(mTextSnPeersVPtr, b, sLastSnPeersV, sizeof(sLastSnPeersV));
  snprintf(b, sizeof(b), "%ld", s->frames());
  setTextIfChanged(mTextSnFramesVPtr, b, sLastSnFramesV, sizeof(sLastSnFramesV));
  snprintf(b, sizeof(b), "%d", s->ssidCount());
  setTextIfChanged(mTextSnSsidsVPtr, b, sLastSnSsidsV, sizeof(sLastSnSsidsV));
}

/* ==================== 热点猎手 ==================== */

/** 12 段强度条：RSSI → 可见段数（-90dBm 以下 0 段；-35dBm 以上全亮）。
 *  段位配色是**固定的**（前 4 段红 / 中 4 段金 / 后 4 段绿），逻辑层只切 visible
 *  ⇒ 亮得越多绿色越多，天然表达"越近越强"。顺带避开"运行时改底色"的坑。 */
int barsOf(int rssi) {
  if (!rssi) return 0;
  int v = rssi + 90;
  if (v < 0) v = 0;
  if (v > 55) v = 55;
  return (v * 12 + 27) / 55;   // 四舍五入
}

const char *levelOf(int rssi) {
  if (!rssi) return "还没有数据";
  if (rssi >= -50) return "极强 · 就在眼前";
  if (rssi >= -60) return "很强 · 非常近了";
  if (rssi >= -70) return "中等 · 继续靠近";
  if (rssi >= -80) return "偏弱 · 换个方向找";
  return "很弱 · 还差得远";
}

/** 记一次采样（EMA 平滑 + 峰值保持）。 */
void huntSample(int rssi) {
  sTgtRssi = rssi;
  if (sTgtAvg == 0) sTgtAvg = rssi;
  else sTgtAvg = sTgtAvg * 0.6 + rssi * 0.4;
  if (rssi > sTgtPeak) sTgtPeak = rssi;
  sTgtSamples++;
}

int huntBleRssiOf(const char *addr, bool *found) {
  *found = false;
  pg::Bt *bt = sBt;
  if (!bt) return 0;
  int n = bt->scannedCount();
  for (int i = 0; i < n; ++i) {
    if (strcmp(bt->scannedAddr(i), addr) != 0) continue;
    *found = true;
    return bt->scannedRssi(i);
  }
  return 0;
}

/** 把追踪目标对齐到最新数据。
 *  "新数据"的判据：WiFi 用 sScanStamp（每轮扫描 +1，**同一轮不重复采样**，
 *  否则 400ms 的轮询会把同一个 RSSI 采样 6 次、均值被灌水）；
 *  BLE 用"原始 RSSI 变了"（广播是持续上报的，值一变就是新数据）。 */
void huntTick() {
  if (sTgtKind == TG_AP) {
    if (sScanStamp == sTgtStamp) return;   // 本轮扫描还没回来
    sTgtStamp = sScanStamp;
    bool found = false;
    for (size_t i = 0; i < sScan.size(); ++i) {
      bool hit = false;
      if (sTgtKey[0] == '#') {
        hit = (strcmp(sScan[i].getSsid().c_str(), sTgtKey + 1) == 0);
      } else if (sTgtKey[0]) {
        hit = (strcmp(sScan[i].getBssid().c_str(), sTgtKey) == 0);
      }
      if (!hit) hit = (sScan[i].getSsid() == sTgtLabel);
      if (!hit) continue;
      found = true;
      huntSample(sScan[i].getRssi());
      break;
    }
    if (found) sTgtMiss = 0;
    else sTgtMiss++;
  } else if (sTgtKind == TG_BLE) {
    bool found = false;
    int r = huntBleRssiOf(sTgtKey, &found);
    if (found) {
      sTgtMiss = 0;
      if (r != sTgtRssi || sTgtSamples == 0) huntSample(r);   // 值变了才是新数据
    } else {
      sTgtMiss++;
    }
  }
}

/** 蜂鸣：越强越密（盖革计数器的手感），由 TIMER_UI（400ms）驱动。
 *  ⚠️ 主界面音效开关关掉时 playSfx 是静默的（不是 bug），要出声先确认音效开着。 */
void huntBeepTick(int bars) {
  if (!sHuntOn || !sBeep) return;
  int period = 0;
  if (bars >= 10) period = 1;        // 400ms
  else if (bars >= 7) period = 2;    // 800ms
  else if (bars >= 4) period = 4;    // 1600ms
  if (!period || (sBeepTick % period) != 0) return;
  pg::DeviceAudio *a = pg::globalAudio();
  if (a) a->playSfx(pg::SFX_CLICK);
}

void setBars(int n) {
  ZKTextView *bars[12] = {mBar0Ptr, mBar1Ptr,  mBar2Ptr,  mBar3Ptr,  mBar4Ptr, mBar5Ptr,
                          mBar6Ptr, mBar7Ptr,  mBar8Ptr,  mBar9Ptr,  mBar10Ptr, mBar11Ptr};
  if (n < 0) n = 0;
  if (n > 12) n = 12;
  for (int i = 0; i < 12; ++i) {
    if (!bars[i]) continue;
    bool want = (i < n);
    if (bars[i]->isVisible() != want) bars[i]->setVisible(want);
  }
}

void syncHuntPage() {
  char b[96];
  if (sTgtKind == TG_NONE) {
    setTextIfChanged(mTextHuntTgtPtr, "未锁定目标", sLastHuntTgt, sizeof(sLastHuntTgt));
    setTextIfChanged(mTextHuntSubPtr, "回列表点一个网络 / 蓝牙设备开始追踪", sLastHuntSub,
                     sizeof(sLastHuntSub));
    setTextIfChanged(mTextHuntDbmPtr, "--", sLastHuntDbm, sizeof(sLastHuntDbm));
    setTextIfChanged(mTextHuntMetaPtr, "", sLastHuntMeta, sizeof(sLastHuntMeta));
    setTextIfChanged(mTextHuntLevelPtr, "等待锁定目标", sLastHuntLevel,
                     sizeof(sLastHuntLevel));
    setTextIfChanged(mTextPeakVPtr, "-- dBm", sLastHuntPeak, sizeof(sLastHuntPeak));
    setTextIfChanged(mTextAvgVPtr, "-- dBm", sLastHuntAvg, sizeof(sLastHuntAvg));
    setTextIfChanged(mTextHuntTipPtr, "拿稳设备慢慢走动，信号最强的地方就是目标所在",
                     sLastHuntTip, sizeof(sLastHuntTip));
    setBars(0);
    return;
  }

  setTextIfChanged(mTextHuntTgtPtr, sTgtLabel, sLastHuntTgt, sizeof(sLastHuntTgt));
  setTextIfChanged(mTextHuntSubPtr, sTgtSub, sLastHuntSub, sizeof(sLastHuntSub));

  const bool lost = (sTgtMiss > 3);
  int bars = lost ? 0 : barsOf((int)sTgtAvg);

  snprintf(b, sizeof(b), "%d", lost ? 0 : (int)sTgtAvg);
  setTextIfChanged(mTextHuntDbmPtr, lost ? "--" : b, sLastHuntDbm, sizeof(sLastHuntDbm));
  if (mTextHuntDbmPtr) {
    mTextHuntDbmPtr->setTextColor(lost ? C_TEXT_DIM
                              : (bars >= 9 ? C_TEXT_GREEN
                                           : (bars >= 5 ? C_TEXT_GOLD : C_TEXT_RED)));
  }

  snprintf(b, sizeof(b), "dBm · %s · 采样 %d", sTgtKind == TG_AP ? "WiFi" : "蓝牙",
           sTgtSamples);
  setTextIfChanged(mTextHuntMetaPtr, b, sLastHuntMeta, sizeof(sLastHuntMeta));

  setTextIfChanged(mTextHuntLevelPtr, lost ? "暂时扫不到这个目标" : levelOf((int)sTgtAvg),
                   sLastHuntLevel, sizeof(sLastHuntLevel));

  if (sTgtPeak == -127) snprintf(b, sizeof(b), "-- dBm");
  else snprintf(b, sizeof(b), "%d dBm", sTgtPeak);
  setTextIfChanged(mTextPeakVPtr, b, sLastHuntPeak, sizeof(sLastHuntPeak));

  snprintf(b, sizeof(b), "%d dBm", (int)sTgtAvg);
  setTextIfChanged(mTextAvgVPtr, b, sLastHuntAvg, sizeof(sLastHuntAvg));

  if (lost) {
    setTextIfChanged(mTextHuntTipPtr,
                     "目标暂时没有广播 —— 往信号强的方向走两步，或点「重新锁定」",
                     sLastHuntTip, sizeof(sLastHuntTip));
  } else if (bars >= 9) {
    setTextIfChanged(mTextHuntTipPtr, "非常近了！沿墙、插座、天花板、通风口仔细看",
                     sLastHuntTip, sizeof(sLastHuntTip));
  } else if (sTgtSamples < 3) {
    setTextIfChanged(mTextHuntTipPtr, "正在建立基线，走动 10 秒让数值稳定下来",
                     sLastHuntTip, sizeof(sLastHuntTip));
  } else {
    setTextIfChanged(mTextHuntTipPtr, "拿稳设备慢慢走动，信号最强的地方就是目标所在",
                     sLastHuntTip, sizeof(sLastHuntTip));
  }
  setBars(bars);
  sLastBars = bars;
}

void huntResetBaseline() {
  sTgtAvg = 0;
  sTgtPeak = -127;
  sTgtSamples = 0;
  sTgtMiss = 0;
  sTgtStamp = -1;
  sLastBars = -1;
}

/* ==================== 分段控件 / 页面切换 ==================== */
int sBgBeep = -1;
/* 页签的底色缓存：4 个页面 × 4 个段位。用数组而不是 16 个具名变量 ——
 * 加页签（4→5）时只改一个数字，不会漏掉某一页（漏一页的症状就是"某页页签不亮"）。 */
int sBgTab[4][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}};

void setSeg(ZKButton *btn, bool on, int *cache) {
  setCtrlBg(btn, on ? C_BG_SEL : C_BG_SEGOFF, pg::ON_CARD, cache);
  if (btn) btn->setTextColor(on ? C_TEXT : C_TEXT_DIM);
}

void syncTabs() {
  /* ⚠️ 每个整屏页**各带一套页签**（不能共用一条浮层页签）——
   *    整屏浮层"显示着"就会吃掉全系统**控件级**触摸，这正是工程记忆第 17 条的坑
   *    （血案：状态栏浮层）。页签条做成浮层的话，下面的列表就点不动了。
   *    代价是 4 页 × 4 段 = 16 个控件，加页签要四页一起改（见文件头注释）。 */
  ZKButton *row[4][4] = {
      {mTabWifiPtr, mTabBtPtr, mTabLanPtr, mTabSnPtr},
      {mTabWifi2Ptr, mTabBt2Ptr, mTabLan2Ptr, mTabSn2Ptr},
      {mTabWifi3Ptr, mTabBt3Ptr, mTabLan3Ptr, mTabSn3Ptr},
      {mTabWifi4Ptr, mTabBt4Ptr, mTabLan4Ptr, mTabSn4Ptr},
  };
  for (int pg = 0; pg < 4; ++pg)
    for (int t = 0; t < 4; ++t) setSeg(row[pg][t], sTab == t, &sBgTab[pg][t]);
  /* 蜂鸣按钮是动态底色，初始在源稿里没有图 ⇒ 进页时必须挂一次圆角 */
  setCtrlBg(mBtnHuntBeepPtr, sBeep ? C_BG_GREEN : C_BG_GRAY, pg::ON_PAGE, &sBgBeep);
  if (mBtnHuntBeepPtr) mBtnHuntBeepPtr->setTextColor(sBeep ? 0xFF000000 : C_TEXT);
}

void showTab(int t) {
  if (t < 0 || t > 3) return;
  sTab = t;
  /* ⚠️ 切页签必须**先把「热点猎手」收起来**（2026-09-15 真机踩到）：
   *   源稿里 WinHunt 写在最后 = 窗口栈最上层 ⇒ 不收起来它会**一直盖在上面**。
   *   症状极具误导性：点页签"没反应"，看到的一直是猎手页
   *   （实测判据：蓝牙页截图 (60,96) 采到的却是猎手页目标名的金色文字）。
   *   目标本身不清（sTgtKind 保留），底部「热点猎手」还能再进去。 */
  if (mWinHuntPtr) mWinHuntPtr->hideWnd();
  sHuntOn = false;

  ZKWindow *wins[4] = {mWinProbeWifiPtr, mWinProbeBtPtr, mWinProbeLanPtr, mWinProbeSniffPtr};
  for (int i = 0; i < 4; ++i) {
    if (!wins[i]) continue;
    if (i == t) wins[i]->showWnd();
    else wins[i]->hideWnd();
  }

  if (t == 1) {
    syncBtPage();
    /* ⚠️ 不要每次进页都 hostScanStart()：本板控制器跑一阵会"哑火"
     *   （advCount 冻住、不再上报广播）——清空后就再也填不回来，用户看到恒空的列表。
     *   只有列表本来就空时才主动扫一次；想刷新点「重新扫描」。
     *   教训同 remoteLogic.cc 的 showPage()。 */
    pg::Bt *bt = btGet();
    if (bt && bt->state() == 2 && bt->scannedCount() == 0) {
      bt->hostScanStart();
      LOGD("probeLogic: 蓝牙列表为空，进页自动扫一次");
    }
    sBtDevCount = -1;
  } else if (t == 2) {
    /* 进页**不自动扫**：一轮局域网扫描 10~25 秒、还会主动连对端
     * （用户可能只是路过看一眼）⇒ 显式点「扫描局域网」。 */
    sLanGen = -1;
    sLanHosts = -1;
    syncLanPage();
  } else if (t == 3) {
    syncSniffPage();
  }
  LOGD("probeLogic: 切到%s页", kTabName[t]);
}

/* ==================== QA 自检通道（免触摸验收） ====================
 * 本板 mt_test 触摸注入时灵时不灵（见 docs/wifi-app.md §3.2），原生控件又走不了
 * dispatchCanvasTouch ⇒ 文件驱动的命令通道是唯一的自动化验收入口。
 * 文件：/tmp/pg_probecmd（**整份内容变化才执行**，每行加序号防去重）。
 *
 *   tab 0..3          切页签（0=WiFi 探测 / 1=蓝牙探测 / 2=局域网设备 / 3=无线嗅探）
 *   scan              触发一次 WiFi 扫描
 *   dump              打印 AP 表（序号/SSID/BSSID/信道/加密/RSSI/风险等级）
 *   sum               打印总览（计数 + 信道建议）
 *   hunt <idx>        锁定 AP 列表第 idx 个 → 进热点猎手页
 *   huntbest          锁"有风险里信号最强的"，没有风险的就锁最强的（不受序号漂移影响）
 *   huntssid <名字>   按 SSID 锁定（**不做 `_`→空格替换**）
 *   bt                启动蓝牙（rtk_init，约 20~40 秒；失败要断电冷启动）
 *   btscan            清空并重新收集 BLE 广播
 *   btdump            打印 BLE 设备表（地址/名称/RSSI/风险等级）
 *   huntbt <idx>      锁定 BLE 列表第 idx 个 → 进热点猎手页
 *   risk [名字]       打印可疑判定结果；不带参数则跑一遍**内置样例表**（验证特征库）
 *   btrisk [名字]     同上，按 BLE 规则判（RSSI 固定 -50）
 *   stop              停止追踪（回 WiFi 页）
 *   beep 0|1          蜂鸣开关
 *   state             打印整体状态（页签/目标/平滑值/段数/蜂鸣/蓝牙）
 *   lan               开始一轮完整局域网扫描（邻居发现 + SSDP + 端口/协议指纹）
 *   lanquick          只做邻居发现 + SSDP（不探端口，快）
 *   lanprobe <ip>     只深探某一台
 *   lanonly 0|1       局域网页「只看风险」筛选（1 = 只显示可疑及以上）
 *   aponly 0|1        WiFi 页「只看风险」筛选
 *   apclear           清掉所有注入的假 AP
 *   apinject <名> [rssi]
 *                      **注入一个假 AP**（QA only）：把"风险置顶"变成可复现判据
 *                      —— 真实环境常常一个可疑 SSID 都没有，排序看不出来
 *   riskrank          打印两个列表的**当前显示顺序**（验证"风险置顶"是否生效）
 *   lanlist           打印局域网设备表
 *   lantest           可疑判定自检（内置样例表 —— **不用真摄像头也能验证判定逻辑**）
 *   sniff on|off      开始/停止无线嗅探（把 wlan1 切成 MONITOR 顺听）
 *   sniff hop on|off  ★ 全信道跳扫开关（**会断开 wlan0**，原网自动存/恢复）
 *   snifflist         打印发射者表 + 顺听到的 SSID（含解出的隐藏网络名）
 *   oui <mac>         查 MAC 厂商（验证 OUI 精选表）
 *   wifi snap         把**当前网络**存成"原网络"快照（切网前的手动存盘）
 *   wifi mode on|off  进/出「目标网模式」（off = 还原配置 + 重读 + 重连）
 *   wifi restore      只还原配置文件并让 wpa_supplicant 重读（不动模式标记）
 *   wifi state        打印快照状态（原网络/psk 长度/是否在目标网模式/控制口）
 *   wpa <命令>        直连 wpa_supplicant 控制口发一条命令（诊断用，如 wpa STATUS）
 *   back              返回主界面
 *   who               诊断"触摸被谁吃了"（屏保/IME/五个窗口 + 嗅探运行态）
 */
char sCmdLast[512] = {0};

void probeCmdDump() {
  LOGD("probeLogic: dump 共 %d 个 AP", (int)sScan.size());
  char why[64], sec[24];
  for (size_t i = 0; i < sScan.size(); ++i) {
    secText(sScan[i].getEncryption(), sec, sizeof(sec));
    int lv = riskOfAp(sScan[i], why, sizeof(why));
    LOGD("probeLogic: AP#%d ssid='%s' bssid=%s ch=%d sec=%s rssi=%d risk=%d%s%s",
         (int)i, sScan[i].getSsid().empty() ? "(隐藏)" : sScan[i].getSsid().c_str(),
         sScan[i].getBssid().c_str(), chanOfFreq(sScan[i].getFreq()), sec,
         sScan[i].getRssi(), lv, lv ? " why=" : "", lv ? why : "");
  }
}

void probeCmdSum() {
  syncWifiSummary();
  int nSus = 0, nWatch = 0;
  char why[64];
  for (size_t i = 0; i < sScan.size(); ++i) {
    int lv = riskOfAp(sScan[i], why, sizeof(why));
    if (lv >= RISK_SUSPECT) ++nSus;
    else if (lv == RISK_WATCH) ++nWatch;
  }
  LOGD("probeLogic: sum total=%d suspect=%d watch=%d ch1=%d ch6=%d ch11=%d", (int)sScan.size(),
       nSus, nWatch, occCount(1), occCount(6), occCount(11));
  LOGD("probeLogic: sum 建议='%s'", sLastSum3);
}

void probeCmdBleDump() {
  pg::Bt *bt = btGet();
  if (!bt) return;
  int n = bt->scannedCount();
  LOGD("probeLogic: btdump 共 %d 个设备（广播累计 %d）", n, bt->advCount());
  char why[64];
  for (int i = 0; i < n; ++i) {
    int lv = riskOfBle(bt->scannedName(i), bt->scannedRssi(i), why, sizeof(why));
    LOGD("probeLogic: BLE#%d addr=%s name='%s' rssi=%d risk=%d%s%s", i, bt->scannedAddr(i),
         bt->scannedName(i), bt->scannedRssi(i), lv, lv ? " why=" : "", lv ? why : "");
  }
}

/* 可疑判定自检：**不用真摄像头也能验证特征库**。
 * 为什么必须有它：办公室里的 AP 名字全是 ChinaNet/TP-LINK 这种，risk 恒为 0 ——
 * 只看真机 dump 根本分不清"判定对了"还是"判定根本没跑"。 */
void probeCmdRisk(const char *name) {
  char why[64];
  if (name && name[0]) {
    WifiInfo ap;
    ap.setSsid(name);
    int lv = riskOfAp(ap, why, sizeof(why));
    LOGD("probeLogic: risk '%s' -> %d %s", name, lv, why);
    return;
  }
  /* 内置样例表：覆盖三档 + 不应误报的常见名字（**假阳性也要能看出来**） */
  static const char *kSamples[] = {
      "IPCamera-4F2A",  "ESP_1A2B3C",  "V380-1234567",  "HIDDEN-CAM",
      "Wireless Camera", "GC-ABCDEF",  "看家摄像头",     "监控-01",
      "TPLink_zkswe",   "ChinaNet-bsyX", "aWiFi",      "zkswe-soft_5G",
      "DIRECT-01-HP Laser 323sdnw", "ADMIN 9959",      "钱多多",
      /* ★ 泛词回归样例（2026-09-15 加）：这些都**不该**是 2 级"可疑"，
       *   只能是 1 级"关注"或 0 级 —— 否则智能家居会淹没真正的摄像头。
       *   （真机血案：BE88U_IoT 因含 "iot" 被判"疑似监控设备"。） */
      "BE88U_IoT",      "SmartHome-2G",  "Wireless-Printer", "MiniPC-5G",
  };
  LOGD("probeLogic: risk ---- 样例表（0普通 1关注 2可疑 3高危）----");
  for (size_t i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); ++i) {
    WifiInfo ap;
    ap.setSsid(kSamples[i]);
    int lv = riskOfAp(ap, why, sizeof(why));
    LOGD("probeLogic: risk [%d] '%s' -> %d %s", (int)i, kSamples[i], lv, why);
  }
  /* 隐藏 SSID（空名字）单独测：要判成"关注" */
  {
    WifiInfo ap;
    int lv = riskOfAp(ap, why, sizeof(why));
    LOGD("probeLogic: risk [隐藏] '' -> %d %s", lv, why);
  }
}

void probeCmdBtRisk(const char *name) {
  char why[64];
  if (name && name[0]) {
    int lv = riskOfBle(name, -50, why, sizeof(why));
    LOGD("probeLogic: btrisk '%s' -> %d %s", name, lv, why);
    return;
  }
  static const char *kSamples[] = {"", "IPC-1234", "V380", "SmartCam-A1",
                                   "Mi Band 8", "PocketGame-RC"};
  LOGD("probeLogic: btrisk ---- 样例表（RSSI 固定 -50）----");
  for (size_t i = 0; i < sizeof(kSamples) / sizeof(kSamples[0]); ++i) {
    int lv = riskOfBle(kSamples[i], -50, why, sizeof(why));
    LOGD("probeLogic: btrisk [%d] '%s' -> %d %s", (int)i,
         kSamples[i][0] ? kSamples[i] : "(匿名)", lv, why);
  }
}

void probeCmdState() {
  LOGD("probeLogic: state tab=%d wm=%d enable=%d ap=%d scanning=%d", sTab, sWm ? 1 : 0,
       sWm ? (sWm->isWifiEnable() ? 1 : 0) : -1, (int)sScan.size(),
       sScanWaitUntil ? 1 : 0);
  LOGD("probeLogic: state hunt kind=%d on=%d key='%s' label='%s' rssi=%d avg=%.1f "
       "peak=%d samples=%d miss=%d bars=%d beep=%d",
       sTgtKind, sHuntOn ? 1 : 0, sTgtKey, sTgtLabel, sTgtRssi, sTgtAvg, sTgtPeak,
       sTgtSamples, sTgtMiss, barsOf((int)sTgtAvg), sBeep ? 1 : 0);
  pg::Bt *bt = btGet();
  if (bt) {
    LOGD("probeLogic: state bt state=%d(%s) adv=%d dev=%d err=%d", bt->state(),
         bt->stateText(), bt->advCount(), bt->scannedCount(), bt->errCode());
  }
}

/* ---------- 局域网页 QA ---------- */
void probeCmdLanList() {
  pg::Lan *l = lanGet();
  const int n = l->hostCount();
  LOGD("probeLogic: lanlist 共 %d 台 run=%d phase=%d prog=%d%% subnet=%s", n,
       l->running() ? 1 : 0, l->phase(), l->progress(), l->subnetText());
  for (int i = 0; i < n; ++i) {
    pg::LanHost h;
    if (!l->hostCopy(i, &h)) continue;
    LOGD("probeLogic: LAN#%d ip=%s mac=%s vendor='%s' kind='%s' ports='%s' level=%d why='%s' "
         "note='%s' ssdp='%s'",
         i, h.ip, h.mac[0] ? h.mac : "-", h.vendor[0] ? h.vendor : "-", h.kind,
         h.ports[0] ? h.ports : "-", h.level, h.why[0] ? h.why : "-",
         h.note[0] ? h.note : "-", h.ssdp[0] ? h.ssdp : "-");
  }
}

/* 判定自检：**不用真摄像头也能验证特征库**。
 * 为什么必须有它：办公室的网段里就是几台手机/路由器，level 恒为 0 —— 只看真机
 * lanlist 根本分不清"判定对了"还是"判定根本没跑"（同 risk/btrisk 的思路）。 */
void lanTestOne(const char *label, const char *mac, const char *ports, const char *note,
                const char *ssdp) {
  pg::LanHost h;
  memset(&h, 0, sizeof(h));
  snprintf(h.mac, sizeof(h.mac), "%s", mac ? mac : "");
  snprintf(h.ports, sizeof(h.ports), "%s", ports ? ports : "");
  snprintf(h.note, sizeof(h.note), "%s", note ? note : "");
  snprintf(h.ssdp, sizeof(h.ssdp), "%s", ssdp ? ssdp : "");
  snprintf(h.vendor, sizeof(h.vendor), "%s", pg::pgOuiLookup(h.mac));
  char why[44] = {0};
  const int lv = pg::Lan::classify(h, why, sizeof(why));
  LOGD("probeLogic: lantest [%s] level=%d why='%s' (oui='%s' ports='%s' note='%s' ssdp='%s')",
       label, lv, why, h.vendor[0] ? h.vendor : "-", h.ports[0] ? h.ports : "-",
       h.note[0] ? h.note : "-", h.ssdp[0] ? h.ssdp : "-");
}

void probeCmdLanTest() {
  LOGD("probeLogic: lantest ---- 应命中（0普通 1关注 2可疑 3高危）----");
  lanTestOne("海康-未鉴权视频流", "44:19:b6:11:22:33", "80 554",
             "RTSP 视频流可直读(H264) · 80:GoAhead-Webs", "");
  lanTestOne("大华-RTSP需鉴权", "3c:ef:8c:11:22:33", "80 554", "RTSP 401 需鉴权 · 80:webs", "");
  lanTestOne("雄迈私有端口34567", "aa:bb:cc:11:22:33", "80 34567", "", "");
  lanTestOne("大华私有端口37777", "aa:bb:cc:11:22:34", "37777", "", "");
  lanTestOne("海康SDK端口8000", "aa:bb:cc:11:22:35", "8000", "", "");
  lanTestOne("IPC-web栈GoAhead", "24:0a:c4:11:22:33", "80", "80:GoAhead-Webs", "");
  lanTestOne("DVR-web栈Boa", "aa:bb:cc:11:22:36", "80", "80:Boa/0.94.14rc21", "");
  lanTestOne("SSDP报IPCAM", "aa:bb:cc:11:22:37", "", "", "Linux/3.10 UPnP/1.0 IPCAM/1.0");
  lanTestOne("安防厂商OUI", "44:19:b6:aa:bb:cc", "", "", "");
  lanTestOne("备用RTSP口8554", "aa:bb:cc:11:22:38", "8554", "", "");
  lanTestOne("未知厂商+怪端口", "aa:bb:cc:11:22:39", "9999", "", "");
  LOGD("probeLogic: lantest ---- 以下必须**不误报**（level 应为 0）----");
  lanTestOne("普通路由器TP-LINK", "50:c7:bf:11:22:33", "80", "80:nginx", "");
  lanTestOne("普通电脑Intel", "00:1b:21:11:22:33", "22 80 445", "80:nginx", "");
  lanTestOne("手机(随机MAC)", "32:3d:56:24:5e:cb", "", "", "");
  lanTestOne("ESP模组(智能插座)", "24:0a:c4:aa:bb:cc", "", "", "Linux/3.14 UPnP/1.0");
}

/* 注入一个假 AP 到扫描结果里 —— **只给 QA 用**。
 * 目的：把"风险置顶"变成可复现判据（真实环境常常一个可疑 SSID 都没有），
 * 顺带验证筛选 / 点名 / 色条整条链路。
 * 副作用：注入项会留在 sScan 里，直到下一次真实扫描把它覆盖掉（无害）。 */
void probeCmdApInject(const char *name, int rssi) {
  if (!name || !name[0]) {
    LOGD("probeLogic: apinject 用法 apinject <名字> [rssi]");
    return;
  }
  WifiInfo ap;
  ap.setSsid(name);
  ap.setRssi(rssi);
  ap.setFreq(2437);                        // 2.4G CH6，方便在列表里认出来
  ap.setBssid("aa:bb:cc:dd:ee:ff");
  sInjectAp.push_back(ap);                  // 钉住（否则下一次扫描就把它洗掉了）
  sScan.push_back(ap);
  std::sort(sScan.begin(), sScan.end(), apRiskDesc);   // ★ 走真实排序路径
  ++sScanSeq;
  char why[64];
  const int lv = riskOfAp(ap, why, sizeof(why));
  LOGD("probeLogic: apinject '%s' rssi=%d -> level=%d why='%s'", name, rssi, lv, why);
  /* ★ 立刻回读排序后的前 3 名：**一条命令就给出"风险是否置顶"的判据**。
   *   为什么不靠后面的 riskrank：scan 的结果回调随时可能把注入项覆盖掉
   *   （`handleWifiScanResult` 是整体赋值 `sScan = *infos`），实测踩到过
   *   —— 那样看到的就是"注入了却没置顶"的假象。 */
  char why2[64];
  for (int i = 0; i < 3 && i < (int)sScan.size(); ++i) {
    const int l2 = riskOfAp(sScan[i], why2, sizeof(why2));
    LOGD("probeLogic: apinject 排序后 #%d ssid='%s' rssi=%d level=%d why='%s'", i,
         sScan[i].getSsid().c_str(), sScan[i].getRssi(), l2, why2);
  }
  if (mListProbeApPtr) mListProbeApPtr->refreshListView();
  syncWifiSummary();
}

void probeCmdOui(const char *mac) {
  if (!mac || !mac[0]) {
    LOGD("probeLogic: oui 用法 oui <mac>；样例：");
    static const char *kM[] = {"44:19:b6:11:22:33", "3c:ef:8c:11:22:33", "24:0a:c4:11:22:33",
                               "50:c7:bf:11:22:33", "00:1b:21:11:22:33",
                               "32:3d:56:24:5e:cb", "aa:bb:cc:11:22:33"};
    for (size_t i = 0; i < sizeof(kM) / sizeof(kM[0]); ++i)
      LOGD("probeLogic: oui %s -> '%s' cam=%d random=%d", kM[i], pg::pgOuiLookup(kM[i]),
           pg::pgOuiIsCam(kM[i]) ? 1 : 0, pg::pgMacIsRandom(kM[i]) ? 1 : 0);
    return;
  }
  LOGD("probeLogic: oui %s -> '%s' cam=%d random=%d", mac, pg::pgOuiLookup(mac),
       pg::pgOuiIsCam(mac) ? 1 : 0, pg::pgMacIsRandom(mac) ? 1 : 0);
}

bool huntLockAp(int idx) {
  if (idx < 0 || idx >= (int)sScan.size()) {
    LOGD("probeLogic: hunt %d 越界（共 %d 个）", idx, (int)sScan.size());
    return false;
  }
  const WifiInfo &ap = sScan[idx];
  sTgtKind = TG_AP;
  /* 目标键优先用 BSSID（同 SSID 可能有多个 AP，Mesh / 同名扩展器）；空则退化为 "#SSID" */
  if (!ap.getBssid().empty()) snprintf(sTgtKey, sizeof(sTgtKey), "%s", ap.getBssid().c_str());
  else snprintf(sTgtKey, sizeof(sTgtKey), "#%.*s", 18, ap.getSsid().c_str());
  snprintf(sTgtLabel, sizeof(sTgtLabel), "%s",
           ap.getSsid().empty() ? "(隐藏网络)" : ap.getSsid().c_str());
  char sec[24];
  secText(ap.getEncryption(), sec, sizeof(sec));
  snprintf(sTgtSub, sizeof(sTgtSub), "%s · CH%d · %s", ap.getFreq() >= 5000 ? "5G" : "2.4G",
           chanOfFreq(ap.getFreq()), sec);
  sTgtRssi = ap.getRssi();
  huntResetBaseline();
  sHuntOn = true;
  if (mWinHuntPtr) mWinHuntPtr->showWnd();
  syncHuntPage();
  LOGD("probeLogic: 锁定 AP '%s' key=%s rssi=%d -> 进热点猎手", sTgtLabel, sTgtKey, sTgtRssi);
  return true;
}

bool huntLockBle(int idx) {
  pg::Bt *bt = btGet();
  if (!bt || idx < 0 || idx >= bt->scannedCount()) {
    LOGD("probeLogic: huntbt %d 越界（共 %d）", idx, bt ? bt->scannedCount() : -1);
    return false;
  }
  sTgtKind = TG_BLE;
  snprintf(sTgtKey, sizeof(sTgtKey), "%s", bt->scannedAddr(idx));
  snprintf(sTgtLabel, sizeof(sTgtLabel), "%s",
           bt->scannedName(idx)[0] ? bt->scannedName(idx) : "(匿名设备)");
  snprintf(sTgtSub, sizeof(sTgtSub), "蓝牙 BLE · %s", bt->scannedAddr(idx));
  sTgtRssi = bt->scannedRssi(idx);
  huntResetBaseline();
  sHuntOn = true;
  if (mWinHuntPtr) mWinHuntPtr->showWnd();
  syncHuntPage();
  LOGD("probeLogic: 锁定 BLE '%s' addr=%s rssi=%d -> 进热点猎手", sTgtLabel, sTgtKey,
       sTgtRssi);
  return true;
}

void probeCmdRun(const char *cmd) {
  if (!cmd || !cmd[0]) return;
  if (strncmp(cmd, "tab ", 4) == 0) {
    showTab(atoi(cmd + 4));
    syncTabs();
  } else if (strncmp(cmd, "scan", 4) == 0) {
    LOGD("probeLogic: cmd scan");
    startScan();
  } else if (strncmp(cmd, "dump", 4) == 0) {
    probeCmdDump();
  } else if (strncmp(cmd, "sum", 3) == 0) {
    probeCmdSum();
  } else if (strncmp(cmd, "lanonly", 7) == 0) {
    sLanOnly = (cmd[7] == ' ' && cmd[8] == '1');
    LOGD("probeLogic: cmd lanonly -> %d", sLanOnly ? 1 : 0);
    if (mListProbeLanPtr) mListProbeLanPtr->refreshListView();
    syncLanPage();
  } else if (strncmp(cmd, "apclear", 7) == 0) {
    const size_t n = sInjectAp.size();
    sInjectAp.clear();
    LOGD("probeLogic: cmd apclear -> 清掉 %u 个注入 AP（下次扫描生效）", (unsigned)n);
  } else if (strncmp(cmd, "apinject", 8) == 0) {
    /* ⚠️ 必须**在空格处截断**：直接把整串传下去的话，名字会变成
     *    "IPCamera-4F2A -85"（把 rssi 也吞进去了）—— 实测踩到。 */
    char nm[64] = {0};
    int rssi = -70;
    if (cmd[8] == ' ') {
      const char *a = cmd + 9;
      const char *sp = strchr(a, ' ');
      if (sp) {
        size_t len = (size_t)(sp - a);
        if (len >= sizeof(nm)) len = sizeof(nm) - 1;
        memcpy(nm, a, len);
        nm[len] = 0;
        rssi = atoi(sp + 1);
      } else {
        snprintf(nm, sizeof(nm), "%s", a);
      }
    }
    probeCmdApInject(nm, rssi);
  } else if (strncmp(cmd, "aponly", 6) == 0) {
    sApOnly = (cmd[6] == ' ' && cmd[7] == '1');
    LOGD("probeLogic: cmd aponly -> %d", sApOnly ? 1 : 0);
    if (mListProbeApPtr) mListProbeApPtr->refreshListView();
    syncWifiSummary();
  } else if (strncmp(cmd, "riskrank", 8) == 0) {
    /* 验收用：打印两个列表的**显示顺序**（前 8 条），一眼看出风险有没有置顶 */
    rebuildLanView();
    LOGD("probeLogic: riskrank ---- 局域网（only=%d 视图 %d/%d 台）----", sLanOnly ? 1 : 0,
         sLanViewN, lanGet()->hostCount());
    for (int i = 0; i < sLanViewN && i < 8; ++i) {
      pg::LanHost h;
      if (!lanGet()->hostCopy(sLanView[i], &h)) continue;
      LOGD("probeLogic: riskrank LAN#%d ip=%s kind=%s level=%d why='%s'", i, h.ip, h.kind,
           h.level, h.why[0] ? h.why : "-");
    }
    rebuildApView();
    LOGD("probeLogic: riskrank ---- AP（only=%d 视图 %d/%d 个）----", sApOnly ? 1 : 0, sApViewN,
         (int)sScan.size());
    char why[64];
    for (int i = 0; i < sApViewN && i < 8; ++i) {
      const WifiInfo &ap = sScan[sApView[i]];
      const int lv = riskOfAp(ap, why, sizeof(why));
      LOGD("probeLogic: riskrank AP#%d ssid='%s' rssi=%d level=%d why='%s'", i,
           ap.getSsid().c_str(), ap.getRssi(), lv, why);
    }
  } else if (strncmp(cmd, "lantest", 7) == 0) {
    probeCmdLanTest();
  } else if (strncmp(cmd, "lanlist", 7) == 0) {
    probeCmdLanList();
  } else if (strncmp(cmd, "lanprobe ", 9) == 0) {
    LOGD("probeLogic: cmd lanprobe %s", cmd + 9);
    lanGet()->startProbeOne(cmd + 9);
  } else if (strncmp(cmd, "lanquick", 8) == 0) {
    LOGD("probeLogic: cmd lanquick（只找邻居 + SSDP）");
    lanGet()->startScan(false, true);
  } else if (strncmp(cmd, "lan", 3) == 0) {
    LOGD("probeLogic: cmd lan（完整扫描：邻居 + SSDP + 端口/指纹）");
    lanGet()->startScan(true, true);
  } else if (strncmp(cmd, "snifflist", 9) == 0) {
    snGet()->logDump();
  } else if (strncmp(cmd, "sniff hop", 9) == 0) {
    /* 全信道模式（会断网！走应用层的 enter/leave，不是直接 PgSniff::setHop） */
    if (strstr(cmd, "off")) {
      LOGD("probeLogic: cmd sniff hop off");
      leaveFullHop("QA");
    } else {
      LOGD("probeLogic: cmd sniff hop on");
      if (!snGet()->active()) {
        snGet()->start();
        syncSniffPage();
      }
      enterFullHop();
    }
    showTab(3);
    syncTabs();
    syncSniffPage();
  } else if (strncmp(cmd, "sniff ", 6) == 0) {
    const char *a = cmd + 6;
    if (strncmp(a, "on", 2) == 0) {
      LOGD("probeLogic: cmd sniff on");
      bool ok = snGet()->start();
      LOGD("probeLogic: sniff start -> %d (%s)", ok ? 1 : 0, snGet()->stateText());
    } else {
      LOGD("probeLogic: cmd sniff off");
      snGet()->stop();
    }
    showTab(3);
    syncTabs();
    syncSniffPage();
  } else if (strncmp(cmd, "oui", 3) == 0) {
    probeCmdOui(cmd[3] == ' ' ? cmd + 4 : 0);
  } else if (strncmp(cmd, "wpa ", 4) == 0) {
    char resp[600];
    int r = wcGet()->wpaCmd(cmd + 4, resp, sizeof(resp));
    LOGD("probeLogic: wpa '%s' -> %d / '%s'", cmd + 4, r, resp);
  } else if (strncmp(cmd, "wifi ", 5) == 0) {
    const char *a = cmd + 5;
    pg::WifiCfg *wc = wcGet();
    if (strncmp(a, "state", 5) == 0) {
      const std::string cur = connectedSsid();
      LOGD("probeLogic: wifi state home='%s' hasHome=%d targetMode=%d pskLen=%d cur='%s' "
           "ctrl=%s conf=%s",
           wc->homeSsid(), wc->hasHome() ? 1 : 0, wc->targetMode() ? 1 : 0, wc->homePskLen(),
           cur.c_str(), wc->ctrlPath(), wc->confPath());
      long sz = -1;
      FILE *f = fopen(wc->homeConfPath(), "rb");
      if (f) {
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fclose(f);
      }
      LOGD("probeLogic: wifi state 快照文件 %s = %ld 字节", wc->homeConfPath(), sz);
    } else if (strncmp(a, "snap", 4) == 0) {
      const std::string cur = connectedSsid();
      bool ok = wc->keepHome(cur.c_str());
      LOGD("probeLogic: wifi snap -> %d 原网络='%s' cur='%s'", ok ? 1 : 0, wc->homeSsid(),
           cur.c_str());
    } else if (strncmp(a, "mode on", 7) == 0) {
      const std::string cur = connectedSsid();
      bool ok = wc->enterTargetMode(cur.c_str());
      sModeSawOther = false;
      LOGD("probeLogic: wifi mode on -> %d 原网络='%s'", ok ? 1 : 0, wc->homeSsid());
      syncLanPage();
    } else if (strncmp(a, "mode off", 8) == 0) {
      char ssid[64] = {0}, psk[96] = {0};
      bool ok = wc->leaveTargetMode(ssid, sizeof(ssid), psk, sizeof(psk));
      LOGD("probeLogic: wifi mode off -> %d 原网络='%s'", ok ? 1 : 0, ssid);
      if (ok && ssid[0]) {
        snprintf(sReconnectSsid, sizeof(sReconnectSsid), "%s", ssid);
        snprintf(sReconnectPsk, sizeof(sReconnectPsk), "%s", psk);
        sReconnectStage = 1;
        sReconnectAt = nowMs();
      }
      sModeSawOther = false;
      syncLanPage();
    } else if (strncmp(a, "restore", 7) == 0) {
      bool ok = wc->restoreConf();
      char resp[256] = {0};
      int r1 = wc->wpaCmd("RECONFIGURE", resp, sizeof(resp));
      LOGD("probeLogic: wifi restore -> conf=%d RECONFIGURE=%d '%s'", ok ? 1 : 0, r1, resp);
    } else {
      LOGD("probeLogic: wifi 子命令未识别 '%s'", a);
    }
  } else if (strncmp(cmd, "huntbest", 8) == 0) {
    /* 确定性选择：优先"有风险的里面信号最强的"，否则选信号最强的。
     * 为什么不用序号：扫描会把列表刷掉、序号漂移（踩过，见 wifi-app.md §4-5）。 */
    int pick = -1, bestRssi = -200;
    char why[64];
    for (size_t i = 0; i < sScan.size(); ++i) {
      if (riskOfAp(sScan[i], why, sizeof(why)) < RISK_SUSPECT) continue;
      if (sScan[i].getRssi() > bestRssi) {
        bestRssi = sScan[i].getRssi();
        pick = (int)i;
      }
    }
    if (pick < 0 && !sScan.empty()) pick = 0;
    if (pick >= 0) huntLockAp(pick);
    else LOGD("probeLogic: huntbest 没有可锁定的 AP");
  } else if (strncmp(cmd, "huntssid ", 9) == 0) {
    const char *want = cmd + 9;
    int pick = -1;
    for (size_t i = 0; i < sScan.size(); ++i) {
      if (strcmp(sScan[i].getSsid().c_str(), want) == 0) {
        pick = (int)i;
        break;
      }
    }
    if (pick >= 0) huntLockAp(pick);
    else LOGD("probeLogic: huntssid '%s' 没找到", want);
  } else if (strncmp(cmd, "hunt ", 5) == 0) {
    huntLockAp(atoi(cmd + 5));
  } else if (strncmp(cmd, "btscan", 6) == 0) {
    pg::Bt *bt = btGet();
    if (bt) {
      bt->hostScanStart();
      sBtDevCount = -1;
      LOGD("probeLogic: cmd btscan（广播累计 %d）", bt->advCount());
    }
  } else if (strncmp(cmd, "btdump", 6) == 0) {
    probeCmdBleDump();
  } else if (strncmp(cmd, "huntbt ", 7) == 0) {
    huntLockBle(atoi(cmd + 7));
  } else if (strncmp(cmd, "btrisk", 6) == 0) {
    probeCmdBtRisk(cmd[6] == ' ' ? cmd + 7 : 0);
  } else if (strncmp(cmd, "risk", 4) == 0) {
    probeCmdRisk(cmd[4] == ' ' ? cmd + 5 : 0);
  } else if (strncmp(cmd, "bt", 2) == 0) {
    pg::Bt *bt = btGet();
    LOGD("probeLogic: cmd bt（启动蓝牙，约 20~40 秒）");
    if (bt) bt->start();
  } else if (strncmp(cmd, "stop", 4) == 0) {
    LOGD("probeLogic: cmd stop（停止追踪）");
    sHuntOn = false;
    sTgtKind = TG_NONE;
    if (mWinHuntPtr) mWinHuntPtr->hideWnd();
    showTab(0);
    syncTabs();
  } else if (strncmp(cmd, "beep ", 5) == 0) {
    sBeep = (atoi(cmd + 5) != 0);
    LOGD("probeLogic: cmd beep -> %d", sBeep ? 1 : 0);
    sBgBeep = -1;
    syncTabs();
    syncHuntPage();
  } else if (strncmp(cmd, "state", 5) == 0) {
    probeCmdState();
  } else if (strncmp(cmd, "back", 4) == 0) {
    LOGD("probeLogic: cmd back -> 返回主界面");
    EASYUICONTEXT->closeActivity("probeActivity");
  } else if (strncmp(cmd, "who", 3) == 0) {
    LOGD("probeLogic: who saverEnable=%d saverOn=%d ime=%d hunt=%d wifiWnd=%d btWnd=%d "
         "lanWnd=%d snWnd=%d huntWnd=%d sniffActive=%d lanRun=%d",
         EASYUICONTEXT->isScreensaverEnable() ? 1 : 0,
         EASYUICONTEXT->isScreensaverOn() ? 1 : 0, EASYUICONTEXT->isIMEShow() ? 1 : 0,
         sHuntOn ? 1 : 0, (mWinProbeWifiPtr && mWinProbeWifiPtr->isWndShow()) ? 1 : 0,
         (mWinProbeBtPtr && mWinProbeBtPtr->isWndShow()) ? 1 : 0,
         (mWinProbeLanPtr && mWinProbeLanPtr->isWndShow()) ? 1 : 0,
         (mWinProbeSniffPtr && mWinProbeSniffPtr->isWndShow()) ? 1 : 0,
         (mWinHuntPtr && mWinHuntPtr->isWndShow()) ? 1 : 0, snGet()->active() ? 1 : 0,
         lanGet()->running() ? 1 : 0);
  } else {
    LOGD("probeLogic: cmd 未识别 '%s'", cmd);
  }
}

void pollProbeCmd() {
  FILE *f = fopen("/tmp/pg_probecmd", "r");
  if (!f) return;
  char all[512] = {0};
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (!n) continue;
    /* ★ 剥掉行尾 `#注释`（与 mainLogic 的 /tmp/pg_autostart 同一约定）：
     *   QA 文件要求"整份内容变了才执行"，所以每次得换个后缀 —— 写 `scan #7`
     *   既换了内容又不会把 `#7` 当成参数。
     *   ⚠️ 必须在这里统一剥，不能让参数去 parse —— 踩过：
     *      `risk 1`（本想用 1 当序号）被判成"判定名为 1 的 SSID"。 */
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int m = (int)strlen(line);
    while (m > 0 && (line[m - 1] == ' ' || line[m - 1] == '\t')) line[--m] = 0;
    if (!m) continue;
    if (strlen(all) + (size_t)n + 2 < sizeof(all)) {
      strcat(all, line);
      strcat(all, "\n");
    }
  }
  fclose(f);
  if (!all[0] || strcmp(all, sCmdLast) == 0) return;  // 内容没变 → 不重复执行
  strncpy(sCmdLast, all, sizeof(sCmdLast) - 1);
  LOGD("probeLogic: ---- 执行 QA 命令块 ----");
  char *p = all;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (*p) probeCmdRun(p);
    if (!nl) break;
    p = nl + 1;
  }
}

/* ==================== 物理按键（本页前台时） ====================
 * 本板实体键 103=音量- 105=音量+ 108=暂停。本页在前台时主 KeyRouter 让路，
 * 所以这里自己注册监听器：
 *   音量键 → pg::volumeStepGlobal（转给 mainLogic 那份唯一的音频实例）
 *   任意键长按 ≥700ms → 返回主界面（本板 gpio-keys **无 autorepeat**，只能按时长判）
 */
class ProbeKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键，不顺手调音量）。
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
        LOGD("probeLogic: 长按 %dms -> 返回主界面", held);
        EASYUICONTEXT->closeActivity("probeActivity");
        return true;
      }
      if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
        int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
        if (pct >= 0) {
          LOGD("probeLogic: 音量%s -> %d%%（OSD 由全局状态栏显示）",
               ke.mKeyCode == 105 ? "+" : "-", pct);
        }
      }
      return true;  // 其余按键吞掉，避免落到框架默认行为
    }
    return false;
  }

  /* ★ 长按达标**立刻**返回，不等按键抬起（本板无 autorepeat，只在 UP 里判会让手感
   *   变成"按了不动、松手才跳"）。由页面定时器轮询"按下且已超时"。 */
  static bool longPressReady() {
    if (sDownCode < 0 || sLPFired) return false;
    if (nowMs() - sDownMs < LONG_PRESS_MS) return false;
    sLPFired = true;
    return true;
  }

 private:
  static int sDownCode;
  static long sDownMs;
  static bool sLPFired;
};
int ProbeKeys::sDownCode = -1;
long ProbeKeys::sDownMs = 0;
bool ProbeKeys::sLPFired = false;
ProbeKeys sKeyRouter;

void tickKeyLongPress() {
  if (ProbeKeys::longPressReady()) {
    LOGD("probeLogic: 长按达标（不等抬起）-> 返回主界面");
    EASYUICONTEXT->closeActivity("probeActivity");
  }
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_UI, UI_MS},      // 常规刷新
    {TIMER_HUNT, HUNT_MS},  // 追踪页重扫
    {TIMER_KEY, KEY_MS},    // 按键长按轮询
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeyRouter);
#endif
  LOGD("probeLogic: init（信号探针独立应用）");
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("probeLogic: onUI_show");
  /* 工作界面禁屏保（EasyUIContext.h:159 "本页自己管"）。
   * ⚠️ 必须在 onUI_show 关、onUI_quit 恢复 —— **不要在 onUI_hide 恢复**：
   *    键盘（IME SysApp）弹出时本页会 hide，一恢复屏保就又回来了（踩过）。
   * 血案：屏保是整屏 SysApp，30 秒盖上来后第一下触摸只能"唤醒"，看着就像"点了没反应"。
   * 本页恰恰是"交互慢"的页（扫描 + 追踪）。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
#ifdef FUN_BUILD
  if (!sWm) {
    NetManager *nm = NETMANAGER;
    sWm = nm ? nm->getWifiManager() : 0;
  }
  if (sWm && !sListenerAdded) {
    sWm->addWifiListener(&sEvents);
    sListenerAdded = true;
  }
  /* 进页把三页的显示态收敛一遍（防上次退出时停在别的页签） */
  showTab(sTab);
  sLastSum1[0] = sLastSum2[0] = sLastSum3[0] = 0;
  sLastHuntTgt[0] = sLastHuntDbm[0] = 0;
  sLastLan1[0] = sLastLan2[0] = sLastLan3[0] = 0;
  sLastSnState[0] = sLastSnChan[0] = sLastSnInfo[0] = sLastSnTip[0] = 0;
  sLastSnHopLbl[0] = 0;
  sBgSnHop = -1;
  /* ★ 自愈：上次异常退出（崩溃 / kill -9）可能把 wlan0 留在 down 状态
   *   —— 不检查的话用户进来就是"没网"，还会以为是这次改坏的。 */
  if (!sHopMode && !wcGet()->staIfaceUp()) {
    LOGD("probeLogic: 发现 wlan0 是 down（上次没还原干净）-> 自动 up 回来");
    wcGet()->setStaIfaceUp(true);
  }
  sBgBeep = -1;
  sLanGen = -1;
  sLanHosts = -1;
  sBgLanMode = -1;
  /* 筛选条的缓存也要清（否则进页时按钮文案/配色可能还是上次的） */
  sBgLanOnly = -1;
  sBgApOnly = -1;
  sLastLanOnlyBtn[0] = 0;
  sLastApOnlyBtn[0] = 0;
  sLanViewStamp = -1;
  sApViewStamp = -1;
  sSnTick = 0;
  syncTabs();
  syncBtPage();
  syncHuntPage();
  syncLanPage();
  syncSniffPage();
  if (sScan.empty()) startScan();  // 首次进页自动扫一次
#endif
}

static void onUI_quit() {
  LOGD("probeLogic: onUI_quit");
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
  /* ⚠️ 蓝牙**不停**：rtk_init 只能成功一次（失败要断电冷启动，见 docs/bt-hid-selftest.md §5），
   *    而且它和蓝牙遥控应用共用同一个 pg::Bt 单例 —— 这里 stop 会把遥控页一起搞坏。 */
  /* ⚠️⚠️ 退出应用**必须**把嗅探停掉并把 wlan1 切回 STATION：
   *    嗅探改的是 wlan1 的**全局**接口类型（MONITOR），不退的话
   *    ① 接口一直留在监听态（将来谁用 wlan1 都会莫名其妙失败）；
   *    ② 抓包线程还在跑，白烧 CPU。
   *    局域网扫描也一并停（worker 挂在那儿没有意义）。 */
  /* ★★ 必须在停嗅探**之前**退出全信道模式：
   *    setHop(false) 要求 monitor 还在跑；更重要是 leaveFullHop 会把 wlan0 up 回来
   *    并还原/重连原网络 —— 漏了这一步用户出去就是"WiFi 坏了"。 */
  leaveFullHop("退出应用");
  if (sSn) sSn->stop();
  if (sLan) sLan->stop();
  sWm = 0;
  sHuntOn = false;
  sTgtKind = TG_NONE;
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) {
  if (id == TIMER_KEY) {
    tickKeyLongPress();
    return true;
  }
  if (id == TIMER_HUNT) {
    /* 追踪页：定时重扫（只在追踪 AP 时扫，避免空转把射频占满）。
     * ⚠️ 单次扫描 1~3 秒，比这个周期还长 ⇒ 用 sScanWaitUntil 做闸门，
     *    上一轮没回来就跳过这一拍（堆扫描请求会让 wpa_supplicant 越来越慢）。 */
    if (sHuntOn && sTgtKind == TG_AP && !sScanWaitUntil && sWm && sWm->isWifiEnable()) {
      sScanWaitUntil = nowMs() + SCAN_WAIT_MS;
      sWm->scan();
    }
    return true;
  }
  if (id != TIMER_UI) return true;

#ifdef FUN_BUILD
  /* ★ 闹钟响铃期间**让位**：本页是全屏独立页，留着就看不到主界面的"闹钟提醒页"。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("probeLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    EASYUICONTEXT->closeActivity("probeActivity");
    return false;
  }
  if (!mActivityPtr) return true;

  pollProbeCmd();  // QA 自检通道：/tmp/pg_probecmd

  if (sScanWaitUntil && nowMs() > sScanWaitUntil) sScanWaitUntil = 0;

  if (sScanDirty) {
    sScanDirty = false;
    if (mListProbeApPtr) mListProbeApPtr->refreshListView();
  }
  tickReconnect();   // 还原原网络后的重连兜底（与页签无关，一直跑）
  syncWifiSummary();
  if (sTab == 1) syncBtPage();
  if (sTab == 2) syncLanPage();
  if (sTab == 3) {
    syncSniffPage();
    /* 嗅探表是**实时变化**的（帧数一直在涨）⇒ 固定节拍刷列表，但别太勤
     * （刷太勤会跟用户滚动抢焦点）：400ms × 3 ≈ 1.2 秒一次。 */
    if (++sSnTick % 3 == 0 && mListProbeSnPtr) mListProbeSnPtr->refreshListView();
  }

  if (sHuntOn) {
    huntTick();
    syncHuntPage();
    sBeepTick++;
    huntBeepTick(barsOf((int)sTgtAvg));
  }
#endif
  return true;
}

/**
 * ⚠️⚠️ 触摸回调里**不给任何控件"按坐标吞触摸"**（血案见 docs/wifi-app.md §9.7：
 *    WiFi 页用坐标白名单挡触摸，结果把整屏最上层的键盘全废了 —— 症状是
 *    "界面上什么都正常、就是点不动、日志一条没有"）。
 *    本页没有编辑框、没有需要"挡穿透"的场景 ⇒ 直接全放行。
 */
static bool onprobeActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;
}

/* ---------- WiFi 探测页 ---------- */

static bool onButtonClick_BtnWifiBack(ZKButton *pButton) {
  (void)pButton;
  EASYUICONTEXT->closeActivity("probeActivity");
  return false;
}

static bool onButtonClick_BtnWifiRefr(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 手动扫描");
  startScan();
  return false;
}

/* 「只看风险设备」（WiFi 页）：把一堆邻居 WiFi 滤掉，只留可疑/高危的 SSID。 */
static bool onButtonClick_BtnApOnly(ZKButton *pButton) {
  (void)pButton;
  sApOnly = !sApOnly;
  LOGD("probeLogic: WiFi 页「只看风险」=%d", sApOnly ? 1 : 0);
  if (mListProbeApPtr) mListProbeApPtr->refreshListView();
  syncWifiSummary();
  return false;
}

static bool onButtonClick_BtnProbeRescan(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 底部「重新扫描」");
  startScan();
  return false;
}

static bool onButtonClick_BtnProbeHunt(ZKButton *pButton) {
  (void)pButton;
  if (sTgtKind == TG_NONE) {
    /* 没锁目标 → 直接锁"信号最强的那个"（最常见路径：在房间里走一圈看谁最强） */
    if (!sScan.empty()) huntLockAp(0);
    else LOGD("probeLogic: 还没有扫描结果，先扫描");
  } else {
    if (mWinHuntPtr) mWinHuntPtr->showWnd();
    sHuntOn = true;
    syncHuntPage();
  }
  return false;
}

/* ---------- WiFi 列表（按 RSSI 降序；点一行 = 锁定追踪） ---------- */

static int getListItemCount_ListProbeAp(const ZKListView *pListView) {
  (void)pListView;
  rebuildApView();
  return sApViewN;
}

static void obtainListItemData_ListProbeAp(ZKListView *pListView,
                                           ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  rebuildApView();
  if (!pListItem || index < 0 || index >= sApViewN) return;
  const WifiInfo &ap = sScan[sApView[index]];   // ★ 走筛选视图（index 是视图下标）
  char b[80], why[64], sec[24];

  /* ★ 不要给行设底色 —— 行卡片的圆角底是 item 模板里的静态子控件 RowCard
   *   （九宫格 ios_card_row.9.png）。setBackgroundColor 画的是直角矩形，
   *   还会把图的四角填成方的（"底色与图片互斥"，见 ui/main.html 注释）。 */

  ZKListView::ZKListSubItem *band = pListItem->findSubItemByID(ID_PROBE_SubPrBand);
  ZKListView::ZKListSubItem *ch = pListItem->findSubItemByID(ID_PROBE_SubPrCh);
  ZKListView::ZKListSubItem *ssid = pListItem->findSubItemByID(ID_PROBE_SubPrSsid);
  ZKListView::ZKListSubItem *sp = pListItem->findSubItemByID(ID_PROBE_SubPrSec);
  ZKListView::ZKListSubItem *rssi = pListItem->findSubItemByID(ID_PROBE_SubPrRssi);
  ZKListView::ZKListSubItem *risk = pListItem->findSubItemByID(ID_PROBE_SubPrRisk);
  ZKListView::ZKListSubItem *bar = pListItem->findSubItemByID(ID_PROBE_ApBar);

  const bool is5g = ap.getFreq() >= 5000;
  if (band) {
    band->setText(is5g ? "5G" : "2.4G");
    band->setTextColor(is5g ? C_TEXT_BLUE : C_TEXT_GOLD);
  }
  if (ch) {
    int c = chanOfFreq(ap.getFreq());
    if (c > 0) snprintf(b, sizeof(b), "CH %d", c);
    else snprintf(b, sizeof(b), "%dMHz", ap.getFreq());
    ch->setText(b);
    ch->setTextColor(C_TEXT_GOLD);
  }
  if (ssid) {
    ssid->setText(ap.getSsid().empty() ? "(隐藏网络)" : ap.getSsid());
    ssid->setTextColor(ap.getSsid().empty() ? C_TEXT_DIM : C_TEXT);
  }
  if (sp) {
    secText(ap.getEncryption(), sec, sizeof(sec));
    snprintf(b, sizeof(b), "%s · %s", sec, ap.getBssid().c_str());
    sp->setText(b);
    sp->setTextColor(C_TEXT_DIM);
  }
  if (rssi) {
    snprintf(b, sizeof(b), "%d dBm", ap.getRssi());
    rssi->setText(b);
    rssi->setTextColor(ap.getRssi() > -60 ? C_TEXT_GREEN
                                          : (ap.getRssi() > -75 ? C_TEXT_BLUE : C_TEXT_DIM));
  }
  /* 判据算**一次**，给风险文字与色条共用 */
  const int lv = riskOfAp(ap, why, sizeof(why));
  if (risk) {
    /* ⚠️ 无风险时**必须显式清空** —— 列表项视图会被跨行复用，
     * 不写就会把上一行的"高危"留在这一行上（工程记忆第 6 条）。 */
    risk->setText(why);
    risk->setTextColor(riskColor(lv));
  }
  setRiskBar(bar, lv);   // 行首色条：红=高危 / 金=可疑 / 灰=关注 / 隐形=普通
}

static void onListItemClick_ListProbeAp(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  /* ⚠️ index 是**筛选视图下标**（列表可能被"只看风险"滤过）
   *    ⇒ 必须先映射回 sScan 的真实下标，否则会锁错目标（而且不报错）。 */
  rebuildApView();
  if (index < 0 || index >= sApViewN) return;
  huntLockAp(sApView[index]);
}

/* ---------- 蓝牙探测页 ---------- */

static bool onButtonClick_BtnBtBack(ZKButton *pButton) {
  (void)pButton;
  EASYUICONTEXT->closeActivity("probeActivity");
  return false;
}

static bool onButtonClick_BtnBtRefr(ZKButton *pButton) {
  (void)pButton;
  pg::Bt *bt = btGet();
  if (bt && bt->state() == 2) {
    bt->hostScanStart();
    sBtDevCount = -1;
    LOGD("probeLogic: BLE 重新扫描");
  } else {
    LOGD("probeLogic: 蓝牙未就绪（state=%d），先点「启动蓝牙」", bt ? bt->state() : -1);
  }
  return false;
}

static bool onButtonClick_BtnBtStart(ZKButton *pButton) {
  (void)pButton;
  pg::Bt *bt = btGet();
  LOGD("probeLogic: 点「启动蓝牙」（rtk_init 约 20~40 秒）");
  if (bt) bt->start();
  syncBtPage();
  return false;
}

static bool onButtonClick_BtnBtRescan(ZKButton *pButton) {
  (void)pButton;
  return onButtonClick_BtnBtRefr(0);
}

static int getListItemCount_ListProbeBt(const ZKListView *pListView) {
  (void)pListView;
  pg::Bt *bt = sBt;
  return bt ? bt->scannedCount() : 0;
}

static void obtainListItemData_ListProbeBt(ZKListView *pListView,
                                           ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  pg::Bt *bt = sBt;
  if (!pListItem || !bt || index < 0 || index >= bt->scannedCount()) return;
  char b[32], why[64];

  ZKListView::ZKListSubItem *name = pListItem->findSubItemByID(ID_PROBE_SubBtName);
  ZKListView::ZKListSubItem *addr = pListItem->findSubItemByID(ID_PROBE_SubBtAddr);
  ZKListView::ZKListSubItem *rssi = pListItem->findSubItemByID(ID_PROBE_SubBtRssi);
  ZKListView::ZKListSubItem *risk = pListItem->findSubItemByID(ID_PROBE_SubBtRisk);

  const char *nm = bt->scannedName(index);
  int r = bt->scannedRssi(index);
  if (name) {
    name->setText(nm[0] ? nm : "(匿名设备)");
    name->setTextColor(nm[0] ? C_TEXT : C_TEXT_DIM);
  }
  if (addr) addr->setText(bt->scannedAddr(index));
  if (rssi) {
    snprintf(b, sizeof(b), "%d dBm", r);
    rssi->setText(b);
    rssi->setTextColor(r > -60 ? C_TEXT_GREEN : (r > -80 ? C_TEXT_BLUE : C_TEXT_DIM));
  }
  if (risk) {
    int lv = riskOfBle(nm, r, why, sizeof(why));
    risk->setText(why);
    risk->setTextColor(riskColor(lv));
  }
}

static void onListItemClick_ListProbeBt(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  huntLockBle(index);
}

/* ---------- 热点猎手页 ---------- */

static bool onButtonClick_BtnHuntBack(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 猎手页返回（保留目标，方便再进来）");
  if (mWinHuntPtr) mWinHuntPtr->hideWnd();
  sHuntOn = false;
  showTab(sTab);
  syncTabs();
  return false;
}

static bool onButtonClick_BtnHuntRelock(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 重新锁定（清基线）");
  huntResetBaseline();
  if (sTgtKind == TG_AP) huntTick();
  else if (sTgtKind == TG_BLE) huntTick();
  syncHuntPage();
  return false;
}

static bool onButtonClick_BtnHuntStop(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 停止追踪");
  sHuntOn = false;
  sTgtKind = TG_NONE;
  if (mWinHuntPtr) mWinHuntPtr->hideWnd();
  showTab(0);
  syncTabs();
  return false;
}

static bool onButtonClick_BtnHuntBeep(ZKButton *pButton) {
  (void)pButton;
  sBeep = !sBeep;
  LOGD("probeLogic: 蜂鸣 -> %d", sBeep ? 1 : 0);
  sBgBeep = -1;
  syncTabs();
  return false;
}

/* ==================================================================
 *        局域网设备页（主动探测，见 platform/PgLan.h）
 * ================================================================== */

static bool onButtonClick_BtnLanBack(ZKButton *pButton) {
  (void)pButton;
  EASYUICONTEXT->closeActivity("probeActivity");
  return false;
}

static bool onButtonClick_BtnLanRefr(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 局域网页刷新 -> 重新完整扫描");
  lanGet()->startScan(true, true);
  syncLanPage();
  return false;
}

static bool onButtonClick_BtnLanScan(ZKButton *pButton) {
  (void)pButton;
  pg::Lan *l = lanGet();
  if (l->running()) {
    LOGD("probeLogic: 局域网扫描 -> 停止");
    l->stop();
  } else {
    LOGD("probeLogic: 局域网扫描 -> 开始（邻居 + SSDP + 端口/协议指纹）");
    l->startScan(true, true);
  }
  syncLanPage();
  return false;
}

static bool onButtonClick_BtnLanProbe(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 深探端口（对已发现设备做端口 + 协议指纹）");
  lanGet()->startProbeAll();
  syncLanPage();
  return false;
}

/* 「目标网 / 恢复原网」—— 切网扫描的"回家快照"开关（见 platform/PgWifiCfg.h）：
 *   进：把当前网络（整份 wpa_supplicant.conf + 解析出的 ssid/psk）存进快照
 *   出：把配置还原 + 让 wpa_supplicant 重读并关联回去（8 秒没连上再交给 zknet 直连） */
static bool onButtonClick_BtnLanMode(ZKButton *pButton) {
  (void)pButton;
  pg::WifiCfg *wc = wcGet();
  const std::string cur = connectedSsid();
  if (!wc->targetMode()) {
    if (wc->enterTargetMode(cur.empty() ? 0 : cur.c_str())) {
      sModeSawOther = false;
      LOGD("probeLogic: ★ 目标网模式 ON（原网络 '%s' 已存好，可以放心去 WiFi 页切网）",
           wc->homeSsid());
    } else {
      LOGD("probeLogic: 目标网模式开启失败：%s", wc->lastError());
    }
  } else {
    char ssid[64] = {0}, psk[96] = {0};
    bool ok = wc->leaveTargetMode(ssid, sizeof(ssid), psk, sizeof(psk));
    LOGD("probeLogic: ★ 目标网模式 OFF（配置还原 %s，原网络 '%s'）", ok ? "成功" : "失败", ssid);
    if (ok && ssid[0]) {
      snprintf(sReconnectSsid, sizeof(sReconnectSsid), "%s", ssid);
      snprintf(sReconnectPsk, sizeof(sReconnectPsk), "%s", psk);
      sReconnectStage = 1;
      sReconnectAt = nowMs();
    }
    sModeSawOther = false;
  }
  syncLanPage();
  return false;
}

/* 「只看风险设备」——把手机/路由器滤掉，剩下的**就是**嫌疑设备。
 * 这是"谁是摄像头"最直接的操作：扫描 → 点一下 → 屏幕上只剩该看的。 */
static bool onButtonClick_BtnLanOnly(ZKButton *pButton) {
  (void)pButton;
  sLanOnly = !sLanOnly;
  LOGD("probeLogic: 局域网页「只看风险」=%d", sLanOnly ? 1 : 0);
  if (mListProbeLanPtr) mListProbeLanPtr->refreshListView();
  syncLanPage();
  return false;
}

static int getListItemCount_ListProbeLan(const ZKListView *pListView) {
  (void)pListView;
  rebuildLanView();
  return sLanViewN;
}

static void obtainListItemData_ListProbeLan(ZKListView *pListView,
                                            ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  rebuildLanView();
  pg::LanHost h;
  if (!pListItem || index < 0 || index >= sLanViewN) return;
  if (!lanGet()->hostCopy(sLanView[index], &h)) return;   // ★ 走筛选视图
  char b[110];

  ZKListView::ZKListSubItem *tag = pListItem->findSubItemByID(ID_PROBE_SubLanTag);
  ZKListView::ZKListSubItem *port = pListItem->findSubItemByID(ID_PROBE_SubLanPort);
  ZKListView::ZKListSubItem *ip = pListItem->findSubItemByID(ID_PROBE_SubLanIp);
  ZKListView::ZKListSubItem *mac = pListItem->findSubItemByID(ID_PROBE_SubLanMac);
  ZKListView::ZKListSubItem *meta = pListItem->findSubItemByID(ID_PROBE_SubLanMeta);
  ZKListView::ZKListSubItem *risk = pListItem->findSubItemByID(ID_PROBE_SubLanRisk);
  ZKListView::ZKListSubItem *note = pListItem->findSubItemByID(ID_PROBE_SubLanNote);
  ZKListView::ZKListSubItem *bar = pListItem->findSubItemByID(ID_PROBE_LanBar);

  /* ★ 不给**行**设底色：行卡片的圆角底是 item 模板里的静态子控件 RowCard
   *   （九宫格 ios_card_row.9.png）。setBackgroundColor 会把它填成直角（见 main.html）。
   *   ⚠️ 但行首那个 5px 的**色条**是独立小控件（div.text，无 picTab）
   *      ⇒ 给它设底色是安全的，且是"一眼看出谁可疑"的关键。 */

  setRiskBar(bar, h.level);

  if (tag) {
    tag->setText(h.kind);
    tag->setTextColor(h.level >= 3 ? C_TEXT_RED : (h.level == 2 ? C_TEXT_GOLD : C_TEXT_DIM));
  }
  if (port) {
    port->setText(h.ports[0] ? h.ports : "");
    port->setTextColor(C_TEXT_BLUE);
  }
  if (ip) ip->setText(h.ip);
  if (mac) {
    if (h.mac[0] && h.vendor[0]) snprintf(b, sizeof(b), "%s %s", h.mac, h.vendor);
    else if (h.mac[0]) snprintf(b, sizeof(b), "%s", h.mac);
    else snprintf(b, sizeof(b), "(未拿到 MAC)");
    mac->setText(b);
  }
  if (meta) {
    int np = portCountOf(h.ports);
    if (np) snprintf(b, sizeof(b), "%d 端口", np);
    else snprintf(b, sizeof(b), "在线");
    meta->setText(b);
    meta->setTextColor(h.level >= 2 ? C_TEXT_GOLD : C_TEXT_BLUE);
  }
  if (risk) {
    /* ⚠️ 无风险时必须**显式清空** —— 列表项视图跨行复用（工程记忆第 6 条） */
    risk->setText(h.why);
    risk->setTextColor(riskColor(h.level));
  }
  if (note) {
    if (h.note[0]) snprintf(b, sizeof(b), "%s", h.note);
    else if (h.ssdp[0]) snprintf(b, sizeof(b), "SSDP: %s", h.ssdp);
    else if (h.mac[0] && pg::pgMacIsRandom(h.mac)) snprintf(b, sizeof(b), "随机 MAC（手机/平板类）");
    else b[0] = 0;
    note->setText(b);
    note->setTextColor(h.note[0] ? C_TEXT_DIM : C_TEXT_DIM);
  }
}

static void onListItemClick_ListProbeLan(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  rebuildLanView();
  if (index < 0 || index >= sLanViewN) return;
  /* ⚠️ index 是筛选视图下标 ⇒ 映射回真实下标再取设备（探错的代价是"深探了别的 IP"）。 */
  pg::LanHost h;
  if (!lanGet()->hostCopy(sLanView[index], &h)) return;
  LOGD("probeLogic: 点设备 %s -> 深探端口/指纹", h.ip);
  lanGet()->startProbeOne(h.ip);
}

/* ==================================================================
 *        无线嗅探页（monitor 顺听，见 platform/PgSniff.h）
 * ================================================================== */

static bool onButtonClick_BtnSnBack(ZKButton *pButton) {
  (void)pButton;
  /* 返回主界面时**不停嗅探**：onUI_quit 会统一停（那里是唯一的收尾点，
   * 防止"点返回后有别的路径绕过"）。 */
  EASYUICONTEXT->closeActivity("probeActivity");
  return false;
}

static bool onButtonClick_BtnSnRefr(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 嗅探页刷新");
  syncSniffPage();
  if (mListProbeSnPtr) mListProbeSnPtr->refreshListView();
  return false;
}

/* 「全信道」开关：★ 会临时断开 WiFi（必须把 wlan0 down 掉才切得动信道）。
 *   开：存快照 → wlan0 down → 跳信道轮扫；关：还原网卡 + 恢复网络。 */
static bool onButtonClick_BtnSnHop(ZKButton *pButton) {
  (void)pButton;
  if (sHopMode) {
    leaveFullHop("用户关闭");
  } else {
    enterFullHop();
  }
  syncSniffPage();
  if (mListProbeSnPtr) mListProbeSnPtr->refreshListView();
  return false;
}

static bool onButtonClick_BtnSnStart(ZKButton *pButton) {
  (void)pButton;
  pg::Sniff *sn = snGet();
  if (sn->active()) {
    LOGD("probeLogic: 嗅探已经在跑");
    syncSniffPage();
    return false;
  }
  LOGD("probeLogic: 开始嗅探（把 wlan1 切成 MONITOR）");
  bool ok = sn->start();
  LOGD("probeLogic: sniff start -> %d (%s)", ok ? 1 : 0, sn->stateText());
  sSnTick = 0;
  syncSniffPage();
  if (mListProbeSnPtr) mListProbeSnPtr->refreshListView();
  return false;
}

static bool onButtonClick_BtnSnStop(ZKButton *pButton) {
  (void)pButton;
  LOGD("probeLogic: 停止嗅探（并把 wlan1 切回 station）");
  snGet()->stop();
  syncSniffPage();
  return false;
}

static int getListItemCount_ListProbeSn(const ZKListView *pListView) {
  (void)pListView;
  return snGet()->peerCount();
}

static void obtainListItemData_ListProbeSn(ZKListView *pListView,
                                           ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  pg::SniffPeer p;
  if (!pListItem || !snGet()->peerCopy(index, &p)) return;
  char b[120], why[40];

  ZKListView::ZKListSubItem *mac = pListItem->findSubItemByID(ID_PROBE_SubSnMac);
  ZKListView::ZKListSubItem *role = pListItem->findSubItemByID(ID_PROBE_SubSnRole);
  ZKListView::ZKListSubItem *rssi = pListItem->findSubItemByID(ID_PROBE_SubSnRssi);
  ZKListView::ZKListSubItem *cnt = pListItem->findSubItemByID(ID_PROBE_SubSnCnt);
  ZKListView::ZKListSubItem *info = pListItem->findSubItemByID(ID_PROBE_SubSnInfo);

  if (mac) mac->setText(p.mac);
  if (role) {
    if (p.flags & pg::SNIFF_F_HIDDEN_AP) {
      snprintf(b, sizeof(b), (p.flags & pg::SNIFF_F_RESOLVED) ? "隐藏AP●" : "隐藏AP");
      role->setTextColor(C_TEXT_GOLD);
    } else {
      snprintf(b, sizeof(b), "%s", p.role);
      role->setTextColor(C_TEXT_BLUE);
    }
    role->setText(b);
  }
  if (rssi) {
    if (p.rssi) snprintf(b, sizeof(b), "%d", p.rssi);
    else snprintf(b, sizeof(b), "--");
    rssi->setText(b);
    rssi->setTextColor(p.rssi > -60 ? C_TEXT_GREEN : (p.rssi > -75 ? C_TEXT_BLUE : C_TEXT_DIM));
  }
  if (cnt) {
    int tot = p.nMgmt + p.nData + p.nCtrl;
    snprintf(b, sizeof(b), "%d", tot);
    cnt->setText(b);
  }
  if (info) {
    /* 第二行拼"最有信息量"的那点东西：风险原因优先，其次 SSID，最后厂商/活跃度 */
    const int lv = riskOfSniff(p, why, sizeof(why));
    char s2[70] = {0};
    if (p.ssid[0]) snprintf(s2, sizeof(s2), "%s '%s'", p.ssidKind[0] ? p.ssidKind : "ssid", p.ssid);
    char s3[60] = {0};
    if (lv && why[0]) snprintf(s3, sizeof(s3), "%s · %s", why, lv >= 3 ? "高危" : (lv == 2 ? "可疑" : "关注"));
    else if (p.vendor[0]) snprintf(s3, sizeof(s3), "%s", p.vendor);
    else if (p.flags & pg::SNIFF_F_RANDOM_MAC) snprintf(s3, sizeof(s3), "随机MAC");
    /* 跳信道模式下"它在哪个信道"是最有价值的信息 ⇒ 放到行首 */
    char chp[14] = {0};
    if (p.chan > 0) snprintf(chp, sizeof(chp), "CH%d ", p.chan);
    if (s2[0] && s3[0]) snprintf(b, sizeof(b), "%s%s · %s · %ds前", chp, s2, s3, p.lastSec);
    else if (s2[0]) snprintf(b, sizeof(b), "%s%s · %ds前", chp, s2, p.lastSec);
    else if (s3[0]) snprintf(b, sizeof(b), "%s%s · %ds前", chp, s3, p.lastSec);
    else snprintf(b, sizeof(b), "%s%ds前%s", chp, p.lastSec,
                  (p.nData > p.nMgmt * 2) ? " · 以数据帧为主" : "");
    info->setText(b);
    info->setTextColor(lv >= 2 ? riskColor(lv) : C_TEXT_DIM);
  }
}

/* ==================================================================
 *        页签（4 页 × 4 段；每页各带一套，见 syncTabs 的注释）
 * ================================================================== */

static void tabGo(int t) {
  if (sTab == t) {
    syncTabs();
    return;
  }
  showTab(t);
  syncTabs();
}

static bool onButtonClick_BarSegProbe(ZKButton *pButton)   { (void)pButton; return false; }
static bool onButtonClick_BarSegProbe2(ZKButton *pButton)  { (void)pButton; return false; }
static bool onButtonClick_BarSegProbe3(ZKButton *pButton)  { (void)pButton; return false; }
static bool onButtonClick_BarSegProbe4(ZKButton *pButton)  { (void)pButton; return false; }

static bool onButtonClick_TabWifi(ZKButton *pButton)  { (void)pButton; tabGo(0); return false; }
static bool onButtonClick_TabBt(ZKButton *pButton)    { (void)pButton; tabGo(1); return false; }
static bool onButtonClick_TabLan(ZKButton *pButton)   { (void)pButton; tabGo(2); return false; }
static bool onButtonClick_TabSn(ZKButton *pButton)    { (void)pButton; tabGo(3); return false; }

static bool onButtonClick_TabWifi2(ZKButton *pButton) { (void)pButton; tabGo(0); return false; }
static bool onButtonClick_TabBt2(ZKButton *pButton)   { (void)pButton; tabGo(1); return false; }
static bool onButtonClick_TabLan2(ZKButton *pButton)  { (void)pButton; tabGo(2); return false; }
static bool onButtonClick_TabSn2(ZKButton *pButton)   { (void)pButton; tabGo(3); return false; }

static bool onButtonClick_TabWifi3(ZKButton *pButton) { (void)pButton; tabGo(0); return false; }
static bool onButtonClick_TabBt3(ZKButton *pButton)   { (void)pButton; tabGo(1); return false; }
static bool onButtonClick_TabLan3(ZKButton *pButton)  { (void)pButton; tabGo(2); return false; }
static bool onButtonClick_TabSn3(ZKButton *pButton)   { (void)pButton; tabGo(3); return false; }

static bool onButtonClick_TabWifi4(ZKButton *pButton) { (void)pButton; tabGo(0); return false; }
static bool onButtonClick_TabBt4(ZKButton *pButton)   { (void)pButton; tabGo(1); return false; }
static bool onButtonClick_TabLan4(ZKButton *pButton)  { (void)pButton; tabGo(2); return false; }
static bool onButtonClick_TabSn4(ZKButton *pButton)   { (void)pButton; tabGo(3); return false; }
static void onListItemClick_ListProbeSn(ZKListView *pListView, int index, int id) {
  LOGD_TRACE("ListProbeSn ListItemClick index = %d, id = %d", index, id);
}




