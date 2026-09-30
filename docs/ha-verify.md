# HA 控制闭环验收

目标 `http://192.168.1.188:8123`（令牌指纹 `6888fbb0`，脚本 `tools/ha_verify.py`）

- 模式：**实际驱动设备（每个都会还原）**
- 观测窗口 `--settle-ms` = **5000 ms**（强制不低于 1500 ms —— 首次踩过：窗口太小会把「命令已生效」误判成「超时」，而且会让还原校验**假通过**）

## 0. 基线快照

- `GET /api/states` → HTTP 200，**34 个实体**，6 ms

| 可开关实体 | 当前 state | friendly_name |
|---|---|---|
| `light.z20_ha_switch_diao_deng` | unavailable | Z20 HA Switch 吊灯 |
| `switch.z20_ha_switch_feng_shan` | unavailable | Z20 HA Switch 风扇 |
| `switch.z20_ha_switch_dian_nao_kai_guan` | unavailable | Z20 HA Switch 电脑开关 |
| `switch.smart_panel_test_ke_ting_deng` | off | Smart Panel Test 客厅灯 |
| `switch.z20_smart_panel_ke_ting_deng` | on | Z20 Smart Panel 客厅灯 |
| `switch.z20_smart_panel_wo_shi_deng` | off | Z20 Smart Panel 卧室灯 |
| `switch.z20_smart_panel_deng_dai` | off | Z20 Smart Panel 灯带 |

## 1. 正例：一次开关的完整往返（★ 同时验「服务响应即真相回填」）

| 实体 | 拨前实测 | 动作 | 服务响应里的 state | 服务往返 | 观察到变化 | 轮询次数 | 还原确认 |
|---|---|---|---|---|---|---|---|
| `light.z20_ha_switch_diao_deng` | unavailable | （跳过：非 on/off，不动它） | | | | | |
| `switch.z20_ha_switch_feng_shan` | unavailable | （跳过：非 on/off，不动它） | | | | | |
| `switch.z20_ha_switch_dian_nao_kai_guan` | unavailable | （跳过：非 on/off，不动它） | | | | | |
| `switch.smart_panel_test_ke_ting_deng` | off | turn_on | **None** | 2 ms | ❌ **超时**（未变成 on） | 30 | ✅ 双读一致 |
| `switch.z20_smart_panel_ke_ting_deng` | on | turn_off | **None** | 5 ms | ✅ 184 ms | 2 | ✅ 双读一致 |
| `switch.z20_smart_panel_wo_shi_deng` | off | turn_on | **None** | 2 ms | ✅ 182 ms | 2 | ✅ 双读一致 |
| `switch.z20_smart_panel_deng_dai` | off | turn_on | **None** | 3 ms | ✅ 200 ms | 2 | ✅ 双读一致 |

- 总体：**★ 有失败项，见上表 ❌**
- ★ **结论（§2.4 的判据）**：服务响应是 **空数组 `[]`** ⇒ **不能靠服务响应回填**。原因是这类实体由 MQTT 驱动，状态变化是**异步**的（HA 发出 command 后要等设备回 state），服务调用在那一刻「还没变化」所以就返回空。⇒ UI 必须按 **乐观态 + 短轮询确认（实测 200~300 ms）** 来写，并且要有**超时回滚**（设备不应答时状态永远不变，见 §3）

## 2. 反例：三种失败长什么样（UI 要能区分，不能都是「连接失败」）

| 场景 | HTTP | 响应体（截断） | UI 该怎么说 |
|---|---|---|---|
| 实体不存在 | **200** | `[]` | 该设备已从 HA 移除，请刷新列表 |
| 服务不存在 | **400** | `400: Bad Request` | HA 版本不支持该操作（别笼统说连接失败） |
| 域不存在 | **400** | `400: Bad Request` | 同上 |
| body 里没 entity_id | **400** | `400: Bad Request` | 本地 bug，不是网络问题 |

## 3. `unavailable` 的实体调用服务会怎样（决定灰态要不要禁点）

| 实体 | 调用前 | 调用结果 HTTP | 调用后 | 变化 |
|---|---|---|---|---|
| `light.z20_ha_switch_diao_deng` | unavailable | **200** | unavailable | 无变化（命令被吞） |
| `switch.z20_ha_switch_feng_shan` | unavailable | **200** | unavailable | 无变化（命令被吞） |
| `switch.z20_ha_switch_dian_nao_kai_guan` | unavailable | **200** | unavailable | 无变化（命令被吞） |

- ⇒ **命令返回 200 但状态不变** ⇒ UI 必须**禁点灰态**，否则用户会以为按钮坏了（HA 侧设备离线时，服务调用是「接受但不生效」）

## 4. 场景按钮：一次调用会不会连带改别的实体

- 按 `button.z20_smart_panel_home`（Z20 Smart Panel home）
- HTTP 200，4 ms，响应里 1 条状态

| 被连带改变的实体 | 前 | 后 |
|---|---|---|
| `switch.z20_smart_panel_wo_shi_deng` | off | on |

- ⇒ **一次服务调用会连带改多个实体** ⇒ 回填要么用服务响应、要么全量刷当前页

- 还原被改动的实体：
  - `switch.z20_smart_panel_wo_shi_deng` -> off
- 还原校验：✅ 全部回到原值

