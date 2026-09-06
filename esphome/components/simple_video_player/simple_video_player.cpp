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
    std::memset(this->canvas_buffer_, 0, this->frame_bytes_);
    lv_draw_buf_flush_cache(this->canvas_draw_buf_, nullptr);
    lv_obj_invalidate(this->canvas_);
  }
  if (!this->canvas_buffer_ready_) {
    ESP_LOGE(TAG, "Failed to access canvas buffer at setup");
    this->mark_failed();
    return;
  }

  // Buffer B: an exact twin of LVGL's canvas buffer (A) -- same byte size -- so decode can
  // ping-pong A/B and loop() just re-points canvas_draw_buf_->data. HW decoder OUTPUT must come
  // from jpeg_alloc_decoder_mem(). Seed it with a copy of A (already blanked above).
  {
    jpeg_decode_memory_alloc_cfg_t out_cfg{};
    out_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t b_actual = 0;
    this->decode_buffer_b_ =
        static_cast<uint16_t *>(jpeg_alloc_decoder_mem(this->frame_bytes_, &out_cfg, &b_actual));
    if (this->decode_buffer_b_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate decode buffer B (%zu bytes)", this->frame_bytes_);
      this->mark_failed();
      return;
    }
    std::memcpy(this->decode_buffer_b_, this->canvas_buffer_, this->frame_bytes_);
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

  if (!this->init_decoder_()) {
    ESP_LOGE(TAG, "Failed to initialize JPEG decoder buffers");
    this->mark_failed();
    return;
  }

  // Compressed-frame input for the HW decoder: one buffer, jpeg_alloc_decoder_mem() INPUT-aligned.
  // Size rounded up to 16 so decode_frame_()'s ALIGN_UP(frame_size, 16) can never exceed it
  // (read_next_frame_ already caps frame_size at input_buffer_size_) -- no per-frame bound check.
  {
    jpeg_decode_memory_alloc_cfg_t in_cfg{};
    in_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    size_t in_actual = 0;
    this->decode_read_buffer_.reset(static_cast<uint8_t *>(
        jpeg_alloc_decoder_mem(ALIGN_UP(this->input_buffer_size_, 16), &in_cfg, &in_actual)));
    if (!this->decode_read_buffer_) {
      ESP_LOGE(TAG, "Failed to allocate decode input buffer (%" PRIu32 " bytes, PSRAM)", this->input_buffer_size_);
      this->mark_failed();
      return;
    }
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
  ESP_LOGCONFIG(TAG, "  Decode input buffer: %" PRIu32 " bytes (PSRAM)", this->input_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Audio: Enabled");
  }
#endif
}

void SimpleVideoPlayer::loop() {
  // Only place LVGL is touched for a frame update, and only on the LVGL thread. present_frame_()
  // (playback task) publishes the just-decoded pixels; retarget LVGL's own canvas draw buf at them
  // and re-assert + invalidate (the working youkorr lvgl_camera_display pattern -- the canvas draw
  // buf has LV_IMAGE_FLAGS_MODIFIABLE from codegen, so LVGL re-reads it).
  if (this->frame_ready_.exchange(false, std::memory_order_acq_rel)) {
    uint16_t *pixels = this->ready_pixels_.load(std::memory_order_acquire);
    if (pixels != nullptr && this->canvas_draw_buf_ != nullptr) {
      this->canvas_draw_buf_->data = reinterpret_cast<uint8_t *>(pixels);
      lv_canvas_set_draw_buf(this->canvas_, this->canvas_draw_buf_);
      lv_obj_invalidate(this->canvas_);
    }
  }
}

void SimpleVideoPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Simple Video Player:");
  ESP_LOGCONFIG(TAG, "  Cache buffer size: %" PRIu32 " bytes", this->cache_buffer_size_);
  ESP_LOGCONFIG(TAG, "  Decode input buffer: %" PRIu32 " bytes", this->input_buffer_size_);
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

  // Update state. video_path_ is written here, before the playback task is created below (the
  // task creation is a full memory barrier), and only read by that task -- no lock needed.
  // state_ / last_error_ are atomic.
  this->video_path_ = video_path;
  this->last_error_.store(PlaybackError::NONE, std::memory_order_relaxed);
  this->playback_task_stop_ = false;
  this->state_.store(PlayerState::PLAYING, std::memory_order_release);

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
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);  // sets state_ = ERROR
  }
}

void SimpleVideoPlayer::pause() {
  PlayerState expected = PlayerState::PLAYING;
  if (this->state_.compare_exchange_strong(expected, PlayerState::PAUSED, std::memory_order_acq_rel)) {
    ESP_LOGI(TAG, "Pausing playback");
    this->on_paused_callbacks_.call();
  }
}

void SimpleVideoPlayer::resume() {
  PlayerState expected = PlayerState::PAUSED;
  if (this->state_.compare_exchange_strong(expected, PlayerState::PLAYING, std::memory_order_acq_rel)) {
    ESP_LOGI(TAG, "Resuming playback");
  }
}

void SimpleVideoPlayer::stop() {
  PlayerState prev = this->state_.exchange(PlayerState::STOPPED, std::memory_order_acq_rel);
  if (prev != PlayerState::STOPPED) {
    ESP_LOGI(TAG, "Stopping playback");
  }
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
  this->decode_fail_count_ = 0;
  this->decode_slot_ = 0;
  this->video_frame_index_ = 0;

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
    std::memset(this->canvas_buffer_, 0, this->frame_bytes_);
    // Write the zeros back to PSRAM now (CPU->memory) so no dirty cache line can evict over the
    // first frame's DMA decode later. C2M (flush), not M2C: the CPU just wrote this buffer.
    // esp_cache_msync, not lv_draw_buf_flush_cache(): this runs on the playback task, and the
    // LVGL cache wrappers must not be called off the LVGL thread.
    esp_cache_msync(this->canvas_buffer_, this->frame_bytes_,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  }

  // Reset file position to start (not needed for AVI - parser is already positioned at movi data)
  if (this->video_format_ != VideoFormat::AVI_MJPEG) {
    this->seek_to_(0);
  }
  this->cache_buffer_valid_ = 0;
  this->cache_buffer_offset_ = 0;

  this->on_started_callbacks_.call();

  // Frame 0's target presentation time is "now"; BufferedFileReader's own read-ahead absorbs the
  // cold-start I/O latency.
  this->playback_start_time_us_ = esp_timer_get_time();
  this->frame_count_ = 0;
  this->frame_duration_us_ = 1000000.0f / this->target_fps_;  // e.g., 40000us for 25fps

  // One state load per iteration. Anything but PLAYING/PAUSED (STOPPED, ERROR) ends the loop.
  while (true) {
    const PlayerState st = this->state_.load(std::memory_order_acquire);
    if (st != PlayerState::PLAYING && st != PlayerState::PAUSED) {
      break;
    }
    // Charge parked wall time to paused_accum_us_ so a pause is not seen as the stream falling
    // behind (which would trigger a re-sync on resume).
    if (st == PlayerState::PAUSED) {
      const int64_t pause_started_us = esp_timer_get_time();
      while (this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
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
      break;  // stopped / aborted
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
      // No logging on this priority-10 path (AGENTS.md) -- plain counter, summarised after the loop.
      this->decode_fail_count_++;
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
    this->decode_slot_ ^= 1;  // next frame decodes into the other buffer

    // frame_count_ tracks the absolute index of the last presented frame (+1), from the frame tag.
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

  // One-line playback-health summary -- safe here (the loop has exited, this is not the hot path).
  if (this->resync_count_ > 0 || this->decode_fail_count_ > 0) {
    ESP_LOGW(TAG, "playback health: %" PRIu32 " re-sync(s), %" PRIu32 " frames dropped, %" PRIu32 " decode failures",
             this->resync_count_, this->resync_frames_dropped_, this->decode_fail_count_);
  }

  // Release any in-flight BufferedFileReader wait before close_file_() tears the reader down.
  this->playback_task_stop_ = true;
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

  // Blank the canvas on stop -- runs on the playback task, so same split as present_frame_():
  // memset + cache flush here (CPU wrote, so C2M), publish buffer A for loop() to show. Cosmetic
  // best-effort (the canvas otherwise keeps showing the last frame).
  if (this->canvas_buffer_ready_) {
    std::memset(this->canvas_buffer_, 0, this->frame_bytes_);
    esp_cache_msync(this->canvas_buffer_, this->frame_bytes_,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    this->ready_pixels_.store(this->canvas_buffer_, std::memory_order_release);
    this->frame_ready_.store(true, std::memory_order_release);
  }

  // Don't clobber an ERROR state recorded by set_error_() -- get_state()/get_last_error() must
  // still report the failure after the task exits. CAS from anything-but-ERROR to STOPPED.
  PlayerState expected = this->state_.load(std::memory_order_acquire);
  while (expected != PlayerState::ERROR &&
         !this->state_.compare_exchange_weak(expected, PlayerState::STOPPED, std::memory_order_acq_rel)) {
  }

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
// Frame Processing
//========================================================================

int SimpleVideoPlayer::read_frame_() {
  while (true) {
    const PlayerState s = this->state_.load(std::memory_order_acquire);
    if (s != PlayerState::PLAYING && s != PlayerState::PAUSED) {
      return -2;  // stopped / aborted
    }
    int n = this->read_next_frame_(this->decode_read_buffer_.get(), this->input_buffer_size_);
    if (n > 0) {
      return n;
    }
    if (n == 0 && this->loop_) {
      this->seek_to_(0);
      this->cache_buffer_valid_ = 0;
      this->cache_buffer_offset_ = 0;
      continue;
    }
    return n;  // 0 = EOF, -1 = read error
  }
}

int SimpleVideoPlayer::next_frame_to_decode_(uint32_t &out_index) {
  // want_index() = the frame that should be on screen right now, off the master (wall-clock)
  // timeline. Recomputed as we go: dropping frames takes real time, so the live edge keeps moving.
  auto want_index = [this]() -> uint32_t {
    const int64_t media_us = esp_timer_get_time() - this->playback_start_time_us_ - this->paused_accum_us_;
    return media_us > 0 ? static_cast<uint32_t>(media_us / this->frame_duration_us_) : 0;
  };

  int payload = this->read_frame_();
  if (payload <= 0) {
    return payload;  // EOF / read error / aborted -- caller handles
  }
  out_index = this->video_frame_index_++;

  if (out_index + RESYNC_LAG_FRAMES >= want_index()) {
    this->resync_active_ = false;  // caught up (or never behind) -- this lag episode, if any, is over
    return payload;                // pace this frame normally
  }

  // Fell behind by more than RESYNC_LAG_FRAMES: this MCU cannot catch up by decoding faster, so
  // read straight through to the live edge without decoding the frames in between. read_frame_()
  // still demuxes each skipped frame, so audio_skip_until_us_ + resync_generation_ keep audio in
  // step (bump the generation once per lag episode so a persistently slow decoder does not re-flush
  // audio every frame). No logging on this priority-10 path (AGENTS.md).
  if (!this->resync_active_) {
    this->resync_active_ = true;
    this->resync_count_++;
    if (this->audio_enabled_) {
      this->resync_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
  }

  uint32_t dropped = 0;
  for (uint32_t w = want_index(); out_index < w; w = want_index()) {
    if (this->audio_enabled_) {
      this->audio_skip_until_us_.store(static_cast<int64_t>(w * this->frame_duration_us_),
                                      std::memory_order_release);
    }
    if (++dropped > RESYNC_MAX_DROP) {
      break;
    }
    payload = this->read_frame_();
    if (payload <= 0) {
      this->resync_frames_dropped_ += dropped;
      return payload;
    }
    out_index = this->video_frame_index_++;
  }
  this->resync_frames_dropped_ += dropped;
  return payload;  // first frame at/after the live edge, already in decode_read_buffer_
}

int SimpleVideoPlayer::read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity) {
  // Read the next VIDEO frame from the file (demuxing + feeding audio inline); runs on the
  // playback task, writing into decode_read_buffer_.
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
  // Synchronous; the esp_err_t return is the completion status. decode target capacity is the fixed
  // frame_bytes_. All per-frame config lives in hw_decode_cfg_, built once.
  uint32_t out_size = 0;
  esp_err_t err = jpeg_decoder_process(this->hw_jpeg_decoder_, &this->hw_decode_cfg_, frame_data,
                                       static_cast<uint32_t>(ALIGN_UP(frame_size, 16)),
                                       reinterpret_cast<uint8_t *>(this->decode_target_()),
                                       static_cast<uint32_t>(this->frame_bytes_), &out_size);
  return err == ESP_OK && out_size > 0;
}

void SimpleVideoPlayer::present_frame_() {
  // Playback task: M2C-invalidate the just-decoded buffer (decoder DMA-wrote PSRAM) and publish its
  // pixels for loop() to hand to LVGL.
  uint16_t *pixels = this->decode_target_();
  esp_cache_msync(pixels, this->frame_bytes_, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  this->ready_pixels_.store(pixels, std::memory_order_release);
  this->frame_ready_.store(true, std::memory_order_release);
}

bool SimpleVideoPlayer::init_decoder_() {
  this->hw_decode_cfg_.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
#if LV_COLOR_16_SWAP
  this->hw_decode_cfg_.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
#else
  this->hw_decode_cfg_.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
#endif
  if (this->hw_jpeg_decoder_ == nullptr) {
    jpeg_decode_engine_cfg_t eng_cfg{};
    eng_cfg.intr_priority = 0;
    eng_cfg.timeout_ms = 200;
    if (jpeg_new_decoder_engine(&eng_cfg, &this->hw_jpeg_decoder_) != ESP_OK) {
      ESP_LOGE(TAG, "Could not create hardware JPEG decoder engine");
      return false;
    }
  }
  return true;
}

bool SimpleVideoPlayer::parse_header_(const uint8_t *buffer, size_t size, uint32_t &width, uint32_t &height) {
  jpeg_decode_picture_info_t header;
  if (jpeg_decoder_get_info(buffer, static_cast<uint32_t>(size), &header) != ESP_OK) {
    return false;
  }
  width = header.width;
  height = header.height;
  return true;
}

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
    if (!this->parse_header_(this->cache_buffer_.get(), static_cast<size_t>(bytes_read), width, height)) {
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

  // Storage-backed file reader (see buffered_file_reader.h): resolves the path against the storage
  // registry, streams via the storage worker, and its read-ahead window is the shared
  // storage::TransferBuffer arena -- it allocates nothing. Kept across play() calls; open() closes
  // any previous stream first.
  if (!this->file_reader_) {
    this->file_reader_ = std::make_unique<BufferedFileReader>();
  }
  // A wait inside the reader returns early once playback_task_stop_ goes true, so an outstanding
  // storage completion cannot block the end of playback.
  this->file_reader_->set_abort_flag(&this->playback_task_stop_);
  if (!this->file_reader_->open(path.c_str())) {
    ESP_LOGE(TAG, "Failed to open file: %s", path.c_str());
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
  // Nothing is allocated here -- decode targets are the canvas buffer (A) and decode_buffer_b_ (B),
  // both fixed-size. This just verifies the actual video fits.
  uint32_t aligned_width = ALIGN_UP(video_width, 16);
  uint32_t aligned_height = ALIGN_UP(video_height, 16);
  uint32_t aligned_max_width = ALIGN_UP(MAX_VIDEO_WIDTH, 16);
  uint32_t aligned_max_height = ALIGN_UP(MAX_VIDEO_HEIGHT, 16);

  ESP_LOGI(TAG, "Verifying buffers for %" PRIu32 "x%" PRIu32 " video (aligned: %" PRIu32 "x%" PRIu32 ")", video_width,
           video_height, aligned_width, aligned_height);

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
  if (this->hw_jpeg_decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->hw_jpeg_decoder_);
    this->hw_jpeg_decoder_ = nullptr;
  }

  // A (canvas_buffer_/canvas_draw_buf_) is LVGL's own -- just drop our references. B is ours.
  this->canvas_draw_buf_ = nullptr;
  this->canvas_buffer_ = nullptr;
  this->canvas_buffer_ready_ = false;
  if (this->decode_buffer_b_ != nullptr) {
    heap_caps_free(this->decode_buffer_b_);
    this->decode_buffer_b_ = nullptr;
  }

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

  // decode_read_buffer_ came from jpeg_alloc_decoder_mem() -- heap_caps_free(), not unique_ptr's
  // delete[].
  if (this->decode_read_buffer_) {
    heap_caps_free(this->decode_read_buffer_.release());
  }
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
  // Exactly LVGL's own buffer size (LV_DRAW_BUF_SIZE: stride-padded, MCU-aligned). Buffer B is
  // allocated to this same size and the decoder is given this as its output capacity, so a decode
  // can never write past either buffer.
  this->frame_bytes_ = draw_buf->data_size;
  this->canvas_buffer_ready_ = true;

  ESP_LOGI(TAG, "Canvas buffer attached (LVGL-owned): %" PRIu32 "x%" PRIu32, width, height);
  return true;
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
  // Publish last_error_ before state_ so any reader that sees ERROR also sees the reason.
  this->last_error_.store(error, std::memory_order_relaxed);
  this->state_.store(PlayerState::ERROR, std::memory_order_release);

  this->on_error_callbacks_.call(static_cast<uint8_t>(error));
}

}  // namespace esphome::simple_video_player
