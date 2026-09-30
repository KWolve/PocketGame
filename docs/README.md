# 文档索引

这个目录里的文档都是**真机上实测出来的**：每条结论背后有一次失败、一次抓屏或一次逐像素比对。
写它们的目的是让下一个人（包括三个月后的自己）不用再把同样的坑踩一遍。

---

## 入门（先读这几篇）

| 文档 | 内容 |
|---|---|
| [`../README.md`](../README.md) | 项目总览、功能清单、目录结构 |
| [`BUILD.md`](BUILD.md) | 编译 / 打包 / 固化烧写，以及烧写相关的三个坑 |
| [`DEPENDENCIES.md`](DEPENDENCIES.md) | 厂商 SDK 与第三方库的获取与放置 |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | 二次开发：加游戏 / 加应用 / 改 UI / 改字库 |
| [`screenshots/`](screenshots/) | 真机截图索引 |

---

## 平台与硬件

| 文档 | 内容 |
|---|---|
| [`hardware-reference.md`](hardware-reference.md) | 板子硬件参数、外设、分区 |
| [`audio-and-input-hardware.md`](audio-and-input-hardware.md) | 音频链路与输入设备（按键 / 触摸节点）实测 |
| [`audio-output.md`](audio-output.md) | 音频输出通路与音量控制 |
| [`battery.md`](battery.md) | 电量读取与状态栏图标 |
| [`disp-layer.md`](disp-layer.md) | 显示图层（视频层 / UI 层）与叠加关系 |
| [`statusbar.md`](statusbar.md) | 全局状态栏（SysApp） |
| [`screensaver.md`](screensaver.md) | 屏保机制与"谁在前台谁说了算" |
| [`display-flip.md`](display-flip.md) | 整屏翻转 / 挂绳倒挂 |
| [`touch-inject.md`](touch-inject.md) | 真触摸注入（MT-A/B 判别与三条铁律） |

## UI 框架与设计

| 文档 | 内容 |
|---|---|
| [`ui-design-baseline.md`](ui-design-baseline.md) | UI 设计基线（尺寸、间距、颜色令牌） |
| [`ui-ios-redesign.md`](ui-ios-redesign.md) | 从"方头方脑"改到手机观感的整轮改版记录 |
| [`canvas-font.md`](canvas-font.md) | 自研软渲染画布上的文字渲染与字号档位制 |
| [`nine-patch-inset-and-aa.md`](nine-patch-inset-and-aa.md) | 九宫格 1px 内缩 / 抗锯齿的实测结论 |
| [`launcher-appgrid.md`](launcher-appgrid.md) | 启动器应用网格：`slot` 与下标的区别、加应用的完整清单 |
| [`settings-navibar-swipe.md`](settings-navibar-swipe.md) | 系统设置 / 导航栏 / 右滑返回 |
| [`page-split-plan.md`](page-split-plan.md) | 主界面拆分成多 ftu 的方案与过程 |
| [`ui-overlay-audit-2026-09-16.md`](ui-overlay-audit-2026-09-16.md) | 浮层遮挡审计（被导航栏盖住的东西不是 bug） |
| [`asset-audit-2026-09-16.md`](asset-audit-2026-09-16.md) | 素材审计报告 |
| [`res-usage-audit-2026-09-16.md`](res-usage-audit-2026-09-16.md) | `/res` 空间占用审计（分区只有 7,995,392 字节） |

## 游戏

| 文档 | 内容 |
|---|---|
| [`games-review.md`](games-review.md) | 21 款游戏的整体复盘（手感、难度、可玩性） |
| [`game-art-pipeline.md`](game-art-pipeline.md) | 游戏美术生产管线（代码生成素材，无外部美术资源） |
| [`match3.md`](match3.md) | 消消乐：关卡目标、连锁、动画 |
| [`dice-game.md`](dice-game.md) | 摇骰子：离线 3D 烘帧 + 音效 |
| [`kids-games.md`](kids-games.md) | 儿童益智三件套（连线画 / 算术泡泡 / 找不同） |
| [`rhythm-games.md`](rhythm-games.md) | 节奏钢琴 + 打鼓（下落式判定） |

## 工具类应用

| 文档 | 内容 |
|---|---|
| [`clock-suite.md`](clock-suite.md) | 时钟套件：世界时钟 + 闹钟 |
| [`wifi-app.md`](wifi-app.md) | WiFi 设置应用 |
| [`wifi-probe-app.md`](wifi-probe-app.md) / [`wifi_probe_report.md`](wifi_probe_report.md) | 信号探针应用与实测报告 |
| [`iptv.md`](iptv.md) / [`iptv-channels.md`](iptv-channels.md) | 网络电视与频道筛选 |
| [`online-media.md`](online-media.md) | 在线媒体播放（HLS / 直连 / 换台） |
| [`radio-probe.md`](radio-probe.md) | 网络收音机：61 台电台的逐台实测方法 |
| [`camera-radio-app.md`](camera-radio-app.md) | 局域网摄像头查看 + 收音机 |
| [`movie-app.md`](movie-app.md) | 视频播放（循环、接缝判据） |
| [`elf-app.md`](elf-app.md) | 小精灵：实拍视频逐帧抠像成画布精灵 |
| [`pet-3d-render.md`](pet-3d-render.md) | 桌面宠物：headless Chrome + three.js 离线 PBR 渲染 |
| [`dlna.md`](dlna.md) | DLNA 投屏与控制 |
| [`h264-direct.md`](h264-direct.md) | 直接走 H.264 硬解通道（绕开 ffmpeg） |
| [`ime-app.md`](ime-app.md) | 自研输入法：拼音候选、字符集、三层上限 |
| [`toggle-once.md`](toggle-once.md) | 开关类控件的幂等去重（同一按键会投递 4 次） |

## 智能家居（Home Assistant）

| 文档 | 内容 |
|---|---|
| [`ha-app.md`](ha-app.md) | **HA 遥控器**：P0~P2 全过程与实测数据（最完整的一篇） |
| [`ha-integration-plan.md`](ha-integration-plan.md) | 接入方案与真机实测修订 |
| [`ha-probe.md`](ha-probe.md) | HA REST / WebSocket 接口探针 |
| [`ha-verify.md`](ha-verify.md) | 验收脚本 |
| [`ha-ux-redesign.md`](ha-ux-redesign.md) | HA 界面改版（磁贴、灰态、分类型控制图） |
| [`emqx-probe.md`](emqx-probe.md) / [`mqtt-watch.md`](mqtt-watch.md) | MQTT / EMQX 探针与嗅探 |

## 蓝牙

| 文档 | 内容 |
|---|---|
| [`bt-bringup.md`](bt-bringup.md) | 蓝牙 bring-up |
| [`bt-hid-selftest.md`](bt-hid-selftest.md) | HID 自测 |
| [`bt-firmware-request.md`](bt-firmware-request.md) | 固件需求与获取 |

## 构建、升级与复盘

| 文档 | 内容 |
|---|---|
| [`upgrade-pack.md`](upgrade-pack.md) / [`upgrade-package.md`](upgrade-package.md) | 固件包结构与固化流程 |
| [`框架缺陷与踩坑清单-2026-09-27.md`](框架缺陷与踩坑清单-2026-09-27.md) | **框架缺陷与踩坑总清单** |
| [`更正-画布448与scale误记-2026-09-30.md`](更正-画布448与scale误记-2026-09-30.md) | 一处误记的更正（画布尺寸与 scale 上限的真实归属） |
| [`组件卡与自检工具详细设计-2026-09-27.md`](组件卡与自检工具详细设计-2026-09-27.md) | 组件卡与自检工具的设计 |
| [`方案深度检讨与开源方案对比-2026-09-29.md`](方案深度检讨与开源方案对比-2026-09-29.md) | 与开源方案的对比检讨 |
| [`方案再评估-扩展能力与系统借用-2026-09-30.md`](方案再评估-扩展能力与系统借用-2026-09-30.md) | 扩展能力再评估 |
| [`product-feature-proposal.md`](product-feature-proposal.md) | 产品功能提案 |
| [`navibar-battery-appgrid-2026-09-16.md`](navibar-battery-appgrid-2026-09-16.md) | 导航栏 / 电量 / 应用网格的一轮改动记录 |

## 知识库（`kb-*`，可独立阅读的平台笔记）

| 文档 | 内容 |
|---|---|
| [`kb-extension-surface.md`](kb-extension-surface.md) | EasyUI 可扩展面：能改什么、从哪改 |
| [`kb-custom-render-paths.md`](kb-custom-render-paths.md) | 自定义渲染的几条路径与各自代价 |
| [`kb-html2json-static-art.md`](kb-html2json-static-art.md) | html2json 转换器的行为与静态素材约定 |
| [`kb-nocanvas-ui-animation.md`](kb-nocanvas-ui-animation.md) | 不用画布时怎么做 UI 动画 |
| [`kb-easyui-translucent-overlay-and-seekbar.md`](kb-easyui-translucent-overlay-and-seekbar.md) | 半透明浮层与 seekbar |
| [`kb-screen-flip.md`](kb-screen-flip.md) | 整屏翻转的两个接口与作用范围 |
| [`kb-online-audio-visualizer.md`](kb-online-audio-visualizer.md) | 在线音频频谱 / VU 表 |
| [`kb-v85x-h264-player.md`](kb-v85x-h264-player.md) | V85X H.264 播放器接入 |
| [`kb-v85x-online-streaming-limits.md`](kb-v85x-online-streaming-limits.md) | 在线播放的边界与限制 |
| [`kb-stream-switch-and-qa-traps.md`](kb-stream-switch-and-qa-traps.md) | 换流的两段式收尾与 QA 陷阱 |
| [`kb-wifi-monitor-sniff.md`](kb-wifi-monitor-sniff.md) | WiFi monitor 模式与空口嗅探 |
| [`kb-risk-presentation-observability.md`](kb-risk-presentation-observability.md) | 风险呈现与可观测性 |
| [`kb-device-preinstalled-libs.md`](kb-device-preinstalled-libs.md) | 设备预装库清单 |
| [`kb-device-ro-partition-red-line.md`](kb-device-ro-partition-red-line.md) | 只读分区红线 |
| [`kb-open-source-stack-integration.md`](kb-open-source-stack-integration.md) | 开源组件接入的经验 |
