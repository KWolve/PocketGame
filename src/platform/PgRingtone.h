/*
 * PgRingtone.h - 闹钟铃声播放（走参考工程的**官方音频播放器** zk_audio_player）
 *
 * 为什么单独一条通道、不走 PgAudio 的音效系统（2026-09-13 查实，见 docs/audio-output.md）：
 *   ① 用户定规：**闹钟响铃走"音频播放器"**（作者 2026-09-13）；
 *   ② "音频播放器"在本板的可用实现 = 参考工程自带的 `zk_audio_player`
 *      （`libzkmedia.a`，实测打开 `pcmC0D0p` = card0 = 喇叭；而 EasyUI 的 ZKMediaPlayer
 *       走 `pcmC1D0p` = card1，本板没接喇叭 ⇒ 静音）；
 *   ③ 铃声要**循环响几分钟**，而 PgAudio 的音效是"一次性播完就停"。
 *
 * ⚠️ 它是**独占**的（官方注释明说），而 PgAudio 的常开流长期独占 `hw:0,0`
 * ⇒ 本类负责自动"让出 / 拿回"：start() 前 `releasePcm()`，stop() 后 `acquirePcm()`。
 *   代价：响铃期间游戏音效静音（闹钟场景本该如此）。
 *
 * 音源：`resources/audio/*.wav`（16bit PCM、单声道，用 CONFIGMANAGER->getResFilePath 定位）。
 */
#ifndef PG_RINGTONE_H_
#define PG_RINGTONE_H_

#include <pthread.h>
#include <stdint.h>

namespace pg {

class Ringtone {
 public:
  static Ringtone &instance();

  // 开始**循环**播放 resources/audio/<wavRelPath>（如 "audio/alarm.wav"）。
  // 幂等：已在响则先停掉再换新铃声。返回 false = 文件缺失/格式不支持/线程起不来。
  bool start(const char *wavRelPath);
  void stop();                       // 停止并把声卡还给 PgAudio（幂等）
  bool playing() const { return running_; }
  const char *current() const { return name_; }   // 当前铃声（自检用）

  // 仅自检：上一次 start 失败的原因（日志之外给 QA 用）
  const char *lastError() const { return err_; }

 private:
  Ringtone() {}
  ~Ringtone() {}
  Ringtone(const Ringtone &);
  Ringtone &operator=(const Ringtone &);

  static void *threadEntry(void *self);
  void *threadMain();

  uint8_t *pcm_ = 0;      // 预载的 PCM 数据（循环用，播放期间零 I/O）
  uint32_t size_ = 0;     // 字节
  int ch_ = 1;
  int rate_ = 22050;
  volatile bool running_ = false;
  bool threadAlive_ = false;
  bool pcmReleased_ = false;   // 是否已把 PgAudio 的声卡让出去
  pthread_t th_;
  char name_[64] = {0};
  char err_[96] = {0};
};

}  // namespace pg

#endif  // PG_RINGTONE_H_
