/*
 * PgSprite.cpp - 贴图加载器（设备侧）：把 PNG 解成画布能 1:1 贴的像素缓冲
 *
 * 用的框架 API：`BitmapHelper::loadBitmapFromFile(bmp, path, reqBpp)`
 *   —— easyui 自带解码器（扫符号确认：内部有 PNG / GIF / WebP / QOI decoder），
 *      所以**不需要引第三方解码库**（本工程没链 libpng，也不该为此引）。
 *
 * 路径基准：**资源相对路径**，如 "images/game/whack/mole.png"
 *   （与 PgSkin 挂 `images/ios_rt_*.png` 同一套基准；`fun launch` 把 resources/
 *   推到设备侧资源根，`fun pack` 则固化进 /res 的 squashfs）。
 *
 * ★ 两个刻意的实现选择：
 *   ① **拷成自己的紧凑缓冲**，然后立刻 unloadBitmap。
 *      为什么不直接用框架返回的缓冲：它的所有权/释放方式归框架（`unloadBitmap`），
 *      而且 `pitch` 受硬件对齐约束（16 字节），宽不是 4 的倍数时 stride != w。
 *      拷一份紧凑的（stride == w）后，Sprite 语义与"编进 .so 的常量表"完全一致，
 *      也没有谁先释放的问题。代价是加载瞬间**双份内存**（最大 480x540 ≈ 1MB ⇒ 2MB），
 *      一次性开销，可以接受。
 *   ② **启动时自检格式**（`selfTest()`）：解码出来的像素到底是不是
 *      "0xAARRGGBB 小端 = B,G,R,A"、alpha 是否预乘 —— 只能实测，不能猜。
 *      用一张探针图（tools/gen_game_art.py 生成）+ QA `spr` 打印。
 */
#include "platform/PgSpriteDevice.h"

#include "core/PgSprite.h"   // SpriteLoader / SpriteBank（本文件是它们的设备侧实现）

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FUN_BUILD
#include "utils/BitmapHelper.h"
#include "utils/Log.h"

namespace pg {
namespace {

/* 把一张图解码成"紧凑 RGBA 缓冲"。
 * 返回的缓冲由调用者 free；失败返回 0 并已打日志。 */
/* ★★ 路径必须**自己拼资源根**（2026-09-15 实测踩到）：
 *   `BitmapHelper::loadBitmapFromFile` 内部就是 fopen，**不做资源解析** ——
 *   传 "images/game/xx.png" 会失败（进程 CWD 不是资源根）。
 *   而框架的资源根写在 EasyUI.cfg 的 `resPath` 里，且**调试/固化两套值不同**：
 *     调试（fun launch）：/tmp/EasyUI.cfg → resPath=/tmp/ui/
 *     固化（fun pack 刷机）：/res/etc/EasyUI.cfg → resPath=/res/ui/
 *   ⇒ 这里读 cfg 取值（先 /tmp 后 /res，与框架自己的优先级一致），
 *     这样同一份代码在两种部署下都能找到素材，**不用改路径也不用重编**。
 *   （相对路径只用于"逻辑路径"，如 images/game/whack/mole.png。） */
const char *resRoot() {
  static char root[128] = {0};
  static bool inited = false;
  if (inited) return root;
  inited = true;
  const char *cands[2] = {"/tmp/EasyUI.cfg", "/res/etc/EasyUI.cfg"};
  for (int i = 0; i < 2 && !root[0]; ++i) {
    FILE *f = fopen(cands[i], "rb");
    if (!f) continue;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strstr(buf, "\"resPath\"");
    if (!p) continue;
    p = strchr(p + 8, ':');
    if (!p) continue;
    p = strchr(p, '"');
    if (!p) continue;
    ++p;
    char *e = strchr(p, '"');
    if (!e) continue;
    size_t len = (size_t)(e - p);
    if (len == 0 || len >= sizeof(root) - 1) continue;
    memcpy(root, p, len);
    root[len] = 0;
  }
  if (!root[0]) snprintf(root, sizeof(root), "%s", "/res/ui/");
  LOGD("PgSprite: 资源根 = %s（取自 EasyUI.cfg 的 resPath）", root);
  return root;
}

/* 逻辑路径 -> 真实路径（绝对路径原样返回）。 */
void makeRealPath(const char *path, char *out, int n) {
  if (path[0] == '/') {
    snprintf(out, n, "%s", path);
    return;
  }
  const char *r = resRoot();
  const bool slash = (r[0] && r[strlen(r) - 1] == '/');
  snprintf(out, n, "%s%s%s", r, slash ? "" : "/", path);
}

uint32_t *decodeTight(const char *path, int *outW, int *outH, int *outStride) {
  char real[192];
  makeRealPath(path, real, sizeof(real));
  bitmap_t *b = 0;
  /* ★★ reqBpp **必须传 0**（2026-09-15 实测，别改成 32）：
   *   QA `spr` 逐值试过（96x88 的 PNG）：
   *     reqBpp=0  → bits=32 bytes=4  pitch=384  （= 96×4，**标准 32bpp ARGB8888**）
   *     reqBpp=16 → bits=128 bytes=16 pitch=1536 （每像素 16 字节 —— 不是 16bpp！）
   *     reqBpp=32 → bits=0  bytes=32 pitch=3072 （每像素 32 字节）
   *   即这个参数不是"目标位深"而是**放大倍数**一类的语义：0 = 原样解出 32bpp。
   *   传 32 的症状很隐蔽：图能"加载成功"，但像素位置全错 ⇒ 贴出来是花屏/全黑。 */
  if (!BitmapHelper::loadBitmapFromFile(b, real, 0) || !b || !b->data) {
    LOGE("PgSprite: 解码失败 '%s'（实际路径 %s）", path, real);
    if (b) BitmapHelper::unloadBitmap(b);
    return 0;
  }

  const int w = (int)b->width;
  const int h = (int)b->height;
  const int pitch = (int)b->pitch;
  if (w <= 0 || h <= 0 || pitch < w * 4 ||
      (pitch & 3) != 0) {   // pitch 必须是 4 的倍数（32bpp 一行是整数个像素）
    LOGE("PgSprite: 尺寸异常 '%s' w=%d h=%d pitch=%d bits=%d bytes=%d", path, w, h,
         pitch, (int)b->bits, (int)b->bytes);
    BitmapHelper::unloadBitmap(b);
    return 0;
  }
  LOGD("PgSprite: 原始 bitmap '%s' type=%d bits=%d bytes=%d alpha=%d ck=%u "
       "%ux%u pitch=%d ap=%u phy=%p",
       path, (int)b->type, (int)b->bits, (int)b->bytes, (int)b->alpha, b->ck,
       b->width, b->height, (int)b->pitch, b->ap, (void *)b->phy_addr);

  uint32_t *buf = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
  if (!buf) {
    LOGE("PgSprite: 内存不足 '%s' %dx%d (%dKB)", path, w, h, w * h * 4 / 1024);
    BitmapHelper::unloadBitmap(b);
    return 0;
  }
  /* 逐行拷贝（pitch != w*4 时会跳过行尾的对齐填充） */
  for (int y = 0; y < h; ++y) {
    memcpy((uint8_t *)buf + (size_t)y * w * 4,
           (const uint8_t *)b->data + (size_t)y * pitch, (size_t)w * 4);
  }
  const int stridePx = pitch / 4;
  const int bits = (int)b->bits;
  const int alphaFlag = (int)b->alpha;
  BitmapHelper::unloadBitmap(b);

  /* ---- 解码结果的兜底修补（都是"图会整张消失/发黑"级别的静默故障）---- */
  /* ① 整张图 alpha 全 0 但 RGB 有内容：解码器把"无 alpha 通道的 PNG"解成了透明
   *    ⇒ drawSprite 会一个像素都不写（症状：元素**完全不显示**，日志却一切正常）。
   *    补成全不透明。 */
  int zeroA = 0;
  for (int i = 0; i < w * h; ++i) {
    if ((buf[i] >> 24) == 0) ++zeroA;
  }
  if (zeroA == w * h) {
    LOGW("PgSprite: '%s' alpha 全 0（PNG 可能没有 alpha 通道）→ 补成全不透明", path);
    for (int i = 0; i < w * h; ++i) buf[i] |= 0xFF000000u;
  }

  if (outW) *outW = w;
  if (outH) *outH = h;
  if (outStride) *outStride = w;
  LOGD("PgSprite: 载入 '%s' %dx%d pitch=%d(%s) bits=%d alpha=%d %dKB", path, w, h,
       pitch, (stridePx == w ? "紧凑" : "有对齐"), bits, alphaFlag, w * h * 4 / 1024);
  return buf;
}

class DevSpriteLoader : public SpriteLoader {
 public:
  bool load(const char *path, Canvas::Sprite *out) {
    int w = 0, h = 0, st = 0;
    uint32_t *buf = decodeTight(path, &w, &h, &st);
    if (!buf) return false;
    out->px = buf;
    out->w = w;
    out->h = h;
    out->stride = st;
    return true;
  }

  void unload(Canvas::Sprite *sp) {
    if (sp && sp->px) {
      free((void *)sp->px);
      sp->px = 0;
      sp->w = 0;
      sp->h = 0;
      sp->stride = 0;
    }
  }
};

DevSpriteLoader gDevLoader;

}  // namespace

/* 装上设备侧加载器。由 mainLogic 在 onUI_init 里调一次。 */
void installSpriteLoader() {
  SpriteBank::instance().setLoader(&gDevLoader);
}

/* 把一张图的信息 + 若干像素打成日志（QA 用）。字节序/预乘只能实测，不能猜。 */
void spriteProbe(const char *path) {
  char real[192];
  makeRealPath(path, real, sizeof(real));
  LOGD("PgSprite: probe '%s' -> %s", path, real);

  /* ① 先用框架原函数**直接看原始 bitmap**（不同 reqBpp 各试一次）——
   *    这一步是为诊断"解出来的到底是不是 ARGB8888"。 */
  const int bpps[3] = {0, 16, 32};
  for (int i = 0; i < 3; ++i) {
    bitmap_t *b = 0;
    if (!BitmapHelper::loadBitmapFromFile(b, real, bpps[i]) || !b) {
      LOGD("PgSprite:   reqBpp=%-2d 加载失败", bpps[i]);
      continue;
    }
    LOGD("PgSprite:   reqBpp=%-2d type=%d bits=%d bytes=%d alpha=%d ck=%u %ux%u "
         "pitch=%d ap=%u", bpps[i], (int)b->type, (int)b->bits, (int)b->bytes,
         (int)b->alpha, b->ck, b->width, b->height, (int)b->pitch, b->ap);
    if (b->data) {
      const uint32_t *p = (const uint32_t *)b->data;
      LOGD("PgSprite:      data[0..3] = %08X %08X %08X %08X", p[0], p[1], p[2], p[3]);
      if (b->pitch >= 8) {
        const uint32_t *q = (const uint32_t *)((const uint8_t *)b->data + b->pitch);
        LOGD("PgSprite:      +1行        = %08X %08X %08X %08X", q[0], q[1], q[2], q[3]);
      }
    }
    BitmapHelper::unloadBitmap(b);
  }

  /* ② 再看**拷成紧凑缓冲之后**的样子（游戏实际贴的就是这份） */
  int w = 0, h = 0, st = 0;
  uint32_t *buf = decodeTight(path, &w, &h, &st);
  if (!buf) {
    LOGD("PgSprite: probe '%s' 紧凑解码失败", path);
    return;
  }
  LOGD("PgSprite: probe 紧凑结果 %dx%d", w, h);
  const int n = (w * h < 8) ? w * h : 8;   // 只看前 8 个像素
  for (int i = 0; i < n; ++i) {
    const uint32_t c = buf[i];
    LOGD("PgSprite:   px[%d] = 0x%08X  A=%d R=%d G=%d B=%d", i, c, (int)(c >> 24),
         (int)((c >> 16) & 0xFF), (int)((c >> 8) & 0xFF), (int)(c & 0xFF));
  }
  free(buf);
}

}  // namespace pg

#else  // !FUN_BUILD：PC / 离屏验收没有框架的解码器，装个空实现（游戏自然降级）

namespace pg {
void installSpriteLoader() {}
void spriteProbe(const char *) {}
}  // namespace pg

#endif  // FUN_BUILD
