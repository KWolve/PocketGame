#!/usr/bin/env bash
# upgrade_device.sh —— 一键「固化升级」：出包 + 通过 ADB 刷进设备并验证
#
# 为什么需要它：
#   `fun launch` 只是**临时调试**——把 libzkgui.so / ui / font 推到 /tmp（本板 /tmp 是
#   **tmpfs，写进去就是吃 RAM**；板子只有 56MB 内存，调试版多占几百 KB~几 MB 会直接
#   把整机推到 OOM）。固化升级把 app 写进 flash 的 res 分区（squashfs），**掉电保留、
#   不占内存**，之后调试/验收都不需要再 launch。
#
# 用法（PC 上，Git Bash）：
#   tools/upgrade_device.sh            # 出包 + 刷机 + 验证（固化：交付 / 长期跑用）
#   tools/upgrade_device.sh 1.0.2      # 指定版本号
#   SKIP_BUILD=1 tools/upgrade_device.sh   # 跳过编译（包已出好）
#   ITER=1 tools/upgrade_device.sh     # ★ 快速迭代：只 `fun launch`（推 /tmp，不刷机不重启）
#
# ★★ 该用哪个（2026-09-16 实测基准，PC 侧 date 计时）：
#   · **迭代期（改一处→验一处）一律 `ITER=1`**：`fun launch` 推 7.6MB 约 **11s**、不重启；
#   · **要交付 / 要长期跑才固化**：全流程约 **40~120s**（编译 4~90s + 出包 + push 4.2MB +
#     刷写 + 应用重启 10~20s），比旧版快 2.5 分钟（旧版在这里白等 —— 见下面 4/5 的说明）；
#   · ⚠️ launch 推的 7.6MB 全进 `/tmp`（tmpfs = 占 RAM，板子 55MB）⇒ **验完 `adb shell reboot`**
#     回落 `/res`（实测 15s，`/tmp` 自动清空）；
#   · ⚠️ **固化不会重启整机**（`setprop ctl.restart zkswe` 只重启 zkgui 应用）⇒ 判据必须是
#     **pid 变化**，不是 `/proc/uptime`（旧判据写错，每轮固定空等 150s）。
set -u

ADB="${ADB:-D:/zkswe/FlyThingsPreview/sdk/platform-tools/adb/adb.exe}"
DEV="${DEV:-20080411}"
# ★★ 2026-09-21 加：**把目标设备告诉子进程**。
#   多台设备同时连着 adb 时（本机实测：目标板 `20080411` + 另一台 `192.168.1.177:5555`
#   的 Zkswe_SSD20X），`fun launch` 这类**自己挑设备**的子命令会直接失败：
#     `FATAL "host:transport 20080411" FAIL: more than one device/emulator`
#   —— 而脚本里我们自己那几条 `"$ADB" -s "$DEV"` 都是显式指定、照常能跑，于是表现为
#   "编译/校验都过了，只有推程序那步没成"（**很容易当成推成功了**，因为脚本只把它的输出
#   打了一行 `fun launch 完成`）。
#   ⇒ 统一用 adb 官方的 `ANDROID_SERIAL` 环境变量指定（子进程会继承）。**不要去 disconnect
#     别人的设备** —— 那可能是用户正在用的另一块板。
export ANDROID_SERIAL="$DEV"
# ★★ 2026-09-23 实测补：**必须关掉 MSYS/Git Bash 的路径转换**。
#   否则 `adb push "$WIN_IMG" /tmp/update.img` 里的 `/tmp/update.img` 会被 Git Bash
#   改写成 Windows 路径（实测变成了 `D:/Temp/update.img`）⇒ push 报
#     `failed to copy 'out\update.img' to 'D:/Temp/update.img': remote No such file or directory`
#   而脚本的重试/验证段落于是**卡住不动**（我实测白等 12 分钟才手工发现）。
#   MSYS_NO_PATHCONV=1 让参数原样传给 adb.exe；对 `adb shell "..."`（整串参数不以 / 开头）
#   本来就无影响，所以全局开着是安全的。
export MSYS_NO_PATHCONV=1
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-1.0.0}"
IMG="$ROOT/out/update.img"
# 验证段要读 /res 里的文件大小，本板**没有 stat/awk**（busybox 提供）。原先是写死的绝对路径 +
# `>/dev/null 2>&1` ⇒ 路径一旦不在，push 静默失败、判据读到空值、脚本会误报"固化失败"
# （2026-09-19：先把这条**静默依赖**变成开头的显式检查，缺了就在动手刷机之前就停）。
BUSYBOX="${BUSYBOX:-D:/zkswe/flythings-mcp-open/bin_tools/v85x/busybox}"

cd "$ROOT" || exit 1

# ---- 计时：每步打印上一步耗时 + 全程总耗时（改动前先看数字，别猜）----
T_ALL0=$(date +%s)
T_STEP=$(date +%s)
step() {
  printf "   [本步 %ss / 全程 %ss] %s\n" \
    "$(( $(date +%s) - T_STEP ))" "$(( $(date +%s) - T_ALL0 ))" "$1"
  T_STEP=$(date +%s)
}

# 取 zkgui 的 pid（**PC 侧**过滤：设备 shell 没有 grep/pidof）。
# ★ 固化后判「应用重启过」只能靠它 —— `ctl.restart zkswe` **不重启整机**，uptime 不会归零。
get_pid() {
  "$ADB" -s "$DEV" shell "ps" 2>/dev/null | grep "[z]kgui" | awk '{print $2}' | tr -d '\r' | head -1
}

echo "== 1/5 编译 =="
if [ "${SKIP_BUILD:-0}" != "1" ]; then
  ./fun.exe install >/dev/null 2>&1
  BUILD_LOG="$(./fun.exe build 2>&1)"
  echo "$BUILD_LOG" | grep -E "error|FAILED|Linking" | tail -3
  # ★★ 编译失败必须**当场停**（2026-09-16 血案：链接失败但脚本继续，`fun pack` 打不出新包，
  #    于是**静默复用上一次的 out/update.img** ⇒ 刷进去的还是旧版本，而脚本的"验证"只看
  #    "/tmp/lib 不存在"，照样报成功 ⇒ 我一度以为 v1.30.8 已生效，实际设备还是 v1.30.7）。
  if echo "$BUILD_LOG" | grep -qE "FAILED:|ninja: build stopped|undefined reference|error:"; then
    echo "!! 编译失败，已中止（别拿旧包去刷）："
    echo "$BUILD_LOG" | grep -E "FAILED:|undefined reference|error:" | head -5
    exit 1
  fi
  [ -f .fun/v85x/libzkgui.so ] || { echo "!! 没生成 .fun/v85x/libzkgui.so"; exit 1; }
fi
step "编译完成"

# ★★ 快速迭代模式（ITER=1）：只推 /tmp，不出包、不刷机、不重启 —— 实测约 11s。
#   迭代期用它把「改一处 → 验一处」压到 ~30s（含 QA 下发）；要交付/长期跑再用默认固化流程。
if [ "${ITER:-0}" = "1" ]; then
  echo "== ITER=1 快速迭代：fun launch（推 /tmp，不重启）=="
  # ★★ 2026-09-21 加：**这一步会失败，而旧脚本照样打"完成"**（静默失败）。
  #   实测：adb 同时连着两台设备时（目标板 + 另一台 Zkswe_SSD20X 的 `192.168.1.177:5555`），
  #   `fun launch -s <dev>` 仍报
  #     `FATAL "host:transport 20080411" FAIL: more than one device/emulator`
  #   —— 它自己要先枚举设备、`-s` 救不了。而 `ANDROID_SERIAL` 环境变量对它也无效（实测）。
  #   ⇒ 这里显式检查它的输出；失败就**明确报错并给出出路**（改用固化流程 / 临时断开其它设备），
  #     绝不让人以为"推上去了"。
  LAUNCH_LOG="$(./fun.exe launch -s "$DEV" 2>&1)"
  echo "$LAUNCH_LOG" | grep -viE "[⢿⣽⣻⣾⣷⣯⣟⡿]" | tail -2
  if echo "$LAUNCH_LOG" | grep -qE "FATAL|more than one device/emulator|error:"; then
    echo "!! **fun launch 失败**（上面那条 FATAL）—— 别以为推上去了。"
    echo "   常见原因：adb 连着多台设备，而 fun 自己挑设备（`-s` 与 ANDROID_SERIAL 都不管用）。"
    echo "   出路：① 直接跑固化流程（它全程用 \`adb -s\`，不受影响）："
    echo "            tools/upgrade_device.sh <版本号>"
    echo "         ② 或临时断开其它设备（\`adb disconnect <host:port>\`，用完记得连回来）。"
    exit 1
  fi
  step "fun launch 完成"
  echo "   ⚠️ 现在跑的是 /tmp 的调试版（占 RAM）⇒ **验完执行： adb shell reboot** 回落 /res（约 15s）"
  exit 0
fi

[ -f "$BUSYBOX" ] || {
  echo "!! 找不到 busybox：$BUSYBOX"
  echo "   固化判据要读 /res 里的文件大小，而本板没有 stat/awk ⇒ 必须有它。"
  echo "   ⇒ 用 BUSYBOX=/path/to/busybox tools/upgrade_device.sh <版本> 指定（别让它静默失败）。"
  exit 1
}

echo "== 2/5 同步素材 + 出升级包（release 版，路径指向 /res） =="
# ★★ 同步素材（2026-09-19 血案）：**改了 resources/images 下的 PNG，`fun build` 不会刷新
#    `.fun/v85x/imgout/ui/images/`**（ninja 认为没有输入变化，直接 "no work to do"），
#    而 `fun pack` 打的就是 imgout ⇒ 会**静默打出一个带旧图的包**。
#    实测：源脸图 7873B（圆角版），imgout 与 /res 里都还是 7114B（旧方角版），
#    而脚本一路打印"固化完成"、判据（比对 .so）也全过 —— 因为 .so 确实换新了。
#    ⇒ 出包前一律同步，并把同步了哪几个文件**打出来**（不静默）。
IMG_SRC="$ROOT/resources/images"
IMG_DST="$ROOT/.fun/v85x/imgout/ui/images"
#    ⚠️ 实测补正（同日稍后）：`fun pack` **并不读 imgout** —— `fun clean` 之后
#      `.fun/v85x/imgout/` 整个不存在，`fun pack` 照样重出一个 6.6MB 的 update.img
#      （它自己按资源目录现生成）。所以这一步只是"万一将来 fun 改回用 staging"的保险，
#      **不是**素材进包的关键；关键是下面第 5 步那条**哨兵校验**。
if [ -d "$IMG_DST" ]; then
  IMG_SYNCED="$(cp -ruv "$IMG_SRC/." "$IMG_DST/" 2>/dev/null | grep -c '' || true)"
  if [ "${IMG_SYNCED:-0}" = "0" ]; then
    echo "   素材：staging 无变化（源 $(ls "$IMG_SRC" | grep -c '') 项）"
  else
    echo "   素材：同步了 $IMG_SYNCED 个文件到 staging"
  fi
else
  echo "   素材：无 staging 目录（fun pack 现生成镜像，不需要它）"
fi

echo "== 2/5 出升级包（release 版，路径指向 /res） =="
# ★★ 2026-09-20 新增：**打包前把 libzkgui.so 剥掉符号表**（`strip`，保留 .dynsym）。
#
# 为什么必须（实测数字，2026-09-20）：
#   · res 分区（mtd3）只有 **7,995,392 字节**；
#   · 未剥符号的 libzkgui.so = 4,807,740，剥完 = 4,082,392（**省 725KB**）；
#   · 落到 squashfs 镜像是 **-163,840 字节**（符号表高度可压缩，别按 1:1 估）；
#   · 本次新增的两段短片（resources/media，H.264 已压缩、进包基本 1:1）要吃掉 **851,968**，
#     ⇒ 不剥就只剩 ~184KB 余量，装上两页 ftu/图标/字库就可能**超分区**。
# 代价：崩溃回溯（`!!FATAL!!` 的地址）不再能反解函数名。日志里的
#   `__FILE__:__LINE__`（LOGD）不受影响 —— 路径/行号是字符串，不看符号表。
# ⚠️ 这一步**不静默**：剥了就打一行，`fun build` 之后重新生成所以每轮都会剥。
STRIP_BIN="${STRIP_BIN:-$FUN_HOME_PATH/toolchains/v85x/bin/arm-unknown-linux-musleabihf-strip.exe}"
SO="$ROOT/.fun/v85x/libzkgui.so"
if [ -x "$STRIP_BIN" ] || [ -f "$STRIP_BIN" ]; then
  SO_BEFORE="$(stat -c %s "$SO" 2>/dev/null || echo 0)"
  # ⚠️ 工具链是 Windows 程序：喂 MSYS 风格路径（/e/...）会报
  #    "'/e/...': No such file"（实测），必须转成盘符路径。
  "$STRIP_BIN" "$(cygpath -w "$SO" 2>/dev/null || echo "$SO")"
  SO_AFTER="$(stat -c %s "$SO" 2>/dev/null || echo 0)"
  echo "   strip: libzkgui.so $SO_BEFORE -> $SO_AFTER 字节（省 $((SO_BEFORE - SO_AFTER))）"
else
  echo "   ⚠️ 找不到 strip（$STRIP_BIN）—— 跳过；若接着报『超出 res 分区』就装上它再跑"
fi

# ★ 防"旧包冒充新包"：`fun pack` 失败时旧文件还在，`[ -f "$IMG" ]` 检查不出来
#   ⇒ 会把上一次的包推上设备（2026-09-16 血案：一度以为 v1.30.8 上机了，实际还是 v1.30.7）。
#   两道保险：① 先删；② **比 mtime**（删可能被宿主环境的"批量删除保护"拦掉，所以不能只靠删）。
IMG_OLD_MTIME="$(stat -c %Y "$IMG" 2>/dev/null || echo 0)"
rm -f "$IMG" 2>/dev/null || true
./fun.exe pack --release-version "$VER" -o out/update.img 2>&1 \
  | grep -viE "[⢿⣽⣻⣾⣷⣯⣟⡿]" | tail -3
[ -f "$IMG" ] || { echo "!! 没生成 $IMG"; exit 1; }
IMG_NEW_MTIME="$(stat -c %Y "$IMG" 2>/dev/null || echo 0)"
if [ "$IMG_NEW_MTIME" = "$IMG_OLD_MTIME" ] && [ "$IMG_NEW_MTIME" != "0" ]; then
  echo "!! 包没更新（mtime 没变 = fun pack 没真正出包），已中止"
  exit 1
fi
ls -l "$IMG"
# ★★ 2026-09-20 新增：**出包后立刻校验"装不装得下 res 分区"**，超了当场停。
# 为什么必须有：分区写不下时，刷写会失败在**擦/写中途**，而升级处理本身跑在应用里
#   ⇒ 表现为"卡 logo / 应用起不来"，现场只能重刷（见 §0 红线）。把判据提前到 PC 侧，
#   失败时连设备都还没碰。
# 判据来源：设备 `/proc/mtd` 的 `mtd3: 007a0000 "res"` = **7,995,392 字节**（硬事实，
#   不是估计）；镜像带上 ZKSWE 头与 squashfs，必须 ≤ 它。
RES_PART_BYTES="${RES_PART_BYTES:-7995392}"
IMG_BYTES="$(stat -c %s "$IMG" 2>/dev/null || echo 0)"
if [ "$IMG_BYTES" -gt "$RES_PART_BYTES" ]; then
  echo "!! 升级包 $IMG_BYTES 字节 > res 分区 $RES_PART_BYTES 字节（超 $((IMG_BYTES - RES_PART_BYTES))）"
  echo "   ⇒ 拒绝刷写（刷到一半会卡 logo）。可用手段："
  echo "      · 调低 resources/media 的体积（tools/gen_movie_assets.py 的 VIDEO_KBPS 有上限检查）"
  echo "      · 确认上面的 strip 那步没被跳过"
  echo "      · 或删掉不用的素材后再出包"
  echo "   （确实要覆盖判定时：RES_PART_BYTES=<字节数> $0 $VER）"
  exit 1
fi
echo "   分区余量：$((RES_PART_BYTES - IMG_BYTES)) 字节（包 $IMG_BYTES / 分区 $RES_PART_BYTES）"
step "出包完成"

echo "== 3/5 推送并触发固化（读 sys.zkupgrade.* 刷写 res 分区 + 重启 zkgui 应用） =="
# adb 在设备刚重启完会短暂抽风（"device offline"/push 失败），重试几次
for i in 1 2 3 4 5; do
  # ⚠️ adb.exe 是 Windows 程序，喂 MSYS 风格路径（/e/...）会 push 失败且报错很含糊
  #    （实测：脚本里 push 一直失败，手敲相对路径就好 —— 就是路径格式问题）。
  WIN_IMG="$(cygpath -w "$IMG" 2>/dev/null || echo "$IMG")"
  if "$ADB" -s "$DEV" push "$WIN_IMG" /tmp/update.img 2>&1 | grep -q "1 file pushed"; then
    echo "   update.img 已推到 /tmp（第 $i 次尝试）"
    break
  fi
  echo "   第 $i 次 push 失败，等 3s 重试"
  sleep 3
  [ "$i" = "5" ] && { echo "!! adb push 始终失败：设备没连上？"; exit 1; }
done
step "update.img 已推到 /tmp"
PID0="$(get_pid)"
# ★ 先 `flag 0` 再 `flag 255`：**属性里放的是同一个值就不算"变化"**，服务不会重新触发
#   （2026-09-19 实测：连续两次刷写用同一个 255，第二次静默不生效 —— 本脚本的
#    "验证"因此抓到过一次"假成功"。这与本项目 QA 通道"内容不变不执行"是同一类坑。）
"$ADB" -s "$DEV" shell "setprop sys.zkupgrade.flag 0"
sleep 1
"$ADB" -s "$DEV" shell "setprop sys.zkupgrade.dir /tmp; setprop sys.zkupgrade.flag 255; setprop ctl.restart zkswe"
echo "   已下发属性（刷写 res + 重启 zkgui 应用；pid0=${PID0:-?}）"

# ★★ 等「zkgui pid 变化」，**不是**「uptime 归零」（2026-09-16 纠错）：
#   `setprop ctl.restart zkswe` 只重启 **zkgui 应用**，整机 uptime **不会归零**
#   （实测：发属性后 120s 内 uptime 从 1681.86 单调涨到 1806.05，从未回落）⇒ 旧判据永不成立，
#   每轮固定空等 30×5 = **150 秒**。改用 pid 变化 = 应用重启的硬判据，上限 40s。
echo "== 4/5 等 zkgui 应用重启（pid 变化） =="
for i in $(seq 1 20); do
  sleep 2
  PID="$(get_pid)"
  if [ -n "$PID" ] && [ "$PID" != "${PID0:-}" ]; then
    echo "   已重启应用（pid ${PID0:-?} -> $PID）"
    break
  fi
  [ -z "$PID" ] && { echo "   第 $i 次：zkgui 暂不在（正在重启）"; continue; }
  echo "   第 $i 次：pid=$PID 还是旧进程"
  [ "$i" = "20" ] && echo "   !! 40s 内 pid 没变化（应用没重启？查 sys.zkupgrade.* 与 /res 内容）"
done
step "刷写并重启应用"

echo "== 5/5 验证固化结果 =="
sleep 5
"$ADB" -s "$DEV" push "$BUSYBOX" /tmp/busybox >/dev/null 2>&1
"$ADB" -s "$DEV" shell "chmod 755 /tmp/busybox 2>/dev/null"
echo "--- /res/lib（应为我们的 libzkgui.so）---"
"$ADB" -s "$DEV" shell "ls -l /res/lib /res/ui 2>&1 | /tmp/busybox head -12"
# ★★ 固化判据必须落到**内容**上（2026-09-19 血案）：
#   「pid 变了」与「/tmp/lib 不见了」都可能被**无关的重启**满足 —— 实测第一次跑本脚本
#   打印的是「固化完成」，但 /res 里还是两天前的包（大小 4797204 vs 本次 4807596），
#   中间那个 pid 变化是应用因别的原因重启造成的。⇒ 唯一可靠判据：
#   **/res 里那个 .so 的字节数 == 本次编出来的那个**。不一致就**当场失败**，
#   别让"假成功"把旧包当成新包交付（这正是本脚本开头注释里 2026-09-16 那条血案的同类）。
LOCAL_SZ="$(stat -c %s .fun/v85x/libzkgui.so 2>/dev/null || echo 0)"
BUSYBOX_PATH="$BUSYBOX"
# 重启会清空 /tmp ⇒ 每次读 /res 前先确认 busybox 还在（不在就重推）
#   ⚠️ 2026-09-19 实测：设备**自己重启过**（uptime 归零），/tmp 里的 pginj/busybox 全没了，
#      而注入命令又带 `2>/dev/null` ⇒ 静默失败、白白查半小时。判据先保证工具在。
ensure_busybox() {
  if ! "$ADB" -s "$DEV" shell "[ -x /tmp/busybox ]" >/dev/null 2>&1; then
    "$ADB" -s "$DEV" push "$BUSYBOX_PATH" /tmp/busybox >/dev/null 2>&1
    "$ADB" -s "$DEV" shell "chmod 755 /tmp/busybox" >/dev/null 2>&1
    # ★ 不要静默：推不上就打出来（下面 dev_res_md5 有兜底，但要让现场知道工具没就位）
    if ! "$ADB" -s "$DEV" shell "[ -x /tmp/busybox ]" >/dev/null 2>&1; then
      echo "   ⚠️ /tmp/busybox 推不上去（设备刚重启/adb 抽风？）—— 改用『拉回来在 PC 侧算 md5』兜底"
    fi
  fi
}
# ★★ 2026-09-20 补：**设备侧读不到 md5 时必须兜底，不能静默返回空**。
#   血案：本次固化跑完，`/res` 里的 .so 其实**完全正确**（md5 与本地一致），
#   但 `/tmp/busybox` 因为设备重启被清掉、补推又没成功 ⇒ `dev_res_md5` 一直返回空
#   ⇒ 脚本在"只能等"的窗口里**白等满 10 分钟**，最后报"固化未确认"（**假失败**）。
#   现在：busybox 拿不到就把 `/res` 里那个文件 pull 回 PC 自己算 —— 判据等价，且不依赖设备工具。
dev_res_md5() {  # $1 = /res 里的路径 —— 返回该文件的 md5
  ensure_busybox
  local out
  out="$("$ADB" -s "$DEV" shell "/tmp/busybox md5sum '$1' 2>/dev/null" | tr -d '\r' | cut -d' ' -f1)"
  if [ -n "$out" ]; then
    echo "$out"
    return
  fi
  local tmp="$ROOT/out/.verify_pull.tmp"
  if "$ADB" -s "$DEV" pull "$1" "$(cygpath -w "$tmp" 2>/dev/null || echo "$tmp")" >/dev/null 2>&1; then
    md5sum "$tmp" 2>/dev/null | cut -d' ' -f1
    rm -f "$tmp"
  fi
}
# ★★ 判据用 **md5** 而不是字节数：字节数相同 ≠ 内容相同 —— 连续两次构建（例如只改了图、
#    没改 C 代码）`.so` 尺寸一模一样，比字节数会**假通过**，把"没刷进去"当成"刷进去了"。
LOCAL_MD5="$(md5sum .fun/v85x/libzkgui.so 2>/dev/null | cut -d' ' -f1)"
DEL_MD5="$(dev_res_md5 /res/lib/libzkgui.so)"
echo "--- 固化判据（比内容 md5）：本次构建 $LOCAL_MD5 / /res 里 ${DEL_MD5:-（读不到）} ---"

# ★★★ 2026-09-19 两次实测的结论：**不要"窗口内没对上就自动重触发"**。
#   升级 res 的写入是「卸载 → 擦除 → 写 → 挂回」，全程 **4~10 分钟**，
#   这期间 `/res` 一定是空的（读不到）。而重触发会**重新开始一轮擦写**，
#   把离线时间越拖越长 ⇒ 窗口内永远读不到 ⇒ **假失败**（两次都报失败、设备上其实全对）。
#   ★ 更要命的是：在写入过程中重复触发/重启 = 打断擦写 = 把分区擦成空白（本文件上方那条血案）。
#   ⇒ 语义改成：**触发一次**（上面第 3 步已做）→ 只等 → 等到了才判成功。
OK=0
if [ -n "$DEL_MD5" ] && [ "$LOCAL_MD5" = "$DEL_MD5" ]; then
  OK=1
else
  echo "   ⚠️ 现在读不到/还是旧值 —— 升级正在重写 res，**只能等**（实测 4~10 分钟）"
  for i in $(seq 1 100); do
    sleep 6
    # ① 设备可能因升级重启 ⇒ adb 短暂 device not found，先等它回来
    if ! "$ADB" -s "$DEV" shell "true" >/dev/null 2>&1; then
      "$ADB" -s "$DEV" wait-for-device >/dev/null 2>&1
      sleep 6
    fi
    # ② 等 /res 挂回来（没挂上时读它必然失败 ⇒ 不算失败）
    if ! "$ADB" -s "$DEV" shell "cat /proc/mounts 2>/dev/null" | grep -q " /res "; then
      [ $((i % 10)) = 0 ] && echo "   … /res 尚未挂回（已等 $((i * 6))s）"
      continue
    fi
    # ③ 挂回来了再看内容
    DEL_MD5="$(dev_res_md5 /res/lib/libzkgui.so)"
    if [ -n "$DEL_MD5" ] && [ "$LOCAL_MD5" = "$DEL_MD5" ]; then
      echo "   ✓ 等到 /res 挂回且为本构建（约 $((i * 6))s）"
      OK=1
      break
    fi
    [ $((i % 10)) = 0 ] && echo "   … /res 已挂回但内容还不是本次（已等 $((i * 6))s）"
  done
fi
if [ "${OK:-0}" != "1" ]; then
  echo "!! **固化未确认**：等满 10 分钟 /res 仍未变成本次构建（读到 ${DEL_MD5:-空}，本次 $LOCAL_MD5）"
  echo "   ⇒ **再走一遍升级流程**（不要手工 mount / 不要手工 dd / 不要在写入期间重启或重复触发）"
  exit 1
fi
echo "   ✓ /res 已是本次构建（md5 $DEL_MD5）"

echo "--- 启动配置 ---"
"$ADB" -s "$DEV" shell "cat /res/etc/EasyUI.cfg 2>&1 | /tmp/busybox grep -E 'startupLibPath|resPath|font|touchDev'"
echo "--- 进程 / 内存 ---"
PID=$("$ADB" -s "$DEV" shell "ps" 2>/dev/null | grep zkgui | awk '{print $2}' | tr -d '\r')
"$ADB" -s "$DEV" shell "ps | /tmp/busybox grep '[z]kgui'; ls /tmp/lib 2>&1; cat /proc/meminfo | /tmp/busybox grep -E 'MemFree|Shmem'"
echo "   (pid=$PID；/tmp/lib 应为 No such file —— 说明跑的是 flash 里的 app)"

# ★★ 2026-09-20 新增：**跑的是不是"本次"的新库**？
#   md5 判据只证明**盘上的文件**对，不证明**内存里的进程**对（本次实测：固化报成功、
#   md5 也对，但应用里的状态文件缺"只有新代码才有的字段" ⇒ 跑的仍是上一版）。
#   这里把两种可能都点出来，免得上机后拿旧行为当新行为判。
"$ADB" -s "$DEV" shell "setprop ctl.restart zkswe" >/dev/null 2>&1
echo "   ✓ 已请求重启应用（setprop ctl.restart zkswe）—— 保证内存里跑的是新库"
echo "     ⚠️ 确认『跑新库』要用**只有新代码才有的判据**（新字段/新日志/新像素），「pid 变了」不算"

# ★★ 2026-09-20 新增：**/res 到底挂上没有**（通用自检 + 恢复指引）。
#   本次实测：固化报成功、md5 也对，但**接着连续整机 reboot 之后**，mtd3 的 squashfs
#   superblock 读不到了（`SQUASHFS error: Can't find a SQUASHFS superblock on mtdblock3`）
#   ⇒ /res 是空的、应用起不来（无日志、无状态文件、QA 通道不响应）。
#   此时**唯一正路是重跑本脚本**（它自己会"卸载→擦→写→挂回"）—— 实测重跑一次即恢复。
#   ⚠️ 绝不手工 mount / dd 回写（工程红线），也别在写入期间整机 reboot 或重复触发。
if ! "$ADB" -s "$DEV" shell "cat /proc/mounts 2>/dev/null" | grep -q " /res "; then
  echo "   !! **/res 没挂上**（应用会起不来）—— 重跑本脚本即恢复："
  echo "        tools/upgrade_device.sh $VER"
  echo "      （别手工 mount / 别手工 dd 回写 / 别急着整机 reboot）"
  exit 1
fi
echo "   ✓ /res 挂载正常"
step "验证完成"
echo "== 固化完成：全程 $(( $(date +%s) - T_ALL0 ))s（改这一版前的旧脚本要 ~180s，其中 150s 是白等）=="
