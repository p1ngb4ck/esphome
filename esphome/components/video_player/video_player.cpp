/**
 * @file video_player.cpp
 * @brief Video player component implementation
 */

#include "video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>

#ifdef ESP32
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace video_player {

static const char *const TAG = "video_player";

// Maximum frame buffer sizes
static constexpr size_t MAX_H264_FRAME_SIZE = 256 * 1024;          // 256KB per frame
static constexpr size_t MAX_YUV_FRAME_SIZE = 1920 * 1080 * 3 / 2;  // 1080p I420

VideoPlayer::VideoPlayer() {}

VideoPlayer::~VideoPlayer() {
  this->cleanup_decoder_();
  this->cleanup_audio_decoder_();
  this->unload_video_();

  // Free RGB frame buffer
  if (this->rgb_frame_buffer_ != nullptr) {
    free(this->rgb_frame_buffer_);
    this->rgb_frame_buffer_ = nullptr;
  }
}

void VideoPlayer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up video player...");

  // Canvas is now optional (for audio-only playback)
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "Canvas not configured - audio-only playback mode");
  } else {
    // Allocate video frame buffers only if canvas exists
    this->h264_frame_buffer_.resize(MAX_H264_FRAME_SIZE);
    this->yuv_frame_buffer_.resize(MAX_YUV_FRAME_SIZE);
    ESP_LOGCONFIG(TAG, "Video frame buffers allocated");
  }

  // Auto-play if configured
  if (this->auto_play_ && !this->video_file_.empty()) {
    this->defer([this]() { this->play(); });
  }
}

void VideoPlayer::loop() {
  if (this->state_ != PlaybackState::PLAYING) {
    return;
  }

  // Process audio (if enabled)
  this->process_audio_();

  // Process next video frame (only if video track exists)
  if (this->demuxer_is_open_() && this->has_video_()) {
    this->process_video_frame_();
  }
}

void VideoPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Video Player:");
  ESP_LOGCONFIG(TAG, "  Video file: %s", this->video_file_.c_str());
  ESP_LOGCONFIG(TAG, "  Auto-play: %s", YESNO(this->auto_play_));
  ESP_LOGCONFIG(TAG, "  Loop: %s", YESNO(this->loop_));

  if (this->demuxer_is_open_() && this->demuxer_is_open_()) {
    const VideoTrackInfo *video = this->get_video_track_();
    if (video != nullptr) {
      ESP_LOGCONFIG(TAG, "  Resolution: %ux%u", video->width, video->height);
      ESP_LOGCONFIG(TAG, "  Duration: %.2f seconds", static_cast<float>(this->get_duration_ms()) / 1000.0f);
      ESP_LOGCONFIG(TAG, "  Frame count: %u", video->sample_count);
    }
  }
}

// ========== Playback Control ==========

void VideoPlayer::play(const std::string &file) {
  // If file specified, load it
  if (!file.empty()) {
    if (!this->load_video_(file)) {
      ESP_LOGE(TAG, "Failed to load video: %s", file.c_str());
      return;
    }
  }

  // If no video loaded, try configured file
  if (!this->demuxer_is_open_()) {
    if (this->video_file_.empty()) {
      ESP_LOGE(TAG, "No video file configured");
      return;
    }
    if (!this->load_video_(this->video_file_)) {
      ESP_LOGE(TAG, "Failed to load video: %s", this->video_file_.c_str());
      return;
    }
  }

  // Initialize decoder if not already done
#ifdef USE_ESP_H264_DECODER
  if (this->decoder_ == nullptr) {
    if (!this->init_decoder_()) {
      ESP_LOGE(TAG, "Failed to initialize H264 decoder");
      return;
    }
  }
#endif

  // Initialize audio decoder if audio track exists and speaker configured
  if (this->has_audio_() && this->speaker_ != nullptr && !this->audio_decoder_) {
    if (!this->init_audio_decoder_()) {
      ESP_LOGW(TAG, "Failed to initialize audio decoder - continuing without audio");
    }
  }

  // Resume from pause or start from beginning
  if (this->state_ == PlaybackState::PAUSED) {
    ESP_LOGI(TAG, "Resuming playback");
  } else {
    ESP_LOGI(TAG, "Starting playback");
    this->demuxer_reset_();
    this->current_position_ms_ = 0;
    this->video_start_time_ms_ = millis();
    this->audio_start_time_ms_ = millis();
  }

  this->state_ = PlaybackState::PLAYING;
  this->last_frame_time_ = millis();
  this->playback_started_callback_.call();
}

void VideoPlayer::pause() {
  if (this->state_ != PlaybackState::PLAYING) {
    return;
  }

  ESP_LOGI(TAG, "Pausing playback");
  this->state_ = PlaybackState::PAUSED;
  this->playback_paused_callback_.call();
}

void VideoPlayer::stop() {
  if (this->state_ == PlaybackState::STOPPED) {
    return;
  }

  ESP_LOGI(TAG, "Stopping playback");
  this->state_ = PlaybackState::STOPPED;
  this->current_position_ms_ = 0;

  if (this->demuxer_is_open_()) {
    this->demuxer_reset_();
  }

  this->playback_stopped_callback_.call();
}

void VideoPlayer::seek(uint32_t position_ms) {
  if (!this->demuxer_is_open_()) {
    ESP_LOGW(TAG, "Cannot seek: no video loaded");
    return;
  }

  ESP_LOGI(TAG, "Seeking to position %u ms", position_ms);
  this->demuxer_seek_video_(position_ms);
  this->current_position_ms_ = position_ms;
}

uint32_t VideoPlayer::get_duration_ms() const {
  if (this->demuxer_is_open_() && this->demuxer_is_open_()) {
    return this->get_video_duration_ms_();
  }
  return 0;
}

// ========== Video Loading ==========

bool VideoPlayer::load_video_(const std::string &file) {
  // Unload any existing video
  this->unload_video_();

  ESP_LOGI(TAG, "Loading media file: %s", file.c_str());

  // Detect container from file extension
  std::string ext = file.substr(file.find_last_of(".") + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

#ifdef USE_MP4_CONTAINER
  if (ext == "mp4" || ext == "m4a" || ext == "m4v") {
    this->mp4_demuxer_ = std::make_unique<MP4Demuxer>();
    if (!this->mp4_demuxer_->open(file)) {
      ESP_LOGE(TAG, "Failed to open MP4 file: %s", file.c_str());
      this->mp4_demuxer_.reset();
      return false;
    }
    this->active_demuxer_ = this->mp4_demuxer_.get();
    this->active_demuxer_type_ = DemuxerType::MP4;
    ESP_LOGI(TAG, "Loaded MP4 container");
  } else
#endif
#ifdef USE_MKV_CONTAINER
      if (ext == "mkv" || ext == "mka" || ext == "webm") {
    this->mkv_demuxer_ = std::make_unique<MKVDemuxer>();
    if (!this->mkv_demuxer_->open(file)) {
      ESP_LOGE(TAG, "Failed to open MKV file: %s", file.c_str());
      this->mkv_demuxer_.reset();
      return false;
    }
    this->active_demuxer_ = this->mkv_demuxer_.get();
    this->active_demuxer_type_ = DemuxerType::MKV;
    ESP_LOGI(TAG, "Loaded MKV container");
  } else
#endif
  {
    ESP_LOGE(TAG, "Unsupported file format: %s (extension: .%s)", file.c_str(), ext.c_str());
    return false;
  }

  // Check if file has at least video or audio
  if (!this->has_video_() && !this->has_audio_()) {
    ESP_LOGE(TAG, "File has neither video nor audio tracks");
    this->unload_video_();
    return false;
  }

  // Handle video track if present
  if (this->has_video_()) {
    const VideoTrackInfo *video = this->get_video_track_();
    ESP_LOGI(TAG, "Video track: %ux%u, %u frames, %.2f seconds", video->width, video->height, video->sample_count,
             static_cast<float>(this->get_video_duration_ms_()) / 1000.0f);

    // Allocate RGB frame buffer from PSRAM based on video dimensions
    if (this->canvas_ != nullptr) {
      size_t rgb_size = video->width * video->height;
      if (this->rgb_frame_buffer_ != nullptr) {
        free(this->rgb_frame_buffer_);
      }

#ifdef ESP32
      this->rgb_frame_buffer_ = (uint16_t *) heap_caps_malloc(rgb_size * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
      if (this->rgb_frame_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer from PSRAM, trying internal RAM");
        this->rgb_frame_buffer_ = (uint16_t *) malloc(rgb_size * sizeof(uint16_t));
      }
#else
      this->rgb_frame_buffer_ = (uint16_t *) malloc(rgb_size * sizeof(uint16_t));
#endif

      if (this->rgb_frame_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer (%zu bytes)", rgb_size * sizeof(uint16_t));
        this->unload_video_();
        return false;
      }

      this->rgb_frame_buffer_size_ = rgb_size;
      ESP_LOGI(TAG, "Allocated RGB buffer: %zu bytes from %s", rgb_size * sizeof(uint16_t),
               heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM" : "internal RAM");
    } else {
      ESP_LOGW(TAG, "Video track present but canvas not configured - video will not be rendered");
    }
  } else {
    ESP_LOGI(TAG, "Audio-only file (no video track)");
  }

  // Handle audio track if present
  if (this->has_audio_()) {
    const AudioTrackInfo *audio = this->get_audio_track_();
    const char *codec_name = "unknown";
    if (audio->codec_type == AudioCodecType::AAC)
      codec_name = "AAC";
    else if (audio->codec_type == AudioCodecType::MP3)
      codec_name = "MP3";
    else if (audio->codec_type == AudioCodecType::FLAC)
      codec_name = "FLAC";

    ESP_LOGI(TAG, "Audio track: %s, %u Hz, %u channels", codec_name, audio->sample_rate, audio->channels);
  }

  return true;
}

void VideoPlayer::unload_video_() {
  this->stop();
  this->cleanup_decoder_();

  if (this->demuxer_is_open_()) {
    this->demuxer_close_();
    this->unload_video_();
  }
}

// ========== Decoder Management ==========

bool VideoPlayer::init_decoder_() {
  // Skip decoder initialization if no video track (audio-only playback)
  if (!this->demuxer_is_open_() || !this->has_video_()) {
    ESP_LOGD(TAG, "No video track - skipping H264 decoder initialization");
    return true;  // Not an error - just audio-only mode
  }

#ifdef USE_ESP_H264_DECODER
  ESP_LOGI(TAG, "Initializing H264 decoder...");

  // Configure decoder for I420 output (compatible with our YUV buffer)
  esp_h264_dec_cfg_sw_t cfg = {};
  cfg.pic_type = ESP_H264_RAW_FMT_I420;

  esp_h264_err_t ret = esp_h264_dec_sw_new(&cfg, &this->decoder_);
  if (ret != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create H264 decoder: %d", ret);
    return false;
  }

  ret = esp_h264_dec_open(this->decoder_);
  if (ret != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open H264 decoder: %d", ret);
    esp_h264_dec_del(this->decoder_);
    this->decoder_ = nullptr;
    return false;
  }

  ESP_LOGI(TAG, "H264 decoder initialized successfully");
  return true;
#else
  ESP_LOGE(TAG, "H264 decoder not available (USE_ESP_H264_DECODER not defined)");
  return false;
#endif
}

void VideoPlayer::cleanup_decoder_() {
#ifdef USE_ESP_H264_DECODER
  if (this->decoder_ != nullptr) {
    esp_h264_dec_close(this->decoder_);
    esp_h264_dec_del(this->decoder_);
    this->decoder_ = nullptr;
  }
#endif
}

// ========== Frame Processing ==========

void VideoPlayer::process_video_frame_() {
  if (!this->demuxer_is_open_()) {
    this->stop();
    return;
  }

  const VideoTrackInfo *video = this->get_video_track_();
  if (video == nullptr) {
    this->stop();
    return;
  }

  // Get next video sample (H264 frame)
  Sample sample;
  if (!this->get_next_video_sample_(sample, this->h264_frame_buffer_.data(), this->h264_frame_buffer_.size())) {
    // End of video
    ESP_LOGI(TAG, "Reached end of video");

    if (this->loop_) {
      ESP_LOGI(TAG, "Looping video");
      this->demuxer_reset_();
      this->current_position_ms_ = 0;
      this->video_start_time_ms_ = millis();
      this->audio_start_time_ms_ = millis();
      return;
    } else {
      this->state_ = PlaybackState::FINISHED;
      this->playback_finished_callback_.call();
      return;
    }
  }

  // A/V Synchronization: Check if it's time to display this frame
  uint32_t now = millis();
  uint32_t elapsed_playback_ms = now - this->video_start_time_ms_;
  uint32_t frame_pts_ms = sample.timestamp_ms;

  // If frame PTS is ahead of playback time, wait
  if (frame_pts_ms > elapsed_playback_ms) {
    uint32_t wait_time_ms = frame_pts_ms - elapsed_playback_ms;
    if (wait_time_ms > 100) {
      // If we're very early, skip this loop iteration
      return;
    }
    // Otherwise, small wait is acceptable (frame will be slightly early)
  }

  // If frame PTS is significantly behind playback time, skip frame (catch up)
  if (elapsed_playback_ms > frame_pts_ms + 100) {
    ESP_LOGD(TAG, "Skipping late frame: PTS=%u ms, elapsed=%u ms", frame_pts_ms, elapsed_playback_ms);
    return;  // Drop frame and get next one
  }

  // Decode H264 frame to YUV
  if (!this->decode_frame_(this->h264_frame_buffer_.data(), sample.size, this->yuv_frame_buffer_.data(),
                           this->yuv_frame_buffer_.size())) {
    ESP_LOGW(TAG, "Failed to decode frame at position %u ms", sample.timestamp_ms);
    return;
  }

  // Render frame to LVGL canvas
  this->render_frame_(this->yuv_frame_buffer_.data(), video->width, video->height);

  // Update playback position and timing
  this->current_position_ms_ = sample.timestamp_ms;
  this->last_frame_time_ = now;
}

bool VideoPlayer::decode_frame_(const uint8_t *h264_data, size_t h264_size, uint8_t *yuv_output, size_t yuv_size) {
#ifdef USE_ESP_H264_DECODER
  if (this->decoder_ == nullptr) {
    return false;
  }

  esp_h264_dec_in_frame_t in_frame = {};
  in_frame.raw_data.buffer = const_cast<uint8_t *>(h264_data);
  in_frame.raw_data.len = h264_size;

  esp_h264_dec_out_frame_t out_frame = {};

  // Decode frame (may need multiple calls for complete frame)
  while (in_frame.raw_data.len > 0) {
    esp_h264_err_t ret = esp_h264_dec_process(this->decoder_, &in_frame, &out_frame);

    if (ret != ESP_H264_ERR_OK) {
      ESP_LOGW(TAG, "H264 decode error: %d", ret);
      return false;
    }

    // Update input buffer position
    in_frame.raw_data.buffer += in_frame.consume;
    in_frame.raw_data.len -= in_frame.consume;

    // If we got decoded data, copy it to output buffer
    if (out_frame.out_size > 0) {
      if (out_frame.out_size > yuv_size) {
        ESP_LOGE(TAG, "Decoded frame size (%u) exceeds buffer size (%zu)", out_frame.out_size, yuv_size);
        return false;
      }
      memcpy(yuv_output, out_frame.outbuf, out_frame.out_size);
      return true;
    }
  }

  // No output data (might be SPS/PPS or incomplete frame)
  return false;
#else
  return false;
#endif
}

void VideoPlayer::render_frame_(const uint8_t *yuv_data, uint16_t width, uint16_t height) {
  if (this->canvas_ == nullptr || this->rgb_frame_buffer_ == nullptr) {
    return;
  }

  // Convert YUV I420 to RGB565
  this->yuv_i420_to_rgb565_(yuv_data, this->rgb_frame_buffer_, width, height);

  // Set canvas buffer with converted RGB565 data
  // LV_IMG_CF_TRUE_COLOR is RGB565 format
  lv_canvas_set_buffer(this->canvas_, (void *) this->rgb_frame_buffer_, width, height, LV_IMG_CF_TRUE_COLOR);

  // Invalidate canvas to trigger redraw
  lv_obj_invalidate(this->canvas_);
}

// ========== Color Conversion ==========

void VideoPlayer::yuv_i420_to_rgb565_(const uint8_t *yuv, uint16_t *rgb, uint16_t width, uint16_t height) {
  // YUV I420 format:
  // - Y plane: width * height bytes
  // - U plane: (width/2) * (height/2) bytes
  // - V plane: (width/2) * (height/2) bytes

  const uint8_t *y_plane = yuv;
  const uint8_t *u_plane = yuv + (width * height);
  const uint8_t *v_plane = u_plane + ((width * height) / 4);

  for (uint16_t row = 0; row < height; row++) {
    for (uint16_t col = 0; col < width; col++) {
      // Get Y value (full resolution)
      uint8_t y = y_plane[row * width + col];

      // Get U and V values (half resolution)
      uint8_t u = u_plane[(row / 2) * (width / 2) + (col / 2)];
      uint8_t v = v_plane[(row / 2) * (width / 2) + (col / 2)];

      // YUV to RGB conversion (ITU-R BT.601)
      int32_t c = y - 16;
      int32_t d = u - 128;
      int32_t e = v - 128;

      int32_t r = (298 * c + 409 * e + 128) >> 8;
      int32_t g = (298 * c - 100 * d - 208 * e + 128) >> 8;
      int32_t b = (298 * c + 516 * d + 128) >> 8;

      // Clamp to [0, 255]
      if (r < 0)
        r = 0;
      if (r > 255)
        r = 255;
      if (g < 0)
        g = 0;
      if (g > 255)
        g = 255;
      if (b < 0)
        b = 0;
      if (b > 255)
        b = 255;

      // Convert to RGB565
      uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
      rgb[row * width + col] = rgb565;
    }
  }
}

// ========== Audio Processing ==========

bool VideoPlayer::init_audio_decoder_() {
  if (!this->has_audio_() || this->speaker_ == nullptr) {
    ESP_LOGD(TAG, "No audio track or speaker not configured - skipping audio");
    return false;
  }

  const AudioTrackInfo *audio = this->get_audio_track_();
  if (audio == nullptr) {
    ESP_LOGE(TAG, "Audio track info not available");
    return false;
  }

  // Determine AudioFileType from detected codec
  audio::AudioFileType file_type = audio::AudioFileType::NONE;
  const char *codec_name = "unknown";

  switch (audio->codec_type) {
#ifdef USE_AUDIO_AAC_SUPPORT
    case AudioCodecType::AAC:
      file_type = audio::AudioFileType::AAC;
      codec_name = "AAC";
      ESP_LOGI(TAG, "Using AAC decoder");
      break;
#endif

#ifdef USE_AUDIO_MP3_SUPPORT
    case AudioCodecType::MP3:
      file_type = audio::AudioFileType::MP3;
      codec_name = "MP3";
      ESP_LOGI(TAG, "Using MP3 decoder");
      break;
#endif

#ifdef USE_AUDIO_FLAC_SUPPORT
    case AudioCodecType::FLAC:
      file_type = audio::AudioFileType::FLAC;
      codec_name = "FLAC";
      ESP_LOGI(TAG, "Using FLAC decoder");
      break;
#endif

    default:
      ESP_LOGE(TAG, "Unknown or unsupported audio codec type");
      return false;
  }

  if (file_type == audio::AudioFileType::NONE) {
    ESP_LOGE(TAG, "Audio codec %s detected but corresponding decoder not enabled in configuration", codec_name);
    return false;
  }

  ESP_LOGI(TAG, "Initializing %s audio decoder: %u Hz, %u channels, %u bits", codec_name, audio->sample_rate,
           audio->channels, audio->bits_per_sample);

  // Create ring buffer for audio data (64KB buffer)
  this->audio_input_ring_buffer_ = std::make_shared<RingBuffer>(64 * 1024);

  // Create AudioDecoder (32KB input buffer, 32KB output buffer)
  this->audio_decoder_ = std::make_unique<audio::AudioDecoder>(32 * 1024, 32 * 1024);

  // Connect ring buffer as source
  std::weak_ptr<RingBuffer> weak_input = this->audio_input_ring_buffer_;
  esp_err_t err = this->audio_decoder_->add_source(weak_input);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add audio source: %d", err);
    this->audio_decoder_.reset();
    this->audio_input_ring_buffer_.reset();
    return false;
  }

  // Connect speaker as sink
  err = this->audio_decoder_->add_sink(this->speaker_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add audio sink: %d", err);
    this->audio_decoder_.reset();
    this->audio_input_ring_buffer_.reset();
    return false;
  }

  // Start decoder with detected codec type
  err = this->audio_decoder_->start(file_type);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start %s audio decoder: %d", codec_name, err);
    this->audio_decoder_.reset();
    this->audio_input_ring_buffer_.reset();
    return false;
  }

  // Allocate sample buffer (max 64KB per audio sample)
  this->audio_sample_buffer_.resize(64 * 1024);

  ESP_LOGI(TAG, "Audio decoder initialized successfully");
  return true;
}

void VideoPlayer::cleanup_audio_decoder_() {
  if (this->audio_decoder_) {
    this->audio_decoder_.reset();
  }
  if (this->audio_input_ring_buffer_) {
    this->audio_input_ring_buffer_.reset();
  }
  this->audio_sample_buffer_.clear();
}

void VideoPlayer::process_audio_() {
  if (!this->audio_decoder_ || !this->has_audio_()) {
    return;
  }

  // Feed audio samples to ring buffer
  this->feed_audio_sample_();

  // Run decoder to process audio and send to speaker
  audio::AudioDecoderState decoder_state = this->audio_decoder_->decode(false);

  if (decoder_state == audio::AudioDecoderState::FAILED) {
    ESP_LOGW(TAG, "Audio decoder failed");
    this->cleanup_audio_decoder_();
  } else if (decoder_state == audio::AudioDecoderState::FINISHED) {
    ESP_LOGD(TAG, "Audio decoding finished");
  }
}

void VideoPlayer::feed_audio_sample_() {
  if (!this->audio_input_ring_buffer_ || !this->demuxer_is_open_()) {
    return;
  }

  // Check if ring buffer has space for more data
  size_t available_space = this->audio_input_ring_buffer_->free();
  if (available_space < 4096) {
    return;  // Wait for buffer to drain
  }

  // Get next audio sample from MP4
  Sample sample;
  if (!this->get_next_audio_sample_(sample, this->audio_sample_buffer_.data(), this->audio_sample_buffer_.size())) {
    // End of audio track
    return;
  }

  // Write audio sample to ring buffer
  size_t written = this->audio_input_ring_buffer_->write(this->audio_sample_buffer_.data(), sample.size);
  if (written < sample.size) {
    ESP_LOGW(TAG, "Audio ring buffer overflow: wrote %zu/%u bytes", written, sample.size);
  }
}

// ========== Demuxer Helper Methods ==========

bool VideoPlayer::demuxer_is_open_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->is_open();
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->is_open();
#endif
    default:
      return false;
  }
}

void VideoPlayer::demuxer_close_() {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      if (this->mp4_demuxer_) {
        this->mp4_demuxer_->close();
        this->mp4_demuxer_.reset();
      }
      break;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      if (this->mkv_demuxer_) {
        this->mkv_demuxer_->close();
        this->mkv_demuxer_.reset();
      }
      break;
#endif
    default:
      break;
  }
  this->active_demuxer_ = nullptr;
  this->active_demuxer_type_ = DemuxerType::NONE;
}

void VideoPlayer::demuxer_reset_() {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      if (this->mp4_demuxer_)
        this->mp4_demuxer_->reset();
      break;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      if (this->mkv_demuxer_)
        this->mkv_demuxer_->reset();
      break;
#endif
    default:
      break;
  }
}

bool VideoPlayer::demuxer_seek_video_(uint64_t timestamp_ms) {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->seek_video(timestamp_ms);
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->seek_video(timestamp_ms);
#endif
    default:
      return false;
  }
}

bool VideoPlayer::has_video_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->has_video();
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->has_video();
#endif
    default:
      return false;
  }
}

bool VideoPlayer::has_audio_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->has_audio();
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->has_audio();
#endif
    default:
      return false;
  }
}

const VideoTrackInfo *VideoPlayer::get_video_track_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ ? this->mp4_demuxer_->get_video_track() : nullptr;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ ? this->mkv_demuxer_->get_video_track() : nullptr;
#endif
    default:
      return nullptr;
  }
}

const AudioTrackInfo *VideoPlayer::get_audio_track_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ ? this->mp4_demuxer_->get_audio_track() : nullptr;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ ? this->mkv_demuxer_->get_audio_track() : nullptr;
#endif
    default:
      return nullptr;
  }
}

bool VideoPlayer::get_next_video_sample_(Sample &sample, uint8_t *data, size_t max_size) {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->get_next_video_sample(sample, data, max_size);
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->get_next_video_sample(sample, data, max_size);
#endif
    default:
      return false;
  }
}

bool VideoPlayer::get_next_audio_sample_(Sample &sample, uint8_t *data, size_t max_size) {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ && this->mp4_demuxer_->get_next_audio_sample(sample, data, max_size);
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ && this->mkv_demuxer_->get_next_audio_sample(sample, data, max_size);
#endif
    default:
      return false;
  }
}

uint64_t VideoPlayer::get_video_duration_ms_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ ? this->mp4_demuxer_->get_video_duration_ms() : 0;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ ? this->mkv_demuxer_->get_video_duration_ms() : 0;
#endif
    default:
      return 0;
  }
}

uint64_t VideoPlayer::get_audio_duration_ms_() const {
  switch (this->active_demuxer_type_) {
#ifdef USE_MP4_CONTAINER
    case DemuxerType::MP4:
      return this->mp4_demuxer_ ? this->mp4_demuxer_->get_audio_duration_ms() : 0;
#endif
#ifdef USE_MKV_CONTAINER
    case DemuxerType::MKV:
      return this->mkv_demuxer_ ? this->mkv_demuxer_->get_audio_duration_ms() : 0;
#endif
    default:
      return 0;
  }
}

}  // namespace video_player
}  // namespace esphome
