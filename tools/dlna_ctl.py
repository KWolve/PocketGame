#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DLNA/UPnP 控制器（PC 侧）—— 用来给 PocketGame 的 DMR 渲染端做真机端到端验收。

它等价于"手机上的投屏 App / Windows 的投放"：
  1) M-SEARCH 组播发现设备（SSDP）
  2) 拉 device description，解析 controlURL
  3) SOAP 调 AVTransport / RenderingControl（SetAVTransportURI / Play / Pause / Stop / Seek / Volume）

只用标准库，方便在任意机器上跑。
用法：
  python dlna_ctl.py discover
  python dlna_ctl.py info    <host> [port]
  python dlna_ctl.py cast    <host> <媒体URL> [port]      # SetAVTransportURI + Play（一步到位）
  python dlna_ctl.py pause|stop|resume <host> [port]
  python dlna_ctl.py seek    <host> <秒> [port]
  python dlna_ctl.py vol     <host> <0-100> [port]
  python dlna_ctl.py raw     <host> <service> <action> [xml片段] [port]
"""
import re
import socket
import sys
import time
import urllib.error
import urllib.request

SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
CMS = "urn:schemas-upnp-org:service:ConnectionManager:1"


# ---------------------------------------------------------------- SSDP 发现
def local_ipv4_addresses():
    """本机所有非环回 IPv4（用来逐网卡发组播）。"""
    addrs = set()
    try:
        for ai in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(ai[4][0])
    except OSError:
        pass
    addrs.discard("127.0.0.1")
    addrs.discard("0.0.0.0")
    return sorted(addrs)


def discover(timeout=3.0, iface=None):
    """发 M-SEARCH，返回 [(location, st, usn, 应答源IP), ...]

    ⚠️ 多网卡的 PC（如"以太网 + WLAN"）必须逐网卡发，否则内核按默认路由表选出口，
    组播可能从**另一张网卡**发出去（实测：设备在同网段却一条都收不到）。
    """
    msg = (
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: %s:%d\r\n"
        'MAN: "ssdp:discover"\r\n'
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n\r\n"
        % (SSDP_ADDR, SSDP_PORT)
    )
    ifaces = [iface] if iface else (local_ipv4_addresses() or [None])
    socks = []
    for ip in ifaces:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if ip:
            try:
                s.bind((ip, 0))
            except OSError as e:
                print("  跳过网卡 %s: %s" % (ip, e))
                s.close()
                continue
            try:
                s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
            except OSError:
                pass
        try:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        except OSError:
            pass
        s.settimeout(0.3)
        s.sendto(msg.encode(), (SSDP_ADDR, SSDP_PORT))
        print("  M-SEARCH 已发出，出口网卡 %s" % (ip or "(默认)"))
        socks.append(s)

    found, seen = [], set()
    deadline = time.time() + timeout
    while time.time() < deadline and socks:
        for s in list(socks):
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                socks.remove(s)
                continue
            text = data.decode("utf-8", "replace")
            loc = re.search(r"(?im)^LOCATION:\s*(\S+)", text)
            if not loc or loc.group(1) in seen:
                continue
            seen.add(loc.group(1))
            st = re.search(r"(?im)^ST:\s*(\S+)", text)
            usn = re.search(r"(?im)^USN:\s*(\S+)", text)
            found.append((loc.group(1), st.group(1) if st else "",
                          usn.group(1) if usn else "", addr[0]))
    for s in socks:
        s.close()
    return found


# ---------------------------------------------------------------- SOAP
def soap(host, port, service_type, action, inner_xml="", control_path=None, timeout=8.0):
    """调一个 SOAP 动作，返回 (http状态, 响应体)"""
    svc_path = {
        AVT: "AVTransport",
        RCS: "RenderingControl",
        CMS: "ConnectionManager",
    }[service_type]
    path = control_path or "/upnp/control/%s" % svc_path
    body = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        "<s:Body>"
        '<u:%s xmlns:u="%s">%s</u:%s>'
        "</s:Body></s:Envelope>" % (action, service_type, inner_xml, action)
    ).encode("utf-8")
    req = urllib.request.Request(
        "http://%s:%d%s" % (host, port, path),
        data=body,
        headers={
            "Content-Type": 'text/xml; charset="utf-8"',
            "SOAPAction": '"%s#%s"' % (service_type, action),
        },
        method="POST",
    )
    # 设备侧在"播放启动/解码缓冲"这种重负载瞬间会短暂拒绝新连接
    #（实测 PC 收到 WinError 10061；等 1 秒再发就好）→ 工具自带重试，别把抖动当故障。
    last = None
    for attempt in range(3):
        try:
            with _opener().open(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")
        except Exception as e:  # URLError / ConnectionRefused / timeout
            last = e
            if attempt < 2:
                time.sleep(0.8)
    raise last


def _opener():
    """不走系统代理。

    ⚠️ 必须在开发机上禁代理：很多环境设了 http_proxy/https_proxy（WorkBuddy 沙箱同理），
    局域网直连 192.168.x.x 会被代理接管 → 明明设备在跑却收到 502 Bad Gateway
    （实测踩过：`info` 返回 HTTP=502，让人误以为是设备坏了）。
    """
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def xml_tag(xml, tag):
    m = re.search(r"<[^>]*%s[^>]*>(.*?)</[^>]*%s>" % (tag, tag), xml, re.S)
    return m.group(1).strip() if m else ""


def avt(host, port, action, inner="", timeout=8.0):
    return soap(host, port, AVT, action, inner, timeout=timeout)


# ---------------------------------------------------------------- 动作封装
def get_transport_info(host, port=8200):
    st, body = avt(host, port, "GetTransportInfo", "<InstanceID>0</InstanceID>")
    return st, xml_tag(body, "CurrentTransportState")


def get_media_info(host, port=8200):
    st, body = avt(host, port, "GetMediaInfo", "<InstanceID>0</InstanceID>")
    return st, xml_tag(body, "MediaDuration"), xml_tag(body, "CurrentURI")


def get_position_info(host, port=8200):
    st, body = avt(host, port, "GetPositionInfo", "<InstanceID>0</InstanceID>")
    return st, xml_tag(body, "TrackDuration"), xml_tag(body, "RelTime")


def set_uri(host, uri, port=8200):
    inner = (
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>%s</CurrentURI>"
        "<CurrentURIMetaData></CurrentURIMetaData>" % uri
    )
    return avt(host, port, "SetAVTransportURI", inner)


def play(host, port=8200):
    return avt(host, port, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>")


def pause(host, port=8200):
    return avt(host, port, "Pause", "<InstanceID>0</InstanceID>")


def stop(host, port=8200):
    return avt(host, port, "Stop", "<InstanceID>0</InstanceID>")


def seek(host, sec, port=8200):
    h, m, s = sec // 3600, (sec % 3600) // 60, sec % 60
    return avt(
        host,
        port,
        "Seek",
        "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%d:%02d:%02d</Target>"
        % (h, m, s),
    )


def set_volume(host, pct, port=8200):
    return soap(
        host,
        port,
        RCS,
        "SetVolume",
        "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>%d</DesiredVolume>" % pct,
    )


def get_volume(host, port=8200):
    st, body = soap(host, port, RCS, "GetVolume", "<InstanceID>0</InstanceID><Channel>Master</Channel>")
    return st, xml_tag(body, "CurrentVolume")


# ---------------------------------------------------------------- CLI
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    a = sys.argv[2:]

    def host_port(default_port=8200, port_index=1):
        """解析 host/port。

        ⚠️ port_index 必须按命令的实参位置给：`vol <host> <0-100>` / `seek <host> <秒>`
        的**第 2 个实参是数值业务参数，不是端口** —— 早期版本一律把 a[1] 当端口，
        于是 `vol 192.168.0.125 40` 去连 40 端口，报 WinError 10061，
        看起来像"设备拒绝连接"，其实是工具自己连错门（踩过）。
        """
        host = a[0]
        port = default_port
        if len(a) > port_index and a[port_index].isdigit():
            v = int(a[port_index])
            if 1 <= v <= 65535:
                port = v
        return host, port

    if cmd == "discover":
        res = discover()
        if not res:
            print("没发现设备（PC 防火墙挡 UDP 回包？或设备没跑 DLNA）")
            return 1
        for loc, st, usn, src in res:
            print("LOCATION=%s\n  from=%s ST=%s\n  USN=%s" % (loc, src, st, usn))
        return 0

    if cmd == "info":
        h, p = host_port()
        st, state = get_transport_info(h, p)
        _, dur, uri = get_media_info(h, p)
        _, pdur, pos = get_position_info(h, p)
        print("HTTP=%s state=%s duration=%s position=%s/%s uri=%s"
              % (st, state, dur, pos, pdur, uri))
        return 0

    if cmd == "cast":
        h = a[0]
        url = a[1]
        p = int(a[2]) if len(a) > 2 else 8200
        st, body = set_uri(h, url, p)
        print("SetAVTransportURI -> HTTP %s" % st)
        if st != 200:
            print(body[:400])
            return 1
        st, body = play(h, p)
        print("Play -> HTTP %s" % st)
        if st != 200:
            print(body[:400])
            return 1
        return 0

    if cmd in ("pause", "stop", "resume"):
        h, p = host_port()
        st, body = {"pause": pause, "stop": stop, "resume": play}[cmd](h, p)
        print("%s -> HTTP %s" % (cmd, st))
        return 0 if st == 200 else 1

    if cmd == "seek":
        h, p = host_port(port_index=2)
        st, body = seek(h, int(a[1]), p)
        print("seek -> HTTP %s" % st)
        return 0 if st == 200 else 1

    if cmd == "vol":
        h, p = host_port(port_index=2)
        st, body = set_volume(h, int(a[1]), p)
        print("set volume -> HTTP %s" % st)
        st2, v = get_volume(h, p)
        print("  readback CurrentVolume=%s (HTTP %s)" % (v, st2))
        return 0

    if cmd == "raw":
        h = a[0]
        svc = {"AVTransport": AVT, "RenderingControl": RCS, "ConnectionManager": CMS}[a[1]]
        action = a[2]
        inner = a[3] if len(a) > 3 else "<InstanceID>0</InstanceID>"
        p = int(a[4]) if len(a) > 4 else 8200
        st, body = soap(h, p, svc, action, inner)
        print("HTTP %s\n%s" % (st, body))
        return 0

    print("未知命令: %s" % cmd)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
