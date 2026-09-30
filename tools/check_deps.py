# -*- coding: utf-8 -*-
"""check_deps.py —— 检查 src/dependencies/ 是否补齐（开源仓库不含厂商 SDK 与二进制）。

用法：
    python tools/check_deps.py

设计原则：**不静默**。缺什么、该放哪、从哪来，逐条打出来；退出码非 0 表示不齐。
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (相对路径, 期望最小字节数, 来源说明)
REQUIRED = [
    # --- 全志 V85X SDK 头文件（挑几个"缺了就编译不过"的锚点）---
    ("src/dependencies/include/mpi_sys.h",          1000, "全志 V85X SDK"),
    ("src/dependencies/include/mpi_vo.h",           1000, "全志 V85X SDK"),
    ("src/dependencies/include/mpi_venc.h",         1000, "全志 V85X SDK"),
    ("src/dependencies/include/mm_comm_video.h",    1000, "全志 V85X SDK"),
    ("src/dependencies/include/aw_type.h",           100, "全志 V85X SDK"),
    ("src/dependencies/include/videodev2.h",        1000, "全志 V85X SDK"),
    ("src/dependencies/include/h264_player.h",       100, "全志 V85X SDK"),
    ("src/dependencies/include/wifi_sta.h",          100, "全志 V85X SDK"),
    ("src/dependencies/include/bt_av.h",             100, "全志 V85X SDK / btstack"),
    # --- ffmpeg 头与静态库 ---
    ("src/dependencies/include/libavcodec/avcodec.h", 1000, "ffmpeg（自行交叉编译或取 SDK 版）"),
    ("src/dependencies/include/libavformat/avformat.h", 1000, "ffmpeg"),
    ("src/dependencies/include/libavutil/frame.h",   1000, "ffmpeg"),
    ("src/dependencies/lib/libavcodec.a",          100000, "ffmpeg 静态库（strip -g 后）"),
    ("src/dependencies/lib/libavformat.a",         100000, "ffmpeg 静态库"),
    ("src/dependencies/lib/libavutil.a",            50000, "ffmpeg 静态库"),
    ("src/dependencies/lib/libswresample.a",        20000, "ffmpeg 静态库"),
    # --- 其它库与固件 ---
    ("src/dependencies/lib/libzkmedia.a",           10000, "全志 SDK"),
    ("src/dependencies/lib/libcrypto.so.1.1",      100000, "全志 SDK / OpenSSL 1.1"),
    ("src/dependencies/lib/libssl.so.1.1",          50000, "全志 SDK / OpenSSL 1.1"),
    ("src/dependencies/lib-no-link/libawh264player.so", 10000, "全志 SDK（不进链接，由 fun pack 打进 /res/lib）"),
    ("src/dependencies/bin/firmware/rtlbt/rtl8733bs_fw", 1000, "Realtek RTL8733BS 蓝牙固件"),
    ("src/dependencies/bin/firmware/rtlbt/rtl8733bs_config", 100, "Realtek RTL8733BS 蓝牙配置"),
]

# 工具链：不在仓库里，但构建需要（给出环境变量名，方便提示）
TOOLS = [
    ("fun", "FlyThings / EasyUI 命令行工具（IDE 自带）", "加入 PATH 或设置 FUN 环境变量"),
    ("adb", "ADB（烧写与真机验收用）", "设置 ADB 环境变量"),
]


def main():
    missing, ok = [], []
    for rel, min_size, source in REQUIRED:
        p = os.path.join(ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(p):
            missing.append((rel, "缺失", source))
        elif os.path.getsize(p) < min_size:
            missing.append((rel, "过小(%d B < %d B)，可能是占位文件" % (os.path.getsize(p), min_size), source))
        else:
            ok.append(rel)

    print("=" * 72)
    print("src/dependencies 依赖检查")
    print("=" * 72)
    for rel in ok:
        print("  [OK]   %s" % rel)
    for rel, why, source in missing:
        print("  [缺少] %-54s 来源：%s" % (rel, source))
        print("         └─ %s" % why)
    print("-" * 72)
    print("就绪 %d / %d" % (len(ok), len(REQUIRED)))

    if missing:
        print("\n还有 %d 项没补齐 —— 完整说明见 docs/DEPENDENCIES.md" % len(missing))
        print("补齐后本工程即可用 `fun install && fun build` 编译。")
    else:
        print("\n依赖齐全，可以开始构建：见 docs/BUILD.md")

    print("\n工具链（不在仓库里，需自行准备）：")
    for name, what, how in TOOLS:
        print("  %-6s %s（%s）" % (name, what, how))

    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
