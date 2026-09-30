/*
 * PgTime.cpp - 网络校时（NTP）实现，见 PgTime.h 的说明。
 *
 * 三个实现要点：
 *  1. **异步**：用 ntp::startSyncTime（内部起线程），绝不阻塞 UI 线程
 *     （同步版 syncTime 会在 DNS/网络不通时卡满超时，UI 会在那几秒整个冻住）。
 *  2. **时区**：同步前 `setenv("TZ","UTC-8")` + `tzset()` —— POSIX 的 TZ 是
 *     "本地 = UTC + (-偏置)"，所以 `UTC-8` 才是北京时区（+8）。这是 ntp 包 README 的写法。
 *     TLS 校验本身用 UTC（不受 TZ 影响），TZ 只影响我们打日志/显示的时间。
 *  3. **重试节流**：失败后至少隔 10s 再试；成功就不再试。
 *     另外 `timeValid()` 一旦为真就直接置为已同步（比如以后加了 RTC/外部对时）。
 */
#include "platform/PgTime.h"

#ifdef FUN_BUILD
#include <stdio.h>
#include <stdlib.h>   // setenv
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "utils/Log.h"
#include "ntp/ntp.h"

namespace pg {

namespace {

volatile int sState = 0;            // 0 未开始 1 同步中 2 已同步
volatile int sInFlight = 0;         // 防重入（startSyncTime 是异步的，回调前别重复发起）
char sServer[64] = "-";
long long sLastTryMs = 0;

long long monoMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 时区只在这一处定义（原文散落在 kick/syncNow 两处，容易改漏）。
// POSIX TZ = "std offset"，本地时间 = UTC + (-offset)，所以东八区写 `UTC-8`。
const char *kTzName = "UTC-8";

// 同步完成回调（在 ntp 自己的线程里跑）
void onSyncEnd(const std::string &server, const timeval *tv) {
  sInFlight = 0;
  if (!tv || tv->tv_sec <= 0) {
    sState = 0;
    LOGW("PgTime: NTP 同步失败（服务器 %s 都没应答），稍后重试", server.c_str());
    return;
  }
  snprintf(sServer, sizeof(sServer), "%s", server.c_str());
  sState = 2;
  time_t t = (time_t)tv->tv_sec;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
  LOGD("PgTime: NTP 同步成功 server=%s 现在=%s（epoch %ld）", sServer, buf, (long)t);
}

// 发起一次异步同步（不阻塞）；返回是否真的发起了
bool kick() {
  if (sInFlight || sState == 2) return false;
  if (monoMs() - sLastTryMs < 10000) return false;  // 失败后节流 10s
  sLastTryMs = monoMs();
  TimeSync::ensureTimezone();
  std::vector<std::string> servers = ntp::defaultServerList();
  sInFlight = 1;
  int r = ntp::startSyncTime(servers, onSyncEnd);
  if (r != 0) {
    sInFlight = 0;
    LOGW("PgTime: ntp::startSyncTime 启动失败 (ret=%d)", r);
    return false;
  }
  LOGD("PgTime: 已发起 NTP 同步（内置服务器 %d 个，异步）", (int)servers.size());
  return true;
}

}  // namespace

void TimeSync::ensureTimezone() {
  static bool done = false;  // 进程内一次就够
  if (done) return;
  done = true;
  setenv("TZ", kTzName, 1);
  tzset();
  LOGD("PgTime: 时区 -> %s（= 北京时间 UTC+8；TZ 只影响显示，TLS 走 UTC）", kTzName);
}

void TimeSync::ensureStarted() {
  if (sState == 2) return;
  if (timeValid()) {  // 系统时间已经是有效值（有 RTC / 已被别处校过）
    if (sState != 2) {
      sState = 2;
      LOGD("PgTime: 系统时间已有效，跳过 NTP");
    }
    return;
  }
  kick();
}

int TimeSync::state() { return sState; }

const char *TimeSync::stateText() {
  switch (sState) {
    case 1: return "同步中";
    case 2: return "已同步";
    default: return "未开始";
  }
}

bool TimeSync::synced() { return sState == 2; }

bool TimeSync::timeValid() { return nowSec() > 1577836800LL; }  // 2020-01-01

long long TimeSync::nowSec() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec;
}

const char *TimeSync::lastServer() { return sServer; }

void TimeSync::reset() {
  sState = 0;
  sInFlight = 0;
  sLastTryMs = 0;
  snprintf(sServer, sizeof(sServer), "-");
}

bool TimeSync::syncNow(const char *ipCsv) {
  std::vector<std::string> servers;
  if (ipCsv && ipCsv[0]) {
    // 逗号/空白分隔的 IP 列表（ntp 包不做 DNS，这里也只接受 IP）
    const char *p = ipCsv;
    while (*p) {
      while (*p == ' ' || *p == ',' || *p == '\t') ++p;
      const char *b = p;
      while (*p && *p != ',' && *p != ' ' && *p != '\t') ++p;
      if (p > b) servers.push_back(std::string(b, p - b));
    }
  }
  if (servers.empty()) servers = ntp::defaultServerList();
  ensureTimezone();
  LOGD("PgTime: 同步校时（%d 个服务器，最多等 3s/个）...", (int)servers.size());
  bool ok = ntp::syncTime(servers, 3000);
  if (ok) {
    sState = 2;
    time_t t = (time_t)nowSec();
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    LOGD("PgTime: 手动同步成功，现在=%s", buf);
  } else {
    LOGW("PgTime: 手动同步失败（网络不通？服务器都没应答？）");
  }
  return ok;
}

}  // namespace pg

#endif  // FUN_BUILD
