/*
 * PgDrum.cpp - 打鼓（下落式跟打 · 6 鼓位 · 木纹鼓架）
 *
 * 玩法：鼓点从上方沿 6 条轨落下，落到判定线时敲对应的鼓垫（也可以直接在轨道上敲）。
 *   判定三档 + 漏；连击加成；打完整段出准确率结算；打完自动重来。
 *
 * 6 个鼓位（音色在 resources/audio/drm1..6.wav，tools/gensfx.py 合成）：
 *   0 底鼓(k) / 1 军鼓(s) / 2 踩镲(h) / 3 低嗵(t) / 4 高嗵(T) / 5 吊镲(c)
 *
 * 谱面写法：一个 token = 一个八分音符，token 里的字符就是"这一拍要敲的鼓"，
 * 所以 `kh` = 底鼓和踩镲同时响、`.` = 空拍。4 小节循环 —— 见 kCharts。
 *   ★ 用字符串而不是结构体数组：读起来就是鼓谱本身（一眼能看出节奏型），
 *     加一段只要写一行字（同 PgPiano 的理由）。
 *
 * 与节奏钢琴共用内核：core/PgMusic.h。
 * 视觉：木纹实体乐器风；素材见 tools/gen_instr_art.py 的 DRUM 段。
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

using gameart::kDrumBg;
using gameart::kDrumFallH;
using gameart::kDrumLaneW;
using gameart::kDrumNoteH;
using gameart::kDrumNoteW;
using gameart::kDrumPadDX;
using gameart::kDrumPadH;
using gameart::kDrumPadW;
using gameart::kDrumPadX0;
using gameart::kDrumPadY0;

const int kPads = 6;
const int kLeadInMs = 1500;
const int kResultMs = 4200;
const int kFallTop = 46;
const int kJudgeShowMs = 380;
const int kJudgeY = kDrumFallH - 4;

// 鼓位中文名（只用于 QA 日志与结算页）
const char *const kPadNames[6] = {"底鼓", "军鼓", "踩镲", "低嗵", "高嗵", "吊镲"};

struct Chart {
  const char *name;
  int bpm;
  const char *pat;   // 一小节（8 个八分音符 token，空格分隔）
  int bars;          // 重复几小节
};

const Chart kCharts[] = {
    {"基础练习", 88, "k . s . k . s .", 4},
    {"摇滚节奏", 108, "kh h sh h kh h sh h", 4},
    {"迪斯科", 120, "kh h kh h sh h kh h", 4},
    {"鼓点独奏", 126, "ch t sh h Th h th h", 4},
};
const int kChartCount = (int)(sizeof(kCharts) / sizeof(kCharts[0]));

int laneOfChar(char c) {
  switch (c) {
    case 'k': return 0;
    case 's': return 1;
    case 'h': return 2;
    case 't': return 3;
    case 'T': return 4;
    case 'c': return 5;
    default: return -1;
  }
}

const gameart::Def *padDef(int lane) {
  static const gameart::Def *const defs[6] = {
      &gameart::kDrumPad0, &gameart::kDrumPad1, &gameart::kDrumPad2,
      &gameart::kDrumPad3, &gameart::kDrumPad4, &gameart::kDrumPad5};
  return defs[lane % 6];
}

const gameart::Def *noteDef(int lane) {
  static const gameart::Def *const defs[6] = {
      &gameart::kDrumNote0, &gameart::kDrumNote1, &gameart::kDrumNote2,
      &gameart::kDrumNote3, &gameart::kDrumNote4, &gameart::kDrumNote5};
  return defs[lane % 6];
}

const char *judgeText(int j) {
  switch (j) {
    case music::J_PERFECT: return "完美";
    case music::J_GOOD: return "不错";
    case music::J_OK: return "还行";
    default: return "漏了";
  }
}

/* 鼓点的屏幕 y（**唯一的映射实现**，render 与 QA 都调它）。
 *
 * ★★ 血案（2026-09-16 用户报"看不到鼓点下落"）：原写法
 *      const int pxPerMs = (kJudgeY + kDrumNoteH) / music::fallMs(diff_);
 *      y = kJudgeY - (atMs - mtAll) * pxPerMs;
 *    **整数除法把"每毫秒几个像素"算成了 0**（322 / 1250~2600 = 0）
 *    ⇒ 所有鼓点的 y 恒等于判定线 296，且被画在**上层**的鼓垫盖住
 *    ⇒ 表现就是"完全没有下落动画"。与钢琴同一处错误（那个是 416/fallMs）。
 *
 * 正确做法：**先乘后除**（保留亚像素精度）。long long：|dt| 最大约几十秒 ✕ 322 ≈ 1e7。
 * 下落时长走 `music::drumDiff(diff).fallMs`（打鼓专用四档，含「入门」3600ms）。 */
inline int drumNoteScreenY(int judgeY, int noteH, int atMs, int mtAll, int diff) {
  const int fall = music::drumDiff(diff).fallMs;
  if (fall <= 0) return judgeY;   // 防御：fallMs 被改成 0 时不要除零
  return judgeY -
         (int)((long long)(atMs - mtAll) * (judgeY + noteH) / fall);
}

/* 打鼓的速度档名（索引与 music::drumDiff 的 {入门, 慢, 中, 快} 严格对应）。
 * ★ 文字留在本 .cpp 而不是 PgMusic.h：画布字库是按"哪个 .cpp 里出现的字面量"收字形的，
 *   放进两个 .cpp 都包含的共用头会让**档位归属不可控**（见 PgMusic.h 顶部的纪律说明）。 */
const char *kDiffNames[music::DRUM_DIFF_COUNT] = {"入门", "慢", "中", "快"};

const char *drumDiffName(int d) {
  if (d < 0 || d >= music::DRUM_DIFF_COUNT) d = music::DRUM_DIFF_DEFAULT;
  return kDiffNames[d];
}
Color judgeColor(int j) {
  switch (j) {
    case music::J_PERFECT: return rgb(0x6BE39A);
    case music::J_GOOD: return rgb(0xFFD24A);
    case music::J_OK: return rgb(0xFFA24A);
    default: return rgb(0xFF5B4A);
  }
}

/* 鼓垫矩形（**唯一的布局实现**：绘制、命中、QA 都走这里）。
 *
 * ★★ 2026-09-16 改：从"3 列 x 2 行（读序 `(lane%3, lane/3)`）"改成**单排 6 个**，
 *    每个垫子**正对上方那条掉落轨**（`x = kDrumPadX0 + lane * kDrumPadDX`）。
 *    原因：掉落轨是左→右一整条横扫，而原布局按"先上行三格、再下行三格"排垫子，
 *    实测 4/6 条轨的 x 偏差 ≥120px（最大 200px = 屏宽 42%）、L3~L5 还差一整行
 *    ⇒ 玩家得在音符落下的 ~1s 内"查表该敲哪个垫"（唯一的颜色线索"轨头色标"还被
 *    顶部信息条盖住）⇒ 这就是"无法操作"的观感来源。对齐后 = 音符掉到哪个垫子上就敲哪个。
 *    垫宽 74px（缝 5px：3/119/198/277/356/435 对音轨中心 40/120/200/280/360/440，偏差 ≤5px），
 *    仍比真触摸实测可用的钢琴键（60px）宽。 */
void padRect(int lane, int &x, int &y) {
  x = kDrumPadX0 + lane * kDrumPadDX;
  y = kDrumPadY0;
}

}  // namespace

// ---------------- 卡片元信息 ----------------
const char *GameDrum::title() const { return "打鼓"; }
const char *GameDrum::desc() const { return "鼓点落到线时敲对应鼓垫，6 鼓位"; }
const char *GameDrum::tag() const { return "DRUM"; }
Color GameDrum::theme() const { return rgb(0xD9822B); }

// ---------------- 谱面 ----------------
void GameDrum::loadChart(int idx) {
  const int n = kChartCount;
  if (n <= 0) return;
  idx %= n;
  if (idx < 0) idx += n;
  chart_ = idx;

  const Chart &ch = kCharts[idx];
  /* 一个 token = 八分音符 ⇒ 四分音符 = 2 * (60000/bpm)。
   * 速度档位只改"下落快慢"（fallMs），**不改曲速** —— 鼓点变速会让节奏型失真，
   * 而"音符掉得更快"已经足够改变难度。 */
  const int eighthMs = (int)(60000LL / (ch.bpm * 2LL));
  nNotes_ = 0;
  int tok = 0;
  for (int bar = 0; bar < ch.bars && nNotes_ < music::NOTE_MAX; ++bar) {
    const char *p = ch.pat;
    tok = 0;
    while (*p) {
      while (*p == ' ') ++p;
      if (!*p) break;
      int t = (bar * 8 + tok) * eighthMs;
      while (*p && *p != ' ') {
        const int lane = laneOfChar(*p);
        if (lane >= 0 && nNotes_ < music::NOTE_MAX) {
          notes_[nNotes_].lane = lane;
          notes_[nNotes_].atMs = t;
          ++nNotes_;
        }
        ++p;
      }
      ++tok;
    }
  }
  if (nNotes_ >= music::NOTE_MAX) {
    logInfo("Drum: 谱面超过 %d 个音符，已截断（节奏型=%s）", music::NOTE_MAX, ch.name);
  }
  for (int i = 0; i < music::NOTE_MAX; ++i) done_[i] = false;
  logInfo("Drum: 载入鼓谱 %s（bpm=%d 速度=%s 八分=%dms 音符=%d）", ch.name, ch.bpm,
          drumDiffName(diff_), eighthMs, nNotes_);
}

// ---------------- 生命周期 ----------------
void GameDrum::reset() {
  score_ = 0;
  combo_ = 0;
  maxCombo_ = 0;
  next_ = 0;
  nowMs_ = 0;
  startDelayMs_ = kLeadInMs;
  lastJudgeMs_ = 0;
  lastJudgeLane_ = -1;
  celebMs_ = 0;
  overFlag_ = false;
  for (int i = 0; i < 5; ++i) cnt_[i] = 0;
  for (int i = 0; i < kPads; ++i) {
    padFlash_[i] = 0;
    padDown_[i] = false;
  }
  loadChart(chart_);
}

void GameDrum::update(int dtMs) {
  if (dtMs < 0) dtMs = 0;
  if (dtMs > 200) dtMs = 200;
  for (int i = 0; i < kPads; ++i) {
    if (padFlash_[i] > 0) padFlash_[i] -= dtMs;
  }
  if (lastJudgeMs_ > 0) lastJudgeMs_ -= dtMs;

  if (celebMs_ > 0) {
    celebMs_ -= dtMs;
    if (celebMs_ <= 0) {
      reset();
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
  if (nowMs_ < kLeadInMs) return;
  const int mt = nowMs_ - kLeadInMs;

  while (next_ < nNotes_ && done_[next_]) ++next_;
  while (next_ < nNotes_ && mt - notes_[next_].atMs > music::drumDiff(diff_).win.ok) {
    done_[next_] = true;
    ++cnt_[music::J_MISS];
    combo_ = 0;
    lastJudge_ = music::J_MISS;
    lastJudgeMs_ = kJudgeShowMs;
    lastJudgeLane_ = notes_[next_].lane;
    logInfo("Drum: 漏掉音符 #%d（%s at=%dms）", next_, kPadNames[notes_[next_].lane],
            notes_[next_].atMs);
    ++next_;
    while (next_ < nNotes_ && done_[next_]) ++next_;
  }

  if (next_ >= nNotes_) {
    celebMs_ = kResultMs;
    logInfo("Drum: 打完了 —— 完美 %d 不错 %d 还行 %d 漏 %d 最高连击 %d 得分 %d",
            cnt_[music::J_PERFECT], cnt_[music::J_GOOD], cnt_[music::J_OK],
            cnt_[music::J_MISS], maxCombo_, score_);
  }
}

// ---------------- 判定 ----------------
bool GameDrum::hitPad(int lane, bool fromAuto) {
  if (lane < 0 || lane >= kPads) return false;
  sfx(SFX_DRM1 + lane);           // 鼓垫永远出声（自由敲打的手感）
  padFlash_[lane] = 200;
  if (state_ != GSTATE_RUNNING || celebMs_ > 0) return true;
  if (nowMs_ < kLeadInMs) return true;
  const int mt = nowMs_ - kLeadInMs;

  const music::Win &win = music::drumDiff(diff_).win;
  int best = -1, bestDt = win.ok + 1;
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
    logInfo("Drum: 空敲 %s（该鼓位无待结算音符）%s", kPadNames[lane],
            fromAuto ? " [auto]" : "");
    return true;
  }
  const music::Judge j = music::judgeOfWin(bestDt, win);
  done_[best] = true;
  ++cnt_[j];
  if (j == music::J_MISS) {
    combo_ = 0;
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
  logInfo("Drum: 命中 #%d %s dt=%dms -> %s combo=%d score=%d%s", best, kPadNames[lane],
          mt - notes_[best].atMs, judgeText(j), combo_, score_, fromAuto ? " [auto]" : "");
  return true;
}

// ---------------- 绘制 ----------------
void GameDrum::render(Canvas &c) {
  const int W = c.width();
  const int H = c.height();
  blit(c, kDrumBg, 0, 0);

  const int mtAll = nowMs_ - kLeadInMs;
  const bool playing = (state_ == GSTATE_RUNNING && celebMs_ <= 0 && nowMs_ >= kLeadInMs);

  /* ---- 「最佳时机」窗口色带（画在鼓点**之下**）----
   * 高度与 drumNoteScreenY 同一套映射：windowPx = ms * (judgeY + noteH) / fallMs。
   * ⚠️ 打鼓有自己的四档表（含「入门」3600ms），**不能**用 music::fallMs()。 */
  const music::Diff4 &dd = music::drumDiff(diff_);
  const int fall = dd.fallMs;
  const int spanY = kJudgeY + kDrumNoteH;
  int bandP = (fall > 0) ? dd.win.perfect * spanY / fall : 4;
  int bandG = (fall > 0) ? dd.win.good * spanY / fall : 8;
  if (bandP < 4) bandP = 4;
  if (bandG < bandP + 3) bandG = bandP + 3;
  c.blendRect(0, kJudgeY - bandG, W, bandG * 2, rgba(0x8A, 0x6A, 0x2A), 52);
  c.blendRect(0, kJudgeY - bandP, W, bandP * 2, rgba(0xFF, 0xC6, 0x4A), 76);

  /* ---- 掉落中的鼓点 ----
   * y 由 drumNoteScreenY 统一计算（**先乘后除**，别在这里重写算式 —— 见它的血案说明）。 */
  uint32_t hotLane = 0;   // 本帧已进入 Perfect 窗口的鼓位位图（驱动落点标记点亮）
  for (int i = 0; i < nNotes_; ++i) {
    if (done_[i]) continue;
    const int y = drumNoteScreenY(kJudgeY, kDrumNoteH, notes_[i].atMs, mtAll, diff_);
    if (playing) {
      const int dt = notes_[i].atMs - mtAll;
      if (dt >= -dd.win.perfect && dt <= dd.win.perfect) hotLane |= (1u << notes_[i].lane);
    }
    if (y < kFallTop) continue;
    if (y > kJudgeY + kDrumNoteH) continue;
    blit(c, *noteDef(notes_[i].lane),
         notes_[i].lane * kDrumLaneW + (kDrumLaneW - kDrumNoteW) / 2, y);
  }

  /* ---- 判定线（加粗提亮）+ 每轨落点标记（进入 Perfect 窗口时变亮放大）---- */
  c.fillRect(0, kJudgeY - 1, W, 3, rgba(0xFF, 0xF0, 0xC0));
  c.blendRect(0, kJudgeY + 2, W, 3, rgba(0x50, 0x38, 0x18), 120);
  for (int i = 0; i < 6; ++i) {
    const int cx = i * kDrumLaneW + kDrumLaneW / 2;
    if ((hotLane >> i) & 1) {
      c.fillRect(cx - 13, kJudgeY - 5, 26, 11, rgba(0xFF, 0xE2, 0x8A));
    } else {
      c.fillRect(cx - 10, kJudgeY - 2, 20, 5, rgba(0xD8, 0xBE, 0x8C));
    }
  }

  /* ---- 鼓垫（**单排 6 个，各自正对上方那条掉落轨**；按下态：下沉 3px；命中时叠主题色高光）----
   * 位置全部由 padRect 给（与命中判定、QA 同源），别在这里硬编坐标。 */
  for (int i = 0; i < kPads; ++i) {
    int px = 0, py = 0;
    padRect(i, px, py);
    blit(c, *padDef(i), px, py + (padDown_[i] ? 3 : 0));
    if (padFlash_[i] > 0) {
      const int a = padFlash_[i] * 120 / 200;
      /* 高光跟着"鼓皮正圆"走（与 bake_drum_pad 同一套几何：直径 = 垫宽 - 14、居中）——
       * 垫子现在是 74px 窄垫，再画满垫的方块会连木框一起盖住，看着像贴了张纸。 */
      const int dia = kDrumPadW - 14;
      c.blendRect(px + (kDrumPadW - dia) / 2, py + (kDrumPadH - dia) / 2 +
                      (padDown_[i] ? 3 : 0),
                  dia, dia, judgeColor(playing ? music::J_PERFECT : music::J_OK), a);
    }
    /* 鼓位名：**居中贴在垫子正下方**（不再塞进垫子内部）。
     * ① 垫子改 74px 窄垫后，垫内的色环把位置占掉了；
     * ② 名字排在"它正上方那条掉落轨"下面，本身就是"该敲哪个"的文字提示；
     * ③ 保持 scale=1（**别改成变量或换档位** —— 那会让画布字库多出一档字形，见 RULES-DETAIL §4）。 */
    c.textCenter(px + kDrumPadW / 2, py + kDrumPadH + 6, kPadNames[i], 1,
                 rgb(0xC9B79A));
  }

  /* ---- 顶部两条信息条 ---- */
  c.fillRectRound(8, 8, 214, 32, 8, rgba(24, 18, 10, 200));
  c.text(18, 11, "节奏型", 2, rgb(0xC9B79A));
  c.text(18 + c.textW("节奏型", 2) + 8, 11, kCharts[chart_].name, 2, rgb(0xF6EFE2));
  c.fillRectRound(258, 8, 214, 32, 8, rgba(24, 18, 10, 200));
  c.text(268, 11, "速度", 2, rgb(0xC9B79A));
  c.text(268 + c.textW("速度", 2) + 8, 11, drumDiffName(diff_), 2, rgb(0xFFD24A));

  /* ---- 判定文字 + 连击 ---- */
  if (lastJudgeMs_ > 0 && playing) {
    c.blendRect(0, kJudgeY - 108, W, 44, rgba(0, 0, 0), lastJudgeMs_ * 90 / 380);
    c.textCenter(W / 2, kJudgeY - 104, judgeText(lastJudge_), 3,
                judgeColor(lastJudge_));
  }
  if (playing && combo_ >= 2) {
    const int w = c.numberW(combo_, 4);
    c.number(W / 2 - w / 2, kJudgeY - 168, combo_, 4, rgb(0xFFE9A8));
    c.textCenter(W / 2, kJudgeY - 216, "连击", 2, rgb(0xC9B79A));
  }

  /* ---- 覆盖层 ---- */
  if (state_ == GSTATE_READY) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 145);
    c.textCenter(W / 2, H / 2 - 120, "打鼓", 5, rgb(0xFFD8A8));
    c.textCenter(W / 2, H / 2 - 34, "鼓点落到线时敲鼓垫", 2, rgb(0xF2F2F7));
    c.textCenter(W / 2, H / 2 + 6, "敲空也不扣分", 2, rgb(0xC7C7CC));
    c.textCenter(W / 2, H / 2 + 48, "点屏幕开始", 2, rgb(0xFFD24A));
  } else if (celebMs_ > 0) {
    c.blendRect(0, 0, W, H, rgba(0, 0, 0), 150);
    const int acc = music::accuracyPermille(cnt_[music::J_PERFECT], cnt_[music::J_GOOD],
                                           cnt_[music::J_OK], cnt_[music::J_MISS]);
    c.textCenter(W / 2, H / 2 - 148, "打完一段", 4, rgb(0xFFD8A8));
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
    int wC = c.textW("漏", 2), wD = c.numberW(cnt_[music::J_MISS], 2);
    x = (W - (wA + wB + wC + wD + 36)) / 2;
    c.text(x, y1, "完美", 2, rgb(0x6BE39A));
    x += wA + 6;
    c.number(x, y1, cnt_[music::J_PERFECT], 2, rgb(0xF2F2F7));
    x += wB + 30;
    c.text(x, y1, "漏", 2, rgb(0xFF5B4A));
    x += wC + 6;
    c.number(x, y1, cnt_[music::J_MISS], 2, rgb(0xF2F2F7));

    const int y3 = y1 + 42;
    const int wL = c.textW("最高连击", 2);
    const int wN = c.numberW(maxCombo_, 2);
    x = (W - (wL + wN + 8)) / 2;
    c.text(x, y3, "最高连击", 2, rgb(0xC9B79A));
    c.number(x + wL + 8, y3, maxCombo_, 2, rgb(0xFFD24A));
  }
}

// ---------------- 输入 ----------------
int GameDrum::padAt(int x, int y) const {
  /* 鼓垫与掉落轨**已经对齐**（同一条列的 x 范围：垫 3..472 ⊂ 轨 0..480），
   * 所以命中不再需要"逐个矩形比对"，**按列判**就是对的，而且顺带得到两个好处：
   *   ① 手指落在"轨"上（不必精确压到鼓垫）也算 —— 对齐之后这本来就该算；
   *   ② 列 ↔ 垫 是同一个下标，永远不会再出现"点 A 判成 B"。
   * y 的两端：判定线稍上方起（kJudgeY-6，手指压线也算），到垫子下方的名牌带为止；
   * **舞台上方的空中区域一律不判**（否则"打空中的音符"会变成误触）。
   * 行的 y 仍从 padRect 取 —— 几何只有一份实现。 */
  int px = 0, py = 0;
  padRect(0, px, py);
  if (y < kJudgeY - 6 || y >= py + kDrumPadH + 12) return -1;
  const int lane = x / kDrumLaneW;
  if (lane < 0 || lane >= kPads) return -1;
  return lane;
}

bool GameDrum::onTouch(int action, int x, int y) {
  if (state_ == GSTATE_READY) {
    if (action == PG_TOUCH_DOWN) {
      reset();
      state_ = GSTATE_RUNNING;
      sfx(SFX_CLICK);
    }
    return true;
  }
  if (action == PG_TOUCH_UP) {
    const int p = padAt(x, y);
    if (p >= 0) padDown_[p] = false;
    return true;
  }
  if (action != PG_TOUCH_DOWN) return true;

  if (y < kFallTop && celebMs_ <= 0) {
    if (x < 240) {
      loadChart(chart_ + 1);
      reset();
      state_ = GSTATE_RUNNING;
      logInfo("Drum: 换节奏型 -> %s", kCharts[chart_].name);
    } else {
      diff_ = (diff_ + 1) % music::DRUM_DIFF_COUNT;
      reset();
      state_ = GSTATE_RUNNING;
      logInfo("Drum: 换速度 -> %s", drumDiffName(diff_));
    }
    return true;
  }
  const int p = padAt(x, y);
  if (p >= 0) {
    padDown_[p] = true;
    hitPad(p, false);
  }
  return true;
}

bool GameDrum::onKey(int key) {
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
const char *GameDrum::info1Value(char *buf, int n) const {
  snprintf(buf, n, "%d", combo_);
  return buf;
}

const char *GameDrum::info2Value(char *buf, int n) const {
  snprintf(buf, n, "%d/%d", next_, nNotes_);
  return buf;
}

const char *GameDrum::hint() const {
  if (state_ == GSTATE_READY) return "点击屏幕或按暂停键开始";
  if (state_ == GSTATE_PAUSED) return "已暂停 - 按暂停键继续";
  return "鼓点落到线时敲鼓垫 · 点顶部换节奏型/速度";
}

bool GameDrum::justGameOver() { return false; }
void GameDrum::clearGameOverFlag() {}

// ---------------- QA ----------------
void GameDrum::debugCmd(const char *rest) {
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
                       "qa drum chart=%s diff=%d notes=%d next=%d now=%d score=%d "
                       "combo=%d max=%d P=%d G=%d O=%d M=%d |",
                       kCharts[chart_].name, diff_ + 1, nNotes_, next_, nowMs_, score_,
                       combo_, maxCombo_, cnt_[music::J_PERFECT], cnt_[music::J_GOOD],
                       cnt_[music::J_OK], cnt_[music::J_MISS]);
    for (int i = 0; i < nNotes_ && off < 200; ++i) {
      off += snprintf(line + off, sizeof(line) - off, " %d@%d%s", notes_[i].lane,
                      notes_[i].atMs, done_[i] ? "*" : "");
    }
    logInfo("%s", line);
    /* 再打一行**待结算鼓点的屏幕 y** —— "鼓点真的在下落"的可脚本判据
     * （隔一秒问一次 `gdbg q`，同一鼓点的 y 必须**变大**）。 */
    {
      char line2[180];
      int off2 = snprintf(line2, sizeof(line2), "qa drum disp(mtAll=%d):",
                          nowMs_ - kLeadInMs);
      for (int i = next_, shown = 0; i < nNotes_ && shown < 4 && off2 < 150; ++i) {
        if (done_[i]) continue;
        off2 += snprintf(line2 + off2, sizeof(line2) - off2, " #%d pad=%d y=%d", i,
                         notes_[i].lane,
                         drumNoteScreenY(kJudgeY, kDrumNoteH, notes_[i].atMs,
                                         nowMs_ - kLeadInMs, diff_));
        ++shown;
      }
      logInfo("%s", line2);
    }
    return;
  }
  if (strcmp(cmd, "pad") == 0) {
    const int p = atoi(arg) - 1;
    if (p < 0 || p >= kPads) {
      logInfo("qa drum pad: 鼓位越界（1..6）");
      return;
    }
    padDown_[p] = true;
    hitPad(p, false);
    padDown_[p] = false;
    return;
  }
  if (strcmp(cmd, "auto") == 0) {
    int n = atoi(arg);
    if (n <= 0) n = 1;
    for (int c = 0; c < n; ++c) {
      while (next_ < nNotes_ && done_[next_]) ++next_;
      if (next_ >= nNotes_) {
        logInfo("qa drum auto: 谱面已走完");
        return;
      }
      nowMs_ = kLeadInMs + notes_[next_].atMs;
      hitPad(notes_[next_].lane, true);
      while (next_ < nNotes_ && done_[next_]) ++next_;
    }
    logInfo("qa drum auto %d -> next=%d P=%d G=%d O=%d M=%d score=%d", n, next_,
            cnt_[music::J_PERFECT], cnt_[music::J_GOOD], cnt_[music::J_OK],
            cnt_[music::J_MISS], score_);
    return;
  }
  if (strcmp(cmd, "chart") == 0) {
    loadChart(atoi(arg) - 1);
    reset();
    state_ = GSTATE_RUNNING;
    logInfo("qa drum chart=%s", kCharts[chart_].name);
    return;
  }
  if (strcmp(cmd, "diff") == 0) {
    diff_ = (atoi(arg) - 1) % music::DRUM_DIFF_COUNT;
    if (diff_ < 0) diff_ = 0;
    reset();
    state_ = GSTATE_RUNNING;
    logInfo("qa drum diff=%s", drumDiffName(diff_));
    return;
  }
  logInfo("qa drum: 未知命令（q/pad n/auto N/chart n/diff n）");
}

}  // namespace pg
