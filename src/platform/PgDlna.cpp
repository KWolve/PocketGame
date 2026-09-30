/*
 * PgDlna.cpp - DLNA/UPnP MediaRenderer 最小实现（见 PgDlna.h 的说明）
 */
#include "platform/PgDlna.h"

#include <arpa/inet.h>
#include <dlfcn.h>   // dlopen：civetweb 的 TLS 是运行时 dlopen libssl/libcrypto
#include <errno.h>
#include <fcntl.h>   // fcntl/O_NONBLOCK（connect 非阻塞 + select 等待）
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>   // IFF_UP / IFF_LOOPBACK（挑本机 IP 时过滤网卡状态）
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/vfs.h>   // statfs：落盘前查空间 + 判断是不是 tmpfs（f_type）
#include <sys/types.h>
#include <time.h>      // clock_gettime：SSDP alive 通告的周期计时
#include <unistd.h>

#include "civetweb.h"
#include "manager/ConfigManager.h"   // CONFIGMANAGER->getResFilePath：定位 /res/ui/certs/cacert.pem
#include "utils/Log.h"
#include "platform/PgTime.h"   // 证书有效期校验依赖系统时间（NTP 校时）

namespace pg {

/* ============================ 常量 ============================ */

static const char *kUuid = "8f4b3a72-1c5e-4d6a-9b21-3f2e7c1a5d40";
static const char *kDevType = "urn:schemas-upnp-org:device:MediaRenderer:1";
static const char *kAvtSvc = "urn:schemas-upnp-org:service:AVTransport:1";
static const char *kRcsSvc = "urn:schemas-upnp-org:service:RenderingControl:1";
static const char *kCmsSvc = "urn:schemas-upnp-org:service:ConnectionManager:1";
static const int kSsdpPort = 1900;
static const char *kSsdpGroup = "239.255.255.250";
/* 落盘到 tmpfs 时的体积上限（/tmp 吃的是内存，本板 56MB —— 宁可不放也不能吃光）。
 * 实测：无视长度直接下，播放中进程会被 OOM 杀掉。 */
#define PG_DLNA_TMPFS_MAX_BYTES (10LL * 1024 * 1024)

/* 支持的 Sink 协议（手机侧会挑它认识的） */
static const char *kSinkProto =
    "http-get:*:video/mp4:*,http-get:*:video/mpeg:*,http-get:*:video/x-matroska:*,"
    "http-get:*:video/avi:*,http-get:*:video/x-msvideo:*,http-get:*:video/quicktime:*,"
    "http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*,http-get:*:audio/x-wav:*,"
    "http-get:*:image/jpeg:*,http-get:*:image/png:*,http-get:*:*:*";

/* ============================ 小工具 ============================ */

// 从 XML 里取 <tag>value</tag>（不做转义解码，够用）
/* XML 实体反转义（就地，目标缓冲区必须够大）。
 * ⚠️ 必须做：SOAP 请求体里的 URI 会把 `&` 转义成 `&amp;`，直接拿去下载 =
 *    查询串变成 `...&amp;ua=...`，CDN 判定签名/参数非法 → 实测 B 站返回 **HTTP 959**、
 *    0 字节。用户侧的表现就是"选了设备但没反应、屏幕没画面"（因为下载失败 →
 *    startCast() 根本没被调用 → 投屏页不跳转）。
 *    解码顺序：先把 lt/gt/quot/apos 换掉，**最后**再换 &amp;（否则 `&amp;lt;` 会被二次解码）。 */
static void xmlUnescape(char *s) {
  if (!s || !strchr(s, '&')) return;
  struct Ent { const char *ent; char ch; };
  static const Ent kEnts[] = {{"&lt;", '<'}, {"&gt;", '>'},
                              {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
  for (int i = 0; i < (int)(sizeof(kEnts) / sizeof(kEnts[0])); ++i) {
    const char *e = kEnts[i].ent;
    size_t el = strlen(e);
    char *p;
    while ((p = strstr(s, e)) != 0) {
      *p = kEnts[i].ch;
      memmove(p + 1, p + el, strlen(p + el) + 1);
      s = p + 1;  // 从替换处继续（内容不会自嵌套）
    }
  }
  {  // &amp; 放最后
    char *p;
    while ((p = strstr(s, "&amp;")) != 0) {
      *p = '&';
      memmove(p + 1, p + 5, strlen(p + 5) + 1);
    }
  }
}

static bool xmlTag(const char *xml, const char *tag, char *out, int outLen) {
  if (!xml || !tag) return false;
  char open[64], close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *p = strstr(xml, open);
  if (!p) {
    // 兼容带命名空间前缀：<u:tag>
    snprintf(open, sizeof(open), ":%s>", tag);
    p = strstr(xml, open);
    if (!p) return false;
    p = strchr(p, '>');
    if (!p) return false;
    p++;
  } else {
    p += strlen(open);
  }
  const char *e = strstr(p, close);
  if (!e) {  // 兼容 </u:tag>
    snprintf(close, sizeof(close), ":%s>", tag);
    e = strstr(p, close);
    if (!e) return false;
  }
  int n = (int)(e - p);
  if (n >= outLen) n = outLen - 1;
  memcpy(out, p, n);
  out[n] = 0;
  xmlUnescape(out);  // XML 实体反转义（见 xmlUnescape 的说明，不做就会下不下来）
  return true;
}

// 毫秒 -> "H:MM:SS"
static void msToTime(int ms, char *out, int n) {
  if (ms < 0) ms = 0;
  int s = ms / 1000;
  snprintf(out, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
}

/* ============================ XML 内容 ============================ */

static const char *kDescriptionFmt =
    "<?xml version=\"1.0\"?>"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion>"
    "<device>"
    "<deviceType>%s</deviceType>"
    "<friendlyName>%s</friendlyName>"
    "<manufacturer>PocketGame</manufacturer>"
    "<manufacturerURL>http://www.flythings.cn</manufacturerURL>"
    "<modelDescription>PocketGame DLNA 渲染器</modelDescription>"
    "<modelName>PocketGame DMR</modelName>"
    "<modelNumber>1.0</modelNumber>"
    "<UDN>uuid:%s</UDN>"
    "<serviceList>"
    "<service><serviceType>%s</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
    "<SCPDURL>/AVTransport/scpd.xml</SCPDURL><controlURL>/upnp/control/AVTransport</controlURL>"
    "<eventSubURL>/upnp/event/AVTransport</eventSubURL></service>"
    "<service><serviceType>%s</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
    "<SCPDURL>/RenderingControl/scpd.xml</SCPDURL><controlURL>/upnp/control/RenderingControl</controlURL>"
    "<eventSubURL>/upnp/event/RenderingControl</eventSubURL></service>"
    "<service><serviceType>%s</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
    "<SCPDURL>/ConnectionManager/scpd.xml</SCPDURL><controlURL>/upnp/control/ConnectionManager</controlURL>"
    "<eventSubURL>/upnp/event/ConnectionManager</eventSubURL></service>"
    "</serviceList>"
    "</device></root>";

static const char *kScpdHead =
    "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
    "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>";

static const char *kScpdTail = "</actionList><serviceStateTable/></scpd>";

/* 每个动作：参数列表（in/out 都要按 UPnP 规范写，缺了部分控制器会不认这个动作） */
static const char *kAvtActions =
    "<action><name>SetAVTransportURI</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>CurrentURI</name><direction>in</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
    "<argument><name>CurrentURIMetaData</name><direction>in</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetMediaInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>NrTracks</name><direction>out</direction><relatedStateVariable>NumberOfTracks</relatedStateVariable></argument>"
    "<argument><name>MediaDuration</name><direction>out</direction><relatedStateVariable>CurrentMediaDuration</relatedStateVariable></argument>"
    "<argument><name>CurrentURI</name><direction>out</direction><relatedStateVariable>AVTransportURI</relatedStateVariable></argument>"
    "<argument><name>CurrentURIMetaData</name><direction>out</direction><relatedStateVariable>AVTransportURIMetaData</relatedStateVariable></argument>"
    "<argument><name>NextURI</name><direction>out</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
    "<argument><name>NextURIMetaData</name><direction>out</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
    "<argument><name>PlayMedium</name><direction>out</direction><relatedStateVariable>PlaybackStorageMedium</relatedStateVariable></argument>"
    "<argument><name>RecordMedium</name><direction>out</direction><relatedStateVariable>RecordStorageMedium</relatedStateVariable></argument>"
    "<argument><name>WriteStatus</name><direction>out</direction><relatedStateVariable>RecordMediumWriteStatus</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetTransportInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportState</name><direction>out</direction><relatedStateVariable>TransportState</relatedStateVariable></argument>"
    "<argument><name>CurrentTransportStatus</name><direction>out</direction><relatedStateVariable>TransportStatus</relatedStateVariable></argument>"
    "<argument><name>CurrentSpeed</name><direction>out</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetPositionInfo</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Track</name><direction>out</direction><relatedStateVariable>CurrentTrack</relatedStateVariable></argument>"
    "<argument><name>TrackDuration</name><direction>out</direction><relatedStateVariable>CurrentTrackDuration</relatedStateVariable></argument>"
    "<argument><name>TrackMetaData</name><direction>out</direction><relatedStateVariable>CurrentTrackMetaData</relatedStateVariable></argument>"
    "<argument><name>TrackURI</name><direction>out</direction><relatedStateVariable>CurrentTrackURI</relatedStateVariable></argument>"
    "<argument><name>RelTime</name><direction>out</direction><relatedStateVariable>RelativeTimePosition</relatedStateVariable></argument>"
    "<argument><name>AbsTime</name><direction>out</direction><relatedStateVariable>AbsoluteTimePosition</relatedStateVariable></argument>"
    "<argument><name>RelCount</name><direction>out</direction><relatedStateVariable>RelativeCounterPosition</relatedStateVariable></argument>"
    "<argument><name>AbsCount</name><direction>out</direction><relatedStateVariable>AbsoluteCounterPosition</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetDeviceCapabilities</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>PlayMedia</name><direction>out</direction><relatedStateVariable>PossiblePlaybackStorageMedia</relatedStateVariable></argument>"
    "<argument><name>RecMedia</name><direction>out</direction><relatedStateVariable>PossibleRecordStorageMedia</relatedStateVariable></argument>"
    "<argument><name>RecQualityModes</name><direction>out</direction><relatedStateVariable>PossibleRecordQualityModes</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetTransportSettings</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>PlayMode</name><direction>out</direction><relatedStateVariable>CurrentPlayMode</relatedStateVariable></argument>"
    "<argument><name>RecQualityMode</name><direction>out</direction><relatedStateVariable>CurrentRecordQualityMode</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Stop</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Play</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Speed</name><direction>in</direction><relatedStateVariable>TransportPlaySpeed</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Pause</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Seek</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Unit</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekMode</relatedStateVariable></argument>"
    "<argument><name>Target</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SeekTarget</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Next</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>Previous</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>SetNextAVTransportURI</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>NextURI</name><direction>in</direction><relatedStateVariable>NextAVTransportURI</relatedStateVariable></argument>"
    "<argument><name>NextURIMetaData</name><direction>in</direction><relatedStateVariable>NextAVTransportURIMetaData</relatedStateVariable></argument>"
    "</argumentList></action>";

static const char *kRcsActions =
    "<action><name>GetVolume</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>CurrentVolume</name><direction>out</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>SetVolume</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>DesiredVolume</name><direction>in</direction><relatedStateVariable>Volume</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetMute</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>CurrentMute</name><direction>out</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>SetMute</name><argumentList>"
    "<argument><name>InstanceID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_InstanceID</relatedStateVariable></argument>"
    "<argument><name>Channel</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Channel</relatedStateVariable></argument>"
    "<argument><name>DesiredMute</name><direction>in</direction><relatedStateVariable>Mute</relatedStateVariable></argument>"
    "</argumentList></action>";

static const char *kCmsActions =
    "<action><name>GetProtocolInfo</name><argumentList>"
    "<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>"
    "<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetCurrentConnectionIDs</name><argumentList>"
    "<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>"
    "</argumentList></action>"
    "<action><name>GetCurrentConnectionInfo</name><argumentList>"
    "<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
    "<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>"
    "<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>"
    "<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>"
    "<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>"
    "<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>"
    "<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>"
    "<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>"
    "</argumentList></action>";

/* ============================ SOAP 应答 ============================ */

// 生成 SOAP 成功应答（body 里是 <u:ActionResponse xmlns:u=svc>args</u:ActionResponse>）
static void soapOk(struct mg_connection *conn, const char *svc, const char *action,
                   const char *argsXml) {
  char body[2048];
  snprintf(body, sizeof(body),
           "<?xml version=\"1.0\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
           "<u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>"
           "</s:Body></s:Envelope>",
           action, svc, argsXml ? argsXml : "", action);
  mg_printf(conn,
            "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
            "EXT:\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(body), body);
}

// 生成 SOAP Fault（UPnP 错误码）
static void soapFault(struct mg_connection *conn, int code, const char *desc) {
  char body[1024];
  snprintf(body, sizeof(body),
           "<?xml version=\"1.0\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
           "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
           "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
           "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
           "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
           code, desc);
  mg_printf(conn,
            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            (int)strlen(body), body);
}

/* ============================ Dlna 实现 ============================ */

Dlna::Dlna()
    : running_(false), httpPort_(0), ctx_(0), stopFlag_(0), aliveReq_(0),
      ssdpJoinable_(0), dlBusy_(0),
      downloadPercent_(-1), positionMs_(0), durationMs_(0), volume_(70),
      qHead_(0), qTail_(0) {
  localIp_[0] = 0;
  friendlyName_[0] = 0;
  strcpy(transportState_, "STOPPED");
  lastUri_[0] = 0;
  mediaPath_[0] = 0;
}

Dlna::~Dlna() { stop(); }

void Dlna::pushAction(int type, const char *path, int arg) {
  pthread_mutex_lock(&qMtx_);
  int next = (qHead_ + 1) % 16;
  if (next != qTail_) {  // 满则丢弃（UI 追不上时不该阻塞网络线程）
    q_[qHead_].type = type;
    q_[qHead_].arg = arg;
    snprintf(q_[qHead_].path, sizeof(q_[qHead_].path), "%s", path ? path : "");
    qHead_ = next;
  } else {
    LOGD("PgDlna: 动作队列满，丢弃 type=%d", type);
  }
  pthread_mutex_unlock(&qMtx_);
}

bool Dlna::pollAction(DlnaAction *out) {
  bool got = false;
  pthread_mutex_lock(&qMtx_);
  if (qTail_ != qHead_) {
    *out = q_[qTail_];
    qTail_ = (qTail_ + 1) % 16;
    got = true;
  }
  pthread_mutex_unlock(&qMtx_);
  return got;
}

void Dlna::setTransportState(const char *state) {
  snprintf(transportState_, sizeof(transportState_), "%s", state ? state : "STOPPED");
  LOGD("PgDlna: 状态 -> %s", transportState_);
}

void Dlna::setVolumeInternal(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  volume_ = pct;
}

/* ---------- 本机 IP（SSDP LOCATION 要用对，否则控制器连不上） ----------
 * ⚠️ 必须"每次用之前重算"，不能只在 start() 里算一次：
 *    本板 DLNA 常在开机早期/应用刚拉起时启动，那一刻 wlan0 可能还没拿到 DHCP 地址，
 *    定格下来就是 127.0.0.1 → 控制器拿到 LOCATION=http://127.0.0.1:8200/... 永远连不上
 *    （实测踩过：重启后启 DLNA，日志 `本机IP=127.0.0.1`）。
 * 优先 wlan0（本机上网/投屏走 WiFi），其次第一个 UP 且非环回的四层地址。 */
static void pickLocalIp(char *out, int n) {
  struct ifaddrs *ifa = 0;
  if (getifaddrs(&ifa) == 0) {
    const char *best = 0;
    const char *fallback = 0;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
      if (!(p->ifa_flags & IFF_UP)) continue;         // 网卡没起来不算
      if (p->ifa_flags & IFF_LOOPBACK) continue;     // 环回不算
      const char *ip = inet_ntoa(((struct sockaddr_in *)p->ifa_addr)->sin_addr);
      if (strcmp(ip, "127.0.0.1") == 0) continue;
      if (!fallback) fallback = ip;
      if (p->ifa_name && strcmp(p->ifa_name, "wlan0") == 0) { best = ip; break; }
    }
    const char *use = best ? best : fallback;
    if (use) {
      snprintf(out, n, "%s", use);
      freeifaddrs(ifa);
      return;
    }
    freeifaddrs(ifa);
  }
  snprintf(out, n, "127.0.0.1");
}

void Dlna::refreshLocalIp() { pickLocalIp(localIp_, sizeof(localIp_)); }

/* ---------- SSDP：收 M-SEARCH 并应答 + 主动 alive 通告 ---------- */

long ssdpNowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 本设备对外宣告的**全部** SSDP 目标。
 * ⚠️ 规范要求：收到 `ST: ssdp:all` 的 M-SEARCH 时，必须**为每个目标各回一条应答**，
 *    而不是回一条 `ST: ssdp:all`（回 ssdp:all 看起来"有应答"，但严格的控制器会因
 *    ST 与其搜索目标不匹配而丢掉它 → 表现就是"手机搜不到"，实测踩过）。
 *    NOTIFY 通告同样要逐个目标发。 */
static const char *kSsdpTargets[] = {
    "upnp:rootdevice",
    kDevType,
    kAvtSvc,
    kRcsSvc,
    kCmsSvc,
};
static const int kSsdpTargetCount =
    (int)(sizeof(kSsdpTargets) / sizeof(kSsdpTargets[0]));

/* 按 UPnP 惯例算 USN：`uuid:xxx::<ST>`；只有 ST 本身就是 uuid 时 USN 就等于它 */
static void ssdpMakeUsn(const char *st, char *usn, int n) {
  if (strncmp(st, "uuid:", 5) == 0) snprintf(usn, n, "%s", st);
  else snprintf(usn, n, "uuid:%s::%s", kUuid, st);
}

/* 一条 M-SEARCH 应答（ST 必须原样回给搜索方，不能改写） */
static void ssdpReply(int fd, struct sockaddr_in *peer, socklen_t plen,
                      const char *ip, int port, const char *st) {
  char usn[192];
  ssdpMakeUsn(st, usn, sizeof(usn));
  char resp[1024];
  int rl = snprintf(resp, sizeof(resp),
                    "HTTP/1.1 200 OK\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "EXT:\r\n"
                    "LOCATION: http://%s:%d/description.xml\r\n"
                    "SERVER: Linux/4.9 UPnP/1.0 PocketGameDMR/1.0\r\n"
                    "ST: %s\r\n"
                    "USN: %s\r\n"
                    "Content-Length: 0\r\n\r\n",
                    ip, port, st, usn);
  sendto(fd, resp, rl, 0, (struct sockaddr *)peer, plen);
  LOGD("PgDlna: SSDP 应答 %s (ST=%s)", inet_ntoa(peer->sin_addr), st);
}

/* 主动通告（ssdp:alive / ssdp:byebye）。
 * 为什么必须有：只应答 M-SEARCH 的话，设备**只会在对方主动搜索时才现身**；
 * 很多手机（系统投屏 / 视频 App 的投屏按钮）是"进页面就列已发现的设备"，
 * 依赖设备自己 NOTIFY 上播 —— 没有 alive 通告 = 列表里空空如也。 */
static void ssdpNotify(int fd, const char *ip, int port, bool alive) {
  struct sockaddr_in grp;
  memset(&grp, 0, sizeof(grp));
  grp.sin_family = AF_INET;
  grp.sin_port = htons(kSsdpPort);
  grp.sin_addr.s_addr = inet_addr(kSsdpGroup);
  for (int i = 0; i < kSsdpTargetCount; ++i) {
    const char *st = kSsdpTargets[i];
    char usn[192];
    ssdpMakeUsn(st, usn, sizeof(usn));
    char msg[1100];
    int ml;
    if (alive) {
      ml = snprintf(msg, sizeof(msg),
                    "NOTIFY * HTTP/1.1\r\n"
                    "HOST: %s:%d\r\n"
                    "CACHE-CONTROL: max-age=1800\r\n"
                    "LOCATION: http://%s:%d/description.xml\r\n"
                    "SERVER: Linux/4.9 UPnP/1.0 PocketGameDMR/1.0\r\n"
                    "NT: %s\r\n"
                    "NTS: ssdp:alive\r\n"
                    "USN: %s\r\n\r\n",
                    kSsdpGroup, kSsdpPort, ip, port, st, usn);
    } else {
      ml = snprintf(msg, sizeof(msg),
                    "NOTIFY * HTTP/1.1\r\n"
                    "HOST: %s:%d\r\n"
                    "NT: %s\r\n"
                    "NTS: ssdp:byebye\r\n"
                    "USN: %s\r\n\r\n",
                    kSsdpGroup, kSsdpPort, st, usn);
    }
    sendto(fd, msg, ml, 0, (struct sockaddr *)&grp, sizeof(grp));
  }
  LOGD("PgDlna: SSDP %s 通告已发（%d 个目标，LOCATION=http://%s:%d/description.xml）",
       alive ? "alive" : "byebye", kSsdpTargetCount, ip, port);
}

void *pgDlnaSsdpThread(void *arg) {
  Dlna *self = (Dlna *)arg;
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    LOGD("PgDlna: SSDP socket 失败 %s", strerror(errno));
    return 0;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kSsdpPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOGD("PgDlna: SSDP bind 1900 失败 %s", strerror(errno));
    close(fd);
    return 0;
  }
  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));
  mreq.imr_multiaddr.s_addr = inet_addr(kSsdpGroup);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    LOGD("PgDlna: SSDP 加入组播失败(仍继续收单播) %s", strerror(errno));
  }
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 400000;  // 400ms 超时，便于检查 stopFlag_
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  LOGD("PgDlna: SSDP 已监听 %s:%d (本机 %s)", kSsdpGroup, kSsdpPort, self->localIp());
  /* 启动即通告一次：让已在监听 NOTIFY 的手机/PC 立刻看到本设备 */
  {
    self->refreshLocalIp();  // 每次都用当前 IP（DHCP 可能晚于 start()）
    ssdpNotify(fd, self->localIp(), self->httpPort(), true);
  }
  long lastAliveMs = ssdpNowMs();

  char buf[2048];
  while (!self->stopFlag_) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int n = (int)recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &plen);
    if (n <= 0) {
      /* 超时分支：顺手维护 alive 周期（600s 重发一次，标准做法是 max-age/2 左右）
       * 以及 UI 侧要求补发的通告（WiFi 拿到 IP 后 LOCATION 才可用）。 */
      if (self->aliveReq_) {
        self->aliveReq_ = 0;
        self->refreshLocalIp();
        ssdpNotify(fd, self->localIp(), self->httpPort(), true);
        lastAliveMs = ssdpNowMs();
      } else if (ssdpNowMs() - lastAliveMs > 600000L) {
        self->refreshLocalIp();
        ssdpNotify(fd, self->localIp(), self->httpPort(), true);
        lastAliveMs = ssdpNowMs();
      }
      continue;
    }
    buf[n] = 0;
    /* 收到就记：SSDP 排查全靠它（是"没收到包"还是"收到了没应答"要一眼能分辨）。
     * ⚠️ 但**绝不能每包都打**：不少路由器以每秒几十条的频率发 SSDP 通告（NOTIFY），
     *    逐包 LOGD 既刷屏、又白烧 CPU 与串口——实测日志几秒就上百行，还会拖慢整机。
     *    所以分两类：M-SEARCH（有人搜我们，关键事件）逐条记；其余（NOTIFY 等）每 10s 汇总一条。 */
    bool isSearch = (strstr(buf, "M-SEARCH") != 0);
    if (isSearch) {
      char from[32];
      snprintf(from, sizeof(from), "%s", inet_ntoa(peer.sin_addr));
      LOGD("PgDlna: SSDP M-SEARCH %d 字节 from %s", n, from);
    } else {
      static int sOther = 0;
      static long long sNextLogMs = 0;
      static char sLastFrom[32] = {0};
      snprintf(sLastFrom, sizeof(sLastFrom), "%s", inet_ntoa(peer.sin_addr));
      ++sOther;
      long long now = ssdpNowMs();
      if (now >= sNextLogMs) {
        sNextLogMs = now + 10000;
        LOGD("PgDlna: SSDP 非搜索包 %d 条（近 10s，最近来自 %s）—— 通告刷屏已折叠",
             sOther, sLastFrom);
        sOther = 0;
      }
    }
    if (!isSearch) continue;
    // 只回 MediaRenderer 相关的搜索目标（ssdp:all / upnp:rootdevice / 本设备类型）
    const char *st = 0;
    char stbuf[128] = {0};
    /* ⚠️ 千万别用 `strstr(buf, "ST:")`：M-SEARCH 里先出现的是 `HOST: 239.255.255.250:1900`，
     *    而 "HOST:" 结尾正好是 "ST:" → 解析出来的是主机名，不是搜索目标。
     *    后果：所有 M-SEARCH 都判为"不受理"，设备在手机/PC 上**永远搜不到**
     *    （实测踩过：日志 `SSDP ST='239.255.255.250:1900' 是否受理=0`）。
     *    正确做法 = 按"行首"匹配 HTTP 头。 */
    for (const char *q = buf; q && *q;) {
      if ((q == buf || q[-1] == '\n') && strncmp(q, "ST:", 3) == 0) {
        q += 3;
        while (*q == ' ' || *q == '\t') q++;
        const char *e = strchr(q, '\r');
        if (!e) e = strchr(q, '\n');
        int m = e ? (int)(e - q) : (int)strlen(q);
        if (m > 0 && m < (int)sizeof(stbuf)) {
          memcpy(stbuf, q, m);
          stbuf[m] = 0;
          st = stbuf;
        }
        break;
      }
      const char *nl = strchr(q, '\n');
      if (!nl) break;
      q = nl + 1;
    }
    if (st) {
      bool want = strstr(st, "ssdp:all") || strstr(st, "upnp:rootdevice") ||
                  strstr(st, kDevType) || strstr(st, "MediaRenderer") ||
                  strstr(st, "AVTransport") || strstr(st, "RenderingControl") ||
                  strstr(st, "ConnectionManager");
      LOGD("PgDlna: SSDP ST='%s' 是否受理=%d", st, want ? 1 : 0);
      if (!want) continue;
    } else {
      st = kDevType;
    }
    /* `ssdp:all` = 要"你这台设备的所有东西" → 按规范逐个目标各回一条（见 kSsdpTargets 说明）。
     * 其余搜索目标原样回一条即可。 */
    if (strstr(st, "ssdp:all")) {
      for (int i = 0; i < kSsdpTargetCount; ++i)
        ssdpReply(fd, &peer, plen, self->localIp(), self->httpPort(), kSsdpTargets[i]);
    } else {
      ssdpReply(fd, &peer, plen, self->localIp(), self->httpPort(), st);
    }
  }
  /* 退出前发 byebye：让控制器立刻把本设备从列表里摘掉（否则要等 max-age 超时） */
  ssdpNotify(fd, self->localIp(), self->httpPort(), false);
  close(fd);
  return 0;
}

/* ---------- 下载：把 SetAVTransportURI 的 URL 拉到本地 ---------- */
/* 裸 socket 发 HTTP GET（不用 civetweb 客户端：实测它的 mg_get_response 对
 * busybox httpd 返回 "No data received"；自己发还顺便能支持 chunked）。
 * 支持: http:// 绝对地址、Content-Length 与 Transfer-Encoding: chunked。 */
/* ==================== 极简 TLS 通道（dlopen 系统 libssl/libcrypto）====================
 * 为什么自己搭这一层（而不是用现成的）：
 *   · **不用 civetweb 的客户端**：本板实测它连纯 http 都拿不到响应 ——
 *     `mg_printf` 返回 115（请求确实写出去了）后 `mg_get_response` 直接报
 *     "No data received"，`errno=EINPROGRESS`（非阻塞 connect 没处理干净）；
 *     `mg_download` 同样失败。而**裸 socket** 的 http 路径是本工程一直在用、实测通的。
 *   · **不用 openssl 组件包的静态库**：包里是 libcrypto.a(3.8MB)+libssl.a(0.7MB)，
 *     静态链进来会让 libzkgui.so 涨几 MB；而我们的 .so 是推到 `/tmp`（tmpfs = 内存）
 *     运行的，本板总共 56MB —— 这个代价不能接受。
 *   · 设备固件**本来就有** `/lib/libssl.so.1.1` + `/lib/libcrypto.so.1.1`
 *     （civetweb 自己也是 dlopen 它们），所以这里同样 dlopen，只取必需函数，体积代价为零。
 *
 * ==================== 证书校验（2026-09-13 加上） ====================
 * 校验成立要两个前提，缺一不可 —— 两个都补齐了才默认开启校验：
 *   ① **系统时间正确**：本板没有 RTC，开机恒为 1970，而证书有效期检查会让**任何**证书
 *      "尚未生效"（实测 `SSL_connect` 报 `certificate is not yet valid`）
 *      → 由 `pg::TimeSync`（NTP 组件包）负责把时间拉正；
 *   ② **CA 根证书**：随包放在 `resources/certs/cacert.pem`
 *      （打包后在 `/res/ui/certs/cacert.pem`，用 `CONFIGMANAGER->getResFilePath()` 定位）。
 *
 * 三档策略（QA：`dlna verify <0|1|2>`，见 pgDlnaSetTlsVerifyMode）：
 *   0 = 不校验（排障用，等于老行为）
 *   1 = **默认**：时间或 CA 任一不具备 → 明确告警并跳过；具备就校验，校验不过**回退一次**
 *       （不校验重连），保证媒体仍能播
 *   2 = 严格：时间/CA 不具备、或校验不通过 → 直接失败（专门用来验证 https 真被校验）
 */
namespace {

/* ---- 校验策略与状态（QA 可切、可查） ---- */
volatile int sTlsVerifyMode = 1;   // 0 不校验 / 1 有时间就校验 / 2 必须校验
volatile int sTlsLastVerify = -1;  // 最近一次 https：0 跳过 1 通过 2 失败

char sCaPath[192] = {0};
bool sCaPathInited = false;

/* ---- 请求头（QA 可改）----
 * 为什么需要：不少 CDN 的防盗链是"看请求头"的 —— 比如 B 站投屏下发的
 * `...mp4?ua=tvproj&upsig=...` 这类链接，是给**电视端客户端**拉的，
 * CDN 会同时校验 URL 里的签名和请求方的 UA/Referer。
 * 用默认的 "PocketGameDMR/1.0" 去拉，CDN 直接回 403（或 B 站私有码 959）。
 * 所以做成可配：`dlna ua <字符串>` / `dlna ref <字符串>`，排障时逐个试。 */
char sHttpUa[128] = "PocketGameDMR/1.0";
char sHttpReferer[192] = {0};
int sLastHttpCode = 0;
char sLastHttpErrBody[256] = {0};  // 非 200 时抓一小段响应体（CDN 的拒绝原因就写在这里）

/* CA 证书路径（懒加载一次）。找不到就返回空串并告警。 */
const char *caFilePath() {
  if (!sCaPathInited) {
    sCaPathInited = true;
    std::string p = CONFIGMANAGER->getResFilePath("certs/cacert.pem");
    snprintf(sCaPath, sizeof(sCaPath), "%s", p.c_str());
    if (access(sCaPath, R_OK) == 0) {
      LOGD("PgDlna: CA 证书就绪 %s（https 将校验证书）", sCaPath);
    } else {
      LOGW("PgDlna: 找不到 CA 证书 %s —— https 无法校验证书", sCaPath);
      sCaPath[0] = 0;
    }
  }
  return sCaPath;
}

struct SslApi {
  void *hSsl, *hCrypto;
  void *(*CTX_new)(const void *);
  const void *(*TLS_client_method)(void);
  void (*CTX_free)(void *);
  void *(*SSL_new)(void *);
  void (*SSL_free)(void *);
  int (*SSL_set_fd)(void *, int);
  int (*SSL_connect)(void *);
  int (*SSL_read)(void *, void *, int);
  int (*SSL_write)(void *, const void *, int);
  long (*SSL_ctrl)(void *, int, long, void *);
  /* 下面三个是证书校验用的 —— **允许缺失**（缺了就降级为不校验/只校验签发链） */
  void (*CTX_set_verify)(void *, int, void *);
  int (*CTX_load_verify_locations)(void *, const char *, const char *);
  int (*SSL_set1_host)(void *, const char *);
  /* ALPN（可选）：**只声明 http/1.1**。有些服务端（Google/部分 Cloudflare 站点）
   * 会因为 ClientHello 里没有 ALPN 直接 handshake_failure；但绝不能声明 h2 ——
   * 一旦协商成 h2，我们发出去的 HTTP/1.1 文本会被当成二进制帧，全乱。 */
  int (*CTX_set_alpn_protos)(void *, const unsigned char *, unsigned int);
  /* 读出错时区分"超时(可重试)"与"真错误"要用它（可选） */
  int (*SSL_get_error)(const void *, int);
  unsigned long (*ERR_get_error)(void);
  void (*ERR_error_string_n)(unsigned long, char *, unsigned long);
  bool ok;
  SslApi()
      : hSsl(0), hCrypto(0), CTX_new(0), TLS_client_method(0), CTX_free(0), SSL_new(0),
        SSL_free(0), SSL_set_fd(0), SSL_connect(0), SSL_read(0), SSL_write(0),
        SSL_ctrl(0), CTX_set_verify(0), CTX_load_verify_locations(0), SSL_set1_host(0),
        CTX_set_alpn_protos(0), SSL_get_error(0), ERR_get_error(0),
        ERR_error_string_n(0), ok(false) {}
};

void *pgDl(const char *const *names, int n) {
  for (int i = 0; i < n; ++i) {
    void *h = dlopen(names[i], RTLD_NOW);
    if (h) return h;
  }
  return 0;
}

SslApi &sslApi() {
  static SslApi a;
  static bool tried = false;
  if (tried) return a;
  tried = true;
  static const char *kSslNames[] = {"libssl.so.1.1", "libssl.so"};
  static const char *kCryptoNames[] = {"libcrypto.so.1.1", "libcrypto.so"};
  a.hSsl = pgDl(kSslNames, 2);
  a.hCrypto = pgDl(kCryptoNames, 2);
  if (!a.hSsl || !a.hCrypto) {
    LOGW("PgDlna: 打不开 libssl/libcrypto（https 不可用）");
    return a;
  }
#define PG_LOAD(field, sym)                                          \
  do {                                                               \
    *(void **)(&a.field) = dlsym(a.hSsl, sym);                       \
    if (!a.field) { LOGW("PgDlna: libssl 缺少 %s", sym); return a; } \
  } while (0)
  PG_LOAD(CTX_new, "SSL_CTX_new");
  PG_LOAD(TLS_client_method, "TLS_client_method");
  PG_LOAD(CTX_free, "SSL_CTX_free");
  PG_LOAD(SSL_new, "SSL_new");
  PG_LOAD(SSL_free, "SSL_free");
  PG_LOAD(SSL_set_fd, "SSL_set_fd");
  PG_LOAD(SSL_connect, "SSL_connect");
  PG_LOAD(SSL_read, "SSL_read");
  PG_LOAD(SSL_write, "SSL_write");
  PG_LOAD(SSL_ctrl, "SSL_ctrl");
#undef PG_LOAD
  /* 校验相关：有就用，没有就降级（不当失败处理） */
  *(void **)(&a.CTX_set_verify) = dlsym(a.hSsl, "SSL_CTX_set_verify");
  *(void **)(&a.CTX_load_verify_locations) =
      dlsym(a.hSsl, "SSL_CTX_load_verify_locations");
  *(void **)(&a.SSL_set1_host) = dlsym(a.hSsl, "SSL_set1_host");
  *(void **)(&a.CTX_set_alpn_protos) = dlsym(a.hSsl, "SSL_CTX_set_alpn_protos");
  *(void **)(&a.SSL_get_error) = dlsym(a.hSsl, "SSL_get_error");
  *(void **)(&a.ERR_get_error) = dlsym(a.hCrypto, "ERR_get_error");
  *(void **)(&a.ERR_error_string_n) = dlsym(a.hCrypto, "ERR_error_string_n");
  a.ok = true;
  LOGD("PgDlna: TLS 通道就绪（dlopen 系统 libssl/libcrypto；证书校验能力=%s）",
       (a.CTX_set_verify && a.CTX_load_verify_locations) ? "有" : "无");
  return a;
}

/* OpenSSL 里 SSL_set_tlsext_host_name 是个宏：
 *   SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME=55, TLSEXT_NAMETYPE_host_name=0, name)
 * 这里手写它的两个常量（tls1.h 定义，1.1.1 起未变）。
 * ⚠️ 不设 SNI 的话，多数 CDN 会回默认站点证书/直接断开 → 握手失败。 */
const int kSslCtrlSetTlsextHostName = 55;
const long kTlsextNametypeHostName = 0;
const int kSslVerifyPeer = 1;  // SSL_VERIFY_PEER

void sslLogErr(const char *what) {
  SslApi &a = sslApi();
  if (!a.ERR_get_error || !a.ERR_error_string_n) return;
  for (int i = 0; i < 3; ++i) {
    unsigned long e = a.ERR_get_error();
    if (!e) break;
    char buf[256] = {0};
    a.ERR_error_string_n(e, buf, sizeof(buf));
    LOGW("PgDlna: TLS %s 错误[%d]: %s", what, i, buf);
  }
}

}  // namespace

int pgDlnaTlsVerifyMode() { return sTlsVerifyMode; }

void pgDlnaSetHttpUa(const char *ua) {
  if (!ua || !ua[0]) {
    LOGD("PgDlna: 当前 UA='%s'", sHttpUa);
    return;
  }
  snprintf(sHttpUa, sizeof(sHttpUa), "%s", ua);
  LOGD("PgDlna: 请求 UA 已设为 '%s'", sHttpUa);
}
void pgDlnaSetHttpReferer(const char *r) {
  if (!r) {
    LOGD("PgDlna: 当前 Referer='%s'", sHttpReferer[0] ? sHttpReferer : "(空)");
    return;
  }
  snprintf(sHttpReferer, sizeof(sHttpReferer), "%s", r);
  LOGD("PgDlna: 请求 Referer 已设为 '%s'", sHttpReferer[0] ? sHttpReferer : "(空)");
}

void pgDlnaSetTlsVerifyMode(int m) {
  if (m < 0 || m > 2) m = 1;
  sTlsVerifyMode = m;
  sTlsLastVerify = -1;
  LOGD("PgDlna: 证书校验策略 -> %d（0 不校验 / 1 有时间就校验 / 2 必须校验）", m);
}

/* TLS 自检（QA 命令 `dlna tlscheck` 调）：本工程的 TLS 是**运行时 dlopen**
 * "libssl.so"/"libcrypto.so" 实现的，所以 https 失败的第一步排查就是
 * "两个 so 能不能打开 + 校验符号在不在 + CA 文件与系统时间就绪没"。 */
void pgDlnaTlsDiag() {
  const char *names[] = {"libssl.so", "libssl.so.1.1", "libcrypto.so",
                         "libcrypto.so.1.1"};
  for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i) {
    void *h = dlopen(names[i], RTLD_NOW);
    if (h) {
      LOGD("PgDlna: dlopen(%s) -> OK", names[i]);
      dlclose(h);
    } else {
      const char *e = dlerror();
      LOGD("PgDlna: dlopen(%s) -> 失败: %s", names[i], e ? e : "未知");
    }
  }
  SslApi &a = sslApi();
  LOGD("PgDlna: 校验符号 SSL_CTX_set_verify=%s SSL_CTX_load_verify_locations=%s SSL_set1_host=%s",
       a.CTX_set_verify ? "有" : "无", a.CTX_load_verify_locations ? "有" : "无",
       a.SSL_set1_host ? "有" : "无");
  const char *ca = caFilePath();
  LOGD("PgDlna: CA 证书 %s（%s）", ca[0] ? ca : "(没有)",
       ca[0] ? "就绪" : "缺失 → https 无法校验");
  LOGD("PgDlna: 系统时间%s（校时状态 %s，服务器 %s）",
       pg::TimeSync::timeValid() ? "有效" : "无效——1970 附近",
       pg::TimeSync::stateText(), pg::TimeSync::lastServer());
  LOGD("PgDlna: 校验策略=%d 最近一次 https 校验结果=%d（0 跳过 1 通过 2 失败）",
       sTlsVerifyMode, sTlsLastVerify);
  LOGD("PgDlna: 请求头 UA='%s' Referer='%s'", sHttpUa, sHttpReferer[0] ? sHttpReferer : "(空)");
  if (sLastHttpCode && sLastHttpCode != 200) {
    LOGD("PgDlna: 最近一次 HTTP %d，应答体：%s", sLastHttpCode, sLastHttpErrBody);
  }
}

/* DNS 自检（QA：`dlna dns <host>`）。
 * 为什么要单独一个：本板**没有 /etc/resolv.conf**（根分区是只读 squashfs），
 * 但应用里的 gethostbyname 确实能解析 —— 所以要能把"到底解析到哪个 IP"打出来，
 * 才能区分"解析错/被劫持"和"这个 IP 连不上"。 */
void pgDlnaDnsCheck(const char *host) {
  if (!host || !host[0]) {
    LOGD("PgDlna: 用法 dlna dns <域名>");
    return;
  }
  struct in_addr a;
  if (inet_aton(host, &a)) {
    LOGD("PgDlna: dns %s -> 本来就是 IP：%s", host, inet_ntoa(a));
    return;
  }
  struct hostent *he = gethostbyname(host);
  if (!he || !he->h_addr_list[0]) {
    LOGD("PgDlna: dns %s -> 解析失败（h_errno=%d）", host, h_errno);
    return;
  }
  int n = 0;
  while (he->h_addr_list[n] && n < 8) ++n;
  char buf[128] = {0};
  for (int i = 0; i < n; ++i) {
    struct in_addr x;
    memcpy(&x, he->h_addr_list[i], 4);
    strncat(buf, inet_ntoa(x), sizeof(buf) - strlen(buf) - 2);
    if (i + 1 < n) strncat(buf, ",", sizeof(buf) - strlen(buf) - 2);
  }
  LOGD("PgDlna: dns %s -> %s（共 %d 个 A 记录）", host, buf, n);
}

/* 统一的收发：明文走 send/recv，TLS 走 SSL_write/SSL_read（ssl 为 0 表示明文） */
static int pgIoWrite(int fd, void *ssl, const void *buf, int len) {
  if (ssl) return sslApi().SSL_write(ssl, buf, len);
  return (int)send(fd, buf, len, 0);
}
static int pgIoRead(int fd, void *ssl, void *buf, int len) {
  if (ssl) return sslApi().SSL_read(ssl, buf, len);
  return (int)recv(fd, buf, len, 0);
}

/* 读返回 <0 时：判断是"暂时没数据(可重试)"还是"真错误"。
 * ⚠️ 这条很关键：本 socket 设了 `SO_RCVTIMEO=10s`，**超时会让 recv 返回 -1/EAGAIN**。
 *    老代码把 `n<=0` 一律当"结束" → 下载中途网络抖一下/慢一下就被判"下载失败"
 *    （实测：5.5MB 的 https 视频下到 **786432 字节** 就报"未收满 Content-Length"）。
 *    正确做法：EAGAIN/EWOULDBLOCK/EINTR（TLS 下是 SSL_ERROR_WANT_READ/WRITE）→ 继续等；
 *    只有 0（对端正常关闭）才是结束，其它 errno 才算真错误。 */
static bool pgIoRetryable(void *ssl) {
  if (ssl) {
    SslApi &a = sslApi();
    if (a.SSL_get_error) {
      int e = a.SSL_get_error(ssl, -1);
      /* 2 = SSL_ERROR_WANT_READ，3 = SSL_ERROR_WANT_WRITE —— 都是"再来一次" */
      return e == 2 || e == 3;
    }
    return true;  // 拿不到 SSL_get_error 时乐观重试（反正有次数上限）
  }
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* 一次连接（+ 可选 TLS 握手）。
 * 返回 0 = 失败；1 = 成功；2 = TLS 成功但**证书校验没过**（调用方按策略决定回退/失败）。
 * out->verified 表示本次是否真的做了校验。失败路径都会自己释放连接资源。 */
struct HttpConn {
  int fd;
  void *ssl;
  void *ctx;
  bool verified;
};

static void pgCloseConn(HttpConn *c) {
  if (c->ssl) sslApi().SSL_free(c->ssl);
  if (c->ctx) sslApi().CTX_free(c->ctx);
  if (c->fd >= 0) close(c->fd);
  c->ssl = 0;
  c->ctx = 0;
  c->fd = -1;
}

static int pgOpenConn(const char *host, int port, bool tls, bool verify, HttpConn *out) {
  out->fd = -1;
  out->ssl = 0;
  out->ctx = 0;
  out->verified = false;

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = inet_addr(host);
  if (sa.sin_addr.s_addr == INADDR_NONE) {
    struct hostent *he = gethostbyname(host);
    if (!he || !he->h_addr_list[0]) {
      LOGD("PgDlna: 域名解析失败 %s", host);
      return 0;
    }
    memcpy(&sa.sin_addr, he->h_addr_list[0], 4);
    LOGD("PgDlna: %s 解析到 %s（gethostbyname）", host, inet_ntoa(sa.sin_addr));
  } else {
    LOGD("PgDlna: %s 是字面 IP", host);
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct timeval tv;
  tv.tv_sec = 20;  // 慢速链路上 10s 容易误判成"结束"（见 pgIoRetryable 的说明）
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  /* 连接：非阻塞 connect + select 等就绪。
   * ⚠️ **绝不能对同一个 socket 反复调 connect()**（老代码就是这么写的，实测踩到）：
   *    第二次会返回 `EALREADY (Operation already in progress)`，被当成失败 →
   *    "重试 3 次"实际全废，直接报错。症状很有误导性：**只有部分站点**连不上
   *    （DNS/握手快的站点第一次就成功，慢的站点才会走到第二轮），
   *    实测 `https://www.youtube.com/` 就是被这条坑掉的：
   *    `connect www.youtube.com:443 失败 Operation already in progress`。
   *    正确做法：connect 拿到 EINPROGRESS 后用 select **反复等**，等到就查 SO_ERROR。 */
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int cr = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  if (cr == 0) {
    // 立即成功（本机/同网段场景很常见）
  } else if (errno == EINPROGRESS) {
    cr = -1;
    for (int i = 0; i < 3; ++i) {  // 最多等 3×5s
      fd_set wf;
      FD_ZERO(&wf);
      FD_SET(fd, &wf);
      struct timeval wt;
      wt.tv_sec = 5;
      wt.tv_usec = 0;
      int sr = select(fd + 1, 0, &wf, 0, &wt);
      if (sr <= 0) continue;  // 超时 → 继续等，**不要重连**
      int soerr = 0;
      socklen_t sl = sizeof(soerr);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
      if (soerr == 0) cr = 0;
      else LOGD("PgDlna: connect %s:%d 失败 %s", host, port, strerror(soerr));
      break;
    }
  }
  fcntl(fd, F_SETFL, flags);  // 恢复阻塞（后续用 SO_RCVTIMEO 控制）
  if (cr < 0) {
    LOGD("PgDlna: connect %s:%d 未成功（errno=%d %s）", host, port, errno,
         strerror(errno));
    close(fd);
    return 0;
  }
  if (!tls) {
    out->fd = fd;
    return 1;
  }

  SslApi &a = sslApi();
  if (!a.ok) {
    close(fd);
    return 0;
  }
  void *ctx = a.CTX_new(a.TLS_client_method());
  if (!ctx) {
    sslLogErr("SSL_CTX_new");
    close(fd);
    return 0;
  }
  if (a.CTX_set_alpn_protos) {
    static const unsigned char kAlpn[] = "\x08http/1.1";  // 只报 http/1.1
    a.CTX_set_alpn_protos(ctx, kAlpn, sizeof(kAlpn) - 1);
  }
  bool doingVerify = false;
  if (verify) {
    if (a.CTX_set_verify && a.CTX_load_verify_locations) {
      a.CTX_set_verify(ctx, kSslVerifyPeer, 0);
      if (a.CTX_load_verify_locations(ctx, caFilePath(), 0) == 1) {
        doingVerify = true;
      } else {
        sslLogErr("SSL_CTX_load_verify_locations");
        LOGW("PgDlna: 加载 CA 失败（%s），本次不校验", caFilePath());
      }
    } else {
      LOGW("PgDlna: libssl 没有证书校验接口，本次不校验");
    }
  }
  void *ssl = a.SSL_new(ctx);
  if (!ssl) {
    sslLogErr("SSL_new");
    a.CTX_free(ctx);
    close(fd);
    return 0;
  }
  a.SSL_set_fd(ssl, fd);
  /* SNI 一定要设（不设多数 CDN 会给默认站点证书或直接断连） */
  long sniRet = a.SSL_ctrl(ssl, kSslCtrlSetTlsextHostName, kTlsextNametypeHostName,
                           (void *)host);
  if (sniRet != 1) LOGW("PgDlna: 设置 SNI('%s') 返回 %ld（!=1 表示没设上）", host, sniRet);
  if (doingVerify && a.SSL_set1_host) {
    a.SSL_set1_host(ssl, host);  // 主机名校验：防"链合法但证书签给别的域名"
  }

  out->fd = fd;
  out->ssl = ssl;
  out->ctx = ctx;
  out->verified = doingVerify;
  if (a.SSL_connect(ssl) != 1) {
    sslLogErr("SSL_connect");
    int r;
    if (doingVerify) {
      LOGW("PgDlna: %s TLS 握手/证书校验未通过", host);
      r = 2;
    } else {
      LOGD("PgDlna: %s TLS 握手失败", host);
      r = 0;
    }
    pgCloseConn(out);
    out->verified = doingVerify;  // pgCloseConn 不碰这个字段，这里再确认一次
    return r;
  }
  LOGD("PgDlna: TLS 握手成功 %s（%s）", host,
       doingVerify ? "证书校验通过" : "未校验证书");
  return 1;
}


/* 一条 HTTP/HTTPS GET，写到文件 f。返回 0 成功；-10 = 需要跟 outLoc 重定向；
 * -3 连接失败；-7 非 200；-9 超过体积上限；-1/-2/-5/-6/-8 其它错误。 */
static int httpGetToFile(const char *host, int port, bool tls, const char *path, FILE *f,
                         volatile int *stopFlag, volatile int *pct,
                         long long *outTotal, long long maxBytes,
                         char *outLoc, int locN) {
  /* ---- 先决定"要不要校验证书"（三档策略见 pgDlnaSetTlsVerifyMode）---- */
  bool wantVerify = false;
  if (tls) {
    if (sTlsVerifyMode == 0) {
      LOGD("PgDlna: https 按策略 0 不校验证书");
    } else if (!pg::TimeSync::timeValid()) {
      LOGW("PgDlna: 系统时间还没校准（校时状态 %s）—— 证书必然\"尚未生效\"，本次跳过校验",
           pg::TimeSync::stateText());
      if (sTlsVerifyMode == 2) return -3;  // 严格模式：宁可失败也不偷偷放行
    } else if (!caFilePath()[0]) {
      LOGW("PgDlna: 没有 CA 证书文件，本次跳过校验");
      if (sTlsVerifyMode == 2) return -3;
    } else {
      wantVerify = true;  // 时间 + CA 齐了 —— 校验
    }
  }

  HttpConn cn;
  int orc = pgOpenConn(host, port, tls, wantVerify, &cn);
  bool fellBack = false;
  if (orc == 2 && sTlsVerifyMode == 1) {
    /* 默认策略：校验没过就**回退一次**（不校验重连）—— 宁可放行也别让用户看不了片子，
     * 但日志会明确记下"这次是没校验的"，别让风险无声无息。 */
    LOGW("PgDlna: 证书校验未通过 —— 按策略 1 回退到不校验重连（存在中间人风险）");
    fellBack = true;
    orc = pgOpenConn(host, port, tls, false, &cn);
  }
  if (orc == 2) {
    sTlsLastVerify = 2;
    LOGW("PgDlna: 证书校验未通过，策略 2（严格）下拒绝下载");
    return -3;
  }
  if (orc == 0) {
    if (tls) sTlsLastVerify = 0;
    return -3;
  }
  if (tls) sTlsLastVerify = fellBack ? 2 : (cn.verified ? 1 : 0);
  int fd = cn.fd;
  void *ssl = cn.ssl;
  void *ctx = cn.ctx;
  (void)ctx;


  /* 请求（分段拼，避免一次性 snprintf 里条件字段写错 —— 这里被 CRLF 坑过一次） */
  char req[3072];  // URI 可能很长（带签名参数），给小了请求行会被截断
  int rl = snprintf(req, sizeof(req),
                    "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n",
                    path, host, sHttpUa);
  if (rl > 0 && rl < (int)sizeof(req) && sHttpReferer[0]) {
    rl += snprintf(req + rl, sizeof(req) - rl, "Referer: %s\r\n", sHttpReferer);
  }
  if (rl > 0 && rl < (int)sizeof(req)) {
    rl += snprintf(req + rl, sizeof(req) - rl, "Connection: close\r\n\r\n");
  }
  LOGD("PgDlna: 请求头 UA='%s' Referer='%s'（%d 字节）", sHttpUa,
       sHttpReferer[0] ? sHttpReferer : "", rl);
  int wn = pgIoWrite(fd, ssl, req, rl);
  if (wn != rl) {
    LOGD("PgDlna: 请求发送失败 (%d/%d)", wn, rl);
    pgCloseConn(&cn);
    return -4;
  }

  /* 读响应头（逐字节到 \r\n\r\n） */
  char hdr[2048];
  int hn = 0;
  char hb;
  while (hn < (int)sizeof(hdr) - 1) {
    int n = pgIoRead(fd, ssl, &hb, 1);
    if (n <= 0) break;
    hdr[hn++] = hb;
    if (hn >= 4 && strncmp(hdr + hn - 4, "\r\n\r\n", 4) == 0) break;
  }
  hdr[hn] = 0;
  if (hn < 12) {
    LOGD("PgDlna: 没有收到响应头（收到 %d 字节）", hn);
    pgCloseConn(&cn);
    return -5;
  }
  int code = 0;
  if (sscanf(hdr, "HTTP/%*d.%*d %d", &code) != 1) {
    LOGD("PgDlna: 响应头解析失败: %.40s", hdr);
    pgCloseConn(&cn);
    return -6;
  }
  if (code != 200) {
    sLastHttpCode = code;
    /* 抓一小段响应体：CDN 的拒绝原因（签名错/过期/IP 不符）通常就写在 body 里，
     * 没有它就只能在"403/959"这种错误码上瞎猜。 */
    {
      int got = 0;
      char b2[257];
      int n2;
      while (got < 256 && (n2 = pgIoRead(fd, ssl, b2 + got, 256 - got)) > 0) got += n2;
      b2[got] = 0;
      for (int i = 0; i < got; ++i) {
        if (b2[i] == '\r' || b2[i] == '\n' || b2[i] == '\t') b2[i] = ' ';
      }
      snprintf(sLastHttpErrBody, sizeof(sLastHttpErrBody), "%s", b2);
      LOGD("PgDlna: HTTP %d（服务器应答体前 %d 字节）：%s", code, got, b2);
    }
    /* 3xx：把 Location 交回调用方（最多跟 3 跳，绝对/相对都支持） */
    if (code >= 300 && code < 400 && outLoc && locN > 0) {
      const char *loc = strstr(hdr, "\nLocation:");
      if (!loc) loc = strstr(hdr, "\nlocation:");
      if (loc) {
        loc += 10;
        while (*loc == ' ') loc++;
        const char *e = strchr(loc, '\r');
        if (!e) e = strchr(loc, '\n');
        int n = e ? (int)(e - loc) : (int)strlen(loc);
        if (n > 0 && n < locN) {
          memcpy(outLoc, loc, n);
          outLoc[n] = 0;
          pgCloseConn(&cn);
          return -10;
        }
      }
    }
    pgCloseConn(&cn);
    return -7;
  }
  bool chunked = strstr(hdr, "Transfer-Encoding: chunked") != 0 ||
                 strstr(hdr, "transfer-encoding: chunked") != 0;
  long long clen = -1;
  const char *cl = strstr(hdr, "Content-Length:");
  if (!cl) cl = strstr(hdr, "content-length:");
  if (cl) clen = atoll(cl + 15);
  LOGD("PgDlna: HTTP 200, Content-Length=%lld chunked=%d（%s）", clen, chunked ? 1 : 0,
       tls ? "TLS" : "明文");
  /* 空间守卫：本板 /tmp 是 tmpfs（吃了就是内存），必须先看总长再决定下不下。
   * 宁可不放，也不能把整机内存吃光（实测：无视长度直接下 → 播放中进程被杀）。 */
  if (maxBytes > 0 && clen > 0 && clen > maxBytes) {
    LOGD("PgDlna: 拒绝下载：源 %lld 字节 > 本机可用上限 %lld 字节（/tmp 是 tmpfs，占内存）",
         clen, maxBytes);
    pgCloseConn(&cn);
    return -9;
  }

  static const int BS = 8192;
  char *buf = (char *)malloc(BS);
  if (!buf) {
    pgCloseConn(&cn);
    return -2;
  }
  long long total = 0;
  int ret = 0;
  auto writeOut = [&](const char *p, int len) -> bool {
    if (maxBytes > 0 && total + len > maxBytes) {
      LOGD("PgDlna: 超过上限 %lld 字节，中止下载", maxBytes);
      return false;
    }
    if (fwrite(p, 1, len, f) != (size_t)len) {
      LOGD("PgDlna: 写盘失败（空间不足?）");
      return false;
    }
    total += len;
    if (clen > 0) *pct = (int)(total * 100 / clen);
    return true;
  };

  if (chunked) {
    /* 逐块读：先读一行 "size[;ext]"，再读 size 字节 + CRLF，size==0 结束 */
    char line[64];
    while (!*stopFlag) {
      int li = 0;
      while (li < (int)sizeof(line) - 1) {
        if (pgIoRead(fd, ssl, line + li, 1) <= 0) break;
        if (li >= 1 && line[li] == '\n' && line[li - 1] == '\r') break;
        li++;
      }
      line[li] = 0;
      long sz = strtol(line, 0, 16);
      if (sz <= 0) break;
      int left = (int)sz;
      int stall = 0;
      while (left > 0) {
        int want = left < BS ? left : BS;
        int n = pgIoRead(fd, ssl, buf, want);
        if (n < 0) {
          if (pgIoRetryable(ssl) && ++stall <= 3) continue;  // 超时 → 继续等
          LOGD("PgDlna: 分块读失败 errno=%d（已收 %lld 字节）", errno, total);
          ret = -8;
          goto done;
        }
        if (n == 0) { LOGD("PgDlna: 分块读到 EOF（已收 %lld 字节）", total); ret = -8; goto done; }
        stall = 0;
        if (!writeOut(buf, n)) { ret = -9; goto done; }
        left -= n;
      }
      pgIoRead(fd, ssl, buf, 2);  // 吃掉块尾 CRLF
    }
  } else {
    int stall = 0;
    while (!*stopFlag) {
      if (clen > 0 && total >= clen) break;
      int want = BS;
      if (clen > 0) {
        long long left = clen - total;
        if (left < BS) want = (int)left;
      }
      int n = pgIoRead(fd, ssl, buf, want);
      if (n < 0) {
        if (pgIoRetryable(ssl) && ++stall <= 3) {
          LOGD("PgDlna: 读暂时没数据（第 %d 次，已收 %lld/%lld 字节），继续等", stall,
               total, clen);
          continue;
        }
        LOGD("PgDlna: 读失败 errno=%d %s（已收 %lld 字节）", errno, strerror(errno), total);
        ret = -8;
        break;
      }
      if (n == 0) {
        LOGD("PgDlna: 对端关闭连接（已收 %lld/%lld 字节）", total, clen);
        break;
      }
      stall = 0;
      if (!writeOut(buf, n)) { ret = -9; goto done; }
    }
    if (clen > 0 && total < clen) {
      LOGD("PgDlna: 未收满 Content-Length（%lld/%lld）", total, clen);
      ret = -8;
    }
  }
done:
  free(buf);
  pgCloseConn(&cn);
  *outTotal = total;
  return ret;
}
/* ==================== 落盘目录与空间（下载用） ==================== */
#define PG_TMPFS_MAGIC 0x01021994L

static bool pgIsTmpfs(const char *dir) {
  struct statfs st;
  if (statfs(dir, &st) != 0) return false;
  return (long)st.f_type == PG_TMPFS_MAGIC;
}
static bool pgIsDirWritable(const char *dir) {
  return dir && access(dir, W_OK) == 0;
}
static long long pgFreeBytes(const char *dir) {
  struct statfs st;
  if (statfs(dir, &st) != 0) return -1;
  return (long long)st.f_bavail * (long long)st.f_bsize;
}

/* 选落盘目录：优先"非 tmpfs 的真实存储"（外置卡/U 盘/其它分区），
 * 都没有才退回 tmpfs（本板就是 /tmp，等于吃内存）。返回 true = 真实存储。 */
static bool pgPickSpoolDir(char *out, int n, long long needBytes) {
  static const char *cands[] = {"/mnt/extsd", "/mnt/sdnand", "/mnt/usb1", "/mnt/usb2",
                                "/mnt/sdotg",  "/mnt/udisk",  "/data",     "/tmp"};
  const char *fallbackTmpfs = 0;
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); ++i) {
    long long freeB = pgFreeBytes(cands[i]);
    if (freeB < 0 || !pgIsDirWritable(cands[i])) continue;
    if (freeB < needBytes) continue;
    bool tmp = pgIsTmpfs(cands[i]);
    if (tmp) {
      if (!fallbackTmpfs) fallbackTmpfs = cands[i];
      continue;  // tmpfs 只当兜底
    }
    snprintf(out, n, "%s", cands[i]);
    LOGD("PgDlna: 落盘目录 %s（可用 %lld 字节，非 tmpfs）", out, freeB);
    return true;
  }
  if (!fallbackTmpfs) return false;
  long long freeB = pgFreeBytes(fallbackTmpfs);
  snprintf(out, n, "%s", fallbackTmpfs);
  LOGD("PgDlna: 落盘目录 %s（tmpfs，可用 %lld 字节 ⚠️ 占内存）", out, freeB);
  return false;
}

static bool pgSplitHttpUrl(const char *url, char *host, int hn, int *port, char *path,
                           int pn, bool *tls) {
  if (!url) return false;
  if (strncmp(url, "https://", 8) == 0) {
    *tls = true;
    *port = 443;
  } else if (strncmp(url, "http://", 7) == 0) {
    *tls = false;
    *port = 80;
  } else {
    return false;
  }
  const char *p = strstr(url, "://") + 3;
  const char *slash = strchr(p, '/');
  if (!slash) return false;
  int hl = (int)(slash - p);
  if (hl <= 0 || hl >= hn) return false;
  memcpy(host, p, hl);
  host[hl] = 0;
  char *colon = strchr(host, ':');
  if (colon) {
    *port = atoi(colon + 1);
    *colon = 0;
  }
  snprintf(path, pn, "%s", slash);
  return true;
}

/* 删除已下载的媒体文件（本板 /tmp 是 tmpfs，文件就是内存；停播/播完就该删） */
void Dlna::cleanupMedia() {
  if (mediaPath_[0]) {
    if (unlink(mediaPath_) == 0) LOGD("PgDlna: 已删除 %s（释放空间）", mediaPath_);
    mediaPath_[0] = 0;
  }
}

int Dlna::downloadTo(const char *uri) {
  if (!uri || (strncmp(uri, "http://", 7) != 0 && strncmp(uri, "https://", 8) != 0)) {
    LOGD("PgDlna: 只支持 http:// 或 https:// 源（拿到 '%s'）", uri ? uri : "");
    return -1;
  }
  /* 先按原 URI 定落盘文件名（扩展名影响播放器判定，尽量取对）。
   * ⚠️ 扩展名要从"最后一个点"截到 **'?' 之前**：旧代码用 strlen(dot) 判断，
   *    带查询串时（几乎所有的 CDN 链接）长度必超 ext[] → 一律退回 .mp4（踩过）。 */
  char ext[8] = ".mp4";
  {
    const char *scheme = strstr(uri, "://");
    const char *p = scheme ? scheme + 3 : uri;  // 跳过 http:// 或 https://
    const char *slash = strchr(p, '/');
    if (!slash) return -1;
    const char *dot = strrchr(slash, '.');
    const char *q = strchr(slash, '?');
    if (q && dot && dot > q) dot = 0;         // 点出现在查询串里 → 不算扩展名
    if (dot) {
      int n = 0;
      long el = (long)((q ? q : uri + strlen(uri)) - dot);
      if (el > 0 && el < (long)sizeof(ext)) {
        for (n = 0; n < (int)el; ++n) ext[n] = dot[n];
        ext[n] = 0;
      }
    }
  }

  cleanupMedia();  // 先清旧文件：本板 /tmp 就是内存，不能留两份
  char dir[160];
  char name[64];
  snprintf(name, sizeof(name), "dlna_media%s", ext);
  /* 先按"够不够放"选目录：不知道大小时给一个保守需求（4MB）探路，真正的
   * Content-Length 校验在 httpGetToFile 里做（拿到响应头才知道总长）。 */
  bool realStorage = pgPickSpoolDir(dir, sizeof(dir), 4LL * 1024 * 1024);
  snprintf(mediaPath_, sizeof(mediaPath_), "%s/%s", dir, name);

  long long room = pgFreeBytes(dir);
  long long maxBytes = (room > 0) ? (room - room / 8) : -1;  // 留 12.5% 余量
  if (!realStorage) {
    // tmpfs：既受可用空间限制，也受内存上限保护
    long long cap = PG_DLNA_TMPFS_MAX_BYTES;
    if (maxBytes < 0 || cap < maxBytes) maxBytes = cap;
  }

  /* 下载循环：最多跟 3 跳重定向（见 httpGetToFile 里的 -10） */
  char curUrl[2048];
  snprintf(curUrl, sizeof(curUrl), "%s", uri);
  long long total = 0;
  int r = -1;
  for (int hop = 0; hop <= 3; ++hop) {
    char host[128];
    int port = 80;
    bool tls = false;
    char path[1600];
    if (!pgSplitHttpUrl(curUrl, host, sizeof(host), &port, path, sizeof(path), &tls)) {
      LOGD("PgDlna: URL 解析失败 '%s'", curUrl);
      return -1;
    }
    LOGD("PgDlna: 下载 %s -> %s (可用 %lld 字节, 上限 %lld)", curUrl, mediaPath_, room,
         maxBytes);
    FILE *f = fopen(mediaPath_, "wb");  // 每跳重开：重定向后必须从 0 写
    if (!f) {
      LOGD("PgDlna: 打不开 %s: %s", mediaPath_, strerror(errno));
      return -5;
    }
    downloadPercent_ = 0;
    total = 0;
    char loc[1600] = {0};
    r = httpGetToFile(host, port, tls, path, f, &stopFlag_, &downloadPercent_, &total,
                      maxBytes, loc, sizeof(loc));
    fclose(f);
    if (r != -10) break;
    /* 重定向：Location 可能是绝对 URL（http/https 都行），也可能是以 '/' 开头的相对路径 */
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
      snprintf(curUrl, sizeof(curUrl), "%s", loc);
    } else if (loc[0] == '/') {
      snprintf(curUrl, sizeof(curUrl), "%s://%s:%d%s", tls ? "https" : "http", host, port,
               loc);
    } else {
      LOGD("PgDlna: 不支持的 Location '%s'", loc);
      r = -7;
      break;
    }
    LOGD("PgDlna: 跟随重定向 -> %s", curUrl);
  }

  if (r != 0) {
    LOGD("PgDlna: 下载失败 r=%d（已收 %lld 字节）", r, total);
    unlink(mediaPath_);
    mediaPath_[0] = 0;
    return r;
  }
  downloadPercent_ = 100;
  LOGD("PgDlna: 下载完成 %lld 字节 -> %s", total, mediaPath_);
  return 0;
}

void *pgDlnaDownloadThread(void *arg) {
  Dlna *self = (Dlna *)arg;
  char uri[2048];  // ⚠️ 别给小了：CDN 链接带签名参数轻松 500+ 字符，截断就下不下来
  snprintf(uri, sizeof(uri), "%s", self->lastUri_);
  int r = self->downloadTo(uri);
  if (r == 0) {
    self->pushAction(DlnaAction::PLAY, self->mediaPath_, 0);
  } else {
    self->setTransportState("STOPPED");
    self->downloadPercent_ = -1;
  }
  self->dlBusy_ = 0;
  return 0;
}

void Dlna::onSetUri(const char *uri) {
  snprintf(lastUri_, sizeof(lastUri_), "%s", uri ? uri : "");
  LOGD("PgDlna: SetAVTransportURI '%s'", lastUri_);
  setTransportState("TRANSITIONING");
  /* ⚠️ **在线流（http/https）不下载**：直接把 URL 作为"待播路径"投给 UI 线程，
   *    由 PgStream（ffmpeg 解封装 + 设备硬解 + VO 上屏）在线播。
   *    为什么不能下：本板 /tmp 是 tmpfs（就是内存），整片下载必然 OOM ——
   *    实测一部 3.9Mbps 的片子下到一半就把 MemFree 从 20MB 吃到 7MB。
   *    本地文件（file:// 或 /路径）才走下面原来的"下载/直接播"路径。 */
  if (strncmp(lastUri_, "http://", 7) == 0 || strncmp(lastUri_, "https://", 8) == 0) {
    LOGD("PgDlna: 在线流直连（不下载、不落盘），交给 PgStream 播");
    pushAction(DlnaAction::PLAY, lastUri_, 0);
    return;
  }
  if (dlBusy_) {
    LOGD("PgDlna: 上一次下载还在跑，忽略新的 URI");
    return;
  }
  pthread_mutex_lock(&qMtx_);  // 清掉待执行的旧动作
  qTail_ = qHead_;
  pthread_mutex_unlock(&qMtx_);
  dlBusy_ = 1;
  downloadPercent_ = 0;
  pthread_create(&dlTid_, 0, pgDlnaDownloadThread, this);
  pthread_detach(dlTid_);
}

void Dlna::onSoapCommand(int type, int arg) { pushAction(type, 0, arg); }

void Dlna::injectUri(const char *uri) { onSetUri(uri); }
void Dlna::injectPlayPause(bool play) { pushAction(play ? DlnaAction::RESUME : DlnaAction::PAUSE, 0, 0); }
void Dlna::injectStop() { pushAction(DlnaAction::STOP, 0, 0); }

/* ---------- HTTP：描述 / SCPD / SOAP / 事件 ---------- */
int pgDlnaHttpHandler(struct mg_connection *conn, void *cbdata) {
  Dlna *self = (Dlna *)cbdata;
  self->httpRequest(conn);
  return 1;  // 已自行应答
}

void Dlna::httpRequest(struct mg_connection *conn) {
  const struct mg_request_info *ri = mg_get_request_info(conn);
  const char *uri = ri ? ri->request_uri : "";
  const char *method = ri ? ri->request_method : "GET";

  if (strstr(uri, "description.xml")) {
    char body[3072];
    int n = snprintf(body, sizeof(body), kDescriptionFmt, kDevType, friendlyName_, kUuid,
                     kAvtSvc, kRcsSvc, kCmsSvc);
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
              "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
              n, body);
    LOGD("PgDlna: GET description.xml");
    return;
  }
  if (strstr(uri, "scpd.xml")) {
    char body[8192];
    const char *acts = strstr(uri, "RenderingControl") ? kRcsActions
                       : strstr(uri, "ConnectionManager") ? kCmsActions
                                                          : kAvtActions;
    int n = snprintf(body, sizeof(body), "%s%s%s", kScpdHead, acts, kScpdTail);
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
              "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
              n, body);
    LOGD("PgDlna: GET %s", uri);
    return;
  }

  // ---- SUBSCRIBE（事件订阅）：MVP 只回 SID/TIMEOUT，不做 NOTIFY 推送 ----
  if (strcmp(method, "SUBSCRIBE") == 0) {
    static int sid = 1000;
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\nSID: uuid:%s-%d\r\nTIMEOUT: Second-1800\r\n"
             "Server: Linux/4.9 UPnP/1.0 PocketGameDMR/1.0\r\nContent-Length: 0\r\n\r\n",
             kUuid, ++sid);
    mg_printf(conn, "%s", hdr);
    LOGD("PgDlna: SUBSCRIBE %s -> 已回 SID（不推 NOTIFY）", uri);
    return;
  }

  // ---- SOAP 控制 ----
  if (strstr(uri, "/upnp/control/")) {
    const bool avt = strstr(uri, "AVTransport") != 0;
    const bool rcs = strstr(uri, "RenderingControl") != 0;
    const char *svc = avt ? kAvtSvc : (rcs ? kRcsSvc : kCmsSvc);

    // 读 body（⚠️ 给足：带签名参数的 CDN 链接能让 SOAP 体轻松超 4KB，
    //   截断了就找不到 </CurrentURI> → 整条 SetAVTransportURI 白丢）
    char body[8192] = {0};
    int total = 0, n;
    while (total < (int)sizeof(body) - 1 &&
           (n = mg_read(conn, body + total, sizeof(body) - 1 - total)) > 0) {
      total += n;
    }
    // SOAPAction: "urn:...:service:AVTransport:1#Play"
    const char *act = mg_get_header(conn, "SOAPACTION");
    char action[64] = {0};
    if (act) {
      const char *h = strrchr(act, '#');
      if (h) {
        h++;
        int i = 0;
        while (h[i] && h[i] != '"' && h[i] != '\r' && i < (int)sizeof(action) - 1) {
          action[i] = h[i];
          i++;
        }
        action[i] = 0;
      }
    }
    LOGD("PgDlna: SOAP %s -> action=%s (body %d 字节)", uri, action, total);
    if (avt || rcs || strstr(uri, "ConnectionManager")) {
      if (!action[0]) {
        soapFault(conn, 401, "Invalid Action");
        return;
      }
      // --- 统一处理 ---
      if (avt) {
        if (strcmp(action, "SetAVTransportURI") == 0) {
          /* URI 可能很长（带签名参数的 CDN 链接轻松 500~800 字符），给小了会被截断 */
          char cur[2048] = {0};
          xmlTag(body, "CurrentURI", cur, sizeof(cur));
          onSetUri(cur);
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "Play") == 0) {
          char sp[16] = {0};
          xmlTag(body, "Speed", sp, sizeof(sp));
          if (strcmp(transportState_, "PAUSED_PLAYBACK") == 0) onSoapCommand(DlnaAction::RESUME, 0);
          else onSoapCommand(DlnaAction::RESUME, 0);  // 播放器支持 resume 语义
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "Pause") == 0) {
          onSoapCommand(DlnaAction::PAUSE, 0);
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "Stop") == 0) {
          onSoapCommand(DlnaAction::STOP, 0);
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "Seek") == 0) {
          char tgt[32] = {0};
          xmlTag(body, "Target", tgt, sizeof(tgt));
          // 支持 "H:MM:SS" 或 "0:00:12"
          int h = 0, m = 0, s = 0;
          if (sscanf(tgt, "%d:%d:%d", &h, &m, &s) >= 2) {
            onSoapCommand(DlnaAction::SEEK, ((h * 60 + m) * 60 + s) * 1000);
          }
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "GetTransportInfo") == 0) {
          char a[256];
          snprintf(a, sizeof(a),
                   "<CurrentTransportState>%s</CurrentTransportState>"
                   "<CurrentTransportStatus>OK</CurrentTransportStatus>"
                   "<CurrentSpeed>1</CurrentSpeed>",
                   transportState_);
          soapOk(conn, svc, action, a);
          return;
        }
        if (strcmp(action, "GetMediaInfo") == 0) {
          char dur[32], a[1024];
          msToTime(durationMs_, dur, sizeof(dur));
          snprintf(a, sizeof(a),
                   "<NrTracks>1</NrTracks><MediaDuration>%s</MediaDuration>"
                   "<CurrentURI>%s</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
                   "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                   "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                   "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
                   dur, lastUri_);
          soapOk(conn, svc, action, a);
          return;
        }
        if (strcmp(action, "GetPositionInfo") == 0) {
          char dur[32], rel[32], a[1024];
          msToTime(durationMs_, dur, sizeof(dur));
          msToTime(positionMs_, rel, sizeof(rel));
          snprintf(a, sizeof(a),
                   "<Track>1</Track><TrackDuration>%s</TrackDuration>"
                   "<TrackMetaData></TrackMetaData><TrackURI>%s</TrackURI>"
                   "<RelTime>%s</RelTime><AbsTime>%s</AbsTime>"
                   "<RelCount>2147483647</RelCount><AbsCount>2147483647</AbsCount>",
                   dur, lastUri_, rel, rel);
          soapOk(conn, svc, action, a);
          return;
        }
        if (strcmp(action, "GetDeviceCapabilities") == 0) {
          soapOk(conn, svc, action,
                 "<PlayMedia>NETWORK,NONE</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                 "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
          return;
        }
        if (strcmp(action, "GetTransportSettings") == 0) {
          soapOk(conn, svc, action,
                 "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
          return;
        }
        if (strcmp(action, "SetNextAVTransportURI") == 0 || strcmp(action, "Next") == 0 ||
            strcmp(action, "Previous") == 0) {
          soapOk(conn, svc, action, "");  // 单曲播放器：接受但不做事
          return;
        }
        soapFault(conn, 401, "Invalid Action");
        return;
      }

      if (rcs) {
        if (strcmp(action, "GetVolume") == 0) {
          char a[128];
          snprintf(a, sizeof(a), "<CurrentVolume>%d</CurrentVolume>", volume_);
          soapOk(conn, svc, action, a);
          return;
        }
        if (strcmp(action, "SetVolume") == 0) {
          char v[16] = {0};
          xmlTag(body, "DesiredVolume", v, sizeof(v));
          int pct = atoi(v);
          setVolumeInternal(pct);
          onSoapCommand(DlnaAction::SET_VOLUME, volume_);
          soapOk(conn, svc, action, "");
          return;
        }
        if (strcmp(action, "GetMute") == 0) {
          soapOk(conn, svc, action, "<CurrentMute>0</CurrentMute>");
          return;
        }
        if (strcmp(action, "SetMute") == 0) {
          soapOk(conn, svc, action, "");
          return;
        }
        soapFault(conn, 401, "Invalid Action");
        return;
      }

      // ConnectionManager
      if (strcmp(action, "GetProtocolInfo") == 0) {
        char a[1024];
        snprintf(a, sizeof(a), "<Source></Source><Sink>%s</Sink>", kSinkProto);
        soapOk(conn, svc, action, a);
        return;
      }
      if (strcmp(action, "GetCurrentConnectionIDs") == 0) {
        soapOk(conn, svc, action, "<ConnectionIDs>0</ConnectionIDs>");
        return;
      }
      if (strcmp(action, "GetCurrentConnectionInfo") == 0) {
        soapOk(conn, svc, action,
               "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
               "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
               "<PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction>"
               "<Status>OK</Status>");
        return;
      }
      soapFault(conn, 401, "Invalid Action");
      return;
    }
    soapFault(conn, 401, "Invalid Action");
    return;
  }

  mg_printf(conn,
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
}

/* ---------- 启停 ---------- */
bool Dlna::start(const char *friendlyName, int httpPort) {
  if (running_) return true;
  snprintf(friendlyName_, sizeof(friendlyName_), "%s",
           (friendlyName && friendlyName[0]) ? friendlyName : "PocketGame-DMR");
  httpPort_ = httpPort > 0 ? httpPort : 8200;
  pthread_mutex_init(&qMtx_, 0);
  pickLocalIp(localIp_, sizeof(localIp_));
  stopFlag_ = 0;
  aliveReq_ = 0;
  char ports[16];
  snprintf(ports, sizeof(ports), "%d", httpPort_);
  const char *opts[] = {"listening_ports", ports,
                        // 本板只有 56MB 内存（Committed_AS 常年在 CommitLimit 上），
                        // 线程越少越省：DLNA 控制流量极小，2 个足够。
                        "num_threads", "3",
                        "request_timeout_ms", "15000",
                        "enable_keep_alive", "no",
                        0};
  ctx_ = (void *)mg_start(0, this, opts);
  if (!ctx_) {
    LOGD("PgDlna: mg_start 失败（端口 %d 被占？）", httpPort_);
    return false;
  }
  mg_set_request_handler((struct mg_context *)ctx_, "/description.xml", pgDlnaHttpHandler, this);
  mg_set_request_handler((struct mg_context *)ctx_, "/AVTransport/scpd.xml", pgDlnaHttpHandler, this);
  mg_set_request_handler((struct mg_context *)ctx_, "/RenderingControl/scpd.xml", pgDlnaHttpHandler, this);
  mg_set_request_handler((struct mg_context *)ctx_, "/ConnectionManager/scpd.xml", pgDlnaHttpHandler, this);
  mg_set_request_handler((struct mg_context *)ctx_, "/upnp/control", pgDlnaHttpHandler, this);
  mg_set_request_handler((struct mg_context *)ctx_, "/upnp/event", pgDlnaHttpHandler, this);

  /* ⚠️ SSDP 线程**不 detach**：要让 stop() 能 join 它（原因见 Dlna::stop 的说明）。
   *    下载线程仍然 detach（它可能在大文件下载中阻塞很久，join 会卡住 UI）。 */
  if (pthread_create(&ssdpTid_, 0, pgDlnaSsdpThread, this) == 0) ssdpJoinable_ = 1;

  running_ = true;
  LOGD("PgDlna: 已启动 名称='%s' 控制端口=%d 本机IP=%s UDN=uuid:%s", friendlyName_,
       httpPort_, localIp_, kUuid);
  return true;
}

void Dlna::stop() {
  if (!running_) return;
  stopFlag_ = 1;
  /* ⚠️ SSDP 线程**必须 join**（不是 detach）：它只阻塞在 400ms 超时的 recvfrom 上，
   *    所以 join 最多等 400ms。不 join 的后果实测很阴：`dlna off` 后对象被 delete，
   *    而旧线程醒来还在读 this（已释放）——表现为**两份 SSDP 应答**、LOCATION 端口变 0
   *    （`http://192.168.0.125:0/description.xml`），控制器拿到就是连不上。 */
  if (ssdpJoinable_) {
    pthread_join(ssdpTid_, 0);
    ssdpJoinable_ = 0;
  }
  if (ctx_) {
    mg_stop((struct mg_context *)ctx_);
    ctx_ = 0;
  }
  running_ = false;
  LOGD("PgDlna: 已停止（SSDP 线程已回收）");
}

}  // namespace pg
