# 触摸注入（V851s：`axs_ts`(MT-B) / `gt9xx`(MT-A) 两块面板）：为什么以前不通、现在怎么用

> 一句话：本机型**换过面板**——老的是 MT 协议 B（`axs_ts`），现在这台是 **MT 协议 A**
> （`gt9xx`）；而工程里的注入工具当年发的是 A 的 `SYN_MT_REPORT` 到 B 的屏上 ——
> 整帧被作废，于是"内核收得到、框架完全没反应"。
> 修成**自动判协议**之后 `tools/pginj.c` 在**两块屏上都实测可用**，能做真正的 UI 自动验收。
>
> 工具：`tools/pginj.c`（一条命令一次点击/滑动/长按）　协议探测：`pginj proto <dev>`
> ⚠️ **别写死 `/dev/input/event4`**：先 `ls /dev/input/` + `pginj proto`（见 §1）。
> 相关：`README.md` §七.9（触摸设备节点错位）、§八（工具用法）

---

## 0. 三十秒上手

```bash
# 交叉编译（工程同款容器/工具链；也可用 D:/zkswe/fun/toolchains/v85x/bin/…）
arm-unknown-linux-musleabihf-gcc -static -O2 -o pginj tools/pginj.c
adb push pginj /tmp/pginj && adb shell chmod 755 /tmp/pginj

adb shell "/tmp/busybox ls /dev/input/"                  # ← 先看节点（换机器必做）
DEV=/dev/input/event4                                   # 老面板(axs_ts/MT-B)；新面板是 event0(gt9xx/MT-A)
adb shell "/tmp/pginj proto $DEV"                        # → MT 协议 B（mt=1 mt_b=1 …）
adb shell "/tmp/pginj tap   $DEV 58 160"                 # 点第一张卡片
adb shell "/tmp/pginj press $DEV 240 400 900"            # 长按 900ms（应用判 >=700ms）
adb shell "/tmp/pginj swipe $DEV 240 620 240 250 24 12"
adb shell "/tmp/pginj key   /dev/input/event3 103 80"     # 物理键注入（一直可用）
```

**怎么判断注入生效**（应用每个触摸事件都会打日志）：

```
PocketGame touch: action=1 x=58 y=160 mode=0     ← 1=DOWN
PocketGame touch: action=2 x=58 y=160 mode=0     ← 2=UP
PocketGame: enter game[0] 2048                   ← 点击真的生效了
```

> `MotionEvent` 常量真值（`control/Common.h`）：**NONE=0 / DOWN=1 / UP=2 / MOVE=3 / CANCEL=4**。
> 别记成 Android 那套（Android 是 0=DOWN），会看反。

---

## 1. 本机型的触摸面板（**有两块，节点和协议都不一样**）

> ⚠️ 2026-09-14 实测：**换机器/换屏后节点会变**，注入脚本写死 `event4` 会直接失败
> （`open: No such file or directory`）。**先跑 `pginj proto`，别假设。**

| 面板 | 驱动 | 节点 | 协议 | 能力位特征 |
|---|---|---|---|---|
| A | `axs_ts`（I2C `2-003b`） | `event4`（次 65） | **MT-B** | 有 `ABS_MT_SLOT/TRACKING_ID/MT_PRESSURE`；`ABS_X/Y` 存在但 range **0..0** |
| B | `gt9xx`（虚拟输入） | **`event0`**（次 65） | **MT-A** | 只有 `ABS_MT_POSITION_X/Y` + `TOUCH_MAJOR`；**无** `ABS_X/Y`、无 `SLOT` |

```bash
adb shell "/tmp/busybox ls /dev/input/"          # 先看有什么（event3 恒为 gpio-keys）
adb shell "/tmp/pginj proto /dev/input/event0"   # → MT 协议 A / B / 单点
```

好消息：**框架自己会挑节点**。实测 `EasyUI.cfg` 里仍写着 `touchDev=/dev/input/event4`
（该节点已不存在），框架照样打开了 `event0`：

```bash
adb shell "ls -l /proc/<zkgui-pid>/fd | busybox grep input"
#  10 -> /dev/input/event0      ← 框架实际读的触摸节点
#  11 -> /dev/input/event3      ← gpio-keys
```

⇒ **应用侧不用改配置**；`pginj` 也**自动判协议**（MT-A 会走 `SYN_MT_REPORT` 那套），
两条面板都实测可用（B 面板：`pginj tap /dev/input/event0 240 75` → 应用 `action=1 → 2`、真的切了分类）。

---

## 2. 三条铁律（每条都是实测撞出来的）

### ① MT-B 上**绝不能发 `SYN_MT_REPORT`**

`SYN_MT_REPORT` 是 **type-A** 的点位分隔符。在 B 设备上发它，整帧被内核/框架判为无效：

| 用的工具 | 帧结构 | 结果 |
|---|---|---|
| 旧 `pginj` / `mt_test` | `TRACKING_ID,POS_X,POS_Y,TOUCH_MAJOR` + **`SYN_MT_REPORT`** + `BTN_TOUCH` + `SYN` | `dd` 能抓到 11 个事件，**应用日志一行都没有** |
| 现在的 `pginj` | `SLOT,TRACKING_ID,TOUCH_MAJOR,MT_PRESSURE,POS_X,POS_Y,BTN_TOUCH` + `SYN` | 应用立刻拿到 `action=1 → 2`，点击生效 |

### ② `BTN_TOUCH` 必须与位置**同帧**

只发 MT 位置（不发 `BTN_TOUCH`）时，框架会给 **DOWN + MOVE，但永远不给 UP** ——
它以为手指一直按着。后果是：**第一次注入之后，后面所有注入都退化成 MOVE**，
一个点击都点不出来（现象看着像"注入时灵时不灵"，其实是从第一步就没收尾）。

### ③ 抬起帧要带 `TRACKING_ID = -1`

MT-B 的"手指离开"语义；`BTN_TOUCH=0` 同帧下发。缺了它同样会留下"幽灵手指"。

> 万一已经踩进②/③的坑（应用卡在"按下"态）：**再点一次就恢复**，或重启应用。
> 判据：日志里只有 `action=3/2`（MOVE/UP）而没有 `action=1`。

---

## 3. 正确的帧长什么样（已验证）

```
按下帧：  ABS_MT_TRACKING_ID 1
          ABS_MT_TOUCH_MAJOR 60
          ABS_MT_PRESSURE 255
          ABS_MT_POSITION_X/Y
          BTN_TOUCH 1
          SYN_REPORT
抬起帧：  ABS_MT_TRACKING_ID -1
          ABS_MT_TOUCH_MAJOR 0
          ABS_MT_PRESSURE 0
          BTN_TOUCH 0
          SYN_REPORT
（`ABS_MT_SLOT 0` 可选：值为 0 时内核会因"与当前值相同"而丢弃，不影响）
```

对照实测的原始流（`timeout 4 busybox dd if=/dev/input/event4 of=/tmp/e.bin bs=16`，
**注意 32 位 ARM 的 `struct input_event` 是 16 字节**，用 `bs=24` 解出来是乱的）：

```
注入 tap 240 75 →
  ABS MT_TRACKING_ID 1 / MT_TOUCH_MAJOR 60 / MT_PRESSURE 255 / MT_POS_X 240 / MT_POS_Y 75
  KEY BTN_TOUCH 1 / SYN
  ABS MT_TRACKING_ID -1 / MT_TOUCH_MAJOR 0 / MT_PRESSURE 0
  KEY BTN_TOUCH 0 / SYN
应用侧 → PocketGame touch: action=1(240,75) → action=2(240,75) → category -> 工具(7 项)
```

---

## 4. 真机验收记录（2026-09-14，全部用 `pginj` 注入，无手指参与）

| 验收项 | 命令 | 实测 |
|---|---|---|
| 主界面点卡片进应用 | `pginj tap … 58 160` | `action=1 → action=2` → `enter game[0] 2048` ✅ |
| 分类 tab 点击 | 3 个 tab 各点 | `category -> 工具(7 项)` / `系统(1 项)` / `游戏(13 项)` ✅ |
| 连点可靠性 | 连续 6 次点 tab | **6/6** 全部命中 ✅ |
| 游戏内滑动 | `swipe … 240 620 240 250 24 12` | `action=1 → 3… → 2`，棋盘像素变化 **57.9%**（真的走了） ✅ |
| 长按 | `pginj press … 240 400 900` | DOWN 03.525 → UP 04.435 = **910ms** ✅ |
| 物理键回归 | `pginj key …event3 103 80` | `code=103 -> 音量- (短按 77ms, mode=1)` ✅ |
| 菜单图标（bug 回归） | 真触摸切分类来回 | 每行图标互不相同；切回后**逐行字节一致** ✅ |
| 工作界面禁屏保 | 点卡片进游戏后停 40s | `performScreensaverOn` 计数 **0** ✅ |

与 QA 通道（`/tmp/pg_autostart`）的分工：

- **`pginj`**：验证"手指能不能点到"、"点击/滑动/长按真的生效吗" —— 走的是**和真手指同一条
  evdev 通路**，能测到框架的命中判定。
- **QA 通道**：验证"某个页面/状态下的行为"（进应用、换分类、开关屏保…），确定性更高、
  不需要坐标。**两者不是替代关系**。

---

## 5. 坑与注意

- `/tmp` 会被 `fun launch` 清掉，注入工具要重新 push。
- 32 位 ARM 上 `struct input_event` = **16 字节**（`<iiHHi`），PC 侧解析别用 24。
- `busybox` 没有 `logcat` applet（`logcat: applet not found`）—— 读日志用系统 `/bin/logcat`。
- 只写打开（`O_WRONLY`）时 `EVIOCGBIT` 可能失败；`pginj` 先试 `O_RDWR`，失败再退 `O_WRONLY`，
  能力探测失败就**默认按本板 MT-B 发**。
- 屏幕坐标 = UI 坐标（480×800）直接写；游戏画布内部会再减 `CANVAS_X/Y`，注入不用管。
- 抓屏前先 `saver off`（30 秒自动屏保会盖住画面）。

---

## 6. ★★ 注入能驱动**控件级**点击（2026-09-15 澄清，附"怎么用它判故障"）

`pginj tap /dev/input/event4 <x> <y>` **能直接驱动框架控件级点击** —— `list click` /
`tab click` / `onButtonClick_*` 都会真的触发。§4 的验收记录本来就是这么测的。

⚠️ **踩过的误判**：2026-09-15 排查"主界面点不动"时，注入怎么都点不出控件回调，
我据此写下"注入驱动不了控件级"的结论 —— **那是错的**。真因是当时**全局状态栏常显**
（整屏 topmost 浮层）把全系统的控件级触摸吃掉了，注入和手指都一样点不动；
把状态栏 `hideStatusBar()` 之后，**同一句注入立刻**打出 `list click index=0 subId=24020`、
并真的 `enter game[0] 2048`（详见 `docs/statusbar.md` §4）。

**这条现在是最有用的排查手段**：症状"界面不动"时，先用注入点一下，按日志分层定位：

| 日志表现 | 结论 | 下一步 |
|---|---|---|
| 有 `PocketGame touch: action=1/2`，但**没有** `list click`/`tab click` | 触摸到了**页面级**，**控件级被挡** | 查有没有**整屏浮层正显示着**（状态栏 / 屏保 / 模态窗口），先 `hideStatusBar()` 再试 |
| `action=3/2` 而没有 `action=1` | **幽灵手指**（②/③ 两个坑没收尾） | 再点一次或重启应用 |
| 连 `action` 都没有 | 触摸没到应用 | 查注入节点（`pginj proto`）、`/tmp/pginj` 是否还在（`fun launch` 会清 /tmp） |
| 有 `list click` 但界面没变 | 触摸**没问题**，是业务逻辑 | 去查回调实现，别再折腾触摸 |

> 口诀：**"控件级回调没日志 = 有东西在挡"**；而"整屏浮层 + `touchable=false`"是头号嫌疑
> （`touchable=false` **不是**穿透，`setTouchPass(true)` 在整屏 topmost 窗口上也无效）。

---

## 7. 捷径：**用 QA 直接喂画布触摸**（不经过内核与框架，2026-09-15）

`pginj` 走的是内核输入设备（真实链路，最好），但它有两个短板：
① 需要推送工具、探测节点；② 偶尔会遇到"注入了但应用收不到"（幽灵手指 / 面板驱动状态），
排查它本身要花时间。

**如果只是要验"画布游戏的手势判定"**，可以直接用 QA 通道把触摸喂进画布 —— 
`src/logic/mainLogic.cc` 里本来就有（**一直存在，之前没人用**）：

```
tap  <x> <y>      # DOWN + UP（画布坐标）
down <x> <y>      # 只发 DOWN
move <x> <y>      # 只发 MOVE
up   <x> <y>      # 只发 UP
```

它们直接调 `dispatchCanvasTouch(act, x, y)`，**完全绕开内核与框架触摸链路** ——
所以既能用来验游戏逻辑，也能用来**二分定位**：
"QA 喂进去有反应、`pginj` 注入没反应" ⇒ 问题在内核/框架那一段，不在游戏逻辑。

⚠️ 坐标是**画布坐标**（480×540，不是 800 高的屏幕）；画布底部 62px 是软按钮条
（`PG_SOFT_BAR_H`），喂手势要避开。

**判据用分数**（`gscore` QA：打印 `title/score/state`）—— 分数只在有效操作时变，
比抓帧稳（抓帧会被游戏的自然演进干扰）。完整案例见 `docs/games-review.md` 末节
（俄罗斯方块上滑直落阈值调优）。

### 顺带记两个"测不出来"的坑

- **游戏进完是 `GSTATE_READY`**：`enter <n>` 只是加载，必须再喂一次 DOWN
  （`tap <画布中心>`）才开始。忘了这步的话 `gscore` 报 `state=0`，
  所有手势都被 READY 分支吞掉 ⇒ **测试结果会全部"看起来没触发"**（我第一次就这么白跑一轮）。
- **QA 是"内容变化才执行"**：连续下发同名命令只有第一条生效。
  要在末尾加一个无效行制造差异（如 `q123`）。**别用 `#` 加后缀** —— `#` 后面会被当注释剥掉。

