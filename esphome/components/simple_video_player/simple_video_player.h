#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef SVP_USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "lvgl.h"
#endif

#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"

// ESP32-P4 hardware JPEG decoder only -- other variants cannot decode fast enough for video.

#ifdef SVP_DSI_OUTPUT
#include "esphome/components/display/display.h"
#endif

#ifdef SVP_STREAM_TOUCH
#include "esphome/components/touchscreen/touchscreen.h"
#endif

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#ifdef USE_AUDIO
#include "esphome/components/audio/audio_decoder.h"
#endif
// ring_buffer::RingBuffer is used for the audio input/decoded rings (see USE_AUDIO members).
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "buffered_file_reader.h"
#include "avi_parser.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdio>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

namespace esphome::simple_video_player {

/// Speaker channel modes for audio routing
enum class SpeakerChannelMode : uint8_t {
  SPEAKER_CHANNEL_MONO = 0,    // Downmix stereo to mono (average L+R)
  SPEAKER_CHANNEL_LEFT = 1,    // Use only left channel
  SPEAKER_CHANNEL_RIGHT = 2,   // Use only right channel
  SPEAKER_CHANNEL_STEREO = 3,  // Pass through stereo unchanged
};

/// Player states
enum class PlayerState : uint8_t {
  STOPPED = 0,
  PLAYING = 1,
  PAUSED = 2,
  ERROR = 3,
};

/// Playback error codes
enum class PlaybackError : uint8_t {
  NONE = 0,
  FILE_NOT_FOUND = 1,
  DECODER_INIT_FAILED = 2,
  BUFFER_ALLOCATION_FAILED = 3,
  DECODE_ERROR = 4,
  FILE_READ_ERROR = 5,
  INVALID_VIDEO_FORMAT = 6,
};

/// Video file formats
enum class VideoFormat : uint8_t {
  UNKNOWN = 0,
  RAW_MJPEG = 1,  // Raw concatenated JPEG frames
  AVI_MJPEG = 2,  // AVI container with MJPEG video
};

// Forward declarations for automation
class SimpleVideoPlayer;

/// Trigger fired when playback starts
class PlaybackStartedTrigger : public Trigger<> {
 public:
  explicit PlaybackStartedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback finishes normally
class PlaybackFinishedTrigger : public Trigger<> {
 public:
  explicit PlaybackFinishedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback is paused
class PlaybackPausedTrigger : public Trigger<> {
 public:
  explicit PlaybackPausedTrigger(SimpleVideoPlayer *parent);
};

/// Trigger fired when playback error occurs
class PlaybackErrorTrigger : public Trigger<uint8_t> {
 public:
  explicit PlaybackErrorTrigger(SimpleVideoPlayer *parent);
};

#ifdef SVP_STREAM_TOUCH
/// Forwards the touchscreen to the network stream sender ('T' messages).
class StreamTouchListener : public touchscreen::TouchListener {
 public:
  explicit StreamTouchListener(SimpleVideoPlayer *parent) : parent_(parent) {}
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;

 protected:
  SimpleVideoPlayer *parent_;
};
#endif

/// Main video player component
class SimpleVideoPlayer : public Component {
 public:
  ~SimpleVideoPlayer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  //========================================================================
  // Configuration (called from codegen)
  //========================================================================

#ifdef SVP_USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl_component) { this->lvgl_component_ = lvgl_component; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
#endif
  void set_cache_buffer_size(uint32_t size) { this->cache_buffer_size_ = size; }
  void set_input_buffer_size(uint32_t size) { this->input_buffer_size_ = size; }
  void set_target_fps(float fps) { this->target_fps_ = fps; }
  void set_prefetch_duration_ms(uint32_t ms) { this->prefetch_duration_ms_ = ms; }

#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_speaker_channel_mode(SpeakerChannelMode mode) { this->speaker_channel_mode_ = mode; }
#endif

#ifdef SVP_DSI_OUTPUT
  // Direct output onto a display with draw_pixels_at() while LVGL is paused. Wired by the FINAL
  // codegen coroutine together with the display's color_depth (16 or 24).
  void set_dsi(display::Display *dsi) { this->dsi_ = dsi; }
  void set_dsi_color_depth(uint8_t bits) { this->dsi_fb_bpp_ = bits == 24 ? 3 : 2; }
#endif

#ifdef SVP_STREAM
  // Portall-style network receiver (udisp protocol over TCP): JPEG rectangles drawn onto the display
  // while no file is playing.
  void set_stream_port(uint16_t port) { this->stream_port_ = port; }
  void set_stream_max_frame_bytes(uint32_t bytes) { this->stream_max_frame_bytes_ = bytes; }
#ifdef SVP_STREAM_TOUCH
  void set_stream_touchscreen(touchscreen::Touchscreen *ts) { this->stream_touchscreen_ = ts; }
  void stream_on_touch(const touchscreen::TouchPoints_t &points);
#endif
#endif

#ifdef SVP_CHANNEL_LIST
  // Channel list from the svp_relay Home Assistant integration, shown in an LVGL dropdown;
  // channel_play() asks the relay to stream the selected channel to stream_port.
  void set_channel_url(const std::string &url) { this->channel_url_ = url; }
  void set_channel_token(const std::string &token) { this->channel_token_ = token; }
  void set_channel_widget(lvgl::LvDropdownType *widget) { this->channel_widget_ = widget; }
  void channel_list_refresh() { this->channel_request_(CHANNEL_CMD_REFRESH, 0); }
  void channel_play();
  void channel_stop() { this->channel_request_(CHANNEL_CMD_STOP, 0); }
#endif

  //========================================================================
  // Playback Control API
  //========================================================================

  /// Start playing a video file (local or network path)
  void play(const std::string &video_path);

  /// Pause playback (can be resumed)
  void pause();

  /// Resume playback from paused state
  void resume();

  /// Stop playback completely
  void stop();

  /// Set loop mode (restart from beginning when finished)
  void set_loop(bool loop) { this->loop_ = loop; }

  //========================================================================
  // State Query
  //========================================================================

  PlayerState get_state() const { return this->state_; }
  bool is_playing() const { return this->state_ == PlayerState::PLAYING; }
  bool is_paused() const { return this->state_ == PlayerState::PAUSED; }
  bool is_stopped() const { return this->state_ == PlayerState::STOPPED; }
  PlaybackError get_last_error() const { return this->last_error_; }
  const std::string &get_current_file() const { return this->video_path_; }

  //========================================================================
  // Automation Callbacks
  //========================================================================

  void add_on_started_callback(std::function<void()> &&callback) {
    this->on_started_callbacks_.add(std::move(callback));
  }
  void add_on_finished_callback(std::function<void()> &&callback) {
    this->on_finished_callbacks_.add(std::move(callback));
  }
  void add_on_paused_callback(std::function<void()> &&callback) { this->on_paused_callbacks_.add(std::move(callback)); }
  void add_on_error_callback(std::function<void(uint8_t)> &&callback) {
    this->on_error_callbacks_.add(std::move(callback));
  }

 protected:
  //========================================================================
  // Playback Task (Core 1): open file, probe dimensions, allocate buffers, then loop:
  // read the next compressed frame (demux + audio feed) straight from BufferedFileReader,
  // HW-decode into A/B, pace, present.
  //========================================================================

  /// FreeRTOS task entry point (decode/playback task, pinned to Core 1)
  static void playback_task_entry_(void *param);

  /// Main playback loop (runs in task)
  void playback_loop_();

  /// Reader/producer task entry point (demux + fill the compressed-frame pre-buffer, pinned to
  /// Core 0 -- never touches DMA2D, so it cannot race decode/PPA on Core 1, see esp-idf#18999).
  static void reader_task_entry_(void *param);
  void reader_loop_();

  /// Wait for a task to stop
  bool wait_for_task_stop_(TaskHandle_t &handle, uint32_t timeout_ms);

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read the next JPEG frame into dest_buffer (capacity dest_capacity)
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity);

  /// read_next_frame_() into dest (capacity cap), with loop rewind and a stop check.
  /// Returns payload size (> 0), 0 at EOF, -1 on read error, -2 if stopped/aborted.
  int read_frame_(uint8_t *dest, size_t cap);

  /// Decode JPEG frame (from the ring slot the decode task currently holds) and update canvas
  bool decode_frame_(const uint8_t *frame_data, size_t frame_size);

#ifdef SVP_DSI_OUTPUT
  /// setup(): read the panel size and allocate the decode buffer. false -> mark_failed.
  bool init_dsi_output_();
#endif
#ifdef SVP_STREAM
  bool setup_stream_();
  static void stream_net_task_entry_(void *param);
  void stream_net_loop_();
  static void stream_draw_task_entry_(void *param);
  void stream_draw_loop_();
  void stream_feed_(const uint8_t *data, size_t len);
  void stream_send_pending_(int client);
  /// Tell the sender whether the panel shows its picture (false while a file plays).
  void stream_set_awake_(bool awake);
#ifdef SVP_CHANNEL_LIST
  enum : uint8_t { CHANNEL_CMD_REFRESH, CHANNEL_CMD_PLAY, CHANNEL_CMD_STOP };
  struct ChannelCmd {
    uint8_t cmd;
    uint16_t index;
  };
  bool setup_channel_list_();
  void channel_request_(uint8_t cmd, uint16_t index);
  void channel_loop_();
  static void channel_task_entry_(void *param);
  void channel_task_loop_();
  /// Blocking HTTP to the relay (channel task only). response: PSRAM buffer, caller frees.
  bool channel_http_(const char *path, const char *body, char **response, size_t *response_len);
#endif
#ifdef USE_AUDIO
  /// PCM payload bytes from the sender (network task): converted to the speaker's channel count and
  /// played in 10 ms blocks; dropped while a file plays.
  void stream_on_audio_(const uint8_t *data, size_t len);
  void stream_emit_audio_frames_(const uint8_t *src, size_t frames);
  void stream_flush_audio_block_();
#endif
#endif
  /// Create the ESP32-P4 hardware JPEG decoder engine, called once from setup().
  bool init_decoder_();
  /// Header-only parse (width/height, no pixel decode), used by get_video_dimensions_() for the
  /// raw-MJPEG case.
  bool parse_header_(const uint8_t *buffer, size_t size, uint32_t &width, uint32_t &height);

  /// Get video dimensions from first frame
  bool get_video_dimensions_(uint32_t &width, uint32_t &height);

#ifdef USE_AUDIO
  /// Initialize audio decoder for AVI audio stream
  bool init_audio_decoder_();

  /// Process audio frames from AVI
  void process_audio_frame_(const AVIFrame &frame, const uint8_t *data, size_t size);

  /// Audio processing task entry point (runs on Core 0)
  static void audio_task_entry_(void *param);

  /// Audio processing loop (decodes and converts channels)
  void audio_processing_loop_();

  /// Stop audio processing task
  void stop_audio_task_();
  /// Right before the first frame: make sure the speaker (started at audio init) is running.
  void wait_speaker_running_();
#endif

  //========================================================================
  // File I/O Abstraction (supports local and network storage)
  //========================================================================

  /// Detect video file format (AVI vs raw MJPEG)
  VideoFormat detect_format_();

  /// Open file (local FatFS or network storage)
  bool open_file_(const std::string &path);

  /// Close file
  void close_file_();

  /// Read data from file (handles both local and network)
  int read_data_(uint8_t *buffer, size_t size);

  /// Seek to position in file
  bool seek_to_(uint64_t position);

  /// Get file size
  bool get_file_size_(uint64_t &size);

  //========================================================================
  // Buffer Management
  //========================================================================

  /// Allocate all buffers (cache, input, output)
  bool allocate_buffers_(uint32_t video_width, uint32_t video_height);

  /// Free all buffers
  void free_buffers_();

  //========================================================================
  // Error Handling
  //========================================================================

  void set_error_(PlaybackError error);

  //========================================================================
  // Members
  //========================================================================

  // Configuration
#ifdef SVP_USE_LVGL
  lvgl::LvglComponent *lvgl_component_{nullptr};
  lv_obj_t *canvas_{nullptr};
#endif
  uint32_t cache_buffer_size_{16 * 1024};   // 16KB internal RAM (aligned cache)
  uint32_t input_buffer_size_{256 * 1024};  // 256KB PSRAM (worst-case single compressed frame size)
  float target_fps_{30.0f};                 // Target frame rate
  uint32_t prefetch_duration_ms_{1000};     // accepted for config compat; read-ahead is the transfer-buffer arena

#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};  // Optional speaker for audio playback
  SpeakerChannelMode speaker_channel_mode_{SpeakerChannelMode::SPEAKER_CHANNEL_MONO};  // Channel routing mode
#endif

  // Playback state. std::atomic because the playback task (Core 1) reads it in its loop while
  // play()/pause()/resume()/stop()/set_error_() write it from the main-loop core -- a plain field
  // could be hoisted out of the loop by the compiler and carries no cross-core visibility
  // guarantee. Transitions use compare_exchange/exchange; no mutex.
  std::atomic<PlayerState> state_{PlayerState::STOPPED};
  // Atomic for the same cross-core reason; written just before state_ becomes ERROR so a reader
  // that sees ERROR also sees the reason.
  std::atomic<PlaybackError> last_error_{PlaybackError::NONE};
  bool loop_{false};
  std::string video_path_;  // written in play() before the task starts, then read only by the task

  // File reader (backed by storage::StorageWorker -- handles local/network storage
  // transparently, see buffered_file_reader.h)
  std::unique_ptr<BufferedFileReader> file_reader_;
  uint64_t file_size_{0};

  // Video format and container parser
  VideoFormat video_format_{VideoFormat::UNKNOWN};
  std::unique_ptr<AVIParser> avi_parser_;  // AVI container parser (if AVI format)

#ifdef USE_AUDIO
  // This player commits to ONE fixed audio format for every video. sample_rate/channels/
  // bits_per_sample are resolved from the referenced speaker's own config (a hardware property,
  // not asked of the user twice -- see __init__.py's _resolve_speaker_audio_format()); audio_codec
  // is the one piece that's actually a user-facing YAML option, since it's a property of the
  // video file, not the speaker. All four reach here as SVP_AUDIO_* defines. That's what makes it
  // possible to size the audio ring
  // buffers/temp buffer once and allocate them once, in setup(), like every other persistent
  // buffer in this component -- instead of recomputing sizes from whatever a given AVI file's
  // audio stream header happens to say and reallocating per play() (AGENTS.md: no heap
  // allocation after setup()). Per-file auto-detected format info is still read at open time
  // (init_audio_decoder_()), but only to VALIDATE it matches this fixed format -- a mismatch is
  // a hard error (no audio for that file), never a reason to resize anything.
#if defined(SVP_AUDIO_SAMPLE_RATE)
  static constexpr uint32_t AUDIO_SAMPLE_RATE = SVP_AUDIO_SAMPLE_RATE;
  static constexpr uint8_t AUDIO_SOURCE_CHANNELS = SVP_AUDIO_SOURCE_CHANNELS;
  static constexpr uint8_t AUDIO_BITS_PER_SAMPLE = SVP_AUDIO_BITS_PER_SAMPLE;
  static constexpr size_t AUDIO_BYTES_PER_FRAME =
      static_cast<size_t>(AUDIO_SOURCE_CHANNELS) * (AUDIO_BITS_PER_SAMPLE / 8);
  static constexpr size_t AUDIO_BYTES_PER_SEC = static_cast<size_t>(AUDIO_SAMPLE_RATE) * AUDIO_BYTES_PER_FRAME;
  // Same target durations / minimums init_audio_decoder_() always used -- just resolved at
  // compile time now instead of recomputed from a parsed file header every play().
  static constexpr size_t AUDIO_INPUT_BUFFER_SIZE = (AUDIO_BYTES_PER_SEC * 250 / 1000) > (32 * 1024)
                                                        ? (AUDIO_BYTES_PER_SEC * 250 / 1000)
                                                        : (32 * 1024);
  static constexpr size_t AUDIO_DECODED_BUFFER_SIZE = (AUDIO_BYTES_PER_SEC * 500 / 1000) > (16 * 1024)
                                                          ? (AUDIO_BYTES_PER_SEC * 500 / 1000)
                                                          : (16 * 1024);
  static constexpr size_t AUDIO_TEMP_BUFFER_SIZE = (AUDIO_BYTES_PER_SEC * 100 / 1000) > (8 * 1024)
                                                       ? (AUDIO_BYTES_PER_SEC * 100 / 1000)
                                                       : (8 * 1024);
  static constexpr size_t AUDIO_DECODER_INPUT_BUFFER_SIZE = AUDIO_SAMPLE_RATE > 48000 ? (96 * 1024) : (64 * 1024);
  static constexpr size_t AUDIO_DECODER_OUTPUT_BUFFER_SIZE = AUDIO_SAMPLE_RATE > 48000 ? (48 * 1024) : (32 * 1024);
#endif

  // Audio decoding (for AVI with audio streams). audio_decoder_/audio_input_ring_buffer_/
  // audio_decoded_ring_buffer_/audio_temp_buffer_ are ALL allocated ONCE in setup() (sized from
  // the AUDIO_* constants above, only when the fixed codec is MP3/FLAC for audio_decoder_ itself
  // -- PCM mode never uses it) and reused for every play() -- only reset() (ring buffers) or
  // re-add_source()/add_sink()/start()'d (audio_decoder_) between sessions, never freed/recreated.
  // Verified against the real audio component source: AudioDecoder::start() already resets its
  // own per-file state (potentially_failed_count_, end_of_file_, a fresh per-codec sub-decoder)
  // on every call, so calling it again on a persistent instance is exactly what it's for.
  std::unique_ptr<audio::AudioDecoder> audio_decoder_;                // Audio decoder (MP3/FLAC/PCM)
  std::shared_ptr<ring_buffer::RingBuffer> audio_input_ring_buffer_;  // Ring buffer for encoded audio (in PSRAM)
  std::shared_ptr<ring_buffer::RingBuffer>
      audio_decoded_ring_buffer_;                 // Ring buffer for decoded audio (in PSRAM, before conversion)
  std::unique_ptr<uint8_t[]> audio_temp_buffer_;  // Temporary buffer for audio processing (in PSRAM)
  uint8_t source_audio_channels_{0};              // Fixed source channel count (mirrors AUDIO_SOURCE_CHANNELS)
  uint8_t speaker_audio_channels_{1};             // Number of channels speaker expects
  uint32_t audio_sample_rate_{0};                 // Fixed sample rate (mirrors AUDIO_SAMPLE_RATE)
  uint8_t audio_bits_per_sample_{16};             // Fixed bits per sample (mirrors AUDIO_BITS_PER_SAMPLE)
  bool audio_enabled_{false};                     // Audio stream available and enabled
  // Whether THIS file's audio track actually uses audio_decoder_ (compressed: MP3/FLAC) vs direct
  // PCM. In a multi-codec build audio_decoder_ exists regardless, so the object pointer can no
  // longer signal the per-file mode -- this flag does. Set in init_audio_decoder_() per play().
  std::atomic<bool> audio_use_decoder_{false};
  TaskHandle_t audio_task_handle_{nullptr};       // Audio processing task (runs on Core 0)
  volatile bool audio_task_stop_{false};          // Signal to stop audio task
#endif

  // Cache buffer state (for frame parsing - only used for raw MJPEG)
  size_t cache_buffer_valid_{0};   // Valid bytes in cache
  size_t cache_buffer_offset_{0};  // Read offset within cache

  // Video properties
  uint32_t video_width_{0};
  uint32_t video_height_{0};

  // Buffers -- allocated ONCE in setup(), sized for the max resolution, reused every play().
  std::unique_ptr<uint8_t[]> cache_buffer_;  // Internal RAM, aligned for DMA
  // Decoded RGB888 double-buffer for the LVGL-canvas path (PSRAM). The P4 HW JPEG decoder
  // DMA2D-writes the next frame into the back buffer while LVGL's PPA reads the front buffer to
  // blit/rotate it, so neither side ever sees a half-written frame -- the former single buffer tore
  // because decode could be tick-preempted mid-write while LVGL blitted it. BOTH buffers come from
  // jpeg_alloc_decoder_mem: the 16-byte/DMA/cache-line alignment is required because the decoder
  // (writer) AND LVGL's PPA (reader) are both DMA engines. Allocated once in setup(), freed via
  // heap_caps_free (NOT delete[]). Not used in the DSI path (that renders into driver FBs).
  uint8_t *output_buffers_[2]{};
  size_t output_buffer_size_{0};
  uint8_t decode_buf_idx_{0};  // which output buffer decode_frame_() writes next (playback-task-local)

  // Published by decode_frame_() (video task, Core 1) once a frame is complete; consumed by loop()
  // (LVGL thread, Core 1) which points the canvas at it and invalidates. All lv_canvas_* calls
  // happen on the LVGL thread only -- never from the video task (LVGL is not thread-safe).
  std::atomic<uint8_t *> display_buffer_{nullptr};
  uint32_t canvas_w_{0};  // ALIGN_UP(width,16) for lv_canvas_set_buffer, set once per session
  uint32_t canvas_h_{0};  // ALIGN_UP(height,16)

  // Set by decode_frame_() after a frame is decoded; consumed by loop() on the LVGL thread.
  std::atomic<bool> frame_ready_{false};

  // Start handshake: the playback task sets 1 once preload is done; loop() (main thread) pauses LVGL
  // for DSI output, fires on_playback_started and sets 2. Nothing is presented or played before that.
  std::atomic<uint8_t> start_req_{0};
  std::atomic<bool> av_started_{false};
  uint32_t avi_fps_num_{0};
  uint32_t avi_fps_den_{0};

#ifdef SVP_DSI_OUTPUT
  display::Display *dsi_{nullptr};  // non-null -> direct display output instead of the LVGL canvas
  bool dsi_lvgl_paused_{false};     // LVGL was paused for the duration of direct playback
  uint16_t dsi_out_w_{0}, dsi_out_h_{0};  // panel native resolution; the video must match it
  uint8_t dsi_fb_bpp_{2};                 // display color_depth: 24 -> 3, 16 -> 2 bytes/pixel
  uint8_t *decode_target_{nullptr};       // own decode buffer (jpeg_alloc_decoder_mem, PSRAM)
  size_t decode_target_len_{0};
#endif

#ifdef SVP_STREAM
  struct StreamFrame {
    uint8_t *data{nullptr};
    size_t capacity{0};
    uint16_t x{0}, y{0}, w{0}, h{0};
    uint32_t total{0};
    uint32_t received{0};
  };
  static constexpr uint8_t STREAM_FRAME_COUNT = 3;
  uint16_t stream_port_{0};
  uint32_t stream_max_frame_bytes_{256 * 1024};
  StreamFrame stream_frames_[STREAM_FRAME_COUNT]{};
  QueueHandle_t stream_empty_q_{nullptr};
  QueueHandle_t stream_filled_q_{nullptr};
  TaskHandle_t stream_draw_task_{nullptr};
  // Held by the file playback for its whole session and by the stream draw per rectangle: both use
  // decode_target_ and the display, and the file has priority.
  SemaphoreHandle_t output_lock_{nullptr};
  // Parser state (network task only).
  StreamFrame *stream_current_{nullptr};
  uint8_t stream_hdr_[16]{};
  size_t stream_hdr_len_{0};
  uint32_t stream_skip_{0};
  bool stream_logged_bad_header_{false};
  std::atomic<bool> stream_connected_{false};
  std::atomic<bool> stream_output_ok_{false};  // loop(): connected and LVGL (if any) paused
  std::atomic<bool> stream_awake_{true};
  std::atomic<bool> stream_status_pending_{false};
  std::atomic<bool> stream_depth_pending_{false};
#ifdef SVP_STREAM_TOUCH
  static constexpr uint8_t STREAM_TOUCH_MAX = 5;
  struct StreamTouch {
    uint8_t count;
    uint8_t id[STREAM_TOUCH_MAX];
    uint16_t x[STREAM_TOUCH_MAX];
    uint16_t y[STREAM_TOUCH_MAX];
  };
  touchscreen::Touchscreen *stream_touchscreen_{nullptr};
  StreamTouchListener stream_touch_listener_{this};
  QueueHandle_t stream_touch_q_{nullptr};
  StreamTouch stream_last_touch_{};
  bool stream_last_touch_valid_{false};
#endif
#ifdef USE_AUDIO
  uint32_t stream_audio_left_{0};  // PCM payload bytes still to come (network task)
  uint8_t stream_audio_src_ch_{1};
  uint32_t stream_audio_rate_{0};
  uint8_t stream_audio_carry_[4]{};  // a sample frame split across two reads
  size_t stream_audio_carry_len_{0};
  uint8_t *stream_audio_block_{nullptr};
  size_t stream_audio_block_size_{0};
  size_t stream_audio_block_used_{0};
  uint8_t stream_audio_out_ch_{1};
  bool stream_logged_audio_format_{false};
  std::atomic<bool> stream_rate_pending_{false};
  std::atomic<bool> stream_audio_on_{false};
  std::atomic<uint32_t> stream_last_audio_ms_{0};
#endif
#endif

#ifdef SVP_CHANNEL_LIST
  std::string channel_url_;
  std::string channel_token_;
  lvgl::LvDropdownType *channel_widget_{nullptr};
  QueueHandle_t channel_q_{nullptr};
  SemaphoreHandle_t channel_lock_{nullptr};
  // Shown in the dropdown (LVGL holds pointers into channel_names_) / next list from the task.
  std::vector<std::string> channel_names_, channel_refs_;
  std::vector<std::string> channel_new_names_, channel_new_refs_;
  std::atomic<bool> channel_new_{false};
  bool channel_requested_once_{false};
#endif

  jpeg_decoder_handle_t hw_jpeg_decoder_{nullptr};

  // ONE compressed "storage-load" buffer: the reader task (Core 0) demuxes the next compressed
  // frame into it, the playback task (Core 1) decodes from it, cycled through two FreeRTOS queues.
  // This is NOT where pre-buffering/read-jitter absorption happens -- that is the PSRAM ring in the
  // storage interface (BufferedFileReader), which sits in front of this. The decoded render
  // double-buffer is output_buffers_[2] (the two frame buffers); this is the single video buffer.
  // Allocated once in setup(), reused every play().
  static constexpr uint8_t FRAME_SLOT_COUNT = 1;
  struct FrameSlot {
    uint8_t *data{nullptr};
    size_t capacity{0};
    int size{0};  // payload bytes (> 0); 0 = EOF sentinel; -1 = read-error sentinel
  };
  FrameSlot frame_slots_[FRAME_SLOT_COUNT]{};
  QueueHandle_t empty_queue_{nullptr};   // slots free for the reader to fill
  QueueHandle_t filled_queue_{nullptr};  // slots filled, waiting for decode
  TaskHandle_t reader_task_handle_{nullptr};
  volatile bool reader_task_stop_{false};
  // Absolute presentation index of the last frame read (demux order, monotonic across a loop
  // rewind). Playback-task-local.
  uint32_t video_frame_index_{0};
  // Set true in the teardown of playback_loop_() (and wired to file_reader_'s abort flag) so an
  // in-flight storage wait returns promptly at end of playback.
  volatile bool playback_task_stop_{false};

  // FreeRTOS task (decode/playback, Core 1 -- alongside the main loop/LVGL, see play()'s
  // xTaskCreatePinnedToCore comment for why decode specifically needs to share that core)
  TaskHandle_t task_handle_{nullptr};

  // Frame pacing -- wall clock is the master timeline. Each frame's target instant is
  // playback_start_time_us_ + paused_accum_us_ + index * frame_duration_us_. The task spins on a
  // wall-clock comparison until that instant, then presents; it never drops and never sleeps.
  // Audio free-runs on the speaker clock on its own Core 0 task -- no explicit A/V re-sync.
  int64_t playback_start_time_us_{0};  // esp_timer_get_time() at frame 0
  int64_t paused_accum_us_{0};         // total wall time spent PAUSED, excluded from media_us
  float frame_duration_us_{0};         // duration of one frame in microseconds (1000000/fps)

  // Playback-task analysis counters, summarised once after the loop (no logging on the pacing
  // path; only cheap micros()/adds per frame, so no runtime regression). Reset per play().
  uint32_t decode_fail_count_{0};
  uint32_t late_frame_count_{0};   // frames whose achieved present interval blew the frame budget
  int64_t last_present_us_{0};     // previous paced-release timestamp, to measure the interval
  uint64_t decode_us_sum_{0};      // time in decode_frame_ (JPEG decode)
  uint32_t decode_us_max_{0};
  uint64_t present_us_sum_{0};     // time in the DSI present (draw_pixels_at) -- DSI path only
  uint32_t present_us_max_{0};
  uint32_t bad_payload_count_{0};  // frame payloads without JPEG SOI/EOI (fetch-path corruption)
  static constexpr uint32_t SOLID_FRAME_LOG_MAX = 8;
  uint32_t solid_frame_count_{0};                      // decoded frames whose 4x4 sample grid is one colour
  uint32_t solid_frame_idx_[SOLID_FRAME_LOG_MAX]{};    // frame indices of the first of them

  // Automation callbacks
  CallbackManager<void()> on_started_callbacks_;
  CallbackManager<void()> on_finished_callbacks_;
  CallbackManager<void()> on_paused_callbacks_;
  CallbackManager<void(uint8_t)> on_error_callbacks_;
};

//========================================================================
// Automation Actions
//========================================================================

template<typename... Ts> class PlayAction : public Action<Ts...> {
 public:
  explicit PlayAction(SimpleVideoPlayer *player) : player_(player) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(const Ts &...x) override {
    auto path = this->path_.value(x...);
    this->player_->play(path);
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class PauseAction : public Action<Ts...> {
 public:
  explicit PauseAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->pause(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ResumeAction : public Action<Ts...> {
 public:
  explicit ResumeAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->resume(); }

 protected:
  SimpleVideoPlayer *player_;
};

#ifdef SVP_CHANNEL_LIST
template<typename... Ts> class ChannelRefreshAction : public Action<Ts...> {
 public:
  explicit ChannelRefreshAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->channel_list_refresh(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ChannelPlayAction : public Action<Ts...> {
 public:
  explicit ChannelPlayAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->channel_play(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ChannelStopAction : public Action<Ts...> {
 public:
  explicit ChannelStopAction(SimpleVideoPlayer *player) : player_(player) {}
  void play(const Ts &...x) override { this->player_->channel_stop(); }

 protected:
  SimpleVideoPlayer *player_;
};
#endif

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(const Ts &...x) override { this->player_->stop(); }

 protected:
  SimpleVideoPlayer *player_;
};

//========================================================================
// Trigger Implementations (after SimpleVideoPlayer is complete)
//========================================================================

inline PlaybackStartedTrigger::PlaybackStartedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_started_callback([this]() { this->trigger(); });
}

inline PlaybackFinishedTrigger::PlaybackFinishedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_finished_callback([this]() { this->trigger(); });
}

inline PlaybackPausedTrigger::PlaybackPausedTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_paused_callback([this]() { this->trigger(); });
}

inline PlaybackErrorTrigger::PlaybackErrorTrigger(SimpleVideoPlayer *parent) {
  parent->add_on_error_callback([this](uint8_t error) { this->trigger(error); });
}

}  // namespace esphome::simple_video_player
