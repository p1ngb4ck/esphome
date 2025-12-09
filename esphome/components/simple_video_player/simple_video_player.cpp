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

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
  ESP_LOGCONFIG(TAG, "  Cache buffer: %u bytes (internal RAM)", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Input buffer: %u bytes (PSRAM)", this->input_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Output buffer: %zu bytes (PSRAM)", this->output_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);
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
    lv_canvas_set_buffer(this->canvas_, this->output_buffer_.get(), aligned_width, aligned_height,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(this->canvas_);
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

  // Reset file position to start
  this->seek_to_(0);
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
  size_t frame_size = 0;
  uint8_t *frame_ptr = this->input_buffer_.get();

  // Search for JPEG EOI marker to find frame boundary
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
        ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %u)", frame_size + chunk_size, this->input_buffer_size_);
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
        ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %u)", frame_size + search_len, this->input_buffer_size_);
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
  // Read first chunk to get JPEG header
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
#else
  ESP_LOGE(TAG, "Hardware JPEG decoder not available");
  return false;
#endif
}

//========================================================================
// File I/O Abstraction
//========================================================================

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

  return true;
}

void SimpleVideoPlayer::close_file_() {
  if (this->file_reader_) {
    this->file_reader_->close();
    this->file_reader_.reset();
  }

  this->is_network_file_ = false;
  this->file_size_ = 0;
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
