#include "bt_offload.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/log.h"
#ifdef USE_BT_OFFLOAD_SPEAKER
#include "speaker/bt_offload_speaker.h"
#endif
#include "esp_hosted_misc.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>

namespace esphome::bt_offload {

static const char *const TAG = "bt_offload";

static constexpr uint32_t MSG_HID_OPEN = 0x42545010u;
static constexpr uint32_t MSG_HID_INPUT = 0x42545011u;
static constexpr uint32_t MSG_HID_CLOSE = 0x42545012u;
static constexpr uint32_t MSG_FOUND = 0x42545021u;
static constexpr uint32_t MSG_AUDIO_STATE = 0x42545033u;
static constexpr uint32_t MSG_AUDIO_CREDIT = 0x42545034u;
static constexpr uint32_t MSG_COMMAND = 0x42545040u;
static constexpr uint8_t TYPE_SCAN = 0x20;
static constexpr uint8_t TYPE_CONNECT = 0x22;
static constexpr uint8_t TYPE_AUDIO_CONNECT = 0x30;
static constexpr uint8_t TYPE_AUDIO_DISCONNECT = 0x31;
static constexpr uint8_t TYPE_AUDIO_VOLUME = 0x35;
static constexpr size_t FOUND_MAX = 16;

static BTOffload *g_offload = nullptr;

void OffloadEvent::release() {
  if (this->large != nullptr) {
    heap_caps_free(this->large);
    this->large = nullptr;
  }
  this->len = 0;
}

static void forward(uint32_t msg_id, const uint8_t *data, size_t len, void *) {
  BTOffload *self = g_offload;
  if (self != nullptr && data != nullptr && len > 0 && len <= UINT16_MAX)
    self->receive_(msg_id, data, len);
}

void BTOffload::receive_(uint32_t msg_id, const uint8_t *data, size_t len) {
  OffloadEvent *event = this->pool_.allocate();
  if (event == nullptr)
    return;
  event->msg_id = 0;
  event->len = 0;
  if (len > OffloadEvent::INLINE_MAX)
    event->large = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (len <= OffloadEvent::INLINE_MAX || event->large != nullptr) {
    event->msg_id = msg_id;
    event->len = static_cast<uint16_t>(len);
    std::memcpy(event->large != nullptr ? event->large : event->inline_data, data, len);
  }
  this->queue_.push(event);
  this->enable_loop_soon_any_context();
}

void BTOffload::loop() {
  OffloadEvent *event;
  while ((event = this->queue_.pop()) != nullptr) {
    switch (event->msg_id) {
      case MSG_HID_OPEN:
        this->on_open_(event->data(), event->len);
        break;
      case MSG_HID_INPUT:
        this->on_input_(event->data(), event->len);
        break;
      case MSG_HID_CLOSE:
        this->on_close_(event->data(), event->len);
        break;
      case MSG_FOUND:
        this->on_found_(event->data(), event->len);
        break;
      case MSG_AUDIO_STATE:
        this->on_audio_state_(event->data(), event->len);
        break;
      case MSG_AUDIO_CREDIT:
        this->on_audio_credit_(event->data(), event->len);
        break;
      default:
        break;
    }
    this->pool_.release(event);
  }
  this->disable_loop();
}

static std::string format_address(const uint8_t *a) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

void BTOffload::setup() {
  this->disable_loop();
  g_offload = this;
  for (uint32_t id : {MSG_HID_OPEN, MSG_HID_INPUT, MSG_HID_CLOSE, MSG_FOUND, MSG_AUDIO_STATE, MSG_AUDIO_CREDIT}) {
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

bool BTOffload::command_(uint8_t type, const uint8_t *data, size_t len) {
  uint8_t msg[16];
  if (len + 1 > sizeof(msg))
    return false;
  msg[0] = type;
  if (len > 0)
    std::memcpy(msg + 1, data, len);
  const esp_err_t err = esp_hosted_send_custom_data(MSG_COMMAND, msg, len + 1);
  if (err != ESP_OK)
    ESP_LOGW(TAG, "Command 0x%02X not sent: %s", type, esp_err_to_name(err));
  return err == ESP_OK;
}

void BTOffload::scan(uint8_t seconds) {
  this->found_.clear();
  this->command_(TYPE_SCAN, &seconds, 1);
}

bool BTOffload::connect(const std::string &address) {
  for (const auto &found : this->found_) {
    if (format_address(found.address.data()) != address)
      continue;
    uint8_t msg[8];
    std::memcpy(msg, found.address.data(), 6);
    msg[6] = found.transport;
    msg[7] = found.addr_type;
    return this->command_(TYPE_CONNECT, msg, sizeof(msg));
  }
  ESP_LOGW(TAG, "%s was not found by the last scan", address.c_str());
  return false;
}

bool BTOffload::audio_connect(const std::string &address) {
  std::array<uint8_t, 6> bytes{};
  unsigned value[6];
  if (std::sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &value[0], &value[1], &value[2], &value[3], &value[4],
                  &value[5]) != 6) {
    ESP_LOGW(TAG, "%s is no Bluetooth address", address.c_str());
    return false;
  }
  for (size_t i = 0; i < 6; i++)
    bytes[i] = static_cast<uint8_t>(value[i]);
  this->audio_address_ = bytes;
  return this->command_(TYPE_AUDIO_CONNECT, bytes.data(), bytes.size());
}

bool BTOffload::audio_disconnect() {
  if (!this->audio_connected_)
    return false;
  return this->command_(TYPE_AUDIO_DISCONNECT, this->audio_address_.data(), this->audio_address_.size());
}

bool BTOffload::set_audio_volume(uint8_t volume) { return this->command_(TYPE_AUDIO_VOLUME, &volume, 1); }

void BTOffload::on_audio_state_(const uint8_t *data, size_t len) {
  if (len < 7)
    return;
  std::memcpy(this->audio_address_.data(), data, 6);
  this->audio_connected_ = data[6] != 0;
  ESP_LOGI(TAG, "Audio %s %s", format_address(data).c_str(), this->audio_connected_ ? "connected" : "disconnected");
#ifdef USE_BT_OFFLOAD_SPEAKER
  if (this->speaker_ != nullptr)
    this->speaker_->on_audio_state(this->audio_connected_);
#endif
}

void BTOffload::on_audio_credit_(const uint8_t *data, size_t len) {
#ifdef USE_BT_OFFLOAD_SPEAKER
  if (len < 4 || this->speaker_ == nullptr)
    return;
  uint32_t bytes;
  std::memcpy(&bytes, data, sizeof(bytes));
  this->speaker_->on_credit(bytes);
#endif
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
