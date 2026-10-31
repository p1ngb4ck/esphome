#include "hci_split.h"
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT

#include "usb_bluetooth.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "esp_bluedroid_hci.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>

// Linker --wrap: every caller of esp_bluedroid_attach_hci_driver() lands here.
extern "C" esp_err_t __real_esp_bluedroid_attach_hci_driver(const esp_bluedroid_hci_driver_operations_t *ops);
extern "C" esp_err_t __wrap_esp_bluedroid_attach_hci_driver(const esp_bluedroid_hci_driver_operations_t *ops);

namespace esphome::usb_bluetooth::hci_split {

static const char *const TAG = "hci_split";

static constexpr uint8_t H4_COMMAND = 0x01;
static constexpr uint8_t H4_ACL = 0x02;
static constexpr uint8_t H4_EVENT = 0x04;

static constexpr uint8_t SIDE_BR = 0x01;
static constexpr uint8_t SIDE_LE = 0x02;

static constexpr uint8_t EVT_CONNECTION_COMPLETE = 0x03;
static constexpr uint8_t EVT_DISCONNECTION_COMPLETE = 0x05;
static constexpr uint8_t EVT_SYNC_CONNECTION_COMPLETE = 0x2C;
static constexpr uint8_t REASON_CONNECTION_TIMEOUT = 0x08;
static constexpr uint8_t EVT_ENCRYPTION_CHANGE = 0x08;
static constexpr uint8_t EVT_READ_REMOTE_VERSION_COMPLETE = 0x0C;
static constexpr uint8_t EVT_COMMAND_COMPLETE = 0x0E;
static constexpr uint8_t EVT_COMMAND_STATUS = 0x0F;
static constexpr uint8_t EVT_NUM_COMPLETED_PACKETS = 0x13;
static constexpr uint8_t EVT_ENCRYPTION_KEY_REFRESH = 0x30;
static constexpr uint8_t EVT_LE_META = 0x3E;
static constexpr uint8_t EVT_AUTH_PAYLOAD_TIMEOUT = 0x57;
static constexpr uint8_t EVT_ENCRYPTION_CHANGE_V2 = 0x59;

static constexpr uint8_t STATUS_SUCCESS = 0x00;
static constexpr uint8_t STATUS_UNKNOWN_COMMAND = 0x01;

static constexpr uint16_t OP_SET_EVENT_MASK = 0x0C01;
static constexpr uint16_t OP_RESET = 0x0C03;
static constexpr uint16_t OP_SET_C2H_FLOW_CONTROL = 0x0C31;
static constexpr uint16_t OP_HOST_BUFFER_SIZE = 0x0C33;
static constexpr uint16_t OP_HOST_NUM_COMPLETED_PACKETS = 0x0C35;
static constexpr uint16_t OP_SET_EVENT_MASK_PAGE_2 = 0x0C63;
static constexpr uint16_t OP_WRITE_LE_HOST_SUPPORT = 0x0C6D;
static constexpr uint16_t OP_READ_LOCAL_VERSION = 0x1001;
static constexpr uint16_t OP_READ_LOCAL_COMMANDS = 0x1002;
static constexpr uint16_t OP_READ_LOCAL_FEATURES = 0x1003;
static constexpr uint16_t OP_READ_LOCAL_EXT_FEATURES = 0x1004;
static constexpr uint16_t OP_READ_BUFFER_SIZE = 0x1005;
static constexpr uint16_t OP_READ_BD_ADDR = 0x1009;
static constexpr uint16_t OP_LE_READ_BUFFER_SIZE = 0x2002;
// Broadcom's Write BD_ADDR (btbcm_set_bdaddr() in Linux btbcm.c).
static constexpr uint16_t OP_BCM_WRITE_BD_ADDR = 0xFC01;
static constexpr uint16_t MANUFACTURER_BROADCOM = 15;
static constexpr uint8_t OGF_LE = 0x08;
static constexpr uint8_t OGF_VENDOR = 0x3F;

// Feature bits Bluedroid checks (hcidefs.h): page 0 LE Supported (Controller) and
// Simultaneous LE and BR/EDR (Controller); page 1 LE Supported (Host) and Simultaneous (Host).
static constexpr size_t FEATURE_LE_OFF = 4;
static constexpr uint8_t FEATURE_LE_MASK = 0x40;
static constexpr size_t FEATURE_SIMUL_OFF = 6;
static constexpr uint8_t FEATURE_SIMUL_MASK = 0x02;
static constexpr uint8_t HOST_FEATURE_LE = 0x02;
static constexpr uint8_t HOST_FEATURE_SIMUL = 0x04;

static constexpr size_t EVENT_BUF = 1 + 2 + 255;
static constexpr size_t ACL_BUF = 1 + 4 + 1021;

static constexpr size_t MAX_LE_LINKS = 16;
// Host-side numbers for LE links: out of the way of the dongle's, inside the 12-bit range.
static constexpr uint16_t LE_HANDLE_BASE = 0x0E00;
static constexpr uint16_t NO_HANDLE = 0xFFFF;

enum class Route : uint8_t { BR, LE, BOTH, LOCAL };

// One address for Bluedroid: the co-processor's (LE pairing is computed by the host with it). It is
// read and, where the dongle can take another (Broadcom), written to the dongle as well.
enum class AddrStage : uint8_t { NONE, READ_LE, WRITE_BR, READ_BR };

struct Pending {
  uint16_t opcode;
  uint8_t waiting;  // sides still to answer
  bool both;
  bool translate;  // the LE answer carries the connection handle after its status
  uint16_t host_handle;
  AddrStage addr_stage;
  uint8_t answer[2][EVENT_BUF];
  size_t answer_len[2];
  uint8_t addr_answer[EVENT_BUF];
  size_t addr_answer_len;
  bool buffer_from_le;  // Read Buffer Size, answered from the LE controller's
  uint32_t since_ms;    // when the command went out
};

static SemaphoreHandle_t s_lock = nullptr;
static USBBluetooth *volatile s_br = nullptr;
static esp_bluedroid_hci_driver_operations_t s_le_ops{};
static esp_bluedroid_hci_driver_callbacks_t s_host{};
static Pending s_pending{};
static uint16_t s_le_ctrl[MAX_LE_LINKS];  // controller handle per host slot, NO_HANDLE when free
// The dongle's open connections, so Bluedroid can be told they are gone with it.
static constexpr size_t MAX_BR_LINKS = 16;
static uint16_t s_br_links[MAX_BR_LINKS];
static uint8_t s_br_max_page = 0;
// Bluedroid was brought up with the dongle there: its BR/EDR side is usable.
static std::atomic<bool> s_started_with_br{false};
static uint8_t s_le_host_bits = 0;
// Only Bluedroid's HCI task sends; only the esp_hosted RX path receives on the LE side.
static uint8_t s_tx[ACL_BUF];
// The splitter's own commands, sent from whichever side's task answered the one before.
static uint8_t s_own[1 + 3 + 6];
static uint8_t s_le_rx[ACL_BUF];

class Lock {
 public:
  Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_lock); }
};

static void ensure_lock() {
  if (s_lock == nullptr)
    s_lock = xSemaphoreCreateMutex();
}

// -- LE handles (under the lock) -----------------------------------------------------------

static void clear_links() {
  std::fill(std::begin(s_le_ctrl), std::end(s_le_ctrl), NO_HANDLE);
  std::fill(std::begin(s_br_links), std::end(s_br_links), NO_HANDLE);
}

// Under the lock: follow the dongle's connections opening and closing.
static void track_br_event(const uint8_t *ev, size_t len) {
  const uint8_t *p = ev + 3;
  if ((ev[1] == EVT_CONNECTION_COMPLETE || ev[1] == EVT_SYNC_CONNECTION_COMPLETE) && len >= 6 &&
      p[0] == STATUS_SUCCESS) {
    const uint16_t handle = (p[1] | (p[2] << 8)) & 0x0FFF;
    for (auto &slot : s_br_links) {
      if (slot == NO_HANDLE) {
        slot = handle;
        break;
      }
    }
  } else if (ev[1] == EVT_DISCONNECTION_COMPLETE && len >= 6 && p[0] == STATUS_SUCCESS) {
    const uint16_t handle = (p[1] | (p[2] << 8)) & 0x0FFF;
    for (auto &slot : s_br_links) {
      if (slot == handle)
        slot = NO_HANDLE;
    }
  }
}

static uint16_t host_of(uint16_t ctrl) {
  for (size_t i = 0; i < MAX_LE_LINKS; i++) {
    if (s_le_ctrl[i] == ctrl)
      return LE_HANDLE_BASE + i;
  }
  return NO_HANDLE;
}

static uint16_t ctrl_of(uint16_t host) {
  if (host < LE_HANDLE_BASE || host >= LE_HANDLE_BASE + MAX_LE_LINKS)
    return NO_HANDLE;
  return s_le_ctrl[host - LE_HANDLE_BASE];
}

static uint16_t link_up(uint16_t ctrl) {
  const uint16_t known = host_of(ctrl);
  if (known != NO_HANDLE)
    return known;
  for (size_t i = 0; i < MAX_LE_LINKS; i++) {
    if (s_le_ctrl[i] == NO_HANDLE) {
      s_le_ctrl[i] = ctrl;
      return LE_HANDLE_BASE + i;
    }
  }
  ESP_LOGE(TAG, "More than %u LE links", static_cast<unsigned>(MAX_LE_LINKS));
  return NO_HANDLE;
}

static void link_down(uint16_t ctrl) {
  for (auto &slot : s_le_ctrl) {
    if (slot == ctrl)
      slot = NO_HANDLE;
  }
}

// The connection handle at p, from the LE controller's number to Bluedroid's.
static void to_host(uint8_t *p) {
  const uint16_t ctrl = (p[0] | (p[1] << 8)) & 0x0FFF;
  const uint16_t host = host_of(ctrl);
  if (host == NO_HANDLE)
    return;
  p[0] = static_cast<uint8_t>(host & 0xFF);
  p[1] = static_cast<uint8_t>((p[1] & 0xF0) | (host >> 8));
}

// -- ACL reassembly check -----------------------------------------------------------------

// Per side, the L2CAP frames still waiting for continuations. Each table is touched only by the
// task its side receives on (USB task, esp_hosted RX task); the counts are read by the main loop.
struct OpenFrame {
  uint16_t handle{NO_HANDLE};
  uint16_t remaining{0};
};
struct AclCheck {
  OpenFrame open[MAX_LE_LINKS];
  std::atomic<uint32_t> orphans{0};  // continuation with no frame open on its handle
  std::atomic<uint32_t> cut{0};      // a new frame started before the open one was complete
  std::atomic<uint16_t> handle{0};   // the last handle either happened on
};
static AclCheck s_check_br;
static AclCheck s_check_le;

static void check_acl(AclCheck &check, const uint8_t *h4, size_t len) {
  if (len < 5)
    return;
  const uint16_t handle = (h4[1] | (h4[2] << 8)) & 0x0FFF;
  const uint8_t boundary = (h4[2] >> 4) & 0x03;
  const uint16_t acl_len = h4[3] | (h4[4] << 8);
  OpenFrame *frame = nullptr;
  OpenFrame *spare = nullptr;
  for (auto &entry : check.open) {
    if (entry.handle == handle)
      frame = &entry;
    else if (entry.handle == NO_HANDLE && spare == nullptr)
      spare = &entry;
  }
  if (boundary == 0x01) {
    if (frame == nullptr) {
      check.orphans.fetch_add(1, std::memory_order_relaxed);
      check.handle.store(handle, std::memory_order_relaxed);
    } else if (acl_len >= frame->remaining) {
      frame->handle = NO_HANDLE;
    } else {
      frame->remaining -= acl_len;
    }
    return;
  }
  if (frame != nullptr) {
    check.cut.fetch_add(1, std::memory_order_relaxed);
    check.handle.store(handle, std::memory_order_relaxed);
    frame->handle = NO_HANDLE;
    spare = frame;
  }
  if (acl_len < 4 || len < 9)
    return;
  const uint32_t total = 4u + (h4[5] | (h4[6] << 8));
  if (total <= acl_len || spare == nullptr)
    return;
  spare->handle = handle;
  spare->remaining = static_cast<uint16_t>(total - acl_len);
}

static void report_side(AclCheck &check, const char *side) {
  const uint32_t orphans = check.orphans.exchange(0, std::memory_order_relaxed);
  const uint32_t cut = check.cut.exchange(0, std::memory_order_relaxed);
  if (orphans == 0 && cut == 0)
    return;
  ESP_LOGV(TAG,
           "ACL from the %s: %" PRIu32 " continuation(s) without a start, %" PRIu32
           " frame(s) cut short, last on handle 0x%03X",
           side, orphans, cut, check.handle.load(std::memory_order_relaxed));
}

void report_acl() {
  report_side(s_check_br, "dongle");
  report_side(s_check_le, "co-processor");
  uint16_t opcode;
  uint8_t waiting;
  uint32_t age;
  {
    Lock lock;
    opcode = s_pending.opcode;
    waiting = s_pending.waiting;
    age = millis() - s_pending.since_ms;
  }
  // Bluedroid sends the next command only once this one is answered.
  if (opcode != 0 && waiting != 0 && age > 5000)
    ESP_LOGW(TAG, "HCI command 0x%04X unanswered for %" PRIu32 " ms (waiting for the%s%s)", opcode, age,
             (waiting & SIDE_BR) ? " dongle" : "", (waiting & SIDE_LE) ? " co-processor" : "");
}

// -- To Bluedroid --------------------------------------------------------------------------

static void deliver(uint8_t *h4, size_t len) {
  if (s_host.notify_host_recv != nullptr)
    s_host.notify_host_recv(h4, static_cast<uint16_t>(len));
}

static size_t command_complete(uint8_t *out, uint16_t opcode, const uint8_t *ret, size_t ret_len) {
  out[0] = H4_EVENT;
  out[1] = EVT_COMMAND_COMPLETE;
  out[2] = static_cast<uint8_t>(3 + ret_len);
  out[3] = 1;
  out[4] = static_cast<uint8_t>(opcode & 0xFF);
  out[5] = static_cast<uint8_t>(opcode >> 8);
  memcpy(out + 6, ret, ret_len);
  return 6 + ret_len;
}

// -- To the controllers --------------------------------------------------------------------

static bool send_side(uint8_t side, uint8_t *h4, size_t len) {
  if (side == SIDE_BR) {
    USBBluetooth *br = s_br;
    if (br == nullptr)
      return false;
    br->hci_send(h4, static_cast<uint16_t>(len));
    return true;
  }
  if (s_le_ops.send == nullptr)
    return false;
  s_le_ops.send(h4, static_cast<uint16_t>(len));
  return true;
}

static Route route_for(uint16_t opcode) {
  switch (opcode) {
    case OP_RESET:
    case OP_SET_EVENT_MASK:
    case OP_SET_EVENT_MASK_PAGE_2:
    case OP_SET_C2H_FLOW_CONTROL:
    case OP_HOST_BUFFER_SIZE:
    case OP_READ_LOCAL_COMMANDS:
      return Route::BOTH;
    // An LE-only controller has nothing to do with it; the feature bits it sets are ours.
    case OP_WRITE_LE_HOST_SUPPORT:
      return Route::LOCAL;
    case OP_READ_LOCAL_VERSION:
      return Route::LE;
    default:
      break;
  }
  const uint8_t ogf = opcode >> 10;
  return ogf == OGF_LE || ogf == OGF_VENDOR ? Route::LE : Route::BR;
}

// Commands whose first parameter is a connection handle and which an LE link can be named in.
static bool names_connection(uint16_t opcode) {
  switch (opcode) {
    case 0x0406:  // Disconnect
    case 0x041D:  // Read Remote Version Information
    case 0x0C08:  // Flush
    case 0x0C2D:  // Read Transmit Power Level
    case 0x0C36:  // Read Link Supervision Timeout
    case 0x0C37:  // Write Link Supervision Timeout
    case 0x0C7B:  // Read Authenticated Payload Timeout
    case 0x0C7C:  // Write Authenticated Payload Timeout
    case 0x1403:  // Read Link Quality
    case 0x1405:  // Read RSSI
    case 0x2013:  // LE Connection Update
    case 0x2015:  // LE Read Channel Map
    case 0x2016:  // LE Read Remote Features
    case 0x2019:  // LE Enable Encryption
    case 0x201A:  // LE Long Term Key Request Reply
    case 0x201B:  // LE Long Term Key Request Negative Reply
    case 0x2020:  // LE Remote Connection Parameter Request Reply
    case 0x2021:  // LE Remote Connection Parameter Request Negative Reply
    case 0x2022:  // LE Set Data Length
    case 0x2030:  // LE Read PHY
    case 0x2032:  // LE Set PHY
    case 0x205A:  // LE Periodic Advertising Sync Transfer
    case 0x205B:  // LE Periodic Advertising Set Info Transfer
    case 0x205C:  // LE Set Periodic Advertising Sync Transfer Parameters
    case 0x2076:  // LE Enhanced Read Transmit Power Level
    case 0x2077:  // LE Read Remote Transmit Power Level
    case 0x207E:  // LE Subrate Request
      return true;
    default:
      return false;
  }
}

static void answer_locally(uint16_t opcode, const uint8_t *params, size_t plen) {
  uint8_t ret[1 + 2 + 8]{};
  size_t ret_len = 1;
  if (opcode == OP_READ_LOCAL_EXT_FEATURES && plen >= 1) {
    // A page the dongle does not have: page 1 holds the host features Bluedroid set. Without a
    // dongle every page is answered here, page 0 with LE alone.
    const bool no_br = s_br == nullptr;
    ret[1] = params[0];
    ret[2] = no_br ? 1 : std::max<uint8_t>(s_br_max_page, 1);
    ret[3] = params[0] == 1 ? s_le_host_bits : 0;
    if (no_br && params[0] == 0)
      ret[3 + FEATURE_LE_OFF] |= FEATURE_LE_MASK;
    ret_len = sizeof(ret);
  }
  uint8_t out[EVENT_BUF];
  {
    Lock lock;
    s_pending.opcode = 0;
    s_pending.waiting = 0;
  }
  deliver(out, command_complete(out, opcode, ret, ret_len));
}

// Bluedroid sends this without waiting for an answer: split by side, LE handles renumbered.
static void send_host_completed(const uint8_t *data, size_t len) {
  if (len < 5)
    return;
  const uint8_t count = data[4];
  if (len < 5u + 4u * count)
    return;
  uint8_t br[EVENT_BUF];
  uint8_t le[EVENT_BUF];
  uint8_t n_br = 0;
  uint8_t n_le = 0;
  {
    Lock lock;
    for (uint8_t i = 0; i < count; i++) {
      const uint8_t *entry = data + 5 + 4 * i;
      const uint16_t host = (entry[0] | (entry[1] << 8)) & 0x0FFF;
      const uint16_t ctrl = ctrl_of(host);
      uint8_t *to = ctrl == NO_HANDLE ? br + 5 + 4 * n_br++ : le + 5 + 4 * n_le++;
      memcpy(to, entry, 4);
      if (ctrl != NO_HANDLE) {
        to[0] = static_cast<uint8_t>(ctrl & 0xFF);
        to[1] = static_cast<uint8_t>(ctrl >> 8);
      }
    }
  }
  auto send = [](uint8_t side, uint8_t *buf, uint8_t n) {
    if (n == 0)
      return;
    buf[0] = H4_COMMAND;
    buf[1] = static_cast<uint8_t>(OP_HOST_NUM_COMPLETED_PACKETS & 0xFF);
    buf[2] = static_cast<uint8_t>(OP_HOST_NUM_COMPLETED_PACKETS >> 8);
    buf[3] = static_cast<uint8_t>(1 + 4 * n);
    buf[4] = n;
    send_side(side, buf, 5 + 4 * n);
  };
  send(SIDE_BR, br, n_br);
  send(SIDE_LE, le, n_le);
}

static void answer_missing(uint8_t side);
static size_t own_command(uint16_t opcode, const uint8_t *params, uint8_t plen);

static void send_command(uint8_t *data, size_t len) {
  if (len < 4)
    return;
  const uint16_t opcode = static_cast<uint16_t>(data[1] | (data[2] << 8));
  const uint8_t *params = data + 4;
  const size_t plen = len - 4;
  if (opcode == OP_HOST_NUM_COMPLETED_PACKETS) {
    send_host_completed(data, len);
    return;
  }
  Route route = route_for(opcode);
  uint8_t *out = data;
  bool translate = false;
  bool buffer_from_le = false;
  uint16_t host_handle = 0;
  {
    Lock lock;
    USBBluetooth *br = s_br;
    if (br == nullptr) {
      // No dongle: Bluedroid has the LE controller alone. It takes its address, its buffers stand
      // in for the BR/EDR ones Bluedroid starts with, and the feature pages say LE only.
      if (opcode == OP_READ_BD_ADDR) {
        route = Route::LE;
      } else if (opcode == OP_READ_LOCAL_EXT_FEATURES) {
        route = Route::LOCAL;
      } else if (opcode == OP_READ_BUFFER_SIZE) {
        route = Route::LE;
        buffer_from_le = true;
      }
    }
    if (names_connection(opcode) && plen >= 2 && len <= sizeof(s_tx)) {
      host_handle = (params[0] | (params[1] << 8)) & 0x0FFF;
      const uint16_t ctrl = ctrl_of(host_handle);
      if (ctrl != NO_HANDLE) {
        route = Route::LE;
        translate = true;
        memcpy(s_tx, data, len);
        s_tx[4] = static_cast<uint8_t>(ctrl & 0xFF);
        s_tx[5] = static_cast<uint8_t>(ctrl >> 8);
        out = s_tx;
      }
    }
    if (opcode == OP_WRITE_LE_HOST_SUPPORT && plen >= 2)
      s_le_host_bits = (params[0] != 0 ? HOST_FEATURE_LE : 0) | (params[1] != 0 ? HOST_FEATURE_SIMUL : 0);
    if (opcode == OP_READ_LOCAL_EXT_FEATURES && plen >= 1 && params[0] > s_br_max_page)
      route = Route::LOCAL;
    if (opcode == OP_RESET)
      clear_links();
    s_pending.addr_stage = AddrStage::NONE;
    if (opcode == OP_READ_BD_ADDR && br != nullptr) {
      // Bluedroid's address is the co-processor's: LE pairing (computed by the host) has to use the
      // address the LE radio sends with. A Broadcom dongle is given it too; any other keeps its own,
      // which only its BR/EDR pairing (computed by the dongle) uses.
      route = Route::LE;
      if (br->manufacturer() == MANUFACTURER_BROADCOM)
        s_pending.addr_stage = AddrStage::READ_LE;
    }
    s_pending.buffer_from_le = buffer_from_le;
    s_pending.opcode = buffer_from_le ? OP_LE_READ_BUFFER_SIZE : opcode;
    s_pending.since_ms = millis();
    s_pending.both = route == Route::BOTH;
    s_pending.translate = translate;
    s_pending.host_handle = host_handle;
    s_pending.answer_len[0] = s_pending.answer_len[1] = 0;
    s_pending.waiting = route == Route::BR     ? SIDE_BR
                        : route == Route::LE   ? SIDE_LE
                        : route == Route::BOTH ? SIDE_BR | SIDE_LE
                                               : 0;
  }
  if (route == Route::LOCAL) {
    answer_locally(opcode, params, plen);
    return;
  }
  if (buffer_from_le) {
    len = own_command(OP_LE_READ_BUFFER_SIZE, nullptr, 0);
    out = s_own;
  }
  for (uint8_t side : {SIDE_BR, SIDE_LE}) {
    if ((route == Route::BOTH || (side == SIDE_BR) == (route == Route::BR)) && !send_side(side, out, len))
      answer_missing(side);
  }
}

static void send_acl(uint8_t *data, size_t len) {
  if (len < 5)
    return;
  uint16_t ctrl;
  {
    Lock lock;
    ctrl = ctrl_of((data[1] | (data[2] << 8)) & 0x0FFF);
  }
  if (ctrl == NO_HANDLE) {
    send_side(SIDE_BR, data, len);
    return;
  }
  if (len > sizeof(s_tx))
    return;
  memcpy(s_tx, data, len);
  s_tx[1] = static_cast<uint8_t>(ctrl & 0xFF);
  s_tx[2] = static_cast<uint8_t>((data[2] & 0xF0) | (ctrl >> 8));
  send_side(SIDE_LE, s_tx, len);
}

// -- From the controllers ------------------------------------------------------------------

// What Bluedroid needs to see in the dongle's feature pages: LE there, on the other radio.
static void patch_features(uint16_t opcode, uint8_t *ev, size_t len) {
  if (len < 7 || ev[6] != STATUS_SUCCESS)
    return;
  if (opcode == OP_READ_LOCAL_FEATURES && len >= 7 + 8) {
    ev[7 + FEATURE_LE_OFF] |= FEATURE_LE_MASK;
    ev[7 + FEATURE_SIMUL_OFF] |= FEATURE_SIMUL_MASK;
  } else if (opcode == OP_READ_LOCAL_EXT_FEATURES && len >= 9 + 8) {
    uint8_t *features = ev + 9;
    if (ev[7] == 0) {
      s_br_max_page = ev[8];
      features[FEATURE_LE_OFF] |= FEATURE_LE_MASK;
      features[FEATURE_SIMUL_OFF] |= FEATURE_SIMUL_MASK;
    } else if (ev[7] == 1) {
      features[0] |= s_le_host_bits;
    }
    ev[8] = std::max<uint8_t>(ev[8], 1);
  }
}

static uint8_t answer_status(const uint8_t *ev, size_t len) {
  if (len < 4)
    return STATUS_UNKNOWN_COMMAND;
  if (ev[1] == EVT_COMMAND_STATUS)
    return ev[3];
  return len >= 7 ? ev[6] : STATUS_SUCCESS;
}

// Under the lock, both sides in: one answer for Bluedroid.
static size_t merge_answers(uint8_t *out) {
  const size_t br_len = s_pending.answer_len[0];
  const size_t le_len = s_pending.answer_len[1];
  const uint8_t br_status = answer_status(s_pending.answer[0], br_len);
  const uint8_t le_status = answer_status(s_pending.answer[1], le_len);
  // The dongle's answer unless it does not know the command; a side that failed otherwise is
  // only logged, since Bluedroid takes a failed start-up command as fatal.
  const bool use_le = br_len == 0 || (br_status == STATUS_UNKNOWN_COMMAND && le_len != 0);
  const uint8_t *base = use_le ? s_pending.answer[1] : s_pending.answer[0];
  const size_t len = use_le ? le_len : br_len;
  memcpy(out, base, len);
  const uint8_t other = use_le ? br_status : le_status;
  if (other != STATUS_SUCCESS && other != STATUS_UNKNOWN_COMMAND)
    ESP_LOGV(TAG, "Command 0x%04X failed on the %s controller (0x%02X)", s_pending.opcode, use_le ? "BR/EDR" : "LE",
             other);
  if (s_pending.opcode == OP_READ_LOCAL_COMMANDS && br_status == STATUS_SUCCESS && le_status == STATUS_SUCCESS) {
    const size_t n = std::min(br_len, le_len);
    for (size_t i = 7; i < n; i++)
      out[i] = s_pending.answer[0][i] | s_pending.answer[1][i];
  }
  return len;
}

static size_t own_command(uint16_t opcode, const uint8_t *params, uint8_t plen) {
  s_own[0] = H4_COMMAND;
  s_own[1] = static_cast<uint8_t>(opcode & 0xFF);
  s_own[2] = static_cast<uint8_t>(opcode >> 8);
  s_own[3] = plen;
  if (plen != 0)
    memcpy(s_own + 4, params, plen);
  return 4 + plen;
}

// Under the lock: the next step of the address handover, or false when the answer is not
// part of it. own_len is set when a command of the splitter's has to go to the dongle.
static bool address_step(uint16_t opcode, const uint8_t *ev, size_t len, uint8_t *out, size_t &out_len,
                         size_t &own_len) {
  own_len = 0;
  const bool ok = len >= 7 && ev[1] == EVT_COMMAND_COMPLETE && ev[6] == STATUS_SUCCESS;
  if (s_pending.addr_stage == AddrStage::READ_LE && opcode == OP_READ_BD_ADDR) {
    if (ok && len >= 13) {
      memcpy(s_pending.addr_answer, ev, len);
      s_pending.addr_answer_len = len;
      s_pending.addr_stage = AddrStage::WRITE_BR;
      s_pending.opcode = OP_BCM_WRITE_BD_ADDR;
      own_len = own_command(OP_BCM_WRITE_BD_ADDR, ev + 7, 6);
    } else {
      s_pending.addr_stage = AddrStage::READ_BR;
      own_len = own_command(OP_READ_BD_ADDR, nullptr, 0);
    }
    s_pending.waiting = SIDE_BR;
    out_len = 0;
    return true;
  }
  if (s_pending.addr_stage == AddrStage::WRITE_BR && opcode == OP_BCM_WRITE_BD_ADDR) {
    if (ok) {
      const uint8_t *a = s_pending.addr_answer + 7;
      ESP_LOGV(TAG, "Both radios on %02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
      out_len = s_pending.addr_answer_len;
      memcpy(out, s_pending.addr_answer, out_len);
      out[3] = 1;
      s_pending.addr_stage = AddrStage::NONE;
      s_pending.waiting = 0;
      s_pending.opcode = 0;
      return true;
    }
    ESP_LOGV(TAG, "The dongle keeps its own address");
    out_len = s_pending.addr_answer_len;
    memcpy(out, s_pending.addr_answer, out_len);
    out[3] = 1;
    s_pending.addr_stage = AddrStage::NONE;
    s_pending.waiting = 0;
    s_pending.opcode = 0;
    return true;
  }
  return false;
}

static void on_command_answer(uint8_t side, const uint8_t *ev, size_t len) {
  const bool complete = ev[1] == EVT_COMMAND_COMPLETE;
  if (len < (complete ? 6u : 7u))
    return;
  const size_t credits_at = complete ? 3 : 4;
  const uint16_t opcode = static_cast<uint16_t>(ev[credits_at + 1] | (ev[credits_at + 2] << 8));
  // The controllers' own "ready for commands": Bluedroid starts with one credit anyway.
  if (opcode == 0)
    return;
  uint8_t out[EVENT_BUF];
  size_t out_len = std::min(len, sizeof(out));
  size_t own_len = 0;
  {
    Lock lock;
    if (s_pending.buffer_from_le && s_pending.opcode == opcode && side == SIDE_LE && complete) {
      // LE Read Buffer Size as Read Buffer Size: the LE ACL packets, no SCO.
      const bool ok = len >= 10 && ev[6] == STATUS_SUCCESS;
      const uint8_t ret[8] = {ok ? STATUS_SUCCESS : (len >= 7 ? ev[6] : STATUS_UNKNOWN_COMMAND),
                              ok ? ev[7] : static_cast<uint8_t>(0),
                              ok ? ev[8] : static_cast<uint8_t>(0),
                              0,
                              ok ? ev[9] : static_cast<uint8_t>(0),
                              0,
                              0,
                              0};
      out_len = command_complete(out, OP_READ_BUFFER_SIZE, ret, sizeof(ret));
      s_pending.buffer_from_le = false;
      s_pending.waiting = 0;
      s_pending.opcode = 0;
    } else if (s_pending.opcode == opcode && (s_pending.waiting & side) != 0 &&
        address_step(opcode, ev, std::min(len, sizeof(out)), out, out_len, own_len)) {
      // Handled: either the answer below, or the next command of the handover.
    } else if (s_pending.opcode != opcode || (s_pending.waiting & side) == 0) {
      memcpy(out, ev, out_len);
      out[credits_at] = s_pending.waiting != 0 ? 0 : 1;
    } else if (s_pending.both) {
      const size_t index = side == SIDE_BR ? 0 : 1;
      memcpy(s_pending.answer[index], ev, out_len);
      s_pending.answer_len[index] = out_len;
      s_pending.waiting &= ~side;
      if (s_pending.waiting != 0)
        return;
      out_len = merge_answers(out);
      out[credits_at] = 1;
      s_pending.opcode = 0;
    } else {
      memcpy(out, ev, out_len);
      out[credits_at] = 1;
      if (complete && side == SIDE_LE && s_pending.translate && out_len >= 9) {
        out[7] = static_cast<uint8_t>(s_pending.host_handle & 0xFF);
        out[8] = static_cast<uint8_t>(s_pending.host_handle >> 8);
      }
      if (complete && side == SIDE_BR)
        patch_features(opcode, out, out_len);
      s_pending.waiting = 0;
      s_pending.opcode = 0;
      s_pending.addr_stage = AddrStage::NONE;
    }
  }
  if (own_len != 0) {
    if (!send_side(SIDE_BR, s_own, own_len))
      answer_missing(SIDE_BR);
    return;
  }
  if (out_len != 0)
    deliver(out, out_len);
}

// A side that is not there: its answer is "unknown command".
static void answer_missing(uint8_t side) {
  uint8_t ev[7];
  const uint8_t status = STATUS_UNKNOWN_COMMAND;
  command_complete(ev, s_pending.opcode, &status, 1);
  on_command_answer(side, ev, sizeof(ev));
}

static bool le_event_names_connection(const uint8_t *ev, size_t len) {
  switch (ev[1]) {
    case EVT_DISCONNECTION_COMPLETE:
    case EVT_ENCRYPTION_CHANGE:
    case EVT_READ_REMOTE_VERSION_COMPLETE:
    case EVT_NUM_COMPLETED_PACKETS:
    case EVT_ENCRYPTION_KEY_REFRESH:
    case EVT_AUTH_PAYLOAD_TIMEOUT:
    case EVT_ENCRYPTION_CHANGE_V2:
      return true;
    case EVT_LE_META:
      if (len < 4)
        return false;
      switch (ev[3]) {
        case 0x01:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x0A:
        case 0x0C:
        case 0x12:
        case 0x14:
        case 0x23:
        case 0x29:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

// Under the lock: the LE controller's connection handles in this event to Bluedroid's.
static void renumber_le_event(uint8_t *ev, size_t len) {
  uint8_t *p = ev + 3;
  const size_t plen = len - 3;
  switch (ev[1]) {
    case EVT_DISCONNECTION_COMPLETE:
      if (plen >= 3) {
        const uint16_t ctrl = (p[1] | (p[2] << 8)) & 0x0FFF;
        to_host(p + 1);
        link_down(ctrl);
      }
      return;
    case EVT_ENCRYPTION_CHANGE:
    case EVT_READ_REMOTE_VERSION_COMPLETE:
    case EVT_ENCRYPTION_KEY_REFRESH:
    case EVT_ENCRYPTION_CHANGE_V2:
      if (plen >= 3)
        to_host(p + 1);
      return;
    case EVT_AUTH_PAYLOAD_TIMEOUT:
      if (plen >= 2)
        to_host(p);
      return;
    case EVT_NUM_COMPLETED_PACKETS:
      for (size_t i = 0; plen >= 1 && i < p[0] && 1 + 4 * i + 2 <= plen; i++)
        to_host(p + 1 + 4 * i);
      return;
    default:
      break;
  }
  switch (p[0]) {
    // Connection complete (legacy, enhanced, enhanced v2): status, handle.
    case 0x01:
    case 0x0A:
    case 0x29:
      if (plen >= 4 && p[1] == STATUS_SUCCESS)
        link_up((p[2] | (p[3] << 8)) & 0x0FFF);
      if (plen >= 4)
        to_host(p + 2);
      return;
    // Status, handle: connection update, remote features, PHY update, subrate change.
    case 0x03:
    case 0x04:
    case 0x0C:
    case 0x23:
      if (plen >= 4)
        to_host(p + 2);
      return;
    // Handle first: LTK request, connection parameter request, data length, channel algorithm.
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x14:
      if (plen >= 3)
        to_host(p + 1);
      return;
    // Advertising set terminated: status, advertising handle, connection handle.
    case 0x12:
      if (plen >= 5)
        to_host(p + 3);
      return;
    default:
      return;
  }
}

static void on_event(uint8_t side, uint8_t *ev, size_t len) {
  if (len < 3)
    return;
  if (ev[1] == EVT_COMMAND_COMPLETE || ev[1] == EVT_COMMAND_STATUS) {
    on_command_answer(side, ev, len);
    return;
  }
  if (side == SIDE_BR) {
    if (ev[1] == EVT_CONNECTION_COMPLETE || ev[1] == EVT_SYNC_CONNECTION_COMPLETE ||
        ev[1] == EVT_DISCONNECTION_COMPLETE) {
      Lock lock;
      track_br_event(ev, len);
    }
    deliver(ev, len);
    return;
  }
  if (!le_event_names_connection(ev, len)) {
    deliver(ev, len);
    return;
  }
  uint8_t out[EVENT_BUF];
  const size_t n = std::min(len, sizeof(out));
  memcpy(out, ev, n);
  {
    Lock lock;
    renumber_le_event(out, n);
  }
  deliver(out, n);
}

// -- Bluedroid's transport -----------------------------------------------------------------

static void split_send(uint8_t *data, uint16_t len) {
  if (data == nullptr || len < 2)
    return;
  switch (data[0]) {
    case H4_COMMAND:
      send_command(data, len);
      break;
    case H4_ACL:
      send_acl(data, len);
      break;
    default:
      send_side(SIDE_BR, data, len);
      break;
  }
}

static bool split_check_send_available() {
  USBBluetooth *br = s_br;
  const bool br_ok = br == nullptr || br->hci_send_available();
  const bool le_ok = s_le_ops.check_send_available == nullptr || s_le_ops.check_send_available();
  return br_ok && le_ok;
}

static void notify_send_available() {
  if (s_host.notify_host_send_available != nullptr)
    s_host.notify_host_send_available();
}

// esp_hosted's RX path.
static int le_recv(uint8_t *data, uint16_t len) {
  if (data == nullptr || len < 2)
    return -1;
  if (data[0] == H4_EVENT) {
    on_event(SIDE_LE, data, len);
    return 0;
  }
  if (data[0] == H4_ACL && len >= 5 && len <= sizeof(s_le_rx)) {
    memcpy(s_le_rx, data, len);
    {
      Lock lock;
      to_host(s_le_rx + 1);
    }
    check_acl(s_check_le, s_le_rx, len);
    deliver(s_le_rx, len);
    return 0;
  }
  deliver(data, len);
  return 0;
}

static const esp_bluedroid_hci_driver_callbacks_t LE_CALLBACKS = {
    .notify_host_send_available = notify_send_available,
    .notify_host_recv = le_recv,
};

static esp_err_t split_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback) {
  if (callback == nullptr) {
    s_host = {};
    return ESP_OK;
  }
  s_host = *callback;
  if (s_le_ops.register_host_callback != nullptr)
    return s_le_ops.register_host_callback(&LE_CALLBACKS);
  return ESP_OK;
}

static const esp_bluedroid_hci_driver_operations_t SPLIT_OPS = {
    .send = split_send,
    .check_send_available = split_check_send_available,
    .register_host_callback = split_register_host_callback,
};

void set_br(USBBluetooth *br) {
  ensure_lock();
  s_br = br;
  if (br != nullptr)
    return;
  s_started_with_br.store(false, std::memory_order_release);
  // The dongle is gone, and its connections with it: say so, rather than leave Bluedroid
  // waiting on links nothing answers for.
  uint16_t gone[MAX_BR_LINKS];
  size_t count = 0;
  {
    Lock lock;
    for (auto &slot : s_br_links) {
      if (slot != NO_HANDLE)
        gone[count++] = slot;
      slot = NO_HANDLE;
    }
  }
  for (size_t i = 0; i < count; i++) {
    uint8_t ev[7] = {H4_EVENT,
                     EVT_DISCONNECTION_COMPLETE,
                     4,
                     STATUS_SUCCESS,
                     static_cast<uint8_t>(gone[i] & 0xFF),
                     static_cast<uint8_t>(gone[i] >> 8),
                     REASON_CONNECTION_TIMEOUT};
    deliver(ev, sizeof(ev));
  }
}

void br_recv(uint8_t *h4, size_t len) {
  if (len >= 3 && h4[0] == H4_EVENT) {
    on_event(SIDE_BR, h4, len);
    return;
  }
  if (h4[0] == H4_ACL)
    check_acl(s_check_br, h4, len);
  deliver(h4, len);
}

void br_send_available() { notify_send_available(); }

bool br_up() { return s_started_with_br.load(std::memory_order_acquire); }

}  // namespace esphome::usb_bluetooth::hci_split

extern "C" esp_err_t __wrap_esp_bluedroid_attach_hci_driver(const esp_bluedroid_hci_driver_operations_t *ops) {
  using namespace esphome::usb_bluetooth::hci_split;
  if (ops == nullptr || ops == &SPLIT_OPS)
    return __real_esp_bluedroid_attach_hci_driver(ops);
  // esp32_ble's esp_hosted transport: the LE side.
  ensure_lock();
  s_le_ops = *ops;
  clear_links();
  const bool with_br = s_br != nullptr;
  s_started_with_br.store(with_br, std::memory_order_release);
  if (with_br) {
    ESP_LOGV(TAG, "Bluedroid on two controllers: LE on the co-processor, BR/EDR on the USB dongle");
  } else {
    ESP_LOGV(TAG, "Bluedroid on the co-processor alone (LE); BR/EDR once the USB dongle is there");
  }
  return __real_esp_bluedroid_attach_hci_driver(&SPLIT_OPS);
}

#endif  // USE_USB_BLUETOOTH_HCI_SPLIT
