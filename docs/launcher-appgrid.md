# 启动器应用网格（`ui/main.html`）—— **slot ≠ 数组下标**

> 相关代码：`src/core/PgGames.{h,cpp}`（应用表）、`src/logic/mainLogic.cc`（网格 + 列表回调）、
> `ui/main.html`（38 个图标控件）、`tools/gen_icons.py`（图标 PNG）、`tools/gen_ui.py`（HIDDEN_CONTROLS）。
> 加应用的历史清单：`src/core/PgGames.cpp` 的 `kAppTable` 上方注释（每次追加都留了记录）。

## 一、网格是怎么工作的

- 主界面是 **`ZKListView`**（`ListGames`），`getListItemCount_ListGames()` 返回
  **当前分类的应用个数** `categoryCount(gCategory)`。
- **每一行**的 item 模板里塞了 **38 个"同位置重叠"的图标控件**（`Icon0..Icon37`，
  各静态引用一张 `app_icon_N.png`），运行时由 `syncRowIcon(item, slot)` **只显示其中一个**。
  - 为什么用这种"笨"办法：`subItem` 的**图片属性不按行区分**（`setBackgroundPic` / 雪碧图 +
    `setBackgroundCrop` 都实测过 ⇒ 所有行显示同一张），只有"预置多张 + 切换可见性"能work。
  - ⚠️ `syncRowIcon` **不能用行号 index 做缓存键**（视图会被跨行复用）——它读控件自身的
    `isVisible()` 比对，不一致才写。
- 点卡片走 **`onListItemClick_ListGames(list, index, id)`** → `categorySlot()` → `startGame(slot)`。
  `onButtonClick_IconNN` 是空桩，**图标不接收点击**。

## 二、★ 核心陷阱：`slot` 是"稳定存档索引"，不是"表里的第几行"

`kAppTable` 的每一行有两个身份：

| | 含义 | 会不会变 |
|---|---|---|
| **数组下标** | 表里第几行 = 分类内卡片顺序 | **会变**（删/加应用后重排） |
| **`slot` 字段** | 全局稳定槽位，用于**存档 / 最高分 / 图标控件编号** | **永不重排**（只追加） |

两个概念**已经脱钩**：应用下线过（fairy / kitten 那批），所以 `slot` 留下了空洞
——**现在是 33/34/35/36 空着，而"智能家居" `slot=37` 只排在第 34 行（下标 33）**。

于是下面这些写法**全部是错的**（都是"拿 slot 当下标 / 拿行数当 slot 上界"）：

```cpp
if (slot < 0 || slot >= appCount()) return;   // ✗ 37 >= 34 ⇒ 直接 return
int index = slot;                             // ✗ slot 不是下标
Game *m = metaForSlot(slot);                  // 里面若用 appCount() 当上界 ✗
const GameEntry &e = appEntry(slot);          // ✗ appEntry 收的是**下标**
```

**正确写法**（`PgGames.h` 提供的三个工具）：

```cpp
const int index = appIndexForSlot(slot);      // slot -> 表下标；找不到 -1
if (index < 0) { LOGW(...); return; }         // ★ 转换失败必须出声，别静默
const GameEntry *e = appBySlot(slot);         // slot -> 表项；找不到 nullptr
const int maxSlot = maxAppSlot();             // "图标/存档槽够不够"要跟它比
```

### 血案记录（同一类坑，两次）

| 时间 | 受害者 | 现象 | 根因 |
|---|---|---|---|
| 2026-09-16 | 系统设置（slot 32） | 「系统设置图标没有找到，进不去」 | `metaForSlot` 的 `static Game *cache[32]` 正好贴上限 |
| 2026-09-24 | **智能家居（slot 37）** | 「**程序入口不见了**」——启动器上那一格**整格空白**，点了没反应 | `metaForSlot` 用 `appCount()`(34) 当上界 ⇒ `37 >= 34` 判成"空位"；`startGame` 同样 `37 >= 34` 直接 return |

两次都是**静默失败**：不崩、不报错、日志一声不吭，只能靠肉眼在网格里找空位。
⇒ 现在两处都加了 `LOGW`（slot 越界 / 表里找不到对应行），并且开机自检改用 `maxAppSlot()`。

## 三、加一个应用要动的地方（完整清单）

以 `kAppTable` 追加一行（**永远追加在表尾**，slot 取 `maxAppSlot()+1`）为起点：

1. `src/core/PgGames.cpp` 的 `kAppTable` —— 追加一行（`slot` 必须唯一且更大）
2. `ui/main.html` —— 加 `Icon<slot>` 控件（位置/尺寸**照抄上一行**，只换 caption）
3. `tools/gen_icons.py` 的 `ICONS` —— 加一条（生成 `app_icon_<slot>.png`）
4. `src/logic/mainLogic.cc` 的 `kIconCount` —— 改成 `maxAppSlot()+1`
5. `tools/gen_ui.py` 的 `HIDDEN_CONTROLS` —— `range(N)` 的 N 同步
6. 独立 ftu 的应用还要：`startGame` 里 `kPageApps` 加一行、`navibar.cc` 的 `kTitleMap`
   （漏了标题会停在兜底值）、`mainLogic.cc` 的 autostart 分发命令

⚠️ 以下**不要写死字面量**：
- `kIconCount` / `HIDDEN_CONTROLS` 的 N —— 用 `maxAppSlot()+1` 推出来；
- `ScoreStore::MAX_GAMES` —— 是**存档槽位数**，必须 > `maxAppSlot()`（现在是 40 > 37）。

## 四、验收（免触摸，走 QA 通道 `/tmp/pg_autostart`）

```bash
# 切到某个分类（0 游戏 / 1 工具 / 2 系统）
adb shell "echo 'cat 2 #1' > /tmp/pg_autostart"
# 点该分类下第 n 行卡片（0 起算；等价于真触摸，走同一条 startGame 路径）
adb shell "echo 'enter 2 #2' > /tmp/pg_autostart"
```

**判据**（比"看起来正常"硬）：
- 卡片数 = `categoryCount(cat)`。抓屏后用**列块检测**数（见下），空位会少一块；
- 日志里必须出现 **`Pg: startGame slot=<slot> -> index=<index> appCount=<n>`**
  —— 这句是 2026-09-24 之后才有的格式，`index` 与 `slot` 不同就说明转换真的生效了；
- 接着应有 `独立页面 <id> -> <activity>` + `导航栏: 标题 act=... -> <标题>`。

数卡片的办法（不用 OCR）：卡片底色与黑底差异明显，按列扫描非黑像素得"列块"，
每块就是一个卡片：

```python
colhit = [sum(1 for y in range(120,700,3) if sum(px[x,y])/3.0 > 20) for x in range(480)]
# 连续 >3 且宽度 >30 的区段 = 一个卡片列
```

⚠️ **抓屏前先唤醒屏保**（`screensaverTimeout=30`）：adb 命令不算触摸，
排查过程中很容易抓到"屏保画面"而误判（实测：`saver_bg_day.png` 主色是亮蓝
`(92,148,252)`、夜晚版是深蓝 `(16,26,58)`，看到这两种底色就是屏保，不是应用）。
