#pragma once

/* 抓帧（QA 用）—— 把"真正播出去的那一帧"搬出来。
 *
 * 为什么需要它：**视频层不在 `/dev/fb0` 里**（fb0 只是 UI 层 disp ch[2]），
 * 截图 / `flythings_device_screenshot` 都抓不到播出来的画面 ⇒ 想看画面只能抓帧。
 *
 * ★ 必须**两条解码路都能用**（血案：只在通用路挂钩子，而实际在跑的是 HW 路，
 *   于是 `camgrab`/`streamgrab` 打了"已安排抓帧"日志却永远不落盘 = 静默失败）：
 *     · HW 路   —— `PgH264Player` 的解码回调（onDecodedFrame，**实际在跑的那条**）
 *     · 通用路 —— `PgStream` 自己 `VO_SendFrame` 前
 *   两边都调 `pg::grabTake()`，谁先把帧数跑到目标谁落盘，只抓一帧。
 *
 * 实现在 `PgStream.cpp`（`sGrabAtFrame` / PGM 写盘都归它管）。
 */
namespace pg {

/* 每解出一帧调一次：该抓就返回 true，并把请求**消费掉**（只抓一帧）。
 * 调用方拿到 true 后接着调 `grabWritePlane()` 落盘。 */
bool grabTake(int frameNo);

/* Y 平面落盘成 PGM（P5 灰度，PC 侧 / PIL 直接可读）。
 * `stride` 传 0 = 按 `w` 紧凑排（解出来的缓冲通常按对齐宽度排，所以要传真实 stride/宽）。
 * `tag` 只进日志，用来区分是哪条路抓的。 */
void grabWritePlane(const unsigned char *y, int w, int h, unsigned stride, const char *tag);

/* PGM 输出路径（日志用） */
const char *grabPath();

}  // namespace pg
