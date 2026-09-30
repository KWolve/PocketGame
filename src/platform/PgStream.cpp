/*
 * PgStream.cpp - 在线流播放第一阶段：ffmpeg 解封装 + 设备硬解出帧
 * 见 PgStream.h 的说明与 docs/online-media.md。
 *
 * 步骤：
 *   avformat_open_input(url)              // 走 http/https（含 TLS/证书，见 PgFf）
 *   find video stream + 建 bsf            // h264_mp4toannexb：MP4 的 AVCC → Annex-B
 *   AW_MPI_SYS_Init / VDEC_CreateChn      // 按流的 WxH 建硬解通道
 *   loop: av_read_frame → bsf → AW_MPI_VDEC_SendStream → AW_MPI_VDEC_GetImage
 *        → 记日志（头几帧）→ ReleaseImage
 */
#include "platform/PgStream.h"
#include "platform/PgGrab.h"   // 抓帧：两条解码路共用（见 PgGrab.h 的说明）

#ifdef FUN_BUILD

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>   // av_display_rotation_get：读容器里的旋转元数据
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

// MPP 头（已拍平进 src/dependencies/include，见 docs/online-media.md）
extern "C" {
#include "mpi_sys.h"
#include "mpi_vdec.h"
#include "mpi_vo.h"       // AW_MPI_VO_*：把解码帧送进 disp 视频层
#include "mm_comm_vo.h"   // VO_VIDEO_LAYER_ATTR_S
}

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "platform/PgAudio.h"   // 在线流音频灌进那条常开 PCM 流
#include "platform/PgFf.h"
#include "platform/PgH264.h"         // ★ 硬件播放器（zk_h264_player）：视频那条路
#include "platform/PgHls.h"          // ★ HLS 环形缓冲直喂（自定义 AVIO，见 openInput）
#include "platform/PgViz.h"          // 双声道 VU 电平（在重采样前按声道取 RMS，见 frameStereoRms）
#include "platform/PgMem.h"          // 起流前清页缓存（drop_caches，见 PgMem.h）
#include "platform/PgVideoLayer.h"   // 收尾时释放 disp 层（见 threadMain 收尾处的说明）
#include "utils/Log.h"

namespace pg {

namespace {

/* ==================== 双声道 RMS（给 VU 指针表；2026-09-19 新增） ====================
 * 为什么在这里取：**灌给喇叭的那条 PCM 是下混后的单声道**（22050/单声道，见 PgAudio.h），
 *   拿它做不出左右差异 ⇒ 必须在"重采样/下混之前"从**解码帧**上按声道取。
 * 为什么不算 FFT：VU 表只要"每声道响度"，RMS 与采样率无关 ⇒ 既不用重采样也不用 FFT，
 *   成本只有几千次乘加（帧长 ~1024、步进 2 再减半）。
 * 支持的解码格式：planar / interleaved 的 float / s16 / s32（ffmpeg 软解常见就这几种）；
 *   其它格式（如 u8）**不静默**——计数并让上层能看出来。声道数 >2 时只取前两路。 */
void frameStereoRms(const AVFrame *f, float *rl, float *rr) {
  *rl = *rr = 0.0f;
  if (!f || !f->data[0] || f->nb_samples <= 0) return;
  const int ch = f->channels > 0 ? f->channels : 1;
  const int n = f->nb_samples;
  const int step = 2;                     // 每 2 个样本取 1 个（够稳了）
  double acc[2] = {0.0, 0.0};
  int cnt = 0;
  const AVSampleFormat fmt = (AVSampleFormat)f->format;
  if (fmt == AV_SAMPLE_FMT_FLTP || fmt == AV_SAMPLE_FMT_S16P || fmt == AV_SAMPLE_FMT_S32P) {
    for (int c = 0; c < 2; ++c) {
      const uint8_t *p = (c < ch) ? f->extended_data[c] : f->extended_data[0];
      if (!p) continue;
      for (int i = 0; i < n; i += step) {
        double v;
        if (fmt == AV_SAMPLE_FMT_FLTP) v = ((const float *)p)[i];
        else if (fmt == AV_SAMPLE_FMT_S16P) v = ((const int16_t *)p)[i] / 32768.0;
        else v = ((const int32_t *)p)[i] / 2147483648.0;
        acc[c] += v * v;
        if (c == 0) ++cnt;
      }
    }
  } else if (fmt == AV_SAMPLE_FMT_FLT || fmt == AV_SAMPLE_FMT_S16 || fmt == AV_SAMPLE_FMT_S32) {
    for (int i = 0; i < n; i += step) {
      for (int c = 0; c < 2; ++c) {
        const int idx = i * ch + ((c < ch) ? c : 0);
        double v;
        if (fmt == AV_SAMPLE_FMT_FLT) v = ((const float *)f->data[0])[idx];
        else if (fmt == AV_SAMPLE_FMT_S16) v = ((const int16_t *)f->data[0])[idx] / 32768.0;
        else v = ((const int32_t *)f->data[0])[idx] / 2147483648.0;
        acc[c] += v * v;
      }
      ++cnt;
    }
  } else {
    static int warned = 0;
    if (warned < 3) {                     // 不静默：打一次就够（别刷屏）
      LOGW("PgStream: VU 电平不支持样本格式 %d（本次不安表）", (int)fmt);
      ++warned;
    }
    return;
  }
  if (!cnt) return;
  *rl = (float)sqrt(acc[0] / cnt);
  *rr = (float)sqrt(acc[1] / cnt);
  if (ch == 1) *rr = *rl;                 // 单声道源：两只表同步摆（诚实：本来就是同一路）
}

/* ==================== MPP 的 dlopen 绑定 ==================== */
struct MppApi {
  void *hMedia, *hVo;
  ERRORTYPE (*SYS_SetConf)(const MPP_SYS_CONF_S *);
  ERRORTYPE (*SYS_Init)();
  ERRORTYPE (*SYS_Exit)();
  /* 内存分配（MMZ）：旋转后要自己造一块 YUV 缓冲送 VO，用它的物理地址/虚拟地址。
   * ⚠️ 必需用 MPP 自己的分配器 —— 普通 malloc 的内存 DE(显示引擎) 访问不到
   * （实测直接拿解码帧地址没问题、但自己 malloc 的缓冲 DE 会报 invalid address）。 */
  ERRORTYPE (*SYS_MmzAlloc_Cached)(unsigned int *pPhyAddr, void **ppVirtAddr, unsigned int uLen);
  ERRORTYPE (*SYS_MmzFree)(unsigned int uPhyAddr, void *pVirtAddr);
  ERRORTYPE (*VDEC_CreateChn)(VDEC_CHN, const VDEC_CHN_ATTR_S *);
  ERRORTYPE (*VDEC_DestroyChn)(VDEC_CHN);
  ERRORTYPE (*VDEC_StartRecvStream)(VDEC_CHN);
  ERRORTYPE (*VDEC_StopRecvStream)(VDEC_CHN);
  ERRORTYPE (*VDEC_SendStream)(VDEC_CHN, const VDEC_STREAM_S *, int);
  ERRORTYPE (*VDEC_GetImage)(VDEC_CHN, VIDEO_FRAME_INFO_S *, int);
  ERRORTYPE (*VDEC_ReleaseImage)(VDEC_CHN, VIDEO_FRAME_INFO_S *);
  /* 收尾用（可选符号：没有就跳过）：
   *  SetStreamEof —— 告诉解码器"输入到此为止"，它的内部线程才会把残留解完并退出。
   *    ⚠️ 中途停播时不发这一步，`VDEC_DestroyChn` 会**永久卡住**（实测）。
   *  ResetChn —— 兜底：把通道复位（清掉内部残留帧/状态）。 */
  ERRORTYPE (*VDEC_SetStreamEof)(VDEC_CHN, int);
  ERRORTYPE (*VDEC_ResetChn)(VDEC_CHN);
  /* Query：查通道内部还剩多少码流/图 —— 收尾时用它判断"排空没排空"（优雅停流的前提）。 */
  ERRORTYPE (*VDEC_Query)(VDEC_CHN, VDEC_CHN_STAT_S *);
  /* 旋转（可选符号）：设备库里 `AW_MPI_VDEC_SetRotate` 内部会按格式选 vdeclib 或 g2d，
   * 是"解码器直接吐转正的帧"，不额外占内存。建通道时也可以用 attr.mInitRotation。 */
  ERRORTYPE (*VDEC_SetRotate)(VDEC_CHN, ROTATE_E);
  ERRORTYPE (*VDEC_GetRotate)(VDEC_CHN, ROTATE_E *);

  /* ---- VO（/lib/eyesee-mpp/libmpp_vo.so）：把解码帧送进 disp 视频层 ----
   * 显示分层（见知识库 v85x/videoview-transparent-window.md）：
   *     UI 层（disp ch2，z=16，最顶）  ← easyui 的 fb0
   *     disp 视频层（disp ch0）        ← 我们/VO 输出的画面（在下）
   * UI 层的 videoview 区域不填不透明内容 → 该处透明 → 下层视频层从那儿透出。
   * 实测框架播放时的视频层参数：`ch[0] lyr[0] z[0] fmt[77] crop[0,0,1280,720] frame[0,0,480,700]`
   *   ⇒ 视频区就是投屏页 `Caster` 控件的位置 (0,0,480,700)。 */
  ERRORTYPE (*VO_Enable)(VO_DEV);
  ERRORTYPE (*VO_Disable)(VO_DEV);
  ERRORTYPE (*VO_EnableVideoLayer)(VO_LAYER);
  ERRORTYPE (*VO_DisableVideoLayer)(VO_LAYER);
  ERRORTYPE (*VO_SetVideoLayerAttr)(VO_LAYER, const VO_VIDEO_LAYER_ATTR_S *);
  ERRORTYPE (*VO_OpenVideoLayer)(VO_LAYER);
  ERRORTYPE (*VO_CloseVideoLayer)(VO_LAYER);
  ERRORTYPE (*VO_CreateChn)(VO_LAYER, VO_CHN);
  ERRORTYPE (*VO_DestroyChn)(VO_LAYER, VO_CHN);
  ERRORTYPE (*VO_StartChn)(VO_LAYER, VO_CHN);
  ERRORTYPE (*VO_StopChn)(VO_LAYER, VO_CHN);
  ERRORTYPE (*VO_SetFrameDisplayRegion)(VO_LAYER, VO_CHN, const RECT_S *);
  ERRORTYPE (*VO_SendFrame)(VO_LAYER, VO_CHN, VIDEO_FRAME_INFO_S *, int);
  bool ok, voOk;
  MppApi()
      : hMedia(0), hVo(0), SYS_SetConf(0), SYS_Init(0), SYS_Exit(0), VDEC_CreateChn(0),
        VDEC_DestroyChn(0), VDEC_StartRecvStream(0), VDEC_StopRecvStream(0),
        VDEC_SendStream(0), VDEC_GetImage(0), VDEC_ReleaseImage(0),
        VO_Enable(0), VO_Disable(0), VO_EnableVideoLayer(0), VO_DisableVideoLayer(0),
        VO_SetVideoLayerAttr(0), VO_OpenVideoLayer(0), VO_CloseVideoLayer(0),
        VO_CreateChn(0), VO_DestroyChn(0), VO_StartChn(0), VO_StopChn(0),
        VO_SetFrameDisplayRegion(0), VO_SendFrame(0), ok(false), voOk(false) {}
};

void *pgDl(const char *const *names, int n) {
  for (int i = 0; i < n; ++i) {
    void *h = dlopen(names[i], RTLD_NOW);
    if (h) return h;
  }
  return 0;
}

MppApi &mpp() {
  static MppApi a;
  static bool tried = false;
  if (tried) return a;
  tried = true;
  /* ⚠️ 只 dlopen 设备上已有的库，**不链接**：同进程里 easyui→libzkmedia 已加载一份
   *    MPP，再静态链一份会变成两套实例抢同一个内核驱动。 */
  static const char *kMedia[] = {"/lib/eyesee-mpp/libmedia_mpp.so", "libmedia_mpp.so"};
  static const char *kVo[] = {"/lib/eyesee-mpp/libmpp_vo.so", "libmpp_vo.so"};
  a.hMedia = pgDl(kMedia, 2);
  a.hVo = pgDl(kVo, 2);
  if (!a.hMedia) {
    LOGW("PgStream: 打不开 libmedia_mpp.so（硬解不可用）");
    return a;
  }
#define PGL(field, sym)                                    \
  do {                                                     \
    *(void **)(&a.field) = dlsym(a.hMedia, sym);            \
    if (!a.field) { LOGW("PgStream: 缺 %s", sym); return a; } \
  } while (0)
  PGL(SYS_SetConf, "AW_MPI_SYS_SetConf");
  PGL(SYS_Init, "AW_MPI_SYS_Init");
  PGL(SYS_Exit, "AW_MPI_SYS_Exit");
  PGL(VDEC_CreateChn, "AW_MPI_VDEC_CreateChn");
  PGL(VDEC_DestroyChn, "AW_MPI_VDEC_DestroyChn");
  PGL(VDEC_StartRecvStream, "AW_MPI_VDEC_StartRecvStream");
  PGL(VDEC_StopRecvStream, "AW_MPI_VDEC_StopRecvStream");
  PGL(VDEC_SendStream, "AW_MPI_VDEC_SendStream");
  PGL(VDEC_GetImage, "AW_MPI_VDEC_GetImage");
  PGL(VDEC_ReleaseImage, "AW_MPI_VDEC_ReleaseImage");
#undef PGL
  /* 这两个是**可选**符号（用 dlsym 直接取，缺了不影响硬解，只是收尾少一层保险） */
  a.VDEC_SetStreamEof = (ERRORTYPE(*)(VDEC_CHN, int))dlsym(a.hMedia, "AW_MPI_VDEC_SetStreamEof");
  a.VDEC_ResetChn = (ERRORTYPE(*)(VDEC_CHN))dlsym(a.hMedia, "AW_MPI_VDEC_ResetChn");
  a.VDEC_Query = (ERRORTYPE(*)(VDEC_CHN, VDEC_CHN_STAT_S *))dlsym(a.hMedia, "AW_MPI_VDEC_Query");
  if (!a.VDEC_SetStreamEof) LOGW("PgStream: 没有 AW_MPI_VDEC_SetStreamEof（收尾少了 EOS 这一步）");
  /* 旋转：可选符号（V85X 的 libmedia_mpp 里有；缺了就是"本平台不支持旋转"） */
  a.VDEC_SetRotate = (ERRORTYPE(*)(VDEC_CHN, ROTATE_E))dlsym(a.hMedia, "AW_MPI_VDEC_SetRotate");
  a.VDEC_GetRotate = (ERRORTYPE(*)(VDEC_CHN, ROTATE_E *))dlsym(a.hMedia, "AW_MPI_VDEC_GetRotate");
  /* MMZ 分配（**可选**：缺了就只能放弃软件旋转，不能因为缺它把整套硬解禁用掉） */
  a.SYS_MmzAlloc_Cached =
      (ERRORTYPE(*)(unsigned int *, void **, unsigned int))dlsym(a.hMedia,
                                                                "AW_MPI_SYS_MmzAlloc_Cached");
  a.SYS_MmzFree = (ERRORTYPE(*)(unsigned int, void *))dlsym(a.hMedia, "AW_MPI_SYS_MmzFree");
  LOGD("PgStream: 硬件旋转能力=%s MMZ 分配=%s",
       a.VDEC_SetRotate ? "有（AW_MPI_VDEC_SetRotate）" : "无",
       a.SYS_MmzAlloc_Cached ? "有" : "无");
  /* VO 符号从 libmpp_vo.so 取；缺了只是"看不到画面"，不该让整个硬解不可用 */
  if (a.hVo) {
#define PGLV(field, sym)                                              \
  do {                                                                \
    *(void **)(&a.field) = dlsym(a.hVo, sym);                         \
    if (!a.field) LOGW("PgStream: libmpp_vo 缺 %s（画面无法上屏）", sym); \
  } while (0)
    PGLV(VO_Enable, "AW_MPI_VO_Enable");
    PGLV(VO_Disable, "AW_MPI_VO_Disable");
    PGLV(VO_EnableVideoLayer, "AW_MPI_VO_EnableVideoLayer");
    PGLV(VO_DisableVideoLayer, "AW_MPI_VO_DisableVideoLayer");
    PGLV(VO_SetVideoLayerAttr, "AW_MPI_VO_SetVideoLayerAttr");
    PGLV(VO_OpenVideoLayer, "AW_MPI_VO_OpenVideoLayer");
    PGLV(VO_CloseVideoLayer, "AW_MPI_VO_CloseVideoLayer");
    PGLV(VO_CreateChn, "AW_MPI_VO_CreateChn");
    PGLV(VO_DestroyChn, "AW_MPI_VO_DestroyChn");
    PGLV(VO_StartChn, "AW_MPI_VO_StartChn");
    PGLV(VO_StopChn, "AW_MPI_VO_StopChn");
    PGLV(VO_SetFrameDisplayRegion, "AW_MPI_VO_SetFrameDisplayRegion");
    PGLV(VO_SendFrame, "AW_MPI_VO_SendFrame");
#undef PGLV
    a.voOk = a.VO_Enable && a.VO_EnableVideoLayer && a.VO_SetVideoLayerAttr &&
             a.VO_OpenVideoLayer && a.VO_CreateChn && a.VO_StartChn && a.VO_SendFrame;
  } else {
    LOGW("PgStream: 打不开 libmpp_vo.so（画面无法上屏）");
  }
  a.ok = true;
  LOGD("PgStream: MPP 已绑定（dlopen 设备自带的 libmedia_mpp；VO 能力=%s）",
       a.voOk ? "有" : "无");
  return a;
}

/* ==================== 画面旋转（顺时针，度）====================
 * 为什么需要：本机 480x800 竖屏，而内容常常是"横着拍的"；尤其**手机竖拍的 MP4**
 * 会把像素存成横的、只在容器里写一个旋转标志（displaymatrix），播放器不认这个标志
 * 画面就是**侧躺**的（这就是"画面方向不对"最常见的原因）。
 *
 * 做法：把角度交给 **VDEC 硬件旋转**（`AW_MPI_VDEC_SetRotate`，建通道时也能用
 * `attr.mInitRotation`）——解码器直接输出转正后的帧，不额外占内存、不额外占 CPU。
 * 参考 V851ExtendedScreen 项目的 `zk_h264_player_set_rot(disp_rot_e)`（同一套语义）。
 *
 * 取值：-1 = auto（跟随容器元数据）；否则 0/90/180/270（顺时针）。
 * 手动值一旦设定就**优先于**元数据，适用于之后每次播放（进程内有效）。 */
volatile int sRotManual = -1;
volatile int sRotSource = -1;   // 最近一次开流从容器读到的角度（-1 = 没有/没读到）
volatile int sRotApplied = -1;  // 最近一次真正下发给解码器的角度
const char *rotName(int deg) {
  if (deg < 0) return "未指定";
  switch ((((deg % 360) + 360) % 360)) {
    case 90: return "90°";
    case 180: return "180°";
    case 270: return "270°";
    default: return "0°";
  }
}
/* 本次该用哪个角度：手动值 > 容器元数据 > **竖屏默认 90°**。
 *
 * ★ 为什么兜底不是 0（2026-09-14 用户报"又没有旋转 90 了"）：
 *   本机是 480x800 **竖屏**设备，而视频内容（IPTV 直播、DLNA 在线流、本地文件）
 *   绝大多数是横屏。横屏源**不带旋转元数据**（实测 `源元数据=-1`），按 0° 直显就是
 *   "画面躺着"，必须转 90° 才能看。原来兜底是 0 ⇒ 只有用户手动 `streamrot 90`
 *   **并且成功落盘**才转；存档一旦被改写（实测发生过：读到一份没有 rot 行的旧存档
 *   → `rot_` 保持默认 -1 → 紧接着 `save()` 把 /data 里原来的 `rot=90` 覆盖掉），
 *   旋转就"又没了"，而且现象是"设置里明明设过、日志却显示 auto"。
 *   ⇒ 现在无元数据时按 90°，**不再依赖存档**。要固定不转用 `streamrot 0`。
 *
 * ⚠️ 只影响"我们自己解码"的那条链路（IPTV / DLNA 在线流 / h264 直推）。
 *    DLNA 播**本地文件**走的是框架 ZKVideoView，auto 时仍交由播放器自己处理
 *    （见 mainLogic.cc 的 cast 段）—— 不被这里改到，避免和它的内建行为叠加转两遍。 */
int desiredRotation() {
  if (sRotManual >= 0) return sRotManual;
  if (sRotSource >= 0) return sRotSource;
  return 90;
}

/* ==================== 线程状态 ==================== */
volatile int sRunning = 0;
/* 收尾（teardown）开始时刻，给 running() 的看门狗用：正常收尾是毫秒级，
 * 超过 8s 就认为卡在某个驱动调用里（实测遇到过一次：停播时硬解输出队列是满的，
 * VDEC_DestroyChn 等它的内部线程 → 永久卡住 → 之后所有播放都"已有任务在跑"）。 */
volatile long long sProgressMs = 0;   // 任务最近一次"有进展"的时刻（起于线程启动，各阶段刷新）
volatile long long sCleanupFromMs = 0;  // 兼容旧名：收尾开始时刻（= 也刷新 sProgressMs）
/* ---- 解码通道"常开"状态（见收尾处的长注释：中途停播后 DestroyChn 会卡死）---- */
int sSysInited = 0;     // MPP SYS 是否已初始化（只做一次，不再 Exit）
int sChnReady = 0;      // 是否已有可用通道
VDEC_CHN sChn = 0;      // 当前使用的解码通道号
/* 停止请求（中途停播）过的通道**不销毁**、改为换号 —— 见收尾处的长注释 */
VDEC_CHN sChnNext = 0;
int sDirtyChnCount = 0;
volatile int sAbortedMidway = 0;
volatile int sVoStarted = 0;   // VO 层/通道是否已建（常开，不再销毁）
volatile int sNeedsReset = 0;  // "本轮流是被手动停止的" → 调用方应复位应用（mainLogic 取走）
/* IPTV 换台标记：这次停止只是"要换一个源"，不是"用户真的要停" ——
 * 收尾时据此**不**置 sNeedsReset（否则每换一个台就把应用重启一遍）。 */
volatile int sSwitchMode = 0;
/* 自愈判据用（见 videoStalled()）：是否含视频轨 / 已解出多少帧 / 主循环何时开始 */
volatile int sHasVideo = 0;
volatile int sFramesOut = 0;
volatile long long sLoopStartMs = 0;
int sChnW = 0, sChnH = 0;
volatile int sStopReq = 0;    // **硬中止**（只在进程收尾等极端场景用；会让解码通道变脏）
/* **优雅停止请求**（DLNA Stop / QA streamstop 用这个）：不再读新包，发 EOS，
 * 把解码器放空后再退出主循环 —— 这样通道是"自然结束"的，能干净销毁。
 * 为什么必须这样：本平台**同时只有一个 VDEC 通道能真正工作**（实测第二个通道
 * CreateChn 返回 0x0 但不出画），而**中途硬中止会把通道搞成不可回收**
 * （DestroyChn 永久卡 / 复用不出画）→ 之后所有投屏都没画面，只能重启应用。 */
volatile int sFinishReq = 0;
char sUrl[768];
int sMaxSec = 0;
/* 画面输出（VO）：1 = 把解码帧送进 disp 视频层。
 * ⚠️ 光送帧是看不见的 —— UI 层在最顶且不透明，必须有"透明窗口"。
 *    投屏页（WinCast）里的 videoview `Caster` 就是那个窗口，位置 (0,0,480,700)。
 *    所以带画面的播放必须先把投屏页 show 出来（mainLogic 里的 streamshow 命令负责）。 */
volatile int sDisp = 0;
int sDispX = 0, sDispY = 0, sDispW = 480, sDispH = 700;
/* ★ 视频显示走哪条路（2026-09-14 用户点名改）
 *   1 = **硬件播放器**（`zk_h264_player`：解码 + 硬件旋转 + 缩放 + 裁剪一体）
 *   0 = MPP 老路（AW_MPI_VDEC + AW_MPI_VO + 应用层 CPU 软件旋转）
 * 为什么默认切成 1：老路的软件旋转（rotateInto）把 VDEC 输出的 **NV12 当 I420 三平面**
 * 拆开转 → 色度全乱（大片纯绿）；转出来的缓冲又是 256x448 而 VO 层的 frame 是 480x700
 * → 显示引擎按 480 宽读 256 宽的缓冲 → **画面横向重复**。用户看到的"错屏"就是这两个。
 * 硬件那套本来就有 `set_rot` / `set_pos` / `set_crop` 三个 API，不用白不用。
 * QA `streamhw <0|1>` 可现场切回老路做对照。 */
volatile int sHwVideo = 1;
int sHwSrcW = 0, sHwSrcH = 0;   // 本次硬件播放器起的源尺寸（改旋转时重算显示区要它）
volatile int sHwFed = 0;        // 硬件播放器成功喂进去的包数
/* [HW] 喂帧允许领先音频的时间（毫秒）。硬件播放器"解出来就上屏"，没有等音频的机制，
 * 这个值就是音画同步的松紧：太大 → 画面跑在声音前面；太小 → 解码器饿着（卡顿）。 */
const long long kHwLeadMs = 600;
/* QA `streamgrab [帧号]`：把第 N 帧的亮度(Y)平面降采样成 PGM 写到 /tmp。
 * 为什么需要它：**视频层不在 `/dev/fb0` 里**（fb0 只是 UI 层 disp ch2），
 * 截图/`flythings_device_screenshot` 都抓不到播出来的画面 ——
 * 于是"画面到底转没转、转到哪个方向"只能靠人眼。
 * 有了这个，帧数据可以被搬出来（PGM 是纯文本头 + 灰度像素，几十 KB）用脚本/工具查看。 */
volatile int sGrabAtFrame = -1;
volatile int sGrabStep = 4;   // 降采样步长（4 → 416x960 变 104x240）
/* VO 设备/层/通道编号：设备的视频层就是 disp ch[0]（UI 是 ch[2]），取 0 即可 */
const int kVoDev = 0, kVoLayer = 0, kVoChn = 0;
/* 送帧节流：源是 24fps 而解码能跑到 40+fps，不节流会"快放" */
const int kDispFps = 25;
/* 时长/进度（毫秒）：DLNA 控制器会轮询 GetPositionInfo/GetMediaInfo，需要它俩 */
volatile long long sDurMs = -1;
volatile long long sPosMs = 0;
/* 音频时钟：已推进声卡的帧数（22050Hz 单声道），减掉队列里还没播的 = 当前播放位置 */
volatile long long sAudioFrames = 0;
/* ★★ 本地文件**连续循环**（见 PgStream.h 的 setFileLoop）：读到 EOF 就 `av_seek_frame` 回起点
 * 继续喂，解码器与 disp 视频层**全程不重建** ⇒ 接缝处画面连续、没有黑屏。
 * 这两个标志由 UI 线程在起流前设/读，解封装线程读/写 —— 与 sStopReq 同类，用 volatile int。 */
volatile int sFileLoop = 0;    // 1 = 本轮起流要循环播这个文件
volatile int sFileLoops = 0;   // 已完成的循环次数（只增；上层验收判据）
/* ★ 接缝的两条观测（QA 判"接缝本身有没有拖延/丢帧"用，见 StreamPlayer::lastSeekMs/hwDrop）：
 *   sLastSeekMs = 最近一次循环 seek 发生在**起流以来的第几毫秒**。
 *     它的**差分就是"一轮的实际时长"**：若接缝处有停顿/黑屏，差分就会 > 片长（实测片长
 *     10s，稳定在 ~10000ms 才说明"播完立刻接上、没有空档"）。这比抓帧可靠得多
 *     （抓一帧 1~2s，而接缝只有几十 ms）。
 *   sHwDrop = 硬件播放器拒收（码流缓冲满）而丢掉的包数。它增长 = 解码侧丢帧，
 *     用来把"帧计数少了"与"接缝停顿"区分开（前者是丢包，后者是时间轴拖延）。 */
volatile long long sLastSeekMs = -1;
volatile int sHwDrop = 0;
/* 本次播放"音频不可用"（声卡被 pcmfree 释放 / 队列长时间收不进去）→ 时钟退回墙钟。
 * 没有这个标志的话，画面会一直等一个**永远不会前进**的音频时钟（表现：整片卡住）。 */
volatile int sAudioOff = 0;
const int kAudioOutRate = 22050;   // 必须与 PgAudio 的 STREAM_RATE 一致

/* 音频时钟（毫秒）。返回 -1 = 没有音频（调用方退回原来的帧率节流）。 */
long long audioClockMs(pg::DeviceAudio *au) {
  if (!au || sAudioOff) return -1;
  long long q = au->streamQueuedFrames();
  long long played = sAudioFrames - q;
  if (played < 0) played = 0;
  return played * 1000 / kAudioOutRate;
}

/* ---- VO 视频层的开关 ----
 * 参数依据（实测框架播放时）：视频层就是 disp `ch[0]`，`frame[0,0,480,700]`，
 * 与投屏页 `Caster`（UI 层里的"透明窗口"）完全重合。
 *
 * ⚠️ 三条实测教训：
 *   ① **每一轮流都要完整重申一遍 setup**（Enable→EnableVideoLayer→SetAttr→Open→
 *      CreateChn→SetFrameDisplayRegion→StartChn）：因为投屏页隐藏后框架会把 VO 的
 *      状态动掉，第二轮再走时 `EnableVideoLayer` 会回 `0xa00f8041`(VO_DEV_NOT_ENABLE)、
 *      `SetVideoLayerAttr`/`OpenVideoLayer` 回 `0xa00f8045`(VO_NOT_ENABLE)。
 *   ② **这些错误码不要中断流程**：实测它们出现时 `CreateChn/SetRegion/StartChn`
 *      仍返回 0x0，且 disp 里能看到我们的层被 enable 并拿到新 addr（画面是好的）。
 *      所以这里只记日志（带错误名），不提前 return。
 *   ③ 收尾**只 StopChn/DestroyChn**，不 Disable/Close 层与 dev —— 本平台 VO 的常态
 *      就是"常开"（KB `v85x/display-layer-debug.md`：easyui 的 zkmedia 播放器退出也不释放）。 */
const char *voErrName(int e) {
  switch (e & 0xff) {
    case 0x40: return "VO_DEV_NOT_CONFIG";
    case 0x41: return "VO_DEV_NOT_ENABLE";
    case 0x42: return "VO_DEV_HAS_ENABLED";
    case 0x43: return "VO_DEV_HAS_BINDED";
    case 0x45: return "VO_NOT_ENABLE";
    case 0x46: return "VO_NOT_DISABLE";
    case 0x48: return "VO_CHN_NOT_DISABLE";
    case 0x49: return "VO_CHN_NOT_ENABLE";
    default: return "";
  }
}

bool voStart(int x, int y, int w, int h) {
  MppApi &m = mpp();
  if (!m.voOk) return false;
  ERRORTYPE er = m.VO_Enable(kVoDev);
  LOGD("PgStream: AW_MPI_VO_Enable(%d) -> 0x%x %s", kVoDev, er, voErrName(er));
  er = m.VO_EnableVideoLayer(kVoLayer);
  LOGD("PgStream: AW_MPI_VO_EnableVideoLayer(%d) -> 0x%x %s", kVoLayer, er, voErrName(er));
  VO_VIDEO_LAYER_ATTR_S la;
  memset(&la, 0, sizeof(la));
  la.stDispRect.X = x;
  la.stDispRect.Y = y;
  la.stDispRect.Width = (unsigned)w;
  la.stDispRect.Height = (unsigned)h;
  la.stImageSize.Width = (unsigned)w;
  la.stImageSize.Height = (unsigned)h;
  la.mDispFrmRt = (unsigned)kDispFps;
  la.enPixFormat = MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
  la.bDoubleFrame = 0;
  la.bClusterMode = 0;
  er = m.VO_SetVideoLayerAttr(kVoLayer, &la);
  LOGD("PgStream: AW_MPI_VO_SetVideoLayerAttr(%d, %d,%d %dx%d) -> 0x%x %s", kVoLayer, x, y, w,
       h, er, voErrName(er));
  er = m.VO_OpenVideoLayer(kVoLayer);
  LOGD("PgStream: AW_MPI_VO_OpenVideoLayer -> 0x%x %s", er, voErrName(er));
  er = m.VO_CreateChn(kVoLayer, kVoChn);
  LOGD("PgStream: AW_MPI_VO_CreateChn -> 0x%x %s", er, voErrName(er));
  if (m.VO_SetFrameDisplayRegion) {
    RECT_S r;
    r.X = x;
    r.Y = y;
    r.Width = (unsigned)w;
    r.Height = (unsigned)h;
    er = m.VO_SetFrameDisplayRegion(kVoLayer, kVoChn, &r);
    LOGD("PgStream: AW_MPI_VO_SetFrameDisplayRegion -> 0x%x %s", er, voErrName(er));
  }
  er = m.VO_StartChn(kVoLayer, kVoChn);
  LOGD("PgStream: AW_MPI_VO_StartChn -> 0x%x %s", er, voErrName(er));
  return true;
}

/* 只停通道；层与 VO dev 留着（见上面③） */
void voStop() {
  MppApi &m = mpp();
  if (!m.voOk) return;
  ERRORTYPE er = m.VO_StopChn(kVoLayer, kVoChn);
  LOGD("PgStream: AW_MPI_VO_StopChn -> 0x%x %s", er, voErrName(er));
  er = m.VO_DestroyChn(kVoLayer, kVoChn);
  LOGD("PgStream: AW_MPI_VO_DestroyChn -> 0x%x %s", er, voErrName(er));
}


/* 把一个解码帧的亮度平面(Y)存成 PGM（见 PgGrab.h / grabWritePlane 的说明）。 */
void dumpFrameY(const VIDEO_FRAME_INFO_S &f) {
  const unsigned char *y = (const unsigned char *)f.VFrame.mpVirAddr[0];
  LOGD("PgStream: 抓帧元数据 源=%dx%d stride=%u fmt=%d virY=%p phyY=0x%x", (int)f.VFrame.mWidth,
       (int)f.VFrame.mHeight, f.VFrame.mStride[0], (int)f.VFrame.mPixelFormat,
       f.VFrame.mpVirAddr[0], f.VFrame.mPhyAddr[0]);
  grabWritePlane(y, (int)f.VFrame.mWidth, (int)f.VFrame.mHeight, f.VFrame.mStride[0], "vo");
}

/* ==================== 旋转输出缓冲（自己转，见 docs §十六）====================
 * ⚠️ 为什么不用 VDEC 的 mInitRotation / AW_MPI_VDEC_SetRotate：
 *   本板实测那条路会输出一块 **显示引擎(DE)访问不到的缓冲** —— 帧的宽高确实变了
 *   （960x400 → 416x960），但像素全是 0，同时内核疯狂刷
 *   `DE invalid address: 0x49b5a000 / L2 PageTable Invalid`，屏幕上是黑的。
 *   MPP 内部**一个错误都不报**（CreateChn/SendFrame 全 0x0），坑得很隐蔽。
 *   （VDEC_SetRotate 在"通道已存在"时还会返回 0xa0058009 且让复用通道彻底不出画。）
 *
 * ⇒ 改成**解码器照常出 0° 的帧，我们把它转正后再送显示**：
 *   · 目标缓冲必须是 **MMZ 内存**（`AW_MPI_SYS_MmzAlloc_Cached`）—— 普通 malloc 的
 *     内存 DE 同样访问不到；
 *   · 转动用 CPU 转置（16x16 分块，缓存友好）。本板帧约 368KB，25fps 下的转置开销
 *     可接受（单核 A7 上属于"跑得动"的量级）。
 */
unsigned int sRotPhy = 0;      // MMZ 物理地址（DE 要的是这个）
void *sRotVir = 0;             // MMZ 虚拟地址（CPU 写）
unsigned int sRotCap = 0;      // 缓冲大小（字节）
int sRotW = 0, sRotH = 0;      // 旋转后的内容尺寸（= 源的高 x 宽）
unsigned int sRotStrideY = 0, sRotStrideUV = 0;

void freeRotBuf() {
  MppApi &m = mpp();
  if (sRotVir && m.SYS_MmzFree) m.SYS_MmzFree(sRotPhy, sRotVir);
  sRotPhy = 0;
  sRotVir = 0;
  sRotCap = 0;
  sRotW = sRotH = 0;
}

/* 申请（或复用）旋转缓冲。srcW/srcH = **源帧**尺寸（旋转后宽高交换）。
 * 返回 false = 本次没法转（缺 MMZ 接口/分配失败）→ 调用方降级为"不转直接送"。 */
bool ensureRotBuf(int srcW, int srcH, int deg) {
  MppApi &m = mpp();
  if (!m.SYS_MmzAlloc_Cached || !m.SYS_MmzFree || srcW <= 0 || srcH <= 0) return false;
  /* 90/270 会交换宽高；180 不换 */
  int w = (deg == 180) ? srcW : srcH;
  int h = (deg == 180) ? srcH : srcW;
  unsigned sy = (unsigned)((w + 15) & ~15);            // 16 字节对齐（DE 要）
  /* ⚠️ 按 YUV420SP 理论大小（×3/2）分配**不够**：实测 DE 读到缓冲末尾之后还会再取一点
   * （内核报 `DE invalid address: <缓冲末尾>` / `L2 PageTable Invalid`，画面那块就没了）。
   * 直接给 **Y 的两倍**（多留 1/2 个 Y 平面当安全带），本板这点内存换显示稳定很值。 */
  unsigned need = sy * (unsigned)h * 2;
  if (sRotVir && sRotW == w && sRotH == h && sRotCap >= need) return true;
  freeRotBuf();
  unsigned int phy = 0;
  void *vir = 0;
  ERRORTYPE er = m.SYS_MmzAlloc_Cached(&phy, &vir, need);
  if (er != SUCCESS || !vir) {
    LOGW("PgStream: MMZ 分配旋转缓冲失败（%u 字节, 0x%x）→ 本次不旋转", need, er);
    return false;
  }
  sRotPhy = phy; sRotVir = vir; sRotCap = need;
  sRotW = w; sRotH = h; sRotStrideY = sy; sRotStrideUV = sy;
  LOGD("PgStream: 旋转缓冲就绪 %dx%d stride=%u 共 %uKB phy=0x%x vir=%p",
       w, h, sy, need / 1024, phy, vir);
  return true;
}

/* 按角度转一个平面（每元素 1 字节：Y 与 I420 的色度平面都是）。
 * sw/sh = 源平面宽高（元素数），sstride/dstride = 跨距（字节）。
 *  90°（顺时针）：dst(x, sh-1-y) = src(y, x)   → 目标尺寸 (sh, sw)，源的"顶"转到目标"右"
 *  270°（逆时针）：dst(sw-1-x, y) = src(y, x)  → 目标尺寸 (sh, sw)，源的"顶"转到目标"左"
 *  180°：dst(sw-1-x, sh-1-y) = src(y, x)       → 目标尺寸 (sw, sh)
 * 16x16 分块是为了缓存友好（转置本身是随机写，分块能显著减少 cache miss）。 */
void rotPlane(const unsigned char *sp, int sw, int sh, unsigned sstride, unsigned char *dp,
              int dstride, int deg) {
  const int B = 16;
  for (int y0 = 0; y0 < sh; y0 += B) {
    int ye = (y0 + B < sh) ? (y0 + B) : sh;
    for (int x0 = 0; x0 < sw; x0 += B) {
      int xe = (x0 + B < sw) ? (x0 + B) : sw;
      for (int y = y0; y < ye; ++y) {
        const unsigned char *srow = sp + (size_t)y * sstride;
        if (deg == 90) {
          for (int x = x0; x < xe; ++x) dp[(size_t)x * dstride + (sh - 1 - y)] = srow[x];
        } else if (deg == 270) {
          for (int x = x0; x < xe; ++x) dp[(size_t)(sw - 1 - x) * dstride + y] = srow[x];
        } else {   /* 180 */
          unsigned char *drow = dp + (size_t)(sh - 1 - y) * dstride;
          for (int x = x0; x < xe; ++x) drow[sw - 1 - x] = srow[x];
        }
      }
    }
  }
}

/* 把源帧按 deg（90/180/270）转到旋转缓冲，并填好输出帧结构。返回 false = 本次转不了
 * （源不是三平面 I420 / 指针缺失）→ 调用方应退回"送原帧"。
 *
 * ⚠️⚠️ **必须按解码器实际的平面布局来填**（这里踩过一个很隐蔽的坑）：
 *   本板 VDEC 输出的是 **YUV420P 三平面（I420）** —— 实测解码帧
 *   `phy[0x48f80000, 0x48fbc000, 0x48fcb000]`，U→V 的距离正好是 (W/2)*(H/2)。
 *   只填 Y+UV 两个平面、把第 3 个留 0 的话，DE 去读第 3 平面会拿到地址 0，内核报
 *   `DE invalid address: 0x0 / L1 PageTable Invalid`，画面上那块就是黑的。
 *   另外 **mStride[1]/[2] 报的值不可信**（报文里是 640，实际 U/V 的行宽是 W/2=320），
 *   所以色度的跨距按"半宽"自己算。
 *   ⇒ 两平面（NV12/NV21）布局这里**不转**（宁可不转也不转错）。 */
bool rotateInto(const VIDEO_FRAME_INFO_S &src, VIDEO_FRAME_INFO_S &out, int deg) {
  const unsigned char *sy = (const unsigned char *)src.VFrame.mpVirAddr[0];
  const unsigned char *su = (const unsigned char *)src.VFrame.mpVirAddr[1];
  const unsigned char *sv = (const unsigned char *)src.VFrame.mpVirAddr[2];
  int W = (int)src.VFrame.mWidth, H = (int)src.VFrame.mHeight;
  if (!sy || !su || !sv || W <= 0 || H <= 0) {
    LOGW("PgStream: 源帧不是三平面 I420（Y=%p U=%p V=%p）→ 本次不转", (const void *)sy,
         (const void *)su, (const void *)sv);
    return false;
  }
  unsigned sSY = src.VFrame.mStride[0];
  if (!sSY) sSY = (unsigned)W;
  const int UW = W / 2, UH = H / 2;
  unsigned sSU = (unsigned)UW;            // 色度跨距：按半宽（报文的 stride[1] 不可信）

  const int dW = (deg == 180) ? W : H;
  const int dH = (deg == 180) ? H : W;
  unsigned dSY = sRotStrideY, dSU = sRotStrideUV;
  unsigned char *dy = (unsigned char *)sRotVir;
  unsigned char *du = dy + (size_t)dSY * dH;
  unsigned char *dv = du + (size_t)dSU * (dH / 2);

  rotPlane(sy, W, H, sSY, dy, (int)dSY, deg);
  rotPlane(su, UW, UH, sSU, du, (int)dSU, deg);
  rotPlane(sv, UW, UH, sSU, dv, (int)dSU, deg);

  /* ⚠️ **先把解码帧的结构整个复制过来，再只覆盖"我们必须改"的字段**。
   * 踩过的坑：一开始用 memset(0) 从零构造，结果 DE 报 `DE invalid address: 0x0`
   * —— 说明结构里还有别的字段是显示引擎要用的，清掉就废。原样继承最稳。 */
  out.VFrame = src.VFrame;
  out.mId = 0;
  out.VFrame.mWidth = (unsigned)dW;
  out.VFrame.mHeight = (unsigned)dH;
  out.VFrame.mOffsetTop = 0;              /* 旋转后的帧没有 crop，偏移按整帧 */
  out.VFrame.mOffsetBottom = (short)dH;
  out.VFrame.mOffsetLeft = 0;
  out.VFrame.mOffsetRight = (short)dW;
  out.VFrame.mPhyAddr[0] = sRotPhy;
  out.VFrame.mpVirAddr[0] = dy;
  out.VFrame.mStride[0] = dSY;
  out.VFrame.mPhyAddr[1] = sRotPhy + dSY * (unsigned)dH;
  out.VFrame.mpVirAddr[1] = du;
  out.VFrame.mStride[1] = dSU;
  out.VFrame.mPhyAddr[2] = sRotPhy + dSY * (unsigned)dH + dSU * (unsigned)(dH / 2);
  out.VFrame.mpVirAddr[2] = dv;
  out.VFrame.mStride[2] = dSU;
  static int sLogOnce = 0;
  if (sLogOnce < 3) {
    ++sLogOnce;
    LOGD("PgStream: [旋转帧 %d°] phy[0x%x,0x%x,0x%x] stride[%u,%u,%u] %dx%d（源 %dx%d）", deg,
         out.VFrame.mPhyAddr[0], out.VFrame.mPhyAddr[1], out.VFrame.mPhyAddr[2],
         out.VFrame.mStride[0], out.VFrame.mStride[1], out.VFrame.mStride[2], dW, dH, W, H);
  }
  return true;
}

long long monoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 打开在线流（含 CA/超时/重试）—— 与 PgFf::probe 用同一套选项 */
/* ==================== HLS 直喂：ffmpeg 自定义 AVIO ====================
 * 2026-09-14 去掉"本地 HTTP 中继"（原来 PgHls 起 127.0.0.1:8199 把环形缓冲吐给 ffmpeg）。
 * 现在由这里挂一个读回调，字节直接从 PgHls 的环形缓冲取 —— 少一层本地 TCP、少一次拷贝。
 * ffmpeg 该做的照旧：mpegts 解封装、音视频分流、时间戳 …… 一个都没变。
 *
 * ⚠️ 只有一条流（工程本来就是单流播放），所以 pb 用静态变量存着，方便收尾时释放。 */
AVIOContext *s_hlsAvio = 0;

/* 读回调：PgHls::readBuffer 内部会阻塞等分片（最多 30s），所以这里**不需要忙等**。
 * 返回 0（流停/超时）时给 EOF —— 让 av_read_frame 报结束，上层循环随之退出。 */
int hlsReadCb(void *opaque, unsigned char *buf, int n) {
  (void)opaque;
  int r = pg::Hls::readBuffer(buf, n, 30000);
  {
    static int s_calls = 0;
    if (s_calls < 4 || (s_calls % 200) == 0) {
      LOGD("PgStream: AVIO 读回调第 %d 次 -> %d 字节%s", s_calls, r, r > 0 ? "" : "（EOF）");
    }
    ++s_calls;
  }
  return (r > 0) ? r : AVERROR_EOF;
}

/* 关闭输入：**自定义 AVIO 必须自己释放** —— ffmpeg 只释放它自己创建的 pb
 * （设了 AVFMT_FLAG_CUSTOM_IO 时它不会碰 pb，这是文档规定的行为）。 */
void closeInput(AVFormatContext **fmt) {
  if (s_hlsAvio) LOGD("PgStream: 关闭自定义 AVIO（HLS 直喂结束）");
  if (*fmt) avformat_close_input(fmt);
  if (s_hlsAvio) {
    /* ⚠️ avio_context_free 只释放 AVIOContext 本身，**缓冲区要自己 free** ——
     * 不还的话每次起播漏 64KB（本板内存紧，几个来回就很可观）。 */
    av_freep(&s_hlsAvio->buffer);
    avio_context_free(&s_hlsAvio);
  }
}

AVFormatContext *openInput(const char *url, int *outVIdx, int *outW, int *outH,
                           int *outPixFmtOk, int *outRotDeg) {
  *outVIdx = -1;
  *outW = *outH = 0;
  *outPixFmtOk = 0;
  *outRotDeg = -1;
  Ff::ensureReady();
  const char *ca = Ff::caPath();

  AVDictionary *opt = 0;
  av_dict_set(&opt, "rw_timeout", "30000000", 0);
  if (ca[0]) {
    av_dict_set(&opt, "ca_file", ca, 0);
    av_dict_set(&opt, "verify", "1", 0);
  } else {
    av_dict_set(&opt, "verify", "0", 0);
  }
  AVFormatContext *fmt = 0;
  long long t0 = monoMs();
  /* ★ 约定前缀（PgHls::start 写进 out 的地址）：数据来自 HLS 环形缓冲，走自定义 AVIO。
   *   别的地址（http/https/本地文件）完全走原来那条路，一个字没改。 */
  const bool isPgHls = (strncmp(url, "pg-hls://", 9) == 0);
  int r = 0;
  if (isPgHls) {
    unsigned char *iobuf = (unsigned char *)av_malloc(64 * 1024);
    if (!iobuf) {
      if (opt) av_dict_free(&opt);
      LOGW("PgStream: AVIO 缓冲分配失败");
      return 0;
    }
    s_hlsAvio = avio_alloc_context(iobuf, 64 * 1024, 0, 0, hlsReadCb, 0, 0);
    if (!s_hlsAvio) {
      av_free(iobuf);
      if (opt) av_dict_free(&opt);
      LOGW("PgStream: 自定义 AVIO 创建失败");
      return 0;
    }
    s_hlsAvio->seekable = 0;   // 直播流不可 seek（mpegts 也不需要）
    fmt = avformat_alloc_context();
    if (!fmt) {
      avio_context_free(&s_hlsAvio);
      if (opt) av_dict_free(&opt);
      return 0;
    }
    fmt->pb = s_hlsAvio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    LOGD("PgStream: 打开 HLS 环形缓冲（自定义 AVIO 直喂，无本地中继）");
    r = avformat_open_input(&fmt, 0, 0, &opt);   // 自定义 IO 时 url 传 NULL
  } else {
    r = avformat_open_input(&fmt, url, 0, &opt);
    if (r < 0) {
      /* ★ RTSP 传输回落：ffmpeg 默认 RTP/AVP(UDP)，但有的 IPC **只在 TCP 上回包**。
       *   探测侧（PgFf::probeFast）是「TCP 优先 → UDP」，这里反过来「UDP 优先 → TCP」，
       *   两条合起来覆盖"只认一种传输"的摄像头。缺了这步就会"探测报找到、播放却没画面"。 */
      const bool isRtsp = (strncmp(url, "rtsp://", 7) == 0);
      char eb0[128] = {0};
      if (isRtsp) {
        av_strerror(r, eb0, sizeof(eb0));
        LOGW("PgStream: RTSP 默认(UDP)打开失败：%s —— 回落 TCP 再试", eb0);
      }
      if (opt) av_dict_free(&opt);
      opt = 0;
      av_dict_set(&opt, "rw_timeout", "30000000", 0);
      if (isRtsp) av_dict_set(&opt, "rtsp_transport", "tcp", 0);
      if (ca[0]) { av_dict_set(&opt, "ca_file", ca, 0); av_dict_set(&opt, "verify", "1", 0); }
      else av_dict_set(&opt, "verify", "0", 0);
      r = avformat_open_input(&fmt, url, 0, &opt);
    }
  }
  if (opt) av_dict_free(&opt);
  if (r < 0) {
    char eb[128] = {0};
    av_strerror(r, eb, sizeof(eb));
    LOGW("PgStream: 打开失败：%s (%d)", eb, r);
    /* ⚠️ 失败时 fmt 已被 ffmpeg 回收，但**自定义 AVIO 还在我们手上**，要自己放掉
     *    （漏了就会在下次起播时泄漏 64KB + 一个 AVIOContext）。 */
    if (isPgHls && s_hlsAvio) avio_context_free(&s_hlsAvio);
    return 0;
  }
  /* ★★ 直播流「宽高未知」时收紧探测预算（2026-09-17 实测踩到，必须留注释）。
   *
   * 症状：**任何 RTSP 摄像头**起流都要卡 **16.5 秒**才出画面，日志三连是
   *   `PgFf[ffmpeg]: Stream #0: not enough frames to estimate rate; consider increasing probesize`
   *   `PgFf[ffmpeg]: Could not find codec parameters for stream 0 (Video: h264): unspecified size`
   *   `PgStream: 打开成功 用时 16530ms 视频[0] codec=h264 0x0`
   * 而摄像头页「等第一帧」超时是 **15s** ⇒ **真机上永远判 ERROR**（其实码流一直在解，只是开流太慢）。
   *
   * 根因：直播流的 H.264 **没有容器头**，宽高只能靠 (a) 软解探一帧 或 (b) 解析 SPS。
   *   · (a) 在本工程被**主动放弃**了 —— 为把 libzkgui.so 从 14.5MB 降到 2.6MB，
   *     `avcodec_find_decoder` 被打了桩（PgFf.cpp），H.264 一律返回 NULL；
   *   · (b) RTSP/RTP 解封装**不会自动设 need_parsing**（RTP 自己就产出完整 AU）。
   *   ⇒ `avformat_find_stream_info` 里 `has_codec_parameters()` 永远为假，
   *     它会一直读包直到 probesize / 分析时长上限才放弃 —— 那 16.5 秒就是这么烧掉的。
   *   ⚠️ 试过只给流设 `need_parsing = AVSTREAM_PARSE_FULL`：**无效**（实测仍是 16.5s /
   *      0x0）—— 该版本里 has_codec_parameters 看的是 `codecpar`，解析器把宽高写进
   *      `internal->avctx`，两边不会同步。所以只能从"少读点"下手。
   *
   * 修法：把探测预算从默认（probesize 5MB / analyze 5s）收紧到 512KB（实测约 3 秒）。
   *   代价是 **0**：宽高本来就要靠应用自己解 SPS
   *   （日志 `PgStream: [HW] 从 SPS 解析到源尺寸 1280x720`），ffmpeg 多读那几 MB
   *   一个有用信息都不会多。帧率也仍有兜底（SDP 的 `a=framerate` / 墙钟同步）。
   *
   * ⚠️ `width==0` 这个条件**不能省**：MP4/MOV 这类有容器头的源本来就有宽高，
   *   不该动它们的探测预算（正常路径保持原样，一个字不改）。
   * ⚠️ 实测依据见 PocketGame/docs/camera-radio-app.md §6（合成 RTSP 源台架 + 对照实验）。 */
  {
    bool dimsUnknown = false;
    for (unsigned si = 0; si < fmt->nb_streams; ++si) {
      AVStream *st = fmt->streams[si];
      if (!st || !st->codecpar) continue;
      if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
      if (st->codecpar->width > 0 && st->codecpar->height > 0) continue;   // 已有宽高：不碰
      if (st->codecpar->codec_id != AV_CODEC_ID_H264 &&
          st->codecpar->codec_id != AV_CODEC_ID_HEVC)
        continue;
      dimsUnknown = true;
      /* 顺手把解析器挂上：SPS 一解出来 avctx 就有宽高（某些版本/某些源能让
       * find_stream_info 提前收手），对我们只有好处。 */
      st->need_parsing = AVSTREAM_PARSE_FULL;
    }
    if (dimsUnknown) {
      fmt->probesize = 512 * 1024;
      fmt->max_analyze_duration = 3 * AV_TIME_BASE;
      LOGD("PgStream: 直播流宽高未知 → 收紧探测预算（probesize 512KB / analyze 3s）；"
           "宽高交给应用侧解 SPS，避免 find_stream_info 白读 16.5s");
    }
  }

  if (avformat_find_stream_info(fmt, 0) < 0) {
    LOGW("PgStream: 取流信息失败");
    closeInput(&fmt);
    return 0;
  }
  /* 诊断用：解析器到底建起来没有（"挂了 need_parsing 却没 parser" 是两种不同的病） */
  for (unsigned si = 0; si < fmt->nb_streams; ++si) {
    AVStream *st = fmt->streams[si];
    if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      LOGD("PgStream: 视频流 %u 分析后 %dx%d，parser=%s", si,
           st->codecpar->width, st->codecpar->height, st->parser ? "有" : "无");
  }
  int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
  if (vi < 0) {
    /* 没有视频流：**只要有音频流就继续**（纯音频源，如电台/音乐投屏）。
     * 硬解与显示通道会被整体跳过，只走音频链路。 */
    int ai2 = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if (ai2 < 0) {
      LOGW("PgStream: 既没有视频流也没有音频流");
      closeInput(&fmt);
      return 0;
    }
    LOGD("PgStream: 没有视频流（纯音频源），只走音频链路");
    *outVIdx = -1;
    *outW = 0;
    *outH = 0;
    return fmt;
  }
  AVCodecParameters *cp = fmt->streams[vi]->codecpar;
  *outVIdx = vi;
  *outW = cp->width;
  *outH = cp->height;

  /* ---- 容器里的旋转元数据（displaymatrix）----
   * 手机竖拍/竖屏录的 MP4 几乎都带这个：像素按横的存，再用一个 3x3 矩阵声明
   * "显示时要转多少度"。播放器不读它，画面就是**侧躺**的 —— 这是"方向不对"最常见的原因。
   * ffmpeg 用 `av_display_rotation_get` 把它换算成**逆时针**角度，这里转成
   * 我们统一使用的**顺时针** 0/90/180/270。 */
  int rotDeg = -1;
  for (unsigned i = 0; i < fmt->streams[vi]->nb_side_data; ++i) {
    AVPacketSideData *sd = &fmt->streams[vi]->side_data[i];
    if (sd->type != AV_PKT_DATA_DISPLAYMATRIX || !sd->data ||
        sd->size < 9 * (int)sizeof(int32_t))
      continue;
    double ccw = av_display_rotation_get((const int32_t *)sd->data);
    if (isnan(ccw)) continue;
    int cw = ((int)lround(-ccw) % 360 + 360) % 360;   // 逆时针 → 顺时针
    rotDeg = ((cw + 45) / 90 * 90) % 360;             // 归一化到 0/90/180/270
    LOGD("PgStream: 容器旋转元数据 = %d°（换算前 %d°）", rotDeg, cw);
    break;
  }
  *outRotDeg = rotDeg;

  LOGD("PgStream: 打开成功 用时 %lldms 视频[%d] codec=%s %dx%d%s", monoMs() - t0, vi,
       avcodec_get_name(cp->codec_id), cp->width, cp->height,
       rotDeg < 0 ? "" : "（源带旋转标志）");
  return fmt;
}

/* ==================== 内存守卫（本板最硬的一条限制） ====================
 * 背景（2026-09-13 实测，内核日志见 docs/oom-2026-09-13/dmesg-full.txt）：
 *   本板 **总内存 56MB**（`MemTotal 56632kB`），而且内核 **`COMPACTION is disabled`**
 *   （没开内存规整）→ 只要出现一次大额 ION 申请就会触发 oom-killer。
 *   实测 1280x720 H.264：`VE: real freq=400000000` 之后 **1 秒内 MemFree 23MB→2.5MB**，
 *   紧接着 `VDecChn0 invoked oom-killer ... ion_page_pool_alloc`，**应用被 SIGKILL**。
 *   ⇒ 症状极具欺骗性：应用"莫名重开一遍"（init 托管会立刻拉起），
 *     而**每次被 SIGKILL 都会漏掉一块不可回收的内存**（VO/ION 缓冲没人释放），
 *     于是内存一路走低（28MB→6MB→1MB），最后变成"起来就死"的**重启死循环**，只能重启机器。
 *   ⚠️ 厂商框架播放器（zkmedia）播同一个 720p 文件**也照样被打死**（pid 连续变），
 *      所以这不是我们参数没调好，是这块板子的物理限制。
 *
 * 策略：**先算再开**。开机就把预算算出来，超了就明确拒绝（打日志 + 不上屏），
 *      绝不让内核 OOM 杀进程 —— 被杀一次的代价（不可回收内存 + 重启循环）远大于"播不了"。
 * 经验系数 32B/像素（含解码帧池 + 码流缓冲 + 对齐余量）来自实测：
 *   640x360 (230k 像素) 实测占用 ~13MB；1280x720 (922k) 需要 >23MB 且仍然死。
 */
const long long kBytesPerPixel = 64;      // 经验值（640x360 实测约吃掉 15MB → 约 64B/像素）
const long long kMaxDecodePixels = 960L * 544L;  // 解码缓冲上限：约 52 万像素
const long long kMinAvailKb = 7 * 1024;   // 可用内存低于 7MB：设备已被 OOM 打残，先重启

/* 源分辨率**硬限**：超过它就没有更小的缩放档了（硬件最大只支持 1/4），直接拒绝。
 * 1080p = 1920x1080；高度留到 1088 是 1080 的宏块对齐值（有些源报 1088）。 */
const int kMaxSrcW = 1920;
const int kMaxSrcH = 1088;

/* 源像素 → 缩放解码倍率（0=不缩放，2=1/2，4=1/4，对应 E_H264_PLAYER_FLAG_SCALE_DOWN_2/_4）。
 *
 * 分档（判据是**源**像素）：
 *   ≤ 960x544（52 万）   → 0    直接解
 *   ≤ 1280x720（92 万）  → 2    1/2 → 解码缓冲最大 640x360
 *   更大（1080p 及以上）  → 4    1/4 → 1080p 变 480x270
 *
 * ★ 为什么 1080p 用 **1/4** 而不是 1/2（2026-09-14 用户要求加 1080p 频道）：
 *   1/2 后是 960x540（≈52 万像素），**正好贴着本板解码缓冲上限** 960x544 ——
 *   参考帧池一点余量都没有（本板 MemTotal 只有 56MB，实测 720p 直解必 OOM）。
 *   1/4 后 480x270（13 万像素），比**已经验过**的 720p@1/2（640x360，23 万）还小一半，
 *   是这个板子上唯一敢用的档位。屏幕宽度本来就只有 480，1/4 也不浪费清晰度。
 *
 * ⚠️ 改这里的分档线要同时想清楚 guardDecodeMemory 的估算（它按这个倍率折算解码像素）。
 */
int pickScaleDown(int w, int h) {
  if (w <= 0 || h <= 0) return 0;
  long long px = (long long)w * h;
  if (px <= kMaxDecodePixels) return 0;
  if (px <= 1280LL * 720) return 2;
  return 4;   // 1080p 及以上：1/4（解码缓冲只有源像素的 1/16）
}

/* QA `streammax <像素数>` 可临时放宽/收紧（现场标定用，只在本次进程有效） */
long long sMaxPixelsOverride = 0;

/* QA `memguard <kB>` 的临时覆盖值（0 = 用默认 kMinAvailKb） */
long long sMinAvailOverride = 0;

long long memAvailKb() {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return -1;
  char line[128];
  long long v = -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemAvailable:", 13) == 0) { v = atoll(line + 13); break; }
  }
  fclose(f);
  return v;
}

/* 回收页缓存 / 可回收 slab。
 * 本板共 56MB，跑久了 Buffers+Cached 能占十几 MB —— 实测 drop_caches 之后
 * MemAvailable 从 14MB 回到 31MB（回收约 17MB）。
 * 应用是 root（uid 0），/proc/sys/vm/drop_caches 可直接写。
 * 只在“内存不足、本来就要拒绝播放”时才调用（起播前，用户感知不到）。 */
long long reclaimCaches() {
  sync();                       // 先把脏页刷下去，drop 才彻底
  FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
  if (!f) return -1;            // 没权限（非 root）→ 让调用方走原来的拒绝逻辑
  fputs("3\n", f);              // 1=页缓存 2=可回收 slab 3=两者
  fclose(f);
  usleep(300 * 1000);           // 给内核一点回收时间
  return memAvailKb();
}

/* ==================== H264 SPS 解析：只为问出"源分辨率" ====================
 * 为什么需要：**直播流开流时源尺寸常常是 0x0**（我们的 HLS 中继从"最新写入位置"
 * 开始，开流那几百毫秒内解封装还没解析到 SPS）—— 而硬件播放器 `zk_h264_player_init`
 * 必须拿到真实宽高（实测拿 0x0 去 init：它按默认尺寸分配画面缓冲，画面全黑且解码
 * 输出队列停住不消化）。
 *
 * 为什么自己写而不是"软解一帧问尺寸"：本工程链的 ffmpeg **没编 H264 解码器**
 * （实测 `avcodec_find_decoder(AV_CODEC_ID_H264)` 返回 0；视频解码全靠设备硬解、
 * 音频才是软解），所以没有软解探针这条路。SPS 里的宽高是定长字段，解析很短。
 *
 * 只做一件事：在 Annex-B 码流里找 NAL type=7（SPS），读出 coded 宽高。 */
struct BitRd {
  const uint8_t *d;
  int n, bit;
};
int rdBit(BitRd &r) {
  int v = (r.bit < r.n * 8) ? ((r.d[r.bit >> 3] >> (7 - (r.bit & 7))) & 1) : 0;
  ++r.bit;
  return v;
}
unsigned rdBits(BitRd &r, int n) {
  unsigned v = 0;
  for (int i = 0; i < n; ++i) v = (v << 1) | (unsigned)rdBit(r);
  return v;
}
unsigned rdUE(BitRd &r) {
  int z = 0;
  while (z < 32 && rdBit(r) == 0) ++z;
  unsigned v = z ? rdBits(r, z) : 0;
  return ((1u << z) - 1) + v;
}
int rdSE(BitRd &r) {
  unsigned v = rdUE(r);
  return (v & 1) ? (int)((v + 1) >> 1) : -(int)(v >> 1);
}
/* nal 指向 NAL 头（含 header 字节），n = 该 NAL 剩余字节数 */
bool parseSpsSize(const uint8_t *nal, int n, int *outW, int *outH) {
  uint8_t ue[1024];
  int un = 0;
  /* 去掉防竞争字节 00 00 03（SPS 里一定会有） */
  for (int i = 1; i < n && un < (int)sizeof(ue); ++i) {
    if (i + 2 < n && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
      ue[un++] = 0;
      ue[un++] = 0;
      i += 2;
    } else {
      ue[un++] = nal[i];
    }
  }
  BitRd r = {ue, un, 0};
  unsigned prof = rdBits(r, 8);
  rdBits(r, 8);   // constraint flags + reserved
  rdBits(r, 8);   // level_idc
  rdUE(r);        // seq_parameter_set_id
  unsigned chroma = 1;   // 默认 4:2:0
  if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 ||
      prof == 86 || prof == 118 || prof == 128 || prof == 138 || prof == 139 || prof == 134 ||
      prof == 135) {
    chroma = rdUE(r);
    if (chroma == 3) rdBit(r);   // separate_colour_plane_flag
    rdUE(r);                     // bit_depth_luma_minus8
    rdUE(r);                     // bit_depth_chroma_minus8
    rdBit(r);                    // qpprime_y_zero_transform_bypass_flag
    if (rdBit(r)) {              // seq_scaling_matrix_present_flag
      int cnt = (chroma == 3) ? 12 : 8;
      for (int i = 0; i < cnt; ++i) {
        if (!rdBit(r)) continue;
        int last = 8, next = 8;
        int size = (i < 6) ? 16 : 64;
        for (int j = 0; j < size; ++j) {
          if (next != 0) {
            int d = rdSE(r);
            next = (last + d + 256) % 256;
          }
          last = next ? next : last;
        }
      }
    }
  }
  rdUE(r);   // log2_max_frame_num_minus4
  unsigned poct = rdUE(r);
  if (poct == 0) {
    rdUE(r);   // log2_max_pic_order_cnt_lsb_minus4
  } else if (poct == 1) {
    rdBit(r);
    rdSE(r);
    rdSE(r);
    unsigned k = rdUE(r);
    for (unsigned i = 0; i < k && i < 256; ++i) rdSE(r);
  }
  rdUE(r);   // max_num_ref_frames
  rdBit(r);  // gaps_in_frame_num_value_allowed_flag
  unsigned wmbs = rdUE(r) + 1;
  unsigned hmbs = rdUE(r) + 1;
  unsigned fmo = rdBit(r);   // frame_mbs_only_flag
  if (!fmo) rdBit(r);        // mb_adaptive_frame_field_flag
  rdBit(r);                  // direct_8x8_inference_flag
  unsigned cl = 0, cr = 0, ct = 0, cb = 0;
  if (rdBit(r)) {            // frame_cropping_flag
    cl = rdUE(r);
    cr = rdUE(r);
    ct = rdUE(r);
    cb = rdUE(r);
  }
  int subW = (chroma == 3) ? 1 : 2;
  int subH = (chroma == 1 || chroma == 0 || chroma == 3) ? 2 : 1;   // 4:2:0 → 2
  int W = (int)wmbs * 16 - (int)(cl + cr) * subW;
  int H = (int)(2 - fmo) * (int)hmbs * 16 - (int)(ct + cb) * subH * (int)(2 - fmo);
  if (W <= 0 || H <= 0 || W > 4096 || H > 4096) return false;
  *outW = W;
  *outH = H;
  return true;
}
/* 在一段 Annex-B 码流里找 SPS 并解析宽高；找到返回 true */
bool findSpsSize(const uint8_t *d, int n, int *outW, int *outH) {
  for (int i = 0; i + 3 < n; ++i) {
    if (d[i] != 0 || d[i + 1] != 0) continue;
    int sc = 0;
    if (d[i + 2] == 1) sc = 3;
    else if (d[i + 2] == 0 && i + 3 < n && d[i + 3] == 1) sc = 4;
    if (!sc) continue;
    const uint8_t *nal = d + i + sc;
    if ((nal[0] & 0x1f) == 7 && parseSpsSize(nal, n - (i + sc), outW, outH)) return true;
    i += sc - 1;
  }
  return false;
}

/* 返回 0 = 放行；非 0 = 拒绝（理由已打日志）
 *
 * ⚠️ 判据只认**像素上限**（物理限制），不拿 MemAvailable 去卡"够不够"：
 *    实测同一分辨率的第二轮播放内存**一点不涨**（解码池会复用），
 *    而播过一轮之后 MemAvailable 会从 28MB 掉到 11MB —— 若按"够不够"判，
 *    第二轮就会被自己拒掉。所以这里只用 MemAvailable 判断**设备是不是已经被
 *    OOM 打残**（那种状态下连小申请都会再次触发 OOM，必须重启才好）。 */
int guardDecodeMemory(int w, int h) {
  if (w <= 0 || h <= 0) return 0;

  /* ① 源分辨率硬限：硬件最大只支持 1/4 缩放，源再大就没档可用了 */
  if (w > kMaxSrcW || h > kMaxSrcH) {
    LOGW("PgStream: 拒绝播放 —— 源分辨率 %dx%d 超过本板可处理的最大源 %dx%d"
         "（硬件最大 1/4 缩放解码，再大的源没有更小的档）。请用 1080p 及以下的源。",
         w, h, kMaxSrcW, kMaxSrcH);
    return -1;
  }

  /* ② ★ 按**解码缓冲**的像素判，不是源像素（2026-09-14 改：加 1080p 支持）。
   *    缩放解码时解码器只按 (源/倍率)² 分配参考帧池（crop 也要折算，见
   *    PgH264::applyCrop）—— 原来拿源像素判 ⇒ 1080p（207 万像素）直接被拒，
   *    即使它会走 1/4 缩放（解码缓冲只有 13 万像素）。 */
  const int sd = pickScaleDown(w, h);
  const int div = (sd > 1) ? sd : 1;
  const long long srcPx = (long long)w * h;
  const long long px = srcPx / ((long long)div * div);
  long long cap = sMaxPixelsOverride > 0 ? sMaxPixelsOverride : kMaxDecodePixels;
  long long needKb = px * kBytesPerPixel / 1024 + 2048;
  long long availKb = memAvailKb();
  LOGD("PgStream: 内存守卫 源 %dx%d（%lld 像素）→ 缩放解码 1/%d → 解码缓冲 %lld 像素，"
       "预计占用 ~%lldkB，可用 %lldkB，解码像素上限 %lld",
       w, h, srcPx, sd ? sd : 1, px, needKb, availKb, cap);
  if (px > cap) {
    LOGW("PgStream: 拒绝播放 —— %dx%d 缩放 1/%d 后仍有 %lld 像素，超过本板安全上限 %lld。"
         "本板共 56MB 内存且内核未开 memory compaction：实测 1280x720 直解 1 秒内必触发 "
         "OOM，把应用 SIGKILL（之后还会漏不可回收内存 → 设备进入重启死循环）。"
         "请用低分辨率源；现场标定用 QA `streammax <像素数>`。",
         w, h, sd ? sd : 1, px, cap);
    return -1;
  }
  long long minAvail = sMinAvailOverride > 0 ? sMinAvailOverride : kMinAvailKb;
  if (availKb > 0 && availKb < minAvail) {
    /* 先自救再拒人：回收页缓存后重判一次。
     * 不做这一步的话，设备连续投屏几次后可用内存掉到安全线以下，用户就只能
     * “重启设备”才能继续（实测：连续投屏后 MemAvailable 6.9MB，连着三轮被拒）。 */
    long long after = reclaimCaches();
    if (after > 0) {
      LOGD("PgStream: 可用内存偏低（%lldkB < %lldkB）→ 回收页缓存后 %lldkB（%s）", availKb,
           minAvail, after, after >= minAvail ? "已放行" : "仍不足");
      availKb = after;
    }
  }
  if (availKb > 0 && availKb < minAvail) {
    LOGW("PgStream: 拒绝播放 —— 设备可用内存只剩 %lldkB（低于 %lldkB 安全线，"
         "已尝试回收页缓存仍不足）。这通常是被 OOM 杀过之后的状态，请**重启设备**再试。",
         availKb, minAvail);
    return -2;
  }
  return 0;
}

void *threadMain(void *arg) {
  (void)arg;
  sProgressMs = monoMs();   // 看门狗起点：从线程启动开始算"还有进展"
  int vi = -1, w = 0, h = 0, dummy = 0;
  int rotMeta = -1;
  AVFormatContext *fmt = openInput(sUrl, &vi, &w, &h, &dummy, &rotMeta);
  if (!fmt) { sRunning = 0; return 0; }

  sProgressMs = monoMs();   // 开流完成 → 刷新
  sRotSource = rotMeta;     // 容器声明的角度（无则为 -1）
  {
    int want = desiredRotation();
    LOGD("PgStream: 旋转 -- 源元数据=%s 手动=%s → 本次用 %s", rotName(rotMeta),
         sRotManual < 0 ? "无（auto）" : rotName(sRotManual), rotName(want));
  }
  const bool hasVideo = (vi >= 0);
  AVCodecParameters *cp = hasVideo ? fmt->streams[vi]->codecpar : 0;
  /* 总时长（AV_TIME_BASE 单位 = 微秒）→ 毫秒；DLNA 的 GetMediaInfo 要用 */
  sDurMs = (fmt->duration > 0) ? (long long)(fmt->duration / 1000) : -1;
  sPosMs = 0;
  sAudioFrames = 0;   // ⚠️ 必须清零：音频时钟 = 已推进 - 队列，跨流累积会让时钟巨大失去意义
  sAudioOff = 0;      // 每轮流重新判断音频是否可用（声卡可能刚被 pcmfree/pcmopen）
  sFileLoops = 0;     // ★ 本地文件循环计数（见 PgStream.h 的 setFileLoop）
  sLastSeekMs = -1;   // 还没有过接缝
  sHwDrop = 0;
  LOGD("PgStream: 总时长 %lld ms", sDurMs);
  /* 只有 H.264 有 h264_mp4toannexb（这个包没编 hevc 的 bsf）；其它编码直接把包丢给解码器试 */
  const AVBitStreamFilter *bf = 0;
  AVBSFContext *bsf = 0;
  bool needBsf = (cp && cp->codec_id == AV_CODEC_ID_H264);
  if (needBsf) {
    bf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bf && av_bsf_alloc(bf, &bsf) == 0) {
      avcodec_parameters_copy(bsf->par_in, cp);
      if (av_bsf_init(bsf) < 0) { av_bsf_free(&bsf); }
    }
  }
  LOGD("PgStream: Annex-B 转换=%s", bsf ? "启用(h264_mp4toannexb)" : "不需要/不可用");

  /* ---- MPP：初始化 + 建硬解通道（纯音频源整段跳过） ---- */
  MppApi &m = mpp();
  VDEC_CHN chn = sChn;   // 中途停播过的通道号会被跳过（见收尾 ④）
  /* [HW] 硬件播放器的起播状态（见下面 hw 分支与主循环里的 [HW] 段） */
  bool hwStarted = false;
  int hwW = 0, hwH = 0;          // 硬件播放器用的源尺寸（从 SPS 解析出来的）
  int hwWaitPkts = 0;            // 等 SPS 期间读了多少个视频包
  if (!hasVideo) {
    LOGD("PgStream: 纯音频源 —— 跳过 MPP/硬解/显示，只解音频");
    Ff::ensureReady();
  } else if (sHwVideo) {
  /* ★★ 视频走**硬件播放器**（zk_h264_player）—— 解码 + 旋转 + 缩放 + 裁剪一体。
   *
   * 为什么换掉老的 MPP 路：老路必须自己拿 CPU 转帧（本板 MPP 的 VDEC 硬件旋转会输出
   * "DE 访问不到的缓冲"，见上面 rotateInto 的长注释），而那个转法有两个硬伤：
   *   ① 把 VDEC 实际输出的 **NV12**（fmt=23 SEMIPLANAR_420）当 **I420 三平面**拆开转
   *      → 色度全乱，屏幕上大片纯绿；
   *   ② 转出来的缓冲是 256x448，而 VO 层的 frame 是 480x700 → 显示引擎按 480 宽去读
   *      一块 stride=256 的缓冲 → **画面横向重复**（这就是用户报的"错屏"）。
   * 硬件这条路：旋转交给 `set_rot`、屏幕位置/大小交给 `set_pos`、裁哪块源画面交给
   * `set_crop` —— 全是硬件干的，没有 CPU 转帧，也就没有格式/几何错配的余地。 */
  {
    int deg = desiredRotation();
    sRotApplied = deg;
    sHwSrcW = w;
    sHwSrcH = h;
    if (w > 0 && h > 0) {
      /* 尺寸已知（MP4/本地文件/带 SPS 的流）→ 直接起播 */
      int scaleDown = pickScaleDown(w, h);
      int rx = 0, ry = 0, rw = 0, rh = 0;
      pg::H264Player::fitRect(w, h, deg, sDispX, sDispY, sDispW, sDispH, &rx, &ry, &rw, &rh);
      LOGD("PgStream: [HW] 视频走硬件播放器 —— 源 %dx%d，旋转 %s，缩放解码 1/%d，"
           "屏幕显示区 (%d,%d %dx%d)（等比放进 %dx%d 的播放页）",
           w, h, rotName(deg), scaleDown ? scaleDown : 1, rx, ry, rw, rh, sDispW, sDispH);
      if (!pg::H264Player::startStream(w, h, scaleDown, deg, rx, ry, rw, rh)) {
        LOGW("PgStream: 硬件播放器起播失败 —— %s（本次不出画；可用 QA streamhw 0 切回 MPP 老路）",
             pg::H264Player::lastError());
        closeInput(&fmt);
        sRunning = 0;
        return 0;
      }
      hwStarted = true;
    } else {
      /* ⚠️⚠️ **直播流开流时源尺寸常是 0x0**（我们的 HLS 中继从"最新写入位置"开始，
       * 开流那几百毫秒内解封装还没见到 SPS；实测 `codecpar` 报 0x0）。
       * **不能**拿 0x0 去 init 硬件播放器 —— 实测它会按一个默认尺寸去分配画面缓冲
       * （disp 里看到 `fb[432,240]` 而 crop 是 448x256），结果**画面全黑、解码输出
       * 队列停在 25 帧不再消化**（日志只有"解码缓冲积压 → 丢弃"，看着像喂帧太快，
       * 其实根本没上屏）。
       * ⇒ 推迟起播：在主循环里边读边**解析 SPS**（`findSpsSize`，本工程 ffmpeg 没编
       *   H264 软解，所以自己解析），拿到真实宽高再起硬件播放器；期间音频照常走。 */
      LOGD("PgStream: [HW] 源尺寸未知（直播流）→ 推迟起播，边读边解析 SPS");
    }
  }
  } else if (!m.ok) {
    closeInput(&fmt);
    sRunning = 0;
    return 0;
  } else if (guardDecodeMemory(w, h) != 0) {
    /* 分辨率/内存不够：**明确拒绝**，绝不冒险去建解码通道 ——
     * 一旦被内核 OOM 杀掉，不只会漏内存，还会让设备进入"起来就死"的重启死循环。 */
    LOGW("PgStream: 本次不播放（分辨率过高/内存不足，详见上一条日志）");
    closeInput(&fmt);
    sRunning = 0;
    return 0;
  } else {
  /* ⚠️⚠️ 通道**常开复用**（2026-09-13 改，重要）
   * 为什么不能每轮流都 Create/Destroy：**中途停播（DLNA stop / streamstop）之后
   * `AW_MPI_VDEC_DestroyChn` 会永久卡住**（实测：日志只到"StopRecvStream ok"就没下文，
   * SetStreamEof / 空包 EOS / 先抽干输出 都救不回来；线程停在 libmedia_mpp 内部等锁，
   * 不是内核驱动等待）。后果是收尾线程永远不退出 → `sRunning` 不归零 → 之后所有播放
   * 都报"已有任务在跑"（播放器直到重启都不可用），声卡的"空闲自动让出"也被一直挡住。
   * ⇒ 本平台的正确做法和 VO 一样是**"常开"**（厂商 zkmedia 退出时也不释放）：
   *    只做 StopRecvStream，通道留着复用；分辨率变了才销毁重建（那一步有看门狗兜底）。 */
  LOGD("PgStream: [T1] 进入 MPP 分支 sSysInited=%d sChnReady=%d", sSysInited, sChnReady);
  if (sChnReady && (sChnW != w || sChnH != h)) {
    LOGD("PgStream: 分辨率变化 %dx%d -> %dx%d，销毁旧通道（这一步厂商实现上可能卡住，有看门狗兜底）",
         sChnW, sChnH, w, h);
    m.VDEC_StopRecvStream(sChn);
    m.VDEC_DestroyChn(sChn);
    sChnReady = 0;
  }
  if (!sSysInited) {
    /* ⚠️ 必须先 SetConf 再 Init：zkmedia 也是这个顺序。
     *    只调 Init 会返回 0xa0028010（低位 0x10 = EN_ERR_SYS_NOTREADY），
     *    此时再调 VDEC_CreateChn 会**直接段错误**（实测崩溃循环）。
     *    SetConf 里就是"图像 buffer 对齐宽度"和"mkfc 临时目录"。 */
    LOGD("PgStream: [T2] 调 SYS_SetConf 前");
    MPP_SYS_CONF_S conf;
    memset(&conf, 0, sizeof(conf));
    conf.nAlignWidth = 16;
    snprintf(conf.mkfcTmpDir, sizeof(conf.mkfcTmpDir), "/tmp");
    ERRORTYPE e0 = m.SYS_SetConf(&conf);
    LOGD("PgStream: AW_MPI_SYS_SetConf -> 0x%x", e0);
    ERRORTYPE e1 = m.SYS_Init();
    LOGD("PgStream: AW_MPI_SYS_Init -> 0x%x", e1);
    if (e1 != SUCCESS) {
      LOGW("PgStream: SYS_Init 未成功，放弃（不要继续调 CreateChn，会崩）");
      closeInput(&fmt);
      sRunning = 0;
      return 0;
    }
    sSysInited = 1;
  }

  ERRORTYPE er = SUCCESS;
  LOGD("PgStream: [T3] 建/复用通道前（sChnReady=%d）", sChnReady);
  if (sChnReady && sChnW == w && sChnH == h) {
    chn = sChn;
    er = m.VDEC_StartRecvStream(chn);
    LOGD("PgStream: [T3a] 复用常开通道 %d，StartRecvStream -> 0x%x（%dx%d）", chn, er, w, h);
    /* 旋转角度在这里**不碰解码器**：解码器恒 0°，角度变了只是我们送显示前多转一道
     * （sRotApplied 在每轮开流时已经按 desiredRotation() 更新）。 */
  } else {
  VDEC_CHN_ATTR_S attr;
  memset(&attr, 0, sizeof(attr));
  attr.mType = PT_H264;
  /* 码流缓冲：**别给大**。原来给 w*h*3/2（720p 就是 1.35MB，还带 1MB 下限）。
   * 本板内存按像素算（见 guardDecodeMemory），每省一帧都是余量。
   * 给 w*h/4（640x360 → 57KB，720p → 230KB），再夹到 [256KB, 768KB]：
   * 4Mbps 码流 256KB ≈ 0.5s 缓冲，够用；缓冲满时我们**重试**而不是丢包（见下面 SendStream）。 */
  attr.mBufSize = (unsigned)(w * h / 4);
  if (attr.mBufSize < 256 * 1024) attr.mBufSize = 256 * 1024;
  if (attr.mBufSize > 768 * 1024) attr.mBufSize = 768 * 1024;
  attr.mPriority = 0;
  attr.mPicWidth = w;
  attr.mPicHeight = h;
  /* ⚠️ **解码器永远按 0° 出帧**。本板用 VDEC 自己的旋转（mInitRotation /
   * AW_MPI_VDEC_SetRotate）会输出一块 **DE 访问不到的缓冲**：帧的宽高变了、像素全是 0，
   * 内核同时刷 `DE invalid address / L2 PageTable Invalid`，屏幕上就是黑的
   * （MPP 内部不报任何错；SetRotate 在通道已存在时还回 0xa0058009 并把通道搞坏）。
   * 旋转统一由我们在**送显示之前**自己做（rotate90Into）。 */
  attr.mInitRotation = ROTATE_NONE;
  sRotApplied = desiredRotation();
  if (sRotApplied == 90 || sRotApplied == 180 || sRotApplied == 270) {
    LOGD("PgStream: 画面旋转 = %s（解码器不转 → 送显示前由我们转）", rotName(sRotApplied));
  } else {
    LOGD("PgStream: 画面旋转 = %s（不转）", rotName(sRotApplied));
  }
  attr.mOutputPixelFormat = MM_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
  attr.mVdecVideoAttr.mRefFrameNum = 1;   // 参考帧最少（内存最省）
  attr.mVdecVideoAttr.mMode = VIDEO_MODE_FRAME;   // 按帧送（我们按包送，mbEndOfFrame=1）
  attr.mVdecVideoAttr.mSupportBFrame = 1;
  /* 帧缓冲数量：头文件说明 `mnFrameBufferNum` 只对 jpeg 有效、`mExtraFrameNum` 是
   * "在初始帧数基础上多申请几个" → 两者都置 0（= 让 vdeclib 自己算最少的）。
   * 实测把 mExtraFrameNum 调大并不会让 720p 变可行（内存是硬限制），故保持 0。 */
  attr.mnFrameBufferNum = 0;
  attr.mExtraFrameNum = 0;
  er = m.VDEC_CreateChn(chn, &attr);
  LOGD("PgStream: AW_MPI_VDEC_CreateChn(%d, %dx%d, 码流缓冲 %uKB) -> 0x%x  可用内存 %lldkB",
       chn, w, h, attr.mBufSize / 1024, er, memAvailKb());
  if (er != SUCCESS) {
    closeInput(&fmt);
    sRunning = 0;
    return 0;
  }
  er = m.VDEC_StartRecvStream(chn);
  LOGD("PgStream: AW_MPI_VDEC_StartRecvStream -> 0x%x", er);
  sChn = chn;
  sChnW = w;
  sChnH = h;
  sChnReady = 1;   // 通道常开：之后只 StopRecvStream，不再 DestroyChn
  }   // end if (!sChnReady)

  /* 要出画面就先建视频层（必须在 SYS_Init 之后；失败只降级为"只看日志不出画"）
   * ⚠️ **只建一次**：本平台 VO 是"常开"设备（厂商播放器退出也不释放），
   * 每轮流都 Stop/Destroy 再建会连带把 VDEC 的引用关系搞坏（见收尾注释）。 */
  if (sDisp && !sVoStarted) {
    if (voStart(sDispX, sDispY, sDispW, sDispH)) sVoStarted = 1;
    else LOGW("PgStream: 视频层未就绪 —— 只解码、不上屏");
  }
  }   // end if (hasVideo)

  /* ---- 音频：ffmpeg 软解 → 重采样 22050Hz/单声道/S16 → 灌进那条常开 PCM 流 ----
   * 为什么不用 MPP 的 ADEC/AO：框架那条（zkmedia→eyesee-mpp AO）**自己拼声卡设备名**
   * （拼成 hw:1,0，而本板喇叭在 card0）→ 日志全对却完全静音。我们这条常开 PCM 流才是
   * 实测能出声的那条，所以直接把 PCM 灌给它（顺带无爆音、与音效共用一条流）。 */
  pg::DeviceAudio *auAudio = pg::globalAudio();
  int ai = -1;
  AVCodecContext *adec = 0;
  SwrContext *swr = 0;
  AVFrame *aframe = av_frame_alloc();
  int nAudioFrames = 0;
  if (auAudio) {
    LOGD("PgStream: [T4] 音频初始化（pcmHeld=%d）", (int)auAudio->pcmHeld());
    /* ⚠️ 这里**不再**自己调 acquirePcm()：实测在"声卡已被空闲让出"的情况下，
     * 从这个播放线程里重新打开声卡会把整个线程卡死（DLNA 起流后没有画面没有声音、
     * 且 sRunning 永远不归零）。拿回动作统一放在**调用方（UI 线程）**做 —— 见
     * startCommon()：起流前先 ensurePcmForPlayback（走的是音效那条已验证 13ms 的路）。 */
    if (!auAudio->pcmHeld()) {
      LOGW("PgStream: 声卡未被占用 —— 本次只播视频（下次起流前会由 UI 线程拿回）");
      sAudioOff = 1;
    }
    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if (ai >= 0) {
      AVCodec *ac = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
      if (ac) {
        adec = avcodec_alloc_context3(ac);
        if (adec && avcodec_parameters_to_context(adec, fmt->streams[ai]->codecpar) == 0 &&
            avcodec_open2(adec, ac, 0) == 0) {
          int64_t inLayout = adec->channel_layout
                                 ? (int64_t)adec->channel_layout
                                 : av_get_default_channel_layout(adec->channels);
          swr = swr_alloc_set_opts(0, AV_CH_LAYOUT_MONO, AV_SAMPLE_FMT_S16, kAudioOutRate,
                                   inLayout, adec->sample_fmt, adec->sample_rate, 0, 0);
          if (swr && swr_init(swr) == 0 && auAudio->streamOn(true)) {
            LOGD("PgStream: 音频就绪 %s %dHz %dch → %dHz 单声道 S16",
                 avcodec_get_name(fmt->streams[ai]->codecpar->codec_id), adec->sample_rate,
                 adec->channels, kAudioOutRate);
          } else {
            /* 声卡被释放（QA pcmfree）或重采样失败 → **降级为只播视频**，用墙钟同步。
             * 必须在这里就认下来：不然循环会一直 streamWrite() 收 0 → 空转卡死。 */
            LOGW("PgStream: 音频不可用（声卡未占用/重采样失败）→ 本次只播视频（墙钟同步）");
            sAudioOff = 1;
            if (swr) { swr_free(&swr); swr = 0; }
            if (adec) { avcodec_free_context(&adec); adec = 0; }
            auAudio = 0;
          }
        } else {
          LOGW("PgStream: 音频解码器打开失败（%s）→ 改用墙钟同步",
               avcodec_get_name(fmt->streams[ai]->codecpar->codec_id));
          sAudioOff = 1;
          if (adec) { avcodec_free_context(&adec); adec = 0; }
        }
      } else {
        LOGW("PgStream: 没有可用的音频解码器（id=%d）→ 改用墙钟同步",
             (int)fmt->streams[ai]->codecpar->codec_id);
        sAudioOff = 1;
      }
    } else {
      /* ⚠️⚠️ 没有音频轨时必须把音频时钟**关掉**（sAudioOff=1）。
       * 否则 audioClockMs() 会返回 `(0 帧 - 0 队列) = 0ms` —— 一个永远不前进的合法时钟！
       * 于是每帧都判"没到显示时刻"，每帧都要等满 500ms 兜底硬送 → **画面只有 2fps**
       * （实测 640x360 无音轨源：解码帧 #1..#10 全是"等音频超 500ms 硬送"）。 */
      LOGD("PgStream: 该流没有音频轨 → 用墙钟同步（关音频时钟）");
      sAudioOff = 1;
    }
  } else {
    LOGW("PgStream: 没有全局音频实例（PgAudio 未 init？）");
  }

  /* ---- 解封装 → 转 Annex-B → 送硬解 → 取出解码帧 ---- */
  AVPacket *pkt = av_packet_alloc();
  AVPacket *op = av_packet_alloc();
  long long t0 = monoMs();
  sHasVideo = hasVideo ? 1 : 0;
  sFramesOut = 0;
  sLoopStartMs = monoMs();
  sProgressMs = monoMs();   // 硬件/解码链路就绪、即将进主循环 → 刷新
  int nPkt = 0, nSent = 0, nFrame = 0;
  sHwFed = 0;
  /* 读窗口（媒体时间前瞻上限）：读得比播得快不能超过这个值。
   * 太小 → 解码器饿死；太大 → 整个文件提前读完（无音频源尤其明显）。 */
  const long long kLookaheadMs = 1500;
  /* ★ 本地文件循环用的**本轮音频时钟基准**（0 = 本轮还没循环过）。
   *   `sAudioFrames` 是**跨轮连续**累计的（故意不清 —— 它与声卡实际播放位置对应），
   *   所以第二轮开始"音频时钟已经 10000ms，而包时间又从 0 起" ⇒ 不折算的话
   *   节流判据 `curPktMs > clk + kHwLeadMs` 恒不成立 ⇒ **整轮包被一口气喂完**
   *   （老毛病：10s 的片 2s 播完、只剩最后一帧挂在屏上）。 */
  long long audioBaseMs = 0;
  /* ---- 待显示帧（挂起）----
   * ⚠️ 这是本循环最重要的结构：帧"还没到显示时刻"时**绝不能阻塞等待** ——
   *    等待期间同一线程就没在读包喂音频，音频环被抽干 → 音频时钟不再前进 → 越等越等不到。
   *    实测（等待上限 2s 那版）整条链路掉到 **0.4 倍实时**、伴音断续。
   *    正确做法：把帧挂起，循环继续读包（喂音频），下一圈再看它到点没。 */
  VIDEO_FRAME_INFO_S fi;
  memset(&fi, 0, sizeof(fi));
  bool haveFrame = false;
  long long framePtsMs = -1;
  long long sLastSentPts = -1;   // 上一帧已发出的 PTS（钳制 B 帧造成的"倒退"）
  long long frameWaitFrom = 0;
  long long curPktMs = -1;   // 最近一个**视频**包对应的媒体时间（给挂起帧用）
  /* 视频包 PTS 队列（先进先出）：硬解是异步的，取出来的帧对应的是**前几个**送进去的包。
   * 用"最近读到的包"当帧 PTS 会把它推到未来（读得越快推得越远）→ 每帧都等满 500ms 兜底，
   * 画面比声音慢一截。这里按 FIFO 一一对上（满了就丢最老的，只影响极端积压场景）。 */
  const int kPtsQ = 256;
  long long ptsQ[kPtsQ];
  int ptsQh = 0, ptsQt = 0;

  bool eosSent = false;
  long long finishFrom = 0;
  /* [关键] 主循环自己的退出原因：只有“因为 sFinishReq 而退出”才算“被用户/控制器停止”。
   * 不能看收尾那一刻的 sFinishReq 值 —— 自然播完后 mainLogic 会收掉投屏页并调
   * stopCast()→StreamPlayer::stop()，把 sFinishReq 也置成 1，于是被判成“被停止”，
   * 结果每播完一个视频就把应用重启一次（实测 pid 每轮都变，2026-09-13 修）。 */
  bool userStopped = false;
  while (!sStopReq) {
    sProgressMs = monoMs();   // 主循环每轮都算有进展（否则会被看门狗误判）
    /* ---- 优雅停止：不再读包，发 EOS，把解码器放空后退出 ----
     * 放空判据用 VDEC_Query 的「左图 0 + 左码流 0 字节 + 左待解帧 0」，
     * 兜底最多等 1.5s（宁可慢一点收尾，也不要留个脏通道把后续投屏全废掉）。 */
    if (sFinishReq) {
      if (!finishFrom) finishFrom = monoMs();
      /* 硬件播放器那条路**没有解码通道要放空**（zk_h264_player 自己管自己的缓冲）：
       * 直接停止收流即可，由收尾处 H264Player::stop() 一次性清干净。 */
      if (sHwVideo && hasVideo) {
        LOGD("PgStream: [HW] 收到停止请求 → 直接收尾（硬件播放器无解码通道需放空）");
        userStopped = true;
        break;
      }
      if (!eosSent) {
        eosSent = true;
        if (m.VDEC_SetStreamEof) m.VDEC_SetStreamEof(chn, 1);
        VDEC_STREAM_S eos;
        memset(&eos, 0, sizeof(eos));
        eos.mbEndOfStream = 1;
        LOGD("PgStream: 收到停止请求 → 发 EOS 并放空解码器");
        if (hasVideo) m.VDEC_SendStream(chn, &eos, 200);
      }
      VIDEO_FRAME_INFO_S f;
      memset(&f, 0, sizeof(f));
      if (hasVideo && m.VDEC_GetImage(chn, &f, 0) == SUCCESS) {
        if (sDisp) m.VO_SendFrame(kVoLayer, kVoChn, &f, 20);
        m.VDEC_ReleaseImage(chn, &f);
        ++nFrame;
        continue;   // 一直取到取不出来为止
      }
      bool idle = false;
      if (hasVideo && m.VDEC_Query) {
        VDEC_CHN_STAT_S st;
        memset(&st, 0, sizeof(st));
        if (m.VDEC_Query(chn, &st) == SUCCESS && st.mLeftPics == 0 &&
            st.mLeftStreamBytes == 0 && st.mLeftStreamFrames == 0) {
          idle = true;
        }
      }
      if (idle || (monoMs() - finishFrom) > 1500) {
        LOGD("PgStream: 解码器已放空（%s，用时 %lldms）→ 收尾",
             idle ? "Query 报空闲" : "等待超时", monoMs() - finishFrom);
        userStopped = true;   /* 确实是被“停止请求”打断的 → 通道会变脏，要复位应用 */
        break;
      }
      usleep(5000);
      continue;
    }
    /* (A) 有挂起帧：到点了就送显示（**硬件播放器那条路没有挂起帧**，见 (B) 的说明） */
    if (!sHwVideo && haveFrame) {
      long long ac = audioClockMs(auAudio);
      if (ac >= 0) ac -= audioBaseMs;   // ★ 本地循环：折算到"本轮"时间轴
      bool due;
      if (ac >= 0 && framePtsMs >= 0) {
        due = (framePtsMs <= ac + 120);                 // 有音频：以音频时钟为准
      } else if (framePtsMs >= 0) {
        due = (framePtsMs <= (monoMs() - t0) + 120);    // 无音频：以墙钟为准
      } else {
        due = true;
      }
      if (!due && (monoMs() - frameWaitFrom) > 500) {
        LOGW("PgStream: 第 %d 帧等音频超 500ms（音频时钟 %lldms / 帧 PTS %lldms），硬送",
             nFrame + 1, ac, framePtsMs);
        due = true;
      }
      if (due) {
        ++nFrame;
        sFramesOut = nFrame;   // 给 videoStalled() 看
        if (framePtsMs > sLastSentPts) sLastSentPts = framePtsMs;  // 供下一帧做单调钳制
        if (sDisp) {
          /* 画面旋转：本板只能由我们自己在送显示前转（见 rotate90Into 的说明）。
           * 转完的帧指向 MMZ 缓冲（DE 能访问），不转时直接用解码器给的帧（零拷贝）。 */
          const VIDEO_FRAME_INFO_S *pf = &fi;
          VIDEO_FRAME_INFO_S rotFrm;
          if (sRotApplied == 90 || sRotApplied == 180 || sRotApplied == 270) {
            if (ensureRotBuf((int)fi.VFrame.mWidth, (int)fi.VFrame.mHeight, sRotApplied) &&
                rotateInto(fi, rotFrm, sRotApplied)) {
              pf = &rotFrm;
            }
          }
          ERRORTYPE vr = m.VO_SendFrame(kVoLayer, kVoChn, (VIDEO_FRAME_INFO_S *)pf, 20);
          if (nFrame <= 3) {
            LOGD("PgStream: AW_MPI_VO_SendFrame(#%d %dx%d stride=%u) -> 0x%x%s", nFrame,
                 pf->VFrame.mWidth, pf->VFrame.mHeight, pf->VFrame.mStride[0], vr,
                 pf == &rotFrm ? "（已旋转）" : "");
          } else if (vr != SUCCESS) {
            LOGW("PgStream: AW_MPI_VO_SendFrame(第 %d 帧) -> 0x%x", nFrame, vr);
          }
          /* QA streamgrab：抓**真正送去显示的那一帧**（旋转后），这才是屏幕上该有的画面 */
          if (grabTake(nFrame)) dumpFrameY(*pf);
        }
        if (nFrame <= 5 || (nFrame % 30) == 0) {
          LOGD("PgStream: 解码帧 #%d 宽高=%dx%d（已送 %d 包 / 墙钟 %lldms / 音频时钟 %lldms / "
               "队列 %d 帧 / 本帧 PTS %lldms）",
               nFrame, fi.VFrame.mWidth, fi.VFrame.mHeight, nSent, monoMs() - t0,
               (audioClockMs(auAudio) < 0 ? -1 : audioClockMs(auAudio) - audioBaseMs),
               auAudio ? auAudio->streamQueuedFrames() : -1,
               framePtsMs);
        }
        m.VDEC_ReleaseImage(chn, &fi);
        haveFrame = false;
        if (sMaxSec > 0 && (monoMs() - t0) > (long long)sMaxSec * 1000) {
          LOGD("PgStream: 到 %ds 上限，停止", sMaxSec);
          break;
        }
        continue;   // 立刻回去读包/取下一帧，不在这儿等
      }
      /* 没到显示时刻：往下走继续读包（这就是"不阻塞"的关键）
       * ⚠️ 例外：**无音频源没有节流器**。有音频时是"音频环满了挡调用方"把读取节流到实时；
       *    没音频时读循环会以网络/解码上限狂奔 —— 实测 10s/1MB 的片子 **301ms 就读完**
       *    （解出 7 帧就 EOF 结束），加 4ms 让步也只是变成 822ms/26 帧。
       *    正解：帧没到点时**这一圈根本不去读包**（continue），读取自然被压到显示节奏。
       *    有音频时绝对不能这么做（那会把音频环抽干 → 整链路掉到 0.4 倍实时，踩过）。 */
      if (sAudioOff && !due) {
        usleep(3000);
        continue;
      }
    }

    /* (B 之前) 读窗口：**按媒体时间**限制"读得比播得快多少"（上限 kLookaheadMs）。
     * 为什么必须有：硬解内部会缓存一堆帧，读循环顺着它一路读下去 —— 实测 10s/300 包的
     * 片子 **2 秒就把 300 个包全读完**（EOF 提前结束，只播了 1/5）。
     * ⚠️ 判据**不能用"已送包 - 已出帧"的帧数差**：那是死锁 —— 解码器要更多输入才吐下一帧，
     *    窗口一关它没输入、又永远吐不出帧（实测卡在第 1 帧、CPU 空转）。
     *    用"最近读到的视频包 PTS 是否超过 已播时间 + 1.5s"就与解码器内部行为解耦了。 */
    if (hasVideo && curPktMs >= 0 && curPktMs > (monoMs() - t0) + kLookaheadMs) {
      usleep(3000);
      continue;
    }

    /* (B) 没有挂起帧：非阻塞取一帧（取到先挂起，下一圈判断到点没）
     * ⚠️ 硬件播放器那条路**不走这里**：帧由 zk_h264_player 自己取、自己按显示节奏上屏，
     *    我们只负责把码流喂进去（见下面 (C) 里的 [HW] 分支）。 */
    if (hasVideo && !sHwVideo && !haveFrame) {
      VIDEO_FRAME_INFO_S f2;
      memset(&f2, 0, sizeof(f2));
      if (m.VDEC_GetImage(chn, &f2, 0) == SUCCESS) {
        fi = f2;
        haveFrame = true;
        /* 帧 PTS：优先取 FIFO 里最老的那个（与出帧顺序对应），空了才退回"最近读到的包" */
        if (ptsQh != ptsQt) {
          framePtsMs = ptsQ[ptsQh];
          ptsQh = (ptsQh + 1) % kPtsQ;
        } else {
          framePtsMs = curPktMs;
        }
        /* B 帧会让解码顺序的 PTS 倒退（实测 #2=200ms #3=100ms）→ 钳成单调，
         * 否则会出现"已经播到 200ms 又退回 100ms 等 100ms"的抖动 */
        if (framePtsMs >= 0 && framePtsMs < sLastSentPts) framePtsMs = sLastSentPts;
        frameWaitFrom = monoMs();
        continue;
      }
    }

    /* (C) 读一个包（会阻塞在网络上），按类型处理 */
    int r = av_read_frame(fmt, pkt);
    if (r < 0) {
      /* ---- ★★ 本地文件**连续循环**：读到 EOF 不停流，seek 回起点接着喂 ----
       * 用户口径（2026-09-20）：「播放结束重新播放不要切换画面」。
       * 走"停流 -> 再起一轮"那条老路的代价是 0.5~1.5s **可见的黑屏**
       * （收尾几十 ms~1.5s + 重新 init + 等首帧 200ms~1s），所以这里改成：
       *   seek 回起点 + 清解码器残留 + **把本轮时间基准归零** + continue。
       * 解封装线程与解码器/视频层**全程不重建** ⇒ 接缝处画面一帧都不缺。
       * ⚠️ 基准不归零 = 老毛病"整轮包一口气喂完"（见 audioBaseMs 的说明）。 */
      if (sFileLoop && r == AVERROR_EOF && !sStopReq && !sFinishReq) {
        const int sr = av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        if (sr >= 0) {
          avformat_flush(fmt);
          /* 音频解码器里的残帧必须清掉，否则新一轮开头会带上一轮的尾巴 */
          if (adec) avcodec_flush_buffers(adec);
          /* 挂起帧/在途 PTS 全部作废（挂起帧只有老路那条才有，HW 路恒为 false） */
          if (haveFrame) {
            m.VDEC_ReleaseImage(chn, &fi);
            haveFrame = false;
          }
          framePtsMs = -1;
          curPktMs = -1;
          sLastSentPts = -1;
          ptsQh = ptsQt = 0;
          sPosMs = 0;                        // 进度回 0（QA 判"新一轮开始了"用）
          t0 = monoMs();                     // 本轮墙钟起点
          const long long ac0 = audioClockMs(auAudio);
          audioBaseMs = (ac0 > 0) ? ac0 : 0;  // 本轮音频基准
          ++sFileLoops;
          sLastSeekMs = monoMs() - sLoopStartMs;   // ★ 接缝时刻（起流以来的 ms）
          LOGD("PgStream: ★ 本地文件循环第 %d 轮（seek 回起点，解码器/视频层全程未重建；"
               "已喂 %d 包 / 已出帧 %d）",
               sFileLoops, nSent, pg::H264Player::framesDecoded());
          continue;
        }
        LOGW("PgStream: 循环 seek 回起点失败 (%d) → 按原逻辑结束", sr);
      }
      LOGD("PgStream: 读到流尾/出错 (%d)，结束循环", r);
      break;
    }
    /* 进度：用包时间戳换算（只更新不前跳，避免 B 帧的 dts 抖动） */
    long long pktMs = -1;
    {
      AVRational tb = fmt->streams[pkt->stream_index]->time_base;
      int64_t st = fmt->streams[pkt->stream_index]->start_time;
      int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
      if (ts != AV_NOPTS_VALUE) {
        if (st == AV_NOPTS_VALUE) st = 0;
        long long ms = (long long)((ts - st) * av_q2d(tb) * 1000.0);
        if (ms > sPosMs) sPosMs = ms;
        pktMs = ms;
      }
    }
    if (pkt->stream_index == vi) curPktMs = pktMs;

    /* 音频包：软解 → 重采样 → 灌 PCM。**队列满就等** —— 声卡按实时消费，
     * 这一等就把整个读取循环节流到了实时速度（音频也就成了 A/V 的主时钟）。 */
    if (adec && swr && pkt->stream_index == ai) {
      if (avcodec_send_packet(adec, pkt) == 0) {
        while (avcodec_receive_frame(adec, aframe) == 0) {
          /* ★ 双声道 VU 电平：**在重采样/下混之前**按声道取 RMS（见 frameStereoRms）。
           *   放在这里而不是 PCM 出口，是因为出口那条是单声道、拿不到左右差异。 */
          if (auAudio) {
            float vl = 0.0f, vr = 0.0f;
            frameStereoRms(aframe, &vl, &vr);
            pg::Viz::noteStereoRms(vl, vr);
          }
          int outMax = (int)av_rescale_rnd(
              swr_get_delay(swr, adec->sample_rate) + aframe->nb_samples, kAudioOutRate,
              adec->sample_rate, AV_ROUND_UP);
          uint8_t *obuf = 0;
          if (av_samples_alloc(&obuf, 0, 1, outMax, AV_SAMPLE_FMT_S16, 0) >= 0) {
            int got = swr_convert(swr, &obuf, outMax,
                                  (const uint8_t **)aframe->extended_data,
                                  aframe->nb_samples);
            if (got > 0) {
              int off = 0;
              int starve = 0;   // 连续"队列不收"的次数（3ms 一次 → 150 次 ≈ 450ms）
              while (off < got && !sStopReq) {
                int n2 = auAudio->streamWrite((const int16_t *)obuf + off, got - off);
                if (n2 <= 0) {
                  if (++starve > 150) {
                    /* 声卡侧长时间不消费（被 pcmfree 释放 / 音频线程死了）→ 关掉本次音频，
                     * 否则这里会一直空转，画面也跟着卡（"音频把整条链路拖死"）。 */
                    LOGW("PgStream: 音频队列 450ms 未接收 → 关闭本次在线流音频，转为只播视频");
                    sAudioOff = 1;
                    auAudio = 0;
                    break;
                  }
                  usleep(3000);
                  continue;
                }
                starve = 0;
                off += n2;
                sAudioFrames += n2;
              }
              if (nAudioFrames++ < 3) {
                LOGD("PgStream: 音频 %d 样本 → 声卡时间轴 %lld ms", got,
                     sAudioFrames * 1000 / kAudioOutRate);
              }
            }
            av_freep(&obuf);
          }
          av_frame_unref(aframe);
        }
      }
      av_packet_unref(pkt);
      continue;
    }
    if (!hasVideo || pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
    ++nPkt;

    AVPacket *use = pkt;
    if (bsf) {
      if (av_bsf_send_packet(bsf, pkt) < 0) { av_packet_unref(pkt); continue; }
      av_packet_unref(pkt);
      if (av_bsf_receive_packet(bsf, op) < 0) continue;
      use = op;
    }

    /* ---- ★ [HW] 硬件播放器：把 Annex-B 码流喂给 zk_h264_player ----
     * 与老路的区别：**不取帧、不送显示** —— 解码、旋转、缩放、裁剪、上屏全在
     * libawh264player 内部完成（就是我们用 set_rot/set_pos/set_crop 配的那三条）。
     *
     * ⚠️ 必须自己按媒体时间节流：硬件播放器是"解出来就上屏"，没有"等音频"的机制，
     *    而我们的读循环可能领先音频 1.5s（读窗口）→ 不节流画面会跑到声音前面。 */
    if (sHwVideo) {
      /* 还在等源尺寸（直播流）：每个包都试着从 SPS 解析宽高，拿到就起硬件播放器。
       * 顺带看一眼解封装自己有没有填上（有些源会填）。 */
      if (!hwStarted) {
        if (findSpsSize(use->data, (int)use->size, &hwW, &hwH)) {
          LOGD("PgStream: [HW] 从 SPS 解析到源尺寸 %dx%d（第 %d 个视频包）", hwW, hwH,
               hwWaitPkts + 1);
        } else if (cp && cp->width > 0 && cp->height > 0) {
          hwW = cp->width;
          hwH = cp->height;
          LOGD("PgStream: [HW] 解封装给出了源尺寸 %dx%d", hwW, hwH);
        }
        if (hwW > 0 && hwH > 0) {
          int cd = pickScaleDown(hwW, hwH);
          int rx = 0, ry = 0, rw = 0, rh = 0;
          pg::H264Player::fitRect(hwW, hwH, sRotApplied, sDispX, sDispY, sDispW, sDispH, &rx, &ry,
                                  &rw, &rh);
          sHwSrcW = hwW;
          sHwSrcH = hwH;
          LOGD("PgStream: [HW] 起硬件播放器：源 %dx%d，旋转 %s，缩放解码 1/%d，显示区 "
               "(%d,%d %dx%d)",
               hwW, hwH, rotName(sRotApplied), cd ? cd : 1, rx, ry, rw, rh);
          if (!pg::H264Player::startStream(hwW, hwH, cd, sRotApplied, rx, ry, rw, rh)) {
            LOGW("PgStream: [HW] 硬件播放器起播失败 —— %s", pg::H264Player::lastError());
            if (bsf) av_packet_unref(op);
            else av_packet_unref(pkt);
            break;
          }
          hwStarted = true;
          /* 不 continue：这一包里就有 SPS/IDR，正好当起播的第一包喂进去 */
        } else {
          ++hwWaitPkts;
          if (bsf) av_packet_unref(op);
          else av_packet_unref(pkt);
          if (hwWaitPkts == 100 || (hwWaitPkts % 300) == 0) {
            LOGD("PgStream: [HW] 还在等 SPS（已读 %d 个视频包，音频在正常播）", hwWaitPkts);
          }
          if (hwWaitPkts > 900) {
            LOGW("PgStream: [HW] 等了 %d 个视频包还没拿到 SPS → 放弃本次出画", hwWaitPkts);
            break;
          }
          continue;
        }
      }
      long long clk = audioClockMs(auAudio);
      if (clk < 0) clk = monoMs() - t0;    // 无音频/音频不可用 → 退回墙钟
      else clk -= audioBaseMs;             // ★ 本地循环：折算到"本轮"时间轴
      int waited = 0;
      while (!sStopReq && !sFinishReq && curPktMs >= 0 && curPktMs > clk + kHwLeadMs &&
             waited < 80) {
        usleep(5000);
        ++waited;
        clk = audioClockMs(auAudio);
        if (clk < 0) clk = monoMs() - t0;
        else clk -= audioBaseMs;           // ★ 同上
      }
      if (pg::H264Player::feed(use->data, (size_t)use->size)) ++sHwFed;
      else ++sHwDrop;   // ★ 硬件播放器拒收（码流缓冲积压）→ 丢包（QA 判据）
      ++nSent;
      sFramesOut = pg::H264Player::framesDecoded();   // 给 videoStalled() 看
      if (bsf) av_packet_unref(op);
      else av_packet_unref(pkt);
      if (nSent == 1 || (nSent % 150) == 0) {
        LOGD("PgStream: [HW] 已喂 %d 包（硬件解码出帧 %d，解码器持有 %d[恒定=池大小，别当队列看]，"
             "需关键帧 %d，丢 %d，媒体时间 %lldms / 播放 %lldms）",
             nSent, pg::H264Player::framesDecoded(), pg::H264Player::pictureCount(),
             pg::H264Player::needIframe(), nSent - sHwFed, curPktMs, clk);
      }
      continue;
    }

    VDEC_STREAM_S st;
    memset(&st, 0, sizeof(st));
    st.pAddr = use->data;
    st.mLen = use->size;
    st.mPTS = (uint64_t)use->pts;
    st.mbEndOfFrame = 1;
    st.mbEndOfStream = 0;
    /* 码流缓冲满（0xa005800f）要**重试**，不能丢包：丢了就是花屏/卡顿，
     * 而且我们把缓冲调小了（省内存），背压会更常见。 */
    ERRORTYPE sr = SUCCESS;
    for (int tryi = 0; tryi < 40 && !sStopReq; ++tryi) {
      sr = m.VDEC_SendStream(chn, &st, 200);
      if (sr == SUCCESS) break;
      if (sr != 0xa005800f) break;   // 其它错误不重试（只记日志）
      usleep(2000);
    }
    if (sr != SUCCESS) {
      /* ⚠️ 每轮前 3 次都要报（原来是 static 计数，跨轮次只报头 5 次 → 后续轮流
       * "送包全失败"会静默无声，排查时完全看不到，踩过）。 */
      if (nSent < 3) {
        LOGW("PgStream: VDEC_SendStream -> 0x%x（第 %d 包，重试 40 次仍未过，丢弃本包）",
             sr, nSent);
      }
    }
    ++nSent;
    /* 入 PTS 队列（与出帧一一对应） */
    if (pktMs >= 0) {
      ptsQ[ptsQt] = pktMs;
      ptsQt = (ptsQt + 1) % kPtsQ;
      if (ptsQt == ptsQh) ptsQh = (ptsQh + 1) % kPtsQ;   // 满了丢最老的
    }
    if (bsf) av_packet_unref(op);
    else av_packet_unref(pkt);
  }
  /* ---- 收到停止请求后：**先把在途帧放完**（不再读包），再收尾 ----
   * 为什么必须（2026-09-13 实测）：半路把解码器拆掉/停掉，它内部会留残留
   * （`VDEC_Query` 里"左码流/左待解帧"每轮累加 ~1.25MB），后果有两种：
   *   ① 复用通道 → 下一轮**不出画**；② 直接销毁 → `DestroyChn` 永久卡住。
   * 这里只把已经解出来的帧送完显示（最多 200 帧 / 1s），解码器就"干净"了。
   * ⚠️ [HW] 硬件播放器那条路没有"我们的解码输出队列"，直接跳过去收尾。 */
  if (hasVideo && !sHwVideo) {
    int flushed = 0;
    for (int i = 0; i < 200; ++i) {
      VIDEO_FRAME_INFO_S f;
      memset(&f, 0, sizeof(f));
      if (m.VDEC_GetImage(chn, &f, 0) != SUCCESS) {
        if (i > 20) break;      // 前面几次可能还没吐出来，给一点时间
        usleep(5000);
        continue;
      }
      if (sDisp) m.VO_SendFrame(kVoLayer, kVoChn, &f, 20);
      m.VDEC_ReleaseImage(chn, &f);
      ++flushed;
    }
    if (flushed) LOGD("PgStream: 停止请求 → 放完在途帧 %d 帧（优雅停流）", flushed);
  }

  /* 退出原因见上面 userStopped 的说明：只有“真的被停止请求打断”才需要复位应用
   * （那种通道拆不干净、下一轮不出画）。自然播完（EOF/到时限）的通道是干净的，
   * 我们照常 DestroyChn 即可，不要再顺手把应用重启掉。 */
  /* ⚠️⚠️ “中途被打断”有**两条路径**，必须都算：
   *   · sStopReq（硬中止）
   *   · userStopped（优雅停止：DLNA Stop / QA streamstop → sFinishReq）
   * 只判 sStopReq 会漏掉后者，于是把“被停止过”的通道当成“自然播完”去 DestroyChn ——
   * 本板那种通道 DestroyChn **必定永久卡住**（收尾停在 ④StopRecvStream ok 之后，
   * 实测 2026-09-13；卡住后连“停止即复位”都被 running() 挡住，要等 40s 看门狗）。
   * 只有**真正播完**（EOF / 到时长上限）的通道才是干净的、可以销毁。 */
  const bool abortedMidway = (sStopReq != 0) || userStopped;
  const bool stoppedByUser = userStopped;
  /* 换台（stopForSwitch）不置复位标志：调用方马上要用这个通道播下一个源，
   * 重启应用对用户来说是"点一下频道闪回主界面"。见 PgStream.h 的 stopForSwitch。 */
  if (stoppedByUser && !sSwitchMode) sNeedsReset = 1;
  if (sSwitchMode) {
    LOGD("PgStream: 本次为**换台**停止（不触发应用复位）");
    sSwitchMode = 0;
  }
  LOGD("PgStream: 收尾 —— 送 %d 包，解出 %d 帧，用时 %lldms，可用内存 %lldkB（%s）", nSent,
       nFrame, monoMs() - t0, memAvailKb(), abortedMidway ? "中途停播" : "自然结束");
  sCleanupFromMs = monoMs();  // 收尾开始的时刻（给 running() 的看门狗用）
  sProgressMs = sCleanupFromMs;

  av_packet_free(&pkt);
  av_packet_free(&op);
  if (bsf) av_bsf_free(&bsf);
  if (auAudio) auAudio->streamOn(false);  // 关在线流音频队列（声卡流照旧跑静音 → 无 pop）
  if (swr) swr_free(&swr);
  if (adec) avcodec_free_context(&adec);
  if (aframe) av_frame_free(&aframe);
  if (hasVideo && !sHwVideo) {
    /* ⚠️⚠️ 顺序很关键（2026-09-13 实测）：**先把 VO 里的在途帧消费掉，再拆 VDEC**。
     * 我们送进 VO 的帧引用着 VDEC 的 ion 缓冲；如果先 `voStop()`（销毁 VO 通道），
     * 那些帧的引用就悬着 → VDEC 侧引用计数永远不归零 → `VDEC_DestroyChn` **永久阻塞**。
     * 所以：① flush 在途帧 → ② 给 VO 一点时间消费 → ③ 拆 VDEC → ④ 最后才 voStop。 */
    if (sDisp) usleep(150 * 1000);
    /* ⚠️ 停播（stop/中途退出）时先把解码输出抽干，再 StopRecvStream。
     * **不调 DestroyChn / SYS_Exit** —— 见上面"通道常开复用"的说明：
     * 中途停播后 DestroyChn 会永久卡住（线程再也不退出 → 播放器直到重启都不可用）。 */
    int drained = 0;
    for (int i = 0; i < 200; ++i) {
      VIDEO_FRAME_INFO_S f;
      memset(&f, 0, sizeof(f));
      if (m.VDEC_GetImage(chn, &f, 0) != SUCCESS) break;
      m.VDEC_ReleaseImage(chn, &f);
      ++drained;
    }
    if (m.VDEC_SetStreamEof) {
      ERRORTYPE e0 = m.VDEC_SetStreamEof(chn, 1);
      LOGD("PgStream: 收尾 ①SetStreamEof -> 0x%x", e0);
    }
    /* ⚠️ **排空后才停**：中间停播时解码器内部往往还压着码流/图，直接 StopRecvStream 会把它
     * 晾在半路 —— 实测后果是通道"中毒"：下一次复用调 StartRecvStream 会永久卡住
     * （三轮投屏-停播复现，日志停在 [T3] 之后）。这里用 VDEC_Query 看到
     * 「左图 0 + 左码流 0 字节 + 左待解帧 0」再往下走，最多等 2s。 */
    if (m.VDEC_Query) {
      int leftPics = -1;
      unsigned leftBytes = 0, leftFrames = 0;
      for (int i = 0; i < 400; ++i) {
        VIDEO_FRAME_INFO_S f;
        memset(&f, 0, sizeof(f));
        if (m.VDEC_GetImage(chn, &f, 0) == SUCCESS) {
          m.VDEC_ReleaseImage(chn, &f);
          ++drained;
          continue;
        }
        VDEC_CHN_STAT_S st;
        memset(&st, 0, sizeof(st));
        if (m.VDEC_Query(chn, &st) != SUCCESS) break;
        leftPics = (int)st.mLeftPics;
        leftBytes = st.mLeftStreamBytes;
        leftFrames = st.mLeftStreamFrames;
        if (st.mLeftPics == 0 && st.mLeftStreamBytes == 0 && st.mLeftStreamFrames == 0) break;
        usleep(5000);
      }
      LOGD("PgStream: 收尾 ①c 排空结果：左图 %d / 左码流 %u 字节 / 左待解帧 %u（累计抽干 %d 帧）",
           leftPics, leftBytes, leftFrames, drained);
    }
    {
      /* 空包 + EndOfStream：让收流侧知道"输入到此为止"（有些实现靠它刷尾） */
      VDEC_STREAM_S eos;
      memset(&eos, 0, sizeof(eos));
      eos.mbEndOfStream = 1;
      ERRORTYPE e1 = m.VDEC_SendStream(chn, &eos, 200);
      LOGD("PgStream: 收尾 ①b 空包EOS -> 0x%x", e1);
      usleep(80 * 1000);
    }
    LOGD("PgStream: 收尾 ②抽干解码输出 %d 帧", drained);
    m.VDEC_StopRecvStream(chn);
    LOGD("PgStream: 收尾 ③StopRecvStream ok");
    /* 解码器已排空（上面 Query 看到 0/0/0）→ 这时销毁是干净的；销毁后通道完全重建，
     * **不要复用**：实测复用通道（只 StopRecvStream 再 StartRecvStream）虽然不卡，
     * 但解码器内部残留（左码流/左待解帧 每轮累加 ~1.25MB）→ **第二轮流开始不出画**。 */
    {
      VIDEO_FRAME_INFO_S f;
      memset(&f, 0, sizeof(f));
      while (m.VDEC_GetImage(chn, &f, 0) == SUCCESS) m.VDEC_ReleaseImage(chn, &f);
      /* ⚠️⚠️ **中途停播的通道不销毁、下轮换通道号**（2026-09-13 三种方案都实测过）：
       *   ① 直接 DestroyChn → **永久卡住**（线程不退出 → 播放器直到重启不可用）；
       *   ② 排空/SetStreamEof/空包 EOS 后再 DestroyChn → 仍然卡；
       *   ③ 不复用（只 StopRecvStream 保留通道）→ 不卡，但解码器内部残留
       *      （左码流/左待解帧每轮 +1.25MB）→ **下一轮不出画**；
       *   ④ 先优雅放完在途帧再 DestroyChn → 仍然卡。
       * ⇒ 这平台的 VDEC 通道**一旦被中途停掉就处于不可回收状态**，唯一稳的做法是
       *    "把它晾着、换一个新的通道号"（通道池由驱动管理，实测同分辨率内存不增长）。
       * 自然结束（EOF/到时长上限）的通道是干净的，照旧销毁，所以正常播放不会浪费通道号。 */
      /* ⚠️⚠️ **全部常开**（2026-09-13 实测 7 种方案后的结论）：
       *   · DestroyChn（含排空/EOS/优雅放完在途帧后再 Destroy）→ **必定永久卡住**；
       *   · 复用通道（只 StopRecvStream）→ 不卡，但解码器内部残留 → 下一轮不出画；
       *   · VO 每轮 Stop/Destroy 再建 → 也会把 VDEC 的引用关系搞坏。
       * 唯一稳的是"**解码通道、VO 通道、SYS 都常开，每轮流只 Start/StopRecvStream**"，
       * 这也正是本平台的原生用法（厂商 zkmedia 退出时同样不释放 VO）。
       * 分辨率变化时才需要重建（见上面 sChnReady 分支）。 */
      m.VDEC_StopRecvStream(chn);
      LOGD("PgStream: 收尾 ④StopRecvStream ok");
      /* ★ 层释放放在这里（④ 之后、⑤ 之前）—— 这是**能走到的最早位置**：
       *   本板收尾在 ⑤（通道处理）处会永久卡住（"任务已 40s 无进展"），
       *   而视频层 `ch0/lyl0`(NV12) 在那一刻**还开着**。放在函数末尾 = 永远执行不到
       *   ⇒ 层残留 ⇒ 下一轮不出画。放这里能实打实执行到。
       *   （另外 startCommon 起播前也会清一遍，双保险。） */
      VideoLayer::release();
      if (abortedMidway) {
        /* **中途被停止**的通道拆不干净（见上面 7 种方案），既不销毁也不能复用：
         * 晾着不动，等 mainLogic 的"停止即复位"把应用重启，下次自然是全新通道。 */
        LOGD("PgStream: ⑤通道 %d 是中途停播的 → 保持不动（由\"停止即复位\"重启应用收尾）", chn);
        sChn = chn;
        sChnReady = 1;
      } else {
        /* **自然播完**（EOF / 到时长上限）的通道是干净的 —— 这时候销毁是安全的。
         * ⚠️ 必须销毁：只 StopRecvStream 留着复用的话，**下一轮同一进程里再播放会不出画**
         * （实测 `左待解帧 300` / 解出 0 帧；解码器内部有残留）。
         * 症状就是"一个视频自动播完 → 再投一个 → 黑屏"，很影响体验。 */
        m.VDEC_DestroyChn(chn);
        /* ⚠️⚠️ **绝对不要在这里调 SYS_Exit**（2026-09-13 实测踩到崩溃）：
         * VO 的层/通道在本工程是"**常开**"的（见 voStart 的说明），而 SYS_Exit 会把 MPP
         * 的全局资源一起释放 —— VO 那边还引用着 → 收尾之后**一碰 VO 就 SIGSEGV**
         * （日志 `!!FATAL!! sig=11 fault_addr=0x14`），于是每播完一个视频应用就崩一次、
         * 被 init 拉起来（表现成"pid 每轮都变"，极易误判成"复位逻辑"或 OOM）。
         * SYS_Init 只需一次，保持常开即可。 */
        sChnReady = 0;
        sChn = 0;
        LOGD("PgStream: ⑤自然结束 → DestroyChn ok（SYS/VO 保持常开，下轮重建通道）");
      }
    }
  } else if (hasVideo) {
    /* ★ [HW] 硬件播放器的收尾：**没有 MPP 通道要处理** —— 老路那一大套"排空 / EOS /
     * StopRecvStream / 通道常开或换号"的坑（见上一分支的长注释）在这条路上一个都不存在。
     * 只需把播放器停掉（它会 hide + deinit，内部释放自己的解码器和显示层）。 */
    pg::H264Player::stop();
    /* 兜底：硬件播放器退出后 disp 层不一定被 enable 清掉（老路踩过 —— 残留会让
     * "下一轮不出画"且日志全正常），这里主动释放一遍（见 PgVideoLayer.h）。 */
    VideoLayer::release();
    LOGD("PgStream: [HW] 硬件播放器已停（喂入 %d 包，硬件解出 %d 帧）", sHwFed,
         pg::H264Player::framesDecoded());
  }
  closeInput(&fmt);
  /* 旋转缓冲（MMZ）在这里还回去：它是按"当前源尺寸"申请的，换源时会重新申请，
   * 但**不还就一直是那 370KB 常驻** —— 本板内存紧（56MB），不值得为省一次分配留着。 */
  freeRotBuf();
  sRunning = 0;
  sCleanupFromMs = 0;
  LOGD("PgStream: 收尾完成（sRunning 已清零，可再次播放）");
  return 0;
}

}  // namespace

/* ==================== 抓帧（两条解码路共用，见 PgGrab.h）==================== */

const char *grabPath() { return "/tmp/pgframe.pgm"; }

bool grabTake(int frameNo) {
  if (sGrabAtFrame > 0 && frameNo >= sGrabAtFrame) {
    sGrabAtFrame = -1;   // 消费掉：只抓一帧
    return true;
  }
  return false;
}

void grabWritePlane(const unsigned char *y, int w, int h, unsigned stride, const char *tag) {
  if (!y) {
    LOGW("PgStream: 抓帧失败[%s] —— 帧没有虚拟地址", tag ? tag : "?");
    return;
  }
  if (stride == 0) stride = (unsigned)w;
  int step = sGrabStep > 0 ? sGrabStep : 4;
  if (w <= 0 || h <= 0 || stride < (unsigned)w) {
    LOGW("PgStream: 抓帧失败[%s] —— 帧几何异常 %dx%d stride=%u", tag ? tag : "?", w, h, stride);
    return;
  }
  const char *path = grabPath();
  FILE *fp = fopen(path, "wb");
  if (!fp) {
    LOGW("PgStream: 抓帧失败[%s] —— 打不开 %s", tag ? tag : "?", path);
    return;
  }
  int ow = w / step, oh = h / step;
  fprintf(fp, "P5\n%d %d\n255\n", ow, oh);
  for (int yy = 0; yy < oh; ++yy) {
    const unsigned char *row = y + (size_t)(yy * step) * stride;
    for (int xx = 0; xx < ow; ++xx) fputc(row[xx * step], fp);
  }
  fclose(fp);
  LOGD("PgStream: 已抓帧[%s] -> %s（源 %dx%d stride=%u，降采样 %d → %dx%d）", tag ? tag : "?", path,
       w, h, stride, step, ow, oh);
}

/* 真正干活：起后台线程（disp 相关状态由调用方先设好） */
bool startCommon(const char *url, int maxSeconds) {
  /* ★ 起播前先把"本轮解码帧数"清零（**此时子线程还没起，没有竞态**）。
   * 所有走 StreamPlayer 的路径（IPTV / DLNA 在线流 / streamshow）都自动获得正确语义；
   * 调用方判"出画"一律用 `pg::H264Player::framesDecodedInRun() > 0`，
   * 不要再自己拿累计值记基线（清零在子线程里，基线会读到清零前的旧值 → 负数）。 */
  pg::H264Player::armRun();
  if (StreamPlayer::running()) {   // 用带看门狗的版本（不能写 running()：类成员不会被无限定查到）
    LOGW("PgStream: 已有任务在跑");
    return false;
  }
  /* ★★ 起播前**先清一遍 disp 残留层**（用户 2026-09-14 给的做法，见 PgVideoLayer.h）。
   *
   * 为什么必须放在"起播前"而不是只放在收尾里 —— 这是实测踩出来的：
   *   本板**收尾在 ④StopRecvStream 之后就卡死**（"任务已 40s 无进展"看门狗会报），
   *   而视频层（`ch0/lyl0`，NV12）**在那一刻还开着**：收尾走不完 ⇒ 放在收尾末尾的
   *   release 永远执行不到 ⇒ 层残留 enable=1 ⇒ **下一轮再播不出画**（日志全正常，
   *   解码回调也在响，最难查的那种）。
   * 放在这里 = 不管上一轮是怎么结束的（正常停 / 卡死 / 进程被杀），这一轮都从干净状态开始、
   * 幂等、没有副作用（UI 层被双重保护跳过）。 */
  VideoLayer::release();

  /* ★★ 起流前**清页缓存**（用户 2026-09-14 给的做法，见 PgMem.h）。
   * 本板 MemTotal 只有 56MB：实测可用内存掉到 2~3MB 时，`zk_h264_player_init`
   * 会失败/进程被杀（日志停在 "[HW] 起硬件播放器" 之后，**什么报错都没有**）。
   * `echo 3 > /proc/sys/vm/drop_caches` 把页缓存/slab 还给内核，进视频前清一次最划算。 */
  dropPageCache();

  /* ⚠️ **起流前在 UI 线程拿回声卡**（本函数由 UI 线程调用：QA 命令 / DLNA 动作队列）。
   * 为什么必须在这里、而不是让播放线程自己开：实测"声卡被空闲自动让出过"时，
   * 播放线程里 openStream（dlopen 的 ALSA + 控制枚举）会把线程卡死 —— 起流后没画面没声音、
   * sRunning 永不归零。走 UI 线程这条（音效 acquire 用的是同一条）实测只要 ~13ms。 */
  if (pg::DeviceAudio *a = pg::globalAudio()) {
    if (!a->pcmHeld()) {
      long long t0 = monoMs();
      a->acquirePcm();
      LOGD("PgStream: 起流前拿回声卡（空闲让出过）耗时 %lldms", monoMs() - t0);
    }
  }
  if (!url || !url[0]) return false;
  snprintf(sUrl, sizeof(sUrl), "%s", url);
  sMaxSec = maxSeconds;
  sStopReq = 0;
  sFinishReq = 0;
  sNeedsReset = 0;   // 新一轮开始，旧的复位请求作废
  sRunning = 1;
  sProgressMs = monoMs();
  sCleanupFromMs = 0;
  pthread_t tid;
  if (pthread_create(&tid, 0, threadMain, 0) != 0) {
    sRunning = 0;
    LOGW("PgStream: 线程创建失败");
    return false;
  }
  pthread_detach(tid);
  LOGD("PgStream: 已启动在线流→硬解（最多 %ds，画面=%s）", maxSeconds, sDisp ? "开" : "关");
  return true;
}

bool StreamPlayer::startDecodeTest(const char *url, int maxSeconds) {
  sDisp = 0;
  sFileLoop = 0;      // 只解码测试：不循环（见 setFileLoop）
  return startCommon(url, maxSeconds);
}

bool StreamPlayer::startWithDisplay(const char *url, int maxSeconds, int x, int y, int w,
                                    int h, bool loop) {
  sDispX = x;
  sDispY = y;
  sDispW = w;
  sDispH = h;
  sDisp = 1;
  /* ★ 显式设置（不是"粘性开关"）：每次起流都由调用方说清楚要不要循环，
   *   否则上一个页面留下的 1 会串到下一个页面（线上搜索/投屏都不该循环）。 */
  sFileLoop = loop ? 1 : 0;
  if (!startCommon(url, maxSeconds)) {
    sDisp = 0;
    return false;
  }
  return true;
}

void StreamPlayer::setFileLoop(bool on) { sFileLoop = on ? 1 : 0; }

int StreamPlayer::fileLoops() { return sFileLoops; }

long long StreamPlayer::lastSeekMs() { return sLastSeekMs; }

int StreamPlayer::hwDrop() { return sHwDrop; }

bool StreamPlayer::running() {
  if (!sRunning) return false;
  /* 看门狗：收尾卡住超过 8s 就对外视为"已结束"。没有它的话，一次卡死会让
   * **播放器直到重启都不可用**（DLNA 后续投屏全被"已有任务在跑"拒掉，
   * 声卡的"空闲自动让出"也会被一直挡住）。这里留 WARN 便于现场发现。 */
  /* 看门狗：**任一阶段**（开流 / 建硬解 / 收尾）卡住超过 40s 就对外视为"已结束"。
   * 没有它的话，一次卡死会让播放器**直到重启都不可用**（DLNA 后续投屏全被"已有任务在跑"
   * 拒掉，声卡的"空闲自动让出"也会被一直挡住）。40s 的取法：avformat_open_input 的
   * rw_timeout 是 30s，慢站点开流本身就可能接近 30s，所以不能比它短。
   * 实测踩过的两种卡死：① 中途停播后 VDEC_DestroyChn 永久阻塞（已改成通道复用规避）；
   * ② 起流阶段偶发（日志全无、线程静默）—— 这条看门狗对两种都兜底。 */
  if (sProgressMs && monoMs() - sProgressMs > 40000) {
    static int warned = 0;
    if (warned++ < 5) {
      LOGW("PgStream: 任务已 40s 无进展（卡在开流/建链路/收尾）→ 对外视为已结束");
    }
    return false;
  }
  return true;
}
void StreamPlayer::stop() {
  /* 用户/控制器按停止 → 优雅停止（放空解码器再退）。
   * 硬中止 sStopReq 只在进程要退出这种“没得选”的场景用。
   * 流已经结束时直接忽略：收尾/收投屏页阶段也会调到这里，若把 sFinishReq 置 1
   * 会污染“退出原因”的判断（见 threadMain 里 userStopped 的说明）。 */
  if (!sRunning) return;
  sFinishReq = 1;
}

void StreamPlayer::stopForSwitch() {
  /* 换台：照常优雅停，但让收尾把这次停止当成"自然结束"（不置 needsReset），
   * 于是一次 stop→起新流 的换台不会把应用重启掉。见 PgStream.h 的说明。 */
  if (!sRunning) return;
  sSwitchMode = 1;
  sFinishReq = 1;
}

void StreamPlayer::setMaxPixels(long long px) {
  sMaxPixelsOverride = px;   // 0 = 恢复默认（kMaxDecodePixels）；语义 = **解码缓冲**像素上限
  LOGD("PgStream: 解码像素上限 -> %s%lld", px > 0 ? "" : "默认 ", px);
}

long long StreamPlayer::memAvailableKb() { return memAvailKb(); }
/* ⚠️⚠️ 上面这行**必须写限定名**（`memAvailKb`）：
 * 成员函数体里不限定的同名调用会**先查到本类自己的静态成员** ⇒ 自我递归；
 * -O2 会把这种尾递归优化成**死循环**（不爆栈、不崩溃、CPU 100%，最难查的一种）。
 * 真机踩过（2026-09-14）：QA `iptvstat` 一调 `StreamPlayer::memAvailableKb()` 就把主线程
 * 卡死在 95% CPU，界面全冻、QA 命令全不响应。同一族坑：`running()`（见 startCommon 注释）。 */

bool StreamPlayer::consumeNeedsReset() {
  if (!sNeedsReset) return false;
  sNeedsReset = 0;
  return true;
}

/* ---- 画面旋转 ----
 * ⚠️ 手动值对"正在播的这一轮"只有**复用通道**那条路生效（SetRotate）；
 * 正常情况下本工程"停止即复位应用"，所以改完 QA 后**下一次播放**必定按新角度建通道。 */
void StreamPlayer::setRotation(int deg) {
  if (deg < 0) {
    sRotManual = -1;
    LOGD("PgStream: 旋转 -> auto（跟随视频元数据；本轮流源内元数据=%s）", rotName(sRotSource));
    return;
  }
  sRotManual = ((deg % 360) + 360) % 360;
  LOGD("PgStream: 旋转 -> 固定 %s（手动值优先于视频元数据）", rotName(sRotManual));
  /* 正在用硬件播放器播 → **立刻生效**（硬件旋转，画面当场转正，不用重开流）。
   * 老路（MPP）那边是"送显示前自己转"，那是每帧都要转的软件路径，不在这里改。 */
  if (sHwVideo && pg::H264Player::streamMode()) {
    pg::H264Player::setRotation(sRotManual);
    /* 源尺寸：起播时可能还不知道（直播流）→ 用播放器解出第一帧后学到的真实尺寸 */
    int sw = sHwSrcW > 0 ? sHwSrcW : pg::H264Player::sourceW();
    int sh = sHwSrcH > 0 ? sHwSrcH : pg::H264Player::sourceH();
    int rx = 0, ry = 0, rw = 0, rh = 0;
    pg::H264Player::fitRect(sw, sh, sRotManual, sDispX, sDispY, sDispW, sDispH, &rx, &ry, &rw, &rh);
    pg::H264Player::setDispRect(rx, ry, rw, rh);
  }
}
/* ★ 视频显示链路切换（QA `streamhw <0|1>`）：1 = 硬件播放器（默认），0 = MPP 老路。
 * 下一条流起播时生效（正在播的那条不受影响）。 */
void StreamPlayer::setHwVideo(bool on) {
  sHwVideo = on ? 1 : 0;
  LOGD("PgStream: 视频链路 -> %s（下一条流起播时生效）",
       sHwVideo ? "硬件播放器 zk_h264_player（解码+旋转+缩放+裁剪一体）" : "MPP VDEC+VO（应用层软件旋转）");
}
bool StreamPlayer::hwVideo() { return sHwVideo != 0; }
int StreamPlayer::rotationDeg() {
  if (sRotManual >= 0) return sRotManual;
  if (sRotSource >= 0) return sRotSource;   // auto：源声明了角度就跟它
  return 90;   // ⚠️ 必须与 desiredRotation() 的兜底一致，否则 QA 打印会与实际生效不符
}
int StreamPlayer::sourceRotationDeg() { return sRotSource; }
void StreamPlayer::setMinAvailKb(long long kb) {
  sMinAvailOverride = kb > 0 ? kb : 0;
  LOGD("PgStream: 播放前要求的最低可用内存 -> %lldkB（0 = 默认）", sMinAvailOverride);
}
bool StreamPlayer::rotationSupported() { return mpp().VDEC_SetRotate != 0; }

void StreamPlayer::requestGrab(int frameNo) {
  sGrabAtFrame = (frameNo > 0) ? frameNo : 30;
  LOGD("PgStream: 已安排抓帧 —— 下一次播放到第 %d 帧时 dump 到 /tmp/pgframe.pgm", sGrabAtFrame);
}

bool StreamPlayer::videoStalled() {
  if (!sRunning || !sHasVideo) return false;
  if (sFramesOut > 0) return false;
  /* 音频还没流起来 → 可能只是网络/开流慢，别急着重启（音频起来了才说明数据通路好的） */
  if (sAudioFrames < 30000) return false;
  if (!sLoopStartMs) return false;
  return (monoMs() - sLoopStartMs) > 4000;
}

bool StreamPlayer::restart() {
  char u[sizeof(sUrl)];
  snprintf(u, sizeof(u), "%s", sUrl);
  if (!u[0]) {
    LOGW("PgStream: 没有可重开的地址");
    return false;
  }
  if (sRunning) {
    sStopReq = 1;
    LOGW("PgStream: 旧任务还在收尾，重开被拒（稍后再试）");
    return false;
  }
  return startCommon(u, sMaxSec);
}

long long StreamPlayer::durationMs() { return sDurMs; }
long long StreamPlayer::positionMs() { return sPosMs; }

}  // namespace pg

#endif  // FUN_BUILD
