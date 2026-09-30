#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gensfx.py - 生成游戏音效 WAV（resources/audio/*.wav）

纯合成，不依赖任何素材：16bit / 22050Hz / 单声道 PCM。
注意：easyui 的 ZKMediaPlayer 播放过短文件可能失败，所以每个音效都做了
      最小 180ms 的包络与淡出，避免出现「点了没声音」。
"""
import math
import os
import struct

SR = 22050
ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OUT_DIR = os.path.join(ROOT, "resources", "audio")


def env(i, n, attack=0.01, release=0.35):
    """简单 ADSR 包络（这里只用 attack + 指数衰减 + 尾部淡出）"""
    t = i / float(n)
    a = min(1.0, t / attack) if attack > 0 else 1.0
    d = math.exp(-3.0 * t)
    r = 1.0
    if t > 1.0 - release:
        r = max(0.0, (1.0 - t) / release)
    return a * d * r


def tone(freq_from, freq_to=None, dur=0.22, vol=0.5, kind="sine",
         noise=0.0, attack=0.01):
    if freq_to is None:
        freq_to = freq_from
    n = int(SR * dur)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / float(max(n - 1, 1))
        f = freq_from + (freq_to - freq_from) * t
        phase += 2.0 * math.pi * f / SR
        if kind == "sine":
            s = math.sin(phase)
        elif kind == "square":
            s = 1.0 if math.sin(phase) >= 0 else -1.0
        elif kind == "tri":
            x = (phase / (2 * math.pi)) % 1.0
            s = 4.0 * abs(x - 0.5) - 1.0
        else:
            s = math.sin(phase)
        if noise > 0.0:
            # 确定性伪噪声（避免依赖 random 的版本差异）
            s = s * (1.0 - noise) + (((i * 1103515245 + 12345) >> 16 & 0x7FFF) /
                                     16384.0 - 1.0) * noise
        out.append(s * env(i, n, attack=attack) * vol)
    return out


def mix(*tracks):
    n = max(len(t) for t in tracks)
    out = [0.0] * n
    for t in tracks:
        for i, v in enumerate(t):
            out[i] += v
    return out


def seq(*tracks):
    out = []
    for t in tracks:
        out.extend(t)
    return out


def silence(dur):
    return [0.0] * int(SR * dur)


def pluck(freq, dur=0.42, vol=0.52, harmonics=(1.0, 0.42, 0.22, 0.11, 0.06),
          decay=3.2):
    """拨弦/琴槌音：基频 + 若干泛音，**高次泛音衰减更快**（这是"琴"味的关键）。

    为什么不用 tone()：单一正弦听起来像"电子蜂鸣器"，不像琴。真实琴弦的泛音
    按 1/k 递减、且高次泛音在几十毫秒内就没了 —— 那正是"叮"的那一下。
    """
    n = int(SR * dur)
    out = []
    for i in range(n):
        t = i / float(max(n - 1, 1))
        s = 0.0
        for k, a in enumerate(harmonics, start=1):
            s += a * math.sin(2.0 * math.pi * freq * k * i / SR) * \
                math.exp(-decay * (0.35 + 0.65 * k) * t)
        a0 = min(1.0, t / 0.005)                       # 极快起音
        r = 1.0 if t < 0.90 else max(0.0, (1.0 - t) / 0.10)
        out.append(s * a0 * r * vol)
    return out


def write_wav(path, samples):
    # 软限幅
    data = bytearray()
    for s in samples:
        if s > 1.0:
            s = 1.0
        if s < -1.0:
            s = -1.0
        data += struct.pack("<h", int(s * 32000))
    n = len(samples)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data)
    return os.path.getsize(path)


def build():
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)
    S = {}
    # 菜单点击：清脆短促（必须 >= 0.18s）
    S["click"] = seq(tone(1180, 880, 0.10, 0.45, "square", attack=0.005),
                     tone(760, 620, 0.12, 0.30, "sine", attack=0.005))
    # 移动：柔和低音
    S["move"] = tone(520, 470, 0.19, 0.34, "tri", attack=0.006)
    # 旋转
    S["rotate"] = tone(600, 900, 0.20, 0.38, "square", attack=0.005)
    # 落地
    S["drop"] = mix(tone(240, 150, 0.22, 0.55, "sine", attack=0.004),
                    tone(120, 80, 0.22, 0.35, "tri"))
    # 消行：上行三音
    S["clear"] = seq(tone(660, 660, 0.09, 0.4, "square"),
                     tone(880, 880, 0.09, 0.4, "square"),
                     tone(1320, 1320, 0.14, 0.4, "square"))
    # 合成（2048）
    S["merge"] = seq(tone(440, 880, 0.11, 0.4, "sine", attack=0.004),
                     tone(660, 990, 0.13, 0.3, "tri", attack=0.004))
    # 得分
    S["score"] = seq(tone(880, 880, 0.10, 0.42, "tri"),
                     tone(1170, 1170, 0.14, 0.36, "tri"))
    # 受击：噪声撞击
    S["hit"] = tone(320, 90, 0.22, 0.6, "tri", noise=0.75, attack=0.002)
    # 游戏结束：下行长音
    S["over"] = seq(tone(700, 700, 0.16, 0.4, "square"),
                    tone(520, 520, 0.16, 0.4, "square"),
                    tone(392, 392, 0.16, 0.4, "square"),
                    tone(262, 180, 0.34, 0.42, "square"))
    # 小鸟振翅
    S["jump"] = tone(680, 1080, 0.19, 0.42, "tri", attack=0.004)
    # 发射
    S["shoot"] = tone(1400, 620, 0.20, 0.26, "square", attack=0.003)
    # 骰子翻滚碰撞（摇骰子）：「咔啦咔啦」= 四个短噪声脉冲首尾相接。
    # 为什么做成"一串"而不是"一声"：游戏侧是按节拍**重复播放**它的
    #（GameDice::update 里每 120ms 播一次），单个脉冲太单薄，连播像在敲钉子。
    S["dice"] = seq(tone(1400, 520, 0.050, 0.44, "tri", noise=0.85, attack=0.002),
                    tone(1050, 380, 0.045, 0.34, "tri", noise=0.90, attack=0.002),
                    tone(1650, 600, 0.050, 0.40, "tri", noise=0.85, attack=0.002),
                    tone(880, 320, 0.055, 0.30, "tri", noise=0.90, attack=0.002))
    # 闹钟铃声：**循环播放友好** —— 每声都有包络、尾部留静音，
    # 所以循环接缝处不会有突变（否则会"咔"一下）。
    # 结构：高-更高-高-更高（四声"嘀"）+ 一段静音 = 1.24s 的节奏循环。
    S["alarm"] = seq(tone(1568, 1568, 0.13, 0.52, "square", attack=0.004),
                     silence(0.09),
                     tone(2093, 2093, 0.13, 0.52, "square", attack=0.004),
                     silence(0.09),
                     tone(1568, 1568, 0.13, 0.52, "square", attack=0.004),
                     silence(0.09),
                     tone(2093, 2093, 0.13, 0.52, "square", attack=0.004),
                     silence(0.45))

    # ---- 节奏钢琴：8 个音（C 大调 do..do'，一个八度）----
    # ⚠️ 每个 wav 必须 >= 180ms（ZKMediaPlayer 播过短文件会失败）——
    #    这里的 0.34s 尾巴正好也是"余音"，两全。
    for i, f in enumerate((523.25, 587.33, 659.25, 698.46,
                           783.99, 880.00, 987.77, 1046.50)):
        S["pno%d" % (i + 1)] = pluck(f, dur=0.34, vol=0.50)

    # ---- 打鼓：6 个鼓位 ----
    # 底鼓：正弦下扫（150→45Hz）——"咚"
    S["drm1"] = mix(tone(150, 45, 0.22, 0.72, "sine", attack=0.002),
                    tone(240, 90, 0.05, 0.30, "tri", noise=0.35, attack=0.001))
    # 军鼓：噪声 + 190Hz 鼓皮
    S["drm2"] = mix(tone(260, 150, 0.19, 0.45, "tri", noise=0.92, attack=0.001),
                    tone(190, 170, 0.19, 0.34, "sine", attack=0.002))
    # 踩镲：高频噪声，短促（补足到 0.19s 以跨过 ZKMediaPlayer 的下限）
    S["drm3"] = seq(tone(9000, 6000, 0.06, 0.30, "tri", noise=0.98, attack=0.001),
                    tone(7000, 5000, 0.13, 0.14, "tri", noise=0.98, attack=0.001))
    # 低嗵：200→110Hz
    S["drm4"] = mix(tone(210, 108, 0.24, 0.60, "sine", attack=0.002),
                    tone(320, 200, 0.04, 0.22, "tri", noise=0.5, attack=0.001))
    # 高嗵：330→170Hz
    S["drm5"] = mix(tone(330, 170, 0.21, 0.58, "sine", attack=0.002),
                    tone(460, 300, 0.04, 0.22, "tri", noise=0.5, attack=0.001))
    # 吊镲：长噪声（慢衰减）
    S["drm6"] = mix(tone(6000, 3200, 0.62, 0.34, "tri", noise=0.96, attack=0.001),
                    tone(12000, 8000, 0.62, 0.14, "tri", noise=0.99, attack=0.001))

    total = 0
    for name in sorted(S):
        p = os.path.join(OUT_DIR, name + ".wav")
        size = write_wav(p, S[name])
        total += size
        print("  %-8s %6d bytes  %.2fs" % (name + ".wav", size, len(S[name]) / SR))
    print("total %.1f KB -> %s" % (total / 1024.0, OUT_DIR))


if __name__ == "__main__":
    build()
