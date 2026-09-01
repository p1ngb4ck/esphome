#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32

#include "esphome/components/microphone/microphone_source.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>

#include <atomic>
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

class A2DPSource : public Component {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::AFTER_CONNECTION; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_microphone_source(microphone::MicrophoneSource *mic_source) { this->mic_source_ = mic_source; }
  void set_local_name(const std::string &name) { this->local_name_ = name; }
  /// @brief Only accept a device whose name starts with this. Empty means the
  ///        strongest signal wins instead.
  void set_target_name(const std::string &name) { this->target_name_ = name; }
  void set_settle_time(uint32_t seconds) { this->settle_seconds_ = seconds; }
  void set_fallback_time(uint32_t seconds) { this->fallback_seconds_ = seconds; }
  void set_volume(uint8_t volume) { this->volume_ = volume; }
  void set_buffer_duration_ms(uint32_t ms) { this->buffer_duration_ms_ = ms; }
  void set_pair_on_boot_if_empty(bool enable) { this->pair_on_boot_if_empty_ = enable; }
  void set_device_select(A2DPDeviceSelect *select) { this->device_select_ = select; }

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
  /// @brief Pairs with one of the devices found in this window, by name.
  ///
  /// The address is remembered and taken the next time the inquiry reports it,
  /// which is the only moment the library will write it to NVS.
  /// @return false when no device of that name was seen.
  bool pair_with_name(const std::string &name);
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

  static const size_t MAX_DISCOVERED = 12;

 protected:
  static bool device_filter_(const char *name, esp_bd_addr_t address, int rssi);
  static int32_t audio_callback_(Frame *frames, int32_t frame_count);
  static void connection_state_(esp_a2d_connection_state_t state, void *self);
  static void audio_state_(esp_a2d_audio_state_t state, void *self);
  static void authentication_complete_(esp_bt_status_t status);

  int32_t fill_frames_(Frame *frames, int32_t frame_count);
  bool accept_device_(const char *name, esp_bd_addr_t address, int rssi);

  BluetoothA2DPSource source_;
  microphone::MicrophoneSource *mic_source_{nullptr};
  RingbufHandle_t ring_{nullptr};

  std::string local_name_{"ESPHome Audio"};
  std::string target_name_;
  uint32_t settle_seconds_{15};
  uint32_t fallback_seconds_{20};
  uint32_t buffer_duration_ms_{100};
  uint8_t volume_{60};
  bool pair_on_boot_if_empty_{true};

  std::atomic<bool> pairing_mode_{false};
  std::atomic<bool> connected_{false};
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

template<typename... Ts> class ForgetDeviceAction : public Action<Ts...>, public Parented<A2DPSource> {
 public:
  void play(Ts... x) override { this->parent_->forget_device(); }
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

#endif  // USE_ESP32_VARIANT_ESP32
