# 智能家居（HA 遥控器）实现与验收

> **P1 = 只读镜像**（已交付，见 §1~§6，保留作历史记录）
> **P2 = 控制闭环 + 我的设备/添加/改名 + 分类型控制图**（已交付，见 **§7 起**，★ 读这一节）
> 规划与实测依据：`docs/ha-integration-plan.md`（§11）、`docs/ha-verify.md`、`docs/ha-probe.md`

> 需求：「在屏幕产品中新增一个智能家居功能，接入 Home Assistant（HA），实现通过屏幕查看并
> 控制 HA 中的面板与设备，最终把这个产品变成一个 HA 遥控器」。
>
> - 规划与 P0 实测：`docs/ha-integration-plan.md`（**§11 = 真机实测与修订**，必读）
> - 探针报告：`docs/ha-probe.md`、`docs/emqx-probe.md`；验收报告：`docs/ha-verify.md`、`docs/mqtt-watch.md`
> - 本文 = **P1 实现记录 + 真机验收判据**（2026-09-23）

---

## 1. P1 的范围（已交付）

**只读镜像**：看得到 HA 里有什么、现在什么状态、连接健不健。**不含控制**。

```
主界面卡片「智能家居」(slot 37, 系统分类)
   → ha.ftu / haActivity（独立 ftu）
       顶栏（标题由全局导航栏显示）
       状态行：连接状态（左）+ 实体数 · 数据新鲜度（右）
       提示行：一句"该怎么办"（未连接/令牌无效/…）
       实体列表：6 行可滚动，每行 = 名称 + 状态（彩色）+ 「域 · 状态 单位 · entity_id」
       底部：立即刷新 / 重载配置   ·   长按音量键(108) 返回列表
```

## 2. 文件

| 文件 | 说明 |
|---|---|
| `src/platform/PgJson.{h,cpp}` | 极简 JSON 解析（索引式树、支持 `\uXXXX`/代理对）。**不引第三方库**——实测最大报文 14 KB |
| `src/platform/PgHa.{h,cpp}` | HA 客户端：单工作线程 + 命令环形队列 + 实体表快照(generation) + 自带 HTTP/1.1 + 连接状态机 + `/data/ha.conf` |
| `ui/ha.html` → `ui/ha.ftu` | 页面布局（`tools/gen_ui.py` 的 `UI_SOURCES` 已加） |
| `src/logic/haLogic.cc` | 页面逻辑（**替换 fun 生成的脚手架**）+ QA 通道 `/tmp/pg_hacmd` |
| `src/core/PgHaApp.cpp` | 卡片元信息（title/desc/tag/theme） |

**应用注册一共 6 处**（比既有约定多一处，务必全改）：
① `ui/main.html` 的 `Icon37` ② `tools/gen_icons.py` 的 `ICONS` ③ `mainLogic.cc` 的 `kIconCount 37→38`
④ `tools/gen_ui.py` 的 `HIDDEN_CONTROLS range(38)` ⑤ `PgGames.cpp` 的 `kAppTable` 追加
`{"ha", createHa, APP_SYSTEM, 37}` ⑥ ★ **`navibar.cc` 的 `kTitleMap` 加 `{"haActivity","智能家居"}`**
—— 第 ⑥ 处是注释里点名的"**第 5 个同步点**"，我第一版漏了，现象正是它写的"标题停在兜底值"。

## 3. 三条来自实测的设计约束（写进代码注释了，别按直觉改）

1. **灰态是主场景**：实测 34 个实体只有 4 个是真 on/off（`unavailable` 5、`unknown` 12）
   ⇒ 状态列必须能区分，且颜色规则为 **on=绿 / off=灰 / 离线未知=橙 / 数值=青**。
2. **不能靠服务响应回填**（`POST /api/services/...` 恒返回 `[]`，MQTT 实体的状态是异步回包才变的）
   ⇒ P1 点击**只打日志、不发命令、不给"能点"的视觉反馈**；控制留给 P2，且必须
   "乐观态 + 短轮询回读确认 + 超时回滚"。
3. **列表必须排序**：HA 返回顺序里最前面是 `sensor.sun_next_dawn`/`sensor.backup_*` 这类
   HA 自己的内务实体，真正的设备排在第 12 条之后 ⇒ 按"可控 on/off → 可控其它 → 其它 →
   只读传感器"分档 + 档内按名字排（顺序稳定，不会每轮跳动）。

## 4. ★ 真机验收（设备 `20080411` @ 192.168.1.27）

### 4.1 功能判据

| 项 | 判据 | 实测 |
|---|---|---|
| 进页 | 点卡片/`haapp` → 打开独立 Activity | ✅ `autostart cmd 'haapp' -> open haActivity (智能家居)` |
| 读配置 | 从 `/data/ha.conf` 读到 url/port/token | ✅ `PgHa: 配置来自 /data/ha.conf host=192.168.1.188 port=8123 tls=0 poll=3 token长度=183` |
| 连通 | 拿到 HTTP 200 且解析成功 | ✅ `phase=已连接(2) http=200 ver=2026.9.3 unit=°F` |
| 实体表 | 数量正确、解析零失败 | ✅ `实体=26`（HA 返回 34，按域过滤掉 8 个内务实体）`parseFail=0 dropped=0` |
| 轮询 | 每 3 秒一次，generation 递增 | ✅ `实体表已更新 #1…#18`，间隔约 3.1 s |
| 冷启动退避 | 首次超时后要能自己恢复 | ✅ `GET /api/states 失败：connect 超时（累计失败 1 次）` → 之后一直成功（`失败=0`） |
| 离线自愈 | 模拟离线后能回在线 | ✅ `模拟离线 = 1` → `失败=3` → 恢复后 `phase=已连接` |
| 不进屏保 | 进页后 40 s 内 `performScreensaverOn` = 0 | ✅ **计数 = 0** |
| 长按返回 | 长按 108 ≥700ms 回列表 | ✅ `haLogic: 长按 900ms -> 返回列表` + `haLogic: quit` |
| 导航栏标题 | 显示「智能家居」而非兜底值 | ✅ `导航栏: 标题 act=haActivity -> 智能家居` |

### 4.2 ★ 像素级证据：屏幕颜色与 QA 数据逐条一致

抓屏（`tools/grab.py`）+ 像素统计（`tools/shot_stat.py`，**不靠"肉眼看着像"**）：

| 屏幕行 | 状态列主色（量化） | 对应实体（QA `dump`） | 一致？ |
|---|---|---|---|
| 行 0 | 灰 (64,64,64) | `switch.smart_panel_test_ke_ting_deng` = off | ✅ |
| 行 1 | 灰 (64,64,64) | `switch.z20_smart_panel_wo_shi_deng` = off | ✅ |
| 行 2 | 灰 (64,64,64) | `switch.z20_smart_panel_ke_ting_deng` = off | ✅ |
| 行 3 | **绿 (32,192,64)** ≈ #30D158 | `switch.z20_smart_panel_deng_dai` = **on** | ✅ |
| 行 4 | **橙 (224,128,0)** ≈ #FF9F0A | `light.z20_ha_switch_diao_deng` = **unavailable** | ✅ |
| 行 5 | **橙 (224,128,0)** | `switch.z20_ha_switch_dian_nao_kai_guan` = **unavailable** | ✅ |

其余带：标题 8..52 有墨迹 2314、状态行 60..92 有 2006、按钮带左(刷新)=#30D158 / 右(重载)=#2C2C2E。

## 5. ⚠️ 已知问题与待办

| # | 项 | 说明 |
|---|---|---|
| 1 | **顶层色条 y0..8 实测 0 像素** | 被全局导航栏（不透明浮层 0..26）完全盖住。与 `settings.html` 等页同构，**无害但是"死像素"**；要么删掉这个控件，要么接受（导航栏隐藏时它会出现） |
| 2 | 状态行只到"1 分前" | `lastOkMs` 的文案只有"刚刚 / 1 分内"，超过 1 分钟要更细（P2 做） |
| 3 | 配置页 | P1 靠 `/data/ha.conf`（adb push）。屏幕上的配置页 / civetweb 配网页留 P2 |
| 4 | 控制 | 完全留 P2：按 `docs/ha-verify.md` 的结论做"乐观态 + 回读 + 超时回滚"，**灰态禁点** |
| 5 | 只读传感器仍占列表后半 | 26 条里约 13 条是 `sensor`（sun/backup/ulanzi…）。P2 可加"只看可控/收藏"过滤 |

## 6. 复现这套验收（QA 通道）

```bash
# 打开页面（主界面的 autostart 通道）
adb -s 20080411 shell "echo 'haapp #1' > /tmp/pg_autostart"

# 页面自己的通道：/tmp/pg_hacmd（内容变化才执行）
echo 'ping #1'      > /tmp/pg_hacmd   # 请求一次 /api/
echo 'states #2'    > /tmp/pg_hacmd   # 请求一次 /api/states
echo 'dump 60 #3'   > /tmp/pg_hacmd   # 状态+实体清单 → /tmp/pg_ha_dump.txt（**判据落文件**）
echo 'fakeoff 1 #4' > /tmp/pg_hacmd   # 模拟离线（不碰网络）
echo 'fakeoff 0 #5' > /tmp/pg_hacmd   # 恢复
echo 'cfg #6'       > /tmp/pg_hacmd   # 打配置（**令牌只打长度**）
echo 'reload #7'    > /tmp/pg_hacmd   # 重读 /data/ha.conf 并重启客户端
echo 'quit #8'      > /tmp/pg_hacmd   # 退出本页（与返回箭头同一条路）

# 长按返回（设备自带注入工具）
adb -s 20080411 shell "/data/pginj key /dev/input/event3 108 900"

# 像素证据
python tools/grab.py "<工程>/docs/shot_ha_page.png"
python tools/shot_stat.py "<工程>/docs/shot_ha_page.png"
```

> ⚠️ **推送前必看**：adb 上有两台设备时 `fun launch` 会失败
> （`FATAL "host:transport 20080411" FAIL: more than one device/emulator`）。
> 解法 = `adb disconnect 192.168.1.177:5555` → `fun launch -s 20080411` →
> `adb connect 192.168.1.177:5555`。**动别人的设备前必须先问用户。**
> 另：这轮之后 USB 那台有时会从 `adb devices` 里消失，`adb kill-server && adb start-server` 即恢复。

---

# P2：控制闭环 + 我的设备/添加/改名 + 分类型控制图

> 用户需求原文（2026-09-23）：
> ① 「页面点击无效果」② 「界面交互设计应该是显示用户添加过的功能」
> ③ 「扫描出来的内容用户点击可以添加到自己的设备列表，并重命名」
> ④ 「扫描出来的不同类型比如 switch、light 这些就做成不同的控制图片效果」

## 7. P2 交付了什么

| 需求 | 做法 | 关键实现 |
|---|---|---|
| ① 点了要有效果 | 磁贴点击 → `PgHa::reqToggle()` → **乐观态 → 短轮询回读确认 → 5s 超时回滚** | `PgHa.cpp` 的 `reqToggle/ctlTick/completeCallPending`；在途时轮询节拍降到 **250ms** 且走**单实体**接口（几十字节，不是全量 14KB） |
| ② 只显示"我添加过的" | 主页只画 `PgHaList`（`/data/ha_dev.txt`）里的那几台；扫描结果移到「添加设备」页 | `src/platform/PgHaList.{h,cpp}`（原子写：`fsync` + `rename`；坏行跳过并 WARN；id 形状校验） |
| ③ 点一下就加入 + 能改名 | 扫描行点击＝加入；已加入的点行＝进编辑页（改名/删除） | `ui/ha.html` 的 WinHaScan / WinHaEdit；候选名点选（本机无中文输入法） |
| ④ 不同类型不同控制图 | `images/ha_tile_<域>_<on|off|flat>.png`，**13 域 × 3 档 = 39 张**，逐格 `setBackgroundPic` | `tools/gen_ha_tiles.py`（含 4 条自检） |

**为什么是"每格一套独立控件"而不是列表**：实测 `listview` 的 subItem **图片不按行区分**
（运行时 `setBackgroundPic` 之后所有行显示同一张图）⇒ 用列表做不出"每格不同图"。
这跟主界面图标用"21 个重叠控件只显示一个"是同一个原因。

## 8. ★ 三条实测约束（决定了整个控制层怎么写）

1. **不能拿服务响应判成败**。`POST /api/services/...` 恒返回 `[]`，而且**给不存在的实体发命令
   也是 `200 + []`** ⇒ "成功"的定义只能是**回读到目标状态**。
2. **灰态禁点，而且要说清原因**。`unavailable` 的实体命令会被吞（200 但状态永久不变）
   ⇒ 让它可点就等于教用户"这个按钮是坏的"。实测提示行原文：
   `「Z20 HA Switch 吊灯」现在是「unavailable」，设备不在线 ⇒ 先别点（发了也不会变）`
3. **单向触发域不回读**。`scene` / `script` / `button` 的 state **不会**因此变成 on/off
   ⇒ 只判 HTTP 2xx，界面上显示"已发送"。（`cover` 是 `open_cover/close_cover`、
   `lock` 是 `lock/unlock`、`button` 是 `press` —— 发错服务会 400。）

## 9. 真机验收（设备 `20080411`，2026-09-23）

QA 通道扩展（`/tmp/pg_hacmd`，**`#` 之后是注释**）：
`win home|scan|edit` · `page n` · `manage 0|1` · `tap <0..5>` · `toggle <id>` ·
`add/rm/rename <id> [别名]` · `savelist/listload` · `listdump` · 原有 `ping/states/dump/fakeoff/cfg/reload/quit`

| 判据 | 实测 |
|---|---|
| 点磁贴 → 真控制 | `控制: 发出=1 确认成功=1`，`最近控制: 已确认：Z20 Smart Panel 客厅灯 → on（197 ms）` |
| 多轮真实操作 | `发出=9 确认成功=7 **失败回滚=2**` —— 那 2 次回滚正是离线设备，**超时回滚路径被真实触发** |
| 灰态禁点 | `tap 3`（`unavailable` 的灯）→ 控制计数**不增**，提示行给出上面那句原文 |
| 单向触发（button） | `tap 4` → `已发送：Z20 Smart Panel away`，且 HA 侧该按钮 state 时间戳从 `09:02:29` 变为 `09:08:19`（**真按下去了**） |
| 加入 / 改名 / 删除 | 扫描行点击 → `/data/ha_dev.txt` 多一行；改名后列表与磁贴同时变；**中文别名落盘为合法 UTF-8** |
| 管理模式 | `manage 1` + `tap 1` → `窗口=edit`，`编辑目标: switch.z20_smart_panel_wo_shi_deng` |
| 分类型图 | 6 格实际引用：`switch_off` ×3 / `light_on` ×1 / `button_flat` ×1 / `other_flat` ×1 —— **各不相同** |
| 状态同步（活的） | 那台吊灯被打开后，格 3 的图标主色**从灰变成 (168,120,0) 琥珀** = `ha_tile_light_on.png` |
| 定时器存活 | `/tmp/pg_ha_tick.txt` 心跳：`tick=51→76`（10 秒涨 25 拍），`homeSync` 每拍都跑，`ents=26` |
| 屏保 | `performScreensaverOn` 计数 **0**（40 秒闲置后仍是 0） |
| ★ **像素级交叉判据** | `tools/ha_tiles_check.py`：6 格的**渲染主色 == 素材 PNG 主色**，全部一致（含活的 `light_on`） |
| 窗口白名单 | 核对生成 JSON：`WinHaHome visible=True`、`WinHaScan/WinHaEdit visible=False`，三个 `backgroundColor=0`（不透明黑）—— 漏了"隐藏"会开机三页叠在一起、漏了"不透明"会透出桌面 |

## 10. 这一轮修掉的 5 个真问题（都是"看起来对"的静默失败）

| # | 问题 | 现象 | 修法 |
|---|---|---|---|
| 1 | QA 的 `#` 注释**没被剥掉** | `listdump #10` 不被识别；更坏的是 `add <id> #10` 把 ` #10` 存进了设备列表 | `haPollCmd` 先剥 `#`；`PgHaList::add` 加 **id 形状校验**（必须含 `.`、不含空白/`#`） |
| 2 | 实体快照**只在扫描页刷新** | 主页磁贴全显示"找不到 / HA 实体表里没有这个 id"（`我的设备=4` 但 `HA实体=0`） | 抽出 `haSyncEnts()`，每拍都调（主页也要看 state/domain） |
| 3 | `reqToggle` 失败**只有 LOGD** | 现场"点了没反应"时，logcat 缓冲只有十几行、原因那行被冲掉 ⇒ **查不到为什么** | 每条失败分支写 `ctlReject_`，进 QA dump + 显示在提示行；另加 `/tmp/pg_ha_tick.txt` 心跳 |
| 4 | 名字按**字节**截断 | `Z20 Smart Panel away`（20 字节）刚好通过、但 120px 盒里横向溢出（textview 不省略） | 改 `truncNamePx()`：按**像素宽**估算（ASCII 0.55×fs / CJK 1.0×fs） |
| 5 | `tools/grab.py` 内部 `adb` **没带 `-s`** | 两台设备时一律失败，症状却是"adb pull 没落下文件"（像盘满/权限问题） | 自动挑唯一设备；**两台以上必须 `--serial`/`PG_SERIAL`**，否则当场拒（不做"随便挑一台"） |

**方法学教训（值得记）**：
- **别在 QA 命令文件里写中文（用 ASCII 编码推送）** —— 会被写成 `???`。要写中文就用 UTF-8（无 BOM）
  推送。这一条是我自己的测试污染，不是产品 bug，但污染了设备上的设备列表。
- 用户会**同时用手操作设备**（本轮日志里看到他进添加页、点行加入、管理模式点磁贴、长按返回）。
  排障时先把"人机并发"这条排除掉，否则会把用户的正常操作误读成"我的命令没生效"。
- **写中文串里的 ASCII `"` 会截断 Python 字符串**（本轮又踩一次，`grab.py`）；一律用「」。

## 11. 已知问题与待办（P3）

| # | 项 | 说明 |
|---|---|---|
| 1 | 顶层色条 y0..8 是"死像素" | 被常显导航栏盖住（与 `settings.html` 同构）；要么删控件、要么接受 |
| 2 | 状态新鲜度文案很粗 | 只有"刚刚 / 1 分内 / 很久" |
| 3 | 配置页 | 仍靠 `/data/ha.conf`（`adb push`）；屏幕上的配置页 / civetweb 配网页未做 |
| 4 | 改名只能选候选名 | ⚠️ **2026-09-23 更正**：这条的前提写错了 —— 本板**有**中文输入法（`src/logic/imeApp.cc` 是 `APP_TYPE_SYS_IME` 全拼输入法，任何 `ZKEditText` 聚焦自动拉起）。所以"改名只能点候选名"是**当时的实现选择，不是能力限制**。改版方案改为：命名页放 `div.input` 自由输入中文，候选池退化为"按域推荐的快捷词片"（见 `docs/ha-ux-redesign.md` §4.6 与设计稿 `ui/ha.preview.html`）。原型详见 `PgHaList.h` 的 `kAliases[]` |
| 5 | 房间分组 / 实时推送 | 需要 WebSocket（`config/*_registry/list` 只有 WS 有）；P3 |
| 6 | 亮度/色温/调速 | 只有"翻转"，没有滑条（`number`/`select` 域要带值，本期明确不做） |
| 7 | 包体余量 | `resources/images` 4,272,620 字节（1106 个文件），`/res` 上限 7,995,392。新增 39 张图仅 **42,508 字节**（1.1KB/张）⇒ 余量充足；固化脚本自带"出包后校验 ≤ 分区，超了当场停" |

## 12. 复现 P2 的验收

```bash
# 1) 上机（adb 上是两台设备 ⇒ 必须 -s；fun launch 需临时断开 177）
adb disconnect 192.168.1.177:5555
./fun.exe launch -s 20080411           # 或走固化流程 tools/upgrade_device.sh
adb connect 192.168.1.177:5555

# 2) 开页
adb -s 20080411 shell "echo 'haapp #1' > /tmp/pg_autostart"

# 3) QA（★ 多行命令用 adb push 一个文件，别用 echo 拼；中文必须 UTF-8 无 BOM）
printf 'win scan #1\nadd switch.z20_smart_panel_ke_ting_deng #1\nwin home #1\nlistdump #1\n' > cmds.txt
adb -s 20080411 push cmds.txt /tmp/pg_hacmd

# 4) 判据（都在设备上，pull 回来看）
adb -s 20080411 shell "cat /tmp/pg_ha_list.txt"    # 每格 pic/name/state + 控制计数 + 被拒原因 + 提示行
adb -s 20080411 shell "cat /tmp/pg_ha_tick.txt"    # 定时器心跳（tick 必须持续增长）
adb -s 20080411 shell "cat /data/ha_dev.txt"       # 我的设备（别名）

# 5) 像素级交叉判据（渲染主色 vs 素材主色，逐格）
python tools/grab.py "<工程>/docs/shot_ha_home.png" --serial 20080411
adb -s 20080411 pull /tmp/pg_ha_list.txt .
python tools/ha_tiles_check.py docs/shot_ha_home.png pg_ha_list.txt   # 期望：6 格全 ✓

# 6) 看某张控制图标长什么样（本机读不了图时的字符画）
python tools/show_tiles.py light switch fan lock cover climate
python tools/shot_stat.py docs/shot_ha_home.png
```

> ⚠️ `tools/grab.py` 现在**拒绝"随便挑一台设备"**：两台以上必须 `--serial 20080411`
> 或设 `PG_SERIAL=20080411`。这是有意为之（以前会把现场那台 Z20 面板当成验收对象）。


---

# P3（v2 交互改版）：2 列 4 行卡片 + 三步向导 + 输入法命名（已真机验收）

> 2026-09-23 交付。设计稿 `ui/ha.preview.html`，方案 `docs/ha-ux-redesign.md`（方案 B）。
> 本文件是**实现记录**；设计与评审结论看那份文档。

## 13. 改了什么（相对 P2）

| 项 | P2 | P3（v2） |
|---|---|---|
| 主页 | 2x3 磁贴 + 上一页/下一页 | **2 列 × 4 行**（一屏 8 台），容器固定高度 + 滚动，**无翻页** |
| 页头 | 左标题 + 右「设置」 | 左标题 + 右侧并排 **[+] 44x36 ｜ [设置] 88x36**（IoT App 做法），底部 CTA 删除 |
| 连接失败 | 一行小字状态 + 一行提示 | **一张状态卡**：L1 发生了什么(橙) → L2 上下文 → L3 影响 → L4 唯一动作 |
| 空态 | 6 个「空位」磁贴 | **独立空态组**（大图标 + 说明 + 大 CTA + 一键添加），与卡片网格互斥 |
| 添加设备 | 点一下即加入 | **三步向导**：选择（多选 + 底部确认条 + 段控过滤）→ 命名 → 完成 |
| 命名 | 从 33 个候选词点选 | **原生 EditText**（点它自动拉起拼音输入法，可自由输入中文）+ 6 个按域推荐词片（点击=填入） |
| 编辑 | 管理模式 + 点磁贴 | **卡片右上「···」→ 半屏操作菜单**（改名/前移/后移/移除）；管理模式删除 |
| 移除 | 点一下即删 | **二次确认对话框**（"只是从这块屏幕上移除…"） |
| 调试 | 「重载配置」在用户界面 | 收进**设置页**（含连接信息、实体统计、关于） |

## 14. 真机验收（设备 `20080411`，2026-09-23）

**上机方式**：`tools/upgrade_device.sh 1.0.4`（固化；编译 → 出包 → `adb -s` push → setprop 触发刷写）。
判据 `/res/lib/libzkgui.so` 字节数 == 本地 `.fun/v85x/libzkgui.so`（4295544）✅

| 判据 | 实测 |
|---|---|
| 进页 | `autostart cmd 'haapp' -> open haActivity (智能家居)`；`haLogic: 切窗口 -> 我的设备` ✅ |
| 定时器 | `/tmp/pg_ha_tick.txt` 的 `tick` 持续增长（10→32）✅ |
| 构建标签 | QA dump `Build: ha-p3-20260923`（**只有新代码才有的判据**）✅ |
| 卡片渲染 | dump `Card0..2` 有 id/alias/dom/glyph/name/state/rendered；空位 `Card3..7 (空)` ✅ |
| ★ 像素级 | 抓屏逐点：页头 `[+]`/`[设置]` `#2C2C2E`、卡0/卡1 中心 `#1C1C1E`、行缝黑、空态组已隐藏、状态卡已隐藏 → **10/10 OK** ✅ |
| ★ 菜单页像素 | 弹层 396..756 `#1C1C1E`、菜单项 `#2C2C2E`、移除按钮 `#FF453A`、取消 `#3A3A3C`、遮罩压暗 → **6/6 OK** ✅ |
| 离线卡禁点 | `card 0` → hint `「Z20 Smart Panel 客厅灯」离线，先别点（发了也不会变）`（**控制计数不增**）✅ |
| 向导①勾选 | `seg all` + `pick 0` → dump `已选=1`，hint `已选 1 台` ✅ |
| 向导②③ | `next` → `命名台数=1`；`chip 0` + `typedone` → hint `已添加 1 台设备` ✅ |
| 落盘生效 | 我的设备 `3 → 4` 台（回主页后 dump 确认）✅ |
| 设置页 | `win set` → `page=6`，连接信息/实体统计有值 ✅ |

### 14.1 遗留：两个"需要造场景"的状态只验了逻辑、没验像素

| 状态 | 现状 | 怎么补验 |
|---|---|---|
| **连接失败态（状态卡）** | 逻辑已通（`syncOfflineCard` 与"已连接时隐藏"是同一套代码，隐藏方向已捕屏 10/10 验证） | QA `fakeoff 1` **本轮没生效**（dump 仍显示"已连接"）—— 需单独查是 QA 命令还是 `PgHa::fakeOffline` 的行为；也可改用"把 host 指向不存在的地址"造真离线 |
| **空态组** | 逻辑已通（dump 有 `空态=` 字段；有设备时已捕屏确认**正确隐藏**） | 临时备份 `/data/ha_dev.txt` → 逐个 `rm <id>` 清空 → 抓屏验空态 → 恢复备份 → `listload` |

> 两个状态都**不是**"没做"，而是"缺一次造场景的像素验收"。判据本来就在 QA dump 里
> （`状态卡=` / `空态=` 两个字段），补验成本很低。

## 15. 这一轮踩的 5 个坑（都是"看起来对"的静默失败）

| # | 坑 | 现象 | 根因 / 修法 |
|---|---|---|---|
| 1 | ★★ **`fun install` 会往 logic 文件追加回调桩** | 编译报 `redefinition of 'onButtonClick_ChipHaName3'` | 它扫**源码文本**找 `onButtonClick_<caption>`；**宏展开的函数它看不见** ⇒ 判"缺失"⇒ 在文件末尾补桩。**修法：回调必须逐个手写，函数名要在源码里字面出现**（我最初用 `CARD_CB(n)`/`CHIP_CB(n)` 宏，24+6 个回调全被补桩） |
| 2 | ★★ **Git Bash 会把 `/tmp/update.img` 改写成 `D:/Temp/update.img`** | `upgrade_device.sh` 卡住 12 分钟无输出；手敲 adb push 报 `remote No such file or directory` | MSYS 路径转换。**修法：脚本顶部 `export MSYS_NO_PATHCONV=1`**（已修进 `tools/upgrade_device.sh`） |
| 3 | ★★ **显隐"变化检测"的初值 == 控件实际状态 ⇒ 首次同步被跳过** | 抓屏发现空态文字盖在卡片上（`(124,410)` 取到文字色而非卡片色） | `sEmptyShown=false` 与"HIDDEN_CONTROLS 未登记、控件实际可见"组合 ⇒ `if (show == sEmptyShown) return;` 直接返回。**修法：① 两组控件登记进 `HIDDEN_CONTROLS`；② 两个"上次显隐"初值改 `true`**（= 与源稿相反，保证首次 sync 一定执行） |
| 4 | **段控不能用 `setBackgroundColor` 表达选中** | —（预判规避） | 运行时 `setBackgroundColor` 会**清掉圆角九宫格图**（老血案）⇒ 改成**两个按钮叠一格**（未选中态/选中态），切 `setVisible`；**且两个都要挂回调**（上层那个才吃得到点） |
| 5 | **宏参数会污染函数签名** | `error: expected ',' or '...' before numeric constant` | `#define CARDCTRL(i)` 展开时把 `int i` 里的 `i` 也替换成 `0` ⇒ `int 0`。修法：取指针函数**显式写**，不用带参宏 |

## 16. 复现验收（QA 通道）

```bash
adb -s 20080411 shell "echo 'haapp #1' > /tmp/pg_autostart"     # 进页
python tools/grab.py "<工程>/docs/shot_ha_v2.png" --serial 20080411

# 走一遍：菜单 / 向导 / 设置（★ 每条命令内容要不同，"内容不变不执行"）
printf 'win home #1\n'  > /tmp/pg_hacmd ; sleep 2
printf 'menu 1 #2\n'    > /tmp/pg_hacmd ; sleep 2   # 卡片菜单（半屏）
printf 'win pick #3\n'  > /tmp/pg_hacmd ; sleep 2   # 向导①
printf 'seg all #4\n'   > /tmp/pg_hacmd ; sleep 2   # 段控切"全部"
printf 'pick 0 #5\n'    > /tmp/pg_hacmd ; sleep 2   # 勾选
printf 'next #6\n'      > /tmp/pg_hacmd ; sleep 2   # -> 命名页
printf 'chip 0 #7\n'    > /tmp/pg_hacmd ; sleep 2   # 填入推荐词
printf 'typedone #8\n'  > /tmp/pg_hacmd ; sleep 2   # 提交 -> 完成页
printf 'win set #9\n'   > /tmp/pg_hacmd ; sleep 2   # 设置页
adb -s 20080411 shell "cat /tmp/pg_ha_list.txt"     # 判据落文件
```

> ⚠️ **用户会同时用手操作设备**：本轮 dump 的 `page=` 值出现过漂移（有人点了屏幕），
> 排障时**别拿 page 当唯一判据** —— 用"命令的副作用"（hint 文案 / 已选数 / 我的设备数）更可靠。

> 验收过程用 QA 往「我的设备」里**加了 1 台设备**（3 -> 4 台），这是真实用户数据变化 —— 不需要的话在卡片菜单里「移除设备」即可。

---

# P4（2026-09-24）：我的设备改 **2 列 listview** + 修掉"图标黑角" + 切图审计

> 用户口径原文：「我的设备页面，采用 listView 实现，没有那么多 icon 就不要显示出来，
> 显示实际有的就可以了。然后你（贴）的图标角落都是黑色的，你有没有发现？把这些都修改掉，
> 再仔细检查一遍，所有切出来的图片。」

## 17. 三个诉求 → 同一个根因

**根因**：P3 的主页是「8 张**固定卡片**，每张 6 个独立控件」。两个后果：

1. 设备不满 8 台 ⇒ 露出**空卡片 + 空域块**（用户说的"那么多 icon"）。
2. ★★ 每个"域标识底块"（`Dic0..7`，`data-bg=#2C2C2E`）会被 `tools/gen_ui.py` 的
   `inject_rounded()` **挂上圆角九宫格**（`ios_btn_gray_r14.9.png`），
   并把 `bgColorTab` 清成 **-1** ⇒ 圆角四角透出的是**窗口黑底**，
   而底块坐在 `#1C1C1E` 的卡片上 ⇒ **四个纯黑角**。
   （真机逐点取色：四角 `(0,0,0)`，卡片 `(28,28,30)` —— 用户看到的"黑角"就是它。）

**改 listview 同时解决这两件事**：行数 = 实际设备数；而 **listview 的 subItem
不挂圆角图、`data-bg` 还会被 html2json 丢掉** ⇒ 图标直接画在行底上，天生没有黑角。

## 18. 改了什么

| 项 | P3 | P4 |
|---|---|---|
| 主页容器 | 8 张固定卡片（48 个控件） | **1 个 2 列 listview**（`ListHaMy`，cols=2 / colSpacing=8 / rowSpacing=8，item 224x148）+ 6 个子项 |
| 行数 | 永远 8（空位也画） | **= 实际设备数**（0 台就一行都不画） |
| 域标识 | `data-bg` + 九宫格圆角图 ⇒ **黑角** | `data-bgpic="ha_domchip_64.png"`（**不透明**、四角烘行底色 #1C1C1E） |
| 行尾菜单 | 独立控件 `Mn0..7`（36x30，被 `···` 撑成 55 宽并越界压到隔壁卡） | 子项 `SubMyMenu`（42x34，文字改 `•••`），点击靠 **子项 id** 区分 |
| 名字截断 | 按**头**截断 ⇒ `Z20 Smart Panel 灯带` 显示成 `Z20 S`（信息量为 0） | **优先保留尾部**（含空格取最后一段 / 否则取最长可放下后缀）⇒ `灯带` |
| 绿主按钮字色 | 白字（源稿写 `#000000`，被 html2json 当"未设置"换成 `#EEF2F6`） | `#010101` ⇒ 对比 1.44:1 → **28.5:1** |
| 空态大图标 | `#48484A` on `#1C1C1E` = **2.08:1**（肉眼几乎看不见） | `#30D158` ⇒ ~3.9:1 |
| 段控 | 四个按钮的可见性**写成了同一个条件** ⇒ 左段整块消失、右段两个叠着 | 两两互斥（灰态/蓝态）+ 选中态文字用对分支（`可控 N`） |
| 设置页 | 只在"按设置按钮"时填值 ⇒ 别的路径进来是空页 | `showPage(PG_SET)` 时 `syncSetPage()` |
| 离线态 | 列表不让位 ⇒ **第一行被状态卡压掉 42px**；提示行被卡盖住 ⇒ 临时提示**看不见** | 列表 `setPosition` 下移到 212；离线时把临时提示写进**卡内 L3**（6 秒后恢复设计文案） |

## 19. 真机验收（设备 `20080411`，固化包 md5 与本地一致）

| 判据 | 实测 |
|---|---|
| ★ 黑角 | 4 个域块（含外圈）四角全部 `(28,28,30)` = 卡片色 ✅（改前 `(0,0,0)`） |
| ★ 行数=实际设备数 | 4 台 ⇒ 2 列 × 2 行（`SubMyDic#0..#3` 绝对坐标全对）；**清空后列表区 0 行**（13,706 个卡片色像素里 12,939 个是空态图标自己）✅ |
| ★ 子项 id 派发 | **触摸注入**点行尾 `•••`（209,181）⇒ `page=4` + `菜单idx=0`（开菜单）；点行本体（124,240）⇒ 走控制路径并给离线提示 ✅ |
| 名字截断 | `Card2 rendered=[nm=灯带 …]`（别名空时不再显示 `Z20 S`）✅ |
| 段控 | 默认 左蓝右灰 / `seg all` 左灰右蓝 / 切回 左蓝右灰 ✅ |
| 绿按钮对比 | 完成页 CTA 底 `(48,209,88)` 字 `(1,1,1)` = **28.51:1** ✅ |
| 设置页 | 走 QA 路径进来三行信息都有墨迹（349/312/342 px）✅ |
| 离线态 | 状态卡 104..200 显示；列表卡片色起点 **213**（在线 159）✅；`fakeoff 0` 后自愈回 159 ✅ |
| 空态 | `空态=1` + 空态组可见 + 主 CTA 绿 ✅ |

> 验收过程用 QA 清空过设备列表，已用 `/data/ha_dev.txt` 备份**原样还原**（4 台，别名/顺序一致）。

## 20. 新增工具（都可复用）

| 工具 | 作用 |
|---|---|
| `tools/ha_ui_review.py` | **设计规格 × 真机像素**逐控件体检：底色/墨迹 bbox/垂直居中/贴边裁切/空控件/控件相交/对比度；支持 `--item-rows N` 把 listview 子项摊成绝对坐标、`--dy/--dy-from` 处理"离线态网格下移"、`--icon <caption>` 出字符画看图标外形 |
| `tools/check_ha_assets.py` | **切图审计**：逐张查"圆角透明 + 坐在卡片上 = 黑角"、运行时改色的图带 alpha、1:1 拉伸、引用与磁盘对不上、白占空间的素材 |
| `tools/gen_ha_ui_art.py` | 新增 `ha_domchip_64.png` / `ha_domchip_40.png`（**不透明**、四角烘行底色；字色/底块色是唯一真值），自检 5/5 |

## 21. 本轮踩的坑（都已修）

| # | 坑 | 现象 / 根因 | 修法 |
|---|---|---|---|
| 1 | ★★ **圆角九宫格的透明角透的是"窗口黑底"，不是下面的卡片** | 域块四角 `(0,0,0)` 坐在 `(28,28,30)` 卡片上 = 黑角 | 坐在卡片上的圆角块必须用**不透明 1:1 PNG**（四角烘容器色）；或改用 listview 子项（不挂圆角图） |
| 2 | ★★ **本板 shell 没有 `printf`** | `printf 'cmd' > /tmp/pg_hacmd` **先把文件截空**再报 not found；而我只捕获 stdout ⇒ **静默失败**，命令一条没执行，误判成"画面冻结/应用卡死"（白查半小时） | 一律用 `echo` 写 QA 文件；**执行外部命令时 stderr 要一起看** |
| 3 | **`···`(U+00B7×3) 触发 html2json 最小尺寸公式** | 算成"需要 55px 宽"⇒ 左列 `Mn0/2/4/6` 被撑到 55（越出卡片 13px、压到隔壁），右列因会超出屏幕而保留 36 ⇒ **左右不对称**；生成日志只在"会超出容器"时告警，撑宽的那几个**静默** | 图标类文字别用会被算宽的字符（改 `•••`）；**listview 子项不受该公式约束** |
| 4 | **本板 shell 没有 `which`；`chmod +x` 在 /tmp 失败** | `pginj` 放 /tmp 报 `Bad mode` + Permission denied | 触摸注入工具放 **`/data/pginj`** 再 `chmod 755` |
| 5 | **html2json 把"纯黑 0"当"没设置"** | 源稿 `data-color="#000000"` 的绿按钮落地变成 `#EEF2F6`（近白）⇒ 绿底白字 1.44:1 | 写 `#010101` |
| 6 | **周期性布局会让"错误偏移"也通过** | 我用 `--dy 54` 假设离线时网格下移，其实是**没下移**（卡片间距 156 vs 148，错位 54 后取样窗口仍落在卡片上 ⇒ 误判"通过"） | 判据要挑**非周期特征点**（改用"卡片色起点 y"这种单一判据） |
| 7 | **改 listview 后行缓存上限不能写死 8** | `kCardCount=8` 会让第 9 台以后**永远不刷新** | 改成 `pg::kHaListMax` |

## 22. 已知遗留

- `•••` 在 fs15 下墨迹只有 **30×2 px**（`•` 在本字库里偏小）⇒ 视觉偏细。若要更清楚的"更多"图标，
  建议出一张 42x34 的三点 PNG（不透明、四角烘行底色）替换文字。
- 名字盒 88px 与菜单热区 42px 之间**矩形相交 4px**（实测墨迹没碰到，仅是矩形相交）；
  要把名字盒收窄到 84px 才彻底不相交（需重建+固化一次）。
- **39 张 `ha_tile_<域>_<状态>.png` 已无引用**（P2 的产物；`PgHa.cpp` 的 `tilePicName()`
  现在没有任何调用点），合计 **42.3 KB**。`/res` 分区只剩 ~3.5 MB 且素材区紧张，
  确认不再用可删（`tools/check_ha_assets.py` 会列出它们）。

---

# P5（2026-09-24）：用户报的 4 个问题（列表横线 / 弹框黑角 / 弹框背景 / 命名确认键）

> 用户口径原文：「我的设备列表背面有两条横线。设备的更多信息里面弹框的改名，删除设备这些图标按键
> 还有黑角。弹框的时候背景的列表可以让他在那里显示，弹框全屏半透80%的透明度遮住。
> 命名与排序中命名后没有确认按键。」
> （两条歧义我用选项问过并拿到明确答复：横线 = **卡片行与行之间的缝隙**；遮罩 = **80% 不透明**。）

## 23. 四条问题 → 根因与改法

### ① 列表"两条横线" = 卡片行间的 8px 黑缝 **＋ `.9.png` 的 1px marker 黑边**

逐像素定位（真机抓屏）：卡片行底 = `data-bgpic="ios_btn_dark_r20.9.png"`。
两个叠加原因：

1. `data-row-spacing="8"` ⇒ 两行卡片之间有 8px 黑缝（3 行卡片 = 2 条缝，正是用户说的"两条"）；
2. ★★ **`.9.png` 经 `data-bgpic` 走的是普通贴图路径、不剥最外 1px marker**，而这张素材的
   marker 是**不透明黑 (0,0,0,255)** ⇒ 每张卡片四周还多一圈 1px 黑边
   （实测 x=12 / x=235 是 `(0,0,0)`，卡片本体从 x=13 起）。上下相邻时两条 marker 叠成 **2px**。

改法（两条一起）：
- `data-row-spacing` 8 → **0**，item 高 `616/4 = 154`（框架按 `(h-(rows-1)*rowSpacing)/rows` 算）。
  实测接缝处最窄 192/224 = 86%（圆角收腰），**没有任何一行是全黑** ⇒ 横线消失。
  （试过 2/4px：缝变成细线、更像线；试过 16px+：要压小卡片高度且仍是一条带。）
- 卡片底换 **1:1 普通 PNG** `ha_card_my.png`（224x154，圆角 20，圆角外烘列表黑底；
  选择页的行底同理换 `ha_row_pick.png` 456x72）。marker 问题、拉伸问题一起消失，
  而且这本来就是本工程的规矩（**素材 1:1、不拉伸**）。

### ② 弹框内的按键"还有黑角" = 圆角九宫格的四角透出**窗口黑**

（与 P4 主页域块同一个病）`inject_rounded()` 挂圆角图时会把 `bgColorTab` 清成 -1，
而 html2json 对"有图的按钮"**干脆不写 bgColorTab** ⇒ 四角是黑的。

改法：在 `tools/gen_ui.py` 加一张声明表 **`CARD_FILL`**（caption → 容器色）+
`patch_card_fill()`，**在 `inject_rounded()` 之后**把 `bgColorTab` 补成**容器色**：
- ha：`BtnHaOffSet` → `#2C2C2E`（状态卡上）；`BtnHaMenu*` / `BtnHaCf*` → `#1C1C1E`（弹层上）
- 顺带同病的：`main.html` 的 `BtnCastStop`、`wifi.html` 的 `BtnPwd*`、`camera.html` 的 `BtnCamPwd*`
  —— 全工程共 **13 个**（`tools/check_rounded_on_card.py` 是那份普查清单）。
> ⚠️ 补的是**容器色**，不是按钮自己的色 —— 填自己的色会让圆角看起来变成方的
> （html2json 源码注释里说的"效果错乱"）。

### ③ 弹框时背景列表要留着 + 全屏 80% 遮罩 → 弹层改用 **modal**

原来弹层是普通 `window`，走 `showPage()` 的"七收一放"⇒ **主页被 `hideWnd()` 收起**，
弹层后面只剩自己那层黑底（用户看到的"背景没有列表"）。

改法三条：
1. `ui/ha.html` 的 `WinHaMenu` / `WinHaConfirm` 改 `class="modal"`
   （modal 是**画在当前窗口之上**的，父窗口保持可见 —— 本工程已有先例：`main.html` 的 `WinPause`）；
2. 二者**从 `gen_ui.py` 的 `OPAQUE_WINDOWS` 移除**（html2json 对 modal 给的是
   `backgroundColor=-1` 透明 ⇒ 遮罩下面能看见列表）；
3. 遮罩 `sheet_mask.png` / `dialog_mask.png` 的 alpha 148/168 → **204（不透明度 80%）**，
   尺寸仍是 480x748（导航栏之下满屏；盖到导航栏会被 `gen_ui` 的 SLIVER 检查判"被浮层切成半截"）。
4. `showPage()` 里加：弹层（`PG_MENU`/`PG_CONFIRM`）显示时**不收起背景页**
   （新增 `sBasePage` 记住最近一个非弹层页，先放它、再放弹层）。

真机实测（背景 = 我的设备列表）：
- 遮罩下的卡片 `(5,5,6)` = 卡片色 `(28,28,30) × 0.2` ✓ **列表隐约可见**（正是"80% 不透明"）
- 弹层本体 `#1C1C1E`、四个菜单项四角全是 `(28,28,30)` ✓ **无黑角**
- 触摸注入：点菜单项 → 生效；点遮罩空白 → 关闭；菜单→确认→取消 全通 ✓
  （modal 会拦"弹窗之外"的输入，对弹框来说正是我们要的）

### ④ 命名页"命名后没有确认按键"

机制：自定义 IME 只在**键盘自己的「完成」键**上把整串写回输入框
（`doneIMETextUpdate` → 框架 `setText` → `onEditTextChanged_EditHaName`），
页面原来**没有任何确认键**（唯一可点的"跳过"= 全部不改名）⇒ 打完字就卡住。

改法：
- 页头加 **`BtnHaNameOk`（绿色「完成」88x36 @288,58）**：把当前输入存下 → 下一台
  （最后一台就进完成页）。y<372 是硬约束（键盘占 372..800）。
- `onEditTextChanged_EditHaName` 不再是空壳：把提交的串存进 `sNameTexts`，
  并在**本页可见**的 `TextHaNameStep` 上给青色提示「X」已上屏 · 点右上「完成」记下这一台。
  - ★ 这里踩到两个坑：① `hintSay()` 写的是**首页**的提示行，在命名页上**根本看不见**
    （跨窗口）= 静默失败 ⇒ 反馈必须写本页控件；② 程序化 `setText`（翻页/词片/清空）
    会**同步**触发同一个回调 ⇒ 加 `sNameSilentSet` 标志挡掉（wifi 那边踩过同样的坑）。
- 不再"自动跳下一台"（会与页级「完成」撞车一次跳两台）；自动推进只由按钮负责。

## 24. 验证（真机 `20080411`，固化包 1.0.9）

| 判据 | 实测 |
|---|---|
| 行缝 | 卡片区（y159..465）**整行无卡片色的行数 = 0** ✅；接缝最窄 192/224 |
| marker 黑边 | 卡片左边缘 x=12 即卡片色（改前 x=12 是 `(0,0,0)`）✅ 选择页行同理 |
| 弹框黑角 | 菜单 4 个按钮 + 确认框 2 个按钮的**四角 = (28,28,30)**（容器色）✅ |
| 弹框背景 | 遮罩下背景卡片 `(5,5,6)` = 卡片色×0.2 ✅（列表可见） |
| 弹框交互 | 触摸注入：菜单项生效、遮罩点击关闭、菜单→确认→取消 全通 ✅ |
| 命名确认键 | 「完成」按钮渲染为绿 `(48,209,88)`；点两次 = 命名 2 台 → 完成页 ✅ |
| 命名反馈 | 键盘「完成」上屏后，命名页提示行出现青色字（宽松容差命中 429 px）✅ |
| 反馈复位 | 点页级「完成」翻到第 2 台后，提示行回到灰色（程序化 setText 未误触发）✅ |
| 输入框白边 | 改前 y=172/227 是 **255,255,255** 白线；现在 y=172 = `(1,1,1)`（`data-bg="#010101"`）✅ |
| 主页回归 | `ha_ui_review.py`：45 个控件 **0 问题**；`check_ha_assets.py` **PASS** ✅ |

> 测试期间用 QA 动过用户设备列表（临时移除/添加/改别名），**已用 `docs/_ha_dev.bak` 原样还原**
> （4 台、别名与顺序一致），并核对过 `/data/ha_dev.txt` 内容。

## 25. ★★ 本轮最大的坑：连着刷两次固化 ⇒ mtd3 的 squashfs superblock 丢了

现象：1.0.6 成功后**几分钟内**又跑 1.0.7；脚本报"固化完成、/res 挂载正常"，
但几分钟后设备**自己重启**（`/proc/uptime` 只有 21s），起来后 **`/res` 是空的**
（`ls /res/lib` = No such file）⇒ 应用起不来、QA 无响应。

根因：mtd3（`res` 分区）的 squashfs superblock 读不到（脚本里 2026-09-20 就记过这条血案：
`SQUASHFS error: Can't find a SQUASHFS superblock on mtdblock3`）。
**触发条件是"写入期间/刚写完就重复触发或整机重启"** —— 我这次是**连续两次固化**。

处置：**重跑一次 `tools/upgrade_device.sh <版本>` 即恢复**（脚本自己"卸载→擦→写→挂回"，
实测一次成功）。**绝不手工 mount / dd 回写**（工程红线）。

⇒ **纪律（写进项目 MEMORY）**：一轮改动**攒齐再一次固化**；两次固化之间至少隔开，
别"改一点刷一次"。

## 26. 遗留

- `•••` 菜单热区在 fs15 下墨迹仍只有 30×2px（本字库 `•` 偏小）⇒ 想要更清楚就出一张
  42x34 的三点 PNG（不透明、四角烘行底色）。
- 名字盒 88px 与菜单热区 42px **矩形相交 4px**（墨迹实测没碰到）。
- **39 张 `ha_tile_<域>_<状态>.png` 仍无人引用**（45.6 KB，P2 遗留）。

---

# P6（2026-09-24 第二轮）：**黑色倒角**根治 + 恢复列表上下间隔

> 用户口径：
> ① 「（贴图）这个就是黑色倒角了。你弄个脚本检讨一下 resources 目录下的资源。」
> ② 「我的设备中间设备列表的上下间隔没有了，看起来更难看了。」
>
> ⇒ 交付：`tools/audit_resources.py`（resources 全量审计）、**黑倒角根治**（全工程按钮）、
> 列表行距 0 → 12。设备 `20080411`，固化包 `1.1.0`。

## 27. ★★ 黑倒角的真因（三层叠在一起，前两轮的修法都只治了表皮）

| 层 | 事实 | 前一轮怎么误判 |
|---|---|---|
| ① | **"卡片包着按钮"在 JSON 里不是父子关系** —— `html2json` 的 `add()` 只把控件写进"**栈顶容器**"，而容器栈只为 `window` / `listview` / `radiogroup` 开 ⇒ **控件的父节点永远是容器**；同一个窗口下的控件（含 listview 的 `subItem`）全在**同一层并列**（`button__66`/`button__67`…）。<br>★ 2026-09-30 更正：原文写"本工程的控件树是平铺的"**过满了** —— 树是有的（`window → 控件`、`listview → item → subItem` 都是真层级），只是**控件之间**没有层级。实测 `ui/*.html` ↔ `ui/*.json` 的控件父子关系 **265/265 完全一致**，转换器没有压平。归属 = **我们工具链 html2json 的容器模型**。 | `inject_rounded` 原用"递归往下传 `on_card`"判"按钮是否坐在面板上" ⇒ **从来没生效过**，所以 `_c`（面板底）变体全工程一个都没被选上 |
| ② | **圆角素材的圆角外是"烘不透明黑"**（`_bake_and_round` 把底烘成 `BG=#000000`） | P4/P5 以为"圆角外是透明的、露的是 `bgColorTab`" ⇒ 于是去补 `bgColorTab`（`CARD_FILL`）——**只能修最外 1px 那一圈**，里面 4~5px 的黑**原样还在**（这就是用户"还有黑角"的来源） |
| ③ | 圆角外那一圈到底显示什么 = **控件自己的 `bgColorTab`**；html2json 对"有图的按钮"**故意不写**它，`inject_rounded` 又清成 `-1`(=黑) | ⇒ 不管坐在什么上，圆角外永远是黑的 |

**真机坐实（改前）**：菜单「改名」按钮（容器 `#1C1C1E`）左上角区 `(30..80,470..530)` 里
有 **4~5px 宽的 L 形纯黑**（`y=475..481, x=37..42` 全 `000000`），而按钮本体是 `#2C2C2E`。

## 28. 修法（三层一起）

### ① 素材侧：静态九宫格套**改烘透明底**（`tools/ios_theme.py`）
```python
STATIC_BAKE = None      # 静态套（ios_btn_* / ios_panel* / ios_seg* / ios_card_row*）烘透明底
BAKES = {"": "BG", "_c": "SURFACE"}   # 只留给运行时那套 ios_rt_*
```
+ `gen_buttons` **不再出 `_c` 变体**（那 26 个文件已删）—— 圆角外交给 `bgColorTab` 之后，
  一种素材就适配任意容器色，不需要按容器色各出一套。
+ `btn_asset_for()` 的 `on_card` 参数**保留但不再影响结果**（容错调用方）。

**为什么"透明底"在本工程是安全的**（三条实测证据，不是推测）：
1. `ui/main.html` 的桌面图标（`app_icon_*.png`，圆角外 `α=0`）走 JSON 路径渲染，角上是**纯黑**=页面色，不是白；
2. ha 完成页 `ha_done.png`（绿圆，圆角外 `α=0`）同结论；
3. 最强：半透明遮罩 `sheet_mask.png`（`α=204`）挂在 picTab 上，实测背景被压成 `原色 × 0.2`
   ⇒ **JSON 路径确实做 alpha 合成**（`ios_theme.py` 的老注释也写着"JSON 里写 picTab/backgroundPic → alpha 正确"）。
⚠️ 只有**运行时 `setBackgroundPic()`** 那条路不保留 alpha ⇒ `ios_rt_*.png` **必须继续烘不透明底**（已分离，互不影响）。

### ② 逻辑侧：`bgColorTab` 填**所坐容器的底色**（`tools/gen_ui.py`）
- 新增 `parse_caption_fill()`：从源稿读 `caption → 容器色`（非黑 `data-bg`；`data-round` 按 `ROUND_FILL` 取面板本体色）。
- `inject_rounded()` 重写：**按几何包含**求"坐在谁上面"（在所有"声明了有色底"的控件里，
  取矩形完整包含它且面积最小的那个），把 `bgColorTab` 填成**容器色**；容器是页面/窗口 ⇒ 填 `-1`。
  - 同时给"挂在有色容器上的 `backgroundPic`"补同一套（受益者：probe 页那些坐在彩色按钮上的 icon）。
- 删掉 `CARD_FILL` 白名单与 `patch_card_fill()` —— **白名单必然漏**，源稿 + 几何包含才是完备判据。
- 刻意**不填按钮自己的色**：那会让圆角被填成方的（html2json 源码注释里的"效果叠色错乱"）。

### ③ 验收侧：新增 `tools/audit_resources.py`（resources 全量审计）
| 节 | 内容 |
|---|---|
| 【1a】 | 全量素材"角行为"普查：**本色角 / 透明角 / 死黑角**（对**内容区**取样，`.9.png` 跳过最外 1px marker） |
| 【1b】 | ★ **座位分析**：坐在非黑容器上的挂图控件，**校验其 `bgColorTab` 是否 == 容器色**（直接验落地结果） |
| 【1c】 | `.9.png` 被 `data-bgpic` 引用的清单（历史写法提示） |
| 【1d】 | `.9.png` marker 规范：拉伸标记必须**连续一段、居中、纯黑、不压圆角** |
| 【2】 | 1:1 铁律（普通 PNG 尺寸必须 == 控件尺寸） |
| 【3】 | 运行时 `setBackgroundPic` 用到的图必须不透明（已剥注释，避免把注释里的示意名当真） |
| 【4】 | 未被引用的素材（按"前缀族"识别运行时拼名，避免误报） |

## 29. ★ 恢复列表上下间隔（用户口径②）

`ListHaMy` 的 `data-row-spacing` **0 → 12**；item 高 = `(616 - 3×12) / 4` = **145**
⇒ `ha_card_my.png` 同步改成 224x145（1:1 铁律）。

当初改 0 是为了消"两条横线"，但那两条是 **8px 行缝 + `.9.png` 的 1px marker 黑边** 叠出来的；
marker 那条已由"卡片底换 1:1 普通 PNG"根治，剩下的"缝"本身就是**正常的卡片间距**
（卡片四角是圆的，缝在圆角处收腰 ⇒ 读起来是两张卡片，不是一条线）。

## 30. 真机验收（`tools/upgrade_device.sh 1.1.0`）

| 判据 | 改前 | 改后 |
|---|---|---|
| ★ 弹层「改名」按钮左上角 8x8（容器 `#1C1C1E`） | `000000` 的 L 形黑，4~5px 宽 | `1C1C1E → 212123 → 262628 → 2C2C2E` **平滑过渡，近黑像素 0 个** ✅ |
| ★ 离线卡「去设置」按钮角（容器 `#2C2C2E`） | 纯黑角 | `2C2C2E → 333335 → 3A3A3C` 平滑 ✅ |
| 列表行几何（`x=228`） | item 154、缝 0 | 卡片 135+2×5(圆角切) = **145**，缝 12（量到 19 = 12+两侧圆角切） ✅ |
| 选择页 | 行底 `#1C1C1E` | 同；**左边缘 x=12..17 全是 `#1C1C1E`**（不再有 marker 1px 黑边） ✅ |
| 完成页 | — | 绿圆角=黑(页面)、绿 CTA 角=黑，**全页纯白像素 745（都是文字/勾）** ✅ |
| 回归 | — | 启动器画面正常（纯白 12250 = 图标字形，无角漏白） ✅ |

**离线段自检（`audit_resources.py`）**：
`【1b】检查 373 个"坐在非黑容器上的挂图控件"：正确 373，会显黑 0` ✅
`【1d】78 张 .9.png marker 全部合规` ✅ ；`【2】1:1 正确 98 处，不符 0` ✅
`check_assets.py`（老的六项边缘体检）**全部通过** ✅

## 31. ★★ 又踩了一次 mtd3：固化后设备自己重启，`/res` 空

现象（与 P5 同源，这次是在**没有手动 reboot** 的情况下复现的）：
固化脚本报"完成、`/res` 正常、pid 变了"，**约 3 分钟后设备整机重启**（`/proc/uptime` 归零），
起来后 `/res` 没挂上（`cat /proc/mounts` 里没有 mtd 条目）、应用起不来、屏幕是白底启动画面。

**恢复**：重跑一次 `tools/upgrade_device.sh 1.1.0`（它自己"卸载→擦→写→挂回"）——**一次成功**，
之后 uptime 持续增长、/res 里能看到新素材。（绝不手工 mount / dd 回写 —— 工程红线。）

⇒ 纪律更新：**固化完成 ≠ 结束**。固化后**再等 3~5 分钟确认设备没有自己重启、`/res` 仍在**，
才算落地；发现 `/res` 空就**原样重跑一次脚本**。

## 32. 新增/更新的工具与纪律

- 新增 `tools/audit_resources.py`（★ resources 全量审计，见 §28③）。
- `tools/check_assets.py`：④ 增加"**静态九宫格套（烘透明底）豁免**"判据；① 白名单补 `EditHaName`。
- `tools/check_rounded_on_card.py` / `check_ha_assets.py`：**标注已被 `audit_resources.py` 取代**
  （它们基于"补颜色就能消黑角"的旧判据，会误导后人）。
- `tools/ios_theme.py` / `tools/gen_ui.py` / `tools/gen_ha_ui_art.py`：见 §28。

## 33. 遗留
- `•••` 菜单热区墨迹仍只有 30×2px（本字库 `•` 偏小）⇒ 建议出 42x34 三点 PNG。
- 名字盒 88px 与菜单热区 42px 矩形相交 4px（墨迹没碰到）。
- 未引用素材：`audit_resources.py` 报 **65 张 / 44.4 KB**，`check_res_usage.py` 报 **24 张 / 27 KB**
  （两者判据不同，前者更严）。**都还没删** —— 要清的话先确认（可能是别处的备用变体）。
