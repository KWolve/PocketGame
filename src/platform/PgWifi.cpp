#include "PgWifi.h"

#include <stdio.h>
#include <string.h>

#include "utils/Log.h"

#ifdef FUN_BUILD
#include "entry/EasyUIContext.h"
#include "net/NetManager.h"
#include "net/WifiInfo.h"
#include "net/WifiManager.h"
#endif

namespace pg {

namespace {

#ifdef FUN_BUILD
WifiManager *wm() {
  NetManager *nm = NETMANAGER;
  return nm ? nm->getWifiManager() : 0;
}
#endif

void copyStr(char *buf, int n, const char *s) {
  if (!buf || n <= 0) return;
  if (!s) {
    buf[0] = 0;
    return;
  }
  snprintf(buf, (size_t)n, "%s", s);
}

}  // namespace

bool WifiService::supported() {
#ifdef FUN_BUILD
  WifiManager *m = wm();
  return m && m->isSupported();
#else
  return false;
#endif
}

bool WifiService::enabled() {
#ifdef FUN_BUILD
  WifiManager *m = wm();
  return m && m->isWifiEnable();
#else
  return false;
#endif
}

bool WifiService::connected() {
#ifdef FUN_BUILD
  WifiManager *m = wm();
  return m && m->isConnected();
#else
  return false;
#endif
}

void WifiService::ssid(char *buf, int n) {
  copyStr(buf, n, 0);
#ifdef FUN_BUILD
  WifiManager *m = wm();
  if (!m) return;
  WifiInfo *info = m->getConnectionInfo();
  if (info) copyStr(buf, n, info->getSsid().c_str());
#endif
}

void WifiService::ip(char *buf, int n) {
  copyStr(buf, n, 0);
#ifdef FUN_BUILD
  WifiManager *m = wm();
  if (!m) return;
  const char *s = m->getIp();
  if (s && s[0]) copyStr(buf, n, s);
#endif
}

void WifiService::mac(char *buf, int n) {
  copyStr(buf, n, 0);
#ifdef FUN_BUILD
  WifiManager *m = wm();
  if (!m) return;
  const char *s = m->getMacAddr();
  if (s && s[0]) copyStr(buf, n, s);
#endif
}

int WifiService::rssi() {
#ifdef FUN_BUILD
  WifiManager *m = wm();
  if (!m) return 0;
  WifiInfo *info = m->getConnectionInfo();
  return info ? info->getRssi() : 0;
#else
  return 0;
#endif
}

void WifiService::setEnabled(bool on) {
#ifdef FUN_BUILD
  WifiManager *m = wm();
  if (!m) return;
  LOGD("PgWifi: setEnabled(%d)", on ? 1 : 0);
  m->enableWifi(on);
#else
  (void)on;
#endif
}

void WifiService::openSystemSettings() {
#ifdef FUN_BUILD
  // 框架自带的活动（扫描/选网/输密码/静态 IP 都在里面），比自己写一套稳
  LOGD("PgWifi: openActivity(WifiSettingActivity)");
  EASYUICONTEXT->openActivity("WifiSettingActivity");
#endif
}

}  // namespace pg
