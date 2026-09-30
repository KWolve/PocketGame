/*
 * ToolPage.h - "原生工具页"的通用外壳（独立 ftu 用）
 *
 * 为什么有它：主界面拆分的口径是"**游戏不动，其余功能界面全部独立成 ftu**"
 * （见 docs/page-split-plan.md）。番茄钟 / 定时器 / 秒表 / 计算器 / 反应计时
 * 这些"原生控件工具页"拆出去之后，每一页都要同一套样板：
 *
 *   · 建一个 Game 子类实例、绑宿主、reset
 *   · 每帧 update(dt) + 把 uiText(slot)/uiButton(i)/hint()/keyBar() 同步到控件
 *     （**必须变化检测**：每帧无条件 setText/setBackgroundColor 会重绘风暴，实测 CPU 127%）
 *   · 三个物理键：108 短按 = 开始/暂停，108 长按（≥700ms）= 返回主列表，
 *     103/105 = 音量±（走进程级钩子 pg::volumeStepGlobal，与主界面同一条路）
 *   · 工作界面**禁屏保**（进页关、离页恢复）
 *   · 长按判定必须在应用层按时长做（本板 gpio-keys 无 autorepeat，
 *     框架的长按事件在实体键上永远不触发，见 docs/ui-design-baseline.md §9）
 *
 * 这些逐页复制 8 份必然各自漂，所以集中在这里。
 * 页面侧只剩：onUI_init 里 init()、onUI_show/hide/quit 转发、onUI_Timer 里 tick()、
 * onButtonClick_BtnU<i> 转发 button(i)、IKeyListener 里转发 keyDown/keyUp。
 */
#ifndef PG_UI_TOOLPAGE_H_
#define PG_UI_TOOLPAGE_H_

#include <stdint.h>

class ZKButton;
class ZKTextView;

namespace pg {

class Game;
class Host;

/** 主界面进程里的宿主（音效 + 存档）。定义在 src/logic/mainLogic.cc。
 *  独立 ftu 的页面直接复用它，不要各自 new（音频/存档是进程级单例语义）。 */
Host *pgHost();

/** 一次 init 需要的东西（控件指针全由 IDE 生成的 ui_<页名>.h 提供） */
struct ToolPageBinds {
  int slot;               // kAppTable 槽位（存档/最高分索引，也是 Game 的身份）
  const char *appName;    // 关闭用的 Activity 名，如 "stopwatchActivity"
  ZKTextView *title;      // 标题（写 Game::title()）
  ZKTextView *mainText;   // 主数值（uiText(0)）
  ZKTextView *phase;      // 状态行（uiText(1)）
  ZKTextView *sub;        // 副行（uiText(2)）
  ZKTextView *hint;       // 操作提示（hint()）
  ZKTextView *keyBar;     // 底部键位条（keyBar()）
  ZKTextView *accentBar;  // 顶部色条（uiAccent()；html 里是纯色 text 块）
  ZKButton **btns;        // BtnU0..BtnU<n-1>
  int btnCount;

  ToolPageBinds()
      : slot(-1), appName(0), title(0), mainText(0), phase(0), sub(0), hint(0),
        keyBar(0), accentBar(0), btns(0), btnCount(0) {}
};

enum ToolKeyResult {
  TOOL_KEY_IGNORE = 0,   // 不是我们的键：交回框架
  TOOL_KEY_HANDLED = 1,  // 已处理（吞掉）
  TOOL_KEY_EXIT = 2,     // 请求关闭本页（长按）——页面自己去 closeActivity
};

class ToolPage {
 public:
  ToolPage();

  /** 建 Game + 绑宿主 + 清缓存（在页面的 onUI_init 里调） */
  bool init(const ToolPageBinds &b);
  /** 进前台：注册按键、关屏保（在 onUI_show 里调） */
  void show();
  /** 退前台：反注册按键、恢复屏保（在 onUI_hide 里调） */
  void hide();
  /** 退出：销毁 Game（在 onUI_quit 里调） */
  void quit();

  /** 每帧：先 update 再同步控件（在 onUI_Timer 里调） */
  void tick(int dtMs);

  /** BtnU<i> 被点（标签为空的按钮会被隐藏，点了也当无效） */
  void button(int i);

  /** 按键转发。按下记时间戳，抬起按"按住时长 >= 700ms"判长按。 */
  ToolKeyResult keyDown(int keyCode, long nowMs);
  ToolKeyResult keyUp(int keyCode, long nowMs);

  /** 长按达标就返回 true（**不等抬起**）—— 由 tick() 内部调用，
   *  所以各工具页不用管。见 ToolPage.cpp 里 tick() 的说明。 */
  bool keyLongPressReady(long nowMs);

  /** 本页现在是不是前台（keyDown/keyUp 会据此忽略非前台事件） */
  bool active() const { return active_; }

  Game *game() const { return game_; }

 private:
  void syncAll();

  ToolPageBinds b_;
  Game *game_;
  bool active_;
  int downCode_;      // 按下的键码（-1 = 没有）
  long downMs_;       // 按下时刻
  bool longFired_;    // 长按是否已处理（避免抬起再触发一次）

  // 文本缓存：只在变化时写控件（每帧无条件写 = 重绘风暴）
  char cTitle_[64], cMain_[64], cPhase_[64], cSub_[64], cHint_[128], cKeyBar_[96];
  char btnText_[20][24];
  int btnStyle_[20];
  int btnShown_[20];
  uint32_t lastAccent_;
};

}  // namespace pg

#endif  // PG_UI_TOOLPAGE_H_
