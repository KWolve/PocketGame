# IPTV（网络电视）· 实现记录与踩坑

> 起因：用户 2026-09-13「做完（时钟套件）再做一个 IPTV 功能」，
> 2026-09-14 拍板走「方案 B（自实现 HLS 客户端）+ zk_h264_player 路线」。
> 本文是**实施后的记录**（开工前的调研结论保留在 §1，§4 起是实测与设计）。

## 0. 一句话结论

**HLS 自实现客户端 + 本地 HTTP 中继 + 复用已验收的硬解链路，真机跑通**：
拉 m3u8 → 顺序下 ts 分片 → 拼成连续 TS 流 → 本地中继（`127.0.0.1:8199`）
→ `StreamPlayer`（ffmpeg mpegts 解封装 + 设备硬解）→ disp 视频层。
连续播放 74 秒 / 1890 帧无中断，换台与停止也跑通。

**一个必须在产品层面接受的硬约束**：本板硬解上限 **960x544（≈52 万像素）**，
720p（92 万）会把解码通道打爆 ⇒ 公开 IPTV 源里**只有低分辨率那一档能播**。

## 1. 为什么不能直接播 m3u8（调研结论，2026-09-13）

本板固件自带的 ffmpeg **没有编 HLS 解封装**：

```
nm --defined-only src/dependencies/lib/libavformat.a | grep -i hls   → 0 个符号
http://222.223.41.27:8888/hls/1/index.m3u8 → Invalid data found (-1094995529)
```

而 IPTV 源绝大多数是 HLS。三条路线里选了 **B（自实现）**：
HLS 本身是很薄的文本协议 —— m3u8 就是分片清单，按顺序把 `.ts` 下下来**首尾拼接**，
拼出来的字节流就是一条连续 MPEG-TS 流。剩下的全部复用已验证链路。

## 2. 现有播放链路（已验证，§4 的设计基础）

`pg::StreamPlayer`（`src/platform/PgStream.*`）此前已趟平并在真机验收：

```
ffmpeg(网络+解封装) → 设备硬解(AW_MPI_VDEC) → disp 视频层
                    → 设备音频解码 → 常开 PCM 流（PgAudio）
```

入口：`StreamPlayer::startWithDisplay(url, maxSeconds, x, y, w, h)`。

⚠️ **必须由 UI 层的"透明窗口"把画面透出来** —— 本平台 UI 层在最顶且不透明，
只有 videoview 区域透明处能露出下层视频层。IPTV 播放页的 `IptvVideo`
（`ui/main.html` 的 `WinIptvPlay`，0,0,480,700）就是这个窗口。

**所以 HLS 只负责"把 m3u8 变成一条可播的 TS 流"，解码/显示一行代码都不用改。**

## 3. ★ 硬约束：只能解 H264，且上限 960x544

`src/platform/PgStream.cpp`：

```cpp
const long long kMaxDecodePixels = 960L * 544L;  // 硬上限（720p=92 万，必死）
const long long kMinAvailKb = 7 * 1024;          // 可用内存低于 7MB：先重启
```

本板 `MemTotal` 只有 **55MB**（实测 `MemFree` 常态 1~10MB）。实测：
720p 的源在 `AW_MPI_VDEC_CreateChn` 阶段就直接触发内核 OOM（厂商播放器也一样）。

**对 IPTV 的直接影响 = 频道表必须预先筛分辨率**（见 §5）。
另有 `E_H264_PLAYER_FLAG_SCALE_DOWN_2/4`（zk_h264_player 的 1/2、1/4 缩放解码）
可作为 720p 的后续增强，见 §8。

## 4. 实现：`src/platform/PgHls.{h,cpp}`

```
m3u8(文本) ──解析──> 分片 URL 列表 ──顺序下载 .ts──> 环形缓冲(1MB)
                                                        │
                       本地 HTTP 服务(127.0.0.1:8199) ──┘
                              │
   StreamPlayer::startWithDisplay("http://127.0.0.1:8199/live.ts", …)
```

**为什么用"本地 HTTP 中继"而不是"在应用内直接把 ts 喂解码器"**：

1. **StreamPlayer 完全不动** —— 它已真机验收（含 OOM 保护、旋转、视频层、
   音频常开 PCM 流、卡死自愈），零回归风险；
2. 模块可**独立测试**：本地那个 URL 用同一个 ffmpeg 探一次就知道通不通；
3. 环形缓冲天然解决"网络抖动"与"下载/解码速度不匹配"。

关键实现点：

| 项 | 做法 | 理由 |
|---|---|---|
| 拉清单/分片 | **ffmpeg 的 `avio_open2` + `avio_read`** | 复用它的 http/https/TLS/证书链路，不必自己引 OpenSSL |
| master playlist | 挑**最低带宽档**（`setMaxBandwidth` 可改） | 本板内存小、硬解上限低，低档才播得动 |
| 起播位置 | 从清单**倒数第 1 个**分片开始 | 直播窗口滚动，太旧的分片可能已被删 |
| 续拉 | 记 `EXT-X-MEDIA-SEQUENCE + 下标`，只下更新的 | 直播语义；落后 ≥4 个就丢旧的追赶 |
| 环形缓冲 | 1MB | 实测标定：本板可用内存只剩 6~9MB，缓冲每多 1MB 都直接影响"能不能播下一个台" |
| 客户端连接 | 从"最新写位置"开始（不重放旧数据） | 直播语义；ffmpeg 会等下一个 PAT/PMT + I 帧 |
| UDP/端口 | 只 `bind 127.0.0.1:8199` | 不对外暴露；避开 DNS 53 / DLNA 8200 |
| `send` | 一律带 `MSG_NOSIGNAL` | 客户端断开时否则 SIGPIPE 直接打死进程 |

**不支持（遇到明确报错，不静默失败）**：`#EXT-X-KEY METHOD=AES-128` 加密流、
fMP4（`#EXT-X-MAP` / `.m4s` 分片 —— 本板链路只认 mpegts）。

## 5. 频道表：`tools/hls_filter.py` → `resources/iptv/channels.m3u`

**为什么需要筛**：公开 IPTV 列表（iptv-org）里绝大多数是 720p/1080p，
直接抄进来用户"点十个有九个播不了"。

筛选两步（任一步失败就丢）：
1. 拉 m3u8：master 取**最低档**的 `RESOLUTION`；单档清单靠名称里的 `(720p)` 标注估；
2. **真去下一个 ts 分片**，确认 HTTP 通且首字节是 TS 同步字 `0x47`
   —— 挡掉"域名能解析但拿不到流"的僵尸源。

输出按可信度分组：`·可播`（分辨率已确认）/ `·未验证`（单档清单、分辨率未知）。

用法：

```bash
python tools/hls_filter.py                       # 默认拉 iptv-org 中国频道
python tools/hls_filter.py -i my.m3u -o out.m3u
python tools/hls_filter.py --max-pixels 520000   # 收紧分辨率上限
python tools/hls_filter.py --keep-hd             # 只筛连通性
```

⚠️ 结果只是"**跑脚本那台机器**上当时可用"，源随时失效 ⇒
设备端 **`/data/iptv.m3u` 优先**（用户自己更新，不用重刷固件）。
`/data` 是可写 jffs2（重启不丢），内置表打包后在 `/res/ui/iptv/channels.m3u`。

## 6. ~~换台必须"复位应用"~~（2026-09-14 作废，见 §11）

> ⚠️ **这一节的结论只对"MPP 老路"（VDEC+VO+应用层软件旋转）成立。**
> 视频链路在 §10 换成 `zk_h264_player` 硬件播放器后，换台/停止**不再需要重启进程**
> —— 实测 pid 不变、画面照常出（证据见 §11.3）。**下面留档，别当现行结论用。**

老路（`sHwVideo=0`）的实测：本平台**被停止过的解码通道会变脏**（`StopRecvStream` 之后
不再出帧），两条路都试过：

| 做法 | 日志 | 结果 |
|---|---|---|
| `stopForSwitch()`（不置 needsReset，让应用活着换源） | 一切正常：`收尾完成（sRunning 已清零，可再次播放）`、`复用常开通道 0，StartRecvStream -> 0x0`、HLS 也在正常下分片 | **清空 logcat 后 12 秒零解码帧** —— 画面定格在旧台 ❌ |
| `stop()`（触发 `tickStopReset` 的 `_exit(0)`） | — | init 立刻拉起，通道重建，**画面正常** ✅ |

于是当年换台 = 写 `/tmp/pg_iptv_resume` + `_exit(0)` + 重启后续播，代价是屏幕闪一下
（像"退回主界面再进"）。**§11 已改成不重启**，那段 `pg_iptv_resume` 续播逻辑仍保留在主循环里
（现在没有任何代码会写它，留着以防以后又需要）。

## 7. 应用与界面

- **应用**：`GameIptv`（`src/core/PgGames.h`，`APP_TOOL`，**slot 20**，
  `nativePage() == 4`）；图标槽 19 → 21（`tools/gen_icons.py` + `gen_ui.py` + `ui/main.html`）。
- **选台页** `WinIptv`：一屏 8 个频道槽（`BtnCh0..7`）+ 上下翻页 + 重新加载；
  槽位显示 `序号 [分组] 名字`，正在播的高亮。
- **播放页** `WinIptvPlay`：`IptvVideo`（透明窗口 0,0,480,700）+ 顶部状态字
  + 底部 100px 黑条（上一个/停止/下一个）。
- **logic**（`src/logic/mainLogic.cc` 的"IPTV 网络电视"区）：
  `iptvLoadChannels()` / `iptvPlayAt()` / `iptvStopAndBack()` / `syncIptv()` /
  `syncIptvPlay()` / `tickIptv()` + 12 个按钮回调。
- ⚠️ **前置声明必须放匿名 namespace 之外**（文件 ~289 行之前）：`enterTool()` 和
  `exitGameToMenu()` 在匿名 namespace 里也要用这些函数/状态；放进去会造出两个
  同名实体，链接期报 "used but never defined"（本工程在时钟套件上踩过同样的坑）。

**QA 命令**（本板注入不了控件触摸，这是唯一验收通道）：

| 命令 | 作用 |
|---|---|
| `20 0` | 进入 IPTV 应用（20 = kAppTable 槽位） |
| `iptvlist` | 把频道表打进日志（序号/分组/名字/URL） |
| `iptv <n>` | 直接播第 n 个频道（0 起）；`n < 0` = 停止回选台页 |
| `hlsstat` / `hlsstop` / `hlsbw <bps>` | HLS 中继状态 / 停止 / 选流带宽上限 |
| `hls <m3u8>` / `hlsonly <m3u8>` | 直接起 HLS 中继（带/不带播放器），排障用 |

## 8. 后续可做（按价值排序）

1. **720p 支持（`libawh264player.so` / `zk_h264_player`）** —— 用户点名的方向。
   设备 `/lib/eyesee-mpp/` 里已有完整 cedar 硬解栈（`libvdecoder.so`、`libVE.so`、
   `libawh264.so`、`libMemAdapter.so`…，正好匹配 `libawh264player.so` 的 NEEDED），
   而 `src/dependencies/lib/libzkmedia.a` 里的 `h264_player.o` 是它的 dlopen wrapper
   （导出 `zk_h264_player_*`，内部 `dlopen("libawh264player.so")`）。
   关键优势是 **`E_H264_PLAYER_FLAG_SCALE_DOWN_2/4`（1/2、1/4 缩放解码）**
   —— 480x800 屏上播 720p 完全够用，可能绕开 960x544 上限。
   ⚠️ 两个待验证点：① `lib-no-link/` 里的 .so **不随 `fun launch` 推送**
   （wiki：只随程序打包），要手动 `adb push` 到 `/tmp/lib`；
   ② `libawh264player.so` 依赖 `/lib/eyesee-mpp/`（不在默认 ld 搜索路径），
   需要 `LD_LIBRARY_PATH` 或等价手段。
2. 频道分组筛选（`·可播` / `·未验证` 目前只体现在 `group-title` 里）。
3. AES-128 加密 HLS（要接 AES-128-CBC 解密）。
4. 换台过度动画（现在复位期间是黑屏 ~1s）。

## 9. 已验证证据（2026-09-14 真机）

| 项 | 证据 |
|---|---|
| HLS 链路 | `清单 OK —— 分片 4 个（从倒数第 1 个起播），档位 360x240 bw=400000` |
| 连续播放 | 解码帧 #1 → #1890，墙钟 74s 无中断（跨多个分片，续拉在工作） |
| 音频 | `音频就绪 aac 48000Hz 2ch → 22050Hz 单声道 S16`，音频时钟与墙钟同步 |
| 选台页 UI | 截图：8 个频道槽全为 `#222C3A` + 白字，紫条/标题/翻页/按钮齐 |
| 播放页 UI | 截图：videoview 区全黑（视频层不透 fb0）+ 底部黑条 + 三按钮 `#26313F`/`#7A3B3B` |
| 换台 | pid 1681→1717，自动续播频道 2，解码帧从 #1 重新计数 |
| 停止 | 写 `-1` → 复位 → 回到选台页；内存回到 17.3MB 可用 |
| 频道表 | `频道表已加载 20 个（内置表，/tmp/ui/iptv/channels.m3u）` |

⚠️ **截图坑**：`flythings_device_screenshot` 有时会抓到**非活动帧**（全屏纯背景色），
判据是看返回的 `screenInfo.pan` —— 活帧是 `"0,800"`（`offsetY: 800`）；
抓到 `"0,0"` 就是空白 buffer，**重抓一次**即可。

## 10. ★ 视频链路切换硬件播放器（2026-09-14，"错屏"的最终修法）

用户报 IPTV"解码出来图像错屏"，抓屏+disp 层分析定位出老路（MPP VDEC+VO+应用层软件旋转）
的两个根因，随后按用户点名切换到 `zk_h264_player` 的**硬件链路**（默认启用）。

### 10.1 老路错屏的两个根因（证据齐全，留档）

1. **几何错配**：软件旋转（`PgStream::rotateInto`）转出 256x448 的缓冲，配在
   `frame=480x700` 的 VO 层上 → DE 按 480 宽读 stride=256 的缓冲 → **画面以 256px
   周期横向重复**（自相关 shift=256 处 corr=0.906）。
2. **格式错配**：VDEC 实际输出 **NV12**（fmt=23），`rotateInto` 按 **I420 三平面**拆
   → 色度全乱（大片纯绿）。判别实验：`streamrot 0` 直送原始帧颜色正常 → 错在旋转代码。

### 10.2 新链路：`zk_h264_player`（默认 `sHwVideo=1`）

```
ffmpeg 解封装(本地中继/网络) → zk_h264_player_put_frame(Annex-B)
    → 硬件解码 + 硬件旋转(set_rot) + 缩放(set_pos) + 裁剪(set_crop) → disp 层
```

- 三个 API 的语义（与硬件基准工程 `V851ExtendedScreen_ap_p2p/src/link/context.cpp` 核对）：
  `set_pos(x,y,w,h)` = **屏幕坐标系**的显示矩形（硬件负责缩放）；
  `set_crop(x,y,w,h)` = **旋转后**坐标系里的源裁剪，**内部要按缩放解码倍率除回去**
  （参考工程 `video_crop` 就是 `x/scale`）；
  `set_rot` = 0/90/180/270 顺时针，转完要重发一次 crop。
- `PgH264` 新增流式接口：`startStream()` / `feed()` / `setRotation()` / `setDispRect()` /
  `setSourceCrop()` / `fitRect()`（等比放进显示区，不裁切不拉伸）。
- `PgStream` 加 `sHwVideo` 开关（QA `streamhw <0|1>`，默认 1）与喂帧节流
  （按媒体时间领先音频 ≤600ms，`kHwLeadMs`）。
- 运行中 `streamrot <deg>` **当场生效**（硬件旋转 + 显示区重算），不用重开流。

### 10.3 ★★ 三个必记的坑（都实测过）

1. **源尺寸未知时绝不能 `init(0,0)`**：直播流开流时 `codecpar` 常是 0x0（HLS 中继从
   "最新写入位置"开始，SPS 要等下一个 I 帧）。拿 0x0 init → 播放器按默认尺寸分配画面
   缓冲（disp 里 `fb[432,240]` 而 crop 是 448x256）→ **画面全黑、解码不再消化**。
   解法：`PgStream` 里**自己解析 SPS**（`findSpsSize`，本工程 ffmpeg **没编 H264 软解**
   ，`avcodec_find_decoder(AV_CODEC_ID_H264)` 返回 0，没有"软解一帧问尺寸"这条路），
   拿到宽高再 init。init 用 SPS 的 coded 尺寸（如 426x240），显示区用 `fitRect` 算。
2. **`zk_h264_player_get_picture_count()` 不是"待显示队列"**：它**恒等于已提交帧数**
   （喂多少报多少，显示正常跑也一样）。拿它做背压 → 喂到阈值后每帧都被"积压"丢掉，
   画面定格、日志只说"积压 N 帧"（自伤，踩了两次）。⇒ 流式模式不做背压，
   判断"真的在出画"只认 `framesDecoded()`（解码回调计数）持续增长。
3. **抓屏验证视频必须先退屏保**：30s 无操作框架自动进屏保（时钟页，UI 层 z16 不透明，
   盖住视频层），抓出来的是时钟不是视频，极易误判成"不出画"。QA `saver to 600` 延长
   超时 + `saver off` 退出。另外设备 shell **没有 sleep/grep/head/wc**，`adb shell` 里
   的这些命令会静默失败——等待一律放 PC 侧。

### 10.4 验证（2026-09-14 真机，CCTV+ 1 直播）

| 项 | 结果 |
|---|---|
| 0° 直播 | 画面实时动（连抓帧间差 51/119），CCTV+ 新闻内容、颜色几何全对，480x270 居中 |
| 90° 旋转 | `streamrot 90` 当场生效；disp 层 `fb[240,426] crop[0,0,240,426] frame[43,0,394,700]`（等比满高度），画面转正、文字可读 |
| 连续播放 | 60s 喂 1500 包 / 解码 1499 帧，丢 0，媒体时间与播放位置同步（±0.3s） |
| disp 层 | ch[0] 由硬件播放器自建自管，退出由 `H264Player::stop()`+`VideoLayer::release()` 兜底 |

## 11. 打开 loading + 超时 + 停止/换台不重启（2026-09-14 用户需求）

> 📦 **本页已于 2026-09-14 拆成独立 ftu**：`iptv.ftu` / `iptvActivity`，
> 布局 `ui/iptv.html`、逻辑 `src/logic/iptvLogic.cc`，自检通道 `/tmp/pg_iptvcmd`。
> 下面 §11 记的是链路本身（PgHls / 相位机 / 超时 / 不重启），跟页面归属无关，仍然有效。

用户两条要求：① **打开频道要 loading 效果 + 超时处理**；② **停止 / 切下一个不要重启**。

### 11.1 为什么原来"像卡死"：清单是**同步**拉的

`Hls::start()` 里原来第一件事就是 `httpFetch(清单)` —— **同步**、在 UI 线程上。
好源几百 ms，死源要等 http 超时（秒级）⇒ 界面整个僵住，连"正在连接"四个字都画不出来。

**修法：把清单挪进 HLS 自己的下载线程**（`Hls::start()` 只做本地准备、立刻返回）：

```cpp
// PgHls：start() 不再拉清单；下载线程第一轮 pullOnce() 去拉
start()            → 立刻返回（只建环形缓冲 + 绑端口 + 起线程）
pullOnce()         → 拉清单/解析/排分片，并记录状态：
select Hls::openState()   // 0 解析中 / 1 已解析（= 频道可用）/ 2 失败（原因看 lastError）
```
- 失败判"连续 2 次"才判死（网络抖动/源偶发 5xx 很常见，一次就报会让用户白看到一次错误）。
- `"清单 OK"` 那条日志也从 `start()` 搬进了 `pullOnce()`。
- 顺带白捡一个好处：播放器**立刻**连本地中继（`serveClient` 先发 HTTP 头、等环形缓冲有数据才发字节），
  所以首帧完全取决于源的速度，不受"同步拉清单"拖累。

### 11.2 相位机：把"黑盒等待"拆成可看/可超时/可错的几步

```
IDLE ──点频道──▶ SWITCHING（旧流收尾）──▶ WAITFRAME（等首帧）──▶ PLAYING
                    │                        │
                    └──── 失败/超时 ─────────┴──▶ ERROR（显示原因，2.6s 后回选台页）
```

`tickIptv()` 每帧只做三件事：推进相位、刷 loading 动画、判超时（**全程不碰网络**）。

| 超时 | 值 | 判据（★ 关键） |
|---|---|---|
| 换台收尾 | 9s | 等 `StreamPlayer::running()` 归零 |
| 首帧**停滞** | 10s | **多久没有新进展**（**按 `Hls::bytes()` 字节判**，不是按分片数、也不是总共等了多久） |
| 首帧**上限** | 35s | 从起播算起，再慢也不无限等 |
| 错误提示停留 | 2.6s | 然后自动回选台页 |

**踩到的三个坑（都是真机上"看起来对、实则错"的那些）**：

1. **"进展"必须看字节，不能看分片数**：实测有源首分片要下 12s 才完成，这期间
   `bytes()` 一路在涨、`Hls::segments()` 一直是 0 ⇒ 用分片数判进展会把**正在正常下载**的流误杀
   （日志铁证：`停止（已下 0 分片 / 425984 字节）`）。
2. **"出画"必须看增量，不能看 `framesDecoded() > 0`**：`H264Player` 的 `stop()`
   **不复位**这个累计计数 ⇒ 换台后第一拍就"以为出画"，直接进 PLAYING，
   **超时再也触发不了**（挂住的频道永远黑屏等你）。做法：起播时记基线 `gIptvBaseFrames`，只认增量。
3. **超时不能"一刀切"**：同一个源一次 10.1s 出画、另一次 13s 还没下到第一个分片；
   先用固定 13s 就把正常但慢的源误杀过一次。⇒ 改成"停滞 + 上限"。

### 11.3 ★ 停止 / 换台不重启（改掉 §6 的老做法）

```cpp
// 换台：优雅停旧流，但**不置复位标志**（sSwitchMode）
pg::StreamPlayer::stopForSwitch();   // 而不是 stop()（后者触发 tickStopReset 的 _exit(0)）
// 收尾完成后由 tickIptv() 的 SWITCHING 相位接着开新台
```
**为什么现在可以**：`zk_h264_player` 的 `stop()` 会 `deinit` 硬件播放器并把 disp 视频层
`VideoLayer::release()` 掉 ⇒ 下一轮是**全新一套**，不存在"复用脏通道"（§6 那个坑是 MPP 老路的）。
退出 IPTV 应用（回主界面）也一并改成 `stopForSwitch()`，不再白闪一下。

真机证据（本地测试台架 + 真实源都测过）：

| 场景 | 判据 | 实测 |
|---|---|---|
| 打开频道 | 日志 | `出画（起播→首帧共 587ms，频道 0，本轮已解码 6 帧）` ✅ |
| **切下一个** | **pid** | 5708 → **5708（没重启）**；`换台（不重启）→ 目标 [1]` → `出画（…1724ms，频道 1）` ✅ |
| **停止** | **pid** | 5708 → **5708**；`停止（不重启）→ 收尾完成后回选台页`，相位回 `IDLE` ✅ |
| 挂住的源 | 日志 + 相位 | `WAITFRAME 字节=0` → 10s 后 `失败 —— 连接超时（10 秒无进展）` → 2.6s 后回列表；**主循环照常响应**（下一条 QA 立刻有回） ✅ |
| 慢源（13KB/s） | 日志 | 进度一路在涨 ⇒ 不被误杀，`出画（…20566ms，频道 0，本轮已解码 419 帧）` ✅ |

### 11.4 界面：加载页三行（纯文字，零资源）

`WinIptvPlay` 里新增 `TextIptvLoad`（标题）/ `TextIptvLoadBar`（进度条）/ `TextIptvLoadHint`（提示），
**必须在 `IptvVideo` 之后**（json 顺序 = 层叠顺序，否则被视频层盖住）。出画时清空文字即隐藏。

| 行 | 加载中 | 失败 |
|---|---|---|
| 标题 | `正在连接/正在缓冲/正在切换到 <频道>` + 循环点号 | `连接失败` |
| 进度条 | `[-----=====------]` 方块平移（**ASCII 拼的**，不引资源、不怕缺字） | 失败原因（`频道不可用：…` / `连接超时（10 秒无进展）`） |
| 提示 | `已等 Ns · 已缓冲 NKB` | `即将返回频道列表` |

配色按状态切：加载 **#4FC3F7**（蓝，与进度条同色）、失败 **#F2B33D**（琥珀，一眼看出是错误）。
真机逐像素验收（`docs/shot_iptv_loading.png` / `docs/shot_iptv_timeout.png`）：
信息行主色分别为 `(79,195,247)` 与 `(242,179,61)`，三行墨迹中心 236~240（居中）✅

> **为什么进度条用 ASCII 拼**：本项目 `font/*.ttf` 是**完全替换**系统字体的子集字库、
> 没有逐字回退，缺哪个字那个字就整个消失（见 docs/screensaver.md §8.3）。
> 用 `[ ] = -` 就永远不会有缺字风险，也不用带图片资源。

### 11.4.1 画面旋转（与 IPTV 直接相关）

竖屏设备看横屏直播源要转 90°，**这条不再依赖手动设置**：`desiredRotation()` 的兜底
已改成 90°（直播流一律没有旋转元数据，auto 原来等于"不转"）。见 docs/online-media.md §17。

### 11.4.2 ★ 视频区"缓冲底图"（2026-09-14 用户报）

**现象**：「视频缓冲还没有解码的时候，视频需要放一个背景图，不然图层透明过去配上黑色的
会有字体模糊的问题。」

**真因**（MCP `v85x/videoview-transparent-window.md` 的权威口径）：
`videoview` 是 **UI 层给下层 disp 视频层开的"透明窗口"** —— 控件区域内 UI 层不填充内容，
视频层有数据就从这里透出。而在**还没有数据**的时候（拉清单 / 等首帧 / 报错），
那块区域就是"透出最底层"，屏幕上是**纯黑 + 一层发虚的字**。

**修法**：在播放页的视频区铺一张**不透明**的底图（480x700 = 视频区尺寸）：

| 项 | 做法 |
|---|---|
| 图 | `resources/images/video_cover.png`，由 `tools/gen_ui.py` 的 `gen_video_cover_asset()` 生成（幂等；深蓝黑渐变 + 主色播放标记 + 上下细描边，**不带 alpha**） |
| 控件 | `ui/main.html` 里 `ImgIptvCover`（`class="icon"` + `data-pic`）⇒ `touchable:false`、初始 `visible:false`（列进 gen_ui 的 `HIDDEN_CONTROLS`） |
| 位置 | 必须在 `IptvVideo` **之后**（json 顺序=层叠，后写的在上）、loading 三行**之前**（文字压在图上） |
| 显隐 | `tickIptv()` 每拍收敛：`SWITCHING / WAITFRAME / ERROR` 三段**铺着**，`IDLE / PLAYING` 立刻**收起**（`iptvBackToList()` 再显式收一次兜底） |

⚠️ **两个必须记住的点**：
1. **必须及时收起** —— UI 层在视频层**之上**，底图留着不撤就把视频挡死了。
   所以判据是"**还没出画面**"，不是"页面打开着"。
2. **图形要避开 loading 文字**：第一版把播放标记放在正中间（y=330），正好压在三行文字上，
   屏幕上一团叠着的图形 + 字。现在整块图形落在 y≈128..272，文字在 290..406，上下分明。

真机验收（`/dev/fb0` 抓 UI 层，视频层抓不到所以看 UI 层）：

| 场景 | 判据 | 实测 |
|---|---|---|
| IPTV 缓冲期 | 三角中心像素 | `(240,330)=(79,195,247)` ✅（旧版位置）；新图形后 `(240,190)=(79,195,247)`、`(240,330)=(16,23,31)` 渐变 |
| 同上 | 渐变 | `(10,100)=(12,17,23)` / `(240,60)=(11,16,22)` ✅ |
| 同上 | loading 文字 | 白像素 **1192**（x=30..444 居中）⇒ 文字在底图之上、与图形不重叠 ✅ |
| 投屏"打开中" | 像素 | 三角 `(240,330)=(79,195,247)` ✅（`streamshow` 后 0.8s） |
| **出画后** | 480x700 区域 | **全黑（UI 层透明）** ⇒ 底图已收起、不挡视频 ✅ |

投屏页（`WinCast`）同款：`ImgCastCover`，`startCast()` 时铺上、播放器回报
`PLAY_STARTED` 后收起；在线流投屏那条路按 `pg::H264Player::framesDecoded() > 0` 判出画
（`tickVideoCover()` 每拍收敛 —— 出画来源有三条，靠事件置位漏一条底图就会一直挡着视频）。

### 11.4.3 ★★ 内部 TS 转发：**已去掉本地 HTTP 中继**（2026-09-14 晚改）

> 本节原来写的是"中继能去掉、本次没改"。**现在改了**，下面是实测结论与做法。

**先厘清三件事（都有实测）**：

| 问题 | 答案 |
|---|---|
| ffmpeg 能直接打开 m3u8 吗？ | **不能**。`streamshow https://…/master720p.m3u8` → `打开失败：Invalid data found (-1094995529)`（这份静态 libavformat 没有 HLS demuxer） |
| "自己拉清单+下分片"（PgHls）能去掉吗？ | **不能**，它是唯一能拿到 ts 的办法 |
| **"本地 HTTP 中继（127.0.0.1:8199）"能去掉吗？** | **能，而且已经去掉了** ✅ |

**做法：ffmpeg 自定义 AVIO 直读环形缓冲**

```
PgHls::pullThread（网络）→ 环形缓冲 ring
                              │
                    Hls::readBuffer(buf,n,timeout)   ← 阻塞读（条件变量，最多等 30s）
                              │
   PgStream::openInput 看到 `pg-hls://` 前缀 → avio_alloc_context(读回调)
                              │
   ffmpeg（mpegts 解封装 / 音视频分流 / 时间戳）→ 设备硬解 → disp 视频层
```

- `PgHls::start()` 写出的播放地址从 `http://127.0.0.1:8199/live.ts` 变成 **`pg-hls://live`**；
- `PgStream::openInput()` 见到这个前缀就用 `avio_alloc_context()` + 读回调（`hlsReadCb` →
  `Hls::readBuffer`）；**别的地址（http/https/本地文件）完全走原路，一个字没改**；
- 起播读指针对齐分片头的逻辑跟着搬进 `Hls::readBuffer`（第一个读者进入时对齐）；
- **`avformat_close_input` 不会释放我们自己建的 pb**（`AVFMT_FLAG_CUSTOM_IO` 的约定），
  所以 `PgStream` 加了一个 `closeInput()` 统一收口：先 `avformat_close_input`，
  再 `av_freep(&avio->buffer)` + `avio_context_free`（**缓冲区得自己 free**，漏了每次起播泄漏 64KB）。

**收益**：少一层本地 TCP、少一次内存拷贝、少一个监听端口（`netstat` 里 8199 消失，
`/proc/net/tcp` 里 `:2007` 计数 = 0 ✅）；起播路径少一个线程。

**真机验收（干净状态下）**：

| 项 | 实测 |
|---|---|
| 起播 | `PgStream: 打开 HLS 环形缓冲（自定义 AVIO 直喂，无本地中继）` → `起播读指针 = 0（对齐分片头）` → `解码回调 #1 —— 640x384 crop(0,0,640,360)` → `出画（起播→首帧 10404ms，频道 32）` ✅ |
| **换台** | `换台（不重启）→ 目标 [33]` → `出画（8787ms，频道 33）`；**pid 1009 → 1009** ✅ |
| **停止** | `停止（不重启）→ 收尾完成后回选台页`；pid 不变 ✅ |
| 端口 | `/proc/net/tcp` 无 `:2007`(=8199) ✅ |
| 视频层 | `disp: ch[0] fb[360,640] crop[0,0,360,640] frame[43,0,393,700]`（640x360 转 90° 后铺进播放区）✅ |
| http 源（对照，不走本改动） | `oceans.mp4` → `解码回调 #1`、`已喂 150 包 解出 148 帧` ✅ |

### 11.4.4 ★★ 用户报"这个流有声音、但背景图概率没隐藏"——两个真 bug

用户给的流：`https://amg00405-rakutentv-…amagi.tv/master.m3u8`（4 档位，PgHls 选中 640x360）。

**bug 1：出画判据有竞态（"概率"的来源）**

原来判"出画"是 `framesDecoded() > gIptvBaseFrames`，基线在 `iptvBeginOpen()` 里记。
问题是 `H264Player::startStream()` 会 **在子线程里**把 `framesDecoded` 清零，而记基线在主线程：

```
基线读到"清零前"的旧值 → 之后被清零 → dec - base 恒为负 ⇒ 永远判不出画
```

**实测铁证**：`iptvstat: … 本轮解码帧=-18`（负数），而日志里 `解码回调 #1` 明明 **1 秒内就响了**
—— 解码早就出帧了，界面却干等 **20.7 秒**（最后是被超时判失败/或被第二次相位重置救回来）。
"概率"的真相：**谁先跑决定结果**。

**修法**：清零与判据都收进 PgH264，不再让调用方记基线：

```cpp
// PgH264：本轮帧数（armRun 由 PgStream::startCommon 在**起线程之前**调，无竞态）
H264Player::armRun();               // s.framesRun = 0
H264Player::framesDecodedInRun();   // onDecodedFrame 里 ++
```
调用方一律判 `framesDecodedInRun() > 0`（IPTV 相位机 + 投屏页 `tickVideoCover`）。
`PgStream::startCommon()` 是唯一收口点 ⇒ IPTV / DLNA 在线流 / `streamshow` 全部自动正确。

**bug 2：起播从"分片中间"开始 ⇒ 视频要等下一个 IDR（音频却是立刻出声）**

`serveClient` 原来 `s_read = s_written`（从**最新字节**开始，直播语义）。后果：
- **音频立刻有**（AAC 帧自带同步，不需要 IDR）—— 用户听到声音以为在播；
- **视频要等下一个 SPS/PPS + IDR** ⇒ 实测 **20.7 秒才出画**，屏幕上是"缓冲底图 + 声音"
  ⇒ 用户报"背景图概率没隐藏"。

**修法**：起播读指针**对齐到"当前分片的起始写位置"**（`s_lastSegStart`，在 `fetchSegment()` 写之前记）
⇒ 从分片头开始读，一定有 SPS/PPS + IDR ⇒ 视频立刻能解。代价：起播最多多等一个分片（~6s）。

⚠️ **顺手踩的坑**：加了 `s_lastSegStart` 却忘了在 `Hls::start()` 里复位它 ⇒ **第二轮起播**
读指针（上一轮的大值）跑到新写指针前面 ⇒ 客户端**永远读不到字节**（实测：字节一直涨、
解码帧恒 0、界面卡在底图）。已复位，并给 `serveClient` 加了范围防御
（`start ∈ (s_written - ring/2, s_written]`，越界就退回最新）。

**真机验收（同一进程连续两次起播）**：

| 场景 | 实测 |
|---|---|
| 第一次 `iptv 0` | `起播读指针 = 0（写 0，分片头 0，对齐分片头）` → `出画（起播→首帧共 13460ms，本轮已解码 3 帧）` ✅ |
| 停止后第二次 `iptv 0` | `出画（…8537ms，本轮已解码 1 帧）` ✅；`iptvstat` 的"本轮解码帧"**正常递增、不再是负数** |
| 底图 | 两次出画后抓屏：**蓝色三角像素 = 0** ⇒ 底图正确收起 ✅ |
| 无重复起流 | `起流 [0]` ×1、`选中档位` ×1 ✅ |

> ⚠️ 8~13 秒出画里，**大部分是源本身慢**：实测第一个分片（918KB）从该 CDN 下完要 **4.6 秒**
> （`iptvstat` 显示 4.6s 时 `字节=0`）。这是外网/源的速度，不是链路问题。

### 11.4.5 ★ 选台页改**列表控件**（2026-09-14，用户要求）

**原来**：`WinIptv` 一屏 8 个频道槽（`BtnCh0..7`）+ 上一页/下一页 —— 42 个频道要翻 6 页，
而且按钮只有一行、名字前面挤着分组（`1. [地方]嵊州综合`）。用户原话："不要用按键切页面，这个体验太差了"。

**现在**：`ZKListView`（`ListIptv`），一屏 7 行、可滚动，每行三段：

```
┌──────────────────────────────────────────────┐
│ 01  CCTV-1 综合                     非全天播出 │  ← 主行 20px（正在播的变绿）+ 备注（琥珀）
│ 央视 · 1280x720@25fps · 1030kbps             │  ← 副行 13px 灰
└──────────────────────────────────────────────┘
```

- 三个回调：`getListItemCount_ListIptv`（行数）/ `obtainListItemData_ListIptv`（行内容）/
  `onListItemClick_ListIptv`（点播放，等价于点频道槽）；
- `syncIptv()` 只剩两件事：**行数变了才 `refreshListView()`**（它会逐行回调，每帧调会白重绘）
  + 刷新状态行；删掉了 8 槽刷新/翻页/`gIptvPage`；
- QA `iptv <n>` 语义不变（n = 列表行索引，0-based）✅ 脚本不用改；
- 频道表新增字段 `size/kbps/note`（来自 `# 实测` 注释与 `tvg-note`），列表副行/右侧显示；
  **旧表或用户自备 `/data/iptv.m3u` 缺这些字段也能跑**（少显示一段而已）。

⚠️ **列表子控件的 `data-align` 必须显式写**：不写时框架按**居中**排（实测墨迹中心正好落在
控件区中心），看起来像"缩进乱"。列表行要 `data-align="left"`。

真机验收：列表 7 行、行高 81/间距 8；`01 CCTV-1 综合` 主行墨迹从 x=27 起（左对齐 ✅）；
第 5 行 `05 嵊州综合` 汉字段宽 78px = **4 个汉字**（缺字只会是 58px ⇒ 字库已修好 ✅）；
副行按内容渲染（`822kbps` 比 `1030kbps` 短 7px ✅）；备注列琥珀色像素在 x=390..443 右对齐 ✅；
点第 1 行 → `点列表第 0 行 -> CCTV-1 综合（央视）` → 起流 ✅。

### 11.5 新增 QA：`iptvstat`

```
PocketGame iptvstat: 相位=WAITFRAME 目标=2 已等=5993ms 字节=0 清单=0 本轮解码帧=0 流=1 内存=21180kB
```
"为什么还在等 / 卡在哪一步"一眼看出来 —— 定位上面那些坑全靠它。
另：`iptvlist`（频道表）、`iptv <n>`（播第 n 个）、`iptv -1`（停止）。

### 11.6 ★ 顺手修掉一个隐蔽的"CPU 打满"死循环

`PgStream.cpp` 里 `StreamPlayer::memAvailableKb()` 的包装写成：

```cpp
long long StreamPlayer::memAvailableKb() { return memAvailableKb(); }  // ❌
```
成员函数体里**不限定的同名调用会先查到本类自己的静态成员** ⇒ 自我递归；
`-O2` 把这种尾递归优化成**死循环**（不爆栈、不崩溃、CPU 100%）——
`iptvstat` 第一次调用它就把主线程卡死在 95% CPU，界面全冻、QA 命令全不响应。
改成 `return memAvailKb();`（把 `PgStream.cpp` 匿名区里那个实现改名，消除同名遮蔽）。
**同一族坑**：`StreamPlayer::running()`（见 `startCommon()` 的注释）。
判别特征：**`top -t` 里主线程 95% CPU + `/proc/<pid>/task/<tid>/syscall` 显示 `running`**
（在用户态空转，不是等锁/等 IO）。

### 11.7 本地测试台架（可复现，不依赖外网源）

外网 IPTV 源又慢又飘（同一频道一次 10.1s、一次 13s 没下到第一个分片），要**确定性**验收就得
自己造源：从真实频道抓两个 `.ts` 分片下来，PC 上用一个小 HTTP 服务伺服成 HLS
（`/a.m3u8`、`/b.m3u8` 正常；`/slow.m3u8` 延迟返回；`/hang.m3u8` 挂住不响应），
设备上放 `/data/iptv.m3u` 覆盖频道表即可。

- ⚠️ 服务必须**绑具体网卡地址**（如 192.168.1.19），绑 `0.0.0.0` 会被 Windows 防火墙拦
  （见 `tools/range_http.py` 的注释）。
- ⚠️ Python3 的 `wfile.write()` **只吃 bytes**，写 `str` 会在 per-request 线程里抛异常、
  返回 500 —— 现象像"服务通了但内容不对"。

### 11.8 抓屏坑（本板特有）：`pan 0,0` 拿到的是空页

`flythings_device_screenshot` 偶尔连抓几次都是 `pan "0,0"` + 很小的文件 ——
那是 `/dev/fb0` 的**非活动页**（UI 层实际显示的是 y=800 那半页，`disp` 里能看到
`crop[0, 800, 480, 800]`）。判据与绕法：

```bash
adb shell "/tmp/busybox dd if=/dev/fb0 bs=1920 skip=800 count=800 | /tmp/busybox gzip -c > /tmp/fb.gz"
adb pull /tmp/fb.gz
# 本地按 32bpp 480x800 stride=1920 解 BGRA → PNG（脚本见本次工具调用记录）
```
⚠️ 设备**没有 dd**（`/bin` 里没有），要用 `/tmp/busybox`；且**别用 shell 管道直接传到 PC**
（二进制会被换行转换搞坏，gzip 解不开）—— 先在设备上落文件再 `adb pull`。
