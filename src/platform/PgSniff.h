#pragma once
/*
 * PgSniff.h - 无线嗅探（monitor mode 顺听空口）
 *
 * 为什么能做（实测，见 PocketGame/docs/wifi-probe-app.md §9）：
 *   本板 WiFi 驱动（8733bs / Realtek rtw 系）**自己声明支持 MONITOR 接口类型**
 *   （`GET_WIPHY` 的 SUPPORTED_IFTYPES = ADHOC STATION AP MONITOR P2P_CLIENT P2P_GO），
 *   而驱动 **vif 上限 = 2**（新建第 3 个口返回 -22 EINVAL）⇒ 正解是**把闲置的 wlan1
 *   改成 MONITOR**（`SET_INTERFACE`，实测返回 0），用完切回 STATION。
 *
 *   为什么 wlan1 是闲置的：`/etc/init.rc` 里有 `service p2p_supplicant … -iwlan1`，
 *   但 `/res/bin` 里**根本没有 p2p_supplicant 这个二进制** ⇒ 该服务永远起不来
 *   ⇒ 本板的 P2P/Miracast 本来就是坏的，wlan1 没有任何进程在用。
 *   ⇒ **不需要"顶掉 zknet"、不需要碰 wlan0 / wpa_supplicant**。
 *
 * 抓到什么（这才是它比手机 App 强的地方）：
 *   ① 同信道的**所有发射者**（AP 和 STA 都算）—— STA 平时完全隐身，只有空口看得见；
 *   ② 每帧 radiotap 里的**真实 RSSI / 信道** ⇒ 可以像"热点猎手"一样定位**任何**设备；
 *   ③ **probe request 里的 SSID** —— 设备在主动找哪个网络（iOS 永远拿不到）；
 *   ④ **assoc request 里的 SSID** —— 客户端连隐藏 AP 时，SSID 是**明文**的
 *      ⇒ **这就是"隐藏 SSID 拿不到名字"的正解**：顺听一次关联就够了。
 *
 * ★★ 全信道嗅探（跳信道）—— 2026-09-15 实测打通，见 docs/wifi-probe-app.md §13：
 *   单射频驱动下，**占用信道的是 wlan0 这个接口本身**，不是"关联状态"：
 *     ① wlan0 关联着      → `SET_CHANNEL` = -16 EBUSY
 *     ② wlan0 up 但没关联 → 仍然 -16 EBUSY   ← 只发 DISCONNECT 是不够的！
 *     ③ **wlan0 down**    → `SET_CHANNEL` = 0，12 个信道全部切换成功
 *   ⇒ 想扫别的信道必须**把 wlan0 关掉**（本模块不负责关；由 probeLogic 配合
 *     「回家快照」在进/出全信道模式时 down/up + 重连）。
 *   ⇒ 代价：嗅探期间设备**没有网络**（局域网扫描页也用不了）。
 *
 * 限制（实测，别指望更多）：
 *   - 看不到加密内容，只有 MAC / 帧类型 / RSSI / 明文的管理帧。
 *   - 现代手机用随机 MAC（本地管理位 = 1）⇒ OUI 查不到厂商。
 *   - 跳信道有"驻留期"：beacon 间隔 100ms，驻留 450ms ⇒ 每个 AP 至少 4 个 beacon；
 *     但**换信道那一瞬**正在传的数据帧会漏（对"统计有什么设备"无影响）。
 *
 * ⚠️ 线程模型：抓包跑在自己的线程（阻塞 recvfrom + 250ms 超时，靠 stop 标志退出），
 *    只写自己的表（加锁）+ LOGD，**绝不碰控件**。UI 在 onUI_Timer 里读。
 * ⚠️ 退出必须还原：stop() 会把 wlan1 切回 STATION。App 退出（onUI_quit）务必调用。
 */
namespace pg {

const int kSniffMaxPeer = 28;
const int kSniffMaxSsid = 24;

struct SniffPeer {
  char mac[18];
  char vendor[16];
  char role[6];      // "AP" / "STA" / "?"
  int  rssi;         // radiotap 里的 dBm（0 = 这帧没带）
  int  nMgmt;
  int  nData;
  int  nCtrl;
  char ssid[34];     // 最近跟它有关的 SSID（probe/assoc/beacon）
  char ssidKind[8];  // "beacon" / "probe" / "assoc" / ""
  int  flags;        // bit0 隐藏 AP / bit1 名字是从 assoc 里解出来的 / bit2 随机 MAC
  int  lastSec;      // 距最后一次看到它多少秒
  int  chan;         // 最后见到它时所在信道（跳信道模式下才有意义）
};

enum { SNIFF_F_HIDDEN_AP = 1, SNIFF_F_RESOLVED = 2, SNIFF_F_RANDOM_MAC = 4 };

class Sniff {
 public:
  static Sniff *instance();

  /** 把 wlan1 切成 MONITOR 并开始抓包。失败时 stateText() 会说明原因。 */
  bool start();
  /** 停抓包并把 wlan1 切回 STATION（**必须调**，否则把接口留在监听态）。 */
  void stop();

  bool active() const;
  const char *stateText() const;   // 一行状态（未启动 / 监听中 / 失败原因）
  const char *ifName() const;

  int  channel() const;            // radiotap 里读到的信道（0 = 还不知道）
  int  elapsedSec() const;
  long frames() const;
  long dataFrames() const;

  int  peerCount() const;
  bool peerCopy(int i, SniffPeer *out) const;

  int  ssidCount() const;
  const char *ssidAt(int i) const;
  /** 其中"从 assoc request 里捡到的"数量 —— 即**解开名字的隐藏网络**数。 */
  int  hiddenResolved() const;

  /* ---- 全信道跳扫（★ 调用方必须先把 wlan0 关掉，否则切不动）---- */

  /** 开/关跳信道。开之前必须先 start()；返回 false = 没在抓包。 */
  bool setHop(bool on);
  bool hopActive() const;
  int  hopCurChan() const;          // 当前停在哪个信道
  int  hopScanned() const;          // 累计切了多少次（含多轮）
  int  hopRounds() const;           // 完整扫完几轮
  int  hopChanTotal() const;        // 信道表长度
  int  hopChanNumAt(int i) const;   // 第 i 个信道号
  long hopChanFrames(int i) const;  // 第 i 个信道抓到的帧数
  long hopFail() const;             // 切信道失败次数（>0 = 射频没释放干净）

  /** 抓包线程里偶尔会调它打印新增设备（免触摸验收的判据）。 */
  void logDump();

  int  generation() const;   // 表有更新就 +1
};

}  // namespace pg
