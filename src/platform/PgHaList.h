#ifndef PG_HA_LIST_H_
#define PG_HA_LIST_H_
/*
 * PgHaList.h - 「我的设备」= 用户从扫描结果里**挑出来**的那几台 + 自定义名字
 *
 * 为什么要有这一层（2026-09-23 用户需求）：
 *   实测这台 HA 有 30+ 个实体，而**真正要当遥控器用的只有 3~6 个**。
 *   直接把 /api/states 全列出来，屏幕头几行全是 `sensor.sun_next_dawn` /
 *   `Backup …` 这类内务实体，当遥控器完全没法用。
 *   ⇒ 界面拆成两页：主页只显示**用户自己添加过的**（本文件），
 *     另一页才是"扫描出来的全部"，从那里挑。
 *
 * 存储：`/data/ha_dev.txt`（可持久）→ 回退 `/tmp/ha_dev.txt`（掉电即失，调试用）。
 * 格式（每行一条，`#` 开头是注释）：
 *     <entity_id>\t<别名>
 *   别名**可以为空**（= 用 HA 的 friendly_name）。用 TAB 分隔是因为别名里可能有空格。
 *   ⚠️ 坏行（没有 entity_id）**跳过并 WARN**，不整表丢弃 —— 一行写坏不该让用户
 *      之前添加的设备全没了。
 *
 * ★ 线程：只有 UI 线程碰它（PgHa 的工作线程不知道这个列表存在），所以**不加锁**。
 *   这是刻意的 —— 任何"网络线程读列表"的写法都会把锁的顺序搞复杂。
 */

#include <string>
#include <vector>

namespace pg {

/** 我的设备上限。**上限不写死在使用处**（本工程的老规矩）：
 *  超了要 WARN 并拒绝添加，不是静默丢。 */
const int kHaListMax = 32;
const int kHaAliasMax = 48;

struct HaDev {
  char id[64];                  // entity_id
  char alias[kHaAliasMax];      // 自定义名（UTF-8）；空 = 用 HA 的 friendly_name

  HaDev() {
    id[0] = 0;
    alias[0] = 0;
  }
};

class HaList {
 public:
  static HaList &inst();

  /** 读盘。文件不存在 = 空表（**不是错误** —— 新装机的正常状态）。 */
  bool load();
  bool save() const;

  int count() const { return (int)items_.size(); }
  bool get(int i, HaDev *out) const;
  /** 在列表里的下标；-1 = 没添加过。 */
  int indexOf(const char *id) const;

  /** 添加（**不重复**：已经有了返回 false）。alias 可为空/0。 */
  bool add(const char *id, const char *alias);
  bool remove(const char *id);
  /** 改名。alias 传空串 = 清掉别名（退回 HA 原名）。 */
  bool rename(const char *id, const char *alias);
  /** 上移/下移（本期没有 UI 入口，留给"排序"用；保留是因为列表顺序是用户可见的）。 */
  bool move(int from, int to);

  /** 变了就让 UI 重刷（与 PgHa::generation 同一套"变了才刷"的做法）。 */
  unsigned generation() const { return gen_; }
  void copy(std::vector<HaDev> *out) const;

  const char *path() const { return path_; }
  int droppedLines() const { return dropped_; }   // 最近一次 load 跳过的坏行数

  /** 展示名：别名优先，没有就用传入的 HA 原名。 */
  static void displayName(const HaDev &d, const char *haName, char *out, int n);

  // ---------------- 重命名候选池（改版后 = 按域推荐的"快捷词片"） ----------------
  /** ★★ 2026-09-23 更正：这里原先写"本板没有中文输入法、只能候选名点选"，**那是错的**。
   *  本工程自带中文输入法：`src/logic/imeApp.cc` 是 `APP_TYPE_SYS_IME` 的 SysApp，
   *  支持**全拼**（候选 6 格 + 上下页），且**任何 ZKEditText 聚焦时系统自动拉起**，
   *  业务页不需要 showIME()。⇒ 命名改走 `div.input` 让用户**自由输入中文**，
   *  见 ui/ha.preview.html（设计稿）与 docs/ha-ux-redesign.md §4.6（四条硬约束）。
   *
   *  这份候选池**保留**，但角色变了：不再是"唯一输入手段"，而是
   *  **按域推荐的快捷词片**（点一下填入输入框，少打几个字）。
   *  这些字都在 GB2312 一级字库里（gen_font 的兜底字集），所以不会"整字消失"。
   *  ⚠️ 这里加名字**不用**重跑生成器，但若用了生僻字要跑 tools/gen_font.py。
   *  📌 落地改版时建议加一对按域取词的接口（如 aliasForDomain(domain, i)），
   *     本期（设计稿阶段）**不预留空声明** —— 免得留下"声明了没人实现"的悬空接口。 */
  static int aliasCount();
  static const char *alias(int i);

 private:
  HaList();
  HaList(const HaList &);
  HaList &operator=(const HaList &);

  std::vector<HaDev> items_;
  unsigned gen_;
  int dropped_;
  char path_[128];
};

}  // namespace pg

#endif  // PG_HA_LIST_H_
