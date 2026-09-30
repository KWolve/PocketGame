# 真机截图

> 全部是 **480×800 竖屏真机抓屏**（`tools/grab.py`），不是设计稿或模拟器截图。
> 每个应用只保留**主界面**一张。

## 启动器

| 截图 | 说明 |
|---|---|
| ![](01-launcher-home.png) | `01-launcher-home.png`<br>启动器主界面（应用网格，按「游戏 / 工具 / 系统」分类分页） |
| ![](A1-icons-overview.png) | `A1-icons-overview.png`<br>全部应用图标总览 —— 这些图标**全部由 `tools/gen_icons.py` 代码生成**，没有一张是手绘的 |

## 系统 / 主要页面

| 截图 | 说明 |
|---|---|
| ![](10-ha-home.png) | `10-ha-home.png`<br>智能家居（Home Assistant 遥控器）：设备列表，含 `unavailable` 灰态 |
| ![](20-ime-pinyin.png) | `20-ime-pinyin.png`<br>自研拼音输入法：九宫格键盘 + 候选词 |
| ![](40-wifi.png) | `40-wifi.png`<br>WiFi 设置：列表 / 详情 / 密码输入 |
| ![](98-remote.png) | `98-remote.png`<br>蓝牙红外遥控：可学习 + 发送 |

## 工具类应用

| 截图 | 说明 |
|---|---|
| ![](30-calc.png) | `30-calc.png`<br>计算器 |
| ![](31-clock-suite.png) | `31-clock-suite.png`<br>时钟套件：世界时钟 + 闹钟 |
| ![](50-iptv.png) | `50-iptv.png`<br>网络电视：1080p 直播播放 |
| ![](51-radio-vu.png) | `51-radio-vu.png`<br>网络收音机：**双指针 VU 表（左右真双声道）**，用框架自带 `ZKPointer` 连续旋转实现 |
| ![](60-probe-main.png) | `60-probe-main.png`<br>信号探针：WiFi / 蓝牙探测 + 热点猎手 |

## 游戏

| 截图 | 说明 |
|---|---|
| ![](70-game-match3.png) | `70-game-match3.png`<br>消消乐（8×8 糖果三消 + 关卡目标） |
| ![](72-game-tetris.png) | `72-game-tetris.png`<br>俄罗斯方块（含硬降） |
| ![](74-game-dice.png) | `74-game-dice.png`<br>摇骰子：3 颗骰子 · 离线 3D 烘帧翻滚 |
| ![](79-game-sudoku.png) | `79-game-sudoku.png`<br>数独（9×9 唯一解 · 三档难度） |
| ![](80-game-piano.png) | `80-game-piano.png`<br>节奏钢琴（下落式跟弹 · 8 键） |
| ![](82-game-connect.png) | `82-game-connect.png`<br>儿童益智：数字连线画（另两款为算术泡泡、找不同，同一套「蜡笔童趣风」） |

> 21 款游戏共用同一个软渲染画布（`src/core/PgCanvas.*`），这里只放代表作；
> 其余各款见 `src/core/Pg*.cpp`。

## 其它

| 截图 | 说明 |
|---|---|
| ![](90-pet.png) | `90-pet.png`<br>桌面宠物：headless Chrome + three.js **离线 PBR 渲染**输出贴图 |
| ![](91-elf.png) | `91-elf.png`<br>小精灵：**实拍视频逐帧抠像**转成画布精灵 |
| ![](95-screensaver-poem.png) | `95-screensaver-poem.png`<br>屏保：诗词（另有像素马里奥动画版） |
