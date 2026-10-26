#include "simple_video_player.h"
#include "emulator.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/components/storage/storage_worker.h"
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <strings.h>

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

//========================================================================
// Component Lifecycle
//========================================================================

SimpleVideoPlayer::~SimpleVideoPlayer() {
  this->stop();
  this->free_buffers_();
}

void SimpleVideoPlayer::setup() {
#if defined(USE_AUDIO) && defined(SVP_AUDIO_SAMPLE_RATE)
  this->wanted_audio_rate_.store(AUDIO_SAMPLE_RATE, std::memory_order_release);
  this->wanted_audio_channels_.store(AUDIO_SOURCE_CHANNELS, std::memory_order_release);
  this->wanted_audio_min_rate_.store(AUDIO_SAMPLE_RATE, std::memory_order_release);
  this->wanted_audio_max_rate_.store(AUDIO_SAMPLE_RATE, std::memory_order_release);
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
  this->session_lock_ = xSemaphoreCreateMutex();
  if (this->media_lock_ == nullptr || this->session_lock_ == nullptr || !this->pool_.init(this->psram_pool_size_)) {
    ESP_LOGE(TAG, "Could not take the %" PRIu32 "-byte PSRAM pool", this->psram_pool_size_);
    this->mark_failed();
    return;
  }
#ifdef SVP_BACKGROUND
  // The background's storage may come after boot (a USB stick mounts later): a file that could
  // not be had is tried again.
  if (storage::global_storage_registry != nullptr)
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) { this->bg_failed_ = false; });
#endif

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

#ifdef SVP_CONTROLS
  this->controls_setup_();  // before plan_pool_(): their share depends on the bar
#endif
  if (!this->plan_pool_()) {
    this->mark_failed();
    return;
  }
  // The lists, at the pool's end for good (plan_pool_()).
  this->listings_ = psram_make<FolderListings>();
  bool lists = this->listings_ && this->playlist_.reserve(this->pool_, MAX_PLAYLIST) &&
               this->playlist_order_.reserve(this->pool_, MAX_PLAYLIST);
#ifdef SVP_AUDIO_FILES
  if (this->file_speaker_ != nullptr)
    lists = lists && this->audio_queue_.reserve(this->pool_, MAX_PLAYLIST) &&
            this->audio_titles_.reserve(this->pool_, MAX_PLAYLIST) &&
            this->audio_order_.reserve(this->pool_, MAX_PLAYLIST);
#endif
  if (!lists) {
    ESP_LOGE(TAG, "No room in the PSRAM pool for the playlist");
    this->mark_failed();
    return;
  }

  // Before loop() runs (which serves its storage calls); its ring comes with the session buffers.
  this->file_reader_ = psram_make<BufferedFileReader>();
  if (!this->file_reader_) {
    ESP_LOGE(TAG, "No PSRAM for the file reader");
    this->mark_failed();
    return;
  }

  this->cache_buffer_ = this->pool_.alloc(CACHE_BUFFER_SIZE);
  if (this->cache_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate cache buffer (%zu bytes)", CACHE_BUFFER_SIZE);
    this->mark_failed();
    return;
  }
#ifdef SVP_AUDIO_COVER
  this->cover_reserve_ = this->cover_bytes_();  // the canvases' sizes; their buffers come with the mode
#endif

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
  // Sound ring + temp buffer at the start of the pool, the ring for no lead (a video with one takes
  // its block anew, sound_block_for_()). init_audio_decoder_() only checks each file against this
  // fixed format.
  if (this->speaker_ != nullptr) {
    this->source_audio_channels_ = AUDIO_SOURCE_CHANNELS;
    this->audio_sample_rate_ = AUDIO_SAMPLE_RATE;
    this->audio_bits_per_sample_ = AUDIO_BITS_PER_SAMPLE;
    this->audio_frame_bytes_ = AUDIO_BYTES_PER_FRAME;

    this->sound_block_len_ = this->sound_bytes_(0);
    this->sound_lead_ms_ = 0;
    if (this->sound_block_len_ != 0) {
      this->sound_block_ = this->pool_.alloc(this->sound_block_len_);
      if (this->sound_block_ != nullptr)
        this->sound_ring_for_(AUDIO_INPUT_RING);
    }
    if (AUDIO_TEMP_BUFFER)
      this->audio_temp_buffer_ = this->pool_.alloc(AUDIO_TEMP_BUFFER_SIZE);
    if ((this->sound_block_len_ != 0 && this->audio_input_ring_buffer_ == nullptr &&
         this->audio_decoded_ring_buffer_ == nullptr) ||
        (AUDIO_TEMP_BUFFER && this->audio_temp_buffer_ == nullptr)) {
      ESP_LOGE(TAG, "No room in the PSRAM pool for the sound buffers");
      this->mark_failed();
      return;
    }

#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC) || defined(SVP_AUDIO_FILES)
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
        psram_make<audio::AudioDecoder>(AUDIO_DECODER_INPUT_BUFFER_SIZE, AUDIO_DECODER_OUTPUT_BUFFER_SIZE,
                                        audio::AudioHeap{pool_object_alloc, pool_object_free});
    if (!this->audio_decoder_) {
      ESP_LOGE(TAG, "No PSRAM for the audio decoder");
      this->mark_failed();
      return;
    }
#endif
#ifdef SVP_AUDIO_FILES
    if (this->file_speaker_ != nullptr) {
      uint8_t *ring = this->pool_.alloc(AUDIO_FILE_RING_SIZE);
      if (ring == nullptr) {
        ESP_LOGE(TAG, "No room in the PSRAM pool for the audio file ring");
        this->mark_failed();
        return;
      }
      this->audio_file_reader_ = psram_make<BufferedFileReader>();
      if (!this->audio_file_reader_) {
        ESP_LOGE(TAG, "No PSRAM for the audio file reader");
        this->mark_failed();
        return;
      }
      this->audio_file_reader_->set_ring(ring, AUDIO_FILE_RING_SIZE);
    }
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

  if (!this->create_tasks_()) {
    ESP_LOGE(TAG, "Failed to create the playback tasks");
    this->mark_failed();
    return;
  }

  // The canvas' buffers are not made here: they come from the pool when a session or a stream
  // needs them (ensure_canvas_outputs_()).

#ifdef SVP_STREAM
  if (this->stream_port_ != 0 && !this->setup_stream_())
    ESP_LOGE(TAG, "Network stream receiver disabled");
#endif
#ifdef SVP_CHANNEL_LIST
  if (!this->channel_url_.empty() && !this->setup_channel_list_())
    ESP_LOGE(TAG, "Channel list disabled");
#endif
#ifdef SVP_PICTURES
  this->pictures_setup_();
  {
    // Pictures and cover art (the picture + audio mode) take the room a video's buffers have in the
    // other modes, handed over on the mode switch: their share must fit in it.
    size_t share = this->pictures_bytes_();
#ifdef SVP_AUDIO_COVER
    share += this->cover_reserve_;
#endif
    const size_t room = this->ring_room_ - this->sound_bytes_(this->max_audio_latency_ms_) +
                        this->frame_bytes_ * FRAME_SLOT_COUNT + this->output_reserve_ + this->controls_reserve_ +
                        this->index_reserve_;
    if (share > room)
      ESP_LOGE(TAG, "psram_pool_size too small: pictures and cover art take %u bytes, the media room is %u",
               static_cast<unsigned>(share), static_cast<unsigned>(room));
  }
#endif
#ifdef SVP_KEYBOARD
  this->keyboard_setup_();
#endif
#ifdef SVP_CAPTURE
  this->capture_setup_();
#endif
}

void SimpleVideoPlayer::loop() {
  if (this->file_reader_)
    this->file_reader_->service();
#ifdef SVP_AUDIO_FILES
  if (this->audio_file_reader_)
    this->audio_file_reader_->service();
#endif
  this->pool_service_();
  this->camera_service_();
#ifdef SVP_EMULATOR
  if (this->emu_ != nullptr)
    this->emu_->service();
#endif
  if (this->finished_pending_.exchange(false, std::memory_order_acq_rel))
    this->on_finished_callbacks_.call();
#ifdef SVP_AUDIO_FILES
  if (this->audio_started_pending_) {
    this->audio_started_pending_ = false;  // an audio file's track started (start_audio_session_())
    this->on_started_callbacks_.call();
  }
#endif
  const int16_t error = this->error_pending_.exchange(-1, std::memory_order_acq_rel);
  if (error >= 0)
    this->on_error_callbacks_.call(static_cast<uint8_t>(error));
  const bool state_changed = this->state_changed_.exchange(false, std::memory_order_acq_rel);
  if (state_changed) {
    this->state_callbacks_.call();
    // The status tells the relay whether a video of the panel's own has the panel ("video").
    if (this->local_video_active() != this->video_shown_) {
      this->video_shown_ = this->local_video_active();
      this->status_callbacks_.call();
    }
  }
#ifdef SVP_AUDIO_FILES
  if (this->audio_changed_.exchange(false, std::memory_order_acq_rel) || state_changed) {
    this->audio_callbacks_.call();
    if (this->audio_playlist_active_() != this->audio_playlist_shown_) {
      this->audio_playlist_shown_ = this->audio_playlist_active_();
      this->status_callbacks_.call();
    }
  }
  if (this->audio_advance_req_.load(std::memory_order_acquire) &&
      (this->audio_remote_.load(std::memory_order_acquire) != AUDIO_REMOTE_NONE ||
       (this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED && !this->session_busy_()))) {
    this->audio_advance_req_.store(false, std::memory_order_relaxed);
    this->audio_advance_();
  }
#endif
  if (this->sound_takeover_req_.exchange(false, std::memory_order_acq_rel))
    this->sound_take_over_();
  if (this->app_end_req_.exchange(false, std::memory_order_acq_rel))
    this->app_end_();  // a video, stream or desktop session started on another task
#ifdef SVP_SPICE
  if (this->spice_sound_claim_.exchange(false, std::memory_order_acq_rel))
    this->spice_sound_take_();
  if (this->spice_changed_.exchange(false, std::memory_order_acq_rel))
    this->spice_callbacks_.call();
#endif
  this->run_pending_start_();
#ifdef SVP_CHANNEL_LIST
  this->channel_loop_();
  this->ha_loop_();
#endif
#ifdef SVP_KEYBOARD
  this->keyboard_loop_();
#endif
#ifdef SVP_PICTURES
  this->pictures_loop_();
#endif
#ifdef SVP_AUDIO_COVER
  this->cover_loop_();
#endif
#ifdef SVP_BACKGROUND
  this->background_loop_();
#endif
#ifdef SVP_CAPTURE
  this->capture_loop_();
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
#ifdef SVP_CONTROLS
    // Still with LVGL: the controls rendered once, so nothing is drawn while the video plays.
    if (this->direct_session_())
      this->prerender_controls_();
#endif
#if defined(SVP_DSI_OUTPUT) && defined(SVP_USE_LVGL)
    // Each item of a playlist decides its output: the display pauses LVGL, a canvas needs it
    // running. Switched only when the item before was in the other mode.
    if (this->lvgl_component_ != nullptr && this->direct_session_() != this->dsi_lvgl_paused_) {
      this->dsi_lvgl_paused_ = this->direct_session_();
      this->lvgl_component_->set_paused(this->dsi_lvgl_paused_, false);
    }
#endif
    this->on_started_callbacks_.call();
    this->start_req_.store(2, std::memory_order_release);
    this->playback_wait_.wake();
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
    // The camera full screen takes the panel as a direct stream does
    const bool cam_direct = this->cam_direct_.load(std::memory_order_acquire);
#ifdef SVP_EMULATOR
    const bool game = this->emu_ != nullptr && this->emu_->wants_panel();
#else
    const bool game = false;
#endif
    const bool panel_taken = streaming || cam_direct || game;
#ifdef SVP_USE_LVGL
    if (panel_taken && this->lvgl_component_ != nullptr && !this->dsi_lvgl_paused_) {
      this->lvgl_component_->set_paused(true, false);
      this->dsi_lvgl_paused_ = true;
    }
    const PlayerState state = this->state_.load(std::memory_order_acquire);
    // Back to LVGL once the playback task is gone (stopped, ended or failed), not between two
    // videos of a playlist. An audio file's session takes no picture: it may have started in the
    // same loop pass the video's task let go (run_pending_start_(), audio_advance_() above).
    bool idle = state == PlayerState::STOPPED || state == PlayerState::ERROR;
#ifdef SVP_AUDIO_FILES
    idle = idle || this->audio_session_.load(std::memory_order_acquire);
#endif
    const bool ended = idle && !this->playback_busy_.load(std::memory_order_acquire);
    if (this->dsi_lvgl_paused_ && !panel_taken && ended && !this->advance_req_.load(std::memory_order_acquire) &&
        !this->start_pending_.load(std::memory_order_acquire)) {
      this->lvgl_component_->set_paused(false, false);
      this->dsi_lvgl_paused_ = false;
    }
    // A paused file: LVGL gets the panel once the playback task is parked (on-device controls),
    // and gives it back here, before the task is let go again, when resume() asks for it.
    if (this->paused_ui_ && state != PlayerState::PAUSED) {
      this->paused_ui_ = false;  // stopped or replaced meanwhile
      this->resume_req_.store(false, std::memory_order_relaxed);
    }
    bool paused_ui = this->dsi_lvgl_paused_ && !panel_taken && state == PlayerState::PAUSED &&
                     !this->is_network_session() && this->playback_parked_.load(std::memory_order_acquire);
#ifdef SVP_CONTROLS
    paused_ui = paused_ui && !this->ctl_ready_.load(std::memory_order_acquire);  // else the controls draw over it
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
      if (this->state_.compare_exchange_strong(expected, PlayerState::PLAYING, std::memory_order_acq_rel)) {
        this->note_state_();
        ESP_LOGI(TAG, "Resuming playback");
#ifdef USE_AUDIO
        this->speaker_pause_(false);
#endif
      }
    }
    const bool lvgl_off = this->lvgl_component_ == nullptr || this->dsi_lvgl_paused_;
#else
    const bool lvgl_off = true;
#endif
    // The camera's frames are drawn once LVGL lets go of the panel
    this->cam_direct_ready_.store(cam_direct && lvgl_off, std::memory_order_release);
#ifdef SVP_EMULATOR
    if (this->emu_ != nullptr && (game && lvgl_off) != this->emu_ready_) {
      this->emu_ready_ = game && lvgl_off;
      this->emu_->panel_ready(this->emu_ready_);
    }
#endif
#if defined(SVP_STREAM) && defined(USE_AUDIO)
    // Stream sound stopped arriving (the speaker runs on).
    if (this->stream_audio_on_.load(std::memory_order_acquire) &&
        millis() - this->stream_last_audio_ms_.load(std::memory_order_acquire) > 500)
      this->stream_audio_on_.store(false, std::memory_order_release);
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
  ESP_LOGCONFIG(TAG, "  Read-ahead ring: %u bytes (prebuffer %" PRIu32 " ms), frame slot %u bytes",
                static_cast<unsigned>(this->ring_size_), this->get_prebuffer(),
                static_cast<unsigned>(this->frame_bytes_));
  ESP_LOGCONFIG(TAG, "  Target FPS: %.1f", this->target_fps_);

  if (this->state_ != PlayerState::STOPPED) {
    ESP_LOGCONFIG(TAG, "  Current file: %s", this->video_path_.c_str());
    ESP_LOGCONFIG(TAG, "  Video size: %" PRIu32 "x%" PRIu32, this->video_width_, this->video_height_);
  }
#ifdef SVP_CAPTURE
  ESP_LOGCONFIG(TAG,
                "  Screen capture: screenshots %s, screencasts %s, JPEG quality %u, %u fps, target %s, sound %s",
                this->shot_dir_.c_str(), this->cast_dir_.c_str(), this->get_capture_quality(), this->cast_fps_,
                this->cast_target_ == CAST_LOCAL ? "folder" : this->cast_target_ == CAST_RELAY ? "svp_relay" : "both",
                this->cast_sound_ ? "on" : "off");
#endif
}

//========================================================================
// Playback Control API
//========================================================================

void SimpleVideoPlayer::play(const std::string &video_path, bool folder_playlist) {
#ifdef SVP_SPICE
  if (this->spice_refuses_())
    return;
#endif
#ifdef SVP_EMULATOR
  if (this->emu_refuses_())
    return;
#endif
  this->sound_take_over_();  // a video plays alone
  this->app_end_();          // and without pictures or the camera
  this->start_path_ = video_path;
  this->start_folder_ = folder_playlist;
  this->start_pending_.store(true, std::memory_order_release);
#ifdef SVP_AUDIO_FILES
  this->audio_start_pending_.store(false, std::memory_order_release);
#endif
  this->end_session_();
  this->run_pending_start_();
}

void SimpleVideoPlayer::play_list(std::vector<std::string> paths, size_t start) {
  if (paths.empty())
    return;
  if (this->listings_->playlist.active) {
    // The worker writes into playlist_ past its entry till then.
    ESP_LOGW(TAG, "Playlist not taken: a folder is being listed for the last one");
    return;
  }
  this->playlist_gen_++;
  this->playlist_.clear();
  for (const auto &path : paths) {
    if (this->playlist_.full())
      break;
    if (!SvpPath::fits(path.size())) {
      ESP_LOGW(TAG, "%s left out of the playlist: its path is too long", path.c_str());
      continue;
    }
    this->playlist_.emplace_back(path.c_str());
  }
  if (this->playlist_.empty())
    return;
  this->play_index_(std::min(start, this->playlist_.size() - 1));
}

void SimpleVideoPlayer::play_index_(size_t index) {
#ifdef SVP_SPICE
  if (this->spice_refuses_())
    return;
#endif
#ifdef SVP_EMULATOR
  if (this->emu_refuses_())
    return;
#endif
  this->start_path_.clear();
  this->start_index_ = index;
  this->start_pending_.store(true, std::memory_order_release);
#ifdef SVP_AUDIO_FILES
  this->audio_start_pending_.store(false, std::memory_order_release);
#endif
  this->end_session_();
  this->run_pending_start_();
}

bool SimpleVideoPlayer::session_busy_() const {
#ifdef USE_AUDIO
  if (this->audio_busy_.load(std::memory_order_acquire))
    return true;
#endif
  return this->playback_busy_.load(std::memory_order_acquire) || this->reader_busy_.load(std::memory_order_acquire);
}

void SimpleVideoPlayer::run_pending_start_() {
#ifdef SVP_AUDIO_FILES
  const bool audio = this->audio_start_pending_.load(std::memory_order_acquire);
#else
  const bool audio = false;
#endif
  if (!this->start_pending_.load(std::memory_order_acquire) && !audio)
    return;
  // What started meanwhile (the network stream) ends too: what was asked for last plays.
  const PlayerState now = this->state_.load(std::memory_order_acquire);
  if (now == PlayerState::PLAYING || now == PlayerState::PAUSED)
    this->end_session_();
  if (this->session_busy_())
    return;
  if (xSemaphoreTake(this->session_lock_, 0) != pdTRUE)
    return;  // the network task is starting a stream: next loop
  // A desktop session that took the panel meanwhile cleared start_pending_ (desktop_take_panel_()).
  if (!audio && !this->start_pending_.load(std::memory_order_acquire)) {
    xSemaphoreGive(this->session_lock_);
    return;
  }
  if (this->session_busy_() || this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING ||
      this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
    xSemaphoreGive(this->session_lock_);
    return;
  }
#ifdef SVP_AUDIO_FILES
  if (audio) {
    this->audio_start_pending_.store(false, std::memory_order_release);
    this->start_audio_session_();
    xSemaphoreGive(this->session_lock_);
    return;
  }
#endif
  this->start_pending_.store(false, std::memory_order_release);
  size_t index = this->start_index_;
  if (!this->start_path_.empty()) {
    // The file first; its folder's videos put round it once listed (at once without the worker).
    this->playlist_.clear();
    this->playlist_index_ = 0;
    this->playlist_gen_++;
    if (SvpPath::fits(this->start_path_.size())) {
      this->playlist_.emplace_back(this->start_path_.c_str());
    } else {
      ESP_LOGW(TAG, "%s: its path is too long", this->start_path_.c_str());
    }
    if (this->start_folder_ && !this->playlist_.empty())
      this->load_folder_playlist_(this->start_path_);
    index = this->playlist_index_;
    this->start_path_.clear();
  }
  if (index < this->playlist_.size()) {
    this->playlist_index_ = index;
    this->advance_req_.store(false, std::memory_order_relaxed);
#ifdef SVP_STREAM
    this->net_session_ = false;
#endif
    this->start_playback_(this->playlist_[index].c_str());
  }
  xSemaphoreGive(this->session_lock_);
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
  FolderListing &job = this->listings_->playlist;
  if (job.active) {
    this->playlist_list_wanted_ = true;  // after the one on its way (whose result is dropped)
    return;
  }
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0 || storage::global_storage_registry == nullptr)
    return;
  const std::string dir = path.substr(0, slash);
  const char *rel = nullptr;
  storage::PathStorage *storage = storage::global_storage_registry->resolve_path(dir.c_str(), &rel);
  if (storage == nullptr) {
    ESP_LOGW(TAG, "Cannot list %s for the playlist", dir.c_str());
    return;
  }
  const char *at = (rel == nullptr || rel[0] == '\0') ? "/" : rel;
  // The names into playlist_'s places after its entry (one place left for that entry to go back).
  job.player = this;
  job.dir = dir;
  job.first = 1;
  job.count = 0;
  job.gen = this->playlist_gen_;
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (worker != nullptr &&
      worker->async_list_dir(storage, at, &SimpleVideoPlayer::playlist_entry_, &job,
                             [this](storage::StorageError err) { this->playlist_listed_(err); }) ==
          storage::StorageError::STORAGE_ERROR_OK) {
    job.active = true;
    return;
  }
  // No worker, or its queue full: read here, unless the worker reads from it on its task.
  if (worker != nullptr && worker->has_active_task_io(storage)) {
    ESP_LOGW(TAG, "Cannot list %s for the playlist", dir.c_str());
    return;
  }
  job.active = true;
  this->playlist_listed_(storage->list_dir(at, &SimpleVideoPlayer::playlist_entry_, &job));
}

// The worker's task (or here): one entry of the folder, a video's name into playlist_'s next place.
bool SimpleVideoPlayer::playlist_entry_(const storage::FileStat *entry, void *ctx) {
  auto *job = static_cast<FolderListing *>(ctx);
  PoolList<SvpPath> &list = job->player->playlist_;
  if (entry->is_dir || entry->name[0] == '.')
    return true;
  const char *dot = strrchr(entry->name, '.');
  if (dot == nullptr)
    return true;
  if (strcasecmp(dot, ".avi") != 0 && strcasecmp(dot, ".mjpeg") != 0 && strcasecmp(dot, ".mjpg") != 0)
    return true;
  if (!SvpPath::fits(job->dir.size() + 1 + std::strlen(entry->name))) {
    ESP_LOGW(TAG, "%s left out of the playlist: its path is too long", entry->name);
    return true;
  }
  SvpPath *at = list.place(job->first + job->count);
  if (at == nullptr)
    return false;
  ::new (at) SvpPath(entry->name);
  job->count++;
  return job->first + job->count < list.capacity();
}

// Main loop: the folder listed; the playlist becomes its videos, sorted, the one playing among them
// (else first).
void SimpleVideoPlayer::playlist_listed_(storage::StorageError err) {
  FolderListing &job = this->listings_->playlist;
  job.active = false;
  const bool wanted = this->playlist_list_wanted_;
  this->playlist_list_wanted_ = false;
  if (job.gen != this->playlist_gen_ || this->playlist_.size() != 1) {
    // Another playlist meanwhile: dropped; its folder now, if it asked for one.
    if (wanted && this->playlist_.size() == 1)
      this->load_folder_playlist_(this->playlist_[0].c_str());
    return;
  }
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    ESP_LOGW(TAG, "Listing %s failed: %s", job.dir.c_str(), storage::error_to_string(err));
    return;
  }
  const SvpPath file = this->playlist_[0];
  this->playlist_.adopt(1 + job.count);
  this->playlist_.erase(this->playlist_.begin());
  sort_by_order(this->playlist_, this->playlist_order_,
                [](const SvpPath &a, const SvpPath &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
  const size_t slash = file.rfind('/');
  const char *name = file.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  size_t found = SIZE_MAX;
  for (size_t i = 0; i < this->playlist_.size(); i++) {
    SvpPath &entry = this->playlist_[i];
    if (found == SIZE_MAX && entry == name)
      found = i;
    SvpPath full(job.dir.c_str());
    full += '/';
    full += entry.c_str();
    entry = full;
  }
  if (found == SIZE_MAX) {
    // The file itself is not among them (another extension, hidden): it first, then the folder.
    this->playlist_.insert(this->playlist_.begin(), file);
    found = 0;
  }
  this->playlist_index_ = found;
  ESP_LOGI(TAG, "Playlist: %u videos in %s", static_cast<unsigned>(this->playlist_.size()), job.dir.c_str());
}

void SimpleVideoPlayer::start_playback_(const std::string &video_path) {
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Cannot play: component failed to initialize (see setup logs)");
    return;
  }
  if (this->session_busy_()) {
    ESP_LOGE(TAG, "Not starting %s: the last session still ends", video_path.c_str());
    return;
  }
  ESP_LOGI(TAG, "Playing video: %s", video_path.c_str());

  // video_path_ and the rest are written before playback_busy_ (release) and the notification: the
  // playback task reads them after taking it.
  this->video_path_ = video_path;
  this->last_error_.store(PlaybackError::NONE, std::memory_order_relaxed);
#ifdef SVP_AUDIO_FILES
  this->audio_session_.store(false, std::memory_order_release);
  // Any session ends the queue's Home Assistant track (svp_relay stops its sound for one of its own).
  this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);
#endif
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
  this->note_state_();
  this->playback_busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->task_handle_);
}

PlayerState SimpleVideoPlayer::get_state() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  // The queue's Home Assistant track while no session of the player's runs: the media player's state.
  if (state == PlayerState::STOPPED && this->media_player_ != nullptr &&
      this->audio_remote_.load(std::memory_order_acquire) != AUDIO_REMOTE_NONE) {
    if (this->media_player_->state == media_player::MEDIA_PLAYER_STATE_PLAYING)
      return PlayerState::PLAYING;
    if (this->media_player_->state == media_player::MEDIA_PLAYER_STATE_PAUSED)
      return PlayerState::PAUSED;
    return PlayerState::STOPPED;
  }
#endif
  return state;
}

void SimpleVideoPlayer::pause() {
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  if (this->audio_remote_command_(media_player::MEDIA_PLAYER_COMMAND_PAUSE))
    return;
#endif
  PlayerState expected = PlayerState::PLAYING;
  if (this->state_.compare_exchange_strong(expected, PlayerState::PAUSED, std::memory_order_acq_rel)) {
    this->note_state_();
    ESP_LOGI(TAG, "Pausing playback");
#ifdef USE_AUDIO
    this->speaker_pause_(true);
#endif
    this->on_paused_callbacks_.call();
  }
}

void SimpleVideoPlayer::resume() {
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  if (this->audio_remote_command_(media_player::MEDIA_PLAYER_COMMAND_PLAY))
    return;
#endif
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
    this->note_state_();
    ESP_LOGI(TAG, "Resuming playback");
#ifdef USE_AUDIO
    this->speaker_pause_(false);
#endif
  }
}

void SimpleVideoPlayer::stop() {
#ifdef SVP_AUDIO_FILES
  // A track asked for while the last session still ends does not start after the stop either.
  const bool audio_pending = this->audio_start_pending_.exchange(false, std::memory_order_acq_rel);
  if (this->is_audio_session() || audio_pending) {
    this->audio_halted_ = true;
    this->audio_changed_.store(true, std::memory_order_release);
  }
#endif
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  if (this->audio_remote_command_(media_player::MEDIA_PLAYER_COMMAND_STOP)) {
    this->start_pending_.store(false, std::memory_order_release);
    return;  // the queue's Home Assistant track: a session of svp_relay's beside it goes on
  }
#endif
  this->start_pending_.store(false, std::memory_order_release);
  this->end_session_();
}

void SimpleVideoPlayer::app_end_() {
  const AppKind was = this->app_;
  if (was == AppKind::NONE)
    return;
  this->app_ = AppKind::NONE;
  if (was == AppKind::PICTURES)
    this->close_picture();
#ifdef SVP_EMULATOR
  if (was == AppKind::GAME && this->emu_ != nullptr)
    this->emu_->app_ended();
#endif
  this->app_end_callbacks_.call(was);
}

#ifdef SVP_EMULATOR
bool SimpleVideoPlayer::emu_refuses_() const {
  if (!this->emu_on_.load(std::memory_order_acquire))
    return false;
  ESP_LOGW(TAG, "Refused: a game holds the panel");
  return true;
}

GameEmulator *SimpleVideoPlayer::make_emulator() {
  this->emu_ = GameEmulator::create(this);
  return this->emu_;
}

void SimpleVideoPlayer::paused_input_default_() {
#if defined(USE_LVGL_HID) && defined(SVP_USE_LVGL)
  if (this->lvgl_component_ == nullptr)
    return;
#ifdef SVP_STREAM_INPUT
  if (this->stream_input_q_ != nullptr) {
    this->lvgl_component_->set_paused_input(&this->stream_input_taker_);
    return;
  }
#endif
  this->lvgl_component_->set_paused_input(nullptr);
#endif
}
#endif

bool SimpleVideoPlayer::app_take(AppKind app, size_t need_bytes) {
#ifdef SVP_EMULATOR
  if (app != AppKind::GAME && this->emu_refuses_())
    return false;
#endif
#ifdef SVP_SPICE
  if (this->is_spice_connected())
    this->spice_disconnect();  // a desktop session ends
#endif
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  const bool video = (state == PlayerState::PLAYING || state == PlayerState::PAUSED) && !this->is_audio_session();
  if (video)
    this->stop();  // a video ends (an audio file plays on)
  if (this->app_ != app)
    this->app_end_();
  this->app_ = app;
#ifdef SVP_AUDIO_FILES
  // The camera beside the audio only if it fits: PSRAM's largest free block against what it takes
  if (app == AppKind::CAMERA && need_bytes != 0 && this->is_audio_session() &&
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < need_bytes) {
    ESP_LOGI(TAG, "Audio stopped: PSRAM has no room for the camera beside it");
    this->stop();
  }
#endif
  return true;
}

void SimpleVideoPlayer::app_release(AppKind app) {
  if (this->app_ == app)
    this->app_ = AppKind::NONE;
}

void SimpleVideoPlayer::sound_take_over_() {
#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  this->audio_remote_command_(media_player::MEDIA_PLAYER_COMMAND_STOP);  // the queue's track
#endif
#ifdef SVP_MEDIA_PLAYER
  if (this->media_player_ != nullptr && (this->media_player_->state == media_player::MEDIA_PLAYER_STATE_PLAYING ||
                                         this->media_player_->state == media_player::MEDIA_PLAYER_STATE_PAUSED))
    this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_STOP).perform();
#endif
}

void SimpleVideoPlayer::end_session_() {
  this->advance_req_.store(false, std::memory_order_relaxed);
#ifdef SVP_AUDIO_FILES
  this->audio_advance_req_.store(false, std::memory_order_relaxed);
  if (this->audio_session_.load(std::memory_order_acquire))
    this->reader_task_stop_ = true;  // a read waiting for the storage returns
#endif
  PlayerState prev = this->state_.exchange(PlayerState::STOPPED, std::memory_order_acq_rel);
  if (prev != PlayerState::STOPPED)
    this->note_state_();
  if (prev == PlayerState::PLAYING || prev == PlayerState::PAUSED) {
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
    ESP_LOGW(TAG, "Seeking needs a local AVI or audio file of known length playing or paused");
    return;
  }
#ifdef SVP_AUDIO_FILES
  if (this->audio_session_.load(std::memory_order_acquire)) {
    const uint32_t duration = this->duration_frames_.load(std::memory_order_acquire);  // ms
    const float ms = std::max(seconds, 0.0f) * 1000.0f;
    this->audio_seek_(ms >= static_cast<float>(duration) ? duration - 1 : static_cast<uint32_t>(ms));
    return;
  }
#endif
  const uint32_t frames = this->duration_frames_.load(std::memory_order_acquire);
  const float frame = std::max(seconds, 0.0f) * 1e6f / this->frame_duration_us_;
  const uint32_t target = frame >= static_cast<float>(frames - 1) ? frames - 1 : static_cast<uint32_t>(frame);
  ESP_LOGI(TAG, "Seeking to %.1f s", target * this->frame_duration_us_ / 1e6f);
  this->seek_target_.store(target, std::memory_order_relaxed);
  this->seek_pending_.store(true, std::memory_order_release);
  this->playback_wait_.wake();
}

//========================================================================
// Playback Task
//========================================================================

// Decode/playback on Core 1, alongside ESPHome's main loop task (which drives App.loop() ->
// LvglComponent::loop() -> lv_timer_handler(), pinned there via esphome/components/esp32/core.cpp's
// xTaskCreateStaticPinnedToCore(..., 1)). The ESP32-P4 hardware JPEG decoder and LVGL's PPA rotation
// both drive DMA2D; on one core they never touch it at the same instant (espressif/esp-idf#18999,
// "DMA2D dma2d_connect hangs indefinitely on ESP32-P4 ... under continuous PPA load"). Priority 1 ==
// loopTask: the tick round-robins the two. The reader (Core 0) never drives DMA2D; the sound (Core 0)
// neither.
bool SimpleVideoPlayer::create_tasks_() {
  if (!create_psram_task(playback_task_entry_, "video_player", 8192, this, 1, &this->task_handle_, 0) ||
      !create_psram_task(reader_task_entry_, "svp_reader", 4096, this, 2, &this->reader_task_handle_, 0))
    return false;
  this->playback_wait_.task = this->task_handle_;
  this->reader_wait_.task = this->reader_task_handle_;
  esp_timer_create_args_t pace{};
  pace.callback = pace_timer_cb_;
  pace.arg = this;
  pace.dispatch_method = ESP_TIMER_TASK;
  pace.name = "svp_pace";
  if (esp_timer_create(&pace, &this->pace_timer_) != ESP_OK)
    return false;
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    if (!create_psram_task(audio_task_entry_, "svp_audio", 4096, this, 1, &this->audio_task_handle_, 0))
      return false;
    this->audio_wait_.task = this->audio_task_handle_;
  }
#endif
  return true;
}

void SimpleVideoPlayer::note_state_() {
  this->state_changed_.store(true, std::memory_order_release);
  this->playback_wait_.wake();
  this->reader_wait_.wake();
  this->audio_wait_.wake();
#ifdef SVP_STREAM
  this->stream_wake_();
#endif
}

void SimpleVideoPlayer::return_slot_(FrameSlot *slot) {
  xQueueSend(this->empty_queue_, &slot, 0);
  this->reader_wait_.wake();
}

void SimpleVideoPlayer::playback_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!player->playback_busy_.load(std::memory_order_acquire))
      continue;
    player->playback_loop_();
    // Also after an early return. The last thing of a session: from here a new one may start.
    player->playback_busy_.store(false, std::memory_order_release);
#ifdef SVP_STREAM
    player->stream_wake_();
#endif
#ifdef SVP_SPICE
    if (player->spice_on_.load(std::memory_order_acquire))
      player->spice_wake();  // a desktop taking the panel waits for this (desktop_take_panel_())
#endif
  }
}

void SimpleVideoPlayer::reader_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!player->reader_busy_.load(std::memory_order_acquire))
      continue;
#ifdef SVP_AUDIO_FILES
    if (player->audio_session_.load(std::memory_order_acquire)) {
      player->audio_file_feed_();
    } else {
      player->reader_loop_();
    }
#else
    player->reader_loop_();
#endif
    player->reader_busy_.store(false, std::memory_order_release);
    player->playback_wait_.wake();
#ifdef SVP_STREAM
    player->stream_wake_();
#endif
#ifdef SVP_SPICE
    if (player->spice_on_.load(std::memory_order_acquire))
      player->spice_wake();
#endif
  }
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
      this->playback_wait_.wake();
    }
    // Woken by a stop, a seek, a slot given back and each chunk the file reader fills (the sound
    // cursor follows the ring).
    auto woken = [this]() {
      const PlayerState state = this->state_.load(std::memory_order_acquire);
      return this->reader_task_stop_ || this->reader_seek_req_.load(std::memory_order_acquire) ||
             (state != PlayerState::PLAYING && state != PlayerState::PAUSED);
    };
    if (at_end) {
      // EOF/error sentinel delivered: nothing left to produce, unless a seek comes before the end.
      this->reader_wait_.wait(woken, portMAX_DELAY);
      continue;
    }

#ifdef USE_AUDIO
    this->advance_audio_cursor_();
#endif
    FrameSlot *slot = nullptr;
    if (xQueueReceive(this->empty_queue_, &slot, 0) != pdTRUE) {
      this->reader_wait_.waiting.store(true, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      if (!woken() && uxQueueMessagesWaiting(this->empty_queue_) == 0)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      this->reader_wait_.waiting.store(false, std::memory_order_relaxed);
      continue;
    }

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
    this->playback_wait_.wake();
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
  if (direct) {
    this->direct_frame_ = nullptr;  // nothing of this session decoded yet
    output_ready = this->ensure_direct_target_(!this->direct_flips_());
  }
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

  // The output taken above (canvas pair or the display's decode target): does this video fit it?
  if (!this->allocate_buffers_(width, height)) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    this->set_error_(PlaybackError::BUFFER_ALLOCATION_FAILED);
    this->close_file_();
#ifdef USE_AUDIO
    this->end_session_audio_();  // its task waits for a first frame that never comes
#endif
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

  // On the task watchdog for the session (not before: the early-return error paths above leave
  // nothing subscribed).
  this->set_playback_wdt_(true);

  // Load is done (headers, dimensions, audio init all read their bytes). The read-ahead ring is
  // filled before the first frame read.
  if (this->file_reader_)
    this->file_reader_->prefill_cache();

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

  // The reader/producer on Core 0 (decode stays on Core 1, see create_tasks_()).
  this->reader_task_stop_ = false;
  this->expected_gen_ = this->frame_gen_.load(std::memory_order_acquire);
  this->reader_busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->reader_task_handle_);

  // Deep pre-buffer: wait (bounded) until the reader has filled the slot queue before presenting
  // the first frame, so decode starts from a full compressed buffer. Not the hot path.
  auto playing = [this]() { return this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING; };
  {
    const uint32_t start = millis();
    this->playback_wait_for_(
        [&]() {
          return uxQueueMessagesWaiting(this->filled_queue_) >= FRAME_SLOT_COUNT || !playing() ||
                 millis() - start >= 2000;
        });
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
  App.wake_loop_threadsafe();
  {
    const uint32_t start = millis();
    this->playback_wait_for_(
        [&]() {
          return this->start_req_.load(std::memory_order_acquire) == 2 || !playing() || millis() - start >= 2000;
        });
  }
#ifdef USE_AUDIO
  // The audio cursor reads ahead while the reader waits for the slot; the sound is released once it
  // is audio_lead_frames_ ahead (bounded: a short file or a slow stream starts with what there is).
  {
    const uint32_t start = millis();
    this->playback_wait_for_(
        [&]() {
          return this->audio_cursor_frames_.load(std::memory_order_acquire) >=
                     this->reader_frames_.load(std::memory_order_acquire) + this->audio_lead_frames_ ||
                 !playing() || millis() - start >= 1000;
        });
  }
#endif
  this->av_started_.store(true, std::memory_order_release);
  this->audio_wait_.wake();

  // One state load per iteration. Anything but PLAYING/PAUSED (STOPPED, ERROR) ends the loop.
  while (true) {
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
      // Blocked until resume, stop, a seek or (controls) a repaint or the frame after a seek; off the
      // task watchdog meanwhile, a pause may last.
      this->set_playback_wdt_(false);
      while (this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
        if (this->seek_pending_.load(std::memory_order_acquire)) {
          this->handle_seek_();
#ifdef SVP_CONTROLS
          this->ctl_step_.store(direct && this->ctl_ready_.load(std::memory_order_acquire), std::memory_order_relaxed);
#endif
        }
#ifdef SVP_CONTROLS
        if (direct && this->ctl_ready_.load(std::memory_order_acquire)) {
          // After a seek the new picture shows, with the controls over it.
          FrameSlot *slot = nullptr;
          if (this->ctl_step_.load(std::memory_order_relaxed) &&
              xQueueReceive(this->filled_queue_, &slot, 0) == pdTRUE) {
            if (slot->gen == this->expected_gen_ && slot->size > 0) {
              this->ctl_step_.store(false, std::memory_order_relaxed);
              this->video_frame_index_++;
              this->position_frame_.store(slot->file_frame, std::memory_order_relaxed);
              const bool decoded = this->decode_frame_(slot->data, static_cast<size_t>(slot->size));
              this->return_slot_(slot);
              if (decoded)
                this->ctl_repaint_.store(true, std::memory_order_release);
            } else if (slot->gen != this->expected_gen_) {
              this->return_slot_(slot);
            } else {
              this->ctl_step_.store(false, std::memory_order_relaxed);
              xQueueSendToFront(this->filled_queue_, &slot, 0);  // end of file: the loop takes it
            }
          }
          if (this->ctl_repaint_.exchange(false, std::memory_order_acq_rel))
            this->repaint_paused_();
        }
#endif
        this->playback_wait_.wait(
            [&]() {
              if (this->state_.load(std::memory_order_acquire) != PlayerState::PAUSED ||
                  this->seek_pending_.load(std::memory_order_acquire))
                return true;
#ifdef SVP_CONTROLS
              if (direct && this->ctl_ready_.load(std::memory_order_acquire) &&
                  (this->ctl_repaint_.load(std::memory_order_acquire) ||
                   (this->ctl_step_.load(std::memory_order_relaxed) && uxQueueMessagesWaiting(this->filled_queue_) > 0)))
                return true;
#endif
              return false;
            },
            portMAX_DELAY);
      }
      this->set_playback_wdt_(true);
      this->playback_parked_.store(false, std::memory_order_release);
      this->paused_accum_us_ += esp_timer_get_time() - pause_started_us;
      continue;
    }

    // The next pre-buffered compressed frame from the reader (Core 0); in steady state it is there.
    FrameSlot *slot = nullptr;
    if (xQueueReceive(this->filled_queue_, &slot, 0) != pdTRUE) {
      this->playback_wait_for_(
          [this]() {
            return uxQueueMessagesWaiting(this->filled_queue_) > 0 ||
                   this->state_.load(std::memory_order_acquire) != PlayerState::PLAYING ||
                   this->seek_pending_.load(std::memory_order_acquire);
          });
      continue;
    }
    if (slot->gen != this->expected_gen_) {  // read before a seek
      this->return_slot_(slot);
      continue;
    }
    const int payload = slot->size;
    if (payload == 0) {  // EOF sentinel
      this->return_slot_(slot);
#ifdef SVP_STREAM
      if (this->net_session_) {
        this->stream_played_to_end_.store(true, std::memory_order_release);
      }
#endif
      ESP_LOGI(TAG, "Playback finished");
#ifdef SVP_STREAM
      if (!this->net_session_)
#endif
        this->advance_req_.store(true, std::memory_order_release);
      this->finished_pending_.store(true, std::memory_order_release);  // automations run from loop()
      break;
    }
    const bool repeat = payload == AVIParser::REPEAT_FRAME;  // the previous picture stays one more frame
    if (payload < 0 && !repeat) {  // read-error sentinel
      this->return_slot_(slot);
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

    // Blocked until the frame's presentation time (pace_timer_ wakes the task then). A pause, a seek
    // or a stop wakes it earlier: the frame goes back to the front of the queue, nothing is dropped.
    const int64_t early_us = target_present_time_us - esp_timer_get_time();
    if (early_us > 0) {
      esp_timer_start_once(this->pace_timer_, static_cast<uint64_t>(early_us));
      this->playback_wait_for_([&]() {
        return esp_timer_get_time() >= target_present_time_us ||
               this->state_.load(std::memory_order_acquire) != PlayerState::PLAYING ||
               this->seek_pending_.load(std::memory_order_acquire);
      });
      esp_timer_stop(this->pace_timer_);
      if (esp_timer_get_time() < target_present_time_us) {
        xQueueSendToFront(this->filled_queue_, &slot, 0);
        this->video_frame_index_--;
        continue;
      }
    }

    if (repeat) {
      this->return_slot_(slot);
      esp_task_wdt_reset();
      continue;
    }

    // decode_frame_() consumes slot->data (compressed) into the decode target. Once it returns, the
    // compressed bytes are no longer needed -- what is shown is drawn from the decoded buffer, not
    // slot->data -- so the slot is returned to the reader right after.
#ifdef SVP_STREAM_STATS
    const int64_t decode_start = esp_timer_get_time();
#endif
    const bool decoded = this->decode_frame_(slot->data, static_cast<size_t>(payload));
#ifdef SVP_STREAM_STATS
    if (decoded) {
      this->stream_stats_.frames.fetch_add(1, std::memory_order_relaxed);
      this->stream_stats_.frame_us.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - decode_start),
                                             std::memory_order_relaxed);
    }
#endif
    // decode_frame_() wrote the frame into the back buffer and published it; loop() points the canvas at it.

#ifdef SVP_DSI_OUTPUT
    if (decoded && direct) {
      // The frame is in direct_frame_ (direct_target_()): the controls over it, then shown. Panel
      // dims are multiples of 16 (init_dsi_output_), so no x_pad.
#ifdef SVP_CONTROLS
      this->composite_controls_();
#endif
      this->direct_show_(this->direct_frame_, 0, 0, this->dsi_out_w_, this->dsi_out_h_, 0);
    }
#endif

    this->return_slot_(slot);

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
  this->reader_wait_.wake();
  {
    const uint32_t start = millis();
    this->playback_wait_for_(
        [&]() { return !this->reader_busy_.load(std::memory_order_acquire) || millis() - start >= 2000; });
  }

  // Release any in-flight BufferedFileReader wait before close_file_() tears the reader down.
  this->close_file_();
  // The session's buffers are not freed here: they stay for the next playback, until another user
  // lacks room and the owner table hands them back (pool_take_()).

#ifdef USE_AUDIO
  this->end_session_audio_();
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
  this->note_state_();

  this->set_playback_wdt_(false);
  ESP_LOGI(TAG, "Playback task finished");
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
        int bytes_read = this->read_data_(this->cache_buffer_, CACHE_BUFFER_SIZE);

        if (bytes_read <= 0) {
          // EOF or error
          return bytes_read;
        }

        this->cache_buffer_valid_ = bytes_read;
        this->cache_buffer_offset_ = 0;
      }

      // Search for EOI marker in cache
      size_t search_len = this->cache_buffer_valid_ - this->cache_buffer_offset_;
      uint8_t *search_start = this->cache_buffer_ + this->cache_buffer_offset_;
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
    // Direct mode: in the display's depth (color_depth from the YAML), where direct_target_() says.
    // BGR element order for BOTH depths: it puts the lowest-addressed byte at blue, which is the
    // little-endian layout esp_lcd's RGB565 and RGB888 framebuffers both read.
    size_t len = 0;
    out_buf = this->direct_target_(true, &len);
    out_cap = static_cast<uint32_t>(len);
    if (out_buf == nullptr)
      return false;
    this->direct_frame_ = out_buf;
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
    int bytes_read = this->read_data_(this->cache_buffer_, CACHE_BUFFER_SIZE);
    if (bytes_read <= 0) {
      return false;
    }

    this->cache_buffer_valid_ = bytes_read;
    this->cache_buffer_offset_ = 0;

    // Parse JPEG header
    if (!this->parse_header_(this->cache_buffer_, static_cast<size_t>(bytes_read), width, height)) {
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
      this->avi_parser_ = psram_make<AVIParser>();
      if (!this->avi_parser_) {
        ESP_LOGE(TAG, "No PSRAM for the AVI parser");
        this->close_file_();
        return false;
      }
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
    // The index now: seeking, and damage passed over to the next intact frame. Its memory only for a
    // file's AVI (a network AVI and raw MJPEG have none), kept with the ring once taken.
    if (this->index_buf_ == nullptr) {
      uint8_t *index = this->media_alloc_(this->index_reserve_);
      xSemaphoreTake(this->media_lock_, portMAX_DELAY);
      this->index_buf_ = reinterpret_cast<uint32_t *>(index);
      xSemaphoreGive(this->media_lock_);
      this->avi_parser_->set_index_memory(this->index_buf_, this->index_reserve_);
    }
    this->avi_parser_->load_index(this->cache_buffer_, CACHE_BUFFER_SIZE);
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
  this->stream_wake_();
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
  this->stream_wake_();  // the network task waits for the sink before it reads the socket again
  this->file_size_ = 0;
  this->video_format_ = VideoFormat::AVI_MJPEG;
  if (!this->avi_parser_) {
    this->avi_parser_ = psram_make<AVIParser>();
    if (!this->avi_parser_) {
      ESP_LOGE(TAG, "No PSRAM for the AVI parser");
      this->close_file_();
      return false;
    }
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
  auto ending = [this]() {
    const PlayerState st = this->state_.load(std::memory_order_acquire);
    return st != PlayerState::PLAYING && st != PlayerState::PAUSED;
  };
#ifdef USE_AUDIO
  const bool audio = this->audio_enabled_ && this->audio_busy_.load(std::memory_order_acquire);
  if (audio) {
    // The reader restarts the decoder and the rings: only once the audio task has let go of them.
    this->audio_held_.store(false, std::memory_order_release);
    this->audio_hold_.store(true, std::memory_order_release);
    this->audio_wait_.wake();
    this->playback_wait_for_(
        [&]() {
          return this->audio_held_.load(std::memory_order_acquire) ||
                 !this->audio_busy_.load(std::memory_order_acquire) || ending();
        });
    if (ending()) {
      this->audio_hold_.store(false, std::memory_order_release);
      this->audio_wait_.wake();
      return;  // playback ends: the seek is dropped
    }
  }
#endif
  this->reader_seek_frame_.store(target, std::memory_order_relaxed);
  this->reader_seek_req_.store(true, std::memory_order_release);
  this->reader_wait_.wake();
  this->playback_wait_for_(
      [&]() {
        return !this->reader_seek_req_.load(std::memory_order_acquire) ||
               !this->reader_busy_.load(std::memory_order_acquire) || ending();
      });
  const bool ok = !this->reader_seek_req_.exchange(false, std::memory_order_acq_rel) &&
                  this->reader_seek_ok_.load(std::memory_order_relaxed);
  this->expected_gen_ = this->frame_gen_.load(std::memory_order_acquire);
#ifdef USE_AUDIO
  if (audio) {
    if (ok) {
      // The speaker keeps running; the sound from the new place goes on audio_lead_frames_ ahead
      // of the picture, as at the start of playback.
      const uint32_t start = millis();
      this->playback_wait_for_(
          [&]() {
            return this->audio_cursor_frames_.load(std::memory_order_acquire) >=
                       this->reader_frames_.load(std::memory_order_acquire) + this->audio_lead_frames_ ||
                   ending() || millis() - start >= 1000;
          });
    }
    this->audio_hold_.store(false, std::memory_order_release);
    this->audio_wait_.wake();
  }
#endif
  if (ok) {
    this->playback_start_time_us_ = 0;  // anchored again on the next frame, like the first one
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
  const bool ok = this->avi_parser_->load_index(this->cache_buffer_, CACHE_BUFFER_SIZE) &&
                  this->avi_parser_->seek_to_frame(frame);
  reader->prefill_cache();
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
  const bool ok = this->video_format_ == VideoFormat::AVI_MJPEG ? this->avi_parser_->rewind() : reader->seek(0);
  reader->prefill_cache();
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
  // output_buffers_ are the canvas' size (ensure_canvas_outputs_(), before this). Just verify this file fits.
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
  this->direct_frame_ = nullptr;
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
  this->controls_reserve_ = pool_round(this->controls_bytes_());
  fixed += this->controls_reserve_;
#endif
#ifdef SVP_STREAM
  if (this->stream_port_ != 0) {
    this->stream_max_frame_bytes_ = pool_round(this->frame_bytes_ / 4);
    fixed += STREAM_FRAME_COUNT * static_cast<size_t>(this->stream_max_frame_bytes_) + pool_round(STREAM_RECV_BYTES);
#ifdef SVP_SPICE
    // The cursors' buffers only while a desktop session runs (desktop_buffers_take_()): not in the plan.
    fixed += pool_round(sizeof(SpiceState)) +
             pool_round(this->spice_saved_count_ * sizeof(SpiceSaved)) +
             pool_round(this->spice_saved_count_ * sizeof(ESPPreferenceObject));
    if (!this->spice_file_path_.empty())
      fixed += pool_round(this->spice_saved_count_ * SPICE_SAVED_USB * sizeof(SpiceSavedUsb)) +
               pool_round(this->spice_file_bytes_());
#endif
#ifdef USE_AUDIO
    if (this->speaker_ != nullptr)
      fixed += pool_round(static_cast<size_t>(AUDIO_SAMPLE_RATE) * STREAM_AUDIO_BLOCK_MS / 1000 * 2 * sizeof(int16_t));
#endif
  }
#endif
#ifdef SVP_PICTURES
  if (this->pic_canvas_ != nullptr)
    fixed += this->images_.bytes(MAX_IMAGES) + this->images_order_.bytes(MAX_IMAGES);
  if (this->pic_canvas_ != nullptr && this->thumbs_enabled_ && this->thumb_container_ != nullptr &&
      this->thumb_count_ > 0)
    fixed += pool_round(static_cast<size_t>(lv_draw_buf_width_to_stride(this->thumb_w_, LV_COLOR_FORMAT_RGB565)) *
                        this->thumb_h_ * this->thumb_count_) +
             this->thumb_slots_.bytes(this->thumb_count_) + this->thumb_queue_.bytes(this->thumb_count_);
#endif
  // The lists (setup(), the features' setups).
  fixed += this->playlist_.bytes(MAX_PLAYLIST) + this->playlist_order_.bytes(MAX_PLAYLIST) +
           pool_round(sizeof(FolderListings));
#ifdef SVP_AUDIO_FILES
  if (this->file_speaker_ != nullptr)
    fixed += this->audio_queue_.bytes(MAX_PLAYLIST) + this->audio_titles_.bytes(MAX_PLAYLIST) +
             this->audio_order_.bytes(MAX_PLAYLIST);
#endif
#ifdef SVP_CHANNEL_LIST
  if (!this->channel_url_.empty()) {
    fixed += pool_round(sizeof(HaTexts));
    this->channel_lists_(nullptr, &fixed);
  }
#endif
#ifdef SVP_AUDIO_FILES
  if (this->file_speaker_ != nullptr)
    fixed += pool_round(AUDIO_FILE_RING_SIZE);
#endif
#ifdef SVP_KEYBOARD
  fixed += pool_round(KBD_LIST_BYTES) + pool_round(KBD_MAX * sizeof(KbdEntry)) + KBD_SLOTS * pool_round(KBD_LAYOUT_BYTES) +
           pool_round(sizeof(KbdAssignTable)) + this->kbd_downloads_.bytes(KBD_DOWNLOADS_MAX) +
           this->kbd_devices_.bytes(KBD_DEVICES_MAX);
#endif
  // The player's objects, a piece each (psram_make(), the sound ring's, the audio decoder's own).
  fixed += 2 * pool_round(sizeof(BufferedFileReader)) + pool_round(sizeof(AVIParser));
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    fixed += pool_round(sizeof(PoolRingBuffer) + 64);
#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC) || defined(SVP_AUDIO_FILES)
    fixed += pool_round(sizeof(audio::AudioDecoder)) + pool_round(sizeof(audio::AudioSinkTransferBuffer)) +
             pool_round(AUDIO_DECODER_OUTPUT_BUFFER_SIZE) + pool_round(sizeof(audio::RingBufferAudioSource));
#ifdef USE_AUDIO_FLAC_SUPPORT
    fixed += pool_round(sizeof(micro_flac::FLACDecoder));
#endif
#ifdef USE_AUDIO_MP3_SUPPORT
    fixed += pool_round(sizeof(micro_mp3::Mp3Decoder));
#endif
#ifdef USE_AUDIO_OPUS_SUPPORT
    fixed += pool_round(sizeof(micro_opus::OggOpusDecoder));
#endif
#ifdef USE_AUDIO_WAV_SUPPORT
    fixed += pool_round(sizeof(micro_wav::WAVDecoder));
#endif
#ifdef USE_AUDIO_AAC_SUPPORT
    fixed += pool_round(sizeof(esp_audio_codec_adapter::AACDecoder));
#endif
#endif
  }
#endif
#if defined(SVP_PICTURES) && (defined(USE_RUNTIME_IMAGE_PNG) || defined(USE_RUNTIME_IMAGE_BMP))
  fixed += pool_round(sizeof(runtime_image::RuntimeImage));
#endif
  this->index_reserve_ = pool_round(std::clamp<size_t>(pool / 32, 128 * 1024, MIB));
  fixed += this->index_reserve_ + pool_round(CACHE_BUFFER_SIZE) + 32 * PsramPool::ALIGN;  // pieces end on whole cache lines

#ifdef USE_AUDIO
  if (this->speaker_ != nullptr && AUDIO_TEMP_BUFFER)
    fixed += pool_round(AUDIO_TEMP_BUFFER_SIZE);
#endif
  // The rest is the read-ahead ring's and the sound block's: as large as the prebuffer and the
  // session's lead need, so their limits come from it.
  this->ring_room_ = fixed < pool ? pool - fixed : 0;
  if (ring_bytes_(MIN_PREBUFFER_MS) + this->sound_bytes_(0) > this->ring_room_) {
    ESP_LOGE(TAG, "psram_pool_size %u is too small: %u bytes go to frame slot, outputs, controls and index",
             static_cast<unsigned>(pool), static_cast<unsigned>(fixed));
    this->ring_room_ = 0;
    return false;
  }
  const uint32_t prebuffer =
      std::clamp(this->prebuffer_ms_.load(std::memory_order_relaxed), MIN_PREBUFFER_MS, this->max_prebuffer());
  this->prebuffer_ms_.store(prebuffer, std::memory_order_relaxed);
  this->max_audio_latency_ms_ = this->latency_limit_ms_(prebuffer);
  return true;
}

size_t SimpleVideoPlayer::ring_bytes_(uint32_t ms) {
  const size_t chunk = BufferedFileReader::FILL_CHUNK;
  const size_t bytes = static_cast<size_t>(static_cast<uint64_t>(MAX_MEDIA_RATE) * ms / 1000);
  return std::max<size_t>((bytes + chunk - 1) / chunk, 2) * chunk;
}

size_t SimpleVideoPlayer::sound_bytes_(uint32_t lead_ms) const {
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr)
    return pool_round(std::max(AUDIO_INPUT_RING ? audio_input_bytes(lead_ms) : 0,
                               AUDIO_DECODED_RING ? audio_decoded_bytes(lead_ms) : 0));
#endif
  return 0;
}

uint32_t SimpleVideoPlayer::latency_limit_ms_(uint32_t prebuffer_ms) const {
  uint32_t lead = 0;
  while (ring_bytes_(prebuffer_ms + lead + LATENCY_STEP_MS) + this->sound_bytes_(lead + LATENCY_STEP_MS) <=
         this->ring_room_)
    lead += LATENCY_STEP_MS;
  return lead;
}

uint32_t SimpleVideoPlayer::max_prebuffer() const {
  uint32_t ms = MIN_PREBUFFER_MS;
  while (ring_bytes_(ms + LATENCY_STEP_MS) + this->sound_bytes_(0) <= this->ring_room_)
    ms += LATENCY_STEP_MS;
  return ms;
}

void SimpleVideoPlayer::set_prebuffer(uint32_t ms) {
  if (this->ring_room_ == 0) {
    this->prebuffer_ms_.store(ms, std::memory_order_relaxed);  // before setup: plan_pool_() keeps it in bounds
    return;
  }
  ms = std::clamp(ms, MIN_PREBUFFER_MS, this->max_prebuffer());
  this->prebuffer_ms_.store(ms, std::memory_order_relaxed);
  this->max_audio_latency_ms_ = this->latency_limit_ms_(ms);
}

uint8_t *SimpleVideoPlayer::pool_take_(size_t size, uint8_t user) {
  uint8_t *piece = this->pool_.alloc(size);
  for (size_t step = 0; piece == nullptr && step < POOL_STEPS && POOL_OWNERS[user][step] != 0; step++) {
    if (this->pool_give_back_(POOL_OWNERS[user][step], user))
      piece = this->pool_.alloc(size);
  }
  return piece;
}

bool SimpleVideoPlayer::pool_give_back_(uint8_t groups, uint8_t user) {
  if (user == POOL_USER_PICTURES || user == POOL_USER_CAPTURE || user == POOL_USER_GAME)
    return this->give_back_media_(groups);  // main loop
  if (groups == POOL_CAPTURE) {
#ifdef SVP_CAPTURE
    return this->pool_request_(groups, user);
#else
    return false;
#endif
  }
  if (groups == POOL_DESKTOP) {
#ifdef SVP_SPICE
    return this->release_desktop_();  // any task, under its own locks
#else
    return false;
#endif
  }
  if (groups == POOL_PICTURES) {
#ifdef SVP_PICTURES
    if (!this->pic_ready_)
      return false;
#else
    return false;
#endif
  }
  return this->pool_request_(groups, user);
}

bool SimpleVideoPlayer::pool_request_(uint8_t groups, uint8_t user) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  PoolRequest *request = nullptr;
  for (auto &slot : this->pool_requests_) {
    TaskHandle_t none = nullptr;
    if (slot.task.compare_exchange_strong(none, self, std::memory_order_acq_rel)) {
      request = &slot;
      break;
    }
  }
  if (request == nullptr)
    return false;
  ulTaskNotifyTake(pdTRUE, 0);  // a stale wake of an earlier answer
  request->user.store(user, std::memory_order_relaxed);
  request->groups.store(groups, std::memory_order_release);
  App.wake_loop_threadsafe();
  const uint32_t start = millis();
  for (uint32_t waited = 0; request->groups.load(std::memory_order_acquire) != 0 && waited < 2000;
       waited = millis() - start)
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000 - waited));
  // Not answered in time: taken back, the main loop frees nothing for nobody.
  const bool answered = request->groups.exchange(0, std::memory_order_acq_rel) == 0;
  request->task.store(nullptr, std::memory_order_release);
  return answered;
}

bool SimpleVideoPlayer::pool_wanted_(uint8_t groups) const {
  for (const auto &request : this->pool_requests_) {
    if ((request.groups.load(std::memory_order_acquire) & groups) != 0)
      return true;
  }
  return false;
}

void SimpleVideoPlayer::pool_answer_(uint8_t groups) {
  for (auto &request : this->pool_requests_) {
    const uint8_t asked = request.groups.load(std::memory_order_acquire);
    if ((asked & groups) != 0)
      pool_answered_(request, asked);
  }
}

bool SimpleVideoPlayer::video_session_() const {
  const PlayerState state = this->state_.load(std::memory_order_acquire);
  bool audio_file = false;
#ifdef SVP_AUDIO_FILES
  audio_file = this->audio_session_.load(std::memory_order_acquire);
#endif
  if ((!audio_file && (state == PlayerState::PLAYING || state == PlayerState::PAUSED)) ||
      this->picture_session_busy_() || this->start_pending_.load(std::memory_order_acquire))
    return true;
#ifdef SVP_STREAM
  if (this->stream_avi_.load(std::memory_order_acquire))
    return true;
#endif
  return false;
}

// Main loop. An output only when nothing draws into it: the stream draws under output_lock_.
void SimpleVideoPlayer::pool_service_() {
  for (auto &request : this->pool_requests_) {
    const uint8_t groups = request.groups.load(std::memory_order_acquire);
#ifdef SVP_CAPTURE
    // A session takes the panel: the screencast ends; answered once its pictures and JPEGs are back.
    if (groups == POOL_CAPTURE) {
      if (this->capture_give_back_(request.user.load(std::memory_order_relaxed)))
        pool_answered_(request, groups);
      continue;
    }
#endif
    if ((groups & (POOL_VIDEO | POOL_DIRECT | POOL_CANVAS)) == 0)
      continue;
    if ((groups & POOL_VIDEO) != 0) {
      // In use by a video session (also one starting): not handed back.
      if (!this->video_session_()) {
        if (xSemaphoreTake(this->media_lock_, 0) != pdTRUE)
          continue;  // next loop
        this->release_video_buffers_();
        xSemaphoreGive(this->media_lock_);
      }
      pool_answered_(request, groups);
      continue;
    }
#ifdef SVP_STREAM
    // The stream never takes a file's output, also not while its session starts or ends: it goes
    // without (its alloc fails, the frame is dropped).
    const uint8_t user = request.user.load(std::memory_order_relaxed);
    if ((user == POOL_USER_STREAM_DIRECT || user == POOL_USER_STREAM_CANVAS) &&
        (this->picture_session_busy_() || this->start_pending_.load(std::memory_order_acquire))) {
      pool_answered_(request, groups);
      continue;
    }
    if (this->output_lock_ != nullptr && xSemaphoreTake(this->output_lock_, 0) != pdTRUE)
      continue;  // next loop
#endif
    if (xSemaphoreTake(this->media_lock_, 0) == pdTRUE) {
      this->release_outputs_(groups);
      xSemaphoreGive(this->media_lock_);
      pool_answered_(request, groups);
    }
#ifdef SVP_STREAM
    if (this->output_lock_ != nullptr)
      xSemaphoreGive(this->output_lock_);
#endif
  }
}

// Main loop, for pictures and cover art: only when nothing plays or streams.
bool SimpleVideoPlayer::give_back_media_(uint8_t groups) {
  if (this->video_session_())
    return false;
#ifdef SVP_STREAM
  if (this->stream_connected_.load(std::memory_order_acquire))
    return false;
#endif
  if (groups == POOL_DESKTOP) {
#ifdef SVP_SPICE
    return this->release_desktop_();
#else
    return false;
#endif
  }
#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr && xSemaphoreTake(this->output_lock_, 0) != pdTRUE)
    return false;  // a rectangle is being drawn
#endif
  bool released = false;
  if (xSemaphoreTake(this->media_lock_, 0) == pdTRUE) {
    released = (groups & POOL_VIDEO) != 0 ? this->release_video_buffers_() : this->release_outputs_(groups);
    xSemaphoreGive(this->media_lock_);
  }
#ifdef SVP_STREAM
  if (this->output_lock_ != nullptr)
    xSemaphoreGive(this->output_lock_);
#endif
  if (released)
    ESP_LOGI(TAG, "Video buffers handed back to the pool for pictures");
  return released;
}

uint8_t *SimpleVideoPlayer::media_alloc_(size_t size) {
  uint8_t *piece = this->pool_take_(size, POOL_USER_VIDEO);
  if (piece == nullptr)
    ESP_LOGE(TAG, "PSRAM pool: no room for %zu bytes (%zu of %zu used, largest free %zu)", size, this->pool_.used(),
             this->pool_.size(), this->pool_.largest_free());
  return piece;
}

bool SimpleVideoPlayer::acquire_session_buffers_() {
  // This session's sizes: the ring holds the prebuffer and the sound's lead, the sound block the lead.
  uint32_t lead_ms = 0;
#ifdef USE_AUDIO
  lead_ms = this->audio_lead_us_ / 1000;
#endif
  const size_t ring_len = ring_bytes_(this->prebuffer_ms_.load(std::memory_order_relaxed) + lead_ms);
  if (this->ring_buf_ != nullptr && this->ring_size_ != ring_len) {
    // Another size than the kept ring's: given back before the new one is taken (no file is open).
    xSemaphoreTake(this->media_lock_, portMAX_DELAY);
    if (this->file_reader_)
      this->file_reader_->set_ring(nullptr, 0);
    this->pool_.free(this->ring_buf_);
    this->ring_buf_ = nullptr;
    this->ring_size_ = 0;
    xSemaphoreGive(this->media_lock_);
  }
  // From the pool; only when it does not fit, what this start took goes back and the owner table's
  // video row hands back step by step: the video's buffers come from one free stretch.
  bool ok = this->take_session_buffers_(lead_ms, ring_len);
  for (size_t step = 0; !ok && step < POOL_STEPS && POOL_OWNERS[POOL_USER_VIDEO][step] != 0; step++) {
    if (this->pool_give_back_(POOL_OWNERS[POOL_USER_VIDEO][step], POOL_USER_VIDEO))
      ok = this->take_session_buffers_(lead_ms, ring_len);
  }
  if (!ok) {
    ESP_LOGE(TAG, "PSRAM pool: no room for the video's buffers (%zu of %zu used, largest free %zu)",
             this->pool_.used(), this->pool_.size(), this->pool_.largest_free());
    return false;
  }
  if (!this->file_reader_)
    this->file_reader_ = psram_make<BufferedFileReader>();
  if (!this->file_reader_)
    return false;
  if (!this->file_reader_->has_ring())
    this->file_reader_->set_ring(this->ring_buf_, this->ring_size_);
  return true;
}

bool SimpleVideoPlayer::take_session_buffers_(uint32_t lead_ms, size_t ring_len) {
  bool sound_taken = false;
#ifdef USE_AUDIO
  if (!this->sound_block_for_(lead_ms, &sound_taken))
    return false;
#endif
  // Taken without media_lock_, set under it. Nothing else sets these while this session starts.
  uint8_t *slots[FRAME_SLOT_COUNT]{};
  bool ok = true;
  for (size_t i = 0; i < FRAME_SLOT_COUNT; i++) {
    if (ok && this->frame_slots_[i].data == nullptr) {
      slots[i] = this->pool_.alloc(this->frame_bytes_);
      ok = slots[i] != nullptr;
    }
  }
  uint8_t *ring = ok && this->ring_buf_ == nullptr ? this->pool_.alloc(ring_len) : nullptr;
  ok = ok && (this->ring_buf_ != nullptr || ring != nullptr);
  if (!ok) {
    // One did not fit: what this try took goes back (the next try takes it after the others).
    for (auto *slot : slots)
      this->pool_.free(slot);
    this->pool_.free(ring);
#ifdef USE_AUDIO
    if (sound_taken) {
      this->pool_.free(this->sound_block_);
      this->sound_block_ = nullptr;
      this->sound_block_len_ = 0;
    }
#endif
    return false;
  }
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  for (size_t i = 0; i < FRAME_SLOT_COUNT; i++) {
    if (slots[i] != nullptr) {
      this->frame_slots_[i].data = slots[i];
      this->frame_slots_[i].capacity = this->frame_bytes_;
    }
  }
  if (ring != nullptr) {
    this->ring_buf_ = ring;
    this->ring_size_ = ring_len;
  }
  xSemaphoreGive(this->media_lock_);
  return true;
}

// Playback or stream task, media lock not held. Only when it does not fit, the owner table's row
// hands back: the other output first (main loop), then the pictures', ...
uint8_t *SimpleVideoPlayer::output_alloc_(size_t size, bool direct) {
  uint8_t user = direct ? POOL_USER_DIRECT : POOL_USER_CANVAS;
#ifdef SVP_STREAM
  if (xTaskGetCurrentTaskHandle() == this->stream_draw_task_)
    user = direct ? POOL_USER_STREAM_DIRECT : POOL_USER_STREAM_CANVAS;
#endif
  uint8_t *piece = this->pool_take_(size, user);
  if (piece == nullptr)
    ESP_LOGE(TAG, "PSRAM pool: no room for %zu bytes (%zu of %zu used, largest free %zu)", size, this->pool_.used(),
             this->pool_.size(), this->pool_.largest_free());
  return piece;
}

#ifdef SVP_DSI_OUTPUT
bool SimpleVideoPlayer::ensure_direct_target_(bool with_target) {
  if (this->decode_target_ != nullptr)
    return true;
  if (!with_target) {
    // Whole frames go into the display's own buffers: only the controls drawn over them.
#ifdef SVP_CONTROLS
    xSemaphoreTake(this->media_lock_, portMAX_DELAY);
    if (this->ctl_mem_ == nullptr && this->controls_reserve_ != 0)
      this->ctl_mem_ = this->pool_.alloc(this->controls_reserve_);
    xSemaphoreGive(this->media_lock_);
#endif
    return true;
  }
  // Whole cache lines, as the JPEG unit writes it by DMA.
  const size_t size = pool_round(static_cast<size_t>(this->dsi_out_w_) * this->dsi_out_h_ * this->dsi_fb_bpp_);
  uint8_t *target = this->output_alloc_(size, true);
  if (target == nullptr)
    return false;
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  // The stream's draw task and a starting file may both get here: one target only.
  if (this->decode_target_ != nullptr) {
    this->pool_.free(target);
  } else {
    this->decode_target_len_ = size;
    this->decode_target_ = target;
  }
#ifdef SVP_CONTROLS
  // The controls drawn over the direct output, with the display's decode buffer.
  if (this->ctl_mem_ == nullptr && this->controls_reserve_ != 0)
    this->ctl_mem_ = this->pool_.alloc(this->controls_reserve_);
#endif
  xSemaphoreGive(this->media_lock_);
  return true;
}

bool SimpleVideoPlayer::direct_flips_() const {
  void *fbs[2]{};
  display::ColorBitness bitness{};
  return this->dsi_->get_scanout_buffers(fbs, 2, &bitness) == 2 &&
         bitness == (this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565);
}

uint8_t *SimpleVideoPlayer::direct_target_(bool whole, size_t *len) {
  if (whole && this->direct_flips_()) {
    void *fbs[2]{};
    display::ColorBitness bitness{};
    this->dsi_->get_scanout_buffers(fbs, 2, &bitness);
    uint8_t *shown = this->dsi_->get_draw_buffer(&bitness);
    *len = static_cast<size_t>(this->dsi_out_w_) * this->dsi_out_h_ * this->dsi_fb_bpp_;
    if (this->dsi_->wait_scanout_flipped(FLIP_WAIT_MS))
      return static_cast<uint8_t *>(fbs[0] == shown ? fbs[1] : fbs[0]);
    // The flip did not come: the own buffer if there is one, else the one shown.
    if (this->decode_target_ == nullptr)
      return shown;
  }
  *len = this->decode_target_len_;
  return this->decode_target_;
}

void SimpleVideoPlayer::direct_show_(uint8_t *target, int x, int y, int w, int h, int x_pad) {
  display::ColorBitness bitness{};
  if (target != this->decode_target_) {
    // One of the display's buffers: written back from the cache by what drew over it (controls).
    if (target != this->dsi_->get_draw_buffer(&bitness))
      this->dsi_->present_scanout_buffer(target);
    return;
  }
  bitness = this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
  this->dsi_->draw_pixels_at(x, y, w, h, target, bitness, false, 0, 0, x_pad);
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
      buf = this->output_alloc_(size, false);
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
  xSemaphoreTake(this->media_lock_, portMAX_DELAY);
  // The stream's draw task and a starting file may both get here: one pair only.
  if (this->output_buffers_[1] != nullptr) {
    for (auto *buf : buffers) {
      if (buf != this->output_buffers_[0] && buf != this->output_buffers_[1])
        this->pool_.free(buf);
    }
  } else {
    this->output_buffer_size_ = size;
    this->output_buffers_[0] = buffers[0];
    this->output_buffers_[1] = buffers[1];
  }
  xSemaphoreGive(this->media_lock_);
  return true;
}
#endif

// Main loop, the media lock held (and output_lock_, with a stream): nothing draws into them.
bool SimpleVideoPlayer::release_outputs_(uint8_t groups) {
  bool released = false;
#ifdef SVP_CANVAS_OUTPUT
  if ((groups & POOL_CANVAS) != 0 && this->output_buffers_[0] != nullptr) {
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
  if ((groups & POOL_DIRECT) != 0) {
    if (this->decode_target_ != nullptr) {
      this->pool_.free(this->decode_target_);
      if (this->direct_frame_ == this->decode_target_)
        this->direct_frame_ = nullptr;
      this->decode_target_ = nullptr;
      this->decode_target_len_ = 0;
      released = true;
    }
#ifdef SVP_CONTROLS
    if (this->ctl_mem_ != nullptr) {
      this->ctl_ready_.store(false, std::memory_order_release);
      this->ctl_prerendered_ = false;  // its sprites went with it
      this->pool_.free(this->ctl_mem_);
      this->ctl_mem_ = nullptr;
      released = true;
    }
#endif
  }
#endif
  return released;
}

// Main loop, the media lock held, no video session.
bool SimpleVideoPlayer::release_video_buffers_() {
  bool released = false;
  if (this->ring_buf_ != nullptr) {
    if (this->file_reader_)
      this->file_reader_->set_ring(nullptr, 0);
    this->pool_.free(this->ring_buf_);
    this->ring_buf_ = nullptr;
    this->ring_size_ = 0;
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
  if (!this->sound_ring_for_(codec_type != audio::AudioFileType::NONE)) {
    ESP_LOGE(TAG, "No sound ring for this file's audio. Playing video-only.");
    return false;
  }
  if (this->audio_input_ring_buffer_)
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
  // state every call). Only compressed formats (MP3/FLAC) use it at all; PCM mode leaves it unused.
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

  // The sound of this session on the audio task (Core 0, made in setup()).
  this->audio_task_stop_ = false;
  this->audio_busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->audio_task_handle_);

  // The speaker starts right before the first frame: a mixer source started earlier stops again
  // after its timeout without data.
  this->audio_enabled_ = true;
  this->session_audio_ = true;
  ESP_LOGI(TAG, "Audio processing initialized successfully (%s mode)", use_decoder ? "decoder" : "direct PCM");
  return true;
}

#endif  // USE_AUDIO
#if defined(USE_AUDIO) || defined(SVP_CAPTURE_MIC)
std::shared_ptr<ring_buffer::RingBuffer> PoolRingBuffer::create(uint8_t *storage, size_t len) {
  PsramPool *pool = PsramPool::instance();
  if (storage == nullptr || pool == nullptr || pool->largest_free() < sizeof(PoolRingBuffer) + 64)
    return nullptr;
  // The object and the shared_ptr's control block in one piece of the pool.
  auto rb = std::allocate_shared<PoolRingBuffer>(PoolAllocator<PoolRingBuffer>());
  if (xRingbufferCreateStatic(len, RINGBUF_TYPE_BYTEBUF, storage, &rb->structure_) != rb->handle_())
    return nullptr;
  rb->created_ = true;
  return rb;
}
#endif
#ifdef USE_AUDIO

// Playback task, video session start, no sound task running: the sound block for this lead. Kept when
// its size is the same; else the rings over it and the decoder's hold on them go first, then the
// block is taken anew.
bool SimpleVideoPlayer::sound_block_for_(uint32_t lead_ms, bool *taken) {
  *taken = false;
  const size_t len = this->sound_bytes_(lead_ms);
  if (len == 0 || (this->sound_block_ != nullptr && this->sound_block_len_ == len)) {
    this->sound_lead_ms_ = lead_ms;
    return true;
  }
  this->audio_input_ring_buffer_.reset();
  this->audio_decoded_ring_buffer_.reset();
#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC) || defined(SVP_AUDIO_FILES)
  if (this->audio_decoder_)
    this->audio_decoder_->add_source(nullptr, 0);  // its source gives back what it holds of the old ring
#endif
  this->pool_.free(this->sound_block_);
  this->sound_block_ = nullptr;
  this->sound_block_len_ = 0;
  uint8_t *block = this->pool_.alloc(len);
  if (block == nullptr)
    return false;
  this->sound_block_ = block;
  this->sound_block_len_ = len;
  this->sound_lead_ms_ = lead_ms;
  *taken = true;
  return true;
}

// Session start, no sound task running: the ring of the kind the file needs, over the block for the
// lead it was taken for. Made anew only when the kind or the block changed (no pool allocation).
bool SimpleVideoPlayer::sound_ring_for_(bool compressed) {
  const size_t len = compressed ? audio_input_bytes(this->sound_lead_ms_) : audio_decoded_bytes(this->sound_lead_ms_);
  const auto &current = compressed ? this->audio_input_ring_buffer_ : this->audio_decoded_ring_buffer_;
  if (current != nullptr && this->sound_ring_len_ == len)
    return true;
  if (this->sound_block_ == nullptr || (compressed ? !AUDIO_INPUT_RING : !AUDIO_DECODED_RING) ||
      len > this->sound_block_len_)
    return false;
  this->audio_input_ring_buffer_.reset();
  this->audio_decoded_ring_buffer_.reset();
#if defined(SVP_AUDIO_SUPPORT_MP3) || defined(SVP_AUDIO_SUPPORT_FLAC) || defined(SVP_AUDIO_FILES)
  if (this->audio_decoder_)
    this->audio_decoder_->add_source(nullptr, 0);  // its source gives back what it holds of the old ring
#endif
  this->sound_ring_len_ = len;
  std::shared_ptr<ring_buffer::RingBuffer> ring = PoolRingBuffer::create(this->sound_block_, len);
  if (compressed) {
    this->audio_input_ring_buffer_ = std::move(ring);
  } else {
    this->audio_decoded_ring_buffer_ = std::move(ring);
  }
  return compressed ? this->audio_input_ring_buffer_ != nullptr : this->audio_decoded_ring_buffer_ != nullptr;
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

  // Incomplete frames are dropped (shouldn't happen with well-formed AVI); counted here, logged at
  // the session's end (end_session_audio_()): no log on the reader's path.
  if (size != bytes_to_write) {
    this->audio_dropped_bytes_.fetch_add(static_cast<uint32_t>(size - bytes_to_write), std::memory_order_relaxed);
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
  if (this->audio_input_ring_buffer_)
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
  uint8_t *scratch = this->cache_buffer_;
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
    this->playback_wait_.wake();
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
  this->playback_wait_.wake();
}

void SimpleVideoPlayer::audio_task_entry_(void *param) {
  auto *player = static_cast<SimpleVideoPlayer *>(param);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!player->audio_busy_.load(std::memory_order_acquire))
      continue;
#ifdef SVP_AUDIO_FILES
    if (player->audio_session_.load(std::memory_order_acquire)) {
      player->audio_file_loop_();
    } else {
      player->audio_processing_loop_();
    }
#else
    player->audio_processing_loop_();
#endif
    player->audio_busy_.store(false, std::memory_order_release);
    player->playback_wait_.wake();
#ifdef SVP_STREAM
    player->stream_wake_();
#endif
  }
}

void SimpleVideoPlayer::audio_processing_loop_() {
  ESP_LOGI(TAG, "Audio processing task started on core %d", xPortGetCoreID());

  // Core 0, prio 1, apart from the Core 1 decode task: a multi-ms jpeg_decoder_process() there does
  // not stall the speaker feed. It blocks on the ring and on the speaker, never spins.
  // Parked until the playback task releases it at the first video frame (or stop_audio_task_()).
  this->audio_wait_.wait(
      [this]() { return this->audio_task_stop_ || this->av_started_.load(std::memory_order_acquire); },
      portMAX_DELAY);
  esp_task_wdt_add(nullptr);

  // A single failed decode drops that chunk and the loop keeps going, instead of tearing audio
  // down on the first hiccup. Only give up on audio entirely after this many consecutive failures
  // (a genuinely broken stream).
  static constexpr uint32_t AUDIO_MAX_CONSECUTIVE_DECODE_FAILURES = 10;
  // Blocking ring read and speaker write: the task sleeps until there is sound or room for it; this
  // only bounds how soon a stop or a seek is seen while neither comes.
  static constexpr TickType_t AUDIO_WAIT = pdMS_TO_TICKS(20);
  uint32_t audio_decode_failures = 0;

  while (!this->audio_task_stop_) {
    esp_task_wdt_reset();

    if (!this->audio_enabled_)
      break;

    // Paused: the sound holds where the picture stopped, so both go on together on resume. Held by
    // a seek: the feed is restarted at the new position. Blocked until that is over (off the task
    // watchdog meanwhile, a pause may last).
    if (this->audio_hold_.load(std::memory_order_acquire) ||
        this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED) {
      this->audio_held_.store(true, std::memory_order_release);
      this->playback_wait_.wake();
      esp_task_wdt_delete(nullptr);
      this->audio_wait_.wait(
          [this]() {
            return this->audio_task_stop_ || !this->audio_held_.load(std::memory_order_acquire) ||
                   (!this->audio_hold_.load(std::memory_order_acquire) &&
                    this->state_.load(std::memory_order_acquire) != PlayerState::PAUSED);
          },
          portMAX_DELAY);
      esp_task_wdt_add(nullptr);
      continue;
    }

    // Run audio decoder only when THIS file is compressed (MP3/FLAC). In a multi-codec build
    // audio_decoder_ exists even for a PCM file, so gate on the per-file mode, not the pointer --
    // calling decode() on a decoder that was never start()ed for this file returns FAILED.
    // decode() itself blocks on its input and on the speaker.
    const bool use_decoder = this->audio_use_decoder_.load(std::memory_order_acquire);
    if (use_decoder) {
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
    }

    // Drain the decoded ring to the speaker. Formats match end to end (config == file == speaker),
    // so this is a straight byte copy -- no conversion.
    if (this->audio_decoded_ring_buffer_ && this->speaker_) {
      const size_t bytes_read = this->audio_decoded_ring_buffer_->read(this->audio_temp_buffer_, AUDIO_TEMP_BUFFER_SIZE,
                                                                       use_decoder ? 0 : AUDIO_WAIT);
#ifdef SVP_STREAM_STATS
      if (bytes_read == 0 && !use_decoder)
        this->stream_stats_.sound_waits.fetch_add(1, std::memory_order_relaxed);
#endif
      size_t bytes_remaining = bytes_read;
      const uint8_t *write_ptr = this->audio_temp_buffer_;
      // A seek drops what is left from before the jump.
      while (bytes_remaining > 0 && !this->audio_task_stop_ && !this->audio_hold_.load(std::memory_order_acquire)) {
        const size_t written = this->speaker_->play(write_ptr, bytes_remaining, AUDIO_WAIT);
        bytes_remaining -= written;
        write_ptr += written;
        esp_task_wdt_reset();
      }
    }
  }  // End of while loop

  ESP_LOGI(TAG, "Audio processing task stopped");
  esp_task_wdt_delete(nullptr);
}

// While paused the speaker keeps what it holds: the mixer does not drain a paused source, so it
// neither plays on nor times out and frees its buffers.
void SimpleVideoPlayer::speaker_pause_(bool paused) {
#ifdef SVP_AUDIO_FILES
  if (this->audio_session_.load(std::memory_order_acquire)) {
    if (this->file_speaker_open_)
      this->file_speaker_->set_pause_state(paused);
    return;
  }
#endif
  // A video's sound: the audio task holds itself; the speaker is left as it is.
}

// Playback task, end of a session: the audio task (also one that turned audio off itself, or that
// still waits for the first frame) and the speaker.
void SimpleVideoPlayer::end_session_audio_() {
  // audio_decoder_ and the rings are permanent (setup()): init_audio_decoder_() starts them anew.
  const bool had_audio = this->session_audio_ || this->audio_busy_.load(std::memory_order_acquire);
  this->stop_audio_task_();
  this->audio_enabled_ = false;
  this->session_audio_ = false;
  const uint32_t dropped = this->audio_dropped_bytes_.exchange(0, std::memory_order_relaxed);
  if (dropped != 0)
    ESP_LOGW(TAG, "Dropped %" PRIu32 " bytes of incomplete audio frames", dropped);
  if (had_audio)
    ESP_LOGI(TAG, "Audio processing stopped");
}

// Playback task: the session's sound ends. A task that does not let go in time keeps audio_busy_,
// and the next session waits for it (session_busy_()).
void SimpleVideoPlayer::stop_audio_task_() {
  if (!this->audio_busy_.load(std::memory_order_acquire))
    return;
  this->audio_task_stop_ = true;
  xTaskNotifyGive(this->audio_task_handle_);
  const uint32_t start = millis();
  this->playback_wait_for_(
      [&]() { return !this->audio_busy_.load(std::memory_order_acquire) || millis() - start >= 3000; });
  if (this->audio_busy_.load(std::memory_order_acquire))
    ESP_LOGE(TAG, "Audio task still busy after 3000 ms");
}

#ifdef SVP_AUDIO_FILES
//========================================================================
// Audio files: their own queue, one track per session
//========================================================================

// The order of play from audio_pos_ 0: `first` leads, shuffled or as added.
void SimpleVideoPlayer::audio_order_build_(size_t first) {
  const size_t count = this->audio_queue_.size();
  this->audio_order_.clear();
  for (size_t i = 0; i < count; i++)
    this->audio_order_.push_back(static_cast<uint16_t>(i));
  if (!this->audio_shuffle_ || count < 2) {
    this->audio_pos_ = std::min(first, count != 0 ? count - 1 : 0);
    return;
  }
  for (size_t i = count - 1; i > 0; i--)
    std::swap(this->audio_order_[i], this->audio_order_[random_uint32() % (i + 1)]);
  if (first < count) {
    const auto at = std::find(this->audio_order_.begin(), this->audio_order_.end(), static_cast<uint16_t>(first));
    std::swap(*at, this->audio_order_[0]);
  }
  this->audio_pos_ = 0;
}

void SimpleVideoPlayer::audio_start_(size_t pos) {
  if (pos >= this->audio_order_.size())
    return;
#ifdef SVP_EMULATOR
  if (this->emu_refuses_())
    return;
#endif
#ifdef SVP_SPICE
  if (this->spice_sound_on_.load(std::memory_order_acquire) && this->spice_sound_shares_files_) {
    ESP_LOGW(TAG, "Refused: the SPICE session's sound plays on this device");
    return;
  }
#endif
#if defined(SVP_MEDIA_PLAYER) && defined(SVP_CHANNEL_LIST)
  this->audio_remote_command_(media_player::MEDIA_PLAYER_COMMAND_STOP);  // a Home Assistant track before
#endif
  this->audio_pos_ = pos;
  this->audio_current_ = true;
  this->audio_halted_ = false;
  this->audio_seek_ms_.store(0, std::memory_order_release);  // a new track from its start
  this->audio_restart_ = false;
  this->audio_seek_paused_ = false;
  this->start_pending_.store(false, std::memory_order_release);
#ifdef SVP_CHANNEL_LIST
  const SvpId &track = this->audio_queue_[this->audio_order_[pos]];
  if (track.starts_with("media-source://")) {
    // Home Assistant's: svp_relay plays it on the media player; its going idle at the end goes on.
    this->audio_start_pending_.store(false, std::memory_order_release);
    if (!this->is_network_session())
      this->end_session_();  // a local session; svp_relay's go on
    if (this->file_speaker_open_ && !this->session_busy_()) {
      this->file_speaker_->finish();
      this->file_speaker_open_ = false;
    }
    this->audio_remote_.store(AUDIO_REMOTE_ASKED, std::memory_order_release);
    this->audio_track_started_ms_ = millis();
    this->audio_changed_.store(true, std::memory_order_release);
    this->ha_play_request_(track.c_str());
    return;
  }
#endif
  this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);
#ifdef SVP_AUDIO_COVER
  // A file's own cover art is not svp_relay's.
  this->cover_clear_ = true;
  this->cover_wanted_ = false;
#endif
  this->audio_start_pending_.store(true, std::memory_order_release);
  this->audio_changed_.store(true, std::memory_order_release);
  this->end_session_();
  this->run_pending_start_();
}

void SimpleVideoPlayer::audio_queue_add(const std::string &path, const std::string &title) {
  if (this->audio_queue_.full())
    return;
  if (!SvpId::fits(path.size())) {
    ESP_LOGW(TAG, "%s not queued: its path is too long", path.c_str());
    return;
  }
  this->audio_queue_.emplace_back(path.c_str());
  this->audio_titles_.emplace_back(title.c_str());
  this->audio_order_.push_back(static_cast<uint16_t>(this->audio_queue_.size() - 1));
  this->audio_queue_gen_++;
  this->audio_changed_.store(true, std::memory_order_release);
}

std::string SimpleVideoPlayer::audio_queue_title(size_t index) const {
  if (index >= this->audio_queue_.size())
    return "";
  if (index < this->audio_titles_.size() && !this->audio_titles_[index].empty())
    return this->audio_titles_[index].c_str();
  const SvpId &path = this->audio_queue_[index];
  return path.substr(path.rfind('/') + 1);
}

void SimpleVideoPlayer::audio_queue_clear() {
  if (this->is_audio_session() || this->audio_start_pending_.load(std::memory_order_acquire))
    this->stop();
  this->audio_start_pending_.store(false, std::memory_order_release);
  this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);
  this->audio_queue_.clear();
  this->audio_titles_.clear();
  this->audio_order_.clear();
  this->audio_queue_gen_++;
  this->audio_pos_ = 0;
  this->audio_current_ = false;
  this->audio_changed_.store(true, std::memory_order_release);
}

void SimpleVideoPlayer::audio_play(size_t index) {
  if (index >= this->audio_queue_.size())
    return;
  this->audio_order_build_(index);
  this->audio_start_(this->audio_pos_);
}

void SimpleVideoPlayer::audio_next() {
  if (this->audio_order_.empty())
    return;
  if (!this->audio_current_) {
    this->audio_play_or_resume();
  } else if (this->audio_pos_ + 1 < this->audio_order_.size()) {
    this->audio_start_(this->audio_pos_ + 1);
  } else if (this->audio_repeat_ == RepeatMode::PLAYLIST) {
    this->audio_order_build_(this->audio_queue_.size());
    this->audio_start_(0);
  } else {
    if (this->is_audio_session())
      this->stop();
    this->audio_current_ = false;
    this->audio_changed_.store(true, std::memory_order_release);
  }
}

void SimpleVideoPlayer::audio_previous() {
  if (this->audio_order_.empty())
    return;
  static constexpr uint32_t RESTART_AFTER_MS = 3000;
  const bool playing = this->is_audio_session() && !this->is_stopped();
  if (!this->audio_current_) {
    this->audio_play_or_resume();
  } else if (playing && millis() - this->audio_track_started_ms_ > RESTART_AFTER_MS) {
    this->audio_start_(this->audio_pos_);
  } else if (this->audio_pos_ > 0) {
    this->audio_start_(this->audio_pos_ - 1);
  } else if (this->audio_repeat_ == RepeatMode::PLAYLIST) {
    this->audio_start_(this->audio_order_.size() - 1);
  } else {
    this->audio_start_(0);
  }
}

void SimpleVideoPlayer::audio_play_or_resume() {
  if (this->is_audio_session()) {
    if (this->is_paused()) {
      this->resume();
      return;
    }
    if (this->is_playing())
      return;
  }
  if (this->audio_queue_.empty())
    return;
  if (this->audio_current_) {
    this->audio_start_(this->audio_pos_);
    return;
  }
  // Before the first track or after the last: from the start, shuffled anew.
  this->audio_order_build_(this->audio_shuffle_ ? this->audio_queue_.size() : 0);
  this->audio_start_(0);
}

void SimpleVideoPlayer::set_audio_shuffle(bool shuffle) {
  if (shuffle == this->audio_shuffle_)
    return;
  this->audio_shuffle_ = shuffle;
  // The track playing keeps playing; the rest follows in the new order.
  const size_t current = this->audio_current_ ? this->audio_order_[this->audio_pos_] : this->audio_queue_.size();
  this->audio_order_build_(current);
  this->audio_changed_.store(true, std::memory_order_release);
}

// Main loop: a track played to its end.
void SimpleVideoPlayer::audio_advance_() {
  if (!this->audio_current_)
    return;
  if (this->audio_repeat_ == RepeatMode::FILE) {
    this->audio_start_(this->audio_pos_);
  } else if (this->audio_pos_ + 1 < this->audio_order_.size()) {
    this->audio_start_(this->audio_pos_ + 1);
  } else if (this->audio_repeat_ == RepeatMode::PLAYLIST) {
    this->audio_order_build_(this->audio_queue_.size());
    this->audio_start_(0);
  } else {
    // The end of the queue: what the speaker still holds plays out.
    this->audio_current_ = false;
    this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);
    if (this->file_speaker_open_) {
      this->file_speaker_->finish();
      this->file_speaker_open_ = false;
    }
    this->audio_session_.store(false, std::memory_order_release);  // between tracks it stays set
    this->audio_changed_.store(true, std::memory_order_release);
  }
}

// Main loop, session_lock_ held, no session runs.
void SimpleVideoPlayer::start_audio_session_() {
  if (!this->audio_current_ || this->audio_pos_ >= this->audio_order_.size())
    return;
  this->audio_session_.store(true, std::memory_order_release);
  this->video_path_ = this->audio_queue_[this->audio_order_[this->audio_pos_]].c_str();
  if (this->is_failed() || this->file_speaker_ == nullptr || !this->audio_decoder_ || !this->audio_file_reader_) {
    this->set_error_(PlaybackError::DECODER_INIT_FAILED);
    return;
  }
  ESP_LOGI(TAG, "Playing audio: %s", this->video_path_.c_str());
  this->last_error_.store(PlaybackError::NONE, std::memory_order_relaxed);
  // A new source first: the old one gives back what it holds of the ring, so the reset drops all.
  // No sink until the file's format is known (audio_file_loop_()).
  this->audio_decoder_->discard_output();
  bool ok = this->sound_ring_for_(true);
  std::weak_ptr<ring_buffer::RingBuffer> source = this->audio_input_ring_buffer_;
  ok = ok && this->audio_decoder_->add_source(source) == ESP_OK;
  if (this->audio_input_ring_buffer_)
    this->audio_input_ring_buffer_->reset();
  this->audio_decoder_->add_sink(static_cast<speaker::Speaker *>(nullptr));
  ok = ok && this->audio_decoder_->start(audio::detect_audio_file_type(nullptr, this->video_path_.c_str())) == ESP_OK;
  if (!ok) {
    ESP_LOGE(TAG, "Cannot decode %s", this->video_path_.c_str());
    this->set_error_(PlaybackError::DECODER_INIT_FAILED);
    return;
  }
  // Position and duration in milliseconds; the reader learns the duration as it opens the file.
  this->frame_duration_us_ = 1000.0f;
  this->duration_frames_.store(0, std::memory_order_relaxed);
  const uint32_t at = this->audio_seek_ms_.load(std::memory_order_acquire);
  this->audio_position_base_ms_.store(at, std::memory_order_relaxed);
  this->position_frame_.store(at, std::memory_order_relaxed);
  this->reader_task_stop_ = false;
  this->audio_task_stop_ = false;
  this->audio_file_eof_.store(false, std::memory_order_release);
  this->audio_track_started_ms_ = millis();
  // A seek's start stays paused when the seek came while paused; only a track's own start is an event.
  const bool paused = this->audio_restart_ && this->audio_seek_paused_;
  if (!this->audio_restart_)
    this->audio_started_pending_ = true;
  this->audio_restart_ = false;
  this->audio_seek_paused_ = false;
  this->state_.store(paused ? PlayerState::PAUSED : PlayerState::PLAYING, std::memory_order_release);
  this->note_state_();
  this->reader_busy_.store(true, std::memory_order_release);
  this->audio_busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->reader_task_handle_);
  xTaskNotifyGive(this->audio_task_handle_);
}

// Reader task: the file into the decoder's input ring, blocking while it is full.
void SimpleVideoPlayer::audio_file_feed_() {
  static constexpr TickType_t RING_WAIT = pdMS_TO_TICKS(20);
  BufferedFileReader *reader = this->audio_file_reader_.get();
  auto running = [this]() {
    const PlayerState state = this->state_.load(std::memory_order_acquire);
    return !this->reader_task_stop_ && (state == PlayerState::PLAYING || state == PlayerState::PAUSED);
  };
  auto feed = [&](const uint8_t *data, size_t len) {
    size_t done = 0;
    while (done < len && running())
      done += this->audio_input_ring_buffer_->write_without_replacement(data + done, len - done, RING_WAIT);
  };
  reader->set_abort_flag(&this->reader_task_stop_);
  if (!reader->open(this->video_path_.c_str())) {
    ESP_LOGE(TAG, "Failed to open %s", this->video_path_.c_str());
    if (running())
      this->set_error_(PlaybackError::FILE_NOT_FOUND);
  } else {
    // The file's layout first: its length, and where the audio (or the position asked for) starts.
    uint8_t head[80];
    uint64_t from = 0;
    uint32_t base_ms = 0;
    const size_t head_len =
        this->audio_file_prepare_(reader, this->audio_seek_ms_.load(std::memory_order_acquire), head, &from, &base_ms);
    this->audio_position_base_ms_.store(base_ms, std::memory_order_release);
    this->duration_frames_.store(this->audio_layout_.duration_ms, std::memory_order_release);
    feed(head, head_len);
    bool ok = reader->seek(from);
    while (ok && running()) {
      const int read = reader->read(this->audio_temp_buffer_, AUDIO_TEMP_BUFFER_SIZE);
      if (read <= 0) {
        ok = read == 0;
        break;
      }
      feed(this->audio_temp_buffer_, static_cast<size_t>(read));
    }
    if (!ok && running()) {
      ESP_LOGE(TAG, "Failed to read %s", this->video_path_.c_str());
      this->set_error_(PlaybackError::FILE_READ_ERROR);
    }
    reader->close();
  }
  this->audio_file_eof_.store(true, std::memory_order_release);
  this->audio_wait_.wake();
}

// Main loop: the track starts anew from `ms` (the decoder fresh, its header fed first).
void SimpleVideoPlayer::audio_seek_(uint32_t ms) {
  ESP_LOGI(TAG, "Seeking to %.1f s", ms / 1000.0f);
  // A seek after a seek not started yet keeps whether the first came while paused.
  if (!this->audio_restart_)
    this->audio_seek_paused_ = this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED;
  this->audio_seek_ms_.store(ms, std::memory_order_release);
  this->audio_restart_ = true;
  this->audio_start_pending_.store(true, std::memory_order_release);
  this->audio_changed_.store(true, std::memory_order_release);
  this->end_session_();
  this->run_pending_start_();
}

static uint32_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

// The length of an ID3v2 tag at the start (header, size syncsafe, footer), 0 without one.
static uint64_t id3v2_size(const uint8_t *p, size_t len) {
  if (len < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
    return 0;
  const uint64_t size = (static_cast<uint32_t>(p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14) | ((p[8] & 0x7F) << 7) |
                        (p[9] & 0x7F);
  return size + 10 + ((p[5] & 0x10) != 0 ? 10 : 0);
}

// An MPEG audio frame header (ISO 11172-3 / 13818-3): samples per frame, rate, bitrate (kbit/s).
struct MpegHeader {
  uint32_t samples{0}, rate{0}, kbps{0};
  bool mpeg1{false}, mono{false};
};
static bool mpeg_header(const uint8_t *h, MpegHeader *out) {
  if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
    return false;
  const uint8_t version = (h[1] >> 3) & 3;  // 0 MPEG-2.5, 1 reserved, 2 MPEG-2, 3 MPEG-1
  const uint8_t layer = (h[1] >> 1) & 3;    // 1 III, 2 II, 3 I, 0 reserved
  const uint8_t bitrate = h[2] >> 4;
  const uint8_t rate = (h[2] >> 2) & 3;
  if (version == 1 || layer == 0 || bitrate == 0 || bitrate == 15 || rate == 3)
    return false;
  static const uint16_t RATES[3][3] = {{11025, 12000, 8000}, {22050, 24000, 16000}, {44100, 48000, 32000}};
  static const uint16_t KBPS[5][15] = {
      {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},  // MPEG-1 I
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},     // MPEG-1 II
      {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},      // MPEG-1 III
      {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},     // MPEG-2/2.5 I
      {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},          // MPEG-2/2.5 II, III
  };
  out->mpeg1 = version == 3;
  out->mono = (h[3] >> 6) == 3;
  out->rate = RATES[version == 3 ? 2 : (version == 2 ? 1 : 0)][rate];
  const int table = out->mpeg1 ? 3 - layer : (layer == 3 ? 3 : 4);
  out->kbps = KBPS[table][bitrate];
  out->samples = layer == 3 ? 384 : (layer == 2 || out->mpeg1 ? 1152 : 576);
  return true;
}

// A FLAC frame header at `h` (RFC 9639 9.1): its CRC-8 right, the coded number out (a frame's
// number with a fixed block size, a sample's with a variable one).
static bool flac_frame_header(const uint8_t *h, size_t avail, uint64_t *number, bool *variable) {
  if (avail < 6 || h[0] != 0xFF || (h[1] & 0xFE) != 0xF8)
    return false;
  const uint8_t block = h[2] >> 4, rate = h[2] & 0x0F;
  if (block == 0 || rate == 15 || (h[3] >> 4) > 10 || ((h[3] >> 1) & 7) == 3 || (h[3] & 1) != 0)
    return false;
  size_t len = 0;
  uint64_t value = h[4];
  if ((h[4] & 0x80) == 0) {
    len = 1;
  } else if ((h[4] & 0xE0) == 0xC0) {
    len = 2, value &= 0x1F;
  } else if ((h[4] & 0xF0) == 0xE0) {
    len = 3, value &= 0x0F;
  } else if ((h[4] & 0xF8) == 0xF0) {
    len = 4, value &= 0x07;
  } else if ((h[4] & 0xFC) == 0xF8) {
    len = 5, value &= 0x03;
  } else if ((h[4] & 0xFE) == 0xFC) {
    len = 6, value &= 0x01;
  } else if (h[4] == 0xFE) {
    len = 7, value = 0;
  } else {
    return false;
  }
  size_t at = 4 + len;
  if (avail < at + 5)
    return false;
  for (size_t i = 5; i < 4 + len; i++) {
    if ((h[i] & 0xC0) != 0x80)
      return false;
    value = (value << 6) | (h[i] & 0x3F);
  }
  at += block == 6 ? 1 : (block == 7 ? 2 : 0);
  at += rate == 12 ? 1 : (rate == 13 || rate == 14 ? 2 : 0);
  uint8_t crc = 0;  // CRC-8, polynomial x^8 + x^2 + x + 1
  for (size_t i = 0; i < at; i++) {
    crc ^= h[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) != 0 ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
  }
  if (crc != h[at])
    return false;
  *number = value;
  *variable = (h[1] & 1) != 0;
  return true;
}

size_t SimpleVideoPlayer::audio_file_prepare_(BufferedFileReader *reader, uint32_t seek_ms, uint8_t *head,
                                              uint64_t *from, uint32_t *base_ms) {
  AudioFileLayout &file = this->audio_layout_;
  file = AudioFileLayout{};
  *from = 0;
  *base_ms = 0;
  if (!reader->get_size(&file.size))
    return 0;
  // A window over the file in the temp buffer: bytes at a place, read there when not in it.
  uint8_t *const buf = this->audio_temp_buffer_;
  uint64_t win = 0;
  size_t win_len = 0;
  auto at = [&](uint64_t pos, size_t len) -> const uint8_t * {
    if (len > AUDIO_TEMP_BUFFER_SIZE || pos + len > file.size)
      return nullptr;
    if (pos < win || pos + len > win + win_len) {
      if (pos != win + win_len && !reader->seek(pos))
        return nullptr;
      const int n = reader->read(buf, AUDIO_TEMP_BUFFER_SIZE);
      win = pos;
      win_len = n > 0 ? static_cast<size_t>(n) : 0;
      if (len > win_len)
        return nullptr;
    }
    return buf + (pos - win);
  };
  const audio::AudioFileType type = audio::detect_audio_file_type(nullptr, this->video_path_.c_str());
  size_t head_len = 0;
  switch (type) {
#ifdef USE_AUDIO_MP3_SUPPORT
    case audio::AudioFileType::MP3: {
      const uint8_t *p = at(0, 10);
      if (p == nullptr)
        break;
      uint64_t pos = id3v2_size(p, 10);
      // The first frame: its header right, and the next one where it says.
      MpegHeader mh;
      bool found = false;
      for (size_t i = 0; i < 4096 && !found; i++, pos++) {
        const uint8_t *h = at(pos, 4);
        if (h == nullptr)
          break;
        found = mpeg_header(h, &mh);
      }
      if (!found)
        break;
      file.data_start = --pos;
      file.data_end = file.size;
      if (file.size >= 128) {
        const uint8_t *tag = at(file.size - 128, 3);
        if (tag != nullptr && tag[0] == 'T' && tag[1] == 'A' && tag[2] == 'G')
          file.data_end = file.size - 128;  // ID3v1 at the end
      }
      // Xing/Info after the side information, VBRI 32 bytes in: the frame count, bytes and TOC.
      const size_t xing = 4 + (mh.mpeg1 ? (mh.mono ? 17 : 32) : (mh.mono ? 9 : 17));
      uint32_t frames = 0;
      const uint8_t *x = at(file.data_start + xing, 120);
      if (x == nullptr)
        x = at(file.data_start + xing, 8);
      if (x != nullptr && (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0)) {
        const uint32_t flags = be32(x + 4);
        size_t field = 8;
        const uint8_t *more = at(file.data_start + xing, 8 + 4 + 4 + 100);
        if (more != nullptr) {
          if ((flags & 1) != 0) {
            frames = be32(more + field);
            field += 4;
          }
          if ((flags & 2) != 0) {
            file.xing_bytes = be32(more + field);
            field += 4;
          }
          if ((flags & 4) != 0) {
            memcpy(file.xing_toc, more + field, sizeof(file.xing_toc));
            file.toc = true;
          }
        }
      } else {
        const uint8_t *v = at(file.data_start + 36, 18);
        if (v != nullptr && memcmp(v, "VBRI", 4) == 0) {
          file.xing_bytes = be32(v + 10);
          frames = be32(v + 14);
        }
      }
      if (frames != 0 && mh.rate != 0) {
        file.duration_ms = static_cast<uint32_t>(static_cast<uint64_t>(frames) * mh.samples * 1000 / mh.rate);
      } else if (mh.kbps != 0) {
        file.duration_ms = static_cast<uint32_t>((file.data_end - file.data_start) * 8 / mh.kbps);  // CBR
      }
      *from = file.data_start;  // the decoder finds the frames' sync itself, past any tag
      if (seek_ms != 0 && file.duration_ms != 0) {
        const float part = std::min(static_cast<float>(seek_ms) / file.duration_ms, 1.0f);
        const uint64_t bytes = file.xing_bytes != 0 ? file.xing_bytes : file.data_end - file.data_start;
        float spot = part;
        if (file.toc) {
          const float percent = part * 100.0f;
          const int i = std::min(static_cast<int>(percent), 99);
          const float a = file.xing_toc[i];
          const float b = i < 99 ? file.xing_toc[i + 1] : 256.0f;
          spot = (a + (b - a) * (percent - i)) / 256.0f;
        }
        *from = std::min<uint64_t>(file.data_start + static_cast<uint64_t>(spot * bytes), file.data_end);
        *base_ms = seek_ms;
      }
      break;
    }
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
    case audio::AudioFileType::FLAC: {
      const uint8_t *p = at(0, 4);
      if (p == nullptr || memcmp(p, "fLaC", 4) != 0)
        break;
      // The metadata blocks: STREAMINFO kept, the rest (pictures, tags) passed over.
      uint64_t pos = 4;
      bool last = false, info = false;
      for (int blocks = 0; !last && blocks < 128; blocks++) {
        const uint8_t *b = at(pos, 4);
        if (b == nullptr)
          break;
        last = (b[0] & 0x80) != 0;
        const uint32_t len = (b[1] << 16) | (b[2] << 8) | b[3];
        if ((b[0] & 0x7F) == 0 && len >= 34) {
          const uint8_t *si = at(pos + 4, 34);
          if (si != nullptr) {
            memcpy(file.streaminfo, si, sizeof(file.streaminfo));
            info = true;
          }
        }
        pos += 4 + len;
      }
      if (!last || !info || pos >= file.size)
        break;
      const uint8_t *si = file.streaminfo;
      file.flac_rate = (static_cast<uint32_t>(si[10]) << 12) | (si[11] << 4) | (si[12] >> 4);
      const uint64_t total = (static_cast<uint64_t>(si[13] & 0x0F) << 32) | be32(si + 14);
      const uint16_t min_block = (si[0] << 8) | si[1], max_block = (si[2] << 8) | si[3];
      file.flac_block = min_block == max_block ? min_block : 0;
      file.data_start = pos;
      file.data_end = file.size;
      if (file.flac_rate == 0)
        break;
      if (total != 0)
        file.duration_ms = static_cast<uint32_t>(total * 1000 / file.flac_rate);
      // The decoder takes "fLaC" and STREAMINFO alone (the last block), then the frames.
      memcpy(head, "fLaC", 4);
      head[4] = 0x80;
      head[5] = 0;
      head[6] = 0;
      head[7] = 34;
      memcpy(head + 8, file.streaminfo, 34);
      head_len = 42;
      *from = file.data_start;
      if (seek_ms != 0 && file.duration_ms != 0) {
        // Near the place by the bytes, then the next frame (its header and CRC right) and its sample.
        const float part = std::min(static_cast<float>(seek_ms) / file.duration_ms, 1.0f);
        uint64_t scan = file.data_start + static_cast<uint64_t>(part * (file.data_end - file.data_start));
        static constexpr uint64_t SCAN_LIMIT = 1024 * 1024;
        const uint64_t scan_end = std::min(file.data_end, scan + SCAN_LIMIT);
        bool found = false;
        while (!found && scan + 16 < scan_end) {
          const size_t len = static_cast<size_t>(std::min<uint64_t>(AUDIO_TEMP_BUFFER_SIZE, scan_end - scan));
          const uint8_t *w = at(scan, len);
          if (w == nullptr)
            break;
          for (size_t i = 0; i + 16 <= len && !found; i++) {
            uint64_t number;
            bool variable;
            if (w[i] == 0xFF && flac_frame_header(w + i, len - i, &number, &variable) &&
                (variable || file.flac_block != 0)) {
              const uint64_t sample = variable ? number : number * file.flac_block;
              if (sample < total || total == 0) {
                *from = scan + i;
                *base_ms = static_cast<uint32_t>(sample * 1000 / file.flac_rate);
                found = true;
              }
            }
          }
          scan += len - 15;  // a header across the window's end is looked at again
        }
      }
      break;
    }
#endif
#ifdef USE_AUDIO_WAV_SUPPORT
    case audio::AudioFileType::WAV: {
      const uint8_t *p = at(0, 12);
      if (p == nullptr || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
        break;
      uint64_t pos = 12;
      bool data = false;
      for (int chunks = 0; !data && chunks < 64; chunks++) {
        const uint8_t *c = at(pos, 8);
        if (c == nullptr)
          break;
        const uint32_t len = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
          if (len < 16 || len > sizeof(file.fmt) || (len & 1) != 0)
            break;
          const uint8_t *f = at(pos + 8, len);
          if (f == nullptr)
            break;
          memcpy(file.fmt, f, len);
          file.fmt_len = static_cast<uint8_t>(len);
          file.byte_rate = le32(f + 8);
          file.block_align = static_cast<uint16_t>(le16(f + 12));
        } else if (memcmp(c, "data", 4) == 0) {
          file.data_start = pos + 8;
          file.data_end =
              len == 0 || len == 0xFFFFFFFFu || file.data_start + len > file.size ? file.size : file.data_start + len;
          data = true;
          break;
        }
        pos += 8 + len + (len & 1);
      }
      if (!data || file.fmt_len == 0 || file.byte_rate == 0 || file.block_align == 0)
        break;
      file.duration_ms = static_cast<uint32_t>((file.data_end - file.data_start) * 1000 / file.byte_rate);
      // At the place asked for, on a whole block; the decoder takes a header saying what is left.
      uint64_t offset = file.data_start;
      if (seek_ms != 0) {
        const uint64_t bytes = static_cast<uint64_t>(seek_ms) * file.byte_rate / 1000;
        offset = std::min(file.data_start + bytes / file.block_align * file.block_align, file.data_end);
      }
      const uint32_t left = static_cast<uint32_t>(file.data_end - offset);
      memcpy(head, "RIFF", 4);
      put_le32(head + 4, 4 + 8 + file.fmt_len + 8 + left);
      memcpy(head + 8, "WAVEfmt ", 8);
      put_le32(head + 16, file.fmt_len);
      memcpy(head + 20, file.fmt, file.fmt_len);
      memcpy(head + 20 + file.fmt_len, "data", 4);
      put_le32(head + 24 + file.fmt_len, left);
      head_len = 28 + file.fmt_len;
      *from = offset;
      *base_ms = static_cast<uint32_t>((offset - file.data_start) * 1000 / file.byte_rate);
      break;
    }
#endif
#ifdef USE_AUDIO_AAC_SUPPORT
    case audio::AudioFileType::AAC: {
      // ADTS only (an .m4a's MP4 boxes have no frame sync): the frames' mean size in the first read.
      const uint8_t *p = at(0, 10);
      if (p == nullptr)
        break;
      const uint64_t start = id3v2_size(p, 10);
      static const uint32_t ADTS_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                              22050, 16000, 12000, 11025, 8000,  7350};
      uint64_t pos = start;
      uint64_t bytes = 0;
      uint32_t frames = 0, rate = 0;
      while (frames < 64) {
        const uint8_t *h = at(pos, 7);
        if (h == nullptr || h[0] != 0xFF || (h[1] & 0xF6) != 0xF0 || ((h[2] >> 2) & 0x0F) >= 13)
          break;
        const uint32_t len = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
        if (len < 7)
          break;
        rate = ADTS_RATES[(h[2] >> 2) & 0x0F];
        frames += (h[6] & 3) + 1;  // raw data blocks of 1024 samples each
        bytes += len;
        pos += len;
      }
      if (frames < 8 || rate == 0)
        break;
      file.data_start = start;
      file.data_end = file.size;
      const uint64_t per_second = bytes * rate / (static_cast<uint64_t>(frames) * 1024);
      if (per_second != 0)
        file.duration_ms = static_cast<uint32_t>((file.data_end - file.data_start) * 1000 / per_second);
      *from = file.data_start;  // the decoder finds the next frame's sync itself
      if (seek_ms != 0 && file.duration_ms != 0) {
        const float part = std::min(static_cast<float>(seek_ms) / file.duration_ms, 1.0f);
        *from = file.data_start + static_cast<uint64_t>(part * (file.data_end - file.data_start));
        *base_ms = seek_ms;
      }
      break;
    }
#endif
    default:
      break;  // no layout read (Ogg Opus): played from its start, no duration, no seeking
  }
  if (file.duration_ms == 0) {
    // Not read: the whole file from its start, as it is.
    *from = 0;
    *base_ms = 0;
    head_len = 0;
  }
  ESP_LOGD(TAG, "%s: %" PRIu32 " ms, audio from byte %" PRIu64, this->video_path_.c_str(), file.duration_ms, *from);
  return head_len;
}

// Audio task: decodes the track into the file speaker; its end (all of the file decoded and handed
// on) ends the session, and the main loop goes on with the queue.
void SimpleVideoPlayer::audio_file_loop_() {
  static constexpr uint32_t MAX_CONSECUTIVE_FAILURES = 10;
  uint32_t failures = 0;
  bool linked = false;
  bool finished = false;
  // The decoder's count goes on over the tracks: this one's from here.
  const uint32_t played_before = this->audio_decoder_->get_playback_ms();
  esp_task_wdt_add(nullptr);
  while (!this->audio_task_stop_) {
    esp_task_wdt_reset();
    const PlayerState state = this->state_.load(std::memory_order_acquire);
    if (state == PlayerState::PAUSED) {
      esp_task_wdt_delete(nullptr);
      this->audio_wait_.wait(
          [this]() {
            return this->audio_task_stop_ || this->state_.load(std::memory_order_acquire) != PlayerState::PAUSED;
          },
          portMAX_DELAY);
      esp_task_wdt_add(nullptr);
      continue;
    }
    if (state != PlayerState::PLAYING)
      break;
    const audio::AudioDecoderState result =
        this->audio_decoder_->decode(this->audio_file_eof_.load(std::memory_order_acquire));
    this->position_frame_.store(this->audio_position_base_ms_.load(std::memory_order_acquire) +
                                    (this->audio_decoder_->get_playback_ms() - played_before),
                                std::memory_order_relaxed);
    if (!linked && this->audio_decoder_->get_audio_stream_info().has_value()) {
      const audio::AudioStreamInfo &info = this->audio_decoder_->get_audio_stream_info().value();
      if (this->file_speaker_open_ && info != this->file_speaker_info_)
        this->file_speaker_->stop();  // another format: it starts again in this one
      this->file_speaker_->set_audio_stream_info(info);
      this->file_speaker_info_ = info;
      this->audio_decoder_->add_sink(this->file_speaker_);
      this->file_speaker_open_ = true;
      linked = true;
    }
    if (result == audio::AudioDecoderState::FINISHED) {
      finished = true;
      break;
    }
    if (result == audio::AudioDecoderState::FAILED) {
      if (++failures >= MAX_CONSECUTIVE_FAILURES) {
        ESP_LOGE(TAG, "Cannot decode %s", this->video_path_.c_str());
        this->set_error_(PlaybackError::DECODE_ERROR);
        break;
      }
      continue;
    }
    failures = 0;
  }
  esp_task_wdt_delete(nullptr);
  if (finished) {
    // The next track goes on through the open speaker; the main loop finishes it at the end.
    PlayerState expected = this->state_.load(std::memory_order_acquire);
    while ((expected == PlayerState::PLAYING || expected == PlayerState::PAUSED) &&
           !this->state_.compare_exchange_weak(expected, PlayerState::STOPPED, std::memory_order_acq_rel)) {
    }
    if (expected == PlayerState::PLAYING || expected == PlayerState::PAUSED) {
      if (expected == PlayerState::PAUSED)
        this->file_speaker_->set_pause_state(false);
      this->audio_advance_req_.store(true, std::memory_order_release);
      this->finished_pending_.store(true, std::memory_order_release);  // on_playback_finished, from loop()
      this->note_state_();
      return;
    }
    // Stopped (or failed) meanwhile: the speaker as after a stop.
  }
  if (this->file_speaker_open_) {
    this->file_speaker_->set_pause_state(false);
    this->file_speaker_->stop();
    this->file_speaker_open_ = false;
  }
}
#endif
#endif

//========================================================================
// Error Handling
//========================================================================

void SimpleVideoPlayer::set_error_(PlaybackError error) {
  // Publish last_error_ before state_ so any reader that sees ERROR also sees the reason.
  this->last_error_.store(error, std::memory_order_relaxed);
  this->state_.store(PlayerState::ERROR, std::memory_order_release);
  this->note_state_();
  // Mostly the playback task: the automations run from loop(), on the main loop.
  this->error_pending_.store(static_cast<int16_t>(error), std::memory_order_release);
}

}  // namespace esphome::simple_video_player
