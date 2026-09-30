/*
 * PgNavi.cpp - 全局导航栏的外部状态（实现见 PgNavi.h 的说明）
 *
 * 只有两个进程级标志：标题覆盖 + 视频播放页。**故意做成"哑状态"** ——
 * 这里不做任何判定（不查 Activity、不管显隐），判定全在 navibar.cc 里，
 * 这样"谁在什么时候复位"只有一个地方要读。
 */
#include "platform/PgNavi.h"

#include <stdio.h>
#include <string.h>

#include "utils/Log.h"

namespace pg {

namespace {
const int kTitleMax = 64;
char sNaviTitle[kTitleMax] = {0};
bool sVideoPage = false;
}  // namespace

void setNaviTitle(const char *title) {
  if (!title) title = "";
  /* 幂等：内容没变就不重写（调用方可能在每帧里调） */
  if (strncmp(sNaviTitle, title, sizeof(sNaviTitle) - 1) == 0) return;
  /* ⚠️ 这条日志是**粘性状态排障的唯一抓手**，别删：覆盖是"谁设的、谁清的"跨文件难题，
   *    2026-09-16 验收就靠它定位"世界时钟覆盖被立刻清掉"（见 navibar.cc 的 sOvrAct 说明）。 */
  LOGD("PgNavi: 标题覆盖 '%s' -> '%s'",
       sNaviTitle[0] ? sNaviTitle : "(无)", title[0] ? title : "(无)");
  snprintf(sNaviTitle, sizeof(sNaviTitle), "%s", title);
}

const char *naviTitleOverride() {
  return sNaviTitle[0] ? sNaviTitle : 0;
}

void setVideoPage(bool on) { sVideoPage = on; }

bool videoPage() { return sVideoPage; }

}  // namespace pg
