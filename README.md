# PocketGame

**一台只有 3 个物理按键 + 触摸屏 + 喇叭的掌上设备，被做成了 34 个应用。**

全志 V85X 平台 / FlyThings (EasyUI) 框架 / **480×800 竖屏**。
整个工程 —— 界面、游戏逻辑、美术素材生成器、真机验收工具链 —— 都在这个仓库里，**零外部美术资源**：
绝大多数图标和贴图是用 Python 脚本（Canvas/PIL 程序化绘制、headless Chrome + three.js 渲染）**生成**出来的。

![启动器 · 游戏页](docs/screenshots/01-launcher-games.png)
![消消乐](docs/screenshots/18-game-match3.png)
![网络收音机](docs/screenshots/20-radio.png)
![智能家居](docs/screenshots/22-ha.png)

> 更多截图见 [`docs/screenshots/`](docs/screenshots/)（15 张，每个应用一张主界面，全部是真机抓屏）。

---

## 功能一览

**34 个应用 = 21 款游戏 + 10 个工具 + 3 个系统页**，启动器按「游戏 / 工具 / 系统」分类分页。

### 游戏（21）

| 类别 | 应用 |
|---|---|
| 经典 | 2048、俄罗斯方块、打飞机、小鸟过水管、贪吃蛇、扫雷、推箱子、打砖块 |
| 触摸优先 | 打地鼠、记忆翻牌、五子棋、数字华容道、反应计时 |
| 消除 / 趣味 | 消消乐（8×8 三消 + 关卡目标）、摇骰子（3 颗骰子 3D 翻滚） |
| 儿童益智 | 数字连线画（数序）、算术泡泡（心算）、找不同（观察力） |
| 乐器 | 数独、节奏钢琴（下落式跟弹 8 键）、打鼓（下落式跟打 6 鼓位） |

### 工具（10）

番茄时钟 · 定时器 · 秒表 · 计算器 · 蓝牙红外遥控（可学习）·
时钟套件（世界时钟 + 闹钟）· 网络电视（HLS/IPTV）· 信号探针（WiFi/蓝牙探测 + 热点猎手）·
网络收音机（61 台实测电台 + 双声道 VU 表）· 局域网摄像头查看

### 系统（3）

WiFi 状态与设置 · 系统设置 · **智能家居遥控器（Home Assistant）**

### 外加

桌面宠物（离线 PBR 渲染）· 小精灵（实拍视频转画布精灵）· 屏保（诗词 / 像素动画）·
全局导航栏 · 状态栏 · 音量 OSD · **自研拼音输入法**（含九宫格键盘与候选词）

---

## 技术要点

这些是这个工程真正花时间的地方，也是二次开发时最值得参考的部分：

- **软渲染画布**（`src/core/PgCanvas.*`）—— 21 款游戏共用一个不依赖 UI 控件的绘制层。
  画布内容画进内存位图，再挂到控件上由**硬件通道**送上屏幕。
- **自研字库子集 + 字号档位制**（`tools/gen_font.py` → `font/pocketgame.ttf`）——
  从系统字体裁出「界面实际用到的字」，把 10 MB 级字库压到几百 KB。
  画布文字只预烘有限个字号档位，**档位是编译期常量**，避免运行时位图放大造成拉伸。
- **代码生成美术**（`tools/gen_icons.py` / `gen_game_art.py` / `ios_theme.py` / `gen_ha_tiles.py`）——
  所有图标、卡片、磁贴、九宫格圆角素材都由脚本产出，可一键重生成、可 diff。
- **离线 PBR 渲染管线**（`tools/pet3d/`）—— headless Chrome + three.js 渲染 3D 角色，
  再逐帧转换成嵌入式可用的贴图序列。
- **实拍视频 → 画布精灵**（`tools/gen_elf_from_video.py`）—— 逐帧量测 → 抠像 → 统一缩放 → 固定锚点。
- **免触摸真机验收**（QA 通道 + `tools/grab.py` + `tools/pginj` + `tools/audit_resources.py`）——
  通过设备上的文件通道下发命令、抓屏、逐像素比对。整个工程的功能验收都不是"手点一下看看"。
- **全套素材审计工具**（`tools/audit_resources.py`）—— 圆角角行为、九宫格 marker、1:1 铁律、
  未引用素材，7 个维度的自动体检（373 个素材全过）。

以及一份**踩坑清单**：这个框架有不少反直觉的行为（浮层如何吃触摸、控件树是平铺的、
九宫格与透明度的两套规则），都记在
[`docs/框架缺陷与踩坑清单-2026-09-27.md`](docs/框架缺陷与踩坑清单-2026-09-27.md)
和 [`docs/kb-*.md`](docs/) 里，建议动手前先扫一遍。

---

## 目录结构

```
PocketGame/
├── src/
│   ├── core/           游戏与应用（Pg*.cpp）、软渲染画布、字库运行时
│   ├── logic/          各页面的逻辑（<名>Logic.cc，与 ui/<名>.ftu 同名绑定）
│   ├── platform/       平台能力：音频 / 蓝牙 / 网络 / 流媒体 / 传感器 / 存储
│   ├── media/          WAV 播放等
│   ├── ui/             自绘控件（ToolPage）
│   ├── activity/       主 Activity
│   └── dependencies/   ⚠️ 厂商 SDK 与第三方库，**本仓库不附带**（见其 README）
├── ui/                 界面真值：*.html（设计源稿）→ *.json → *.ftu（设备加载）
├── resources/          素材：images/ audio/ certs/ iptv/ media/
├── font/               项目字库子集（由 tools/gen_font.py 生成）
├── tools/              生成器 + 真机验收工具 + 素材审计
├── docs/               实测文档（含 screenshots/）
├── Manifest.xml        依赖声明（FlyThings 官方包）
├── package.properties  平台配置（分辨率 / 触摸设备 / 屏保超时 / 字库路径）
└── CHANGELOG.md        逐版本开发日志（含每一版的实测数据与原因）
```

---

## 快速开始

### 1 准备环境

- **FlyThings / EasyUI 工具链**（`fun` 命令行）与 ADB —— **本仓库不附带**
- **全志 V85X SDK 头文件与库**、ffmpeg 静态库等 —— **本仓库不附带**

先跑一遍依赖检查，缺什么它会直接列出来：

```bash
python tools/check_deps.py
```

按 [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) 把缺的补齐。

### 2 编译、打包、烧写

```bash
fun install                       # 拉取 Manifest.xml 声明的官方包
fun build                         # 编译
fun pack -p V85X --release-version 1.0.0 -o out/update.img   # 出固件包

tools/upgrade_device.sh 1.0.0     # 出包 + ADB 刷进设备 + 校验
ITER=1 tools/upgrade_device.sh    # 快速迭代：只推临时目录，不刷机
```

完整流程、环境变量、以及**固化烧写的三个真实的坑**（多设备时 `fun launch` 必失败、
Git Bash 路径转换、固化后设备会自己重启导致分区挂不上）见 [`docs/BUILD.md`](docs/BUILD.md)。

> **红线**：设备上的 `/res` 与 `/dev/mtd*` 是只读系统分区，严禁 `dd` / `mount` / `flash_erase`。
> 唯一正路是 `tools/upgrade_device.sh`。

### 3 二次开发

先读 [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md)。三条最常见的路径：

| 想做什么 | 大概要动什么 |
|---|---|
| **加一款画布游戏** | 写一个 `pg::Game` 子类（`src/core/Pg*.cpp`）+ 在 `PgGames.cpp` 的 `kAppTable` **表尾加一行** |
| **加一个应用页** | 新建 `ui/<名>.html` + `src/logic/<名>Logic.cc` + **同步 6 处注册点**（清单在文档里，漏一处就是静默 bug） |
| **改界面外观** | 改 `ui/*.html` → 跑 `tools/gen_ui.py` →（文案变了再跑）`tools/gen_font.py` |

---

## 关于本仓库的边界

本仓库是**纯源码 + 纯文档**，刻意不含任何二进制：

| 不含 | 原因 | 怎么补 |
|---|---|---|
| 全志 V85X SDK 头文件与静态库 | 厂商 SDK，分发以厂商协议为准 | [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) §B1 |
| ffmpeg / OpenSSL 等第三方库 | 再分发受各自许可证约束 | 同上 §B2 / §B3 |
| FlyThings 工具链（`fun` / `fui.exe`） | 专有工具（约 38 MB） | 装 FlyThings IDE |
| 构建产物（`out/`、`.fun/`、`Release/`） | 可由源码重新生成 | `fun build` |

**因此：克隆下来不能立刻编译**，要先按上表补齐依赖（`python tools/check_deps.py` 会告诉你差什么）。

少数工具脚本里带有原作者机器上的默认路径（如 `D:\zkswe\...`），
已经全部改成可用环境变量覆盖：`PG_ADB`、`PG_HTML2JSON`、`PG_MCP_UI_TOOLS`、`PG_MCP_FONTS`、
`PG_SERIAL`、`ZKSWE_MCP_FONTS`。

---

## 文档

| 从这里开始 | |
|---|---|
| [`docs/BUILD.md`](docs/BUILD.md) | 编译 / 打包 / 烧写 |
| [`docs/DEPENDENCIES.md`](docs/DEPENDENCIES.md) | 依赖获取 |
| [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) | 二次开发指南 |
| [`docs/README.md`](docs/README.md) | **全部 79 篇文档的索引** |
| [`docs/screenshots/`](docs/screenshots/) | 15 张真机截图（启动器三页 + 各应用主界面） |
| [`CHANGELOG.md`](CHANGELOG.md) | 逐版本开发日志（每个版本都带实测数据与结论） |

---

## 许可证

本项目代码以 **MIT** 许可发布 —— 见 [`LICENSE`](LICENSE)。

通过 FlyThings 包管理器和厂商 SDK 引入的第三方组件（EasyUI 框架、ffmpeg、OpenSSL、zlib、
civetweb、btstack 等）**不在本仓库内**，其使用与分发请遵守各自的许可证。

---

## 致谢

- UI 框架：[FlyThings / EasyUI](https://www.flythings.cn/)
- 平台：全志 V85X
- 字库基底：思源黑体（Source Han Sans，SIL OFL）
- 离线 3D 渲染：three.js + headless Chrome
