# 接入 Home Assistant —— 实施规划（2026-09-23）

> 需求原文：「在屏幕产品中新增一个智能家居功能，接入 Home Assistant（HA），实现通过屏幕查看并控制
> HA 中的面板与设备，**最终把这个产品变成一个 HA 遥控器**。」
>
> 本文是**立项前的完整规划**（不含代码）：对接方式 → 功能范围 → UI/交互 → 架构与模块 → 分阶段步骤 → 风险。
> 口径与既有约定一致：**每片都要真机验收**，验收判据落文件，不做"编译通过就算完"。

> ## ⚑ P0 前置验证**全部完成**（2026-09-23 实测，含控制闭环）
> HA `2026.9.3` @ `192.168.1.188:8123`、EMQX `5.8.7` @ `:18083` 已实测接入；
> **设备侧（板子 `20080411`，`192.168.1.27`）到 HA 的 HTTP 通路已打通**（拿到 HA 的 401 受控应答）；
> **控制闭环已真机验收**：命令 → 观测到状态变化 **156~210 ms**，离线设备 5 s 超时且状态不变。
> **P0 的 6 个待拍板问题全部解掉**，并推翻了四条原本写死的假设（最重要的一条见 §2.4）。
> - 实测报告：[`docs/ha-probe.md`](ha-probe.md)、[`docs/emqx-probe.md`](emqx-probe.md)
> - 验收报告：[`docs/ha-verify.md`](ha-verify.md)、[`docs/mqtt-watch.md`](mqtt-watch.md)、[`docs/toggle-once.md`](toggle-once.md)
> - 工具：`tools/ha_probe.py`、`tools/emqx_probe.py`、`tools/ha_verify.py`、`tools/ha_toggle_once.py`、`tools/ha_set.py`、`tools/mqtt_watch.py`
>
> **细节与规划修订见 §11**（★ 若只读一节，读 §11.3 与 §11.7）。
>
> ### ⚑ P1（只读镜像）已实现并真机验收通过（2026-09-23 傍晚）
> 新增应用「智能家居」`slot 37` / `ha.ftu` / `haActivity`；`PgJson` + `PgHa` 落在
> `src/platform/`；页面 `ui/ha.html` + `src/logic/haLogic.cc`。
> **实现记录与全部判据 → [`docs/ha-app.md`](ha-app.md)**（含"加独立 ftu 应用要改 6 处"、
> 像素级证据、以及 `fun launch` 在多设备下的正解）。
>
> ### ⚑ P2（控制闭环 + 我的设备/添加/改名 + 分类型控制图）已实现并真机验收通过（2026-09-23 晚）
> 用户四条需求（点击无效 / 只显示添加过的 / 扫描结果可点加入并改名 / 不同类型不同控制图）全部落地：
> - **控制**：`PgHa::reqToggle` = 乐观态 → 短轮询回读确认 → 5s 超时回滚（**实测 197~210ms 确认**；
>   真实操作中已触发过 2 次回滚 = 离线设备）；灰态**禁点并把原因写在屏幕上**。
> - **我的设备**：`src/platform/PgHaList.{h,cpp}` → `/data/ha_dev.txt`（原子写）。
> - **分类型图**：`tools/gen_ha_tiles.py` 生成 **13 域 × 3 档 = 39 张**（仅 42.5KB），
>   逐格 `setBackgroundPic` —— **这是唯一能"每格不同图"的写法**（listview 行图片不分行）。
> - **实现与验收全部细节 → [`docs/ha-app.md`](ha-app.md) §7 起**（★ 只读这一节就够）。

---

## 0. 结论先行

**做得了，而且这台机器很适合当"遥控器"，但不适合当"第二个 HA 前端"。**

| 判断 | 依据 |
|---|---|
| ✅ **能做的** | 设备列表 / 状态镜像 / 开关·亮度·色温·温度·窗帘·场景触发 / 实时状态同步 / 摄像头快照 / HA→设备通知 |
| ❌ **不做也做不了的** | **原样渲染 Lovelace 面板**（HA 前端是个 JS 单页应用，需要浏览器内核；本机只有 EasyUI 原生控件 + 软渲染 Canvas） |
| 关键设计转向 | 「面板」不按 Lovelace 渲染，而是**由 HA 的实体注册表（房间 / 设备 / 标签）动态生成原生磁贴面板** —— 语义上等效，观感上更"遥控器" |
| 首选对接通道 | **REST（必需，先落地）+ 本地 WebSocket（第二期，做实时）**；`civetweb` 反向接收 HA 的推送做通知 |
| 最大的非网络风险 | ★★ **实体名是中文，而项目字库是子集裁剪的 —— 字库没有的字会"整字消失"**（不是方框）。必须做"实体名 → 字库"的生成链路，见 §7.2 |
| 工作量 | 分 5 片，P0 前置验证 0.5~1 天 → P4 收尾 2~3 天（详见 §6） |

**一句话路线**：先把「看到的」做对（只读镜像 + 状态机），再把「点得动」做对（控制闭环），
再把「不卡」做对（WebSocket 实时），最后做差异化（通知 + 摄像头 + 媒体）。

---

## 1. 能力边界：这台机器有什么、缺什么

### 1.1 ✅ 已经躺在工程里的地基（**这次不用从零造**）

| 地基 | 位置 | 对本次的价值 |
|---|---|---|
| **手写 HTTP(S) 客户端** | `PgDlna.cpp` 的 `httpGetToFile()` | 已有 socket 连接、**TLS（dlopen `libssl`/`libcrypto`）**、重定向（最多 3 跳）、Content-Length 校验、DNS 自检、UA/Referer、证书校验策略 0/1/2。HA 是标准 HTTP API ⇒ **改造成"内存响应"版本即可**，不用引入 curl |
| **worker 线程 + 动作队列 → UI 线程** | `PgDlna`（SSDP/HTTP/下载三条线程）、`pg::TimeSync`、`pg::Alarm` | 照抄这套线程模型：**工作线程绝不碰控件**，只投队列；主循环 `pollResult()` 取。这是本项目已被验证的跨线程纪律 |
| **`civetweb` HTTP 服务端** | `PgDlna` 用 8200 端口 | 两处复用：① **HA → 设备** 的通知/告警接收端点；② **配对页**（浏览器粘贴 token，见 §4.4） |
| **独立 ftu 页模板** | `wifi.ftu` / `radio.ftu` / `movie` 系列 + `pg::ToolPage` | 页面外壳、长按 108 返回、屏保开关、**自己的 QA 通道** `/tmp/pg_<页>cmd`，全是现成的 |
| **软渲染画布 + 原生控件** | `GameCanvas` + `Canvas` + `tools/genfont.py` | 滑块 / 圆环 / 色轮 / 摄像头快照 / 图表这类"控件做不了"的东西画在画布上 |
| **视频硬解链路** | `pg::StreamPlayer` / `pg::Hls`（≤960×544 + 内存守卫） | HA 摄像头的 **HLS 流**理论上可复用（远期、带限制，见 §3.4） |
| **应用注册机制** | `PgGames.cpp::kAppTable`（追加表尾 + 图标四处） | slot **37**，`MAX_GAMES`(=40) > 37 ✓ |

### 1.2 ⚠️ 硬约束（每一条都会影响设计）

| 约束 | 数值 / 事实 | 设计影响 |
|---|---|---|
| 屏幕 | **480×800 竖屏**，顶栏 navibar 常占 y<52 | 面板只能 2 列磁贴；正文从 y≥52 起 |
| 内存 | 总 56 MB，**可用 ~16 MB** | ★ HA 的 `GET /api/states` 在实体多时是**几百 KB JSON** ⇒ 必须"只取我关心的"，且缓冲**定长**（见 §2.4） |
| 存储 | `/data` 仅 **528 KB** 可写（持久）；`/tmp` 19~27 MB（掉电即失） | 配置（URL/token）写 `/data/ha.conf`（与 `/data/radio.txt` 同规矩）；快照缓存写 `/tmp` |
| `/res` 分区 | **7,995,392 字节**上限，脚本有硬检查 | 新页面 + 图标 + 字库都要算进包体，加完必须看 `out/update.img` 大小 |
| 无 RTC | 开机是 1970，靠 NTP | ★ **https 证书时间校验会失败**；HA 走局域网 http 不受影响。若必须 https，先确认 `TimeSync` 已校时 |
| 无麦克风 / 无 USB Host / 无蓝牙固件 | 已知 | 不做语音助手、不做 USB 外设、BLE 方案不列入 |
| EasyUI 控件贴图 | **只能来自本地文件**，且 **listview 的 subItem 图片不按行区分**（踩过） | 动态图（摄像头缩略图、实时曲线）**必须画在 Canvas 上**；静态图标沿用"同位置多图标 + 按 slot 显隐"的笨办法（`syncRowIcon`） |
| 字库 | 子集裁剪，**无逐字回退，缺字形 = 整字消失** | ★★ 见 §7.2 —— 这是本功能最容易被低估的坑 |
| html2json | 有字符黑名单（`⌫℃■●‹－＋–…→★◆▶▷①` 等） | 实体名/单位里的 `℃`、`°`、符号可能被丢 ⇒ **单位符号要过白名单核对** |
| 单射频 | STA 联网与 monitor 嗅探互斥 | HA 遥控与"信号探针"嗅探不能同时用（文档里提一句即可） |

---

## 2. 与 HA 的对接方式

### 2.1 四条通道对比

| 通道 | 能给什么 | 成本 | 结论 |
|---|---|---|---|
| **REST** `/api/*` | 拿状态、调服务、渲染模板、拿摄像头快照 | **低**（复用现有 HTTP 客户端） | ✅ **第一期主力** |
| **WebSocket** `/api/websocket` | `subscribe_events(state_changed)` 实时推送；**`config/area_registry/list` 等"房间/设备/实体注册表"只有 WS 有** | 中（要自己实现握手 + 帧解析） | ✅ **第二期**，实时化 + 房间分组的唯一途径 |
| **设备侧 civetweb 收 HA 推送** | 门铃/告警/自动化结果弹到屏幕 | 低 | ✅ 差异化亮点，可在 P2 顺手做 |
| **MQTT（EMQX 已在跑）** | 设备级推送；★ 实测 **这台 HA 的设备本来就是 MQTT 接进来的**（§11.3(3)）| 中（设备侧要一个 MQTT 客户端，工程里目前没有） | ⚠️ **可选**：语义不如 HA 完整（无服务调用/模板/注册表/权限模型），只在真机实测确实需要时引入；**若同时上，只允许一条通道做"写"** |
| `webhook` / HA 自定义集成 | 更深的双向集成 | 高 | ❌ 本期不做；"把设备注册成 HA 的一个 `media_player`/`sensor`"现在走 **MQTT 发现**很便宜 ⇒ 提前到 **P4** |

### 2.2 认证

- **Long-Lived Access Token（LLAT）**，`Authorization: Bearer <token>`，有效期 10 年 —— 不做过期刷新，稳定性最好。
- Token 在 HA 的「个人资料 → 长期访问令牌」里生成（183 字符量级）。
- ⚠️ 401 = token 失效或被吊销 ⇒ UI 必须**明确提示"令牌无效"**而不是笼统"连接失败"（静默失败必须消灭）。

### 2.3 用到的具体端点

| 用途 | 请求 |
|---|---|
| 连通性 + 版本 | `GET /api/config` → `version`、`location_name`、`unit_system` |
| 实体清单（粗） | `GET /api/states` → 全量数组（**视实体数决定用不用**，见 §2.4） |
| 单个实体 | `GET /api/states/<entity_id>` |
| 控制 | `POST /api/services/<domain>/<service>`，body `{"entity_id": ...}` |
| ★ 批量取"我关心的"状态 | `POST /api/template`，body `{"template": "..."}` —— 模板里只吐收藏夹那几个实体，**响应体积可降一个数量级** |
| 摄像头 | `GET /api/camera_proxy/<entity_id>`（单张 JPEG）/ `GET /api/camera_proxy_stream/<entity_id>`（MJPEG 流） |
| 历史（可选 sparkline） | `GET /api/history/period/<iso>?filter_entity_id=...&minimal_response` |
| 房间/设备/标签分组 | **仅 WS**：`config/area_registry/list`、`config/device_registry/list`、`config/entity_registry/list` |
| 实时事件 | **仅 WS**：`subscribe_events` / `subscribe_trigger` |

### 2.4 ★ 关键设计：**不能靠服务响应回填**（2026-09-23 实测推翻初版假设）

初版这里写的是"`POST /api/services/<domain>/<service>` 的返回值就是受影响实体的最新状态 ⇒ 直接回填"。
**验收实测证明这条对这台 HA 不成立**（见 §11.7）：响应的确是"受影响实体的状态数组"，
但**这台 HA 的设备是 MQTT 驱动的，状态变化是异步的** —— HA 只负责把命令 publish 出去，
真正的新状态要等设备回 `stat_t`。所以服务调用在那一刻"还没有任何实体发生变化"，返回 **`[]`**。

更糟的是：**给一个不存在的实体发命令，同样返回 200 + `[]`** ——
和"成功但还没回包"**完全无法区分**（静默失败）。⇒ **不能拿响应判断成败。**

**修正后的控制流程**：

```
用户点开关（★ 先判断这个实体当前是不是 on/off）
   → 实体是 unavailable/unknown ⇒ 【直接禁点】，不让命令发出去（实测命令会被吞，200 但不生效）
   → 立刻置「乐观态」+ 转圈（按钮变亮）
   → POST /api/services/<domain>/<service>   （只看 HTTP：4xx/5xx = 硬失败，立刻回滚 + 报错）
   → 【短轮询确认】GET /api/states/<entity_id>，100~200 ms 一次，
      实测 156~210 ms 就能看到新状态 ⇒ 拿到就转「已确认」
   → 轮询窗口（建议 5 s）内仍没变成目标值 ⇒ **回滚到操作前状态 + 明确报"设备无应答/超时"**
     （离线设备就是这种：命令被吞、状态永远不变）
```

⇒ 手感仍然是"点完立刻有反应"（乐观态），但**确认与回滚是必须的**，
因为"命令被吞"是这台 HA 的**常态**而不是异常（实测 34 个实体里 17 个不是 on/off、3 个可开关实体是离线）。

⚠️ 另外两个实测约束：
- **一次调用会连带改别的实体**：实测按一次场景按钮
  （`button.z20_smart_panel_home`）把 `switch.z20_smart_panel_wo_shi_deng` 从 off 改成了 on。
  ⇒ 别只刷新被点的那一个；本地操作后要么刷新当前页所有实体，要么至少刷新"该场景可能涉及"的集合。
- **`button` 域的服务响应和 `switch` 不一样**：`button.press` 返回了 1 条状态（按钮自己），
  而 `switch.turn_*` 返回 `[]`。⇒ **响应体结构不要按 domain 猜**，按"是不是数组"统一处理。

**顺带解决内存红线**（这条仍然成立）：`/api/states` 的全量响应在实体多的 HA 上很大，
而 `POST /api/template` 返回的是**我们自己裁剪的紧凑 JSON** ⇒ 默认走模板接口，
全量接口只在"全部设备"浏览页按需调用（且带实体数上限 + WARN，不写死）。

---

## 3. 功能范围

### 3.1 面板展示（「面板」= HA 的实体组织，不是 Lovelace）

| 层级 | 内容 | 数据来源 |
|---|---|---|
| L1 磁贴面板 | 用户置顶的快捷磁贴（场景 / 灯 / 开关 / 传感器大数值），2 列 | 本地收藏（`/data/ha.conf`）|
| L2 房间面板 | 按 HA 的 **area** 分组（客厅/卧室/厨房…） | WS 的 `area_registry` + `entity_registry`（第一期先用本地自定义分组兜底）|
| L3 领域浏览 | 按 domain 分组的全部实体（灯/开关/窗帘/空调/传感器…） | `/api/states` |
| L4 设备详情 | 单个实体的完整控制 + 属性 | 单实体状态 + 能力属性 |

### 3.2 设备控制（按 domain 的原生控件）

| domain | 控件 | 服务 | 关键属性 / 坑 |
|---|---|---|---|
| `light` | 开关 + 亮度滑条 + 色温滑条 + 色轮 | `turn_on/off/toggle` | `supported_color_modes` 决定显示哪些控件；★ **色温新旧属性不同名**（`color_temp`(mired) vs `color_temp_kelvin`），亮度二选一（`brightness` 0-255 / `brightness_pct`） |
| `switch`/`input_boolean` | 开关 | `turn_on/off/toggle` | — |
| `fan` | 开关 + 档位滑条 + 预设 | `fan.turn_on/off`、`fan.set_percentage` | `percentage_step`、`preset_modes` |
| `cover` | 开 / 停 / 关 + 位置滑条 | `open_cover/close_cover/stop_cover/set_cover_position` | `current_position` 可能不存在 ⇒ 按 `supported_features` 隐藏滑条 |
| `climate` | 当前温度 + 目标温度（圆环或 ± 步进）+ 模式段控 + 风速 | `set_temperature/set_hvac_mode/set_fan_mode` | `hvac_modes`、`min_temp/max_temp/target_temp_step` —— **全部要按属性动态布局**，别写死 |
| `media_player` | 播放/暂停/上一个/下一个/音量/音量静音 | `media_play_pause`、`volume_set`、`turn_off` | `volume_level` 是 0..1；`source_list` 可做源选择 |
| `scene`/`script`/`automation`/`button`/`input_button` | 一个大按钮 | `scene.turn_on` / `script.turn_on` / `automation.trigger` / `button.press` | 无状态，点完只给"已触发"反馈 |
| `lock` | 锁 / 解锁（**二次确认**） | `lock.lock/unlock` | 误触代价高 |
| `number`/`input_number` | 数值滑条 | `number.set_value` | `min/max/step/mode` |
| `select`/`input_select` | 选项列表 | `select.select_option` | `options` |
| `sensor`/`binary_sensor` | 只读：数值 + 单位 + 图标（可选 7 点 sparkline） | — | `unit_of_measurement`、`device_class`（★ 单位符号可能被 html2json 黑名单丢）|
| `camera` | 缩略图 + 点开预览 | `camera_proxy` | 只读；见 §3.4 |
| `vacuum`（可选） | 启停 / 回充 | `vacuum.start/return_to_base` | — |

> **原则**：控件由 `supported_features` / `*_list` 属性**驱动生成**，
> 不认识的 domain **降级成"只读卡片"**（显示 state + 前 2 个数值属性），
> 绝不因为"不认识"就崩或空白 —— 这是 HA 侧设备千奇百怪的必然要求。

### 3.3 状态同步

三个来源，优先级从高到低：

1. **服务调用响应**（§2.4）—— 本地操作的即时真相；
2. **WebSocket `state_changed` 订阅**（第二期）—— 别处改动的实时同步（目标 <500 ms）；
3. **REST 轮询**（第一期 / WS 降级路径）—— 默认 3 s，只拉收藏 + 当前页可见实体。

状态机（必须**可观测**，每个状态都能在 UI 和日志里看见）：

```
IDLE → CONNECTING → ONLINE
                 ↘ AUTH_FAIL（token 无效，不重试，提示用户）
                 ↘ OFFLINE（网络不可达，指数退避重连 2/4/8/16/30 s 封顶）
                 ↘ PROTO_ERR（HA 版本/接口不符，打印原始响应）
```

UI 侧固定显示：**连接指示 + 数据新鲜度**（"3 s 前 / 已离线 1 分 12 秒"）。
★ 别做"看起来还在线"的假象 —— 老数据必须能一眼看出是老的。

### 3.4 摄像头（要分层给期望）

| 档 | 做法 | 可行性 |
|---|---|---|
| A. 缩略图/单帧 | `camera_proxy` 拉一张 JPEG → 写 `/tmp` → Canvas 解码绘制 | ✅ 稳，先做这个 |
| B. MJPEG 预览 | `camera_proxy_stream` 连续 JPEG 帧（1~5 fps）| ✅ 可行（JPEG 软解 + 画布），**限定低帧率、小分辨率** |
| C. HLS 实时流 | HA 的 `camera/stream` → m3u8 | ⚠️ 复用 `pg::Hls` + 硬解，但**源常常 >960×544 ⇒ 撞解码内存守卫**，且必须"退出即停流"（55 MB 内存，不停流下个应用必炸）|

⇒ 本期做到 **A + B**；C 列为**远期可选**，且必须按 `docs/movie-app.md` / `iptv.md` 的停流纪律写。

### 3.5 差异化：**HA → 设备**（这是"遥控器"之外的加分项）

1. **通知/告警推屏**：设备 civetweb 开一个本地端点（如 `:8301/ha/notify`），
   HA 侧加一条 `rest_command` 或自动化 action ⇒ 门铃响、漏水告警、自动化完成，**屏幕弹提示 + 响铃/振动**。
   ★ 复用现成的"响铃让位"机制思路（`pg::Alarm::instance().ringing()`）：任何页面都能被通知抢占。
2. 设备状态**回传** HA（把屏幕当一个人机界面/虚拟遥控器）：远期。
3. 远期：把设备注册成 HA 的一个 `media_player`（TTS 播报落地端）—— 需要 HA 自定义集成或 MQTT，成本高。

### 3.6 明确不做（写进文档，避免需求漂移）

- ❌ Lovelace 原样渲染 / HA 前端登录 / 卡片 YAML 解析
- ❌ Add-on 管理、HA 配置编辑、日志查看（`/api/error_log` 可选做只读）
- ❌ 复杂历史图表（最多做 7 点 sparkline）
- ❌ 云/远程访问（Nabu Casa 之类）—— 只支持局域网直连
- ❌ 语音控制（无麦克风）

---

## 4. 界面与交互设计

### 4.1 页面结构（`ha.ftu` 一个 ftu + 多个整屏 window，符合《页面架构规范》）

> 同一业务域内的页签/二级页/弹窗 → 同 ftu 内多 window（`showWnd/hideWnd`），不新建 Activity。

| window | 内容 | 入口 |
|---|---|---|
| `WinHaHome` | **遥控器主界面**：顶栏（连接状态 + 实体数 + 刷新）→ 页签（磁贴 / 房间 / 全部）→ 磁贴网格 | 应用卡片 / 返回 |
| `WinHaEntity` | **设备详情/控制页**：按 domain 渲染控件（大字当前值 + 滑块/圆环/段控） | 点任一磁贴/列表项 |
| `WinHaScenes` | **场景面板**：大磁贴（回家 / 影院 / 晚安 / 全关） | 主界面页签 |
| `WinHaSettings` | **连接配置**：地址、端口、Token 状态、TLS、轮询间隔、`连接测试`、`配对页提示` | 主界面右上齿轮 |
| `WinHaCamera` | 摄像头预览（B 档 MJPEG / A 档单帧刷新） | 摄像头磁贴 |
| （复用）`WinVolume` / `WinAlarmRing` | 全局音量 OSD / 响铃提醒 —— **留在 main.ftu**，本页照抄"响铃让位" | — |

### 4.2 版式（遵守 `docs/ui-design-baseline.md`）

- 顶栏 y 0..60 + 色条 0..8；**正文从 y≥52 起**（navibar）—— 本页是独立 ftu，
  按 §16 的规矩：`navibar` 只在 main 显示，独立页要确认 `setNaviTitle` 与之不冲突。
- **视觉主角只允许一个**：详情页 = 当前值大字（亮度 62% / 目标 24.5 ℃）；
  主界面 = 磁贴网格本身，别再加装饰。
- 磁贴规格：2 列 × 约 210×140，圆角 12~16，行距 16 / 列距 8（与既有按钮网格同节奏）。
- 状态色（本项目既有语言）：**开 = 主题色/暖色高亮**，关 = `#2A2A2E` 级别灰；
  ⚠️ 本项目是**竖屏中文 UI**，不用股票那种红涨绿跌语义，色彩只表达"开/关/告警"。
- **控件贴图只能来自本地文件** ⇒ 所有图标（灯/开关/窗帘/空调/场景/摄像头/传感器…）
  用 `tools/gen_icons.py` 的同一套做法**程序化生成 PNG**（一次生成、静态引用）。

### 4.3 交互规则（直接抄既有约定，别重新发明）

| 场景 | 规则 | 出处 |
|---|---|---|
| 长按返回 | 108 长按 ≥700 ms → 返回应用列表；短按 108 = 强制刷新状态 | `ToolPage` / `wifi.ftu` 同款 |
| 音量键 | 103/105 → `pg::volumeStepGlobal` | 同上 |
| 屏保 | 进页 `setScreensaverEnable(false)`，退出恢复；**响铃期间让位自动退出** | `clocksuite` / `iptv` 的 §9.1 纪律 |
| 滑块 | `data-drag="1"`；★ `setProgress()` 也会触发 `onProgressChanged`（异步）⇒ 同步 UI 时要防回环；**拖动中节流（≥100 ms）或抬手才下发服务**，别每像素发一次 HTTP | 项目 §22 |
| 半透明浮层 | 用带 alpha 的 PNG 挂 `button.picTab` + 底图 button 在 html **第一个定义** + `setTouchPass` 不能用；面板底不加 `setTouchPass` | 项目 §21 |
| 浮层触摸 | 要能点的按钮，**每一层祖先都要 `touchable=true`** | 项目 §8 |
| 列表图标 | 沿用"同位置多图标 + 按 slot 只显一个 + **以控件自身可见性为准**（别用行号缓存）" | `syncRowIcon` 的教训 |
| 重绘 | **变了才 setText/setPosition**；动画一律**绝对时钟**推导 | 屏保页 / 频谱柱的教训 |
| 键位条 | 底部 y 700..800 固定键位提示 | design baseline |
| 网络失败 | **不许静默**：UI 显式显示"离线/令牌无效/无此实体"，日志带原始 HTTP 码 | 项目 §11 |

### 4.4 ★ Token 怎么输进去（这是个真问题）

Token 183 字符，用 `ime.ftu` 在 480×800 屏幕上敲完不现实，而且没有剪贴板。三个方案：

| 方案 | 做法 | 评价 |
|---|---|---|
| **A. 网页配对（推荐）** | 设备 civetweb 开 `http://<设备IP>:8301/ha`，PC/手机浏览器打开 → 填地址 + 粘贴 token → POST 写入 `/data/ha.conf` | ✅ 体验最好；复用现成 civetweb；也顺便解决了"服务器地址"录入 |
| B. 文件预置 | PC 侧 `adb push ha.conf /data/ha.conf` | ✅ 最省事，适合自助/工厂；文档里写清楚格式即可 |
| C. 屏上输入 | 复用 `ime.ftu`，分段输入 | 兜底，留一个"高级"入口 |

⇒ **A 为主 + B 兜底 + C 保留**。三者写的是**同一个文件**，格式统一（`key=value`，含 `url/port/token/tls/interval/favorites`）。

---

## 5. 整体架构与模块划分

```
┌─ UI 线程（EasyUI，单线程，唯一能碰控件的地方）──────────────────────────────┐
│  ui/ha.html ──gen_ui.py──> ha.json/ha.ftu                                   │
│  src/logic/haLogic.cc        窗口切换 / 控件同步 / 按钮与触摸 / 按键 / QA      │
│  src/ui/HaPage.{h,cpp}       页面外壳（比照 pg::ToolPage / pg::MoviePage）     │
│      · show()/hide()   管屏保 + 响铃让位 + 连接生命周期                       │
│      · tick(dt)        取结果队列 → 刷 UI（变了才写）                          │
└───────────────────────────▲─────────────────────────────────────────────────┘
                            │ pollResult()（无锁快照）        │ pushCommand()
┌───────────────────────────┴─────────────────────────────────────────────────┐
│  src/platform/PgHa.{h,cpp}   HA 客户端（业务/网络层，单工作线程）             │
│   ├ CmdQueue    getConfig / getStates / getStatesByTemplate / callService     │
│   │             cameraSnapshot / ping                                        │
│   ├ HttpSession 复用 PgDlna 的 TLS+socket 思路 → request(method,path,body)    │
│   │             改为【内存响应】（现版是"下到文件"，要加一个内存版）           │
│   ├ PgJson      极简 JSON 解析（*决策点，见 §7.4）                            │
│   ├ EntityStore entity_id → {state, attributes, last_changed}，定长 + 上限   │
│   ├ FavoriteStore 收藏/磁贴顺序（持久到 /data/ha.conf）                        │
│   └ StateMachine OFFLINE/CONNECTING/ONLINE/AUTH_FAIL/PROTO_ERR + 指数退避     │
│  （二期）PgHaWs.{h,cpp}  WebSocket：握手 / 帧编解码 / ping-pong / 订阅事件     │
│  （P2 起）PgHaNotify.{h,cpp} civetweb：/ha/notify 接收端点 + /ha 配对页        │
└───────────────────────────┬─────────────────────────────────────────────────┘
                            │ TCP + 可选 TLS（libssl via dlopen）
                     ┌──────▼───────┐
                     │ Home Assistant │  :8123  REST + WebSocket
                     └───────────────┘
```

### 5.1 模块清单与落地位置

| 模块 | 文件 | 职责 | 备注 |
|---|---|---|---|
| HA 客户端 | `src/platform/PgHa.{h,cpp}` | 线程、队列、HTTP、状态机、实体表 | **核心**，预计 900~1300 行 |
| JSON | `src/platform/PgJson.{h,cpp}` | 解析 HA 响应（对象/数组/字符串转义/数字/嵌套） | 见 §7.4 决策 |
| 配置持久化 | 并入 `PgHa` 或 `src/platform/PgHaConf.{h,cpp}` | `/data/ha.conf` 读写 + 默认值 + 迁移 | 与 `PgStore` 同风格 |
| WebSocket（二期） | `src/platform/PgHaWs.{h,cpp}` | ws:// 客户端 | **独立切片**，失败可整体回退轮询 |
| 通知/配对服务 | `src/platform/PgHaNotify.{h,cpp}` | civetweb 端点 + 配网页 | 端口与 DLNA(8200) 错开 |
| 页面外壳 | `src/ui/HaPage.{h,cpp}` | show/hide/tick/按键/QA | 照 `ToolPage` |
| 页面逻辑 | `src/logic/haLogic.cc` | 控件同步、domain→控件映射、事件 | 预计 700~1100 行 |
| 布局 | `ui/ha.html` + `tools/gen_ui.py` 的 `UI_SOURCES` | 5 个整屏 window | 改 html 必须重跑 `gen_ui.py` |
| 图标 | `tools/gen_icons.py` + `ui/main.html` + `kIconCount` + `HIDDEN_CONTROLS` | 卡片图标 + domain 图标 | **四处一起改** |
| 字库生成 | `tools/gen_ha_font.py`（新） | 从 HA 导出实体名 → 裁字库 | ★ 见 §7.2 |
| 验收脚本 | `tools/ha_qa.py`（新） | PC 侧对照 HA REST 与设备显示 | 判据落文件 |

### 5.2 线程与并发纪律（**必须照抄，别自创**）

1. **工作线程只做阻塞 IO**：socket/connect/read/TLS。**绝不**调用任何 EasyUI API、绝不 log 到 UI。
2. 命令下行 = `pushCommand()`（互斥锁保护的环形队列）；结果上行 = `pollResult()`（主循环每帧取）。
3. 实体表用**双缓冲快照**：工作线程写"后台副本"，UI 读"前台副本"，切换点加锁 —— 避免 UI 读到半更新的数组。
4. 退出页面必须 `stop()`：停线程、**join 可 join 的线程**、关 socket（`PgDlna` 里 `ssdpJoinable_` 那个 use-after-free 教训）。
5. 断网/HA 重启期间的调用要能**被取消**（页面已退出就别再投结果）。

---

## 6. 分阶段实施步骤

> 每阶段独立可交付、可回退；**每阶段都必须过"四条纪律"**：
> ① 图标四处同步 ② 新中文重跑 `gen_font.py` ③ 独立页自带 QA 通道 ④ 屏保/长按返回/响铃让位。

### P0 · 前置验证（0.5~1 天）——**先证伪，再立项**

| 项 | 做法 | 判据（**不通过就不往下走**） |
|---|---|---|
| 网络可达 | 设备侧 ping/连 HA 的 8123 | 能连上并收到 HTTP 响应 |
| 认证 | 发一次 `GET /api/config` | 返回 JSON 含 `version`，打印到 `/tmp/ha_probe.txt` |
| 响应体积 | `GET /api/states` 与 `POST /api/template` 各一次 | **实测两者字节数**（决定 §2.4 走哪条），并确认实体总数 |
| 能力盘点 | 统计 HA 里 domain 分布 + 实体名是否含中文/生僻字 | 得到**实体名清单**（喂给 `gen_ha_font.py`） |
| 决策点 | ① JSON 库从哪来 ② WS 自研 or 只 REST ③ token 录入方式 | 三项拍板（§8） |

**交付物**：`docs/ha-probe.md`（实测数字 + 决策结论）。

### P1 · 只读镜像（1~2 天）——「看到的」先对

- `PgHa` 骨架：线程 + 队列 + HTTP 内存版 + `PgJson` + 状态机（OFFLINE/ONLINE/AUTH_FAIL）
- 页面骨架：`ha.ftu`（`WinHaHome` 列表 + `WinHaSettings`），`HaPage` 外壳（屏保/长按/响铃）
- 列表页：按 domain 分组浏览 + 状态显示（开/关/数值 + 单位）
- QA 通道 `/tmp/pg_hacmd`：`ha ping` / `hastates` / `hadump <entity>` / `haoff`（模拟离线）/ `quit`

**验收判据**：
- `ha ping` → 打印 HA `version` + 实体数；
- 屏幕看到 N 个实体，**随机抽 5 个**与 PC 侧 `curl /api/states/<id>` 逐字段一致（脚本比对，落文件）；
- **拔网线/停 HA** → UI 显示"离线"+ 时间戳；恢复后**自动回到 ONLINE**（无需重启应用）；
- `AUTH_FAIL`：故意用错 token → UI 明确提示"令牌无效"（不是"连接失败"）；
- 进页 40 s：`performScreensaverOn` 计数 **= 0**；长按 108 → 返回列表。

### P2 · 控制闭环（2~3 天）——「点得动」且手感对

- domain → 控件映射表（§3.2），至少覆盖：`light / switch / scene / script / cover / climate / media_player / sensor`
- 乐观态 + **服务响应回填** + 失败回滚（§2.4）
- 收藏夹/磁贴（`/data/ha.conf`）+ 场景面板 `WinHaScenes`
- 配置页 + **网页配对**（civetweb `/ha`）
- 可选顺手做：**HA → 设备通知**（civetweb `/ha/notify`）

**验收判据**：
- 设备点"开灯" → HA 侧 `curl` 读回 `state=on`；亮度滑到 40% → 读回 `brightness≈102`（±2 容差）；
- 服务调用**失败注入**（改错 entity_id）→ UI 回滚 + 明确报错，**不静默**；
- 拖动滑条期间**不会每像素发一次请求**（抓 HA 日志计数）；
- 配对页：手机浏览器打开 → 写入 → 设备在 **3 s 内**进入 ONLINE（无需重启）。

### P3 · 实时化 + 房间面板（2~3 天）

- WebSocket：握手 → `auth` → `subscribe_events(state_changed)` → 增量更新实体表
- 断线自动重订阅；**WS 不可用时自动降级回 3 s 轮询**（降级要能看见）
- `area_registry` / `entity_registry` → 房间分组；磁贴面板支持"按房间/按收藏"

**验收判据**：
- 手机 HA App 改一个灯 → 设备屏幕 **<500 ms** 同步（抓帧/日志时间差）；
- 拔网 30 s 再恢复 → 订阅自动重建，状态自愈，**不重启进程**；
- 房间分组与 HA 前端的区域名一致（抽样 3 个）。

### P4 · 差异化与收尾（2~3 天）

- 摄像头 A + B 档（缩略图 / 低帧 MJPEG），退出即停流
- HA 通知推屏（若 P2 未做）
- `tools/gen_ha_font.py` 固化进流程；`README` / `docs/ha-app.md` 定稿
- 包体核算（`out/update.img` ≤ 7,995,392）、内存核算（进程 RSS 增量 + `dmesg` 无 OOM）
- 沉淀 MCP 知识库文档（按用户级约定：**产出到工程 `docs/` + 提交说明**，不代改 MCP）

---

## 7. 风险清单与对策

### 7.1 高风险（会直接决定成败）

| # | 风险 | 后果 | 对策 |
|---|---|---|---|
| R1 | **WebSocket 自研复杂**（掩码、分片、ping/pong、握手校验、大帧） | 卡住实时化 | ① **P1/P2 完全不依赖 WS**，REST 轮询就能交付可用产品；② WS 做成**独立切片**，失败即回退轮询；③ 优先只做 `ws://`（局域网明文），`wss://` 列为可选 |
| R2 | **实体名中文 → 字库缺字形（整字消失）** | 屏幕上设备名变"空气" | 见 §7.2 —— 必须做"实体名 → 字库"的生成链路 |
| R3 | **内存**（56 MB 总 / ~16 MB 可用，且本项目在视频/贴图上多次踩内存红线） | 静默被杀（`zkgui_ui` 被 OOM 掉，查 `dmesg`） | ① 默认走 `/api/template` 只取关心的实体；② JSON 缓冲**定长 + 上限 + 超限 WARN**；③ 实体条数上限不写死（项目 §27 纪律）；④ 摄像头用低帧率小图；⑤ 全程 `dmesg` 盯 OOM |
| R4 | **token 录入**（183 字符 + 无剪贴板） | 用户根本配不上 | §4.4 三方案，**网页配对为主** |

### 7.2 ★ R2 展开：中文实体名与字库（最容易被低估）

**机理**：项目 `font/` 里是**完全替换系统字体的裁剪子集，没有逐字回退** ⇒
字库里没有的字**整个字消失**（不是方框）。而 HA 的 `friendly_name` 是用户自定的中文，
比如"客厅主灯""主卧空调""凉霸"——**必然落在子集之外**。

**对策（三层，全做）**：

1. **生成链路（主）**：`tools/gen_ha_font.py` ——
   从 HA 导出全部实体的 `friendly_name`（P0 那次探测顺手做）→ 与项目现有字库取并集 →
   用带 fontTools 的解释器重跑 `gen_font.py`（`C:/Users/.../envs/default/Scripts/python.exe`）。
   ⇒ **实体名集合是有限且已知的**，所以这条路一定能走通。
   注意：实体**新增/改名后要重跑**（写进 README，别让用户自己发现）。
2. **显示层兜底**：磁贴主标题优先用**中文本名**，副标题显示 `entity_id` 的英文部分；
   若某字缺失，至少还有英文可读。**绝不依赖"字库一定有"**。
3. **单位/符号白名单核对**：`℃` `°` `%` `kWh` 等可能在 `html2json` 黑名单里被丢
   （已知 `℃` 在黑名单中）⇒ 单位符号要过一遍 `check_assets.py` 式的核对，
   必要时用 `C` 或画布自绘替代。

### 7.3 中风险

| # | 风险 | 对策 |
|---|---|---|
| R5 | **服务参数随 HA 版本变化**（`color_temp`→`color_temp_kelvin`、`brightness` vs `brightness_pct`） | 按**属性/`supported_color_modes` 驱动**生成控件；老字段做兼容分支；不认识的属性忽略而不是报错 |
| R6 | **TLS / 自签证书**（设备无系统 CA，且无 RTC 时时间不对会导致校验失败） | 复用 `PgDlna` 的 CA 查找 + 校验策略 0/1/2（`pgDlnaSetTlsVerifyMode`）；**默认引导用户用局域网 http**；https 首次配置前先确认 NTP 已校时（`TimeSync`） |
| R7 | **实体数量爆炸**（HA 常见 100~1000 实体） | 默认视图 = 收藏 + 可控 domain；"全部"页要带上限与分页；**控件数固定**（EasyUI 是静态 html 生成，不能运行时建控件） |
| R8 | **滑块拖动 → 请求风暴** | 拖动中只更新本地显示，**抬手或 ≥100 ms 节流才下发**；并合并同一实体的连续值（只发最后一次） |
| R9 | **UI 重绘/`setPosition` 风暴** | 变了才写；动画用绝对时钟；列表项图标按"控件当前可见性"判定（别用行号缓存） |
| R10 | **`/res` 分区超限**（7,995,392 字节） | 加页面/图标后必看 `out/update.img` 大小（脚本有硬检查）；新增大图先算体积再进包 |
| R11 | **与既有应用冲突**：WiFi 嗅探占单射频；视频页占解码通道与内存 | HA 页**不起流**（除摄像头 B 档）；文档写清"HA 遥控与嗅探不能同时用"；退出时确认 `running()==false` |
| R12 | **HA 侧限流/重启** | 指数退避（2/4/8/16/30 s 封顶）+ 请求超时（连接 ≤3 s、整体 ≤8 s）；HA 重启后自动恢复，不依赖人工重启应用 |

### 7.4 低风险 / 待决策

| # | 事项 | 说明 |
|---|---|---|
| R13 | **JSON 解析从哪来** | ① 平台仓库搜 `json`（`flythings_package_search`，以前 curl/openssl 就是这么找到的）；② 自研极简解析器（约 400~600 行，项目已有"手写 HTTP"的先例，可控性最好）；③ 引入 cJSON/json-c。**倾向 ①→②**，避免新依赖带来的体积与适配风险 |
| R14 | **HA 版本差异** | P0 记录版本号；接口以 `/api/config` 与 `/api/services` 实测为准，不照文档假设 |
| R15 | **多 HA 实例** | 本期只支持 1 个（配置里留 `url` 单值）；多实例列为远期 |
| R16 | **安全边界**（token 明文存 `/data`） | 威胁模型：设备在用户自己家里、root 可读、不做公网暴露。对策：只用 LLAT（可随时在 HA 侧吊销）、不写日志输出完整 token、配网页只在局域网可访问、README 明示"token 等同家庭控制权限" |

---

## 8. 需要你拍板的事

1. **「面板」的定义**：确认走**原生磁贴 + 房间分组**（而不是尝试渲染 Lovelace）——
   这是整份规划的基座，若你要的是"照搬 Lovelace"，那需要换技术路线（浏览器内核，本机不现实）。
2. **首期要不要 WebSocket**：建议 **P1/P2 不依赖 WS**（先出可用产品），WS 放 P3 独立切片。
3. **token 录入方式**：确认 **网页配对（civetweb）为主**、`adb push /data/ha.conf` 兜底。
4. **HA 实例信息**（做 P0 要用）：HA 的**局域网地址 + 版本**；
   以及应允不允许你**生成一个临时 LLAT** 给我做真机探测（用后即吊销）。
5. **实体规模**：HA 里大概多少实体、domain 分布，以及**实体名是不是中文**（决定 R2 工作量）。
6. **应用的归类与位置**：建议 `slot = 37`、分类 **`APP_SYSTEM`（系统）**、卡片名"智能家居"。
   （`MAX_GAMES`=40 > 37 ✓，但再加应用要同时检查这一行。）

---

## 9. 附：本次规划的事实依据

| 结论 | 依据 |
|---|---|
| 已有手写 HTTP(S) + TLS + 重定向 | `src/platform/PgDlna.cpp` 的 `httpGetToFile()` / `pgDlnaTlsDiag()` / `pgDlnaSetTlsVerifyMode()` |
| 已有 worker 线程 + 队列 + UI 隔离纪律 | `PgDlna.h` 的线程模型注释（"绝不能碰 UI"，动作队列 `pollAction()`） |
| 有 civetweb 可用 | `PgDlna` 用其提供 SCPD/SOAP；端口 8200 |
| 独立 ftu 模板成熟 | `docs/page-split-plan.md` §3（9 步）、§8.2（`pg::ToolPage`）|
| 应用注册四处 + slot 追加 + `MAX_GAMES` | `src/core/PgGames.cpp::kAppTable`（当前 37 项，slot 0~36）、`kIconCount=37`、`PgStore.h::MAX_GAMES=40` |
| 能力边界（内存/存储/无 RTC/无麦克风/字库/黑名单） | `docs/product-feature-proposal.md` §1 及附录 |
| 视频流停流纪律与内存红线 | `docs/movie-app.md`、`docs/iptv.md`、`docs/online-media.md` |
| 滑块/浮层/重绘/长按/屏保等交互约定 | `.workbuddy/memory/MEMORY.md` §8/§21/§22 与 `docs/ui-design-baseline.md` |
| HA 接口 | HA 官方开发者文档：REST API（`/api/states`、`/api/services/<d>/<s>`、`/api/template`）、Auth API（LLAT 10 年）、WebSocket API（`subscribe_events`、`config/*_registry/list`） |

---

# 11. P0 前置验证：实测结果与规划修订（2026-09-23）

> 全部数字来自真机实测，原始记录在 `docs/ha-probe.md` 与 `docs/emqx-probe.md`。
> 探针：`tools/ha_probe.py`（HA REST + 手写 WebSocket）、`tools/emqx_probe.py`（EMQX API + 手写 MQTT 嗅探）。
> ⚠️ 两份报告现在**一次跑出来就是一份证据**，改动前先重跑，别手工维护数字。

## 11.1 实测环境

| 项 | 实测值 |
|---|---|
| HA | `http://192.168.1.188:8123`，版本 **2026.9.3**，`location_name=我的家`，`time_zone=Asia/Shanghai`，`language=zh-Hans`，components **147** |
| HA 单位制 | ⚠️ **US customary**：`temperature=°F`、`length=mi`、`mass=lb`、`pressure=psi`、`volume=gal` |
| EMQX | `http://192.168.1.188:18083`，版本 **5.8.7**；监听 `tcp:1883` / `ssl:8883` / `ws:8083` / `wss:8084` |
| 端口 | 8123 / 18083 / 1883 / 8083 / 8883 **全开** |
| 认证 | HA 登录流程（`login_flow` → `auth/token`）可用，216 ms 拿到 30 分钟 token + refresh_token |
| 设备 | PocketGame 板 `20080411`（USB adb）★ **当前未联网**；另有 `192.168.1.177:5555` = **Z20 面板本体**（见 §11.4） |

## 11.2 关键实测数字

| 判据 | 实测 | 对规划的意义 |
|---|---|---|
| 实体总数 | **36** 个（13 个 domain） | R7「实体爆炸」**不成立**，列表页没有压力 |
| `GET /api/states` 全量 | **14 476 字节（14.4 KB）**，27 ms | R3「内存」风险大幅下降 |
| ★ `POST /api/template` 裁剪 | **595 字节 = 全量的 4.0%**，49 ms | §2.4 的设计**成立且余量很大** |
| ★ WebSocket | 握手 **101**（2~14 ms）→ `auth_required` → `auth_ok` → `subscribe_events` **成功** | R1 从"高风险"降为"已打通" |
| 注册表 | area 4 / device 8 / entity 36 条 | 「读 HA 的房间/设备结构」这条路可行 |
| state 分布 | `on` 2 / `off` 2 / **`unavailable` 5** / **`unknown` 12** / 其它 15 | ★ 只有 **4/36** 是真 on/off ⇒ **灰态是主场景，不是边角** |
| 中文 friendly_name | **12 / 36（33%）**，去重汉字 **23 个** | R2 成立，但**工作量极小**（23 字，不是几百） |

## 11.3 ★ 三条被实测推翻/改写的假设

### (1) HA 的 Jinja 沙箱**禁掉了 `dict.update`** —— 模板裁剪的写法被锁死

原规划 §2.4 只写了"用模板裁剪"，没写写法。实测三种：

| 写法 | 结果 |
|---|---|
| `namespace + list 追加 + tojson`：`{% set ns = namespace(o=[]) %}{% for e in [...] %}{% set ns.o = ns.o + [[e, states(e), state_attr(e,'friendly_name')]] %}{% endfor %}{{ ns.o | tojson }}` | ✅ **200，595 字节** |
| `namespace + ns.o.update({...})` | ❌ 400 `SecurityError: access to attribute 'update' of 'dict' object is unsafe` |
| 手工拼 JSON 文本（循环里写字面 `{`/`}`） | ❌ 400 `TemplateSyntaxError: unexpected '%'` |

⇒ **写法固定为第一种**，设备侧实现直接照抄。这是本次最"省钱"的一条实测：
不用再花一轮去踩这个坑。

### (2) 房间分组**现状做不了**（房间建好了，但设备没放进去）

原规划 §3.1 的 L2「按 HA 的 area 分组」被当作可行项。实测：

- 4 个 area 确实存在：`客厅`、`厨房`、`卧室`、`钟总办公室`
- 但 `entity_registry` 里 **0/36** 条有 `area_id`；`device_registry` 里 **1/8** 条有 `area_id`
- ⇒ 三表拼接的结果：**36 个实体全部落到「未分配房间」**

**自证**（检查工具必须先证明"它报得出来"）：报告里把两个注册表的**原始字段**摊开打印了 ——
`area_id` 这个键确实存在（不是字段名写错），且 `device_registry` 里**确实有 1 条非空**
（说明拼接逻辑能命中，只是这台 HA 基本没做归属）。

⇒ 两条路，选一条：
- **A（推荐）**：先花 5 分钟在 HA 前端把设备归到区域，L2 按原计划做；
- **B（兜底）**：L2 退化成「设备侧本地自定义分组」，房间由用户在屏幕上自己分。
  **P1/P2 不依赖它**，所以不阻塞开工。

### (3) ★★ **这台 HA 的设备其实是走 MQTT 接进来的** —— 多出一条通道，还顺手打开了反向集成

`/api/config` 的 components 里有 `mqtt`、`mqtt.light`、`mqtt.switch`、`mqtt.sensor`、`mqtt.button`。
去 EMQX 侧一查，topic 结构完全暴露（从订阅表 + 保留的 discovery 报文实测）：

```
smartpanel/PANEL-B9D9223B/switch/relay_{1,2,3}/state     ← Z20 面板的 3 路继电器状态
smartpanel/PANEL-B9D9223B/switch/+/command               ← 面板订阅的命令主题
smartpanel/PANEL-B9D9223B/scene/+/command                ← 场景命令
smartpanel/PANEL-B9D9223B/sensor/{temperature,humidity}  ← 面板自带温湿度
smartpanel/PANEL-B9D9223B/availability
smartpanel_test/switch/relay_1/state
zk_switch_FACD/switch/switch_{1,2,3}/state  +  zk_switch_FACD/status   ← 第三方 ZK 开关（当前 offline）
homeassistant/{light,switch,sensor,button}/<uniq_id>/config            ← HA MQTT 发现
```

- broker 上只有 2 个客户端：`smartpanel_PANEL-B9D9223B`（来自 **192.168.1.177**）与 HA 自己（`51Ti3…`@172.28.10.1）
- ★ **`192.168.1.177` 就是 adb 上那台 `192.168.1.177:5555` —— 那台 Z20 面板本体**
- ACL：订阅 `#` **被拒**（SUBACK `0x80`），须订具体前缀（`homeassistant/#` 允许）
- 7 个 `button` 实体 = 场景 `home`/`away`/`sleep`/`movie`/`read`/`party`/`night`
- 3 个 `unavailable` 的「Z20 HA Switch」实体来自 `zk_switch_FACD`（它的 `availability` 是 offline）

**对规划的三个影响**：

1. **多一条实时通道，但主线不变**。设备直连 MQTT 订阅 state 主题确实能拿到推送、不必手写 WebSocket。
   但 MQTT 这条路的语义**不如 HA 完整**（没有服务调用/模板/注册表/摄像头，也没法用 HA 的权限模型）。
   ⇒ **主通道仍是 HA REST + WS**（§2），MQTT 定位成**可选加速 / 通知通道**，
   并且**只在真机实测确实需要时才引入** —— 本板每多一条长连接就多一份内存与重连逻辑。
   
   ⚠️ 若将来真的两条都上，**只允许一条做"写"**（命令），否则会出现"设备以为关了、HA 以为开着"。

2. **★ 反向集成从"远期"提前到 P4**：既然这台 HA 本来就是靠 MQTT 发现接设备的，
   那"把 PocketGame 屏幕注册成 HA 的一个设备"（发布 `homeassistant/switch/pg_xxx/config` + 一个 state 主题）
   就只是**发几条 MQTT 报文**的事 —— 原规划里因成本高而搁置的那一项，现在很划算。

3. **设备侧 MQTT 客户端要单独评估**：本工程没有现成 MQTT 库。
   要么在平台仓库搜 `mosquitto`/`paho`（`flythings_package_search`，curl/openssl 就是这么找到的），
   要么手写最小客户端 —— 本次 Python 探针已经把协议细节趟平（CONNECT 报文/变长长度/SUBACK `0x80` 语义），
   移植到 C++ 是可控工作量。

## 11.4 阻塞项状态（2026-09-23 下午更新）

| # | 事项 | 状态 |
|---|---|---|
| 1 | PocketGame 板子的 WiFi | ✅ **已解除** —— `wlan0 = 192.168.1.27`，`ping 192.168.1.188` **0% 丢包**（平均 37 ms） |
| 2 | 长期访问令牌（LLAT） | ✅ **已解除** —— 已提供（183 字符，sha256 前 8 位 `6888fbb0`）。⚠️ **令牌只写设备的 `/data/ha.conf`，不进仓库、不进日志**；PC 侧探针用环境变量 `HA_TOKEN` 传，报告里只落指纹 |
| 3 | **控制闭环测试的授权** | ⏳ **待你点头** —— 要真发一次 `POST /api/services/switch/turn_on`（会真的开关灯）。建议拿 `switch.z20_smart_panel_deng_dai`（当前已是 on）练手，或你说"随便点" |

> 另：`192.168.1.177:5555` 那台是**现场在用的 Z20 面板**（adb 在线 + MQTT 在线）。
> 按项目约定**不去动它**；全程只用 `-s 20080411` 明确指定设备（本板 IP = `192.168.1.27`）。

## 11.5 规划修订清单（照着改，别按旧版做）

| 原规划条目 | 修订 |
|---|---|
| §2.4 模板裁剪（待验证） | ★ **已验证**，写法固定为 `namespace + list + tojson`；595 B / 4% |
| §6 P0"响应体积实测未做" | ✅ 已完成，见 §11.2 |
| §7.4 **R13 JSON 库决策** | 结论倾向明确：要处理的是**小 JSON**（最大 14.4 KB）⇒ **自研极简解析器（~400 行）足够**，不引新依赖；先 `flythings_package_search` 快速确认一下有没有现成的即可 |
| §7.1 **R1 WS 自研（高）** | **降为中**：80 行手写客户端已打通握手 / auth / 订阅 / 注册表，方案可行性已证 |
| §7.1 **R3 内存（高）** | **降为低**：36 实体、全量 14.4 KB；但"定长缓冲 + 上限 WARN"纪律保留 |
| §7.2 **R2 中文名字库（高）** | **确认成立、但量级很小**：去重汉字 **23 个** ⇒ 从"高成本"变"一个脚本就完事"，**但仍必须做**（缺字形是整字消失） |
| §7.3 R7 实体爆炸 | **不成立**（36 个） |
| §7.3 **R5 HA 版本差异** | 按 **2026.9.3** 实测，不照文档假设 |
| §3.1 L2 房间分组 | 需先在 HA 里分配区域；否则本地分组兜底（§11.3(2)） |
| §3.2 domain 控件表 | **新增两行要求**：① 必须能画 `unavailable`/`unknown` 灰态（实测 17/36 不是 on/off）；② 单位按 HA 的 US customary 显示（°F/mi/gal），要 °C 得在 HA 侧改单位制或设备侧换算 |
| §3.5 反向集成 | 从"远期"提到 **P4**（MQTT 发现路径已趟通，见 §11.3(3)） |
| §2 通道表 | 新增第 5 行 MQTT（可选加速/通知；有 ACL 限制；只允许一条做"写"） |
| §4.4 token 录入 | 不变（网页配对为主），但**现在 P0/P1 阶段先用 LLAT 烧进 `/data/ha.conf` 更快** |

## 11.6 ★ 设备侧实测：板子到 HA 的通路已验证（2026-09-23 下午）

板子 `20080411` 已联网（`wlan0 = 192.168.1.27`），L3 通。接下来要证的是 **TCP + HTTP**。
板子 shell 里**没有 curl / wget / nc**（`ls /bin` 只有 toolbox 那点东西 ⇒ 别指望 shell 侧探测），
所以改用它**自带的 HTTP 通路**做真实请求：`/tmp/pg_autostart` 注入 `ffprobe <url>`
（`pg::Ff::probe` → ffmpeg 直接发 HTTP GET）。

| 目标 | 日志实测 | 结论 |
|---|---|---|
| `http://192.168.1.188:8123/api/` | `PgFf: 打开失败 '…/api/'：**Server returned 401 Unauthorized (authorization failed)** (-825242872)，耗时 158ms → 重试 50ms` | ✅ **HA 应答了**：TCP 通 + HTTP 通 + 401 是**受控应答**（不是超时、不是拒绝） |
| `http://192.168.1.188:8123/` | `PgFf: 打开失败 '…/'：**Invalid data found when processing input**，耗时 56ms → 20ms` | ✅ 拿到 **200 + 非媒体内容**（HA 前端 HTML），与上一条互相印证 |
| `http://192.168.1.199:8123/api/`（不存在的主机） | `PgFf[ffmpeg]: Connection to tcp://192.168.1.199:8123 failed: **Connection refused**`，12 ms | ✅ **自证**：这条探针**能报出失败**，而且能区分「401 受控应答 / 连接被拒」——上面两条的结论因此可信 |

**顺带得到的三条事实**：

1. **`PgFf` 会预热 CA 证书（188 900 字节）** —— 说明 ffmpeg 侧的 https 证书链路是通的，
   将来若 HA 上 TLS，设备侧有现成基础（但本实例是 http，不受影响）。
2. ⚠️ **`ffprobe` QA 命令的 `info` 回填是空的**（日志里是 `ffprobe 失败 —— ` 后面什么都没有），
   真正有用的信息在 `PgFf:` 自己的日志里。⇒ 后续要脚本化判据时，**别去解析 `ffprobe` 的 info，
   要锚定 `PgFf:` 行**（这是"判据要落在真的会出现的行上"的老坑）。
3. **实体数会变**：两次探测同一台 HA，实体数 **36 → 34**、`/api/states` 14 476 → 13 923 字节。
   ⇒ 再次印证项目纪律：**上限/数量一律不写死**，列表与缓存都要能容纳变化。

**设备侧 P0 剩余项**：控制闭环 —— 已于 §11.7 完成。

## 11.7 ★ 控制闭环验收（2026-09-23 16:00，已授权真机驱动）

验收脚本 `tools/ha_verify.py`（`--actuate` 才真驱动，**每个实体都会还原**），
逐步诊断 `tools/ha_toggle_once.py`，MQTT 侧取证 `tools/mqtt_watch.py`。
原始报告：`docs/ha-verify.md`、`docs/mqtt-watch.md`、`docs/toggle-once.md`。

### (1) 正例：一次开关的完整往返

| 实体 | 拨前实测 | 动作 | 服务响应 | 命令→观测到变化 | 还原确认 |
|---|---|---|---|---|---|
| `switch.z20_smart_panel_ke_ting_deng` | on | turn_off | `[]` | ✅ **184 ms** | ✅ 双读一致 |
| `switch.z20_smart_panel_wo_shi_deng` | off | turn_on | `[]` | ✅ **182 ms** | ✅ 双读一致 |
| `switch.z20_smart_panel_deng_dai` | off | turn_on | `[]` | ✅ **200 ms** | ✅ 双读一致 |
| `switch.smart_panel_test_ke_ting_deng`（设备离线） | off | turn_on | `[]` | ❌ **5 s 超时**（状态始终不变） | ✅ |

单实体逐步观察（`ha_toggle_once.py`）：命令 → 观测到 `off` = **210 ms**；
MQTT 侧时序（`mqtt_watch.py`）看得更细：
**t=3.07 s HA 发出 `.../relay_1/command` = `ON` → t=3.16 s 面板回 `.../relay_1/state` = `ON`**
⇒ **设备侧响应约 90 ms，端到端约 200 ms。**

### (2) ★★ 三条被验收推翻/强化的结论

| # | 实测 | 对规划的修改 |
|---|---|---|
| 1 | **服务响应恒为 `[]`**（MQTT 实体的状态是异步回包才变的） | ★ **§2.4 重写**：删掉"服务响应即真相回填"，改成「乐观态 + 短轮询确认（实测 156~210 ms）+ 5 s 超时回滚」 |
| 2 | **给不存在的实体发命令 = `200` + `[]`**，与"成功但未回包"**无法区分** | ★ **不能用响应判断成败**；只能靠回读。UI 的"成功"必须是"回读到了目标状态"，不是"HTTP 200" |
| 3 | **`unavailable` 实体命令被吞**：3 个离线实体全部 `200` 但状态永久不变 | 灰态**必须禁点**；并且要把"回读超时"这条路径当**常态**来设计，不是异常 |

### (3) 错误响应矩阵（UI 要能分开说，不许都报"连接失败"）

| 场景 | HTTP | 响应体 | UI 该说什么 |
|---|---|---|---|
| 实体不存在 | **200** | `[]` | ⚠️ **静默**！只能靠回读超时发现 ⇒ "设备可能已被移除，请刷新" |
| 服务不存在 | 400 | `400: Bad Request` | HA 版本不支持该操作 |
| 域不存在 | 400 | `400: Bad Request` | 同上 |
| body 缺 `entity_id` | 400 | `400: Bad Request` | 本地 bug，不是网络问题 |

### (4) 场景按钮：**一次调用会连带改多个实体**

按 `button.z20_smart_panel_home` → HTTP 200（**响应里 1 条状态**，与 `switch` 的 `[]` 不同），
并连带把 `switch.z20_smart_panel_wo_shi_deng` 从 `off` 改成 `on`（已还原）。
⇒ 本地操作后的刷新**不能只刷被点的那一个实体**。

### (5) ★ 验收工具自身的两个坑（值得记）

1. **观测窗口太小 = 双重假结论**：初版 `--settle-ms` 实际只跑了一两轮，
   于是"命令生效了"被误判成"超时"，而"还原校验"因为**读到的还是旧值**而**假通过**。
   ⇒ 现在脚本**强制窗口下限 1500 ms**，并在报告头把窗口值打出来；还原改成**双读确认**。
   ★ 教训与 §43 那条同源：**判据设置错了，比没有判据更危险**（它会给你绿灯）。
2. **基准不能用"开场快照"**：现在改成**拨之前当场读一次**（`拨前实测` 列）。
   实测两次运行的基线就不同（上一轮脚本的假还原把状态留在了别处）。

> 收尾：验收结束后已用 `tools/ha_set.py`（显式设值 + 回读确认）
> 把三路继电器**还原到验收前的状态**（`客厅灯=off / 卧室灯=on / 灯带=on`），
> 确认耗时 156~178 ms。

