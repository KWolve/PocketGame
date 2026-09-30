# disp 视频层释放（releaseLayer）· 实现与实测

> 起因：2026-09-14 用户给出 `releaseLayer()` 代码并指出「**视频打开后关闭需要
> ReleaseLayer**」。参考工程同一套实现在 `src/system/hardware.cpp` 的 `_release_layer()`。
> 本文记录**为什么需要、本工程怎么实现的、以及实测出的关键时序**。

## 0. 一句话结论

**视频层（`ch0/lyl0`，NV12）在流结束后会残留 `enable=1`** —— 层不释放，下一轮再播
就"不出画"，而且**日志一切正常**（解码在跑、回调在响），是本板最难查的一类问题。

**而 `PgStream` 的收尾在 ④StopRecvStream 之后会永久卡死**（本板已知），
所以释放**不能只放在收尾末尾** —— 实测这么放**永远执行不到**。
最终落在**两处**：
1. **`StreamPlayer::startCommon()` 起播前**（不依赖上一轮是否正常结束，幂等）
2. **收尾的 ④ 与 ⑤ 之间**（能执行到的最早位置）

## 1. 为什么需要它（实测复现）

用 QA `streamshow` 播一条在线流，播完/停止后查层：

```
PgVideoLayer: ch0/lyl0 GET r=0 enable=1 fmt=76(NV12) 256x448   ← 残留！
disp 层自检 —— 除 UI 层外仍开着的层数 = 1
```

对比：**空闲时**是 `enable=0`（除 UI 层外 0 个）。

也就是说 `ch0/lyl0` 这个 **VIDEO 层**在流结束后**没有被 MPP 关掉**
（我们自己 DestroyChn 的是 MPP 的 VO 通道，disp 层是另一层东西）。

残留的后果：**下一轮起播复用通道时状态不一致**，实测表现为
`[T3] 建/复用通道前` 之后直接"在线流播放结束" —— 投屏页收了、画面没有。

## 2. 实现：`src/platform/PgVideoLayer.{h,cpp}`

直接 ioctl `/dev/disp`，把除 UI 层外的所有 channel/layer 的 `enable` 写回 0。

```c
struct disp_layer_config { struct disp_layer_info info; bool enable;
                           unsigned int channel; unsigned int layer_id; };
DISP_LAYER_GET_CONFIG = 0x48 / DISP_LAYER_SET_CONFIG = 0x47
```

ioctl 入参形式（**照抄参考工程，别改**）：

```c
unsigned long args[4] = {0};
args[1] = (unsigned long)cfg;   // config 指针
args[2] = 1;
ioctl(fd, cmd, args);
```

> ⚠️ 不要改成 `_IOWR('D', cmd, ...)` 那种标准编码 —— sunxi disp 驱动认的是这个形式。

### 2.1 头文件依赖（踩坑）

`sunxi_display2.h` 在 **toolchain sysroot** 里：
`D:/zkswe/fun/toolchains/v85x/arm-unknown-linux-musleabihf/sysroot/usr/include/video/sunxi_display2.h`

但它用的是 SDK 的 `u32` / `s32`，而 sysroot 里**没有 `typedef.h`** —— 直接引会报一片
`'u32' has not been declared`。**参考工程是自带一份 `src/dependencies/include/typedef.h`**，
本工程照做（已拷贝进来）。include 顺序不能反：

```c
extern "C" {
#include <typedef.h>               // 必须先
#include <video/sunxi_display2.h>  // 后
}
```

### 2.2 ⚠️ UI 层绝不能碰（双重保护）

本板 **UI 层 = `channel 2 / layer_id 0`**（见 `PgStream.cpp` 的分层注释）。
清掉它 = **整个界面立刻消失**。所以：

1. **硬规则**：显式跳过 `(ch == 2 && lyl == 0)`；
2. **格式兜底**：再跳过 ARGB 系列（`DISP_FORMAT_ARGB_8888` … `DISP_FORMAT_BGRA_5551`）
   —— 参考工程只靠这一条过滤。实测本板 UI 层是 `fmt=0 (ARGB8888) 480x800`。

**实测确认没有误伤**：释放日志是 `已释放 1 个视频层（跳过 0 个 UI 类格式层）`，
UI 层（`ch2/lyl0 enable=1`）从头到尾没被写过。

### 2.3 ★ 程序启动时释放（2026-09-14，用户要求）

**`src/Main.cpp` 的 `onEasyUIInit()` 第一件事就是 `pg::VideoLayer::release()`。**

原因：zkgui 由 init 托管（`/etc/init.rc` 里 `service zkswe /bin/zkgui`，非 oneshot）
⇒ 崩溃/被 kill 后**进程立刻重启，但 disp 层的 enable 是内核态的，不随进程消失**。
实测（本次）：播流中 `kill -9` 后 pid 846→1061，`ch0/lyl0`（NV12 256x448）**照样 enable**，
屏幕就冻在上一轮的画面上（fb0 内容 6 秒以上不变）。启动时清一次，重启即恢复。

不依赖"上一轮怎么结束"（正常停 / 卡死 / 被杀），且幂等 —— 干净时日志是
`启动图层释放 -> 清掉 0 个残留视频层`。

## 3. ★ 关键时序：必须放在"收尾卡死点之前"

本工程 `PgStream` 收尾的最后几步是：

```
收尾 ①SetStreamEof → ①b/①c 排空 → ②抽干解码输出 → ③StopRecvStream → ④StopRecvStream
   → ⑤通道处理（abortedMidway 就"保持不动"）   ← ★ 本板在这里**永久卡住**
   → 收尾完成
```

实测「自然播完」的流也会卡在 ⑤：日志停在 `收尾 ④StopRecvStream ok`，
然后 40 秒后看门狗报 `任务已 40s 无进展（卡在开流/建链路/收尾）→ 对外视为已结束`。

**推论**：把 `VideoLayer::release()` 放在 `收尾完成` 之前 = **永远执行不到**。
实测确实如此（第一版就是放在末尾，日志里连一次"已释放"都没有）。

**最终方案（两处）**：

| 位置 | 作用 |
|---|---|
| `StreamPlayer::startCommon()` 开头（起播前） | **不依赖上一轮怎么结束**（正常停/卡死/被杀），这一轮都从干净状态开始。幂等、无副作用。 |
| 收尾 `④StopRecvStream ok` 之后、`⑤` 之前 | 正常收尾时顺手释放，让层尽早干净。 |

（`PgH264::stop()` 里也调了一次 —— zk_h264_player 那条路有独立的 deinit。）

## 4. 实测

**播放中**（`ch0/lyl0` 被 VIDEO 层占用）：

```
PgVideoLayer: ch0/lyl0 GET r=0 enable=1 fmt=76(NV12) 256x448
PgVideoLayer: ch2/lyl0 GET r=0 enable=1 fmt=0(ARGB8888) 480x800  ← UI 层(跳过)
disp 层自检 —— 除 UI 层外仍开着的层数 = 1
```

**修复后停流**：

```
PgVideoLayer: 关闭层 ch0/lyl0（fmt=NV12 256x448）      ← 起播前清残留
PgStream: 收尾 ④StopRecvStream ok
PgVideoLayer: 关闭层 ch0/lyl0（fmt=NV12 256x448）      ← 收尾中释放（关键）
PgVideoLayer: ch0/lyl0 GET r=0 enable=0 fmt=0(ARGB8888) 0x0
disp 层自检 —— 除 UI 层外仍开着的层数 = 0              ← 干净了
```

> ⚠️ 查层状态要**在播放稳定之后**（`zk_h264_player` 的层是异步建的，
> 刚 `h264play` 完立刻 `layerstat` 会读到 0，误以为"没层"）。

## 5. 一个悬而未决的点

**`zk_h264_player` 播放时，`ch0..3 × lyl0..3` 里读不到它的层**（实测 16 个槽位全
`enable=0`，只有 UI 层开着）—— 说明 `libhwdisplay` 的 `hwd_layer` 用的是**另一套层**
（或 `disp_layer_config2`）。所以本模块对 zk_h264_player 这条路**目前是兜底性质**
（它自己的 `deinit()` 会处理）。

**如果后续发现 zk_h264_player 也会残留层**，需要再查它的层号（可从
`feat`/`DISP_LAYER_GET_CONFIG2` 或 `hwd_layer` 的日志入手）。

## 6. 命令速查（QA）

| 命令 | 作用 |
|---|---|
| `layerstat` | 只统计（**不改**）：逐层打印 ch/lyl 的 `enable`/格式/尺寸，并汇总"除 UI 层外仍开着的层数" |
| `layerfree` | 强制释放一遍（视频异常退出后的手动兜底） |

`layerstat` 的详细日志是这块唯一的"眼睛"（层状态**没有 sysfs 可查**），所以它常开。
