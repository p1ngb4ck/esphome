#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32H4) || defined(USE_ESP32_VARIANT_ESP32S31)

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"
#include "uac_types.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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

// How a volume value is put on the wire.
//
// DB is the class definition: a signed 16-bit value in 1/256 dB, little-endian, inside the
// range the device reports through MIN, MAX and RES.
//
// BYTE is for devices that do not follow it and take the setting as a plain 0..255 level in
// the low byte, with 0x80 in the high byte. Those devices cannot be told apart from a
// conforming one by asking them -- Linux carries the equivalent per device in its quirk
// table rather than detecting it -- so this is selected per VID and PID from the
// configuration.
enum class VolumeEncoding : uint8_t {
  DB = 0,
  BYTE = 1,
};

class USBAudioDevice;

// A class request sequence, run on the control task.
enum class CtrlOp : uint8_t {
  // After a connect: UAC 2.0 sample rates, volume ranges, volume and mute of both directions.
  SETUP,
  // Volume or mute changed; sends the pending ones.
  CONTROLS,
  // UAC 1.0 sample rate, once the stream's alt-setting is active.
  SPK_RATE,
  MIC_RATE,
};

struct CtrlRequest {
  CtrlOp op{CtrlOp::SETUP};
  USBAudioDevice *device{nullptr};
  // Connection the request belongs to; the control task drops it once that is gone.
  uint32_t gen{0};
  // Reported back when done, so the main loop acts only on the answer to its latest request.
  uint32_t seq{0};
};

// One configured device quirk, matched against the device descriptor on connection.
struct UsbAudioQuirk {
  uint16_t vid;
  uint16_t pid;
  VolumeEncoding volume_encoding;
};

class USBAudioClient;

// What the client tells the microphone about its stream, on the main loop.
enum class MicStreamEvent : uint8_t {
  OPEN,
  // The open stream closed: the device went away or the stream died. It reopens once the
  // device is back, as long as the microphone has not been suspended.
  LOST,
  // An open was refused; the microphone retries with a backoff.
  FAILED,
};

struct EndpointConfig {
  uint8_t channels{0};
  uint16_t bits_per_sample{0};
  uint32_t sample_rate{0};
  bool configured{false};
};

using SpeakerRoute = std::function<void(speaker::Speaker *sink, const char *name, uint32_t key, uint32_t sample_rate,
                                         uint8_t bits_per_sample, uint8_t channels, bool added)>;
using MicrophoneRoute = std::function<void(microphone::Microphone *mic, const char *name, uint32_t key, bool added)>;

class USBAudioSink : public speaker::Speaker {
 public:
  explicit USBAudioSink(USBAudioClient *client) : client_(client) {}

  void start() override;
  void stop() override;
  void finish() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override;
  bool has_buffered_data() const override;

  // Pause keeps the isochronous stream running and feeds it silence, so the device's clock
  // never stops and playback resumes without renegotiating the interface.
  void set_pause_state(bool pause_state) override;
  bool get_pause_state() const override { return this->paused_.load(); }

  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  void attach(USBAudioDevice *device, ring_buffer::RingBuffer *ring);
  void detach();
  USBAudioDevice *device() const { return this->device_.load(); }
  bool idle() const { return this->writers_.load() == 0 && !this->active_.load(); }
  bool playing() const {
    return this->active_.load(std::memory_order_relaxed) && !this->paused_.load(std::memory_order_relaxed);
  }
  bool finishing() const { return this->finish_requested_.load(std::memory_order_relaxed); }
  bool wanted() const { return this->active_.load(); }
  bool writing() const { return this->writers_.load() != 0; }
  uint32_t last_play_ms() const { return this->last_play_ms_.load(std::memory_order_relaxed); }
  void check_finish();
  void set_timer_id(uint32_t id) { this->timer_id_ = id; }

 protected:
  size_t play_internal_(const uint8_t *data, size_t length, uint32_t time_budget_ms);
  // Accumulate the accepted byte count and warn once per window if it falls behind the
  // sample clock. bytes_per_ms is the nominal rate of the configured stream format.
  void check_throughput_(size_t accepted, size_t bytes_per_ms);

  USBAudioClient *client_;
  std::atomic<USBAudioDevice *> device_{nullptr};
  std::atomic<ring_buffer::RingBuffer *> ring_{nullptr};
  std::atomic<uint8_t> writers_{0};
  std::atomic<bool> active_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> finish_requested_{false};
  std::atomic<uint32_t> last_play_ms_{0};
  bool finish_timer_set_{false};
  bool warned_unavailable_{false};
  uint32_t timer_id_{0};
  uint32_t finish_deadline_ms_{0};
  // Throughput window (see check_throughput_). start 0 means "not started yet".
  uint32_t rate_window_start_ms_{0};
  size_t rate_window_bytes_{0};
};

class USBAudioCapture : public microphone::Microphone {
 public:
  explicit USBAudioCapture(USBAudioClient *client) : client_(client) {}

  // Safe from any task; the client's loop acts on the request.
  void start() override;
  void stop() override;

  void on_stream_event(MicStreamEvent event);
  void attach(USBAudioDevice *device, ring_buffer::RingBuffer *ring);
  void detach();
  USBAudioDevice *device() const { return this->device_; }
  bool idle() const { return this->listeners_.load() == 0 && this->state_ == microphone::STATE_STOPPED; }
  void process();
  size_t read(std::vector<uint8_t> &buffer);
  void deliver(std::vector<uint8_t> &buffer);
  bool running() const { return this->running_.load(std::memory_order_relaxed); }
  bool stopping() const { return this->state_ == microphone::STATE_STOPPING; }
  void set_delivering(bool delivering) { this->delivering_.store(delivering); }
  void set_timer_id(uint32_t id) { this->timer_id_ = id; }
  void add_enabled_channel(uint8_t channel) { this->enabled_mask_ |= static_cast<uint8_t>(1u << channel); }

 protected:
  static constexpr uint8_t COMMAND_START = 1 << 0;
  static constexpr uint8_t COMMAND_STOP = 1 << 1;
  // Same limit as i2s_audio's microphone.
  static constexpr uint8_t MAX_LISTENERS = 16;
  static constexpr uint32_t RETRY_MIN_MS = 1000;
  static constexpr uint32_t RETRY_MAX_MS = 30000;

  void request_(uint8_t set, uint8_t clear);
  void open_stream_();
  void set_running_();
  // Copy the first enabled channel over the disabled ones, in place.
  void apply_channel_selection_(uint8_t *data, size_t length);

  USBAudioClient *client_;
  USBAudioDevice *device_{nullptr};
  ring_buffer::RingBuffer *ring_{nullptr};
  std::atomic<uint8_t> commands_{0};
  // start() calls not yet matched by a stop(); capture stops when the last listener stops.
  std::atomic<uint8_t> listeners_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> delivering_{false};
  uint32_t retry_delay_ms_{RETRY_MIN_MS};
  uint32_t timer_id_{0};
  // Channel selection: a bit per enabled channel.
  uint8_t enabled_mask_{0};
  bool logged_invalid_frame_{false};
};

// -----------------------------------------------------------------------------
// USBAudioDevice
//
// One USB audio device of a client: its descriptors, streams, rings and controls.
//
// Thread model:
//   USB task     - fires isoc callbacks, reads/writes ring buffers
//   Main loop    - opens/closes streams, follows the sinks and the capture
//   Control task - class requests
// -----------------------------------------------------------------------------
class USBAudioDevice {
  friend class USBAudioClient;

 public:
  static constexpr uint8_t MAX_SINKS = 4;

  USBAudioDevice(USBAudioClient *client, usb_host::UsbDevice *dev);

  bool speaker_available() const { return this->device_connected_ && this->spk_format_ok_; }
  bool microphone_available() const { return this->device_connected_ && this->mic_format_ok_; }
  bool device_connected() const { return this->device_connected_.load(); }
  bool microphone_stream_open() const { return this->mic_stream_open_; }
  // Bytes the isochronous OUT sample clock moves per service interval, i.e. the granularity
  // the sink consumes at. 0 while no speaker stream is open. Writers align their chunks to
  // this so no partial packet is queued.
  uint32_t get_speaker_packet_bytes() const { return this->spk_stream_open_ ? this->spk_stream_.packet_size : 0; }
  const char *name() const { return this->name_; }
  uint32_t key() const { return this->key_; }
  const EndpointConfig &speaker_format() const { return this->spk_fmt_; }
  const EndpointConfig &microphone_format() const { return this->mic_fmt_; }

  // True when the stream is open or opening; on_stream_event() then reports the outcome.
  bool resume_microphone();
  void suspend_microphone();

  void set_speaker_volume_level(float volume);
  void set_speaker_mute_state(bool mute_state);
  void set_microphone_volume_level(float volume);
  void set_microphone_mute_state(bool mute_state);
  void wake_loop();

 protected:
  void on_connected();
  void on_disconnected();
  bool loop_();
  bool busy_() const;

  void on_stream_packet_(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error);
  size_t fill_(uint8_t *data, size_t max_len);
  void on_stream_open_(usb_host::IsocStream &stream, bool ok);
  size_t read_mixed_(uint8_t *dst, size_t len);

  // -- Descriptor parsing ----------------------------------------------------
  bool find_ac_interface_();  // sets ac_intf_
  // Record the units and terminals of the AudioControl interface. Returns the number
  // written to nodes, which holds at most UAC_MAX_TOPOLOGY_NODES entries.
  uint8_t collect_topology_(UacTopologyNode *nodes);
  // Record every Feature Unit of the AudioControl interface, at most max_count.
  uint8_t collect_feature_units_(UacFeatureUnit *units, uint8_t max_count);
  bool parse_feature_units_();
  // Channel layout declared by a Terminal, for UAC 1.0 where it is not on the stream.
  uint32_t terminal_channel_config_(uint8_t terminal_id);
  // Playback and capture halves of the multichannel pair mapping. Both run on the USB
  // task from the isochronous callbacks.
  size_t fill_mapped_(uint8_t *data, size_t max_len);
  void push_mapped_capture_(const uint8_t *data, size_t len);
  bool parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits,
                            uint32_t sample_rate, uint8_t &intf_out, UacAltInfo &alt_out, EndpointConfig &fmt_out);

  // -- UAC control requests --------------------------------------------------
  // Issue a class-specific control transfer and wait for it to finish, up to
  // CTRL_TIMEOUT_MS. The completion callback runs on the USB task and can fire after the
  // wait has been given up, so the result is not kept on the caller's stack.
  // Pass out_data for a request with an outgoing data stage, in_data to keep an incoming
  // one; both are limited to UAC_CTRL_MAX_DATA_LEN bytes.
  bool uac_control_transfer_(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                             const uint8_t *out_data, size_t out_len, uint8_t *in_data, size_t in_len,
                             bool *rejected = nullptr);
  // Issue a class-specific control transfer and busy-wait for completion.
  // wIndex for interface requests: (unit_id << 8) | ac_intf_num
  // wIndex for endpoint requests:  ep_addr
  bool uac_set_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                               const uint8_t *data, size_t len, bool *rejected = nullptr);
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
  void probe_sticky_volume_(UacControlState &ctl, const char *what, uint8_t channel);
  bool apply_volume_(UacControlState &ctl, const char *what);
  bool apply_mute_(UacControlState &ctl, const char *what);
  void set_volume_level_(std::atomic<float> &request, uint8_t pending_bit, float volume);
  void set_mute_state_(std::atomic<bool> &request, uint8_t pending_bit, bool mute_state);
  void resolve_device_quirks_();
  void read_identity_();

  // -- Control task --------------------------------------------------------------
  void run_ctrl_(const CtrlRequest &request);
  bool post_ctrl_(CtrlOp op, uint32_t seq);
  void post_controls_();
  // Control task side of a connect, and of a volume or mute change.
  void connect_setup_();
  void apply_pending_controls_();
  // Main loop side, once the control task has answered.
  void finish_connect_();
  void finish_speaker_open_();
  void finish_microphone_open_();

  // -- Stream open / close ---------------------------------------------------
  bool ensure_started_speaker_();
  bool ensure_started_microphone_();
  bool open_speaker_stream_();
  bool open_microphone_stream_();
  void close_speaker_stream_();
  void close_microphone_stream_();
  // Drop whatever is still queued so a reopened stream does not start by playing audio from
  // the previous session.
  void flush_speaker_buffer_();
  bool has_speaker_rings_() const;
  // Reopening a stream that keeps failing must not be retried on every play() call. Returns
  // true when the next attempt is due, and records the outcome so the delay can grow.
  bool reopen_due_(const uint32_t &next_attempt_ms, uint8_t fail_count) const;
  void note_open_result_(bool ok, uint32_t &next_attempt_ms, uint8_t &fail_count);
  // Report a condition that leaves the component unusable until the device changes.
  void set_stream_error_(const char *what);
  void close_idle_speaker_();
  void close_idle_microphone_();
  uint32_t timer_id_(uint32_t kind) const { return 0x100u + static_cast<uint32_t>(this->dev_->addr) * 4u + kind; }

  USBAudioClient *client_;
  usb_host::UsbDevice *dev_;
  char name_[48]{};
  uint32_t key_{0};

  struct SinkSlot {
    std::atomic<USBAudioSink *> sink{nullptr};
    std::unique_ptr<ring_buffer::RingBuffer> ring;
  };
  SinkSlot slots_[MAX_SINKS];
  USBAudioCapture *capture_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> mic_rb_;

  // True when the device enumerated at high speed (only the ESP32-P4 host can). Gates the
  // isochronous high-bandwidth multiplier and selects the OUT pacing service interval.
  bool device_is_high_speed_{false};

  // -- Descriptor parsing results --------------------------------------------
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
  EndpointConfig spk_fmt_{};
  EndpointConfig mic_fmt_{};

  // -- Stream state ----------------------------------------------------------
  usb_host::IsocStream spk_stream_{};
  usb_host::IsocStream mic_stream_{};
  usb_host::IsocStream spk_fb_stream_{};  // async OUT feedback IN stream (shares spk interface)
  bool spk_stream_open_{false};
  bool spk_fb_open_{false};
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

  // -- Playback state --------------------------------------------------------
  bool mic_suspended_{false};   // true when microphone is explicitly paused
  // Started and not stopped: with an idle timeout only a wanted stream is opened or kept.
  bool spk_wanted_{false};
  bool mic_wanted_{false};
  std::atomic<bool> device_connected_{false};
  std::atomic<bool> gone_{false};

  VolumeEncoding volume_encoding_{VolumeEncoding::DB};

  // -- Control task state ------------------------------------------------------
  static constexpr uint8_t PENDING_SPK_VOLUME = 1 << 0;
  static constexpr uint8_t PENDING_SPK_MUTE = 1 << 1;
  static constexpr uint8_t PENDING_MIC_VOLUME = 1 << 2;
  static constexpr uint8_t PENDING_MIC_MUTE = 1 << 3;

  // Bumped on disconnect; requests of an older connection are not sent.
  std::atomic<uint32_t> ctrl_gen_{0};
  // Control task only: the connection the request it runs belongs to.
  uint32_t ctrl_op_gen_{0};
  // Set while the control task hands a request to the host, so a disconnect waits for that
  // before the device and its transfer buffers go away.
  std::atomic<bool> ctrl_submitting_{false};
  std::atomic<uint8_t> ctrl_refs_{0};
  std::atomic<bool> ctrl_controls_queued_{false};
  std::atomic<uint8_t> ctrl_pending_{0};
  // The latest requested volume and mute; the control task sends them.
  std::atomic<float> spk_vol_req_{1.0f};
  std::atomic<bool> spk_mute_req_{false};
  std::atomic<float> mic_vol_req_{1.0f};
  std::atomic<bool> mic_mute_req_{false};
  // Written by the control task when a request finishes, read by the main loop.
  std::atomic<uint32_t> setup_done_seq_{0};
  std::atomic<uint32_t> spk_rate_done_seq_{0};
  std::atomic<uint32_t> mic_rate_done_seq_{0};
  // Main loop only: what is waiting for an answer.
  bool setup_pending_{false};
  bool spk_rate_pending_{false};
  bool mic_rate_pending_{false};
  uint32_t setup_seq_{0};
  uint32_t ctrl_seq_{0};
  uint32_t spk_rate_seq_{0};
  uint32_t mic_rate_seq_{0};
};

// -----------------------------------------------------------------------------
// USBAudioClient
//
// Native USB Audio Class host driver built on USBClient, for every audio device routed to
// it. No uac_host library - implements the class protocol directly using
// USBClient's control_transfer() and stream_open() / stream_close().
// -----------------------------------------------------------------------------
class USBAudioClient : public usb_host::USBClient {
  friend class USBAudioDevice;
  friend class USBAudioSink;
  friend class USBAudioCapture;

 public:
  // Only devices that describe an Audio Class interface are ours. Without this the client
  // matches anything that enumerates, including hubs and mass storage, and takes the first
  // device it sees. vid and pid stay 0 by default, which the host reads as "any", and are
  // narrowed from the config when the user names a specific device.
  USBAudioClient() : usb_host::USBClient(0, 0) { this->add_class_match(USB_CLASS_AUDIO); }

  void set_vid(uint16_t vid) { this->vid_ = vid; }
  void set_pid(uint16_t pid) { this->pid_ = pid; }

  void setup() override;
  void loop() override;
  bool teardown() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  // -- Configuration setters -------------------------------------------------
  void set_microphone_buffer_size(uint32_t size) { this->mic_buf_size_ = size; }
  void set_speaker_buffer_size(uint32_t size) { this->spk_buf_size_ = size; }

  void set_microphone_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);
  void set_speaker_params(uint8_t channels, uint16_t bits, uint32_t sample_rate);
  // Use an asynchronous OUT endpoint's feedback stream to pace playback (default on).
  void set_feedback_enabled(bool enabled) { this->feedback_enabled_ = enabled; }
  // A stream not used for this long is closed, giving its pipes (host channels) back; 0 keeps
  // the streams open from connect to disconnect.
  void set_stream_idle_timeout(uint32_t ms) { this->stream_idle_timeout_ms_ = ms; }
  void set_volume_curve(VolumeCurve curve) { this->volume_curve_ = curve; }
  // Register a per-device quirk, matched against the device descriptor on connection.
  void add_device_quirk(uint16_t vid, uint16_t pid, VolumeEncoding encoding) {
    this->quirks_.push_back(UsbAudioQuirk{vid, pid, encoding});
  }
  // Which pair of a multichannel device is driven as the stereo stream. Only consulted
  // when the device offers no plain stereo alt-setting.
  void set_channel_pair(UacChannelPair pair) { this->channel_pair_ = pair; }

  void add_speaker_route(SpeakerRoute route) { this->speaker_routes_.push_back(std::move(route)); }
  void add_microphone_route(MicrophoneRoute route) { this->microphone_routes_.push_back(std::move(route)); }
  void set_speaker(USBAudioSink *speaker) { this->speaker_ = speaker; }
  void set_microphone(USBAudioCapture *microphone) { this->microphone_ = microphone; }

  bool device_connected() const;
  void set_speaker_volume_level(float volume);
  void set_speaker_mute_state(bool mute_state);
  float get_speaker_volume_level() const { return this->spk_volume_.load(); }
  bool get_speaker_mute_state() const { return this->spk_mute_.load(); }

  uint32_t get_speaker_buffer_size() const { return this->spk_buf_size_; }
  uint32_t get_microphone_buffer_size() const { return this->mic_buf_size_; }
  const EndpointConfig &speaker_config() const { return this->spk_cfg_; }
  const EndpointConfig &microphone_config() const { return this->mic_cfg_; }

 protected:
  bool accepts_more_devices_() const override { return true; }
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;
  bool device_busy_(const usb_host::UsbDevice &dev) const override;
  uint32_t skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                              const usb_intf_desc_t *alt) const override;

  // USB-task context.
  void on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error,
                        bool skipped) override;
  size_t on_stream_fill(usb_host::IsocStream &stream, uint8_t *data, size_t max_len) override;
  void on_stream_out_error(usb_host::IsocStream &stream) override;
  void on_stream_open(usb_host::IsocStream &stream, bool ok) override;

  static USBAudioDevice *device_of_(const usb_host::IsocStream &stream) {
    return stream.device != nullptr ? static_cast<USBAudioDevice *>(stream.device->context) : nullptr;
  }
  USBAudioSink *take_sink_();
  USBAudioCapture *take_capture_();
  void attach_routes_(USBAudioDevice *device);
  void attach_fixed_();
  void release_device_(USBAudioDevice *device);
  void make_name_unique_(USBAudioDevice *device);

  static void ctrl_task(void *param);
  bool post_ctrl_(CtrlOp op, USBAudioDevice *device, uint32_t seq);
  static void capture_task(void *param);
  void capture_task_loop_();
  void wake_capture_task_();

  std::vector<SpeakerRoute> speaker_routes_;
  std::vector<MicrophoneRoute> microphone_routes_;
  USBAudioSink *speaker_{nullptr};
  USBAudioCapture *microphone_{nullptr};
  std::vector<USBAudioSink *> sinks_;
  std::vector<USBAudioCapture *> captures_;

  EndpointConfig mic_cfg_{};
  EndpointConfig spk_cfg_{};
  uint32_t spk_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};
  uint32_t mic_buf_size_{USB_AUDIO_DEFAULT_BUFFER_SIZE};
  bool feedback_enabled_{true};
  uint32_t stream_idle_timeout_ms_{0};
  VolumeCurve volume_curve_{VolumeCurve::LINEAR};
  UacChannelPair channel_pair_{UacChannelPair::FRONT};
  std::vector<UsbAudioQuirk> quirks_;
  std::atomic<float> spk_volume_{1.0f};
  std::atomic<bool> spk_mute_{false};

  // The task that runs the component loop, captured in setup().
  TaskHandle_t loop_task_{nullptr};
  static constexpr uint32_t REOPEN_TIMEOUT_ID = 2;  // the reopen backoff wakes loop()

  static constexpr size_t CTRL_QUEUE_LENGTH = 8;
  StaticTask ctrl_task_;
  QueueHandle_t ctrl_queue_{nullptr};
  StaticQueue_t ctrl_queue_struct_{};
  std::array<uint8_t, CTRL_QUEUE_LENGTH * sizeof(CtrlRequest)> ctrl_queue_storage_{};
  std::atomic<uint32_t> transfer_errors_{0};

  StaticTask capture_task_;
  SemaphoreHandle_t capture_lock_{nullptr};
  StaticSemaphore_t capture_lock_struct_{};
  std::vector<uint8_t> capture_buffer_;
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
