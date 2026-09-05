#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lvgl/lvgl_esphome.h"

#if defined(USE_HWJPG)
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#elif defined(USE_NEWJPEG)
#include "esp_jpeg_dec.h"
#include "esp_jpeg_common.h"
#else
#include <JPEGDEC.h>
#endif

#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#ifdef USE_AUDIO
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#endif
#include "lvgl.h"
#include "buffered_file_reader.h"
#include "avi_parser.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdio>

#ifdef USE_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#endif

namespace esphome::simple_video_player {

/// Which JPEG backend esp32.require_hw_jpeg() selected for this platform.
enum class JpegBackend {
  HW_P4,     // ESP32-P4 hardware JPEG codec (esp_driver_jpeg)
  NEW_JPEG,  // ESP32-S2/S3 esp_new_jpeg (SIMD-optimized software)
  JPEGDEC,   // Software fallback (bitbank2/JPEGDEC) - other ESP32 variants
};

#if defined(USE_HWJPG)
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::HW_P4;
#elif defined(USE_NEWJPEG)
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::NEW_JPEG;
#else
static constexpr JpegBackend JPEG_BACKEND = JpegBackend::JPEGDEC;
#endif

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

/// One slot of the video frame ring buffer: a compressed JPEG frame, read ahead by the loader
/// task (Core 0) and consumed by the decode/playback task (Core 1). See SimpleVideoPlayer's
/// frame_ring_ for the synchronization contract.
struct VideoFrameSlot {
  enum class Status : uint8_t { FRAME_OK, END_OF_FILE, READ_ERROR };

  std::unique_ptr<uint8_t[]> data;      // PSRAM, input_buffer_size_ bytes
  size_t size{0};                       // valid compressed bytes; meaningful only if FRAME_OK
  Status status{Status::FRAME_OK};
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
  /// Depth of the video frame ring buffer (see frame_ring_). Each slot costs
  /// input_buffer_size_ bytes of PSRAM -- generous by design on P4 (32MB PSRAM).
  void set_prefetch_frames(uint32_t frames) { this->prefetch_frames_ = frames; }

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
  // Playback Task (decode + pacing, Core 1) and Loader Task (I/O + demux, Core 0)
  //
  // The loader task reads ahead into frame_ring_ using the same blocking BufferedFileReader
  // used everywhere else in this component -- blocking is no longer a problem once it's this
  // task's only job, isolated from decode's presentation deadline. The playback task does the
  // one-time setup (open file, probe dimensions, allocate buffers) sequentially, starts the
  // loader, then becomes a pure consumer: wait for a ready ring slot (bounded by one frame's
  // duration), decode, pace, swap, repeat.
  //========================================================================

  /// FreeRTOS task entry point (decode/playback task, pinned to Core 1)
  static void playback_task_entry_(void *param);

  /// Main playback loop (runs in task)
  void playback_loop_();

  /// FreeRTOS task entry point (loader task, pinned to Core 0)
  static void loader_task_entry_(void *param);

  /// Loader loop: demuxes and reads ahead into frame_ring_ until EOF or stop is signaled
  void loader_loop_();

  /// Wait for a task to stop (generic: used for both the playback and loader tasks)
  bool wait_for_task_stop_(TaskHandle_t &handle, uint32_t timeout_ms);

  //========================================================================
  // Video Frame Ring Buffer (see VideoFrameSlot)
  //========================================================================

  /// Allocate frame_ring_ (prefetch_frames_ slots of input_buffer_size_ bytes each)
  bool allocate_frame_ring_();

  /// Free frame_ring_ and its synchronization primitives
  void free_frame_ring_();

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read the next JPEG frame into dest_buffer (capacity dest_capacity)
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity);

  /// Decode JPEG frame (from the ring slot the decode task currently holds) and update canvas
  bool decode_frame_(const uint8_t *frame_data, size_t frame_size);
  // Backend-specific decode, selected at compile time via JPEG_BACKEND (same dispatch pattern
  // as runtime_image/jpeg_decoder.h -- only one explicit specialization is ever defined per
  // build, in simple_video_player.cpp, each behind the #ifdef that also guards its backend's
  // headers above).
  template<JpegBackend Backend> bool decode_frame_backend_(const uint8_t *frame_data, size_t frame_size);
  // Backend-specific decoder/buffer initialization, called once from setup(). Same dispatch
  // pattern as decode_frame_backend_ above.
  template<JpegBackend Backend> bool init_decoder_backend_();
  // Backend-specific header-only parse (width/height, no pixel decode), used by
  // get_video_dimensions_() for the raw-MJPEG case. Same dispatch pattern as above.
  template<JpegBackend Backend>
  bool parse_header_backend_(const uint8_t *buffer, size_t size, uint32_t &width, uint32_t &height);

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

  /// Convert audio channels based on speaker configuration
  /// @param input_data Input PCM audio data
  /// @param output_data Output buffer for converted audio
  /// @param frame_count Number of audio frames to convert
  /// @param input_channels Number of channels in input (e.g., 2 for stereo)
  /// @param output_channels Number of channels in output (e.g., 1 for mono)
  /// @param bits_per_sample Bits per sample (must be 16)
  /// @return true if conversion succeeded, false otherwise
  bool convert_audio_channels_(const uint8_t *input_data, uint8_t *output_data, size_t frame_count,
                               uint8_t input_channels, uint8_t output_channels, uint8_t bits_per_sample);
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

  /// Allocate both canvas_buffer_[2] slots (see header comment there for why two, why
  /// jpeg_alloc_decoder_mem(), and why here instead of setup()) and build their canvas_draw_buf_[2]
  /// wrappers via lv_draw_buf_init(), then attach slot 0 via lv_canvas_set_draw_buf(). Must be
  /// called with lvgl_mutex_ already held. Returns false (and leaves canvas_buffer_ready_ false)
  /// on allocation failure.
  bool setup_canvas_buffer_();

  /// Attach whichever canvas_buffer_ slot decode_frame_backend_ just finished writing into --
  /// the ONLY point that actually touches LVGL for a frame update, deliberately deferred here
  /// (not run immediately after decode) so presentation happens at the paced, precisely-timed
  /// moment the caller computes, not whenever decode happens to finish. Non-blocking try-lock on
  /// lvgl_mutex_ (0 timeout): a miss just skips presenting this one frame (decode_frame_backend_
  /// will overwrite the same undisplayed buffer next cycle), never blocks.
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
  uint32_t input_buffer_size_{256 * 1024};  // 256KB PSRAM (per ring-buffer slot capacity)
  float target_fps_{30.0f};                 // Target frame rate
  static constexpr uint32_t DEFAULT_PREFETCH_FRAMES = 8;
  uint32_t prefetch_frames_{DEFAULT_PREFETCH_FRAMES};  // Depth of frame_ring_

#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};  // Optional speaker for audio playback
  SpeakerChannelMode speaker_channel_mode_{SpeakerChannelMode::SPEAKER_CHANNEL_MONO};  // Channel routing mode
#endif

  // Playback state
  PlayerState state_{PlayerState::STOPPED};
  PlaybackError last_error_{PlaybackError::NONE};
  bool loop_{false};
  std::string video_path_;

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
  bool needs_channel_conversion_{false};          // Whether channel conversion is needed
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

  // Double-buffered decode target == the canvas's own pixel data, no separate scratch buffer and
  // no per-frame copy: the JPEG decoder writes straight into whichever of these two isn't
  // currently attached to LVGL, ping-ponged by present_frame_(). This deliberately reintroduces a
  // direct-into-canvas-memory decode (once reverted for a genuine cross-core race when decode ran
  // on Core 0 against LVGL's render on Core 1 -- see git history on b5d550aa37/ab832e02c9) but
  // that race no longer applies: decode/playback now runs on Core 1, the SAME core as the main
  // loop/lv_timer_handler(), at higher priority, so LVGL's render can only ever run at a point
  // this task actually yields/blocks -- never truly concurrently. The one remaining assumption
  // (deliberate, not an oversight): by the time this task cycles back to decode into a buffer
  // again, LVGL's render+flush of that buffer from ~2 frames ago has long since finished, since
  // decode+audio+prefetch dominates the frame budget far more than LVGL's own render pass does.
  //
  // Allocated via jpeg_alloc_decoder_mem() (not plain heap_caps_malloc/MALLOC_CAP_SPIRAM): the
  // hardware decoder's 2D-DMA output write requires both address and size aligned to its own
  // cache/DMA2D alignment (verified against the real esp_driver_jpeg source, jpeg_common.c) --
  // undersized alignment here is a real way to corrupt adjacent PSRAM, not just a style choice.
  // Fixed at MAX_VIDEO_WIDTH x MAX_VIDEO_HEIGHT: single fixed-resolution panel, set correctly in
  // YAML from the start, no runtime "resize" case. Allocated lazily from play(), on the first
  // play() ever and never again (guarded by canvas_buffer_[0] == nullptr at the call site) --
  // NOT from setup(): this->canvas_ is a widget LVGL itself creates and owns, and component
  // setup() order gives no guarantee LVGL has finished building it by the time this component's
  // own setup() runs.
  uint16_t *canvas_buffer_[2]{nullptr, nullptr};
  // One lv_draw_buf_t wrapper per canvas_buffer_ slot, built once (in setup_canvas_buffer_()) via
  // lv_draw_buf_init() and never touched again except by lv_canvas_set_draw_buf() swapping which
  // one is attached -- the cheap LVGL 9.5 API for this (verified against the real lv_canvas.c
  // source: lv_canvas_set_draw_buf() is a pointer assignment + cache-drop, lv_canvas_set_buffer()
  // recomputes stride and reinitializes the whole draw_buf struct every call).
  lv_draw_buf_t canvas_draw_buf_[2]{};
  int active_display_idx_{0};  // which of canvas_buffer_[2]/canvas_draw_buf_[2] LVGL currently shows
  int canvas_buffer_width_{0};
  int canvas_buffer_height_{0};
  bool canvas_buffer_ready_{false};

#if defined(USE_HWJPG)
  // Created once in init_decoder_backend_<HW_P4>(), reused for every frame's decode_frame_backend_
  // call, destroyed in free_buffers_() -- creating/tearing down the hardware JPEG engine per frame
  // (as opposed to per playback session) is far too expensive to do 25+ times a second.
  jpeg_decoder_handle_t hw_jpeg_decoder_{nullptr};
#elif defined(USE_NEWJPEG)
  // Same reasoning as hw_jpeg_decoder_ above, for esp_new_jpeg's decoder handle.
  jpeg_dec_handle_t new_jpeg_decoder_{nullptr};
#endif

  // Video frame ring buffer -- producer: loader task (Core 0), consumer: playback/decode task
  // (Core 1). ring_head_ is loader-owned (next slot to fill), ring_tail_ is decode-owned (next
  // slot to consume); the two semaphores are the only cross-task synchronization needed for a
  // single-producer/single-consumer ring, no additional locking required.
  std::unique_ptr<VideoFrameSlot[]> frame_ring_;
  uint32_t ring_head_{0};
  uint32_t ring_tail_{0};
  SemaphoreHandle_t ring_slots_free_{nullptr};   // counts empty slots, initial = prefetch_frames_
  SemaphoreHandle_t ring_slots_ready_{nullptr};  // counts filled slots, initial = 0

  // Loader task (Core 0, pure storage I/O -- no DMA2D/PPA/JPEG hardware involved): demuxes and
  // reads ahead into frame_ring_
  TaskHandle_t loader_task_handle_{nullptr};
  volatile bool loader_task_stop_{false};

  // FreeRTOS task (decode/playback, Core 1 -- alongside the main loop/LVGL, see play()'s
  // xTaskCreatePinnedToCore comment for why decode specifically needs to share that core)
  TaskHandle_t task_handle_{nullptr};
  SemaphoreHandle_t state_mutex_{nullptr};
  SemaphoreHandle_t lvgl_mutex_{nullptr};  // Mutex for LVGL thread safety

  // Frame pacing: proper timing for video FPS vs display refresh rate
  int64_t playback_start_time_us_{0};  // Microsecond timestamp when playback started
  uint32_t frame_count_{0};            // Number of frames decoded so far
  float frame_duration_us_{0};         // Duration of one frame in microseconds (1000000/fps)

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
