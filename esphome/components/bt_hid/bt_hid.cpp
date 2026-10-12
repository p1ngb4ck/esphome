#include "bt_hid.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_hidh_api.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <deque>

namespace esphome::bt_hid {

static const char *const TAG = "bt_hid";

static constexpr uint32_t RETRY_MIN_MS = 5000;
static constexpr uint32_t RETRY_MAX_MS = 60000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr size_t MAX_QUEUED_EVENTS = 64;

namespace {

// Bluedroid calls back on its BTC task; everything is handed to the main loop as one of these.
struct HostEvent {
  enum Type : uint8_t { INIT, OPEN, CLOSE, DESCRIPTOR, DATA, PIN, PASSKEY, AUTH };
  Type type{INIT};
  uint8_t handle{0xFF};
  esp_bd_addr_t bda{};
  int status{0};
  uint32_t value{0};
  std::vector<uint8_t> data;
};

// The HID host and Classic GAP callbacks are global to Bluedroid, so one host serves every
// configured device. The device list is complete before Bluedroid comes up and never changes
// after, so the BTC task reads it without the lock.
struct Host {
  Mutex lock;
  std::deque<HostEvent> events;
  std::vector<BTHIDDevice *> devices;
  bool started{false};
  bool ready{false};
  bool connecting{false};
  uint32_t connecting_since{0};
};

Host &host() {
  static Host instance;
  return instance;
}

BTHIDDevice *find_by_address(const uint8_t *bda) {
  for (auto *dev : host().devices) {
    if (memcmp(dev->address(), bda, ESP_BD_ADDR_LEN) == 0)
      return dev;
  }
  return nullptr;
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

// BTC task.
void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_PIN_REQ_EVT: {
      // Legacy pairing: the PIN is sent here and typed on the keyboard. Only configured devices.
      BTHIDDevice *dev = find_by_address(param->pin_req.bda);
      esp_bt_pin_code_t code{};
      uint8_t len = 0;
      if (dev != nullptr) {
        len = std::min<size_t>(dev->pin().size(), ESP_BT_PIN_CODE_LEN);
        memcpy(code, dev->pin().data(), len);
      }
      esp_bt_gap_pin_reply(param->pin_req.bda, dev != nullptr, len, code);
      HostEvent e;
      e.type = HostEvent::PIN;
      memcpy(e.bda, param->pin_req.bda, ESP_BD_ADDR_LEN);
      push(std::move(e));
      break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, find_by_address(param->cfm_req.bda) != nullptr);
      break;
    case ESP_BT_GAP_KEY_NOTIF_EVT: {
      HostEvent e;
      e.type = HostEvent::PASSKEY;
      memcpy(e.bda, param->key_notif.bda, ESP_BD_ADDR_LEN);
      e.value = param->key_notif.passkey;
      push(std::move(e));
      break;
    }
    case ESP_BT_GAP_KEY_REQ_EVT:
      // Nothing here to type a passkey on.
      esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
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
    default:
      return;
  }
  push(std::move(e));
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
      ESP_LOGI(TAG, "HID host ready");
      break;
    case HostEvent::OPEN: {
      h.connecting = false;
      BTHIDDevice *dev = find_by_address(e.bda);
      if (dev != nullptr) {
        dev->on_open(e.handle, e.status == ESP_HIDH_OK, e.status);
      } else if (e.status == ESP_HIDH_OK) {
        format_mac_addr_upper(e.bda, mac);
        ESP_LOGW(TAG, "[%s] Not configured, disconnecting", mac);
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
        ESP_LOGD(TAG, "Device %04" PRIX32 ":%04" PRIX32 ", report descriptor %u bytes", e.value >> 16, e.value & 0xFFFF,
                 static_cast<unsigned>(e.data.size()));
        dev->on_descriptor(e.data);
      }
      break;
    case HostEvent::DATA:
      if (BTHIDDevice *dev = find_by_handle(e.handle))
        dev->on_report(e.data);
      break;
    case HostEvent::PIN:
      format_mac_addr_upper(e.bda, mac);
      if (BTHIDDevice *dev = find_by_address(e.bda)) {
        ESP_LOGI(TAG, "[%s] PIN requested: type %s and Enter on the device", mac, dev->pin().c_str());
      } else {
        ESP_LOGW(TAG, "[%s] Not configured, pairing refused", mac);
      }
      break;
    case HostEvent::PASSKEY:
      if (BTHIDDevice *dev = find_by_address(e.bda))
        dev->on_passkey(e.value);
      break;
    case HostEvent::AUTH:
      format_mac_addr_upper(e.bda, mac);
      if (e.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "[%s] Paired", mac);
      } else {
        ESP_LOGW(TAG, "[%s] Pairing failed: %d", mac, e.status);
      }
      break;
  }
}

void host_loop() {
  Host &h = host();
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    if (h.started) {
      h.started = false;
      h.ready = false;
      h.connecting = false;
      for (auto *dev : h.devices) {
        if (dev->is_connected())
          dev->on_close();
      }
    }
    return;
  }
  if (!h.started) {
    h.started = true;
    esp_bt_gap_register_callback(gap_cb);
    // DisplayOnly: a keyboard gets a passkey to type, everything else pairs with Just Works.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_OUT;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    // Variable: legacy pairing asks through ESP_BT_GAP_PIN_REQ_EVT, per device.
    esp_bt_pin_code_t unused{};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused);
    esp_bt_hid_host_register_callback(hh_cb);
    if (esp_err_t err = esp_bt_hid_host_init(); err != ESP_OK)
      ESP_LOGE(TAG, "HID host init failed: %s", esp_err_to_name(err));
  }

  std::deque<HostEvent> events;
  {
    LockGuard guard(h.lock);
    events.swap(h.events);
  }
  for (auto &e : events)
    handle_event(e);

  // One outgoing connection at a time.
  const uint32_t now = millis();
  if (h.connecting && now - h.connecting_since > CONNECT_TIMEOUT_MS)
    h.connecting = false;
  if (!h.ready || h.connecting)
    return;
  for (auto *dev : h.devices) {
    if (dev->is_connected() || static_cast<int32_t>(now - dev->next_connect_ms()) < 0)
      continue;
    if (esp_bt_hid_host_connect(const_cast<uint8_t *>(dev->address())) == ESP_OK) {
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
}

void BTHIDDevice::setup() { host().devices.push_back(this); }

void BTHIDDevice::loop() {
  if (host().devices.front() == this)
    host_loop();
}

void BTHIDDevice::dump_config() {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  ESP_LOGCONFIG(TAG,
                "Bluetooth Classic HID device:\n"
                "  Address: %s\n"
                "  Legacy PIN: %s",
                mac, this->pin_.c_str());
}

void BTHIDDevice::connect_attempted(uint32_t now) {
  this->retry_ms_ = this->retry_ms_ == 0 ? RETRY_MIN_MS : std::min(this->retry_ms_ * 2, RETRY_MAX_MS);
  this->next_connect_ms_ = now + this->retry_ms_;
}

void BTHIDDevice::on_open(uint8_t handle, bool success, int status) {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  if (!success) {
    ESP_LOGD(TAG, "[%s] Connection failed: %d", mac, status);
    return;
  }
  this->handle_ = handle;
  this->connected_ = true;
  this->retry_ms_ = 0;
  ESP_LOGI(TAG, "[%s] Connected", mac);
}

void BTHIDDevice::on_close() {
  if (this->connected_) {
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    format_mac_addr_upper(this->bda_, mac);
    ESP_LOGI(TAG, "[%s] Disconnected", mac);
  }
  this->connected_ = false;
  this->handle_ = 0xFF;
  this->map_.reset();
  this->release_all_hid();
}

void BTHIDDevice::on_descriptor(const std::vector<uint8_t> &descriptor) {
  this->map_.reset(hid::HIDReportMap::parse_report_map_data(descriptor.data(), descriptor.size()));
  if (this->map_ == nullptr) {
    ESP_LOGW(TAG, "Report descriptor (%u bytes) not understood", static_cast<unsigned>(descriptor.size()));
    return;
  }
  this->write_leds_(this->leds_);
}

void BTHIDDevice::on_report(const std::vector<uint8_t> &report) {
  if (this->map_ == nullptr || report.empty())
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

void BTHIDDevice::on_passkey(uint32_t passkey) {
  char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  format_mac_addr_upper(this->bda_, mac);
  ESP_LOGI(TAG, "[%s] Passkey: type %06" PRIu32 " and Enter on the device", mac, passkey);
  this->passkey_cb_.call(passkey);
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
