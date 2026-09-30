/*
 * PgPoem.cpp - 屏保"每日诗词"实现。设计说明见 PgPoem.h。
 */
#include "platform/PgPoem.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include "platform/PgFf.h"
#include "utils/Log.h"

namespace pg {

namespace {

const char *kDefaultUrl = "https://v2.jinrishici.com/one.json";
const int kFetchPerDay = 6;          // 每天抓几首（当天就在这批里换）
const int kFetchTimeoutMs = 8000;
const int kMaxPool = 40;             // 池子上限（本板内存小，别无限涨）
const int kTickThrottleMs = 900;     // tick() 内部节流：最快 ~1s 一次

const char *kUserAgent =
    "Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Mobile";

/* ============================ 题材归类表 ============================
 * 接口给不出可靠的题材，而且"不喜欢的题材不再推送"要求离线也成立
 * ⇒ 本地按关键词归类。命中多个时按表序取胜（越靠前的题材越"具体"）。 */
struct TagRule {
  const char *tag;
  const char *keys[8];
};
const TagRule kTagRules[] = {
    {"边塞", {"塞", "关", "征战", "沙场", "胡", "戍", "军", "长城"}},
    {"送别", {"送", "别", "离", "饯", "赠", "帆", "折柳"}},
    {"思乡", {"故乡", "思乡", "归", "乡", "家书", "天涯", "客", "羁旅"}},
    {"爱情", {"相思", "红颜", "佳人", "情人", "锦瑟", "鸳鸯", "闺", "楼台"}},
    {"节日", {"元日", "中秋", "重阳", "清明", "七夕", "除夕", "端午", "岁除"}},
    {"田园", {"田园", "农", "桑", "锄", "村", "牧", "稻", "麦"}},
    {"山水", {"山", "水", "云", "江", "月", "风", "雨", "瀑"}},
    {"咏物", {"梅", "兰", "竹", "菊", "荷", "柳", "蝉", "松"}},
    {"愁绪", {"愁", "恨", "孤", "寂", "空", "残", "寒", "泪"}},
    {"励志", {"志", "壮", "豪", "少年", "丈夫", "策", "青云", "功名"}},
};

std::string classify(const Poem &p) {
  std::string hay = p.title + p.text;
  for (unsigned i = 0; i < sizeof(kTagRules) / sizeof(kTagRules[0]); ++i) {
    const TagRule &r = kTagRules[i];
    for (unsigned k = 0; k < sizeof(r.keys) / sizeof(r.keys[0]) && r.keys[k]; ++k) {
      if (hay.find(r.keys[k]) != std::string::npos) return r.tag;
    }
  }
  return "其他";
}

/* ============================ 小工具 ============================ */

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

/* 把 UTF-8 文本里的制表/换行替换掉，避免破坏"一行一首诗"的落盘格式 */
std::string oneLine(const std::string &s) {
  std::string o = s;
  for (size_t i = 0; i < o.size(); ++i)
    if (o[i] == '\t' || o[i] == '\n' || o[i] == '\r') o[i] = ' ';
  return trim(o);
}

bool contains(const std::vector<std::string> &v, const std::string &s) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i] == s) return true;
  return false;
}

void addUnique(std::vector<std::string> &v, const std::string &s) {
  if (!s.empty() && !contains(v, s)) v.push_back(s);
}

long nowMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int todayKey() {
  time_t t = time(0);
  struct tm tmv;
  localtime_r(&t, &tmv);
  return (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
}

/* ============================ 极简 JSON 取值 ============================
 * 只处理本接口用到的形态（`"key":"value"` / `"key":[...]`），够用且零依赖：
 * 本工程没有 json 包（Manifest 里也没有），为 6 个字段引一个库不划算。 */
std::string jsonStr(const std::string &j, const char *key, size_t from = 0) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k, from);
  if (p == std::string::npos) return "";
  p = j.find(':', p + k.size());
  if (p == std::string::npos) return "";
  p = j.find('"', p);
  if (p == std::string::npos) return "";
  std::string out;
  for (size_t i = p + 1; i < j.size(); ++i) {
    char c = j[i];
    if (c == '\\' && i + 1 < j.size()) {  // \" \\ \n \uXXXX（\u 直接跳过转义，中文一般不走它）
      char n = j[++i];
      if (n == 'n') out += '\n';
      else if (n == 't') out += ' ';
      else if (n == 'r') out += ' ';
      else if (n == 'u') { i += 4; }
      else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }
  return trim(out);
}

/* 取 `"key":[ "a", "b" ]` 里所有字符串，拼成一行 */
std::string jsonArrJoin(const std::string &j, const char *key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = j.find(k);
  if (p == std::string::npos) return "";
  p = j.find('[', p);
  if (p == std::string::npos) return "";
  size_t e = j.find(']', p);
  if (e == std::string::npos) e = j.size();
  std::string out;
  for (size_t i = p; i < e; ++i) {
    if (j[i] != '"') continue;
    std::string s;
    for (size_t m = i + 1; m < e; ++m) {
      char c = j[m];
      if (c == '\\' && m + 1 < e) { s += j[m + 1]; ++m; continue; }
      if (c == '"') { i = m; break; }
      s += c;
    }
    out += trim(s);
  }
  return out;
}

/* ============================ HTTP(S) 拉取 ============================
 * 复用 ffmpeg 的 avio（http/https/TLS/证书链路已经编进去了，见 PgFf）。
 * 自己再引 OpenSSL 只会多一份体积和一堆证书坑。 */
bool httpFetch(const std::string &url, const std::string &extraHeaders,
               std::string *out, std::string *err) {
  Ff::ensureReady();
  AVIOContext *avio = 0;
  AVDictionary *opt = 0;
  char tmo[32];
  snprintf(tmo, sizeof(tmo), "%d", kFetchTimeoutMs * 1000);  // rw_timeout 单位是微秒
  av_dict_set(&opt, "rw_timeout", tmo, 0);
  av_dict_set(&opt, "user_agent", kUserAgent, 0);
  if (!extraHeaders.empty()) av_dict_set(&opt, "headers", extraHeaders.c_str(), 0);
  if (Ff::caPath() && Ff::caPath()[0]) av_dict_set(&opt, "ca_file", Ff::caPath(), 0);

  int r = avio_open2(&avio, url.c_str(), AVIO_FLAG_READ, 0, &opt);
  if (opt) av_dict_free(&opt);
  if (r < 0) {
    char eb[96];
    av_strerror(r, eb, sizeof(eb));
    if (err) *err = std::string("打开失败: ") + eb;
    return false;
  }
  out->clear();
  uint8_t buf[4096];
  while (out->size() < 64 * 1024) {
    int n = avio_read(avio, buf, sizeof(buf));
    if (n > 0) { out->append((const char *)buf, (size_t)n); continue; }
    if (n == AVERROR_EOF) break;
    if (!out->empty()) break;  // 收尾报错但数据已拿到 → 先用着
    char eb[96];
    av_strerror(n, eb, sizeof(eb));
    if (err) *err = std::string("读取失败: ") + eb;
    avio_close(avio);
    return false;
  }
  avio_close(avio);
  if (out->empty()) {
    if (err) *err = "空响应";
    return false;
  }
  return true;
}

/* ============================ 内置种子诗库 ============================
 * 没有网络 / 没校时（1970 → https 证书一律"尚未生效"）/ 接口挂了 —— 都不能让屏保空白。
 * 这 24 首覆盖 10 个题材，够"每天一首 + 喜欢不喜欢换"用很久。 */
const char *kSeed[][4] = {
    {"床前明月光，疑是地上霜。举头望明月，低头思故乡。", "静夜思", "唐·李白", "思乡"},
    {"月落乌啼霜满天，江枫渔火对愁眠。姑苏城外寒山寺，夜半钟声到客船。", "枫桥夜泊", "唐·张继", "愁绪"},
    {"白日依山尽，黄河入海流。欲穷千里目，更上一层楼。", "登鹳雀楼", "唐·王之涣", "励志"},
    {"空山不见人，但闻人语响。返景入深林，复照青苔上。", "鹿柴", "唐·王维", "山水"},
    {"千山鸟飞绝，万径人踪灭。孤舟蓑笠翁，独钓寒江雪。", "江雪", "唐·柳宗元", "山水"},
    {"锄禾日当午，汗滴禾下土。谁知盘中餐，粒粒皆辛苦。", "悯农", "唐·李绅", "田园"},
    {"春眠不觉晓，处处闻啼鸟。夜来风雨声，花落知多少。", "春晓", "唐·孟浩然", "田园"},
    {"故人西辞黄鹤楼，烟花三月下扬州。孤帆远影碧空尽，唯见长江天际流。", "黄鹤楼送孟浩然之广陵", "唐·李白", "送别"},
    {"渭城朝雨浥轻尘，客舍青青柳色新。劝君更尽一杯酒，西出阳关无故人。", "送元二使安西", "唐·王维", "送别"},
    {"秦时明月汉时关，万里长征人未还。但使龙城飞将在，不教胡马度阴山。", "出塞", "唐·王昌龄", "边塞"},
    {"葡萄美酒夜光杯，欲饮琵琶马上催。醉卧沙场君莫笑，古来征战几人回。", "凉州词", "唐·王翰", "边塞"},
    {"青海长云暗雪山，孤城遥望玉门关。黄沙百战穿金甲，不破楼兰终不还。", "从军行", "唐·王昌龄", "边塞"},
    {"独在异乡为异客，每逢佳节倍思亲。遥知兄弟登高处，遍插茱萸少一人。", "九月九日忆山东兄弟", "唐·王维", "节日"},
    {"爆竹声中一岁除，春风送暖入屠苏。千门万户曈曈日，总把新桃换旧符。", "元日", "宋·王安石", "节日"},
    {"明月几时有，把酒问青天。不知天上宫阙，今夕是何年。", "水调歌头", "宋·苏轼", "节日"},
    {"无边落木萧萧下，不尽长江滚滚来。万里悲秋常作客，百年多病独登台。", "登高", "唐·杜甫", "愁绪"},
    {"锦瑟无端五十弦，一弦一柱思华年。庄生晓梦迷蝴蝶，望帝春心托杜鹃。", "锦瑟", "唐·李商隐", "爱情"},
    {"相见时难别亦难，东风无力百花残。春蚕到死丝方尽，蜡炬成灰泪始干。", "无题", "唐·李商隐", "爱情"},
    {"墙角数枝梅，凌寒独自开。遥知不是雪，为有暗香来。", "梅花", "宋·王安石", "咏物"},
    {"咬定青山不放松，立根原在破岩中。千磨万击还坚劲，任尔东西南北风。", "竹石", "清·郑燮", "咏物"},
    {"碧玉妆成一树高，万条垂下绿丝绦。不知细叶谁裁出，二月春风似剪刀。", "咏柳", "唐·贺知章", "咏物"},
    {"老夫聊发少年狂，左牵黄，右擎苍。锦帽貂裘，千骑卷平冈。", "江城子·密州出猎", "宋·苏轼", "励志"},
    {"会当凌绝顶，一览众山小。荡胸生曾云，决眦入归鸟。", "望岳", "唐·杜甫", "励志"},
    {"两只黄鹂鸣翠柳，一行白鹭上青天。窗含西岭千秋雪，门泊东吴万里船。", "绝句", "唐·杜甫", "山水"},
};

/* ============================ 后台抓取 ============================ */

struct FetchJob {
  std::string url;
  std::string headers;
  int want;
};

void *fetchThread(void *arg) {
  FetchJob *job = (FetchJob *)arg;
  PoemBook &bk = PoemBook::instance();
  int ok = 0;
  std::string lastErr;
  std::string headers = job->headers;

  for (int i = 0; i < job->want; ++i) {
    std::string body, err;
    if (!httpFetch(job->url, headers, &body, &err)) {
      lastErr = err;
      break;  // 连续失败没意义（多半是没网），直接收
    }
    if (body.find("\"status\":\"success\"") == std::string::npos &&
        body.find("\"content\"") == std::string::npos) {
      lastErr = "响应里没有诗句字段";
      break;
    }
    Poem p;
    p.text = jsonStr(body, "content");
    p.title = jsonStr(body, "title");
    std::string dyn = jsonStr(body, "dynasty");
    std::string au = jsonStr(body, "author");
    p.author = dyn.empty() ? au : (au.empty() ? dyn : dyn + "·" + au);
    if (p.text.empty()) {
      // 有些返回把整首放在 origin.content 数组里
      p.text = jsonArrJoin(body, "content");
    }
    if (p.text.empty()) { lastErr = "诗句为空"; continue; }
    p.tag = classify(p);
    bk.addPoem(p);  // 由 PoemBook 去重
    ++ok;
  }

  bk.onFetchDone(ok, lastErr);
  delete job;
  return 0;
}

}  // namespace

/* ==================== PoemBook ==================== */

PoemBook &PoemBook::instance() {
  static PoemBook s;
  return s;
}

PoemBook::PoemBook()
    : loaded_(false), cur_(0), fetchDay_(0), lastAttemptDay_(0), fetching_(0),
      lastFetched_(0), lastTickMs_(0) {
  path_[0] = '\0';
}

void PoemBook::ensureLoaded() {
  if (loaded_) return;
  loaded_ = true;
  // 可写路径：/data 持久 → TF 卡 → /tmp（仅本次开机）
  const char *cand[] = {"/data/pocketgame_poem.dat", "/mnt/extsd/pocketgame_poem.dat",
                        "/tmp/pocketgame_poem.dat"};
  for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); ++i) {
    FILE *f = fopen(cand[i], "rb");
    if (f) {
      fclose(f);
      snprintf(path_, sizeof(path_), "%s", cand[i]);
      break;
    }
  }
  if (path_[0] == '\0') {
    snprintf(path_, sizeof(path_), "%s", cand[0]);  // 还不存在 → 先按 /data 试写
  }
  buildSeed();
  loadFromDisk();
}

void PoemBook::buildSeed() {
  if (!pool_.empty()) return;
  const int n = (int)(sizeof(kSeed) / sizeof(kSeed[0]));
  for (int i = 0; i < n; ++i)
    pool_.push_back(Poem(kSeed[i][0], kSeed[i][1], kSeed[i][2], kSeed[i][3]));
}

void PoemBook::loadFromDisk() {
  FILE *f = fopen(path_, "rb");
  if (!f) return;
  std::vector<Poem> saved;
  int cur = -1;
  int fday = 0;
  int aday = 0;
  std::string date;
  std::vector<std::string> liked, blocked;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    std::string s = trim(line);
    if (s.empty()) continue;
    if (s.compare(0, 5, "date=") == 0) { date = s.substr(5); continue; }
    if (s.compare(0, 4, "cur=") == 0) { cur = atoi(s.c_str() + 4); continue; }
    if (s.compare(0, 6, "fday=") == 0) { fday = atoi(s.c_str() + 5); continue; }
    if (s.compare(0, 5, "aday=") == 0) { aday = atoi(s.c_str() + 5); continue; }
    if (s.compare(0, 6, "poem=") == 0) {
      // poem=<tag>\t<title>\t<author>\t<text>
      std::string v = s.substr(5);
      size_t p1 = v.find('\t');
      size_t p2 = p1 == std::string::npos ? std::string::npos : v.find('\t', p1 + 1);
      size_t p3 = p2 == std::string::npos ? std::string::npos : v.find('\t', p2 + 1);
      if (p3 == std::string::npos) continue;
      saved.push_back(Poem(v.substr(p3 + 1), v.substr(p1 + 1, p2 - p1 - 1),
                           v.substr(p2 + 1, p3 - p2 - 1), v.substr(0, p1)));
      continue;
    }
    if (s.compare(0, 6, "liked=") == 0) {
      std::string v = s.substr(6);
      size_t b = 0;
      while (b <= v.size()) {
        size_t e = v.find(',', b);
        if (e == std::string::npos) e = v.size();
        addUnique(liked, v.substr(b, e - b));
        b = e + 1;
      }
      continue;
    }
    if (s.compare(0, 8, "blocked=") == 0) {
      std::string v = s.substr(8);
      size_t b = 0;
      while (b <= v.size()) {
        size_t e = v.find(',', b);
        if (e == std::string::npos) e = v.size();
        addUnique(blocked, v.substr(b, e - b));
        b = e + 1;
      }
    }
  }
  fclose(f);

  liked_ = liked;
  blocked_ = blocked;
  fetchDay_ = fday;
  lastAttemptDay_ = aday;
  date_ = date;
  // 只接受"当天抓的"池子（跨天就重新抓）；种子永远保留
  if (!saved.empty() && fday == todayKey()) {
    for (size_t i = 0; i < saved.size(); ++i) pool_.push_back(saved[i]);
  }
  cur_ = (cur >= 0 && cur < (int)pool_.size()) ? cur : 0;
  LOGD("PgPoem: 载入 %s：池 %d 首（含种子），今天已抓=%d，偏好 喜欢%d/拉黑%d",
       path_, (int)pool_.size(), fetchDay_ == todayKey() ? 1 : 0, (int)liked_.size(),
       (int)blocked_.size());
}

void PoemBook::saveToDisk() const {
  FILE *f = fopen(path_, "wb");
  if (!f) {
    LOGW("PgPoem: 写不了 %s（偏好与当日池子将丢失）", path_);
    return;
  }
  char head[128];
  snprintf(head, sizeof(head), "date=%s\ncur=%d\nfday=%d\naday=%d\n", date_.c_str(),
           cur_, fetchDay_, lastAttemptDay_);
  fputs(head, f);
  std::string s;
  for (size_t i = 0; i < liked_.size(); ++i) { s += liked_[i]; s += ','; }
  fprintf(f, "liked=%s\n", s.c_str());
  s.clear();
  for (size_t i = 0; i < blocked_.size(); ++i) { s += blocked_[i]; s += ','; }
  fprintf(f, "blocked=%s\n", s.c_str());
  // 只存"非种子"的那部分（种子代码里本来就有，落盘纯属浪费）
  const int seedN = (int)(sizeof(kSeed) / sizeof(kSeed[0]));
  for (size_t i = (size_t)seedN; i < pool_.size(); ++i) {
    const Poem &p = pool_[i];
    fprintf(f, "poem=%s\t%s\t%s\t%s\n", p.tag.c_str(), oneLine(p.title).c_str(),
            oneLine(p.author).c_str(), oneLine(p.text).c_str());
  }
  fclose(f);
}

/* 抓取线程回调（由 fetchThread 调；只在抓取线程里跑） */
void PoemBook::addPoem(const Poem &p) {
  Poem q = p;
  q.text = oneLine(q.text);
  q.title = oneLine(q.title);
  q.author = oneLine(q.author);
  for (size_t i = 0; i < pool_.size(); ++i)
    if (pool_[i].text == q.text) return;  // 去重
  if ((int)pool_.size() >= kMaxPool) pool_.erase(pool_.begin() + kMaxPool / 2);
  pool_.push_back(q);
  LOGD("PgPoem: 抓到《%s》%s [%s]", q.title.c_str(), q.author.c_str(), q.tag.c_str());
}

void PoemBook::onFetchDone(int ok, const std::string &err) {
  fetching_ = 0;
  lastFetched_ = ok;
  lastError_ = err;
  time_t t = time(0);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char d[16];
  snprintf(d, sizeof(d), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
           tmv.tm_mday);
  date_ = d;
  if (ok > 0) {
    fetchDay_ = todayKey();
    saveToDisk();
    LOGD("PgPoem: 今天抓到 %d 首，池子 %d 首", ok, (int)pool_.size());
  } else {
    LOGW("PgPoem: 抓取失败（%s）—— 继续用种子/缓存（屏保不会空白）", err.c_str());
  }
}

void PoemBook::startFetch() {
  if (fetching_) return;
  ensureLoaded();
  fetching_ = 1;
  lastFetched_ = 0;
  FetchJob *job = new FetchJob();
  job->url = urlOverride_.empty() ? std::string(kDefaultUrl) : urlOverride_;
  job->headers = "";
  job->want = kFetchPerDay;
  pthread_t th;
  if (pthread_create(&th, 0, fetchThread, job) != 0) {
    delete job;
    fetching_ = 0;
    lastError_ = "起线程失败";
    LOGW("PgPoem: 起抓取线程失败");
    return;
  }
  pthread_detach(th);
}

bool PoemBook::tick() {
  ensureLoaded();
  const long t = nowMs();
  if (t - lastTickMs_ < kTickThrottleMs) return false;
  lastTickMs_ = t;

  /* 日切：新的一天 → 换一首 + **最多抓一次**。
   * ⚠️ `lastAttemptDay_` 不能省：抓取失败时 fetchDay_ 不会前进，如果只判
   *    `fetchDay_ != today`，网络不通的板子会**每秒换一首诗 + 每秒重抓一次**
   *    （实测过一次：日志里"换一首"刷屏）。失败就等明天（QA `poem reload` 可立即重试）。 */
  const int today = todayKey();
  if (fetchDay_ != today && !fetching_ && lastAttemptDay_ != today) {
    lastAttemptDay_ = today;
    next();        // 换掉昨天的诗（种子池按顺序轮转 = 每天一首）
    saveToDisk();  // ⚠️ 立刻落盘"今天已经换过"：没网时 fetchDay_ 不会前进，
                   //    不存 aday 的话**每次重启都会再换一首**（成了"每次开机更新"）
    startFetch();
    return true;
  }
  return false;
}

const Poem *PoemBook::current() const {
  if (pool_.empty()) return 0;
  int i = cur_;
  if (i < 0 || i >= (int)pool_.size()) i = 0;
  return &pool_[i];
}

void PoemBook::pickIndex() {
  if (pool_.empty()) return;
  const int n = (int)pool_.size();
  // 从当前位置往后找第一首没被拉黑的题材；全被拉黑就退回"按天选"
  for (int k = 1; k <= n; ++k) {
    int i = (cur_ + k) % n;
    if (!contains(blocked_, pool_[i].tag)) { cur_ = i; return; }
  }
  cur_ = todayKey() % n;
}

void PoemBook::next() {
  ensureLoaded();
  pickIndex();
  time_t t = time(0);
  struct tm tmv;
  localtime_r(&t, &tmv);
  char d[16];
  snprintf(d, sizeof(d), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
           tmv.tm_mday);
  date_ = d;
  saveToDisk();
  const Poem *p = current();
  LOGD("PgPoem: 换一首 -> 《%s》[%s]（第 %d/%d 首）", p ? p->title.c_str() : "?",
       p ? p->tag.c_str() : "?", cur_ + 1, (int)pool_.size());
}

void PoemBook::like() {
  const Poem *p = current();
  if (p) {
    addUnique(liked_, p->tag);
    // 取消之前的拉黑（用户又喜欢了，说明不反感）
    for (size_t i = 0; i < blocked_.size(); ++i)
      if (blocked_[i] == p->tag) { blocked_.erase(blocked_.begin() + i); break; }
    LOGD("PgPoem: 喜欢 [%s]，换下一首", p->tag.c_str());
  }
  next();
}

void PoemBook::dislike() {
  const Poem *p = current();
  if (p) {
    addUnique(blocked_, p->tag);
    for (size_t i = 0; i < liked_.size(); ++i)
      if (liked_[i] == p->tag) { liked_.erase(liked_.begin() + i); break; }
    LOGD("PgPoem: 不喜欢 [%s] -> 以后不再推送该题材", p->tag.c_str());
  }
  next();
}

std::string PoemBook::status() const {
  const Poem *p = current();
  char b[320];
  snprintf(b, sizeof(b), "池 %d 首 / 第 %d 首 / 题材[%s] / 抓取%s(成功 %d) / url=%s",
           (int)pool_.size(), cur_ + 1, p ? p->tag.c_str() : "?",
           fetching_ ? "中" : "闲", lastFetched_,
           urlOverride_.empty() ? "默认" : urlOverride_.c_str());
  return std::string(b) + (lastError_.empty() ? "" : (" / 上次错误: " + lastError_));
}

void PoemBook::setUrlOverride(const std::string &url) {
  urlOverride_ = url;
  LOGD("PgPoem: 接口地址 -> %s", url.empty() ? "默认" : url.c_str());
}

void PoemBook::forceRefetch() {
  ensureLoaded();
  fetchDay_ = 0;
  lastAttemptDay_ = 0;
  LOGD("PgPoem: 强制重抓");
  startFetch();
}

void PoemBook::resetAll() {
  ensureLoaded();
  pool_.clear();
  liked_.clear();
  blocked_.clear();
  cur_ = 0;
  fetchDay_ = 0;
  lastAttemptDay_ = 0;
  buildSeed();
  saveToDisk();
  LOGD("PgPoem: 已复位（清偏好 + 回种子库 %d 首）", (int)pool_.size());
}

}  // namespace pg
