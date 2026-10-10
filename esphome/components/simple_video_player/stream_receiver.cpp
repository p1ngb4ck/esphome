// Portall-style network receiver: Espressif's udisp protocol over TCP (as sent by Portall's
// udisp_send.py / ha_send.py, or the svp_relay integration). The network task queues JPEG and PCM
// packets verbatim into read_ring_ -- the read-ahead ring a playing file streams through -- and the
// present task takes them out in order: sound goes to the speaker, each JPEG through
// frame_slots_[0] and the HW decoder into decode_target_ and onto the display with
// draw_pixels_at(). A sender that states its frame rate (UDISP_TYPE_SVP_TIMING) is presented at that
// cadence from a pre-rolled queue; otherwise every rectangle is drawn as it arrives. A playing file
// takes all of this over (output_lock_); touches go back over the same socket.

#include "simple_video_player.h"

#ifdef SVP_STREAM

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_AUDIO
#include "esphome/components/audio/audio.h"
#endif
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.stream";

static constexpr uint8_t UDISP_TYPE_JPG = 3;
static constexpr uint8_t UDISP_TYPE_PCM = 0x10;
// SVP extension: width/height carry the sender's frame rate as numerator/denominator.
static constexpr uint8_t UDISP_TYPE_SVP_TIMING = 0x11;
static constexpr uint32_t STREAM_AUDIO_DEFAULT_RATE = 48000;
static constexpr uint32_t STREAM_AUDIO_BLOCK_MS = 10;
static constexpr size_t UDISP_HEADER_BYTES = 16;
static constexpr size_t STREAM_RECV_BYTES = 16 * 1024;
static constexpr uint32_t STREAM_RECV_TIMEOUT_MS = 30000;
static constexpr uint32_t STREAM_SLICE_MS = 5;
// Paced senders: frames queued before the first presentation, and again after the queue ran dry.
static constexpr uint32_t STREAM_PREROLL_FRAMES = 5;

static inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool SimpleVideoPlayer::setup_stream_() {
  this->output_lock_ = xSemaphoreCreateMutex();
  this->stream_ring_mutex_ = xSemaphoreCreateMutex();
  if (this->output_lock_ == nullptr || this->stream_ring_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create stream locks");
    return false;
  }
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    this->stream_audio_out_ch_ = this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_STEREO ? 2 : 1;
    this->stream_audio_block_size_ = static_cast<size_t>(AUDIO_SAMPLE_RATE) * STREAM_AUDIO_BLOCK_MS / 1000 *
                                     this->stream_audio_out_ch_ * sizeof(int16_t);
    this->stream_audio_block_ =
        static_cast<uint8_t *>(heap_caps_malloc(this->stream_audio_block_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (this->stream_audio_block_ == nullptr)
      ESP_LOGW(TAG, "No memory for the %u-byte audio block: stream plays without sound",
               static_cast<unsigned>(this->stream_audio_block_size_));
  }
#endif
#ifdef SVP_STREAM_TOUCH
  if (this->stream_touchscreen_ != nullptr) {
    this->stream_touch_q_ = xQueueCreate(8, sizeof(StreamTouch));
    if (this->stream_touch_q_ != nullptr)
      this->stream_touchscreen_->register_listener(&this->stream_touch_listener_);
  }
#endif
  // Present task: Core 1 with the decoder (esp-idf#18999) and, like the file playback task, at
  // loopTask's priority, because it paces by spinning on the clock.
  if (xTaskCreatePinnedToCore(stream_present_task_entry_, "svp_stream_present", 4096, this, 1,
                              &this->stream_present_task_, 1) != pdPASS ||
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

void SimpleVideoPlayer::stream_ring_off_() {
  xSemaphoreTake(this->stream_ring_mutex_, portMAX_DELAY);
  this->stream_ring_on_.store(false, std::memory_order_release);
  xSemaphoreGive(this->stream_ring_mutex_);
}

void SimpleVideoPlayer::stream_claim_output_() {
  if (this->output_lock_ == nullptr)
    return;
  this->stream_file_claims_.fetch_add(1, std::memory_order_acq_rel);
  this->stream_set_awake_(false);
  if (this->stream_present_task_ != nullptr)
    xTaskNotifyGive(this->stream_present_task_);
  xSemaphoreTake(this->output_lock_, portMAX_DELAY);
}

void SimpleVideoPlayer::stream_return_output_() {
  if (this->output_lock_ == nullptr)
    return;
  xSemaphoreGive(this->output_lock_);
  if (this->stream_file_claims_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    this->stream_set_awake_(true);  // a connected sender redraws the whole page
  if (this->stream_present_task_ != nullptr)
    xTaskNotifyGive(this->stream_present_task_);
}

void SimpleVideoPlayer::stream_net_task_entry_(void *param) {
  static_cast<SimpleVideoPlayer *>(param)->stream_net_loop_();
}

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
      this->stream_copy_ = 0;
      this->stream_frame_us_.store(0, std::memory_order_release);
#ifdef USE_AUDIO
      this->stream_rate_pending_.store(this->stream_audio_block_ != nullptr, std::memory_order_release);
#endif
      this->stream_depth_pending_.store(true, std::memory_order_release);
      this->stream_status_pending_.store(true, std::memory_order_release);
#ifdef SVP_STREAM_TOUCH
      if (this->stream_touch_q_ != nullptr)
        xQueueReset(this->stream_touch_q_);
      this->stream_last_touch_valid_ = false;
#endif
      this->stream_connected_.store(true, std::memory_order_release);
      xTaskNotifyGive(this->stream_present_task_);

      uint32_t last_recv = millis();
      while (true) {
        // Only read what read_ring_ can take whole: leaving the socket unread is the flow control.
        if (this->stream_ring_on_.load(std::memory_order_acquire) &&
            this->read_ring_->free() < STREAM_RECV_BYTES + UDISP_HEADER_BYTES) {
          this->stream_send_pending_(client);
          vTaskDelay(pdMS_TO_TICKS(STREAM_SLICE_MS));
          last_recv = millis();
          continue;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(client, &readable);
        struct timeval slice = {.tv_sec = 0, .tv_usec = STREAM_SLICE_MS * 1000};
        const int ready = ::select(client + 1, &readable, nullptr, nullptr, &slice);
        this->stream_send_pending_(client);
        if (ready == 0) {
          if (millis() - last_recv > STREAM_RECV_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Sender silent for %u s, disconnecting",
                     static_cast<unsigned>(STREAM_RECV_TIMEOUT_MS / 1000));
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
        xSemaphoreTake(this->stream_ring_mutex_, portMAX_DELAY);
        this->stream_feed_(buffer, static_cast<size_t>(received));
        xSemaphoreGive(this->stream_ring_mutex_);
      }

      this->stream_ring_off_();
      this->stream_connected_.store(false, std::memory_order_release);
      xTaskNotifyGive(this->stream_present_task_);
      ::close(client);
    }
    ::close(listener);
  }
}

void SimpleVideoPlayer::stream_feed_(const uint8_t *data, size_t len) {
  // Stable for this call: both only change under stream_ring_mutex_, which the caller holds, or
  // while stream_ring_on_ is false.
  const bool ring_on = this->stream_ring_on_.load(std::memory_order_acquire);
  const uint32_t epoch = this->stream_ring_epoch_.load(std::memory_order_acquire);
  bool queued = false;
  while (len > 0) {
    if (this->stream_skip_ > 0) {
      const size_t take = std::min<size_t>(this->stream_skip_, len);
      this->stream_skip_ -= take;
      data += take;
      len -= take;
      continue;
    }

    if (this->stream_copy_ > 0) {
      const size_t take = std::min<size_t>(this->stream_copy_, len);
      // The ring was taken away or reset after this packet's header went in: the rest goes nowhere.
      const bool keep = ring_on && epoch == this->stream_copy_epoch_;
      if (keep) {
        // Counted before its last bytes land: the present task waits for the bytes anyway, and can
        // never take a frame out before it was counted.
        if (take == this->stream_copy_ && this->stream_copy_jpg_)
          this->stream_frames_queued_.fetch_add(1, std::memory_order_acq_rel);
        this->read_ring_->write_without_replacement(data, take, 0, false);
        queued = true;
      }
      this->stream_copy_ -= take;
      data += take;
      len -= take;
      continue;
    }

    const size_t take = std::min(UDISP_HEADER_BYTES - this->stream_hdr_len_, len);
    std::memcpy(this->stream_hdr_ + this->stream_hdr_len_, data, take);
    this->stream_hdr_len_ += take;
    data += take;
    len -= take;
    if (this->stream_hdr_len_ < UDISP_HEADER_BYTES)
      break;
    this->stream_hdr_len_ = 0;

    // <HBBHHHHI: crc16, type, cmd, x, y, width, height, frame_id:10 | payload_total:22
    const uint8_t type = this->stream_hdr_[2];
    const uint16_t x = rd16(this->stream_hdr_ + 4);
    const uint16_t y = rd16(this->stream_hdr_ + 6);
    const uint16_t w = rd16(this->stream_hdr_ + 8);
    const uint16_t h = rd16(this->stream_hdr_ + 10);
    const uint32_t total = rd32(this->stream_hdr_ + 12) >> 10;

    if (type == UDISP_TYPE_SVP_TIMING) {
      const uint32_t den = h == 0 ? 1 : h;
      this->stream_frame_us_.store(w == 0 ? 0 : static_cast<uint32_t>(1000000ULL * den / w), std::memory_order_release);
      this->stream_skip_ = total;
      continue;
    }

    bool keep = false;
    if (type == UDISP_TYPE_JPG) {
      keep = w > 0 && h > 0 && static_cast<uint32_t>(x) + w <= this->dsi_out_w_ &&
             static_cast<uint32_t>(y) + h <= this->dsi_out_h_ && total > 0 && total <= this->frame_slots_[0].capacity;
      if (!keep && !this->stream_logged_bad_header_) {
        this->stream_logged_bad_header_ = true;
        ESP_LOGW(TAG,
                 "Ignoring frames: %ux%u at %u,%u of %" PRIu32 " bytes is not a JPEG rectangle inside %ux%u of at most "
                 "%u bytes (input_buffer_size)",
                 w, h, x, y, total, this->dsi_out_w_, this->dsi_out_h_,
                 static_cast<unsigned>(this->frame_slots_[0].capacity));
      }
    }
#ifdef USE_AUDIO
    // Sound: width carries the channel count (0 = 1), height the rate (0 = 48000).
    else if (type == UDISP_TYPE_PCM && this->stream_audio_block_ != nullptr) {
      const uint32_t channels = w == 0 ? 1 : w;
      const uint32_t rate = h == 0 ? STREAM_AUDIO_DEFAULT_RATE : h;
      keep = AUDIO_BITS_PER_SAMPLE == 16 && rate == AUDIO_SAMPLE_RATE && channels <= 2;
      if (!keep && !this->stream_logged_audio_format_) {
        this->stream_logged_audio_format_ = true;
        ESP_LOGW(TAG, "Sound of %u Hz, %u channel(s) does not fit the speaker (%u Hz, %u bit): not played",
                 static_cast<unsigned>(rate), static_cast<unsigned>(channels), static_cast<unsigned>(AUDIO_SAMPLE_RATE),
                 static_cast<unsigned>(AUDIO_BITS_PER_SAMPLE));
      }
    }
#endif

    // Heartbeats/END, unusable rectangles and sound without a speaker are counted out like any
    // payload; so is everything while the stream does not hold the ring.
    if (keep && ring_on) {
      this->read_ring_->write_without_replacement(this->stream_hdr_, UDISP_HEADER_BYTES, 0, false);
      this->stream_copy_ = total;
      this->stream_copy_jpg_ = type == UDISP_TYPE_JPG;
      this->stream_copy_epoch_ = epoch;
      queued = true;
    } else {
      this->stream_skip_ = total;
    }
  }
  if (queued)
    xTaskNotifyGive(this->stream_present_task_);
}

void SimpleVideoPlayer::stream_send_pending_(int client) {
  if (this->stream_depth_pending_.exchange(false, std::memory_order_acq_rel)) {
    const uint8_t message[2] = {'C', static_cast<uint8_t>(this->dsi_fb_bpp_ * 8)};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->stream_depth_pending_.store(true, std::memory_order_release);
  }
#ifdef USE_AUDIO
  // 'A': the rate this panel's speaker runs at (kHz, then 50 Hz steps), so the sender captures at it.
  if (this->stream_rate_pending_.exchange(false, std::memory_order_acq_rel)) {
    const uint8_t message[3] = {'A', static_cast<uint8_t>(AUDIO_SAMPLE_RATE / 1000),
                                static_cast<uint8_t>((AUDIO_SAMPLE_RATE % 1000) / 50)};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->stream_rate_pending_.store(true, std::memory_order_release);
  }
#endif
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

void SimpleVideoPlayer::stream_present_task_entry_(void *param) {
  static_cast<SimpleVideoPlayer *>(param)->stream_present_loop_();
}

void SimpleVideoPlayer::stream_present_loop_() {
  jpeg_decode_cfg_t decode_cfg = {};
  decode_cfg.output_format = this->dsi_fb_bpp_ == 3 ? JPEG_DECODE_OUT_FORMAT_RGB888 : JPEG_DECODE_OUT_FORMAT_RGB565;
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  const display::ColorBitness bitness =
      this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
  FrameSlot &slot = this->frame_slots_[0];
  ring_buffer::RingBuffer &ring = *this->read_ring_;

  bool owned = false;  // holding output_lock_
  uint8_t hdr[UDISP_HEADER_BYTES]{};
  bool have_hdr = false;
  uint32_t pcm_left = 0;
  // Pacing (senders that stated a frame rate): frame n is presented at anchor_us + n * frame period.
  bool prerolled = false;
  int64_t anchor_us = 0;  // 0: the next presented frame starts the clock
  uint32_t frame_index = 0;

  while (true) {
    const bool want = this->stream_connected_.load(std::memory_order_acquire) &&
                      this->stream_output_ok_.load(std::memory_order_acquire) &&
                      this->stream_file_claims_.load(std::memory_order_acquire) == 0;
    if (!want) {
      if (owned) {
        this->stream_ring_off_();
        xSemaphoreGive(this->output_lock_);
        owned = false;
      }
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (!owned) {
      if (xSemaphoreTake(this->output_lock_, 0) != pdTRUE) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      owned = true;
    }
    if (!this->stream_ring_on_.load(std::memory_order_acquire)) {
      // Fresh start (output just taken, or a new connection): whatever the ring holds is stale.
      ring.reset();
      this->stream_frames_queued_.store(0, std::memory_order_release);
      this->stream_ring_epoch_.fetch_add(1, std::memory_order_acq_rel);
      have_hdr = false;
      pcm_left = 0;
      prerolled = false;
      anchor_us = 0;
#ifdef USE_AUDIO
      this->stream_audio_carry_len_ = 0;
#endif
      this->stream_ring_on_.store(true, std::memory_order_release);
      continue;
    }

    const int64_t frame_us = this->stream_frame_us_.load(std::memory_order_acquire);
    // Nothing to take out yet. If a paced frame is already due, the queue ran dry: build it up again.
    auto wait_for_data = [&]() {
      if (frame_us != 0 && prerolled && anchor_us != 0 &&
          esp_timer_get_time() > anchor_us + static_cast<int64_t>(frame_index) * frame_us)
        prerolled = false;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    };

    if (!have_hdr) {
      if (ring.available() < UDISP_HEADER_BYTES) {
        wait_for_data();
        continue;
      }
      ring.read(hdr, UDISP_HEADER_BYTES, 0);
      have_hdr = true;
      if (hdr[2] == UDISP_TYPE_PCM) {
        pcm_left = rd32(hdr + 12) >> 10;
#ifdef USE_AUDIO
        this->stream_audio_src_ch_ = rd16(hdr + 8) == 0 ? 1 : static_cast<uint8_t>(rd16(hdr + 8));
#endif
      }
    }

    // Sound: straight on to the speaker, in stream order with the pictures.
    if (hdr[2] == UDISP_TYPE_PCM) {
      while (pcm_left > 0) {
        size_t n = 0;
        void *chunk = ring.receive_acquire(n, pcm_left, 0);
        if (chunk == nullptr)
          break;
#ifdef USE_AUDIO
        this->stream_on_audio_(static_cast<const uint8_t *>(chunk), n);
#endif
        ring.receive_release(chunk);
        pcm_left -= n;
      }
      if (pcm_left > 0) {
        wait_for_data();
        continue;
      }
      have_hdr = false;
      continue;
    }

    // JPEG rectangle (the only other packet the network task queues).
    const uint32_t total = rd32(hdr + 12) >> 10;
    if (frame_us != 0 && !prerolled) {
      if (this->stream_frames_queued_.load(std::memory_order_acquire) < STREAM_PREROLL_FRAMES) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      prerolled = true;
      anchor_us = 0;
    }
    if (ring.available() < total) {
      wait_for_data();
      continue;
    }
    size_t got = 0;
    while (got < total) {
      const size_t n = ring.read(slot.data + got, total - got, 0);
      if (n == 0)
        break;
      got += n;
    }
    this->stream_frames_queued_.fetch_sub(1, std::memory_order_acq_rel);
    have_hdr = false;

    // Decode first, then wait for the frame's time: decode time does not shift the cadence.
    uint32_t out_size = 0;
    const bool decoded =
        got == total &&
        jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, slot.data, total, this->decode_target_,
                             static_cast<uint32_t>(this->decode_target_len_), &out_size) == ESP_OK &&
        out_size > 0;
    if (frame_us != 0) {
      const int64_t now = esp_timer_get_time();
      if (anchor_us == 0) {
        anchor_us = now;
        frame_index = 0;
      }
      int64_t due = anchor_us + static_cast<int64_t>(frame_index) * frame_us;
      // More than a frame behind: move the clock and carry on at 1x -- no catch-up, nothing skipped.
      if (now - due > frame_us) {
        anchor_us = now - static_cast<int64_t>(frame_index) * frame_us;
        due = now;
      }
      while (due - esp_timer_get_time() > 0) {
      }
      frame_index++;
    }
    if (decoded) {
      const uint16_t w = rd16(hdr + 8);
      // Decoded rows are padded to 16; x_pad skips the padding at the end of each row.
      const uint16_t padded_w = (w + 15) & ~15;
      this->dsi_->draw_pixels_at(rd16(hdr + 4), rd16(hdr + 6), w, rd16(hdr + 10), this->decode_target_,
                                 display::COLOR_ORDER_RGB, bitness, false, 0, 0, padded_w - w);
    }
  }
}

#ifdef USE_AUDIO
void SimpleVideoPlayer::stream_on_audio_(const uint8_t *data, size_t len) {
  // A playing file owns the speaker.
  if (this->state_.load(std::memory_order_acquire) != PlayerState::STOPPED)
    return;
  if (!this->speaker_->is_running()) {
    this->speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(AUDIO_BITS_PER_SAMPLE, this->stream_audio_out_ch_, AUDIO_SAMPLE_RATE));
    this->speaker_->start();
  }
  this->stream_last_audio_ms_.store(millis(), std::memory_order_release);
  this->stream_audio_on_.store(true, std::memory_order_release);

  const size_t in_frame = static_cast<size_t>(this->stream_audio_src_ch_) * sizeof(int16_t);
  while (len > 0) {
    if (this->stream_audio_carry_len_ > 0 || len < in_frame) {
      const size_t take = std::min(in_frame - this->stream_audio_carry_len_, len);
      std::memcpy(this->stream_audio_carry_ + this->stream_audio_carry_len_, data, take);
      this->stream_audio_carry_len_ += take;
      data += take;
      len -= take;
      if (this->stream_audio_carry_len_ < in_frame)
        return;
      this->stream_emit_audio_frames_(this->stream_audio_carry_, 1);
      this->stream_audio_carry_len_ = 0;
      continue;
    }
    const size_t frames = len / in_frame;
    this->stream_emit_audio_frames_(data, frames);
    data += frames * in_frame;
    len -= frames * in_frame;
  }
}

void SimpleVideoPlayer::stream_emit_audio_frames_(const uint8_t *src, size_t frames) {
  const uint8_t in_ch = this->stream_audio_src_ch_;
  const uint8_t out_ch = this->stream_audio_out_ch_;
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
    std::memcpy(this->stream_audio_block_ + this->stream_audio_block_used_, out, out_frame);
    this->stream_audio_block_used_ += out_frame;
    if (this->stream_audio_block_used_ + out_frame > this->stream_audio_block_size_)
      this->stream_flush_audio_block_();
  }
}

void SimpleVideoPlayer::stream_flush_audio_block_() {
  // Non-blocking: what the speaker does not take now is carried; a full block it refuses is dropped.
  const size_t length = this->stream_audio_block_used_;
  const size_t written = this->speaker_->play(this->stream_audio_block_, length);
  if (written >= length) {
    this->stream_audio_block_used_ = 0;
    return;
  }
  if (written == 0) {
    this->stream_audio_block_used_ = 0;
    return;
  }
  std::memmove(this->stream_audio_block_, this->stream_audio_block_ + written, length - written);
  this->stream_audio_block_used_ = length - written;
}
#endif

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
