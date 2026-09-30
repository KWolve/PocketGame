#!/usr/bin/env python3
"""带 Range 支持的极简静态 HTTP 服务（真机在线流测试台架）。

为什么不能用 `python -m http.server`：它**不支持 Range 请求**。而很多测试片
（filesamples 的 sample_*.mp4）把 moov 放在文件尾部，ffmpeg 必须发 Range 请求去尾部取
moov；服务端忽略 Range 就会让 ffmpeg 解析错乱 —— 现象是 `av_read_frame` 第一次调用就
返回 EOF（日志：送 0 包 / 解出 0 帧），非常像"播放器坏了"，其实是台架的问题。

用法: python range_http.py [端口] [目录]
"""
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
ROOT = sys.argv[2] if len(sys.argv) > 2 else "."
# 绑到具体网卡地址（而不是 0.0.0.0）：Windows 防火墙对 0.0.0.0 监听会拦外部连接，
# 实测设备连 0.0.0.0:8098 是 "Operation timed out"，绑 192.168.x.x 就通。
BIND = sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 精简日志
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send_range(self, path, head_only=False):
        try:
            size = os.path.getsize(path)
            f = open(path, "rb")
        except OSError:
            self.send_error(404)
            return
        ctype = "video/mp4" if path.endswith(".mp4") else "application/octet-stream"
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        code = 200
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)", rng.strip())
            if m:
                if m.group(1):
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else size - 1
                elif m.group(2):  # 后缀范围：最后 N 字节
                    start = max(0, size - int(m.group(2)))
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    f.close()
                    return
                code = 206
        length = end - start + 1
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        if head_only:
            f.close()
            return
        f.seek(start)
        left = length
        while left > 0:
            chunk = f.read(min(65536, left))
            if not chunk:
                break
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                break
            left -= len(chunk)
        f.close()

    def do_GET(self):
        p = self.translate_path(self.path)
        if os.path.isdir(p):
            p = os.path.join(p, "index.html")
        if not os.path.isfile(p):
            self.send_error(404)
            return
        self._send_range(p)

    def do_HEAD(self):
        p = self.translate_path(self.path)
        if not os.path.isfile(p):
            self.send_error(404)
            return
        self._send_range(p, head_only=True)

    def translate_path(self, path):
        path = path.split("?", 1)[0].split("#", 1)[0]
        import urllib.parse

        path = urllib.parse.unquote(path)
        parts = [x for x in path.split("/") if x not in ("", ".", "..")]
        return os.path.join(ROOT, *parts)


if __name__ == "__main__":
    srv = ThreadingHTTPServer((BIND, PORT), Handler)
    print("range-http serving %s on %s:%d" % (os.path.abspath(ROOT), BIND, PORT), flush=True)
    srv.serve_forever()
