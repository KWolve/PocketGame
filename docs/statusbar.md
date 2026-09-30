# 全局状态栏 / 全局音量 OSD

> 需求原文（2026-09-14）：「音量弹框做成全局的，可以用状态栏实现。现在的音量弹框在其他界面看不到。」
> **2026-09-15 改版（v1.17.1）**：显隐从"常显"改成**按需显隐** —— 修掉"整机所有页面点不动"的血案（见 §4）。
> **2026-09-16 第二轮改版**：「音量弹出框效果参考手机修改，现在的显示效果太差了。可以做成半透的加进度条」
> ⇒ **半透明圆角面板 + 手机风格细进度条**，见 §2.1（含"填充图是缩放不是裁剪""圆钮为什么不能加"的实测结论）。

## 1. 为什么必须是状态栏

改造前：音量 OSD 是**主界面私有窗口**（`main.ftu` 的 `WinVolume`），WiFi 页嫌看不见**又抄了一份**，
而工具页 / 时钟套件 / IPTV / 投屏 上按音量键 —— **只有声音、没有界面**（用户就是这么发现的）。

状态栏正好是官方给的"全局浮层"：它是 `APP_TYPE_SYS_STATUSBAR` 的 **SysApp**，悬浮在所有 Activity 之上，
所以任何 ftu 里按音量键都能弹同一个面板。

```
任意页面按音量键 / QA
   └─ pg::volumeStepGlobal(±1)                     ← 全工程音量的唯一收口点
        ├─ （mainLogic 注入的钩子）真改音量
        └─ PgAudio 广播 setVolumeNotifyHook(fn)
             └─ src/logic/statusbar.cc：showOsd() 弹面板 / 1600ms 后（或点面板外）收起
```

## 2. 组成

| 文件 | 作用 |
|---|---|
| `ui/statusbar.html` | 布局：root（**不写 data-bg** ⇒ 透明）+ 一个 `WinSbVolume` 面板（**240x112 @120,544**） |
| `src/logic/statusbar.cc` | 逻辑：显隐（走框架接口）、音量广播、点面板外收起、QA 通道 |
| `tools/gen_ui.py` | `UI_SOURCES` 加 `statusbar.html`；`HIDDEN_CONTROLS` 加 `WinSbVolume`；<br>**patch 根节点 position = 面板位置**（与 statusbar.cc 的 `kPanelWinLeft/Top` 必须一致）；<br>`gen_volume_bar_assets()`（进度条图）+ `gen_osd_panel()`（半透明面板底） |
| `.fun/v85x/generated/ui_statusbar.cpp` | **fun 自动生成**：`REGISTER_SYSAPP(APP_TYPE_SYS_STATUSBAR, statusbar)` —— 不用手写注册 |
| `src/platform/PgAudio.*` | `setVolumeNotifyHook(fn)` + `volumeStepGlobal()` 成功改音量后广播；<br>★ 2026-09-17 加 `setVolumeSetHook(fn)` / `setVolumePercentGlobal(pct)`（**绝对设值**，给设置页拖动）与 `suppressVolumeOsd(ms)`（拖动时压掉 OSD） |

主界面与 WiFi 页各自的 `WinVolume` **已删除**（否则会同时弹两个面板）。

## 2.0 ★★ 2026-09-17：进度条左侧的**音量图标 = 静音开关**

用户原话：「statusbar 里面的进度条左侧放一个音量图标，点一下图标变成静音的样子，
再点一下解除静音（= 现在的音效开关），调整音量自动解除静音」；并明确
「**音效开关就是等于静音按键了，就不存在音效开关的说法**」。

### 面板布局（240x112）

```
┌──────────────────────────────────────────┐
│ 音量                                70% │  ← TextSbVolTitle / TextSbVolPct
│ [🔊]  [==================----------]     │  ← BtnSbVolIcon(+Mute) / BarSbVol
│ 点图标静音 · 音量键调节（自动解除静音）   │  ← TextSbVolHint
└──────────────────────────────────────────┘
  16      60                       224
```
- 图标按钮 `BtnSbVolIcon` / `BtnSbVolIconMute`：**32x32 @ (16,46)**（与条同一行、条左侧）；
- 进度条 `BarSbVol`：**164x10 @ (60,57)**（原来 200 宽，让位给图标 ⇒ 素材也跟着换，
  见 `tools/gen_ui.py` 的 `VOL_BAR_VARIANTS` 第一项从 `(200,…)` 改成 `(164,…)`）。

### 三个实现要点

1. **两态图标 = 两个按钮叠在同一格、按状态显示其一**（不是"一个按钮换图"）
   `platform/PgSkin.h` 实测记着 —— **运行时 `setBackgroundPic()` 这条路径不保留 alpha**
   （透明像素被渲染成纯白）。图标是透明底线框 PNG，用那条路换图会得到一整块白方块。
   ⇒ 照主界面应用网格图标的老办法（`mainLogic.cc` 的 `syncRowIcon`）：两个都建好，只显示一个。
   ★ 切换时**必须同时 `setTouchable()`**：两个控件同位置，藏起来那枚若还 touchable=true
     会继续吃掉这一格上的点击。
   ★ 两个都写进 `gen_ui.py` 的 `HIDDEN_CONTROLS`（否则开机第一帧两枚图标叠着）。

2. **图标由转换器按 `data-icon` 自动生成**（`icon_<名>_<宽>x<高>_<色>.png` + `_p` 按下态）；
   可用名见 `ui_tools/gen_res.py` 的 `_GLYPHS`（40 枚，含 `volume` / `mute`）。
   ★ **静音态用琥珀色 `#FF9F0A`**（有声态白 `#F2F2F7`）：mute 图标的"叉"是 1px 细线，
     32px 下真机渲染只有半亮灰（实测：喇叭体是 `#`、叉是 `+`），不加颜色不好分辨；
     琥珀与状态栏"充电中"那一档同色，视觉上是一套。

3. **"调整音量自动解除静音"落在 pg 层，两条路都要管**（`src/platform/PgAudio.cpp`）：
   - 按键 → `volumeStepGlobal()`（本来就有：音量+ 时先取消静音）；
   - 拖动/绝对设值 → `setVolumePercentGlobal()`（2026-09-17 补：`pct > 0` 时先解除，
     "拖到 0"不算想听）。
   ★ 设置页那一行也换成同一个开关（`pg::setMutedGlobal`），两处状态天然一致。

## 2.1 ★★ 2026-09-16 第二轮：半透明 + 手机风格进度条

用户原话：「音量弹出框效果参考手机修改，现在的显示效果太差了。可以做成半透的加进度条」。

### 旧版差在哪（真机像素实测，不是感觉）
| 项 | 旧版 | 现在 |
|---|---|---|
| 面板底 | **不透明** `#1C1C1E`，和背后页面糊成一块 | **半透明黑 72%** + 圆角 22（`osd_panel.png`） |
| 面板尺寸 | 240x160（偏大、信息挤） | **240x112**（紧凑，中心仍在 y≈600） |
| 进度条 | 20px 粗、**四角直角**、两个纯色方块 | **10px 细条 + 胶囊圆角 + 半透明白轨道** |
| 填充 | 竖向渐变蓝块 | 实色胶囊（OSD 白 / 设置页 `#64D2FF`） |

### 三个实现要点（各有实测依据）

1. **面板底用 `button + picTab`，不是 `data-bg`**
   MCP `devflow/pixel-analysis-ai.md` 的结论：**真透明装饰件用 button + picTab（alpha 混合正确）**。
   而且 JSON 里的颜色走 `setBackgroundColor(int)` 链路，alpha（0xAARRGGBB）在 JSON 解析层**不可控**
   （SDK 头文件确实是 `uint32_t` + 0xARGB，但那个十进制数要 > 2^31 才带得进 A 通道）。
   ⇒ 面板底 = `BtnSbPanelBg`（240x112，`picTab.pic0 = osd_panel.png`，`data-untouchable="1"`），
     **第一个定义**（= 最底层）。
   ⚠️ **它故意不加 `setTouchPass(true)`**（与"装饰件要放行"的通例相反）：面板必须**吸收**落在它身上的
     触摸，否则点面板空白处会穿透到下面页面（比如直接把游戏卡片点了）。

2. **进度条只用 track + fill 两张图，⛔ 不要 `data-thumb`**
   实测结论（下面那条"填充图是缩放不是裁剪"是关键）：
   * ZKSeekBar 的 `progressPic` 是**把整张图按进度缩放**（不是裁剪）⇒ **胶囊填充图缩放后两端天然是圆的**，
     观感已经对，不需要圆钮；
   * 加了 `data-thumb` 之后圆钮被画在"进度位置"上 ⇒ **0% 时也留一个 10px 白点**（实测：vol=0 填充宽 10px、期望 0）；
   * 而且 html2json 见到 `data-thumb` 会**自动置 `touchable=true`**（"可拖滑块"），与"条只负责显示"冲突。
   ⇒ 去掉圆钮后实测：**填充宽与进度严格 1:1**（0%→0px、6%→12px、42%→84px、100%→200px）。

3. **`data-untouchable="1"` 把条设为只显示**
   改音量走音量键 / 面板上的静音按钮；条可拖但 `onProgressChanged_BarSbVol` 没落地音量的话，
   会出现"看着能拖、音量不变"的假交互。要真做成可拖，得在 statusbarLogic 里落地。

### 半透明是怎么验的（定量，别只看"看着像"）
同一页面抓两帧（OSD 关 / 开），逐像素算 `开/关` 亮度比：
```
面板内 652 个采样点（背景亮度 > 150）的 B/A 中位数 = 0.279
⇒ 实测面板 alpha = 1 - 0.279 ≈ 72%   （设计值 184/255 = 72.2% ✓）
   例：背景 #A0A0AA(160,160,170) → 面板上 #2D2D2F(45,45,47)，160×0.279 ≈ 45 ✓
四角 (122,546)/(358,546)/(122,654)/(358,654) 两帧完全相同 ⇒ 圆角外的页面未被覆盖 ✓
```
> 面板透明度是 `tools/gen_ui.py` 的 `OSD_PANEL_BG_RGBA`（现 184）。想更实/更透改这一个数即可
> ——⚠️ 无模糊可用，太透会让小字压在花背景上难读。

## 3. ★★ 显隐策略：**按需显隐**（2026-09-15 定稿）

**显隐 = 整个状态栏的显隐**，用框架接口（`src/logic/statusbar.cc`）：

```cpp
void showOsd(int pct, int holdMs) {          // 音量变化 / QA vol
  updatePanel(pct);
  sWantOsd = true;                           // ⚠️ 必须先置位（onUI_show 靠它判断）
  EASYUICONTEXT->showStatusBar();            // ① 整屏浮层显示
  if (mWinSbVolumePtr) mWinSbVolumePtr->showWnd();   // ② 面板显示
  sHideMs = nowMs() + (holdMs > 0 ? holdMs : 1600);
}

void hideOsd(const char *why) {              // 三条路都走它
  sWantOsd = false;
  if (mWinSbVolumePtr) mWinSbVolumePtr->hideWnd();
  EASYUICONTEXT->hideStatusBar();            // ★ 收起整屏浮层 ⇒ 从此不再挡任何触摸
  sHideMs = 0;
}
```

三个收起触发点：**① 1.6s 超时**（`onUI_Timer`）**② 点面板外**（`onstatusbarActivityTouchEvent`，
用户要求 2026-09-15）**③ QA `hide`**。

**为什么必须"平时不显示"**：见 §4 —— 整屏 SysApp 浮层只要显示着，就吃掉全系统的控件级触摸。

**一个必须知道的时序**：状态栏被框架 `loadStatusBar()` 加载后**默认就是"显示"**状态，
而 `onUI_init()` 太早（框架的显示标记还在后面）、`onUI_show()` **实测根本不被回调**
⇒ 只能靠 `onUI_Timer` 里每 100ms 收敛一次：

```cpp
if (!sWantOsd && EASYUICONTEXT->isStatusBarShow()) {
  LOGD("状态栏: 收敛收起（当前无音量 OSD 要显示）");
  EASYUICONTEXT->hideStatusBar();
}
```

## 4. ★★ 血案：整屏 SysApp 浮层会吃掉**全系统**的控件级触摸

**症状**（用户报障）：「主界面下没有触摸了」——
**报点正常**（日志里有触摸坐标）、屏保退出正常，但**所有页面的控件都点不动**。

**三段实测证据**（注入 + 抓帧/日志，可复现）：

| 条件 | 结果 |
|---|---|
| 状态栏**显示** | `PocketGame touch: action=1 x=72 y=174`（**页面级**回调有坐标）<br>但**控件级**零输出：无 `list click`、无按下态 |
| `hideStatusBar()`（`isStatusBarShow()=0`） | 立刻 `PocketGame: list click index=0 subId=24020` ✅ |
| 把 `statusbar.ftu` 从设备移走（框架加载不到） | 同样立刻恢复 ✅（对照实验） |

**关键结论（都实测过，别再试）**：

1. **`touchable: false` 不是触摸穿透** —— 整屏 topmost 窗口照样挡住下层，下层连 `DOWN` 都收不到。
2. **运行时的 `setTouchPass(true)` 也放不过去** —— 给根窗口 `setTouchable(false) + setTouchPass(true)`，
   仍然只有页面级回调。
3. **`hideStatusBar()` 之后状态栏页完全活着**：定时器还在跑、QA 通道照常响应、音量广播回调照常触发
   ⇒ "按需显隐"闭环成立，不需要每次重建页面。
4. 症状的迷惑性在于：**页面级触摸回调是全局注册的、不受窗口命中影响** ——
   所以"有报点、没反应"看着像业务代码的 bug，其实是被浮层吃了。

**正确做法**：`hideStatusBar()` 常隐 + 音量变化才 `showStatusBar()`（§3）。
**不要做**：把 `showStatusBar()` 放进主循环"收敛常显"——那正是元凶（旧版就是这么写的）。

> ⚠️ **SysApp 页收不到全局触摸**：`EventApp<BaseApp>`（SysApp）与普通 Activity 不同，
> 实测它的 `onstatusbarActivityTouchEvent` **一条都不触发**（普通 Activity 的同类回调照常有坐标）。
> 而"点面板外收起"需要它 ⇒ 在 `onUI_init()` 里**显式注册**：
> ```cpp
> EASYUICONTEXT->registerGlobalTouchListener(mstatusbarPtr);   // 生成的代码只在 DESTROY 时 unregister
> ```

## 5. 根节点属性（gen_ui 会 patch，别手动去掉）

| 属性 | 值 | 为什么 |
|---|---|---|
| `data-bg` | **不写** | html2json：「不写则透明」——官方就是给 navibar/statusbar 留的；写了底色整屏被挡 |
| `touchable` | `false` | 语义上"不参与交互"。**注意它不等于穿透**（§4），穿透靠"平时不显示" |
| `topmost` / `modal` | `true` / `false` | 在最顶层；但**不能是模态**（模态是输入黑洞，工程历史坑） |

## 6. 四个坑（实测踩到）

1. **逻辑文件名必须是 `statusbar.cc`，不是 `statusbarLogic.cc`**。fun 对 sysapp 页
   （`screensaver`/`navibar`/`statusbar`）约定的逻辑文件是 `<页名>.cc`；放错会让 fun 再生成一份模板，
   **两份都定义 `onUI_init`**（`static`）⇒ 谁生效由注册顺序决定，实测**模板那份赢了**。
2. **`showStatusBar()` 不能在 `Main.cpp::onEasyUIInit()` 里调**：那时 SysApp 工厂还没就绪，调了不生效
   （日志里连"状态栏就绪"都没有）。**也不要用主循环常显**（§4）。
3. **面板显隐的收敛不要只靠事件**：音量回调可能来自非 UI 线程 ⇒ 回调里**只记一个 `volatile int`**，
   控件更新 / 弹面板都在**定时器**里做（工程铁律：控件操作只能在主线程）。
4. **`BarSbVol` 拖动回调必须有定义**：面板显示时状态栏会吃触摸，这个回调实际不会被触发，
   但 fun 按控件生成了绑定 ⇒ 不写就是链接期 `undefined reference to onProgressChanged_BarSbVol`。

## 7. QA（`/tmp/pg_statusbarcmd`，独立 SysApp 必须有自己的通道）

| 命令 | 作用 |
|---|---|
| `vol <0-100> [holdMs]` | 弹出面板（`holdMs` 用于抓帧/交互验收，默认 1600ms） |
| `hide` | 立刻收起 |
| `dump` | 打印 `面板=显示/隐藏 值= 剩余=ms 状态栏=显示/隐藏` |

> 主界面 QA `vol <n>`（`/tmp/pg_autostart`）仍可用，但它的 **hold 参数会被忽略**
> （代码里写死了 0）——要在面板停留期间做交互验收，请用上面状态栏这条。

## 8. 真机验收（`v1.17.1`，注入 + 抓帧，全部可复现）

| 场景 | 判据 | 实测 |
|---|---|---|
| 启动即隐藏 | QA `dump` | `面板=隐藏 ... 状态栏=隐藏` ✅ |
| **控件级触摸可用** | 注入点图标 `pginj tap /dev/input/event4 72 174` | `PocketGame: list click index=0 subId=24020` + `enter game[0] 2048` ✅ |
| 跨页面 | 注入 wifi 页返回键 (30,32) | `wifiLogic: onUI_quit` ✅ |
| 音量弹面板 | QA `vol 62 8000` + 抓帧 | 面板底 `(150,545)=#1C1C1E`、音量条 `(240,600)=#57BAF1` ✅ |
| **点面板外收起** | 面板显示中点 (240,200) | `状态栏: 触摸面板外 (240,200) → 收起` → `dump: 面板=隐藏 状态栏=隐藏` ✅ |
| 点面板内不收起 | 面板显示中点 (240,600) | 无收起日志、`dump: 面板=显示` ✅ |
| 1.6s 自动收起 | 日志 | `状态栏: 收起音量 OSD（1.6s 超时）` ✅ |
| 证据图 | 文件 | `docs/shot_statusbar_osd.png`（面板显示）、`docs/shot_statusbar_vol.png` |

### 8.2 图标=静音开关的验收（2026-09-17，固化版）

```
布局： 抓到面板字符画 —— 图标（面板内 x=24..42）在条（x=60..224）左侧 ✅
      条填充与音量严格对应（69% → 113px ≈ 164×0.69）✅
点一次：状态栏: 点音量图标 -> 静音（成功）
       PgAudio: 静音 -> 开（输出开关 2 个，成功 2 个）
       像素判据：（固化版）点之前 白像素=100 / 琥珀=0 → 点之后 白=0 / 琥珀=116 ✅
再点： 状态栏: 点静音图标 -> 解除静音（成功）✅
调音量自动解除：mute 1 后再拖动设置页音量条 →
       PgAudio: 调整音量 90% -> 自动解除静音 ；QA dump muted=0 ✅
设置页：点「静音」行 → settings: 静音 -> 开（成功）；QA dump muted=1 ✅
```

> ⚠️ **抓图时机**：面板会 **1.6s 自动收起**，点完图标要**立刻**抓（`pginj tap` 之后直接 grab，
> 中间别 sleep）。我第一轮就是 sleep 2 后抓，抓到的是面板底下的页面内容，白折腾一次。

### 8.1 第二轮改版的验收（2026-09-16，面板 240x112 半透明版）

| 场景 | 判据（全部真机） | 实测 |
|---|---|---|
| **半透明** | 同页抓 OSD 关/开两帧，逐像素算 `开/关` 亮度比 | 面板内 652 点比值中位数 **0.279 ⇒ alpha ≈ 72%**（设计 72.2%）✅ |
| **圆角** | 面板四角两帧应完全相同（未被覆盖） | 四角全部相同 ✅ |
| **细条比例** | 填充宽 vs 进度 | 0%→0px / 6%→12px / 42%→84px / 100%→200px，**严格 1:1** ✅ |
| **0% 不留白点** | 同上（加过圆钮时是 10px） | 去掉 `data-thumb` 后 0% = 0 ✅ |
| **静音按钮可点** | `pginj tap` 面板内 (170,630) | `状态栏: 点静音按钮 -> 静音（成功）` + `PgAudio: 静音 -> 开` ✅ |
| **点面板外收起** | 面板显示中点 (60,300) | `状态栏: 触摸面板外 (60,300) → 收起` ✅（窗口位置改成 544 后仍正确） |
| 1.6s 自动收起 | 日志 | `收起音量 OSD（1.6s 超时）` ✅ |

> **注入是可靠的**：`pginj tap /dev/input/event4 <x> <y>` 能驱动框架**控件级**点击
> （`list click` / `tab click` 都会打日志）。
> 旧文档写的"注入时灵时不灵、点击类验收要多试几次"是**误判** ——
> 真正原因是当时状态栏常显、把控件级触摸全吃了（§4）。
> 判据很硬：**控件级回调没日志 = 有东西在挡，先查有没有整屏浮层正显示着**。

## 9. 与屏保的关系

屏保是另一个整屏 SysApp，会盖住状态栏 ⇒ 屏保时看不到音量面板。
但屏保里按任意键/触摸就退出（见 `docs/screensaver.md`），退出后状态栏照常按需弹出。
