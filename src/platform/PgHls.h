/*
 * PgHls.h - HLS（m3u8）客户端：自实现简易拉流 + 本地 HTTP 中继
 *
 * ============================ 为什么需要它 ============================
 * 本板固件自带的 ffmpeg（`src/dependencies/lib/libavformat.a`）**没有编 HLS 解封装**：
 *   `nm --defined-only libavformat.a | grep -i hls` → 0 个符号，
 *   喂 m3u8 进去实测报 `Invalid data found (-1094995529)`。
 * 而 IPTV 源绝大多数就是 HLS（m3u8 + ts 分片），所以"直接播 m3u8"这条路不通。
 *
 * 换带 HLS 的 ffmpeg 需要交叉重编（裁剪参数靠猜，风险最高）；
 * 本文件走**自实现**：HLS 本身是个很薄的文本协议 ——
 *   m3u8 就是个分片清单文本，按清单顺序把 .ts 下载下来**首尾拼接**，
 *   拼出来的字节流就是一条连续的 MPEG-TS 流。
 * 所以只要"拉清单 + 下分片 + 拼流"，剩下的全部复用工程里**已验证**的链路。
 *
 * ============================ 数据流 ============================
 *   m3u8(文本) ──解析──> 分片 URL 列表 ──顺序下载 .ts──> 环形缓冲
 *                                                          │
 *                              ┌───────────────────────────┘
 *                              ▼
 *        `Hls::readBuffer()`（阻塞读，读到数据或超时才返回）
 *                              │
 *                              ▼
 *   pg::StreamPlayer::startWithDisplay("pg-hls://live", …)
 *        └─ ffmpeg 用**自定义 AVIO** 直接从环形缓冲取字节（mpegts 解封装照旧）
 *           → 设备硬解 → disp 视频层
 *
 * 为什么是"自定义 AVIO"而不是原来的**本地 HTTP 中继**（2026-09-14 改）：
 *   老做法：起一个 127.0.0.1:8199 的 HTTP 服务，把环形缓冲吐给 ffmpeg。
 *   它当年是为了"StreamPlayer 完全不动"（那时还没真机验过），代价是多一层本地 TCP
 *   + 一次内存拷贝 + 一个监听端口 + accept/断连/超时那一套。链路稳定后这层就是纯开销，
 *   而且它的"起播读指针对齐"逻辑跟缓冲耦合在一起，反而更难排查。
 *   现在：ffmpeg 仍然负责解封装（mpegts / 音频 / 视频分流全不动），只是**字节从哪来**
 *   换成了我们的读回调 —— 少一层、少一次拷贝、少一个端口。
 *   环形缓冲仍然保留：它负责"网络抖动"与"下载/解码速度不匹配"的缓冲。
 *
 * ============================ 直播语义 ============================
 * 直播 m3u8 是**滚动窗口**：清单里只有最近 N 个分片，老分片会被挤掉。
 *   ① 起播：从清单里**倒数第 kStartFromBack 个**分片开始下（留一点追赶余量），
 *      攒够 kPrebufferSegments 个再对外服务，避免 ffmpeg 探测阶段读到 EOF；
 *   ② 续拉：记录已下过的分片（`#EXT-X-MEDIA-SEQUENCE` + 下标），只下新的；
 *   ③ m3u8 自己也要**反复重拉**（分片列表会更新）。
 *
 * 已知不支持（遇到就明确报错，不静默失败）：
 *   · `#EXT-X-KEY METHOD=AES-128` 加密流（要再加 AES-128 解密，本板还没接）
 *   · fMP4（`#EXT-X-MAP` / `.m4s` 分片）—— 本板硬解链路走 mpegts，先只认 .ts
 */
#ifndef PG_HLS_H_
#define PG_HLS_H_

namespace pg {

class Hls {
 public:
  /**
   * 启动 HLS 拉流 + 本地中继（幂等：已在跑会先停掉旧的）。
   * @param m3u8Url 清单地址（http/https）
   * @param out     输出**播放地址**：固定写 `pg-hls://live`（自定义 AVIO 的约定前缀，
   *                见 PgStream::openInput —— 它看到这个前缀就用 Hls::readBuffer 喂字节），可为 0
   * @param n       out 的容量
   * @return true = 已启动（此时 out 里是可以直接交给 StreamPlayer 的地址）
   */
  static bool start(const char *m3u8Url, char *out, int n);

  /** 停止：断客户端、停两个线程、清缓冲（幂等） */
  static void stop();

  static bool running();

  /* 清单解析状态（★ 异步：`start()` 不再同步拉清单）。
   *   0 = 正在解析（还没拿到清单）
   *   1 = 清单已解析，进入拉流（可以认为"频道可用"）
   *   2 = 解析失败（原因看 lastError()）
   * 为什么异步：`start()` 里那次同步 httpFetch 是**秒级阻塞**（死源要等 http 超时），
   * 压在 UI 线程上就是"点了频道整个界面僵住"—— 连"正在连接"四个字都画不出来。
   * 现在清单交给下载线程的第一轮 pullOnce 去拉，start() 只做本地准备（立刻返回）。 */
  static int openState();

  /**
   * 给 ffmpeg 的**自定义 AVIO 读回调**用：从环形缓冲取最多 n 字节。
   *
   * 语义（原来在本地中继的 serveClient 里，现在搬到这里）：
   *   · 第一次调用（还没有读者）会把读指针对齐到**当前分片的起始位置** —— 那里必有
   *     SPS/PPS + IDR，否则 ffmpeg 要等下一个 IDR 才出画面（实测：音频先响、视频 20 秒
   *     才出，看着像"缓冲底图没隐藏"）；
   *   · 没数据就在条件变量上睡，最多等 timeoutMs；
   *   · 返回本次读到的字节数；**0 = 流已停 / 超时**（调用方按 EOF 处理，让上层退出）。
   */
  static int readBuffer(unsigned char *out, int n, int timeoutMs);

  /* ---------------- 状态（UI 显示 / 自检用） ---------------- */

  /** 已下载并写入缓冲的分片数 */
  static int segments();

  /** 已产出到缓冲的字节数 */
  static int bytes();

  /** 选中的档位描述，如 "854x480 bw=1821600"（media playlist 直接给分片时为 "-"） */
  static const char *variant();

  /** 一句话状态："解析清单 / 缓冲中(1/3) / 拉流中 / 客户端已连 / 已停止 / 出错" */
  static const char *status();

  static const char *lastError();

  /** 播放器是否正在读缓冲（正常播放时为 1；原来叫"本地中继客户端数"） */
  static int clients();

  /** 原 m3u8 地址（自检/日志用） */
  static const char *sourceUrl();

  /**
   * 选流档位上限（bit/s）：master playlist 里**只挑带宽不超过它**的档位，
   * 并从中挑**最高**的一档；0 = 自动（挑最低档，最省内存）。
   * 本板 55MB 内存、硬解上限 960x544（见 PgStream.cpp kMaxDecodePixels），
   * 所以默认挑最低档是对的；放开只是为了现场标定。
   */
  static void setMaxBandwidth(int bps);
  static int maxBandwidth();
};

}  // namespace pg

#endif  // PG_HLS_H_
