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
// silence and is never a range endpoint.
static constexpr int16_t UAC_VOLUME_SILENCE = static_cast<int16_t>(0x8000);
// Smallest gain the class can express; 0x8000 below it is the silence code, not a value.
static constexpr int16_t UAC_VOLUME_MIN_GAIN = static_cast<int16_t>(0x8001);
// A device whose reported scale tops out at or below this is not describing a scale it
// plays on. The Linux USB audio mixer draws the same line at -96 dB.
static constexpr int16_t UAC_VOLUME_BOGUS_MAX = static_cast<int16_t>(-96 * 256);

// How a 0..1 setting is turned into a position on the device's volume scale.
//
// LINEAR treats the setting as a position on the scale itself: 50 percent is the middle of
// what the device reports between MIN and MAX. That is what the class definition describes
// and what the reference implementations do, and on a device with a narrow scale it is
// what feels right.
//
// LOGARITHMIC treats the setting as an amplitude and converts it: 50 percent is 6.02 dB
// below full, a quarter is 12.04 dB below, and so on, clamped into the device's scale. On a
// device whose scale runs down to -100 dB or further this is the more predictable of the
// two, because a linear position spends most of its travel in a range that is already
// inaudible.
enum class VolumeCurve : uint8_t {
  LINEAR = 0,
  LOGARITHMIC = 1,
};

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
  void set_volume_curve(VolumeCurve curve) { this->volume_curve_ = curve; }

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
  float get_speaker_volume_level() const { return this->spk_ctl_.volume; }
  bool get_speaker_mute_state() const { return this->spk_ctl_.muted; }

  // The capture side of the same thing. A device that describes a Feature Unit in its
  // capture path takes these the same way the playback path takes the two above.
  void set_microphone_volume_level(float volume);
  void set_microphone_mute_state(bool mute_state);
  float get_microphone_volume_level() const { return this->mic_ctl_.volume; }
  bool get_microphone_mute_state() const { return this->mic_ctl_.muted; }

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
  // Tell the device which rate the stream runs at. UAC 1.0 puts that control on the
  // isochronous endpoint, UAC 2.0 on the Clock Source entity behind the terminal.
  bool set_sample_rate_(const UacAltInfo &alt, const UacControlState &ctl, uint32_t freq);
  // Find the Clock Source entity that drives a terminal, or 0 when there is none.
  uint8_t find_clock_source_(uint8_t terminal_id);
  // Read the class-specific AudioControl header of the first audio function and record
  // which version of the class the device speaks.
  void detect_uac_version_();
  // Read the volume range the device reports, so a fraction can be mapped onto it.
  void probe_volume_range_(UacControlState &ctl, const char *what);
  bool apply_volume_(UacControlState &ctl, const char *what);
  bool apply_mute_(UacControlState &ctl, const char *what);
  void set_volume_level_(UacControlState &ctl, const char *what, float volume);
  void set_mute_state_(UacControlState &ctl, const char *what, bool mute_state);

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
  // The task that runs the component loop, captured in setup().
  TaskHandle_t loop_task_{nullptr};
  uint8_t ac_intf_{0};             // AudioControl interface number
  // Which version of the audio class the attached device speaks, from bcdADC.
  uint16_t uac_version_{UAC_VERSION_1};
  // Feature Unit, volume and mute of each direction.
  UacControlState spk_ctl_{};
  UacControlState mic_ctl_{};

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

  // -- Volume mapping --------------------------------------------------------
  VolumeCurve volume_curve_{VolumeCurve::LINEAR};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
