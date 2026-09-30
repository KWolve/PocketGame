# 资源冗余核查：`resources/images`（2026-09-16）

用户提问：*"resources 目录下我看图标很多，还有一些是 .9.png 的，是不是有重复的图片资源。"*

结论先说：**有重复，但你看到的"多"绝大部分不是重复，是"禁止拉伸 + 按主题色/尺寸各烘一张"的代价。**
真正该清的只有 **15 张 / 20KB**。

复现命令（新增的第 5 个体检脚本）：

```bash
python tools/check_res_usage.py       # 汇总 + 孤儿清单
python tools/check_res_usage.py -v    # 连重复组、只在生成器里出现的也列出来
```

## 1. 总量与构成

**903 张 PNG，合计 0.56 MB**（不是体积问题）。

| 族 | 张数 | 构成 | 加载方式 |
|---|---|---|---|
| `ios_rt_*` | **540** | 9 主题色 × **15 种控件尺寸** × 4 变体（普通 / `_p` / `_c` / `_p_c`） | **运行时拼名字**（`src/platform/PgSkin.cpp`） |
| `ios_btn_*` | **144** | 9 色 × (r8/r10/r14/r20 × 普通/按下)，**九宫格 `.9.png`** | **JSON 静态路径**（`ui/*.json` 的 `picTab`） |
| `batt_*` | **33** | 3 色 × 11 档（外壳/电量条/闪电烘在同一张整图里） | **运行时拼名字**（`src/logic/navibar.cc`） |
| `app_icon_*` | 33 | 应用网格图标 | JSON 静态路径 |
| 其余 49 族 | 72 | 图标 / 翻页钟 / 开关等 | 混合 |

> ⚠️ **本表已随 2026-09-16 第二轮改动更新**：电池原来是
> 「外壳 + 电量条（3 色 × 40 档宽度）+ 闪电」= **122 张、3 个控件**，拼名
> `batt_fill_%s_%d.png`；用户要求「做成对应的图片直接贴就不存在错位了」之后改成
> **一枚整图 `batt_%s_%d.png`（33 张）**，见 `docs/battery.md` §3.1 与
> `docs/navibar-battery-appgrid-2026-09-16.md`。`app_icon_*` 也从 27 涨到 **33**
> （应用数 = 图标控件数，加应用要四处同步）。

`.9.png` 共 **150 张（17%）**：`ios_btn_*`(144) + `ios_row_*` / `ios_card_row` / `ios_seg*` / `ios_panel*`。
**它们不是遗留物** —— 光 `ios_btn_select_r14.9.png` 就被 `ui/*.json` 引用 **42 处**。

> 两套机制并存是设计选择：**JSON 路径可以吃九宫格**（一套图适配多种宽度），
> **运行时挂图不行**（`setBackgroundPic` 会把 `.9.png` 的 1px marker 边渲成白线，
> 见 `README` 与 `docs/ui-ios-redesign.md`）⇒ 后者只能按尺寸各烘一张。

## 2. 内容完全相同的重复：77 组 / 156 张 / 46.5 KB

按"族间关系"归类：

| 关系 | 组数 |
|---|---|
| `ios_rt_dark == ios_rt_gray` | 30 |
| `ios_rt_gray == ios_rt_select` | 30 |
| `ios_btn_gray == ios_btn_select` | 8 |
| `ios_btn_dark == ios_btn_gray` | 6 |
| `ios_btn_dark == ios_panel.9` | 1 |
| `ios_btn_dark == ios_btn_gray == ios_panel2(.9 / _c.9)` | 2 |

**根因**：`dark` / `gray` / `select` 这三套主题色的**底色定义恰好相同**（同一档浅灰），
所以烘出来的 PNG 字节级一致。

**结论：不建议删。** 框架**按名字取图** —— `ios_rt_dark_106x68.png` 与 `ios_rt_gray_106x68.png`
必须都存在，删掉任何一个，对应主题的控件就露底。要瘦身只能在代码里做"别名回落"
（读 dark 时回落到 gray），省 46.5KB（squashfs 压缩后更少），**风险收益不成比例**。

## 3. 无人引用的孤儿：15 张 / 20 KB ← 只有这批能删

| 文件 | 同族「在用」的版本 |
|---|---|
| `icon_back_44x44_EEF2F6.png` / `_p.png` | 在用 `F2F2F7` / `FFFFFF` |
| `icon_lock_22x22_9A9AA0.png` | 该族仅此一张（锁图标已换实现） |
| `icon_refresh_222x72_D8E2F0.png` / `_p.png` | 该尺寸仅此两份 |
| `icon_refresh_44x44_D8E2F0.png` | 在用 `F2F2F7` |
| `icon_refresh_56x44_D8E2F0.png` / `_p.png` | 在用 `F2F2F7` |
| `icon_search_222x72_D8E2F0.png` / `_p.png` | 该尺寸仅此两份 |
| `icon_search_44x44_D8E2F0.png` | 在用 `FF9F0A` / `FFFFFF` |
| `icon_search_44x44_F2F2F7.png` | 在用 `FF9F0A` / `FFFFFF` |
| `icon_volume_36x36_5CA8E2.png` | 在用 `0A84FF` |
| `icon_wifi_44x44_0A84FF.png` | 在用 `FFFFFF` |
| `icon_wifi_44x44_5CA8E2.png` | 在用 `FFFFFF` |

**性质**：图标名带**底色 hex**（`icon_<名>_<尺寸>_<底色>.png`）⇒ **改配色 = 新文件名**，
旧文件原地留下且 `ui/*.json` 不再引用它 ⇒ 永久孤儿。典型是"搜索图标从浅灰 `D8E2F0` 改成琥珀
`FF9F0A`""WiFi 图标从蓝 `0A84FF` 改成白 `FFFFFF`"这类改版留下的。
**清掉是安全的**（不涉及代码，不改任何在用的文件）。

## 4. 方法学：为什么第一版结论是错的（★ 下次别重犯）

第一版只做"文件名静态字符串匹配"，报出 **792 个孤儿** —— 与实际（15 个）差 50 倍。
原因：`ios_rt_*`（540）与 `batt_*`（当时 122 张 `batt_fill_*`）是**运行时 `snprintf` 拼名字**加载的，
源码里只有格式串，静态匹配永远找不到完整文件名。

第二版把格式串转成正则作为"动态引用模式"：

```
images/ios_rt_%s_%dx%d%s.png   →   ^ios_rt_[A-Za-z0-9_]*_\d+x\d+[A-Za-z0-9_]*\.png$
images/batt_%s_%d.png          →   ^batt_[A-Za-z0-9_]*_\d+\.png$      （2026-09-16 改版后的拼名）
images/batt_fill_%s_%d.png     →   ^batt_fill_[A-Za-z0-9_]*_\d+\.png$ （改版前的拼名，已废弃）
```

> ⚠️ 改过拼名格式的族要**同步这个模型的来源**：旧格式串若只留在注释/文档里，
> 脚本仍会把它当一条模板去匹配（匹配不到任何文件 —— 无害，但会让人以为"还有这套图"）。

附带踩到第二个坑：**`re.escape()` 在 Python 3.7+ 不转义 `%`**
⇒ 按 `r'\%s'` 做替换永远匹配不到，正则带着裸 `%s` 去匹配文件名 ⇒ 又一次"全都没被引用"。
必须按**裸 `%s`** 替换。

## 5. 建议

1. **现在可以删**：上表 15 张（20KB，一次性的旧底色残留）。删完 `fun pack` + 固化即可，
   不需要改任何代码 —— 因为它们不在任何引用里。
2. **不要动**那 77 组内容重复：按名字取图是框架行为，删了会露底。
3. **以后改图标配色/尺寸后**，跑一次 `tools/check_res_usage.py`，把"无人引用"那批顺手清掉。
4. 若哪天真的要瘦身几百 KB，方向不是删图，而是**减少主题色数量**或**让 `PgSkin` 做别名回落**。
