#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * radioLogic.cc - 网络收音机（独立 ftu：radio.ftu -> radioActivity）
 *
 * 拆分口径见 docs/page-split-plan.md：游戏不动，其余功能界面全部独立成 ftu。
 * 本页装两个窗口：
 *   WinRadio     电台列表页 —— 默认显示
 *   WinRadioPlay 播放页（大字台名 + 已播时长 + 上一个/停止/下一个）
 *
 * ============================ 播放链路 ============================
 * 两条源都走 **pg::StreamPlayer**，但入口不同：
 *
 *   ① 直连音频流（http/https 的 mp3/aac/ogg，icecast/shoutcast 那种）
 *      → `StreamPlayer::startDecodeTest(url, 0)`
 *        它等价于 `startCommon` 且 `sDisp=0`（不要画面）。
 *        ⚠️ **PgStream 本来就支持纯音频源** —— PgStream.cpp 里
 *           `if (!hasVideo) { …跳过 MPP/硬解/显示，只解音频 }`，
 *           音频走 ffmpeg 软解 → 重采样 22050Hz/单声道/S16 → 灌进那条常开 PCM 流
 *           （PgAudio::streamOn/streamWrite）。**本页不需要 videoview、不需要视频层。**
 *
 *   ② HLS（.m3u8，央广/省台基本都是这种）
 *      → `pg::Hls::start(m3u8, play, n)` 先起"拉清单+下分片+拼 MPEG-TS"的线程，
 *        拿到播放地址（固定 `pg-hls://live`，走自定义 AVIO）后同样交给
 *        `StreamPlayer::startDecodeTest`。这条链路与 IPTV 完全同源，只是不要画面。
 *        ⚠️ 纯音频的 m3u8（`zgzs/index.m3u8` 这类）解出来没有视频轨 ⇒ 自动落到
 *           上面的"纯音频"分支，不会有任何多余的解码开销。
 *
 * ============================ 电台表 ============================
 * `radioLoadStations()` 两级：
 *   ① `/data/radio.txt`（每行 `名称|地址`，`#` 开头是注释）—— 用户自己的台
 *   ② 没有文件（或文件里一条都解析不出来）→ 内置示例台 `kBuiltinSta`
 * ⚠️ 名字里若用了字库子集里没有的汉字，**会静默不画**（本项目 font/ 是完全替换
 *    系统字体、没有逐字回退）。用户自己的台名建议用常见字或 ASCII。
 *
 * ============================ 自检通道 ============================
 * `/tmp/pg_radiocmd`（独立页必须有自己的通道 —— 后台 Activity 的定时器不跑，
 * 主界面的 /tmp/pg_autostart 在这里失效）：
 *   radio <n>              播第 n 个台（n<0 = 停止回列表）
 *   radiolist              打印电台表（含每台的**实测**结论）
 *   radiostat              打印播放状态（相位/下标/位置ms/内存）
 *   radiosave <名称>|<地址> 往 /data/radio.txt 追加一条并重载
 *   radiohint              打印电台表文件路径与用法
 *   radioscan [每台ms]     ★ **真机实测整张表**：逐台走真实播放路径，判据是
 *                          "位置毫秒真的在走"（= 真出声）；结果写 /data/radio_scan.txt
 *   radioscanstat / radioscanstop   进度 / 中止（中止也会落盘）
 *   viz                    ★ 打印播放页频谱快照（音量 / 系数 / 原始段值 / 实际柱高px / FFT 次数）
 *   vizgain <dB>           调显示增益（标定用，默认 -70dB）
 *   vizfollow [0|1]        幅度是否跟随系统音量（默认 1 = 开；0 便于做对照实验）
 *   vol <0..100>           设系统音量（验"幅度跟着音量走"用；也方便不退页调音量）
 */
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
#include "platform/PgHls.h"
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgStream.h"
#include "platform/PgViz.h"       // 播放页频谱（真 FFT；数据源在 PgAudio 灌喇叭那条路上）
#include "ui/ToolPage.h"          // pg::pgHost()：读系统音量（幅度跟音量走，见 syncRadioViz）
#include "utils/Log.h"

namespace {

const char *kStaFile = "/data/radio.txt";
const int kMaxSta = 160;            // 表容量（2026-09-18：内置表由 7 台扩到 40+ 台；满了要 WARN，不许静默丢）
const int kConnTimeoutMs = 20000;   // 连接/起播超时（直播源死的时候等太久像卡死）
const int kEqBars = 30;             // 播放页频谱柱数（== pg::kVizBands，改一处必须改两处）
const int kEqBottom = 596;          // 频谱柱底边 y（与 ui/radio.html 的初始盒一致）
const int kEqMaxH = 132;            // 满格柱高

struct Sta {
  char name[40];
  char url[224];
  char group[20];
  signed char ok;     // -1 未实测 / 1 实测能播 / 0 实测打不开（radioscan 填）
};

Sta gSta[kMaxSta];
int gCount = 0;
int gPlaying = -1;      // 正在播的下标（-1 = 没在播）
long long gPhaseMs = 0; // 相位开始时刻
int gPhase = 0;         // 0 空闲 / 1 连接中 / 2 播放中 / 3 出错

char gListStatus[160] = {0};   // 列表页状态行（缓存，变了才 setText）
char gPlayName[80] = {0};      // 播放页台名缓存
char gPlayState[96] = {0};     // 播放页状态行缓存
char gPlayInfo[160] = {0};     // 播放页信息行缓存
char gPlayTime[32] = {0};      // 播放页"已播时长"缓存
int gListCount = -1;           // 已下发给列表的行数（变了才 refreshListView）

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 只在内容真的变了才 setText：本页定时器 100ms 一拍，无脑 setText 会持续触发重绘。 */
template <typename T>
void setTextIfChanged(T *c, char *cache, size_t n, const char *txt) {
  if (!c) return;
  if (txt && strncmp(cache, txt, n - 1) == 0) return;
  snprintf(cache, n, "%s", txt ? txt : "");
  c->setText(cache);
}

/* ==================== 电台表 ==================== */

/* 内置电台表（**2026-09-18 逐台实测**，不是拍脑袋抄来的）。
 *
 * 怎么来的（可复现）：
 *   ① PC 侧用 `tools/radio_probe.py` 批量探（直连、绕过本机的 http 代理；只看"真的
 *      拉到音频帧"—— 找同步头并**用第二帧确认**，因为流前面常有一段非音频前缀）；
 *   ② 入选的真机再逐台起流复验（页面 QA `radioscan`，见文件末尾）—— **以真机为准**。
 *
 * 排序原则：**先能最快出声音的**（直连 MP3 → HLS），坏的/慢的排后面，用户点第一个就有声。
 * ⚠️ 名字请用**常用字**：本项目 font/ 是完全替换系统的子集字库、没有逐字回退，
 *    生僻字会**整个字消失**（不是方框）。改完名字要重跑 `tools/gen_font.py`。 */
struct BuiltinSta { const char *name; const char *url; const char *group; };
const BuiltinSta kBuiltinSta[] = {
    /* ---- 央广（中央广播电视总台）---- */
    {"中国之声",        "https://ngcdn001.cnr.cn/live/zgzs/index.m3u8",      "央广"},
    {"经济之声",        "https://ngcdn002.cnr.cn/live/jjzs/index.m3u8",      "央广"},
    {"中国交通广播",    "https://ngcdn002.cnr.cn/live/gsgljtgb/index.m3u8",  "央广"},
    {"香港之声",        "https://ngcdn002.cnr.cn/live/xgzs/index.m3u8",      "央广"},
    {"中国之声(直连)",  "https://lhttp.qtfm.cn/live/15318317/64k.mp3",       "央广"},
    /* ---- 国家台（中国国际广播电台 CRI）---- */
    {"环球资讯广播",    "https://sk.cri.cn/hyhq.m3u8",                       "国家台"},
    {"世界华声",        "https://sk.cri.cn/hxfh.m3u8",                       "国家台"},
    {"英语资讯广播",    "https://sk.cri.cn/am846.m3u8",                      "国家台"},
    {"南海之声",        "https://sk.cri.cn/nhzs.m3u8",                       "国家台"},
    /* ---- 省市台（64k MP3 直连，起播最快）---- */
    {"北京交通广播",    "https://lhttp.qtfm.cn/live/336/64k.mp3",            "省市台"},
    {"北京音乐广播",    "https://lhttp.qtfm.cn/live/332/64k.mp3",            "省市台"},
    {"上海新闻广播",    "https://lhttp.qtfm.cn/live/270/64k.mp3",            "省市台"},
    {"第一财经广播",    "https://lhttp.qtfm.cn/live/276/64k.mp3",            "省市台"},
    {"广东珠江经济台",  "https://lhttp.qtfm.cn/live/1259/64k.mp3",           "省市台"},
    {"广东音乐之声",    "https://lhttp.qtfm.cn/live/1260/64k.mp3",           "省市台"},
    {"广东交通之声",    "https://lhttp.qtfm.cn/live/1262/64k.mp3",           "省市台"},
    {"深圳新闻广播",    "https://lhttp.qtfm.cn/live/1270/64k.mp3",           "省市台"},
    {"深圳音乐广播",    "https://lhttp.qtfm.cn/live/1271/64k.mp3",           "省市台"},
    {"四川交通广播",    "https://lhttp.qtfm.cn/live/4886/64k.mp3",           "省市台"},
    {"湖北楚天交通",    "https://lhttp.qtfm.cn/live/1291/64k.mp3",           "省市台"},
    {"湖北经典音乐",    "https://lhttp.qtfm.cn/live/1296/64k.mp3",           "省市台"},
    {"陕西交通广播",    "https://lhttp.qtfm.cn/live/1601/64k.mp3",           "省市台"},
    {"福建交通广播",    "https://lhttp.qtfm.cn/live/1733/64k.mp3",           "省市台"},
    {"山东经济广播",    "https://lhttp.qtfm.cn/live/20236/64k.mp3",          "省市台"},
    {"辽宁交通广播",    "https://lhttp.qtfm.cn/live/20025/64k.mp3",          "省市台"},
    {"青岛新闻广播",    "https://lhttp.qtfm.cn/live/1673/64k.mp3",           "省市台"},
    {"云南新闻广播",    "https://lhttp.qtfm.cn/live/1926/64k.mp3",           "省市台"},
    {"温州音乐之声",    "https://lhttp.qtfm.cn/live/1149/64k.mp3",           "省市台"},
    {"贵州音乐广播",    "https://lhttp.qtfm.cn/live/20067/64k.mp3",          "省市台"},
    /* ---- 华语音乐 ---- */
    {"华语金曲台",      "https://lhttp.qtfm.cn/live/5022308/64k.mp3",        "音乐"},
    {"怀集音乐之声",    "https://lhttp.qtfm.cn/live/4804/64k.mp3",           "音乐"},
    {"清晨音乐台",      "https://lhttp.qtfm.cn/live/4915/64k.mp3",           "音乐"},
    {"广州金曲音乐",    "https://lhttp.qtfm.cn/live/20192/64k.mp3",          "音乐"},
    {"两广之声音乐台",  "https://lhttp.qtfm.cn/live/20500149/64k.mp3",       "音乐"},
    {"亚洲粤语台",      "https://lhttp.qtfm.cn/live/15318569/64k.mp3",       "音乐"},
    {"河南星河音乐",    "https://lhttp.qtfm.cn/live/20210755/64k.mp3",       "音乐"},
    {"山东音乐广播",    "http://audiolive302.iqilu.com/sdradioYinyue/sdradio07/playlist.m3u8", "音乐"},
    {"吉林音乐广播",    "https://live-jlr.jlntv.cn/live/fm927.m3u8",         "音乐"},
    /* ---- 氛围 / 电子（国外源，WiFi 直连）---- */
    {"SomaFM Groove Salad",  "https://ice2.somafm.com/groovesalad-128-mp3",  "氛围"},
    {"SomaFM Drone Zone",    "https://ice6.somafm.com/dronezone-128-mp3",    "氛围"},
    {"SomaFM Lush",          "https://ice5.somafm.com/lush-128-mp3",         "氛围"},
    {"SomaFM Secret Agent",  "https://ice5.somafm.com/secretagent-128-mp3",  "氛围"},
    {"SomaFM Indie Pop",     "https://ice2.somafm.com/indiepop-128-mp3",     "氛围"},
    {"SomaFM Beat Blender",  "https://ice2.somafm.com/beatblender-128-mp3",  "氛围"},
    {"SomaFM Suburbs of Goa","https://ice2.somafm.com/suburbsofgoa-128-mp3", "氛围"},
    {"SomaFM The Trip",      "https://ice2.somafm.com/thetrip-128-mp3",      "氛围"},
    {"SomaFM 80s",           "https://ice5.somafm.com/u80s-128-mp3",         "氛围"},
    {"SomaFM Deep Space",    "https://ice2.somafm.com/deepspaceone-128-mp3", "氛围"},
    {"Radio Paradise",       "http://stream.radioparadise.com/mp3-128",      "氛围"},
    {"Radio Paradise 舒缓",  "http://stream.radioparadise.com/mellow-128",   "氛围"},
    {"Radio Paradise 摇滚",  "http://stream.radioparadise.com/rock-128",     "氛围"},
    {"Smooth Chill",         "https://media-ssl.musicradio.com/ChillMP3",    "氛围"},
    {"1.FM Chillout",        "http://strm112.1.fm/chilloutlounge_mobile_mp3","氛围"},
    {"Bayern Chillout",      "http://mp3channels.webradio.antenne.de/chillout", "氛围"},
    /* ---- 爵士 / 古典 ---- */
    {"瑞士古典爵士",    "http://stream.srg-ssr.ch/m/rsj/mp3_128",            "爵士"},
    {"Jazz Radio 爵士", "http://jazzradio.ice.infomaniak.ch/jazzradio-high.mp3", "爵士"},
    {"Jazz Radio 蓝调", "http://jazzblues.ice.infomaniak.ch/jazzblues-high.mp3", "爵士"},
    {"Smooth Jazz 101", "http://jking.cdnstream1.com/b22139_128mp3",         "爵士"},
    {"经典调频",        "http://media-ice.musicradio.com/ClassicFMMP3",      "古典"},
    {"Radio Swiss 古典","http://stream.srg-ssr.ch/m/rsc_de/mp3_128",        "古典"},
    {"美国公共广播",    "http://relax.stream.publicradio.org/relax.mp3",     "古典"},
};

void trimInplace(char *s) {
  int n = (int)strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                   s[n - 1] == '\n')) {
    s[--n] = 0;
  }
  int i = 0;
  while (s[i] == ' ' || s[i] == '\t') ++i;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* 从地址里抠一个"能显示的名字"（域名主体），用于"只写了地址"的行。
 * 例：http://ice1.somafm.com/groovesalad-128-mp3 → ice1.somafm.com */
void nameFromUrl(const char *url, char *out, int n) {
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  int j = 0;
  for (int i = 0; p[i] && p[i] != '/' && p[i] != '?' && j < n - 1; ++i) out[j++] = p[i];
  out[j] = 0;
  if (j == 0) snprintf(out, n, "电台");
}

/* 追加一条到 /data/radio.txt（QA radiosave 用；也方便"另存一个能编辑的文件"）。 */
bool radioAppendStation(const char *name, const char *url) {
  FILE *f = fopen(kStaFile, "a");
  if (!f) {
    LOGW("radioLogic: 打不开 %s，无法追加", kStaFile);
    return false;
  }
  fprintf(f, "%s|%s\n", (name && name[0]) ? name : "自定义", url);
  fclose(f);
  LOGD("radioLogic: 已追加电台到 %s（%s）", kStaFile, url);
  return true;
}

/* 读一行 → 一条电台。返回 false 表示这行不是有效条目。 */
bool parseStaLine(char *line, Sta *out) {
  trimInplace(line);
  if (line[0] == 0 || line[0] == '#') return false;   // 空行 / 注释
  char *sep = strchr(line, '|');
  if (!sep) {
    /* 没有 '|'：按"第一个空白"分（名称里带空格时不写 '|' 就会被误解，
     * 所以文档里推荐 '|'）。再不行就当整行是地址。 */
    sep = strchr(line, '\t');
    if (!sep) {
      char *sp = strchr(line, ' ');
      if (sp && strstr(sp, "://")) sep = sp;
    }
  }
  if (sep) {
    *sep = 0;
    char *name = line;
    char *url = sep + 1;
    trimInplace(name);
    trimInplace(url);
    if (url[0] == 0) return false;
    if (name[0]) snprintf(out->name, sizeof(out->name), "%s", name);
    else nameFromUrl(url, out->name, sizeof(out->name));
    snprintf(out->url, sizeof(out->url), "%s", url);
  } else {
    nameFromUrl(line, out->name, sizeof(out->name));
    snprintf(out->url, sizeof(out->url), "%s", line);
  }
  /* 只认带协议的地址 —— 免得把一行说明文字当成电台（静默失败的一种） */
  if (!strstr(out->url, "://")) {
    LOGW("radioLogic: 跳过非地址行 '%s'", out->url);
    return false;
  }
  return true;
}

int radioLoadStations() {
  gCount = 0;
  int dropped = 0;
  FILE *f = fopen(kStaFile, "r");
  if (f) {
    char line[320];
    while (fgets(line, sizeof(line), f)) {
      Sta s;
      s.ok = -1;
      if (parseStaLine(line, &s)) {
        if (gCount >= kMaxSta) {      // ★ 不许静默丢（原 kMaxSta=64 时超了没人知道）
          ++dropped;
          continue;
        }
        snprintf(s.group, sizeof(s.group), "自定义");
        gSta[gCount++] = s;
      }
    }
    fclose(f);
    if (dropped) {
      LOGW("radioLogic: %s 的电台超出上限 %d，**丢弃了 %d 条**（改大 kMaxSta 或精简表）",
           kStaFile, kMaxSta, dropped);
    }
    LOGD("radioLogic: 读 %s → %d 条", kStaFile, gCount);
  } else {
    LOGD("radioLogic: 没有 %s（用内置台表）", kStaFile);
  }
  if (gCount == 0) {
    const int n = (int)(sizeof(kBuiltinSta) / sizeof(kBuiltinSta[0]));
    for (int i = 0; i < n && gCount < kMaxSta; ++i) {
      snprintf(gSta[gCount].name, sizeof(gSta[gCount].name), "%s", kBuiltinSta[i].name);
      snprintf(gSta[gCount].url, sizeof(gSta[gCount].url), "%s", kBuiltinSta[i].url);
      snprintf(gSta[gCount].group, sizeof(gSta[gCount].group), "%s", kBuiltinSta[i].group);
      gSta[gCount].ok = -1;
      ++gCount;
    }
    LOGD("radioLogic: 内置台表 %d 条", gCount);
  }
  return gCount;
}

/* ==================== 播放 ==================== */

bool isHlsUrl(const char *u) { return strstr(u, ".m3u8") != 0; }

void radioStopEngine() {
  /* ⚠️ 一律用 stopForSwitch() 而不是 stop()：stop() 会置 sNeedsReset，
   *   那是给"主界面主循环"消费的（mainLogic.cc 的 consumeNeedsReset）。
   *   本页是独立 ftu，主循环不在前台跑，没人消费 ⇒ 会话一直挂着"待复位"。
   *   stopForSwitch 的语义正好：停掉旧流、马上要用这个通道起新源。 */
  pg::Hls::stop();
  if (pg::StreamPlayer::running()) pg::StreamPlayer::stopForSwitch();
}

void radioShowPlayWin(bool show) {
  if (show) {
    if (mWinRadioPtr) mWinRadioPtr->hideWnd();
    if (mWinRadioPlayPtr) mWinRadioPlayPtr->showWnd();
  } else {
    if (mWinRadioPlayPtr) mWinRadioPlayPtr->hideWnd();
    if (mWinRadioPtr) mWinRadioPtr->showWnd();
  }
}

/* ==================== 换台：**等引擎收完再起流** ====================
 * ★★ 2026-09-19 实测（血案 30 的收音机侧现场）：
 *   `radioStopEngine()` 里的 `stopForSwitch()` 只是**请求停止**，真正的收尾由解码线程
 *   异步做（实测一次要 **1500ms**，等的是"解码器放空"超时）。在这之前调
 *   `startDecodeTest()` 会被**直接拒掉**，而拒掉的表现是 `起流=失败` ⇒ 界面弹出
 *   **"打不开这个台"** —— 把"引擎正忙"当成了"这个台坏了"，误导用户。
 *   实测（`radio 31..34` 每隔 1s 连发）：**隔一次失败一次**
 *   （31 失败 → 收尾完成 → 32 成功 → 33 失败 → 34 成功），对着界面就是
 *   "快速连按两下『下一个』，第二下必报打不开"。
 * ⇒ 规则：收到换台请求时**只登记**，等 `running()` 变 false（或超过 kStopWaitMs 兜底）
 *   再真正起流。**不阻塞 UI 线程**（1500ms 的阻塞会让界面僵住）。
 *   这就是扫描路径一直在用的做法（见 scanGoNext/tickScan），这里把它补到正常换台上。 */
int sStartTo = -1;                 // 欠着的起流下标（-1 = 没有）
const int kStopWaitMs = 2500;      // 等引擎收尾的上限（实测收尾 ~1500ms，留足余量）

/** 真正起流（HLS / 直连两条路各一）。只由 radioPlayAt / pumpPendingStart 调。 */
void startStreamNow(int i) {
  const char *url = gSta[i].url;
  bool ok = false;
  char play[80] = {0};
  if (isHlsUrl(url)) {
    if (!pg::Hls::start(url, play, sizeof(play))) {
      LOGW("radioLogic: Hls::start 失败（%s）", url);
    } else {
      LOGD("radioLogic: HLS 中继已起 → %s", play);
      ok = pg::StreamPlayer::startDecodeTest(play, 0);
    }
  } else {
    ok = pg::StreamPlayer::startDecodeTest(url, 0);
  }
  gPhase = ok ? 1 : 3;
  gPhaseMs = nowMs();                // ★ 超时从**真正起流**那一刻算，不含等收尾的时间
  LOGD("radioLogic: 播放 #%d '%s' url='%s' 起流=%s", i, gSta[i].name, url, ok ? "成功" : "失败");
  if (!ok) {
    LOGW("radioLogic: 起流失败（引擎=%s，等收尾 %lldms）—— 地址不可达，或收尾还没完",
         pg::StreamPlayer::running() ? "仍忙" : "空闲",
         (long long)(nowMs() - gPhaseMs));
  }
}

/** 消费"欠着的起流"：等上一台收完（或超时兜底）再起。返回**真正起流的下标**，没起返回 -1。 */
int pumpPendingStart() {
  if (sStartTo < 0) return -1;
  if (pg::StreamPlayer::running() && nowMs() - gPhaseMs < kStopWaitMs) return -1;   // 还在收尾
  const int i = sStartTo;
  sStartTo = -1;
  startStreamNow(i);
  return i;
}

void radioPlayAt(int i) {
  if (i < 0 || i >= gCount) {
    LOGW("radioLogic: radioPlayAt(%d) 越界（共 %d 个）", i, gCount);   // 不静默失败
    return;
  }
  radioStopEngine();
  /* ★ 换台也要**立刻清频谱**（`radioStopTo` 一直有这句，换台这条路漏了）：
   *   快照保存的是"最后一块 PCM"的值 ⇒ 不清的话，旧台的柱子会一直挂在屏幕上；
   *   而新台要是**起流失败**（phase=3），那屏柱子就永远不消失 —— 看上去像"还在播"，
   *   正是本项目最忌讳的"静默失败"。VU 表针不用在这清（它有自己的回落弹道，见 vuTick）。 */
  pg::Viz::reset();
  gPlaying = i;
  gPhase = 1;                        // 先当"连接中"；真正起流后 startStreamNow 会定相位
  gPhaseMs = nowMs();
  gListCount = -1;                   // 强制刷列表（"播放中"标记变了）
  sStartTo = i;                      // ★ 只登记，等泵去起（见上面的说明）
  radioShowPlayWin(true);
}

void radioStopTo(bool back) {
  radioStopEngine();
  pg::Viz::reset();       // ★ 停了就得把频谱清零：快照是"最后一块 PCM"的值，不清会留一屏柱子
  sStartTo = -1;          // ★ 欠着的起流一起作废（否则停完还会自己爬起来）
  gPlaying = -1;
  gPhase = 0;
  gListCount = -1;
  radioShowPlayWin(false);
  if (back) {
    LOGD("radioLogic: 退出页面");
    EASYUICONTEXT->closeActivity("radioActivity");
  }
}

/* ==================== UI 同步 ==================== */

char gKeyBarCache[96] = {0};   // 播放页第 n/N 行

/* 直接改列表页状态行（tick / 按钮回调都要用 ⇒ 提前定义）。 */
void localSetStatus(const char *s) {
  snprintf(gListStatus, sizeof(gListStatus), "%s", s);
  if (mTextRadioStatusPtr) mTextRadioStatusPtr->setText(gListStatus);
}

void syncRadioList() {
  if (!mWinRadioPtr || !mWinRadioPtr->isWndShow()) return;
  /* 行数变了才 refreshListView（它会逐行回调 obtainListItemData ⇒ 每帧调会白白重绘）。
   * gListCount = -1 由各处"强制重刷"点置位。 */
  if (mListRadioPtr && gCount != gListCount) {
    gListCount = gCount;
    mListRadioPtr->refreshListView();
    LOGD("radioLogic: 电台列表已刷新（%d 行）", gCount);
  }
}

void syncRadioStatus() {
  if (gPhase == 3) {
    const char *hls = (gPlaying >= 0 && isHlsUrl(gSta[gPlaying].url)) ? pg::Hls::lastError() : "";
    char b[160];
    snprintf(b, sizeof(b), "打不开这个台：%s", (hls && hls[0]) ? hls : "地址不可达或格式不支持");
    setTextIfChanged(mTextRadioStatusPtr, gListStatus, sizeof(gListStatus), b);
    return;
  }
  char b[160];
  if (gPlaying >= 0 && gPhase == 2) {
    snprintf(b, sizeof(b), "正在播放：%s（共 %d 个台）", gSta[gPlaying].name, gCount);
  } else {
    snprintf(b, sizeof(b), "共 %d 个台 · 表：%s", gCount, kStaFile);
  }
  setTextIfChanged(mTextRadioStatusPtr, gListStatus, sizeof(gListStatus), b);
}

void syncRadioPlay() {
  if (!mWinRadioPlayPtr || !mWinRadioPlayPtr->isWndShow()) return;
  if (gPlaying < 0) {
    /* 没在播也把提示写全，免得停在一个空白页 */
    setTextIfChanged(mTextRadioPlayNamePtr, gPlayName, sizeof(gPlayName), "未在播放");
    setTextIfChanged(mTextRadioPlayStatePtr, gPlayState, sizeof(gPlayState), "已停止");
    setTextIfChanged(mTextRadioPlayInfoPtr, gPlayInfo, sizeof(gPlayInfo), "");
    setTextIfChanged(mTextRadioPlayTimePtr, gPlayTime, sizeof(gPlayTime), "");
    setTextIfChanged(mTextRadioPlayKeyBarPtr, gKeyBarCache, sizeof(gKeyBarCache), "");
    return;
  }
  setTextIfChanged(mTextRadioPlayNamePtr, gPlayName, sizeof(gPlayName), gSta[gPlaying].name);

  char st[96];
  if (gPhase == 1) snprintf(st, sizeof(st), "正在连接…");
  else if (gPhase == 2) snprintf(st, sizeof(st), "播放中");
  else if (gPhase == 3) snprintf(st, sizeof(st), "打不开这个台");
  else snprintf(st, sizeof(st), "已停止");
  if (gPhase == 3) {
    const char *e = isHlsUrl(gSta[gPlaying].url) ? pg::Hls::lastError() : "";
    if (e && e[0]) snprintf(st, sizeof(st), "打不开：%s", e);
  }
  setTextIfChanged(mTextRadioPlayStatePtr, gPlayState, sizeof(gPlayState), st);

  /* 副信息：分组 · 域名（HLS 时再带上分片/字节 —— 那是"链路还活着"的判据） */
  char info[192];
  char host[80];
  nameFromUrl(gSta[gPlaying].url, host, sizeof(host));
  if (isHlsUrl(gSta[gPlaying].url)) {
    snprintf(info, sizeof(info), "%s · %s · %d 分片", gSta[gPlaying].group, host,
             pg::Hls::segments());
  } else {
    snprintf(info, sizeof(info), "%s · %s", gSta[gPlaying].group, host);
  }
  setTextIfChanged(mTextRadioPlayInfoPtr, gPlayInfo, sizeof(gPlayInfo), info);

  /* 已播时长（大字，等宽观感） */
  long long pos = pg::StreamPlayer::positionMs();
  long long sec = (pos > 0) ? pos / 1000 : 0;
  char tm[32];
  snprintf(tm, sizeof(tm), "%02lld:%02lld", sec / 60, sec % 60);
  setTextIfChanged(mTextRadioPlayTimePtr, gPlayTime, sizeof(gPlayTime), tm);

  char kb[96];
  snprintf(kb, sizeof(kb), "%d / %d · %s", gPlaying + 1, gCount, gSta[gPlaying].group);
  setTextIfChanged(mTextRadioPlayKeyBarPtr, gKeyBarCache, sizeof(gKeyBarCache), kb);
}

void syncRadioViz();   // 定义在下面（频谱/电平）

/* 频谱组的三个符号**提前声明**（下面的 syncView 要用它们；定义在 syncRadioViz 那一节） */
int gEqDrawn[kEqBars];
int gLevelW = -1;
ZKTextView *eqBar(int i);

/* ==================== 视图切换：30 段频谱 ⇄ 双指针 VU 表 ====================
 * 用户需求（2026-09-19）：「频谱和 db 值部分做成两个左右声道的指针仪表效果」+
 *   「老的方式不要去掉，做个按键切换」⇒ 两组控件**同区域、按模式互相显隐**。
 * 默认视图 = **表盘**（新东西先给用户看）；QA `view 0|1` 可以切。 */
bool sViewVu = true;
int sVuAngL = -9999, sVuAngR = -9999;     // 上次下发的角度（变了才写）
char gVuDbL[16] = {0}, gVuDbR[16] = {0};

/** 指针角度：**与 `tools/gen_vu_art.py::db_to_ang()` 必须逐字一致**
 *  （-20dB → -52°、0dB → +16°、+3dB → +34°；0dB 落在量程末端约 80%，红区占最后 ~20%）。 */
int vuAngleDeg(int ch) {
  float db = pg::Viz::vuDb(ch);
  if (db < -20.0f) db = -20.0f;
  if (db > 3.0f) db = 3.0f;
  const float a = (db <= 0.0f) ? (-52.0f + (db + 20.0f) * 68.0f / 20.0f)
                               : (16.0f + db * 18.0f / 3.0f);
  return (int)(a + (a < 0 ? -0.5f : 0.5f));
}

/** 把当前视图落到控件上（显隐 + 页签两态）。切视图 / 进页 / 退页都要调。
 *  ⚠️ 隐藏的那半枚页签**必须同时 `setTouchable(false)`** —— 否则看不见但还能点。 */
void syncView() {
  const bool vu = sViewVu;
  if (mTabVuOnPtr) { mTabVuOnPtr->setVisible(vu); mTabVuOnPtr->setTouchable(false); }
  if (mTabSpecOnPtr) { mTabSpecOnPtr->setVisible(!vu); mTabSpecOnPtr->setTouchable(false); }
  if (mTabVuOffPtr) { mTabVuOffPtr->setVisible(!vu); mTabVuOffPtr->setTouchable(!vu); }
  if (mTabSpecOffPtr) { mTabSpecOffPtr->setVisible(vu); mTabSpecOffPtr->setTouchable(vu); }
  if (mVuFaceLPtr) mVuFaceLPtr->setVisible(vu);
  if (mVuFaceRPtr) mVuFaceRPtr->setVisible(vu);
  if (mVuNeedleLPtr) mVuNeedleLPtr->setVisible(vu);
  if (mVuNeedleRPtr) mVuNeedleRPtr->setVisible(vu);
  if (mTextVuDbLPtr) mTextVuDbLPtr->setVisible(vu);
  if (mTextVuDbRPtr) mTextVuDbRPtr->setVisible(vu);
  for (int i = 0; i < kEqBars; ++i) {
    ZKTextView *b = eqBar(i);
    if (b) b->setVisible(!vu);
  }
  if (mRpFloorPtr) mRpFloorPtr->setVisible(!vu);
  if (mRpLevelPtr) mRpLevelPtr->setVisible(!vu);
  if (mRpLevelBgPtr) mRpLevelBgPtr->setVisible(!vu);
  /* 切视图后强制重画（"变了才写"的缓存要作废，否则切回来的第一拍是旧几何） */
  gEqDrawn[0] = -1;
  gLevelW = -1;
  sVuAngL = sVuAngR = -9999;
  LOGD("radioLogic: 视图 -> %s", vu ? "双指针表（L/R）" : "30 段频谱");
}

/** 双指针表：每 25ms 一拍（40fps，和屏保页同口径 —— 20fps 的针会有"台阶感"）。
 *  指针由本页定时器驱动（`animatable=0`），角度直接用 dB 映射；**弹道在 PgViz 里做**。 */
void tickVu() {
  if (!sViewVu) return;
  if (!mWinRadioPlayPtr || !mWinRadioPlayPtr->isWndShow()) return;
  const int aL = vuAngleDeg(0);
  const int aR = vuAngleDeg(1);
  if (aL != sVuAngL) {
    sVuAngL = aL;
    if (mVuNeedleLPtr) mVuNeedleLPtr->setTargetAngle((float)aL);
  }
  if (aR != sVuAngR) {
    sVuAngR = aR;
    if (mVuNeedleRPtr) mVuNeedleRPtr->setTargetAngle((float)aR);
  }
}

/** dB 读数（写在表盘那个窗里）——**只在 100ms 那一拍刷**：文本重绘比指针贵，
 *  而 0.1dB 的跳动人眼也不需要 40fps。 */
void syncRadioVuText() {
  if (!sViewVu) return;
  if (!mWinRadioPlayPtr || !mWinRadioPlayPtr->isWndShow()) return;
  char b[16];
  const bool on = (gPlaying >= 0 && gPhase == 2);
  if (!on) {
    setTextIfChanged(mTextVuDbLPtr, gVuDbL, sizeof(gVuDbL), "--.-");
    setTextIfChanged(mTextVuDbRPtr, gVuDbR, sizeof(gVuDbR), "--.-");
    return;
  }
  snprintf(b, sizeof(b), "%+.1f", pg::Viz::vuDb(0));
  setTextIfChanged(mTextVuDbLPtr, gVuDbL, sizeof(gVuDbL), b);
  snprintf(b, sizeof(b), "%+.1f", pg::Viz::vuDb(1));
  setTextIfChanged(mTextVuDbRPtr, gVuDbR, sizeof(gVuDbR), b);
}

void syncRadio() {
  syncRadioList();
  syncRadioStatus();
  syncRadioPlay();
  syncRadioViz();
  syncRadioVuText();
}

/* ==================== 播放页频谱（真 FFT） ====================
 * 数据源 `pg::Viz`：在 **PgAudio 灌喇叭的那条路**上做 512 点 FFT（见 PgViz.h）⇒
 * 频谱与耳朵听到的必然一致（没出声就不动，不存在"假动画"）。
 *
 * ★★ 2026-09-18 追加（用户需求：「音频的幅度跟随系统声音调整最高幅度」）：
 *   **取点在 PCM 出口 ⇒ 音量是"下游"做的** —— 本板音量写的是 codec 的**数字音量**，
 *   PCM 数据本身**不含音量信息**（这也是为什么"静音后频谱不动"能成立）。
 *   于是"把音量调小、柱子还顶到天花板"看起来就不对。
 *   ⇒ 显示侧**自己乘一个音量系数**（`vizAmpScale`），让**最大幅度跟着系统音量走**：
 *        音量 100% → 系数 255（满格）；音量 30% → 系数 76（柱顶约 1/3）；
 *        **静音 → 系数 0**（整排只剩脚底那条短桩）。
 *   ⚠️ 这是**显示侧**的系数，不改 PgViz 的原始测量值（QA `viz` 同时打出两者，便于对照）。
 *   ⚠️ "连接中"的等待扫描**不乘这个系数** —— 它表示"正在连"，不是音频幅度；
 *      否则音量调小之后，等待动画会比播放时的柱子还高，看着像坏了。
 *
 * 两条纪律（都是本项目踩过的）：
 *   ① **变了才 setPosition** —— 每帧无脑写 30 个控件 = 重绘风暴（屏保页的教训）；
 *   ② 连接中/停止时的"等待动画"必须**由绝对时钟推导**（相位不漂），
 *      不许用"累加 dt / 帧计数"（宠物页冻帧两次）。 */
bool sVizFollow = true;       // （gEqDrawn / gLevelW 的声明见上面的"提前声明"）       // 幅度是否跟随系统音量（QA `vizfollow 0|1` 可关，便于对照）

/* 显示幅度系数 0..255（跟随系统音量；静音 = 0）。
 * 取不到音量时按"满"处理 —— 宁可显示满格，也别让频谱整屏消失（静默失败）。 */
int vizAmpScale() {
  if (!sVizFollow) return 255;
  if (pg::isMutedGlobal()) return 0;
  pg::Host *h = pg::pgHost();
  const int pct = h ? h->volumePercent() : -1;
  if (pct < 0) return 255;
  if (pct > 100) return 255;
  return pct * 255 / 100;
}

/* 30 根柱子按序号取控件（生成名的转发表；captions 改了这里必须跟着改） */
ZKTextView *eqBar(int i) {
  static ZKTextView **tab = 0;
  static ZKTextView *t[kEqBars];
  if (!tab) {
    t[0] = mEqB00Ptr;  t[1] = mEqB01Ptr;  t[2] = mEqB02Ptr;  t[3] = mEqB03Ptr;
    t[4] = mEqB04Ptr;  t[5] = mEqB05Ptr;  t[6] = mEqB06Ptr;  t[7] = mEqB07Ptr;
    t[8] = mEqB08Ptr;  t[9] = mEqB09Ptr;  t[10] = mEqB10Ptr; t[11] = mEqB11Ptr;
    t[12] = mEqB12Ptr; t[13] = mEqB13Ptr; t[14] = mEqB14Ptr; t[15] = mEqB15Ptr;
    t[16] = mEqB16Ptr; t[17] = mEqB17Ptr; t[18] = mEqB18Ptr; t[19] = mEqB19Ptr;
    t[20] = mEqB20Ptr; t[21] = mEqB21Ptr; t[22] = mEqB22Ptr; t[23] = mEqB23Ptr;
    t[24] = mEqB24Ptr; t[25] = mEqB25Ptr; t[26] = mEqB26Ptr; t[27] = mEqB27Ptr;
    t[28] = mEqB28Ptr; t[29] = mEqB29Ptr;
    tab = t;
  }
  return (i >= 0 && i < kEqBars) ? tab[i] : 0;
}

const int kEqX0 = 2;      // 与 ui/radio.html 的 EqB00 的 x 一致
const int kEqStep = 16;   // 柱间距（柱宽 12 + 间隔 4）

void syncRadioViz() {
  if (!mWinRadioPlayPtr || !mWinRadioPlayPtr->isWndShow()) return;
  if (sViewVu) return;                    // 表盘视图下这组控件是隐藏的，不用算
  uint8_t v[kEqBars];
  const int amp = vizAmpScale();          // 0..255：跟随系统音量的显示幅度系数
  if (gPhase == 2) {
    pg::Viz::snapshot(v, kEqBars);
    /* ★ 真数据为 0（比如纯静音的片头）时给一个**很低**的底噪，免得整屏像"死了"。 */
    for (int i = 0; i < kEqBars; ++i) {
      if (v[i] < 6) v[i] = 6;
      v[i] = (uint8_t)((int)v[i] * amp / 255);   // ← 幅度跟音量（静音时整排归零）
    }
  } else if (gPhase == 1) {
    /* 还没出声：一道来回扫描的"等待"波形（绝对时钟推导：相位不会漂）。
     * ★ 不乘 amp —— 它是"正在连"的状态提示，不是音频幅度。 */
    const long long ph = nowMs() % 2400;
    const int pos = (int)((ph < 1200) ? (ph * (kEqBars - 1) / 1200)
                                      : ((2400 - ph) * (kEqBars - 1) / 1200));
    for (int i = 0; i < kEqBars; ++i) {
      const int d = (i > pos) ? (i - pos) : (pos - i);
      v[i] = (d < 4) ? (uint8_t)(150 - d * 30) : 10;
    }
  } else {
    memset(v, 0, sizeof(v));
  }
  for (int i = 0; i < kEqBars; ++i) {
    int h = 8 + (int)((long long)v[i] * (kEqMaxH - 8) / 255);
    if (h == gEqDrawn[i]) continue;              // 变了才写（纪律 ①）
    gEqDrawn[i] = h;
    ZKTextView *b = eqBar(i);
    if (b) b->setPosition(LayoutPosition(kEqX0 + i * kEqStep, kEqBottom - h, 12, h));
  }
  /* 总电平条（按宽度改尺寸；素材 320x8 == 控件盒）—— 同样跟音量走 */
  int lv = (gPhase == 2) ? pg::Viz::level() : 0;
  lv = lv * amp / 255;
  int lw = 4 + (int)((long long)lv * (320 - 4) / 255);
  if (lw != gLevelW) {
    gLevelW = lw;
    if (mRpLevelPtr) mRpLevelPtr->setPosition(LayoutPosition(80, 454, lw, 8));
  }
}

/* ==================== 电台表"真机实测"扫描（QA radioscan） ====================
 * 为什么要在**设备上**再测一遍：PC 侧能连上 ≠ 板子能放出声音（DNS/IPv6/TLS 握手/解码器
 * 支持度都不一样，本项目在 RTSP 上就栽过"探测通、播放不通"）。所以"这台能不能听"
 * 一律**以真机为准** —— 而且判据不是"能打开"，是**位置毫秒真的在走**（= 真的出声了）。
 *
 * 做法就是**逐台走真实播放路径**（radioPlayAt）—— 不另写一套探测代码：
 *   QA 钩子必须与真实操作同一条路，否则"验过的"和"用户点的"不是一回事。
 *
 * 结果写在 gSta[i].ok（列表页会显示），并落盘 /data/radio_scan.txt（可回溯）。 */
bool sScanOn = false;
bool sScanWaiting = false;      // 正在**等引擎收完**（stopForSwitch 的收尾是异步的）
int sScanRetry = 0;             // 本台因"引擎正忙"重试了几次
int sScanIdx = 0;
int sScanTotal = 0;
int sScanOk = 0;
int sScanFail = 0;
long long sScanPhaseMs = 0;
/* 单台最长等待（超过就当"打不开"）。
 * ★ 默认从 7000 提到 **9000**：实测有台"慢但能播"的（蜻蜓的 `湖北经典音乐` 要 **7.2s** 才出声），
 *   7s 窗口会把它误判成打不开 —— **评估工具本身不能把"慢"当成"坏"**。 */
int sScanPerMs = 9000;
char sScanNote[80] = {0};

void scanWriteReport() {
  FILE *f = fopen("/data/radio_scan.txt", "w");
  if (!f) {
    LOGW("radioLogic radioscan: 打不开 /data/radio_scan.txt，结果只在日志里");
    return;
  }
  fprintf(f, "# 网络收音机真机实测（radioscan）：能播 %d / 打不开 %d / 共 %d\n",
          sScanOk, sScanFail, sScanTotal);
  fprintf(f, "# 每行 状态|名称|地址\n");
  for (int i = 0; i < gCount; ++i) {
    fprintf(f, "%s|%s|%s\n", gSta[i].ok == 1 ? "OK" : (gSta[i].ok == 0 ? "FAIL" : "?"),
            gSta[i].name, gSta[i].url);
  }
  fclose(f);
  LOGD("radioLogic radioscan: 报告已写 /data/radio_scan.txt");
}

void scanFinish() {
  sScanOn = false;
  radioStopEngine();
  gPlaying = -1;
  gPhase = 0;
  radioShowPlayWin(false);
  gListCount = -1;
  LOGD("radioLogic radioscan: ==== 结束：能播 %d / 打不开 %d / 共 %d ====",
       sScanOk, sScanFail, sScanTotal);
  scanWriteReport();
  {
    char b[160];
    snprintf(b, sizeof(b), "实测完成：能播 %d / 打不开 %d / 共 %d 个台",
             sScanOk, sScanFail, sScanTotal);
    localSetStatus(b);
  }
  syncRadio();
}

void scanStart(int perMs) {
  sScanPerMs = (perMs >= 2000 && perMs <= 30000) ? perMs : 9000;
  sScanIdx = 0;
  sScanTotal = gCount;
  sScanOk = sScanFail = 0;
  sScanRetry = 0;
  for (int i = 0; i < gCount; ++i) gSta[i].ok = -1;
  sScanOn = (gCount > 0);
  LOGD("radioLogic radioscan: 开始实测 %d 个台（单台上限 %dms，判据=位置毫秒在走）",
       sScanTotal, sScanPerMs);
  if (sScanOn) {
    /* ★★ 起手先停引擎、**等它真的收完**再起第一台。
     * 血案（2026-09-18）：`stopForSwitch()` 只是"请求停止"，收尾是解码线程异步做的；
     * 连续 stop→start 会被 `startDecodeTest` 直接拒掉 ⇒ **整表 61 台全报"打不开"**
     * （每台 0.1s 就出错），把"引擎正忙"当成了"这个台坏了"。
     * ⇒ 规则：**起流前必须先确认 `running()==false`**（见 tickScan 的 sScanWaiting）。 */
    radioStopEngine();
    sStartTo = -1;             // ★ 清掉手动播放欠下的起流请求，别让它插进扫描序列
    sScanWaiting = true;
    sScanPhaseMs = nowMs();
  }
  syncRadio();
}

/** 记完账 → 停引擎 → 进"等空闲"状态（下一拍起下一台）。 */
void scanGoNext() {
  radioStopEngine();
  pg::Viz::reset();
  gPlaying = -1;
  gPhase = 0;
  ++sScanIdx;
  sScanRetry = 0;
  syncRadio();
  if (sScanIdx >= sScanTotal) {
    scanFinish();
    return;
  }
  sScanWaiting = true;      // 等引擎收完（tickScan 会看 running()）
  sScanPhaseMs = nowMs();
}

/* 每拍推进（在 tickRadio 之前调）：等空闲 → 起流 → 等"出声" → 记账 → 下一台 */
void tickScan() {
  if (!sScanOn) return;
  /* 扫描期间不走 tickRadio() ⇒ 欠着的起流要在这里泵掉。
   * 计时也改到"真的起流"那一刻（否则等收尾的 1.5s 会从单台窗口里白扣）。 */
  if (pumpPendingStart() >= 0) sScanPhaseMs = nowMs();
  if (sScanWaiting) {
    /* 等解码线程真的收完（最多 2s，避免死等把整轮卡住）。 */
    if (pg::StreamPlayer::running() && nowMs() - sScanPhaseMs < 2000) return;
    sScanWaiting = false;
    LOGD("radioLogic radioscan: [%d/%d] 起流 '%s'（引擎已空闲）",
         sScanIdx + 1, sScanTotal, gSta[sScanIdx].name);
    radioPlayAt(sScanIdx);
    sScanPhaseMs = nowMs();
    return;
  }
  if (gPlaying != sScanIdx) {          // 还没起（或上一台被停掉）→ 起这一台
    radioPlayAt(sScanIdx);
    sScanPhaseMs = nowMs();
    return;
  }
  const long long el = nowMs() - sScanPhaseMs;
  /* ★★ 相位推进必须在这里也做一遍：扫描期间**不走 tickRadio()**（onUI_Timer 里是二选一），
   * 而"真的出声了"这件事正是 tickRadio 用 `positionMs() > 0` 判出来、把 1 抬成 2 的。
   * 少了这一句 ⇒ gPhase 永远停在"连接中"⇒ 每台都"等出声超时"
   * （2026-09-18 血案：第一版整表 61 台全报打不开，而同一台手动 `radio 4` 明明在出声）。 */
  if (gPhase == 1) {
    if (pg::StreamPlayer::positionMs() > 0) {
      gPhase = 2;
      LOGD("radioLogic radioscan: #%d '%s' 已出声（位置 %lldms）", sScanIdx,
           gSta[sScanIdx].name, (long long)pg::StreamPlayer::positionMs());
    } else if (isHlsUrl(gSta[sScanIdx].url) && pg::Hls::openState() == 2) {
      gPhase = 3;
      LOGW("radioLogic radioscan: #%d HLS 清单解析失败：%s", sScanIdx, pg::Hls::lastError());
    }
  }
  if (gPhase == 2) {
    gSta[sScanIdx].ok = 1;
    ++sScanOk;
    LOGD("radioLogic radioscan: [%d/%d] #%d '%s' = 能播（%.1fs 出声）",
         sScanIdx + 1, sScanTotal, sScanIdx, gSta[sScanIdx].name, el / 1000.0);
    snprintf(sScanNote, sizeof(sScanNote), "实测中 %d/%d · 能播 %d",
             sScanIdx + 1, sScanTotal, sScanOk);
    scanGoNext();
    return;
  }
  if (gPhase == 3 || el > sScanPerMs) {
    const bool refused = (gPhase == 3 && el < 400);
    if (refused && sScanRetry < 3) {
      ++sScanRetry;
      LOGW("radioLogic radioscan: #%d '%s' 起流被拒（%lldms）→ 等引擎空闲后重试第 %d 次",
           sScanIdx, gSta[sScanIdx].name, el, sScanRetry);
      radioStopEngine();
      sScanWaiting = true;
      sScanPhaseMs = nowMs();
      return;
    }
    gSta[sScanIdx].ok = 0;
    ++sScanFail;
    LOGW("radioLogic radioscan: [%d/%d] #%d '%s' = 打不开（%.1fs，%s）",
         sScanIdx + 1, sScanTotal, sScanIdx, gSta[sScanIdx].name, el / 1000.0,
         (gPhase == 3) ? "相位=出错" : "等出声超时");
    snprintf(sScanNote, sizeof(sScanNote), "实测中 %d/%d · 能播 %d 打不开 %d",
             sScanIdx + 1, sScanTotal, sScanOk, sScanFail);
    scanGoNext();
    return;
  }
  /* 还在等出声 */
}

/* ==================== 定时器里推进相位机（只改状态，控件都在 sync* 里刷） ==================== */
void tickRadio() {
  if (gPlaying < 0) return;
  pumpPendingStart();          // ★ 先把"欠着的起流"结掉（等上一台收完）
  if (gPhase == 1) {
    long long el = nowMs() - gPhaseMs;
    if (pg::StreamPlayer::positionMs() > 0) {
      gPhase = 2;
      LOGD("radioLogic: #%d '%s' 已出声（位置 %lldms，起流后 %lldms）", gPlaying,
           gSta[gPlaying].name, (long long)pg::StreamPlayer::positionMs(), el);
    } else if (isHlsUrl(gSta[gPlaying].url) && pg::Hls::openState() == 2) {
      gPhase = 3;
      LOGW("radioLogic: HLS 清单解析失败：%s", pg::Hls::lastError());
    } else if (el > kConnTimeoutMs) {
      gPhase = 3;
      LOGW("radioLogic: 起播超时 %lldms（HLS=%s，流=%d）", el, pg::Hls::status(),
           (int)pg::StreamPlayer::running());
    }
  } else if (gPhase == 2) {
    if (!pg::StreamPlayer::running()) {
      /* 直播源一般不会自己结束；到这儿说明网络断了或服务端关了。
       * 回列表页显示原因，而不是停在一个"看着像在播"的页面。 */
      gPlaying = -1;
      gPhase = 0;
      radioShowPlayWin(false);
      localSetStatus("播放已结束（网络中断或服务端关闭）");
      LOGW("radioLogic: 流已结束（running=0）");
    }
  }
}

/* ==================== 自检通道 /tmp/pg_radiocmd ==================== */
char sQaTag[512] = {0};

/* 前置声明**必须在命名空间作用域**（不是写进函数体里！）。
 * ★ 血案 2026-09-16：把 `void radioQuitAndCleanup();` 写在 `handleRadioCmd` 函数体里，
 *   编译期没问题、**链接期报 `undefined reference to '(anonymous namespace)::radioQuitAndCleanup()'`**
 *   —— 块作用域的函数声明按标准算"外部链接"，与匿名命名空间里的内部链接定义**不是同一个实体**。
 *   cameraLogic.cc 顶部那条同名前置声明就是正确写法（照抄它的位置）。 */
void radioQuitAndCleanup();

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_radiocmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

void qaHint() {
  LOGD("radioLogic radiohint: 表文件=%s（每行 名称|地址，# 开头是注释）；"
       "命令 radio <n> / radiolist / radiostat / radiosave 名称|地址 / "
       "radioscan [每台ms]（真机实测整表）/ radioscanstat / radioscanstop / "
       "viz（频谱+双声道电平快照，同时**写 /data/radio_viz.txt**）/ vizgain <dB> / "
       "vizfollow 0|1 / view 0|1（频谱/表盘）/ vu0 <dBFS>（VU 表 0VU 标定）/ vol <0..100> / quit",
       kStaFile);
}

void radioDebugList() {
  LOGD("radioLogic radiolist: 共 %d 条（%s）", gCount, kStaFile);
  for (int i = 0; i < gCount; ++i) {
    LOGD("radioLogic radiolist: #%d '%s' [%s] %s 实测=%s", i, gSta[i].name, gSta[i].group,
         gSta[i].url, gSta[i].ok == 1 ? "能播" : (gSta[i].ok == 0 ? "打不开" : "未测"));
  }
}

/* 频谱/电平的快照（逐台看图之前先看这个数；`viz` 是播放页动画的"数字证据"）
 * ★ 同时打出"显示侧"的三件东西：系统音量 / 幅度系数 / **实际柱高(px)** ——
 *   验收"幅度跟随系统音量"就是比这两组柱高：音量 100 与 30 时柱高应该接近 10:3。 */
void radioDebugViz() {
  uint8_t v[pg::kVizBands];
  const int n = pg::Viz::snapshot(v, pg::kVizBands);
  const int amp = vizAmpScale();
  char b[256], hb[256];
  int p = 0, hp = 0;
  for (int i = 0; i < n && p < (int)sizeof(b) - 6; ++i) {
    p += snprintf(b + p, sizeof(b) - p, "%s%d", i ? "," : "", v[i]);
    const int h = 8 + (int)((long long)((int)v[i] * amp / 255) * (kEqMaxH - 8) / 255);
    hp += snprintf(hb + hp, sizeof(hb) - hp, "%s%d", i ? "," : "", h);
  }
  pg::Host *h = pg::pgHost();
  LOGD("radioLogic viz: 相位=%d 音量=%d%% 静音=%d 跟随=%d 系数=%d/255 电平=%d 段数=%d "
       "FFT次数=%lld 已灌帧=%lld",
       gPhase, h ? h->volumePercent() : -1, (int)pg::isMutedGlobal(), (int)sVizFollow, amp,
       pg::Viz::level(), n, pg::Viz::fftCount(), pg::Viz::framesFed());
  LOGD("radioLogic viz: 原始段=[%s]", b);
  LOGD("radioLogic viz: 柱高px=[%s]", hb);
  /* ★ 双指针表的数字证据：左右 RMS/电平/相对 0VU 的 dB/指针角度（见 PgViz 的 VU 弹道） */
  LOGD("radioLogic vu: 视图=%s 0VU=%.0fdBFS L[电平=%d dB=%+.1f 角=%d°] R[电平=%d dB=%+.1f 角=%d°]",
       sViewVu ? "表盘" : "频谱", pg::Viz::vuZeroDbfs(), pg::Viz::vuLevel(0), pg::Viz::vuDb(0),
       vuAngleDeg(0), pg::Viz::vuLevel(1), pg::Viz::vuDb(1), vuAngleDeg(1));

  /* ★★ 同一份快照**再写一份文件**（`/data/radio_viz.txt`，单行、每次覆盖）。
   *   为什么要有它：本板的日志服务很脆 —— 被 SSDP 之类的噪声刷爆后**整个日志服务会卡住**
   *   （`adb logcat` 从此收不到新行），而"标定 VU 表零点 / 验收指针角度"恰恰依赖这组数字。
   *   读一个文件比抢日志稳得多，也不会因为缓冲区滚动而丢样本。
   *   ⚠️ 只在 QA 调用 `viz` 时写（**不在每拍 tick 里写**）：`/data` 在 NAND 上，别为了
   *      诊断去磨闪存。（这也是"快照 = 按需"而不是"持续落盘"的原因。） */
  if (gPlaying >= 0 && gPlaying < gCount) {
    FILE *f = fopen("/data/radio_viz.txt", "w");
    if (f) {
      fprintf(f, "idx=%d name=%s phase=%d vu0=%.1f\n"
                 "L level=%d db=%+.1f ang=%d\nR level=%d db=%+.1f ang=%d\n"
                 "spec_level=%d fft=%lld frames=%lld vol=%d%% amp=%d/255\n",
              gPlaying, gSta[gPlaying].name, gPhase, pg::Viz::vuZeroDbfs(),
              pg::Viz::vuLevel(0), pg::Viz::vuDb(0), vuAngleDeg(0),
              pg::Viz::vuLevel(1), pg::Viz::vuDb(1), vuAngleDeg(1),
              pg::Viz::level(), pg::Viz::fftCount(), pg::Viz::framesFed(),
              h ? h->volumePercent() : -1, amp);
      fclose(f);
    } else {
      LOGW("radioLogic viz: 写 /data/radio_viz.txt 失败（快照只在日志里）");
    }
  }
}

void handleRadioCmd(const char *line) {
  if (strncmp(line, "radiolist", 9) == 0) {
    radioDebugList();
    return;
  }
  if (strncmp(line, "radiostat", 9) == 0) {
    LOGD("radioLogic radiostat: 相位=%d 下标=%d 位置=%lldms 流=%d HLS[%s 分片=%d 字节=%d] 内存=%lldkB",
         gPhase, gPlaying, (long long)pg::StreamPlayer::positionMs(),
         (int)pg::StreamPlayer::running(), pg::Hls::status(), pg::Hls::segments(),
         pg::Hls::bytes(), (long long)pg::StreamPlayer::memAvailableKb());
    return;
  }
  if (strncmp(line, "radiohint", 9) == 0) {
    qaHint();
    return;
  }
  /* ---- 真机实测整张表 ---- */
  if (strncmp(line, "radioscanstop", 13) == 0) {
    if (sScanOn) {
      sScanOn = false;
      LOGD("radioLogic radioscan: 手动停止（已测 %d 台，能播 %d）", sScanIdx, sScanOk);
      scanWriteReport();
    }
    return;
  }
  if (strncmp(line, "radioscanstat", 13) == 0) {
    LOGD("radioLogic radioscanstat: 运行=%d 进度=%d/%d 能播=%d 打不开=%d 当前=#%d(相位%d)",
         (int)sScanOn, sScanIdx, sScanTotal, sScanOk, sScanFail, gPlaying, gPhase);
    return;
  }
  if (strncmp(line, "radioscan", 9) == 0) {
    const int per = (strlen(line) > 9) ? atoi(line + 9) : 0;
    scanStart(per);
    return;
  }
  /* ---- 播放页频谱（真 FFT）自检 ---- */
  if (strncmp(line, "vizgain ", 8) == 0) {
    const float db = (float)atof(line + 8);
    pg::Viz::setGainDb(db);
    LOGD("radioLogic vizgain: 显示增益 -> %.0fdB", pg::Viz::gainDb());
    return;
  }
  /* 幅度是否跟随系统音量（默认开）。关掉便于做"同一段音乐、同一音量"的对照实验。 */
  if (strncmp(line, "vizfollow", 9) == 0) {
    const int on = (strlen(line) > 9) ? atoi(line + 9) : 1;
    sVizFollow = (on != 0);
    gEqDrawn[0] = -1;                 // 强制下一拍重画（系数变了，柱子要立刻跟上）
    LOGD("radioLogic vizfollow: 幅度跟随系统音量 -> %s（系数 %d/255）",
         sVizFollow ? "开" : "关", vizAmpScale());
    syncRadio();
    return;
  }
  /* VU 表标定：0 VU 对应多少 dBFS（默认 -18）。电台流普遍偏轻，现场用 `vu` 看实际值再调。 */
  if (strncmp(line, "vu0 ", 4) == 0) {
    pg::Viz::setVuZeroDbfs((float)atof(line + 4));
    LOGD("radioLogic vu0: 0VU -> %.1fdBFS（当前 L=%+.1f R=%+.1f）", pg::Viz::vuZeroDbfs(),
         pg::Viz::vuDb(0), pg::Viz::vuDb(1));
    return;
  }
  /* 视图切换（0 = 30 段频谱 / 1 = 双指针表）—— 也是验收"两组控件显隐对不对"的钩子 */
  if (strncmp(line, "view", 4) == 0) {
    const int v = (strlen(line) > 4) ? atoi(line + 4) : 1;
    sViewVu = (v != 0);
    syncView();
    syncRadio();
    LOGD("radioLogic view: 视图=%d（%s）", (int)sViewVu, sViewVu ? "双指针表" : "频谱");
    return;
  }
  /* 本页也能改系统音量 —— 一是方便现场验"幅度跟着走"，二是别让人为了调音量退出去。 */
  if (strncmp(line, "vol ", 4) == 0) {
    int vv = atoi(line + 4);
    if (vv < 0) vv = 0;
    if (vv > 100) vv = 100;
    const int got = pg::setVolumePercentGlobal(vv);
    gEqDrawn[0] = -1;
    LOGD("radioLogic vol: 设 %d%% -> 实际 %d%%（幅度系数 %d/255）", vv, got, vizAmpScale());
    syncRadio();
    return;
  }
  if (strncmp(line, "viz", 3) == 0) {
    radioDebugViz();
    return;
  }
  if (strncmp(line, "radiosave ", 10) == 0) {
    char buf[320];
    snprintf(buf, sizeof(buf), "%s", line + 10);
    char *sep = strchr(buf, '|');
    const char *nm = "自定义";
    const char *ur = buf;
    if (sep) {
      *sep = 0;
      nm = buf;
      ur = sep + 1;
    }
    if (radioAppendStation(nm, ur)) {
      radioLoadStations();
      gListCount = -1;
      syncRadio();
    }
    return;
  }
  if (strncmp(line, "radio ", 6) == 0 || strcmp(line, "radio") == 0) {
    int n = (strlen(line) > 6) ? atoi(line + 6) : 0;
    if (n < 0) {
      radioStopTo(false);
      syncRadio();
      LOGD("radioLogic: radio 停止");
      return;
    }
    LOGD("radioLogic: radio 播放第 %d 个", n);
    radioPlayAt(n);
    syncRadio();
    return;
  }
  /* `quit` / `exit`：等价于点左上角返回箭头（也等于长按返回键）—— **脚本化验收的收尾**。
   * 本页是独立 ftu/Activity，主界面那条 `/tmp/pg_autostart` 管不到它；与返回箭头**同一个函数**
   * `radioQuitAndCleanup()`（停引擎 + closeActivity）⇒ QA 退出与人工退出收尾一致。 */
  if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
    LOGD("radioLogic: QA quit -> 退出该页（= 点返回箭头）");
    radioQuitAndCleanup();
    return;
  }
  LOGD("radioLogic QA: 未知命令 '%s'", line);
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_radiocmd", "r");
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
    char *h = strchr(line, '#');          // 行内注释（写 `radio 0 #1` 保证内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    handleRadioCmd(line);
  }
}

/* ==================== 物理按键 ==================== */
class RadioKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键）。必须在第一句 —— 框架按键分发是
     *   短路式的，本页若先返回 true，屏保页自己的监听器可能收不到按键。 */
    if (pg::wakeSaverByKey(ke)) return true;
    long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = t;
      sLPFired = false;
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;
    int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= 700 && !sLPFired) {
      LOGD("radioLogic: 长按 %dms -> 退出", held);
      radioStopTo(true);
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
      int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("radioLogic: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      return true;
    }
    if (ke.mKeyCode == 108) {
      /* 播放页短按 = 停止回列表；列表页短按 = 退出。 */
      if (gPlaying >= 0) {
        radioStopTo(false);
        syncRadio();
      } else {
        radioStopTo(true);
      }
      return true;
    }
    return true;
  }

  /** ★ 长按达标**立刻**返回，不等抬手（本板 gpio-keys 没有 autorepeat）。 */
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
int RadioKeys::sDownCode = -1;
long RadioKeys::sDownMs = 0;
bool RadioKeys::sLPFired = false;
RadioKeys sKeys;

void radioQuitAndCleanup() {
  sScanOn = false;            // 实测扫描中途退出 → 停掉（引擎在下面一起收）
  radioStopEngine();
  sStartTo = -1;      // ★ 欠着的起流一起作废
  pg::Viz::reset();           // 频谱归零：下次进来别把上次的柱子留在屏幕上
  gPlaying = -1;
  gPhase = 0;
  EASYUICONTEXT->closeActivity("radioActivity");
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

const int TIMER_TICK = 1;
const int TICK_MS = 100;
const int TIMER_VU = 2;      // ★ 指针表专用：25ms（40fps）—— 20fps 的针能看出台阶
const int VU_MS = 25;

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_TICK, TICK_MS},
    {TIMER_VU, VU_MS},
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  /* ⚠️ 起播前先把 ffmpeg 的按需注册做掉（PgStream 内部也会 ensureReady，这里是为了
   *    把"首次初始化"的耗时挪到进页那一刻，而不是用户点第一台的时候）。 */
  pg::Ff::ensureReady();
  int n = radioLoadStations();
  LOGD("radioLogic: 电台表 %d 个 -> 列表页", n);
  gListCount = -1;
  syncView();          // ★ 视图（频谱/表盘）显隐 —— 必须在 syncRadio 之前
  syncRadio();
  /* 首屏把"表在哪/怎么加台"直接写在状态行上 —— 免读文档 */
  localSetStatus("正在加载电台表…");
  qaSyncTag();                    // 自检基线：只执行"进页之后新推的"命令
  qaHint();
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("radioLogic: onUI_show");
  /* 独立 ftu 必须自己管屏保（主界面不在前台时它的策略不跑，30 秒就会盖上来）。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
  gListCount = -1;
  syncView();
  syncRadio();
}

static void onUI_hide() { LOGD("radioLogic: onUI_hide"); }

static void onUI_quit() {
  LOGD("radioLogic: onUI_quit");
  sScanOn = false;
  radioStopEngine();              // 离开页面必须停流（本板 55MB 内存，留着下一个应用必炸）
  pg::Viz::reset();
  gPlaying = -1;
  gPhase = 0;
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  EASYUICONTEXT->resetScreensaverTimeOut();
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&sKeys);
#endif
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) {
  if (id == TIMER_VU) {          // ★ 只干一件事：把指针推到当前电平（40fps）
    tickVu();
    return true;
  }
  if (id != TIMER_TICK) return false;
  /* ★ 响铃期间**让位**（与 iptv/probe 一致）：本页是全屏独立页，留着就看不到主界面的
   *   "闹钟提醒页"（提醒页在 main.ftu，由主循环弹）。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("radioLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    radioQuitAndCleanup();
    return false;
  }
  if (RadioKeys::longPressReady()) {   // 长按达标即退出，不等抬手
    radioStopTo(true);
    return false;
  }
  qaPoll();
  /* ★ 表针回落（100ms 一拍，与音频无关）：停播/换台/打不开时**解码线程不再喂电平**，
   *   不主动回落的话指针会冻在最后一个值上（实测：出错相位下僵在 -4.4dB 不动，
   *   看着像"表坏了"）。内部有 400ms 静音保持 + 回落弹道，幂等，可以无脑调。 */
  pg::Viz::vuTick();
  if (sScanOn) {
    tickScan();          // 实测扫描期间由它驱动（它自己会起流/换台）
    syncRadio();
    return true;
  }
  tickRadio();
  syncRadio();
  return true;
}

static bool onradioActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;   // 全部放行：本页都是原生按钮
}

/* ==================================================================
 *  按钮 / 列表回调（必须留在文件作用域 static：INIT_UI_EVENT_BINDINGS 会在文件顶部
 *  声明它们，放进匿名 namespace 会变成另一个实体 → 链接错）
 * ================================================================== */

static bool onButtonClick_BtnRadioBack(ZKButton *p) {
  (void)p;
  LOGD("radioLogic: 点返回");
  radioStopTo(true);
  return true;
}

static bool onButtonClick_BtnRadioReloadTop(ZKButton *p) {
  (void)p;
  radioStopTo(false);
  int n = radioLoadStations();
  gListCount = -1;
  syncRadio();
  LOGD("radioLogic: 重新加载电台表 → %d 个", n);
  return true;
}

static bool onButtonClick_BtnRadioReload(ZKButton *p) {
  (void)p;
  int n = radioLoadStations();
  gListCount = -1;
  syncRadio();
  LOGD("radioLogic: 重新加载电台表 → %d 个", n);
  return true;
}

static bool onButtonClick_BtnRadioEdit(ZKButton *p) {
  (void)p;
  /* 不做"界面里编辑文本"：本页没有 EditText/键盘（IME 是 SysApp，拉起来会把下半屏
   * 全盖住，本页也没有"保存"落点）。直接给用户可操作的路径 + 一条现成命令。 */
  LOGD("radioLogic: 电台表在 %s（每行 名称|地址）；也可用 QA: radiosave 名称|地址", kStaFile);
  char b[160];
  snprintf(b, sizeof(b), "表：%s · 每行 名称|地址", kStaFile);
  localSetStatus(b);
  return true;
}

static bool onButtonClick_BtnRadioPlayBack(ZKButton *p) {
  (void)p;
  LOGD("radioLogic: 播放页点返回（回列表）");
  radioStopTo(false);
  syncRadio();
  return true;
}

/* ---- 视图切换页签（两枚，各"亮/暗"两态叠同一格；可点的是**暗的那枚**）---- */
static bool onButtonClick_TabVuOff(ZKButton *p) {
  (void)p;
  if (sViewVu) return true;
  LOGD("radioLogic: 切到「表盘」视图（点页签）");
  sViewVu = true;
  syncView();
  syncRadio();
  return true;
}

static bool onButtonClick_TabSpecOff(ZKButton *p) {
  (void)p;
  if (!sViewVu) return true;
  LOGD("radioLogic: 切到「频谱」视图（点页签）");
  sViewVu = false;
  syncView();
  syncRadio();
  return true;
}

/* 亮态那两枚只是"当前状态"的指示，点它无意义（也已在 syncView 里 setTouchable(false)） */
static bool onButtonClick_TabVuOn(ZKButton *p) { (void)p; return true; }
static bool onButtonClick_TabSpecOn(ZKButton *p) { (void)p; return true; }

static bool onButtonClick_BtnRadioPrev(ZKButton *p) {
  (void)p;
  if (gCount <= 0) return true;
  int i = (gPlaying > 0) ? (gPlaying - 1) : (gCount - 1);
  radioPlayAt(i);
  syncRadio();
  return true;
}

static bool onButtonClick_BtnRadioNext(ZKButton *p) {
  (void)p;
  if (gCount <= 0) return true;
  int i = (gPlaying >= 0 && gPlaying + 1 < gCount) ? (gPlaying + 1) : 0;
  radioPlayAt(i);
  syncRadio();
  return true;
}

static bool onButtonClick_BtnRadioStop(ZKButton *p) {
  (void)p;
  LOGD("radioLogic: 停止");
  radioStopTo(false);
  localSetStatus("已停止。点电台即播放");
  syncRadio();
  return true;
}

/* ==================== 列表回调 ==================== */
static int getListItemCount_ListRadio(const ZKListView *pListView) {
  (void)pListView;
  return gCount;
}

static void obtainListItemData_ListRadio(ZKListView *pListView,
                                         ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  if (!pListItem || index < 0 || index >= gCount) return;
  const Sta *s = &gSta[index];

  /* ⚠️ 行视图是**跨行复用**的：每行每个字段都要写全（不能依赖上一行留下的状态）。 */
  ZKListView::ZKListSubItem *nm = pListItem->findSubItemByID(ID_RADIO_SubStaName);
  ZKListView::ZKListSubItem *info = pListItem->findSubItemByID(ID_RADIO_SubStaInfo);
  ZKListView::ZKListSubItem *nt = pListItem->findSubItemByID(ID_RADIO_SubStaNote);

  if (nm) {
    char b[64];
    snprintf(b, sizeof(b), "%02d  %s", index + 1, s->name);
    nm->setText(b);
    nm->setTextColor(index == gPlaying ? 0xFF30D158 : 0xFFF2F2F7);   // 正在播的高亮
  }
  if (info) {
    char host[80];
    nameFromUrl(s->url, host, sizeof(host));
    /* 实测结论直接写在行上（`radioscan` 之前是"未测"）—— 用户点之前就知道哪个能听 */
    const char *mark = (s->ok == 1) ? " · 实测可播" : (s->ok == 0 ? " · 实测打不开" : "");
    char b[128];
    snprintf(b, sizeof(b), "%s · %s%s", s->group, host, mark);
    info->setText(b);
    info->setTextColor(s->ok == 0 ? 0xFF6E6E73 : 0xFF9A9AA0);
  }
  if (nt) {
    nt->setText(index == gPlaying ? "播放中" : "");
    nt->setTextColor(0xFF30D158);
  }
}

static void onListItemClick_ListRadio(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  if (index < 0 || index >= gCount) return;
  LOGD("radioLogic: 点列表第 %d 行 -> %s", index, gSta[index].name);
  radioPlayAt(index);
  syncRadio();
}
