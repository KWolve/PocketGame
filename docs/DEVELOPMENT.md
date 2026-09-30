# 二次开发指南

本篇讲「拿到这个工程之后，怎么往里加东西」。读完 [README](../README.md) 的目录结构再看这篇。

---

## 0 先建立工程的心智模型

```
看得到的界面                       背后是谁在画 / 谁在响应
──────────────────────────────    ─────────────────────────────────────────────
启动器（应用网格，分类分页）        ui/main.html → main.ftu + src/logic/mainLogic.cc
  └─ 点开一款游戏  ────────────→   pg::Game 子类（软渲染画布，不走控件）
  └─ 点开一个工具  ────────────→   独立的 ui/<名>.html → <名>.ftu + <名>Logic.cc
  └─ 点开系统设置/HA ──────────→   同上（独立 Activity）
常显的导航栏 / 状态栏               ui/navibar.html、ui/statusbar.html（SysApp，全局常显）
```

三个关键概念：

| 概念 | 是什么 | 注意 |
|---|---|---|
| **ftu / Activity** | 一个独立页面（一个 `ui/<名>.html` 对应一个 `<名>.ftu` 和一个 Activity） | 逻辑文件名**必须**叫 `src/logic/<名>Logic.cc`，`fun` 靠同名把两者绑起来 |
| **`pg::Game` 子类** | 软渲染画布上的一个"应用"（游戏、也有工具用它做自绘页） | 注册在 `src/core/PgGames.cpp` 的 `kAppTable` |
| **`slot`** | 应用表里的**稳定存档索引**，最高分/设置按它存盘 | ⚠️ **`slot` ≠ 数组下标**，见 §4 |

---

## 1 加一款画布游戏（最简单，推荐从这里开始）

画布游戏不依赖任何 UI 控件，是纯 C++ 绘制，加一款只需要**两处**：

1. 新建 `src/core/Pg<你的游戏>.cpp`，写一个 `pg::Game` 子类：

   ```cpp
   #include "PgGame.h"
   namespace pg {
   class GameMyThing : public Game {
    public:
     void onEnter() override;              // 初始化
     bool onTick(uint32_t ms) override;    // 逻辑推进（绝对时间基准，别用"帧累加"）
     void onDraw(Canvas &c) override;      // 画到画布
     bool onTouch(int action, int x, int y) override;
   };
   }
   ```

2. 在 `src/core/PgGames.cpp` 的 `kAppTable` **表尾追加**一行：

   ```cpp
   Game *createMyThing() { return new GameMyThing(); }
   ...
   {"mything", createMyThing, APP_GAME, 38},   // slot 用没被占用的递增号
   ```

3. 跑 `python tools/gen_icons.py` 出图标，并按 §4 的清单确认图标控件数已同步。

**参考实现**（由简到繁）：`PgReact.cpp` → `PgSnake.cpp` → `PgTetris.cpp` → `PgMatch3.cpp`。

画布上画字的约束见 [`canvas-font.md`](canvas-font.md)：自研字库只预烘**有限档位**，
字号档位是**编译期常量**，超出档位会被位图放大（= 拉伸，本项目禁止）。

---

## 2 加一个「原生控件」应用（独立 ftu）

用 FlyThings 控件做页面，适合表单/列表/设置类。**一共 6 处必须同步**，漏一处就是静默 bug：

| # | 位置 | 改什么 | 漏了的症状 |
|---|---|---|---|
| ① | `ui/main.html` | 加图标控件 `Icon<slot>` | 启动器上没有这张卡片 |
| ② | `tools/gen_icons.py` 的 `ICONS` | 加一条图标定义 | 图标缺失 / 错位 |
| ③ | `src/logic/mainLogic.cc` 的 `kIconCount` | 上限 +1 | 最后一张卡片点不动 |
| ④ | `tools/gen_ui.py` 的 `HIDDEN_CONTROLS` | 加上新控件的隐藏项 | 新页控件在主界面串出来 |
| ⑤ | `src/core/PgGames.cpp` 的 `kAppTable`（**表尾追加**）+ `PgGames.h` 的类声明 | 注册应用 | 卡片空白、点了没反应 |
| ⑥ | `src/logic/navibar.cc` 的 `kTitleMap` | 加标题映射 | **标题停在兜底值**（这处最容易漏） |

另外两处联动（同一个应用才有）：
- `src/logic/mainLogic.cc` 的 **autostart 分发**（QA 免触摸通道要能进这个应用）
- `src/logic/mainLogic.cc` 的 **`onButtonClick_Icon<slot>`** 回调

然后新建 `ui/<名>.html` → 在 `tools/gen_ui.py` 的 `UI_SOURCES` 里登记 → 新建
`src/logic/<名>Logic.cc` → 跑生成器（见 §5）。

> **铁律：下标和上限不写字面量**，一律从 `kAppTable` 的 `appCount()` / `maxAppSlot()` 推出来。

---

## 3 改 UI 外观（设计源稿 → 界面）

界面的**真值**是 `ui/*.html`（带自定义的 `class` 与 `data-*` 属性），设备实际加载的是生成出来的
`.ftu`。改完**必须重跑生成器**，否则改了没生效：

```bash
python tools/gen_ui.py          # ui/*.html → ui/*.json → ui/*.ftu（并调 fui.exe 打包）
python tools/gen_font.py        # 页面文案变了 ⇒ 字库子集要跟着重裁
```

`tools/gen_ui.py` 不只是调转换器，它还会做几件转换器不做的事（详见脚本头部注释）：
修正 `.modal` / `.card` 的初始可见性、按 `data-bg` / `data-round` 给圆角控件回填容器底色等。

### 素材规范（这几条踩过坑，务必遵守）

1. **1:1，绝不拉伸**。静态元素一律用原始尺寸 PNG。图内容与控件尺寸相当就**禁用九宫格**。
   检查：`python tools/check_stretch.py`、`python tools/check_assets.py`。
2. **坐在卡片/面板上的圆角块，不能用圆角九宫格**。`.9.png` 的四角是透明的，
   透出来的是**窗口黑底**（不是它下面的卡片色）⇒ 卡片上会出现四个纯黑角。
   **正解 = 不透明 1:1 PNG，四角直接烘容器底色**。
3. **运行时 `setBackgroundPic()` 的那条路不保留 alpha** ⇒ 运行时切换的图必须烘**不透明底**
   （透明底会变成白块）。而**静态九宫格套**相反：烘**透明底**，圆角外交给控件的 `bgColorTab`
   （其值 = 该控件所坐容器的底色，由 `gen_ui.py` 自动填）。
   ⚠️ 这两套是**分离**的，改一套别影响另一套。
4. 改 `resources/images` 下的 PNG 之后，**`fun build` 不会刷新 `imgout`**，要先把旧的清掉。
5. 全量体检：`python tools/audit_resources.py`（角行为 / 座位 / 九宫格 marker / 1:1 铁律 /
   未引用素材，共 7 节）。

### 加中文

`font/pocketgame.ttf` 是**从系统字体裁出来的子集**（只含界面实际用到的字），
不是完整字库。所以**新写了中文文案就必须重跑**：

```bash
python tools/gen_font.py
```

`gen_font.py` 会扫描 `ui/*.json` 与 `src/**` 里的字符串字面量自动收集字符集，
并内置 GB2312 全集兜底。注意：

- **缺字形的表现是「整个字消失」，不是方框**（ASCII 数字缺字形时更隐蔽）。
- 输入法相关还有第二层：`gen_font.py` → `gen_ime_pinyin.py` **顺序不能反**
  （反过来会用旧字库过滤，新字被静默丢掉）。
- 设备系统字体是**裁剪版**，不要指望它兜底 —— 项目开了 `enable.font.location=true`，
  上了项目字库就**完全**用项目字体，没有逐字回退。

---

## 4 `slot` 与数组下标（踩过两次的坑）

`kAppTable` 里有两个"序号"，含义完全不同：

- **数组下标** = 它在**所属分类里排第几行** = 卡片顺序 = QA 命令用的序号
- **`slot`** = **稳定存档索引**，最高分/设置按它存盘

删过应用之后两者会**脱钩**（本项目当前 `slot` 33/34/35/36 就是空洞，HA 的 `slot=37`
实际排在第 34 行）。所以：

```cpp
// ✗ 禁止
if (slot >= appCount()) ...
appEntry(slot);  gameEntry(slot);

// ✓ 必须
appIndexForSlot(slot);  appBySlot(slot);  maxAppSlot();
```

- 新应用**一律追加到表尾**，`slot` 用递增的新号，**不要在中间插行**。
- 图标是 `Icon<slot>`、存档也按 `slot` ⇒ "够不够"要和 **`maxAppSlot()`** 比，不是 `appCount()`。
- `ScoreStore::MAX_GAMES` 要 **> 最大 slot**，否则最高分会被**静默丢弃**。
- 完整清单与验收判据： [`launcher-appgrid.md`](launcher-appgrid.md)。

---

## 5 免触摸真机验收（这个工程的核心工作方式）

设备只有 3 个物理按键 + 触摸，但验收**不靠手点** —— 走一个文件 QA 通道：

```bash
# 进某个应用（裸槽位号）
echo 22 > /tmp/pg_autostart          # 进 slot 22 的应用

# 给当前页发命令（每页一个文件；内容"变化"才会执行；# 后面是注释）
echo 'start #1' > /tmp/pg_maincmd
```

配合工具：

| 工具 | 干什么 |
|---|---|
| `tools/grab.py` | 抓设备屏幕到本地（**多设备时必须** `--serial` / `PG_SERIAL`，否则直接拒绝） |
| `tools/pginj`（源码 `tools/pginj.c`） | 真触摸注入：点击 / 长按 / 滑动。可判 `touch:` 有而 `click` 无 = 有浮层在挡 |
| `tools/audit_resources.py` | 素材全量审计 |
| `tools/shot_stat.py` | 抓屏像素统计（比对设计值） |
| `tools/ha_ui_review.py` | 把设计规格 `ui/ha.json` 与真机像素逐控件比对（底色/居中/裁切/对比度） |

**四条硬经验**：

1. **检查工具要先自证「它报得出来」** —— 用一个已知会失败的场景跑一遍，确认判据真的会红。
   否则你得到的"通过"可能只是判据本身写错了。
2. **别依赖日志整句匹配**。这块板的 logcat 缓冲只有十几行，判据要**落到文件**
   （例：定时器心跳写 `/tmp/pg_ha_tick.txt`，看它的计数是否在涨）。
3. **抓屏前必须唤醒屏保**（`screensaverTimeout=30`）。adb 命令不算触摸，
   否则抓到的是屏保画面，会被误判成"渲染错乱"。
4. **本板 shell 很穷**：没有 `grep` / `head` / `tail` / `awk` / `stat` / `printf` / `which` / `date`。
   写 QA 文件一律用 `echo`；设备侧排障只用 `cat` / `ls` / `mount` / `getprop`，别写管道。

---

## 6 红线

- **绝不写系统只读分区**：`/res`、`/dev/block/mtdblock*`、`/dev/mtd*`。
  不要 `dd` / `mount` / `flash_erase` / `mkfs`。**唯一正路是 `tools/upgrade_device.sh`**。
- **不要整机连着 reboot**：会让 mtd3 的 squashfs superblock 丢失，`/res` 挂不上。
  重启应用用 `setprop ctl.restart zkswe`。
- 一次改动**攒齐再固化**，别「改一点刷一点」，两次固化之间留间隔（见 [`BUILD.md`](BUILD.md) §3）。

---

## 7 目录速查

| 我想改… | 去这里 |
|---|---|
| 页面布局 / 控件真值 | `ui/*.html`（改完跑 `tools/gen_ui.py`） |
| 某个页面的逻辑 | `src/logic/<名>Logic.cc`（与 ftu 同名） |
| 游戏/画布应用 | `src/core/Pg*.cpp`，注册在 `PgGames.cpp` |
| 平台能力（音频/蓝牙/网络/流媒体） | `src/platform/Pg*.cpp` |
| 画布绘制与字库运行时 | `src/core/PgCanvas.*`、`src/core/PgFontData.h` |
| 图标 / 美术素材生成器 | `tools/gen_icons.py`、`tools/gen_game_art.py`、`tools/ios_theme.py` |
| 字库 | `tools/gen_font.py` → `font/pocketgame.ttf` |
| 音效 | `resources/audio/*.wav`（新增要追加到 `PgGame.h` 的 `SfxId` **表尾**） |
