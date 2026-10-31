#include "bt_hid.h"

#ifdef USE_ESP32

#include "esphome/components/bt_classic/bt_classic.h"
#ifdef USE_BT_HID_SELECT
#include "select/bt_hid_device_select.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_bt_device.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh_api.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <deque>
#include <memory>

namespace esphome::bt_hid {

static const char *const TAG = "bt_hid";

static constexpr uint32_t RETRY_MIN_MS = 5000;
static constexpr uint32_t RETRY_MAX_MS = 60000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr size_t MAX_QUEUED_EVENTS = 64;

namespace {

// Bluedroid calls back on its BTC task; everything is handed to the main loop as one of these.
struct HostEvent {
  enum Type : uint8_t {
    INIT,
    OPEN,
    CLOSE,
    DESCRIPTOR,
    DATA,
    FEATURE,
    PIN,
    PASSKEY,
    ADOPTED,
    LISTED,
    AUTH,
    FOUND,
    SCAN_STATE,
    CLAIMED,
    SIGNAL
  };
  Type type{INIT};
  uint8_t handle{0xFF};
  uint8_t conn{0};  // OPEN: esp_hidh_connection_state_t
  esp_bd_addr_t bda{};
  int status{0};
  uint32_t value{0};
  int8_t rssi{0};
  bool has_rssi{false};
  std::vector<uint8_t> data;
};

// The HID host and Classic GAP callbacks are global to Bluedroid, so one host serves every
// configured device. The device list is complete before Bluedroid comes up and never changes
// after; device addresses do change (pairing), so they are read and written under the lock.
struct Host {
  Mutex lock;
  std::deque<HostEvent> events;
  std::vector<BTHIDDevice *> devices;
  BTHIDDevice *pairing{nullptr};
  uint32_t pairing_until{0};
  bool started{false};
  bool ready{false};
  bool connecting{false};
  bool discovering{false};
  bool scan_requested{false};  // bt_hid.scan: its finds are logged
  uint32_t connecting_since{0};
  // Shared slots (bt_pairing's): their paired devices (under the lock, the BTC task reads it),
  // the ones disconnected on request (main loop), and the ones still to be paged once (main loop).
  std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> paired;
  std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> refused;
  std::deque<std::array<uint8_t, ESP_BD_ADDR_LEN>> page_queue;
  bool queue_built{false};
};

bool in_list(const std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> &list, const uint8_t *bda) {
  for (const auto &entry : list) {
    if (memcmp(entry.data(), bda, ESP_BD_ADDR_LEN) == 0)
      return true;
  }
  return false;
}

Host &host() {
  static Host instance;
  return instance;
}

// Never called with the lock held.
BTHIDDevice *find_by_address(const uint8_t *bda) {
  LockGuard guard(host().lock);
  for (auto *dev : host().devices) {
    if (dev->has_address() && memcmp(dev->address(), bda, ESP_BD_ADDR_LEN) == 0)
      return dev;
  }
  return nullptr;
}

void push(HostEvent &&event);

// A device not configured here that pairs or connects on its own while a slot's pairing window
// is open (it was in pairing mode already, or found us first): the slot takes it.
// A paired device of the shared slots takes a free one of them.
BTHIDDevice *find_or_adopt(const uint8_t *bda) {
  if (BTHIDDevice *dev = find_by_address(bda))
    return dev;
  BTHIDDevice *slot = nullptr;
  HostEvent::Type type = HostEvent::ADOPTED;
  {
    LockGuard guard(host().lock);
    if (in_list(host().paired, bda)) {
      for (auto *dev : host().devices) {
        if (dev->is_pooled() && !dev->has_address() && host().pairing != dev) {
          slot = dev;
          break;
        }
      }
      if (slot == nullptr)
        return nullptr;
      type = HostEvent::CLAIMED;
    } else {
      slot = host().pairing;
      // With a select the device is only listed, see note_connected().
      if (slot == nullptr || slot->has_fixed_address() || slot->has_address() || slot->has_device_select())
        return nullptr;
    }
    slot->adopt_address(bda);
  }
  HostEvent e;
  e.type = type;
  memcpy(e.bda, bda, ESP_BD_ADDR_LEN);
  push(std::move(e));
  return slot;
}

// A device connected on its own: an open slot takes it, or with a select lists it.
void note_connected(const uint8_t *bda) {
  if (find_by_address(bda) != nullptr)
    return;
  bool listed = false;
  {
    LockGuard guard(host().lock);
    BTHIDDevice *slot = host().pairing;
    listed = slot != nullptr && slot->has_device_select() && !slot->has_address() && !in_list(host().paired, bda);
  }
  if (!listed) {
    find_or_adopt(bda);
    return;
  }
  HostEvent e;
  e.type = HostEvent::LISTED;
  memcpy(e.bda, bda, ESP_BD_ADDR_LEN);
  push(std::move(e));
}

// Bluedroid's bond store.
bool is_bonded(const uint8_t *bda) {
  int num = esp_bt_gap_get_bond_device_num();
  if (num <= 0)
    return false;
  std::unique_ptr<esp_bd_addr_t[]> list(new esp_bd_addr_t[num]);
  if (esp_bt_gap_get_bond_device_list(&num, list.get()) != ESP_OK)
    return false;
  for (int i = 0; i < num; i++) {
    if (memcmp(list[i], bda, ESP_BD_ADDR_LEN) == 0)
      return true;
  }
  return false;
}

bool parse_mac(const std::string &text, uint8_t *bda) {
  if (text.size() != 17)
    return false;
  for (size_t i = 0; i < ESP_BD_ADDR_LEN; i++) {
    if ((i > 0 && text[i * 3 - 1] != ':') || parse_hex(text.c_str() + i * 3, 2, bda + i, 1) != 2)
      return false;
  }
  return true;
}

BTHIDDevice *find_by_handle(uint8_t handle) {
  if (handle == 0xFF)
    return nullptr;
  for (auto *dev : host().devices) {
    if (dev->handle() == handle)
      return dev;
  }
  return nullptr;
}

void push(HostEvent &&event) {
  {
    LockGuard guard(host().lock);
    auto &events = host().events;
    // Reports are the only thing that can pile up; the oldest go first.
    if (events.size() >= MAX_QUEUED_EVENTS) {
      auto it =
          std::find_if(events.begin(), events.end(), [](const HostEvent &e) { return e.type == HostEvent::DATA; });
      if (it != events.end())
        events.erase(it);
    }
    events.push_back(std::move(event));
  }
  App.wake_loop_threadsafe();
}

// BTC task, through bt_classic. Pairing requests are answered only for this component's devices.
bool gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void * /*arg*/) {
  switch (event) {
    case ESP_BT_GAP_PIN_REQ_EVT: {
      // Legacy pairing: the PIN is sent here and typed on the keyboard.
      BTHIDDevice *dev = find_or_adopt(param->pin_req.bda);
      if (dev == nullptr)
        return false;
      esp_bt_pin_code_t code{};
      uint8_t len;
      if (dev->pin() == PIN_WIIMOTE || dev->pin() == PIN_WIIMOTE_SYNC) {
        // A Wii Remote takes an address backwards: its own when paired with 1+2, this
        // device's when paired with its sync button.
        const uint8_t *bda = dev->pin() == PIN_WIIMOTE ? param->pin_req.bda : esp_bt_dev_get_address();
        len = 6;
        for (int i = 0; i < 6 && bda != nullptr; i++)
          code[i] = bda[5 - i];
      } else {
        len = std::min<size_t>(dev->pin().size(), ESP_BT_PIN_CODE_LEN);
        memcpy(code, dev->pin().data(), len);
      }
      esp_bt_gap_pin_reply(param->pin_req.bda, true, len, code);
      HostEvent e;
      e.type = HostEvent::PIN;
      memcpy(e.bda, param->pin_req.bda, ESP_BD_ADDR_LEN);
      push(std::move(e));
      return true;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
      if (find_or_adopt(param->cfm_req.bda) == nullptr)
        return false;
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      return true;
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT: {
      if (param->read_rssi_delta.stat != ESP_BT_STATUS_SUCCESS || find_by_address(param->read_rssi_delta.bda) == nullptr)
        return false;
      HostEvent e;
      e.type = HostEvent::SIGNAL;
      memcpy(e.bda, param->read_rssi_delta.bda, ESP_BD_ADDR_LEN);
      e.rssi = param->read_rssi_delta.rssi_delta;
      e.has_rssi = true;
      push(std::move(e));
      return true;
    }
    case ESP_BT_GAP_KEY_NOTIF_EVT: {
      find_or_adopt(param->key_notif.bda);
      HostEvent e;
      e.type = HostEvent::PASSKEY;
      memcpy(e.bda, param->key_notif.bda, ESP_BD_ADDR_LEN);
      e.value = param->key_notif.passkey;
      push(std::move(e));
      break;
    }
    case ESP_BT_GAP_DISC_RES_EVT: {
      HostEvent e;
      e.type = HostEvent::FOUND;
      memcpy(e.bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t &prop = param->disc_res.prop[i];
        if (prop.type == ESP_BT_GAP_DEV_PROP_COD && prop.len >= (int) sizeof(uint32_t)) {
          e.value = *static_cast<const uint32_t *>(prop.val);
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_RSSI && prop.len >= 1) {
          e.rssi = *static_cast<const int8_t *>(prop.val);
          e.has_rssi = true;
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME && e.data.empty() && prop.len > 0) {
          const auto *name = static_cast<const uint8_t *>(prop.val);
          e.data.assign(name, name + strnlen(reinterpret_cast<const char *>(name), prop.len));
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && e.data.empty()) {
          uint8_t len = 0;
          auto *eir = static_cast<uint8_t *>(prop.val);
          uint8_t *name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
          if (name == nullptr)
            name = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
          if (name != nullptr)
            e.data.assign(name, name + len);
        }
      }
      push(std::move(e));
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
      HostEvent e;
      e.type = HostEvent::SCAN_STATE;
      e.value = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
      push(std::move(e));
      break;
    }
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      // A keyboard in pairing mode connects on its own, but Bluedroid refuses the pairing it
      // starts (btm_sec_pin_code_request: the host originates keyboard bonding). An open slot
      // takes it and connects to it itself.
      if (param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS)
        note_connected(param->acl_conn_cmpl_stat.bda);
      break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      HostEvent e;
      e.type = HostEvent::AUTH;
      memcpy(e.bda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
      e.status = param->auth_cmpl.stat;
      push(std::move(e));
      break;
    }
    default:
      break;
  }
  return false;
}

// BTC task. Descriptor and report buffers are freed once this returns, so they are copied.
void hh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
  HostEvent e;
  switch (event) {
    case ESP_HIDH_INIT_EVT:
      e.type = HostEvent::INIT;
      e.status = param->init.status;
      break;
    case ESP_HIDH_OPEN_EVT:
      e.type = HostEvent::OPEN;
      e.status = param->open.status;
      e.conn = param->open.conn_status;
      e.handle = param->open.handle;
      memcpy(e.bda, param->open.bd_addr, ESP_BD_ADDR_LEN);
      break;
    case ESP_HIDH_CLOSE_EVT:
      e.type = HostEvent::CLOSE;
      e.handle = param->close.handle;
      e.status = param->close.reason;
      break;
    case ESP_HIDH_VC_UNPLUG_EVT:
      e.type = HostEvent::CLOSE;
      e.handle = param->unplug.handle;
      break;
    case ESP_HIDH_GET_DSCP_EVT:
      if (param->dscp.status != ESP_HIDH_OK || param->dscp.dsc_list == nullptr || param->dscp.dl_len == 0)
        return;
      e.type = HostEvent::DESCRIPTOR;
      e.handle = param->dscp.handle;
      e.value = static_cast<uint32_t>(param->dscp.vendor_id) << 16 | param->dscp.product_id;
      e.data.assign(param->dscp.dsc_list, param->dscp.dsc_list + param->dscp.dl_len);
      break;
    case ESP_HIDH_DATA_IND_EVT:
      if (param->data_ind.status != ESP_HIDH_OK || param->data_ind.data == nullptr || param->data_ind.len == 0)
        return;
      e.type = HostEvent::DATA;
      e.handle = param->data_ind.handle;
      e.data.assign(param->data_ind.data, param->data_ind.data + param->data_ind.len);
      break;
    case ESP_HIDH_GET_RPT_EVT:
      // A failed GET_REPORT arrives without data.
      e.type = HostEvent::FEATURE;
      e.handle = param->get_rpt.handle;
      if (param->get_rpt.status == ESP_HIDH_OK && param->get_rpt.data != nullptr)
        e.data.assign(param->get_rpt.data, param->get_rpt.data + param->get_rpt.len);
      break;
    default:
      return;
  }
  push(std::move(e));
}

// Class of Device, peripheral major class: bits 7..6 keyboard/pointing, bits 5..2 the device type.
const char *peripheral_kind(uint32_t cod) {
  const uint32_t minor = esp_bt_gap_get_cod_minor_dev(cod);
  switch (minor & 0x0F) {
    case 1:
      return "joystick";
    case 2:
      return "gamepad";
    case 3:
      return "remote";
    default:
      break;
  }
  switch ((minor >> 4) & 0x03) {
    case 1:
      return "keyboard";
    case 2:
      return "mouse";
    case 3:
      return "keyboard/mouse";
    default:
      return "peripheral";
  }
}

void handle_event(HostEvent &e) {
  Host &h = host();
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  switch (e.type) {
    case HostEvent::INIT:
      if (e.status != ESP_HIDH_OK) {
        ESP_LOGE(TAG, "HID host init failed: %d", e.status);
        break;
      }
      h.ready = true;
      // Paired devices reconnect on their own when they wake up.
      esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
      ESP_LOGV(TAG, "HID host ready");
      break;
    case HostEvent::OPEN: {
      // esp_bt_hid_host_connect() answers at once with OK and CONNECTING, without a handle
      // (btc_hh_connect); the connection's own OPEN follows.
      if (e.status == ESP_HIDH_OK && e.conn == ESP_HIDH_CONN_STATE_CONNECTING)
        break;
      h.connecting = false;
      const bool opened = e.status == ESP_HIDH_OK && e.conn == ESP_HIDH_CONN_STATE_CONNECTED && e.handle != 0xFF;
      BTHIDDevice *dev = opened ? find_or_adopt(e.bda) : find_by_address(e.bda);
      if (dev != nullptr && opened && (!dev->get_auto_connect() || (dev->is_pooled() && in_list(h.refused, e.bda)))) {
        format_mac_addr_upper(e.bda, mac);
        ESP_LOGV(TAG, "[%s] Disconnected by request, refusing", mac);
        esp_bt_hid_host_disconnect(e.bda);
        if (dev->is_pooled())
          dev->release();
      } else if (dev != nullptr) {
        // Another connection under way in Bluedroid (btc_hh_connect): the page has not gone out.
        if (e.status == ESP_HIDH_BUSY)
          dev->set_page_pending(true);
        dev->on_open(e.handle, opened, e.status);
        // A failed attempt is retried until the window closes: the PIN may still be typed.
        if (dev == h.pairing && opened)
          dev->end_pairing(true, nullptr);
      } else if (opened) {
        format_mac_addr_upper(e.bda, mac);
        ESP_LOGW(TAG, in_list(h.paired, e.bda) ? "[%s] No free slot, disconnecting" : "[%s] Not configured, disconnecting",
                 mac);
        esp_bt_hid_host_disconnect(e.bda);
      }
      break;
    }
    case HostEvent::CLOSE:
      if (BTHIDDevice *dev = find_by_handle(e.handle))
        dev->on_close();
      break;
    case HostEvent::DESCRIPTOR:
      if (BTHIDDevice *dev = find_by_handle(e.handle)) {
        ESP_LOGV(TAG, "Device %04" PRIX32 ":%04" PRIX32 ", report descriptor %u bytes", e.value >> 16, e.value & 0xFFFF,
                 static_cast<unsigned>(e.data.size()));
        dev->on_descriptor(e.data, e.value >> 16, e.value & 0xFFFF);
      }
      break;
    case HostEvent::DATA:
      if (BTHIDDevice *dev = find_by_handle(e.handle))
        dev->on_report(e.data);
      break;
    case HostEvent::FEATURE:
      if (BTHIDDevice *dev = find_by_handle(e.handle))
        dev->on_feature(e.data);
      break;
    case HostEvent::PIN:
      if (BTHIDDevice *dev = find_by_address(e.bda))
        dev->on_pin_request();
      break;
    case HostEvent::PASSKEY:
      if (BTHIDDevice *dev = find_by_address(e.bda))
        dev->on_passkey(e.value);
      break;
    case HostEvent::ADOPTED:
      if (h.pairing != nullptr)
        h.pairing->on_adopted();
      break;
    case HostEvent::CLAIMED:
      if (BTHIDDevice *dev = find_by_address(e.bda))
        dev->on_claimed();
      break;
    case HostEvent::SIGNAL:
      if (BTHIDDevice *dev = find_by_address(e.bda))
        dev->set_signal(e.rssi, true);
      break;
    case HostEvent::LISTED:
      if (h.pairing != nullptr && h.pairing->has_device_select() && find_by_address(e.bda) == nullptr)
        h.pairing->on_found(e.bda, 0, "");
      break;
    case HostEvent::FOUND: {
      if (esp_bt_gap_get_cod_major_dev(e.value) != ESP_BT_COD_MAJOR_DEV_PERIPHERAL)
        break;
      format_mac_addr_upper(e.bda, mac);
      const std::string name(e.data.begin(), e.data.end());
      // Other inquiries (bt_pairing, a2dp_source) report their own finds.
      if (h.pairing != nullptr || h.scan_requested) {
        ESP_LOGV(TAG, "Found %s %s '%s', RSSI %d", peripheral_kind(e.value), mac, name.c_str(),
                 e.has_rssi ? e.rssi : 0);
      } else {
        ESP_LOGV(TAG, "Found %s %s '%s'", peripheral_kind(e.value), mac, name.c_str());
      }
      if (h.pairing != nullptr && find_by_address(e.bda) == nullptr) {
        if (h.pairing->has_device_select()) {
          h.pairing->on_found(e.bda, e.value, name);
        } else {
          h.pairing->on_pairing_candidate(e.bda, e.value, name);
        }
      }
      break;
    }
    case HostEvent::SCAN_STATE:
      h.discovering = e.value != 0;
      if (h.pairing != nullptr || h.scan_requested) {
        ESP_LOGV(TAG, "Scan %s", h.discovering ? "started" : "finished");
      } else {
        ESP_LOGV(TAG, "Scan %s", h.discovering ? "started" : "finished");
      }
      if (!h.discovering)
        h.scan_requested = false;
      // Without a device found the window stays open until it times out: a device in pairing
      // mode may still connect on its own.
      if (!h.discovering && h.pairing != nullptr && !h.pairing->has_address()) {
        ESP_LOGV(TAG, "No device in pairing mode found; waiting for one to connect");
      }
      break;
    case HostEvent::AUTH:
      format_mac_addr_upper(e.bda, mac);
      if (e.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGV(TAG, "[%s] Paired", mac);
      } else {
        // The pairing window stays open: a key kept from before fails first, then the device
        // pairs anew; a wrong PIN can be typed again.
        ESP_LOGW(TAG, "[%s] Authentication failed: %d", mac, e.status);
      }
      break;
  }
}

void host_loop() {
  Host &h = host();
  if (!bt_classic::ensure_gap_callback()) {
    if (h.started) {
      // The dongle went: the host goes down while Bluedroid can still take it, so that it can be
      // started again once Classic is back.
      if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
        esp_bt_hid_host_deinit();
      h.started = false;
      h.ready = false;
      h.connecting = false;
      h.queue_built = false;
      h.page_queue.clear();
      for (auto *dev : h.devices) {
        if (dev->is_connected())
          dev->on_close();
        dev->page_again();
      }
    }
    return;
  }
  if (!h.started) {
    h.started = true;
    esp_bt_hid_host_register_callback(hh_cb);
    if (esp_err_t err = esp_bt_hid_host_init(); err != ESP_OK)
      ESP_LOGE(TAG, "HID host init failed: %s", esp_err_to_name(err));
  }

  size_t pending;
  {
    LockGuard guard(h.lock);
    pending = h.events.size();
  }
  for (; pending > 0; pending--) {
    HostEvent e;
    {
      LockGuard guard(h.lock);
      if (h.events.empty())
        break;
      e = std::move(h.events.front());
      h.events.pop_front();
    }
    handle_event(e);
  }

  const uint32_t now = millis();
  if (h.pairing != nullptr && static_cast<int32_t>(now - h.pairing_until) >= 0)
    h.pairing->end_pairing(false, "timed out");
  for (auto *dev : h.devices)
    dev->restore_paired();

  // Shared slots: each paired device is paged once after Classic came up; a slot held for a
  // device that never connected is given up.
  BTHIDDevice *shared = nullptr;
  for (auto *dev : h.devices) {
    if (dev->is_pooled()) {
      shared = dev;
      break;
    }
  }
  if (shared != nullptr) {
#ifdef USE_BT_PAIRING
    if (h.ready && !h.queue_built && shared->paired_loaded()) {
      h.queue_built = true;
      std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> paired;
      shared->paired_addresses(paired);
      h.page_queue.assign(paired.begin(), paired.end());
    }
#endif
    for (auto *dev : h.devices) {
      if (dev->is_pooled() && dev->has_address() && !dev->is_connected() && !dev->page_pending() &&
          h.pairing != dev && !h.connecting && now - dev->held_since() > CONNECT_TIMEOUT_MS)
        dev->release();
    }
  }

  // One outgoing connection at a time, and none while a scan runs: paging during an inquiry
  // fails, so a device found by a pairing scan is connected once the scan has stopped.
  if (h.connecting && now - h.connecting_since > CONNECT_TIMEOUT_MS)
    h.connecting = false;
  if (!h.ready || h.connecting || h.discovering)
    return;
  while (!h.page_queue.empty()) {
    BTHIDDevice *free = nullptr;
    for (auto *dev : h.devices) {
      if (dev->is_pooled() && !dev->has_address() && h.pairing != dev) {
        free = dev;
        break;
      }
    }
    if (free == nullptr)
      break;
    const auto bda = h.page_queue.front();
    h.page_queue.pop_front();
    if (find_by_address(bda.data()) != nullptr || in_list(h.refused, bda.data()))
      continue;
    free->hold_for_page(bda.data());
    break;
  }
  for (auto *dev : h.devices) {
    if (dev->is_connected() || !dev->has_address() || !dev->get_auto_connect() ||
        (!dev->page_pending() && h.pairing != dev) || static_cast<int32_t>(now - dev->next_connect_ms()) < 0)
      continue;
    if (esp_bt_hid_host_connect(const_cast<uint8_t *>(dev->address())) == ESP_OK) {
      dev->set_page_pending(false);
      h.connecting = true;
      h.connecting_since = now;
    }
    dev->connect_attempted(now);
    break;
  }
}

}  // namespace

void BTHIDDevice::set_address(uint64_t address) {
  for (int i = 0; i < ESP_BD_ADDR_LEN; i++)
    this->bda_[i] = static_cast<uint8_t>(address >> (40 - 8 * i));
  this->fixed_address_ = true;
}

void BTHIDDevice::set_bda_(const uint8_t *bda) {
  LockGuard guard(host().lock);
  if (bda == nullptr) {
    memset(this->bda_, 0, sizeof(this->bda_));
  } else {
    memcpy(this->bda_, bda, sizeof(this->bda_));
  }
}

bool BTHIDDevice::has_address() const {
  for (uint8_t b : this->bda_) {
    if (b != 0)
      return true;
  }
  return false;
}

bool BTHIDDevice::is_pairing() const { return host().pairing == this; }

void BTHIDDevice::setup() {
  if (host().devices.empty())
    bt_classic::add_gap_handler(gap_cb, nullptr);
  host().devices.push_back(this);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    this->pairing_->add_target(this);
#ifdef USE_BT_PAIRING_HID
    // The first shared slot keeps the host's list of the shared slot's paired devices.
    if (this->pooled_ && std::none_of(host().devices.begin(), host().devices.end() - 1,
                                      [](const BTHIDDevice *dev) { return dev->is_pooled(); }))
      this->pairing_->add_on_devices_callback([this]() { this->refresh_paired_(); });
#endif
    this->pairing_->add_on_forget_callback([this](const bt_pairing::PairingRecord &record) {
      if (this->pooled_) {
        // Only the slot holding the device; it is given up once the device has gone.
        if (!this->has_address() || memcmp(record.address, this->bda_, ESP_BD_ADDR_LEN) != 0)
          return;
        if (this->connected_) {
          esp_bt_hid_host_virtual_cable_unplug(this->bda_);
        } else {
          this->release();
        }
        return;
      }
      if (record.slot != this->slot_ && memcmp(record.address, this->bda_, ESP_BD_ADDR_LEN) != 0)
        return;
      if (this->connected_)
        esp_bt_hid_host_virtual_cable_unplug(this->bda_);
      if (!this->fixed_address_)
        this->set_bda_(nullptr);
      if (this->address_set_ != nullptr)
        this->address_set_("");
    });
  }
#endif
}

// The slot's device: the address global when it holds one, else the pairing database.
bool BTHIDDevice::stored_address_(uint8_t *bda) const {
  if (this->address_get_ != nullptr && parse_mac(this->address_get_(), bda))
    return true;
#ifdef USE_BT_PAIRING
  // A shared slot holds no device of its own.
  if (this->pairing_ != nullptr && !this->pooled_) {
    if (const auto *record = this->pairing_->find_slot(this->slot_); record != nullptr) {
      memcpy(bda, record->address, ESP_BD_ADDR_LEN);
      return true;
    }
  }
#endif
  return false;
}

// A slot without a configured address takes the device it was paired with: from the
// address global, or once the pairing database is loaded, from there.
void BTHIDDevice::restore_paired() {
  if (this->restored_ || this->fixed_address_ || this->pooled_)
    return;
  uint8_t bda[ESP_BD_ADDR_LEN];
  const bool from_global = this->address_get_ != nullptr && parse_mac(this->address_get_(), bda);
#ifdef USE_BT_PAIRING
  if (!from_global && this->pairing_ != nullptr && !this->pairing_->is_loaded())
    return;
#endif
  this->restored_ = true;
  if ((from_global || this->stored_address_(bda)) && !this->has_address()) {
    this->set_bda_(bda);
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(bda, mac);
    ESP_LOGV(TAG, "[%s] Paired device", mac);
  }
}

void BTHIDDevice::loop() {
  if (host().devices.front() == this)
    host_loop();
}

void BTHIDDevice::dump_config() {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  if (this->fixed_address_) {
    format_mac_addr_upper(this->bda_, mac);
  } else {
    strcpy(mac, "set by pairing");
  }
  ESP_LOGCONFIG(TAG, "Bluetooth Classic HID device: %s, auto connect %s", mac, YESNO(this->auto_connect_));
}

void BTHIDDevice::connect() {
  this->auto_connect_ = true;
  this->page_pending_ = true;
  this->retry_ms_ = 0;
  this->next_connect_ms_ = millis();
}

void BTHIDDevice::disconnect() {
  this->auto_connect_ = false;
  if (this->connected_)
    esp_bt_hid_host_disconnect(this->bda_);
}

// Connected: a virtual cable unplug, which also deletes the bond (or a close that does, for a
// device without virtual cable support). Otherwise the bond is deleted directly. A device in
// the pairing database goes through it, which does the same and forgets the device.
void BTHIDDevice::unpair() {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    ESP_LOGW(TAG, "[%s] Bluetooth is not running, cannot unpair", mac);
    return;
  }
#ifdef USE_BT_PAIRING
  if (this->pooled_ && this->pairing_ != nullptr) {
    if (this->has_address() && this->pairing_->find_address(this->bda_) != nullptr) {
      this->pairing_->forget_address(this->bda_);
    } else {
      ESP_LOGW(TAG, "No device connected");
    }
    return;
  }
  if (this->pairing_ != nullptr && this->pairing_->find_slot(this->slot_) != nullptr) {
    this->pairing_->forget_slot(this->slot_);
    return;
  }
#endif
  if (!this->has_address()) {
    ESP_LOGW(TAG, "No device paired");
    return;
  }
  esp_err_t err =
      this->connected_ ? esp_bt_hid_host_virtual_cable_unplug(this->bda_) : esp_bt_gap_remove_bond_device(this->bda_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Unpair failed: %s", mac, esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "[%s] Unpaired", mac);
  }
  if (!this->fixed_address_) {
    this->set_bda_(nullptr);
    if (this->address_set_ != nullptr)
      this->address_set_("");
  }
}

void BTHIDDevice::pair(uint32_t duration_ms) {
  Host &h = host();
  if (this->fixed_address_) {
    ESP_LOGV(TAG, "Configured address: connecting, which pairs it if it is not paired yet");
    this->connect();
    return;
  }
  if (!h.ready) {
    ESP_LOGW(TAG, "Bluetooth Classic is not running, cannot pair");
    return;
  }
  if (h.pairing != nullptr) {
    ESP_LOGW(TAG, "A pairing is already running");
    return;
  }
  if (h.discovering) {
    ESP_LOGW(TAG, "A scan is running, pair once it has finished");
    return;
  }
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr && !this->pairing_->is_loaded()) {
    ESP_LOGW(TAG, "Pairing database not loaded yet");
    return;
  }
#endif
  const uint32_t units =
      std::clamp<uint32_t>((duration_ms + 1279) / 1280, ESP_BT_GAP_MIN_INQ_LEN, ESP_BT_GAP_MAX_INQ_LEN);
  if (esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0); err != ESP_OK) {
    ESP_LOGW(TAG, "Pairing scan failed to start: %s", esp_err_to_name(err));
    return;
  }
  this->found_.clear();
  this->publish_found_();
  this->open_window_(millis() + units * 1280 + CONNECT_TIMEOUT_MS);
  if (this->device_select_ != nullptr) {
    ESP_LOGV(TAG, "Pairing: listing the devices in pairing mode, pick one");
  } else {
    ESP_LOGV(TAG, "Pairing: looking for a device in pairing mode");
  }
}

// The device paired so far stays stored until another one has paired.
void BTHIDDevice::open_window_(uint32_t until) {
  Host &h = host();
  if (this->connected_)
    esp_bt_hid_host_disconnect(this->bda_);
  this->set_bda_(nullptr);
  this->restored_ = true;
  this->auto_connect_ = true;
  this->candidate_name_.clear();
  this->candidate_class_ = 0;
  {
    LockGuard guard(h.lock);
    h.pairing = this;
  }
  h.pairing_until = until;
}

void BTHIDDevice::publish_found_() {
#ifdef USE_BT_HID_SELECT
  if (this->device_select_ != nullptr)
    this->device_select_->set_devices(this->found_);
#endif
}

void BTHIDDevice::on_found(const uint8_t *bda, uint32_t device_class, const std::string &name) {
  for (auto &found : this->found_) {
    if (memcmp(found.address, bda, ESP_BD_ADDR_LEN) != 0)
      continue;
    if (found.name.empty() && !name.empty()) {
      found.name = name;
      found.device_class = device_class;
      this->publish_found_();
    }
    return;
  }
  if (this->found_.size() >= MAX_FOUND)
    return;
  FoundDevice found;
  memcpy(found.address, bda, ESP_BD_ADDR_LEN);
  found.device_class = device_class;
  found.name = name;
  this->found_.push_back(std::move(found));
  this->publish_found_();
}

void BTHIDDevice::pair_with(size_t index) {
  if (index < this->found_.size())
    this->pair_found_(this->found_[index]);
}

#ifdef USE_BT_PAIRING
bool BTHIDDevice::pair_device(const bt_pairing::ScannedDevice &device) {
  FoundDevice found;
  memcpy(found.address, device.address, ESP_BD_ADDR_LEN);
  found.device_class = device.device_class;
  found.name = device.name;
  return this->pair_found_(found);
}
#endif

bool BTHIDDevice::pair_found_(const FoundDevice &picked) {
  Host &h = host();
  if (!h.ready) {
    ESP_LOGW(TAG, "Bluetooth Classic is not running, cannot pair");
    return false;
  }
  if (h.pairing != nullptr && h.pairing != this) {
    ESP_LOGW(TAG, "Another pairing is running");
    return false;
  }
  const FoundDevice found = picked;
  // The window closed since the scan, or another device was picked in it: start over with this one.
  if (h.pairing == nullptr || this->has_address())
    this->open_window_(millis() + 2 * CONNECT_TIMEOUT_MS);
  if (this->on_pairing_candidate(found.address, found.device_class, found.name))
    return true;
  this->end_pairing(false, "the device belongs to another slot");
  return false;
}

bool BTHIDDevice::on_pairing_candidate(const uint8_t *bda, uint32_t device_class, const std::string &name) {
  if (this->has_address())
    return false;
#ifdef USE_BT_PAIRING
  // Another slot's device; this slot's own may pair again, and so may one left from an old config.
  if (this->pairing_ != nullptr) {
    const auto *record = this->pairing_->find_address(bda);
    if (record != nullptr && record->slot != this->slot_ && this->pairing_->slot_in_use(record->slot))
      return false;
  }
#endif
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(bda, mac);
  ESP_LOGV(TAG, "[%s] Pairing with '%s'", mac, name.c_str());
  // A device in pairing mode has dropped any key kept here from before; pair it anew.
  if (is_bonded(bda))
    esp_bt_gap_remove_bond_device(const_cast<uint8_t *>(bda));
  this->set_bda_(bda);
  this->candidate_class_ = device_class;
  this->candidate_name_ = name;
  this->retry_ms_ = 0;
  this->next_connect_ms_ = millis();
  // Time to connect and type the PIN, whatever was left of the scan.
  host().pairing_until = millis() + 2 * CONNECT_TIMEOUT_MS;
  esp_bt_gap_cancel_discovery();
  return true;
}

void BTHIDDevice::adopt_address(const uint8_t *bda) {
  memcpy(this->bda_, bda, sizeof(this->bda_));
  this->held_since_ = millis();
}

void BTHIDDevice::on_claimed() {
  this->auto_connect_ = true;
  this->page_pending_ = false;
  this->retry_ms_ = 0;
}

void BTHIDDevice::hold_for_page(const uint8_t *bda) {
  this->set_bda_(bda);
  this->held_since_ = millis();
  this->connect();
}

void BTHIDDevice::release() {
  this->set_bda_(nullptr);
  this->auto_connect_ = true;
  this->page_pending_ = false;
  this->retry_ms_ = 0;
}

void BTHIDDevice::paired_addresses(std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> &out) const {
#ifdef USE_BT_PAIRING
  if (this->pairing_ == nullptr)
    return;
  for (const auto &record : this->pairing_->records()) {
    if (record.transport != static_cast<uint8_t>(bt_pairing::Transport::CLASSIC) || record.slot != this->slot_)
      continue;
    std::array<uint8_t, ESP_BD_ADDR_LEN> bda;
    memcpy(bda.data(), record.address, ESP_BD_ADDR_LEN);
    out.push_back(bda);
  }
#endif
}

void BTHIDDevice::refresh_paired_() {
  std::vector<std::array<uint8_t, ESP_BD_ADDR_LEN>> paired;
  this->paired_addresses(paired);
  LockGuard guard(host().lock);
  host().paired.swap(paired);
}

#ifdef USE_BT_PAIRING
void BTHIDDevice::connect_address(const uint8_t *address) {
  if (!this->pooled_) {
    this->connect();
    return;
  }
  auto &refused = host().refused;
  refused.erase(std::remove_if(refused.begin(), refused.end(),
                               [address](const std::array<uint8_t, ESP_BD_ADDR_LEN> &entry) {
                                 return memcmp(entry.data(), address, ESP_BD_ADDR_LEN) == 0;
                               }),
                refused.end());
  if (this->has_address() && memcmp(this->bda_, address, ESP_BD_ADDR_LEN) == 0) {
    this->connect();
    return;
  }
  if (this->has_address() || this->is_pairing()) {
    ESP_LOGW(TAG, "All Bluetooth Classic HID slots in use");
    return;
  }
  this->hold_for_page(address);
}

void BTHIDDevice::disconnect_address(const uint8_t *address) {
  if (!this->pooled_) {
    this->disconnect();
    return;
  }
  if (!in_list(host().refused, address)) {
    std::array<uint8_t, ESP_BD_ADDR_LEN> bda;
    memcpy(bda.data(), address, ESP_BD_ADDR_LEN);
    host().refused.push_back(bda);
  }
  if (this->has_address() && memcmp(this->bda_, address, ESP_BD_ADDR_LEN) == 0)
    this->disconnect();
}
#endif

void BTHIDDevice::on_adopted() {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  ESP_LOGV(TAG, "[%s] Pairing with the device that connected", mac);
  this->candidate_class_ = 0;
  this->candidate_name_.clear();
  this->retry_ms_ = 0;
  this->next_connect_ms_ = millis();
  host().pairing_until = millis() + 2 * CONNECT_TIMEOUT_MS;
  if (host().discovering)
    esp_bt_gap_cancel_discovery();
}

void BTHIDDevice::cancel_pairing() {
  if (host().pairing == this)
    this->end_pairing(false, "cancelled");
}

void BTHIDDevice::end_pairing(bool success, const char *reason) {
  Host &h = host();
  if (h.pairing != this)
    return;
  {
    LockGuard guard(h.lock);
    h.pairing = nullptr;
  }
  if (h.discovering)
    esp_bt_gap_cancel_discovery();
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  uint8_t previous[ESP_BD_ADDR_LEN];
  const bool has_previous = this->stored_address_(previous);
  const bool same_device = has_previous && memcmp(previous, this->bda_, ESP_BD_ADDR_LEN) == 0;
  if (!success) {
    ESP_LOGW(TAG, "Pairing failed: %s", reason);
    if (this->connected_)
      esp_bt_hid_host_disconnect(this->bda_);
    // A bond the failed attempt may have left goes; the device paired before comes back.
    if (this->has_address() && !same_device)
      esp_bt_gap_remove_bond_device(this->bda_);
    this->set_bda_(has_previous ? previous : nullptr);
    // The window took it off: it is paged once again.
    this->page_pending_ = has_previous;
    this->retry_ms_ = 0;
    this->next_connect_ms_ = millis();
    return;
  }
  ESP_LOGI(TAG, "[%s] Paired with '%s'", mac, this->candidate_name_.c_str());
  // The device it replaces loses its bond.
  if (has_previous && !same_device)
    esp_bt_gap_remove_bond_device(previous);
  if (this->address_set_ != nullptr)
    this->address_set_(mac);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    bt_pairing::PairingRecord record{};
    memcpy(record.address, this->bda_, sizeof(record.address));
    record.transport = static_cast<uint8_t>(bt_pairing::Transport::CLASSIC);
    record.slot = this->slot_;
    record.device_class = this->candidate_class_;
    strncpy(record.name, this->candidate_name_.c_str(), sizeof(record.name) - 1);
    this->pairing_->store(record);
  }
#endif
}

void BTHIDDevice::scan(uint32_t duration_ms) {
  if (!host().ready) {
    ESP_LOGW(TAG, "Bluetooth Classic is not running, cannot scan");
    return;
  }
  // Inquiry length is in units of 1.28 s.
  const uint32_t units =
      std::clamp<uint32_t>((duration_ms + 1279) / 1280, ESP_BT_GAP_MIN_INQ_LEN, ESP_BT_GAP_MAX_INQ_LEN);
  if (esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0); err != ESP_OK) {
    ESP_LOGW(TAG, "Scan failed to start: %s", esp_err_to_name(err));
    return;
  }
  host().scan_requested = true;
}

void BTHIDDevice::page_again() {
  this->page_pending_ = true;
  this->retry_ms_ = 0;
  this->next_connect_ms_ = millis();
}

void BTHIDDevice::connect_attempted(uint32_t now) {
  this->retry_ms_ = this->retry_ms_ == 0 ? RETRY_MIN_MS : std::min(this->retry_ms_ * 2, RETRY_MAX_MS);
  this->next_connect_ms_ = now + this->retry_ms_;
}

void BTHIDDevice::on_open(uint8_t handle, bool success, int status) {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  if (!success) {
    ESP_LOGV(TAG, "[%s] Connection failed: %d", mac, status);
    // A shared slot is given up, unless the page has not gone out yet or a pairing retries.
    if (this->pooled_ && status != ESP_HIDH_BUSY && host().pairing != this)
      this->release();
    return;
  }
  this->handle_ = handle;
  this->connected_ = true;
  this->retry_ms_ = 0;
  ESP_LOGI(TAG, "[%s] Connected", mac);
  this->connect_cb_.call();
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr)
    this->pairing_->on_connection(this, true);
#endif
}

void BTHIDDevice::on_close() {
  const bool was_connected = this->connected_;
  if (was_connected) {
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(this->bda_, mac);
    ESP_LOGI(TAG, "[%s] Disconnected", mac);
  }
  this->connected_ = false;
  this->handle_ = 0xFF;
  this->map_.reset();
  this->gamepad_detach_();
  this->release_all_hid();
  if (was_connected) {
    this->disconnect_cb_.call();
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr)
      this->pairing_->on_connection(this, false);
#endif
  }
  if (this->pooled_ && host().pairing != this)
    this->release();
}

void BTHIDDevice::on_descriptor(const std::vector<uint8_t> &descriptor, uint16_t vid, uint16_t pid) {
  this->map_.reset(hid::HIDReportMap::parse_report_map_data(descriptor.data(), descriptor.size()));
  this->gamepad_attach_(hid::HIDBus::BT_CLASSIC, vid, pid, 0);
  this->gamepad_start_();
  if (this->map_ == nullptr) {
    ESP_LOGW(TAG, "Report descriptor (%u bytes) not understood", static_cast<unsigned>(descriptor.size()));
    return;
  }
  this->write_leds_(this->leds_);
}

void BTHIDDevice::on_feature(const std::vector<uint8_t> &report) {
  const uint8_t id = this->feature_id_;
  this->feature_id_ = 0;
  this->gamepad_feature_(id, report.data(), report.size());
}

// Output reports on the interrupt channel, report ID first where the device numbers them.
bool BTHIDDevice::send_output_report(const uint8_t *data, size_t len) {
  if (!this->connected_ || len == 0)
    return false;
  return esp_bt_hid_host_send_data(this->bda_, const_cast<uint8_t *>(data), len) == ESP_OK;
}

bool BTHIDDevice::set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) {
  if (!this->connected_ || len == 0)
    return false;
  const auto type = kind == hid::HIDReportKind::FEATURE  ? ESP_HIDH_REPORT_TYPE_FEATURE
                    : kind == hid::HIDReportKind::OUTPUT ? ESP_HIDH_REPORT_TYPE_OUTPUT
                                                         : ESP_HIDH_REPORT_TYPE_INPUT;
  return esp_bt_hid_host_set_report(this->bda_, type, const_cast<uint8_t *>(data), len) == ESP_OK;
}

// One at a time: the answer does not say which report it is.
bool BTHIDDevice::request_feature_report(uint8_t id, uint16_t len) {
  if (!this->connected_ || this->feature_id_ != 0)
    return false;
  if (esp_bt_hid_host_get_report(this->bda_, ESP_HIDH_REPORT_TYPE_FEATURE, id, len) != ESP_OK)
    return false;
  this->feature_id_ = id;
  return true;
}

void BTHIDDevice::on_report(const std::vector<uint8_t> &report) {
  if (report.empty() || this->gamepad_report_(report.data(), report.size()))
    return;
  if (this->map_ == nullptr)
    return;
  const uint8_t *data = report.data();
  size_t length = report.size();
  hid::HIDReportSource source;
  source.characteristic_handle = this->handle_;
  if (this->map_->uses_report_ids()) {
    source.has_report_id = true;
    source.report_id = data[0];
    data++;
    length--;
  }
  this->process_hid_values(this->map_->parse(source, data, length));
}

void BTHIDDevice::on_pin_request() {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  if (this->pin_ == PIN_WIIMOTE || this->pin_ == PIN_WIIMOTE_SYNC) {
    ESP_LOGI(TAG, "[%s] PIN requested: Wii Remote address sent", mac);
  } else {
    ESP_LOGI(TAG, "[%s] PIN requested: type %s and Enter on the device", mac, this->pin_.c_str());
  }
  this->pin_cb_.call(this->pin_);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr)
    this->pairing_->on_pin_request(this, this->pin_);
#endif
}

bool BTHIDDevice::request_signal() {
  return this->connected_ && esp_bt_gap_read_rssi_delta(this->bda_) == ESP_OK;
}

void BTHIDDevice::on_passkey(uint32_t passkey) {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  ESP_LOGI(TAG, "[%s] Passkey: type %06" PRIu32 " and Enter on the device", mac, passkey);
  this->passkey_cb_.call(passkey);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr)
    this->pairing_->on_passkey(this, passkey);
#endif
}

// Output reports go on the interrupt channel, report ID first where the device uses them.
void BTHIDDevice::write_leds_(uint8_t leds) {
  if (!this->connected_ || this->map_ == nullptr)
    return;
  uint8_t report_id = 0;
  std::vector<uint8_t> payload;
  if (!hid::build_led_report(*this->map_, leds, report_id, payload))
    return;
  if (this->map_->uses_report_ids())
    payload.insert(payload.begin(), report_id);
  esp_bt_hid_host_send_data(this->bda_, payload.data(), payload.size());
}

}  // namespace esphome::bt_hid

#endif  // USE_ESP32
