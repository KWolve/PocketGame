# 音频输出 + 输入设备（触摸 / 物理按键）硬件背景与实测参数

> 🔍 **检索导引（命中条件）**：用户问「**设备没有声音 / 完全静音 / 音效不响 / 喇叭不出声 / 播放日志正常却听不到**」
> 「**声音输出到哪块声卡 / 怎么选声卡 / 默认通路是哪块卡 / 喇叭不响但另一路在响**」
> 「**codec 输出开关 / SPK Switch / LINEOUT Switch 是 Off**」「**混音器设置了不生效 / tinymix 写了没反应**」
> 「**有爆音 pop / 咔哒声 / 每次出声前后有杂音**」「**短音效不响、只有最长的那个音能响**」
> 「**声音太小 / 怎么调音量 / 默认音量是多少**」
> 「**触摸不生效 / 点了没反应 / 触摸节点是哪个 / eventN / touchDev 配错**」
> 「**触摸不准 / 触摸坐标偏移 / 触摸坐标不对 / 点击位置和实际不符 / 坐标偏移 / 越往下越偏 / 上下偏移 / 底部点不到 / 触摸量程不对 / 触摸偏差**」
> 「**注入触摸坐标恒 0 / MT 协议 / Type A 还是 B / 触摸量程 / ABS_MT_POSITION 最大值 / 触摸不上报坐标**」
> 「**物理按键不响应 / 按键键码是多少 / gpio-keys / 按键被识别成触摸手势**」
> 且平台为 **全志 V851s / V853 / V85X（FlyThings / EasyUI）** → **本篇就是答案**。
> 音频通路与 evdev 设备枚举的**方法是平台无关的**（任何 Linux + ALSA / input 子系统都适用），
> 本篇的**实测量值**来自 V851s 板实测；换板时按 §4 清单逐项重测即可。
> 相关：`../v85x/display-layer-debug.md`（显示/取帧）、`../devflow/touch-inject-autotest.md`（触摸注入自测）。

## 一句话

本板**有喇叭的声卡是 `card0`（片内 codec `audiocodec`）**，但**系统 ALSA 默认通路指向 `card1`
（`snddaudio0` → 外部 ES714X I2S DAC，未接喇叭）** —— 所以"日志一切正常、PCM 也打开了，就是没声音"
的**头号原因就是播到了没接喇叭的那块卡**；**第二名是 codec 输出开关 `SPK/LINEOUT Switch` 被关掉**。
输入侧：**触摸 / 物理三键**，且**节点名与 evdev 次设备号是错位的**，**绝不能按设备号猜节点**。

> ⚠️ **触摸节点不唯一（2026-09-14 实测：换机器/换屏后节点会变）**：
> | 面板 | 驱动 | 节点 | 协议 | 特征 |
> |---|---|---|---|---|
> | A | `axs_ts`（I2C `2-003b`） | `event4`（次 65） | **MT 协议 B**（有 `ABS_MT_SLOT`/`TRACKING_ID`） | 有 `ABS_X/Y` 但 range 是 `0..0` |
> | B | `gt9xx`（虚拟输入） | **`event0`**（次 65） | **MT 协议 A** | 只有 `ABS_MT_POSITION_X/Y`，无 `ABS_X/Y` |
>
> 两块面板**能力位不同** ⇒ 注入/读事件前**必须先探**（`pginj proto /dev/input/eventN`，
> 或 `cat /sys/class/input/eventN/device/capabilities/abs`，注意**高位字在前**）。
> 好消息：**框架自己会挑**（实测 `EasyUI.cfg` 里还写着 `touchDev=/dev/input/event4` 时，
> 框架仍然打开了存在的 `event0`：`ls -l /proc/<pid>/fd` 可见 `10 -> /dev/input/event0`）。
> 所以**应用侧不用改配置**；但注入脚本**不能写死 event4**，详见 `docs/touch-inject.md`。

> 目标场景：480×800 竖屏掌机形态（触摸屏 + 3 个物理键 + 板载喇叭），应用走 FlyThings/EasyUI。

## 0. 实测环境（先对齐，换板时逐项重测）

| 项 | 实测值 |
|---|---|
| SoC / 平台 | 全志 V851s（FlyThings 平台标识 `V85X`） |
| 屏幕 | 480×800 竖屏；`/sys/class/graphics/fb0/virtual_size` = `480,1600`（**双缓冲**，取值见取帧文档） |
| 声卡 | `card0` = `audiocodec`（片内 codec，**喇叭在这**）；`card1` = `snddaudio0`（外部 ES714X，**未接**） |
| 输入 | `event3` = `gpio-keys`（3 个物理键）；`event4` = `axs_ts`（触摸屏，I2C 2-003b） |
| 应用运行身份 | `uid=0`（`/dev/snd`、`/dev/input` 下节点均为 `crw------- 0 0`，非 root 需另给权限） |

---

## 1. 音频输出

### 1.1 两块声卡：一场"播到没接喇叭的卡"的经典事故

```bash
adb shell "ls -l /sys/class/sound/"          # 看有几块卡、各挂在哪
adb shell "cat /sys/class/sound/card0/id"    # audiocodec
adb shell "cat /sys/class/sound/card1/id"    # snddaudio0
```

| 声卡 | `/sys/class/sound/cardN/id` | 平台设备（sysfs） | 编解码 | 本板接喇叭？ |
|------|------------------------------|-------------------|--------|--------------|
| **card0** | `audiocodec` | `soc@03000000:codec_mach` | 片内 audio codec（SPK 由 LINEOUTL 引出） | ✅ **喇叭接这里** |
| card1 | `snddaudio0` | `soc@03000000:daudio0_mach` | 外部 ES714X I2S DAC | ❌ 未接 |

PCM 设备节点（`/dev/snd/`）：

```text
controlC0  controlC1  pcmC0D0c  pcmC0D0p  pcmC1D0c  pcmC1D0p  timer
                      ↑ 播放用这块（card0 device0 playback）
```

> ⚠️ 本板内核**没有挂 `/proc/asound`**（`cat /proc/asound/cards` = No such file），
> 别用 `proc` 查声卡信息，改用 `sysfs`（如上）+ `tinymix -D N`。

### 1.2 默认通路指向没接喇叭的卡（"日志全正常却静音"的头号原因）

`/etc/alsa/asound.conf`：

```text
pcm.!default { type asym; playback.pcm "PlaybackRateDmix"; capture.pcm "CaptureDsnoop" }
pcm.PlaybackRateDmix → plug → softvol → hooks → dmix → slave.pcm "hw:1,0"   # ← card1！
```

`dmix` 的 `slave.pcm` 写死 **`hw:1,0`**（即 card1 = ES714X），所以：

- 任何**走 ALSA 名字解析**（`pcm.!default`）的播放路径都会静音；
- 框架媒体播放器（`ZKMediaPlayer` → eyesee-mpp AO）**自己拼设备名**（`libmedia_mpp.so` 里直接有
  `hw:0` / `hw:1,0` / `hw:UAC1Gadget` 字面量与 `AW_MPI_AO_SetPcmCardType`），于是**播到 card1**；
- 症状极具欺骗性：`create ao channel` / `media play ok` / `play completed` 全部 success，
  `/proc/<pid>/fd` 里能看到 `pcmC1D0p`，**就是一个音都没有**。

❌ **改 ALSA 配置救不回来**（实测结论，别再重复踩）：

| 尝试 | 结果 |
|---|---|
| 工程内放一份把 `PlaybackRateDmix` 覆盖到 `hw:0,0` 的配置 + `setenv("ALSA_CONFIG_PATH", …)` | 播放时打开的**仍是 `pcmC1D0p`** |
| 判定实验：把那份配置**故意写成语法错误**再部署 | 一播放进程就退出被 init 拉起 → **说明配置确实被 AO 读到了**；合法覆盖却无效 ⇒ **AO 不按 ALSA 名字解析，是自己拼设备名** |
| 用平台"播 PCM 的包"（`zkmedia` / `audio-utility`） | **V85X 没有这种包**（只有 F133/F136 有）；设备固件里全盘搜 `zk_audio_player` 为空 |

**可行解只有两个**：① 自己 `dlopen("libasound.so.2")` **直出 `hw:0,0`**（推荐，见 §1.6）；
② 改用 `aw-middleware` 的 AO 通道并显式 `AW_MPI_AO_SetPcmCardType(dev, chn, PCM_CARD_TYPE_AUDIOCODEC)`
（必须在 `CreateChn` **之后**调，否则崩在 `audioHw_AO_searchChannel`）。

### 1.3 codec 输出开关（`SPK/LINEOUT Switch`）—— 静音的第二名，必须**回读校验**

`tinymix -D 0` 全表 24 个控件，关键的两路输出开关是 **BOOL** 类型：

```text
numid  type  name              value
 19    BOOL  LINEOUT Switch    Off   ← 应为 On
 20    BOOL  SPK Switch        On    ← 应为 On
```

三条硬知识（都是实测踩出来的）：

1. **系统配置里其实已经要求打开它们** —— `/etc/alsa/asound.conf` 的
   `hook_args.HookArgsDefault / HookArgsPlayback` 里明确写了 `SPK Switch = 1`、`LINEOUT Switch = 1`
   （`preserve true` / `optional true`），**但实测启动后仍是 Off** ⇒
   **不能依赖系统 hook，应用必须自己在打开 PCM 之后设一次**。
2. **按名字查 simple-mixer 开关 → 永远查不到**：`snd_mixer` 的 simple-element 会把 ` Switch`
   后缀当**角色**拆掉（元素名其实是 `SPK`），用 `snd_mixer_find_selem("SPK Switch")` 必然失败。
   更糟的写法是"查不到只打一条 WARN 就返回"——程序会**以为已经打开**，排查时被这条误导很久。
   ✅ 正解：走**原始 control 接口**（`snd_ctl_open` → `elem_list` 枚举控件 → 挑名字含
   `SPK`/`LINEOUT` 且类型为 BOOLEAN 的 → `elem_write` 置 1 → **`elem_read` 回读校验**），
   日志明确报 `输出开关 SPK Switch -> On（回读 1）`。**凡是"设置了但没验证结果"的写法都要回读。**
3. ⚠️ **本板 `tinymix` 只认控件序号、不认名字**：
   `tinymix 'SPK Switch' 1` **返回 0 但值不变**（静默失败，极坑）。手工排查必须写序号：

```bash
adb shell "tinymix 20 1; tinymix 19 1"        # SPK Switch / LINEOUT Switch → On
adb shell "tinymix -D 0"                       # 回读确认
adb shell "tinymix -D 1"                       # card1 的控件表（另一块卡）
```

**音量（问「声音太小 / 怎么调音量 / 默认音量多少」看这里）** —— 同一张表里的 INT 控件，
**同样按序号写**：

| 序号 | 控件 | 本板实测值 | 作用 |
|---|---|---|---|
| 7 | `digital volume` | 63 | 数字音量 |
| 8 | `DAC volume` | 160 | DAC 音量 |
| 15 | `LINEOUT volume` | 31 | LINEOUT 音量（**喇叭走这一路**，调音量优先动它） |

```bash
adb shell "tinymix 15 31"        # 例：设置 LINEOUT 音量
```

> 软件增益也能做（写数据前乘系数），本工程留了口子但默认不衰减；
> **优先调上面的 codec 音量控件**，改软件增益要小心削顶失真。

### 1.4 PCM 参数：`start_threshold` 不设成 1 帧，短音效会被整段吞掉

| 参数 | 实测/建议值 | 说明 |
|---|---|---|
| 格式 | **22050 Hz / 单声道 / S16_LE** | 音效 WAV 就是这格式 = 直通，不需要重采样 |
| period / buffer | **160 帧 × 4 = 640 帧 ≈ 29 ms** | 小周期小缓冲：音效延迟低、又留调度余量 |
| **`start_threshold`** | **必须 = 1 帧** | 默认≈半个缓冲；本板驱动默认缓冲实测 **14336 帧 ≈ 0.65 s**，而音效只有 0.19~0.32 s，**全部躺在缓冲里 DMA 不起播** |
| `silence_size` | = 整缓冲 | 偶发欠载时内核自动补静音，流不中断 |

> 🔎 **"只有最长的那个音效会响"就是这个坑的指纹** —— 实测只有 0.82 s 的"失败"音超过默认阈值，
> 其余短音效全哑。反过来，看到这个现象先查 `start_threshold`，不要怀疑音效文件。

### 1.5 无声排查顺序（照这个顺序做，别猜）

```bash
# ① 输出开关（第一嫌疑！）
adb shell "tinymix -D 0"                       # SPK Switch / LINEOUT Switch 是否 On
# ② 硬件通路交叉验证（直接抢设备播一段测试音；常开流不会独占，能抢）
adb shell "tinyplay /tmp/tone.wav -D 0"        # card0 有声 → 喇叭在这块
adb shell "tinyplay /tmp/tone48s.wav -D 1"     # card1 需 48k/立体声；本板应无声
# ③ 应用实际开的是哪块 PCM
adb shell "ls -l /proc/<pid>/fd | grep snd"    # pcmC1D0p = 开错卡了；pcmC0D0p = 卡对
# ④ 默认通路指向
adb shell "cat /etc/alsa/asound.conf"          # dmix slave.pcm 是 hw:1,0 还是 hw:0,0
```

- ⚠️ `tinyplay` 的 `-D` 必须写在**文件名之后**（`tinyplay x.wav -D 1`），写在前面会被当成文件名；
- ⚠️ `hw:1,0`（ES714X）**只接受 48 kHz 立体声**，喂 22050 单声道会直接
  `cannot set hw params: Invalid argument`（这**不代表** card0 也挑格式）；
- 生成"能数出声数"的测试音，别拿耳朵猜：`tools/gentone.py <频率> <秒> <输出wav> [采样率] [声道]`。

### 1.6 推荐实现形态：一条**常开流** + 空闲写静音（消爆音）

爆音（pop）的根因是**反复 open/close PCM** —— 每次开关都让 DAC 充放电，听感就是"咔/噗"。
做法：独立音频线程按周期（160 帧 ≈ 7 ms）**不停**往 `hw:0,0` 写数据，**有音效写音效、没音效写 0**，
流永不停止。附带好处：

- 音效在 init 时**一次性预载**并统一成流参数 → 触发音效只是换一块内存指针，
  **UI 线程零 I/O、零阻塞**；
- 音效开关只影响"是否写音效"（关掉时流照旧跑静音，所以**开关本身也没有 pop**）；
- 音量交给驱动默认值（本板 `digital volume 63` / `DAC volume 160` / `LINEOUT volume 31`）。

**自检（不用耳朵）**：空闲不操作时 `/proc/<pid>/fd` 里就应有 `pcmC0D0p`，
播放音效时 **pid 与 fd 都不变**（证明流没有重开过）；音效关闭期间应能靠日志判定
（打一条 `音效已关闭，忽略播放请求`，或数"写音效"次数为 0）。

---

## 2. 输入设备（触摸 + 物理按键）

本板输入只有两个节点：**`event3` = `soc@03000000:gpio-keys`（3 个物理键，键码 103/105/108）**、
**`event4` = `axs_ts`（触摸屏：I2C 2-003b、MT Type B、5 点、坐标量程 X=480 / Y=960）**。
触摸最常见的两个坑是**节点配错**（`touchDev` 指向不存在的节点）与**量程 ≠ 逻辑分辨率**；
按键最常见的坑是**键码映射错、或误加了过滤把真按键屏蔽**。本节四个小节依次给出实测值、
节点错位陷阱、`EasyUI.cfg` 优先级、键码表与自检命令。

### 2.1 实测清单

```bash
adb shell "cat /proc/bus/input/devices"
```

```text
I: Bus=0019 ... N: Name="soc@03000000:gpio-keys"   H: Handlers=event3   B: EV=3   B: KEY=1280 0 0 0
I: Bus=0018 ... N: Name="axs_ts"                   H: Handlers=event4   B: EV=b   B: KEY=420 0 0 ...
```

| 节点 | 设备号（`ls -l /dev/input/`） | 名字 | 说明 |
|---|---|---|---|
| `/dev/input/event3` | `13, 64` | `soc@03000000:gpio-keys` | **3 个物理按键**（键码 103/105/108） |
| `/dev/input/event4` | `13, 65` | `axs_ts` | **触摸屏**（I2C `2-003b`，`INPUT_PROP_DIRECT`，**MT Type B / 5 点**，见下） |

#### 完整能力位（用 `getevent -lp`，**带 min/max 量程** —— 判协议、判坐标量程就看这个）

```text
add device 1: /dev/input/event4
  name: "axs_ts"
  events:
    KEY (0001): BTN_TOOL_FINGER   BTN_TOUCH               ← 只有这两个，没有任何方向键
    ABS (0003): ABS_X              : value 0, min 0, max 0    ← ⚠️ max=0：单点轴没配量程，不可用
                ABS_Y              : value 0, min 0, max 0    ← ⚠️ 同上
                ABS_MT_SLOT            : min 0, max 4         ← 支持 5 点触控
                ABS_MT_TOUCH_MAJOR     : min 0, max 255
                ABS_MT_POSITION_X      : min 0, max 480       ← 逻辑宽 = 480 ✓
                ABS_MT_POSITION_Y      : min 0, max 960       ← ⚠️ 960 ≠ 屏高 800！
                ABS_MT_TRACKING_ID     : min 0, max 65535     ← 有 SLOT+TRACKING_ID ⇒ MT Type B
                ABS_MT_PRESSURE        : min 0, max 255
  input props: INPUT_PROP_DIRECT

add device 2: /dev/input/event3
  name: "soc@03000000:gpio-keys"
  events:
    KEY (0001): KEY_UP   KEY_LEFT   KEY_DOWN                   ← 印证三键 = 103/105/108
```

**三个必记点**：

1. **本板触摸是 MT Type B 协议**（`ABS_MT_SLOT` + `ABS_MT_TRACKING_ID` 齐全，最多 5 点）。
   自己注入触摸**必须**走 MT 序列：
   `SLOT` → `TRACKING_ID`（按下给非 -1 / 抬起给 -1）→ `MT_POSITION_X/Y` → `SYN_REPORT`；
   **用老的单点协议（`ABS_X/Y` + `BTN_TOUCH`）不生效**。
   （同类坑见 `../devflow/touch-inject-autotest.md`：别板 gt9xx 用单点协议注入 → 坐标恒 0。）
2. **`ABS_MT_POSITION_Y` 的 max = 960，而屏幕只有 800 高** —— **触摸量程 ≠ 逻辑分辨率**。
   自行读 evdev 时必须按 max 归一化（`x/480*480`、`y/960*800`）；直接当像素用会**偏下/底部点不到**。
   换板遇到"触摸点不准 / 越往下越偏"，先量这个比值。
   （本工程走框架 `touchDev`，映射由框架做，应用层无感 —— 只有自读 evdev / 自写注入才踩。）
3. **`ABS_X`/`ABS_Y` 的 max=0**（单点轴没配量程）→ 单点协议不仅不生效、读出来还是 0。
   这正是"用单点协议注入坐标恒 0"的直接原因：**判断触摸协议一律看 `getevent -lp`**。

### 2.2 陷阱一：节点名与 evdev 次设备号**错位** → 不能按设备号猜节点

`event3` 的次设备号是 **64**、`event4` 是 **65**（常规应是 event0 = 次 64）。
**所以千万不要按"次设备号"推断哪个是触摸**，要按**名字 / 能力位**判断：

```bash
adb shell "ls -l /dev/input/"                                  # 看 (major, minor)
adb shell "getevent -lp"                                       # 带量程看能力位（谁有 ABS_MT_* 谁是触摸；顺带看坐标 max）
adb shell "cat /sys/class/input/input0/capabilities/key"       # 能力位图（高位字在前读）
```

### 2.3 陷阱二：框架读哪个触摸节点由 **EasyUI.cfg** 决定，且 **`/tmp` 那份优先**

```bash
adb shell "cat /res/etc/EasyUI.cfg"    # 出厂配置（/res 是只读 squashfs，改不了）
adb shell "cat /tmp/EasyUI.cfg"        # 部署期由工程 package.properties 生成 —— 生效的是这份
```

本板出厂配置写的是 `touchDev: /dev/input/event1`，而这块板上**根本没有这个节点**
（`/dev/input/` 下只有 event3/event4）→ 触摸完全无响应。工程侧覆盖：

```properties
EasyUI.cfg={"rotateScreen": 0, "touchDev": "/dev/input/event4"}
```

> `rotateScreen` 是**硬件物理方向适配**（另见 `../v85x/display-layer-debug.md`）；
> `rotateTouch` 只在触摸方向与显示不一致时才写。
> 联调救急可临时补节点（`/dev` 是 tmpfs，重启即失效）：
> `busybox mknod /dev/input/event1 c 13 65` —— **这只是临时方案**，正解是让 `touchDev` 指向真实节点。
> 自检判据：注入一次划动后看应用 CPU / 画面有没有变化，**只读日志容易被"事件到了但没人接"骗过**。

### 2.4 物理按键键码（本板实测固化）

本板三个物理键**都是方向键**，实测键码：

| 键码 | Linux 键名 | 逻辑映射 |
|---|---|---|
| **103** | `KEY_UP` | A（确定 / 开始 / 暂停） |
| **105** | `KEY_LEFT` | B（返回） |
| **108** | `KEY_DOWN` | C（重玩 / 清空） |

能力位 `B: KEY=1280 0 0 0` 按"高位字在前"读 → `0x1280` 落在 word3 → bit `96+7/9/12`
→ 键码 **103 / 105 / 108**，与 `getevent` 实测一致。

> ⚠️ 这三个都是**方向键**，很容易被误判成"触摸手势合成的伪按键"。**实测不是**：
> 触摸设备 `axs_ts` 的能力位里只有 `BTN_TOUCH` / `BTN_TOOL_FINGER`（没有任何方向键），
> 且 `libeasyui.so` / `libbase-utility.a` 里**没有**「手势 → 按键」的合成逻辑。
> ⇒ 方向键只可能来自这 3 个物理键，**直接映射即可，不要加任何过滤**（加过滤反而把真按键屏蔽）。

```bash
adb shell "getevent -l"                     # 逐个按键：看 0001 后面的事件码（0x67=103 等）
adb shell "ps" | grep zkgui                 # 确认应用进程；按键会打日志便于核对
```

### 2.5 输入侧自检（换板必做）

```bash
adb shell "cat /proc/bus/input/devices"     # ① 有几个输入设备、名字/handlers 分别是什么
adb shell "ls -l /dev/input/"               # ② 节点名 ↔ 次设备号是否错位
adb shell "getevent -lp"                    # ③ 能力位**带量程**：协议(MT-A/B)、坐标 max、几个触点
adb shell "getevent -l"                     # ④ 实按/实划一次，核对键码与上报的坐标值
adb shell "cat /sys/class/input/input1/capabilities/abs"   # ⑤ 原始能力位图（交叉验证上一条）
```

---

## 3. 一页速查（排障直接照抄）

| 现象 | 第一嫌疑 | 立即验证 |
|---|---|---|
| 完全无声，但日志/参数全正常 | **播到了没接喇叭的 card1** | `ls -l /proc/<pid>/fd \| grep snd` 看是 `pcmC0D0p` 还是 `pcmC1D0p` |
| 完全无声，fd 显示 card0 正确 | **codec 输出开关 Off** | `tinymix -D 0` 看 `SPK/LINEOUT Switch`；`tinymix 20 1; tinymix 19 1` |
| 短音效不响、只有最长的响 | **`start_threshold` 太大** | 设成 1 帧，见 §1.4 |
| 出声前后有"咔/噗" | 反复 open/close PCM | 改常开流 + 空闲写静音，见 §1.6 |
| `tinymix '名字' 1` 没反应 | **本板只认序号** | 用 `tinymix 20 1`（19 = LINEOUT） |
| 触摸完全无响应 | **`touchDev` 指向不存在的节点** | `cat /tmp/EasyUI.cfg` + `ls /dev/input/` |
| 触摸点不准 / 越往下越偏 / 底部点不到 | **触摸量程 ≠ 逻辑分辨率** | `getevent -lp` 看 `ABS_MT_POSITION_Y max`（本板 **960** vs 屏 800）→ 自读 evdev 要按 max 归一化 |
| 注入触摸坐标恒 0 / 完全没反应 | **用错触摸协议** | 本板是 MT Type B（有 `SLOT`+`TRACKING_ID`），必须走 MT 序列；`ABS_X/Y` 的 max=0，见 §2.1 |
| 认错触摸/按键节点 | 按次设备号猜了 | 按名字与能力位判断，见 §2.2 |
| 物理键不响应 | 键码映射错 / 被过滤逻辑屏蔽 | `getevent -l` 实测，见 §2.4 |

## 4. 换板 / 移植对照清单（逐项重测，别照抄量值）

1. **声卡数**：`ls /sys/class/sound/` → 喇叭接哪块？（`tinyplay x.wav -D N` 逐个试听，最快）
2. **默认通路**：`cat /etc/alsa/asound.conf` → `dmix slave.pcm` 指向的卡是否 = 喇叭那块？
3. **输出开关**：`tinymix -D 0` → 有哪些 BOOL 输出开关、**序号**各是多少（名字不可靠）？
4. **框架播放器走哪条路**：部署后 `ls -l /proc/<pid>/fd | grep snd`，是 `pcmC?D0p` 哪个？
5. **PCM 能力**：目标卡接受的格式/采样率（用 `tinyplay` 试，别猜；不同卡要求可能不同）
6. **输入设备**：`cat /proc/bus/input/devices` → 触摸/按键分别是哪个 event，**次设备号是多少**
7. **触摸配置**：`cat /tmp/EasyUI.cfg` 的 `touchDev` 是否指向真实节点；`rotateScreen/rotateTouch` 是否匹配物理方向
8. **触摸协议与坐标量程**：`getevent -lp` → 是 MT Type A 还是 B（有无 `SLOT`/`TRACKING_ID`）、最多几个触点、
   **`ABS_MT_POSITION_X/Y` 的 max 是否等于逻辑分辨率的宽/高**（本板 X=480 ✓ / **Y=960 ≠ 屏高 800**）；
   自读 evdev 与自写注入都必须按这些来
9. **按键键码**：`getevent -l` 实按核对；能力位 `B: KEY=` 可交叉验证

## 5. 参考实现

- 音频：`src/platform/PgAudio.cpp` / `PgAudio.h` —— `dlopen("libasound.so.2")` 直出 `hw:0,0`、
  常开流 + 空闲写静音、`start_threshold=1`、原始 control 接口开输出开关并回读校验。
- 输入：`src/logic/mainLogic.cc` 顶部三行键码表（`PG_KEYCODE_A/B/C`）；
  EasyUI 触摸节点由工程 `package.properties` 的 `EasyUI.cfg` 覆盖。
- 取帧/像素验收：`../v85x/display-layer-debug.md`、`../devflow/ui-layout-verify.md`。
