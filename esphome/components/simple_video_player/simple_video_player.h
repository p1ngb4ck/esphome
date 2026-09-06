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
#endif
// Used for the video frame ring unconditionally (not just USE_AUDIO): ESPHome's own
// ring_buffer::RingBuffer (a thin wrapper over ESP-IDF's native RingbufHandle_t) replaces what
// used to be a hand-rolled VideoFrameSlot[] + two raw counting semaphores here -- see
// video_frame_ring_buffer_'s header comment.
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

#ifdef USE_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
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

// Framing for video_frame_ring_buffer_ (see that member's header comment). A real frame entry is
//   [uint32_t frame_index][uint32_t payload_size][payload_size bytes]
// The absolute frame_index (0-based, assigned by the loader in demux order) lets the consumer
// drop stale frames by tag during an A/V re-sync without any risk of getting out of step with the
// loader. The two sentinel values below are a lone uint32_t with no index and no payload; they are
// picked from the top of the range so a real frame_index can never collide with them.
static constexpr uint32_t VIDEO_FRAME_EOF = 0xFFFFFFFFu;
static constexpr uint32_t VIDEO_FRAME_READ_ERROR = 0xFFFFFFFEu;

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
  /// How much of the COMPRESSED source stream to keep prefetched, as TIME (see
  /// video_frame_ring_buffer_) -- prefetch_frames_ (the byte-budget basis, not a real slot count
  /// any more) is derived from this and target_fps_ once both are known, in setup() (see
  /// allocate_frame_ring_()).
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
  // Playback Task (decode + pacing, Core 1) and Loader Task (I/O + demux, Core 0)
  //
  // The loader task reads ahead into video_frame_ring_buffer_ using the same blocking
  // BufferedFileReader used everywhere else in this component -- blocking is no longer a problem
  // once it's this task's only job, isolated from decode's presentation deadline. The playback
  // task does the one-time setup (open file, probe dimensions, allocate buffers) sequentially,
  // starts the loader, then becomes a pure consumer: pop the next frame (a real, bounded
  // RingBuffer::read() block, not a spin), decode, pace, present, repeat.
  //========================================================================

  /// FreeRTOS task entry point (decode/playback task, pinned to Core 1)
  static void playback_task_entry_(void *param);

  /// Main playback loop (runs in task)
  void playback_loop_();

  /// FreeRTOS task entry point (loader task, pinned to Core 0)
  static void loader_task_entry_(void *param);

  /// Loader loop: demuxes and reads ahead into video_frame_ring_buffer_ until EOF or stop is
  /// signaled
  void loader_loop_();

  /// Wait for a task to stop (generic: used for both the playback and loader tasks)
  bool wait_for_task_stop_(TaskHandle_t &handle, uint32_t timeout_ms);

  /// esp_timer one-shot callback (task-dispatch context): notifies the playback task so its
  /// pacing wait resumes exactly at the armed presentation instant. See the pacing loop in
  /// playback_loop_() for why this replaced a tick-quantised vTaskDelay().
  static void present_timer_cb_(void *arg);

  //========================================================================
  // Video Frame Ring Buffer (see video_frame_ring_buffer_)
  //========================================================================

  /// Allocate video_frame_ring_buffer_ and its two scratch read buffers (loader_read_buffer_,
  /// decode_read_buffer_), sized from prefetch_frames_ * input_buffer_size_.
  bool allocate_frame_ring_();

  /// Free video_frame_ring_buffer_ and its scratch read buffers.
  void free_frame_ring_();

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read the next JPEG frame into dest_buffer (capacity dest_capacity)
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity);

  /// Pop one entry from video_frame_ring_buffer_ (see its framing comment). On a real frame,
  /// writes the payload into dest (capacity dest_cap), sets out_index to its absolute frame index,
  /// decrements frames_in_ring_, and returns the payload size (> 0). Returns 0 for the EOF
  /// sentinel, -1 for the READ_ERROR sentinel, -2 if the wait was aborted because state_ became
  /// STOPPED/ERROR or the payload didn't fit dest_cap. Used by next_frame_to_decode_() and its
  /// re-sync drop path.
  int read_ring_entry_(uint32_t &out_index, uint8_t *dest, size_t dest_cap);

  /// Pop the next frame the pacing loop should DECODE (into decode_read_buffer_), applying A/V
  /// re-sync: if the natural next frame is more than RESYNC_LAG_FRAMES behind the wall-clock media
  /// time, discard intervening frames without decoding and fire the audio-side re-sync (bump
  /// resync_generation_, set audio_skip_until_us_). out_index gets the returned frame's absolute
  /// index. Return codes match read_ring_entry_ (>0 size, 0 EOF, -1 error, -2 aborted).
  int next_frame_to_decode_(uint32_t &out_index);

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
  uint32_t prefetch_duration_ms_{1000};  // How much source stream to keep prefetched, as time
  // video_frame_ring_buffer_'s byte-budget basis -- derived from prefetch_duration_ms_ and
  // target_fps_ once both are known (allocate_frame_ring_(), called from setup()), NOT a user-
  // facing constant: sizing this in frame COUNT made the actual prefetched TIME depend on
  // target_fps_ in a way the YAML option never expressed, so a given prefetch_frames value meant
  // something different at 25fps than at 30fps. Sizing by time and deriving this fixes that. No
  // longer a literal ring slot count since video_frame_ring_buffer_'s buffer is one contiguous
  // byte stream now, not discrete slots -- this * input_buffer_size_ is the ring's allocated size,
  // a worst-case bound real (usually smaller) frames pack into more tightly than that many slots
  // ever could.
  uint32_t prefetch_frames_{0};

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

  // Decode target == the canvas's OWN existing pixel buffer, in place. This is NOT allocated by
  // this component at all: LVGL's own canvas widget codegen
  // (esphome/components/lvgl/widgets/canvas.py) already built one lv_draw_buf_t, sized exactly to
  // the YAML-declared width/height, via lv_expr.malloc_core() + lv_draw_buf_init(), and attached it
  // with lv_canvas_set_draw_buf() -- as generated top-level code that runs before ANY
  // Component::setup() (verified against esphome/writer.py's generated main.cpp), so it already
  // exists by the time even THIS component's own setup() runs, let alone play(). setup() fetches
  // it there (attach_canvas_buffer_() only READS the pointer via lv_canvas_get_draw_buf(), never
  // allocates or replaces it) and blanks it immediately, since lv_malloc_core() doesn't zero its
  // memory -- left alone, the canvas would show leftover PSRAM garbage from boot until whenever
  // play() first runs.
  //
  // A second, component-owned decode buffer with lv_canvas_set_draw_buf() ping-ponging between the
  // two was tried this session and rejected: swapping which lv_draw_buf_t is attached tears down
  // the canvas's existing attachment and breaks rendering outright. The buffering this player
  // actually needs is upstream, on the COMPRESSED source bytes feeding the decoder (see
  // video_frame_ring_buffer_), not on the decoded pixel output -- decoding directly, in place, into the one
  // buffer LVGL already owns and renders from is both simpler and the only approach that doesn't
  // fight LVGL's own buffer management. This does mean decode and LVGL's render are touching the
  // same memory without a lock between them; that's accepted the same way the old reference
  // implementation accepted it (see git history) -- decode/playback runs on Core 1, the SAME core
  // as the main loop/lv_timer_handler(), at higher priority, so LVGL's render can only ever run at
  // a point this task actually yields/blocks, never truly concurrently with a decode write.
  //
  // Fixed at MAX_VIDEO_WIDTH x MAX_VIDEO_HEIGHT: single fixed-resolution panel, set correctly in
  // YAML from the start, no runtime "resize" case -- attach_canvas_buffer_() validates the actual
  // buffer it finds is within these bounds, it does not derive them.
  lv_draw_buf_t *canvas_draw_buf_{nullptr};  // owned by LVGL; never allocated or freed by us
  uint16_t *canvas_buffer_{nullptr};         // == canvas_draw_buf_->data, cached for convenience
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
  // (Core 1). This is esphome::ring_buffer::RingBuffer (a thin wrapper over ESP-IDF's native
  // RingbufHandle_t, RINGBUF_TYPE_BYTEBUF -- verified against the real ring_buffer.cpp/.h in this
  // repo, the SAME class already used a few members down for audio), NOT a hand-rolled slot array:
  // a previous version of this file reinvented a ring buffer from a VideoFrameSlot[] plus two raw
  // FreeRTOS counting semaphores and manual head/tail indices -- upstream already solves exactly
  // this, correctly, and this component already depends on it for audio, so there was never a
  // reason to hand-roll a second implementation for video.
  //
  // Framing: RingBuffer is a plain byte stream with no item-boundary concept of its own, so each
  // variable-length compressed frame is pushed as [4-byte uint32_t size][that many payload bytes]
  // (VIDEO_FRAME_EOF/VIDEO_FRAME_READ_ERROR are the two header-only sentinel values, no payload
  // follows them). Sized at allocate_frame_ring_() time from prefetch_frames_ * input_buffer_size_
  // bytes -- a worst-case bound (every frame at max size), so real (usually much smaller) frames
  // pack in more tightly than that many discrete slots ever could.
  //
  // Both write_without_replacement() and read() take a REAL FreeRTOS tick timeout and block
  // properly (verified against the real ring_buffer.cpp -- xRingbufferSend()/xRingbufferReceiveUpTo()
  // underneath), which is exactly the blocking primitive this component's own pacing loop had to
  // learn to rely on this session (vTaskDelay(), never taskYIELD(), to actually let a
  // lower-priority task run) -- no separate signaling semaphore needed on top of it.
  std::shared_ptr<ring_buffer::RingBuffer> video_frame_ring_buffer_;
  // Loader task's own scratch buffer: read_next_frame_() writes into this (PSRAM,
  // input_buffer_size_ bytes), then the loader copies it into video_frame_ring_buffer_ --
  // RingBuffer's write() API only ever copies FROM a caller-supplied buffer, it has no "give me a
  // pointer to fill" mode the way the old per-slot array did.
  std::unique_ptr<uint8_t[]> loader_read_buffer_;
  // Decode/playback task's own scratch buffer: popped out of video_frame_ring_buffer_ via
  // RingBuffer::read() (PSRAM, input_buffer_size_ bytes), then decode_frame_() reads from this.
  std::unique_ptr<uint8_t[]> decode_read_buffer_;
  // How many complete frames are sitting in video_frame_ring_buffer_ right now, real frames only
  // (EOF/error sentinels never counted) -- RingBuffer has no notion of "frame count", only bytes,
  // so this is tracked separately, purely for the startup pre-buffering heuristic (see
  // playback_loop_()) and logging. std::atomic, not a plain uint32_t: incremented by the loader
  // task, decremented by the playback task, from different cores.
  std::atomic<uint32_t> frames_in_ring_{0};

  // Loader task (Core 0, pure storage I/O -- no DMA2D/PPA/JPEG hardware involved): demuxes and
  // reads ahead into video_frame_ring_buffer_
  TaskHandle_t loader_task_handle_{nullptr};
  volatile bool loader_task_stop_{false};

  // FreeRTOS task (decode/playback, Core 1 -- alongside the main loop/LVGL, see play()'s
  // xTaskCreatePinnedToCore comment for why decode specifically needs to share that core)
  TaskHandle_t task_handle_{nullptr};

  // One-shot high-resolution timer used to wake the playback task at the exact presentation
  // instant (see the pacing loop in playback_loop_()). Created once in setup(), re-armed per
  // frame with esp_timer_start_once(), never recreated. systimer-backed: microsecond resolution,
  // no FreeRTOS-tick quantisation.
  esp_timer_handle_t present_timer_{nullptr};

  // Frame pacing. The wall clock is the master timeline: media_us = esp_timer_get_time() -
  // playback_start_time_us_ - paused_accum_us_. Video is paced to it (see playback_loop_()); audio
  // free-runs on the I2S clock. Neither stream ever waits on the other -- when one falls too far
  // behind media_us the pace controller fires a single A/V re-sync (drop stale video without
  // decoding it, skip audio forward to the same media time, flush the audio queues) instead of
  // stalling or drifting.
  int64_t playback_start_time_us_{0};  // esp_timer_get_time() at frame 0
  int64_t paused_accum_us_{0};         // total wall time spent PAUSED, excluded from media_us
  uint32_t frame_count_{0};            // absolute index of the next frame to present
  float frame_duration_us_{0};         // duration of one frame in microseconds (1000000/fps)

  // A/V re-sync coordination.
  //   resync_generation_ : bumped by the pace controller once per lag episode; the audio task
  //                        watches it and, on a change, flushes audio_input_ring_buffer_ /
  //                        audio_decoded_ring_buffer_ (the stale audio queued before the skip).
  //   audio_skip_until_us_ : the loader discards demuxed audio chunks (still counting their bytes)
  //                          until audio_bytes_demuxed_ corresponds to at least this media time,
  //                          then resumes feeding process_audio_frame_().
  //   audio_bytes_demuxed_ : running total of audio payload bytes the loader has pulled from the
  //                          file (fed or skipped); divided by the fixed bytes-per-second it gives
  //                          the audio stream's media time.
  std::atomic<uint32_t> resync_generation_{0};
  std::atomic<int64_t> audio_skip_until_us_{0};
  std::atomic<uint64_t> audio_bytes_demuxed_{0};
  // Playback task only: true while a lag episode is being ridden out, so the audio-queue flush
  // (resync_generation_ bump) fires once at the start of the episode, not once per dropped frame.
  bool resync_active_{false};
  // Playback task only, plain counters (no logging on the priority-10 path): number of re-sync
  // episodes this session, total frames discarded across them, and decode failures skipped.
  // Summarised in one line after the playback loop exits.
  uint32_t resync_count_{0};
  uint32_t resync_frames_dropped_{0};
  uint32_t decode_fail_count_{0};
  // How far behind the wall-clock media time the current frame may fall before the pace controller
  // stops walking frame-by-frame and drops straight to the live edge.
  static constexpr uint32_t RESYNC_LAG_FRAMES = 4;
  // Hard cap on frames discarded in one re-sync, so a case where delivery itself is slower than
  // real time degrades to a low frame rate instead of an unbounded drain loop.
  static constexpr uint32_t RESYNC_MAX_DROP = 240;

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
