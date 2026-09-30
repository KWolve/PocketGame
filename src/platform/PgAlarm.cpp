/*
 * PgAlarm.cpp - 闹钟（守护线程 + 到点响铃/亮屏）。设计说明见 PgAlarm.h。
 */
#include "platform/PgAlarm.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "platform/PgRingtone.h"
#include "utils/BrightnessHelper.h"   // BRIGHTNESSHELPER->screenOn()（zkhardware）
#include "utils/Log.h"

namespace pg {

namespace {

const int kRingMaxMs = 60 * 1000;   // 响铃自动超时（1 分钟，防止无人按停一直响）
const int kGuardTickMs = 400;       // 守护线程轮询间隔（< 1s，不会漏掉分钟边界）
const char *kRingWav = "audio/alarm.wav";

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool writablePath(const char *p) {
  FILE *f = fopen(p, "ab");
  if (!f) return false;
  fclose(f);
  return true;
}

}  // namespace

Alarm &Alarm::instance() {
  static Alarm s;
  return s;
}

void Alarm::sleepMs(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000;
  nanosleep(&ts, 0);
}

/* ==================== 存储 ==================== */

void Alarm::load() {
  /* 载入路径与保存路径分开看：读可以"第一个存在者优先"，存必须"第一个**可写**者优先" */
  const char *cand[] = {"/data/pocketgame_alarms.txt", "/mnt/extsd/pocketgame_alarms.txt",
                        "/tmp/pocketgame_alarms.txt"};
  const char *found = 0;
  for (int i = 0; i < 3; ++i) {
    FILE *f = fopen(cand[i], "rb");
    if (f) {
      fclose(f);
      found = cand[i];
      break;
    }
  }

  count_ = 0;
  if (found) {
    FILE *f = fopen(found, "rb");
    if (f) {
      char line[128];
      while (fgets(line, sizeof(line), f) && count_ < kMaxAlarms) {
        if (line[0] == '#' || line[0] == '\n') continue;
        int en = 0, hh = 0, mm = 0, mask = 0;
        if (sscanf(line, "%d %d %d %d", &en, &hh, &mm, &mask) == 4) {
          if (hh < 0 || hh > 23 || mm < 0 || mm > 59) continue;
          items_[count_].enabled = (en != 0);
          items_[count_].hour = hh;
          items_[count_].minute = mm;
          items_[count_].mask = mask;
          ++count_;
        }
      }
      fclose(f);
    }
  }

  /* 保存目标：第一个可写路径（本板 /data 是可写 jffs2，重启不丢） */
  path_[0] = 0;
  for (int i = 0; i < 3; ++i) {
    if (writablePath(cand[i])) {
      snprintf(path_, sizeof(path_), "%s", cand[i]);
      break;
    }
  }
  LOGD("PgAlarm: 载入 %d 个闹钟（来源 %s，保存到 %s）", count_, found ? found : "(无文件)",
       path_[0] ? path_ : "(无)");
}

bool Alarm::save() {
  if (!path_[0]) return false;
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path_);
  FILE *f = fopen(tmp, "wb");
  const char *target = tmp;
  if (!f) {
    f = fopen(path_, "wb");
    target = path_;
  }
  if (!f) {
    LOGW("PgAlarm: 保存失败 %s", path_);
    return false;
  }
  fprintf(f, "# PocketGame 闹钟：enabled hour minute mask（mask 0=仅一次, 127=每天）\n");
  for (int i = 0; i < count_; ++i) {
    fprintf(f, "%d %02d %02d %d\n", items_[i].enabled ? 1 : 0, items_[i].hour,
            items_[i].minute, items_[i].mask);
  }
  fflush(f);
  fclose(f);
  if (target == tmp && rename(tmp, path_) != 0) {
    LOGW("PgAlarm: rename 失败（%s）", path_);
    return false;
  }
  LOGD("PgAlarm: 已保存 %d 个闹钟 -> %s", count_, path_);
  return true;
}

bool Alarm::add(int hh, int mm, int mask) {
  if (count_ >= kMaxAlarms) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  items_[count_].enabled = true;
  items_[count_].hour = hh;
  items_[count_].minute = mm;
  items_[count_].mask = mask;
  ++count_;
  save();
  LOGD("PgAlarm: 新增 %02d:%02d mask=%d（共 %d 个）", hh, mm, mask, count_);
  return true;
}

bool Alarm::setItem(int i, int hh, int mm, int mask) {
  if (i < 0 || i >= count_) return false;
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  items_[i].hour = hh;
  items_[i].minute = mm;
  items_[i].mask = mask;
  save();
  LOGD("PgAlarm: 更新 #%d -> %02d:%02d mask=%d", i, hh, mm, mask);
  return true;
}

bool Alarm::setEnabled(int i, bool on) {
  if (i < 0 || i >= count_) return false;
  items_[i].enabled = on;
  save();
  return true;
}

bool Alarm::removeAt(int i) {
  if (i < 0 || i >= count_) return false;
  for (int k = i; k + 1 < count_; ++k) items_[k] = items_[k + 1];
  --count_;
  save();
  LOGD("PgAlarm: 删除 #%d（剩 %d 个）", i, count_);
  return true;
}

void Alarm::clearAll() {
  count_ = 0;
  save();
  LOGD("PgAlarm: 已清空");
}

/* ==================== 守护线程 ==================== */

void *Alarm::guardEntry(void *self) {
  return ((Alarm *)self)->guardLoop();
}

void *Alarm::guardLoop() {
  LOGD("PgAlarm: 守护线程启动（间隔 %dms，响铃超时 %ds）", kGuardTickMs, kRingMaxMs / 1000);
  while (running_) {
    sleepMs(kGuardTickMs);

    long long t = nowMs();

    /* 响铃自动超时 */
    if (ringUntilMs_ > 0 && t >= ringUntilMs_) {
      LOGD("PgAlarm: 响铃超时（%ds），自动停止", kRingMaxMs / 1000);
      Ringtone::instance().stop();
      ringUntilMs_ = 0;
      snprintf(status_, sizeof(status_), "响铃超时自动停止");
    }

    /* 「N 秒后响」自检 */
    if (pendingAtMs_ > 0 && t >= pendingAtMs_) {
      pendingAtMs_ = -1;
      fire(-1, -1, -1);
      continue;
    }

    /* 分钟边界检查：同一个"年月日时分"只触发一次 */
    time_t now = time(0);
    struct tm lt;
    localtime_r(&now, &lt);
    int key = ((lt.tm_year + 1900) * 100 + (lt.tm_mon + 1)) * 1000000 +
              lt.tm_mday * 10000 + lt.tm_hour * 100 + lt.tm_min;
    if (key == lastMinuteKey_) continue;
    lastMinuteKey_ = key;

    for (int i = 0; i < count_; ++i) {
      const AlarmItem &a = items_[i];
      if (!a.enabled) continue;
      if (a.hour != lt.tm_hour || a.minute != lt.tm_min) continue;
      if (a.mask != 0 && !(a.mask & (1 << lt.tm_wday))) continue;   // wday: 0=周日
      fire(i, a.hour, a.minute);
      break;
    }
  }
  LOGD("PgAlarm: 守护线程退出");
  return 0;
}

void Alarm::fire(int index, int hh, int mm) {
  lastFired_ = index;
  const bool manual = (hh < 0 || mm < 0);
  if (manual) {
    snprintf(status_, sizeof(status_), "手动测试响铃");
    LOGD("PgAlarm: ★ 手动测试响铃（#%d）-> 响铃 + 亮屏", index);
  } else {
    snprintf(status_, sizeof(status_), "触发 #%d %02d:%02d", index, hh, mm);
    LOGD("PgAlarm: ★ 闹钟到点 #%d %02d:%02d -> 响铃 + 亮屏", index, hh, mm);
  }

  /* ① 响铃（官方音频播放器，循环） */
  if (!Ringtone::instance().start(kRingWav)) {
    LOGW("PgAlarm: 铃声起不来（%s）", Ringtone::instance().lastError());
    snprintf(status_, sizeof(status_), "响铃失败：%s", Ringtone::instance().lastError());
  }
  ringUntilMs_ = nowMs() + kRingMaxMs;

  /* ② 亮屏（如果正好是熄屏状态） */
  BRIGHTNESSHELPER->screenOn();

  /* ③ 「仅一次」的闹钟响完就关掉（存盘），否则明天同一时间还会响 */
  if (index >= 0 && index < count_ && items_[index].mask == 0) {
    items_[index].enabled = false;
    save();
    LOGD("PgAlarm: 「仅一次」闹钟已自动关闭 #%d", index);
  }

  /* ④ 通知 UI（可能在另一个线程，回调里只做线程安全的事） */
  if (fireCb_) fireCb_(index, hh, mm);
}

void Alarm::testFireNow() {
  fire(-1, -1, -1);
}

void Alarm::fireIn(int sec) {
  if (sec < 1) sec = 1;
  pendingAtMs_ = nowMs() + (long long)sec * 1000;
  LOGD("PgAlarm: 已排定 %d 秒后响铃（自检）", sec);
}

void Alarm::snooze(int minutes) {
  if (minutes < 1) minutes = 1;
  stopRing();   // 先停当前的
  /* ⚠️ 注意 pendingAtMs_ 不要与 ringUntilMs_（响铃超时）混：这里是"下一次触发时刻" */
  pendingAtMs_ = nowMs() + (long long)minutes * 60 * 1000;
  snprintf(status_, sizeof(status_), "贪睡 %d 分钟", minutes);
  LOGD("PgAlarm: 贪睡 %d 分钟（%lld 后再次响铃）", minutes, pendingAtMs_);
}

bool Alarm::ringing() const {
  return Ringtone::instance().playing();
}

void Alarm::stopRing() {
  if (ringUntilMs_ > 0 || Ringtone::instance().playing()) {
    Ringtone::instance().stop();
    ringUntilMs_ = 0;
    snprintf(status_, sizeof(status_), "已停止响铃");
    LOGD("PgAlarm: 已停止响铃");
  }
}

void Alarm::setFireCb(FireCb cb) {
  fireCb_ = cb;
}

void Alarm::start() {
  if (started_) return;
  started_ = true;
  load();
  running_ = true;
  pthread_t th;
  if (pthread_create(&th, 0, guardEntry, this) == 0) {
    pthread_detach(th);
  } else {
    running_ = false;
    LOGE("PgAlarm: 守护线程创建失败（闹钟不会响）");
  }
}

}  // namespace pg
