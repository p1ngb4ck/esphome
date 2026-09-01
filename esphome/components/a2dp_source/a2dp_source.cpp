#include "a2dp_source.h"

#include "select/a2dp_device_select.h"

#ifdef USE_ESP32_VARIANT_ESP32

#include "esphome/core/log.h"

#include <nvs.h>

#include <cstring>

namespace esphome {
namespace a2dp_source {

static const char *const TAG = "a2dp_source";

static const uint32_t SAMPLE_RATE = 44100;
static const size_t BYTES_PER_FRAME = 4;

static const char *const NVS_NAMESPACE = "connected_bda";
static const char *const NVS_KEY = "src_bda";

static A2DPSource *global_a2dp_source = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

bool A2DPSource::device_filter_(const char *name, esp_bd_addr_t address, int rssi) {
  if (global_a2dp_source == nullptr) {
    return false;
  }
  return global_a2dp_source->accept_device_(name, address, rssi);
}

int32_t A2DPSource::audio_callback_(Frame *frames, int32_t frame_count) {
  if (global_a2dp_source == nullptr) {
    memset(frames, 0, (size_t) frame_count * BYTES_PER_FRAME);
    return frame_count;
  }
  return global_a2dp_source->fill_frames_(frames, frame_count);
}

void A2DPSource::connection_state_(esp_a2d_connection_state_t state, void *self) {
  auto *component = static_cast<A2DPSource *>(self);
  if (component == nullptr) {
    return;
  }
  component->pending_connection_state_.store((uint8_t) state);
  bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  bool was = component->connected_.exchange(connected);
  if (connected && !was) {
    component->connect_pending_.store(true);
  } else if (!connected && was) {
    component->disconnect_pending_.store(true);
  }
}

void A2DPSource::audio_state_(esp_a2d_audio_state_t state, void *self) {
  auto *component = static_cast<A2DPSource *>(self);
  if (component == nullptr) {
    return;
  }
  component->pending_audio_state_.store((uint8_t) state);
  bool streaming = (state == ESP_A2D_AUDIO_STATE_STARTED);
  bool was = component->streaming_.exchange(streaming);
  if (streaming && !was) {
    component->streaming_start_pending_.store(true);
  } else if (!streaming && was) {
    component->streaming_stop_pending_.store(true);
  }
}

void A2DPSource::setup() {
  global_a2dp_source = this;

  this->devices_.reserve(MAX_DISCOVERED);

  size_t ring_bytes = (size_t) SAMPLE_RATE * BYTES_PER_FRAME * this->buffer_duration_ms_ / 1000;
  this->ring_ = xRingbufferCreate(ring_bytes, RINGBUF_TYPE_BYTEBUF);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the %u byte buffer", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }

  if (this->mic_source_ != nullptr) {
    this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
      if (data.empty()) {
        return;
      }
      if (xRingbufferSend(this->ring_, data.data(), data.size(), 0) != pdTRUE) {
        size_t dropped = 0;
        void *stale = xRingbufferReceiveUpTo(this->ring_, &dropped, 0, data.size());
        if (stale != nullptr) {
          vRingbufferReturnItem(this->ring_, stale);
        }
        xRingbufferSend(this->ring_, data.data(), data.size(), 0);
      }
    });
    this->mic_source_->start();
  }

  this->source_.set_local_name(this->local_name_.c_str());
  this->source_.set_volume(this->volume_);
  this->source_.set_data_callback_in_frames(A2DPSource::audio_callback_);
  this->source_.set_ssid_callback(A2DPSource::device_filter_);
  this->source_.set_on_connection_state_changed(A2DPSource::connection_state_, this);
  this->source_.set_on_audio_state_changed(A2DPSource::audio_state_, this);

  this->source_.set_auto_reconnect(true);

  bool stored = this->has_stored_device();
  if (!stored && this->pair_on_boot_if_empty_) {
    ESP_LOGI(TAG, "No device stored, opening the pairing window");
    this->start_pairing();
  } else if (stored) {
    ESP_LOGD(TAG, "Connecting to the stored device");
  } else {
    ESP_LOGI(TAG, "No device stored, call a2dp_source.start_pairing to add one");
  }

  this->source_.start();
  this->started_.store(true);
}

bool A2DPSource::has_stored_device() const {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  esp_bd_addr_t addr = {};
  size_t size = sizeof(addr);
  esp_err_t err = nvs_get_blob(handle, NVS_KEY, addr, &size);
  nvs_close(handle);
  if (err != ESP_OK || size != sizeof(addr)) {
    return false;
  }
  for (size_t i = 0; i < sizeof(addr); i++) {
    if (addr[i] != 0) {
      return true;
    }
  }
  return false;
}

void A2DPSource::start_pairing() {
  uint32_t now = millis();
  this->devices_.clear();
  this->chosen_valid_ = false;
  this->devices_changed_.store(true);
  this->best_valid_ = false;
  this->best_rssi_ = -128;
  this->best_name_[0] = '\0';
  this->settle_until_ms_ = now + this->settle_seconds_ * 1000;
  this->fallback_after_ms_ = this->settle_until_ms_ + this->fallback_seconds_ * 1000;
  this->pairing_mode_.store(true);

  ESP_LOGI(TAG, "Pairing window open");
  if (this->device_select_ != nullptr) {
    ESP_LOGD(TAG, "Found devices appear in the select");
  }
}

bool A2DPSource::pair_with_name(const std::string &name) {
  for (const auto &device : this->devices_) {
    if (device.name != name) {
      continue;
    }
    memcpy(this->chosen_addr_, device.address, ESP_BD_ADDR_LEN);
    this->chosen_valid_ = true;
    this->pairing_mode_.store(true);
    ESP_LOGI(TAG, "Pairing with %s on its next sighting", name.c_str());
    return true;
  }
  ESP_LOGW(TAG, "%s was not among the devices found", name.c_str());
  return false;
}

void A2DPSource::publish_device_list_() {
  if (this->device_select_ == nullptr) {
    return;
  }
  this->device_select_->set_devices(this->devices_);
}

void A2DPSource::forget_device() {
  ESP_LOGI(TAG, "Forgetting the stored device");
  this->source_.clean_last_connection();
}

bool A2DPSource::accept_device_(const char *name, esp_bd_addr_t address, int rssi) {
  if (!this->pairing_mode_.load()) {
    return false;
  }

  ESP_LOGD(TAG, "Found %-24s %02x:%02x:%02x:%02x:%02x:%02x  %d dBm", name, address[0], address[1], address[2],
           address[3], address[4], address[5], rssi);

  bool known = false;
  for (auto &device : this->devices_) {
    if (memcmp(device.address, address, ESP_BD_ADDR_LEN) == 0) {
      device.rssi = rssi;
      known = true;
      break;
    }
  }
  if (!known && this->devices_.size() < MAX_DISCOVERED) {
    DiscoveredDevice device;
    device.name = name;
    memcpy(device.address, address, ESP_BD_ADDR_LEN);
    device.rssi = rssi;
    this->devices_.push_back(device);
    this->devices_changed_.store(true);
  }

  if (this->chosen_valid_) {
    if (memcmp(address, this->chosen_addr_, ESP_BD_ADDR_LEN) != 0) {
      return false;
    }
    ESP_LOGI(TAG, "Connecting to the chosen device");
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }

  if (!this->target_name_.empty()) {
    if (strncmp(name, this->target_name_.c_str(), this->target_name_.size()) != 0) {
      return false;
    }
    ESP_LOGI(TAG, "Connecting to the target_name match");
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }

  if (this->device_select_ != nullptr) {
    return false;
  }

  uint32_t now = millis();

  if (now < this->settle_until_ms_) {
    if (!this->best_valid_ || rssi > this->best_rssi_) {
      strncpy(this->best_name_, name, sizeof(this->best_name_) - 1);
      this->best_name_[sizeof(this->best_name_) - 1] = '\0';
      memcpy(this->best_addr_, address, ESP_BD_ADDR_LEN);
      this->best_rssi_ = rssi;
      this->best_valid_ = true;
    }
    return false;
  }

  if (this->best_valid_ && memcmp(address, this->best_addr_, ESP_BD_ADDR_LEN) == 0) {
    ESP_LOGI(TAG, "Connecting to the strongest device at %d dBm", this->best_rssi_);
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }

  if (now > this->fallback_after_ms_) {
    ESP_LOGW(TAG, "%s stopped answering, connecting to this one instead",
             this->best_valid_ ? this->best_name_ : "(nothing)");
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }
  return false;
}

int32_t A2DPSource::fill_frames_(Frame *frames, int32_t frame_count) {
  size_t wanted = (size_t) frame_count * BYTES_PER_FRAME;
  auto *out = reinterpret_cast<uint8_t *>(frames);
  size_t filled = 0;

  while (filled < wanted) {
    size_t got = 0;
    void *item = xRingbufferReceiveUpTo(this->ring_, &got, pdMS_TO_TICKS(2), wanted - filled);
    if (item == nullptr) {
      break;
    }
    memcpy(out + filled, item, got);
    vRingbufferReturnItem(this->ring_, item);
    filled += got;
  }

  if (filled < wanted) {
    memset(out + filled, 0, wanted - filled);
    this->underrun_frames_.fetch_add((uint32_t) ((wanted - filled) / BYTES_PER_FRAME));
  }
  return frame_count;
}

void A2DPSource::loop() {
  uint8_t conn_state = this->pending_connection_state_.exchange(0xFF);
  if (conn_state != 0xFF) {
    ESP_LOGD(TAG, "Connection state: %s", this->source_.to_str((esp_a2d_connection_state_t) conn_state));
  }
  uint8_t audio_state = this->pending_audio_state_.exchange(0xFF);
  if (audio_state != 0xFF) {
    ESP_LOGD(TAG, "Audio state: %s", this->source_.to_str((esp_a2d_audio_state_t) audio_state));
  }

  if (this->devices_changed_.exchange(false)) {
    this->publish_device_list_();
  }
  if (this->paired_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Paired with %s", this->paired_name_.c_str());
    for (auto *trigger : this->paired_triggers_) {
      trigger->trigger(this->paired_name_);
    }
  }
  if (this->connect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Connected");
    for (auto *trigger : this->connected_triggers_) {
      trigger->trigger();
    }
  }
  if (this->disconnect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Disconnected");
    for (auto *trigger : this->disconnected_triggers_) {
      trigger->trigger();
    }
  }
  if (this->streaming_start_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Streaming");
    for (auto *trigger : this->streaming_start_triggers_) {
      trigger->trigger();
    }
  }
  if (this->streaming_stop_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Streaming stopped");
    for (auto *trigger : this->streaming_stop_triggers_) {
      trigger->trigger();
    }
  }

  uint32_t now = millis();
  if (now - this->last_report_ms_ >= 10000) {
    this->last_report_ms_ = now;
    uint32_t frames = this->underrun_frames_.exchange(0);
    if (frames > 0) {
      ESP_LOGW(TAG, "Inserted %" PRIu32 " frames of silence in the last 10 s (%" PRIu32 " ms)", frames,
               frames * 1000 / SAMPLE_RATE);
    }
  }
}

void A2DPSource::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP source:");
  ESP_LOGCONFIG(TAG, "  Local name: %s", this->local_name_.c_str());
  if (this->target_name_.empty()) {
    ESP_LOGCONFIG(TAG, "  Target: strongest signal in the pairing window");
    ESP_LOGCONFIG(TAG, "  Settle time: %" PRIu32 " s", this->settle_seconds_);
    ESP_LOGCONFIG(TAG, "  Fallback after: %" PRIu32 " s", this->fallback_seconds_);
  } else {
    ESP_LOGCONFIG(TAG, "  Target name: %s", this->target_name_.c_str());
  }
  if (this->device_select_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Target: chosen from the select");
  }
  ESP_LOGCONFIG(TAG, "  Buffer: %" PRIu32 " ms", this->buffer_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Device stored: %s", YESNO(this->has_stored_device()));
}

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32
