/*
 * PgTime.h - 网络校时（NTP）
 *
 * 为什么必须要它：
 *   本板**没有 RTC**（掉电不保持时间），`zkgui` 起来时系统时钟恒为 `1970-01-01`。
 *   而 TLS 证书校验的第一条就是"当前时间是否落在证书的 notBefore~notAfter 之间" ——
 *   1970 年时**任何证书都"尚未生效"**，https 握手必然失败（实测：
 *   `SSL_connect` 报 `certificate is not yet valid`）。
 *   所以 **https 的前提是先把系统时间拉正**，这就是本模块的职责。
 *
 * 用 ntp 组件包（`Manifest.xml` 里 `<package id="ntp" version="^2.1.0"/>`）。
 * ⚠️ 那个包**只认 IP、不做 DNS 解析**（库的未定义符号里只有 settimeofday，
 *    没有 gethostbyname/getaddrinfo），所以服务器列表必须写 IP。
 *    `ntp::defaultServerList()` 里就是厂商内置的国内公共时间服务器（清华 tuna、
 *    阿里、腾讯等），直接用最稳。要自定义也**只能给 IP**。
 *
 * 用法：主循环里不断调 ensureStarted()（幂等），WiFi 通了就自己同步。
 */
#ifndef PG_TIME_H_
#define PG_TIME_H_

namespace pg {

class TimeSync {
 public:
  // 幂等：未同步且已过重试间隔时发起一次异步同步（不阻塞调用线程）
  static void ensureStarted();

  // 幂等：把进程时区设成北京时间。POSIX 的 TZ 是「本地 = UTC + (-偏置)」，
  // 所以东八区写作 `UTC-8`（ntp 包 README 的写法）。TZ 只影响 localtime 的显示，
  // 不影响 TLS 校验（那用 UTC）。
  // ⚠️ **setenv 只作用于当前进程**，`zkgui` 一重启就回到 UTC —— 所以必须**开机就设**，
  //    不能只在"真要校时那一刻"设（否则没校时过 / 重启后，屏保等界面显示的是 UTC 时间）。
  static void ensureTimezone();

  // 0 = 还没发起过；1 = 同步中；2 = 已同步成功
  static int state();
  static const char *stateText();

  static bool synced();      // 本进程内是否已同步成功
  static bool timeValid();   // 系统时间是否"看起来有效"（晚于 2020-01-01）
  static long long nowSec();  // 当前 unix 时间（秒）

  // QA 用：立刻同步一次（逗号分隔的 IP 列表；传 0/空 = 用内置列表）。
  // 这是**同步阻塞**调用，只给自检通道用，别在 UI 线程随手调。
  static bool syncNow(const char *ipCsv);

  // QA 用：把状态清回"未同步"，便于重复验证
  static void reset();

  // 上次成功同步用的服务器（没同步过返回 "-"）
  static const char *lastServer();
};

}  // namespace pg

#endif  // PG_TIME_H_
