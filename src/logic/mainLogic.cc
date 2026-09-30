#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif // FUN_BUILD
#pragma once
#include "uart/ProtocolSender.h"

/*
 * mainLogic.cc - PocketGame 掌上游戏机 · UI 与业务关联层
 *
 * 职责边界（遵循 FlyThings 规范）：
 *   - 本文件只做「控件 <-> 业务对象」的关联：取控件指针、刷 HUD、路由输入、驱动主循环
 *   - 游戏逻辑与渲染在 src/core/*.cpp（不依赖 easyui，可单独编译验证）
 *   - 设备能力（画布 / 音效 / 最高分存档）在 src/platform/*.cpp
 *
 * 交互模型：
 *   - 主界面：游戏列表，条目由 pg::gameCount() 驱动 -> 加游戏自动多一张卡片
 *   - 游戏内：触摸操作；3 个物理按键 = A 确定/开始/暂停、B 返回列表、C 重玩
 *   - 游戏画面：ZKTextView(GameCanvas) 挂软渲染帧缓冲（见 platform/PgDisplay.cpp）
 */

#include <stdio.h>
#include <stdarg.h>   // iptvFail() 的可变参数（报错原因要带数值）
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>   // settimeofday：QA 钩子 ntp setclock（复现冷启动的 1970 时钟）
#include <time.h>
#include <netdb.h>      // getaddrinfo / gai_strerror（QA `dns test`）
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "core/PgGame.h"
#include "core/PgGames.h"
#include "core/PgLog.h"             // core 层日志钩子（素材尺寸不符这类"静默"问题要留痕）
#include "core/PgSprite.h"          // 游戏贴图仓库（运行时加载 PNG，见 docs/game-art-pipeline.md）
#include "platform/PgSpriteDevice.h" // 装上设备侧解码器（BitmapHelper）
#include "platform/PgSaver.h"
#include "platform/PgFlip.h"     // 整屏 180° 翻转（挂绳倒挂；键码也定义在这里）
#include "platform/PgSwipe.h"   // 右滑返回（手势判定与 navibar 共用一套）     // pg::wakeSaverByKey（屏保任意键唤醒）
#include "platform/PgNavi.h"      // 导航栏外部状态：页面级标题覆盖 + 视频播放页标志
#include "platform/PgSkin.h"      // pg::applyRoundedBg（动态底色的圆角形状）
#include "platform/PgAudio.h"
#include "platform/PgDisplay.h"
/* 蓝牙（platform/PgBt）不在这里用：蓝牙遥控是独立 ftu，逻辑在 logic/remoteLogic.cc */
#include "platform/PgDlna.h"
#include "platform/PgWifi.h"
#include "platform/PgTime.h"
#include "platform/PgFf.h"
#include "platform/PgStream.h"
#include "platform/PgStore.h"
#include "platform/PgBattery.h"   // 电量/充电状态（板级参数见 docs/hardware-reference.md）
#include "platform/PgRingtone.h"  // 闹钟铃声：官方音频播放器 zk_audio_player（见 docs/audio-output.md）
#include "platform/PgAlarm.h"     // 闹钟：守护线程 + 到点响铃/亮屏（任意页面都会响）
#include "platform/PgDns.h"       // 本地 DNS 中继（本板没有 /etc/resolv.conf）
#include "platform/PgHls.h"       // HLS（m3u8）客户端：拉分片 + 本地中继（IPTV 用）
#include "platform/PgH264.h"      // 硬件 H264 直接解码（zk_h264_player，缩放解码走这条）
#include "platform/PgVideoLayer.h"  // disp 视频层清理（视频停止后释放层，见其头文件说明）
/* 硬件能力（zkhardware 包，MCP `get_package_api` 查到；本板实测见 docs/clock-suite.md）：
 *   BRIGHTNESSHELPER  背光/亮屏/熄屏 —— 闹钟到点亮屏、屏保调暗都用它
 *   HARDWAREMANAGER   蜂鸣器 beep() / setBeepPWM(freq,duty) —— 闹钟响铃的省电选择 */
#include "utils/BrightnessHelper.h"
#include "hw/HardwareManager.h"
/* 框架自带的音频播放器（官方文档 multimedia/audio.md）。
 * ⚠️ 本板能否出声需要实测，见下面的 ZkProbe（判据：它打开 pcmC0D0p 还是 pcmC1D0p）。
 * 结论记在 docs/clock-suite.md，别凭注释猜。 */
#include "media/ZKMediaPlayer.h"
#include "manager/ConfigManager.h"   // CONFIGMANAGER->getResFilePath（定位 /tmp/ui/audio/*.wav）
#include "media/wav_utils.h"         // zk_wav_play：参考工程的官方音频播放器（zk_audio_player 封装）

using namespace pg;

/* 时钟套件的编辑辅助：定义在文件后部的"时钟套件回调"区（**全局作用域**）。
 * ⚠️ 声明必须也在**全局作用域**：QA 命令写在匿名 namespace 内、而定义在 namespace 之外，
 *    若把声明放进匿名 namespace 就会出现 "(anonymous)::f" 与 "::f" 两个候选
 *    → `call of overloaded ... is ambiguous`（这次踩过）。 */

/* ==================== ZKMediaPlayer 声卡探针（时钟套件选型用） ====================
 * 目的：**实测**框架自带的"音频播放器"在本板能否出声，不靠注释/传闻下结论。
 * 本板两块声卡：card0 = 片内 codec（**喇叭接这块**）、card1 = ES714X I2S DAC（没接喇叭）。
 * 判据（不需要耳朵）：播放期间 `cat /proc/<pid>/fd | grep pcm` ——
 *   打开 pcmC0D0p = 走 card0 = 能出声；pcmC1D0p = 走 card1 = 静音。
 * QA：`zkplay [<相对路径，默认 audio/over.wav>]` / `zkstop`
 * ⚠️ PgAudio 的常开流独占 hw:0,0，测之前必须先 `pcmfree` 让出声卡（否则打开会挂住）。 */
class ZkProbeListener : public ZKMediaPlayer::IPlayerMessageListener {
 public:
  void onPlayerMessage(ZKMediaPlayer *p, int msg, void *data) override {
    (void)p;
    (void)data;
    LOGD("PocketGame: ZKMediaPlayer 消息 msg=%d", msg);
  }
};
ZkProbeListener sZkProbeListener;
ZKMediaPlayer *sZkPlayer = nullptr;

/* ==================== 官方音频播放器探针（参考工程的正解） ====================
 * 参考工程 S:/.../V851ExtendedScreen_ap_p2p 的"出声音"路径（2026-09-13 查实）：
 *   src/media/wav_utils.c::zk_wav_play()  —— 解析 wav 头 → zk_audio_player_init
 *     → 循环 zk_audio_player_put_frame(4096) → zk_audio_player_deinit
 *   src/media/audio_context.cpp::init()  —— setenv("ALSA_CONFIG_DIR", "/res/ui/alsa")、
 *     zk_audio_player_set_max_volume()、change_output_mode(E_AUDIO_PLAYER_MODE_SPK)
 *   zk_audio_player_* 的实现**不在设备 rootfs**（/lib/libzkmedia.so 里没有这些符号），
 *   而在参考工程自带的 src/dependencies/lib/libzkmedia.a（含自带 tinyalsa 的 pcm.o/mixer.o）。
 *   ⇒ 已把该 .a 与 audio_player.h / wav_utils.{h,c} 搬进本工程（见 docs/clock-suite.md）。
 * QA：`zkwav [<相对路径，默认 audio/over.wav>]`（后台线程播，不卡主循环）
 * ⚠️ 测之前先 `pcmfree`：tinyalsa 打不开被 PgAudio 独占的 card0。 */
static void *zkWavThread(void *arg) {
  char *path = (char *)arg;
  int ret = zk_wav_play(path);
  LOGD("PocketGame: zk_wav_play('%s') 返回 %d（0=正常）", path, ret);
  free(path);
  return 0;
}

// 投屏/视频播放（DLNA 渲染端实验）：定义在 onUI_init 之前（**全局作用域**，
// 别写进下面的匿名 namespace —— 那样声明与定义会分属两个 namespace，链接不到）
void startCast(const char *pathOrUrl);
void stopCast();
/* 投屏接收端（DLNA DMR）的自动启动：定义在 tickDlna 之后（**全局作用域**） */
extern bool gDlnaUserOff;  // 定义在下面（全局作用域）：`dlna off` 后不再自动启动
void ensureDlna();

/* 网络校时（NTP）：https 证书校验要求系统时间正确，而本板没有 RTC（开机恒为 1970）。
 * 定义在 ensureDlna 之后（**全局作用域**）。 */
void ensureTimeSync();

/* ==================== DLNA 渲染端（DMR） ====================
 * 网络线程（SSDP/civetweb）只把动作投递到队列，UI 线程在主循环里 tickDlna() 取出来执行。
 * 启动：QA 命令 `dlna on`（默认控制端口 8200）。详见 platform/PgDlna.h。
 */
pg::Dlna *gDlna = 0;
static void dlnaSetStatus(const char *s);
/* 播放器里是否已经有媒体。⚠️ 控制器（手机）的时序是 SetAVTransportURI 后**立刻**发 Play，
 *    那时我们还在下载——若直接把这个 Play 转成 mCasterPtr->resume()，
 *    播放器会报 "resume called in an invalid state: 0"（实测踩过）。
 *    正确做法：没载入就只记"想播"的意图，等 startCast() 真正 play() 即可。 */
static bool sCastLoaded = false;
/* 我们自己镜像的播放状态（播放器没有"是否处于暂停"的查询接口）：
 *   只有"我们主动暂停过"才允许 resume —— 否则控制器的 Play 会在
 *   媒体尚未就绪/正在播放时打到暂停器上，播放器直接报
 *   "resume called in an invalid state: 0"（实测踩过，且状态会假报 PLAYING）。 */
static bool sCastPaused = false;
/* ⚠️ 播完别立刻删媒体文件：播放器收尾（释放/seek 到末尾）可能还要碰这个 fd，
 *    实测"一播完就 unlink"偶发整机重启（无 FATAL 栈，像被播放器内部带崩）。
 *    改成延迟 10s 删 —— 既让播放器彻底收尾，也把 tmpfs 内存还回去。 */
static long sCastCleanupAtMs = 0;

bool dlnaStart(int port) {
#ifdef FUN_BUILD
  /* ⚠️ 对象**长期存在、反复复用**（不 new 第二次）：
   *    下载线程是 detached 的，可能还在拉大文件；stop() 只让它自己退出。
   *    若 stop 后立刻 delete，那个线程醒来就会读已释放内存 ——
   *    实测症状：`dlna off` → `on` 之后出现**两份 SSDP 应答**、LOCATION 端口变 0
   *    （`http://192.168.0.125:0/description.xml`），控制器拿到也连不上。
   *    所以：实例只建一次；off 只是 stop（线程收工），on 再 start 复用同一对象。 */
  if (!gDlna) gDlna = new pg::Dlna();
  if (gDlna->running()) return true;
  return gDlna->start("PocketGame-DMR", port);
#else
  (void)port;
  return false;
#endif
}

void dlnaStop() {
  if (!gDlna) return;
  gDlna->stop();  // 线程收工；**不 delete**（见上面 dlnaStart 的说明）
}

static void dlnaSetStatus(const char *s) {
#ifdef FUN_BUILD
  if (mTextCastMsgPtr) mTextCastMsgPtr->setText(s);
#else
  (void)s;
#endif
}

/* 在线流播放是否占着投屏页（跑完要收掉，见主循环里的轮询） */
volatile int gStreamPageShown = 0;

/* 投屏页是否**已经出画面**（框架播放器链路：PLAY_STARTED 回报；在线流链路：
 * 主循环按解码帧数判 —— 见 tickVideoCover）。用来决定"缓冲底图"收不收。 */
volatile int gCastPainted = 0;
/* 0 = 不动；1 = 请主循环亮出投屏页；2 = 请主循环收掉投屏页
 * （投屏页显隐函数定义在全局作用域，这里不能直接调，见 streamshow 的注释） */
volatile int gStreamWantPage = 0;
/* 1 = 当前"在播"的是我们自己的在线流链路（PgStream），0 = 框架播放器（本地文件）。
 * DLNA 的 Play/Pause/Stop/进度回报都要按这个分流。 */
volatile int gStreamCasting = 0;

/* QA 命令里 "<url> [秒]" 的拆解（streamtest / streamshow 共用） */

// 主循环里执行 DLNA 动作
void tickDlna() {
#ifdef FUN_BUILD
  if (!gDlna) return;
  pg::DlnaAction a;
  while (gDlna->pollAction(&a)) {
    switch (a.type) {
      case pg::DlnaAction::PLAY: {
        /* ⚠️ 按源分两条路（这是"投屏不该下载"的落地点）：
         *   http/https → **在线流直连**：交给 PgStream（ffmpeg 解封装 + 设备硬解 + VO 上屏）。
         *                 **绝不下载到 /tmp** —— /tmp 是 tmpfs（就是内存），整片下载必然 OOM。
         *   本地文件   → 原来的框架播放器路径（ZKVideoView 播本地文件）。 */
        bool isUrl = (strncmp(a.path, "http://", 7) == 0 || strncmp(a.path, "https://", 8) == 0);
        LOGD("PocketGame dlna: 播放 %s（%s）", a.path, isUrl ? "在线流直连" : "本地文件");
        if (isUrl) {
          if (pg::StreamPlayer::startWithDisplay(a.path, 0, 0, 0, 480, 700)) {
            gStreamWantPage = 1;  // 主循环下一拍亮出投屏页（= 视频层的"透明窗口"）
            gStreamCasting = 1;
            gDlna->setTransportState("PLAYING");
            dlnaSetStatus("DLNA 投屏中（在线流）");
          } else {
            LOGW("PocketGame dlna: 在线流启动失败（多半是已有播放任务在跑）");
            gDlna->setTransportState("STOPPED");
            dlnaSetStatus("投屏失败：已有播放任务");
          }
        } else {
          startCast(a.path);
          gStreamCasting = 0;
          gDlna->setTransportState("PLAYING");
          dlnaSetStatus("DLNA 投屏中");
        }
        break;
      }
      case pg::DlnaAction::RESUME:
        if (gStreamCasting) {
          if (!pg::StreamPlayer::running()) {
            /* 流式播放当前实现没有"暂停"，所以 Pause 走的是停掉；这里的 Play 只能重开（从头播） */
            LOGD("PocketGame dlna: 在线流重开（流式播放不支持断点续播，从头开始）");
            if (pg::StreamPlayer::restart()) gStreamWantPage = 1;
          }
          gDlna->setTransportState("PLAYING");
        } else if (sCastLoaded && sCastPaused && mCasterPtr) {
          mCasterPtr->resume();
          sCastPaused = false;
          gDlna->setTransportState("PLAYING");
        } else if (sCastLoaded) {
          gDlna->setTransportState("PLAYING");  // 已在播，回个状态就好，别动播放器
        } else {
          LOGD("PocketGame dlna: Play 到达时媒体未就绪，等就绪后自动起播");
        }
        break;
      case pg::DlnaAction::PAUSE:
        if (gStreamCasting) {
          /* 如实处理：在线流暂不支持暂停 → 停掉画面，状态回报 PAUSED_PLAYBACK（Play 会从头重开） */
          LOGW("PocketGame dlna: 在线流暂不支持暂停 —— 按停止处理（再次 Play 会从头播）");
          pg::StreamPlayer::stop();
          gDlna->setTransportState("PAUSED_PLAYBACK");
          dlnaSetStatus("DLNA 已暂停（在线流为停止）");
        } else if (sCastLoaded && mCasterPtr) {
          mCasterPtr->pause();
          sCastPaused = true;
          gDlna->setTransportState("PAUSED_PLAYBACK");
          dlnaSetStatus("DLNA 已暂停");
        }
        break;
      case pg::DlnaAction::STOP:
        if (gStreamCasting) {
          pg::StreamPlayer::stop();
          gStreamCasting = 0;
        }
        stopCast();
        gDlna->cleanupMedia();  // 本地文件路径才可能有 /tmp 残留；流式路径压根没文件
        gDlna->setTransportState("STOPPED");
        break;
      case pg::DlnaAction::SEEK:
        if (gStreamCasting) {
          LOGW("PocketGame dlna: 在线流暂不支持拖动进度");
        } else if (sCastLoaded && mCasterPtr) {
          mCasterPtr->seekTo(a.arg);
        }
        break;
      case pg::DlnaAction::SET_VOLUME:
        if (gStreamCasting) {
          LOGW("PocketGame dlna: 在线流的音量还没接入（请求 %d）", a.arg);
        } else if (sCastLoaded && mCasterPtr) {
          mCasterPtr->setVolume(a.arg / 100.0f);
        }
        break;
      default:
        break;
    }
  }
  /* 位置/时长回报（控制器会轮询 GetPositionInfo / GetMediaInfo）—— 按当前在跑的链路取数 */
  if (gStreamCasting) {
    long long d = pg::StreamPlayer::durationMs();
    long long p = pg::StreamPlayer::positionMs();
    if (d > 0) gDlna->setDurationMs((int)d);
    if (p >= 0) gDlna->setPositionMs((int)p);
  } else if (mCasterPtr && mCasterPtr->isPlaying()) {
    gDlna->setPositionMs(mCasterPtr->getCurrentPosition());
    gDlna->setDurationMs(mCasterPtr->getDuration());
  }
#endif
}

/* ⚠️ IPTV（WinIptv/WinIptvPlay）已于 2026-09-14 拆成独立 ftu（iptv.ftu / iptvActivity），
 * 状态量与相位机都在 src/logic/iptvLogic.cc 里，本文件不再引用。 */
/* ⚠️ tickVideoCover 不要在这里声明：它的**定义**在匿名命名空间里、且在使用点之前
 *    （2526 vs 3160），再加一个全局声明就变成两个实体 → "call of overloaded ... is ambiguous"
 *    （老坑：pgHost 同款）。 */

/* 投屏页显隐（定义在匿名 namespace 之后，但 h264play 等 QA 命令要用 —— 必须在这里声明） */
void showCastPageForStream(const char *tip);
void hideCastPageForStream();

namespace {

/* ==================== 画布参数（与 ui/main.json 的 GameCanvas 一致） ==================== */
const int CANVAS_X = 0;
const int CANVAS_Y = 160;
const int CANVAS_W = 480;
const int CANVAS_H = 540;

/* 主界面右下角显示的版本号。
 * ⚠️ 只在这一处定义（`ui/main.html` 里那行是首帧占位，真值在这里 setText）。
 * 与 `tools/upgrade_device.sh <版本>` 传的**固化版本**保持一致 —— 那是随包发布的版本号来源。
 * 历史沿革：v1.2（初版）→ v1.7.x（在线流/投屏）→ v1.8.x（旋转/稳定性/电池）。 */
const char *kAppVersion = "PocketGame v1.32.3";

/* ==================== 主循环 ==================== */
const int TIMER_LOOP = 0;
/* 定时器周期（**实际 dt 一律用实测值**，见 TIMER_LOOP 里的说明）。两个档：
 *   LOOP_MS      33ms ≈ 30fps —— 列表/工具页（省电，那儿没有连续动画）
 *   LOOP_MS_GAME 16ms ≈ 60fps —— **游戏内**（用户 2026-09-15 明确要求"设计要做到 60fps"）
 * 为什么不干脆全局 60fps：主循环里还挂着巡检、电量、校时、投屏自愈等一堆事，
 * 列表页把它们跑两倍频纯属白耗电（本机是便携设备）。
 * 切换用 `resetTimer(TIMER_LOOP, ...)`，在 startGame / exitGameToMenu 里各切一次。
 * ★ 素材化之后游戏渲染不再是瓶颈（贴图 vs 逐像素几何），实测 33fps 时已经**打满定时器**
 *   （QA `fps` = 33 次/秒），所以这里放开周期就能直接拿到更高帧率。 */
const int LOOP_MS = 33;
const int LOOP_MS_GAME = 16;

/* QA `fps` 探针（2026-09-15 加）：统计 2 秒内主循环跑了几次。
 * 为什么要它：本工程是**软渲染全屏重绘**，帧率是"手感/耗时"的第一手数据，
 * 而设备上没有 top/vmstat 之类的工具（shell 也没有 sleep/awk）。 */
static int sFpsProbe = 0;      // >0 = 正在统计
static int sFpsCnt = 0;
static long long sFpsT0 = 0;

/* QA `bench` 探针（2026-09-15 加）：把**每帧的时间拆开**——update / render / present
 * 各占多少微秒。
 * 为什么必须拆：帧率低的时候"渲染慢"只是猜测，实际瓶颈可能是
 *   ① 游戏自己的 render（几何绘制/贴图）
 *   ② present（把 480x540 的位图交给框架合成上屏，走 G2D）
 *   ③ 主循环里挂着的其它巡检
 * 三者优化手段完全不同。**先量再改**（血案：把小鸟的背景渐变当成瓶颈优化掉了，
 * 帧率一点没动 —— 真正吃时间的是别处）。 */
static int sBenchProbe = 0;
static long long sBenchUpd = 0, sBenchRnd = 0, sBenchPre = 0, sBenchLoop = 0;
static int sBenchFrames = 0;
static long long sBenchT0 = 0;

enum AppMode {
  MODE_MENU = 0,
  MODE_GAME,   // 画布类应用（游戏）：软渲染帧缓冲挂到 ZKTextView
  MODE_TOOL,   // 原生控件类应用（番茄钟/定时器/秒表/计算器）：控件由 syncToolUi 驱动
};

/* ==================== 运行期对象 ==================== */

/* 单调毫秒时钟（定义在本文件的匿名命名空间里，位置在后面）。
 * 这里先声明：GameHostImpl::playSfx 要用它量"拿回声卡花了多久"。
 * ⚠️ 声明必须与定义在**同一个（匿名）命名空间**里，否则会造出第二个实体 →
 *    调用报 "ambiguous"（本工程踩过：runAutoCmd / showCastPageForStream）。 */
static long pgNowMs();

// 宿主实现：把音效与最高分存储接到游戏引擎
class GameHostImpl : public Host {
 public:
  GameHostImpl() {}

  bool init() {
    store.load();
    audio.init();
    /* 把音频实例暴露给进程级：PgStream 要往这条常开 PCM 流里灌在线流的音频
     * （框架 AO 那条路会自己拼 hw:1,0 → 本板完全静音，所以不能用它）。 */
    pg::setGlobalAudio(&audio);
    // 用存档里的音效开关初始化（主界面按钮显示同一状态）
    audio.setEnabled(store.soundOn());
    // 恢复上次音量（存档里没有 volume= 这一行时 volumePercent() = -1，保持驱动默认）
    if (store.volumePercent() >= 0) {
      int got = audio.setVolumePercent(store.volumePercent());
      // 告诉 pg 层"当前音量是多少"：静音被取消时要用它恢复（见 pg::noteVolumePercent）
      if (got >= 0) pg::noteVolumePercent(got);
    }
    return true;
  }

  void playSfx(int sfxId) {
    /* 声卡可能已被"空闲自动让出"放掉（tickPcmIdle）→ 有声音需求就先拿回来。
     * 代价：让出后的**第一次**出声要先 openStream + 枚举输出开关（实测 ~100ms 量级），
     * 之后就是常开流不 pop 的老样子。不要这个特性的用 QA `pcmidle 0` 关掉。 */
    if (!audio.pcmHeld()) {
      long long t0 = pgNowMs();
      audio.acquirePcm();
      LOGD("PocketGame: 声卡空闲已被让出 → 拿回耗时 %lldms（首次出声会稍晚）",
           pgNowMs() - t0);
    }
    audio.playSfx(sfxId);
  }
  int highScore(int index) const { return store.high(index); }
  void setHighScore(int index, int score) { store.setHigh(index, score); }

  // ---- 系统能力：WiFi（实现在 platform/PgWifi，core 层不依赖 zknet）----
  bool wifiSupported() { return WifiService::supported(); }
  bool wifiEnabled() { return WifiService::enabled(); }
  bool wifiConnected() { return WifiService::connected(); }
  void wifiSsid(char *buf, int n) { WifiService::ssid(buf, n); }
  void wifiIp(char *buf, int n) { WifiService::ip(buf, n); }
  void wifiMac(char *buf, int n) { WifiService::mac(buf, n); }
  int wifiRssi() { return WifiService::rssi(); }
  void wifiSetEnabled(bool on) { WifiService::setEnabled(on); }
  void openWifiSettings() { WifiService::openSystemSettings(); }

  /* 蓝牙（平台层 pg::Bt）**不再从这里转发**：蓝牙遥控已是独立 ftu（remote.ftu /
   * remoteActivity），界面逻辑在 src/logic/remoteLogic.cc 里直接调
   * `pg::Bt::instance()`。这样"独立功能 = 独立 ftu + 独立 logic"，主界面不必
   * 为了别的应用多挂一层转发（见 ui/remote.html 头部说明）。 */

  bool soundOn() const { return audio.enabled(); }
  /* 音量读写（系统设置页用）：**走同一份存档**，所以设置页调完与音量键调完等价。 */
  int volumePercent() const { return store.volumePercent(); }
  void setVolumePercent(int pct) {
    int got = audio.setVolumePercent(pct);
    if (got >= 0) {
      store.setVolumePercent(got);      // 落盘：重启后 init() 会恢复
      pg::noteVolumePercent(got);       // 让"取消静音时恢复音量"知道当前值
    }
  }
  // 切换音效开关：生效 + 落盘
  void setSoundOn(bool on) {
    audio.setEnabled(on);
    store.setSoundOn(on);
  }
  const char *audioBackend() const { return audio.backend(); }

  DeviceAudio audio;
  ScoreStore store;
};

// 物理按键 -> 逻辑键
//
// 2026-09-12 语义重定义：三个物理键现在是「音量- / 音量+ / 暂停」，不再是 A/B/C。
// 名字保留 PHYS_KEY_A/B/C 是为了不动其它引用点；语义以注释为准：
//   PHYS_KEY_A -> 暂停 / 继续（游戏内）· 开始 / 暂停（工具页）   ← 物理键 108
//   PHYS_KEY_B -> 返回列表（仅外接键盘 ESC / BACK）              ← 物理键已不映射
//   PHYS_KEY_C -> 重玩本局（仅外接键盘 MENU / KEY_RIGHT）        ← 物理键已不映射
enum {
  PHYS_KEY_A = 0,  // 暂停/继续（游戏内）· 开始/暂停（工具页）
  PHYS_KEY_B = 1,  // 返回列表（仅外接键盘）
  PHYS_KEY_C = 2,  // 重玩（仅外接键盘）
  PHYS_KEY_VOL_DOWN = 3,  // 音量 -（物理键 103）
  PHYS_KEY_VOL_UP = 4,    // 音量 +（物理键 105）
};

// 陌生板子自动认领三个未知键时的顺序，与物理键 103/105/108 一致
static const int kSlotLogical[3] = {PHYS_KEY_VOL_DOWN, PHYS_KEY_VOL_UP, PHYS_KEY_A};
static const char *kSlotName[3] = {"音量-", "音量+", "暂停"};

GameHostImpl *gHost = 0;
Game *gGame = 0;
int gIndex = -1;
DeviceDisplay *gDisplay = 0;
AppMode gMode = MODE_MENU;
bool gTouchInsideCanvas = false;
/*
 * 原生暂停弹窗（mWinPause）是否正在显示。
 *
 * ⚠️ 触摸派发的开关条件必须是"弹窗在显示"，而不是"游戏处于 PAUSED 状态"：
 * 贪吃蛇是"点一下 = 暂停/继续"——游戏在 onTouch 里自己把状态改成 GSTATE_PAUSED，
 * 只在画布内画遮罩，**不会**弹 mWinPause。若按状态拦触摸，暂停后的所有触摸都被
 * logic 层吞掉，游戏的"已暂停 → 点击继续"分支永远收不到事件，
 * 症状就是"能暂停、不能继续"（2026-09-12 用户实机复现）。
 * 只有 setPaused(true) 弹出原生弹窗时，触摸才让给弹窗按钮。
 */
bool gPauseWinShowing = false;

// HUD 缓存：只有值变化才 setText，避免每帧刷控件造成无谓重绘
struct HudCache {
  int score;
  int best;
  // info 值用字符串：工具类应用要显示 "05:00" / "1/5" / "25分" 这种非纯数字
  char info1[24];
  char info2[24];
  int nums;  // 数字区（得分/最高分）是否可见
  char title[64];
  char hint[160];
  char keybar[160];
  char i1l[32];
  char i2l[32];
  void reset() {
    score = -1;
    best = -1;
    info1[0] = 0;
    info2[0] = 0;
    nums = -1;
    title[0] = 0;
    hint[0] = 0;
    keybar[0] = 0;
    i1l[0] = 0;
    i2l[0] = 0;
  }
};
HudCache gHud;

// 当前主界面分类（游戏 / 工具 / 系统）
int gCategory = APP_GAME;

/*
 * 主菜单列表的「待刷新」计数：>0 时由主循环调用 refreshList()。
 *
 * 为什么不直接在 onUI_init / onUI_show 里同步刷：
 *   - 实测「改了 ftu 之后 fun launch」这条路线上，框架会**热重载布局**（列表控件被换成新实例），
 *     而 onUI_init 不会再跑 → 列表里一条都没有：静态的标题/tab/提示都在，唯独动态填充的
 *     卡片全空（截图见过一次：列表区 100% 是背景色），直到用户切一次 tab 才恢复；
 *   - 同步调用本身也不保险：窗口/列表此时可能还没布局，刷了也会丢。
 * 所以统一改成"标脏 + 主循环里刷"，主循环跑起来时窗口一定已经就绪。
 */
int gMenuDirty = 0;

/*
 * 音效顺序自检（QA / 产线自检）：把 1..SFX_COUNT-1 逐个播一遍，每声间隔 1.2s。
 * 本板注入不了触摸、也不能靠耳朵自动判定，所以让机器"点着数"播、人只需听。
 * 由 QA 命令 sfxseq 触发，主循环推进（任何模式下都跑）。
 */
int gSfxSeqIdx = 0;       // 0=未在跑；1..SFX_COUNT-1 = 下一个要播的序号
int gSfxSeqWaitMs = 0;

const char *kSfxNames[] = {"-",     "click", "move", "rotate", "drop", "clear",
                           "merge", "score", "hit",  "over",   "jump", "shoot"};
// 画布触摸派发（QA 脚本也会用，所以要前置声明）
bool dispatchCanvasTouch(int act, int x, int y);
// 本 activity 是否在前台：不在前台时按键不拦（否则会把系统设置页的返回键吃掉）
bool gActivityActive = true;
void syncTabs();
void switchCategory(int cat);

void startGame(int slot);
void exitGameToMenu();

/* IPTV 的前置声明**不能放这里**：本段在匿名 namespace 内，而 IPTV 的实现写在
 * 匿名 namespace 之外（文件靠后的"IPTV 网络电视"区）—— 在这里声明会造出
 * `{anonymous}::syncIptv` 这个**另一个实体**，链接期报 "used but never defined"。
 * 正确的声明位置在下面 `}  // namespace` 之后。 */

void togglePause();
void setPaused(bool paused);
void syncHud(bool force);
void refreshList();
bool handleLogicalKey(int logical);
// 画布底部软按钮：绘制 + 按下态（实现见下面"画布触摸派发"区）
void drawSoftButtons(Canvas &c);

// 原生工具页面（MODE_TOOL）的同步：定义在下面"列表辅助"区
void syncToolUi();

void resetToolUiCache();

/* ==================== 物理按键监听 ==================== */

#ifdef FUN_BUILD

/*
 * 三个物理按键的键码 —— 本机实测，来源：`adb shell getevent` + gpio-keys 能力位
 *
 *   设备：soc@03000000:gpio-keys -> /dev/input/event3
 *   能力位 KEY=1280 0 0 0（该文件是高位字在前）= word3 的 bit7/9/12 -> 103/105/108
 *   实测上报：0x67 = 103 = KEY_UP、0x69 = 105 = KEY_LEFT、0x6c = 108 = KEY_DOWN
 *
 * 映射（换机器只改这三行；拿不准就先看日志里的 `PocketGame key: raw code=...`）：
 *   103 KEY_UP   -> 音量 -
 *   105 KEY_LEFT -> 音量 +
 *   108 KEY_DOWN -> 暂停 / 继续
 *
 * 2026-09-12 按产品要求重定义（原为 A确定 / B返回列表 / C重玩本局）。
 * "返回列表 / 重玩本局"不再占物理键 —— 由「暂停键 → 暂停弹窗」里的按钮承担
 * （弹窗本来就有 继续游戏 / 重新开始 / 返回游戏列表 三个按钮），触摸也可达；
 * 外接键盘的 ESC/BACK、MENU/KEY_RIGHT 仍分别映射到 返回 / 重玩，供调试。
 */
enum {
  PG_KEYCODE_VOL_DOWN = pg::PG_KEY_CODE_VOL_DOWN,  // 103 KEY_UP   -> 音量 -
  PG_KEYCODE_VOL_UP = pg::PG_KEY_CODE_VOL_UP,      // 105 KEY_LEFT -> 音量 +
  PG_KEYCODE_PAUSE = pg::PG_KEY_CODE_C,            // 108 KEY_DOWN -> 暂停/继续（物理键"C"）
  /* ⚠️ 键码只在 platform/PgFlip.h 里定义一次：屏保页（"C 键=翻转 180°"）与这里必须同源，
   *    各写一份迟早漂（本项目"同一份真值不许两处手抄"是吃过亏的规则）。 */
};

// 长按判定阈值（应用层按 DOWN->UP 时长判定，见 KeyRouter::onKeyEvent）
const int PG_LONG_PRESS_MS = 700;

class KeyRouter : public EasyUIContext::IKeyListener {
 public:
  /* ★ 长按达标**立刻**执行返回动作，不等按键抬起（主循环每帧调）。
   *   本板 gpio-keys **没有 autorepeat** ⇒ 长按期间内核一个事件都不发；只在 E_KEY_UP 里
   *   判长按的话，用户按满 700ms 还得一直按到松手才返回（"按了不动、松手才跳"）。
   *   主循环 ~33ms 一拍，判定精度足够。 */
  void tickLongPress() {
    if (!gActivityActive) return;              // 本界面不在前台不拦（与 onKeyEvent 一致）
    if (sDownCode < 0 || sDownLogical < 0 || sLongFired) return;
    if (nowMs() - sDownMs < PG_LONG_PRESS_MS) return;
    sLongFired = true;                         // 抬起时不再重复触发
    LOGD("PocketGame key: code=%d 按住 >= %dms -> 判定为长按（不等抬起）", sDownCode,
         PG_LONG_PRESS_MS);
    longPressAction(sDownLogical);
  }

  virtual bool onKeyEvent(const KeyEvent &ke) {
    /* ★ 屏保开着：任意键只负责**唤醒**（本次按键吞掉，不触发页面动作）。
     *   放在最前面 —— 屏保时主界面不活跃（下面那句会 return false 放行），
     *   但"唤醒"这件事仍然要在这里做（否则可能没人做）。见 platform/PgSaver.h。 */
    if (pg::wakeSaverByKey(ke)) return true;
    // 我们的界面不在前台（比如系统设置页盖在上面）时不拦按键
    if (!gActivityActive) {
      sDownCode = -1;
      return false;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      // 原始键码日志：换板子/排查伪按键时照这行校准
      LOGD("PocketGame key: raw code=%d status=%d", ke.mKeyCode,
           (int)ke.mKeyStatus);
      /*
       * ⚠️ 短按/长按统一在「抬起」时判定（2026-09-12 追坑）：
       * 本板 gpio-keys 没开 autorepeat（getevent -p 无 REP 能力位），内核
       * 永远不会发 repeat 事件，框架的 E_KEY_LONG_PRESS 在**真机实体键**上
       * 永远不触发（pgkey 注入能触发是因为它自己模拟了 repeat）。
       * 所以按下只记时间戳，抬起时按「按住时长 >= 700ms = 长按」判定，
       * 不依赖内核/框架的任何长按机制。USB 键盘等有 repeat 的输入源仍走
       * 下面 E_KEY_LONG_PRESS 分支，两条路都保留。
       */
      sDownCode = ke.mKeyCode;
      sDownLogical = mapKeyCode(ke.mKeyCode);
      sDownMs = nowMs();
      sLongFired = false;
      return true;  // 按下先吞掉，不做任何动作
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) {
      // 只有会发 autorepeat 的输入源（外接 USB 键盘等）才走这里
      int logical = (ke.mKeyCode == sDownCode)
                        ? sDownLogical
                        : mapKeyCode(ke.mKeyCode);
      sLongFired = true;
      LOGD("PocketGame key: code=%d 长按(repeat 路径) -> %s", ke.mKeyCode,
           logical == PHYS_KEY_A ? "暂停键" : "其它");
      longPressAction(logical);
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_UP) {
      if (ke.mKeyCode != sDownCode || sDownLogical < 0) return false;
      int held = (int)(nowMs() - sDownMs);
      int logical = sDownLogical;  // 先存局部，下面清状态后日志还要用
      sDownCode = -1;
      sDownLogical = -1;
      if (sLongFired) return true;  // repeat 路径已经处理过长按，抬起不再触发
      if (held >= PG_LONG_PRESS_MS) {
        LOGD("PocketGame key: code=%d 按住 %dms -> 判定为长按", ke.mKeyCode,
             held);
        longPressAction(logical);
        return true;
      }
      // 短按：走原有逻辑
      static const char *kLogicalName[] = {"暂停", "返回", "重玩", "音量-",
                                           "音量+"};
      LOGD("PocketGame key: code=%d -> %s (短按 %dms, mode=%d)", ke.mKeyCode,
           (logical >= 0 && logical < 5) ? kLogicalName[logical] : "?", held,
           (int)gMode);
      return handleLogicalKey(logical);
    }
    return false;
  }

 private:
  static long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
  }
  /*
   * 长按暂停键 = 返回上一级（回应用列表）。
   *
   * 这条路径是**必须**的：三个物理键改成 音量±/暂停 之后，"返回"不再有物理键，
   * 而工具类应用（番茄钟/计算器/WiFi…）的页面上**没有任何触摸"返回"按钮** ——
   * 没有它，那些页面进得去出不来。
   * 短按仍然是 暂停/继续，只有长按才返回。
   */
  void longPressAction(int logical) {
    if (logical == PHYS_KEY_A && gMode != MODE_MENU) {
      if (mWinPausePtr) mWinPausePtr->hideWnd();
      gPauseWinShowing = false;
      LOGD("PocketGame: 长按暂停键 -> 返回列表");
      exitGameToMenu();
      return;
    }
    /* ★ 音量键长按 = 静音切换（2026-09-16 用户需求「音量弹出框也可以快速静音」的
     *   第二条入口：在游戏/主界面里不用去点弹框，长按音量键就静音）。
     *   ⚠️ 短按仍然是调音量（handleLogicalKey），只有长按 ≥700ms 才切静音 ——
     *     两者靠 sLongFired 互斥，抬手不会再补一次短按动作。 */
    if (logical == PHYS_KEY_VOL_UP || logical == PHYS_KEY_VOL_DOWN) {
      const bool want = !pg::isMutedGlobal();
      const bool ok = pg::setMutedGlobal(want);
      LOGD("PocketGame: 长按音量键 -> %s（%s）", want ? "静音" : "取消静音",
           ok ? "成功" : "失败");
      if (gHost) gHost->playSfx(SFX_CLICK);
    }
  }

  int mapKeyCode(int code) {
    switch (code) {
      // ---- 本机 gpio-keys 实测键码（2026-09-12 语义重定义）----
      case PG_KEYCODE_VOL_DOWN:
        return PHYS_KEY_VOL_DOWN;
      case PG_KEYCODE_VOL_UP:
        return PHYS_KEY_VOL_UP;
      case PG_KEYCODE_PAUSE:
        return PHYS_KEY_A;  // 暂停键复用原 A 的全部逻辑
      // ---- 同系列板子可能多出的方向键 ----
      case 106:  // KEY_RIGHT
        return PHYS_KEY_C;
      // ---- 常见通用键码（外接 USB 键盘 / 其它模组调试用）----
      case 28:   // KEY_ENTER
      case 57:   // KEY_SPACE
        return PHYS_KEY_A;
      case 1:    // KEY_ESC
      case 158:  // KEY_BACK
        return PHYS_KEY_B;
      case 127:  // KEY_MENU
      case 139:  // KEY_MENU(旧)
        return PHYS_KEY_C;
      default:
        break;
    }
    for (int i = 0; i < 3; ++i) {
      if (sKnown[i] == code) return kSlotLogical[i];
    }
    for (int i = 0; i < 3; ++i) {
      if (sKnown[i] == 0) {
        sKnown[i] = code;
        LOGW("PocketGame key: unknown code %d mapped to slot %d (%s)", code, i,
             kSlotName[i]);
        return kSlotLogical[i];
      }
    }
    return PHYS_KEY_A;
  }

  static int sKnown[3];
  // DOWN/UP 长按判定状态（见 onKeyEvent 里 E_KEY_DOWN 分支的说明）
  static int sDownCode;      // 已按下未抬起的原始键码，-1 = 无
  static int sDownLogical;   // 该键映射后的逻辑键
  static long sDownMs;       // 按下时刻（CLOCK_MONOTONIC 毫秒）
  static bool sLongFired;    // repeat 路径是否已触发过长按
};

int KeyRouter::sKnown[3] = {0, 0, 0};
int KeyRouter::sDownCode = -1;
int KeyRouter::sDownLogical = -1;
long KeyRouter::sDownMs = 0;
bool KeyRouter::sLongFired = false;
KeyRouter gKeyRouter;

/* ==================== 致命信号回溯（排查现场用） ====================
 * 设备上 fd 1/2 指向 /dev/null，崩溃信息看不到；且 zkgui 是 init 的 `class main`
 * 服务，一退出就被重启，现象只是"应用莫名其妙重开一遍"。
 * 这里装一个信号处理器：把信号号、故障地址、PC/LR 及其符号名打到 logcat，
 * 事后可用工具链 addr2line 反查源码行：
 *   arm-unknown-linux-musleabihf-addr2line -f -C -e libzkgui.so <pc-基址>
 */
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>

static void pgFatalHandler(int sig, siginfo_t *info, void *uctx) {
  ucontext_t *u = (ucontext_t *)uctx;
  unsigned long pc = 0, lr = 0, sp = 0;
  if (u) {
    pc = (unsigned long)u->uc_mcontext.arm_pc;
    lr = (unsigned long)u->uc_mcontext.arm_lr;
    sp = (unsigned long)u->uc_mcontext.arm_sp;
  }
  LOGD("PocketGame: !!FATAL!! sig=%d fault_addr=%p pc=0x%lx lr=0x%lx sp=0x%lx",
       sig, info ? info->si_addr : (void *)0, pc, lr, sp);
  const unsigned long adrs[2] = {pc, lr};
  for (int i = 0; i < 2; ++i) {
    Dl_info di;
    if (adrs[i] && dladdr((void *)adrs[i], &di)) {
      LOGD("PocketGame:   [%d] 0x%lx = %s + 0x%lx (%s)", i, adrs[i],
           di.dli_fname ? di.dli_fname : "?",
           di.dli_fbase ? adrs[i] - (unsigned long)di.dli_fbase : 0,
           di.dli_sname ? di.dli_sname : "?");
    } else {
      LOGD("PocketGame:   [%d] 0x%lx = ?", i, adrs[i]);
    }
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static void installFatalHandler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = pgFatalHandler;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
  sigaction(SIGABRT, &sa, 0);
  sigaction(SIGILL, &sa, 0);
}

#endif  // FUN_BUILD

/* ==================== 音量 OSD：**已收归状态栏**（2026-09-14） ====================
 * 原先是主界面自己的 WinVolume 面板 + WiFi 页再抄一份，工具页/套件/IPTV 上按音量键
 * **只有声音、没有界面**（用户报："音量弹框在其他界面看不到"）。
 * 现在统一到**全局状态栏**（APP_TYPE_SYS_STATUSBAR 的 SysApp，悬浮在所有页面之上）：
 *   src/logic/statusbarLogic.cc + ui/statusbar.html
 * 数据流：任意页面 → `pg::volumeStepGlobal()` → PgAudio 广播 → 状态栏弹面板、1.6s 自动收起。
 * ⇒ 本文件**不再持有任何音量 OSD 控件**，音量键只负责"改音量 + 播提示音"。
 */
static long pgNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 微秒版时间（QA `bench` 用）。为什么需要：单帧渲染常常只有几百微秒到几毫秒，
 * 毫秒精度会把"渲染 0.4ms"和"渲染 0.9ms"都记成 0/1，根本看不出谁在吃时间。 */
static long long pgNowUs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}
bool handleLogicalKey(int logical) {
  /* 闹钟响铃时：**任意键先停铃**（并吃掉这次按键）—— 响铃是最高优先级的"打断"，
   * 用户睡得迷糊时随手按哪个键都该停下来（不用记哪个键是"停止"）。
   * 触摸侧同理，见 dispatchCanvasTouch / onmainActivityTouchEvent 的开头。 */
  if (pg::Alarm::instance().ringing()) {
    pg::Alarm::instance().stopRing();
    LOGD("PocketGame: 按键停铃（logical=%d）", logical);
    return true;
  }
  // 音量键：不分模式，任何页面都能调（音量是硬件级操作，掌机习惯如此）。
  // 调整后弹出音量进度条 OSD + 播一声 click（听觉 + 视觉双反馈）。
  if (logical == PHYS_KEY_VOL_DOWN || logical == PHYS_KEY_VOL_UP) {
    if (gHost) {
      /* ⚠️ 走 `pg::volumeStepGlobal`（而不是直接 gHost->audio.volumeStep）：
       *    它是全工程音量的**唯一收口点**，PgAudio 在那里向状态栏广播
       *    ⇒ 全局音量 OSD 才弹得出来（见 PgAudio.h 的 setVolumeNotifyHook 说明）。 */
      int pct = pg::volumeStepGlobal(logical == PHYS_KEY_VOL_UP ? 1 : -1);
      if (pct >= 0) {
        LOGD("PocketGame: 音量%s -> %d%%",
             logical == PHYS_KEY_VOL_UP ? "+" : "-", pct);
        gHost->store.setVolumePercent(pct);  // 落盘，重启后由 init() 恢复
        gHost->playSfx(SFX_CLICK);           // 听觉反馈；视觉反馈由状态栏给
      } else {
        LOGW("PocketGame: 音量调节失败（看 PgAudio 的 WARN）");
      }
    }
    return true;
  }

  if (gMode == MODE_MENU) {
    // 主界面：暂停键不再"进入第一款游戏"（用暂停键进游戏语义不对）——
    // 进入游戏请点卡片（触摸）。
    return true;  // 菜单里把按键吞掉，避免落到系统默认行为
  }

  if (logical == PHYS_KEY_B) {
    exitGameToMenu();
    return true;
  }
  if (!gGame) return true;

  // 工具类应用：A = 开始/暂停（或退格），C = 重置/清空，语义由工具自己解释
  if (gMode == MODE_TOOL) {
    gGame->onKey(logical == PHYS_KEY_A ? PG_KEY_A : PG_KEY_C);
    resetToolUiCache();  // 强制下一帧把变化写回控件
    syncToolUi();
    return true;
  }

  if (logical == PHYS_KEY_C) {
    if (gGame->state() == GSTATE_PAUSED && mWinPausePtr) mWinPausePtr->hideWnd();
    gPauseWinShowing = false;
    gGame->onKey(PG_KEY_C);
    gGame->setState(GSTATE_RUNNING);
    gHud.reset();
    syncHud(true);
    if (gHost) gHost->playSfx(SFX_CLICK);
    return true;
  }

  /* ★★ 2026-09-18：**挂绳倒挂页**（电子宠物/机器人）—— 短按暂停键改判为"整屏翻转 180°"
   *   （用户需求：「因为我的屏幕挂绳在底部，屏保和机器人界面按下 C 按键切换成倒 180 度显示」。
   *     生效范围是用户当面选定的"只这两页"：本页 + 屏保，见 platform/PgFlip.h。）
   *   · 敢拿走"按键摸头"的理由：本页的按键动作在**触摸上全都有**
   *     （点屏幕=摸头、左右滑=换表情、长按屏幕=表情面板）⇒ 功能一个没丢；
   *   · 长按仍是"返回列表"（走 longPressAction，不经过本函数）；
   *   · 翻转后画面倒过来是**整屏**效果（导航栏也跟着倒），触摸坐标由框架一起转，不用我们管。 */
  if (gGame->isFlipPage()) {
    const bool now = pg::flipToggleWanted();
    LOGD("PocketGame: 短按暂停键 -> 整屏翻转 -> %s（本页 %s 是挂绳倒挂页）", now ? "倒 180°" : "正向",
         gGame->tag());
    if (gHost) gHost->playSfx(SFX_CLICK);
    return true;
  }

  /* ★ 有些应用（电子宠物）把暂停键当"互动键"用、没有暂停语义：
   *   原样交给 onKey，并且**不动 state_** —— 否则第一下走完这条 else 分支后被置成
   *   RUNNING，第二下就落到"暂停/继续"上去了（用户会看到按一下跳出暂停面板）。
   *   默认 false ⇒ 既有游戏一字不变。
   *   ⚠️ 电子宠物现在走的是**上面那条**（isFlipPage），这条留给"以后新加的互动页"。 */
  if (gGame->pauseKeyIsAction()) {
    gGame->onKey(PG_KEY_A);
    return true;
  }

  // A：开始 / 暂停 / 继续 / 重来
  GameState st = gGame->state();
  if (st == GSTATE_RUNNING || st == GSTATE_PAUSED) {
    togglePause();
  } else {
    gGame->onKey(PG_KEY_A);
    if (gGame->state() != GSTATE_OVER) gGame->setState(GSTATE_RUNNING);
    gHud.reset();
    syncHud(true);
  }
  return true;
}

/* ==================== 给"独立 ftu"的页面复用宿主 ====================
 * 番茄钟/定时器/秒表/计算器…拆成独立 Activity 后，它们同样需要一个 Host
 * （音效 + 最高分 + 系统能力）。音效与存档在语义上是**进程级单例**：
 * 各页各自 new 一个 GameHostImpl 会重复 init 音频栈、重复 setGlobalAudio。
 * 所以这里把主界面的宿主直接暴露出去（主界面 Activity 始终在返回栈里，
 * onUI_init 之后 gHost 一直有效）。
 * 声明见 src/ui/ToolPage.h（`pg::Host *pgHost();`）。
 * ⚠️ 定义必须放在**匿名命名空间之外**（下面是 `}  // namespace` 之后再定义）：
 *    写在里面会变成内部链接，ToolPage.cpp 链接时报 undefined reference（实测踩过）。 */

/* ==================== 进入 / 退出 / 暂停 ==================== */

void startGame(int slot) {
  /* ★★ 2026-09-24 修：`slot` 是**稳定存档索引**，不是表下标 —— kAppTable 删过应用
   *    之后两者已经脱钩（智能家居 slot=37，却排在第 34 行 / 下标 33）。
   *    旧代码是 `if (slot >= appCount()) return;  int index = slot;`：
   *      · 37 >= 34 ⇒ **直接 return**，点智能家居卡片毫无反应；
   *      · 就算放过去，`index = 37` 也会让后面 gameEntry(index) 取到夹紧后的错行。
   *    ⇒ 必须显式转换，且**转换失败要出声**（工程纪律：静默失败必须消灭）。 */
  const int index = appIndexForSlot(slot);
  LOGD("Pg: startGame slot=%d -> index=%d appCount=%d", slot, index, appCount());
  if (index < 0) {
    LOGW("PocketGame: startGame slot=%d 在 kAppTable 里找不到（slot 写重/漏了？）", slot);
    return;
  }
  // 独立 ftu 的功能页：直接打开对应活动，不走画布。
  // 注意 kAppTable 里这些行必须保留（slot = 稳定存档索引），只是不进画布。
  //   wifi   -> wifi.ftu   / wifiActivity   （WiFi 主页 + 信号探针）
  //   remote -> remote.ftu / remoteActivity （蓝牙遥控 + 学习页）
  /* 独立 ftu 的页面：id -> Activity 名。**加页只在这里加一行**
   * （kAppTable 的行必须留着：slot 是存档/最高分的索引，不能因为拆页而重排）。
   * 拆分口径见 docs/page-split-plan.md：游戏不动，其余功能界面全部独立。 */
  static const struct {
    const char *id;
    const char *activity;
  } kPageApps[] = {
      {"wifi", "wifiActivity"},
      {"remote", "remoteActivity"},
      {"stopwatch", "stopwatchActivity"},   // 秒表   （2026-09-14 拆出）
      {"pomodoro", "pomodoroActivity"},     // 番茄钟
      {"timer", "timerActivity"},           // 定时器
      {"calc", "calcActivity"},             // 计算器
      {"clocksuite", "clocksuiteActivity"}, // 时钟套件（主页 + 世界钟；响铃页留在主界面）
      {"react", "reactActivity"},           // 反应计时
      {"iptv", "iptvActivity"},             // 网络电视（选台页 + 播放页）
      {"probe", "probeActivity"},           // 信号探针（WiFi 探测 + 蓝牙探测 + 热点猎手）
      {"radio", "radioActivity"},           // 网络收音机（2026-09-16 独立）
      {"camera", "cameraActivity"},         // 局域网摄像头查看（2026-09-16 独立）
      {"settings", "settingsActivity"},     // 系统设置（2026-09-16：音效/WiFi/音量/背光）
      /* ★ 2026-09-23：fairy（飞天仙女）/ kitten（可爱小猫）两个应用**已下线**
       *   （用户要求去掉这 4 个 APP 省空间）⇒ kPageApps 里也删掉。
       *   同时记得：navibar.cc 的 `videoPageActive()` 名单里那两行也删了 —— 那两页是
       *   视频播放页，靠那个白名单保持"导航栏隐藏"；既然页没了，名单也别留着误导人。 */
      /* 2026-09-23：智能家居（Home Assistant 遥控器）。
       * ⚠️ 与 fairy/kitten 不同：本页**不占视频层**，所以**不需要**加进
       *    navibar.cc 的 `videoPageActive()` 白名单（那个名单是"播放页要保持导航栏隐藏"用的）。
       *    ⇒ 本页 navibar 正常常显，页面内容从 y>=52 起排（ui/ha.html 已按此排版）。 */
      {"ha", "haActivity"},                 // 智能家居（HA 遥控器 · slot 37）
  };
  for (unsigned pi = 0; pi < sizeof(kPageApps) / sizeof(kPageApps[0]); ++pi) {
    if (strcmp(gameEntry(index).id, kPageApps[pi].id) == 0) {
      LOGD("PocketGame: 独立页面 %s -> %s", kPageApps[pi].id, kPageApps[pi].activity);
      EASYUICONTEXT->openActivity(kPageApps[pi].activity);
      return;
    }
  }
  // 让列表停在这个应用所属的分类（返回时能看到原位置）
  // ★ 用 index（表下标）查表：slot 不是下标，见 startGame 开头的说明。
  if (appEntry(index).category != gCategory) {
    gCategory = appEntry(index).category;
    syncTabs();
  }
  exitGameToMenu();  // 先清理上一局

  if (!gDisplay) gDisplay = new DeviceDisplay();

  gIndex = index;
  gGame = gameEntry(index).create();
  if (!gGame) {
    gIndex = -1;
    return;
  }
  gGame->bind(gHost, index);
  gGame->setViewport(CANVAS_W, CANVAS_H);
  gGame->reset();
  gGame->setState(GSTATE_READY);

  /*
   * 工具类应用（nativeUi）= 不碰画布：显示原生窗口，文本与按钮由 syncToolUi 每帧同步。
   * 与画布类应用的差别只有"谁来画"，生命周期（update / 按键 / 返回）完全一致。
   */
  if (gGame->nativeUi()) {
    gMode = MODE_TOOL;
    gTouchInsideCanvas = false;
    int page = gGame->nativePage();
    if (mWinGamePtr) mWinGamePtr->hideWnd();
    if (mWinPausePtr) mWinPausePtr->hideWnd();
    gPauseWinShowing = false;
    if (mWinClockPtr) {
      if (page == 0) mWinClockPtr->showWnd(); else mWinClockPtr->hideWnd();
    }
    if (mWinCalcPtr) {
      if (page == 1) mWinCalcPtr->showWnd(); else mWinCalcPtr->hideWnd();
    }
    if (mWinReactPtr) {
      if (page == 2) mWinReactPtr->showWnd(); else mWinReactPtr->hideWnd();
    }
    /* IPTV（原 page 4）已于 2026-09-14 拆成独立 ftu —— 那两个窗口不在 main.ftu 里了。 */
    /* 提醒页总是从关闭态进入（到点事件才弹）。套件主页/世界钟已搬到 clocksuite.ftu。 */
    if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
    // 进入时清缓存 → 第一帧全量写一遍控件
    resetToolUiCache();
    syncToolUi();
    if (gHost) gHost->playSfx(SFX_CLICK);
    LOGD("PocketGame: enter tool[%d] %s (native page %d)", index, gGame->title(),
         page);
    return;
  }

  if (!gDisplay->attach(mGameCanvasPtr, CANVAS_W, CANVAS_H)) {
    LOGW("PocketGame: canvas attach failed");
  }

  gHud.reset();
  gMode = MODE_GAME;
  /* 游戏内把主循环提到 60fps（见 LOOP_MS_GAME 的说明）；
   * 退出时在 exitGameToMenu 里切回 33ms。 */
  if (mActivityPtr) mActivityPtr->resetUserTimer(TIMER_LOOP, LOOP_MS_GAME);
  gTouchInsideCanvas = false;
  if (mWinPausePtr) mWinPausePtr->hideWnd();
  gPauseWinShowing = false;
  if (mWinGamePtr) mWinGamePtr->showWnd();

  syncHud(true);
  // 先画一帧，避免 showWnd 之后短暂空白
  if (gDisplay->attached()) {
    gGame->render(gDisplay->canvas());
    drawSoftButtons(gDisplay->canvas());
    gDisplay->present();
  }
  if (gHost) gHost->playSfx(SFX_CLICK);
  LOGD("PocketGame: enter game[%d] %s", index, gGame->title());
}

/*
 * 退出游戏回到列表。步骤顺序有讲究（都是踩坑换来的）：
 *   1) 先把模式切回菜单 —— 主循环立刻停止 render/present，不再往画布写
 *   2) 再隐藏整屏窗口 —— 渲染层不再画这块画布
 *   3) 最后才解绑画布 —— 避开「渲染层正在读 bmp->data」的时间窗
 *   4) 画布缓冲本身永不释放（见 platform/PgDisplay.h），否则就是 use-after-free
 */
void exitGameToMenu() {
  if (gMode == MODE_MENU && !gGame) return;  // 已经在列表，无事可做
  if (gGame) LOGD("PocketGame: leave game[%d]", gIndex);

  gMode = MODE_MENU;
  // 回列表：主循环切回 30fps（省电，见 LOOP_MS_GAME 的说明）
  if (mActivityPtr) mActivityPtr->resetUserTimer(TIMER_LOOP, LOOP_MS);
  gTouchInsideCanvas = false;
  if (mWinPausePtr) mWinPausePtr->hideWnd();
  gPauseWinShowing = false;
  if (mWinGamePtr) mWinGamePtr->hideWnd();
  if (mWinClockPtr) mWinClockPtr->hideWnd();
  if (mWinCalcPtr) mWinCalcPtr->hideWnd();
  if (mWinReactPtr) mWinReactPtr->hideWnd();
  if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
  /* IPTV 的停流已归 iptvLogic.cc 自己管（onUI_quit / 长按返回时停）。
   * 这里只管主界面自己的窗口。 */
  if (gDisplay) gDisplay->detach();
  /* 贴图缓存一并还回去：素材是**运行时解码到堆**的（一张 480x540 的底图就是 1MB，
   * 本板只有 56MB 内存），退出游戏必须释放 —— 否则换几个游戏就把内存吃光，
   * 表现是视频/大图起播时进程静默退出（见 docs/online-media.md 的 OOM 记录）。 */
  if (pg::sprites().count()) {
    LOGD("PocketGame: 释放贴图缓存 %d 张 / %dKB", pg::sprites().count(),
         pg::sprites().bytes() / 1024);
    pg::sprites().clear();
  }
  if (gGame) {
    delete gGame;
    gGame = 0;
  }
  gIndex = -1;
  refreshList();
  LOGD("PocketGame: back to menu");
}

void setPaused(bool paused) {
  if (!gGame) return;
  LOGD("PocketGame: setPaused(%d) from state=%d", (int)paused,
       (int)gGame->state());
  if (paused) {
    gGame->setState(GSTATE_PAUSED);
    if (mTextPauseTitlePtr) mTextPauseTitlePtr->setText("已暂停");
    if (mTextPauseInfoPtr) {
      char buf[64];
      snprintf(buf, sizeof(buf), "得分 %d", gGame->score());
      mTextPauseInfoPtr->setText(buf);
    }
    if (mWinPausePtr) mWinPausePtr->showWnd();
    gPauseWinShowing = true;
  } else {
    gGame->setState(GSTATE_RUNNING);
    if (mWinPausePtr) mWinPausePtr->hideWnd();
    gPauseWinShowing = false;
  }
  gHud.reset();
  syncHud(true);
}

void togglePause() {
  if (!gGame) return;
  if (gGame->state() == GSTATE_PAUSED) {
    setPaused(false);
  } else if (gGame->state() == GSTATE_RUNNING) {
    setPaused(true);
  }
  if (gHost) gHost->playSfx(SFX_CLICK);
}

void onGameOver() {
  if (!gGame) return;
  // 最高分由游戏内部写入（saveBestIfNeeded），这里只做 UI 收尾
  if (mTextPauseTitlePtr) mTextPauseTitlePtr->setText("游戏结束");
  if (mTextPauseInfoPtr) {
    char buf[64];
    snprintf(buf, sizeof(buf), "得分 %d   最高 %d", gGame->score(),
             gHost ? gHost->highScore(gIndex) : 0);
    mTextPauseInfoPtr->setText(buf);
  }
  gHud.reset();
  syncHud(true);
}

/* ==================== HUD 同步 ==================== */

void syncHud(bool force) {
  if (!gGame) return;
  char buf[64];

  // 工具类应用不需要"得分/最高分"，把它们整块隐藏（画布自己画主要信息）
  int showNums = gGame->showBest() ? 1 : 0;
  if (force || showNums != gHud.nums) {
    gHud.nums = showNums;
    if (mTextScoreLabelPtr) mTextScoreLabelPtr->setVisible(showNums != 0);
    if (mTextScoreValuePtr) mTextScoreValuePtr->setVisible(showNums != 0);
    if (mTextBestLabelPtr) mTextBestLabelPtr->setVisible(showNums != 0);
    if (mTextBestValuePtr) mTextBestValuePtr->setVisible(showNums != 0);
  }

  int score = gGame->score();
  int best = gHost ? gHost->highScore(gIndex) : 0;
  if (score > best) best = score;

  if (showNums) {
    if (force || score != gHud.score) {
      gHud.score = score;
      if (mTextScoreValuePtr) mTextScoreValuePtr->setText(score);
    }
    if (force || best != gHud.best) {
      gHud.best = best;
      if (mTextBestValuePtr) mTextBestValuePtr->setText(best);
    }
  }

  const char *s = gGame->title();
  if (s && (force || strcmp(s, gHud.title) != 0)) {
    snprintf(gHud.title, sizeof(gHud.title), "%s", s);
    if (mTextGameTitlePtr) mTextGameTitlePtr->setText(gHud.title);
  }

  const char *l1 = gGame->info1Label();
  if (l1 && (force || strcmp(l1, gHud.i1l) != 0)) {
    snprintf(gHud.i1l, sizeof(gHud.i1l), "%s", l1);
    if (mTextInfo1LabelPtr) mTextInfo1LabelPtr->setText(gHud.i1l);
  }
  const char *l2 = gGame->info2Label();
  if (l2 && (force || strcmp(l2, gHud.i2l) != 0)) {
    snprintf(gHud.i2l, sizeof(gHud.i2l), "%s", l2);
    if (mTextInfo2LabelPtr) mTextInfo2LabelPtr->setText(gHud.i2l);
  }

  if (l1 && l1[0]) {
    const char *pv = gGame->info1Value(buf, sizeof(buf));
    if (!pv) pv = "";
    if (force || strcmp(pv, gHud.info1) != 0) {
      snprintf(gHud.info1, sizeof(gHud.info1), "%s", pv);
      if (mTextInfo1ValuePtr) mTextInfo1ValuePtr->setText(gHud.info1);
    }
  }
  if (l2 && l2[0]) {
    const char *pv = gGame->info2Value(buf, sizeof(buf));
    if (!pv) pv = "";
    if (force || strcmp(pv, gHud.info2) != 0) {
      snprintf(gHud.info2, sizeof(gHud.info2), "%s", pv);
      if (mTextInfo2ValuePtr) mTextInfo2ValuePtr->setText(gHud.info2);
    }
  }

  const char *hint = gGame->hint();
  if (hint && (force || strcmp(hint, gHud.hint) != 0)) {
    snprintf(gHud.hint, sizeof(gHud.hint), "%s", hint);
    if (mTextGameHintPtr) mTextGameHintPtr->setText(gHud.hint);
  }
  const char *kb = gGame->keyBar();
  if (kb && (force || strcmp(kb, gHud.keybar) != 0)) {
    snprintf(gHud.keybar, sizeof(gHud.keybar), "%s", kb);
    if (mTextGameKeyBarPtr) mTextGameKeyBarPtr->setText(gHud.keybar);
  }
}

/* ==================== 列表辅助 ==================== */

/* ==================================================================
 *              工具类应用：原生控件同步（MODE_TOOL）
 *
 * 工具不画布，而是把状态以"文本槽 + 按钮标签"的形式暴露出来（见 PgGame.h），
 * 这里负责写进 WinClock / WinCalc 的控件。
 * 关键点：**只在内容变化时才 setText / setBackgroundColor** —— 每帧无条件 setText
 * 会让控件不停重绘（30fps 下肉眼可见闪烁，还白耗 CPU）。
 * ================================================================== */

struct ToolUiCache {
  char title[64];
  char main_[32];
  char phase_[64];
  char sub_[96];
  char hint_[128];
  char keybar[128];
  char btn[Game::UI_BTN_MAX][24];
  int btnStyle[Game::UI_BTN_MAX];
  int btnShown[Game::UI_BTN_MAX];  // 0 隐藏 / 1 显示 / -1 未知
  uint32_t accent;                 // 顶部色条/状态行当前用的强调色

  void resetStrings() {
    const char *kSentinel = "\x01";  // 与任何真实文本都不同 → 第一帧必定写一次
    snprintf(title, sizeof(title), "%s", kSentinel);
    snprintf(main_, sizeof(main_), "%s", kSentinel);
    snprintf(phase_, sizeof(phase_), "%s", kSentinel);
    snprintf(sub_, sizeof(sub_), "%s", kSentinel);
    snprintf(hint_, sizeof(hint_), "%s", kSentinel);
    snprintf(keybar, sizeof(keybar), "%s", kSentinel);
    accent = 0;  // 哨兵：真实强调色都不会是 0
    for (int i = 0; i < Game::UI_BTN_MAX; ++i) {
      snprintf(btn[i], sizeof(btn[i]), "%s", kSentinel);
      btnStyle[i] = -99;
      btnShown[i] = -1;
    }
  }

  void reset() { resetStrings(); }
};
ToolUiCache gTool;

// 原生按钮指针表（page 0 = 时钟类 → BtnU0..U7；page 1 = 计算器 → BtnK0..K19）
ZKButton *toolButton(int page, int i) {
  // page 2 = 反应计时：整个下半屏只有一个"反应区"大按钮
  if (page == 2) return (i == 0) ? mBtnReactPtr : 0;
  if (page == 1) {
    switch (i) {
      case 0: return mBtnK0Ptr;
      case 1: return mBtnK1Ptr;
      case 2: return mBtnK2Ptr;
      case 3: return mBtnK3Ptr;
      case 4: return mBtnK4Ptr;
      case 5: return mBtnK5Ptr;
      case 6: return mBtnK6Ptr;
      case 7: return mBtnK7Ptr;
      case 8: return mBtnK8Ptr;
      case 9: return mBtnK9Ptr;
      case 10: return mBtnK10Ptr;
      case 11: return mBtnK11Ptr;
      case 12: return mBtnK12Ptr;
      case 13: return mBtnK13Ptr;
      case 14: return mBtnK14Ptr;
      case 15: return mBtnK15Ptr;
      case 16: return mBtnK16Ptr;
      case 17: return mBtnK17Ptr;
      case 18: return mBtnK18Ptr;
      case 19: return mBtnK19Ptr;
      default: return 0;
    }
  }
  switch (i) {
    case 0: return mBtnU0Ptr;
    case 1: return mBtnU1Ptr;
    case 2: return mBtnU2Ptr;
    case 3: return mBtnU3Ptr;
    case 4: return mBtnU4Ptr;
    case 5: return mBtnU5Ptr;
    case 6: return mBtnU6Ptr;
    case 7: return mBtnU7Ptr;
    default: return 0;
  }
}

/*
 * 改控件底色。踩坑：只调 setBackgroundColor 是**看不到变化**的 ——
 * 框架绘制时取的是「该状态下的背景色」，常态必须用
 * setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, c) 一起设。
 *
 * ★ 2026-09-14 UI 改版：这里同时负责"形状"。见 platform/PgSkin.h ——
 *   静态按钮的圆角由 gen_ui.py 挂 JSON 图，**动态改底色的按钮（tab / 工具页按键 /
 *   开关 / 电池）就得在这里顺手把对应的圆角九宫格挂上**，否则它们会是全工程仅剩的直角块。
 */
void setCtrlBg(ZKBase *v, uint32_t color) {
  if (!v) return;
  v->setBackgroundColor(color);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
  pg::applyRoundedBg(v, color);   // 颜色 → 圆角图（内部会清底色，顺序在最后）
}

// 把颜色提亮（用于深色底上的文字：直接用强调色当文字色会太暗、看不清）
uint32_t brighten(uint32_t c, int percent) {
  int r = (int)((c >> 16) & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)(c & 0xFF);
  r += (255 - r) * percent / 100;
  g += (255 - g) * percent / 100;
  b += (255 - b) * percent / 100;
  return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// 只在文本变化时写控件（cache = 上一次写进去的内容）
/* ★ 视频区"缓冲底图"显隐（ImgIptvCover / ImgCastCover）。
 *
 * 为什么需要它（2026-09-14 用户报）：videoview 是 **UI 层给下层 disp 视频层开的透明窗口**
 * （见 MCP `v85x/videoview-transparent-window.md`）—— 视频层还没数据（拉清单/等首帧）时
 * 那块区域是"透出最底层"，屏幕上就是**纯黑 + 一层发虚的文字**。铺一张不透明底图即可。
 *
 * ⚠️ **必须及时收起**：UI 层在视频层**之上**，底图留着不撤就把视频挡死了。
 *    所以判据统一是"还没出画面"（见 tickIptv / tickVideoCover 里的收敛）。
 * ⚠️ 幂等（内部比 isVisible），可以在每拍的主循环里调用。 */
void setVideoCover(ZKTextView *tv, bool on) {
  if (!tv) return;
  if (tv->isVisible() == on) return;
  tv->setVisible(on);
}

void setToolText(ZKTextView *tv, char *cache, int n, const char *val) {
  if (!tv) return;
  if (!val) val = "";
  if (strcmp(cache, val) == 0) return;
  snprintf(cache, n, "%s", val);
  tv->setText(cache);
}

/* 工具页按键底色（按语义档位）。
 * ★ 2026-09-14 UI 改版：色值全部换成 iOS 深色令牌。
 *   ⚠️ 这几个色值**必须同时存在于 tools/ios_theme.py 的 BTN_STYLES 里**，
 *      否则 pg::applyRoundedBg() 找不到对应九宫格 → 该按钮退回直角（不会报错，只会"看着不对"）。
 *      用到的档位：SUCCESS(主) / T1(次，近白) / ACCENT(强调) / SURFACE2(默认)。
 *   ⚠⚠️ 但"次"这一档不能用近白 #F2F2F7 —— 近白按钮上的文字是浅色，会看不见。
 *      iOS 的做法是"次级按钮 = 深灰底 + 主文字色"，所以这里用 SURFACE2。 */
/* 工具页按钮配色：**转发到唯一真值** pg::toolBtnColor（platform/PgSkin.h）。
 * 这里曾经是本工程第二份色表 —— UI 改版时只改了这份（或只改了 ToolPage 那份），
 * 两边就漂了。现在只有一份，本函数只是保留调用点、让 diff 最小。 */
uint32_t toolBtnBg(int page, int style) {
  (void)page;   // 页面不再影响配色（改版前 page==1 时底色不同，纯属历史包袱）
  return pg::toolBtnColor(style);
}

void syncToolButtons(int page) {
  int n = (page == 2) ? 1 : ((page == 1) ? 20 : 8);
  for (int i = 0; i < n; ++i) {
    ZKButton *b = toolButton(page, i);
    if (!b) continue;
    const char *label = gGame ? gGame->uiButton(i) : "";
    if (!label) label = "";
    int show = (label[0] != 0) ? 1 : 0;
    if (gTool.btnShown[i] != show) {
      gTool.btnShown[i] = show;
      b->setVisible(show != 0);
    }
    if (!show) continue;
    if (strcmp(gTool.btn[i], label) != 0) {
      snprintf(gTool.btn[i], sizeof(gTool.btn[i]), "%s", label);
      b->setText(gTool.btn[i]);
    }
    int style = gGame ? gGame->uiButtonStyle(i) : 0;
    if (gTool.btnStyle[i] != style) {
      gTool.btnStyle[i] = style;
      setCtrlBg(b, toolBtnBg(page, style));
      // 文字色同样走唯一真值（亮底黑字 / 深底白字），见 PgSkin.toolBtnFg
      b->setTextColor(pg::toolBtnFg(style));
    }
  }
}

/* ==================== 时钟套件（WinClockSuite / WinWorld / WinAlarmRing） ====================
 * 为什么单独一套（不复用 syncToolUi 的"8 键/20 键"模板）：控件多且结构特殊 ——
 *   5 行闹钟 × 4 个控件 + 一整块编辑区（时/分 ± 、重复、保存/取消）+ 世界时钟页 + 响铃提醒页。
 *
 * 闹钟的数据、到点检查、响铃都在 platform/PgAlarm.*（**独立守护线程**，与页面无关，
 * 所以用户在 wifi / 蓝牙遥控这类独立 ftu 里也会响）。
 * ⚠️ 到点回调来自守护线程 ⇒ 那里**只置标志**（gRingPending），
 *    所有控件操作都回到主循环 tickAlarmRing() 里做（跨线程碰控件不安全）。
 */
/* 星期文案（周日 .. 周六）。与 screensaver.cc 里那份各自 static —— 两处用法相同但不跨文件依赖。 */
/* 守护线程 → 主循环 的交接（只有标志，没有控件指针） */
volatile bool gRingPending = false;
volatile int gRingIdx = -1, gRingHh = -1, gRingMm = -1;

/* 世界时钟：**固定偏移**时区表（本板无 tzdata，见 docs/clock-suite.md）——不做夏令时。
 * 用 gmtime(utc + off*3600) 算，避免依赖 TZ 环境变量。 */
/* 守护线程回调：**只置标志**（绝对不能在这里碰控件） */
void onAlarmFireFromGuard(int index, int hh, int mm) {
  gRingIdx = index;
  gRingHh = hh;
  gRingMm = mm;
  gRingPending = true;
}

/* 星期文案（周日 .. 周六）。原来是和时钟套件共用一份，套件搬到 clocksuiteLogic.cc 后
 * 这里自带一份 —— 响铃提醒页仍留在主界面，要用它写"yyyy-mm-dd 星期X"。 */
const char *const WEEK_CN[7] = {"日", "一", "二", "三", "四", "五", "六"};

/* 到点后由主循环调用：弹提醒页（控件操作必须在主线程） */
void tickAlarmRing() {
  if (!gRingPending) return;
  gRingPending = false;
  if (!mWinAlarmRingPtr) return;

  char b[48];
  time_t now = time(0);
  struct tm lt;
  localtime_r(&now, &lt);
  snprintf(b, sizeof(b), "%02d:%02d", lt.tm_hour, lt.tm_min);
  if (mTextRingTimePtr) mTextRingTimePtr->setText(b);
  snprintf(b, sizeof(b), "%04d-%02d-%02d 星期%s", lt.tm_year + 1900, lt.tm_mon + 1,
           lt.tm_mday, WEEK_CN[(lt.tm_wday % 7 + 7) % 7]);
  if (mTextRingDatePtr) mTextRingDatePtr->setText(b);
  if (gRingHh >= 0)
    snprintf(b, sizeof(b), "闹钟 %02d:%02d", gRingHh, gRingMm);
  else
    snprintf(b, sizeof(b), "闹钟（手动测试）");
  if (mTextRingLabelPtr) mTextRingLabelPtr->setText(b);

  mWinAlarmRingPtr->showWnd();
  LOGD("PocketGame: 闹钟提醒页已弹出（%s）", b);
}

void syncToolUi() {
  if (!gGame) return;
  int page = gGame->nativePage();

  /* ⚠️ 时钟套件（原 nativePage 3）已于 2026-09-14 搬到 clocksuite.ftu，
   * 它的同步在 src/logic/clocksuiteLogic.cc 里，不再经过这里。 */

  uint32_t accent = gGame->uiAccent();
  char buf[128];

  // 强调色（色条 + 时钟页状态行文字）：必须变化检测！每帧 setBackgroundColor
  // 会让渲染线程一直重绘，实测 CPU 打到 127%、界面看起来像卡死。
  if (accent && gTool.accent != accent) {
    gTool.accent = accent;
    if (page == 1) {
      setCtrlBg(mBarCalcPtr, accent);
    } else if (page == 2) {
      // 反应计时：色条 + 状态行随阶段变色（等待蓝 → 就是现在绿 → 出成绩琥珀 → 抢跑红）
      setCtrlBg(mBarReactPtr, accent);
      if (mTextReactPhasePtr)
        mTextReactPhasePtr->setTextColor(brighten(accent, 55));
    } else {
      setCtrlBg(mBarClockPtr, accent);
      // 状态行直接用强调色太暗（实测暗蓝文字在深底上几乎看不见）→ 提亮再用
      if (mTextClockPhasePtr)
        mTextClockPhasePtr->setTextColor(brighten(accent, 55));
    }
  }

  if (page == 1) {
    /* ---------------- 计算器 ---------------- */
    setToolText(mTextCalcTitlePtr, gTool.title, sizeof(gTool.title), gGame->title());
    setToolText(mTextCalcMainPtr, gTool.main_, sizeof(gTool.main_),
                gGame->uiText(0, buf, sizeof(buf)));
    setToolText(mTextCalcStatusPtr, gTool.phase_, sizeof(gTool.phase_),
                gGame->uiText(1, buf, sizeof(buf)));
    setToolText(mTextCalcHintPtr, gTool.hint_, sizeof(gTool.hint_), gGame->hint());
    setToolText(mTextCalcKeyBarPtr, gTool.keybar, sizeof(gTool.keybar),
                gGame->keyBar());
    syncToolButtons(1);
    return;
  }

  if (page == 2) {
    /* ---------------- 反应计时 ----------------
     * 全部文字走原生控件（矢量字库）：这个游戏的核心就是那个 96px 的毫秒数，
     * 画布点阵放到这个字号会糊（见 docs/ui-design-baseline.md §8）。 */
    setToolText(mTextReactTitlePtr, gTool.title, sizeof(gTool.title),
                gGame->title());
    setToolText(mTextReactMainPtr, gTool.main_, sizeof(gTool.main_),
                gGame->uiText(0, buf, sizeof(buf)));
    setToolText(mTextReactPhasePtr, gTool.phase_, sizeof(gTool.phase_),
                gGame->uiText(1, buf, sizeof(buf)));
    setToolText(mTextReactSubPtr, gTool.sub_, sizeof(gTool.sub_),
                gGame->uiText(2, buf, sizeof(buf)));
    setToolText(mTextReactHintPtr, gTool.hint_, sizeof(gTool.hint_),
                gGame->hint());
    setToolText(mTextReactKeyBarPtr, gTool.keybar, sizeof(gTool.keybar),
                gGame->keyBar());
    syncToolButtons(2);
    return;
  }

  /* ---------------- 时钟类（番茄钟 / 定时器 / 秒表） ---------------- */
  setToolText(mTextClockTitlePtr, gTool.title, sizeof(gTool.title), gGame->title());
  setToolText(mTextClockMainPtr, gTool.main_, sizeof(gTool.main_),
              gGame->uiText(0, buf, sizeof(buf)));
  setToolText(mTextClockPhasePtr, gTool.phase_, sizeof(gTool.phase_),
              gGame->uiText(1, buf, sizeof(buf)));
  setToolText(mTextClockSubPtr, gTool.sub_, sizeof(gTool.sub_),
              gGame->uiText(2, buf, sizeof(buf)));
  setToolText(mTextClockHintPtr, gTool.hint_, sizeof(gTool.hint_), gGame->hint());
  setToolText(mTextClockKeyBarPtr, gTool.keybar, sizeof(gTool.keybar),
              gGame->keyBar());
  syncToolButtons(0);
}

// 清掉"上一次写进控件的文本"缓存 → 下一帧全量重写（进入应用、按键、点按钮后调用）
void resetToolUiCache() { gTool.resetStrings(); }

void refreshList() {
  if (mListGamesPtr) mListGamesPtr->refreshListView();
}

// 三个 tab 的选中态：选中的亮，其余暗
// ★ 2026-09-14 UI 改版：分段控件（iOS segmented）。选中态 = SURFACE3 圆角胶囊，
//   未选中 = **SURFACE（#1C1C1E）**——与底下的 ios_seg 容器同色，等于隐形，
//   于是形成"胶囊容器 + 内嵌选中块"的分段控件观感。
//   ⚠️ 未选中为什么不用"透明"：框架没有公开的清背景图语义（见 PgSkin.cpp），
//      "同色图"比"清空图"安全且视觉完全等价。
void syncTabs() {
  ZKButton *tabs[3] = {mBtnTab0Ptr, mBtnTab1Ptr, mBtnTab2Ptr};
  for (int i = 0; i < 3; ++i) {
    if (!tabs[i]) continue;
    bool on = (i == gCategory);
    /* ⚠️ tab 是**坐在段容器**（BarSegTab，#1C1C1E 的圆角块）上的，不是坐在页面底上。
     *    所以这里必须用 ON_CARD —— 烘底把圆角的抗锯齿像素和底色绑死了，
     *    用页面黑底那套会让胶囊四周出现一圈黑边（肉眼就是"锯齿/脏边"）。 */
    pg::applyRoundedBg(tabs[i], on ? 0xFF3A3A3C : 0xFF1C1C1E, pg::ON_CARD);   // SURFACE3 / SURFACE
    tabs[i]->setTextColor(on ? 0xFFF2F2F7 : 0xFF9A9AA0);   // T1 / T2
  }
}

void switchCategory(int cat) {
  if (cat < 0 || cat >= APP_CATEGORY_COUNT || cat == gCategory) return;
  gCategory = cat;
  syncTabs();
  if (mListGamesPtr) {
    mListGamesPtr->setSelection(-1);
    mListGamesPtr->refreshListView();
  }
  LOGD("PocketGame: category -> %s (%d 项)", categoryName(cat),
       categoryCount(cat));
}

/*
 * QA / 自动化钩子：命令文件 /tmp/pg_autostart
 *   内容 "<游戏序号> [状态]"  ，序号 -1 = 返回列表；
 *   状态 0=READY(默认) 1=RUNNING 2=PAUSED（弹暂停窗）
 *   每次内容变化就执行一次（无需重启应用），用于真机截图验收 / 产线自检。
 *   正式固件不放这个文件即完全无影响。
 */
/*
 * 每行一条命令，**整份文件内容变化时才执行**（所以每轮测试换个后缀即可重跑）：
 *   5 1          进槽位 5 的应用，状态 1=RUNNING（0=READY 2=PAUSED）
 *   -1           返回主列表
 *   cat 1        切换分类（0 游戏 / 1 工具 / 2 系统）
 *   wifi         打开框架内置 WiFi 设置界面
 *   tap X Y      在画布坐标点一下（本板注入不了触摸，用它做自动验收）
 *   down/move/up X Y   分步触摸（做滑动用）
 */
void splitUrlSec(const char *a, char *urlOut, int urlN, int *secOut, int defSec) {
  int sec = defSec;
  char tmp[768];
  snprintf(tmp, sizeof(tmp), "%s", a);
  char *sp = strrchr(tmp, ' ');
  if (sp && sp > tmp) {
    int v = atoi(sp + 1);
    if (v > 0) {
      sec = v;
      *sp = 0;
    }
  }
  snprintf(urlOut, urlN, "%s", tmp);
  *secOut = sec;
}

void runAutoCmd(char *buf) {
  /* 统一剥掉行内注释（`#` 起）。
   * 为什么必须在这里做：QA 文件习惯写 `cmd #编号`（**内容必须变，否则整份去重不执行**），
   * 而各命令的参数解析方式不同 —— `strncmp` 只看前缀的（saver off #x）本来就没影响，
   * 但 "后面必须结束"（`*arg == 0`）和 `sscanf("%d")` 那类会被 `#编号` 干扰到**静默失败**。
   * 实测踩过：`bright #p1` 收到命令却什么都不做，排查了一轮。 */
  char *hash = strchr(buf, '#');
  if (hash) *hash = 0;
  int blen = (int)strlen(buf);
  while (blen > 0 && (buf[blen - 1] == ' ' || buf[blen - 1] == '\t')) buf[--blen] = 0;
  if (buf[0] == 0) return;

  /* 诊断：把实际收到的命令原样打出来。
   * 为什么值得常驻：本板注入不了触摸，QA 是唯一验收通道；而"命令没生效"既可能是
   * 分发没匹配、也可能是写法/前缀问题 —— 有过把"命令无效"误判成"功能坏了"的前例。 */
  LOGD("PocketGame: autostart 收到 '%s'", buf);

  // 扩展命令（都是"内容变化才执行"，方便真机自动化验收）：
  //   "cat <0|1|2>"  切换主界面分类（游戏/工具/系统）
  //   "wifi"         打开框架内置 WiFi 设置界面
  if (strncmp(buf, "cat", 3) == 0) {
    int c = atoi(buf + 3);
    LOGD("PocketGame: autostart cmd '%s' -> category=%d", buf, c);
    switchCategory(c);
    return;
  }
  if (strncmp(buf, "flip", 4) == 0) {
    /* 整屏翻转（挂绳倒挂）：`flip` = 切换、`flip on` / `flip off` = 指定。
     * ★ 生效范围只有"环境页"（屏保 / 机器人）—— 所以在菜单里下发时，日志会显示
     *   "意愿已记住、屏幕实际仍 0°"，这正是要验的行为（不是没生效）。 */
    const char *a = buf + 4;
    while (*a == ' ') ++a;
    if (strncmp(a, "on", 2) == 0) pg::flipSetWanted(true);
    else if (strncmp(a, "off", 3) == 0) pg::flipSetWanted(false);
    else pg::flipToggleWanted();
    LOGD("PocketGame: autostart flip -> 意愿=%s | 屏幕实际 %d° | 环境页 saver=%d pet=%d | 抑制=%d",
         pg::flipWanted() ? "倒 180°" : "正向", pg::flipDeg(),
         pg::flipPageOn(pg::FLIP_PAGE_SAVER) ? 1 : 0,
         pg::flipPageOn(pg::FLIP_PAGE_PET) ? 1 : 0, pg::flipSuppressed() ? 1 : 0);
    return;
  }
  if (strncmp(buf, "dlnauri ", 8) == 0) {
    // 等价于控制器发 SetAVTransportURI（免网络自检）
    if (gDlna) gDlna->injectUri(buf + 8);
    else LOGD("PocketGame: dlnauri 失败：DLNA 未启动（先 `dlna on`）");
    return;
  }
  if (strncmp(buf, "dlnaplay", 8) == 0) {
    if (gDlna) gDlna->injectPlayPause(true);
    return;
  }
  if (strncmp(buf, "dlnapause", 9) == 0) {
    if (gDlna) gDlna->injectPlayPause(false);
    return;
  }
  if (strncmp(buf, "dlnastop", 8) == 0) {
    if (gDlna) gDlna->injectStop();
    return;
  }
  /* ---- 在线多媒体探测（ffmpeg）：证明"能打开在线流" ----
   *   ffprobe <url>   打开在线流并打印容器/时长/各流编码（不下载、不落盘） */
  /* streamstop —— 停掉在线流播放（投屏页由主循环收掉）。
   * 为什么需要：`streamshow` 在已有任务时会拒绝启动，先来一条它再重开最顺。 */
  /* 本地 DNS 中继（见 platform/PgDns.h）：
   *   dns                打印状态（是否绑定 / 转发统计 / 上游）
   *   dns up <ip>        换上游 DNS（默认 223.5.5.5）
   *   dns on|off         起 / 停中继
   *   dns test <域名>    用 getaddrinfo 实测解析（走 libc → 127.0.0.1:53 → 我们的中继） */
  if (strncmp(buf, "dns", 3) == 0) {
    pg::DnsRelay &dr = pg::DnsRelay::instance();
    const char *arg = buf + 3;
    while (*arg == ' ') ++arg;
    if (strncmp(arg, "up ", 3) == 0) {
      dr.setUpstream(arg + 3);
      LOGD("PocketGame: dns up -> %s", dr.upstream());
    } else if (strncmp(arg, "on", 2) == 0) {
      LOGD("PocketGame: dns on -> %d", dr.start() ? 1 : 0);
    } else if (strncmp(arg, "off", 3) == 0) {
      dr.stop();
      LOGD("PocketGame: dns off");
    } else if (strncmp(arg, "test ", 5) == 0) {
      const char *host = arg + 5;
      struct addrinfo hints, *res = 0;
      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = SOCK_STREAM;
      int r = getaddrinfo(host, 0, &hints, &res);
      if (r != 0 || !res) {
        LOGD("PocketGame: dns test %s -> 失败(%s)", host,
             r == 0 ? "无结果" : gai_strerror(r));
      } else {
        char ip[64] = {0};
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
        LOGD("PocketGame: dns test %s -> %s（转发 %d 次/成功 %d 次）", host, ip,
             dr.queries(), dr.answered());
        freeaddrinfo(res);
      }
    } else {
      LOGD("PocketGame: dns 状态 运行=%d 已绑端口=%d 转发=%d 成功=%d 上游=%s err=%s",
           dr.running() ? 1 : 0, dr.bound() ? 1 : 0, dr.queries(), dr.answered(),
           dr.upstream(), dr.lastError());
    }
    return;
  }

  if (strncmp(buf, "streamstate", 11) == 0) {
    /* 排障用：当前是否有在线流任务在跑（"已有任务在跑"到底是什么占着，一眼可见）。
     * running() 里带看门狗：收尾卡住 >8s 会报 false（详见 PgStream 的收尾注释）。 */
    LOGD("PocketGame: autostart streamstate -> running=%d",
         (int)pg::StreamPlayer::running());
    return;
  }
  if (strncmp(buf, "streammax ", 10) == 0) {
    /* 现场标定"最大可硬解像素数"：本板 56MB 内存，默认上限见 PgStream.cpp。
     * 例：`streammax 400000` / `streammax 0`（恢复默认）。 */
    long long px = atoll(buf + 10);
    pg::StreamPlayer::setMaxPixels(px);
    return;
  }
  if (strncmp(buf, "memguard ", 9) == 0) {
    /* 临时改"播放前要求的最低可用内存"（kB；0 = 恢复默认 7168）。
     * 低于该值时播放器会**先回收页缓存**再判一次，仍不足才拒绝。
     * 例：`memguard 32000` 用来验证回收逻辑；`memguard 0` 恢复。 */
    pg::StreamPlayer::setMinAvailKb(atoll(buf + 9));
    return;
  }
  if (strncmp(buf, "streamgrab", 10) == 0) {
    /* 抓一帧画面到 /tmp/pgframe.pgm（PGM=P5 灰度）。
     * ⚠️ 视频层不在 /dev/fb0（那是 UI 层），截图抓不到播出来的画面；
     *    要验收"旋转方向对不对"就用这个把帧搬出来看。
     * 用法：`streamgrab` = 下一轮播放的第 30 帧；`streamgrab 60` = 第 60 帧。 */
    pg::StreamPlayer::requestGrab(atoi(buf + 10));
    return;
  }
  if (strncmp(buf, "streamhw", 8) == 0) {
    /* ★ 视频链路切换（2026-09-14）：
     *   streamhw        → 打印当前用哪条
     *   streamhw 1      → 硬件播放器（zk_h264_player：解码+硬件旋转+缩放+裁剪，默认）
     *   streamhw 0      → MPP 老路（AW_MPI_VDEC+VO+应用层软件旋转，对照用）
     * ⚠️ 切换在**下一条流起播时**生效（正在播的那条不改）。 */
    const char *arg = buf + 8;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: 视频链路 = %s", pg::StreamPlayer::hwVideo()
                                            ? "硬件播放器 zk_h264_player（默认）"
                                            : "MPP VDEC+VO（软件旋转）");
      LOGD("PocketGame: 用法 streamhw <0|1>（下一条流起播时生效）");
    } else {
      pg::StreamPlayer::setHwVideo(atoi(arg) != 0);
    }
    return;
  }
  if (strncmp(buf, "streamrot", 9) == 0) {
    /* 画面旋转（顺时针）。竖屏设备看横屏内容、或手机竖拍视频方向不对时用。
     *   streamrot            → 打印当前角度
     *   streamrot 90         → 固定顺时针 90°（手动值优先于视频自带元数据）
     *   streamrot 0          → 固定不转
     *   streamrot auto       → 回到自动 = 跟随容器里的旋转元数据；
     *                          **源没有元数据时按竖屏默认 90°**（2026-09-14 改：
     *                          直播流一律没有元数据，原来 auto 等于"不转"，画面躺着）
     * ⚠️ 硬件播放器那条路（默认）**当场生效**：走的是 `zk_h264_player_set_rot`，
     *    画面立刻转正、显示区按新角度重算，不用重开流。MPP 老路要重开流。 */
    const char *arg = buf + 9;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: streamrot -> 生效角度 %d°（源自带元数据 %d°，-1=无；"
           "未手动指定且源无元数据时按竖屏默认 90°） 硬件旋转=%s",
           pg::StreamPlayer::rotationDeg(), pg::StreamPlayer::sourceRotationDeg(),
           pg::StreamPlayer::rotationSupported() ? "支持" : "不支持");
      LOGD("PocketGame: 用法 streamrot <0|90|180|270|auto>（auto = 跟随元数据，无元数据则 90°）");
    } else if (strncmp(arg, "auto", 4) == 0) {
      pg::StreamPlayer::setRotation(-1);
      if (gHost) gHost->store.setRotDeg(-1);   // 落盘：停止投屏会复位应用，不存就丢
    } else {
      int d = atoi(arg);
      pg::StreamPlayer::setRotation(d);
      if (gHost) gHost->store.setRotDeg(d);
    }
    return;
  }
  if (strncmp(buf, "memstate", 8) == 0) {
    /* 内存自检：被 OOM 杀过之后内存不会自己回来（需重启），这条用来快速确认 */
    LOGD("PocketGame: autostart memstate -> 可用内存 %lldkB", pg::StreamPlayer::memAvailableKb());
    return;
  }
  /* zkmedia 码流缓冲（ZKMEDIA_H264_VBVSIZE）：
   *   vbv          → 打印当前值
   *   vbv <字节>   → 改（顺便落盘）；范围 64KB~64MB
   * ⚠️ 库是在**进程里第一次起播时** dlopen 的、环境变量那时才生效 ⇒ 改完要
   *    重启应用（`stopcast`/换台会自动复位，或 QA `layerfree` 后 killall zkgui）。
   * 什么时候要动它：高码率 720p 源起播静默退出（日志停在 "[HW] 起硬件播放器"）时往上调，
   * 参考工程留了 2MB / 3MB 的档位。 */
  if (strncmp(buf, "vbv", 3) == 0) {
    const char *arg = buf + 3;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: ZKMEDIA_H264_VBVSIZE = %d 字节（进程环境：%s）", pg::H264Player::vbvBytes(),
           getenv("ZKMEDIA_H264_VBVSIZE") ? getenv("ZKMEDIA_H264_VBVSIZE")
                                          : "尚未写入（首次起播时才 setenv）");
      LOGD("PocketGame: 用法 vbv <字节数>（64KB~64MB；改完需重启应用生效）");
    } else {
      int n = atoi(arg);
      if (n < 65536 || n > (64 << 20)) {
        LOGW("PocketGame: vbv 值不合法（%d）—— 允许 65536~67108864", n);
      } else {
        pg::H264Player::setVbvBytes(n);
        if (gHost) gHost->store.setVbvBytes(n);   // 落盘：进程复位后仍生效
        LOGW("PocketGame: vbv 已设为 %d 字节（已落盘）—— ⚠️ 需重启应用生效（库在首次起播时 dlopen）",
             n);
      }
    }
    return;
  }
  if (strncmp(buf, "batt", 4) == 0) {
    /* 电池自检。`batt` = 读当前值（强制刷新一次）；`battraw` = 连续采 8 次原始 ADC
     * （现场标定通道/分压用，看抖动幅度）。板级参数见 docs/hardware-reference.md §3.1 */
    if (strncmp(buf, "battraw", 7) == 0) {
      char line[256];
      int n = snprintf(line, sizeof(line), "PocketGame: autostart battraw -> ADC:");
      for (int i = 0; i < 8; ++i) {
        n += snprintf(line + n, sizeof(line) - n, " %d", pg::Battery::readAdcOnce());
        usleep(60 * 1000);
      }
      LOGD("%s", line);
      return;
    }
    /* `battfake <电量> <状态>` / `battfake off` —— **仅自检用**：伪造读数来验收界面图标的
     * 各个状态（设备常年插着 USB，等不到"低电/未充电"）。状态：0=未充电 1=充电中 2=已充满。
     * 例：`battfake 15 0`（红条低电）、`battfake 50 0`（蓝条半格）、`battfake off`（回真值）。 */
    if (strncmp(buf, "battfake", 8) == 0) {
      const char *arg = buf + 8;
      while (*arg == ' ') ++arg;
      if (strncmp(arg, "off", 3) == 0) {
        pg::Battery::setFake(-1, -1);
        LOGD("PocketGame: autostart battfake -> 已恢复真实读数（电量 %d%% 状态 %s）",
             pg::Battery::percent(), pg::Battery::stateText());
      } else {
        int pct = atoi(arg);
        const char *sp = arg;
        while (*sp && *sp != ' ') ++sp;
        int st = atoi(sp);
        pg::Battery::setFake(pct, st);
        LOGD("PocketGame: autostart battfake -> 伪造 电量 %d%% 状态 %s（低电=%d）",
             pg::Battery::percent(), pg::Battery::stateText(), (int)pg::Battery::low());
      }
      return;
    }
    pg::Battery::refresh();
    LOGD("PocketGame: autostart batt -> 电量 %d%% 电压 %dmV ADC %d 状态 %s 低电=%d",
         pg::Battery::percent(), pg::Battery::voltageMv(), pg::Battery::rawAdc(),
         pg::Battery::stateText(), (int)pg::Battery::low());
    return;
  }
  if (strncmp(buf, "streamstop", 10) == 0) {
    pg::StreamPlayer::stop();
    LOGD("PocketGame: 已请求停止在线流播放（收尾后主循环会收掉投屏页）");
    return;
  }
  /* streamtest <url> [秒] —— 在线流 → 设备硬解（不落盘、不占内存），日志报解码帧数 */
  if (strncmp(buf, "streamtest ", 11) == 0) {
    char urlbuf[768];
    int sec = 30;
    splitUrlSec(buf + 11, urlbuf, sizeof(urlbuf), &sec, 30);
    pg::StreamPlayer::startDecodeTest(urlbuf, sec);
    return;
  }
  /* streamshow <url> [秒] —— 同 streamtest，但**带画面**：
   *   ① 先亮出投屏页（那个 480x700 的 videoview 就是 UI 层的"透明窗口"）；
   *   ② PgStream 建 disp 视频层，并把 ffmpeg 解出来的帧送上去。
   * ⚠️ 投屏页的 show/hide 交给**全局作用域的主循环**去做（gStreamWantPage）：
   *    本函数在匿名命名空间里，直接声明同名函数会造出第二个实体 → 调用报
   *    "call of overloaded ... is ambiguous"（本文件注释里记过这个坑，这次又踩了）。 */
  if (strncmp(buf, "streamshow ", 11) == 0) {
    char urlbuf[768];
    int sec = 30;
    splitUrlSec(buf + 11, urlbuf, sizeof(urlbuf), &sec, 30);
    if (pg::StreamPlayer::startWithDisplay(urlbuf, sec, 0, 0, 480, 700)) {
      gStreamWantPage = 1;   // 主循环下一拍把投屏页亮出来
    } else {
      gStreamWantPage = 2;
    }
    return;
  }

  /* ---- zk_h264_player 直解（验证 720p 缩放解码，见 platform/PgH264.h）----
   *   h264play <文件> <源宽> <源高> [缩放]
   *       缩放 0=不缩放 / 2=1/2 / 4=1/4（对应 FLAG_SCALE_DOWN_2/4）。
   *       例：h264play /tmp/test_hd.h264 1280 720 2   ← 720p 缩一半解
   *   h264stop / h264stat
   * ⚠️ 必须与 StreamPlayer 互斥（同一颗 VE、同一个 disp 视频层）—— 起之前先 streamstop。 */
  if (strncmp(buf, "h264stat", 8) == 0) {
    LOGD("PocketGame: H264 状态 —— running=%d 已喂 %d/%d 帧｜**解码出帧 %d**｜解码器缓冲 %d｜需关键帧 %d｜%s",
         (int)pg::H264Player::running(), pg::H264Player::framesFed(),
         pg::H264Player::totalUnits(), pg::H264Player::framesDecoded(),
         pg::H264Player::pictureCount(), pg::H264Player::needIframe(),
         pg::H264Player::describe());
    if (pg::H264Player::lastError()[0] != '-') LOGW("  错误: %s", pg::H264Player::lastError());
    return;
  }
  if (strncmp(buf, "h264stop", 8) == 0) {
    pg::H264Player::stop();
    LOGD("PocketGame: H264 已停止");
    return;
  }
  /* ---- disp 层清理（用户 2026-09-14 给的做法，见 platform/PgVideoLayer.h）----
   * 视频停止后如果不把 disp 层的 enable 清掉，下一轮再播会"不出画"且日志全正常。
   *   layerstat   只统计（不改）：除 UI 层外还有几个层开着
   *   layerfree   强制释放一遍（视频异常退出后手动兜底用） */
  if (strncmp(buf, "layerstat", 9) == 0) {
    int n = pg::VideoLayer::countEnabled();
    LOGD("PocketGame: disp 层自检 —— 除 UI 层外仍开着的层数 = %d（<0 表示打不开 /dev/disp）", n);
    return;
  }
  if (strncmp(buf, "layerfree", 9) == 0) {
    int n = pg::VideoLayer::release();
    LOGD("PocketGame: 已强制释放 disp 视频层 %d 个", n);
    return;
  }
  /* ---- 硬件播放器（zk_h264_player）运行中调参 ----
   *   h264rot <0|90|180|270>            → 硬件旋转（当场生效）
   *   h264fit <源宽> <源高>             → 按"等比铺满播放页"重算显示区并下发（set_pos）
   *   h264crop <x> <y> <w> <h>          → 源裁剪（旋转后坐标系；set_crop）
   *   h264crop 0 0 0 0                  → 取消裁剪（整帧）
   * 这三个就是 `zk_h264_player_set_rot/pos/crop` 的直接入口。 */
  if (strncmp(buf, "h264rot", 7) == 0) {
    int d = atoi(buf + 7);
    pg::H264Player::setRotation(d);
    LOGD("PocketGame: 硬件旋转 -> %d°", d);
    return;
  }
  if (strncmp(buf, "h264fit", 7) == 0) {
    int sw = 0, sh = 0;
    if (sscanf(buf + 7, "%d %d", &sw, &sh) < 2) {
      LOGW("PocketGame: 用法 h264fit <源宽> <源高>");
      return;
    }
    int x = 0, y = 0, w = 0, h = 0;
    int rot = pg::H264Player::rotation();
    pg::H264Player::fitRect(sw, sh, rot, 0, 0, 480, 700, &x, &y, &w, &h);
    pg::H264Player::setDispRect(x, y, w, h);
    LOGD("PocketGame: h264fit %dx%d（%d°）→ 显示区 (%d,%d %dx%d)", sw, sh, rot, x, y, w, h);
    return;
  }
  if (strncmp(buf, "h264crop", 8) == 0) {
    int x = 0, y = 0, w = 0, h = 0;
    if (sscanf(buf + 8, "%d %d %d %d", &x, &y, &w, &h) < 4) {
      LOGW("PocketGame: 用法 h264crop <x> <y> <w> <h>（0 0 0 0 = 整帧）");
      return;
    }
    if (w <= 0 || h <= 0) { pg::H264Player::clearCrop(); LOGD("PocketGame: 裁剪已取消（整帧）"); }
    else { pg::H264Player::setSourceCrop(x, y, w, h); LOGD("PocketGame: 裁剪 -> (%d,%d %dx%d)", x, y, w, h); }
    return;
  }
  if (strncmp(buf, "h264play ", 9) == 0) {
    char path[256];
    int sw = 0, sh = 0, sc = 0, rot = 0;
    if (sscanf(buf + 9, "%255s %d %d %d %d", path, &sw, &sh, &sc, &rot) < 3) {
      LOGW("PocketGame: 用法 h264play <文件> <源宽> <源高> [缩放0/2/4] [旋转0/90/180/270]");
      return;
    }
    pg::StreamPlayer::stop();   // 互斥：先确保 MPP 那条链路停干净
    /* 显示区按"等比铺满 480x700 播放页"算（与在线流那条路一致），再按 rot 下发硬件旋转 */
    int px = 0, py = 0, pw = 0, ph = 0;
    pg::H264Player::fitRect(sw, sh, rot, 0, 0, 480, 700, &px, &py, &pw, &ph);
    if (pg::H264Player::playFile(path, sw, sh, sc, px, py, pw, ph)) {
      if (rot) pg::H264Player::setRotation(rot);
      showCastPageForStream("硬件 H264 直解（zk_h264_player）");
      gStreamPageShown = 1;
      LOGD("PocketGame: h264play 已起（旋转 %d°，显示区 %d,%d %dx%d）—— %s", rot, px, py, pw, ph,
           pg::H264Player::describe());
    } else {
      LOGW("PocketGame: h264play 失败 —— %s", pg::H264Player::lastError());
    }
    return;
  }
  if (strncmp(buf, "hlsstat", 7) == 0) {
    LOGD("PocketGame: HLS 状态 —— %s｜档位 %s｜分片 %d｜字节 %d｜客户端 %d｜源 %s｜错误 %s",
         pg::Hls::status(), pg::Hls::variant(), pg::Hls::segments(), pg::Hls::bytes(),
         pg::Hls::clients(), pg::Hls::sourceUrl(), pg::Hls::lastError());
    return;
  }
  if (strncmp(buf, "hlsstop", 7) == 0) {
    pg::StreamPlayer::stop();
    pg::Hls::stop();
    LOGD("PocketGame: HLS 已停止（中继 + 播放）");
    return;
  }
  if (strncmp(buf, "hlsbw", 5) == 0) {
    const char *a = buf + 5;
    while (*a == ' ') ++a;
    pg::Hls::setMaxBandwidth(atoi(a));
    return;
  }
  if (strncmp(buf, "hlsonly ", 8) == 0 || strncmp(buf, "hls ", 4) == 0) {
    bool alsoPlay = (buf[3] == ' ');
    const char *arg = buf + (alsoPlay ? 4 : 8);
    char urlbuf[768];
    char local[128];
    int sec = 0;   // 直播没有"播完"，0 = 不限时（要限时就在命令尾写秒数）
    splitUrlSec(arg, urlbuf, sizeof(urlbuf), &sec, 0);

    if (!pg::Hls::start(urlbuf, local, sizeof(local))) {
      LOGW("PocketGame: HLS 启动失败：%s", pg::Hls::lastError());
      return;
    }
    LOGD("PocketGame: HLS 已就绪 → %s", local);

    if (alsoPlay) {
      if (pg::StreamPlayer::startWithDisplay(local, sec, 0, 0, 480, 700)) {
        gStreamWantPage = 1;
      } else {
        LOGW("PocketGame: HLS 中继起来了，但播放器启动失败");
        gStreamWantPage = 2;
        pg::Hls::stop();
      }
    }
    return;
  }
  if (strncmp(buf, "ffverify ", 9) == 0) {
    pg::Ff::setVerify(atoi(buf + 9) != 0);
    return;
  }
  if (strncmp(buf, "ffprobe ", 8) == 0) {
    char info[512];
    bool ok = pg::Ff::probe(buf + 8, info, sizeof(info));
    LOGD("PocketGame: ffprobe %s —— %s", ok ? "成功" : "失败", info);
    return;
  }
  /* ---- 网络校时（NTP）：https 证书校验的前提 ----
   *   ntp                    打印状态（状态/服务器/当前时间）
   *   ntp sync               立刻同步一次（可带 IP 列表，包**不做 DNS 只认 IP**）
   *   ntp reset              清状态，便于重复验证 */
  if (strncmp(buf, "ntp", 3) == 0) {
    const char *sub = buf + 3;
    while (*sub == ' ') sub++;
    if (strncmp(sub, "sync", 4) == 0) {
      const char *sp = sub + 4;
      while (*sp == ' ') sp++;
      bool ok = pg::TimeSync::syncNow(sp);
      LOGD("PocketGame: ntp sync %s", ok ? "成功" : "失败");
    } else if (strncmp(sub, "reset", 5) == 0) {
      pg::TimeSync::reset();
      LOGD("PocketGame: 校时状态已清零（下次 ensureTimeSync 会重新发起）");
    } else if (strncmp(sub, "setclock", 8) == 0) {
      /* ntp setclock <epoch>：把系统时钟设成指定 epoch。
       * 用途：**复现冷启动**（本板无 RTC，开机就是 1970）—— `ntp setclock 0` 退回 1970
       * 后重启进程，就能验证"自动校时"这条路径真的会自己把时间拉回来。 */
      long long e = atoll(sub + 8);
      struct timeval tv2;
      tv2.tv_sec = (time_t)e;
      tv2.tv_usec = 0;
      int r = settimeofday(&tv2, 0);
      LOGD("PocketGame: setclock %lld -> %s（现在 %lld）", e, r == 0 ? "成功" : "失败",
           pg::TimeSync::nowSec());
    } else {
      time_t t = (time_t)pg::TimeSync::nowSec();
      struct tm tmv;
      localtime_r(&t, &tmv);
      char tb[64];
      strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tmv);
      LOGD("PocketGame: 校时 状态=%s 服务器=%s 当前时间=%s（%s）", pg::TimeSync::stateText(),
           pg::TimeSync::lastServer(), tb,
           pg::TimeSync::timeValid() ? "有效" : "无效——1970 附近，https 会失败");
    }
    return;
  }
  if (strncmp(buf, "dlna ", 5) == 0) {
    // dlna on [port] / dlna off / dlna state
    const char *sub = buf + 5;
    if (strncmp(sub, "on", 2) == 0) {
      int port = 8200;
      const char *sp = strchr(sub, ' ');
      if (sp) port = atoi(sp + 1);
      gDlnaUserOff = false;  // 手动 on 取消"用户已关"状态，让自动通路恢复正常
      bool ok = dlnaStart(port);
      LOGD("PocketGame: DLNA 启动 %s (端口 %d)", ok ? "成功" : "失败", port);
    } else if (strncmp(sub, "tlscheck", 8) == 0) {
      pg::pgDlnaTlsDiag();  // https 排查第一步：libssl/libcrypto 能不能 dlopen
    } else if (strncmp(sub, "ua", 2) == 0) {
      const char *v = sub + 2;
      while (*v == ' ') v++;
      pg::pgDlnaSetHttpUa(v);
    } else if (strncmp(sub, "ref", 3) == 0) {
      const char *v = sub + 3;
      while (*v == ' ') v++;
      pg::pgDlnaSetHttpReferer(v);
    } else if (strncmp(sub, "dns", 3) == 0) {
      const char *h = sub + 3;
      while (*h == ' ') h++;
      pg::pgDlnaDnsCheck(h);
    } else if (strncmp(sub, "verify", 6) == 0) {
      // dlna verify [0|1|2]：https 证书校验策略（不带参数 = 查询当前值）
      const char *sp2 = sub + 6;
      while (*sp2 == ' ') sp2++;
      if (*sp2 >= '0' && *sp2 <= '2') pg::pgDlnaSetTlsVerifyMode(*sp2 - '0');
      else LOGD("PgDlna: 当前证书校验策略=%d", pg::pgDlnaTlsVerifyMode());
    } else if (strncmp(sub, "off", 3) == 0) {
      dlnaStop();
      gDlnaUserOff = true;  // 手动关掉后不要再被 ensureDlna() 自动拉起来
      LOGD("PocketGame: DLNA 已关闭（本次运行不再自动启动）");
    } else {
      if (gDlna) {
        LOGD("PocketGame: DLNA 运行中 port=%d ip=%s state=%s uri='%s' dl=%d%%",
             gDlna->httpPort(), gDlna->localIp(), gDlna->transportState(),
             gDlna->lastUri(), gDlna->downloadPercent());
      } else {
        LOGD("PocketGame: DLNA 未启动");
      }
    }
    return;
  }

  /* ==================== 蓝牙相关命令已搬到蓝牙遥控应用 ====================
   * 蓝牙遥控是独立 ftu（remote.ftu / remoteActivity），它的自检命令走**自己的**通道：
   *   /tmp/pg_remotecmd   —— send / page / scan / conn / disc / learn / dump / selftest / back
   * 这里只保留一个"打开蓝牙遥控应用"的入口（见上面的 remote 命令）。 */

  if (strncmp(buf, "caststop", 8) == 0) {
    LOGD("PocketGame: autostart cmd caststop");
    stopCast();
    return;
  }
  if (strncmp(buf, "cast ", 5) == 0) {
    // 投屏/视频播放实验：`cast /tmp/x.mp4` 或 `cast http://host/x.mp4`
    startCast(buf + 5);
    return;
  }

  if (strncmp(buf, "probe", 5) == 0) {
    // 信号探针（独立 ftu：probe.ftu / probeActivity，见 docs/wifi-probe-app.md）
    // 页面内的自动化操作用**它自己的**通道 /tmp/pg_probecmd（见 probeLogic.cc）。
    // ⚠️ 排在 "remote"/"wifiapp" 之外没有前缀冲突，但**不要**挪到 "wifi" 之后。
    LOGD("PocketGame: autostart cmd '%s' -> open probeActivity (信号探针)", buf);
    EASYUICONTEXT->openActivity("probeActivity");
    return;
  }
  if (strncmp(buf, "remote", 6) == 0) {
    // 蓝牙遥控（独立 ftu：remote.ftu / remoteActivity）
    // 页面内的自动化操作用**它自己的**通道 /tmp/pg_remotecmd（见 remoteLogic.cc）。
    LOGD("PocketGame: autostart cmd '%s' -> open remoteActivity", buf);
    EASYUICONTEXT->openActivity("remoteActivity");
    return;
  }
  if (strncmp(buf, "haapp", 5) == 0) {
    /* 智能家居（独立 ftu：ha.ftu / haActivity，slot 37）。
     * 页面内的自动化操作用**它自己的**通道 /tmp/pg_hacmd（见 haLogic.cc）。
     * ⚠️ 用 "haapp" 而不是 "ha"：`ha` 太短，容易和别的命令前缀撞上
     *   （本工程已经因为 "wifiapp"/"wifi" 的包含关系踩过，见上面那段注释）。 */
    LOGD("PocketGame: autostart cmd '%s' -> open haActivity (智能家居)", buf);
    EASYUICONTEXT->openActivity("haActivity");
    return;
  }
  if (strncmp(buf, "wifiapp", 7) == 0) {
    // 我们自己的 WiFi 应用（独立 ftu：wifi.ftu / wifiActivity）
    // ⚠️ 必须排在下面 "wifi" 判断之前 —— "wifiapp" 也以 "wifi" 开头。
    LOGD("PocketGame: autostart cmd '%s' -> open wifiActivity (我们的 WiFi 应用)", buf);
    EASYUICONTEXT->openActivity("wifiActivity");
    return;
  }
  if (strncmp(buf, "wifi", 4) == 0) {
    LOGD("PocketGame: autostart cmd '%s' -> open WifiSettingActivity", buf);
    if (gHost) gHost->openWifiSettings();
    return;
  }

  if (strncmp(buf, "pcmfree", 7) == 0) {
    /* 让出声卡：停音频线程 + close(hw:0,0)。现场最重要的一条 ——
     * 常开流独占 card0 时，设备上别的进程（含 /bin/tinyplay）打开声卡会被**挂住**。
     * 之后 streamshow 仍能播（自动降级为只播视频），pcmopen 可随时拿回。 */
    if (gHost) {
      bool ok = gHost->audio.releasePcm();
      LOGD("PocketGame: autostart pcmfree -> %s（声卡占用=%d）", ok ? "已释放" : "失败",
           (int)gHost->audio.pcmHeld());
    }
    return;
  }
  if (strncmp(buf, "pcmopen", 7) == 0) {
    if (gHost) {
      bool ok = gHost->audio.acquirePcm();
      LOGD("PocketGame: autostart pcmopen -> %s（声卡占用=%d）", ok ? "已拿回" : "失败",
           (int)gHost->audio.pcmHeld());
    }
    return;
  }
  if (strncmp(buf, "pcmidle ", 8) == 0) {
    /* 声卡"空闲自动让出"阈值（秒）：0 = 关（永远常开，默认）。现场按需调。 */
    int sec = atoi(buf + 8);
    if (gHost) {
      gHost->audio.setPcmIdleSec(sec);
      LOGD("PocketGame: autostart pcmidle -> %d 秒（当前空闲 %llds，占用=%d）", sec,
           gHost->audio.idleMs() / 1000, (int)gHost->audio.pcmHeld());
    }
    return;
  }
  if (strncmp(buf, "pcmstate", 8) == 0) {
    if (gHost) {
      LOGD("PocketGame: autostart pcmstate -> 声卡占用=%d backend=%s 空闲阈值=%ds 已空闲=%llds",
           (int)gHost->audio.pcmHeld(), gHost->audio.backend(),
           gHost->audio.pcmIdleSec(), gHost->audio.idleMs() / 1000);
    }
    return;
  }
  if (strncmp(buf, "audiosw", 7) == 0) {
    // 重新检查并打开 codec 输出开关（现场自检用；正常流程启动时会自动做）
    LOGD("PocketGame: autostart audiosw (重新检查/打开输出开关)");
    if (gHost) gHost->audio.reassertOutputSwitch();
    return;
  }
  if (strncmp(buf, "vol ", 4) == 0) {
    // 音量自检（免按键）：`vol 1` 升一档 / `vol -1` 降一档；日志打百分比，
    // 再用 tinymix -D 0 回读控件值即可判定真的写进去了。
    // 第二个参数 = OSD 停留毫秒（抓帧验收用，不传就是默认 1600ms）。
    int d = atoi(buf + 4);
    int hold = 0;
    const char *sp = strchr(buf + 4, ' ');
    if (sp) hold = atoi(sp + 1);
    if (gHost) {
      /* 同按音量键：走全局钩子（状态栏 OSD 由广播驱动）。
       * ⚠️ hold 参数在这里已无用（面板归状态栏管）——想抓帧用状态栏自己的通道：
       *    `echo 'vol 60 5000' > /tmp/pg_statusbarcmd`（见 statusbarLogic.cc）。 */
      int pct = pg::volumeStepGlobal(d);
      LOGD("PocketGame: autostart vol %d -> 音量 %d%% (hold=%d 已忽略，OSD 归状态栏)", d, pct, hold);
    }
    return;
  }
  if (strncmp(buf, "mute", 4) == 0) {
    /* 静音自检（免触摸）：`mute 1` 静音 / `mute 0` 取消 / `mute` 只打印当前状态。
     * 判据（真静音的硬证据，不看日志）：
     *   tinymix -D 0  →  `SPK Switch` / `LINEOUT Switch` 的 value 应为 Off/On。 */
    const char *arg = buf + 4;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: 静音状态 muted=%d（1=静音中）", pg::isMutedGlobal() ? 1 : 0);
    } else {
      const bool want = (*arg == '1');
      const bool ok = pg::setMutedGlobal(want);
      LOGD("PocketGame: autostart mute %s -> %s（回读 muted=%d）", arg, ok ? "成功" : "失败",
           pg::isMutedGlobal() ? 1 : 0);
    }
    return;
  }
  if (strncmp(buf, "sound ", 6) == 0) {
    /* 与主界面"音效 开/关"按钮同一条路径（免触摸验证开关效果）。
     * ⚠️ 参数要**同时认 on/off 与 1/0**：以前只 `atoi()`，于是 `sound on` →
     *    atoi("on") = 0 → **被当成关闭**。我 2026-09-15 排查"机器没声音"时就踩了：
     *    想打开音效，结果又关了一次（还落了盘）。凡是"会让状态反转"的命令，
     *    参数解析必须宽进、且**打印最终生效值**（下面那条 LOGD 就是判据）。 */
    const char *v = buf + 6;
    int on;
    if (strncmp(v, "on", 2) == 0 || strncmp(v, "ON", 2) == 0 ||
        strncmp(v, "1", 2) == 0 || strncmp(v, "true", 4) == 0) {
      on = 1;
    } else if (strncmp(v, "off", 3) == 0 || strncmp(v, "OFF", 3) == 0 ||
               strncmp(v, "0", 2) == 0 || strncmp(v, "false", 5) == 0) {
      on = 0;
    } else {
      on = atoi(v) != 0;
    }
    if (gHost) {
      gHost->setSoundOn(on);
      gHost->playSfx(SFX_CLICK);
    }
    LOGD("PocketGame: autostart sound -> %s（已保存）", on ? "开" : "关");
    return;
  }
  if (strncmp(buf, "sfxseq", 6) == 0) {
    LOGD("PocketGame: autostart sfxseq (逐个播放全部音效)");
    gSfxSeqIdx = 1;
    gSfxSeqWaitMs = 200;
    return;
  }
  if (strncmp(buf, "sfx ", 4) == 0) {
    int i = atoi(buf + 4);
    LOGD("PocketGame: autostart sfx %d (%s)", i,
         (i > 0 && i < SFX_COUNT) ? kSfxNames[i] : "越界");
    if (gHost && i > 0 && i < SFX_COUNT) gHost->playSfx(i);
    return;
  }
  if (strncmp(buf, "btn ", 4) == 0) {
    // 原生工具页的按钮：直接走 onUiButton（本板注入不了触摸，靠它做自动验收）
    int i = atoi(buf + 4);
    LOGD("PocketGame: autostart btn %d", i);
    if (gGame && gMode == MODE_TOOL) {
      gGame->onUiButton(i);
      resetToolUiCache();
      syncToolUi();
    } else {
      // 静默忽略过一次，结果把"命令无效"误判成"按钮回调坏了" —— 明确喊出来
      LOGW("PocketGame: autostart btn 只在原生工具页(MODE_TOOL)生效；当前 mode=%d，"
           "游戏内请用 tap/down/up/key", (int)gMode);
    }
    return;
  }
  if (strncmp(buf, "sel ", 4) == 0) {
    int n = atoi(buf + 4);
    LOGD("PocketGame: autostart sel %d", n);
    if (mListGamesPtr) mListGamesPtr->setSelection(n);
    return;
  }
  /* enter <行号> —— 等价于点第 N 行卡片（走 onListItemClick_ListGames 同一条路）。
   * 为什么需要：主界面**进游戏只认触摸**（按键被刻意吞掉，见 handleLogicalKey 里
   * "主界面：暂停键不再进入第一款游戏"），而本板触摸注入时灵时不灵 —— 没有它就没法
   * 自动验收"游戏页里不进屏保"这类只在 MODE_GAME 才成立的行为。 */
  if (strncmp(buf, "enter ", 6) == 0) {
    int idx = atoi(buf + 6);
    int slot = categorySlot(gCategory, idx);
    LOGD("PocketGame: autostart enter 第 %d 行 -> slot=%d（等价于点卡片）", idx, slot);
    if (slot >= 0) startGame(slot);
    else LOGW("PocketGame: enter %d 越界（当前分类 %d 共 %d 项）", idx, (int)gCategory,
              categoryCount(gCategory));
    return;
  }
  if (strncmp(buf, "tap ", 4) == 0) {
    int x = 0, y = 0;
    if (sscanf(buf + 4, "%d %d", &x, &y) == 2) {
      LOGD("PocketGame: autostart tap (%d,%d)", x, y);
      dispatchCanvasTouch(PG_TOUCH_DOWN, x, y);
      dispatchCanvasTouch(PG_TOUCH_UP, x, y);
    }
    return;
  }
  if (strncmp(buf, "down ", 5) == 0) {
    int x = 0, y = 0;
    if (sscanf(buf + 5, "%d %d", &x, &y) == 2)
      dispatchCanvasTouch(PG_TOUCH_DOWN, x, y);
    return;
  }
  if (strncmp(buf, "move ", 5) == 0) {
    int x = 0, y = 0;
    if (sscanf(buf + 5, "%d %d", &x, &y) == 2)
      dispatchCanvasTouch(PG_TOUCH_MOVE, x, y);
    return;
  }
  if (strncmp(buf, "up ", 3) == 0) {
    int x = 0, y = 0;
    if (sscanf(buf + 3, "%d %d", &x, &y) == 2)
      dispatchCanvasTouch(PG_TOUCH_UP, x, y);
    return;
  }
  /*
   * `key <A|B|C>` —— 注入一次逻辑按键，走 handleLogicalKey（和真实按键同一条路）。
   *
   * 为什么需要它：`<slot> 1` 会**附带**调一次 onKey(A)（见 startGame 那段），
   * 对"A 键有额外语义"的游戏就会污染测试。最典型的是扫雷 ——
   *   onKey(A) = 切换"插旗模式"，所以 `5 1` 之后 tap 只能插旗、永远挖不开格子。
   * 而物理键在 2026-09-12 被重定义为「音量-/音量+/暂停」，A/C 只剩
   * 「暂停弹窗的按钮」一条路，QA 又点不到弹窗（`btn` 只在 MODE_TOOL 生效）。
   * 于是有了这条命令 —— 也是目前唯一能测 A/C 语义的手段。
   */
  if (strncmp(buf, "key ", 4) == 0) {
    char k = 0;
    if (sscanf(buf + 4, " %c", &k) != 1) return;
    int logical = -1;
    if (k == 'A' || k == 'a') logical = PHYS_KEY_A;
    else if (k == 'B' || k == 'b') logical = PHYS_KEY_B;
    else if (k == 'C' || k == 'c') logical = PHYS_KEY_C;
    LOGD("PocketGame: autostart key '%c' -> logical %d", k, logical);
    if (logical >= 0) handleLogicalKey(logical);
    return;
  }

  /* 屏保（APP_TYPE_SYS_SCREENSAVER 的 SysApp）进出控制。
   * 真实路径是「EasyUI.cfg 的 screensaverTimeout=30 秒无操作，框架自动进入」，
   * 但本板注入不了触摸（见 docs/MCP-待改清单.md G2），所以要能用脚本驱动 ——
   * 这条命令免等 30 秒、也免触摸。屏保自己的命令在 /tmp/pg_savercmd。 */
  if (strncmp(buf, "saver", 5) == 0) {
    const char *arg = buf + 5;
    while (*arg == ' ') ++arg;
    if (strncmp(arg, "on", 2) == 0) {
      EASYUICONTEXT->screensaverOn();
      LOGD("PocketGame: autostart saver on -> 进入屏保");
    } else if (strncmp(arg, "off", 3) == 0) {
      EASYUICONTEXT->screensaverOff();
      LOGD("PocketGame: autostart saver off -> 退出屏保");
    } else if (strncmp(arg, "to ", 3) == 0) {
      /* 临时改屏保超时（秒）。自检时常要"长时间停在一个页面不让屏保插进来"——
       * 而 QA 命令属于程序化操作，框架的空闲检测**不认**它（只有真触摸/按键才算活动），
       * 所以自动化验收里 30 秒一到必然被屏保打断。用 saver to 600 悄悄延长即可。 */
      int sec = atoi(arg + 3);
      EASYUICONTEXT->setScreensaverTimeOut(sec);
      EASYUICONTEXT->setScreensaverEnable(sec > 0);
      LOGD("PocketGame: 屏保超时 -> %d 秒（enable=%d）", sec,
           EASYUICONTEXT->isScreensaverEnable() ? 1 : 0);
    } else {
      LOGD("PocketGame: autostart saver -> isOn=%d timeout=%d enable=%d",
           EASYUICONTEXT->isScreensaverOn() ? 1 : 0,
           EASYUICONTEXT->getScreensaverTimeOut(),
           EASYUICONTEXT->isScreensaverEnable() ? 1 : 0);
    }
    return;
  }

  /* 硬件能力自检（时钟套件/闹钟要用 —— MCP `get_package_api` 查到的 API，这里验硬件在不在）：
   *   beep               蜂鸣器短鸣        （HARDWAREMANAGER->beep）
   *   beepfreq <hz> <d>  自定义 PWM 鸣叫   （setBeepPWM，d = 占空比 0~255）
   *   bright             打印当前/最大亮度 + 屏是否亮
   *   bright <0-100>     设亮度（验证背光**可调**；回读校验）
   *   screen off|on      熄屏 / 亮屏（⚠️ off 之后要 on 回来） */
  if (strncmp(buf, "beep", 4) == 0) {
    if (strncmp(buf, "beepfreq ", 9) == 0) {
      unsigned f = 0, d = 0;
      if (sscanf(buf + 9, "%u %u", &f, &d) == 2) {
        HARDWAREMANAGER->setBeepPWM(f, (uint8_t)d);
        LOGD("PocketGame: beepfreq -> %u Hz，占空比 %u", f, d);
      }
    } else {
      HARDWAREMANAGER->beep();
      LOGD("PocketGame: beep() 已调用（听有没有响）");
    }
    return;
  }
  if (strncmp(buf, "bright", 6) == 0) {
    const char *arg = buf + 6;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: 亮度 cur=%d max=%d 屏亮=%d", BRIGHTNESSHELPER->getBrightness(),
           BRIGHTNESSHELPER->getMaxBrightness(), BRIGHTNESSHELPER->isScreenOn() ? 1 : 0);
    } else {
      int v = 0;
      if (sscanf(arg, "%d", &v) == 1) {
        BRIGHTNESSHELPER->setBrightness(v);
        LOGD("PocketGame: setBrightness(%d) -> 回读 %d", v, BRIGHTNESSHELPER->getBrightness());
      }
    }
    return;
  }
  if (strncmp(buf, "screen ", 7) == 0) {
    if (strncmp(buf + 7, "off", 3) == 0) {
      BRIGHTNESSHELPER->screenOff();
      LOGD("PocketGame: screenOff()");
    } else {
      BRIGHTNESSHELPER->screenOn();
      LOGD("PocketGame: screenOn()");
    }
    return;
  }
  /* 框架音频播放器探针（选型实验，见顶部 ZkProbeListener 注释）。
   * ⚠️ 测之前先 `pcmfree`，否则它会去打开被我们独占的 hw:0,0 而挂住。 */
  if (strncmp(buf, "zkplay", 6) == 0) {
    if (!sZkPlayer) {
      sZkPlayer = new ZKMediaPlayer(ZKMediaPlayer::E_MEDIA_TYPE_AUDIO);
      sZkPlayer->setPlayerMessageListener(&sZkProbeListener);
      LOGD("PocketGame: ZKMediaPlayer 已创建（E_MEDIA_TYPE_AUDIO）");
    }
    const char *arg = buf + 6;
    while (*arg == ' ') ++arg;
    std::string rel = (*arg ? std::string(arg) : std::string("audio/over.wav"));
    std::string full = CONFIGMANAGER->getResFilePath(rel);
    sZkPlayer->setVolume(0.8f);
    LOGD("PocketGame: ZKMediaPlayer->play('%s') 解析=%s", rel.c_str(), full.c_str());
    sZkPlayer->play(full.c_str());
    return;
  }
  if (strncmp(buf, "zkstop", 6) == 0) {
    if (sZkPlayer) {
      sZkPlayer->stop();
      LOGD("PocketGame: ZKMediaPlayer->stop()");
    }
    return;
  }
  /* 官方音频播放器（参考工程 zk_wav_play）：见顶部 zkWavThread 注释 */
  if (strncmp(buf, "zkwav", 5) == 0) {
    const char *arg = buf + 5;
    while (*arg == ' ') ++arg;
    std::string rel = (*arg ? std::string(arg) : std::string("audio/over.wav"));
    std::string full = CONFIGMANAGER->getResFilePath(rel);
    char *p = strdup(full.c_str());
    pthread_t th;
    LOGD("PocketGame: zk_wav_play('%s') 后台线程启动", full.c_str());
    if (p && pthread_create(&th, 0, zkWavThread, p) == 0) {
      pthread_detach(th);
    } else {
      free(p);
      LOGW("PocketGame: 播放线程创建失败");
    }
    return;
  }
  /* 闹钟铃声（循环，走官方音频播放器；见 platform/PgRingtone.*）：
   *   ring                循环响默认铃声 audio/alarm.wav
   *   ring <相对路径>      指定铃声（如 ring audio/over.wav）
   *   ring off            停止（并把声卡还给音效系统） */
  if (strncmp(buf, "ring", 4) == 0) {
    const char *arg = buf + 4;
    while (*arg == ' ') ++arg;
    if (strncmp(arg, "off", 3) == 0) {
      Ringtone::instance().stop();
      LOGD("PocketGame: ring off -> 仍在响=%d", Ringtone::instance().playing() ? 1 : 0);
    } else if (strncmp(arg, "stop", 4) == 0) {
      /* 等价于点提醒页的「停止」按钮（停铃 + 收起提醒页） */
      pg::Alarm::instance().stopRing();
      if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
      LOGD("PocketGame: ring stop -> 停铃并收起提醒页");
    } else if (strncmp(arg, "snooze", 6) == 0) {
      /* 等价于点提醒页的「贪睡 5 分钟」 */
      pg::Alarm::instance().snooze(5);
      if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
      LOGD("PocketGame: ring snooze -> 已排定 5 分钟后再次响铃，收起提醒页");
    } else if (strncmp(arg, "state", 5) == 0) {
      LOGD("PocketGame: ring state 响铃中=%d 提醒页=%d（%s）",
           pg::Alarm::instance().ringing() ? 1 : 0,
           (mWinAlarmRingPtr && mWinAlarmRingPtr->isWndShow()) ? 1 : 0,
           pg::Alarm::instance().statusText());
    } else {
      const char *rel = (*arg ? arg : "audio/alarm.wav");
      bool ok = Ringtone::instance().start(rel);
      LOGD("PocketGame: ring start('%s') -> %s（err=%s）", rel, ok ? "OK" : "失败",
           Ringtone::instance().lastError());
    }
    return;
  }
  /* 闹钟（存储 + 到点；守护线程独立于页面，见 platform/PgAlarm.*）：
   *   alarm list                  列出（序号 / 开关 / 时间 / 重复）
   *   alarm add <hh> <mm> [mask]   新增（mask 省略=0 仅一次；127=每天）
   *   alarm on|off|del <序号>
   *   alarm clear                 清空
   *   alarm in <秒>                 N 秒后响一次（验收用，不落盘）
   *   alarm test                   立刻响一次
   *   alarm stop                   停铃（按任意键也会停） */
  if (strncmp(buf, "alarm", 5) == 0) {
    pg::Alarm &al = pg::Alarm::instance();
    const char *arg = buf + 5;
    while (*arg == ' ') ++arg;
    if (strncmp(arg, "list", 4) == 0) {
      LOGD("PocketGame: alarm list -> 共 %d 个（存储 %s）", al.count(),
           al.path()[0] ? al.path() : "(不可写)");
      for (int i = 0; i < al.count(); ++i) {
        const pg::AlarmItem *it = &al.items()[i];
        LOGD("PocketGame:   #%d %s %02d:%02d mask=%d", i, it->enabled ? "开" : "关",
             it->hour, it->minute, it->mask);
      }
    } else if (strncmp(arg, "add ", 4) == 0) {
      int hh = 0, mm = 0, mask = 0;
      int n = sscanf(arg + 4, "%d %d %d", &hh, &mm, &mask);
      if (n >= 2) {
        bool ok = al.add(hh, mm, mask);
        LOGD("PocketGame: alarm add %02d:%02d mask=%d -> %s（共 %d 个）", hh, mm, mask,
             ok ? "OK" : "失败(已满或时间非法)", al.count());
      }
    } else if (strncmp(arg, "on ", 3) == 0) {
      al.setEnabled(atoi(arg + 3), true);
      LOGD("PocketGame: alarm on %d", atoi(arg + 3));
    } else if (strncmp(arg, "off ", 4) == 0) {
      al.setEnabled(atoi(arg + 4), false);
      LOGD("PocketGame: alarm off %d", atoi(arg + 4));
    } else if (strncmp(arg, "del ", 4) == 0) {
      al.removeAt(atoi(arg + 4));
      LOGD("PocketGame: alarm del %d -> 剩 %d", atoi(arg + 4), al.count());
    } else if (strncmp(arg, "clear", 5) == 0) {
      al.clearAll();
      LOGD("PocketGame: alarm clear");
    } else if (strncmp(arg, "in ", 3) == 0) {
      int sec = atoi(arg + 3);
      al.fireIn(sec);
      LOGD("PocketGame: alarm in %d 秒", sec);
    } else if (strncmp(arg, "test", 4) == 0) {
      al.testFireNow();
      LOGD("PocketGame: alarm test -> 立刻响");
    } else if (strncmp(arg, "stop", 4) == 0) {
      al.stopRing();
      LOGD("PocketGame: alarm stop -> 仍在响=%d（%s）", al.ringing() ? 1 : 0, al.statusText());
    } else {
      LOGD("PocketGame: alarm 子命令未知：'%s'", arg);
    }
    return;
  }

  /* 画布缺字统计（2026-09-15 加）：字库是**按档位收字的子集**，没收到的字过去
   * 是"静默"的（画个方框或者干脆消失），验收时最容易漏。
   *   · miss=0 且 fallback=0 → 该画的字全在字库里（正常状态）
   *   · fallback>0 → 某档没收这个字，用了别的档（字形尺寸会不对，要补字）
   *   · miss>0   → 全档都没有（会画空心方框），必须跑 tools/genfont.py 补
   * 用法：`glyphmiss`（打印）/ `glyphmiss reset`（清零，便于"进某游戏前后对比"）。 */
  if (strncmp(buf, "glyphmiss", 9) == 0) {
    const char *arg = buf + 9;
    while (*arg == ' ') ++arg;
    if (strncmp(arg, "reset", 5) == 0) {
      pg::Canvas::resetGlyphStats();
      LOGD("PocketGame: glyphmiss reset");
      return;
    }
    int miss = pg::Canvas::missingGlyphCount();
    int fb = pg::Canvas::sizeFallbackCount();
    LOGD("PocketGame: glyphmiss miss=%d fallback=%d %s", miss, fb,
         (miss == 0 && fb == 0) ? "(字库完整)" : "(有缺字，跑 tools/genfont.py)");
    for (int i = 0; i < 16; ++i) {
      uint32_t cp = pg::Canvas::missingGlyphAt(i);
      if (!cp) break;
      char u8[8] = {0};
      if (cp < 0x80) {
        u8[0] = (char)cp;
      } else if (cp < 0x800) {
        u8[0] = (char)(0xC0 | (cp >> 6));
        u8[1] = (char)(0x80 | (cp & 0x3F));
      } else {
        u8[0] = (char)(0xE0 | (cp >> 12));
        u8[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u8[2] = (char)(0x80 | (cp & 0x3F));
      }
      LOGD("PocketGame:   #%d U+%04X '%s'", i, cp, u8);
    }
    return;
  }

  /* 游戏贴图（运行时 PNG 加载）的验收命令（2026-09-15 加）：
   *   `spr`              打印缓存统计（张数 / 占堆 KB / 失败数 / 已缓存路径）
   *   `spr <path>`       解码一张图并打印尺寸 + 前 8 个像素的 ARGB
   *                      （**字节序与是否预乘 alpha 只能实测**，肉眼看抓屏分不清）
   *   `spr clear`        清空缓存（等价于退出游戏）
   * 路径是**资源相对路径**，如 images/game/whack/mole.png。 */
  if (strncmp(buf, "spr", 3) == 0 &&
      (buf[3] == 0 || buf[3] == ' ')) {
    const char *arg = buf + 3;
    while (*arg == ' ') ++arg;
    if (*arg == 0) {
      LOGD("PocketGame: spr 缓存 %d 张 / %dKB / 失败 %d / loader=%s",
           pg::sprites().count(), pg::sprites().bytes() / 1024,
           pg::sprites().failCount(), pg::sprites().ready() ? "on" : "off");
      for (int i = 0; i < pg::sprites().count(); ++i) {
        const char *p = pg::sprites().pathAt(i);
        const pg::Canvas::Sprite *s = pg::sprites().get(p);
        LOGD("PocketGame:   [%d] %s %dx%d", i, p, s ? s->w : -1, s ? s->h : -1);
      }
      return;
    }
    if (strncmp(arg, "clear", 5) == 0) {
      pg::sprites().clear();
      LOGD("PocketGame: spr 缓存已清空");
      return;
    }
    pg::spriteProbe(arg);
    return;
  }

  /* 打印当前游戏的分数：`gscore`（**验收用**，2026-09-15 加）。
   * 为什么需要它：调俄罗斯方块"上滑直落"的阈值时，用抓帧判"有没有触发直落"会被
   *   "方块自然下落 / 消行"干扰（两者都会让画面变化）⇒ 需要一个**确定性判据**。
   *   分数只在**有效操作**时变（直落 += 落格数×2、软降 +1、消行加分），自然下落不变。
   * 用法：进游戏后 `gscore`；对同一手势做"注入前 / 注入后"两次读数，比较分数是否有增量。
   * ⚠️ 分数是累计值，只能看增量；换手势测试前先退出重进游戏（reset 会清零）。 */
  /* 帧率统计（2 秒）：`fps` —— 判断软渲染有没有把帧率吃掉（见 sFpsProbe 的说明） */
  if (strncmp(buf, "fps", 3) == 0) {
    sFpsProbe = 1;
    sFpsCnt = 0;
    sFpsT0 = 0;
    LOGD("PocketGame: fps 统计开始（2 秒后打印）");
    return;
  }

  /* 每帧耗时拆解：`bench`（2 秒后打印 update/render/present 的平均微秒）。
   * 只在游戏内有效（其它模式没有画布渲染）。 */
  if (strncmp(buf, "bench", 5) == 0) {
    sBenchProbe = 1;
    sBenchUpd = sBenchRnd = sBenchPre = sBenchLoop = 0;
    sBenchFrames = 0;
    sBenchT0 = 0;
    LOGD("PocketGame: bench 统计开始（2 秒后打印 update/render/present）");
    return;
  }

  if (strncmp(buf, "gscore", 6) == 0) {
    if (!gGame) {
      LOGD("PocketGame: gscore 没有运行中的游戏");
      return;
    }
    LOGD("PocketGame: gscore title=%s score=%d state=%d", gGame->title(), gGame->score(),
         (int)gGame->state());
    return;
  }

  /* 把调试命令转发给当前游戏：`gdbg <...>`（各游戏自己解析，见 Game::debugCmd）。
   * 用途举例：**打地鼠连击赞赏的验收** —— `gdbg hit 0` / `gdbg hit 1` / … 连续命中
   *   （手点复现不出连击：冒头位置随机，点空一次连击就归零）。 */
  if (strncmp(buf, "gdbg ", 5) == 0) {
    if (!gGame) {
      LOGD("PocketGame: gdbg 没有运行中的游戏");
      return;
    }
    LOGD("PocketGame: gdbg '%s' -> %s", buf + 5, gGame->title());
    gGame->debugCmd(buf + 5);
    return;
  }

  int idx = -1, st = 0;
  if (sscanf(buf, "%d %d", &idx, &st) < 1) return;
  LOGD("PocketGame: autostart cmd '%s' -> idx=%d state=%d", buf, idx, st);
  if (idx < 0) {
    exitGameToMenu();
    return;
  }
  if (idx >= gameCount()) return;
  startGame(idx);
  if (!gGame) return;
  if (st == 1) {
    gGame->onKey(PG_KEY_A);
    if (gGame->state() != GSTATE_OVER) gGame->setState(GSTATE_RUNNING);
    gHud.reset();
    syncHud(true);
  } else if (st == 2) {
    gGame->onKey(PG_KEY_A);
    setPaused(true);
  }
}

/* ==================== 声卡空闲自动让出 ====================
 * 背景：常开 PCM 流独占 `hw:0,0`（"音效进出无爆音"的前提），但**别的进程打开 card0 会挂住**
 * （实测 `/bin/tinyplay x.wav -D 0` 直接挂住、无任何输出）。专用机上无所谓；若要给系统
 * 提示音/产测脚本留出声卡，就让它在**真正空闲**时让出去、要用的时候再拿回来。
 *
 * 判据（三条都要满足）：① 阈值 > 0（QA `pcmidle <秒>` 可改，0 = 关，永远常开）；
 * ② 没有在线流在跑（PgStream 正往这条流灌 PCM，让出会把声音弄断）；
 * ③ 距上次"要用声音"（音效 / 灌 PCM）超过阈值。
 * 拿回是**按需**的：GameHostImpl::playSfx 先 acquire；PgStream 起流前也会试一次。
 */
/* ==================== 视频链路卡死自愈 ====================
 * 现象：某平台（V85X）上"被手动停止过的解码通道"会变脏 —— 下一轮再收流时
 * **音频正常但一帧视频都不出**。本函数检测这种情况（音频在流 + 4s 无视频帧），
 * 然后**复位应用进程**（`_exit(0)`）：zkgui 被 init 托管，会立刻重新拉起，
 * MPP/解码通道状态焕然一新，用户再投一次就正常。
 * 为什么选"复位进程"而不是在进程内修：实测 7 种进程内的收尾/复用方案都救不回来
 * （见 docs/online-media.md §十五），进程重启是唯一可靠且代价可接受（~1s）的办法。
 * 防抖：90 秒内最多复位一次，避免任何误判导致重启循环。 */
/* ---- "停止就复位"（主动路径，2026-09-13 用户确认）----
 * 本平台"被手动停止过的解码通道"下一轮再收流**不出画**（见 §十五）。
 * 与其等下一轮投屏黑 4 秒再靠自愈复位，不如**一按停止就复位**：
 * 应用回到主界面（本来停止后也是回主界面），~1s 后重新拉起，之后每次投屏都是干净的。
 * 触发条件：① 投屏页已收掉；② 流任务已收尾；③ PgStream 报告"本轮是被手动停止的"。
 * 自然播完（EOF/到时长上限）**不触发**（那种通道是可以继续用的）。 */
void tickStopReset() {
  if (gStreamCasting) return;                 // 等投屏页收掉
  if (pg::StreamPlayer::running()) return;    // 等收尾完成
  if (!pg::StreamPlayer::consumeNeedsReset()) return;
  LOGW("PocketGame: 投屏已按停止 → 复位应用（下次投屏立刻可用；init 会立刻拉起）");
  usleep(300 * 1000);
  _exit(0);
}

/* 投屏页的"缓冲底图"收敛：**每拍按"是否已出画面"设一次**（幂等）。
 * 为什么不用"事件置位"就够：出画有三条来源（框架播放器 PLAY_STARTED / 在线流解码帧 /
 * 我们自己的硬解链路），漏一条底图就会一直挡着视频。这里按状态收敛，漏了也会下一拍纠正。 */
void tickVideoCover() {
  if (!mWinCastPtr || !mWinCastPtr->isWndShow()) {
    setVideoCover(mImgCastCoverPtr, false);   // 投屏页没显示：底图一律收起
    return;
  }
  bool painted = (gCastPainted != 0) ||
                 (gStreamCasting && pg::H264Player::framesDecodedInRun() > 0);
  setVideoCover(mImgCastCoverPtr, !painted);
}

void tickVideoSelfHeal() {
  static long long sLastHeal = 0;
  if (!gStreamCasting) return;            // 只在投屏中判断
  if (!pg::StreamPlayer::videoStalled()) return;
  long long now = pgNowMs();
  if (sLastHeal && now - sLastHeal < 90000) return;
  sLastHeal = now;
  LOGW("PocketGame: 投屏中音频在流但 4s 无视频帧 → 解码通道变脏，复位应用（init 会立刻拉起）");
  /* 先把投屏页收掉，让用户看到的是"回到主界面"而不是黑屏 */
  stopCast();
  usleep(300 * 1000);
  _exit(0);   // 直接退出：内核回收全部资源（含 DLNA 端口/MPM 通道），init 重新拉起
}

/* ==================== 工作界面禁止屏保（2026-09-14 用户报的 bug）====================
 * 屏保是"主界面空闲 30s 自动起"，而它**盖在视频层/游戏画布上面** ——
 * 用户在看视频/玩游戏时被翻页钟打断，观感就是"视频没了 / 游戏花屏"。
 * 框架给了开关（EasyUIContext.h 里"升级界面不能进屏保"同款用法）：
 *   setScreensaverEnable(false) 关掉屏保检测；回主界面时再打开并重置计时。
 * 判据 = "当前是不是工作界面"，在主循环里每拍同步（内部去重，稳定态零开销）：
 *   · MODE_GAME / MODE_TOOL —— 画布游戏、原生工具页
 *   · gStreamPageShown      —— 投屏/在线流页（视频层的透明窗口就在这页）
 * ⚠️ 回主界面时 `setScreensaverEnable` 只在"超时 > 0"时才打开：QA `saver to 0` 的语义是
 *    "永不进屏保"，不能被这里又打开。 */
void tickScreensaverPolicy() {
  static int sWork = -1;   // -1 = 未知 / 0 = 主界面 / 1 = 工作界面
  /* ⚠️ 主界面不在前台（用户进了独立 ftu：wifi/remote/工具页…）时**什么都别做**：
   *    否则这里会按 gMode==MODE_MENU 判成"非工作界面"→ 把屏保重新打开，
   *    而那个页面自己刚把它关掉 —— 两边互相覆盖，表现为"工作页里照样弹屏保"。
   *    屏保开关的归属：**谁在前台谁说了算**（见 src/ui/ToolPage.h 的说明）。 */
  if (!gActivityActive) return;
  /* ⚠️ IPTV 页已独立，它自己管屏保（onUI_show 关 / onUI_quit 恢复）—— 这里不再看它。 */
  const bool work = (gMode != MODE_MENU) || gStreamPageShown;
  /* ⚠️ 收敛判据必须是"**框架的实际开关** vs 期望值"，不能缓存模式（踩过）：
   *    原来缓存 `sWork`，模式没变就直接 return。但独立 ftu（wifi/工具页）会自己把屏保
   *    关掉，回主界面时模式还是 MODE_MENU（和进之前一样）⇒ 判"没变化"直接返回，
   *    屏保**再也回不来**（实测：主界面 QA `saver` 报 enable=0）。
   *    改成对实际状态收敛后，谁改的都能纠回来，且天然幂等。 */
  const bool want = !work && (EASYUICONTEXT->getScreensaverTimeOut() > 0);
  if (EASYUICONTEXT->isScreensaverEnable() == want) return;   // 已经一致：不动（也别重置计时）
  sWork = work ? 1 : 0;
  EASYUICONTEXT->setScreensaverEnable(want);
  if (want) {
    EASYUICONTEXT->resetScreensaverTimeOut();   // 重新计时，别"刚回菜单就进屏保"
    LOGD("PocketGame: 回到主界面 -> 屏保恢复（timeout=%d）",
         EASYUICONTEXT->getScreensaverTimeOut());
  } else {
    if (EASYUICONTEXT->isScreensaverOn()) EASYUICONTEXT->screensaverOff();
    LOGD("PocketGame: 进入工作界面（mode=%d 投屏页=%d）-> 关闭屏保", (int)gMode,
         (int)gStreamPageShown);
  }
}

void tickPcmIdle() {
  if (!gHost) return;
  static long long sNextCheck = 0;
  long long now = pgNowMs();
  if (now < sNextCheck) return;
  sNextCheck = now + 500;  // 阈值是秒级，500ms 查一次足够
  int idleSec = gHost->audio.pcmIdleSec();
  if (idleSec <= 0 || !gHost->audio.pcmHeld()) return;
  if (pg::StreamPlayer::running()) return;
  long long idle = gHost->audio.idleMs();
  if (idle > (long long)idleSec * 1000) {
    LOGD("PocketGame: 声卡空闲 %llds（阈值 %ds）→ 让出 hw:0,0（有声音需求会自动拿回）",
         idle / 1000, idleSec);
    gHost->audio.releasePcm();
  }
}

void pollAutoStart() {
  static char lastAll[512] = {0};
  FILE *f = fopen("/tmp/pg_autostart", "rb");
  if (!f) return;
  char all[512] = {0};
  size_t n = fread(all, 1, sizeof(all) - 1, f);
  all[n] = 0;
  fclose(f);
  if (all[0] == 0 || strcmp(all, lastAll) == 0) return;
  snprintf(lastAll, sizeof(lastAll), "%s", all);

  // 逐行执行
  char *p = all;
  while (*p) {
    char *nl = strchr(p, '\n');
    if (nl) *nl = 0;
    // 去掉行尾 \r 与首尾空格
    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ')) p[--len] = 0;
    while (*p == ' ') ++p;
    if (*p) runAutoCmd(p);
    if (!nl) break;
    p = nl + 1;
  }
}

// 列表子项上色：bgColorTab 与 backgroundColor 都写，兼容不同引擎取值优先级
void paintSub(ZKListView::ZKListSubItem *sub, uint32_t color) {
  if (!sub) return;
  sub->setBackgroundColor(color);
  sub->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, color);
}

/* 卡片圆形图标控件个数（**四处必须一致**，缺一处就会出现"某些应用没图标/图标空白"）：
 *   ① `ui/main.html` 的 Icon0..Icon36（37 个同位置重叠的控件）
 *   ② `tools/gen_icons.py` 的 ICONS 表（37 条，slot 0..36）
 *   ③ 本文件的回调桩 `onButtonClick_Icon0..Icon36`
 *   ④ `tools/gen_ui.py` 的 `HIDDEN_CONTROLS`（`range(37)` —— 首帧全隐藏，否则 37 个图标叠一起）
 * ⇒ 上限 = 应用总数（`kAppCount`，超出没有控件可显示，那个 slot 的卡片就是**空白**）。
 *   ★★ 2026-09-16 起 `onUI_init` 里加了一条**开机自检告警**（kIconCount < appCount 就 WARN）：
 *      这类"加应用忘了同步四处"以前是**静默失效**（卡片凭空消失，只能靠肉眼发现），
 *      现在开机日志里直接点名。**加应用时四处一起加**，见 docs/page-split-plan.md。
 *   · 2026-09-20：35 -> **37**（新增 slot 35 飞天仙女 / 36 可爱小猫）。 */
// ★ 2026-09-23：37 -> **38**（新增 slot 37 智能家居）。四处同步：
//   ui/main.html 的 Icon37 / tools/gen_icons.py 的 ICONS /
//   本文件的 kIconCount / tools/gen_ui.py 的 HIDDEN_CONTROLS。
static const int kIconCount = 38;

/*
 * 卡片圆形图标：item 模板里有 **21 个同位置重叠的图标控件**（Icon0..Icon20，
 * 各静态引用一张 PNG，由 tools/gen_icons.py 生成），这里按 slot 只显示其中一个。
 *
 * 为什么用这种"笨"办法 —— 三个方案都在真机上验证过，只有这个能work：
 *   ① 运行时 setBackgroundPic("images/app_icon_N.png") → **所有行显示同一个图标**
 *      （换成静态字符串字面量传入也一样，排除"栈缓冲生命周期"的猜测）；
 *   ② 拼一张雪碧图 + setBackgroundCrop 按 slot 切格 → 同样所有行一样；
 *   ③ "通用圆形蒙版图 + 每行 paintSub 底色" → **setBackgroundColor 会把 backgroundPic 清掉**
 *      （实测：圆形消失，只剩纯色方块）。
 *   ⇒ 结论：subItem 的**图片属性不按行区分，且图片与底色互斥**。
 *
 * slot 传 -1 = 空行（19 个全隐藏）。
 *
 * ⚠️⚠️ **不要用行号 index 做缓存键**（2026-09-14 用户报的 bug）：列表项视图会被**跨行复用**
 * （滚动、切换分类时同一块 item 换一行显示），而"哪个 Icon 可见"是挂在**视图**上的。
 * 原来 `sShown[index]` 命中就 return，复用过来的视图就会顶着**上一行（别的 slot）**的图标
 * 显示下去 —— 现象正是"滚着滚着/换完分类，所有行都变成同一个图标"。
 * ⇒ 改成**以控件自身当前可见性为准**：先读一遍，只有不一致才写。
 *    稳定态下 19 次读、0 次写（不产生任何重绘），视图怎么复用都不会错。
 */
void syncRowIcon(ZKListView::ZKListItem *item, int slot) {
  if (!item) return;
  bool need = false;
  for (int i = 0; i < kIconCount && !need; ++i) {
    ZKListView::ZKListSubItem *ic = item->findSubItemByID(ID_MAIN_Icon0 + i);
    if (ic && ic->isVisible() != (i == slot)) need = true;
  }
  if (!need) return;
  for (int i = 0; i < kIconCount; ++i) {
    ZKListView::ZKListSubItem *ic = item->findSubItemByID(ID_MAIN_Icon0 + i);
    if (ic) ic->setVisible(i == slot);
  }
}

// 把一次「画布坐标」触摸派发给当前应用（真实触摸与 QA 脚本共用）
/* ---------------- 画布底部软按钮（2026-09-13 新增） ----------------
 * 游戏只"声明"按钮（pg::Game::softButtons），**绘制与命中统一在这里做**：
 *   - 消灭"推箱子自己画一排底部按钮 + 硬编码命中"这种重复实现；
 *   - 给俄罗斯方块这类手势冲突严重的游戏一条出路（左/下/右/转 变成按钮）。
 * 软按钮区域优先于游戏自身的 onTouch —— 命中即消费，游戏收不到该事件。
 */
int gSoftPressed = -1;  // 当前按下的软按钮 id（-1 = 无），用于画按下态

void drawSoftButtons(Canvas &c) {
  int n = 0;
  const SoftButton *bs = gGame ? gGame->softButtons(n) : 0;
  if (!bs || n <= 0) return;
  for (int i = 0; i < n; ++i) {
    const SoftButton &b = bs[i];
    bool on = (b.id == gSoftPressed);
    c.fillRectRound(b.x, b.y, b.w, b.h, 10, on ? 0xFF3E5C86 : 0xFF243244);
    c.strokeRect(b.x, b.y, b.w, b.h, 1, on ? 0xFF7FA8E0 : 0xFF44608A);
    if (b.label && b.label[0]) {
      int tw = c.textW(b.label, 2);
      int th = c.textH(2);
      c.text(b.x + (b.w - tw) / 2, b.y + (b.h - th) / 2, b.label, 2,
             0xFFF2F2F7);
    }
  }
}

bool dispatchCanvasTouch(int act, int x, int y) {
  /* 闹钟响铃时：触摸先停铃（与按键同一条语义，见 handleLogicalKey 开头） */
  if (pg::Alarm::instance().ringing()) {
    pg::Alarm::instance().stopRing();
    LOGD("PocketGame: 触摸停铃（%d,%d）", x, y);
    return true;
  }
  if (gMode != MODE_GAME || !gGame) return false;
  if (gPauseWinShowing) return false;  // 原生暂停弹窗显示中：触摸交给弹窗按钮
  bool inside = (x >= 0 && y >= 0 && x < CANVAS_W && y < CANVAS_H);
  if (act == PG_TOUCH_DOWN) {
    gTouchInsideCanvas = inside;
    if (!inside) return false;
  }
  if (!gTouchInsideCanvas) return false;

  // ① 软按钮优先命中
  int sn = 0;
  const SoftButton *sbs = gGame->softButtons(sn);
  if (sbs && sn > 0) {
    for (int i = 0; i < sn; ++i) {
      const SoftButton &b = sbs[i];
      if (x < b.x || x >= b.x + b.w || y < b.y || y >= b.y + b.h) continue;
      // 按下即响应（连点手感好 —— 俄罗斯方块要快速连按左右移动）
      if (act == PG_TOUCH_DOWN) {
        gSoftPressed = b.id;
        gGame->onSoftButton(b.id);
      } else if (act == PG_TOUCH_UP) {
        gSoftPressed = -1;
      }
      if (act == PG_TOUCH_UP) gTouchInsideCanvas = false;
      return true;  // 区域内的事件一律消费，不落到游戏手势里
    }
  }
  if (act == PG_TOUCH_DOWN) gSoftPressed = -1;  // 按在别处 → 清掉残留按下态

  bool consumed = gGame->onTouch(act, x, y);
  if (act == PG_TOUCH_UP) gTouchInsideCanvas = false;
  return consumed;
}

}  // namespace

/* 独立 ftu 的页面复用主界面的宿主（音效 + 存档）。见 src/ui/ToolPage.h。
 * ⚠️ 两个坑都踩过：
 *   ① 必须放在匿名命名空间**之外**，否则是内部链接（链接期 undefined reference）；
 *   ② 必须**用 `namespace pg` 包起来**：写成 `pg::Host *pgHost()` 定义的是
 *      **全局** `::pgHost`（限定名在这里只是返回类型），签名对不上照样 undefined。 */
namespace pg {
Host *pgHost() { return gHost; }
}  // namespace pg

/* ==================================================================
 *                    FlyThings 逻辑入口（回调名由控件 caption 决定）
 * ================================================================== */

/* ★ 2026-09-16：主界面的「标题」与「电量」**已搬到全局导航栏**（navibar）——
 *   用户要求「状态栏显示不完整，直接把电池，页面 title 都放到状态栏这样子改动最小」。
 *   ui/main.html 里的 TextTitle / BattShell / BattFill / BattBolt **控件已删除**
 *   （控件没了，这里的 mBattFillPtr 等指针也不存在），所以：
 *     · `syncBatteryIcon()` 与 `tickBattery()` **整套已删**（原定义见下方"电池"区，已移除）；
 *     · 电量改由 `navibar.cc` 的 `refreshBatt()` 画（素材同一套 batt_*.png）。
 *   要改电量的显示逻辑 ⇒ 改 navibar.cc，**别在这里加回控件**。 */

/**
 * 注册定时器
 * 注意：id 不能重复
 */
static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_LOOP, LOOP_MS},  // 游戏主循环 ~30fps
};

/**
 * 当界面构造时触发
 */
/* ==================== 投屏 / 视频播放（DLNA 渲染端实验） ====================
 * ZKVideoView::play(路径或URL) 是框架播放器链路：它自己解封装/解码并把画面送到
 * disp 视频层，UI 侧的 videoview 控件只是给视频层开的一块透明窗口
 * （见 ui/main.html 的 WinCast 注释）。
 * ⚠️ 因此 fb0 截图**看不到**视频画面 —— 验收只能靠：
 *    ① 这里的播放器回调日志（PLAY_STARTED / ERROR_*）；
 *    ② `cat /sys/class/disp/disp/attr/sys` 看视频层是否 enable + 有没有地址。
 */
#ifdef FUN_BUILD
class CastMsgListener : public ZKVideoView::IVideoPlayerMessageListener {
 public:
  // ⚠️ 这里的 msg 是 **ZKVideoView 自己的** 枚举（STARTED=0 / COMPLETED=1 / ERROR=2），
  //    不是 ZKMediaPlayer::E_MSGTYPE_*（那个 PLAY_STARTED=6）。用错枚举就一条都匹配不上
  //    （踩过：状态字一直停在"正在打开…"）。
  void onVideoPlayerMessage(ZKVideoView *pVideoView, int msg) override {
    LOGD("PocketGame cast: 播放器消息 msg=%d", msg);
    if (msg == ZKVideoView::E_MSGTYPE_VIDEO_PLAY_STARTED) {
      int dur = pVideoView ? pVideoView->getDuration() : -1;
      LOGD("PocketGame cast: PLAY_STARTED 开始出画面 dur=%dms", dur);
      gCastPainted = 1;   // 出画面了 → 下一拍收起缓冲底图
      if (mTextCastMsgPtr) {
        char b[64];
        snprintf(b, sizeof(b), "播放中 · %d.%ds", dur / 1000, (dur % 1000) / 100);
        mTextCastMsgPtr->setText(b);
      }
    } else if (msg == ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED) {
      LOGD("PocketGame cast: PLAY_COMPLETED 播放结束（解码链路全程无错）");
      if (mTextCastMsgPtr) mTextCastMsgPtr->setText("播放结束");
      /* ⚠️ 播完必须把状态机复位：否则之后控制器的 Pause/Play（点暂停、拖进度）
       *    会打到"已播完"的播放器上，报 pause/resume called in an invalid state: 0
       *    （实测踩过）。DLNA 语义上"播完"= STOPPED。 */
      sCastLoaded = false;
      sCastPaused = false;
      if (gDlna) gDlna->setTransportState("STOPPED");
      sCastCleanupAtMs = pgNowMs() + 10000;  // 10s 后再删文件（等播放器收尾）
    } else if (msg == ZKVideoView::E_MSGTYPE_VIDEO_PLAY_ERROR) {
      LOGD("PocketGame cast: PLAY_ERROR 播放出错");
      sCastLoaded = false;
      sCastPaused = false;
      if (gDlna) gDlna->setTransportState("STOPPED");
      if (mTextCastMsgPtr) mTextCastMsgPtr->setText("播放出错（格式/源不支持？）");
    }
  }
};
CastMsgListener sCastMsg;
#endif

// 显示投屏页并播放（pathOrUrl 可为 /tmp/x.mp4 或 http://host/x.mp4）
void startCast(const char *pathOrUrl) {
#ifdef FUN_BUILD
  if (!mCasterPtr) return;
  LOGD("PocketGame cast: play '%s' (win=%p caster=%p msg=%p)", pathOrUrl,
       (const void *)mWinCastPtr, (const void *)mCasterPtr, (const void *)mTextCastMsgPtr);
  if (mTextCastMsgPtr) mTextCastMsgPtr->setText("正在打开…");
  gCastPainted = 0;                       // 还没出画面 → 底图铺上（tickVideoCover 收敛）
  setVideoCover(mImgCastCoverPtr, true);
  exitGameToMenu();          // 先把游戏画布收掉
  if (mWinGamePtr) mWinGamePtr->hideWnd();
  if (mWinCastPtr) mWinCastPtr->showWnd();
  mCasterPtr->setVisible(true);
  /* ★ 2026-09-16「检讨 UI 覆盖」修正 ②：投屏页也是"视频区 480x700 @y=0"，导航栏会压掉
   *   画面顶部 52px ⇒ 让 navibar 自己收起来（见 src/platform/PgNavi.h）。
   *   与 stopCast() 成对；另外 onUI_hide/onUI_quit 里还有一道兜底。 */
  pg::setVideoPage(true);
  /* 画面旋转（本地文件走的是框架播放器这条路）。
   * ZKVideoView::setRotation 的参数是"顺时针档位"：0=不转 / 1=90 / 2=180 / 3=270，
   * 与 MPP 的 ROTATE_E、参考项目 V851ExtendedScreen 的 disp_rot_e 完全一致。
   * ⚠️ 只在**手动指定过**角度时才设置（auto 时让播放器自己处理视频自带的旋转标志，
   *    否则会和它的内建行为叠加、转两遍）。 */
  if (gHost) {
    int rot = gHost->store.rotDeg();
    if (rot >= 0) {
      mCasterPtr->setRotation(((rot % 360) / 90) % 4);
      LOGD("PocketGame cast: 框架播放器旋转 = %d°（档位 %d）", rot, ((rot % 360) / 90) % 4);
    } else {
      LOGD("PocketGame cast: 框架播放器旋转 = auto（交给播放器按视频自带标志处理）");
    }
  }
  mCasterPtr->play(pathOrUrl, 0);
  sCastLoaded = true;
  sCastPaused = false;
#endif
}

void stopCast() {
#ifdef FUN_BUILD
  sCastLoaded = false;
  sCastPaused = false;
  gCastPainted = 0;
  /* 在线流链路（PgStream）也要停 —— 投屏页的"停止播放"按钮就调这里 */
  if (gStreamCasting) {
    pg::StreamPlayer::stop();
    gStreamCasting = 0;
  }
  if (mCasterPtr) mCasterPtr->stop();
  if (mWinCastPtr) mWinCastPtr->hideWnd();
  pg::setVideoPage(false);      // 让 navibar 回来（与 startCast 成对，见 PgNavi.h）
  if (mTextKeyHintPtr) mTextKeyHintPtr->setText("[A] 确定    [B] 返回    [C] 菜单");
  LOGD("PocketGame cast: stop");
#endif
}

/* ==================== 我们自己的流式播放（画面由 PgStream 送） ====================
 * V85X 的显示分层：UI 层（最顶、不透明）/ disp 视频层（在下）/ 再下面按 4,3,2,1 叠。
 * UI 层里 videoview 控件覆盖的区域是**透明窗口** —— 下层视频层的画面从那儿透出。
 * 所以"自维护出图"要两件事：
 *   ① UI 侧：把投屏页（含 480x700 的 Caster）显示出来 —— 这就是"做好图层"；
 *   ② 出图侧：往视频层送帧 —— 由 PgStream（ffmpeg 解封装 + 设备硬解）负责。
 * ⚠️ 只 show 窗口、**不调 mCasterPtr->play()**：那个是框架播放器链路，会和我们的 VO 抢。
 *    （知识库 v85x/videoview-transparent-window.md：走自维护出图时控件零关联代码） */
void showCastPageForStream(const char *tip) {
#ifdef FUN_BUILD
  exitGameToMenu();  // 先把游戏画布收掉
  if (mWinGamePtr) mWinGamePtr->hideWnd();
  if (mWinCastPtr) mWinCastPtr->showWnd();
  if (mCasterPtr) mCasterPtr->setVisible(true);   // ⚠️ 必须可见，隐藏了就没有透明窗口
  if (mTextCastMsgPtr && tip) mTextCastMsgPtr->setText(tip);
  pg::setVideoPage(true);     // 同 startCast：投屏页也要把导航栏收起来（见 PgNavi.h）
  (void)tip;
#endif
}

void hideCastPageForStream() {
#ifdef FUN_BUILD
  if (mWinCastPtr) mWinCastPtr->hideWnd();
  pg::setVideoPage(false);    // 让 navibar 回来（与 showCastPageForStream 成对）
  if (mTextKeyHintPtr) mTextKeyHintPtr->setText("[A] 确定    [B] 返回    [C] 菜单");
#endif
}

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
  /* 开机就定下北京时间。为什么必须在这里而不是"校时时再设"：
   * setenv("TZ") 只作用于**当前进程**，zkgui 一重启就回到 UTC —— 没校时过或重启后，
   * 所有 localtime 的显示（屏保大钟、日期）都会差 8 小时。见 PgTime::ensureTimezone。 */
  pg::TimeSync::ensureTimezone();
  /* 30 秒无操作进屏保（APP_TYPE_SYS_SCREENSAVER，见 src/logic/screensaver.cc）。
   * ⚠️ 实测（2026-09-13）：**框架不读 EasyUI.cfg 里的 screensaverTimeout** ——
   *    出厂那份(/res/etc)与工程那份(/tmp)都写了 30，运行起来仍是 -1
   *    （证据：QA `saver` 打印 timeout=-1 enable=1）；把 /tmp 那份改成 20 再重启也一样。
   *    所以**必须用 API 显式设**。package.properties 里那一项保留作双保险
   *    （万一某版框架开始读它，两边值一致，不会打架）。 */
  EASYUICONTEXT->setScreensaverTimeOut(30);
  EASYUICONTEXT->setScreensaverEnable(true);
  EASYUICONTEXT->registerKeyListener(&gKeyRouter);
  installFatalHandler();  // 崩溃时把 PC/LR 打到 logcat（详见上面的说明）
  /* 开机就把 IPv6 关掉：ffmpeg 4.1 的 tcp_open() 只试 getaddrinfo 的第一个地址，
   * 而本板 wlan0 只有 link-local IPv6（无全局路由）→ 一旦解析结果里 IPv6 在前，
   * https 就必然 "Network unreachable"。这里提前关，比等到第一次联网再关更稳
   * （内核移除 IPv6 地址是异步的，见 PgFf::disableIpv6 的说明）。 */
  pg::Ff::disableIpv6();
#endif

  seedRandom();

  /* 闹钟守护线程：**必须在这里（开机）就起**，而不是靠主界面定时器 —— 主界面的
   * onUI_Timer 只在主界面在前台时跑（用户进 wifi/遥控这类独立 ftu 后就不跑了），
   * 而闹钟要"任何页面都会响"。守护线程独立于 activity 生命周期，见 platform/PgAlarm.*。 */
  pg::Alarm::instance().start();
  /* 到点回调来自**闹钟守护线程**：那里只置标志，弹页交给主循环（tickAlarmRing） */
  pg::Alarm::instance().setFireCb(onAlarmFireFromGuard);
  /* 本地 DNS 中继：本板根分区只读、**没有 /etc/resolv.conf**，musl 默认只查 127.0.0.1:53
   * ⇒ 不解决的话任何域名都解析不了（IPTV/域名源/按名字找设备全废）。见 platform/PgDns.h。 */
  pg::DnsRelay::instance().start();

  /* 游戏贴图：装上设备侧解码器（框架自带 BitmapHelper，解 PNG）。
   * 之后 `pg::sprites().get("images/game/xxx.png")` 即可 1:1 贴到画布上，
   * 见 core/PgSprite.h 与 docs/game-art-pipeline.md。 */
  pg::installSpriteLoader();
  /* core 层（画布/游戏/贴图）不依赖 easyui，所以它自己的日志走钩子转过来。
   * 没有这一步，"素材尺寸与清单不符"这类问题在设备上就是**静默**的（draw 出来只是差几像素）。*/
  pg::setLogSink([](const char *msg) { LOGD("%s", msg); });

  gHost = new GameHostImpl();
  gHost->init();
  /* 投屏画面旋转：从存档恢复到播放器。
   * ⚠️ 必须持久化：本工程"停止投屏会复位应用"（解码通道限制，见 docs/online-media.md §十五），
   *    只放内存里的话，用户设好角度一停止就丢了，下次投屏又是歪的。 */
  pg::StreamPlayer::setRotation(gHost->store.rotDeg());
  /* zkmedia 码流缓冲（ZKMEDIA_H264_VBVSIZE）：从存档灌进播放器，**必须在任何一次起播之前**。
   * 默认 1MB；不设的话 720p + 旋转起播会让进程静默退出（见 docs/h264-direct.md）。 */
  pg::H264Player::setVbvBytes(gHost->store.vbvBytes());
  LOGD("PocketGame: zkmedia 码流缓冲 VBV = %d 字节（存档值）", pg::H264Player::vbvBytes());
  /* 电池：开机就初始化（照参考工程"启动时先读一次"），主循环再靠 tick() 维持刷新 */
  pg::Battery::init();
  // 给独立 activity（wifiLogic）转发音量操作用（见 PgAudio.h 的钩子说明）
  pg::setVolumeHook([](int d) -> int {
    return gHost ? gHost->audio.volumeStep(d) : -1;
  });
  /* 绝对设值（2026-09-17：系统设置页的进度条支持**直接拖动**）——
   * 与 `volumeStep` 是两条路：拖动是绝对定位语义，用步进凑会连发 N 次广播。 */
  pg::setVolumeSetHook([](int pct) -> int {
    if (!gHost) return -1;
    gHost->setVolumePercent(pct);        // 写 codec + 落盘 + noteVolumePercent
    return gHost->volumePercent();       // 读回实际值（codec 会量化）
  });
  /* 静音钩子（2026-09-16）：真改 codec 输出开关。音量键"+ 时先取消静音"的逻辑
   * 在 pg::volumeStepGlobal 里（见 PgAudio.h 的说明），这里只负责"真去写开关"。 */
  pg::setMuteHook([](bool m) -> bool {
    return gHost ? gHost->audio.setMuted(m) : false;
  });

  gMode = MODE_MENU;
  if (mWinGamePtr) mWinGamePtr->hideWnd();
  if (mWinPausePtr) mWinPausePtr->hideWnd();
  gPauseWinShowing = false;
  if (mWinClockPtr) mWinClockPtr->hideWnd();
  if (mWinCalcPtr) mWinCalcPtr->hideWnd();
  if (mWinReactPtr) mWinReactPtr->hideWnd();
  if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
  // 投屏页：初始隐藏 + 挂播放器回调（回调里只打日志/改状态字）
  if (mWinCastPtr) mWinCastPtr->hideWnd();
  pg::setVideoPage(false);          // 兜底：开机/重载布局时不许留着"视频页"标志
  pg::setNaviTitle(0);              // 兜底：同理，标题回到按 Activity 映射
  if (mCasterPtr) mCasterPtr->setVideoPlayerMessageListener(&sCastMsg);
  if (mTextKeyHintPtr) mTextKeyHintPtr->setText("[A] 确定    [B] 返回    [C] 菜单");
  if (mTextTipPtr) mTextTipPtr->setText("点卡片进入应用 · 列表可上下滑动");
  if (mTextVersionPtr) mTextVersionPtr->setText(kAppVersion);   // 版本号只在上面 kAppVersion 一处定义
  // 按钮点击由框架按 caption 自动绑定到 onButtonClick_<Caption>
  syncTabs();
  // ★ 2026-09-16：原来的 `syncBatteryIcon(true)` 已删除 —— 电量搬到全局导航栏（见本文件顶部说明）
  refreshList();
  gMenuDirty = 2;  // 再让主循环刷一次（onUI_init 时列表可能还没布局好）

  /* ★★ 开机自检：**图标控件数必须 > 最大 slot**（2026-09-16 加，2026-09-24 修判据）。
   *   为什么必须有：这条线原来是"静默失效"—— 加应用时漏改 kIconCount /
   *   main.html 的 Icon 控件，那一款应用在网格里就是**一张空白卡片**
   *   （用户报「系统设置图标没有找到，进不去」就是这个形态），
   *   而日志一声不吭，只能靠肉眼满屏找。现在开机直接点名。
   *   ★ 判据修正：图标是**按 slot 索引**的（Icon<slot>），所以要跟"**最大 slot**"比，
   *     不能跟"应用个数"比 —— 删过应用之后 slot 会跳号（现在最大 37、个数只有 34），
   *     拿个数比会在"新增一个 slot 更大的应用"时算不出来（个数 35 < 38 不报警，
   *     但 slot 38 已经没有对应图标控件了）。 */
  const int maxSlot = maxAppSlot();
  if (maxSlot >= kIconCount) {
    LOGW("PocketGame: 最大 slot %d ≥ 图标控件数 %d ⇒ slot %d 起的卡片会是空白！"
         "（四处同步：main.html 的 Icon / gen_icons.py / 本文件 kIconCount / gen_ui.py）",
         maxSlot, kIconCount, kIconCount);
  }
  if (maxSlot >= ScoreStore::MAX_GAMES) {
    LOGW("PocketGame: 最大 slot %d ≥ 存档上限 %d ⇒ 该应用的最高分会被静默丢弃！",
         maxSlot, ScoreStore::MAX_GAMES);
  }

  LOGD("PocketGame: init ok, %d apps(游戏%d/工具%d/系统%d), store=%s, sfx=%s, audio=%s, sound=%s",
       appCount(), categoryCount(APP_GAME), categoryCount(APP_TOOL),
       categoryCount(APP_SYSTEM), gHost->store.path(),
       gHost->audio.available() ? "on" : "off", gHost->audioBackend(),
       gHost->soundOn() ? "on" : "off");
}

/**
 * 当切换到该界面时触发
 */
static void onUI_intent(const Intent *intentPtr) {
  if (intentPtr != NULL) {
    //TODO
  }
}

/*
 * 当界面显示时触发
 */
static void onUI_show() {
  LOGD("PocketGame: onUI_show (mode=%d)", (int)gMode);
  gActivityActive = true;
  // 每次界面显示时把列表标脏（覆盖"框架热重载布局"与"从后台返回"两种情况）
  if (gMode == MODE_MENU) gMenuDirty = 2;
}

/*
 * 当界面隐藏时触发（切后台/进屏保）：自动暂停，回来不会白死
 */
static void onUI_hide() {
  gActivityActive = false;  // 让出按键（例如打开了系统 WiFi 设置页）
  /* ★ 两个全局粘性标志必须在这里复位（2026-09-16）：本页退到后台/被关掉时，
   *   navibar（常显，不随本页退出）会继续读它们 ⇒ 不复位的话"标题卡在游戏名上、
   *   导航栏永远藏着"。navibar 侧只有 Activity 白名单自愈那道兜底，不能只靠它。 */
  pg::setNaviTitle(0);
  pg::setVideoPage(false);
  if (gMode == MODE_GAME && gGame && gGame->state() == GSTATE_RUNNING) {
    setPaused(true);
  }
}

/*
 * 当界面完全退出时触发
 */
static void onUI_quit() {
  pg::setNaviTitle(0);      // 同上：本页退出后导航栏不能继续显示游戏名
  pg::setVideoPage(false);
#ifdef FUN_BUILD
  EASYUICONTEXT->unregisterKeyListener(&gKeyRouter);
#endif
  exitGameToMenu();
  if (gDisplay) {
    gDisplay->detach();
    delete gDisplay;
    gDisplay = 0;
  }
  if (gHost) {
    gHost->audio.shutdown();
    delete gHost;
    gHost = 0;
  }
}

/**
 * 串口数据回调接口
 */
static void onProtocolDataUpdate(const SProtocolData &data) {
}

/* ==================== 投屏接收端（DLNA DMR）：自动启动 ====================
 * ⚠️ 以前只有 QA 命令 `dlna on` 能起服务，**界面上没有任何入口** ——
 *    结果就是"手机根本搜不到"（服务压根没启动，1900/8200 都没有监听）。
 *    现在改成**开机自动启动**，让它像一台真正的投屏接收端：
 *      · App 起来后延迟 2s 起（别跟首屏渲染抢资源）；失败每 5s 重试（端口被占/内存紧）
 *      · ⚠️ WiFi 常常晚于本函数才拿到 IP → 一旦 IP 变成真实地址就**补发一次 SSDP alive**
 *        （否则通告里的 LOCATION 写成 127.0.0.1，控制器拿了也连不上 —— 老代码踩过）
 *      · `dlna off` 仍可手动关掉，且关掉后不再自动重开（gDlnaUserOff）
 */
bool gDlnaUserOff = false;

void ensureDlna() {
#ifdef FUN_BUILD
  static int phase = 0;  // 0 = 还没排延迟  1 = 等待/重试中  2 = 已启动
  static long nextTryMs = 0;
  static bool ipAnnounced = false;
  if (gDlnaUserOff) return;
  long now = pgNowMs();

  if (phase == 0) {  // 首次进入：排一个 2s 的延迟，别跟首屏抢
    nextTryMs = now + 2000;
    phase = 1;
    return;
  }
  if (phase == 1) {
    if (now < nextTryMs) return;
    if (dlnaStart(8200)) {
      phase = 2;
      LOGD("PocketGame: 投屏接收端自动启动成功（DLNA DMR，端口 8200）");
    } else {
      nextTryMs = now + 5000;  // 端口被占/内存紧 → 5s 后再试
    }
    return;
  }

  /* 已启动：等 wlan0 真正拿到 IP 后补发 alive 通告（只在菜单页改提示条） */
  if (!ipAnnounced) {
    const char *ip = gDlna->localIp();
    if (ip && strcmp(ip, "127.0.0.1") != 0) {
      ipAnnounced = true;
      gDlna->requestAlive();
      LOGD("PocketGame: DLNA alive 通告已补发（本机 IP=%s）", ip);
      if (mTextTipPtr) {
        // 让用户知道"手机投屏里该选什么名字"
        mTextTipPtr->setText("投屏接收端已就绪：手机投屏里选 PocketGame-DMR");
      }
    }
  }
#endif
}

/* ==================== 网络校时（NTP）：https 的前提 ====================
 * 本板**没有 RTC** —— 开机时系统时间恒为 `1970-01-01`，而 TLS 校验会检查
 * "当前时间是否在证书有效期内"，1970 年时**任何证书都是"尚未生效"**，
 * 于是 https 握手必然失败。所以只要网络通了就把时间拉正。
 *
 * 时机与节流都在 pg::TimeSync 内部（异步、失败 10s 重试、成功一次即止），
 * 这里只负责"网络可用了"这个触发条件，以及打一条用户可见的状态日志。
 */
void ensureTimeSync() {
#ifdef FUN_BUILD
  static bool done = false;
  if (done) return;
  /* 触发条件：WiFi 已连上（拿到 IP 才可能通 NTP；没网时发起也是白费一次超时） */
  if (!pg::WifiService::connected()) return;
  pg::TimeSync::ensureStarted();
  if (pg::TimeSync::synced()) {
    done = true;  // 成功后就别再进这个函数了（主循环 15ms 一次，别反复判断）
    return;
  }
  /* 状态变化才打日志（避免每帧刷屏） */
  static int lastState = -1;
  if (pg::TimeSync::state() != lastState) {
    lastState = pg::TimeSync::state();
    LOGD("PocketGame: 校时状态 -> %s（服务器 %s）", pg::TimeSync::stateText(),
         pg::TimeSync::lastServer());
  }
#endif
}

/**
 * 定时器触发函数：这里是游戏主循环
 * 返回值 true = 继续运行
 */
static bool onUI_Timer(int id) {
  switch (id) {
    case TIMER_LOOP: {
      const long long loopT0 = pgNowUs();   // QA `bench` 用：整个回调的起点
      /* 让 platform 层知道"现在是不是在画布游戏里" —— 右滑返回的归属判定要用
       * （游戏里由主界面退游戏；独立 ftu 页由全局导航栏 goBack()，见 PgSwipe.h）。
       * 每帧同步而不是在各处 gMode 赋值点分别调：改动路径多，漏一处就出鬼。 */
      pg::setGameMode(gMode == MODE_GAME);
      /* ★★ 2026-09-16「检讨 UI 覆盖」修正 ①：游戏名交给导航栏显示。
       *   游戏页的 TextGameTitle（y=0..52）**整块被 navibar 盖死**，而它显示的正是
       *   "当前游戏名"（gHud.title，由 syncHudText 写），navibar 原来只会显示
       *   Activity 级的"口袋游戏机" ⇒ 用户在游戏里看不到自己在玩哪个游戏。
       *   页面里没有第二个位置能放它（HUD 下面就是 540 高的画布，下移就压画布）
       *   ⇒ 直接把名字写到状态栏上（pg::setNaviTitle），退出游戏传 0 恢复。
       *   ⚠️ 与 setGameMode 同一处每帧同步：赋值点有十几处，逐点加必漏
       *      （这条纪律是 PgSwipe.h 上那条注释教的）。 */
      /* ⚠️ 只在**主界面真在前台**时才写这两个全局标志（2026-09-16 真机验收抓到）：
       *   主界面的定时器在别的 Activity 起来之后**可能还跑一拍**（框架切页的尾拍），
       *   那一拍会把别的页面刚设好的导航栏标题**清掉** —— 实测现象是"时钟套件切到
       *   世界时钟子页，标题闪一下就回到'时钟套件'"。加一道前台判断最省且最稳：
       *   不是前台就完全不碰标志（navibar 侧还有 Activity 所有者校验兜底）。 */
      const char *actNow = EASYUICONTEXT->currentAppName();
      if (actNow && strcmp(actNow, "mainActivity") == 0) {
        if (gMode == MODE_GAME && gHud.title[0]) pg::setNaviTitle(gHud.title);
        else pg::setNaviTitle(0);
      }
      /* ★★ 用**实测经过时间**当 dt，而不是固定的 LOOP_MS（2026-09-15 修）。
       *
       * 为什么必须改：定时器周期只是"最小间隔" —— 回调里要跑渲染（软渲染全屏重绘），
       * 一旦某帧超过 33ms，这一 tick 就被拉长，但传下去的 dt 仍是 33
       * ⇒ **游戏内时间比真实时间慢**。血案：打地鼠的连击赞赏设 900ms，
       * 实测在屏幕上停了 3.6 秒（帧率掉到 ~7fps 时差 4 倍）；
       * 同理"30 秒倒计时"在这种情况会跑 2 分钟。
       *
       * 钳到 200ms：卡顿/暂停恢复时别让游戏"一步跳太大"（会瞬移、会漏判定）。 */
      const long long nowLoopMs = pgNowMs();
      static long long sPrevLoopMs = 0;
      int loopDt = sPrevLoopMs ? (int)(nowLoopMs - sPrevLoopMs) : LOOP_MS;
      if (loopDt < 1) loopDt = 1;
      if (loopDt > 200) loopDt = 200;
      sPrevLoopMs = nowLoopMs;
      if (sFpsProbe) {
        ++sFpsCnt;
        if (!sFpsT0) sFpsT0 = nowLoopMs;
        if (nowLoopMs - sFpsT0 >= 2000) {
          long long span = nowLoopMs - sFpsT0;
          LOGD("PocketGame: fps = %d 次/秒（%lldms 内 %d 次循环，dt 实测）",
               (int)(sFpsCnt * 1000 / (span ? span : 1)), span, sFpsCnt);
          sFpsProbe = 0;
        }
      }
      gKeyRouter.tickLongPress();  // 长按达标即返回（不等按键抬起）
      /* 屏幕翻转（挂绳倒挂，2026-09-18）：把"机器人页在不在显示"报给 PgFlip，
       * 由它按"用户意愿 + 有没有环境页在显示"算出该不该倒。
       * ★ 每拍重新算 = 自愈：漏报一拍、掉帧、从屏保/独立页跳回来都不会把屏幕留在错的角度；
       *   状态没变时它是个空操作（只有变化才真的下发旋转）。见 platform/PgFlip.h。 */
      pg::flipSetPageOn(pg::FLIP_PAGE_PET, gMode == MODE_GAME && gGame && gGame->isFlipPage());
      /* 闹钟提醒页是"必须正着读"的浮层 ⇒ 它显示时抑制翻转（否则挂着看时钟时闹钟是倒的，实测过） */
      pg::flipSetSuppress(mWinAlarmRingPtr && mWinAlarmRingPtr->isWndShow());
      pg::flipTick();
      pollAutoStart();  // QA 钩子：/tmp/pg_autostart
      tickAlarmRing();  // 闹钟到点 → 弹提醒页（**任何模式**都判：游戏/工具页都要弹）
      /* IPTV 的相位机/刷新已归 iptvLogic.cc 的定时器（独立 ftu，主循环不再驱动）。 */
      /* IPTV 换台续播（读 /tmp/pg_iptv_resume 再 startGame）已删除：
       * 2026-09-14 起换台走 `stopForSwitch()` **不再重启进程**（见 docs/iptv.md §11.3），
       * 没有任何代码会写这个文件了；IPTV 自己也已独立成 iptv.ftu。 */
      pg::Battery::tick();  // 电量/充电状态（内部 500ms 节流，照参考工程）
                            // ★ 2026-09-16：下面的 tickBattery() 已删 —— 电量搬到全局导航栏
                            //   （mBattFillPtr 等控件已从 ui/main.html 移除，见本文件顶部说明）
      tickVideoCover();     // 投屏页缓冲底图（按"是否已出画面"收敛）
      tickVideoSelfHeal();  // 视频链路卡死自愈（兜底：黑屏 4s 才发现脏通道）
      tickStopReset();      // 主动路径：一按停止就复位（下次投屏立刻可用）
      tickScreensaverPolicy();  // 工作界面（游戏/工具/视频）禁止屏保
      tickPcmIdle();    // 声卡空闲自动让出（别的进程要用声卡时才需要它让）
      /* ★ 全局状态栏（全局音量 OSD 的宿主）**改为按需显隐**（2026-09-15 定稿）：
       *   以前这里每秒收敛 `showStatusBar()` 保证它常显 —— 但状态栏根窗口是
       *   **480x800 整屏 + topmost**，常显 = 悬浮在所有页面之上，而实测
       *   `touchable=false` / `setTouchPass(true)` 都挡不住触摸
       *   ⇒ **整机所有页面点不动**（只有页面级回调照常，症状"报点正常、控件无响应"）。
       *   现在：显隐完全交给状态栏自己（src/logic/statusbar.cc 的 showOsd/hideOsd，
       *   走 `EASYUICONTEXT->showStatusBar()/hideStatusBar()`）—— 只有音量 OSD 在的
       *   1.6 秒里它才存在，其余时间窗口栈里没有它，触摸自然全通。
       *   ⚠️ 不要再加"常显/收敛"逻辑回来（会把这个坑重新引入）。 */
      /* 音量 OSD 的自动隐藏已归状态栏的定时器（src/logic/statusbarLogic.cc） */
      // 投屏媒体文件的延迟清理（见 sCastCleanupAtMs 的说明）
      if (sCastCleanupAtMs && pgNowMs() >= sCastCleanupAtMs) {
        sCastCleanupAtMs = 0;
        if (gDlna) gDlna->cleanupMedia();
      }
      tickDlna();       // DLNA 动作队列（网络线程 → UI 线程）
      ensureDlna();     // 投屏接收端：自动启动 + IP 到手后补发 alive 通告
      ensureTimeSync(); // 网络校时：https 证书校验的前提（本板无 RTC，开机是 1970）
      /* 在线流（streamshow）：亮/收投屏页 + 播完自动收尾。
       * 顺序：命令先起流（开流要 2~4s），投屏页在主循环下一拍亮出来，绝对赶得上第一帧。 */
      if (gStreamWantPage == 1) {
        gStreamWantPage = 0;
        showCastPageForStream("在线流播放中…");
        gStreamPageShown = 1;
      } else if (gStreamWantPage == 2) {
        gStreamWantPage = 0;
        hideCastPageForStream();
        gStreamPageShown = 0;
      } else if (gStreamPageShown && !pg::StreamPlayer::running() &&
                 !pg::H264Player::running()) {
        /* ⚠️ 必须也判 H264Player：zk_h264_player 那条路**不用** StreamPlayer，
         * 只判 StreamPlayer 会在 h264play 刚起来时误判成"流已结束"，
         * 把投屏页（=视频层唯一能透出的透明窗口）收掉 → 画面全黑、帧全堆在解码器里。 */
        gStreamPageShown = 0;
        hideCastPageForStream();
        LOGD("PocketGame: 在线流播放结束，已收掉投屏页");
        if (gStreamCasting) {  // 顺带把 DLNA 状态回报给控制器
          gStreamCasting = 0;
          if (gDlna) gDlna->setTransportState("STOPPED");
        }
      }

      // 音效顺序自检：每 1.2s 播一声，日志会打"第 N 声"
      if (gSfxSeqIdx > 0) {
        gSfxSeqWaitMs -= loopDt;
        if (gSfxSeqWaitMs <= 0) {
          if (gSfxSeqIdx < SFX_COUNT) {
            LOGD("PocketGame: sfxseq 第 %d/%d 声 = %s", gSfxSeqIdx, SFX_COUNT - 1,
                 kSfxNames[gSfxSeqIdx]);
            if (gHost) gHost->playSfx(gSfxSeqIdx);
            ++gSfxSeqIdx;
            gSfxSeqWaitMs = 1200;
          } else {
            gSfxSeqIdx = 0;
            LOGD("PocketGame: sfxseq 播放完毕（共 %d 声）", SFX_COUNT - 1);
          }
        }
      }

      // 主菜单：把"待刷新列表"放到事件循环里执行（见 gMenuDirty 的说明）
      if (gMode == MODE_MENU && gMenuDirty > 0) {
        if (--gMenuDirty == 0) {
          LOGD("PocketGame: 主菜单列表明细刷新（延迟执行）");
          refreshList();
        }
        break;
      }

      // 工具类应用：只推进状态 + 同步原生控件（不涉及画布/最高分/暂停窗）
      if (gMode == MODE_TOOL) {
        if (!gGame) break;
        gGame->update(loopDt);
        syncToolUi();
        break;
      }

      if (gMode != MODE_GAME || !gGame || !gDisplay) break;
      {
        /*
         * 暂停时冻结画面（不再每帧 update/render），但要**补画一帧**：
         * 游戏自己"点一下暂停"时状态是在触摸回调里切过去的，冻结前最后一帧
         * 还是正常的游戏画面 —— 若直接 break，"已暂停"遮罩永远画不出来，
         * 用户看到的是静止的游戏，没有任何暂停提示。只在切入暂停后的第一帧
         * 画一次（gPauseFrameDrawn），之后保持冻结不耗 CPU。
         */
        static bool gPauseFrameDrawn = false;
        if (gGame->state() == GSTATE_PAUSED) {
          if (!gPauseFrameDrawn && gDisplay->attached()) {
            gGame->render(gDisplay->canvas());
            gDisplay->present();
            gPauseFrameDrawn = true;
          }
          break;
        }
        gPauseFrameDrawn = false;  // 离开暂停（继续/重开），下次暂停再补画
      }

      /* ★★ 静止画面只画一帧（见 Game::stillFrame 的说明）：READY/OVER 的整屏
       * 半透明遮罩每帧重画要 8ms，而它内容不变 ⇒ 进静止态画一次就够。
       * 判据用"仍然静止 + 状态编号"：状态一变（1→2）就重画；回到原状态时
       * 因为中间经过 lastStill=-1 也会重画。 */
      {
        static int lastStill = -1;
        if (gGame->stillFrame()) {
          const int st = (int)gGame->state() + 1;
          if (st == lastStill) break;   // 同一静止态已经画过，跳过本帧 render/present
          lastStill = st;
        } else {
          lastStill = -1;
        }
      }

      long long bt0 = pgNowUs();
      gGame->update(loopDt);
      long long bt1 = pgNowUs();

      if (gGame->justGameOver()) {
        gGame->clearGameOverFlag();
        onGameOver();
      }
      syncHud(false);

      if (gDisplay->attached()) {
        long long bt2 = pgNowUs();
        gGame->render(gDisplay->canvas());
        drawSoftButtons(gDisplay->canvas());
        long long bt3 = pgNowUs();
        gDisplay->present();
        long long bt4 = pgNowUs();
        if (sBenchProbe) {
          sBenchUpd += bt1 - bt0;
          sBenchRnd += bt3 - bt2;
          sBenchPre += bt4 - bt3;
          sBenchLoop += pgNowUs() - loopT0;   // 整个回调（含下面所有巡检）
          ++sBenchFrames;
          if (!sBenchT0) sBenchT0 = bt4;
          if (bt4 - sBenchT0 >= 2000000LL) {
            const int n = sBenchFrames ? sBenchFrames : 1;
            LOGD("PocketGame: bench %d 帧  每帧均值 update=%lldus render=%lldus "
                 "present=%lldus 整个回调=%lldus（帧间隔≈%lldus）",
                 sBenchFrames, sBenchUpd / n, sBenchRnd / n, sBenchPre / n,
                 sBenchLoop / n, (bt4 - sBenchT0) / n);
            sBenchProbe = 0;
          }
        }
      }
      break;
    }
    default:
      break;
  }
  return true;
}

/**
 * 全局触摸事件：游戏模式下把画布区域的触摸转给当前游戏
 * 返回值 true = 事件在此被拦截
 */
static bool onmainActivityTouchEvent(const MotionEvent &ev) {
  LOGD("PocketGame touch: action=%d x=%d y=%d mode=%d", (int)ev.mActionStatus,
       ev.mX, ev.mY, (int)gMode);
  /* 响铃时触摸先停铃（不分模式：主界面/工具页/游戏都要能停） */
  if (pg::Alarm::instance().ringing()) {
    pg::Alarm::instance().stopRing();
    LOGD("PocketGame: 触摸停铃（真实触摸 %d,%d）", ev.mX, ev.mY);
    return true;
  }
  /* ★ 右滑返回（2026-09-16 用户需求：「应用界面内模仿 android 手机右滑…返回主页」）
   *
   * 为什么游戏要单独处理：画布游戏是**主界面内的整屏 window**，不是独立 Activity
   * ⇒ 全局导航栏那条 `EASYUICONTEXT->goBack()` 管不到它（navibar.cc 处理独立 ftu 页）。
   * 手势判定复用 `pg::swipe*`（阈值同一份：水平 ≥90px、纵向 ≤70px、时长 ≤900ms）。
   *
   * ⚠️ 只在**游戏模式**下生效：菜单态主界面是窗口栈底，滑动退应用不是用户要的语义。
   * ⚠️ 若以后有游戏抱怨"横向拖拽被吃掉"，先把 PgSwipe.cpp 的 kMinDx 提到 120，
   *    不要在这里给某个游戏开小灶（手感判据要全局一致，见 RULES-DETAIL §4）。 */
  if (gMode == MODE_GAME) {
    if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN) {
      pg::swipeDown(ev.mX, ev.mY);
    } else if (ev.mActionStatus == MotionEvent::E_ACTION_MOVE) {
      pg::swipeMove(ev.mX, ev.mY);
    } else if (ev.mActionStatus == MotionEvent::E_ACTION_UP) {
      /* ⚠️ 读 `swipeTriggered()` 而不是 `swipeUp()` 的返回值：全局导航栏也收同一笔触摸，
       *    它可能**先**判完（在游戏里它的 goBack() 无效，但状态已被判过）⇒ 用返回值会漏。
       *    详见 platform/PgSwipe.h 的说明。 */
      pg::swipeUp(ev.mX, ev.mY);
      if (pg::swipeTriggered()) {
        pg::swipeClearTriggered();
        LOGD("PocketGame: 右滑返回 -> 退出当前游戏回列表");
        exitGameToMenu();
        return true;
      }
    } else if (ev.mActionStatus == MotionEvent::E_ACTION_CANCEL) {
      pg::swipeCancel();
    }
  }

  if (gMode != MODE_GAME || !gGame) return false;

  int act = PG_TOUCH_MOVE;
  if (ev.mActionStatus == MotionEvent::E_ACTION_DOWN) {
    act = PG_TOUCH_DOWN;
  } else if (ev.mActionStatus == MotionEvent::E_ACTION_UP ||
             ev.mActionStatus == MotionEvent::E_ACTION_CANCEL) {
    act = PG_TOUCH_UP;
  }
  /*
   * 统一走 dispatchCanvasTouch —— 它内部有"软按钮优先命中"和"暂停弹窗显示中不拦触摸"
   * 两条判定。真实触摸与 QA 注入（tap/down/move/up）因此走**同一条路径**，
   * 不会出现"注入能点软按钮、手指点不到"这类分裂。
   */
  return dispatchCanvasTouch(act, ev.mX - CANVAS_X, ev.mY - CANVAS_Y);
}

/* ---------------- 游戏列表：三回调 ---------------- */

static int getListItemCount_ListGames(const ZKListView *pListView) {
  (void)pListView;
  return categoryCount(gCategory);
}

/* 应用元信息对象按"槽位"缓存（列表滚动时逐行回调，不能有耗时操作）
 *
 * ⚠️⚠️ **上限必须 ≥ 最大 slot，不要写死**（2026-09-16 血案）：
 *   这里原来是 `static Game *cache[32]` + `slot >= 32 就 return 0`，而「系统设置」是
 *   **slot 32**（本轮之前新增、按规矩追加在表尾）⇒ 它的卡片拿不到元信息 ⇒
 *   `onObtainListItemData` 走"空位"分支 ⇒ **系统分类里那一张是空白格**，
 *   用户报「系统设置图标没有找到，进不去系统设置界面」。
 *   ★ 同一类坑在本工程反复出现（`ScoreStore::MAX_GAMES` 当初也是"正好贴上限"）⇒
 *     判据：**凡是按 slot 下标的数组/上限，都要用"应用总数/共享常量"，不许写字面量**。
 *   ★ 而且**不许静默返回**：超限要打 WARN（工程纪律"静默失败必须消灭"），
 *     否则下次再加应用还是"卡片凭空消失"这种查半天的问题。
 *   新增应用后请复查：`grep -n "kMaxAppSlots\|maxSlot" src/logic/mainLogic.cc`。 */
static const int kMaxAppSlots = ScoreStore::MAX_GAMES;   // 40；与存档上限同一个真值
static Game *metaForSlot(int slot) {
  static Game *cache[kMaxAppSlots] = {0};
  /* ★★ 2026-09-24 修：上界必须用 **kMaxAppSlots**，不能用 `appCount()`。
   *   为什么（用户报"程序入口不见了"）：`appCount()` 是**表里行数**（34），
   *   而 `slot` 是**稳定存档索引**（智能家居 = 37）—— 删过应用后 slot 会留空洞
   *   （现在 33/34/35/36 是空的），最大值 37 > 行数 34 ⇒ 用 appCount() 当上界，
   *   智能家居这一格被判成"空位" ⇒ **启动器上整格空白**（不报错、只留日志一句都没有，
   *   只能靠肉眼发现）。
   *   取表同理：必须**按 slot 查**（appBySlot），不能拿 slot 当下标（appEntry 会夹紧，
   *   夹对了是巧合，下一个应用就会指错行）。 */
  if (slot < 0 || slot >= kMaxAppSlots) {
    LOGW("PocketGame: slot %d 超出元信息缓存上限 %d（新增应用后要同步提上限）",
         slot, kMaxAppSlots);
    return 0;
  }
  if (!cache[slot]) {
    const GameEntry *e = appBySlot(slot);
    if (!e) {
      LOGW("PocketGame: slot %d 在 kAppTable 里没有对应行（slot 写重/漏了？）", slot);
      return 0;
    }
    cache[slot] = e->create();
  }
  return cache[slot];
}

static void obtainListItemData_ListGames(ZKListView *pListView,
                                         ZKListView::ZKListItem *pListItem,
                                         int index) {
  (void)pListView;
  if (!pListItem) return;

  /* 图标仍是"21 个同位置重叠的控件、按 slot 显示一个"，见 syncRowIcon()。
   * ★ 2026-09-14 第二版：主界面改成**桌面网格**，item 模板里只剩 图标 + 名称 + 最高分
   *   （副标题/卡片底/「最高分」标签都去掉了）⇒ 这里同步删掉对 desc / tag 的引用。
   *   ⚠️ 控件从 ui/main.html 删掉后，生成的 mainActivity.h 里**不会再有**对应的
   *      ID_MAIN_SubGameDesc / ID_MAIN_SubGameBestTag —— 留着这两行会直接编译失败。 */
  ZKListView::ZKListSubItem *name = pListItem->findSubItemByID(ID_MAIN_SubGameName);
  ZKListView::ZKListSubItem *best = pListItem->findSubItemByID(ID_MAIN_SubGameBest);

  int slot = categorySlot(gCategory, index);
  Game *meta = metaForSlot(slot);
  if (!meta) {
    // 该格没有内容：整格留空（桌面网格下就是"空位"，不画任何东西）
    // ★ 也**不能给行设底色**：桌面要的是纯黑背景 + 悬浮图标，一行实色会露出直角。
    syncRowIcon(pListItem, -1);  // 空位：21 个图标全部隐藏
    if (name) name->setText("");
    if (best) best->setText("");
    return;
  }

  (void)meta->theme();  // theme 不再用于图标（颜色烘在图标图里）；保留便于将来"按主题染色"

  /* 图标：按 slot 显示对应那一张（21 个控件同位置重叠，细节见 syncRowIcon 的注释）。
   * 颜色和符号都在图片里，所以这里不需要再 paintSub / setText。 */
  syncRowIcon(pListItem, slot);
  if (name) {
    name->setText(meta->title());
    name->setTextColor(0xFFF2F2F7);   // iOS T1
  }
  /* 最高分：只有游戏显示（工具/系统页整格隐藏）。桌面上它是"次要信息"，
   * 用小字灰色压在名称下方，不要用橙色强调 —— 否则 21 个图标里一片橙点。 */
  const GameEntry *slotEnt = appBySlot(slot);   // ★ 按 slot 查（slot 不是表下标）
  const bool isGame = (slotEnt && slotEnt->category == APP_GAME);
  if (best) {
    if (isGame) {
      best->setText(gHost ? gHost->highScore(slot) : 0);
      best->setTextColor(0xFF9A9AA0);   // iOS T2
      best->setVisible(true);
    } else {
      best->setText("");
      best->setVisible(false);
    }
  }
}

static void onListItemClick_ListGames(ZKListView *pListView, int index, int id) {
  (void)pListView;
  LOGD("PocketGame: list click index=%d subId=%d", index, id);
  int slot = categorySlot(gCategory, index);
  if (slot < 0) return;
  startGame(slot);
}

/* ---------------- 工具页按钮（WinClock 的 8 个 + WinCalc 的 20 个） ----------------
 *
 * 原生按钮的点击最终还是回到应用的 onUiButton(i)，与画布应用走 onTouch 是同一个位置。
 */

/* 投屏页的"停止播放"按钮 */
static bool onButtonClick_BtnCastStop(ZKButton *pButton) {
  (void)pButton;
  stopCast();
  return false;
}

/* 每个按钮一个显式函数：框架的代码生成器是按函数名做文本匹配的，
 * 用宏拼接出来的名字它认不出来，会又给你追加一份空骨架（然后编译报重定义）。
 * 所以这里宁可写 28 遍。点击最终都回到当前应用的 onUiButton(i)。 */
/* ---- 响铃提醒页：停止 / 贪睡 ---- */
static bool onButtonClick_BtnRingStop(ZKButton *pButton) {
  (void)pButton;
  pg::Alarm::instance().stopRing();   // 停铃 + 自动把声卡还给音效系统
  if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
  LOGD("PocketGame: 提醒页「停止」");
  return true;
}
static bool onButtonClick_BtnRingSnooze(ZKButton *pButton) {
  (void)pButton;
  pg::Alarm::instance().snooze(5);
  if (mWinAlarmRingPtr) mWinAlarmRingPtr->hideWnd();
  LOGD("PocketGame: 提醒页「贪睡 5 分钟」");
  return true;
}

/* ==================== 时钟套件：按钮回调结束 ==================== */

static bool onButtonClick_BtnU0(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(0);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU1(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(1);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU2(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(2);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU3(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(3);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU4(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(4);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU5(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(5);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU6(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(6);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnU7(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(7);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK0(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(0);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK1(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(1);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK2(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(2);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK3(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(3);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK4(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(4);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK5(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(5);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK6(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(6);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK7(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(7);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK8(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(8);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK9(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(9);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK10(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(10);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK11(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(11);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK12(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(12);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK13(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(13);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK14(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(14);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK15(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(15);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK16(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(16);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK17(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(17);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK18(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(18);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}
static bool onButtonClick_BtnK19(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(19);
    resetToolUiCache();
    syncToolUi();
  }
  return true;
}

/* ---------------- 主界面：分类 tab ---------------- */

/* 分段控件三个 tab。
 * ★ 2026-09-15：**每个 tab 都打一行日志** —— 排查"主界面点不动"时要能区分
 *   "触摸没到控件"（无日志）和"到了但逻辑没生效"（有日志）。 */
static bool onButtonClick_BtnTab0(ZKButton *pButton) {
  (void)pButton;
  LOGD("PocketGame: tab click -> 游戏");
  switchCategory(APP_GAME);
  return true;
}
static bool onButtonClick_BtnTab1(ZKButton *pButton) {
  (void)pButton;
  LOGD("PocketGame: tab click -> 工具");
  switchCategory(APP_TOOL);
  return true;
}
static bool onButtonClick_BtnTab2(ZKButton *pButton) {
  (void)pButton;
  LOGD("PocketGame: tab click -> 系统");
  switchCategory(APP_SYSTEM);
  return true;
}

/* ---------------- 主界面：音效开关（2026-09-16 已搬走） ----------------
 * 原来这里有一颗「音效 开/关」按钮 + syncSoundButton() 同步文案/底色。
 * 用户要求把音效开关收进系统设置 ⇒ 按钮与这套同步代码**整体删除**，
 * 现在改音效只有一条路：系统设置页（src/logic/settings.cc）。
 * 判据：`grep -n mBtnSoundPtr src/` 应为 0 条（控件也一起删了）。 */

/*
 * 列表卡片的 21 个圆形图标按钮：**纯展示用**，不需要点击响应。
 *
 * ⚠️ 为什么必须把这 19 个空回调显式写出来：
 *   fun build 会给"源码里找不到回调的按钮"自动注入空骨架，而且**每次 build 追加一次**
 *   → 第二次编译就 redefinition。（这也是为什么这里不用宏/循环生成名字。）
 *   ⚠️ 同理：它们必须是 div.btn —— 试过用 div.text，**textview 不绘制 backgroundPic**，
 *      结果 21 个图标全都不显示。
 * 返回 false 表示不消费事件（列表项的点击仍由 listview 处理）。
 */
static bool onButtonClick_Icon0(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon1(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon2(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon3(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon4(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon5(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon6(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon7(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon8(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon9(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon10(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon11(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon12(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon13(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon14(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon15(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon16(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon17(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon18(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon19(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon20(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon21(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon22(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon23(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon24(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon25(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon26(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon27(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon28(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon29(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon30(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon31(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon32(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon33(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon34(ZKButton *p) { (void)p; return false; }
/* 2026-09-20 新增（slot 35/36）。图标点击本身不做事 —— 卡片命中统一走
 * onListItemClick_ListGames()，这里只是"四处同步"里的第 ③ 处桩函数。 */
static bool onButtonClick_Icon35(ZKButton *p) { (void)p; return false; }
static bool onButtonClick_Icon36(ZKButton *p) { (void)p; return false; }
/* 2026-09-23 新增（slot 37 智能家居）。同样是"四处同步"里的第 ③ 处桩函数。 */
static bool onButtonClick_Icon37(ZKButton *p) { (void)p; return false; }


/* ---------------- 电池图标：**已搬到全局导航栏**（2026-09-16） ----------------
 * 用户需求原文：「状态栏显示不完整，直接把电池，页面 title 都放到状态栏这样子改动最小」。
 * ⇒ ui/main.html 里的 TextTitle / BattShell / BattFill / BattBolt 控件已删除，
 *   本文件原来的 syncBatteryIcon() 与 tickBattery() 也随之删掉（控件指针不存在 ⇒ 编译不过）。
 * 现在电量由 `src/logic/navibar.cc` 的 refreshBatt() 画（素材复用同一套 batt_*.png，
 * 低电红 / 充电琥珀 / 正常蓝 的取色规则一字未改）。
 * ⚠️ 要改电量的显示 ⇒ 改 navibar.cc，**别在这里加回控件/函数**（会与状态栏重复）。 */

/* ---------------- 暂停弹窗按钮 ---------------- */

static bool onButtonClick_BtnResume(ZKButton *pButton) {
  (void)pButton;
  setPaused(false);
  if (gHost) gHost->playSfx(SFX_CLICK);
  return false;
}

static bool onButtonClick_BtnRestart(ZKButton *pButton) {
  (void)pButton;
  if (gGame) {
    if (gGame->state() == GSTATE_PAUSED && mWinPausePtr) mWinPausePtr->hideWnd();
    gPauseWinShowing = false;
    gGame->reset();
    gGame->setState(GSTATE_RUNNING);
    gHud.reset();
    syncHud(true);
    if (gHost) gHost->playSfx(SFX_CLICK);
  }
  return false;
}

static bool onButtonClick_BtnBackList(ZKButton *pButton) {
  (void)pButton;
  exitGameToMenu();
  if (gHost) gHost->playSfx(SFX_CLICK);
  return false;
}




static bool onButtonClick_BtnW0(ZKButton* pButton) {
  LOGD_TRACE("BtnW0 click");
  return false;
}

static bool onButtonClick_BtnW1(ZKButton* pButton) {
  LOGD_TRACE("BtnW1 click");
  return false;
}

static bool onButtonClick_BtnW2(ZKButton* pButton) {
  LOGD_TRACE("BtnW2 click");
  return false;
}


/* 注：原 `onProgressChanged_BarVol`（音量进度条拖动回调）已随 WinVolume 面板一起删除 ——
 * 音量 OSD 现在归全局状态栏（src/logic/statusbarLogic.cc），主界面不再有这个控件，
 * fun 也就不会再生成该回调的声明（删掉实现才不会链接错）。 */



static void onVideoViewPlayerMessageListener_Caster(ZKVideoView *pVideoView, int msg) {
	switch (msg) {
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_STARTED:
		break;
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_COMPLETED:
		break;
	case ZKVideoView::E_MSGTYPE_VIDEO_PLAY_ERROR:
		break;
	}
}
/*
 * 反应计时的唯一按钮（整个下半屏那块"反应区"）。
 *
 * ⚠️ 这个回调原本是 **fun build 自动注入的空骨架**（只打一条 LOGD_TRACE 就 return false），
 *    后果是"按钮点了完全没反应、状态机永远停在待机"。排查时要注意：
 *    注入的桩**不是编译错误**，只是什么都不做，很容易被误判成"UI 没刷新"。
 *    ⇒ 凡是新加的原生按钮，都要在 mainLogic.cc 里**显式实现**它的回调。
 */
static bool onButtonClick_BtnReact(ZKButton *pButton) {
  (void)pButton;
  if (gGame && gMode == MODE_TOOL) {
    gGame->onUiButton(0);   // GameReact 只用一个按钮（index 0）
    resetToolUiCache();     // 清文本缓存 → 下一帧全量重写（否则状态变了界面不跟）
    syncToolUi();           // 立即同步一次，不必等下一帧
  }
  return true;
}static bool onButtonClick_BarSegTab(ZKButton* pButton) {
  LOGD_TRACE("BarSegTab click");
  return false;
}


