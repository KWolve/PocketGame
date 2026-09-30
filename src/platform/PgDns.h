/*
 * PgDns.h - 本地 DNS 中继（根治"本板没有 /etc/resolv.conf"）
 *
 * 问题（2026-09-13 实测）：
 *   本板根分区是**只读 squashfs**，**没有 `/etc/resolv.conf`**，也建不出来
 *   （`echo > /etc/resolv.conf` → Read-only file system）。
 *   而 libc（musl）在 conf 不存在时**默认只查 `127.0.0.1:53`** ⇒ 域名一律解析失败
 *   （`getaddrinfo` 返回 h_errno=2 / TRY_AGAIN）。
 *   直接后果：**任何走域名的功能都不能用**（IPTV 频道地址、在线流的域名源、
 *   DLNA 按名字找设备…）。之前能用域名的地方都是"碰巧用 IP"。
 *
 * 解法：**在本进程里起一个极简 DNS 中继**，绑 `127.0.0.1:53`，
 *   收到查询就原样转发给上游公共 DNS（默认阿里 223.5.5.5，可 QA 换），
 *   把响应原样回给请求者。对上层完全透明 —— `getaddrinfo`/ffmpeg/OpenSSL 都照常工作，
 *   **https 的 SNI 与证书校验也不受影响**（不像"把域名换成 IP"那种 hack）。
 *
 * 为什么不是别的做法：
 *   - 改 `/etc/resolv.conf`：根分区只读，改不了；
 *   - 自己解析 + 换成 IP 直连：https 会因 SNI/证书不匹配失败；http 可以但只解决一半；
 *   - 起 `dnsmasq`：设备上没有这个二进制，也不该为一个功能塞个守护进程。
 *
 * ⚠️ 绑 53 端口需要权限（本进程以 root 跑）。绑不上时打 ERROR 并**不影响其它功能**。
 * ⚠️ 只做 UDP 直通转发（不解析报文、没有缓存）—— 够用，且没有解析 bug 风险。
 *   要缓存/DNS over TCP 的话再加（目前实测够快：阿里 DNS 往返 ~12ms）。
 */
#ifndef PG_DNS_H_
#define PG_DNS_H_

namespace pg {

class DnsRelay {
 public:
  static DnsRelay &instance();

  // 幂等：绑 127.0.0.1:53 + 起中继线程。返回 false = 绑不上（端口占用/无权限）
  bool start();
  void stop();

  bool running() const { return running_; }
  int queries() const { return queries_; }      // 已转发查询数（自检）
  int answered() const { return answered_; }    // 成功拿到响应数（自检）
  bool bound() const { return bound_; }
  const char *upstream() const { return upstream_; }
  const char *lastError() const { return err_; }
  bool setUpstream(const char *ip);             // QA 换上游（形如 "223.5.5.5"）

 private:
  DnsRelay() {}
  ~DnsRelay() {}
  DnsRelay(const DnsRelay &);
  DnsRelay &operator=(const DnsRelay &);

  static void *threadEntry(void *self);
  void *threadLoop();

  volatile bool running_ = false;
  bool bound_ = false;
  volatile int queries_ = 0;
  volatile int answered_ = 0;
  int listenFd_ = -1;
  char upstream_[32] = "223.5.5.5";
  char err_[96] = {0};
};

}  // namespace pg

#endif  // PG_DNS_H_
