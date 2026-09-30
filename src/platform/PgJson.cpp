/*
 * PgJson.cpp - 极简 JSON 解析实现（只解析，不生成）
 *
 * 只支持标准 JSON：对象 / 数组 / 字符串 / 数字 / true / false / null。
 * 容忍（真实 HA 响应会出现的）：
 *   · UTF-8 原样字节（中文 friendly_name）—— 不当 ASCII 处理；
 *   · `\uXXXX` 转义（HA 的 `|\uXXXX|` 形式，见实测报告里的
 *     `"Z20 HA Switch \u540a\u706f"`）⇒ 转成 UTF-8，并正确合并代理对；
 *   · 数字用 double（HA 的 state 基本都是字符串，数值属性够用）。
 * 不支持（用不到，且要说清楚）：
 *   · 注释、单引号字符串、尾随逗号、NaN/Infinity —— 这些都不是合法 JSON。
 */

#include "platform/PgJson.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace pg {
namespace json {

const std::string Doc::kEmpty;
static const int kMaxDepth = 24;      // HA 响应正常 3~4 层；留足余量防畸形输入
static const int kMaxPathSeg = 8;

bool Doc::fail(const char *why, int pos) {
  char b[128];
  snprintf(b, sizeof(b), "位置 %d：%s", pos, why ? why : "解析失败");
  err_ = b;
  return false;
}

void Doc::skipWs(const char *s, int len, int &i) {
  while (i < len) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++i;
    } else {
      break;
    }
  }
}

// 把 \uXXXX 的码点按 UTF-8 追加；跳过代理对时返回需要再读的那个低位单元
static void appendUtf8(std::string *out, unsigned cp) {
  if (cp < 0x80) {
    out->push_back((char)cp);
  } else if (cp < 0x800) {
    out->push_back((char)(0xC0 | (cp >> 6)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back((char)(0xE0 | (cp >> 12)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out->push_back((char)(0xF0 | (cp >> 18)));
    out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back((char)(0x80 | (cp & 0x3F)));
  }
}

static int hex4(const char *s, int len, int i) {
  if (i + 4 > len) return -1;
  int v = 0;
  for (int k = 0; k < 4; ++k) {
    char c = s[i + k];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = (v << 4) | d;
  }
  return v;
}

int Doc::parseString(const char *s, int len, int &i) {
  if (i >= len || s[i] != '"') {
    fail("期望字符串起始引号", i);
    return -1;
  }
  ++i;
  Node nd;
  nd.type = T_STR;
  nd.str.reserve(32);
  while (i < len) {
    char c = s[i];
    if (c == '"') {
      ++i;
      return push(nd);
    }
    if (c == '\\') {
      ++i;
      if (i >= len) break;
      char e = s[i++];
      switch (e) {
        case '"':  nd.str.push_back('"'); break;
        case '\\': nd.str.push_back('\\'); break;
        case '/':  nd.str.push_back('/'); break;
        case 'b':  nd.str.push_back('\b'); break;
        case 'f':  nd.str.push_back('\f'); break;
        case 'n':  nd.str.push_back('\n'); break;
        case 'r':  nd.str.push_back('\r'); break;
        case 't':  nd.str.push_back('\t'); break;
        case 'u': {
          int cp = hex4(s, len, i);
          if (cp < 0) {
            fail("\\u 转义不是 4 位十六进制", i);
            return -1;
          }
          i += 4;
          // 代理对：高 0xD800~0xDBFF + 低 0xDC00~0xDFFF → 合成一个码点
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= len && s[i] == '\\' &&
              s[i + 1] == 'u') {
            int lo = hex4(s, len, i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            }
          }
          appendUtf8(&nd.str, (unsigned)cp);
          break;
        }
        default:
          // 不是合法转义：原样保留那个字符（容忍，不整份失败）
          nd.str.push_back(e);
          break;
      }
      continue;
    }
    nd.str.push_back(c);
    ++i;
  }
  fail("字符串没有结束引号", i);
  return -1;
}

int Doc::parseNumber(const char *s, int len, int &i) {
  int st = i;
  if (i < len && (s[i] == '-' || s[i] == '+')) ++i;
  bool any = false;
  while (i < len && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
  if (i < len && s[i] == '.') {
    ++i;
    while (i < len && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
  }
  if (any && i < len && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < len && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < len && s[i] >= '0' && s[i] <= '9') ++i;
  }
  if (!any) {
    fail("数字格式不对", st);
    return -1;
  }
  std::string tmp(s + st, (size_t)(i - st));
  Node nd;
  nd.type = T_NUM;
  nd.num = atof(tmp.c_str());
  return push(nd);
}

int Doc::parseValue(const char *s, int len, int &i, int depth) {
  if (depth > kMaxDepth) {
    fail("嵌套太深（超过 24 层）", i);
    return -1;
  }
  skipWs(s, len, i);
  if (i >= len) {
    fail("内容意外结束", i);
    return -1;
  }
  char c = s[i];
  if (c == '{') {
    ++i;
    Node nd;
    nd.type = T_OBJ;
    int idx = push(nd);          // 先占位，子节点会 push 到它后面
    skipWs(s, len, i);
    if (i < len && s[i] == '}') {
      ++i;
      return idx;
    }
    while (i < len) {
      skipWs(s, len, i);
      int k = parseString(s, len, i);
      if (k < 0) return -1;
      skipWs(s, len, i);
      if (i >= len || s[i] != ':') {
        fail("对象里期望冒号", i);
        return -1;
      }
      ++i;
      int v = parseValue(s, len, i, depth + 1);
      if (v < 0) return -1;
      // ⚠️ 这里 nodes_ 可能已经扩容 ⇒ **必须重新按下标取引用**，不能持有旧引用
      nodes_[idx].keys.push_back(nodes_[k].str);
      nodes_[idx].kids.push_back(v);
      skipWs(s, len, i);
      if (i < len && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < len && s[i] == '}') {
        ++i;
        return idx;
      }
      fail("对象里期望逗号或右花括号", i);
      return -1;
    }
    fail("对象没有结束", i);
    return -1;
  }
  if (c == '[') {
    ++i;
    Node nd;
    nd.type = T_ARR;
    int idx = push(nd);
    skipWs(s, len, i);
    if (i < len && s[i] == ']') {
      ++i;
      return idx;
    }
    while (i < len) {
      int v = parseValue(s, len, i, depth + 1);
      if (v < 0) return -1;
      nodes_[idx].kids.push_back(v);   // 同上：不持有旧引用
      skipWs(s, len, i);
      if (i < len && s[i] == ',') {
        ++i;
        continue;
      }
      if (i < len && s[i] == ']') {
        ++i;
        return idx;
      }
      fail("数组里期望逗号或右方括号", i);
      return -1;
    }
    fail("数组没有结束", i);
    return -1;
  }
  if (c == '"') return parseString(s, len, i);
  if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return parseNumber(s, len, i);
  if (len - i >= 4 && strncmp(s + i, "true", 4) == 0) {
    i += 4;
    Node nd;
    nd.type = T_BOOL;
    nd.b = true;
    return push(nd);
  }
  if (len - i >= 5 && strncmp(s + i, "false", 5) == 0) {
    i += 5;
    Node nd;
    nd.type = T_BOOL;
    nd.b = false;
    return push(nd);
  }
  if (len - i >= 4 && strncmp(s + i, "null", 4) == 0) {
    i += 4;
    Node nd;
    nd.type = T_NULL;
    return push(nd);
  }
  fail("不认识的值", i);
  return -1;
}

bool Doc::parse(const char *text, int len) {
  nodes_.clear();
  root_ = -1;
  err_.clear();
  if (!text || len <= 0) {
    return fail("空输入", 0);
  }
  nodes_.reserve(64);   // 先留一手，减少扩容（HA 的响应节点数远大于 64）
  int i = 0;
  int r = parseValue(text, len, i, 0);
  if (r < 0) {
    root_ = -1;
    return false;
  }
  skipWs(text, len, i);
  if (i != len) {
    // 尾部有多余内容：对 `/api/states` 这种整份响应来说就是坏数据，要报出来
    return fail("尾部有多余内容", i);
  }
  root_ = r;
  return true;
}

int Doc::path(const char *dotted) const {
  if (!dotted || !*dotted) return root_;
  int cur = root_;
  const char *p = dotted;
  char seg[64];
  int segN = 0;
  for (int guard = 0; guard < kMaxPathSeg; ++guard) {
    if (!p || !*p) return cur;
    segN = 0;
    while (*p && *p != '.') {
      if (segN < (int)sizeof(seg) - 1) seg[segN++] = *p;
      ++p;
    }
    seg[segN] = 0;
    cur = member(cur, seg);
    if (cur < 0) return -1;
    if (*p == '.') ++p;
  }
  return cur;
}

}  // namespace json
}  // namespace pg
