# 固化升级（出 update.img 并刷进设备）—— 为什么、怎么做、坑在哪

> 本工程实测记录（2026-09-12）。**调试推送（`fun launch`）≠ 固化（`fun pack` + 升级）**。

## 一、为什么必须固化：本板 /tmp 是 tmpfs，调试推送直接吃内存

本板（V85X / V851s，480x800）内存只有 **56 MB**（`MemTotal: 56632 kB`），而：

| 挂载点 | 类型 | 说明 |
|---|---|---|
| `/` | squashfs | 只读 |
| `/tmp` | **tmpfs (27 MB)** | **写进去就是占 RAM**（`Shmem` 直接涨） |
| `/data` | jffs2 (832 KB) | 可写但极小（装 745 KB 的 so 都费劲） |
| `/res` | squashfs (7.6 MB 分区) | **固化升级的目标分区** |

`fun launch` 会把 `libzkgui.so`（745 KB）+ `ui/` + `font/` 推到 **`/tmp/lib`、`/tmp/ui`**，
再写 `/tmp/EasyUI.cfg` 把 `startupLibPath` 指过去 —— 也就是**调试版本住在内存里**。

实测代价（本板）：

- 固化前：`MemFree 1.4 MB / Shmem 20 MB / Committed_AS 27972k（CommitLimit 28316k，98.8%）`
  → 再要一块视频解码缓冲就分配失败，**应用被拖崩/被 OOM 杀**（DLNA 投屏播放中进程重启，查了一轮才发现是内存）。
- 固化后：`MemFree 22.5 MB / Shmem 1.2 MB / Committed_AS 7848k`，`/tmp/lib` 不再存在。

**结论：功能稳住的版本就固化进 flash，之后调试/验收都不需要再 launch。**

## 二、出包（PC 侧）

```bash
cd <工程根>
fun install                     # 依赖
fun build                       # 编译（产物 .fun/v85x/libzkgui.so）
fun pack --release-version 1.0.3 -o out/update.img
# → .fun/v85x/update.img（本例 420.6 KB），同时拷到 out/update.img
```

包体结构（前 16 字节是私有头，随后是 **squashfs**）：

```
"ZKSWEV1.0-1801270" + 头 + "hsqs"(squashfs magic) ...
```

**打包前务必确认 release 配置指向 /res**（`fun pack` 会按 release 路径生成，
`.fun/v85x/imgout/etc/EasyUI.cfg` 就是会被写进镜像的启动配置，先看一眼最保险）：

```json
{
  "startupLibPath": "/res/lib/libzkgui.so",
  "resPath": "/res/ui/",
  "font": "/res/font/pocketgame.ttf",
  "touchDev": "/dev/input/event4",      // ← 本板必须是 event4（出厂 /res 里写的是 event1，不存在）
  "rotateScreen": 0
}
```

> `package.properties` 里的 `EasyUI.cfg={...}` 是**覆盖层**，只写要覆盖的字段；
> 其余路径（/res/lib、/res/ui、/res/tr）由打包器按 release 自动生成。

## 三、刷进设备（ADB 固化，实测可用）

框架启动时会加载 `libzkupgrade.so`，读 `sys.zkupgrade.*` 属性 → 找 update.img → 刷 res 分区 → 自动重启：

```bash
adb push out/update.img /tmp/update.img
adb shell setprop sys.zkupgrade.dir /tmp
adb shell setprop sys.zkupgrade.flag 255
adb shell setprop ctl.restart zkswe        # 服务名来自 /etc/init.rc: service zkswe /bin/zkgui
```

> 升级库字符串里可见的属性：`sys.zkupgrade.flag/dir/force/state/umount`、
> 记录文件 `/data/.zkupgraderec`、TF 卡路径 `/mnt/extsd /mnt/mmc /mnt/usb`。
> 升级完成打印 `upgrade success, will reboot system!`。

**一键脚本**：`tools/upgrade_device.sh [版本号]`（出包 → 推送 → 触发 → 等重启 → 自动验证）。
其它刷法（TF 卡根目录放 update.img、插卡自动升级 `zkautoupgrade`）见知识库
`knowledge/devflow/upgrade-pack-image.md`。

## 四、坑（都踩过）

| 现象 | 根因 | 处理 |
|---|---|---|
| 脚本里 `adb push` 一直失败（手敲却成功） | 脚本里传的是 **MSYS 风格路径** `/e/AICODE/...`，adb.exe 是 Windows 程序不认 | 传参前 `cygpath -w` 转换（脚本已内置） |
| 设备刚重启完 push 报 device offline / 失败 | adb 短暂抽风 | 脚本内置 5 次重试 |
| 升级后屏幕空白 | 镜像里 `startupLibPath`/`resPath` 指向不对 | 刷前看 `.fun/v85x/imgout/etc/EasyUI.cfg` |
| 升级后触摸失效 | `touchDev` 被覆盖回出厂值（event1，本板不存在） | `package.properties` 里显式写 `touchDev: "/dev/input/event4"` |

## 五、验证（脚本第 5 步自动做）

```bash
adb shell "ls -l /res/lib /res/ui"        # 应看到我们的 libzkgui.so + main.ftu/wifi.ftu
adb shell "cat /res/etc/EasyUI.cfg"       # startupLibPath=/res/lib/libzkgui.so
adb shell "ps | grep zkgui"               # 进程在
adb shell "ls /tmp/lib"                   # 应为 No such file（跑的是 flash 里的）
adb shell "cat /proc/meminfo | grep -E 'MemFree|Shmem'"
```

实测（v1.0.3）：`/res/lib/libzkgui.so = 745544`、
`MemFree 22520 kB / Shmem 1200 kB`、`/tmp/lib: No such file or directory`，
启动日志 `load /res/etc/EasyUI.cfg ok!` + `PocketGame: init ok, 13 apps(...)`。

### 2026-09-15 追加实测（v1.22.0 消消乐首版 → v1.23.0 特殊块）

```bash
ADB=adb tools/upgrade_device.sh 1.22.0    # 第 1 次：消消乐首版
ADB=adb tools/upgrade_device.sh 1.23.0    # 第 2 次：加特殊块（炸弹/彩虹球）
```

| 判据 | 实测 |
|---|---|
| 包大小 | v1.22.0 = 3252796 B（3.1 MB）；v1.23.0 也 3.1 MB 级（`imgout/` 未压缩 8.8 MB） |
| **分区容量够不够** | `cat /proc/mtd` → `mtd3: 007a0000` = **7.6 MB** ⇒ 3.1MB 安全。
⚠️ `df /res` 显示的是 **squashfs 压缩后内容大小**（`Free 0K` 是正常的，只读文件系统），**别拿它当分区容量** |
| 刷写 | 两次都 `已重启（uptime 6.x）`；`/res/ui/*.ftu` 全部更新到刷写时刻 |
| 新 so | v1.22.0 = 4183864 → v1.23.0 = **4204808**（+20.9KB = 特殊块代码；**so 大小本身就是"刷进去了没有"的判据**） |
| 启动配置 | `startupLibPath=/res/lib/libzkgui.so`、`resPath=/res/ui/`、`touchDev=/dev/input/event4` ✅ |
| 跑的是 flash 版 | `/tmp/lib: No such file or directory`；`/tmp` 只占 **1 MB**（不再是 11~12 MB） |
| **★ 内存（最有说服力的数字）** | 刷前（调试版）：`MemFree 5520 kB / Shmem 12168 kB`；刷后：`MemFree 23672 kB / Shmem 1200 kB` ⇒ **还给系统 ~18MB** |
| **★ 帧率也跟着好** | 同一台设备同一场景：调试版 **51~58fps**（render 4.8~5.1ms）；**固化版 60fps** —— 内存压力小了，主循环间隔从 ~18.8ms 回到 ~16.7ms |
| **新功能在不在**（关键） | `ls /res/ui/images/game/match3/` → **13 张**（v1.23.0 独有的 `mark_h/mark_v/rainbow` 都在）；QA `22 1` → `enter game[22]`；固化版实测 `两个炸弹互换 → 引爆 → 第 3 行 +7 格`、`彩虹球交换 → 清掉颜色 1 的 10 颗` ✅ |

> ⚠️ **固化后必须验一次"新功能在不在"**：`fun launch` 只是把新库推到 `/tmp`，
> **设备一重启就回落 `/res` 旧库** —— 不固化的话，用户重启后会发现"游戏/新玩法不见了"。
> 判据要选"**只有新版本才有的东西**"（这里 = `/res/ui/images/game/match3/` 的三张新素材、
> `libzkgui.so` 的**字节数**、`enter game[22]`），而不是"进程在跑"这种两版都成立的现象。
> **顺带**：`Shmem` 变高（>10MB）本身就是"跑的是调试版"的旁证，也是帧率变差的常见原因。

### ★ 换了一台机器（2026-09-15 实测）：**序列号不能用来区分板子**

同一批板子 adb 报的序列号**都是 `20080411`** ⇒ `adb devices` 看不出换没换机，
也不能靠它判断"这台刷过没有"。**判断当前设备里是哪个版本，只能看 `/res` 的内容**：

```bash
adb -s 20080411 shell "ls -l /res/lib/libzkgui.so"          # 字节数 = 版本指纹
adb -s 20080411 shell "ls /res/ui/images/game/match3/"      # 素材张数 = 功能指纹
```

| `libzkgui.so` | match3 素材 | 版本 |
|---|---|---|
| 4183864 | 10 张（无 `rainbow/mark_h/mark_v`） | v1.22.0（无特殊块） |
| **4204808** | **13 张** | **v1.23.0** |

出包/刷写流程与单机完全一样（`ADB=adb tools/upgrade_device.sh 1.23.0`），
刷完的验收也照 §五。换机实测结果：出包 3269180 B → 推送 → `uptime` 归零重启
→ `so = 4204808`、13 张素材、`/tmp/lib` 不存在、`MemFree 15092kB / Shmem 1200kB`
→ QA `22 1` → `enter game[22] 消消乐`、`fps = 58`、特殊块三分支 + 差分渲染判据全过
→ 主界面图标 idx13 均色 `RGB(237,123,76)` 精确命中 `#FF7A45`。

**给别的机器刷同一个版本**：出好的包已按版本归档（沿用 `out/update_v*.img` 老约定），
不想重新编译时直接推归档包 + 下属性即可（`sys.zkupgrade.*` 就是脚本第 3 步做的事）：

```bash
adb push out/update_v1.23.0.img /tmp/update.img        # 相对路径（MSYS 绝对路径会 push 失败）
adb shell "setprop sys.zkupgrade.dir /tmp; setprop sys.zkupgrade.flag 255; setprop ctl.restart zkswe"
# 等 1~2 分钟自动重启，再按上面的指纹表确认版本
```

## 六、回滚

- 升级前的 `/res` 已备份到 `out/res_backup/`（6.6 MB，含出厂 `libzkgui.so`/nginx/loading 图）。
- 要回滚：把备份整理成 imgout 结构重新 pack，或直接 `fun launch` 用 /tmp 调试版顶一段时间。
- **不建议**把出厂包刷回去当"回滚"——那是整个 res 分区，风险更大。
