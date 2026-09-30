/*
 * pgkey.c —— 物理按键注入（QA / 免手指自测用）
 *
 * 往 gpio-keys 的 evdev 节点写一次「按下 + 抬起」，用来验证按键映射是否生效
 * （不依赖手按，也不依赖框架的触摸注入工具）。
 *
 * 用法：
 *   pgkey <键码> [hold_ms] [设备节点]      例：pgkey 103            # 注入 KEY_UP
 *                                              pgkey 108 1500        # 按住 1.5s（触发长按）
 *
 * 本板键码（2026-09-12 语义重定义后）：
 *   103 -> 音量 -      105 -> 音量 +      108 -> 暂停 / 继续（长按 = 返回列表）
 *
 * ⚠️ 长按（E_KEY_LONG_PRESS）必须模拟**内核 autorepeat**：按住期间周期性发
 *    `EV_KEY code 2`（repeat）+ SYN。只发 1/0 两次的话，框架只会给一个 E_KEY_DOWN
 *    （实测：按住 3 秒也只有 status=1）。
 *    ⚠️⚠️ 本板 gpio-keys **没有 autorepeat**（getevent -p 无 REP 能力位）——
 *    "真实按键由内核发 repeat" 的假设是错的，实体键长按框架根本收不到 repeat。
 *    所以应用层已在 KeyRouter 里按 DOWN->UP 时长自行判定长按（>=700ms），
 *    注入工具要测框架 repeat 路径才需要模拟 repeat；测应用层长按判定请用
 *    busybox sendevent（只发 down + SYN，隔 1 秒再发 up + SYN，不带 repeat）。
 *
 * 判据：注入后看应用日志 `PocketGame key: code=... -> ...`（status=2 即长按），
 *       音量键再用 `tinymix -D 0` 回读 digital volume 是否变化。
 *
 * 交叉编译（项目工具链）：
 *   /d/zkswe/fun/toolchains/v85x/bin/arm-unknown-linux-musleabihf-gcc.exe \
 *       -static -O2 -o pgkey tools/pgkey.c
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int gFd = -1;

static void sendEvent(int type, int code, int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof(ev));
  gettimeofday(&ev.time, NULL);  /* 时间戳必须填（内核依赖它排序） */
  ev.type = (unsigned short)type;
  ev.code = (unsigned short)code;
  ev.value = value;
  if (write(gFd, &ev, sizeof(ev)) < 0) perror("write");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <keycode> [hold_ms] [dev] [norepeat]\n", argv[1] == NULL ? "" : argv[0]);
    fprintf(stderr, "  hold_ms 默认 50；要触发框架的长按事件（如长按暂停键返回列表）给 1500 左右\n");
    fprintf(stderr, "  norepeat：不发 repeat 事件 —— 模拟本板实体键的真实形态（应用层\n");
    fprintf(stderr, "            按 DOWN->UP 时长判长按；本板 gpio-keys 无 autorepeat）\n");
    return 2;
  }
  int code = atoi(argv[1]);
  const char *dev = "/dev/input/event3";
  int holdMs = 50;
  int noRepeat = 0;
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] >= '0' && argv[i][0] <= '9') {
      holdMs = atoi(argv[i]);
      if (holdMs < 1) holdMs = 1;
    } else if (strcmp(argv[i], "norepeat") == 0) {
      noRepeat = 1;
    } else {
      dev = argv[i];
    }
  }

  gFd = open(dev, O_WRONLY);
  if (gFd < 0) {
    perror("open");
    return 1;
  }
  sendEvent(EV_KEY, code, 1);
  sendEvent(EV_SYN, SYN_REPORT, 0);
  /*
   * 按住 holdMs。⚠️ 必须周期性发 EV_KEY value=2（内核 autorepeat 的形态）——
   * 框架的「长按」就是靠它判定的；只发 1 和 0 的话按住再久也只得到 E_KEY_DOWN
   * （实测：按住 3 秒仍只有 status=1）。真实按键由内核驱动发 repeat，所以真机长按本来就好使。
   * 时序照内核默认：前 300ms 不发，之后每 100ms 一次。
   */
  {
    int t = 0;
    while (t < holdMs) {
      int step = 100;
      if (t + step > holdMs) step = holdMs - t;
      usleep((useconds_t)step * 1000);
      t += step;
      if (!noRepeat && t >= 300) {
        sendEvent(EV_KEY, code, 2);
        sendEvent(EV_SYN, SYN_REPORT, 0);
      }
    }
  }
  sendEvent(EV_KEY, code, 0);
  sendEvent(EV_SYN, SYN_REPORT, 0);
  close(gFd);

  printf("injected key code=%d hold=%dms%s -> %s\n", code, holdMs,
         noRepeat ? " norepeat" : "", dev);
  return 0;
}
