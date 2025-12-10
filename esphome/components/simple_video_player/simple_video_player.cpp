#include "simple_video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cstring>

#ifdef USE_ESP32
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_dma_utils.h"
#endif

#ifdef USE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"
#include "hal/color_types.h"
#endif

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player";

// JPEG EOI (End of Image) marker
static const uint16_t JPEG_EOI = 0xd9ff;

// Alignment helpers
#define ALIGN_UP(num, align) (((num) + ((align) -1)) & ~((align) -1))
#define ALIGN_DOWN(num, align) ((num) & ~((align) -1))

// Cache alignment for optimal SD/storage performance
static constexpr size_t CACHE_ALIGNMENT = 1024;
static constexpr size_t DMA_ALIGNMENT = 128;

//========================================================================
// Component Lifecycle
//========================================================================

SimpleVideoPlayer::~SimpleVideoPlayer() {
  this->stop();
  this->free_buffers_();

  if (this->state_mutex_ != nullptr) {
    vSemaphoreDelete(this->state_mutex_);
  }

  if (this->lvgl_mutex_ != nullptr) {
    vSemaphoreDelete(this->lvgl_mutex_);
  }
}

void SimpleVideoPlayer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Simple Video Player...");

  // Verify transcoder is available
  if (this->transcoder_ == nullptr) {
    ESP_LOGE(TAG, "Transcoder component not set");
    this->mark_failed();
    return;
  }

  // Initialize JPEG decoder during setup (not during playback)
  // This ensures resources are allocated early and won't fail during playback
#ifdef USE_HARDWARE_JPEG_DECODER
  jpeg_decoder_handle_t decoder = this->transcoder_->get_jpeg_decoder();
  if (decoder == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize JPEG decoder during setup");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Hardware JPEG decoder initialized (handle: %p)", decoder);
#elif defined(USE_ESP_JPEG_DECODER)
  if (!this->transcoder_->is_jpeg_decoder_available()) {
    ESP_LOGE(TAG, "ESP-JPEG decoder not available");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "ESP-JPEG decoder ready");
#else
  ESP_LOGW(TAG, "No hardware JPEG decoder available");
#endif

  // Verify canvas is set
  if (this->canvas_ == nullptr) {
    ESP_LOGE(TAG, "Canvas not set");
    this->mark_failed();
    return;
  }

  // Create state mutex
  this->state_mutex_ = xSemaphoreCreateMutex();
  if (this->state_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create state mutex");
    this->mark_failed();
    return;
  }

  // Create LVGL mutex for thread-safe LVGL API calls
  this->lvgl_mutex_ = xSemaphoreCreateMutex();
  if (this->lvgl_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create LVGL mutex");
    this->mark_failed();
    return;
  }

  // Allocate cache buffer (internal RAM, aligned for DMA)
#ifdef USE_ESP32
  this->cache_buffer_.reset(
      static_cast<uint8_t *>(heap_caps_aligned_alloc(DMA_ALIGNMENT, this->cache_buffer_size_, MALLOC_CAP_INTERNAL)));
#else
  this->cache_buffer_.reset(new uint8_t[this->cache_buffer_size_]);
#endif

  if (!this->cache_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate cache buffer (%u bytes)", this->cache_buffer_size_);
    this->mark_failed();
    return;
  }

  // Pre-allocate input and output buffers during setup (in PSRAM)
  // This ensures resources are allocated early and won't fail during playback
  ESP_LOGI(TAG, "Pre-allocating PSRAM buffers...");

#ifdef USE_HARDWARE_JPEG_DECODER
  // Allocate input buffer (JPEG encoded frame buffer - PSRAM)
  jpeg_decode_memory_alloc_cfg_t input_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };

  size_t actual_input_size = 0;
  uint8_t *input_buf =
      static_cast<uint8_t *>(jpeg_alloc_decoder_mem(this->input_buffer_size_, &input_cfg, &actual_input_size));

  if (input_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate input buffer (%u bytes)", this->input_buffer_size_);
    this->mark_failed();
    return;
  }

  this->input_buffer_.reset(input_buf);
  ESP_LOGI(TAG, "Input buffer allocated: %zu bytes (PSRAM)", actual_input_size);

  // Allocate maximum output buffer (decoded RGB565 frame buffer - PSRAM)
  // Calculate max size based on typical 720p video (1280x720) with alignment
  uint32_t max_width = 1280;
  uint32_t max_height = 720;
  uint32_t aligned_max_width = ALIGN_UP(max_width, 16);
  uint32_t aligned_max_height = ALIGN_UP(max_height, 16);
  size_t max_output_size = aligned_max_width * aligned_max_height * 2;  // RGB565 = 2 bytes per pixel

  jpeg_decode_memory_alloc_cfg_t output_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };

  size_t actual_output_size = 0;
  uint8_t *output_buf =
      static_cast<uint8_t *>(jpeg_alloc_decoder_mem(max_output_size, &output_cfg, &actual_output_size));

  if (output_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", max_output_size);
    this->mark_failed();
    return;
  }

  this->output_buffer_.reset(output_buf);
  this->output_buffer_size_ = actual_output_size;
  ESP_LOGI(TAG, "Output buffer allocated: %zu bytes (PSRAM, max %ux%u)", actual_output_size, aligned_max_width,
           aligned_max_height);
#else
  ESP_LOGW(TAG, "Hardware JPEG decoder not available - buffers not allocated");
#endif

  // Audio buffers are now allocated in init_audio_decoder_() when playback starts
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    ESP_LOGI(TAG, "Audio playback enabled with speaker");
  } else {
    ESP_LOGI(TAG, "Audio playback disabled (no speaker configured)");
  }
#endif

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
  ESP_LOGCONFIG(TAG, "  Cache buffer: %u bytes (internal RAM)", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Input buffer: %u bytes (PSRAM)", this->input_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Output buffer: %zu bytes (PSRAM)", this->output_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Audio: Enabled");
  }
#endif
}

void SimpleVideoPlayer::loop() {
  // Nothing to do in loop - playback runs in separate task
}

void SimpleVideoPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Simple Video Player:");
  ESP_LOGCONFIG(TAG, "  Cache buffer size: %u bytes", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Input buffer size: %u bytes", this->input_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);

  if (this->state_ != PlayerState::STOPPED) {
    ESP_LOGCONFIG(TAG, "  Current file: %s", this->video_path_.c_str());
    ESP_LOGCONFIG(TAG, "  Video size: %ux%u", this->video_width_, this->video_height_);
  }
}

//========================================================================
// Playback Control API
//========================================================================

void SimpleVideoPlayer::play(const std::string &video_path) {
  ESP_LOGI(TAG, "Playing video: %s", video_path.c_str());

  // Stop any existing playback
  if (this->state_ != PlayerState::STOPPED) {
    this->stop();
    this->wait_for_task_stop_(5000);
  }

  // Update state
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  this->video_path_ = video_path;
  this->state_ = PlayerState::PLAYING;
  this->last_error_ = PlaybackError::NONE;
  xSemaphoreGive(this->state_mutex_);

  // Create playback task
  BaseType_t result = xTaskCreate(playback_task_entry_, "video_player",
                                  8192,  // Stack size
                                  this,
                                  5,  // Priority
                                  &this->task_handle_);

  if (result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create playback task");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->state_ = PlayerState::ERROR;
  }
}

void SimpleVideoPlayer::pause() {
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  if (this->state_ == PlayerState::PLAYING) {
    ESP_LOGI(TAG, "Pausing playback");
    this->state_ = PlayerState::PAUSED;
    xSemaphoreGive(this->state_mutex_);
    this->on_paused_callbacks_.call();
  } else {
    xSemaphoreGive(this->state_mutex_);
  }
}

void SimpleVideoPlayer::resume() {
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  if (this->state_ == PlayerState::PAUSED) {
    ESP_LOGI(TAG, "Resuming playback");
    this->state_ = PlayerState::PLAYING;
    xSemaphoreGive(this->state_mutex_);
  } else {
    xSemaphoreGive(this->state_mutex_);
  }
}

void SimpleVideoPlayer::stop() {
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  if (this->state_ != PlayerState::STOPPED) {
    ESP_LOGI(TAG, "Stopping playback");
    this->state_ = PlayerState::STOPPED;
  }
  xSemaphoreGive(this->state_mutex_);
}

//========================================================================
// Playback Task
//========================================================================

void SimpleVideoPlayer::playback_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  player->playback_loop_();
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::playback_loop_() {
  bool canvas_positioned = false;
  ESP_LOGI(TAG, "Playback task started");

  // Open file
  if (!this->open_file_(this->video_path_)) {
    ESP_LOGE(TAG, "Failed to open video file: %s", this->video_path_.c_str());
    this->set_error_(PlaybackError::FILE_NOT_FOUND);
    return;
  }

  // Get video dimensions from first frame
  uint32_t width = 0;
  uint32_t height = 0;
  if (!this->get_video_dimensions_(width, height)) {
    ESP_LOGE(TAG, "Failed to get video dimensions");
    this->set_error_(PlaybackError::INVALID_VIDEO_FORMAT);
    this->close_file_();
    return;
  }
  ESP_LOGI(TAG, "Video dimensions: %ux%u", width, height);

  if (!canvas_positioned) {
    lv_coord_t canvas_width = lv_obj_get_width(this->canvas_);
    lv_coord_t canvas_height = lv_obj_get_height(this->canvas_);
    if ((lv_coord_t) width < canvas_width || (lv_coord_t) height < canvas_height) {
      lv_coord_t canvas_x = lv_obj_get_x(this->canvas_);
      lv_coord_t canvas_y = lv_obj_get_y(this->canvas_);
      lv_coord_t x_offset = (canvas_width - width) / 2;
      lv_coord_t y_offset = (canvas_height - height) / 2;
      ESP_LOGI(TAG, "Resizing canvas from %ux%u to %ux%u", canvas_width, canvas_height, width, height);
      lv_obj_set_size(this->canvas_, width, height);
      lv_obj_set_pos(this->canvas_, canvas_x + x_offset, canvas_y + y_offset);
      lv_obj_invalidate(this->canvas_);
    }
    canvas_positioned = true;
  }

  // Initialize audio decoder for AVI files with audio
#ifdef USE_AUDIO
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    this->init_audio_decoder_();  // Non-fatal if audio init fails
  }
#endif

  // Allocate buffers based on video size
  if (!this->allocate_buffers_(width, height)) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
    return;
  }

  // Set canvas buffer with aligned dimensions
  // The decoder outputs aligned dimensions, so we must tell LVGL about the actual buffer layout
  uint32_t aligned_width = ALIGN_UP(width, 16);
  uint32_t aligned_height = ALIGN_UP(height, 16);

  // Lock LVGL mutex before calling LVGL APIs from FreeRTOS task
  // This prevents crashes from concurrent access to LVGL (not thread-safe)
  if (xSemaphoreTake(this->lvgl_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
    // Set canvas buffer to match video dimensions
    lv_canvas_set_buffer(this->canvas_, this->output_buffer_.get(), aligned_width, aligned_height,
                         LV_IMG_CF_TRUE_COLOR);

    xSemaphoreGive(this->lvgl_mutex_);
  } else {
    ESP_LOGW(TAG, "Failed to acquire LVGL mutex for canvas setup");
  }

#ifdef USE_HARDWARE_JPEG_DECODER
  // Acquire exclusive access to JPEG decoder for duration of playback
  if (this->transcoder_ != nullptr) {
    if (!this->transcoder_->acquire_jpeg_decoder_exclusive("simple_video_player")) {
      ESP_LOGE(TAG, "Failed to acquire exclusive JPEG decoder access");
      this->set_error_(PlaybackError::DECODER_INIT_FAILED);
      this->close_file_();
      return;
    }
  }
#endif

  // Reset file position to start (not needed for AVI - parser is already positioned at movi data)
  if (this->video_format_ != VideoFormat::AVI_MJPEG) {
    this->seek_to_(0);
  }
  this->cache_buffer_valid_ = 0;
  this->cache_buffer_offset_ = 0;

  // Fire started callback
  this->on_started_callbacks_.call();

  // Calculate frame delay for target FPS
  uint32_t frame_delay_ms = static_cast<uint32_t>(1000.0f / this->target_fps_);

  // Main playback loop
  while (this->state_ != PlayerState::STOPPED) {
    // Handle pause state
    if (this->state_ == PlayerState::PAUSED) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Record frame start time for FPS control
    TickType_t frame_start = xTaskGetTickCount();

    // Read next frame
    int frame_size = this->read_next_frame_();

    if (frame_size < 0) {
      // Read error
      ESP_LOGE(TAG, "Failed to read frame");
      this->set_error_(PlaybackError::FILE_READ_ERROR);
      break;
    } else if (frame_size == 0) {
      // End of file
      ESP_LOGI(TAG, "Playback finished");

      if (this->loop_) {
        ESP_LOGI(TAG, "Looping video");
        this->seek_to_(0);
        this->cache_buffer_valid_ = 0;
        this->cache_buffer_offset_ = 0;
        continue;
      } else {
        this->on_finished_callbacks_.call();
        break;
      }
    }

    // Decode frame
    if (!this->decode_frame_(frame_size)) {
      ESP_LOGW(TAG, "Failed to decode frame, skipping");
      // Don't stop on decode errors, just skip the frame
      continue;
    }

#ifdef USE_AUDIO
    // Audio decoding and channel conversion is now handled by dedicated audio task on Core 0
    // No audio processing needed in main playback loop
#endif

    // Frame rate control
    TickType_t frame_end = xTaskGetTickCount();
    TickType_t elapsed = frame_end - frame_start;
    TickType_t delay_ticks = pdMS_TO_TICKS(frame_delay_ms);

    if (elapsed < delay_ticks) {
      vTaskDelay(delay_ticks - elapsed);
    }
  }

  // Cleanup
  this->close_file_();
  // Note: Buffers are NOT freed here - they persist for reuse in next playback
  // Buffers are only freed in destructor when component is destroyed

#ifdef USE_AUDIO
  // Stop and cleanup audio processing
  if (this->audio_enabled_) {
    // Stop audio processing task first
    this->stop_audio_task_();

    // Stop speaker
    if (this->speaker_) {
      this->speaker_->stop();
    }

    // Cleanup audio resources
    this->audio_decoder_.reset();
    this->audio_input_ring_buffer_.reset();
    this->audio_decoded_ring_buffer_.reset();
    this->audio_temp_buffer_.reset();
    this->audio_enabled_ = false;

    ESP_LOGI(TAG, "Audio processing stopped");
  }
#endif

#ifdef USE_HARDWARE_JPEG_DECODER
  // Release exclusive access to decoder after playback
  if (this->transcoder_ != nullptr) {
    this->transcoder_->release_jpeg_decoder_exclusive();
  }
#endif

  // Clear canvas (lock LVGL mutex for thread safety)
  if (this->output_buffer_) {
    std::memset(this->output_buffer_.get(), 0, this->output_buffer_size_);
    if (xSemaphoreTake(this->lvgl_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
      lv_obj_invalidate(this->canvas_);
      xSemaphoreGive(this->lvgl_mutex_);
    }
  }

  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  this->state_ = PlayerState::STOPPED;
  xSemaphoreGive(this->state_mutex_);

  this->task_handle_ = nullptr;

  ESP_LOGI(TAG, "Playback task finished");
}

bool SimpleVideoPlayer::wait_for_task_stop_(uint32_t timeout_ms) {
  if (this->task_handle_ == nullptr) {
    return true;
  }

  uint32_t elapsed = 0;
  while (this->task_handle_ != nullptr && elapsed < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(10));
    elapsed += 10;
  }

  return this->task_handle_ == nullptr;
}

//========================================================================
// Frame Processing
//========================================================================

int SimpleVideoPlayer::read_next_frame_() {
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    // AVI format - use parser to get next frame (video or audio)
    AVIFrame frame;
    int bytes_read = this->avi_parser_->read_next_frame(frame, this->input_buffer_.get(), this->input_buffer_size_);

    if (bytes_read <= 0) {
      return bytes_read;  // EOF or error
    }

    // Skip audio frames if no speaker, otherwise process them
    while (frame.stream_type != AVIStreamType::VIDEO) {
#ifdef USE_AUDIO
      if (frame.stream_type == AVIStreamType::AUDIO && this->audio_enabled_) {
        this->process_audio_frame_(frame, this->input_buffer_.get(), bytes_read);
      }
#endif
      // Skip this frame (audio) and read next frame
      bytes_read = this->avi_parser_->read_next_frame(frame, this->input_buffer_.get(), this->input_buffer_size_);
      if (bytes_read <= 0) {
        return bytes_read;
      }
    }

    return bytes_read;
  } else {
    // Raw MJPEG - search for JPEG EOI marker to find frame boundary
    size_t frame_size = 0;
    uint8_t *frame_ptr = this->input_buffer_.get();

    while (true) {
      // Read more data into cache if needed
      if (this->cache_buffer_offset_ >= this->cache_buffer_valid_) {
        // Cache is exhausted, read next chunk
        int bytes_read = this->read_data_(this->cache_buffer_.get(), this->cache_buffer_size_);

        if (bytes_read <= 0) {
          // EOF or error
          return bytes_read;
        }

        this->cache_buffer_valid_ = bytes_read;
        this->cache_buffer_offset_ = 0;
      }

      // Search for EOI marker in cache
      size_t search_len = this->cache_buffer_valid_ - this->cache_buffer_offset_;
      uint8_t *search_start = this->cache_buffer_.get() + this->cache_buffer_offset_;
      uint8_t *eoi_ptr = static_cast<uint8_t *>(memmem(search_start, search_len, &JPEG_EOI, 2));

      if (eoi_ptr != nullptr) {
        // Found EOI marker
        size_t chunk_size = (eoi_ptr - search_start) + 2;  // Include EOI marker

        // Check if frame fits in input buffer
        if (frame_size + chunk_size > this->input_buffer_size_) {
          ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %u)", frame_size + chunk_size,
                   this->input_buffer_size_);
          return -1;
        }

        // Copy chunk to input buffer
        std::memcpy(frame_ptr, search_start, chunk_size);
        frame_size += chunk_size;
        this->cache_buffer_offset_ += chunk_size;

        // Frame complete
        return frame_size;
      } else {
        // EOI not found in this cache chunk, copy entire remaining cache to frame buffer

        // Check if frame fits in input buffer
        if (frame_size + search_len > this->input_buffer_size_) {
          ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %u)", frame_size + search_len,
                   this->input_buffer_size_);
          return -1;
        }

        std::memcpy(frame_ptr, search_start, search_len);
        frame_ptr += search_len;
        frame_size += search_len;
        this->cache_buffer_offset_ = this->cache_buffer_valid_;

        // Continue to next cache chunk
      }
    }
  }
}

bool SimpleVideoPlayer::decode_frame_(size_t frame_size) {
#ifdef USE_HARDWARE_JPEG_DECODER
  // Use decoder acquired exclusively for this playback session
  jpeg_decoder_handle_t decoder = this->transcoder_->get_jpeg_decoder();
  if (decoder == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder not available (should have been acquired at playback start)");
    return false;
  }

  // Align frame size to 16 bytes (hardware requirement)
  size_t aligned_size = ALIGN_UP(frame_size, 16);

  if (aligned_size > this->input_buffer_size_) {
    ESP_LOGE(TAG, "Aligned frame size too large");
    return false;
  }

  // Configure decoder for RGB565 output
  // LVGL uses RGB565 little-endian format, so we need BGR element order to match
  jpeg_decode_cfg_t decode_cfg = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
  };

  // Decode frame
  uint32_t out_size = this->output_buffer_size_;
  esp_err_t err = jpeg_decoder_process(decoder, &decode_cfg, this->input_buffer_.get(), aligned_size,
                                       this->output_buffer_.get(), this->output_buffer_size_, &out_size);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "JPEG decode failed: %d", err);
    return false;
  }

  // Update canvas (lock LVGL mutex for thread safety)
  if (xSemaphoreTake(this->lvgl_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
    lv_obj_invalidate(this->canvas_);
    xSemaphoreGive(this->lvgl_mutex_);
  } else {
    ESP_LOGW(TAG, "Failed to acquire LVGL mutex for canvas invalidate");
  }

  return true;
#else
  ESP_LOGE(TAG, "Hardware JPEG decoder not available");
  return false;
#endif
}

bool SimpleVideoPlayer::get_video_dimensions_(uint32_t &width, uint32_t &height) {
#ifdef USE_HARDWARE_JPEG_DECODER
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    // Get dimensions from AVI header
    const AVIStreamInfo *video_info = this->avi_parser_->get_video_info();
    if (video_info == nullptr) {
      ESP_LOGE(TAG, "No video stream found in AVI file");
      return false;
    }

    width = video_info->width;
    height = video_info->height;

    // Store dimensions
    this->video_width_ = width;
    this->video_height_ = height;

    ESP_LOGI(TAG, "AVI video dimensions: %ux%u, FPS: %u/%u", width, height, video_info->fps_num, video_info->fps_den);
    return true;
  } else {
    // Raw MJPEG - read first chunk to get JPEG header
    int bytes_read = this->read_data_(this->cache_buffer_.get(), this->cache_buffer_size_);
    if (bytes_read <= 0) {
      return false;
    }

    this->cache_buffer_valid_ = bytes_read;
    this->cache_buffer_offset_ = 0;

    // Parse JPEG header
    jpeg_decode_picture_info_t header;
    esp_err_t err = jpeg_decoder_get_info(this->cache_buffer_.get(), bytes_read, &header);

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to parse JPEG header");
      return false;
    }

    width = header.width;
    height = header.height;

    // Store dimensions
    this->video_width_ = width;
    this->video_height_ = height;

    // Reset file position for playback
    this->seek_to_(0);
    this->cache_buffer_valid_ = 0;
    this->cache_buffer_offset_ = 0;

    return true;
  }
#else
  ESP_LOGE(TAG, "Hardware JPEG decoder not available");
  return false;
#endif
}

//========================================================================
// File I/O Abstraction
//========================================================================

VideoFormat SimpleVideoPlayer::detect_format_() {
  // Read first 12 bytes to detect file format
  uint8_t header[12];
  int bytes_read = this->read_data_(header, sizeof(header));

  if (bytes_read < 12) {
    ESP_LOGE(TAG, "Failed to read file header for format detection");
    return VideoFormat::UNKNOWN;
  }

  // Check for AVI RIFF signature: "RIFF....AVI "
  if (header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' && header[8] == 'A' &&
      header[9] == 'V' && header[10] == 'I' && header[11] == ' ') {
    ESP_LOGI(TAG, "Detected AVI container format");
    return VideoFormat::AVI_MJPEG;
  }

  // Check for JPEG SOI marker: 0xFF 0xD8
  if (header[0] == 0xFF && header[1] == 0xD8) {
    ESP_LOGI(TAG, "Detected raw MJPEG format");
    return VideoFormat::RAW_MJPEG;
  }

  ESP_LOGW(TAG, "Unknown video format (header: %02X %02X %02X %02X)", header[0], header[1], header[2], header[3]);
  return VideoFormat::UNKNOWN;
}

bool SimpleVideoPlayer::open_file_(const std::string &path) {
#ifdef USE_STORAGE
  // Check if this is a network path
  if (storage::global_storage && storage::global_storage->is_network_path(path)) {
    ESP_LOGI(TAG, "Opening network file: %s", path.c_str());
    this->is_network_file_ = true;
    // For network files, we don't open a FILE* handle
    // Instead, we'll use network storage API in read_data_()

    // Try to get file size (may not be supported by all backends)
    // For now, assume unknown size for network files
    this->file_size_ = 0;
    return true;
  }
#endif

  // Open local file with optimized buffered reader
  ESP_LOGI(TAG, "Opening local file with buffered reader: %s", path.c_str());
  this->is_network_file_ = false;

  // Create buffered file reader
  this->file_reader_ = std::make_unique<BufferedFileReader>();
  if (!this->file_reader_->open(path.c_str())) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    this->file_reader_.reset();
    return false;
  }

  // Get file size
  if (!this->file_reader_->get_size(&this->file_size_)) {
    ESP_LOGW(TAG, "Failed to get file size");
  } else {
    ESP_LOGI(TAG, "File size: %llu bytes", this->file_size_);
  }

  // Detect video format
  this->video_format_ = this->detect_format_();
  if (this->video_format_ == VideoFormat::UNKNOWN) {
    ESP_LOGE(TAG, "Unknown video format");
    this->close_file_();
    return false;
  }

  // Initialize AVI parser if needed
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    this->avi_parser_ = std::make_unique<AVIParser>();

    // Seek back to start for parser
    if (!this->seek_to_(0)) {
      ESP_LOGE(TAG, "Failed to seek to start for AVI parsing");
      this->close_file_();
      return false;
    }

    // Open AVI file
    if (!this->avi_parser_->open(this->file_reader_.get())) {
      ESP_LOGE(TAG, "Failed to parse AVI file");
      this->avi_parser_.reset();
      this->close_file_();
      return false;
    }

    ESP_LOGI(TAG, "AVI parser initialized successfully");
  } else {
    // Raw MJPEG - seek back to start for frame reading
    if (!this->seek_to_(0)) {
      ESP_LOGE(TAG, "Failed to seek to start");
      this->close_file_();
      return false;
    }
  }

  return true;
}

void SimpleVideoPlayer::close_file_() {
  // Close AVI parser if open
  if (this->avi_parser_) {
    this->avi_parser_->close();
    this->avi_parser_.reset();
  }

  // Close file reader
  if (this->file_reader_) {
    this->file_reader_->close();
    this->file_reader_.reset();
  }

  this->is_network_file_ = false;
  this->file_size_ = 0;
  this->video_format_ = VideoFormat::UNKNOWN;
}

int SimpleVideoPlayer::read_data_(uint8_t *buffer, size_t size) {
  if (this->is_network_file_) {
#ifdef USE_STORAGE
    // Network file - use storage API
    // Note: This is a simplified implementation that reads chunks
    // A full implementation would need to handle streaming more efficiently
    if (storage::global_storage == nullptr) {
      return -1;
    }

    // For network streaming, we need to read the file in chunks
    // This is a placeholder - actual implementation depends on network storage backend
    ESP_LOGW(TAG, "Network file streaming not fully implemented yet");
    return -1;
#else
    return -1;
#endif
  } else {
    // Local file - use buffered reader
    if (!this->file_reader_ || !this->file_reader_->is_open()) {
      return -1;
    }

    return this->file_reader_->read(buffer, size);
  }
}

bool SimpleVideoPlayer::seek_to_(uint64_t position) {
  if (this->is_network_file_) {
    // Network files don't support seeking in this simple implementation
    // Would need to close and reopen with range request
    ESP_LOGW(TAG, "Seek not supported for network files");
    return false;
  } else {
    if (!this->file_reader_ || !this->file_reader_->is_open()) {
      return false;
    }

    return this->file_reader_->seek(position);
  }
}

bool SimpleVideoPlayer::get_file_size_(uint64_t &size) {
  if (this->is_network_file_) {
    // Network file size might not be known
    size = 0;
    return false;
  } else {
    if (!this->file_reader_ || !this->file_reader_->is_open()) {
      return false;
    }

    return this->file_reader_->get_size(&size);
  }
}

//========================================================================
// Buffer Management
//========================================================================

bool SimpleVideoPlayer::allocate_buffers_(uint32_t video_width, uint32_t video_height) {
  // Align width and height to 16 bytes (hardware requirement)
  uint32_t aligned_width = ALIGN_UP(video_width, 16);
  uint32_t aligned_height = ALIGN_UP(video_height, 16);

  // Calculate required output buffer size (RGB565 = 2 bytes per pixel)
  size_t required_output_size = aligned_width * aligned_height * 2;

  ESP_LOGI(TAG, "Verifying buffers for %ux%u video (aligned: %ux%u)", video_width, video_height, aligned_width,
           aligned_height);
  ESP_LOGI(TAG, "Required output buffer: %zu bytes, allocated: %zu bytes", required_output_size,
           this->output_buffer_size_);

  // Verify buffers were pre-allocated during setup
  if (!this->input_buffer_) {
    ESP_LOGE(TAG, "Input buffer not pre-allocated (this should not happen)");
    return false;
  }

  if (!this->output_buffer_) {
    ESP_LOGE(TAG, "Output buffer not pre-allocated (this should not happen)");
    return false;
  }

  // Check if pre-allocated buffers are large enough
  if (required_output_size > this->output_buffer_size_) {
    ESP_LOGE(TAG, "Video too large for pre-allocated buffer: %ux%u requires %zu bytes, only %zu bytes available",
             aligned_width, aligned_height, required_output_size, this->output_buffer_size_);
    ESP_LOGE(TAG, "Increase max video resolution in setup() or use smaller video");
    return false;
  }

  ESP_LOGI(TAG, "Buffers verified - Input: %u bytes, Output: %zu bytes (using %zu bytes)", this->input_buffer_size_,
           this->output_buffer_size_, required_output_size);

  return true;
}

void SimpleVideoPlayer::free_buffers_() {
  if (this->input_buffer_) {
    heap_caps_free(this->input_buffer_.release());
  }

  if (this->output_buffer_) {
    heap_caps_free(this->output_buffer_.release());
  }

  this->output_buffer_size_ = 0;
}

//========================================================================
// Audio Processing
//========================================================================

#ifdef USE_AUDIO
bool SimpleVideoPlayer::init_audio_decoder_() {
  // Check if audio is enabled
  if (this->speaker_ == nullptr) {
    return false;
  }

  const AVIStreamInfo *audio_info = this->avi_parser_->get_audio_info();
  if (audio_info == nullptr) {
    ESP_LOGI(TAG, "No audio stream found in AVI file");
    return false;
  }

  // Store audio parameters
  this->source_audio_channels_ = audio_info->channels;
  this->audio_sample_rate_ = audio_info->sample_rate;
  this->audio_bits_per_sample_ = audio_info->bits_per_sample;

  // Determine output channel count based on speaker configuration
  this->speaker_audio_channels_ = 1;  // Default to mono
  if (this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_STEREO) {
    this->speaker_audio_channels_ = 2;
  }

  // Check if channel conversion is needed
  this->needs_channel_conversion_ = (this->source_audio_channels_ != this->speaker_audio_channels_);

  ESP_LOGI(TAG, "Audio routing: %u-channel source → %u-channel speaker (mode: %s)%s", this->source_audio_channels_,
           this->speaker_audio_channels_,
           this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_MONO     ? "mono"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_LEFT   ? "left"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_RIGHT  ? "right"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_STEREO ? "stereo"
                                                                                       : "unknown",
           this->needs_channel_conversion_ ? " [conversion needed]" : "");

  // Determine audio codec type
  audio::AudioFileType codec_type = audio::AudioFileType::NONE;
  if (audio_info->codec == static_cast<uint32_t>(AVIAudioCodec::MP3)) {
    codec_type = audio::AudioFileType::MP3;
    ESP_LOGI(TAG, "Audio codec: MP3, %u Hz, %u channels, %u bits", audio_info->sample_rate, audio_info->channels,
             audio_info->bits_per_sample);
  } else if (audio_info->codec == static_cast<uint32_t>(AVIAudioCodec::FLAC)) {
    codec_type = audio::AudioFileType::FLAC;
    ESP_LOGI(TAG, "Audio codec: FLAC, %u Hz, %u channels, %u bits", audio_info->sample_rate, audio_info->channels,
             audio_info->bits_per_sample);
  } else if (audio_info->codec == static_cast<uint32_t>(AVIAudioCodec::PCM)) {
    codec_type = audio::AudioFileType::WAV;
    ESP_LOGI(TAG, "Audio codec: PCM, %u Hz, %u channels, %u bits", audio_info->sample_rate, audio_info->channels,
             audio_info->bits_per_sample);
  } else {
    ESP_LOGW(TAG, "Unsupported audio codec: 0x%04X", audio_info->codec);
    return false;
  }

  // CRITICAL: Configure speaker's audio stream info based on SPEAKER config, not file
  audio::AudioStreamInfo speaker_stream_info(audio_info->bits_per_sample, this->speaker_audio_channels_,
                                             audio_info->sample_rate);
  this->speaker_->set_audio_stream_info(speaker_stream_info);

  // Start the speaker to initialize I2S driver
  this->speaker_->start();

  // Wait for speaker to finish initialization (STATE_STARTING → STATE_RUNNING)
  uint32_t wait_start = millis();
  const uint32_t SPEAKER_INIT_TIMEOUT_MS = 1000;
  while (!this->speaker_->is_running() && (millis() - wait_start) < SPEAKER_INIT_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (!this->speaker_->is_running()) {
    ESP_LOGE(TAG, "Speaker failed to start within %u ms", SPEAKER_INIT_TIMEOUT_MS);
    return false;
  }

  ESP_LOGI(TAG, "Speaker initialized: %u-bit, %u-channel, %u Hz", audio_info->bits_per_sample,
           this->speaker_audio_channels_, audio_info->sample_rate);

  // ============================================================================
  // CORRECT Buffer Size Calculations
  // ============================================================================

  // Calculate bytes per frame and per second for DECODED PCM audio
  size_t bytes_per_frame = this->source_audio_channels_ * (this->audio_bits_per_sample_ / 8);
  size_t bytes_per_second_decoded = this->audio_sample_rate_ * bytes_per_frame;

  // Target buffering durations (in milliseconds)
  const uint32_t INPUT_BUFFER_DURATION_MS = 250;    // 250ms of COMPRESSED audio data
  const uint32_t DECODED_BUFFER_DURATION_MS = 500;  // 500ms of decoded PCM audio
  const uint32_t TEMP_BUFFER_DURATION_MS = 100;     // 100ms for channel conversion temp buffer

  // Calculate buffer sizes
  // Input buffer: For COMPRESSED audio (FLAC/MP3)
  // FLAC: ~50% compression (2x ratio), MP3: ~10% compression (10x ratio)
  // Use conservative estimate: allocate 250ms of PCM equivalent for compressed data
  // This gives us ~500ms-2500ms of actual compressed audio depending on codec
  size_t input_buffer_size = (bytes_per_second_decoded * INPUT_BUFFER_DURATION_MS) / 1000;

  // Decoded buffer: Actual PCM data before channel conversion (500ms of uncompressed audio)
  size_t decoded_buffer_size = (bytes_per_second_decoded * DECODED_BUFFER_DURATION_MS) / 1000;

  // Temp buffer: For channel conversion processing (100ms of uncompressed audio)
  this->audio_temp_buffer_size_ = (bytes_per_second_decoded * TEMP_BUFFER_DURATION_MS) / 1000;

  // Ensure minimum sizes for edge cases (low sample rates)
  input_buffer_size = std::max(input_buffer_size, static_cast<size_t>(32 * 1024));                         // Min 32KB
  decoded_buffer_size = std::max(decoded_buffer_size, static_cast<size_t>(16 * 1024));                     // Min 16KB
  this->audio_temp_buffer_size_ = std::max(this->audio_temp_buffer_size_, static_cast<size_t>(8 * 1024));  // Min 8KB

  ESP_LOGI(TAG, "Audio buffer config: PCM data rate = %zu bytes/sec (%.1f KB/s)", bytes_per_second_decoded,
           bytes_per_second_decoded / 1024.0f);
  ESP_LOGI(TAG, "  Input buffer: %zu KB (%u ms)", input_buffer_size / 1024, INPUT_BUFFER_DURATION_MS);
  ESP_LOGI(TAG, "  Decoded buffer: %zu KB (%u ms)", decoded_buffer_size / 1024, DECODED_BUFFER_DURATION_MS);
  ESP_LOGI(TAG, "  Temp buffer: %zu KB (%u ms)", this->audio_temp_buffer_size_ / 1024, TEMP_BUFFER_DURATION_MS);

  // Create ring buffer for encoded audio input (automatically uses PSRAM via RAMAllocator)
  this->audio_input_ring_buffer_ = RingBuffer::create(input_buffer_size);
  if (this->audio_input_ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio input ring buffer (%zu KB)", input_buffer_size / 1024);
    return false;
  }

  // If channel conversion is needed, create intermediate ring buffer for decoded audio
  if (this->needs_channel_conversion_) {
    this->audio_decoded_ring_buffer_ = RingBuffer::create(decoded_buffer_size);
    if (this->audio_decoded_ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create audio decoded ring buffer (%zu KB)", decoded_buffer_size / 1024);
      return false;
    }

    // Allocate temp buffer for channel conversion in PSRAM (optimal for ESP32-P4 DMA)
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    this->audio_temp_buffer_.reset(allocator.allocate(this->audio_temp_buffer_size_));
    if (!this->audio_temp_buffer_) {
      ESP_LOGE(TAG, "Failed to allocate audio temp buffer in PSRAM (%zu KB)", this->audio_temp_buffer_size_ / 1024);
      return false;
    }
    ESP_LOGD(TAG, "Channel conversion temp buffer allocated in PSRAM: %zu KB", this->audio_temp_buffer_size_ / 1024);
  }

  // Calculate AudioDecoder internal buffer sizes based on codec and sample rate
  // Input buffer: Should hold compressed frames (larger for higher bitrates)
  // Output buffer: Should hold decoded PCM frames
  size_t decoder_input_buffer_size = 64 * 1024;   // 64KB for compressed audio frames
  size_t decoder_output_buffer_size = 32 * 1024;  // 32KB for decoded PCM output

  // Adjust for high sample rates (>48kHz)
  if (this->audio_sample_rate_ > 48000) {
    decoder_input_buffer_size = 96 * 1024;   // 96KB for high-quality audio
    decoder_output_buffer_size = 48 * 1024;  // 48KB output
  }

  ESP_LOGD(TAG, "AudioDecoder buffers: input=%zu KB, output=%zu KB", decoder_input_buffer_size / 1024,
           decoder_output_buffer_size / 1024);

  // Create audio decoder with calculated buffer sizes
  this->audio_decoder_ = std::make_unique<audio::AudioDecoder>(decoder_input_buffer_size, decoder_output_buffer_size);

  // Add source ring buffer
  std::weak_ptr<RingBuffer> source_weak = this->audio_input_ring_buffer_;
  if (this->audio_decoder_->add_source(source_weak) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add audio decoder source");
    return false;
  }

  // Add sink based on whether channel conversion is needed
  if (this->needs_channel_conversion_) {
    // Decoder outputs to intermediate buffer (we'll convert in audio task)
    std::weak_ptr<RingBuffer> decoded_weak = this->audio_decoded_ring_buffer_;
    if (this->audio_decoder_->add_sink(decoded_weak) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to add audio decoder sink (intermediate buffer)");
      return false;
    }
  } else {
    // No conversion needed, decoder writes directly to speaker
    if (this->audio_decoder_->add_sink(this->speaker_) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to add audio decoder sink (speaker)");
      return false;
    }
  }

  // Start audio decoder
  if (this->audio_decoder_->start(codec_type) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start audio decoder");
    return false;
  }

  // Start audio processing task on Core 0 (video playback runs on Core 1)
  this->audio_task_stop_ = false;
  BaseType_t result = xTaskCreatePinnedToCore(audio_task_entry_, "svp_audio", 4096,  // 4KB stack
                                              this, 5,                               // Priority 5 (high)
                                              &this->audio_task_handle_,
                                              0);  // Core 0

  if (result != pdPASS || this->audio_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio processing task");
    return false;
  }

  this->audio_enabled_ = true;
  ESP_LOGI(TAG, "Audio decoder and processing task initialized successfully");
  return true;
}

void SimpleVideoPlayer::process_audio_frame_(const AVIFrame &frame, const uint8_t *data, size_t size) {
  if (!this->audio_enabled_ || this->audio_input_ring_buffer_ == nullptr) {
    ESP_LOGW(TAG, "process_audio_frame_: audio_enabled=%d, ring_buffer=%p - cannot process %zu bytes",
             this->audio_enabled_, (void *) this->audio_input_ring_buffer_.get(), size);
    return;
  }

  // Write audio frame to ring buffer (decoder is called continuously in main loop)
  size_t written = this->audio_input_ring_buffer_->write(data, size);
  if (written < size) {
    ESP_LOGW(TAG, "Audio ring buffer full, dropped %zu bytes (wrote %zu/%zu)", size - written, written, size);
  } else {
    ESP_LOGD(TAG, "Audio frame written to input buffer: %zu bytes (total available: %zu)", written,
             this->audio_input_ring_buffer_->available());
  }
}

bool SimpleVideoPlayer::convert_audio_channels_(const uint8_t *input_data, uint8_t *output_data, size_t frame_count,
                                                uint8_t input_channels, uint8_t output_channels,
                                                uint8_t bits_per_sample) {
  // Only 16-bit audio is supported
  if (bits_per_sample != 16) {
    ESP_LOGE(TAG, "Channel conversion only supports 16-bit audio, got %u-bit", bits_per_sample);
    return false;
  }

  const int16_t *input = reinterpret_cast<const int16_t *>(input_data);
  int16_t *output = reinterpret_cast<int16_t *>(output_data);

  // Stereo → Mono conversion
  if (input_channels == 2 && output_channels == 1) {
    for (size_t i = 0; i < frame_count; i++) {
      int16_t left = input[i * 2];
      int16_t right = input[i * 2 + 1];

      switch (this->speaker_channel_mode_) {
        case SpeakerChannelMode::SPEAKER_CHANNEL_LEFT:
          // Use only left channel
          output[i] = left;
          break;

        case SpeakerChannelMode::SPEAKER_CHANNEL_RIGHT:
          // Use only right channel
          output[i] = right;
          break;

        case SpeakerChannelMode::SPEAKER_CHANNEL_MONO:
        default:
          // Downmix: average both channels (with proper overflow handling)
          output[i] = (static_cast<int32_t>(left) + static_cast<int32_t>(right)) / 2;
          break;
      }
    }
    return true;
  }

  // Mono → Stereo conversion
  if (input_channels == 1 && output_channels == 2) {
    for (size_t i = 0; i < frame_count; i++) {
      int16_t sample = input[i];
      output[i * 2] = sample;      // Left
      output[i * 2 + 1] = sample;  // Right (duplicate)
    }
    return true;
  }

  // Pass-through (no conversion needed)
  if (input_channels == output_channels) {
    size_t bytes = frame_count * input_channels * (bits_per_sample / 8);
    memcpy(output_data, input_data, bytes);
    return true;
  }

  ESP_LOGE(TAG, "Unsupported channel conversion: %u → %u", input_channels, output_channels);
  return false;
}

void SimpleVideoPlayer::audio_task_entry_(void *param) {
  SimpleVideoPlayer *player = static_cast<SimpleVideoPlayer *>(param);
  player->audio_processing_loop_();
}

void SimpleVideoPlayer::audio_processing_loop_() {
  ESP_LOGI(TAG, "Audio processing task started on core %d", xPortGetCoreID());

  uint32_t loop_count = 0;
  uint32_t last_log_time = millis();

  while (!this->audio_task_stop_) {
    if (!this->audio_enabled_ || !this->audio_decoder_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    loop_count++;

    // Log buffer status every second for debugging
    if ((millis() - last_log_time) >= 1000) {
      size_t input_available = this->audio_input_ring_buffer_ ? this->audio_input_ring_buffer_->available() : 0;
      size_t decoded_available = this->audio_decoded_ring_buffer_ ? this->audio_decoded_ring_buffer_->available() : 0;
      ESP_LOGI(TAG, "Audio buffers: input=%zu bytes, decoded=%zu bytes, loops=%u/sec", input_available,
               decoded_available, loop_count);
      loop_count = 0;
      last_log_time = millis();
    }

    // Run audio decoder (decodes compressed audio to PCM)
    audio::AudioDecoderState decode_state = this->audio_decoder_->decode(false);

    if (decode_state == audio::AudioDecoderState::FAILED) {
      ESP_LOGE(TAG, "Audio decoding FAILED - decoder returned FAILED state");
      this->audio_enabled_ = false;
      break;
    }

    if (decode_state == audio::AudioDecoderState::DECODING) {
      ESP_LOGV(TAG, "Audio decoder DECODING");
    } else if (decode_state == audio::AudioDecoderState::FINISHED) {
      ESP_LOGD(TAG, "Audio decoder FINISHED");
    }

    // If channel conversion is needed, pull decoded data and convert it
    if (this->needs_channel_conversion_ && this->audio_decoded_ring_buffer_ && this->speaker_) {
      size_t available = this->audio_decoded_ring_buffer_->available();

      if (available > 0) {
        // Calculate how many frames we can process (limit to temp buffer size)
        size_t bytes_per_frame_input = this->source_audio_channels_ * 2;  // 16-bit = 2 bytes per sample
        size_t bytes_per_frame_output = this->speaker_audio_channels_ * 2;
        size_t max_input_bytes = std::min(available, this->audio_temp_buffer_size_);
        size_t frame_count = max_input_bytes / bytes_per_frame_input;

        if (frame_count > 0) {
          // Read decoded audio from intermediate buffer
          size_t bytes_to_read = frame_count * bytes_per_frame_input;
          size_t bytes_read = this->audio_decoded_ring_buffer_->read(this->audio_temp_buffer_.get(), bytes_to_read);

          if (bytes_read > 0) {
            // Perform channel conversion
            size_t actual_frames = bytes_read / bytes_per_frame_input;
            size_t output_bytes = actual_frames * bytes_per_frame_output;

            // For in-place conversion when output <= input size, use same buffer
            // Otherwise we'd need a second buffer (but this shouldn't happen for stereo→mono)
            if (this->convert_audio_channels_(this->audio_temp_buffer_.get(), this->audio_temp_buffer_.get(),
                                              actual_frames, this->source_audio_channels_,
                                              this->speaker_audio_channels_, 16)) {
              // Write converted audio to speaker
              this->speaker_->play(this->audio_temp_buffer_.get(), output_bytes);
            }
          }
        }
      } else {
        // No data available, yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    } else {
      // No conversion needed or passthrough mode - decoder writes directly to speaker
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  ESP_LOGI(TAG, "Audio processing task stopped");
  this->audio_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::stop_audio_task_() {
  if (this->audio_task_handle_ != nullptr) {
    ESP_LOGI(TAG, "Stopping audio processing task...");
    this->audio_task_stop_ = true;

    // Wait for task to finish (with timeout)
    uint32_t timeout_ms = 1000;
    uint32_t start = millis();
    while (this->audio_task_handle_ != nullptr && (millis() - start) < timeout_ms) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (this->audio_task_handle_ != nullptr) {
      ESP_LOGW(TAG, "Audio task didn't stop gracefully, deleting forcefully");
      vTaskDelete(this->audio_task_handle_);
      this->audio_task_handle_ = nullptr;
    }
  }
}
#endif

//========================================================================
// Error Handling
//========================================================================

void SimpleVideoPlayer::set_error_(PlaybackError error) {
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  this->last_error_ = error;
  this->state_ = PlayerState::ERROR;
  xSemaphoreGive(this->state_mutex_);

  this->on_error_callbacks_.call(static_cast<uint8_t>(error));
}

}  // namespace esphome::simple_video_player
