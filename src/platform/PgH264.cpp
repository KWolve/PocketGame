/*
 * PgH264.cpp - 硬件 H264 直接解码实现。设计说明见 PgH264.h。
 */
#include "platform/PgH264.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>

#include "h264_player.h"         // zk_h264_player_*（实现在 libzkmedia.a 的 wrapper 里）
                                 // ⚠️ 直接写文件名：它放在 src/dependencies/include/ 下，
                                 // 那个目录本身就在 -I 里（写成 media/h264_player.h 会找不到）
#include "platform/PgGrab.h"          // 抓帧：HW 路的钩子（见 PgGrab.h 的说明）
#include "platform/PgMem.h"          // 起播前清页缓存（drop_caches，见 PgMem.h）
#include "platform/PgVideoLayer.h"   // 停止后释放 disp 层（见 stop() 里的说明）
#include "utils/Log.h"

namespace pg {

namespace {

/* ==================== zkmedia 环境变量（★ 必须在第一次 dlopen 之前设）====================
 * 参考工程在 `src/media/audio_context.cpp` 的 `init()` 里设了这一组，本工程原来**一个都没设**
 * —— 这是 2026-09-14 定位"720p + 旋转起播静默崩"时发现的差异。
 *
 *   ZKMEDIA_H264_VBVSIZE = 码流缓冲（VBV）大小，默认值偏小。
 *     720p 的 I 帧实测能到几百 KB（萧山 720p@1.5Mbps），喂进太小的缓冲会出事。
 *     参考工程用 **1048576（1MB）**，旁边还留着 2MB / 3MB 的档位注释。
 *
 * ⚠️ 用 overwrite=0：外部（init.rc / QA 里 export）已经设过的值优先，方便现场调参。
 * ⚠️ 时机：libawh264player.so 是在 `zk_h264_player_preload()` 里 dlopen 的，
 *    库在 dlopen 时读环境变量 —— 所以这里必须在 preload **之前**调，故用一次性 guard。 */
int sVbvBytes = 1048576;   // 可由 PgStore 覆盖（mainLogic 启动时灌进来；QA `vbv` 可改）

void ensureZkmediaEnv() {
  static bool done = false;
  if (done) return;
  done = true;
  char v[32];
  snprintf(v, sizeof(v), "%d", sVbvBytes);
  setenv("ZKMEDIA_H264_VBVSIZE", v, 0);   // 0 = 外部（init.rc/export）已设的不覆盖
  LOGD("PgH264: zkmedia 环境 —— ZKMEDIA_H264_VBVSIZE=%s（本工程默认 1MB；外部已设则不覆盖）",
       getenv("ZKMEDIA_H264_VBVSIZE") ? getenv("ZKMEDIA_H264_VBVSIZE") : "(null)");
}

/* ==================== init 的 flag 组装（**唯一来源**）====================
 * 原来 playFile 与 startStream 各抄一份（两处 4 行一模一样），加一个 flag 就要改两遍
 * —— 已踩过同类坑（"同一份东西两处手抄必然漂"，见 docs/ui-design-baseline.md）。
 *
 * 位含义（src/dependencies/include/h264_player.h）：
 *   E_H264_PLAYER_FLAG_STREAM_EOF    = 0x01  流模式（参考工程当默认值用）
 *   E_H264_PLAYER_FLAG_DISP_UNCACHE  = 0x02  ✗ **本板已放弃**，别加（见下）
 *   E_H264_PLAYER_FLAG_SCALE_DOWN_2  = 0x10  1/2 缩放解码
 *   E_H264_PLAYER_FLAG_SCALE_DOWN_4  = 0x20  1/4 缩放解码
 *
 * ============================================================================
 * ✗✗ `E_H264_PLAYER_FLAG_DISP_UNCACHE`：**本板放弃这条优化**（2026-09-14 定案）
 * ============================================================================
 * 用户建议过（参考工程 AirPlay 用了它：`mark_cv201/CV201_PND/src/link/context.cpp:261`，
 * rot 传在 init），确实能省内存，但**代价是画面废掉**，且本板无退路：
 *
 * | 配置（同频道 720p@1/2、固化、同板） | MemAvailable | VmRSS | RssAnon | 画面 |
 * |---|---|---|---|---|
 * | 无 UNCACHE（STREAM_EOF\|SCALE_2） | 8.6MB | 19064 | 13280 | ✅ 铺满 frame[43,0,393,700] |
 * | UNCACHE（set_rot 后下发 / rot 传 init 都试过） | **14.4MB** | **14852** | **8128** | ❌ 只填 y≈417、下方全黑 |
 *
 * **机制（用户点明，这是本质）**：UNCACHE = **VDEC 不经 DISP 直出到显示器**（绕开 DISP 的
 * 缓冲、合成与拷贝）。而**旋转正是 DISP 在做的事** —— 不走 DISP，就**没有旋转**。
 *   · 非 UNCACHE：VDEC → DISP 缓冲（**拷贝时顺带做旋转**）→ 显示器；
 *   · UNCACHE：  VDEC 缓冲 → 显示器（零拷贝；省下的正是那份显示缓冲 ≈5.8MB，**但没有旋转**）。
 * disp 层铁证与此吻合：无 UNCACHE `fb[360,640] crop[0,0,360,640]`（缓冲已旋转、与 crop 自洽）；
 * 有 UNCACHE `fb[640,384] crop[0,0,360,640]`（缓冲**未旋转**、与 crop 不自洽）⇒ 可见高度
 * 416/700 = 0.594 ≈ 384/640，disp 拿"旋转后的 crop（高 640）"去读"高只有 384 的未旋转缓冲"，
 * 超出的 40% 全黑。
 *
 * **为什么本板没有退路**：屏幕 480x800 **竖屏**，而 IPTV / 投屏内容绝大多数是横屏 ⇒
 * 必须转 90°。拿旋转换内存不划算、也没得换。**除非将来换到"旋转 + 直显"都支持的库版本**，
 * 才值得重开（届时要满足两条判据：画面铺满 `frame[43,0,393,700]` + 与正常渲染逐像素一致）。
 *
 * ⇒ 所以这个 flag **不再出现在本工程的 flag 组装里**（不留开关，免得以后有人顺手打开）。
 * ============================================================================
 */
int h264Flags(int scaleDown) {
  int flag = E_H264_PLAYER_FLAG_STREAM_EOF;
  if (scaleDown == 2) flag |= E_H264_PLAYER_FLAG_SCALE_DOWN_2;
  else if (scaleDown == 4) flag |= E_H264_PLAYER_FLAG_SCALE_DOWN_4;
  return flag;
}

const int kFrameIntervalMs = 33;    // 喂帧节流（≈30fps）——没有 PTS 只能自己压
const int kMaxBufferedFrames = 8;   // 文件喂帧模式下等它的阈值（见 feedThread）
/* ⚠️⚠️⚠️ **`zk_h264_player_get_picture_count()` 不是"待显示队列长度"，而是"累计已提交帧数"**
 * —— 喂多少它就报多少（正常播放也一样涨）。这个语义前后踩了**三次**：
 *   · 2026-09-14 两次：拿它当"缓冲积压"判据 ⇒ 喂到阈值之后**每一帧都被当积压丢掉**，
 *     画面定格、日志只报"积压 N 帧"，看起来像喂太快，其实是自伤。
 *   · ★★ 2026-09-21 **真的把画面冻结在用户眼前**（长时间工作的现场）：
 *     新的 `loop=true` seek 循环**不再重建解码器**，于是这个"累计值"一路涨到 **100000**
 *     —— 15fps 下只是 **1.85 小时** —— 撞上当时那道"安全上限"后，**每一帧都被丢弃**
 *     （`drop` 一路涨到 4.5 万），解码器被彻底饿死：
 *       `PgH264: 解码器持有帧 100001（远超池大小，驱动异常？）→ 丢弃本次喂入（累计丢 37350）`
 *       `PgStream: [HW] 已喂 137400 包（硬件解码出帧 100001 …）`   ← 出帧数**冻结**
 *     现场表现 = **画面静帧**（抓帧 6 秒不落盘），而 `running`/`fileloops`/相位**照常推进**
 *     （"假活着"，只看状态文件根本看不出来）。
 * ⇒ **结论：绝不用它做背压/丢弃判据**（流式模式本来就不背压，节流交给调用方按 PTS 比）。
 *   `kPqLogLine` 只是个"涨到天文数字时打一条日志"的观测线，**不影响喂入**
 *   （它对 uint32 计数，15fps 要 9 年才溢出 —— 我们只播 10 秒的循环片，涨多少都无害）。 */
const int kPqLogLine = 500000;

struct State {
  bool running = false;
  bool inited = false;
  bool streamMode = false;    // ★ 流式（外部 feed）
  bool autoSize = false;      // init 时源尺寸未知（直播流常见）→ 等第一帧解码出来再定
  int srcW = 0, srcH = 0;     // 源分辨率（init 的参数）
  int scaleDown = 1;          // 1/2/4（FLAG_SCALE_DOWN_*）；缩放后缓冲 = 源/它
  int rot = 0;                // 顺时针角度（0/90/180/270）
  int posX = 0, posY = 0, posW = 0, posH = 0;          // 屏幕显示矩形
  int areaX = 0, areaY = 0, areaW = 480, areaH = 700;  // 可用显示区（autoSize 时按它算）
  int cropX = 0, cropY = 0, cropW = 0, cropH = 0;      // 源裁剪（旋转后坐标系）
  volatile int autoPosPending = 0;   // 解码回调发现真实尺寸 → 由喂帧线程下发（别在回调线程里调 API）
  int dropped = 0;
  int framesFed = 0;
  int totalUnits = 0;
  int pqLogged = 0;                 // "累计提交帧数"涨到观测线只记一次日志（见 kPqLogLine）
  volatile int framesDecoded = 0;   // 解码帧回调次数（= 解码器真的吐出画了吗）【进程累计】
  volatile int framesRun = 0;       // 本轮（自 armRun 起）的解码帧数，见 H264Player::armRun
  volatile int lastW = 0, lastH = 0;
  char desc[192] = {0};
  char err[160] = {0};
  pthread_t th = 0;
  uint8_t *data = 0;
  size_t size = 0;
  std::vector<size_t> units;   // 每个 access unit 的起始偏移（末尾放 size 作哨兵）
};

State s;

/* 角度 → 硬件枚举（顺时针语义一致，见 h264_player.h 的 disp_rot_e） */
disp_rot_e dispRotOf(int deg) {
  switch ((((deg % 360) + 360) % 360)) {
    case 90: return E_DISP_ROT_90;
    case 180: return E_DISP_ROT_180;
    case 270: return E_DISP_ROT_270;
    default: return E_DISP_ROT_0;
  }
}

/* 按当前角度算"旋转后"的帧尺寸（90/270 交换宽高） */
void rotatedSize(int srcW, int srcH, int deg, int *rw, int *rh) {
  int d = (((deg % 360) + 360) % 360);
  if (d == 90 || d == 270) { *rw = srcH; *rh = srcW; }
  else { *rw = srcW; *rh = srcH; }
}

/* 把裁剪参数下发给硬件（crop 坐标是**解码缓冲**口径 ⇒ 缩放解码时要除以倍率，
 * 这是参考工程 video_crop 的做法：`zk_h264_player_set_crop(x/scale, …)`）。
 * cropW <= 0 = 整帧（旋转后的尺寸）。 */
void applyCrop() {
  if (!s.inited) return;
  if (s.srcW <= 0 || s.srcH <= 0) return;   // 尺寸未知（直播流还没解出第一帧）→ 先不动
  int rw = 0, rh = 0;
  rotatedSize(s.srcW, s.srcH, s.rot, &rw, &rh);
  int sc = s.scaleDown ? s.scaleDown : 1;
  if (s.cropW > 0 && s.cropH > 0) {
    zk_h264_player_set_crop(s.cropX / sc, s.cropY / sc, s.cropW / sc, s.cropH / sc);
  } else {
    zk_h264_player_set_crop(0, 0, rw / sc, rh / sc);
  }
}

/* 直播流起播时**源尺寸常常还不知道**（我们的 HLS 中继从"最新写入位置"开始，
 * 第一个 SPS 要等下一个 I 帧），ffmpeg 报的 `codecpar->width/height` 就是 0。
 * 这时候按 0x0 去 init 是不行的 —— 但硬解自己解出第一帧就知道真实尺寸了，
 * 所以这里在**解码回调**里记下真实尺寸，把"重算显示区"的活交给喂帧线程做
 * （回调跑在播放器自己的显示线程里，不在那里调 set_pos/set_crop 更安全）。 */
void applyAutoSize() {
  if (!s.inited || !s.autoSize) return;
  int rw = 0, rh = 0;
  rotatedSize(s.srcW, s.srcH, s.rot, &rw, &rh);
  int x = 0, y = 0, w = 0, h = 0;
  H264Player::fitRect(s.srcW, s.srcH, s.rot, s.areaX, s.areaY, s.areaW, s.areaH, &x, &y, &w, &h);
  s.autoSize = false;
  zk_h264_player_set_pos(x, y, w, h);
  applyCrop();
  LOGD("PgH264: 直播流真实尺寸 %dx%d（fps 未知）→ 显示区 (%d,%d %dx%d)，旋转 %d° 后 %dx%d",
       s.srcW, s.srcH, x, y, w, h, s.rot, rw, rh);
}

/* 解码帧回调：**这是判断"解码器到底有没有在出画"的唯一硬证据**。
 * 光看 `get_picture_count()` 会误判 —— 它更像"已提交帧数"，喂多少它就报多少，
 * 哪怕显示线程根本没取走。只有这个回调响了，才说明硬解真的吐出了帧。 */
void onDecodedFrame(h264_decode_frame_t *f) {
  ++s.framesDecoded;
  ++s.framesRun;
  if (f) {
    s.lastW = f->right - f->left;
    s.lastH = f->bottom - f->top;
    /* ★ 起播时不知道源尺寸（直播流）→ 第一帧解出来就是真实尺寸，交给喂帧线程去下发。
     * 用 f->width/height（帧缓冲尺寸）优先，取不到才退回 crop 出来的宽高。
     * ⚠️ 这里**不能**直接调 set_pos/set_crop：本回调跑在播放器自己的显示线程里。 */
    if (s.autoSize && f->width > 0 && f->height > 0) {
      s.srcW = f->width;
      s.srcH = f->height;
      s.autoPosPending = 1;
    }
    if (s.framesDecoded == 1 || (s.framesDecoded % 60) == 0) {
      LOGD("PgH264: 解码回调 #%d —— fmt=%d %dx%d crop(%d,%d,%d,%d) data0=%p", s.framesDecoded,
           f->fmt, f->width, f->height, f->left, f->top, f->right, f->bottom,
           (void *)f->data0);
    }
    /* ★ 抓帧（QA）：**HW 路才是实际在跑的那条**，所以钩子必须挂在这里 ——
     * 只挂在 PgStream 自己的 SendFrame 前会"打了日志却永远不落盘"。
     * data0 是 Y 平面；解码缓冲按**对齐宽度**排（实测 426 宽的帧 width=448）⇒ stride 取 width。 */
    if (grabTake(s.framesDecoded)) {
      grabWritePlane((const unsigned char *)f->data0, f->width, f->height, (unsigned)f->width, "hw");
    }
  }
}

void sleepMs(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, 0);
}

void setErr(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s.err, sizeof(s.err), fmt, ap);
  va_end(ap);
  LOGE("PgH264: %s", s.err);
}

/* 扫描 start code，按 AUD（nal_type == 9）切 access unit。
 * 为什么按 AUD 切：H264 的 access unit 边界就是 AUD（有些流没有 AUD，那退化成
 * "按 start code 切"，解码器能容忍 —— 多喂几个包比漏喂安全）。 */
void splitUnits() {
  s.units.clear();
  const uint8_t *d = s.data;
  size_t n = s.size;
  for (size_t i = 0; i + 3 < n; ++i) {
    if (d[i] != 0 || d[i + 1] != 0) continue;
    size_t sc = 0;
    if (d[i + 2] == 1) sc = 3;
    else if (d[i + 2] == 0 && i + 4 < n && d[i + 3] == 1) sc = 4;
    if (!sc) continue;
    uint8_t nal = d[i + sc] & 0x1F;
    if (nal == 9) s.units.push_back(i);   // AUD = 新的一帧开始
    i += sc - 1;
  }
  /* 一个 AUD 都没有：退化成"整段一次性喂"（至少能看出解码器能不能吃） */
  if (s.units.size() < 2) {
    s.units.clear();
    s.units.push_back(0);
  }
  s.units.push_back(n);   // 哨兵
}

void *feedThread(void *) {
  LOGD("PgH264: 喂帧线程启动（%d 个 access unit，%d 字节）", s.totalUnits, (int)s.size);

  int n = (int)s.units.size() - 1;
  for (int i = 0; i < n && s.running; ++i) {
    size_t b = s.units[i], e = s.units[i + 1];
    /* 背压：解码器缓冲堆积时等一下（本板内存只有 55MB，塞爆就 OOM）。
     * ⚠️ 上限要小：原来写 guard<100 × 10ms = 每帧最多等 1 秒，
     * 而缓冲长期在阈值之上 ⇒ **每帧都等满 1 秒**，看起来像"喂不动了"。 */
    int guard = 0;
    while (s.running && zk_h264_player_get_picture_count() > kMaxBufferedFrames &&
           guard++ < 30) {
      sleepMs(5);
    }
    if (!s.running) break;

    zk_h264_player_put_frame(s.data + b, (uint32_t)(e - b));
    ++s.framesFed;

    if ((i % 60) == 0) {
      LOGD("PgH264: 已喂 %d/%d 帧，解码器待显示 %d 帧，需关键帧=%d", i, n,
           zk_h264_player_get_picture_count(), zk_h264_player_need_iframe());
    }
    sleepMs(kFrameIntervalMs);
  }

  if (s.running) {
    LOGD("PgH264: 文件喂完（%d 帧），等解码器排空…", s.framesFed);
    /* 喂完了再等一会儿，让显示线程把在途帧放完 */
    for (int i = 0; i < 40 && s.running; ++i) sleepMs(50);
    LOGD("PgH264: 结束（共喂 %d 帧）", s.framesFed);
  }
  s.running = false;
  return 0;
}

}  // namespace

void H264Player::setVbvBytes(int n) {
  if (n < 65536 || n > (64 << 20)) return;
  sVbvBytes = n;
}
int H264Player::vbvBytes() { return sVbvBytes; }

bool H264Player::playFile(const char *path, int srcW, int srcH, int scaleDown, int dispX,
                          int dispY, int dispW, int dispH) {
  if (s.running) stop();
  s.err[0] = 0;
  s.framesFed = 0;

  /* ★ 先把页缓存还回去（见 PgMem.h）：本板内存紧，等下要 malloc 整个 ES 文件
   * 再让解码器分配缓冲，进视频前清一次页缓存最划算。 */
  dropPageCache();

  /* ① 读文件（ES 就几 MB，整段进内存最简单；本板喂完即释放） */
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    setErr("打不开 %s", path);
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (sz <= 0) {
    fclose(fp);
    setErr("文件为空 %s", path);
    return false;
  }
  s.data = (uint8_t *)malloc((size_t)sz);
  if (!s.data) {
    fclose(fp);
    setErr("内存不足（要 %ld 字节，本板可用很少）", sz);
    return false;
  }
  s.size = (size_t)fread(s.data, 1, (size_t)sz, fp);
  fclose(fp);

  splitUnits();
  s.totalUnits = (int)s.units.size() - 1;
  if (s.totalUnits <= 0) {
    setErr("切不出 access unit");
    free(s.data);
    s.data = 0;
    return false;
  }

  /* ② 起解码器。
   *    ⚠️ 顺序：先 init → set_pos → show，与参考工程 context.cpp 一致。
   *    flag（STREAM_EOF | 缩放档）由 h264Flags() 统一给，见那儿的说明。 */
  int flag = h264Flags(scaleDown);

  ensureZkmediaEnv();
  zk_h264_player_preload();   // 预载（内部起线程 dlopen，重复调用无害）
  int r = zk_h264_player_init(srcW, srcH, E_DISP_ROT_0, flag);
  /* 注册解码回调 —— 用来证实"硬解真的出帧了"（见 onDecodedFrame 的说明）。
   * ⚠️ 必须在 init 之后（参考工程也是 init 完再 set_decode_cb）。 */
  zk_h264_player_set_decode_cb(onDecodedFrame);
  snprintf(s.desc, sizeof(s.desc),
           "zk_h264_player_init(%d,%d,rot=0,flag=0x%X[STREAM_EOF]) -> %d；显示区 (%d,%d %dx%d)",
           srcW, srcH, flag, r, dispX, dispY, dispW, dispH);
  LOGD("PgH264: %s", s.desc);
  if (r != 0) {
    setErr("init 失败（%d）—— 多半是 dlopen libawh264player.so 没成功（看上面 ffmpeg/loader 的报错）", r);
    free(s.data);
    s.data = 0;
    return false;
  }
  s.inited = true;

  zk_h264_player_set_pos(dispX, dispY, dispW, dispH);
  zk_h264_player_show();

  /* ③ 起喂帧线程 */
  s.running = true;
  if (pthread_create(&s.th, 0, feedThread, 0) != 0) {
    s.running = false;
    setErr("喂帧线程创建失败");
    zk_h264_player_deinit();
    s.inited = false;
    free(s.data);
    s.data = 0;
    return false;
  }
  pthread_detach(s.th);

  LOGD("PgH264: ★ 已起播 %s（源 %dx%d，缩放 1/%d）", path, srcW, srcH, scaleDown ? scaleDown : 1);
  return true;
}

void H264Player::stop() {
  if (!s.running && !s.inited && !s.data) return;
  LOGD("PgH264: 停止（%s，共喂 %d 帧）", s.streamMode ? "流式" : "文件", s.framesFed);
  s.running = false;
  /* 文件模式要等喂帧线程从 sleep/循环里退出来（它可能正卡在 put_frame 前后）；
   * 流式模式没有自己的线程（喂帧的就是调用方线程）→ 直接收。 */
  if (!s.streamMode) sleepMs(120);
  s.streamMode = false;
  if (s.inited) {
    zk_h264_player_set_decode_cb(0);
    zk_h264_player_hide();
    zk_h264_player_deinit();
    s.inited = false;
  }
  /* ⚠️ 关键收尾：zk_h264_player 退出后，**disp 层的 enable 不一定被清掉**。
   * 层层残留的后果是"这一轮播完、下一轮不出画"，而日志一切正常（解码在跑、
   * 回调在响）—— 极难从应用侧定位。所以这里主动把视频层释放掉（见 PgVideoLayer.h）。 */
  VideoLayer::release();
  if (s.data) {
    free(s.data);
    s.data = 0;
    s.size = 0;
  }
  s.units.clear();
  s.totalUnits = 0;
  s.framesFed = 0;
  s.desc[0] = 0;
}

bool H264Player::running() { return s.running; }
int H264Player::framesFed() { return s.framesFed; }
int H264Player::framesDecoded() { return s.framesDecoded; }
void H264Player::armRun() { s.framesRun = 0; }
int H264Player::framesDecodedInRun() { return s.framesRun; }
int H264Player::totalUnits() { return s.totalUnits; }
int H264Player::pictureCount() { return s.inited ? zk_h264_player_get_picture_count() : -1; }
int H264Player::needIframe() { return s.inited ? zk_h264_player_need_iframe() : -1; }
const char *H264Player::lastError() { return s.err[0] ? s.err : "-"; }
const char *H264Player::describe() { return s.desc[0] ? s.desc : "-"; }

/* ==================== ★ 流式播放（IPTV / 在线流）====================
 * 帧由外部 feed() 喂进来；**旋转/位置/裁剪全部是硬件干的**（就是用户点名的
 * zk_h264_player_set_rot / set_pos / set_crop 三个 API）。
 *
 * 为什么不自己在应用层转：上一版 `PgStream::rotateInto()` 拿 CPU 转帧，把 VDEC 输出的
 * NV12 当 I420 三平面拆开转 → 色度全乱（大片纯绿）；而且它转出的 256x448 缓冲配在
 * "frame=480x700" 的层上 → 显示引擎按 480 宽读 256 宽的缓冲 → 画面横向重复。
 * 硬件这条路则是"解码器直接吐转正、缩放好的帧"，代价为零。 */
bool H264Player::startStream(int srcW, int srcH, int scaleDown, int rotDeg, int dispX, int dispY,
                            int dispW, int dispH) {
  if (s.running || s.inited) stop();
  s.err[0] = 0;
  s.framesFed = 0;
  s.framesDecoded = 0;
  s.dropped = 0;
  s.pqLogged = 0;
  s.autoPosPending = 0;

  /* 源尺寸未知（直播流常态：我们的 HLS 中继从"最新写入位置"开始，第一个 SPS 要等
   * 下一个 I 帧）→ 先按 0x0 init（硬解自己会从码流里得到真实尺寸），等第一帧解出来
   * 再按真实宽高重算显示区（见 onDecodedFrame/applyAutoSize）。 */
  const bool autoSize = (srcW <= 0 || srcH <= 0);
  if (autoSize) {
    LOGD("PgH264: 源尺寸未知 → 交给硬解自适应（解出第一帧后按真实宽高摆放显示区）");
  }
  int flag = h264Flags(scaleDown);   // STREAM_EOF | 缩放档（UNCACHE 已放弃，见文件头）

  s.autoSize = autoSize;
  s.srcW = autoSize ? 0 : srcW;
  s.srcH = autoSize ? 0 : srcH;
  s.scaleDown = scaleDown ? scaleDown : 1;
  s.rot = rotDeg;
  s.posX = dispX; s.posY = dispY; s.posW = dispW; s.posH = dispH;
  s.areaX = dispX; s.areaY = dispY; s.areaW = dispW; s.areaH = dispH;
  s.cropX = s.cropY = s.cropW = s.cropH = 0;

  /* ★ 起播前**紧挨着 init 再清一次页缓存**（见 PgMem.h）：接下来这几行就是
   * zk_h264_player 申请解码/显示缓冲的时刻，本板实测可用内存 <3MB 时它会失败
   * 或让进程静默消失（日志停在 "[HW] 起硬件播放器" 之后）—— 这里清一次最值。 */
  dropPageCache();
  ensureZkmediaEnv();
  zk_h264_player_preload();
  /* ★ rot 传什么：**两种都行**（2026-09-14 干净开机受控 A/B 各跑 36 秒 / 900 帧 / 0 重启）：
   *   A) init(rot=0) → … → show → set_rot(90) → crop   ← 本工程统一用这条
   *   B) init(rot=90) 直接带旋转（不再 set_rot）        ← 同样正常
   *   本工程选 A 只为**与参考工程 `context.cpp`、以及文件播放 `h264play` 的顺序一致**，
   *   不是硬性限制。
   *
   * ⚠️ 历史教训（别重踩）：起播瞬间"静默退出"（无任何报错、日志断在起播那几行、
   *   进程被 init 拉起）**真凶是 `ZKMEDIA_H264_VBVSIZE` 没设**（库默认 VBV 装不下 720p 的
   *   I 帧，见 ensureZkmediaEnv()），其次才是可用内存 <3MB。当年先怀疑"旋转不能进 init"、
   *   再怀疑"crop 发在 set_rot 之前"，绕了两轮 —— **先查环境变量与内存，再怀疑参数组合**。
   *   另外 `.desc` 那句日志必须打印**实际**传进去的参数（曾硬编码 "rot=0"，白跑一轮 A/B）。
   *
   * ✗ 别再试"init 直接带 rot + DISP_UNCACHE"那组合：DISP_UNCACHE 已放弃（它绕开 DISP，
   *   而旋转就是 DISP 做的），理由见文件头。本工程固定用上面 A 那条。 */
  int r = zk_h264_player_init(autoSize ? 0 : srcW, autoSize ? 0 : srcH, E_DISP_ROT_0, flag);
  zk_h264_player_set_decode_cb(onDecodedFrame);
  snprintf(s.desc, sizeof(s.desc),
           "zk_h264_player_init(%d,%d,rot=0,flag=0x%X) -> %d；显示区 (%d,%d %dx%d) 缩放 1/%d，"
           "旋转 %d°(起播后下发)%s",
           autoSize ? 0 : srcW, autoSize ? 0 : srcH, flag, r, dispX, dispY,
           dispW, dispH, s.scaleDown, rotDeg, autoSize ? " 源尺寸自适应" : "");

  LOGD("PgH264: %s", s.desc);
  if (r != 0) {
    setErr("init 失败（%d）—— 多半是 dlopen libawh264player.so 没成功（看上面 loader 的报错）", r);
    return false;
  }
  s.inited = true;
  s.streamMode = true;

  /* ★ 顺序**必须与文件播放 playFile + setRotation 完全一致**（实测：只有这个顺序在
   *   "720p + 1/2 缩放 + 90°" 下不崩）：
   *     init(rot=0) → set_pos → show → set_rot(90) → applyCrop()
   *   ⚠️ crop 一定要放在 set_rot **之后**：rotate 没生效时 crop 是按"旋转后坐标系"
   *   算的（s.rot 已经是目标角度），提前发就是给一个未旋转的播放器灌旋转后的裁剪，
   *   起播第一包就崩（本次踩过）。 */
  zk_h264_player_set_pos(dispX, dispY, dispW, dispH);
  zk_h264_player_show();
  if (dispRotOf(rotDeg) != E_DISP_ROT_0) {
    zk_h264_player_set_rot(dispRotOf(rotDeg));
    LOGD("PgH264: 起播后下发硬件旋转 -> %d°", rotDeg);
  }
  applyCrop();   /* 整帧裁剪（旋转后坐标系）—— 放在最后发 */
  LOGD("PgH264: 流式起播 —— 显示区 (%d,%d %dx%d)，裁剪 整帧(%d°旋转后)，旋转 %d°",
       dispX, dispY, dispW, dispH, rotDeg, rotDeg);

  s.running = true;
  return true;
}

bool H264Player::feed(const uint8_t *data, size_t size) {
  if (!s.inited || !s.running || !data || size == 0) return false;
  /* 喂帧前处理"尺寸自适应"（见 applyAutoSize 的说明：不能在解码回调线程里调 API） */
  if (s.autoPosPending) {
    s.autoPosPending = 0;
    applyAutoSize();
  }
  /* ★★ 这里**不再有"按 picture_count 丢弃"的保护**（2026-09-21 删掉）——
   *   那个值是"累计提交帧数"而不是积压，长时间循环播放涨到 10 万后会把**每一帧**都误判成
   *   "驱动异常"丢掉，把解码器饿死 ⇒ 画面静帧。详见 kPqLogLine 的三次血案。
   *   现在只观测、不丢弃：喂进去的每一帧都真的交给解码器。 */
  const int pq = zk_h264_player_get_picture_count();
  if (pq > kPqLogLine && !s.pqLogged) {
    s.pqLogged = 1;
    LOGD("PgH264: get_picture_count=%d（这是**累计提交帧数**、不是积压；只记录，不影响喂入）", pq);
  }
  zk_h264_player_put_frame((uint8_t *)data, (uint32_t)size);
  ++s.framesFed;
  return true;
}

void H264Player::setRotation(int deg) {
  if (!s.inited) return;
  s.rot = deg;
  zk_h264_player_set_rot(dispRotOf(deg));
  applyCrop();   /* 旋转后裁剪坐标系变了 → 重新下发一次（参考工程 video_rot_and_crop 同理） */
  LOGD("PgH264: 硬件旋转 -> %d°（crop 已按旋转后尺寸重算）", deg);
}

int H264Player::rotation() { return s.rot; }

void H264Player::setDispRect(int x, int y, int w, int h) {
  if (!s.inited) return;
  s.posX = x; s.posY = y; s.posW = w; s.posH = h;
  zk_h264_player_set_pos(x, y, w, h);
  LOGD("PgH264: 显示区 -> (%d,%d %dx%d)", x, y, w, h);
}

void H264Player::setSourceCrop(int x, int y, int w, int h) {
  if (!s.inited) return;
  s.cropX = x; s.cropY = y; s.cropW = w; s.cropH = h;
  applyCrop();
}

void H264Player::clearCrop() {
  if (!s.inited) return;
  s.cropX = s.cropY = s.cropW = s.cropH = 0;
  applyCrop();
}

/* 等比放进显示区：算出来的矩形不裁内容、不拉伸（多余处是黑边）。 */
void H264Player::fitRect(int srcW, int srcH, int rotDeg, int areaX, int areaY, int areaW,
                         int areaH, int *outX, int *outY, int *outW, int *outH) {
  int rw = 0, rh = 0;
  rotatedSize(srcW, srcH, rotDeg, &rw, &rh);
  if (rw <= 0 || rh <= 0 || areaW <= 0 || areaH <= 0) {
    if (outX) *outX = areaX;
    if (outY) *outY = areaY;
    if (outW) *outW = areaW;
    if (outH) *outH = areaH;
    return;
  }
  /* 用整数比较避免浮点：w/h 取 min(areaW/rw, areaH/rh) */
  long long byW = (long long)areaW * rh, byH = (long long)areaH * rw;
  int w, h;
  if (byW <= byH) {          // 宽度是限制边
    w = areaW;
    h = (int)((long long)areaW * rh / rw);
  } else {
    h = areaH;
    w = (int)((long long)areaH * rw / rh);
  }
  if (w > areaW) w = areaW;
  if (h > areaH) h = areaH;
  if (outX) *outX = areaX + (areaW - w) / 2;
  if (outY) *outY = areaY + (areaH - h) / 2;
  if (outW) *outW = w;
  if (outH) *outH = h;
}

bool H264Player::streamMode() { return s.streamMode; }
int H264Player::sourceW() { return s.srcW; }
int H264Player::sourceH() { return s.srcH; }

}  // namespace pg
