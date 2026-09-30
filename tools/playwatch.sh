#!/bin/sh
# 设备端：播放期间每秒采样 wlan0 速率 + CPU 空闲 + MemFree + 应用 pid
#   pid 变了 = 应用被 OOM 杀掉重启（这是判断"异常"的关键指标）
#   用法: sh /tmp/playwatch.sh <秒数> [标签]
B=/tmp/busybox
SEC="${1:-12}"
TAG="${2:-}"
rx() { $B cat /proc/net/dev | $B awk '/wlan0:/{print $2}'; }
cpu() { $B cat /proc/stat | $B awk '/^cpu /{idle=$5; tot=0; for(i=2;i<=NF;i++) tot+=$i; printf "%s %s", idle, tot}'; }
mf() { $B grep MemFree /proc/meminfo | $B awk '{print $2}'; }
echo "=== playwatch $TAG ==="
R0=$(rx); C=$(cpu); I0=${C% *}; T0=${C#* }
PID0=$($B pidof zkgui)
echo "起始 pid=$PID0 MemFree=$(mf)kB"
i=1
while [ "$i" -le "$SEC" ]; do
  $B sleep 1
  R1=$(rx); C=$(cpu); I1=${C% *}; T1=${C#* }
  D=$((R1 - R0)); DI=$((I1 - I0)); DT=$((T1 - T0))
  if [ "$DT" -gt 0 ]; then IDLE=$((DI * 100 / DT)); else IDLE=0; fi
  P=$($B pidof zkgui)
  MARK=""
  [ "$P" != "$PID0" ] && MARK="  <<< pid 变了(OOM重启)!"
  echo "t=${i}s rx=$((D * 8 / 1000))kbps cpu=${IDLE}%free mem=$(mf)kB pid=$P$MARK"
  R0=$R1; I0=$I1; T0=$T1
  i=$((i + 1))
done
echo "=== playwatch done ==="
