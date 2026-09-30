# 静态图形控件怎么选：`class="icon"`（textview+背景图）vs `btn` + 初始显隐必须在生成阶段落地

> 2026-09-18 入库（来源：把一个"用静态图拼出来的页面/动效"从 82 个 `btn` 改成 55 个 `icon` 的实测）。
> 补充 `html-subset-quickref.md` / `gui-controls-gap.md` 的两个缺口：
> **① `class="icon"` + `data-pic` 才是"只要图、不要点击"的正解；② textview 类控件 html2json 不写 `visible` 字段**。
> 检索词：class icon/data-pic/data-bgpic 区别/textview 挂背景图/静态图控件/装饰图怎么做/
> 不要点击回调/onButtonClick 未定义/链接失败 undefined reference/touchable 默认值/
> visible 字段缺失/data-visible 不支持/初始隐藏/控件叠同一格/负坐标被改 0/left 变 0。

## 1. `data-pic` vs `data-bgpic`（最常混）

| 写法 | 生成控件 | touchable | 需要点击回调吗 | 备注 |
|---|---|---|---|---|
| `<div class="icon" data-pic="images/a.png">` | **textview** + `backgroundPic` | **false** | **不需要** | ★ 静态图/装饰图/纯展示层首选 |
| `<div class="btn" data-bgpic="images/a.png">` | button + `backgroundPic` | true | **需要** | 会被"按钮必配回调"约束（见 §2） |
| `<div class="icon" data-pic="a.png">` | 同上 | false | 不需要 | 路径**不含 `/`** ⇒ 自动加 `images/` 前缀 |

- `class="icon"`（也认 `img/image/pic/iconfont`）→ 走 **textview** 分支，`touchable:false`，
  其 `backgroundPic` 的图**按控件盒缩放**（所以改宽高 = 缩放，和 btn 一样）。
- ⚠️ **textview 只认 `data-pic`**（button 分支才认 `data-bgpic`/`data-pic0`）——写错就是"图没上屏"。

## 2. ★ 为什么"静态图"千万别用 `btn`：每个 button 都必须有同名点击回调

`fun` 生成的 `INIT_UI_EVENT_BINDINGS` 会给**每一个 button** 绑 `UI_EVENT_TYPE_CLICK`，
并引用 `onButtonClick_<Caption>(ZKButton*)` ⇒ **源稿里加一个 btn、逻辑里没写这个函数，就是链接错误**。
一个 55 个静态图的页面 = 55 个空回调函数（纯噪音）。
改成 `class="icon"` 后：生成代码里 `onButtonClick_*` 出现 **0 次**，也不必再写那 55 个空函数。
（实测：同一页面 82 个 btn → 55 个 icon，交互零变化、代码少一大截。）

顺带一条收益：**`touchable=false` 不会吃掉"整屏浮层/系统页"的全局触摸**。
在"任意触摸即退出"的系统页（屏保）上，全页控件都是可点按钮时，触摸有可能先被控件消费掉；
换成 icon 后这类隐患一并消失（实测：改完 `pginj tap` 依然能正常唤醒）。

## 3. ★★ 初始显隐：html2json **不支持 `data-visible`**，且对 textview **根本不写 `visible` 字段**

- 需要"初始隐藏"时，**不能**在 html 里写 `data-visible`（转换器不认）；
- 而且 textview/icon 分支生成出来的节点**没有 `visible` 键**（button 分支有）——
  "缺字段"的行为不可靠（同类前车之鉴：EditText 缺 `touchable` 导致输入框点不动）。
- ⇒ 正解：在**生成阶段后处理** json，显式写 `visible`：
  1. 按**控件名规则**批量设（如"N 张同位置叠图只留第 0 个"）；
  2. 维护一张"初始隐藏"清单（整屏 window、叠图、缓冲底图…），逐个 `visible=false`。
- ⚠️ **控件改名时必须同步改这两处**（正则按名字匹配），否则表现为"**一屏控件全叠在一起**"。

## 4. 其它两个实测坑

- **负坐标会被钳成 0**：源稿写 `data-x="-80"`（想让它从屏幕外滑进来）→ 生成出来是 `left: 0`。
  ⇒ "从屏幕外进入"只能靠**运行时 `setPosition`**（首帧必须先写一次位置，别指望初值）。
- **同位置叠 N 张图做帧动画时**，除当前帧外全部要 `visible:false`，且**可见的那张也要显式 `true`**
  （靠"缺字段默认可见"会在某些控件类型上翻车）。

## 5. 自检（生成阶段就该拦住，别等上机）

- **图尺寸 == 控件盒**（不等 ⇒ 框架缩放 ⇒ 拉伸/发糊）：本工程有 `check_stretch` 这类脚本自动比对；
- **部分重叠检查**（两个控件盒部分相交 = 一定互相遮挡）：脚本按盒模型算，抓屏查不出来；
- **资源引用一致性**（源稿引用的每张图都存在、且没被改名）：对不上就报；
- **无引用资源体检**（帮 `class="icon"` 改造后清掉退役图）：本项目实测改版后清出 22 张退役图。
