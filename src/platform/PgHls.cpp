/*
 * PgHls.cpp - HLS 客户端实现。设计说明见 PgHls.h。
 */
#include "platform/PgHls.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include "platform/PgFf.h"
#include "utils/Log.h"

namespace pg {

namespace {

/* ============================ 可调参数 ============================ */

/* 环形缓冲 1MB —— 实测标定：本板 MemTotal 只有 55MB，播放时可用内存只剩 6~9MB，
 * 缓冲每多 1MB 都直接影响"能不能播下一个频道"。1MB 对低码率源≈20 秒、
 * 对 2Mbps 源≈4 秒，都够 ffmpeg 抵抗网络抖动（它自己是边读边解的，不整片缓存）。 */
const int kRingSize = 1024 * 1024;
const int kStartFromBack = 1;            // 起播从清单**倒数第几个**分片开始（0=最后一个）
const int kPrebufferSegs = 2;            // 至少下到几个分片才让客户端连（起播更快）
const int kM3u8RefreshMs = 2000;         // m3u8 无新分片时的重拉间隔
const int kSegRetry = 3;                 // 单个分片下载重试次数
const int kSegRetryDelayMs = 400;
const int kMaxM3u8Fail = 12;             // m3u8 连续失败多少次算断流
const int kHttpFetchTimeoutMs = 12000;   // 单次 HTTP（清单/分片）的超时
const int kReadWaitMs = 30000;           // readBuffer 等数据的超时（与老的 http rw_timeout 一致）
const int kProbeSampleBytes = 64 * 1024; // 拉清单时最多读多少字节（清单就几 KB）

/* 很多 IPTV 源会看 UA，不给就 403。用常见播放器的 UA。 */
const char *kUserAgent =
    "Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Mobile";

/* ============================ 全局状态 ============================ */

pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t s_dataCv = PTHREAD_COND_INITIALIZER;    // 有数据可读
pthread_cond_t s_spaceCv = PTHREAD_COND_INITIALIZER;   // 有空间可写

bool s_running = false;
bool s_pullAlive = false;
pthread_t s_pullTh = 0;

uint8_t *s_ring = 0;
long long s_written = 0;    // 累计写入字节（单调递增）
long long s_read = 0;       // 累计读出字节（单调递增）
/* 最近一个分片在 ring 里的**起始写位置**（见 Hls::readBuffer 的起播读指针）。 */
long long s_lastSegStart = 0;
bool s_clientActive = false;   // 播放器是否正在读缓冲
int s_clients = 0;

/* 统计 / 状态 */
volatile int s_segments = 0;
volatile int s_m3u8Fail = 0;
volatile int s_lastSegFail = 0;
char s_variant[96] = {0};
char s_status[128] = "未启动";
char s_err[160] = {0};
char s_srcUrl[768] = {0};
int s_maxBw = 0;   // 0 = 自动选最低档

/* 分片进度 */
long long s_mediaSeq = -1;    // 已下过的最后一个分片序号（EXT-X-MEDIA-SEQUENCE + 下标）
std::vector<std::string> s_pending;   // 起播时要先补下的分片
bool s_primed = false;                // 是否已过"起播预缓冲"阶段

/* 清单解析的异步状态（见 Hls::openState 的说明）。
 * ⚠️ 为什么失败要"连续 2 次"才判死：网络抖动/源偶发 5xx 很常见，
 *    第一次失败就报错会让用户白看到一次"打不开"。 */
volatile int s_openState = 0;         // 0 解析中 / 1 已解析 / 2 失败
int s_primeTries = 0;                 // 清单连续失败次数
const int kPrimeMaxTries = 2;

/* ============================ 小工具 ============================ */

void setStatus(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_status, sizeof(s_status), fmt, ap);
  va_end(ap);
}

void setError(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_err, sizeof(s_err), fmt, ap);
  va_end(ap);
  LOGW("PgHls: %s", s_err);
}

long long nowMs() {
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void sleepMs(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, 0);
}

/* ---- URL 拼接（分片常写相对路径）---- */
std::string urlJoin(const std::string &base, const std::string &rel) {
  if (rel.empty()) return rel;
  if (rel.compare(0, 7, "http://") == 0 || rel.compare(0, 8, "https://") == 0) return rel;

  size_t schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) return rel;
  size_t hostEnd = base.find('/', schemeEnd + 3);
  std::string origin = (hostEnd == std::string::npos) ? base : base.substr(0, hostEnd);

  if (rel[0] == '/') return origin + rel;

  /* 相对的：取 base 的目录部分 */
  size_t q = base.find_first_of("?#");
  std::string clean = (q == std::string::npos) ? base : base.substr(0, q);
  size_t lastSlash = clean.rfind('/');
  if (lastSlash == std::string::npos || lastSlash < schemeEnd + 3) return origin + "/" + rel;
  return clean.substr(0, lastSlash + 1) + rel;
}

/* ---- 去空白 ---- */
std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

/* ---- 用 ffmpeg 的 avio 拉 HTTP(S)（复用它的 http/https/TLS/证书链路）----
 * 为什么不用裸 socket：https 需要 TLS，而 TLS 已经编进 ffmpeg 了（PgFf 里配了 CA），
 * 自己再引一套 OpenSSL 只会多一份体积和一堆证书坑。 */
bool httpFetch(const char *url, std::string *out, int maxBytes, bool *truncated) {
  if (truncated) *truncated = false;
  Ff::ensureReady();

  AVIOContext *avio = 0;
  AVDictionary *opt = 0;
  char tmo[24];
  snprintf(tmo, sizeof(tmo), "%d", kHttpFetchTimeoutMs * 1000);
  av_dict_set(&opt, "rw_timeout", tmo, 0);
  av_dict_set(&opt, "user_agent", kUserAgent, 0);
  if (Ff::caPath() && Ff::caPath()[0]) {
    av_dict_set(&opt, "ca_file", Ff::caPath(), 0);
  }

  int r = avio_open2(&avio, url, AVIO_FLAG_READ, 0, &opt);
  if (opt) av_dict_free(&opt);
  if (r < 0) {
    char eb[96];
    av_strerror(r, eb, sizeof(eb));
    setError("打开失败 %s（%s）", url, eb);
    return false;
  }

  out->clear();
  uint8_t buf[8192];
  while ((int)out->size() < maxBytes) {
    int n = avio_read(avio, buf, sizeof(buf));
    if (n > 0) {
      out->append((const char *)buf, (size_t)n);
      continue;
    }
    if (n == AVERROR_EOF) break;          // 正常读完
    // 其它错误：已经把数据拿到了就先用着（很多源收尾会给莫名其妙的错误）
    if (!out->empty()) break;
    char eb[96];
    av_strerror(n, eb, sizeof(eb));
    setError("读取失败 %s（%s）", url, eb);
    avio_close(avio);
    return false;
  }
  avio_close(avio);
  if (out->empty()) {
    setError("空响应 %s", url);
    return false;
  }
  if (truncated) *truncated = ((int)out->size() >= maxBytes);
  return true;
}

/* ============================ m3u8 解析 ============================ */

struct Playlist {
  bool master = false;
  bool encrypted = false;
  bool fmp4 = false;
  int targetDur = 6;
  long long mediaSeq = 0;
  std::vector<std::string> segs;        // 绝对地址
  std::string variantDesc;              // 选中的档位（master 才填）
};

/* 从 master playlist 里选一档：返回 variant 的绝对地址 */
std::string pickVariant(const std::string &text, const std::string &base, std::string *desc) {
  struct Cand {
    long long bw;
    std::string res;
    std::string url;
  };
  std::vector<Cand> cands;

  std::vector<std::string> lines;
  {
    size_t p = 0;
    while (p <= text.size()) {
      size_t nl = text.find('\n', p);
      if (nl == std::string::npos) {
        lines.push_back(text.substr(p));
        break;
      }
      lines.push_back(text.substr(p, nl - p));
      p = nl + 1;
    }
  }

  for (size_t i = 0; i < lines.size(); ++i) {
    std::string l = trim(lines[i]);
    if (l.compare(0, 18, "#EXT-X-STREAM-INF:") != 0) continue;
    std::string attrs = l.substr(18);
    Cand c;
    c.bw = 0;
    size_t bp = attrs.find("BANDWIDTH=");
    if (bp != std::string::npos) c.bw = atoll(attrs.c_str() + bp + 10);
    size_t rp = attrs.find("RESOLUTION=");
    if (rp != std::string::npos) {
      size_t e = attrs.find_first_of(",", rp);
      c.res = attrs.substr(rp + 11, (e == std::string::npos) ? std::string::npos : e - rp - 11);
    }
    /* 下一行非注释行就是地址 */
    for (size_t j = i + 1; j < lines.size(); ++j) {
      std::string u = trim(lines[j]);
      if (u.empty() || u[0] == '#') continue;
      c.url = urlJoin(base, u);
      break;
    }
    if (!c.url.empty()) cands.push_back(c);
  }

  if (cands.empty()) return "";

  /* 选档策略：
   *   设了 maxBw → 在不超过它的里面挑**最高**的（尽量清晰）；
   *   没设      → 挑**最低**的（本板内存小、硬解上限 960x544，见 PgHls.h 注释）。 */
  int best = -1;
  for (size_t i = 0; i < cands.size(); ++i) {
    if (s_maxBw > 0 && cands[i].bw > s_maxBw) continue;
    if (best < 0) {
      best = (int)i;
      continue;
    }
    if (s_maxBw > 0) {
      if (cands[i].bw > cands[best].bw) best = (int)i;    // 取最高
    } else {
      if (cands[i].bw < cands[best].bw || cands[best].bw == 0) best = (int)i;  // 取最低
    }
  }
  if (best < 0) {
    /* 全都不满足上限：退回最低档，别让功能直接不可用 */
    best = 0;
    for (size_t i = 1; i < cands.size(); ++i) {
      if (cands[i].bw && cands[i].bw < cands[best].bw) best = (int)i;
    }
  }

  char d[96];
  snprintf(d, sizeof(d), "%s bw=%lld",
           cands[best].res.empty() ? "?" : cands[best].res.c_str(), cands[best].bw);
  if (desc) *desc = d;
  return cands[best].url;
}

bool parsePlaylist(const std::string &text, const std::string &base, Playlist *pl) {
  if (text.find("#EXTM3U") == std::string::npos) {
    setError("不是 m3u8（响应里没有 #EXTM3U）");
    return false;
  }

  if (text.find("#EXT-X-STREAM-INF") != std::string::npos) {
    pl->master = true;
    std::string desc;
    std::string vurl = pickVariant(text, base, &desc);
    if (vurl.empty()) {
      setError("master playlist 里没有可用档位");
      return false;
    }
    /* 递归拉子清单 */
    std::string sub;
    if (!httpFetch(vurl.c_str(), &sub, 512 * 1024, 0)) return false;
    if (!parsePlaylist(sub, vurl, pl)) return false;
    pl->variantDesc = desc;
    return true;
  }

  /* media playlist */
  pl->master = false;
  size_t p = 0;
  bool sawSeg = false;
  while (p <= text.size()) {
    size_t nl = text.find('\n', p);
    std::string raw = (nl == std::string::npos) ? text.substr(p) : text.substr(p, nl - p);
    if (nl == std::string::npos) p = text.size() + 1; else p = nl + 1;

    std::string l = trim(raw);
    if (l.empty()) continue;

    if (l[0] == '#') {
      if (l.compare(0, 22, "#EXT-X-MEDIA-SEQUENCE:") == 0) {
        pl->mediaSeq = atoll(l.c_str() + 22);
      } else if (l.compare(0, 21, "#EXT-X-TARGETDURATION:") == 0) {
        pl->targetDur = atoi(l.c_str() + 21);
      } else if (l.compare(0, 11, "#EXT-X-KEY:") == 0) {
        if (l.find("METHOD=NONE") == std::string::npos) pl->encrypted = true;
      } else if (l.compare(0, 11, "#EXT-X-MAP:") == 0) {
        pl->fmp4 = true;
      }
      continue;
    }

    sawSeg = true;
    pl->segs.push_back(urlJoin(base, l));
  }

  if (!sawSeg) {
    setError("清单里没有分片（可能是空清单或格式不支持）");
    return false;
  }
  return true;
}

/* ============================ 环形缓冲 ============================ */

/* 写端：缓冲满则等待（有客户端时）/ 推进读指针丢弃（没客户端时）。
 * 返回实际写入的字节数。 */
int ringWrite(const uint8_t *data, int len) {
  int done = 0;
  while (done < len) {
    pthread_mutex_lock(&s_lock);
    while (s_running && s_clientActive && (s_written - s_read) >= kRingSize) {
      /* 客户端在跟播但缓冲满了：等它读走一些（最多等 200ms 再看一眼 running） */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 200 * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&s_spaceCv, &s_lock, &ts);
    }
    if (!s_running) { pthread_mutex_unlock(&s_lock); break; }

    int chunk = len - done;
    long long space = kRingSize - (s_written - s_read);
    if (space <= 0) {
      /* 没有客户端在跟播：直接丢弃最旧的（直播不在乎旧数据） */
      s_read = s_written - kRingSize / 2;
      if (s_read < 0) s_read = 0;
      space = kRingSize - (s_written - s_read);
    }
    if ((long long)chunk > space) chunk = (int)space;
    if (chunk <= 0) { pthread_mutex_unlock(&s_lock); break; }

    /* 可能跨环尾：分两段写 */
    int off = (int)(s_written % kRingSize);
    int first = kRingSize - off;
    if (first > chunk) first = chunk;
    memcpy(s_ring + off, data + done, (size_t)first);
    if (chunk > first) memcpy(s_ring, data + done + first, (size_t)(chunk - first));
    s_written += chunk;
    done += chunk;
    pthread_cond_broadcast(&s_dataCv);
    pthread_mutex_unlock(&s_lock);
  }
  return done;
}

/* ============================ 下载线程 ============================ */

/* 下载一个分片并写进环形缓冲。返回 false = 这个分片废了（重试也失败）。 */
bool fetchSegment(const std::string &url) {
  AVIOContext *avio = 0;
  AVDictionary *opt = 0;
  char tmo[24];
  snprintf(tmo, sizeof(tmo), "%d", kHttpFetchTimeoutMs * 1000);
  av_dict_set(&opt, "rw_timeout", tmo, 0);
  av_dict_set(&opt, "user_agent", kUserAgent, 0);
  if (Ff::caPath() && Ff::caPath()[0]) av_dict_set(&opt, "ca_file", Ff::caPath(), 0);

  int r = avio_open2(&avio, url.c_str(), AVIO_FLAG_READ, 0, &opt);
  if (opt) av_dict_free(&opt);
  if (r < 0) return false;

  /* ★ 记下这个分片在 ring 里的起始写位置：播放器起播时要从**分片头**开始读，
   *   那里一定有 SPS/PPS + IDR（见 serveClient 的说明）。 */
  s_lastSegStart = s_written;

  /* 边读边写（不整片驻留内存 —— 分片可能好几 MB，本板只有 55MB） */
  uint8_t buf[32 * 1024];
  long long total = 0;
  bool ok = true;
  while (s_running) {
    int n = avio_read(avio, buf, sizeof(buf));
    if (n > 0) {
      int w = ringWrite(buf, n);
      total += w;
      if (w < n) { ok = s_running; break; }   // 被停止打断
      continue;
    }
    if (n == AVERROR_EOF) break;
    /* 读到一半出错：分片不完整，会让解码器报错。但很多源收尾就是这样，
     * 已经拿到大部分数据的话就认了（否则直播会频繁丢分片）。 */
    ok = (total > 32 * 1024);
    break;
  }
  avio_close(avio);
  return ok && total > 0;
}

/* 拉一次清单 + 把新分片下下来 */
bool pullOnce(bool *gotNew) {
  *gotNew = false;

  std::string text;
  if (!httpFetch(s_srcUrl, &text, 512 * 1024, 0)) {
    /* 起播阶段（还没拿到过清单）连续打不开 → 判死，让 UI 立刻给明确原因 */
    if (s_openState == 0 && ++s_primeTries >= kPrimeMaxTries) {
      s_openState = 2;
      setError("打不开清单（网络不可用或源失效）");
      setStatus("打不开清单");
      LOGW("PgHls: 清单连续 %d 次打不开 → 判定失败：%s", s_primeTries, s_srcUrl);
    }
    ++s_m3u8Fail;
    if (s_m3u8Fail >= kMaxM3u8Fail) setStatus("清单连续失败 %d 次", s_m3u8Fail);
    return false;
  }
  s_m3u8Fail = 0;

  Playlist pl;
  if (!parsePlaylist(text, s_srcUrl, &pl)) {
    if (s_openState == 0) { s_openState = 2; setError("清单解析失败"); }
    return false;
  }

  if (pl.encrypted && !s_primed) {
    setError("该频道是 AES-128 加密流，暂不支持");
    setStatus("加密流不支持");
    if (s_openState == 0) s_openState = 2;
    return false;
  }
  if (pl.fmp4 && !s_primed) {
    setError("该频道是 fMP4 分片（.m4s），本板链路只认 .ts");
    setStatus("fMP4 不支持");
    if (s_openState == 0) s_openState = 2;
    return false;
  }
  if (s_openState == 0) {
    /* 第一次解析成功 = 频道可用（"清单 OK" 这条日志从 start() 搬到这里） */
    s_openState = 1;
    s_primeTries = 0;
    LOGD("PgHls: 清单 OK —— 分片 %d 个（从倒数第 %d 个起播），媒体序号 %lld，档位 %s",
         (int)pl.segs.size(), kStartFromBack, pl.mediaSeq, s_variant[0] ? s_variant : "(media)");
  }

  if (pl.variantDesc.size() && s_variant[0] == 0) {
    snprintf(s_variant, sizeof(s_variant), "%s", pl.variantDesc.c_str());
    LOGD("PgHls: 选中档位 %s（共 %d 个分片）", s_variant, (int)pl.segs.size());
  }

  long long seqBase = pl.mediaSeq;

  if (!s_primed) {
    /* 起播：从倒数第 kStartFromBack 个开始，把这之后的都排进待下队列 */
    int n = (int)pl.segs.size();
    int from = n - 1 - kStartFromBack;
    if (from < 0) from = 0;
    for (int i = from; i < n; ++i) {
      s_pending.push_back(pl.segs[i]);
    }
    s_mediaSeq = seqBase + n - 1;   // 之后只认比它新的
    return true;
  }

  /* 续拉：挑出序号比已下过的更新的分片 */
  long long lastSeq = seqBase + (long long)pl.segs.size() - 1;
  if (lastSeq <= s_mediaSeq) {
    return false;   // 没有新分片
  }
  /* 窗口可能已经滚过我们记的位置（下载太慢被甩开）——从窗口里最老的开始补，别越界 */
  long long startSeq = s_mediaSeq + 1;
  if (startSeq < seqBase) startSeq = seqBase;
  int skipped = 0;
  for (long long s = startSeq; s <= lastSeq; ++s) {
    int idx = (int)(s - seqBase);
    if (idx < 0 || idx >= (int)pl.segs.size()) continue;
    /* 一次补太多说明严重落后，丢掉前面的（直播要追最新的） */
    if (lastSeq - s >= 4) { ++skipped; continue; }
    s_pending.push_back(pl.segs[idx]);
  }
  if (skipped) LOGW("PgHls: 下载落后，跳过 %d 个旧分片追赶", skipped);
  s_mediaSeq = lastSeq;
  return true;
}

void *pullThread(void *) {
  LOGD("PgHls: 下载线程启动");
  while (s_running) {
    if (!s_pending.empty()) {
      std::string u = s_pending.front();
      s_pending.erase(s_pending.begin());

      bool ok = false;
      for (int i = 0; i < kSegRetry && s_running; ++i) {
        if (fetchSegment(u)) { ok = true; break; }
        sleepMs(kSegRetryDelayMs);
      }
      if (!ok) {
        ++s_lastSegFail;
        LOGW("PgHls: 分片下载失败（第 %d 次），跳过：%s", s_lastSegFail, u.c_str());
      } else {
        ++s_segments;
        if (!s_primed) {
          /* 起播预缓冲够了就放客户端进来 */
          if (s_segments >= kPrebufferSegs) {
            s_primed = true;
            setStatus("就绪（已缓冲 %d 分片）", s_segments);
            LOGD("PgHls: 预缓冲完成（%d 分片），等待播放器连接", s_segments);
          } else {
            setStatus("缓冲中(%d/%d)", s_segments, kPrebufferSegs);
          }
        } else if ((s_segments % 5) == 0) {
          setStatus("拉流中（%d 分片 / %d 次取流）", s_segments, s_clients);
          LOGD("PgHls: 已下 %d 分片，%d 字节，档位 %s，取流 %d 次", s_segments,
               (int)s_written, s_variant, s_clients);
        }
      }
      continue;
    }

    /* 队列空了：重拉清单找新分片 */
    bool gotNew = false;
    pullOnce(&gotNew);
    if (!gotNew) sleepMs(kM3u8RefreshMs);
  }
  LOGD("PgHls: 下载线程退出（共 %d 分片，%d 字节）", s_segments, (int)s_written);
  return 0;
}

/* ============================ 本地 HTTP 服务 ============================ */

/* 服务一个客户端：先把手上的积压数据吐出去，然后跟着下载进度实时吐。 */
/* ============================ 收尾 ============================ */

void cleanup() {
  s_running = false;
  s_pullAlive = false;
  pthread_cond_broadcast(&s_dataCv);
  pthread_cond_broadcast(&s_spaceCv);

  /* ⚠️ 线程是 detached 的，没法 join —— 但它们都会在 s_running=false 后
   * 从阻塞点（poll / cond_timedwait 最多 300ms）醒来退出。
   * 这里先等一下子，避免"线程还在用 s_ring / s_pending"时被下面 free 掉。 */
  sleepMs(500);

  if (s_ring) {
    free(s_ring);
    s_ring = 0;
  }
  pthread_mutex_lock(&s_lock);
  s_written = 0;
  s_read = 0;
  s_clientActive = false;
  s_clients = 0;
  s_primed = false;
  s_openState = 0;     // 起播状态复位（下一次 start 会重新置 0）
  s_primeTries = 0;
  s_mediaSeq = -1;
  s_segments = 0;
  s_m3u8Fail = 0;
  s_lastSegFail = 0;
  s_variant[0] = 0;
  pthread_mutex_unlock(&s_lock);
}

}  // namespace

/* ============================ 对外接口 ============================ */

void Hls::setMaxBandwidth(int bps) {
  s_maxBw = (bps > 0) ? bps : 0;
  LOGD("PgHls: 选流带宽上限 -> %d（0 = 自动选最低档）", s_maxBw);
}

int Hls::maxBandwidth() { return s_maxBw; }

bool Hls::start(const char *m3u8Url, char *out, int n) {
  if (!m3u8Url || !*m3u8Url) return false;

  if (s_running) stop();
  s_err[0] = 0;
  s_status[0] = 0;
  s_variant[0] = 0;

  snprintf(s_srcUrl, sizeof(s_srcUrl), "%s", m3u8Url);
  setStatus("解析清单");
  LOGD("PgHls: 启动 HLS —— %s", m3u8Url);

  /* ① 只做"本地准备"，**同步拉清单这一步已经拿掉**（见 Hls::openState 的说明）：
   *    清单交给下载线程的第一轮 pullOnce()，start() 立刻返回。
   *    好处：UI 线程一次都不阻塞 ⇒ "正在连接 + 动画 + 超时"才画得出来；
   *    代价：频道不可用（清单打不开/加密/fMP4）不再在调用点就报，
   *          改为异步 —— 调用方轮询 openState()（IPTV 的加载页就是这么做的）。 */
  s_running = true;      // ringWrite 等函数要先看到它
  s_primed = false;
  s_openState = 0;
  s_primeTries = 0;
  s_pending.clear();
  s_mediaSeq = -1;

  /* ② 环形缓冲 */
  s_ring = (uint8_t *)malloc(kRingSize);
  if (!s_ring) {
    s_running = false;
    setError("环形缓冲分配失败（%d 字节）", kRingSize);
    setStatus("内存不足");
    return false;
  }
  memset(s_ring, 0, kRingSize);
  s_written = 0;
  s_read = 0;
  /* ⚠️ 这个也必须复位：它记的是"上一轮最后一个分片的写位置"，不复位的话第二轮起播
   *    会拿到一个**比新写指针还大**的值 ⇒ 读指针跑到写指针前面 ⇒ 客户端永远读不到字节
   *    （实测：第二轮起播"字节"一直在涨、解码帧恒 0，界面卡在缓冲底图）。 */
  s_lastSegStart = 0;

  /* ③ 不再起本地 HTTP 中继（2026-09-14 去掉）：ffmpeg 改为**自定义 AVIO**
   *    直接从这块环形缓冲读字节，见 Hls::readBuffer / PgStream::openInput。 */
  /* ④ 起下载线程（只剩一个了 —— 中继线程已去掉） */
  s_pullAlive = true;
  int pr = pthread_create(&s_pullTh, 0, pullThread, 0);
  if (pr == 0) pthread_detach(s_pullTh);
  if (pr != 0) {
    s_running = false;
    setError("线程创建失败（pull=%d）", pr);
    setStatus("线程失败");
    cleanup();
    return false;
  }

  if (out && n > 0) snprintf(out, n, "pg-hls://live");
  setStatus("连接中…");   // 真正"清单 OK / 缓冲"的状态由 pullOnce 异步刷新
  LOGD("PgHls: ★ 已启动，播放地址 pg-hls://live（ffmpeg 自定义 AVIO 直读环形缓冲）");
  return true;
}

void Hls::stop() {
  if (!s_running && !s_ring) return;
  LOGD("PgHls: 停止（已下 %d 分片 / %d 字节）", s_segments, (int)s_written);
  cleanup();
  setStatus("已停止");
}

/* ==================== 给 ffmpeg 自定义 AVIO 的读接口 ====================
 * 逻辑从原来的本地中继 serveClient 搬来（2026-09-14 去中继），语义不变。
 * ⚠️ 这个函数运行在 **ffmpeg 的读线程**上，会被 av_read_frame 反复调用；
 *    它只碰环形缓冲 + 条件变量，绝不做网络 IO（网络在 pullThread 里）。 */
int Hls::readBuffer(unsigned char *out, int n, int timeoutMs) {
  if (!out || n <= 0) return 0;
  /* ⚠️ s_ring 可能已经被 cleanup() 释放（start() 里 s_running 先置 true、ring 后分配，
   *    收尾时又反过来）—— 少判一次就是空指针 memcpy ⇒ 进程静默消失（无 FATAL 日志）。 */
  if (!s_running || !s_ring) return 0;
  {
    static int s_calls = 0;
    if (s_calls < 4) {
      ++s_calls;
      LOGD("PgHls: readBuffer 第 %d 次调用（要 %d 字节，可读 %lld，已写 %lld）", s_calls, n,
           s_written - s_read, s_written);
    }
  }

  /* 第一个读者：把读指针对齐到**当前分片的起始位置**（那里一定有 SPS/PPS + IDR）。
   * 从"最新字节"开始 = 从分片中间开始 ⇒ 音频（AAC 自带同步）会先响、视频要等下一个
   * IDR 才出画面 —— 实测 20.7 秒，看着就像"缓冲底图没隐藏"（见 docs/iptv.md §11.4.4）。 */
  if (!s_clientActive) {
    pthread_mutex_lock(&s_lock);
    if (!s_clientActive) {
      s_clientActive = true;
      ++s_clients;
      long long start = s_lastSegStart;
      /* 范围防御：必须落在 (写指针 - ring/2, 写指针] 内。越界一次就是"永远读不到数据"。 */
      if (start < s_written - kRingSize / 2 || start > s_written) start = s_written;
      s_read = start;
      pthread_cond_broadcast(&s_spaceCv);
      LOGD("PgHls: 起播读指针 = %lld（写 %lld，分片头 %lld，%s）", start, s_written,
           s_lastSegStart, (start == s_lastSegStart) ? "对齐分片头" : "退回最新");
      setStatus("播放中（%d 分片）", s_segments);
    }
    pthread_mutex_unlock(&s_lock);
  }

  int got = 0;
  pthread_mutex_lock(&s_lock);
  if (s_running && s_written == s_read) {   // 没数据：睡到有数据 / 超时
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ns = (long long)ts.tv_nsec + (long long)timeoutMs * 1000000LL;
    ts.tv_sec += (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    pthread_cond_timedwait(&s_dataCv, &s_lock, &ts);
  }
  if (s_running) {
    long long avail = s_written - s_read;
    if (avail > 0) {
      int chunk = (avail > (long long)n) ? n : (int)avail;
      int off = (int)(s_read % kRingSize);
      int first = kRingSize - off;
      if (first > chunk) first = chunk;
      memcpy(out, s_ring + off, (size_t)first);
      if (chunk > first) memcpy(out + first, s_ring, (size_t)(chunk - first));
      s_read += chunk;
      pthread_cond_broadcast(&s_spaceCv);
      got = chunk;
    }
  }
  pthread_mutex_unlock(&s_lock);
  return got;   // 0 = 流已停 / 超时（调用方按 EOF 处理）
}

bool Hls::running() { return s_running; }
int Hls::openState() { return s_openState; }

int Hls::segments() { return s_segments; }
int Hls::bytes() { return (int)s_written; }
int Hls::clients() { return s_clientActive ? 1 : 0; }   // 播放器是否在读缓冲
const char *Hls::variant() { return s_variant[0] ? s_variant : "-"; }
const char *Hls::status() { return s_status[0] ? s_status : "-"; }
const char *Hls::lastError() { return s_err[0] ? s_err : "-"; }
const char *Hls::sourceUrl() { return s_srcUrl[0] ? s_srcUrl : "-"; }

}  // namespace pg
