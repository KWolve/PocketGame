#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
/*
 * iptvLogic.cc - 网络电视 IPTV（独立 ftu：iptv.ftu -> iptvActivity）
 *
 * 拆分口径见 docs/page-split-plan.md：游戏不动，其余功能界面全部独立成 ftu。
 * 本页装原 main.ftu 的两个窗口：
 *   WinIptv     选台页（8 槽 + 翻页 + 重新加载）—— 默认显示
 *   WinIptvPlay 播放页（videoview 透明窗口 + 三行加载提示 + 缓冲底图 + 底部三键）
 *
 * 链路（见 docs/iptv.md）：PgHls 拉 m3u8 分片 → 本地中继 → StreamPlayer(ffmpeg mpegts
 * + zk_h264_player 硬解) → disp 视频层，从 videoview 的"透明窗口"透出。
 *
 * 相位机（IDLE→SWITCHING→WAITFRAME→PLAYING / ERROR）由本页定时器驱动；
 * 出画判据用 `pg::H264Player::framesDecodedInRun() > 0`（**别用累计值做减法** ——
 * 清零在子线程里，自己记基线会有竞态，见 docs/iptv.md §11.4.4）。
 *
 * 按键（本板实体键）：108 短按 = 停止回选台页；108 长按 ≥700ms = 返回应用列表；
 *   103/105 = 音量±。⚠️ 本板 gpio-keys 无 autorepeat ⇒ 长按只能应用层按时长判。
 *
 * 自检通道：/tmp/pg_iptvcmd（独立页必须有自己的通道 —— 后台 Activity 的定时器不跑，
 * 主界面的 /tmp/pg_autostart 在这里失效，wifi/remote/工具页/时钟套件都踩过）：
 *   iptv <n>  播第 n 个频道（n<0 = 停止回选台页）
 *   iptvlist  打印频道表
 *   iptvstat  打印相位机状态
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/PgGame.h"
#include "control/ZKListView.h"   // 频道列表（ListIptv）
#include "entry/EasyUIContext.h"
#include "manager/ConfigManager.h"   // CONFIGMANAGER->getResFilePath（内置频道表）
#include "platform/PgSaver.h"     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgAudio.h"        // pg::volumeStepGlobal（音量键）
#include "platform/PgAlarm.h"     // 响铃让位
#include "platform/PgH264.h"
#include "platform/PgHls.h"
#include "platform/PgStream.h"
#include "platform/PgNavi.h"      // 视频播放页标志（navibar 会据此收起自己）
#include "utils/Log.h"

namespace {

/* IPTV 相位机（原在 mainLogic 的全局区；随本页搬过来）。语义见下面 tickIptv 的注释。 */
enum {
  IPTV_IDLE = 0,     // 没在播（选台页）
  IPTV_SWITCHING,    // 旧流在收尾（等 StreamPlayer::running() 归零）
  IPTV_WAITFRAME,    // 已起流，等第一帧解码出来
  IPTV_PLAYING,      // 正常播放
  IPTV_ERROR         // 失败/超时：显示原因，短暂停留后回选台页
};

/* 下面这两个是"看起来通用"的小 helper，其实都埋在 mainLogic.cc 的匿名 namespace 里
 * ⇒ 独立 ftu 必须自带一份（不跨文件依赖，否则要么链接错、要么造出第二个实体）。 */
void setCtrlBg(ZKBase *v, uint32_t color) {
  if (!v) return;
  v->setBackgroundColor(color);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
}

/* 视频区"缓冲底图"显隐（见 docs/iptv.md §11.4.2）。幂等，可以每拍调。 */
void setVideoCover(ZKTextView *tv, bool on) {
  if (!tv) return;
  if (tv->isVisible() == on) return;
  tv->setVisible(on);
}

/* 本页内部函数的前置声明（定义写在下面，互相调用要按顺序）。 */
static long long iptvNowMs();
static void iptvLoadText(const char *title, const char *bar, const char *hint, int barColor);
static void iptvHideLoad();
static void iptvShowPlayPage();
static void iptvBackToList();
static void iptvBeginOpen(int idx);
void resetIptvCache();
void syncIptv();
void syncIptvPlay();
void tickIptv();
void stopIptvAndQuit();   // 停流 + 关页（按键与响铃让位都要用）

/* 文本写入：只在变化时写控件（与 mainLogic 里同名实现一致，独立 ftu 自带一份，
 * 不跨文件依赖）。 */
void setToolText(ZKTextView *tv, char *cache, int n, const char *val) {
  if (!tv) return;
  if (!val) val = "";
  if (strncmp(cache, val, (size_t)n - 1) == 0) return;
  snprintf(cache, (size_t)n, "%s", val);
  tv->setText(cache);
}

/* ==================== IPTV 网络电视（WinIptv / WinIptvPlay） ====================
 * 数据链路在 `platform/PgHls.*`（HLS 拉分片 + 本地中继），播放交给 `StreamPlayer`。
 * 这里只干三件事：① 读频道表；② 选台页填充/翻页；③ 起停播放 + 播放页状态。
 *
 * ⚠️ **本板硬解上限 960x544（约 52 万像素）**，720p/1080p 会直接把解码通道打爆
 *    （见 PgStream.cpp 的 kMaxDecodePixels 及其注释）。所以：
 *      · 频道表由 `tools/hls_filter.py` **预先筛过** → `resources/iptv/channels.m3u`；
 *      · 设备端支持 `/data/iptv.m3u` **覆盖**（用户自己更新源，不用重刷固件）；
 *      · PgHls 对多档清单**自动挑最低码率档**，进一步降低踩雷概率。
 *
 * 内存：频道表用静态数组（100 条 × ~532B ≈ 52KB，不占堆）。
 */
const int kIptvMaxCh = 100;   // 频道表上限（畸形 m3u 也不至于把内存吃光）

/* 一个频道（列表每行显示的就是这三个字段拆出来的信息）。
 *   name/group      来自 `#EXTINF` 的"最后一个逗号之后"与 `group-title`（中文）
 *   size/kbps       来自 `# 实测 …` 注释（tools/iptv_probe.py 真拉码流解出来的）——
 *                   列表副行显示，让用户一眼知道"这个台清不清晰、会不会太吃带宽"
 *   note            来自 `tvg-note`（如"仅限国内"=Geo-blocked、"非全天播出"=Not 24/7）
 * ⚠️ 频道表由 tools/gen_channels.py 规范成这个格式（中文名 + 这些字段），
 *    旧表/用户自备的 /data/iptv.m3u 没有这些字段也能跑（对应字段为空，列表就少显示一段）。 */
struct IptvChannel {
  char name[64];
  char group[28];
  char size[26];    // "1280x720@25.00fps"
  char kbps[12];    // "1500kbps"
  char note[26];    // "仅限国内"
  char url[440];
};

IptvChannel gIptvCh[kIptvMaxCh];
int gIptvCount = 0;
int gIptvPlaying = -1;        // 正在播的频道下标（-1 = 没在播）
bool gIptvPlayWin = false;    // 播放页是否亮着
bool gIptvLoaded = false;
char gIptvSrcTag[48] = "";    // 频道表来源（显示用）

char gIptvStatusCache[160], gIptvPlayMsgCache[128];
int gIptvListCount = -1;      // 列表当前的行数（变了才 refreshListView）

/* ==================== 打开/切换的相位机（2026-09-14）====================
 * 用户两条要求：① 打开频道要有 **loading 效果 + 超时处理**；② 停止/切下一个**不要重启**。
 *
 * 相位把原来"点一下黑盒等半天"的过程拆成可见、可超时的几步：
 *   IDLE → SWITCHING（旧流收尾）→ WAITFRAME（起流 + 等首帧）→ PLAYING
 *   任何一步失败/超时 → ERROR（显示原因，停一会回选台页）
 * 与之配套：`Hls::start()` 已改成**非阻塞**（清单交给它的下载线程拉，见 PgHls::openState），
 * 否则 UI 线程被秒级的 httpFetch 堵住，连"正在连接"四个字都画不出来。 */
int gIptvPhase = IPTV_IDLE;   // 相位枚举在上面（全局区）已定义
int gIptvTarget = -1;         // 目标频道（相位机在跑时有效；-1 = 回选台页）
long long gIptvPhaseMs = 0;   // 当前相位的开始时刻（超时判据）
long long gIptvAnimMs = 0;    // loading 动画上次推进时刻
int gIptvAnimStep = 0;
int gIptvLastBytes = 0;       // 上次看到的 HLS 已下字节数（"有没有进展"的判据）
/* ⚠️ 原来这里有个"起播前累计解码帧基线"，用来算"本轮解码了几帧"。**已废弃**：
 *    `H264Player::startStream()` 会清零累计值、而清零发生在**子线程**里，
 *    基线在主线程记 —— 谁先跑决定结果。实测踩到：基线读到清零前的旧值，
 *    "本轮帧数"变成**负数**，出画判据永远不成立（视频 1 秒就出帧了，
 *    界面却干等 20 秒才收底图）。
 *    现在统一用 `pg::H264Player::framesDecodedInRun()`（PgStream::startCommon
 *    在起线程之前清零，无竞态）。 */
long long gIptvStartMs = 0;   // 本轮起播的绝对起点（判绝对上限）
char gIptvErr[128] = "";      // 最后一次失败原因

/* 超时（取"用户可忍"与"网络真慢"之间）：
 *   · 换台收尾 9s —— 正常收尾 1s 内；卡住说明通道收尾异常，直接报错别干等
 *   · 首帧**停滞** 10s —— ⚠️ 判据是"**多久没有新进展**"，不是"总共等了多久"。
 *       而且"进展"必须看 **Hls::bytes()（已下字节）**，**不能看 segments()**：
 *       实测（真机）有源首分片要下 12s 才完成，这期间 bytes 一路在涨、
 *       segments 一直是 0 —— 用 segments 判进展就会把**正在正常下载**的流误杀
 *       （踩过：日志 `停止（已下 0 分片 / 425984 字节）` = 明明下了 416KB）。
 *   · 首帧**绝对上限** 35s —— 再慢也不无限等
 *   · 错误提示 2.6s —— 够看清原因，又不至于让用户觉得卡住 */
const int kIptvSwitchTimeoutMs = 9000;
const int kIptvFirstFrameStallMs = 10000;
const int kIptvFirstFrameCapMs = 35000;
const int kIptvErrorHoldMs = 2600;

/* 按槽位取按钮（0..7）：框架生成器给的是 8 个独立成员，只能这样映射 */
/*
 * 读频道表。`/data/iptv.m3u` 优先（用户可自己更新，源失效不用等固件），
 * 没有就用随固件的 resources/iptv/channels.m3u（打包后在 /res/ui/iptv/）。
 * 解析只认最基本的两样：#EXTINF 行的"名字"和紧随其后的 URL 行；
 * group-title 顺便取出来当分组名（只用于选台页分组显示）。
 */
int iptvLoadChannels() {
  gIptvCount = 0;
  gIptvSrcTag[0] = 0;

  const char *openPath = 0;
  FILE *fp = fopen("/data/iptv.m3u", "rb");
  if (fp) {
    openPath = "/data/iptv.m3u";
    snprintf(gIptvSrcTag, sizeof(gIptvSrcTag), "用户表 /data/iptv.m3u");
  } else {
    std::string rp = CONFIGMANAGER->getResFilePath("iptv/channels.m3u");
    fp = fopen(rp.c_str(), "rb");
    if (fp) {
      openPath = rp.c_str();
      snprintf(gIptvSrcTag, sizeof(gIptvSrcTag), "内置表");
    }
  }
  if (!fp) {
    LOGW("PocketGame iptv: 没有找到频道表（/data/iptv.m3u 和内置表都打不开）");
    return 0;
  }

  char line[640];
  char pendName[64] = {0};
  char pendGroup[28] = {0};
  char pendSize[26] = {0};    // `# 实测 1280x720@25.00fps · 1500kbps` 的前半
  char pendKbps[12] = {0};    //                                           后半
  char pendNote[26] = {0};    // `tvg-note="仅限国内"`
  bool haveInfo = false;
  while (fgets(line, sizeof(line), fp)) {
    /* 去尾部空白 */
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) {
      line[--n] = 0;
    }
    if (n == 0) continue;

    if (line[0] == '#') {
      if (strncmp(line, "#EXTINF", 7) != 0) {
        /* `# 实测 1280x720@25.00fps · 1500kbps` —— 探测脚本真拉码流解出来的清晰度/码率，
         * 列表副行会显示（没有这行也不影响播放，只是少显示一段）。 */
        char *m = strstr(line, "实测");
        if (m) {
          m += 6;                       // "实测" 在 UTF-8 里是 6 字节
          while (*m == ' ') ++m;
          char *sep = strstr(m, " · ");  // "空格 + ·(2字节) + 空格" = 4 字节
          if (sep) {
            int l = (int)(sep - m);
            if (l > (int)sizeof(pendSize) - 1) l = (int)sizeof(pendSize) - 1;
            memcpy(pendSize, m, (size_t)l);
            pendSize[l] = 0;
            snprintf(pendKbps, sizeof(pendKbps), "%s", sep + 4);
          } else {
            snprintf(pendSize, sizeof(pendSize), "%s", m);
            pendKbps[0] = 0;
          }
        }
        continue;
      }
      /* 名字 = 最后一个逗号之后 */
      char *comma = strrchr(line, ',');
      snprintf(pendName, sizeof(pendName), "%s", comma ? comma + 1 : "");
      /* group-title="..." */
      char *g = strstr(line, "group-title=\"");
      pendGroup[0] = 0;
      if (g) {
        g += 13;
        char *e = strchr(g, '"');
        if (e) {
          int gl = (int)(e - g);
          if (gl > (int)sizeof(pendGroup) - 1) gl = (int)sizeof(pendGroup) - 1;
          memcpy(pendGroup, g, (size_t)gl);
          pendGroup[gl] = 0;
        }
      }
      /* tvg-note="..." —— 源的限制（"仅限国内"/"非全天播出"），列表右侧会显示 */
      char *nt = strstr(line, "tvg-note=\"");
      pendNote[0] = 0;
      if (nt) {
        nt += 10;                     // `tvg-note="` 共 10 字节
        char *e = strchr(nt, '"');
        if (e) {
          int l = (int)(e - nt);
          if (l > (int)sizeof(pendNote) - 1) l = (int)sizeof(pendNote) - 1;
          memcpy(pendNote, nt, (size_t)l);
          pendNote[l] = 0;
        }
      }
      haveInfo = true;
      continue;
    }

    /* URL 行 */
    if (strncmp(line, "http://", 7) != 0 && strncmp(line, "https://", 8) != 0) continue;
    if (gIptvCount >= kIptvMaxCh) continue;

    IptvChannel *c = &gIptvCh[gIptvCount];
    snprintf(c->name, sizeof(c->name), "%s", haveInfo && pendName[0] ? pendName : line);
    snprintf(c->group, sizeof(c->group), "%s", pendGroup);
    snprintf(c->size, sizeof(c->size), "%s", pendSize);
    snprintf(c->kbps, sizeof(c->kbps), "%s", pendKbps);
    snprintf(c->note, sizeof(c->note), "%s", pendNote);
    snprintf(c->url, sizeof(c->url), "%s", line);
    ++gIptvCount;
    haveInfo = false;
    pendName[0] = 0;
    pendSize[0] = 0;
    pendKbps[0] = 0;
    pendNote[0] = 0;
  }
  fclose(fp);

  gIptvPlaying = -1;
  gIptvLoaded = true;
  LOGD("PocketGame iptv: 频道表已加载 %d 个（%s，%s）", gIptvCount, gIptvSrcTag, openPath);
  return gIptvCount;
}

/*
 * ============ 打开 / 换台 / 停止（2026-09-14 重做）============
 *
 * 旧做法：点频道**同步**拉清单（UI 线程被堵住，像卡死）；换台/停止写"续播请求" +
 * `_exit(0)` 重启整个进程（屏幕闪一下回主界面）。
 * 新做法：不阻塞、不重启，用相位机 + 超时把过程做成可看、可等、可错的体验。
 *
 * **为什么现在敢"不重启"**：当年"停止过的解码通道会变脏、复用它不再出帧"的结论，
 * 针对的是 **MPP 老路**（VDEC+VO+应用层软件旋转）。视频链路现在默认走
 * `zk_h264_player` 硬件播放器（`sHwVideo=1`，见 docs/iptv.md §10）：它的 `stop()` 会
 * `deinit` 硬件播放器并把 disp 视频层 `VideoLayer::release()` 掉，下一轮是**全新一套**，
 * 不存在"复用脏通道"。所以这里改用 `StreamPlayer::stopForSwitch()` ——
 * 同一条优雅停流路径，只是**不置复位标志**（`sSwitchMode`）。
 */
static long long iptvNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 加载页三行：标题 / 移动的进度条 / 提示。
 * ⚠️ 进度条用 ASCII 拼（'[' ']' '=' '-'），不引图片资源，也完全避开字库缺字。 */
/* 信息行配色：加载中是蓝（与进度条同色），失败是琥珀（一眼看出是错误）。
 * 只在变化时写控件 —— 这行每 120ms 刷一次动画，别每次都设色。 */
const int kIptvBarBlue = 0xFF64D2FF;
const int kIptvBarAmber = 0xFFFF9F0A;

static void iptvLoadText(const char *title, const char *bar, const char *hint, int barColor) {
  static char c1[96], c2[64], c3[96];
  static int curColor = 0;
  if (barColor && barColor != curColor) {
    curColor = barColor;
    if (mTextIptvLoadBarPtr) mTextIptvLoadBarPtr->setTextColor(barColor);
  }
  if (strcmp(c1, title ? title : "") != 0) {
    snprintf(c1, sizeof(c1), "%s", title ? title : "");
    if (mTextIptvLoadPtr) mTextIptvLoadPtr->setText(c1);
  }
  if (strcmp(c2, bar ? bar : "") != 0) {
    snprintf(c2, sizeof(c2), "%s", bar ? bar : "");
    if (mTextIptvLoadBarPtr) mTextIptvLoadBarPtr->setText(c2);
  }
  if (strcmp(c3, hint ? hint : "") != 0) {
    snprintf(c3, sizeof(c3), "%s", hint ? hint : "");
    if (mTextIptvLoadHintPtr) mTextIptvLoadHintPtr->setText(c3);
  }
}

static void iptvHideLoad() { iptvLoadText("", "", "", 0); }

/* loading 动画：标题后面的点号 + 进度条里的方块一起走（~8fps 够了，别再快）。 */
static void iptvAnim(const char *what, long long now, long long elapsedMs, int timeoutMs) {
  if (now - gIptvAnimMs < 120) return;
  gIptvAnimMs = now;
  ++gIptvAnimStep;

  char title[96];
  int dots = gIptvAnimStep % 4;   // 0..3
  snprintf(title, sizeof(title), "%s%s", what, dots == 1 ? "." : dots == 2 ? ".." : dots == 3 ? "..." : "");

  const int W = 16, B = 5;
  char bar[32];
  int slot = gIptvAnimStep % (W - B + 1);
  bar[0] = '[';
  for (int i = 0; i < W; ++i) bar[1 + i] = (i >= slot && i < slot + B) ? '=' : '-';
  bar[1 + W] = ']';
  bar[2 + W] = 0;

  /* 提示行：把 HLS 的真实状态带上（"缓冲中(1/2)"这类）—— 用户能看到"它在动"，
   * 比只写"已等 N 秒"踏实得多；排障时也不用另外开日志。 */
  /* 提示行：带上"已缓冲多少 KB"—— 这是最能说明"它在动"的数字（慢源下
   * HLS 的 status 词会一直停在"缓冲中(0/2)"，用户看着像卡住）。 */
  char hint[128];
  if (pg::Hls::running()) {
    snprintf(hint, sizeof(hint), "已等 %ds · 已缓冲 %dKB", (int)(elapsedMs / 1000),
             pg::Hls::bytes() / 1024);
  } else {
    snprintf(hint, sizeof(hint), "已等 %ds · 准备中…", (int)(elapsedMs / 1000));
  }
  iptvLoadText(title, bar, hint, kIptvBarBlue);
}

/* 切到播放页（幂等） */
static void iptvShowPlayPage() {
  gIptvPlayWin = true;
  if (mWinIptvPtr) mWinIptvPtr->hideWnd();
  if (mWinIptvPlayPtr) mWinIptvPlayPtr->showWnd();
  /* ★ 2026-09-16「检讨 UI 覆盖」修正 ②（用户已审批）：播放页的视频区是
   *   480x700 @y=0 且"等比铺满"⇒ navibar(0..52 常显) 会永久压掉画面顶部 52px。
   *   置标志让 navibar 自己收起来（退出时 iptvBackToList() 复位）。见 PgNavi.h。 */
  pg::setVideoPage(true);
}

/* 回选台页（幂等）：相位归零 + 清加载文字 */
static void iptvBackToList() {
  iptvHideLoad();
  setVideoCover(mImgIptvCoverPtr, false);   // 回选台页：底图不能留在列表上
  gIptvPhase = IPTV_IDLE;
  gIptvTarget = -1;
  gIptvPlayWin = false;
  gIptvPlaying = -1;
  if (mWinIptvPlayPtr) mWinIptvPlayPtr->hideWnd();
  if (mWinIptvPtr) mWinIptvPtr->showWnd();
  pg::setVideoPage(false);                  // 让 navibar 回来（见上）
  resetIptvCache();
  syncIptv();
}

/* 失败/超时：停掉一切、把原因写在加载页上，停 kIptvErrorHoldMs 秒后回选台页。
 * 为什么不停在那里等用户点：IPTV 的播放页只有三个按钮（上/停/下），
 * 与其让用户自己琢磨，不如"报清楚 + 自动退回列表"让他直接换一个台。 */
static void iptvFail(const char *fmt, ...) {
  char msg[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  snprintf(gIptvErr, sizeof(gIptvErr), "%s", msg);
  LOGW("PocketGame iptv: 失败 —— %s", msg);
  pg::Hls::stop();
  if (pg::StreamPlayer::running()) pg::StreamPlayer::stopForSwitch();   // 不重启
  gIptvPlaying = -1;
  gIptvPhase = IPTV_ERROR;
  gIptvPhaseMs = iptvNowMs();
  gIptvAnimMs = 0;   // 让错误页的第一帧就写出来
  iptvShowPlayPage();
  iptvLoadText("连接失败", gIptvErr, "即将返回频道列表", kIptvBarAmber);
}

/* 真正起播：`Hls::start()` 现在**立刻返回**（清单在它的下载线程里拉），
 * 播放器也立刻起来（它自己等数据），所以这一步不会卡 UI。 */
static void iptvBeginOpen(int idx) {
  if (idx < 0 || idx >= gIptvCount) { iptvFail("频道不存在"); return; }
  IptvChannel *c = &gIptvCh[idx];
  gIptvTarget = idx;

  pg::Hls::stop();   // 保险：清掉可能残留的中继
  char local[128];
  if (!pg::Hls::start(c->url, local, sizeof(local))) {
    iptvFail("中继启动失败：%s", pg::Hls::lastError());
    return;
  }
  if (!pg::StreamPlayer::startWithDisplay(local, 0, 0, 0, 480, 700)) {
    pg::Hls::stop();
    iptvFail("播放器启动失败");
    return;
  }
  gIptvPlaying = idx;
  iptvShowPlayPage();
  gIptvPhase = IPTV_WAITFRAME;
  gIptvPhaseMs = iptvNowMs();
  gIptvStartMs = gIptvPhaseMs;
  gIptvLastBytes = 0;
  /* 出画判据用 `framesDecodedInRun()`（PgH264 内部按"本轮"计），
   * 不需要也不允许在这里记基线 —— 清零在子线程里，记基线就有竞态（见上方说明）。 */
  gIptvAnimMs = 0;
  {
    char t[96];
    snprintf(t, sizeof(t), "正在连接 %s", c->name);
    iptvLoadText(t, "[-----=====------]", "", kIptvBarBlue);
  }
  LOGD("PocketGame iptv: 起流 [%d] %s（%s）→ %s", idx, c->name, c->group, local);
}

/* 起播第 idx 个频道（用户点频道 / 上一个 / 下一个 / QA `iptv n` 都走这里）。
 * ⚠️ 本函数**必须立刻返回**：UI 线程上不许有网络、不许等流。 */
void iptvPlayAt(int idx) {
  if (idx < 0 || idx >= gIptvCount) return;

  /* 有流在跑 → 先优雅停旧的（stopForSwitch：不置复位标志 = **不重启进程**），
   * 等它在 tickIptv 里收干净再开新台。 */
  if (pg::StreamPlayer::running()) {
    gIptvTarget = idx;
    gIptvPhase = IPTV_SWITCHING;
    gIptvPhaseMs = iptvNowMs();
    gIptvAnimMs = 0;
    iptvShowPlayPage();
    {
      char t[96];
      snprintf(t, sizeof(t), "正在切换到 %s", gIptvCh[idx].name);
      iptvLoadText(t, "", "", kIptvBarBlue);
    }
    pg::Hls::stop();
    pg::StreamPlayer::stopForSwitch();
    LOGD("PocketGame iptv: 换台（不重启）→ 目标 [%d] %s", idx, gIptvCh[idx].name);
    return;
  }
  iptvBeginOpen(idx);
}

/* 停止并回到选台页（同样是**优雅停 + 不重启**） */
void iptvStopAndBack() {
  gIptvTarget = -1;
  if (pg::StreamPlayer::running()) {
    gIptvPhase = IPTV_SWITCHING;
    gIptvPhaseMs = iptvNowMs();
    gIptvAnimMs = 0;
    iptvShowPlayPage();
    iptvLoadText("正在停止", "", "", kIptvBarBlue);
    pg::Hls::stop();
    pg::StreamPlayer::stopForSwitch();
    LOGD("PocketGame iptv: 停止（不重启）→ 收尾完成后回选台页");
    return;
  }
  pg::Hls::stop();
  iptvBackToList();
  LOGD("PocketGame iptv: 已停止并回到选台页");
}

/* 清 UI 缓存 → 下一帧全量重写控件（否则"值没变"的文本不会刷新）。
 * 重载/进出播放页/回到选台页时都要调一次。
 * 列表行不用在这儿清：行视图由 refreshListView() 重新回调 obtainListItemData 填。 */
void resetIptvCache() {
  gIptvStatusCache[0] = 0;
  gIptvPlayMsgCache[0] = 0;
  gIptvListCount = -1;     // 强制下次 syncIptv 重刷列表
}

/* 选台页刷新：**只做"行数 + 状态行"**，行内容由列表回调按需填（框架机制）。
 * 2026-09-14 用户要求把"8 个槽 + 翻页"换成列表控件 —— 一屏 8 个还要翻页，体验太差。
 * 现在：一屏 7 行可见、可滚动、每行带中文名 + 分组 + 实测清晰度。 */
void syncIptv() {
  if (!mWinIptvPtr || !mWinIptvPtr->isWndShow()) return;

  /* 频道表行数变了才 refreshListView（它会逐行回调 obtainListItemData，
   * 每帧调会白白重绘）。gIptvListCount = -1 由 resetIptvCache() 置，用于强制重刷。 */
  if (mListIptvPtr && gIptvCount != gIptvListCount) {
    gIptvListCount = gIptvCount;
    mListIptvPtr->refreshListView();
    LOGD("iptvLogic: 频道列表已刷新（%d 行）", gIptvCount);
  }

  char b[160];
  if (gIptvPlayWin && gIptvPlaying >= 0) {
    snprintf(b, sizeof(b), "正在播放：%s", gIptvCh[gIptvPlaying].name);
  } else if (gIptvCount == 0) {
    snprintf(b, sizeof(b), "频道表为空 —— 请检查 %s", gIptvSrcTag);
  } else {
    snprintf(b, sizeof(b), "共 %d 个频道 · %s · 点列表里的频道播放", gIptvCount, gIptvSrcTag);
  }
  setToolText(mTextIptvStatusPtr, gIptvStatusCache, sizeof(gIptvStatusCache), b);
}

/* 播放页刷新：只更新顶部状态字（视频画面在 disp 视频层，不在 UI 层） */
void syncIptvPlay() {
  if (!gIptvPlayWin) return;
  char b[128];
  if (gIptvPlaying >= 0) {
    snprintf(b, sizeof(b), "%s · %s · %d分片", gIptvCh[gIptvPlaying].name,
             pg::Hls::status(), pg::Hls::segments());
  } else if (gIptvPhase == IPTV_SWITCHING) {
    snprintf(b, sizeof(b), "正在切换…");
  } else if (gIptvPhase == IPTV_ERROR) {
    snprintf(b, sizeof(b), "%s", gIptvErr);
  } else {
    snprintf(b, sizeof(b), "已停止");
  }
  if (strcmp(gIptvPlayMsgCache, b) != 0) {
    snprintf(gIptvPlayMsgCache, sizeof(gIptvPlayMsgCache), "%s", b);
    if (mTextIptvPlayMsgPtr) mTextIptvPlayMsgPtr->setText(gIptvPlayMsgCache);
  }
}

/* 把频道表打进日志（QA `iptvlist`）。本板注入不了控件触摸，
 * 所以"表读对了没、第几个是什么"必须能从日志直接看出来。 */
void iptvDebugList() {
  if (!gIptvLoaded) iptvLoadChannels();
  for (int i = 0; i < gIptvCount; ++i) {
    /* 把列表会显示的字段都打出来（排障"列表某段没显示"最快的手段） */
    LOGD("PocketGame iptv[%d] 分组='%s' 名='%s' 实测='%s' '%s' 备注='%s' %s", i,
         gIptvCh[i].group, gIptvCh[i].name, gIptvCh[i].size, gIptvCh[i].kbps,
         gIptvCh[i].note, gIptvCh[i].url);
  }
  LOGD("PocketGame iptvlist: 共 %d 个（%s）", gIptvCount, gIptvSrcTag);
}

/* ★ IPTV 相位机（主循环每帧调用，见上面相位的定义）。
 * 这里承担三件事：推进相位、刷 loading 动画、判三个超时。
 * ⚠️ 全程只读状态 + 写控件，**不做任何网络/阻塞动作**。 */
void tickIptv() {
  /* 缓冲底图：只在"等旧流收尾 / 等首帧 / 报错"这三段铺着 —— 那是视频层**没有画面**的时段
   * （透明窗口透出黑底，字发虚）。进入正常播放或回到选台页立刻收起。
   * 放在最前面（早退之前）：IDLE 时也要保证收起。 */
  setVideoCover(mImgIptvCoverPtr, gIptvPhase == IPTV_SWITCHING ||
                                      gIptvPhase == IPTV_WAITFRAME ||
                                      gIptvPhase == IPTV_ERROR);
  if (gIptvPhase == IPTV_IDLE) return;
  const long long now = iptvNowMs();
  const long long el = now - gIptvPhaseMs;

  switch (gIptvPhase) {
    case IPTV_SWITCHING:
      /* 等旧流收尾干净（收尾里会 deinit 硬件播放器 + 释放 disp 视频层） */
      if (pg::StreamPlayer::running()) {
        if (el > kIptvSwitchTimeoutMs) {
          iptvFail("停止旧流超时（%d 秒）", kIptvSwitchTimeoutMs / 1000);
        } else if (gIptvTarget >= 0) {
          char t[96];
          snprintf(t, sizeof(t), "正在切换到 %s", gIptvCh[gIptvTarget].name);
          iptvAnim(t, now, el, kIptvSwitchTimeoutMs);
        } else {
          iptvAnim("正在停止", now, el, kIptvSwitchTimeoutMs);
        }
        return;
      }
      if (gIptvTarget >= 0) iptvBeginOpen(gIptvTarget);
      else iptvBackToList();
      return;

    case IPTV_WAITFRAME: {
      /* ① 频道不可用：清单打不开 / 加密 / fMP4 —— 立刻给明确原因，别让用户干等 */
      if (pg::Hls::openState() == 2) {
        iptvFail("频道不可用：%s", pg::Hls::lastError());
        return;
      }
      /* ② 出画了 → 进正常播放，收起加载页（判据用**解码回调计数**，
       *    不能用 get_picture_count —— 它恒等于已提交帧数，见 docs/iptv.md §10.3） */
      {
        const int dec = pg::H264Player::framesDecodedInRun();
        if (dec > 0) {
          LOGD("PocketGame iptv: 出画（起播→首帧共 %lldms，频道 %d，本轮已解码 %d 帧）",
               now - gIptvStartMs, gIptvPlaying, dec);
          iptvHideLoad();
          gIptvPhase = IPTV_PLAYING;
          gIptvPhaseMs = now;
          return;
        }
      }
      /* ③ 播放器自己结束了（起流失败/源断开）→ 直接报错 */
      if (!pg::StreamPlayer::running()) {
        iptvFail("流已中断（%s）", pg::Hls::status());
        return;
      }
      /* ④ 超时：判"**多久没有新进展**"，不是"总共等了多久"。
       *    只要 HLS 还在下新分片就重新计时（正常但慢的源不该被误杀）；
       *    再配一个绝对上限兜底，避免"一直有点进展但永不出画"卡死在这里。 */
      {
        const int by = pg::Hls::bytes();
        if (by > gIptvLastBytes) {
          gIptvLastBytes = by;
          gIptvPhaseMs = now;   // 有数据在进 → 重新计时（慢源不该被判死）
        }
        if (el > kIptvFirstFrameStallMs) {
          iptvFail("连接超时（%d 秒无进展）", kIptvFirstFrameStallMs / 1000);
          return;
        }
        if (now - gIptvStartMs > kIptvFirstFrameCapMs) {
          iptvFail("连接超时（已等 %d 秒仍无画面）", kIptvFirstFrameCapMs / 1000);
          return;
        }
      }
      char t[96];
      snprintf(t, sizeof(t), "正在缓冲 %s", gIptvCh[gIptvPlaying].name);
      iptvAnim(t, now, now - gIptvStartMs, kIptvFirstFrameCapMs);
      return;
    }

    case IPTV_PLAYING:
      /* 正常播放中流自己结束（源断/播完/被停）→ 回选台页 */
      if (!pg::StreamPlayer::running()) {
        LOGD("PocketGame iptv: 流已结束，收掉播放页");
        pg::Hls::stop();
        iptvBackToList();
      }
      return;

    case IPTV_ERROR:
      if (el > kIptvErrorHoldMs) iptvBackToList();
      return;

    default:
      return;
  }
}


/* ==================== 自检通道 /tmp/pg_iptvcmd ==================== */
char sQaTag[512] = {0};

void qaSyncTag() {
  FILE *f = fopen("/tmp/pg_iptvcmd", "r");
  if (!f) return;
  size_t n = fread(sQaTag, 1, sizeof(sQaTag) - 1, f);
  fclose(f);
  sQaTag[n] = 0;
}

/* IPTV 的免触摸操作（命令名沿用主界面时期的，脚本不用改）。 */
void handleIptvCmd(const char *line) {
  if (strncmp(line, "iptvstat", 8) == 0) {
    static const char *kName[] = {"IDLE", "SWITCHING", "WAITFRAME", "PLAYING", "ERROR"};
    int ph = (gIptvPhase >= 0 && gIptvPhase <= 4) ? gIptvPhase : -1;
    LOGD("iptvLogic iptvstat: 相位=%s 目标=%d 已等=%lldms 字节=%d 清单=%d 本轮解码帧=%d 流=%d 内存=%lldkB",
         ph >= 0 ? kName[ph] : "?", gIptvTarget,
         (long long)(iptvNowMs() - gIptvPhaseMs), pg::Hls::bytes(), pg::Hls::openState(),
         pg::H264Player::framesDecodedInRun(), (int)pg::StreamPlayer::running(),
         (long long)pg::StreamPlayer::memAvailableKb());
    return;
  }
  if (strncmp(line, "iptvlist", 8) == 0) {
    iptvDebugList();
    return;
  }
  if (strncmp(line, "iptv ", 5) == 0 || strcmp(line, "iptv") == 0) {
    int n = (strlen(line) > 5) ? atoi(line + 5) : 0;
    if (n < 0) {
      iptvStopAndBack();
      resetIptvCache();
      syncIptv();
      LOGD("iptvLogic: iptv 停止");
      return;
    }
    LOGD("iptvLogic: iptv 播放第 %d 个", n);
    iptvPlayAt(n);
    return;
  }
  LOGD("iptvLogic QA: 未知命令 '%s'", line);
}

void qaPoll() {
  FILE *f = fopen("/tmp/pg_iptvcmd", "r");
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
    char *h = strchr(line, '#');          // 行内注释（习惯写 `iptv 0 #1` 保证内容变化）
    if (h) *h = 0;
    int ln = (int)strlen(line);
    while (ln > 0 && (line[ln - 1] == ' ' || line[ln - 1] == '\t')) line[--ln] = 0;
    if (line[0] == 0) continue;
    handleIptvCmd(line);
  }
}

/* ==================== 物理按键 ==================== */
class IptvKeys : public EasyUIContext::IKeyListener {
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
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;
    int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= 700 && !sLPFired) {
      LOGD("iptvLogic: 长按 %dms -> 返回列表", held);
      stopIptvAndQuit();
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
      int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("iptvLogic: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      return true;
    }
    if (ke.mKeyCode == 108) {
      /* 播放页里短按 = 停止回选台页；选台页里无动作。 */
      if (gIptvPlayWin) {
        iptvStopAndBack();
        resetIptvCache();
        syncIptv();
      }
      return true;
    }
    return true;
  }

  /** ★ 长按达标**立刻**返回，不等按键抬起。
   *   本板 gpio-keys **没有 autorepeat** ⇒ 长按期间内核一个事件都不发；只在 E_KEY_UP 里
   *   判长按的话，用户按满 700ms 还得一直按到松手才切页（手感是"按了不动、松手才跳"）。
   *   本页定时器本来就是 100ms 一拍，直接在 onUI_Timer 里轮询即可。 */
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
int IptvKeys::sDownCode = -1;
long IptvKeys::sDownMs = 0;
bool IptvKeys::sLPFired = false;
IptvKeys sKeys;

/* 退出页面前先停流（**不停流直接离开会让 55MB 内存的板子下一个应用必炸**）。 */
void stopIptvAndQuit() {
  if (gIptvPlayWin || gIptvPlaying >= 0) {
    pg::Hls::stop();
    pg::StreamPlayer::stopForSwitch();   // 不重启进程（见 docs/iptv.md §11.3）
    gIptvPlayWin = false;
    gIptvPlaying = -1;
    gIptvPhase = IPTV_IDLE;
    gIptvTarget = -1;
    LOGD("iptvLogic: 退出页面，已停流（不重启）");
  }
  EASYUICONTEXT->closeActivity("iptvActivity");
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

const int TIMER_TICK = 1;
const int TICK_MS = 100;

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
  EASYUICONTEXT->registerKeyListener(&sKeys);
#endif
  int n = iptvLoadChannels();     // 频道表（/data/iptv.m3u 优先，其次内置资源）
  LOGD("iptvLogic: 频道表 %d 个 -> 选台页", n);
  resetIptvCache();
  syncIptv();
  qaSyncTag();                    // 自检基线：只执行"进页之后新推的"命令
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("iptvLogic: onUI_show");
  /* 独立 ftu 必须自己管屏保（主界面不在前台时它的策略不跑，30 秒就会盖上来）。 */
  EASYUICONTEXT->setScreensaverEnable(false);
  if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
  EASYUICONTEXT->resetScreensaverTimeOut();
  resetIptvCache();
  syncIptv();
}

static void onUI_hide() {
  LOGD("iptvLogic: onUI_hide");
  pg::setVideoPage(false);      // 兜底：切后台时别把 navibar 粘在"隐藏"状态
}

static void onUI_quit() {
  LOGD("iptvLogic: onUI_quit");
  pg::setVideoPage(false);      // 同上（长按返回是 closeActivity，不走 iptvBackToList）
  stopIptvAndQuit();              // 离开页面必须停流（否则视频层/内存残留）
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
  if (id != TIMER_TICK) return false;
  /* ★ 响铃期间**让位**：本页是全屏独立页，留着就看不到主界面的"闹钟提醒页"
   *   （提醒页在 main.ftu，由主循环 tickAlarmRing() 弹出）⇒ 停流 + 收掉自己。
   *   见 docs/page-split-plan.md §9.1。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("iptvLogic: 响铃中 -> 让位（回主界面弹提醒页）");
    stopIptvAndQuit();
    return false;
  }
  /* 长按达标即"停流 + 返回列表"（不等抬手）。放最前：别让相位机白跑一拍。
   * ⚠️ 退出必须停流（55MB 内存，留着下一个应用必炸），所以走 stopIptvAndQuit。 */
  if (IptvKeys::longPressReady()) {
    LOGD("iptvLogic: 长按达标（不等抬起）-> 停流 + 返回列表");
    stopIptvAndQuit();
    return false;
  }
  tickIptv();          // 相位机（推进 / 刷 loading / 判超时）
  if (gIptvPlayWin) syncIptvPlay();
  else syncIptv();
  qaPoll();
  return true;
}

/**
 * @brief 触摸事件（本页控件都是原生按钮，无需拦截）
 */
static bool oniptvActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;   // 全部放行：按钮/视频区各归各的
}

/* ==================================================================
 *                   按钮回调（必须留在文件作用域 static：
 *   INIT_UI_EVENT_BINDINGS 会在文件顶部声明它们，放进匿名 namespace 会变成
 *   另一个实体 → 绑定的函数指针找不到定义（链接错，拆页时踩过）
 * ================================================================== */
/* ==================== IPTV：列表回调 ====================
 * 三个回调按 caption（ListIptv）由框架匹配调用，见 INIT_UI_EVENT_BINDINGS。
 * ⚠️ 行视图是**跨行复用**的：每行每个字段都要**写全**，不能依赖"上一行留下的状态"
 *    （老坑：用行号做缓存键会串行，见 docs/ui-design-baseline.md）。
 */
static int getListItemCount_ListIptv(const ZKListView *pListView) {
  (void)pListView;
  return gIptvCount;
}

/* "1280x720@25.00fps" → "1280x720@25fps"：列表副行省地方（原值仍在频道表里）。
 * 注意 29.97 这类**不能**动（只去掉 .00）。 */
static void briefSize(const char *src, char *out, int n) {
  int j = 0;
  for (int i = 0; src[i] && j < n - 1; ++i) {
    if (src[i] == '.' && src[i + 1] == '0' && src[i + 2] == '0') { i += 2; continue; }
    out[j++] = src[i];
  }
  out[j] = 0;
}

static void obtainListItemData_ListIptv(ZKListView *pListView,
                                        ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  if (!pListItem || index < 0 || index >= gIptvCount) return;
  const IptvChannel *c = &gIptvCh[index];

  /* ★ 2026-09-14 UI 改版：**这里不能再给行设底色** —— 行卡片的圆角底是 item 模板里
   * 的静态子控件 RowCard（九宫格 ios_card_row.9.png）。setBackgroundColor 画的是
   * 直角矩形，而且会把图的四角填成方的（"底色与图片互斥"，见 ui/main.html 的注释）。 */

  ZKListView::ZKListSubItem *nm = pListItem->findSubItemByID(ID_IPTV_SubChName);
  ZKListView::ZKListSubItem *info = pListItem->findSubItemByID(ID_IPTV_SubChInfo);
  ZKListView::ZKListSubItem *nt = pListItem->findSubItemByID(ID_IPTV_SubChNote);

  if (nm) {
    /* 名字前带两位序号：滚下去再回来时还能对上是第几个台 */
    char b[96];
    snprintf(b, sizeof(b), "%02d  %s", index + 1, c->name);
    nm->setText(b);
    nm->setTextColor(index == gIptvPlaying ? 0xFF30D158 : 0xFFF2F2F7);  // 正在播的高亮
  }
  if (info) {
    /* 副行：分组 · 实测清晰度 · 码率 —— 缺哪个少显示哪个（用户自备表可能没有实测） */
    char sz[32] = "";
    if (c->size[0]) briefSize(c->size, sz, sizeof(sz));
    char b[128];
    b[0] = 0;
    if (c->group[0]) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s", c->group);
    if (sz[0]) snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%s", b[0] ? " · " : "", sz);
    if (c->kbps[0])
      snprintf(b + strlen(b), sizeof(b) - strlen(b), "%s%s", b[0] ? " · " : "", c->kbps);
    info->setText(b);
    info->setTextColor(0xFF9A9AA0);
  }
  if (nt) {
    nt->setText(c->note);            // 空串 = 不显示（"仅限国内"/"非全天播出"）
    nt->setTextColor(0xFFFF9F0A);
  }
}

/* 点一行 = 播这个频道（与原来点频道槽等价）。 */
static void onListItemClick_ListIptv(ZKListView *pListView, int index, int id) {
  (void)pListView;
  (void)id;
  if (index < 0 || index >= gIptvCount) return;
  LOGD("iptvLogic: 点列表第 %d 行 -> %s（%s）", index, gIptvCh[index].name,
       gIptvCh[index].group);
  iptvPlayAt(index);
}

static bool onButtonClick_BtnIptvReload(ZKButton *p) {
  (void)p;
  int n = iptvLoadChannels();
  resetIptvCache();
  syncIptv();
  LOGD("PocketGame iptv: 重新加载频道表 → %d 个", n);
  return true;
}

/* 播放页三个键：上一个 / 停止 / 下一个 */
static bool onButtonClick_BtnIptvPrev(ZKButton *p) {
  (void)p;
  if (gIptvCount <= 0) return true;
  int i = (gIptvPlaying > 0) ? (gIptvPlaying - 1) : (gIptvCount - 1);
  iptvPlayAt(i);
  return true;
}
static bool onButtonClick_BtnIptvNext(ZKButton *p) {
  (void)p;
  if (gIptvCount <= 0) return true;
  int i = (gIptvPlaying >= 0 && gIptvPlaying + 1 < gIptvCount) ? (gIptvPlaying + 1) : 0;
  iptvPlayAt(i);
  return true;
}
static bool onButtonClick_BtnIptvStop(ZKButton *p) {
  (void)p;
  iptvStopAndBack();
  resetIptvCache();
  syncIptv();
  return true;
}static bool onButtonClick_BtnCh0(ZKButton* pButton) {
  LOGD_TRACE("BtnCh0 click");
  return false;
}

static bool onButtonClick_BtnCh1(ZKButton* pButton) {
  LOGD_TRACE("BtnCh1 click");
  return false;
}

static bool onButtonClick_BtnCh2(ZKButton* pButton) {
  LOGD_TRACE("BtnCh2 click");
  return false;
}

static bool onButtonClick_BtnCh3(ZKButton* pButton) {
  LOGD_TRACE("BtnCh3 click");
  return false;
}

static bool onButtonClick_BtnCh4(ZKButton* pButton) {
  LOGD_TRACE("BtnCh4 click");
  return false;
}

static bool onButtonClick_BtnCh5(ZKButton* pButton) {
  LOGD_TRACE("BtnCh5 click");
  return false;
}

static bool onButtonClick_BtnCh6(ZKButton* pButton) {
  LOGD_TRACE("BtnCh6 click");
  return false;
}

static bool onButtonClick_BtnCh7(ZKButton* pButton) {
  LOGD_TRACE("BtnCh7 click");
  return false;
}

static bool onButtonClick_BtnChPrev(ZKButton* pButton) {
  LOGD_TRACE("BtnChPrev click");
  return false;
}

static bool onButtonClick_BtnChNext(ZKButton* pButton) {
  LOGD_TRACE("BtnChNext click");
  return false;
}


