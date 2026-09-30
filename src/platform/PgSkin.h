/*
 * PgSkin.h - 「形状」层：把颜色映射到 iOS 圆角九宫格资源
 *
 * 背景（2026-09-14 UI 改版）：
 *   改版前所有控件都是直角矩形（卡片 / 按钮 / tab / 电池），因为 FlyThings 没有 CSS 引擎。
 *   实测确认 html2json 对 div.card 的 border-radius **能自动转图**，但 div.btn 的**不转**
 *   —— 全工程 285 个按钮逐个切图不现实。
 *
 * 所以形状这一层拆成两半：
 *   · **静态按钮**（底色从不变）→ gen_ui.py 按底色自动挂 JSON 的 `picTab` 两态图，
 *     连按下态都由框架处理，运行时一行代码都不用写。
 *   · **动态按钮**（底色会被逻辑改，如 tab 选中态、工具页按键、开关）→ 不能用静态图
 *     （`setBackgroundColor` 会把 `backgroundPic` 清掉，实机踩过），改由本文件在
 *     "设底色"的那一刻顺带把对应的圆角图挂上。
 *
 * ⚠️ 这里是与 `tools/ios_theme.py` 的**跨语言对照表**，改色板必须两边一起改
 *    （色值与圆角档位写死在下面，因为设备端没有 Python）。
 *    对应关系：TOKENS 的 SURFACE2/ACCENT/... ↔ kStyles 的色值。
 *
 * ⚠️⚠️ 资源是**烘底**的（四周烘页面黑底，不是透明）—— 2026-09-14 真机实测：
 *    **运行时 `setBackgroundPic()` 这条路径不保留 alpha**，透明像素会被渲染成**纯白**，
 *    于是每个圆角按钮四周多出一圈 1px 白描边（圆角的羽化边也会糊成白的）。
 *    对照实验（同一张 ios_btn_select_r14.9.png）：
 *      · JSON 里写 picTab / backgroundPic → alpha 正确（列表行卡片四角干净）
 *      · 运行时 setBackgroundPic()        → 透明渲染成白（tab 胶囊、音效按钮一圈白）
 *    ⇒ 凡是**运行时**挂的图，都必须烘它实际坐落的底色。
 */
#ifndef PG_SKIN_H_
#define PG_SKIN_H_

#include <stdint.h>

class ZKBase;

namespace pg {

/**
 * 控件"坐在什么底上" —— 决定圆角图烘哪种底色。
 *
 * ⚠️ 这是**必须**传对的参数（2026-09-14 第二版血案）：
 *    烘底把"圆角的抗锯齿像素"和原底色绑死了。图坐错底，圆角边缘就会露出一圈别的颜色
 *    —— 肉眼就是**锯齿 / 脏边**。最典型的是分段控件：胶囊坐在 `#1C1C1E` 的段容器上，
 *    却烘的是页面黑底 ⇒ 胶囊四周一圈黑。
 */
enum OnSurface {
  ON_PAGE = 0,   // 直接坐在页面上（页面底 #000000）→ 资源无后缀
  ON_CARD = 1,   // 坐在面板 / 卡片 / 段容器上（#1C1C1E）→ 资源带 `_c` 后缀
};

/**
 * 颜色 + 控件尺寸 + 所坐底色 → 圆角资源路径（如 "images/ios_rt_gray_141x56.png"）。
 * @param argb 0xAARRGGBB（和 setBackgroundColor 用同一套色值）
 * @param w,h  控件宽高 —— 运行时用的是**普通 PNG**（图尺寸必须 == 控件尺寸），
 *             不是九宫格（原因见 .cpp 里的说明：marker 边会被渲染成白线）
 * @param on   坐在什么底上（决定烘哪种底色的那一套图）
 * @return 常量字符串；**nullptr = 该颜色没有对应资源**（调用方应保持纯色，别硬套）
 */
const char *roundedAssetFor(uint32_t argb, int w, int h, OnSurface on = ON_PAGE);

/**
 * 给控件套上 iOS 圆角：按底色 + 所坐底，选对应的运行时圆角图挂上去。
 *
 * 为什么必须由调用方在"改底色的地方"调，而不是统一在某个同步函数里轮询：
 *   底色是各页 logic 自己算的（tab 选中态 / 工具页风格 / 开关状态），
 *   在这一个点上顺手设图，几何与颜色永远同源；轮询就多一份状态、必然走偏。
 *
 * ⚠️ 内部会把 `bgColorTab` 清成 -1：图是**烘底**的（不透明），留底色会盖掉圆角。
 * @param v  目标控件；nullptr 安全
 * @param on 控件坐在什么底上（见 OnSurface；传错 = 圆角边缘出现异色脏边）
 */
void applyRoundedBg(ZKBase *v, uint32_t argb, OnSurface on = ON_PAGE);

/* ---------- 工具页（原生工具页外壳）的按钮配色 ----------
 * ★ 这是**唯一真值**：主界面 mainLogic 的 toolBtnBg() 与 src/ui/ToolPage.cpp
 *   都从这里取。改版前有两份色表（mainLogic 一份、ToolPage.cpp 一份），
 *   结果只改了主界面那份，工具页整片还是旧配色 —— 血案，别再复制第二份。
 *
 * 语义：0 普通键 / 1 主行动 / 2 次要 / 3 强调（运算符、关键动作）
 * 视觉：普通=深灰、次要=中灰、主行动=绿、强调=橙（iOS 计算器同款，一眼能认）。
 */
uint32_t toolBtnColor(int style);

/** 按钮文字色：亮底（绿/橙）用黑字保证对比度，深底用白字。 */
uint32_t toolBtnFg(int style);

}  // namespace pg

#endif  // PG_SKIN_H_
