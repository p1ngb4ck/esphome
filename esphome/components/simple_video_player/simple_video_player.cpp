#include "simple_video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_dma_utils.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

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

// Output (decoded RGB565) double-buffer is sized once at setup for the largest video this player
// will ever be asked to play -- the ESP32-P4 target panel is 1280x800.
static constexpr uint32_t MAX_VIDEO_WIDTH = 1280;
static constexpr uint32_t MAX_VIDEO_HEIGHT = 800;

// Reserved out of every frame's nominal slot for LVGL's own render+flush pass (observed ~10-15ms
// for this panel) -- present_frame_()'s target time is deliberately this much EARLIER than the
// frame's full 1/fps mark, not the mark itself. Without this, decode+audio+prefetch consuming the
// full frame period leaves lv_timer_handler() no guaranteed window to actually render/flush
// before the next frame's own present is due, however "on time" our own decode pacing looks in
// isolation.
static constexpr int64_t LVGL_RENDER_RESERVE_US = 12000;

// Forward-declare the explicit specialization actually compiled for this backend (mirrors the
// JPEG_BACKEND selection in simple_video_player.h): setup()/playback_loop_() call these via the
// compile-time-constant JPEG_BACKEND before their out-of-line definitions appear further down in
// this file, and an explicit specialization must be declared before any implicit instantiation of
// that same template argument -- without this, the call site implicitly instantiates the
// (undefined) primary template, making the later explicit-specialization definition an error.
#if defined(USE_HWJPG)
template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::HW_P4>();
template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::HW_P4>(const uint8_t *frame_data,
                                                                              size_t frame_size);
#elif defined(USE_NEWJPEG)
template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::NEW_JPEG>();
template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::NEW_JPEG>(const uint8_t *frame_data,
                                                                                 size_t frame_size);
#else
template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::JPEGDEC>();
template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::JPEGDEC>(const uint8_t *frame_data,
                                                                                size_t frame_size);
#endif

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

  // Verify canvas is set
  if (this->canvas_ == nullptr) {
    ESP_LOGE(TAG, "Canvas not set");
    this->mark_failed();
    return;
  }

  // No VSYNC callback needed: canvas updates happen synchronously in decode_frame_(), under
  // lvgl_mutex_, the same way picture_viewer's update_canvas_() writes into its canvas buffer
  // directly and invalidates right after -- see canvas_buffer_'s comment in the header.
  if (this->lvgl_component_ == nullptr) {
    ESP_LOGE(TAG, "LVGL component not set");
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
  // ESP32-P4 only
  this->cache_buffer_.reset(
      static_cast<uint8_t *>(heap_caps_aligned_alloc(DMA_ALIGNMENT, this->cache_buffer_size_, MALLOC_CAP_INTERNAL)));

  if (!this->cache_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate cache buffer (%" PRIu32 " bytes)", this->cache_buffer_size_);
    this->mark_failed();
    return;
  }

  // Pre-allocate the JPEG decoder's input/output buffers during setup (backend-specific: see
  // init_decoder_backend_ specializations below). This ensures resources are allocated early
  // and won't fail during playback.
  if (!this->init_decoder_backend_<JPEG_BACKEND>()) {
    ESP_LOGE(TAG, "Failed to initialize JPEG decoder buffers");
    this->mark_failed();
    return;
  }

  // Video frame ring buffer: producer (loader task, Core 0) / consumer (decode task, Core 1).
  if (!this->allocate_frame_ring_()) {
    ESP_LOGE(TAG, "Failed to allocate video frame ring buffer");
    this->mark_failed();
    return;
  }

#ifdef USE_AUDIO
  // Audio ring buffers + temp buffer: allocated ONCE here, sized from the fixed AUDIO_* compile-
  // time constants (see header) -- not per play(). init_audio_decoder_() only validates each
  // file's actual audio format against this fixed configuration and resets/reuses these buffers.
  if (this->speaker_ != nullptr) {
    this->source_audio_channels_ = AUDIO_SOURCE_CHANNELS;
    this->audio_sample_rate_ = AUDIO_SAMPLE_RATE;
    this->audio_bits_per_sample_ = AUDIO_BITS_PER_SAMPLE;

    this->audio_input_ring_buffer_ = ring_buffer::RingBuffer::create(AUDIO_INPUT_BUFFER_SIZE);
    if (this->audio_input_ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create audio input ring buffer (%zu KB)", AUDIO_INPUT_BUFFER_SIZE / 1024);
      this->mark_failed();
      return;
    }

    this->audio_decoded_ring_buffer_ = ring_buffer::RingBuffer::create(AUDIO_DECODED_BUFFER_SIZE);
    if (this->audio_decoded_ring_buffer_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create audio decoded ring buffer (%zu KB)", AUDIO_DECODED_BUFFER_SIZE / 1024);
      this->mark_failed();
      return;
    }

    uint8_t *temp_buf = static_cast<uint8_t *>(heap_caps_malloc(AUDIO_TEMP_BUFFER_SIZE, MALLOC_CAP_SPIRAM));
    if (!temp_buf) {
      ESP_LOGE(TAG, "Failed to allocate audio temp buffer in PSRAM (%zu KB)", AUDIO_TEMP_BUFFER_SIZE / 1024);
      this->mark_failed();
      return;
    }
    this->audio_temp_buffer_.reset(temp_buf);

#if defined(SVP_AUDIO_CODEC_MP3) || defined(SVP_AUDIO_CODEC_FLAC)
    // audio_decoder_ itself: allocated ONCE here too, same as everything else above. The codec is
    // just as fixed by YAML as sample_rate/channels/bits_per_sample are (see __init__.py), so
    // whether this is ever needed at all is already known at compile time -- PCM mode never
    // touches it, so it's never constructed there. AudioDecoder's own start() (verified against
    // the real audio component source) already resets its per-file state (potentially_failed_
    // count_, end_of_file_, a fresh per-codec sub-decoder) on every call, and add_source()/
    // add_sink() are safe to call again on the same instance -- init_audio_decoder_() just calls
    // those again on this persistent instance instead of recreating the whole object, avoiding a
    // fresh output_transfer_buffer_ allocation every single play().
    this->audio_decoder_ =
        std::make_unique<audio::AudioDecoder>(AUDIO_DECODER_INPUT_BUFFER_SIZE, AUDIO_DECODER_OUTPUT_BUFFER_SIZE);
#endif

    ESP_LOGI(TAG, "Audio playback enabled with speaker (fixed format: %" PRIu32 " Hz, %u ch, %u-bit)",
             AUDIO_SAMPLE_RATE, AUDIO_SOURCE_CHANNELS, AUDIO_BITS_PER_SAMPLE);
  } else {
    ESP_LOGI(TAG, "Audio playback disabled (no speaker configured)");
  }
#else
  if (this->speaker_ != nullptr) {
    ESP_LOGI(TAG, "Audio playback enabled with speaker");
  } else {
    ESP_LOGI(TAG, "Audio playback disabled (no speaker configured)");
  }
#endif

  // Canvas buffer is deliberately NOT touched here: this->canvas_ is a pointer to a widget LVGL
  // itself creates and owns (from the lvgl: YAML's own canvas: block), and setup() order across
  // components isn't something this component controls -- LVGL's own widget-tree construction is
  // not guaranteed to have finished by the time THIS setup() runs. play() is triggered by an
  // explicit user/automation action well after boot, which is the only point this component can
  // be sure LVGL has actually finished building that widget -- see setup_canvas_buffer_()'s own
  // comment for the one-time-only call it makes there instead.

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
  ESP_LOGCONFIG(TAG, "  Cache buffer: %" PRIu32 " bytes (internal RAM)", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Frame ring: %" PRIu32 " slots x %" PRIu32 " bytes (PSRAM)", this->prefetch_frames_,
                this->input_buffer_size_);
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
  ESP_LOGCONFIG(TAG, "  Cache buffer size: %" PRIu32 " bytes", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Frame ring: %" PRIu32 " slots x %" PRIu32 " bytes", this->prefetch_frames_,
                this->input_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);

  if (this->state_ != PlayerState::STOPPED) {
    ESP_LOGCONFIG(TAG, "  Current file: %s", this->video_path_.c_str());
    ESP_LOGCONFIG(TAG, "  Video size: %" PRIu32 "x%" PRIu32, this->video_width_, this->video_height_);
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
    this->wait_for_task_stop_(this->task_handle_, 5000);
  }

  // Update state
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  this->video_path_ = video_path;
  this->state_ = PlayerState::PLAYING;
  this->last_error_ = PlaybackError::NONE;
  xSemaphoreGive(this->state_mutex_);

  // Create the decode/playback task on Core 1, alongside ESPHome's main loop task (which drives
  // App.loop() -> LvglComponent::loop() -> lv_timer_handler(), i.e. the actual LVGL
  // render/rotate/flush pipeline -- pinned there via esphome/components/esp32/core.cpp's
  // xTaskCreateStaticPinnedToCore(..., 1)).
  //
  // This used to run on Core 0 specifically to get away from the main loop task, to stop
  // FreeRTOS priority scheduling from starving it of CPU time whenever decode fell behind. That
  // traded one bug for a worse one: the ESP32-P4 hardware JPEG decoder uses DMA2D internally
  // (jpeg_decoder_process() -> dma2d_enqueue()), and this board's LVGL rotation uses PPA (also
  // DMA2D-based -- see lvgl_esphome.cpp's ppa_do_scale_rotate_mirror()). On one core, decode and
  // LVGL rendering could never truly execute at the same instant, which incidentally prevented
  // decode and PPA rotation from ever touching DMA2D concurrently. Splitting them across real
  // cores let that happen for the first time, hitting a real, open ESP-IDF hardware issue
  // (espressif/esp-idf#18999, "DMA2D dma2d_connect hangs indefinitely on ESP32-P4 ... under
  // continuous PPA load") -- decode hung forever on its very first call once PPA was actually
  // active concurrently, which look like "nothing ever decodes" from here.
  //
  // Back on Core 1: the per-frame yield fix below (vTaskDelay of at least one tick every cycle,
  // regardless of pacing) is what actually prevents the original starvation, without needing
  // physical core isolation that reintroduces a DMA2D hardware race.
  BaseType_t result = xTaskCreatePinnedToCore(playback_task_entry_, "video_player",
                                              8192,  // Stack size
                                              this,
                                              10,  // Priority (higher than main loop and most components)
                                              &this->task_handle_,
                                              1);  // Core 1

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
  ESP_LOGI(TAG, "Playback task started (Core 1)");

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
  ESP_LOGI(TAG, "Video dimensions: %" PRIu32 "x%" PRIu32, width, height);

  // No canvas widget resize/reposition here: this is a single, fixed-resolution panel, and the
  // canvas is already the correct size and position from YAML -- there is no placeholder-then-
  // grow case to support, so touching lv_obj_set_size()/lv_obj_set_pos() here was pure
  // unnecessary risk (and unnecessary lvgl_mutex_ contention) for a no-op in the common case.
  // Visibility (hidden flag, foreground order, which page/screen is active) is the caller's job,
  // not this component's -- expected usage is a dedicated page holding just the video canvas,
  // switched to by the caller's own action before play() and away from after stop().

  // Audio/speaker initialization -- independent of the video ring buffer, runs before the
  // loader/decode pipeline starts.
#ifdef USE_AUDIO
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    ESP_LOGI(TAG, "Initializing audio decoder and speaker...");
    if (!this->init_audio_decoder_()) {
      ESP_LOGW(TAG, "Audio initialization failed, continuing with video only");
    } else {
      ESP_LOGI(TAG, "Audio system ready");
    }
  }
#endif

  // Allocate output (decoded RGB565) buffers based on video size
  if (!this->allocate_buffers_(width, height)) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
    return;
  }

  if (this->canvas_buffer_[0] == nullptr) {
    // First play() ever, and only then: allocate both canvas_buffer_ slots and attach slot 0 to
    // LVGL. this->canvas_ is a pointer to a widget LVGL itself creates and owns (from the lvgl:
    // YAML's own canvas: block) -- setup() order across components does not guarantee LVGL has
    // finished building it yet, but by the time a play() action actually runs (well after boot,
    // triggered explicitly by the user/an automation) it is guaranteed to have.
    // No static timeout of any duration: xSemaphoreTake(..., 0) is a true non-blocking try-lock,
    // not a shortened wait -- it returns immediately whether it got the mutex or not. On a miss,
    // taskYIELD() (not vTaskDelay()) hands the CPU to whatever's holding it with no minimum wait
    // imposed, then we try again -- bounded by attempt count as a pure safety cap, not a time
    // budget, so this can never impose a fixed stall on top of real contention.
    bool canvas_ready = false;
    for (int attempt = 0; attempt < 1000 && !canvas_ready; attempt++) {
      if (xSemaphoreTake(this->lvgl_mutex_, 0) == pdTRUE) {
        canvas_ready = this->setup_canvas_buffer_();
        xSemaphoreGive(this->lvgl_mutex_);
      } else {
        taskYIELD();
      }
    }
    if (!canvas_ready) {
      ESP_LOGE(TAG, "Failed to set up canvas buffer for playback");
      this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
      this->close_file_();
      return;
    }
  } else if (this->canvas_buffer_ready_) {
    // Every later play(): both buffers are already allocated and already built into
    // canvas_draw_buf_[2] (their pointers never change), so there is nothing left to touch on
    // LVGL's side at all -- just clear our own private memory (both slots -- whichever is
    // currently attached AND the back one decode is about to target) back to black for this new
    // session. Raw writes to buffers we own, not LVGL API calls, need no lvgl_mutex_/blocking.
    // Deliberately NOT calling lv_obj_invalidate() here either: that was tried before (on the old
    // single-buffer design) and regressed the cold-start case -- present_frame_()'s own
    // invalidate, once the first real frame of this session is decoded, is what actually gets
    // this canvas its next redraw.
    size_t buffer_size =
        static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * sizeof(uint16_t);
    std::memset(this->canvas_buffer_[0], 0, buffer_size);
    std::memset(this->canvas_buffer_[1], 0, buffer_size);
  }

  // Reset file position to start (not needed for AVI - parser is already positioned at movi data)
  if (this->video_format_ != VideoFormat::AVI_MJPEG) {
    this->seek_to_(0);
  }
  this->cache_buffer_valid_ = 0;
  this->cache_buffer_offset_ = 0;

  // Reset the frame ring to a clean state (drain any leftovers from a previous session, so
  // semaphore counts and head/tail always start at "all slots free, none ready, index 0"
  // regardless of how the last playback ended).
  while (xSemaphoreTake(this->ring_slots_ready_, 0) == pdTRUE) {
  }
  while (xSemaphoreTake(this->ring_slots_free_, 0) == pdTRUE) {
  }
  for (uint32_t i = 0; i < this->prefetch_frames_; i++) {
    xSemaphoreGive(this->ring_slots_free_);
  }
  this->ring_head_ = 0;
  this->ring_tail_ = 0;

  // Fire started callback
  this->on_started_callbacks_.call();

  // Start the loader task on Core 0: it begins reading ahead into frame_ring_ immediately,
  // decoupled from this task's decode+pacing work entirely. Unlike decode, the loader is pure
  // storage I/O -- it never touches DMA2D/PPA/JPEG hardware, so it has no reason to share Core 1
  // with the main loop/decode the way decode itself now must (see play()'s task-creation comment
  // for the DMA2D/PPA hardware-serialization reason decode is pinned there). Keeping it on Core 0
  // instead of piling every task onto Core 1 actually uses both cores.
  this->loader_task_stop_ = false;
  BaseType_t loader_result = xTaskCreatePinnedToCore(loader_task_entry_, "svp_loader",
                                                     8192,  // Stack size
                                                     this,
                                                     9,  // Priority: below decode (10), above default
                                                     &this->loader_task_handle_,
                                                     0);  // Core 0
  if (loader_result != pdPASS) {
    ESP_LOGE(TAG, "Failed to create loader task");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
    return;
  }

#ifdef USE_AUDIO
  // Wait for audio buffer to have sufficient data -- runs in parallel with the loader task
  // above, which is already filling the video ring at the same time.
  if (this->audio_enabled_ && this->speaker_ != nullptr && this->audio_decoded_ring_buffer_) {
    // Calculate target: 200ms of audio for smooth startup
    size_t bytes_per_ms = (this->source_audio_channels_ * 2 * this->audio_sample_rate_) / 1000;
    size_t target_bytes = bytes_per_ms * 200;  // 200ms buffer

    ESP_LOGI(TAG, "Waiting for audio buffer to fill (target: %zu bytes)...", target_bytes);
    uint32_t wait_start = millis();
    while (this->audio_decoded_ring_buffer_->available() < target_bytes && (millis() - wait_start) < 1000) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    size_t buffered = this->audio_decoded_ring_buffer_->available();
    ESP_LOGI(TAG, "Audio buffer ready: %zu bytes (%.1f ms)", buffered, (float) buffered / bytes_per_ms);
  }
#endif

  // Buffer before starting the presentation clock: block until the ring is either fully
  // pre-filled (prefetch_frames_ slots ready) or the loader has already finished producing
  // everything it ever will (a short video reaching EOF, or a read error) -- whichever comes
  // first. Starting the clock immediately (as if frame 0's storage read were instant) is what
  // caused the endless "loader could not keep up" storm: the very first read pays real cold-start
  // latency (file open, first seek, first chunk parse) that a single frame's presentation budget
  // never covers, so every early cycle missed its deadline before the loader had a fair chance to
  // get ahead. No fixed give-up deadline here -- however long the initial fill genuinely takes is
  // how long we wait; uxSemaphoreGetCount() only queries the ring's real state, it doesn't
  // consume/perturb it.
  ESP_LOGI(TAG, "Buffering...");
  int64_t buffer_wait_start_us = esp_timer_get_time();
  while (uxSemaphoreGetCount(this->ring_slots_ready_) < this->prefetch_frames_ &&
         this->loader_task_handle_ != nullptr) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  ESP_LOGI(TAG, "Buffered %u/%" PRIu32 " ring slots in %" PRId64 " ms",
           static_cast<unsigned>(uxSemaphoreGetCount(this->ring_slots_ready_)), this->prefetch_frames_,
           (esp_timer_get_time() - buffer_wait_start_us) / 1000);

  // Initialize frame pacing with presentation timestamps. Frame 0's target presentation time is
  // "now", so the loop below presents it as soon as it's decoded -- the ring is already
  // sufficiently full at this point (or the whole video fit in it), removing the first-frame
  // stall without needing to keep re-deriving it cycle by cycle in the pacing loop itself.
  this->playback_start_time_us_ = esp_timer_get_time();
  this->frame_count_ = 0;
  this->frame_duration_us_ = 1000000.0f / this->target_fps_;  // e.g., 40000us for 25fps

  while (this->state_ != PlayerState::STOPPED) {
    // Handle pause state
    if (this->state_ == PlayerState::PAUSED) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Wait for the next ring slot. No artificial per-cycle deadline here -- matching the
    // single-task version this replaced, a slow read just means this frame gets presented late
    // (the "wait only if early" pacing below already handles that gracefully); it must not be
    // treated as a reason to give up on the frame and spin back around immediately, which only
    // starves the loader of CPU time and can never actually resolve on its own.
    xSemaphoreTake(this->ring_slots_ready_, portMAX_DELAY);

    VideoFrameSlot &slot = this->frame_ring_[this->ring_tail_];

    if (slot.status == VideoFrameSlot::Status::END_OF_FILE) {
      ESP_LOGI(TAG, "Playback finished");
      this->ring_tail_ = (this->ring_tail_ + 1) % this->prefetch_frames_;
      xSemaphoreGive(this->ring_slots_free_);
      this->on_finished_callbacks_.call();
      break;
    }
    if (slot.status == VideoFrameSlot::Status::READ_ERROR) {
      ESP_LOGE(TAG, "Failed to read frame");
      this->ring_tail_ = (this->ring_tail_ + 1) % this->prefetch_frames_;
      xSemaphoreGive(this->ring_slots_free_);
      this->set_error_(PlaybackError::FILE_READ_ERROR);
      break;
    }

    // Calculate when this frame should be PRESENTED -- deliberately earlier than its nominal
    // 1/fps mark by LVGL_RENDER_RESERVE_US, so lv_timer_handler() has a guaranteed window to
    // actually render+flush this frame before the next one's present is due (see that constant's
    // comment). Decode timing itself is NOT controlled here -- the hardware/software JPEG decoder
    // takes however long it takes; only the moment we hand the result to LVGL is paced.
    int64_t target_present_time_us = this->playback_start_time_us_ +
                                     static_cast<int64_t>(this->frame_count_ * this->frame_duration_us_) -
                                     LVGL_RENDER_RESERVE_US;

    bool decoded = this->decode_frame_(slot.data.get(), slot.size);

    // Release the slot back to the loader immediately -- decode_frame_backend_ has already
    // consumed everything it needs from it synchronously.
    this->ring_tail_ = (this->ring_tail_ + 1) % this->prefetch_frames_;
    xSemaphoreGive(this->ring_slots_free_);

    if (!decoded) {
      ESP_LOGW(TAG, "Failed to decode frame, skipping");
      continue;
    }

    // Wait until exactly the right moment to present -- not immediately when decode happens to
    // finish. This task runs at priority 10, pinned to Core 1 -- the SAME core ESPHome's main
    // loop (and therefore LvglComponent::loop()/lv_timer_handler(), which is what actually
    // renders, rotates, and flushes to the display) normally runs on; FreeRTOS priority scheduling
    // means the lower-priority main loop task can only run while THIS task is genuinely blocked,
    // so every wait here is also LVGL's only chance to get scheduled.
    //
    // Coarse vTaskDelay() for the bulk of the wait (real yield, arbitrary tick granularity), then
    // a tight taskYIELD()-spin for the final stretch to land close to the exact microsecond --
    // "nanosecond-precision" isn't achievable through FreeRTOS ticks, but this gets close without
    // ever hard-spinning (taskYIELD() still lets the main loop/LVGL run between checks). If
    // already behind (wait_time_us <= 0), both loops below execute zero iterations and
    // present_frame_() fires immediately -- fire-and-forget, no catch-up logic, matching this
    // MCU's actual constraint: there is no slack to catch up with, only less work to waste.
    int64_t current_time_us = esp_timer_get_time();
    int64_t wait_time_us = target_present_time_us - current_time_us;
    if (wait_time_us > 1000) {
      vTaskDelay(pdMS_TO_TICKS((wait_time_us - 1000) / 1000));
    }
    while (esp_timer_get_time() < target_present_time_us) {
      taskYIELD();
    }

    this->present_frame_();

    // Increment frame counter for next frame's presentation timestamp
    this->frame_count_++;

    // Feed watchdog every 100 frames to prevent task watchdog timeout during long playback
    if (this->frame_count_ % 100 == 0) {
#ifdef USE_ESP32
      esp_task_wdt_reset();
#endif
    }
  }

  // Stop the loader task before closing the file -- it must not still be reading via
  // file_reader_ once close_file_() tears it down. The loader's own wait on ring_slots_free_ is
  // bounded (50ms), so it notices loader_task_stop_ promptly regardless of ring state.
  this->loader_task_stop_ = true;
  this->wait_for_task_stop_(this->loader_task_handle_, 5000);

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

    // audio_decoder_ is permanent now too (allocated once in setup(), see there) -- deliberately
    // NOT reset()'d here, same reasoning as the ring buffers below: init_audio_decoder_() calls
    // add_source()/add_sink()/start() on it again right before the next session starts, which is
    // enough to reinitialize it for a new file (verified against the real audio component
    // source). audio_input_ring_buffer_/audio_decoded_ring_buffer_/audio_temp_buffer_ are the
    // same story -- init_audio_decoder_() clears the ring buffers itself, and the temp buffer
    // needs no clearing (fully overwritten before every read).
    this->audio_enabled_ = false;

    ESP_LOGI(TAG, "Audio processing stopped");
  }
#endif

  // Blank the canvas on stop -- same direct, mutex-protected write as decode_frame_(), since this
  // still runs on the decode/playback task itself. True non-blocking try-lock (0 timeout, not a
  // shortened wait), skip gracefully on failure: this is a cosmetic best-effort step (the canvas
  // simply keeps showing the last frame until the next play()), never worth any wait at all.
  if (xSemaphoreTake(this->lvgl_mutex_, 0) == pdTRUE) {
    if (this->canvas_buffer_ready_) {
      // Blank whichever slot is currently attached -- that's the one actually on screen.
      size_t frame_bytes = static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * 2;
      std::memset(this->canvas_buffer_[this->active_display_idx_], 0, frame_bytes);
      // Same order lv_canvas_fill_bg() uses: flush the CPU cache for the buffer BEFORE
      // invalidating, so the render pass reads the just-written bytes rather than stale cache
      // lines.
      lv_draw_buf_flush_cache(&this->canvas_draw_buf_[this->active_display_idx_], nullptr);
      lv_obj_invalidate(this->canvas_);
    }
    xSemaphoreGive(this->lvgl_mutex_);
  }

  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  this->state_ = PlayerState::STOPPED;
  xSemaphoreGive(this->state_mutex_);

  this->task_handle_ = nullptr;

  ESP_LOGI(TAG, "Playback task finished");
}

bool SimpleVideoPlayer::wait_for_task_stop_(TaskHandle_t &handle, uint32_t timeout_ms) {
  if (handle == nullptr) {
    return true;
  }

  uint32_t elapsed = 0;
  while (handle != nullptr && elapsed < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(10));
    elapsed += 10;
  }

  return handle == nullptr;
}

//========================================================================
// Loader Task (Core 0): reads ahead into frame_ring_, decoupled from decode/pacing
//========================================================================

void SimpleVideoPlayer::loader_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  player->loader_loop_();
  player->loader_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::loader_loop_() {
  ESP_LOGI(TAG, "Loader task started (Core 0)");

  while (!this->loader_task_stop_) {
    // Wait for a free ring slot; bounded so a stop request is noticed promptly even when the
    // ring is full (consumer stalled/stopped).
    if (xSemaphoreTake(this->ring_slots_free_, pdMS_TO_TICKS(50)) != pdTRUE) {
      continue;
    }
    if (this->loader_task_stop_) {
      xSemaphoreGive(this->ring_slots_free_);  // don't consume the token, we're not using it
      break;
    }

    VideoFrameSlot &slot = this->frame_ring_[this->ring_head_];
    int n = this->read_next_frame_(slot.data.get(), this->input_buffer_size_);

    if (n == 0 && this->loop_) {
      // EOF with looping enabled: rewind and retry without publishing a slot -- transparent to
      // the consumer, which never sees an EOF marker for a looping video.
      ESP_LOGI(TAG, "Looping video");
      this->seek_to_(0);
      this->cache_buffer_valid_ = 0;
      this->cache_buffer_offset_ = 0;
      xSemaphoreGive(this->ring_slots_free_);
      continue;
    }

    if (n > 0) {
      slot.size = static_cast<size_t>(n);
      slot.status = VideoFrameSlot::Status::FRAME_OK;
    } else {
      slot.size = 0;
      slot.status = n == 0 ? VideoFrameSlot::Status::END_OF_FILE : VideoFrameSlot::Status::READ_ERROR;
    }

    this->ring_head_ = (this->ring_head_ + 1) % this->prefetch_frames_;
    xSemaphoreGive(this->ring_slots_ready_);

    // EOF/error is terminal for this task -- the consumer will see it via the ring and stop
    // too, and there is nothing more useful for the loader to read.
    if (slot.status != VideoFrameSlot::Status::FRAME_OK) {
      break;
    }
  }

  ESP_LOGI(TAG, "Loader task finished");
}

//========================================================================
// Frame Processing
//========================================================================

int SimpleVideoPlayer::read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity) {
  // Read frame directly from file - runs on the loader task, writing into a frame_ring_ slot
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    // AVI format - use parser to get next frame (video or audio)
    AVIFrame frame;
    int bytes_read = this->avi_parser_->read_next_frame(frame, dest_buffer, dest_capacity);

    if (bytes_read <= 0) {
      return bytes_read;  // EOF or error
    }

    // Skip audio frames if no speaker, otherwise process them
    while (frame.stream_type != AVIStreamType::VIDEO) {
#ifdef USE_AUDIO
      if (frame.stream_type == AVIStreamType::AUDIO && this->audio_enabled_) {
        this->process_audio_frame_(frame, dest_buffer, bytes_read);
      }
#endif
      // Skip this frame (audio) and read next frame
      bytes_read = this->avi_parser_->read_next_frame(frame, dest_buffer, dest_capacity);
      if (bytes_read <= 0) {
        return bytes_read;
      }
    }

    return bytes_read;
  } else {
    // Raw MJPEG - search for JPEG EOI marker to find frame boundary
    size_t frame_size = 0;
    uint8_t *frame_ptr = dest_buffer;

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

        // Check if frame fits in the destination slot
        if (frame_size + chunk_size > dest_capacity) {
          ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %zu)", frame_size + chunk_size, dest_capacity);
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

        // Check if frame fits in the destination slot
        if (frame_size + search_len > dest_capacity) {
          ESP_LOGE(TAG, "Frame too large for input buffer (%zu > %zu)", frame_size + search_len, dest_capacity);
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

bool SimpleVideoPlayer::decode_frame_(const uint8_t *frame_data, size_t frame_size) {
  // Just decode -- straight into the "back" canvas_buffer_ slot (see that struct's header
  // comment). Presentation (attaching that slot to LVGL) is a separate, deliberately later step:
  // see present_frame_() and the pacing loop's own comment on why it's timed, not immediate.
  return this->decode_frame_backend_<JPEG_BACKEND>(frame_data, frame_size);
}

void SimpleVideoPlayer::present_frame_() {
  if (!this->canvas_buffer_ready_) {
    return;
  }
  // True non-blocking try-lock (0 timeout): this MCU's whole per-frame budget is ~40ms shared
  // across decode, audio, and storage prefetch, so even a "short" fixed wait here is a large
  // fraction of it every single frame, not a rounding error -- any wait at all is unaffordable on
  // this path. Skip gracefully on a miss: decode_frame_() will just overwrite the same
  // not-yet-shown buffer again next cycle -- a dropped frame's visual update, not a stall.
  if (xSemaphoreTake(this->lvgl_mutex_, 0) == pdTRUE) {
    int new_idx = 1 - this->active_display_idx_;
    // Cheap swap (pointer assignment + cache-drop, verified against the real LVGL 9.5 source) --
    // NOT lv_canvas_set_buffer(), which recomputes stride and reinitializes the whole draw_buf
    // struct on every call; canvas_draw_buf_[2] were already fully built once, in
    // setup_canvas_buffer_().
    lv_canvas_set_draw_buf(this->canvas_, &this->canvas_draw_buf_[new_idx]);
    // Flush the CPU cache for the buffer BEFORE invalidating, so the render pass reads the
    // just-written bytes rather than stale cache lines -- same order lv_canvas_fill_bg() uses.
    lv_draw_buf_flush_cache(&this->canvas_draw_buf_[new_idx], nullptr);
    lv_obj_invalidate(this->canvas_);
    this->active_display_idx_ = new_idx;
    xSemaphoreGive(this->lvgl_mutex_);
  }
}

//========================================================================
// JPEG Backend Implementations
//
// Exactly one of the three blocks below is compiled per build, selected by which of
// USE_HWJPG / USE_NEWJPEG / neither is defined -- the same defines JPEG_BACKEND
// (simple_video_player.h) is derived from. See runtime_image/jpeg_decoder.cpp for the same
// pattern applied to image decoding.
//========================================================================

#if defined(USE_HWJPG)

// ESP32-P4: hardware JPEG codec (esp_driver_jpeg). Unlike runtime_image's HW_P4 backend (which
// decodes a single still image and can afford a per-call engine open/close), video needs the
// engine created once here and held open for the whole playback session -- decode_frame_backend_
// below just reuses it every frame; creating/tearing it down 25+ times a second was catastrophic.
template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::HW_P4>() {
  ESP_LOGI(TAG, "Pre-allocating PSRAM buffers (hardware JPEG decoder, ESP32-P4)...");

  if (this->hw_jpeg_decoder_ == nullptr) {
    jpeg_decode_engine_cfg_t eng_cfg{};
    eng_cfg.intr_priority = 0;
    eng_cfg.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&eng_cfg, &this->hw_jpeg_decoder_) != ESP_OK) {
      ESP_LOGE(TAG, "Could not create hardware JPEG decoder engine");
      return false;
    }
  }

  // Compressed-frame input buffers now live in frame_ring_ (see allocate_frame_ring_()), not a
  // single input_buffer_ -- the ring is what lets the loader task (Core 0) read ahead of the
  // decode task (Core 1) instead of serializing I/O with decode+pacing on one task.
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // whichever canvas_buffer_[2] slot isn't currently attached to LVGL (see that struct's header
  // comment), allocated lazily from play() by setup_canvas_buffer_() instead -- this->canvas_ is
  // not guaranteed to exist yet at this point (setup() order across components), so there is
  // nothing to size a decode target against here.
  return true;
}

template<>
bool SimpleVideoPlayer::parse_header_backend_<JpegBackend::HW_P4>(const uint8_t *buffer, size_t size,
                                                                   uint32_t &width, uint32_t &height) {
  jpeg_decode_picture_info_t header;
  if (jpeg_decoder_get_info(buffer, static_cast<uint32_t>(size), &header) != ESP_OK) {
    return false;
  }
  width = header.width;
  height = header.height;
  return true;
}

template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::HW_P4>(const uint8_t *frame_data,
                                                                              size_t frame_size) {
  // Aligned to 16 bytes (hardware requirement) -- this matches the component's own original,
  // confirmed-working implementation (checked out separately at commit 54eb52387c for reference),
  // not picture_viewer's still-image call shape. The two aren't the same call: picture_viewer
  // decodes one full, standalone JPEG file with real EOF/EOI framing around it; this decodes a
  // frame carved out of a much larger interleaved AVI stream, and the hardware decoder here reads
  // up to aligned_size, not frame_size, regardless of what's declared.
  size_t aligned_size = ALIGN_UP(frame_size, 16);
  if (aligned_size > this->input_buffer_size_) {
    return false;
  }

  // Must match whatever byte order LVGL's own RGB565 canvas actually expects, not assume one --
  // same requirement, and same LV_COLOR_16_SWAP branch, as the NEW_JPEG and JPEGDEC backends
  // below (both of which point back to this comment). esphome/components/lvgl only defines
  // LV_COLOR_16_SWAP when color_depth is 16 (always true for RGB565 canvases), from
  // lvgl.byte_order -- which itself DEFAULTS to big_endian when neither the display nor the
  // lvgl: config sets it explicitly (see lvgl/__init__.py). The driver's own header documents BGR
  // order as "small endian" and RGB order as "big endian" output -- i.e. exactly
  // LV_COLOR_16_SWAP's two states -- so select between them at compile time instead of hardcoding
  // one and needing a separate manual byte-swap to compensate for the other.
  jpeg_decode_cfg_t decode_cfg{};
  decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
#if LV_COLOR_16_SWAP
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
#else
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
#endif

  // Decode straight into the "back" buffer (whichever slot ISN'T currently attached to LVGL) --
  // see canvas_buffer_'s header comment for why this is safe on this core-pinning and why no
  // extra scratch buffer/copy is needed.
  uint16_t *decode_target = this->canvas_buffer_[1 - this->active_display_idx_];
  size_t decode_target_capacity =
      static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * sizeof(uint16_t);

  uint32_t out_size = 0;
  jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, frame_data, static_cast<uint32_t>(aligned_size),
                       reinterpret_cast<uint8_t *>(decode_target), static_cast<uint32_t>(decode_target_capacity),
                       &out_size);

  return true;
}

#elif defined(USE_NEWJPEG)

// ESP32-S2/S3: esp_new_jpeg (SIMD-optimized software decoder). Buffers use heap_caps_aligned_alloc
// (16-byte aligned, which is all esp_new_jpeg requires) rather than jpeg_calloc_align, so
// free_buffers_()'s existing heap_caps_free() calls stay correct across every backend without
// needing their own dispatch.
template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::NEW_JPEG>() {
  ESP_LOGI(TAG, "Pre-allocating PSRAM buffers (esp_new_jpeg decoder)...");

  if (this->new_jpeg_decoder_ == nullptr) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    // Must match whatever byte order LVGL's RGB565 canvas actually expects -- see the HW_P4
    // backend's decode_frame_backend_ for why LV_COLOR_16_SWAP (not a hardcoded assumption) is
    // the correct thing to branch on here.
#if LV_COLOR_16_SWAP
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
#else
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
#endif
    if (jpeg_dec_open(&config, &this->new_jpeg_decoder_) != JPEG_ERR_OK) {
      ESP_LOGE(TAG, "Could not create esp_new_jpeg decoder");
      return false;
    }
  }

  // Compressed-frame input buffers live in frame_ring_ (see allocate_frame_ring_()).
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // whichever canvas_buffer_[2] slot isn't currently attached to LVGL (see that struct's header
  // comment), allocated lazily from play() by setup_canvas_buffer_() instead.
  return true;
}

template<>
bool SimpleVideoPlayer::parse_header_backend_<JpegBackend::NEW_JPEG>(const uint8_t *buffer, size_t size,
                                                                      uint32_t &width, uint32_t &height) {
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  jpeg_dec_handle_t decoder = nullptr;
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
    return false;
  }

  jpeg_dec_io_t io{};
  io.inbuf = const_cast<uint8_t *>(buffer);
  io.inbuf_len = static_cast<int>(size);

  jpeg_dec_header_info_t header_info;
  bool ok = jpeg_dec_parse_header(decoder, &io, &header_info) == JPEG_ERR_OK;
  jpeg_dec_close(decoder);
  if (!ok) {
    return false;
  }
  width = header_info.width;
  height = header_info.height;
  return true;
}

template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::NEW_JPEG>(const uint8_t *frame_data,
                                                                                size_t frame_size) {
  if (frame_size > this->input_buffer_size_) {
    ESP_LOGE(TAG, "Frame too large for input buffer");
    return false;
  }

  // Decode straight into the "back" buffer -- see canvas_buffer_'s header comment.
  jpeg_dec_io_t io{};
  io.inbuf = const_cast<uint8_t *>(frame_data);
  io.inbuf_len = static_cast<int>(frame_size);
  io.outbuf = reinterpret_cast<uint8_t *>(this->canvas_buffer_[1 - this->active_display_idx_]);

  jpeg_dec_header_info_t header_info;
  jpeg_error_t err = jpeg_dec_parse_header(this->new_jpeg_decoder_, &io, &header_info);
  if (err == JPEG_ERR_OK) {
    err = jpeg_dec_process(this->new_jpeg_decoder_, &io);
  }

  if (err != JPEG_ERR_OK) {
    ESP_LOGW(TAG, "esp_new_jpeg decode failed: %d", err);
    return false;
  }
  return true;
}

#else

// Other ESP32 variants: JPEGDEC (bitbank2, software fallback). JPEGDEC's draw callback has no
// user-`this` slot beyond setUserPointer(), so the destination buffer/stride is passed through
// that instead of touching the player instance from the callback.
struct SvpJpegDrawCtx {
  uint8_t *out;        // RGB565 destination buffer (the "back" canvas_buffer_ slot)
  uint32_t out_width;  // aligned row width, for stride
};

static int svp_jpegdec_draw_callback_(JPEGDRAW *jpeg) {
  auto *ctx = static_cast<SvpJpegDrawCtx *>(jpeg->pUser);
  for (int y = 0; y < jpeg->iHeight; y++) {
    uint16_t *dst_row = reinterpret_cast<uint16_t *>(ctx->out) + (jpeg->y + y) * ctx->out_width + jpeg->x;
    const uint16_t *src_row = jpeg->pPixels + y * jpeg->iWidth;
    std::memcpy(dst_row, src_row, jpeg->iWidth * sizeof(uint16_t));
  }
  return 1;
}

template<> bool SimpleVideoPlayer::init_decoder_backend_<JpegBackend::JPEGDEC>() {
  ESP_LOGI(TAG, "Pre-allocating PSRAM buffers (software JPEGDEC decoder)...");

  // Compressed-frame input buffers live in frame_ring_ (see allocate_frame_ring_()).
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // whichever canvas_buffer_[2] slot isn't currently attached to LVGL (see that struct's header
  // comment), allocated lazily from play() by setup_canvas_buffer_() instead.
  return true;
}

template<>
bool SimpleVideoPlayer::parse_header_backend_<JpegBackend::JPEGDEC>(const uint8_t *buffer, size_t size,
                                                                     uint32_t &width, uint32_t &height) {
  JPEGDEC jpeg;
  if (!jpeg.openRAM(const_cast<uint8_t *>(buffer), static_cast<int>(size), nullptr)) {
    return false;
  }
  width = jpeg.getWidth();
  height = jpeg.getHeight();
  jpeg.close();
  return true;
}

template<> bool SimpleVideoPlayer::decode_frame_backend_<JpegBackend::JPEGDEC>(const uint8_t *frame_data,
                                                                               size_t frame_size) {
  if (frame_size > this->input_buffer_size_) {
    ESP_LOGE(TAG, "Frame too large for input buffer");
    return false;
  }

  // Decode straight into the "back" buffer -- see canvas_buffer_'s header comment.
  JPEGDEC jpeg;
  SvpJpegDrawCtx ctx{reinterpret_cast<uint8_t *>(this->canvas_buffer_[1 - this->active_display_idx_]),
                     ALIGN_UP(this->video_width_, 16)};

  if (!jpeg.openRAM(const_cast<uint8_t *>(frame_data), static_cast<int>(frame_size), svp_jpegdec_draw_callback_)) {
    ESP_LOGW(TAG, "Could not open frame for decoding: %d", jpeg.getLastError());
    return false;
  }
  jpeg.setUserPointer(&ctx);
  // Must match whatever byte order LVGL's RGB565 canvas actually expects -- see the HW_P4
  // backend's decode_frame_backend_ for why LV_COLOR_16_SWAP (not a hardcoded assumption) is the
  // correct thing to branch on here.
#if LV_COLOR_16_SWAP
  jpeg.setPixelType(RGB565_BIG_ENDIAN);
#else
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
#endif

  bool ok = jpeg.decode(0, 0, 0);
  jpeg.close();

  if (!ok) {
    ESP_LOGW(TAG, "JPEGDEC decode failed: %d", jpeg.getLastError());
    return false;
  }
  return true;
}

#endif

bool SimpleVideoPlayer::get_video_dimensions_(uint32_t &width, uint32_t &height) {
  // ESP32-P4 only: Hardware JPEG decoder
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

    ESP_LOGI(TAG, "AVI video dimensions: %" PRIu32 "x%" PRIu32 ", FPS: %" PRIu32 "/%" PRIu32, width, height,
             video_info->fps_num, video_info->fps_den);
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
    if (!this->parse_header_backend_<JPEG_BACKEND>(this->cache_buffer_.get(), static_cast<size_t>(bytes_read), width,
                                                    height)) {
      ESP_LOGE(TAG, "Failed to parse JPEG header");
      return false;
    }

    // Store dimensions
    this->video_width_ = width;
    this->video_height_ = height;

    // Reset file position for playback
    this->seek_to_(0);
    this->cache_buffer_valid_ = 0;
    this->cache_buffer_offset_ = 0;

    return true;
  }
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
  ESP_LOGI(TAG, "Opening file: %s", path.c_str());

  // Storage-backed file reader: resolves the path against the storage registry and handles
  // local (filesystem) vs network storage transparently -- see buffered_file_reader.h. Created
  // once and reused for every play() call, not recreated each time: BufferedFileReader owns two
  // 1MB PSRAM read-ahead buffers, and destroying+recreating it per play() meant freeing and
  // fresh-malloc'ing 2MB of PSRAM on every single video start -- the exact "allocate once, reuse"
  // mistake already fixed for canvas_buffer_/output_buffer_, just not applied here too.
  // BufferedFileReader::open() already handles being called on an already-open instance (closes
  // first), and its own buffers are only allocated the first time (if (!buf) ...), so a fresh
  // open() after a previous close() reuses them rather than reallocating.
  if (!this->file_reader_) {
    this->file_reader_ = std::make_unique<BufferedFileReader>();
  }
  if (!this->file_reader_->open(path.c_str())) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
    return false;
  }
  this->file_reader_->prefill_cache();

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

  // Initialize AVI parser if needed. Created once and reused, same as file_reader_ above:
  // AVIParser::open() already resets all of its own state at the top (has_video_/has_audio_/
  // movi_offset_/movi_size_/current_offset_/current_frame_) before parsing, and close() owns no
  // buffers to free, so it's already fully self-resetting -- safe to keep the same instance
  // across every play() instead of destroying and reallocating it each time.
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    if (!this->avi_parser_) {
      this->avi_parser_ = std::make_unique<AVIParser>();
    }

    // Seek back to start for parser
    if (!this->seek_to_(0)) {
      ESP_LOGE(TAG, "Failed to seek to start for AVI parsing");
      this->close_file_();
      return false;
    }

    // Open AVI file
    if (!this->avi_parser_->open(this->file_reader_.get())) {
      ESP_LOGE(TAG, "Failed to parse AVI file");
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
  // Close AVI parser if open -- NOT reset()/destroyed, same reasoning as file_reader_ below:
  // it owns no buffers, and open() already fully re-initializes its own state on the next call.
  if (this->avi_parser_) {
    this->avi_parser_->close();
  }

  // Close file reader -- NOT reset()/destroyed: kept alive so its PSRAM read-ahead buffers are
  // reused by the next open_file_() instead of being freed and re-malloc'd every play() call.
  if (this->file_reader_) {
    this->file_reader_->close();
  }

  this->file_size_ = 0;
  this->video_format_ = VideoFormat::UNKNOWN;
}

int SimpleVideoPlayer::read_data_(uint8_t *buffer, size_t size) {
  if (!this->file_reader_ || !this->file_reader_->is_open()) {
    return -1;
  }
  return this->file_reader_->read(buffer, size);
}

bool SimpleVideoPlayer::seek_to_(uint64_t position) {
  if (!this->file_reader_ || !this->file_reader_->is_open()) {
    return false;
  }
  return this->file_reader_->seek(position);
}

bool SimpleVideoPlayer::get_file_size_(uint64_t &size) {
  if (!this->file_reader_ || !this->file_reader_->is_open()) {
    return false;
  }
  return this->file_reader_->get_size(&size);
}

//========================================================================
// Buffer Management
//========================================================================

bool SimpleVideoPlayer::allocate_buffers_(uint32_t video_width, uint32_t video_height) {
  // Nothing is allocated here any more -- the decode target is canvas_buffer_[2] (fixed at
  // MAX_VIDEO_WIDTH x MAX_VIDEO_HEIGHT, allocated once by setup_canvas_buffer_()), not a separate
  // output_buffer_ sized per call. This just verifies the actual video fits that fixed capacity
  // and that the frame ring exists, same checks as before.
  uint32_t aligned_width = ALIGN_UP(video_width, 16);
  uint32_t aligned_height = ALIGN_UP(video_height, 16);
  uint32_t aligned_max_width = ALIGN_UP(MAX_VIDEO_WIDTH, 16);
  uint32_t aligned_max_height = ALIGN_UP(MAX_VIDEO_HEIGHT, 16);

  ESP_LOGI(TAG, "Verifying buffers for %" PRIu32 "x%" PRIu32 " video (aligned: %" PRIu32 "x%" PRIu32 ")", video_width,
           video_height, aligned_width, aligned_height);

  if (!this->frame_ring_) {
    ESP_LOGE(TAG, "Frame ring buffer not pre-allocated (this should not happen)");
    return false;
  }

  if (aligned_width > aligned_max_width || aligned_height > aligned_max_height) {
    ESP_LOGE(TAG,
             "Video too large for the fixed canvas buffer: %" PRIu32 "x%" PRIu32 " exceeds %" PRIu32 "x%" PRIu32,
             aligned_width, aligned_height, aligned_max_width, aligned_max_height);
    return false;
  }

  ESP_LOGI(TAG, "Buffers verified - Input: %" PRIu32 " bytes", this->input_buffer_size_);
  return true;
}

void SimpleVideoPlayer::free_buffers_() {
#if defined(USE_HWJPG)
  if (this->hw_jpeg_decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->hw_jpeg_decoder_);
    this->hw_jpeg_decoder_ = nullptr;
  }
#elif defined(USE_NEWJPEG)
  if (this->new_jpeg_decoder_ != nullptr) {
    jpeg_dec_close(this->new_jpeg_decoder_);
    this->new_jpeg_decoder_ = nullptr;
  }
#endif

  // Both canvas_buffer_ slots are allocated by this component (setup_canvas_buffer_()) and only
  // ever handed to LVGL as a raw pointer (inside canvas_draw_buf_[2]) -- LVGL does not take
  // ownership or free them (confirmed against the real LVGL 9.5 source), so they're ours to free
  // here. jpeg_alloc_decoder_mem()'s memory (the HW_P4 case) is itself just a heap_caps_aligned_
  // calloc() under the hood (verified against the real esp_driver_jpeg source), so plain
  // heap_caps_free() is correct for it too, same as the plain heap_caps_malloc() case the other
  // two backends use.
  for (auto &buf : this->canvas_buffer_) {
    if (buf != nullptr) {
      heap_caps_free(buf);
      buf = nullptr;
    }
  }
  this->canvas_buffer_ready_ = false;

#ifdef USE_AUDIO
  // Permanent audio buffers (allocated once in setup(), see there) -- true end-of-life free, same
  // as output_buffer_/canvas_buffer_ above. audio_temp_buffer_ was heap_caps_malloc()'d (PSRAM),
  // so it needs heap_caps_free(), not its unique_ptr default deleter. The two ring buffers are
  // shared_ptr<RingBuffer> -- resetting them is enough, RingBuffer's own destructor frees its
  // internal storage.
  if (this->audio_temp_buffer_) {
    heap_caps_free(this->audio_temp_buffer_.release());
  }
  this->audio_input_ring_buffer_.reset();
  this->audio_decoded_ring_buffer_.reset();
#endif

  this->free_frame_ring_();
}

bool SimpleVideoPlayer::setup_canvas_buffer_() {
  // Called lazily from play(), on the first play() ever and never again -- see this function's
  // header comment for why that's the earliest safe point, not setup(). Fixed at MAX_VIDEO_WIDTH
  // x MAX_VIDEO_HEIGHT: this is a single, fixed-resolution panel (set correctly in YAML from the
  // start), never a variable one, so there is no per-play() "resize" case to support. Caller holds
  // lvgl_mutex_ already.
  uint32_t aligned_width = ALIGN_UP(MAX_VIDEO_WIDTH, 16);
  uint32_t aligned_height = ALIGN_UP(MAX_VIDEO_HEIGHT, 16);
  const size_t buffer_size = static_cast<size_t>(aligned_width) * aligned_height * sizeof(uint16_t);

  for (int i = 0; i < 2; i++) {
#if defined(USE_HWJPG)
    // jpeg_alloc_decoder_mem(), not plain heap_caps_malloc: the hardware decoder's 2D-DMA output
    // write needs both address and size aligned to its own cache/DMA2D alignment (see header
    // comment on canvas_buffer_) -- this is the one function that actually knows that alignment.
    jpeg_decode_memory_alloc_cfg_t output_cfg{};
    output_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t actual_size = 0;
    auto *new_buffer = static_cast<uint16_t *>(jpeg_alloc_decoder_mem(buffer_size, &output_cfg, &actual_size));
#else
    // Software decoders (esp_new_jpeg / JPEGDEC) have no DMA2D alignment requirement of their own.
    auto *new_buffer = static_cast<uint16_t *>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM));
#endif
    if (new_buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate canvas buffer %d in PSRAM: %zu bytes", i, buffer_size);
      return false;
    }
    this->canvas_buffer_[i] = new_buffer;
    std::memset(this->canvas_buffer_[i], 0, buffer_size);

    // data_size must be LV_DRAW_BUF_SIZE(w, h, cf) for THESE declared dimensions -- same as
    // canvas.py's own codegen always does (see git history on b8d0d450f5: a size inconsistent
    // with the declared width/height here is a real way for LVGL's draw pipeline to reject the
    // buffer outright). stride 0 means "auto: width * bytes-per-pixel".
    uint32_t draw_buf_size = LV_DRAW_BUF_SIZE(aligned_width, aligned_height, LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_init(&this->canvas_draw_buf_[i], aligned_width, aligned_height, LV_COLOR_FORMAT_RGB565, 0,
                     this->canvas_buffer_[i], draw_buf_size);
    lv_draw_buf_set_flag(&this->canvas_draw_buf_[i], LV_IMAGE_FLAGS_MODIFIABLE);
  }

  // Attach slot 0 initially; present_frame_() flips between the two from here on.
  lv_canvas_set_draw_buf(this->canvas_, &this->canvas_draw_buf_[0]);
  this->active_display_idx_ = 0;
  this->canvas_buffer_width_ = static_cast<int>(aligned_width);
  this->canvas_buffer_height_ = static_cast<int>(aligned_height);
  this->canvas_buffer_ready_ = true;

  ESP_LOGI(TAG, "Canvas buffers allocated (PSRAM): 2 x %zu bytes, %" PRIu32 "x%" PRIu32, buffer_size, aligned_width,
           aligned_height);
  return true;
}

bool SimpleVideoPlayer::allocate_frame_ring_() {
  if (this->prefetch_frames_ == 0) {
    ESP_LOGE(TAG, "prefetch_frames must be at least 1");
    return false;
  }

  this->frame_ring_ = std::make_unique<VideoFrameSlot[]>(this->prefetch_frames_);

  for (uint32_t i = 0; i < this->prefetch_frames_; i++) {
    uint8_t *buf = nullptr;
#if defined(USE_HWJPG)
    // Same DMA2D-alignment contract as the (now removed) single input_buffer_ used --
    // jpeg_decoder_process() requires it for its bit_stream argument too.
    jpeg_decode_memory_alloc_cfg_t input_cfg{};
    input_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    size_t actual_size = 0;
    buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(this->input_buffer_size_, &input_cfg, &actual_size));
#else
    buf = static_cast<uint8_t *>(heap_caps_malloc(this->input_buffer_size_, MALLOC_CAP_SPIRAM));
#endif
    if (buf == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate frame ring slot %" PRIu32 " (%" PRIu32 " bytes)", i,
               this->input_buffer_size_);
      return false;
    }
    this->frame_ring_[i].data.reset(buf);
  }

  this->ring_slots_free_ = xSemaphoreCreateCounting(this->prefetch_frames_, this->prefetch_frames_);
  this->ring_slots_ready_ = xSemaphoreCreateCounting(this->prefetch_frames_, 0);
  if (this->ring_slots_free_ == nullptr || this->ring_slots_ready_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create ring buffer semaphores");
    return false;
  }

  double total_mb = (static_cast<double>(this->prefetch_frames_) * this->input_buffer_size_) / (1024.0 * 1024.0);
  ESP_LOGI(TAG, "Video frame ring buffer: %" PRIu32 " slots x %" PRIu32 " bytes (%.2f MB total, PSRAM)",
           this->prefetch_frames_, this->input_buffer_size_, total_mb);
  return true;
}

void SimpleVideoPlayer::free_frame_ring_() {
  if (this->frame_ring_) {
    for (uint32_t i = 0; i < this->prefetch_frames_; i++) {
      if (this->frame_ring_[i].data) {
        heap_caps_free(this->frame_ring_[i].data.release());
      }
    }
    this->frame_ring_.reset();
  }
  if (this->ring_slots_free_ != nullptr) {
    vSemaphoreDelete(this->ring_slots_free_);
    this->ring_slots_free_ = nullptr;
  }
  if (this->ring_slots_ready_ != nullptr) {
    vSemaphoreDelete(this->ring_slots_ready_);
    this->ring_slots_ready_ = nullptr;
  }
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

  // This player is configured for ONE fixed audio format, resolved from the speaker's own YAML
  // config (sample_rate/channels/bits_per_sample -- see __init__.py's
  // _resolve_speaker_audio_format()) -- validate the file's actual format matches it instead of
  // resizing anything to fit. source_audio_channels_/audio_sample_rate_/audio_bits_per_sample_
  // were already set to the fixed AUDIO_* constants in setup() and never change here.
  if (audio_info->channels != AUDIO_SOURCE_CHANNELS || audio_info->sample_rate != AUDIO_SAMPLE_RATE ||
      audio_info->bits_per_sample != AUDIO_BITS_PER_SAMPLE) {
    ESP_LOGE(TAG,
             "Audio format mismatch: file's audio track is %" PRIu32 " Hz, %u ch, %u-bit, but the speaker is "
             "configured for %" PRIu32 " Hz, %u ch, %u-bit only -- re-encode the file's audio track to match the "
             "speaker's sample_rate/channel/bits_per_sample. Playing video-only.",
             audio_info->sample_rate, audio_info->channels, audio_info->bits_per_sample, AUDIO_SAMPLE_RATE,
             AUDIO_SOURCE_CHANNELS, AUDIO_BITS_PER_SAMPLE);
    return false;
  }

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

  // Codec is also fixed by YAML (audio_codec) -- SVP_AUDIO_CODEC_{PCM,MP3,FLAC} is the one define
  // set by codegen, so which branch is "live" is resolved at compile time. A file whose audio
  // track uses a different codec than configured is a hard mismatch, same as the format checks
  // above -- never silently reconfigure the decoder per file.
#if defined(SVP_AUDIO_CODEC_MP3)
  if (audio_info->codec != static_cast<uint32_t>(AVIAudioCodec::MP3)) {
    ESP_LOGE(TAG, "Audio codec mismatch: file's audio track is not MP3 (this player is configured for MP3 only, "
                  "codec=0x%04" PRIX32 "). Playing video-only.",
             audio_info->codec);
    return false;
  }
  audio::AudioFileType codec_type = audio::AudioFileType::MP3;
  ESP_LOGI(TAG, "Audio codec: MP3, %" PRIu32 " Hz, %u channels, %u bits", audio_info->sample_rate,
           audio_info->channels, audio_info->bits_per_sample);
#elif defined(SVP_AUDIO_CODEC_FLAC)
  if (audio_info->codec != static_cast<uint32_t>(AVIAudioCodec::FLAC)) {
    ESP_LOGE(TAG, "Audio codec mismatch: file's audio track is not FLAC (this player is configured for FLAC only, "
                  "codec=0x%04" PRIX32 "). Playing video-only.",
             audio_info->codec);
    return false;
  }
  audio::AudioFileType codec_type = audio::AudioFileType::FLAC;
  ESP_LOGI(TAG, "Audio codec: FLAC, %" PRIu32 " Hz, %u channels, %u bits", audio_info->sample_rate,
           audio_info->channels, audio_info->bits_per_sample);
#else  // SVP_AUDIO_CODEC_PCM (default)
  if (audio_info->codec != static_cast<uint32_t>(AVIAudioCodec::PCM)) {
    ESP_LOGE(TAG, "Audio codec mismatch: file's audio track is not raw PCM (this player is configured for PCM "
                  "only, codec=0x%04" PRIX32 "). Playing video-only.",
             audio_info->codec);
    return false;
  }
  // PCM audio in AVI is raw samples without WAV header -- handled directly without AudioDecoder.
  audio::AudioFileType codec_type = audio::AudioFileType::NONE;  // Signal that we don't need a decoder
  ESP_LOGI(TAG, "Audio codec: PCM (raw), %" PRIu32 " Hz, %u channels, %u bits - will process directly",
           audio_info->sample_rate, audio_info->channels, audio_info->bits_per_sample);
#endif

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
    ESP_LOGE(TAG, "Speaker failed to start within %" PRIu32 " ms", SPEAKER_INIT_TIMEOUT_MS);
    return false;
  }

  ESP_LOGI(TAG, "Speaker initialized: %u-bit, %u-channel, %" PRIu32 " Hz", audio_info->bits_per_sample,
           this->speaker_audio_channels_, audio_info->sample_rate);

  // Ring buffers + temp buffer already exist (allocated once in setup(), sized from the fixed
  // AUDIO_* constants) -- just clear out whatever a previous play() left in them so this session
  // starts clean. reset() is a cheap FreeRTOS ringbuffer reset, not a reallocation.
  this->audio_input_ring_buffer_->reset();
  if (this->audio_decoded_ring_buffer_) {
    this->audio_decoded_ring_buffer_->reset();
  }

  // For PCM audio, we don't need a decoder - just handle raw samples directly
  bool use_decoder = (codec_type != audio::AudioFileType::NONE);

  // audio_decoder_ itself is persistent now too (allocated once in setup(), see there) --
  // add_source()/add_sink()/start() are all safe to call again on the same instance for a new
  // file (verified against the real audio component source: start() resets its own per-file
  // state every call). Only compressed formats (MP3/FLAC) use it at all; PCM mode leaves it null.
  if (use_decoder) {
    // Add source ring buffer
    std::weak_ptr<ring_buffer::RingBuffer> source_weak = this->audio_input_ring_buffer_;
    if (this->audio_decoder_->add_source(source_weak) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to add audio decoder source");
      return false;
    }

    // Add sink based on whether channel conversion is needed
    if (this->needs_channel_conversion_) {
      // Decoder outputs to intermediate buffer (we'll convert in audio task)
      std::weak_ptr<ring_buffer::RingBuffer> decoded_weak = this->audio_decoded_ring_buffer_;
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

    ESP_LOGI(TAG, "Audio decoder initialized successfully");
  }  // End of if (use_decoder)

  // Start audio processing task on Core 0, alongside the loader -- like the loader, audio never
  // touches DMA2D/PPA/JPEG hardware, so unlike decode it has no reason to share Core 1 with the
  // main loop (see play()'s xTaskCreatePinnedToCore comment for why decode specifically must).
  // For PCM: task handles channel conversion and direct speaker output
  // For MP3/FLAC: task handles decoder + channel conversion + speaker output
  // Audio task priority 10 (same as decode) to prevent audio underruns
  this->audio_task_stop_ = false;
  BaseType_t result = xTaskCreatePinnedToCore(audio_task_entry_, "svp_audio", 4096,  // 4KB stack
                                              this, 10,  // Priority 10 (high - same as decode task)
                                              &this->audio_task_handle_,
                                              0);  // Core 0

  if (result != pdPASS || this->audio_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio processing task");
    return false;
  }

  this->audio_enabled_ = true;
  ESP_LOGI(TAG, "Audio processing initialized successfully (%s mode)", use_decoder ? "decoder" : "direct PCM");
  return true;
}

void SimpleVideoPlayer::process_audio_frame_(const AVIFrame &frame, const uint8_t *data, size_t size) {
  if (!this->audio_enabled_) {
    return;
  }

  // Dispatch by MODE, not by buffer presence: audio_input_ring_buffer_/audio_decoded_ring_buffer_
  // are both permanent, allocated unconditionally in setup() (see header), so they're non-null
  // regardless of codec -- audio_decoder_ (only ever created for MP3/FLAC, see
  // init_audio_decoder_()) and needs_channel_conversion_ are the real mode signals now.
  if (this->audio_decoder_) {
    // Compressed audio (MP3/FLAC): feed the decoder's input ring buffer.
    this->audio_input_ring_buffer_->write(data, size);
    return;
  }

  // PCM: data is already decoded - write only complete frames to avoid glitches.
  size_t bytes_per_frame = this->source_audio_channels_ * (this->audio_bits_per_sample_ / 8);
  size_t complete_frames = size / bytes_per_frame;
  size_t bytes_to_write = complete_frames * bytes_per_frame;

  // Log warning if we're dropping incomplete frames (shouldn't happen with well-formed AVI)
  if (size != bytes_to_write) {
    ESP_LOGW(TAG, "Dropped %zu bytes of incomplete audio frame", size - bytes_to_write);
  }
  if (bytes_to_write == 0) {
    return;
  }

  if (this->needs_channel_conversion_) {
    // Buffer PCM for audio_processing_loop_'s channel-conversion step.
    this->audio_decoded_ring_buffer_->write(data, bytes_to_write);
  } else if (this->speaker_) {
    // No conversion needed: straight to speaker. Explicit ticks_to_wait=0 (best-effort,
    // non-blocking), not the 2-arg overload: this runs on the loader task, same as the video
    // frame reads -- if the speaker backend's play() blocks when its internal buffer is full
    // (implementation-defined for the 2-arg overload, per this class's own doc comment), that
    // stalls video frame delivery too, since audio and video share this one task reading the
    // same interleaved AVI stream. Matches the same drop-rather-than-block philosophy the other
    // audio path above already gets for free from RingBuffer::write() (discards old data on
    // overflow instead of waiting).
    this->speaker_->play(data, bytes_to_write, 0);
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

  while (!this->audio_task_stop_) {
    if (!this->audio_enabled_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Run audio decoder if we have one (MP3/FLAC mode)
    // For PCM mode, audio_decoder_ is null and we skip decoding
    if (this->audio_decoder_) {
      audio::AudioDecoderState decode_state = this->audio_decoder_->decode(false);

      if (decode_state == audio::AudioDecoderState::FAILED) {
        ESP_LOGE(TAG, "Audio decoding FAILED");
        this->audio_enabled_ = false;
        break;
      }
    }  // End of if (this->audio_decoder_)

    // Channel conversion processing (runs for both decoder mode and PCM mode)
    // For decoder mode: pulls from decoded_ring_buffer (decoder output)
    // For PCM mode: pulls from decoded_ring_buffer (direct PCM frames)
    if (this->needs_channel_conversion_ && this->audio_decoded_ring_buffer_ && this->speaker_) {
      size_t available = this->audio_decoded_ring_buffer_->available();

      if (available > 0) {
        // Calculate how many frames we can process (limit to temp buffer size)
        size_t bytes_per_frame_input = this->source_audio_channels_ * 2;  // 16-bit = 2 bytes per sample
        size_t bytes_per_frame_output = this->speaker_audio_channels_ * 2;
        size_t max_input_bytes = std::min(available, AUDIO_TEMP_BUFFER_SIZE);
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
              // Handle partial writes - speaker might not accept all data if buffer is full
              size_t bytes_written = 0;
              size_t bytes_remaining = output_bytes;
              const uint8_t *write_ptr = this->audio_temp_buffer_.get();

              while (bytes_remaining > 0) {
                size_t written = this->speaker_->play(write_ptr, bytes_remaining);
                if (written > 0) {
                  bytes_written += written;
                  bytes_remaining -= written;
                  write_ptr += written;
                } else {
                  // Speaker buffer full, yield briefly and retry
                  vTaskDelay(pdMS_TO_TICKS(1));
                }
              }
            }
          }
        }
      } else {
        // No data available, yield briefly to other tasks
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    } else {
      // No conversion needed - yield briefly to avoid tight loop
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }  // End of while loop

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
