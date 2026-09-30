# src/dependencies —— 厂商 SDK 与第三方库（**本仓库不附带**）

本目录在原工程里存放两类东西：

1. **全志 V85X 平台的 SDK 头文件与库**（`mpi_*.h` / `aw_*.h` / `cedarx` / `isp` / `v4l2` …）
2. **第三方库**（ffmpeg 静态库、OpenSSL 1.1 动态库、本项目构建的 `libawh264player.so`）

为了让仓库保持「纯源码」，**这些二进制与厂商私有头文件都没有提交**。
它们不属于本项目，且分发许可以厂商 SDK 协议为准。缺失的后果是：**克隆后无法直接编译**，
需要先按 [`docs/DEPENDENCIES.md`](../../docs/DEPENDENCIES.md) 把依赖补齐。

补齐后本目录应该是这个样子（文件大小仅供核对是否拿对了东西）：

```
src/dependencies/
├── include/                      # 编译期头文件（约 3.5 MB）
│   ├── mpi_*.h  mm_comm_*.h      # 全志媒体中间层 MPI（VI/VO/VENC/VDEC/AI/AO…）
│   ├── aw_*.h   adec_*.h  aenc_*.h
│   ├── cedarx_*.h  vdecoder.h  vencoder.h   # CedarX 编解码
│   ├── isp*.h  v4l2-*.h  videodev2.h        # 摄像头 / ISP
│   ├── sunxi_camera_v2.h  uvcInput.h  h264_player.h
│   ├── wifi_sta.h  wifi_ap.h  smartlink.h   # 无线
│   ├── bt_*.h  bluetooth.h  bt_sock.h       # BlueZ / btstack
│   └── libavcodec/ libavformat/ libavutil/ libswresample/ ...   # ffmpeg 头
├── lib/                          # 链接期静态/动态库（约 5.2 MB）
│   ├── libavcodec.a  libavformat.a  libavutil.a  libswresample.a
│   ├── libzkmedia.a              # 全志 zk 媒体静态库
│   └── libcrypto.so.1.1  libssl.so.1.1          # https（dlna / ha / iptv 用）
├── lib-no-link/                  # 不参与链接、但会被打进固件包的库
│   └── libawh264player.so        # zk_h264_player 的实现（约 21 KB）
└── bin/
    └── firmware/rtlbt/           # 蓝牙固件，会被打进 /res/bin/firmware/
        ├── rtl8733bs_fw
        └── rtl8733bs_config
```

> ⚠️ `lib-no-link/` 的语义：**不进链接命令行，但 `fun pack` 会把它拷进固件包的 `/res/lib/`**。
> 设备的 `LD_LIBRARY_PATH` 本来就含 `/res/lib`，所以固化之后不需要手工推送；
> 而 `fun launch`（临时调试）**不会**推送它 —— 这是排查「调试能跑、固化不能跑」类问题的关键。
