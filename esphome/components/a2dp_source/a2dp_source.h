#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32) || defined(USE_A2DP_SOURCE_EXTERNAL_HOST)

#ifdef USE_MICROPHONE
#include "esphome/components/microphone/microphone_source.h"
#endif
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#ifdef USE_A2DP_SOURCE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BT_PAIRING
#include "esphome/components/bt_pairing/bt_pairing.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "BluetoothA2DPSource.h"

namespace esphome {
namespace a2dp_source {

class A2DPDeviceSelect;

struct DiscoveredDevice {
  std::string name;
  esp_bd_addr_t address;
  int rssi;
};

class A2DPSource : public Component
#ifdef USE_BT_PAIRING
    ,
                   public bt_pairing::PairingTarget
#endif
{
 public:
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_CONNECTION; }

  void setup() override;
  void loop() override;
  void dump_config() override;

#ifdef USE_MICROPHONE
  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
#endif
  void set_local_name(const std::string &name) { this->local_name_ = name; }
  /// @brief Only accept a device whose name starts with this. Empty means the
  ///        strongest signal wins instead.
  void set_target_name(const std::string &name) { this->target_name_ = name; }
  void set_pin_code(const std::string &pin_code) { this->pin_code_ = pin_code; }
  void set_settle_time(uint32_t seconds) { this->settle_seconds_ = seconds; }
  void set_fallback_time(uint32_t seconds) { this->fallback_seconds_ = seconds; }
  void set_volume(uint8_t volume) { this->volume_ = volume; }
  void set_buffer_duration_ms(uint32_t ms) { this->buffer_duration_ms_ = ms; }
  void set_idle_timeout_ms(uint32_t ms) { this->idle_timeout_ms_ = ms; }
  void set_pair_on_boot_if_empty(bool enable) { this->pair_on_boot_if_empty_ = enable; }
  /// @brief Not connected, outside a pairing window: page the stored device every interval (true),
  ///        or wait for it to connect on its own (false). It is paged once at boot either way.
  void set_paging(bool enabled) {
    this->paging_ = enabled;
    this->source_.set_paging(enabled);
  }
  bool is_paging() const { return this->paging_; }
  void set_paging_interval(uint32_t seconds) {
    this->paging_interval_s_ = seconds;
    this->source_.set_paging_interval_ms(seconds * 1000);
  }
  uint32_t get_paging_interval() const { return this->paging_interval_s_; }
  /// @brief No paging while paused (e.g. while something plays).
  void set_reconnect_paused(bool paused) { this->source_.set_reconnect_paused(paused); }
  void set_device_select(A2DPDeviceSelect *select) { this->device_select_ = select; }
#ifdef USE_A2DP_SOURCE_NUMBER
  void set_delay_number(number::Number *number) { this->delay_number_ = number; }
#endif

  /// @brief How long the connected device takes to play what it receives (ms), as set for it; 0
  ///        while none is connected. Kept per device.
  uint32_t output_delay_ms() const {
    return this->connected_.load() ? this->delay_ms_.load(std::memory_order_relaxed) : 0;
  }
  /// @brief Sets and keeps the delay of the connected device.
  void set_output_delay_ms(uint32_t ms);
  static constexpr uint32_t MAX_OUTPUT_DELAY_MS = 1600;
  /// @brief The most a delay may be set to (what its user has room for, e.g. a video player's sound
  ///        lead), at most MAX_OUTPUT_DELAY_MS. A larger kept delay stays kept; its user clamps it.
  void set_max_output_delay_ms(uint32_t ms);
  uint32_t max_output_delay_ms() const { return this->max_delay_ms_.load(std::memory_order_relaxed); }

  void add_on_paired_trigger(Trigger<std::string> *trigger) { this->paired_triggers_.push_back(trigger); }
  void add_on_streaming_start_trigger(Trigger<> *trigger) { this->streaming_start_triggers_.push_back(trigger); }
  void add_on_streaming_stop_trigger(Trigger<> *trigger) { this->streaming_stop_triggers_.push_back(trigger); }
  void add_on_connected_trigger(Trigger<> *trigger) { this->connected_triggers_.push_back(trigger); }
  void add_on_disconnected_trigger(Trigger<> *trigger) { this->disconnected_triggers_.push_back(trigger); }

  /// @brief Opens the pairing window. Every audio device that answers is logged;
  ///        the one that is picked gets written to NVS and used from then on.
  void start_pairing();
  /// @brief Drops the stored device. The next pairing window picks a new one.
  void forget_device();
  /// @brief Closes a running pairing window; the stored device stays.
  void stop_pairing();
  /// @brief Pairs with one of the devices found in this window, by name.
  ///
  /// The address is remembered and taken the next time the inquiry reports it,
  /// which is the only moment the library will write it to NVS.
  /// @return false when no device of that name was seen.
  bool pair_with_name(const std::string &name);
  /// @brief Pairs with the device at this place in discovered_devices() (the select).
  bool pair_with_index(size_t index);
#ifdef USE_BT_PAIRING
  void set_pairing(bt_pairing::BTPairing *pairing) { this->pairing_ = pairing; }
  void set_slot(uint32_t slot) { this->slot_ = slot; }
  bool pairs(bt_pairing::Transport transport, bt_pairing::DeviceKind kind) const override {
    return transport == bt_pairing::Transport::CLASSIC && kind == bt_pairing::DeviceKind::AUDIO;
  }
  uint32_t pairing_slot() const override { return this->slot_; }
  /// The device picked replaces the stored one.
  bool is_free() const override { return true; }
  bool pair_device(const bt_pairing::ScannedDevice &device) override;
  bool is_device_connected() const override { return this->connected_.load(); }
  void connect_device() override { this->source_.reconnect(); }
  void disconnect_device() override { this->source_.disconnect(); }
  /// Several audio devices stay paired; one is connected at a time.
  bool holds_many() const override { return true; }
  const uint8_t *connected_address() const override {
    return this->connected_.load() || this->reporting_disconnect_ ? this->connected_addr_ : nullptr;
  }
  bool is_address_connected(const uint8_t *address) const override {
    return this->connected_.load() && memcmp(this->connected_addr_, address, ESP_BD_ADDR_LEN) == 0;
  }
  void connect_address(const uint8_t *address) override;
  void disconnect_address(const uint8_t *address) override;
  void cancel_device_pairing() override {
    this->picked_valid_ = false;
    this->stop_pairing();
    this->restore_stored_();
  }
#endif
  /// @brief Devices seen since the pairing window opened.
  const std::vector<DiscoveredDevice> &discovered_devices() const { return this->devices_; }
  /// @brief Upper bound on that list, so the select can size its storage once.
  static size_t max_discovered() { return MAX_DISCOVERED; }
  bool is_pairing() const { return this->pairing_mode_.load(); }
  bool is_connected() const { return this->connected_.load(); }
  /// @brief Whether the sink is taking audio. A connection can stand without it.
  bool is_streaming() const { return this->streaming_.load(); }
  /// @brief Whether a device address is stored, which is what survives a reboot.
  bool has_stored_device() const;

  /// @brief Speaker platform: 16 bit stereo 44.1 kHz into the stream. Returns the bytes taken; while
  ///        no sink is connected everything is taken and dropped.
  size_t write_audio(const uint8_t *data, size_t length, TickType_t ticks_to_wait);
  void clear_audio();
  bool has_audio() const;
  /// @brief A speaker that becomes active starts the stream only once the buffer is full.
  void set_speaker_active(bool active) {
    if (!active) {
      this->speaker_active_.store(false);
    } else if (!this->speaker_active_.exchange(true)) {
      this->priming_.store(true);
    }
    this->speaker_finishing_.store(false);
  }
  /// @brief The speaker's input ended: what is buffered plays out, however little.
  void set_speaker_finishing() { this->speaker_finishing_.store(true); }
  /// @brief 0..127; sent to the sink as absolute volume when connected.
  void set_output_volume(uint8_t volume) { this->source_.set_volume(volume); }
  /// @brief Speaker frames handed to Bluetooth since the last call.
  uint32_t take_played_frames() { return this->played_frames_.exchange(0); }

  static const size_t MAX_DISCOVERED = 12;

 protected:
  static bool device_filter_(const char *name, esp_bd_addr_t address, int rssi);
  static int32_t audio_callback_(Frame *frames, int32_t frame_count);
  static void connection_state_(esp_a2d_connection_state_t state, void *self);
  static void audio_state_(esp_a2d_audio_state_t state, void *self);
  static void authentication_complete_(esp_bt_status_t status);
  static bool discovery_gate_();
  static bool gap_fallback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg);
  static bool gap_handler_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg);

  int32_t fill_frames_(Frame *frames, int32_t frame_count);
  bool accept_device_(const char *name, esp_bd_addr_t address, int rssi);

  BluetoothA2DPSource source_;
#ifdef USE_MICROPHONE
  microphone::MicrophoneSource *mic_source_{nullptr};
#endif
#ifdef USE_BT_PAIRING
  bt_pairing::BTPairing *pairing_{nullptr};
  bt_pairing::ScannedDevice picked_{};  // stored in bt_pairing once it connects
  uint32_t slot_{0};
  bool picked_valid_{false};
  // The stored device connected without a record in bt_pairing: kept with the name it reports
  // (asked for with Read Remote Name; the answer comes on Bluedroid's task).
  uint8_t name_addr_[ESP_BD_ADDR_LEN]{};
  char name_buf_[sizeof(bt_pairing::PairingRecord::name)]{};   // main loop
  char rmt_name_[sizeof(bt_pairing::PairingRecord::name)]{};   // Bluedroid's task, then name_ready_
  std::atomic<bool> name_wanted_{false};
  std::atomic<bool> name_ready_{false};
  bool keep_pending_{false};  // stored once bt_pairing has its database
  uint8_t switch_addr_[ESP_BD_ADDR_LEN]{};  // connected to once the one connected now is gone
  bool switch_pending_{false};
  bool rejected_{false};  // a device not paired was sent away: its disconnect is not reported
  uint8_t restore_addr_[ESP_BD_ADDR_LEN]{};  // connected before a pairing; back to it if that fails
  bool restore_valid_{false};
  bool restore_connect_{false};  // it was connected: connected to again
  bool reporting_disconnect_{false};  // connected_address() still names the device that went
  void restore_stored_();
  void check_pick_failed_(uint8_t conn_state);
#endif
  uint8_t connected_addr_[ESP_BD_ADDR_LEN]{};  // main loop
  RingbufHandle_t ring_{nullptr};
  size_t ring_bytes_{0};
  std::atomic<bool> speaker_active_{false};
  std::atomic<bool> speaker_finishing_{false};
  std::atomic<bool> priming_{false};  // silence until the buffer is full (the speaker started)
  std::atomic<uint32_t> played_frames_{0};

  std::string local_name_{"ESPHome Audio"};
  std::string target_name_;
  std::string pin_code_;
  uint32_t settle_seconds_{15};
  uint32_t fallback_seconds_{20};
  uint32_t buffer_duration_ms_{100};
  uint8_t volume_{60};
  bool pair_on_boot_if_empty_{true};
  bool paging_{false};
  uint32_t paging_interval_s_{30};

  std::atomic<bool> pairing_mode_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint32_t> delay_ms_{0};
  std::atomic<uint32_t> max_delay_ms_{MAX_OUTPUT_DELAY_MS};
  ESPPreferenceObject delay_pref_;
  bool delay_pref_valid_{false};
#ifdef USE_A2DP_SOURCE_NUMBER
  number::Number *delay_number_{nullptr};
#endif
  void load_output_delay_();
  void publish_output_delay_();
  std::atomic<bool> streaming_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> paired_pending_{false};
  std::atomic<bool> connect_pending_{false};
  std::atomic<bool> disconnect_pending_{false};
  std::atomic<bool> streaming_start_pending_{false};
  std::atomic<bool> streaming_stop_pending_{false};
  std::atomic<uint8_t> pending_audio_state_{0xFF};
  std::atomic<uint8_t> pending_connection_state_{0xFF};

  void publish_device_list_();
  void update_media_();

  A2DPDeviceSelect *device_select_{nullptr};
  std::vector<DiscoveredDevice> devices_;
  esp_bd_addr_t chosen_addr_{};
  bool chosen_valid_{false};
  std::atomic<bool> devices_changed_{false};

  uint32_t settle_until_ms_{0};
  uint32_t fallback_after_ms_{0};
  char best_name_[64]{};
  esp_bd_addr_t best_addr_{};
  int best_rssi_{-128};
  bool best_valid_{false};
  std::string paired_name_;

  std::atomic<uint32_t> underrun_frames_{0};
  uint32_t last_report_ms_{0};
  uint32_t idle_timeout_ms_{0};
  uint32_t idle_since_ms_{0};
  bool media_wanted_{true};

  std::vector<Trigger<std::string> *> paired_triggers_;
  std::vector<Trigger<> *> connected_triggers_;
  std::vector<Trigger<> *> disconnected_triggers_;
  std::vector<Trigger<> *> streaming_start_triggers_;
  std::vector<Trigger<> *> streaming_stop_triggers_;
};

template<typename... Ts> class StartPairingAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->start_pairing(); }
};

template<typename... Ts> class StopPairingAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->stop_pairing(); }
};

template<typename... Ts> class ForgetDeviceAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->forget_device(); }
};

template<typename... Ts> class PauseReconnectAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->set_reconnect_paused(true); }
};

template<typename... Ts> class ResumeReconnectAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->set_reconnect_paused(false); }
};

template<typename... Ts> class PairWithNameAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  TEMPLATABLE_VALUE(std::string, name)
  void play(Ts... x) override { this->parent_->pair_with_name(this->name_.value(x...)); }
};

template<typename... Ts> class IsConnectedCondition : public Condition<Ts...>, public Parented<A2DPSource> {
 public:
  bool check(Ts... x) override { return this->parent_->is_connected(); }
};

template<typename... Ts> class IsStreamingCondition : public Condition<Ts...>, public Parented<A2DPSource> {
 public:
  bool check(Ts... x) override { return this->parent_->is_streaming(); }
};

template<typename... Ts> class IsPairedCondition : public Condition<Ts...>, public Parented<A2DPSource> {
 public:
  bool check(Ts... x) override { return this->parent_->has_stored_device(); }
};

}  // namespace a2dp_source
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32 || USE_A2DP_SOURCE_EXTERNAL_HOST
