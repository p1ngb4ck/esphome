#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_audio/usb_audio.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include <atomic>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace usb_audio {

class USBAudioMicrophone : public microphone::Microphone,
                           public Component,
                           public Parented<USBAudioClient>,
                           public USBAudioMicrophoneListener {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;

  // Safe from any task; loop() acts on the request.
  void start() override;
  void stop() override;

  void on_stream_event(MicStreamEvent event) override;

  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_bits_per_sample(uint16_t bits) { this->bits_per_sample_ = bits; }
  void set_channels(uint8_t channels) { this->channels_ = channels; }
  void add_enabled_channel(uint8_t channel) { this->enabled_channels_.push_back(channel); }

 protected:
  static constexpr uint8_t COMMAND_START = 1 << 0;
  static constexpr uint8_t COMMAND_STOP = 1 << 1;
  // Same limit as i2s_audio's microphone.
  static constexpr uint8_t MAX_LISTENERS = 16;
  static constexpr uint32_t RETRY_MIN_MS = 1000;
  static constexpr uint32_t RETRY_MAX_MS = 30000;
  static constexpr uint32_t RETRY_START_ID = 0;
  static constexpr size_t READ_TIMEOUT_MS = 20;
  static constexpr size_t MIC_TASK_STACK_SIZE = 4096;
  static constexpr UBaseType_t MIC_TASK_PRIORITY = 18;

  static void mic_task(void *param);
  void mic_task_loop_();
  void request_(uint8_t set, uint8_t clear);
  void open_stream_();
  void set_running_();
  // Copy the first enabled channel over the disabled ones, in place.
  void apply_channel_selection_(uint8_t *data, size_t length);

  // Created once in setup() and parked between sessions.
  StaticTask mic_task_;
  // Filled by the mic task and handed to the data callbacks; resized only within its capacity.
  std::vector<uint8_t> read_buffer_;
  std::vector<uint8_t> enabled_channels_;
  std::atomic<uint8_t> commands_{0};
  // start() calls not yet matched by a stop(); capture stops when the last listener stops.
  std::atomic<uint8_t> listeners_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> task_idle_{true};
  // Main loop only.
  uint32_t retry_delay_ms_{RETRY_MIN_MS};

  uint32_t sample_rate_{16000};
  uint16_t bits_per_sample_{16};
  uint8_t channels_{1};
  // Channel selection, resolved in setup(): a bit per enabled channel, and the one copied.
  uint8_t enabled_mask_{0};
  uint8_t primary_channel_{0};
  bool logged_invalid_frame_{false};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
