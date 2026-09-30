# 依赖获取：把不能开源的二进制补回来

本仓库**只含本项目自己写的代码与素材**。要让工程真正编译出固件，需要再补两类东西：

- **A 类：FlyThings / EasyUI 工具链与包**（可用官方包管理器自动拉取）
- **B 类：全志 V85X SDK 头文件与库、第三方库**（需要手工放置到 `src/dependencies/`）

---

## A 类 · FlyThings 工具链与官方包

工程用 [`Manifest.xml`](../Manifest.xml) 声明依赖，用 `fun` 命令行驱动。
`fun` 来自 FlyThings IDE（EasyUI for V85X）。

| 需要的东西 | 说明 |
|---|---|
| `fun`（CLI，Windows 下为 `fun.exe`） | FlyThings IDE 自带；本仓库**不含**（约 33 MB，专有工具） |
| 官方包仓库 | `https://package.flythings.cn`（见 [`.deps.lock`](../.deps.lock)） |

`Manifest.xml` 里声明的包，由 `fun install` 自动下载到工程的包缓存里：

| 包 | 版本约束 | 实际锁定版本 | 用途 |
|---|---|---|---|
| `easyui` | `^2.3.0` | 2.9.0 | FlyThings UI 框架本体 |
| `base-utility` | `^10.12.0` | 10.12.4 | 基础工具 |
| `civetweb` | `^1.16.1` | 1.17.0 | 内置 HTTP 服务（DLNA / HA / 探测页用） |
| `btstack` | `1.7.2` | 1.7.2 | 蓝牙协议栈（HID / A2DP / SPP） |
| `ntp` | `^2.1.0` | 2.1.0 | 联网校时。**本板无 RTC**，不校时会让 https 证书判定「尚未生效」而握手失败 |
| `z` | `^1.2.11` | 1.2.11 | zlib（ffmpeg 的 http/gzip 与 mov 解封装依赖） |
| `log` / `zkhardware` / `zknet` | `0.0.0` | — | 平台日志 / 硬件抽象 / 网络 |

> 精确版本以 [`Manifest.xml`](../Manifest.xml) + [`​.deps.lock`](../.deps.lock) + [`​.fun-lock.json`](../.fun-lock.json) 为准。
> 这两个 lock 文件已随仓库提交，便于复现同一套包版本。

---

## B 类 · 手工放置到 `src/dependencies/`

目录结构与完整清单见 [`src/dependencies/README.md`](../src/dependencies/README.md)。
按来源分三块：

### B1 · 全志 V85X SDK 头文件与库（`include/` + `lib/libzkmedia.a`）

来源：全志官方 SDK（`eyesee-mpp` 一系的 `media/include`、`media/lib`）。
若你手上的板卡厂（如 Zkswe）提供了配套 SDK 包，优先用**板卡厂给的版本** —— 头文件与设备上的
库版本必须匹配，混用不同版本的 `mpi_*.h` 会出现「编得过、跑飞」。

放置：`include/` 下的 `.h`（含 `libav*/` 子目录若 SDK 自带）、`lib/libzkmedia.a`。

### B2 · ffmpeg 静态库（`lib/libav{codec,format,util}.a` + `libswresample.a`）

来源：自行交叉编译 ffmpeg，或从板卡厂 SDK 取其 arm 版。
本项目**只要解码 + 解封装 + 重采样**（播放 IPTV / 收音机 / 视频），编成静态库并
**`strip -g` 剥掉 DWARF 调试信息**（未剥时体积约 4.5 MB 的调试段）。

要求：
- 目标架构与工具链一致（本工程为全志 V85X 的 arm 工具链）
- 需要启用 `--enable-protocol=http,file,rtmp,hls`、`--enable-demuxer=mov,mpegts,hls`、
  `--enable-decoder=h264,aac,mp3`、`--enable-swresample`
- ffmpeg 为 LGPL/GPL，**再分发请自行遵守其许可证**（LGPL 静态链接时需要提供可重链接的目标文件）

### B3 · OpenSSL 1.1 与 H.264 播放器库

| 文件 | 来源 | 说明 |
|---|---|---|
| `lib/libcrypto.so.1.1`、`lib/libssl.so.1.1` | 全志 SDK / 板卡厂 | https（DLNA 抓取、HA 对接、IPTV 拉流） |
| `lib-no-link/libawh264player.so` | 全志 SDK | `zk_h264_player` 的实现；**不进链接，由 `fun pack` 打进 `/res/lib/`** |
| `bin/firmware/rtlbt/{rtl8733bs_fw,rtl8733bs_config}` | 板卡厂 / Realtek | 蓝牙固件（RTL8733BS），会被打进 `/res/bin/firmware/` |

---

## 校验依赖是否齐了

```bash
python tools/check_deps.py
```

脚本会按上面的清单逐项检查（存在性 + 大小量级），缺什么直接列出来，不会静默放过。
