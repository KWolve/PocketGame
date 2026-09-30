/*
 * PgHa.cpp - Home Assistant 客户端实现
 *
 * 只用标准 socket（**不引第三方 HTTP/JSON 库**）。理由见 PgHa.h 顶部与 PgJson.h。
 *
 * 关于 HTTP 的两个刻意选择（都是为了"少写代码、少出 bug"）：
 *   ① **每个请求新建连接、带 `Connection: close`，然后读到 EOF 为止**。
 *      这样**完全不用处理 chunked / keep-alive**，也不需要知道 Content-Length。
 *      代价是每轮一次 TCP 三次握手 —— 局域网实测 HA 往返 6~40 ms，可忽略。
 *   ② **只支持 http**。`tls=true` 会返回明确错误（不是静默降级）——
 *      本实例实测就是 http://192.168.1.188:8123。TLS 复用 PgDlna 的 dlopen 方案，留待需要时。
 */

#include "platform/PgHa.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include "platform/PgJson.h"
#include "utils/Log.h"

namespace pg {

// ---------------------------------------------------------------- 域表

static const char *kDomainNames[HD_COUNT] = {
    "other", "light", "switch", "sensor", "binary_sensor", "scene", "script",
    "automation", "button", "climate", "cover", "media_player", "fan", "lock",
    "camera", "input_boolean", "number", "select", "vacuum",
};

/* 可控域 = 点了会"做事"的域。只读域（sensor/binary_sensor/camera）不在内。
 * ⚠️ 这只回答"这个域有没有控制能力"；**要不要真的允许点**还要叠加
 *    "当前 state 是不是 on/off"（实测 unavailable 的实体命令会被吞，见文件头 ③）。 */
static const bool kDomainCtrl[HD_COUNT] = {
    /*other*/ false, /*light*/ true, /*switch*/ true, /*sensor*/ false,
    /*binary_sensor*/ false, /*scene*/ true, /*script*/ true, /*automation*/ true,
    /*button*/ true, /*climate*/ true, /*cover*/ true, /*media_player*/ true,
    /*fan*/ true, /*lock*/ true, /*camera*/ false, /*input_boolean*/ true,
    /*number*/ true, /*select*/ true, /*vacuum*/ true,
};

const char *haDomainName(int d) {
  if (d < 0 || d >= HD_COUNT) return "other";
  return kDomainNames[d];
}

bool haDomainControllable(int d) {
  if (d < 0 || d >= HD_COUNT) return false;
  return kDomainCtrl[d];
}

/* 单向触发域：命令发出去就"结束"，state **不会**因此变成 on/off ⇒ 不做回读确认。
 * 实测依据（docs/ha-verify.md）：`scene.turn_on` 之后 `scene.*` 的 state 一直是
 * 场景名（如 "movie"）或 unknown，永远不会等于我们设的"目标状态"。
 * script 同理；button 域本来就只有 press。 */
static const bool kDomainMomentary[HD_COUNT] = {
    /*other*/ false, /*light*/ false, /*switch*/ false, /*sensor*/ false,
    /*binary_sensor*/ false, /*scene*/ true, /*script*/ true, /*automation*/ false,
    /*button*/ true, /*climate*/ false, /*cover*/ false, /*media_player*/ false,
    /*fan*/ false, /*lock*/ false, /*camera*/ false, /*input_boolean*/ false,
    /*number*/ false, /*select*/ false, /*vacuum*/ false,
};

bool haDomainMomentary(int d) {
  if (d < 0 || d >= HD_COUNT) return false;
  return kDomainMomentary[d];
}

/* 「域 + 当前状态」→ 要调用的服务名。
 *
 * ⚠️ 这张表是**实测踩出来的**，不是照 HA 文档抄的：
 *   · switch/light/fan/input_boolean/automation/climate/media_player 都是 turn_on/turn_off；
 *   · cover 不是 turn_*，是 **open_cover/close_cover**（发 turn_on 会 400）；
 *   · lock 是 **lock/unlock**；
 *   · button 是 **press**；
 *   · scene / script 是 turn_on（没有 off）。
 * 发错服务的表现是 HTTP 400（硬失败，能立刻看出来），所以这里宁可写全。 */
bool haToggleServiceFor(int domain, const char *curState, char *svcOut, int n) {
  if (!svcOut || n <= 0) return false;
  svcOut[0] = 0;
  if (!haDomainControllable(domain)) return false;

  const bool on = curState && strcmp(curState, "on") == 0;
  const char *svc = 0;
  switch (domain) {
    case HD_BUTTON:
      svc = "press";
      break;
    case HD_SCENE:
    case HD_SCRIPT:
      svc = "turn_on";           // 单向触发：没有"关场景"这回事
      break;
    case HD_COVER:
      svc = on ? "close_cover" : "open_cover";
      break;
    case HD_LOCK:
      svc = on ? "unlock" : "lock";   // 注意：lock 的 state 是 locked/unlocked，
                                      // 不是 on/off ⇒ 走不到这里（被 isOnOff 挡掉），
                                      // 保留分支只为"以后支持 locked/unlocked"时不再改错方向
      break;
    case HD_NUMBER:
    case HD_SELECT:
      return false;              // 这两个要带具体值，不是"翻转"，本期不做
    default:
      svc = on ? "turn_off" : "turn_on";
      break;
  }
  snprintf(svcOut, (size_t)n, "%s", svc);
  return true;
}

int haTileStateOf(const char *state) {
  if (!state || !state[0]) return HTS_FLAT;
  if (strcmp(state, "on") == 0) return HTS_ON;
  if (strcmp(state, "off") == 0) return HTS_OFF;
  /* locked / unlocked 也算"明确的两态"，别当成离线灰 —— 它们是有效状态。 */
  if (strcmp(state, "locked") == 0) return HTS_ON;
  if (strcmp(state, "unlocked") == 0) return HTS_OFF;
  return HTS_FLAT;
}

void haTilePicName(int domain, int tileState, char *out, int n) {
  if (!out || n <= 0) return;
  static const char *kTileDom[HD_COUNT] = {
      "other", "light", "switch", "sensor", "binary_sensor", "scene", "script",
      "automation", "button", "climate", "cover", "media_player", "fan", "lock",
      "camera", "input_boolean", "number", "select", "vacuum",
  };
  const char *dom = (domain >= 0 && domain < HD_COUNT) ? kTileDom[domain] : "other";
  /* ⚠️ 图片只有三档（on/off/flat）—— 别的状态（unavailable/unknown/数值）
   *    一律落到 flat。理由：实测 34 个实体里只有 4 个是真 on/off，
   *    如果给每个 state 都烘一张图，图档会爆 /res 分区（只剩 ~330KB 余量）。 */
  static const char *kSuf[3] = {"off", "on", "flat"};
  int s = (tileState >= 0 && tileState <= 2) ? tileState : HTS_FLAT;
  snprintf(out, (size_t)n, "images/ha_tile_%s_%s.png", dom, kSuf[s]);
}

bool HaEntity::strEq(const char *s) const {
  return s && strcmp(state, s) == 0;
}

static int domainOf(const char *entityId) {
  if (!entityId) return HD_OTHER;
  const char *dot = strchr(entityId, '.');
  if (!dot) return HD_OTHER;
  size_t n = (size_t)(dot - entityId);
  for (int d = 0; d < HD_COUNT; ++d) {
    if (strlen(kDomainNames[d]) == n && strncmp(entityId, kDomainNames[d], n) == 0) {
      return d;
    }
  }
  return HD_OTHER;
}

/* 列表里要显示的域（把 sun / zone / conversation / tts / todo / weather / event / person
 * 这些"HA 自己的内务实体"挡在外面 —— 实测 34 个实体里有一大半是这类，全列出来只会淹掉
 * 真正的设备）。**但个数上限不写死**：由 kHaMaxEntity 兜底并 WARN。 */
static bool domainWanted(int d) {
  return d != HD_OTHER;
}

// ---------------------------------------------------------------- 工具

static long nowMsImpl() {
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void copyStr(char *dst, size_t n, const char *src) {
  if (!dst || n == 0) return;
  if (!src) {
    dst[0] = 0;
    return;
  }
  snprintf(dst, n, "%s", src);
}

// ---------------------------------------------------------------- HaCfg

HaCfg::HaCfg() : port(8123), tls(false), pollSec(3) {
  host[0] = 0;
  token[0] = 0;
  parseUrl("http://192.168.1.188:8123");
}

void HaCfg::parseUrl(const char *url) {
  if (!url || !*url) return;
  const char *p = url;
  tls = false;
  if (strncmp(p, "https://", 8) == 0) {
    tls = true;
    p += 8;
  } else if (strncmp(p, "http://", 7) == 0) {
    p += 7;
  }
  const char *slash = strchr(p, '/');
  size_t hostLen = slash ? (size_t)(slash - p) : strlen(p);
  char tmp[80];
  if (hostLen >= sizeof(tmp)) hostLen = sizeof(tmp) - 1;
  memcpy(tmp, p, hostLen);
  tmp[hostLen] = 0;
  const char *colon = strrchr(tmp, ':');
  if (colon) {
    int pt = atoi(colon + 1);
    if (pt > 0 && pt < 65536) port = pt;
    size_t hl = (size_t)(colon - tmp);
    tmp[hl] = 0;
  } else {
    port = tls ? 443 : 8123;
  }
  copyStr(host, sizeof(host), tmp);
}

void HaCfg::url(char *out, int n) const {
  snprintf(out, (size_t)n, "%s://%s:%d", tls ? "https" : "http", host, port);
}

/* 配置格式（每行 key=value，`#` 开头是注释）：
 *   url=http://192.168.1.188:8123
 *   token=eyJhbGci...
 *   poll=3
 * ⚠️ 设备上**只放 /data/ha.conf**（可持久）。/tmp/ha.conf 是给 adb push 快调的临时位，
 *    掉电即失 —— 所以是**回退**而不是首选。 */
bool HaCfg::load() {
  static const char *kPaths[] = {"/data/ha.conf", "/tmp/ha.conf"};
  for (unsigned k = 0; k < sizeof(kPaths) / sizeof(kPaths[0]); ++k) {
    FILE *f = fopen(kPaths[k], "r");
    if (!f) continue;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      int n = (int)strlen(line);
      while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
      if (!n || line[0] == '#') continue;
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      const char *key = line;
      const char *val = eq + 1;
      if (strcmp(key, "url") == 0) {
        parseUrl(val);
      } else if (strcmp(key, "token") == 0) {
        copyStr(token, sizeof(token), val);
      } else if (strcmp(key, "poll") == 0) {
        pollSec = atoi(val);
        if (pollSec < 0) pollSec = 0;
        if (pollSec > 3600) pollSec = 3600;
      }
    }
    fclose(f);
    LOGD("PgHa: 配置来自 %s host=%s port=%d tls=%d poll=%d token长度=%d",
         kPaths[k], host, port, (int)tls, pollSec, (int)strlen(token));
    return host[0] != 0;
  }
  LOGW("PgHa: 找不到配置文件（/data/ha.conf 或 /tmp/ha.conf）");
  return false;
}

bool HaCfg::save() const {
  FILE *f = fopen("/data/ha.conf", "w");
  if (!f) {
    LOGW("PgHa: /data/ha.conf 写不了（只读或空间不足）");
    return false;
  }
  fprintf(f, "# PocketGame 智能家居配置（本文件含令牌，别提交进仓库）\n");
  fprintf(f, "# 用法：url=http://<HA的IP>:8123 / token=<长期访问令牌> / poll=<轮询秒>\n");
  char u[96];
  url(u, sizeof(u));
  fprintf(f, "url=%s\n", u);
  fprintf(f, "token=%s\n", token);
  fprintf(f, "poll=%d\n", pollSec);
  fclose(f);
  LOGD("PgHa: 配置已写入 /data/ha.conf（%s，poll=%d）", u, pollSec);
  return true;
}

// ---------------------------------------------------------------- Ha 单例

Ha &Ha::inst() {
  static Ha s;
  return s;
}

Ha::Ha()
    : tid_(0),
      tidValid_(0),
      running_(0),
      stopReq_(0),
      qHead_(0),
      qTail_(0),
      phase_(OFFLINE),
      lastHttp_(0),
      lastOkMs_(0),
      nextPollMs_(0),
      backoffMs_(0),
      fakeOffline_(0),
      reqTotal_(0),
      errTotal_(0),
      httpFail_(0),
      parseFail_(0),
      generation_(0),
      dropped_(0),
      cmdTotal_(0),
      cmdFail_(0),
      ctlOk_(0) {
  pthread_mutex_init(&mtx_, 0);
  pthread_cond_init(&cv_, 0);
  haVersion_[0] = 0;
  copyStr(unitSystem_, sizeof(unitSystem_), "-");
  lastErr_[0] = 0;
  ctlMsg_[0] = 0;
  ctlReject_[0] = 0;
  ents_.reserve(64);
}

Ha::~Ha() {
  stop();
  pthread_mutex_destroy(&mtx_);
  pthread_cond_destroy(&cv_);
}

// ---------------------------------------------------------------- 命令队列

bool Ha::push(int cmd, const char *arg) {
  pthread_mutex_lock(&mtx_);
  int next = (qTail_ + 1) % kHaQSize;
  if (next == qHead_) {
    /* 队列满了。★ 不能静默丢 —— 但要给个明确的说法（UI 点太快时的正常现象）。 */
    LOGW("PgHa: 命令队列满（%d 条），丢弃 cmd=%d", kHaQSize, cmd);
    pthread_mutex_unlock(&mtx_);
    return false;
  }
  q_[qTail_].cmd = cmd;
  copyStr(q_[qTail_].arg, sizeof(q_[qTail_].arg), arg);
  qTail_ = next;
  pthread_cond_signal(&cv_);
  pthread_mutex_unlock(&mtx_);
  return true;
}

void Ha::reqPing() { push(CMD_PING, 0); }
void Ha::reqConfig() { push(CMD_CONFIG, 0); }
void Ha::reqStates() { push(CMD_STATES, 0); }
void Ha::reqEntity(const char *eid) { push(CMD_ENTITY, eid); }

// ---------------------------------------------------------------- ★★ 控制

bool Ha::ctlPendingLocked(const char *entityId, char *targetOut, int n) const {
  if (!entityId) return false;
  for (int i = 0; i < kHaMaxPending; ++i) {
    if (!pend_[i].used) continue;
    if (strcmp(pend_[i].id, entityId) != 0) continue;
    if (targetOut && n > 0) copyStr(targetOut, (size_t)n, pend_[i].target);
    return true;
  }
  return false;
}

bool Ha::ctlReadbackLocked(char *out, int n) const {
  for (int i = 0; i < kHaMaxPending; ++i) {
    /* deadlineMs_ == 0 的是单向触发（不等回读）⇒ 跳过，别为它空转轮询。 */
    if (!pend_[i].used || pend_[i].deadlineMs == 0) continue;
    if (out && n > 0) copyStr(out, (size_t)n, pend_[i].id);
    return true;
  }
  return false;
}

void Ha::entityNameLocked(const char *id, char *out, int n) const {
  if (!out || n <= 0) return;
  for (size_t k = 0; k < ents_.size(); ++k) {
    if (strcmp(ents_[k].id, id) == 0 && ents_[k].name[0]) {
      copyStr(out, (size_t)n, ents_[k].name);
      return;
    }
  }
  copyStr(out, (size_t)n, id);
}

void Ha::completeCallPending(int httpCode, bool ok, const char *what) {
  if (!callEntity_ || !callEntity_[0]) return;
  char msg[160] = {0};
  char nm[48] = {0};

  pthread_mutex_lock(&mtx_);
  entityNameLocked(callEntity_, nm, sizeof(nm));
  /* 找到并清掉这条 pending（按 id 定位，不靠槽位号 —— 期间可能有别的点击）。 */
  bool had = false;
  for (int i = 0; i < kHaMaxPending; ++i) {
    if (pend_[i].used && strcmp(pend_[i].id, callEntity_) == 0) {
      pend_[i].used = false;
      had = true;
      break;
    }
  }
  if (!had) {
    pthread_mutex_unlock(&mtx_);
    return;      // 已经被 ctlTick 结算过（比如超时先到了）
  }
  if (ok) {
    snprintf(msg, sizeof(msg), "%s：%s", what, nm);
    ++ctlOk_;
  } else {
    if (httpCode > 0) {
      snprintf(msg, sizeof(msg), "%s（HTTP %d）：%s", what, httpCode, nm);
    } else {
      snprintf(msg, sizeof(msg), "%s：%s", what, nm);
    }
    ++cmdFail_;
  }
  copyStr(ctlMsg_, sizeof(ctlMsg_), msg);
  ++generation_;                 // 让 UI 立刻重画（乐观态 → 真实态 / 回滚）
  pthread_mutex_unlock(&mtx_);
  if (ok) {
    LOGD("PgHa: 控制%s —— %s", what, msg);
  } else {
    LOGW("PgHa: 控制失败 —— %s", msg);
  }
}

void Ha::ctlTick() {
  long now = nowMsImpl();
  char doneMsg[160] = {0};
  int doneOk = 0;
  char failMsg[160] = {0};
  int doneFail = 0;

  pthread_mutex_lock(&mtx_);
  for (int i = 0; i < kHaMaxPending; ++i) {
    Pending &p = pend_[i];
    if (!p.used) continue;
    if (p.deadlineMs == 0) continue;      // 单向触发由 doOnce 当场结算

    /* 回读：实体表里有这个 id 吗？状态到位了吗？ */
    const HaEntity *e = 0;
    for (size_t k = 0; k < ents_.size(); ++k) {
      if (strcmp(ents_[k].id, p.id) == 0) {
        e = &ents_[k];
        break;
      }
    }
    if (e && strcmp(e->state, p.target) == 0) {
      long ms = now - p.startMs;
      snprintf(doneMsg, sizeof(doneMsg), "已确认：%s → %s（%ld ms）",
               e->name[0] ? e->name : p.id, p.target, ms);
      doneOk = 1;
      p.used = false;
      ++ctlOk_;
      ++generation_;                        // 让 UI 立刻重画（乐观态 → 真实态）
      continue;
    }
    if (now >= p.deadlineMs) {
      snprintf(failMsg, sizeof(failMsg), "设备无应答，已回滚：%s（%d 秒超时）",
               e && e->name[0] ? e->name : p.id, kHaCtlTimeoutMs / 1000);
      doneFail = 1;
      p.used = false;
      ++cmdFail_;
      ++generation_;
      continue;
    }
  }
  if (doneMsg[0]) copyStr(ctlMsg_, sizeof(ctlMsg_), doneMsg);
  if (failMsg[0]) copyStr(ctlMsg_, sizeof(ctlMsg_), failMsg);
  pthread_mutex_unlock(&mtx_);

  if (doneOk) LOGD("PgHa: %s", ctlMsg_);
  if (doneFail) LOGW("PgHa: %s", failMsg);
}

bool Ha::reqToggle(const char *entityId) {
  /* ★ 每条失败分支都要写清原因到 ctlReject_（并在日志里也打一条）。
   *   用宏而不是手写 —— 免得以后新增分支时又忘了（"静默失败必须消灭"）。 */
  #define RJ(fmt, ...)                                                       \
    do {                                                                     \
      pthread_mutex_lock(&mtx_);                                             \
      snprintf(ctlReject_, sizeof(ctlReject_), fmt, ##__VA_ARGS__);          \
      pthread_mutex_unlock(&mtx_);                                           \
      LOGW("PgHa: reqToggle 被拒：" fmt, ##__VA_ARGS__);                     \
    } while (0)

  if (!entityId || !entityId[0]) {
    RJ("entity_id 是空的");
    return false;
  }

  char arg[kHaArgMax];
  char target[16] = {0};
  const char *domName = "other";
  char svc[24] = {0};
  bool momentary = false;
  int dom = HD_OTHER;
  char curState[40] = {0};

  pthread_mutex_lock(&mtx_);
  if (!running_) {
    pthread_mutex_unlock(&mtx_);
    RJ("客户端没在跑（没配置 / 已 stop）");
    return false;
  }
  if (ctlPendingLocked(entityId, 0, 0)) {
    pthread_mutex_unlock(&mtx_);
    RJ("%s 上一条命令还在确认中（等它出结果再点）", entityId);
    return false;
  }
  /* 实体必须在当前快照里（列表是 HA 给的，不在 = 名字写错了） */
  const HaEntity *e = 0;
  for (size_t k = 0; k < ents_.size(); ++k) {
    if (strcmp(ents_[k].id, entityId) == 0) {
      e = &ents_[k];
      break;
    }
  }
  if (!e) {
    pthread_mutex_unlock(&mtx_);
    RJ("%s 不在实体快照里（快照 %d 条）", entityId, (int)ents_.size());
    return false;
  }
  dom = e->domain;
  copyStr(curState, sizeof(curState), e->state);
  domName = haDomainName(dom);

  if (!haDomainControllable(dom)) {
    pthread_mutex_unlock(&mtx_);
    RJ("%s 是只读域 %s", entityId, domName);
    return false;
  }
  momentary = haDomainMomentary(dom);
  /* ★★ 灰态禁点：实测给 unavailable 的实体发命令是 **200 但状态永久不变**
   *    ⇒ 让它可点就等于教用户"这个按钮是坏的"。 */
  if (!momentary && !e->isOnOff()) {
    pthread_mutex_unlock(&mtx_);
    RJ("%s 当前是「%s」不是 on/off，实测这种实体命令会被吞", entityId, curState);
    return false;
  }
  if (!haToggleServiceFor(dom, curState, svc, sizeof(svc))) {
    pthread_mutex_unlock(&mtx_);
    RJ("%s 域 %s 没有可用的翻转服务", entityId, domName);
    return false;
  }

  /* 目标状态：单向触发域没有"目标"（它的 state 不会变），留空。 */
  if (!momentary) {
    const bool isOn = (strcmp(curState, "on") == 0);
    snprintf(target, sizeof(target), "%s", isOn ? "off" : "on");
  }

  /* 占一个 pending 槽（乐观态）。找不到空位 = 已经 6 个在途，拒掉。 */
  int slot = -1;
  for (int i = 0; i < kHaMaxPending; ++i) {
    if (!pend_[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    pthread_mutex_unlock(&mtx_);
    RJ("在途控制已达上限 %d（说明前面几条一直没被确认）", kHaMaxPending);
    return false;
  }
  Pending &p = pend_[slot];
  p.used = true;
  ctlReject_[0] = 0;              // 这次已经受理，清掉上一条拒绝原因
  copyStr(p.id, sizeof(p.id), entityId);
  copyStr(p.target, sizeof(p.target), target);
  p.startMs = nowMsImpl();
  p.deadlineMs = momentary ? 0 : (p.startMs + kHaCtlTimeoutMs);
  /* ★ 立刻要一次回读（把定时器拨到"已到点"）。不写这一句的话，回读要等上一次
   *   定时轮询的 3 秒周期到点 —— 乐观态会挂 3 秒才变成真值，手感完全变味。 */
  if (!momentary) nextPollMs_ = p.startMs;
  ++cmdTotal_;
  ++generation_;                    // UI 立刻按乐观态重画
  pthread_mutex_unlock(&mtx_);

  snprintf(arg, sizeof(arg), "%s/%s/%s", domName, svc, entityId);
  if (!push(CMD_CALL, arg)) {
    /* 队列满：把占位退掉，否则 UI 会永远显示"在途"，5 秒后才假回滚。 */
    pthread_mutex_lock(&mtx_);
    p.used = false;
    --cmdTotal_;
    snprintf(ctlReject_, sizeof(ctlReject_), "命令队列满，%s 没发出去", entityId);
    ++generation_;
    pthread_mutex_unlock(&mtx_);
    LOGW("PgHa: 控制命令队列满，%s 没发出去", entityId);
    return false;
  }
  LOGD("PgHa: 控制 %s（%s，%s）→ %s/%s，目标=%s", e ? e->name : entityId, entityId,
       curState, domName, svc, target[0] ? target : "（单向触发，不回读）");
  #undef RJ
  return true;
}

const char *Ha::ctlReject() const {
  /* ⚠️ 同 ctlMsg：返回内部缓冲，调用方立刻用掉。 */
  return ctlReject_;
}

int Ha::ctlPending(const char *entityId, char *targetOut, int n) const {
  pthread_mutex_lock(&mtx_);
  bool r = ctlPendingLocked(entityId, targetOut, n);
  pthread_mutex_unlock(&mtx_);
  return r ? 1 : 0;
}

const char *Ha::ctlMsg() const {
  /* ⚠️ 返回内部缓冲的指针：调用方**立刻**用掉（塞进控件），别存。 */
  return ctlMsg_;
}

int Ha::cmdTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = cmdTotal_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

int Ha::cmdFailTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = cmdFail_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

int Ha::ctlOkTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = ctlOk_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

// ---------------------------------------------------------------- 生命周期

bool Ha::start() {
  if (running_) return true;
  if (!cfg_.load() || !cfg_.host[0]) {
    LOGW("PgHa: 没有配置（host 为空），不启动");
    return false;
  }
  if (!cfg_.token[0]) {
    LOGW("PgHa: 配置里没有 token，不启动");
    return false;
  }
  stopReq_ = 0;
  running_ = 1;
  pthread_mutex_lock(&mtx_);
  qHead_ = qTail_ = 0;
  phase_ = CONNECTING;
  nextPollMs_ = nowMsImpl();     // 立刻先来一轮，别等 3 秒
  backoffMs_ = 0;
  pthread_mutex_unlock(&mtx_);
  if (pthread_create(&tid_, 0, &Ha::threadEntry, this) != 0) {
    running_ = 0;
    LOGW("PgHa: 工作线程创建失败");
    return false;
  }
  tidValid_ = 1;
  LOGD("PgHa: 已启动（%s://%s:%d，poll=%d 秒）", cfg_.tls ? "https" : "http",
       cfg_.host, cfg_.port, cfg_.pollSec);
  return true;
}

void Ha::stop() {
  if (!running_) return;
  stopReq_ = 1;
  pthread_mutex_lock(&mtx_);
  pthread_cond_signal(&cv_);
  pthread_mutex_unlock(&mtx_);
  if (tidValid_) {
    pthread_join(tid_, 0);      // 必须 join：否则线程还在用 this（PgDlna 的教训）
    tidValid_ = 0;
  }
  running_ = 0;
  LOGD("PgHa: 已停止");
}

void *Ha::threadEntry(void *self) {
  static_cast<Ha *>(self)->loop();
  return 0;
}

void Ha::loop() {
  while (!stopReq_) {
    Item it;
    bool has = false;

    pthread_mutex_lock(&mtx_);
    /* ★ 在途控制优先：它比"3 秒一次的定时轮询"更急，而且要走**单实体**读
     *   （`GET /api/states/<id>` 几十字节），不是全量 14KB。 */
    char pendId[64] = {0};
    const bool ctlWait = ctlReadbackLocked(pendId, sizeof(pendId));

    if (qHead_ != qTail_) {
      it = q_[qHead_];
      qHead_ = (qHead_ + 1) % kHaQSize;
      has = true;
    } else if (ctlWait && nowMsImpl() >= nextPollMs_) {
      it.cmd = CMD_ENTITY;
      snprintf(it.arg, sizeof(it.arg), "%s", pendId);
      has = true;
    } else if (cfg_.pollSec > 0 && nowMsImpl() >= nextPollMs_) {
      it.cmd = CMD_POLL;
      has = true;
    } else {
      /* 等到"有命令"或"该轮询了"中较早的那个；用带超时的 wait（不能死等，
       * 否则定时轮询永远不触发）。 */
      long waitMs = 500;
      if (cfg_.pollSec > 0) {
        long d = nextPollMs_ - nowMsImpl();
        if (d > 0 && d < waitMs) waitMs = d;
      }
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += waitMs / 1000;
      ts.tv_nsec += (waitMs % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
      }
      pthread_cond_timedwait(&cv_, &mtx_, &ts);
    }
    pthread_mutex_unlock(&mtx_);

    if (stopReq_) break;
    if (!has) continue;

    bool ok = doOnce(it.cmd, it.arg);

    pthread_mutex_lock(&mtx_);
    if (it.cmd == CMD_POLL || it.cmd == CMD_STATES) {
      nextPollMs_ = nowMsImpl() + (long)cfg_.pollSec * 1000L;
    }
    /* ★ 在途控制期间把轮询节拍提到 250ms。注意这一句**必须在下一轮取命令之前生效**
     *   —— loop 每轮都重新读它，所以只在这里设就够。 */
    if (ctlReadbackLocked(0, 0)) {
      nextPollMs_ = nowMsImpl() + kHaCtlPollMs;
    }
    if (ok) {
      /* 成功：清退避。ONLINE 的判定放在这里（有成功响应就是在线）。 */
      backoffMs_ = 0;
      lastOkMs_ = nowMsImpl();
      if (phase_ != ONLINE && phase_ != AUTH_FAIL) phase_ = ONLINE;
    } else {
      /* 失败：指数退避 2/4/8/16/30 秒封顶。★ AUTH_FAIL 不退避也不重试 ——
       * 令牌错了重试一万次还是错，白耗电、白刷日志。 */
      if (phase_ != AUTH_FAIL) {
        if (backoffMs_ == 0) {
          backoffMs_ = 2000;
        } else if (backoffMs_ < 30000) {
          backoffMs_ *= 2;
          if (backoffMs_ > 30000) backoffMs_ = 30000;
        }
        nextPollMs_ = nowMsImpl() + backoffMs_;
      } else {
        nextPollMs_ = nowMsImpl() + 60000;   // 令牌错：一分钟才重试一次
      }
    }
    pthread_mutex_unlock(&mtx_);

    /* ★ 每拍结算在途控制：读到目标值 = 成功；到点没读到 = 回滚 + 说明原因。
     *   放在退避逻辑之后，所以它读到的是**这一拍刚更新过的实体表**。 */
    ctlTick();
  }
}

// ---------------------------------------------------------------- HTTP

/** 建 TCP 连接（带超时的非阻塞 connect）。成功返回 fd，失败返回 -1 并填 err。 */
static int tcpConnect(const char *host, int port, int timeoutMs, std::string *err) {
  char portStr[16];
  snprintf(portStr, sizeof(portStr), "%d", port);
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;          // 本板无 IPv6 需求；AF_INET 也能避开一些解析坑
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = 0;
  int gai = getaddrinfo(host, portStr, &hints, &res);
  if (gai != 0 || !res) {
    if (err) {
      char b[96];
      snprintf(b, sizeof(b), "域名/IP 解析失败(%s)：%s", host, gai_strerror(gai));
      *err = b;
    }
    return -1;
  }
  int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    if (err) *err = "socket 创建失败";
    return -1;
  }
  int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  int rc = connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (rc < 0 && errno != EINPROGRESS) {
    if (err) {
      char b[96];
      snprintf(b, sizeof(b), "connect 失败：%s", strerror(errno));
      *err = b;
    }
    close(fd);
    return -1;
  }
  if (rc < 0) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd, &wf);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int s = select(fd + 1, 0, &wf, 0, &tv);
    if (s <= 0) {
      if (err) *err = (s == 0) ? "connect 超时" : "connect 出错(select)";
      close(fd);
      return -1;
    }
    int soErr = 0;
    socklen_t sl = sizeof(soErr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &sl);
    if (soErr != 0) {
      if (err) {
        char b[96];
        snprintf(b, sizeof(b), "connect 被拒：%s", strerror(soErr));
        *err = b;
      }
      close(fd);
      return -1;
    }
  }
  // 回到阻塞 + 收发超时（后者是"读卡住"的唯一防线）
  fcntl(fd, F_SETFL, fl);
  struct timeval rto;
  rto.tv_sec = 8;
  rto.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rto, sizeof(rto));
  return fd;
}

static bool sendAll(int fd, const std::string &s, std::string *err) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) {
      if (errno == EINTR) continue;
      if (err) {
        char b[96];
        snprintf(b, sizeof(b), "send 失败：%s", strerror(errno));
        *err = b;
      }
      return false;
    }
    off += (size_t)n;
  }
  return true;
}

/* 一次请求。刻意用 `Connection: close` + 读到 EOF —— 于是不需要处理 chunked 与
 * Content-Length，代码少一半、出错面也小一半。
 * 返回 true = 拿到了 HTTP 响应（不管状态码是多少）；false = 连接层就失败了。 */
bool Ha::fetch(const char *method, const char *path, const char *body,
               int *httpCode, std::string *resp, std::string *err) {
  if (httpCode) *httpCode = 0;
  if (resp) resp->clear();
  if (err) err->clear();

  if (cfg_.tls) {
    /* ★ 明确报错，不静默降级。本实例实测是 http，所以这条不影响现网；
     *   真要用 https 时再复用 PgDlna 里 dlopen libssl 的那套。 */
    if (err) *err = "暂不支持 https（本版本只做 http，见 PgHa.h 说明）";
    return false;
  }

  int fd = tcpConnect(cfg_.host, cfg_.port, 3000, err);
  if (fd < 0) return false;

  std::string req;
  req.reserve(512);
  req += method;
  req += " ";
  req += path;
  req += " HTTP/1.1\r\nHost: ";
  req += cfg_.host;
  req += "\r\nAuthorization: Bearer ";
  req += cfg_.token;
  req += "\r\nUser-Agent: V851s-PocketGame/1.0\r\nAccept: application/json\r\n";
  if (body && *body) {
    char len[32];
    snprintf(len, sizeof(len), "%d", (int)strlen(body));
    req += "Content-Type: application/json\r\nContent-Length: ";
    req += len;
    req += "\r\n";
  }
  req += "Connection: close\r\n\r\n";
  if (body && *body) req += body;

  bool ok = sendAll(fd, req, err);
  std::string raw;
  if (ok) {
    raw.reserve(16384);
    char buf[4096];
    for (;;) {
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n > 0) {
        raw.append(buf, (size_t)n);
        /* 防御：正常响应 < 20KB（实测全量 states 14.4KB）。超了就别再涨了 ——
         * 内存只有 ~16MB 可用，宁可报错也不能让它把进程拖死。 */
        if (raw.size() > 512 * 1024) {
          if (err) *err = "响应超过 512KB，中止（防御性上限）";
          ok = false;
          break;
        }
        continue;
      }
      if (n == 0) break;                       // 对端关闭 = 读完
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* 读超时。已收到的部分仍可用（HA 一般 Content-Length 完整） —— 但要说清楚。 */
        if (raw.empty()) {
          if (err) *err = "读响应超时（8 秒无数据）";
          ok = false;
        } else {
          LOGW("PgHa: 读响应超时，但已收到 %d 字节，按截断处理", (int)raw.size());
        }
        break;
      }
      if (err) {
        char b[96];
        snprintf(b, sizeof(b), "recv 失败：%s", strerror(errno));
        *err = b;
      }
      ok = false;
      break;
    }
  }
  close(fd);
  if (!ok) return false;
  if (raw.empty()) {
    if (err) *err = "响应为空";
    return false;
  }

  /* 拆状态行与响应体。状态行形如 `HTTP/1.1 401 Unauthorized` */
  size_t sp = raw.find(' ');
  if (raw.compare(0, 5, "HTTP/")) {
    // 不是 HTTP 响应（比如被中间设备劫持） ⇒ PROTO_ERR 而不是"离线"
    if (err) *err = "响应不是 HTTP（可能被劫持/代理干扰）";
    return false;
  }
  int code = (sp != std::string::npos) ? atoi(raw.c_str() + sp + 1) : 0;
  if (httpCode) *httpCode = code;
  size_t hdrEnd = raw.find("\r\n\r\n");
  if (hdrEnd != std::string::npos && resp) {
    resp->assign(raw, hdrEnd + 4, std::string::npos);
  }
  return true;
}

// ---------------------------------------------------------------- 状态机

void Ha::setPhase(int p, const char *err) {
  pthread_mutex_lock(&mtx_);
  phase_ = p;
  if (err) {
    copyStr(lastErr_, sizeof(lastErr_), err);
  }
  pthread_mutex_unlock(&mtx_);
}

bool Ha::doOnce(int cmd, const char *arg) {
  callEntity_ = 0;              // 每条命令重新建立上下文，别留上一条的
  callMomentary_ = false;
  /* ★ fakeOffline：QA 用来验 UI 的离线提示与自愈，不用真拔网线。
   *   刻意放在**发起请求之前**，所以它模拟的是"网络层直接失败"。 */
  int fake = 0;
  pthread_mutex_lock(&mtx_);
  fake = fakeOffline_;
  pthread_mutex_unlock(&mtx_);
  if (fake) {
    setPhase(OFFLINE, "（QA 模拟离线）");
    ++errTotal_;
    return false;
  }

  const char *path = "/api/";
  const char *method = "GET";
  std::string body;
  static char pbuf[128];
  /* ★ 控制命令：失败**不能**把全局相位改成 PROTO_ERR/OFFLINE。
   *   理由：一次 `switch.turn_on` 被拒（比如设备离线）不代表这台 HA 连不上——
   *   状态行会变成"响应异常"，而轮询明明是好的，用户看到的是自相矛盾的界面。
   *   ⇒ 控制失败只在 ctlMsg_ 里说话。 */
  const bool isCall = (cmd == CMD_CALL);
  if (cmd == CMD_CONFIG) {
    path = "/api/config";
  } else if (cmd == CMD_STATES || cmd == CMD_POLL) {
    path = "/api/states";
  } else if (cmd == CMD_ENTITY) {
    snprintf(pbuf, sizeof(pbuf), "/api/states/%s", arg ? arg : "");
    path = pbuf;
  } else if (isCall) {
    method = "POST";
    /* arg = "domain/service/entity_id" */
    const char *slash1 = arg ? strchr(arg, '/') : 0;
    const char *slash2 = slash1 ? strchr(slash1 + 1, '/') : 0;
    if (!slash1 || !slash2) {
      LOGW("PgHa: 控制命令参数格式不对（要 domain/service/entity_id）：'%s'", arg ? arg : "");
      completeCallPending(0, false, "命令参数格式不对");
      return false;
    }
    char dom[24] = {0};
    char svc[24] = {0};
    size_t dn = (size_t)(slash1 - arg);
    size_t sn = (size_t)(slash2 - slash1 - 1);
    if (dn >= sizeof(dom)) dn = sizeof(dom) - 1;
    if (sn >= sizeof(svc)) sn = sizeof(svc) - 1;
    memcpy(dom, arg, dn);
    memcpy(svc, slash1 + 1, sn);
    const char *eid = slash2 + 1;
    snprintf(pbuf, sizeof(pbuf), "/api/services/%s/%s", dom, svc);
    path = pbuf;
    body = "{\"entity_id\":\"";
    body += eid;
    body += "\"}";
    callEntity_ = eid;
    callMomentary_ = haDomainMomentary(domainOf(eid));
  }

  int code = 0;
  std::string resp, err;
  ++reqTotal_;
  bool got = fetch(method, path, body.empty() ? 0 : body.c_str(), &code, &resp, &err);
  pthread_mutex_lock(&mtx_);
  lastHttp_ = code;
  pthread_mutex_unlock(&mtx_);

  if (!got) {
    /* 连不上 / 超时 / 协议不对。PROTO_ERR 与 OFFLINE 要分开，
     * 因为"能连上但响应看不懂"和"根本连不上"给用户的建议完全不同。 */
    const bool proto = (err.find("不是 HTTP") != std::string::npos);
    ++errTotal_;
    LOGW("PgHa: %s %s 失败：%s（累计失败 %d 次）", method, path, err.c_str(), errTotal_);
    if (isCall) {
      completeCallPending(0, false, "命令没发出去（网络层失败）");
      return false;
    }
    setPhase(proto ? PROTO_ERR : OFFLINE, err.c_str());
    return false;
  }

  if (code == 401 || code == 403) {
    /* ★ 令牌问题**不重试**（循环里会退避到 60 秒一次）。要说人话。 */
    ++errTotal_;
    ++httpFail_;
    LOGW("PgHa: 认证失败 HTTP %d —— 令牌需要重新生成", code);
    if (isCall) {
      completeCallPending(0, false, "令牌无效（HTTP 401/403）");
      return false;
    }
    setPhase(AUTH_FAIL, "令牌无效或被吊销（HTTP 401/403）");
    return false;
  }
  if (code < 200 || code >= 300) {
    char b[80];
    snprintf(b, sizeof(b), "HTTP %d（%s %s）", code, method, path);
    ++errTotal_;
    ++httpFail_;
    LOGW("PgHa: %s", b);
    if (isCall) {
      completeCallPending(code, false, "命令被拒绝");
      return false;
    }
    setPhase(PROTO_ERR, b);
    return false;
  }

  // ---- 按命令解析 ----
  pthread_mutex_lock(&mtx_);
  lastErr_[0] = 0;
  pthread_mutex_unlock(&mtx_);

  if (cmd == CMD_PING) {
    LOGD("PgHa: /api/ 通（HTTP %d，%d 字节）", code, (int)resp.size());
    return true;
  }
  if (cmd == CMD_CONFIG) {
    applyConfigJson(resp);
    return true;
  }
  if (cmd == CMD_STATES || cmd == CMD_POLL) {
    applyStatesJson(resp);
    return true;
  }
  if (cmd == CMD_ENTITY) {
    /* 单实体：**只更新表里那一条**（P2 的控制回读要用它）。
     * ⚠️ 千万不能走 applyStatesJson —— 那会拿"只有一条"的响应把整表换掉，
     *    表现就是"点完一个开关，列表里其它设备全没了"。 */
    applyOneEntityJson(resp);
    return true;
  }
  if (isCall) {
    /* ★★ 这里**绝不能**按响应内容判成败：实测响应恒为 `[]`，
     *   而且给不存在的实体发命令也是 `200 + []`（与"成功还没回包"无法区分）。
     *   ⇒ HTTP 2xx 只说明"HA 收下了"；真正的成功由 ctlTick() 的回读确认判定。
     *   单向触发域（scene/script/button）没有回读可言，到此就算完成。 */
    LOGD("PgHa: 服务调用 2xx（%s）响应 %d 字节：%.80s", path, (int)resp.size(), resp.c_str());
    if (callMomentary_) {
      completeCallPending(code, true, "已发送");
    }
    return true;
  }
  return true;
}

/** 把 `/api/states/<id>` 的单条响应合并进实体表（找到就更新，找不到就追加）。 */
void Ha::applyOneEntityJson(const std::string &body) {
  json::Doc doc;
  if (!doc.parse(body)) {
    ++parseFail_;
    LOGW("PgHa: 单实体响应解析失败：%s", doc.err().c_str());
    return;
  }
  int e = doc.root();
  if (doc.type(e) != json::T_OBJ) {
    ++parseFail_;
    setPhase(PROTO_ERR, "单实体响应不是对象");
    return;
  }
  const char *id = doc.memberStr(e, "entity_id", "");
  if (!id[0]) {
    ++parseFail_;
    LOGW("PgHa: 单实体响应里没有 entity_id");
    return;
  }
  const int dom = domainOf(id);

  pthread_mutex_lock(&mtx_);
  bool found = false;
  for (size_t i = 0; i < ents_.size(); ++i) {
    if (strcmp(ents_[i].id, id) != 0) continue;
    copyStr(ents_[i].state, sizeof(ents_[i].state), doc.memberStr(e, "state", ""));
    int at = doc.member(e, "attributes");
    if (at >= 0) {
      const char *nm = doc.memberStr(at, "friendly_name", "");
      if (nm[0]) copyStr(ents_[i].name, sizeof(ents_[i].name), nm);
      const char *un = doc.memberStr(at, "unit_of_measurement", "");
      copyStr(ents_[i].unit, sizeof(ents_[i].unit), un);
    }
    found = true;
    break;
  }
  if (!found) {
    /* 表里没有（比如之前被 domainWanted 挡掉）：塞进去，但别越过硬上限 */
    if ((int)ents_.size() >= kHaMaxEntity) {
      LOGW("PgHa: 单实体 %s 想入表但已到上限 %d，丢弃", id, kHaMaxEntity);
    } else {
      HaEntity he;
      he.domain = (unsigned char)dom;
      copyStr(he.id, sizeof(he.id), id);
      copyStr(he.state, sizeof(he.state), doc.memberStr(e, "state", ""));
      int at = doc.member(e, "attributes");
      if (at >= 0) {
        copyStr(he.name, sizeof(he.name), doc.memberStr(at, "friendly_name", ""));
        copyStr(he.unit, sizeof(he.unit), doc.memberStr(at, "unit_of_measurement", ""));
      }
      if (!he.name[0]) copyStr(he.name, sizeof(he.name), id);
      ents_.push_back(he);
    }
  }
  ++generation_;
  pthread_mutex_unlock(&mtx_);
  LOGD("PgHa: 单实体已更新 %s", id);
}

// ---------------------------------------------------------------- JSON 映射

void Ha::applyConfigJson(const std::string &body) {
  json::Doc doc;
  if (!doc.parse(body)) {
    ++parseFail_;
    LOGW("PgHa: /api/config 解析失败：%s", doc.err().c_str());
    return;
  }
  int r = doc.root();
  const char *ver = doc.memberStr(r, "version", "");
  const char *loc = doc.memberStr(r, "location_name", "?");
  int us = doc.member(r, "unit_system");
  const char *temp = (us >= 0) ? doc.memberStr(us, "temperature", "?") : "?";
  pthread_mutex_lock(&mtx_);
  copyStr(haVersion_, sizeof(haVersion_), ver);
  copyStr(unitSystem_, sizeof(unitSystem_), temp);
  pthread_mutex_unlock(&mtx_);
  LOGD("PgHa: HA 版本=%s 位置=%s 温度单位=%s（components=%d）", ver, loc, temp,
       doc.size(doc.member(r, "components")));
}

/* 列表排序权重（**越小越靠前**）。
 *
 * 为什么必须排（2026-09-23 真机看到的）：HA 返回的 34 条里排在最前面的是
 * `sensor.sun_next_dawn` / `sensor.backup_*` 这些**HA 自己的内务实体**，
 * 而真正的设备（switch/light/button）排在第 12 条之后 —— 屏幕上头 6 行全是
 * "Sun Next dawn" 之类，当遥控器看毫无用处。
 * ⇒ 可控设备优先、只读传感器垫底；同档内按名字排，保证顺序稳定
 *   （HA 的返回顺序不保证稳定，不排的话每轮刷新列表都可能跳动）。 */
static int entityRank(const HaEntity &e) {
  if (haDomainControllable(e.domain)) {
    return e.isOnOff() ? 0 : 1;      // 能拨且状态明确的最靠前；按钮类（state=时间戳）次之
  }
  if (e.domain == HD_SENSOR || e.domain == HD_BINARY_SENSOR) return 3;
  return 2;
}

static bool entityLess(const HaEntity &a, const HaEntity &b) {
  int ra = entityRank(a);
  int rb = entityRank(b);
  if (ra != rb) return ra < rb;
  return strcmp(a.name, b.name) < 0;
}

/* `/api/states` 的响应是对象数组，每条：
 *   { "entity_id": "...", "state": "...", "attributes": { "friendly_name": "...",
 *     "unit_of_measurement": "..." }, ... }
 * 我们**只留想要的域**（见 domainWanted）：实测 34 个实体里 sun/zone/tts/conversation
 * 这类内务实体占了一大半，全列出来会把真正的设备淹掉。 */
void Ha::applyStatesJson(const std::string &body) {
  json::Doc doc;
  if (!doc.parse(body)) {
    ++parseFail_;
    setPhase(PROTO_ERR, "states 响应不是合法 JSON");
    LOGW("PgHa: /api/states 解析失败：%s（前 120 字节：%.120s）", doc.err().c_str(),
         body.c_str());
    return;
  }
  int r = doc.root();
  if (doc.type(r) != json::T_ARR) {
    ++parseFail_;
    setPhase(PROTO_ERR, "states 响应不是数组");
    LOGW("PgHa: /api/states 不是数组（type=%d）", doc.type(r));
    return;
  }

  std::vector<HaEntity> v;
  v.reserve(64);
  int n = doc.size(r);
  int dropped = 0;
  for (int i = 0; i < n; ++i) {
    int e = doc.at(r, i);
    if (doc.type(e) != json::T_OBJ) continue;
    const char *id = doc.memberStr(e, "entity_id", "");
    if (!id[0]) continue;
    int dom = domainOf(id);
    if (!domainWanted(dom)) continue;
    if ((int)v.size() >= kHaMaxEntity) {
      ++dropped;      // ★ 不静默丢：下面会 WARN
      continue;
    }
    HaEntity he;
    he.domain = (unsigned char)dom;
    copyStr(he.id, sizeof(he.id), id);
    copyStr(he.state, sizeof(he.state), doc.memberStr(e, "state", ""));
    int at = doc.member(e, "attributes");
    if (at >= 0) {
      copyStr(he.name, sizeof(he.name), doc.memberStr(at, "friendly_name", ""));
      copyStr(he.unit, sizeof(he.unit), doc.memberStr(at, "unit_of_measurement", ""));
    }
    if (!he.name[0]) copyStr(he.name, sizeof(he.name), id);   // 没名字就退回 entity_id
    v.push_back(he);
  }

  /* ★ 排序（理由见上面 entityLess 的注释）：不排的话屏幕头几行全是
   *   "Sun Next dawn"/"Backup …" 这类内务传感器，当遥控器完全没法用。 */
  std::sort(v.begin(), v.end(), entityLess);

  pthread_mutex_lock(&mtx_);
  ents_.swap(v);
  ++generation_;
  dropped_ = dropped;
  pthread_mutex_unlock(&mtx_);

  if (dropped > 0) {
    LOGW("PgHa: 实体数超过上限 %d，丢弃 %d 条（要提上限就改 PgHa.h 的 kHaMaxEntity，"
         "别让它静默丢）", kHaMaxEntity, dropped);
  }
  LOGD("PgHa: 实体表已更新 #%u —— %d 条（响应 %d 字节，HA 返回 %d 条）",
       generation_, (int)entityCount(), (int)body.size(), n);
}

// ---------------------------------------------------------------- 读状态

int Ha::phase() const {
  pthread_mutex_lock(&mtx_);
  int p = phase_;
  pthread_mutex_unlock(&mtx_);
  return p;
}

const char *Ha::phaseText() const {
  switch (phase()) {
    case CONNECTING: return "连接中";
    case ONLINE: return "已连接";
    case AUTH_FAIL: return "令牌无效";
    case PROTO_ERR: return "响应异常";
    default: return "未连接";
  }
}

const char *Ha::phaseHint() const {
  switch (phase()) {
    case CONNECTING: return "正在连接 Home Assistant…";
    case ONLINE: return "数据来自 Home Assistant";
    case AUTH_FAIL: return "令牌无效或被吊销，请在配置页换一个长期访问令牌";
    case PROTO_ERR: return "能连上但响应看不懂，检查 HA 版本或地址";
    default: return "连不上 HA，检查网络与地址（配置页可改）";
  }
}

int Ha::entityCount() const {
  pthread_mutex_lock(&mtx_);
  int n = (int)ents_.size();
  pthread_mutex_unlock(&mtx_);
  return n;
}

unsigned Ha::generation() const {
  pthread_mutex_lock(&mtx_);
  unsigned g = generation_;
  pthread_mutex_unlock(&mtx_);
  return g;
}

void Ha::copyEntities(std::vector<HaEntity> *out) const {
  if (!out) return;
  pthread_mutex_lock(&mtx_);
  *out = ents_;
  pthread_mutex_unlock(&mtx_);
}

const char *Ha::haVersion() const {
  return haVersion_;      // 只在工作线程写、UI 读；单字节串，容错足够
}

const char *Ha::unitSystem() const { return unitSystem_; }

const char *Ha::lastErr() const { return lastErr_; }

int Ha::lastHttpCode() const {
  pthread_mutex_lock(&mtx_);
  int c = lastHttp_;
  pthread_mutex_unlock(&mtx_);
  return c;
}

long Ha::lastOkMs() const {
  pthread_mutex_lock(&mtx_);
  long t = lastOkMs_;
  pthread_mutex_unlock(&mtx_);
  return t;
}

long Ha::nowMs() const { return nowMsImpl(); }

int Ha::reqTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = reqTotal_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

int Ha::errTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = errTotal_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

int Ha::httpFailTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = httpFail_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

int Ha::parseFailTotal() const {
  pthread_mutex_lock(&mtx_);
  int v = parseFail_;
  pthread_mutex_unlock(&mtx_);
  return v;
}

void Ha::fakeOffline(bool on) {
  pthread_mutex_lock(&mtx_);
  fakeOffline_ = on ? 1 : 0;
  if (on) phase_ = OFFLINE;
  pthread_mutex_unlock(&mtx_);
  LOGD("PgHa: 模拟离线 = %d（QA 用，不碰网络）", on ? 1 : 0);
}

bool Ha::applyCfg(const HaCfg &c) {
  bool wasRunning = running_ != 0;
  if (wasRunning) stop();
  cfg_ = c;
  bool saved = cfg_.save();
  if (!saved) {
    // 存不下也要能当场生效（现场调试的常态），但必须说清楚
    LOGW("PgHa: 配置没能写进 /data/ha.conf，本次运行有效但重启后会丢");
  }
  bool started = start();
  LOGD("PgHa: 应用配置 host=%s port=%d poll=%d -> 启动=%d", cfg_.host, cfg_.port,
       cfg_.pollSec, started ? 1 : 0);
  return started;
}

void Ha::dumpTo(std::string *out, int maxEntities) const {
  if (!out) return;
  char b[256];
  snprintf(b, sizeof(b),
           "PgHa 状态: phase=%s(%d) http=%d ver=%s unit=%s 实体=%d gen=%u "
           "请求=%d 失败=%d httpFail=%d parseFail=%d dropped=%d 最近成功=%ldms前\n",
           phaseText(), phase(), lastHttpCode(), haVersion(), unitSystem(),
           entityCount(), generation(), reqTotal(), errTotal(), httpFailTotal(),
           parseFailTotal(), dropped_,
           lastOkMs() ? (nowMsImpl() - lastOkMs()) : -1L);
  out->append(b);
  if (lastErr()[0]) {
    out->append("  最近错误: ");
    out->append(lastErr());
    out->append("\n");
  }
  /* ★ 控制段：这是"点了有没有反应"的唯一可判据的地方（UI 上的转圈/回滚
   *   在抓屏里看不出来，只能靠这几个数字 + ctlMsg）。 */
  {
    char pend[200] = {0};
    int n = 0;
    pthread_mutex_lock(&mtx_);
    for (int i = 0; i < kHaMaxPending; ++i) {
      if (!pend_[i].used) continue;
      long age = nowMsImpl() - pend_[i].startMs;
      char one[48];
      snprintf(one, sizeof(one), "%s%s->%s(%ldms)", n ? " " : "", pend_[i].id,
               pend_[i].target[0] ? pend_[i].target : "单向", age);
      strncat(pend, one, sizeof(pend) - strlen(pend) - 1);
      ++n;
    }
    pthread_mutex_unlock(&mtx_);
    snprintf(b, sizeof(b),
             "  控制: 发出=%d 确认成功=%d 失败回滚=%d 在途=%d\n",
             cmdTotal(), ctlOkTotal(), cmdFailTotal(), n);
    out->append(b);
    if (n) {
      out->append("  在途: ");
      out->append(pend);
      out->append("\n");
    }
    if (ctlMsg()[0]) {
      out->append("  最近控制: ");
      out->append(ctlMsg());
      out->append("\n");
    }
    if (ctlReject()[0]) {
      out->append("  ★上次控制被拒的原因: ");
      out->append(ctlReject());
      out->append("\n");
    }
  }
  std::vector<HaEntity> v;
  copyEntities(&v);
  int n = (int)v.size();
  if (maxEntities > 0 && n > maxEntities) n = maxEntities;
  for (int i = 0; i < n; ++i) {
    snprintf(b, sizeof(b), "  [%d] %-42s dom=%-14s state=%-12s name=%s%s%s\n", i,
             v[i].id, haDomainName(v[i].domain), v[i].state, v[i].name,
             v[i].unit[0] ? " " : "", v[i].unit);
    out->append(b);
  }
  if ((int)v.size() > n) {
    snprintf(b, sizeof(b), "  …（共 %d 条，只列前 %d 条）\n", (int)v.size(), n);
    out->append(b);
  }
}

}  // namespace pg
