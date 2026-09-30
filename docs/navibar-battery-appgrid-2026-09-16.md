# 状态栏「返回主页」/ 电池错位 / 应用图标找不到 —— 三处修正（2026-09-16 第二轮）

> 🔍 **检索导引（命中条件）**：用户问「**状态栏上一直显示返回主页**」「**标题被顶掉/看不见**」
> 「**电池图标错位/电量条跑到壳子外面**」「**某个应用的图标找不到、点不进去**」
> 「**系统设置进不去**」「**卡片是空白的**」，且平台为 **全志 V851s / V85X + FlyThings(EasyUI)**
> → **本篇就是答案**。相关：`docs/battery.md`（电池数据来源与素材）、
> `docs/ui-overlay-audit-2026-09-16.md`（navibar 覆盖判据）、`docs/page-split-plan.md`（加应用四处同步）。

## 0. 用户原话（三个独立问题）

> 「状态栏上面显示的返回主页去掉，电池图标不对。做成对应的图片直接贴就不存存在错位了。
>   系统设置图标没有找到。进不去系统设置界面」

三个问题各有独立的根因，**都不是"看起来那样"**：

| # | 现象 | 真根因（一句话） |
|---|---|---|
| ① | 状态栏常显「返回主页」、页面标题被顶掉 | `pg::swipeUp()` **不复位跟手位置** ⇒ 右滑过哪怕一次，`swipeProgress()` 永远返回 255 |
| ② | 电池电量条错位（顶出外壳） | 电量条的 y 有**两个来源**（源稿声明 y=16 / 运行时常量 y=5）⇒ 必然漂 |
| ③ | 系统设置卡片空白、点不进去 | `metaForSlot()` 里**写死 `slot >= 32` 就返回空** ⇒ slot 32（系统设置）拿不到元信息 |

---

## 1. ① 「返回主页」常显 —— 是**状态泄漏**，不是"设计如此"

### 1.1 现象
状态栏左侧长期显示「🏠 返回主页」，**页面标题被它顶掉**（标题控件在提示显示期间被 `setVisible(false)`）。
用户以为是"这个元素不该有"，其实它是**卡住了**。

### 1.2 根因（`src/platform/PgSwipe.cpp`）

```cpp
bool swipeUp(int x, int y) {          // 抬手
  ...
  sCurX = x;
  sActive = false;                    // ← 只清了 active
  sTriggered = (...);                 // 判定
  return sTriggered;                  // ← sStartX / sCurX 没复位！
}

int swipeProgress() {
  if (sStartX < 0 || sCurX < 0) return 0;    // 只有"从没滑过"才返回 0
  const int dx = sCurX - sStartX;            // 抬手后仍是被判过的那一整段位移
  if (dx >= kMinDx) return 255;              // ⇒ 永远 255
  ...
}
```

navibar 的提示判据正是 `pr > 0` ⇒ **提示永久显示**，`swipeCancel()` 又只有"触摸被 cancel"时才走。
实测验尸：`pginj swipe`（真触摸右滑）之后，提示一直在。

### 1.3 修法（两层）
1. **按用户要求删掉整组提示控件**（`ui/navibar.html` 的 `ImgNbBackHome` + `TextNbBackHome`
   + `navibar.cc` 的 `updateSwipeVisual()` 与 `sVisBack/sVisDx/sBackFlashMs`）。
   ★ **右滑返回手势本身保留**（继续 `goBack()`），只是不再有视觉反馈 —— 别再把它一起删掉，
     那是用户最初明确要的功能。
2. **同时修掉状态泄漏本身**（`PgSwipe.cpp`）：`swipeUp()` 结束手势后把 `sStartX/sCurX` 复位，
   让 `swipeProgress()` 回到 0（只复位位置，`sTriggered` 按设计保留）。
   ⇒ 不修的话，"按 `pr > 0` 显示浮层"这种写法对**任何**后续使用者都是地雷。

### 1.4 验收（真触摸）
```
/data/pginj swipe /dev/input/event0 8 300 300 305 25 12     # 从左边缘右滑
→ 日志：导航栏: 右滑达标 -> goBack()      （手势活着）
→ 页面上：状态栏显示的是"口袋游戏机"（标题），**没有**「返回主页」
```

---

## 2. ② 电池错位 —— 「一枚整图 = 一个状态」

### 2.1 现象与根因
电量条比外壳**高 11px、顶出壳外**。根因不是"常量填错了"，而是**同一份几何有两个来源**：

| 来源 | 电量条的 y |
|---|---|
| `ui/navibar.html`（源稿声明 `ImgNbBattFill`） | **16** |
| `src/logic/navibar.cc`（运行时 `setPosition` 用的常量） | **5** ← 实际生效 |

三控件拼装（外壳静态图 + 电量条运行时改宽度/位置 + 闪电 `setVisible`）天然要求
"运行时定位"，而定位常量与源稿必然各自漂。**实测取证**：截图里电量条落在屏幕 y≈5..25，
外壳在 13..39。

### 2.2 修法（用户给的方向就是对）
**烘成 52×26 的整图**：`images/batt_<色>_<档>.png`，把外壳描边 + 正极头 + 电量条 +
充电闪电**都画进同一张图**；运行时只 `setBackgroundPic`，**永不 setPosition** ⇒ 结构上不可能错位。

| 项 | 值 |
|---|---|
| 控件 | `ui/navibar.html` 的 **`ImgNbBatt`**（52×26 @ (378,13)）—— 就这一个（原来 3 个） |
| 素材 | 3 色（blue/red/amber）× 11 档（0,10,…,100）= **33 张**，替换旧的 122 张（更省） |
| 生成器 | `tools/ios_theme.py` 的 `gen_battery()`（**带尺寸自检**：写盘后回读校验 52×26） |
| 状态映射 | 低电→red / 充电→amber（**闪电烘在 amber 档里**，因为"充电⇒amber"是同一件事） |

细节、`battfake` 复现命令、以及"为什么不能再拆回三控件"见 **`docs/battery.md` §3.1**。

### 2.3 验收（真机像素级）
`battfake 50 0` → 电量条 **x=381..400、y=16..36**（= 外壳 13..39 内缩 3px，**完全在外壳内**）；
`battfake 12 0` → 红短条；`battfake 80 1` → 琥珀长条 **且图内有白色闪电**。

生成器侧另有自检：33 张全部 52×26、电量条位置/高度/宽度符合档位、闪电只出现在 amber 档。

---

## 3. ③ 应用图标找不到 —— slot 上限写死在**另一处**

### 3.1 现象
「系统分类」里只有 WiFi 一张卡，**系统设置那张是空白格**（看不见、也就不会去点）。

### 3.2 根因（`src/logic/mainLogic.cc`）

```cpp
static Game *metaForSlot(int slot) {
  static Game *cache[32] = {0};
  if (slot < 0 || slot >= appCount() || slot >= 32) return 0;   // ★★ 写死的 32
  ...
}
```

「系统设置」按规矩**追加在应用表尾** ⇒ `slot = 32` ⇒ `metaForSlot()` 返回 0 ⇒
`onObtainListItemData` 走"空位"分支（`syncRowIcon(item,-1)` + 清空名称）⇒ **卡片空白**。
`categorySlot()` 与图标控件（`Icon0..Icon32`，ID 连续）都是对的 —— 只有这一处上限没跟着涨。

> ⚠️ 排查时的**关键坑**：`/dev/fb0` 是 480×1600 双缓冲，用的抓屏方式如果不按 `pan` 选半页，
> 会拿到**上一帧/另一页**，看起来"卡片是有的"。必须先按 pan 对齐
> （用工程自带 `tools/grab.py`，它读 `sys/class/graphics/fb0/pan` 再取偏移）。
> 我第一轮就是被这个骗了：错误的半页显示 4 张卡，正确的半页才是 2 张。

### 3.3 修法
1. `metaForSlot()`：缓存容量与上限改用**共享真值** `ScoreStore::MAX_GAMES`（40），
   并把 `MAX_GAMES` 从 `private` 挪到 **public**（它本来就是"slot 下标容器"的容量真值）；
2. 超限**不再静默**：打 `LOGW` 点名（"slot %d 超出元信息缓存上限"）；
3. `onUI_init` 加**开机自检**：
   - `appCount() > kIconCount` ⇒ WARN（"slot %d 起的卡片会是空白"+ 四处同步清单）
   - `appCount() > ScoreStore::MAX_GAMES` ⇒ WARN（"最高分会被静默丢弃"）

### 3.4 验收（真机）
「系统分类」现在是 **2 张卡**：cell0 图标 == `app_icon_12`（WiFi）、
cell1 图标 == `app_icon_32`（系统设置）；**真触摸点 cell1 → `act=settingsActivity`** ✓

---

## 4. 这一类问题的判据（值得记住的三条）

1. **"看起来一直显示"的元素，先查它读的标志有没有在结束处复位** ——
   `swipeProgress()` 这种"跟手进度"天然是粘性状态，抬手必须归零。
2. **同一份几何不要有两个来源**：图能烘进图里的，就别让运行时再定位
   （判定式：**"静态图 + 运行时 setPosition" = 迟早漂**）。
3. **凡是按 slot 下标的数组/上限，用"应用总数/共享常量"，不许写字面量，且超限要告警** ——
   本工程在 `ScoreStore::MAX_GAMES` 上栽过一次（16 → 32 → 40），这次又在 `metaForSlot` 上栽一次。

## 5. 改动文件

| 文件 | 改动 |
|---|---|
| `ui/navibar.html` | 删「返回主页」两控件；电池三控件 → **一个 `ImgNbBatt`**（52×26） |
| `src/logic/navibar.cc` | 删跟手动效与相关状态；`refreshBatt()` 改成"一档一图"（只 `setBackgroundPic`） |
| `src/logic/mainLogic.cc` | `metaForSlot()` 上限改共享常量 + 超限 WARN；`onUI_init` 加两条开机自检 |
| `src/platform/PgStore.h` | `MAX_GAMES` 挪到 public（供"slot 下标容器"共用） |
| `src/platform/PgSwipe.{h,cpp}` | `swipeUp()` 复位跟手位置（修状态泄漏）+ 文档说明 |
| `tools/ios_theme.py` | `gen_battery()` 重写成整图生成（33 张 + 清旧素材 + 尺寸自检） |
| `tools/check_res_usage.py` | 文档：电池拼名格式从 `batt_fill_%s_%d` 换成 `batt_%s_%d` |
| `docs/battery.md` | §3 重写：显示方 = navibar，素材 = 整图（含血案与复现命令） |
