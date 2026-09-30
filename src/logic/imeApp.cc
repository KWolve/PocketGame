/*
 * imeApp.cc - 自定义输入法（自定义 IME 应用）
 *
 * 为什么要做：本板系统内置 IME 在 480x800 竖屏下**超出屏幕边界**（用户实测）。
 * 官方正解是做一个 `APP_TYPE_SYS_IME` 的 SysApp —— 系统在**任何** ZKEditText 聚焦时
 * 自动拉起它，业务页不需要手动 showIME()。
 *
 * ⚠️ 铁律：非游戏功能**禁止自定义绘图，一律用原生控件**。所以键盘是 ui/ime.html 里的
 *    按钮（ZKButton），本文件只做「按键 -> 字符」的查表与 IME 协议对接。
 *
 * 流程（框架 IME 协议，见 ime/IMEContext.h）：
 *   EditText 聚焦 → EASYUICONTEXT->showIME(info, listener) → 框架建/显示本 App
 *   → onInitIME(info) 拿到（isPassword / passwordTextType / 现有文本）
 *   → 用户在键盘上打字（本地缓冲，只刷回显条）
 *   → 「完成」doneIMETextUpdate(整串) / 「收起」cancelIMETextUpdate()
 *
 * ⚠️ 与 fun 生成器的关系：fun **不认 ime 这个 sysapp 页名**（它只认 screensaver /
 *    statusbar / navibar），所以它会为 ui/ime.html 另外生成一个普通 activity
 *    （`ui_ime.cpp` 里的 `REGISTER_ACTIVITY(imeActivity)`）。那个是**惰性的**：没有任何
 *    代码 openActivity("ime")，它的控件指针也不会被初始化。本文件才是真正生效的 IME。
 *    我们只从生成的 `ui_ime.h` 取 `ID_IME_*` 控件 ID，保证 ID 只有一个来源。
 *
 * ======================= 2026-09-23：加中文拼音（全拼）=======================
 * 需求（用户）：「修改名字可以让用户自己输入。FlyThings 支持中文输入。」
 *
 * 设计（为什么这么选，别处改前先读）：
 *   ① **模式显式切换**，不做"自动猜中文"。框架只给两种文本类型
 *      （E_IME_TEXT_TYPE_ALL / NUMBER，见 IMEContext.h），**没有"这是中文输入框"这种提示**
 *      ⇒ 猜不了。所以给一个「中文 / 英文」切换键，并且：
 *        · 默认**英文**（不改动既有输入体验：WiFi SSID/密码、摄像头地址都还是老样子）；
 *        · **密码框强制英文**（永不带记忆的中文态进去，否则密码里混进汉字）；
 *        · 记住**上次选择**（`sLastChinese`）：连着改几个设备名时只需切一次。
 *   ② **全拼**，不做模糊音/简拼。词典见 tools/gen_ime_pinyin.py 生成的头文件；
 *      候选顺序是「人工常用字优先级 -> 工程文案里出现过的字 -> 码点」，
 *      这就是"打 shi 先出「室」"而不是字典序第一位的那个字。
 *   ③ ★ **候选只收字库里真有字形的字**。本工程 font/*.ttf 是完全替换系统字体的子集、
 *      没有逐字回退 ⇒ 候选里放一个缺字形的字，用户选中后那个字**整个消失**。
 *      生成器已按 `font/pocketgame.ttf` 的 cmap 过滤（见那个脚本的头注释）。
 *   ④ **候选落点 = kCandPerPage(5) 个格 + 上页/下页**（见 ui/ime.html 的版面说明）。
 *      数字键 1..kCandPerPage 也能选候选（中文模式下、有缓冲时），这是手机全键盘的习惯。
 *
 * ================= 2026-09-24：候选"太少"的整改（读懂再改） =================
 * 用户反馈「候选词数量太少、缺少候选字」。逐项量过，三个真因：
 *   ① **字库只有 GB2312 一级 3755 字** ⇒ 二级字（鑫/淼/婷/妍/怡/璇/瑜/瑾…）
 *      一个都进不了候选表（人名 30 字里 19 个打不出）。修法在 `tools/gen_font.py`
 *      的 `common_chinese()`（一级 → 全集 6763）+ `EXTRA_HANZI`（GBK 独有的人名用字：
 *      玥/珺/喆/垚/犇/骉…）。字库 944KB → 1760KB。
 *   ② **生成器的上限太小**：`MAX_CHARS_PER_PY=60` 会把 `yi`(149 字) 截掉一半，
 *      `MAX_WORDS=1500` 砍掉 3525 条自动词。见 tools/gen_ime_pinyin.py。
 *   ③ **运行时的 `kMaxCand=60`** ⇒ 就算生成器放宽了，用户还是只看到 60 个候选。
 *      ★ 这两处必须成对改（生成器 ↔ 本文件），只改一边等于没改。
 * 另修：候选格 6 → 5 格、宽 76 → 92px，因为 4 字词（客厅窗帘 80px）会被裁。
 * 排序仍然有效：常用字优先级 → 工程文案字 → 码点，所以前 1~2 页就是常用字。
 * 内存：候选 170 个短串 ≈ 几 KB；字体多了 800KB 但它在 /res（flash，只读），
 *      不是常驻 DRAM 堆（字体文件本身不吃应用 RAM）。
 *
 * QA（免触摸验收，通道 /tmp/pg_imecmd，整份内容变化才执行，每行一条）：
 *   k <1..45>      按键（序号见 kKeyIds，与 ui/ime.html 的 BtnK01..BtnK45 一一对应）
 *   btn <1..45>    同上，但走**真实控件指针 -> onClick**（验 ID 映射表）
 *   shift / del / clear / done / hide / lbl
 *   mode           切中/英（toggle）；`mode cn` / `mode en` = **显式设置**（脚本用这个更稳）
 *   cand <1..5>    选中本页第 n 个候选（范围 = 候选格数 kCandPerPage）
 *   page up|down   候选翻页
 *   type <串>      把整串喂给 handleKey（省得写 20 行 QA 才输一个词）
 *                  ★ 行尾的 ` #注释` 会被剥掉（整份去重靠它，别喂给键盘）
 *   dump           把 文本/拼音/候选 落到 /tmp/pg_ime_dump.txt（可判据的证据）
 *
 * ★ 真机验收脚本：`python tools/ime_qa.py <拼音> [--shot]`（自动拉起输入法 + 读回候选）
 * ★ 候选表与字库的一致性：`python tools/check_ime_candidates.py`（改完生成器必跑）
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "app/IMEBaseApp.h"
#include "app/SysAppFactory.h"
#include "control/ZKButton.h"
#include "control/ZKTextView.h"
#include "utils/Log.h"

#include "imePinyinData.h"  // 生成物：拼音 -> 字/词（tools/gen_ime_pinyin.py）
#include "ui_ime.h"         // fun 生成：ID_IME_* / INIT_UI_EVENT_BINDINGS（控件 ID 唯一来源）

namespace {

/* 键序号 1..45 -> 字符；'\0' 表示功能键（31=大写切换, 44=退格, 45=完成）。
 * 顺序与 ui/ime.html 完全一致：第 0~3 行各 10 键，第 4 行是 5 个不等宽键。 */
const char kKeyChar[46] = {
    '\0',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',  // 01-10
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',  // 11-20
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-',  // 21-30
    '\0',                                              // 31 大写/小写切换
    'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', '@',       // 32-40
    '_', '#', ' ', '\0', '\0',                         // 41 _  42 #  43 空格  44 退格  45 完成
};

/* 键序号 -> 控件 ID（顺序同上；ID 来自生成的 ui_ime.h，不要手写数字） */
const int kKeyIds[46] = {
    0,
    ID_IME_BtnK01, ID_IME_BtnK02, ID_IME_BtnK03, ID_IME_BtnK04, ID_IME_BtnK05,
    ID_IME_BtnK06, ID_IME_BtnK07, ID_IME_BtnK08, ID_IME_BtnK09, ID_IME_BtnK10,  // 01-10
    ID_IME_BtnK11, ID_IME_BtnK12, ID_IME_BtnK13, ID_IME_BtnK14, ID_IME_BtnK15,
    ID_IME_BtnK16, ID_IME_BtnK17, ID_IME_BtnK18, ID_IME_BtnK19, ID_IME_BtnK20,  // 11-20
    ID_IME_BtnK21, ID_IME_BtnK22, ID_IME_BtnK23, ID_IME_BtnK24, ID_IME_BtnK25,
    ID_IME_BtnK26, ID_IME_BtnK27, ID_IME_BtnK28, ID_IME_BtnK29, ID_IME_BtnK30,  // 21-30
    ID_IME_BtnK31,                                                             // 31 大写
    ID_IME_BtnK32, ID_IME_BtnK33, ID_IME_BtnK34, ID_IME_BtnK35, ID_IME_BtnK36,
    ID_IME_BtnK37, ID_IME_BtnK38, ID_IME_BtnK39, ID_IME_BtnK40,                // 32-40
    ID_IME_BtnK41, ID_IME_BtnK42, ID_IME_BtnK43, ID_IME_BtnK44, ID_IME_BtnK45,  // 41-45
};

/* 候选格（与 ui/ime.html 的 BtnCand0..4 一一对应；格数 = kCandPerPage） */
const int kCandIds[5] = {
    ID_IME_BtnCand0, ID_IME_BtnCand1, ID_IME_BtnCand2,
    ID_IME_BtnCand3, ID_IME_BtnCand4,
};

const int K_SHIFT = 31;
const int K_DEL = 44;
const int K_DONE = 45;

const int MAX_CHARS = 63;      // WPA2 PSK 上限（同时是"交回给 EditText 的上限"）
const int MAX_PY = 24;         // 拼音缓冲上限（"ketingdeng" 才 11 个字母，24 够）
const int kCandPerPage = 5;    // = 候选格数量。★ 2026-09-24 由 6 改 5（ui/ime.html 的候选格
                               //   从 76px 加宽到 92px，好让「客厅窗帘」这种 4 字词不被裁）。
                               //   ⚠️ 三处必须同时改：ui/ime.html 的 BtnCand*、本常量、kCandIds。
const int kMaxCand = 170;      // 候选总数上限。★ 2026-09-24 由 60 提到 170：
                               //   生成器把单字上限放到了 MAX_CHARS_PER_PY=150
                               //   （GB2312 全集里 `yi` 就有 149 个同音字），
                               //   再加该拼音的词（MAX_WORDS_PER_PY=10）⇒ 上限必须 ≥160。
                               //   **这个值和生成器的 MAX_CHARS_PER_PY 必须成对改**：
                               //   生成器放宽、这里还卡 60，用户看到的仍然只有 60 个候选。
                               //   内存代价：170 个短串 ≈ 几 KB，可忽略（真正吃内存的是字形位图）。
const char kEmptyCell[] = "　";  // 候选空格位显示全角空格（占位、看得见底色）

// UTF-8 码点个数（掩码位数、长度上限都按"字符"算，不按字节）
int utf8Count(const std::string &s) {
  int n = 0;
  for (size_t i = 0; i < s.size(); ++i)
    if ((s[i] & 0xC0) != 0x80) ++n;
  return n;
}

// 去掉最后一个 UTF-8 字符
void utf8PopBack(std::string &s) {
  if (s.empty()) return;
  size_t i = s.size() - 1;
  while (i > 0 && (s[i] & 0xC0) == 0x80) --i;
  s.erase(i);
}

// 取 s 里第 i 个 UTF-8 字符（i 从 0 开始）；越界返回空串
std::string utf8At(const std::string &s, int idx) {
  int n = 0;
  for (size_t i = 0; i < s.size();) {
    size_t j = i + 1;
    while (j < s.size() && (s[j] & 0xC0) == 0x80) ++j;
    if (n == idx) return s.substr(i, j - i);
    i = j;
    ++n;
  }
  return std::string();
}

// 把「A B C」这样的空格分隔串拆成候选（词组表用这个格式）
void splitBySpace(const char *t, std::vector<std::string> *out) {
  if (!t) return;
  const char *p = t;
  while (*p) {
    const char *sp = strchr(p, ' ');
    size_t len = sp ? (size_t)(sp - p) : strlen(p);
    if (len) out->push_back(std::string(p, len));
    if (!sp) break;
    p = sp + 1;
  }
}

// 把紧凑串按 UTF-8 字符逐个作为候选（单字表用这个格式）
void splitByChar(const char *t, std::vector<std::string> *out) {
  if (!t) return;
  std::string s(t);
  for (size_t i = 0; i < s.size();) {
    size_t j = i + 1;
    while (j < s.size() && (s[j] & 0xC0) == 0x80) ++j;
    out->push_back(s.substr(i, j - i));
    i = j;
  }
}

/* 拼音表查找。两张表都由生成器保证按 py **升序** ⇒ 二分取 lower_bound，
 * 精确匹配看这一项，前缀匹配从这里往后扫到不匹配为止。 */
int pyLowerBound(const ImePyEnt *t, int n, const char *key) {
  int lo = 0, hi = n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(t[mid].py, key) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

const char *pyExact(const ImePyEnt *t, int n, const char *key) {
  int i = pyLowerBound(t, n, key);
  if (i < n && strcmp(t[i].py, key) == 0) return t[i].text;
  return NULL;
}

/* 按键底色/按下态：默认态是 json 里的底色，按下态必须显式给（否则按下无反馈）。
 * 见工程记忆：只 setBackgroundColor 看不到变化，要配 setBgStatusColor。 */
const uint32_t kPressedNormal = 0xFF3B4D63;  // 普通键按下
const uint32_t kPressedAction = 0xFF3E9E76;  // 完成键按下
const uint32_t kPressedFunc = 0xFF2A3A4C;    // 功能键按下

// 模式
enum { MODE_EN = 0, MODE_CN = 1 };

/* ★ 记住"上次用的是不是中文"（App 实例会随每次 show 重建，所以必须是静态的）。
 *   为什么值得记：改名场景通常连着改好几个设备，每次都重新切一次很烦。
 *   为什么密码框不继承：密码里混进汉字 = 连接必失败，且看不出原因。 */
bool sLastChinese = false;

}  // namespace

class PgImeApp : public IMEBaseApp {
 public:
  PgImeApp();

 protected:
  const char *getAppName() const { return "ime.ftu"; }

  void onCreate();
  void onClick(ZKBase *pBase);
  bool onTimer(int id);
  void onInitIME(SIMETextInfo *pInfo);

 private:
  void handleKey(int idx);       // idx = 1..45
  void typeChar(char c, bool isLetter);
  void commit();                 // 完成：交回整串
  void cancel();                 // 收起：不保存
  void syncInput();              // 刷新 回显条 + 功能行 + 候选格 + 模式键
  void syncShiftKeys();          // 刷新 26 个字母键的大小写
  void applyKeyStyles();         // 按下态配色（只在 onCreate 做一次）
  void setMode(int m);
  void buildCands();             // 按 py_ 重算候选
  void candCommit(int idxOnPage);
  void candPage(int delta);
  bool candUsable() const { return mode_ == MODE_CN && !py_.empty() && !cands_.empty(); }

  void pollQa();                 // 免触摸验收通道
  void qaSyncTag();              // 把 QA 文件当前内容设为基线（不执行）
  void qaDump();                 // 落 /tmp/pg_ime_dump.txt

  ZKButton *keyBtn_[46];         // 键序号 -> 按钮（1..45，0 不用）
  ZKButton *candBtn_[kCandPerPage];
  ZKButton *modeBtn_;
  ZKTextView *inputBarPtr_;      // 输入回显
  ZKTextView *hintPtr_;          // 功能行（拼音缓冲 / 页码 / 提示）

  std::string text_;             // 真实缓冲（未打码）
  std::string py_;               // 拼音缓冲（中文模式下的字母）
  std::vector<std::string> cands_;
  int mode_;
  int page_;
  std::string shown_;            // 回显条当前内容（比差值，防重绘）
  std::string hintShown_;
  std::string candShown_[kCandPerPage];
  std::string modeShown_;
  bool isPassword_;
  bool shift_;
  char qaTag_[64];               // QA 文件内容去重
};

PgImeApp::PgImeApp()
    : modeBtn_(NULL), inputBarPtr_(NULL), hintPtr_(NULL),
      mode_(MODE_EN), page_(0), isPassword_(false), shift_(false) {
  memset(keyBtn_, 0, sizeof(keyBtn_));
  memset(candBtn_, 0, sizeof(candBtn_));
  qaTag_[0] = '\0';
}

void PgImeApp::onCreate() {
  IMEBaseApp::onCreate();
  for (int i = 1; i <= 45; ++i) {
    keyBtn_[i] = (ZKButton *)findControlByID(kKeyIds[i]);
  }
  for (int i = 0; i < kCandPerPage; ++i) {
    candBtn_[i] = (ZKButton *)findControlByID(kCandIds[i]);
  }
  modeBtn_ = (ZKButton *)findControlByID(ID_IME_BtnKbMode);
  inputBarPtr_ = (ZKTextView *)findControlByID(ID_IME_TextKbInput);
  hintPtr_ = (ZKTextView *)findControlByID(ID_IME_TextKbHint);
  int missing = 0, cmiss = 0;
  for (int i = 1; i <= 45; ++i)
    if (!keyBtn_[i]) ++missing;
  for (int i = 0; i < kCandPerPage; ++i)
    if (!candBtn_[i]) ++cmiss;
  // ★ 控件取不到必须**报出来**（静默失败最坑：按了没反应，日志里什么都没有）
  LOGD("IME 键盘就绪：45 键缺 %d、%d 个候选格缺 %d；回显 %s 功能行 %s 模式键 %s",
       missing, kCandPerPage, cmiss, inputBarPtr_ ? "ok" : "NULL",
       hintPtr_ ? "ok" : "NULL", modeBtn_ ? "ok" : "NULL");
  applyKeyStyles();
  registerTimer(1, 250);   // QA 轮询（免触摸验收用；键盘显示期间才有意义）
}

void PgImeApp::applyKeyStyles() {
  for (int i = 1; i <= 45; ++i) {
    ZKButton *b = keyBtn_[i];
    if (!b) continue;
    uint32_t c = (i == K_DONE) ? kPressedAction
                 : (i == K_SHIFT || i == K_DEL) ? kPressedFunc
                                                : kPressedNormal;
    b->setBgStatusColor(ZK_CONTROL_STATUS_PRESSED, c);
  }
  for (int i = 0; i < kCandPerPage; ++i)
    if (candBtn_[i]) candBtn_[i]->setBgStatusColor(ZK_CONTROL_STATUS_PRESSED, kPressedNormal);
  if (modeBtn_) modeBtn_->setBgStatusColor(ZK_CONTROL_STATUS_PRESSED, kPressedFunc);
}

void PgImeApp::onInitIME(SIMETextInfo *pInfo) {
  shift_ = false;
  text_.clear();
  py_.clear();
  cands_.clear();
  page_ = 0;
  isPassword_ = false;
  if (pInfo) {
    isPassword_ = pInfo->isPassword;
    text_ = pInfo->text;
  }
  /* 手机惯例：**密码框强制英文**（密码里混汉字 = 必然连不上，且报错看不出原因）；
   * 其它框继承上次选择 —— 改名通常连着改几个，切一次就够。 */
  mode_ = (isPassword_ || !sLastChinese) ? MODE_EN : MODE_CN;
  LOGD("IME 打开：password=%d 类型=%d 初始 %d 字符，模式=%s",
       isPassword_ ? 1 : 0, pInfo ? (int)pInfo->imeTextType : -1, utf8Count(text_),
       mode_ == MODE_CN ? "中文" : "英文");
  qaSyncTag();                // 本会话的 QA 基线：只执行"打开之后新推的"命令
  syncShiftKeys();
  shown_.clear();
  hintShown_.clear();         // 强制重画（上一次的值可能残留）
  for (int i = 0; i < kCandPerPage; ++i) candShown_[i].clear();
  modeShown_.clear();
  syncInput();
}

void PgImeApp::setMode(int m) {
  if (mode_ == m) return;
  mode_ = m;
  sLastChinese = (m == MODE_CN);
  py_.clear();                // 换模式丢掉半截拼音（留着只会让人困惑）
  cands_.clear();
  page_ = 0;
  syncShiftKeys();
  syncInput();
  LOGD("IME 模式 -> %s", m == MODE_CN ? "中文（拼音）" : "英文");
}

/* 按当前 py_ 重算候选。
 * 顺序刻意是「整串词组 -> 整串单字 -> 前缀词组 -> 前缀单字」：
 *   打全了 `ketingdeng` 就该第一个出「客厅灯」；
 *   打到一半 `keting` 出「客厅」；
 *   再多打一个字母 `ketingd` 还能靠"前缀"给出「客厅灯」——不会因为"不完整"就空手。 */
void PgImeApp::buildCands() {
  cands_.clear();
  if (mode_ != MODE_CN || py_.empty()) return;
  const char *k = py_.c_str();

  const char *w = pyExact(kImePyWords, kImePyWordCount, k);
  if (w) splitBySpace(w, &cands_);
  const char *c = pyExact(kImePyChars, kImePyCharCount, k);
  if (c) splitByChar(c, &cands_);

  if ((int)cands_.size() < kMaxCand) {
    size_t klen = py_.size();
    int i = pyLowerBound(kImePyChars, kImePyCharCount, k);
    for (; i < kImePyCharCount && (int)cands_.size() < kMaxCand; ++i) {
      if (strncmp(kImePyChars[i].py, k, klen) != 0) break;
      if (strcmp(kImePyChars[i].py, k) == 0) continue;   // 精确那份上面加过了
      std::vector<std::string> tmp;
      splitByChar(kImePyChars[i].text, &tmp);
      for (size_t t = 0; t < tmp.size() && (int)cands_.size() < kMaxCand; ++t)
        cands_.push_back(tmp[t]);
    }
    i = pyLowerBound(kImePyWords, kImePyWordCount, k);
    for (; i < kImePyWordCount && (int)cands_.size() < kMaxCand; ++i) {
      if (strncmp(kImePyWords[i].py, k, klen) != 0) break;
      if (strcmp(kImePyWords[i].py, k) == 0) continue;
      std::vector<std::string> tmp;
      splitBySpace(kImePyWords[i].text, &tmp);
      for (size_t t = 0; t < tmp.size() && (int)cands_.size() < kMaxCand; ++t)
        cands_.push_back(tmp[t]);
    }
  }

  LOGD("IME 拼音 '%s' -> %d 个候选（首 %s）", k, (int)cands_.size(),
       cands_.empty() ? "无" : cands_[0].c_str());
}

void PgImeApp::candCommit(int idxOnPage) {
  int idx = page_ * kCandPerPage + idxOnPage;
  if (idx < 0 || idx >= (int)cands_.size()) return;
  const std::string &c = cands_[idx];
  if (utf8Count(text_) + utf8Count(c) > MAX_CHARS) {
    LOGW("IME 候选 '%s' 会让文本超上限 %d，已忽略", c.c_str(), MAX_CHARS);
    return;
  }
  text_ += c;
  py_.clear();
  cands_.clear();
  page_ = 0;
  syncInput();
  LOGD("IME 选中候选 '%s' -> 文本 %d 字符", c.c_str(), utf8Count(text_));
}

void PgImeApp::candPage(int delta) {
  int pages = ((int)cands_.size() + kCandPerPage - 1) / kCandPerPage;
  if (pages <= 0) return;
  int p = page_ + delta;
  if (p < 0) p = 0;
  if (p > pages - 1) p = pages - 1;
  if (p == page_) return;
  page_ = p;
  syncInput();
  LOGD("IME 候选翻页 -> 第 %d/%d 页", page_ + 1, pages);
}

void PgImeApp::syncInput() {
  if (inputBarPtr_) {
    std::string show;
    if (isPassword_) {
      int n = utf8Count(text_);
      for (int i = 0; i < n; ++i) show += "●";
    } else {
      show = text_;
    }
    if (show != shown_) {  // 只在变化时写控件：每帧无条件写 = 重绘风暴
      shown_ = show;
      inputBarPtr_->setText(shown_);
    }
  }

  // ---- 候选格 ----
  int pages = ((int)cands_.size() + kCandPerPage - 1) / kCandPerPage;
  for (int i = 0; i < kCandPerPage; ++i) {
    if (!candBtn_[i]) continue;
    int idx = page_ * kCandPerPage + i;
    std::string t = (idx < (int)cands_.size()) ? cands_[idx] : std::string(kEmptyCell);
    if (t != candShown_[i]) {
      candShown_[i] = t;
      candBtn_[i]->setText(t);
    }
  }

  // ---- 模式键：文字就是状态（不靠颜色，颜色只是辅助） ----
  if (modeBtn_) {
    std::string label = (mode_ == MODE_CN) ? "中文" : "英文";
    if (label != modeShown_) {
      modeShown_ = label;
      modeBtn_->setText(label);
    }
  }

  // ---- 功能行 ----
  if (hintPtr_) {
    char h[160];
    if (mode_ == MODE_EN) {
      if (isPassword_)
        snprintf(h, sizeof(h), "英文输入 · 密码不可用中文 · 点「完成」提交");
      else
        snprintf(h, sizeof(h), "英文输入 · 点「中文」可打汉字 · 点「完成」提交");
    } else if (py_.empty()) {
      snprintf(h, sizeof(h), "中文输入 · 直接打拼音，如 keting");
    } else if (cands_.empty()) {
      snprintf(h, sizeof(h), "拼音 %s · 没有匹配 · 退格改一下", py_.c_str());
    } else {
      snprintf(h, sizeof(h), "拼音 %s · 第 %d/%d 页 · 数字键或点格选字",
               py_.c_str(), page_ + 1, pages);
    }
    if (hintShown_ != h) {
      hintShown_ = h;
      hintPtr_->setText(hintShown_);
    }
  }
}

void PgImeApp::syncShiftKeys() {
  // 中文模式下字母键永远显示小写（拼音不打大写，免得误导）
  bool up = shift_ && mode_ == MODE_EN;
  for (int i = 1; i <= 45; ++i) {
    char c = kKeyChar[i];
    if (c < 'a' || c > 'z' || !keyBtn_[i]) continue;
    char label[2] = {(char)(up ? (c - 32) : c), 0};
    keyBtn_[i]->setText(label);
  }
  if (keyBtn_[K_SHIFT]) {
    keyBtn_[K_SHIFT]->setText(up ? "小写" : "大写");
    keyBtn_[K_SHIFT]->setTextColor(mode_ == MODE_EN ? 0xFFFF9F0A : 0xFF636366);
  }
}

/* 输入一个字符（字母/符号都走这里）。
 * 中文模式下**只有字母**进拼音缓冲，其余字符（数字、标点）先把拼音"落定"再插入。 */
void PgImeApp::typeChar(char c, bool isLetter) {
  if (mode_ == MODE_CN && isLetter) {
    if (utf8Count(py_) >= MAX_PY) {
      LOGW("IME 拼音缓冲已满 %d，忽略 '%c'", MAX_PY, c);
      return;
    }
    py_ += (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
    page_ = 0;
    buildCands();
    syncInput();
    return;
  }
  if (mode_ == MODE_CN && !py_.empty()) {
    /* 标点/数字落下时：**先把当前第一候选落定**（手机惯例：打完拼音接标点 = 认第一个字）。
     * ⚠️ 没候选（拼音打错了）也要**把拼音清掉** —— 否则拼音会挂在那里，
     *    后面再打字母会拼成一个四不像的串，用户只会觉得"坏了"。 */
    if (!cands_.empty()) {
      candCommit(0);
    } else {
      LOGD("IME 拼音 '%s' 无候选，接标点时丢弃", py_.c_str());
      py_.clear();
      cands_.clear();
      page_ = 0;
    }
  }
  if (utf8Count(text_) >= MAX_CHARS) return;
  text_ += c;
  syncInput();
}

void PgImeApp::handleKey(int idx) {
  if (idx <= 0 || idx > 45) return;
  if (idx == K_SHIFT) {
    if (mode_ == MODE_CN) {
      // 中文模式按大写键 = 想打大写字母 ⇒ 等价于切回英文（比什么都不做友好）
      setMode(MODE_EN);
      return;
    }
    shift_ = !shift_;
    syncShiftKeys();
    LOGD("IME 大小写 -> %s", shift_ ? "大写" : "小写");
    return;
  }
  if (idx == K_DEL) {
    if (mode_ == MODE_CN && !py_.empty()) {   // 先退拼音，拼音空了才退正文
      py_.erase(py_.size() - 1);
      page_ = 0;
      buildCands();
      syncInput();
      LOGD("IME 退格拼音 -> '%s'", py_.c_str());
      return;
    }
    if (text_.empty()) return;
    utf8PopBack(text_);
    syncInput();
    LOGD("IME 退格 -> %d 字符", utf8Count(text_));
    return;
  }
  if (idx == K_DONE) {
    if (candUsable()) candCommit(0);   // 有候选还没选就先落定，避免"打了拼音点完成丢字"
    commit();
    return;
  }
  char c = kKeyChar[idx];
  if (c == '\0') return;

  // 数字键在中文模式下 = 选本页第 n 个候选（1..kCandPerPage，与候选格数一致）
  // ⚠️ 别写 `c >= '1' && c <= '0'` —— '0'(48) < '1'(49)，那样一个数字都进不来。
  // ⚠️ 上界按 kCandPerPage 算，别写死 '6'（那样改成 5 格后按 6 会越界到下一页）。
  if (mode_ == MODE_CN && c >= '1' && c <= (char)('0' + kCandPerPage) &&
      !py_.empty() && !cands_.empty()) {
    candCommit(c - '1');
    return;
  }
  if (c == ' ' && mode_ == MODE_CN && !py_.empty()) {
    // 空格 = 选第一个候选（手机惯例）；拼音没候选时退回"插入空格"
    if (!cands_.empty()) {
      candCommit(0);
      return;
    }
  }
  bool wasLetter = (c >= 'a' && c <= 'z');
  if (mode_ == MODE_EN && shift_ && wasLetter) c = (char)(c - 32);
  typeChar(c, wasLetter);
  // 打一个**字母**后回小写（手机惯例）；符号不重置大写态（踩过：打 # 会把大写吃掉）
  if (mode_ == MODE_EN && shift_ && wasLetter) {
    shift_ = false;
    syncShiftKeys();
  }
  LOGD("IME 键 '%c' -> 正文 %d 字符，拼音 '%s'", c, utf8Count(text_), py_.c_str());
}

void PgImeApp::commit() {
  LOGD("IME 完成：交回 %d 字符 '%s'", utf8Count(text_), isPassword_ ? "(密码不外显)" : text_.c_str());
  doneIMETextUpdate(text_);
}

void PgImeApp::cancel() {
  LOGD("IME 收起：不保存");
  cancelIMETextUpdate();
}

void PgImeApp::onClick(ZKBase *pBase) {
  if (!pBase) return;
  int id = pBase->getID();
  if (id == ID_IME_BtnKbClear) {
    text_.clear();
    py_.clear();
    cands_.clear();
    page_ = 0;
    syncInput();
    LOGD("IME 清空");
    return;
  }
  if (id == ID_IME_BtnKbHide) {
    cancel();
    return;
  }
  if (id == ID_IME_BtnKbMode) {
    setMode(mode_ == MODE_CN ? MODE_EN : MODE_CN);
    return;
  }
  if (id == ID_IME_BtnKbPageUp) { candPage(-1); return; }
  if (id == ID_IME_BtnKbPageDown) { candPage(1); return; }
  for (int i = 0; i < kCandPerPage; ++i) {
    if (kCandIds[i] == id) { candCommit(i); return; }
  }
  for (int i = 1; i <= 45; ++i) {
    if (kKeyIds[i] == id) {
      handleKey(i);
      return;
    }
  }
  IMEBaseApp::onClick(pBase);
}

/* -------------------- QA：免触摸验收（/tmp/pg_imecmd） -------------------- */
void PgImeApp::qaSyncTag() {
  FILE *fp = fopen("/tmp/pg_imecmd", "r");
  if (!fp) return;
  size_t n = fread(qaTag_, 1, sizeof(qaTag_) - 1, fp);
  fclose(fp);
  qaTag_[n] = '\0';
}

void PgImeApp::qaDump() {
  /* ★ 判据必须**落文件**：本板 logcat 缓冲只十几行，中文日志还容易被截，
   *   验收脚本读这个文件比 grep 日志可靠。 */
  FILE *f = fopen("/tmp/pg_ime_dump.txt", "w");
  if (!f) {
    LOGW("IME QA: /tmp/pg_ime_dump.txt 打不开（落盘失败）");
    return;
  }
  fprintf(f, "mode=%s password=%d shift=%d\n", mode_ == MODE_CN ? "cn" : "en",
          isPassword_ ? 1 : 0, shift_ ? 1 : 0);
  fprintf(f, "text=%s\n", text_.c_str());
  fprintf(f, "textChars=%d\n", utf8Count(text_));
  fprintf(f, "py=%s\n", py_.c_str());
  int pages = ((int)cands_.size() + kCandPerPage - 1) / kCandPerPage;
  fprintf(f, "cands=%d page=%d pages=%d\n", (int)cands_.size(), page_ + 1, pages);
  for (size_t i = 0; i < cands_.size() && i < 24; ++i)
    fprintf(f, "  cand[%d]=%s\n", (int)i, cands_[i].c_str());
  fclose(f);
  LOGD("IME QA: 已写 /tmp/pg_ime_dump.txt（文本 %d 字符，拼音 '%s'，%d 候选）",
       utf8Count(text_), py_.c_str(), (int)cands_.size());
}

void PgImeApp::pollQa() {
  /* ⚠️ 只在键盘显示期间执行。IME 每次 show 都可能重建 App 实例，
   *    实例一重建 qaTag_ 就没了 —— 若不设基线，**上一次留下的命令会被重放**
   *    （实测踩到：重开后自动执行了上一条 `k 45`，用空密码发起了一次连接）。
   *    所以 onInitIME 里先把"当前文件内容"记成基线，只认之后的新内容。 */
  if (!isShow()) return;
  FILE *fp = fopen("/tmp/pg_imecmd", "r");
  if (!fp) return;
  char buf[1024] = {0};
  size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  fclose(fp);
  if (n == 0) return;
  if (strncmp(buf, qaTag_, sizeof(qaTag_) - 1) == 0) return;  // 整份去重
  strncpy(qaTag_, buf, sizeof(qaTag_) - 1);
  qaTag_[sizeof(qaTag_) - 1] = '\0';

  char *save = NULL;
  for (char *line = strtok_r(buf, "\r\n", &save); line;
       line = strtok_r(NULL, "\r\n", &save)) {
    while (*line == ' ') ++line;
    if (*line == '\0' || *line == '#') continue;
    if (strncmp(line, "k ", 2) == 0) {
      int idx = atoi(line + 2);
      LOGD("IME QA: k %d", idx);
      handleKey(idx);
    } else if (strncmp(line, "btn ", 4) == 0) {
      /* 与 `k` 的区别：`k` 直接调 handleKey（跳过派发），`btn` 拿**真实控件指针**
       * 喂给 onClick() —— 验的是"控件 ID -> 键序号"映射表（最可能写错的地方）。 */
      int idx = atoi(line + 4);
      LOGD("IME QA: btn %d (id=%d)", idx, (idx >= 1 && idx <= 45) ? kKeyIds[idx] : -1);
      if (idx >= 1 && idx <= 45 && keyBtn_[idx]) onClick(keyBtn_[idx]);
    } else if (strncmp(line, "shift", 5) == 0) {
      handleKey(K_SHIFT);
    } else if (strncmp(line, "del", 3) == 0) {
      handleKey(K_DEL);
    } else if (strncmp(line, "done", 4) == 0) {
      handleKey(K_DONE);
    } else if (strncmp(line, "hide", 4) == 0) {
      cancel();
    } else if (strncmp(line, "clear", 5) == 0) {
      text_.clear();
      py_.clear();
      cands_.clear();
      page_ = 0;
      syncInput();
    } else if (strncmp(line, "mode", 4) == 0) {
      /* `mode` = 切换；`mode cn` / `mode en` = **显式设置**。
       * 为什么要有显式版：toggle 的 QA 必须先 dump 读一次"现在是什么模式"才知道要不要翻，
       * 多一轮往返；而且读到的模式与真正执行时可能已经变了。 */
      const char *a = line + 4;
      while (*a == ' ') ++a;
      if (strncmp(a, "cn", 2) == 0) setMode(MODE_CN);
      else if (strncmp(a, "en", 2) == 0) setMode(MODE_EN);
      else setMode(mode_ == MODE_CN ? MODE_EN : MODE_CN);
    } else if (strncmp(line, "cand ", 5) == 0) {
      int n = atoi(line + 5);
      LOGD("IME QA: cand %d", n);
      if (n >= 1 && n <= kCandPerPage) candCommit(n - 1);
    } else if (strncmp(line, "page ", 5) == 0) {
      candPage(strncmp(line + 5, "up", 2) == 0 ? -1 : 1);
    } else if (strncmp(line, "type ", 5) == 0) {
      /* 整串喂键：省得为输一个词写 20 行 QA。走的还是 handleKey（同真实按键）。
       *
       * ⚠️ 必须**先剥行尾 `#...` 注释**（与 mainLogic.cc 的 runAutoCmd 同一纪律）。
       *   不剥的后果是**静默**的：`type keting #12` 会把 '#'、'1'、'2' 也当键喂进去
       *   （它们都是键盘上真有的键），于是文本里多出 "#12" —— 而 QA 脚本习惯用
       *   `#编号` 保证"整份内容变化才执行"，等于每一轮都悄悄脏一次。
       *   ⚠️ 真要在 QA 里输入 `#` 字符，请用 `k 42`（键序号，见 kKeyChar）。 */
      char *arg = line + 5;
      char *hash = strchr(arg, '#');
      if (hash) *hash = '\0';
      LOGD("IME QA: type '%s'", arg);
      for (const char *p = arg; *p; ++p) {
        char c = *p;
        if (c == ' ') continue;
        int idx = -1;
        for (int i = 1; i <= 45; ++i)
          if (kKeyChar[i] == c) { idx = i; break; }
        if (idx < 0) {
          LOGW("IME QA: type 里 '%c' 不是键盘上的键，跳过", c);
          continue;
        }
        handleKey(idx);
      }
    } else if (strncmp(line, "dump", 4) == 0) {
      qaDump();
    } else if (strncmp(line, "lbl", 3) == 0) {
      LOGD("IME QA: 文本(%d) = '%s' 拼音 '%s'", utf8Count(text_), text_.c_str(), py_.c_str());
    } else {
      LOGD("IME QA: 未知命令 '%s'", line);
    }
  }
}

bool PgImeApp::onTimer(int id) {
  if (id == 1) pollQa();
  return true;
}

REGISTER_SYSAPP(APP_TYPE_SYS_IME, PgImeApp);
