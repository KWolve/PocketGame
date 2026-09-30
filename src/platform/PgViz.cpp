/*
 * PgViz.cpp - 音频可视化（真 FFT）实现
 *
 * 链路：notePcm(音频写线程) → 环形缓冲 → 每 30ms 取 512 点做 Hann 窗 + radix-2 FFT
 *       → 30 段对数分频取峰值 → dB 映射 + 快起慢落平滑 → snapshot(UI 线程)
 *
 * 为什么是 512 点 @22050Hz：频率分辨率 43Hz —— 60Hz 起的低频段不至于挤在一格里；
 *   CPU 成本一轮 ≈ 5·N·log2(N) ≈ 2.3 万次浮点，33 次/秒 ≈ 76 万次/秒，
 *   在本板（Cortex-A7 + 硬浮点）上 ≪1%；实测见 docs/camera-radio-app.md。
 *
 * ⚠️ 只吃**单声道 22050** —— 这是 PgStream 重采样后灌给喇叭的格式（见 PgAudio.h）。
 *    若将来改了 STREAM_RATE，这里的频率映射与分频表要跟着改。
 */
#include "platform/PgViz.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

namespace pg {
namespace {

const int kN = 512;                 // FFT 点数
const int kMsPerFft = 30;           // 最多 33 次/秒（与页面 100ms 一拍、屏保 25ms 一拍都对得上）
const float kFMin = 60.0f;          // 最低频段中心
const float kFMax = 9000.0f;        // 最高频段中心
const float kAttack = 0.55f;        // 上升快（跟着鼓点跳）
const float kDecay = 0.13f;         // 下落慢（好看，不至于闪）
const float kSpanDb = 50.0f;        // dB 动态范围（增益零点往上算这么多就满格）

int16_t sRing[kN * 2];
int sRingW = 0;                     // 写指针
int sHave = 0;                      // 已攒样本数
long long sLastFftMs = 0;
long long sFramesFed = 0;
long long sFftCount = 0;

float sBands[kVizBands];            // 平滑后的 0..1
/* 显示增益（dBFS 零点；<= 这个值算 0）。标定过的默认值：见 docs/camera-radio-app.md
 * —— 电台流普遍比 CD 低 10~20dB，-70/50 这套实测能把响的台打到 200+、轻的台留出层次。 */
float sGainDb = -70.0f;

pthread_mutex_t sMu = PTHREAD_MUTEX_INITIALIZER;

/* UI 可读的快照（snapshot/level 用；只在算完 FFT 时更新） */
uint8_t sSnap[kVizBands];
int sLevel = 0;

/* ==================== 双声道 VU 电平（独立于 FFT：RMS + 弹道） ====================
 * 为什么与 FFT 分开：VU 表要的是"每声道的响度"，不关心频率分布、也不需要重采样；
 *   而且它必须来自**下混之前**的声道数据（见 PgViz.h 的说明）。 */
float sVuLin[2] = {0.0f, 0.0f};      // 弹道后的线性幅值（0..1）
int sVuLevel[2] = {0, 0};
float sVuZero = -8.0f;               // 0 VU = -8dBFS（实测电台流 RMS 在 -9~-24dBFS 区间）
long long sVuLastMs = 0;
const float kVuRiseMs = 55.0f;       // 上升时间常数（针"甩上去"）
const float kVuFallMs = 320.0f;      // 回落时间常数（针"慢慢落" —— 真表的观感就在这）
/* 停喂多久才开始回落：解码帧平时 20~46ms 一块；留 400ms 免得正常播放中
 * 偶发的小停顿让针乱掉（HLS 起播/卡顿确实可能超过它 —— 那种情况本来就该落针）。 */
const int kVuHoldMs = 400;

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 线性幅值 → 相对 0VU 的 dB → 0..255（**刻度 -20dB .. +3dB 铺满整个弧**）。
 * 抽出来是因为 `noteStereoRms`（音频线程）与 `vuTick`（UI 线程）两处都要用 ——
 * 两处必须**同一套换算**，否则回落那一刻的针位会跟正常播放时对不上。 */
int vuLinToLevel(float lin) {
  if (lin < 1e-7f) lin = 1e-7f;
  const float db = 20.0f * log10f(lin) - sVuZero;
  float v = (db + 20.0f) / 23.0f;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  return (int)(v * 255.0f + 0.5f);
}

/* ---- radix-2 复数 FFT（就地；n 必须是 2 的幂）---- */
void fftRadix2(float *re, float *im, int n) {
  /* 位反转置换 */
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * 3.14159265358979323846 / (double)len;
    const float wr = (float)cos(ang), wi = (float)sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; ++k) {
        const int a = i + k, b = i + k + len / 2;
        const float xr = re[b] * cr - im[b] * ci;
        const float xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        const float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

/* 第 b 段（0..kVizBands-1）覆盖的 bin 区间：[lo, hi) */
void bandBins(int b, int sampleRate, int *lo, int *hi) {
  const float r = powf(kFMax / kFMin, 1.0f / (float)kVizBands);
  const float f0 = kFMin * powf(r, (float)b);
  const float f1 = f0 * r;
  const float binHz = (float)sampleRate / (float)kN;
  int a = (int)(f0 / binHz), c = (int)(f1 / binHz) + 1;
  if (a < 1) a = 1;                       // 丢掉 DC
  if (c > kN / 2) c = kN / 2;
  if (c <= a) c = a + 1;
  *lo = a;
  *hi = c;
}

float re[kN], im[kN], win[kN];
bool sWinReady = false;

void computeOnce(int sampleRate) {
  if (!sWinReady) {
    for (int i = 0; i < kN; ++i) {
      win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979323846f * (float)i / (float)(kN - 1));
    }
    sWinReady = true;
  }
  /* 取最近 kN 个样本（环形缓冲）；sRingW 指向"下一个要写的位置" */
  long long sum2 = 0;
  for (int i = 0; i < kN; ++i) {
    const int idx = (sRingW + i) % kN;
    const float s = (float)sRing[idx] * win[i];
    re[i] = s;
    im[i] = 0.0f;
    sum2 += (long long)sRing[idx] * sRing[idx];
  }
  fftRadix2(re, im, kN);

  /* 幅度归一化：先除以 N（FFT 增益），再除以 32768（int16 → ±1）。
   * ⇒ **满幅单音 = 0.5**（Hann 窗的相干增益 0.5），即 -6dBFS，符合"dB 直觉"。 */
  const float scale = 2.0f / ((float)kN * 32768.0f);
  for (int b = 0; b < kVizBands; ++b) {
    int lo, hi;
    bandBins(b, sampleRate, &lo, &hi);
    float peak = 0.0f;
    for (int k = lo; k < hi && k < kN / 2; ++k) {
      const float m = sqrtf(re[k] * re[k] + im[k] * im[k]) * scale;
      if (m > peak) peak = m;
    }
    /* dB → 0..1（sGainDb 以下记 0） */
    float v = 0.0f;
    if (peak > 1e-7f) {
      const float db = 20.0f * log10f(peak);
      v = (db - sGainDb) / kSpanDb;
      if (v < 0.0f) v = 0.0f;
      if (v > 1.0f) v = 1.0f;
    }
    float prev = sBands[b];
    sBands[b] = (v > prev) ? (prev + (v - prev) * kAttack) : (prev + (v - prev) * kDecay);
    sSnap[b] = (uint8_t)(sBands[b] * 255.0f + 0.5f);
  }
  /* 总电平：整体 RMS → 0..255（指针/电平条用；比峰值稳） */
  const float rms = sqrtf((float)sum2 / (float)kN) / 32768.0f;
  int lv = 0;
  if (rms > 1e-6f) {
    const float db = 20.0f * log10f(rms);
    float v = (db - sGainDb) / kSpanDb;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    lv = (int)(v * 255.0f + 0.5f);
  }
  /* 总电平也用慢落，免得闪 */
  if (lv > sLevel) sLevel = lv;
  else sLevel = sLevel + (int)((lv - sLevel) * 0.25f);
  ++sFftCount;
}

}  // namespace

void Viz::notePcm(const int16_t *pcm, int frames) {
  if (!pcm || frames <= 0) return;
  /* 先只搬样本进环（不加锁：写指针只有本线程改，读者只用"最近 kN 个"） */
  for (int i = 0; i < frames; ++i) {
    sRing[sRingW] = pcm[i];
    sRingW = (sRingW + 1) % kN;
  }
  sHave += frames;
  sFramesFed += frames;
  if (sHave < kN) return;                      // 还没攒够一轮
  const long long t = nowMs();
  if (t - sLastFftMs < kMsPerFft) return;
  sLastFftMs = t;
  sHave = 0;
  pthread_mutex_lock(&sMu);
  computeOnce(22050);                          // STREAM_RATE（见 PgAudio.h）
  pthread_mutex_unlock(&sMu);
}

int Viz::snapshot(uint8_t *out, int n) {
  if (!out || n <= 0) return 0;
  pthread_mutex_lock(&sMu);
  const int m = (n < kVizBands) ? n : kVizBands;
  memcpy(out, sSnap, (size_t)m);
  for (int i = m; i < n; ++i) out[i] = 0;
  pthread_mutex_unlock(&sMu);
  return m;
}

int Viz::level() {
  pthread_mutex_lock(&sMu);
  const int v = sLevel;
  pthread_mutex_unlock(&sMu);
  return v;
}

void Viz::reset() {
  /* ⚠️ 只清"给 UI 看的"那几份（持锁）；环形缓冲/写指针只有音频线程能动，
   *    从 UI 线程去动它就是数据竞争（读者只用"最近 kN 个样本"，不清也自然会被覆盖）。 */
  pthread_mutex_lock(&sMu);
  memset(sSnap, 0, sizeof(sSnap));
  for (int i = 0; i < kVizBands; ++i) sBands[i] = 0.0f;
  sLevel = 0;
  pthread_mutex_unlock(&sMu);
}

long long Viz::framesFed() { return sFramesFed; }
long long Viz::fftCount() { return sFftCount; }

/* ==================== 双声道电平 ==================== */
void Viz::noteStereoRms(float rmsL, float rmsR) {
  if (rmsL < 0.0f) rmsL = 0.0f;
  if (rmsR < 0.0f) rmsR = 0.0f;
  const long long t = nowMs();
  /* ★ 加锁：`vuTick()` 会从 **UI 线程**回落表针，两边都动 sVuLin/sVuLastMs。
   *   以前这里是无锁的（只有音频线程碰），加了 vuTick 之后必须一起保护。 */
  pthread_mutex_lock(&sMu);
  int dt = sVuLastMs ? (int)(t - sVuLastMs) : 20;
  sVuLastMs = t;
  if (dt < 1) dt = 1;
  if (dt > 200) dt = 200;              // 卡顿/暂停恢复时别一步跳太大
  const float in[2] = {rmsL, rmsR};
  for (int ch = 0; ch < 2; ++ch) {
    const float tau = (in[ch] > sVuLin[ch]) ? kVuRiseMs : kVuFallMs;
    const float coef = 1.0f - expf(-(float)dt / tau);
    sVuLin[ch] += (in[ch] - sVuLin[ch]) * coef;
    sVuLevel[ch] = vuLinToLevel(sVuLin[ch]);
  }
  pthread_mutex_unlock(&sMu);
}

void Viz::vuTick() {
  const long long t = nowMs();
  pthread_mutex_lock(&sMu);
  if (!sVuLastMs || (t - sVuLastMs) < kVuHoldMs) {
    pthread_mutex_unlock(&sMu);        // 还在喂 / 刚喂过 ⇒ 交给音频侧，别插手
    return;
  }
  int dt = (int)(t - sVuLastMs);
  if (dt > 400) dt = 400;              // 页面定时器 100ms 一拍；封顶防跳变
  sVuLastMs = t;                       // 推进时钟：下一拍只衰减"新过去的那一段"
  for (int ch = 0; ch < 2; ++ch) {
    const float coef = 1.0f - expf(-(float)dt / kVuFallMs);
    sVuLin[ch] += (0.0f - sVuLin[ch]) * coef;
    if (sVuLin[ch] < 1e-7f) sVuLin[ch] = 0.0f;   // 落到底就钉住，别永远是个极小值
    sVuLevel[ch] = vuLinToLevel(sVuLin[ch]);
  }
  pthread_mutex_unlock(&sMu);
}

int Viz::vuLevel(int ch) {
  if (ch < 0 || ch > 1) return 0;
  return sVuLevel[ch];
}

float Viz::vuDb(int ch) {
  if (ch < 0 || ch > 1) return -40.0f;
  float lin = sVuLin[ch];
  if (lin < 1e-7f) lin = 1e-7f;
  return 20.0f * log10f(lin) - sVuZero;
}

void Viz::setVuZeroDbfs(float db) {
  if (db > 0.0f) db = 0.0f;
  if (db < -60.0f) db = -60.0f;
  sVuZero = db;
}
float Viz::vuZeroDbfs() { return sVuZero; }

void Viz::setGainDb(float db) {
  if (db > 0.0f) db = 0.0f;
  if (db < -100.0f) db = -100.0f;
  sGainDb = db;
}
float Viz::gainDb() { return sGainDb; }

}  // namespace pg
