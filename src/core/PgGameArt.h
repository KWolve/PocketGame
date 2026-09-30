/*
 * PgGameArt.h - 游戏素材**清单**（由 tools/gen_game_art.py 生成，勿手改）
 *
 * 这里只有「路径 + 尺寸 + 锚点」，**没有像素数据** —— 像素在
 * resources/images/game/<游戏>/*.png 里，运行时由 pg::sprites().get()
 * 解码加载（见 core/PgSprite.h）。所以：**改图只要覆盖 PNG**（尺寸不变时
 * 连这个头都不用重生成），不用重编、不用重新刷机。
 *
 * 用法：
 *   const Canvas::Sprite *s = sprites().get(kWhackMole.path);
 *   if (s) c.drawSprite(*s, px - kWhackMole.ax, py - kWhackMole.ay);
 *        // 锚点 = 图内的参考点（洞中心 / 地鼠圆心 / 锤子握把…）
 *
 * ⚠️ 尺寸必须与素材严格一致（Canvas 是 1:1 贴图，不做缩放）——
 *    check() 里对不上会打日志，QA `spr` 也能看到实际解出来的尺寸。
 */
#ifndef PG_GAME_ART_H_
#define PG_GAME_ART_H_

namespace pg {
namespace gameart {

struct Def {
  const char *path;   // 资源相对路径
  int w, h;           // 素材尺寸（== 屏幕上贴的尺寸，1:1）
  int ax, ay;         // 锚点：图内的参考点坐标
};

// ---- whack ----
// 草地 + 9 个洞口（静态整屏，合成版）
const Def kWhackBg = {"images/game/whack/bg.png", 480, 540, 0, 0};
// 洞口前沿（只保留下半，用来盖住地鼠下半身）
const Def kWhackHoleFront = {"images/game/whack/hole_front.png", 156, 76, 78, 38};
// 地鼠（锚点=圆心）
const Def kWhackMole = {"images/game/whack/mole.png", 96, 88, 48, 46};
// 被击中的地鼠（亮）
const Def kWhackMoleHit = {"images/game/whack/mole_hit.png", 96, 88, 48, 46};
// 锤子 up 帧（**锚点=锤头中心**：贴图时锤头落在触点上）
const Def kWhackHammerUp = {"images/game/whack/hammer_up.png", 120, 120, 63, 37};
// 锤子 mid 帧（**锚点=锤头中心**：贴图时锤头落在触点上）
const Def kWhackHammerMid = {"images/game/whack/hammer_mid.png", 120, 120, 59, 30};
// 锤子 hit 帧（**锚点=锤头中心**：贴图时锤头落在触点上）
const Def kWhackHammerHit = {"images/game/whack/hammer_hit.png", 120, 120, 54, 39};
// 连击放射线（普通）
const Def kWhackRays = {"images/game/whack/rays.png", 176, 176, 88, 88};
// 连击放射线（高阶）
const Def kWhackRaysHot = {"images/game/whack/rays_hot.png", 176, 176, 88, 88};
// ---- snake ----
// 场地底图（底色+外框+场地+细网格，静态整屏）
const Def kSnakeBg = {"images/game/snake/bg.png", 480, 540, 0, 0};
// ---- flappy ----
// 天空渐变+地面（静态整屏；云与滚动条纹仍是代码绘制）
const Def kFlappyBg = {"images/game/flappy/bg.png", 480, 540, 0, 0};
// ---- match3 ----
// 花园渐变 + 木质棋盘 + 8x8 浅色糖果凹槽（静态整屏，每帧只贴一次）
const Def kMatch3Bg = {"images/game/match3/bg.png", 480, 540, 0, 0};
// 糖果 0：circle（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy0 = {"images/game/match3/candy0.png", 56, 56, 0, 0};
// 糖果 1：square（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy1 = {"images/game/match3/candy1.png", 56, 56, 0, 0};
// 糖果 2：star（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy2 = {"images/game/match3/candy2.png", 56, 56, 0, 0};
// 糖果 3：hex（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy3 = {"images/game/match3/candy3.png", 56, 56, 0, 0};
// 糖果 4：diamond（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy4 = {"images/game/match3/candy4.png", 56, 56, 0, 0};
// 糖果 5：heart（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy5 = {"images/game/match3/candy5.png", 56, 56, 0, 0};
// 糖果 6：stripe（**含凹槽底的不透明整图** ⇒ memcpy 快路径；锚点=左上角，图尺寸 == 格尺寸）
const Def kMatch3Candy6 = {"images/game/match3/candy6.png", 56, 56, 0, 0};
// 选中高亮框（金色圆角框 + 四角加粗）
const Def kMatch3Sel = {"images/game/match3/sel.png", 56, 56, 0, 0};
// 消除爆花（锚点=中心；靠整体 alpha 淡出）
const Def kMatch3Burst = {"images/game/match3/burst.png", 112, 112, 56, 56};
// 彩虹球（5 连生成；**含凹槽底的不透明整图** ⇒ memcpy 快路径）
const Def kMatch3Rainbow = {"images/game/match3/rainbow.png", 56, 56, 0, 0};
// 横向炸弹的**方向标记**（叠在糖果上；只烘 2 张 —— 标记是中性色，不必按糖果色铺 14 张）
const Def kMatch3MarkH = {"images/game/match3/mark_h.png", 56, 56, 0, 0};
// 纵向炸弹的**方向标记**（叠在糖果上；只烘 2 张 —— 标记是中性色，不必按糖果色铺 14 张）
const Def kMatch3MarkV = {"images/game/match3/mark_v.png", 56, 56, 0, 0};
// ---- dice ----
// 绿毡桌面 + 木框 + 暗角 + 结果板（静态整屏，每帧只贴一次；板上的和值/评语由运行时写字）
const Def kDiceBg = {"images/game/dice/bg.png", 480, 540, 0, 0};
// 静止帧：值为 1 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace1 = {"images/game/dice/face1.png", 152, 152, 76, 76};
// 静止帧：值为 2 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace2 = {"images/game/dice/face2.png", 152, 152, 76, 76};
// 静止帧：值为 3 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace3 = {"images/game/dice/face3.png", 152, 152, 76, 76};
// 静止帧：值为 4 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace4 = {"images/game/dice/face4.png", 152, 152, 76, 76};
// 静止帧：值为 5 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace5 = {"images/game/dice/face5.png", 152, 152, 76, 76};
// 静止帧：值为 6 的面朝上（锚点=图中心=立方体中心投影）
const Def kDiceFace6 = {"images/game/dice/face6.png", 152, 152, 76, 76};
// 翻滚第 0 帧（绕斜轴转 0°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll0 = {"images/game/dice/roll0.png", 152, 152, 76, 76};
// 翻滚第 1 帧（绕斜轴转 30°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll1 = {"images/game/dice/roll1.png", 152, 152, 76, 76};
// 翻滚第 2 帧（绕斜轴转 60°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll2 = {"images/game/dice/roll2.png", 152, 152, 76, 76};
// 翻滚第 3 帧（绕斜轴转 90°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll3 = {"images/game/dice/roll3.png", 152, 152, 76, 76};
// 翻滚第 4 帧（绕斜轴转 120°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll4 = {"images/game/dice/roll4.png", 152, 152, 76, 76};
// 翻滚第 5 帧（绕斜轴转 150°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll5 = {"images/game/dice/roll5.png", 152, 152, 76, 76};
// 翻滚第 6 帧（绕斜轴转 180°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll6 = {"images/game/dice/roll6.png", 152, 152, 76, 76};
// 翻滚第 7 帧（绕斜轴转 210°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll7 = {"images/game/dice/roll7.png", 152, 152, 76, 76};
// 翻滚第 8 帧（绕斜轴转 240°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll8 = {"images/game/dice/roll8.png", 152, 152, 76, 76};
// 翻滚第 9 帧（绕斜轴转 270°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll9 = {"images/game/dice/roll9.png", 152, 152, 76, 76};
// 翻滚第 10 帧（绕斜轴转 300°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll10 = {"images/game/dice/roll10.png", 152, 152, 76, 76};
// 翻滚第 11 帧（绕斜轴转 330°；12 帧转满一圈 ⇒ 可无缝循环）
const Def kDiceRoll11 = {"images/game/dice/roll11.png", 152, 152, 76, 76};
// 接触阴影（半透明椭圆；贴图走逐像素 alpha 混合）
const Def kDiceShadow = {"images/game/dice/shadow.png", 116, 40, 58, 20};
// ---- sudoku ----
// 木纹棋盘（81 格凹槽 + 3x3 宫线）+ 平的琴托色区（静态整屏，整图不透明）
const Def kSudokuBg = {"images/game/sudoku/bg.png", 480, 540, 0, 0};
// 数字键（不透明，底烘琴托色；9 个键共用同一张，数字由画布字库写）
const Def kSudokuNumBtn = {"images/game/sudoku/num_btn.png", 46, 46, 0, 0};
// 动作键（擦除/笔记/难度/提示/新局，5 个共用同一张）
const Def kSudokuActBtn = {"images/game/sudoku/act_btn.png", 88, 58, 0, 0};
// ---- piano ----
// 木纹舞台 + 8 条掉落轨 + 判定线 + 平的琴托色区（静态整屏，整图不透明）
const Def kPianoBg = {"images/game/piano/bg.png", 480, 540, 0, 0};
// 第 0 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey0 = {"images/game/piano/key_0.png", 60, 144, 0, 0};
// 第 1 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey1 = {"images/game/piano/key_1.png", 60, 144, 0, 0};
// 第 2 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey2 = {"images/game/piano/key_2.png", 60, 144, 0, 0};
// 第 3 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey3 = {"images/game/piano/key_3.png", 60, 144, 0, 0};
// 第 4 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey4 = {"images/game/piano/key_4.png", 60, 144, 0, 0};
// 第 5 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey5 = {"images/game/piano/key_5.png", 60, 144, 0, 0};
// 第 6 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey6 = {"images/game/piano/key_6.png", 60, 144, 0, 0};
// 第 7 号琴键（不透明，底烘琴托色；底部彩帽 = 音轨色）
const Def kPianoKey7 = {"images/game/piano/key_7.png", 60, 144, 0, 0};
// 第 0 号下落音符（带 alpha，贴合轨）
const Def kPianoNote0 = {"images/game/piano/note_0.png", 56, 28, 0, 0};
// 第 1 号下落音符（带 alpha，贴合轨）
const Def kPianoNote1 = {"images/game/piano/note_1.png", 56, 28, 0, 0};
// 第 2 号下落音符（带 alpha，贴合轨）
const Def kPianoNote2 = {"images/game/piano/note_2.png", 56, 28, 0, 0};
// 第 3 号下落音符（带 alpha，贴合轨）
const Def kPianoNote3 = {"images/game/piano/note_3.png", 56, 28, 0, 0};
// 第 4 号下落音符（带 alpha，贴合轨）
const Def kPianoNote4 = {"images/game/piano/note_4.png", 56, 28, 0, 0};
// 第 5 号下落音符（带 alpha，贴合轨）
const Def kPianoNote5 = {"images/game/piano/note_5.png", 56, 28, 0, 0};
// 第 6 号下落音符（带 alpha，贴合轨）
const Def kPianoNote6 = {"images/game/piano/note_6.png", 56, 28, 0, 0};
// 第 7 号下落音符（带 alpha，贴合轨）
const Def kPianoNote7 = {"images/game/piano/note_7.png", 56, 28, 0, 0};
// ---- drum ----
// 木纹舞台 + 6 条掉落轨 + 平的鼓架色区（静态整屏，整图不透明）
const Def kDrumBg = {"images/game/drum/bg.png", 480, 540, 0, 0};
// 第 0 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad0 = {"images/game/drum/pad_0.png", 74, 112, 0, 0};
// 第 1 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad1 = {"images/game/drum/pad_1.png", 74, 112, 0, 0};
// 第 2 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad2 = {"images/game/drum/pad_2.png", 74, 112, 0, 0};
// 第 3 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad3 = {"images/game/drum/pad_3.png", 74, 112, 0, 0};
// 第 4 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad4 = {"images/game/drum/pad_4.png", 74, 112, 0, 0};
// 第 5 号鼓垫（不透明，底烘鼓架色）
const Def kDrumPad5 = {"images/game/drum/pad_5.png", 74, 112, 0, 0};
// 第 0 号下落音符（带 alpha，贴合轨）
const Def kDrumNote0 = {"images/game/drum/note_0.png", 72, 26, 0, 0};
// 第 1 号下落音符（带 alpha，贴合轨）
const Def kDrumNote1 = {"images/game/drum/note_1.png", 72, 26, 0, 0};
// 第 2 号下落音符（带 alpha，贴合轨）
const Def kDrumNote2 = {"images/game/drum/note_2.png", 72, 26, 0, 0};
// 第 3 号下落音符（带 alpha，贴合轨）
const Def kDrumNote3 = {"images/game/drum/note_3.png", 72, 26, 0, 0};
// 第 4 号下落音符（带 alpha，贴合轨）
const Def kDrumNote4 = {"images/game/drum/note_4.png", 72, 26, 0, 0};
// 第 5 号下落音符（带 alpha，贴合轨）
const Def kDrumNote5 = {"images/game/drum/note_5.png", 72, 26, 0, 0};

// ==================== 布局常量（数独 / 节奏钢琴 / 打鼓）====================
// 由 tools/gen_instr_art.py 的常量表生成 —— **素材尺寸与布局必须同源**，
// 否则就会出现「图上画在一处、代码判在另一处」（缩进 1px 的错位肉眼看不出来）。
// ---- 数独：9x9 棋盘 + 数字键行 + 动作键行 ----
const int kSudokuCell = 44;
const int kSudokuBX = 42, kSudokuBY = 6;
const int kSudokuNumX = 17, kSudokuNumY = 410, kSudokuNumW = 46, kSudokuNumH = 46,
    kSudokuNumGap = 4;
const int kSudokuActX = 12, kSudokuActY = 464, kSudokuActW = 88, kSudokuActH = 58,
    kSudokuActGap = 4, kSudokuActN = 5;
// ---- 节奏钢琴：8 键（键宽 = 画布宽 / 8）----
const int kPianoKeyW = 60, kPianoKeyH = 144, kPianoKeyY = 392;
const int kPianoJudgeY = 388;
const int kPianoNoteW = 56, kPianoNoteH = 28;
// ---- 打鼓：6 鼓位（**单排，各自正对上方那条掉落轨**）+ 6 条掉落轨 ----
const int kDrumPadW = 74, kDrumPadH = 112;
const int kDrumPadX0 = 3, kDrumPadDX = 79;
const int kDrumPadY0 = 302;
const int kDrumNoteW = 72, kDrumNoteH = 26;
const int kDrumLaneW = 80, kDrumFallH = 300;

}  // namespace gameart
}  // namespace pg

#endif  // PG_GAME_ART_H_
