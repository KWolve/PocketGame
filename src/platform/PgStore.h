/*
 * PgStore.h - 最高分 + 设置项持久化
 *
 * 文件格式：前 16 行 = 各游戏最高分（一行一个整数），之后是设置项（`key=value`）：
 *   sound=1 / sound=0        音效开关（主界面右上角按钮）
 * 追加设置项对老存档向后兼容（老文件没有该行 → 用默认值）。
 *
 * 设备上按顺序尝试可写路径（首个可写路径胜出）：
 *   /mnt/extsd/pocketgame.dat  ->  可持久（TF 卡）
 *   /tmp/pocketgame.dat        ->  仅本次开机有效
 * PC 上（hosttest）用 ./pocketgame.dat。
 */
#ifndef PG_STORE_H_
#define PG_STORE_H_

namespace pg {

class ScoreStore {
 public:
  ScoreStore();
  ~ScoreStore();

  void load();
  void save();

  int high(int index) const;
  void setHigh(int index, int score);

  bool soundOn() const { return sound_ != 0; }
  void setSoundOn(bool on);

  // 音量（0..100 百分比）；-1 = 从未调过 → 启动时**不主动设置**，保持驱动默认
  int volumePercent() const { return volume_; }
  void setVolumePercent(int pct);

  /** 投屏/在线流的画面旋转（顺时针度）。-1 = auto（跟随视频自带的旋转元数据）；
   *  0/90/180/270 = 固定角度。竖屏设备看横屏内容、或手机竖拍视频方向不对时改它。
   *  存起来的原因：本工程"停止投屏就会复位应用"（解码通道限制，见 docs/online-media.md
   *  §十五），只存在内存里的话一停止就丢了。 */
  int rotDeg() const { return rot_; }
  void setRotDeg(int deg);

  /** zkmedia 码流缓冲（VBV）大小，给 `ZKMEDIA_H264_VBVSIZE` 用。默认 1MB。
   *  为什么要可调：默认小值下 **720p（1/2 缩放解码）+ 旋转** 会让进程静默退出
   *  （见 docs/h264-direct.md），现场遇到高码率源可以往上调（参考工程留了 2MB/3MB 的档位）。
   *  ⚠️ 只在**起播时**读（库在 dlopen 时读环境变量）→ 改完要重启应用才生效。 */
  int vbvBytes() const { return vbv_; }
  void setVbvBytes(int n);

  const char *path() const { return path_; }

  /* 最高分槽位数 = **应用数上限**（slot 是稳定存档索引，见 PgGames.cpp 的 kAppTable）。
   *
   * ★★ 2026-09-15 从 16 扩到 32（摇骰子接入时发现的**静默失败**）：
   *    应用已经排到 slot 23（消消乐 22 / 摇骰子 23），而这里是 16 ⇒
   *    `setHigh(23, …)` 被 `index >= MAX_GAMES` 直接 return —— **不报错、不落盘、
   *    最高分永远显示 0**。受影响的不止新游戏：slot 16~23（五子棋/反应计时/时钟套件/
   *    网络电视/信号探针/消消乐…）的最高分一直没存上。
   *    扩到 32 对老存档**向下兼容**：load() 是按行顺序读分数的，旧文件只有 16 行，
   *    剩下的默认 0，再 save() 时自然补齐。（应用数再涨就继续加，别再让它静默。）
   *
   * ★★ 2026-09-16 再扩到 **40**：本次加了 5 个应用，slot 排到 **31**，
   *    而 32 这条线**正好贴上限**（`index >= MAX_GAMES` 时 31 还合法，但下一个应用
   *    就会又被静默丢弃）。留 8 个空位，加应用时只需看这一行。
   *
   * ★★ 2026-09-16 挪到 **public**：它同时是"**按 slot 下标的数组/缓存**"的容量真值
   *    —— mainLogic 的 `metaForSlot()` 缓存与它同一量级。以前那里各自写死 32，
   *    结果 slot 32（系统设置）拿不到元信息、卡片变空白（用户报"图标找不到"）。
   *    ⇒ 公开出来让所有"slot 下标容器"共用**一个**上限，别再各写一份。 */
  static const int MAX_GAMES = 40;

 private:
  int high_[MAX_GAMES];
  int sound_;   // 1 = 音效开
  int volume_;  // -1 = 未设置；0..100 = 上次音量百分比（物理音量键改动时落盘）
  int rot_;     // -1 = auto；0/90/180/270 = 投屏画面固定旋转角
  int vbv_;     // zkmedia 码流缓冲字节数（默认 1048576）
  char path_[128];
};

}  // namespace pg

#endif  // PG_STORE_H_
