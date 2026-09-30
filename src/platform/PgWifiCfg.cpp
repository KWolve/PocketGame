#define _GNU_SOURCE
/*
 * PgWifiCfg.cpp - WiFi「回家快照」实现
 *
 * 设计要点见 PgWifiCfg.h。这里只补三条实现纪律：
 *
 * ⚠️ ① **别把密码写进日志**：psk 只报长度。日志在设备上会被别人看到（adb / 串口）。
 * ⚠️ ② **写文件要"先写临时名再 rename"**：直接覆盖 `wpa_supplicant.conf` 时，
 *     如果正好被杀掉（或掉电），配置文件就成了半截 —— 那样**连原网络也回不去**了。
 *     rename 在同一分区内是原子的。
 * ⚠️ ③ 权限要对齐原文件（`0666`，见设备上 `-rw-rw-rw-`）：wpa_supplicant 以 uid 0 跑，
 *     我们改窄了它照样能写，但保持一致最省心（也避免有人用非 root 工具去读）。
 */
#include "platform/PgWifiCfg.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "utils/Log.h"

namespace pg {

static const char *kConfPath = "/data/misc/wifi/wpa_supplicant.conf";
static const char *kHomeConf = "/data/misc/wifi/pg_home.conf";
static const char *kMetaPath = "/data/misc/wifi/pg_home.meta";

static const int kSsidMax = 64;
static const int kPskMax = 96;

/* ==========================================================================
 * 小工具
 * ========================================================================== */
static bool fileExists(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static long fileSize(const char *p) {
  struct stat st;
  if (!p || stat(p, &st) != 0) return -1;
  return (long)st.st_size;
}

/** 原子写：写到 `<dst>.tmp` 再 rename 覆盖。 */
static bool writeAtomic(const char *dst, const void *data, int n) {
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  size_t w = fwrite(data, 1, (size_t)n, f);
  fflush(f);
  fclose(f);
  if ((int)w != n) {
    unlink(tmp);
    return false;
  }
  if (rename(tmp, dst) != 0) {
    unlink(tmp);
    return false;
  }
  chmod(dst, 0666);
  return true;
}

static bool copyFileAtomic(const char *src, const char *dst) {
  FILE *fi = fopen(src, "rb");
  if (!fi) return false;
  char buf[4096];
  char tmp[256];
  snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
  FILE *fo = fopen(tmp, "wb");
  if (!fo) {
    fclose(fi);
    return false;
  }
  bool ok = true;
  int n;
  while ((n = (int)fread(buf, 1, sizeof(buf), fi)) > 0) {
    if ((int)fwrite(buf, 1, (size_t)n, fo) != n) {
      ok = false;
      break;
    }
  }
  if (ferror(fi)) ok = false;
  fflush(fo);
  fclose(fo);
  fclose(fi);
  if (!ok) {
    unlink(tmp);
    return false;
  }
  if (rename(tmp, dst) != 0) {
    unlink(tmp);
    return false;
  }
  chmod(dst, 0666);
  return true;
}

/** 从一行里取 `key="值"` 或 `key=值` 的值（去掉引号与首尾空白）。 */
static bool lineValue(const char *line, const char *key, char *out, int n) {
  size_t kl = strlen(key);
  const char *p = line;
  while (*p == ' ' || *p == '\t') ++p;
  if (strncmp(p, key, kl) != 0) return false;
  p += kl;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '=') ++p;
  else return false;
  while (*p == ' ' || *p == '\t') ++p;
  bool quoted = (*p == '"');
  if (quoted) ++p;
  int i = 0;
  while (*p && *p != '\n' && *p != '\r' && i < n - 1) {
    if (quoted && *p == '"') break;
    out[i++] = *p++;
  }
  out[i] = 0;
  /* 去尾空白 */
  while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t')) out[--i] = 0;
  return true;
}

/* ==========================================================================
 * 实现
 * ========================================================================== */
class WifiCfgImpl {
 public:
  char homeSsid[kSsidMax] = {0};
  char homePsk[kPskMax] = {0};
  bool target = false;
  char err[128] = {0};
  char ctrl[64] = {0};
};

static WifiCfgImpl gW;
static WifiCfg gApi;

void WifiCfg::setErr(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(gW.err, sizeof(gW.err), fmt, ap);
  va_end(ap);
  LOGD("PgWifiCfg: %s", gW.err);
}

const char *WifiCfg::confPath() { return kConfPath; }
const char *WifiCfg::homeConfPath() { return kHomeConf; }
const char *WifiCfg::metaPath() { return kMetaPath; }
const char *WifiCfg::lastError() const { return gW.err; }
bool WifiCfg::hasHome() const { return gW.homeSsid[0] != 0 && fileExists(kHomeConf); }
const char *WifiCfg::homeSsid() const { return gW.homeSsid; }
int WifiCfg::homePskLen() const { return (int)strlen(gW.homePsk); }
bool WifiCfg::targetMode() const { return gW.target; }

/* ---------- 解析 conf 里的第一个 network 块 ---------- */
static bool parseConf(const char *path, char *ssid, int sn, char *psk, int pn) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  char line[512];
  bool inBlock = false;
  bool gotSsid = false;
  ssid[0] = 0;
  psk[0] = 0;
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "network", 7) == 0) {
      inBlock = true;
      continue;
    }
    if (inBlock && *p == '}') {
      if (gotSsid) break;     // 第一个有名字的块就够了
      inBlock = false;
      continue;
    }
    if (!inBlock) continue;
    if (!gotSsid && lineValue(p, "ssid", ssid, sn)) gotSsid = true;
    else if (psk[0] == 0) lineValue(p, "psk", psk, pn);
  }
  fclose(f);
  return gotSsid;
}

/* ---------- meta 读写 ---------- */
static void writeMeta() {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "ssid=%s\npsk=%s\ntarget=%d\n",
                   gW.homeSsid[0] ? gW.homeSsid : "", gW.homePsk, gW.target ? 1 : 0);
  /* ⚠️ if/else 必须带花括号：LOGD 是宏，展开后跟一个 `;` 会让 `else` 找不到 if
   *    （编译错误 "'else' without a previous 'if'"）。宏 + if/else 一律加大括号。 */
  if (!writeAtomic(kMetaPath, buf, n)) {
    LOGD("PgWifiCfg: meta 写失败");
  } else {
    /* meta 里也有密码 ⇒ 和 conf 快照一样收紧到 0600。
     * ⚠️ 必须在这个写函数里收：enterTargetMode 会在 keepHome 之后再写一次 meta，
     *    只在 keepHome 里 chmod 会被这次重写重置回 0666（实测踩到）。 */
    chmod(kMetaPath, 0600);
  }
}

static void readMeta() {
  FILE *f = fopen(kMetaPath, "r");
  if (!f) return;
  char line[512];
  gW.homeSsid[0] = gW.homePsk[0] = 0;
  while (fgets(line, sizeof(line), f)) {
    if (!lineValue(line, "ssid", gW.homeSsid, sizeof(gW.homeSsid)) &&
        !lineValue(line, "psk", gW.homePsk, sizeof(gW.homePsk)) &&
        strncmp(line, "target=", 7) == 0) {
      gW.target = (atoi(line + 7) != 0);
    }
  }
  fclose(f);
}

/* ---------- wpa_supplicant 控制口 ---------- */
static bool isSocketPath(const char *p) {
  struct stat st;
  return p && stat(p, &st) == 0 && S_ISSOCK(st.st_mode);
}

/** 找控制口：优先 `/dev/socket/wlan0`，否则在 /dev/socket 里找第一个 wlan* 的 socket。 */
/** 在某个目录里找 wlan* 的 unix socket，找到就填进 gW.ctrl。 */
static bool ctrlScanDir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool got = false;
  struct dirent *e;
  while ((e = readdir(d)) != 0) {
    if (e->d_name[0] == '.') continue;
    char p[160];
    /* ① 直接就是 wlan* 的 socket */
    if (strncmp(e->d_name, "wlan", 4) == 0) {
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      if (isSocketPath(p)) {
        snprintf(gW.ctrl, sizeof(gW.ctrl), "%s", p);
        got = true;
        break;
      }
    }
    /* ② ★ 递归一层子目录：实测存在 `/dev/socket/wpa_cli/` 这种布局
     *    （conf 的 `ctrl_interface` 指到子目录时，控制口就落在里面） */
    if (e->d_type == DT_DIR) {
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      char sub[200];
      snprintf(sub, sizeof(sub), "%s/wlan0", p);
      if (isSocketPath(sub)) {
        snprintf(gW.ctrl, sizeof(gW.ctrl), "%s", sub);
        got = true;
        break;
      }
    }
  }
  closedir(d);
  return got;
}

/**
 * 找 wpa_supplicant 的控制口（unix socket）。
 *
 * ★★ 路径**不是固定的**（2026-09-15 真机踩到，症状极具误导性）：
 *   - init.rc 用 `-C/dev/socket/` 启动 ⇒ 正常是 `/dev/socket/wlan0`；
 *   - 但 conf 里的 `ctrl_interface=` **会覆盖命令行** —— 写成 `ctrl_interface=wlan0` 时
 *     wpa_supplicant 把 "wlan0" 当**目录**，控制口就不在 /dev/socket/wlan0 了
 *     （实测那会儿 /dev/socket 里只剩 zknet 自建的 `wpa_cli/wpa_ctrl_<pid>-*`）。
 *   写死路径的后果 = 所有 `wpaCmd` ENOENT ⇒ **"退出全信道后网络恢复不了"**，
 *   而日志只有一行失败（很难定位）。⇒ 多候选 + 扫目录（含一层子目录）。
 */
static void findCtrl() {
  if (gW.ctrl[0]) return;
  static const char *cand[] = {
      "/dev/socket/wlan0",                 /* init.rc -C/dev/socket/（最常见） */
      "/dev/socket/wpa_cli/wlan0",         /* -C/dev/socket/wpa_cli/ */
      "/dev/socket/wpa_supplicant/wlan0",  /* 另一种布局 */
      "/var/run/wpa_supplicant/wlan0",     /* 桌面发行版 */
  };
  for (size_t i = 0; i < sizeof(cand) / sizeof(cand[0]); ++i) {
    if (isSocketPath(cand[i])) {
      snprintf(gW.ctrl, sizeof(gW.ctrl), "%s", cand[i]);
      LOGD("PgWifiCfg: 控制口 = %s（候选 %d）", gW.ctrl, (int)i);
      return;
    }
  }
  if (ctrlScanDir("/dev/socket")) {
    LOGD("PgWifiCfg: 控制口 = %s（扫目录得到）", gW.ctrl);
    return;
  }
  LOGD("PgWifiCfg: ⚠ 没找到 wpa_supplicant 控制口 —— 检查 conf 的 ctrl_interface "
       "与 init.rc 的 -C 是否一致（wpa 命令都会失败）");
}

const char *WifiCfg::ctrlPath() const {
  if (!gW.ctrl[0]) findCtrl();
  return gW.ctrl;
}

int WifiCfg::wpaCmd(const char *cmd, char *resp, int rn) {
  if (!cmd || !cmd[0]) return -1;
  for (int attempt = 0; attempt < 2; ++attempt) {
    /* ★ 第 2 次尝试前清掉缓存重探：wpa_supplicant 重启 / 换了 ctrl_interface 目录时，
     *   缓存里的旧路径会一直 ENOENT（自愈）。 */
    if (attempt) {
      gW.ctrl[0] = 0;
      LOGD("PgWifiCfg: wpa 命令失败 -> 清缓存重新探测控制口");
    }
    int r = wpaCmdOnce(cmd, resp, rn);
    if (r >= 0 || gW.ctrl[0] == 0) return r;
    findCtrl();
    if (!gW.ctrl[0]) return r;
  }
  return -1;
}

/** wpaCmd 的单次执行体（不带重探逻辑）。 */
int WifiCfg::wpaCmdOnce(const char *cmd, char *resp, int rn) {
  if (!gW.ctrl[0]) findCtrl();
  if (!gW.ctrl[0]) {
    setErr("没有 wpa_supplicant 控制口（/dev/socket/wlan0 不存在？）");
    return -1;
  }
  if (resp && rn > 0) resp[0] = 0;

  const char *localPath = "/tmp/pg_wpa_ctl";

  /* ---- 先按 unix **datagram** 试（wpa_supplicant 的默认编法） ---- */
  {
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd >= 0) {
      struct sockaddr_un local, dst;
      memset(&local, 0, sizeof(local));
      local.sun_family = AF_UNIX;
      snprintf(local.sun_path, sizeof(local.sun_path), "%s", localPath);
      unlink(local.sun_path);
      if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0) {
        memset(&dst, 0, sizeof(dst));
        dst.sun_family = AF_UNIX;
        snprintf(dst.sun_path, sizeof(dst.sun_path), "%s", gW.ctrl);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 500000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int w = (int)sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&dst, sizeof(dst));
        if (w > 0) {
          char dummy[8];
          char *rb = (resp && rn > 0) ? resp : dummy;
          size_t rl = (resp && rn > 0) ? (size_t)(rn - 1) : sizeof(dummy) - 1;
          int r = (int)recv(fd, rb, rl, 0);
          if (r >= 0) {
            if (resp && rn > 0) resp[r < rn ? r : rn - 1] = 0;
            /* 应答里可能有 \r\n，去掉尾部换行（日志里好看） */
            if (resp) {
              int l = (int)strlen(resp);
              while (l > 0 && (resp[l - 1] == '\n' || resp[l - 1] == '\r')) resp[--l] = 0;
            }
            unlink(localPath);
            close(fd);
            return r;
          }
        }
      }
      unlink(localPath);
      close(fd);
    }
  }

  /* ---- 退一步：按 **STREAM** 试（有些版本编成 stream） ---- */
  {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
      struct sockaddr_un dst;
      memset(&dst, 0, sizeof(dst));
      dst.sun_family = AF_UNIX;
      snprintf(dst.sun_path, sizeof(dst.sun_path), "%s", gW.ctrl);
      struct timeval tv;
      tv.tv_sec = 1;
      tv.tv_usec = 500000;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
        if (send(fd, cmd, strlen(cmd), 0) > 0) {
          char dummy[8];
          char *rb = (resp && rn > 0) ? resp : dummy;
          size_t rl = (resp && rn > 0) ? (size_t)(rn - 1) : sizeof(dummy) - 1;
          int r = (int)recv(fd, rb, rl, 0);
          if (resp && rn > 0 && r > 0) resp[r < rn ? r : rn - 1] = 0;
          close(fd);
          if (r >= 0) {
            if (resp) {
              int l = (int)strlen(resp);
              while (l > 0 && (resp[l - 1] == '\n' || resp[l - 1] == '\r')) resp[--l] = 0;
            }
            return r;
          }
        }
      }
      close(fd);
    }
  }
  setErr("控制口命令 '%s' 失败（%s）", cmd, strerror(errno));
  return -1;
}

/* ---------- 快照 / 还原 ---------- */
bool WifiCfg::keepHome(const char *curSsid) {
  if (gW.target) {
    LOGD("PgWifiCfg: 目标网模式中 -> 不刷新快照（否则会把目标网当成'家'）");
    return false;
  }
  if (!fileExists(kConfPath)) {
    setErr("读不到 %s", kConfPath);
    return false;
  }
  char ssid[kSsidMax] = {0}, psk[kPskMax] = {0};
  bool ok = parseConf(kConfPath, ssid, sizeof(ssid), psk, sizeof(psk));
  if (!ok || !ssid[0]) {
    /* 配置里没有可用的 network 块（没连过任何网）—— 不算错，只是没得存 */
    setErr("配置里没有 network 条目，没得存");
    return false;
  }
  if (!copyFileAtomic(kConfPath, kHomeConf)) {
    setErr("拷贝 %s -> %s 失败（%s）", kConfPath, kHomeConf, strerror(errno));
    return false;
  }
  snprintf(gW.homeSsid, sizeof(gW.homeSsid), "%s", ssid);
  snprintf(gW.homePsk, sizeof(gW.homePsk), "%s", psk);
  writeMeta();
  /* ⚠️ 快照里**含明文密码** ⇒ 权限收到 0600（原 conf 是 0666，我们没必要照抄这个宽松度；
   *    本模块只以 root 读写，收紧不影响任何功能）。 */
  chmod(kHomeConf, 0600);
  chmod(kMetaPath, 0600);
  gW.err[0] = 0;
  LOGD("PgWifiCfg: ★ 已保存原网络 '%s'（conf %ld 字节，psk 长度 %d）curSsid='%s'",
       gW.homeSsid, fileSize(kHomeConf), (int)strlen(gW.homePsk), curSsid ? curSsid : "");
  return true;
}

bool WifiCfg::restoreConf() {
  if (!fileExists(kHomeConf)) {
    setErr("没有快照文件 %s", kHomeConf);
    return false;
  }
  long before = fileSize(kConfPath);
  if (!copyFileAtomic(kHomeConf, kConfPath)) {
    setErr("还原 %s -> %s 失败（%s）", kHomeConf, kConfPath, strerror(errno));
    return false;
  }
  LOGD("PgWifiCfg: 已把 %s（%ld 字节）覆盖回 %s（原 %ld 字节）", kHomeConf,
       fileSize(kHomeConf), kConfPath, before);
  return true;
}

bool WifiCfg::enterTargetMode(const char *curSsid) {
  if (!hasHome() || gW.homeSsid[0] == 0) readMeta();
  /* 进模式时**重新存一次**当前网络：上次留下的快照可能已经过期
   * （用户中途又换过别家网）——"存的是刚切走的那张"。 */
  bool saved = keepHome(curSsid);
  if (!saved && !hasHome()) {
    setErr("没有可保存的原网络（先连上一张网再进目标网模式）");
    return false;
  }
  gW.target = true;
  writeMeta();
  LOGD("PgWifiCfg: ★ 进入目标网模式（原网络 '%s' 已存好，可以放心切网）", gW.homeSsid);
  return true;
}

void WifiCfg::clearTargetMode() {
  if (!gW.target) return;
  gW.target = false;
  writeMeta();
  LOGD("PgWifiCfg: 退出目标网模式（不还原文件 —— 由调用方决定）");
}

bool WifiCfg::leaveTargetMode(char *ssidOut, int n, char *pskOut, int m) {
  if (!gW.homeSsid[0]) readMeta();
  if (ssidOut && n > 0) snprintf(ssidOut, (size_t)n, "%s", gW.homeSsid);
  if (pskOut && m > 0) snprintf(pskOut, (size_t)m, "%s", gW.homePsk);

  bool ok = restoreConf();
  if (ok) {
    /* 让 wpa_supplicant 重读配置并关联回原网络。三步的顺序不能变：
     *   ① RECONFIGURE      重新读配置文件（这之后文件里的 network 才是原网络）
     *   ② ENABLE_NETWORK all ★ 把网络**重新启用** —— 必须补这一步（2026-09-15 实测踩到）：
     *      切网过程中如果对目标网认证失败，wpa_supplicant 会把那张网标成
     *      `[TEMP-DISABLED]`（`LIST_NETWORKS` 里能看到），而且**这个标记跟 network id 绑定**；
     *      还原之后如果不显式启用，原网络可能一直是 disabled ⇒ REASSOCIATE/RECONNECT 都
     *      只会回 `OK` 却**永远不关联**（症状：`wpa_state=DISCONNECTED`、ifconfig 无 IP，
     *      看着像"还原失败"，其实是网络被临时禁用了）。
     *   ③ RECONNECT        关联回去
     * ⚠️ 还要知道：**手动把 IP 打回去不算修好** —— address 是 zknet 的连接事件里 DHCP/静态
     *    配置给的，所以恢复的最后一步应当交给上层（本页的 tickReconnect 会在 8 秒后用 zknet
     *    再 connect 一次兜底）。 */
    char resp[256] = {0};
    int r1 = wpaCmd("RECONFIGURE", resp, sizeof(resp));
    LOGD("PgWifiCfg: RECONFIGURE -> %d '%s'", r1, resp);
    usleep(200000);
    resp[0] = 0;
    int r0 = wpaCmd("ENABLE_NETWORK all", resp, sizeof(resp));
    LOGD("PgWifiCfg: ENABLE_NETWORK all -> %d '%s'", r0, resp);
    usleep(200000);
    resp[0] = 0;
    int r2 = wpaCmd("RECONNECT", resp, sizeof(resp));
    LOGD("PgWifiCfg: RECONNECT -> %d '%s'", r2, resp);
    if (r1 < 0 && r2 < 0) {
      setErr("文件已还原，但控制口不通（wpa_supplicant 没重读）—— 需要 zknet 兜底重连");
    }
  }
  gW.target = false;
  writeMeta();
  LOGD("PgWifiCfg: ★ 退出目标网模式：还原 '%s' %s", gW.homeSsid, ok ? "成功" : "失败");
  return ok;
}

WifiCfg *WifiCfg::instance() {
  /* 进程首次使用时把 meta 读进来（快照可能是上一次开机存的） */
  static bool inited = false;
  if (!inited) {
    inited = true;
    readMeta();
    if (gW.homeSsid[0])
      LOGD("PgWifiCfg: 载入上次的快照 '%s'（target=%d）", gW.homeSsid, gW.target ? 1 : 0);
  }
  return &gApi;
}

/**
 * 打开/关闭 STA 网卡（wlan0）。
 *
 * ★★ 为什么需要它（2026-09-15 实测）：全信道嗅探要跳信道，而**占用射频的是
 *    wlan0 这个接口本身**，不是"关联状态"：
 *      wlan0 关联着 / wlan0 up 但没关联  => SET_CHANNEL 返回 -16 EBUSY
 *      **wlan0 down**                     => SET_CHANNEL 返回 0，全信道可切
 *    ⇒ 只能真把接口关掉。用完必须 up 回来（否则用户"网没了"）。
 * ⚠️ 关接口**不会**改配置文件，但 wpa_supplicant 会认为断开 ⇒ up 回来之后
 *    要再 `RECONNECT`（或让上层 zknet 重连）。
 */
bool WifiCfg::staIfaceUp() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return false;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wlan0");
  bool up = false;
  if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) up = (ifr.ifr_flags & IFF_UP) != 0;
  close(fd);
  return up;
}

bool WifiCfg::setStaIfaceUp(bool up) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    LOGD("WifiCfg: socket 失败，无法%s wlan0", up ? "打开" : "关闭");
    return false;
  }
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wlan0");
  bool ok = false;
  if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
    const bool wasUp = (ifr.ifr_flags & IFF_UP) != 0;
    if (up) ifr.ifr_flags |= IFF_UP;
    else ifr.ifr_flags &= ~IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) == 0) ok = true;
    LOGD("WifiCfg: wlan0 %s（原来 %s）-> %s", up ? "up" : "down", wasUp ? "up" : "down",
         ok ? "成功" : "失败");
  } else {
    LOGD("WifiCfg: 读 wlan0 flags 失败（没有这张网卡？）");
  }
  close(fd);
  return ok;
}

}  // namespace pg
