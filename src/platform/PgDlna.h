#pragma once
/*
 * PgDlna.h - DLNA/UPnP MediaRenderer (DMR) 最小实现
 *
 * 目标：手机上的"投屏"能把本机当成一台 DLNA 渲染设备，把视频/音乐推过来播放。
 *
 * 结构：
 *   SSDP 应答线程  : UDP 1900 收 M-SEARCH，回 description 的 LOCATION（并做 ssdp:alive 通告）
 *   HTTP 服务线程  : civetweb 提供
 *                      /description.xml            设备描述
 *                      /AVTransport/scpd.xml 等     三个服务的 SCPD（部分控制器会去拉）
 *                      /upnp/control/<Service>      SOAP 控制（AVTransport/RenderingControl/ConnectionManager）
 *                      /upnp/event/<Service>        SUBSCRIBE（返回 SID；NOTIFY 推送见说明）
 *   下载线程      : SetAVTransportURI 之后把 URL 拉到 /tmp 本地文件
 *
 * ⚠️ 线程模型：civetweb 与 SSDP 都在自己的线程里跑，**绝不能碰 UI**。
 *    它们只把动作塞进 action 队列，由 UI 主循环 pollAction() 取出来执行
 *    （框架是单线程 UI，跨线程调控件会崩）。
 *
 * ⚠️ MVP 限制：框架播放器 `ZKVideoView::play()` **只吃本地路径**（实测传 http:// 会
 *    报 "file[...] is not exist!"），所以这里是"**先下载完再播**"。大文件受
   *    /tmp（tmpfs）容量限制；要边下边播得用 av/ffmpeg + aw-mpp 自建管线（后续升级项）。
 */
#include <pthread.h>

// ⚠️ 这两个必须在**全局作用域**前置声明：写在 namespace pg 里面的话，
//    `struct mg_connection *` 会被 C++ 当成 pg::mg_connection 新建一个不完整类型，
//    结果 civetweb 的全局 mg_printf(mg_connection*,...) 全都匹配不上（踩过）。
struct mg_connection;

namespace pg {

// 供类内部调用的自由函数（也作为 friend；先在此声明，普通名字查找才找得到 ——
// 只写 friend 声明的话，类内调用会因为"friend 只能靠 ADL 找到"而报未声明）
void *pgDlnaSsdpThread(void *arg);
void *pgDlnaDownloadThread(void *arg);
int pgDlnaHttpHandler(struct mg_connection *conn, void *cbdata);
void pgDlnaTlsDiag();  // TLS 自检：dlopen libssl/libcrypto 是否可用（QA: dlna tlscheck）
// https 证书校验策略：0 不校验 / 1（默认）有时间+有 CA 就校验、失败回退 / 2 必须校验通过
int pgDlnaTlsVerifyMode();          // QA: dlna verify（不带参数=查询）
void pgDlnaSetTlsVerifyMode(int m);  // QA: dlna verify <0|1|2>
void pgDlnaDnsCheck(const char *host);
void pgDlnaSetHttpUa(const char *ua);        // QA: dlna ua <字符串>
void pgDlnaSetHttpReferer(const char *r);    // QA: dlna ref <字符串>  // QA: dlna dns <域名>（本板无 /etc/resolv.conf，需能看解析结果）

struct DlnaAction {
  enum Type { NONE = 0, PLAY, PAUSE, RESUME, STOP, SEEK, SET_VOLUME, EXIT };
  int type;
  int arg;         // SEEK: 毫秒; SET_VOLUME: 0..100
  char path[2048];  // PLAY: 本地文件路径**或在线流 URL**
                    // ⚠️ 别给小了：CDN 链接带签名参数轻松 500+ 字符（实测 B 站那条 511 字符），
                    //    截断就会 403/959 或直接连不上。
  DlnaAction() : type(NONE), arg(0) { path[0] = 0; }
};

class Dlna {
 public:
  Dlna();
  ~Dlna();

  // 启动服务。friendlyName 建议全 ASCII（部分控制器对 UTF-8 的 friendlyName 处理不稳）。
  bool start(const char *friendlyName, int httpPort);
  void stop();
  bool running() const { return running_; }
  int httpPort() const { return httpPort_; }
  // ⚠️ 每次调用都重算（wlan0 的 DHCP 可能晚于 start()；算一次会定格成 127.0.0.1，
  //    控制器拿到的 LOCATION 就永远是环回地址 → 连不上。实测踩过。）
  const char *localIp() {
    refreshLocalIp();
    return localIp_;
  }
  // 重算本机四层地址（优先 wlan0，跳过环回/未 UP 的网卡）
  void refreshLocalIp();
  /* 请 SSDP 线程**补发一次 alive 通告**。
   * 为什么需要：start() 时 wlan0 可能还没拿到 IP（LOCATION 会写成 127.0.0.1，
   * 控制器拿了也连不上）。UI 侧发现 IP 变成真实地址时调这个补一次通告。 */
  void requestAlive() { aliveReq_ = 1; }
  // 删除已下载的媒体文件（本板 /tmp 是 tmpfs，文件就是内存；停播/播完就该删）
  void cleanupMedia();

  // ---------------- UI 线程侧 ----------------
  // 取一个待执行动作（没有就返回 false）。由主循环每帧调用。
  bool pollAction(DlnaAction *out);
  // UI 执行完动作后回报实际状态，控制器查询 GetTransportInfo 时用
  void setTransportState(const char *state);  // STOPPED/PLAYING/PAUSED_PLAYBACK/TRANSITIONING
  const char *transportState() const { return transportState_; }
  const char *lastUri() const { return lastUri_; }
  int downloadPercent() const { return downloadPercent_; }
  // UI 侧回报当前位置（GetPositionInfo 用）
  void setPositionMs(int ms) { positionMs_ = ms; }
  void setDurationMs(int ms) { durationMs_ = ms; }

  // ---------------- 自检/QA 入口（等价于控制器行为，免网络）----------------
  void injectUri(const char *uri);      // == SetAVTransportURI
  void injectPlayPause(bool play);      // == Play / Pause
  void injectStop();                    // == Stop
  void setVolumeInternal(int pct0to100);
  int volumePercent() const { return volume_; }

 private:
  bool running_;
  int httpPort_;
  char localIp_[32];
  char friendlyName_[64];
  char transportState_[32];
  char lastUri_[2048];  // 控制器给的 CDN 链接可能很长，别截断
  char mediaPath_[256];
  volatile int downloadPercent_;
  volatile int positionMs_;
  volatile int durationMs_;
  volatile int volume_;

  void *ctx_;          // mg_context*
  pthread_t ssdpTid_;
  pthread_t dlTid_;
  int ssdpJoinable_;  // SSDP 线程已创建且**未 detach**，stop() 要 join 它（防 use-after-free）
  volatile int stopFlag_;
  volatile int aliveReq_;   // UI 侧要求补发 alive 通告（见 requestAlive）
  volatile int dlBusy_;

  // 动作队列（SSDP/HTTP 线程 → UI 线程）
  pthread_mutex_t qMtx_;
  DlnaAction q_[16];
  int qHead_, qTail_;
  void pushAction(int type, const char *path, int arg);

  friend void *pgDlnaSsdpThread(void *arg);
  friend void *pgDlnaDownloadThread(void *arg);
  friend int pgDlnaHttpHandler(struct mg_connection *conn, void *cbdata);

 public:
  // 给内部线程用的实现（不要在 UI 线程直接调）
  void onSetUri(const char *uri);      // 起下载线程
  void onSoapCommand(int type, int arg);
  void httpRequest(struct mg_connection *conn);
  int downloadTo(const char *uri);     // 返回 0 成功
  void notifyHttpIp();
};

}  // namespace pg
