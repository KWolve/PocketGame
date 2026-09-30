#pragma once
/*
 * PgJson.h - 极简 JSON 解析（只为解析 Home Assistant 的响应）
 *
 * 为什么自己写而不引依赖（2026-09-23 决策）：
 *   实测这台 HA 的 `/api/states` 全量只有 **14 KB**、裁剪模板只有 **595 字节**，
 *   要处理的 JSON 很小、结构也很规矩（对象/数组/字符串/数字/布尔/null）。
 *   本工程已有"手写 HTTP 客户端"的先例（PgDlna），再引一个第三方库要多担一份
 *   体积、ABI 与构建适配的风险 —— 不划算。见 docs/ha-integration-plan.md §7.4。
 *
 * 设计：**索引式树**（不用裸指针）。
 *   · 解析一次把整棵树塞进 `Doc` 内部的 vector，节点之间用 int 下标互相引用。
 *   · 好处：没有递归 new/delete、不会有所有权问题，拷贝/销毁都是 O(1) 级别的清理；
 *     越界下标一律返回 -1（null），调用方不用到处判空指针。
 *   · 代价：`Doc` 不能跨线程共享（它本来就是"解析完就地取值"的用法）。
 *
 * ⚠️ 本板字库/内存都紧，所以：
 *   · 解析**不复制**输入文本，只在需要时把字符串 unescape 进节点；
 *   · 深度上限 `kMaxDepth`（防畸形输入把栈打爆 —— HA 的响应正常只有 3~4 层）。
 */

#include <string>
#include <vector>

namespace pg {
namespace json {

enum Type {
  T_NULL = 0,
  T_BOOL,
  T_NUM,
  T_STR,
  T_ARR,
  T_OBJ,
};

struct Node {
  int type;
  bool b;
  double num;
  std::string str;                 // T_STR
  std::vector<int> kids;           // T_ARR: 元素; T_OBJ: 值
  std::vector<std::string> keys;   // T_OBJ: 键（与 kids 一一对应）
  Node() : type(T_NULL), b(false), num(0) {}
};

class Doc {
 public:
  Doc() {}

  /** 解析。成功返回 true；失败时 `err()` 给出位置与原因。 */
  bool parse(const char *text, int len);
  bool parse(const std::string &s) { return parse(s.data(), (int)s.size()); }

  const std::string &err() const { return err_; }

  /** 根节点下标；解析失败为 -1 */
  int root() const { return root_; }

  int type(int n) const { return valid(n) ? nodes_[n].type : T_NULL; }
  bool isNull(int n) const { return type(n) == T_NULL; }

  /** 取 T_STR；非字符串返回空串 */
  const std::string &str(int n) const { return valid(n) ? nodes_[n].str : kEmpty; }
  const char *cstr(int n) const { return str(n).c_str(); }

  double num(int n) const { return valid(n) ? nodes_[n].num : 0.0; }
  bool boolean(int n) const { return valid(n) ? nodes_[n].b : false; }

  /** T_ARR / T_OBJ 的元素个数（其它类型返回 0） */
  int size(int n) const { return valid(n) ? (int)nodes_[n].kids.size() : 0; }

  /** T_ARR 第 i 个元素；越界返回 -1 */
  int at(int n, int i) const {
    if (!valid(n) || i < 0 || i >= (int)nodes_[n].kids.size()) return -1;
    return nodes_[n].kids[i];
  }

  /** T_OBJ 按键取成员；找不到返回 -1 */
  int member(int n, const char *key) const {
    if (!valid(n) || nodes_[n].type != T_OBJ || !key) return -1;
    const std::vector<std::string> &ks = nodes_[n].keys;
    for (size_t i = 0; i < ks.size(); ++i) {
      if (ks[i] == key) return nodes_[n].kids[i];
    }
    return -1;
  }

  /**
   * 便捷链路：root -> 一层层取成员。
   *   doc.path("attributes.friendly_name")  == member(member(root,"attributes"),"friendly_name")
   * 点号分隔，最多支持 kMaxPathSeg 段。任何一段缺失都返回 -1。
   */
  int path(const char *dotted) const;

  /** 字符串便捷取法：缺失/类型不对都返回 fallback（**不为 NULL**，调用方可以直接用） */
  const char *strOr(int n, const char *fallback) const {
    return (valid(n) && nodes_[n].type == T_STR) ? nodes_[n].str.c_str() : fallback;
  }

  /** 当 n 是对象时，取成员并当字符串用 */
  const char *memberStr(int n, const char *key, const char *fallback) const {
    int v = member(n, key);
    return strOr(v, fallback);
  }

 private:
  bool valid(int n) const { return n >= 0 && n < (int)nodes_.size(); }
  int push(const Node &nd) {
    nodes_.push_back(nd);
    return (int)nodes_.size() - 1;
  }

  // 递归下降。深度由调用点用 depth 控制。
  int parseValue(const char *s, int len, int &i, int depth);
  int parseString(const char *s, int len, int &i);
  int parseNumber(const char *s, int len, int &i);
  void skipWs(const char *s, int len, int &i);
  bool fail(const char *why, int pos);

  std::vector<Node> nodes_;
  int root_;
  std::string err_;
  static const std::string kEmpty;
};

}  // namespace json
}  // namespace pg
