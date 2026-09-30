/*
 * PgLan.cpp - 局域网设备扫描（主动探测）实现
 *
 * 全流程见 PgLan.h 的注释。这里只强调三条实现上的硬约束：
 *
 * ⚠️ ① 绝不碰控件：整个 worker 线程只写自己的结构（加锁）+ LOGD。
 * ⚠️ ② 必须可中断：stop() 置标志 + shutdown 掉 socket，让阻塞的 recv/select 立刻返回；
 *      只在每批之间检查标志的话，"停止"要等十几秒才生效（用户会以为按钮坏了）。
 * ⚠️ ③ 所有 socket 都要有超时：本板单射频、信号可能很弱，connect 挂住不动很常见。
 */
#define _GNU_SOURCE   // strcasestr / strncasecmp（musl 要显式打开）
#include "platform/PgLan.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "utils/Log.h"

namespace pg {

static long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ==========================================================================
 * MAC 厂商（OUI 精选表）
 *
 * ⚠️ 这是**精选子集**，不是全量 IEEE 库（全量 3 万多条 ≈ 数百 KB，塞进 7.6MB 的
 *    /res 分区不划算）。选表原则：① 安防/摄像头厂商（本应用的正事）尽量全；
 *    ② 常见网络设备/消费电子厂商挑"我确定"的；③ 不确定的**宁可不要** ——
 *    给错厂商名比给"未知"更糟（会误导判断）。
 *    未命中一律显示"未知厂商"，不影响风险判定（判定主要靠端口 + 协议指纹）。
 * ========================================================================== */
struct OuiRec { const char *oui; const char *name; int cam; /* 1 = 安防/摄像头系 */ };

static const OuiRec kOui[] = {
    /* ---- 安防 / 摄像头（命中即"值得看一眼"） ---- */
    {"44:19:b6", "海康威视", 1}, {"bc:ad:28", "海康威视", 1}, {"c0:56:e3", "海康威视", 1},
    {"4c:bd:8f", "海康威视", 1}, {"28:57:be", "海康威视", 1}, {"8c:e7:48", "海康威视", 1},
    {"a4:14:37", "海康威视", 1}, {"18:68:cb", "海康威视", 1},
    {"3c:ef:8c", "大华", 1},     {"4c:11:bf", "大华", 1},     {"e0:50:8b", "大华", 1},
    {"90:02:a9", "大华", 1},     {"6c:1c:71", "大华", 1},
    {"00:40:8c", "Axis", 1},     {"00:02:d1", "Vivotek", 1},  {"00:62:6e", "Foscam", 1},
    /* ---- IoT 模组（大量廉价 IPC / 智能插座都用它，命中给"关注"而不是"可疑"） ---- */
    {"24:0a:c4", "Espressif", 0}, {"30:ae:a4", "Espressif", 0}, {"3c:71:bf", "Espressif", 0},
    {"84:f3:eb", "Espressif", 0}, {"a4:cf:12", "Espressif", 0}, {"7c:df:a1", "Espressif", 0},
    {"48:3f:da", "Espressif", 0}, {"5c:cf:7f", "Espressif", 0}, {"8c:aa:b5", "Espressif", 0},
    {"b4:e6:2d", "Espressif", 0}, {"cc:50:e3", "Espressif", 0}, {"dc:4f:22", "Espressif", 0},
    {"ec:fa:bc", "Espressif", 0}, {"18:fe:34", "Espressif", 0}, {"24:62:ab", "Espressif", 0},
    {"3c:61:05", "Espressif", 0}, {"a0:20:a6", "Espressif", 0}, {"ac:d0:74", "Espressif", 0},
    {"e8:db:84", "Espressif", 0}, {"10:52:1c", "涂鸦模组", 0},
    {"b8:27:eb", "树莓派", 0},    {"dc:a6:32", "树莓派", 0},    {"e4:5f:01", "树莓派", 0},
    {"28:cd:c1", "树莓派", 0},    {"d8:3a:dd", "树莓派", 0},
    /* ---- 网络设备 ---- */
    {"50:c7:bf", "TP-LINK", 0},  {"14:cc:20", "TP-LINK", 0},  {"b0:4e:26", "TP-LINK", 0},
    {"9c:53:22", "TP-LINK", 0},  {"ec:08:6b", "TP-LINK", 0},  {"a4:2b:b0", "TP-LINK", 0},
    {"c0:25:e9", "TP-LINK", 0},  {"f4:ec:38", "TP-LINK", 0},  {"3c:46:d8", "TP-LINK", 0},
    {"04:d4:c4", "ASUS", 0},     {"1c:87:2c", "ASUS", 0},     {"2c:fd:a1", "ASUS", 0},
    {"38:d5:47", "ASUS", 0},     {"50:46:5d", "ASUS", 0},     {"70:4d:7b", "ASUS", 0},
    {"ac:22:0b", "ASUS", 0},     {"d8:50:e6", "ASUS", 0},
    {"00:14:6c", "NETGEAR", 0},  {"20:4e:7f", "NETGEAR", 0},  {"2c:30:33", "NETGEAR", 0},
    {"44:94:fc", "NETGEAR", 0},  {"9c:3d:cf", "NETGEAR", 0},  {"a0:40:a0", "NETGEAR", 0},
    {"c4:3d:c7", "NETGEAR", 0},  {"e0:46:9a", "NETGEAR", 0},  {"e8:fc:af", "NETGEAR", 0},
    {"00:1b:11", "D-Link", 0},   {"14:d6:4d", "D-Link", 0},   {"1c:7e:e5", "D-Link", 0},
    {"28:10:7b", "D-Link", 0},   {"34:08:04", "D-Link", 0},   {"5c:d9:98", "D-Link", 0},
    {"78:54:2e", "D-Link", 0},   {"84:c9:b2", "D-Link", 0},   {"c8:be:19", "D-Link", 0},
    {"00:0c:42", "MikroTik", 0}, {"08:55:31", "MikroTik", 0}, {"18:fd:74", "MikroTik", 0},
    {"48:8f:5a", "MikroTik", 0}, {"64:d1:54", "MikroTik", 0}, {"74:4d:28", "MikroTik", 0},
    {"cc:2d:e0", "MikroTik", 0}, {"dc:2c:6e", "MikroTik", 0},
    {"04:18:d6", "Ubiquiti", 0}, {"24:a4:3c", "Ubiquiti", 0}, {"44:d9:e7", "Ubiquiti", 0},
    {"68:d7:9a", "Ubiquiti", 0}, {"74:ac:b9", "Ubiquiti", 0}, {"78:8a:20", "Ubiquiti", 0},
    {"80:2a:a8", "Ubiquiti", 0}, {"b4:fb:e4", "Ubiquiti", 0}, {"f0:9f:c2", "Ubiquiti", 0},
    {"00:0b:86", "Aruba", 0},    {"6c:f3:7f", "Aruba", 0},    {"d8:c7:c8", "Aruba", 0},
    {"00:00:0c", "Cisco", 0},    {"00:1b:0c", "Cisco", 0},    {"00:25:9c", "Cisco", 0},
    {"2c:3f:38", "Cisco", 0},    {"58:6d:8f", "Cisco", 0},
    {"00:23:89", "H3C", 0},      {"70:ba:ef", "H3C", 0},      {"c8:3a:35", "Tenda", 0},
    {"00:13:49", "ZyXEL", 0},    {"5c:f4:ab", "ZyXEL", 0},    {"00:08:dc", "WIZnet", 0},
    /* ---- 消费电子 / 电脑 ---- */
    {"44:65:0d", "Amazon", 0},   {"68:37:e9", "Amazon", 0},   {"74:c2:46", "Amazon", 0},
    {"84:d6:d0", "Amazon", 0},   {"a0:02:dc", "Amazon", 0},   {"0c:47:c9", "Amazon", 0},
    {"ac:63:be", "Amazon", 0},   {"f0:27:2d", "Amazon", 0},
    {"3c:5a:b4", "Google", 0},   {"54:60:09", "Google", 0},   {"94:eb:2c", "Google", 0},
    {"f4:f5:d8", "Google", 0},   {"f4:f5:e8", "Google", 0},   {"1c:f2:9a", "Google", 0},
    {"6c:ad:f8", "Google", 0},   {"9c:4f:da", "Google", 0},   {"a4:77:33", "Google", 0},
    {"d8:6c:63", "Google", 0},   {"e4:f0:42", "Google", 0},   {"20:df:b9", "Google", 0},
    {"00:12:fb", "三星", 0},     {"00:15:99", "三星", 0},     {"08:37:3d", "三星", 0},
    {"18:3a:2d", "三星", 0},     {"28:39:5e", "三星", 0},     {"38:aa:3c", "三星", 0},
    {"5c:0a:5b", "三星", 0},     {"78:1f:db", "三星", 0},     {"8c:77:12", "三星", 0},
    {"c8:19:f7", "三星", 0},     {"f0:25:b7", "三星", 0},     {"f4:0e:22", "三星", 0},
    {"00:1b:21", "Intel", 0},    {"3c:97:0e", "Intel", 0},    {"48:51:b7", "Intel", 0},
    {"5c:e0:c5", "Intel", 0},    {"7c:7a:91", "Intel", 0},    {"94:65:9c", "Intel", 0},
    {"a4:c4:94", "Intel", 0},    {"d0:57:7b", "Intel", 0},    {"e4:a4:71", "Intel", 0},
    {"f8:16:54", "Intel", 0},
    {"00:e0:4c", "Realtek", 0},  {"00:10:18", "Broadcom", 0}, {"00:1b:e9", "Broadcom", 0},
    {"00:14:22", "Dell", 0},     {"18:66:da", "Dell", 0},     {"b8:2a:72", "Dell", 0},
    {"00:1b:78", "HP", 0},       {"3c:d9:2b", "HP", 0},       {"9c:b6:54", "HP", 0},
    {"b0:5a:da", "HP", 0},       {"d4:85:64", "HP", 0},       {"f4:ce:46", "HP", 0},
    {"00:00:85", "Canon", 0},    {"2c:9e:fc", "Canon", 0},    {"88:87:17", "Canon", 0},
    {"00:26:ab", "Epson", 0},    {"38:1a:52", "Epson", 0},    {"44:d2:44", "Epson", 0},
    {"a4:ee:57", "Epson", 0},    {"00:80:77", "Brother", 0},  {"30:05:5c", "Brother", 0},
    {"00:11:32", "Synology", 0}, {"90:09:d0", "Synology", 0}, {"00:08:9b", "QNAP", 0},
    {"24:5e:be", "QNAP", 0},
    {"00:0e:58", "Sonos", 0},    {"48:a6:b8", "Sonos", 0},    {"5c:aa:fd", "Sonos", 0},
    {"78:28:ca", "Sonos", 0},    {"94:9f:3e", "Sonos", 0},    {"b8:e9:37", "Sonos", 0},
    {"ac:3a:7a", "Roku", 0},     {"b0:a7:37", "Roku", 0},     {"cc:6d:a0", "Roku", 0},
    {"d8:31:34", "Roku", 0},
    {"00:13:a9", "索尼", 0},     {"30:f9:ed", "索尼", 0},     {"ac:9b:0a", "索尼", 0},
    {"00:1c:62", "LG", 0},       {"10:f1:f2", "LG", 0},       {"2c:54:cf", "LG", 0},
    {"a8:23:fe", "LG", 0},       {"c8:08:e9", "LG", 0},
    {"00:12:5a", "微软", 0},     {"28:18:78", "微软", 0},     {"7c:1e:52", "微软", 0},
    {"00:09:bf", "任天堂", 0},   {"58:bd:a3", "任天堂", 0},   {"98:b6:e9", "任天堂", 0},
};

const char *pgOuiLookup(const char *mac) {
  if (!mac || strlen(mac) < 8) return "";
  char key[9];
  for (int i = 0; i < 8; ++i) {
    char c = mac[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    key[i] = c;
  }
  key[8] = 0;
  for (size_t i = 0; i < sizeof(kOui) / sizeof(kOui[0]); ++i)
    if (strncmp(key, kOui[i].oui, 8) == 0) return kOui[i].name;
  return "";
}

bool pgOuiIsCam(const char *mac) {
  if (!mac || strlen(mac) < 8) return false;
  char key[9];
  for (int i = 0; i < 8; ++i) {
    char c = mac[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    key[i] = c;
  }
  key[8] = 0;
  for (size_t i = 0; i < sizeof(kOui) / sizeof(kOui[0]); ++i)
    if (kOui[i].cam && strncmp(key, kOui[i].oui, 8) == 0) return true;
  return false;
}

bool pgMacIsRandom(const char *mac) {
  if (!mac || strlen(mac) < 2) return false;
  char c0 = mac[0], c1 = mac[1];
  if (c0 >= 'A' && c0 <= 'F') c0 = (char)(c0 - 'A' + 'a');
  if (c1 >= 'A' && c1 <= 'F') c1 = (char)(c1 - 'A' + 'a');
  int hi = (c0 <= '9') ? c0 - '0' : c0 - 'a' + 10;
  int lo = (c1 <= '9') ? c1 - '0' : c1 - 'a' + 10;
  return ((hi << 4 | lo) & 0x02) != 0;   // bit1 = 本地管理位（随机 MAC）
}

/* ==========================================================================
 * 工具
 * ========================================================================== */
/** 是不是网关（x.y.z.1）—— 网关开着 80/UPnP 是**完全正常**的，
 *  不能因为"厂商未知 + 有端口"就给它挂个"关注"，那样每台设备都带噪点。 */
static bool isGatewayIp(const char *ip) {
  if (!ip || !ip[0]) return false;
  const char *dot = strrchr(ip, '.');
  return dot && strcmp(dot + 1, "1") == 0;
}

static bool hasPort(const char *ports, int p) {
  if (!ports || !ports[0]) return false;
  char needle[8];
  snprintf(needle, sizeof(needle), "%d", p);
  const char *q = ports;
  size_t nl = strlen(needle);
  while ((q = strstr(q, needle)) != 0) {
    /* 整词匹配：前后不能是数字（防 "80" 命中 "8000"） */
    bool lb = (q == ports) || (q[-1] == ' ');
    bool rb = (q[nl] == 0) || (q[nl] == ' ');
    if (lb && rb) return true;
    ++q;
  }
  return false;
}

/* 端口探测顺序：按"是摄像头的可能性"排，前面的先探、先出结果 */
static const int kPorts[kLanMaxPort] = {554, 80, 8000, 8080, 34567, 37777, 8554, 8001, 5000, 88, 22, 9100};

/* 摄像头页专用的端口子集（`Lan::startScan(..., camOnly=true)`）——理由见 PgLan.h。
 * ⚠️ 张数必须 ≤ kLanMaxPort（probePorts 里的 PConn/done 数组按它开）。 */
static const int kCamPorts[] = {554, 8554, 8000, 34567, 37777, 5000};
static const int kCamPortN = (int)(sizeof(kCamPorts) / sizeof(kCamPorts[0]));

/* ==========================================================================
 * Lan 实现
 * ========================================================================== */
class LanImpl {
 public:
  LanHost hosts[kLanMaxHost];
  int n = 0;
  volatile int phase_ = 0;
  volatile int progress_ = 0;
  volatile bool run = false;
  volatile bool stopReq = false;
  volatile int gen = 0;
  bool warnedFull = false;     // 表满警告只打一次（见 readArp）
  pthread_t th = 0;
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

  /* ★★ 显示顺序（风险降序 → 同风险按 IP 升序）—— 2026-09-15 用户实测反馈：
   *   "扫出来很多数据，但不知道谁是摄像头"。
   *   原来 hosts[] 是 **IP 升序**（读 ARP 时插入排序），而 IP 序与风险**完全无关**
   *   ⇒ 摄像头埋在十几台手机/路由器中间，只能逐行看右侧 11px 的小字。
   *   现在让风险高的永远排最前面：配合"只看风险"筛选 + 行首色条，一眼定位。
   *
   *   ⚠️ 只做**视图映射**，绝不重排 hosts[] 本身：
   *      findIdx(reqIp) / startProbeOne() 都按"原下标"定位设备，
   *      重排会让"点某一行深探"探到**别的设备**（而且不报错，最阴的那种 bug）。
   */
  int order[kLanMaxHost];
  int orderGen = -2;            // 已建序对应的 gen（-2 = 从未建过）

  /** a 是否应排在 b 前面：风险高的在前；同风险按 IP 升序（稳定、可预期）。 */
  bool riskLess(int a, int b) {
    if (hosts[a].level != hosts[b].level) return hosts[a].level > hosts[b].level;
    return ipBin(hosts[a].ip) < ipBin(hosts[b].ip);
  }

  void rebuildOrderLocked() {
    if (orderGen == (int)gen) return;
    for (int i = 0; i < n; ++i) order[i] = i;
    /* 插入排序：n ≤ 64，且相邻两次扫描间"基本有序"（只多/少几台），
     * O(n²) 的常数极小，比 setjmp 型快排更简单可控。 */
    for (int i = 1; i < n; ++i) {
      int k = order[i];
      int j = i - 1;
      while (j >= 0 && riskLess(k, order[j])) {
        order[j + 1] = order[j];
        --j;
      }
      order[j + 1] = k;
    }
    orderGen = (int)gen;
  }

  int reqMode = 0;      // 1 = 全量扫描 / 2 = 只补端口 / 3 = 只探一台
  bool reqFp = true;
  bool reqSsdp = true;
  bool reqCamOnly = false;   // true = 只探摄像头相关端口（摄像头页用，见 PgLan.h）
  char reqIp[16] = {0};

  char localIp[20] = {0};
  char localMask[20] = {0};
  char subnet[24] = {0};
  int  localIpBin = 0;

  /* ---------- 网络信息 ---------- */
  bool readNetInfo() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    struct ifreq ifr;
    const char *names[3] = {"wlan0", "eth0", "wlan1"};
    bool ok = false;
    for (int k = 0; k < 3 && !ok; ++k) {
      memset(&ifr, 0, sizeof(ifr));
      snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", names[k]);
      struct sockaddr_in *sin;
      if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        sin = (struct sockaddr_in *)&ifr.ifr_addr;
        snprintf(localIp, sizeof(localIp), "%s", inet_ntoa(sin->sin_addr));
        localIpBin = ntohl(sin->sin_addr.s_addr);
        memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", names[k]);
        if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0) {
          sin = (struct sockaddr_in *)&ifr.ifr_netmask;
          snprintf(localMask, sizeof(localMask), "%s", inet_ntoa(sin->sin_addr));
        } else {
          snprintf(localMask, sizeof(localMask), "255.255.255.0");
        }
        ok = true;
      }
    }
    close(fd);
    if (!ok || strcmp(localIp, "0.0.0.0") == 0) {
      localIp[0] = localMask[0] = 0;
      return false;
    }
    /* 子网展示串：一律按 /24 展示（我们扫的也是 /24 —— 见 sweep()） */
    unsigned a = (unsigned)localIpBin;
    snprintf(subnet, sizeof(subnet), "%u.%u.%u.0/24", (a >> 24) & 0xFF, (a >> 16) & 0xFF,
             (a >> 8) & 0xFF);
    return true;
  }

  /* ---------- ① 邻居发现 ---------- */
  /* 往 /24 里每个地址发一个 UDP 包：内核为了发出去必须做 ARP 解析
   * ⇒ 对方回 ARP 应答后，/proc/net/arp 里就有它的 IP+MAC。
   * 为什么不用 ping：ARP 连"所有端口都关着"的设备也能发现（比 ICMP 更灵），
   * 而且不需要考虑 ICMP 权限/限速。 */
  int sweep() {
    unsigned base = (unsigned)localIpBin & 0xFFFFFF00u;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_port = htons(9);   // discard
    const char one = 1;
    int sent = 0;
    for (int pass = 0; pass < 2 && !stopReq; ++pass) {
      for (int i = 1; i < 255; ++i) {
        if (stopReq) break;
        unsigned ip = base | (unsigned)i;
        if ((int)ip == localIpBin) continue;   // 自己
        d.sin_addr.s_addr = htonl(ip);
        if (sendto(s, &one, 1, 0, (struct sockaddr *)&d, sizeof(d)) > 0) ++sent;
        if ((i % 48) == 0) usleep(40000);      // 别把 ARP 队列灌爆（unres_qlen 默认 3）
      }
      usleep(pass == 0 ? 700000 : 900000);
    }
    close(s);
    return sent;
  }

  /** 对**单个**地址做一次"踢 ARP"：发一个 UDP 包逼内核去解析它的 MAC，
   *  然后（调用方）读 /proc/net/arp 就能拿到它。
   *  为什么需要：`lanprobe <ip>` 是"只探一台"，如果本进程还没做过整段扫描，
   *  表里就是空的 ⇒ findIdx 返回 -1 ⇒ **一声不响什么都不做**
   *  （实测踩到：日志只有"开始扫描 mode=3"和"本机 …"，没有 host 行，
   *   看着像卡死，其实是"没找到这台"）。 */
  bool arpPokeOne(const char *ip) {
    struct in_addr ia;
    if (inet_aton(ip, &ia) != 1) return false;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in d;
    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_port = htons(9);
    d.sin_addr = ia;
    const char one = 1;
    for (int i = 0; i < 2; ++i) {          // 发两次：第一次可能撞上 ARP 队列满
      sendto(s, &one, 1, 0, (struct sockaddr *)&d, sizeof(d));
      usleep(120000);
    }
    close(s);
    usleep(250000);                        // 等 ARP 应答落进邻居表
    return true;
  }

  int readArp() {
    unsigned base = (unsigned)localIpBin & 0xFFFFFF00u;
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return 0;
    char line[256];
    int got = 0;
    if (!fgets(line, sizeof(line), f)) { /* 表头 */
    }
    pthread_mutex_lock(&mu);
    while (fgets(line, sizeof(line), f)) {
      char ip[24] = {0}, mac[24] = {0};
      unsigned flags = 0;
      /* IPaddress HWtype Flags HWaddress Mask Device */
      if (sscanf(line, "%23s %*s %x %23s", ip, &flags, mac) != 3) continue;
      if (flags != 0x2) continue;                       // 0x2 = 完整（有应答）
      if (strcmp(mac, "00:00:00:00:00:00") == 0) continue;
      struct in_addr ia;
      if (inet_aton(ip, &ia) != 1) continue;
      unsigned bip = ntohl(ia.s_addr);
      if ((bip & 0xFFFFFF00u) != base) continue;        // 只收本子网
      if ((int)bip == localIpBin) continue;             // 排除自己
      int idx = findIdx(ip);
      if (idx < 0) {
        if (n >= kLanMaxHost) {
          /* ★ 表满**必须喊一声**（2026-09-17）：原来是 `continue` 静默丢弃，
           *   现场症状就是"局域网里明明有摄像头，扫描列表里没有它"，
           *   而日志只有一句"共 40 台设备"，看不出还有多少台被丢了。
           *   每轮只警告一次，避免 100 台设备刷屏。 */
          if (!warnedFull) {
            warnedFull = true;
            LOGW("PgLan: 主机表已满（上限 %d，本网段邻居更多）—— 多出来的设备**不参与扫描**；"
                 "要找的机器若没出现，先把 kLanMaxHost 调大", kLanMaxHost);
          }
          continue;
        }
        idx = n++;
        memset(&hosts[idx], 0, sizeof(hosts[idx]));
        snprintf(hosts[idx].ip, sizeof(hosts[idx].ip), "%s", ip);
        snprintf(hosts[idx].mac, sizeof(hosts[idx].mac), "%s", mac);
        snprintf(hosts[idx].vendor, sizeof(hosts[idx].vendor), "%s", pgOuiLookup(mac));
        snprintf(hosts[idx].kind, sizeof(hosts[idx].kind), "在线设备");
        hosts[idx].level = 0;
        ++got;
      }
    }
    /* 按 IP 升序排（列表看着整齐，也方便 QA 比对） */
    for (int i = 0; i < n; ++i)
      for (int j = i + 1; j < n; ++j)
        if (ipBin(hosts[j].ip) < ipBin(hosts[i].ip)) {
          LanHost t = hosts[i];
          hosts[i] = hosts[j];
          hosts[j] = t;
        }
    pthread_mutex_unlock(&mu);
    fclose(f);
    return got;
  }

  static unsigned ipBin(const char *ip) {
    struct in_addr ia;
    if (inet_aton(ip, &ia) != 1) return 0;
    return ntohl(ia.s_addr);
  }

  int findIdx(const char *ip) {
    for (int i = 0; i < n; ++i)
      if (strcmp(hosts[i].ip, ip) == 0) return i;
    return -1;
  }

  /* ---------- ② SSDP M-SEARCH ---------- */
  void ssdp() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct sockaddr_in src, dst;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    src.sin_port = 0;
    if (bind(s, (struct sockaddr *)&src, sizeof(src)) < 0) {
      close(s);
      return;
    }
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);

    static const char *kSt[] = {"ssdp:all", "urn:schemas-upnp-org:device:Basic:1",
                                "upnp:rootdevice"};
    for (int i = 0; i < 3; ++i) {
      char req[256];
      int nn = snprintf(req, sizeof(req),
                        "M-SEARCH * HTTP/1.1\r\n"
                        "HOST: 239.255.255.250:1900\r\n"
                        "MAN: \"ssdp:discover\"\r\n"
                        "MX: 1\r\n"
                        "ST: %s\r\n"
                        "USER-AGENT: PocketGame/1.0\r\n\r\n",
                        kSt[i]);
      sendto(s, req, (size_t)nn, 0, (struct sockaddr *)&dst, sizeof(dst));
      usleep(120000);
    }

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 300000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[1400];
    int deadline = 26;   // 约 2.6s
    while (deadline-- > 0 && !stopReq) {
      struct sockaddr_in from;
      socklen_t fl = sizeof(from);
      int r = (int)recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
      if (r <= 0) continue;
      buf[r] = 0;
      char ip[24];
      snprintf(ip, sizeof(ip), "%s", inet_ntoa(from.sin_addr));
      if ((int)ntohl(from.sin_addr.s_addr) == localIpBin) continue;   // 我们自己（PgDlna）
      processSsdp(ip, buf);
    }
    close(s);
  }

  void processSsdp(const char *ip, const char *resp) {
    char server[64] = {0}, loc[200] = {0};
    const char *p = resp;
    while (p && *p) {
      if (strncasecmp(p, "SERVER:", 7) == 0) {
        const char *v = p + 7;
        while (*v == ' ') ++v;
        int i = 0;
        while (v[i] && v[i] != '\r' && v[i] != '\n' && i < (int)sizeof(server) - 1) {
          server[i] = v[i];
          ++i;
        }
        server[i] = 0;
      } else if (strncasecmp(p, "LOCATION:", 9) == 0) {
        const char *v = p + 9;
        while (*v == ' ') ++v;
        int i = 0;
        while (v[i] && v[i] != '\r' && v[i] != '\n' && i < (int)sizeof(loc) - 1) {
          loc[i] = v[i];
          ++i;
        }
        loc[i] = 0;
      }
      const char *nl = strchr(p, '\n');
      p = nl ? nl + 1 : 0;
    }
    if (!server[0] && !loc[0]) return;

    pthread_mutex_lock(&mu);
    int idx = findIdx(ip);
    if (idx < 0 && n < kLanMaxHost) {
      idx = n++;
      memset(&hosts[idx], 0, sizeof(hosts[idx]));
      snprintf(hosts[idx].ip, sizeof(hosts[idx].ip), "%s", ip);
      snprintf(hosts[idx].kind, sizeof(hosts[idx].kind), "在线设备");
    }
    bool stored = false;
    if (idx >= 0 && !hosts[idx].ssdp[0]) {
      snprintf(hosts[idx].ssdp, sizeof(hosts[idx].ssdp), "%s", server[0] ? server : loc);
      stored = true;
    }
    pthread_mutex_unlock(&mu);
    /* ⚠️ 只在这一台**第一次**报型号时打日志：同一台设备会对我们的三条 M-SEARCH
     *    各回好几条（同一 SERVER 重复十来行），不去重会把 logcat 刷爆
     *    —— 本板日志缓冲只有十几行，刷爆就看不清别的了（踩过）。 */
    if (stored) LOGD("PgLan: ssdp %s server='%s' loc='%s'", ip, server, loc);
  }

  /* ---------- ③ 端口 + 协议指纹 ---------- */
  struct PConn { int fd; int port; };

  int probePorts(const char *ip, int *openPorts, int maxp) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &a.sin_addr);

    PConn cs[kLanMaxPort];
    const int *pl = reqCamOnly ? kCamPorts : kPorts;
    const int pn = reqCamOnly ? kCamPortN : kLanMaxPort;
    int nc = 0;
    for (int i = 0; i < pn; ++i) {
      if (stopReq) break;
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) continue;
      int fl = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, fl | O_NONBLOCK);
      struct sockaddr_in b = a;
      b.sin_port = htons((unsigned short)pl[i]);
      int r = connect(fd, (struct sockaddr *)&b, sizeof(b));
      if (r == 0 || errno == EINPROGRESS || errno == EALREADY) {
        cs[nc].fd = fd;
        cs[nc].port = pl[i];
        ++nc;
      } else {
        close(fd);
      }
    }
    if (!nc) return 0;

    /* ★★ 必须**循环** select 直到全部有结果或超时（2026-09-15 真机踩到）：
     *    select 是"**有一个**就绪就返回" —— 写一次就收工的话，先返回的那个 fd
     *    把控制权抢回来，**其余端口即使后来也连上了也不会被检查**。
     *    症状极隐蔽：PC 侧日志明明记着"80 和 554 都连进来了"，
     *    设备却只报 554 —— 因为 554 先就绪，一次 select 之后就退出了。
     *    （端口扫描这种事"少报一个口"就是漏掉一台摄像头，不能含糊。） */
    /* ★ 单台预算：局域网 RTT 是亚毫秒级，350ms 还没连上的端口就是"关着/被丢"。
     *   摄像头模式（只探 6 个 IPC 口）压到 350ms —— 88~96 台就是 30~67s 与 15~34s 的差别。
     *   全量模式保留 700ms：要照顾 9100/22 这类可能慢一点的通用服务。 */
    const long deadline = nowMs() + (reqCamOnly ? 350 : 700);
    bool done[kLanMaxPort];
    for (int i = 0; i < nc; ++i) done[i] = false;
    int pending = nc;
    int nopen = 0;
    while (pending > 0) {
      fd_set ws;
      FD_ZERO(&ws);
      int mx = -1;
      for (int i = 0; i < nc; ++i) {
        if (done[i]) continue;
        FD_SET(cs[i].fd, &ws);
        if (cs[i].fd > mx) mx = cs[i].fd;
      }
      if (mx < 0) break;
      long left = deadline - nowMs();
      if (left <= 0) break;                // 超时：剩下的当"没开"
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = (left > 200 ? 200 : left) * 1000;   // 每次最多等 200ms，好收尾
      int nsel = select(mx + 1, 0, &ws, 0, &tv);
      if (nsel <= 0) continue;
      for (int i = 0; i < nc; ++i) {
        if (done[i] || !FD_ISSET(cs[i].fd, &ws)) continue;
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(cs[i].fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
          if (nopen < maxp) openPorts[nopen++] = cs[i].port;
        }
        done[i] = true;
        --pending;
      }
    }
    for (int i = 0; i < nc; ++i) close(cs[i].fd);
    return nopen;
  }

  /* HTTP HEAD：拿 Server 头（IPC 的 Web 栈特征极明显） */
  void httpHead(const char *ip, int port, char *out, int n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
      close(fd);
      return;
    }
    char req[160];
    int rn = snprintf(req, sizeof(req),
                      "HEAD / HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: PocketGame\r\n\r\n", ip,
                      port);
    if (send(fd, req, (size_t)rn, 0) > 0) {
      char buf[600];
      int r = (int)recv(fd, buf, sizeof(buf) - 1, 0);
      if (r > 0) {
        buf[r] = 0;
        /* 抓 Server: 头 */
        const char *p = strcasestr(buf, "\nserver:");
        if (p) {
          p += 8;
          while (*p == ' ') ++p;
          int i = 0;
          while (p[i] && p[i] != '\r' && p[i] != '\n' && i < n - 1) {
            out[i] = p[i];
            ++i;
          }
          out[i] = 0;
        } else if (strncmp(buf, "HTTP/", 5) == 0) {
          snprintf(out, (size_t)n, "HTTP");
        }
      }
    }
    close(fd);
  }

  /* RTSP DESCRIBE：**这是"坐实摄像头"最硬的一招**
   *   - 回 200 + body 有 m=video  ⇒ 这是一路**未鉴权、可直接读取的视频流**（最严重）
   *   - 回 401/403               ⇒ 确认是 RTSP 视频设备（只是要密码）
   *   - 回别的/无响应             ⇒ 端口开着但不是标准 RTSP（可能是别的服务） */
  void rtspDescribe(const char *ip, int port, char *out, int n) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
      close(fd);
      return;
    }
    char req[220];
    int rn = snprintf(req, sizeof(req),
                      "DESCRIBE rtsp://%s:%d/ RTSP/1.0\r\nCSeq: 1\r\n"
                      "Accept: application/sdp\r\nUser-Agent: PocketGame\r\n\r\n",
                      ip, port);
    if (send(fd, req, (size_t)rn, 0) > 0) {
      char buf[1000];
      int total = 0;
      /* RTSP 响应可能分片；读几拍拼起来（有超时，不会挂死） */
      for (int k = 0; k < 4 && total < (int)sizeof(buf) - 1; ++k) {
        int r = (int)recv(fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0);
        if (r <= 0) break;
        total += r;
        if (strstr(buf, "m=video")) break;
      }
      if (total > 0) {
        buf[total] = 0;
        int code = 0;
        if (strncmp(buf, "RTSP/1.0 ", 9) == 0) code = atoi(buf + 9);
        if (code == 200) {
          if (strstr(buf, "m=video")) {
            const char *c = strstr(buf, "a=rtpmap:");
            if (c) {
              const char *sp = strchr(c, ' ');
              char codec[20] = "视频流";
              if (sp) {
                int i = 0;
                ++sp;
                while (sp[i] && sp[i] != '/' && i < 12) {
                  codec[i] = sp[i];
                  ++i;
                }
                codec[i] = 0;
              }
              snprintf(out, (size_t)n, "RTSP 视频流可直读(%s)", codec);
            } else {
              snprintf(out, (size_t)n, "RTSP 视频流可直读");
            }
          } else {
            snprintf(out, (size_t)n, "RTSP 200 音视频设备");
          }
        } else if (code == 401) {
          snprintf(out, (size_t)n, "RTSP 401 需鉴权");
        } else if (code == 403) {
          snprintf(out, (size_t)n, "RTSP 403 拒绝");
        } else if (code) {
          snprintf(out, (size_t)n, "RTSP %d", code);
        } else {
          snprintf(out, (size_t)n, "RTSP 有响应");
        }
      }
    }
    close(fd);
  }

  void fingerprint(int idx) {
    LanHost &h = hosts[idx];
    int openPorts[kLanMaxPort];
    int nopen = probePorts(h.ip, openPorts, kLanMaxPort);
    /* 端口拼成升序串（探的是"是否开放"，展示顺序无所谓，排一下更好看） */
    for (int i = 0; i < nopen; ++i)
      for (int j = i + 1; j < nopen; ++j)
        if (openPorts[j] < openPorts[i]) {
          int t = openPorts[i];
          openPorts[i] = openPorts[j];
          openPorts[j] = t;
        }
    char ports[48] = {0};
    for (int i = 0; i < nopen; ++i) {
      char t[10];
      snprintf(t, sizeof(t), "%s%d", i ? " " : "", openPorts[i]);
      strncat(ports, t, sizeof(ports) - strlen(ports) - 1);
    }
    char note[68] = {0};
    if (nopen) {
      if (hasPort(ports, 554)) {
        char b[44] = {0};
        rtspDescribe(h.ip, 554, b, sizeof(b));
        strncat(note, b, sizeof(note) - strlen(note) - 1);
      } else if (hasPort(ports, 8554)) {
        char b[44] = {0};
        rtspDescribe(h.ip, 8554, b, sizeof(b));
        strncat(note, b, sizeof(note) - strlen(note) - 1);
      }
      int hp = hasPort(ports, 80) ? 80 : (hasPort(ports, 8080) ? 8080
                                          : (hasPort(ports, 88) ? 88 : 0));
      if (hp) {
        char b[40] = {0};
        httpHead(h.ip, hp, b, sizeof(b));
        if (b[0]) {
          if (note[0]) strncat(note, " · ", sizeof(note) - strlen(note) - 1);
          char t[48];
          snprintf(t, sizeof(t), "%s:%s", hp == 88 ? "88" : (hp == 80 ? "80" : "8080"), b);
          strncat(note, t, sizeof(note) - strlen(note) - 1);
        }
      }
    }
    pthread_mutex_lock(&mu);
    snprintf(h.ports, sizeof(h.ports), "%s", ports);
    snprintf(h.note, sizeof(h.note), "%s", note);
    if (!h.vendor[0]) snprintf(h.vendor, sizeof(h.vendor), "%s", pgOuiLookup(h.mac));
    h.level = Lan::classify(h, h.why, sizeof(h.why));
    /* 设备类型标签 */
    if (h.level >= 3) snprintf(h.kind, sizeof(h.kind), "摄像头");
    else if (h.level == 2) snprintf(h.kind, sizeof(h.kind), "疑似摄像头");
    else if (strstr(h.note, "RTSP")) snprintf(h.kind, sizeof(h.kind), "音视频设备");
    else if (pgOuiIsCam(h.mac)) snprintf(h.kind, sizeof(h.kind), "安防设备");
    else if (hasPort(h.ports, 9100)) snprintf(h.kind, sizeof(h.kind), "打印机");
    else if (pgMacIsRandom(h.mac)) snprintf(h.kind, sizeof(h.kind), "手机/平板");
    else if (hasPort(h.ports, 22) || hasPort(h.ports, 445)) snprintf(h.kind, sizeof(h.kind), "电脑/NAS");
    else if (h.ip[0] && strcmp(strrchr(h.ip, '.'), ".1") == 0)
      snprintf(h.kind, sizeof(h.kind), "路由器/网关");
    else if (hasPort(h.ports, 80) || hasPort(h.ports, 443)) snprintf(h.kind, sizeof(h.kind), "网络设备");
    else snprintf(h.kind, sizeof(h.kind), "在线设备");
    pthread_mutex_unlock(&mu);
  }

  /** 并行分片：[begin, end) 逐台端口/指纹。
   *  ⚠️ 写成 **static 成员**（显式接 this）而不是普通成员：给它传参的 tramp 是被
   *     pthread 直接调用的，而文件级的 `gLan` 是在**类定义之后**才出现的 —— 类内联函数
   *     里引用它会编译不过（踩过）。传指针最省事也最不容易再犯。 */
  static void probeRange(LanImpl *L, int begin, int end) {
    for (int i = begin; i < end; ++i) {
      if (L->stopReq) return;
      L->fingerprint(i);
      /* ★ camOnly 不逐台打行：89 台 × 一行会把本板十几行的日志缓冲刷爆，
       *   结果是"扫描期间别的日志全看不见"。只打**真开了端口**的那几台（那才是要看的）。 */
      if (L->hosts[i].ports[0]) L->logHost(i);
    }
  }
  static void *fpTramp(void *p) {
    long long *a = (long long *)p;
    LanImpl *L = (LanImpl *)a[0];
    int b = (int)a[1], e = (int)a[2];
    free(a);
    probeRange(L, b, e);
    return 0;
  }

  void logHost(int i) {
    pthread_mutex_lock(&mu);
    LOGD("PgLan: host ip=%-15s mac=%s vendor='%s' kind='%s' ports='%s' level=%d why='%s' "
         "note='%s' ssdp='%s'",
         hosts[i].ip, hosts[i].mac[0] ? hosts[i].mac : "-",
         hosts[i].vendor[0] ? hosts[i].vendor : "-", hosts[i].kind, hosts[i].ports,
         hosts[i].level, hosts[i].why, hosts[i].note, hosts[i].ssdp);
    pthread_mutex_unlock(&mu);
  }

  /* ---------- worker ---------- */
  void worker() {
    LOGD("PgLan: ---- 开始扫描 mode=%d fp=%d ssdp=%d camOnly=%d ----", reqMode,
         reqFp ? 1 : 0, reqSsdp ? 1 : 0, reqCamOnly ? 1 : 0);
    /* ⚠️ 线程生命周期：worker 结束时**只清 run**，th 句柄留给下一次 start* 去
     *    pthread_join（join 一个已返回的线程是立刻返回的，不会卡 UI）。
     *    反过来"worker 自己把 thAlive 清了"会导致**永远没人 join** ⇒ 每次扫描
     *    泄漏一个线程栈（这块板子内存只有 56MB，不能随便漏）。 */
    if (!readNetInfo()) {
      LOGD("PgLan: 没有拿到本机 IP（WiFi 没连上？）");
      phase_ = 4;
      progress_ = 100;
      run = false;
      return;
    }
    LOGD("PgLan: 本机 %s mask %s subnet %s", localIp, localMask, subnet);

    if (reqMode == 3) {          // 只探一台
      pthread_mutex_lock(&mu);
      int idx = findIdx(reqIp);
      pthread_mutex_unlock(&mu);
      if (idx < 0) {
        /* 表里还没有它 ⇒ 先给它单独"踢"一次 ARP，再读表（见 arpPokeOne 的注释） */
        LOGD("PgLan: 表里没有 %s -> 单独踢一次 ARP", reqIp);
        arpPokeOne(reqIp);
        readArp();
        pthread_mutex_lock(&mu);
        idx = findIdx(reqIp);
        pthread_mutex_unlock(&mu);
      }
      if (idx >= 0) {
        phase_ = 3;
        progress_ = 50;
        fingerprint(idx);
        logHost(idx);
      } else {
        LOGD("PgLan: %s 没有 ARP 应答（不在线 / 不在本子网）", reqIp);
      }
      gen++;
      phase_ = 4;
      progress_ = 100;
      run = false;
      return;
    }

    if (reqMode == 1) {
      phase_ = 1;
      progress_ = 5;
      pthread_mutex_lock(&mu);
      n = 0;                       // 新一轮：清表
      warnedFull = false;          // 表满警告每轮重新计
      pthread_mutex_unlock(&mu);
      sweep();
      int a = readArp();
      gen++;
      progress_ = 25;
      LOGD("PgLan: 邻居发现完成，第一轮 %d 台", a);
      if (!stopReq && reqSsdp) {
        phase_ = 2;
        progress_ = 30;
        ssdp();
        int b = readArp();
        gen++;
        progress_ = 45;
        LOGD("PgLan: SSDP 完成（新增 %d 台）", b);
      }
    }

    if (reqFp && !stopReq) {
      phase_ = 3;
      int total;
      pthread_mutex_lock(&mu);
      /* ★ 2026-09-17：原来这里写死 `n > 24 ? 24 : n` —— 与 kLanMaxHost 一样是**静默截断**。
       *   邻居发现拿到 40 台、只有前 24 台做端口/指纹，后 16 台在列表里**一个端口都没有**
       *   （看着像"不在线"），摄像头页据此过滤 554/8554 就把它们全滤掉了。
       *   实测现场：海康 IPC 就在 .163，正因排在第 24 名之后而**扫不到**。 */
      total = n;
      if (total > kLanMaxFinger) {
        LOGW("PgLan: 本轮只对前 %d 台做端口/指纹（共 %d 台）—— 后面的设备端口信息会是空的",
             kLanMaxFinger, total);
        total = kLanMaxFinger;
      }
      pthread_mutex_unlock(&mu);
      LOGD("PgLan: 开始端口/指纹探测（%d 台%s）", total, reqCamOnly ? "，camOnly 并行" : "");
      if (reqCamOnly && total > 8) {
        /* ★★ 摄像头页专用：把 89~96 台的端口探测**分给 4 条线程并行**。
         *   为什么敢并行：每台只写自己的 `hosts[i]`（互不重叠），probePorts/rtspDescribe/
         *   httpHead 全是局部状态；这段里没人读 /proc/net/arp、没人改 n。
         *   不并行的代价实测：89 台 × 平均 163ms = **14.5s**，而串行等的全是"防火墙丢包"的机器。
         *   ⚠️ **只在 camOnly 下并行** —— 探针页要的是"可读的逐台审计轨迹"，
         *     行为一个字不改（那边继续串行 + 每台一行日志）。 */
        pthread_t th[4];
        int nn = 0;
        for (int k = 0; k < 4; ++k) {
          int b = total * k / 4, e = total * (k + 1) / 4;
          if (b >= e) continue;
          long long *arg = (long long *)malloc(sizeof(long long) * 3);
          arg[0] = (long long)(size_t)this;
          arg[1] = b;
          arg[2] = e;
          if (pthread_create(&th[nn], 0, &LanImpl::fpTramp, arg) == 0) ++nn;
          else free(arg);
        }
        for (int k = 0; k < nn; ++k) pthread_join(th[k], 0);
        gen++;
        progress_ = 95;
      } else {
        for (int i = 0; i < total && !stopReq; ++i) {
          fingerprint(i);
          logHost(i);
          gen++;
          progress_ = 45 + (total ? (i + 1) * 50 / total : 50);
        }
      }
    }
    if (!reqFp) {
      pthread_mutex_lock(&mu);
      for (int i = 0; i < n; ++i) {
        hosts[i].level = Lan::classify(hosts[i], hosts[i].why, sizeof(hosts[i].why));
        if (pgMacIsRandom(hosts[i].mac)) snprintf(hosts[i].kind, sizeof(hosts[i].kind), "手机/平板");
        else if (pgOuiIsCam(hosts[i].mac)) snprintf(hosts[i].kind, sizeof(hosts[i].kind), "安防设备");
      }
      pthread_mutex_unlock(&mu);
    }
    phase_ = 4;
    progress_ = 100;
    pthread_mutex_lock(&mu);
    LOGD("PgLan: ---- 扫描结束，共 %d 台设备 ----", n);
    pthread_mutex_unlock(&mu);
    gen++;
    run = false;
  }

  static void *tramp(void *p) {
    ((LanImpl *)p)->worker();
    return 0;
  }
};

static LanImpl gLan;
static Lan gLanApi;

/** 回收上一轮线程（已返回的线程 join 是立即返回的；run 为真时不会被调用）。 */
static void lanJoinPrev() {
  if (gLan.th) {
    pthread_join(gLan.th, 0);
    gLan.th = 0;
  }
}

/* ==========================================================================
 * Lan 门面
 * ========================================================================== */
Lan *Lan::instance() { return &gLanApi; }

void Lan::startScan(bool fingerprint, bool ssdp, bool camOnly) {
  if (gLan.run) {
    LOGD("PgLan: 已在扫描中，忽略新的扫描请求");
    return;
  }
  lanJoinPrev();
  gLan.reqMode = 1;
  gLan.reqFp = fingerprint;
  gLan.reqSsdp = ssdp;
  gLan.reqCamOnly = camOnly;
  gLan.stopReq = false;
  gLan.phase_ = 1;
  gLan.progress_ = 0;
  gLan.run = true;
  if (pthread_create(&gLan.th, 0, LanImpl::tramp, &gLan) != 0) {
    gLan.th = 0;
    gLan.run = false;
    LOGD("PgLan: 线程创建失败");
  }
}

void Lan::startProbeAll() {
  if (gLan.run) return;
  pthread_mutex_lock(&gLan.mu);
  int n = gLan.n;
  pthread_mutex_unlock(&gLan.mu);
  if (!n) {
    LOGD("PgLan: 还没有设备，先扫一遍");
    startScan(true, true);
    return;
  }
  lanJoinPrev();
  gLan.reqMode = 2;
  gLan.reqFp = true;
  gLan.reqSsdp = false;
  /* ★ 深探**必须显式写回全量端口**：这是个**粘性请求字段**，如果本页之前跑过
   *   camOnly（只探 6 个 IPC 口），不写这一行"深探"就会悄悄只探 6 个口
   *   —— 按钮叫"深探端口指纹"却探得比常规还少，属于静默失效。 */
  gLan.reqCamOnly = false;
  gLan.stopReq = false;
  gLan.phase_ = 3;
  gLan.progress_ = 0;
  gLan.run = true;
  if (pthread_create(&gLan.th, 0, LanImpl::tramp, &gLan) != 0) {
    gLan.th = 0;
    gLan.run = false;
  }
}

void Lan::startProbeOne(const char *ip) {
  if (!ip || !ip[0]) return;
  if (gLan.run) {
    LOGD("PgLan: 扫描中，单台探测请求排队忽略（%s）", ip);
    return;
  }
  lanJoinPrev();
  gLan.reqMode = 3;
  gLan.reqFp = true;
  gLan.reqSsdp = false;
  gLan.reqCamOnly = false;    // 单台探测用全量端口（同上：粘性字段要显式写）
  gLan.stopReq = false;
  snprintf(gLan.reqIp, sizeof(gLan.reqIp), "%s", ip);
  gLan.phase_ = 3;
  gLan.progress_ = 0;
  gLan.run = true;
  if (pthread_create(&gLan.th, 0, LanImpl::tramp, &gLan) != 0) {
    gLan.th = 0;
    gLan.run = false;
  }
}

void Lan::stop() {
  if (!gLan.run) return;
  gLan.stopReq = true;
  LOGD("PgLan: 收到停止请求（会在当前这一台探完后退出）");
}

bool Lan::running() const { return gLan.run; }
int Lan::phase() const { return gLan.phase_; }
int Lan::progress() const { return gLan.progress_; }
int Lan::generation() const { return gLan.gen; }

int Lan::hostCount() const {
  int v;
  pthread_mutex_lock((pthread_mutex_t *)&gLan.mu);
  v = gLan.n;
  pthread_mutex_unlock((pthread_mutex_t *)&gLan.mu);
  return v;
}

bool Lan::hostCopy(int i, LanHost *out) const {
  if (!out) return false;
  pthread_mutex_lock((pthread_mutex_t *)&gLan.mu);
  /* ★ 这里返回的是**按风险排序后的第 i 台**（不是 hosts[i]）——
   *   列表显示、QA dump、点击定位全都走这个函数，所以三者天然一致。
   *   点击回调拿到的也是"排序视图下标"，它接着调 startProbeOne(h.ip)，
   *   用 IP（不是下标）定位，所以排序不会导致探错设备。 */
  gLan.rebuildOrderLocked();
  bool ok = (i >= 0 && i < gLan.n);
  if (ok) {
    int k = gLan.order[i];
    if (k < 0 || k >= gLan.n) k = i;      // 兜底：order 越界就退回原始下标
    *out = gLan.hosts[k];
  }
  pthread_mutex_unlock((pthread_mutex_t *)&gLan.mu);
  return ok;
}

bool Lan::netInfo(char *ip, int n, char *mask, int m) const {
  if (!gLan.localIp[0]) return false;
  if (ip && n > 0) snprintf(ip, (size_t)n, "%s", gLan.localIp);
  if (mask && m > 0) snprintf(mask, (size_t)m, "%s", gLan.localMask);
  return true;
}

const char *Lan::subnetText() const { return gLan.subnet; }
const char *Lan::ifName() const { return "wlan0"; }

/* ==========================================================================
 * 风险判定（纯函数 —— QA 自检也走它）
 *
 * 分级原则：**只看证据强度**，不猜。
 *   3 高危：确认是视频/安防设备（RTSP 应答 / 安防私有端口 / 未鉴权视频流）
 *   2 可疑：像摄像头但没坐实（IPC 常见 Web 栈 / 安防厂商 OUI / SSDP 报 IPCAM）
 *   1 关注：有点意思（未知厂商 + 开着奇怪端口）
 * ========================================================================== */
int Lan::classify(const LanHost &h, char *why, int wn) {
  if (why && wn > 0) why[0] = 0;
  const char *n = h.note;
  const char *s = h.ssdp;

  /* ---- 3 高危 ---- */
  if (strstr(n, "视频流可直读")) {
    snprintf(why, (size_t)wn, "视频流可直读");
    return 3;
  }
  if (strstr(n, "RTSP")) {   // 401 / 200 / 任何 RTSP 应答 ⇒ 确认是 RTSP 视频设备
    snprintf(why, (size_t)wn, "RTSP 视频设备");
    return 3;
  }
  if (hasPort(h.ports, 34567)) {
    snprintf(why, (size_t)wn, "雄迈摄像头端口");
    return 3;
  }
  if (hasPort(h.ports, 37777)) {
    snprintf(why, (size_t)wn, "大华私有端口");
    return 3;
  }
  if (hasPort(h.ports, 8000)) {
    snprintf(why, (size_t)wn, "海康 SDK 端口");
    return 3;
  }

  /* ---- 2 可疑 ---- */
  /* ⚠️ 这里的关键词会命中 HTTP `Server:` 头，太泛的词（webs/video）会把普通
   *    网络设备误判成"可疑" —— 只用**安防/DVR 专有**的栈名。 */
  static const char *kIpcStack[] = {"goahead", "boa", "hipcam", "v380", "ipcam",
                                    "netsurveillance", "uc-httpd", "app-webs", "h264dvr",
                                    "xmeye", "onvif", "dvr", "nvr", "cam"};
  char lowNote[80], lowSsdp[64], lowVendor[24];
  int i = 0;
  for (; n[i] && i < 79; ++i) lowNote[i] = (char)tolower((unsigned char)n[i]);
  lowNote[i] = 0;
  for (i = 0; s[i] && i < 63; ++i) lowSsdp[i] = (char)tolower((unsigned char)s[i]);
  lowSsdp[i] = 0;
  for (i = 0; h.vendor[i] && i < 23; ++i) lowVendor[i] = (char)tolower((unsigned char)h.vendor[i]);
  lowVendor[i] = 0;

  for (size_t k = 0; k < sizeof(kIpcStack) / sizeof(kIpcStack[0]); ++k) {
    if (strstr(lowNote, kIpcStack[k])) {
      snprintf(why, (size_t)wn, "IPC 特征(%s)", kIpcStack[k]);
      return 2;
    }
  }
  if (strstr(lowSsdp, "ipcam") || strstr(lowSsdp, "camera") || strstr(lowSsdp, "dvr") ||
      strstr(lowSsdp, "hikvision") || strstr(lowSsdp, "dahua")) {
    snprintf(why, (size_t)wn, "SSDP 报安防设备");
    return 2;
  }
  if (pgOuiIsCam(h.mac)) {
    snprintf(why, (size_t)wn, "安防厂商设备");
    return 2;
  }
  if (hasPort(h.ports, 8554)) {
    snprintf(why, (size_t)wn, "RTSP 备用端口");
    return 2;
  }
  /* 只有 1 个端口开着、却是"摄像头常用端口"里的非标准一个 —— 给关注 */
  if (hasPort(h.ports, 8001)) {
    snprintf(why, (size_t)wn, "IPC 备用端口");
    return 2;
  }

  /* ---- 1 关注 ---- */
  /* 只给"非网关 + 未知厂商 + 确实暴露了点东西"的挂关注：
   * 网关/路由器本来就会开 80 和管理口、也多半会答 UPnP —— 那不是异常。 */
  if (!h.vendor[0] && h.mac[0] && !isGatewayIp(h.ip) && (h.ports[0] || h.ssdp[0])) {
    snprintf(why, (size_t)wn, "未知厂商");
    return 1;
  }
  return 0;
}

}  // namespace pg
