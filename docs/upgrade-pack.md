# 固化升级：出 update.img 并刷进设备（v1.8.4）

> 本文记录 2026-09-14 把 PocketGame 整包**固化**进真机的完整过程与验收证据。
> 判据（铁律）：**掉电后还要在 → 固化（`flythings_pack_upgrade`）；只是看效果 → 调试
> （`flythings_build_ui_flow` / `fun launch`）**。

## 一、出包

```bash
# MCP 正规入口（等价命令）：
flythings_pack_upgrade(project_root, out_path, release_version="1.8.4", with_build=True)
#   → fun install && fun build && fun pack -p V85X --release-version 1.8.4 -o <out_path>
```

**产物**：

| 文件 | 大小 | MD5 |
|---|---|---|
| `out/update_v1.8.4.img`（带版本号，交付用） | 2 171 452 B (2.07 MB) | `abb918dab928ae3895b8d0cae2698f17` |
| `out/update.img`（同内容，IDE/脚本约定的默认名） | 同上 | 同上 |
| `.fun/v85x/update.img`（fun 的真实输出位置） | 同上 | 同上 |

包内 83 个文件，结构（= 设备的 `/res`）：

```
bin/firmware/rtlbt/{rtl8733bs_fw,rtl8733bs_config}   蓝牙固件
etc/EasyUI.cfg                                       分辨率/触摸设备/屏保超时/字库路径
font/pocketgame.ttf                                   项目字库子集（208 KB）
lib/libzkgui.so                                       主程序（3.27 MB）
lib/libawh264player.so   ★新增                        zk_h264_player 实现（21 KB）
lib/{libcrypto.so.1.1,libssl.so.1.1}                  https
ui/{main,ime,wifi,screensaver,remote}.ftu             5 张布局
ui/audio/*.wav                                        11 个音效
ui/certs/cacert.pem                                   https 根证书
ui/images/*.png                                       图标 / 翻页钟卡片
ui/iptv/channels.m3u   ★新增                          20 个已筛频道
```

### ★ `lib-no-link` 会被打进 `/res/lib`（本次关键收获）

开发期我们一直手动把 `libawh264player.so` push 到 `/data`（因为
`fun launch` **不推送** `src/dependencies/lib-no-link/` 下的库）。
**固化包会把它放进 `/res/lib`**，而设备 `LD_LIBRARY_PATH` 本来就含 `/res/lib`：

```
export LD_LIBRARY_PATH /data:/tmp:/res/lib:/res/zkswe:/lib:/lib/eyesee-mpp
```

⇒ 固化后**不需要任何手动 push**，`zk_h264_player` 直接可用（实测见下）。
本次也顺手删掉了 `/data/libawh264player.so`（它在 ld 路径更前面，会遮蔽固化版，
将来升级库时会新旧不一致 —— **别留这种遮蔽**）。

## 二、刷进设备（ADB 固化）

```bash
adb push out/update_v1.8.4.img /tmp/update.img
adb shell setprop sys.zkupgrade.flag 255      # 255 = 强制升级
adb shell setprop sys.zkupgrade.dir /tmp      # 升级包所在目录
adb shell setprop ctl.restart zkswe           # 触发
```

> ⚠️ `/data` 只有 **832 KB**（jffs2，要留给存档），装不下 2 MB 的包 ——
> 所以只能放 `/tmp`（tmpfs，27 MB）。升级程序用完会自己删掉 `/tmp/update.img`。
>
> 其余刷法（TF 卡 / 插卡自动升级 / 远程 OTA）见 MCP
> `knowledge/devflow/upgrade-pack-image.md`。

## 三、验收证据（真机 20080411）

| 项 | 结果 |
|---|---|
| `/res/lib` 时间戳 | 全部更新为固化时刻（`2026-09-14 02:02`） |
| `libzkgui.so` 体积 | 2 874 120 → **3 273 300** B（新版含 IPTV/H264/时钟套件等） |
| `libawh264player.so` | **出现在 `/res/lib`**（21 624 B） |
| 运行镜像 | `/proc/719/maps` 显示 **`/res/lib/libzkgui.so`**（不是 `/tmp/lib`） |
| 掉电保留 | `reboot` 整机重启 → 程序自动拉起、`/res/lib` 内容不变 |
| 调试依赖 | 重启后 `/tmp` 只剩 `liblylog.so`（init 建），**零依赖** |
| 用户数据 | `/data/pocketgame.dat`（存档）、`pocketgame_alarms.txt`（闹钟 1 条）**完好** |
| 启动日志 | 193 行、**异常 0 行**；DHCP → NTP 校时 → 30s 进屏保 |
| IPTV 功能 | 从 `/res/ui/iptv/channels.m3u` 载入 20 频道；播放 → `PgHls 清单 OK 360x240` → 解码帧 #1→#120 |
| H264 硬解 | 删掉 `/data` 手动库后仍 `zk_h264_player_init(...) -> 0`、解码回调 #1/#60 —— **固化库可用** |

**行为确认（重点排查过的一项）**：重启/播放中直接断电重启，**不会自动续播 IPTV** ——
程序干净启动进屏保。`/tmp/pg_iptv_resume` 只在"换台/停止"瞬间写入且是 tmpfs，
整机重启即失效，符合设计。

## 四、两个坑（都花过时间）

1. **`flythings_pack_upgrade` 未传 `out_path` 时，返回的 `updateImg`/`modified` 指向旧的
   `out/update.img`**（本次第一次调用报的是 9/13 的旧包），而 `fun` 的真实产物在
   `.fun/v85x/update.img`。**判断"包是不是新的"要看 `.fun/<平台>/update.img` 的
   mtime**，别只信返回值。传了 `out_path` 后回报正常。
2. **`update.img` 每次打包字节都不同**（连续两次同参数打包差 32 万字节）——
   squashfs 重建时带文件 mtime，属正常现象。⇒ **不能用 MD5 判断"代码有没有变"**，
   要比代码就比 `libzkgui.so` 或直接看功能。

## 五、当前设备状态（固化完成）

- 固件：PocketGame **v1.8.9**（21 个应用 = 13 游戏 + 7 工具 + 1 系统页）—— 见 §七
- 运行：`/bin/zkgui` + `/res/lib/libzkgui.so`，可用内存 ~21 MB
- 数据：`/data` 存档 / 闹钟 / `preferences.json` 保留；自定义频道表可放 `/data/iptv.m3u`（优先级高于内置）

## 六、v1.8.5（2026-09-14 12:07）

**唯一改动**：`src/Main.cpp` 的 `onEasyUIInit()` 开头加 `pg::VideoLayer::release()`
（程序启动即清掉上一轮残留的 disp 视频层，见 §disp-layer.md §2.3）+ 版本号 1.8.5。

出包 / 刷机与 §一、§二 完全一致：

```bash
./fun.exe build
./fun.exe pack -p V85X --release-version 1.8.5 -o out/update_v1.8.5.img
adb push out/update_v1.8.5.img /tmp/update.img
adb shell "setprop sys.zkupgrade.flag 255; setprop sys.zkupgrade.dir /tmp; setprop ctl.restart zkswe"
```

验收（真机 20080411）：

| 项 | 结果 |
|---|---|
| 包 | `out/update_v1.8.5.img` 2 183 740 B（12:07:37 产出） |
| `/res/lib/libzkgui.so` | 3 287 076 B、时间戳 04:07（=固化时刻） |
| 一致性 | 设备 `/res/lib/libzkgui.so` 与本地 `.fun/v85x/libzkgui.so` **MD5 完全相同** `1f530056…` |
| 运行镜像 | `/proc/719/maps` → `/res/lib/libzkgui.so`（非 /tmp/lib） |
| 新功能 | 启动日志 `PocketGame: 启动图层释放 -> 清掉 0 个残留视频层` |
| 掉电保留 | `reboot` 后 `/proc/uptime`=58s、`/res` 时间戳不变、功能仍在 |
| 用户数据 | `/data/pocketgame.dat`、`pocketgame_alarms.txt`、`preferences.json` 完好 |
| 清理 | `/tmp/update.img` 升级后自动删除（`/tmp` 只剩 busybox） |


## 七、v1.8.9（2026-09-14 13:31）—— 含 v1.8.6~1.8.8 的全部改动

> v1.8.6~1.8.8 当时只记在 `docs/` 各专题与工作日志里，这一节一次性收拢。

**本版累计改动**

| # | 改动 | 文档 |
|---|---|---|
| 1 | `Main.cpp` 启动即释放残留 disp 视频层（异常重启后屏幕不再冻在上一轮画面） | `disp-layer.md` §2.3 |
| 2 | 进视频前 `drop_caches` 清页缓存（实测可用内存 +16.4MB） | `online-media.md` |
| 3 | **`ZKMEDIA_H264_VBVSIZE`=1MB** —— 修掉"720p + 旋转起播静默退出"（根因：环境变量一个没设） | `h264-direct.md` |
| 4 | 旋转改为**起播后** `set_rot` 并重发 crop；QA `vbv <字节>` 可调 + 落盘 | `h264-direct.md` |
| 5 | IPTV 频道表：PC 侧逐条实测（H264/≤720p/≤30fps）+ 设备端抽查 ⇒ **47 条** | `iptv-channels.md` |
| 6 | **主菜单图标 bug**：行视图跨行复用，缓存键不能用 `index` ⇒ 改为"按控件当前可见性判脏" | `README.md` §七.7 |
| 7 | **工作界面禁屏保**：`MODE_GAME/MODE_TOOL/投屏页` 里 `setScreensaverEnable(false)` | `screensaver.md` §1 |

**出包 / 刷机 / 验收**

```bash
./fun.exe build
./fun.exe pack -p V85X --release-version 1.8.9 -o out/update_v1.8.9.img
adb push out/update_v1.8.9.img /tmp/update.img
adb shell "setprop sys.zkupgrade.flag 255; setprop sys.zkupgrade.dir /tmp; setprop ctl.restart zkswe"
```

| 项 | 结果 |
|---|---|
| 包 | `out/update_v1.8.9.img` 2 183 740 B |
| 一致性 | 设备 `/res/lib/libzkgui.so` 3 291 844 B 与本地 `.fun/v85x/libzkgui.so` **MD5 相同**（`24f8aacf…`） |
| 运行镜像 | `/proc/719/maps` → `/res/lib/libzkgui.so`（**注意本板 pid 常驻 719，别用 pid 判断有没有重启，看 `/proc/uptime`**） |
| 掉电保留 | `reboot` 后 `uptime`=53.9s、`/res` 时间戳仍是 `05:31`、启动日志有"启动图层释放" |
| **图标 bug（真触摸回归）** | 切分类来回：两类各自 **5 行图标互不相同**、`游戏↔工具` 两两不同、切回游戏**逐行字节一致** |
| **屏保 bug（真触摸回归）** | 点卡片进游戏 → `进入工作界面（mode=1）-> 关闭屏保`；停 **40 秒**（超时 30s）`performScreensaverOn` 计数 **0** |
| 用户数据 | `/data` 存档 / 闹钟 / `preferences.json` 完好 |

> ⚠️ 本版之后**触摸注入已打通**（`tools/pginj.c`），上面两条回归就是用它做的真触摸验收，
> 做法与三条铁律见 `docs/touch-inject.md`。

## 八、v1.9.0（2026-09-14 13:59）—— 补齐"时钟套件/网络电视"图标

**改动**：应用上限从 19 提到 **21**（三处一起改，见 `README.md` §七.11）

| 位置 | 改动 |
|---|---|
| `ui/main.html` | 新增 `Icon20`（第 21 个同位置重叠的图标控件） |
| `tools/gen_icons.py` | 补 slot 20（网络电视）→ 重跑生成 `app_icon_0..20.png`（21 张，自检通过） |
| `src/logic/mainLogic.cc` | 新增 `kIconCount = 21`（循环只认这一处）+ 补 `onButtonClick_Icon19/20` 回调桩 |

```bash
./fun.exe build
./fun.exe pack -p V85X --release-version 1.9.0 -o out/update_v1.9.0.img
adb push out/update_v1.9.0.img /tmp/update.img
adb shell "setprop sys.zkupgrade.flag 255; setprop sys.zkupgrade.dir /tmp; setprop ctl.restart zkswe"
```

**验收（真触摸 + 逐像素比对，设备 = 当前接入的这台）**

| 项 | 结果 |
|---|---|
| 一致性 | `/res/lib/libzkgui.so` 含 `PocketGame v1.9.0`，与本地 `.fun/v85x/libzkgui.so` **MD5 相同**（`2b73bdee…`） |
| 滚到底后的工具分类 | 秒表/计算器/蓝牙遥控/**时钟套件**/**网络电视** 五行图标与磁盘 PNG 比对差值 **0.20/0.31/0.21/0.28/0.31**（次选 24~34） |
| 游戏分类回归 | 行0..4 = 2048/方块/飞机/小鸟/蛇（差值 0.16~0.31），没被改坏 |
| 掉电保留 | `reboot` 后 uptime=54s，重做同一比对**结论一致** |
| IPTV 频道表 | `/res/ui/iptv/channels.m3u` = 47 条 |
| 触摸 | **本机面板已换成 `gt9xx`(MT-A, `event0`)**；框架自动选节点（`/proc/<pid>/fd` → event0），注入与点击均正常 |

> ⚠️ 本机是"换过面板"的机器：注入脚本/文档里**别再写死 `/dev/input/event4`**
> （那是老面板 `axs_ts`/MT-B）。详见 `docs/touch-inject.md` §1。

## 九、v1.11.0（2026-09-14 20:30）—— 1080p 频道（1/4 缩放解码）

**改动**

| # | 改动 | 文档 |
|---|---|---|
| 1 | **支持 1080p 源**：新增 `PgStream::pickScaleDown()` 按源像素分档（≤960x544 不缩放 / ≤720p 走 1/2 / 更大走 **1/4**）；两处起播决策点统一走它 | `online-media.md` §19 |
| 2 | **内存守卫改判「解码缓冲像素」**：原来拿源像素判 ⇒ 1080p（207 万）直接被拒；现按缩放倍率折算后再判，另加源分辨率硬限 `1920x1088` | 同上 |
| 3 | IPTV 频道表 **42 → 52 条**（新增 13 个实测可连的 1080p 中文频道；其中 3 条同名把旧的 720x576 版顶掉） | `iptv-channels.md` |

```bash
./fun.exe build
./fun.exe pack -p V85X --release-version 1.11.0 -o out/update_v1.11.0.img
adb push out/update_v1.11.0.img /tmp/update.img
adb shell "setprop sys.zkupgrade.flag 255; setprop sys.zkupgrade.dir /tmp; setprop ctl.restart zkswe"
```

### ★★★ 本版最重要的发现：**固化能省 4.55MB RAM**（这就是 1080p 第一次测崩的原因）

第一次测 1080p 时，**1/4 和 1/2 都让进程静默退出**（日志停在 `decode thread start`、
零 FATAL、pid 变了 = init 重新拉起）。当时排查方向全错（怀疑缩放档、怀疑源、怀疑硬件），
**真因是内存不够**，而内存被 `/tmp` 吃掉了：

| | 固化前（`fun launch`） | 固化后（`fun pack` + 刷机） |
|---|---|---|
| 库位置 / 大小 | `/tmp/lib/libzkgui.so` **3.46MB** | `/res/lib/libzkgui.so`（flash，**不占 RAM**） |
| 字体位置 / 大小 | `/tmp/font/pocketgame.ttf` **1.09MB** | `/res/font/pocketgame.ttf`（同上） |
| `/tmp` 占用 | **5 MB**（`df /tmp` → Used 5M） | **0**（只剩 init 建的 `liblylog.so` 符号链接） |
| `MemFree` | 5948 kB | **16772 kB** |
| `MemAvailable` | **12880 kB** | **31120 kB** |
| 运行镜像（`/proc/719/maps`） | `/tmp/lib/libzkgui.so` | **`/res/lib/libzkgui.so`** ✅ |

⇒ **`fun launch` 是调试推送**（库/字体/UI 全落 `/tmp`，tmpfs 每字节都占 RAM）；
**`fun pack` + 刷机是固化**（落 `/res`，只读 squashfs，不占 RAM）。
**"内存不够/起播静默退出"的第一诊断动作应该是 `adb shell df /tmp`。**

### 验收（真机，固化后）

| 项 | 结果 |
|---|---|
| 包 | `out/update_v1.11.0.img` 2 695 740 B |
| `/res/lib/libzkgui.so` | 3 460 564 B（时间戳 = 固化时刻） |
| `/tmp` | **零依赖**（只有 `liblylog.so`），`df /tmp` Used **0** |
| 运行镜像 | `/proc/719/maps` → `/res/lib/libzkgui.so` |
| 1080p 起播 | `起硬件播放器：源 1920x1080，旋转 90°，缩放解码 1/4` → `解码回调 #1 fmt=5 480x288 crop(0,0,480,270)` → **`出画 6327ms`** |
| 画面层 | `disp ch[0] fb[272,480] crop[0,0,270,480] frame[43,0,393,700]` |
| 换台 | `换台（不重启）→ 出画 5240ms`；**pid 719 不变** |
| 高码率 1080p（4000kbps） | 出画 5551ms、连播 30 秒稳定 |
| 频道表 | `/data/iptv.m3u`（我推的测试表）读入 **52 条**；13 个 1080p 频道列表显示正常 |
| 重启后 | `uptime` 从 0 起算（系统重启过）；pid 719 常驻（**别用 pid 判断有没有重启，看 `/proc/uptime`**） |

> ✅ **频道表已随本包固化**：`fun pack` 是在频道表更新（52 条）之后打的，所以
> `/res/ui/iptv/channels.m3u` **就是 52 条**（实测拉回来数过：52 条 / 13 个 1080p，
> 与本地 `resources/iptv/channels.m3u` 一致）。启动日志：
> `频道表已加载 52 个（内置表，/res/ui/iptv/channels.m3u）`。
> 验收期间临时 `adb push` 到 `/data/iptv.m3u` 的那份**已删掉** —— 让它读固化表，
> 否则 `/data` 那份会一直遮蔽 `/res`，将来更新固件时看不出效果。

> ⚠️ 播放中可用内存会掉到 **3.0~15.7MB**（随网络缓冲波动），偏紧但停止即释放。
> 1080p 播放时不要再叠加其它内存大户。

## 十、v1.12.2（2026-09-14 21:20）—— 放弃 `DISP_UNCACHE`（回退到纯 STREAM_EOF | 缩放档）

**这个版本是"减法"**：把上一版试着加的 `E_H264_PLAYER_FLAG_DISP_UNCACHE` 彻底去掉。

| # | 改动 | 说明 |
|---|---|---|
| 1 | `PgH264::h264Flags()` **不再 OR `DISP_UNCACHE`** | 也不留 `kUseDispUncache` 开关（免得以后顺手打开） |
| 2 | 删掉"UNCACHE 时 init 直接带 rot"那条分支 | 恢复单一路径：`init(rot=0) → set_pos → show → set_rot → applyCrop` |
| 3 | 文件头写清**机制与实测数据** | UNCACHE 绕开 DISP ⇒ 没有旋转（详见下表） |

**为什么放弃（用户点明的机制）**：UNCACHE = **VDEC 不经 DISP 直出到显示器**（绕开 DISP 的
缓冲/合成/拷贝）；而**旋转正是 DISP 在做的事** ⇒ 不走 DISP 就没有旋转。

| 配置（同板同源，720p 走 1/2，固化） | MemAvailable | VmRSS | RssAnon | 画面 |
|---|---|---|---|---|
| 无 UNCACHE（`flag=0x11`） | 8.6MB | 19064 | 13280 | ✅ 铺满 `frame[43,0,393,700]` |
| UNCACHE（`rot` 后下发 / `rot` 传 init 都试过） | **14.4MB** | 14852 | 8128 | ❌ 只填 y≈417、下方 40% 全黑 |

内存收益是真的（+5.8MB），但本板 480x800 **竖屏**、横屏源必须转 90°，**没得换**。

```bash
./fun.exe build
./fun.exe pack -p V85X --release-version 1.12.2 -o out/update_v1.12.2.img
adb push out/update_v1.12.2.img /tmp/update.img
adb shell "setprop sys.zkupgrade.flag 255; setprop sys.zkupgrade.dir /tmp; setprop ctl.restart zkswe"
```

### 验收（真机，固化后）

| 项 | 结果 |
|---|---|
| 包 | `out/update_v1.12.2.img` 2 695 740 B |
| 固化 | `/proc/<pid>/maps` → `/res/lib/libzkgui.so`；`df /tmp` → **Used 0K** ✅ |
| 720p（1/2） | `flag=0x11` → `fb[360,640] crop[0,0,360,640] frame[43,0,393,700]`（缓冲已旋转、自洽）、出画 7443ms ✅ |
| 1080p（1/4） | init `flag=0x21` → `fb[272,480] crop[0,0,270,480]`、出画 6890ms、pid 719 不变 ✅ |
| 画面填充 | 1080p 合成帧实测 **y=0..699**（UNCACHE 坏时只到 417）✅ |
| 换台/停止 | `stopForSwitch` 不重启（pid 不变）✅ |
