#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD



/**
 * @brief 有新的触摸事件时触发
 * @param ev 触摸事件
 * @return true 表示该触摸事件在此被拦截，系统不再将此触摸事件传递到控件上
 *         false 触摸事件将继续传递到控件上
 */
 static bool oncameraActivityTouchEvent(const MotionEvent &ev) {
  switch (ev.mActionStatus) {
  case MotionEvent::E_ACTION_DOWN: // 触摸按下
    // LOGD_TRACE("时刻 = %ld 坐标  x = %d, y = %d", ev.mEventTime, ev.mX, ev.mY);
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
/*
 * cameraLogic.cc - 局域网摄像头查看（独立 ftu：camera.ftu -> cameraActivity）
 *
 * 拆分口径见 docs/page-split-plan.md：游戏不动，其余功能界面全部独立成 ftu。
 * 本页装两个窗口：
 *   WinCam     扫描页 —— 默认显示（局域网里"疑似摄像头"的设备列表）
 *   WinCamView 查看页（videoview 透明窗口 + 探测/加载提示 + 底部三键）
 *
 * ============================ 三段链路 ============================
 *
 *  ① 找设备 —— **完全复用 `pg::Lan`**（信号探针那套，不新写一份）
 *       邻居发现（UDP 广播 → /proc/net/arp）拿 IP+MAC → MAC 厂商 OUI →
 *       摄像头端口扫描（554/8554/8000/8080/34567/37777…）+ **RTSP DESCRIBE 指纹**
 *       （回 200/401 且带 m=video = 坐实"这是一路视频流"）。
 *     ★ 本页**只列端口带 554 或 8554 的设备** —— 用户要的是"看画面"，
 *       不是"看风险排名"（那是探针页的事）。
 *
 *  ② 找地址 —— 本页唯一的新代码：**worker 线程用 `pg::Ff::probeFast()` 试路径**
 *       IPC 的 RTSP 路径各家不同（海康 `/h264/ch1/main/av_stream`、大华
 *       `/cam/realmonitor?channel=1&subtype=0`、通用 `/stream1` `/11` `/onvif1`…），
 *       而 rtsp 的 DESCRIBE 回 200/401 **立刻**就回来了 ⇒ 逐个试是最实在的办法。
 *       第一轮不带口令；全失败再带一组**常见默认口令**（admin/admin 等）重试。
 *       ⚠️ 用 `probeFast`（单次、2s 超时）而不是 `probe`（30s×2 重试）——
 *          后者是给"开机后第一次 https 冷握手"用的，拿来扫 14 个候选要十几分钟。
 *
 *  ③ 出画面 —— `pg::StreamPlayer::startWithDisplay(url, 0, 0, 0, 480, 700)`
 *       ffmpeg 解封装（**rtsp 解封装在本板 ffmpeg 里是编进去的**，见 PgFf.h）
 *       → 设备硬解（zk_h264_player）→ disp 视频层 → 从 `CamVideo` 这个 UI 层
 *       **透明窗口**透出来。
 *       ⚠️⚠️ **起流必须由 UI 线程发起**：`startCommon()` 里要在 UI 线程先"拿回声卡"
 *          （在播放线程里 openStream 会把线程卡死，见 PgStream.cpp 的注释）。
 *          所以 worker **只探测**，结果由 onUI_Timer 取走再起流。
 *
 * ============================ 相位机 ============================
 *   IDLE → PROBING → SWITCHING → WAITFRAME → PLAYING
 *                    ↘ ERROR（超时/失败，显示原因，可重试）
 *   ⚠️ 判"出画"用 `pg::H264Player::framesDecodedInRun() > 0`
 *      —— **别用累计值做减法**（清零在子线程里，自己记基线会有竞态，
 *      见 docs/iptv.md §11.4.4）。
 *
 * ============================ 手动地址表 ============================
 * `/data/camera.txt`（每行 `名称|rtsp://地址`，`#` 是注释）里的条目会**排在最前面**，
 * 直接用给定地址、不探测 —— 自动探测找不到（要账号密码/奇怪路径）时用这个兜底。
 *
 * ==================== 账号密码（2026-09-17 新增）====================
 * 有些 IPC（**海康 / 大华**…）要 **Digest 鉴权**：不带凭据时 RTSP 只回 `401`，
 * 探测拿不到任何一条可用 URL（现场那台海康 `.163` 就是这样）。
 * 让用户在**设备上直接输入**（不用 adb 写文件）：
 *   扫描页点「账号登录」→ 弹窗（WinCamPwd）填账号/密码 → 用**这份凭据再跑一轮路径探测**
 *   → 命中哪条路径，就把**那条实测能播的完整 URL** 写进 `/data/camera.txt`（去重）
 *   → 下次进页它就是"手动"条目、排在最前、点一下直接播。
 *   ★ 为什么不硬拼"IP + 海康路径"：各家路径不同，拼出来只是"猜的"；
 *     用凭据探测一遍，落盘的一定是**实测通的**，而且什么牌子都通用。
 *   ★ 凭据**用完即弃**（不常驻内存，也不参与下一台设备的探测）——
 *     否则"点了另一台"会悄悄拿上一次的密码去试。
 *   ★ **口令绝不进日志**：所有打印 URL 的地方一律先过 `camMaskUrl()`（→ `:****@`）。
 *
 * ⚠️ 弹窗的布局约束与坑**整段照抄 ui/wifi.html 的密码弹窗**：
 *   · 普通 window（**不是 modal**，modal 是输入黑洞）；所有可交互控件必须 `y < 424`
 *     （下半屏是 IME 键盘的地盘）；
 *   · 弹窗窗口 `touchable` **必须保持 false** —— 整屏窗口一旦可触摸，连键盘区域的
 *     触摸都会被本 Activity 吃掉，IME 就再也收不到按键（"wifi 键盘敲不进字"的老血案）；
 *   · 代价：**窗口里的按钮收不到点击** ⇒ 输入框/取消/保存四块在
 *     `oncameraActivityTouchEvent` 里**按坐标手动分发**，那里的坐标必须与
 *     ui/camera.html 的控件盒逐个对齐（改布局要一起改）。
 *
 * ============================ 自检通道 /tmp/pg_camcmd ============================
 *   camlist              打印列表（设备 + 手动条目）
 *   camlogin             打开「账号密码」弹窗（等价于点它，便于免触摸验收）
 *   campwd <账号> <密码>  直接写进弹窗两个输入框（余下行全算密码，可含空格）
 *   camok                等价于点「保存并播放」
 *   camcred <账号:密码>    直接设凭据（跳过弹窗，快速验"探测→落盘→播放"链路）
 *   camfile              打印 /data/camera.txt 当前内容（口令打码）
 *   camstat              打印相位机 / 解码帧数 / 内存
 *   cam <n>              看第 n 台（n<0 = 停止回列表）
 *   camurl <rtsp地址>     直接播一个地址（不探测，跳过列表）
 *   camgrab              抓一帧到 /tmp/pgframe.pgm（视频层不在 /dev/fb0，截图抓不到）
 *   camrescan            重新扫描局域网
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/PgGame.h"
#include "control/ZKListView.h"
#include "entry/EasyUIContext.h"
#include "platform/PgAlarm.h"     // 响铃让位
#include "platform/PgAudio.h"     // pg::volumeStepGlobal（音量键）
#include "platform/PgFf.h"
#include "platform/PgH264.h"
#include "platform/PgLan.h"
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgStream.h"
#include "platform/PgNavi.h"      // 视频播放页标志（navibar 会据此收起自己）
#include "control/ZKEditText.h"   // 账号密码弹窗的两个 div.input（原生 ZKEditText）
#include "ime/IMEContext.h"        // showIME / IMEContext::SIMETextInfo（同 wifiLogic）
#include "utils/Log.h"

#include <string>

namespace {

const char *kCamFile = "/data/camera.txt";
const int kCamMax = 64;
const int kProbeTimeoutMs = 2000;    // 单个候选地址的探测超时
const int kProbeWallMs = 45000;      // 一轮探测的总上限（别让用户无限等）
const int kWaitFrameMs = 15000;      // 起流后等第一帧的上限

enum {
  CAM_IDLE = 0,
  CAM_PROBING,     // worker 在试地址
  CAM_SWITCHING,   // 等上一条流收尾
  CAM_WAITFRAME,   // 已起流，等第一帧
  CAM_PLAYING,
  CAM_ERROR
};

/* ---------------- 列表条目 ---------------- */
struct CamEntry {
  bool manual;        // true = 来自 /data/camera.txt（地址已给定）
  char ip[16];
  int port;
  char name[48];
  char info[96];
  char note[32];
  int level;
  char url[256];      // manual 时有效；非 manual 留空（要探测）
};
CamEntry gCam[kCamMax];
int gCamN = 0;
int gListCount = -1;

/* 账号密码相关的前置声明（实现在文件后半段"账号密码弹窗"一节）。
 * ⚠️ 必须前置：探测 worker 在文件前半段就要用 camMaskUrl / camSaveManual。 */
void camMaskUrl(const char *in, char *out, int n);
bool camSaveManual(const char *name, const char *ip, const char *url);

/* ---------------- 相位机 ---------------- */
int gPhase = CAM_IDLE;
long long gPhaseMs = 0;
int gCur = -1;                 // 当前在看的条目下标
char gPlayUrl[256] = {0};      // 当前起流的地址
int gScanGenSeen = -1;         // 已同步过的 Lan generation
int gLogLastN = -1;            // 上次打日志时的列表条数（只在变化时打，见 camRebuildList）

/* ---------------- 探测 worker（结果区加锁） ---------------- */
pthread_mutex_t gMx = PTHREAD_MUTEX_INITIALIZER;
volatile int gProbeRun = 0;       // worker 在跑
volatile int gProbeCancel = 0;    // 请求取消
volatile int gProbeGen = 0;       // 结果代号（UI 比它决定"要不要取结果"）
int gProbeSeen = 0;
char gProbeFound[256] = {0};      // 找到的地址（空 = 没找到）
char gProbeTry[256] = {0};        // 正在试的地址（给界面显示）
int gProbeDone = 0, gProbeTotal = 0;
char gProbeFail[128] = {0};       // 全失败时的一句话原因

/* ---------------- 控件文本缓存 ---------------- */
char gStatus[192] = {0};
char gLoad[64] = {0};
char gLoadBar[64] = {0};
char gLoadHint[160] = {0};
char gViewMsg[160] = {0};

/* ---------------- 账号密码弹窗（2026-09-17） ----------------
 * 详见文件头的「账号密码」一节。 */
const int kCamKbTopY = 424;        // IME 键盘面板顶边（必须与 ui/ime.html 对齐）
bool gPwdDlgOpen = false;          // 弹窗显示中（触摸分发用它判断要不要接管）
bool gPwdSilentSet = false;        // 程序化写输入框时挡 onEditTextChanged（同 wifi 的 sPwdSilentSet）
char gUserCred[96] = {0};          // "user:pass"，只给"下一轮探测"用；用完即弃
bool gSaveFoundUrl = false;        // 这一轮探测命中后是否落盘到 /data/camera.txt
int  gPwdTargetIdx = -1;           // 弹窗针对的列表下标
char gPwdTargetText[64] = {0};     // 弹窗副标题（"ip:port"）

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 前置声明：CamKeys 的按键回调和 camTick 的出错分支都要用它们，而定义在更后面。 */
void snapLoadClear();
void camQuitAndCleanup();

template <typename T>
void setTextIfChanged(T *c, char *cache, size_t n, const char *txt) {
  if (!c) return;
  if (txt && strncmp(cache, txt, n - 1) == 0) return;
  snprintf(cache, n, "%s", txt ? txt : "");
  c->setText(cache);
}

void setCover(bool show) {
  if (mImgCamCoverPtr) mImgCamCoverPtr->setVisible(show);
}

bool hasPort(const char *ports, const char *want) {
  if (!ports || !want) return false;
  int wl = (int)strlen(want);
  const char *p = ports;
  while (p && *p) {
    const char *sp = strchr(p, ' ');
    int len = sp ? (int)(sp - p) : (int)strlen(p);
    if (len == wl && strncmp(p, want, len) == 0) return true;
    p = sp ? sp + 1 : 0;
  }
  return false;
}

/* ==================== 列表重建 ==================== */

int camLoadManual() {
  int n = 0;
  FILE *f = fopen(kCamFile, "r");
  if (!f) return 0;
  char line[400];
  while (fgets(line, sizeof(line), f) && n < 12) {      // 手动条目最多 12 条
    char *s = line;
    while (*s == ' ' || *s == '\t') ++s;
    int ln = (int)strlen(s);
    while (ln > 0 && (s[ln - 1] == '\n' || s[ln - 1] == '\r' || s[ln - 1] == ' ')) s[--ln] = 0;
    if (s[0] == 0 || s[0] == '#') continue;
    char *sep = strchr(s, '|');
    const char *nm = "手动地址";
    const char *ur = s;
    if (sep) { *sep = 0; nm = s; ur = sep + 1; }
    if (!strstr(ur, "://")) continue;                   // 不是地址的行不静默当条目
    CamEntry &e = gCam[n];
    memset(&e, 0, sizeof(e));
    e.manual = true;
    snprintf(e.name, sizeof(e.name), "%s", nm);
    snprintf(e.url, sizeof(e.url), "%s", ur);
    snprintf(e.info, sizeof(e.info), "来自 %s", kCamFile);
    snprintf(e.note, sizeof(e.note), "手动");
    ++n;
  }
  fclose(f);
  if (n) LOGD("cameraLogic: %s → %d 条手动地址", kCamFile, n);
  return n;
}

/* 从 Lan 重建列表（**只留端口带 554/8554 的**）+ 手动条目排前面。
 * ⚠️ 全程持 gMx：探测 worker 会 `gCam[gCur]` 拷一份，这里同时改数组会读到半更新结构。 */
void camRebuildList() {
  pthread_mutex_lock(&gMx);
  int n = camLoadManual();
  pg::Lan *l = pg::Lan::instance();
  int hosts = l->hostCount();
  int cand = 0, cams = 0;
  for (int i = 0; i < hosts && n < kCamMax; ++i) {
    pg::LanHost h;
    if (!l->hostCopy(i, &h)) continue;
    ++cand;
    int port = 0;
    if (hasPort(h.ports, "554")) port = 554;
    else if (hasPort(h.ports, "8554")) port = 8554;
    if (!port) continue;                                 // 没有 RTSP 端口 → 不看
    ++cams;
    CamEntry &e = gCam[n];
    memset(&e, 0, sizeof(e));
    e.manual = false;
    snprintf(e.ip, sizeof(e.ip), "%s", h.ip);
    e.port = port;
    /* 名字优先用设备类型（Lan 已经判过 kind），没有就用厂商，再没有就用 IP */
    if (h.kind[0]) snprintf(e.name, sizeof(e.name), "%s %s", h.ip, h.kind);
    else if (h.vendor[0]) snprintf(e.name, sizeof(e.name), "%s %s", h.ip, h.vendor);
    else snprintf(e.name, sizeof(e.name), "%s", h.ip);
    /* 信息行：厂商 · 开放端口 · RTSP 指纹 */
    char b[96];
    b[0] = 0;
    if (h.vendor[0]) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s", h.vendor);
    if (h.ports[0])
      snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s端口 %s", b[0] ? " · " : "", h.ports);
    snprintf(e.info, sizeof(e.info), "%s", b[0] ? b : "已发现 RTSP 端口");
    snprintf(e.note, sizeof(e.note), "%s", h.note[0] ? h.note : (port == 554 ? "疑似摄像头" : "备用端口"));
    e.level = h.level;
    ++n;
  }
  gCamN = n;
  /* ★ 只在"条数变化"时打日志（2026-09-17）。原来每探完一台 gen 就 +1、就 rebuild 一次，
   *   88~96 台会**重复打 90+ 遍**一模一样的两行 —— 本板日志缓冲只有十几行，
   *   刷爆之后真出问题（比如某台设备指纹异常）反而看不见。进度由状态行显示，不需要靠日志。 */
  if (gCamN != gLogLastN) {
    gLogLastN = gCamN;
    LOGD("cameraLogic: 列表重建 —— 局域网 %d 台，其中 %d 台开了 RTSP 端口，加手动 %d 条 ⇒ 共 %d 条",
         cand, cams, n - cams, gCamN);
    if (gCamN == 0) {
      LOGW("cameraLogic: 列表为空（局域网里没扫到开 554/8554 的设备；"
           "也可以把手动地址写进 %s）", kCamFile);
    }
  }
  pthread_mutex_unlock(&gMx);
}

/* ==================== 探测 worker ==================== */

/* 常见 RTSP 路径。**顺序 = 命中概率**（海康、大华、通用、ONVIF、V380/XMEye…）。
 * ⚠️ 第二轮的"默认口令"只重试**前 6 条** ⇒ 最该先命中的那几条必须排在前面。
 * 2026-09-17：把**海康威视**的规范路径提到最前 —— 国内现场就是海康（实测
 * 192.168.1.163 的 `/streaming/channels/101` 回 **401**（= 设备认识这个路径、只是要口令），
 * 其余路径全 404），原来的表里**根本没有它**，等于这台机器永远探不出来。 */
const char *kPaths[] = {
    "",                                       // 根（不少 IPC 直接接受 rtsp://ip:554/）
    "/Streaming/Channels/101",                // ★ 海康 主码流（文档写的就是大写 S/C）
    "/Streaming/Channels/102",                // ★ 海康 子码流
    "/h264/ch1/main/av_stream",
    "/h264/ch1/sub/av_stream",
    "/stream1",
    "/stream2",
    "/11",
    "/12",
    "/cam/realmonitor?channel=1&subtype=0",   // 大华
    "/cam/realmonitor?channel=1&subtype=1",
    "/onvif1",
    "/onvif2",
    "/live/ch0",
    "/live/ch1",
    "/1/stream1",
};
const int kPathN = (int)(sizeof(kPaths) / sizeof(kPaths[0]));

/* 常见默认口令（第二轮才用）。只在自己家的设备上试，属于"排障"用途。 */
const char *kCreds[] = {"admin:admin", "admin:12345", "admin:", "admin:admin123",
                        "root:root", "admin:123456"};
const int kCredN = (int)(sizeof(kCreds) / sizeof(kCreds[0]));

void setProbeTry(const char *url) {
  pthread_mutex_lock(&gMx);
  snprintf(gProbeTry, sizeof(gProbeTry), "%s", url);
  pthread_mutex_unlock(&gMx);
}

bool buildUrl(char *out, int n, const char *ip, int port, const char *cred, const char *path) {
  if (cred && cred[0]) {
    return snprintf(out, n, "rtsp://%s@%s:%d%s", cred, ip, port, path) > 0;
  }
  return snprintf(out, n, "rtsp://%s:%d%s", ip, port, path) > 0;
}

void *camProbeThread(void *arg) {
  CamEntry e;
  pthread_mutex_lock(&gMx);
  e = gCam[gCur];      // 拷一份，worker 不碰全局数组（UI 可能同时重建列表）
  pthread_mutex_unlock(&gMx);
  if (e.manual && e.url[0]) {
    /* 手动条目：不探测，直接把地址交回去 */
    pthread_mutex_lock(&gMx);
    snprintf(gProbeFound, sizeof(gProbeFound), "%s", e.url);
    snprintf(gProbeTry, sizeof(gProbeTry), "%s", e.url);
    gProbeTotal = gProbeDone = 1;
    gProbeRun = 0;
    ++gProbeGen;
    pthread_mutex_unlock(&gMx);
    char shown[320];
    camMaskUrl(e.url, shown, sizeof(shown));   // ★ 手动条目 URL 里带口令，必须打码
    LOGD("cameraLogic: 手动地址直接用 %s", shown);
    return 0;
  }

  const long long t0 = nowMs();
  char url[320];
  int done = 0;
  bool found = false;

  /* ★ 用户刚在弹窗里填了账号密码 ⇒ **第一轮就带凭据**。
   *   要 Digest 鉴权的 IPC 不带凭据一律只回 401，"先裸扫一轮"纯属白等；
   *   而且这时**不再试"常见默认口令"**（用户给的就是权威凭据，
   *   猜默认口令只会让等待翻倍）。凭据在探测结束时被清掉（用完即弃）。 */
  char cred[96] = {0};
  pthread_mutex_lock(&gMx);
  snprintf(cred, sizeof(cred), "%s", gUserCred);
  pthread_mutex_unlock(&gMx);
  const bool useUserCred = (cred[0] != 0);
  int total = useUserCred ? kPathN : (kPathN + kCredN * 6);
  if (useUserCred)
    LOGD("cameraLogic: 本轮**带用户凭据**探测（账号 %d 字符、密码 %d 字符），不再试默认口令",
         (int)(strchr(cred, ':') ? (strchr(cred, ':') - cred) : (int)strlen(cred)),
         (int)(strchr(cred, ':') ? strlen(strchr(cred, ':') + 1) : 0));

  /* 第一轮：带用户凭据（有的话）/ 不带口令，全路径 */
  for (int i = 0; i < kPathN && !found; ++i) {
    if (gProbeCancel) break;
    if (nowMs() - t0 > kProbeWallMs) {
      LOGW("cameraLogic: 探测总时长超过 %dms，提前收工", kProbeWallMs);
      break;
    }
    buildUrl(url, sizeof(url), e.ip, e.port, useUserCred ? cred : 0, kPaths[i]);
    char shown[320];
    camMaskUrl(url, shown, sizeof(shown));   // ★ 界面串/日志串一律打码
    setProbeTry(shown);
    pthread_mutex_lock(&gMx);
    gProbeDone = ++done;
    pthread_mutex_unlock(&gMx);
    if (pg::Ff::probeFast(url, kProbeTimeoutMs)) found = true;
  }
  /* 第二轮：常见默认口令（只在"用户没给凭据"时跑） */
  if (!found && !gProbeCancel && !useUserCred) {
    LOGD("cameraLogic: 第一轮都没通 —— 带常见默认口令再试一轮");
    for (int c = 0; c < kCredN && !found; ++c) {
      for (int i = 0; i < 6 && !found; ++i) {
        if (gProbeCancel) break;
        if (nowMs() - t0 > kProbeWallMs) break;
        buildUrl(url, sizeof(url), e.ip, e.port, kCreds[c], kPaths[i]);
        char shown2[320];
        camMaskUrl(url, shown2, sizeof(shown2));
        setProbeTry(shown2);
        pthread_mutex_lock(&gMx);
        gProbeDone = ++done;
        pthread_mutex_unlock(&gMx);
        if (pg::Ff::probeFast(url, kProbeTimeoutMs)) found = true;
      }
    }
  }

  pthread_mutex_lock(&gMx);
  gProbeTotal = total;
  if (found) snprintf(gProbeFound, sizeof(gProbeFound), "%s", url);
  else if (useUserCred)
    snprintf(gProbeFail, sizeof(gProbeFail),
             "账号或密码不对、或路径特殊（试了 %d 个地址）—— 再点「账号登录」重填", done);
  else snprintf(gProbeFail, sizeof(gProbeFail), "试了 %d 个地址都不通（要口令或路径特殊）", done);
  gProbeRun = 0;
  ++gProbeGen;
  const bool save = gSaveFoundUrl;
  gSaveFoundUrl = false;
  gUserCred[0] = 0;      // ★ 凭据用完即弃：否则"接着点另一台"会悄悄拿上一次的密码去试
  pthread_mutex_unlock(&gMx);

  char shown[320];
  camMaskUrl(found ? url : "-", shown, sizeof(shown));
  /* ★ 落盘放在锁**外**（文件 IO 不该占着探测锁），并把新条目立刻带进列表最前面。 */
  if (found && save && !e.manual) {
    camSaveManual(e.name, e.ip, url);
    camRebuildList();
  }
  LOGD("cameraLogic: 探测结束 found=%d 用时 %lldms 地址='%s'%s", (int)found, nowMs() - t0,
       shown, (found && save) ? "（已落盘 /data/camera.txt）" : "");
  return 0;
}

void camStartProbe(int idx) {
  if (idx < 0 || idx >= gCamN) {
    LOGW("cameraLogic: camStartProbe(%d) 越界（共 %d）", idx, gCamN);
    return;
  }
  if (gProbeRun) {
    LOGW("cameraLogic: 上一轮探测还没结束 —— 取消它");
    gProbeCancel = 1;
  }
  gCur = idx;
  pthread_mutex_lock(&gMx);
  gProbeFound[0] = 0;
  gProbeFail[0] = 0;
  gProbeTry[0] = 0;
  gProbeDone = 0;
  gProbeTotal = kPathN;
  gProbeCancel = 0;
  gProbeRun = 1;
  pthread_mutex_unlock(&gMx);

  pthread_t tid;
  if (pthread_create(&tid, 0, camProbeThread, 0) != 0) {
    pthread_mutex_lock(&gMx);
    gProbeRun = 0;
    snprintf(gProbeFail, sizeof(gProbeFail), "探测线程创建失败");
    ++gProbeGen;
    pthread_mutex_unlock(&gMx);
    LOGW("cameraLogic: 探测线程创建失败");
    return;
  }
  pthread_detach(tid);
  gPhase = CAM_PROBING;
  gPhaseMs = nowMs();
  LOGD("cameraLogic: 开始探测 #%d '%s'（%s:%d）", idx, gCam[idx].name, gCam[idx].ip,
       gCam[idx].port);
}

/* ==================== 起流 / 停流 ==================== */

void camShowViewWin(bool show) {
  if (show) {
    if (mWinCamPtr) mWinCamPtr->hideWnd();
    if (mWinCamViewPtr) mWinCamViewPtr->showWnd();
  } else {
    if (mWinCamViewPtr) mWinCamViewPtr->hideWnd();
    if (mWinCamPtr) mWinCamPtr->showWnd();
  }
  /* ★ 2026-09-16「检讨 UI 覆盖」修正 ②（用户已审批）：查看页的视频区是
   *   480x700 @y=0 且"等比铺满"⇒ navibar(0..52 常显) 会永久压掉画面顶部 52px。
   *   本函数是查看页唯一的显隐入口 ⇒ 标志在这里同步最稳（见 PgNavi.h）。 */
  pg::setVideoPage(show);
}

/* 停流。⚠️ 一律 stopForSwitch()，不用 stop()：stop() 会置 sNeedsReset，
 * 那是给**主界面主循环**消费的（mainLogic 的 consumeNeedsReset）—— 本页是独立 ftu，
 * 主循环不在前台，没人消费 ⇒ 会话一直挂着"待复位"。 */
void camStopStream() {
  if (pg::StreamPlayer::running()) pg::StreamPlayer::stopForSwitch();
}

void camStartStream(const char *url) {
  camStopStream();
  /* ⚠️ 透明窗口（WinCamView）**必须先亮出来**再起流：V85X 上 UI 层在最顶且不透明，
   *   只有 videoview 那块是透明区域，下层视频层的画面才能从那儿透出来。 */
  camShowViewWin(true);
  setCover(true);
  gPhase = CAM_SWITCHING;
  gPhaseMs = nowMs();
  snprintf(gPlayUrl, sizeof(gPlayUrl), "%s", url);
  /* 先把三行提示立起来（探测期的"正在找可用地址"到这儿就该换掉了） */
  setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg),
                   (gCur >= 0) ? gCam[gCur].name : "");
  setTextIfChanged(mTextCamLoadPtr, gLoad, sizeof(gLoad), "正在连接画面");
  setTextIfChanged(mTextCamLoadBarPtr, gLoadBar, sizeof(gLoadBar), "");
  setTextIfChanged(mTextCamLoadHintPtr, gLoadHint, sizeof(gLoadHint), url);
  char shownUrl[320];
  camMaskUrl(url, shownUrl, sizeof(shownUrl));   // ★ 起流地址可能带口令
  LOGD("cameraLogic: 请求起流 '%s'", shownUrl);
}

/* ==================== 相位机 ==================== */

void camSetStatus(const char *s) {
  snprintf(gStatus, sizeof(gStatus), "%s", s);
  if (mTextCamStatusPtr) mTextCamStatusPtr->setText(gStatus);
}

const char *camPhaseName(int p) {
  switch (p) {
    case CAM_IDLE: return "IDLE";
    case CAM_PROBING: return "PROBING";
    case CAM_SWITCHING: return "SWITCHING";
    case CAM_WAITFRAME: return "WAITFRAME";
    case CAM_PLAYING: return "PLAYING";
    case CAM_ERROR: return "ERROR";
    default: return "?";
  }
}

void camTick() {
  const long long el = nowMs() - gPhaseMs;

  if (gPhase == CAM_PROBING) {
    /* 进度文案：正在试哪个地址 */
    pthread_mutex_lock(&gMx);
    char tryUrl[256];
    snprintf(tryUrl, sizeof(tryUrl), "%s", gProbeTry);
    int done = gProbeDone, total = gProbeTotal, run = gProbeRun;
    pthread_mutex_unlock(&gMx);
    char b[192];
    if (run && tryUrl[0]) {
      snprintf(b, sizeof(b), "正在找可用地址… %d/%d\n%s", done, total, tryUrl);
    } else {
      snprintf(b, sizeof(b), "正在找可用地址… %d/%d", done, total);
    }
    setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg), b);
    snprintf(gLoad, sizeof(gLoad), "正在探测地址");
    if (mTextCamLoadPtr) mTextCamLoadPtr->setText(gLoad);
    /* ASCII 进度条（不引素材、也不怕缺字） */
    int pct = (total > 0) ? (done * 100 / total) : 0;
    int seg = pct / 4;
    if (seg > 25) seg = 25;
    char bar[40];
    int k = 0;
    bar[k++] = '[';
    for (int i = 0; i < 25; ++i) bar[k++] = (i < seg) ? '=' : ' ';
    bar[k++] = ']';
    bar[k] = 0;
    setTextIfChanged(mTextCamLoadBarPtr, gLoadBar, sizeof(gLoadBar), bar);
    setTextIfChanged(mTextCamLoadHintPtr, gLoadHint, sizeof(gLoadHint),
                     "逐个试常见 RTSP 路径；找不到就用手动地址表");

    if (!run && gProbeGen != gProbeSeen) {
      gProbeSeen = gProbeGen;
      char url[256], fail[128];
      pthread_mutex_lock(&gMx);
      snprintf(url, sizeof(url), "%s", gProbeFound);
      snprintf(fail, sizeof(fail), "%s", gProbeFail);
      pthread_mutex_unlock(&gMx);
      if (url[0]) {
        char shown2[320];
        camMaskUrl(url, shown2, sizeof(shown2));
        LOGD("cameraLogic: 探测到可用地址 %s → 交给 UI 线程起流", shown2);
        camStartStream(url);
      } else {
        gPhase = CAM_ERROR;
        gPhaseMs = nowMs();
        char b2[192];
        snprintf(b2, sizeof(b2), "打不开这台设备\n%s\n可把手动地址写进 %s", fail, kCamFile);
        setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg), b2);
        snprintf(gLoad, sizeof(gLoad), "没找到可用地址");
        if (mTextCamLoadPtr) mTextCamLoadPtr->setText(gLoad);
        setTextIfChanged(mTextCamLoadBarPtr, gLoadBar, sizeof(gLoadBar), "");
        setTextIfChanged(mTextCamLoadHintPtr, gLoadHint, sizeof(gLoadHint), "");
        LOGW("cameraLogic: 探测失败 —— %s", fail);
      }
    }
    return;
  }

  if (gPhase == CAM_SWITCHING) {
    if (pg::StreamPlayer::running()) {
      /* 旧流还在收尾（本板收尾偶尔要几秒），等它 */
      if (el > 8000) {
        LOGW("cameraLogic: 等旧流收尾超时 %lldms → 直接起新的", el);
      } else {
        return;
      }
    }
    /* ★★ 必须显式指定旋转，别靠 auto —— 两件事都要挡住：
     *   ① `sRotManual` 是 `PgStream` 的**全局粘性值**，主界面起播时会用**存盘偏好**设它
     *      （`mainLogic: setRotation(store.rotDeg())`，那是网络电视页"手动转屏"留下的）
     *      ⇒ 不显式复位，用户在网络电视转过一次屏，再进摄像头页**画面就躺倒**。
     *   ② `desiredRotation()` 在"源没元数据"时**兜底返回 90°** —— 那是给**投屏/网络电视**
     *      定的（手机横屏源投到竖屏机，必须转 90° 才看得见）。**摄像头不能用这条兜底**：
     *      IP 摄像头一律是**横屏源且不带旋转元数据**，套上兜底就变成"竖屏里放横着的画面"。
     *   ⇒ 摄像头 = **0° 不转**（`fitRect` 会按 480x700 的区域等比适配、上下留黑），
     *      看到的就是摄像头自己那幅正着的画面。 */
    pg::StreamPlayer::setRotation(0);
    if (!pg::StreamPlayer::startWithDisplay(gPlayUrl, 0, 0, 0, 480, 700)) {
      gPhase = CAM_ERROR;
      gPhaseMs = nowMs();
      setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg),
                       "起流失败（可能已有任务在跑）");
      LOGW("cameraLogic: startWithDisplay 返回失败");
      return;
    }
    gPhase = CAM_WAITFRAME;
    gPhaseMs = nowMs();
    LOGD("cameraLogic: 已起流，等第一帧");
    return;
  }

  if (gPhase == CAM_WAITFRAME) {
    if (pg::H264Player::framesDecodedInRun() > 0) {
      gPhase = CAM_PLAYING;
      gPhaseMs = nowMs();
      setCover(false);        // ⚠️ 出画立刻隐藏底图（留着就把视频挡死了）
      if (mTextCamLoadPtr) mTextCamLoadPtr->setText("");
      if (mTextCamLoadBarPtr) mTextCamLoadBarPtr->setText("");
      if (mTextCamLoadHintPtr) mTextCamLoadHintPtr->setText("");
      gLoad[0] = gLoadBar[0] = gLoadHint[0] = 0;
      char b[192];
      snprintf(b, sizeof(b), "正在播放：%s（%dx%d 源）", gCam[gCur].name, 0, 0);
      /* 尺寸拿不到就算了，信息行只报台名 + 地址末段 */
      setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg), gCam[gCur].name);
      LOGD("cameraLogic: 第一帧已出 → PLAYING（%lldms）", el);
      return;
    }
    if (!pg::StreamPlayer::running()) {
      gPhase = CAM_ERROR;
      gPhaseMs = nowMs();
      setCover(true);
      setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg),
                       "流已断开（起流后没出画面）");
      snapLoadClear();
      LOGW("cameraLogic: 还没出画，流就结束了");
      return;
    }
    if (el > kWaitFrameMs) {
      gPhase = CAM_ERROR;
      gPhaseMs = nowMs();
      setCover(true);
      char b[128];
      snprintf(b, sizeof(b), "起流成功但 %d 秒没出画面（可能是编码不支持）", kWaitFrameMs / 1000);
      setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg), b);
      snapLoadClear();
      LOGW("cameraLogic: 等第一帧超时 %lldms", el);
      return;
    }
    /* 还在等：转个圈提示 */
    const char *dots[] = {".", "..", "...", "...."};
    char b[64];
    snprintf(b, sizeof(b), "正在连接画面%s", dots[(el / 400) % 4]);
    setTextIfChanged(mTextCamLoadPtr, gLoad, sizeof(gLoad), b);
    setTextIfChanged(mTextCamLoadHintPtr, gLoadHint, sizeof(gLoadHint),
                     gPlayUrl);
    return;
  }

  if (gPhase == CAM_PLAYING) {
    if (!pg::StreamPlayer::running()) {
      gPhase = CAM_ERROR;
      gPhaseMs = nowMs();
      setCover(true);
      setTextIfChanged(mTextCamViewMsgPtr, gViewMsg, sizeof(gViewMsg),
                       "画面已结束（网络断开或设备关闭了流）");
      LOGW("cameraLogic: 播放中流结束");
    }
    return;
  }
}

/* 等第一帧/播放中出错时把三行提示清掉（底图已经盖上了）。 */
void snapLoadClear() {
  if (mTextCamLoadPtr) mTextCamLoadPtr->setText("");
  if (mTextCamLoadBarPtr) mTextCamLoadBarPtr->setText("");
  if (mTextCamLoadHintPtr) mTextCamLoadHintPtr->setText("");
  gLoad[0] = gLoadBar[0] = gLoadHint[0] = 0;
}

/* ==================== UI 同步 ==================== */

void syncCamList() {
  if (!mWinCamPtr || !mWinCamPtr->isWndShow()) return;
  if (mListCamPtr && gCamN != gListCount) {
    gListCount = gCamN;
    mListCamPtr->refreshListView();
    LOGD("cameraLogic: 列表已刷新（%d 行）", gCamN);
  }
}

void syncCamStatus() {
  pg::Lan *l = pg::Lan::instance();
  if (gPhase == CAM_IDLE) {
    char b[192];
    if (l->running()) {
      snprintf(b, sizeof(b), "扫描中… %s（%s）· 阶段 %d，进度 %d%%", l->subnetText(),
               l->ifName(), l->phase(), l->progress());
    } else if (gCamN > 0) {
      snprintf(b, sizeof(b), "%s · 找到 %d 台开了 RTSP 端口的设备 · 点一行查看画面",
               l->subnetText(), gCamN);
    } else {
      snprintf(b, sizeof(b), "%s · 没扫到开 RTSP 端口的设备\n"
                             "（要连进目标网络；也可把手动地址写进 %s）",
               l->subnetText(), kCamFile);
    }
    setTextIfChanged(mTextCamStatusPtr, gStatus, sizeof(gStatus), b);
  } else {
    char b[192];
    snprintf(b, sizeof(b), "查看中：%s · %s", (gCur >= 0) ? gCam[gCur].name : "?",
             camPhaseName(gPhase));
    setTextIfChanged(mTextCamStatusPtr, gStatus, sizeof(gStatus), b);
  }
}

void syncCam() {
  pg::Lan *l = pg::Lan::instance();
  if (l->generation() != gScanGenSeen) {
    gScanGenSeen = l->generation();
    camRebuildList();
    gListCount = -1;
  }
  syncCamList();
  syncCamStatus();
}

/* ==================== 账号密码弹窗 ==================== */

/** 把 URL 里的口令打码：`rtsp://admin:secret@ip/...` → `rtsp://admin:***@ip/...`。
 *  ★ 任何"要打印/要显示"的 URL 都必须先过这里 —— 口令进日志（或进界面串）都是事故。 */
void camMaskUrl(const char *in, char *out, int n) {
  if (!out || n <= 0) return;
  out[0] = 0;
  if (!in) return;
  const char *sl = strstr(in, "://");
  if (!sl) { snprintf(out, (size_t)n, "%s", in); return; }
  const char *auth = sl + 3;
  /* authority 段到第一个 '/' 为止（没有就到最后） */
  const char *slash = strchr(auth, '/');
  const char *authEnd = slash ? slash : (in + strlen(in));
  /* ★★ 必须取 authority 段内**最后一个** '@' 当分隔符：
   *   密码里本来就可能有 '@'（如 `ab@1`），用第一个 '@' 定位会把**密码尾部**当成主机名露出来
   *   —— 实测漏过（打码后是 `admin:***@1@192.168.1.19:8554`，那个 `1` 就是密码的一部分）。
   *   这与 ffmpeg 的解析规则一致：userinfo 与主机之间用**最后一个 '@'** 分隔。 */
  const char *at = 0;
  for (const char *q = auth; q < authEnd; ++q)
    if (*q == '@') at = q;
  if (!at) { snprintf(out, (size_t)n, "%s", in); return; }
  /* 账号/密码之间用**第一个 ':'** 分隔（也是 ffmpeg 的规则） */
  const char *colon = 0;
  for (const char *q = auth; q < at; ++q)
    if (*q == ':') { colon = q; break; }
  if (!colon) { snprintf(out, (size_t)n, "%s", in); return; }
  int head = (int)(colon - in) + 1;                  // 含那个冒号
  snprintf(out, (size_t)n, "%.*s***%s", head, in, at);
}

/** 追加一条手动地址到 /data/camera.txt。
 *  去重按 **IP**（同一个摄像头只留一条）：本条 URL 是"探测成功"的地址，
 *  所以已有的那条一定也是能播的，重复追加只会让列表出现两条一样的。 */
bool camSaveManual(const char *name, const char *ip, const char *url) {
  char shown[320];
  camMaskUrl(url, shown, sizeof(shown));
  FILE *f = fopen(kCamFile, "r");
  if (f) {
    char line[400];
    bool dup = false;
    while (fgets(line, sizeof(line), f)) {
      if ((ip && ip[0] && strstr(line, ip)) || strstr(line, url)) { dup = true; break; }
    }
    fclose(f);
    if (dup) {
      LOGD("cameraLogic: %s 里已有该设备（%s），不重复写", kCamFile, ip ? ip : shown);
      return true;
    }
  }
  f = fopen(kCamFile, "a");
  if (!f) {
    LOGW("cameraLogic: 打不开 %s，保存失败（地址仍可用于本次播放）", kCamFile);
    return false;
  }
  fprintf(f, "%s|%s\n", name, url);
  fflush(f);
  fclose(f);
  LOGD("cameraLogic: 已保存到 %s —— %s|%s", kCamFile, name, shown);
  return true;
}

/* IME 回调：把键盘上敲的字交回输入框。
 * ⚠️ showIME 只存指针，所以 info 必须是静态的（同 wifiLogic 的 sQaImeInfo）。 */
class CamImeListener : public IMEContext::IIMETextUpdateListener {
 public:
  void onIMETextUpdate(const std::string &text) override;
};
CamImeListener gCamImeListener;
IMEContext::SIMETextInfo gCamImeInfo;
ZKEditText *gCamImeEdit = 0;      // 这次键盘是给哪个输入框敲的

void CamImeListener::onIMETextUpdate(const std::string &text) {
  LOGD("cameraLogic: IME 交回 %d 字符 → %s", (int)text.size(),
       gCamImeEdit == mEditCamPwdPtr ? "密码框" : "账号框");
  if (!gCamImeEdit) return;
  gPwdSilentSet = true;
  gCamImeEdit->setText(text);
  gPwdSilentSet = false;          // setText 是同步回调，设完立刻复位
}

/** 显式拉起键盘。为什么不靠控件聚焦自动拉：框架的自动拉起是间歇性的（wifi 页实测）。 */
void showCamPwdIme(ZKEditText *edit, bool password) {
  if (!edit) return;
  gCamImeEdit = edit;
  gCamImeInfo.isPassword = password;
  gCamImeInfo.passwordChar = '*';
  /* ★ 必须用"全字符"类型：密码里会有 `:` `@` `/` 这些符号，
   *   限成数字/字母的话海康的密码根本敲不进去。 */
  gCamImeInfo.imeTextType = IMEContext::E_IME_TEXT_TYPE_ALL;
  gCamImeInfo.text = edit->getText();
  LOGD("cameraLogic: 拉起键盘（%s，已有 %d 字符）", password ? "密码" : "账号",
       (int)gCamImeInfo.text.size());
  EASYUICONTEXT->showIME(&gCamImeInfo, &gCamImeListener);
}

/** 开关弹窗。开的时候清空两个输入框（程序化写 ⇒ 用 gPwdSilentSet 挡掉 changed 回调）。 */
void camPwdShow(bool on) {
  if (!mWinCamPwdPtr) return;
  if (on) {
    if (mTextCamPwdTargetPtr) mTextCamPwdTargetPtr->setText(gPwdTargetText);
    if (mTextCamPwdMsgPtr) mTextCamPwdMsgPtr->setText("");
    gPwdSilentSet = true;
    if (mEditCamUserPtr) mEditCamUserPtr->setText("");
    if (mEditCamPwdPtr) mEditCamPwdPtr->setText("");
    gPwdSilentSet = false;
    mWinCamPwdPtr->setVisible(true);
    gPwdDlgOpen = true;
    LOGD("cameraLogic: 打开账号密码弹窗（目标 %s）", gPwdTargetText);
  } else {
    EASYUICONTEXT->hideIME();     // 收键盘：不收的话键盘会留在屏幕上、把列表挡住
    mWinCamPwdPtr->setVisible(false);
    gPwdDlgOpen = false;
    gCamImeEdit = 0;
    LOGD("cameraLogic: 关闭账号密码弹窗");
  }
}

void camPwdMsg(const char *s) {
  if (mTextCamPwdMsgPtr) mTextCamPwdMsgPtr->setText(s);
  LOGD("cameraLogic: 弹窗提示 '%s'", s);
}

/* ⚠️ 下面三个是**普通 helper**，不是 `onButtonClick_*`：
 *   那几个名字由 `INIT_UI_EVENT_BINDINGS` 在**全局作用域**声明，必须定义在**文件作用域**
 *   （见文件尾"按钮 / 列表回调"那段说明），放进匿名空间会变成另一个实体 ⇒ 链接错。
 *   所以真正的实现在这里，文件尾只放三行"转发"。 */
void camLoginOpen() {
  /* 目标 = 当前在看的/选中那台；还没选就用列表第一台 */
  int idx = (gCur >= 0 && gCur < gCamN) ? gCur : (gCamN > 0 ? 0 : -1);
  if (idx < 0) {
    camSetStatus("先「重新扫描」或点一台设备，再填账号密码");
    LOGD("cameraLogic: 账号登录 —— 列表空，没有可填的目标");
    return;
  }
  if (gCam[idx].manual) {
    camSetStatus("这台是手动地址（已能直接播），不用再填账号密码");
    LOGD("cameraLogic: 账号登录 —— #%d 是手动条目，跳过", idx);
    return;
  }
  gPwdTargetIdx = idx;
  snprintf(gPwdTargetText, sizeof(gPwdTargetText), "%s:%d", gCam[idx].ip, gCam[idx].port);
  camPwdShow(true);
}

void camPwdCancel() {
  LOGD("cameraLogic: 账号密码弹窗 —— 取消");
  camPwdShow(false);
}

void camPwdSubmit() {
  std::string u = mEditCamUserPtr ? mEditCamUserPtr->getText() : std::string();
  std::string w = mEditCamPwdPtr ? mEditCamPwdPtr->getText() : std::string();
  auto trim = [](std::string &t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
  };
  trim(u);
  trim(w);
  if (u.empty()) {
    camPwdMsg("账号不能为空");
    return;
  }
  /* ★ 地址是 `rtsp://账号:密码@IP:端口/路径` 拼出来的：
   *   `@` 与 `:` 是**安全**的 —— URL 用**最后一个 `@`** 分隔 userinfo 与主机、用
   *   **第一个 `:`** 分隔账号与密码（实测 `rtsp://admin:ab@1@192.168.1.19:8554` 能通）。
   *   但 `/` 会让 authority 段提前结束，`?` `#` 会被当成 query/fragment 起点，
   *   `%` 会被当成转义前缀 —— 这几个**没法安全拼进去**。
   *   ⇒ **明确报错**，绝不静默失败："密码明明对却连不上"是最难查的一类问题。 */
  /* ⚠️ **账号里的 `:` 也要挡**：URL 用"第一个 `:`"分隔账号与密码，
   *   账号里再出现 `:` 会让后半个账号被当成密码（凭据错位、且报的还是"密码不对"）。 */
  if (w.find_first_of("/?#%") != std::string::npos ||
      u.find_first_of("/?#%:") != std::string::npos) {
    camPwdMsg("账号里不能有 / ? # % :；密码里不能有 / ? # %");
    LOGD("cameraLogic: 账号或密码含无法安全拼进 URL 的字符，已拒绝提交（不静默失败）");
    return;
  }
  if (gPwdTargetIdx < 0 || gPwdTargetIdx >= gCamN) {
    camPwdMsg("目标已失效，请重新扫描");
    camPwdShow(false);
    return;
  }
  snprintf(gUserCred, sizeof(gUserCred), "%s:%s", u.c_str(), w.c_str());
  gSaveFoundUrl = true;
  /* ⚠️ 日志里**只打账号长度/要不要密码**，绝不打明文 */
  LOGD("cameraLogic: 账号密码已填（账号 %d 字符，密码 %d 字符）→ 带凭据探测 #%d",
       (int)u.size(), (int)w.size(), gPwdTargetIdx);
  camSetStatus("正在用账号密码探测画面地址...");
  int idx = gPwdTargetIdx;
  camPwdShow(false);              // 先收弹窗再起探测（屏幕还给加载提示）
  camStartProbe(idx);
}

/* ==================== 自检通道 ==================== */

char sQaTag[512] = {0};

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_camcmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void camDebugList() {
  LOGD("cameraLogic camlist: 共 %d 条（%s）", gCamN, "/data/camera.txt + pg::Lan");
  for (int i = 0; i < gCamN; ++i) {
    char shown[320];
    camMaskUrl(gCam[i].url, shown, sizeof(shown));   // ★ 手动条目 URL 里可能带口令
    LOGD("cameraLogic camlist: #%d %s '%s' ip=%s port=%d info='%s' note='%s' url='%s'",
         i, gCam[i].manual ? "手动" : "扫描", gCam[i].name, gCam[i].ip, gCam[i].port,
         gCam[i].info, gCam[i].note, shown);
  }
}

void handleCamCmd(const char *line) {
  if (strncmp(line, "camlist", 7) == 0) {
    camDebugList();
    return;
  }
  if (strncmp(line, "camstat", 7) == 0) {
    char shown[320];
    camMaskUrl(gPlayUrl, shown, sizeof(shown));      // ★ 起流地址也可能带口令
    LOGD("cameraLogic camstat: 相位=%s 下标=%d 本轮解码帧=%d 流=%d 地址='%s' 列表=%d 内存=%lldkB",
         camPhaseName(gPhase), gCur, pg::H264Player::framesDecodedInRun(),
         (int)pg::StreamPlayer::running(), shown, gCamN,
         (long long)pg::StreamPlayer::memAvailableKb());
    return;
  }
  if (strncmp(line, "camgrab", 7) == 0) {
    pg::StreamPlayer::requestGrab(30);
    LOGD("cameraLogic: camgrab -> /tmp/pgframe.pgm（视频层不在 /dev/fb0，截图抓不到画面）");
    return;
  }
  if (strncmp(line, "camrescan", 9) == 0) {
    pg::Lan::instance()->startScan(true, false, true);  // camOnly + 不发 SSDP（本页不读 ssdp 字段，白等 8.4s）
    LOGD("cameraLogic: camrescan -> Lan::startScan(tp=true, ssdp=false, camOnly=true)");
    return;
  }
  /* ---- 账号密码弹窗（免触摸验收用；每条都走**真实那条路**，不另开旁路） ---- */
  if (strncmp(line, "camlogin", 8) == 0) {
    LOGD("cameraLogic QA: camlogin -> 等价于点「账号登录」");
    camLoginOpen();
    return;
  }
  if (strncmp(line, "campwd ", 7) == 0) {
    /* campwd <账号> <余下全部算密码>（密码里可以有空格）*/
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", line + 7);
    char *sp = strchr(tmp, ' ');
    const char *user = tmp;
    const char *pass = "";
    if (sp) { *sp = 0; pass = sp + 1; }
    gPwdSilentSet = true;
    if (mEditCamUserPtr) mEditCamUserPtr->setText(user);
    if (mEditCamPwdPtr) mEditCamPwdPtr->setText(pass);
    gPwdSilentSet = false;
    /* ⚠️ 只打长度，不打明文 */
    LOGD("cameraLogic QA: campwd -> 账号 %d 字符、密码 %d 字符（明文不进日志）",
         (int)strlen(user), (int)strlen(pass));
    return;
  }
  if (strncmp(line, "camok", 5) == 0) {
    LOGD("cameraLogic QA: camok -> 等价于点「保存并播放」");
    camPwdSubmit();
    return;
  }
  if (strncmp(line, "camcred ", 8) == 0) {
    /* 跳过弹窗直接设凭据并起探测（快速验"带凭据探测 → 落盘 → 起流"整链） */
    snprintf(gUserCred, sizeof(gUserCred), "%s", line + 8);
    gSaveFoundUrl = true;
    int idx = (gCur >= 0 && gCur < gCamN) ? gCur : (gCamN > 0 ? 0 : -1);
    LOGD("cameraLogic QA: camcred -> 凭据 %d 字符，目标 #%d", (int)strlen(gUserCred), idx);
    if (idx >= 0) { gPwdTargetIdx = idx; camStartProbe(idx); }
    else LOGW("cameraLogic QA: camcred —— 列表为空，只设了凭据没起探测");
    return;
  }
  if (strncmp(line, "camfile", 7) == 0) {
    FILE *f = fopen(kCamFile, "r");
    if (!f) { LOGD("cameraLogic camfile: %s 不存在（还没保存过手动地址）", kCamFile); return; }
    char l[400];
    int k = 0;
    while (fgets(l, sizeof(l), f) && k < 20) {
      ++k;
      int ln = (int)strlen(l);
      while (ln > 0 && (l[ln - 1] == '\n' || l[ln - 1] == '\r')) l[--ln] = 0;
      char shown[400];
      char *bar = strchr(l, '|');            // 形如 名称|rtsp://...
      if (bar) {
        *bar = 0;
        char m[320];
        camMaskUrl(bar + 1, m, sizeof(m));   // ★ 口令打码
        snprintf(shown, sizeof(shown), "%s|%s", l, m);
      } else snprintf(shown, sizeof(shown), "%s", l);
      LOGD("cameraLogic camfile: %s", shown);
    }
    fclose(f);
    LOGD("cameraLogic camfile: 共 %d 行（%s）", k, kCamFile);
    return;
  }
  if (strncmp(line, "camurl ", 7) == 0) {
    camStartStream(line + 7);
    return;
  }
  if (strncmp(line, "cam ", 4) == 0 || strcmp(line, "cam") == 0) {
    int n = (strlen(line) > 4) ? atoi(line + 4) : 0;
    if (n < 0) {
      camStopStream();
      gPhase = CAM_IDLE;
      gCur = -1;
      camShowViewWin(false);
      LOGD("cameraLogic: cam 停止");
      return;
    }
    if (n >= gCamN) {
      LOGW("cameraLogic: cam %d 越界（共 %d 条）", n, gCamN);
      return;
    }
    camStartProbe(n);
    camShowViewWin(true);
    return;
  }
  /* `quit` / `exit`：等价于点左上角返回箭头（也等于长按返回键）—— **脚本化验收的收尾**。
   * 为什么必须有：本页是**独立 ftu/Activity**，主界面那条 `/tmp/pg_autostart` 管不到它，
   * 以前脚本验收"进来之后出不来"（只能重启设备或人去点箭头）。
   * 与返回箭头走**同一个函数** `camQuitAndCleanup()`（它会停流 + closeActivity），
   * 所以 QA 退出的收尾状态与人工退出完全一致（这条纪律见 docs/touch-inject.md §7）。 */
  if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
    LOGD("cameraLogic: QA quit -> 退出该页（= 点返回箭头）");
    camQuitAndCleanup();
    return;
  }
  LOGD("cameraLogic QA: 未知命令 '%s'", line);
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_camcmd", "r");
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
    char *h = strchr(line, '#');          // 行内注释（写 `cam 0 #1` 保证整份内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    handleCamCmd(line);
  }
}

/* ==================== 物理按键 ==================== */
class CamKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    if (pg::wakeSaverByKey(ke)) return true;      // 屏保开着：任意键只唤醒
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = t;
      sLPFired = false;
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;
    int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= 700 && !sLPFired) {
      LOGD("cameraLogic: 长按 %dms -> 退出", held);
      camQuitAndCleanup();
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
      int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("cameraLogic: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      return true;
    }
    if (ke.mKeyCode == 108) {
      if (gPhase != CAM_IDLE) {
        camStopStream();
        gProbeCancel = 1;
        gPhase = CAM_IDLE;
        gCur = -1;
        camShowViewWin(false);
      } else {
        camQuitAndCleanup();
      }
      return true;
    }
    return true;
  }

  static bool longPressReady() {
    if (sDownCode < 0 || sLPFired) return false;
    if (nowMs() - sDownMs < 700) return false;
    sLPFired = true;
    return true;
  }

 private:
  static int sDownCode;
  static long sDownMs;
  static bool sLPFired;
};
int CamKeys::sDownCode = -1;
long CamKeys::sDownMs = 0;
bool CamKeys::sLPFired = false;
CamKeys sKeys;

void camQuitAndCleanup() {
  gProbeCancel = 1;
  camStopStream();
  gPhase = CAM_IDLE;
  gCur = -1;
  EASYUICONTEXT->closeActivity("cameraActivity");
}

/* 上一台/下一台：在列表里换一台重新探测 + 播放（同列表里"翻页"最直观）。 */
void camStep(int dir) {
  if (gCamN <= 0) return;
  int i = (gCur < 0) ? 0 : (gCur + dir);
  if (i < 0) i = gCamN - 1;
  if (i >= gCamN) i = 0;
  LOGD("cameraLogic: %s → #%d '%s'", dir > 0 ? "下一台" : "上一台", i, gCam[i].name);
  camStartProbe(i);
  camShowViewWin(true);
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

const int TIMER_TICK = 1;
const int TICK_MS = 100;

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_TICK, TICK_MS},
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  pg::Ff::ensureReady();
  gScanGenSeen = -1;
  gLogLastN = -1;   // 重进本页时把日志门也复位（首行日志要能看见）
  camRebuildList();
  gListCount = -1;
  setCover(true);
  syncCam();
  camSetStatus("正在扫描局域网…（要连进目标网络才有结果）");
  qaSyncTag();
  LOGD("cameraLogic: 进页，列表 %d 条", gCamN);
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("cameraLogic: onUI_show");
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
  pg::Lan *l = pg::Lan::instance();
  if (!l->running() && l->hostCount() == 0) {
    LOGD("cameraLogic: 首次进页 → 自动扫一次（端口指纹 + SSDP）");
    l->startScan(true, false, true);                   // camOnly + 不发 SSDP（白白等 8.4s）
  }
  gScanGenSeen = -1;
  gLogLastN = -1;   // 重进本页时把日志门也复位（首行日志要能看见）
  camRebuildList();
  gListCount = -1;
  syncCam();
}

static void onUI_hide() {
  LOGD("cameraLogic: onUI_hide");
  pg::setVideoPage(false);      // 兜底：切后台时别把 navibar 粘在"隐藏"状态
}

static void onUI_quit() {
  LOGD("cameraLogic: onUI_quit");
  pg::setVideoPage(false);      // 同上（长按返回是 closeActivity，不走 camShowViewWin）
  gProbeCancel = 1;
  camStopStream();             // 离开页面必须停流（本板 55MB 内存，留着下一个应用必炸）
  gPhase = CAM_IDLE;
  gCur = -1;
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeys);
#endif
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) {
  if (id != TIMER_TICK) return false;
  /* 响铃让位（与 iptv/probe/radio 一致）：本页是全屏独立页，留着就看不到主界面的
   * 闹钟提醒页（提醒页在 main.ftu，由主循环弹）。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("cameraLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    camQuitAndCleanup();
    return false;
  }
  if (CamKeys::longPressReady()) {
    camQuitAndCleanup();
    return false;
  }
  qaPoll();
  syncCam();
  camTick();
  return true;
}

/* 弹窗打开时的触摸分发（**必须留在匿名空间里**）。
 *
 * ⚠️ 为什么不直接写在文件尾的 `oncameraActivityTouchEvent` 里：那个函数在**匿名空间之外**，
 *   而生成宏 `INIT_UI_EVENT_BINDINGS` 已在**全局作用域**声明了 `onButtonClick_BtnCamPwdCancel`
 *   等同名函数；匿名空间里再定义一份会被注入全局作用域 ⇒ 在全局作用域调用会
 *   **同时命中两个声明 ⇒ "call of overloaded ... is ambiguous" 编译失败**（实测踩到）。
 *   搬进匿名空间后内层声明隐藏外层声明，调用无歧义；文件尾只留一行转发。
 *
 * ⚠️ 这里调的是 `camPwdCancel()/camPwdSubmit()`（普通 helper），不是 `onButtonClick_*`。 */
bool camPwdTouch(const MotionEvent &ev) {
  /* 弹窗没开时：全部放行（本页都是原生按钮，视频区不需要拦截）。 */
  if (!gPwdDlgOpen) return false;

  /* ★★ 键盘（IME SysApp）显示期间，**y ≥ 424 一个触摸都不能吞**。
   *   老血案（见 ui/wifi.html 与 wifiLogic 的同名函数）：原来白名单没有下限，
   *   把键盘面板（y=424..800）的触摸也吞了 ⇒ 键位永远收不到 DOWN
   *   （症状："键盘弹出来了但敲不进字"）。 */
  if (ev.mY >= kCamKbTopY) return false;

  /* ★★ 为什么四块可交互区要**按坐标手动分发**：
   *   弹窗窗口 WinCamPwd 是 `touchable=false`（这条不能改 —— 整屏窗口一旦可触摸，
   *   连键盘区域的触摸都会被本 Activity 吃掉，IME 就再也收不到按键）。
   *   代价就是**窗口里的按钮收不到 click**（实测：回调一次都不会被触发），
   *   只能在这里判坐标 → 直接调回调。
   * ⚠️ 下面这些数字必须与 ui/camera.html 的控件盒**逐个对齐**，改布局要一起改。 */
  static const int kEditX = 60, kEditW = 360, kEditH = 52;
  static const int kUserY = 184;        // EditCamUser
  static const int kPwdY = 244;         // EditCamPwd
  static const int kBtnY = 328, kBtnH = 52, kBtnW = 170;
  static const int kBtnCancelX = 60, kBtnOkX = 250;
  static const int kCardX = 40, kCardY = 110, kCardW = 400, kCardH = 300;

  if (ev.mActionStatus == MotionEvent::E_ACTION_UP) {
    if (ev.mX >= kEditX && ev.mX < kEditX + kEditW &&
        ev.mY >= kUserY && ev.mY < kUserY + kEditH) {
      showCamPwdIme(mEditCamUserPtr, false);
      return true;
    }
    if (ev.mX >= kEditX && ev.mX < kEditX + kEditW &&
        ev.mY >= kPwdY && ev.mY < kPwdY + kEditH) {
      showCamPwdIme(mEditCamPwdPtr, true);
      return true;
    }
    if (ev.mX >= kBtnCancelX && ev.mX < kBtnCancelX + kBtnW &&
        ev.mY >= kBtnY && ev.mY < kBtnY + kBtnH) {
      LOGD("cameraLogic: 弹窗内点「取消」（手动分发）");
      onButtonClick_BtnCamPwdCancel(0);
      return true;
    }
    if (ev.mX >= kBtnOkX && ev.mX < kBtnOkX + kBtnW &&
        ev.mY >= kBtnY && ev.mY < kBtnY + kBtnH) {
      LOGD("cameraLogic: 弹窗内点「保存并播放」（手动分发）");
      onButtonClick_BtnCamPwdOk(0);
      return true;
    }
  }
  /* 卡片范围内的其余触摸：吞掉（不然会穿透到下层列表、误开别的摄像头）。 */
  if (ev.mX >= kCardX && ev.mX < kCardX + kCardW &&
      ev.mY >= kCardY && ev.mY < kCardY + kCardH) {
    return true;
  }
  return false;
}

/* ==================================================================
 *  按钮 / 列表回调（必须留在文件作用域 static：INIT_UI_EVENT_BINDINGS 会在文件顶部
 *  声明它们，放进匿名 namespace 会变成另一个实体 → 链接错）
 * ================================================================== */

static bool onButtonClick_BtnCamBack(ZKButton *p) {
  (void)p;
  LOGD("cameraLogic: 点返回");
  camQuitAndCleanup();
  return true;
}

static bool onButtonClick_BtnCamRescanTop(ZKButton *p) {
  (void)p;
  pg::Lan::instance()->startScan(true, false, true);  // camOnly + 不发 SSDP（本页不读 ssdp 字段，白等 8.4s）
  syncCam();
  LOGD("cameraLogic: 重新扫描（带端口指纹 + SSDP）");
  return true;
}

static bool onButtonClick_BtnCamRescan(ZKButton *p) {
  (void)p;
  pg::Lan::instance()->startScan(true, false, true);  // camOnly + 不发 SSDP（本页不读 ssdp 字段，白等 8.4s）
  syncCam();
  LOGD("cameraLogic: 重新扫描（带端口指纹 + SSDP）");
  return true;
}

static bool onButtonClick_BtnCamDeep(ZKButton *p) {
  (void)p;
  /* 已经扫到主机了，只补"端口 + 指纹"那一刀（比整轮重扫快）。 */
  pg::Lan::instance()->startProbeAll();
  syncCam();
  LOGD("cameraLogic: 深探端口指纹（startProbeAll）");
  return true;
}

/* ↓↓↓ 三个"生成名"回调：按本文件约定**必须留在文件作用域**（理由见上面那段说明）。
 *    真正实现在匿名空间的 camLoginOpen / camPwdCancel / camPwdSubmit。 */
static bool onButtonClick_BtnCamLogin(ZKButton *p) {
  (void)p;
  camLoginOpen();
  return true;
}

static bool onButtonClick_BtnCamPwdCancel(ZKButton *p) {
  (void)p;
  camPwdCancel();
  return true;
}

static bool onButtonClick_BtnCamPwdOk(ZKButton *p) {
  (void)p;
  camPwdSubmit();
  return true;
}

static bool onButtonClick_BtnCamPrev(ZKButton *p) {
  (void)p;
  camStep(-1);
  return true;
}

static bool onButtonClick_BtnCamNext(ZKButton *p) {
  (void)p;
  camStep(+1);
  return true;
}

static bool onButtonClick_BtnCamStop(ZKButton *p) {
  (void)p;
  LOGD("cameraLogic: 停止");
  gProbeCancel = 1;
  camStopStream();
  gPhase = CAM_IDLE;
  camShowViewWin(false);
  setCover(true);
  camSetStatus("已停止。点一行再看画面");
  return true;
}

/* ==================== 列表回调 ==================== */
static int getListItemCount_ListCam(const ZKListView *pListView) {
  (void)pListView;
  return gCamN;
}

static void obtainListItemData_ListCam(ZKListView *pListView,
                                       ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  if (!pListItem || index < 0 || index >= gCamN) return;
  const CamEntry &e = gCam[index];

  /* ⚠️ 行视图跨行复用：每个字段每行都要写全。 */
  ZKListView::ZKListSubItem *nm = pListItem->findSubItemByID(ID_CAMERA_SubCamName);
  ZKListView::ZKListSubItem *info = pListItem->findSubItemByID(ID_CAMERA_SubCamInfo);
  ZKListView::ZKListSubItem *nt = pListItem->findSubItemByID(ID_CAMERA_SubCamNote);

  if (nm) {
    nm->setText(e.name);
    nm->setTextColor(index == gCur ? 0xFF0A84FF : 0xFFF2F2F7);
  }
  if (info) {
    info->setText(e.info);
    info->setTextColor(0xFF9A9AA0);
  }
  if (nt) {
    nt->setText(e.note);
    /* 高危/可疑用橙红，普通用灰 —— 列表里一眼能挑出"更像摄像头"的那台 */
    nt->setTextColor(e.level >= 2 ? 0xFFFF453A : (e.level == 1 ? 0xFFFF9F0A : 0xFF8E8E93));
  }
}

static void onListItemClick_ListCam(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  if (index < 0 || index >= gCamN) return;
  LOGD("cameraLogic: 点列表第 %d 行 -> %s（%s）", index, gCam[index].name,
       gCam[index].manual ? "手动地址" : "扫描到");
  camStartProbe(index);
  camShowViewWin(true);
}

/* 输入框内容变化：本页**不做自动提交**（填完要点「保存并播放」）——
 * 所以这里只记**长度**。⚠️ 绝不能把内容打进日志（那是密码明文）。 */
static void onEditTextChanged_EditCamUser(const std::string &text) {
  if (gPwdSilentSet) return;      // 程序化 setText（开弹窗时清空），不是用户输入
  LOGD("cameraLogic: 账号框 %d 字符", (int)text.size());
}

static void onEditTextChanged_EditCamPwd(const std::string &text) {
  if (gPwdSilentSet) return;
  LOGD("cameraLogic: 密码框 %d 字符", (int)text.size());
}
