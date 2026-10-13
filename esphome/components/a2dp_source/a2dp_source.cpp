#include "a2dp_source.h"

#ifdef USE_A2DP_SOURCE_SELECT
#include "select/a2dp_device_select.h"
#endif

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "esphome/components/bt_classic/bt_classic.h"
#include "esphome/core/log.h"

#include <esp_timer.h>

#include <nvs.h>

#include <algorithm>
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
    memset(static_cast<void *>(frames), 0, (size_t) frame_count * BYTES_PER_FRAME);
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

bool A2DPSource::discovery_gate_() { return global_a2dp_source != nullptr && global_a2dp_source->pairing_mode_.load(); }

bool A2DPSource::reconnect_gate_() { return global_a2dp_source != nullptr && global_a2dp_source->device_kept_.load(); }

bool A2DPSource::gap_fallback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg) {
  return static_cast<A2DPSource *>(arg)->source_.handle_unclaimed_pairing(event, param);
}

bool A2DPSource::gap_handler_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg) {
  return static_cast<A2DPSource *>(arg)->source_.handle_gap_event(event, param);
}

void A2DPSource::authentication_complete_(esp_bt_status_t status) {
  if (global_a2dp_source == nullptr) {
    return;
  }

  global_a2dp_source->paired_pending_.store(status == ESP_BT_STATUS_SUCCESS);
  if (status == ESP_BT_STATUS_SUCCESS)
    global_a2dp_source->pairing_mode_.store(false);
}

void A2DPSource::setup() {
  global_a2dp_source = this;

  this->devices_.reserve(MAX_DISCOVERED);

  size_t ring_bytes = (size_t) SAMPLE_RATE * BYTES_PER_FRAME * this->buffer_duration_ms_ / 1000;
  ring_bytes -= ring_bytes % BYTES_PER_FRAME;
  this->ring_ = xRingbufferCreate(ring_bytes, RINGBUF_TYPE_BYTEBUF);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the %u byte buffer", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }
  this->ring_bytes_ = ring_bytes;

#ifdef USE_MICROPHONE
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
#endif

  this->source_.set_local_name(this->local_name_.c_str());
  this->source_.set_pin_code(this->pin_code_.c_str(), ESP_BT_PIN_TYPE_VARIABLE);
  this->source_.set_task_core(CONFIG_BT_BLUEDROID_PINNED_TO_CORE);
  this->source_.set_volume(this->volume_);
  this->source_.set_data_callback_in_frames(A2DPSource::audio_callback_);
  this->source_.set_ssid_callback(A2DPSource::device_filter_);
  this->source_.set_on_connection_state_changed(A2DPSource::connection_state_, this);
  this->source_.set_on_audio_state_changed(A2DPSource::audio_state_, this);
  this->source_.set_on_authentication_complete(A2DPSource::authentication_complete_);

  this->media_wanted_ = this->idle_timeout_ms_ == 0;
#ifdef USE_MICROPHONE
  if (this->mic_source_ != nullptr)
    this->media_wanted_ = true;
#endif
  this->idle_since_ms_ = millis() - this->idle_timeout_ms_;
  this->source_.set_media_active(this->media_wanted_);
  this->source_.set_auto_reconnect(true);
  this->source_.set_discovery_gate(A2DPSource::discovery_gate_);
  this->source_.set_reconnect_gate(A2DPSource::reconnect_gate_);
  bt_classic::add_gap_handler(A2DPSource::gap_handler_, this);
  bt_classic::add_gap_fallback(A2DPSource::gap_fallback_, this);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    this->device_kept_.store(false);
    this->pairing_->add_target(this);
    this->pairing_->add_on_forget_callback([this](const bt_pairing::PairingRecord &record) {
      if (record.slot == this->slot_)
        this->drop_device_(false);
    });
  }
#endif

  bool stored = this->has_stored_device();
  if (!stored && this->pair_on_boot_if_empty_) {
    ESP_LOGI(TAG, "No device stored, opening the pairing window");
    this->start_pairing();
  } else if (stored) {
    ESP_LOGD(TAG, "Connecting to the stored device");
  } else {
    ESP_LOGI(TAG, "No device stored, call a2dp_source.start_pairing to add one");
  }

#ifdef USE_A2DP_SOURCE_EXTERNAL_HOST
  // Bluedroid is brought up by usb_bluetooth once its dongle is ready; loop() starts then.
#else
  this->source_.start();
  this->started_.store(true);
#endif
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
  if (this->started_.load())
    this->source_.resume_discovery();

  ESP_LOGI(TAG, "Pairing window open");
  if (this->device_select_ != nullptr) {
    ESP_LOGD(TAG, "Found devices appear in the select");
  }
}

bool A2DPSource::pair_with_index(size_t index) {
  if (index >= this->devices_.size())
    return false;
  const DiscoveredDevice &device = this->devices_[index];
  memcpy(this->chosen_addr_, device.address, ESP_BD_ADDR_LEN);
  this->chosen_valid_ = true;
  this->pairing_mode_.store(true);
  if (this->started_.load())
    this->source_.resume_discovery();
  ESP_LOGI(TAG, "Pairing with %s on its next sighting", device.name.c_str());
  return true;
}

#ifdef USE_BT_PAIRING
bool A2DPSource::pair_device(const bt_pairing::ScannedDevice &device) {
  if (!this->started_.load()) {
    ESP_LOGW(TAG, "Bluetooth is not running, cannot pair");
    return false;
  }
  if (this->connected_.load()) {
    ESP_LOGW(TAG, "Connected to another device; forget it and switch it off first");
    return false;
  }
  memcpy(this->chosen_addr_, device.address, ESP_BD_ADDR_LEN);
  this->chosen_valid_ = true;
  this->picked_ = device;
  this->picked_valid_ = true;
  this->pairing_mode_.store(true);
  this->source_.resume_discovery();
  ESP_LOGI(TAG, "Pairing with %s on its next sighting", device.name.c_str());
  return true;
}
#endif

bool A2DPSource::pair_with_name(const std::string &name) {
  for (const auto &device : this->devices_) {
    if (device.name != name) {
      continue;
    }
    memcpy(this->chosen_addr_, device.address, ESP_BD_ADDR_LEN);
    this->chosen_valid_ = true;
    this->pairing_mode_.store(true);
    if (this->started_.load())
      this->source_.resume_discovery();
    ESP_LOGI(TAG, "Pairing with %s on its next sighting", name.c_str());
    return true;
  }
  ESP_LOGW(TAG, "%s was not among the devices found", name.c_str());
  return false;
}

void A2DPSource::publish_device_list_() {
#ifdef USE_A2DP_SOURCE_SELECT
  if (this->device_select_ != nullptr)
    this->device_select_->set_devices(this->devices_);
#endif
}

void A2DPSource::stop_pairing() {
  if (!this->pairing_mode_.load())
    return;
  this->pairing_mode_.store(false);
  this->chosen_valid_ = false;
  this->best_valid_ = false;
  ESP_LOGI(TAG, "Pairing window closed");
}

void A2DPSource::forget_device() {
#ifdef USE_BT_PAIRING
  // bt_pairing drops the record and the bond; its forget callback lets go of the device here.
  if (this->pairing_ != nullptr && this->pairing_->find_slot(this->slot_) != nullptr) {
    this->pairing_->forget_slot(this->slot_);
    return;
  }
#endif
  ESP_LOGI(TAG, "Forgetting the stored device");
  this->drop_device_(true);
}

#ifdef USE_BT_PAIRING
// The device picked in the pairing scan is kept once it has paired or connected, whichever is first.
void A2DPSource::keep_picked_() {
  if (this->pairing_ == nullptr || !this->picked_valid_)
    return;
  this->picked_valid_ = false;
  bt_pairing::PairingRecord record{};
  memcpy(record.address, this->picked_.address, sizeof(record.address));
  record.transport = static_cast<uint8_t>(bt_pairing::Transport::CLASSIC);
  record.slot = this->slot_;
  record.device_class = this->picked_.device_class;
  const std::string &name = this->picked_.name.empty() ? this->paired_name_ : this->picked_.name;
  strncpy(record.name, name.c_str(), sizeof(record.name) - 1);
  this->pairing_->store(record);
}
#endif

void A2DPSource::drop_device_(bool delete_bond) {
  if (this->connected_.load())
    this->source_.disconnect();
  esp_bd_addr_t *address = this->source_.get_last_peer_address();
  static const esp_bd_addr_t NO_ADDRESS = {};
  if (delete_bond && memcmp(*address, NO_ADDRESS, ESP_BD_ADDR_LEN) != 0 &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
    esp_bt_gap_remove_bond_device(*address);
  this->source_.clean_last_connection();
}

bool A2DPSource::accept_device_(const char *name, esp_bd_addr_t address, int rssi) {
  if (!this->pairing_mode_.load()) {
    return false;
  }

  ESP_LOGV(TAG, "Found %-24s %02x:%02x:%02x:%02x:%02x:%02x  %d dBm", name, address[0], address[1], address[2],
           address[3], address[4], address[5], rssi);

  // A device without EIR reports its name in a later result that has no RSSI
  const bool name_only = rssi < -128;
  bool known = false;
  for (auto &device : this->devices_) {
    if (memcmp(device.address, address, ESP_BD_ADDR_LEN) == 0) {
      if (!name_only)
        device.rssi = rssi;
      if (*name != '\0' && device.name != name) {
        device.name = name;
        this->devices_changed_.store(true);
      }
      known = true;
      break;
    }
  }
  if (!known && !name_only && this->devices_.size() < MAX_DISCOVERED) {
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
    return true;
  }

  if (name_only && !known)
    return false;

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

  if (name_only)
    return false;

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

  bool producer = this->speaker_active_.load();
#ifdef USE_MICROPHONE
  producer = producer || this->mic_source_ != nullptr;
#endif
  if (this->speaker_active_.load())
    this->played_frames_.fetch_add((uint32_t) (filled / BYTES_PER_FRAME));
  if (filled < wanted) {
    memset(out + filled, 0, wanted - filled);
    if (producer)
      this->underrun_frames_.fetch_add((uint32_t) ((wanted - filled) / BYTES_PER_FRAME));
  }
  this->keep_pace_(frame_count);
  return frame_count;
}

// Bluedroid fetches what its send queue has room for; while the link lags that is less than real
// time and it drops nothing, so audio piles up in front of it and the delay grows. Measured
// against the clock, what it is behind is skipped at the front of the buffer.
void A2DPSource::keep_pace_(int32_t frame_count) {
  static constexpr int64_t RESTART_US = 500000;              // a gap this long: the stream restarted
  static constexpr int64_t SLACK_FRAMES = SAMPLE_RATE / 16;  // about two of Bluedroid's 30 ms ticks
  const int64_t now = esp_timer_get_time();
  if (this->pace_start_us_ == 0 || now - this->pace_last_us_ > RESTART_US) {
    this->pace_start_us_ = now;
    this->pace_frames_ = 0;
  }
  this->pace_last_us_ = now;
  this->pace_frames_ += frame_count;
  const int64_t due = (now - this->pace_start_us_) * SAMPLE_RATE / 1000000;
  int64_t behind = due - this->pace_frames_ - SLACK_FRAMES;
  while (behind > 0) {
    size_t got = 0;
    void *item = xRingbufferReceiveUpTo(this->ring_, &got, 0, (size_t) behind * BYTES_PER_FRAME);
    if (item == nullptr)
      break;
    vRingbufferReturnItem(this->ring_, item);
    const int64_t frames = (int64_t) (got / BYTES_PER_FRAME);
    this->dropped_frames_.fetch_add((uint32_t) frames);
    this->pace_frames_ += frames;
    behind -= frames;
  }
  // Nothing left to skip: the delay is not here, so the debt is not carried on.
  if (behind > 0)
    this->pace_frames_ += behind;
}

size_t A2DPSource::write_audio(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->ring_ == nullptr)
    return 0;
  // While the stream starts (or resumes after idling) the audio waits in the buffer
  if (!this->connected_.load())
    return length;
  size_t chunk = std::min(length, this->ring_bytes_ / 4);
  const size_t free = xRingbufferGetCurFreeSize(this->ring_);
  if (free >= BYTES_PER_FRAME)
    chunk = std::min(chunk, free);
  chunk -= chunk % BYTES_PER_FRAME;
  if (chunk == 0)
    return 0;
  return xRingbufferSend(this->ring_, data, chunk, ticks_to_wait) == pdTRUE ? chunk : 0;
}

void A2DPSource::clear_audio() {
  if (this->ring_ == nullptr)
    return;
  size_t got = 0;
  while (void *item = xRingbufferReceiveUpTo(this->ring_, &got, 0, this->ring_bytes_))
    vRingbufferReturnItem(this->ring_, item);
}

bool A2DPSource::has_audio() const {
  if (this->ring_ == nullptr)
    return false;
  UBaseType_t waiting = 0;
  vRingbufferGetInfo(this->ring_, nullptr, nullptr, nullptr, nullptr, &waiting);
  return waiting != 0;
}

void A2DPSource::update_media_() {
  if (this->idle_timeout_ms_ == 0)
    return;
  bool producer = this->speaker_active_.load();
#ifdef USE_MICROPHONE
  producer = producer || this->mic_source_ != nullptr;
#endif
  const uint32_t now = millis();
  if (producer || this->has_audio())
    this->idle_since_ms_ = now;
  const bool wanted = now - this->idle_since_ms_ < this->idle_timeout_ms_;
  if (wanted == this->media_wanted_)
    return;
  this->media_wanted_ = wanted;
  this->source_.set_media_active(wanted);
}

void A2DPSource::loop() {
  bt_classic::ensure_gap_callback();
#ifdef USE_A2DP_SOURCE_EXTERNAL_HOST
  if (!this->started_.load()) {
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
      return;
    ESP_LOGI(TAG, "Bluetooth is up, starting");
    this->source_.start();
    this->started_.store(true);
  }
#endif
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr)
    this->device_kept_.store(this->pairing_->find_slot(this->slot_) != nullptr, std::memory_order_relaxed);
#endif
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
  conn_state = this->pending_connection_state_.exchange(0xFF);
  if (conn_state != 0xFF) {
    ESP_LOGD(TAG, "Connection state: %s", this->source_.to_str((esp_a2d_connection_state_t) conn_state));
  }
  audio_state = this->pending_audio_state_.exchange(0xFF);
  if (audio_state != 0xFF) {
    ESP_LOGD(TAG, "Audio state: %s", this->source_.to_str((esp_a2d_audio_state_t) audio_state));
  }

  if (this->paired_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Paired with %s", this->paired_name_.c_str());
#ifdef USE_BT_PAIRING
    this->keep_picked_();
#endif
    for (auto *trigger : this->paired_triggers_) {
      trigger->trigger(this->paired_name_);
    }
  }
  if (this->connect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Connected");
    for (auto *trigger : this->connected_triggers_) {
      trigger->trigger();
    }
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr) {
      this->keep_picked_();
      this->pairing_->on_connection(this, true);
    }
#endif
  }
  if (this->disconnect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Disconnected");
    for (auto *trigger : this->disconnected_triggers_) {
      trigger->trigger();
    }
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr)
      this->pairing_->on_connection(this, false);
#endif
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

  this->update_media_();

  uint32_t now = millis();
  if (now - this->last_report_ms_ >= 10000) {
    this->last_report_ms_ = now;
    uint32_t frames = this->underrun_frames_.exchange(0);
    if (frames > 0) {
      ESP_LOGW(TAG, "Inserted %" PRIu32 " frames of silence in the last 10 s (%" PRIu32 " ms)", frames,
               frames * 1000 / SAMPLE_RATE);
    }
    frames = this->dropped_frames_.exchange(0);
    if (frames > 0) {
      ESP_LOGW(TAG, "Skipped %" PRIu32 " frames in the last 10 s (%" PRIu32 " ms): Bluetooth fell behind", frames,
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
  ESP_LOGCONFIG(TAG, "  Idle timeout: %" PRIu32 " ms", this->idle_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Device stored: %s", YESNO(this->has_stored_device()));
}

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32 || USE_A2DP_SOURCE_EXTERNAL_HOST
