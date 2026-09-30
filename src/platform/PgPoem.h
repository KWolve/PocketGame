/*
 * PgPoem.h - 屏保"每日诗词"
 *
 * 需求（2026-09-14）：
 *   · 屏保底部留白区显示一首诗词；
 *   · 点「喜欢 / 不喜欢」换一首；
 *   · **不喜欢的题材以后不再推送**；
 *   · **每天更新一次**。
 *
 * 数据来源：**联网抓取**（今日诗词 v2 API），每天抓一小批（默认 6 首）存本地，
 *   当天就在这批里换。设计上的三条硬约束（都是本板现实逼出来的）：
 *
 *   ① **绝不空白**：网络没起来 / 没连 WiFi / 证书时间不对（本板没有 RTC，未校时前
 *      是 1970，https 握手必失败）都会抓不到 ⇒ 必须**内置种子诗库**兜底。
 *      所以"联网"是增强，不是唯一来源。
 *   ② **不阻塞 UI**：抓取走后台线程（屏保是 20fps 的定时器，绝不能在回调里做网络）。
 *   ③ **题材本地归类**：接口给不出可靠的题材字段，而且"不喜欢某题材"要求离线也成立
 *      ⇒ 用关键词表从正文/标题归类（山水/思乡/边塞/送别/田园/咏物/愁绪/励志/爱情/节日）。
 *
 * 持久化（本板可写的持久分区）：/data/pocketgame_poem.dat，
 *   退化为 /mnt/extsd/pocketgame_poem.dat → /tmp/pocketgame_poem.dat（仅本次开机）。
 *   文件同时存"今天的日期 + 当前第几首 + 点赞/拉黑的题材"，所以**重启后当天的诗不换、
 *   偏好不丢**。
 *
 * QA（/tmp/pg_savercmd，只在屏保显示期间执行；命令见 screensaver.cc）：
 *   poem            打印状态（题材/来源/池大小/抓取是否在跑）
 *   poem next       换一首
 *   poem like       喜欢（换一首 + 题材加分）
 *   poem dislike    不喜欢（换一首 + 拉黑该题材）
 *   poem reload     清掉"今天已抓"的标记，立刻重新抓一次
 *   poem url <u>    临时换接口地址（自检用；`poem url -` 恢复默认）
 *   poem reset      清空偏好与缓存（回到内置种子）
 */
#ifndef PG_POEM_H_
#define PG_POEM_H_

#include <string>
#include <vector>

namespace pg {

/** 一首诗（只留屏保要显示的字段） */
struct Poem {
  std::string text;    // 正文（多句连成一行，句内用中文标点）
  std::string title;   // 标题
  std::string author;  // "唐·李白" 这种"朝代·作者"
  std::string tag;     // 题材（本地归类）

  Poem() {}
  Poem(const std::string &t, const std::string &ti, const std::string &a,
       const std::string &g)
      : text(t), title(ti), author(a), tag(g) {}
};

class PoemBook {
 public:
  static PoemBook &instance();

  /** 读缓存（幂等；首次调用时读盘） */
  void ensureLoaded();

  /** 屏保每帧调一次（内部按秒节流）：日切 + 必要时发起后台抓取。
   *  @return true = 需要刷新界面（换了诗 / 抓取状态变了） */
  bool tick();

  const Poem *current() const;

  /** 换一首（自动跳过被拉黑的题材；池子用完就用种子库补足） */
  void next();

  /** 喜欢：换一首，并给该题材记一次好评（用于优先推送） */
  void like();

  /** 不喜欢：换一首，并把该题材拉黑（以后不再推送） */
  void dislike();

  /** 状态行（给日志/QA 用） */
  std::string status() const;

  /* ---- 抓取线程回调（内部用；放 public 是因为抓取线程是自由函数，
   *      拿不到 private 访问权）---- */
  void addPoem(const Poem &p);                       // 去重后入池
  void onFetchDone(int ok, const std::string &err);  // 收尾：落盘 / 记日志

  // --------------------- QA / 自检 ---------------------
  void setUrlOverride(const std::string &url);  // 空串 = 恢复默认接口
  std::string urlOverride() const { return urlOverride_; }
  void resetAll();                              // 清偏好 + 清缓存 + 回种子
  void forceRefetch();                          // 清"今天已抓"标记并立刻重抓

 private:
  PoemBook();

  void loadFromDisk();
  void saveToDisk() const;
  void buildSeed();                             // 内置种子（兜底，必须始终可用）
  void startFetch();                            // 起后台抓取线程（已在跑则忽略）
  void pickIndex();                             // 在当前池里挑一首（避开拉黑题材）

  // 状态
  bool loaded_;
  std::string date_;                          // 池子是哪天抓的（北京时间的 YYYY-MM-DD）
  std::vector<Poem> pool_;                    // 今天的候选池（种子 + 当天抓到的）
  int cur_;                                   // 当前显示第几首
  std::vector<std::string> liked_;            // 喜欢的题材
  std::vector<std::string> blocked_;          // 拉黑的题材
  int fetchDay_;                              // 已经为哪一天抓过（YYYYMMDD，0 = 没抓过）
  int lastAttemptDay_;                        // 已经为哪一天**尝试**过抓取（防失败后每秒重试）
  volatile int fetching_;                     // 后台抓取进行中
  int lastFetched_;                           // 本次抓取成功了几首（日志/QA 用）
  std::string lastError_;                     // 最近一次抓取失败原因
  std::string urlOverride_;
  char path_[128];

  long lastTickMs_;
};

}  // namespace pg

#endif  // PG_POEM_H_
