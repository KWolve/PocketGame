#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""极简 MP4 盒子解析：打印分辨率/时长/帧率/编码（无 ffmpeg 时的兜底）。

用法: python tools/mp4info.py <file.mp4> [...]
"""
import struct
import sys

CONTAINERS = {
    b"moov", b"trak", b"mdia", b"minf", b"stbl", b"dinf", b"edts",
    b"udta", b"mvex", b"moof", b"traf", b"mfra", b"skip", b"strk",
}
VIDEO_CODECS = {b"avc1", b"avc3", b"hvc1", b"hev1", b"mp4v", b"vp09", b"av01"}
AUDIO_CODECS = {b"mp4a", b"ac-3", b"ec-3", b"Opus", b"alac"}


def walk(data, start, end, depth, out, path):
    i = start
    while i + 8 <= end:
        size = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        hdr = 8
        if size == 1:
            if i + 16 > end:
                break
            size = struct.unpack(">Q", data[i + 8:i + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - i
        if size < hdr or i + size > end:
            break
        body = i + hdr
        name = typ.decode("latin1")
        if typ == b"mvhd":
            ver = data[body]
            if ver == 1:
                ts, du = struct.unpack(">IQ", data[body + 20:body + 32])
            else:
                ts, du = struct.unpack(">II", data[body + 12:body + 20])
            out.append("%s%-6s timescale=%d duration=%d (%.2fs)" % (
                "  " * depth, name, ts, du, (du / ts) if ts else 0))
        elif typ == b"mdhd":
            ver = data[body]
            if ver == 1:
                ts, du = struct.unpack(">IQ", data[body + 20:body + 32])
            else:
                ts, du = struct.unpack(">II", data[body + 12:body + 20])
            out.append("%s%-6s timescale=%d duration=%d (%.2fs)" % (
                "  " * depth, name, ts, du, (du / ts) if ts else 0))
        elif typ == b"tkhd":
            ver = data[body]
            off = body + (32 if ver == 1 else 20)
            # 跳过 reserved(8) + layer(2) + altgroup(2) + volume(2) + reserved(2) + matrix(36)
            off += 8 + 4 + 4 + 36
            w, h = struct.unpack(">II", data[off:off + 8])
            out.append("%s%-6s w=%d h=%d (%.0fx%.0f)" % (
                "  " * depth, name, w >> 16, h >> 16, w / 65536.0, h / 65536.0))
        elif typ == b"hdlr":
            ht = data[body + 8:body + 12]
            out.append("%s%-6s handler=%s" % ("  " * depth, name, ht.decode("latin1")))
        elif typ in VIDEO_CODECS or typ in AUDIO_CODECS:
            line = "%s%-6s" % ("  " * depth, name)
            if typ in VIDEO_CODECS and body + 86 <= end:
                w, h = struct.unpack(">HH", data[body + 24:body + 28])
                line += " %dx%d" % (w, h)
                if typ in (b"avc1", b"avc3", b"hvc1", b"hev1"):
                    # 去找 avcC/hvcC 里的 profile/level
                    j = body + 86
                    while j + 8 <= i + size:
                        ss = struct.unpack(">I", data[j:j + 4])[0]
                        st = data[j + 4:j + 8]
                        if st in (b"avcC", b"hvcC") and ss > 8:
                            cfg = data[j + 8:j + 8 + 6]
                            line += " %s=[%s]" % (st.decode(),
                                                  " ".join("%02x" % b for b in cfg))
                            break
                        if ss < 8:
                            break
                        j += ss
            elif typ in AUDIO_CODECS and body + 28 <= end:
                ch, bits = struct.unpack(">HH", data[body + 16:body + 20])
                sr = struct.unpack(">I", data[body + 24:body + 28])[0] >> 16
                line += " %dHz %dch %dbit" % (sr, ch, bits)
            out.append(line)
        elif typ == b"stts":
            n = struct.unpack(">I", data[body + 4:body + 8])[0]
            out.append("%s%-6s entries=%d" % ("  " * depth, name, n))
        if typ in CONTAINERS:
            path.append(name)
            walk(data, body, i + size, depth + 1, out, path)
            path.pop()
        i += size


def main():
    for fn in sys.argv[1:]:
        with open(fn, "rb") as f:
            data = f.read()
        out = []
        walk(data, 0, len(data), 0, out, [])
        print("=" * 60)
        print("%s  (%d bytes / %.2f MB)" % (fn, len(data), len(data) / 1048576.0))
        print("=" * 60)
        for l in out:
            print(l)
        print()


if __name__ == "__main__":
    main()
