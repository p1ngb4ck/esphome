#include "simple_video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/storage/storage_worker.h"
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_dma_utils.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_attr.h"

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
}

void SimpleVideoPlayer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Simple Video Player...");
#if defined(USE_AUDIO) && defined(SVP_AUDIO_SAMPLE_RATE)
  this->wanted_audio_rate_.store(AUDIO_SAMPLE_RATE, std::memory_order_release);
  this->wanted_audio_channels_.store(AUDIO_SOURCE_CHANNELS, std::memory_order_release);
#ifdef SVP_AUDIO_ROUTER
  if (this->audio_router_ != nullptr) {
    this->audio_router_->add_on_output_callback([this](size_t) { this->on_audio_output_changed_(); });
    this->on_audio_output_changed_();
  }
#endif
#endif
#ifdef SVP_MEDIA_PLAYER
  this->setup_media_player_();
#endif

  // Every PSRAM buffer of the player comes from here (psram_pool_size); plan_pool_() shares it out.
  this->media_lock_ = xSemaphoreCreateMutex();
  if (this->media_lock_ == nullptr || !this->pool_.init(this->psram_pool_size_)) {
    ESP_LOGE(TAG, "Could not take the %" PRIu32 "-byte PSRAM pool", this->psram_pool_size_);
    this->mark_failed();
    return;
  }

#ifdef SVP_DSI_OUTPUT
  if (this->dsi_ != nullptr && !this->init_dsi_output_()) {
    this->mark_failed();
    return;
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  {
    // The output pair comes from the pool the first time a video plays here.
    if (this->canvas_ == nullptr) {
      ESP_LOGE(TAG, "Canvas not set");
      this->mark_failed();
      return;
    }
  }
#endif

  if (!this->plan_pool_()) {
    this->mark_failed();
    return;
  }

  // Internal RAM, aligned for DMA.
  this->cache_buffer_.reset(
      static_cast<uint8_t *>(heap_caps_aligned_alloc(DMA_ALIGNMENT, CACHE_BUFFER_SIZE, MALLOC_CAP_INTERNAL)));
  if (!this->cache_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate cache buffer (%zu bytes)", CACHE_BUFFER_SIZE);
    this->mark_failed();
    return;
  }

  if (!this->init_decoder_()) {
    ESP_LOGE(TAG, "Failed to initialize JPEG decoder buffers");
    this->mark_failed();
    return;
  }

  // Compressed-frame pre-buffer for the HW decoder: FRAME_SLOT_COUNT buffers cycled between the
  // reader (Core 0) and the decoder (Core 1) through empty_queue_/filled_queue_; their memory comes
  // from the pool with the first session (acquire_session_buffers_()).
  this->empty_queue_ = xQueueCreate(FRAME_SLOT_COUNT, sizeof(FrameSlot *));
  this->filled_queue_ = xQueueCreate(FRAME_SLOT_COUNT, sizeof(FrameSlot *));
  if (this->empty_queue_ == nullptr || this->filled_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create frame-slot queues");
    this->mark_failed();
    return;
  }

#ifdef USE_AUDIO
  // Sound rings + temp buffer: once, at the start of the pool, sized for the most the sound can run
  // ahead (plan_pool_()). init_audio_decoder_() only checks each file against this fixed format.
  if (this->speaker_ != nullptr) {
    this->source_audio_channels_ = AUDIO_SOURCE_CHANNELS;
    this->audio_sample_rate_ = AUDIO_SAMPLE_RATE;
    this->audio_bits_per_sample_ = AUDIO_BITS_PER_SAMPLE;
    this->audio_frame_bytes_ = AUDIO_BYTES_PER_FRAME;

    const size_t input_bytes = audio_input_bytes(this->max_audio_latency_ms_);
    const size_t decoded_bytes = audio_decoded_bytes(this->max_audio_latency_ms_);
    this->audio_input_ring_buffer_ = ring_buffer::RingBuffer::create_in(this->pool_.alloc(input_bytes), input_bytes);
    this->audio_decoded_ring_buffer_ =
        ring_buffer::RingBuffer::create_in(this->pool_.alloc(decoded_bytes), decoded_bytes);
    this->audio_temp_buffer_ = this->pool_.alloc(AUDIO_TEMP_BUFFER_SIZE);
    if (this->audio_input_ring_buffer_ == nullptr || this->audio_decoded_ring_buffer_ == nullptr ||
        this->audio_temp_buffer_ == nullptr) {
      ESP_LOGE(TAG, "No room in the PSRAM pool for the sound buffers");
      this->mark_failed();
      return;
    }

#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC)
    // audio_decoder_ itself: allocated ONCE here too, same as everything else above. One
    // AudioDecoder instance handles whichever supported compressed codec a given file uses -- its
    // start(AudioFileType) selects the per-file codec path (see init_audio_decoder_), so this is
    // constructed whenever at least one COMPRESSED codec is supported. A PCM-only build (no
    // compressed codec listed) never touches it. AudioDecoder's own start() (verified against
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

#ifdef SVP_STREAM
  if (this->stream_port_ != 0 && !this->setup_stream_())
    ESP_LOGE(TAG, "Network stream receiver disabled");
#endif
#ifdef SVP_CHANNEL_LIST
  if (this->channel_widget_ != nullptr && !this->setup_channel_list_())
    ESP_LOGE(TAG, "Channel list disabled");
#endif
#ifdef SVP_CONTROLS
  if (this->ctl_widget_ != nullptr && this->ctl_touchscreen_ != nullptr)
    this->ctl_touchscreen_->register_listener(&this->ctl_listener_);
#endif
#ifdef SVP_PICTURES
  this->pictures_setup_();
#endif

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
  ESP_LOGCONFIG(TAG, "  PSRAM pool: %u bytes (PSRAM left: %u bytes, largest block %u)",
                static_cast<unsigned>(this->pool_.size()),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  ESP_LOGCONFIG(TAG, "  Read-ahead ring: %u bytes (audio latency up to %" PRIu32 " ms), frame slot %u bytes",
                static_cast<unsigned>(this->ring_size_), this->max_audio_latency_ms_,
                static_cast<unsigned>(this->frame_bytes_));
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Audio: Enabled");
  }
#endif
}

void SimpleVideoPlayer::loop() {
  this->handle_output_release_();
#ifdef SVP_CHANNEL_LIST
  this->channel_loop_();
#endif
#ifdef SVP_PICTURES
  this->pictures_loop_();
#endif
  // A file played to its end: the next of the playlist, before LVGL would get the screen back.
  if (this->advance_req_.load(std::memory_order_acquire) &&
      this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED) {
    this->advance_req_.store(false, std::memory_order_relaxed);
    if (this->playlist_index_ + 1 < this->playlist_.size()) {
      this->play_index_(this->playlist_index_ + 1);
    } else if (this->repeat_ == RepeatMode::PLAYLIST && !this->playlist_.empty()) {
      this->play_index_(0);
    }
  }
#ifdef SVP_CONTROLS
  this->controls_loop_();
#endif
  if (this->start_req_.load(std::memory_order_acquire) == 1) {
#if defined(SVP_DSI_OUTPUT) && defined(SVP_USE_LVGL)
    if (this->direct_session_() && this->lvgl_component_ != nullptr && !this->dsi_lvgl_paused_) {
      this->lvgl_component_->set_paused(true, false);
      this->dsi_lvgl_paused_ = true;
    }
#endif
    this->on_started_callbacks_.call();
    this->start_req_.store(2, std::memory_order_release);
  }
#ifdef SVP_DSI_OUTPUT
  if (this->dsi_ != nullptr) {
    // Direct playback owns the panel; the video task draws each frame itself. Nothing to
    // invalidate on the LVGL thread. Hand the panel back to LVGL once playback has fully stopped.
    // A udisp sender draws onto the panel only when its picture is made for it.
#ifdef SVP_STREAM
    const bool streaming = this->stream_connected_.load(std::memory_order_acquire) && this->stream_direct_();
#else
    const bool streaming = false;
#endif
#ifdef SVP_USE_LVGL
    if (streaming && this->lvgl_component_ != nullptr && !this->dsi_lvgl_paused_) {
      this->lvgl_component_->set_paused(true, false);
      this->dsi_lvgl_paused_ = true;
    }
    const PlayerState state = this->state_.load(std::memory_order_acquire);
    if (this->dsi_lvgl_paused_ && !streaming && state == PlayerState::STOPPED &&
        !this->advance_req_.load(std::memory_order_acquire)) {
      this->lvgl_component_->set_paused(false, false);
      this->dsi_lvgl_paused_ = false;
    }
    // A paused file: LVGL gets the panel once the playback task is parked (on-device controls),
    // and gives it back here, before the task is let go again, when resume() asks for it.
    if (this->paused_ui_ && state != PlayerState::PAUSED) {
      this->paused_ui_ = false;  // stopped or replaced meanwhile
      this->resume_req_.store(false, std::memory_order_relaxed);
    }
    bool paused_ui = this->dsi_lvgl_paused_ && !streaming && state == PlayerState::PAUSED &&
                     !this->is_network_session() && this->playback_parked_.load(std::memory_order_acquire);
#ifdef SVP_CONTROLS
    paused_ui = paused_ui && (this->ctl_widget_ == nullptr || this->ctl_failed_);  // else the controls draw over it
#endif
    if (paused_ui) {
      this->lvgl_component_->set_paused(false, false);
      this->dsi_lvgl_paused_ = false;
      this->paused_ui_ = true;
    }
    if (this->paused_ui_ && this->resume_req_.exchange(false, std::memory_order_acq_rel)) {
      this->lvgl_component_->set_paused(true, false);
      this->dsi_lvgl_paused_ = true;
      this->paused_ui_ = false;
      PlayerState expected = PlayerState::PAUSED;
      if (this->state_.compare_exchange_strong(expected, PlayerState::PLAYING, std::memory_order_acq_rel))
        ESP_LOGI(TAG, "Resuming playback");
    }
    const bool lvgl_off = this->lvgl_component_ == nullptr || this->dsi_lvgl_paused_;
#else
    const bool lvgl_off = true;
#endif
#if defined(SVP_STREAM) && defined(USE_AUDIO)
    // Stream sound stopped arriving: let the speaker go (a playing file manages it itself).
    if (this->stream_audio_on_.load(std::memory_order_acquire) &&
        millis() - this->stream_last_audio_ms_.load(std::memory_order_acquire) > 500) {
      this->stream_audio_on_.store(false, std::memory_order_release);
      if (this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED)
        this->speaker_->stop();
    }
#endif
#ifdef SVP_STREAM
    const bool output_ok = streaming && lvgl_off;
    if (output_ok != this->stream_output_ok_.load(std::memory_order_acquire)) {
      this->stream_output_ok_.store(output_ok, std::memory_order_release);
      if (this->stream_draw_task_ != nullptr)
        xTaskNotifyGive(this->stream_draw_task_);
    }
#else
    (void) streaming;
    (void) lvgl_off;
#endif
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  // Runs on the LVGL thread. decode_frame_() (video task) finished a frame in the back buffer and
  // published it via display_buffer_ + frame_ready_. Point the canvas at that completed buffer and
  // invalidate -- all lv_canvas_* happens HERE, never on the video task. Cheap load first so the
  // common no-frame path is a plain relaxed read, not an atomic RMW.
#ifdef SVP_STREAM
  // udisp rectangles copied into the page (stream_copy_to_canvas_): show the page if a file showed
  // something else, and tell LVGL which part changed.
  {
    bool dirty;
    lv_area_t area;
    portENTER_CRITICAL(&this->stream_dirty_lock_);
    dirty = this->stream_dirty_;
    area = {this->stream_dirty_x1_, this->stream_dirty_y1_, this->stream_dirty_x2_, this->stream_dirty_y2_};
    this->stream_dirty_ = false;
    portEXIT_CRITICAL(&this->stream_dirty_lock_);
    if (dirty && this->canvas_ != nullptr && this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED) {
      if (this->display_buffer_.load(std::memory_order_acquire) != this->output_buffers_[0]) {
        this->canvas_w_ = this->canvas_size_w_;
        this->canvas_h_ = this->canvas_size_h_;
        this->canvas_stride_.store(((this->canvas_size_w_ + 15) & ~15) * 3, std::memory_order_relaxed);
        this->display_buffer_.store(this->output_buffers_[0], std::memory_order_release);
        this->frame_ready_.store(true, std::memory_order_release);
      } else {
        lv_area_t at;
        lv_obj_get_coords(this->canvas_, &at);
        area.x1 += at.x1;
        area.x2 += at.x1;
        area.y1 += at.y1;
        area.y2 += at.y1;
        lv_obj_invalidate_area(this->canvas_, &area);
      }
    }
  }
#endif
  if (this->frame_ready_.load(std::memory_order_acquire) &&
      this->frame_ready_.exchange(false, std::memory_order_acq_rel)) {
    uint8_t *buf = this->display_buffer_.load(std::memory_order_acquire);
    if (buf != nullptr && this->canvas_ != nullptr) {
      // Only the picture's own pixels are shown: the rows keep the decoder's padding (stride), and
      // the canvas widget keeps its YAML size, centring a smaller picture (lv_image's inner align).
      const uint32_t stride = this->canvas_stride_.load(std::memory_order_relaxed);
      lv_draw_buf_t *shown = lv_canvas_get_draw_buf(this->canvas_);
      if (shown == &this->canvas_draw_buf_ && shown->header.w == this->canvas_w_ &&
          shown->header.h == this->canvas_h_ && shown->header.stride == stride) {
        // Same buffer object, same layout: only the pixels move. LVGL reads an uncompressed
        // variable image straight from draw_buf->data at every draw and never caches it
        // (lv_bin_decoder.c "Do not put image to cache if it can be used directly"), and
        // lv_draw_buf_init() sets data and unaligned_data alike (lv_draw_buf.c).
        shown->data = buf;
        shown->unaligned_data = buf;
      } else if (lv_draw_buf_init(&this->canvas_draw_buf_, this->canvas_w_, this->canvas_h_, LV_COLOR_FORMAT_RGB888,
                                  stride, buf, static_cast<uint32_t>(this->output_buffer_size_)) == LV_RESULT_OK) {
        lv_canvas_set_draw_buf(this->canvas_, &this->canvas_draw_buf_);
      }
      lv_obj_invalidate(this->canvas_);
    }
  }
#endif
}

void SimpleVideoPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Simple Video Player:");
  ESP_LOGCONFIG(TAG, "  PSRAM pool: %u bytes, %u in use", static_cast<unsigned>(this->pool_.size()),
                static_cast<unsigned>(this->pool_.used()));
  ESP_LOGCONFIG(TAG, "  Read-ahead ring: %u bytes, frame slot %u bytes", static_cast<unsigned>(this->ring_size_),
                static_cast<unsigned>(this->frame_bytes_));
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);

  if (this->state_ != PlayerState::STOPPED) {
    ESP_LOGCONFIG(TAG, "  Current file: %s", this->video_path_.c_str());
    ESP_LOGCONFIG(TAG, "  Video size: %" PRIu32 "x%" PRIu32, this->video_width_, this->video_height_);
  }
}

//========================================================================
// Playback Control API
//========================================================================

void SimpleVideoPlayer::play(const std::string &video_path, bool folder_playlist) {
  this->playlist_.clear();
  this->playlist_index_ = 0;
  if (folder_playlist)
    this->load_folder_playlist_(video_path);
  if (this->playlist_.empty())
    this->playlist_.push_back(video_path);
  this->play_index_(this->playlist_index_);
}

void SimpleVideoPlayer::play_list(std::vector<std::string> paths, size_t start) {
  if (paths.empty())
    return;
  if (paths.size() > MAX_PLAYLIST)
    paths.resize(MAX_PLAYLIST);
  this->playlist_ = std::move(paths);
  this->play_index_(std::min(start, this->playlist_.size() - 1));
}

void SimpleVideoPlayer::play_index_(size_t index) {
  this->playlist_index_ = index;
  this->advance_req_.store(false, std::memory_order_relaxed);
#ifdef SVP_STREAM
  this->net_session_ = false;
#endif
  this->start_playback_(this->playlist_[index]);
}

void SimpleVideoPlayer::next() {
  if (this->playlist_.empty())
    return;
  if (this->playlist_index_ + 1 < this->playlist_.size()) {
    this->play_index_(this->playlist_index_ + 1);
  } else if (this->repeat_ == RepeatMode::PLAYLIST) {
    this->play_index_(0);
  } else {
    ESP_LOGI(TAG, "End of the playlist");
  }
}

void SimpleVideoPlayer::previous() {
  if (this->playlist_.empty())
    return;
  static constexpr float RESTART_AFTER_S = 3.0f;
  const bool playing = !this->is_stopped();
  if (playing && this->get_position() > RESTART_AFTER_S) {
    if (this->can_seek()) {
      this->seek(0.0f);
    } else {
      this->play_index_(this->playlist_index_);
    }
  } else if (this->playlist_index_ > 0) {
    this->play_index_(this->playlist_index_ - 1);
  } else if (this->repeat_ == RepeatMode::PLAYLIST) {
    this->play_index_(this->playlist_.size() - 1);
  } else {
    this->play_index_(0);
  }
}

void SimpleVideoPlayer::load_folder_playlist_(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0 || storage::global_storage_registry == nullptr)
    return;
  if (this->state_.load(std::memory_order_acquire) != PlayerState::STOPPED) {
    this->stop();
    this->wait_for_task_stop_(this->task_handle_, 5000);
  }
  const std::string dir = path.substr(0, slash);
  const char *rel = nullptr;
  storage::PathStorage *storage = storage::global_storage_registry->resolve_path(dir.c_str(), &rel);
  if (storage == nullptr || (storage::global_storage_worker != nullptr &&
                             storage::global_storage_worker->has_active_task_io(storage))) {
    ESP_LOGW(TAG, "Cannot list %s for the playlist", dir.c_str());
    return;
  }
  std::vector<std::string> names;
  const storage::StorageError err = storage->list_dir(
      (rel == nullptr || rel[0] == '\0') ? "/" : rel,
      [](const storage::FileStat *entry, void *context) -> bool {
        auto *names = static_cast<std::vector<std::string> *>(context);
        if (entry->is_dir || entry->name[0] == '.')
          return true;
        const char *dot = strrchr(entry->name, '.');
        if (dot == nullptr)
          return true;
        std::string extension(dot);
        for (char &c : extension)
          c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (extension == ".avi" || extension == ".mjpeg" || extension == ".mjpg")
          names->emplace_back(entry->name);
        return names->size() < MAX_PLAYLIST;
      },
      &names);
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGW(TAG, "Listing %s failed: %s", dir.c_str(), storage::error_to_string(err));
    return;
  }
  auto lower = [](std::string text) {
    for (char &c : text)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
  };
  std::sort(names.begin(), names.end(),
            [&lower](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
  const std::string file = path.substr(slash + 1);
  for (size_t i = 0; i < names.size(); i++) {
    if (names[i] == file)
      this->playlist_index_ = i;
    this->playlist_.push_back(dir + "/" + names[i]);
  }
  if (this->playlist_index_ >= this->playlist_.size() || this->playlist_[this->playlist_index_] != path) {
    // The file itself is not among them (another extension, hidden): play it first, then the folder.
    this->playlist_.insert(this->playlist_.begin(), path);
    this->playlist_index_ = 0;
  }
  ESP_LOGI(TAG, "Playlist: %u videos in %s", static_cast<unsigned>(this->playlist_.size()), dir.c_str());
}

void SimpleVideoPlayer::start_playback_(const std::string &video_path) {
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Cannot play: component failed to initialize (see setup logs)");
    return;
  }
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
  this->start_req_.store(0, std::memory_order_release);
  this->av_started_.store(false, std::memory_order_release);
  this->seek_pending_.store(false, std::memory_order_relaxed);
  this->reader_seek_req_.store(false, std::memory_order_relaxed);
  this->audio_hold_.store(false, std::memory_order_relaxed);
  this->duration_frames_.store(0, std::memory_order_relaxed);
  this->position_frame_.store(0, std::memory_order_relaxed);
#ifdef USE_AUDIO
  this->audio_lead_us_ = std::min<uint32_t>(this->audio_latency_.value(), this->max_audio_latency_ms_) * 1000;
#endif
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
  // Core 1, priority 1 -- the SAME priority as ESPHome's loopTask (esp32/core.cpp creates it at
  // prio 1, pinned to Core 1). Equal priority means the FreeRTOS tick round-robins the two every
  // tick, so LVGL and the rest of App.loop() keep running WITHOUT this task ever calling a
  // blocking yield -- that is what replaces the old per-frame vTaskDelay. It must stay on Core 1
  // (not 0): the HW JPEG decoder and LVGL's PPA rotate both drive DMA2D, and single-core
  // execution is what keeps them from touching it concurrently (espressif/esp-idf#18999).
  BaseType_t result = xTaskCreatePinnedToCore(playback_task_entry_, "video_player",
                                              8192,  // Stack size
                                              this,
                                              1,  // Priority == loopTask (round-robin, no yield needed)
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
#if defined(SVP_DSI_OUTPUT) && defined(SVP_USE_LVGL)
  if (this->paused_ui_) {
    // LVGL draws the paused UI onto the panel. resume() may be running inside an LVGL event (a
    // button), so LVGL is paused from loop(), outside lv_timer_handler(), and only then does the
    // playback task get to draw again.
    if (this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED)
      this->resume_req_.store(true, std::memory_order_release);
    return;
  }
#endif
  PlayerState expected = PlayerState::PAUSED;
  if (this->state_.compare_exchange_strong(expected, PlayerState::PLAYING, std::memory_order_acq_rel)) {
    ESP_LOGI(TAG, "Resuming playback");
  }
}

void SimpleVideoPlayer::stop() {
  this->advance_req_.store(false, std::memory_order_relaxed);
  PlayerState prev = this->state_.exchange(PlayerState::STOPPED, std::memory_order_acq_rel);
  if (prev != PlayerState::STOPPED) {
    ESP_LOGI(TAG, "Stopping playback");
  }
}

bool SimpleVideoPlayer::can_seek() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  return (state == PlayerState::PLAYING || state == PlayerState::PAUSED) &&
         this->duration_frames_.load(std::memory_order_acquire) != 0 && this->frame_duration_us_ > 0;
}

float SimpleVideoPlayer::get_position() const {
  if (this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED)
    return 0.0f;
  return this->position_frame_.load(std::memory_order_relaxed) * this->frame_duration_us_ / 1e6f;
}

float SimpleVideoPlayer::get_duration() const {
  return this->duration_frames_.load(std::memory_order_acquire) * this->frame_duration_us_ / 1e6f;
}

void SimpleVideoPlayer::seek(float seconds) {
  if (!this->can_seek()) {
    ESP_LOGW(TAG, "Seeking needs a local AVI file playing or paused");
    return;
  }
  const uint32_t frames = this->duration_frames_.load(std::memory_order_acquire);
  const float frame = std::max(seconds, 0.0f) * 1e6f / this->frame_duration_us_;
  const uint32_t target = frame >= static_cast<float>(frames - 1) ? frames - 1 : static_cast<uint32_t>(frame);
  ESP_LOGI(TAG, "Seeking to %.1f s", target * this->frame_duration_us_ / 1e6f);
  this->seek_target_.store(target, std::memory_order_relaxed);
  this->seek_pending_.store(true, std::memory_order_release);
}

//========================================================================
// Playback Task
//========================================================================

void SimpleVideoPlayer::playback_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  player->playback_loop_();
  player->task_handle_ = nullptr;  // also after an early return
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::reader_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  player->reader_loop_();
  player->reader_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::reader_loop_() {
  // Core 0 producer: demux ahead into the compressed-frame slots. Only storage reads + memcpy --
  // no JPEG decode, no PPA -- so it never drives DMA2D and cannot race the Core 1 decode/PPA
  // (esp-idf#18999). Backpressure is empty_queue_: it stalls here once FRAME_SLOT_COUNT frames are
  // read ahead and resumes as the decoder returns slots, so the storage ring is drained steadily.
  bool at_end = false;
  uint32_t raw_frame = 0;
  while (!this->reader_task_stop_) {
    const PlayerState st = this->state_.load(std::memory_order_acquire);
    if (st != PlayerState::PLAYING && st != PlayerState::PAUSED)
      break;

    if (this->reader_seek_req_.load(std::memory_order_acquire)) {
      const bool ok = this->reader_seek_(this->reader_seek_frame_.load(std::memory_order_relaxed));
      if (ok) {
        this->frame_gen_.fetch_add(1, std::memory_order_acq_rel);
        at_end = false;
      }
      this->reader_seek_ok_.store(ok, std::memory_order_relaxed);
      this->reader_seek_req_.store(false, std::memory_order_release);
    }
    if (at_end) {
      // EOF/error sentinel delivered: nothing left to produce, unless a seek comes before the end.
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

#ifdef USE_AUDIO
    this->advance_audio_cursor_();
#endif
    FrameSlot *slot = nullptr;
    // Short timeout so a stop/stall is noticed promptly without a busy spin.
    if (xQueueReceive(this->empty_queue_, &slot, pdMS_TO_TICKS(20)) != pdTRUE)
      continue;

    const int n = this->read_frame_(slot->data, slot->capacity);
    if (n == -2) {
      // Stopped/aborted mid-read: return the slot unused, deliver no sentinel.
      xQueueSend(this->empty_queue_, &slot, 0);
      break;
    }
    slot->size = n;  // > 0 payload, REPEAT_FRAME, 0 EOF sentinel, -1 read-error sentinel
    slot->gen = this->frame_gen_.load(std::memory_order_relaxed);
    slot->file_frame =
        this->video_format_ == VideoFormat::AVI_MJPEG ? this->avi_parser_->current_frame() - 1 : raw_frame++;
    xQueueSend(this->filled_queue_, &slot, portMAX_DELAY);
    if (n == 0 || n == -1)
      at_end = true;
  }
}

void SimpleVideoPlayer::playback_loop_() {
  ESP_LOGI(TAG, "Playback task started (Core 1)");

  // The session's buffers from the pool: taken once, kept for the next video.
  if (!this->acquire_session_buffers_()) {
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    return;
  }

  // Open file
  if (!this->open_file_(this->video_path_)) {
    ESP_LOGE(TAG, "Failed to open video file: %s", this->video_path_.c_str());
    this->set_error_(PlaybackError::FILE_NOT_FOUND);
    return;
  }

  this->avi_fps_num_ = 0;
  this->avi_fps_den_ = 0;
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

  // Nothing is scaled: the display takes a video of exactly its native picture (transposed at
  // transcode to the panel's orientation), the canvas one of at most its size, centred. A stream
  // was made for the output it asked for.
  uint32_t panel_w = 0, panel_h = 0, canvas_w = 0, canvas_h = 0;
#ifdef SVP_DSI_OUTPUT
  if (this->dsi_ != nullptr) {
    panel_w = this->dsi_out_w_;
    panel_h = this->dsi_out_h_;
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  canvas_w = this->canvas_size_w_;
  canvas_h = this->canvas_size_h_;
#endif
  const bool fits_canvas = width <= canvas_w && height <= canvas_h;
  bool direct = width == panel_w && height == panel_h;
#ifdef SVP_STREAM
  if (direct && fits_canvas && this->net_session_ && !this->stream_direct_())
    direct = false;
#endif
  if (!direct && !fits_canvas) {
    ESP_LOGE(TAG,
             "Video %" PRIu32 "x%" PRIu32 " fits no output (display: exactly %" PRIu32 "x%" PRIu32
             ", canvas: at most %" PRIu32 "x%" PRIu32 ") -- transcode it. Aborting playback.",
             width, height, panel_w, panel_h, canvas_w, canvas_h);
    this->set_error_(PlaybackError::INVALID_VIDEO_FORMAT);
    this->close_file_();
    return;
  }
#ifdef SVP_DSI_OUTPUT
  this->direct_.store(direct, std::memory_order_release);
#endif
  bool output_ready = false;
#ifdef SVP_DSI_OUTPUT
  if (direct)
    output_ready = this->ensure_direct_target_();
#endif
#ifdef SVP_CANVAS_OUTPUT
  if (!direct)
    output_ready = this->ensure_canvas_outputs_();
#endif
  if (!output_ready) {
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
    return;
  }
  ESP_LOGI(TAG, "Output: %s", direct ? "display" : "canvas");
  if (!direct) {
    // Record the canvas dimensions for loop() to pass to lv_canvas_set_buffer. The canvas is pointed
    // at the freshly decoded buffer from loop() (the LVGL thread) -- NEVER from this video task, since
    // LVGL is not thread-safe and loopTask runs the render/rotate/flush pipeline on the same core.
    // RGB888: the decoder always outputs RGB888 (RGB565 is buggy on some P4 revs); LVGL converts
    // RGB888 -> the panel's format on blit (PPA/SW).
    this->canvas_w_ = width;
    this->canvas_h_ = height;
    this->canvas_stride_.store(ALIGN_UP(width, 16) * 3, std::memory_order_relaxed);
  }

  // Fresh pacing state for this session.
  this->paused_accum_us_ = 0;
  this->decode_fail_count_ = 0;
  this->late_frame_count_ = 0;
  this->last_present_us_ = 0;
  this->decode_us_sum_ = 0;
  this->decode_us_max_ = 0;
  this->present_us_sum_ = 0;
  this->present_us_max_ = 0;
  this->bad_payload_count_ = 0;
  this->solid_frame_count_ = 0;
  this->video_frame_index_ = 0;

  // No canvas widget resize/reposition here: this is a single, fixed-resolution panel, and the
  // canvas is already the correct size and position from YAML -- there is no placeholder-then-
  // grow case to support, so touching lv_obj_set_size()/lv_obj_set_pos() here was pure
  // unnecessary risk for a no-op in the common case.
  // Visibility (hidden flag, foreground order, which page/screen is active) is the caller's job,
  // not this component's -- expected usage is a dedicated page holding just the video canvas,
  // switched to by the caller's own action before play() and away from after stop().

  // Audio/speaker init -- only when a speaker is configured AND this AVI actually has a matching
  // audio track. init_audio_decoder_() returns false (quietly, video-only) for no-audio files.
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr && this->video_format_ == VideoFormat::AVI_MJPEG) {
    if (this->init_audio_decoder_()) {
      ESP_LOGI(TAG, "Audio system ready");
    }
  }
#endif

  // Allocate output (decoded RGB888) buffers based on video size
  if (!this->allocate_buffers_(width, height)) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
    return;
  }

  if (!direct) {
    // Clear the region the decode won't cover (padding / rounding tail, or a previous larger video's
    // leftover pixels around a smaller new one) in BOTH buffers; the decode overwrites the
    // ALIGN_UP(w,16) x ALIGN_UP(h,16) x 3 region. For a max-resolution video this is a no-op.
    const size_t covered = static_cast<size_t>(ALIGN_UP(width, 16)) * ALIGN_UP(height, 16) * 3;
    if (covered < this->output_buffer_size_) {
      for (auto *buf : this->output_buffers_)
        std::memset(buf + covered, 0, this->output_buffer_size_ - covered);
    }
    // Start decoding into buffer 0; show buffer 1 (black) until frame 0 is published. loop() will
    // repoint the canvas when it sees frame_ready_.
    this->decode_buf_idx_ = 0;
    this->display_buffer_.store(this->output_buffers_[1], std::memory_order_release);
    this->frame_ready_.store(true, std::memory_order_release);
  }

  // Reset file position to start (not needed for AVI - parser is already positioned at movi data)
  if (this->video_format_ != VideoFormat::AVI_MJPEG) {
    this->seek_to_(0);
  }
  this->cache_buffer_valid_ = 0;
  this->cache_buffer_offset_ = 0;

  // Frame rate from the AVI header (dwRate/dwScale); target_fps is only the fallback for raw MJPEG
  // or a header without a usable rate.
  if (this->video_format_ == VideoFormat::AVI_MJPEG && this->avi_fps_num_ != 0 && this->avi_fps_den_ != 0) {
    this->frame_duration_us_ = 1000000.0f * this->avi_fps_den_ / this->avi_fps_num_;
  } else {
    this->frame_duration_us_ = 1000000.0f / this->target_fps_;
  }
  ESP_LOGI(TAG, "Frame interval: %.0f us (%.2f fps)", this->frame_duration_us_, 1000000.0f / this->frame_duration_us_);
  // Fixed for the whole session -- computed once here, not cast from the float every frame.
  const int64_t frame_dur = static_cast<int64_t>(this->frame_duration_us_);
  {
    bool seekable = this->video_format_ == VideoFormat::AVI_MJPEG;
#ifdef SVP_STREAM
    seekable = seekable && !this->net_session_;
#endif
    const AVIStreamInfo *video_info = seekable ? this->avi_parser_->get_video_info() : nullptr;
    this->duration_frames_.store(video_info != nullptr ? video_info->total_frames : 0, std::memory_order_release);
  }
#ifdef USE_AUDIO
  this->audio_lead_frames_ =
      this->video_format_ == VideoFormat::AVI_MJPEG && this->audio_enabled_ && frame_dur > 0
          ? static_cast<uint32_t>((this->audio_lead_us_ + frame_dur / 2) / frame_dur)
          : 0;
  this->reader_frames_.store(0, std::memory_order_relaxed);
  this->reset_audio_cursor_();
  if (this->audio_lead_frames_ > 0)
    ESP_LOGI(TAG, "Audio leads the picture by %" PRIu32 " ms (%" PRIu32 " frames)", this->audio_lead_us_ / 1000,
             this->audio_lead_frames_);
#else
  this->audio_lead_frames_ = 0;
#endif
  // Anchored on the first paced frame in the loop (see there), not here -- so cold-start read
  // latency is not counted as the stream already running late.
  this->playback_start_time_us_ = 0;

  // Core 1, prio 1 (== loopTask). From here the task never sleeps -- the pacing gate is a
  // wall-clock comparison spin, not a wait -- so it must feed the task WDT itself. loopTask is
  // round-robined in by the FreeRTOS tick regardless, keeping LVGL and the rest of ESPHome alive.
  // Subscribed here (not at task entry) so the early-return error paths above never leave a
  // subscription dangling past vTaskDelete().
  esp_task_wdt_add(nullptr);

  // Load is done (headers, dimensions, audio init all read their bytes with the blocking reader).
  // Precache: block until the read-ahead ring is full so the first frame reads already have their
  // bytes. Then switch the reader to non-blocking single-drain -- every read from here is a
  // hot-path frame read.
  if (this->file_reader_) {
    this->file_reader_->prefill_cache();
    this->file_reader_->set_streaming(true);
  }

  // Reset the slot queues to a clean start: every slot free in empty_queue_, filled_queue_ empty,
  // regardless of how a prior session ended.
  {
    FrameSlot *s = nullptr;
    while (xQueueReceive(this->empty_queue_, &s, 0) == pdTRUE) {
    }
    while (xQueueReceive(this->filled_queue_, &s, 0) == pdTRUE) {
    }
    for (auto &slot : this->frame_slots_) {
      slot.size = 0;
      FrameSlot *p = &slot;
      xQueueSend(this->empty_queue_, &p, 0);
    }
  }

  // Start the reader/producer on Core 0 (decode stays on Core 1, see play()'s esp-idf#18999 note).
  this->reader_task_stop_ = false;
  this->expected_gen_ = this->frame_gen_.load(std::memory_order_acquire);
  xTaskCreatePinnedToCore(reader_task_entry_, "svp_reader", 4096, this, 2, &this->reader_task_handle_, 0);

  // Deep pre-buffer: wait (bounded) until the reader has filled the slot queue before presenting
  // the first frame, so decode starts from a full compressed buffer and the storage ring stays
  // primed throughout -- constant pre-buffering, never a mid-frame empty ring. Pumping the worker
  // here keeps the fill chain advancing while we wait. Not the hot path (nothing presented yet).
  {
    uint32_t waited_ms = 0;
    while (uxQueueMessagesWaiting(this->filled_queue_) < FRAME_SLOT_COUNT) {
      if (this->state_.load(std::memory_order_acquire) != PlayerState::PLAYING)
        break;
      if (storage::global_storage_worker != nullptr)
        storage::global_storage_worker->update();
      vTaskDelay(pdMS_TO_TICKS(5));
      if ((waited_ms += 5) >= 2000)
        break;
    }
  }

  // Preloaded: ring full and the first compressed frame queued. Only now ask the main thread to
  // start (pause LVGL for DSI output, fire on_playback_started) and wait for it -- still before the
  // first frame, so nothing is rendered, presented or played earlier. Audio drains from here on.
#ifdef SVP_STREAM
  // File playback has priority over the network stream: hold the output for the whole session and
  // tell a connected sender to pause until it ends.
  if (this->output_lock_ != nullptr) {
    xSemaphoreTake(this->output_lock_, portMAX_DELAY);
    this->stream_set_awake_(false);
  }
#endif
  this->start_req_.store(1, std::memory_order_release);
  {
    uint32_t waited_ms = 0;
    while (this->start_req_.load(std::memory_order_acquire) != 2 &&
           this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING && waited_ms < 2000) {
      if (storage::global_storage_worker != nullptr)
        storage::global_storage_worker->update();
      vTaskDelay(pdMS_TO_TICKS(1));
      waited_ms++;
    }
  }
#ifdef USE_AUDIO
  if (this->audio_enabled_)
    this->wait_speaker_running_();
  // The audio cursor reads ahead while the reader waits for the slot; the sound is released once it
  // is audio_lead_frames_ ahead (bounded: a short file or a slow stream starts with what there is).
  {
    uint32_t waited_ms = 0;
    while (this->audio_cursor_frames_.load(std::memory_order_acquire) <
               this->reader_frames_.load(std::memory_order_acquire) + this->audio_lead_frames_ &&
           this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING && waited_ms < 1000) {
      if (storage::global_storage_worker != nullptr)
        storage::global_storage_worker->update();
      vTaskDelay(pdMS_TO_TICKS(1));
      waited_ms++;
    }
  }
#endif
  this->av_started_.store(true, std::memory_order_release);
#ifdef USE_AUDIO
  if (this->audio_task_handle_ != nullptr)
    xTaskNotifyGive(this->audio_task_handle_);
#endif

  // One state load per iteration. Anything but PLAYING/PAUSED (STOPPED, ERROR) ends the loop.
  while (true) {
    // Pump the storage worker's completion delivery ourselves. read_chunk() completions
    // (on_fill_done_ -> arena copy into the ring -> next kick_fill_) only ever fire from
    // StorageWorker::update() on the main loop; this task never yields Core 1, so without this
    // the ring would never refill after the precache drains. (update() also runs from loopTask's
    // scheduler -- concurrent calls are possible; the completion sweep is short and both are on
    // Core 1.)
    if (storage::global_storage_worker != nullptr) {
      storage::global_storage_worker->update();
    }

    const PlayerState st = this->state_.load(std::memory_order_acquire);
    if (st != PlayerState::PLAYING && st != PlayerState::PAUSED) {
      break;
    }
    if (this->seek_pending_.load(std::memory_order_acquire)) {
      this->handle_seek_();
      continue;
    }
    // Charge parked wall time to paused_accum_us_ so a pause is not seen as the stream falling
    // behind (which would trigger a re-sync on resume).
    if (st == PlayerState::PAUSED) {
      const int64_t pause_started_us = esp_timer_get_time();
      // Parked: nothing of this task touches the panel until the state changes, so loop() may
      // hand it to LVGL meanwhile.
      this->playback_parked_.store(true, std::memory_order_release);
      // Paused is not playback -- a coarse sleep here is fine (and correct: it lets the rest of
      // the system run at full speed). The zero-wait rule is about the active decode/pace path.
      while (this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
        if (this->seek_pending_.load(std::memory_order_acquire)) {
          this->handle_seek_();
#ifdef SVP_CONTROLS
          this->ctl_step_.store(direct && this->ctl_widget_ != nullptr, std::memory_order_relaxed);
#endif
        }
#ifdef SVP_CONTROLS
        if (direct && this->ctl_widget_ != nullptr) {
          // After a seek the new picture shows, with the controls over it.
          FrameSlot *slot = nullptr;
          if (this->ctl_step_.load(std::memory_order_relaxed) &&
              xQueueReceive(this->filled_queue_, &slot, 0) == pdTRUE) {
            if (slot->gen == this->expected_gen_ && slot->size > 0) {
              this->ctl_step_.store(false, std::memory_order_relaxed);
              this->video_frame_index_++;
              this->position_frame_.store(slot->file_frame, std::memory_order_relaxed);
              const bool decoded = this->decode_frame_(slot->data, static_cast<size_t>(slot->size));
              xQueueSend(this->empty_queue_, &slot, 0);
              if (decoded)
                this->ctl_repaint_.store(true, std::memory_order_release);
            } else if (slot->gen != this->expected_gen_) {
              xQueueSend(this->empty_queue_, &slot, 0);
            } else {
              this->ctl_step_.store(false, std::memory_order_relaxed);
              xQueueSendToFront(this->filled_queue_, &slot, 0);  // end of file: the loop takes it
            }
          }
          if (this->ctl_repaint_.exchange(false, std::memory_order_acq_rel))
            this->repaint_paused_();
        }
#endif
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
      }
      this->playback_parked_.store(false, std::memory_order_release);
      this->paused_accum_us_ += esp_timer_get_time() - pause_started_us;
      continue;
    }

    // Pull the next pre-buffered compressed frame from the reader (Core 0). Short timeout so the
    // loop keeps pumping the storage worker between frames; the reader stays ahead, so in steady
    // state this returns immediately.
    FrameSlot *slot = nullptr;
    if (xQueueReceive(this->filled_queue_, &slot, pdMS_TO_TICKS(20)) != pdTRUE) {
      continue;
    }
    if (slot->gen != this->expected_gen_) {  // read before a seek
      xQueueSend(this->empty_queue_, &slot, 0);
      continue;
    }
    const int payload = slot->size;
    if (payload == 0) {  // EOF sentinel
      xQueueSend(this->empty_queue_, &slot, 0);
#ifdef SVP_STREAM
      if (this->net_session_)
        this->stream_played_to_end_.store(true, std::memory_order_release);
#endif
      ESP_LOGI(TAG, "Playback finished");
#ifdef SVP_STREAM
      if (!this->net_session_)
#endif
        this->advance_req_.store(true, std::memory_order_release);
      this->on_finished_callbacks_.call();
      break;
    }
    const bool repeat = payload == AVIParser::REPEAT_FRAME;  // the previous picture stays one more frame
    if (payload < 0 && !repeat) {  // read-error sentinel
      xQueueSend(this->empty_queue_, &slot, 0);
      ESP_LOGE(TAG, "Failed to read frame");
      this->set_error_(PlaybackError::FILE_READ_ERROR);
      break;
    }
    const uint32_t frame_index = this->video_frame_index_++;
    this->position_frame_.store(slot->file_frame, std::memory_order_relaxed);

    // Anchor the wall clock on the first paced frame -- once its payload is in hand, so reader
    // cold-start latency isn't counted as the stream already running late.
    // The picture starts audio_lead_us_ after the sound, which was released at av_started_.
    if (this->playback_start_time_us_ == 0) {
      this->paused_accum_us_ = 0;
      this->playback_start_time_us_ =
          esp_timer_get_time() - static_cast<int64_t>(frame_index * frame_dur) + this->audio_lead_us_;
    }

    int64_t target_present_time_us =
        this->playback_start_time_us_ + this->paused_accum_us_ + static_cast<int64_t>(frame_index * frame_dur);

#ifdef SVP_DSI_OUTPUT
    // Direct path: if more than one frame BEHIND the file clock, re-anchor the clock to now and
    // carry on at 1x -- this HW cannot sprint to catch up. Every frame still plays, just shifted
    // later; audio rides the same feed rate so it stays aligned. Not frame-dropping.
    if (direct && (esp_timer_get_time() - target_present_time_us) > frame_dur) {
      this->playback_start_time_us_ =
          esp_timer_get_time() - static_cast<int64_t>(frame_index * frame_dur) - this->paused_accum_us_;
      target_present_time_us =
          this->playback_start_time_us_ + this->paused_accum_us_ + static_cast<int64_t>(frame_index * frame_dur);
    }
#endif

    // Sync to the wall clock by COMPARING it, never sleeping on it. Bare spin -- one storage
    // update() per frame (top of the loop) is enough (each fetched chunk is pre-decode compressed
    // video), and the per-frame esp_task_wdt_reset() below covers the <=1-frame spin.
    while (target_present_time_us - esp_timer_get_time() > 0) {
    }

    // Late-frame accounting (plain counter, summarised after the loop; no logging here). The
    // achieved interval between paced releases: when decode kept up, the spin above aligned it to
    // ~frame_dur, so a longer interval means the previous frame overran the budget. >10% over
    // filters out sub-ms wall-clock jitter.
    const int64_t present_now = esp_timer_get_time();
    if (this->last_present_us_ != 0 && (present_now - this->last_present_us_) > frame_dur + frame_dur / 10) {
      this->late_frame_count_++;
    }
    this->last_present_us_ = present_now;

    if (repeat) {
      xQueueSend(this->empty_queue_, &slot, 0);
      esp_task_wdt_reset();
      continue;
    }

    // decode_frame_() consumes slot->data (compressed) into the decode target. Once it returns, the
    // compressed bytes are no longer needed -- the async PPA rotate reads the decoded buffer, not
    // slot->data -- so the slot is returned to the reader right after.
    {
      const uint8_t *d = slot->data;
      bool ok = payload >= 4 && d[0] == 0xFF && d[1] == 0xD8;
      if (ok) {
        ok = false;
        for (int i = payload - 2; i >= 0 && i >= payload - 16; i--) {
          if (d[i] == 0xFF && d[i + 1] == 0xD9) {
            ok = true;
            break;
          }
        }
      }
      if (!ok)
        this->bad_payload_count_++;
    }
    const uint32_t dec_start = micros();
    const bool decoded = this->decode_frame_(slot->data, static_cast<size_t>(payload));
    if (decoded) {
      const uint32_t dec_us = micros() - dec_start;
      this->decode_us_sum_ += dec_us;
      if (dec_us > this->decode_us_max_)
        this->decode_us_max_ = dec_us;
    } else {
      // No logging on this pacing path (AGENTS.md) -- plain counter, summarised after the loop.
      this->decode_fail_count_++;
    }
    // decode_frame_() wrote the frame into the back buffer and published it; loop() points the canvas at it.

#ifdef SVP_DSI_OUTPUT
    if (decoded && direct) {
      // Portall's plain mode: the frame was decoded into our own buffer in the display's depth; the
      // display copies it in. Panel dims are multiples of 16 (init_dsi_output_), so no x_pad.
      {
        // 4x4 sample grid; all 16 identical -> the decoder produced a single-colour frame.
        const uint8_t bpp = this->dsi_fb_bpp_;
        const size_t stride = static_cast<size_t>(this->dsi_out_w_) * bpp;
        const uint8_t *first = nullptr;
        bool solid = true;
        for (uint32_t gy = 0; gy < 4 && solid; gy++) {
          const size_t y = (2 * gy + 1) * this->dsi_out_h_ / 8;
          for (uint32_t gx = 0; gx < 4; gx++) {
            const uint8_t *px = this->decode_target_ + y * stride + ((2 * gx + 1) * this->dsi_out_w_ / 8) * bpp;
            if (first == nullptr) {
              first = px;
            } else if (std::memcmp(px, first, bpp) != 0) {
              solid = false;
              break;
            }
          }
        }
        if (solid) {
          if (this->solid_frame_count_ < SOLID_FRAME_LOG_MAX)
            this->solid_frame_idx_[this->solid_frame_count_] = frame_index;
          this->solid_frame_count_++;
        }
      }
#ifdef SVP_CONTROLS
      this->composite_controls_();
#endif
      const uint32_t present_start = micros();
      const display::ColorBitness bitness =
          this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
      this->dsi_->draw_pixels_at(0, 0, this->dsi_out_w_, this->dsi_out_h_, this->decode_target_,
                                 display::COLOR_ORDER_RGB, bitness, false, 0, 0, 0);
      const uint32_t present_us = micros() - present_start;
      this->present_us_sum_ += present_us;
      if (present_us > this->present_us_max_)
        this->present_us_max_ = present_us;
    }
#endif

    xQueueSend(this->empty_queue_, &slot, 0);  // return the slot to the reader

    esp_task_wdt_reset();
  }

#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr) {
    xSemaphoreGive(this->output_lock_);
    this->stream_set_awake_(true);  // a connected sender redraws the whole page
  }
#endif

  // Stop the reader/producer and wait for it to exit before the file reader is torn down.
  // playback_task_stop_ also aborts any in-flight BufferedFileReader wait the reader is parked in.
  this->reader_task_stop_ = true;
  this->playback_task_stop_ = true;
  this->wait_for_task_stop_(this->reader_task_handle_, 2000);

  // One-line playback analysis summary -- safe here (the loop has exited, not the hot path). All
  // figures came from plain per-frame counters/micros() on the pacing path, never logging there.
  const uint32_t stat_frames = this->video_frame_index_;
  if (stat_frames > 0) {
    const float late_pct = 100.0f * this->late_frame_count_ / stat_frames;
    ESP_LOGI(TAG,
             "playback stats: %" PRIu32 " frames, %" PRIu32 " late (%.1f%%), decode avg %" PRIu32 " / max %" PRIu32
             " us, present avg %" PRIu32 " / max %" PRIu32 " us, %" PRIu32 " decode failures, %" PRIu32 " bad payloads (no SOI/EOI)",
             stat_frames, this->late_frame_count_, late_pct,
             static_cast<uint32_t>(this->decode_us_sum_ / stat_frames), this->decode_us_max_,
             static_cast<uint32_t>(this->present_us_sum_ / stat_frames), this->present_us_max_,
             this->decode_fail_count_, this->bad_payload_count_);
#ifdef SVP_DSI_OUTPUT
    if (direct) {
      char idx[SOLID_FRAME_LOG_MAX * 11 + 1] = "";
      size_t pos = 0;
      const uint32_t shown = std::min<uint32_t>(this->solid_frame_count_, SOLID_FRAME_LOG_MAX);
      for (uint32_t i = 0; i < shown && pos < sizeof(idx); i++)
        pos += snprintf(idx + pos, sizeof(idx) - pos, " %" PRIu32, this->solid_frame_idx_[i]);
      ESP_LOGI(TAG, "decoded single-colour frames: %" PRIu32 "%s%s", this->solid_frame_count_, shown ? ", first at:" : "",
               idx);
    }
#endif
  }
  if (this->file_reader_ != nullptr && stat_frames > 0) {
    const auto &fs = this->file_reader_->fill_stats();
    const float play_us = static_cast<float>(stat_frames) * this->frame_duration_us_;
    ESP_LOGI(TAG,
             "read stats: %" PRIu32 " underruns, min ring %u KB, %" PRIu32 " chunks, chunk avg %" PRIu32
             " / max %" PRIu32 " us, fill %.2f MB/s, video needs %.2f MB/s",
             fs.underruns, static_cast<unsigned>(fs.min_avail / 1024), fs.chunks,
             fs.chunks ? static_cast<uint32_t>(fs.chunk_us / fs.chunks) : 0u, fs.chunk_us_max,
             fs.chunk_us ? static_cast<float>(fs.bytes_filled) / static_cast<float>(fs.chunk_us) : 0.0f,
             play_us > 0 ? static_cast<float>(this->file_reader_->tell()) / play_us : 0.0f);
  }

  // Release any in-flight BufferedFileReader wait before close_file_() tears the reader down.
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

  // Blank the canvas on stop -- cosmetic best-effort (it otherwise keeps showing the last frame).
  // memset here on the playback task, invalidate from loop() via frame_ready_.
  if (!direct && this->output_buffers_[0] != nullptr) {
    for (auto *buf : this->output_buffers_)
      std::memset(buf, 0, this->output_buffer_size_);
    this->display_buffer_.store(this->output_buffers_[0], std::memory_order_release);
    this->frame_ready_.store(true, std::memory_order_release);
  }

  // Don't clobber an ERROR state recorded by set_error_() -- get_state()/get_last_error() must
  // still report the failure after the task exits. CAS from anything-but-ERROR to STOPPED.
  PlayerState expected = this->state_.load(std::memory_order_acquire);
  while (expected != PlayerState::ERROR &&
         !this->state_.compare_exchange_weak(expected, PlayerState::STOPPED, std::memory_order_acq_rel)) {
  }

  esp_task_wdt_delete(nullptr);
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

int SimpleVideoPlayer::read_frame_(uint8_t *dest, size_t cap) {
  while (true) {
    int n = this->read_next_frame_(dest, cap);
    if (n > 0 || n == AVIParser::REPEAT_FRAME) {
      return n;
    }
#ifdef SVP_STREAM
    const bool can_rewind = !this->net_session_;
#else
    const bool can_rewind = true;
#endif
    if (n == 0 && this->loop_ && can_rewind) {
      // Only re-check state on the rare rewind path; the caller already checked it this iteration.
      const PlayerState s = this->state_.load(std::memory_order_acquire);
      if (s != PlayerState::PLAYING && s != PlayerState::PAUSED) {
        return -2;  // stopped / aborted
      }
      if (!this->reader_rewind_())
        return -1;
      this->cache_buffer_valid_ = 0;
      this->cache_buffer_offset_ = 0;
#ifdef USE_AUDIO
      this->reset_audio_cursor_();
#endif
      continue;
    }
    return n;  // 0 = EOF, -1 = read error
  }
}

int SimpleVideoPlayer::read_next_frame_(uint8_t *dest_buffer, size_t dest_capacity) {
  // Read the next VIDEO frame from the file (demuxing + feeding audio inline); runs on the
  // reader task (Core 0), writing into the caller-provided frame-slot buffer.
  if (this->video_format_ == VideoFormat::AVI_MJPEG) {
    // AVI format - use parser to get next frame (video or audio)
    AVIFrame frame;
    int bytes_read = this->avi_parser_->read_next_frame(frame, dest_buffer, dest_capacity);
    // Consume interleaved audio chunks until the next video frame -- feed each one straight
    // through; audio free-runs on the speaker clock.
    while (bytes_read > 0 && frame.stream_type != AVIStreamType::VIDEO) {
#ifdef USE_AUDIO
      if (frame.stream_type == AVIStreamType::AUDIO && this->audio_enabled_) {
        this->reader_audio_chunk_(frame, dest_buffer, bytes_read);
      }
#endif
      bytes_read = this->avi_parser_->read_next_frame(frame, dest_buffer, dest_capacity);
    }
    if (bytes_read <= 0 && bytes_read != AVIParser::REPEAT_FRAME) {
      return bytes_read;  // EOF or error
    }
#ifdef USE_AUDIO
    this->reader_passed_video_();
#endif

    return bytes_read;
  } else {
    // Raw MJPEG - search for JPEG EOI marker to find frame boundary
    size_t frame_size = 0;
    uint8_t *frame_ptr = dest_buffer;

    while (true) {
      // Read more data into cache if needed
      if (this->cache_buffer_offset_ >= this->cache_buffer_valid_) {
        // Cache is exhausted, read next chunk
        int bytes_read = this->read_data_(this->cache_buffer_.get(), CACHE_BUFFER_SIZE);

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
  // Canvas path: RGB888/BGR so the in-memory byte order (B,G,R) matches LVGL's LV_COLOR_FORMAT_RGB888.
  // Frame size padded to 16 (HW requirement); the decoder is told the target buffer's own size.
  jpeg_decode_cfg_t decode_cfg = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
  };
  // Canvas path: decode into the back buffer (the one loop() is NOT currently showing).
  uint8_t *out_buf = this->output_buffers_[this->decode_buf_idx_];
  uint32_t out_cap = static_cast<uint32_t>(this->output_buffer_size_);
#ifdef SVP_DSI_OUTPUT
  const bool direct = this->direct_session_();
  if (direct) {
    // Direct mode: decode into our own buffer in the display's depth (color_depth from the YAML).
    // BGR element order for BOTH depths: it puts the lowest-addressed byte at blue, which is the
    // little-endian layout esp_lcd's RGB565 and RGB888 framebuffers both read.
    out_buf = this->decode_target_;
    out_cap = static_cast<uint32_t>(this->decode_target_len_);
    if (this->dsi_fb_bpp_ == 2) {
      decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    }
  }
#endif
  uint32_t out_size = 0;
  esp_err_t err = jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, frame_data,
                                       static_cast<uint32_t>(ALIGN_UP(frame_size, 16)), out_buf, out_cap, &out_size);
  if (err != ESP_OK || out_size == 0)
    return false;
#ifdef SVP_DSI_OUTPUT
  if (!direct)
#endif
  {
    // The decoder pads each row to its MCU width: 16 pixels for 4:2:2 and 4:2:0, 8 otherwise
    // (esp_driver_jpeg jpeg_parse_marker.c, process_h/process_v). Only 8-pixel rows give w8 * h8.
    const uint32_t w8 = ALIGN_UP(this->canvas_w_, 8);
    const uint32_t row = out_size / 3 == w8 * ALIGN_UP(this->canvas_h_, 8) ? w8 : ALIGN_UP(this->canvas_w_, 16);
    this->canvas_stride_.store(row * 3, std::memory_order_relaxed);
    // Publish the just-decoded buffer for loop() and flip the decode target to the other buffer, so
    // the next decode never writes the buffer LVGL is about to blit (the paced decode + frequent
    // loop() guarantee loop() repoints before the buffer is reused -- same 2-buffer invariant the
    // DSI direct path relies on).
    this->display_buffer_.store(out_buf, std::memory_order_release);
    this->decode_buf_idx_ ^= 1;
    this->frame_ready_.store(true, std::memory_order_release);  // loop() points the canvas at it
  }
  return true;
}

#ifdef SVP_DSI_OUTPUT
bool SimpleVideoPlayer::init_dsi_output_() {
  // Portall's plain mode: decode into our own buffer in the display's depth, then draw_pixels_at()
  // copies it in. The video is authored in the panel's native orientation + resolution (the user
  // transposes at transcode); a size mismatch is caught at play().
  this->dsi_out_w_ = static_cast<uint16_t>(this->dsi_->get_native_width());
  this->dsi_out_h_ = static_cast<uint16_t>(this->dsi_->get_native_height());

  // The HW JPEG decoder writes whole 16x16 units; with a panel that is a multiple of 16 the decoded
  // rows have no padding, so draw_pixels_at() stays a single transfer (no per-line x_pad copy).
  if (this->dsi_out_w_ == 0 || this->dsi_out_h_ == 0 || (this->dsi_out_w_ & 15) != 0 || (this->dsi_out_h_ & 15) != 0) {
    ESP_LOGE(TAG, "panel %ux%u is not a multiple of 16 on both axes; direct video needs that", this->dsi_out_w_,
             this->dsi_out_h_);
    return false;
  }

  ESP_LOGI(TAG, "Direct output: panel %ux%u, %s, drawn with draw_pixels_at", this->dsi_out_w_, this->dsi_out_h_,
           this->dsi_fb_bpp_ == 3 ? "RGB888" : "RGB565");
  return true;
}
#endif  // SVP_DSI_OUTPUT

bool SimpleVideoPlayer::init_decoder_() {
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

    this->avi_fps_num_ = video_info->fps_num;
    this->avi_fps_den_ = video_info->fps_den;
    ESP_LOGI(TAG, "AVI video dimensions: %" PRIu32 "x%" PRIu32 ", FPS: %" PRIu32 "/%" PRIu32, width, height,
             video_info->fps_num, video_info->fps_den);
    return true;
  } else {
    // Raw MJPEG - read first chunk to get JPEG header
    int bytes_read = this->read_data_(this->cache_buffer_.get(), CACHE_BUFFER_SIZE);
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
#ifdef SVP_STREAM
  if (this->net_session_)
    return this->open_stream_source_();
#endif
  ESP_LOGI(TAG, "Opening file: %s", path.c_str());

  // Storage-backed file reader (see buffered_file_reader.h): resolves the path against the storage
  // registry and streams via the storage worker into the read-ahead ring (pool) -- it allocates
  // nothing. Kept across play() calls; open() closes any previous stream first.
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
    this->avi_parser_->set_index_memory(this->index_buf_, this->index_reserve_);

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
    // The index now: seeking, and damage passed over to the next intact frame.
    this->avi_parser_->load_index(this->cache_buffer_.get(), CACHE_BUFFER_SIZE);
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

#ifdef SVP_STREAM
  // The network task stops pushing (and drops the connection) once this is gone.
  this->stream_sink_.store(nullptr, std::memory_order_release);
#endif
  // Close file reader -- NOT reset()/destroyed: kept alive so its PSRAM read-ahead buffers are
  // reused by the next open_file_() instead of being freed and re-malloc'd every play() call.
  if (this->file_reader_) {
    this->file_reader_->close();
  }

  this->file_size_ = 0;
  this->video_format_ = VideoFormat::UNKNOWN;
}

#ifdef SVP_STREAM
bool SimpleVideoPlayer::open_stream_source_() {
  // An AVI arriving on the stream port (svp_relay): read through the same reader and ring as a
  // file, which the network task fills instead of the storage worker.
  this->file_reader_->set_abort_flag(&this->playback_task_stop_);
  if (!this->file_reader_->open_stream()) {
    ESP_LOGE(TAG, "Failed to open the network stream");
    return false;
  }
  this->stream_sink_.store(this->file_reader_.get(), std::memory_order_release);
  this->file_size_ = 0;
  this->video_format_ = VideoFormat::AVI_MJPEG;
  if (!this->avi_parser_) {
    this->avi_parser_ = std::make_unique<AVIParser>();
  }
  this->avi_parser_->set_index_memory(this->index_buf_, this->index_reserve_);
  if (!this->avi_parser_->open(this->file_reader_.get())) {
    ESP_LOGE(TAG, "Network stream is not an AVI this player reads");
    this->close_file_();
    return false;
  }
  return true;
}
#endif

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

void SimpleVideoPlayer::handle_seek_() {
  this->seek_pending_.store(false, std::memory_order_release);
  const uint32_t target = this->seek_target_.load(std::memory_order_relaxed);
#ifdef USE_AUDIO
  const bool audio = this->audio_enabled_ && this->audio_task_handle_ != nullptr;
  if (audio) {
    this->audio_held_.store(false, std::memory_order_release);
    this->audio_hold_.store(true, std::memory_order_release);
    for (uint32_t waited = 0; !this->audio_held_.load(std::memory_order_acquire) && waited < 500; waited++)
      vTaskDelay(pdMS_TO_TICKS(1));
  }
#endif
  this->reader_seek_frame_.store(target, std::memory_order_relaxed);
  this->reader_seek_req_.store(true, std::memory_order_release);
  while (this->reader_seek_req_.load(std::memory_order_acquire) && this->reader_task_handle_ != nullptr) {
    const PlayerState st = this->state_.load(std::memory_order_acquire);
    if (st != PlayerState::PLAYING && st != PlayerState::PAUSED)
      break;
    if (storage::global_storage_worker != nullptr)
      storage::global_storage_worker->update();
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  const bool ok = !this->reader_seek_req_.exchange(false, std::memory_order_acq_rel) &&
                  this->reader_seek_ok_.load(std::memory_order_relaxed);
  this->expected_gen_ = this->frame_gen_.load(std::memory_order_acquire);
#ifdef USE_AUDIO
  if (audio) {
    if (ok) {
      // The speaker drops what it still holds from before the jump, then starts again as at the
      // start of playback, with the sound audio_lead_frames_ ahead of the picture.
      this->speaker_->stop();
      for (uint32_t waited = 0; !this->speaker_->is_stopped() && waited < 500; waited++)
        vTaskDelay(pdMS_TO_TICKS(1));
      this->speaker_->start();
      this->wait_speaker_running_();
      for (uint32_t waited = 0; this->audio_cursor_frames_.load(std::memory_order_acquire) <
                                    this->reader_frames_.load(std::memory_order_acquire) + this->audio_lead_frames_ &&
                                waited < 1000;
           waited++) {
        if (storage::global_storage_worker != nullptr)
          storage::global_storage_worker->update();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    this->audio_hold_.store(false, std::memory_order_release);
  }
#endif
  if (ok) {
    this->playback_start_time_us_ = 0;  // anchored again on the next frame, like the first one
    this->last_present_us_ = 0;
    this->position_frame_.store(target, std::memory_order_relaxed);
  } else {
    ESP_LOGW(TAG, "Seek failed");
  }
}

bool SimpleVideoPlayer::reader_seek_(uint32_t frame) {
  if (this->video_format_ != VideoFormat::AVI_MJPEG || !this->avi_parser_ || !this->file_reader_)
    return false;
  BufferedFileReader *reader = this->file_reader_.get();
  // Index and seek need blocking reads; the ring then restarts empty and is filled before frames
  // are read again, as at the start of playback.
  reader->set_streaming(false);
  const bool ok = this->avi_parser_->load_index(this->cache_buffer_.get(), CACHE_BUFFER_SIZE) &&
                  this->avi_parser_->seek_to_frame(frame);
  reader->prefill_cache();
  reader->set_streaming(true);
  if (!ok)
    return false;
#ifdef USE_AUDIO
  if (this->audio_enabled_)
    this->restart_audio_feed_();
  this->reset_audio_cursor_();
#endif
  return true;
}

bool SimpleVideoPlayer::reader_rewind_() {
  if (!this->file_reader_ || !this->file_reader_->is_open())
    return false;
  BufferedFileReader *reader = this->file_reader_.get();
  reader->set_streaming(false);
  const bool ok = this->video_format_ == VideoFormat::AVI_MJPEG ? this->avi_parser_->rewind() : reader->seek(0);
  reader->prefill_cache();
  reader->set_streaming(true);
  return ok;
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
#ifdef SVP_DSI_OUTPUT
  if (this->direct_session_()) {
    // DSI direct output: the video must be transcoded to exactly the resolution the panel needs
    // (native res with W/H swapped for a 90/270 rotation). No scaling -- the user matches it.
    if (video_width != this->dsi_out_w_ || video_height != this->dsi_out_h_) {
      ESP_LOGE(TAG, "Video is %" PRIu32 "x%" PRIu32 " but this panel needs exactly %ux%u -- re-transcode.", video_width,
               video_height, this->dsi_out_w_, this->dsi_out_h_);
      return false;
    }
    ESP_LOGI(TAG, "Buffers verified (direct): %ux%u", this->dsi_out_w_, this->dsi_out_h_);
    return true;
  }
#endif
  // output_buffers_ are allocated once in setup() at the max resolution. Just verify this file fits.
  uint32_t aligned_width = ALIGN_UP(video_width, 16);
  uint32_t aligned_height = ALIGN_UP(video_height, 16);
  size_t required = static_cast<size_t>(aligned_width) * aligned_height * 3;  // RGB888

  ESP_LOGI(TAG, "Verifying buffers for %" PRIu32 "x%" PRIu32 " video (aligned: %" PRIu32 "x%" PRIu32 ", %zu bytes)",
           video_width, video_height, aligned_width, aligned_height, required);

  if (this->output_buffers_[0] == nullptr || required > this->output_buffer_size_) {
    ESP_LOGE(TAG, "Video too large for the output buffer: %" PRIu32 "x%" PRIu32 " needs %zu bytes, have %zu",
             aligned_width, aligned_height, required, this->output_buffer_size_);
    return false;
  }
  return true;
}

void SimpleVideoPlayer::free_buffers_() {
  if (this->hw_jpeg_decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->hw_jpeg_decoder_);
    this->hw_jpeg_decoder_ = nullptr;
  }

  for (auto *&buf : this->output_buffers_) {
    this->pool_.free(buf);
    buf = nullptr;
  }
  this->output_buffer_size_ = 0;
  this->pool_.free(this->ring_buf_);
  this->ring_buf_ = nullptr;
  this->display_buffer_.store(nullptr, std::memory_order_release);

#ifdef USE_AUDIO
  // The sound buffers live in the pool, which is never given back.
  this->audio_input_ring_buffer_.reset();
  this->audio_decoded_ring_buffer_.reset();
  this->audio_temp_buffer_ = nullptr;
#endif

  for (auto &slot : this->frame_slots_) {
    this->pool_.free(slot.data);
    slot.data = nullptr;
  }
  if (this->empty_queue_ != nullptr) {
    vQueueDelete(this->empty_queue_);
    this->empty_queue_ = nullptr;
  }
  if (this->filled_queue_ != nullptr) {
    vQueueDelete(this->filled_queue_);
    this->filled_queue_ = nullptr;
  }

#ifdef SVP_DSI_OUTPUT
  this->pool_.free(this->decode_target_);
  this->decode_target_ = nullptr;
  this->decode_target_len_ = 0;
#endif
}

//========================================================================
// PSRAM pool
//========================================================================

static constexpr size_t MIB = 1024 * 1024;
static size_t pool_round(size_t bytes) { return (bytes + PsramPool::ALIGN - 1) & ~(PsramPool::ALIGN - 1); }

size_t SimpleVideoPlayer::canvas_output_size_() const {
#ifdef SVP_CANVAS_OUTPUT
  return pool_round(static_cast<size_t>(ALIGN_UP(this->canvas_size_w_, 16)) * ALIGN_UP(this->canvas_size_h_, 16) * 3);
#else
  return 0;
#endif
}

// Everything but the ring gets what it needs at most; the ring gets the rest, in whole fill chunks.
bool SimpleVideoPlayer::plan_pool_() {
  const size_t pool = this->pool_.size();
  this->frame_bytes_ = pool_round(std::clamp<size_t>(pool / 24, 256 * 1024, MIB));
  size_t fixed = this->frame_bytes_ * FRAME_SLOT_COUNT;
  size_t output = 0;
#ifdef SVP_DSI_OUTPUT
  if (this->dsi_ != nullptr)
    output = pool_round(static_cast<size_t>(this->dsi_out_w_) * this->dsi_out_h_ * this->dsi_fb_bpp_);
#endif
  output = std::max(output, 2 * this->canvas_output_size_());
  this->output_reserve_ = output;
  fixed += output;
#ifdef SVP_CONTROLS
  if (this->ctl_widget_ != nullptr && this->dsi_ != nullptr) {
    lv_obj_update_layout(this->ctl_widget_);
    const size_t area =
        static_cast<size_t>(lv_obj_get_width(this->ctl_widget_)) * static_cast<size_t>(lv_obj_get_height(this->ctl_widget_));
    this->controls_reserve_ = 2 * pool_round(sizeof(ControlsBlock) + area * this->dsi_fb_bpp_);
    fixed += this->controls_reserve_;
  }
#endif
#ifdef SVP_STREAM
  if (this->stream_port_ != 0) {
    this->stream_max_frame_bytes_ = pool_round(this->frame_bytes_ / 4);
    fixed += STREAM_FRAME_COUNT * static_cast<size_t>(this->stream_max_frame_bytes_);
  }
#endif
#ifdef SVP_PICTURES
  if (this->pic_canvas_ != nullptr && this->thumbs_enabled_ && this->thumb_container_ != nullptr &&
      this->thumb_count_ > 0)
    fixed += pool_round(static_cast<size_t>(lv_draw_buf_width_to_stride(this->thumb_w_, LV_COLOR_FORMAT_RGB565)) *
                        this->thumb_h_ * this->thumb_count_);
#endif
  this->index_reserve_ = pool_round(std::clamp<size_t>(pool / 32, 128 * 1024, MIB));
  fixed += this->index_reserve_ + 32 * PsramPool::ALIGN;  // pieces end on whole cache lines

  const size_t chunk = BufferedFileReader::FILL_CHUNK;
  auto ring_for = [pool, chunk](size_t taken) -> size_t { return taken + 2 * chunk <= pool ? (pool - taken) / chunk * chunk : 0; };
  size_t ring = ring_for(fixed);
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr && ring != 0) {
    // The sound buffers hold what the ring lets the sound run ahead; sized for the larger ring, they
    // fit the smaller one that is left.
    const auto latency = static_cast<uint32_t>(ring * 100 / MIB);
    ring = ring_for(fixed + pool_round(audio_input_bytes(latency)) + pool_round(audio_decoded_bytes(latency)) +
                    pool_round(AUDIO_TEMP_BUFFER_SIZE));
  }
#endif
  if (ring == 0) {
    ESP_LOGE(TAG, "psram_pool_size %u is too small: %u bytes go to frame slot, outputs, sound, controls and index",
             static_cast<unsigned>(pool), static_cast<unsigned>(fixed));
    return false;
  }
  this->ring_size_ = ring;
  this->max_audio_latency_ms_ = static_cast<uint32_t>(ring * 100 / MIB);
  return true;
}

uint8_t *SimpleVideoPlayer::media_alloc_(size_t size) {
  uint8_t *piece = this->pool_.alloc(size);
#ifdef SVP_PICTURES
  if (piece == nullptr && this->pic_ready_) {
    // The pictures' buffers, handed back by the main loop once nothing of theirs is in flight.
    this->pool_reclaim_req_.store(true, std::memory_order_release);
    for (uint32_t waited = 0; this->pool_reclaim_req_.load(std::memory_order_acquire) && waited < 2000; waited += 5)
      vTaskDelay(pdMS_TO_TICKS(5));
    piece = this->pool_.alloc(size);
  }
#endif
  if (piece == nullptr)
    ESP_LOGE(TAG, "PSRAM pool: no room for %zu bytes (%zu of %zu used, largest free %zu)", size, this->pool_.used(),
             this->pool_.size(), this->pool_.largest_free());
  return piece;
}

bool SimpleVideoPlayer::acquire_session_buffers_() {
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  for (auto &slot : this->frame_slots_) {
    if (slot.data == nullptr) {
      slot.data = this->media_alloc_(this->frame_bytes_);
      slot.capacity = slot.data != nullptr ? this->frame_bytes_ : 0;
    }
  }
  if (this->index_buf_ == nullptr)
    this->index_buf_ = reinterpret_cast<uint32_t *>(this->media_alloc_(this->index_reserve_));
  if (this->ring_buf_ == nullptr)
    this->ring_buf_ = this->media_alloc_(this->ring_size_);
  bool ok = this->ring_buf_ != nullptr && this->index_buf_ != nullptr;
  for (auto &slot : this->frame_slots_)
    ok = ok && slot.data != nullptr;
  xSemaphoreGive(this->media_lock_);
  if (!ok)
    return false;
  if (!this->file_reader_)
    this->file_reader_ = std::make_unique<BufferedFileReader>();
  if (!this->file_reader_->has_ring())
    this->file_reader_->set_ring(this->ring_buf_, this->ring_size_);
  return true;
}

// Playback or stream task, media lock not held. When the other output's memory is needed, the main
// loop gives it back (handle_output_release_()); then the pictures' if still short.
uint8_t *SimpleVideoPlayer::output_alloc_(size_t size, uint8_t other) {
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  uint8_t *piece = this->pool_.alloc(size);
  xSemaphoreGive(this->media_lock_);
  if (piece != nullptr)
    return piece;
  this->output_release_req_.store(other, std::memory_order_release);
  for (uint32_t waited = 0; this->output_release_req_.load(std::memory_order_acquire) != 0 && waited < 2000;
       waited += 5)
    vTaskDelay(pdMS_TO_TICKS(5));
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  piece = this->media_alloc_(size);
  xSemaphoreGive(this->media_lock_);
  return piece;
}

#ifdef SVP_DSI_OUTPUT
bool SimpleVideoPlayer::ensure_direct_target_() {
  if (this->decode_target_ != nullptr)
    return true;
  // Whole cache lines, as the JPEG unit writes it by DMA.
  const size_t size = pool_round(static_cast<size_t>(this->dsi_out_w_) * this->dsi_out_h_ * this->dsi_fb_bpp_);
  uint8_t *target = this->output_alloc_(size, RELEASE_CANVAS);
  if (target == nullptr)
    return false;
#ifdef SVP_CONTROLS
  // The controls drawn over the direct output: both pictures, with the display's decode buffer.
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  for (int i = 0; i < 2 && this->controls_reserve_ != 0; i++) {
    if (this->ctl_bufs_[i] == nullptr) {
      this->ctl_bufs_[i] = this->pool_.alloc(this->controls_reserve_ / 2);
      this->ctl_buf_size_[i] = this->ctl_bufs_[i] != nullptr ? this->controls_reserve_ / 2 : 0;
    }
  }
  xSemaphoreGive(this->media_lock_);
#endif
  this->decode_target_len_ = size;
  this->decode_target_ = target;
  return true;
}
#endif

#ifdef SVP_CANVAS_OUTPUT
bool SimpleVideoPlayer::ensure_canvas_outputs_() {
  if (this->output_buffers_[1] != nullptr)
    return true;
  // RGB888 double buffer of the canvas' size (rows of 16): decode writes one while LVGL reads the
  // other. RGB888, not RGB565: the decoder's RGB565 output is buggy on some P4 revisions.
  const size_t size = this->canvas_output_size_();
  uint8_t *buffers[2] = {this->output_buffers_[0], nullptr};
  for (auto *&buf : buffers) {
    if (buf == nullptr)
      buf = this->output_alloc_(size, RELEASE_DIRECT);
  }
  if (buffers[0] == nullptr || buffers[1] == nullptr) {
    xSemaphoreTake(this->media_lock_, portMAX_DELAY);
    for (auto *buf : buffers)
      this->pool_.free(buf);
    this->output_buffers_[0] = nullptr;
    xSemaphoreGive(this->media_lock_);
    return false;
  }
  for (auto *buf : buffers)
    std::memset(buf, 0, size);
  this->output_buffer_size_ = size;
  this->output_buffers_[0] = buffers[0];
  this->output_buffers_[1] = buffers[1];
  return true;
}
#endif

// Main loop. Only when nothing draws into the output given back: the stream draws under output_lock_.
void SimpleVideoPlayer::handle_output_release_() {
  const uint8_t which = this->output_release_req_.load(std::memory_order_acquire);
  if (which == 0)
    return;
#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr && xSemaphoreTake(this->output_lock_, 0) != pdTRUE)
    return;  // next loop
#endif
  if (xSemaphoreTake(this->media_lock_, 0) == pdTRUE) {
#ifdef SVP_CANVAS_OUTPUT
    if (which == RELEASE_CANVAS && this->output_buffers_[0] != nullptr) {
      lv_image_set_src(this->canvas_, nullptr);
      std::memset(&this->canvas_draw_buf_, 0, sizeof(this->canvas_draw_buf_));
      this->display_buffer_.store(nullptr, std::memory_order_release);
      this->frame_ready_.store(false, std::memory_order_release);
      for (auto *&buf : this->output_buffers_) {
        this->pool_.free(buf);
        buf = nullptr;
      }
      this->output_buffer_size_ = 0;
    }
#endif
#ifdef SVP_DSI_OUTPUT
    if (which == RELEASE_DIRECT && this->decode_target_ != nullptr) {
      this->pool_.free(this->decode_target_);
      this->decode_target_ = nullptr;
      this->decode_target_len_ = 0;
#ifdef SVP_CONTROLS
      for (int i = 0; i < 2; i++) {
        this->pool_.free(this->ctl_bufs_[i]);
        this->ctl_bufs_[i] = nullptr;
        this->ctl_buf_size_[i] = 0;
      }
#endif
    }
#endif
    xSemaphoreGive(this->media_lock_);
    this->output_release_req_.store(0, std::memory_order_release);
  }
#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr)
    xSemaphoreGive(this->output_lock_);
#endif
}

bool SimpleVideoPlayer::release_media_() {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  if (state == PlayerState::PLAYING || state == PlayerState::PAUSED || this->task_handle_ != nullptr ||
      this->reader_task_handle_ != nullptr)
    return false;
#ifdef USE_AUDIO
  if (this->audio_task_handle_ != nullptr)
    return false;
#endif
#ifdef SVP_STREAM
  if (this->stream_connected_.load(std::memory_order_acquire) || this->stream_avi_.load(std::memory_order_acquire))
    return false;
  if (this->output_lock_ != nullptr && xSemaphoreTake(this->output_lock_, 0) != pdTRUE)
    return false;  // a rectangle is being drawn
#endif
  bool released = false;
  if (xSemaphoreTake(this->media_lock_, 0) == pdTRUE) {
#ifdef SVP_CANVAS_OUTPUT
    if (this->output_buffers_[0] != nullptr) {
      // LVGL lets go of it first: no source, and the next frame sets the canvas up anew.
      lv_image_set_src(this->canvas_, nullptr);
      std::memset(&this->canvas_draw_buf_, 0, sizeof(this->canvas_draw_buf_));
      this->display_buffer_.store(nullptr, std::memory_order_release);
      this->frame_ready_.store(false, std::memory_order_release);
      for (auto *&buf : this->output_buffers_) {
        this->pool_.free(buf);
        buf = nullptr;
      }
      this->output_buffer_size_ = 0;
      released = true;
    }
#endif
#ifdef SVP_DSI_OUTPUT
    if (this->decode_target_ != nullptr) {
      this->pool_.free(this->decode_target_);
      this->decode_target_ = nullptr;
      this->decode_target_len_ = 0;
      released = true;
    }
#ifdef SVP_CONTROLS
    for (int i = 0; i < 2; i++) {
      this->pool_.free(this->ctl_bufs_[i]);
      this->ctl_bufs_[i] = nullptr;
      this->ctl_buf_size_[i] = 0;
    }
#endif
#endif
    if (this->ring_buf_ != nullptr) {
      if (this->file_reader_)
        this->file_reader_->set_ring(nullptr, 0);
      this->pool_.free(this->ring_buf_);
      this->ring_buf_ = nullptr;
      released = true;
    }
    for (auto &slot : this->frame_slots_) {
      if (slot.data != nullptr) {
        this->pool_.free(slot.data);
        slot.data = nullptr;
        slot.capacity = 0;
        released = true;
      }
    }
    if (this->index_buf_ != nullptr) {
      if (this->avi_parser_)
        this->avi_parser_->set_index_memory(nullptr, 0);
      this->pool_.free(reinterpret_cast<uint8_t *>(this->index_buf_));
      this->index_buf_ = nullptr;
      released = true;
    }
    xSemaphoreGive(this->media_lock_);
  }
#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr)
    xSemaphoreGive(this->output_lock_);
#endif
  return released;
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
  // Bits per sample only for PCM: a compressed track's header carries none (ffmpeg writes 0 for
  // MP3, libavformat/riffenc.c), its decoder produces them.
  const bool pcm = static_cast<AVIAudioCodec>(audio_info->codec) == AVIAudioCodec::PCM;
#ifdef SVP_AUDIO_ROUTER
  // Through an audio router: the file's own format goes to the router, which converts channels and
  // width to the selected output's (senders send that output's rate and channels). Bounded only by
  // the buffers, which are sized for the largest output.
  const uint8_t file_bits = pcm ? static_cast<uint8_t>(audio_info->bits_per_sample) : 16;
  const size_t file_bytes_per_sec =
      static_cast<size_t>(audio_info->sample_rate) * audio_info->channels * (file_bits / 8);
  if (audio_info->channels == 0 || audio_info->channels > 8 || audio_info->sample_rate == 0 ||
      (file_bits != 16 && file_bits != 24 && file_bits != 32) || file_bytes_per_sec > AUDIO_BYTES_PER_SEC) {
    ESP_LOGE(TAG,
             "Audio format not playable: %" PRIu32 " Hz, %u ch, %u-bit (at most %zu bytes/s, 16/24/32-bit, "
             "up to 8 channels). Playing video-only.",
             audio_info->sample_rate, audio_info->channels, audio_info->bits_per_sample, AUDIO_BYTES_PER_SEC);
    return false;
  }
  this->source_audio_channels_ = static_cast<uint8_t>(audio_info->channels);
  this->audio_sample_rate_ = audio_info->sample_rate;
  this->audio_bits_per_sample_ = file_bits;
  this->audio_frame_bytes_ = static_cast<size_t>(audio_info->channels) * (file_bits / 8);
  this->speaker_audio_channels_ = this->source_audio_channels_;
  ESP_LOGI(TAG, "Audio: %u-channel source to the audio router", this->source_audio_channels_);
#else
  if (audio_info->channels != AUDIO_SOURCE_CHANNELS || audio_info->sample_rate != AUDIO_SAMPLE_RATE ||
      (pcm && audio_info->bits_per_sample != AUDIO_BITS_PER_SAMPLE)) {
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

  ESP_LOGI(TAG, "Audio routing: %u-channel source → %u-channel speaker (mode: %s)", this->source_audio_channels_,
           this->speaker_audio_channels_,
           this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_MONO     ? "mono"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_LEFT   ? "left"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_RIGHT  ? "right"
           : this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_STEREO ? "stereo"
                                                                                       : "unknown");
#endif

  // audio_codec is a LIST of supported codecs -- each listed one compiles in its decoder path
  // (SVP_AUDIO_SUPPORT_{PCM,MP3,FLAC}); the AVI parser detected THIS file's actual codec, so pick
  // the matching path at runtime. A file whose audio track uses a codec not in the supported set
  // falls through to video-only (same as the format mismatch above) -- never silently reconfigure.
  audio::AudioFileType codec_type = audio::AudioFileType::NONE;
  switch (static_cast<AVIAudioCodec>(audio_info->codec)) {
#ifdef SVP_AUDIO_SUPPORT_PCM
    case AVIAudioCodec::PCM:
      // PCM audio in AVI is raw samples without WAV header -- handled directly without AudioDecoder.
      codec_type = audio::AudioFileType::NONE;  // Signal that we don't need a decoder
      ESP_LOGI(TAG, "Audio codec: PCM (raw), %" PRIu32 " Hz, %u channels, %u bits - will process directly",
               audio_info->sample_rate, audio_info->channels, audio_info->bits_per_sample);
      break;
#endif
#ifdef SVP_AUDIO_SUPPORT_MP3
    case AVIAudioCodec::MP3:
      codec_type = audio::AudioFileType::MP3;
      ESP_LOGI(TAG, "Audio codec: MP3, %" PRIu32 " Hz, %u channels, %u bits", audio_info->sample_rate,
               audio_info->channels, audio_info->bits_per_sample);
      break;
#endif
#ifdef SVP_AUDIO_SUPPORT_FLAC
    case AVIAudioCodec::FLAC:
      codec_type = audio::AudioFileType::FLAC;
      ESP_LOGI(TAG, "Audio codec: FLAC, %" PRIu32 " Hz, %u channels, %u bits", audio_info->sample_rate,
               audio_info->channels, audio_info->bits_per_sample);
      break;
#endif
    default:
      ESP_LOGE(TAG,
               "Audio codec 0x%04" PRIX32 " is not in this player's supported audio_codec set. Playing video-only.",
               audio_info->codec);
      return false;
  }

  // CRITICAL: Configure speaker's audio stream info based on SPEAKER config, not file
  audio::AudioStreamInfo speaker_stream_info(this->audio_bits_per_sample_, this->speaker_audio_channels_,
                                             audio_info->sample_rate);
  this->speaker_->set_audio_stream_info(speaker_stream_info);
  ESP_LOGI(TAG, "Speaker configured: %u-bit, %u-channel, %" PRIu32 " Hz",
           static_cast<unsigned>(this->audio_bits_per_sample_),
           this->speaker_audio_channels_, audio_info->sample_rate);

  // Ring buffers + temp buffer already exist (allocated once in setup(), sized from the fixed
  // AUDIO_* constants) -- just clear out whatever a previous play() left in them so this session
  // starts clean. reset() is a cheap FreeRTOS ringbuffer reset, not a reallocation.
  this->audio_input_ring_buffer_->reset();
  if (this->audio_decoded_ring_buffer_) {
    this->audio_decoded_ring_buffer_->reset();
  }
#ifdef SVP_AUDIO_SUPPORT_FLAC
  // FLAC from an AVI: the decoder needs the native stream header ahead of the bare frames.
  if (codec_type == audio::AudioFileType::FLAC) {
    if (audio_info->stream_header_len == 0) {
      ESP_LOGE(TAG, "FLAC track without STREAMINFO in its format header. Playing video-only.");
      return false;
    }
    this->audio_input_ring_buffer_->write(audio_info->stream_header, audio_info->stream_header_len);
  }
#endif

  // For PCM audio, we don't need a decoder - just handle raw samples directly
  bool use_decoder = (codec_type != audio::AudioFileType::NONE);
  this->audio_file_type_ = codec_type;
  // Publish the per-file mode for the audio task / process_audio_frame_: in a multi-codec build
  // audio_decoder_ exists even for a PCM file, so the pointer can't signal the mode anymore.
  this->audio_use_decoder_.store(use_decoder, std::memory_order_release);

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

    // Formats are locked to match end to end -- decoder writes straight to the speaker.
    if (this->audio_decoder_->add_sink(this->speaker_) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to add audio decoder sink (speaker)");
      return false;
    }

    // Start audio decoder
    if (this->audio_decoder_->start(codec_type) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to start audio decoder");
      return false;
    }

    ESP_LOGI(TAG, "Audio decoder initialized successfully");
  }  // End of if (use_decoder)

  // Audio feed task on Core 0 -- audio never touches DMA2D/PPA/JPEG, so it stays off Core 1
  // entirely. Priority 1: it never sleeps, so a higher priority would let it starve the storage
  // worker / system tasks that also live on Core 0. At prio 1 the tick round-robins it with them,
  // it gets Core 0 whenever they are I/O-blocked (most of the time), and the speaker's own I2S
  // task drains the DMA in parallel. Fully decoupled from the Core 1 video decode/pace task.
  this->audio_task_stop_ = false;
  BaseType_t result = xTaskCreatePinnedToCore(audio_task_entry_, "svp_audio", 4096,  // 4KB stack
                                              this, 1,  // Priority 1 (never sleeps; must not starve Core 0)
                                              &this->audio_task_handle_,
                                              0);  // Core 0

  if (result != pdPASS || this->audio_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create audio processing task");
    return false;
  }

  // Speaker starts up in parallel with the video buffering; feeding it begins at the first frame
  // (wait_speaker_running_() + av_started_).
  this->speaker_->start();
  this->audio_enabled_ = true;
  ESP_LOGI(TAG, "Audio processing initialized successfully (%s mode)", use_decoder ? "decoder" : "direct PCM");
  return true;
}

void SimpleVideoPlayer::process_audio_frame_(const AVIFrame &frame, const uint8_t *data, size_t size) {
  // audio_enabled_ was already checked by the caller (read_next_frame_).

  // Dispatch by MODE, not by buffer presence: audio_input_ring_buffer_/audio_decoded_ring_buffer_
  // are both permanent, allocated unconditionally in setup() (see header), so they're non-null
  // regardless of codec -- audio_use_decoder_ (set per file in init_audio_decoder_()) is the mode
  // signal. In a multi-codec build audio_decoder_ exists even for a PCM file, so the pointer can't
  // signal the mode anymore.
  if (this->audio_use_decoder_.load(std::memory_order_acquire)) {
    // Compressed audio (MP3/FLAC): feed the decoder's input ring buffer. Whole chunks only and
    // never over older data: write() overwrites the oldest bytes when full (ring_buffer.h), which
    // before playback starts (nothing drains yet) is the FLAC stream header, and mid-stream cuts a
    // compressed frame in two. A chunk that does not fit is dropped whole.
    this->audio_input_ring_buffer_->write_without_replacement(data, size, 0, false);
    return;
  }

  // PCM: data is already decoded - write only complete frames to avoid glitches.
  const size_t bytes_per_frame = this->audio_frame_bytes_;
  size_t complete_frames = size / bytes_per_frame;
  size_t bytes_to_write = complete_frames * bytes_per_frame;

  // Log warning if we're dropping incomplete frames (shouldn't happen with well-formed AVI)
  if (size != bytes_to_write) {
    ESP_LOGW(TAG, "Dropped %zu bytes of incomplete audio frame", size - bytes_to_write);
  }
  if (bytes_to_write == 0) {
    return;
  }

  // Always buffer PCM into the decoded ring; the svp_audio task drains it to the speaker at a
  // steady rate. Feeding speaker->play() directly from here dumped a whole frame's worth of audio
  // in one burst every ~40 ms and the speaker dropped most of it (non-blocking), which is the
  // underrun.
  this->audio_decoded_ring_buffer_->write(data, bytes_to_write);
}

// Chunk ids of the AVI frame data (avi_parser.cpp).
static constexpr uint32_t AVI_CHUNK_LIST = 0x5453494C;  // 'LIST'
static constexpr uint32_t AVI_CHUNK_00DC = 0x63643030;  // '00dc'
static constexpr uint32_t AVI_CHUNK_00DB = 0x62643030;  // '00db'
static constexpr uint32_t AVI_CHUNK_01WB = 0x62773130;  // '01wb'

// Reader task, with the audio task held: the rings and the decoder start over at the new position.
void SimpleVideoPlayer::restart_audio_feed_() {
  const bool use_decoder = this->audio_use_decoder_.load(std::memory_order_acquire);
  bool ok = true;
  if (use_decoder) {
    // A new source first: the old one gives back what it holds of the ring, so the reset drops all.
    this->audio_decoder_->discard_output();
    std::weak_ptr<ring_buffer::RingBuffer> source = this->audio_input_ring_buffer_;
    ok = this->audio_decoder_->add_source(source) == ESP_OK;
  }
  this->audio_input_ring_buffer_->reset();
  if (this->audio_decoded_ring_buffer_)
    this->audio_decoded_ring_buffer_->reset();
  if (!use_decoder)
    return;
#ifdef SVP_AUDIO_SUPPORT_FLAC
  if (this->audio_file_type_ == audio::AudioFileType::FLAC) {
    const AVIStreamInfo *info = this->avi_parser_->get_audio_info();
    if (info != nullptr && info->stream_header_len != 0)
      this->audio_input_ring_buffer_->write(info->stream_header, info->stream_header_len);
  }
#endif
  if (!ok || this->audio_decoder_->start(this->audio_file_type_) != ESP_OK) {
    ESP_LOGE(TAG, "Audio decoder restart failed -- video only");
    this->audio_enabled_ = false;
  }
}

// Reader task. The cursor starts where the reader is.
void SimpleVideoPlayer::reset_audio_cursor_() {
  this->audio_cursor_pos_ = this->file_reader_ ? this->file_reader_->tell() : 0;
  this->audio_cursor_frames_.store(this->reader_frames_.load(std::memory_order_relaxed), std::memory_order_release);
}

// Reader task: walks the chunks ahead of the reader in the read-ahead ring, without taking them, and
// feeds their audio until it is audio_lead_frames_ video frames ahead. Stops where the ring has no
// data yet, and at an audio chunk larger than its scratch buffer (the frame read feeds that one).
void SimpleVideoPlayer::advance_audio_cursor_() {
  if (this->audio_lead_frames_ == 0 || !this->audio_enabled_ || !this->file_reader_ || !this->avi_parser_)
    return;
  BufferedFileReader *reader = this->file_reader_.get();
  const uint64_t end = this->avi_parser_->data_end();
  // The cache buffer only serves raw MJPEG; AVI playback leaves it free.
  uint8_t *scratch = this->cache_buffer_.get();
  while (this->audio_cursor_frames_.load(std::memory_order_relaxed) <
         this->reader_frames_.load(std::memory_order_relaxed) + this->audio_lead_frames_) {
    const uint64_t at = this->audio_cursor_pos_;
    if (at >= end || end - at < 8 || at < reader->tell())
      return;
    const size_t offset = static_cast<size_t>(at - reader->tell());
    uint8_t hdr[8];
    if (!reader->peek(offset, hdr, sizeof(hdr)))
      return;
    const uint32_t id = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    const uint32_t size = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
    if (id == AVI_CHUNK_LIST) {
      this->audio_cursor_pos_ = at + 12;  // into the list, as the parser goes
      continue;
    }
    if (id == AVI_CHUNK_00DC || id == AVI_CHUNK_00DB) {
      this->audio_cursor_frames_.fetch_add(1, std::memory_order_release);
    } else if (id == AVI_CHUNK_01WB && size > 0) {
      if (size > CACHE_BUFFER_SIZE || !reader->peek(offset + 8, scratch, size))
        return;
      AVIFrame frame{};
      frame.stream_type = AVIStreamType::AUDIO;
      frame.size = size;
      this->process_audio_frame_(frame, scratch, size);
    }
    this->audio_cursor_pos_ = at + 8 + size + (size & 1);
  }
}

// Reader task: an audio chunk the frame read came to. Fed unless the cursor already fed it.
void SimpleVideoPlayer::reader_audio_chunk_(const AVIFrame &frame, const uint8_t *data, size_t size) {
  const uint64_t end = this->file_reader_->tell();
  if (end <= this->audio_cursor_pos_)
    return;
  this->process_audio_frame_(frame, data, size);
  this->audio_cursor_pos_ = end;
}

// Reader task: a video frame was handed on. A cursor the reader caught up with continues behind it.
void SimpleVideoPlayer::reader_passed_video_() {
  const uint32_t frames = this->reader_frames_.fetch_add(1, std::memory_order_release) + 1;
  const uint64_t end = this->file_reader_->tell();
  if (end > this->audio_cursor_pos_) {
    this->audio_cursor_pos_ = end;
    this->audio_cursor_frames_.store(frames, std::memory_order_release);
  }
}

void SimpleVideoPlayer::audio_task_entry_(void *param) {
  SimpleVideoPlayer *player = static_cast<SimpleVideoPlayer *>(param);
  player->audio_processing_loop_();
}

void SimpleVideoPlayer::audio_processing_loop_() {
  ESP_LOGI(TAG, "Audio processing task started on core %d", xPortGetCoreID());

  // Core 0, prio 1. Never sleeps while playing (only while paused). When there is nothing to
  // push (ring empty, or speaker DMA full) it simply re-checks. The FreeRTOS tick still lets the
  // storage worker and system tasks preempt it; the speaker's own I2S task drains its DMA in
  // parallel. This decouples audio entirely from the Core 1 video decode/pace task -- a multi-ms
  // jpeg_decoder_process() over there no longer stalls the speaker feed.
  // Parked until the playback task releases it at the first video frame (or stop_audio_task_()).
  while (!this->audio_task_stop_ && !this->av_started_.load(std::memory_order_acquire))
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  esp_task_wdt_add(nullptr);

  // A single failed decode drops that chunk and the loop keeps going, instead of tearing audio
  // down on the first hiccup. Only give up on audio entirely after this many consecutive failures
  // (a genuinely broken stream).
  static constexpr uint32_t AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES = 10;
  uint32_t audio_decode_failures = 0;

  while (!this->audio_task_stop_) {
    esp_task_wdt_reset();

    if (!this->audio_enabled_)
      break;

    // Paused: the sound holds where the picture stopped (the reader feeds at most one frame
    // further meanwhile, well within the rings' 250 ms margin), so both go on together on resume.
    // Held by a seek: the feed is restarted at the new position. Neither is the hot path.
    if (this->audio_hold_.load(std::memory_order_acquire) ||
        this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
      this->audio_held_.store(true, std::memory_order_release);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Run audio decoder only when THIS file is compressed (MP3/FLAC). In a multi-codec build
    // audio_decoder_ exists even for a PCM file, so gate on the per-file mode, not the pointer --
    // calling decode() on a decoder that was never start()ed for this file returns FAILED.
    if (this->audio_use_decoder_.load(std::memory_order_acquire)) {
      audio::AudioDecoderState decode_state = this->audio_decoder_->decode(false);

      if (decode_state == audio::AudioDecoderState::FAILED) {
        if (++audio_decode_failures >= AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES) {
          ESP_LOGE(TAG, "Audio decode failed %" PRIu32 " times in a row -- disabling audio", audio_decode_failures);
          this->audio_enabled_ = false;
          break;
        }
        ESP_LOGW(TAG, "Audio decode error -- dropping chunk (%" PRIu32 "/%" PRIu32 ")", audio_decode_failures,
                 AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES);
        continue;
      }
      audio_decode_failures = 0;
    }  // End of if (this->audio_decoder_)

    // Drain the decoded ring to the speaker. Formats match end to end (config == file == speaker),
    // so this is a straight byte copy -- no conversion. speaker_->play() is non-blocking and
    // returns bytes accepted; whatever it can't take this pass we retry next pass -- no wait.
    if (this->audio_decoded_ring_buffer_ && this->speaker_) {
      size_t available = this->audio_decoded_ring_buffer_->available();
      if (available > 0) {
        size_t to_read = std::min(available, AUDIO_TEMP_BUFFER_SIZE);
        size_t bytes_read = this->audio_decoded_ring_buffer_->read(this->audio_temp_buffer_, to_read, 0);

        size_t bytes_remaining = bytes_read;
        const uint8_t *write_ptr = this->audio_temp_buffer_;
        while (bytes_remaining > 0 && !this->audio_task_stop_) {
          size_t written = this->speaker_->play(write_ptr, bytes_remaining);
          if (written > 0) {
            bytes_remaining -= written;
            write_ptr += written;
          } else {
            esp_task_wdt_reset();  // speaker DMA full -- spin, never sleep
          }
        }
      }
    }
  }  // End of while loop

  ESP_LOGI(TAG, "Audio processing task stopped");
  esp_task_wdt_delete(nullptr);
  this->audio_task_handle_ = nullptr;
  vTaskDelete(nullptr);
}

void SimpleVideoPlayer::wait_speaker_running_() {
  // Runs once on the playback task right before the first frame (not the per-frame path). The
  // speaker was started at audio init, so after the buffering this normally returns at once.
  static constexpr uint32_t SPEAKER_START_TIMEOUT_MS = 1000;
  const uint32_t wait_start = millis();
  while (!this->speaker_->is_running() && (millis() - wait_start) < SPEAKER_START_TIMEOUT_MS)
    vTaskDelay(pdMS_TO_TICKS(1));
  if (!this->speaker_->is_running()) {
    ESP_LOGE(TAG, "Speaker failed to start within %" PRIu32 " ms -- playing video-only", SPEAKER_START_TIMEOUT_MS);
    this->speaker_->stop();
    this->audio_enabled_ = false;
  }
}

void SimpleVideoPlayer::stop_audio_task_() {
  if (this->audio_task_handle_ != nullptr) {
    ESP_LOGI(TAG, "Stopping audio processing task...");
    this->audio_task_stop_ = true;
    xTaskNotifyGive(this->audio_task_handle_);

    // Teardown, not the zero-wait path -- a coarse sleep is fine while the audio task exits.
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
