/*
 * nlprobe.c - 无线能力探针（nl80211 / generic-netlink，**独立可执行**）
 *
 * 干什么：把"这块板子的 WiFi 驱动到底支持哪些接口类型 / 能不能开监听模式"
 *   从"查 .ko 符号猜"变成"驱动亲口声明"。
 *
 * 用法（默认只读，不动任何状态）：
 *   ./nlprobe                 列出 wiphy 支持的接口类型 + 每个网卡的当前类型/信道
 *   ./nlprobe mon <phy>       尝试在 phy 上新建 monitor 接口 mon0（**写操作**）
 *   ./nlprobe sniff <秒>      在 mon0 上按 wlan0 的信道收帧，统计"有多少个不同的发射 MAC"
 *   ./nlprobe del <ifname>    删掉接口（收尾）
 *
 * 编译（与工程其它 tools/*.c 同一套）：
 *   arm-unknown-linux-musleabihf-gcc -static -O2 -o nlprobe tools/nlprobe.c
 *
 * ⚠️ 为什么需要它：板子上**没有 iw / iwconfig / wpa_cli**（`ls /bin /sbin /usr/bin` 只有
 *    ifconfig / ping / wpa_supplicant），要看 nl80211 能力只能自己写。
 *    zkgui 以 uid 0 跑，所以普通进程也有权限查（建接口需要 CAP_NET_ADMIN，root 有）。
 *
 * ⚠️ 与 zknet 的关系：本工具**只查询 + 可选新建一个独立的 monitor 网卡**，
 *    不碰 wlan0 的 STA 状态、不碰 wpa_supplicant。默认模式（不带参数）**只读**。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

#ifndef NLA_ALIGNTO
#define NLA_ALIGNTO 4
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#define NLA_HDRLEN ((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif

static int g_family = -1;
static unsigned g_seq = 1;

/* ---------------- 小工具 ---------------- */

static const char *iftypeName(int t) {
  switch (t) {
    case NL80211_IFTYPE_ADHOC: return "ADHOC";
    case NL80211_IFTYPE_STATION: return "STATION";
    case NL80211_IFTYPE_AP: return "AP";
    case NL80211_IFTYPE_AP_VLAN: return "AP_VLAN";
    case NL80211_IFTYPE_WDS: return "WDS";
    case NL80211_IFTYPE_MONITOR: return "MONITOR";
    case NL80211_IFTYPE_MESH_POINT: return "MESH_POINT";
    case NL80211_IFTYPE_P2P_CLIENT: return "P2P_CLIENT";
    case NL80211_IFTYPE_P2P_GO: return "P2P_GO";
    case NL80211_IFTYPE_P2P_DEVICE: return "P2P_DEVICE";
    case NL80211_IFTYPE_OCB: return "OCB";
    case NL80211_IFTYPE_NAN: return "NAN";
    default: return "?";
  }
}

static int openGenl(void) {
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
  if (fd < 0) { perror("socket(NETLINK_GENERIC)"); return -1; }
  struct sockaddr_nl sa;
  memset(&sa, 0, sizeof(sa));
  sa.nl_family = AF_NETLINK;
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); close(fd); return -1; }
  return fd;
}

/** 发一条 genl 消息（attr 用 "type, len, data" 三元组尾部追加，简化起见手工拼） */
struct Msg {
  struct nlmsghdr n;
  struct genlmsghdr g;
  char buf[2048];
  int len;
};

static void msgInit(struct Msg *m, int cmd, int flags) {
  memset(m, 0, sizeof(*m));
  m->n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
  m->n.nlmsg_type = g_family;
  m->n.nlmsg_flags = NLM_F_REQUEST | flags;
  m->n.nlmsg_seq = g_seq++;
  m->g.cmd = cmd;
  m->g.version = 1;
  m->len = NLMSG_ALIGN(m->n.nlmsg_len);
}

static void putU32(struct Msg *m, int type, unsigned v) {
  struct nlattr *a = (struct nlattr *)((char *)&m->n + m->len);
  a->nla_type = type;
  a->nla_len = NLA_HDRLEN + 4;
  *(unsigned *)((char *)a + NLA_HDRLEN) = v;
  m->len += NLA_ALIGN(a->nla_len);
  m->n.nlmsg_len = m->len;
}

static void putStr(struct Msg *m, int type, const char *s) {
  struct nlattr *a = (struct nlattr *)((char *)&m->n + m->len);
  int l = (int)strlen(s) + 1;
  a->nla_type = type;
  a->nla_len = NLA_HDRLEN + l;
  memcpy((char *)a + NLA_HDRLEN, s, l);
  m->len += NLA_ALIGN(a->nla_len);
  m->n.nlmsg_len = m->len;
}

static int sendMsg(int fd, struct Msg *m) {
  struct sockaddr_nl dst;
  memset(&dst, 0, sizeof(dst));
  dst.nl_family = AF_NETLINK;
  return sendto(fd, &m->n, m->n.nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/** 收消息，cb 对每条 NLMSG 调用；返回 0 正常收完 */
static int recvMsgs(int fd, int (*cb)(struct nlmsghdr *, void *), void *ctx, int multi) {
  char buf[8192];
  for (;;) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) { if (errno == EINTR) continue; perror("recv"); return -1; }
    struct nlmsghdr *h = (struct nlmsghdr *)buf;
    while (NLMSG_OK(h, n)) {
      if (h->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
        if (e->error != 0) fprintf(stderr, "  [netlink] 错误 %d (%s)\n", e->error, strerror(-e->error));
        return e->error;
      }
      if (h->nlmsg_type == NLMSG_DONE) return 0;
      if (cb) cb(h, ctx);
      h = NLMSG_NEXT(h, n);
    }
    if (!multi) return 0;
  }
}

/* ---------------- 1) 解析 nl80211 family ---------------- */

struct FamCtx { int id; };
static int famCb(struct nlmsghdr *h, void *ctx) {
  struct FamCtx *c = (struct FamCtx *)ctx;
  struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
  int len = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
  struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
  for (; len >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len;
       len -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
    if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID)
      c->id = *(unsigned short *)((char *)a + NLA_HDRLEN);
  }
  return 0;
}

static int resolveFamily(int fd, const char *name) {
  struct Msg m;
  msgInit(&m, CTRL_CMD_GETFAMILY, 0);
  /* ⚠️ 查 family 这一步**还没拿到 family id**，所以 nlmsg_type 必须是
   *    GENL_ID_CTRL（=0x10），不是 g_family（那时是 -1）——
   *    发错类型内核直接回 -ENOENT，看起来像"内核没编 cfg80211"（踩过）。 */
  m.n.nlmsg_type = GENL_ID_CTRL;
  putStr(&m, CTRL_ATTR_FAMILY_NAME, name);
  if (sendMsg(fd, &m) < 0) { perror("send"); return -1; }
  struct FamCtx c = { -1 };
  recvMsgs(fd, famCb, &c, 0);
  return c.id;
}

/* ---------------- 2) GET_WIPHY：支持哪些接口类型 ---------------- */

struct WiphyCtx { int want; int seen; };
static int wiphyCb(struct nlmsghdr *h, void *ctx) {
  struct WiphyCtx *c = (struct WiphyCtx *)ctx;
  struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
  int len = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
  struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);

  int idx = -1;
  char name[64] = "";
  char iftypes[512] = "";
  for (; len >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len;
       len -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
    int t = a->nla_type & NLA_TYPE_MASK;
    if (t == NL80211_ATTR_WIPHY) idx = *(unsigned *)((char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_WIPHY_NAME)
      snprintf(name, sizeof(name), "%s", (char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_SUPPORTED_IFTYPES) {
      /* 嵌套属性：每个子属性的 type 就是 iftype 值 */
      int slen = a->nla_len - NLA_HDRLEN;
      struct nlattr *s = (struct nlattr *)((char *)a + NLA_HDRLEN);
      for (; slen >= (int)NLA_HDRLEN && s->nla_len >= NLA_HDRLEN && s->nla_len <= slen;
           slen -= NLA_ALIGN(s->nla_len), s = (struct nlattr *)((char *)s + NLA_ALIGN(s->nla_len))) {
        int it = s->nla_type & NLA_TYPE_MASK;
        char one[40];
        snprintf(one, sizeof(one), "%s(%d) ", iftypeName(it), it);
        strncat(iftypes, one, sizeof(iftypes) - strlen(iftypes) - 1);
      }
    }
  }
  if (c->want >= 0 && idx != c->want) return 0;
  /* ⚠️ 本驱动对 GET_WIPHY dump 会回多条消息，只有一条带 SUPPORTED_IFTYPES
   *   ⇒ 没有 iftypes 的直接跳过，否则同一 wiphy 被打印好几遍（看着像有多个网卡）。 */
  if (!iftypes[0]) return 0;
  c->seen++;
  printf("  wiphy%d  name=%s\n", idx, name[0] ? name : "?");
  printf("    支持的接口类型: %s\n", iftypes[0] ? iftypes : "(无)");
  int hasMon = strstr(iftypes, "MONITOR") != NULL;
  printf("    ⇒ MONITOR 模式: %s\n", hasMon ? "**驱动声明支持**" : "**未声明支持**");
  return 0;
}

/* ---------------- 3) GET_INTERFACE：每个网卡的当前状态 ---------------- */

struct IFCtx { int n; };
static int ifaceCb(struct nlmsghdr *h, void *ctx) {
  struct IFCtx *c = (struct IFCtx *)ctx;
  struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
  int len = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
  struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
  int idx = -1, type = -1, freq = 0, wiphy = -1;
  char name[64] = "", mac[32] = "";
  for (; len >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len;
       len -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
    int t = a->nla_type & NLA_TYPE_MASK;
    if (t == NL80211_ATTR_IFINDEX) idx = *(unsigned *)((char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_IFTYPE) type = *(unsigned *)((char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_WIPHY_FREQ) freq = *(unsigned *)((char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_WIPHY) wiphy = *(unsigned *)((char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_IFNAME)
      snprintf(name, sizeof(name), "%s", (char *)a + NLA_HDRLEN);
    else if (t == NL80211_ATTR_MAC) {
      unsigned char *p = (unsigned char *)a + NLA_HDRLEN;
      snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]);
    }
  }
  c->n++;
  printf("  %-8s ifindex=%-3d type=%-10s wiphy=%d mac=%s freq=%s\n", name, idx,
         iftypeName(type), wiphy, mac[0] ? mac : "-",
         freq ? ({ static char b[16]; snprintf(b, sizeof(b), "%dMHz", freq); b; }) : "-");
  return 0;
}

/* ---------------- 4) 建/删接口 ---------------- */

static int newMonitor(int fd, const char *phyName, const char *ifname) {
  struct Msg m;
  msgInit(&m, NL80211_CMD_NEW_INTERFACE, NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL);
  putStr(&m, NL80211_ATTR_WIPHY_NAME, phyName);
  putStr(&m, NL80211_ATTR_IFNAME, ifname);
  putU32(&m, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_MONITOR);
  if (sendMsg(fd, &m) < 0) { perror("send NEW_INTERFACE"); return -1; }
  return recvMsgs(fd, NULL, NULL, 0);
}

/**
 * 把**已存在**的网卡改成另一种接口类型（NL80211_CMD_SET_INTERFACE）。
 * 为什么要它：`NEW_INTERFACE` 新建 vif 在本驱动上返回 EINVAL（很可能撞了"vif 数量上限"，
 * 因为驱动已经开了两个 STATION 接口）⇒ 那就"复用"一个闲置的（wlan1 是 P2P 用的第二个口，
 * 平时没有流量），改成 monitor 试试。
 */
static int setIfaceType(int fd, const char *ifname, int type) {
  unsigned ifi = if_nametoindex(ifname);
  if (!ifi) { fprintf(stderr, "  %s 不存在\n", ifname); return -1; }
  struct Msg m;
  msgInit(&m, NL80211_CMD_SET_INTERFACE, NLM_F_ACK);
  putU32(&m, NL80211_ATTR_IFINDEX, ifi);
  putU32(&m, NL80211_ATTR_IFTYPE, (unsigned)type);
  if (sendMsg(fd, &m) < 0) { perror("send SET_INTERFACE"); return -1; }
  return recvMsgs(fd, NULL, NULL, 0);
}

static int delIface(int fd, const char *ifname) {
  struct Msg m;
  msgInit(&m, NL80211_CMD_DEL_INTERFACE, NLM_F_ACK);
  putStr(&m, NL80211_ATTR_IFNAME, ifname);
  if (sendMsg(fd, &m) < 0) { perror("send DEL_INTERFACE"); return -1; }
  return recvMsgs(fd, NULL, NULL, 0);
}

/** 把 monitor 接口调到指定频率（monitor 必须停在某个信道上才收得到帧） */
static int setChannel(int fd, const char *ifname, int freq) {
  struct Msg m;
  msgInit(&m, NL80211_CMD_SET_CHANNEL, NLM_F_ACK);
  unsigned ifi = if_nametoindex(ifname);
  if (!ifi) { fprintf(stderr, "  %s 不存在\n", ifname); return -1; }
  putU32(&m, NL80211_ATTR_IFINDEX, ifi);
  putU32(&m, NL80211_ATTR_WIPHY_FREQ, (unsigned)freq);
  putU32(&m, NL80211_ATTR_WIPHY_CHANNEL_TYPE, NL80211_CHAN_HT20);
  if (sendMsg(fd, &m) < 0) { perror("send SET_CHANNEL"); return -1; }
  return recvMsgs(fd, NULL, NULL, 0);
}

/** 取 wlan0 当前所在频率（= 我们连的那个 AP 的信道） */
static int queryFreqOf(const char *ifname) {
  int fd = openGenl();
  if (fd < 0) return 0;
  g_family = resolveFamily(fd, "nl80211");
  if (g_family < 0) { close(fd); return 0; }
  unsigned ifi = if_nametoindex(ifname);
  struct Msg m;
  msgInit(&m, NL80211_CMD_GET_INTERFACE, NLM_F_ACK);
  putU32(&m, NL80211_ATTR_IFINDEX, ifi);
  if (sendMsg(fd, &m) < 0) { close(fd); return 0; }
  /* 复用 ifaceCb 不方便取返回值，这里单独解析 */
  char buf[8192];
  int n = recv(fd, buf, sizeof(buf), 0);
  close(fd);
  if (n <= 0) return 0;
  struct nlmsghdr *h = (struct nlmsghdr *)buf;
  if (h->nlmsg_type == NLMSG_ERROR) return 0;
  struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
  int len = h->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
  struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
  for (; len >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len;
       len -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
    if ((a->nla_type & NLA_TYPE_MASK) == NL80211_ATTR_WIPHY_FREQ)
      return *(unsigned *)((char *)a + NLA_HDRLEN);
  }
  return 0;
}

/** 信道号 -> 频率（2.4G / 5G 常用信道）。 */
static int chanToFreq(int ch) {
  if (ch == 14) return 2484;
  if (ch >= 1 && ch <= 13) return 2407 + ch * 5;     /* ch1=2412 ch6=2437 ch11=2462 */
  if (ch >= 36 && ch <= 177) return 5000 + ch * 5;   /* ch36=5180 ch149=5745 */
  return 0;
}

/* ---------------- 5) 在 monitor 口上抓帧统计 ---------------- */

/** 从 radiotap + 802.11 头里取"发射者 MAC"（addr2，偏移固定 10） */
struct Sniff {
  unsigned long long total;
  unsigned long long mgmt, ctrl, data, other;
  struct { unsigned char mac[6]; unsigned long long n; } top[32];
  int ntop;
  char ssid[24][34];      /* beacon / probe-resp 里明文带出来的网络名 */
  int nssid;
};

/** 把 SSID 记进列表（去重）。 */
static void sniffSsid(struct Sniff *s, const char *name) {
  if (!name || !name[0]) return;
  for (int i = 0; i < s->nssid; i++)
    if (strcmp(s->ssid[i], name) == 0) return;
  if (s->nssid < 24) snprintf(s->ssid[s->nssid++], 34, "%s", name);
}

/**
 * 从管理帧里取 SSID（tag 0）。subtype 8=beacon / 5=probe-resp / 4=probe-req。
 * ⚠️ 隐藏 AP 的 SSID element 长度是 0 ⇒ 不能把空串当名字。
 */
static void sniffParseSsid(struct Sniff *s, const unsigned char *fr, int flen, int sub) {
  if (sub != 8 && sub != 5 && sub != 4) return;
  int off = (sub == 4) ? 24 : 36;   /* probe-req 头 24 字节；beacon/probe-resp 是 24+12 */
  if (off >= flen) return;
  const unsigned char *p = fr + off;
  const unsigned char *end = fr + flen;
  while (p + 2 <= end) {
    int id = p[0], ln = p[1];
    if (p + 2 + ln > end) break;
    if (id == 0 && ln > 0 && ln <= 32) {
      char nm[34];
      memcpy(nm, p + 2, ln);
      nm[ln] = 0;
      int ok = 1;
      for (int i = 0; i < ln; i++)
        if ((unsigned char)nm[i] < 0x20 || (unsigned char)nm[i] == 0x7f) { ok = 0; break; }
      if (ok) sniffSsid(s, nm);
      return;
    }
    p += 2 + ln;
  }
}

static void sniffCount(struct Sniff *s, unsigned char *mac) {
  for (int i = 0; i < s->ntop; i++)
    if (memcmp(s->top[i].mac, mac, 6) == 0) { s->top[i].n++; return; }
  if (s->ntop < 32) {
    memcpy(s->top[s->ntop].mac, mac, 6);
    s->top[s->ntop].n = 1;
    s->ntop++;
  }
}

static int sniffRun(const char *ifname, int seconds, int freq) {
  /* ETH_P_ALL = 0x0003，网络字节序 = 0x0300 */
  int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) { perror("socket(AF_PACKET)"); return -1; }
  unsigned ifi = if_nametoindex(ifname);
  if (!ifi) { fprintf(stderr, "%s 不存在\n", ifname); close(fd); return -1; }
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = (int)ifi;
  if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind(AF_PACKET)"); close(fd); return -1; }

  printf("  在 %s 上抓帧 %d 秒（频率 %dMHz）...\n", ifname, seconds, freq);
  struct Sniff st;
  memset(&st, 0, sizeof(st));
  time_t t0 = time(NULL);
  while (time(NULL) - t0 < seconds) {
    unsigned char buf[4096];
    int n = (int)recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) continue;
    st.total++;
    if (n < 4) continue;
    int rt = buf[2] | (buf[3] << 8);            /* radiotap 头长度 */
    if (rt <= 0 || rt + 10 > n) continue;
    unsigned char *f = buf + rt;                 /* 802.11 帧起始 */
    int type = (f[0] >> 2) & 0x3;
    if (type == 0) st.mgmt++;
    else if (type == 1) st.ctrl++;
    else if (type == 2) st.data++;
    else st.other++;
    if (rt + 16 <= n) sniffCount(&st, f + 10);   /* addr2 = 发射者 */
    if (type == 0 && rt + 16 <= n) sniffParseSsid(&st, f, n - rt, (f[0] >> 4) & 0xF);
  }
  close(fd);
  printf("  总帧数 %llu（管理 %llu / 控制 %llu / 数据 %llu / 其他 %llu）\n", st.total,
         st.mgmt, st.ctrl, st.data, st.other);
  printf("  出现的不同发射 MAC：%d 个\n", st.ntop);
  printf("  顺听到的 SSID（%d 个）：", st.nssid);
  for (int i = 0; i < st.nssid; i++) printf("%s%s", i ? " | " : "", st.ssid[i]);
  printf("\n");
  for (int i = 0; i < st.ntop && i < 32; i++) {
    unsigned char *m = st.top[i].mac;
    printf("    %02x:%02x:%02x:%02x:%02x:%02x  %llu 帧\n", m[0], m[1], m[2], m[3], m[4], m[5],
           st.top[i].n);
  }
  return 0;
}

/**
 * 跳信道轮扫：monitor 口在多个信道上轮流驻留抓帧。
 *
 * ⚠️ 只有在**射频空闲**（STA 断开关联 / 接口 down）时 SET_CHANNEL 才可能成功 ——
 *    单射频驱动下 wlan0 一关联，整块射频就锁在那个信道上（返回 -EBUSY）。
 *    所以本模式是"断开网络之后"才用的。
 */
static int hopRun(int gfd, const char *ifname, int dwellMs, int rounds,
                  const int *chs, int nch) {
  int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (fd < 0) { perror("socket(AF_PACKET)"); return -1; }
  unsigned ifi = if_nametoindex(ifname);
  if (!ifi) { fprintf(stderr, "%s 不存在\n", ifname); close(fd); return -1; }
  struct sockaddr_ll sll;
  memset(&sll, 0, sizeof(sll));
  sll.sll_family = AF_PACKET;
  sll.sll_protocol = htons(ETH_P_ALL);
  sll.sll_ifindex = (int)ifi;
  if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); close(fd); return -1; }
  struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  printf("  跳信道轮扫：%s，每信道 %d ms，%d 轮，共 %d 个信道\n", ifname, dwellMs, rounds, nch);
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < nch; i++) {
      int f = chanToFreq(chs[i]);
      if (f <= 0) continue;
      int dr = setChannel(gfd, ifname, f);
      if (dr != 0 && i == 0 && r == 0)
        printf("  ⚠️ SET_CHANNEL %dMHz 失败(%d) —— 射频被 STA 占着？\n", f, dr);
      unsigned long long nframe = 0, nmgmt = 0;
      struct Sniff st; memset(&st, 0, sizeof(st));
      struct timeval t0, tn; gettimeofday(&t0, 0);
      while (1) {
        gettimeofday(&tn, 0);
        int el = (int)((tn.tv_sec - t0.tv_sec) * 1000 + (tn.tv_usec - t0.tv_usec) / 1000);
        if (el >= dwellMs) break;
        unsigned char buf[4096];
        int n = (int)recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) continue;
        if (n < 4) continue;
        int rt = buf[2] | (buf[3] << 8);
        if (rt <= 0 || rt + 10 > n) continue;
        unsigned char *fr = buf + rt;
        int type = (fr[0] >> 2) & 0x3;
        nframe++;
        if (type == 0) nmgmt++;
        if (rt + 16 <= n) sniffCount(&st, fr + 10);
        if (type == 0 && rt + 16 <= n) sniffParseSsid(&st, fr, n - rt, (fr[0] >> 4) & 0xF);
      }
      printf("    CH%-3d (%4dMHz): %6llu 帧 (管理 %llu) / %d 发射者 / SSID %d",
             chs[i], f, nframe, nmgmt, st.ntop, st.nssid);
      for (int k = 0; k < st.nssid; k++) printf("  [%s]", st.ssid[k]);
      printf("\n");
      fflush(stdout);
    }
  }
  close(fd);
  return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
  int fd = openGenl();
  if (fd < 0) return 1;
  g_family = resolveFamily(fd, "nl80211");
  if (g_family < 0) { fprintf(stderr, "nl80211 family 解析失败（内核没编 cfg80211？）\n"); return 1; }
  printf("nl80211 family id = %d\n", g_family);

  const char *mode = (argc > 1) ? argv[1] : "";

  if (!strcmp(mode, "mon")) {
    const char *phy = (argc > 2) ? argv[2] : "phy0";
    const char *ifn = (argc > 3) ? argv[3] : "mon0";
    printf("[写] 尝试在 %s 上新建 monitor 接口 %s ...\n", phy, ifn);
    int r = newMonitor(fd, phy, ifn);
    printf("  ⇒ 结果 %d（0=成功）\n", r);
    close(fd);
    return r == 0 ? 0 : 2;
  }
  if (!strcmp(mode, "settype")) {
    const char *ifn = (argc > 2) ? argv[2] : "wlan1";
    const char *tn = (argc > 3) ? argv[3] : "monitor";
    int t = !strcmp(tn, "monitor") ? NL80211_IFTYPE_MONITOR
            : (!strcmp(tn, "station") ? NL80211_IFTYPE_STATION : -1);
    if (t < 0) { fprintf(stderr, "只支持 monitor|station\n"); return 1; }
    printf("[写] 把 %s 的类型改成 %s ...\n", ifn, tn);
    int r = setIfaceType(fd, ifn, t);
    printf("  ⇒ 结果 %d（0=成功）\n", r);
    close(fd);
    return r == 0 ? 0 : 2;
  }
  if (!strcmp(mode, "del")) {
    const char *ifn = (argc > 2) ? argv[2] : "mon0";
    int r = delIface(fd, ifn);
    printf("删除 %s ⇒ %d\n", ifn, r);
    close(fd);
    return 0;
  }
  if (!strcmp(mode, "setchan")) {
    const char *ifn = (argc > 2) ? argv[2] : "wlan1";
    int ch = (argc > 3) ? atoi(argv[3]) : 6;
    int f = chanToFreq(ch);
    unsigned ifi = if_nametoindex(ifn);
    printf("[写] %s -> CH%d (%dMHz)，ifindex=%u\n", ifn, ch, f, ifi);
    int r = setChannel(fd, ifn, f);
    printf("  ⇒ 结果 %d（0=成功；-16=EBUSY 射频被占；-22=EINVAL）\n", r);
    close(fd);
    return r == 0 ? 0 : 2;
  }
  if (!strcmp(mode, "hop")) {
    const char *ifn = (argc > 2) ? argv[2] : "wlan1";
    int dwell = (argc > 3) ? atoi(argv[3]) : 1500;
    int rounds = (argc > 4) ? atoi(argv[4]) : 1;
    static int def[] = {1, 6, 11, 36, 40, 44, 48, 149, 153, 157, 161, 165};
    int chs[32]; int nch = 0;
    for (int i = 5; i < argc && nch < 32; i++) chs[nch++] = atoi(argv[i]);
    const int *use = chs; int n = nch;
    if (n == 0) { use = def; n = (int)(sizeof(def) / sizeof(def[0])); }
    int r = hopRun(fd, ifn, dwell, rounds, use, n);
    close(fd);
    return r;
  }
  if (!strcmp(mode, "sniff")) {
    int sec = (argc > 2) ? atoi(argv[2]) : 10;
    const char *ifn = (argc > 3) ? argv[3] : "mon0";
    int f = queryFreqOf("wlan0");
    printf("wlan0 当前频率 = %dMHz\n", f);
    if (f > 0) {
      int dr = setChannel(fd, ifn, f);
      printf("把 %s 调到 %dMHz ⇒ %d\n", ifn, f, dr);
    }
    close(fd);
    return sniffRun(ifn, sec, f);
  }

  /* 默认：只读 */
  printf("\n[1] wiphy 支持的接口类型\n");
  {
    struct Msg m;
    msgInit(&m, NL80211_CMD_GET_WIPHY, NLM_F_DUMP);
    struct WiphyCtx c = { -1, 0 };
    sendMsg(fd, &m);
    recvMsgs(fd, wiphyCb, &c, 1);
  }
  printf("\n[2] 现有网卡\n");
  {
    struct Msg m;
    msgInit(&m, NL80211_CMD_GET_INTERFACE, NLM_F_DUMP);
    struct IFCtx c = { 0 };
    sendMsg(fd, &m);
    recvMsgs(fd, ifaceCb, &c, 1);
  }
  close(fd);
  return 0;
}
