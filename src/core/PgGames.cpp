/*
 * PgGames.cpp - 应用注册表（游戏 + 工具 + 系统页）
 *
 * ★ 新增一个应用：写一个 pg::Game 子类，然后在下面 kAppTable 里加一行即可。
 *   主界面列表（按分类分页）、最高分存储、HUD 都会自动适配。
 *
 * ⚠️ 表里的 **slot 必须稳定**：最高分/设置按 slot 存盘，中间插一行会让老存档错位，
 *    所以新应用一律**追加在表尾**并沿用递增的 slot。
 */
#include "PgGames.h"
#include "PgRemote.h"
#include "PgProbe.h"
#include "PgRadio.h"
#include "PgCam.h"

#include <stdlib.h>
#include <time.h>

namespace pg {

namespace {

Game *create2048() { return new Game2048(); }
Game *createTetris() { return new GameTetris(); }
Game *createPlane() { return new GamePlane(); }
Game *createFlappy() { return new GameFlappy(); }
Game *createSnake() { return new GameSnake(); }
Game *createMines() { return new GameMines(); }
Game *createSokoban() { return new GameSokoban(); }
Game *createBreakout() { return new GameBreakout(); }
Game *createPomodoro() { return new ToolPomodoro(); }
Game *createTimer() { return new ToolTimer(); }
Game *createStopwatch() { return new ToolStopwatch(); }
Game *createCalc() { return new ToolCalc(); }
Game *createWifi() { return new AppWifi(); }
Game *createRemote() { return new AppRemote(); }
// ---- 2026-09-13 新增：适合触摸屏的 4 款 ----
Game *createWhack() { return new GameWhack(); }      // 打地鼠（触摸专属·反应类）
Game *createMemory() { return new GameMemory(); }    // 记忆翻牌（纯几何图案）
Game *createGomoku() { return new GameGomoku(); }    // 五子棋（点一下落子）
Game *createSlide() { return new GameSlide(); }      // 数字华容道（点相邻块滑动）
Game *createReact() { return new GameReact(); }             // 反应计时（原生控件大字）
Game *createClockSuite() { return new GameClockSuite(); }   // 时钟套件（世界时钟 + 闹钟）
Game *createIptv() { return new GameIptv(); }               // 网络电视（HLS/IPTV）
Game *createProbe() { return new AppProbe(); }              // 信号探针（WiFi + 蓝牙）
/* 2026-09-15 新增：消消乐（糖果三消）。参考产品 UI =《开心消消乐》，
 * 素材在 tools/gen_game_art.py 的 match3 段，实现见 PgMatch3.cpp / docs/match3.md。 */
Game *createMatch3() { return new GameMatch3(); }
/* 2026-09-15 新增：摇骰子（3 颗骰子 · 离线 3D 烘帧 · 新音效 dice.wav）。
 * 素材在 tools/gen_game_art.py 的 dice 段，实现见 PgDice.cpp / docs/dice-game.md。 */
Game *createDice() { return new GameDice(); }
/* 2026-09-16 新增：儿童益智三件套（面向 5-10 岁 · 零素材 · 共用「蜡笔童趣风」PgKids.h）。
 * 实现见 PgConnect.cpp / PgBubble.cpp / PgSpot.cpp，说明见 docs/kids-games.md。 */
Game *createConnect() { return new GameConnect(); }  // 数字连线画（数序 + 点数）
Game *createBubble() { return new GameBubble(); }    // 算术泡泡（心算 + 数感）
Game *createSpot() { return new GameSpot(); }        // 找不同（观察力 + 专注）
/* 2026-09-16 新增：木纹实体乐器风三件套（slot 27~29）。素材由
 * tools/gen_instr_art.py 出（被 tools/gen_game_art.py 调用），音色在
 * resources/audio/pno1..8.wav + drm1..6.wav；说明见 docs/wood-instruments.md。 */
Game *createSudoku() { return new GameSudoku(); }    // 数独（9x9 · 唯一解 · 三档）
Game *createPiano() { return new GamePiano(); }      // 节奏钢琴（下落式跟弹 · 8 键）
Game *createDrum() { return new GameDrum(); }        // 打鼓（下落式跟打 · 6 鼓位）
/* 2026-09-16 新增：两个 FlyThings 原生 UI 工具应用（slot 30~31）。
 * ⚠️ 它们的界面**不在画布上**，是独立 ftu（ui/radio.html / ui/camera.html），
 *    这两行只是为了主界面卡片能取到元信息 —— 与 probe/wifi/remote 同一套路。 */
Game *createRadio() { return new AppRadio(); }       // 网络收音机（在线电台）
Game *createCamera() { return new AppCam(); }        // 摄像头查看（局域网 RTSP）
Game *createSettings() { return new AppSettings(); }  // 系统设置（2026-09-16）
/* 2026-09-23 新增：智能家居（slot 37；界面/逻辑全在独立 ftu，本行只给卡片元信息）。 */
Game *createHa() { return new AppHa(); }               // 智能家居（Home Assistant 遥控器）

// slot = 存档索引，必须稳定；顺序 = 分类内卡片顺序
const GameEntry kAppTable[] = {
    {"2048", create2048, APP_GAME, 0},
    {"tetris", createTetris, APP_GAME, 1},
    {"plane", createPlane, APP_GAME, 2},
    {"flappy", createFlappy, APP_GAME, 3},
    {"snake", createSnake, APP_GAME, 4},
    {"mines", createMines, APP_GAME, 5},
    {"sokoban", createSokoban, APP_GAME, 6},
    {"breakout", createBreakout, APP_GAME, 7},
    {"pomodoro", createPomodoro, APP_TOOL, 8},
    {"timer", createTimer, APP_TOOL, 9},
    {"stopwatch", createStopwatch, APP_TOOL, 10},
    {"calc", createCalc, APP_TOOL, 11},
    {"wifi", createWifi, APP_SYSTEM, 12},
    /* ⚠️ 新应用**一律追加到表尾**：startGame/QA 命令用的是**数组下标**，
     *    插在中间会让后面所有应用的下标漂移（踩过：remote 插到 wifi 前面，
     *    结果 gameEntry(13) 变成了 wifi 页）。
     *    表里的 slot 字段才是"稳定存档索引"，两者是不同概念。 */
    {"remote", createRemote, APP_TOOL, 13},
    /* 2026-09-13 新增的 4 款触摸优先游戏（slot 14~17）。
     * ⚠️ 仍按规矩**追加在表尾**：数组下标 = 分类内卡片顺序 = QA 命令用的序号，
     *    插在中间会让已有应用的下标漂移（见上面那段注释的教训）。 */
    {"whack", createWhack, APP_GAME, 14},    // 打地鼠
    {"memory", createMemory, APP_GAME, 15},  // 记忆翻牌
    {"gomoku", createGomoku, APP_GAME, 16},  // 五子棋
    {"slide", createSlide, APP_GAME, 17},    // 数字华容道
    {"react", createReact, APP_GAME, 18},    // 反应计时（原生控件页）
    {"clocksuite", createClockSuite, APP_TOOL, 19},  // 时钟套件（世界时钟 + 闹钟）
    {"iptv", createIptv, APP_TOOL, 20},              // 网络电视（HLS/IPTV 直播）
    /* 2026-09-15 新增：信号探针（从 WiFi 应用里的二级页提升为独立应用）。
     * 界面在 ui/probe.html -> probe.ftu -> probeActivity，逻辑在 probeLogic.cc；
     * 本行只是为了卡片元信息（title/desc/tag/theme），点进去走 openActivity。 */
    {"probe", createProbe, APP_TOOL, 21},             // 信号探针（WiFi 探测 + 蓝牙探测 + 热点猎手）
    /* 2026-09-15 新增：消消乐（糖果三消）。仍是**追加在表尾**（slot 22）——
     * 上面那段"插中间会让下标漂移"的教训一次都没忘。 */
    {"match3", createMatch3, APP_GAME, 22},           // 消消乐（糖果三消 + 关卡目标）
    /* 2026-09-15 新增：摇骰子。**仍然是追加在表尾**（slot 23）——
     * 上面那段"插中间会让下标漂移"的教训继续有效。 */
    {"dice", createDice, APP_GAME, 23},               // 摇骰子（3 颗骰子 · 3D 翻滚 + 音效）
    /* 2026-09-16 新增：儿童益智三件套（slot 24~26）。**仍然是追加在表尾** ——
     * 上面那条"插中间会让已有应用下标漂移"的教训继续有效。
     * ⚠️ 加完必须确认两件事：① 图标控件数已从 24 提到 27（main.html / gen_icons.py /
     *    mainLogic.cc 的 kIconCount / gen_ui.py 的 HIDDEN_CONTROLS，共四处）；
     * ② `ScoreStore::MAX_GAMES`(=32) 要 > 26，否则这三款的最高分会被**静默丢弃**。 */
    {"connect", createConnect, APP_GAME, 24},         // 数字连线画（数序 · 5-8 岁）
    {"bubble", createBubble, APP_GAME, 25},           // 算术泡泡（心算 · 5-9 岁）
    {"spot", createSpot, APP_GAME, 26},               // 找不同（观察力 · 5-10 岁）
    /* 2026-09-16 新增：数独 / 节奏钢琴 / 打鼓（slot 27~29）+ 网络收音机 / 摄像头查看
     * （slot 30~31）。**仍然是追加在表尾** —— 那条"插中间会让已有应用下标漂移"的
     * 教训继续有效。本次共 5 个应用 ⇒ 图标四处都要从 27 提到 **32**：
     *   ui/main.html 的 Icon27..Icon31 / tools/gen_icons.py 的 ICONS 五条 /
     *   mainLogic.cc 的 kIconCount=32 + onButtonClick_Icon27..31 /
     *   tools/gen_ui.py 的 HIDDEN_CONTROLS range(32)。
     * ⚠️ `ScoreStore::MAX_GAMES` 本次已从 32 提到 **40**（slot 已到 31，正好贴上限 ——
     *    再加应用必须同时再提一次，否则最高分会被**静默丢弃**）。 */
    {"sudoku", createSudoku, APP_GAME, 27},           // 数独
    {"piano", createPiano, APP_GAME, 28},             // 节奏钢琴
    {"drum", createDrum, APP_GAME, 29},               // 打鼓
    {"radio", createRadio, APP_TOOL, 30},             // 网络收音机（独立 ftu）
    {"camera", createCamera, APP_TOOL, 31},           // 摄像头查看（独立 ftu）
    {"settings", createSettings, APP_SYSTEM, 32},     // 系统设置（独立 ftu；slot 追加表尾）
    /* 2026-09-23 新增：智能家居（Home Assistant 遥控器）。**仍然是追加在表尾**（slot 37）——
     * 上面那条"插中间会让已有应用下标漂移"的教训继续有效。
     * ⚠️ 加完必须确认三件事：
     *   ① 图标控件数已从 37 提到 **38**（main.html / gen_icons.py / mainLogic.cc 的 kIconCount /
     *      gen_ui.py 的 HIDDEN_CONTROLS，共四处）；
     *   ② `ScoreStore::MAX_GAMES`(=40) > 37 ⇒ 够用（本页 showBest=false，不写最高分）；
     *   ③ noCanvas 页不占 res 空间（界面是原生控件，没有大贴图）⇒ 包体影响很小，
     *      但仍要看 `out/update.img` ≤ mtd3 的 7,995,392 字节。
     * 规划/实测：docs/ha-integration-plan.md（§11 = P0 真机实测与修订）。 */
    {"ha", createHa, APP_SYSTEM, 37},                  // 智能家居（HA 遥控器 · 独立 ftu）
};

const int kAppCount = (int)(sizeof(kAppTable) / sizeof(kAppTable[0]));

const char *kCategoryNames[APP_CATEGORY_COUNT] = {"游戏", "工具", "系统"};

}  // namespace

int appCount() { return kAppCount; }

const GameEntry &appEntry(int index) {
  /* ⚠️ 参数是**表下标**。越界夹紧保证"永不返回野引用"，
   *   但它会**掩盖**"拿 slot 当下标"这类错误（slot 37 会被夹成 33 ⇒ 正好指到智能家居，
   *   看上去对、实则巧合，下一个应用就会指错行）⇒ 按 slot 查一律走 appBySlot()。 */
  if (index < 0) index = 0;
  if (index >= kAppCount) index = kAppCount - 1;
  return kAppTable[index];
}

int appIndexForSlot(int slot) {
  for (int i = 0; i < kAppCount; ++i) {
    if (kAppTable[i].slot == slot) return i;
  }
  return -1;
}

const GameEntry *appBySlot(int slot) {
  const int i = appIndexForSlot(slot);
  return (i < 0) ? 0 : &kAppTable[i];
}

int maxAppSlot() {
  int m = -1;
  for (int i = 0; i < kAppCount; ++i) {
    if (kAppTable[i].slot > m) m = kAppTable[i].slot;
  }
  return m;
}

int categoryCount(int category) {
  int n = 0;
  for (int i = 0; i < kAppCount; ++i) {
    if (kAppTable[i].category == category) ++n;
  }
  return n;
}

int categorySlot(int category, int k) {
  if (k < 0) return -1;
  int n = 0;
  for (int i = 0; i < kAppCount; ++i) {
    if (kAppTable[i].category != category) continue;
    if (n == k) return kAppTable[i].slot;
    ++n;
  }
  return -1;
}

const char *categoryName(int category) {
  if (category < 0 || category >= APP_CATEGORY_COUNT) return "";
  return kCategoryNames[category];
}

int gameCount() { return appCount(); }

const GameEntry &gameEntry(int index) { return appEntry(index); }

void seedRandom() {
  static bool inited = false;
  if (inited) return;
  inited = true;
  srand((unsigned)time(0));
}

}  // namespace pg
