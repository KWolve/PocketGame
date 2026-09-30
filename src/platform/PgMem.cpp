/*
 * PgMem.cpp - 内存小工具实现。设计说明见 PgMem.h。
 */
#include "platform/PgMem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/Log.h"

namespace pg {

long long memAvailableKb() {
  FILE *f = fopen("/proc/meminfo", "rb");
  if (!f) return -1;
  char line[128];
  long long kb = -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemAvailable:", 13) == 0) {
      kb = atoll(line + 13);
      break;
    }
  }
  fclose(f);
  return kb;
}

long long dropPageCache() {
  long long before = memAvailableKb();
  int rc = system("echo 3 > /proc/sys/vm/drop_caches");
  long long after = memAvailableKb();
  long long delta = (before >= 0 && after >= 0) ? (after - before) : -1;
  LOGD("PgMem: drop_caches(3) rc=%d —— 可用内存 %lld -> %lld kB（%+lld）", rc, before, after, delta);
  return delta;
}

}  // namespace pg
