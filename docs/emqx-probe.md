# EMQX（MQTT Broker）P0 探针结果

目标：`http://192.168.1.188:18083`（由 `tools/emqx_probe.py` 实测，只读）

## 1. Dashboard API

- `POST /api/v5/login` → HTTP 200
- EMQX 版本：**5.8.7**
- 已取得 API token：是

- `GET /nodes` → HTTP 200（1 条）

| node | version | uptime |
|---|---|---|
| emqx@172.28.10.10 | 5.8.7 | 72511598 |

- `GET /listeners` → HTTP 200（4 条）

| id | type | current_connections | running | bind |
|---|---|---|---|---|
| tcp:default | tcp |  |  | 0.0.0.0:1883 |
| ssl:default | ssl |  |  | 0.0.0.0:8883 |
| ws:default | ws |  |  | 0.0.0.0:8083 |
| wss:default | wss |  |  | 0.0.0.0:8084 |


- `GET /clients` → HTTP 200（2 条，共 2）

| clientid | username | ip_address | connected |
|---|---|---|---|
| smartpanel_PANEL-B9D9223B | admin | 192.168.1.177 | True |
| 51Ti3JgxeTbVBCTUAkkFCp | None | 172.28.10.1 | True |


- `GET /subscriptions` → HTTP 200（94 条，共 94）

| clientid | topic | qos |
|---|---|---|
| 51Ti3JgxeTbVBCTUAkkFCp | zk_switch_FACD/switch/switch_3/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | zk_switch_FACD/switch/switch_2/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | zk_switch_FACD/switch/switch_1/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | zk_switch_FACD/status | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | ulanzi_1c10/status | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | ulanzi_1c10/info/prefix | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | tasmota/discovery/# | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel_test/switch/relay_1/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel_test/status | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/switch/relay_3/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/switch/relay_2/state | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/switch/relay_1/state | 0 |
| smartpanel_PANEL-B9D9223B | smartpanel/PANEL-B9D9223B/switch/+/command | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/sensor/temperature | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/sensor/humidity | 0 |
| smartpanel_PANEL-B9D9223B | smartpanel/PANEL-B9D9223B/scene/+/command | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | smartpanel/PANEL-B9D9223B/availability | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | prism/hello | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | pglab/discovery/# | 0 |
| 51Ti3JgxeTbVBCTUAkkFCp | inels/status/# | 0 |


- **全部订阅 topic 去重（94 个）**：
  - `/greencell/broadcast/device`
  - `cloudapp/QBUSMQTTGW/+/state`
  - `cloudapp/QBUSMQTTGW/config`
  - `cloudapp/QBUSMQTTGW/state`
  - `drop_connect/discovery/#`
  - `dsmr/#`
  - `esphome/discover/#`
  - `fully/deviceInfo/+`
  - `homeassistant/alarm_control_panel/+/+/config`
  - `homeassistant/alarm_control_panel/+/config`
  - `homeassistant/binary_sensor/+/+/config`
  - `homeassistant/binary_sensor/+/config`
  - `homeassistant/button/+/+/config`
  - `homeassistant/button/+/config`
  - `homeassistant/camera/+/+/config`
  - `homeassistant/camera/+/config`
  - `homeassistant/climate/+/+/config`
  - `homeassistant/climate/+/config`
  - `homeassistant/cover/+/+/config`
  - `homeassistant/cover/+/config`
  - `homeassistant/date/+/+/config`
  - `homeassistant/date/+/config`
  - `homeassistant/datetime/+/+/config`
  - `homeassistant/datetime/+/config`
  - `homeassistant/device/+/+/config`
  - `homeassistant/device/+/config`
  - `homeassistant/device_automation/+/+/config`
  - `homeassistant/device_automation/+/config`
  - `homeassistant/device_tracker/+/+/config`
  - `homeassistant/device_tracker/+/config`
  - `homeassistant/event/+/+/config`
  - `homeassistant/event/+/config`
  - `homeassistant/fan/+/+/config`
  - `homeassistant/fan/+/config`
  - `homeassistant/humidifier/+/+/config`
  - `homeassistant/humidifier/+/config`
  - `homeassistant/image/+/+/config`
  - `homeassistant/image/+/config`
  - `homeassistant/infrared/+/+/config`
  - `homeassistant/infrared/+/config`
  - `homeassistant/lawn_mower/+/+/config`
  - `homeassistant/lawn_mower/+/config`
  - `homeassistant/light/+/+/config`
  - `homeassistant/light/+/config`
  - `homeassistant/lock/+/+/config`
  - `homeassistant/lock/+/config`
  - `homeassistant/notify/+/+/config`
  - `homeassistant/notify/+/config`
  - `homeassistant/number/+/+/config`
  - `homeassistant/number/+/config`
  - `homeassistant/scene/+/+/config`
  - `homeassistant/scene/+/config`
  - `homeassistant/select/+/+/config`
  - `homeassistant/select/+/config`
  - `homeassistant/sensor/+/+/config`
  - `homeassistant/sensor/+/config`
  - `homeassistant/siren/+/+/config`
  - `homeassistant/siren/+/config`
  - `homeassistant/switch/+/+/config`
  - `homeassistant/switch/+/config`

## 2. 原生 MQTT 连接 + 嗅探（手写 CONNECT/SUBSCRIBE）

- `CONNECT` → return code **0**（0 = 接受；4 = 用户名/密码错；5 = 未授权）
- 握手耗时 1 ms
- `SUBACK`：mid=1 -> ['0x80']; mid=2 -> ['0x0']（granted qos 里 **0x80 = 该主题被 ACL 拒绝**）
- 嗅探 15 秒，收到 **41 条**消息

| topic | payload 摘要 |
|---|---|
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_1/config` | {"name":"home","uniq_id":"smartpanel_PANEL-B9D9223B_scene_1","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_2/config` | {"name":"away","uniq_id":"smartpanel_PANEL-B9D9223B_scene_2","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_3/config` | {"name":"sleep","uniq_id":"smartpanel_PANEL-B9D9223B_scene_3","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_4/config` | {"name":"movie","uniq_id":"smartpanel_PANEL-B9D9223B_scene_4","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_5/config` | {"name":"read","uniq_id":"smartpanel_PANEL-B9D9223B_scene_5","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_6/config` | {"name":"party","uniq_id":"smartpanel_PANEL-B9D9223B_scene_6","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_7/config` | {"name":"night","uniq_id":"smartpanel_PANEL-B9D9223B_scene_7","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/device/ccc4b2441c10/config` | {"dev":{"ids":"ccc4b2441c10","name":"Ulanzi Pixel Clock","mf":"Ulanzi","mdl":"TC002","sw":"9.1.4","hw":"V1.0.1 |
| `homeassistant/light/zk_switch_FACD_switch_1/config` | {"name":"吊灯","uniq_id":"zk_switch_FACD_switch_1","avty_t":"zk_switch_FACD/status","pl_avail":"online","pl_not_ |
| `homeassistant/sensor/smartpanel_PANEL-B9D9223B_humidity/config` | {"name":"湿度","uniq_id":"smartpanel_PANEL-B9D9223B_humidity","p":"sensor","dev_cla":"humidity","unit_of_meas":" |
| `homeassistant/sensor/smartpanel_PANEL-B9D9223B_temperature/config` | {"name":"温度","uniq_id":"smartpanel_PANEL-B9D9223B_temperature","p":"sensor","dev_cla":"temperature","unit_of_m |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_1/config` | {"name":"客厅灯","uniq_id":"smartpanel_PANEL-B9D9223B_relay_1","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_2/config` | {"name":"卧室灯","uniq_id":"smartpanel_PANEL-B9D9223B_relay_2","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_3/config` | {"name":"灯带","uniq_id":"smartpanel_PANEL-B9D9223B_relay_3","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/sw |
| `homeassistant/switch/smartpanel_test_relay_1/config` | {"name": "\u5ba2\u5385\u706f", "uniq_id": "smartpanel_test_relay_1", "p": "switch", "stat_t": "smartpanel_test |
| `homeassistant/switch/zk_switch_FACD_switch_2/config` | {"name":"风扇","uniq_id":"zk_switch_FACD_switch_2","avty_t":"zk_switch_FACD/status","pl_avail":"online","pl_not_ |
| `homeassistant/switch/zk_switch_FACD_switch_3/config` | {"name":"电脑开关","uniq_id":"zk_switch_FACD_switch_3","avty_t":"zk_switch_FACD/status","pl_avail":"online","pl_no |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_1/config` | {"name":"客厅灯","uniq_id":"smartpanel_PANEL-B9D9223B_relay_1","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_2/config` | {"name":"卧室灯","uniq_id":"smartpanel_PANEL-B9D9223B_relay_2","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_3/config` | {"name":"灯带","uniq_id":"smartpanel_PANEL-B9D9223B_relay_3","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/sw |
| `homeassistant/sensor/smartpanel_PANEL-B9D9223B_temperature/config` | {"name":"温度","uniq_id":"smartpanel_PANEL-B9D9223B_temperature","p":"sensor","dev_cla":"temperature","unit_of_m |
| `homeassistant/sensor/smartpanel_PANEL-B9D9223B_humidity/config` | {"name":"湿度","uniq_id":"smartpanel_PANEL-B9D9223B_humidity","p":"sensor","dev_cla":"humidity","unit_of_meas":" |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_1/config` | {"name":"home","uniq_id":"smartpanel_PANEL-B9D9223B_scene_1","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_2/config` | {"name":"away","uniq_id":"smartpanel_PANEL-B9D9223B_scene_2","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_3/config` | {"name":"sleep","uniq_id":"smartpanel_PANEL-B9D9223B_scene_3","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_4/config` | {"name":"movie","uniq_id":"smartpanel_PANEL-B9D9223B_scene_4","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_5/config` | {"name":"read","uniq_id":"smartpanel_PANEL-B9D9223B_scene_5","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/s |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_6/config` | {"name":"party","uniq_id":"smartpanel_PANEL-B9D9223B_scene_6","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/button/smartpanel_PANEL-B9D9223B_scene_7/config` | {"name":"night","uniq_id":"smartpanel_PANEL-B9D9223B_scene_7","p":"button","cmd_t":"smartpanel/PANEL-B9D9223B/ |
| `homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_1/config` | {"name":"客厅灯","uniq_id":"smartpanel_PANEL-B9D9223B_relay_1","p":"switch","stat_t":"smartpanel/PANEL-B9D9223B/s |

- **去重 topic 数：17**

### 2.1 去重 topic 全量（前 60）

```
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_1/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_2/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_3/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_4/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_5/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_6/config
homeassistant/button/smartpanel_PANEL-B9D9223B_scene_7/config
homeassistant/device/ccc4b2441c10/config
homeassistant/light/zk_switch_FACD_switch_1/config
homeassistant/sensor/smartpanel_PANEL-B9D9223B_humidity/config
homeassistant/sensor/smartpanel_PANEL-B9D9223B_temperature/config
homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_1/config
homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_2/config
homeassistant/switch/smartpanel_PANEL-B9D9223B_relay_3/config
homeassistant/switch/smartpanel_test_relay_1/config
homeassistant/switch/zk_switch_FACD_switch_2/config
homeassistant/switch/zk_switch_FACD_switch_3/config
```

