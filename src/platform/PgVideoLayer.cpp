/*
 * PgVideoLayer.cpp - disp 视频层清理实现。设计说明见 PgVideoLayer.h。
 */
#include "platform/PgVideoLayer.h"

#ifdef __PLATFORM_V85X__

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
/* ⚠️ 顺序不能反：sunxi_display2.h 用的是 SDK 的 u32/s32，而 sysroot 里**没有**
 * `typedef.h`（它在参考工程里是随工程带的）—— 所以先把本工程那份 include 进来。
 * 不引它的话直接报一片 `'u32' has not been declared`。 */
#include <typedef.h>
#include <video/sunxi_display2.h>   // disp_layer_config / DISP_LAYER_GET|SET_CONFIG
}

#include "utils/Log.h"

namespace pg {

namespace {

/* 本板的层布局（与 PgStream.cpp 的显示分层注释一致）：
 *   UI 层     = channel 2 / layer_id 0   ← **绝不能动**
 *   视频层    = channel 0                ← 我们要清理的就是它这类
 * 通道/层数按 SDK 惯例给 4/4（参考工程同值）。 */
const int kChnNum = 4;
const int kLylNum = 4;
const int kUiChn = 2;
const int kUiLyl = 0;

int layerConfig(int fd, int cmd, disp_layer_config *cfg) {
  /* ⚠️ ioctl 的入参形式照抄参考工程：args[1] = config 指针、args[2] = 1。
   * 不要改成 _IOWR('D', cmd, ...) 那种编码 —— sunxi disp 驱动认的是这个形式。 */
  unsigned long args[4] = {0};
  args[1] = (unsigned long)cfg;
  args[2] = 1;
  return ioctl(fd, cmd, args);
}

const char *fmtName(int fmt) {
  switch (fmt) {
    case DISP_FORMAT_ARGB_8888: return "ARGB8888";
    case DISP_FORMAT_ABGR_8888: return "ABGR8888";
    case DISP_FORMAT_RGBA_8888: return "RGBA8888";
    case DISP_FORMAT_BGRA_8888: return "BGRA8888";
    case DISP_FORMAT_RGB_888:   return "RGB888";
    case DISP_FORMAT_YUV444_I_AYUV: return "AYUV444";
    case DISP_FORMAT_YUV420_P:  return "YUV420P";
    case DISP_FORMAT_YUV420_SP_UVUV: return "NV12";
    case DISP_FORMAT_YUV420_SP_VUVU: return "NV21";
    default: return "?";
  }
}

/* UI 层用的都是 ARGB 系列（本板 fb0 是 BGRA/ARGB 8888）。
 * 除了 (ch2,lyl0) 那条硬规则，再加一层格式兜底 —— 参考工程就只靠这条。 */
bool isUiLikeFormat(int fmt) {
  return fmt >= DISP_FORMAT_ARGB_8888 && fmt <= DISP_FORMAT_BGRA_5551;
}

}  // namespace

int VideoLayer::countEnabled() {
  int fd = open("/dev/disp", O_RDWR);
  if (fd < 0) {
    LOGE("PgVideoLayer: 打开 /dev/disp 失败（%s）", strerror(errno));
    return -1;
  }
  int n = 0;
  for (int ch = 0; ch < kChnNum; ++ch) {
    for (int lyl = 0; lyl < kLylNum; ++lyl) {
      disp_layer_config config;
      memset(&config, 0, sizeof(config));
      config.channel = (unsigned int)ch;
      config.layer_id = (unsigned int)lyl;
      int r = layerConfig(fd, DISP_LAYER_GET_CONFIG, &config);
      /* ⚠️ 详细日志常开：这一块是"看不见"的（层状态没有 sysfs 可查），
       * 出了问题只能靠它。成功/失败都打，perror 给 errno。 */
      LOGD("PgVideoLayer: ch%d/lyl%d GET r=%d enable=%d fmt=%d(%s) %dx%d%s", ch, lyl, r,
           (int)config.enable, config.info.fb.format, fmtName(config.info.fb.format),
           config.info.fb.size[0].width, config.info.fb.size[0].height,
           (ch == kUiChn && lyl == kUiLyl) ? "  ← UI 层(跳过)" : "");
      if (r != 0) continue;
      if (ch == kUiChn && lyl == kUiLyl) continue;
      if (!config.enable) continue;
      if (isUiLikeFormat(config.info.fb.format)) continue;
      ++n;
    }
  }
  close(fd);
  return n;
}

int VideoLayer::release() {
  int fd = open("/dev/disp", O_RDWR);
  if (fd < 0) {
    LOGE("PgVideoLayer: 打开 /dev/disp 失败（%s）", strerror(errno));
    return -1;
  }

  int closed = 0;
  int touchedUi = 0;
  for (int ch = 0; ch < kChnNum; ++ch) {
    for (int lyl = 0; lyl < kLylNum; ++lyl) {
      /* ① 硬规则：UI 层（ch2/lyl0）直接跳过 —— 清了它整个界面就没了 */
      if (ch == kUiChn && lyl == kUiLyl) continue;

      disp_layer_config config;
      memset(&config, 0, sizeof(config));
      config.channel = (unsigned int)ch;
      config.layer_id = (unsigned int)lyl;
      if (layerConfig(fd, DISP_LAYER_GET_CONFIG, &config) != 0) continue;
      if (!config.enable) continue;

      /* ② 格式兜底：ARGB 系列当作 UI 层，不碰 */
      if (isUiLikeFormat(config.info.fb.format)) {
        ++touchedUi;
        continue;
      }

      config.enable = 0;
      if (layerConfig(fd, DISP_LAYER_SET_CONFIG, &config) == 0) {
        LOGD("PgVideoLayer: 关闭层 ch%d/lyl%d（fmt=%s %dx%d）", ch, lyl,
             fmtName(config.info.fb.format), config.info.fb.size[0].width, config.info.fb.size[0].height);
        ++closed;
      } else {
        LOGW("PgVideoLayer: 关闭层 ch%d/lyl%d 失败（%s）", ch, lyl, strerror(errno));
      }
    }
  }
  close(fd);
  if (closed || touchedUi) {
    LOGD("PgVideoLayer: ★ 已释放 %d 个视频层（跳过 %d 个 UI 类格式层）", closed, touchedUi);
  }
  return closed;
}

}  // namespace pg

#else   // ---------- 非 V85X（PC hosttest 等）：空实现 ----------

#include "utils/Log.h"

namespace pg {
int VideoLayer::release() { return 0; }
int VideoLayer::countEnabled() { return 0; }
}  // namespace pg

#endif  // __PLATFORM_V85X__
