# 构建、打包与烧写

本篇是把工程变成「设备上正在跑的东西」的完整流程。所有命令都在**工程根目录**执行。

> **先决条件**：`src/dependencies/` 已补齐 → 先跑 `python tools/check_deps.py` 确认。

---

## 1 三条命令的区别（先搞清楚这个，能省很多时间）

| 命令 | 干什么 | 落到哪里 | 掉电还在？ | 什么时候用 |
|---|---|---|---|---|
| `fun launch` | 把 `libzkgui.so` / `ui` / `font` 推到设备 **`/tmp`** 并重启应用 | `/tmp`（**tmpfs = 吃内存**） | ❌ | **改一处 → 验一处的迭代期** |
| `fun build` | 只编译，产出 `libzkgui.so` 等 | 本地 `.fun/v85x/` | — | 看编译过不过 |
| `fun pack` | 出固件包 `update.img` | `out/` | — | 要交付 / 要长期跑 |
| `tools/upgrade_device.sh` | 出包 + 通过 ADB **刷进 flash 的 res 分区**并验证 | 设备 `/res`（squashfs） | ✅ | **固化、交付、长期跑** |

⚠️ **迭代期请一律用 `fun launch`**。它推的几 MB 全部进 `/tmp`（tmpfs 占 RAM），
这块板子只有 ~56 MB 内存，多推几次就可能把应用 OOM 掉。
验完记得 `adb shell reboot` 让系统回落 `/res`（`/tmp` 会自动清空）。

---

## 2 编译与打包

```bash
# 拉取 Manifest.xml 里声明的官方包（需要网络 + fun 的包仓库可达）
fun install

# 编译
fun build

# 出固件包（带版本号，交付用）
fun pack -p V85X --release-version 1.0.0 -o out/update.img

# 等价的一键固化（出包 + 刷机 + 验证）
tools/upgrade_device.sh 1.0.0

# 快速迭代：只 launch，不刷机不重启
ITER=1 tools/upgrade_device.sh
```

### 关于 `fun build` 在终端里挂住

如果 `fun build` 从**终端**（而不是 IDE）跑，可能在 CMake 阶段卡住并报
`CMAKE_MAKE_PROGRAM-NOTFOUND` —— 原因是 `ninja` 不在 `PATH` 里。修法：

```bash
pip install --user ninja
export PATH="$PATH:$HOME/AppData/Roaming/Python/Python3xx/Scripts"   # ninja 所在目录
rm -f .fun/v85x/CMakeCache.txt        # 让 CMake 重新探测，必须删缓存
fun build                              # 如仍失败，手动补一次 configure
```

> ⚠️ **不要删 `.fun/v85x` 整个目录** —— 里面除了缓存还有工程的中间产物，
> 删了要全量重编。只删 `CMakeCache.txt` 即可。

---

## 3 烧写进设备（`tools/upgrade_device.sh`）

脚本做四件事：出包 → `adb push` → 写到 flash 的 res 分区 → 校验 `/res` 里的文件大小。

```bash
tools/upgrade_device.sh 1.0.0        # 固化
SKIP_BUILD=1 tools/upgrade_device.sh # 包已出好，跳过编译
ITER=1 tools/upgrade_device.sh       # 只 launch（快速迭代）
```

需要按你的机器改的环境变量（脚本里都有默认值，指向作者的机器，**请改成你自己的**）：

| 变量 | 含义 |
|---|---|
| `ADB` | `adb.exe` 的绝对路径 |
| `DEV` | 目标设备序列号（`adb devices` 里的那个）。**多台设备同时连着时必填** |
| `BUSYBOX` | 设备侧 busybox 的本地路径（脚本用它读 `/res` 里的文件大小做校验） |

### 三个真实的坑（踩过，写在这里省你时间）

1. **多设备时 `fun launch` 必失败** —— `fun` 自己挑设备，两台以上会报
   `more than one device/emulator`，但脚本里显式的 `adb -s` 命令照常能跑，
   于是表现为「编译都过了，只有推程序那步没成」，很容易被当成成功。
   脚本已统一用 `export ANDROID_SERIAL="$DEV"` 解决。
   ⚠️ **不要去 `adb disconnect` 别人的设备** —— 那可能是同事正在用的板子。
2. **Git Bash 的路径转换会毁掉 `adb push`** —— `/tmp/update.img` 被改写成 Windows 路径，
   push 报 `remote No such file or directory`，然后脚本在重试里静默卡住。
   脚本已 `export MSYS_NO_PATHCONV=1`。
3. **⚠️⚠️ 固化完成 ≠ 落地完成** —— 实测过两次：脚本报「完成、`/res` 正常」之后
   **约 3 分钟**设备会自己整机重启，起来后 **mtd3 的 squashfs superblock 读不到**，
   `/res` 没挂上（`cat /proc/mounts` 里没有 mtd 条目、屏幕停在白底启动画面、应用起不来）。
   - **判据**：`cat /proc/uptime` 只有 20 多秒 ⇒ 说明刚自己重启过。
   - **恢复**：原样再跑一次 `tools/upgrade_device.sh <版本>`（实测一次就恢复）。
   - **绝对禁止**手工 `mount` / `dd` 回写 flash 分区。
   - **纪律**：固化之后**再等 3~5 分钟**确认设备没有自己重启、`/res` 仍在，才算完成；
     别「改一点刷一点」，两次固化之间要留间隔。

> **红线**：设备的 `/res`、`/dev/block/mtdblock*`、`/dev/mtd*` 都是**只读系统分区**，
> 严禁 `dd` / `mount` / `flash_erase` / `mkfs`。**唯一的正路就是 `tools/upgrade_device.sh`**。

---

## 4 固件包里有什么

`out/update.img` 解包后就是设备上的 `/res`：

```
bin/firmware/rtlbt/{rtl8733bs_fw,rtl8733bs_config}   蓝牙固件
etc/EasyUI.cfg                                       分辨率 / 触摸设备 / 屏保超时 / 字库路径
font/pocketgame.ttf                                  项目字库子集
lib/libzkgui.so                                      主程序
lib/libawh264player.so                               H.264 播放器（来自 lib-no-link/）
lib/{libcrypto,libssl}.so.1.1                        https
ui/*.ftu                                             各页布局
ui/audio/*.wav                                       音效
ui/certs/cacert.pem                                  https 根证书
ui/images/*.png                                      图标 / 卡片图
ui/iptv/channels.m3u                                 已筛频道
```

**包体上限**：目标板的 `res` 分区（mtd3）只有 **7,995,392 字节**。
出包后请核对 `out/update.img` 不超过这个数，超了就得回头精简素材
（`tools/audit_resources.py` 会列出未被引用的素材）。
