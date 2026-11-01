#include "avi_media_player.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"

#include <cinttypes>

namespace esphome::avi {

static const char *const TAG = "avi.media_player";

static constexpr size_t ALIGN = 128;
static constexpr uint32_t FLIP_WAIT_MS = 100;

static constexpr size_t SOUND_RING = 64 * 1024;
static constexpr size_t SOUND_DECODER_IN = 64 * 1024;
static constexpr size_t SOUND_DECODER_OUT = 32 * 1024;

#ifdef USE_SPEAKER
class PsramRing : public ring_buffer::RingBuffer {
 public:
  ~PsramRing() {
    if (this->created_)
      vRingbufferDelete(this->handle_());
  }
  static std::shared_ptr<ring_buffer::RingBuffer> make(uint8_t *storage, size_t len) {
    auto ring = std::make_shared<PsramRing>();
    if (xRingbufferCreateStatic(len, RINGBUF_TYPE_BYTEBUF, storage, &ring->structure_) != ring->handle_())
      return nullptr;
    ring->created_ = true;
    return ring;
  }

 protected:
  bool created_{false};
};
#endif

static uint8_t *psram_piece(size_t size) {
  return static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, (size + ALIGN - 1) & ~(ALIGN - 1),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void AviMediaPlayer::setup() {
  this->own_canvas_.set_display(this->display_);
  this->own_output_.add_canvas(&this->own_canvas_);
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
  this->disable_loop();
}

void AviMediaPlayer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "AVI media player:\n"
                "  Read-ahead: %zu bytes\n"
                "  Frame: %zu bytes",
                this->ring_size_, this->frame_size_);
}

media_player::MediaPlayerTraits AviMediaPlayer::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.set_supports_pause(true);
  return traits;
}

void AviMediaPlayer::play_index_(size_t index) {
  if (index >= this->list_.size())
    return;
  this->index_ = index;
  if (this->run_.load() != RUN_IDLE) {
    this->pending_ = static_cast<int>(index);
    this->advance_ = false;
    this->stop_();
    return;
  }
  this->advance_ = true;
  this->stream_source_ = false;
  this->start_(this->list_[index]);
}

void AviMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  const bool enqueue =
      call.get_command().has_value() && call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_ENQUEUE;
  if (call.get_media_url().has_value()) {
    if (enqueue) {
      this->list_.push_back(call.get_media_url().value());
      if (this->run_.load() == RUN_IDLE && this->pending_ < 0)
        this->play_index_(this->list_.size() - 1);
      return;
    }
    this->list_.clear();
    this->list_.push_back(call.get_media_url().value());
    this->play_index_(0);
    return;
  }
  if (!call.get_command().has_value())
    return;
  switch (call.get_command().value()) {
    case media_player::MEDIA_PLAYER_COMMAND_NEXT:
      this->play_index_(this->index_ + 1);
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PREVIOUS:
      this->play_index_(this->index_ > 0 ? this->index_ - 1 : 0);
      break;
    case media_player::MEDIA_PLAYER_COMMAND_CLEAR_PLAYLIST:
      this->list_.clear();
      this->index_ = 0;
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PLAY:
      if (this->run_.load() == RUN_PAUSED) {
        this->run_.store(RUN_PLAYING);
        xTaskNotifyGive(this->task_handle_);
#ifdef USE_SPEAKER
        if (this->sound_task_ != nullptr)
          xTaskNotifyGive(this->sound_task_);
#endif
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        this->publish_state();
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
      if (this->run_.load() == RUN_PLAYING) {
        this->run_.store(RUN_PAUSED);
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        this->publish_state();
      }
      break;
    case media_player::MEDIA_PLAYER_COMMAND_TOGGLE: {
      const uint8_t run = this->run_.load();
      if (run == RUN_PLAYING || run == RUN_PAUSED) {
        auto next = this->make_call();
        next.set_command(run == RUN_PLAYING ? media_player::MEDIA_PLAYER_COMMAND_PAUSE
                                            : media_player::MEDIA_PLAYER_COMMAND_PLAY);
        next.perform();
      }
      break;
    }
    case media_player::MEDIA_PLAYER_COMMAND_STOP:
      this->advance_ = false;
      this->pending_ = -1;
      this->stop_();
      break;
    default:
      break;
  }
}

bool AviMediaPlayer::start_(const std::string &path) {
  if (this->run_.load() != RUN_IDLE) {
    ESP_LOGW(TAG, "Not starting %s: playback still ends", path.c_str());
    return false;
  }
  if (this->display_ == nullptr) {
    ESP_LOGE(TAG, "No display");
    return false;
  }
  if (this->holds_()) {
    ESP_LOGW(TAG, "Not starting %s: the last canvas picture is still shown", path.c_str());
    return false;
  }
  this->path_ = path;
  this->ring_ = psram_piece(this->ring_size_);
  this->frame_ = psram_piece(this->frame_size_);
  if (this->ring_ == nullptr || this->frame_ == nullptr) {
    ESP_LOGE(TAG, "No memory for %s (largest PSRAM block %zu)", path.c_str(),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    this->release_();
    return false;
  }
  esp_timer_create_args_t pace{};
  pace.callback = &AviMediaPlayer::pace_cb_;
  pace.arg = this;
  pace.dispatch_method = ESP_TIMER_TASK;
  pace.name = "avi_pace";
  if (esp_timer_create(&pace, &this->pace_timer_) != ESP_OK) {
    this->pace_timer_ = nullptr;
    this->release_();
    return false;
  }
  this->abort_ = false;
  this->ended_.store(false);
  this->frame_failed_ = false;
  this->run_.store(RUN_STARTING);
  if (xTaskCreatePinnedToCoreWithCaps(&AviMediaPlayer::task_entry_, "avi_play", 8192, this, 1, &this->task_handle_, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->task_handle_ = nullptr;
    this->run_.store(RUN_IDLE);
    this->release_();
    return false;
  }
  this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
  this->publish_state();
  this->enable_loop();
  return true;
}

bool AviMediaPlayer::play_stream(void (*room)(void *), void *arg) {
  if (this->run_.load() != RUN_IDLE)
    return false;
  this->list_.clear();
  this->index_ = 0;
  this->advance_ = false;
  this->pending_ = -1;
  this->stream_source_ = true;
  this->room_cb_ = room;
  this->room_arg_ = arg;
  return this->start_("network stream");
}

void AviMediaPlayer::stop_() {
  const uint8_t run = this->run_.load();
  if (run == RUN_IDLE || run == RUN_STOPPING)
    return;
  this->run_.store(RUN_STOPPING);
  this->abort_ = true;
  xTaskNotifyGive(this->task_handle_);
#ifdef USE_SPEAKER
  if (this->sound_task_ != nullptr)
    xTaskNotifyGive(this->sound_task_);
#endif
}

void AviMediaPlayer::loop() {
  this->output_->loop();
  if (this->reader_ != nullptr)
    this->reader_->service();
  if (this->ended_.exchange(false))
    this->end_();
  if (this->run_.load() == RUN_IDLE && !this->holds_())
    this->disable_loop();
}

void AviMediaPlayer::end_() {
  this->release_();
  if (this->task_handle_ != nullptr) {
    vTaskDeleteWithCaps(this->task_handle_);
    this->task_handle_ = nullptr;
  }
  this->run_.store(RUN_IDLE);
  const int pending = this->pending_;
  this->pending_ = -1;
  if (pending >= 0) {
    this->play_index_(static_cast<size_t>(pending));
  } else if (this->advance_ && this->index_ + 1 < this->list_.size()) {
    this->play_index_(this->index_ + 1);
  }
  if (this->run_.load() != RUN_IDLE)
    return;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
  this->publish_state();
  if (!this->holds_())
    this->disable_loop();
}

void AviMediaPlayer::release_() {
#ifdef USE_SPEAKER
  if (this->sound_task_ != nullptr) {
    vTaskDeleteWithCaps(this->sound_task_);
    this->sound_task_ = nullptr;
  }
  this->sound_decoder_.reset();
  this->sound_ring_.reset();
  heap_caps_free(this->sound_mem_);
  this->sound_mem_ = nullptr;
#endif
  this->sink_.store(nullptr);
  this->parser_.reset();
  this->reader_.reset();
  if (this->pace_timer_ != nullptr) {
    esp_timer_stop(this->pace_timer_);
    esp_timer_delete(this->pace_timer_);
    this->pace_timer_ = nullptr;
  }
  if (this->holds_())
    this->canvas_->close();
  heap_caps_free(this->ring_);
  this->ring_ = nullptr;
  heap_caps_free(this->frame_);
  this->frame_ = nullptr;
}

void AviMediaPlayer::task_entry_(void *arg) {
  auto *player = static_cast<AviMediaPlayer *>(arg);
  player->task_();
  player->ended_.store(true);
  App.wake_loop_threadsafe();
  while (true)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void AviMediaPlayer::pace_cb_(void *arg) { xTaskNotifyGive(static_cast<AviMediaPlayer *>(arg)->task_handle_); }

void AviMediaPlayer::wait_until_(int64_t when_us) {
  const int64_t now = esp_timer_get_time();
  if (when_us <= now)
    return;
  ulTaskNotifyTake(pdTRUE, 0);
  esp_timer_stop(this->pace_timer_);
  esp_timer_start_once(this->pace_timer_, static_cast<uint64_t>(when_us - now));
  while (esp_timer_get_time() < when_us && this->run_.load() == RUN_PLAYING)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

bool AviMediaPlayer::open_() {
  this->reader_ = std::make_unique<BufferedFileReader>();
  this->reader_->set_abort_flag(&this->abort_);
  this->reader_->set_ring(this->ring_, this->ring_size_);
  if (this->stream_source_) {
    if (this->room_cb_ != nullptr)
      this->reader_->set_room_callback(this->room_cb_, this->room_arg_);
    if (!this->reader_->open_stream()) {
      ESP_LOGE(TAG, "The network stream not opened");
      return false;
    }
    this->sink_.store(this->reader_.get());
    if (this->room_cb_ != nullptr)
      this->room_cb_(this->room_arg_);
  } else if (!this->reader_->open(this->path_.c_str())) {
    ESP_LOGE(TAG, "%s not opened", this->path_.c_str());
    return false;
  }
  this->parser_ = std::make_unique<AVIParser>();
  if (!this->parser_->open(this->reader_.get())) {
    ESP_LOGE(TAG, "%s is no AVI", this->path_.c_str());
    return false;
  }
  const AVIStreamInfo *video = this->parser_->get_video_info();
  if (video == nullptr || video->fps_num == 0) {
    ESP_LOGE(TAG, "%s has no video stream", this->path_.c_str());
    return false;
  }
  this->frame_us_ = static_cast<uint32_t>(1000000ull * video->fps_den / video->fps_num);
  const uint32_t fps = (video->fps_num + video->fps_den - 1) / video->fps_den;
  this->canvas_ = this->output_->open(video->width, video->height, fps, false, this);
  if (this->canvas_ == nullptr) {
    ESP_LOGE(TAG, "%s is %" PRIu32 "x%" PRIu32 " at %" PRIu32 " fps: no canvas or screen output takes it",
             this->path_.c_str(), video->width, video->height, fps);
    return false;
  }
  this->pcm_ = false;
#ifdef USE_SPEAKER
  const AVIStreamInfo *audio = this->parser_->get_audio_info();
  if (this->speaker_ != nullptr && audio != nullptr) {
    if (static_cast<AVIAudioCodec>(audio->codec) == AVIAudioCodec::PCM && audio->bits_per_sample == 16) {
      this->speaker_->set_audio_stream_info(
          audio::AudioStreamInfo(16, static_cast<uint8_t>(audio->channels), audio->sample_rate));
      this->pcm_ = true;
    } else if (!this->sound_start_(audio)) {
      ESP_LOGW(TAG, "%s: its sound (codec 0x%04" PRIX32 ") is not played", this->path_.c_str(), audio->codec);
    }
  }
#endif
  return true;
}

void AviMediaPlayer::task_() {
  if (!this->open_())
    return;
  this->run_.store(RUN_PLAYING);
  this->played_to_end_.store(false);
  this->play_();
  this->played_to_end_.store(this->run_.load() == RUN_PLAYING);
  if (this->canvas_ != nullptr)
    this->canvas_->stop_feed();
#ifdef USE_SPEAKER
  this->sound_stop_();
#endif
}

void AviMediaPlayer::play_() {
  const bool sound = this->pcm_
#ifdef USE_SPEAKER
                     || this->sound_ring_ != nullptr
#endif
      ;
  int64_t due = esp_timer_get_time() + (sound ? this->audio_latency_us_ : 0);
  while (true) {
    uint8_t run = this->run_.load();
    if (run == RUN_PAUSED && this->canvas_ != nullptr)
      this->canvas_->stop_feed();
    while (run == RUN_PAUSED) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      run = this->run_.load();
      due = esp_timer_get_time();
    }
    if (run != RUN_PLAYING)
      return;
    AVIFrame frame{};
    int n = this->parser_->read_next_frame(frame, this->frame_, this->frame_size_);
    while (n > 0 && frame.stream_type != AVIStreamType::VIDEO) {
      if (frame.stream_type == AVIStreamType::AUDIO && this->pcm_)
        this->sound_(this->frame_, static_cast<size_t>(n));
#ifdef USE_SPEAKER
      if (frame.stream_type == AVIStreamType::AUDIO && this->sound_ring_ != nullptr)
        this->sound_chunk_(this->frame_, static_cast<size_t>(n));
#endif
      n = this->parser_->read_next_frame(frame, this->frame_, this->frame_size_);
    }
    if (n == AVIParser::REPEAT_FRAME) {
      due += this->frame_us_;
      this->wait_until_(due);
      continue;
    }
    if (n <= 0)
      return;
    this->wait_until_(due);
    due += this->frame_us_;
    const bool shown = this->frame_(this->frame_, static_cast<size_t>(n));
    if (!shown && !this->frame_failed_)
      ESP_LOGW(TAG, "Frames of %s not decoded", this->path_.c_str());
    this->frame_failed_ = !shown;
  }
}

bool AviMediaPlayer::frame_(const uint8_t *data, size_t len) {
  return this->canvas_ != nullptr && this->canvas_->frame(data, len, FLIP_WAIT_MS);
}

void AviMediaPlayer::sound_(const uint8_t *data, size_t len) {
#ifdef USE_SPEAKER
  while (len > 0 && this->run_.load() == RUN_PLAYING) {
    const size_t written = this->speaker_->play(data, len, pdMS_TO_TICKS(20));
    data += written;
    len -= written;
  }
#endif
}


#ifdef USE_SPEAKER
bool AviMediaPlayer::sound_start_(const AVIStreamInfo *info) {
  audio::AudioFileType type = audio::AudioFileType::NONE;
  switch (static_cast<AVIAudioCodec>(info->codec)) {
#ifdef USE_AUDIO_MP3_SUPPORT
    case AVIAudioCodec::MP3:
      type = audio::AudioFileType::MP3;
      break;
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
    case AVIAudioCodec::FLAC:
      if (info->stream_header_len == 0)
        return false;
      type = audio::AudioFileType::FLAC;
      break;
#endif
    default:
      return false;
  }
  this->sound_mem_ = psram_piece(SOUND_RING);
  if (this->sound_mem_ == nullptr)
    return false;
  this->sound_ring_ = PsramRing::make(this->sound_mem_, SOUND_RING);
  this->sound_decoder_ = std::make_unique<audio::AudioDecoder>(SOUND_DECODER_IN, SOUND_DECODER_OUT);
  if (this->sound_ring_ == nullptr)
    return false;
  std::weak_ptr<ring_buffer::RingBuffer> source = this->sound_ring_;
  if (this->sound_decoder_->add_source(source) != ESP_OK || this->sound_decoder_->add_sink(this->speaker_) != ESP_OK)
    return false;
  this->speaker_->set_audio_stream_info(
      audio::AudioStreamInfo(16, static_cast<uint8_t>(info->channels), info->sample_rate));
  if (this->sound_decoder_->start(type) != ESP_OK)
    return false;
  if (info->stream_header_len != 0 && static_cast<AVIAudioCodec>(info->codec) == AVIAudioCodec::FLAC)
    this->sound_ring_->write(info->stream_header, info->stream_header_len);
  this->sound_end_.store(false);
  this->sound_eof_.store(false);
  this->sound_done_.store(false);
  this->sound_waiter_ = nullptr;
  if (xTaskCreatePinnedToCoreWithCaps(&AviMediaPlayer::sound_entry_, "avi_sound", 4096, this, 1, &this->sound_task_, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->sound_task_ = nullptr;
    return false;
  }
  return true;
}

void AviMediaPlayer::sound_chunk_(const uint8_t *data, size_t len) {
  while (len > 0 && this->run_.load() == RUN_PLAYING) {
    const size_t written = this->sound_ring_->write_without_replacement(data, len, pdMS_TO_TICKS(20));
    data += written;
    len -= written;
  }
}

void AviMediaPlayer::sound_stop_() {
  if (this->sound_task_ == nullptr)
    return;
  if (this->run_.load() == RUN_PLAYING) {
    this->sound_eof_.store(true);
  } else {
    this->sound_end_.store(true);
  }
  this->sound_waiter_ = xTaskGetCurrentTaskHandle();
  xTaskNotifyGive(this->sound_task_);
  while (!this->sound_done_.load())
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void AviMediaPlayer::sound_entry_(void *arg) {
  auto *player = static_cast<AviMediaPlayer *>(arg);
  player->sound_loop_();
  player->sound_done_.store(true);
  if (player->sound_waiter_ != nullptr)
    xTaskNotifyGive(player->sound_waiter_);
  while (true)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void AviMediaPlayer::sound_loop_() {
  while (!this->sound_end_.load()) {
    if (this->run_.load() == RUN_PAUSED) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    const audio::AudioDecoderState state = this->sound_decoder_->decode(this->sound_eof_.load());
    if (state != audio::AudioDecoderState::DECODING)
      return;
  }
}
#endif

}

#endif
