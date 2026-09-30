/*
 * PgFf.h - 在线多媒体（ffmpeg 组件包）
 *
 * 为什么需要它（这是本项目最重要的一次架构修正）：
 *   本板固件自带的 cedarx 媒体栈 **只编了 file 流** ——
 *   `/lib/eyesee-mpp/libcdx_stream.so` 里只有 `__FileStreamConnect` 一个流实现，
 *   未定义符号只有 close/dup/lseek（连 socket 都没有）⇒ **没有任何 http/https 能力**。
 *   所以：
 *     · 框架的 `ZKVideoView::play()` 只认本地文件（丢 URL 进去直接报
 *       `fatal error! file[...] is not exist!`，实测）；
 *     · `aw-mpp` 的 `DEMUX_CHN_ATTR_S` 虽然有 `SOURCETYPE_URL`，但底层流层没实现 http，
 *       喂 URL 一样打不开。
 *   ⇒ 之前的做法只能是"整片下载到 /tmp 再播"，而 /tmp 是 **tmpfs（就是内存）**，
 *     片子一大必然炸内存 —— 这就是这条路走不通的根本原因。
 *
 *   **ffmpeg 组件包（v85x 专版 4.1.9-configure4）才是官方的在线多媒体方案**，
 *   它的 configure 实测包含：
 *     · protocols: file http https rtp tcp udp tls   ← 网络协议齐全
 *     · external : openssl                            ← TLS 真编进去了
 *     · demuxers : mov mp4 mpegts rtsp asf(flv) rm ogg mp3 aac flac wav
 *     · bsfs     : h264_mp4toannexb（MP4 的 H264 转 Annex-B，正好喂硬件解码）
 *   所以正确的播放链路是：
 *
 *     控制器给的 URL
 *       → avformat_open_input(url)      // ffmpeg 自己走 http/https（含 TLS、证书、时间）
 *       → av_read_frame()               // 拿到一帧帧编码数据（H264/H265/AAC…）
 *       → h264_mp4toannexb 转 Annex-B   // 硬件解码器要的是这种裸流
 *       → AW_MPI_VDEC_SendStream()      // 设备硬解
 *       → 视频层显示（VideoView 控件的显示区域）
 *
 *   **内存占用恒定**（只有解封装缓冲），不再整片落 /tmp。
 *
 * 当前进度：本文件先落地"**探测**"能力（等价于 ffprobe），用来在真机上证明
 *   "能打开在线流 + 拿到流信息"。它是后面的解封装/喂硬解的基础。
 */
#ifndef PG_FF_H_
#define PG_FF_H_

namespace pg {

class Ff {
 public:
  /**
   * 打开在线流（http/https/rtsp…）并取流信息，等价于一次 ffprobe。
   * @param url  媒体地址
   * @param out  接收摘要（容器/时长/码率/各条流的编码与分辨率），可为 0
   * @param n    out 的容量
   * @return true = 成功打开并拿到流信息
   */
  static bool probe(const char *url, char *out, int n);

  /**
   * **单次、短超时**的探测：只回答"这个地址能不能打开"。
   *
   * 为什么需要它：`probe()` 是 30s 超时 × 2 次重试（那是为了容忍开机后第一次
   * https 的冷缓存握手）—— 拿它去**批量试候选地址**的话，10 个候选就是 10 分钟，
   * 界面上看着像卡死。局域网摄像头扫地址要的是"快问快答"：
   * 连不上/拒绝/401 都是**立刻**返回的，只有"没有这台主机"才会耗到超时。
   *
   * @param timeoutMs 单次 rw_timeout（毫秒；<=0 用 3000ms）
   * @return true = 打开成功（成功也会立刻关掉，只做判定，不取流信息）
   */
  static bool probeFast(const char *url, int timeoutMs);

  /** ffmpeg 的网络子系统是否已初始化（首次 probe 时初始化） */
  static bool inited();

  /** 确保 ffmpeg 已初始化（网络 + 按需注册 + 关 IPv6）；给其它模块复用 */
  static void ensureReady();

  /** 当前用于 https 校验的 CA 证书路径（没有则空串） */
  static const char *caPath();

  /** 开关 https 证书校验（排障用；默认开） */
  static void setVerify(int on);

  /** 关掉 IPv6（本板无 IPv6 出口，而 ffmpeg 只试 getaddrinfo 的第一个地址）。
   *  首次 probe 时自动调用；也可以在启动时主动调一次。 */
  static void disableIpv6();
};

}  // namespace pg

#endif  // PG_FF_H_
