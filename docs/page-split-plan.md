# 主界面拆分方案（main.ftu → 每个应用一页）

> 需求（2026-09-14）：「主界面内容太多，全屏的窗口根据功能拆分成处理的 ftu 页面」+
> 追问确认粒度 = **每个应用一页**。
>
> 本文是**可执行的分步方案**（含每个应用要搬的控件/代码清单、风险点、验收判据），
> 不是一次性改完的记录 —— 这是一次结构改造，必须按切片做、每片都验机。
>
> 依据：MCP《页面架构规范》（`knowledge/devflow/page-architecture-spec.md`）——
> 「跨业务域 / 需独立生命周期或返回栈 / 大页面 → 独立 ftu（openActivity）；
>  同一业务域内的页签·二级页·弹窗 → 同 ftu 内多个整屏 window + showWnd/hideWnd」。

## 1. 现状

`ui/main.html`（456 行）→ `ui/main.json`（**4461 行**）一个 ftu 里塞了 **11 个整屏窗口**：

| 窗口 | 属于哪些应用（`kAppTable` slot） |
|---|---|
| `WinGame` + `WinPause` | 12 个画布游戏（0-7、14-17） |
| `WinClock` | 番茄钟 8 / 定时器 9 / 秒表 10 |
| `WinCalc` | 计算器 11 |
| `WinReact` | 反应计时 18 |
| `WinClockSuite` + `WinWorld` + `WinAlarmRing` | 时钟套件 19 |
| `WinIptv` + `WinIptvPlay` | 网络电视 20 |
| `WinCast` | DLNA 投屏（非应用，由控制器 URL 触发） |
| `WinVolume` | 全局音量 OSD（**建议留在 main**，它不是应用页） |

`src/logic/mainLogic.cc` 4765 行里，与这些窗口相关的部分约占 1300~1600 行。

## 2. 目标页面清单

`ftu = Activity = 独立编译单元`；`src/logic/<页名>Logic.cc` 由 fun 自动绑定到生成的 `ui_<页名>.h`。

| 新 ftu | 承载 | slot | 备注 |
|---|---|---|---|
| `game.ftu` | 画布游戏页 + 暂停弹窗 | 0-7,14-17 | **一个页面服务 12 个游戏**（用 Intent 传 slot）。若确实要"一个游戏一页"，见 §5 的成本说明 |
| `pomodoro.ftu` | 番茄钟 | 8 | 每应用独立 |
| `timer.ftu` | 定时器 | 9 | |
| `stopwatch.ftu` | 秒表 | 10 | |
| `calc.ftu` | 计算器 | 11 | 20 键 |
| `react.ftu` | 反应计时 | 18 | 1 个大按钮 |
| `clocksuite.ftu` | 时钟套件（**世界钟 / 响铃 作为同 ftu 内 window** —— 它们是同域二级页） | 19 | |
| `iptv.ftu` | 网络电视（**选台 / 播放 作为同 ftu 内 window**） | 20 | |
| `cast.ftu` | 投屏播放页 | — | 由 `startCast()` 打开 |
| `main.ftu` | 只剩：主菜单列表 + 分类 tab + 音量 OSD + 电池图标 | — | |

已独立的无需再动：`wifi.ftu`、`remote.ftu`、`ime.ftu`、`screensaver.ftu`。

## 3. 每个应用页的标准做法（机械步骤）

以 `stopwatch.ftu` 为例：

1. **建布局**：`ui/stopwatch.html` ← 从 `ui/main.html` 的 `WinClock` 块整段搬出来，
   `div.modal` 改成根 `div.screen`（内容不变，但**去掉 `data-caption="WinClock"` 这一层**，
   控件 caption 保持 `TextClockMain`/`BtnU0..U7` 不变 → 逻辑搬运时改名最少）。
   版式按 `docs/ui-design-baseline.md` 重新排（本次已把 WinClock 的按钮沉底，直接沿用）。
2. **加进生成清单**：`tools/gen_ui.py` 的 `UI_SOURCES` 追加 `"stopwatch.html"`
   （`HIDDEN_PAGE_WINDOWS` 里去掉 `WinClock`）。
3. **生成**：`python tools/gen_ui.py` → `ui/stopwatch.json` / `.ftu`。
4. **fun 生成骨架**：`fun build`（会产出 `.fun/v85x/generated/ui_stopwatch.{h,cpp}` 与
   `src/logic/stopwatchLogic.cc` 脚手架）。
5. **搬逻辑**：把 `mainLogic.cc` 里该应用的那部分搬进 `stopwatchLogic.cc`：
   - `PgTools.cpp` 的 `ToolStopwatch` 类**不动**（它已经是一个干净的 Game 子类，
     对外只有 `uiText/uiButton/uiButtonStyle/onUiButton/uiAccent/hint/keyBar`）；
   - 新建实例 + `bind(host, slot)` + `reset()` + `setState(GSTATE_RUNNING)`；
   - 每帧（`onUI_Timer`）：`update(dt)` → 用 `uiText(slot)`/`uiButton(i)` 同步控件
     （**沿用"只在变化时 setText"的缓存写法**，见 `syncToolUi`）；
   - `onButtonClick_BtnU*` → `onUiButton(i)`；
   - `onKeyEvent`：108 = 开始/暂停、长按 ≥700ms = 返回主界面；
   - 屏保策略：进入即 `setScreensaverEnable(false)`，退出恢复（可抽成公用小函数）。
6. **主界面改跳转**：`mainLogic.cc::startGame()` 里那串 `if (strcmp(id,"wifi")==0) openActivity(...)`
   追加该应用（`kAppTable` 的 slot **必须保留**，它是存档索引）。
7. **QA 通道**：新 ftu 用**自己的**命令文件（如 `/tmp/pg_stopwatchcmd`）——
   后台 Activity 的定时器不跑，`/tmp/pg_autostart` 在这些页面上是失效的
   （这是 wifi/remote 已经踩过并成文的约定）。
8. **字库**：新页面若引入新中文 → 重跑 `tools/gen_font.py`（用带 fontTools 的解释器：
   `C:/Users/Admin/.workbuddy/binaries/python/envs/default/Scripts/python.exe`），
   否则中文**整个消失**（不是方框）。
9. **验机**（一条都不能省）：
   - `python tools/gen_ui.py` 无报错、`fun build` 通过；
   - 真机：点卡片进页 → `enter tool[N] xxx` 日志；按键/触摸可用；长按返回列表；
   - 屏保：进页后停 40s，`logcat -d | grep -c performScreensaverOn` = **0**；
   - 返回后主列表仍停在原分类。

## 4. 风险评估（为什么必须切片做）

| 风险 | 说明 | 对策 |
|---|---|---|
| **画布生命周期** | `WinGame` 依赖 `DeviceDisplay::attach/detach(mGameCanvasPtr)`；换 Activity 后控件指针换了，**attach 必须先于首帧渲染**，detach 必须在窗口隐藏之后（顺序错了就是 use-after-free，见 `exitGameToMenu` 注释） | game.ftu 单独做、单独验 |
| **视频层 / 解码通道** | `WinIptvPlay`/`WinCast` 是 `videoview` 透明窗口，退出必须 `StreamPlayer::stop() + Hls::stop()`（本板 55MB 内存，不停流下个应用必炸） | iptv/cast 放最后做 |
| **按键路由** | 现在 108 键/长按返回的逻辑集中在 `mainLogic` 的 KeyRouter + `gMode`；拆开后每个 Activity 各自注册 `IKeyListener` | 抽公用 `imgKeyRouter` 小函数，别各写一份。⚠️ **长按判定要放定时器轮询**（无 autorepeat，只在 UP 判会"按满还得等松手"）——见 `docs/ui-design-baseline.md` 自测清单 |
| **屏保策略** | 现在靠主循环按 `gMode` 收敛；拆开后每页自己管 | 同上，抽公用 |
| **存档索引** | `kAppTable` 的 slot 是存档（最高分、分类）的索引，**不能因为拆页而重排** | 只加跳转，不动表 |
| **QA 通道** | 后台 Activity 的定时器不跑；`/tmp/pg_autostart` 在独立页失效 | 每页自己的 cmd 文件 |

## 5. 关于"一个游戏一页"

`WinGame` 是**同一个画布页服务 12 个游戏**（游戏差异全在 `src/core/Pg*.cpp` 的 Game 子类里）。
若要 12 个游戏各自一个 ftu，按 §3 的步骤复制 12 份 HUD/画布/暂停弹窗布局与逻辑，
**每份逻辑都是同一套**（attach 画布 → 同步 HUD → 转发按键），属于纯重复。

建议：**game.ftu 一个页面 + Intent 传 slot**（MCP 规范里的"大页面"处理），
如果确实要拆，先把共用部分抽成 `src/core/GamePage.h`（一个类管 attach/HUD/按键/暂停），
那样每个游戏的 `xxxLogic.cc` 只需 3~5 行 —— **这是"一游戏一页"能接受的最低成本形态**。

## 6. 建议的切片顺序（每片独立可交付、可回退）

1. `stopwatch.ftu`（最自包含：3 个按钮、无网络、无画布）——**用最小切片跑通整条链路**
2. `calc.ftu` + `react.ftu`（原生控件，逻辑量小）
3. `pomodoro.ftu` + `timer.ftu`
4. `clocksuite.ftu`（含世界钟/响铃 window）
5. `game.ftu`（画布生命周期，单独验）
6. `iptv.ftu` + `cast.ftu`（视频层/内存，最后做）
7. 收尾：`main.json` 从 4461 行降到 ~600 行；README / 本文更新

## 7. 已具备的"地基"（本次已完成，拆分时可直接用）

- 原生工具页的**版式**已经改好（按钮沉底、间距只用 6/8/16、大钟 88px 为唯一主角）——
  拆页时直接把 `WinClock` 那一块搬进新 html 即可，见 `ui/main.html` 的注释。
- 计算器配色已拉到设计基线（`#2A3B4F` 功能键 / `#26313F` 数字 / `#1E2A38` 运算符 /
  `#2E7D5B` 等号）。
- 独立 ftu 的三件套模板已经在跑（`wifi.ftu` + `wifiLogic.cc` 的 QA 通道 + 长按返回），
  照它抄即可。

---

## 8. 口径调整 + 进度（2026-09-14 下午，用户确认）

> 用户定：「**游戏全部还是不动，剩余的功能界面全部独立出来**」。

**改了什么**：`game.ftu` 从计划里去掉 —— 12 个画布游戏继续留在 `main.ftu`。
好处不只是省事：**画布 attach/detach 生命周期**（本计划里风险最高的一项）整个消失了。

### 8.1 已完成的 5 页（真机验收过）

| ftu | slot | 按钮 | 验收判据（全部实测通过） |
|---|---|---|---|
| `stopwatch.ftu` | 10 | 8 | `btn 0` → `主='00:02.55' 状态='计时中' 按钮=[暂停\|计次\|清零]`；3s 后 `00:06.05`（计时准）；计次 → `副='计次 1 · 最近 00:15.85'`；短按 108 → `已暂停`；长按 801ms → 返回列表；**40 秒内进屏保 0 次** |
| `pomodoro.ftu` | 8 | 8 | `主='24:57' 状态='专注中' 副='工作 25 分 · 休息 5 分 · 已完成 0 个'` |
| `timer.ftu` | 9 | 8 | `主='04:57' 状态='倒计时中' 副='设定 05:00'` |
| `calc.ftu` | 11 | 20 | `btn 5/15/6/19` → `主='17' 状态='8 + 9 ='`（这是 8+9，不是 7+3——按键是行优先排的）；20 个 `id=` 与 `ID_CALC_BtnK*` 全部对上 |
| `react.ftu` | 18 | 1 | `btn 0` → `主='点！' 状态='就是现在！' 按钮0='点这里！'`（阶段换色/换字正常） |

页面版式统一按 `docs/ui-design-baseline.md`：顶栏 0..60 + 色条 0..8、大数值居中为唯一主角、
**按钮沉底**（2 行 x 4 列 108x80，行距 16/列距 8）、底部键位条 700..800。
像素验收（秒表页）：大钟墨迹中心 **239.0**、状态行 238.5、按钮第 1 行 y=512..591、第 2 行**无墨迹**（只显示 3 个按钮）✓

### 8.2 关键：`src/ui/ToolPage.*`（8 个工具页共用一份外壳）

拆出来之后每页都要同一套样板，逐页复制 8 份必然各自漂，所以抽成 `pg::ToolPage`：

- `init(binds)` 建 Game、绑宿主、清缓存；`tick(dt)` 每帧 update + 同步控件（**变化检测**）；
- `button(i)` 转发按钮；`keyDown/keyUp` 管 108 短按=开始/暂停、**长按 ≥700ms=返回列表**、
  103/105=音量±（走 `pg::volumeStepGlobal`，与主界面同一条路）；
- `show()/hide()` 管**工作界面禁屏保**。

于是每页的 `xxxLogic.cc` 只剩 ~100 行机械转发（秒表/番茄钟/定时器三页是从同一份派生的）。

### 8.3 这次踩到的 4 个坑（都是我第一版没写对、编译/运行才暴露的）

| 坑 | 现象 | 正解 |
|---|---|---|
| **`pgHost()` 写在匿名命名空间里** | 链接期 `undefined reference to pg::pgHost()` | 必须放在匿名命名空间**之外**；而且要用 `namespace pg { Host *pgHost() {...} }` 包起来 —— 写 `pg::Host *pgHost()` 定义的是**全局** `::pgHost`（限定名在那只是返回类型），签名对不上照样链接失败 |
| `EASYUICONTEXT->unregisterTimer()` | 编译报 no member | 别自己反注册，定时器随页面销毁 |
| 页面 caption 少一个文本槽 | 编译报 `mTextCalcPhasePtr was not declared` | 外壳固定绑 6 个文本槽（title/main/phase/sub/hint/keyBar）→ **新页必须把 6 个 caption 都写上**（用不到的留空块即可） |
| 批量派生页面时先替换小写再替换大写 | 生成出 `ID_STOPWATCH_BtnPm0` 这种宏 | 派生前把 `stopwatch`/`STOPWATCH` 两种大小写一起换 |

### 8.4 还剩 3 页（都是"难"的那一类，按风险从低到高）

| 待拆 | 为什么放后面 |
|---|---|
| `clocksuite.ftu`（slot 19） | 控件结构特殊（5 行闹钟 x 4 控件 + 编辑区 + 世界钟页 + 响铃页），要搬 `syncClockSuite/syncWorldClock/tickAlarmRing`；闹钟守护线程与页面无关（`pg::Alarm`），搬的时候别把守护线程也带走 |
| `iptv.ftu`（slot 20） | 选台页 + 播放页（`videoview` 透明窗口）+ HLS 中继 + 硬解通道；**退出必须停流**（55MB 内存，不停流下个应用必炸） |
| `cast.ftu` | 投屏页由 DLNA 控制器 URL 触发（不是应用卡），生命周期与 DLNA 会话绑定；`StreamPlayer/Hls` 的停止路径要与页面销毁对齐 |

> 每页的验收清单见 §3 第 9 条；**独立页必须有自己的 QA 通道**
> （`/tmp/pg_<页名>cmd` —— 后台 Activity 的定时器不跑，`/tmp/pg_autostart` 在独立页上是失效的）。

---

## 9. 进度续：`clocksuite.ftu` 已完成（2026-09-14）

口径不变（游戏不动，其余功能界面全部独立）。已完成的第 6 页：

| ftu | slot | 内容 | 真机验收 |
|---|---|---|---|
| `clocksuite.ftu` | 19 | 套件主页（大钟/日期/5 行闹钟/编辑区）+ 世界钟页 | 点卡片 → `独立页面 clocksuite -> clocksuiteActivity`；QA `suite new/hr/min/rep/ok/sw/del/world/back/dump` 全通；长按 108 返回列表；**40s 内进屏保 0 次** ✅ |

### 9.1 ★ 这一页特有的一个设计决策：响铃提醒页**故意不搬**

`WinAlarmRing` 留在 `main.ftu` —— 闹钟是**跨页面**的（`pg::Alarm` 守护线程在任何页面都响），
提醒页由主循环 `tickAlarmRing()` 弹出，属于"整屏浮层"（与音量 OSD 同类）。
塞进某个应用页会导致"在别的应用里响铃看不到提醒"。

配套做法：**应用页在响铃期间自动收掉自己**（`onUI_Timer` 里判 `ringing()` → `closeActivity`），
回主界面后主循环把提醒页弹出来。实测：

```
PgAlarm: ★ 闹钟到点 #2 18:18 -> 响铃 + 亮屏
时钟套件: 响铃中 -> 让位（回主界面弹提醒页）
PocketGame: onUI_show (mode=0)
PocketGame: 闹钟提醒页已弹出（闹钟 18:18）      ← 抓屏确认提醒页可见
```
> **这条顺带补了老版本的空档**：以前在 wifi / 工具页里响铃**只有声音、没有界面**，
> 用户找不到"停止"（只能等 60s 自动停）。
>
> ⇒ **后续每一页（含 iptv/cast）都要照抄这一条**：
> ```cpp
> if (pg::Alarm::instance().ringing()) { EASYUICONTEXT->closeActivity("<页>Activity"); return false; }
> ```

### 9.2 还剩 2 页

| 待拆 | 要点 |
|---|---|
| `iptv.ftu`（slot 20） | 选台页 + 播放页（`videoview` 透明窗口）+ HLS 中继 + 硬解通道；**相位机（loading/超时）与 `stopForSwitch`（不重启）已就绪**（见 docs/iptv.md §11），搬过去时要带上 `tickIptv` 与 4 个 QA 命令 |
| `cast.ftu` | 投屏页由 DLNA 控制器 URL 触发（不是应用卡片），生命周期与 DLNA 会话绑定；`StreamPlayer/Hls` 的停止路径要与页面销毁对齐 |

---

## 10. 进度续：`iptv.ftu` 已完成（2026-09-14 深夜）

| ftu | slot | 内容 | 真机验收 |
|---|---|---|---|
| `iptv.ftu` | 20 | 选台页（8 槽 + 翻页 + 重新加载）+ 播放页（videoview 透明窗口 + 三行加载提示 + 缓冲底图 + 底部三键） | 点卡片 → `独立页面 iptv -> iptvActivity`；`iptvLogic: 频道表 47 个`；QA `iptv/i`/`iptvlist`/`iptvstat` 全通；**频道 1 出画 4177ms、频道 2 出画 9378ms**；失败源 10s 超时后回选台页；**换台 pid 8100→8100（不重启）**；**40s 内进屏保 0 次**；长按 901ms → `退出页面，已停流（不重启）` → 回主界面 ✅ |

### 10.1 这次搬了什么

- 布局：`ui/iptv.html`（root screen + `WinIptv` 默认显示 / `WinIptvPlay` 初始隐藏）
- 逻辑：`src/logic/iptvLogic.cc`（**908 行**；`mainLogic.cc` 从 4765 → **3788 行**，减约 980 行）
- 搬走的：`iptvLoadChannels` / 相位机（`gIptvPhase` 等 6 个状态量 + 5 个超时常量）/
  `iptvPlayAt` / `iptvStopAndBack` / `iptvBeginOpen` / `iptvFail` / `iptvAnim` / `iptvBackToList` /
  `resetIptvCache` / `syncIptv` / `syncIptvPlay` / `tickIptv` / `iptvDebugList` /
  7 个按钮回调（BtnCh0-7、BtnChPrev/Next、BtnIptvReload、BtnIptvPrev/Stop/Next）
- **顺带删掉一段过时代码**：主循环里的"换台续播"（读 `/tmp/pg_iptv_resume` 再 `startGame(20)`）——
  自 §11.3 换台改 `stopForSwitch()` **不再重启进程**后，已经没有任何代码写那个文件了。
- QA 通道：新页自带 `/tmp/pg_iptvcmd`（命令名沿用 `iptv <n>` / `iptvlist` / `iptvstat`，脚本不用改）。
- 屏保：`onUI_show` 关 / `onUI_quit` 恢复（独立页必须自己管，主界面不在前台时它的策略不跑）。
- 响铃让位：`onUI_Timer` 里判 `pg::Alarm::instance().ringing()` → `stopIptvAndQuit()`。
- 按键：108 长按 ≥700ms = 停流 + 返回列表；108 短按 = 停止回选台页；103/105 = 音量±。

### 10.2 这次踩到的（都已修）

| 坑 | 现象 | 正解 |
|---|---|---|
| 相位机枚举 `IPTV_IDLE/…` 定义在 **mainLogic 的全局区**，不在被搬的"IPTV 区"里 | 一大片 `'IPTV_IDLE' was not declared` | 随页搬过去，放本页匿名 namespace 顶部 |
| `setCtrlBg` / `setVideoCover` 是 mainLogic 匿名 namespace 里的"小 helper" | 同上 | 独立页**自带一份**（老坑：独立 ftu 不跨文件依赖） |
| 本页函数互相调用但定义顺序不对 | `'resetIptvCache' was not declared` 等 | 文件顶部加"本页内部函数前置声明"块 |
| `CONFIGMANAGER` / `pg::volumeStepGlobal` 要额外 include | 各自未声明 | 补 `manager/ConfigManager.h` / `platform/PgAudio.h` |
| mainLogic 里还有 IPTV 残留引用（`mWinIptvPlayPtr`、续播段） | 编译报未声明 | 逐个清掉（页面搬走后这些窗口在本页不存在） |

### 10.3 还剩 1 页

| 待拆 | 要点 |
|---|---|
| `cast.ftu` | 投屏页由 **DLNA 控制器 URL 触发**（不是应用卡片），生命周期与 DLNA 会话绑定；`StreamPlayer/Hls` 的停止路径要与页面销毁对齐；`mCasterPtr`（框架 ZKVideoView）那条本地文件路也在这一页 |

> 每页验收清单见 §3 第 9 条；**独立页必须有自己的 QA 通道**
> （`/tmp/pg_<页名>cmd` —— 后台 Activity 的定时器不跑，`/tmp/pg_autostart` 在独立页上是失效的）。
