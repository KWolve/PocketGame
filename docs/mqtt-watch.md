# MQTT 侧观察：一次 HA 服务调用到底发出了什么

- 订阅 `smartpanel/#`，观察 14 秒
- 在第 3.0 秒调用 `switch/turn_on` → `switch.z20_smart_panel_ke_ting_deng`

- MQTT CONNECT 返回码 **0**（0 = 接受）
- 调用前 `switch.z20_smart_panel_ke_ting_deng` = **off**
- 服务调用 HTTP **200**，响应体：`[]`
- 调用后 `switch.z20_smart_panel_ke_ting_deng` = **on**

## MQTT 报文时序（time / topic / payload）

| t(s) | topic | payload |
|---|---|---|
| 0.04 | `SUBACK` | `mid=1 granted=['0x0']` |
| 0.04 | `smartpanel/PANEL-B9D9223B/availability` | `online` |
| 0.04 | `smartpanel/PANEL-B9D9223B/sensor/humidity` | `58.0` |
| 0.04 | `smartpanel/PANEL-B9D9223B/sensor/temperature` | `26.5` |
| 0.04 | `smartpanel/PANEL-B9D9223B/status` | `{"dev":"SmartHomePanel","id":"PANEL-B9D9223B","ip":"192.168.1.177","model":"SW48480040D1/Z20","relays":[false,false,false],"relay_names":["客厅灯","卧室灯","灯带"],"scene":"away","temp":26` |
| 0.04 | `smartpanel/PANEL-B9D9223B/switch/relay_1/state` | `OFF` |
| 0.04 | `smartpanel/PANEL-B9D9223B/switch/relay_2/state` | `OFF` |
| 0.04 | `smartpanel/PANEL-B9D9223B/switch/relay_3/state` | `OFF` |
| 3.07 | `smartpanel/PANEL-B9D9223B/switch/relay_1/command` | `ON` |
| 3.16 | `smartpanel/PANEL-B9D9223B/switch/relay_1/state` | `ON` |

## 判定（自动推导）

| 观察到的 | 数量 | 推论 |
|---|---|---|
| 命令类 topic（`*/command`、`*/set`） | 1 | HA 的命令**确实发出去了** ⇒ 不该怪 HA 的服务调用链路 |
| 状态类 topic（`*/state`） | 4 | 设备**回了状态** ⇒ 若 HA 仍不变，是 HA 侧映射问题 |

- ⇒ 见上表两行推论的组合。

