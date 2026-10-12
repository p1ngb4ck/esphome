#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <atomic>
#include <string>
#include <vector>

namespace esphome::audio_router {

// A speaker that hands everything to one of several output speakers, the selected one. Players
// write here and never see the switch; the outputs must accept this speaker's format.
class AudioRouter : public speaker::Speaker, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void add_output(speaker::Speaker *output, const char *name);
  void set_preference_key(uint32_t key) { this->preference_key_ = key; }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  void set_pause_state(bool pause_state) override;
  bool get_pause_state() const override;
  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  /// Main loop. Takes effect once no play() is in flight; the new output continues where the
  /// old one was (running, paused, volume, mute).
  void select_output(size_t index);
  size_t active_output() const { return this->active_; }
  size_t output_count() const { return this->outputs_.size(); }
  const char *output_name(size_t index) const { return this->outputs_[index].name; }
  /// Called with the new index once a switch has happened.
  template<typename F> void add_on_output_callback(F &&callback) {
    this->output_callback_.add(std::forward<F>(callback));
  }

 protected:
  struct Output {
    speaker::Speaker *speaker;
    const char *name;
  };
  speaker::Speaker *current_() const { return this->outputs_[this->active_].speaker; }
  void switch_now_();

  std::vector<Output> outputs_;
  CallbackManager<void(size_t)> output_callback_;
  ESPPreferenceObject pref_;
  uint32_t preference_key_{0};
  std::atomic<size_t> active_{0};
  std::atomic<int> in_play_{0};
  std::atomic<bool> switching_{false};
  size_t pending_{0};
  bool paused_{false};
};

}  // namespace esphome::audio_router

#endif  // USE_ESP32
