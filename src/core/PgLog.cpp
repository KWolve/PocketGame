#include "core/PgLog.h"

#include <stdarg.h>
#include <stdio.h>

namespace pg {

namespace {
LogSink gSink = 0;
}

void setLogSink(LogSink sink) { gSink = sink; }

void logInfo(const char *fmt, ...) {
  if (!gSink || !fmt) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  gSink(buf);
}

}  // namespace pg
