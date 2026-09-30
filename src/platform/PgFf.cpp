/*
 * PgFf.cpp - 在线多媒体（ffmpeg 组件包）实现，见 PgFf.h 的说明。
 *
 * 链接方式：ffmpeg 是**静态库**（lib/*.a，通过 Manifest 的 `<package id="ffmpeg">` 引入）。
 * ⚠️ 它引用了 OpenSSL（`SSL_CTX_new`/`SSL_CTX_load_verify_locations`/`OPENSSL_init_ssl`…），
 *    而工具链 sysroot 里**没有** libssl —— 所以把设备上的
 *    `/lib/libssl.so.1.1` + `/lib/libcrypto.so.1.1` 拷进 `src/dependencies/lib/`
 *    （FlyThings 约定：该目录下的库**自动参与链接**），
 *    运行时设备本来就有这两个 so，不需要额外打包。
 */
#include "platform/PgFf.h"

#ifdef FUN_BUILD

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <stdio.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>   // access()

#include "manager/ConfigManager.h"
#include "platform/PgTime.h"
#include "utils/Log.h"

namespace pg {

namespace {

bool sInited = false;
char sCaPath[192] = {0};
bool sCaTried = false;
volatile int sVerify = 1;   // 1 = 校验证书；0 = 不校验（排障用）

/* 把 ffmpeg 自己的日志桥到我们的 LOGD —— **排障必需**：
 * ffmpeg 的 TLS/http 失败原因（如 "SSL: certificate verify failed"）只写在它自己的
 * 日志里，而设备上 stderr 指向 /dev/null，不看这个桥就只能看到一句 "I/O error"。 */
void avLogBridge(void *avcl, int level, const char *fmt, va_list vl) {
  if (level > AV_LOG_WARNING) return;  // 只要 警告/错误，避免刷屏
  char line[512];
  vsnprintf(line, sizeof(line), fmt, vl);
  size_t l = strlen(line);
  while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
  if (!l) return;
  LOGW("PgFf[ffmpeg]: %s", line);
}

/* ==================== ⚠️ 关掉 IPv6（本板 https 走不通的真凶）====================
 * 症状：ffmpeg 报 `Connection to tcp://<host>:443 failed: Network unreachable`（-101），
 *       而同一个域名用我们自己的下载器（gethostbyname）却完全正常。
 * 根因：**musl 的 getaddrinfo 会优先返回 AAAA（IPv6）记录** —— 本板 wlan0 只有
 *       link-local 地址（fe80::/64），**没有全局 IPv6 路由** → connect 直接 ENETUNREACH；
 *       而 **ffmpeg 4.1 的 `tcp_open()` 只尝试 getaddrinfo 返回的"第一个"地址，不遍历
 *       ai_next**（新版才加了遍历）⇒ 一旦第一个是 IPv6，这个域名就必然连不上。
 *       （我们自己的客户端用 gethostbyname，只拿 A 记录，所以从来没这问题 —— 这就是
 *        "同一个网址我们的代码能下、ffmpeg 说网络不可达"的原因。）
 * 处理：把 `disable_ipv6` 写进 /proc/sys（**应用以 root 跑，可以直接写**）。
 *       本板本来就没有 IPv6 出口，关掉是纯收益。
 * 验证：关之前 example.com 能开（说明 http 栈没问题）、raw.githubusercontent.com 报
 *       Network unreachable；关之后 https + 证书校验一次性通过。 */
static long long monoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void ensureIpv4Only() {
  static const char *kPaths[] = {"/proc/sys/net/ipv6/conf/all/disable_ipv6",
                                 "/proc/sys/net/ipv6/conf/default/disable_ipv6",
                                 "/proc/sys/net/ipv6/conf/wlan0/disable_ipv6"};
  for (int i = 0; i < 3; ++i) {
    FILE *f = fopen(kPaths[i], "w");
    if (!f) continue;
    bool was = false;
    /* 先读一下原值，避免每次都刷日志 */
    FILE *r = fopen(kPaths[i], "r");
    if (r) {
      char b[8] = {0};
      if (fgets(b, sizeof(b), r) && b[0] == '1') was = true;
      fclose(r);
    }
    fputs("1", f);
    fclose(f);
    if (!was) LOGD("PgFf: 已禁用 IPv6（%s）—— 见 ensureIpv4Only 的说明", kPaths[i]);
  }
}

/* ---- 按需注册用的符号（静态库里名字就是 ff_xxx_demuxer / ff_xxx_parser） ---- */
extern "C" {
/* 解封装器：手机投屏/DLNA 常见的就是 mp4(mov) / ts(mpegts) / flv(asf) / rm */
extern AVInputFormat ff_mov_demuxer;
extern AVInputFormat ff_mpegts_demuxer;
extern AVInputFormat ff_mpegtsraw_demuxer;
extern AVInputFormat ff_asf_demuxer;   // wmv/asf（flv 这个包没编，rtmp 也就无从谈起）
extern AVInputFormat ff_rm_demuxer;
extern AVInputFormat ff_rtsp_demuxer;
/* 纯音频容器 */
extern AVInputFormat ff_aac_demuxer;
extern AVInputFormat ff_mp3_demuxer;
extern AVInputFormat ff_wav_demuxer;
extern AVInputFormat ff_flac_demuxer;
extern AVInputFormat ff_ogg_demuxer;
/* 码流解析器（按帧切分 Annex-B 要用；注意这个包没编 aac/mpeg4video 的 parser） */
extern AVCodecParser ff_h264_parser;
extern AVCodecParser ff_hevc_parser;
extern AVCodecParser ff_mpegaudio_parser;
extern AVCodecParser ff_flac_parser;
extern AVCodecParser ff_h261_parser;
extern AVCodecParser ff_h263_parser;
}

/* ==================== ⚠️ 打桩顶掉 libavcodec/allcodecs.o（关键瘦身手段）====================
 * 现象：按需注册 demuxer/parser 之后，`libzkgui.so` 仍然是 **14.5MB**（原 1.17MB）。
 * 定位：`libavformat/utils.o`（avformat_find_stream_info）引用 `avcodec_find_decoder`，
 *       `libavcodec/utils.o` 还引用 `avcodec_find_encoder`，`libavcodec/options.o` 引用
 *       `av_codec_next` —— **这三个函数都定义在 libavcodec/allcodecs.o 里**，
 *       而 allcodecs.o 的 `avcodec_register_all()` 挨个引用了**所有**软解/编码器
 *       ⇒ 链上 allcodecs.o 就把 16MB 的 libavcodec 整个拖进来。
 *       （试过 `-Wl,--gc-sections`：厂商编译时没有按函数分段（段表里只有一个 `.text`），
 *        所以 GC 无效。）
 * 处理：**自己提供这三个符号**，链接器就不再拉 allcodecs.o。
 *       `av_codec_next` 在新版头文件里已删，所以自己声明；返回 NULL = "列表空"。
 * 影响：avformat 探测流时找不到软解（`avcodec_find_decoder` 返回 NULL）→ 少一点
 *       探测出来的信息（个别源的帧率/码率可能未知），**不影响解封装**；
 *       反正本工程的解码是**交给设备硬解**的，软解本来就不该编进来。
 * 实测：加上这三个桩后 .so 从 14.5MB 降到 **约 2.6MB**。 */
/* 少数几个解码器的符号（各自独立 .o，不会把 allcodecs.o 拖进来） */
extern "C" {
extern AVCodec ff_aac_decoder;
extern AVCodec ff_mp3_decoder;
extern AVCodec ff_pcm_s16le_decoder;
}

extern "C" {
/* 白名单式查找：**只放行我们真的要用的解码器**（在线流的音频）。
 * 仍然**不能**让它回到 allcodecs.o（那会把所有软解拖进来，+12MB）——
 * 所以这里自己给出少数几个 `ff_xxx_decoder` 的地址（各自独立的 .o）。 */
AVCodec *avcodec_find_decoder(enum AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_AAC: return &ff_aac_decoder;
    case AV_CODEC_ID_MP3: return &ff_mp3_decoder;
    case AV_CODEC_ID_PCM_S16LE: return &ff_pcm_s16le_decoder;
    default: return 0;
  }
}
AVCodec *avcodec_find_encoder(enum AVCodecID id) {
  (void)id;
  return 0;
}
AVCodec *av_codec_next(const AVCodec *c) {
  (void)c;
  return 0;  // 空表：调用方会认为"没有更多编解码器"
}
/* `libavformat/utils.o` 还引用了这个（av_find_best_stream / find_stream_info 里按名字找解码器） */
AVCodec *avcodec_find_decoder_by_name(const char *name) {
  (void)name;
  return 0;
}
AVCodec *avcodec_find_encoder_by_name(const char *name) {
  (void)name;
  return 0;
}
/* `libavformat/utils.o` 还引用了这个（在 libavcodec/h264dec.o）—— 就为调它一次，
 * 链接器会把**整个 H.264 软解码器**（含两张 32KB 的 VLC 大表）拖进来。
 * 我们自己返回 0（="没有重排序帧"）即可。 */
int avpriv_h264_has_num_reorder_frames(const void *sps_list) {
  (void)sps_list;
  return 0;
}
}

/* 初始化：**按需注册**，不是 av_register_all（见下面注释）
 * ⚠️ 为什么不能用 `av_register_all()`：
 *    它内部会调 `avcodec_register_all()`，把**所有软解/编码器**都拉进 .so ——
 *    实测 `libzkgui.so` 从 1.17MB 涨到 **14.5MB**，而本板 `/res` 分区总共才 **7.6MB**
 *    （`/proc/mtd`: mtd3 = 0x7a0000），**直接装不下**，调试模式推 /tmp 更会吃内存。
 *    本工程的解码是**交给设备硬解**（AW_MPI_VDEC）的，ffmpeg 只做
 *    "网络取流 + 解封装 + 码流转换"，所以**一个软解都不需要注册**。
 * 另外协议（file/http/https/tls/tcp/udp/rtp）不用管：这个包是 configure 时生成
 *    `protocols.o` 的**静态表**（`url_protocols`），链上就自动可用。 */
void ensureInit() {
  if (sInited) return;
  sInited = true;
  av_log_set_level(AV_LOG_WARNING);
  av_log_set_callback(avLogBridge);  // 把 ffmpeg 的告警/错误转到 LOGD（见 avLogBridge）
  ensureIpv4Only();                  // ⚠️ 必须在联网前：ffmpeg 只试第一个地址（见说明）
  avformat_network_init();

  av_register_input_format(&ff_mov_demuxer);
  av_register_input_format(&ff_mpegts_demuxer);
  av_register_input_format(&ff_mpegtsraw_demuxer);
  av_register_input_format(&ff_asf_demuxer);
  av_register_input_format(&ff_rm_demuxer);
  av_register_input_format(&ff_rtsp_demuxer);
  av_register_input_format(&ff_aac_demuxer);
  av_register_input_format(&ff_mp3_demuxer);
  av_register_input_format(&ff_wav_demuxer);
  av_register_input_format(&ff_flac_demuxer);
  av_register_input_format(&ff_ogg_demuxer);

  av_register_codec_parser(&ff_h264_parser);
  av_register_codec_parser(&ff_hevc_parser);
  av_register_codec_parser(&ff_mpegaudio_parser);
  av_register_codec_parser(&ff_flac_parser);
  av_register_codec_parser(&ff_h261_parser);
  av_register_codec_parser(&ff_h263_parser);

  LOGD("PgFf: ffmpeg 就绪（按需注册 %d 个 demuxer + %d 个 parser；解码走设备硬解）",
       11, 6);
}

}  // namespace

void Ff::disableIpv6() { ensureIpv4Only(); }

void Ff::setVerify(int on) {
  sVerify = on ? 1 : 0;
  LOGD("PgFf: 证书校验 -> %s", sVerify ? "开" : "关");
}

const char *Ff::caPath() {
  if (!sCaTried) {
    sCaTried = true;
    std::string p = CONFIGMANAGER->getResFilePath("certs/cacert.pem");
    if (access(p.c_str(), R_OK) == 0) {
      snprintf(sCaPath, sizeof(sCaPath), "%s", p.c_str());
      /* 预热：把 CA 读一遍塞进 page cache。固化版 CA 在 squashfs 上，
       * 冷缓存时首次 TLS 握手要现解压 189KB，实测能把 10s 的 rw_timeout 耗光
       * （现象：开机后第一次 https 报 "I/O error"，第二次就好了）。 */
      FILE *w = fopen(sCaPath, "rb");
      if (w) {
        char tmp[4096];
        size_t got = 0, n;
        while ((n = fread(tmp, 1, sizeof(tmp), w)) > 0) got += n;
        fclose(w);
        LOGD("PgFf: CA 证书已预热 %zu 字节", got);
      }
    } else {
      LOGW("PgFf: 找不到 CA 证书（%s）—— ffmpeg 的 https 将无法校验证书", p.c_str());
    }
  }
  return sCaPath;
}

bool Ff::inited() { return sInited; }

void Ff::ensureReady() { ensureInit(); }

bool Ff::probe(const char *url, char *out, int n) {
  if (out && n > 0) out[0] = 0;
  if (!url || !url[0]) return false;
  ensureInit();

  AVDictionary *opt = 0;
  /* rw_timeout 单位是**微秒**。原来给 10s，实测**开机后第一次** https 会超时
   * （固化版 CA 在 squashfs 上 + 冷缓存 + 首次握手），报 "I/O error (-5)"，
   * 第二次就好 —— 典型的超时而非证书问题。放宽到 30s。 */
  av_dict_set(&opt, "rw_timeout", "30000000", 0);
  /* TLS：用随包带的 CA 做校验（本板无 RTC，时间已由 NTP 校正，见 PgTime） */
  const char *ca = caPath();
  if (!sVerify) {
    av_dict_set(&opt, "verify", "0", 0);
    LOGW("PgFf: 按配置不校验证书（ffverify 0）");
  } else if (ca[0]) {
    av_dict_set(&opt, "ca_file", ca, 0);
    av_dict_set(&opt, "verify", "1", 0);
  } else {
    av_dict_set(&opt, "verify", "0", 0);  // 没证书就退化为不校验（并已告警）
  }

  AVFormatContext *fmt = 0;
  long long t0 = monoMs();
  int r = avformat_open_input(&fmt, url, 0, &opt);
  long long cost = monoMs() - t0;
  if (r < 0) {
    char eb[128] = {0};
    av_strerror(r, eb, sizeof(eb));
    LOGW("PgFf: 打开失败 '%s'：%s (%d)，耗时 %lldms —— 重试一次（首次常因超时/冷缓存）",
         url, eb, r, cost);
    if (opt) av_dict_free(&opt);
    opt = 0;
    av_dict_set(&opt, "rw_timeout", "30000000", 0);
    if (ca[0]) { av_dict_set(&opt, "ca_file", ca, 0); av_dict_set(&opt, "verify", sVerify ? "1" : "0", 0); }
    else av_dict_set(&opt, "verify", "0", 0);
    t0 = monoMs();
    r = avformat_open_input(&fmt, url, 0, &opt);
    cost = monoMs() - t0;
  }
  if (opt) av_dict_free(&opt);
  if (r < 0) {
    char eb[128] = {0};
    av_strerror(r, eb, sizeof(eb));
    LOGW("PgFf: 打开失败 '%s'：%s (%d)，耗时 %lldms", url, eb, r, cost);
    return false;
  }
  LOGD("PgFf: avformat_open_input 用时 %lldms", cost);
  r = avformat_find_stream_info(fmt, 0);
  if (r < 0) {
    char eb[128] = {0};
    av_strerror(r, eb, sizeof(eb));
    LOGW("PgFf: 取流信息失败：%s (%d)", eb, r);
    avformat_close_input(&fmt);
    return false;
  }

  int off = 0;
#define PG_PUT(...)                                                        \
  do {                                                                     \
    if (out && off < n - 1) {                                              \
      int w = snprintf(out + off, n - off, __VA_ARGS__);                   \
      if (w > 0) off += (w < n - off) ? w : (n - off - 1);                 \
    }                                                                      \
  } while (0)

  LOGD("PgFf: 打开成功 '%s'", url);
  PG_PUT("容器=%s 时长=%.1fs 码率=%.0fkbps 流数=%d",
         fmt->iformat ? fmt->iformat->name : "?", (double)fmt->duration / AV_TIME_BASE,
         fmt->bit_rate / 1000.0, (int)fmt->nb_streams);
  LOGD("PgFf:   %s", out ? out : "");
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream *st = fmt->streams[i];
    AVCodecParameters *cp = st->codecpar;
    const char *cname = avcodec_get_name(cp->codec_id);
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
      PG_PUT(" | 视频[%u]:%s %dx%d", i, cname, cp->width, cp->height);
      LOGD("PgFf:    视频流[%u] codec=%s %dx%d", i, cname, cp->width, cp->height);
    } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
      PG_PUT(" | 音频[%u]:%s %dHz %dch", i, cname, cp->sample_rate, cp->channels);
      LOGD("PgFf:    音频流[%u] codec=%s %dHz %dch", i, cname, cp->sample_rate,
           cp->channels);
    }
  }
#undef PG_PUT
  avformat_close_input(&fmt);
  return true;
}

/* 单次、短超时探测（见 PgFf.h 的说明）。刻意**不做** probe() 那次重试 ——
 * 重试是给"开机后第一次 https 冷握手"用的，局域网 RTSP 不长那样。 */
bool Ff::probeFast(const char *url, int timeoutMs) {
  if (!url || !url[0]) return false;
  ensureInit();

  /* ⚠️⚠️ 单位是**微秒**，而调用方给的是**毫秒** —— 必须 ×1000。
   *   （2026-09-17 修：原来直接把毫秒串塞进微秒字段 ⇒ 实际超时只有 **3ms**，
   *     局域网上连接稍慢就被判"打不开"，探测结果不可信。） */
  char to[32];
  snprintf(to, sizeof(to), "%lld", (long long)((timeoutMs > 0) ? timeoutMs : 3000) * 1000);

  /* ★ 两种 RTSP 传输都试：**TCP 优先**（有些 IPC 只在 RTP/AVP/TCP 上回包），
   *   不通再试 **UDP**（也有机子只认 UDP）。两条都不通才算这个地址不可用。
   *   ⚠️ 必须和播放侧（PgStream::openInput 的 UDP→TCP 回落）合起来看：
   *      探测与播放**用同一条传输**，否则会"探测报找到、播放却没画面"。 */
  const bool isRtsp = (strncmp(url, "rtsp://", 7) == 0);
  static const char *kTransports[2] = {"tcp", "udp"};
  const int numTransport = isRtsp ? 2 : 1;
  const char *ca = caPath();

  for (int t = 0; t < numTransport; ++t) {
    AVDictionary *opt = 0;
    av_dict_set(&opt, "rw_timeout", to, 0);   // 通用 socket I/O 超时（微秒）
    /* ★★ **RTSP 下绝对不能设 `timeout`**（2026-09-17 血案，必须留注释）：
     *   同名选项在 **RTSP 解封装器**里的含义是
     *     「等入连的最大秒数；**非负值 imply flag listen**」
     *   —— 一旦设了非负值，ffmpeg 就把 rtsp 切到 **listen 模式**，去 `bind()`
     *   URL 里的那个 ip:port；而对端地址当然不是本机地址 ⇒ `Address not available (-99)`，
     *   **每条路径都在 1ms 内"失败"**。
     *   现场症状极具欺骗性：**所有** RTSP 探测全挂（连本机起的合成源也打不开），
     *   而 `camurl`（直连播放，走 PgStream）却完全正常 ⇒ 看着像"摄像头不支持"，
     *   其实是探测自己把地址当成了"要我监听的那个地址"。
     *   ⇒ RTSP 只用 `stimeout`（它的含义才是"socket TCP I/O 超时"，单位微秒）；
     *     `timeout` 只留给非 RTSP（http/tcp 那些协议里它确实就是连接超时）。 */
    if (isRtsp) av_dict_set(&opt, "stimeout", to, 0);
    else av_dict_set(&opt, "timeout", to, 0);
    if (isRtsp) av_dict_set(&opt, "rtsp_transport", kTransports[t], 0);
    if (!sVerify || !ca[0]) av_dict_set(&opt, "verify", "0", 0);
    else { av_dict_set(&opt, "ca_file", ca, 0); av_dict_set(&opt, "verify", "1", 0); }

    AVFormatContext *fmt = 0;
    long long t0 = monoMs();
    int r = avformat_open_input(&fmt, url, 0, &opt);
    long long cost = monoMs() - t0;
    if (opt) av_dict_free(&opt);

    if (r >= 0) {
      LOGD("PgFf: probeFast 成功 '%s'（transport=%s，耗时 %lldms）", url,
           isRtsp ? kTransports[t] : "-", cost);
      avformat_close_input(&fmt);
      return true;
    }
    char eb[128] = {0};
    av_strerror(r, eb, sizeof(eb));
    LOGD("PgFf: probeFast 打不开 '%s'（transport=%s）：%s (%d)，耗时 %lldms", url,
         isRtsp ? kTransports[t] : "-", eb, r, cost);
  }
  return false;
}

}  // namespace pg

#endif  // FUN_BUILD
