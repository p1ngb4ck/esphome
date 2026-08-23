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

// Backoff for reopening a stream that keeps failing: first retry after this delay, doubling
// up to the maximum.
static constexpr uint32_t STREAM_REOPEN_BASE_MS = 1000;
static constexpr uint32_t STREAM_REOPEN_MAX_MS = 30000;

// UAC 1.0 Feature Unit volume is a signed 1/256 dB value. 0x8000 is reserved to mean
// silence and is never a range endpoint. When a device gives no usable range, span this
// much instead (-64 dB to 0 dB).
static constexpr int16_t UAC_VOLUME_SILENCE = static_cast<int16_t>(0x8000);
static constexpr int16_t UAC_VOLUME_FALLBACK_MIN = -64 * 256;

// -- Forward declarations -----------------------------------------------------
class USBAudioMicrophone;
class USBAudioSpeaker;

// -----------------------------------------------------------------------------
// USBAudioClient
//
// Native USB Audio Class 1.0 host driver built on USBClient.
// No uac_host library - implements the class protocol directly using
// USBClient's control_transfer() and stream_open_() / stream_close_().
//
// Supports simultaneous speaker (ISOC OUT) and microphone (ISOC IN) streams.
// Both streams route through on_isoc_packet() which dispatches by ep_addr.
//
// Thread model:
//   USB task   - fires isoc callbacks, reads/writes ring buffers
//   Main loop  - runs loop(), applies pending volume/mute, opens/closes streams
// -----------------------------------------------------------------------------
class USBAudioClient : public usb_host::USBClient {
 public:
  USBAudioClient() : usb_host::USBClient(0, 0) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  uint8_t get_interface_class() const { return USB_CLASS_AUDIO; }

  // -- Configuration setters -------------------------------------------------
  void set_microphone_buffer_size(uint32_t size) { this->mic_buf_size_ = size; }
  void set_speaker_buffer_size(uint32_t size) { this->spk_buf_size_ = size; }

  void set_microphone_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);
  void set_speaker_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);
  // Use an asynchronous OUT endpoint's feedback stream to pace playback (default on).
  void set_feedback_enabled(bool enabled) { this->feedback_enabled_ = enabled; }

  void set_microphone(USBAudioMicrophone *mic) { this->microphone_ = mic; }
  void set_speaker(USBAudioSpeaker *spk) { this->speaker_ = spk; }

  // -- Subcomponent API - called from USBAudioSpeaker / USBAudioMicrophone ----
  bool ensure_started_speaker();
  bool ensure_started_microphone();
  void resume_speaker();
  void suspend_speaker();
  void resume_microphone();
  void suspend_microphone();

  void set_speaker_volume_level(float volume);
  void set_speaker_mute_state(bool mute_state);
  // The last requested values. A request is held while the stream is closed, so these can
  // be ahead of what the device has been told.
  float get_speaker_volume_level() const { return this->spk_volume_; }
  bool get_speaker_mute_state() const { return this->spk_muted_; }

  // Speaker write - called from speaker task / play(); returns bytes accepted.
  // timeout_ms: FreeRTOS ring buffer block time (0 = non-blocking).
  esp_err_t write_speaker(const uint8_t *data, size_t length, uint32_t timeout_ms);

  // Microphone read - called from microphone task; blocks up to timeout_ms.
  esp_err_t read_microphone(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms);

  // Bytes the isochronous OUT sample clock moves per service interval, i.e. the granularity
  // the sink consumes at. 0 while no speaker stream is open. Writers align their chunks to
  // this so no partial packet is queued.
  uint32_t get_speaker_packet_bytes() const {
    return this->spk_stream_open_ ? this->spk_stream_.packet_size : 0;
  }

  // Bytes still queued for the speaker. Exact, unlike inferring it from the time of the last
  // write, which cannot tell a paused stream from a drained one.
  uint32_t get_speaker_queued_bytes() const;

  uint32_t get_speaker_buffer_size() const { return this->spk_buf_size_; }
  uint32_t get_microphone_buffer_size() const { return this->mic_buf_size_; }
  bool device_connected() const { return this->device_connected_; }

 protected:
  void on_connected() override;
  void on_disconnected() override;

  // USB-task context - dispatches by endpoint direction.
  void on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) override;

  // -- Descriptor parsing ----------------------------------------------------
  bool parse_descriptors_();
  bool find_ac_interface_();  // sets ac_intf_
  // Record the units and terminals of the AudioControl interface. Returns the number
  // written to nodes, which holds at most UAC_MAX_TOPOLOGY_NODES entries.
  uint8_t collect_topology_(UacTopologyNode *nodes);
  // Record every Feature Unit of the AudioControl interface, at most max_count.
  uint8_t collect_feature_units_(UacFeatureUnit *units, uint8_t max_count);
  bool parse_feature_units_();
  bool parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits,
                            uint32_t sample_rate, uint8_t &intf_out, UacAltInfo &alt_out);

  // -- UAC control requests --------------------------------------------------
  // Issue a class-specific control transfer and wait for it to finish, up to
  // CTRL_TIMEOUT_MS. The completion callback runs on the USB task and can fire after the
  // wait has been given up, so the result is not kept on the caller's stack.
  // Pass out_data for a request with an outgoing data stage, in_data to keep an incoming
  // one; both are limited to UAC_CTRL_MAX_DATA_LEN bytes.
  bool uac_control_transfer_(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                             const uint8_t *out_data, size_t out_len, uint8_t *in_data, size_t in_len);
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
  // Fill channels with the Feature Unit channel numbers a speaker volume request has to be
  // written to, and return how many. channels must hold UAC_FU_MAX_CHANNELS entries.
  uint8_t speaker_volume_channels_(uint8_t *channels) const;
  // Read the volume range the device reports, so a fraction can be mapped onto it.
  void probe_speaker_volume_range_();
  bool apply_speaker_volume_();
  bool apply_speaker_mute_();

  // -- Stream open / close ---------------------------------------------------
  bool open_speaker_stream_();
  bool open_microphone_stream_();
  void close_speaker_stream_();
  void close_microphone_stream_();
  // Drop whatever is still queued so a reopened stream does not start by playing audio from
  // the previous session.
  void flush_speaker_buffer_();
  // Reopening a stream that keeps failing must not be retried on every play() call. Returns
  // true when the next attempt is due, and records the outcome so the delay can grow.
  bool reopen_due_(const uint32_t &next_attempt_ms, uint8_t fail_count) const;
  void note_open_result_(bool ok, uint32_t &next_attempt_ms, uint8_t &fail_count);
  // Report a condition that leaves the component unusable until the device changes.
  void set_stream_error_(const char *what);

  // -- Subcomponent pointers ------------------------------------------------
  USBAudioMicrophone *microphone_{nullptr};
  USBAudioSpeaker *speaker_{nullptr};

  // -- Audio format config (set from to_code / subcomponent setup) ----------
  struct EndpointConfig {
    uint8_t  channels{0};
    uint16_t bits_per_sample{0};
    uint32_t sample_rate{0};
    bool configured{false};
  };
  EndpointConfig mic_cfg_{};
  EndpointConfig spk_cfg_{};
  // True when the device enumerated at high speed (only the ESP32-P4 host can). Gates the
  // isochronous high-bandwidth multiplier and selects the OUT pacing service interval.
  bool device_is_high_speed_{false};

  // -- Descriptor parsing results --------------------------------------------
  uint8_t ac_intf_{0};             // AudioControl interface number
  UacFeatureUnit speaker_fu_{};    // Feature Unit serving the speaker terminal
  UacFeatureUnit mic_fu_{};        // Feature Unit serving the microphone terminal

  uint8_t spk_as_intf_{0};         // AudioStreaming interface for speaker
  UacAltInfo spk_alt_{};           // chosen alt-setting for speaker
  uint8_t mic_as_intf_{0};         // AudioStreaming interface for microphone
  UacAltInfo mic_alt_{};           // chosen alt-setting for microphone

  // -- Stream state ----------------------------------------------------------
  usb_host::IsocStream spk_stream_{};
  usb_host::IsocStream mic_stream_{};
  usb_host::IsocStream spk_fb_stream_{};  // async OUT feedback IN stream (shares spk interface)
  bool spk_stream_open_{false};
  bool spk_fb_open_{false};
  bool feedback_enabled_{true};
  bool mic_stream_open_{false};

  // -- Stream open backoff ---------------------------------------------------
  // A device that enumerates but refuses to stream would otherwise be retried on every
  // play() call. Delay grows from 1 s to STREAM_REOPEN_MAX_MS and resets on success or on
  // reconnect.
  uint32_t spk_reopen_at_ms_{0};
  uint32_t mic_reopen_at_ms_{0};
  uint8_t  spk_open_fails_{0};
  uint8_t  mic_open_fails_{0};
  bool     spk_format_ok_{false};  // a usable speaker alt-setting was found on this device
  bool     mic_format_ok_{false};

  // -- Ring buffers (SPSC: USB task <-> application task) ---------------------
  RingbufHandle_t spk_rb_{nullptr};   // writer: speaker task; reader: USB task isoc OUT callback
  RingbufHandle_t mic_rb_{nullptr};   // writer: USB task isoc IN callback; reader: microphone task
  uint32_t spk_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};
  uint32_t mic_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};

  // -- Playback state --------------------------------------------------------
  bool spk_suspended_{false};   // true when speaker is explicitly paused
  bool mic_suspended_{false};   // true when microphone is explicitly paused
  bool device_connected_{false};

  // -- Volume / mute state ---------------------------------------------------
  float   spk_volume_{1.0f};
  bool    spk_muted_{false};

  // -- Volume range (from GET_MIN / GET_MAX on connect) ---------------------
  int16_t spk_vol_min_{-0x7FFF};
  int16_t spk_vol_max_{0x0000};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
