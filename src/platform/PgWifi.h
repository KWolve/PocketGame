/*
 * PgWifi.h - WiFi 状态/控制（设备侧实现）
 *
 * 用 zknet 包（`net/NetManager.h` → `NETMANAGER->getWifiManager()`）读状态、开关 WiFi；
 * 「打开系统 WiFi 设置」直接跳框架内置的 WifiSettingActivity（标准 wpa_supplicant 那套
 * 扫描/选网/输密码界面）。core 层只通过 Host 的 wifiXXX() 虚函数访问，不依赖 zknet。
 */
#ifndef PG_WIFI_H_
#define PG_WIFI_H_

namespace pg {

class WifiService {
 public:
  static bool supported();
  static bool enabled();
  static bool connected();
  static void ssid(char *buf, int n);
  static void ip(char *buf, int n);
  static void mac(char *buf, int n);
  static int rssi();
  static void setEnabled(bool on);
  static void openSystemSettings();  // 打开框架内置 WiFi 设置界面
};

}  // namespace pg

#endif  // PG_WIFI_H_
