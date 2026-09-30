# 音频输出：三条路径与"官方播放器"正解

> 起因：闹钟响铃要"走音频播放器"（用户 2026-09-13 指示），并要求**先看参考工程**
> `S:/projects/LearningProject/V851ExtendedScreen_ap_p2p` 是怎么出声的。
> 这份文档是那次调查的结论 —— **全部结论都有实测判据，不是读注释猜的**。

## 0. 一句话结论

**参考工程的"音频播放器" = `zk_audio_player`（实现在它自带的 `src/dependencies/lib/libzkmedia.a`），
本板实测它走 `card0`（片内 codec = 有喇叭的那张卡），可用；而 EasyUI 框架的 `ZKMediaPlayer`
走 `card1`（ES714X I2S DAC，本板没接喇叭），是静音的。**

## 1. ★ 判据：看进程打开的 `/dev/snd/pcmC?D?p`

不需要耳朵，也不需要读图 —— **播放期间看 `/proc/<pid>/fd`**：

```sh
ADB push <busybox> /tmp/busybox && chmod 755
# 播放中：
$ADB shell "/tmp/busybox ls -l /proc/<zkgui pid>/fd | /tmp/busybox grep pcmC"
```

本板两块声卡（`docs/hardware-reference.md`）：

| 节点 | 对应卡 | 实际硬件 |
|---|---|---|
| `pcmC0D0p` | card0 | **片内 audiocodec = 板载喇叭** ✅ |
| `pcmC1D0p` | card1 | ES714X I2S DAC，**没接喇叭** ❌ |

### 实测对比（2026-09-13，同一进程、同一块板）

| 播放器 | 打开的设备 | 结论 |
|---|---|---|
| EasyUI `ZKMediaPlayer`（`ui/media/ZKMediaPlayer.h`） | `pcmC1D0p` | ❌ 静音（声音进了没喇叭的卡） |
| **参考工程官方播放器 `zk_wav_play` → `zk_audio_player`** | **`pcmC0D0p`** | ✅ 走喇叭 |
| 本工程 `PgAudio`（裸 ALSA `hw:0,0`） | `pcmC0D0p` | ✅ 走喇叭（一直靠它出声） |

配套日志证据（官方播放器）：

```
PocketGame: zk_wav_play('/tmp/ui/audio/_probe.wav') 后台线程启动
[播放期间 /proc/6369/fd]  25 -> /dev/snd/pcmC0D0p
PocketGame: zk_wav_play('/tmp/ui/audio/_probe.wav') 返回 0（0=正常）
[播放结束后] fd 里已无 pcmC 节点 → 设备正确释放
```

> ⚠️ 测之前必须让出 `card0`（QA `pcmfree`）：`PgAudio` 的常开流独占它，
> 别人再去打开会**挂住**（不是报错）。

## 2. 为什么框架的 `ZKMediaPlayer` 是静音的

它不是按 ALSA 名字解析的设备，而是走 **eyesee-mpp AO**，AO 自己拼设备名（拼成 card1），
**不读 `/etc/asound.conf`**。所以：

- 改 `asound.conf` / `ALSA_CONFIG_PATH` 都没用（这条路我们早期验证过，节点始终是 `pcmC1D0p`）；
- `EasyUIContext`/`ZKMediaPlayer` 也没有"选声卡"的接口。

⇒ **想用框架播放器在本板出声，没有官方开关**（AO 的 `AW_MPI_AO_SetPcmCardType` 需要拿到
它内部的 AO 句柄，`ZKMediaPlayer` 不暴露）。

## 3. ★ 正解：参考工程自带的 `zk_audio_player`

### 3.1 它在哪

| 东西 | 位置 | 说明 |
|---|---|---|
| 头文件 | `src/media/audio_player.h` | API 声明（`extern "C"`） |
| **实现** | `src/dependencies/lib/libzkmedia.a` （45 KB） | ⚠️ **不在设备 rootfs** —— `/lib/libzkmedia.so` 里没有 `zk_audio_player_*` 符号（那里面是 `media::_Register_AUDIO_MppPlayer`，是媒体框架，同名但不同物） |
| wav 播放范例 | `src/media/wav_utils.c` | `zk_wav_play()`：解析 wav 头 → init → 循环 put_frame → deinit |
| 循环播放范例 | `src/test/loop_player.cpp` | 播完在消息回调里再 `play()` 一次（**注意：它用的是框架 ZKMediaPlayer，本板静音，只能当写法参考**） |
| 初始化 | `src/media/audio_context.cpp::init()` | 见 §4 |

`libzkmedia.a` 内含 5 个目标文件（`nm` 实测）：

```
audio_player.o    ← zk_audio_player_* / zk_audio_multi_player_* / zk_audio_record_*
pcm.o             ← 自带一份 **tinyalsa**（/dev/snd/pcmC%uD%u%c）
mixer.o           ← 自带 tinyalsa 的 mixer（按名字找控件）
h264_player.o     ← 视频（本工程不用）
audio_record.o    ← 录音（本板无麦克风，不用）
```

### 3.2 API（`src/media/audio_player.h`）

```c
// 独占模式：同时只能有一个播放器输出声音
int  zk_audio_player_init(uint32_t channels, uint32_t rate,
                          uint32_t period_size, uint32_t period_count);
void zk_audio_player_deinit();
int  zk_audio_player_put_frame(uint8_t *data, uint32_t size);   // 推 PCM 帧
void zk_audio_player_set_gain(float gain);
void zk_audio_player_set_volume(float vol);      // 0.0 ~ 1.0
void zk_audio_player_set_max_volume(int max_vol);
void zk_audio_player_set_mode(audio_player_mode_e mode);        // SPK/HP/FM/BT/AUX
void zk_audio_player_mixer_set_value(int card, const char *key, int val);

// 多实例版本
void *zk_audio_multi_player_init(uint32_t ch, uint32_t rate, uint32_t ps, uint32_t pc);
int   zk_audio_multi_player_put_frame(void *player, uint8_t *data, uint32_t size);
void  zk_audio_multi_player_deinit(void *player);
```

`audio_player_mode_e`：`E_AUDIO_PLAYER_MODE_SPK`（扬声器）/ `HP`（耳机）/ `FM` / `BT` / `AUX`。

### 3.3 最简用法（照 `wav_utils.c`）

```c
zk_audio_player_init(channels, sample_rate, 0, 0);   // 0,0 = 用默认 period
while ((n = fread(buf, 1, 4096, fp)) > 0)
    zk_audio_player_put_frame(buf, n);
zk_audio_player_deinit();
```

### 3.4 接入本工程（已做，可复用）

1. 复制三个文件（**和 ffmpeg 一样走"本地库"约定**）：
   - `libzkmedia.a` → `src/dependencies/lib/`
   - `audio_player.h`、`wav_utils.{h,c}` → `src/media/`
2. **不需要改 CMakeLists** —— `fun` 会自动扫描 `src/dependencies/lib/` 并加入链接
   （验证：`grep -c libzkmedia .fun/v85x/CMakeLists.txt` = 1，编译无 `undefined reference`）。
   它的 `pcm_open`/`mixer_*` 依赖由**同一个 .a 里的 `pcm.o`/`mixer.o`** 满足。
3. 代码里 `#include "media/wav_utils.h"` 即可（`src/` 是 include 根）。

QA 自检命令（已上机）：

| 命令 | 作用 |
|---|---|
| `zkwav [<相对路径>]` | 后台线程跑 `zk_wav_play()`（默认 `audio/over.wav`）；**阻塞式播完整个文件**，只用于自检 |
| `zkplay [<相对路径>]` / `zkstop` | 框架 `ZKMediaPlayer` 探针（**反例**：用来复现"走 card1"这条结论）。首次 `zkplay` 会 `new` 一个实例 |

> ⚠️ 两个探针都要求先 `pcmfree`（`zkplay` 是打开被独占的设备会挂；`zkwav` 会打不开）。

## 4. 参考工程的音频初始化（`audio::init()`）

```cpp
setenv("ALSA_CONFIG_DIR", "/res/ui/alsa", 1);   // 用工程自带的 alsa 配置目录
setenv("ZKMEDIA_SOUND_UNADJABLE", "1", 0);      // 禁用 zkmedia 自己调音量
setenv("ZKMEDIA_H264_VBVSIZE", "1048576", 1);   // 视频缓冲
zk_audio_player_set_max_volume(MEDIA_SOUND_MAX_VOL);
change_output_mode(get_output_mode());           // 默认 E_AUDIO_PLAYER_MODE_SPK
```

它自带 `resources/alsa/`（`alsa.conf` 9137 B + `asound.conf` 7465 B）。`asound.conf` 的关键：

```
pcm.!default { type asym; playback.pcm "PlaybackRateDmix"; capture.pcm "Capture1Mic" }
pcm.PlaybackRateDmix {
  type plug → softvol → hooks → dmix
  slave { pcm "hw:0,0" }                      ← 默认设备最终就是 card0
  hooks.0 { type ctl_elems hook_args HookArgsPlayback }   ← 自动设 SPK/LINEOUT Switch=1
  control { name "Soft Volume Master"; card audiocodec }  ← 软音量
  rate_converter "awrate"
}
```

三件事值得记：

1. **`pcm.!default` 本来就指到 `hw:0,0`（card0）**，而且**设备固件自带的 `/etc/alsa/asound.conf`
   也是这样**（实测 `pcm.!default → PlaybackRateDmix`）。所以"ALSA 默认设备 = 喇叭"在本板是成立的。
2. **`hooks` + `ctl_elems` 声明式地打开 codec 输出开关**（`SPK Switch` / `LINEOUT Switch` / `LINEOUT Output Select`）
   —— 这正是我们当初用"原始 control 接口枚举 + 回读校验"手工做掉的那件事（`PgAudio::ensureSpeakerOn`）。
3. **`dmix`（`ipc_key 1111`）** 让多路 PCM 共享声卡。

> ⚠️ **但官方播放器和我们的 `PgAudio` 都是 tinyalsa 直连 `/dev/snd/pcmC0D0p`**（绕过 `default`），
> 所以 `asound.conf` 的 dmix **不会自动让两者共存** —— 它们仍然互斥。
> 要用上 dmix，得让应用侧打开 **`"default"`** 这个名字而不是 `hw:0,0`
> （未来可选优化：`PgAudio` + 播放器都走 `default`，则闹钟铃声可以和游戏音效混音）。

## 5. 与现有 `PgAudio` 的协同（闹钟必须知道）

`zk_audio_player` 自己的注释写着 **"独占模式，不支持多实例播放，同时只能有一个播放器输出声音"**；
而 `PgAudio` 的常开流**长期独占 `hw:0,0`**。两者要么错开、要么统一。

本工程选**错开**（改造成本最低、风险最小）：

```
闹钟响铃前：pg::globalAudio()->releasePcm();   // 让出 card0（QA `pcmfree`）
响铃期间：  zk_audio_player_init/put_frame…循环
响铃结束：  zk_audio_player_deinit();
             pg::globalAudio()->acquirePcm();   // 拿回（QA `pcmopen`，实测 ~100ms）
```

代价：切换期间游戏音效静音（闹钟场景可接受，本来也该让位）。

## 6. 待确认 / 未验证

- ⚠️ **"是否真的能听见"要耳朵确认**（软件侧只能证明"走对了卡 + 播放流程返回 0"）。
  自检：`pcmfree` → `zkwav audio/over.wav`。
- `zk_audio_player_set_volume()` 内部疑似写 `/dev/oflash`（字符串里出现），
  **未验证是否有副作用**；音量优先用 `PgAudio::setVolumePercent()`（codec digital volume）。
- 多实例版本（`zk_audio_multi_player_*`）未测（本板单喇叭，没有多路需求）。

## 7. 相关文件与命令

| 项 | 位置 |
|---|---|
| 库/头文件 | `src/dependencies/lib/libzkmedia.a`、`src/media/{audio_player.h,wav_utils.h,wav_utils.c}` |
| 探针代码 | `src/logic/mainLogic.cc`（`zkWavThread` / `ZkProbeListener` + QA 分支） |
| 参考工程 | `S:/projects/LearningProject/V851ExtendedScreen_ap_p2p/src/media/*`、`src/dependencies/lib/libzkmedia.a` |
| 实测截图/日志 | 见 `docs/MCP-待改清单.md` 与本文 §1 |
