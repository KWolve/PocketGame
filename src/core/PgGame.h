/*
 * PgGame.h - 游戏抽象接口 + 音效/存档宿主接口
 *
 * 设计约束：
 *  - 本文件及其实现不依赖 easyui，可在 PC 上编译（tools/hosttest）做离屏渲染验收。
 *  - 新增一款游戏 = 写一个 Game 子类 + 在 PgGames.cpp 的注册表加一行。
 */
#ifndef PG_GAME_H_
#define PG_GAME_H_

#include <stdint.h>

#include "PgCanvas.h"

namespace pg {

// 音效 id（与 resources/audio/*.wav 一一对应，见 PgAudio）
enum SfxId {
  SFX_NONE = 0,
  SFX_CLICK,   // click.wav   菜单/按钮
  SFX_MOVE,    // move.wav    移动/滑动
  SFX_ROTATE,  // rotate.wav  旋转
  SFX_DROP,    // drop.wav    落地/快速落下
  SFX_CLEAR,   // clear.wav   消行
  SFX_MERGE,   // merge.wav   合成（2048）
  SFX_SCORE,   // score.wav   得分
  SFX_HIT,     // hit.wav     受击/碰撞
  SFX_OVER,    // over.wav    游戏结束
  SFX_JUMP,    // jump.wav    小鸟振翅
  SFX_SHOOT,   // shoot.wav   发射
  SFX_DICE,    // dice.wav    骰子翻滚碰撞（摇骰子：连着播就是"咔啦咔啦"）
  /* ⚠️ 新音效**一律追加在表尾** —— 插中间会让 platform/PgAudio.cpp 的
   *    kSfxFiles[] 整体错位（声音对不上号，而且不报错）。 */
  SFX_PNO1,    // pno1.wav    钢琴 do   （节奏钢琴：按音高分 8 个独立音效）
  SFX_PNO2,    // pno2.wav    钢琴 re
  SFX_PNO3,    // pno3.wav    钢琴 mi
  SFX_PNO4,    // pno4.wav    钢琴 fa
  SFX_PNO5,    // pno5.wav    钢琴 sol
  SFX_PNO6,    // pno6.wav    钢琴 la
  SFX_PNO7,    // pno7.wav    钢琴 si
  SFX_PNO8,    // pno8.wav    钢琴 do'
  SFX_DRM1,    // drm1.wav    底鼓
  SFX_DRM2,    // drm2.wav    军鼓
  SFX_DRM3,    // drm3.wav    踩镲
  SFX_DRM4,    // drm4.wav    低嗵
  SFX_DRM5,    // drm5.wav    高嗵
  SFX_DRM6,    // drm6.wav    吊镲
  SFX_COUNT
};

// 触摸动作
enum TouchAction {
  PG_TOUCH_DOWN = 0,
  PG_TOUCH_MOVE = 1,
  PG_TOUCH_UP = 2,
};

// 3 个物理按键的逻辑名
enum GameKey {
  PG_KEY_A = 0,  // 确定 / 开始 / 暂停
  PG_KEY_B = 1,  // 返回
  PG_KEY_C = 2,  // 重玩 / 菜单
};

// 游戏对外可见的运行状态
enum GameState {
  GSTATE_READY = 0,  // 已加载未开始
  GSTATE_RUNNING,
  GSTATE_PAUSED,
  GSTATE_OVER,
};

// 宿主提供的能力（音效 + 最高分 + 系统能力）。
//
// 系统能力（WiFi 等）刻意只暴露"值/动作"，不暴露网络库类型：core 层不依赖 zknet，
// 由 platform 层（PgWifi）去实现。默认实现是空/假，老代码不受影响。
class Host {
 public:
  virtual ~Host() {}
  virtual void playSfx(int sfxId) = 0;
  virtual int highScore(int gameIndex) const = 0;
  virtual void setHighScore(int gameIndex, int score) = 0;

  // ---- 系统能力：WiFi ----
  virtual bool wifiSupported() { return false; }
  virtual bool wifiEnabled() { return false; }
  virtual bool wifiConnected() { return false; }
  virtual void wifiSsid(char *buf, int n) { if (buf && n) buf[0] = 0; }
  virtual void wifiIp(char *buf, int n) { if (buf && n) buf[0] = 0; }
  virtual void wifiMac(char *buf, int n) { if (buf && n) buf[0] = 0; }
  virtual int wifiRssi() { return 0; }
  virtual void wifiSetEnabled(bool on) { (void)on; }
  virtual void openWifiSettings() {}  // 打开框架内置的 WiFi 设置界面

  /* ---- 音效开关 / 音量（2026-09-16：系统设置页把主界面那颗音效按钮收了过去）----
   * 为什么放进 Host 而不是让页面直接摸 audio/store：音频与存档是**进程级单例**，
   * 独立 ftu 的页面只能通过宿主拿（见 ui/ToolPage.h 的 pgHost 说明）。 */
  virtual bool soundOn() const { return true; }
  virtual void setSoundOn(bool on) { (void)on; }
  virtual int volumePercent() const { return -1; }
  virtual void setVolumePercent(int pct) { (void)pct; }

  /* ⚠️ 蓝牙**不在这里**：蓝牙遥控是独立 ftu（remote.ftu / remoteActivity），
   * 它的界面逻辑在 src/logic/remoteLogic.cc 里直接调 platform/PgBt 的单例。
   * 独立功能独立 ftu，主界面就不必为它挂一层转发（见 ui/remote.html）。 */
};

// 手柄（软按键）：画布底部的一排触摸按钮，游戏可用可不用。
//
// 为什么要有它：只用全屏手势的游戏，手势一多就会互相冲突 ——
// 俄罗斯方块最初把"移动 / 软降 / 直落 / 旋转"四种手势全塞进同一个触摸区域，
// 想横移手抖了会被判成旋转；推箱子则自己另写了一排底部按钮（自绘 + 硬编码命中）。
// 于是本机制把"画布底部按钮"统一起来：**游戏只声明按钮，绘制与命中由 logic 层负责**。
struct SoftButton {
  int x, y, w, h;
  const char *label;
  int id;
};

// 画布底部为软按钮保留的条带高度（游戏做布局时要把它从可用高度里减掉）
enum { PG_SOFT_BAR_H = 62 };

// 便捷布局：在画布底部生成 n 个等宽软按钮（label / id 由调用者填）
inline void layoutSoftBar(SoftButton *b, int n, int vw, int vh) {
  const int side = 6, gap = 6;
  int w = (vw - 2 * side - (n - 1) * gap) / n;
  for (int i = 0; i < n; ++i) {
    b[i].x = side + i * (w + gap);
    b[i].y = vh - PG_SOFT_BAR_H + 6;
    b[i].w = w;
    b[i].h = PG_SOFT_BAR_H - 12;
  }
}

class Game {
 public:
  Game() : host_(0), index_(0), state_(GSTATE_READY), vw_(0), vh_(0) {}
  virtual ~Game() {}

  void bind(Host *host, int index) {
    host_ = host;
    index_ = index;
  }

  // 画布可视区尺寸（由 logic 层在创建画布后告知），游戏据此做布局
  void setViewport(int w, int h) {
    vw_ = w;
    vh_ = h;
  }
  int vw() const { return vw_; }
  int vh() const { return vh_; }

  // ---- 元信息（游戏列表用，必须为 UTF-8 中文字面量）----
  virtual const char *title() const = 0;    // 中文名
  virtual const char *desc() const = 0;     // 一行说明
  virtual const char *tag() const = 0;      // 卡片图标上的短标签（ASCII，<=4 字符）
  virtual Color theme() const = 0;          // 卡片配色

  // ---- 生命周期 ----
  virtual void reset() = 0;                 // 开始/重玩
  virtual void update(int dtMs) = 0;        // 固定步长推进
  virtual void render(Canvas &c) = 0;       // 绘制一帧（画布已 clear）

  // ---- 输入 ----
  // 返回 true 表示游戏消费了该触摸事件
  virtual bool onTouch(int action, int x, int y) = 0;
  // 物理按键。返回 true 表示游戏消费了（例如 A 在游戏中=暂停）
  virtual bool onKey(int key) { (void)key; return false; }

  /*
   * ---- 画布底部软按钮（可选）----
   * 返回按钮数组并把数量写进 n（返回 0 或 n<=0 = 本游戏不用软按钮）。
   * logic 层负责在画布底部**绘制**它们（有按下态）并在触摸时**优先派发** onSoftButton(id)，
   * 所以游戏侧只需要"声明 + 处理点击"，不用管绘制与命中检测。
   *
   * 注意：坐标依赖视口，而本方法是 const —— 需要惰性计算时把缓存成员声明为 mutable。
   * 用了软按钮的游戏，做布局时要把 PG_SOFT_BAR_H 从可用高度里减掉。
   */
  virtual const SoftButton *softButtons(int &n) const {
    n = 0;
    return 0;
  }
  virtual bool onSoftButton(int id) {
    (void)id;
    return false;
  }

  /*
   * ---- 原生 UI 页面（工具/设置类应用）----
   *
   * 画布（软渲染点阵字体）适合游戏，但"界面型"应用（时钟/计算器/设置）用 FlyThings
   * 原生控件渲染效果明显更好：矢量字体任意字号都清晰、按钮有原生按下态、布局改起来不用重编。
   *
   * 约定：nativeUi() 返回 true 的应用**完全不碰画布**，logic 层改为
   *   1) 显示对应的原生窗口（nativePage() 选模板）
   *   2) 每帧把 uiText()/uiButton() 同步到控件（只在变化时 setText）
   *   3) 把按钮点击通过 onUiButton() 回传
   * 触摸/按键/主循环时序与画布应用完全一致，所以两种应用可以混在同一个列表里。
   */
  virtual bool nativeUi() const { return false; }

  // 原生页面模板（决定用哪个窗口的控件集合）：
  //   0 = 时钟类（色条 + 标题 + 大字 + 状态行 + 副行 + 2x4 按钮）
  //   1 = 计算器（标题 + 表达式行 + 大字 + 5x4 键盘）
  virtual int nativePage() const { return 0; }

  // 文本槽（logic 层每帧读取，用于 setText）：
  //   0 = 主大字（倒计时 / 计算结果）
  //   1 = 状态行（专注中 / 已暂停 / 时间到！）
  //   2 = 副行（已完成 2 个 / 设定 5 分 / 计次）
  // 返回空串表示该行留空。buf 用于格式化，可返回 buf 或字面量。
  virtual const char *uiText(int slot, char *buf, int n) const {
    (void)slot;
    (void)buf;
    (void)n;
    return "";
  }

  enum { UI_BTN_MAX = 20 };  // 计算器要 20 个键，时钟类只用前 8 个

  // 按钮标签：返回空串 = 该按钮隐藏（控件 setVisible(false)）
  virtual const char *uiButton(int i) const {
    (void)i;
    return "";
  }
  // 按钮样式（logic 层映射成配色）：0 普通 1 主/确认(绿) 2 次/danger(暗红) 3 强调(蓝)
  virtual int uiButtonStyle(int i) const {
    (void)i;
    return 0;
  }
  // 按钮按下（原生按钮的点击回调最终走到这里）
  virtual bool onUiButton(int i) {
    (void)i;
    return false;
  }
  // 强调色（0 = 不改变）：用于顶部色条/主数值变色，例如番茄钟「专注红 / 休息绿」
  virtual uint32_t uiAccent() const { return 0; }

  // ---- HUD ----
  virtual const char *scoreLabel() const { return "得分"; }
  virtual int score() const = 0;
  virtual bool showBest() const { return true; }
  virtual const char *info1Label() const { return ""; }
  virtual const char *info1Value(char *buf, int n) const { (void)buf; (void)n; return ""; }
  virtual const char *info2Label() const { return ""; }
  virtual const char *info2Value(char *buf, int n) const { (void)buf; (void)n; return ""; }
  virtual const char *hint() const { return ""; }

  /* 游戏自带的调试命令（QA 通道：宿主 `gdbg <...>` 原样转发过来）。
   *
   * 为什么要有它：有些**交互过程**用触摸/按键很难稳定复现 —— 比如打地鼠的连击赞赏，
   * 得连续命中随机冒头的地鼠，而"点空一下就连击归零"，盲点几乎必然断链 ⇒ 没法验收。
   * 有了这条通道，游戏可以暴露"强制让第 i 个洞冒头/判定命中"这类最小钩子，
   * **走的是与真实操作完全同一条代码路径**（见 GameWhack::hitHole），
   * 所以 QA 验到的现象与实际玩到的一致（这条纪律见 docs/touch-inject.md §7）。
   *
   * 实现约定：命令串是"去掉前缀后的剩余部分"，如 `hit 3`；返回值无用。
   * ⚠️ 只加"让状态前进"的钩子，别加"直接改分"的（那会让验收失去意义）。 */
  virtual void debugCmd(const char *rest) { (void)rest; }

  /*
   * ---- 把"暂停键"当"互动键"用（2026-09-17 加，为电子宠物）----
   *
   * 宿主默认把暂停键解释成"暂停/继续"（见 mainLogic 的 handleLogicalKey）：
   * 运行中按一下 = 暂停、再按 = 继续，而且**根本不会走到 onKey()**。
   * 但有些应用（桌面宠物）压根没有"暂停"这个语义 —— 它要的是"每按一下都有反应"。
   * 覆盖本方法返回 true，宿主就把暂停键原样交给 `onKey(PG_KEY_A)`，**且不碰 state_**。
   * 默认 false ⇒ 既有游戏的行为一字不变（这条改动是纯增量的）。
   */
  virtual bool pauseKeyIsAction() const { return false; }

  /*
   * ---- 本页是不是"挂绳倒挂页"（2026-09-18 加，为电子宠物/机器人）----
   *
   * 背景（用户需求）：「屏幕挂绳在底部，屏保和机器人界面按下 C 按键切换成倒 180 度显示」。
   * 覆盖返回 true 的页面，在**它显示期间**：
   *   · 屏幕可以整屏倒 180°（用户按 C 切出来的意愿，落盘记住）——见 platform/PgFlip.h；
   *   · **短按暂停键 = 翻转 180°**，不再交给 `onKey(PG_KEY_A)`（长按仍是"返回列表"，不变）。
   * 默认 false ⇒ 既有游戏的行为一字不变（和其它钩子一样，是纯增量）。
   *
   * ⚠️ 覆盖它之前先确认：本页的"按键动作"在**触摸上全都有**（否则按键语义就是被拿走了）。
   *    电子宠物满足：点屏幕=摸头、左右滑=换表情、长按屏幕=表情面板。
   */
  virtual bool isFlipPage() const { return false; }

  // 画布下方按键条文案（随游戏而变）
  virtual const char *keyBar() const {
    return "暂停键 暂停/继续 · 长按 返回列表 · 音量键 调音量";
  }

  /*
   * ---- 画面是否**静止**（宿主据此跳过重绘，2026-09-15 加）----
   *
   * 为什么要有它：READY / OVER 这类覆盖层是**整屏半透明遮罩** + 几行文字，
   * 每帧重画一遍实测要 **8ms**（259200 像素的两两混合）—— 足以把所有游戏从
   * 60fps 拖到 **44fps**（用 QA `bench` 挖出来的，见 docs/game-art-pipeline.md §9）。
   * 而它其实一动不动：内容只在状态切换那一刻变。
   * ⇒ 宿主"进入静止态画一帧，之后不再重绘"，帧率立刻回到 60+。
   *
   * 默认实现 = "非运行中即静止"，覆盖 READY / OVER / PAUSED 三种覆盖层场景，
   * **大多数游戏不用管**。以下情况必须覆盖本方法（否则画面会卡住不动）：
   *   · 覆盖层上有动画（倒计时闪烁、分数滚动、特效还在播 —— 打地鼠的锤子回落
   *     与连击弹出就是）：把那些计时器一起判进来，动画期间返回 false。
   *   · RUNNING 时画面其实没动（纯回合制）：也可以返回 true 省电，但要求
   *     任何状态变化都体现在 state_ 或你自己判的条件里。
   */
  virtual bool stillFrame() const { return state_ != GSTATE_RUNNING; }

  /* 子类实现 stillFrame 的便捷写法：把覆盖层上**还在动的特效剩余时间**列进来。
   * 全部归零后才算静止 —— 否则"撞机抖屏""踩雷爆炸"这类特效会被冻在半路。
   * 例：`bool stillFrame() const { return stillUnless(shakeMs_); }` */
  bool stillUnless(int fxMs) const {
    return fxMs <= 0 && state_ != GSTATE_RUNNING;
  }
  bool stillUnless(int fx1, int fx2) const {
    return fx1 <= 0 && fx2 <= 0 && state_ != GSTATE_RUNNING;
  }

  // 状态机由 logic 层驱动；游戏只声明自己的意图
  GameState state() const { return state_; }
  void setState(GameState s) { state_ = s; }
  bool isPlaying() const { return state_ == GSTATE_RUNNING; }

  // 每帧上报，logic 层据此刷新 HUD 与最高分
  virtual bool justGameOver() { return false; }
  virtual void clearGameOverFlag() {}

 protected:
  void sfx(int id) {
    if (host_ && id > SFX_NONE && id < SFX_COUNT) host_->playSfx(id);
  }
  int best() const { return host_ ? host_->highScore(index_) : 0; }
  void commitBest(int s) {
    if (host_) host_->setHighScore(index_, s);
  }
  void saveBestIfNeeded(int s) {
    if (host_ && s > host_->highScore(index_)) host_->setHighScore(index_, s);
  }

  Host *host_;
  int index_;
  GameState state_;
  int vw_;
  int vh_;
};

}  // namespace pg

#endif  // PG_GAME_H_
