# DLNA 投屏接收端（DMR）—— 交付说明与实测记录

> 设备当"小电视"：手机/PC 上的投屏 App 选中本设备 → 视频在设备上硬解播放。
> 实测日期 2026-09-12，V85X / V851s 板（480x800，56 MB 内存）。

## 一、架构（MVP：下载后播放）

```
手机/PC（DMR 控制器）                    本设备（DMR 渲染端，PgDlna）
────────────────────────                ────────────────────────────────────────
SSDP M-SEARCH (组播 239.255.255.250:1900)
      ───────────────────────────────▶  SSDP 线程：解析 ST → 回 200 + LOCATION
GET http://<设备>:8200/description.xml
      ───────────────────────────────▶  返回 MediaRenderer:1 描述（AVTransport /
                                        RenderingControl / ConnectionManager）
SUBSCRIBE /upnp/event/*                → 回 SID（MVP 不推 NOTIFY）
POST /upnp/control/AVTransport
   SetAVTransportURI(CurrentURI)       → 起下载线程：裸 socket HTTP GET（支持 chunked）
                                         写到落盘目录（优先非 tmpfs，否则 /tmp）
POST .../AVTransport Play              → 下载完成后 play(本地路径)，UI 起投屏页
   GetTransportInfo / GetMediaInfo     → 回报 STOPPED/PLAYING/PAUSED_PLAYBACK + 时长/进度
   Pause / Seek
POST .../RenderingControl SetVolume    → 设备音量（0~100，可回读）
```

**为什么不用框架播放器直接放 URL**：`ZKMediaPlayer` 只认本地路径
（`http://...` → `file[...] is not exist!`，实测），所以必须"先下后播"。
代价与对策见"内存约束"一节。

### 代码位置

| 文件 | 作用 |
|---|---|
| `src/platform/PgDlna.h/.cpp` | DLNA 服务：SSDP 应答、civetweb HTTP/SOAP、下载线程、动作队列 |
| `src/logic/mainLogic.cc` | `tickDlna()` 消费动作队列（UI 线程执行播放器操作）+ QA 命令 |
| `ui/main.html` | `WinCast` 投屏页（videos 视图 videoview 480x700 + 状态字 + 停止按钮） |
| `tools/dlna_ctl.py` | **PC 侧 DLNA 控制器**（发现/投屏/暂停/音量/跳转/状态，等价于手机上的投屏 App） |
| `tools/dlna_devtest.sh` | 设备内自测（不依赖 PC）：描述/SCPD/SOAP 全流程 |

线程模型：**网络线程只投递动作，UI 线程执行播放器操作**（播放器必须在 UI 线程用）。

### QA 命令（免触摸自检）

```bash
echo 'dlna on 8200' > /tmp/pg_autostart      # 启动 DMR
echo 'dlna state'   > /tmp/pg_autostart      # 打印 port/ip/state/uri/下载进度
echo 'dlnauri http://host/x.mp4' > /tmp/pg_autostart   # 等价于 SetAVTransportURI
echo 'dlnaplay' / 'dlnapause' / 'dlnastop' > /tmp/pg_autostart
echo 'cast /tmp/x.mp4' > /tmp/pg_autostart   # 跳过网络，直接投本地文件（调试用）
```

PC 侧控制器：

```bash
python tools/dlna_ctl.py discover                       # SSDP 发现
python tools/dlna_ctl.py cast 192.168.0.125 http://.../a.mp4   # URI + Play
python tools/dlna_ctl.py info 192.168.0.125             # state/duration/position
python tools/dlna_ctl.py pause|resume|stop|seek <host> [值]|vol <host> 0-100
```

## 二、实测验收（v1.0.3，固化版）

| 项 | 结果 |
|---|---|
| SSDP 组播发现（PC 侧） | ✅ `LOCATION=http://192.168.0.125:8200/description.xml` |
| 描述文档 | ✅ `MediaRenderer:1` + 三个服务，控制 URL 齐全 |
| SetAVTransportURI（8.5 MB MP4） | ✅ `HTTP 200, Content-Length=8494526 chunked=0` → 下载完成 |
| 解码 | ✅ `demux → vdec(硬解) → video layer → vo` + `adec → ao → clock`，全通道建成功，无 E/ 日志 |
| 播放 | ✅ `state=PLAYING duration=0:00:08 position=0:00:02/0:00:08`（进度真的在走） |
| 播放中暂停 | ✅ `state=PAUSED_PLAYBACK`（无 `invalid state` 报错） |
| 继续 / 跳转 / 音量 | ✅ 均 HTTP 200；音量回读 `CurrentVolume=40` |
| 播完 | ✅ `PLAY_COMPLETED` → 状态 `STOPPED` → 10 s 后删除媒体文件（释放内存） |
| 进程稳定性 | ✅ 全程 pid 不变（投屏/暂停/跳转/播完），无 FATAL 栈 |

视频画面在 **disp 视频层**（不在 `/dev/fb0`）：播放中
`/sys/class/disp/disp/attr/sys` 会出现 `ch[0] ... frame[0,0,480,700]`（正好是 videoview 的位置），
截图只能看到 UI 层（投屏页黑底 + 底部条 + 停止按钮）。

## 三、内存约束（这条决定 DLNA 能放多大的片子）

- 本板 `/tmp` 是 **tmpfs（27 MB，全在 RAM）**，`/data` 只有 832 KB，无 TF/USB。
- "下载后播放"意味着**整片进内存**：8.5 MB 片子 + 解码缓冲，在 56 MB 机器上很容易压垮
  （实测：内存紧张时播放中进程被干掉 / 无 FATAL 栈地重启）。
- 已实现的护栏（`PgDlna.cpp`）：
  1. 落盘目录**优先非 tmpfs**（有 TF/USB 就用），否则用 /tmp；
  2. **下之前先看 `Content-Length`**：超过可用空间/上限（tmpfs 上限 10 MB）直接拒绝并打日志，
     宁可不放也不把整机搞崩；
  3. **每次下载前清旧文件**、**停播清理**、**播完延迟 10 s 清**（等播放器收尾，实测立即 unlink 偶发崩溃）；
  4. civetweb 线程数压到 3（内存换稳定）。
- 想投大文件（几十 MB 以上）的路线：
  **插 TF 卡**（落盘目标自动优先外置存储）→ 或做流式（`av`/ffmpeg 4.1.9 + aw-mpp 自建管线，
  边下边解，工程量大）。固件里 `ffmpeg 4.1.9 / aw-mpp / civetweb / rtsp·rtp·srt` 包都在。

## 四、踩过的坑（都写进代码注释/技能了）

1. **`strstr(buf, "ST:")` 会命中 `HOST:` 里的 "ST:"** —— M-SEARCH 解析把 ST 读成
   `239.255.255.250:1900`，于是**所有 M-SEARCH 都判为"不受理"，设备在手机/PC 上永远搜不到**
   （日志：`SSDP ST='239.255.255.250:1900' 是否受理=0`）。必须**按行首**匹配 HTTP 头。
2. **框架播放器不支持 http://**：`file[http://...] is not exist!` → 只能先下载到本地再播。
3. **`localIp()` 不能只在启动时算一次**：设备常在开机早期启动 DMR，那一刻 wlan0 还没拿到
   DHCP → 定格成 `127.0.0.1`，控制器拿到的 LOCATION 永远连不上。改成每次用时重算（优先 wlan0）。
4. **播放器没有"是否暂停"查询接口** → 自己镜像状态机（`sCastLoaded/sCastPaused`）：
   控制器的 Play 常常在**下载还没完成**时就到，直接 `resume()` 会报
   `resume called in an invalid state: 0`；播完后若不复位，后续 Pause/Play 也会报同样错。
5. **控制器的 SetVolume 别直接打播放器**：本工程走 `mCasterPtr->setVolume()` 与自家
   常开 ALSA 流同设备，改成映射到设备主音量更稳。
6. **civetweb 客户端 API 的坑**：`mg_get_response` 对 busybox httpd 返回 "No data received" →
   改用**裸 socket 发 GET**（顺便支持 chunked）。
7. **PC 侧网络验收的两个环境问题**（不影响真机投屏，只影响本机测试）：
   - 开发机装了系统代理（`http_proxy`），会把局域网请求劫持成 502 → 工具必须禁用代理；
   - Windows 防火墙挡入站（无管理员权限加不了规则）→ PC 当媒体源时设备拉不到。
     所以本次验收用**设备本地 HTTP 源**（busybox httpd）验证同一条代码路径；
     真实场景源在手机上，设备是**主动外连**方，不受此限。

## 五、下一步

- 插 TF 卡后解除 10 MB 上限，直接投手机里的长视频（落盘目标已自动优先外置存储）。
- 可选：SSDP NOTIFY（`ssdp:alive` 周期通告）+ 事件订阅 NOTIFY 推送（提升部分控制器的兼容性）。
- 可选：流式播放（av/ffmpeg + aw-mpp），彻底摆脱"整片进内存"。

---

## 六、★ 修「手机搜不到」：SSDP 发现的三处真问题（2026-09-13）

用户实测："手机搜不到 WinCast。" 从 PC 同网段做真实抓包/发包定位（设备 192.168.0.125，
PC 192.168.0.110，同一 /24），确认**不是网络、不是防火墙、不是 SSDP 收包有问题**，
而是下面三件事：

### 1. 服务压根没启动（**第一原因**）
`dlnaStart()` 全工程只有**一个**调用点：QA 命令 `dlna on`。界面上没有任何入口，
开机也不启动 ⇒ 手机搜到什么都不会有（实测 `1900/8200` 都没有监听）。
**修**：改成**开机自动启动**（`ensureDlna()`，主循环里驱动）：
延迟 2s 起（别抢首屏）→ 失败每 5s 重试 → WiFi 拿到真实 IP 后**补发一次 alive 通告**
（`Dlna::requestAlive()`：DHCP 晚于 start() 时 LOCATION 会写成 127.0.0.1，控制器连不上）。
`dlna off` 仍可手动关（置 `gDlnaUserOff`，之后不再自动拉起）；`dlna on` 会清掉该标志。
就绪后主界面提示条变成 **"投屏接收端已就绪：手机投屏里选 PocketGame-DMR"**。

### 2. 没有 `ssdp:alive` 通告 ⇒ 手机不搜索就看不到设备
只应答 M-SEARCH 的话，设备**只在对方主动搜索时才现身**；而很多手机（系统投屏、
视频 App 的投屏按钮）是"进页面直接列已发现设备"，依赖设备自己 NOTIFY 上播。
头注释里写了"并做 ssdp:alive 通告"，但代码里**一条都没实现**（`grep ssdp:alive` = 0）。
**修**：启动即发一轮 alive（`upnp:rootdevice` + 设备类型 + 3 个服务类型，共 5 个目标），
之后每 600s 重发；stop 时发 `ssdp:byebye`。PC 侧抓包实测（监听 1900 组播）：

```
NOTIFY NTS=ssdp:alive  NT=upnp:rootdevice                              LOCATION=http://192.168.0.125:8200/description.xml
NOTIFY NTS=ssdp:alive  NT=urn:schemas-upnp-org:device:MediaRenderer:1  LOCATION=...
NOTIFY NTS=ssdp:alive  NT=urn:schemas-upnp-org:service:AVTransport:1   LOCATION=...
（共 5 条；dlna off 时同样收到 5 条 ssdp:byebye）
```

### 3. `ssdp:all` 的应答不合规范
规范要求：收到 `ST: ssdp:all` 要**为每个自己宣告的目标各回一条**；旧代码只回一条
`ST: ssdp:all`（连 USN 都是 `uuid:xxx::ssdp:all`）。严格的控制器会因为 ST 与搜索目标
不匹配而**丢掉这条应答** —— 表现就是"搜不到"。**修**：ssdp:all → 逐目标回 5 条；
其余搜索目标原样回 1 条。PC 实测：`ssdp:all` → **5 条**，单目标 → **1 条**。

### 4. 顺带修掉一个 use-after-free（`dlna off` → `on` 后应答翻倍）
`Dlna::stop()` 只置 `stopFlag_` 就返回，SSDP 线程当时是 **detached**、没人 join；
而 `dlnaStop()` 紧接着 `delete gDlna` ⇒ 线程醒来**读已释放内存**。
实测症状极具特征：**同一个 ST 收到两份应答**，其中一份
`LOCATION: http://192.168.0.125:0/description.xml`（**端口 0** = 读到的垃圾 httpPort_），
控制器拿到这种 LOCATION 必然连不上。
**修**：① SSDP 线程改为**可 join**（不 detach），`stop()` 里 join（最多等 400ms = recv 超时）；
② `dlnaStop()` **不再 delete 实例**（下载线程仍是 detached、可能还在拉大文件），
实例长期复用，`on` 时对同一对象重新 `start()`。

### 自查清单（换网络/换手机时照这个顺序查）
1. `grep -c 076C /proc/net/udp`（1900）与 `grep -c 2008 /proc/net/tcp`（8200）都要是 1；
2. PC 上 `curl http://<设备IP>:8200/description.xml` 应返回 200 + XML；
3. PC 上监听 1900 组播，应看到设备发 `ssdp:alive`；
4. PC 上发 M-SEARCH（`ssdp:all` 应回 5 条、`MediaRenderer:1` 应回 1 条），
   且 LOCATION 里**端口必须是 8200、IP 必须是设备真实 IP**；
5. 手机与设备**必须同一网段**（本板 wlan0 是 192.168.0.125）；部分路由器的
   "AP 隔离/客户端隔离"或访客网络会挡掉组播 —— 那属于网络配置问题，不是设备问题。

---

## 七、★ 「投了没反应、屏幕没画面」：下载链路的 4 个 bug（2026-09-13）

用户实测："手机能搜到了，但页面没有跳转，屏幕看不到视频。"
抓设备的 SOAP 日志，看到手机确实发了 `SetAVTransportURI` + `Play`，但下载失败：

```
PgDlna: SetAVTransportURI 'http://upos-sz-estghw.bilivideo.com/...mp4?e=...&amp;ua=tvproj&amp;oi=...
PgDlna: HTTP 959
PgDlna: 下载失败 r=-7（已收 0 字节）
```

下载失败 → `startCast()` 根本没被调用 → **投屏页不跳转、自然也没画面**。根因四个：

### 1. ★ XML 实体没反转义（本次的直接原因）
SOAP 体里的 URI 把 `&` 转义成 `&amp;`，我们**原样拿去下载** → 查询串变成
`...&amp;ua=tvproj...`，CDN 判定签名非法（B 站回 **959**）。
**修**：`xmlTag()` 统一做 XML 反转义（`&lt; &gt; &quot; &apos; &#39;`，**最后**才 `&amp;`，避免二次解码）。
修后同一条链接的日志变成 `...&ua=tvproj&oi=...`（正确），B 站改回 **403**（见第 5 节）。

### 2. URI 缓冲区太小（512 字节，正好把这条链接截断）
用户那条链接原文 **511 字符**（含 `&amp;`）/ 467（反转义后），而当时：
`cur[512]`、`lastUri_[512]`、下载线程里的 `uri[512]` —— 全在临界值上，**再长一点就截断**。
**修**：`cur[2048]` / `lastUri_[2048]` / 线程 `uri[2048]` / 请求行 `req[3072]` / SOAP body `[8192]`。

### 3. 不跟 3xx 重定向
`code != 200` 一律当失败。实测 `http://media.w3.org/.../trailer.mp4` 回 **301** → 直接失败。
**修**：3xx 且有 `Location` 时返回特殊码 `-10`，调用方**最多跟 3 跳**（支持绝对 URL 与 `/相对路径`）。
⚠️ 若 Location 指向 **https**，我们只支持 http → 明确打日志 `不支持的 Location 'https://...'` 并失败
（**https 仍是未支持的**，见第八节）。

### 4. 带查询串时扩展名取错
```c
const char *dot = strrchr(path, '.');           // 最后一个点
if (dot && (!q || dot < q) && strlen(dot) < 8)  // ← strlen(dot) 把 ?a=1&b=2 也算进去了
```
`click.wav?a=1&b=2` 的 `strlen(dot)` = 13 ≥ 8 → 条件不成立 → 一律退回 `.mp4`。
**修**：扩展名取 `dot` 到 `?`（或串尾）之间的子串，再限长 8。

### 5. 验证与遗留限制（**要如实知道**）
- **本地投屏路径正常**（用 QA `cast /tmp/test_media.wav` 跳过下载）：
  `PocketGame cast: play '/tmp/test_media.wav'` → `demux/adec channel success` →
  `PLAY_STARTED 开始出画面`，截图确认投屏页已显示（视频区在黑底之外，底部停止按钮 y≈720..760）。
  ⇒ **"页面不跳转"纯粹是下载失败导致**，播放/页面逻辑本身是好的。
- **B 站那条链接谁都拉不到**：PC 上用 curl 直连（换 UA / 加 Referer / Origin 全试过）同样
  **403 或 959** ⇒ 该 CDN 链接**与请求方（手机）绑定**，属服务端防盗链，**不是本机代码能解决的**。
  这类 App 的投屏要能放，得走"手机代理转推"或"设备侧自己做鉴权"（如 TV 端 bilisync），本工程不做。
- **https 源不支持**：大量 App 下发的是 https 链接，会直接 `不支持的 Location 'https://...'`。
- **10MB 上限**：本板 `/data` 只剩 588KB、无外置存储，落盘只能落到 `/tmp`（tmpfs=内存），
  所以 `PG_DLNA_TMPFS_MAX_BYTES` 卡在 10MB（23MB 的测试片会 `r=-9` 被拒）。

---

## 八、★ 投屏改为**在线流直连**（不再"下载到 /tmp 再播"）—— 2026-09-13

### 为什么必须改
`/tmp` 在本板是 **tmpfs（就是内存）**：旧实现把整片下到 `/tmp/dlna_media.mp4` 再播，
片子一大必然 OOM。实测一部 3.9Mbps 的片子下到一半就把 `MemFree` 从 ~20MB 吃到 **7MB**；
更要命的是**下载中途断流也会被当成"下载成功"**（远端用 chunked、没有 `Content-Length` 时，
读循环把"对端关闭"当正常结束）→ 去播一个不完整的文件（实测下载到 89.5% 就播了）。

### 现在怎么走
`SetAVTransportURI` 拿到 URI 后**按源分流**（`PgDlna::onSetUri`）：

| 源 | 走哪条路 |
|---|---|
| `http://` / `https://` | **在线流直连**：不下载、不落盘，直接把 URL 投给 UI 线程 → `PgStream`（ffmpeg 解封装 + 设备硬解 + VO 上屏）|
| 本地文件（`file://` 或 `/路径`） | 原来的框架播放器路径（ZKVideoView）|

UI 线程（`tickDlna`）按 `gStreamCasting` 分流处理各动作：

| 控制器动作 | 在线流的处理 |
|---|---|
| `Play` | 启动 PgStream（`startWithDisplay`，矩形 (0,0,480,700)）+ 亮出投屏页当透明窗口；状态 `PLAYING` |
| `Pause` | **如实降级**：在线流暂不支持暂停 → 停掉画面，状态回报 `PAUSED_PLAYBACK` |
| `Play`（暂停后） | 从头重开（`PgStream::restart()`；流式播放没有断点续播） |
| `Stop` | `PgStream::stop()` + 收页 + 状态 `STOPPED` |
| `Seek` | 暂不支持（记警告日志，不改状态） |
| `GetPositionInfo` | 用 `PgStream` 的时长/进度回报（实测 `TrackDuration=0:00:46`、`RelTime` 随播放递增） |

⚠️ 顺带修掉一处隐患：`DlnaAction::path` 原本只有 **512 字节** —— 而 CDN 链接带签名参数
轻松 500+ 字符（实测 B 站那条 **511 字符**），截断就会 403/959 或直接连不上。已加大到 **2048**。

### 真机验收（v1.7.4 固化版，PC 侧 `tools/dlna_ctl.py` 当控制器）
```
SetAVTransportURI 'https://vjs.zencdn.net/v/oceans.mp4' → HTTP 200
PgDlna: 在线流直连（不下载、不落盘），交给 PgStream 播
PocketGame dlna: 播放 https://vjs.zencdn.net/v/oceans.mp4（在线流直连）
PgStream: 打开成功 用时 473ms 视频[0] codec=h264 960x400 ；总时长 46613 ms
VO_Enable/EnableVideoLayer/SetVideoLayerAttr/OpenVideoLayer/CreateChn/StartChn 全 0x0
GetPositionInfo → TrackDuration=0:00:46  RelTime=0:00:09（递增）
/tmp/dlna_media* 不存在（**没有落盘**）
内存：播放 10s/20s 均为 9116 kB、停后 9336；第二轮 9116 → 9324（Shmem 恒 1200）⇒ **无泄漏**
stop → HTTP 200 → 状态 STOPPED + VO_StopChn/DestroyChn 0x0 + 自动收投屏页
```
⚠️ 站点兼容性：个别 https 站点握手被**服务端**拒绝（OpenSSL alert `1040` handshake_failure，
如 `download.samplelib.com`），与本机实现无关；`vjs.zencdn.net` / `raw.githubusercontent.com` /
`example.com` 等均正常。
