#include "simple_video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
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

// Sanity bound for the canvas buffer this component finds and attaches to (see
// attach_canvas_buffer_()) -- never allocated here, just validated against it. The ESP32-P4 target
// panel is 1280x800.
static constexpr uint32_t MAX_VIDEO_WIDTH = 1280;
static constexpr uint32_t MAX_VIDEO_HEIGHT = 800;

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

  if (this->present_timer_ != nullptr) {
    esp_timer_stop(this->present_timer_);
    esp_timer_delete(this->present_timer_);
  }

  if (this->state_mutex_ != nullptr) {
    vSemaphoreDelete(this->state_mutex_);
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

  // No VSYNC callback needed: canvas updates happen synchronously in present_frame_(), the same way
  // picture_viewer's update_canvas_() writes into its canvas buffer directly and invalidates right
  // after -- see canvas_buffer_'s comment in the header.
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

  // One-shot timer that wakes the playback task at each frame's exact presentation instant (see
  // the pacing loop in playback_loop_()). Task-dispatch, not ISR-dispatch: the callback only does
  // an xTaskNotifyGive(), and task dispatch has no CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
  // dependency. Created once, re-armed per frame, deleted in the destructor.
  const esp_timer_create_args_t present_timer_args = {
      .callback = &SimpleVideoPlayer::present_timer_cb_,
      .arg = this,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "svp_present",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&present_timer_args, &this->present_timer_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create presentation timer");
    this->mark_failed();
    return;
  }

  // Attach (never allocate -- see canvas_buffer_'s header comment) LVGL's own canvas buffer and
  // blank it, RIGHT HERE in setup(), not deferred to first play(). Verified against ESPHome's own
  // codegen (esphome/writer.py's generated main.cpp: every to_code()-emitted statement, including
  // LVGL's widget/buffer construction, runs in the generated top-level setup() BEFORE App.setup()
  // is called -- and App.setup() is what dispatches to every Component::setup() override,
  // including this one, in priority order, afterwards). So by the time ANY Component::setup()
  // runs, the canvas widget and its buffer already exist, unconditionally -- no retry loop needed.
  // No lock: no other task exists yet at this point in boot (play() hasn't run), so there is
  // nothing to serialize against.
  //
  // Why this has to happen at all: LVGL's canvas codegen (canvas.py) allocates its buffer with
  // lv_malloc_core() -> heap_caps_malloc() (verified against the real lvgl_esphome.cpp) -- plain
  // malloc, NOT zeroed. Left untouched, the canvas shows whatever garbage was already sitting in
  // that PSRAM from the moment it's built (well before any Component::setup() runs) until this
  // component's first play() -- a user/automation-triggered action, potentially a long time after
  // boot. That gap is what showed up as "canvas is garbage/broken at start".
  if (this->attach_canvas_buffer_()) {
    size_t buffer_size =
        static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * sizeof(uint16_t);
    std::memset(this->canvas_buffer_, 0, buffer_size);
    lv_draw_buf_flush_cache(this->canvas_draw_buf_, nullptr);
    lv_obj_invalidate(this->canvas_);
  }
  if (!this->canvas_buffer_ready_) {
    ESP_LOGE(TAG, "Failed to access canvas buffer at setup");
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

  // Derive the frame ring's slot count from the configured prefetch TIME and target_fps_ -- both
  // are already set (codegen's to_code() calls every setter before this component's setup() ever
  // runs). Rounded up (ceil) so the configured duration is a floor, never short-changed by integer
  // truncation, and floored at 2 so there is always at least one slot the loader can be filling
  // while decode holds the other. There's no separate upper cap here: prefetch_duration itself is
  // already YAML-range-validated (__init__.py), which is what actually bounds PSRAM cost.
  this->prefetch_frames_ = std::max<uint32_t>(
      2, static_cast<uint32_t>(std::ceil(
             (static_cast<double>(this->prefetch_duration_ms_) / 1000.0) * this->target_fps_)));
  ESP_LOGCONFIG(TAG, "  Prefetching %" PRIu32 "ms of source stream -> %" PRIu32 " ring slots at %.1f fps",
                this->prefetch_duration_ms_, this->prefetch_frames_, this->target_fps_);

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

  // Canvas buffer was already attached and blanked earlier in this same setup() -- see that
  // block's comment for why setup() itself is a safe, always-built point to do it (verified
  // against ESPHome's own codegen), not something that needs to wait for play().

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
  ESP_LOGCONFIG(TAG, "  Cache buffer: %" PRIu32 " bytes (internal RAM)", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Frame ring: budgeted for %" PRIu32 " frames x %" PRIu32 " bytes (PSRAM)",
                this->prefetch_frames_, this->input_buffer_size_);
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

void SimpleVideoPlayer::present_timer_cb_(void *arg) {
  auto *player = static_cast<SimpleVideoPlayer *>(arg);
  TaskHandle_t task = player->task_handle_;
  if (task != nullptr) {
    xTaskNotifyGive(task);
  }
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

  // Fresh A/V-sync state for this session, set BEFORE the audio task is created (in
  // init_audio_decoder_()) so that task captures generation 0 as its baseline and only reacts to
  // real re-syncs afterwards.
  this->resync_generation_.store(0, std::memory_order_release);
  this->audio_skip_until_us_.store(0, std::memory_order_release);
  this->audio_bytes_demuxed_.store(0, std::memory_order_release);
  this->paused_accum_us_ = 0;
  this->resync_active_ = false;
  this->resync_count_ = 0;
  this->resync_frames_dropped_ = 0;

  // No canvas widget resize/reposition here: this is a single, fixed-resolution panel, and the
  // canvas is already the correct size and position from YAML -- there is no placeholder-then-
  // grow case to support, so touching lv_obj_set_size()/lv_obj_set_pos() here was pure
  // unnecessary risk for a no-op in the common case.
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

  // Canvas buffer was already attached AND blanked once, in setup() (this component would have
  // mark_failed()'d and never reached play() at all otherwise) -- the pointer never changes since
  // it's LVGL's own. Every play() session just clears it back to black again. A raw write to a
  // buffer we merely reference, not an LVGL API call, needs no lock. Deliberately NOT calling
  // lv_obj_invalidate() here either: that regressed the cold-start case before --
  // present_frame_()'s own invalidate, once the first real frame of this session is decoded, is
  // what actually gets this canvas its next redraw.
  if (this->canvas_buffer_ready_) {
    size_t buffer_size =
        static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * sizeof(uint16_t);
    std::memset(this->canvas_buffer_, 0, buffer_size);
  }

  // Reset file position to start (not needed for AVI - parser is already positioned at movi data)
  if (this->video_format_ != VideoFormat::AVI_MJPEG) {
    this->seek_to_(0);
  }
  this->cache_buffer_valid_ = 0;
  this->cache_buffer_offset_ = 0;

  // Reset the frame ring to a clean state (drain any leftover bytes from a previous session --
  // RingBuffer::reset() discards everything currently in it).
  this->video_frame_ring_buffer_->reset();
  this->frames_in_ring_.store(0, std::memory_order_relaxed);

  // Fire started callback
  this->on_started_callbacks_.call();

  // Start the loader task on Core 0: it begins reading ahead into video_frame_ring_buffer_
  // immediately,
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

  // Buffer before starting the presentation clock: block until a SMALL startup threshold of
  // frames is ready, or the loader has already finished producing everything it ever will (a
  // short video reaching EOF, or a read error) -- whichever comes first. Starting the clock
  // immediately (as if frame 0's storage read were instant) is what caused the endless "loader
  // could not keep up" storm: the very first read pays real cold-start latency (file open, first
  // seek, first chunk parse) that a single frame's presentation budget never covers, so every
  // early cycle missed its deadline before the loader had a fair chance to get ahead.
  //
  // Deliberately NOT prefetch_frames_ here: that's the STEADY-STATE ring depth (now derived from
  // prefetch_duration, which can be several seconds' worth of frames -- see its header comment),
  // not a startup gate. Requiring the full configured depth to fill before EVER decoding a single
  // frame regressed this from "starts almost immediately" (the old fixed default of 8 frames) to
  // "nothing happens for however long dozens of frames take to read" the moment prefetch_duration
  // was raised past what 8 frames used to represent -- a real bug this session introduced, not a
  // tradeoff. A handful of frames is enough to absorb the cold-start latency the comment above
  // describes; the ring still fills to its full configured depth during normal playback, it just
  // doesn't gate the FIRST frame on that.
  static constexpr uint32_t STARTUP_FILL_TARGET = 4;
  uint32_t startup_fill_target = std::min(this->prefetch_frames_, STARTUP_FILL_TARGET);
  ESP_LOGI(TAG, "Buffering (target: %" PRIu32 " frames)...", startup_fill_target);
  int64_t buffer_wait_start_us = esp_timer_get_time();
  while (this->frames_in_ring_.load(std::memory_order_acquire) < startup_fill_target &&
         this->loader_task_handle_ != nullptr) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  ESP_LOGI(TAG, "Buffered %u/%" PRIu32 " frames in %" PRId64 " ms",
           static_cast<unsigned>(this->frames_in_ring_.load(std::memory_order_relaxed)), startup_fill_target,
           (esp_timer_get_time() - buffer_wait_start_us) / 1000);

  // Initialize frame pacing with presentation timestamps. Frame 0's target presentation time is
  // "now", so the loop below presents it as soon as it's decoded -- the ring is already
  // sufficiently full at this point (or the whole video fit in it), removing the first-frame
  // stall without needing to keep re-deriving it cycle by cycle in the pacing loop itself.
  this->playback_start_time_us_ = esp_timer_get_time();
  this->frame_count_ = 0;
  this->frame_duration_us_ = 1000000.0f / this->target_fps_;  // e.g., 40000us for 25fps

  // ERROR is terminal here too, not just STOPPED: set_error_() sets state_ to ERROR, and a fall-
  // through into the reads below (with a dead loader) would spin/hang. PAUSED keeps the loop alive.
  while (this->state_ == PlayerState::PLAYING || this->state_ == PlayerState::PAUSED) {
    // Handle pause state. Charge the wall time spent parked to paused_accum_us_ so it is excluded
    // from media_us -- a pause must not look like the stream falling behind and trigger a re-sync
    // on resume.
    if (this->state_ == PlayerState::PAUSED) {
      const int64_t pause_started_us = esp_timer_get_time();
      while (this->state_ == PlayerState::PAUSED) {
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      this->paused_accum_us_ += esp_timer_get_time() - pause_started_us;
      ulTaskNotifyTake(pdTRUE, 0);  // drop any present-timer notification that landed while parked
      continue;
    }

    // Pop the next frame to decode. next_frame_to_decode_() applies the A/V re-sync (drop stale
    // video without decoding, fast-forward audio) when the wall clock has run ahead of the stream;
    // otherwise it just returns the next frame in order. out_index is that frame's absolute
    // presentation index -- the pacing timestamp is anchored to it, not to a running counter, so
    // long-run average fps stays correct even across a drop.
    uint32_t frame_index = 0;
    const int payload = this->next_frame_to_decode_(frame_index);
    if (payload == -2) {
      break;  // stop/error, or an oversized frame -- ring is torn down and re-primed by next play()
    }
    if (payload == 0) {
      ESP_LOGI(TAG, "Playback finished");
      this->on_finished_callbacks_.call();
      break;
    }
    if (payload == -1) {
      ESP_LOGE(TAG, "Failed to read frame");
      this->set_error_(PlaybackError::FILE_READ_ERROR);
      break;
    }

    // When this frame should be PRESENTED: its nominal 1/fps mark on the master (wall-clock)
    // timeline, from its absolute index. Decode timing itself is NOT controlled here -- the JPEG
    // decoder takes however long it takes; only the moment the result is handed to LVGL is paced.
    const int64_t target_present_time_us = this->playback_start_time_us_ + this->paused_accum_us_ +
                                           static_cast<int64_t>(frame_index * this->frame_duration_us_);

    if (!this->decode_frame_(this->decode_read_buffer_.get(), static_cast<size_t>(payload))) {
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
    // Wait out the remainder with a one-shot high-resolution timer, NOT vTaskDelay(): vTaskDelay()
    // rounds up to a whole 1ms FreeRTOS tick, and on this frame budget that rounding is time the
    // decode/audio/prefetch pipeline never gets back. esp_timer is systimer-backed (64-bit
    // microsecond clock, no tick quantisation); its callback notifies this task, which is
    // genuinely Blocked in the meantime so the lower-priority main loop (LvglComponent::loop() ->
    // lv_timer_handler(): render, rotate, flush) gets the CPU. The task resumes one esp_timer
    // dispatch + context switch after the target instant -- far tighter than a tick, and no CPU
    // burned spinning.
    //
    // The loop re-checks because ulTaskNotifyTake() can also return on its 50ms cap (there so a
    // missed notification / a state change can't wedge the task forever); normally the timer
    // notification lands first, far inside that. If already behind (remaining <= 0) nothing waits
    // and present_frame_() fires immediately -- fire-and-forget, no catch-up, matching this MCU.
    ulTaskNotifyTake(pdTRUE, 0);  // drain any stale notification from a prior frame's timer
    while (true) {
      int64_t remaining_us = target_present_time_us - esp_timer_get_time();
      if (remaining_us <= 0)
        break;
      esp_timer_start_once(this->present_timer_, static_cast<uint64_t>(remaining_us));
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      esp_timer_stop(this->present_timer_);  // harmless if it already fired; disarm if we woke early
    }

    this->present_frame_();

    // frame_count_ tracks the absolute index of the last presented frame (+1). It comes from the
    // frame's own tag, not a blind ++ -- after an A/V re-sync drop it jumps forward with the tag.
    this->frame_count_ = frame_index + 1;

    // Feed watchdog periodically to prevent task watchdog timeout during long playback.
    if (this->frame_count_ % 100 == 0) {
#ifdef USE_ESP32
      esp_task_wdt_reset();
#endif
    }
  }

  // Disarm the presentation timer in case the loop exited (EOF/error/stop) with it still pending.
  esp_timer_stop(this->present_timer_);

  // One-line A/V re-sync summary -- safe here (the loop has exited, this is not the hot path).
  if (this->resync_count_ > 0) {
    ESP_LOGW(TAG, "A/V re-sync fired %" PRIu32 " time(s), %" PRIu32 " frames dropped total",
             this->resync_count_, this->resync_frames_dropped_);
  }

  // Stop the loader task before closing the file -- it must not still be reading via
  // file_reader_ once close_file_() tears it down. The loader's own write_without_replacement()
  // retries are bounded (50ms each, via push_ring_entry()), so it notices loader_task_stop_
  // promptly regardless of ring state.
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

  // Blank the canvas on stop -- same direct write as decode_frame_(), on the decode/playback task
  // itself. Cosmetic best-effort (the canvas otherwise keeps showing the last frame until the next
  // play()); no lock needed for the same reason as present_frame_() (see its header comment).
  if (this->canvas_buffer_ready_) {
    size_t frame_bytes = static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * 2;
    std::memset(this->canvas_buffer_, 0, frame_bytes);
    // Same order lv_canvas_fill_bg() uses: flush the CPU cache for the buffer BEFORE invalidating,
    // so the render pass reads the just-written bytes rather than stale cache lines.
    lv_draw_buf_flush_cache(this->canvas_draw_buf_, nullptr);
    lv_obj_invalidate(this->canvas_);
  }

  // Don't clobber an ERROR state recorded by set_error_() -- get_state()/get_last_error() must
  // still report the failure after the task exits.
  xSemaphoreTake(this->state_mutex_, portMAX_DELAY);
  if (this->state_ != PlayerState::ERROR) {
    this->state_ = PlayerState::STOPPED;
  }
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
// Loader Task (Core 0): reads ahead into video_frame_ring_buffer_, decoupled from decode/pacing
//========================================================================

void SimpleVideoPlayer::loader_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  player->loader_loop_();
  player->loader_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

namespace {
// Pushes one length-prefixed entry (see VIDEO_FRAME_EOF/VIDEO_FRAME_READ_ERROR's header comment)
// into a RingBuffer, retrying the real, bounded RingBuffer::write_without_replacement() block
// until it fits or *stop_requested goes true. Real block (a genuine FreeRTOS wait, not a spin) is
// what actually lets a lower-priority task run while this one waits for the consumer to drain
// room -- same lesson this component's own pacing loop learned about vTaskDelay() vs taskYIELD()
// this session, now via RingBuffer's own internal use of it instead of anything hand-rolled here.
bool push_ring_entry(ring_buffer::RingBuffer &ring, const uint8_t *data, size_t len,
                     const volatile bool &stop_requested) {
  size_t offset = 0;
  while (offset < len) {
    size_t written =
        ring.write_without_replacement(data + offset, len - offset, pdMS_TO_TICKS(50), true);
    if (written == 0) {
      if (stop_requested)
        return false;
      continue;
    }
    offset += written;
  }
  return true;
}
}  // namespace

void SimpleVideoPlayer::loader_loop_() {
  ESP_LOGI(TAG, "Loader task started (Core 0)");

  // Absolute presentation index of the next video frame this task emits. read_next_frame_() only
  // ever returns VIDEO frames (it consumes/skips audio internally), one per call, in order -- so
  // this is exactly the frame's presentation index, and it keeps counting across a loop rewind so
  // the consumer's monotonic pacing never sees it go backwards.
  uint32_t video_out_index = 0;

  while (!this->loader_task_stop_) {
    int n = this->read_next_frame_(this->loader_read_buffer_.get(), this->input_buffer_size_);

    if (n == 0 && this->loop_) {
      // EOF with looping enabled: rewind and retry without publishing anything -- transparent to
      // the consumer, which never sees an EOF marker for a looping video.
      ESP_LOGI(TAG, "Looping video");
      this->seek_to_(0);
      this->cache_buffer_valid_ = 0;
      this->cache_buffer_offset_ = 0;
      continue;
    }

    if (n > 0) {
      // Real frame: [uint32 frame_index][uint32 payload_size] header, then the payload.
      const uint32_t header[2] = {video_out_index, static_cast<uint32_t>(n)};
      if (!push_ring_entry(*this->video_frame_ring_buffer_, reinterpret_cast<const uint8_t *>(header),
                           sizeof(header), this->loader_task_stop_)) {
        break;  // stop requested while waiting for room for the header
      }
      if (!push_ring_entry(*this->video_frame_ring_buffer_, this->loader_read_buffer_.get(),
                           static_cast<size_t>(n), this->loader_task_stop_)) {
        break;  // stop requested mid-payload -- the header we already pushed is now a lie, but the
                // whole ring is drained and reset()'d before the next session (see playback_loop_())
      }
      this->frames_in_ring_.fetch_add(1, std::memory_order_release);
      video_out_index++;
      continue;
    }

    // EOF/error: push the lone sentinel (no index, no payload) and stop -- the consumer sees it
    // via the ring and stops too, and there is nothing more useful for the loader to read.
    const uint32_t sentinel = (n == 0) ? VIDEO_FRAME_EOF : VIDEO_FRAME_READ_ERROR;
    push_ring_entry(*this->video_frame_ring_buffer_, reinterpret_cast<const uint8_t *>(&sentinel),
                    sizeof(sentinel), this->loader_task_stop_);
    break;
  }

  ESP_LOGI(TAG, "Loader task finished");
}

//========================================================================
// Frame Processing
//========================================================================

int SimpleVideoPlayer::read_ring_entry_(uint32_t &out_index, uint8_t *dest, size_t dest_cap) {
  // Accumulate exactly `len` bytes from the ring into `buf`. RingBuffer::read() is a byte stream
  // and can return a short count (verified against ring_buffer.cpp) -- a 1-3 byte partial happens
  // when the loader had to split a header across two writes under a full ring. false == aborted
  // because state_ became STOPPED/ERROR while blocked.
  auto read_exact = [this](void *buf, size_t len) -> bool {
    auto *p = static_cast<uint8_t *>(buf);
    size_t offset = 0;
    while (offset < len) {
      offset += this->video_frame_ring_buffer_->read(p + offset, len - offset, pdMS_TO_TICKS(50));
      if (offset < len && (this->state_ == PlayerState::STOPPED || this->state_ == PlayerState::ERROR)) {
        return false;
      }
    }
    return true;
  };

  uint32_t first = 0;
  if (!read_exact(&first, sizeof(first))) {
    return -2;
  }
  if (first == VIDEO_FRAME_EOF) {
    return 0;
  }
  if (first == VIDEO_FRAME_READ_ERROR) {
    return -1;
  }

  out_index = first;
  uint32_t size = 0;
  if (!read_exact(&size, sizeof(size))) {
    return -2;
  }
  if (size > dest_cap) {
    // Framing corruption (should be impossible: the loader caps every frame at input_buffer_size_).
    // Terminal -- record it in the error state, no logging on this priority-10 path.
    this->set_error_(PlaybackError::DECODE_ERROR);
    return -2;
  }
  if (!read_exact(dest, size)) {
    return -2;
  }
  this->frames_in_ring_.fetch_sub(1, std::memory_order_acq_rel);
  return static_cast<int>(size);
}

int SimpleVideoPlayer::next_frame_to_decode_(uint32_t &out_index) {
  // want_index() = the frame that should be on screen right now, off the master (wall-clock)
  // timeline. Recomputed as we go: dropping frames takes real time, so the live edge keeps moving.
  auto want_index = [this]() -> uint32_t {
    const int64_t media_us = esp_timer_get_time() - this->playback_start_time_us_ - this->paused_accum_us_;
    return media_us > 0 ? static_cast<uint32_t>(media_us / this->frame_duration_us_) : 0;
  };

  int payload = this->read_ring_entry_(out_index, this->decode_read_buffer_.get(), this->input_buffer_size_);
  if (payload <= 0) {
    return payload;  // EOF / read error / aborted -- caller handles
  }

  if (out_index + RESYNC_LAG_FRAMES >= want_index()) {
    this->resync_active_ = false;  // caught up (or never behind) -- this lag episode, if any, is over
    return payload;                // pace this frame normally
  }

  // Fell behind by more than RESYNC_LAG_FRAMES: this MCU cannot catch up by decoding faster, so
  // don't try -- drop straight to the live edge without decoding the frames in between, and point
  // the audio side at the same media time (the loader skips demuxed audio up to audio_skip_until_us_
  // without feeding it). The one-time queue flush (resync_generation_ bump, watched by the audio
  // task) happens only at the START of a lag episode -- a persistently slow decoder must not
  // re-flush audio every frame, which would leave audio permanently silent. No logging on this
  // path: it is priority-10 and time-critical (AGENTS.md). resync_count_/resync_frames_dropped_
  // are plain counters, summarised once after the loop exits.
  if (!this->resync_active_) {
    this->resync_active_ = true;
    this->resync_count_++;
    this->resync_generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  uint32_t dropped = 0;
  for (uint32_t w = want_index(); out_index < w; w = want_index()) {
    this->audio_skip_until_us_.store(static_cast<int64_t>(w * this->frame_duration_us_),
                                    std::memory_order_release);
    if (++dropped > RESYNC_MAX_DROP) {
      // Delivery itself can't keep up with real time -- stop chasing a target we can't reach and
      // just present what we have. Playback becomes a low frame rate rather than an infinite drain.
      break;
    }
    payload = this->read_ring_entry_(out_index, this->decode_read_buffer_.get(), this->input_buffer_size_);
    if (payload <= 0) {
      this->resync_frames_dropped_ += dropped;
      return payload;  // hit EOF / error / abort mid-drop -- report it, caller handles uniformly
    }
  }
  this->resync_frames_dropped_ += dropped;
  return payload;  // first frame at/after the live edge, already in decode_read_buffer_
}

int SimpleVideoPlayer::read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity) {
  // Read frame directly from file - runs on the loader task, writing into loader_read_buffer_
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    // AVI format - use parser to get next frame (video or audio)
    AVIFrame frame;
    int bytes_read = this->avi_parser_->read_next_frame(frame, dest_buffer, dest_capacity);

    if (bytes_read <= 0) {
      return bytes_read;  // EOF or error
    }

    // Consume interleaved audio chunks until the next video frame.
    while (frame.stream_type != AVIStreamType::VIDEO) {
#ifdef USE_AUDIO
      if (frame.stream_type == AVIStreamType::AUDIO) {
        // Count every audio byte demuxed (fed or skipped) -- / bytes-per-second gives the audio
        // stream's media time, which is how an A/V re-sync knows how far to fast-forward audio.
        this->audio_bytes_demuxed_.fetch_add(static_cast<uint32_t>(bytes_read), std::memory_order_relaxed);
        if (this->audio_enabled_) {
          const uint64_t bytes_per_sec = static_cast<uint64_t>(this->source_audio_channels_) *
                                         (this->audio_bits_per_sample_ / 8) * this->audio_sample_rate_;
          const int64_t audio_media_us =
              bytes_per_sec > 0 ? static_cast<int64_t>(this->audio_bytes_demuxed_.load(std::memory_order_relaxed) *
                                                       1000000ULL / bytes_per_sec)
                                : 0;
          if (audio_media_us >= this->audio_skip_until_us_.load(std::memory_order_acquire)) {
            this->process_audio_frame_(frame, dest_buffer, bytes_read);
          }
          // else: dropping stale audio while catching up to the re-sync point
        }
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
  // Just decode -- straight into canvas_buffer_, in place (see that member's header comment).
  // Presentation (cache flush + invalidate) is a separate, deliberately later step: see
  // present_frame_() and the pacing loop's own comment on why it's timed, not immediate.
  return this->decode_frame_backend_<JPEG_BACKEND>(frame_data, frame_size);
}

void SimpleVideoPlayer::present_frame_() {
  if (!this->canvas_buffer_ready_) {
    return;
  }
  // No lock: this runs on the decode/playback task (Core 1, priority 10), the same core as the
  // main loop / lv_timer_handler() but at higher priority, so LVGL's render only runs while this
  // task is blocked -- never concurrently with this write (see canvas_buffer_'s header comment).
  // The removed lvgl_mutex_ here was this component's own mutex, which LVGL's renderer never took,
  // so it synchronized nothing.
  //
  // Flush the CPU cache for the buffer BEFORE invalidating, so the render pass reads the
  // just-written bytes rather than stale cache lines -- same order lv_canvas_fill_bg() uses. No
  // lv_canvas_set_draw_buf()/lv_canvas_set_buffer() call here at all: this is still the same
  // lv_draw_buf_t LVGL's own codegen attached, never swapped -- see canvas_buffer_'s header comment
  // for why re-attaching a different buffer broke rendering.
  lv_draw_buf_flush_cache(this->canvas_draw_buf_, nullptr);
  lv_obj_invalidate(this->canvas_);
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

  // Compressed-frame input buffers now live in video_frame_ring_buffer_ (see
  // allocate_frame_ring_()), not a single input_buffer_ -- the ring is what lets the loader task
  // (Core 0) read ahead of the decode task (Core 1) instead of serializing I/O with decode+pacing
  // on one task.
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // canvas_buffer_, LVGL's own existing canvas pixel buffer (see that member's header comment),
  // fetched lazily from play() by attach_canvas_buffer_() instead -- this->canvas_ is not
  // guaranteed to have its buffer built yet at this point (setup() order across components), so
  // there is nothing to fetch a decode target from here.
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

  // Decode straight into canvas_buffer_ -- LVGL's own existing canvas pixel buffer, in place. See
  // that member's header comment for why this is safe on this core-pinning and why no extra
  // scratch buffer/copy is needed.
  uint16_t *decode_target = this->canvas_buffer_;
  size_t decode_target_capacity =
      static_cast<size_t>(this->canvas_buffer_width_) * this->canvas_buffer_height_ * sizeof(uint16_t);

  uint32_t out_size = 0;
  jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, frame_data, static_cast<uint32_t>(aligned_size),
                       reinterpret_cast<uint8_t *>(decode_target), static_cast<uint32_t>(decode_target_capacity),
                       &out_size);

  return true;
}

#elif defined(USE_NEWJPEG)

// ESP32-S2/S3: esp_new_jpeg (SIMD-optimized software decoder). Decodes directly into
// canvas_buffer_ (LVGL's own buffer, never allocated or freed by this component -- see that
// member's header comment), same as every other backend.
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

  // Compressed-frame input buffers live in video_frame_ring_buffer_ (see allocate_frame_ring_()).
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // canvas_buffer_, LVGL's own existing canvas pixel buffer (see that member's header comment),
  // fetched lazily from play() by attach_canvas_buffer_() instead.
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

  // Decode straight into canvas_buffer_, in place -- see that member's header comment.
  jpeg_dec_io_t io{};
  io.inbuf = const_cast<uint8_t *>(frame_data);
  io.inbuf_len = static_cast<int>(frame_size);
  io.outbuf = reinterpret_cast<uint8_t *>(this->canvas_buffer_);

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
  uint8_t *out;        // RGB565 destination buffer (canvas_buffer_, in place)
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

  // Compressed-frame input buffers live in video_frame_ring_buffer_ (see allocate_frame_ring_()).
  //
  // No decode output buffer allocated here: decode_frame_backend_ below writes directly into
  // canvas_buffer_, LVGL's own existing canvas pixel buffer (see that member's header comment),
  // fetched lazily from play() by attach_canvas_buffer_() instead.
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

  // Decode straight into canvas_buffer_, in place -- see that member's header comment.
  JPEGDEC jpeg;
  SvpJpegDrawCtx ctx{reinterpret_cast<uint8_t *>(this->canvas_buffer_), ALIGN_UP(this->video_width_, 16)};

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
  // Nothing is allocated here at all -- the decode target is canvas_buffer_, LVGL's own existing
  // canvas pixel buffer (fetched once by attach_canvas_buffer_()), not a separate output_buffer_
  // sized per call. This just verifies the actual video fits that buffer's fixed capacity and that
  // the frame ring exists, same checks as before.
  uint32_t aligned_width = ALIGN_UP(video_width, 16);
  uint32_t aligned_height = ALIGN_UP(video_height, 16);
  uint32_t aligned_max_width = ALIGN_UP(MAX_VIDEO_WIDTH, 16);
  uint32_t aligned_max_height = ALIGN_UP(MAX_VIDEO_HEIGHT, 16);

  ESP_LOGI(TAG, "Verifying buffers for %" PRIu32 "x%" PRIu32 " video (aligned: %" PRIu32 "x%" PRIu32 ")", video_width,
           video_height, aligned_width, aligned_height);

  if (!this->video_frame_ring_buffer_) {
    ESP_LOGE(TAG, "Video frame ring buffer not pre-allocated (this should not happen)");
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

  // canvas_buffer_/canvas_draw_buf_ are NOT freed here, or anywhere in this component: they are
  // LVGL's own, owned by the canvas widget's codegen, never allocated by us in the first place
  // (see canvas_buffer_'s header comment). Just drop our references to them.
  this->canvas_draw_buf_ = nullptr;
  this->canvas_buffer_ = nullptr;
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

bool SimpleVideoPlayer::attach_canvas_buffer_() {
  // Called once from setup() -- see this function's header comment for why that's a safe point.
  // Nothing is allocated here: LVGL's own canvas codegen (canvas.py) already built and attached
  // this buffer before any Component::setup() runs -- this just reads the pointer/size/format back
  // out of the widget. No lock needed: no other task exists this early in boot.
  lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(this->canvas_);
  if (draw_buf == nullptr || draw_buf->data == nullptr) {
    ESP_LOGE(TAG, "Canvas has no draw buffer yet (LVGL widget tree not fully built?)");
    return false;
  }

  uint32_t width = draw_buf->header.w;
  uint32_t height = draw_buf->header.h;
  if (width == 0 || height == 0 || width > MAX_VIDEO_WIDTH || height > MAX_VIDEO_HEIGHT) {
    ESP_LOGE(TAG, "Canvas buffer is %" PRIu32 "x%" PRIu32 ", expected non-zero and <= %" PRIu32 "x%" PRIu32,
             width, height, MAX_VIDEO_WIDTH, MAX_VIDEO_HEIGHT);
    return false;
  }
  // This decoder only ever writes RGB565 -- must match what the canvas: YAML block declared
  // (transparent: false, the default -- see canvas.py's to_code(), which picks
  // LV_COLOR_FORMAT_NATIVE for that case, itself RGB565 for every color_depth: 16 build).
  if (draw_buf->header.cf != LV_COLOR_FORMAT_RGB565 && draw_buf->header.cf != LV_COLOR_FORMAT_NATIVE) {
    ESP_LOGE(TAG, "Canvas color format (%d) is not RGB565 -- set canvas: transparent: false (the default)",
             draw_buf->header.cf);
    return false;
  }

  this->canvas_draw_buf_ = draw_buf;
  this->canvas_buffer_ = reinterpret_cast<uint16_t *>(draw_buf->data);
  this->canvas_buffer_width_ = static_cast<int>(width);
  this->canvas_buffer_height_ = static_cast<int>(height);
  this->canvas_buffer_ready_ = true;

  ESP_LOGI(TAG, "Canvas buffer attached (LVGL-owned): %" PRIu32 "x%" PRIu32, width, height);
  return true;
}

bool SimpleVideoPlayer::allocate_frame_ring_() {
  // Defensive only: setup() derives and floors prefetch_frames_ (>= 2) before ever calling this.
  if (this->prefetch_frames_ == 0) {
    ESP_LOGE(TAG, "prefetch_frames_ was never derived from prefetch_duration_ms_ (this should not happen)");
    return false;
  }

  // Byte budget: worst case, every buffered frame is a full input_buffer_size_, plus one 4-byte
  // length header per frame -- see video_frame_ring_buffer_'s header comment for why real
  // (usually smaller) frames pack in more tightly than that in practice. This is easily hundreds
  // of KB to multiple MB -- MUST be PSRAM. This MCU's internal SRAM is only 512KB total, shared
  // with the WiFi/BT stacks, every task's own stack, and everything else already resident there --
  // an allocation this size EVER landing there, even as a "fallback", is a guaranteed crash, not a
  // degraded-but-working state.
  //
  // ring_buffer::RingBuffer::create()'s MemoryPreference has no true "external-only, hard-fail"
  // mode: verified against the real RAMAllocator source (esphome/core/helpers.h) --
  // MemoryPreference::EXTERNAL_FIRST maps to RAMAllocator::NONE, whose get_caps_() enables BOTH
  // regions (external primary, internal SECONDARY) via heap_caps_malloc_prefer(), not
  // ALLOC_EXTERNAL alone (which RingBuffer's own API never exposes a way to request). So the
  // fallback this component must never take is reachable through create() itself, silently, if we
  // just called it and trusted the result.
  //
  // Closed by refusing to even ATTEMPT the allocation unless PSRAM already has enough contiguous
  // free space for it: heap_caps_malloc_prefer() only ever falls through to its second (internal)
  // capability set when the FIRST (external) attempt fails outright -- so confirming ahead of time
  // that the first attempt WILL succeed, via the same MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT capability
  // set RAMAllocator's external path itself uses, makes the internal-fallback branch provably
  // unreachable for this call, without needing to patch the upstream ring_buffer component itself.
  size_t ring_bytes = static_cast<size_t>(this->prefetch_frames_) * (this->input_buffer_size_ + sizeof(uint32_t));
  size_t psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (psram_largest_block < ring_bytes) {
    ESP_LOGE(TAG,
             "Refusing to allocate the %zu-byte video frame ring buffer: only %zu bytes of contiguous PSRAM free. "
             "This MUST come from PSRAM (internal SRAM is only 512KB, shared with everything else) -- lower "
             "prefetch_duration or input_buffer_size in YAML, or free PSRAM elsewhere.",
             ring_bytes, psram_largest_block);
    return false;
  }
  this->video_frame_ring_buffer_ =
      ring_buffer::RingBuffer::create(ring_bytes, ring_buffer::RingBuffer::MemoryPreference::EXTERNAL_FIRST);
  if (!this->video_frame_ring_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate video frame ring buffer (%zu bytes, PSRAM)", ring_bytes);
    return false;
  }

  // loader_read_buffer_: plain PSRAM, no DMA2D alignment needed -- it's only ever memcpy'd out of
  // (into video_frame_ring_buffer_ by RingBuffer::write_without_replacement()), never handed to
  // hardware directly.
  this->loader_read_buffer_.reset(
      static_cast<uint8_t *>(heap_caps_malloc(this->input_buffer_size_, MALLOC_CAP_SPIRAM)));
  if (!this->loader_read_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate loader read buffer (%" PRIu32 " bytes, PSRAM)", this->input_buffer_size_);
    return false;
  }

  // decode_read_buffer_: THIS one is handed to the hardware JPEG decoder as its compressed-input
  // (bit_stream) argument, which needs the same DMA2D/cache alignment as any other buffer the
  // decoder touches directly (see decode_frame_backend_<HW_P4>()) -- jpeg_alloc_decoder_mem(), not
  // plain heap_caps_malloc, same reasoning this component has used for every hardware-facing
  // buffer all along.
  uint8_t *decode_buf = nullptr;
#if defined(USE_HWJPG)
  jpeg_decode_memory_alloc_cfg_t input_cfg{};
  input_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t actual_size = 0;
  decode_buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(this->input_buffer_size_, &input_cfg, &actual_size));
#else
  decode_buf = static_cast<uint8_t *>(heap_caps_malloc(this->input_buffer_size_, MALLOC_CAP_SPIRAM));
#endif
  if (decode_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate decode read buffer (%" PRIu32 " bytes, PSRAM)", this->input_buffer_size_);
    return false;
  }
  // .reset(), not construction via make_unique: this pointer came from heap_caps_malloc/
  // jpeg_alloc_decoder_mem, not `new[]` -- it must be freed with heap_caps_free() (explicit
  // release()+heap_caps_free() in free_frame_ring_() below), never left to unique_ptr<uint8_t[]>'s
  // own default deleter (delete[]), which would be the wrong allocator's free function.
  this->decode_read_buffer_.reset(decode_buf);

  double total_mb = static_cast<double>(ring_bytes) / (1024.0 * 1024.0);
  ESP_LOGI(TAG, "Video frame ring buffer: %.2f MB (PSRAM, budgeted for ~%" PRIu32 " frames at %" PRIu32 " bytes each)",
           total_mb, this->prefetch_frames_, this->input_buffer_size_);
  return true;
}

void SimpleVideoPlayer::free_frame_ring_() {
  this->video_frame_ring_buffer_.reset();
  // Both scratch buffers came from heap_caps_malloc/jpeg_alloc_decoder_mem, not `new[]` -- release()
  // + heap_caps_free(), never unique_ptr's own default deleter (delete[], the wrong allocator's
  // free function). Same reasoning as canvas_buffer_'s allocation used to need before it was
  // removed in favor of LVGL's own buffer.
  if (this->loader_read_buffer_) {
    heap_caps_free(this->loader_read_buffer_.release());
  }
  if (this->decode_read_buffer_) {
    heap_caps_free(this->decode_read_buffer_.release());
  }
  this->frames_in_ring_.store(0, std::memory_order_relaxed);
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

  // A single failed decode drops that chunk and the loop keeps going, instead of tearing audio
  // down on the first hiccup. Only give up on audio entirely after this many consecutive failures
  // (a genuinely broken stream).
  static constexpr uint32_t AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES = 10;
  uint32_t audio_decode_failures = 0;

  // A/V re-sync: the pace controller bumps resync_generation_ when it drops stale video to the
  // live edge. On a change, drop everything this side has queued (encoded + decoded audio) -- the
  // loader is already fast-forwarding the file's audio to the same media time, so what's buffered
  // here is stale. The MP3/FLAC decoder is left alone: it will consume its partial in-flight frame
  // then resync on the next frame header on its own (one glitchy frame), which is cheaper than
  // recreating the sub-decoder (a heap allocation -- AGENTS.md: none after setup()). The speaker
  // keeps its own small residual (there is no cheap flush for it -- stop()/start() rebuilds the
  // I2S driver); it drains that and then briefly goes quiet until the skipped audio flows through.
  uint32_t last_resync_generation = this->resync_generation_.load(std::memory_order_acquire);

  while (!this->audio_task_stop_) {
    if (!this->audio_enabled_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    const uint32_t resync_generation = this->resync_generation_.load(std::memory_order_acquire);
    if (resync_generation != last_resync_generation) {
      last_resync_generation = resync_generation;
      // No logging: priority-10 path (AGENTS.md). The video side counts the re-sync.
      if (this->audio_input_ring_buffer_) {
        this->audio_input_ring_buffer_->reset();
      }
      if (this->audio_decoded_ring_buffer_) {
        this->audio_decoded_ring_buffer_->reset();
      }
      audio_decode_failures = 0;
    }

    // Run audio decoder if we have one (MP3/FLAC mode)
    // For PCM mode, audio_decoder_ is null and we skip decoding
    if (this->audio_decoder_) {
      audio::AudioDecoderState decode_state = this->audio_decoder_->decode(false);

      if (decode_state == audio::AudioDecoderState::FAILED) {
        if (++audio_decode_failures >= AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES) {
          ESP_LOGE(TAG, "Audio decode failed %" PRIu32 " times in a row -- disabling audio",
                   audio_decode_failures);
          this->audio_enabled_ = false;
          break;
        }
        ESP_LOGW(TAG, "Audio decode error -- dropping chunk (%" PRIu32 "/%" PRIu32 ")", audio_decode_failures,
                 AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES);
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      audio_decode_failures = 0;
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
