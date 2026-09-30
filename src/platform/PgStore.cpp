#include "PgStore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/Log.h"

namespace pg {

namespace {

/*
 * 存档落盘位置，按"能跨重启保留"优先：
 *   /data       —— 本板是**可写的 jffs2 闪存分区**（重启不丢），首选
 *   /mnt/extsd  —— TF 卡（插了卡才有）
 *   /mnt/udisk  —— U 盘
 *   /tmp        —— tmpfs，仅本次开机有效（最后兜底，保证"能存"）
 */
const char *kCandidates[] = {
#ifdef FUN_BUILD
    "/data/pocketgame.dat",
    "/mnt/extsd/pocketgame.dat",
    "/mnt/udisk/pocketgame.dat",
    "/tmp/pocketgame.dat",
#else
    "pocketgame.dat",
    "/tmp/pocketgame.dat",
#endif
};

bool writable(const char *path) {
  FILE *f = fopen(path, "ab");
  if (!f) return false;
  fclose(f);
  return true;
}

}  // namespace

ScoreStore::ScoreStore() {
  memset(high_, 0, sizeof(high_));
  sound_ = 1;   // 默认音效开
  volume_ = -1;  // -1 = 没调过，启动时不主动设音量（保持驱动默认）
  rot_ = -1;     // -1 = 投屏画面旋转跟随视频元数据（auto）
  vbv_ = 1048576;  // zkmedia 码流缓冲 1MB（参考工程同值，见 docs/h264-direct.md）
  path_[0] = 0;
}

ScoreStore::~ScoreStore() { save(); }

void ScoreStore::load() {
  const int n = (int)(sizeof(kCandidates) / sizeof(kCandidates[0]));
  const char *loaded = 0;
  for (int i = 0; i < n; ++i) {
    FILE *f = fopen(kCandidates[i], "rb");
    if (!f) continue;
    loaded = kCandidates[i];
    char line[256];
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
      // 设置项：key=value（老存档没有这些行，用默认值）
      if (strncmp(line, "sound=", 6) == 0) {
        sound_ = (atoi(line + 6) != 0) ? 1 : 0;
        continue;
      }
      if (strncmp(line, "volume=", 7) == 0) {
        int v = atoi(line + 7);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        volume_ = v;
        continue;
      }
      if (strncmp(line, "rot=", 4) == 0) {
        int v = atoi(line + 4);
        if (v < 0) v = -1;                       // -1 = auto
        else rot_ = ((v % 360) + 360) % 360;      // 归一到 0/90/180/270
        continue;
      }
      if (strncmp(line, "vbv=", 4) == 0) {
        int v = atoi(line + 4);
        if (v >= 65536 && v <= (64 << 20)) vbv_ = v;   // 合理范围才收（防手误写 0/负数）
        continue;
      }
      if (idx < MAX_GAMES) {
        int v = atoi(line);
        if (v > 0) high_[idx] = v;
        ++idx;
      }
    }
    fclose(f);
    break;  // 优先级最高的那个文件为准
  }

  // 保存目标单独选：**优先可跨重启的分区**（/data 是 jffs2 闪存，重启不丢），
  // 而不是"从哪读的就写哪" —— 否则一旦读到 /tmp 里那份，就永远存在 /tmp 了。
  path_[0] = 0;
  for (int i = 0; i < n; ++i) {
    if (writable(kCandidates[i])) {
      snprintf(path_, sizeof(path_), "%s", kCandidates[i]);
      break;
    }
  }
  if (!path_[0] && loaded) snprintf(path_, sizeof(path_), "%s", loaded);

  if (loaded && path_[0] && strcmp(loaded, path_) != 0) {
    LOGD("PgStore: 存档 %s -> 迁到 %s（可跨重启）", loaded, path_);
    save();  // 把读到的内容写到更好的位置
  }
  LOGD("PgStore: loaded %s (sound=%d), save to %s",
       loaded ? loaded : "(none)", sound_, path_[0] ? path_ : "(none)");
}

void ScoreStore::save() {
  if (!path_[0]) return;
  // 先写临时文件再改名：避免掉电/复位时把存档写坏（闪存上尤其重要）
  char tmp[160];
  int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path_);
  const char *target = (n > 0 && n < (int)sizeof(tmp)) ? tmp : path_;
  FILE *f = fopen(target, "wb");
  if (!f && target == tmp) f = fopen(path_, "wb");  // 建不了临时文件就直接写原文件
  if (!f) {
    LOGW("PgStore: 保存失败 %s", path_);
    return;
  }
  for (int i = 0; i < MAX_GAMES; ++i) {
    fprintf(f, "%d\n", high_[i]);
  }
  fprintf(f, "sound=%d\n", sound_);
  if (volume_ >= 0) fprintf(f, "volume=%d\n", volume_);
  /* 旋转始终写（含 -1=auto）：用户从固定角度改回 auto 时也要能覆盖掉旧行 */
  fprintf(f, "rot=%d\n", rot_);
  fprintf(f, "vbv=%d\n", vbv_);
  fflush(f);
  fclose(f);
  if (target == tmp) rename(tmp, path_);
}

// 音量改动 → 落盘（下次启动 PgAudio 会用它恢复）
void ScoreStore::setVolumePercent(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (volume_ == pct) return;
  volume_ = pct;
  save();
}

void ScoreStore::setVbvBytes(int n) {
  if (n < 65536 || n > (64 << 20)) return;
  vbv_ = n;
  save();
}

void ScoreStore::setRotDeg(int deg) {
  int v = (deg < 0) ? -1 : (((deg % 360) + 360) % 360);
  if (rot_ == v) return;
  rot_ = v;
  save();
}

void ScoreStore::setSoundOn(bool on) {
  int v = on ? 1 : 0;
  if (sound_ == v) return;
  sound_ = v;
  save();
}

int ScoreStore::high(int index) const {
  if (index < 0 || index >= MAX_GAMES) return 0;
  return high_[index];
}

void ScoreStore::setHigh(int index, int score) {
  if (index < 0 || index >= MAX_GAMES) return;
  if (score <= high_[index]) return;
  high_[index] = score;
  save();
}

}  // namespace pg
