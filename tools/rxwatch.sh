#!/bin/sh
# 设备端：纯采样 wlan0 收包速率 + CPU 空闲率（不动网络，用于配合应用侧播放看"喂进来的速度"）
#   用法: sh /tmp/rxwatch.sh <秒数> [标签]
B=/tmp/busybox
SEC="${1:-10}"
TAG="${2:-}"
rx() { $B cat /proc/net/dev | $B awk '/wlan0:/{print $2}'; }
cpu() { $B cat /proc/stat | $B awk '/^cpu /{idle=$5; tot=0; for(i=2;i<=NF;i++) tot+=$i; printf "%s %s", idle, tot}'; }
echo "=== rxwatch $TAG ==="
R0=$(rx); C=$(cpu); I0=${C% *}; T0=${C#* }
i=1
while [ "$i" -le "$SEC" ]; do
  $B sleep 1
  R1=$(rx); C=$(cpu); I1=${C% *}; T1=${C#* }
  D=$((R1 - R0)); DI=$((I1 - I0)); DT=$((T1 - T0))
  if [ "$DT" -gt 0 ]; then IDLE=$((DI * 100 / DT)); else IDLE=0; fi
  echo "t=${i}s  rx=$((D * 8 / 1000))kbps ($((D / 1024))KB/s)  cpu_idle=${IDLE}%"
  R0=$R1; I0=$I1; T0=$T1
  i=$((i + 1))
done
echo "=== rxwatch done ==="
