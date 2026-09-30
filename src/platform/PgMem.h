/*
 * PgMem.h - 内存小工具（进视频前清页缓存）
 *
 * 本板 **MemTotal 只有 56MB**，播视频（尤其 720p 走 1/2 缩放解码）时非常吃紧：
 * 实测可用内存掉到 2~3MB 时，`zk_h264_player_init` 会失败/进程直接被杀
 * （日志停在 "[HW] 起硬件播放器" 之后，**什么报错都没有**，极难查）。
 *
 * 用户给的解法：进视频前先 `echo 3 > /proc/sys/vm/drop_caches` 把页缓存/slab 清掉。
 * 本工程在**每个进视频的入口**都调一次（`PgStream::startCommon()` 起流前、
 * `H264Player::playFile/startStream()` 起播前）—— 幂等，重复调无害。
 */
#ifndef PG_MEM_H_
#define PG_MEM_H_

namespace pg {

/** 当前 MemAvailable（kB）；< 0 = 读不到 /proc/meminfo */
long long memAvailableKb();

/**
 * 清页缓存 + 可回收 slab（`echo 3 > /proc/sys/vm/drop_caches`）。
 * @return 释放的 kB（< 0 = 前后读数有一次失败）；日志里会打 "可用内存 A -> B kB"
 * ⚠️ 需要 root —— 本应用就是 root（init.rc 里 `service zkswe /bin/zkgui`，user root）
 */
long long dropPageCache();

}  // namespace pg

#endif  // PG_MEM_H_
