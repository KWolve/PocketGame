/*
 * uart_termios.c —— btstack 用的 termios 串口实现（V85X 板上接 BT 模块用）
 *
 * 为什么不用 btstack 自带的 `btstack_uart_posix_instance()`：
 *   1) **Realtek RTL8733BS 的 BT 串口是 8E1（偶校验）**，POSIX 实现只做 cfmakeraw
 *      （8N1），且没有对外接口能改；
 *   2) 需要"先 115200 初始化、成功后切 1500000"（Realtek 的 hciattach 流程如此），
 *      RTS/CTS 也要可选；
 *   3) 读取用 **run loop data source**（把 fd 注册进 btstack run loop）→ 回调都在
 *      run loop 线程里执行，符合 btstack 的硬规矩。
 *
 * 参考 btstack_uart.h 的 btstack_uart_t；sleep / H5 frame 相关一律留 NULL
 * （H4 不需要；H5 在 uart 没提供 frame 接口时会用自己的 SLIP）。
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "btstack/btstack_uart.h"
#include "btstack/btstack_run_loop.h"

#ifndef B1500000
#define B1500000 0010012
#endif
#ifndef B3000000
#define B3000000 0010014
#endif

static const char *sDev = "/dev/ttyS2";
static int sFd = -1;
static uint32_t sBaud = 115200;
static int sFlow = 0;
static int sParity = BTSTACK_UART_PARITY_OFF;

static void (*sBlockReceived)(void) = 0;
static void (*sBlockSent)(void) = 0;

static uint8_t *sRxBuf = 0;
static uint16_t sRxLen = 0;
static uint16_t sRxGot = 0;
static btstack_data_source_t sRxDs;
static int sRxDsAdded = 0;
static int sRxDsArmed = 0;

/* ---------- H5(SLIP) 帧模式 ----------
 * ⚠️ btstack 的 H5 传输会调用 uart 的 frame 级接口（set_frame_received / send_frame …）。
 *    这些指针留 NULL 会**空指针段错误**（实测：H4 正常、H5 一上电就 segfault）。
 *    所以这里自己实现标准 SLIP：0xC0 帧界，0xDB 转义（0xDB 0xDC=0xC0，0xDB 0xDD=0xDB）。 */
#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD
#define FRAME_MAX 1600

static void (*sFrameReceived)(uint16_t frame_size) = 0;
static void (*sFrameSent)(void) = 0;
static uint8_t sFrameBuf[FRAME_MAX];
static uint16_t sFrameLen = 0;
static int sFrameEsc = 0;
static int sFrameMode = 0;  // 1 = 走 frame 模式（H5）
static uint8_t *sFrameOut = 0;   // 收满后拷到这里（btstack 的接收缓冲）
static uint16_t sFrameOutLen = 0;

static int applyTermios(void);

static void rxArm(int on) {
  if (!sRxDsAdded || on == sRxDsArmed) return;
  if (on) btstack_run_loop_enable_data_source_callbacks(&sRxDs, DATA_SOURCE_CALLBACK_READ);
  else    btstack_run_loop_disable_data_source_callbacks(&sRxDs, DATA_SOURCE_CALLBACK_READ);
  sRxDsArmed = on;
}

/* fd 可读 → 把"本次要收的 block"补齐；frame 模式下则喂 SLIP 解码器 */
static void rxDataSourceHandler(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
  (void)ds;
  if (type != DATA_SOURCE_CALLBACK_READ) return;

  if (sFrameMode) {
    uint8_t tmp[256];
    int n = (int)read(sFd, tmp, sizeof(tmp));
    if (n <= 0) return;
    for (int i = 0; i < n; ++i) {
      uint8_t b = tmp[i];
      if (b == SLIP_END) {
        if (sFrameLen > 0) {
          uint16_t cp = sFrameLen < sFrameOutLen ? sFrameLen : sFrameOutLen;
          if (sFrameOut) memcpy(sFrameOut, sFrameBuf, cp);
          sFrameLen = 0;
          sFrameEsc = 0;
          rxArm(0);
          if (sFrameReceived) sFrameReceived(cp);
          return;
        }
        continue;
      }
      if (b == SLIP_ESC) { sFrameEsc = 1; continue; }
      if (sFrameEsc) {
        b = (b == SLIP_ESC_END) ? SLIP_END : (b == SLIP_ESC_ESC) ? SLIP_ESC : b;
        sFrameEsc = 0;
      }
      if (sFrameLen < FRAME_MAX) sFrameBuf[sFrameLen++] = b;
    }
    return;
  }

  if (!sRxBuf || sRxGot >= sRxLen) return;
  int n = (int)read(sFd, sRxBuf + sRxGot, sRxLen - sRxGot);
  if (n <= 0) return;  // EAGAIN：等下次事件
  sRxGot = (uint16_t)(sRxGot + n);
  if (sRxGot >= sRxLen) {
    sRxBuf = 0; sRxLen = 0; sRxGot = 0;
    rxArm(0);
    if (sBlockReceived) sBlockReceived();
  }
}

/* ---------- btstack_uart_t ---------- */
static int uartInit(const btstack_uart_config_t *cfg) {
  if (cfg->device_name) sDev = cfg->device_name;
  sBaud = cfg->baudrate;
  sFlow = cfg->flowcontrol;
  sParity = cfg->parity;
  printf("[UART] init dev=%s baud=%u flow=%d parity=%d\n", sDev, sBaud, sFlow, sParity);
  fflush(stdout);
  return 0;
}

static speed_t baudConst(uint32_t b) {
  switch (b) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 3000000: return B3000000;
    default: return B115200;
  }
}

static int applyTermios(void) {
  struct termios tty;
  if (sFd < 0) return -1;
  if (tcgetattr(sFd, &tty) != 0) return -1;
  cfmakeraw(&tty);
  tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
#ifdef CRTSCTS
  tty.c_cflag &= ~CRTSCTS;
#endif
  tty.c_cflag |= (CS8 | CLOCAL | CREAD);
  if (sParity == BTSTACK_UART_PARITY_EVEN) tty.c_cflag |= PARENB;               // 8E1
  if (sParity == BTSTACK_UART_PARITY_ODD) tty.c_cflag |= (PARENB | PARODD);     // 8O1
#ifdef CRTSCTS
  if (sFlow) tty.c_cflag |= CRTSCTS;
#endif
  cfsetispeed(&tty, baudConst(sBaud));
  cfsetospeed(&tty, baudConst(sBaud));
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;
  return tcsetattr(sFd, TCSANOW, &tty);
}

static int uartOpen(void) {
  sFd = open(sDev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (sFd < 0) {
    printf("[UART] open %s 失败: %s\n", sDev, strerror(errno));
    fflush(stdout);
    return -1;
  }
  if (applyTermios() != 0) {
    printf("[UART] 配置 termios 失败: %s\n", strerror(errno));
    fflush(stdout);
    return -1;
  }
  sRxDs.source.fd = sFd;
  btstack_run_loop_set_data_source_handler(&sRxDs, &rxDataSourceHandler);
  btstack_run_loop_add_data_source(&sRxDs);
  sRxDsAdded = 1;
  rxArm(0);
  printf("[UART] 打开 %s @ %u%s%s\n", sDev, sBaud, sFlow ? " RTS/CTS" : " 无流控",
         sParity == BTSTACK_UART_PARITY_EVEN ? " 偶校验(8E1)" :
         sParity == BTSTACK_UART_PARITY_ODD  ? " 奇校验(8O1)" : " 无校验(8N1)");
  fflush(stdout);
  return 0;
}

static int uartClose(void) {
  rxArm(0);
  if (sRxDsAdded) {
    btstack_run_loop_remove_data_source(&sRxDs);
    sRxDsAdded = 0;
  }
  if (sFd >= 0) close(sFd);
  sFd = -1;
  return 0;
}

static void uartSetBlockReceived(void (*h)(void)) { sBlockReceived = h; }
static void uartSetBlockSent(void (*h)(void)) { sBlockSent = h; }

static int uartSetBaudrate(uint32_t baud) {
  sBaud = baud;
  int r = applyTermios();
  printf("[UART] 切波特率 -> %u (%s)\n", baud, r == 0 ? "ok" : "失败");
  fflush(stdout);
  return r;
}

static int uartSetParity(int parity) { sParity = parity; return applyTermios(); }
static int uartSetFlowcontrol(int fc) { sFlow = fc; return applyTermios(); }

static void uartReceiveBlock(uint8_t *buffer, uint16_t len) {
  sRxBuf = buffer;
  sRxLen = len;
  sRxGot = 0;
  int n = (int)read(sFd, buffer, len);
  if (n > 0) sRxGot = (uint16_t)n;
  if (sRxGot >= len) {
    sRxBuf = 0; sRxLen = 0; sRxGot = 0;
    if (sBlockReceived) sBlockReceived();
    return;
  }
  rxArm(1);  // 剩余字节等 data source 事件
}

static void uartSendBlock(const uint8_t *buffer, uint16_t length) {
  int written = 0;
  while (written < (int)length) {
    int n = (int)write(sFd, buffer + written, length - written);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    written += n;
  }
  if (sBlockSent) sBlockSent();
}

static int uartGetSleepModes(void) { return 0; }

/* ---------- H5 frame 接口（SLIP） ---------- */
static void uartSetFrameReceived(void (*h)(uint16_t)) { sFrameReceived = h; sFrameMode = 1; }
static void uartSetFrameSent(void (*h)(void)) { sFrameSent = h; sFrameMode = 1; }

static void uartReceiveFrame(uint8_t *buffer, uint16_t len) {
  sFrameOut = buffer;
  sFrameOutLen = len;
  sFrameLen = 0;
  sFrameEsc = 0;
  // 先尽力立刻读一把（数据可能已经躺在 tty 缓冲里）
  rxDataSourceHandler(&sRxDs, DATA_SOURCE_CALLBACK_READ);
  if (sFrameOut && sFrameLen == 0) rxArm(1);  // 还没凑齐 → 等事件
  else rxArm(1);
}

static void uartSendFrame(const uint8_t *buffer, uint16_t length) {
  uint8_t out[FRAME_MAX + 8];
  uint16_t o = 0;
  out[o++] = SLIP_END;
  for (uint16_t i = 0; i < length && o < sizeof(out) - 2; ++i) {
    uint8_t b = buffer[i];
    if (b == SLIP_END) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_END; }
    else if (b == SLIP_ESC) { out[o++] = SLIP_ESC; out[o++] = SLIP_ESC_ESC; }
    else out[o++] = b;
  }
  out[o++] = SLIP_END;
  int written = 0;
  while (written < (int)o) {
    int n = (int)write(sFd, out + written, o - written);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      break;
    }
    written += n;
  }
  if (sFrameSent) sFrameSent();
}

static const btstack_uart_t kUartTermios = {
    uartInit, uartOpen, uartClose,
    uartSetBlockReceived, uartSetBlockSent,
    uartSetBaudrate, uartSetParity, uartSetFlowcontrol,
    uartReceiveBlock, uartSendBlock,
    uartGetSleepModes,
    0, 0,        // set_sleep / set_wakeup_handler
    uartSetFrameReceived, uartSetFrameSent, uartReceiveFrame, uartSendFrame
};

const btstack_uart_t *btstack_uart_termios_instance(void) { return &kUartTermios; }
