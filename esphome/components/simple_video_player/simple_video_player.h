#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lvgl/lvgl_esphome.h"

#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"

// ESP32-P4 hardware JPEG decoder only -- other variants cannot decode fast enough for video.

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#ifdef USE_AUDIO
#include "esphome/components/audio/audio_decoder.h"
#endif
// ring_buffer::RingBuffer is used for the audio input/decoded rings (see USE_AUDIO members).
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "lvgl.h"
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

/// Main video player component
class SimpleVideoPlayer : public Component {
 public:
  explicit SimpleVideoPlayer(lvgl::LvglComponent *lvgl_component) { this->lvgl_component_ = lvgl_component; }
  ~SimpleVideoPlayer();

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  //========================================================================
  // Configuration (called from codegen)
  //========================================================================

  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  void set_cache_buffer_size(uint32_t size) { this->cache_buffer_size_ = size; }
  void set_input_buffer_size(uint32_t size) { this->input_buffer_size_ = size; }
  void set_target_fps(float fps) { this->target_fps_ = fps; }
  void set_prefetch_duration_ms(uint32_t ms) { this->prefetch_duration_ms_ = ms; }

#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_speaker_channel_mode(SpeakerChannelMode mode) { this->speaker_channel_mode_ = mode; }
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

  /// Wait for a task to stop
  bool wait_for_task_stop_(TaskHandle_t &handle, uint32_t timeout_ms);

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read the next JPEG frame into dest_buffer (capacity dest_capacity)
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity);

  /// read_next_frame_() into decode_read_buffer_, with loop rewind and a stop check.
  /// Returns payload size (> 0), 0 at EOF, -1 on read error, -2 if stopped/aborted.
  int read_frame_();

  /// Decode JPEG frame (from the ring slot the decode task currently holds) and update canvas
  bool decode_frame_(const uint8_t *frame_data, size_t frame_size);
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

  /// Fetch the lv_draw_buf_t* LVGL's OWN canvas codegen already built and attached (see
  /// canvas_buffer_'s header comment) -- allocates nothing, just reads the pointer/size/format out
  /// of the widget and validates them. Returns false (and leaves canvas_buffer_ready_ false) if the
  /// canvas has no buffer yet or it's not sane.
  bool attach_canvas_buffer_();

  /// Flush the CPU cache for canvas_buffer_ (decode writes it directly, in place -- see that
  /// member's header comment) and invalidate the canvas so LVGL redraws it -- the ONLY point that
  /// actually touches LVGL for a frame update, deliberately deferred here (not run immediately
  /// after decode) so presentation happens at the paced, precisely-timed moment the caller
  /// computes, not whenever decode happens to finish. No lock: this runs on the decode/playback
  /// task (Core 1, priority 10), the same core as the main loop / lv_timer_handler() but at higher
  /// priority, so LVGL's render can only run while this task is blocked -- never concurrently with
  /// a decode write or this invalidate (see canvas_buffer_'s header comment).
  void present_frame_();

  //========================================================================
  // Error Handling
  //========================================================================

  void set_error_(PlaybackError error);

  //========================================================================
  // Members
  //========================================================================

  // Configuration
  lvgl::LvglComponent *lvgl_component_{nullptr};  // Parent LVGL component (required at construction; not otherwise used)
  lv_obj_t *canvas_{nullptr};
  uint32_t cache_buffer_size_{16 * 1024};   // 16KB internal RAM (aligned cache)
  uint32_t input_buffer_size_{256 * 1024};  // 256KB PSRAM (worst-case single compressed frame size)
  float target_fps_{30.0f};                 // Target frame rate
  uint32_t prefetch_duration_ms_{1000};  // accepted for config compat; read-ahead is the transfer-buffer arena

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
  static constexpr size_t AUDIO_INPUT_BUFFER_SIZE =
      (AUDIO_BYTES_PER_SEC * 250 / 1000) > (32 * 1024) ? (AUDIO_BYTES_PER_SEC * 250 / 1000) : (32 * 1024);
  static constexpr size_t AUDIO_DECODED_BUFFER_SIZE =
      (AUDIO_BYTES_PER_SEC * 500 / 1000) > (16 * 1024) ? (AUDIO_BYTES_PER_SEC * 500 / 1000) : (16 * 1024);
  static constexpr size_t AUDIO_TEMP_BUFFER_SIZE =
      (AUDIO_BYTES_PER_SEC * 100 / 1000) > (8 * 1024) ? (AUDIO_BYTES_PER_SEC * 100 / 1000) : (8 * 1024);
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
  std::unique_ptr<audio::AudioDecoder> audio_decoder_;   // Audio decoder (MP3/FLAC/PCM)
  std::shared_ptr<ring_buffer::RingBuffer> audio_input_ring_buffer_;  // Ring buffer for encoded audio (in PSRAM)
  std::shared_ptr<ring_buffer::RingBuffer>
      audio_decoded_ring_buffer_;                 // Ring buffer for decoded audio (in PSRAM, before conversion)
  std::unique_ptr<uint8_t[]> audio_temp_buffer_;  // Temporary buffer for audio processing (in PSRAM)
  uint8_t source_audio_channels_{0};              // Fixed source channel count (mirrors AUDIO_SOURCE_CHANNELS)
  uint8_t speaker_audio_channels_{1};             // Number of channels speaker expects
  uint32_t audio_sample_rate_{0};                 // Fixed sample rate (mirrors AUDIO_SAMPLE_RATE)
  uint8_t audio_bits_per_sample_{16};             // Fixed bits per sample (mirrors AUDIO_BITS_PER_SAMPLE)
  bool audio_enabled_{false};                     // Audio stream available and enabled
  TaskHandle_t audio_task_handle_{nullptr};       // Audio processing task (runs on Core 0)
  volatile bool audio_task_stop_{false};          // Signal to stop audio task
#endif

  // Cache buffer state (for frame parsing - only used for raw MJPEG)
  size_t cache_buffer_valid_{0};   // Valid bytes in cache
  size_t cache_buffer_offset_{0};  // Read offset within cache

  // Video properties
  uint32_t video_width_{0};
  uint32_t video_height_{0};

  // Buffers (allocated on demand)
  std::unique_ptr<uint8_t[]> cache_buffer_;  // Internal RAM (16KB), aligned for DMA

  // Decode target == the canvas's OWN existing pixel buffer, in place. LVGL's canvas codegen
  // (esphome/components/lvgl/widgets/canvas.py) built one lv_draw_buf_t before any
  // Component::setup(); attach_canvas_buffer_() only reads the pointer back via
  // lv_canvas_get_draw_buf(), never allocates or replaces it, and blanks it once at setup. Decode
  // writes straight into it -- decode/playback runs on Core 1 above the main loop /
  // lv_timer_handler(), so LVGL's render only runs when this task blocks, never concurrently with
  // a decode write. No component-owned second buffer.
  lv_draw_buf_t *canvas_draw_buf_{nullptr};  // owned by LVGL; never allocated or freed by us
  uint16_t *canvas_buffer_{nullptr};         // == canvas_draw_buf_->data at setup, cached
  int canvas_buffer_width_{0};
  int canvas_buffer_height_{0};
  bool canvas_buffer_ready_{false};

  // Double buffering (kills tearing): back_buffer_ is a byte-for-byte copy of the canvas draw buf,
  // allocated once in setup() (size = canvas_draw_buf_->data_size verbatim, not computed).
  // decode_frame_() writes decode_target_ (the off-screen one); present_frame_() points
  // canvas_draw_buf_->data at it (pointer swap only, as youkorr's lvgl_camera_display) so LVGL
  // never renders a buffer a decode is writing.
  uint16_t *back_buffer_{nullptr};
  uint16_t *decode_target_{nullptr};

  // Set by present_frame_() (or the stop-blank) once canvas_buffer_ holds a new frame and its
  // cache is synced; consumed by loop() on the LVGL thread for the one lv_obj_invalidate().
  std::atomic<bool> frame_ready_{false};

  jpeg_decoder_handle_t hw_jpeg_decoder_{nullptr};

  // The playback task reads each next compressed frame here (jpeg_alloc_decoder_mem INPUT buffer,
  // input_buffer_size_ bytes); decode_frame_() feeds it to the HW decoder. read_next_frame_()
  // handles file-I/O read-ahead itself via BufferedFileReader (file_reader_).
  std::unique_ptr<uint8_t[]> decode_read_buffer_;
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

  // Playback-task counter, summarised once after the loop (no logging on the pacing path).
  uint32_t decode_fail_count_{0};

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

  void play(Ts... x) override {
    auto path = this->path_.value(x...);
    this->player_->play(path);
  }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class PauseAction : public Action<Ts...> {
 public:
  explicit PauseAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(Ts... x) override { this->player_->pause(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class ResumeAction : public Action<Ts...> {
 public:
  explicit ResumeAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(Ts... x) override { this->player_->resume(); }

 protected:
  SimpleVideoPlayer *player_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(SimpleVideoPlayer *player) : player_(player) {}

  void play(Ts... x) override { this->player_->stop(); }

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
