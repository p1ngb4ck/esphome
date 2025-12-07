#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/transcoder/transcoder.h"
#include "lvgl.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdio>

#ifdef USE_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#endif

namespace esphome::simple_video_player {

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

// Forward declarations for automation
class SimpleVideoPlayer;

/// Trigger fired when playback starts
class PlaybackStartedTrigger : public Trigger<> {
 public:
  explicit PlaybackStartedTrigger(SimpleVideoPlayer *parent) {
    parent->add_on_started_callback([this]() { this->trigger(); });
  }
};

/// Trigger fired when playback finishes normally
class PlaybackFinishedTrigger : public Trigger<> {
 public:
  explicit PlaybackFinishedTrigger(SimpleVideoPlayer *parent) {
    parent->add_on_finished_callback([this]() { this->trigger(); });
  }
};

/// Trigger fired when playback is paused
class PlaybackPausedTrigger : public Trigger<> {
 public:
  explicit PlaybackPausedTrigger(SimpleVideoPlayer *parent) {
    parent->add_on_paused_callback([this]() { this->trigger(); });
  }
};

/// Trigger fired when playback error occurs
class PlaybackErrorTrigger : public Trigger<PlaybackError> {
 public:
  explicit PlaybackErrorTrigger(SimpleVideoPlayer *parent) {
    parent->add_on_error_callback([this](PlaybackError error) { this->trigger(error); });
  }
};

/// Main video player component
class SimpleVideoPlayer : public Component {
 public:
  SimpleVideoPlayer() = default;
  ~SimpleVideoPlayer() override;

  // Component lifecycle
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  //========================================================================
  // Configuration (called from codegen)
  //========================================================================

  void set_transcoder(transcoder::Transcoder *tc) { this->transcoder_ = tc; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  void set_cache_buffer_size(uint32_t size) { this->cache_buffer_size_ = size; }
  void set_input_buffer_size(uint32_t size) { this->input_buffer_size_ = size; }
  void set_target_fps(float fps) { this->target_fps_ = fps; }

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
  void add_on_error_callback(std::function<void(PlaybackError)> &&callback) {
    this->on_error_callbacks_.add(std::move(callback));
  }

 protected:
  //========================================================================
  // Playback Task
  //========================================================================

  /// FreeRTOS task entry point
  static void playback_task_entry_(void *param);

  /// Main playback loop (runs in task)
  void playback_loop_();

  /// Wait for task to stop
  bool wait_for_task_stop_(uint32_t timeout_ms);

  //========================================================================
  // Frame Processing
  //========================================================================

  /// Read next JPEG frame from file into input buffer
  /// Returns frame size or 0 if EOF, -1 on error
  int read_next_frame_();

  /// Decode JPEG frame and update canvas
  bool decode_frame_(size_t frame_size);

  /// Get video dimensions from first frame
  bool get_video_dimensions_(uint32_t &width, uint32_t &height);

  //========================================================================
  // File I/O Abstraction (supports local and network storage)
  //========================================================================

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
  transcoder::Transcoder *transcoder_{nullptr};
  lv_obj_t *canvas_{nullptr};
  uint32_t cache_buffer_size_{16 * 1024};   // 16KB internal RAM (aligned cache)
  uint32_t input_buffer_size_{256 * 1024};  // 256KB PSRAM (JPEG frame buffer)
  float target_fps_{30.0f};                 // Target frame rate

  // Playback state
  PlayerState state_{PlayerState::STOPPED};
  PlaybackError last_error_{PlaybackError::NONE};
  bool loop_{false};
  std::string video_path_;

  // File handle (polymorphic - local or network)
  FILE *file_handle_{nullptr};
  bool is_network_file_{false};
  uint64_t file_size_{0};
  uint64_t file_position_{0};

  // Cache buffer state (for aligned reads)
  uint64_t cache_buffer_file_pos_{0};  // File position of cached data
  size_t cache_buffer_valid_{0};       // Valid bytes in cache
  size_t cache_buffer_offset_{0};      // Read offset within cache

  // Video properties
  uint32_t video_width_{0};
  uint32_t video_height_{0};

  // Buffers (allocated on demand)
  std::unique_ptr<uint8_t[]> cache_buffer_;   // Internal RAM, aligned for DMA
  std::unique_ptr<uint8_t[]> input_buffer_;   // PSRAM, JPEG encoded frame
  std::unique_ptr<uint8_t[]> output_buffer_;  // PSRAM, decoded RGB565 frame
  size_t output_buffer_size_{0};

  // FreeRTOS task
  TaskHandle_t task_handle_{nullptr};
  SemaphoreHandle_t state_mutex_{nullptr};

  // Automation callbacks
  CallbackManager<void()> on_started_callbacks_;
  CallbackManager<void()> on_finished_callbacks_;
  CallbackManager<void()> on_paused_callbacks_;
  CallbackManager<void(PlaybackError)> on_error_callbacks_;
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

}  // namespace esphome::simple_video_player
