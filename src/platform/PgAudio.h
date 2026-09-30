/*
 * PgAudio.h - 音效播放（V851s 音频输出）
 *
 * 实现：**一条常开的 PCM 输出流**（hw:0,0，片内 codec = 板载喇叭那块卡）。
 *   - 独立的音频线程按周期（160 帧 ≈ 7ms）不停往声卡写数据；
 *     有音效时写音效数据，**空闲时写静音**，流永不停止。
 *   - 这样做的原因：爆音（pop）来自反复 open/close PCM —— 每次开关都会让 DAC
 *     充放电，听感就是"咔/噗"一声。持续灌数据（哪怕是 0）能让 DAC 一直处于
 *     工作状态，音效进出都不会有 pop。
 *   - 音效文件在 init() 时一次性预载并转成流参数（22050Hz/单声道/S16），
 *     触发音效只是换一块内存指针，UI 线程零 I/O、零阻塞。
 *
 * 为什么不用框架的 ZKMediaPlayer / AO：
 *   框架媒体播放器（ZKMediaPlayer → eyesee-mpp AO）**自己拼声卡设备名（hw:1,0）**，
 *   不走 ALSA 名字解析，而本板喇叭接在 card0 —— 于是日志全正常却完全静音；
 *   改 asound.conf / ALSA_CONFIG_PATH 都无效（实测：故意写坏配置会让播放进程退出，
 *   说明配置确实被读；但合法覆盖后仍打开 pcmC1D0p）。V85X 平台也没有
 *   zkmedia/audio-utility 这类"播 PCM 的包"（只有 F133/f136 有）。
 *
 * 音效文件放 resources/audio/*.wav，路径用 CONFIGMANAGER->getResFilePath() 解析；
 * 同一音效 60ms 内不重复触发（防连发把音轨打满）。
 */
#ifndef PG_AUDIO_H_
#define PG_AUDIO_H_

#include "core/PgGame.h"

namespace pg {

class DeviceAudio {
 public:
  DeviceAudio();
  ~DeviceAudio();

  bool init();          // 起音频线程 + 预载音效（幂等）
  void shutdown();
  void playSfx(int sfxId);
  // 音效开关（主界面"音效"按钮）：关掉只出不写音效（流照旧跑静音，仍无 pop）
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }
  // 重新检查并打开 codec 输出开关（SPK/LINEOUT Switch）。启动时自动做一次；
  // 暴露出来是给现场自检/排查用（QA 命令 audiosw），正常流程不需要调。
  void reassertOutputSwitch();
  // 音量：按档步进（delta>0 升 / <0 降），返回调整后的音量百分比 0~100；失败返回 -1。
  // 走 codec 的"数字音量"INT 控件（本板 tinymix 里的 digital volume），内部
  // 「读当前 → ±步进 → 写 → 回读校验」，无状态、不怕被外部改。
  // 由物理音量键（103 = 音量- / 105 = 音量+）调用，调用方负责给听觉反馈（播一声）。
  int volumeStep(int delta);
  // 按百分比 0..100 直接设置（用于**启动时恢复**存档里的音量）。
  // 返回实际生效的百分比，失败 -1。
  int setVolumePercent(int pct);

  /* ==================== 静音（codec 输出开关） ====================
   * 实现 = 写 codec 的 `SPK Switch` / `LINEOUT Switch`（关掉就是真静音，不是把音量调 0）。
   * ★ 为什么不把音量调到 0 当静音：那样"恢复到多少"要靠额外记住一个数，
   *   而且从 0 往上按要一档一档爬回来；开关是**可逆的硬件状态**，恢复只差一次写。
   *   ⚠️ 与 ensureSpeakerOn() 是同一组控件：拿回声卡（acquirePcm）之后会重新打开，
   *     所以"静音状态"要在调用方（pg 层）记着，并在需要时重放。 */
  bool setMuted(bool muted);
  bool isMuted();
  // 音效文件缺失时置 false（可在 UI 上提示）
  bool available() const { return available_; }
  // 当前实际生效的输出通路（日志/自检用）
  const char *backend() const { return backend_; }

  /* ==================== 在线流音频（PgStream 灌 PCM 用） ====================
   * 为什么走这条而不是框架的 AO：框架播放器（zkmedia→eyesee-mpp AO）自己拼声卡设备名
   * （拼成 hw:1,0，而本板喇叭在 card0）→ 日志全正常却完全静音。我们这条常开 PCM 流
   * 是实测能出声的那条，所以把在线流的音频也灌进它。
   * 格式必须与流参数一致：**22050Hz / 单声道 / S16**（调用方负责重采样）。 */
  // 开/关在线流音频队列。**返回 false = 声卡现在不可用**（被 releasePcm() 释放了），
  // 调用方（PgStream）据此降级为"只播视频 + 墙钟同步"，不要傻等队列。
  bool streamOn(bool on);
  int streamWrite(const int16_t *pcm, int frames);  // 推入；返回实际接收的帧数（满则少收）
  int streamQueuedFrames() const;                   // 队列里还没播掉的帧数（算 A/V 时钟用）

  /* ==================== 声卡占用控制（现场最重要的一条） ====================
   * 常开流的代价：`hw:0,0` 由我们**长期独占** —— 实测此时别的进程（含 `/bin/tinyplay`）
   * 打开 card0 会**阻塞住**（不是报错，是挂住），现场表现就是"设备上别的程序一放声音就卡"。
   * 所以对外提供显式的「让出 / 拿回」：
   *   releasePcm() —— 停音频线程 + close(PCM)，把 card0 还给系统（音效期间才需要它）；
   *   acquirePcm() —— 重新 open + 起线程（幂等；重新打开后会自动再校一次输出开关）。
   * 两侧都不销毁实例（音效仍在内存里预载着，拿回来即刻可用）。QA：`pcmfree` / `pcmopen`。 */
  bool releasePcm();
  bool acquirePcm();
  bool pcmHeld() const;  // 当前是否占着声卡（日志/自检用）

  /* ---- 空闲自动让出（默认 30 秒；0 = 关，永远常开）----
   * 动机：常开流独占 `hw:0,0` 时，别的进程打开 card0 会**挂住**（实测 tinyplay 直接挂）。
   *   专用机上没别的应用，但产测脚本/系统提示音可能要用 —— 空闲让出能把这条彻底消掉。
   * 代价：让出后**第一次出声**要先重新打开声卡（openStream + 输出开关枚举），
   *   实测约 100ms 量级（日志 `已拿回声卡` 前后），所以是"按需拿回"。
   * ⚠️ 在线流播放期间**不能**让出（PgStream 正往这条流灌 PCM），由调用方传 busy 判断。
   * 想回到"永远常开"：QA `pcmidle 0`。 */
  void setPcmIdleSec(int sec);
  int pcmIdleSec() const { return pcmIdleSec_; }
  /* 距上次"要用声音"（触发音效 / 往在线流队列写 PCM）过去了多少毫秒 */
  long long idleMs() const;

 private:
  void *impl_;  // 内部实现（ALSA 句柄 + 音频线程 + 预载音效）
  const char *backend_;
  bool enabled_;
  bool available_;
  int pcmIdleSec_ = 30;  // 空闲自动让出声卡（秒）；0 = 关闭（永远常开）
  long long lastActMs_ = 0;  // 上次"要用声音"的时刻（playSfx / 在线流灌 PCM）
};

// 进程级音量钩子：独立 activity（wifi.ftu 的 wifiLogic）不持有 DeviceAudio 实例
// （音频线程只能有一份），通过 mainLogic 注入的钩子转发音量操作。
// 未注入时 volumeStepGlobal 返回 -1（视为失败，UI 不弹进度条）。
void setVolumeHook(int (*fn)(int delta));

/* ★ 音量**变化广播**（全局音量 OSD 用，2026-09-14 加）。
 *
 * 背景：音量 OSD 原来主界面/ WiFi 页各有一份，工具页/套件/IPTV 上按音量键只有声音没界面。
 * 现在统一到**状态栏**（SysApp，悬浮在所有页面之上）——但它怎么知道音量变了？
 * 做法：所有界面的音量操作都收敛到 `volumeStepGlobal()`，就在这里广播一次。
 *
 *   `pct >= 0`（真的改了音量）才广播；失败（-1）不广播，UI 不该弹。
 *   回调在**调用方线程**执行（正常是 UI 线程）——订阅方要自己保证"改控件在 UI 线程"
 *   （状态栏就是这么做的：回调只记一个数，控件在定时器里更新）。 */
void setVolumeNotifyHook(void (*fn)(int pct));

// 进程级音频实例（mainLogic 在 init 时注入）。PgStream 是静态类、不持有实例，
// 靠它拿到那条常开 PCM 流来灌在线流的音频。
void setGlobalAudio(DeviceAudio *a);
DeviceAudio *globalAudio();
int volumeStepGlobal(int delta);

/* ==================== 绝对音量（2026-09-17 用户需求） ====================
 * 用户原话：「进度条不需要按键，直接拖动就好了」。
 *
 * 为什么不能拿 volumeStepGlobal 凑：它是**按档步进**（delta）语义 —— 要让 30% 拖到 70%，
 * 就得连调 40 次，每次都会广播一次（音量 OSD 会闪 40 下），而且 codec 每档量化后
 * 未必停在用户想要的百分比。拖动是**绝对定位**语义，必须有一条自己的通路。
 *
 * 注入方式：mainLogic 在 init 时把 DeviceAudio::setVolumePercent 接进来
 * （与 volumeStepHook 同一处）。
 */
void setVolumeSetHook(int (*fn)(int pct));      // 注入：绝对设值，返回实际生效的百分比（-1=失败）
int setVolumePercentGlobal(int pct);            // 设绝对值；成功则更新"当前音量"并按需广播

/* 「抑制音量 OSD」的时间窗（毫秒，只延长不缩短）。
 * 场景：系统设置页拖自己那条音量条时，不该再弹全局音量面板（用户正看着那条）。
 * ★ 用**时间窗**而不是布尔标志：时间到自动失效，不会有"忘了复位"的粘性状态
 *   —— 本工程栽过 swipeProgress 的粘性泄漏（见 platform/PgSwipe.cpp 的注释）。 */
void suppressVolumeOsd(int ms);

/* ==================== 进程级静音（2026-09-16 用户需求） ====================
 * 用户原话：「按键调整音量的默认去掉静音，音量弹出框也可以快速静音」。
 *
 * 设计：
 *   · `pg` 层持有**唯一的静音状态** sMuted（真值仍在 codec 开关上，这里只是缓存，
 *     供 UI/按键逻辑读取，免得每次去查 ALSA）；
 *   · 静音变化 → 走钩子写真开关（mainLogic 注入，与音量钩子同一套机制）；
 *   · `volumeStepGlobal(+1)` 在静音时**先取消静音**并恢复上次音量 —— 这就是
 *     "按键调整音量的默认去掉静音"：现场最常见的抱怨是"按了 + 还是没声音"。
 *
 * 注：系统默认**不静音**（启动时不写开关，保持 codec 的默认导通状态）。 */
void setMuteHook(bool (*fn)(bool muted));      // 注入：真改开关，返回是否成功
void setMuteNotifyHook(void (*fn)(bool muted));  // 静音变化广播（OSD 图标刷新）
bool isMutedGlobal();
bool setMutedGlobal(bool muted);               // 返回是否成功
void noteVolumePercent(int pct);               // 让 pg 层知道"当前音量"（恢复静音时用）

}  // namespace pg

#endif  // PG_AUDIO_H_
