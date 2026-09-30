#define _GNU_SOURCE
/*
 * PgSniff.cpp - 无线嗅探实现（nl80211 切 monitor + AF_PACKET 抓帧 + radiotap/802.11 解析）
 *
 * 设计取舍（为什么这么写）：
 *  ① **不 shell 出去调外部程序**：工程里虽有 tools/nlprobe.c 这个独立可执行，
 *     但那是"调试用的探针"；应用内必须自包含（固化后 /tmp 里不会有它）。
 *     genl 那几十行直接搬进来，逻辑与 nlprobe 完全一致。
 *  ② **只动 wlan1**：wlan0 是 zknet/wpa_supplicant 在用的 STA 口，一根手指都不要碰。
 *  ③ **退出必还原**：stop() → 停线程 → `SET_INTERFACE wlan1 STATION`。
 *     哪怕抓包失败也要还原（否则把接口留在监听态）。
 *  ④ 解析只做到"够用的深度"：radiotap 只取 CHANNEL + DBM_ANTSIGNAL（位 3、位 5），
 *     802.11 只要 addr2（发射者）+ 管理帧 subtype + SSID element。
 *     不做序列号去重、不做信道跳变（单射频跳不了）。
 */
#include "platform/PgSniff.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

#include "platform/PgLan.h"     // pgMacIsRandom / pgOuiLookup（同一份 OUI 表）
#include "utils/Log.h"

#ifndef NLA_ALIGNTO
#define NLA_ALIGNTO 4
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_HDRLEN ((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif

namespace pg {

static long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void macToStr(const uint8_t *m, char *out, int n) {
  snprintf(out, (size_t)n, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* ==========================================================================
 * generic-netlink 小工具（与 tools/nlprobe.c 同源）
 * ========================================================================== */
struct GenlMsg {
  struct nlmsghdr n;
  struct genlmsghdr g;
  char buf[1024];
  int len;
};

/* ==========================================================================
 * ★★ 全信道跳扫（跳信道）
 *
 * 实测（2026-09-15，docs/wifi-probe-app.md §13）：单射频驱动下**占用信道的是
 * wlan0 这个接口本身**，不是关联状态：
 *     ① wlan0 关联着      -> SET_CHANNEL = -16 EBUSY
 *     ② wlan0 up 但没关联 -> 仍然 -16 EBUSY    ← 只 DISCONNECT 是不够的
 *     ③ **wlan0 down**    -> SET_CHANNEL = 0，全信道都能切
 * 所以想扫别的信道只能把 wlan0 关掉（由 probeLogic 配合「回家快照」处理进出）。
 * ========================================================================== */
static const int kHopChans[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                36, 40, 44, 48, 149, 153, 157, 161, 165};
static const int kHopChanN = (int)(sizeof(kHopChans) / sizeof(kHopChans[0]));

static int chanToFreq(int ch) {
  if (ch == 14) return 2484;
  if (ch >= 1 && ch <= 13) return 2407 + ch * 5;
  if (ch >= 36 && ch <= 177) return 5000 + ch * 5;
  return 0;
}

struct SniffImpl {
  /* ---- nl80211 ---- */
  int genlFd = -1;
  int family = -1;

  /* ---- 抓包 ---- */
  int pktFd = -1;
  pthread_t th = 0;
  bool haveTh = false;
  volatile bool run = false;
  volatile bool stopReq = false;

  /* ---- 统计 ---- */
  volatile long frames = 0;
  volatile long dataFrames = 0;
  volatile int  chan = 0;
  volatile int  gen = 0;
  long t0 = 0;

  /* ---- 跳信道（全信道嗅探）---- */
  volatile bool hop = false;
  volatile int  hopIdx = 0;         // 当前在 kHopChans 里的下标
  volatile int  hopCurChan = 0;     // 当前停的信道号
  volatile int  hopScanned = 0;     // 累计切换次数
  volatile int  hopRounds = 0;      // 完整扫完几轮
  volatile int  hopDwellMs = 500;   // 每信道驻留（beacon 间隔 100ms => 至少 5 个）
  volatile long hopNextAt = 0;      // 下次切信道的时刻
  volatile long hopFail = 0;        // 切失败次数（>0 = 射频没释放干净）
  unsigned hopIfIdx = 0;            // wlan1 的 ifindex
  volatile long chanFrames[kHopChanN];

  /* ---- 表 ---- */
  SniffPeer peers[kSniffMaxPeer];
  long peerLast[kSniffMaxPeer];   // 各设备最后活动时刻（CLOCK_MONOTONIC ms）
  int nPeer = 0;
  char ssids[kSniffMaxSsid][34];
  int ssidFromAssoc[kSniffMaxSsid];
  int nSsid = 0;
  volatile int hiddenResolved = 0;

  char state[96] = "未启动";
  bool ok = false;

  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

  static unsigned seq;
};

unsigned SniffImpl::seq = 1;
static SniffImpl gS;
static Sniff gSniffApi;

static void msgInit(GenlMsg *m, int type, int cmd, int flags) {
  memset(m, 0, sizeof(*m));
  m->n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  m->n.nlmsg_type = (unsigned short)type;
  m->n.nlmsg_flags = NLM_F_REQUEST | (unsigned short)flags;
  m->n.nlmsg_seq = SniffImpl::seq++;
  m->g.cmd = (uint8_t)cmd;
  m->g.version = 1;
  m->len = NLMSG_ALIGN(m->n.nlmsg_len);
}

static void putU32(GenlMsg *m, int type, unsigned v) {
  struct nlattr *a = (struct nlattr *)((char *)&m->n + m->len);
  a->nla_type = (unsigned short)type;
  a->nla_len = (unsigned short)(NLA_HDRLEN + 4);
  *(unsigned *)((char *)a + NLA_HDRLEN) = v;
  m->len += NLA_ALIGN(a->nla_len);
  m->n.nlmsg_len = (unsigned)m->len;
}

static void putStr(GenlMsg *m, int type, const char *s) {
  struct nlattr *a = (struct nlattr *)((char *)&m->n + m->len);
  int l = (int)strlen(s) + 1;
  a->nla_type = (unsigned short)type;
  a->nla_len = (unsigned short)(NLA_HDRLEN + l);
  memcpy((char *)a + NLA_HDRLEN, s, (size_t)l);
  m->len += NLA_ALIGN(a->nla_len);
  m->n.nlmsg_len = (unsigned)m->len;
}

static int genlSend(int fd, GenlMsg *m) {
  struct sockaddr_nl dst;
  memset(&dst, 0, sizeof(dst));
  dst.nl_family = AF_NETLINK;
  return (int)sendto(fd, &m->n, m->n.nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static int genlOpen() {
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  if (fd < 0) return -1;
  struct sockaddr_nl sa;
  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/** 解析 `CTRL_CMD_GETFAMILY` 的应答，取 FAMILY_ID。 */
static int genlFamilyId(int fd, const char *name) {
  GenlMsg m;
  msgInit(&m, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0);
  /* ⚠️ 这一步还没拿到 family id，nlmsg_type 必须是 GENL_ID_CTRL；
   *    写成"待解析的 family"会收到 -ENOENT，看着像"内核没编 cfg80211"（踩过）。 */
  putStr(&m, CTRL_ATTR_FAMILY_NAME, name);
  if (genlSend(fd, &m) < 0) return -1;

  char buf[8192];
  int n = (int)recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) return -1;
  struct nlmsghdr *h = (struct nlmsghdr *)buf;
  while (NLMSG_OK(h, n)) {
    if (h->nlmsg_type == NLMSG_ERROR) return -1;
    if (h->nlmsg_type == NLMSG_DONE) break;
    struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
    int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN));
    struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
    while (alen >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= alen) {
      if ((a->nla_type & 0x3FFF) == CTRL_ATTR_FAMILY_ID) {
        return (int)(*(uint16_t *)((char *)a + NLA_HDRLEN));
      }
      alen -= NLA_ALIGN(a->nla_len);
      a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len));
    }
    h = NLMSG_NEXT(h, n);
  }
  return -1;
}

/** 发一条带 ACK 的 nl80211 命令，返回 0 = 成功。 */
static int nl80211Cmd(int fd, int family, int cmd, unsigned ifindex, int iftype, int *errOut) {
  GenlMsg m;
  msgInit(&m, family, cmd, NLM_F_ACK);
  if (ifindex) putU32(&m, NL80211_ATTR_IFINDEX, ifindex);
  if (iftype >= 0) putU32(&m, NL80211_ATTR_IFTYPE, (unsigned)iftype);
  if (genlSend(fd, &m) < 0) {
    if (errOut) *errOut = -errno;
    return -1;
  }
  char buf[4096];
  int n = (int)recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) {
    if (errOut) *errOut = -errno;
    return -1;
  }
  struct nlmsghdr *h = (struct nlmsghdr *)buf;
  while (NLMSG_OK(h, n)) {
    if (h->nlmsg_type == NLMSG_ERROR) {
      struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
      if (errOut) *errOut = e->error;
      return e->error == 0 ? 0 : -1;
    }
    h = NLMSG_NEXT(h, n);
  }
  if (errOut) *errOut = -999;
  return -1;
}

/**
 * SET_CHANNEL（把 monitor 口调到指定频率）。
 * 这里的 recv 必须**带超时**：跳信道每 500ms 一次、跑在抓包线程里，
 * 没有超时的话内核一次不响应就把整个抓包线程**永久卡死**（stop 也不管用）。
 */
static int nl80211SetChannel(int fd, int family, unsigned ifindex, int freq, int *errOut) {
  GenlMsg m;
  msgInit(&m, family, NL80211_CMD_SET_CHANNEL, NLM_F_ACK);
  putU32(&m, NL80211_ATTR_IFINDEX, ifindex);
  putU32(&m, NL80211_ATTR_WIPHY_FREQ, (unsigned)freq);
  putU32(&m, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT20);
  if (genlSend(fd, &m) < 0) {
    if (errOut) *errOut = -errno;
    return -1;
  }
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(fd, &rf);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 400000;
  int sel = select(fd + 1, &rf, 0, 0, &tv);
  if (sel <= 0) {
    if (errOut) *errOut = -9999;   // 超时
    return -1;
  }
  char buf[4096];
  int n = (int)recv(fd, buf, sizeof(buf), 0);
  if (n <= 0) {
    if (errOut) *errOut = -errno;
    return -1;
  }
  struct nlmsghdr *h = (struct nlmsghdr *)buf;
  while (NLMSG_OK(h, n)) {
    if (h->nlmsg_type == NLMSG_ERROR) {
      struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
      if (errOut) *errOut = e->error;
      return e->error == 0 ? 0 : -1;
    }
    h = NLMSG_NEXT(h, n);
  }
  if (errOut) *errOut = -999;
  return -1;
}

/**
 * 把 monitor 口切到 kHopChans[hopIdx] 并排好下一次切换的时间。
 * 切失败**也要前进**（否则会卡死在一个信道上，看着像扫描停了）。
 */
static void hopApply() {
  int ch = kHopChans[gS.hopIdx];
  int f = chanToFreq(ch);
  int err = 0;
  int r = -1;
  if (gS.genlFd >= 0 && gS.hopIfIdx)
    r = nl80211SetChannel(gS.genlFd, gS.family, gS.hopIfIdx, f, &err);
  if (r != 0) {
    gS.hopFail++;
    /* 只在前两次打印，避免刷屏（logcat 缓冲只有十几行） */
    if (gS.hopFail <= 2)
      LOGD("PgSniff: 跳信道 CH%d (%dMHz) 失败 err=%d（-16=EBUSY：wlan0 还占着射频，接口必须 down）",
           ch, f, err);
  }
  gS.hopCurChan = ch;
  gS.hopNextAt = nowMs() + gS.hopDwellMs;
  gS.hopScanned++;
  /* 跳信道时把收包超时调小，切换才跟得上 */
  if (gS.pktFd >= 0) {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 120000;
    setsockopt(gS.pktFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
}

static void hopNext() {
  gS.hopIdx++;
  if (gS.hopIdx >= kHopChanN) {
    gS.hopIdx = 0;
    gS.hopRounds++;
  }
  hopApply();
}

static int ifSetUp(const char *ifname, bool up) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
  int r = -1;
  if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
    if (up) ifr.ifr_flags |= IFF_UP;
    else ifr.ifr_flags &= ~IFF_UP;
    r = ioctl(fd, SIOCSIFFLAGS, &ifr);
  }
  close(fd);
  return r;
}

/* ==========================================================================
 * radiotap：只取 CHANNEL(位3) 与 DBM_ANTSIGNAL(位5)
 *
 * ⚠️ 字段必须**按位号顺序**走，并且按各自的对齐要求补 padding
 *    （位 0 TSFT 要 8 字节对齐、位 3 CHANNEL 要 2 字节对齐）。不对齐取到的
 *    freq/rssi 会整体错位 —— 症状是"信道显示成一堆奇怪的数、RSSI 恒为 -128"。
 * ========================================================================== */
struct Radiotap {
  int hdrLen;
  int freq;
  int dbm;
};

static bool parseRadiotap(const uint8_t *d, int n, Radiotap *rt) {
  rt->hdrLen = 0;
  rt->freq = 0;
  rt->dbm = 0;
  if (n < 8 || d[0] != 0) return false;
  int len = d[2] | (d[3] << 8);
  if (len < 8 || len > n) return false;
  rt->hdrLen = len;

  uint32_t present = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) |
                     ((uint32_t)d[7] << 24);
  int off = 8;
  /* 只需要走到位 5，所以只认 0..5 这几号的长度/对齐 */
  for (int bit = 0; bit <= 5; ++bit) {
    if (!((present >> bit) & 1u)) continue;
    switch (bit) {
      case 0:  // TSFT: 8 字节，8 对齐
        off = (off + 7) & ~7;
        off += 8;
        break;
      case 1:  // FLAGS: 1 字节
        off += 1;
        break;
      case 2:  // RATE: 1 字节
        off += 1;
        break;
      case 3: {  // CHANNEL: 2 字节 freq(LE) + 2 字节 flags，2 对齐
        off = (off + 1) & ~1;
        if (off + 2 <= len) rt->freq = d[off] | (d[off + 1] << 8);
        off += 4;
        break;
      }
      case 4:  // FHSS: 1 字节
        off += 1;
        break;
      case 5:  // DBM_ANTSIGNAL: 有符号 1 字节
        if (off + 1 <= len) rt->dbm = (int)(int8_t)d[off];
        off += 1;
        break;
      default:
        break;
    }
  }
  return true;
}

static int freqToChan(int f) {
  if (f <= 0) return 0;
  if (f >= 5000) return (f - 5000) / 5;
  if (f == 2484) return 14;
  if (f >= 2412 && f <= 2472) return (f - 2412) / 5 + 1;
  return 0;
}

/* ==========================================================================
 * 802.11 + SSID element
 * ========================================================================== */
static const char *mgmtSubName(uint8_t sub) {
  switch (sub) {
    case 0: return "assoc-req";
    case 1: return "assoc-resp";
    case 2: return "reassoc-req";
    case 4: return "probe-req";
    case 5: return "probe-resp";
    case 8: return "beacon";
    case 10: return "disassoc";
    case 11: return "auth";
    case 12: return "deauth";
    default: return "mgmt";
  }
}

/** 从 tagged params 里取 SSID（element id 0）。返回 true = 取到了非空名字。 */
static bool parseSsid(const uint8_t *ies, int n, char *out, int outn) {
  out[0] = 0;
  int i = 0;
  while (i + 2 <= n) {
    int id = ies[i];
    int len = ies[i + 1];
    if (len == 0) {
      i += 2;
      continue;
    }
    if (i + 2 + len > n) break;
    if (id == 0) {
      int c = len;
      if (c > outn - 1) c = outn - 1;
      for (int k = 0; k < c; ++k) {
        uint8_t ch = ies[i + 2 + k];
        out[k] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
      }
      out[c] = 0;
      /* 全 0（有些厂商拿 SSID 当占位）与零长（wildcard probe）都算"没名字" */
      if (c == 0) return false;
      bool allZero = true;
      for (int k = 0; k < c; ++k)
        if (out[k] != 0 && out[k] != '.') {
          allZero = false;
          break;
        }
      return !allZero;
    }
    i += 2 + len;
  }
  return false;
}

/* ==========================================================================
 * 表维护（都在锁里）
 * ========================================================================== */
static int peerIdx(const char *mac) {
  for (int i = 0; i < gS.nPeer; ++i)
    if (strcmp(gS.peers[i].mac, mac) == 0) return i;
  return -1;
}

static SniffPeer *peerTouch(const char *mac, bool *isNew) {
  int i = peerIdx(mac);
  if (i < 0) {
    if (gS.nPeer >= kSniffMaxPeer) {
      if (isNew) *isNew = false;
      return 0;
    }
    i = gS.nPeer++;
    memset(&gS.peers[i], 0, sizeof(gS.peers[i]));
    snprintf(gS.peers[i].mac, sizeof(gS.peers[i].mac), "%s", mac);
    snprintf(gS.peers[i].vendor, sizeof(gS.peers[i].vendor), "%s", pgOuiLookup(mac));
    snprintf(gS.peers[i].role, sizeof(gS.peers[i].role), "?");
    if (pgMacIsRandom(mac)) gS.peers[i].flags |= SNIFF_F_RANDOM_MAC;
    if (isNew) *isNew = true;
  } else if (isNew) {
    *isNew = false;
  }
  gS.peerLast[i] = nowMs();
  /* 记下它是在哪个信道被看到的 —— 跳信道模式下这是**最有价值的一列**
   * （哪台设备在哪个信道 = 空口占用地图）。 */
  gS.peers[i].chan = gS.hop ? gS.hopCurChan : gS.chan;
  return &gS.peers[i];
}

static void ssidRemember(const char *name, bool fromAssoc, int *isNew) {
  if (isNew) *isNew = 0;
  if (!name || !name[0] || strcmp(name, "(隐藏)") == 0) return;
  for (int i = 0; i < gS.nSsid; ++i) {
    if (strcmp(gS.ssids[i], name) == 0) {
      if (fromAssoc && !gS.ssidFromAssoc[i]) {
        gS.ssidFromAssoc[i] = 1;
        gS.hiddenResolved++;
        if (isNew) *isNew = 1;
      }
      return;
    }
  }
  if (gS.nSsid >= kSniffMaxSsid) return;
  snprintf(gS.ssids[gS.nSsid], sizeof(gS.ssids[0]), "%s", name);
  gS.ssidFromAssoc[gS.nSsid] = fromAssoc ? 1 : 0;
  if (fromAssoc) gS.hiddenResolved++;
  gS.nSsid++;
  if (isNew) *isNew = 1;
}

/* ==========================================================================
 * 抓包线程
 * ========================================================================== */
static void sniffFrame(const uint8_t *buf, int n) {
  Radiotap rt;
  if (!parseRadiotap(buf, n, &rt)) return;
  int off = rt.hdrLen;
  if (off + 10 > n) return;
  const uint8_t *f = buf + off;
  int flen = n - off;
  uint8_t fc0 = f[0];
  int type = (fc0 >> 2) & 0x3;
  int sub = (fc0 >> 4) & 0xF;

  /* ★★ 跳信道模式下**只统计当前信道的帧**：切信道之后 socket 缓冲里还压着
   *    上一个信道的帧（radiotap 里的 freq 是发送时的真实频率），不过滤就会把
   *    两个信道的数据混在一起 —— 症状是 CH1 的统计里冒出 CH36 的 AP，
   *    设备归属的信道也全乱（跳扫的核心价值就是"谁在哪个信道"）。 */
  if (gS.hop && gS.hopCurChan > 0) {
    int fc = rt.freq ? freqToChan(rt.freq) : 0;
    if (fc > 0 && fc != gS.hopCurChan) return;
  }

  /* ⚠️ 控制帧长度不一（ACK/CTS 只有 10 字节、RTS 16 字节）：
   *    没这个长度检查就会读到缓冲区尾部的**上一次残留**
   *    ⇒ 症状是"列表里冒出一堆鬼 MAC"。 */
  if (flen < 16) {
    if (type == 1) gS.frames++;   // 短控制帧：只计数，不认人
    return;
  }

  if (gS.hop) {
    /* 跳信道模式下 chan 表示"我们正在听哪个信道"（不是最后见到帧的信道） */
    gS.chan = gS.hopCurChan;
    if (gS.hopIdx >= 0 && gS.hopIdx < kHopChanN) gS.chanFrames[gS.hopIdx]++;
  } else if (rt.freq) {
    int c = freqToChan(rt.freq);
    if (c > 0 && c != gS.chan) gS.chan = c;
  }

  char a2[18];
  char a1[18];
  macToStr(f + 10, a2, sizeof(a2));   // addr2 = 发射者
  macToStr(f + 4, a1, sizeof(a1));    // addr1 = 接收者

  gS.frames++;

  /* ---- 管理帧：这是"情报"的来源 ---- */
  if (type == 0) {
    const int hdr = 24;
    if (flen < hdr) return;
    const uint8_t *ies = 0;
    int ielen = 0;
    if (sub == 4 || sub == 0 || sub == 2) {          // probe-req / assoc-req：固定参数 4 字节
      if (flen >= hdr + 4) {
        ies = f + hdr + 4;
        ielen = flen - hdr - 4;
      }
    } else if (sub == 5 || sub == 8 || sub == 1) {    // probe-resp / beacon：固定参数 12 字节
      if (flen >= hdr + 12) {
        ies = f + hdr + 12;
        ielen = flen - hdr - 12;
      }
    }
    char ssid[34] = {0};
    bool hasSsid = ies && parseSsid(ies, ielen, ssid, sizeof(ssid));

    pthread_mutex_lock(&gS.mu);
    bool isNew = false;
    SniffPeer *p = peerTouch(a2, &isNew);
    if (p) {
      p->nMgmt++;
      if (rt.dbm) p->rssi = rt.dbm;
      if (sub == 8 || sub == 5 || sub == 1) {
        snprintf(p->role, sizeof(p->role), "AP");
        if (hasSsid) {
          snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
          snprintf(p->ssidKind, sizeof(p->ssidKind), "beacon");
          /* AP 的 beacon/探测应答里就是**明文 SSID** ⇒ 一并收进"顺听到的名单"
           * （这样"顺听 SSID"这一格才反映"空口上到底有哪几个名字"） */
          ssidRemember(ssid, false, 0);
        } else {
          /* beacon 里没有 SSID = **隐藏 AP**。这个信息本身就有价值
           * （它就在你身边，但不想让你知道它叫什么）。 */
          if (!(p->flags & SNIFF_F_HIDDEN_AP)) {
            p->flags |= SNIFF_F_HIDDEN_AP;
            LOGD("PgSniff: 发现隐藏 AP bssid=%s ch=%d rssi=%d", a2, gS.chan, rt.dbm);
          }
        }
      } else if (sub == 4) {
        snprintf(p->role, sizeof(p->role), "STA");
        if (hasSsid) {
          snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
          snprintf(p->ssidKind, sizeof(p->ssidKind), "probe");
          /* 定向 probe 的 SSID = 这台设备**正在找**的网络名（零长是通配探测，已过滤） */
          ssidRemember(ssid, false, 0);
        }
      } else if (sub == 0 || sub == 2) {
        /* ★★ assoc request：**明文的 SSID** —— 隐藏网络的名字就是这么拿到的。
         *    addr1 = 它要连的 AP。 */
        snprintf(p->role, sizeof(p->role), "STA");
        if (hasSsid) {
          snprintf(p->ssid, sizeof(p->ssid), "%s", ssid);
          snprintf(p->ssidKind, sizeof(p->ssidKind), "assoc");
          int isNewSsid = 0;
          ssidRemember(ssid, true, &isNewSsid);
          int ai = peerIdx(a1);
          if (ai >= 0 && (gS.peers[ai].flags & SNIFF_F_HIDDEN_AP) &&
              gS.peers[ai].ssid[0] == 0) {
            snprintf(gS.peers[ai].ssid, sizeof(gS.peers[ai].ssid), "%s", ssid);
            snprintf(gS.peers[ai].ssidKind, sizeof(gS.peers[ai].ssidKind), "assoc");
            gS.peers[ai].flags |= SNIFF_F_RESOLVED;
            LOGD("PgSniff: ★ 解开隐藏 AP 名字 bssid=%s -> '%s'（从 assoc 顺听）", a1, ssid);
          }
          if (isNewSsid) {
            LOGD("PgSniff: 顺听到 SSID '%s'（来自 %s 的 assoc）", ssid, a2);
          }
        }
      }
    }
    gS.gen++;
    pthread_mutex_unlock(&gS.mu);
    if (isNew && p)
      LOGD("PgSniff: 新设备 mac=%s role=%s(%s) ssid='%s'(%s) rssi=%d", a2, p->role,
           mgmtSubName((uint8_t)sub), p->ssid, p->ssidKind, rt.dbm);
    return;
  }

  /* ---- 数据帧：只数数（还带一点"活跃度"信息：谁在传数据） ---- */
  if (type == 2) {
    pthread_mutex_lock(&gS.mu);
    SniffPeer *p = peerTouch(a2, 0);
    if (p) {
      p->nData++;
      if (rt.dbm) p->rssi = rt.dbm;
    }
    gS.dataFrames++;
    pthread_mutex_unlock(&gS.mu);
    return;
  }

  /* ---- 控制帧：只数数（ACK 之类，说明有人在收包） ---- */
  if (type == 1) {
    pthread_mutex_lock(&gS.mu);
    SniffPeer *p = peerTouch(a2, 0);
    if (p) p->nCtrl++;
    pthread_mutex_unlock(&gS.mu);
  }
}

static void *sniffThread(void *arg) {
  (void)arg;
  uint8_t buf[4096];
  while (!gS.stopReq) {
    /* ★ 跳信道：到点就切下一个（recv 有超时，所以切换精度约等于超时值）*/
    if (gS.hop && nowMs() >= gS.hopNextAt) hopNext();
    int n = (int)recv(gS.pktFd, buf, sizeof(buf), 0);
    if (n <= 0) continue;   // SO_RCVTIMEO 到点 = 没有帧，正好回去看 stop 标志
    sniffFrame(buf, n);
  }
  LOGD("PgSniff: 抓包线程退出（共 %ld 帧）", gS.frames);
  return 0;
}

/* ==========================================================================
 * 门面
 * ========================================================================== */
Sniff *Sniff::instance() { return &gSniffApi; }

bool Sniff::start() {
  if (gS.run) return gS.ok;
  gS.stopReq = false;
  gS.frames = 0;
  gS.dataFrames = 0;
  gS.chan = 0;
  gS.nPeer = 0;
  gS.nSsid = 0;
  gS.hiddenResolved = 0;
  gS.gen = 0;
  gS.hop = false;          // 每次重新开始都要显式再开
  gS.hopIdx = 0;
  gS.hopCurChan = 0;
  gS.hopScanned = 0;
  gS.hopRounds = 0;
  gS.hopFail = 0;
  gS.ok = false;

  const char *IF = "wlan1";
  unsigned idx = if_nametoindex(IF);
  gS.hopIfIdx = idx;
  if (!idx) {
    snprintf(gS.state, sizeof(gS.state), "没有 %s 接口", IF);
    LOGD("PgSniff: %s", gS.state);
    return false;
  }

  if (gS.genlFd < 0) gS.genlFd = genlOpen();
  if (gS.genlFd < 0) {
    snprintf(gS.state, sizeof(gS.state), "netlink 打不开（权限？）");
    LOGD("PgSniff: %s", gS.state);
    return false;
  }
  if (gS.family < 0) gS.family = genlFamilyId(gS.genlFd, "nl80211");
  if (gS.family < 0) {
    snprintf(gS.state, sizeof(gS.state), "内核没有 nl80211");
    LOGD("PgSniff: %s", gS.state);
    return false;
  }

  /* 把 wlan1 切成 MONITOR。⚠️ 只碰 wlan1，wlan0/wpa_supplicant 一律不动。 */
  int err = 0;
  int r = nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx,
                     NL80211_IFTYPE_MONITOR, &err);
  if (r != 0) {
    /* 有些驱动要求接口先 up 才能改类型 —— 补一次（实测本板不需要，但便宜） */
    ifSetUp(IF, true);
    r = nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx,
                   NL80211_IFTYPE_MONITOR, &err);
  }
  if (r != 0) {
    snprintf(gS.state, sizeof(gS.state), "切监听失败(err=%d)", err);
    LOGD("PgSniff: SET_INTERFACE %s -> MONITOR 失败 err=%d", IF, err);
    return false;
  }
  LOGD("PgSniff: %s 已切成 MONITOR", IF);
  ifSetUp(IF, true);

  gS.pktFd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (gS.pktFd < 0) {
    snprintf(gS.state, sizeof(gS.state), "原始套接字打不开");
    LOGD("PgSniff: socket(AF_PACKET) 失败 %s -> 还原接口", strerror(errno));
    nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx, NL80211_IFTYPE_STATION, &err);
    return false;
  }
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = (int)idx;
  if (bind(gS.pktFd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
    snprintf(gS.state, sizeof(gS.state), "绑定 %s 失败", IF);
    LOGD("PgSniff: bind 失败 %s -> 还原接口", strerror(errno));
    close(gS.pktFd);
    gS.pktFd = -1;
    nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx, NL80211_IFTYPE_STATION, &err);
    return false;
  }
  /* ⚠️ 250ms 收包超时：让阻塞的 recv 定期返回，stop 标志才来得及生效
   *    （只靠标志 + 永久阻塞的 recv ⇒ 点"停止"要等下一帧才动，看着像卡死）。 */
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 250000;
  setsockopt(gS.pktFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  gS.t0 = nowMs();
  gS.run = true;
  if (pthread_create(&gS.th, 0, sniffThread, 0) != 0) {
    gS.th = 0;
    gS.run = false;
    snprintf(gS.state, sizeof(gS.state), "抓包线程创建失败");
    close(gS.pktFd);
    gS.pktFd = -1;
    nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx, NL80211_IFTYPE_STATION, &err);
    return false;
  }
  gS.haveTh = true;
  gS.ok = true;
  snprintf(gS.state, sizeof(gS.state), "监听中");
  LOGD("PgSniff: ---- 开始抓包（%s / MONITOR）----", IF);
  return true;
}

void Sniff::stop() {
  if (!gS.run && !gS.haveTh) return;
  gS.stopReq = true;
  gS.run = false;
  if (gS.haveTh) {
    pthread_join(gS.th, 0);
    gS.haveTh = false;
    gS.th = 0;
  }
  if (gS.pktFd >= 0) {
    close(gS.pktFd);
    gS.pktFd = -1;
  }
  /* 还原 wlan1 -> STATION（不还的话接口一直停在监听态） */
  if (gS.genlFd >= 0 && gS.family >= 0) {
    unsigned idx = if_nametoindex("wlan1");
    if (idx) {
      int err = 0;
      int r = nl80211Cmd(gS.genlFd, gS.family, NL80211_CMD_SET_INTERFACE, idx,
                         NL80211_IFTYPE_STATION, &err);
      LOGD("PgSniff: 还原 wlan1 -> STATION %s (err=%d)", r == 0 ? "成功" : "失败", err);
    }
  }
  snprintf(gS.state, sizeof(gS.state), "已停止");
  LOGD("PgSniff: ---- 停止抓包（共 %ld 帧 / %d 个发射者 / %d 个 SSID）----", gS.frames,
       gS.nPeer, gS.nSsid);
  gS.ok = false;
}

bool Sniff::active() const { return gS.run; }
const char *Sniff::stateText() const { return gS.state; }
const char *Sniff::ifName() const { return "wlan1"; }
int Sniff::channel() const { return gS.chan; }
long Sniff::frames() const { return gS.frames; }
long Sniff::dataFrames() const { return gS.dataFrames; }
int Sniff::generation() const { return gS.gen; }
int Sniff::hiddenResolved() const { return gS.hiddenResolved; }

int Sniff::elapsedSec() const {
  if (!gS.run || !gS.t0) return 0;
  return (int)((nowMs() - gS.t0) / 1000);
}

int Sniff::peerCount() const { return gS.nPeer; }

bool Sniff::peerCopy(int i, SniffPeer *out) const {
  if (!out) return false;
  pthread_mutex_lock((pthread_mutex_t *)&gS.mu);
  bool ok = (i >= 0 && i < gS.nPeer);
  if (ok) {
    *out = gS.peers[i];
    /* ⚠️ "距最后活动多少秒"在**读的时候现算**：worker 里存的是时刻。
     *   存"秒数"的话它只会在有新帧时才更新 —— 静默设备会永远显示"0 秒前"。 */
    long dt = gS.peerLast[i] ? (nowMs() - gS.peerLast[i]) : 0;
    out->lastSec = (int)(dt / 1000);
  }
  pthread_mutex_unlock((pthread_mutex_t *)&gS.mu);
  return ok;
}

int Sniff::ssidCount() const { return gS.nSsid; }

const char *Sniff::ssidAt(int i) const {
  if (i < 0 || i >= gS.nSsid) return "";
  return gS.ssids[i];
}

/* ==========================================================================
 * 全信道跳扫
 * ========================================================================== */
bool Sniff::setHop(bool on) {
  if (!gS.run) {
    LOGD("PgSniff: 还没开始抓包，不能开跳信道");
    return false;
  }
  if (on == gS.hop) return true;
  gS.hop = on;
  if (on) {
    gS.hopIdx = 0;
    gS.hopScanned = 0;
    gS.hopRounds = 0;
    gS.hopFail = 0;
    for (int i = 0; i < kHopChanN; ++i) gS.chanFrames[i] = 0;
    hopApply();   /* 立刻切到第一个信道（不等 worker 下一拍） */
    LOGD("PgSniff: 跳信道 ON（%d 个信道，每信道 %d ms）", kHopChanN, (int)gS.hopDwellMs);
  } else {
    LOGD("PgSniff: 跳信道 OFF（停在 CH%d，累计切换 %d 次 / %d 轮 / 失败 %ld 次）",
         gS.hopCurChan, gS.hopScanned, gS.hopRounds, gS.hopFail);
    if (gS.pktFd >= 0) {
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 250000;   /* 非跳信道模式恢复省 CPU 的超时 */
      setsockopt(gS.pktFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
  }
  return true;
}

bool Sniff::hopActive() const { return gS.hop; }
int Sniff::hopCurChan() const { return gS.hopCurChan; }
int Sniff::hopScanned() const { return gS.hopScanned; }
int Sniff::hopRounds() const { return gS.hopRounds; }
int Sniff::hopChanTotal() const { return kHopChanN; }
int Sniff::hopChanNumAt(int i) const {
  return (i >= 0 && i < kHopChanN) ? kHopChans[i] : 0;
}
long Sniff::hopChanFrames(int i) const {
  return (i >= 0 && i < kHopChanN) ? gS.chanFrames[i] : 0;
}
long Sniff::hopFail() const { return gS.hopFail; }

void Sniff::logDump() {
  pthread_mutex_lock(&gS.mu);
  LOGD("PgSniff: dump 共 %d 个发射者 / %ld 帧（数据 %ld）/ 信道 %d / 运行 %d 秒"
       " / 跳信道=%d 当前CH%d 已扫%d 轮%d 失败%ld",
       gS.nPeer, gS.frames, gS.dataFrames, gS.chan, elapsedSec(), gS.hop ? 1 : 0,
       gS.hopCurChan, gS.hopScanned, gS.hopRounds, gS.hopFail);
  if (gS.hop) {
    /* 每个信道的帧数 = "空口占用地图"（哪个信道最挤一眼看出） */
    char line[512] = {0};
    for (int i = 0; i < kHopChanN; ++i) {
      char one[24];
      snprintf(one, sizeof(one), "CH%d=%ld ", kHopChans[i], gS.chanFrames[i]);
      if (strlen(line) + strlen(one) < sizeof(line) - 1) strcat(line, one);
    }
    LOGD("PgSniff: 信道帧数 %s", line);
  }
  for (int i = 0; i < gS.nPeer; ++i) {
    LOGD("PgSniff: peer#%d mac=%s role=%s rssi=%d mgmt=%d data=%d ctrl=%d flags=%d "
         "ssid='%s'(%s)",
         i, gS.peers[i].mac, gS.peers[i].role, gS.peers[i].rssi, gS.peers[i].nMgmt,
         gS.peers[i].nData, gS.peers[i].nCtrl, gS.peers[i].flags, gS.peers[i].ssid,
         gS.peers[i].ssidKind);
  }
  for (int i = 0; i < gS.nSsid; ++i) {
    LOGD("PgSniff: ssid#%d '%s' fromAssoc=%d", i, gS.ssids[i], gS.ssidFromAssoc[i]);
  }
  pthread_mutex_unlock(&gS.mu);
}

}  // namespace pg
