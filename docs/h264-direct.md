# 硬件 H264 直解（zk_h264_player）· 打通记录

> 起因：2026-09-14 用户指路「ts 流出来后有 **zk_h264player** 的组件包。或者看参考代码里面
> 有解码视频流的。这个硬件只能解码 H264。」
> 目的：IPTV 卡在 **960x544 硬解上限**（见 `docs/iptv.md` §3），而公开源绝大多数是 720p。
> 本文记录把这条路彻底打通并**实测 720p 可用**的过程。

## 0. 一句话结论

**`zk_h264_player` 在本板完全可用，而且能解 720p** —— 关键靠
`E_H264_PLAYER_FLAG_SCALE_DOWN_2 / _4` 缩放解码。实测三档数据：

| 配置 | 解码输出 | MemFree | MemAvailable |
|---|---|---|---|
| 1280x720 不缩放 | 1280x736 | 1252 kB | **2652 kB** ⚠️ 危险 |
| 1280x720 **1/2** | 640x384 | 1480 kB | **4148 kB** ⭐ 推荐 |
| 1280x720 **1/4** | 320x192 | 2880 kB | **6500 kB** ✅ 最安全 |

对比原来的 MPP 路线（`PgStream`，`kMaxDecodePixels = 960*544`）**720p 直接被拒**，
这是**绕开上限的正路**。480x800 屏上 1/2 缩放（640x360）已经比屏幕还大，完全够用。

## 1. 它到底是什么（三层结构）

```
zk_h264_player_*            ← 我们调用（头文件 src/dependencies/include/h264_player.h）
      ↑ 实现是 **dlopen wrapper**：src/dependencies/lib/libzkmedia.a 里的 h264_player.o
      ↓ dlopen("libawh264player.so") + dlsym
h264_player_*               ← 真实现：src/dependencies/lib-no-link/libawh264player.so
      ↓ NEEDED（动态依赖）
libvdecoder / libVE / libhwdisplay / libMemAdapter /
libcdc_base / libcdx_base / libcdx_common / libvideoengine / libawh264
      ↑ **本板设备上全在 `/lib/eyesee-mpp/`**（实测 ls 逐个确认）
```

**关键结论：依赖问题不存在。** 设备的 `/etc/ld-musl-armhf.path` 内容就是

```
/tmp:/res/lib:/res/zkswe:/lib:/lib/eyesee-mpp:/late/lib:/late/lib/eyesee-mpp
```

`/lib/eyesee-mpp` 已经在搜索路径里 ⇒ **不需要任何 `LD_LIBRARY_PATH` 手脚**。
（init.rc 里另有 `export LD_LIBRARY_PATH /data:/tmp:/res/lib:/res/zkswe:/lib:/lib/eyesee-mpp`
—— 含 `/data` 与 `/tmp`，所以把 `libawh264player.so` 丢这两个目录也能被 dlopen 到。）

## 2. ⚠️ 部署：`lib-no-link` 不随 `fun launch` 推送

官方 wiki（`manifest/add_local_lib.md`）原文：

> 如果存在 `dependencies/lib-no-link` 文件夹，该文件夹下的动态链接库**仅随程序打包，
> 不参与编译**。

实测确认：把 `libawh264player.so` 放进 `src/dependencies/lib-no-link/` 后
`fun launch` 的推送列表里**没有它**（只有 `libzkgui.so` / `font` / `EasyUI.cfg` /
`ui/*.ftu`）。

**开发期要手动放**（选一个 ld 搜索路径里的目录）：

```bash
adb push libawh264player.so /tmp/            # 或 /data（重启不丢，但 /data 只有 832K 总空间）
```

**固化时**它应该随升级包一起走（"仅随程序打包"指的就是这个），落到设备的
`/res/...` 或 `/lib/...`。这条**待固化时验证**。

## 3. API 语义（照参考工程 `src/link/context.cpp` 校准）

```c
zk_h264_player_preload();                        // 预载（内部起线程做 dlopen）
int  r = zk_h264_player_init(srcW, srcH, rot, flag);   // ⚠️ w/h 是**源分辨率**，不是显示区
zk_h264_player_set_decode_cb(cb);                // 解码帧回调（init 之后注册）
zk_h264_player_set_pos(x, y, w, h);              // 屏幕上显示区域
zk_h264_player_show() / hide();
zk_h264_player_put_frame(data, size);            // 喂 H264 Annex-B 数据
zk_h264_player_deinit();
```

`flag`（`enum h264_player_flag_e`）：

| 值 | 含义 |
|---|---|
| `0x01` | `E_H264_PLAYER_FLAG_STREAM_EOF`（参考工程当"默认值"用） |
| `0x02` | `E_H264_PLAYER_FLAG_DISP_UNCACHE` ← ✗ **本板已放弃**，见下 |
| `0x10` | `E_H264_PLAYER_FLAG_SCALE_DOWN_2` ← **1/2 缩放解码** |
| `0x20` | `E_H264_PLAYER_FLAG_SCALE_DOWN_4` ← **1/4 缩放解码** |

> ✗✗ **`0x02 DISP_UNCACHE` 不能用**（2026-09-14 实测定案）：它让 **VDEC 不经 DISP 直出显示器**，
> 省下那份显示缓冲（实测 MemAvailable +5.8MB / 进程 RSS −4.2MB），但**旋转正是 DISP 做的** ⇒
> **不走 DISP 就没有旋转**。本板 480x800 竖屏、横屏源必须转 90°，所以无退路（`rot` 传 init
> 也救不回来，AirPlay 那个写法在本板同样只填 y≈417、下方 40% 全黑）。
> 完整数据与判据见 `docs/kb-v85x-h264-player.md` 的 flag 一节。

`rot` 用 `enum disp_rot_e`：`E_DISP_ROT_0/90/180/270`（顺时针）。

环境变量（参考工程 `audio_context.cpp` 的 `init()` 里设的）：
`ZKMEDIA_H264_VBVSIZE`（码流缓冲，参考工程设 `1048576`）、`ZKMEDIA_H264_LAYER`（显示层号）。

### ★★ `ZKMEDIA_H264_VBVSIZE` 必须设（2026-09-14 定位的静默崩溃）

**症状**：720p 源（1/2 缩放解码）+ 90° 旋转起播时，zkgui **当场静默退出**——
没有任何日志/报错（stderr 指向 `/dev/null`），日志停在 `[HW] 起硬件播放器：源 1280x720，
旋转 90°，缩放解码 1/2` 那一行之后，init 的返回日志都来不及打。

**根因**：本工程原来**一个 zkmedia 环境变量都没设**，库用的是默认的小 VBV；
720p 的 I 帧几百 KB，喂进太小的缓冲就出事。参考工程一直设着 1MB。

**做法**（`PgH264.cpp` 的 `ensureZkmediaEnv()`，在第一次 `zk_h264_player_preload()`
之前调用 —— 库是在 dlopen 时读环境变量的）：

```c
setenv("ZKMEDIA_H264_VBVSIZE", "1048576", 0);   // 0 = 外部已设的不覆盖，方便现场调参
```

**修复后**：同一条 720p 流（萧山 1.5Mbps / CCTV-1 1.2Mbps，rot=90，1/2 缩放）
`解码回调 #240 fmt=5 640x384`、`已喂 300 包解码出帧 252 丢 0`、pid 全程不变。

> 排查时排掉的假设（省得再试）：内存不足（drop_caches 腾到 26.7MB 照样崩）、
> init 里传旋转、crop 发在 set_rot 之前、旋转与缩放"组合不支持"（960x540 不缩放 + rot90 正常）。

### ★ 修正：`rot` **可以**放在 init 里（2026-09-14 干净开机受控 A/B）

当年先怀疑"旋转不能进 init"，是因为**没有先查环境变量**。补做单变量 A/B（同一条 720p 流、
开机后单次受控、每次跑满 36 秒）：

| 组 | init 传参 | 结果 |
|---|---|---|
| A | `init(rot=0)` + 起播后 `set_rot(90)` | 0 次重启、解码回调 **#900**、丢 0 ✅ |
| B | `init(rot=90)`（不再 set_rot） | 0 次重启、解码回调 **#900**、丢 0 ✅ |

⇒ **两种都行**。本工程统一用 A，只是为了与参考工程 `context.cpp`、文件播放 `h264play`
的顺序一致，**不是硬性限制**。

**真正会让起播"静默退出"的两件事，按顺序查**：

1. **`ZKMEDIA_H264_VBVSIZE` 没设**（库默认 VBV 装不下 720p 的 I 帧）—— 本节上面那条；
2. **可用内存 < 3MB**（`MemAvailable`；被 OOM 杀过的进程内存不会自己回来，**要重启板子**）。

> 诊断口诀：起播瞬间"无任何日志就没了" ⇒ **先查环境变量与内存**，再怀疑参数组合。
> 另外：打日志时**必须打印实际传进去的参数**（`.desc` 那句曾硬编码 `rot=0`，白跑一轮 A/B）。


## 4. 本工程实现：`src/platform/PgH264.{h,cpp}`

当前定位是**验证/调试用**（`h264play <file> <srcW> <srcH> [scale]`），
把 H264 ES 文件按 **AUD（nal_type=9）切 access unit** 后逐帧喂进去，后台线程节流约 30fps。

**两个踩过的坑（都在代码注释里留了）**：

1. **`get_picture_count()` 不能用来判断"解码器有没有出画"** —— 它更像"已提交帧数"，
   喂多少就报多少，哪怕显示线程一帧没取走。**唯一硬证据是解码回调**
   （`set_decode_cb`）有没有响。加上回调后立刻看到 `解码回调 #60 —— fmt=5 448x256`。
2. **背压的 guard 别开太大**：原来 `guard<100 × 10ms` = 每帧最多等 1 秒，而缓冲长期在阈值之上
   ⇒ **每帧都等满 1 秒**，看起来像"喂不动了"。改成 `30 × 5ms`。

## 5. ★ 顺带修掉的一个跨模块 bug

`h264play` 起来后画面全黑、帧全堆在解码器里。根因在 `mainLogic` 主循环：

```cpp
} else if (gStreamPageShown && !pg::StreamPlayer::running()) {
    hideCastPageForStream();     // ← 把投屏页收掉了
```

**因为 zk_h264_player 这条路不用 `StreamPlayer`**，只判 `StreamPlayer::running()` 会在
`h264play` 刚起来时**误判成"流已结束"**，把投屏页（= 视频层唯一能透出的透明窗口）收掉。
**修**：判断里加上 `&& !pg::H264Player::running()`。

## 6. 实测证据（2026-09-14 真机 · 设备 20080411）

```
PgH264: zk_h264_player_init(426,240,rot=0,flag=0x1) -> 0        ← dlopen + init 成功
awh264player: display thread start / decode thread start
awh264player: vdec create buffer end
awh264player: h264 vp crop(0, 0, 426, 240)
PgH264: 解码回调 #60 —— fmt=5 448x256 crop(0,0,426,240) data0=0x41610000
```

进程内加载到的库（`/proc/<pid>/maps` 过滤）：

```
libMemAdapter.so  libVE.so  libawh264.so  libawh264player.so  libawlog.so
libcdc_base.so  libcdx_base.so  libcdx_common.so  libhwdisplay.so
libvdecoder.so  libvideoengine.so
```

720p 三档（同一段 `/tmp/test_hd.h264`，1.5MB，含 SPS/PPS/IDR）：

```
不缩放: h264 vp crop(0,0,1280,720) + 解码回调 1280x736  → MemAvailable 2652 kB
1/2   : h264 vp crop(0,0,640,360)  + 解码回调 640x384   → MemAvailable 4148 kB
1/4   : h264 vp crop(0,0,320,180)  + 解码回调 320x192   → MemAvailable 6500 kB
```

停流后内存回到 8196 kB 可用。

## 7. 测试素材怎么来的：`tools/ts2h264.py`

要从 IPTV 的 HLS 流里拿到"没被解码"的 H264 ES 才能做上面这些对照实验。
`tools/ts2h264.py` 干这件事：解析 PAT → PMT → 视频 PID，剥掉 PES 头，把 ES 顺序写出。

```bash
python tools/ts2h264.py -i seg.ts -o out.h264
python tools/ts2h264.py -i https://host/x.m3u8 -o out.h264 --seconds 20
```

**写这个脚本踩的两个 TS 解析坑**（都写进注释了）：

1. **PSI section 前面有 `pointer_field`**（`payload_unit_start_indicator=1` 时），
   必须先跳过 1 字节再从 `table_id` 读。不跳的话整段偏移错 1 字节 —— 症状是
   "PAT 解出来的 PMT PID 是个不存在的值（0x10E2），真实视频 PID 永远找不到"。
2. **PMT 里要跳过 `program_info` 描述符**（`off = 12 + program_info_length`，
   不是 `off = 12`）。不跳会在描述符里乱读 —— 症状是"解出 `stream_type=0x25`、
   PID 等于 PMT 自己"这种明显不合理的值。

脚本还会统计 NAL 类型，并**警告"没有 IDR / 没有 SPS"**（解码器起播必须有这两个）。

## 8. 下一步（若要把它接进 IPTV）

当前 PgH264 只吃**本地 ES 文件**。要让 IPTV 的 720p 频道真正可播，需要补一层
"TS → H264 ES"的在线拆包：

```
PgHls（已有：拉 m3u8 分片 + 本地中继）
  → ffmpeg(mpegts demux，只解封装不解码) 抽出 H264 packet
  → zk_h264_player_put_frame()
```

也就是把 `PgStream` 里"demux 之后送 MPP 解码"改成"demux 之后送 zk_h264_player"，
作为 **MPP 路线的备选后端**（`PgStream` 的 demux/音频/背压/日志都可以复用）。

**触发策略建议**：频道分辨率 ≤ 960x544 走现有 MPP 路线（已验证、有旋转/自愈等成熟机制）；
超了就走 zk_h264_player + 1/2 缩放，并在选台页标注"缩放播放"。

## 9. 命令速查（QA）

| 命令 | 作用 |
|---|---|
| `h264play <文件> <源宽> <源高> [缩放0/2/4]` | 播一段 H264 ES（例：`h264play /tmp/test_hd.h264 1280 720 2`） |
| `h264stat` | 状态：running / 已喂帧 / **解码出帧** / 解码器缓冲 / 需关键帧 / init 参数 |
| `h264stop` | 停止（会自动 destroy buffer、退 display/decode 线程） |
| `vbv` | 打印当前 `ZKMEDIA_H264_VBVSIZE`（默认 1MB，存档值） |
| `vbv <字节>` | 改码流缓冲并**落盘**（64KB~64MB）。⚠️ 需**重启应用**才生效（库在首次起播时 dlopen 并读环境变量）。高码率 720p 起播静默退出时往上调 —— 参考工程留了 2MB / 3MB 的档位 |

⚠️ 与 `StreamPlayer` **互斥**（同一颗 VE、同一个 disp 视频层）—— 起之前先 `streamstop`。
