#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "uac_types.h"

#include <atomic>
#include <vector>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
}

namespace esphome {
namespace usb_audio {

static constexpr uint32_t USB_AUDIO_DEFAULT_BUFFER_SIZE = 6400;

// ── Forward declarations ─────────────────────────────────────────────────────
class USBAudioMicrophone;
class USBAudioSpeaker;

// ─────────────────────────────────────────────────────────────────────────────
// USBAudioClient
//
// Native USB Audio Class 1.0 host driver built on USBClient.
// No uac_host library — implements the class protocol directly using
// USBClient's control_transfer() and stream_open_() / stream_close_().
//
// Supports simultaneous speaker (ISOC OUT) and microphone (ISOC IN) streams.
// Both streams route through on_isoc_packet() which dispatches by ep_addr.
//
// Thread model:
//   USB task   — fires isoc callbacks, reads/writes ring buffers
//   Main loop  — runs loop(), applies pending volume/mute, opens/closes streams
// ─────────────────────────────────────────────────────────────────────────────
class USBAudioClient : public usb_host::USBClient {
 public:
  USBAudioClient() : usb_host::USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  uint8_t get_interface_class() const override { return USB_CLASS_AUDIO; }

  // ── Configuration setters ─────────────────────────────────────────────────
  void set_microphone_buffer_size(uint32_t size) { this->mic_buf_size_ = size; }
  void set_speaker_buffer_size(uint32_t size) { this->spk_buf_size_ = size; }

  void set_microphone_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);
  void set_speaker_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);

  void set_microphone(USBAudioMicrophone *mic) { this->microphone_ = mic; }
  void set_speaker(USBAudioSpeaker *spk) { this->speaker_ = spk; }

  // ── Subcomponent API — called from USBAudioSpeaker / USBAudioMicrophone ────
  bool ensure_started_speaker();
  bool ensure_started_microphone();
  void resume_speaker();
  void suspend_speaker();
  void resume_microphone();
  void suspend_microphone();

  void set_speaker_volume_level(float volume);
  void set_speaker_mute_state(bool mute_state);

  // Speaker write — called from speaker task / play(); returns bytes accepted.
  // timeout_ms: FreeRTOS ring buffer block time (0 = non-blocking).
  esp_err_t write_speaker(const uint8_t *data, size_t length, uint32_t timeout_ms);

  // Microphone read — called from microphone task; blocks up to timeout_ms.
  esp_err_t read_microphone(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms);

  uint32_t get_speaker_buffer_size() const { return this->spk_buf_size_; }
  uint32_t get_microphone_buffer_size() const { return this->mic_buf_size_; }
  bool device_connected() const { return this->device_connected_; }

 protected:
  void on_connected() override;
  void on_disconnected() override;

  // USB-task context — dispatches by endpoint direction.
  void on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) override;

  // ── Descriptor parsing ────────────────────────────────────────────────────
  bool parse_descriptors_();
  bool find_ac_interface_();  // sets ac_intf_
  bool parse_feature_units_();
  bool parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits,
                            uint32_t sample_rate, uint8_t &intf_out, UacAltInfo &alt_out);

  // ── UAC control requests ──────────────────────────────────────────────────
  // Issue a class-specific control transfer and busy-wait for completion.
  // wIndex for interface requests: (unit_id << 8) | ac_intf_num
  // wIndex for endpoint requests:  ep_addr
  bool uac_set_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                               const uint8_t *data, size_t len);
  bool uac_get_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                               uint8_t *data, size_t len);
  bool uac_set_cur_endpoint_(uint8_t ep_addr, uint8_t selector,
                              const uint8_t *data, size_t len);
  bool set_sampling_frequency_(uint8_t ep_addr, uint32_t freq);
  bool apply_speaker_volume_();
  bool apply_speaker_mute_();

  // ── Stream open / close ───────────────────────────────────────────────────
  bool open_speaker_stream_();
  bool open_microphone_stream_();
  void close_speaker_stream_();
  void close_microphone_stream_();

  // ── Subcomponent pointers ────────────────────────────────────────────────
  USBAudioMicrophone *microphone_{nullptr};
  USBAudioSpeaker *speaker_{nullptr};

  // ── Audio format config (set from to_code / subcomponent setup) ──────────
  struct EndpointConfig {
    uint8_t  channels{0};
    uint16_t bits_per_sample{0};
    uint32_t sample_rate{0};
    bool configured{false};
  };
  EndpointConfig mic_cfg_{};
  EndpointConfig spk_cfg_{};

  // ── Descriptor parsing results ────────────────────────────────────────────
  uint8_t ac_intf_{0};             // AudioControl interface number
  UacFeatureUnit speaker_fu_{};    // Feature Unit serving the speaker terminal
  UacFeatureUnit mic_fu_{};        // Feature Unit serving the microphone terminal

  uint8_t spk_as_intf_{0};         // AudioStreaming interface for speaker
  UacAltInfo spk_alt_{};           // chosen alt-setting for speaker
  uint8_t mic_as_intf_{0};         // AudioStreaming interface for microphone
  UacAltInfo mic_alt_{};           // chosen alt-setting for microphone

  // ── Stream state ──────────────────────────────────────────────────────────
  usb_host::IsocStream spk_stream_{};
  usb_host::IsocStream mic_stream_{};
  bool spk_stream_open_{false};
  bool mic_stream_open_{false};

  // ── Ring buffers (SPSC: USB task ↔ application task) ─────────────────────
  RingbufHandle_t spk_rb_{nullptr};   // writer: speaker task; reader: USB task isoc OUT callback
  RingbufHandle_t mic_rb_{nullptr};   // writer: USB task isoc IN callback; reader: microphone task
  uint32_t spk_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};
  uint32_t mic_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};

  // ── Playback state ────────────────────────────────────────────────────────
  bool spk_suspended_{false};   // true when speaker is explicitly paused
  bool mic_suspended_{false};   // true when microphone is explicitly paused
  bool device_connected_{false};

  // ── Volume / mute state ───────────────────────────────────────────────────
  float   spk_volume_{1.0f};
  bool    spk_muted_{false};
  bool    pending_spk_volume_{true};
  bool    pending_spk_mute_{true};
  bool    spk_volume_supported_{true};
  bool    spk_mute_supported_{true};
  bool    spk_volume_warned_{false};
  bool    spk_mute_warned_{false};

  // ── Volume range (from GET_MIN / GET_MAX on connect) ─────────────────────
  int16_t spk_vol_min_{-0x7FFF};
  int16_t spk_vol_max_{0x0000};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
