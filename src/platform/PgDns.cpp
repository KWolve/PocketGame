/*
 * PgDns.cpp - 本地 DNS 中继。设计说明见 PgDns.h。
 */
#include "platform/PgDns.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "utils/Log.h"

namespace pg {

namespace {
const int kUpstreamTimeoutMs = 2500;   // 上游等待（超时就直接不回，让调用方自己重试）
const int kPollMs = 300;               // 监听超时（保证 stop() 能及时退出）
}  // namespace

DnsRelay &DnsRelay::instance() {
  static DnsRelay s;
  return s;
}

void *DnsRelay::threadEntry(void *self) {
  return ((DnsRelay *)self)->threadLoop();
}

bool DnsRelay::setUpstream(const char *ip) {
  if (!ip || !*ip) return false;
  struct in_addr a;
  if (inet_aton(ip, &a) == 0) return false;   // 只接受 IP（不做域名解析，避免自环）
  snprintf(upstream_, sizeof(upstream_), "%s", ip);
  LOGD("PgDns: 上游 DNS -> %s", upstream_);
  return true;
}

void *DnsRelay::threadLoop() {
  LOGD("PgDns: 中继线程启动（127.0.0.1:53 -> %s:53）", upstream_);
  while (running_) {
    struct pollfd pfd;
    pfd.fd = listenFd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, kPollMs);
    if (pr <= 0) continue;

    uint8_t q[1500];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int n = (int)recvfrom(listenFd_, q, sizeof(q), 0, (struct sockaddr *)&from, &flen);
    if (n <= 0) continue;
    ++queries_;

    /* 转发给上游（每个查询一个短命 socket：不共享 fd，避免串包） */
    int up = socket(AF_INET, SOCK_DGRAM, 0);
    if (up < 0) continue;
    struct timeval tv;
    tv.tv_sec = kUpstreamTimeoutMs / 1000;
    tv.tv_usec = (kUpstreamTimeoutMs % 1000) * 1000;
    setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in ua;
    memset(&ua, 0, sizeof(ua));
    ua.sin_family = AF_INET;
    ua.sin_port = htons(53);
    ua.sin_addr.s_addr = inet_addr(upstream_);

    if (sendto(up, q, (size_t)n, 0, (struct sockaddr *)&ua, sizeof(ua)) > 0) {
      uint8_t r[1500];
      int rn = (int)recv(up, r, sizeof(r), 0);
      if (rn > 0) {
        if (sendto(listenFd_, r, (size_t)rn, 0, (struct sockaddr *)&from, flen) > 0) {
          ++answered_;
        }
      } else if ((queries_ % 20) == 1) {
        /* 打一次就够：上游不通时说明网络本身有问题（不是我们的中继） */
        LOGW("PgDns: 上游 %s 无响应（已转发 %d 次，成功 %d 次）", upstream_, queries_,
             answered_);
      }
    }
    close(up);
  }
  LOGD("PgDns: 中继线程退出（共转发 %d 次，成功 %d 次）", queries_, answered_);
  return 0;
}

bool DnsRelay::start() {
  if (running_) return true;

  listenFd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (listenFd_ < 0) {
    snprintf(err_, sizeof(err_), "socket() 失败");
    LOGE("PgDns: %s", err_);
    return false;
  }
  /* 允许复用地址（进程重启后端口还在 TIME_WAIT 也能立刻绑上） */
  int on = 1;
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只监听本机（libc 正是查这里）

  if (bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    snprintf(err_, sizeof(err_), "bind 127.0.0.1:53 失败（端口被占或无权限）");
    LOGE("PgDns: %s", err_);
    close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  bound_ = true;
  running_ = true;

  pthread_t th;
  int pr = pthread_create(&th, 0, threadEntry, this);
  if (pr != 0) {
    snprintf(err_, sizeof(err_), "线程创建失败(%d)", pr);
    LOGE("PgDns: %s", err_);
    running_ = false;
    return false;
  }
  pthread_detach(th);
  err_[0] = 0;
  LOGD("PgDns: ★ 本地 DNS 中继已启动（本板没有 /etc/resolv.conf，musl 默认查 127.0.0.1:53）");
  return true;
}

void DnsRelay::stop() {
  if (!running_) return;
  running_ = false;
  /* 线程最多 kPollMs 后自己退出（poll 超时） */
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = (kPollMs + 200) * 1000000L;
  nanosleep(&ts, 0);
  if (listenFd_ >= 0) {
    close(listenFd_);
    listenFd_ = -1;
  }
  bound_ = false;
}

}  // namespace pg
