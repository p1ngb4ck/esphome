// Portall-style network receiver: Espressif's udisp protocol over TCP (as sent by Portall's
// udisp_send.py / ha_send.py). JPEG rectangles are decoded into decode_target_ and drawn onto the
// display with draw_pixels_at(); touches go back over the same socket. File playback has priority.

#include "simple_video_player.h"

#ifdef SVP_STREAM

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"

#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.stream";

static constexpr uint8_t UDISP_TYPE_JPG = 3;
static constexpr size_t UDISP_HEADER_BYTES = 16;
static constexpr size_t STREAM_RECV_BYTES = 16 * 1024;
static constexpr uint32_t STREAM_RECV_TIMEOUT_MS = 30000;
static constexpr uint32_t STREAM_FRAME_WAIT_MS = 250;

static inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool SimpleVideoPlayer::setup_stream_() {
  this->output_lock_ = xSemaphoreCreateMutex();
  this->stream_empty_q_ = xQueueCreate(STREAM_FRAME_COUNT, sizeof(StreamFrame *));
  this->stream_filled_q_ = xQueueCreate(STREAM_FRAME_COUNT, sizeof(StreamFrame *));
  if (this->output_lock_ == nullptr || this->stream_empty_q_ == nullptr || this->stream_filled_q_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create stream queues");
    return false;
  }
  jpeg_decode_memory_alloc_cfg_t in_cfg{};
  in_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  for (auto &frame : this->stream_frames_) {
    size_t got = 0;
    frame.data = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(this->stream_max_frame_bytes_, &in_cfg, &got));
    if (frame.data == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate a %" PRIu32 "-byte stream frame buffer (PSRAM)", this->stream_max_frame_bytes_);
      return false;
    }
    frame.capacity = got;
    StreamFrame *p = &frame;
    xQueueSend(this->stream_empty_q_, &p, 0);
  }
#ifdef SVP_STREAM_TOUCH
  if (this->stream_touchscreen_ != nullptr) {
    this->stream_touch_q_ = xQueueCreate(8, sizeof(StreamTouch));
    if (this->stream_touch_q_ != nullptr)
      this->stream_touchscreen_->register_listener(&this->stream_touch_listener_);
  }
#endif
  if (xTaskCreatePinnedToCore(stream_draw_task_entry_, "svp_stream_draw", 4096, this, 2, &this->stream_draw_task_, 1) !=
          pdPASS ||
      xTaskCreatePinnedToCore(stream_net_task_entry_, "svp_stream_net", 4096, this, 4, nullptr, tskNO_AFFINITY) !=
          pdPASS) {
    ESP_LOGE(TAG, "Failed to create stream tasks");
    return false;
  }
  ESP_LOGCONFIG(TAG, "Listening on port %u for udisp frames (%ux%u, %u-bit)", this->stream_port_, this->dsi_out_w_,
                this->dsi_out_h_, this->dsi_fb_bpp_ * 8);
  return true;
}

void SimpleVideoPlayer::stream_set_awake_(bool awake) {
  if (this->output_lock_ == nullptr)
    return;
  this->stream_awake_.store(awake, std::memory_order_release);
  this->stream_status_pending_.store(true, std::memory_order_release);
}

void SimpleVideoPlayer::stream_net_task_entry_(void *param) { static_cast<SimpleVideoPlayer *>(param)->stream_net_loop_(); }

void SimpleVideoPlayer::stream_net_loop_() {
  auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(STREAM_RECV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate the %u-byte receive buffer (PSRAM)", static_cast<unsigned>(STREAM_RECV_BYTES));
    vTaskDelete(nullptr);
    return;
  }

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
    addr.sin_port = htons(this->stream_port_);
    if (::bind(listener, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(listener, 1) < 0) {
      ESP_LOGE(TAG, "Could not listen on port %u: %s", this->stream_port_, strerror(errno));
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
      ESP_LOGI(TAG, "Sender connected from %s", peer_text);

      this->stream_hdr_len_ = 0;
      this->stream_skip_ = 0;
      this->stream_current_ = nullptr;
      this->stream_depth_pending_.store(true, std::memory_order_release);
      this->stream_status_pending_.store(true, std::memory_order_release);
#ifdef SVP_STREAM_TOUCH
      if (this->stream_touch_q_ != nullptr)
        xQueueReset(this->stream_touch_q_);
      this->stream_last_touch_valid_ = false;
#endif
      this->stream_connected_.store(true, std::memory_order_release);

      uint32_t last_recv = millis();
      while (true) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(client, &readable);
        struct timeval slice = {.tv_sec = 0, .tv_usec = 5000};
        const int ready = ::select(client + 1, &readable, nullptr, nullptr, &slice);
        this->stream_send_pending_(client);
        if (ready == 0) {
          if (millis() - last_recv > STREAM_RECV_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Sender silent for %u s, disconnecting", static_cast<unsigned>(STREAM_RECV_TIMEOUT_MS / 1000));
            break;
          }
          continue;
        }
        if (ready < 0)
          break;
        const int received = ::recv(client, buffer, STREAM_RECV_BYTES, 0);
        if (received <= 0) {
          ESP_LOGI(TAG, "Sender disconnected");
          break;
        }
        last_recv = millis();
        this->stream_feed_(buffer, static_cast<size_t>(received));
      }

      this->stream_connected_.store(false, std::memory_order_release);
      if (this->stream_current_ != nullptr) {
        xQueueSend(this->stream_empty_q_, &this->stream_current_, 0);
        this->stream_current_ = nullptr;
      }
      ::close(client);
    }
    ::close(listener);
  }
}

void SimpleVideoPlayer::stream_feed_(const uint8_t *data, size_t len) {
  while (len > 0) {
    if (this->stream_skip_ > 0) {
      const size_t take = std::min<size_t>(this->stream_skip_, len);
      this->stream_skip_ -= take;
      data += take;
      len -= take;
      continue;
    }

    if (this->stream_current_ != nullptr) {
      StreamFrame *frame = this->stream_current_;
      const size_t take = std::min<size_t>(frame->total - frame->received, len);
      std::memcpy(frame->data + frame->received, data, take);
      frame->received += take;
      data += take;
      len -= take;
      if (frame->received == frame->total) {
        xQueueSend(this->stream_filled_q_, &frame, portMAX_DELAY);
        this->stream_current_ = nullptr;
      }
      continue;
    }

    const size_t take = std::min(UDISP_HEADER_BYTES - this->stream_hdr_len_, len);
    std::memcpy(this->stream_hdr_ + this->stream_hdr_len_, data, take);
    this->stream_hdr_len_ += take;
    data += take;
    len -= take;
    if (this->stream_hdr_len_ < UDISP_HEADER_BYTES)
      return;
    this->stream_hdr_len_ = 0;

    // <HBBHHHHI: crc16, type, cmd, x, y, width, height, frame_id:10 | payload_total:22
    const uint8_t type = this->stream_hdr_[2];
    const uint16_t x = rd16(this->stream_hdr_ + 4);
    const uint16_t y = rd16(this->stream_hdr_ + 6);
    const uint16_t w = rd16(this->stream_hdr_ + 8);
    const uint16_t h = rd16(this->stream_hdr_ + 10);
    const uint32_t total = rd32(this->stream_hdr_ + 12) >> 10;

    // Heartbeats/END and sound (not played here) are counted out like any payload.
    if (type != UDISP_TYPE_JPG) {
      this->stream_skip_ = total;
      continue;
    }
    const bool usable = w > 0 && h > 0 && static_cast<uint32_t>(x) + w <= this->dsi_out_w_ &&
                        static_cast<uint32_t>(y) + h <= this->dsi_out_h_ && total > 0 &&
                        total <= this->stream_frames_[0].capacity;
    if (!usable) {
      if (!this->stream_logged_bad_header_) {
        this->stream_logged_bad_header_ = true;
        ESP_LOGW(TAG,
                 "Ignoring frames: %ux%u at %u,%u of %" PRIu32 " bytes is not a JPEG rectangle inside %ux%u of at most "
                 "%u bytes",
                 w, h, x, y, total, this->dsi_out_w_, this->dsi_out_h_,
                 static_cast<unsigned>(this->stream_frames_[0].capacity));
      }
      this->stream_skip_ = total;
      continue;
    }

    // No free buffer: not reading the socket for a moment is the flow control; after that the
    // rectangle is counted out so the next header is found.
    StreamFrame *frame = nullptr;
    if (xQueueReceive(this->stream_empty_q_, &frame, pdMS_TO_TICKS(STREAM_FRAME_WAIT_MS)) != pdTRUE) {
      this->stream_skip_ = total;
      continue;
    }
    frame->x = x;
    frame->y = y;
    frame->w = w;
    frame->h = h;
    frame->total = total;
    frame->received = 0;
    this->stream_current_ = frame;
  }
}

void SimpleVideoPlayer::stream_send_pending_(int client) {
  if (this->stream_depth_pending_.exchange(false, std::memory_order_acq_rel)) {
    const uint8_t message[2] = {'C', static_cast<uint8_t>(this->dsi_fb_bpp_ * 8)};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->stream_depth_pending_.store(true, std::memory_order_release);
  }
  if (this->stream_status_pending_.exchange(false, std::memory_order_acq_rel)) {
    const uint8_t message[2] = {'S', static_cast<uint8_t>(this->stream_awake_.load(std::memory_order_acquire) ? 1 : 0)};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->stream_status_pending_.store(true, std::memory_order_release);
  }
#ifdef SVP_STREAM_TOUCH
  if (this->stream_touch_q_ == nullptr)
    return;
  StreamTouch event;
  while (xQueuePeek(this->stream_touch_q_, &event, 0) == pdTRUE) {
    uint8_t message[2 + STREAM_TOUCH_MAX * 5];
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
    xQueueReceive(this->stream_touch_q_, &event, 0);
  }
#endif
}

void SimpleVideoPlayer::stream_draw_task_entry_(void *param) {
  static_cast<SimpleVideoPlayer *>(param)->stream_draw_loop_();
}

void SimpleVideoPlayer::stream_draw_loop_() {
  jpeg_decode_cfg_t decode_cfg = {};
  decode_cfg.output_format =
      this->dsi_fb_bpp_ == 3 ? JPEG_DECODE_OUT_FORMAT_RGB888 : JPEG_DECODE_OUT_FORMAT_RGB565;
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  const display::ColorBitness bitness =
      this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;

  while (true) {
    StreamFrame *frame = nullptr;
    if (xQueueReceive(this->stream_filled_q_, &frame, portMAX_DELAY) != pdTRUE)
      continue;
    // Draw only once loop() has LVGL (if any) paused; released by loop() via notification.
    while (this->stream_connected_.load(std::memory_order_acquire) &&
           !this->stream_output_ok_.load(std::memory_order_acquire))
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // A playing file holds output_lock_ for its whole session: its picture wins.
    if (this->stream_output_ok_.load(std::memory_order_acquire) && xSemaphoreTake(this->output_lock_, 0) == pdTRUE) {
      uint32_t out_size = 0;
      if (jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, frame->data, frame->received,
                               this->decode_target_, static_cast<uint32_t>(this->decode_target_len_),
                               &out_size) == ESP_OK &&
          out_size > 0) {
        // Decoded rows are padded to 16; x_pad skips the padding at the end of each row.
        const uint16_t padded_w = (frame->w + 15) & ~15;
        this->dsi_->draw_pixels_at(frame->x, frame->y, frame->w, frame->h, this->decode_target_,
                                   display::COLOR_ORDER_RGB, bitness, false, 0, 0, padded_w - frame->w);
      }
      xSemaphoreGive(this->output_lock_);
    }
    xQueueSend(this->stream_empty_q_, &frame, 0);
  }
}

#ifdef SVP_STREAM_TOUCH
void StreamTouchListener::update(const touchscreen::TouchPoints_t &points) { this->parent_->stream_on_touch(points); }

void StreamTouchListener::release() {
  const touchscreen::TouchPoints_t none;
  this->parent_->stream_on_touch(none);
}

void SimpleVideoPlayer::stream_on_touch(const touchscreen::TouchPoints_t &points) {
  if (this->stream_touch_q_ == nullptr || !this->stream_connected_.load(std::memory_order_acquire) ||
      !this->stream_awake_.load(std::memory_order_acquire))
    return;
  StreamTouch event{};
  for (const auto &point : points) {
    if (event.count >= STREAM_TOUCH_MAX)
      break;
    event.id[event.count] = point.id;
    event.x[event.count] = point.x;
    event.y[event.count] = point.y;
    event.count++;
  }
  if (this->stream_last_touch_valid_ && std::memcmp(&event, &this->stream_last_touch_, sizeof(event)) == 0)
    return;
  this->stream_last_touch_ = event;
  this->stream_last_touch_valid_ = true;
  if (xQueueSend(this->stream_touch_q_, &event, 0) != pdTRUE) {
    // Full: drop the oldest so the newest (often the release) always goes out.
    StreamTouch discarded;
    xQueueReceive(this->stream_touch_q_, &discarded, 0);
    xQueueSend(this->stream_touch_q_, &event, 0);
  }
}
#endif

}  // namespace esphome::simple_video_player

#endif  // SVP_STREAM
