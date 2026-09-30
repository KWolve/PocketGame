#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gentone.py - 生成音频测试音（排查"设备到底有没有声音 / 哪块声卡接的是喇叭"）

为什么需要它：
  本板（V851s）有**两块声卡**，而且系统默认播放通路并不指向接喇叭的那块：
    card0 = audiocodec（片内 codec）   <- 板载喇叭接这里
    card1 = snddaudio0（ES714X I2S DAC）<- /etc/alsa/asound.conf 的默认通路走这里
  音量/路由这类问题必须用"能数出来的测试音"来定位（见 README 第七节音频坑）。

用法:
  python tools/gentone.py                          # 440Hz / 2 秒 / 22050Hz / 单声道
  python tools/gentone.py 440 3 beep.wav           # 频率 时长 输出文件
  python tools/gentone.py 440 2 beep48s.wav 48000 2  # 指定采样率/声道（I2S DAC 需 48k 立体声）

设备侧播放:
  adb push beep.wav /tmp/beep.wav
  adb shell "tinyplay /tmp/beep.wav -D 0"    # card0：本板有声音的那块
  adb shell "tinyplay /tmp/beep.wav -D 1"    # card1：ES714X（本板未接喇叭 → 静音）
注意 tinyplay 的 -D 必须写在文件名之后，且 hw:1,0 只接受 48kHz/立体声。
"""
import math
import struct
import sys
import wave


def main():
    freq = float(sys.argv[1]) if len(sys.argv) > 1 else 440.0
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
    out = sys.argv[3] if len(sys.argv) > 3 else "tone.wav"
    rate = int(sys.argv[4]) if len(sys.argv) > 4 else 22050
    ch = int(sys.argv[5]) if len(sys.argv) > 5 else 1

    n = int(rate * dur)
    # 峰值 20000/32767 ≈ 61% FS，够响又不削顶
    frames = bytearray()
    for i in range(n):
        v = int(20000 * math.sin(2 * math.pi * freq * i / rate))
        frames += struct.pack("<h", v) * ch

    w = wave.open(out, "wb")
    w.setnchannels(ch)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(bytes(frames))
    w.close()
    print("已生成 %s：%.0fHz / %.1fs / %dHz / %dch（%d 字节）"
          % (out, freq, dur, rate, ch, len(frames)))


if __name__ == "__main__":
    sys.exit(main())
