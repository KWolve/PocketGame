/*
 * PgSkin.cpp - 「形状」层实现。设计说明见 PgSkin.h。
 */
#include "platform/PgSkin.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "control/ZKBase.h"
#include "utils/Log.h"

namespace pg {

namespace {

/* 语义色 → 资源名前缀。
 * ⚠️ 与 tools/ios_theme.py 的 BTN_STYLES 一一对应，改一处必须改两处
 *    （设备端没有 Python，只能手抄一份）。色值取自 ios_theme.TOKENS。 */
struct Style {
  uint32_t argb;
  const char *prefix;
};

const Style kStyles[] = {
    {0xFF2C2C2E, "gray"},      // SURFACE2  次级按钮
    {0xFF1C1C1E, "dark"},      // SURFACE   卡片上的次级块
    {0xFF3A3A3C, "select"},    // SURFACE3  分段控件选中胶囊
    {0xFF0A84FF, "blue"},      // ACCENT    系统蓝
    {0xFF30D158, "green"},     // SUCCESS   成功 / 开始
    {0xFFFF453A, "red"},       // DANGER    危险
    {0xFF3A1F1F, "redsoft"},   // DANGER_SOFT 危险降权
    {0xFFFF9F0A, "amber"},     // DATA      数据 / 告警
    {0xFFF2F2F7, "white"},     // T1        高对比
};

/** 圆角档 → 后缀（与 ios_theme.RADIUS_VARIANTS 对应） */
const char *radiusSuffix(int h) {
  if (h >= 80) return "r20";
  if (h >= 48) return "r14";
  if (h >= 30) return "r10";
  return "r8";
}

/* 路径是常量拼接的结果，必须静态存放 —— 返回栈缓冲会被下一次调用踩掉
 * （框架内部若只存指针就变成悬垂；按 slot 轮转 8 份足够覆盖同帧的多次调用）。 */
char sPathBuf[8][64];
int sPathIdx = 0;

}  // namespace

const char *roundedAssetFor(uint32_t argb, int w, int h, OnSurface on) {
  // 只按 RGB 比对，忽略 alpha（页面里写的都是 0xFF 不透明色）
  uint32_t rgb = argb & 0x00FFFFFF;
  for (size_t i = 0; i < sizeof(kStyles) / sizeof(kStyles[0]); ++i) {
    if ((kStyles[i].argb & 0x00FFFFFF) == rgb) {
      char *buf = sPathBuf[sPathIdx];
      sPathIdx = (sPathIdx + 1) % 8;
      /* ★ 运行时挂的图必须是**普通 PNG**（`ios_rt_<style>_<W>x<H>[_c].png`），不能用九宫格：
       *   `.9.png` 四周那 1px marker 边，只有在 **JSON 路径**（picTab/backgroundPic 字段）
       *   才会被正确消费；**运行时 `setBackgroundPic()` 会把它渲染成一条 1px 白线**
       *   （实测：段容器=JSON picTab 边缘干净；tab 胶囊/音效按钮=运行时挂图，最外圈恒白，
       *    图里烘什么底色都盖不住，因为白线画在内容之外）。
       *   ⇒ 普通 PNG 没有 marker 边，代价是"一种 (样式, 尺寸, 所坐底) 一组图"。
       *     尺寸集合由 gen_ui.py 扫源稿的 `data-noround` 得出，不会漏。
       * ★ 后缀 `_c` = 坐在面板/卡片底（#1C1C1E）上那套。烘的是"圆角抗锯齿像素混进什么色"，
       *   坐错底 ⇒ 圆角边缘一圈异色（看着就是锯齿/脏边）。所以 on 必须传对。 */
      snprintf(buf, sizeof(sPathBuf[0]), "images/ios_rt_%s_%dx%d%s.png",
               kStyles[i].prefix, w, h, (on == ON_CARD) ? "_c" : "");
      return buf;
    }
  }
  return nullptr;
}

void applyRoundedBg(ZKBase *v, uint32_t argb, OnSurface on) {
  if (!v) return;
  const LayoutPosition &pos = v->getPosition();
  /* 太薄的东西不参与圆角：阶段强调色条只有 8~10px 高，套圆角会变成"胶囊"、
   * 而且 2*radius > 高度会让九宫格角区重叠、拉伸失真。
   * 24 这个下限 = 最薄的按钮（36）与色条（10）之间的安全分界。 */
  if (pos.mHeight < 24) return;
  const char *pic = roundedAssetFor(argb, pos.mWidth, pos.mHeight, on);
  if (!pic) {
    /* 没有对应资源：**保持原样，不主动清空**。
     * 为什么不清：框架没有公开"清除背景图"的语义（`setBackgroundPic` 只收路径），
     * 传 nullptr/"" 的行为未定义、有崩溃风险。改成"表里必须能找到颜色"这条纪律：
     *   任何**运行时**会改底色的颜色，都必须出现在 kStyles 里
     *   （iOS 令牌一共就 9 个语义色，加颜色是低频动作）。
     * 日志只打一次，避免在 30fps 的主循环里刷屏。 */
    static int sWarned = 0;
    if (sWarned < 8) {
      LOGD("PgSkin: 颜色 %08X（%dx%d）没有对应圆角资源 —— 该控件保持原样（直角）",
           argb, pos.mWidth, pos.mHeight);
      ++sWarned;
    }
    return;
  }
  // ⚠️ 顺序：先设图，再清底色。
  //    图里已烘上页面底，但控件的 bgColorTab 仍是实色 ⇒ 会盖在图上/影响按下态，必须清掉。
  v->setBackgroundPic(pic);
  v->setBackgroundColor(-1);
  v->setBgStatusColor(ZK_CONTROL_STATUS_NORMAL, -1);
}

uint32_t toolBtnColor(int style) {
  switch (style) {
    case 1: return 0xFF30D158;   // 主行动（开始 / 确认）SUCCESS 绿
    case 2: return 0xFF3A3A3C;   // 次要（重置 / 清零 / 退格）SURFACE3 中灰
    case 3: return 0xFFFF9F0A;   // 强调（运算符 / 关键动作）DATA 橙
    default: return 0xFF2C2C2E;  // 普通键（数字）SURFACE2 深灰
  }
}

uint32_t toolBtnFg(int style) {
  // 亮底（绿 / 橙）用黑字：小屏上对比度优先；深底用高对比白字
  return (style == 1 || style == 3) ? 0xFF000000u : 0xFFF2F2F7u;
}

}  // namespace pg
