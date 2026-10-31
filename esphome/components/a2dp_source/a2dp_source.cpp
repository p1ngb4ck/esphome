#include "a2dp_source.h"

#ifdef USE_A2DP_SOURCE_SELECT
#include "select/a2dp_device_select.h"
#endif

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#include "esphome/components/bt_classic/bt_classic.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <nvs.h>
#ifdef USE_BT_PERSISTENCE
#include "osi/esphome_nvs_redirect.h"
#endif

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
  if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    component->reported_delay_ms_.store(0, std::memory_order_relaxed);
    if (component->streaming_.exchange(false))
      component->streaming_stop_pending_.store(true);
  }
}

void A2DPSource::delay_report_(uint16_t delay, void *self) {
  auto *component = static_cast<A2DPSource *>(self);
  if (component == nullptr)
    return;
  component->reported_delay_ms_.store(std::min<uint32_t>((delay + 5u) / 10u, MAX_OUTPUT_DELAY_MS),
                                      std::memory_order_relaxed);
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

bool A2DPSource::gap_fallback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg) {
  return static_cast<A2DPSource *>(arg)->source_.handle_unclaimed_pairing(event, param);
}

bool A2DPSource::gap_handler_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg) {
  auto *self = static_cast<A2DPSource *>(arg);
#ifdef USE_BT_PAIRING
  if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT && self->name_wanted_.load() &&
      memcmp(param->read_rmt_name.bda, self->name_addr_, ESP_BD_ADDR_LEN) == 0) {
    self->rmt_name_[0] = '\0';
    if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
      strncpy(self->rmt_name_, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name),
              sizeof(self->rmt_name_) - 1);
      self->rmt_name_[sizeof(self->rmt_name_) - 1] = '\0';
    }
    self->name_wanted_.store(false);
    self->name_ready_.store(true);
    return false;
  }
#endif
  return self->source_.handle_gap_event(event, param);
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
  // PSRAM only: no fall-back into internal RAM.
  this->ring_ = xRingbufferCreateWithCaps(ring_bytes, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate the %u byte buffer in PSRAM", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }
  this->ring_bytes_ = ring_bytes;

#ifdef USE_MICROPHONE
  if (this->mic_source_ != nullptr) {
    this->mic_source_->add_data_callback([this](const microphone::AudioCapture &capture) {
      const std::span<const uint8_t> data = capture.data;
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
      this->fed_.store(true);
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
  this->source_.set_on_delay_report(A2DPSource::delay_report_, this);
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
  bt_classic::add_gap_handler(A2DPSource::gap_handler_, this);
  bt_classic::add_gap_fallback(A2DPSource::gap_fallback_, this);
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr) {
    this->pairing_->add_target(this);
    this->pairing_->add_on_forget_callback([this](const bt_pairing::PairingRecord &record) {
      if (record.slot != this->slot_)
        return;
      if (this->is_address_connected(record.address))
        this->source_.disconnect();
      if (memcmp(record.address, *this->source_.get_last_peer_address(), ESP_BD_ADDR_LEN) != 0)
        return;
      // Another paired device takes its place, else none is stored.
      for (const auto &other : this->pairing_->records()) {
        if (other.slot == this->slot_) {
          esp_bd_addr_t addr;
          memcpy(addr, other.address, ESP_BD_ADDR_LEN);
          this->source_.store_device(addr);
          return;
        }
      }
      this->forget_device();
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
    ESP_LOGI(TAG, "No device stored");
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
    ESP_LOGV(TAG, "Found devices appear in the select");
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
    if (memcmp(device.address, *this->source_.get_peer_address(), ESP_BD_ADDR_LEN) == 0) {
      // The device picked is the one connected (paired before the database held it): kept as it is.
      bt_pairing::PairingRecord record{};
      memcpy(record.address, device.address, sizeof(record.address));
      record.transport = static_cast<uint8_t>(bt_pairing::Transport::CLASSIC);
      record.slot = this->slot_;
      record.device_class = device.device_class;
      strncpy(record.name, device.name.c_str(), sizeof(record.name) - 1);
      if (!this->pairing_->store(record))
        return false;
      this->keep_pending_ = false;
      this->defer([this]() { this->pairing_->on_connection(this, true); });
      return true;
    }
  }
  memcpy(this->chosen_addr_, device.address, ESP_BD_ADDR_LEN);
  this->chosen_valid_ = true;
  this->picked_ = device;
  this->picked_valid_ = true;
  this->pairing_mode_.store(true);
  this->switch_pending_ = false;
  // The stored device (connected again if it was) comes back if the pairing does not succeed.
  memcpy(this->restore_addr_, *this->source_.get_last_peer_address(), ESP_BD_ADDR_LEN);
  this->restore_valid_ = this->source_.has_stored_peer();
  this->restore_connect_ = this->connected_.load();
  if (this->connected_.load()) {
    // The one connected goes first; the search resumes once it has (loop()).
    ESP_LOGI(TAG, "Disconnecting %s to pair another device", this->source_.to_str(this->connected_addr_));
    this->source_.disconnect();
    return true;
  }
  this->source_.resume_discovery();
  ESP_LOGI(TAG, "Pairing with %s on its next sighting", device.name.c_str());
  return true;
}

void A2DPSource::connect_address(const uint8_t *address) {
  if (!this->started_.load())
    return;
  if (this->connected_.load()) {
    if (memcmp(this->connected_addr_, address, ESP_BD_ADDR_LEN) == 0)
      return;
    // One device at a time: the one connected goes, then the one asked for is connected (loop()).
    memcpy(this->switch_addr_, address, ESP_BD_ADDR_LEN);
    this->switch_pending_ = true;
    this->source_.disconnect();
    return;
  }
  esp_bd_addr_t addr;
  memcpy(addr, address, ESP_BD_ADDR_LEN);
  this->source_.connect_to_device(addr);
}

void A2DPSource::disconnect_address(const uint8_t *address) {
  if (this->is_address_connected(address))
    this->source_.disconnect();
}

// The device picked was found but did not connect: the pairing ends, the one before comes back.
void A2DPSource::check_pick_failed_(uint8_t conn_state) {
  if (conn_state != ESP_A2D_CONNECTION_STATE_DISCONNECTED || !this->picked_valid_ || this->pairing_mode_.load() ||
      this->connected_.load())
    return;
  ESP_LOGW(TAG, "Pairing with %s failed", this->picked_.name.c_str());
  this->picked_valid_ = false;
  this->restore_stored_();
}

void A2DPSource::restore_stored_() {
  if (!this->restore_valid_)
    return;
  this->restore_valid_ = false;
  if (this->connected_.load() || !this->started_.load())
    return;
  esp_bd_addr_t addr;
  memcpy(addr, this->restore_addr_, ESP_BD_ADDR_LEN);
  ESP_LOGI(TAG, "Pairing did not succeed, back to %s", this->source_.to_str(addr));
  if (this->restore_connect_) {
    this->source_.connect_to_device(addr);
  } else {
    this->source_.store_device(addr);
  }
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
#ifdef USE_BT_PAIRING
  // A pick still pending ends with the window.
  this->picked_valid_ = false;
  this->restore_stored_();
#endif
}

// The connected device's delay: a preference per device address.
void A2DPSource::load_output_delay_() {
  const uint8_t *address = *this->source_.get_last_peer_address();
  uint32_t hash = fnv1_hash("a2dp_source_output_delay");
  for (int i = 0; i < ESP_BD_ADDR_LEN; i++)
    hash = fnv1_hash_extend(hash, address[i]);
  this->delay_pref_ = global_preferences->make_preference<uint32_t>(hash, true);
  this->delay_pref_valid_ = true;
  uint32_t ms = 0;
  if (!this->delay_pref_.load(&ms))
    ms = 0;
  this->delay_ms_.store(std::min(ms, MAX_OUTPUT_DELAY_MS), std::memory_order_relaxed);
  ESP_LOGV(TAG, "Output delay of this device: %" PRIu32 " ms", this->delay_ms_.load(std::memory_order_relaxed));
  this->publish_output_delay_();
}

void A2DPSource::set_output_delay_ms(uint32_t ms) {
  if (!this->connected_.load() || !this->delay_pref_valid_) {
    ESP_LOGW(TAG, "No device connected");
    this->publish_output_delay_();
    return;
  }
  ms = std::min(ms, this->max_delay_ms_.load(std::memory_order_relaxed));
  this->delay_ms_.store(ms, std::memory_order_relaxed);
  this->delay_pref_.save(&ms);
  this->publish_output_delay_();
}

void A2DPSource::set_max_output_delay_ms(uint32_t ms) {
  ms = std::min(ms, MAX_OUTPUT_DELAY_MS);
  this->max_delay_ms_.store(ms, std::memory_order_relaxed);
#ifdef USE_A2DP_SOURCE_NUMBER
  if (this->delay_number_ != nullptr)
    this->delay_number_->traits.set_max_value(static_cast<float>(ms));
#endif
}

void A2DPSource::publish_output_delay_() {
#ifdef USE_A2DP_SOURCE_NUMBER
  if (this->delay_number_ != nullptr)
    this->delay_number_->publish_state(static_cast<float>(this->delay_ms_.load(std::memory_order_relaxed)));
#endif
}

void A2DPSource::forget_device() {
  ESP_LOGI(TAG, "Forgetting the stored device");
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

  const bool feeding = this->speaker_active_.load() && !this->speaker_finishing_.load();
  if (this->priming_.load()) {
    const uint32_t delay = this->output_delay_ms();
    size_t target = this->ring_bytes_;
    if (delay != 0) {
      target = std::min<size_t>(target, (size_t) SAMPLE_RATE * BYTES_PER_FRAME * delay / 1000);
      target -= target % BYTES_PER_FRAME;
    }
    const size_t used = this->ring_bytes_ - xRingbufferGetCurFreeSize(this->ring_);
    if (feeding && used < target) {
      memset(out, 0, wanted);
      return frame_count;
    }
    this->priming_.store(false);
  }

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
  const bool fed = this->fed_.exchange(false);
  if (fed && this->pending_gap_frames_ != 0)
    this->underrun_frames_.fetch_add(this->pending_gap_frames_);
  this->pending_gap_frames_ = 0;
  if (filled < wanted) {
    memset(out + filled, 0, wanted - filled);
    if (producer && fed)
      this->pending_gap_frames_ = (uint32_t) ((wanted - filled) / BYTES_PER_FRAME);
  }
  return frame_count;
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
  if (xRingbufferSend(this->ring_, data, chunk, ticks_to_wait) != pdTRUE)
    return 0;
  this->fed_.store(true);
  return chunk;
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
  const bool classic_up = bt_classic::ensure_gap_callback();
#ifdef USE_A2DP_SOURCE_EXTERNAL_HOST
  if (this->started_.load() && !classic_up) {
    // The dongle went (or Bluedroid): the profiles go down while Bluedroid can still take it, and
    // come back with start() once Classic is up again.
    ESP_LOGI(TAG, "Bluetooth Classic is down, stopping");
    this->source_.stack_down(esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED);
    this->started_.store(false);
    // Gone with the stack, no event says so: the connection (and stream) ends here.
    A2DPSource::audio_state_(ESP_A2D_AUDIO_STATE_SUSPEND, this);
    A2DPSource::connection_state_(ESP_A2D_CONNECTION_STATE_DISCONNECTED, this);
  }
  if (!this->started_.load()) {
    if (!classic_up)
      return;
    ESP_LOGI(TAG, "Bluetooth is up, starting");
    this->source_.start();
    this->started_.store(true);
  }
#endif
  uint8_t conn_state = this->pending_connection_state_.exchange(0xFF);
  if (conn_state != 0xFF) {
    ESP_LOGV(TAG, "Connection state: %s", this->source_.to_str((esp_a2d_connection_state_t) conn_state));
  }
#ifdef USE_BT_PAIRING
  this->check_pick_failed_(conn_state);
#endif
  uint8_t audio_state = this->pending_audio_state_.exchange(0xFF);
  if (audio_state != 0xFF) {
    ESP_LOGV(TAG, "Audio state: %s", this->source_.to_str((esp_a2d_audio_state_t) audio_state));
  }

  if (this->devices_changed_.exchange(false)) {
    this->publish_device_list_();
  }
  conn_state = this->pending_connection_state_.exchange(0xFF);
  if (conn_state != 0xFF) {
    ESP_LOGV(TAG, "Connection state: %s", this->source_.to_str((esp_a2d_connection_state_t) conn_state));
  }
#ifdef USE_BT_PAIRING
  this->check_pick_failed_(conn_state);
#endif
  audio_state = this->pending_audio_state_.exchange(0xFF);
  if (audio_state != 0xFF) {
    ESP_LOGV(TAG, "Audio state: %s", this->source_.to_str((esp_a2d_audio_state_t) audio_state));
  }

  if (this->paired_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Paired with %s", this->paired_name_.c_str());
    for (auto *trigger : this->paired_triggers_) {
      trigger->trigger(this->paired_name_);
    }
  }
  if (this->connect_pending_.exchange(false)) {
    memcpy(this->connected_addr_, *this->source_.get_peer_address(), ESP_BD_ADDR_LEN);
#ifdef USE_BT_PAIRING
    const bool was_stored =
        memcmp(this->connected_addr_, *this->source_.get_last_peer_address(), ESP_BD_ADDR_LEN) == 0;
    const bool is_picked =
        this->picked_valid_ && memcmp(this->connected_addr_, this->picked_.address, ESP_BD_ADDR_LEN) == 0;
    const bt_pairing::PairingRecord *known =
        this->pairing_ != nullptr ? this->pairing_->find_address(this->connected_addr_) : nullptr;
    // A device that connects on its own is taken only when paired here (or picked in a pairing window).
    if (this->pairing_ != nullptr && this->pairing_->is_loaded() && !this->pairing_mode_.load() && !is_picked &&
        !was_stored && (known == nullptr || known->slot != this->slot_)) {
      ESP_LOGW(TAG, "%s is not paired, disconnecting", this->source_.to_str(this->connected_addr_));
      this->rejected_ = true;
      this->source_.source_disconnect(this->connected_addr_);
    } else {
      this->source_.keep_peer(this->connected_addr_);
      if (is_picked)
        this->restore_valid_ = false;
#endif
    ESP_LOGI(TAG, "Connected to %s", this->source_.to_str(this->connected_addr_));
    this->load_output_delay_();
    for (auto *trigger : this->connected_triggers_) {
      trigger->trigger();
    }
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr) {
      // The device picked in the pairing scan is kept once it has connected.
      if (is_picked) {
        this->picked_valid_ = false;
        bt_pairing::PairingRecord record{};
        memcpy(record.address, this->picked_.address, sizeof(record.address));
        record.transport = static_cast<uint8_t>(bt_pairing::Transport::CLASSIC);
        record.slot = this->slot_;
        record.device_class = this->picked_.device_class;
        strncpy(record.name, this->picked_.name.c_str(), sizeof(record.name) - 1);
        this->pairing_->store(record);
      } else {
        // The stored device, known to Bluedroid and kept here, but missing from the pairing
        // database (stored before it held it): kept there too, under the name it reports.
        uint8_t *peer = this->connected_addr_;
        if (was_stored && known == nullptr && !this->name_wanted_.load()) {
          // Kept at once (named as the scan saw it, if it did), renamed once it answers.
          memcpy(this->name_addr_, peer, ESP_BD_ADDR_LEN);
          this->name_buf_[0] = '\0';
          for (const auto &device : this->pairing_->scanned_devices()) {
            if (memcmp(device.address, peer, ESP_BD_ADDR_LEN) == 0)
              strncpy(this->name_buf_, device.name.c_str(), sizeof(this->name_buf_) - 1);
          }
          this->keep_pending_ = true;
          this->name_wanted_.store(true);
          if (esp_bt_gap_read_remote_name(this->name_addr_) != ESP_OK)
            this->name_wanted_.store(false);
        }
      }
      this->pairing_->on_connection(this, true);
    }
    }
#endif
  }
#ifdef USE_BT_PAIRING
  if (this->pairing_ != nullptr && this->name_ready_.exchange(false)) {
    if (this->rmt_name_[0] != '\0') {
      memcpy(this->name_buf_, this->rmt_name_, sizeof(this->name_buf_));
      this->keep_pending_ = true;
    }
  }
  if (this->pairing_ != nullptr && this->keep_pending_ && this->pairing_->is_loaded()) {
    bt_pairing::PairingRecord record{};
    memcpy(record.address, this->name_addr_, sizeof(record.address));
    record.transport = static_cast<uint8_t>(bt_pairing::Transport::CLASSIC);
    record.slot = this->slot_;
    memcpy(record.name, this->name_buf_, sizeof(record.name));
    record.name[sizeof(record.name) - 1] = '\0';
    this->keep_pending_ = false;
    if (this->pairing_->store(record)) {
      ESP_LOGV(TAG, "Kept %s '%s' in the paired devices", this->source_.to_str(this->name_addr_), record.name);
      this->pairing_->on_connection(this, this->connected_.load());
    }
  }
#endif
  if (this->disconnect_pending_.exchange(false)) {
#ifdef USE_BT_PAIRING
    if (this->rejected_) {
      this->rejected_ = false;
    } else {
#endif
    ESP_LOGI(TAG, "Disconnected");
    for (auto *trigger : this->disconnected_triggers_) {
      trigger->trigger();
    }
#ifdef USE_BT_PAIRING
    if (this->pairing_ != nullptr) {
      this->reporting_disconnect_ = true;
      this->pairing_->on_connection(this, false);
      this->reporting_disconnect_ = false;
    }
    }
    if (this->switch_pending_) {
      this->switch_pending_ = false;
      esp_bd_addr_t addr;
      memcpy(addr, this->switch_addr_, ESP_BD_ADDR_LEN);
      this->source_.connect_to_device(addr);
    } else if (this->pairing_mode_.load() && this->chosen_valid_) {
      this->source_.resume_discovery();
    }
#endif
  }
  if (this->streaming_start_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Streaming");
#ifdef USE_A2DP_SOURCE_PAUSE_BLE_SCAN
    esp32_ble_tracker::global_esp32_ble_tracker->pause_scan();
#endif
    for (auto *trigger : this->streaming_start_triggers_) {
      trigger->trigger();
    }
  }
  if (this->streaming_stop_pending_.exchange(false)) {
    ESP_LOGI(TAG, "Streaming stopped");
#ifdef USE_A2DP_SOURCE_PAUSE_BLE_SCAN
    esp32_ble_tracker::global_esp32_ble_tracker->resume_scan();
#endif
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
  if (this->paging_) {
    ESP_LOGCONFIG(TAG, "  Reconnect: paging every %" PRIu32 " s", this->paging_interval_s_);
  } else {
    ESP_LOGCONFIG(TAG, "  Reconnect: listen");
  }
  ESP_LOGCONFIG(TAG, "  Device stored: %s", YESNO(this->has_stored_device()));
}

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32 || USE_A2DP_SOURCE_EXTERNAL_HOST
