/**
 * @file video_player.h
 * @brief Video player component with H264 decoding and LVGL rendering
 *
 * This component provides video playback functionality for ESP32-P4 using:
 * - MP4 container demuxing
 * - Hardware H264 video decoding (via esp_h264)
 * - LVGL canvas rendering
 * - Optional audio playback (via i2s_audio speaker)
 * - A/V synchronization using PTS timestamps
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/ring_buffer.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/audio/audio_decoder.h"

// Common types used by all demuxers (currently defined in mp4_demuxer.h)
// Include unconditionally since video_player.cpp accesses these struct members
#include "mp4_demuxer.h"

#ifdef USE_MKV_CONTAINER
#include "mkv_demuxer.h"
#endif

#ifdef USE_ESP_H264_DECODER
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_types.h"
#endif

#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace video_player {

/**
 * @brief Playback state enumeration
 */
enum class PlaybackState {
  STOPPED,   // No video loaded or playback stopped
  PLAYING,   // Currently playing
  PAUSED,    // Paused
  FINISHED,  // Reached end of video
};

/**
 * @brief Video player component
 *
 * Manages video playback lifecycle:
 * 1. Load MP4 file and parse container
 * 2. Initialize H264 decoder
 * 3. Decode frames and render to LVGL canvas
 * 4. Handle playback controls (play/pause/stop/seek)
 * 5. Optional audio playback with A/V sync
 */
class VideoPlayer : public Component {
 public:
  VideoPlayer();
  ~VideoPlayer();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ========== Configuration ==========

  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_video_file(const std::string &file) { this->video_file_ = file; }
  void set_auto_play(bool auto_play) { this->auto_play_ = auto_play; }
  void set_loop(bool loop) { this->loop_ = loop; }

  // ========== Playback Control ==========

  /**
   * @brief Start or resume playback
   * @param file Optional file path (if different from configured)
   */
  void play(const std::string &file = "");

  /**
   * @brief Pause playback
   */
  void pause();

  /**
   * @brief Stop playback and reset to beginning
   */
  void stop();

  /**
   * @brief Seek to specific position
   * @param position_ms Position in milliseconds
   */
  void seek(uint32_t position_ms);

  /**
   * @brief Get current playback state
   */
  PlaybackState get_state() const { return this->state_; }

  /**
   * @brief Get current playback position in milliseconds
   */
  uint32_t get_position_ms() const { return this->current_position_ms_; }

  /**
   * @brief Get total video duration in milliseconds
   */
  uint32_t get_duration_ms() const;

  /**
   * @brief Check if video is currently playing
   */
  bool is_playing() const { return this->state_ == PlaybackState::PLAYING; }

  // ========== Automation Triggers ==========

  void add_on_playback_started_callback(std::function<void()> &&callback) {
    this->playback_started_callback_.add(std::move(callback));
  }

  void add_on_playback_paused_callback(std::function<void()> &&callback) {
    this->playback_paused_callback_.add(std::move(callback));
  }

  void add_on_playback_stopped_callback(std::function<void()> &&callback) {
    this->playback_stopped_callback_.add(std::move(callback));
  }

  void add_on_playback_finished_callback(std::function<void()> &&callback) {
    this->playback_finished_callback_.add(std::move(callback));
  }

 protected:
  // Video loading and initialization
  bool load_video_(const std::string &file);
  void unload_video_();
  bool init_decoder_();
  void cleanup_decoder_();

  // Demuxer helpers (handle both MP4 and MKV)
  bool demuxer_is_open_() const;
  void demuxer_close_();
  void demuxer_reset_();
  bool demuxer_seek_video_(uint64_t timestamp_ms);
  bool has_video_() const;
  bool has_audio_() const;
  const VideoTrackInfo *get_video_track_() const;
  const AudioTrackInfo *get_audio_track_() const;
  bool get_next_video_sample_(Sample &sample, uint8_t *data, size_t max_size);
  bool get_next_audio_sample_(Sample &sample, uint8_t *data, size_t max_size);
  uint64_t get_video_duration_ms_() const;
  uint64_t get_audio_duration_ms_() const;

  // Frame processing
  void process_video_frame_();
  bool decode_frame_(const uint8_t *h264_data, size_t h264_size, uint8_t *yuv_output, size_t yuv_size);
  void render_frame_(const uint8_t *yuv_data, uint16_t width, uint16_t height);

  // H264 format conversion (AVCC to Annex-B)
  size_t convert_avcc_to_annexb_(const uint8_t *avcc_data, size_t avcc_size, uint8_t *annexb_data, size_t max_size,
                                 uint8_t nalu_length_size, const std::vector<uint8_t> *sps_data,
                                 const std::vector<uint8_t> *pps_data);

  // Audio processing
  bool init_audio_decoder_();
  void cleanup_audio_decoder_();
  void process_audio_();
  void feed_audio_sample_();

  // YUV to RGB conversion
  void yuv_i420_to_rgb565_(const uint8_t *yuv, uint16_t *rgb, uint16_t width, uint16_t height);

  // Configuration
  lv_obj_t *canvas_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  std::string video_file_;
  bool auto_play_{false};
  bool loop_{false};

  // Playback state
  PlaybackState state_{PlaybackState::STOPPED};
  uint32_t current_position_ms_{0};
  uint32_t last_frame_time_{0};

  // Container demuxers (only one active at a time)
#ifdef USE_MP4_CONTAINER
  std::unique_ptr<MP4Demuxer> mp4_demuxer_;
#endif
#ifdef USE_MKV_CONTAINER
  std::unique_ptr<MKVDemuxer> mkv_demuxer_;
#endif

  // Active demuxer pointer (points to whichever is loaded)
  // We need a way to access common interface - for now use void* and cast
  void *active_demuxer_{nullptr};
  enum class DemuxerType { NONE, MP4, MKV } active_demuxer_type_{DemuxerType::NONE};

  // H264 decoder
#ifdef USE_ESP_H264_DECODER
  esp_h264_dec_handle_t decoder_{nullptr};
#endif

  // Frame buffers (PSRam allocated - ESP32-P4 has 32MB PSRam @ 200MHz, only 512KB internal SRAM)
  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> h264_frame_buffer_;    // Buffer for compressed H264 frame (AVCC)
  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> annexb_frame_buffer_;  // Buffer for Annex-B converted (PSRAM)
  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> yuv_frame_buffer_;     // Buffer for decoded YUV420 frame
  uint16_t *rgb_frame_buffer_{nullptr};  // Buffer for RGB565 frame (PSRAM allocated)
  size_t rgb_frame_buffer_size_{0};      // Size of RGB buffer in uint16_t elements

  // Audio decoder and buffers (PSRam allocated)
  std::unique_ptr<audio::AudioDecoder> audio_decoder_;
  std::shared_ptr<RingBuffer> audio_input_ring_buffer_;                      // Raw FLAC data from MP4
  std::vector<uint8_t, ExternalRAMAllocator<uint8_t>> audio_sample_buffer_;  // Buffer for single audio sample
  uint64_t audio_start_time_ms_{0};  // When audio playback started (for A/V sync)
  uint64_t video_start_time_ms_{0};  // When video playback started (for A/V sync)

  // Decoder state
  bool first_frame_decoded_{false};  // Flag to reset timing after first frame decode

  // Automation callbacks
  CallbackManager<void()> playback_started_callback_;
  CallbackManager<void()> playback_paused_callback_;
  CallbackManager<void()> playback_stopped_callback_;
  CallbackManager<void()> playback_finished_callback_;
};

// ========== Automation Actions ==========

template<typename... Ts> class PlayAction : public Action<Ts...> {
 public:
  explicit PlayAction(VideoPlayer *parent) : parent_(parent) {}

  void set_file(std::function<std::string(Ts...)> file) { this->file_ = file; }

  void play(Ts... x) override {
    if (this->file_.has_value()) {
      this->parent_->play(this->file_.value()(x...));
    } else {
      this->parent_->play();
    }
  }

 protected:
  VideoPlayer *parent_;
  optional<std::function<std::string(Ts...)>> file_;
};

template<typename... Ts> class PauseAction : public Action<Ts...> {
 public:
  explicit PauseAction(VideoPlayer *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->pause(); }

 protected:
  VideoPlayer *parent_;
};

template<typename... Ts> class StopAction : public Action<Ts...> {
 public:
  explicit StopAction(VideoPlayer *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop(); }

 protected:
  VideoPlayer *parent_;
};

template<typename... Ts> class SeekAction : public Action<Ts...> {
 public:
  explicit SeekAction(VideoPlayer *parent) : parent_(parent) {}

  void set_position(std::function<uint32_t(Ts...)> position) { this->position_ = position; }

  void play(Ts... x) override {
    if (this->position_.has_value()) {
      this->parent_->seek(this->position_.value()(x...));
    }
  }

 protected:
  VideoPlayer *parent_;
  optional<std::function<uint32_t(Ts...)>> position_;
};

// ========== Automation Triggers ==========

class PlaybackStartedTrigger : public Trigger<> {
 public:
  explicit PlaybackStartedTrigger(VideoPlayer *parent) {
    parent->add_on_playback_started_callback([this]() { this->trigger(); });
  }
};

class PlaybackPausedTrigger : public Trigger<> {
 public:
  explicit PlaybackPausedTrigger(VideoPlayer *parent) {
    parent->add_on_playback_paused_callback([this]() { this->trigger(); });
  }
};

class PlaybackStoppedTrigger : public Trigger<> {
 public:
  explicit PlaybackStoppedTrigger(VideoPlayer *parent) {
    parent->add_on_playback_stopped_callback([this]() { this->trigger(); });
  }
};

class PlaybackFinishedTrigger : public Trigger<> {
 public:
  explicit PlaybackFinishedTrigger(VideoPlayer *parent) {
    parent->add_on_playback_finished_callback([this]() { this->trigger(); });
  }
};

}  // namespace video_player
}  // namespace esphome
