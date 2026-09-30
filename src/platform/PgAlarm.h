/*
 * PgAlarm.h - 闹钟（存储 + 到点检查 + 响铃 + 亮屏）
 *
 * 关键设计（为什么不用主界面定时器做检查）：
 *   主界面的 `onUI_Timer` **只在主界面在前台时跑** —— 用户进了 wifi / 蓝牙遥控这类
 *   独立 ftu 页面后就不跑了（实测：主界面的 QA 文件在独立页面上失效，就是同一个原因）。
 *   而闹钟必须"任何页面都会响" ⇒ **起一个独立的守护线程**（pthread），
 *   它不依赖任何 activity 的生命周期。
 *
 * 到点动作（顺序固定）：
 *   ① `Ringtone`（参考工程的官方音频播放器 `zk_audio_player`，循环播 alarm.wav，
 *      见 docs/audio-output.md）② `BRIGHTNESSHELPER->screenOn()` 亮屏
 *      ③ 回调通知 UI（可选，用来弹全屏提醒页）④ 「仅一次」的闹钟自动关闭并存盘
 *   响铃有**自动超时**（`kRingMaxMs`）：没人按停也不会响一整天。
 *
 * 存储：`/data/pocketgame_alarms.txt`（本板 /data 是可写 jffs2，重启不丢；
 * 回退 /mnt/extsd 与 /tmp）。写盘用「临时文件 + rename」防掉电。
 * 行格式：`enabled hour minute mask`（mask：0 = 仅一次；bit0..bit6 = 周日..周六，127 = 每天）。
 */
#ifndef PG_ALARM_H_
#define PG_ALARM_H_

namespace pg {

const int kMaxAlarms = 5;   // 与 UI（WinClockSuite 的 5 行）一致

struct AlarmItem {
  bool enabled;
  int hour;    // 0..23（本地时间）
  int minute;  // 0..59
  int mask;    // 0 = 仅一次；bit0=周日 .. bit6=周六（127 = 每天）
};

class Alarm {
 public:
  static Alarm &instance();

  void start();   // 载入 + 起守护线程（幂等）

  // ---- 存储 ----
  void load();
  bool save();
  int count() const { return count_; }
  const AlarmItem *items() const { return items_; }
  bool add(int hh, int mm, int mask);
  bool setItem(int i, int hh, int mm, int mask);   // 改已有条目（编辑用）
  bool setEnabled(int i, bool on);
  bool removeAt(int i);
  void clearAll();
  const char *path() const { return path_; }

  // ---- 到点 ----
  // 回调从**守护线程**调用：里面只能做"线程安全"的事（置标志、调框架接口），
  // 不要直接操作控件。用于弹全屏提醒页。
  typedef void (*FireCb)(int index, int hh, int mm);
  void setFireCb(FireCb cb);

  // ---- 自检 / QA ----
  void testFireNow();          // 立刻响一次（不落到存储）
  void fireIn(int sec);        // sec 秒后响一次（验收用；不落到存储）
  void snooze(int minutes);    // 贪睡：先停铃，minutes 后再响一次（不落到存储）
  bool ringing() const;
  void stopRing();             // 停止响铃（用户按任意键时也调它）
  int lastFiredIndex() const { return lastFired_; }
  const char *statusText() const { return status_; }   // 最近一次触发/停止的说明

 private:
  Alarm() {}
  ~Alarm() {}
  Alarm(const Alarm &);
  Alarm &operator=(const Alarm &);

  static void *guardEntry(void *self);
  void *guardLoop();
  void fire(int index, int hh, int mm);
  static void sleepMs(int ms);

  AlarmItem items_[kMaxAlarms];
  int count_ = 0;
  char path_[128] = {0};
  char status_[96] = {0};
  volatile bool running_ = false;
  bool started_ = false;
  long long pendingAtMs_ = -1;   // >0 = "N 秒后响"（自检用）
  long long ringUntilMs_ = 0;    // 0 = 不在响；否则到点自动停
  int lastFired_ = -1;
  int lastMinuteKey_ = -1;       // "年月日时分" 压缩值，防同一分钟重复触发
  FireCb fireCb_ = 0;
};

}  // namespace pg

#endif  // PG_ALARM_H_
