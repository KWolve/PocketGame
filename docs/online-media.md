# 在线多媒体（网络流播放）—— 方案、实测与踩坑

> 2026-09-13。起因：用户指出"投屏要下载完再播，`/tmp` 是内存，大文件必然炸；应该播在线流"。
> 本文件记录**为什么之前走不通**、**正解是哪个组件包**、**怎么接进来**、**实测到哪一步**。

---

## 一、先说结论

| 问题 | 结论 |
|---|---|
| 设备自带的播放器能播网络流吗？ | **不能**。固件里的 cedarx 媒体栈只编了 file 流（证据见第二节） |
| 那"在线流"靠什么？ | **官方 `ffmpeg` 组件包（v85x 专版 `4.1.9-configure4`）**，它带 http/https/rtsp/rtp/udp + openssl |
| 现在的播放链路对不对？ | 之前"整片下载到 `/tmp` 再播"是**错的**（`/tmp` 是 tmpfs=内存）。正解：**ffmpeg 解封装 → 设备硬解 → 显示**，内存恒定 |
| 实测到哪一步？ | ✅ **全链路已完成**：https 打开（含证书校验）→ ffmpeg 解封装 → **VDEC 硬解** → **VO 上屏** → A/V 同步（软解音频灌常开 PCM 流）；DLNA 投屏按源分流（http/https **在线流直连**、本地文件走框架播放器）。**画面旋转 90/180/270** 也已落地（本板只能用"自己转"） |
| 还有哪些坑 / 现在什么状态？ | 内存红线（720p 必 OOM）、"停止过的 VDEC 通道回不来"、"收尾两条铁律"、内存自救、自愈 —— 见 **§九 起**；本文档**后面的章节就是完整实现与验收记录** |

---

## 二、为什么"把 URL 丢给播放器"这条路在固件上根本走不通（证据）

三处独立证据，结论一致 —— **固件的流层只实现了 file**：

### 1. 框架播放器直接拒绝 URL
```
PocketGame cast: play 'https://.../big_buck_bunny.mp4'
E/zkgui: (f:playerThreadLoop, l:391) fatal error! file[...] is not exist!
PocketGame cast: PLAY_ERROR 播放出错
```
`ZKVideoView::play(const char *pFilePath)` / `ZKMediaPlayer::play()` 的入参就叫"文件路径"，
easyui 在 `playerThreadLoop()` 里先判"文件是否存在"。知识库也印证：VideoView 只支持
本地文件 + `main_video_list.txt` 轮播。

### 2. 设备上的 cedarx 流层只有 file
```bash
# /lib/eyesee-mpp/libcdx_stream.so 的导出与依赖
T AwStreamRegister / T CdxStreamCreate / T CdxStreamOpen / __FileStreamConnect
U close, dup, lseek, malloc ...      # ← 没有 socket / connect / getaddrinfo
```
只有一个流实现（`__FileStreamConnect`），未定义符号里连 socket 都没有
⇒ **固件没有编 http/https/rtsp 流插件**。

### 3. `aw-mpp` 的 DEMUX 虽然写了 `SOURCETYPE_URL`，但底下没人实现 http
`mm_comm_demux.h`:
```c
typedef enum SOURCETYPE_E { SOURCETYPE_FD, SOURCETYPE_URL, ... };
typedef struct DEMUX_CHN_ATTR_S { SOURCETYPE_E mSourceType; char *mSourceUrl; int mFd; ... };
```
接口在、实现不在（见第 2 点）——所以喂 URL 也一样打不开。

> 也顺带查过 `libzkmedia.so`（框架播放器的真正后端）：C 接口是
> `zk_media_create/play/pause/...`，签名与 easyui 一样是"文件路径"，没有 URL 入口。

---

## 三、正解：ffmpeg 组件包（用户指出的那个）

```
https://package.flythings.cn/v85x/ffmpeg/4.1.9-configure4
```
包内 `README.md` 是它的 configure 摘要（**这就是"支持在线多媒体"的书面依据**）：

```
network support           yes
External libraries:       iconv  openssl  zlib          ← TLS 真编进去了
Enabled protocols:        file http https rtp tcp tls udp
Enabled demuxers:         aac asf flac mov mp3 mpegts ogg rm rtsp wav
Enabled bsfs:             aac_adtstoasc  h264_mp4toannexb  null
Enabled decoders:         h264 hevc aac mp3 flac vorbis mjpeg h261 h263 pcm* wavpack
Libraries:                avcodec avdevice avfilter avformat avutil swresample swscale
```
符号层复核（`nm`）也确认：`ff_http_protocol` / `ff_https_protocol` / `tls_openssl.o` /
`ff_mov_demuxer` / `ff_mpegts_demuxer` / `ff_rtsp_demuxer` 都在。

⇒ **正确的播放链路**：
```
控制器给的 URL
  → avformat_open_input(url)     // ffmpeg 自己走 http/https（含 TLS、证书、系统时间）
  → av_read_frame()              // 逐帧编码数据（H264/H265/AAC…）
  → h264_mp4toannexb (bsf)       // MP4 封装 → Annex-B 裸流（硬件解码器要这种）
  → AW_MPI_VDEC_SendStream()     // 设备硬解（**不软解**，V85X 软解带不动）
  → 视频层上屏（VideoView 控件的显示区域）
```
**内存恒定**（只有解封装缓冲），不再整片落 `/tmp`。

---

## 四、怎么接进工程（含 4 个必须知道的坑）

### 4.1 链接方式：按官方"本地库"约定，把库放进 `src/dependencies/`
```
src/dependencies/include/   ← ffmpeg 头文件（自动加入 include 路径）
src/dependencies/lib/       ← libavformat.a / libavcodec.a / libavutil.a / libswresample.a
                              以及 libssl.so.1.1 / libcrypto.so.1.1
```
- `Manifest.xml` 里**不用**加 ffmpeg 包（我们自带了），但要加 **`z`（zlib）**：
  ffmpeg 的 http(gzip)/mov 解封装要 `uncompress`/`inflate*`，否则链接报
  `undefined reference to uncompress / inflateInit2_`。
- ffmpeg 的静态库**引用 OpenSSL**（`SSL_CTX_new`/`SSL_CTX_load_verify_locations`/`OPENSSL_init_ssl`…），
  而工具链 sysroot 里**没有** libssl ⇒ 把**设备上的** `/lib/libssl.so.1.1`、`/lib/libcrypto.so.1.1`
  拷进 `dependencies/lib`（SONAME 与设备一致，运行时不需要额外打包）。

### 4.2 ⚠️ 体积：14.5MB → 2.49MB 的三步瘦身（这步不做，`/res` 根本装不下）
`/res` 分区只有 **7.6MB**（`/proc/mtd`：mtd3 = `0x007a0000`），所以 `.so` 必须瘦。

| 步骤 | 做法 | 结果 |
|---|---|---|
| ① 禁止 `av_register_all()` | 它内部会调 `avcodec_register_all()`，把**所有软解/编码器**拉进来 | 14.5MB |
| ② 改成**按需注册** | `av_register_input_format(&ff_mov_demuxer)` 等 11 个 demuxer + 6 个 parser | 9.2MB |
| ③ **打桩顶掉 `allcodecs.o` / `h264dec.o`** | 见下 | 7.17MB |
| ④ **`strip -g` 去掉调试段** | ffmpeg 的 `.a` 带 **4.5MB DWARF**（`.debug_info` 3.1MB…），代码其实只有 2.3MB | **2.49MB** |

③ 的具体细节（**很反直觉，值得记住**）：
- `libavformat/utils.o` 引用 `avcodec_find_decoder`、`avcodec_find_decoder_by_name`；
  `libavcodec/utils.o` 引用 `avcodec_find_encoder`；`libavcodec/options.o` 引用 `av_codec_next`
  —— 这 4 个符号**都定义在 `libavcodec/allcodecs.o`**，而 allcodecs.o 的
  `avcodec_register_all()` 挨个引用所有软解 ⇒ 链上它就把 16MB 的 libavcodec 整个拖进来。
- `libavformat/utils.o` 还引用 `avpriv_h264_has_num_reorder_frames`（在 `h264dec.o`）
  ⇒ 就为调它一次，**整个 H.264 软解码器**（含两张 32KB VLC 大表）被拖进来。
- 处理：**在 `PgFf.cpp` 里自己实现这几个符号**（返回 NULL / 0），链接器就不再拉那两个 .o。
  见 `src/platform/PgFf.cpp` 的"打桩顶掉 allcodecs.o"一节。
- `-Wl,--gc-sections` **没用**：厂商编译时没有按函数分段（段表里只有一个 `.text`）。

### 4.3 ⚠️ ⚠️ `https` 全失败的真凶：**IPv6 优先**（本次最深的坑）
症状极具误导性：
```
PgFf: 打开失败 'https://raw.githubusercontent.com/...'：I/O error (-5)
```
而 `http://example.com/` 能开（报"无解封装器"= 网络 OK），且**同一个域名用我们自己的
下载器完全正常**。

打开 ffmpeg 的日志（我们把它的日志桥到了 LOGD）后才看清：
```
PgFf[ffmpeg]: Connection to tcp://raw.githubusercontent.com:443 failed: Network unreachable
```
根因：
1. **musl 的 `getaddrinfo()` 会优先返回 AAAA（IPv6）**；本板 `wlan0` 只有
   link-local（`fe80::4ea3:...`），**没有全局 IPv6 路由** → connect 直接 `ENETUNREACH`；
2. 而 **ffmpeg 4.1 的 `tcp_open()` 只尝试 `getaddrinfo` 返回的第一个地址，不遍历
   `ai_next`**（新版才加遍历）⇒ 第一个是 IPv6 这个域名就必然连不上；
3. 我们自己的客户端用 `gethostbyname()`（只取 A 记录），所以从来没这个问题
   —— 这就是"同一个网址我们能下、ffmpeg 说网络不可达"的原因。

处理：**应用启动时把 IPv6 关掉**（`Ff::disableIpv6()`，写
`/proc/sys/net/ipv6/conf/{all,default,wlan0}/disable_ipv6 = 1`；zkgui 以 root 跑）。
本板本来没有 IPv6 出口，关掉是纯收益。
⚠️ 内核移除 IPv6 地址是**异步**的 → 放在 `onUI_init` 里提前关，比等到第一次联网再关更可靠。

**实测（关掉后）**：
```
PgFf: 打开成功 'https://raw.githubusercontent.com/mediaelement/mediaelement-files/master/big_buck_bunny.mp4'
PgFf:   容器=mov,mp4,m4a,3gp,3g2,mj2 时长=60.1s 码率=734kbps 流数=4
PgFf:    音频流[0] codec=aac 22050Hz 2ch
PgFf:    视频流[1] codec=h264 640x360
```
—— https + **证书校验（verify=1，用随包的 `resources/certs/cacert.pem`）一次性通过**。

### 4.4 排障必备：把 ffmpeg 的日志桥出来
设备上 `stderr` 指向 `/dev/null`，ffmpeg 的错误原因只写在它自己的日志里
（不看就只能看到一句 `I/O error`）。`PgFf.cpp` 里用
`av_log_set_callback()` 把 `AV_LOG_WARNING` 以上转到 `LOGD`，前缀 `PgFf[ffmpeg]:`。

---

## 五、关于"B 站投屏链接播不了"（把那条链接彻底解出来）

之前那条（从 `SetAVTransportURI` 日志里原样抓下来的）：
```
http://upos-sz-estghw.bilivideo.com/upgcxcode/21/76/41735357621/41735357621-1-192.mp4
  ?e=ig8euxZM2rNcNbRz7WdVhwdlhWhBhwdVhoNvNC8BqJIzNbfqXBvEuENvNC8aNEVEtEvE9I
  &ua=tvproj      ← 客户端标记：TV 投屏
  &oi=3071376681  &og=hw  &nbs=1  &trid=d8623a82...  &gen=playurlv3
  &mid=491699326  ← 用户 UID
  &uipk=5         ← 与"请求方 IP"相关
  &deadline=1789274750   ← 有效期（= 2026-09-13 12:45:50）
  &os=estghw      &upsig=06569b5412066d13fa178b232e87ec19   ← URL 签名
```
**实测（同一条链接、同一个出口 IP 183.17.125.41）：**

| 做法 | 结果 |
|---|---|
| 设备侧下载器（默认 UA） | HTTP **403** |
| 设备侧换 B 站 TV UA / 浏览器 UA / 加 `Referer: https://www.bilibili.com` | 全部 **403** |
| PC 直连（不走代理） | HTTP **403**，响应头带 `X-Upsig-Version: 20220801`，响应体里 `Client_ip: 183.17.125.41` |

**完整拒绝应答**（这就是"解码"出来的东西）：
```html
HTTP/1.1 403 Forbidden
Server: openresty
X-Upsig-Version: 20220801          ← CDN 做了 URL 签名校验
Content-Type: video/mp4
via: CHN-GDguangzhou-CT8-CACHE31[0]
...
Client_ip: 183.17.125.41           ← CDN 看到的客户端公网 IP（= 我们这条宽带）
Node_info: 4317-CACHE31
```

结论（有实测支撑，不是猜）：
1. CDN 对该 URL 执行 **`upsig` 签名校验**（`X-Upsig-Version` 头就是证据），签名不匹配 → 403；
2. **不是"换了设备所以 IP 不对"**：从 PC 直连（同一出口 IP）同样 403，`Client_ip` 打出来的
   就是我们这条宽带的地址；
3. 所以这条链接绑定的不是"设备"，而是**手机 App 那次请求的会话上下文**
   （App 侧的 sig/风控指纹），第三方设备复现不了；
4. 由此：**"App 内投屏"给的链接不是通用直链**。要在我们设备上放，只能
   —— 让手机端做转推/代理，或设备端实现同款鉴权（= 复刻 B 站 TV 客户端，不现实），
   或**改用通用 DLNA 源**（PC/NAS 上的 DLNA 服务器、Windows 媒体共享等，给的都是普通直链）。
   ⚠️ 这与"能不能在线流播放"是**两件事**：链路的锅已经修好了（第三节），
      这条链接是**源侧鉴权**问题。

---

## 六、自查 / 自检命令（QA 通道 `/tmp/pg_autostart`）

| 命令 | 作用 |
|---|---|
| `ffprobe <url>` | 打开在线流并打印容器/时长/码率/各流编码与分辨率（**不下载、不落盘**） |
| `ffverify <0\|1>` | 开关 https 证书校验（默认开；排障时可关） |
| `dlna tlscheck` | 自己的 TLS 通道自检（dlopen、符号、CA、系统时间、校验策略） |
| `dlna dns <域名>` | 打印解析结果（排查"DNS 劫持/解析错"） |
| `dlna ua <字符串>` / `dlna ref <字符串>` | 改我们自己下载器的请求头（防盗链排查） |
| `ntp` / `ntp sync` / `ntp setclock <epoch>` | 校时状态 / 立即同步 / 复现 1970 冷启动 |

---

## 七、当时的"下一步"（播放链路）—— ⚠️ **后面全部做完了**

> **本节是历史计划，保留作对照。** 里面列的 4 件事**都已实现**，实际做法与这里略有出入
> （例如上屏最终用的是"透出视频层 + 自己 `VO_SendFrame`"，不是 `SYS_Bind`），
> **看实现请看 §九 起的章节**。

当时已具备：**能拿到在线流的编码数据**。当时还差：
1. `av_read_frame()` 循环 → 取 H264/H265 包 → `h264_mp4toannexb` 转 Annex-B →
   `AW_MPI_VDEC_SendStream()`（`aw-mpp` 包，设备已有 `libmedia_mpp.so`）；
2. 上屏：复用 `WinCast` 里的 `ZKVideoView` 控件**只当显示区域**
   （知识库 `v85x/videoview-transparent-window.md`：控件放好、**不要调 play()**，
   视频层画面就从那块区域透出），由我们自己把解码帧送进视频层；
3. 音频：AAC → `AW_MPI_ADEC` → `AW_MPI_AO`；
4. 内存：解码缓冲在 MPP 侧（`AW_MPI_SYS_Init` 后的 CMA/ion 分配），
   **不再需要 `/tmp` 存整片** ⇒ 前面那个"下载到 tmpfs"的限制可以整段删掉。

---

## 八、固化与最终验收（v1.7.1）

固化包：`tools/upgrade_device.sh 1.7.1` → `out/update.img` **1.5MB**（`/res` 分区 7.6MB，余量充足）。

```
/res/lib/libzkgui.so      2492140      ← 含 ffmpeg 的完整版本
/res/lib/libssl.so.1.1     239836       ← 这两个是 dependencies/lib 里的（运行时用 /lib 的，/res 里这份是冗余，见下"可优化"）
/res/lib/libcrypto.so.1.1  879680
/res/ui/certs/cacert.pem   188900       ← 随包 CA
/res/ui/{main,remote,wifi}.ftu
启动配置 startupLibPath=/res/lib/libzkgui.so  resPath=/res/ui/  touchDev=/dev/input/event4
/tmp/lib: No such file                  ← 跑的是 flash 里的 app
MemFree 20608 kB / Shmem 1200 kB        ← 固化后内存很宽裕（调试模式推 /tmp 时只有 7.6MB）
```

**开机后【第一次】https 探测（冷缓存，最苛刻的场景）**：
```
PgFf: ffmpeg 就绪（按需注册 11 个 demuxer + 6 个 parser；解码走设备硬解）
PgFf: CA 证书已预热 188900 字节
PgFf: avformat_open_input 用时 825ms
PgFf: 打开成功 'https://raw.githubusercontent.com/.../big_buck_bunny.mp4'
PgFf:   容器=mov,mp4,m4a,3gp,3g2,mj2 时长=60.1s 码率=734kbps 流数=4
PgFf:    音频流[0] codec=aac 22050Hz 2ch
PgFf:    视频流[1] codec=h264 640x360
```
同时确认：`/proc/sys/net/ipv6/conf/all/disable_ipv6 = 1`（**应用开机自己关的**）。

⚠️ 过程中遇到并已处理的"开机后第一次 https 报 I/O error"：
- 现象：固化版首次 https 失败（`error:00000000` + `I/O error (-5)`，**OpenSSL 错误队列是空的** ⇒ 不是证书问题），
  紧接着再试就成功；`ffverify 0` 也一定成功。
- 处理：① `rw_timeout` 10s → **30s**；② **启动时把 CA 读一遍预热 page cache**
  （固化版 CA 在 squashfs 上）；③ open 失败**自动重试一次**。
- 之后连续验收（含冷缓存首测）**一次成功**，耗时 825ms。

**可优化（未做）**：`dependencies/lib` 里的 `libssl.so.1.1`/`libcrypto.so.1.1` 会被 `fun pack`
一并打进 `/res/lib`（约 1.1MB）。运行时动态加载器只查 `/lib`、`/usr/lib`，**不会**去 `/res/lib`，
所以这两份是冗余的。要清掉得让"只打包、不参与链接"（`lib-no-link`）——但那样链接期就缺符号了，
除非改成用 `-lssl` 之类的其他方式引入。当前 1.5MB 的包里这点冗余不影响使用。

---

## 九、播放链路 阶段一：ffmpeg 解封装 → 设备硬解出帧（✅ 已真机验证）

新增 `src/platform/PgStream.{h,cpp}`，QA 命令：`streamtest <url> [秒]`（后台线程跑，不阻塞 UI）。

**MPP 的取用方式（重要）**：**dlopen 设备上已有的 .so**，**不链接** ——
`/lib/eyesee-mpp/libmedia_mpp.so`（SYS/DEMUX/VDEC/ADEC/AO/CLOCK）+ `libmpp_vo.so`（VO）。
原因：同进程里 easyui→libzkmedia 已经加载了一份 MPP，再静态链一份会变成**两套 MPP 实例**
抢同一个内核驱动；dlopen 复用同一份最稳，且**零体积代价**
（和 PgAudio dlopen ALSA、PgDlna dlopen OpenSSL 是同一套路）。
头文件用法：把 `aw-mpp` 包的 301 个头**拍平**进 `src/dependencies/include/`
（该目录已是 include 路径；头之间是相对 include，拍平后自然成立）。

### 踩坑：`AW_MPI_SYS_Init` 必须**先 SetConf**
```c
AW_MPI_SYS_SetConf(&conf);   // conf.nAlignWidth=16, mkfcTmpDir="/tmp"
AW_MPI_SYS_Init();
```
只调 Init → 返回 **`0xa0028010`**（低位 `0x10` = `EN_ERR_SYS_NOTREADY`），
此时**再调 `AW_MPI_VDEC_CreateChn` 会直接段错误**（实测：应用被 init 反复拉起，
形成崩溃循环，logcat 里能看到 pid 一路 957→975→993→1008→1023 地变）。
zkmedia 也是 SetConf→Init 这个顺序 —— 照抄它。
（顺带：设备节点 `cedar_dev` / `disp` / `ion` 都在，不是节点缺失。）

### 真机结果（`big_buck_bunny.mp4`，640x360 H.264）
```
PgStream: 打开成功 用时 3577ms 视频[1] codec=h264 640x360
PgStream: Annex-B 转换=启用(h264_mp4toannexb)
PgStream: AW_MPI_SYS_SetConf -> 0x0
PgStream: AW_MPI_SYS_Init -> 0x0
PgStream: 解码帧 #630 宽高=640x384 id=5（已送 1019 包 / 用时 14716ms）
PgStream: 收尾 —— 送 1025 包，解出 643 帧，用时 15464ms
```
- **15 秒解出 643 帧**（≈43fps，含取帧/释放开销）⇒ 确实是**硬件解码**在跑。
- 帧高 384（对齐到 16 的倍数），宽 640 —— 与片源一致。
- 偶发 `VDEC_SendStream -> 0xa005800f`（码流缓冲满时的正常背压），不影响出帧。
- **无内存泄漏**：连续播 4 次，`Shmem` 恒为 `9028 kB`（tmpfs 里是我们自己的 .so/ui/font），
  `MemFree` 稳定 —— VDEC Create/Destroy + SYS_Init/Exit 这一圈是干净的。

### 阶段二（当时待做 —— **现已完成**，见 §九 起）
1. **上屏**：`AW_MPI_VO_Enable/EnableVideoLayer/SetVideoLayerAttr/OpenVideoLayer/CreateChn/StartChn`
   —— 按 `WinCast` 页里视频区的位置/尺寸建一个 video layer（zkmedia 的 `createVOLayer/createVOChn`
   就是这个套路），再把 VDEC 与 VO 用 `AW_MPI_SYS_Bind` 绑起来（绑上后解码帧自动进 VO，
   不用自己 SendFrame）。
2. **音频**：AAC → `AW_MPI_ADEC_*` → `AW_MPI_AO_*`（zkmedia 用的就是这套，可照抄）。
3. **接进投屏流程**：`SetAVTransportURI` 时若 URL 是 http/https → 走本流的流式播放；
   是本地文件 → 保留原来的框架播放器路径。**同时删掉 `PgDlna` 里"下载到 /tmp"那一段**
   （以及 10MB 上限、边下边播那些权宜之计）。

---

## 十、播放链路 阶段二①：VO 上屏（✅ 真机验证通过）

新增 QA：`streamshow <url> [秒]`（带画面）/ `streamstop`（停）。`streamtest` 仍是"只解码不上屏"。

### 显示分层（权威口径 + 实测）
```
UI 层    disp ch2 / lyr0 / z=16   ← easyui 的 fb0，最顶
视频层   disp ch0 / lyr0 / z=0    ← 我们/VO 输出的画面
再下     按 4,3,2,1 叠
```
**UI 层里的 videoview 控件覆盖的区域是"透明窗口"**，视频层画面从那儿透出；
两边互不感知（UI 侧只管放控件，出图侧只管往视频层送帧）。

- 实测框架播放（ZKVideoView 播本地文件）时的视频层：
  `ch[0] lyr[0] z[0] fmt[77] crop[0,0,1280,720] frame[0,0,480,700]`
  ⇒ 视频区 = 投屏页 `Caster`（`ui/main.html` 里 `data-x/y/w/h = 0,0,480,700`）。
- 判据（**透明窗口是否真透明**）：dump UI 层当前显示缓冲的像素 alpha
  （`dd if=/dev/fb0`，注意 **fb0 是双缓冲**、活动缓冲偏移见 `disp attr/sys` 的 `crop`）：
  投屏页视频区 `00 00 00 00`（**A=00 透明**）、底部按钮 `3b 3b 7a ff`（不透明）。

### 实现（`src/platform/PgStream.cpp`）
```
VO_Enable(dev0) → VO_EnableVideoLayer(0) → VO_SetVideoLayerAttr(rect=(0,0,480,700))
→ VO_OpenVideoLayer → VO_CreateChn → VO_SetFrameDisplayRegion(同 rect) → VO_StartChn
解码循环：VDEC_GetImage → VO_SendFrame(layer,chn,&frame,20) → VDEC_ReleaseImage
          （同一块 ion 缓冲，零拷贝）+ 节流到 25fps
收尾：    VO_StopChn → VO_DestroyChn（层与 dev 留着）
```
实测：`626 帧 / 25.0s = 25.0fps`（节流生效）；收尾 `StopChn/DestroyChn -> 0x0`。

### ⚠️ 三个坑
1. **不要每轮流结束就 Disable/Close 层与 dev**：本平台 VO 的常态是"常开"
   （KB `v85x/display-layer-debug.md`：easyui 的 zkmedia 播放器退出也不释放 VO dev0）。
   实测第二轮若走完整 teardown，则下一次 `EnableVideoLayer` 回 `0xa00f8041`
   (`EN_ERR_VO_DEV_NOT_ENABLE`)、`SetVideoLayerAttr`/`OpenVideoLayer` 回 `0xa00f8045`
   (`EN_ERR_VO_NOT_ENABLE`)。
2. **这些错误码不要中断流程**：它们出现时 `CreateChn/SetFrameDisplayRegion/StartChn`
   仍返回 `0x0`，且 `disp` 里能看到我们的层被 enable、addr 在随帧更新
   —— 也就是**画面是好的**。所以只记日志（带错误名），不提前 return。
3. **不要把"投屏页显隐"写成匿名命名空间里前向声明**：`mainLogic.cc` 的 `runAutoCmd`
   在 `namespace {}` 内，声明全局函数会造出第二个实体 → `call of overloaded ... is ambiguous`。
   改用标志位（`gStreamWantPage`）交给全局作用域的主循环调用。

### 用法（真机）
```
printf 'streamshow https://vjs.zencdn.net/v/oceans.mp4 30\n#t1\n' > /tmp/pg_autostart
```
⚠️ 两个操作细节：
- **进程重启会重放 `/tmp/pg_autostart` 的最后内容**（去重状态在进程内存里）→
  重启后它会自己再跑一次旧命令，导致你紧接着推的新 `streamshow` 被"已有任务在跑"拒绝。
  先写一条无害内容（如 `#idle`）再重启，或先 `streamstop`。
- 一条流最多 `[秒]` 秒；**播完主循环会自动收掉投屏页**，屏幕回主界面。

---

## 十一、播放链路 阶段三：接进投屏流程（✅ 真机端到端）

`SetAVTransportURI` 的 URI 按源分流：http/https → **在线流直连**（本文件的 PgStream 链路）；
本地文件 → 框架播放器。**"下载到 /tmp 再播"这条路已废弃**（原因见 `docs/dlna.md` §八：
/tmp 是 tmpfs=内存，且断流会被误判成下载成功）。

- 控制器动作分流见 `docs/dlna.md` §八 的表（Play/Stop/Seek/Pause + 进度回报）。
- 实测（v1.7.4 固化版 + PC 侧 `tools/dlna_ctl.py`）：投屏 473ms 开流、`TrackDuration=0:00:46`、
  `RelTime` 递增、**无落盘**、两轮播放内存稳定（9116→9324kB，Shmem 恒 1200）= 无泄漏。
- 遗留：**在线流的音频还没接**（`AW_MPI_ADEC` + `AW_MPI_AO`），所以现在是有画面没声音；
  控制器发来的 `SetVolume` 目前只记日志。下一步做音频。

---

## 十二、播放链路 阶段二②：音频（✅ 本地源实测实时）

### 走哪条路（**关键决定**）
**不接框架的 ADEC/AO**，而是「ffmpeg 软解 AAC → 重采样成 22050Hz/单声道/S16 → 灌进
`PgAudio` 那条常开 PCM 流」。理由：
- 框架那条（zkmedia→eyesee-mpp AO）**自己拼声卡设备名**（拼成 `hw:1,0`，本板喇叭在 card0）
  → 日志全对却完全静音（本工程历史上为此打过一整天仗）；
- `PgAudio` 那条常开流是**实测能出声**的（消爆音设计、输出开关回读校验都在里面），
  直接把 PCM 灌给它最稳，还顺带与音效共用一条流、无额外爆音。

实现要点（`src/platform/PgStream.cpp` + `PgAudio.{h,cpp}`）：
- `PgAudio` 新增 400ms 环形队列（`streamOn/streamWrite/streamQueuedFrames`），音频线程**优先**播它、
  空了补静音（不让声卡欠载出咔哒声）；
- 解码器**白名单**：`avcodec_find_decoder` 桩改成只放行 `aac / mp3 / pcm_s16le`
  （仍然不碰 `allcodecs.o`，体积 +340KB：`libzkgui.so` 2.51MB → **2.85MB**）；
- 重采样用 `libswresample`（`swr_convert`），采样率/声道/格式任意 → 22050/单声道/S16；
- 支持**纯音频源**（无视频流时只走音频链路，跳过 MPP/硬解/显示）。

### ⚠️ A/V 同步：**帧必须"挂起"，不能阻塞等**
第一版我用"帧没到点就 `usleep` 等音频时钟"——结果整条链路掉到 **0.4 倍实时**、伴音断续。
根因：等待期间同一线程没在读包喂音频 → 音频环被抽干 → 音频时钟不再前进 → **越等越等不到**。
**正确结构**（现在的实现）：
```
(A) 有挂起帧 → 到点才送显示（音频时钟为准；等超 500ms 硬送兜底）
              不到点则**不阻塞**，继续往下走
(B) 没有挂起帧 → 非阻塞 GetImage 取一帧挂起
(C) 读一个包（音频包就软解+重采样+灌 PCM，队列满就等 3ms 重试 —— 这一等就是全链路的实时节流器）
```
另外：音频环一开始给了 1s，结果**画面跑到网络前面 1 秒多**、和上面的等待互相拖死；
缩到 **400ms** 后稳定。`sAudioFrames` 必须**每轮流清零**（时钟 = 已推进 - 队列，跨流累积会失真）。

### 实测（v1.7.5）
- **本地 12s 测试音**（`streamtest file:///tmp/tone12s.wav`）：`音频就绪 pcm_s16le 22050Hz 1ch`
  → `总时长 12000 ms` → 播完 **用时 11602ms ≈ 实时** ✓（可闻 440Hz 长音）
- **在线流**：`音频就绪 aac 48000Hz 2ch → 22050Hz 单声道 S16`，`声卡时间轴` 按 20ms 步进；
  CPU 约 **17% 单核**（视频硬解 + 软解 AAC + 显示一起）。
- ~~⚠️ 带宽是当前瓶颈：实测流媒体期间设备 wlan0 实收只有 ~799 kbps……~~
  **⚠️ 这条结论是错的，2026-09-13 已用对照实验推翻，见 §十三.4。**
  真实情况：链路能力 **9~67 Mbps**（取决于 AP），而流媒体期间 RX 低是因为**我们自己把读取
  节流到了消费速度**（自限流），不是无线链路的锅。

---

## 十三、2026-09-13：把"应用莫名重开/越播越卡"追到根 —— **内核 OOM**，以及三个链路 bug

> 一句话结论：本板 **56MB 内存 + 内核未开 memory compaction**，**1280x720 硬解必然触发
> 内核 OOM**（连厂商框架播放器也一样）；被 OOM `SIGKILL` 之后**内存不会自己回来**
> （28MB→11MB→6MB→1MB），设备会进入"起来就死"的重启循环，**只能重启机器**。

### 13.1 症状与误判
现场症状是"**应用莫名重开一遍**"（`pidof zkgui` 的 pid 在变）、设备越来越卡。
因为工程里 `zkgui` 被 init 托管（重启就立刻拉起），只看画面很像"崩溃重启"，很容易误判成
"音频/ALSA 占用了什么导致的"。**排查第一步永远是：`pidof zkgui` 看 pid 有没有变 + 看内核日志**。

内核日志（本板 `dmesg` 命令不存在，用 `/tmp/busybox dmesg`；完整取证见
`docs/oom-2026-09-13/dmesg-full.txt`）：
```
[ 833.807] VE: before freq=200000000
[ 833.807] VE: real freq=400000000            ← 视频引擎升频（720p 硬解开始）
[ 834.095] VDecChn0 invoked oom-killer: gfp_mask=0x242c2c2, order=2, oom_score_adj=0
[ 834.095] COMPACTION is disabled!!!          ← 本内核没开内存规整
           __alloc_pages_nodemask ← ion_page_pool_alloc ← ion_system_heap_allocate
                                 ← __ion_alloc ← ion_ioctl
[ 834.095] Normal free:2520kB min:940kB ... present:65536kB managed:56632kB
[ 834.095] Out of memory: Kill process 1267 (zkgui_ui) score 100 or sacrifice child
[ 834.102] oom_reaper: reaped process 1267 (zkgui_ui)
```
- **总内存只有 56MB**（`MemTotal 56632kB`），`COMPACTION is disabled`：只要出现一次较大的
  连续 ION 申请就会直接进 OOM，没有"挤一挤"的余地。
- 触发点是 `VDecChn0` 申请解码缓冲（`ion_ioctl`）——**不是** 我们的 malloc，也不是 ALSA。

### 13.2 定量：不同分辨率要多少内存（真机实测）
| 源 | 像素 | 结果 |
|---|---|---|
| 640x360 H.264 | 230k | ✅ 可播；池约吃掉 **15MB**，之后**同分辨率再播内存一点不涨**（池复用）|
| 960x400 H.264 + AAC | 384k | ✅ 可播（同样复用）|
| **1280x720 H.264** | 922k | ❌ **1 秒内 MemFree 23MB→2.5MB → OOM 杀进程** |

**对照实验（关键）**：把同一个 720p 文件交给**厂商框架播放器**（`cast /tmp/xx.mp4`，
zkmedia 自己的 vdec/vo 通道）——它同样 `create vdec channel[0] success` 之后被杀，
pid 连续变、`MemFree` 掉到 2.7MB。⇒ **不是我们的 MPP 参数没调好，是这块板子的物理限制。**

### 13.3 修复：**先算再开**，绝不让内核 OOM 杀进程
`PgStream.cpp` 新增内存守卫（`guardDecodeMemory()`），在 `VDEC_CreateChn` **之前**判断：
- **像素硬上限** `kMaxDecodePixels = 960*544`（约 52 万像素）。超了明确拒绝并打日志，
  绝不"试一下"——被 OOM 杀一次的代价（漏不可回收内存 + 重启循环）远大于"播不了"。
- **内存地板** `kMinAvailKb = 7MB`：低于它说明设备已被 OOM 打残（内存不会自己回来），
  直接拒绝并提示**重启设备**。
- ⚠️ 判据**只认像素上限**、不拿 `MemAvailable` 卡"够不够"：实测同分辨率第二轮播放内存一点不涨
  （池复用），但播过一轮后 `MemAvailable` 会从 28MB 掉到 11MB —— 按"够不够"判会把第二轮自己拒掉。
- 现场标定：QA `streammax <像素数>`（`streammax 921600` 可强行放开 720p，用于验证/换板测试）。
- 其它省内存的改动：码流缓冲从 `w*h*3/2`（≥1MB）改成 `w*h/4` 并夹到 **[256KB, 768KB]**；
  `VDEC_SendStream` 遇 `0xa005800f`（缓冲满）**重试而不是丢包**（缓冲小了背压会更常见）。

**验收**：720p 现在被干净拒绝 —— 日志 `拒绝播放 —— 分辨率 1280x720（921600 像素）超过本板安全
上限 522240 像素…`，且 **pid 不变、MemAvailable 不变、无 OOM**（修复前是必被 SIGKILL）。

### 13.4 顺带纠正一条旧结论：**带宽从来不是瓶颈**
用 busybox `wget` 拉 20MB 文件量设备 wlan0 的真实收包速率（`tools/netrate.sh`，**不经过我们
的解码链路**）：

| 网络 | 实测吞吐 |
|---|---|
| `zkswe-soft_5G`（5180MHz ch36，真 5G） | **53–67 Mbps** |
| 原 SSID（5G） | **26–36 Mbps** |
| `zkswe-soft`（**实为 2.4G**，2412MHz ch1） | 9–25 Mbps |

而最强的源也只要 4 Mbps ⇒ **带宽富余一个数量级**。旧结论里"流媒体期间 RX 只有 799kbps"
是**我们自己把读取节流到消费速度**的表现（自限流），不是链路。
顺带记一笔：`zkswe-soft` 是 2.4G，`zkswe-soft_5G` 才是 5G（扫频实测 `freq=5180`）。

### 13.5 三个链路 bug（都是这轮修掉的）
1. **无音轨源 → 画面只有 2fps**（最隐蔽）
   `audioClockMs()` 在"没有音频轨"时返回的是 `(0 帧 - 0 队列) = 0ms` —— 一个**永远不前进的
   合法时钟**。于是每帧都判"没到显示时刻"，每帧都等满 500ms 兜底硬送。
   **修**：没有音频轨（或音频解码器打不开）时置 `sAudioOff=1`，时钟退回墙钟。
2. **读循环狂奔 → 片子只播 1/5 就结束**
   有音频时是"音频环满了挡调用方"把读取节流到实时；**没有音频就没有节流器** ——
   实测 10s/300 包的片子 **301ms 就把 300 个包全读完**（解出 7 帧就 EOF）。
   **修**：① 无音频时"帧没到点就不读包"（`continue`）；② 加**读窗口**：最近读到的视频包
   PTS 超过 `已播时间 + 1.5s` 就停读。
   ⚠️ 读窗口**不能用"已送包 - 已出帧"的帧数差**判：那是死锁（解码器要更多输入才吐下一帧，
   窗口一关它没输入、又永远吐不出帧 —— 实测卡在第 1 帧、CPU 空转）。
3. **帧 PTS 用"最近读到的包"→ A/V 漂移**
   硬解是异步的，取出的帧对应的是**前几个**包。改用 **FIFO**（送包时入队、出帧时出队），
   并把 B 帧造成的 PTS 倒退**钳成单调**。
   **修后实测**（960x400 + AAC，5G）：`368 帧 / 14.1s ≈ 26fps`，
   `解码帧 #240 → 墙钟 9143ms / 音频时钟 9948ms / 本帧 PTS 10051ms` —— 音画同步在 ~100ms 内。

### 13.6 声卡占用（回答"是不是你占用 ALSA 导致的异常"）
**占用是真的，但不是 OOM 的原因，两者要分开说。**
- 现象：常开流会**长期独占 `hw:0,0`**。实测此时别的进程打开 card0 会**挂住而不是报错** ——
  `/bin/tinyplay /tmp/tone6s.wav -D 0` 跑 8 秒被 timeout 杀掉、**无任何输出**（rc=143）。
  如果设备上有别的程序要出声（或产测脚本要放音），就会表现为"卡住"。
- 为什么不做成"用完就关"：爆音（pop）来自反复 open/close PCM，常开流是**音效无 pop** 的前提。
- **修**：给出显式"让出/拿回"能力 —— QA `pcmfree` / `pcmopen` / `pcmstate`。
  实测闭环：
  - `pcmfree` → 日志 `已释放声卡 hw:0,0`，`/proc/<pid>/fd` 里 `pcmC0D0p` **数量 0**；
    随后 `tinyplay` 立刻正常播放（`Playing sample: 1 ch, 22050 hz, 16 bit`，rc=0）。
  - 释放期间在线流**自动降级为只播视频**（墙钟同步），不会因为"喂不进队列"而空转卡死
    （`streamOn()` 返回 false 即降级；队列 450ms 收不进也降级）。
  - `pcmopen` → `已拿回声卡 hw:0,0（常开流恢复）`，fd 回到 1。
- ⇒ **空闲自动让出（v1.7.7 起默认开启，30 秒）**：`mainLogic` 的 `tickPcmIdle()`
  每 500ms 检查一次，满足「阈值 > 0 + 没有在线流在跑 + 距上次"要用声音"超过阈值」就让出；
  拿回是**按需**的：`GameHostImpl::playSfx` 先 acquire（**实测耗时 13ms**，几乎无感），
  `PgStream` 起流前也会试一次（拿不回才降级为只播视频）。
  开关：QA **`pcmidle <秒>`**（`0` = 关，回到"永远常开"）。`pcmstate` 可看当前阈值与已空闲时长。
  真机闭环实测：

  | 步骤 | 结果 |
  |---|---|
  | 默认（阈值 30s）静置 35s | 日志 `声卡空闲 30s（阈值 30s）→ 让出 hw:0,0`，`/proc/<pid>/fd` 里 `pcmC0D0p` **1→0** |
  | 让出后触发音效（`sfx 3`） | `已拿回声卡 hw:0,0（常开流恢复）` + **`拿回耗时 13ms`** + `播放 audio/rotate.wav`，fd 回到 1 |
  | 让出后直接起在线流 | `声卡当前未被占用（空闲让出过）→ 先拿回` → `音频就绪 aac 48000Hz` → 帧/伴音正常，fd=1 |
  | `pcmidle 0` 后静置 | 一直持有（fd 恒 1）—— 特性可关 |
  | `pcmidle 5` 后静置 9s | `声卡空闲 10s（阈值 5s）→ 让出`，fd=0 |

### 13.7 测试台架的一个坑：**服务器必须支持 Range**
`python -m http.server` **不支持 Range 请求**。而很多测试片（filesamples 的 `sample_*.mp4`）
把 `moov` 放在文件尾部，ffmpeg 必须发 Range 去尾部取 `moov`；服务端忽略 Range 就会让 ffmpeg
解析错乱 —— 现象是 `av_read_frame` **第一次调用就返回 EOF**（日志 `送 0 包 / 解出 0 帧`），
看起来像"播放器坏了"，其实是台架问题。
**用 `tools/range_http.py`**（自带 Range、支持 206）：
```
python tools/range_http.py 8099 <目录> 192.168.0.110   # 第三个参数 = 绑定网卡地址
```
⚠️ 另外两个台架细节：Windows 下**绑 `0.0.0.0` 会被防火墙拦**（设备侧表现为 `Operation timed
out`），要绑具体网卡地址；`adb push` 只认 **Windows 盘符路径**。

### 13.8 本轮新增的自检命令
| 命令 | 作用 |
|---|---|
| `pcmstate` | 打印"声卡是否被我们占用 + backend" |
| `pcmfree` / `pcmopen` | 让出 / 拿回声卡 `hw:0,0`（让出后其它进程才能用声卡） |
| `streammax <像素数>` | 现场标定"允许硬解的最大像素数"（0 = 恢复默认 960x544） |
| `memstate` | 打印 `MemAvailable`（判断"是否被 OOM 打残了、要不要重启"） |

配套脚本：`tools/netrate.sh`（原始吞吐）、`tools/rxwatch.sh`（收包+CPU）、
`tools/playwatch.sh`（收包+CPU+**MemFree+pid**，pid 变即 OOM 重启）、`tools/range_http.py`（带 Range 的测试服务）。

### 13.9 现场处置三条（写给后续排查的人）
1. 设备"变卡/应用反复重开" → 先 `pidof zkgui` 看 pid、再看 `/tmp/busybox dmesg | tail`：
   有 `Out of memory: Kill process ... zkgui_ui` 就是 OOM，**重启设备**（内存不会自己回来）。
2. 播放失败先看日志里有没有 `拒绝播放` —— 那是内存守卫在保护你，不是链路坏了。
3. 换新片源测试时优先用 **≤960x544、≤4Mbps** 的源；720p 及以上在当前硬件上不要试。


### 13.10 固化 v1.7.6（2026-09-13）与验收
`tools/upgrade_device.sh 1.7.6` → `out/update.img` **1.7MB（1765948 B）**，
刷入后 `/res/lib/libzkgui.so` **2855028 B**（`/res/ui` 下 3 个 ftu + certs + audio + images），
`/res/etc/EasyUI.cfg` 指向 `/res`（`startupLibPath=/res/lib/libzkgui.so`、`touchDev=event4`），
`/tmp/lib` 不存在 ⇒ 跑的是 flash 版。

固化版真机验收：

| 项 | 结果 |
|---|---|
| 640x360 在线流 | `送 300 包 / 解出 281 帧 / 9248ms` ≈ 实时；pid 全程不变 |
| 720p 在线流 | **干净拒绝**（`拒绝播放 —— 分辨率 1280x720（921600 像素）超过本板安全上限 522240 像素`），pid 不变 |
| 本机 OOM 计数 | `dmesg \| grep -c "Out of memory"` = **0** |
| 内存 | 固化版启动后 `MemAvailable 31.4MB`（比 `fun launch` 调试版多 ~2.7MB —— 资源在 squashfs 里，不占 tmpfs）|

> 顺带一条：**固化版比调试版更省内存**（资源不进 tmpfs），所以现场排查内存问题时
> 应该以固化版为准，用 `fun launch` 复现时会把水位压低一截。

---

## 十四、2026-09-13 追加：**停播后收尾会永久卡死**（播放器直到重启不可用）+ 空闲让出声卡

> 起因：用户说"1-2 直接做"——① 声卡策略 ② 设备切回原网络。做①时按
> "让出/空闲让出"的思路复测，撞出两个更严重的问题。

### 14.1 🔴 `AW_MPI_VDEC_DestroyChn` 在"中途停播"后**永久阻塞**

**现象**：DLNA `Stop` 或 QA `streamstop` 之后，日志停在 `收尾 ③StopRecvStream ok`，
`DestroyChn` 与后续完全没有下文 —— **线程再也不退出**。后果比"一次播放失败"严重得多：
- `sRunning` 永不归零 → 之后**所有**播放都报 `PgStream: 已有任务在跑`，
  **播放器直到应用重启都不可用**（DLNA 投屏全废）；
- 声卡的"空闲自动让出"被 `running()` 一直挡住（见 14.3）。

**踩过的三条错路**（都试过，都没用）：
1. 先抽干解码输出再停（抽干只解决了"队列满"的疑点，DestroyChn 照卡）；
2. `AW_MPI_VDEC_SetStreamEof(chn,1)`（返回 0x0，但没用）；
3. 补发"空包 + `mbEndOfStream=1`"（同样返回 0x0，没用）。

**定位**：卡住时看进程内所有线程 —— 16 个线程全是 `S` 态、**没有任何 `D` 态（驱动等待）**，
多数停在 `futex_wait_queue_me` ⇒ 是 **libmedia_mpp 内部等锁**，不是内核驱动。
（`/proc/<pid>/task/*/wchan` + `/proc/<pid>/task/*/stack` 是这类问题的第一手证据。）

**正解 = 通道"常开"复用**（和 VO 一样的思路，厂商 zkmedia 退出时也不释放）：
- 每个流**只做 `VDEC_StopRecvStream`**，**不调 `DestroyChn`/`SYS_Exit`**；
- 下一个流如果**分辨率相同** → 直接复用通道（`ResetChn`（可选符号）+ `StartRecvStream`），
  连 `SYS_Init` 都省了（更快）；分辨率变了才 `Stop/Destroy` 旧通道再建新的。
- 实测：中途停播 → `收尾 ①②③` 全过 + `收尾完成（sRunning 已清零，可再次播放）`；
  紧接着再投一次 → 正常出帧（同一分辨率走复用路径）。

### 14.2 起流前拿回声卡：**必须在 UI 线程做**

14.3 的"空闲让出"会让声卡在空闲时被放掉，起流时得拿回来。**第一版把 `acquirePcm()`
放在播放线程里 —— DLNA 起流直接卡死**（没有画面没有声音、`sRunning` 也不归零）。
**正解**：把拿回动作放在 `StreamPlayer::startCommon()` 里 —— 它的调用方（QA 命令、DLNA 动作队列）
**本来就在 UI 线程**，而 UI 线程拿回声卡是已验证的路（音效 acquire 实测 **13ms**）。
播放线程里只保留"拿不到就降级为只播视频"的判断。

### 14.3 声卡"空闲自动让出"（v1.7.7 默认 30 秒）

见 §13.6 的表。补充两条实测：
- 拿回声卡 **13ms**（`openStream` + 输出开关枚举），听感无感；
- 让出期间起流 → `起流前拿回声卡` → `音频就绪 aac 48000Hz` → 帧/伴音正常。

### 14.4 全程进度看门狗（防"静默卡死"）

除了 14.1 那个已知卡点，还遇到过**起流阶段偶发静默卡死**（日志全无、线程不动）。
`StreamPlayer::running()` 里加看门狗：`sProgressMs` 在线程启动 / 开流完成 / 建链路完成 /
主循环每轮 / 收尾开始 都刷新，**任一阶段超过 40s 没进展就对外视为"已结束"**
（阈值 40s > `avformat_open_input` 的 `rw_timeout` 30s，不会误判慢站点）。
效果：卡死的任务最多让后续播放等 40s，不再"直到重启都不可用"；
一旦触发会打 `任务已 40s 无进展 → 对外视为已结束`，现场一眼可见。

### 14.5 本轮新增/变更的 QA 命令
| 命令 | 作用 |
|---|---|
| `streamstate` | 当前是否有在线流任务在跑（"已有任务在跑"到底是什么占着，一眼可见） |
| `pcmidle <秒>` | 声卡空闲让出阈值（默认 30；`0` = 关） |

### 14.6 固化
`tools/upgrade_device.sh 1.7.8` → `out/update.img` 约 1.7MB。
（v1.7.7 = 空让出特性；v1.7.8 = 通道常开复用 + UI 线程拿回 + 看门狗）

### 14.7 仍未彻底查清（交接给后续）
**起流阶段的偶发静默卡死**没找到根因（首次出现于"空闲让出过之后又起流"这类组合场景）。
现有兜底：通道复用（规避已知卡点）+ 40s 看门狗（放行后续任务）。
若要继续查，建议顺序：① 在 `[T1]~[T4]` 追踪点上再加更细的打点（`openInput` 前后、
`SYS_SetConf` 前后、`CreateChn` 前后）；② 卡住时抓 `/proc/<pid>/task/*/wchan` + `stack`；
③ 试把 `nAlignWidth`/`mkfcTmpDir` 之外的 `MPP_SYS_CONF_S` 字段也显式赋值（默认值未验证）。

---

## 十五、2026-09-13 定论：**停止过的解码通道回不来**（7 种方案实测）+ 自愈设计

> 这一节是 §14 的收口：把"停播后收尾卡死 / 第二轮流不出画"这件事查到了**平台限制**这一层。

### 15.1 关键事实（三条实测结论）
1. **`AW_MPI_VDEC_DestroyChn` 在"被停止过"的通道上必定永久阻塞**（线程停在 libmedia_mpp
   内部等锁；全进程线程都是 `S` 态、无 `D` 态）。**只有"流自己播完"（EOF / 到时长上限）时销毁才是安全的。**
2. **被停止过的通道即便只 `StopRecvStream` 再 `StartRecvStream`（不复用销毁）也不再出画**：
   `StartRecvStream` 返回 0x0、音频照常流，但**一帧视频都没有**；`VDEC_Query` 里
   "左码流/左待解帧"每轮还累加 ~1.25MB（内部状态坏了）。
3. **换新通道号也没用**：`CreateChn(1)` 返回 0x0，但同样不出画 ⇒
   本板**同时只有一个 VDEC 通道能真正工作**（创建成功 ≠ 能用）。

### 15.2 试过的 7 种方案（都记下来，避免后人重走）
| # | 方案 | 结果 |
|---|---|---|
| 1 | 中途停播 → 直接 `DestroyChn` | ❌ 永久卡（线程不退出 → 播放器直到重启不可用） |
| 2 | 先 `VDEC_Query` 排空到 0/0/0 再销毁 | ❌ 仍卡 |
| 3 | `SetStreamEof(chn,1)` 后再销毁 | ❌ 仍卡 |
| 4 | 空包 + `mbEndOfStream=1` 后再销毁 | ❌ 仍卡 |
| 5 | 先把在途帧全部送显示（优雅停流）再销毁 | ❌ 仍卡 |
| 6 | 只 `StopRecvStream`、通道常开复用（不销毁） | ⚠️ 不卡，但**下一轮不出画** |
| 7 | 通道号递增（脏通道不销毁、换号重建） | ⚠️ `CreateChn` 成功但**新通道也不出画** |
| 8 | 收尾顺序反转（先拆 VDEC 后停 VO）+ 全部常开 | ✅ **唯一"应用不死、可反复投屏"的组合** |

对照：**厂商框架播放器（zkmedia）连续播两次是好的**（各自 `create vdec channel[0] success`
+ `media play ok`），说明平台本身能做，但它的收尾序列在 libzkmedia 内部、看不到源码。
**建议**：把本节发给模组厂（Zkswe）要"VDEC 通道在手动停止后正确的 Stop/Destroy 序列"。

### 15.3 最终设计（v1.7.9 起）
1. **通道/VO/SYS 全常开**：每轮流只 `StartRecvStream` / `StopRecvStream`，**绝不 `DestroyChn`**
   （分辨率变化才重建，那一步有看门狗兜底）。
2. **停止 = 优雅停止（不再硬中止）**：`StreamPlayer::stop()` 置 `sFinishReq`，主循环不再读新包、
   发 EOS（`SetStreamEof` + 空包 EOS），用 `VDEC_Query` 看到「左图 0 / 左码流 0 / 左待解帧 0」
   才退出（兜底 1.5s）。实测放空只要 **7~16ms**。这样收尾必定干净、`sRunning` 必定归零。
3. **全程进度看门狗**：任一阶段 40s 无进展 → 对外视为已结束（放行后续播放），
   日志 `任务已 40s 无进展`。
4. **视频链路卡死自愈**（`tickVideoSelfHeal()`）：投屏中**音频在流但 4s 无视频帧** ⇒ 判定为脏通道，
   自动 `_exit(0)` 复位应用（zkgui 由 init 托管，~1s 拉起；90s 内最多一次防抖）。
   实测闭环：投屏① 出画 → 停止 → 投屏② 检出脏通道 → **自动复位（pid 变化）** → 再投一次**正常出画**。

**给现场的说法**：*"投屏 → 停止 → 再投屏"时，第二次可能先黑 4 秒然后自动回到主界面
（那是播放器在复位），**再点一次投屏就正常**。* 单次投屏（含长时间播放）不受影响。

### 15.4 ✅ 已实现：把复位提前到"停止"动作上（用户确认后做的）

**一按停止就复位应用**（~1s 后自己拉起），因此**第二次投屏不会黑屏**、也不用重投。
代价是每次停止界面闪一下（本来就回主界面，观感差别不大）。

实现：`mainLogic::tickStopReset()` —— 条件「投屏页已收掉 + 流任务已收尾 +
`StreamPlayer::consumeNeedsReset()` 为真」→ `_exit(0)`。**自然播完（EOF/到时长上限）不触发**
（那种通道是干净的）。真机验收：投屏出画 → 停止 → 日志 `投屏已按停止 → 复位应用`（pid 变化）
→ 再投**立刻出画**；`streamtest ... 5`（自然到上限）验证 **pid 不变**、不误触发。

---

## 十六、2026-09-13 追加：**画面旋转**（竖屏设备看横屏内容 / 手机竖拍视频）

> 需求原话：「视频画面是竖着的，视频播放的时候要把视频内容旋转 90 度。
> 方法在那个 v851 extscreen 项目也有参考」。

### 16.1 参考项目怎么做的

`S:/projects/LearningProject/V851ExtendedScreen_ap_p2p`（手机副屏/投屏工程）的做法是
**给播放器一个"旋转角度"**，语义统一为**顺时针档位**：

```cpp
// src/media/h264_player.h（ZKSWE 封装库）
enum disp_rot_e { E_DISP_ROT_0, E_DISP_ROT_90, E_DISP_ROT_180, E_DISP_ROT_270 };
int  zk_h264_player_init(int w, int h, enum disp_rot_e rot, int flag);
void zk_h264_player_set_rot(enum disp_rot_e rot);
```

它自己的调用（`src/link/context.cpp`）：`rot = enable ? E_DISP_ROT_180 : E_DISP_ROT_0`，
`video_rot_and_crop(rot)` —— 并且注释里写明**旋转后要重新裁剪**
（`swap(crop_w, crop_h)`，它那段被注释掉了）。

框架侧还有一个更直接的接口（`ZKVideoView.h`，本工程投屏页用的就是这个控件）：

```cpp
/* clockwise rotation: val=0 no rotation, val=1 90 degree; val=2 180 degree; val=3 270 degree */
void setRotation(int val);
```

⇒ **两边的语义完全一致（顺时针 0/90/180/270），也都是"把角度交给播放/显示链路"。**

### 16.2 先试的路：**VDEC 自带旋转**（❌ 失败，最终方案见 16.5 第 3 版）

本工程的视频链路是 `ffmpeg 解封装 → AW_MPI_VDEC 硬解 → AW_MPI_VO 上屏`，
所以等价接口在解码器这一层（设备库 `libmedia_mpp.so` **确实有**）：

```
AW_MPI_VDEC_SetRotate(VDEC_CHN, ROTATE_E)
AW_MPI_VDEC_GetRotate(VDEC_CHN, ROTATE_E *)
map_ROTATE_E_to_cedarv_rotation / VideoDecRotateFrame     ← 库内部按格式选 vdeclib 或 g2d
```

- 建通道时用 `VDEC_CHN_ATTR_S.mInitRotation`（**解码器直接吐转正的帧**，零额外内存/CPU）；
  通道已存在时（复用路径）改用 `AW_MPI_VDEC_SetRotate`。
- 与 `ROTATE_E` 一样是**顺时针**：`0=不转 / 1=90 / 2=180 / 3=270`。

**默认策略 = auto**：读容器里的旋转元数据（`AV_PKT_DATA_DISPLAYMATRIX`，
用 `av_display_rotation_get()` 换算；手机竖拍的 MP4 几乎都带这个标志 —— 不读它画面就是侧躺的）。
`ffmpeg` 给的是**逆时针**角度，代码里换成顺时针并归一到 0/90/180/270。

**手动覆盖**：QA `streamrot <0|90|180|270|auto>`，手动值**优先于**元数据（不叠加）。
存到 `PgStore`（`/data/pocketgame.dat` 的 `rot=` 一行）—— ⚠️ **必须持久化**：
本工程"停止投屏就会复位应用"（见 §十五），只放内存里的话用户设好角度一停止就丢了。

### 16.3 两条播放路径（本地文件那条走框架自己的旋转）

| 路径 | 谁在播 | 旋转接口 |
|---|---|---|
| 在线流（http/https，DLNA 直连） | 我们的 ffmpeg + MPP | `VDEC.mInitRotation` / `AW_MPI_VDEC_SetRotate` |
| 本地文件（DLNA 下载后播） | 框架 `ZKVideoView`（zkmedia） | `mCasterPtr->setRotation(档位)` |

⚠️ 本地文件那条路**只在"手动指定过角度"时才调 `setRotation`** —— auto 模式下交给
播放器自己处理视频自带的旋转标志，否则会和它的内建行为叠加、**转两遍**。

### 16.4 真机实测（2026-09-13，设备 192.168.0.125）

| 场景 | 旋转前 | 旋转后（`streamrot 90`） |
|---|---|---|
| 在线流 `oceans_960x400.mp4` | 帧 `960x400` | 帧 **`416x960`**（宽高交换；416 = 400 按 16 对齐） |
| disp 视频层（在线流） | `fb[960,400]` | `fb[ 416, 960] crop[0,0,480,700] frame[0,0,480,700]` |
| 框架播放器 `/tmp/bb360.mp4` | `fb[640,360]` | `fb[ 384, 640] crop[0, 0, 360, 640] frame[0,0,480,700]` |
| 日志 | — | `解码旋转 = 90°（mInitRotation=1）` / `框架播放器旋转 = 90°（档位 1）` |
| A/V 同步 | — | 墙钟 2400ms / 音频时钟 2394ms（旋转不影响同步） |
| 持久化 | — | `/data/pocketgame.dat` 出现 `rot=90`，重启应用后 `旋转 -> 固定 90°` |

`streamrot` 无参数时打印当前状态：
`streamrot -> 生效角度 90°（视频自带元数据 -1°，-1=无） 硬件旋转=支持`。

### 16.5 ⚠️ 踩坑全过程（三版方案，**别再重走**）

**第 1 版：VDEC 硬件旋转**（`attr.mInitRotation` / `AW_MPI_VDEC_SetRotate`）→ **失败**
- 帧的宽高确实换了（`960x400 → 416x960`），但**像素全是 0（黑帧）**；
- `VDEC_SetRotate` 在"通道已存在"时返回 `0xa0058009`，且**让复用通道彻底不出画**；
- MPP 内部**一个错误都不报**（CreateChn/SendFrame 全 0x0）。

**第 2 版：以为 DE 报错是根因** → **误判**
- 黑帧时内核刷 `DE invalid address: 0x49b5a000 / L2 PageTable Invalid`，看着像"缓冲没映射进显示引擎"；
- 但做 0° 对照才发现：**0° 播放同样报 `DE invalid address: <某地址>`** ⇒
  这是**本平台 DISP 的固有噪声**（每次图层配置都会刷），不能拿它当旋转失败的证据。
  ⚠️ 教训：**内核日志要先做基线对照再下结论**。
  真问题只有一条：**帧的像素数据是 0**。

**第 3 版（现方案）：解码器照常出 0° 帧，我们自己转正后再送显示** → **成功**
1. 目标缓冲用 **`AW_MPI_SYS_MmzAlloc_Cached`**（MMZ）分配 —— 普通 malloc 的内存 DE 访问不到；
2. CPU 转置（16x16 分块，缓存友好），Y/色度平面分别转；
3. 构造 `VIDEO_FRAME_INFO_S` 送给 VO。
   （`rotPlane()` 统一支持 90/180/270，见 §16.6；两平面 NV12 源**不转**（宁可不转也不转错）。）

⚠️ **构造帧结构时踩的两个坑**：
- **必须按解码器的实际平面布局填**：本板 VDEC 输出 **YUV420P 三平面（I420）**
  （实测 `phy[0x48f80000, 0x48fbc000, 0x48fcb000]`，U→V 距离 = (W/2)*(H/2)）。
  只填 Y+UV、第 3 个留 0 → DE 去读第 3 平面拿到地址 0 → `DE invalid address: 0x0`，那片就黑。
- **`mStride[1]/[2]` 报的值不可信**（报文里是 640，实际色度行宽是 W/2=320）⇒ 色度跨距自己按半宽算。
- **结构先整体复制解码帧再覆盖**（`out.VFrame = src.VFrame`），别用 `memset(0)` 从零构造 ——
  里面有 DE 要用的字段（offset 等），清掉就废。

### 16.6 支持的角度：90 / 180 / 270（顺时针）

`rotateInto()` 用统一的角度参数实现（`rotPlane()` 按角度选映射），
**180° 不交换宽高**（`ensureRotBuf` 按角度决定目标尺寸），其余两个交换。

**四个角度的几何验证**（每个角度都单独重启应用、抓帧，再与 0° 帧的各种变换比相关性）：

| 实拍帧 | vs 0°帧"顺时针90°" | vs "逆时针90°" | vs "180°" | vs "不转" |
|---|---|---|---|---|
| **90°** | **0.899** ✓ | -0.088 | -0.136 | -0.129 |
| **180°** | -0.125 | -0.134 | **0.873** ✓ | -0.091 |
| **270°** | -0.089 | **0.902** ✓ | -0.127 | -0.133 |

⇒ 只有"对应的那个变换"高度相关（0.87~0.90），其余全在 ±0.13 附近 —— 角度与方向都对。
（帧尺寸也随之验证：0°/180° → 640x384；90°/270° → 384x640。）

### 16.6.1 ⭐ 怎么"看到"播出来的画面（视频层不在 fb0）

**`/dev/fb0` 只是 UI 层（disp ch2），播放出来的视频在 disp ch0 —— 截图工具抓不到它。**
全志平台自带一个 disp 节点可以直接 dump **合成后的整屏**：

```bash
# ⚠️ 写入值是**文件路径**，不是 0/1（写 1 会失败，只在 dmesg 留 `open 1 err`）
echo "/tmp/cap.bin" > /sys/class/disp/disp/attr/capture_dump
adb pull /tmp/cap.bin        # 480x800x4 = 1536000 字节 BGRA
```

实测（同一位置两张对比）：**主界面 vs 播放中，视频区 y0-700 差异 91.4%、底部 UI 区 0.0%**
⇒ 里面确实含视频层。用途：验证"视频区是不是黑屏"、以及把视频区与自己的抓帧做相关性判断方向。

### 16.7 真机验收（2026-09-13，设备 192.168.0.125）

| 项 | 结果 |
|---|---|
| 帧结构 | `[旋转帧(三平面I420)] phy[0x49980000,0x499bc000,0x499cb000] stride[384,192,192] 384x640` |
| **旋转正确性** | 抓 0° 帧(160x96) 与 90° 帧(96x160)，把 0° 帧**顺时针转 90°** 后对比：**相关系数 0.899**；<br>逆时针只有 **-0.088**；不转/180° 尺寸都不匹配 ⇒ **确实是顺时针 90°，实现正确** |
| 帧率 | 60 帧 / 1982ms ≈ **30fps**（转置没拖慢播放） |
| CPU | 播放期间空闲 79~93% ⇒ 转置开销可接受（单核 A7） |
| 内存 | 旋转缓冲 **480KB**（按 Y 的 2 倍申请，见下） |
| 持久化 | `/data/pocketgame.dat` 的 `rot=90`，重启应用后 `画面旋转 = 90°` |

**缓冲为什么要按"Y 的两倍"申请**：按 YUV420SP 理论大小（×3/2）分配时，DE 会读到缓冲**末尾之后**
（内核报 `DE invalid address: <缓冲末尾>`）。多留半个 Y 平面当安全带，本板这点内存换稳定很值。

### 16.8 顺带修掉的产品缺陷：自然播完后**必须销毁解码通道**

`PgStream` 收尾原来是"通道常开、只 StopRecvStream"（为了绕开"中途停播时 DestroyChn 永久卡住"）。
但实测发现：**只 Stop 不销毁，同一进程里下一轮再播放会不出画**（`VDEC_Query` 报
`左待解帧 300`、解出 0 帧）—— 症状就是"**一个视频自动播完 → 再投一个 → 黑屏**"，
用户必须重启应用。

现在按"退出原因"分开处理（见 §十五的结论）：
- **自然结束**（EOF / 到时长上限）→ `DestroyChn + SYS_Exit`（这种通道是干净的，销毁安全）
  → 下轮重建，**不会再踩"复用不出画"**；
- **中途被停止** → 通道拆不干净，保持不动，交给 `mainLogic::tickStopReset()` 的"**停止即复位**"。

实测：自然结束 → 立刻再播 → `CreateChn -> 0x0` + 正常出帧 ✓

### 16.9 QA 命令

| 命令 | 作用 |
|---|---|
| `streamrot` | 打印当前生效角度 + 视频自带元数据角度 + 硬件旋转能力 |
| `streamrot <0\|90\|180\|270\|auto>` | 设置角度（落盘，重启应用仍生效）。`auto`=跟随视频自带旋转标志 |
| `streamgrab [帧号]` | **把"真正送去显示的那一帧"降采样成 `/tmp/pgframe.pgm`**（PGM=P5 灰度）。<br>⚠️ 视频层不在 `/dev/fb0`（那是 UI 层），**截图抓不到播出来的画面** —— 验收旋转方向只能靠它 |

### 16.10 连续投屏稳定性（2026-09-13 又挖出两个 bug，都修了）

用户让"继续"时顺手做了一次"**连续投多个视频**"的真实场景测试，结果挖出两个问题
（都会表现成"**播放器越用越不对 / 每播完一次应用就像重启了**"）：

**Bug ① 自然结束收尾时调 `SYS_Exit()` → 之后必崩（SIGSEGV）**
- 现象：`⑤自然结束 → DestroyChn + SYS_Exit ok` 之后紧跟
  `!!FATAL!! sig=11 fault_addr=0x14 pc=0x4105b94c`，应用被 init 拉起（**pid 每轮都变**）。
- 根因：**VO 的层/通道在本工程是"常开"的**，而 `SYS_Exit()` 会把 MPP 全局资源一起释放 ——
  VO 那边还引用着 → 收尾之后一碰 VO 就段错误。
- 正解：**`DestroyChn` 可以调（自然结束的通道是干净的），`SYS_Exit` 绝对不要调**
  （SYS_Init 只需一次，保持常开）。
- ⚠️ 这个坑还有一个副作用：它让人**误判**成"复位逻辑不对"或"OOM" ——
  因为表现都是"pid 变了"。**排查时必须看 logcat 里的 `!!FATAL!!` 那行**。

**Bug ② "中途停止"的判据漏了一条路径 → 收养脏通道 → `DestroyChn` 永久卡住**
- 现象：QA `streamstop` 之后收尾停在 `④StopRecvStream ok` 再无下文，
  `streamstate` 仍报 `running=1`，要等 40s 看门狗才放行。
- 根因：判断"中途被打断"只看了 `sStopReq`（硬中止），而 **DLNA Stop / QA streamstop 走的是
  `sFinishReq`（优雅停止）** → 被误判成"自然播完" → 对"被停止过"的通道调 `DestroyChn`
  → 本板那种通道**必定永久卡住**（§十五的老结论）。
- 正解：`abortedMidway = (sStopReq != 0) || userStopped` ——
  **把两条路径都算上**；只有真正播完（EOF / 到时长限制）的通道才允许销毁。
- 顺带：`StreamPlayer::stop()` 在流已结束时直接 return（收尾阶段也会被调到，
  否则会把 `sFinishReq` 置 1、污染"退出原因"的判断 → **每播完一次就误复位一次**）。

**验收（v1.8.3，真机）**

| 场景 | 结果 |
|---|---|
| 连续 **5 轮**自然播完（同一进程，不重启） | **pid 全程不变（1255）**、每轮都有解码帧、`FATAL=0` |
| 播放中途 `streamstop` | 收尾干净（日志 `（中途停播）` + `⑤通道 0 是中途停播的 → 保持不动`），<br>**8 秒内自动复位**（`投屏已按停止 → 复位应用`），无需等 40s 看门狗 |
| 复位后再投 | 正常出画（全新通道） |

**内存观察（同一轮测试）**：重启后 `MemAvailable` **32MB** → 投屏一次后 **13.5MB** →
之后**稳定在 13.5MB 不再下降**（不泄漏；那 18MB 里大部分是可回收的页缓存/驱动缓冲）。
⚠️ 所以**内存守卫会在设备跑久之后误拒播放**（可用 < 7MB 时直接拒绝并提示"重启设备"）——
这是设计行为，见 §十三。

### 16.11 内存不足时"先自救再拒人"（v1.8.4）

**问题**：内存守卫原来在 `MemAvailable` 低于安全线时**直接拒绝播放**并提示"请重启设备"
（§13 的设计）。但设备连续投屏几次后 `MemAvailable` 会掉到 7MB 左右 ——
用户就得重启设备才能继续，体验很差。

**关键实测（2026-09-13）**：那些内存**大部分是可回收的页缓存**：

| 状态 | MemFree | MemAvailable | Buffers | Cached |
|---|---|---|---|---|
| 连投两次后 | 1.7MB | **14.0MB** | 4.0MB | 11.5MB |
| `echo 3 > /proc/sys/vm/drop_caches` 后 | 24.6MB | **31.4MB** | 0.02MB | 10.3MB |

⇒ 一次回收能拿回 **~17MB**，而应用是 **root（uid 0）**，`/proc/sys/vm/drop_caches` 可写。

**做法**：守卫在"本来就要拒绝"时，**先 `sync()` + `drop_caches` 再判一次**，
够就放行；确实不够才拒绝（拒绝文案也改成"已尝试回收页缓存仍不足"）。

真机验证：

```
内存守卫 640x360 预计占用 ~16448kB，可用 10900kB
可用内存偏低（10900kB < 20000kB）→ 回收页缓存后 27716kB（已放行）
解码帧 #60 宽高=640x384 ...            ← 播放正常
```

也验证了"回收后仍不足"的分支（把门槛人为设成 40MB）：
`回收页缓存后 28048kB（仍不足）` → 拒绝，文案正确。

**QA**：`memguard <kB>` 临时改这个安全线（默认 7168kB；`0` = 恢复默认）。
主要给现场标定/验证用 —— 例如 `memguard 20000` 能把"回收逻辑"逼出来。

> 注：只在**内存不足时**才回收（不是每次起播都回收），避免频繁清缓存拖慢整个系统。

### 16.12 真实链路（DLNA 手机投屏）端到端验收

前面 §16.6~16.11 都是用 QA 命令（`streamshow`）驱动的；这里走**产品实际用的那条路**
（控制器 `SetAVTransportURI` → `Play` → `Stop`），确认旋转在真实链路上同样生效。

| 步骤 | 结果 |
|---|---|
| 投 `http://…/oceans_960x400.mp4` | `SetAVTransportURI -> HTTP 200` / `Play -> HTTP 200`；<br>`PgDlna: 在线流直连（不下载、不落盘）` → `状态 -> PLAYING` → **`画面旋转 = 90°`** |
| 屏幕内容（`capture_dump`，§16.6.1） | 视频区 (0,0,480,700)：**均值 108.6 / 非零 97.8%**（有画面，不是黑屏）；<br>底部 UI 区均值 33.8（投屏页在） |
| 抓帧（`streamgrab`） | 源 **384x640**（= 960x400 旋转后的尺寸）✓ |
| `Stop`（播到一半） | `⑤通道 0 是中途停播的 → 保持不动` + `收尾完成` + **8 秒内复位**（pid 719→962） |
| 复位后再投 | `状态 -> PLAYING` + `画面旋转 = 90°` + `解码帧 #1 / #60` ✓ |

> 测这一项要注意**源时长**：第一次用 10 秒的片子，`cast` 之后 8~10 秒才 dump，
> 视频已播完收页 → dump 出来是**全黑**，差点误判成"投屏没出画"。
> 换 27 秒的 `oceans` 并提前 dump 才是有效对照。

---

## 十七、2026-09-14：**"又没有旋转 90 了"** —— 旋转不能只靠一份存档

**用户现象**：播放视频（IPTV / 在线流）画面**横躺着**，而"以前是转正的"。

### 17.1 真因：旋转的唯一防线是 `/data/pocketgame.dat` 里的一行 `rot=90`

`StreamPlayer` 里角度是按这个优先级定的（`desiredRotation()`）：

```
手动值（QA streamrot / 存档 rot=） > 容器元数据 > 兜底
```

而当时：**存档里是 `rot=-1`（auto），容器元数据也没有**（直播流/普通 mp4 都不带旋转标志）
⇒ 命中**兜底 0°** ⇒ 不转。

那 90 是怎么丢的？这套设计有两处脆：

| 脆点 | 说明 |
|---|---|
| **存档读到缺 `rot` 行的旧文件** | `load()` 里未出现的键保持**默认 -1**；而 `~ScoreStore()` 退出时会 `save()` → **反手把 /data 里原来的 `rot=90` 覆盖成 -1**。代码注释里其实记过同类事故（"别从哪读的就写哪，否则永久存错位置"），这次是同一个坑的另一面 |
| **兜底 0° 对竖屏设备无意义** | 本机 480x800 **竖屏**，内容绝大多数是横屏；横屏源**不带元数据** ⇒ auto 等于"永远不转"，只有用户手动设过才看得见正画面 |

一句话：**"用户手动设过并成功落盘"被当成了唯一防线**，一旦存档被改写就"又没了"。

### 17.2 修法：兜底从 0° 改成 **90°**（竖屏设备的合理默认）

```cpp
int desiredRotation() {
  if (sRotManual >= 0) return sRotManual;   // 手动优先
  if (sRotSource >= 0) return sRotSource;   // 有元数据：跟随
  return 90;                                // ★ 无手值、无元数据 → 竖屏默认转 90°
}
```
- `StreamPlayer::rotationDeg()`（QA `streamrot` 打印用）**同步改成同样的兜底** ——
  否则 QA 显示 `-1°` 而实际生效 90°，排障时会被自己误导。
- **只影响"我们自己解码"的那条链路**（IPTV / DLNA 在线流 / h264 直推）。
  DLNA 播**本地文件**走框架 `ZKVideoView`，auto 时仍交由播放器自己处理（§16.3），
  不被这里改到 —— 避免和它的内建旋转标志叠加转两遍。
- 想固定不转：QA `streamrot 0`。

### 17.3 真机验收（2026-09-14）

| 步骤 | 判据 | 实测 |
|---|---|---|
| 只给兜底（存档仍是 `rot=-1`） | QA 打印 | `streamrot -> 生效角度 90°（源自带元数据 -1°，-1=无；未手动指定且源无元数据时按竖屏默认 90°）` ✅ |
| 播在线流 `oceans.mp4`（960x400，无元数据） | 日志 | `PgStream: 旋转 -- 源元数据=未指定 手动=无（auto） → 本次用 90°` ✅ |
| 同上 | **disp 硬件层** | `vp crop(0,0,960,400)` → **`dst crop(0,0,400,960)`**（宽高交换）<br>`fb[400,960] crop[0,0,400,960] frame[94,0,291,700]` ← 转正后等比放进 480x700 ✅ |
| 解码在跑（不是黑屏） | 日志 | `解码回调 #240 —— fmt=5 960x416 crop(0,0,960,400)` ✅ |
| 顺手恢复用户原来的手动值 | 存档 | QA `streamrot 90` → `/data/pocketgame.dat` 的 `rot=90` 回来了（**本地文件投屏那条路也跟着转**）✅ |
| IPTV 链路 | 日志 | `起流 [0] … → 旋转 -- 源元数据=未指定 手动=90° → 本次用 90°` ✅ |

> ⚠️ 当天那个外网 IPTV 源本身挂了（`连接超时（已等 35 秒仍无画面）`，前一轮同源 13KB/s 要 20s 才出画）——
> IPTV 那一跳只验到"角度按 90° 下发"，**出画后的 disp 层是用 `oceans.mp4` 验的**（同一条 `desiredRotation()` 路径）。

### 17.4 教训（比这个 bug 本身重要）

1. **"用户设置"不能是唯一防线**：设备有一个**合理默认**（竖屏设备 → 90°），
   用户设置只做**覆盖**。反过来设计的话，每次存档被改写/迁移就"功能又没了"，
   而且用户描述一定是"**又**不转了"这种没法定位的说法。
2. **默认值与"QA 打印的当前值"必须同源**（本次 `rotationDeg()` 差点漏改）。
3. 排障口诀：**先看 `desiredRotation()` 的三个输入（手动 / 元数据 / 兜底）分别是什么**，
   `PgStream: 旋转 -- 源元数据=X 手动=Y → 本次用 Z` 这一行日志就是全部信息。\n
---

## 十八、2026-09-14：视频区"缓冲底图"（投屏页 / IPTV 播放页）

用户原话：「视频缓冲还没有解码的时候，视频需要放一个背景图，不然图层透明过去配上黑色的
会有字体模糊的问题。」

**机制**：`videoview` **不是普通控件** —— 它是 UI 层给下层 disp 视频层开的**透明窗口**
（控件区域 UI 层不填充，视频层有数据就从这里透出）。所以"视频层还没数据"的那几秒
（DLNA 打开中 / IPTV 拉清单等首帧 / 报错），屏幕上是**纯黑 + 一层发虚的字**。

**做法**：该区域铺一张**不透明**底图，出画立刻收起。

```html
<videoView Caster 480x700>          <!-- 透明窗口：视频从这透出 -->
<icon ImgCastCover data-pic=images/video_cover.png 480x700 touchable=false visible=false>
<text  TextCastMsg ...>              <!-- 状态字在底图之上 -->
```

- 图：`tools/gen_ui.py::gen_video_cover_asset()` 生成 `resources/images/video_cover.png`
  （深蓝黑渐变 + 主色播放标记 + 上下描边；**RGB 不带 alpha** —— 带 alpha 等于没铺）。
- 显隐（**幂等收敛，不是事件置位**）：
  - IPTV：`tickIptv()` 按相位 —— `SWITCHING/WAITFRAME/ERROR` 铺、`IDLE/PLAYING` 收。
  - 投屏：`tickVideoCover()` 拍级收敛 —— `gCastPainted`（框架播放器 `PLAY_STARTED`）
    **或** `gStreamCasting && H264Player::framesDecoded() > 0`（在线流硬解）即视为已出画。
- ⚠️ **UI 层在视频层之上**：底图忘收就把视频挡死，所以判据必须是"还没出画面"。
- ⚠️ 图形别压住文字：第一版播放标记放在中间 y=330，正好压在三行 loading 文字上。

真机验收（抓 `/dev/fb0` 的 UI 层；视频层抓不到，但底图在 UI 层看得到）：
缓冲期 `(240,190)=(79,195,247)`（三角）+ `(10,100)=(12,17,23)`（渐变）+ 文字白像素 1192；
出画后 480x700 区域**全黑 = UI 层透明**（底图已收）。

> 入库建议（没动 MCP 仓库）：`v85x/videoview-transparent-window.md` 补一条 ——
> "**视频层没数据时该区域透出黑底，黑底上的文字发虚** ⇒ 缓冲期铺一张不透明底图、
> 出画立刻收起；图形位置要避开页面上的提示文字"。

---

## 十九、1080p 支持（1/4 缩放解码）—— 2026-09-14

用户需求：「电台只有 42 个，可以加入一些 1080P 的，采用 1/4 的方式解码」。

### 19.1 分档规则（`PgStream::pickScaleDown()`）

| 源像素 | 档位 | 解码缓冲 | 备注 |
|---|---|---|---|
| ≤ 960x544（52 万） | 不缩放 | 同源 | 原始设计 |
| ≤ 1280x720（92 万） | **1/2** | ≤ 640x360 | 已验多轮 |
| 更大（1080p 及以上） | **1/4** | 1080p → 480x270 | ★ 本次新增 |

**为什么 1080p 用 1/4 而不是 1/2**：1/2 后是 960x540（≈52 万像素）**正好贴着本板解码缓冲上限**
（960x544），参考帧池一点余量没有；1/4 后 480x270（13 万像素）比**已经验过**的 720p@1/2
（640x360，23 万）还小一半。屏幕宽度本来只有 480，1/4 也不浪费清晰度。

> 参考工程（`S:/projects/LearningProject/V851ExtendedScreen_ap_p2p/src/link/context.cpp`）
> **只用过 `SCALE_DOWN_2`、从没用过 `_4`**（它的判据是 `w >= SCREEN_HEIGHT*2`）——
> 所以 1/4 这条路是本次新开的，没有现成先例，只能实测（结论见 19.3）。

### 19.2 内存守卫改判「解码缓冲像素」

```cpp
// 原来：拿**源**像素判 ⇒ 1080p（207 万）直接被拒，连 1/4 都用不上
long long px = (long long)w * h;
// 现在：先按 pickScaleDown 折算，再判；另加源分辨率硬限（硬件最大 1/4，再大没档可用）
const int sd = pickScaleDown(w, h);          // 0 / 2 / 4
const long long px = (long long)w * h / ((long long)div*div);
if (w > kMaxSrcW || h > kMaxSrcH) 拒绝;       // kMaxSrcW/H = 1920x1088
```
`crop` 的折算早就有（`PgH264::applyCrop()` 里 `cropX/scale`），不用动。

### 19.3 ★★★ 前提：必须**固化部署**（本节最重要）

**第一次测 1080p 时，1/4 和 1/2 都让进程静默退出**（日志停在 `decode thread start`、
一条 FATAL 都没有，pid 变了 = 被 init 重新拉起）。当时怀疑过：缩放档不支持？源不兼容？
硬件能力不够？

**根因是内存不够**（用户一句话点破），而内存是被 **`/tmp`（tmpfs，吃 RAM）** 吃掉的：

| | 升级前（`fun launch`） | 升级后（`fun pack` + 刷机） |
|---|---|---|
| 库 | `/tmp/lib/libzkgui.so` **3.46MB** | `/res/lib/libzkgui.so`（squashfs = flash，**不占 RAM**） |
| 字体 | `/tmp/font/pocketgame.ttf` **1.09MB** | `/res/font/pocketgame.ttf`（同上） |
| `/tmp` 占用 | **5 MB** | **0**（只剩 init 建的 `liblylog.so` 符号链接） |
| `MemFree` | 5.9 MB | **16.8 MB** |
| `MemAvailable` | **12.9 MB** | **31.2 MB** |

⇒ 固化后 1080p 1/4 立刻正常出画（§19.4）。

> **通用规则（已进 MEMORY.md）**：
> `fun launch` 是**调试推送**，库/字体/UI 全部落在 `/tmp`（tmpfs），**每个字节都占 RAM**。
> `fun pack` + 刷机是**固化**，落到 `/res`（只读 squashfs，flash，不占 RAM）。
> **凡遇到"内存不够 / 起播就静默退出"，先 `adb shell df /tmp` 看 /tmp 占用**，
> 而不是先怀疑解码参数。

### 19.4 真机验收（固化后，v1.11.0）

| 项 | 实测 |
|---|---|
| 起播（浙江国际 1920x1080@25fps） | `起硬件播放器：源 1920x1080，旋转 90°，缩放解码 1/4` → `解码回调 #1 —— fmt=5 480x288 crop(0,0,480,270)` → **`出画（起播→首帧 6327ms，本轮已解码 11 帧）`** ✅ |
| 画面层（铁证） | `disp ch[0] fb[272,480] crop[0,0,270,480] frame[43,0,393,700]`（1/4 后 480x270 → 转 90° → 等比铺进播放区）✅ |
| **换台** | `换台（不重启）→ 目标 [13] 赤峰新闻综合` → **出画 5240ms**；**pid 719 → 719** ✅ |
| 高码率 1080p（吉林市 4000kbps） | 出画 5551ms、`已喂 300 包 / 解码 209 帧`、连播 30 秒稳定 ✅ |
| 二次起播 | 停止后内存回到 12.3MB → 再播 `出画 5531ms`（内存守卫放行）✅ |
| 列表 | 13 个 1080p 频道在列表里正常显示（`iptv[39] 分组='国际' 名='浙江国际' 实测='1920x1080@25.00fps' '1500kbps'`）✅ |

⚠️ **播放中可用内存会掉到 3.0~15.7MB**（随网络缓冲状态波动），偏紧；但**停止立即释放**回到 12MB+，
所以正常"看一个台"的用法没问题。**不要在 1080p 播放中再叠加别的内存大户**。

### 19.5 频道表：42 → 52 条

新增 13 个实测可连的 1080p 频道（做法与清单见 `docs/iptv-channels.md`）。
