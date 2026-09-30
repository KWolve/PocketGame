/*
 * PgMusic.h - 「下落式节奏」共用内核（节奏钢琴 / 打鼓 两款共用）
 *
 * 为什么单独抽出来：这两款游戏的玩法骨架一模一样 ——
 *     音符从上方按时刻下落 → 落地判定线时按对应键/鼓位 → 三档判定 + 连击 + 准确率
 * 只有"乐器怎么发声"和"谱面数据"不同。内核（判定窗口 / 计分 / 谱面游标）抽在这里，
 * 两款游戏各自只写"布局 + 谱面 + 音色"。
 *
 * ⚠️ **本文件禁止调用任何文字 API**（`c.text/bigText/number` 等）！
 *    工程纪律：`tools/genfont.py` 会扫描**头文件**里的文字调用，把字面量收进字库；
 *    但共用头被两个 .cpp 包含 ⇒ 收进去的字到底属于哪一档会变得不可控，
 *    而且最坏情况会被判成"非字面量实参"而退化（血案：字库 2.70MB → 4.67MB）。
 *    所有文字绘制留在各自的 .cpp 里。
 */
#ifndef PG_MUSIC_H_
#define PG_MUSIC_H_

namespace pg {
namespace music {

/* 判定窗口（毫秒，取 |实际点击时刻 - 音符应击时刻|）。
 * 手感标定：本板触摸上报到 MotionEvent 之间有约 20~40ms 的不确定度，
 * 所以 Perfect 不能小于 50ms，否则"手指明明跟上了却全是 Good"。
 * 三档 + 漏（超过 OK 窗口就不再接这一击，音符自己走过去变成 MISS）。 */
enum { JUMP_PERFECT = 60, JUMP_GOOD = 130, JUMP_OK = 200 };

enum Judge { J_MISS = 0, J_OK, J_GOOD, J_PERFECT };

/* 判定窗口的三档阈值（毫秒）。
 * ★ 为什么抽成结构体：**共享内核只提供"标准窗口"，不替任何游戏做决定**——
 *   谁要另开一档（例如打鼓的「入门」把窗口放宽到 100/200/300）就把自己的表传给
 *   `judgeOfWin()`。踩过的教训：在共用内核里改默认值 ⇒ 会把另一个游戏一起改掉。 */
struct Win {
  int perfect, good, ok;
};

inline const Win &stdWin() {
  static const Win w = {JUMP_PERFECT, JUMP_GOOD, JUMP_OK};
  return w;
}

inline Judge judgeOfWin(int dtAbsMs, const Win &w) {
  if (dtAbsMs <= w.perfect) return J_PERFECT;
  if (dtAbsMs <= w.good) return J_GOOD;
  if (dtAbsMs <= w.ok) return J_OK;
  return J_MISS;
}

/* 钢琴用的标准三档（行为与加 `Win` 之前完全一致）。 */
inline Judge judgeOf(int dtAbsMs) { return judgeOfWin(dtAbsMs, stdWin()); }

/* 每档的基础分（连击再加成）。数值取自"打满一首歌大约 2~4 千分"的手感。 */
inline int judgeScore(Judge j, int combo) {
  int base = 0;
  if (j == J_PERFECT) {
    base = 100;
  } else if (j == J_GOOD) {
    base = 60;
  } else if (j == J_OK) {
    base = 25;
  } else {
    return 0;
  }
  int bonus = combo * 2;
  if (bonus > 200) bonus = 200;   // 连击加成封顶，避免"后半程一次点空就崩盘"
  return base + bonus;
}

/* 准确率（0..1000，千分比）：perfect 记 1000 / good 650 / ok 300 / miss 0。
 * 用千分比而不是百分比的**整数**，是为了让"HUD 显示 97.4%"与"结算页的 97.4%"
 * 同源（同一份整数算出，不做两次浮点舍入）。 */
inline int accuracyPermille(int perfect, int good, int ok, int miss) {
  int n = perfect + good + ok + miss;
  if (n <= 0) return 0;
  long long s = (long long)perfect * 1000 + (long long)good * 650 +
                (long long)ok * 300;
  return (int)(s / n);
}

/* 三档难度：下落速度（音符从顶走到判定线要多久）与曲速。 */
enum { DIFF_EASY = 0, DIFF_NORMAL, DIFF_HARD, DIFF_COUNT };

inline const char *diffName(int d) {
  if (d == DIFF_EASY) return "慢";
  if (d == DIFF_HARD) return "快";
  return "中";
}
/* 下落时长（毫秒）：数值越小音符掉得越快。 */
inline int fallMs(int diff) {
  if (diff == DIFF_EASY) return 2600;
  if (diff == DIFF_HARD) return 1250;
  return 1800;
}
/* 曲速倍率（千分比，作用在谱面自带的 BPM 上） */
inline int tempoPermille(int diff) {
  if (diff == DIFF_EASY) return 850;
  if (diff == DIFF_HARD) return 1180;
  return 1000;
}

/* ---------------- 打鼓专用的四档（比共用三档多一个「入门」） ----------------
 *
 * ★ 为什么不直接扩 `fallMs()`：那是**钢琴与打鼓共用**的表，插一档会让钢琴也变
 *   （本次需求只要打鼓加档）⇒ 打鼓自己这张表放这里，**钢琴一行都不受影响**。
 *
 * 索引顺序固定 {入门, 慢, 中, 快} —— 与 QA `gdbg diff n`（n=1..4）、info 条的"速度"、
 * 以及**默认档**（中 = 2，与加档之前的手感一致）都以它为准，**别插中间**。
 * ⚠️ 档位不落盘（每次进游戏从 `DRUM_DIFF_DEFAULT` 开始），所以顺序只影响当下这一次。
 *
 * 「入门」的取舍：下落 3600ms（音符在屏时间 ≈2.8s）+ 窗口 100/200/300ms，
 * 给小孩与第一次玩的人；**曲速不变**（打鼓的既定设计：变速会让节奏型失真）。 */
struct Diff4 {
  int fallMs;
  Win win;
};

inline const Diff4 &drumDiff(int d) {
  static const Diff4 t[4] = {
      {3600, {100, 200, 300}},   // 入门
      {2600, {60, 130, 200}},    // 慢（== 共用的"慢"）
      {1800, {60, 130, 200}},    // 中（默认，== 共用的"中"）
      {1250, {60, 130, 200}},    // 快（== 共用的"快"）
  };
  if (d < 0 || d > 3) d = 2;
  return t[d];
}

enum { DRUM_DIFF_COUNT = 4, DRUM_DIFF_DEFAULT = 2 };

/* 一个可命中的音符。谱面表是静态常量，**运行时不改它** ——
 * 已命中的状态放在游戏侧的 hitMask 里（谱面 ≤ 200 个音符，用 4 个 uint32 位图）。 */
struct Note {
  int lane;    // 音轨（钢琴 0..7 = do..do'，鼓 0..5 = 鼓位）
  int atMs;    // 应击时刻（相对曲子开头）
};

const int NOTE_MAX = 200;   // 单曲音符上限（够 4 段 8 小节的简单旋律）

}  // namespace music
}  // namespace pg

#endif  // PG_MUSIC_H_
