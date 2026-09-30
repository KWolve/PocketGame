#pragma once
/*
 * PgHa.h - Home Assistant 客户端（智能家居接入的网络层）
 *
 * 依据：docs/ha-integration-plan.md + docs/ha-probe.md + docs/ha-verify.md（都是真机实测）。
 * 本文件负责**除 UI 之外的全部**：配置、HTTP、JSON 映射、实体表、连接状态机。
 *
 * ★★ 线程模型（与 PgDlna / PgLan 完全一致，别自创）：
 *    · **单工作线程**做全部阻塞 IO（connect / send / recv），**绝不碰任何控件**，
 *      连 LOGD 之外什么都不做；
 *    · UI → 工作线程：`reqXxx()` 往**命令环形队列**里塞一条（互斥锁 + 条件变量）；
 *    · 工作线程 → UI：把结果写进**加锁的实体表**，并让 `generation_` 自增；
 *      UI 每拍用 `generation()` 比对 —— **变了才 copyEntities()**（照抄 PgLan 的做法，
 *      否则每拍拷 34KB 会拖垮 UI）。
 *    · `stop()` 要 join 可 join 的线程、关 socket（PgDlna 那边有 use-after-free 的教训）。
 *
 * ★★ 实测得到的三条硬约束（**别按直觉改**，见 docs/ha-verify.md）：
 *    ① `POST /api/services/...` 的响应**恒为空数组 `[]`** —— 这台 HA 的设备是 MQTT 驱动的，
 *       状态变化是**异步回包**才发生的 ⇒ **不能靠响应回填**。控制必须"乐观态 + 短轮询确认"。
 *    ② **给不存在的实体发命令也是 `200 + []`**，与"成功但还没回包"无法区分
 *       ⇒ **"成功"的定义 = 回读到目标状态**，不是 HTTP 200。
 *    ③ `unavailable` 实体的命令**会被吞**（200 但状态永久不变）⇒ 灰态要禁点，
 *       "回读超时"是**常态路径**而不是异常。
 *
 * ⚠️ 本期（P1）只做 http；`tls=true` 会**明确报错**而不是静默降级
 *    （本实例是 http://192.168.1.188:8123，实测 401 受控应答已通）。
 *    TLS 可复用 PgDlna 里 dlopen libssl 的那套（见其 pgDlnaTlsDiag），留到需要时再做。
 */

#include <pthread.h>
#include <string>
#include <vector>

namespace pg {

/** 本板屏 480x800，一屏列表远放不下 34 个实体；上限**不写死**在列表逻辑里，
 *  但底层存储要有硬上限（防畸形/超大 HA 把内存撑爆）—— 超了必须 WARN，不能静默丢。 */
const int kHaMaxEntity = 256;

/** 命令环形队列长度（UI 点不出那么快；满了要 WARN，不静默丢）。
 *  头尾两处（这里是容量真值，.cpp 里用同一个常量）—— 别再各写一个数。 */
const int kHaQSize = 8;

/** 队列里一条命令的参数字节数。
 *  ⚠️ 控制命令的 arg 是 `domain/service/entity_id` 拼串 —— 实测最长的实体名
 *  `switch.z20_smart_panel_ke_ting_deng` 就 33 字节，加上域/服务名要留够。
 *  原来这里是 64，控制一上来就**静默截断**（会发出一条指向错实体的命令）。 */
const int kHaArgMax = 128;

/** 同时在途的控制命令上限。UI 一屏 6 个磁贴，点不出更多。 */
const int kHaMaxPending = 6;

/** 控制超时（毫秒）：从"命令发出"算起，这段时间内没回读到目标状态就算失败并回滚。
 *  实测端到端 156~210ms、设备侧回包 ~90ms ⇒ 5 秒是"给足余量但不至于让用户干等"。 */
const int kHaCtlTimeoutMs = 5000;

/** 在途控制时的回读节拍（毫秒）。走 `GET /api/states/<id>`（几十字节），
 *  不是全量 states（14KB）—— 5 秒里轮 20 次也不会把内存/带宽吃出问题。 */
const int kHaCtlPollMs = 250;

enum HaDomain {
  HD_OTHER = 0,
  HD_LIGHT,
  HD_SWITCH,
  HD_SENSOR,
  HD_BINARY_SENSOR,
  HD_SCENE,
  HD_SCRIPT,
  HD_AUTOMATION,
  HD_BUTTON,
  HD_CLIMATE,
  HD_COVER,
  HD_MEDIA_PLAYER,
  HD_FAN,
  HD_LOCK,
  HD_CAMERA,
  HD_INPUT_BOOLEAN,
  HD_NUMBER,
  HD_SELECT,
  HD_VACUUM,
  HD_COUNT,
};

const char *haDomainName(int d);

/** 这个域能不能"点"（可控）。只读域（sensor/binary_sensor/camera）返回 false。
 *  ⚠️ 还要叠加"当前 state 是不是 on/off"再决定要不要禁点 —— 见 §3 实测：
 *     unavailable 的实体命令会被吞。 */
bool haDomainControllable(int d);

/** ★ 单向触发域（scene / script / button）：命令发出去就"结束"，
 *  它们的 state **不会**因此变成 on/off（scene 的 state 永远是场景名或 unknown）。
 *  ⇒ 这类**不做回读确认**（做了必然 5 秒超时），只判 HTTP 通不通。
 *  代价：设备真的离线时，界面不会知道 —— 所以它另给一条"命令已发出"的短提示。 */
bool haDomainMomentary(int d);

/** 把"域 + 当前状态"翻译成要调用的服务名（turn_on / turn_off / press / …）。
 *  返回 false = 这个域不能翻转（只读域）。 */
bool haToggleServiceFor(int domain, const char *curState, char *svcOut, int n);

/** 磁贴/图标的三档状态（UI 换图用）。**
 *  实测这台 HA 34 个实体里只有 4 个是真 on/off，其余是 unavailable / unknown /
 *  数值 ⇒ 三档之外的信息全部由**文字**表达，图片只用"亮 / 灭 / 灰"说话。 */
enum HaTileState {
  HTS_OFF = 0,   // 明确 off
  HTS_ON = 1,    // 明确 on
  HTS_FLAT = 2,  // 离线(unavailable/unknown) 或 非开关态（数值/文本）
};
int haTileStateOf(const char *state);

/** 磁贴图标文件名（含 `images/` 前缀，可**直接**丢给 setBackgroundPic）。
 *  ⚠️ setBackgroundPic 只收路径指针 ⇒ out 必须是**调用方持有的长寿命缓冲**
 *     （static / 成员数组），指向栈上临时串会悬垂（navibar 的血案）。
 *  命名：`images/ha_tile_<域>_<on|off|flat>.png`，由 tools/gen_ha_tiles.py 生成。 */
void haTilePicName(int domain, int tileState, char *out, int n);

/** 一个实体（**定长**：UI 侧快照会整体拷贝，不做堆分配） */
struct HaEntity {
  char id[64];       // entity_id
  char state[40];    // 状态原文（on / off / 26.5 / unavailable / unknown …）
  char name[48];     // friendly_name（UTF-8，可能是中文）
  char unit[12];     // unit_of_measurement
  unsigned char domain;

  HaEntity() : domain(HD_OTHER) {
    id[0] = 0;
    state[0] = 0;
    name[0] = 0;
    unit[0] = 0;
  }
  bool isOnOff() const { return state[0] && (strEq("on") || strEq("off")); }
  bool isOfflineState() const { return strEq("unavailable") || strEq("unknown"); }
  bool controllable() const {
    return haDomainControllable(domain) && isOnOff();
  }
  bool strEq(const char *s) const;
};

/** 连接配置（持久到 /data/ha.conf） */
struct HaCfg {
  char host[64];
  int port;
  bool tls;
  char token[256];
  int pollSec;      // 轮询间隔（秒），0 = 不自动轮询

  HaCfg();
  /** 按顺序尝试 /data/ha.conf（持久）、/tmp/ha.conf（临时，adb push 调试用） */
  bool load();
  bool save() const;
  /** 把 "http://192.168.1.188:8123" 拆进 host/port/tls */
  void parseUrl(const char *url);
  void url(char *out, int n) const;
};

class Ha {
 public:
  enum Phase {
    OFFLINE = 0,   // 没联通（网络不可达 / 拒绝 / 超时）
    CONNECTING,    // 正在连（还没拿到第一个成功响应）
    ONLINE,        // 有成功响应，且最近一次不早于 3 个轮询周期
    AUTH_FAIL,     // 401/403 —— token 无效或被吊销。**不重试**，要用户去改
    PROTO_ERR,     // 连上了但响应不是我们能懂的东西（版本/接口不符）
  };

  static Ha &inst();

  /** 读配置 + 起工作线程。可重复调用（已在跑就直接返回 true）。
   *  配置缺失/为空 ⇒ 返回 false 并且不建线程（UI 应当引导去配置页）。 */
  bool start();
  void stop();
  bool running() const { return running_ != 0; }

  // ---------------- UI 线程 → 工作线程（异步，立即返回） ----------------
  void reqPing();                          // GET /api/          （连通性）
  void reqConfig();                        // GET /api/config    （版本/单位制）
  void reqStates();                        // GET /api/states    （全量，34 个实体用得起）
  void reqEntity(const char *entityId);    // GET /api/states/<id>

  /** 请求立刻轮询一次（UI 手动刷新按钮用） */
  void reqPollNow() { reqStates(); }

  // ---------------- ★★ 控制（P2） ----------------
  /** 翻转一个实体（UI 线程调用，立刻返回）。
   *
   *  内部把实测得到的三条硬约束一次做掉（见文件头）：
   *    ① 先判**能不能点**（只读域 / 非 on/off 的灰态 / 已有在途命令 ⇒ 直接拒，返回 false）
   *       —— 因为给 `unavailable` 实体发命令是 **200 但状态永久不变**，点了没反应最像 bug；
   *    ② 记一条**乐观态**（pending，期望状态 = 目标值），UI 立刻按它画"已开/已关 + 转圈"；
   *    ③ 之后**短轮询回读确认**（`GET /api/states/<id>`，250ms 一次）：
   *       - 读到目标值 ⇒ 成功，清 pending；
   *       - 5 秒没读到 ⇒ 失败回滚，清 pending + 记 `ctlMsg()`（UI 显示"设备无应答"）。
   *
   *  ⚠️ **绝不拿服务响应判断成败**：响应恒为 `[]`，而且**给不存在的实体发命令也是
   *     `200 + []`**（与"成功但还没回包"完全无法区分）。
   *
   *  单向触发域（scene/script/button）不做回读 —— 它们的 state 不会变成 on/off。
   */
  bool reqToggle(const char *entityId);

  /** 在途控制查询（UI 画乐观态用）。返回 1 = 有在途命令；targetOut 填**期望状态**
   *  （单向触发域填 ""）。返回 0 = 没有。 */
  int ctlPending(const char *entityId, char *targetOut, int n) const;

  /** 最近一次控制的结果说明（成功/已发出/超时回滚/被拒）。
   *  UI 拿它显示在提示行 —— 这是"点了没反应"唯一的解释来源。"" = 暂无。 */
  const char *ctlMsg() const;

  /** ★ **reqToggle 为什么被拒**（每次返回 false 都会写一条人话原因）。
   *  加它的理由：第一版 reqToggle 的每条失败分支只打 LOGD/LOGW，
   *  而**本板 logcat 缓冲只有十几行**、被轮询/SSDP 刷掉 ⇒ 现场"点了没反应"
   *  时**根本查不到原因**（实测：只看到 `reqToggle(...) 返回 false`，原因那行没了）。
   *  ⇒ 失败原因必须能进 QA dump（判据落文件），不能只依赖日志。"" = 没有过失败。 */
  const char *ctlReject() const;

  /** 控制计数（QA 判据：证明命令真的发出去了 / 失败了几次） */
  int cmdTotal() const;
  int cmdFailTotal() const;
  int ctlOkTotal() const;

  // ---------------- UI 线程读状态 ----------------
  int phase() const;
  const char *phaseText() const;           // 中文，直接给 UI 显示
  const char *phaseHint() const;           // 给用户的一句"该怎么办"
  int entityCount() const;
  /** 实体表版本号：变了才需要 copyEntities() */
  unsigned generation() const;
  void copyEntities(std::vector<HaEntity> *out) const;

  const char *haVersion() const;           // "2026.9.3"
  const char *unitSystem() const;          // 温度单位（°C/°F）—— 实测这台是 US customary
  const char *lastErr() const;
  int lastHttpCode() const;
  long lastOkMs() const;                   // 最近一次成功响应时刻（0 = 还没有过）
  long nowMs() const;

  // 累计计数（QA 判据用：证明"请求真的发出去了"）
  int reqTotal() const;
  int errTotal() const;
  int httpFailTotal() const;               // HTTP 4xx/5xx 次数
  int parseFailTotal() const;              // JSON 解析失败次数

  // ---------------- 配置（UI 线程） ----------------
  const HaCfg &cfg() const { return cfg_; }
  /** 存盘（/data/ha.conf）+ 重新解析 + 重启工作线程。返回是否成功。 */
  bool applyCfg(const HaCfg &c);

  /** 给 QA 用的文本快照（写进 /tmp 或打日志，自动化验收的判据） */
  void dumpTo(std::string *out, int maxEntities) const;

  /** 模拟"网络断开"时的行为（QA `ha fakeoff`）：只改状态机，不碰网络。
   *  用来验 UI 的离线提示与自愈，不用真拔网线。 */
  void fakeOffline(bool on);

 private:
  Ha();
  ~Ha();
  Ha(const Ha &);
  Ha &operator=(const Ha &);

  // ---- 命令 ----
  enum Cmd { CMD_NONE = 0, CMD_PING, CMD_CONFIG, CMD_STATES, CMD_ENTITY, CMD_POLL,
             CMD_CALL };   // CMD_CALL: arg = "domain/service/entity_id"
  struct Item {
    int cmd;
    char arg[kHaArgMax];
    Item() : cmd(CMD_NONE) { arg[0] = 0; }
  };
  bool push(int cmd, const char *arg);

  /** 在途控制记录（乐观态）。**全部在 mtx_ 保护下访问。** */
  struct Pending {
    bool used;
    char id[64];
    char target[16];    // 期望状态；单向触发域是 ""
    long startMs;
    long deadlineMs;    // 0 = 单向触发（不等回读）
    Pending() : used(false), startMs(0), deadlineMs(0) {
      id[0] = 0;
      target[0] = 0;
    }
  };

  /** 控制超时/回读的每拍结算（工作线程里调）。 */
  void ctlTick();
  /** 一条控制命令"当场"结算：**只用于失败**，以及单向触发域的成功。
   *  非单向域的成功**不在这里结算** —— 它的成功只能由回读确认（见 ctlTick）。 */
  void completeCallPending(int httpCode, bool ok, const char *what);
  /** 锁内取实体名（找不到就回 id），给日志/提示行用。out 需 ≥48 字节。 */
  void entityNameLocked(const char *id, char *out, int n) const;
  /** 锁内：找一个还没结算的 pending，把它要回读的实体名写进 out。
   *  返回 false = 没有在途控制。 */
  bool ctlReadbackLocked(char *out, int n) const;
  /** 锁内：查某个实体有没有在途命令。 */
  bool ctlPendingLocked(const char *entityId, char *targetOut, int n) const;

  // ---- 工作线程 ----
  static void *threadEntry(void *self);
  void loop();
  bool doOnce(int cmd, const char *arg);
  bool fetch(const char *method, const char *path, const char *body,
             int *httpCode, std::string *resp, std::string *err);

  void setPhase(int p, const char *err);
  void applyConfigJson(const std::string &body);
  void applyStatesJson(const std::string &body);
  /** 单条实体的响应合并进表（**不能**走 applyStatesJson，否则整表会被一条换掉） */
  void applyOneEntityJson(const std::string &body);

  // ---- 状态 ----
  mutable pthread_mutex_t mtx_;
  pthread_cond_t cv_;
  pthread_t tid_;
  int tidValid_;
  volatile int running_;
  volatile int stopReq_;

  Item q_[kHaQSize];   // 命令环形队列（满了 WARN 后丢弃）
  int qHead_;
  int qTail_;

  HaCfg cfg_;
  int phase_;
  int lastHttp_;
  long lastOkMs_;
  long nextPollMs_;
  int backoffMs_;
  int fakeOffline_;
  int reqTotal_;
  int errTotal_;
  int httpFail_;
  int parseFail_;

  char haVersion_[24];
  char unitSystem_[12];
  char lastErr_[160];

  std::vector<HaEntity> ents_;    // 加锁保护；UI 通过 copyEntities 拿副本
  unsigned generation_;
  int dropped_;                   // 超上限被丢掉的实体数（>0 时必须 WARN）

  Pending pend_[kHaMaxPending];
  char ctlMsg_[160];              // 最近一次控制结果（UI 提示行）
  char ctlReject_[200];           // 最近一次 reqToggle 被拒的原因（QA dump 用）
  int cmdTotal_;
  int cmdFail_;
  int ctlOk_;
  /* 当前正在发的这条控制命令的上下文（工作线程自用，单线程访问不需要锁） */
  const char *callEntity_;
  bool callMomentary_;
};

}  // namespace pg
