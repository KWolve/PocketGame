---
id: devflow-device-preinstalled-libs
title: 设备端已自带的可借用库清单（dlopen 即用 / 免编译）—— 先查这里再决定自己编
category: devflow
status: review
confidence: real-device
verified_at: 2026-09-30
stale_days: 180
origin: partial
source: 2026-09-30 Z21（Zkswe_SSD21X_SPINOR / 192.168.1.217）真机盘点 + readelf 验符号；F133 侧引自 components/vinyl/platforms.md
needs_evidence: false
platforms: [Z21, F133]
tags: [设备自带库, 已装库, dlopen, nanovg, libpng, freetype, libjpeg, libmad, zlib, 第三方库, 免编译, mi_gfx, 2D 加速, 符号校验]
evidence:
  - {cmd: "adb -s <设备>:5555 shell '/tmp/busybox ls -l /lib /res/lib'", kind: real-device}
  - {cmd: "arm-pc-linux-gnueabihf-readelf.exe -d libnanovg.so", kind: offline}
  - {cmd: "arm-pc-linux-gnueabihf-readelf.exe --dyn-syms libnanovg.so", kind: offline}
---
# 设备端已自带的可借用库清单（dlopen 即用 / 免编译）

> **检索导引**：想用的库设备上有没有 / 能不能直接用 / 需不需要自己编译 / nanovg 能用吗 /
> libpng 在哪 / freetype 有没有 / 图片解码用什么 / MP3 解码 / zlib / 有没有 2D 加速库 /
> dlopen 找不到库 / 注册表里没有这个包是不是就没有 / 设备自带 .so 清单 / 免编译借用。
>
> **一句话**：**"注册表里没有"≠"平台没有"** —— 设备 `/lib` 里躺着 80 个库，其中一批
> 图形/解码/加速库**可直接 dlopen 或链接**。**先查本表，再决定是否自己交叉编译。**

## 1. 判据（先跑这两条命令，别猜）

```bash
# ① 设备上有哪些库（busybox 会由 MCP 工具自动推到 /tmp；Git Bash 下 adb 必须 MSYS_NO_PATHCONV=1）
adb -s <IP>:5555 shell "/tmp/busybox ls -l /lib /res/lib"
# ② 某个 .so 的 ABI / 依赖 / 导出符号（用对应平台工具链的 readelf）
arm-pc-linux-gnueabihf-readelf.exe -d <lib>.so            # Class/Machine/NEEDED/SONAME
arm-pc-linux-gnueabihf-readelf.exe --dyn-syms <lib>.so    # 导出符号（对照头文件核 API）
```
工具链选择按 libc：**V85X/T113 = musl（`arm-unknown-linux-musleabihf-*`）**、
**Z20/Z21 = glibc（`arm-pc-linux-gnueabihf-*`）**、**F133/F135 = RISC-V64 musl**。

## 2. Z21（Zkswe_SSD21X_SPINOR）实测清单 —— 2026-09-30

> 环境：easyui **2.2.0** / kernel 4.9.84 / **glibc 2.30**（`ld-2.30.so`）；
> 通用库在 `/lib`，应用库在 `/res/lib`（本例只有 `libzkgui.so`）。

### 2.1 图形 / 图像 / 音频（**最值得借的一批**）

| 库 | 体积 | 能干什么 | 链接属性（readelf 实测） |
|---|---|---|---|
| **`libnanovg.so`** | 51 KB | **矢量绘制（AGG 后端）**：`nvgCreateAGG`/`nvgDeleteAGG`、`nvgBeginFrame`/`nvgEndFrame`、`nvgScale`、`nvgCreateImageRGBA`、`nvgImagePattern` | ELF32 ARM DYN；NEEDED=`libgcc_s.so.1,libc.so.6`；**无 SONAME**；**100 个 GLOBAL FUNC** |
| **`libpng12.so.0.56.0`** | 112 KB | PNG 解码/编码 | SONAME=`libpng12.so.0`；NEEDED=`libz.so.1,libm.so.6,libc.so.6`；**228 个 GLOBAL FUNC**，符号带版本 `@@PNG12_0` |
| `libfreetype.so.6.11.4` | 137 KB | 字体光栅化（TrueType） | 标准 SONAME 链 |
| `libjpeg.so.9.1.0` | 177 KB | JPEG 解码 | — |
| `libz.so.1.2.8` | — | zlib（gzip/inflate） | — |
| `libmad.so.0.2.1` | 83 KB | **MP3 解码** | — |

**已验通道（F133 侧，`components/vinyl/platforms.md`）**：`/lib/libnanovg.so` 与注册表包
**同为一份构建（符号逐条相同）**；nanovg 后端 320×320 **23~44ms/帧**（定点后端 6~12ms），
**只支持 `NVG_TEXTURE_BGRA` 目标**。⚠️ **Z21 本轮未跑运行时验证**（只验了库存在与符号），
故 `platforms: [Z21, F133]` 中标 Z21 为**仅静态验证**。

### 2.2 全志 MI 系列（芯片侧能力入口）

| 库 | 用途 |
|---|---|
| **`libmi_gfx.so`** | **MI 图形加速模块**（2D 相关能力入口，**待逐符号核 API**） |
| `libmi_disp.so` | 显示/图层（disp） |
| `libmi_rgn.so` | 区域/OSD 叠加（Region） |
| `libmi_divp.so` | 视频处理（缩放/去隔行一类） |
| `libmi_ao.so` / `libmi_sys.so` / `libmi_common.so` / `libmi_panel.so` | 音频输出 / MI 基础 / 面板 |

⇒ **这组是"芯片侧能力"的正式入口**，与框架层能力互补；具体 API 需按头文件核对后使用。

### 2.3 框架与平台服务

`libeasyui.so`（GUI 引擎）、`libinternalapp.so`、`libzkhardware.so` / `libzkhw.so`（硬件门面）、
`libzkmedia.so`、`libzknet.so`、`libzkupgrade.so`（升级）、`libts.so` / `libtsupdate.so`（触摸校准）、
`libnl-tiny.so`（netlink）、`libiniparser.so`、`libcam_os_wrapper.so`、`libcutils.so`、`liblog.so`。

### 2.4 运行时基础（**决定"能不能借"的那一层**）

`ld-2.30.so` / `libc-2.30.so` / `libstdc++.so.6.0.26` / `libgcc_s.so.1` /
**`libgomp.so.1.0.0`（OpenMP，可做多线程加速）** / `libssp` / `libpthread` / `libdl` / `librt`。

## 3. 三条使用纪律

1. **注册表没有 ≠ 不能借**：nanovg 在**四个平台的注册表里都没有包**，但**设备 `/lib` 里有**
   ⇒ 走 dlopen（或自带头文件 + 链接设备上的 .so）即可，**不必自己交叉编译**。
2. **头文件不在设备上**：设备只装 `.so`，**头文件要从 SDK/参考工程/组件里取**；
   用 `--dyn-syms` 的符号表**逐条对照头文件**确认签名（本仓纪律：不许凭记忆写 API）。
3. **libc 必须匹配**：Z20/Z21=glibc、V85X/T113=musl、F133=RISC-V64 musl；
   跨平台复用同一份 `.so` **一定失败**。

## 4. 本文明说的边界（待补）

- Z21 的 nanovg **只有静态验证（库存在 + 符号 + ABI）**，**运行时未验**；
  要升 `verified` 需在 Z21 上跑一次真实绘制（建议直接复用 `components/vinyl` 的 A/B 口径）；
- `libmi_gfx.so` 的 API **未核**（只有库名与体积），要写进材料前必须 `--dyn-syms` + 头文件对照；
- 本清单是**Z21 单点**盘点：**换板必须重跑 §1 的两条命令**（不同平台库集合不同）。
