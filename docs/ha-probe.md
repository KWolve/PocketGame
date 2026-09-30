# HA P0 探针结果

目标：`http://192.168.1.188:8123`（由 `tools/ha_probe.py` 实测，脚本只读，凭据走环境变量）

## 1. 端口可达性

| 端口 | 结果 |
|---|---|
| 8123 | open |
| 18083 | open |
| 1883 | open |
| 8083 | open |
| 8883 | open |

## 2. 未认证访问 `GET /api/`

- HTTP 401（**401 = HA 在跑且要求认证，这是期望值**）
- body: `401: Unauthorized`

## 3. 认证

- 使用**长期访问令牌（LLAT）**直接做 Bearer（**设备侧要走的就是这条路**）
- 令牌指纹：`eyJhbGci…WsIDtc`（长度 183，sha256 前 8 位 `6888fbb0`）

## 4. `GET /api/config`

| 项 | 值 |
|---|---|
| version | 2026.9.3 |
| location_name | 我的家 |
| time_zone | Asia/Shanghai |
| country | CN |
| language | zh-Hans |
| unit_system | {"length": "mi", "accumulated_precipitation": "in", "area": "ft²", "mass": "lb", "pressure": "psi", "temperature": "°F", "volume": "gal", "wind_speed": "mph"} |
| components 数 | 147 |

- 关注组件：`websocket_api, camera, conversation, mqtt.light, mqtt.button, mqtt, mqtt.sensor, mqtt.switch, mqtt.binary_sensor`

## 5. `GET /api/states`（全量）

- HTTP 200，**34 个实体**，**13923 字节**（13.6 KB），耗时 15 ms

### 5.1 domain 分布（前 25）

| domain | 数量 |
|---|---|
| `sensor` | 11 |
| `button` | 7 |
| `switch` | 6 |
| `conversation` | 1 |
| `zone` | 1 |
| `sun` | 1 |
| `event` | 1 |
| `person` | 1 |
| `todo` | 1 |
| `tts` | 1 |
| `weather` | 1 |
| `binary_sensor` | 1 |
| `light` | 1 |

- domain 种类共 **13** 个

### 5.2 state 取值分布

| state | 数量 |
|---|---|
| `on` | 2 |
| `off` | 2 |
| `unavailable` | 5 |
| `unknown` | 12 |
| `其它` | 13 |

- ⚠️ **有 17 个实体不是 on/off** ⇒ 磁贴必须能画「离线/未知」灰态，并且**不能把它当成 off**（点了没反应会像 bug）

### 6. ★ 实体中文名占比（决定字库工作量）

- 含中文的 friendly_name：**10 / 34**（29%）
- **去重汉字数：20 个**（这就是要补进项目字库的量级）
- 样例：`我的家`、`谷歌翻译 en com Google Translate en com`、`Forecast 我的家`、`Z20 HA Switch 吊灯`、`Z20 HA Switch 风扇`、`Z20 HA Switch 电脑开关`、`Smart Panel Test 客厅灯`、`Z20 Smart Panel 客厅灯`、`Z20 Smart Panel 卧室灯`、`Z20 Smart Panel 灯带`
- 汉字全集（可直接喂给 tools/gen_font.py）：

```
关卧厅吊客室家带开我扇歌灯电的翻脑译谷风
```

### 7. ★ `POST /api/template` 裁剪响应（内存红线的关键判据）

| 模板写法 | HTTP | 字节 | 耗时 | 说明 |
|---|---|---|---|---|
| A: namespace + list 追加 + tojson | 200 | 595 | 16 ms | 可用 |
| B: namespace + dict.update（预期被沙箱拒） | 400 | 111 | 19 ms | Error rendering template: SecurityError: access to attribute 'update'  |
| C: 手工拼 JSON 文本 | 400 | 75 | 28 ms | Error rendering template: TemplateSyntaxError: unexpected '%' |

- 取 7 个可控实体：**0.6 KB**，相比全量 13.6 KB 降到 **4.3%**
- 可用写法 = A: namespace + list 追加 + tojson；样例响应：

```json
[["light.z20_ha_switch_diao_deng", "unavailable", "Z20 HA Switch \u540a\u706f"], ["switch.z20_ha_switch_feng_shan", "unavailable", "Z20 HA Switch \u98ce\u6247"], ["switch.z20_ha_switch_dian_nao_kai_guan", "unavailable", "Z20 HA Switch \u7535\u8111\u5f00\u5173"], ["switch.smart_panel_test_ke_ting_deng", "off", "Smart Panel Test \u5ba2\u5385\u706f"], ["switch.z20_smart_panel_ke_ting_deng", "off", "Z20 Smart Panel \u5ba2\u5385\u706f"], ["switch.z20_smart_panel_wo_shi_deng", "on", "Z20 Smart Panel \u5367\u5ba4\u706f"], ["switch.z20_smart_panel_deng_dai", "on", "Z20 Smart Panel \u706f\u5e26"]]
```

### 8. 可控实体清单（前 30，`/api/states` 的口径）

| entity_id | state | friendly_name |
|---|---|---|
| `light.z20_ha_switch_diao_deng` | unavailable | Z20 HA Switch 吊灯 |
| `switch.z20_ha_switch_feng_shan` | unavailable | Z20 HA Switch 风扇 |
| `switch.z20_ha_switch_dian_nao_kai_guan` | unavailable | Z20 HA Switch 电脑开关 |
| `switch.smart_panel_test_ke_ting_deng` | off | Smart Panel Test 客厅灯 |
| `switch.z20_smart_panel_ke_ting_deng` | off | Z20 Smart Panel 客厅灯 |
| `switch.z20_smart_panel_wo_shi_deng` | on | Z20 Smart Panel 卧室灯 |
| `switch.z20_smart_panel_deng_dai` | on | Z20 Smart Panel 灯带 |

## 9. ★ WebSocket 实测（实时化这一片的风险判据）

| 项 | 结果 |
|---|---|
| 握手 101 | True (13 ms) |
| 首帧 | auth_required |
| auth_ok | True |
| 订阅 state_changed | True |
| ha_version | 2026.9.3 |
| area_registry | 4 条 |
| device_registry | 8 条 |
| entity_registry | 34 条 |

- **房间（area）清单**：`客厅`、`厨房`、`卧室`、`钟总办公室`

### 9.1 ★ 「房间分组」可行性实测（entity_registry + device_registry + area_registry 三表拼接）

| 房间 | 实体数 | 实体 |
|---|---|---|
| **（未分配房间）** | 34 | `binary_sensor.sun_solar_rising`, `sensor.sun_next_dawn`, `sensor.sun_next_dusk`, `sensor.sun_next_midnight`, `sensor.sun_next_noon`, `sensor.sun_next_rising`, `sensor.sun_next_setting`, `sensor.sun_solar_elevation` |

- ⇒ ★ **拼不出房间分组**：注册表里没有 area 归属，L2 要退化成「本地自定义分组」

### 9.2 自证：注册表原始字段（证明「未分配」不是我字段名写错）

- `entity_registry` 第 1 条的 area 相关字段：

```json
{
 "entity_id": "binary_sensor.sun_solar_rising",
 "area_id": null,
 "device_id": "35d198bf33b2cdd7308630d2b7cac3cf",
 "disabled_by": "integration",
 "hidden_by": null
}
```
- `device_registry` 第 1 条的 area 相关字段：

```json
{
 "id": "35d198bf33b2cdd7308630d2b7cac3cf",
 "name": "Sun",
 "area_id": null,
 "name_by_user": null
}
```
- 统计：`entity_registry` 里 **0/34** 条有 area_id；`device_registry` 里 **1/8** 条有 area_id
- 字段名用的是 HA 官方 `area_id`（WS 注册表接口），4 个房间（客厅、厨房、卧室、钟总办公室）本身是存在的 ——
  **是「房间建好了但没把设备放进去」**，不是解析错。
  ⇒ 要做房间分组面板，得先在 HA 前端把设备分配到区域（人工，5 分钟）；
    否则 L2 退化成「本地自定义分组」（规划里已经留了这条兜底）。


## 10. 结论与对策（自动推导）

| 判据 | 实测 | 对规划的影响 |
|---|---|---|
| HA 版本 | 2026.9.3 | R14 已消除：按此版本实测接口，不照文档假设 |
| 实体总数 | 34 | 规模小，列表页压力不大 |
| 中文实体名 | 10 个（29%） | ★ 字库链路（tools/gen_ha_font.py）**必需**，不能省（规划 R2） |
| 全量 states | 13.6 KB | 体积可接受 |
| WS | 可用 | 实时化可行，P3 按计划做 |

