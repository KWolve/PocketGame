/*
 * haLogic.cc - 智能家居页（独立 ftu：ha.ftu -> haActivity，slot 37）
 *
 * 界面在 ui/ha.html（生成 ui_ha.h），网络层 src/platform/PgHa.{h,cpp}，
 * "我的设备"持久列表 src/platform/PgHaList.{h,cpp}。
 * 设计稿 ui/ha.preview.html，方案与结论 docs/ha-ux-redesign.md。
 *
 * ★★ 2026-09-23 交互改版（v2）相对 v1 的差异：
 *   ① 主页 2x3 磁贴 + 翻页 → **2 列 x 4 行**（一屏 8 台），容器固定高度 + 滚动，**无翻页**；
 *   ② 页头右侧并排 **[+] + [设置]**（IoT App 做法），底部不再放 CTA；
 *   ③ 连接失败 = 一张**状态卡**（L1 发生了什么 → L2 上下文 → L3 影响 → L4 动作）；
 *   ④ 空态独立成组（与卡片网格互斥）；
 *   ⑤ 添加流程 = 三步向导：选择（多选）→ 命名（**原生 EditText，点它自动拉起拼音输入法**）
 *      → 完成；不再"点一下即加入"；
 *   ⑥ 卡片操作菜单（半屏）+ 移除二次确认；⑦ 调试动作收进设置页。
 *
 * ★★ 六条必须照做的纪律（都是本项目反复踩出来的）：
 *   ① **UI 线程是唯一能碰控件的地方**；PgHa 的工作线程绝不碰控件。
 *      本文件用 generation 比对 + 逐项变化检测，**变了才刷控件**（每帧无条件刷 = 重绘风暴）。
 *   ② **卡片必须逐格独立控件**：listview 的 subItem 图片**不按行区分**
 *      ⇒ 主页的"每格不同域"只能每格一套控件；列表页里"域块"用
 *      **同底色图 + 逐行 setText/setTextColor**（这两个是按行生效的）。
 *   ③ 卡片上"域块/菜单热区"都盖在卡片之上、会先吃到点 ⇒ **三条路都挂回调**，
 *      全部路由到同一函数，**不依赖任何"穿透"语义**（那是本工程的血案）。
 *   ④ **进页关屏保、离页恢复**；**响铃让位**（闹钟是跨页面的）。
 *   ⑤ **长按 ≥700ms 返回列表**（本板 gpio-keys 无 autorepeat，框架长按事件永不触发）。
 *   ⑥ 切窗口要**先把上层整屏 window 收起来**（源稿里最后的 window 在最上层）；
 *      本页用 showPage() 统一"七收一放"。
 *
 * ★★ 命名页的输入法约束（见 ui/ha.html 与 docs/ha-ux-redesign.md §4.7）：
 *   · 自定义 IME 的面板占 **y=372..800** ⇒ 命名页**所有可交互控件必须在 y<372**；
 *   · **键盘的「完成」键只负责"上屏"**（doneIMETextUpdate → 框架写回输入框）；
 *     页面上另有一个「完成」按钮负责"记下这一台并继续"（2026-09-24 用户报
 *     "命名后没有确认按键"后补的）。两者分工写清楚，别指望用户猜到；
 *   · 用户输入的文本从 `EditHaName->getText()` 取，**可能含任意中文字**（字库有 GB2312 一级兜底）。
 *
 * QA 通道：`/tmp/pg_hacmd`（整份内容变化才执行，每行可带 `#序号`）
 *   ping / states / dump [n] / listdump / fakeoff 1|0 / cfg / reload / quit
 *   win home|pick|name|done|menu|confirm|set   —— 切窗口
 *   card <0..7>      模拟点第 N 张卡（走与触摸完全相同的 cardTap）
 *   menu <0..7>      模拟点第 N 张卡的菜单热区
 *   pick <i>         在选择页勾选/取消第 i 行（真实实体表下标）
 *   seg ctrl|all     段控切换
 *   next             向导① 的"下一步"（收集勾选 → 进命名页）
 *   chip <0..5>      命名页点第 n 个推荐词片
 *   clearname        命名页"清空"
 *   typedone/nameok  命名页"提交当前输入 → 下一台或完成页"（键盘完成键 / 页级「完成」）
 *   skip / prevdev   命名页跳过 / 上一台
 *   cfyes / cfno     移除确认：确认 / 取消
 *   oneclick         一键添加全部可控设备
 *   add <id> / rm <id> / rename <id> <别名> / savelist / listload
 *   toggle <id>
 */
#ifdef FUN_BUILD
#include GENERATED_UI_DEFINITIONS
INIT_UI_EVENT_BINDINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

#include "control/ZKEditText.h"
#include "control/ZKListView.h"
#include "entry/EasyUIContext.h"
#include "platform/PgAlarm.h"      // 响铃让位
#include "platform/PgAudio.h"      // pg::volumeStepGlobal（音量键）
#include "platform/PgHa.h"         // HA 客户端
#include "platform/PgHaList.h"     // 我的设备（持久列表）
#include "platform/PgSaver.h"      // pg::wakeSaverByKey（屏保任意键唤醒）
#include "utils/Log.h"

/* ★★ FlyThings 的回调（onUI_* / onButtonClick_* / onListItemClick_*）必须在**全局作用域**
 *   —— 生成的 ui_ha.cpp 事件表按**全局名**取函数地址，放进匿名命名空间会变成另一个实体
 *   （链接期 undefined reference）。而被回调调用的大函数也得在全局作用域。 */
enum Page { PG_HOME = 0, PG_PICK, PG_NAME, PG_DONE, PG_MENU, PG_CONFIRM, PG_SET };

void haSyncStatus();
void haSyncHome();
void haSyncList();
void haSyncEnts();
void haQuit();
void haQuitAndCleanup();
void haPollCmd();
void showPage(Page p);
void syncSetPage();          // showPage(PG_SET) 要调它（进页即填值）
/* 命名页的两个小工具（定义在下面，但 onUI_init 的复位要先用 ⇒ 这里前置声明） */
void nameStepSay(const char *txt, unsigned color);
void nameSetBox(const std::string &t);
void cardTap(int i);
void menuTap(int i);

namespace {

const int TIMER_UI = 1;
/* 400ms 一拍：卡片上的"开启中/回滚"要跟手，但不至于每帧写控件。
 * ⚠️ 这不是"轮询 HA 的间隔" —— 那个由 PgHa 的 pollSec 决定（默认 3 秒，控制期间 250ms）。 */
const int UI_MS = 400;
const int kLongPressMs = 700;
const int kHintHoldMs = 6000;

/* 行缓存上限 = 我的设备上限（listview 的行数 = 实际设备数，不再是"固定 8 格"）。
 * ★ 2026-09-23 改 listview 时一起改：写死 8 会让第 9 台以后的设备**永远不刷新**
 *   （变化检测只覆盖前 8 行）。 */
const int kCardCount = pg::kHaListMax;

pg::Ha &ha() { return pg::Ha::inst(); }
pg::HaList &myList() { return pg::HaList::inst(); }

long long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ---------------- 本页视图状态 ---------------- */
Page sPage = PG_HOME;
/* ★ 2026-09-24：弹层（菜单/确认框）是 **modal**，要盖在"当前页"之上而不是取代它
 *   ⇒ 记住最近一个**非弹层**页，弹层显示时把它留在窗口表里（背景可见）。 */
Page sBasePage = PG_HOME;
/* ★ 「程序化写输入框」不是用户提交（wifi 那边踩过同样的坑）：
 *   syncNameHeader() / 词片 / 清空 都会 setText，而 setText 会**同步**触发
 *   onEditTextChanged_EditHaName ⇒ 不加标志就会把"我给的值"当成"用户上屏了"，
 *   于是出现"翻到第 2 台时提示'输入框是空的'"这种莫名其妙的话。
 *   ⚠️ setText 是同步回调 ⇒ 置位、写、立刻复位（中间不要有别的调用）。 */
bool sNameSilentSet = false;
std::vector<pg::HaDev> sMyDevs;          // 我的设备（快照）
unsigned sMyGen = 0xFFFFFFFFu;
std::vector<pg::HaEntity> sEnts;         // HA 实体（快照）
unsigned sEntGen = 0xFFFFFFFFu;

char sPhase[32] = {0};
char sCount[64] = {0};
char sHint[224] = {0};
long long sHintUntilMs = 0;
unsigned sPhaseColor = 0;
bool sCardsDirty = true;
/* ⚠️ 这两个"上次显隐"的初值必须是 **true**：控件在源稿里是可见的，而 HIDDEN_CONTROLS
 *   把它们改成了初始隐藏 ⇒ 逻辑层眼里的"当前状态"要与之相反，第一次 sync(false)
 *   才会真正执行隐藏。写成 false 会跳过首次同步（实测：空态文字盖在卡片上）。 */
bool sOfflineShown = true;
bool sEmptyShown = true;

/* ★ 可观测性打点（2026-09-23 实测逼出来的）：三个计数器 + 构建标签一起进 QA dump，
 *   一眼区分"定时器没跑 / 主页同步没跑 / 快照没换过"三种完全不同的原因。 */
const char *kBuildTag = "ha-p3-20260923";
/* 连接状态卡 L3 行的**设计文案**（与 ui/ha.html 的 TextHaOffL3 一致）
 * —— 离线态若有一次性的临时提示，就临时改用它，6 秒后自动回到这句。 */
const char *kOffL3Text = "未连接时不会发送控制命令，下面显示最后已知状态";
int sTickCount = 0;
int sHomeSyncCount = 0;
int sEntSyncCount = 0;
char sLastReject[200] = {0};

/* 每张卡上次下发的值（变化检测；避免每拍无脑写控件造成重绘） */
char sCardNm[kCardCount][96];
char sCardSt[kCardCount][48];
char sCardSb[kCardCount][112];
char sCardDic[kCardCount][4];

/* ---------------- 向导状态 ---------------- */
std::vector<int> sPickSel;               // 选择页勾选的实体表下标
bool sPickCtrlOnly = true;               // 段控：只看可控
std::vector<std::string> sNameIds;       // 待命名的设备 id（顺序 = 面板顺序）
std::vector<std::string> sNameTexts;     // 每台已输入的名字（空 = 用 HA 原名）
int sNameStep = 0;

/* ---------------- 菜单 / 确认 ---------------- */
int sMenuIdx = -1;                       // 菜单对着第几张卡（-1 = 无）

/* ---------------- 按键状态 ---------------- */
int sDownCode = -1;
long long sDownMs = 0;
bool sLPFired = false;

/* ==================== 物理按键 ==================== */
class HaKeys : public EasyUIContext::IKeyListener {
 public:
  bool onKeyEvent(const KeyEvent &ke) override {
    /* ★ 屏保开着：任意键只负责唤醒（吞掉本次按键）。必须在第一句。 */
    if (pg::wakeSaverByKey(ke)) return true;

    long long t = nowMs();
    if (ke.mKeyStatus == KeyEvent::E_KEY_DOWN) {
      sDownCode = ke.mKeyCode;
      sDownMs = t;
      sLPFired = false;
      return true;
    }
    if (ke.mKeyStatus == KeyEvent::E_KEY_LONG_PRESS) return true;   // 无 repeat，忽略
    if (ke.mKeyStatus != KeyEvent::E_KEY_UP) return false;
    if (ke.mKeyCode != sDownCode) return false;

    int held = (int)(t - sDownMs);
    sDownCode = -1;
    if (held >= kLongPressMs && !sLPFired) {
      LOGD("haLogic: 长按 %dms -> 返回列表", held);
      haQuitAndCleanup();
      return true;
    }
    if (ke.mKeyCode == 105 || ke.mKeyCode == 103) {
      int pct = pg::volumeStepGlobal(ke.mKeyCode == 105 ? 1 : -1);
      LOGD("haLogic: 音量%s -> %d%%", ke.mKeyCode == 105 ? "+" : "-", pct);
      return true;
    }
    if (ke.mKeyCode == 108) {
      LOGD("haLogic: 短按 108 -> 刷新实体");
      ha().reqStates();
      return true;
    }
    return true;
  }
};
HaKeys sHaKeys;

/* ==================== 小工具 ==================== */
void hintSay(const char *s) {
  snprintf(sHint, sizeof(sHint), "%s", s ? s : "");
  sHintUntilMs = nowMs() + kHintHoldMs;
}
void hintKeep() { sHintUntilMs = nowMs() + kHintHoldMs; }

/* 心跳文件：**本板 logcat 缓冲只有十几行**，"定时器还在不在跑"在日志里看不出来 ⇒ 落文件。 */
void tickHeartbeat() {
  static long long lastMs = 0;
  long long t = nowMs();
  if (t - lastMs < 1000) return;
  lastMs = t;
  FILE *f = fopen("/tmp/pg_ha_tick.txt", "w");
  if (!f) return;
  fprintf(f, "tick=%d homeSync=%d entSync=%d win=%d\n",
          sTickCount, sHomeSyncCount, sEntSyncCount, (int)sPage);
  fclose(f);
}

void cardsDirty() { sCardsDirty = true; }

/* 卡片的三个按钮（底/域块/菜单热区）都路由到 cardTap / menuTap；
 * 名字/状态/副行是 textview（html2json 生成时 touchable=false，不吃触摸）。
 * ⚠️ 这里**不用宏**：宏参数会把 `int i` 里的 i 一起替换掉（写成 CARDCTRL(i) 会展开出
 *    `int 0` 这种非法声明，已踩一次）。老老实实写五个取指针函数。 */
/* ★ 2026-09-23 改 listview 后不再需要"按格取控件指针"的 5 个函数
 *   （cardBtn/dicBtn/nmTv/stTv/sbTv）—— 行内控件由 obtainListItemData_ListHaMy 回填。
 *   行下标 == 我的设备下标（见 cardDevIdx），点击/菜单都靠行下标定位。 */

const char *domCn(int domain) {
  switch (domain) {
    case pg::HD_LIGHT: return "灯";
    case pg::HD_SWITCH: return "开关";
    case pg::HD_SCENE: return "场景";
    case pg::HD_SCRIPT: return "脚本";
    case pg::HD_AUTOMATION: return "自动化";
    case pg::HD_BUTTON: return "按钮";
    case pg::HD_CLIMATE: return "空调";
    case pg::HD_COVER: return "幕布";
    case pg::HD_MEDIA_PLAYER: return "播放器";
    case pg::HD_FAN: return "风扇";
    case pg::HD_LOCK: return "门锁";
    case pg::HD_CAMERA: return "摄像头";
    case pg::HD_SENSOR: return "传感器";
    case pg::HD_BINARY_SENSOR: return "二元传感器";
    case pg::HD_INPUT_BOOLEAN: return "开关量";
    case pg::HD_NUMBER: return "数值";
    case pg::HD_SELECT: return "选择";
    case pg::HD_VACUUM: return "扫地机";
    default: return "其它";
  }
}

/* 域块里的那个汉字（一个字，和 domCn 的前缀保持一致；都在 GB2312 一级字库内）。 */
const char *domGlyph(int domain) {
  switch (domain) {
    case pg::HD_LIGHT: return "灯";
    case pg::HD_SWITCH: return "开";
    case pg::HD_SCENE: return "景";
    case pg::HD_SCRIPT: return "本";
    case pg::HD_AUTOMATION: return "自";
    case pg::HD_BUTTON: return "钮";
    case pg::HD_CLIMATE: return "调";
    case pg::HD_COVER: return "幕";
    case pg::HD_MEDIA_PLAYER: return "播";
    case pg::HD_FAN: return "扇";
    case pg::HD_LOCK: return "锁";
    case pg::HD_CAMERA: return "像";
    case pg::HD_SENSOR: return "感";
    case pg::HD_BINARY_SENSOR: return "感";
    case pg::HD_VACUUM: return "扫";
    default: return "设";
  }
}

/* 域块的文字色（逐行 setTextColor 用；域块底图是所有行共用的一张灰圆角图）。 */
unsigned domColor(int domain) {
  switch (domain) {
    case pg::HD_LIGHT: return 0xFFEF9F27u;      // 琥珀
    case pg::HD_SWITCH: return 0xFF64D2FFu;     // 青
    case pg::HD_SCENE: return 0xFFBF5AF2u;      // 紫
    case pg::HD_COVER: return 0xFFFF9F0Au;      // 橙
    case pg::HD_CLIMATE: return 0xFF0A84FFu;    // 蓝
    case pg::HD_SENSOR: return 0xFF8E8E93u;     // 灰
    case pg::HD_BINARY_SENSOR: return 0xFF8E8E93u;
    default: return 0xFF9A9AA0u;
  }
}

/* 状态短文案（"已开 / 已关 / 离线 / 点按执行"）+ 颜色。 */
void stateShort(const pg::HaEntity &e, char *out, int n, unsigned *color) {
  if (e.isOfflineState()) {
    snprintf(out, n, "离线");
    *color = 0xFFFF9F0Au;
    return;
  }
  if (strcmp(e.state, "on") == 0) { snprintf(out, n, "已开"); *color = 0xFF30D158u; return; }
  if (strcmp(e.state, "off") == 0) { snprintf(out, n, "已关"); *color = 0xFF9A9AA0u; return; }
  if (pg::haDomainMomentary(e.domain)) { snprintf(out, n, "点按执行"); *color = 0xFF64D2FFu; return; }
  if (e.unit[0]) { snprintf(out, n, "%s%s", e.state, e.unit); *color = 0xFF64D2FFu; return; }
  snprintf(out, n, "%s", e.state[0] ? e.state : "—");
  *color = 0xFF64D2FFu;
}

/* ★ 按**像素宽**截断（textview 不会自动省略号）。
 *   实测血案：`Z20 Smart Panel away`（20 字节）按字节截断刚好通过，但 120px 盒里横向溢出。 */
/* 文本按像素宽量（中文 1 字宽 / ASCII 0.55 字宽 —— 与 html2json 的最小尺寸公式同口径） */
int textWidthPx(const char *s, int fs) {
  int w = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    int bytes = (*p < 0x80) ? 1 : ((*p & 0xE0) == 0xC0 ? 2 : ((*p & 0xF0) == 0xE0 ? 3 : 4));
    w += (*p < 0x80) ? (fs * 55) / 100 : fs;
    p += bytes;
  }
  return w;
}

/* 名字盒截断 —— ★ **优先保留尾部**（2026-09-23 用户实测逼出来的）
 * 血案：alias 为空时用 HA 原名 "Z20 Smart Panel 灯带"，按**头**截断只显示成 "Z20 S"
 *      —— 信息量为 0，用户看到一串没意义的型号前缀。
 *      HA 的命名惯例是「设备型号 + 位置/用途」⇒ 信息量在**尾部**。
 * 规则：① 整条放得下 → 用整条；
 *      ② 放不下且含空格 → 先用**最后一个空格之后**的部分（"灯带" / "客厅灯"）；
 *      ③ 还放不下 → 从尾部往前找"最长的能放下的后缀"。 */
void truncNamePx(const char *src, char *out, int n, int maxPx, int fs) {
  if (!src || !n) { if (out && n) out[0] = 0; return; }
  if (textWidthPx(src, fs) <= maxPx) { snprintf(out, n, "%s", src); return; }

  const char *tail = 0;
  for (const char *q = src; *q; ++q) if (*q == ' ') tail = q + 1;
  if (tail && *tail && textWidthPx(tail, fs) <= maxPx) { snprintf(out, n, "%s", tail); return; }

  const char *use = (tail && *tail) ? tail : src;
  int len = (int)strlen(use);
  for (int st = 0; st < len; ++st) {
    if (((unsigned char)use[st] & 0xC0) == 0x80) continue;      // 不是字符起点
    if (textWidthPx(use + st, fs) <= maxPx) { snprintf(out, n, "%s", use + st); return; }
  }
  out[0] = 0;                                                    // 极端：连一个字都放不下
}

const pg::HaEntity *findEnt(const char *id) {
  for (size_t i = 0; i < sEnts.size(); ++i) {
    if (strcmp(sEnts[i].id, id) == 0) return &sEnts[i];
  }
  return 0;
}

/* 卡片 i 对应的"我的设备"下标；-1 = 该格为空。 */
int cardDevIdx(int i) { return (i >= 0 && i < (int)sMyDevs.size()) ? i : -1; }

/* ==================== 推荐词片（按域） ====================
 * ★ 为什么在 .cc 里放一张局部表而不是动 PgHaList：
 *   平台层的 kAliases 是"通用池"，改成按域要动接口与存档邻域；
 *   这里只需"给 6 个推荐词"，放本页最省事、也不会影响别处。
 *   这些字都在 GB2312 一级字库内（gen_font.py 的兜底字集），不会整字消失。 */
const char *kChipLight[6] = {"客厅灯", "卧室灯", "台灯", "吊灯", "主灯", "夜灯"};
const char *kChipSwitch[6] = {"电脑开关", "电视开关", "风扇", "插座", "加湿器", "净化器"};
const char *kChipScene[6] = {"回家", "离家", "睡觉", "观影", "起床", "全关"};
const char *kChipCover[6] = {"窗帘", "幕布", "客厅窗帘", "卧室窗帘", "卷帘", "电动帘"};
const char *kChipCommon[6] = {"客厅", "卧室", "书房", "餐厅", "玄关", "厨房"};

const char **chipsFor(int domain) {
  switch (domain) {
    case pg::HD_LIGHT: return kChipLight;
    case pg::HD_SWITCH: return kChipSwitch;
    case pg::HD_SCENE: return kChipScene;
    case pg::HD_COVER: return kChipCover;
    default: return kChipCommon;
  }
}

}  // namespace

/* ==================================================================
 *                    FlyThings 回调（名字由 caption 决定）
 * ================================================================== */

static S_ACTIVITY_TIMEER REGISTER_ACTIVITY_TIMER_TAB[] = {
    {TIMER_UI, UI_MS},
};

static void onUI_init() {
#ifdef FUN_BUILD
  INIT_UI_TIMERS
#endif
  LOGD("haLogic: init（配置 %s://%s:%d poll=%d）", ha().cfg().tls ? "https" : "http",
       ha().cfg().host, ha().cfg().port, ha().cfg().pollSec);
  /* 缓存全部作废，强制首帧刷一遍 */
  sEntGen = 0xFFFFFFFFu;
  sMyGen = 0xFFFFFFFFu;
  sPhase[0] = 0;
  sCount[0] = 0;
  sHint[0] = 0;
  sHintUntilMs = 0;
  sPhaseColor = 0;
  sPage = PG_HOME;
  sCardsDirty = true;
  sOfflineShown = true;      // 见上面的说明：初值 = "控件现在可见"，与 HIDDEN 相反
  sEmptyShown = true;
  sPickSel.clear();
  sPickCtrlOnly = true;
  sNameIds.clear();
  sNameTexts.clear();
  sNameStep = 0;
  sMenuIdx = -1;
  sLastReject[0] = 0;
  for (int i = 0; i < kCardCount; ++i) {
    sCardNm[i][0] = 0; sCardSt[i][0] = 0; sCardSb[i][0] = 0; sCardDic[i][0] = 0;
  }
  /* ★ 输入框内容要清空：ZKEditText 的文本是**控件状态**，重进页会留着上次的字。 */
  nameSetBox("");

  myList().load();
  if (!ha().start()) LOGW("haLogic: PgHa 没起来（缺 /data/ha.conf？），本页会显示未连接");
  ha().reqConfig();
  showPage(PG_HOME);
}

static void onUI_intent(const Intent *intent) { (void)intent; }

static void onUI_show() {
  LOGD("haLogic: show");
  EASYUICONTEXT->setScreensaverEnable(false);
  EASYUICONTEXT->registerKeyListener(&sHaKeys);
  if (!ha().running()) {
    if (!ha().start()) LOGW("haLogic: 重新启动失败（配置缺失？）");
  }
  ha().reqStates();
}

static void onUI_hide() {
  LOGD("haLogic: hide");
  EASYUICONTEXT->unregisterKeyListener(&sHaKeys);
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  ha().stop();
}

static void onUI_quit() {
  LOGD("haLogic: quit");
  EASYUICONTEXT->unregisterKeyListener(&sHaKeys);
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  ha().stop();
}

static void onProtocolDataUpdate(const SProtocolData &data) { (void)data; }

static bool onUI_Timer(int id) {
  if (id != TIMER_UI) return true;
  ++sTickCount;

  /* ★ 响铃让位：闹钟是跨页面的，不写这句在别的应用里响铃会"只有声音没有界面"。 */
  if (pg::Alarm::instance().ringing()) {
    LOGD("haLogic: 响铃中 -> 让位");
    haQuitAndCleanup();
    return false;
  }

  haPollCmd();
  haSyncStatus();
  haSyncEnts();                       // 实体快照：主页与选择页都要用
  if (sPage == PG_HOME) haSyncHome();
  else if (sPage == PG_PICK) haSyncList();
  tickHeartbeat();
  return true;
}

static bool onhaActivityTouchEvent(const MotionEvent &ev) {
  (void)ev;
  return false;
}

/* ==================================================================
 *                          主页（我的设备）
 * ================================================================== */

void haSyncEnts() {
  if (ha().generation() == sEntGen) return;
  sEntGen = ha().generation();
  ++sEntSyncCount;
  ha().copyEntities(&sEnts);
  if (sPage == PG_PICK && mListHaPickPtr) mListHaPickPtr->refreshListView();
}

/* 连接失败状态卡的显隐（只在变化时动控件） */
void syncOfflineCard(bool show) {
  if (show == sOfflineShown) return;
  sOfflineShown = show;
  if (mCardHaOfflinePtr) mCardHaOfflinePtr->setVisible(show);
  if (mTextHaOffL1Ptr) mTextHaOffL1Ptr->setVisible(show);
  if (mTextHaOffL2Ptr) mTextHaOffL2Ptr->setVisible(show);
  if (mTextHaOffL3Ptr) mTextHaOffL3Ptr->setVisible(show);
  if (mBtnHaOffSetPtr) mBtnHaOffSetPtr->setVisible(show);
  /* ★ 设备列表整体给状态卡让位：在线 158..774 / 离线 212..774。
   *   原来没让位 ⇒ 离线时**第一行被状态卡压掉 42px**（逐像素实测：98..200 全被盖住），
   *   用户看到的是"第一排卡片缺了个头"。 */
  if (mListHaMyPtr) {
    mListHaMyPtr->setPosition(LayoutPosition(12, show ? 212 : 158, 456, show ? 562 : 616));
  }
  LOGD("haLogic: 连接状态卡 %s", show ? "显示" : "隐藏");
}

/* 空态的显隐（与卡片网格互斥） */
void syncEmptyGroup(bool show) {
  if (show == sEmptyShown) return;
  sEmptyShown = show;
  if (mImgHaEmptyPtr) mImgHaEmptyPtr->setVisible(show);
  if (mTextHaEmpTPtr) mTextHaEmpTPtr->setVisible(show);
  if (mTextHaEmpL1Ptr) mTextHaEmpL1Ptr->setVisible(show);
  if (mTextHaEmpL2Ptr) mTextHaEmpL2Ptr->setVisible(show);
  if (mTextHaEmpL3Ptr) mTextHaEmpL3Ptr->setVisible(show);
  if (mBtnHaEmpAddPtr) mBtnHaEmpAddPtr->setVisible(show);
  if (mBtnHaEmpOnePtr) mBtnHaEmpOnePtr->setVisible(show);
  if (mTextHaEmpHintPtr) mTextHaEmpHintPtr->setVisible(show);
  /* ★ 设备列表：**没有设备时行数就是 0**，listview 什么都不画 ⇒ 不需要再"逐个隐藏
   *   8 张卡片"（那正是用户看到的"空卡片 + 空域块"）。这里只把列表整体收起，
   *   空态下连滚动条都不会出现。 */
  if (mListHaMyPtr) mListHaMyPtr->setVisible(!show);
}
/* 第 i 行的显示内容（**唯一真值**：haSyncHome 用它做变化检测，
 *  obtainListItemData_ListHaMy 用它回填控件 ⇒ 不会出现两处口径不一致）。 */
void myRowText(int i, char *nm, int nN, char *st, int nS, unsigned *stc,
               char *sb, int nB, char *dg, int nD, unsigned *dgc) {
  nm[0] = 0; st[0] = 0; sb[0] = 0; dg[0] = 0;
  *stc = 0xFF9A9AA0u; *dgc = 0xFF8E8E93u;
  if (i < 0 || i >= (int)sMyDevs.size()) return;

  const pg::HaDev &d = sMyDevs[i];
  const pg::HaEntity *e = findEnt(d.id);
  char haName[64], disp[128];
  if (e) snprintf(haName, sizeof(haName), "%s", e->name);
  else snprintf(haName, sizeof(haName), "%s", d.id);
  pg::HaList::displayName(d, haName, disp, sizeof(disp));
  truncNamePx(disp, nm, nN, 88, 17);              // 名字盒 88px @ fs17（**取尾部**）

  char pend[16] = {0};
  int pendState = ha().ctlPending(d.id, pend, sizeof(pend));
  if (!e) {
    snprintf(st, nS, "找不到");
    snprintf(sb, nB, "HA 实体表里没有这个 id");
    *stc = 0xFFFF9F0Au;
  } else if (pendState) {
    /* 乐观态：按**目标状态**画；单向触发域没有目标 ⇒ 沿用它当前状态 */
    snprintf(st, nS, "%s", pend[0] ? (strcmp(pend, "on") == 0 ? "开启中" : "关闭中") : "已发出");
    snprintf(sb, nB, "%s", domCn(e->domain));
    *stc = 0xFF64D2FFu;
  } else {
    stateShort(*e, st, nS, stc);
    snprintf(sb, nB, "%s%s", domCn(e->domain), e->isOfflineState() ? " · 设备不在线" : "");
  }
  if (e) {
    snprintf(dg, nD, "%s", domGlyph(e->domain));
    *dgc = domColor(e->domain);
  } else {
    snprintf(dg, nD, "?");
  }
}

void haSyncHome() {
  ++sHomeSyncCount;

  /* ① 我的设备变了就重取快照 */
  if (myList().generation() != sMyGen) {
    sMyGen = myList().generation();
    myList().copy(&sMyDevs);
    cardsDirty();
  }

  const bool empty = sMyDevs.empty();
  syncEmptyGroup(empty);

  /* ② 逐行算值 + 变化检测：**只有内容真的变了才 refreshListView()**
   *    （每拍无脑刷会让列表不停重画；本板没 GPU，重画是肉眼可见的顿） */
  bool changed = false;
  if (sCardsDirty) {
    sCardsDirty = false;
    changed = true;
    for (int i = 0; i < kCardCount; ++i) {
      sCardNm[i][0] = 0; sCardSt[i][0] = 0; sCardSb[i][0] = 0; sCardDic[i][0] = 0;
    }
  }
  int n = (int)sMyDevs.size();
  if (n > kCardCount) n = kCardCount;
  for (int i = 0; i < n; ++i) {
    char nm[96], st[48], sb[112], dg[8];
    unsigned stc = 0, dgc = 0;
    myRowText(i, nm, sizeof(nm), st, sizeof(st), &stc, sb, sizeof(sb), dg, sizeof(dg), &dgc);
    if (strcmp(sCardNm[i], nm) != 0 || strcmp(sCardSt[i], st) != 0
        || strcmp(sCardSb[i], sb) != 0 || strcmp(sCardDic[i], dg) != 0) {
      changed = true;
      snprintf(sCardNm[i], sizeof(sCardNm[i]), "%s", nm);
      snprintf(sCardSt[i], sizeof(sCardSt[i]), "%s", st);
      snprintf(sCardSb[i], sizeof(sCardSb[i]), "%s", sb);
      snprintf(sCardDic[i], sizeof(sCardDic[i]), "%s", dg);
    }
  }
  if (changed && mListHaMyPtr) mListHaMyPtr->refreshListView();

  /* ③ 状态条右侧"N 台" */
  char b[64];
  snprintf(b, sizeof(b), "%d 台", (int)sMyDevs.size());
  if (strcmp(sCount, b) != 0) {
    snprintf(sCount, sizeof(sCount), "%s", b);
    if (mTextHaCountPtr) mTextHaCountPtr->setText(sCount);
  }
}

/* ---------------- listview 三件套（我的设备） ----------------
 * ★ 2026-09-23：主页由"8 张固定卡片"改成 **2 列 listview**（用户要求）：
 *   ① 行数 = 实际设备数 ⇒ 没有空卡片/空域块
 *   ② subItem 不挂圆角图、data-bg 会被 html2json 丢掉 ⇒ 文字直接画在行底上，
 *      彻底消灭"圆角块四角透窗口黑底"的黑角（真机血案）
 *   ③ 行内控件由这里回填，取值统一走 myRowText()（与变化检测同源） */
static int getListItemCount_ListHaMy(const ZKListView *pListView) {
  (void)pListView;
  return (int)sMyDevs.size();
}

static void obtainListItemData_ListHaMy(ZKListView *pListView,
                                       ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  if (!pListItem) return;
  ZKListView::ZKListSubItem *dic = pListItem->findSubItemByID(ID_HA_SubMyDic);
  ZKListView::ZKListSubItem *nm = pListItem->findSubItemByID(ID_HA_SubMyNm);
  ZKListView::ZKListSubItem *st = pListItem->findSubItemByID(ID_HA_SubMySt);
  ZKListView::ZKListSubItem *sb = pListItem->findSubItemByID(ID_HA_SubMySb);

  char a[96], b[48], c[112], d[8];
  unsigned stc = 0, dgc = 0;
  myRowText(index, a, sizeof(a), b, sizeof(b), &stc, c, sizeof(c), d, sizeof(d), &dgc);
  if (nm) nm->setText(a);
  if (st) { st->setText(b); st->setTextColor(stc); }
  if (sb) sb->setText(c);
  if (dic) { dic->setText(d); dic->setTextColor(dgc); }
}

/* ★ 子项点击：`id` 就是**被点中的子项 id**（官方口径，见 wiki/devflow/callback_functions.md
 *   的 switch(id) 范例）⇒ 行尾「•••」热区与"点行控制设备"能靠 id 区分，两条路都通。 */
static void onListItemClick_ListHaMy(ZKListView *pListView, int index, int id) {
  (void)pListView;
  LOGD("haLogic: 我的设备 点击 row=%d subId=%d", index, id);
  if (id == ID_HA_SubMyMenu) menuTap(index);
  else cardTap(index);
}
/* ---------- 卡片交互 ---------- */

/* 点卡片（卡片底 / 域块 / 菜单热区 三条路都走这里 —— 不依赖"穿透"） */
void cardTap(int i) {
  int idx = i;                         // listview 的行下标 == 我的设备下标
  if (idx < 0 || idx >= (int)sMyDevs.size()) return;
  const char *id = sMyDevs[idx].id;
  const pg::HaEntity *e = findEnt(id);
  if (!e) {
    char b[200];
    snprintf(b, sizeof(b), "「%s」不在 HA 的实体表里（改名了/删了？）",
             sMyDevs[idx].alias[0] ? sMyDevs[idx].alias : id);
    hintSay(b);
    return;
  }
  /* ★★ 本地先把"为什么不能点"说清楚 —— 这是"点了没反应"的唯一解药。
   *   实测：给 unavailable 的实体发命令是 200 但状态永久不变 ⇒ 必须禁点并说明。 */
  if (!pg::haDomainControllable(e->domain)) {
    char b[200];
    snprintf(b, sizeof(b), "「%s」是只读的%s，不能开关（只显示状态）", e->name, domCn(e->domain));
    hintSay(b);
    return;
  }
  if (!pg::haDomainMomentary(e->domain) && !e->isOnOff()) {
    char b[220];
    snprintf(b, sizeof(b), "「%s」离线，先别点（发了也不会变）", e->name);
    hintSay(b);
    return;
  }
  if (ha().reqToggle(id)) {
    sHint[0] = 0; sHintUntilMs = 0; sLastReject[0] = 0;
  } else {
    const char *rj = ha().ctlReject();
    if (rj && rj[0]) snprintf(sHint, sizeof(sHint), "没发出去：%s", rj);
    else snprintf(sHint, sizeof(sHint), "「%s」这次没发出去", e->name);
    hintKeep();
    snprintf(sLastReject, sizeof(sLastReject), "%s", sHint);
  }
}

/* 点卡片右上角菜单热区 */
void menuTap(int i) {
  int idx = i;                         // 同上：行下标 == 设备下标
  if (idx < 0 || idx >= (int)sMyDevs.size()) return;
  sMenuIdx = idx;
  const pg::HaDev &d = sMyDevs[idx];
  const pg::HaEntity *e = findEnt(d.id);
  char disp[96];
  pg::HaList::displayName(d, e ? e->name : d.id, disp, sizeof(disp));
  if (mTextHaMenuTPtr) mTextHaMenuTPtr->setText(disp);
  if (mTextHaMenuSPtr) {
    char b[160];
    unsigned c = 0;
    char st[48] = "—";
    if (e) stateShort(*e, st, sizeof(st), &c);
    snprintf(b, sizeof(b), "%s · %s", e ? domCn(e->domain) : "未知", st);
    mTextHaMenuSPtr->setText(b);
  }
  showPage(PG_MENU);
  LOGD("haLogic: 打开卡片菜单 idx=%d (%s)", idx, d.id);
}

/* 移除一张卡（确认之后调） */
void doRemoveCard() {
  if (sMenuIdx < 0 || sMenuIdx >= (int)sMyDevs.size()) return;
  char id[72];
  snprintf(id, sizeof(id), "%s", sMyDevs[sMenuIdx].id);
  if (myList().remove(id)) {
    hintSay("已移除。Home Assistant 里的设备没有变");
    cardsDirty();
    LOGD("haLogic: 已移除 %s", id);
  } else {
    hintSay("移除失败（这台不在我的设备里？）");
  }
  sMenuIdx = -1;
  showPage(PG_HOME);
}

/* ==================================================================
 *                        选择页（向导 ①）
 * ================================================================== */

/* 段控过滤后的实体下标表 */
void buildPickRows(std::vector<int> *out) {
  out->clear();
  for (size_t i = 0; i < sEnts.size(); ++i) {
    if (sPickCtrlOnly && !pg::haDomainControllable(sEnts[i].domain)) continue;
    out->push_back((int)i);
  }
}

int pickRowCount() {
  std::vector<int> rows;
  buildPickRows(&rows);
  return (int)rows.size();
}

int pickIndexOfEntity(int entIdx) {
  std::vector<int> rows;
  buildPickRows(&rows);
  for (size_t i = 0; i < rows.size(); ++i) if (rows[i] == entIdx) return (int)i;
  return -1;
}

bool pickIsSelected(int entIdx) {
  for (size_t i = 0; i < sPickSel.size(); ++i) if (sPickSel[i] == entIdx) return true;
  return false;
}

void pickToggle(int entIdx, const char *name) {
  if (entIdx < 0 || entIdx >= (int)sEnts.size()) return;
  if (myList().indexOf(sEnts[entIdx].id) >= 0) {
    hintSay("这台已经在面板里了");
    return;
  }
  bool removed = false;
  for (size_t i = 0; i < sPickSel.size(); ++i) {
    if (sPickSel[i] == entIdx) { sPickSel.erase(sPickSel.begin() + i); removed = true; break; }
  }
  if (!removed) sPickSel.push_back(entIdx);
  char b[120];
  snprintf(b, sizeof(b), "已选 %d 台", (int)sPickSel.size());
  hintSay(b);
  if (mListHaPickPtr) mListHaPickPtr->refreshListView();
  /* 底部确认条 */
  if (mTextHaSelSumPtr) {
    char s[96];
    snprintf(s, sizeof(s), "已选 %d 台（上限 %d 台）", (int)sPickSel.size(), pg::kHaListMax);
    mTextHaSelSumPtr->setText(s);
  }
  if (mBtnHaPickNextPtr) {
    if (sPickSel.empty()) {
      mBtnHaPickNextPtr->setText("请选择要添加的设备");
      mBtnHaPickNextPtr->setTextColor(0xFF8E8E93u);
    } else {
      char s[96];
      snprintf(s, sizeof(s), "下一步 · 命名（%d 台）", (int)sPickSel.size());
      mBtnHaPickNextPtr->setText(s);
      mBtnHaPickNextPtr->setTextColor(0xFF000000u);
    }
  }
  (void)name;
}

/* 段控文字（可控 N / 全部 N） */
void syncSegTexts() {
  int ctrl = 0;
  for (size_t i = 0; i < sEnts.size(); ++i) if (pg::haDomainControllable(sEnts[i].domain)) ++ctrl;
  char b[48], b2[48];
  snprintf(b, sizeof(b), "可控 %d", ctrl);
  snprintf(b2, sizeof(b2), "全部 %d", (int)sEnts.size());
  if (mSegHaCtrlPtr) mSegHaCtrlPtr->setText(b);
  if (mSegHaAllPtr) mSegHaAllPtr->setText(b2);
  if (mSegHaAllOnPtr) mSegHaAllOnPtr->setText(b2);
  /* ★ 选中态用**两个按钮叠一格**：运行时 setBackgroundColor 会把圆角九宫格图清掉
   *   （实机血案）⇒ 只切 setVisible，不碰底色。 */
  /* ★★ 两列段控各是"两个按钮叠一格"：**灰态与蓝态必须互斥**。
   *   实测血案（2026-09-23 逐点取色发现）：把四个都按同一个条件设可见 ⇒
   *     · "可控"整块消失（两个都不可见 ⇒ 露出窗口黑底，取到 (0,0,0)）
   *     · "全部"两个叠在一起（取到蓝色，分不清选中）
   *   ⇒ 四个的可见性必须**两两相反**。 */
  if (mSegHaCtrlPtr) mSegHaCtrlPtr->setVisible(!sPickCtrlOnly);     // 灰 = 未选中
  if (mSegHaCtrlOnPtr) mSegHaCtrlOnPtr->setVisible(sPickCtrlOnly);  // 蓝 = 选中
  if (mSegHaAllPtr) mSegHaAllPtr->setVisible(sPickCtrlOnly);
  if (mSegHaAllOnPtr) mSegHaAllOnPtr->setVisible(!sPickCtrlOnly);
  /* ★ 选中态的"可控"要显示**可控 N**，不是"全部 N"（曾经误用 b2 ⇒ 文字与状态自相矛盾） */
  if (mSegHaCtrlOnPtr) mSegHaCtrlOnPtr->setText(b);
}

void haSyncList() {
  syncSegTexts();
  char b[96];
  snprintf(b, sizeof(b), "已选 %d 台", (int)sPickSel.size());
  if (mTextHaSelSumPtr) mTextHaSelSumPtr->setText(b);
}

static int getListItemCount_ListHaPick(const ZKListView *pListView) {
  (void)pListView;
  return pickRowCount();
}

static void obtainListItemData_ListHaPick(ZKListView *pListView, ZKListView::ZKListItem *pListItem, int index) {
  (void)pListView;
  if (!pListItem) return;
  ZKListView::ZKListSubItem *dic = pListItem->findSubItemByID(ID_HA_SubPickDic);
  ZKListView::ZKListSubItem *nm = pListItem->findSubItemByID(ID_HA_SubPickName);
  ZKListView::ZKListSubItem *inf = pListItem->findSubItemByID(ID_HA_SubPickInfo);
  ZKListView::ZKListSubItem *tag = pListItem->findSubItemByID(ID_HA_SubPickTag);
  /* ⚠️ 行视图**跨行复用** ⇒ 每个 subItem 每轮都要显式写值（包括清空）。 */
  std::vector<int> rows;
  buildPickRows(&rows);
  if (index < 0 || index >= (int)rows.size()) {
    if (dic) dic->setText("");
    if (nm) nm->setText("");
    if (inf) inf->setText("");
    if (tag) tag->setText("");
    return;
  }
  const pg::HaEntity &e = sEnts[rows[index]];
  bool added = (myList().indexOf(e.id) >= 0);
  bool sel = pickIsSelected(rows[index]);

  /* ★ 域块：**同一张灰圆角底图**（sub item 的图不按行区分）+ 逐行 setText/setTextColor */
  if (dic) { dic->setText(domGlyph(e.domain)); dic->setTextColor(domColor(e.domain)); }
  /* ★ 选中态用**文字**表达（没有勾字符可用，且图片不能按行区分）：名字变蓝 */
  if (nm) {
    nm->setText(e.name);
    nm->setTextColor(sel ? 0xFF0A84FFu : 0xFFF2F2F7u);
  }
  if (inf) {
    char b[200];
    snprintf(b, sizeof(b), "%s · %s", domCn(e.domain), e.state);
    inf->setText(b);
  }
  if (tag) {
    if (added) { tag->setText("已在面板"); tag->setTextColor(0xFF30D158u); }
    else if (sel) { tag->setText("已选"); tag->setTextColor(0xFF0A84FFu); }
    else { tag->setText("可添加"); tag->setTextColor(0xFF8E8E93u); }
  }
}

static void onListItemClick_ListHaPick(ZKListView *pListView, int index, int id) {
  (void)pListView; (void)id;
  std::vector<int> rows;
  buildPickRows(&rows);
  if (index < 0 || index >= (int)rows.size()) return;
  pickToggle(rows[index], sEnts[rows[index]].name);
}

/* 段控切换 */
void pickSetFilter(bool ctrlOnly) {
  sPickCtrlOnly = ctrlOnly;
  syncSegTexts();
  if (mListHaPickPtr) mListHaPickPtr->refreshListView();
  LOGD("haLogic: 段控 -> %s", ctrlOnly ? "可控" : "全部");
}

/* 进命名页：把勾选的实体按面板顺序排好 */
void pickGoName() {
  if (sPickSel.empty()) { hintSay("请先选择要添加的设备"); return; }
  sNameIds.clear();
  sNameTexts.clear();
  /* 实体表顺序 = 扫描顺序（本身已按"可控优先+名字"排过，见 ha-app.md §3） */
  for (size_t i = 0; i < sPickSel.size(); ++i) {
    sNameIds.push_back(std::string(sEnts[sPickSel[i]].id));
    sNameTexts.push_back(std::string());
  }
  sNameStep = 0;
  hintSay("给设备起个名字（可跳过）");
  showPage(PG_NAME);
  LOGD("haLogic: 进命名页 %d 台", (int)sNameIds.size());
}

/* ==================================================================
 *                        命名页（向导 ②）
 * ================================================================== */

const pg::HaEntity *nameCurEnt() {
  if (sNameStep < 0 || sNameStep >= (int)sNameIds.size()) return 0;
  return findEnt(sNameIds[sNameStep].c_str());
}

/* 命名页的反馈要写在**本页可见**的地方！
 * ⚠️ hintSay() 写的是首页的提示行（mTextHaHintPtr 在 WinHaHome 上）——
 *    在命名页上**根本看不见**（跨窗口）⇒ 拿它做反馈就是"静默失败"。
 *    所以这里用本页的 TextHaNameStep（标题下那行，y=104，在键盘之上）。 */
void nameStepSay(const char *txt, unsigned color) {
  if (mTextHaNameStepPtr) {
    mTextHaNameStepPtr->setText(txt);
    mTextHaNameStepPtr->setTextColor(color);
  }
}

const char *kNameStepDefault = "第 2 步 / 共 3 步 · 打完字先按键盘「完成」上屏，再点右上「完成」";

/* 程序化写输入框（不触发"用户上屏"处理） */
void nameSetBox(const std::string &t) {
  if (!mEditHaNamePtr) return;
  sNameSilentSet = true;
  mEditHaNamePtr->setText(t);
  sNameSilentSet = false;
}

void syncNameChips() {
  const pg::HaEntity *e = nameCurEnt();
  const char **chips = chipsFor(e ? e->domain : pg::HD_OTHER);
  ZKButton *btns[6] = {mChipHaName0Ptr, mChipHaName1Ptr, mChipHaName2Ptr,
                       mChipHaName3Ptr, mChipHaName4Ptr, mChipHaName5Ptr};
  for (int i = 0; i < 6; ++i) if (btns[i]) btns[i]->setText(chips[i]);
}

void syncNameHeader() {
  char b[160];
  const pg::HaEntity *e = nameCurEnt();
  const char *haName = e ? e->name : (sNameStep < (int)sNameIds.size() ? sNameIds[sNameStep].c_str() : "—");
  snprintf(b, sizeof(b), "%d / %d · %s", sNameStep + 1, (int)sNameIds.size(), haName);
  if (mTextHaNameCurPtr) mTextHaNameCurPtr->setText(b);
  nameStepSay(kNameStepDefault, 0xFF9A9AA0u);
  /* 输入框显示当前这台已输入的名字（空 = 用 HA 原名） */
  const char *cur = (sNameStep < (int)sNameTexts.size()) ? sNameTexts[sNameStep].c_str() : "";
  nameSetBox(cur);
  syncNameChips();
}

/* 提交当前这台（把输入框文本存进 sNameTexts），返回是否还有下一台 */
bool nameCommitCurrent() {
  if (sNameStep < 0 || sNameStep >= (int)sNameIds.size()) return false;
  std::string t;
  if (mEditHaNamePtr) t = mEditHaNamePtr->getText();
  /* 去掉首尾空格（输入法可能带进来） */
  size_t a = t.find_first_not_of(" \t\r\n");
  size_t b = t.find_last_not_of(" \t\r\n");
  t = (a == std::string::npos) ? std::string() : t.substr(a, b - a + 1);
  sNameTexts[sNameStep] = t;
  return (sNameStep + 1) < (int)sNameIds.size();
}

/* 把 sNameIds/sNameTexts 一次性写进"我的设备"（原子写盘一次） */
void nameApplyAll() {
  int ok = 0;
  for (size_t i = 0; i < sNameIds.size(); ++i) {
    const char *alias = sNameTexts[i].empty() ? "" : sNameTexts[i].c_str();
    if (myList().add(sNameIds[i].c_str(), alias)) ++ok;
  }
  cardsDirty();
  char b[120];
  snprintf(b, sizeof(b), "已添加 %d 台设备", ok);
  hintSay(b);
  LOGD("haLogic: 向导完成，写入 %d/%d 台", ok, (int)sNameIds.size());
  if (mTextHaDoneTiPtr) {
    char s[96];
    snprintf(s, sizeof(s), "已添加 %d 台设备", ok);
    mTextHaDoneTiPtr->setText(s);
  }
  return;
}

void nameNextOrDone() {
  bool more = nameCommitCurrent();
  if (!more) {
    nameApplyAll();
    showPage(PG_DONE);
    return;
  }
  ++sNameStep;
  syncNameHeader();
  showPage(PG_NAME);        // 保持命名页（输入法会自己收起/再拉起）
  LOGD("haLogic: 命名 -> 第 %d 台", sNameStep + 1);
}

/* 一键添加：把当前快照里"可控且未添加"的全部加入 */
void oneClickAdd() {
  int ok = 0;
  for (size_t i = 0; i < sEnts.size(); ++i) {
    if (!pg::haDomainControllable(sEnts[i].domain)) continue;
    if (myList().indexOf(sEnts[i].id) >= 0) continue;
    if (myList().add(sEnts[i].id, "")) ++ok;
  }
  cardsDirty();
  char b[120];
  if (ok > 0) snprintf(b, sizeof(b), "已添加 %d 台可控设备", ok);
  else snprintf(b, sizeof(b), "没有可添加的可控设备（或到上限 %d 台）", pg::kHaListMax);
  hintSay(b);
  LOGD("haLogic: 一键添加 -> %d 台", ok);
  if (mTextHaDoneTiPtr) {
    char s[96];
    snprintf(s, sizeof(s), "已添加 %d 台设备", ok);
    mTextHaDoneTiPtr->setText(s);
  }
  showPage(PG_DONE);
}

/* ==================================================================
 *                        窗口与状态行
 * ================================================================== */

/* ★ 统一切窗口：**七收一放**。
 *   源稿里写在最后的 window = 最上层；只显示目标窗口而不管别的，
 *   会出现"点了没反应"（其实是被上层 window 盖着）—— 本工程的老坑。 */
void showPage(Page p) {
  sPage = p;
  ZKWindow *wins[7] = {mWinHaHomePtr, mWinHaPickPtr, mWinHaNamePtr,
                       mWinHaDonePtr, mWinHaMenuPtr, mWinHaConfirmPtr, mWinHaSetPtr};
  /* ★★ 弹层（WinHaMenu / WinHaConfirm）是 **modal**：它们是"画在当前窗口**之上**"的，
   *   父窗口保持可见 —— 这正是"弹框时背景的列表还在那儿显示"的实现（用户 2026-09-24 要求）。
   *   ⚠️ 所以**不能**像普通页那样把别的窗口全 hideWnd() 掉，否则遮罩后面什么都没有，
   *      半透明遮罩就白做了（改前就是这个症状：弹层后面一片纯黑）。
   *   ⚠️ 顺序：先把背景页放出来，再放弹层 —— 这样弹层在窗口表里更靠上。 */
  const bool popup = (p == PG_MENU || p == PG_CONFIRM);
  if (!popup) sBasePage = p;
  ZKWindow *base = ((int)sBasePage >= 0 && (int)sBasePage < 7) ? wins[(int)sBasePage] : 0;
  if (popup && base && !base->isVisible()) base->showWnd();
  for (int i = 0; i < 7; ++i) {
    if (!wins[i]) continue;
    if (i == (int)p) { wins[i]->showWnd(); continue; }
    if (popup && wins[i] == base) continue;      // 背景页留着当"底"
    wins[i]->hideWnd();
  }
  if (p == PG_PICK) {
    if (mListHaPickPtr) mListHaPickPtr->refreshListView();
    syncSegTexts();
    ha().reqStates();
  }
  if (p == PG_NAME) syncNameHeader();
  /* ★ 设置页的几行信息原来只在"按设置按钮"时填 ⇒ 走别的路径（QA/返回/被别处跳转）
   *   进来是**空的一页**。改成进页即填（幂等，重进也无害）。 */
  if (p == PG_SET) syncSetPage();
  const char *nm = p == PG_HOME ? "我的设备" : (p == PG_PICK ? "选择设备" :
                   (p == PG_NAME ? "命名" : (p == PG_DONE ? "完成" :
                   (p == PG_MENU ? "卡片菜单" : (p == PG_CONFIRM ? "移除确认" : "设置")))));
  LOGD("haLogic: 切窗口 -> %s", nm);
}

void haSyncStatus() {
  /* ---- 连接状态 ---- */
  const char *txt = ha().phaseText();
  const bool online = (ha().phase() == pg::Ha::ONLINE);
  char line[160];
  long ok = ha().lastOkMs();
  if (online && ok > 0) {
    long ago = (long)(ha().nowMs() - ok);
    snprintf(line, sizeof(line), "• 已连接 · %s前同步",
             ago < 3000 ? "刚刚" : (ago < 60000 ? "1 分" : "很久"));
  } else {
    snprintf(line, sizeof(line), "• %s", txt);
  }
  if (strcmp(sPhase, line) != 0) {
    snprintf(sPhase, sizeof(sPhase), "%s", line);
    if (mTextHaStatePtr) mTextHaStatePtr->setText(sPhase);
  }
  unsigned want = 0xFFFF9F0Au;
  if (online) want = 0xFF30D158u;
  else if (ha().phase() == pg::Ha::AUTH_FAIL) want = 0xFFFF453Au;
  else if (ha().phase() == pg::Ha::CONNECTING) want = 0xFF64D2FFu;
  if (want != sPhaseColor) {
    if (mTextHaStatePtr) mTextHaStatePtr->setTextColor(want);
    sPhaseColor = want;
  }

  /* ---- 连接失败状态卡（L2 说上下文、L3 说影响） ---- */
  syncOfflineCard(!online);
  if (!online) {
    char b[200];
    if (ha().cfg().host[0]) {
      snprintf(b, sizeof(b), "%s:%d 无响应 · 数据停在 %s",
               ha().cfg().host, ha().cfg().port, ok > 0 ? "刚才" : "还没有过");
    } else {
      snprintf(b, sizeof(b), "没有找到 /data/ha.conf 配置");
    }
    if (mTextHaOffL2Ptr) mTextHaOffL2Ptr->setText(b);
  }

  /* ---- 提示行 ----
   * 优先级：**最近一次控制结果**（用户刚点完最需要看到它）> 其它一次性提示 > 常显建议。 */
  static char sCtlLast[200] = {0};
  const char *msg = ha().ctlMsg();
  if (msg && msg[0] && strcmp(msg, sCtlLast) != 0) {
    snprintf(sCtlLast, sizeof(sCtlLast), "%s", msg);
    hintSay(msg);
  }

  const char *wantHint = 0;
  if (sHint[0] && ha().nowMs() < sHintUntilMs) {
    wantHint = sHint;
  } else {
    sHint[0] = 0;
    sHintUntilMs = 0;
    wantHint = ha().phaseHint();
  }
  static char sHintShown[224] = {0};
  if (strcmp(sHintShown, wantHint) != 0) {
    snprintf(sHintShown, sizeof(sHintShown), "%s", wantHint);
    if (mTextHaHintPtr) mTextHaHintPtr->setText(sHintShown);
  }

  /* ★★ 离线态：提示行**被连接状态卡整个盖住**（卡是 12,104 456x96 的不透明块）
   *   ⇒ 任何一次性提示（比如点离线设备时的"先别点（发了也不会变）"）都会
   *     **看不见 = 静默失败**（本项目红线）。
   *   办法：离线时把"临时提示"写进**卡内 L3 行**；提示过期后恢复设计文案。 */
  if (!online) {
    const bool temp = (sHint[0] && ha().nowMs() < sHintUntilMs);
    const char *l3 = temp ? sHint : kOffL3Text;
    static char sL3Shown[224] = {0};
    if (strcmp(sL3Shown, l3) != 0) {
      snprintf(sL3Shown, sizeof(sL3Shown), "%s", l3);
      if (mTextHaOffL3Ptr) mTextHaOffL3Ptr->setText(sL3Shown);
    }
  }
}

/* ==================================================================
 *                        设置页
 * ================================================================== */
void syncSetPage() {
  char b[200];
  if (mTextHaSetV1Ptr) {
    if (ha().cfg().host[0]) {
      snprintf(b, sizeof(b), "%s://%s:%d · 实体 %d 个",
               ha().cfg().tls ? "https" : "http", ha().cfg().host, ha().cfg().port,
               ha().entityCount());
    } else {
      snprintf(b, sizeof(b), "没有配置 /data/ha.conf");
    }
    mTextHaSetV1Ptr->setText(b);
  }
  if (mTextHaSetV2Ptr) {
    long ok = ha().lastOkMs();
    if (ok > 0) {
      long ago = (long)(ha().nowMs() - ok);
      snprintf(b, sizeof(b), "每 %d 秒自动同步 · 最近一次 %ld 秒前", ha().cfg().pollSec, ago / 1000);
    } else {
      snprintf(b, sizeof(b), "每 %d 秒自动同步 · 还没成功过", ha().cfg().pollSec);
    }
    mTextHaSetV2Ptr->setText(b);
  }
  if (mTextHaSetV4Ptr) {
    int ctrl = 0, ro = 0;
    for (size_t i = 0; i < sEnts.size(); ++i) {
      if (pg::haDomainControllable(sEnts[i].domain)) ++ctrl; else ++ro;
    }
    snprintf(b, sizeof(b), "我的设备 %d 台 · 可控 %d 台 · 只读 %d 台",
             (int)sMyDevs.size(), ctrl, ro);
    mTextHaSetV4Ptr->setText(b);
  }
}

/* ==================================================================
 *                        按钮 / 列表回调
 * ================================================================== */

/* 卡片三路回调（**逐个手写，不用宏**）：
 * ⚠️ `fun install` 会扫源码**文本**找 `onButtonClick_<caption>`；宏展开的函数它看不见
 *    ⇒ 判定"缺失" ⇒ 在文件末尾补一份桩 ⇒ **重定义、编译失败**（已踩一次）。
 *    所以这里必须让函数名在源码里字面出现。 */
static bool onButtonClick_Card0(ZKButton *p) { (void)p; cardTap(0); return true; }
static bool onButtonClick_Dic0(ZKButton *p) { (void)p; cardTap(0); return true; }
static bool onButtonClick_Mn0(ZKButton *p) { (void)p; menuTap(0); return true; }
static bool onButtonClick_Card1(ZKButton *p) { (void)p; cardTap(1); return true; }
static bool onButtonClick_Dic1(ZKButton *p) { (void)p; cardTap(1); return true; }
static bool onButtonClick_Mn1(ZKButton *p) { (void)p; menuTap(1); return true; }
static bool onButtonClick_Card2(ZKButton *p) { (void)p; cardTap(2); return true; }
static bool onButtonClick_Dic2(ZKButton *p) { (void)p; cardTap(2); return true; }
static bool onButtonClick_Mn2(ZKButton *p) { (void)p; menuTap(2); return true; }
static bool onButtonClick_Card3(ZKButton *p) { (void)p; cardTap(3); return true; }
static bool onButtonClick_Dic3(ZKButton *p) { (void)p; cardTap(3); return true; }
static bool onButtonClick_Mn3(ZKButton *p) { (void)p; menuTap(3); return true; }
static bool onButtonClick_Card4(ZKButton *p) { (void)p; cardTap(4); return true; }
static bool onButtonClick_Dic4(ZKButton *p) { (void)p; cardTap(4); return true; }
static bool onButtonClick_Mn4(ZKButton *p) { (void)p; menuTap(4); return true; }
static bool onButtonClick_Card5(ZKButton *p) { (void)p; cardTap(5); return true; }
static bool onButtonClick_Dic5(ZKButton *p) { (void)p; cardTap(5); return true; }
static bool onButtonClick_Mn5(ZKButton *p) { (void)p; menuTap(5); return true; }
static bool onButtonClick_Card6(ZKButton *p) { (void)p; cardTap(6); return true; }
static bool onButtonClick_Dic6(ZKButton *p) { (void)p; cardTap(6); return true; }
static bool onButtonClick_Mn6(ZKButton *p) { (void)p; menuTap(6); return true; }
static bool onButtonClick_Card7(ZKButton *p) { (void)p; cardTap(7); return true; }
static bool onButtonClick_Dic7(ZKButton *p) { (void)p; cardTap(7); return true; }
static bool onButtonClick_Mn7(ZKButton *p) { (void)p; menuTap(7); return true; }

static bool onButtonClick_BtnHaAdd(ZKButton *p) { (void)p; sPickSel.clear(); showPage(PG_PICK); return true; }
static bool onButtonClick_BtnHaSet(ZKButton *p) { (void)p; syncSetPage(); showPage(PG_SET); return true; }
static bool onButtonClick_BtnHaOffSet(ZKButton *p) { (void)p; syncSetPage(); showPage(PG_SET); return true; }
static bool onButtonClick_BtnHaEmpAdd(ZKButton *p) { (void)p; sPickSel.clear(); showPage(PG_PICK); return true; }
static bool onButtonClick_BtnHaEmpOne(ZKButton *p) { (void)p; oneClickAdd(); return true; }

/* ---- 选择页 ---- */
static bool onButtonClick_BtnHaPickX(ZKButton *p) { (void)p; sPickSel.clear(); showPage(PG_HOME); return true; }
static bool onButtonClick_SegHaCtrl(ZKButton *p) { (void)p; pickSetFilter(true); return true; }
static bool onButtonClick_SegHaAll(ZKButton *p) { (void)p; pickSetFilter(false); return true; }
static bool onButtonClick_BtnHaPickNext(ZKButton *p) { (void)p; pickGoName(); return true; }

/* ---- 命名页 ---- */
static bool onButtonClick_BtnHaSkip(ZKButton *p) { (void)p; nameApplyAll(); showPage(PG_DONE); return true; }
static bool onButtonClick_BtnHaClear(ZKButton *p) {
  (void)p;
  nameSetBox("");
  nameStepSay("已清空 · 点右上「完成」就用 HA 原名", 0xFF64D2FFu);
  return true;
}
/* ★ 命名页的页级「完成」：记下这一台 → 下一台（最后一台就进完成页）。
 *   与键盘的「完成」分工：键盘那个只把字**上屏**（框架机制），这个才推进流程。
 *   ⚠️ 输入框为空 = 用 HA 原名（提示里写明，不做静默处理）。 */
static bool onButtonClick_BtnHaNameOk(ZKButton *p) {
  (void)p;
  if (sNameStep >= 0 && sNameStep < (int)sNameIds.size()) {
    std::string t = mEditHaNamePtr ? mEditHaNamePtr->getText() : std::string();
    if (t.empty()) hintSay("名字留空 · 这台保留 HA 原名");
  }
  nameNextOrDone();
  return true;
}
static bool onButtonClick_BtnHaPrevDev(ZKButton *p) {
  (void)p;
  if (sNameStep <= 0) { hintSay("已经是第一台"); return true; }
  nameCommitCurrent();
  --sNameStep;
  syncNameHeader();
  return true;
}
/* 推荐词片：点一下**填入输入框**（不再是"点一下即改名"）。 */
void nameChip(int n) {
  const pg::HaEntity *e = nameCurEnt();
  const char **ch = chipsFor(e ? e->domain : pg::HD_OTHER);
  if (n >= 0 && n < 6) {
    nameSetBox(ch[n]);
    char b[96];
    snprintf(b, sizeof(b), "已填入「%s」· 可以直接改，或点右上「完成」", ch[n]);
    nameStepSay(b, 0xFF64D2FFu);
  }
}
static bool onButtonClick_ChipHaName0(ZKButton *p) { (void)p; nameChip(0); return true; }
static bool onButtonClick_ChipHaName1(ZKButton *p) { (void)p; nameChip(1); return true; }
static bool onButtonClick_ChipHaName2(ZKButton *p) { (void)p; nameChip(2); return true; }
static bool onButtonClick_ChipHaName3(ZKButton *p) { (void)p; nameChip(3); return true; }
static bool onButtonClick_ChipHaName4(ZKButton *p) { (void)p; nameChip(4); return true; }
static bool onButtonClick_ChipHaName5(ZKButton *p) { (void)p; nameChip(5); return true; }

/* ★ 段控的"选中态"按钮也要挂回调：两个按钮叠一格，**上层那个才吃得到点**，
 *   漏了就会"点选中的那半边没反应"。 */
static bool onButtonClick_SegHaCtrlOn(ZKButton *p) { (void)p; pickSetFilter(true); return true; }
static bool onButtonClick_SegHaAllOn(ZKButton *p) { (void)p; pickSetFilter(false); return true; }

/* ---- 完成页 ---- */
static bool onButtonClick_BtnHaDoneGo(ZKButton *p) { (void)p; sNameIds.clear(); sNameTexts.clear(); showPage(PG_HOME); return true; }
static bool onButtonClick_BtnHaDoneMore(ZKButton *p) { (void)p; sPickSel.clear(); showPage(PG_PICK); return true; }

/* ---- 卡片菜单 ---- */
static bool onButtonClick_BtnHaMenuRename(ZKButton *p) {
  (void)p;
  if (sMenuIdx < 0) return true;
  /* 进命名页改这一台的名字（单台模式） */
  sNameIds.clear();
  sNameTexts.clear();
  sNameIds.push_back(std::string(sMyDevs[sMenuIdx].id));
  sNameTexts.push_back(std::string(sMyDevs[sMenuIdx].alias));
  sNameStep = 0;
  showPage(PG_NAME);
  return true;
}
static bool onButtonClick_BtnHaMenuUp(ZKButton *p) {
  (void)p;
  if (sMenuIdx > 0 && myList().move(sMenuIdx, sMenuIdx - 1)) {
    hintSay("已移到前面"); cardsDirty();
  } else {
    hintSay("已经在最前面");
  }
  sMenuIdx = -1;
  showPage(PG_HOME);
  return true;
}
static bool onButtonClick_BtnHaMenuDown(ZKButton *p) {
  (void)p;
  if (sMenuIdx >= 0 && sMenuIdx + 1 < (int)sMyDevs.size() && myList().move(sMenuIdx, sMenuIdx + 1)) {
    hintSay("已移到后面"); cardsDirty();
  } else {
    hintSay("已经在最后面");
  }
  sMenuIdx = -1;
  showPage(PG_HOME);
  return true;
}
static bool onButtonClick_BtnHaMenuRemove(ZKButton *p) {
  (void)p;
  if (sMenuIdx >= 0 && sMenuIdx < (int)sMyDevs.size()) {
    const pg::HaDev &d = sMyDevs[sMenuIdx];
    const pg::HaEntity *e = findEnt(d.id);
    char disp[96];
    pg::HaList::displayName(d, e ? e->name : d.id, disp, sizeof(disp));
    if (mTextHaCfTPtr) {
      char b[160];
      snprintf(b, sizeof(b), "移除「%s」？", disp);
      mTextHaCfTPtr->setText(b);
    }
  }
  showPage(PG_CONFIRM);
  return true;
}
/* 点遮罩空白处 = 关掉弹层（遮罩在菜单卡片**下面**一层，点菜单内部不会落到这里）。 */
static bool onButtonClick_ImgHaMask(ZKButton *p) { (void)p; sMenuIdx = -1; showPage(PG_HOME); return true; }
static bool onButtonClick_ImgHaMask2(ZKButton *p) { (void)p; showPage(PG_MENU); return true; }
static bool onButtonClick_BtnHaMenuCancel(ZKButton *p) { (void)p; sMenuIdx = -1; showPage(PG_HOME); return true; }

/* ---- 移除确认 ---- */
static bool onButtonClick_BtnHaCfNo(ZKButton *p) { (void)p; showPage(PG_MENU); return true; }
static bool onButtonClick_BtnHaCfYes(ZKButton *p) { (void)p; doRemoveCard(); return true; }

/* ---- 设置页 ---- */
static bool onButtonClick_BtnHaSetBack(ZKButton *p) { (void)p; showPage(PG_HOME); return true; }
static bool onButtonClick_BtnHaSetReload(ZKButton *p) {
  (void)p;
  pg::HaCfg c;
  if (!c.load()) {
    hintSay("没有配置文件：请把 /data/ha.conf 放进去");
    if (mTextHaSetMsgPtr) mTextHaSetMsgPtr->setText("找不到 /data/ha.conf 或 /tmp/ha.conf");
    return true;
  }
  bool ok = ha().applyCfg(c);
  char b[160];
  snprintf(b, sizeof(b), "已重新读取配置（%s）", ok ? "已应用" : "应用失败");
  if (mTextHaSetMsgPtr) mTextHaSetMsgPtr->setText(b);
  hintSay(b);
  LOGD("haLogic: 重读配置 启动=%d", ok ? 1 : 0);
  return true;
}
static bool onButtonClick_RowHaSet3(ZKButton *p) { (void)p; syncSetPage(); return true; }
static bool onButtonClick_RowHaSet1(ZKButton *p) { (void)p; syncSetPage(); return true; }
static bool onButtonClick_RowHaSet4(ZKButton *p) { (void)p; syncSetPage(); return true; }

/* ---- 退出 ---- */
void haQuit() { EASYUICONTEXT->goBack(); }
void haQuitAndCleanup() {
  EASYUICONTEXT->unregisterKeyListener(&sHaKeys);
  EASYUICONTEXT->setScreensaverEnable(EASYUICONTEXT->getScreensaverTimeOut() > 0);
  ha().stop();
  EASYUICONTEXT->goBack();
}

/* ==================================================================
 *                          QA（免触摸验收）
 * ================================================================== */

void haListDump() {
  FILE *f = fopen("/tmp/pg_ha_list.txt", "w");
  if (!f) return;
  fprintf(f, "Build: %s  打点: tick=%d homeSync=%d entSync=%d\n",
          kBuildTag, sTickCount, sHomeSyncCount, sEntSyncCount);
  fprintf(f, "Win: page=%d 我的设备=%d HA实体=%d 已选=%d 命名台数=%d 菜单idx=%d 空态=%d 状态卡=%d\n",
          (int)sPage, (int)sMyDevs.size(), (int)sEnts.size(), (int)sPickSel.size(),
          (int)sNameIds.size(), sMenuIdx, sEmptyShown ? 1 : 0, sOfflineShown ? 1 : 0);
  fprintf(f, "Phase: %s  color=0x%06X  hint=%s\n", sPhase, sPhaseColor & 0xFFFFFFu, sHint);
  fprintf(f, "Reject: %s\n", sLastReject);
  fprintf(f, "Ctl: %s\n", ha().ctlMsg());
  for (int i = 0; i < kCardCount; ++i) {
    int idx = cardDevIdx(i);
    if (idx < 0) { fprintf(f, "Card%d: (空)\n", i); continue; }
    const pg::HaEntity *e = findEnt(sMyDevs[idx].id);
    fprintf(f, "Card%d: id=%s alias=%s dom=%s glyph=%s name=%s state=%s rendered=[nm=%s st=%s sb=%s]\n",
            i, sMyDevs[idx].id, sMyDevs[idx].alias[0] ? sMyDevs[idx].alias : "-",
            e ? pg::haDomainName(e->domain) : "?", sCardDic[i],
            e ? e->name : "-", e ? e->state : "-", sCardNm[i], sCardSt[i], sCardSb[i]);
  }
  fclose(f);
}

void haDumpToFile(int maxEnt) {
  std::string out;
  ha().dumpTo(&out, maxEnt);
  FILE *f = fopen("/tmp/pg_ha_dump.txt", "w");
  if (!f) return;
  fputs(out.c_str(), f);
  fclose(f);
}

/* 命令执行（一条一行；`#` 之后是注释，由 haPollCmd 先剥） */
void haCmdRun(const char *cmd) {
  if (!cmd || !cmd[0]) return;
  char c[256];
  snprintf(c, sizeof(c), "%s", cmd);

  if (!strncmp(c, "ping", 4)) { ha().reqPing(); return; }
  if (!strncmp(c, "states", 6)) { ha().reqStates(); return; }
  if (!strncmp(c, "reload", 6)) {
    pg::HaCfg cfg;
    if (cfg.load()) ha().applyCfg(cfg);
    return;
  }
  if (!strncmp(c, "cfg", 3)) { haDumpToFile(0); return; }
  if (!strncmp(c, "listdump", 8)) { haListDump(); return; }
  if (!strncmp(c, "dump", 4)) {
    int n = atoi(c + 4);
    haDumpToFile(n > 0 ? n : 40);
    return;
  }
  if (!strncmp(c, "fakeoff", 7)) { ha().fakeOffline(atoi(c + 7) != 0); return; }
  if (!strncmp(c, "quit", 4)) { haQuitAndCleanup(); return; }

  if (!strncmp(c, "win ", 4)) {
    const char *w = c + 4;
    if (!strcmp(w, "home")) showPage(PG_HOME);
    else if (!strcmp(w, "pick")) showPage(PG_PICK);
    else if (!strcmp(w, "name")) showPage(PG_NAME);
    else if (!strcmp(w, "done")) showPage(PG_DONE);
    else if (!strcmp(w, "menu")) showPage(PG_MENU);
    else if (!strcmp(w, "confirm")) showPage(PG_CONFIRM);
    else if (!strcmp(w, "set")) showPage(PG_SET);
    return;
  }
  if (!strncmp(c, "card ", 5)) { cardTap(atoi(c + 5)); return; }
  if (!strncmp(c, "menu ", 5)) { menuTap(atoi(c + 5)); return; }
  if (!strncmp(c, "pick ", 5)) {
    int i = atoi(c + 5);
    pickToggle(i, i >= 0 && i < (int)sEnts.size() ? sEnts[i].name : "");
    return;
  }
  if (!strncmp(c, "seg ", 4)) { pickSetFilter(strcmp(c + 4, "all") != 0); return; }
  if (!strncmp(c, "next", 4)) { pickGoName(); return; }
  if (!strncmp(c, "chip ", 5)) {
    int n = atoi(c + 5);
    const pg::HaEntity *e = nameCurEnt();
    const char **ch = chipsFor(e ? e->domain : pg::HD_OTHER);
    if (n >= 0 && n < 6 && mEditHaNamePtr) mEditHaNamePtr->setText(ch[n]);
    return;
  }
  if (!strncmp(c, "clearname", 9)) { if (mEditHaNamePtr) mEditHaNamePtr->setText(""); return; }
  if (!strncmp(c, "typedone", 8)) { nameNextOrDone(); return; }
  if (!strncmp(c, "nameok", 6)) { nameNextOrDone(); return; }   // 同 typedone（页级「完成」按钮）
  if (!strncmp(c, "skip", 4)) { nameApplyAll(); showPage(PG_DONE); return; }
  if (!strncmp(c, "prevdev", 7)) { if (sNameStep > 0) { nameCommitCurrent(); --sNameStep; syncNameHeader(); } return; }
  if (!strncmp(c, "cfyes", 5)) { doRemoveCard(); return; }
  if (!strncmp(c, "cfno", 4)) { showPage(PG_MENU); return; }
  if (!strncmp(c, "oneclick", 8)) { oneClickAdd(); return; }

  if (!strncmp(c, "toggle ", 7)) { ha().reqToggle(c + 7); return; }
  if (!strncmp(c, "add ", 4)) {
    char *sp = strchr(c + 4, ' ');
    if (sp) { *sp = 0; myList().add(c + 4, sp + 1); } else { myList().add(c + 4, ""); }
    cardsDirty();
    return;
  }
  if (!strncmp(c, "rm ", 3)) { myList().remove(c + 3); cardsDirty(); return; }
  if (!strncmp(c, "rename ", 7)) {
    char *sp = strchr(c + 7, ' ');
    if (sp) { *sp = 0; myList().rename(c + 7, sp + 1); } else { myList().rename(c + 7, ""); }
    cardsDirty();
    return;
  }
  if (!strncmp(c, "savelist", 8)) { myList().save(); return; }
  if (!strncmp(c, "listload", 8)) { myList().load(); cardsDirty(); return; }
  LOGW("haLogic: 未知 QA 命令: %s", c);
}

/* 轮询 QA 命令文件：**内容变化才执行**（整份比对），`#` 之后是注释。 */
void haPollCmd() {
  static char last[4096] = {0};
  FILE *f = fopen("/tmp/pg_hacmd", "r");
  if (!f) return;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);
  if (strcmp(buf, last) == 0) return;
  snprintf(last, sizeof(last), "%s", buf);
  /* ⚠️ 必须先剥 `#` 注释：不剥的话 `listdump #10` 不识别，
   *    更坏的是 `add <id> #10` 会把 ` #10` 存进设备列表（已踩）。 */
  char *save = 0;
  for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    while (*line == ' ' || *line == '\t') ++line;
    size_t L = strlen(line);
    while (L > 0 && (line[L - 1] == ' ' || line[L - 1] == '\r' || line[L - 1] == '\t')) line[--L] = 0;
    if (L) haCmdRun(line);
  }
}
static bool onButtonClick_CardHaOffline(ZKButton* pButton) {
  LOGD_TRACE("CardHaOffline click");
  return false;
}

static bool onButtonClick_ImgHaEmpty(ZKButton* pButton) {
  LOGD_TRACE("ImgHaEmpty click");
  return false;
}

/* 键盘自己的「完成」键 → doneIMETextUpdate(整串) → 框架写回 EditText → 到这里。
 * ★ 2026-09-24 之前这里什么都不做 ⇒ 用户打完字、按了键盘的「完成」，输入框里有字了，
 *   但**页面上没有任何"下一步"可点**（唯一可点的"跳过"是"全部不改名"）
 *   —— 用户报的"命名与排序中命名后没有确认按键"就是这条。
 * 现在做两件事：
 *   ① 把提交的串存进 sNameTexts（这样随后点"跳过 / 完成"都能带上它）；
 *   ② 给一句提示，指着右上角的「完成」。
 * ⚠️ 这里**不自动跳下一台** —— 那会和页级「完成」撞车（用户按一次键盘完成 + 一次页面完成
 *    就会连跳两台）。自动推进交给页级按钮，行为可预期。 */
static void onEditTextChanged_EditHaName(const std::string &text) {
  if (sNameSilentSet) {
    LOGD("haLogic: 命名框程序化赋值（忽略，不是用户上屏）");
    return;
  }
  if (sNameStep >= 0 && sNameStep < (int)sNameTexts.size()) {
    sNameTexts[sNameStep] = text;
  }
  char b[140];
  if (text.empty()) {
    snprintf(b, sizeof(b), "已清空 · 再点右上「完成」就用 HA 原名");
    nameStepSay(b, 0xFFFF9F0Au);
  } else {
    snprintf(b, sizeof(b), "「%s」已上屏 · 点右上「完成」记下这一台", text.c_str());
    nameStepSay(b, 0xFF64D2FFu);
  }
  hintSay(text.empty() ? "命名框已清空" : "名字已上屏");
  LOGD("haLogic: 命名输入框用户上屏 %d 字节（第 %d 台）", (int)text.size(), sNameStep + 1);
}

static bool onButtonClick_ImgHaDone(ZKButton* pButton) {
  LOGD_TRACE("ImgHaDone click");
  return false;
}

static bool onButtonClick_CardHaMenu(ZKButton* pButton) {
  LOGD_TRACE("CardHaMenu click");
  return false;
}

static bool onButtonClick_CardHaConfirm(ZKButton* pButton) {
  LOGD_TRACE("CardHaConfirm click");
  return false;
}


