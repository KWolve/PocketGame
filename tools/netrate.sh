#!/bin/sh
# 设备端原始吞吐测量（不经过我们的应用解码链路）
#   用法（设备上）: sh /tmp/netrate.sh <url> <秒数> [标签]
#   原理: 后台 busybox wget 拉大文件，每秒读 /proc/net/dev 的 wlan0 RX 字节数，
#         打印瞬时速率；顺带打印 CPU 空闲率（/proc/stat），用来区分
#         "无线链路差" vs "CPU 被占满导致协议栈喂不动"。
B=/tmp/busybox
URL="${1:-http://192.168.0.110:8099/big20m.bin}"
SEC="${2:-8}"
TAG="${3:-}"

# ⚠️ 不要对 $0 做 gsub（会重切字段，$2 变空 → 速率恒 0）：直接取第 2 列（第 1 列是 "wlan0:"）
rx() { $B cat /proc/net/dev | $B awk '/wlan0:/{print $2}'; }
cpu() { $B cat /proc/stat | $B awk '/^cpu /{idle=$5; tot=0; for(i=2;i<=NF;i++) tot+=$i; printf "%s %s", idle, tot}'; }

echo "=== netrate $TAG url=$URL sec=$SEC ==="
$B wget -q -O /dev/null "$URL" &
WPID=$!

R0=$(rx)
C=$(cpu); I0=${C% *}; T0=${C#* }

i=1
while [ "$i" -le "$SEC" ]; do
  $B sleep 1
  R1=$(rx)
  C=$(cpu); I1=${C% *}; T1=${C#* }
  D=$((R1 - R0))
  DI=$((I1 - I0)); DT=$((T1 - T0))
  if [ "$DT" -gt 0 ]; then IDLE=$((DI * 100 / DT)); else IDLE=0; fi
  echo "t=${i}s  rx=$((D / 1024))KB/s  ($((D * 8 / 1000))kbps)  cpu_idle=${IDLE}%"
  R0=$R1; I0=$I1; T0=$T1
  i=$((i + 1))
done
$B kill $WPID 2>/dev/null
$B killall wget 2>/dev/null
echo "=== netrate done ==="
