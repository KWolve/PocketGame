#pragma once
/*
 * PgLan.h - 局域网设备扫描（找"STA 型"偷拍设备 / IP 摄像头的正解）
 *
 * 为什么需要它（见 PocketGame/docs/wifi-probe-app.md §9）：
 *   被动 AP 扫描只能看见"自己发热点"的设备（softAP 型）。而**主流的偷拍设备是 STA 型**
 *   —— 它连进房间/酒店的 WiFi 往云端推流，不发 beacon、不广播任何东西，AP 列表里根本没有它。
 *   要发现它只能：**加入同一张网 → 主动探测**。本模块就是这套主动探测的落地：
 *
 *     ① 邻居发现   : 往整个子网发 UDP（内核为每个目的地址做 ARP 解析）→ 读 /proc/net/arp
 *                    ⇒ 拿到"在线主机的 IP + MAC"。比 ping 扫段更快、也不需要 ICMP 权限。
 *     ② SSDP 顺听  : 发 M-SEARCH（ssdp:all）→ 收 UPnP 应答里的 SERVER 头
 *                    ⇒ 很多 IP 摄像头/路由器会把型号写在里面（"IPCAM/1.0"、"Hipcam …"）。
 *     ③ 端口 + 指纹 : TCP connect 扫一组"摄像头端口"，对开着的口做**协议指纹**：
 *                    - 554/8554 发 RTSP DESCRIBE → 回 200/401 且带 m=video
 *                      = **确认这是一路视频流**（这是"坐实摄像头"最硬的证据）
 *                    - 80/8080/… 发 HTTP HEAD → Server: 头 + realm=
 *                      （GoAhead / Boa / Hipcam / V380 / NETSurveillance 都是 IPC 常用栈）
 *     ④ 判定       : 端口 + 协议指纹 + SSDP SERVER + MAC 厂商 OUI + IP 角色（网关/随机 MAC）
 *                    合成 0~3 级风险与一句"为什么"。
 *
 * ⚠️ 线程模型（与工程里所有"长动作"一致）：
 *   扫描跑在**自己的线程**里，只写自己的数据结构（加锁），**绝不碰任何控件**；
 *   UI 侧在 onUI_Timer 里读（先比 generation，变了才刷列表）。
 *   每完成一步都 LOGD 一行（`PgLan: host …`），这是免触摸验收的判据。
 *
 * ⚠️ 代价（必须让用户知道）：
 *   ① 必须**连进目标网络**才有意义；
 *   ② 端口探测是"主动"的 —— 会在被扫设备的日志里留下连接记录；
 *   ③ 换 SSID 会顶掉 zknet 里保存的网络（zknet 只保留一个 network 条目）。
 */
namespace pg {

const int kLanMaxHost = 128;  // 设备表上限（防极端网络把内存撑爆）
                              // ★ 2026-09-17 从 **40 提到 128**：实测本网段 /proc/net/arp 里
                              //   有 **100 条**邻居，40 的上限会**静默丢掉 60 台**；而 ARP 表
                              //   是**哈希序**（不是 IP 序），被丢的恰好可能是你要找的摄像头
                              //   —— 现场就是"海康 IPC 在 .163、设备扫不到"（见 docs/camera-radio-app.md §6.1）。
                              //   单条 LanHost 约 296B，128 条 ≈ 37KB BSS，本板 55MB 可承受。
const int kLanMaxPort = 12;   // 单台设备并发的端口探测数
const int kLanMaxFinger = 96; // 最多对多少台做端口+指纹（★ 原为写死的 24，同样会静默丢设备）

struct LanHost {
  char ip[16];
  char mac[18];     // "aa:bb:cc:dd:ee:ff"；空 = 没拿到
  char vendor[20];  // MAC 厂商（OUI 精选表；未命中为 ""）
  char kind[22];    // 设备类型标签（摄像头 / 路由器/网关 / 手机/平板 / 打印机 …）
                    // ⚠️ 别给小了：中文一个字 3 字节，"路由器/网关" 就要 19 字节
                    //    —— 14 字节的缓冲会把它**静默截断**成"路由器/网"（踩过）。
  char ports[48];   // 开放端口，升序，空格分隔
  char note[68];    // 协议指纹摘要（RTSP 结果 / Server 头）
  char ssdp[56];    // SSDP 应答里的 SERVER 头
  int level;        // 0 普通 / 1 关注 / 2 可疑 / 3 高危
  char why[44];     // 判定原因（列表右侧直接显示）
};

class Lan {
 public:
  static Lan *instance();

  /** 开始一轮扫描。
   * @param fingerprint true = 找到主机后做端口 + 协议指纹（慢、但能坐实摄像头）
   * @param ssdp        true = 顺带发 SSDP M-SEARCH 收设备型号
   * @param camOnly     true = **只探摄像头相关的那 6 个端口**（554/8554/8000/34567/37777/5000），
   *                    并把单台预算从 700ms 压到 350ms。
   *                    ★ 摄像头页专用：全量 12 个口里 80/8080/9100/22 是"几乎每台机器都有"的
   *                      通用口，对它们做 HTTP 指纹是单台耗时的大头 —— 而这一页**只看 554/8554**。
   *                      实测 88 台：全量模式端口指纹阶段 **32.5s**；camOnly 后见 §6.7。
   *                    探针页（要看全端口/风险分级）**不要**用它。
   * 扫描中重复调用会忽略（running() 为 true 时）。 */
  void startScan(bool fingerprint, bool ssdp, bool camOnly = false);

  /** 只对**已发现**的主机重做端口 + 指纹（快速扫描之后补一刀）。 */
  void startProbeAll();

  /** 只探某一台（列表点行时用）。 */
  void startProbeOne(const char *ip);

  void stop();
  bool running() const;

  /** 0 空闲 / 1 邻居发现 / 2 SSDP / 3 端口指纹 / 4 完成 */
  int phase() const;
  int progress() const;     // 0..100（跨阶段的总进度，仅用于显示）

  int hostCount() const;
  /** 取第 i 台设备（**拷贝**给调用方，不是返回内部指针 —— 列表在 worker 线程里
   *  随时可能被改写，UI 拿指针会读到半更新的结构）。越界返回 false。 */
  bool hostCopy(int i, LanHost *out) const;

  /** 每完成一个阶段 / 表有更新就 +1 —— UI 用它判断"要不要刷列表"。 */
  int generation() const;

  /** 本机网络信息（走 ioctl，和 zknet 无关，不受它影响）。 */
  bool netInfo(char *ip, int n, char *mask, int m) const;
  const char *subnetText() const;      // "192.168.0.0/24"
  const char *ifName() const;          // "wlan0"

  /** 纯函数，供 QA 自检（不用真摄像头也能验证判定逻辑）。 */
  static int classify(const LanHost &h, char *why, int wn);
};

/** MAC → 厂商（OUI 精选表）。mac 大小写不敏感；未命中返回 ""。 */
const char *pgOuiLookup(const char *mac);
/** 该 MAC 是否属于"安防/摄像头系"厂商（嗅探页也要用它判风险，所以导出来）。 */
bool pgOuiIsCam(const char *mac);
/** 随机 MAC 判定（第一字节 bit1 = 本地管理位）。 */
bool pgMacIsRandom(const char *mac);

}  // namespace pg
