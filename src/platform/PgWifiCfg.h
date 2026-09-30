#pragma once
/*
 * PgWifiCfg.h - WiFi「回家快照」：切网前存盘，退出目标网模式后一键切回来
 *
 * 解决什么问题（血案见 PocketGame/docs/wifi-app.md 末尾）：
 *   zknet 的网络配置里**只保留一个 network 条目**（`update_config=1` + `SET_NETWORK 0`），
 *   配置落在 `/data/misc/wifi/wpa_supplicant.conf`：
 *
 *       ctrl_interface=/dev/socket/
 *       update_config=1
 *       network={
 *           ssid="TP-LINK_5G_C9E1"
 *           psk="..."
 *       }
 *
 *   ⇒ 只要连一次别的网络（局域网扫描找 STA 型摄像头时必须做的事），
 *   **原来那张网的 SSID+密码就被顶掉了**，用户得重新输密码才能回家。
 *
 * 做法（三步）：
 *   ① `keepHome(curSsid)`：把**当前这张网**的家底存成两份 ——
 *      - `pg_home.conf`：`wpa_supplicant.conf` 的**整份拷贝**（见下"为什么要整份"）
 *      - `pg_home.meta`：解析出来的 `ssid=` / `psk=`（给 UI 显示 + 兜底重连用）
 *   ② 切到目标网去扫描（这期间 zknet 随便顶，反正我们有备份）
 *   ③ `leaveTargetMode()`：把 `pg_home.conf` **覆盖回** `wpa_supplicant.conf`，
 *      再通过 wpa_supplicant 的控制口让它重读并重新关联：
 *          RECONFIGURE   ← 重新读配置文件
 *          RECONNECT     ← 关联回（现在文件里只有那张原网络了）
 *
 * ⚠️ **为什么备份"整份文件"而不是只记 ssid/psk**：原文件里可能还有
 *    `scan_ssid=1`（连隐藏网络的开关）、`key_mgmt`、`priority` 等 ——
 *    只回写 ssid/psk 会把这些丢掉（隐藏网络就再也连不上了）。整份拷贝回去是**逐字节等价**。
 *
 * ⚠️ 控制口协议：`/dev/socket/wlan0` 是 wpa_supplicant 的 ctrl_interface
 *    （unix **datagram** socket）。客户端要**自己 bind 一个本地路径**再 sendto 过去，
 *    服务端把应答发回我们的地址。板子上**没有 `wpa_cli`**，所以这里自己实现最小客户端。
 *    本模块 DGRAM 失败会退化成 STREAM 再试一次（不同 wpa 版本编法不同）。
 *
 * ⚠️ 位置选在 `/data/misc/wifi/`（**和原文件同一个可写分区**）：`/tmp` 是 tmpfs，
 *    设备一重启东西就没了；而"切网扫描"完全可能跨一次重启。
 */
namespace pg {

class WifiCfg {
 public:
  static WifiCfg *instance();

  /** 存一份"当前网络"的家底（整份 conf + 解析出的 ssid/psk）。
   * @param curSsid 当前**已连接**的 SSID（调用方给，省得这里再问框架一遍）
   * @return true = 存好了 */
  bool keepHome(const char *curSsid);

  bool hasHome() const;
  const char *homeSsid() const;     // 快照里那张网的 SSID（没有则 ""）
  const char *lastError() const;
  int homePskLen() const;           // 只报长度（**别把密码写进日志**）

  /** 进目标网模式：先把当前网络存好，再打标记。
   *  ⚠️ 标记期间 `keepHome()` 会被忽略 —— 否则用户一连上目标网，
   *     自动快照就把目标网当成"家"了（那就永远回不去）。 */
  bool enterTargetMode(const char *curSsid);
  bool targetMode() const;
  /** 只清标记（不还原文件）—— 用于"用户自己已经回到原网络"的收敛。 */
  void clearTargetMode();

  /** 出目标网模式：还原 conf + RECONFIGURE + RECONNECT。
   * @param ssidOut/pskOut 出参：快照里那张网的凭据（供调用方用 zknet 兜底重连）
   * @return true = 文件已还原（关联成不成还要看信号） */
  bool leaveTargetMode(char *ssidOut, int n, char *pskOut, int m);

  /** 打开/关闭 STA 网卡（wlan0）。
   *  ★★ 全信道嗅探必须先把 wlan0 关掉才切得动信道（见 PgSniff.h 的实测）：
   *     占用射频的是**接口本身**，不是关联状态。用完记得 up 回来。 */
  bool setStaIfaceUp(bool up);
  /** wlan0 现在是不是 up（用于"上次异常退出没还原"的自愈检查）。 */
  bool staIfaceUp();

  /** 底层原子操作（QA 也能直接调） */
  bool restoreConf();
  int wpaCmd(const char *cmd, char *resp, int rn);   // 返回读到的字节数，<0 = 失败
  /** wpaCmd 的单次执行体（失败后的重探逻辑在 wpaCmd 里）。 */
  int wpaCmdOnce(const char *cmd, char *resp, int rn);
  const char *ctrlPath() const;

  static const char *confPath();
  static const char *homeConfPath();
  static const char *metaPath();

 private:
  void setErr(const char *fmt, ...);
};

}  // namespace pg
