#include "a2dp_source.h"

#ifdef USE_ESP32_VARIANT_ESP32

#include "esphome/core/log.h"

#include <nvs.h>

#include <cstring>

namespace esphome {
namespace a2dp_source {

static const char *const TAG = "a2dp_source";

// A2DP carries SBC and SBC is 44100 Hz, 16 bit, stereo. The frame the library
// hands out is two int16 samples.
static const uint32_t SAMPLE_RATE = 44100;
static const size_t BYTES_PER_FRAME = 4;

// The library stores the peer address in this namespace, under the key its
// last_bda_nvs_name() returns for the source role. Forgetting a device writes
// six zero bytes rather than erasing the entry, so an all-zero address is what
// "nothing stored" looks like.
static const char *const NVS_NAMESPACE = "connected_bda";
static const char *const NVS_KEY = "src_bda";

// The library's callbacks are plain function pointers with no user argument, so
// the component has to be reachable from a file-scope pointer. One A2DP source
// per device is the only sensible configuration anyway -- there is one radio.
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
  bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  bool was = component->connected_.exchange(connected);
  if (connected && !was) {
    component->connect_pending_.store(true);
  } else if (!connected && was) {
    component->disconnect_pending_.store(true);
  }
}

void A2DPSource::setup() {
  global_a2dp_source = this;

  size_t ring_bytes = (size_t) SAMPLE_RATE * BYTES_PER_FRAME * this->buffer_duration_ms_ / 1000;
  this->ring_ = xRingbufferCreate(ring_bytes, RINGBUF_TYPE_BYTEBUF);
  if (this->ring_ == nullptr) {
    ESP_LOGE(TAG, "could not allocate the %u byte buffer", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }

  if (this->mic_source_ != nullptr) {
    this->mic_source_->add_data_callback([this](const std::vector<uint8_t> &data) {
      if (data.empty()) {
        return;
      }
      // Never block here: this runs on whatever task the microphone reads from.
      // If Bluetooth is not draining, the newest audio is worth more than the
      // oldest, so the oldest goes.
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

  // Always on, in both modes, and this is not cosmetic: the library only writes
  // the peer address to NVS when reconnect_status is not NoReconnect. Leaving it
  // off means a successful pairing is forgotten on reset.
  this->source_.set_auto_reconnect(true);

  bool stored = this->has_stored_device();
  if (!stored && this->pair_on_boot_if_empty_) {
    ESP_LOGI(TAG, "no device stored, opening the pairing window");
    this->start_pairing();
  } else if (stored) {
    ESP_LOGI(TAG, "connecting to the stored device");
  } else {
    ESP_LOGI(TAG, "no device stored; call a2dp_source.start_pairing to add one");
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
  this->best_valid_ = false;
  this->best_rssi_ = -128;
  this->best_name_[0] = '\0';
  this->settle_until_ms_ = now + this->settle_seconds_ * 1000;
  this->fallback_after_ms_ = this->settle_until_ms_ + this->fallback_seconds_ * 1000;
  this->pairing_mode_.store(true);

  ESP_LOGI(TAG, "pairing window open");
  ESP_LOGI(TAG, "put the speaker or receiver into pairing mode now -- on an AV");
  ESP_LOGI(TAG, "receiver that is a menu entry, it is not discoverable otherwise");
}

void A2DPSource::forget_device() {
  ESP_LOGI(TAG, "forgetting the stored device");
  this->source_.clean_last_connection();
}

bool A2DPSource::accept_device_(const char *name, esp_bd_addr_t address, int rssi) {
  if (!this->pairing_mode_.load()) {
    // Outside the pairing window nothing is accepted. The stored address is what
    // counts, and a passing device with a similar name must not displace it.
    return false;
  }

  ESP_LOGI(TAG, "  found %-24s %02x:%02x:%02x:%02x:%02x:%02x  %d dBm", name, address[0], address[1], address[2],
           address[3], address[4], address[5], rssi);

  if (!this->target_name_.empty()) {
    if (strncmp(name, this->target_name_.c_str(), this->target_name_.size()) != 0) {
      return false;
    }
    ESP_LOGI(TAG, "  -> matches target_name");
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }

  uint32_t now = millis();

  // First stage: collect. Taking whichever device answers first would be a race
  // with the order the inquiry happens to report them in, and that order says
  // nothing about which one is meant.
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

  // Second stage: wait for the winner to come round again. The inquiry reports
  // each device repeatedly, so it will. Accepting has to happen from inside this
  // callback because that is the only place the library writes NVS.
  if (this->best_valid_ && memcmp(address, this->best_addr_, ESP_BD_ADDR_LEN) == 0) {
    ESP_LOGI(TAG, "  -> strongest of the window at %d dBm", this->best_rssi_);
    this->paired_name_ = name;
    this->pairing_mode_.store(false);
    this->paired_pending_.store(true);
    return true;
  }

  // If it never comes back, take what is still answering rather than stall.
  if (now > this->fallback_after_ms_) {
    ESP_LOGW(TAG, "  -> %s stopped answering, taking this one instead",
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
    // Short reads are filled with silence rather than reported: returning fewer
    // frames than asked stalls the stream, and a gap is worse than quiet.
    memset(out + filled, 0, wanted - filled);
    this->underrun_frames_.fetch_add((uint32_t) ((wanted - filled) / BYTES_PER_FRAME));
  }
  return frame_count;
}

void A2DPSource::loop() {
  if (this->paired_pending_.exchange(false)) {
    ESP_LOGI(TAG, "paired with %s", this->paired_name_.c_str());
    for (auto *trigger : this->paired_triggers_) {
      trigger->trigger(this->paired_name_);
    }
  }
  if (this->connect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "connected");
    for (auto *trigger : this->connected_triggers_) {
      trigger->trigger();
    }
  }
  if (this->disconnect_pending_.exchange(false)) {
    ESP_LOGI(TAG, "disconnected");
    for (auto *trigger : this->disconnected_triggers_) {
      trigger->trigger();
    }
  }

  uint32_t now = millis();
  if (now - this->last_report_ms_ >= 10000) {
    this->last_report_ms_ = now;
    uint32_t frames = this->underrun_frames_.exchange(0);
    if (frames > 0) {
      // A steady trickle means the sender's rate is off; bursts mean something
      // stalled. Either way it is audible, so it is a warning and not a debug
      // line.
      ESP_LOGW(TAG, "inserted %" PRIu32 " frames of silence in the last 10 s (%" PRIu32 " ms)", frames,
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
  ESP_LOGCONFIG(TAG, "  Buffer: %" PRIu32 " ms", this->buffer_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Device stored: %s", YESNO(this->has_stored_device()));
}

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32
