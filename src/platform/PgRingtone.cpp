/*
 * PgRingtone.cpp - 闹钟铃声循环播放（官方音频播放器 zk_audio_player）
 * 设计说明见 PgRingtone.h。
 */
#include "platform/PgRingtone.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manager/ConfigManager.h"
#include "media/audio_player.h"   // zk_audio_player_*（实现来自参考工程的 libzkmedia.a）
#include "platform/PgAudio.h"     // 让出/拿回声卡
#include "utils/Log.h"

namespace pg {

namespace {

const uint32_t kChunk = 4096;   // 每次推给播放器的字节数（≈93ms @22050Hz/单声道/16bit）

struct WavData {
  uint8_t *pcm = 0;
  uint32_t size = 0;
  int channels = 1;
  int rate = 22050;
  int bits = 16;
};

/* 极简 wav 解析：chunk 遍历，只取 fmt / data（照参考工程 src/media/wav_utils.c 的写法）。
 * 只支持 16bit PCM —— 我们自己生成的音效都是这个格式。 */
bool loadWav(const char *path, WavData *out) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;

  char id[4];
  uint32_t sz = 0;
  bool gotFmt = false, gotData = false;

  if (fread(id, 1, 4, fp) != 4 || memcmp(id, "RIFF", 4) != 0) {
    fclose(fp);
    return false;
  }
  if (fread(&sz, 4, 1, fp) != 1) { fclose(fp); return false; }   // RIFF size
  if (fread(id, 1, 4, fp) != 4 || memcmp(id, "WAVE", 4) != 0) {
    fclose(fp);
    return false;
  }

  while (!gotData) {
    if (fread(id, 1, 4, fp) != 4) break;
    if (fread(&sz, 4, 1, fp) != 1) break;
    if (memcmp(id, "fmt ", 4) == 0) {
      uint16_t fmt = 0, ch = 0, ba = 0, bps = 0;
      uint32_t rate = 0, br = 0;
      if (fread(&fmt, 2, 1, fp) != 1 || fread(&ch, 2, 1, fp) != 1 ||
          fread(&rate, 4, 1, fp) != 1 || fread(&br, 4, 1, fp) != 1 ||
          fread(&ba, 2, 1, fp) != 1 || fread(&bps, 2, 1, fp) != 1) {
        break;
      }
      if (fmt != 1) { fclose(fp); out->bits = -1; return false; }   // 非 PCM
      out->channels = ch;
      out->rate = (int)rate;
      out->bits = bps;
      if (sz > 16) fseek(fp, (long)(sz - 16), SEEK_CUR);
      gotFmt = true;
    } else if (memcmp(id, "data", 4) == 0) {
      out->pcm = (uint8_t *)malloc(sz ? sz : 1);
      if (!out->pcm) break;
      out->size = (uint32_t)fread(out->pcm, 1, sz, fp);
      gotData = true;
    } else {
      fseek(fp, (long)(sz + (sz & 1)), SEEK_CUR);   // 未知 chunk：跳过（偶数字节对齐）
    }
  }
  fclose(fp);

  if (!gotFmt || !gotData || !out->pcm || out->size == 0) {
    if (out->pcm) { free(out->pcm); out->pcm = 0; }
    return false;
  }
  return true;
}

}  // namespace

Ringtone &Ringtone::instance() {
  static Ringtone s;
  return s;
}

void *Ringtone::threadEntry(void *self) {
  return ((Ringtone *)self)->threadMain();
}

void *Ringtone::threadMain() {
  int ret = zk_audio_player_init((uint32_t)ch_, (uint32_t)rate_, 0, 0);
  LOGD("PgRingtone: zk_audio_player_init(%d, %d) -> %d", ch_, rate_, ret);
  if (ret != 0) {
    snprintf(err_, sizeof(err_), "zk_audio_player_init=%d", ret);
    running_ = false;
    return 0;
  }

  /* 循环推帧。put_frame 是阻塞写（缓冲满则等），所以这里不需要 sleep；
   * 一次最多阻塞 kChunk（≈93ms），因此 stop() 的 join 不会久等。 */
  uint32_t pos = 0;
  uint32_t loops = 0;
  while (running_ && size_ > 0) {
    uint32_t n = size_ - pos;
    if (n > kChunk) n = kChunk;
    if (zk_audio_player_put_frame(pcm_ + pos, n) != 0) {
      LOGW("PgRingtone: put_frame 失败，停止响铃");
      break;
    }
    pos += n;
    if (pos >= size_) {
      pos = 0;
      ++loops;
      /* 每循环 ~25 次（约 30 秒）报一次，便于判断"还在响" */
      if ((loops % 25) == 0) LOGD("PgRingtone: 已循环 %u 次（仍在响铃）", loops);
    }
  }

  zk_audio_player_deinit();
  LOGD("PgRingtone: 播放线程退出（循环 %u 次）", loops);
  return 0;
}

bool Ringtone::start(const char *wavRelPath) {
  stop();   // 幂等：先收拾上一次

  if (!wavRelPath || !*wavRelPath) {
    snprintf(err_, sizeof(err_), "空路径");
    return false;
  }

  std::string full = CONFIGMANAGER->getResFilePath(wavRelPath);
  WavData w;
  if (!loadWav(full.c_str(), &w)) {
    snprintf(err_, sizeof(err_), "wav 读取失败(%s)", full.c_str());
    LOGE("PgRingtone: %s", err_);
    return false;
  }
  if (w.bits != 16) {
    snprintf(err_, sizeof(err_), "只支持 16bit PCM（该文件 %d bit）", w.bits);
    LOGE("PgRingtone: %s", err_);
    free(w.pcm);
    return false;
  }

  pcm_ = w.pcm;
  size_ = w.size;
  ch_ = w.channels;
  rate_ = w.rate;
  strncpy(name_, wavRelPath, sizeof(name_) - 1);
  name_[sizeof(name_) - 1] = 0;
  err_[0] = 0;

  /* 让出 PgAudio 的常开流（两个播放器互斥，都独占 card0） */
  if (DeviceAudio *a = globalAudio()) {
    if (a->pcmHeld()) {
      pcmReleased_ = a->releasePcm();
      LOGD("PgRingtone: 让出 PgAudio 声卡 -> %d", pcmReleased_ ? 1 : 0);
    }
  }

  running_ = true;
  if (pthread_create(&th_, 0, threadEntry, this) != 0) {
    running_ = false;
    snprintf(err_, sizeof(err_), "线程创建失败");
    /* 把声卡还回去，别让音效一直哑着 */
    if (pcmReleased_) {
      if (DeviceAudio *a = globalAudio()) a->acquirePcm();
      pcmReleased_ = false;
    }
    free(pcm_);
    pcm_ = 0;
    size_ = 0;
    return false;
  }
  threadAlive_ = true;
  LOGD("PgRingtone: 开始响铃 %s（%u 字节 / %d Hz / %d 声道）", name_, size_, rate_, ch_);
  return true;
}

void Ringtone::stop() {
  if (running_ || threadAlive_) {
    running_ = false;
    if (threadAlive_) {
      pthread_join(th_, 0);
      threadAlive_ = false;
    }
    LOGD("PgRingtone: 停止响铃 %s", name_);
  }
  if (pcm_) {
    free(pcm_);
    pcm_ = 0;
    size_ = 0;
  }
  if (pcmReleased_) {
    if (DeviceAudio *a = globalAudio()) a->acquirePcm();
    pcmReleased_ = false;
  }
}

}  // namespace pg
