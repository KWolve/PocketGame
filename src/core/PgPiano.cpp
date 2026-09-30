/*
 * PgPiano.cpp - 节奏钢琴（下落式跟弹 · 8 键 · 木纹立式琴）
 *
 * 玩法：音符从上方沿 8 条音轨落下，落到判定线（y=388）时点对应琴键。
 *   判定三档 Perfect/Good/OK + 漏（MISS）；连击加成；打完整曲出准确率结算。
 *   **空点琴键也会出声**（自由弹奏的余味），只是不计分 —— 这是"乐器"该有的手感。
 *
 * 音高：8 个音 = C 大调 do..do'（C4 523.25Hz → C5 1046.5Hz），
 *   素材在 resources/audio/pno1..8.wav（tools/gensfx.py 的 pluck 合成）。
 *   ⚠️ **不为变调去改播放速率**：那会连带改时长 ⇒ 节奏会漂。所以是"一个音一个文件"。
 *
 * 与打鼓共用内核：core/PgMusic.h（判定窗口 / 计分 / 三档速度）。
 *
 * 视觉：木纹实体乐器风；素材见 tools/gen_instr_art.py 的 PIANO 段。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "PgGames.h"
#include "core/PgLog.h"
#include "core/PgMusic.h"
#include "core/PgSpriteDraw.h"

namespace pg {

namespace {

using gameart::kPianoBg;
using gameart::kPianoJudgeY;
using gameart::kPianoKeyH;
using gameart::kPianoKeyW;
using gameart::kPianoKeyY;
using gameart::kPianoNoteH;

const int kLanes = 8;
const int kLaneW = 60;      // == kPianoKeyW（键宽 = 画布宽 / 8）
const int kLeadInMs = 1500; // 起播留白（音符先掉下来，给玩家反应时间）
const int kResultMs = 4200; // 结算页停留
const int kFallTop = 46;    // 顶部 46px 留给"曲目/速度"两个信息条
const int kJudgeShowMs = 380;

/* 曲谱：degree 串（1..8 = do..do'，0 = 休止，一个 token = 一拍）。
 * ★ 为什么用字符串而不是结构体数组：加一首歌只要写一行字，改谱不用重排下标，
 *   而且"谱面是不是对的"肉眼就能读（结构体数组读起来是 8 个数字，看不出旋律）。
 * 解析在 loadSong 里；**上限 music::NOTE_MAX = 200 个音符**，超了会打日志截断。 */
struct Song {
  const char *name;
  int bpm;
  int repeat;
  const char *deg;
};

const Song kSongs[] = {
    {"小星星", 108, 1,
     "1 1 5 5 6 6 5 0 4 4 3 3 2 2 1 0 5 5 4 4 3 3 2 0 5 5 4 4 3 3 2 0 "
     "1 1 5 5 6 6 5 0 4 4 3 3 2 2 1 0"},
    {"欢乐颂", 112, 1,
     "3 3 4 5 5 4 3 2 1 1 2 3 3 2 2 0 3 3 4 5 5 4 3 2 1 1 2 3 2 1 1 0"},
    {"玛丽小羊", 116, 1,
     "3 2 1 2 3 3 3 0 2 2 2 0 3 5 5 0 3 2 1 2 3 3 3 3 2 2 3 2 1 0 0 0"},
    {"划船歌", 100, 1,
     "1 1 1 2 3 3 2 3 4 5 5 5 3 3 3 1 1 1 5 4 3 2 1 0"},
};
const int kSongCount = (int)(sizeof(kSongs) / sizeof(kSongs[0]));

const gameart::Def *noteDef(int lane) {
  static const gameart::Def *const defs[8] = {
      &gameart::kPianoNote0, &gameart::kPianoNote1, &gameart::kPianoNote2,
      &gameart::kPianoNote3, &gameart::kPianoNote4, &gameart::kPianoNote5,
      &gameart::kPianoNote6, &gameart::kPianoNote7};
  return defs[lane & 7];
}

const gameart::Def *keyDef(int lane) {
  static const gameart::Def *const defs[8] = {
      &gameart::kPianoKey0, &gameart::kPianoKey1, &gameart::kPianoKey2,
      &gameart::kPianoKey3, &gameart::kPianoKey4, &gameart::kPianoKey5,
      &gameart::kPianoKey6, &gameart::kPianoKey7};
  return defs[lane & 7];
}

const char *judgeText(int j) {
  switch (j) {
    case music::J_PERFECT: return "完美";
    case music::J_GOOD: return "不错";
    case music::J_OK: return "还行";
    default: return "漏了";
  }
}

/* 音符的屏幕 y（**唯一的映射实现**，render 与 QA 都调它）。
 *
 * ★★ 血案（2026-09-16 用户报"看不到音符下落"）：原写法是
 *      const int pxPerMs = (kPianoJudgeY + kPianoNoteH) / music::fallMs(diff_);
 *      y = kPianoJudgeY - (atMs - mtAll) * pxPerMs;
 *    —— **整数除法先把"每毫秒几个像素"算成了 0**（416 / 1250~2600 = 0，
 *    即每秒只走 0.16~0.33 像素）⇒ 所有音符的 y 恒等于判定线 388，
 *    而且那一行正好被画在**上层**的琴键（kPianoKeyY=392）盖住
 *    ⇒ 表现就是"完全没有下落动画"。编译、断言、QA 计数全都正常，
 *    只有真机像素看得出 —— 与"改了生成器几何常量"那类坑同源：**几何算式要看真值**。
 *
 * 正确做法：**先乘后除**（保留亚像素精度）。用 long long：|dt| 最大约几十秒 ✕ 416 ≈ 1e7，
 * 远小于 int32 上限；dt=0 时 y=判定线，dt=fallMs 时 y=-noteH（刚好从顶部完全进入）。 */
inline int noteScreenY(int judgeY, int noteH, int atMs, int mtAll, int diff) {
  const int fall = music::fallMs(diff);
  if (fall <= 0) return judgeY;   // 防御：fallMs 被改成 0 时不要除零
  return judgeY -
         (int)((long long)(atMs - mtAll) * (judgeY + noteH) / fall);
}
Color judgeColor(int j) {
  switch (j) {
    case music::J_PERFECT: return rgb(0x6BE39A);
    case music::J_GOOD: return rgb(0xFFD24A);
    case music::J_OK: return rgb(0xFFA24A);
    default: return rgb(0xFF5B4A);
  }
}

}  // namespace

// ---------------- 卡片元信息 ----------------
const char *GamePiano::title() const { return "节奏钢琴"; }
const char *GamePiano::desc() const { return "音符落到线时按琴键，8 键跟弹"; }
const char *GamePiano::tag() const { return "PIANO"; }
Color GamePiano::theme() const { return rgb(0x7A5CC6); }

// ---------------- 谱面 ----------------
void GamePiano::loadSong(int idx) {
  const int n = kSongCount;
  if (n <= 0) return;
  idx %= n;
  if (idx < 0) idx += n;
  song_ = idx;

  const Song &s = kSongs[idx];
  const int beatMs = (int)((60000LL * music::tempoPermille(diff_)) / (s.bpm * 1000LL));
  nNotes_ = 0;
  int beat = 0;
  const char *p = s.deg;
  while (*p && nNotes_ < music::NOTE_MAX) {
    while (*p == ' ') ++p;
    if (!*p) break;
    const char c = *p++;
    if (c >= '1' && c <= '8') {
      notes_[nNotes_].lane = c - '1';
      notes_[nNotes_].atMs = beat * beatMs;
      ++nNotes_;
    }
    ++beat;   // '0'（休止）与其它字符都只推进节拍
  }
  if (*p && nNotes_ >= music::NOTE_MAX) {
    logInfo("Piano: 谱面超过 %d 个音符，已截断（曲子=%s）", music::NOTE_MAX, s.name);
  }
  for (int i = 0; i < music::NOTE_MAX; ++i) done_[i] = false;
  logInfo("Piano: 载入曲目 %s（bpm=%d 速度=%s 拍长=%dms 音符=%d 时长=%dms）", s.name,
          s.bpm, music::diffName(diff_), beatMs, nNotes_,
          nNotes_ > 0 ? notes_[nNotes_ - 1].atMs + beatMs : 0);
}

// ---------------- 生命周期 ----------------
void GamePiano::reset() {
  score_ = 0;
  combo_ = 0;
  maxCombo_ = 0;
  next_ = 0;
  nowMs_ = 0;
  msAcc_ = 0;
  startDelayMs_ = kLeadInMs;
  lastJudgeMs_ = 0;
  lastJudgeLane_ = -1;
  celebMs_ = 0;
  metronomeMs_ = 0;
  overFlag_ = false;
  for (int i = 0; i < 5; ++i) cnt_[i] = 0;
  for (int i = 0; i < kLanes; ++i) {
    keyFlash_[i] = 0;
    keyDown_[i] = false;
  }
  loadSong(song_);
}

void GamePiano::update(int dtMs) {
  if (dtMs < 0) dtMs = 0;
  if (dtMs > 200) dtMs = 200;   // 与宿主一致地钳一下，防止一次大跳把谱面冲过去
  for (int i = 0; i < kLanes; ++i) {
    if (keyFlash_[i] > 0) keyFlash_[i] -= dtMs;
  }
  if (lastJudgeMs_ > 0) lastJudgeMs_ -= dtMs;

  if (celebMs_ > 0) {
    celebMs_ -= dtMs;
    if (celebMs_ <= 0) {
      reset();                       // 同一首、同一速度再来一遍
      state_ = GSTATE_RUNNING;
    }
    return;
  }
  if (state_ != GSTATE_RUNNING) return;

  if (startDelayMs_ > 0) {
    startDelayMs_ -= dtMs;
    if (startDelayMs_ < 0) startDelayMs_ = 0;
  }
  nowMs_ += dtMs;
  msAcc_ += dtMs;
  metronomeMs_ = (metronomeMs_ + dtMs) % 500;
  if (nowMs_ < kLeadInMs) return;   // 起播留白期间不计漏（音符还没到线）
  const int mt = nowMs_ - kLeadInMs;

  /* ---- 漏判：把"已经过判定窗口还没被结算"的音符依次标记 ---- */
  while (next_ < nNotes_ && done_[next_]) ++next_;
  while (next_ < nNotes_ && mt - notes_[next_].atMs > music::JUMP_OK) {
    done_[next_] = true;
    ++cnt_[music::J_MISS];
    combo_ = 0;
    lastJudge_ = music::J_MISS;
    lastJudgeMs_ = kJudgeShowMs;
    lastJudgeLane_ = notes_[next_].lane;
    logInfo("Piano: 漏掉音符 #%d（lane=%d at=%dms）", next_, notes_[next_].lane,
            notes_[next_].atMs);
    ++next_;
    while (next_ < nNotes_ && done_[next_]) ++next_;
  }

  /* ---- 整曲结束：所有音符都结算完，且最后一个音符过了收尾窗口 ---- */
  if (next_ >= nNotes_) {
    celebMs_ = kResultMs;
    logInfo("Piano: 打完了 —— 完美 %d 不错 %d 还行 %d 漏 %d 最高连击 %d 得分 %d",
            cnt_[music::J_PERFECT], cnt_[music::J_GOOD], cnt_[music::J_OK],
            cnt_[music::J_MISS], maxCombo_, score_);
  }
}

// ---------------- 判定 ----------------
bool GamePiano::tapLane(int lane, bool fromAuto) {
  if (lane < 0 || lane >= kLanes) return false;
  /* 琴键永远出声（自由弹奏的手感）—— 但**计数只算曲子进行中**的点击。 */
  sfx(SFX_PNO1 + lane);
  keyFlash_[lane] = 220;
  if (state_ != GSTATE_RUNNING || celebMs_ > 0) return true;
  if (nowMs_ < kLeadInMs) return true;      // 留白期：只出声不计分
  const int mt = nowMs_ - kLeadInMs;

  /* 找该音轨里"离当前时刻最近且还没结算"的音符。谱面只有几十上百个，
   * 线性扫一遍远比维护"每轨游标"简单，而点击是低频事件（不进每帧路径）。 */
  int best = -1, bestDt = music::JUMP_OK + 1;
  for (int i = 0; i < nNotes_; ++i) {
    if (done_[i] || notes_[i].lane != lane) continue;
    int dt = mt - notes_[i].atMs;
    if (dt < 0) dt = -dt;
    if (dt < bestDt) {
      bestDt = dt;
      best = i;
    }
  }
  if (best < 0) {
    /* 该音轨附近没有音符 —— **空点不扣分**（乐器就该这样），只打一行日志。
     * ★ 这一行很重要：没有它，"音符没被结算"与"点击根本没进判定"分不出来。 */
    logInfo("Piano: 空点 lane=%d（该轨无待结算音符）%s", lane, fromAuto ? " [auto]" : "");
    return true;
  }
  const music::Judge j = music::judgeOf(bestDt);
  done_[best] = true;
  ++cnt_[j];
  if (j == music::J_MISS) {
    combo_ = 0;   // judgeOf 只在窗口内返回非 MISS，这里理论上进不来（防御）
  } else {
    ++combo_;
    if (combo_ > maxCombo_) maxCombo_ = combo_;
  }
  score_ += music::judgeScore(j, combo_);
  saveBestIfNeeded(score_);
  lastJudge_ = j;
  lastJudgeMs_ = kJudgeShowMs;
  lastJudgeLane_ = lane;
  while (next_ < nNotes_ && done_[next_]) ++next_;
  logInfo("Piano: 命中 #%d lane=%d dt=%dms -> %s combo=%d score=%d%s", best, lane,
          mt - notes_[best].atMs, judgeText(j), combo_, score_, fromAuto ? " [auto]" : "");
  return true;
}

// ---------------- 绘制 ----------------
void GamePiano::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  blit(c, kPianoBg, 0, 0);

  const int mtAll = nowMs_ - kLeadInMs;
  const bool playing = (state_ == GSTATE_RUNNING && celebMs_ <= 0 && nowMs_ >= kLeadInMs);

  /* ---- 「最佳时机」窗口色带（画在音符**之下**，所以音符落进来时还看得见）----
   * 高度用**与 noteScreenY 同一套映射**换算：windowPx = ms * (judgeY + noteH) / fallMs
   * （先乘后除；血案见 noteScreenY 的说明）。
   * 玩家读到的信息就是「音符落进这条带子里时按 = 最佳」：内层亮带 = Perfect，
   * 外层淡带 = Good；再外面就是 OK（不画，靠线的颜色反馈）。 */
  const int fall = music::fallMs(diff_);
  const music::Win &win = music::stdWin();
  const int spanY = kPianoJudgeY + kPianoNoteH;      // 与 noteScreenY 的分子同源
  int bandP = (fall > 0) ? win.perfect * spanY / fall : 4;
  int bandG = (fall > 0) ? win.good * spanY / fall : 8;
  if (bandP < 4) bandP = 4;                          // 太快档位下也要看得见
  if (bandG < bandP + 3) bandG = bandP + 3;
  c.blendRect(0, kPianoJudgeY - bandG, W, bandG * 2, rgba(0x8A, 0x6A, 0x2A), 52);
  c.blendRect(0, kPianoJudgeY - bandP, W, bandP * 2, rgba(0xFF, 0xC6, 0x4A), 76);

  /* ---- 音符（画在琴键/信息条**之前**的层级：音符在下、键在上）----
   * y 由 noteScreenY 统一计算（**先乘后除**，别在这里重写算式 —— 见它的血案说明）。 */
  uint32_t hotLane = 0;   // 本帧已进入 Perfect 窗口的音轨位图（驱动落点标记点亮）
  for (int i = 0; i < nNotes_; ++i) {
    if (done_[i]) continue;
    const int y = noteScreenY(kPianoJudgeY, kPianoNoteH, notes_[i].atMs, mtAll, diff_);
    if (playing) {
      const int dt = notes_[i].atMs - mtAll;
      if (dt >= -win.perfect && dt <= win.perfect) hotLane |= (1u << notes_[i].lane);
    }
    if (y < kFallTop) continue;                       // 还没进舞台
    if (y > kPianoJudgeY + kPianoNoteH) continue;     // 已经过线（马上会被判漏）
    blit(c, *noteDef(notes_[i].lane), notes_[i].lane * kLaneW + 2, y);
  }

  /* ---- 判定线（把底图那条细木线加粗提亮）+ 每轨落点标记 ----
   * 落点标记 = 音轨与判定线的交点；该轨的音符进入 Perfect 窗口时**变亮放大**
   *（等于告诉玩家「现在按这一条」）。 */
  c.fillRect(0, kPianoJudgeY - 1, W, 3, rgba(0xFF, 0xF0, 0xC0));
  c.blendRect(0, kPianoJudgeY + 2, W, 3, rgba(0x50, 0x38, 0x18), 120);
  for (int i = 0; i < kLanes; ++i) {
    const int cx = i * kLaneW + kLaneW / 2;
    if ((hotLane >> i) & 1) {
      c.fillRect(cx - 11, kPianoJudgeY - 5, 22, 11, rgba(0xFF, 0xE2, 0x8A));
    } else {
      c.fillRect(cx - 8, kPianoJudgeY - 2, 16, 5, rgba(0xD8, 0xBE, 0x8C));
    }
  }

  /* ---- 琴键（含按下态：整体下沉 5px，露出上方的键槽阴影）---- */
  for (int i = 0; i < kLanes; ++i) {
    const int dy = keyDown_[i] ? 5 : 0;
    blit(c, *keyDef(i), i * kLaneW, kPianoKeyY + dy);
    if (keyFlash_[i] > 0) {
      const int a = keyFlash_[i] * 110 / 220;
      c.blendRect(i * kLaneW + 4, kPianoKeyY + dy + 4, kLaneW - 8, kPianoKeyH - 8,
                  judgeColor(playing ? music::J_PERFECT : music::J_OK), a);
    }
  }

  /* ---- 顶部两条信息条（点它切曲目/切速度）---- */
  c.fillRectRound(8, 8, 214, 32, 8, rgba(28, 18, 10, 200));
  c.text(18, 11, "曲目", 2, rgb(0xC9B79A));
  c.text(18 + c.textW("曲目", 2) + 8, 11, kSongs[song_].name, 2, rgb(0xF6EFE2));
  c.fillRectRound(258, 8, 214, 32, 8, rgba(28, 18, 10, 200));
  c.text(268, 11, "速度", 2, rgb(0xC9B79A));
  c.text(268 + c.textW("速度", 2) + 8, 11, music::diffName(diff_), 2, rgb(0xFFD24A));

  /* ---- 节拍器：判定线两端各一个小点，每拍闪一次（帮助建立"拍"的感觉）---- */
  if (playing) {
    const bool on = metronomeMs_ < 120;
    const Color mc = on ? rgb(0xFFE9A8) : rgb(0x6A5638);
    c.fillCircle(8, kPianoJudgeY + 14, 5, mc);
    c.fillCircle(W - 8, kPianoJudgeY + 14, 5, mc);
  }

  /* ---- 判定文字 + 连击（在判定线上方）---- */
  if (lastJudgeMs_ > 0 && playing) {
    const int a = lastJudgeMs_ > 200 ? 255 : lastJudgeMs_ * 255 / 200;
    c.blendRect(0, kPianoJudgeY - 92, W, 44, rgba(0, 0, 0), a * 90 / 255);
    c.textCenter(W / 2, kPianoJudgeY - 88, judgeText(lastJudge_), 3,
                judgeColor(lastJudge_));
  }
  if (playing && combo_ >= 2) {
    const int w = c.numberW(combo_, 4);
    c.number(W / 2 - w / 2, kPianoJudgeY - 148, combo_, 4, rgb(0xFFE9A8));
    c.textCenter(W / 2, kPianoJudgeY - 196, "连击", 2, rgb(0xC9B79A));
  }

  /* ---- 覆盖层 ---- */
  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 145);
    c.textCenter(W / 2, H / 2 - 120, "节奏钢琴", 5, rgb(0xE3D4FF));
    c.textCenter(W / 2, H / 2 - 34, "音符落到线时按琴键", 2, rgb(0xF2F2F7));
    c.textCenter(W / 2, H / 2 + 6, "空点也出声，随便玩", 2, rgb(0xC7C7CC));
    c.textCenter(W / 2, H / 2 + 48, "点屏幕开始", 2, rgb(0xFFD24A));
  } else if (celebMs_ > 0) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    const int acc = music::accuracyPermille(cnt_[music::J_PERFECT], cnt_[music::J_GOOD],
                                           cnt_[music::J_OK], cnt_[music::J_MISS]);
    c.textCenter(W / 2, H / 2 - 148, "演奏结束", 4, rgb(0xE3D4FF));
    /* ★★ 这里**手工排版**而不是 snprintf 拼串再 textCenter ：
     *   `tools/genfont.py` 对"非字面量字符串实参"会退化成"本文件全部短字面量"的保守超集
     *   （见 MEMORY.md 规则 9 的血案）。数字一律走 bigNumber/number（**不在扫描名单** ⇒
     *   零新增字形），标签用字面量，位置自己用宽度算。 */
    const int pw = c.bigNumberW(acc / 10, 2);
    const int dw = c.textW(".", 4);
    const int fw = c.numberW(acc % 10, 4);
    const int gw = c.textW("%", 4);
    int x = (W - (pw + dw + fw + gw)) / 2;
    const int yAcc = H / 2 - 86;
    c.bigNumber(x, yAcc, acc / 10, 2, rgb(0xFFE9A8));
    x += pw;
    c.text(x, yAcc, ".", 4, rgb(0xFFE9A8));
    x += dw;
    c.number(x, yAcc, acc % 10, 4, rgb(0xFFE9A8));
    x += fw;
    c.text(x, yAcc, "%", 4, rgb(0xFFE9A8));
    c.textCenter(W / 2, yAcc + 56, "准确率", 2, rgb(0xC9B79A));

    const int y1 = H / 2 + 10;
    int wA = c.textW("完美", 2), wB = c.numberW(cnt_[music::J_PERFECT], 2);
    int wC = c.textW("不错", 2), wD = c.numberW(cnt_[music::J_GOOD], 2);
    x = (W - (wA + wB + wC + wD + 36)) / 2;
    c.text(x, y1, "完美", 2, rgb(0x6BE39A));
    x += wA + 6;
    c.number(x, y1, cnt_[music::J_PERFECT], 2, rgb(0xF2F2F7));
    x += wB + 30;
    c.text(x, y1, "不错", 2, rgb(0xFFD24A));
    x += wC + 6;
    c.number(x, y1, cnt_[music::J_GOOD], 2, rgb(0xF2F2F7));

    const int y2 = y1 + 40;
    wA = c.textW("还行", 2), wB = c.numberW(cnt_[music::J_OK], 2);
    wC = c.textW("漏", 2), wD = c.numberW(cnt_[music::J_MISS], 2);
    x = (W - (wA + wB + wC + wD + 36)) / 2;
    c.text(x, y2, "还行", 2, rgb(0xFFA24A));
    x += wA + 6;
    c.number(x, y2, cnt_[music::J_OK], 2, rgb(0xF2F2F7));
    x += wB + 30;
    c.text(x, y2, "漏", 2, rgb(0xFF5B4A));
    x += wC + 6;
    c.number(x, y2, cnt_[music::J_MISS], 2, rgb(0xF2F2F7));

    const int y3 = y2 + 40;
    const int wL = c.textW("最高连击", 2);
    const int wN = c.numberW(maxCombo_, 2);
    x = (W - (wL + wN + 8)) / 2;
    c.text(x, y3, "最高连击", 2, rgb(0xC9B79A));
    c.number(x + wL + 8, y3, maxCombo_, 2, rgb(0xFFD24A));
  }
}

// ---------------- 输入 ----------------
int GamePiano::laneAt(int x, int y) const {
  if (y < kPianoKeyY - 26 || y >= kPianoKeyY + kPianoKeyH) return -1;  // 判定线上下都算"琴键区"
  if (x < 0 || x >= kLanes * kLaneW) return -1;
  return x / kLaneW;
}

bool GamePiano::onTouch(int action, int x, int y) {
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      reset();
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (action == PG_TOUCH_UP) {
    const int l = laneAt(x, y);
    if (l >= 0) keyDown_[l] = false;
    return true;
  }
  if (action != PG_TOUCH_DOWN) return true;

  /* 顶部信息条：切曲目 / 切速度 */
  if (y < kFallTop && celebMs_ <= 0) {
    if (x < 240) {
      loadSong(song_ + 1);
      reset();
      state_ = GSTATE_RUNNING;
      logInfo("Piano: 换曲 -> %s", kSongs[song_].name);
    } else {
      diff_ = (diff_ + 1) % music::DIFF_COUNT;
      reset();
      state_ = GSTATE_RUNNING;
      logInfo("Piano: 换速度 -> %s", music::diffName(diff_));
    }
    return true;
  }
  const int l = laneAt(x, y);
  if (l >= 0) {
    keyDown_[l] = true;
    tapLane(l, false);
  }
  return true;
}

bool GamePiano::onKey(int key) {
  if (key == PG_KEY_A) {
    if (state_ == GSTATE_READY) {
      reset();
      state_ = GSTATE_RUNNING;
    }
    return true;
  }
  if (key == PG_KEY_C) {
    reset();
    state_ = GSTATE_RUNNING;
    return true;
  }
  return false;
}

// ---------------- HUD ----------------
const char *GamePiano::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", combo_);
  return buf;
}

const char *GamePiano::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", next_, nNotes_);
  return buf;
}

const char *GamePiano::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "音符落到线时按对应琴键 · 点顶部换曲目/速度";
}

bool GamePiano::justGameOver() { return false; }
void GamePiano::clearGameOverFlag() {}

// ---------------- QA ----------------
void GamePiano::debugCmd(const char *rest) {
  char cmd[24];
  int k = 0;
  while (rest[k] && rest[k] != ' ' && k < 23) {
    cmd[k] = rest[k];
    ++k;
  }
  cmd[k] = 0;
  const char *arg = rest + k;
  while (*arg == ' ') ++arg;

  if (strcmp(cmd, "q") == 0) {
    char line[240];
    int off = snprintf(line, sizeof(line),
                       "qa piano song=%s diff=%d notes=%d next=%d now=%d lead=%d "
                       "score=%d combo=%d max=%d P=%d G=%d O=%d M=%d |",
                       kSongs[song_].name, diff_ + 1, nNotes_, next_, nowMs_,
                       startDelayMs_, score_, combo_, maxCombo_, cnt_[music::J_PERFECT],
                       cnt_[music::J_GOOD], cnt_[music::J_OK], cnt_[music::J_MISS]);
    for (int i = 0; i < nNotes_ && off < 200; ++i) {
      off += snprintf(line + off, sizeof(line) - off, " %d@%d%s", notes_[i].lane,
                      notes_[i].atMs, done_[i] ? "*" : "");
    }
    logInfo("%s", line);
    /* 再打一行**待结算音符的屏幕 y** —— 这是"音符真的在下落"的可脚本判据：
     * 隔一秒问一次 `gdbg q`，同一音符的 y 必须**变大**（见 docs/rhythm-games.md 验收表）。
     * ★ 之所以要它：这类"每帧动的画面"用抓屏逐帧比对很笨，而 y 是**同源**的中间量。 */
    {
      char line2[180];
      int off2 = snprintf(line2, sizeof(line2), "qa piano disp(mtAll=%d):",
                          nowMs_ - kLeadInMs);
      for (int i = next_, shown = 0; i < nNotes_ && shown < 4 && off2 < 150; ++i) {
        if (done_[i]) continue;
        off2 += snprintf(line2 + off2, sizeof(line2) - off2, " #%d lane=%d y=%d", i,
                         notes_[i].lane,
                         noteScreenY(kPianoJudgeY, kPianoNoteH, notes_[i].atMs,
                                     nowMs_ - kLeadInMs, diff_));
        ++shown;
      }
      logInfo("%s", line2);
    }
    return;
  }
  if (strcmp(cmd, "key") == 0) {
    const int l = atoi(arg) - 1;
    if (l < 0 || l >= kLanes) {
      logInfo("qa piano key: 音轨越界（1..8）");
      return;
    }
    keyDown_[l] = true;
    tapLane(l, false);
    keyDown_[l] = false;
    return;
  }
  if (strcmp(cmd, "auto") == 0) {
    /* 自动把接下来 N 个音符"按应击时刻"点对 —— 走的是与触摸**同一条**路径
     * （tapLane）。验收靠它拿"全 Perfect"，比盲点可复现得多。 */
    int n = atoi(arg);
    if (n <= 0) n = 1;
    for (int c = 0; c < n; ++c) {
      while (next_ < nNotes_ && done_[next_]) ++next_;
      if (next_ >= nNotes_) {
        logInfo("qa piano auto: 谱面已走完");
        return;
      }
      const int t = notes_[next_].atMs;
      /* 直接把曲子时间推到应击时刻（等价于"玩家在这个时间点按下"） */
      nowMs_ = kLeadInMs + t;
      tapLane(notes_[next_].lane, true);
      while (next_ < nNotes_ && done_[next_]) ++next_;
    }
    logInfo("qa piano auto %d -> next=%d P=%d G=%d O=%d M=%d score=%d", n, next_,
            cnt_[music::J_PERFECT], cnt_[music::J_GOOD], cnt_[music::J_OK],
            cnt_[music::J_MISS], score_);
    return;
  }
  if (strcmp(cmd, "song") == 0) {
    loadSong(atoi(arg) - 1);
    reset();
    state_ = GSTATE_RUNNING;
    logInfo("qa piano song=%s", kSongs[song_].name);
    return;
  }
  if (strcmp(cmd, "diff") == 0) {
    diff_ = (atoi(arg) - 1) % music::DIFF_COUNT;
    if (diff_ < 0) diff_ = 0;
    reset();
    state_ = GSTATE_RUNNING;
    logInfo("qa piano diff=%s", music::diffName(diff_));
    return;
  }
  logInfo("qa piano: 未知命令（q/key n/auto N/song n/diff n）");
}

}  // namespace pg
