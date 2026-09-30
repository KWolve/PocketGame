/*
 * PgHaList.cpp - 「我的设备」持久列表（实现）
 *
 * 设计理由与存储格式见 PgHaList.h。这里只记两条踩过的坑：
 *
 * ⚠️ ① **别用逗号/空格分隔**：entity_id 里有点号，而别名是中文短语，"用空格拼一行"
 *      在别名带空格时（"客厅 主灯"）会解析错。用 **TAB**，并且别名**整段收下**（不再二次切分）。
 * ⚠️ ② **保存用"先写临时文件再 rename"**：本板是 NAND，写一半掉电会留下半行，
 *      下次 load 会解析成一条坏记录。rename 是原子的，最坏情况是"这次没改成功"，
 *      而不是"列表坏了"。
 */

#include "platform/PgHaList.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utils/Log.h"

namespace pg {

namespace {

void copyStr(char *dst, size_t n, const char *src) {
  if (!dst || n == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  snprintf(dst, n, "%s", src);
}

/* 可写路径按顺序试（与 PgStore 同一套策略：可持久优先，/tmp 是调试兜底）。 */
const char *kPaths[] = {"/data/ha_dev.txt", "/tmp/ha_dev.txt"};

}  // namespace

// ---------------------------------------------------------------- 候选名池

/* 顺序 = 界面上从上到下。自己会的常用字都在 GB2312 一级字库里。 */
static const char *kAliases[] = {
    "客厅灯", "餐厅灯", "厨房灯", "主卧灯", "次卧灯", "书房灯", "卫生间灯",
    "阳台灯", "过道灯", "灯带", "台灯", "落地灯", "氛围灯", "主灯", "射灯",
    "壁灯", "客厅风扇", "排气扇", "空调", "加湿器", "插座", "电视", "窗帘",
    "扫地机", "门锁", "摄像头", "温度", "湿度", "客厅", "厨房", "卧室", "书房",
};

int HaList::aliasCount() { return (int)(sizeof(kAliases) / sizeof(kAliases[0])); }

const char *HaList::alias(int i) {
  if (i < 0 || i >= aliasCount()) return "";
  return kAliases[i];
}

// ---------------------------------------------------------------- 单例

HaList &HaList::inst() {
  static HaList s;
  return s;
}

HaList::HaList() : gen_(0), dropped_(0) {
  copyStr(path_, sizeof(path_), kPaths[0]);
}

// ---------------------------------------------------------------- 读写

bool HaList::load() {
  items_.clear();
  dropped_ = 0;
  /* ★ 先把"打算用哪条路径"填上：文件不存在时也留着值。
   *   第一版只在 fopen 成功时才写 path_ ⇒ 新装机（还没这个文件）时 QA dump 里
   *   这一行是**空的**，看起来像"路径没解析出来"（诊断信息缺失=误判来源）。 */
  copyStr(path_, sizeof(path_), kPaths[0]);

  FILE *f = 0;
  for (unsigned k = 0; k < sizeof(kPaths) / sizeof(kPaths[0]); ++k) {
    f = fopen(kPaths[k], "r");
    if (f) {
      copyStr(path_, sizeof(path_), kPaths[k]);
      break;
    }
  }
  if (!f) {
    /* ★ 文件不存在 = 空表，**不是错误**。第一次进"添加设备"页就是这个状态。 */
    LOGD("PgHaList: 还没有 /data/ha_dev.txt（我的设备为空，属正常）");
    ++gen_;
    return false;
  }

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int n = (int)strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n == 0 || line[0] == '#') continue;

    char id[64] = {0};
    char al[kHaAliasMax] = {0};
    const char *tab = strchr(line, '\t');
    if (tab) {
      size_t idLen = (size_t)(tab - line);
      if (idLen >= sizeof(id)) idLen = sizeof(id) - 1;
      memcpy(id, line, idLen);
      copyStr(al, sizeof(al), tab + 1);
    } else {
      /* 兼容"只有 id 没有别名"的行（手写文件/老版本写的） */
      copyStr(id, sizeof(id), line);
    }
    if (!id[0]) {
      ++dropped_;
      continue;
    }
    if ((int)items_.size() >= kHaListMax) {
      ++dropped_;
      continue;
    }
    HaDev d;
    copyStr(d.id, sizeof(d.id), id);
    copyStr(d.alias, sizeof(d.alias), al);
    items_.push_back(d);
  }
  fclose(f);

  if (dropped_) {
    /* ★ 不静默：坏行/超限都要说清楚（本工程的老规矩）。 */
    LOGW("PgHaList: %s 里有 %d 行被跳过（坏行或超过上限 %d）",
         path_, dropped_, kHaListMax);
  }
  ++gen_;
  LOGD("PgHaList: 已载入 %d 台设备（%s）", (int)items_.size(), path_);
  return true;
}

bool HaList::save() const {
  /* 按 load() 时确定的路径写；没载过就用首选路径。 */
  const char *p = path_[0] ? path_ : kPaths[0];
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", p);

  FILE *f = fopen(tmp, "w");
  if (!f) {
    LOGW("PgHaList: 写不了 %s（%s）—— 本次改动只在内存里，重启会丢", tmp, strerror(errno));
    return false;
  }
  fputs("# PocketGame 我的设备（entity_id<TAB>别名）—— 别名可空 = 用 HA 原名\n", f);
  for (size_t i = 0; i < items_.size(); ++i) {
    fprintf(f, "%s\t%s\n", items_[i].id, items_[i].alias);
  }
  /* ★ 必须 fsync/close 之后再 rename：只 close 的话数据可能还在页缓存里，
   *   掉电会得到"新文件名 + 空内容"。 */
  fflush(f);
  fsync(fileno(f));
  fclose(f);

  /* ⚠️ 必须写 `::rename` / `::remove` —— 不写的话名字查找先命中本类的成员
   *    `HaList::rename` / `HaList::remove`，在 const 成员函数里会报
   *    "passing 'const pg::HaList' as 'this' argument discards qualifiers"（已踩）。 */
  if (::rename(tmp, p) != 0) {
    LOGW("PgHaList: rename %s -> %s 失败：%s", tmp, p, strerror(errno));
    ::remove(tmp);
    return false;
  }
  LOGD("PgHaList: 已保存 %d 台设备 -> %s", (int)items_.size(), p);
  return true;
}

// ---------------------------------------------------------------- 增删改查

bool HaList::get(int i, HaDev *out) const {
  if (!out || i < 0 || i >= (int)items_.size()) return false;
  *out = items_[i];
  return true;
}

int HaList::indexOf(const char *id) const {
  if (!id || !id[0]) return -1;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (strcmp(items_[i].id, id) == 0) return (int)i;
  }
  return -1;
}

bool HaList::add(const char *id, const char *alias) {
  if (!id || !id[0]) return false;
  /* ★ 形状校验：HA 的 entity_id 一定是 `<domain>.<object_id>`，且**不含空白**。
   *   这一条是**实测逼出来的**：QA 命令 `add switch.x #10` 里的 `#10` 是注释、
   *   不是参数，剥注释前的版本把 ` #10` 一起当成了 id 存进列表 ——
   *   于是列表里躺着一条永远不会命中的设备（点了没反应、还查不出为什么）。
   *   ⇒ 与其在列表里静默存垃圾，不如当场拒掉并说清楚。 */
  if (!strchr(id, '.') || strchr(id, ' ') || strchr(id, '\t') || strchr(id, '#')) {
    LOGW("PgHaList: 拒绝添加 '%s' —— 不像 entity_id（要 <域>.<对象名>，且不含空格）",
         id);
    return false;
  }
  if (indexOf(id) >= 0) {
    LOGD("PgHaList: %s 已经添加过了，不重复加", id);
    return false;
  }
  if ((int)items_.size() >= kHaListMax) {
    LOGW("PgHaList: 已达上限 %d，拒绝添加 %s（要更多请改 kHaListMax）", kHaListMax, id);
    return false;
  }
  HaDev d;
  copyStr(d.id, sizeof(d.id), id);
  copyStr(d.alias, sizeof(d.alias), alias ? alias : "");
  items_.push_back(d);
  ++gen_;
  save();
  LOGD("PgHaList: 已添加 %s（别名 '%s'）—— 现在 %d 台", id, d.alias, (int)items_.size());
  return true;
}

bool HaList::remove(const char *id) {
  int i = indexOf(id);
  if (i < 0) return false;
  LOGD("PgHaList: 已移除 %s（原来第 %d 台）", id, i);
  items_.erase(items_.begin() + i);
  ++gen_;
  save();
  return true;
}

bool HaList::rename(const char *id, const char *alias) {
  int i = indexOf(id);
  if (i < 0) {
    LOGW("PgHaList: 改名失败 —— %s 不在我的设备里", id ? id : "(null)");
    return false;
  }
  const char *a = alias ? alias : "";
  if (strcmp(items_[i].alias, a) == 0) return true;      // 没变化，不写盘（省 NAND 寿命）
  copyStr(items_[i].alias, sizeof(items_[i].alias), a);
  ++gen_;
  save();
  LOGD("PgHaList: %s 改名为 '%s'", id, a[0] ? a : "(清空，用 HA 原名)");
  return true;
}

bool HaList::move(int from, int to) {
  int n = (int)items_.size();
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
  HaDev d = items_[from];
  items_.erase(items_.begin() + from);
  items_.insert(items_.begin() + to, d);
  ++gen_;
  save();
  return true;
}

void HaList::copy(std::vector<HaDev> *out) const {
  if (!out) return;
  *out = items_;
}

void HaList::displayName(const HaDev &d, const char *haName, char *out, int n) {
  if (!out || n <= 0) return;
  if (d.alias[0]) {
    copyStr(out, (size_t)n, d.alias);
    return;
  }
  if (haName && haName[0]) {
    copyStr(out, (size_t)n, haName);
    return;
  }
  copyStr(out, (size_t)n, d.id);
}

}  // namespace pg
