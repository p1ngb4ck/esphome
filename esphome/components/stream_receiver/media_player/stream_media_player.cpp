#include "stream_media_player.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#if defined(USE_WIFI) && defined(USE_WIFI_RUNTIME_ROAMING_SUPPRESSION)
#define STREAM_HOLDS_ROAMING 1
#endif
#if defined(USE_WIFI) && defined(USE_WIFI_RUNTIME_POWER_SAVE)
#define STREAM_HOLDS_POWER_SAVE 1
#endif
#if defined(STREAM_HOLDS_ROAMING) || defined(STREAM_HOLDS_POWER_SAVE)
#include "esphome/components/wifi/wifi_component.h"
#endif

#include "esp_heap_caps.h"
#include "esp_vfs_eventfd.h"

#include <lwip/sockets.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace esphome::stream_receiver {

static const char *const TAG = "stream_receiver";

static constexpr uint8_t UDISP_TYPE_JPG = 3;
static constexpr uint8_t UDISP_TYPE_PCM = 0x10;
static constexpr uint8_t UDISP_TYPE_ECHO = 0x22;
static constexpr uint32_t ECHO_PENDING = 1u << 16;
static constexpr uint32_t SOUND_DEFAULT_RATE = 48000;
static constexpr uint32_t SOUND_BLOCK_MS = 10;
static constexpr size_t HEADER_BYTES = 16;
static constexpr size_t RECV_BYTES = 32768;
static constexpr size_t ALIGN = 128;
static constexpr uint32_t ROAM_QUIET_MS = 15000;
static constexpr int HEARTBEAT_BYTES = 16;
static constexpr uint32_t RECV_TIMEOUT_MS = 30000;
static constexpr uint32_t FRAME_WAIT_MS = 250;
static constexpr uint32_t DRAIN_WAIT_MS = 100;

static inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static uint8_t *psram_piece(size_t size) {
  return static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, (size + ALIGN - 1) & ~(ALIGN - 1),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void StreamMediaPlayer::setup() {
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
  const esp_vfs_eventfd_config_t eventfd_config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&eventfd_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "eventfd: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->wake_fd_ = eventfd(0, 0);
  this->empty_q_ = xQueueCreateStatic(FRAME_COUNT, sizeof(Frame *), reinterpret_cast<uint8_t *>(this->empty_slots_),
                                      &this->empty_store_);
  this->filled_q_ = xQueueCreateStatic(FRAME_COUNT, sizeof(Frame *), reinterpret_cast<uint8_t *>(this->filled_slots_),
                                       &this->filled_store_);
  if (this->wake_fd_ < 0 || this->empty_q_ == nullptr || this->filled_q_ == nullptr) {
    ESP_LOGE(TAG, "No room for the receiver's queues");
    this->mark_failed();
    return;
  }
#ifdef USE_STREAM_TOUCH
  if (this->touchscreen_ != nullptr) {
    this->touch_q_ =
        xQueueCreateStatic(8, sizeof(Touch), reinterpret_cast<uint8_t *>(this->touch_slots_), &this->touch_store_);
    if (this->touch_q_ != nullptr)
      this->touchscreen_->register_listener(&this->touch_listener_);
  }
#endif
  if (xTaskCreatePinnedToCoreWithCaps(&StreamMediaPlayer::draw_entry_, "stream_draw", 4096, this, 2, &this->draw_task_,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
      xTaskCreatePinnedToCoreWithCaps(&StreamMediaPlayer::net_entry_, "stream_net", 4096, this, 4, nullptr, 0,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "No room for the receiver's tasks");
    this->mark_failed();
    return;
  }
  this->disable_loop();
}

void StreamMediaPlayer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Stream receiver:\n"
                "  Port: %u\n"
                "  Frame: up to %zu bytes",
                this->port_, this->max_frame_bytes_);
}

media_player::MediaPlayerTraits StreamMediaPlayer::get_traits() {
  media_player::MediaPlayerTraits traits;
  traits.set_supports_pause(false);
  return traits;
}

void StreamMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!call.get_command().has_value() || call.get_command().value() != media_player::MEDIA_PLAYER_COMMAND_STOP)
    return;
  this->drop_.store(true);
  this->wake_();
#ifdef USE_STREAM_AVI
  if (this->avi_streaming_.load())
    this->avi_stop_.store(true);
  this->enable_loop();
#endif
}

void StreamMediaPlayer::loop() {
  if (this->output_ != nullptr)
    this->output_->loop();
#ifdef USE_STREAM_AVI
  if (this->avi_wanted_.exchange(false)) {
    if (!this->avi_->play_stream(&StreamMediaPlayer::room_cb_, this))
      this->avi_refused_.store(true);
    this->wake_();
  }
  if (this->avi_stop_.exchange(false)) {
    auto call = this->avi_->make_call();
    call.set_command(media_player::MEDIA_PLAYER_COMMAND_STOP);
    call.perform();
  }
#endif
  bool closing = this->close_page_.load();
#ifdef USE_STREAM_CANVAS
  closing = closing || this->reopen_.load();
#endif
  if (closing && !this->draw_busy_.load() && uxQueueMessagesWaiting(this->filled_q_) == 0) {
    this->close_page_.store(false);
    if (this->canvas_ != nullptr && this->canvas_->held_by(this))
      this->canvas_->close();
#ifdef USE_STREAM_CANVAS
    this->reopen_.store(false);
#endif
  }
  if (this->state_changed_.exchange(false)) {
    bool playing = this->connected_.load();
#ifdef USE_STREAM_AVI
    playing = playing || this->avi_streaming_.load();
#endif
    this->state = playing ? media_player::MEDIA_PLAYER_STATE_PLAYING : media_player::MEDIA_PLAYER_STATE_IDLE;
    this->publish_state();
  }
  const bool held = this->canvas_ != nullptr && this->canvas_->held_by(this);
  if (!this->connected_.load() && !this->close_page_.load() && !held)
    this->disable_loop();
}

bool StreamMediaPlayer::page_target_(uint32_t *width, uint32_t *height) const {
#ifdef USE_STREAM_CANVAS
  if (!this->fullscreen_.load()) {
    *width = this->canvas_w_;
    *height = this->canvas_h_;
    return true;
  }
#endif
  return this->output_ != nullptr && this->output_->page_size(width, height, this);
}

display::VideoCanvas *StreamMediaPlayer::open_target_() {
#ifdef USE_STREAM_CANVAS
  if (!this->fullscreen_.load())
    return this->output_->open_page_on(this->canvas_index_, this->canvas_w_, this->canvas_h_, this);
#endif
  return this->output_->open_page(this);
}

#ifdef USE_STREAM_CANVAS
void StreamMediaPlayer::set_fullscreen(bool fullscreen) {
  if (this->fullscreen_.exchange(fullscreen) == fullscreen)
    return;
  ESP_LOGI(TAG, "Streams are made for the %s", fullscreen ? "screen" : "canvas");
  uint32_t width = 0;
  uint32_t height = 0;
  if (!this->page_target_(&width, &height))
    width = height = 0;
  this->page_w_ = static_cast<uint16_t>(width);
  this->page_h_ = static_cast<uint16_t>(height);
  this->reopen_.store(true);
  this->view_pending_.store(true);
  this->wake_();
  this->enable_loop();
}
#endif

void StreamMediaPlayer::wake_() {
  if (this->wake_fd_ < 0)
    return;
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

void StreamMediaPlayer::room_cb_(void *arg) { static_cast<StreamMediaPlayer *>(arg)->wake_(); }

void StreamMediaPlayer::hold_wifi_(bool &held, bool want) {
#if defined(STREAM_HOLDS_ROAMING) || defined(STREAM_HOLDS_POWER_SAVE)
  if (held == want || wifi::global_wifi_component == nullptr)
    return;
  if (want) {
#ifdef STREAM_HOLDS_ROAMING
    wifi::global_wifi_component->request_roaming_suppression();
#endif
#ifdef STREAM_HOLDS_POWER_SAVE
    wifi::global_wifi_component->request_high_performance();
#endif
  } else {
#ifdef STREAM_HOLDS_ROAMING
    wifi::global_wifi_component->release_roaming_suppression();
#endif
#ifdef STREAM_HOLDS_POWER_SAVE
    wifi::global_wifi_component->release_high_performance();
#endif
  }
  held = want;
#else
  (void) held;
  (void) want;
#endif
}

bool StreamMediaPlayer::take_buffers_() {
  this->recv_buf_ = psram_piece(RECV_BYTES);
  if (this->recv_buf_ == nullptr)
    return false;
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->sound_block_size_ = static_cast<size_t>(this->sound_rate_) * SOUND_BLOCK_MS / 1000 *
                              this->sound_channels_ * sizeof(int16_t);
    this->sound_block_ = psram_piece(this->sound_block_size_);
    if (this->sound_block_ == nullptr)
      ESP_LOGW(TAG, "No memory for the %zu-byte sound block: the stream plays without sound",
               this->sound_block_size_);
  }
#endif
  return true;
}

void StreamMediaPlayer::give_buffers_() {
  xQueueReset(this->empty_q_);
  xQueueReset(this->filled_q_);
  for (auto &frame : this->frames_) {
    heap_caps_free(frame.data);
    frame = Frame{};
  }
  heap_caps_free(this->recv_buf_);
  this->recv_buf_ = nullptr;
#ifdef USE_SPEAKER
  heap_caps_free(this->sound_block_);
  this->sound_block_ = nullptr;
  this->sound_block_used_ = 0;
#endif
}

void StreamMediaPlayer::net_entry_(void *arg) { static_cast<StreamMediaPlayer *>(arg)->net_loop_(); }

void StreamMediaPlayer::net_loop_() {
  while (true) {
    int listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(this->port_);
    if (::bind(listener, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(listener, 1) < 0) {
      ESP_LOGE(TAG, "Could not listen on port %u: %s", this->port_, strerror(errno));
      ::close(listener);
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    while (true) {
      struct sockaddr_in peer = {};
      socklen_t peer_len = sizeof(peer);
      int client = ::accept(listener, reinterpret_cast<struct sockaddr *>(&peer), &peer_len);
      if (client < 0)
        break;
      ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      ::setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
      char peer_text[16] = {};
      ::inet_ntoa_r(peer.sin_addr, peer_text, sizeof(peer_text));
      if (!this->take_buffers_()) {
        ESP_LOGW(TAG, "Sender from %s refused: no memory for the receive buffer", peer_text);
        this->give_buffers_();
        ::close(client);
        continue;
      }
      ESP_LOGI(TAG, "Sender connected from %s", peer_text);
      this->serve_(client);
      ::close(client);
    }
    ::close(listener);
  }
}

void StreamMediaPlayer::serve_(int client) {
  this->header_len_ = 0;
  this->skip_ = 0;
  this->current_ = nullptr;
  this->logged_bad_header_ = false;
  this->drop_.store(false);
  this->echo_.store(0);
  uint32_t width = 0;
  uint32_t height = 0;
  if (!this->page_target_(&width, &height))
    width = height = 0;
  this->page_w_ = static_cast<uint16_t>(width);
  this->page_h_ = static_cast<uint16_t>(height);
#ifdef USE_SPEAKER
  this->sound_left_ = 0;
  this->sound_carry_len_ = 0;
  this->sound_pending_.store(this->speaker_ != nullptr);
#endif
#ifdef USE_STREAM_TOUCH
  if (this->touch_q_ != nullptr)
    xQueueReset(this->touch_q_);
  this->last_touch_valid_ = false;
#endif
  this->view_pending_.store(true);
  this->status_pending_.store(true);
  this->connected_.store(true);
  this->state_changed_.store(true);
  this->enable_loop_soon_any_context();
  this->send_pending_(client);

  uint32_t last_recv = millis();
  uint32_t last_stream = last_recv;
  bool wifi_held = false;
  bool kind_known = false;
  [[maybe_unused]] bool sink_seen = false;
  [[maybe_unused]] bool sender_done = false;
  auto wait = [&](bool socket, uint32_t timeout_ms) -> int {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(this->wake_fd_, &readable);
    if (socket)
      FD_SET(client, &readable);
    struct timeval tv = {.tv_sec = static_cast<time_t>(timeout_ms / 1000),
                         .tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
    const int ready = ::select(std::max(client, this->wake_fd_) + 1, &readable, nullptr, nullptr, &tv);
    if (ready < 0)
      return -1;
    if (ready > 0 && FD_ISSET(this->wake_fd_, &readable)) {
      uint64_t count;
      ::read(this->wake_fd_, &count, sizeof(count));
    }
    return socket && ready > 0 && FD_ISSET(client, &readable) ? 1 : 0;
  };
  auto next_due_ms = [&]() -> uint32_t {
    const uint32_t now = millis();
    uint32_t due = RECV_TIMEOUT_MS - std::min<uint32_t>(now - last_recv, RECV_TIMEOUT_MS);
    if (wifi_held)
      due = std::min<uint32_t>(due, ROAM_QUIET_MS - std::min<uint32_t>(now - last_stream, ROAM_QUIET_MS));
    return due + 1;
  };
  while (!this->drop_.load()) {
#ifdef USE_STREAM_AVI
    avi::BufferedFileReader *sink = nullptr;
    if (this->avi_streaming_.load()) {
      sink = this->avi_->stream_sink();
      if (sink != nullptr) {
        sink_seen = true;
      } else if (sink_seen || this->avi_refused_.load() || (!this->avi_wanted_.load() && this->avi_->is_idle())) {
        const uint8_t message[2] = {'E', static_cast<uint8_t>(this->avi_->played_to_end() ? 1 : 0)};
        ::send(client, message, sizeof(message), MSG_DONTWAIT);
        break;
      }
      if (sender_done) {
        this->send_pending_(client);
        wait(false, RECV_TIMEOUT_MS);
        continue;
      }
      if (sink == nullptr || sink->stream_room() < RECV_BYTES) {
        this->send_pending_(client);
        if (sink != nullptr) {
          sink->want_room();
          if (sink->stream_room() < RECV_BYTES)
            wait(false, RECV_TIMEOUT_MS);
        } else {
          wait(false, RECV_TIMEOUT_MS);
        }
        last_recv = millis();
        continue;
      }
    }
#endif
    const int ready = wait(true, next_due_ms());
    this->send_pending_(client);
    if (wifi_held && millis() - last_stream > ROAM_QUIET_MS)
      this->hold_wifi_(wifi_held, false);
    if (ready == 0) {
      if (millis() - last_recv > RECV_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Sender silent for %u s, disconnecting", static_cast<unsigned>(RECV_TIMEOUT_MS / 1000));
        break;
      }
      continue;
    }
    if (ready < 0)
      break;
    if (!kind_known) {
      uint8_t magic[4];
      const int peeked = ::recv(client, magic, sizeof(magic), MSG_PEEK);
      if (peeked <= 0)
        break;
      if (peeked < static_cast<int>(sizeof(magic))) {
        vTaskDelay(1);
        continue;
      }
      kind_known = true;
      if (std::memcmp(magic, "RIFF", sizeof(magic)) == 0) {
#ifdef USE_STREAM_AVI
        this->avi_refused_.store(false);
        this->avi_wanted_.store(true);
        this->avi_streaming_.store(true);
        this->connected_.store(false);
        this->state_changed_.store(true);
        this->enable_loop_soon_any_context();
        continue;
#else
        ESP_LOGW(TAG, "An AVI arrived, but no avi_player is configured");
        break;
#endif
      }
    }
    const int received = ::recv(client, this->recv_buf_, RECV_BYTES, 0);
#ifdef USE_STREAM_AVI
    if (received == 0 && sink != nullptr) {
      sink->end_stream();
      sender_done = true;
      this->hold_wifi_(wifi_held, false);
      continue;
    }
#endif
    if (received <= 0) {
      ESP_LOGI(TAG, "Sender disconnected");
      break;
    }
    last_recv = millis();
    if (received > HEARTBEAT_BYTES) {
      last_stream = last_recv;
      this->hold_wifi_(wifi_held, true);
    }
#ifdef USE_STREAM_AVI
    if (sink != nullptr) {
      sink->push(this->recv_buf_, static_cast<size_t>(received));
      continue;
    }
#endif
    this->feed_(this->recv_buf_, static_cast<size_t>(received));
  }

#ifdef USE_STREAM_AVI
  if (this->avi_streaming_.exchange(false)) {
    avi::BufferedFileReader *sink = this->avi_->stream_sink();
    if (sink != nullptr)
      sink->end_stream();
  }
#endif
  this->hold_wifi_(wifi_held, false);
  if (this->current_ != nullptr) {
    xQueueSend(this->empty_q_, &this->current_, 0);
    this->current_ = nullptr;
  }
  this->connected_.store(false);
  while (this->draw_busy_.load() || uxQueueMessagesWaiting(this->filled_q_) != 0)
    wait(false, DRAIN_WAIT_MS);
  this->give_buffers_();
  this->close_page_.store(true);
  this->state_changed_.store(true);
  this->enable_loop_soon_any_context();
}

void StreamMediaPlayer::feed_(const uint8_t *data, size_t len) {
  while (len > 0) {
    if (this->skip_ > 0) {
      const size_t take = std::min<size_t>(this->skip_, len);
      this->skip_ -= take;
      data += take;
      len -= take;
      continue;
    }
#ifdef USE_SPEAKER
    if (this->sound_left_ > 0) {
      const size_t take = std::min<size_t>(this->sound_left_, len);
      this->play_sound_(data, take);
      this->sound_left_ -= take;
      data += take;
      len -= take;
      continue;
    }
#endif
    if (this->current_ != nullptr) {
      Frame *frame = this->current_;
      const size_t take = std::min<size_t>(frame->total - frame->received, len);
      std::memcpy(frame->data + frame->received, data, take);
      frame->received += take;
      data += take;
      len -= take;
      if (frame->received == frame->total) {
        xQueueSend(this->filled_q_, &frame, portMAX_DELAY);
        this->current_ = nullptr;
      }
      continue;
    }
    const size_t take = std::min(HEADER_BYTES - this->header_len_, len);
    std::memcpy(this->header_ + this->header_len_, data, take);
    this->header_len_ += take;
    data += take;
    len -= take;
    if (this->header_len_ < HEADER_BYTES)
      return;
    this->header_len_ = 0;
    const uint8_t type = this->header_[2];
    const uint16_t x = rd16(this->header_ + 4);
    const uint16_t y = rd16(this->header_ + 6);
    const uint16_t w = rd16(this->header_ + 8);
    const uint16_t h = rd16(this->header_ + 10);
    const uint32_t total = rd32(this->header_ + 12) >> 10;
#ifdef USE_SPEAKER
    if (type == UDISP_TYPE_PCM && this->sound_block_ != nullptr) {
      this->sound_in_ch_ = w == 0 ? 1 : static_cast<uint8_t>(w);
      this->sound_in_rate_ = h == 0 ? SOUND_DEFAULT_RATE : h;
      this->sound_left_ = total;
      continue;
    }
#endif
    if (type == UDISP_TYPE_ECHO) {
      this->echo_.store(ECHO_PENDING | x);
      this->wake_();
    }
    if (type != UDISP_TYPE_JPG) {
      this->skip_ = total;
      continue;
    }
    const bool usable = w > 0 && h > 0 && static_cast<uint32_t>(x) + w <= this->page_w_ &&
                        static_cast<uint32_t>(y) + h <= this->page_h_ && total > 0 && total <= this->max_frame_bytes_;
    if (!usable) {
      if (!this->logged_bad_header_) {
        this->logged_bad_header_ = true;
        ESP_LOGW(TAG,
                 "Ignoring frames: %ux%u at %u,%u of %" PRIu32 " bytes is not a JPEG rectangle inside %ux%u of at most "
                 "%zu bytes",
                 w, h, x, y, total, this->page_w_, this->page_h_, this->max_frame_bytes_);
      }
      this->skip_ = total;
      continue;
    }
    Frame *frame = nullptr;
    bool got = xQueueReceive(this->empty_q_, &frame, 0) == pdTRUE;
    if (!got) {
      for (auto &slot : this->frames_) {
        if (slot.data == nullptr) {
          slot.data = psram_piece(this->max_frame_bytes_);
          if (slot.data != nullptr) {
            slot.capacity = this->max_frame_bytes_;
            frame = &slot;
            got = true;
          }
          break;
        }
      }
    }
    if (!got)
      got = xQueueReceive(this->empty_q_, &frame, pdMS_TO_TICKS(FRAME_WAIT_MS)) == pdTRUE;
    if (!got) {
      this->skip_ = total;
      continue;
    }
    frame->x = x;
    frame->y = y;
    frame->w = w;
    frame->h = h;
    frame->total = total;
    frame->received = 0;
    this->current_ = frame;
  }
}

void StreamMediaPlayer::send_pending_(int client) {
  const uint32_t echo = this->echo_.exchange(0);
  if ((echo & ECHO_PENDING) != 0) {
    const uint8_t message[3] = {'R', static_cast<uint8_t>(echo & 0xFF), static_cast<uint8_t>((echo >> 8) & 0xFF)};
    ::send(client, message, sizeof(message), MSG_DONTWAIT);
  }
  if (this->view_pending_.exchange(false)) {
    uint8_t report[32];
    const size_t report_len = this->output_ != nullptr ? this->output_->report(report, sizeof(report)) : 0;
    uint8_t message[12 + sizeof(report)];
    size_t at = 0;
    message[at++] = 'C';
    message[at++] = report_len > 7 ? report[7] : 16;
    message[at++] = 'V';
    message[at++] = static_cast<uint8_t>(this->page_w_ & 0xFF);
    message[at++] = static_cast<uint8_t>(this->page_w_ >> 8);
    message[at++] = static_cast<uint8_t>(this->page_h_ & 0xFF);
    message[at++] = static_cast<uint8_t>(this->page_h_ >> 8);
    message[at++] = 0;
    const uint32_t still = static_cast<uint32_t>(this->max_frame_bytes_);
    for (int shift = 0; shift < 32; shift += 8)
      message[at++] = static_cast<uint8_t>(still >> shift);
    std::memcpy(message + at, report, report_len);
    at += report_len;
    if (::send(client, message, at, MSG_DONTWAIT) != static_cast<int>(at))
      this->view_pending_.store(true);
  }
#ifdef USE_SPEAKER
  if (this->sound_pending_.exchange(false)) {
    uint32_t rate = this->sound_rate_;
    uint32_t min_rate = this->sound_rate_;
    uint32_t max_rate = this->sound_rate_;
    uint8_t channels = this->sound_channels_;
#ifdef USE_STREAM_RESAMPLER
    if (this->resampler_ != nullptr) {
      const audio::AudioStreamInfo format = this->resampler_->active_format();
      rate = format.get_sample_rate();
      min_rate = this->resampler_->active_min_sample_rate();
      max_rate = this->resampler_->active_max_sample_rate();
      channels = std::min<uint8_t>(format.get_channels(), this->sound_channels_);
    }
#endif
    const uint8_t message[12] = {'A',
                                 static_cast<uint8_t>(rate / 1000),
                                 static_cast<uint8_t>((rate % 1000) / 50),
                                 'L',
                                 static_cast<uint8_t>(min_rate / 1000),
                                 static_cast<uint8_t>((min_rate % 1000) / 50),
                                 static_cast<uint8_t>(max_rate / 1000),
                                 static_cast<uint8_t>((max_rate % 1000) / 50),
                                 'N',
                                 channels,
                                 'F',
                                 this->codecs_};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->sound_pending_.store(true);
  }
#endif
  if (this->status_pending_.exchange(false)) {
    const bool shown = this->canvas_ != nullptr && this->canvas_->held_by(this);
    const uint8_t message[2] = {'S', static_cast<uint8_t>(shown ? 1 : 0)};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->status_pending_.store(true);
  }
#ifdef USE_STREAM_TOUCH
  if (this->touch_q_ == nullptr)
    return;
  Touch event;
  while (xQueuePeek(this->touch_q_, &event, 0) == pdTRUE) {
    uint8_t message[2 + TOUCH_MAX * 5];
    message[0] = 'T';
    message[1] = event.count;
    size_t at = 2;
    for (uint8_t i = 0; i < event.count; i++) {
      message[at++] = event.id[i];
      message[at++] = static_cast<uint8_t>(event.x[i] & 0xFF);
      message[at++] = static_cast<uint8_t>(event.x[i] >> 8);
      message[at++] = static_cast<uint8_t>(event.y[i] & 0xFF);
      message[at++] = static_cast<uint8_t>(event.y[i] >> 8);
    }
    if (::send(client, message, at, MSG_DONTWAIT) != static_cast<int>(at))
      return;
    xQueueReceive(this->touch_q_, &event, 0);
  }
#endif
}

void StreamMediaPlayer::draw_entry_(void *arg) { static_cast<StreamMediaPlayer *>(arg)->draw_loop_(); }

void StreamMediaPlayer::draw_loop_() {
  while (true) {
    Frame *frame = nullptr;
    if (xQueueReceive(this->filled_q_, &frame, portMAX_DELAY) != pdTRUE || frame == nullptr)
      continue;
    this->draw_busy_.store(true);
#ifdef USE_STREAM_CANVAS
    if (this->reopen_.load()) {
      xQueueSend(this->empty_q_, &frame, 0);
      this->draw_busy_.store(false);
      this->wake_();
      this->enable_loop_soon_any_context();
      continue;
    }
#endif
    if (this->connected_.load() && this->output_ != nullptr &&
        (this->canvas_ == nullptr || !this->canvas_->held_by(this))) {
      display::VideoCanvas *canvas = this->open_target_();
      if (canvas != nullptr) {
        this->canvas_ = canvas;
        if (canvas->width() != this->page_w_ || canvas->height() != this->page_h_) {
          this->page_w_ = static_cast<uint16_t>(canvas->width());
          this->page_h_ = static_cast<uint16_t>(canvas->height());
          this->view_pending_.store(true);
        }
        this->status_pending_.store(true);
        this->enable_loop_soon_any_context();
      }
    }
    if (this->canvas_ != nullptr && this->canvas_->held_by(this))
      this->canvas_->page_rect(frame->data, frame->received, frame->x, frame->y, frame->w, frame->h);
    xQueueSend(this->empty_q_, &frame, 0);
    this->draw_busy_.store(false);
    this->wake_();
  }
}

#ifdef USE_SPEAKER
void StreamMediaPlayer::play_sound_(const uint8_t *data, size_t len) {
  uint32_t min_rate = this->sound_rate_;
  uint32_t max_rate = this->sound_rate_;
#ifdef USE_STREAM_RESAMPLER
  if (this->resampler_ != nullptr) {
    min_rate = this->resampler_->active_min_sample_rate();
    max_rate = this->resampler_->active_max_sample_rate();
  }
#endif
  const uint32_t rate = this->sound_in_rate_;
  if (rate < min_rate || rate > max_rate || this->sound_in_ch_ > 2) {
    if (!this->logged_sound_format_) {
      this->logged_sound_format_ = true;
      ESP_LOGW(TAG, "Sound of %u Hz, %u channel(s) does not fit the speaker (%u-%u Hz): not played",
               static_cast<unsigned>(rate), this->sound_in_ch_, static_cast<unsigned>(min_rate),
               static_cast<unsigned>(max_rate));
    }
    this->sound_carry_len_ = 0;
    return;
  }
  this->logged_sound_format_ = false;
  this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, this->sound_channels_, rate));
  const size_t in_frame = static_cast<size_t>(this->sound_in_ch_) * sizeof(int16_t);
  if (this->sound_carry_len_ >= in_frame)
    this->sound_carry_len_ = 0;
  while (len > 0) {
    if (this->sound_carry_len_ > 0 || len < in_frame) {
      const size_t take = std::min(in_frame - this->sound_carry_len_, len);
      std::memcpy(this->sound_carry_ + this->sound_carry_len_, data, take);
      this->sound_carry_len_ += take;
      data += take;
      len -= take;
      if (this->sound_carry_len_ < in_frame)
        return;
      this->emit_sound_(this->sound_carry_, 1);
      this->sound_carry_len_ = 0;
      continue;
    }
    const size_t frames = len / in_frame;
    this->emit_sound_(data, frames);
    data += frames * in_frame;
    len -= frames * in_frame;
  }
}

void StreamMediaPlayer::emit_sound_(const uint8_t *src, size_t frames) {
  const uint8_t in_ch = this->sound_in_ch_;
  const uint8_t out_ch = this->sound_channels_;
  const size_t out_frame = static_cast<size_t>(out_ch) * sizeof(int16_t);
  for (size_t i = 0; i < frames; i++) {
    int16_t in[2] = {0, 0};
    std::memcpy(in, src + i * in_ch * sizeof(int16_t), in_ch * sizeof(int16_t));
    int16_t out[2] = {0, 0};
    if (in_ch == out_ch) {
      out[0] = in[0];
      out[1] = in_ch == 2 ? in[1] : 0;
    } else if (in_ch == 1) {
      out[0] = out[1] = in[0];
    } else {
      out[0] = static_cast<int16_t>((static_cast<int32_t>(in[0]) + in[1]) / 2);
    }
    std::memcpy(this->sound_block_ + this->sound_block_used_, out, out_frame);
    this->sound_block_used_ += out_frame;
    if (this->sound_block_used_ + out_frame > this->sound_block_size_)
      this->flush_sound_();
  }
}

void StreamMediaPlayer::flush_sound_() {
  const size_t length = this->sound_block_used_;
  const size_t written = this->speaker_->play(this->sound_block_, length);
  if (written >= length || written == 0) {
    this->sound_block_used_ = 0;
    return;
  }
  std::memmove(this->sound_block_, this->sound_block_ + written, length - written);
  this->sound_block_used_ = length - written;
}
#endif

#ifdef USE_STREAM_TOUCH
void StreamTouchListener::update(const touchscreen::TouchPoints_t &points) { this->parent_->on_touch(points); }

void StreamTouchListener::release() {
  const touchscreen::TouchPoints_t none;
  this->parent_->on_touch(none);
}

void StreamMediaPlayer::on_touch(const touchscreen::TouchPoints_t &points) {
  if (this->touch_q_ == nullptr || !this->connected_.load() || this->canvas_ == nullptr ||
      !this->canvas_->held_by(this))
    return;
  Touch event{};
  for (const auto &point : points) {
    if (event.count >= TOUCH_MAX)
      break;
    int32_t x = point.x;
    int32_t y = point.y;
    if (!this->canvas_->canvas_point(&x, &y))
      continue;
    event.id[event.count] = point.id;
    event.x[event.count] = static_cast<uint16_t>(x);
    event.y[event.count] = static_cast<uint16_t>(y);
    event.count++;
  }
  if (this->last_touch_valid_ && std::memcmp(&event, &this->last_touch_, sizeof(event)) == 0)
    return;
  this->last_touch_ = event;
  this->last_touch_valid_ = true;
  if (xQueueSend(this->touch_q_, &event, 0) != pdTRUE) {
    Touch discarded;
    xQueueReceive(this->touch_q_, &discarded, 0);
    xQueueSend(this->touch_q_, &event, 0);
  }
  this->wake_();
}
#endif

}

#endif
