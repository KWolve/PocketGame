/*
 * pginj.c - PocketGame 真机输入联调工具（按键 + 触摸注入）
 *
 * 用途：在没有真人手指的情况下验证「3 个物理按键」与「触摸」是否真的到达应用，
 *       也可作为产线/QA 的自动化输入源。
 *
 * 用法:
 *   pginj key   <dev> <keycode> <hold_ms>
 *   pginj sweep <dev> <from> <to> [hold_ms]        # 扫描键码区间（找真实按键码）
 *   pginj tap   <dev> <x> <y> [hold_ms] [tid] [toolfinger]
 *   pginj press <dev> <x> <y> <hold_ms>            # 长按（应用层判 >=700ms）
 *   pginj swipe <dev> <x1> <y1> <x2> <y2> [steps] [每步ms] [toolfinger]
 *   pginj proto <dev>                              # 打印触摸协议（MT-A / MT-B / 单点）
 *   pginj raw   <dev> <type>:<code>:<value> ...     # 逐条注入
 *
 * 示例:
 *   pginj key   /dev/input/event3 103 80
 *   pginj sweep /dev/input/event3 1 255 25
 *   pginj proto /dev/input/event4
 *   pginj tap   /dev/input/event4 58 160
 *   pginj press /dev/input/event4 240 400 900
 *   pginj swipe /dev/input/event4 240 600 240 250 30 12
 *
 * 交叉编译（V85X）:
 *   arm-unknown-linux-musleabihf-gcc -static -O2 -o pginj tools/pginj.c
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

static int g_fd = -1;

static void send(int type, int code, int value) {
  struct input_event e;
  memset(&e, 0, sizeof(e));
  gettimeofday(&e.time, 0);
  e.type = (unsigned short)type;
  e.code = (unsigned short)code;
  e.value = value;
  if (write(g_fd, &e, sizeof(e)) != (ssize_t)sizeof(e)) perror("write");
}

static void syn(void) { send(EV_SYN, SYN_REPORT, 0); }

/* type-A 的点位分隔符（协议 B 不需要，见下面铁律①） */
static void synMt(void) { send(EV_SYN, SYN_MT_REPORT, 0); }

/* ─────────────── 触摸注入（2026-09-14 打通，见 docs/touch-inject.md）───────────────
 *
 * 本板 panel（`axs_ts`，event4）是 **MT 协议 B**：能力位里有
 *   ABS_MT_SLOT(47) / TOUCH_MAJOR(48) / POSITION_X(53) / POSITION_Y(54) /
 *   TRACKING_ID(57) / MT_PRESSURE(58)，而 ABS_X/ABS_Y 的 range 是 **0..0**
 *   ⇒ 单点那套（ABS_X/Y + ABS_PRESSURE）根本写不进坐标，必须走 MT。
 *
 * 三条实测铁律（都踩过）：
 *   ① **MT-B 绝不能发 SYN_MT_REPORT**（那是 type-A 的分隔符）。发了 → 整帧被丢弃，
 *      表现为"内核 dd 能抓到事件、框架完全没反应"（旧版 pginj / mt_test 就栽在这）。
 *   ② **BTN_TOUCH 必须和位置在同一帧里**。只发 MT 位置 → 框架只给 DOWN+MOVE，
 *      **永远不给 UP**（它会一直以为手指按着），于是点击判定不成立、后续注入全退化成 MOVE。
 *   ③ 抬起帧要带 TRACKING_ID=-1（MT-B 的"手指离开"语义），BTN_TOUCH=0 同帧。
 *
 * 框架 MotionEvent 值：NONE=0 / DOWN=1 / UP=2 / MOVE=3 / CANCEL=4。
 * 判定注入有没有生效：`logcat` 里应用会打 `PocketGame touch: action=1(按下) → 2(抬起)`。
 */

static int g_mt_b = 0;          /* MT 协议 B（有 ABS_MT_SLOT） */
static int g_mt = 0;            /* 有多点能力 */
static int g_single = 0;        /* 有可用的 ABS_X/ABS_Y */
static int g_mt_pressure = 0;   /* 有 ABS_MT_PRESSURE */
static int g_mt_major = 0;      /* 有 ABS_MT_TOUCH_MAJOR */

static void probe(void) {
  unsigned long bits[(ABS_MAX / (8 * sizeof(long))) + 1];
  memset(bits, 0, sizeof(bits));
  if (ioctl(g_fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) >= 0) {
#define A(q) ((bits[(q) / (8 * sizeof(long))] >> ((q) % (8 * sizeof(long)))) & 1UL)
    g_mt_b = A(ABS_MT_SLOT);
    g_mt = A(ABS_MT_POSITION_X) || A(ABS_MT_POSITION_Y);
    g_mt_pressure = A(ABS_MT_PRESSURE);
    g_mt_major = A(ABS_MT_TOUCH_MAJOR);
#undef A
  }
  struct input_absinfo ai;
  memset(&ai, 0, sizeof(ai));
  if (ioctl(g_fd, EVIOCGABS(ABS_X), &ai) >= 0 && ai.maximum > ai.minimum) g_single = 1;
  if (!g_mt && !g_single) {
    /* ioctl 打不开（只写 fd 上有些驱动会失败）：默认按本板 MT-B 走 */
    g_mt = 1;
    g_mt_b = 1;
    g_mt_pressure = 1;
    g_mt_major = 1;
  }
}

/* type-A 才需要的"点位分隔符"；协议 B 发它会把整帧作废 */
static void synMtIfTypeA(void) {
  if (g_mt && !g_mt_b) synMt();
}

static void down(int x, int y, int tid, int toolfinger) {
  if (g_mt) {
    if (g_mt_b) {
      send(EV_ABS, ABS_MT_SLOT, 0);
      send(EV_ABS, ABS_MT_TRACKING_ID, tid);
    }
    if (g_mt_major) send(EV_ABS, ABS_MT_TOUCH_MAJOR, 60);
    if (g_mt_pressure) send(EV_ABS, ABS_MT_PRESSURE, 255);
    send(EV_ABS, ABS_MT_POSITION_X, x);
    send(EV_ABS, ABS_MT_POSITION_Y, y);
    synMtIfTypeA();
    if (toolfinger) send(EV_KEY, BTN_TOOL_FINGER, 1);
    send(EV_KEY, BTN_TOUCH, 1);   /* ★ 铁律②：与位置同帧 */
    syn();
    return;
  }
  send(EV_ABS, ABS_X, x);
  send(EV_ABS, ABS_Y, y);
  send(EV_ABS, ABS_PRESSURE, 255);
  send(EV_KEY, BTN_TOUCH, 1);
  syn();
}

static void move(int x, int y) {
  if (g_mt) {
    send(EV_ABS, ABS_MT_POSITION_X, x);
    send(EV_ABS, ABS_MT_POSITION_Y, y);
    synMtIfTypeA();
    syn();
    return;
  }
  send(EV_ABS, ABS_X, x);
  send(EV_ABS, ABS_Y, y);
  syn();
}

static void up(int toolfinger) {
  if (g_mt) {
    if (g_mt_b) {
      send(EV_ABS, ABS_MT_SLOT, 0);
      send(EV_ABS, ABS_MT_TRACKING_ID, -1);   /* ★ 铁律③：手指离开 */
    }
    if (g_mt_major) send(EV_ABS, ABS_MT_TOUCH_MAJOR, 0);
    if (g_mt_pressure) send(EV_ABS, ABS_MT_PRESSURE, 0);
    synMtIfTypeA();
    send(EV_KEY, BTN_TOUCH, 0);
    if (toolfinger) send(EV_KEY, BTN_TOOL_FINGER, 0);
    syn();
    return;
  }
  send(EV_ABS, ABS_PRESSURE, 0);
  send(EV_KEY, BTN_TOUCH, 0);
  syn();
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage:\n"
            "  %s key   <dev> <keycode> <hold_ms>\n"
            "  %s tap   <dev> <x> <y> [hold_ms] [tid] [toolfinger]\n"
            "  %s press <dev> <x> <y> <hold_ms>          # 长按（应用层判 >=700ms）\n"
            "  %s swipe <dev> <x1> <y1> <x2> <y2> [steps] [每步ms] [toolfinger]\n"
            "  %s sweep <dev> <from> <to> [hold_ms]      # 扫键码区间（找真实按键码）\n"
            "  %s raw   <dev> <type>:<code>:<value> ...\n"
            "  %s proto <dev>                            # 打印触摸协议（A/B/单点）\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 1;
  }
  const char *mode = argv[1];
  const char *dev = argv[2];
  g_fd = open(dev, O_RDWR);
  if (g_fd < 0) g_fd = open(dev, O_WRONLY);
  if (g_fd < 0) {
    perror("open");
    return 2;
  }
  probe();

  if (strcmp(mode, "proto") == 0) {
    printf("%s -> %s（mt=%d mt_b=%d single=%d mt_pressure=%d mt_major=%d）\n", dev,
           g_mt_b ? "MT 协议 B" : (g_mt ? "MT 协议 A" : (g_single ? "单点 ABS_X/Y" : "未知")),
           g_mt, g_mt_b, g_single, g_mt_pressure, g_mt_major);
  } else if (strcmp(mode, "key") == 0 && argc >= 5) {
    int code = atoi(argv[3]);
    int hold = atoi(argv[4]);
    send(EV_KEY, code, 1);
    syn();
    if (hold > 0) usleep(hold * 1000);
    send(EV_KEY, code, 0);
    syn();
    printf("key %d on %s hold=%dms\n", code, dev, hold);
  } else if (strcmp(mode, "tap") == 0 && argc >= 5) {
    int x = atoi(argv[3]), y = atoi(argv[4]);
    int hold = (argc >= 6) ? atoi(argv[5]) : 60;
    int tid = (argc >= 7) ? atoi(argv[6]) : 1;
    int tf = (argc >= 8) ? atoi(argv[7]) : 0;
    down(x, y, tid, tf);
    usleep(hold * 1000);
    up(tf);
    printf("tap (%d,%d) dev=%s hold=%dms tid=%d toolfinger=%d proto=%s\n", x, y, dev,
           hold, tid, tf, g_mt_b ? "MT-B" : (g_mt ? "MT-A" : "单点"));
  } else if (strcmp(mode, "press") == 0 && argc >= 6) {
    int x = atoi(argv[3]), y = atoi(argv[4]), hold = atoi(argv[5]);
    down(x, y, 1, 0);
    usleep(hold * 1000);
    up(0);
    printf("press (%d,%d) %dms dev=%s\n", x, y, hold, dev);
  } else if (strcmp(mode, "swipe") == 0 && argc >= 7) {
    int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
    int x2 = atoi(argv[5]), y2 = atoi(argv[6]);
    int steps = (argc >= 8) ? atoi(argv[7]) : 20;
    int step_ms = (argc >= 9) ? atoi(argv[8]) : 12;
    int tf = (argc >= 10) ? atoi(argv[9]) : 0;
    if (steps < 2) steps = 2;
    down(x1, y1, 1, tf);
    usleep(40 * 1000);
    for (int i = 1; i <= steps; ++i) {
      int x = x1 + (x2 - x1) * i / steps;
      int y = y1 + (y2 - y1) * i / steps;
      move(x, y);
      if (step_ms > 0) usleep(step_ms * 1000);
    }
    usleep(40 * 1000);
    up(tf);
    printf("swipe (%d,%d)->(%d,%d) dev=%s steps=%d step=%dms tf=%d\n", x1, y1, x2,
           y2, dev, steps, step_ms, tf);
  } else if (strcmp(mode, "sweep") == 0 && argc >= 5) {
    int from = atoi(argv[3]);
    int to = atoi(argv[4]);
    int hold = (argc >= 6) ? atoi(argv[5]) : 25;
    if (hold < 5) hold = 5;
    printf("sweep %d..%d on %s (hold %dms)\n", from, to, dev, hold);
    for (int code = from; code <= to; ++code) {
      send(EV_KEY, code, 1);
      syn();
      usleep(hold * 1000);
      send(EV_KEY, code, 0);
      syn();
      usleep(hold * 1000);
    }
    printf("sweep done\n");
  } else if (strcmp(mode, "raw") == 0) {
    for (int i = 3; i < argc; ++i) {
      int t, c, v;
      if (sscanf(argv[i], "%d:%d:%d", &t, &c, &v) == 3) send(t, c, v);
    }
    syn();
    printf("raw injected\n");
  } else {
    fprintf(stderr, "bad args\n");
    close(g_fd);
    return 1;
  }

  close(g_fd);
  return 0;
}
