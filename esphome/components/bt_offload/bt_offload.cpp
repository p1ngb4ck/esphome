#include "bt_offload.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/log.h"
#include "esp_hosted_misc.h"

#include <cstdio>
#include <cstring>

namespace esphome::bt_offload {

static const char *const TAG = "bt_offload";

static constexpr uint32_t MSG_HID_OPEN = 0x42545010u;
static constexpr uint32_t MSG_HID_INPUT = 0x42545011u;
static constexpr uint32_t MSG_HID_CLOSE = 0x42545012u;
static constexpr uint32_t MSG_SCAN = 0x42545020u;
static constexpr uint32_t MSG_FOUND = 0x42545021u;
static constexpr uint32_t MSG_CONNECT = 0x42545022u;
static constexpr size_t FOUND_MAX = 16;

static BTOffload *g_offload = nullptr;

static void forward(uint32_t msg_id, const uint8_t *data, size_t len, void *) {
  BTOffload *self = g_offload;
  if (self == nullptr || data == nullptr || len == 0)
    return;
  self->defer([self, msg_id, copy = std::vector<uint8_t>(data, data + len)]() {
    switch (msg_id) {
      case MSG_HID_OPEN:
        self->on_open_(copy.data(), copy.size());
        break;
      case MSG_HID_INPUT:
        self->on_input_(copy.data(), copy.size());
        break;
      case MSG_HID_CLOSE:
        self->on_close_(copy.data(), copy.size());
        break;
      case MSG_FOUND:
        self->on_found_(copy.data(), copy.size());
        break;
      default:
        break;
    }
  });
}

static std::string format_address(const uint8_t *a) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

void BTOffload::setup() {
  g_offload = this;
  for (uint32_t id : {MSG_HID_OPEN, MSG_HID_INPUT, MSG_HID_CLOSE, MSG_FOUND}) {
    const esp_err_t err = esp_hosted_register_custom_callback(id, forward, nullptr);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Handler 0x%08X not registered: %s", static_cast<unsigned>(id), esp_err_to_name(err));
      this->mark_failed();
      return;
    }
  }
}

void BTOffload::dump_config() { ESP_LOGCONFIG(TAG, "Bluetooth on the C6 (USB dongle through esp32_hosted)"); }

BTOffload::Device *BTOffload::device_(const uint8_t *address) {
  for (auto &dev : this->devices_) {
    if (std::memcmp(dev.address.data(), address, 6) == 0)
      return &dev;
  }
  return nullptr;
}

void BTOffload::scan(uint8_t seconds) {
  this->found_.clear();
  esp_err_t err = esp_hosted_send_custom_data(MSG_SCAN, &seconds, 1);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "Scan request not sent: %s", esp_err_to_name(err));
}

bool BTOffload::connect(const std::string &address) {
  for (const auto &found : this->found_) {
    if (format_address(found.address.data()) != address)
      continue;
    uint8_t msg[8];
    std::memcpy(msg, found.address.data(), 6);
    msg[6] = found.transport;
    msg[7] = found.addr_type;
    return esp_hosted_send_custom_data(MSG_CONNECT, msg, sizeof(msg)) == ESP_OK;
  }
  ESP_LOGW(TAG, "%s was not found by the last scan", address.c_str());
  return false;
}

void BTOffload::on_found_(const uint8_t *data, size_t len) {
  if (len < 9 || len < 9u + data[8])
    return;
  for (const auto &found : this->found_) {
    if (std::memcmp(found.address.data(), data, 6) == 0)
      return;
  }
  if (this->found_.size() >= FOUND_MAX)
    return;
  Found found;
  std::memcpy(found.address.data(), data, 6);
  found.transport = data[6];
  found.addr_type = data[7];
  found.name.assign(reinterpret_cast<const char *>(data + 9), data[8]);
  ESP_LOGI(TAG, "Found %s %s (%s)", format_address(data).c_str(), found.name.c_str(),
           found.transport == 1 ? "LE" : "BR/EDR");
  this->found_.push_back(std::move(found));
}

void BTOffload::on_open_(const uint8_t *data, size_t len) {
  if (len < 7)
    return;
  const size_t name_len = data[6];
  if (len < 7 + name_len + 2)
    return;
  const size_t map_len = data[7 + name_len] | (data[8 + name_len] << 8);
  const uint8_t *map = data + 9 + name_len;
  if (len < 9 + name_len + map_len)
    return;
  Device *dev = this->device_(data);
  if (dev == nullptr) {
    this->devices_.push_back({});
    dev = &this->devices_.back();
    std::memcpy(dev->address.data(), data, 6);
  }
  dev->name.assign(reinterpret_cast<const char *>(data + 7), name_len);
  dev->map.reset(map_len > 0 ? hid::HIDReportMap::parse_report_map_data(map, map_len) : nullptr);
  ESP_LOGI(TAG, "%s %s connected (report map %u bytes)", format_address(data).c_str(), dev->name.c_str(),
           static_cast<unsigned>(map_len));
}

void BTOffload::on_input_(const uint8_t *data, size_t len) {
  if (len < 8)
    return;
  Device *dev = this->device_(data);
  if (dev == nullptr || dev->map == nullptr)
    return;
  hid::HIDReportSource source;
  if (dev->map->uses_report_ids()) {
    source.has_report_id = true;
    source.report_id = data[6];
  }
  this->process_hid_values(dev->map->parse(source, data + 8, len - 8));
}

void BTOffload::on_close_(const uint8_t *data, size_t len) {
  if (len < 6)
    return;
  for (auto it = this->devices_.begin(); it != this->devices_.end(); ++it) {
    if (std::memcmp(it->address.data(), data, 6) == 0) {
      ESP_LOGI(TAG, "%s %s disconnected", format_address(data).c_str(), it->name.c_str());
      this->devices_.erase(it);
      break;
    }
  }
  this->release_all_hid();
}

}

#endif
