#!/bin/sh
# dlna_devtest.sh - DLNA 渲染端「设备内自测」（不需要 PC / 不需要真正的局域网控制器）
#
# 做四件事：
#   1) GET /description.xml          看设备描述对不对
#   2) SSDP M-SEARCH（单播打到 127.0.0.1:1900）看 SSDP 线程有没有应答
#   3) GET 某个 SCPD                  看服务描述能不能拉
#   4) SOAP 走一遍：SetAVTransportURI -> GetTransportInfo -> Play -> Stop
#      （URI 用设备自己 busybox httpd 起的本地源，避免依赖外网）
#
# 用法（设备上）：/tmp/dlna_devtest.sh [端口] [媒体URL]
#   默认端口 8200；媒体默认用一个由本脚本自己起的 busybox httpd 提供 /tmp/test.mp4
BUSY=/tmp/busybox
PORT=${1:-8200}
CTRL="http://127.0.0.1:$PORT/upnp/control"

echo "===== 1) 设备描述 /description.xml ====="
$BUSY wget -q -O - "http://127.0.0.1:$PORT/description.xml" | $BUSY head -c 900
echo

echo "===== 2) SSDP M-SEARCH -> 127.0.0.1:1900 ====="
$BUSY printf 'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 1\r\nST: upnp:rootdevice\r\n\r\n' > /tmp/msrch.txt
$BUSY nc -u -w 3 127.0.0.1 1900 < /tmp/msrch.txt | $BUSY head -12
echo

echo "===== 3) SCPD /AVTransport/scpd.xml ====="
$BUSY wget -q -O - "http://127.0.0.1:$PORT/AVTransport/scpd.xml" | $BUSY head -c 260
echo

# ---------- SOAP 小工具：POST 一个 action ----------
# 用法: soap <Service> <Action> <argsXml...>
soap() {
  SVC=$1; ACT=$2; shift 2
  ARGS="$*"
  BODY="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:$ACT xmlns:u=\"urn:schemas-upnp-org:service:$SVC:1\">$ARGS</u:$ACT></s:Body></s:Envelope>"
  $BUSY printf '%s' "$BODY" > /tmp/soap.xml
  LEN=$($BUSY stat -c %s /tmp/soap.xml)
  {
    $BUSY printf 'POST /upnp/control/%s HTTP/1.1\r\n' "$SVC"
    $BUSY printf 'Host: 127.0.0.1:%s\r\n' "$PORT"
    $BUSY printf 'SOAPACTION: "urn:schemas-upnp-org:service:%s:1#%s"\r\n' "$SVC" "$ACT"
    $BUSY printf 'Content-Type: text/xml; charset="utf-8"\r\n'
    $BUSY printf 'Content-Length: %s\r\n' "$LEN"
    $BUSY printf 'Connection: close\r\n\r\n'
    $BUSY cat /tmp/soap.xml
  } > /tmp/soapreq.bin
  $BUSY nc -w 5 127.0.0.1 $PORT < /tmp/soapreq.bin | $BUSY tr -d '\r' | $BUSY grep -E 'HTTP/|CurrentTransportState|<CurrentVolume>|errorCode' | $BUSY head -6
}

echo "===== 4) SOAP: GetProtocolInfo (ConnectionManager) ====="
soap ConnectionManager GetProtocolInfo ""
echo
echo "===== 5) SOAP: GetTransportInfo (AVTransport) ====="
soap AVTransport GetTransportInfo "<InstanceID>0</InstanceID>"
echo
echo "===== 6) SOAP: SetAVTransportURI (本地 httpd 提供的 /tmp/test.mp4) ====="
$BUSY ps | $BUSY grep -q "[h]ttpd" || $BUSY httpd -p 8899 -h /tmp
URL=${2:-http://127.0.0.1:8899/test.mp4}
echo "  媒体 URL = $URL"
soap AVTransport SetAVTransportURI "<InstanceID>0</InstanceID><CurrentURI>$URL</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
echo "  （等 8s 让下载完成并起播）"
$BUSY sleep 8
echo
echo "===== 7) SOAP: GetTransportInfo（应为 PLAYING）====="
soap AVTransport GetTransportInfo "<InstanceID>0</InstanceID>"
echo
echo "===== 8) SOAP: GetPositionInfo ====="
soap AVTransport GetPositionInfo "<InstanceID>0</InstanceID>"
echo
echo "===== 9) SOAP: Pause -> GetTransportInfo -> Play -> Stop ====="
soap AVTransport Pause "<InstanceID>0</InstanceID>"
$BUSY sleep 1
soap AVTransport GetTransportInfo "<InstanceID>0</InstanceID>"
soap AVTransport Play "<InstanceID>0</InstanceID><Speed>1</Speed>"
$BUSY sleep 1
soap AVTransport Stop "<InstanceID>0</InstanceID>"
$BUSY sleep 1
soap AVTransport GetTransportInfo "<InstanceID>0</InstanceID>"
echo
echo "===== 10) RenderingControl: GetVolume ====="
soap RenderingControl GetVolume "<InstanceID>0</InstanceID><Channel>Master</Channel>"
echo
echo "===== 完成 ====="
