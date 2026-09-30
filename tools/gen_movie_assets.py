#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把用户给的竖屏 HEVC 片源转成设备能硬解的 H.264 MP4（两个"动画电影"应用用）。

============================================================================
为什么必须转码（三条都是实测/文档结论，不是猜的）
============================================================================
1. **片源是 HEVC(H.265)**，而本工程的视频链路是 **H.264 专用**：
   `zk_h264_player`（libawh264player）只解 H264，PgStream 的 bsf 用的是
   `h264_mp4toannexb`。直接把 hvc1 丢进去 = 黑屏。
   见 docs/kb-v85x-h264-player.md「本包只解 H264」。
2. **源是 720x1280 = 921600 像素**，超过解码内存守卫的 960x544（522240 像素）
   安全线；本板只有 56MB 内存，720p 不缩放会直接触发内核 OOM
   （见 docs/online-media.md §13）。缩到 480x800（384000 像素）后**低于**上限，
   不需要缩放解码，最省内存。
3. **/res 分区只有 7.6MB，而 out/update.img 已经 ~6.96MB**（余量约 1.0MB）。
   H.264 本身已压缩，进 squashfs 基本 1:1 ⇒ 两个片子总共只能占 ~0.8MB。

============================================================================
铺满 480x800 的几何（用户已确认"铺满全屏"）
============================================================================
源 720x1280（9:16, 0.5625），屏 480x800（0.6）。
按宽度铺满 → 高要 480/0.5625 = 853.3 > 800 ⇒ 需要裁掉纵向。
不裁而直接拉伸会纵向拉长约 6.7%（用户选了"铺满"而不是"拉伸"）。
做法：先把源纵向裁成 **720x1200**（上下各去 40px，居中）—— 1200/720 = 1.667 = 800/480，
正好是屏幕比例；再缩放到 480x800。⇒ 屏幕上 1:1 像素，无黑边、无变形、无需硬件缩放。

============================================================================
用法
============================================================================
    python tools/gen_movie_assets.py              # 转码两个片子
    python tools/gen_movie_assets.py --check      # 只看现在占多少，不转码

⚠️ 输出到 `resources/media/`（随包进 `/res/ui/media/`），**提交进工程**。
⚠️ 需要 ffmpeg；找不到时用环境变量 FFMPEG 指路。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "resources", "media")

# 源片（用户放在工作区 pet_video/ 下）
SRC_DIR = os.path.abspath(os.path.join(ROOT, "..", "pet_video"))

# (应用 id, 源文件, 输出文件)   —— 顺序与 kAppTable 的 slot 35/36 一致
MOVIES = [
    ("fairy", "moon.mp4", "fairy.mp4"),     # 飞天仙女
    ("kitten", "cat.mp4", "kitten.mp4"),    # 可爱小猫
]

# ---- 编码参数（改了要重新量体积，见文件头的 §3）----
CROP = "crop=720:1200:0:40"     # 源 720x1280 → 720x1200（纵向居中各裁 40px）
SCALE = "480x800"               # = 屏幕分辨率，播放时 1:1
FPS = 15                        # 24 → 15fps：10 秒片的体积/质量折中（SSIM 0.93）
VIDEO_KBPS = 300                # 2-pass 目标码率（单文件约 372KB）
AUDIO_KBPS = 40                 # AAC-LC 22050Hz 单声道 —— 与 PgAudio 常开流同规格，省一次重采样
MAX_TOTAL_KB = 850              # 两个片子合计上限（/res 余量约 1.0MB，留头）

FFMPEG_CANDIDATES = [
    os.environ.get("FFMPEG", ""),
    "ffmpeg",
    r"E:\AICODE\F102\ffmpeg.exe",
    r"E:\AICODE\jiaqian\bin\ffmpeg.exe",
]


def find_ffmpeg():
    for c in FFMPEG_CANDIDATES:
        if not c:
            continue
        p = c if os.path.isabs(c) else shutil.which(c)
        if not p:
            continue
        try:
            subprocess.run([p, "-version"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            return p
        except Exception:
            pass
    return None


def kb(path):
    return os.path.getsize(path) / 1024.0


def probe_audio_channels(ff, src):
    """源片的音频声道数（1=单声道 / 2=立体声 / 0=没音轨 / -1=有音轨但没认出布局）。"""
    try:
        p = subprocess.run([ff, "-hide_banner", "-i", src], capture_output=True)
        txt = p.stderr.decode("utf-8", "replace")
    except Exception:
        return 0
    for line in txt.splitlines():
        if "Audio:" not in line:
            continue
        if "mono" in line:
            return 1
        if "stereo" in line:
            return 2
        m = re.search(r"(\d+) channels", line)
        return int(m.group(1)) if m else -1
    return 0


def encode(ff, src, dst):
    """2-pass ABR：体积可预测（CRF 在这个内容上波动太大）。"""
    vf = "%s,scale=%s,fps=%d" % (CROP, SCALE, FPS)
    base = [ff, "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-vf", vf,
            "-c:v", "libx264", "-profile:v", "main", "-level", "3.1",
            "-preset", "slow", "-pix_fmt", "yuv420p",
            "-b:v", "%dk" % VIDEO_KBPS, "-maxrate", "%dk" % (VIDEO_KBPS * 4 // 3),
            "-bufsize", "%dk" % (VIDEO_KBPS * 2)]
    logf = os.path.join(os.path.dirname(dst), ".passlog_" + os.path.basename(dst))
    # ★★ 音频下混要**显式写平均**（2026-09-21 实测踩到）：
    #   片源有的是单声道、有的是立体声。立体声走 ffmpeg 默认矩阵下混到单声道时，
    #   两声道同相的内容会**叠加** ⇒ 电平实测 **+3dB**（RMS 2100 → 2959）——
    #   结果是"两段片子一个偏响一个正常"，用户切来切去听得出来。
    #   ⇒ 立体声源一律用 `pan=mono|c0=0.5*c0+0.5*c1`（真正的平均）；单声道源原样。
    ch = probe_audio_channels(ff, src)
    if ch >= 2:
        aopt = ["-af", "pan=mono|c0=0.5*c0+0.5*c1"]
        print("   音频：源 %d 声道 → 平均下混为单声道" % ch)
    else:
        aopt = ["-ac", "1"]
        print("   音频：源 %s（原样）" % ({1: "单声道", 0: "无音轨", -1: "布局未识别"}.get(ch, "?")))
    # ★ pass 1 的输出必须是**真实文件**：Windows 上 `/dev/null` 不存在、`nul` 也不吃，
    #   2-pass 会直接报 "can't open stats file"（实测踩到）。用完即删。
    tmp1 = os.path.join(os.path.dirname(dst), ".pass1_" + os.path.basename(dst))
    aenc = ["-c:a", "aac", "-b:a", "%dk" % AUDIO_KBPS] + aopt + ["-ar", "22050"]
    p1 = base + aenc + ["-pass", "1", "-passlogfile", logf, tmp1]
    subprocess.run(p1, check=True)
    if os.path.exists(tmp1):
        os.remove(tmp1)
    # pass 2（★ +faststart：把 moov 挪到文件头，ffmpeg 不用发 Range 去尾部找）
    p2 = base + aenc + ["-movflags", "+faststart", "-pass", "2", "-passlogfile", logf, dst]
    subprocess.run(p2, check=True)
    for f in os.listdir(os.path.dirname(dst) or "."):
        if f.startswith(os.path.basename(logf)):
            os.remove(os.path.join(os.path.dirname(dst), f))


def report(tag):
    print("=" * 64)
    total = 0.0
    for _id, _s, out in MOVIES:
        p = os.path.join(OUT_DIR, out)
        if os.path.exists(p):
            s = kb(p)
            total += s
            print("  %-12s %8.0f KB" % (out, s))
        else:
            print("  %-12s %8s" % (out, "缺失"))
    print("  %-12s %8.0f KB   （上限 %d KB）" % ("合计", total, MAX_TOTAL_KB))
    print("=" * 64)
    return total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只统计不转码")
    args = ap.parse_args()

    if args.check:
        report("现状")
        return 0

    ff = find_ffmpeg()
    if not ff:
        print("!! 找不到 ffmpeg。装一个，或用 FFMPEG=<路径> 指路。", file=sys.stderr)
        return 2
    print("ffmpeg = %s" % ff)

    os.makedirs(OUT_DIR, exist_ok=True)
    for _id, srcname, out in MOVIES:
        src = os.path.join(SRC_DIR, srcname)
        if not os.path.exists(src):
            print("!! 源片不存在：%s" % src, file=sys.stderr)
            return 2
        dst = os.path.join(OUT_DIR, out)
        print("转码 %s -> %s ..." % (srcname, out))
        encode(ff, src, dst)

    total = report("结果")
    if total > MAX_TOTAL_KB:
        print("!! 合计超过 %d KB —— /res 分区可能装不下，"
              "请调低 VIDEO_KBPS 或 FPS 后重跑。" % MAX_TOTAL_KB, file=sys.stderr)
        return 1
    print("OK：两个片子已就绪，可以 build 了。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
