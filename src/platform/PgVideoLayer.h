/*
 * PgVideoLayer.h - disp 视频层清理（关闭视频后把硬件层释放掉）
 *
 * ============================ 为什么需要它 ============================
 * 这是**用户给的做法**（2026-09-14），参考工程 `src/system/hardware.cpp` 的
 * `_release_layer()` 同一套：
 *
 *   视频链路（MPP 的 AW_MPI_VO / zk_h264_player 的 hwd_layer）退出后，
 *   **disp 层上的 `enable` 标志不一定被清干净**。层一旦残留 enable，下一轮
 *   再开视频时那块层已经被占着 —— 表现就是"上一轮播过、这一轮不出画"，
 *   而且**日志一切正常**（解码在跑、回调在响），极难从应用侧定位。
 *
 * 做法：直接 ioctl `/dev/disp`，把除 UI 层以外的所有 channel/layer 的
 * `enable` 写回 0。对应 `video/sunxi_display2.h`：
 *
 *   struct disp_layer_config { struct disp_layer_info info; bool enable;
 *                              unsigned int channel; unsigned int layer_id; };
 *   DISP_LAYER_GET_CONFIG = 0x48 / DISP_LAYER_SET_CONFIG = 0x47
 *
 * ioctl 的入参是 `unsigned long args[4]`，`args[1]` 放 config 指针、`args[2] = 1`
 * （这个参数形式出自参考工程，照抄即可 —— 直接用 `_IOWR` 宏编码反而不对）。
 *
 * ============================ ⚠️ 绝不能碰 UI 层 ============================
 * 本板 **UI 层 = channel 2 / layer_id 0**（见 PgStream.cpp 的显示分层注释：
 * "UI 层 disp ch2 z=16 最顶，视频层 disp ch0 在下"）。
 * 把 UI 层的 enable 清掉 = **整个界面立刻消失**。
 * 所以这里**双重保护**：
 *   ① 显式跳过 (ch == 2 && lyl == 0)；
 *   ② 再按像素格式兜底跳过 ARGB 系列（`DISP_FORMAT_ARGB_8888` … `DISP_FORMAT_BGRA_5551`
 *      —— 那是 UI 用的格式，参考工程也是靠这一条过滤的）。
 */
#ifndef PG_VIDEO_LAYER_H_
#define PG_VIDEO_LAYER_H_

namespace pg {

class VideoLayer {
 public:
  /**
   * 关闭除 UI 层以外的所有 disp 层（幂等，可反复调用）。
   * @return 实际被关闭的层数；< 0 = 打不开 /dev/disp（会打日志）
   */
  static int release();

  /** 只统计不修改：返回当前"除 UI 层外仍处于 enable 的层"数量（自检用）。
   *  ⚠️ 只读，不会改任何层的状态。 */
  static int countEnabled();
};

}  // namespace pg

#endif  // PG_VIDEO_LAYER_H_
