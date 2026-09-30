/*
 * PgH264.h - 硬件 H264 直接解码（zk_h264_player / libawh264player.so）
 *
 * ============================ 它是什么 ============================
 * 参考工程（`S:\projects\LearningProject\V851ExtendedScreen_ap_p2p`）里那套
 * `src/media/h264_player.h` 就是本文件的封装对象。分两层：
 *
 *   `zk_h264_player_*`（我们调用的）
 *        ↑ 实现在 `src/dependencies/lib/libzkmedia.a` 的 `h264_player.o` —— 一个
 *          **dlopen wrapper**：`dlopen("libawh264player.so")` + `dlsym` 取真符号。
 *        ↓
 *   `h264_player_*`（真实现，在 `src/dependencies/lib-no-link/libawh264player.so`）
 *        ↓ 依赖（NEEDED）
 *   `libvdecoder.so` / `libVE.so` / `libhwdisplay.so` / `libMemAdapter.so` /
 *   `libcdc_base.so` / `libcdx_base.so` / `libcdx_common.so` / `libvideoengine.so`
 *        ↑ **本板设备上都在 `/lib/eyesee-mpp/`**（实测），而这个目录已经在
 *          设备的 `/etc/ld-musl-armhf.path` 里 ⇒ dlopen 的依赖能被找到，不用额外
 *          设 `LD_LIBRARY_PATH`。
 *
 * ⚠️ 部署注意：`lib-no-link/` 里的 .so **不随 `fun launch` 推送**（官方 wiki：
 *    "该文件夹下的动态链接库仅随程序打包，不参与编译"）。设备上要能 dlopen 到它，
 *    得让文件落在 ld 搜索路径里 —— 本板 `LD_LIBRARY_PATH` 含 `/data` 与 `/tmp`。
 *
 * ============================ 为什么用它 ============================
 * 本工程**已有**一条在线流链路（`pg::StreamPlayer`：ffmpeg mpegts + AW_MPI_VDEC 硬解），
 * 也已经真机验收。那为什么还要这套？**因为内存**：
 *
 *   StreamPlayer 走 MPP 建解码通道，`kMaxDecodePixels = 960*544`（≈52 万像素）是实测
 *   硬上限 —— 720p（92 万）一建通道就触发内核 OOM（本板 MemTotal 只有 55MB）。
 *
 * 而这一套的 `E_H264_PLAYER_FLAG_SCALE_DOWN_2 / _4` 提供 **1/2、1/4 缩放解码**
 * （参考工程就是拿它把大分辨率投屏压到屏幕上播的）。480x800 的屏上播 720p 缩一半
 * 完全够看，**这才是绕开 960x544 上限的正路**。
 *
 * ============================ 用法 ============================
 *   H264Player::playFile("/tmp/x.h264", srcW, srcH, 2, 0, 0, 480, 700);
 *   H264Player::stop();
 *
 * ⚠️ 与 `StreamPlayer` **互斥**（同一个 disp 视频层、同一颗 VE）—— 调用方负责保证
 *    在起这个之前 `StreamPlayer` 已经停干净。
 */
#ifndef PG_H264_H_
#define PG_H264_H_

#include <stddef.h>   // size_t（stream feed 用）
#include <stdint.h>   // uint8_t

namespace pg {

class H264Player {
 public:
  /**
   * 播一段 H264 裸流（Annex-B）文件，画面送到 disp 视频层。
   * ⚠️ 调用方必须先亮出"透明窗口"（本工程是 IPTV 播放页 / 投屏页的 videoview）。
   *
   * @param path       H264 ES 文件路径（Annex-B；需含 SPS/PPS/IDR 才能起播）
   * @param srcW/srcH  **源分辨率**（zk_h264_player_init 的 w/h）
   * @param scaleDown  0=不缩放；2=1/2；4=1/4（对应 FLAG_SCALE_DOWN_2/4）
   * @param dispX/Y/W/H 屏幕上显示区域
   * @return true = 已起（后台线程在喂帧）
   */
  static bool playFile(const char *path, int srcW, int srcH, int scaleDown, int dispX,
                       int dispY, int dispW, int dispH);

  /* ==================== ★ 流式播放（IPTV / 在线流）====================
   * 与 playFile 的区别：帧**不是**从文件读，而是由外部（ffmpeg 解封装线程）持续
   * `feed()` 喂进来 —— 直播流是无限的。
   *
   * ⚠️⚠️ 旋转/定位/裁剪**全部交给硬件播放器**（`zk_h264_player_set_rot/pos/crop`）。
   * **不要**在应用层拿 CPU 转帧：上一版就是那么干的（`PgStream::rotateInto` 把
   * VDEC 输出的 NV12 当 I420 三平面拆开转），结果是色度全乱（大片纯绿）+ 几何错配
   * → 用户看到的"错屏"。硬件播放器这套 API 本来就是干这个的（参考工程
   * V851ExtendedScreen 的 `video_rot/video_show/video_crop` 三个调用一一对应）。*/
  static bool startStream(int srcW, int srcH, int scaleDown, int rotDeg, int dispX, int dispY,
                          int dispW, int dispH);

  /** 喂一段 Annex-B 码流（一个包/多个 access unit 都行）。
   *  @return false = 这次没喂进去（解码缓冲积压，已丢；下一个 I 帧会自动恢复） */
  static bool feed(const uint8_t *data, size_t size);

  /** 运行中改旋转角度（0/90/180/270，**顺时针**；负值/非 90 倍数按 0 处理）。
   * 转完后会按新角度重算"整帧裁剪"，显示位置由调用方用 setDispRect 跟新。 */
  static void setRotation(int deg);
  static int rotation();

  /** 运行中改显示区域（屏幕坐标；硬件负责把画面缩放到这个矩形） */
  static void setDispRect(int x, int y, int w, int h);

  /** 源裁剪（**旋转后**的坐标系；w/h <= 0 = 用整帧）。
   * 语义与参考工程一致：内部会按缩放解码倍率折算成解码缓冲坐标。 */
  static void setSourceCrop(int x, int y, int w, int h);
  static void clearCrop();

  /** 等比"放进"显示区（不裁掉内容，多余处留黑边）→ 算出应该 set_disp 的矩形。
   *  srcW/srcH = 源尺寸，rotDeg = 旋转角度，area* = 可用显示区（本工程 0,0,480,700）。*/
  static void fitRect(int srcW, int srcH, int rotDeg, int areaX, int areaY, int areaW, int areaH,
                      int *outX, int *outY, int *outW, int *outH);

  /** 当前是不是"流式模式"（streamstop 之类要判它） */
  static bool streamMode();

  /** 当前生效的源尺寸（直播流起播时是 0，解出第一帧后才有值） */
  static int sourceW();
  static int sourceH();

  /** zkmedia 码流缓冲（`ZKMEDIA_H264_VBVSIZE`）字节数。默认 1MB。
   *  ⚠️ **必须盖掉库的默认小值**：不设的话 720p（1/2 缩放解码）+ 旋转起播会让进程静默退出
   *  （见 docs/h264-direct.md 的"必须设"一节）。改完要**重启应用**才生效（库 dlopen 时读）。 */
  static void setVbvBytes(int n);
  static int vbvBytes();

  /** 停止并释放（幂等） */
  static void stop();

  static bool running();

  /** 已喂进去的 access unit 数 */
  static int framesFed();

  /**
   * **解码帧回调被触发的次数** —— 判断"硬解是不是真的出画"的唯一硬证据。
   * 只看 pictureCount() 会误判（它更像"已提交帧数"，喂多少报多少）。
   * 这个值在涨 = 硬件解码器确实在吐帧；一直是 0 = 解码器没吃进去（或没起）。
   */
  static int framesDecoded();
  /* ★ "本轮"解码帧数（自 armRun() 起）。为什么需要它：
   *   `framesDecoded()` 是**进程累计**值，而 `startStream()` 会在**子线程**里把它清零 ——
   *   调用方若自己记基线（"起播前的累计值"）就有竞态：基线可能读到清零前的旧值，
   *   于是"本轮帧数 = 累计 - 基线"变成**负数**，判据永远不成立（实测踩到：
   *   IPTV 出画判据失效，视频其实 1 秒就出帧了，界面却等了 20 秒才收起底图）。
   * 用法：**起播发起之前**（此时还没有子线程）调 `armRun()`，之后判
   *   `framesDecodedInRun() > 0`。清零动作由 PgStream::startCommon 统一做，无竞态。 */
  static void armRun();
  static int framesDecodedInRun();

  /** 解码器里待显示的帧数（zk_h264_player_get_picture_count） */
  static int pictureCount();

  /** 解码器当前是否在等关键帧（1 = 等，粘包/丢包后为 1） */
  static int needIframe();

  /** 文件里一共切出多少个 access unit（起播前就知道"该有多少帧"） */
  static int totalUnits();

  static const char *lastError();

  /** 把最近一次 init 用的参数打成一行（自检/日志用） */
  static const char *describe();
};

}  // namespace pg

#endif  // PG_H264_H_
