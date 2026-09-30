#include "PgAudio.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <string>

#include "platform/PgViz.h"   // 音频可视化（在线流的频谱数据源，见 PgViz.h）
#include "utils/Log.h"

#ifdef FUN_BUILD
#include <dlfcn.h>

#include "manager/ConfigManager.h"
#endif

namespace pg {

namespace {

// 与 SfxId 一一对应（见 PgGame.h）
const char *kSfxFiles[] = {
    "",                    // SFX_NONE
    "audio/click.wav",     // SFX_CLICK
    "audio/move.wav",      // SFX_MOVE
    "audio/rotate.wav",    // SFX_ROTATE
    "audio/drop.wav",      // SFX_DROP
    "audio/clear.wav",     // SFX_CLEAR
    "audio/merge.wav",     // SFX_MERGE
    "audio/score.wav",     // SFX_SCORE
    "audio/hit.wav",       // SFX_HIT
    "audio/over.wav",      // SFX_OVER
    "audio/jump.wav",      // SFX_JUMP
    "audio/shoot.wav",     // SFX_SHOOT
    "audio/dice.wav",      // SFX_DICE
    /* 节奏钢琴 / 打鼓（与 core/PgGame.h 的枚举**同序**，追加在表尾）：
     * 这两款游戏要"按音高/鼓位播不同的声"，所以音色必须是**独立文件**
     * （PCM 没有变调播放的接口；而且变调会连带改时长 ⇒ 节奏会漂）。 */
    "audio/pno1.wav",      // SFX_PNO1  do
    "audio/pno2.wav",      // SFX_PNO2  re
    "audio/pno3.wav",      // SFX_PNO3  mi
    "audio/pno4.wav",      // SFX_PNO4  fa
    "audio/pno5.wav",      // SFX_PNO5  sol
    "audio/pno6.wav",      // SFX_PNO6  la
    "audio/pno7.wav",      // SFX_PNO7  si
    "audio/pno8.wav",      // SFX_PNO8  do'
    "audio/drm1.wav",      // SFX_DRM1  底鼓
    "audio/drm2.wav",      // SFX_DRM2  军鼓
    "audio/drm3.wav",      // SFX_DRM3  踩镲
    "audio/drm4.wav",      // SFX_DRM4  低嗵
    "audio/drm5.wav",      // SFX_DRM5  高嗵
    "audio/drm6.wav",      // SFX_DRM6  吊镲
};

// 输出流参数（与 resources/audio/*.wav 一致；不一致的音效预载时会转过来）
const int STREAM_RATE = 22050;
const int STREAM_CH = 1;
// 小周期 + 小缓冲 → 音效延迟低（160*4 = 640 帧 ≈ 29ms）。
// 配合下面 silence_size = 缓冲大小：即使偶发 xrun，内核自动补静音、流不停，也不会有 pop。
const int PERIOD_FRAMES = 160;
const int PERIOD_COUNT = 4;
// 同一音效的最小间隔（防连发把音轨打满；也避免逐帧触发的音效变成"哒哒哒"）
const long SFX_MIN_GAP_MS = 60;

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef FUN_BUILD

// ---------------- ALSA 稳定 ABI 的最小声明 ----------------
// 只用 dlopen 取符号，不引入链接期依赖（工具链不保证提供 asoundlib.h）。
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params snd_pcm_sw_params_t;
// 原始 control 接口（访问 codec 的输出开关；simple-mixer 那条路在本板查不到元素，见 ensureSpeakerOn 注释）
typedef struct _snd_ctl snd_ctl_t;
typedef struct _snd_ctl_elem_list snd_ctl_elem_list_t;
typedef struct _snd_ctl_elem_info snd_ctl_elem_info_t;
typedef struct _snd_ctl_elem_value snd_ctl_elem_value_t;
typedef struct _snd_ctl_elem_id snd_ctl_elem_id_t;
const int PG_CTL_ELEM_TYPE_BOOLEAN = 1;
// snd_ctl_elem_type_t: 1=BOOLEAN 2=INTEGER 3=ENUMERATED 4=BYTES ...
const int PG_CTL_ELEM_TYPE_INTEGER = 2;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;

enum { SND_PCM_STREAM_PLAYBACK = 0 };
enum { SND_PCM_FORMAT_S16_LE = 2 };  // pcm.h: UNKNOWN=-1, S8=0, U8=1, S16_LE=2
enum { SND_PCM_ACCESS_RW_INTERLEAVED = 3 };

struct AlsaApi {
  void *lib;
  int (*pcm_open)(snd_pcm_t **, const char *, int, int);
  int (*pcm_close)(snd_pcm_t *);
  int (*pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
  int (*pcm_recover)(snd_pcm_t *, int, int);
  int (*pcm_drop)(snd_pcm_t *);
  int (*pcm_prepare)(snd_pcm_t *);
  int (*hw_malloc)(snd_pcm_hw_params_t **);
  void (*hw_free)(snd_pcm_hw_params_t *);
  int (*hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
  // 注意：这三个的**第一个参数是 pcm**（不是 params），漏了会变成野指针直接崩
  int (*hw_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
  int (*hw_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
  int (*hw_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
  int (*hw_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
  int (*hw_set_period_size_near)(snd_pcm_t *, snd_pcm_hw_params_t *,
                                 snd_pcm_uframes_t *, int *);
  int (*hw_set_periods_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *,
                             int *);
  int (*hw_get_buffer_size)(const snd_pcm_hw_params_t *, snd_pcm_uframes_t *);
  int (*hw)(snd_pcm_t *, snd_pcm_hw_params_t *);
  int (*sw_malloc)(snd_pcm_sw_params_t **);
  void (*sw_free)(snd_pcm_sw_params_t *);
  int (*sw_current)(snd_pcm_t *, snd_pcm_sw_params_t *);
  int (*sw_set_start_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *,
                                snd_pcm_uframes_t);
  int (*sw_set_silence_threshold)(snd_pcm_t *, snd_pcm_sw_params_t *,
                                  snd_pcm_uframes_t);
  int (*sw_set_silence_size)(snd_pcm_t *, snd_pcm_sw_params_t *,
                             snd_pcm_uframes_t);
  int (*sw)(snd_pcm_t *, snd_pcm_sw_params_t *);
  const char *(*strerror)(int);
  // 原始 control 接口：用来把 codec 的输出开关（SPK/LINEOUT Switch）打开，
  // 这是本工程唯一"必须动混音器"的地方（详见 ensureSpeakerOn 的注释）。
  int (*ctl_open)(snd_ctl_t **, const char *, int);
  int (*ctl_close)(snd_ctl_t *);
  int (*ctl_elem_list_malloc)(snd_ctl_elem_list_t **);
  void (*ctl_elem_list_free)(snd_ctl_elem_list_t *);
  int (*ctl_elem_list)(snd_ctl_t *, snd_ctl_elem_list_t *);
  unsigned int (*ctl_elem_list_get_count)(const snd_ctl_elem_list_t *);
  int (*ctl_elem_list_alloc_space)(snd_ctl_elem_list_t *, unsigned int);
  int (*ctl_elem_list_get_id)(const snd_ctl_elem_list_t *, unsigned int,
                              snd_ctl_elem_id_t *);
  int (*ctl_elem_id_malloc)(snd_ctl_elem_id_t **);
  void (*ctl_elem_id_free)(snd_ctl_elem_id_t *);
  const char *(*ctl_elem_id_get_name)(const snd_ctl_elem_id_t *);
  int (*ctl_elem_info_malloc)(snd_ctl_elem_info_t **);
  void (*ctl_elem_info_free)(snd_ctl_elem_info_t *);
  int (*ctl_elem_info_set_id)(snd_ctl_elem_info_t *, const snd_ctl_elem_id_t *);
  int (*ctl_elem_info)(snd_ctl_t *, snd_ctl_elem_info_t *);
  int (*ctl_elem_info_get_type)(const snd_ctl_elem_info_t *);
  // 音量用（INT 控件：读 min/max/step + 读改写整数）
  int (*ctl_elem_info_get_min)(const snd_ctl_elem_info_t *);
  int (*ctl_elem_info_get_max)(const snd_ctl_elem_info_t *);
  int (*ctl_elem_info_get_step)(const snd_ctl_elem_info_t *);
  void (*ctl_elem_value_set_integer)(snd_ctl_elem_value_t *, unsigned int, long);
  long (*ctl_elem_value_get_integer)(const snd_ctl_elem_value_t *, unsigned int);
  int (*ctl_elem_id_set_numid)(snd_ctl_elem_id_t *, unsigned int);
  unsigned int (*ctl_elem_id_get_numid)(const snd_ctl_elem_id_t *);
  int (*ctl_elem_value_malloc)(snd_ctl_elem_value_t **);
  void (*ctl_elem_value_free)(snd_ctl_elem_value_t *);
  int (*ctl_elem_value_set_id)(snd_ctl_elem_value_t *, const snd_ctl_elem_id_t *);
  void (*ctl_elem_value_set_boolean)(snd_ctl_elem_value_t *, unsigned int, long);
  int (*ctl_elem_value_get_boolean)(const snd_ctl_elem_value_t *, unsigned int);
  int (*ctl_elem_write)(snd_ctl_t *, snd_ctl_elem_value_t *);
  int (*ctl_elem_read)(snd_ctl_t *, snd_ctl_elem_value_t *);

  bool load() {
    if (lib) return true;
    lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) {
      LOGE("PgAudio: dlopen libasound.so.2 失败: %s", dlerror());
      return false;
    }
#define PG_SYM(field, name)                \
  *(void **)(&field) = dlsym(lib, name);   \
  if (!field) LOGW("PgAudio: 缺少符号 %s", name);
    PG_SYM(pcm_open, "snd_pcm_open");
    PG_SYM(pcm_close, "snd_pcm_close");
    PG_SYM(pcm_writei, "snd_pcm_writei");
    PG_SYM(pcm_recover, "snd_pcm_recover");
    PG_SYM(pcm_drop, "snd_pcm_drop");
    PG_SYM(pcm_prepare, "snd_pcm_prepare");
    PG_SYM(hw_malloc, "snd_pcm_hw_params_malloc");
    PG_SYM(hw_free, "snd_pcm_hw_params_free");
    PG_SYM(hw_any, "snd_pcm_hw_params_any");
    PG_SYM(hw_set_access, "snd_pcm_hw_params_set_access");
    PG_SYM(hw_set_format, "snd_pcm_hw_params_set_format");
    PG_SYM(hw_set_channels, "snd_pcm_hw_params_set_channels");
    PG_SYM(hw_set_rate_near, "snd_pcm_hw_params_set_rate_near");
    PG_SYM(hw_set_period_size_near, "snd_pcm_hw_params_set_period_size_near");
    PG_SYM(hw_set_periods_near, "snd_pcm_hw_params_set_periods_near");
    PG_SYM(hw_get_buffer_size, "snd_pcm_hw_params_get_buffer_size");
    PG_SYM(hw, "snd_pcm_hw_params");
    PG_SYM(sw_malloc, "snd_pcm_sw_params_malloc");
    PG_SYM(sw_free, "snd_pcm_sw_params_free");
    PG_SYM(sw_current, "snd_pcm_sw_params_current");
    PG_SYM(sw_set_start_threshold, "snd_pcm_sw_params_set_start_threshold");
    PG_SYM(sw_set_silence_threshold, "snd_pcm_sw_params_set_silence_threshold");
    PG_SYM(sw_set_silence_size, "snd_pcm_sw_params_set_silence_size");
    PG_SYM(sw, "snd_pcm_sw_params");
    PG_SYM(strerror, "snd_strerror");
    PG_SYM(ctl_open, "snd_ctl_open");
    PG_SYM(ctl_close, "snd_ctl_close");
    PG_SYM(ctl_elem_list_malloc, "snd_ctl_elem_list_malloc");
    PG_SYM(ctl_elem_list_free, "snd_ctl_elem_list_free");
    PG_SYM(ctl_elem_list, "snd_ctl_elem_list");
    PG_SYM(ctl_elem_list_get_count, "snd_ctl_elem_list_get_count");
    PG_SYM(ctl_elem_list_alloc_space, "snd_ctl_elem_list_alloc_space");
    PG_SYM(ctl_elem_list_get_id, "snd_ctl_elem_list_get_id");
    PG_SYM(ctl_elem_id_malloc, "snd_ctl_elem_id_malloc");
    PG_SYM(ctl_elem_id_free, "snd_ctl_elem_id_free");
    PG_SYM(ctl_elem_id_get_name, "snd_ctl_elem_id_get_name");
    PG_SYM(ctl_elem_info_malloc, "snd_ctl_elem_info_malloc");
    PG_SYM(ctl_elem_info_free, "snd_ctl_elem_info_free");
    PG_SYM(ctl_elem_info_set_id, "snd_ctl_elem_info_set_id");
    PG_SYM(ctl_elem_info, "snd_ctl_elem_info");
    PG_SYM(ctl_elem_info_get_type, "snd_ctl_elem_info_get_type");
    PG_SYM(ctl_elem_info_get_min, "snd_ctl_elem_info_get_min");
    PG_SYM(ctl_elem_info_get_max, "snd_ctl_elem_info_get_max");
    PG_SYM(ctl_elem_info_get_step, "snd_ctl_elem_info_get_step");
    PG_SYM(ctl_elem_value_set_integer, "snd_ctl_elem_value_set_integer");
    PG_SYM(ctl_elem_value_get_integer, "snd_ctl_elem_value_get_integer");
    PG_SYM(ctl_elem_id_set_numid, "snd_ctl_elem_id_set_numid");
    PG_SYM(ctl_elem_id_get_numid, "snd_ctl_elem_id_get_numid");
    PG_SYM(ctl_elem_value_malloc, "snd_ctl_elem_value_malloc");
    PG_SYM(ctl_elem_value_free, "snd_ctl_elem_value_free");
    PG_SYM(ctl_elem_value_set_id, "snd_ctl_elem_value_set_id");
    PG_SYM(ctl_elem_value_set_boolean, "snd_ctl_elem_value_set_boolean");
    PG_SYM(ctl_elem_value_get_boolean, "snd_ctl_elem_value_get_boolean");
    PG_SYM(ctl_elem_write, "snd_ctl_elem_write");
    PG_SYM(ctl_elem_read, "snd_ctl_elem_read");
#undef PG_SYM
    return pcm_open && pcm_writei && hw_any && hw && sw_current && sw;
  }
};

// ---------------- 预载音效（统一转成流参数） ----------------
struct Clip {
  uint8_t *data;  // 交错 S16 数据
  uint32_t size;  // 字节
  Clip() : data(0), size(0) {}
};

uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/*
 * 读 WAV（PCM 8/16bit，单/双声道，任意采样率）→ 转成流参数（22050/单声道/S16）。
 * 本工程的音效本来就是 22050/单声道/16bit，这条转换路径平时是直通。
 */
bool loadClip(const char *path, Clip *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint8_t hdr[12];
  if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0) {
    fclose(f);
    return false;
  }
  int channels = 0, bits = 0, rate = 0, audioFmt = 0;
  long dataOff = -1;
  uint32_t dataLen = 0;
  for (;;) {
    uint8_t ck[8];
    if (fread(ck, 1, 8, f) != 8) break;
    uint32_t sz = rd32(ck + 4);
    if (memcmp(ck, "fmt ", 4) == 0) {
      uint8_t b[16];
      uint32_t need = sz < 16 ? sz : 16;
      if (fread(b, 1, need, f) != need) break;
      audioFmt = b[0] | (b[1] << 8);
      channels = b[2] | (b[3] << 8);
      rate = (int)rd32(b + 4);
      bits = b[14] | (b[15] << 8);
      if (sz > need) fseek(f, (long)(sz - need), SEEK_CUR);
    } else if (memcmp(ck, "data", 4) == 0) {
      dataOff = ftell(f);
      dataLen = sz;
      fseek(f, (long)sz, SEEK_CUR);
    } else {
      fseek(f, (long)sz, SEEK_CUR);
    }
  }
  if (audioFmt != 1 || channels < 1 || channels > 2 || rate <= 0 ||
      (bits != 8 && bits != 16) || dataOff < 0 || dataLen == 0) {
    LOGW("PgAudio: %s 不是可用的 PCM WAV(格式%d %dch %dbit)", path, audioFmt,
         channels, bits);
    fclose(f);
    return false;
  }
  uint8_t *raw = (uint8_t *)malloc(dataLen);
  if (!raw) {
    fclose(f);
    return false;
  }
  fseek(f, dataOff, SEEK_SET);
  if (fread(raw, 1, dataLen, f) != dataLen) {
    free(raw);
    fclose(f);
    return false;
  }
  fclose(f);

  int bytesPerSample = bits / 8;
  uint32_t frames = dataLen / (uint32_t)(bytesPerSample * channels);
  if (frames == 0) {
    free(raw);
    return false;
  }
  uint32_t outFrames =
      (uint32_t)((uint64_t)frames * (uint32_t)STREAM_RATE / (uint32_t)rate);
  if (outFrames == 0) outFrames = 1;
  int16_t *dst = (int16_t *)malloc((size_t)outFrames * 2);
  if (!dst) {
    free(raw);
    return false;
  }
  for (uint32_t i = 0; i < outFrames; ++i) {
    uint32_t src =
        (uint32_t)((uint64_t)i * (uint32_t)rate / (uint32_t)STREAM_RATE);
    if (src >= frames) src = frames - 1;
    int v;
    if (bits == 16) {
      const int16_t *p = (const int16_t *)raw;
      v = (channels == 1) ? p[src] : ((int)p[src * channels] +
                                      (int)p[src * channels + 1]) / 2;
    } else {
      const uint8_t *p = raw;
      int s = (channels == 1)
                  ? (int)p[src]
                  : ((int)p[src * channels] + (int)p[src * channels + 1]) / 2;
      v = (s - 128) << 8;
    }
    dst[i] = (int16_t)v;
  }
  free(raw);
  out->data = (uint8_t *)dst;
  out->size = outFrames * 2;
  return true;
}

// ---------------- 运行时状态 ----------------
struct Impl {
  AlsaApi a;
  snd_pcm_t *pcm;
  pthread_t th;
  pthread_mutex_t mu;
  bool run;      // 音频线程续跑标志
  bool started;  // 线程已创建
  bool enabled;  // 音效开关
  int cur;       // 正在播放的 clip（-1 = 空闲 → 写静音）
  uint32_t pos;  // 当前 clip 播放位置（字节）
  Clip clips[SFX_COUNT];
  int lastSfx;       // 上一次触发的音效（连发抑制）
  long lastMs;
  uint64_t writes;   // 已写块数（日志/自检）
  bool loggedGap;    // 只报一次"写失败"
  bool loggedOff;    // 只报一次"音效关闭，忽略请求"

  /* ---- 在线流音频队列（PgStream 灌进来的 PCM：22050/单声道/S16）----
   * 环形缓冲 400ms。满了就让调用方少收 → 天然把网络读取节流到实时速度。 */
  uint8_t *sbuf;
  uint32_t scap, shead, stail;
  bool son;

  Impl()
      : pcm(0),
        run(false),
        started(false),
        enabled(true),
        cur(-1),
        pos(0),
        lastSfx(0),
        lastMs(0),
        writes(0),
        loggedGap(false),
        loggedOff(false),
        sbuf(0),
        scap(0),
        shead(0),
        stail(0),
        son(false) {
    memset(&a, 0, sizeof(a));
    pthread_mutex_init(&mu, 0);
  }
};

/* 环形缓冲已用字节数（调用方需持有 im->mu） */
static uint32_t ringUsed(Impl *im) {
  return (im->shead + im->scap - im->stail) % im->scap;
}

void writeBlock(Impl *im, const uint8_t *buf) {
  snd_pcm_uframes_t left = (snd_pcm_uframes_t)PERIOD_FRAMES;
  const uint8_t *p = buf;
  const int frameBytes = STREAM_CH * 2;
  int guard = 0;
  while (left > 0 && im->run) {
    snd_pcm_sframes_t w = im->a.pcm_writei(im->pcm, p, left);
    if (w < 0) {
      if (w == -11 /*-EAGAIN*/) continue;
      if (im->a.pcm_recover && im->a.pcm_recover(im->pcm, (int)w, 1) >= 0) {
        continue;
      }
      /* ⚠️ 判据要带 `im->run`：`releasePcm()` 的顺序是 run=false → pcm_drop() → join，
       *   所以 pcm_drop 之后 writei 必然返回错误（-EBADFD "File descriptor in bad state"）——
       *   那是**正常退出路径**，不是故障。以前无条件报 WARN，日志里看起来像出了事
       *   （2026-09-15 排查"声音没了"时就被它误导过一轮）。 */
      if (!im->loggedGap && im->run) {
        im->loggedGap = true;
        LOGW("PgAudio: 写声卡失败: %s", im->a.strerror ? im->a.strerror((int)w)
                                                      : "?");
      }
      return;
    }
    left -= (snd_pcm_uframes_t)w;
    p += (size_t)w * frameBytes;
    if (++guard > 64) return;
  }
  ++im->writes;
}

/*
 * 音频线程：**一直**往里写数据 —— 有音效写音效，没音效写静音。
 * 这样 PCM 流永远处于 RUNNING，DAC 不会反复开关，也就没有 pop。
 * （写是阻塞的：缓冲满了 writei 自己会等，所以这里不需要 sleep 来节流。）
 */
void *audioThread(void *arg) {
  Impl *im = (Impl *)arg;
  const int blockBytes = PERIOD_FRAMES * STREAM_CH * 2;
  uint8_t *buf = (uint8_t *)malloc((size_t)blockBytes);
  if (!buf) {
    LOGE("PgAudio: 音频线程缓冲分配失败");
    return 0;
  }
  LOGD("PgAudio: 音频线程启动（常开流 + 空闲写静音）");
  while (im->run) {
    int got = 0;
    pthread_mutex_lock(&im->mu);
    /* 在线流优先：有数据就从环形缓冲取一块（不够一块就有多少取多少，剩下补静音 ——
     * 宁可静一下，也不让声卡欠载出咔哒声）。 */
    if (im->son && im->shead != im->stail) {
      uint32_t used = ringUsed(im);
      uint32_t n = ((uint32_t)blockBytes < used) ? (uint32_t)blockBytes : used;
      uint32_t first = im->scap - im->stail;
      if (first > n) first = n;
      memcpy(buf, im->sbuf + im->stail, first);
      if (n > first) memcpy(buf + first, im->sbuf, n - first);
      im->stail = (im->stail + n) % im->scap;
      got = (int)n;
    } else if (im->enabled && im->cur >= 0 && im->cur < SFX_COUNT) {
      Clip &c = im->clips[im->cur];
      if (c.data && im->pos < c.size) {
        uint32_t left = c.size - im->pos;
        uint32_t n = (uint32_t)blockBytes < left ? (uint32_t)blockBytes : left;
        memcpy(buf, c.data + im->pos, n);
        im->pos += n;
        got = (int)n;
      }
      if (!c.data || im->pos >= c.size) im->cur = -1;
    }
    pthread_mutex_unlock(&im->mu);
    // 空闲/尾部补静音：这是"没有声音的时候写静音数据"的地方
    if (got < blockBytes) memset(buf + got, 0, (size_t)(blockBytes - got));
    writeBlock(im, buf);
    /* 自检：每 500 块（≈3.6s 音频）打一条，用来判断声卡到底以多快在消费。
     * （排查"在线流慢"时，就是靠它区分"声卡消费慢" vs "网络喂得慢"。） */
    if ((im->writes % 500) == 0) {
      pthread_mutex_lock(&im->mu);
      uint32_t used = im->sbuf ? ringUsed(im) : 0;
      pthread_mutex_unlock(&im->mu);
      LOGD("PgAudio: 音频线程已写 %llu 块（%llu ms 音频）/ 队列剩 %u 帧",
           (unsigned long long)im->writes,
           (unsigned long long)(im->writes * PERIOD_FRAMES * 1000 / STREAM_RATE),
           (unsigned)(used / (STREAM_CH * 2)));
    }
  }
  free(buf);
  LOGD("PgAudio: 音频线程退出（共写 %llu 块）", (unsigned long long)im->writes);
  return 0;
}

/*
 * ⚠️ 喇叭输出开关（本工程最坑的一处"完全无声"来源，改这段前请读完）
 *
 * 2026-09-12 实测：应用侧一切"看起来正常" —— PCM 打开成功、参数协商成功、音频线程
 * 持续写数据、无 xrun、无写失败、CPU 正常 —— 但**一点声音都没有**。真机 `tinymix -D 0`
 * 显示 `SPK Switch` / `LINEOUT Switch` 都是 **Off**（codec 输出被静音）。
 *
 * 老代码用 snd_mixer 的 simple-element 按名字查 "SPK Switch"：simple-mixer 会把
 * " Switch" 后缀当角色拆掉（元素名其实是 "SPK"），所以**永远查不到** —— 老代码只打一条
 * WARN 就返回了，应用"以为"开关已经是 On，于是每次排查都白跑（这条静音持续了一整天）。
 *
 * 现在直接走**原始 control 接口**（与 `tinymix <numid> <val>` 同一条路，本板实测有效）：
 * 枚举全部控件 → 挑出「名字含 SPK / LINEOUT 且类型为 BOOL」的 → 置 1 → **回读校验**并打日志。
 * 任何一步失败都留明确的 WARN，不再静默通过。
 *
 * 另注：本板 `tinymix` **只认控件序号、不认名字**（`tinymix 'SPK Switch' 1` 返回 0 但不生效），
 * 手工排查请用 `tinymix 20 1`（20 = SPK Switch 的序号，19 = LINEOUT Switch）。
 */
bool writeBoolVal(Impl *im, snd_ctl_t *ctl, snd_ctl_elem_id_t *id,
                  const char *name, bool on) {
  AlsaApi &a = im->a;
  snd_ctl_elem_value_t *v = 0;
  if (!a.ctl_elem_value_malloc || a.ctl_elem_value_malloc(&v) < 0 || !v) {
    LOGW("PgAudio: ctl_elem_value 分配失败");
    return false;
  }
  a.ctl_elem_value_set_id(v, id);
  a.ctl_elem_value_set_boolean(v, 0, on ? 1 : 0);
  int err = a.ctl_elem_write(ctl, v);
  if (err < 0) {
    LOGW("PgAudio: 设置 %s = %s 失败: %s", name, on ? "On" : "Off",
         a.strerror ? a.strerror(err) : "?");
    if (a.ctl_elem_value_free) a.ctl_elem_value_free(v);
    return false;
  }
  // 回读校验：写成功不等于写生效（本板 tinymix 按名字写就是这个坑）
  int after = -1;
  if (a.ctl_elem_read && a.ctl_elem_read(ctl, v) >= 0 &&
      a.ctl_elem_value_get_boolean)
    after = a.ctl_elem_value_get_boolean(v, 0);
  if (a.ctl_elem_value_free) a.ctl_elem_value_free(v);
  LOGD("PgAudio: 输出开关 %s -> %s（回读 %d）", name, on ? "On" : "Off", after);
  return after == (on ? 1 : 0);
}

void ensureSpeakerOn(Impl *im) {
  AlsaApi &a = im->a;
  if (!a.ctl_open || !a.ctl_elem_list_malloc || !a.ctl_elem_info_malloc ||
      !a.ctl_elem_value_malloc) {
    LOGW("PgAudio: libasound 缺 ctl 符号，跳过输出开关设置（可能无声）");
    return;
  }
  snd_ctl_t *ctl = 0;
  int err = a.ctl_open(&ctl, "hw:0", 0);
  if (err < 0 || !ctl) {
    LOGW("PgAudio: snd_ctl_open(hw:0) 失败: %s",
         a.strerror ? a.strerror(err) : "?");
    return;
  }
  snd_ctl_elem_list_t *list = 0;
  if (a.ctl_elem_list_malloc(&list) < 0 || !list) {
    a.ctl_close(ctl);
    return;
  }
  a.ctl_elem_list(ctl, list);
  unsigned int count = a.ctl_elem_list_get_count(list);
  if (count > 0 && a.ctl_elem_list_alloc_space(list, count) >= 0)
    a.ctl_elem_list(ctl, list);

  snd_ctl_elem_info_t *info = 0;
  snd_ctl_elem_id_t *id = 0;
  a.ctl_elem_info_malloc(&info);
  a.ctl_elem_id_malloc(&id);
  int found = 0, ok = 0;
  for (unsigned int i = 0; i < count && info && id; ++i) {
    if (a.ctl_elem_list_get_id(list, i, id) < 0) continue;
    const char *nm = a.ctl_elem_id_get_name(id);
    if (!nm) continue;
    // 只要输出那两路（SPK / LINEOUT），别去动 MIC/LINEIN 输入路由
    if (!strstr(nm, "SPK") && !strstr(nm, "LINEOUT")) continue;
    if (!strstr(nm, "Switch")) continue;
    a.ctl_elem_info_set_id(info, id);
    if (a.ctl_elem_info(ctl, info) < 0) continue;
    if (a.ctl_elem_info_get_type(info) != PG_CTL_ELEM_TYPE_BOOLEAN) continue;
    ++found;
    if (writeBoolVal(im, ctl, id, nm, true)) ++ok;
  }
  if (info) a.ctl_elem_info_free(info);
  if (id) a.ctl_elem_id_free(id);
  a.ctl_elem_list_free(list);
  a.ctl_close(ctl);

  // 注意：LOGW/LOGD 是宏（展开后带尾分号），这里必须加花括号，否则 else 会配不上
  if (found == 0) {
    LOGW("PgAudio: 没找到 SPK/LINEOUT 输出开关（共 %u 个控件）—— 若无声请查 tinymix",
         count);
  } else {
    LOGD("PgAudio: 输出开关检查完成：找到 %d 个，成功打开 %d 个", found, ok);
  }
}

bool openStream(Impl *im) {
  AlsaApi &a = im->a;
  const char *dev = "hw:0,0";  // card0 = 片内 codec = 板载喇叭
  int err = a.pcm_open(&im->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0 /*blocking*/);
  if (err < 0 || !im->pcm) {
    LOGE("PgAudio: 打开 %s 失败: %s", dev,
         a.strerror ? a.strerror(err) : "?");
    im->pcm = 0;
    return false;
  }
  snd_pcm_hw_params_t *hw = 0;
  if (a.hw_malloc(&hw) < 0 || !hw) {
    LOGE("PgAudio: hw_params 分配失败");
    a.pcm_close(im->pcm);
    im->pcm = 0;
    return false;
  }
  err = a.hw_any(im->pcm, hw);
  if (err < 0) {
    LOGE("PgAudio: hw_params_any 失败: %s", a.strerror ? a.strerror(err) : "?");
    a.hw_free(hw);
    a.pcm_close(im->pcm);
    im->pcm = 0;
    return false;
  }
  unsigned rate = (unsigned)STREAM_RATE;
  snd_pcm_uframes_t period = (snd_pcm_uframes_t)PERIOD_FRAMES;
  unsigned periods = (unsigned)PERIOD_COUNT;
  a.hw_set_access(im->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  a.hw_set_format(im->pcm, hw, SND_PCM_FORMAT_S16_LE);
  a.hw_set_channels(im->pcm, hw, (unsigned)STREAM_CH);
  a.hw_set_rate_near(im->pcm, hw, &rate, 0);
  a.hw_set_period_size_near(im->pcm, hw, &period, 0);
  a.hw_set_periods_near(im->pcm, hw, &periods, 0);
  err = a.hw(im->pcm, hw);
  snd_pcm_uframes_t bufFrames = 0;
  if (a.hw_get_buffer_size) a.hw_get_buffer_size(hw, &bufFrames);
  a.hw_free(hw);
  if (err < 0) {
    LOGE("PgAudio: hw_params(%dHz/%dch) 失败: %s", STREAM_RATE, STREAM_CH,
         a.strerror ? a.strerror(err) : "?");
    a.pcm_close(im->pcm);
    im->pcm = 0;
    return false;
  }

  snd_pcm_sw_params_t *sw = 0;
  a.sw_malloc(&sw);
  a.sw_current(im->pcm, sw);
  // 写入第一帧就起播（默认阈值≈半个缓冲，短音效会躺在缓冲里不起播）
  a.sw_set_start_threshold(im->pcm, sw, 1);
  // xrun 时由内核自动补静音、流不停 → 不会因为偶发欠载重新起播而 pop
  if (a.sw_set_silence_threshold && a.sw_set_silence_size && bufFrames > 0) {
    a.sw_set_silence_threshold(im->pcm, sw, bufFrames);
    a.sw_set_silence_size(im->pcm, sw, bufFrames);
  }
  err = a.sw(im->pcm, sw);
  a.sw_free(sw);
  if (err < 0) LOGW("PgAudio: sw_params 返回 %d（继续）", err);

  LOGD("PgAudio: 常开流就绪 %s %dHz/%dch S16，周期 %lu 帧 x %u，缓冲 %lu 帧(%.0fms)",
       dev, STREAM_RATE, STREAM_CH, (unsigned long)period, periods,
       (unsigned long)bufFrames,
       bufFrames * 1000.0 / (double)STREAM_RATE);
  return true;
}

#endif  // FUN_BUILD

}  // namespace

/* ==================== 音量（codec 数字音量控件） ====================
 * 为什么用硬件控件而不是软件增益：本板 codec 自带数字音量（INT 类型），让硬件做不占 CPU、
 * 也不会因为乘系数而削顶失真。
 *
 * 控件怎么选：按优先级取第一个存在的 —— `digital volume` > `DAC volume` > `LINEOUT volume`
 * （名字来自 `tinymix -D 0` 实测）；只要 INTEGER 类型且 max>min。
 *
 * ⚠️ 与输出开关同一个坑：本板 `tinymix` 只认控件**序号**不认名字，所以这里
 *    **用 numid 定位控件**（名字只用来挑"是哪一个"），不靠名字去查值。
 *
 * 无状态设计：每次调用都走一遍「枚举找控件 → 读当前 → ±步进 → 写 → 回读校验」。
 * 按键触发频率低（人手按），换来的是不怕被外部（tinymix/其它进程）改过之后算错。
 */
struct VolInfo {
  unsigned int numid;
  long min, max, step, cur;
  // ⚠️ 必须复制字符串：snd_ctl_elem_id_get_name() 返回的是 id 内部缓冲的指针，
  // id 一 free 就失效（第一版直接用指针 → 日志里控件名打印为空）。
  char name[64];
};

bool probeVolume(Impl *im, snd_ctl_t *ctl, VolInfo *out) {
  AlsaApi &a = im->a;
  if (!a.ctl_elem_list_malloc || !a.ctl_elem_info_malloc || !a.ctl_elem_id_malloc ||
      !a.ctl_elem_value_malloc || !a.ctl_elem_id_set_numid)
    return false;
  snd_ctl_elem_list_t *list = 0;
  if (a.ctl_elem_list_malloc(&list) < 0 || !list) return false;
  a.ctl_elem_list(ctl, list);
  unsigned int count = a.ctl_elem_list_get_count(list);
  if (count > 0 && a.ctl_elem_list_alloc_space(list, count) >= 0)
    a.ctl_elem_list(ctl, list);

  snd_ctl_elem_info_t *info = 0;
  snd_ctl_elem_id_t *id = 0;
  a.ctl_elem_info_malloc(&info);
  a.ctl_elem_id_malloc(&id);
  memset(out, 0, sizeof(*out));
  int best = 99;
  for (unsigned int i = 0; i < count && info && id; ++i) {
    if (a.ctl_elem_list_get_id(list, i, id) < 0) continue;
    const char *nm = a.ctl_elem_id_get_name(id);
    if (!nm) continue;
    int pri = 9;
    if (strstr(nm, "digital volume")) {
      pri = 0;
    } else if (strstr(nm, "DAC volume")) {
      pri = 1;
    } else if (strstr(nm, "LINEOUT volume")) {
      pri = 2;
    } else {
      continue;
    }
    if (pri >= best) continue;
    a.ctl_elem_info_set_id(info, id);
    if (a.ctl_elem_info(ctl, info) < 0) continue;
    if (a.ctl_elem_info_get_type(info) != PG_CTL_ELEM_TYPE_INTEGER) continue;
    long vmin = a.ctl_elem_info_get_min ? a.ctl_elem_info_get_min(info) : 0;
    long vmax = a.ctl_elem_info_get_max ? a.ctl_elem_info_get_max(info) : 0;
    if (vmax <= vmin) continue;
    best = pri;
    out->numid = a.ctl_elem_id_get_numid ? a.ctl_elem_id_get_numid(id) : 0;
    out->min = vmin;
    out->max = vmax;
    out->step = a.ctl_elem_info_get_step ? a.ctl_elem_info_get_step(info) : 1;
    snprintf(out->name, sizeof(out->name), "%s", nm);
  }
  if (info) a.ctl_elem_info_free(info);
  if (id) a.ctl_elem_id_free(id);
  a.ctl_elem_list_free(list);
  return best != 99 && out->numid != 0;
}

long readVolume(Impl *im, snd_ctl_t *ctl, const VolInfo *vi) {
  AlsaApi &a = im->a;
  snd_ctl_elem_id_t *id = 0;
  snd_ctl_elem_value_t *v = 0;
  long ret = -1;
  if (a.ctl_elem_id_malloc(&id) == 0 && id && a.ctl_elem_value_malloc(&v) == 0 && v) {
    a.ctl_elem_id_set_numid(id, vi->numid);
    a.ctl_elem_value_set_id(v, id);
    if (a.ctl_elem_read(ctl, v) >= 0 && a.ctl_elem_value_get_integer)
      ret = a.ctl_elem_value_get_integer(v, 0);
  }
  if (id) a.ctl_elem_id_free(id);
  if (v) a.ctl_elem_value_free(v);
  return ret;
}

bool writeVolume(Impl *im, snd_ctl_t *ctl, const VolInfo *vi, long val) {
  AlsaApi &a = im->a;
  snd_ctl_elem_id_t *id = 0;
  snd_ctl_elem_value_t *v = 0;
  bool ok = false;
  if (a.ctl_elem_id_malloc(&id) == 0 && id && a.ctl_elem_value_malloc(&v) == 0 && v) {
    a.ctl_elem_id_set_numid(id, vi->numid);
    a.ctl_elem_value_set_id(v, id);
    a.ctl_elem_value_set_integer(v, 0, val);
    if (a.ctl_elem_write(ctl, v) >= 0) ok = true;
  }
  if (id) a.ctl_elem_id_free(id);
  if (v) a.ctl_elem_value_free(v);
  return ok;
}

int DeviceAudio::volumeStep(int delta) {
#ifdef FUN_BUILD
  Impl *im = (Impl *)impl_;
  if (!im || !im->a.ctl_open) return -1;
  snd_ctl_t *ctl = 0;
  if (im->a.ctl_open(&ctl, "hw:0", 0) < 0 || !ctl) return -1;

  VolInfo vi;
  if (!probeVolume(im, ctl, &vi)) {
    LOGW("PgAudio: 找不到数字音量控件（用 tinymix -D 0 看有哪些 INT 控件）");
    im->a.ctl_close(ctl);
    return -1;
  }
  long cur = readVolume(im, ctl, &vi);
  if (cur < 0) cur = (vi.min + vi.max) / 2;

  // 步进取「16 档均分」，比控件自带的 step 好用（本板 step=1，一次只动 1 个刻度太小）；
  // 但控件若要求更大步长则以控件为准。
  long step = (vi.max - vi.min) / 16;
  if (vi.step > 1 && vi.step > step) step = vi.step;
  if (step < 1) step = 1;

  long next = cur + (long)delta * step;
  if (next < vi.min) next = vi.min;
  if (next > vi.max) next = vi.max;
  bool wrote = true;
  if (next != cur) wrote = writeVolume(im, ctl, &vi, next);
  long back = wrote ? readVolume(im, ctl, &vi) : cur;   // 写后回读校验
  if (back < 0) back = next;
  int pct = (int)((back - vi.min) * 100 / (vi.max - vi.min));
  if (!wrote) {
    LOGW("PgAudio: 音量写入失败（%s: %ld -> %ld）", vi.name, cur, next);
  } else {
    LOGD("PgAudio: 音量 %s %ld -> %ld（范围 %ld..%ld，步进 %ld）= %d%%", vi.name,
         cur, back, vi.min, vi.max, step, pct);
  }
  im->a.ctl_close(ctl);
  return wrote ? pct : -1;
#else
  (void)delta;
  return -1;
#endif
}

int DeviceAudio::setVolumePercent(int pct) {
#ifdef FUN_BUILD
  Impl *im = (Impl *)impl_;
  if (!im || !im->a.ctl_open) return -1;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  snd_ctl_t *ctl = 0;
  if (im->a.ctl_open(&ctl, "hw:0", 0) < 0 || !ctl) return -1;
  VolInfo vi;
  if (!probeVolume(im, ctl, &vi)) {
    im->a.ctl_close(ctl);
    return -1;
  }
  long target = vi.min + (vi.max - vi.min) * (long)pct / 100;
  bool ok = writeVolume(im, ctl, &vi, target);
  long back = ok ? readVolume(im, ctl, &vi) : -1;
  if (back < 0) back = target;
  int got = (int)((back - vi.min) * 100 / (vi.max - vi.min));
  LOGD("PgAudio: 启动恢复音量 %s -> %ld（%d%%）", vi.name, back, got);
  im->a.ctl_close(ctl);
  return ok ? got : -1;
#else
  (void)pct;
  return -1;
#endif
}

/* ---------------- 静音：写 codec 的输出开关（SPK / LINEOUT Switch） ----------------
 * 与 ensureSpeakerOn() 是**同一组控件**，只是写入值相反（它恒写 On）。
 * 遍历方式照抄它：本板 `tinymix` 只认控件**序号**不认名字，所以"按名字挑、按 id 写"。
 *
 * ⚠️ 真静音用开关，不用"把音量调 0"：开关可逆、恢复只差一次写；
 *   而且调 0 之后从 0 往上按要一档一档爬（用户抱怨的"按了还是没声音"就是这类）。 */
bool DeviceAudio::setMuted(bool muted) {
#ifdef FUN_BUILD
  Impl *im = (Impl *)impl_;
  if (!im || !im->a.ctl_open) return false;
  AlsaApi &a = im->a;
  snd_ctl_t *ctl = 0;
  if (a.ctl_open(&ctl, "hw:0", 0) < 0 || !ctl) return false;
  snd_ctl_elem_list_t *list = 0;
  if (a.ctl_elem_list_malloc(&list) < 0 || !list) {
    a.ctl_close(ctl);
    return false;
  }
  a.ctl_elem_list(ctl, list);
  unsigned int count = a.ctl_elem_list_get_count(list);
  if (count > 0 && a.ctl_elem_list_alloc_space(list, count) >= 0)
    a.ctl_elem_list(ctl, list);

  snd_ctl_elem_info_t *info = 0;
  snd_ctl_elem_id_t *id = 0;
  a.ctl_elem_info_malloc(&info);
  a.ctl_elem_id_malloc(&id);
  int found = 0, ok = 0;
  for (unsigned int i = 0; i < count && info && id; ++i) {
    if (a.ctl_elem_list_get_id(list, i, id) < 0) continue;
    const char *nm = a.ctl_elem_id_get_name(id);
    if (!nm) continue;
    if (!strstr(nm, "SPK") && !strstr(nm, "LINEOUT")) continue;
    if (!strstr(nm, "Switch")) continue;
    a.ctl_elem_info_set_id(info, id);
    if (a.ctl_elem_info(ctl, info) < 0) continue;
    if (a.ctl_elem_info_get_type(info) != PG_CTL_ELEM_TYPE_BOOLEAN) continue;
    ++found;
    if (writeBoolVal(im, ctl, id, nm, !muted)) ++ok;
  }
  if (info) a.ctl_elem_info_free(info);
  if (id) a.ctl_elem_id_free(id);
  a.ctl_elem_list_free(list);
  a.ctl_close(ctl);
  LOGD("PgAudio: 静音 -> %s（输出开关 %d 个，成功 %d 个）", muted ? "开" : "关", found, ok);
  return found > 0 && ok == found;
#else
  (void)muted;
  return false;
#endif
}

bool DeviceAudio::isMuted() {
#ifdef FUN_BUILD
  Impl *im = (Impl *)impl_;
  if (!im || !im->a.ctl_open) return false;
  AlsaApi &a = im->a;
  snd_ctl_t *ctl = 0;
  if (a.ctl_open(&ctl, "hw:0", 0) < 0 || !ctl) return false;
  snd_ctl_elem_list_t *list = 0;
  if (a.ctl_elem_list_malloc(&list) < 0 || !list) {
    a.ctl_close(ctl);
    return false;
  }
  a.ctl_elem_list(ctl, list);
  unsigned int count = a.ctl_elem_list_get_count(list);
  if (count > 0 && a.ctl_elem_list_alloc_space(list, count) >= 0)
    a.ctl_elem_list(ctl, list);
  snd_ctl_elem_info_t *info = 0;
  snd_ctl_elem_id_t *id = 0;
  a.ctl_elem_info_malloc(&info);
  a.ctl_elem_id_malloc(&id);
  int spkOn = -1;
  for (unsigned int i = 0; i < count && info && id; ++i) {
    if (a.ctl_elem_list_get_id(list, i, id) < 0) continue;
    const char *nm = a.ctl_elem_id_get_name(id);
    if (!nm) continue;
    if (!strstr(nm, "SPK Switch") && !strstr(nm, "LINEOUT Switch")) continue;
    a.ctl_elem_info_set_id(info, id);
    if (a.ctl_elem_info(ctl, info) < 0) continue;
    if (a.ctl_elem_info_get_type(info) != PG_CTL_ELEM_TYPE_BOOLEAN) continue;
    snd_ctl_elem_value_t *v = 0;
    if (a.ctl_elem_value_malloc && a.ctl_elem_value_malloc(&v) == 0 && v) {
      a.ctl_elem_value_set_id(v, id);
      if (a.ctl_elem_read && a.ctl_elem_read(ctl, v) >= 0 && a.ctl_elem_value_get_boolean)
        spkOn = a.ctl_elem_value_get_boolean(v, 0);
      if (a.ctl_elem_value_free) a.ctl_elem_value_free(v);
    }
    if (strstr(nm, "SPK Switch") && spkOn >= 0) break;   // 以 SPK 为准
  }
  if (info) a.ctl_elem_info_free(info);
  if (id) a.ctl_elem_id_free(id);
  a.ctl_elem_list_free(list);
  a.ctl_close(ctl);
  return spkOn == 0;
#else
  return false;
#endif
}

void DeviceAudio::reassertOutputSwitch() {
#ifdef FUN_BUILD
  if (impl_) ensureSpeakerOn((Impl *)impl_);
#endif
}

DeviceAudio::DeviceAudio()
    : impl_(0), backend_("none"), enabled_(true), available_(false) {}

DeviceAudio::~DeviceAudio() { shutdown(); }

bool DeviceAudio::init() {
#ifdef FUN_BUILD
  if (impl_) return true;
  Impl *im = new Impl();
  if (!im->a.load()) {
    delete im;
    return false;
  }
  // 预载全部音效（一次性 I/O，之后触发音效零 I/O）
  int loaded = 0;
  for (int i = 1; i < SFX_COUNT; ++i) {
    std::string p = CONFIGMANAGER->getResFilePath(kSfxFiles[i]);
    if (loadClip(p.c_str(), &im->clips[i])) {
      ++loaded;
    } else {
      LOGW("PgAudio: 音效缺失 %s", p.c_str());
    }
  }
  available_ = loaded > 0;
  if (!openStream(im)) {
    delete im;
    backend_ = "none";
    available_ = false;
    return false;
  }
  lastActMs_ = nowMs();  // 刚打开就算"活动"，空闲让出从这里开始计时
  // 打开流之后再设输出开关：这才是最终生效的状态（顺序反了可能被驱动复位覆盖）
  ensureSpeakerOn(im);
  im->enabled = enabled_;
  impl_ = im;
  backend_ = "alsa-hw0,0(常开流)";
  if (im->run) return true;
  im->run = true;
  if (pthread_create(&im->th, 0, audioThread, im) != 0) {
    im->run = false;
    LOGE("PgAudio: 音频线程创建失败");
    return false;
  }
  im->started = true;
  LOGD("PgAudio: 音频输出 backend=%s 预载 %d/%d 个音效 sfx=%s",
       backend_, loaded, SFX_COUNT - 1, available_ ? "on" : "off");
  return true;
#else
  return false;
#endif
}

void DeviceAudio::setEnabled(bool on) {
#ifdef FUN_BUILD
  if (impl_) ((Impl *)impl_)->loggedOff = false;  // 每次切换后允许再报一次
#endif
  enabled_ = on;
#ifdef FUN_BUILD
  if (impl_) {
    Impl *im = (Impl *)impl_;
    pthread_mutex_lock(&im->mu);
    im->enabled = on;
    if (!on) im->cur = -1;  // 立刻停掉当前音效，之后只写静音
    pthread_mutex_unlock(&im->mu);
  }
  LOGD("PgAudio: 音效开关 -> %s（流照旧，只改是否写音效）", on ? "开" : "关");
#endif
}

void DeviceAudio::shutdown() {
#ifdef FUN_BUILD
  if (impl_) {
    Impl *im = (Impl *)impl_;
    im->run = false;
    if (im->pcm) im->a.pcm_drop(im->pcm);  // 解除 writei 阻塞，让线程尽快退出
    im->son = false;
    if (im->sbuf) { free(im->sbuf); im->sbuf = 0; im->scap = 0; }
    if (im->started) {
      pthread_join(im->th, 0);
      im->started = false;
    }
    if (im->pcm) {
      im->a.pcm_close(im->pcm);
      im->pcm = 0;
    }
    for (int i = 1; i < SFX_COUNT; ++i) free(im->clips[i].data);
    if (im->a.lib) dlclose(im->a.lib);
    pthread_mutex_destroy(&im->mu);
    delete im;
    impl_ = 0;
  }
#endif
  backend_ = "none";
  available_ = false;
}

/* ==================== 声卡占用控制（releasePcm / acquirePcm） ====================
 * 背景（现场实测 2026-09-13）：常开流把 `hw:0,0` **长期独占**，此时别的进程打开 card0
 * 会**挂住**（`/bin/tinyplay t.wav -D 0` 跑 6 秒被 timeout 杀掉、无任何输出）——
 * 即"应用在跑，设备上别的程序一出声就卡"。所以把"让出/拿回"做成显式能力。
 *
 * 释放顺序很讲究（照抄 shutdown 里验证过的顺序，少一步就会卡在 writei 上）：
 *   ① run=false → ② pcm_drop()（**必须**，否则线程阻塞在 writei 里等缓冲腾位置，
 *   join 永远等不到）→ ③ join → ④ close。
 * 拿回：openStream()（重建 hw/sw params）+ ensureSpeakerOn()（重开 SPK/LINEOUT 开关，
 * 因为 close 过 DAC 之后驱动可能把它复位）+ 重启线程。音效数据一直留在内存里，无需重载。
 */
bool DeviceAudio::releasePcm() {
#ifdef FUN_BUILD
  if (!impl_) return false;
  Impl *im = (Impl *)impl_;
  if (!im->pcm && !im->started) {
    LOGD("PgAudio: 声卡已经是释放状态（无需再释放）");
    return true;
  }
  im->run = false;
  if (im->pcm) im->a.pcm_drop(im->pcm);  // 解除 writei 阻塞
  pthread_mutex_lock(&im->mu);
  im->son = false;                       // 在线流音频队列一并关掉
  pthread_mutex_unlock(&im->mu);
  if (im->started) {
    pthread_join(im->th, 0);
    im->started = false;
  }
  if (im->pcm) {
    im->a.pcm_close(im->pcm);
    im->pcm = 0;
  }
  backend_ = "alsa-hw0,0(已释放)";
  LOGD("PgAudio: 已释放声卡 hw:0,0 —— 其它进程现在可以打开 card0（QA: pcmopen 拿回）");
  return true;
#else
  return false;
#endif
}

bool DeviceAudio::acquirePcm() {
#ifdef FUN_BUILD
  if (!impl_) return false;
  Impl *im = (Impl *)impl_;
  if (im->pcm && im->started) return true;  // 幂等
  if (!im->pcm && !openStream(im)) {
    LOGE("PgAudio: 拿回声卡失败（hw:0,0 打开不了）");
    return false;
  }
  ensureSpeakerOn(im);  // 重新打开后必须再确认一次输出开关（close 过 DAC，可能被复位）
  pthread_mutex_lock(&im->mu);
  im->cur = -1;
  im->pos = 0;
  im->writes = 0;
  im->loggedGap = false;
  im->shead = im->stail = 0;
  pthread_mutex_unlock(&im->mu);
  if (!im->started) {
    im->run = true;
    if (pthread_create(&im->th, 0, audioThread, im) != 0) {
      im->run = false;
      LOGE("PgAudio: 音频线程重建失败");
      return false;
    }
    im->started = true;
  }
  backend_ = "alsa-hw0,0(常开流)";
  lastActMs_ = nowMs();
  LOGD("PgAudio: 已拿回声卡 hw:0,0（常开流恢复）");
  return true;
#else
  return false;
#endif
}

bool DeviceAudio::pcmHeld() const {
#ifdef FUN_BUILD
  if (!impl_) return false;
  return ((Impl *)impl_)->pcm != 0;
#else
  return false;
#endif
}

/* 距上次"要用声音"的毫秒数。lastActMs_ 在 playSfx / streamOn / streamWrite 里刷新
 * （streamWrite 每块刷一次 ≈137 次/秒，只写一个 volatile long long，开销可忽略）。 */
long long DeviceAudio::idleMs() const {
  if (lastActMs_ <= 0) return 0;
  long long d = nowMs() - lastActMs_;
  return d > 0 ? d : 0;
}

void DeviceAudio::setPcmIdleSec(int sec) {
  if (sec < 0) sec = 0;
  if (sec > 3600) sec = 3600;
  pcmIdleSec_ = sec;
  LOGD("PgAudio: 声卡空闲让出阈值 -> %d 秒（0 = 关闭，永远常开）", pcmIdleSec_);
}

void DeviceAudio::playSfx(int sfxId) {
  if (sfxId <= SFX_NONE || sfxId >= SFX_COUNT) return;
  lastActMs_ = nowMs();  // 有播放意图就算"要用声音"（空闲让出的判据）
#ifdef FUN_BUILD
  if (!impl_) return;
  Impl *im = (Impl *)impl_;
  long t = nowMs();
  pthread_mutex_lock(&im->mu);
  if (im->enabled && im->clips[sfxId].data) {
    if (!(sfxId == im->lastSfx && (t - im->lastMs) < SFX_MIN_GAP_MS)) {
      im->cur = sfxId;  // 直接换块：新音效从头播（游戏音效就该这样）
      im->pos = 0;
      LOGD("PgAudio: 播放 %s", kSfxFiles[sfxId]);
    }
    im->lastSfx = sfxId;
    im->lastMs = t;
  } else if (!im->enabled && !im->loggedOff) {
    // 只报一次：音效关闭期间所有触发都被忽略（排查时不用靠耳朵听）
    im->loggedOff = true;
    LOGD("PgAudio: 音效已关闭，忽略播放请求（含本次 %s）", kSfxFiles[sfxId]);
  }
  pthread_mutex_unlock(&im->mu);
#endif
}


/* ==================== 进程级音量钩子 ====================
 * wifi.ftu 的 wifiActivity 不持有第二份 DeviceAudio（音频线程只能有一份），
 * mainLogic 的 onUI_init 注入钩子，把音量操作转发给 Host 里的那一份实例。
 */
static int (*sVolumeHookFn)(int) = 0;

/* 音量变化广播（全局音量 OSD）：见 PgAudio.h 的说明。
 * 状态栏（statusbar.ftu）在 onUI_init 里注册，任何界面调 volumeStepGlobal 都会通知它。 */
static void (*sVolumeNotifyFn)(int) = 0;

/* ==================== 在线流 PCM 队列（PgStream 用） ==================== */
namespace {
DeviceAudio *s_globalAudio = 0;
}
void setGlobalAudio(DeviceAudio *a) { s_globalAudio = a; }
DeviceAudio *globalAudio() { return s_globalAudio; }

bool DeviceAudio::streamOn(bool on) {
  Impl *im = (Impl *)impl_;
  if (!im) return false;
  /* ⚠️ 声卡被 releasePcm() 释放时**必须**返回 false：否则调用方会一直
   *    streamWrite() 拿到 0 → 在 usleep 里空转，把整条解码链路卡死（"看起来像卡了"）。
   *    返回 false = 让 PgStream 降级成"只播视频 + 墙钟同步"。 */
  if (on && !im->pcm) {
    LOGW("PgAudio: 声卡未被占用（pcmfree 过）→ 本次在线流不接音频");
    pthread_mutex_lock(&im->mu);
    im->son = false;
    pthread_mutex_unlock(&im->mu);
    return false;
  }
  pthread_mutex_lock(&im->mu);
  if (on && !im->sbuf) {
    /* ⚠️ 只给 400ms：这条环同时充当"喂食节流器"（满了就挡调用方）。
     * 给太大（试过 1s）会让**画面**跑在网络前面 1 秒多，而画面等音频时钟又反过来
     * 卡住喂食 → 队列时满时空、整条链路慢到 1/2 实时且伴音断续（实测踩过）。 */
    im->scap = (uint32_t)STREAM_RATE * STREAM_CH * 2 * 4 / 10;  // 400ms
    im->sbuf = (uint8_t *)malloc(im->scap);
    if (!im->sbuf) im->scap = 0;
  }
  im->shead = im->stail = 0;
  im->son = on && im->sbuf != 0;
  bool usable = im->son;
  uint32_t cap = im->scap;
  pthread_mutex_unlock(&im->mu);
  if (on) lastActMs_ = nowMs();  // 在线流要用音频 → 刷新"活动时刻"
  LOGD("PgAudio: 在线流音频队列 %s（%u 字节）", im->son ? "开" : "关", cap);
  return on ? usable : true;
}

int DeviceAudio::streamWrite(const int16_t *pcm, int frames) {
  Impl *im = (Impl *)impl_;
  if (!im || !pcm || frames <= 0) return 0;
  /* ★ 音频可视化取点（2026-09-18）：这里是"喇叭里真正要放的那块 PCM"，
   *   PgViz 拿它做真 FFT（收音机播放页的频谱/电平就来自这里）。
   *   取在**这条路上**的意义：静音/没出声时频谱必然不动，不会出现"假动画"。
   *   成本：每 30ms 一次 512 点 FFT（≈2.3 万次浮点），本板 ≪1% CPU。 */
  pg::Viz::notePcm(pcm, frames);
  if (im->son) lastActMs_ = nowMs();  // 在线流正在灌音频 → 绝不算"空闲"
  const uint32_t bytes = (uint32_t)frames * STREAM_CH * 2;
  int taken = 0;
  pthread_mutex_lock(&im->mu);
  if (im->son && im->sbuf) {
    uint32_t freeB = im->scap - 1 - ringUsed(im);
    uint32_t n = (bytes < freeB) ? bytes : freeB;
    const uint32_t frameBytes = STREAM_CH * 2;   // 按整帧收，避免半个采样错位
    n -= n % frameBytes;
    if (n) {
      const uint8_t *src = (const uint8_t *)pcm;
      uint32_t first = im->scap - im->shead;
      if (first > n) first = n;
      memcpy(im->sbuf + im->shead, src, first);
      if (n > first) memcpy(im->sbuf, src + first, n - first);
      im->shead = (im->shead + n) % im->scap;
      taken = (int)(n / frameBytes);
    }
  }
  pthread_mutex_unlock(&im->mu);
  return taken;
}

int DeviceAudio::streamQueuedFrames() const {
  Impl *im = (Impl *)impl_;
  if (!im || !im->son) return 0;
  pthread_mutex_lock(&im->mu);
  uint32_t used = ringUsed(im);
  pthread_mutex_unlock(&im->mu);
  return (int)(used / (STREAM_CH * 2));
}

void setVolumeHook(int (*fn)(int)) { sVolumeHookFn = fn; }

void setVolumeNotifyHook(void (*fn)(int)) { sVolumeNotifyFn = fn; }

/* ==================== 进程级静音（2026-09-16 用户需求） ====================
 * 用户原话：「按键调整音量的默认去掉静音，音量弹出框也可以快速静音」。
 *
 * 为什么要有一个"pg 层"的状态：真值在 codec 的输出开关上，但
 *   ① 每次按音量键都去枚举 ALSA 控件太重（枚举一遍 24 个控件）；
 *   ② 按键逻辑要判断"现在是不是静音"才能决定"先取消静音还是调音量"。
 * 所以这里缓存一份，钩子负责写真开关。
 *
 * ⚠️ 系统默认**不静音**（启动时不动开关，保持 codec 默认导通）。
 *    sLastPct 也默认 -1：未知时"恢复静音"兜底到 60%（比 0% 有用，也比 100% 不吓人）。 */
static bool (*sMuteHookFn)(bool) = 0;
static void (*sMuteNotifyFn)(bool) = 0;
static bool sMuted = false;
static int sLastPct = -1;

void setMuteHook(bool (*fn)(bool)) { sMuteHookFn = fn; }
void setMuteNotifyHook(void (*fn)(bool)) { sMuteNotifyFn = fn; }
bool isMutedGlobal() { return sMuted; }
void noteVolumePercent(int pct) {
  if (pct >= 0) sLastPct = pct;
}

bool setMutedGlobal(bool muted) {
  if (muted == sMuted) {
    if (sMuteNotifyFn) sMuteNotifyFn(muted);   // 幂等，但 OSD 还是要画对
    return true;
  }
  const bool ok = sMuteHookFn ? sMuteHookFn(muted) : false;
  if (!ok) {
    LOGW("PgAudio: 静音切换失败（钩子未注入或输出开关写入失败）");
    return false;
  }
  sMuted = muted;
  LOGD("PgAudio: 静音 %s（当前音量 %d%%）", muted ? "开" : "关", sLastPct);
  if (sMuteNotifyFn) sMuteNotifyFn(muted);
  return true;
}

int volumeStepGlobal(int delta) {
  /* ★ 「按键调整音量的默认去掉静音」：静音状态下按"音量+"，
   *   先把静音取消、（音量本身不动）并恢复上次音量 —— 而不是"音量 +1 档、喇叭还是哑的"。
   *   现场最常见的抱怨就是这个（按 + 没反应，得先去设置里取消静音）。 */
  if (delta > 0 && sMuted) {
    const int restore = (sLastPct >= 0) ? sLastPct : 60;
    if (setMutedGlobal(false)) {
      LOGD("PgAudio: 音量+ 时先取消静音（恢复到 %d%%）", restore);
      if (sVolumeNotifyFn) sVolumeNotifyFn(restore);
      return restore;
    }
  }
  int pct = sVolumeHookFn ? sVolumeHookFn(delta) : -1;
  if (pct >= 0) sLastPct = pct;   // 记住"当前音量"：静音恢复、音量条显示都要用
  /* ★ 全局音量 OSD 的数据源：**只在这里广播**（所有界面的音量操作都走本函数）。
   *   失败（-1）不广播 —— UI 不该为一个没生效的操作弹面板。 */
  if (pct >= 0 && sVolumeNotifyFn) sVolumeNotifyFn(pct);
  return pct;
}

/* ---------------- 绝对音量 + OSD 抑制窗（2026-09-17） ---------------- */
static int (*sVolumeSetFn)(int) = 0;
static long long sOsdSuppressUntilMs = 0;

void setVolumeSetHook(int (*fn)(int)) { sVolumeSetFn = fn; }

void suppressVolumeOsd(int ms) {
  const long long until = (long long)nowMs() + (ms > 0 ? ms : 0);
  if (until > sOsdSuppressUntilMs) sOsdSuppressUntilMs = until;   // 只延长，不缩短
}

int setVolumePercentGlobal(int pct) {
  if (!sVolumeSetFn) return -1;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  /* ★ 「调整音量自动解除静音」（2026-09-17 用户需求）—— 与 volumeStepGlobal 同一纪律：
   *   主动调音量就是"我要听"的表达，不该还哑着。音量面板上的图标是显式的静音开关，
   *   而"拨音量"隐含的是解除。pct > 0 才解除：拖到 0 不是"想听"。 */
  if (sMuted && pct > 0) {
    if (setMutedGlobal(false)) {
      LOGD("PgAudio: 调整音量 %d%% -> 自动解除静音", pct);
      if (sVolumeNotifyFn) sVolumeNotifyFn(pct);
    }
  }
  const int got = sVolumeSetFn(pct);
  if (got < 0) return -1;
  sLastPct = got;                       // 与 volumeStepGlobal 同一份"当前音量"
  /* 抑制窗内不广播：拖动设置页自己的条时不该再弹全局音量面板。
   * （按键那条路 volumeStepGlobal 不检查本窗 —— 按键弹 OSD 是它的语义。） */
  if ((long long)nowMs() >= sOsdSuppressUntilMs && sVolumeNotifyFn) sVolumeNotifyFn(got);
  return got;
}

}  // namespace pg
