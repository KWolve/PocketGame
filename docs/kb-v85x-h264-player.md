# V85X 硬件 H264 播放器（awh264player / zk_h264_player）用法与避坑

> 🔍 **检索导引**：V85X/V553/V853「**硬件 H264 解码**」「**zk_h264_player**」「**awh264player 包怎么用**」
> 「**HLS/RTSP/TS 流送硬解上屏**」「**视频旋转/缩放/裁剪（disp 硬层）**」「**720p 解码**」
> 「**起播就静默退出 / 进程无报错消失**」「**get_picture_count 不准**」问题。
> 💡 本包**只解 H264**；先分割封装（TS/mp4 → H264 Annex-B）再喂帧。
> 💡 与 MPP（`libmedia_mpp` / `AW_MPI_VDEC_*`）是**两条独立链路**，同一颗 VE、同一个 disp 视频层，**互斥**。
> 💡 显示层结构（UI 层 / 视频层 / videoView 透明窗口）见 `v85x/videoview-transparent-window.md`；
>    「错屏 / 无图像 / 回放方向」四板斧见 `v85x/display-layer-debug.md`。
> 定规：**只记录怎么用**（内部实现闭源，不解析不深挖）。来源：V851s 真机实测（V853/V553 同平台）。

---

## 0. 一句话

`zk_h264_player_*` 是 **dlopen wrapper**：静态库提供符号、运行时 `dlopen("libawh264player.so")`
调真实现。**能解 H264，支持硬件旋转/缩放/裁剪，还敢解 720p**（靠 `SCALE_DOWN_2/4`）。

**最少可用序列**（照抄即可）：

```c
setenv("ZKMEDIA_H264_VBVSIZE", "1048576", 0);   // ★★ 必须在 dlopen 之前（见 §5.1）
zk_h264_player_preload();                        // 内部起线程做 dlopen
int r = zk_h264_player_init(srcW, srcH, E_DISP_ROT_0, flag);   // w/h = **源**分辨率
zk_h264_player_set_decode_cb(on_frame);          // 判定"真的出画了"的唯一硬证据
zk_h264_player_set_pos(x, y, w, h);              // 屏幕上显示区域
zk_h264_player_show();
/* 之后持续喂： */
zk_h264_player_put_frame(es_data, es_size);      // H264 Annex-B（含 SPS/PPS 的 IDR 起播）
/* 收尾： */
zk_h264_player_hide();
zk_h264_player_deinit();                         // 自动退 display/decode 线程
```

---

## 1. 包与门面（三层，一看就懂）

```
zk_h264_player_*   ← 应用调用（头文件 h264_player.h）
      ↑ 与平台无关的 dlopen wrapper（静态库里的 h264_player.o，随工程链接）
h264_player_*      ← 真实现 libawh264player.so（dlopen + dlsym）
      ↓ NEEDED
libvdecoder/libVE/libhwdisplay/libMemAdapter/libcdc_base/libcdx_base/libcdx_common/
libvideoengine/libawh264/libawlog  ← **设备自带**，V85X 上在 /lib/eyesee-mpp/
```

**依赖不需要你操心**：设备 `/etc/ld-musl-armhf.path` 已含 `/lib:/lib/eyesee-mpp`；
init.rc 的 `LD_LIBRARY_PATH` 还额外含 `/data:/tmp:/res/lib:/res/zkswe`。

| 平台 | 包名 | 可用版本（离线目录） | 备注 |
|---|---|---|---|
| V85X / V85X-eMMC | `awh264player` | `1.0.0` | 目录里 description 为空 —— 用法看本篇 |
| F133 | `awh264player` | `1.1.0` | |
| F136 | `awh264player` | `1.1.0` / `1.0.1` / `1.0.0` | |

> 目录里没有描述、API 也查不到（`get_package_api` 返回空）⇒ **以本篇 + 头文件为准**。
> 头文件 `h264_player.h` 在工程的 `src/dependencies/include/`（官方包形式则在包的 include/ 下）。

### 1.1 两种接入方式 + 部署的坑

| 方式 | 做法 | 注意 |
|---|---|---|
| **A. 官方包**（推荐） | `Manifest.xml` 加 `<package id="awh264player" version="^1.0.0"/>` → `fun install` 拉依赖 | 版本以 `package_search` / `query_package` 现场查询为准 |
| **B. 本地库** | 头文件放 `src/dependencies/include/`、真实现 `.so` 放 **`src/dependencies/lib-no-link/`**、门面静态库放 `dependencies/lib/` | 见下 |

⚠️ `lib-no-link/` 里的 `.so` **只随"打包"走**：不参与编译，**也不随 `fun launch` 推送**。
调试期必须手动补到 ld 搜索路径里（`adb push libawh264player.so /tmp/`），
否则表现是 `init 失败 / dlopen 失败`；**固化打包时它会自动进包**（落到设备 `/res/lib/`）。
判据：`ls /res/lib/libawh264player.so` 在（约 21KB 级），`/proc/<pid>/maps` 里有它。


---

## 2. API 全表（头文件原样，共 14 个）

```c
void zk_h264_player_preload(void);
int  zk_h264_player_init(int w, int h, enum disp_rot_e rot, int flag);   // 返回 0 = 成功
void zk_h264_player_deinit(void);
void zk_h264_player_set_decode_cb(h264_decode_frame_cb cb);
void zk_h264_player_flush(void);
void zk_h264_player_show(void);
void zk_h264_player_hide(void);
void zk_h264_player_set_mirror(int mirror);
void zk_h264_player_set_rot(enum disp_rot_e rot);
void zk_h264_player_set_pos(int x, int y, int w, int h);      // 显示区域（屏幕坐标）
void zk_h264_player_set_crop(int x, int y, int w, int h);     // 裁剪（**旋转后**坐标系）
void zk_h264_player_put_frame(uint8_t *data, uint32_t size);  // 喂 Annex-B
int  zk_h264_player_get_picture_count(void);
int  zk_h264_player_need_iframe(void);                        // 1 = 当前需要关键帧
```

枚举与回调：

```c
enum disp_rot_e { E_DISP_ROT_0, E_DISP_ROT_90, E_DISP_ROT_180, E_DISP_ROT_270 };  // 顺时针

enum h264_player_flag_e {
  E_H264_PLAYER_FLAG_STREAM_EOF    = 0x01,   // 流模式（参考工程当默认值用）
  E_H264_PLAYER_FLAG_DISP_UNCACHE  = 0x02,   // ✗ 见下面警告：绕开 DISP ⇒ 没有旋转
  E_H264_PLAYER_FLAG_SCALE_DOWN_2  = 0x10,   // ★ 缩放解码 1/2
  E_H264_PLAYER_FLAG_SCALE_DOWN_4  = 0x20,   // ★ 缩放解码 1/4
};

typedef struct {                    // 解码回调给的帧
  int fmt;                          // 实测 = 5（半平面 YUV，配 data0/data1 两平面使用）
  int width, height;                // 解码缓冲尺寸（**可能 = 源高 +16 对齐**，别当显示尺寸）
  int left, top, right, bottom;      // **有效裁剪区**（crop）
  uint8_t *data0, *data1, *data2;
} h264_decode_frame_t;
```

> ⚠️ 回调里的 `width/height` 是**解码缓冲尺寸**，`left/top/right/bottom` 才是有效画面
> （例：源 426x240 → 缓冲 448x256、crop(0,0,426,240)）。要拿尺寸请用 **crop 区**。
> 本包里 `fmt=5` 的两平面**不能当 I420 解释**（照 I420 转帧会出色度错乱）。

`flag` 可组合，例如 720p 播 1/2：`E_H264_PLAYER_FLAG_STREAM_EOF | E_H264_PLAYER_FLAG_SCALE_DOWN_2`
（实测 `flag=0x11` 就是这一组；1080p 走 1/4 则 `0x21`）。

### ⚠️⚠️ `E_H264_PLAYER_FLAG_DISP_UNCACHE`（0x02）：**要省内存就别要旋转**（实测定案）

**机制**：UNCACHE = **VDEC 不经 DISP 直出到显示器**（绕开 DISP 的缓冲、合成与拷贝）。
而**旋转恰恰是 DISP 做的** ⇒ **不走 DISP 就没有旋转**。
- 非 UNCACHE：VDEC → DISP 缓冲（**拷贝时顺带做旋转**）→ 显示器；
- UNCACHE：  VDEC 缓冲 → 显示器（零拷贝；省下的正是那份显示缓冲，**但没有旋转**）。

**实测（V851s、480x800 竖屏、720p 源走 1/2、固化部署、同板同源）**：

| 配置 | MemAvailable | VmRSS | RssAnon | 画面 |
|---|---|---|---|---|
| 无 UNCACHE（`0x11`） | 8.6MB | 19064 | 13280 | ✅ 铺满 `frame[43,0,393,700]` |
| UNCACHE（`rot` 在 init 传 / `set_rot` 后下发，两种都试过） | **14.4MB** | **14852** | **8128** | ❌ 只填到 y≈417、下方约 40% 全黑 |

- 内存收益**是真的**（可用内存 +5.8MB、进程 RSS −4.2MB）—— 省的就是"显示缓冲那份拷贝"。
- 画面**废掉**：disp 层铁证 `fb[640,384] crop[0,0,360,640]`（缓冲**未**旋转、与 crop 不自洽）；
  正常时是 `fb[360,640] crop[0,0,360,640]`（缓冲已旋转、自洽）。可见高度
  416/700 = 0.594 ≈ 384/640 ⇒ 拿"旋转后的 crop（高 640）"去读"高只有 384 的未旋转缓冲"。
- **init 带 `rot` 也救不回来**（AirPlay 就是这么写的，本板实测同样只填 y≈417）。

**结论**：竖屏屏看横屏内容（IPTV / 投屏）必须转 90°，**不能拿旋转换内存** ——
除非将来换成"旋转 + 直显"都支持的库版本；届时的验收判据是两条：
① 画面铺满 `frame[43,0,393,700]`；② 与关闭该 flag 时逐像素一致（相关系数 >0.7）。

---

## 3. 缩放解码：本包的杀手锏（内存实测）

V851s 只有 **56MB 内存**。720p 不缩放时解码缓冲直接把内存吃干：

| 源 | 缩放 | 解码输出 | MemAvailable |
|---|---|---|---|
| 1280x720 | 不缩放 | 1280x736 | **2.6 MB** ⚠️ 危险 |
| 1280x720 | **1/2**（`0x10`） | 640x384 | **4.1 MB** ⭐ 推荐 |
| 1280x720 | **1/4**（`0x20`） | 320x192 | **6.5 MB** ✅ 最安全 |
| 426x240 | 不缩放 | 448x256 | 充裕 |

> 480x800 屏上 1/2 缩放（640x360）已经比屏幕还大，**完全够用**；
> 挑档位规则：`源宽 > 屏宽上限` 就下一档，能 1/2 就别硬扛不缩放。

---

## 4. 显示：它是 disp **硬件视频层**，不是控件

- `set_pos` 给的是**屏幕坐标**的显示区域；画面从 **UI 层的透明窗口**（`videoView`，
  `visible:true`）透出来 —— **UI 层不放透明窗口，画面就被盖住**（典型"解码正常但黑屏"）。
- `set_crop` 用的是**旋转后**的坐标系 ⇒ **`set_rot` 之后再发一次 `set_crop`**。
- 本层是**内核态**的：进程崩溃/重启后**不释放**。异常重启后屏幕会**冻在上一轮画面**。
  ⇒ 程序启动早期应主动关掉残留的非 UI 层（"启动释放图层"），否则重启后画面是上一轮残影。
- 排查视频层用 `/sys/class/disp/disp/attr/sys`（**不在 /dev/fb0 里**，fb0 只有 UI 层）。

---

## 5. ★ 五个必踩的坑（都带症状与做法）

### 5.1 起播瞬间"静默退出"= **`ZKMEDIA_H264_VBVSIZE` 没设**（头号坑）

- **症状**：`init()` 前后进程**没有任何报错就没了**（stderr 被重定向到 /dev/null），
  日志断在"起播"那几行，随后被 init 拉起重启；在带 QA 脚本的现场会形成"重启→重播→再重启"。
- **根因**：库用**默认的小码流缓冲**，720p 的 I 帧几百 KB 装不下。
- **做法**：在**第一次 dlopen 之前**（即首次 `preload()` 前）设
  `setenv("ZKMEDIA_H264_VBVSIZE", "1048576", 0)`（1MB；`0` = 外部已设不覆盖，方便现场调参）。
  这个环境变量是 **dlopen 时读的**，改完要**重启应用**才生效。
- **实测**：设之前 720p(1.2~1.5Mbps) + 1/2 缩放**起播即崩**；设之后 240~900 帧连续跑、丢 0。
- 另有 `ZKMEDIA_H264_LAYER`（显示层号，参考工程里出现过），本平台一般不用改。

### 5.2 内存 < 3MB 起播必挂（且"不自己回来"）

起播那一刻要申请解码/显示缓冲。**可用内存 < 3MB 时 init 会失败或进程静默消失**。
被 OOM 杀过的进程**内存不会自己回来，必须重启板子**（这一点很反直觉，浪费过半天）。

> 做法：起播前 `echo 3 > /proc/sys/vm/drop_caches` 清页缓存（本板实测一次腾出 **+16MB**）。
> 判据：盯 `MemAvailable`（`/proc/meminfo`），别只看 `MemFree`。

### 5.3 直播流"源尺寸 0x0"→ 先解析 SPS 再 init

HLS/RTSP 的第一个包往往不是关键帧。`init(0,0,...)` 会黑屏且**没有软解可探测**。
做法：边读边解析 SPS，拿到真实宽高**再** init（本包 init 传 `0,0` 也能起步，但要等解码回调里
用**真实尺寸重算显示区**）；`put_frame` 前必须已有 **SPS/PPS + IDR**。

### 5.4 `get_picture_count()` **不是**队列长度，别拿它做背压

它更像"已提交帧数"：**喂多少就报多少**，哪怕显示线程一帧没取走（实测恒等于池大小）。
⇒ 判断"解码器有没有真出画"**只看解码回调**；做背压请用**媒体时间 vs 播放时间**
（例如落后 >600ms 才丢帧），不要用这个计数。

### 5.5 与 MPP 链路**互斥**（同一颗 VE + 同一个 disp 视频层）

同一时刻只能有一条链路在跑。切链路前先优雅停掉另一条（停止 → 释放视频层 → 再起）。
另外：**停止后要主动释放 disp 视频层**，否则下次起播可能拿不到层 / 画面是上一轮残影。

---

## 6. 起播"无声消失"的排查清单（按这个顺序，别绕）

1. **环境变量**：`ZKMEDIA_H264_VBVSIZE` 设了吗？（§5.1，头号原因）
2. **内存**：起播前 `MemAvailable` 有 **≥3MB** 吗？被 OOM 过就重启板子（§5.2）。
3. **码流**：`put_frame` 的数据里**是否已有 SPS/PPS/IDR**？用工具查 NAL 类型，缺了就一直等。
4. **协议**：喂的是 **Annex-B** 吗？（TS → 要去掉 PES 头；mp4 → 要 `h264_mp4toannexb`）
5. **显示**：UI 层有 `visible:true` 的 videoView 透明窗口吗？
6. **参数组合**（最后才怀疑）：`rot` 放 init 里、crop 与 set_rot 的先后 —— 实测**都可用**，
   不是崩因（见 §7）。

> 排障手段：把 `stderr` 从 `/dev/null` 引出来（或写文件）才有机会看到库内报错；
> 起播前后各 `cat /proc/meminfo`；用 `logcat` 对齐"最后一条日志"与"进程消失"的时间点。

---

## 7. 参数组合的实测结论（省得你再 A/B 一遍）

| 组合 | 实测 |
|---|---|
| `init(rot=0)` → `show()` → `set_rot(90)` → `set_crop` | ✅ 连续 36 秒 / 900 帧 / 0 重启 |
| `init(rot=90)`（不再 `set_rot`） | ✅ 同样 36 秒 / 900 帧 / 0 重启 |
| 720p + `SCALE_DOWN_2` + 90° | ✅（VBV 设好、内存充足的前提下） |
| 960x540 不缩放 + 90° | ✅ |

⇒ **旋转放哪都行**；推荐"init 传 rot"或"起播后 set_rot"择一，**但 `set_rot` 之后必须重发 `set_crop`**。

---

## 8. 最短验收流程（真机）

```bash
# 1) 看层：视频层是否 enable（ch 号按平台）
cat /sys/class/disp/disp/attr/sys
# 2) 看解码：回调有没有在涨（这是"真的出画了"的唯一证据）
logcat -d | grep -E "解码回调|zk_h264_player_init"
#    期望：解码回调 #1 → #60 → …  ；出现 init(...,flag=0x11) -> 0 说明 dlopen + init 都成
# 3) 看编码器缓冲与丢帧
logcat -d | grep "已喂"
# 4) 抓一帧确认画面（注意先关屏保，否则抓到的是时钟）
```

---

## 9. 与其它方案的分工

| 场景 | 用什么 |
|---|---|
| H264 直播/点播上屏、要旋转/缩放/裁剪 | **本包**（`zk_h264_player`） |
| 需要 MPP 家族能力（与摄像头/录像/多路复用同链路） | MPP（`AW_MPI_VDEC_*`），注意 960x544 上限与"停不下来"的自愈机制 |
| JPEG / MJPEG（照片、UVC） | 见 `v85x/jpeg-decode-record.md` |
| 只是要显示"播放器控件"（本地 mp4 等） | 框架 `ZKVideoView`（另一条路） |
