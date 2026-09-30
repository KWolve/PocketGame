/*
 * PgBt.h - 蓝牙 BLE HID 遥控器（外设侧）
 *
 * 架构与 PgWifi/PgDlna 一致：core 层只通过 Host 的虚函数访问，**不 include 任何
 * btstack 头**；本模块（platform 层）负责真正的 btstack 调用。
 *
 * 线程模型（btstack 硬规矩，踩过）：
 *   - 全部 btstack API 都在**跑 run loop 的那条线程**里调，包括
 *     `btstack_run_loop_add_data_source` / `add_timer`（放主线程会让 run loop
 *     起来后永远停在 HCI_STATE_INITIALIZING，连 tick 日志都没有）。
 *   - UI 线程要发按键 → 走 socketpair 投递（见 sendKey），绝不跨线程碰 btstack。
 *
 * 初始化顺序：rtk_init（上电时序 + 下补丁固件 + 切 1500000）→ TLV(/data/bttlv.db)
 *   → hci_init(H5 + 偶校验 8E1) → l2cap/sm/att/gatt_client/hids_device
 *   → 广播（HID UUID + Appearance + 名字）+ 扫描 → run loop。
 *
 * ⚠️ rtk_init 会间歇性失败（OP_H5_SYNC timeout），一旦失败后续难恢复：
 *    最可能是 BT 电源域只有**物理冷启动**才能回到 ROM 态（软 reboot 不断它的电）。
 *    所以产品上 BT **只初始化一次**（进本应用时），不要反复重初始化。
 */
#ifndef PG_BT_H_
#define PG_BT_H_

#include <stdint.h>

namespace pg {

class Bt {
 public:
  static Bt *instance();

  // 启动（幂等）：第一次调用起 BT 线程，之后直接返回 true
  bool start(const char *dev = "/dev/ttyS2");
  void stop();
  bool started() const { return started_; }

  // 0 = 未启动, 1 = 初始化中（rtk_init/HCI 同步）, 2 = 就绪（HCI_STATE_WORKING）
  int state() const;
  const char *stateText() const;
  bool connected() const;  // 主机已启用 HID 输入报告（= 真的能用）
  bool paired() const;     // 已配对（配对信息写进了 TLV）
  int advCount() const;    // 扫描到的广播条数（证明 BLE 在跑）
  const char *name() const; // 广播名（手机搜索时看到的名字）
  int errCode() const;     // 0 正常；-1 = rtk_init 失败

  /* Consumer Control 报告位图（bit 位置对应 HID 报告描述符里 usage 的书写顺序）。
   * ⚠️ 必须加前缀：`KEY_POWER`/`KEY_MENU`/... **是 linux/input.h 的宏**，
   *    一旦有别的文件 include 了 input 相关头，裸名会被宏替换成数字 → 语法错（踩过）。 */
  enum {
    RC_POWER = 1u << 0,
    RC_MENU = 1u << 1,
    RC_NEXT = 1u << 4,
    RC_PREV = 1u << 5,
    RC_VOL_UP = 1u << 6,
    RC_VOL_DOWN = 1u << 7,
    RC_MUTE = 1u << 8,
    RC_PLAY = 1u << 9,
    RC_HOME = 1u << 12,
    RC_BACK = 1u << 13,
  };

  // UI 线程调用：投递给 BT 线程发一份 HID 输入报告（mask 为上面的位图）
  void sendKey(uint16_t mask, bool alsoRelease = true);

  /* ==================== 主机侧：连别的 BLE HID 设备（学习遥控器按键）====================
   * 本机既能当 HID 外设（上面），也能当 HID 主机（这里）—— 后者用于"学习"：
   * 连上真实的 BLE 遥控器/键盘，把它的按键报告读进来，再在本机建"物理键 → 动作"映射。
   * ⚠️ "抓包重放别人的蓝牙键"在 BT 上不成立（HID 按键走加密链路），
   *    正解就是这种"本机当主机去读"的形态。 */
  void hostScanStart();                   // 清空并开始收集扫描结果
  int  scannedCount() const;
  const char *scannedAddr(int i) const;   // "AA:BB:CC:DD:EE:FF"
  const char *scannedName(int i) const;
  int  scannedRssi(int i) const;
  void hostConnect(int i);                // 连列表里第 i 个设备
  void hostDisconnect();
  int  hostState() const;                 // 0 空闲 1 连接中 2 已连(发现服务) 3 已订阅(学习中)
  const char *hostStateText() const;
  const char *hostPeer() const;
  int  learnedCount() const;              // 学到的报告条数（最多 8 条，滚动覆盖）
  const char *learnedHex(int i) const;    // 第 i 条报告的文本（按键名，如"音量加"）
  uint16_t learnedMask(int i) const;      // 第 i 条报告映射成的"我们的"consumer 位图（重放用）
  static uint16_t usageToMask(uint16_t usage_page, uint16_t usage);

  /* ==================== HID 报告解析（"学习"的核心：把 hex 变成按键名）====================
   * `btstack_hid_parser` 是**纯函数**（不依赖 btstack 全局状态），所以可以在 UI 线程安全调用。
   * 自验证思路：拿**我们自己的**报告描述符 + 构造几个报告跑一遍 —— 不需要第二台设备
   * 就能确认解析器工作正常（本机本来就是 HID 外设，格式已知）。 */
  static const uint8_t *ourReportMap(int *len);
  static const char *usageName(uint16_t usage_page, uint16_t usage);
  // 解析一个报告，把"被按下的字段"写成可读串；返回识别到的字段数
  static int parseReport(const uint8_t *desc, int dlen, const uint8_t *rep, int rlen,
                         char *out, int outn);
  // 自验证：用我们的描述符解析几个构造报告并打日志（QA 命令 btselfparse 调）
  void selfTestParse();

  // 人类可读的按键名（界面显示用）
  static const char *keyName(uint16_t mask);

  /* 实现体（定义在 .cpp）。放 public 是为了让 .cpp 里的自由函数/回调能访问它
   * （btstack 的回调是 C 风格的函数指针，不能是成员函数）。 */
  struct Impl;

 private:
  Bt();
  ~Bt();
  Bt(const Bt &);
  Bt &operator=(const Bt &);

  Impl *impl_;
  bool started_;
};

}  // namespace pg

#endif  // PG_BT_H_
