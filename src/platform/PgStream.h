/*
 * PgStream.h - 在线流播放（ffmpeg 解封装 + 设备硬解）
 *
 * 背景：固件的 cedarx 只编了 file 流（没有 http），框架播放器也不吃 URL，
 * 所以"投屏=先下载到 /tmp 再播"会把内存吃掉。正解见 docs/online-media.md：
 *
 *     ffmpeg(网络+解封装) → 设备硬解(AW_MPI_VDEC) → 视频层显示
 *                         → 设备音频解码(AW_MPI_ADEC) → AW_MPI_AO
 *
 * 本文件是**分阶段实现**：当前完成第一阶段（解封装 + 硬解出帧），
 * 用来在真机上证实"ffmpeg 喂的流能在本芯片上被硬解"这条最难的链路。
 *
 * MPP 的取用方式：**dlopen 设备上已有的 .so**（`/lib/eyesee-mpp/libmedia_mpp.so`、
 * `libmpp_vo.so`），**不链接** —— 因为同进程里 easyui→libzkmedia 已经加载了一份
 * MPP，再静态链一份会出现两套 MPP 实例抢同一个内核驱动。dlopen 复用同一份最稳妥，
 * 而且零体积代价（和 PgAudio dlopen ALSA、PgDlna dlopen OpenSSL 是同一套路）。
 */
#ifndef PG_STREAM_H_
#define PG_STREAM_H_

namespace pg {

class StreamPlayer {
 public:
  /**
   * 后台线程跑一次"在线流 → 硬解"测试（不阻塞 UI 线程）。
   * @param url        媒体地址（http/https）
   * @param maxSeconds 最多跑多久（到点自己停；0 = 不限）
   * @return true = 线程已启动
   */
  static bool startDecodeTest(const char *url, int maxSeconds);

  /**
   * 带画面：解封装 + 硬解 + **把解码帧送进 disp 视频层**（区域 x,y,w,h）。
   *
   * ⚠️ 调用方必须先把"透明窗口"亮出来 —— V85X 上 UI 层在最顶且不透明，
   * 只有 UI 层的 videoview 区域是透明的，下层视频层的画面才能从那儿透出。
   * 本工程的透明窗口就是投屏页 `WinCast` 里的 `Caster` 控件，位置 (0,0,480,700)
   * （实测框架播放时视频层的 `frame[0,0,480,700]` 与它一致）。
   * 也就是说：**视频层由我们建，帧由我们的 ffmpeg 链路送**，UI 只提供透出区域。
   */
  static bool startWithDisplay(const char *url, int maxSeconds, int x, int y, int w, int h,
                               bool loop = false);

  /**
   * ★★ **本地文件连续循环**（`loop=true` 时生效）：读到 EOF **不停流、不重建解码器**，
   * 直接 `av_seek_frame` 回起点继续喂 —— 循环接缝处**画面一帧都不缺**。
   *
   * 为什么必须有它（2026-09-20 用户口径：「视频不连贯，播放结束重新播放不要切换画面」）：
   *   原先"播完 -> 停流 -> 再起一轮"这条路，接缝处的代价是**可见的黑屏**：
   *     ① 本页得先盖上不透明底图（否则 UI 层没有内容，透出上一层残留）；
   *     ② 等上一轮收尾（实测几十 ms ~ 1.5s）；
   *     ③ 再起流 + **等第一帧 200ms ~ 1s**（硬解要重新 init）。
   *   三段加起来 0.5 ~ 1.5s，用户看到的就是"放完黑一下再从头开始"。
   *   改成 loop 之后：解复用线程只多做一次 seek（读文件，几十 ms 内），
   *   解码器与 disp 视频层**全程不重建** ⇒ 接缝处没有空档、没有黑屏、画面连续。
   *
   * ⚠️ 三条实现约束（都在 PgStream.cpp 的循环里处理了，改代码时别漏）：
   *   ① **本轮时间基准要复位**：节流判据用的是"当前包的媒体时间 vs 已播时间"，
   *      seek 后包时间从 0 重来，不把基准归零就会变成"永远不节流" ——
   *      整轮包会被一口气喂完（实测过的老毛病：10s 的片 2s 播完）。
   *      所以 `t0` 与音频基准 `audioBaseMs` 都要在 seek 时重置。
   *   ② **音频解码器要 flush**（`avcodec_flush_buffers`），否则新一轮开头会带上一轮
   *      尾巴的残帧；音频 PCM 时间轴是**连续的**（不清 `sAudioFrames`），
   *      与声卡实际播放位置保持一致。
   *   ③ 只对**本地可 seek** 的源有意义；直播/网络流 `loop` 传 false（保持原行为）。
   */
  static void setFileLoop(bool on);

  /** 本轮起流以来**完整循环了几次**（`loop=false` 时恒为 0）。
   *  给上层做验收判据：它在涨 = 真的循环了（且 `running()` 全程为 1 = 中间没停过）。 */
  static int fileLoops();

  /** 最近一次循环 seek 发生在**本轮起流以来的第几毫秒**（-1 = 还没循环过）。
   *  ★ 它的**差分 = 一轮的实际时长** —— 这是"接缝没拖延"的最硬判据：
   *    片长 10s 时差分稳定在 ~10000ms，说明"播完立刻接上、中间没有空档/黑屏"；
   *    若接缝处停了 0.5s，差分就会变成 ~10500ms 并**逐轮累积**。 */
  static long long lastSeekMs();

  /** 硬件播放器**拒收而丢掉**的包数（`feed()` 返回 false = 码流缓冲积压）。
   *  用来把"帧计数少了"归因清楚：它在涨 = 解码侧丢帧（与接缝无关）。 */
  static int hwDrop();

  /** 是否还在跑 */
  static bool running();

  /** 请求停止（置标志，线程自己收尾） */
  static void stop();

  /**
   * **IPTV 换台专用**：停下当前流，但**不触发"停止即复位应用"**。
   *
   * 为什么需要它：`stop()` 之后调用方要靠 `consumeNeedsReset()` 复位进程
   * （本板"被停止过的解码通道"下一轮不出画，见 videoStalled 的说明）。
   * 但 IPTV 换台是**高频操作** —— 每换一个台就把应用重启一遍，用户看到的是
   * "点一下频道，整个应用闪回主界面"，不可接受。
   *
   * 语义差别：`stop()` = 用户真的要停（该复位）；
   *           `stopForSwitch()` = 我们马上要拿这个通道播下一个源。
   *
   * ⚠️ 前提是**新流要尽快跟上**（调用方负责）。如果实测发现换台后不出画，
   *    说明这个通道确实脏了，那就只能回到"复位 + 自动续播"的方案。
   */
  static void stopForSwitch();

  /** 用上一次的地址重开（DLNA 的 Pause→Play 语义：流式播放不支持暂停，
   *  只能"停掉再从头开"，所以控制器看到的会是"从头播"）。 */
  static bool restart();

  /** 媒体总时长 / 当前进度（毫秒；未知返回 -1 / 已播毫秒）。
   *  DLNA 控制器会轮询 GetPositionInfo / GetMediaInfo，需要这两个值。 */
  static long long durationMs();
  static long long positionMs();

  /** 临时调整"允许硬解的最大像素数"（QA `streammax <像素数>`；0 = 恢复默认）。
   *  默认上限见 PgStream.cpp 的 kMaxDecodePixels —— 本板 56MB 内存，
   *  1280x720 一建解码通道就触发内核 OOM（连厂商播放器也一样），所以默认拒绝。 */
  static void setMaxPixels(long long px);

  /** 当前设备可用内存（kB，-1 = 读不到）。现场判断"被 OOM 杀过之后内存回不来"用。 */
  static long long memAvailableKb();

  /** 临时改"播放前要求的最低可用内存"（QA `memguard <kB>`；0 = 恢复默认）。
   *  默认见 PgStream.cpp 的 kMinAvailKb。低于它时会**先回收页缓存**再判一次。 */
  static void setMinAvailKb(long long kb);

  /** ⚠️ **视频链路卡死自愈用**：音频已经在流（说明网络/解封装正常）但超过 N 秒
   *  一帧视频都没解出来 —— 本平台典型成因是"上一轮被手动停止过的解码通道变脏"。
   *  调用方（mainLogic 主循环）据此复位应用进程：init 托管会立刻重新拉起，
   *  MPP 状态焕然一新，用户再投一次就正常了（见 docs/online-media.md §十五）。 */
  static bool videoStalled();

  /** ⚠️ **主动复位标记**：流是"被手动停止"而结束的（不是自然播完）时置位，
   *  调用方取走后应当复位应用进程 —— 本平台"停止过的解码通道"下一轮不出画
   *  （见 videoStalled 的说明）。做成"读取即清除"，避免重复复位。 */
  static bool consumeNeedsReset();

  /* ==================== 画面旋转（竖屏设备看横屏内容） ====================
   * 背景：本机是 **480x800 竖屏**，而投屏/在线流的内容常常是横着拍的；
   * 加上手机竖拍的 MP4 会把像素存成横的、只用一个"旋转标志"声明要转多少度 ——
   * 播放器不处理那个标志，画面就是**侧躺**的。参考做法见 V851ExtendedScreen 项目
   * （它的 `zk_h264_player_init(w,h,rot,flag)` / `zk_h264_player_set_rot(disp_rot_e)`）。
   *
   * 本工程走 MPP 的 **VDEC 硬件旋转**（`AW_MPI_VDEC_SetRotate`；设备库里该符号内部
   * 会按格式选 vdeclib 或 g2d），所以是"解码器直接吐转正的帧"，不占额外内存/CPU。
   *
   * 角度语义：**顺时针** 0/90/180/270，与 `ROTATE_E` 和参考项目 `disp_rot_e` 一致。
   *
   * 默认策略 = **auto**：读容器元数据（MP4 的 displaymatrix，也就是手机上那个
   * "竖拍视频"标志）；读不到就 0°。竖拍视频自动就正了。
   * QA `streamrot <0|90|180|270|auto>` 可手动指定（手动值优先于元数据，进程内有效）。*/
  static void setRotation(int deg);        // 0/90/180/270 = 手动；-1 = 回到 auto
  static int rotationDeg();                // 当前生效角度（auto 且未开流时返回 -1）
  static int sourceRotationDeg();          // 最近一次开流从元数据读到的角度（-1 = 无）
  static bool rotationSupported();         // 设备库里有没有那个符号（现场判断用）

  /**
   * ★★ 视频显示链路切换（默认 **true = 硬件播放器**）。
   *
   *   true  = `zk_h264_player`（libawh264player）：解码 + **硬件旋转** + 缩放 + 裁剪一体。
   *           `set_pos` 决定画面在屏幕上放哪、多大；`set_rot` 决定角度；
   *           `set_crop` 决定裁源画面的哪一块 —— 这三个都是硬件干的，零 CPU 代价。
   *   false = MPP 老路（AW_MPI_VDEC + AW_MPI_VO + **应用层 CPU 软件旋转**）。
   *
   * 为什么默认切成硬件：老路那条软件旋转把 VDEC 输出的 NV12 当 I420 三平面拆开转
   * （色度全乱 → 大片纯绿），转出的 256x448 缓冲又配在 frame=480x700 的层上
   * （显示引擎按 480 宽读 256 宽的缓冲 → 画面横向重复）。这就是"IPTV 错屏"的两个根因。
   *
   * QA `streamhw <0|1>` 可现场切回老路做对照；切换在**下一条流起播时**生效。
   */
  static void setHwVideo(bool on);
  static bool hwVideo();

  /** 抓一帧画面存成 `/tmp/pgframe.pgm`（PGM=P5 灰度，PC 侧可直接读）。
   *  ⚠️ 视频层不在 `/dev/fb0`（那是 UI 层），**截图抓不到播出来的画面** ——
   *  要确认"旋转方向对不对"只能用这个把帧搬出来。`frameNo<=0` 用默认（第 30 帧）。 */
  static void requestGrab(int frameNo);
};

}  // namespace pg

#endif  // PG_STREAM_H_
