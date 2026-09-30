/*
 * PgBt.cpp - 蓝牙 BLE HID 遥控器（外设侧）实现
 *
 * 骨架来自已真机验证的探针 `bt_probe/src/ble/ble_selftest.cpp`：
 *   rtk_init → btstack(posix run loop) → socketpair data source → TLV
 *   → hci_init(H5 + 8E1 + 1500000) → l2cap/sm/att/gatt_client/hids_device
 *   → 广播(HID UUID+Appearance+名字) + 扫描 → run_loop_execute()
 *
 * 两条铁律（踩过，见 docs/bt-hid-selftest.md）：
 *   1) 所有 btstack API（含 run loop 的 data source / timer 注册）都必须在
 *      跑 run loop 的那条线程里调；否则 run loop 起来后永远停在 INITIALIZING。
 *   2) UI 线程绝不直接调 btstack —— 只往 socketpair 写消息。
 */
#include "platform/PgBt.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "btstack/btstack.h"
#include "btstack/btstack_util.h"
#include "btstack/hci_transport_h5.h"
#include "btstack/posix/btstack_run_loop_posix.h"
#include "btstack/ble/le_device_db_tlv.h"
#include "btstack/ble/gatt-service/hids_device.h"
#include "btstack/ble/gatt-service/hids_client.h"
#include "btstack/btstack_hid_parser.h"
#include "btstack/ble/sm.h"
#include "btstack/ble/att_server.h"
#include "btstack/ble/gatt_client.h"

#include "platform/bt/tlv_posix.h"
#include "platform/bt/gatt_profile_data.h"

#include "utils/Log.h"

/* 移植过来的两件：自研串口（支持 8E1）+ Realtek 预初始化 */
extern "C" const btstack_uart_t *btstack_uart_termios_instance(void);
extern "C" int rtk_init(const char *dev);

#define BT_TLV_DB    "/data/bttlv.db"
#define BT_NAME      "PocketGame-RC"
#define KEY_REPORT_ID 1

namespace pg {

namespace {

enum { CMD_SEND_KEY = 1, CMD_STOP = 2, CMD_HOST_SCAN = 3, CMD_HOST_CONNECT = 4, CMD_HOST_DISCONNECT = 5 };
struct PostMsg {
  uint8_t cmd;
  uint8_t size;
  uint8_t data[8];
};

/* HID 报告描述符（Report ID 1 = Consumer Control 位图；ID 2 = Digitizer 触摸屏，
 * 后续做"反控"时用）。原样取自同事的 V851ExtendedScreen_ap_p2p 工程。 */
uint8_t *reportMap() {
  static uint8_t map[] = {
      0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x30, 0x09, 0x40, 0x09, 0x82,
      0x09, 0xA0, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x09, 0xCD,
      0x0A, 0x83, 0x01, 0x0A, 0x96, 0x01, 0x0A, 0x23, 0x02, 0x0A, 0x24, 0x02, 0x0A, 0x2D,
      0x02, 0x0A, 0x2E, 0x02, 0x19, 0x00, 0x29, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
      0x95, 0x10, 0x81, 0x02, 0xC0,
      0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x0D, 0x09, 0x22, 0xA1, 0x02,
      0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x09, 0x32,
      0x81, 0x02, 0x09, 0x47, 0x81, 0x02, 0x09, 0x51, 0x75, 0x05, 0x95, 0x01, 0x15, 0x00,
      0x25, 0x05, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x75, 0x0C, 0x95, 0x01, 0x55, 0x0E,
      0x65, 0x33, 0x26, 0xE8, 0x03, 0x81, 0x02, 0x09, 0x31, 0x75, 0x0C, 0x95, 0x01, 0x55,
      0x0E, 0x65, 0x33, 0x26, 0xE8, 0x03, 0x81, 0x02, 0xC0, 0xC0,
  };
  return map;
}

/**
 * 解析广播里的名字（AD type 0x08 = 短名 / 0x09 = 完整名）。
 * @return true = 本次解析到了名字（out 已被覆盖）；false = 这包没带名字（out 保持原值）
 *
 * ⚠️⚠️ **解析不到时不能把 out 清空**（2026-09-15 实测踩到）：
 *     BLE 设备的广播包是**交替**发的 —— 一包带名字、下一包只带 flags/service uuid。
 *     原来这里无条件 `out[0] = 0`，于是名字列**来回闪**：同一个地址同一秒，
 *     一次读到 'Ulanzi TC002 1c10'、下一次变成空（我们的页面显示"(匿名设备)"）。
 *     更坑的是"可疑判定"会跟着抖（匿名 + 强信号 = 可疑），列表看着像抽风。
 *     正确行为：记下"曾经见过的名字"，没有新名字就沿用旧的。
 */
bool advName(const uint8_t *data, uint8_t len, char *out, int outn) {
  int i = 0;
  while (i + 1 < len) {
    uint8_t flen = data[i];
    if (flen == 0 || i + 1 + flen > len) break;
    uint8_t type = data[i + 1];
    if ((type == 0x08 || type == 0x09) && flen >= 2) {
      int n = flen - 1;
      if (n > outn - 1) n = outn - 1;
      memcpy(out, data + i + 2, n);
      out[n] = 0;
      return true;
    }
    i += flen + 1;
  }
  return false;
}

}  // namespace

/* ==================== 实现体（只有 BT 线程会碰 btstack 部分） ==================== */
struct Bt::Impl {
  /* 下面这组是 UI 线程要读的，用 volatile */
  volatile int state;       // 0 未启动 / 1 初始化中 / 2 就绪
  volatile int connected;   // 主机启用了 HID 输入报告
  volatile int paired;      // 已配对
  volatile int advCount;    // 扫描到的广播数
  volatile int errCode;     // 0 正常；-1 rtk_init 失败

  const char *dev;
  int fds[2];
  pthread_t tid;
  bool threadStarted;

  /* 以下只在 BT 线程里使用 */
  btstack_data_source_t postDs;
  btstack_timer_source_t bootTimer;
  btstack_timer_source_t tickTimer;
  btstack_timer_source_t releaseTimer;
  btstack_packet_callback_registration_t hciEv;
  btstack_packet_callback_registration_t smEv;
  tlv_posix_t tlvCtx;
  hci_con_handle_t conHandle;
  uint8_t adv[32];
  uint8_t advLen;
  volatile uint16_t pendingMask;   // 待发的 HID 位图
  volatile int pendingValid;
  int advScanStarted;

  /* ==================== 主机侧（学习遥控器按键）====================
   * 扫描结果缓存 + HID 客户端（hids_client）状态 + 学到的报告。
   * 这些字段只有 BT 线程会改；UI 线程读地址/名字这类"写一次就不动"的字符串是安全的。 */
  struct ScanEntry {
    char addr[18];
    char name[24];
    int8_t rssi;
    uint8_t type;
  };
  ScanEntry scan[16];
  volatile int scanCount;
  volatile int scanCollect;     // 1 = 把扫到的设备收进列表
  int advStallTicks;            // 连续多少拍没有新广播（>0 说明扫描"哑了"→ 重起）
  int advCountLast;             // 上一拍看到的 advCount
  volatile int hostState;       // 0 空闲 1 连接中 2 已连(发现服务) 3 已订阅(学习中)
  volatile int hostMode;        // 1 = 当前这条 LE 连接是我们主动发起的
  char hostPeer[18];
  hci_con_handle_t hostHandle;
  uint16_t hidsCid;
  volatile int learnedCount;
  char learned[8][40];          // 滚动保存最近 8 条报告的"按键名"
  volatile uint16_t learnedMask[8];  // 对应的"我们的"consumer 位图（点一下就能重放）
  uint8_t hidDescStorage[512];  // hids_client 存对端报告描述符
};

/* 主机侧事件回调（hids_client 用）+ 前向声明（btPostHandler 里要用） */
static void btHostHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void btHostScanStart(void);
static void btHostConnect(int index);
static void btMaybeStartAdvScan(void);
/* 解析报告 → 按键名 + 可重放位图（定义在文件后部，btHostHandler 里要用） */
static uint16_t parseReportToMask(const uint8_t *desc, int dlen, const uint8_t *rep, int rlen,
                                  char *out, int outn);

static Bt *s_inst = 0;
static Bt::Impl *s_impl = 0;   // BT 线程回调里用（单例，直接引用）

/* ==================== HID 发送（BT 线程） ==================== */
static void btSendMask(uint16_t mask) {
  if (!s_impl || s_impl->conHandle == HCI_CON_HANDLE_INVALID) return;
  uint8_t r = hids_device_send_input_report_for_id(s_impl->conHandle, KEY_REPORT_ID,
                                                   (uint8_t *)&mask, sizeof(mask));
  LOGD("PgBt: 发 HID mask=0x%04x -> ret=%u", mask, r);
}

/* 发完一个非 0 位图后，120ms 自动补一个"全松开"，否则主机会以为键一直按着 */
static void releaseTimer(btstack_timer_source_t *ts) {
  (void)ts;
  btSendMask(0);
}

static void requestSend(void) {
  if (s_impl && s_impl->conHandle != HCI_CON_HANDLE_INVALID)
    hids_device_request_can_send_now_event(s_impl->conHandle);
}

/* ==================== 广播 / 扫描（BT 线程） ==================== */
static void btStartAdv(void) {
  Bt::Impl *im = s_impl;
  uint8_t base[] = {
      0x02, BLUETOOTH_DATA_TYPE_FLAGS, 0x06,
      0x03, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS,
      ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE & 0xff,
      ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE >> 8,
      0x03, BLUETOOTH_DATA_TYPE_APPEARANCE, 0xC1, 0x03,
  };
  uint8_t blen = sizeof(base);
  const char *name = BT_NAME;
  uint8_t nlen = (uint8_t)strlen(name);
  memcpy(im->adv, base, blen);
  im->adv[blen] = nlen + 1;
  im->adv[blen + 1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
  memcpy(im->adv + blen + 2, name, nlen);
  im->advLen = (uint8_t)(blen + nlen + 2);

  bd_addr_t null_addr;
  memset(null_addr, 0, 6);
  gap_advertisements_set_params(0x0030, 0x0030, 0, 0, null_addr, 0x07, 0x00);
  gap_advertisements_set_data(im->advLen, im->adv);
  gap_advertisements_enable(1);
  LOGD("PgBt: 广播已开（%u 字节，名字 \"%s\"，HID UUID + Appearance）", im->advLen, name);
}

static void btStartScan(void) {
  gap_set_scan_parameters(1, 0x0030, 0x0030);
  gap_start_scan();
  LOGD("PgBt: 扫描已开");
}

static void btMaybeStartAdvScan(void) {
  Bt::Impl *im = s_impl;
  if (!im || im->advScanStarted || im->state != 2) return;
  im->advScanStarted = 1;
  btStartAdv();
  btStartScan();
}

/* ==================== 事件（BT 线程） ==================== */
static void btPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET || !s_impl) return;
  Bt::Impl *im = s_impl;
  uint8_t ev = hci_event_packet_get_type(packet);

  switch (ev) {
    case BTSTACK_EVENT_STATE: {
      uint8_t st = btstack_event_state_get_state(packet);
      if (st == HCI_STATE_WORKING && im->state != 2) {
        im->state = 2;
        LOGD("PgBt: HCI 就绪（WORKING）");
        btMaybeStartAdvScan();
      } else if (st == HCI_STATE_OFF) {
        im->state = 0;
      }
      break;
    }

    case GAP_EVENT_ADVERTISING_REPORT: {
      im->advCount++;
      /* 低频心跳（每 200 条一次）：排查"列表恒空"时最有用的一条 ——
       * 能立刻区分「控制器不报广播了」还是「报了但没进收集代码」。 */
      if ((im->advCount % 200) == 0)
        LOGD("PgBt: 广播累计 %d 条，已收集 %d 个", im->advCount, im->scanCount);
      {
      bd_addr_t a;
      gap_event_advertising_report_get_address(packet, a);
      const char *as = bd_addr_to_str(a);
      int k = -1;
      for (int i = 0; i < im->scanCount; ++i)
        if (strcmp(im->scan[i].addr, as) == 0) { k = i; break; }
      if (k < 0 && im->scanCount < 16) {
        k = im->scanCount++;
        strncpy(im->scan[k].addr, as, sizeof(im->scan[k].addr) - 1);
        im->scan[k].addr[17] = 0;
        im->scan[k].name[0] = 0;
        LOGD("PgBt: 收集到设备 [%d] %s", k, as);
      }
      if (k >= 0) {
        im->scan[k].rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);
        im->scan[k].type = gap_event_advertising_report_get_address_type(packet);
        uint8_t dlen = gap_event_advertising_report_get_data_length(packet);
        const uint8_t *d = gap_event_advertising_report_get_data(packet);
        /* ⚠️ 只在**解析到名字**时才覆盖（详见 advName 的注释：交替包会把名字刷空） */
        if (dlen) advName(d, dlen, im->scan[k].name, sizeof(im->scan[k].name));
      }
      }
      break;
    }

    case HCI_EVENT_META_GAP: {
      if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
        hci_con_handle_t h = gap_subevent_le_connection_complete_get_connection_handle(packet);
        if (im->hostMode) {
          /* 我们主动发起的连接（学习模式）：交给 hids_client 去发现 HID 服务。
           * hids_client_connect 会自动发现服务/报告映射，并**自动启用输入报告通知**。 */
          im->hostMode = 0;
          im->hostState = 2;
          im->hostHandle = h;
          uint16_t cid = 0;
          uint8_t r = hids_client_connect(h, &btHostHandler, HID_PROTOCOL_MODE_REPORT, &cid);
          im->hidsCid = cid;
          LOGD("PgBt: LE 已连上 handle=0x%04x → hids_client_connect=%u cid=%u", h, r, cid);
          if (r != ERROR_CODE_SUCCESS) {
            im->hostState = 0;
            LOGD("PgBt: hids_client 连接失败（对端可能没有 HID 服务）");
            gap_disconnect(h);
          }
        } else {
          LOGD("PgBt: 主机已连接 handle=0x%04x", h);
          /* 遥控器要低延迟：请求 11.25~30ms 连接间隔 */
          gap_request_connection_parameter_update(h, 9, 24, 0, 400);
        }
      }
      break;
    }

    case HCI_EVENT_HIDS_META: {
      uint8_t sub = hci_event_hids_meta_get_subevent_code(packet);
      if (sub == HIDS_SUBEVENT_INPUT_REPORT_ENABLE) {
        im->conHandle =
            (hci_con_handle_t)hids_subevent_input_report_enable_get_con_handle(packet);
        im->connected = 1;
        LOGD("PgBt: HID 输入报告已启用 handle=0x%04x —— 遥控器可用", im->conHandle);
        /* 立刻发一次"播放/暂停"当作握手示意 */
        im->pendingMask = Bt::RC_PLAY;
        im->pendingValid = 1;
        requestSend();
      } else if (sub == HIDS_SUBEVENT_CAN_SEND_NOW) {
        if (im->pendingValid) {
          im->pendingValid = 0;
          uint16_t m = im->pendingMask;
          btSendMask(m);
          if (m) {
            /* 自动松开 */
            btstack_run_loop_set_timer_handler(&im->releaseTimer, &releaseTimer);
            btstack_run_loop_set_timer(&im->releaseTimer, 120);
            btstack_run_loop_add_timer(&im->releaseTimer);
          }
        }
      }
      break;
    }

    case HCI_EVENT_DISCONNECTION_COMPLETE:
      im->conHandle = HCI_CON_HANDLE_INVALID;
      im->connected = 0;
      if (im->hostState || im->hostHandle != HCI_CON_HANDLE_INVALID) {
        im->hostState = 0;
        im->hostHandle = HCI_CON_HANDLE_INVALID;
        im->hidsCid = 0;
      }
      LOGD("PgBt: 连接断开");
      break;

    case SM_EVENT_JUST_WORKS_REQUEST:
      LOGD("PgBt: 配对请求（Just Works）→ 自动确认");
      sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
      break;
    case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
      sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_passkey(packet));
      break;
    case SM_EVENT_PAIRING_COMPLETE:
      im->paired = 1;
      LOGD("PgBt: 配对完成（信息写入 %s）", BT_TLV_DB);
      break;

    default:
      break;
  }
}

/* ==================== socketpair 投递（BT 线程读） ==================== */
static void btPostHandler(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
  (void)ds;
  if (type != DATA_SOURCE_CALLBACK_READ || !s_impl) return;
  Bt::Impl *im = s_impl;
  PostMsg m;
  while (read(im->fds[0], &m, sizeof(m)) == (ssize_t)sizeof(m)) {
    if (m.cmd == CMD_SEND_KEY) {
      uint16_t mask = (uint16_t)(m.data[0] | (m.data[1] << 8));
      if (!im->connected) {
        LOGD("PgBt: 未连接，忽略按键 0x%04x", mask);
        continue;
      }
      im->pendingMask = mask;
      im->pendingValid = 1;
      requestSend();
      btMaybeStartAdvScan();
    } else if (m.cmd == CMD_HOST_SCAN) {
      btHostScanStart();
      btMaybeStartAdvScan();
    } else if (m.cmd == CMD_HOST_CONNECT) {
      btHostConnect(m.data[0]);
    } else if (m.cmd == CMD_HOST_DISCONNECT) {
      if (im->hostState && im->hostHandle != HCI_CON_HANDLE_INVALID)
        gap_disconnect(im->hostHandle);
      im->hostState = 0;
    } else if (m.cmd == CMD_STOP) {
      btstack_run_loop_trigger_exit();
    }
  }
}

/* ==================== 定时器（BT 线程） ==================== */
static void btTickTimer(btstack_timer_source_t *ts) {
  btMaybeStartAdvScan();
  static int tickN = 0;
  if ((++tickN % 15) == 0)  // 每 30s 一条心跳：证明 BT 线程的定时器还活着
    LOGD("PgBt: 心跳 tick=%d adv=%d scan=%d", tickN, s_impl ? s_impl->advCount : -1,
         s_impl ? s_impl->scanCount : -1);
  /* 自愈：扫描"哑了"就重起。
   * 实测控制器跑一阵会自己停上报（advCount 冻结、列表恒空）；这里每 2s 看一次，
   * 连续 5 拍（≈10s）没有新广播就重起一次 LE 扫描（重起也不会丢已收集的列表）。 */
  Bt::Impl *im = s_impl;
  if (im && im->state == 2) {
    if (im->advCount == im->advCountLast) {
      if (++im->advStallTicks >= 5) {
        im->advStallTicks = 0;
        LOGD("PgBt: 扫描已哑（advCount 卡在 %d），走冷路径重起 LE 扫描", im->advCount);
        /* ⚠️ 同样不要 stop→start 紧邻调用（会停在关闭态，见 btHostScanStart 的注释） */
        im->advScanStarted = 0;
        btMaybeStartAdvScan();
      }
    } else {
      im->advStallTicks = 0;
    }
    im->advCountLast = im->advCount;
  }
  btstack_run_loop_set_timer(ts, 2000);
  btstack_run_loop_add_timer(ts);
}

static void btBootTimer(btstack_timer_source_t *ts) {
  (void)ts;
  btstack_run_loop_set_timer_handler(&s_impl->tickTimer, &btTickTimer);
  btstack_run_loop_set_timer(&s_impl->tickTimer, 2000);
  btstack_run_loop_add_timer(&s_impl->tickTimer);
  LOGD("PgBt: HCI_POWER_ON");
  hci_power_control(HCI_POWER_ON);
}

/* ==================== 主机侧：HID 客户端事件 + 操作 ==================== */
static void btHostHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET || !s_impl) return;
  Bt::Impl *im = s_impl;
  if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

  switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
    case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
      uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
      uint8_t st = gattservice_subevent_hid_service_connected_get_status(packet);
      uint8_t mode = gattservice_subevent_hid_service_connected_get_protocol_mode(packet);
      uint8_t n = gattservice_subevent_hid_service_connected_get_num_instances(packet);
      LOGD("PgBt: HID 服务已连接 cid=%u status=0x%02x mode=%u 实例数=%u", cid, st, mode, n);
      if (st == ERROR_CODE_SUCCESS) {
        im->hidsCid = cid;
        im->hostState = 3;   /* 通知已由 hids_client 自动启用 → 进入"学习中" */
        LOGD("PgBt: ★ 已订阅输入报告 —— 现在按对方遥控器/键盘的键");
      } else {
        im->hostState = 2;
        LOGD("PgBt: 对端没有可用的 HID 输入报告");
      }
      break;
    }
    case GATTSERVICE_SUBEVENT_HID_REPORT: {
      uint8_t rid = gattservice_subevent_hid_report_get_report_id(packet);
      uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
      const uint8_t *d = gattservice_subevent_hid_report_get_report(packet);
      uint8_t svc = gattservice_subevent_hid_report_get_service_index(packet);
      /* 用**对端的报告描述符**把这条报告解析成按键名 —— 这就是"学习" */
      const uint8_t *desc =
          hids_client_descriptor_storage_get_descriptor_data(im->hidsCid, svc);
      uint16_t dlen = hids_client_descriptor_storage_get_descriptor_len(im->hidsCid, svc);
      char names[40];
      uint16_t mask = parseReportToMask(desc, dlen, d, len, names, sizeof(names));
      int slot = im->learnedCount & 7;
      snprintf(im->learned[slot], sizeof(im->learned[slot]), "%s", names);
      im->learnedMask[slot] = mask;
      im->learnedCount++;
      LOGD("PgBt: ★ 学到 #%d rid=%u len=%u → %s（可重放位图 0x%04x）", im->learnedCount, rid,
           len, names, mask);
      break;
    }
    case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
      im->hostState = 0;
      im->hidsCid = 0;
      LOGD("PgBt: HID 服务断开");
      break;
    default:
      break;
  }
}

static void btHostScanStart(void) {
  Bt::Impl *im = s_impl;
  if (!im) return;
  im->scanCount = 0;
  im->scanCollect = 1;
  /* ⚠️⚠️ **不要无条件 `gap_stop_scan(); btStartScan();`**（2026-09-15 真机踩到）：
   *   本板在这条"stop 紧接 start"的路径上会把 LE 扫描**彻底弄死** ——
   *     调用前 advCount 一直在涨（扫描是活的）→ 调用后 advCount **立刻冻死**、
   *     列表恒 0，连 btTickTimer 的哑火自愈（每 10s 重起一次）也救不回来。
   *     实测判据：`btscan` 前 adv=13400 且持续增长；之后 10 分钟仍卡在 20987、dev=0。
   *     （btstack 的 gap_stop_scan/gap_start_scan 要紧邻调用时，HCI 侧
   *      "command disallowed" 会让扫描停在关闭态。）
   *  ⇒ 只在**确实哑火**时才重起，并且走"冷路径" btMaybeStartAdvScan()
   *    （= 当初让扫描跑起来的那条路，内部只 start、不 stop）。
   *    扫描正常时，"重新扫描"就只是**清空重收**，完全不碰控制器状态。
   *    ⚠️ API 叫 gap_stop_scan()，**不是** gap_scan_stop()（btstack 里 start/stop
   *    的构词不一致，写错直接编译不过）。 */
  const int last = im->advCountLast;
  if (im->advCount == last) {
    LOGD("PgBt: 重新扫描时发现扫描已哑（advCount 卡在 %d）→ 冷路径重起 LE 扫描", last);
    im->advScanStarted = 0;
    btMaybeStartAdvScan();
  } else {
    LOGD("PgBt: 开始收集扫描结果（最多 16 个，扫描在跑：adv=%d，不动控制器状态）",
         im->advCount);
  }
  im->advCountLast = im->advCount;
  im->advStallTicks = 0;
}

static void btHostConnect(int index) {
  Bt::Impl *im = s_impl;
  if (!im) return;
  if (index < 0 || index >= im->scanCount) {
    LOGD("PgBt: btconn 索引越界（当前只有 %d 个）", im->scanCount);
    return;
  }
  bd_addr_t addr;
  sscanf_bd_addr(im->scan[index].addr, addr);
  snprintf(im->hostPeer, sizeof(im->hostPeer), "%s", im->scan[index].addr);
  im->hostMode = 1;
  im->hostState = 1;
  uint8_t r = gap_connect(addr, (bd_addr_type_t)im->scan[index].type);
  LOGD("PgBt: gap_connect(%s type=%u) -> %u", im->scan[index].addr, im->scan[index].type, r);
  if (r != ERROR_CODE_SUCCESS) im->hostState = 0;
}

/* ==================== BT 线程 ==================== */
static void btLeHidSetup(void) {
  l2cap_init();
  sm_init();
  sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
  att_server_init(profile_data, NULL, NULL);
  gatt_client_init();
  hids_device_init(0, reportMap(), 139);

  s_impl->hciEv.callback = &btPacketHandler;
  hci_add_event_handler(&s_impl->hciEv);
  s_impl->smEv.callback = &btPacketHandler;
  sm_add_event_handler(&s_impl->smEv);
  hids_device_register_packet_handler(btPacketHandler);
  /* 主机侧（学习模式）：给 hids_client 一块存对端报告描述符的空间 */
  hids_client_init(s_impl->hidDescStorage, sizeof(s_impl->hidDescStorage));
  LOGD("PgBt: le_hid_setup 完成（外设 + 主机双角色）");
}

static void *btThread(void *arg) {
  const char *dev = (const char *)arg;
  Bt::Impl *im = s_impl;

  /* ① Realtek 预初始化（上电时序 + 下补丁固件 + 切 1500000）。
   *    ⚠️ 必须放在这条线程里：它要 20~40 秒，放在 UI 线程会卡住整个界面。
   *    失败会间歇性发生（OP_H5_SYNC timeout）→ 断电重试最多 3 次。 */
  int r = -1;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    r = rtk_init(dev);
    LOGD("PgBt: rtk_init 第 %d 次 → %d", attempt, r);
    if (r == 0) break;
    const char *sb = "/sys/devices/platform/soc/soc@03000000:netRF/state_bt";
    FILE *f = fopen(sb, "w");
    if (f) { fwrite("0", 1, 1, f); fclose(f); }
    sleep(2);
    f = fopen(sb, "w");
    if (f) { fwrite("1", 1, 1, f); fclose(f); }
    sleep(1);
  }
  if (r != 0) {
    im->errCode = -1;
    im->state = 0;
    LOGD("PgBt: rtk_init 三次都失败（BT 需物理冷启动；见 docs/bt-hid-selftest.md）");
    /* 不 return：继续让 btstack 试一下，界面也能显示状态 */
  }

  btstack_memory_init();
  btstack_run_loop_init(btstack_run_loop_posix_get_instance());

  /* 跨线程投递 */
  btstack_run_loop_set_data_source_fd(&im->postDs, im->fds[0]);
  btstack_run_loop_set_data_source_handler(&im->postDs, &btPostHandler);
  btstack_run_loop_enable_data_source_callbacks(&im->postDs, DATA_SOURCE_CALLBACK_READ);
  btstack_run_loop_add_data_source(&im->postDs);

  /* TLV 持久化（配对信息） */
  memset(&im->tlvCtx, 0, sizeof(im->tlvCtx));
  const btstack_tlv_t *tlv = tlv_posix_init_instance(&im->tlvCtx, BT_TLV_DB);
  if (tlv) {
    btstack_tlv_set_instance(tlv, &im->tlvCtx);
    le_device_db_tlv_configure(tlv, &im->tlvCtx);
    LOGD("PgBt: TLV 就绪（%s）", BT_TLV_DB);
  } else {
    LOGD("PgBt: TLV 初始化失败（%s 不可写？）", BT_TLV_DB);
  }

  /* HCI：H5 + 偶校验 8E1 + 1500000（RTL8733BS 规格） */
  const btstack_uart_t *uart = btstack_uart_termios_instance();
  const hci_transport_t *transport = hci_transport_h5_instance(uart);
  hci_transport_config_uart_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.type = HCI_TRANSPORT_CONFIG_UART;
  cfg.device_name = dev;
  cfg.baudrate_init = 1500000;
  cfg.baudrate_main = 0;
  cfg.flowcontrol = 0;
  cfg.parity = BTSTACK_UART_PARITY_EVEN;
  hci_init(transport, &cfg);

  btLeHidSetup();

  btstack_run_loop_set_timer_handler(&im->bootTimer, &btBootTimer);
  btstack_run_loop_set_timer(&im->bootTimer, 1);
  btstack_run_loop_add_timer(&im->bootTimer);

  btstack_run_loop_execute();
  LOGD("PgBt: run loop 退出");
  return 0;
}

/* ==================== 对外接口 ==================== */
Bt::Bt() : impl_(0), started_(false) {}

Bt::~Bt() {
  stop();
  if (impl_) {
    delete static_cast<Impl *>(impl_);
    impl_ = 0;
    s_impl = 0;
  }
}

Bt *Bt::instance() {
  if (!s_inst) s_inst = new Bt();
  return s_inst;
}

bool Bt::start(const char *dev) {
  if (started_) return true;
  if (!impl_) impl_ = new Impl();
  s_impl = static_cast<Bt::Impl *>(impl_);

  Impl *im = static_cast<Impl *>(impl_);
  /* 逐个赋值（volatile 成员不能靠 memset 一把清零） */
  im->state = 1;
  im->connected = 0;
  im->paired = 0;
  im->advCount = 0;
  im->errCode = 0;
  im->dev = dev;
  im->threadStarted = false;
  im->conHandle = HCI_CON_HANDLE_INVALID;
  im->hostHandle = HCI_CON_HANDLE_INVALID;
  im->hostState = 0;
  im->hostMode = 0;
  im->scanCount = 0;
  im->scanCollect = 0;
  im->learnedCount = 0;
  im->hostPeer[0] = 0;
  im->pendingMask = 0;
  im->pendingValid = 0;
  im->advScanStarted = 0;
  im->advStallTicks = 0;
  im->advCountLast = 0;
  im->fds[0] = im->fds[1] = -1;

  if (socketpair(PF_UNIX, SOCK_STREAM, 0, im->fds) < 0) {
    LOGD("PgBt: socketpair 失败");
    im->errCode = -2;
    return false;
  }

  /* 起 BT 线程：rtk_init 与所有 btstack 调用都在它里面（不阻塞 UI 线程） */
  if (pthread_create(&im->tid, 0, btThread, (void *)dev) != 0) {
    LOGD("PgBt: 起线程失败");
    im->errCode = -3;
    return false;
  }
  im->threadStarted = true;
  started_ = true;
  LOGD("PgBt: 已启动（dev=%s）", dev);
  return true;
}

void Bt::stop() {
  if (!started_) return;
  Impl *im = static_cast<Impl *>(impl_);
  if (im->fds[1] >= 0) {
    PostMsg m;
    memset(&m, 0, sizeof(m));
    m.cmd = CMD_STOP;
    ssize_t n = write(im->fds[1], &m, sizeof(m));
    (void)n;
  }
  if (im->threadStarted) {
    pthread_join(im->tid, 0);
    im->threadStarted = false;
  }
  if (im->fds[0] >= 0) close(im->fds[0]);
  if (im->fds[1] >= 0) close(im->fds[1]);
  im->fds[0] = im->fds[1] = -1;
  started_ = false;
  LOGD("PgBt: 已停止");
}

int Bt::state() const {
  return impl_ ? static_cast<Impl *>(impl_)->state : 0;
}

const char *Bt::stateText() const {
  int s = state();
  if (impl_ && static_cast<Impl *>(impl_)->errCode == -1) return "初始化失败";
  switch (s) {
    case 0: return "未启动";
    case 1: return "初始化中";
    case 2: return connected() ? "已连接" : "就绪（可被发现）";
    default: return "?";
  }
}

bool Bt::connected() const {
  return impl_ ? (static_cast<Impl *>(impl_)->connected != 0) : false;
}

bool Bt::paired() const {
  return impl_ ? (static_cast<Impl *>(impl_)->paired != 0) : false;
}

int Bt::advCount() const {
  return impl_ ? static_cast<Impl *>(impl_)->advCount : 0;
}

/* ---- 主机侧对外接口 ---- */
void Bt::hostScanStart() {
  if (!impl_) return;
  Impl *im = static_cast<Impl *>(impl_);
  if (im->fds[1] < 0) return;
  PostMsg m;
  memset(&m, 0, sizeof(m));
  m.cmd = CMD_HOST_SCAN;
  ssize_t n = write(im->fds[1], &m, sizeof(m));
  (void)n;
}

int Bt::scannedCount() const { return impl_ ? static_cast<Impl *>(impl_)->scanCount : 0; }

const char *Bt::scannedAddr(int i) const {
  Impl *im = static_cast<Impl *>(impl_);
  if (!im || i < 0 || i >= im->scanCount) return "";
  return im->scan[i].addr;
}

const char *Bt::scannedName(int i) const {
  Impl *im = static_cast<Impl *>(impl_);
  if (!im || i < 0 || i >= im->scanCount) return "";
  return im->scan[i].name;
}

int Bt::scannedRssi(int i) const {
  Impl *im = static_cast<Impl *>(impl_);
  if (!im || i < 0 || i >= im->scanCount) return 0;
  return im->scan[i].rssi;
}

void Bt::hostConnect(int i) {
  if (!impl_) return;
  Impl *im = static_cast<Impl *>(impl_);
  if (im->fds[1] < 0) return;
  PostMsg m;
  memset(&m, 0, sizeof(m));
  m.cmd = CMD_HOST_CONNECT;
  m.data[0] = (uint8_t)i;
  ssize_t n = write(im->fds[1], &m, sizeof(m));
  (void)n;
}

void Bt::hostDisconnect() {
  if (!impl_) return;
  Impl *im = static_cast<Impl *>(impl_);
  if (im->fds[1] < 0) return;
  PostMsg m;
  memset(&m, 0, sizeof(m));
  m.cmd = CMD_HOST_DISCONNECT;
  ssize_t n = write(im->fds[1], &m, sizeof(m));
  (void)n;
}

int Bt::hostState() const { return impl_ ? static_cast<Impl *>(impl_)->hostState : 0; }

const char *Bt::hostStateText() const {
  switch (hostState()) {
    case 0: return "空闲";
    case 1: return "连接中";
    case 2: return "已连接（发现服务）";
    case 3: return "学习中（按对方按键）";
    default: return "?";
  }
}

const char *Bt::hostPeer() const {
  Impl *im = static_cast<Impl *>(impl_);
  return (im && im->hostPeer[0]) ? im->hostPeer : "-";
}

int Bt::learnedCount() const { return impl_ ? static_cast<Impl *>(impl_)->learnedCount : 0; }

uint16_t Bt::learnedMask(int i) const {
  Impl *im = static_cast<Impl *>(impl_);
  if (!im) return 0;
  int n = im->learnedCount;
  if (n > 8) n = 8;
  if (i < 0 || i >= n) return 0;
  return im->learnedMask[i];
}

const char *Bt::learnedHex(int i) const {
  Impl *im = static_cast<Impl *>(impl_);
  if (!im) return "";
  int n = im->learnedCount;
  if (n > 8) n = 8;
  if (i < 0 || i >= n) return "";
  return im->learned[i];
}

/* ==================== HID 报告解析 ==================== */
const uint8_t *Bt::ourReportMap(int *len) {
  if (len) *len = 139;
  return reportMap();
}

const char *Bt::usageName(uint16_t page, uint16_t usage) {
  if (page == 0x0C) {  /* Consumer Control */
    switch (usage) {
      case 0x30: return "电源";
      case 0x40: return "菜单";
      case 0x41: return "菜单选择";
      case 0x82: return "模式切换";
      case 0xB5: return "下一首";
      case 0xB6: return "上一首";
      case 0xB7: return "停止";
      case 0xCD: return "播放暂停";
      case 0xE2: return "静音";
      case 0xE9: return "音量加";
      case 0xEA: return "音量减";
      case 0x223: return "主页";
      case 0x224: return "返回";
      case 0x22A: return "选择";
      case 0x196: return "浏览器";
      default: break;
    }
  } else if (page == 0x07) {  /* Keyboard */
    if (usage >= 0x04 && usage <= 0x1D) return "字母键";
    if (usage >= 0x1E && usage <= 0x27) return "数字键";
    switch (usage) {
      case 0x28: return "回车";
      case 0x29: return "ESC";
      case 0x2C: return "空格";
      case 0x4F: return "右键";
      case 0x50: return "左键";
      case 0x51: return "下键";
      case 0x52: return "上键";
      case 0x4A: return "Home键";
      case 0x4D: return "End键";
      case 0xE8: return "音量加";
      case 0xE9: return "音量减";
      case 0xCD: return "播放暂停";
      case 0xB5: return "下一首";
      case 0xB6: return "上一首";
      default: break;
    }
  } else if (page == 0x0D) {  /* Digitizer */
    switch (usage) {
      case 0x42: return "触摸";
      case 0x30: return "笔尖X";
      case 0x31: return "笔尖Y";
      default: break;
    }
  } else if (page == 0x01) {  /* Generic Desktop */
    switch (usage) {
      case 0x30: return "X轴";
      case 0x31: return "Y轴";
      default: break;
    }
  }
  return "按键";
}

int Bt::parseReport(const uint8_t *desc, int dlen, const uint8_t *rep, int rlen, char *out,
                    int outn) {
  if (!out || outn <= 0) return 0;
  out[0] = 0;
  if (!desc || dlen <= 0 || !rep || rlen <= 0) return 0;
  btstack_hid_parser_t parser;
  btstack_hid_parser_init(&parser, desc, (uint16_t)dlen, HID_REPORT_TYPE_INPUT, rep,
                          (uint16_t)rlen);
  int n = 0, off = 0;
  while (btstack_hid_parser_has_more(&parser) && off < outn - 30) {
    uint16_t up = 0, u = 0;
    int32_t v = 0;
    btstack_hid_parser_get_field(&parser, &up, &u, &v);
    if (v != 0) {
      off += snprintf(out + off, outn - off, "%s ", usageName(up, u));
      n++;
      if (n >= 4) break;  /* 一层最多列 4 个 */
    }
  }
  if (n == 0) snprintf(out, outn, "(无按键)");
  return n;
}

/* usage → "我们的"consumer 位图（重放用：把对端按键映射成我们能发出去的键） */
uint16_t Bt::usageToMask(uint16_t page, uint16_t usage) {
  if (page != 0x0C) return 0;
  switch (usage) {
    case 0x30: return 1u << 0;   // Power
    case 0x40: return 1u << 1;   // Menu
    case 0xB5: return 1u << 4;   // Scan Next
    case 0xB6: return 1u << 5;   // Scan Prev
    case 0xE9: return 1u << 6;   // Volume Up
    case 0xEA: return 1u << 7;   // Volume Down
    case 0xE2: return 1u << 8;   // Mute
    case 0xCD: return 1u << 9;   // Play/Pause
    case 0x223: return 1u << 12;  // AC Home
    case 0x224: return 1u << 13;  // AC Back
    default: return 0;
  }
}

/* 解析报告 → 文本（按键名）+ 第一个可映射 usage 对应的位图。
 * 这就是"学习"的落点：对端按键 → 中文名 + 我们能重放的键。 */
static uint16_t parseReportToMask(const uint8_t *desc, int dlen, const uint8_t *rep, int rlen,
                                  char *out, int outn) {
  if (!out || outn <= 0) return 0;
  out[0] = 0;
  if (!desc || dlen <= 0 || !rep || rlen <= 0) return 0;
  btstack_hid_parser_t parser;
  btstack_hid_parser_init(&parser, desc, (uint16_t)dlen, HID_REPORT_TYPE_INPUT, rep,
                          (uint16_t)rlen);
  int off = 0;
  uint16_t mask = 0;
  while (btstack_hid_parser_has_more(&parser) && off < outn - 24) {
    uint16_t up = 0, u = 0;
    int32_t v = 0;
    btstack_hid_parser_get_field(&parser, &up, &u, &v);
    if (v == 0) continue;
    off += snprintf(out + off, outn - off, "%s ", Bt::usageName(up, u));
    if (!mask) mask = Bt::usageToMask(up, u);
  }
  if (off == 0) snprintf(out, outn, "(未知按键)");
  return mask;
}

/* 自验证：用我们自己的描述符解析几个"构造报告"，确认解析器工作正常。
 * 注意：本机报告 ID 1 是 16 个 bit 的 Consumer 位图，所以报告是 [id][bitmap_lo][bitmap_hi]，
 * 而 btstack_hid_parser 吃的是**不含 report id** 的 payload，所以这里要跳过首字节。 */
void Bt::selfTestParse() {
  int dlen = 0;
  const uint8_t *desc = ourReportMap(&dlen);
  LOGD("PgBt: [自验证] 描述符长度=%d", dlen);
  /* 诊断：btstack 能不能算出各报告的字节数（算不出来说明它没解析懂我们的描述符） */
  {
    int sz1 = btstack_hid_get_report_size_for_id(1, HID_REPORT_TYPE_INPUT, desc, dlen);
    int szu = btstack_hid_get_report_size_for_id(HID_REPORT_ID_UNDEFINED, HID_REPORT_TYPE_INPUT,
                                                 desc, dlen);
    int sz2 = btstack_hid_get_report_size_for_id(2, HID_REPORT_TYPE_INPUT, desc, dlen);
    LOGD("PgBt: [自验证] btstack 算出的 input 报告字节数: id1=%d id2=%d 无id=%d", sz1, sz2, szu);
  }
  struct Case {
    uint8_t rep[3];
    int len;          /* ⚠️ 必须包含 report id：报告 ID 1 是 16 bit 位图 → 共 3 字节 */
    const char *what;
  } cases[] = {
      {{0x01, 0x00, 0x00}, 3, "全 0（无按键）"},
      {{0x01, 0x00, 0x02}, 3, "bit9 -> 期望播放暂停"},
      {{0x01, 0x40, 0x00}, 3, "bit6 -> 期望音量加"},
      {{0x01, 0x80, 0x00}, 3, "bit7 -> 期望音量减"},
      {{0x01, 0x02, 0x00}, 3, "bit1 -> 期望菜单"},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    char buf[128];
    /* ⚠️ 必须传**含 report id 的完整报告**（BLE HID 通知的数据本就带 id，
     *    btstack_hid_parser 也按这个约定解析）。早先我跳过首字节 → 一项都解析不出来。 */
    int n = parseReport(desc, dlen, cases[i].rep, cases[i].len, buf, sizeof(buf));
    LOGD("PgBt: [自验证] %s -> 识别 %d 项: %s", cases[i].what, n, buf);
  }
}

const char *Bt::name() const { return BT_NAME; }

int Bt::errCode() const {
  return impl_ ? static_cast<Impl *>(impl_)->errCode : 0;
}

void Bt::sendKey(uint16_t mask, bool alsoRelease) {
  (void)alsoRelease;  // 松开由 BT 线程的 120ms 定时器自动补
  Impl *im = static_cast<Impl *>(impl_);
  if (!im || im->fds[1] < 0) return;
  PostMsg m;
  memset(&m, 0, sizeof(m));
  m.cmd = CMD_SEND_KEY;
  m.size = 2;
  m.data[0] = (uint8_t)(mask & 0xff);
  m.data[1] = (uint8_t)(mask >> 8);
  ssize_t n = write(im->fds[1], &m, sizeof(m));
  (void)n;
  LOGD("PgBt: UI 投递按键 0x%04x", mask);
}

const char *Bt::keyName(uint16_t mask) {
  switch (mask) {
    case RC_POWER: return "电源";
    case RC_MENU: return "菜单";
    case RC_NEXT: return "下一首";
    case RC_PREV: return "上一首";
    case RC_VOL_UP: return "音量+";
    case RC_VOL_DOWN: return "音量-";
    case RC_MUTE: return "静音";
    case RC_PLAY: return "播放/暂停";
    case RC_HOME: return "主页";
    case RC_BACK: return "返回";
    default: return "自定义";
  }
}

}  // namespace pg
