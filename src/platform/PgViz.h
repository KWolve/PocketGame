/*
 * PgViz.h - 音频可视化数据源（**真 FFT**，不是"看着像在动"的假动画）
 *
 * 为什么要有它：
 *   网络收音机/在线音频播起来时，屏幕上只有"正在播放"四个字 —— 用户要的是
 *   "像手机音乐 App 那样，频谱/指针跟着音乐动"。要让频谱**真的**跟音乐动，
 *   必须在**真正出声的那条路上**取 PCM：
 *
 *     PgStream（ffmpeg 软解 → 重采样 22050/单声道/S16）
 *        → PgAudio::streamWrite(pcm, frames)      ← ★ 在这里 notePcm()
 *        → 常开 PCM 流 → 喇叭
 *
 *   这条正是 `pg::DeviceAudio` 的在线流入口（喇叭里放的就是它），所以频谱与
 *   耳朵听到的**必然一致**；换个位置取（比如在解码器出口）就会"静音时还在动"。
 *
 * 数据形态：低频 30 段（对数分频 60Hz~9kHz），每段 0..255。UI 层只负责把
 *   第 b 段映射成"第 b 根柱子的高度" —— 这里不做任何界面假设（同一份数据也
 *   可以驱动指针表 / 点阵环形）。
 *
 * 线程：`notePcm` 在**音频写线程**（PgStream 解码线程）调；`snapshot/level` 在
 *   **UI 线程**调。共享数据用一个互斥锁保护，但**只在真的要算 FFT 时（≤33 次/秒）**
 *   才加锁，所以 137 次/秒的灌数据路径上没有额外开销。
 */
#ifndef PG_VIZ_H_
#define PG_VIZ_H_

#include <stdint.h>

namespace pg {

/** 频谱段数（UI 侧按这个数建柱子；改这里必须同步改页面的控件数） */
const int kVizBands = 30;

class Viz {
 public:
  /** 灌一块 PCM（**必须与喇叭里放的一致**：22050Hz / 单声道 / S16）。
   *  由 PgAudio::streamWrite 调用；其它人不要调（会有第二份时钟/干扰）。 */
  static void notePcm(const int16_t *pcm, int frames);

  /** 取当前每段能量（0..255），写入 out[0..n)（n > kVizBands 时多余的填 0）。
   *  返回值 = 真正写入的段数。 */
  static int snapshot(uint8_t *out, int n);

  /** 总电平 0..255（RMS；指针表/电平条用） */
  static int level();

  /** 清空（停止播放时调，免得停台后频谱还留着一屏柱子） */
  static void reset();

  /** 统计（QA/日志用）：已灌帧数、已算 FFT 次数、上次各段最大值 */
  static long long framesFed();
  static long long fftCount();

  /** 显示增益（dB 映射的零点，默认 -62dB）：不同台响度差得多，现场可调。
   *  `vizgain <db>` 只是把这行日志换成指定值，方便标定。 */
  static void setGainDb(float db);
  static float gainDb();

  /* ==================== 双声道电平（VU 指针表用） ====================
   * ⚠️ **必须在"重采样/下混之前"按声道算好 RMS 再喂进来** —— 灌给喇叭的那条 PCM 是
   *    下混后的单声道（见 PgAudio 的 22050/单声道），拿它做不出左右差异。
   *    RMS 与采样率无关，所以这一路**不需要重采样、也不需要 FFT**，成本可忽略。
   * 电平做了 **VU 弹道**（上升 ~55ms / 回落 ~320ms）：真表的针是"甩上去、慢慢落"，
   *    不做弹道的话指针只会"抖"，不像仪表。 */
  static void noteStereoRms(float rmsL, float rmsR);   // 线性 RMS（0..1）

  /** 第 ch 声道的电平 0..255（ch: 0=L / 1=R；弹道后） */
  static int vuLevel(int ch);

  /** 第 ch 声道相对 **0 VU** 的 dB（0VU 默认 = -8dBFS，`setVuZeroDbfs` 可标定）。
   *  返回值不裁剪（可能大于 +3 或小于 -20），UI 侧自己决定怎么画。 */
  static float vuDb(int ch);

  /** ★ **表针回落**（由**页面定时器**每拍调，与音频无关）。
   *  为什么需要它：VU 电平是**解码线程**喂进来的 —— 一旦停播/换台/打不开，
   *  那一侧就再也不喂了，`sVuLin` 会**冻在最后一个值上**（实测：出错相位下表针
   *  僵在 -4.4dB 一动不动 —— 看上去像"表坏了"而不是"没声音了"）。
   *  这里做"**静音保持 ~400ms 后按回落弹道归零**"：换台时指针不会抖，
   *  真停了会像真表一样自己落底。**幂等**，可以每拍无脑调。 */
  static void vuTick();

  /** 标定：0 VU 对应多少 dBFS（默认 **-8**；实测本板电台流 RMS 在 -9~-24dBFS，
   *  -18 会让大部分台的指针顶死在刻度末端，见 docs/camera-radio-app.md §1.6） */
  static void setVuZeroDbfs(float db);
  static float vuZeroDbfs();
};

}  // namespace pg

#endif  // PG_VIZ_H_
