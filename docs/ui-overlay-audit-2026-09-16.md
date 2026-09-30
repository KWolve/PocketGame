# Ui 目录布局「浮层覆盖」检讨报告（2026-09-16）

> 🔍 **检索导引（命中条件）**：用户问「**状态栏下面有一条灰/黑边**」「**页面顶部被切掉半截**」
> 「**标题显示不完整**」「**游戏里看不到自己在玩哪个游戏**」「**世界时钟页不知道在哪一页**」
> 「**视频播放时画面顶部被状态栏压住**」「statebar/navibar 盖住 UI 怎么排查」
> 且平台为 **全志 V851s / V85X + FlyThings(EasyUI)** → **本篇就是答案**。
> 判据工具：`tools/check_overlay.py`（已接进 `tools/gen_ui.py`）。

## 0. 需求原文

> 「把 Ui 目录下的 json 布局全部检讨一遍，通过 MCP 你是可以了解到布局的结构的，
>   navibar 是盖在 UI 上面的，检讨有 UI 区域覆盖的情况下告警并修复，
>   如果你不确认的部分就汇总提出来让我审批。修改后一次性更新到设备测试。」

## 1. 判据（几何，别凭感觉改）

| 浮层 | 几何 | 显隐 |
|---|---|---|
| **navibar**（状态栏） | **480x52 @(0,0)**，`topmost=true` | **常显**；只在**屏保**与**视频播放页**隐藏 |
| **statusbar**（音量 OSD） | 240x160 @(120,520)，`topmost=true` | **按需**（音量变化弹出、1.6s 收起）|

⇒ navibar 压住的是 **屏幕 y ∈ [0,52)** 这一整条。
⇒ **页面内容必须从 y≥52 开始放**（`ui/navibar.html` 里写的规则，本轮才真正落实）。

### 分档（关键：区分"露残边"与"整块被盖"，以及"根本不画"）

| 档 | 条件 | 含义 | 处理 |
|---|---|---|---|
| `SLIVER` | 常显浮层只盖住**一部分**，且该控件**确实会画东西** | 被切成半截 ⇒ **肉眼可见的瑕疵** | **告警，必修** |
| `BURIED` | 整块被罩住 | 不是残影，但可能丢信息 | 人工判断 |
| `PAGE_BASE` / `NOSHOW` | 整屏窗口 / 透明背板 | 被盖的只是底色 / 根本不画像素 | 不报 |
| `PANEL_HIT` | 只与按需浮层（音量 OSD）相交 | 它的设计语义就是"弹出来盖住" | 只统计 |

## 2. 告警清单与处置（18 个 json 全扫）

### ① 必修：11 处标题在状态栏下沿露出 8px 残带 —— 已修

`Text*Title`，盒 `y=8 h=52`（8..60）、底色 `#1C1C1E`；navibar 只盖到 52
⇒ **露 52..60 的一条 8px 深灰横带**。

出现位置：`calc / pomodoro / stopwatch / timer / react / settings / remote(含学习页) /
main 的 WinClock·WinCalc·WinReact`（共 11 处，8 个 html）。

**修法**：`data-h` 52→44（盒 8..52，**整块收进 navibar 覆盖区**）。
标题本身已由 navibar 统一显示（`kTitleMap` 按 Activity 名映射），所以不是"藏起来"，
而是消掉重复的残影；**只改 html，零 logic 改动、零布局位移**。

**真机像素证据**（系统设置页，`/tmp/pg_autostart` 免触摸进入）：

```
改前  y= 48..50  #0B0B0D   ← navibar
      y= 52..59  #1C1C1E   ← ★ 残带
      y= 60..    #000000   ← 页面底色
改后  y= 48..50  #0B0B0D   ← navibar
      y= 52..59  #000000   ← ★ 残带消失
```

### ② 必修：投屏页状态字被整块盖死 —— 已修

`main/WinCast/TextCastMsg` 盒 `(12,12,456x36)` 全在 0..52 内 ⇒ 投屏的
"正在打开…/正在缓冲…"等状态字**永远看不到**。
**修法**：`data-y` 12→52（紧贴状态栏下沿，仍压在视频区上，不与底部黑条(700)/停止键(718)冲突）。

### ③ 页面专属名字被盖死、navibar 代不了 —— 已修（用户审批：给 navibar 加动态标题）

| 位置 | 被盖的控件 | navibar 原显示 | 现在 |
|---|---|---|---|
| 游戏页 | `TextGameTitle`（**当前游戏名**，`gHud.title`） | 只会显示"口袋游戏机" | navibar 显示游戏名 |
| 时钟套件·世界时钟子页 | `TextWorldTitle`（"世界时钟"） | 只会显示"时钟套件" | navibar 显示"世界时钟" |

为什么不能挪页面：游戏 HUD 下面紧接 540 高的画布；WinWorld 的 10 行城市从 y=60 起、
每行 64px 已铺到 y=700（下面还有提示行与返回键）。⇒ **没有第二个位置**。

**实现**：新增 `src/platform/PgNavi.h` / `PgNavi.cpp`：

```c
void pg::setNaviTitle(const char*);   // 传 0 = 恢复按 Activity 映射；幂等
const char* pg::naviTitleOverride();
void pg::setVideoPage(bool);          // 见 ④
bool pg::videoPage();
```

调用点（全部**成对**，且都在"唯一入口"上）：

- 游戏：`mainLogic.cc` 的 `onUI_Timer` 里与 `pg::setGameMode()` **同一处每帧同步**
  （赋值点十几处，逐点加必漏）+ `onUI_hide/onUI_quit` 兜底；
- 子页：`clocksuiteLogic.cc` 新增 `suiteSetWorldPage(bool)`，把 5 条切页路径
  （按钮 / 短按 108 / QA 两条）**全部收进这一个函数**。

### ④ 三个视频播放页的视频区被盖 52px —— 已修（用户审批：进播放页隐藏导航栏）

`Caster` / `IptvVideo` / `CamVideo` 都是 **480x700 @y=0**，且逻辑层按"**等比铺满**"
下发 `set_pos` ⇒ navibar 永久压掉画面顶部 **52/700 = 7.4%**（**真丢内容**，不是黑边）。

视频层是硬件 disp 层，**不能改坐标**（`docs/iptv.md`：`set_pos/set_crop` 与解码倍率耦合）
⇒ 走屏保那条老路：**播放期间把 navibar 收起来**，退出即恢复。

```c
// navibar.cc 的定时器：三条判断互斥且有顺序
if (videoPageActive())            hideNaviBar();      // 视频播放页
else if (isScreensaverOn())       hideNaviBar();      // 屏保
else if (!isNaviBarShow())        showNaviBar();      // 收敛重新显示
```

`videoPageActive()` 带**自愈兜底**：当前 Activity 不是 main/iptv/camera 就就地清标志 ——
宁可少藏一次，也不能让状态栏永远回不来。

调用点（每个页面都挂在**唯一入口**上）：`iptvShowPlayPage()/iptvBackToList()`、
`camShowViewWin(bool)`、`startCast()/stopCast()/showCastPageForStream()/hideCastPageForStream()`，
外加三个页面的 `onUI_hide/onUI_quit` 兜底。

### ⑤ 不报的（已定案，别再当新问题）

| 项 | 原因 |
|---|---|
| 各页整屏窗口 `Win*`（480x800） | 被盖的只是页底底色 |
| 各页 480x64 的 `Bar*Top` 顶栏背板 | 底色 `-1`（不画像素）⇒ 无残影（`check_overlay.py` 的 `NO_PAINT` 白名单）|
| 各页顶部 8px 强调色条 `Bar*` | 全在 0..8 被整块盖；其中 ToolPage 系已被 `ToolPage::init()` 显式隐藏（改版决定）|
| `TextCamTitle/TextRadioTitle/TextWifiTitle×2/TextSuiteTitle` | 与 navibar 标题**完全重复**，整块被盖、**无残影** |
| 音量 OSD（240x160 @120,520）压住的按钮 | **按需显隐**是它的设计语义（1.6s 自动收起），只统计不判错 |

## 3. 真机验收（全部免触摸：`/tmp/pg_autostart` + 各页自己的通道）

| # | 判据 | 结果 |
|---|---|---|
| A | 11 处残带消失 | ✅ 像素级：y=52..59 由 `#1C1C1E` → `#000000` |
| B | 游戏名进状态栏 | ✅ `导航栏: 标题 act=mainActivity -> 俄罗斯方块（页面覆盖）` |
| C | 世界时钟子页名进状态栏 + 返回恢复 | ✅ `-> 世界时钟（页面覆盖）` / 返回后 `-> 时钟套件` |
| D | 播放页隐藏导航栏 | ✅ IPTV 频道 48（426x240，正常出画）与摄像头查看页均 `show=0`；回列表 `show=1` |

### ⚠️ 验收中抓到的两个真问题（都已修）

1. **覆盖跨 Activity 不失效**（粘性状态经典死法）：
   进游戏后再进别的应用，状态栏标题**仍停在游戏名**。根因是覆盖由 mainLogic 的定时器
   维护、而它在切页的尾拍可能还会跑一次。**双重修法**：
   navibar 侧记"覆盖是哪个 Activity 设的"（`sOvrAct`，Activity 一变立刻作废）+
   mainLogic 侧只在 `currentAppName()=="mainActivity"` 时才写标志。
2. **进程重启不是本次改动引入**（对照实验 + dmesg 证据）：
   进 IPTV 播**频道 0（1280x720）**后 zkgui 被重启。取 `busybox dmesg`：

   ```
   VE: real freq=400000000                       ← 视频引擎升频（开始硬解）
   zkgui_ui invoked oom-killer: ... COMPACTION is disabled!!!
   Killed process 1334 (zkgui_ui)                ← pid 在变
   ```

   ⇒ 正是工程已记录的**在线流内存红线**（`docs/kb-v85x-online-streaming-limits.md`：
   720p 硬解必 OOM，本板 56MB）。**对照**：改播频道 48（**426x240**）时出画正常、
   进程稳定（pid 不变）、导航栏照常隐藏 ⇒ 与"隐藏导航栏"无关。

## 4. 防回归

- `tools/check_overlay.py`：独立入口，退出码 1 = 有 `SLIVER`（可进 CI）；
  `-v` 列全部（含 `PAGE_BASE/NOSHOW/PANEL_HIT`）。
- 已接进 `tools/gen_ui.py` 的检查链（`check_stretch` → `check_overlap` → `check_overlay`），
  每次改 UI 都会自动跑。
- 已知/已接受的项写在 `check_overlay.py` 的 `SETTLED` / `NO_PAINT` 里，带原因 ——
  下次跑出来不会当成新问题。

## 5. 顺带发现（**本轮未改**，属既有问题，请另行决定）

`tools/check_overlap.py` 报 **10 处高优先级**，都与 navibar 无关：

- **触摸遮挡 2 处（settings）**：`TextSetSoundVal` / `TextSetWifiVal` 声明在按钮**之后**
  ⇒ 盒压在 `BtnSetSound` / `BtnSetWifi` 上，按钮右半部分点不到（左侧仍可点）。
  修法：把这两个数值文本的盒缩到不压按钮，或改声明顺序。
- **视觉重叠 8 处**：`remote`（状态卡 × 标签/提示）、`settings`（数值文本 × 进度条）、
  `wifi`（顶栏背板 × 返回/刷新按钮、开关卡 × 列表标题）。
- **低优先级 1 处**：`navibar` 的 `ImgNbBattFill` × `ImgNbBattBolt` 部分重叠
  （电量条与充电闪电同区域 —— 看形态是**刻意的叠加**，但严格说闪电压住了电量条末端）。

## 6. 改动文件清单

| 文件 | 改动 |
|---|---|
| `ui/calc.html` `pomodoro.html` `stopwatch.html` `timer.html` `react.html` `settings.html` `remote.html`(2 处) `main.html`(4 处) | 标题盒 `data-h` 52→44；`main.html` 的 `TextCastMsg` `data-y` 12→52 |
| `ui/navibar.html` | 修正陈旧的"480x30/26"注释（实际 52）；补本轮检讨结论与"内容从 y≥52 放"规则 |
| `src/platform/PgNavi.h` / `PgNavi.cpp` | **新增**：标题覆盖 + 视频播放页两个全局粘性标志（带使用纪律注释）|
| `src/logic/navibar.cc` | 标题覆盖优先 + `sOvrAct` 过期自愈 + 视频页隐藏（含 Activity 白名单兜底）|
| `src/logic/mainLogic.cc` | 游戏名写进状态栏（前台判断）+ 投屏/整屏两处 `setVideoPage` + 三个生命周期兜底 |
| `src/logic/clocksuiteLogic.cc` | 新增 `suiteSetWorldPage()`，5 条切页路径统一走它 + 生命周期兜底 |
| `src/logic/iptvLogic.cc` / `cameraLogic.cc` | 播放页进/出 `setVideoPage` + 生命周期兜底 |
| `tools/check_overlay.py` | **新增**：浮层覆盖检讨工具 |
| `tools/gen_ui.py` | 接入 `check_overlay` 到检查链 |

> 回滚备份：`.bak_ov20260916/`（8 个 html + 5 个 .cc）。本工程**没有 git**。
