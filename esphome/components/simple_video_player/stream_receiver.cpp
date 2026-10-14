// Network receiver. An AVI (svp_relay) is played through the file playback, into either output.
// With direct display output also Portall-style senders: Espressif's udisp protocol over TCP (as
// sent by Portall's udisp_send.py / ha_send.py), JPEG rectangles decoded into decode_target_ and
// drawn onto the display with draw_pixels_at(). Touches go back over the same socket. File playback
// has priority.

#include "simple_video_player.h"

#ifdef SVP_STREAM

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_AUDIO
#include "esphome/components/audio/audio.h"
#endif
#include "esp_heap_caps.h"
#include "esp_cache.h"

// Present only when the wifi component was built with the runtime controls __init__.py asks for
// whenever stream_port is set (as Portall's network.cpp).
#if defined(USE_ESP32) && defined(USE_WIFI) && defined(USE_WIFI_RUNTIME_ROAMING_SUPPRESSION)
#define SVP_HOLDS_ROAMING 1
#endif
#if defined(USE_ESP32) && defined(USE_WIFI) && defined(USE_WIFI_RUNTIME_POWER_SAVE)
#define SVP_HOLDS_POWER_SAVE 1
#endif
#if defined(SVP_HOLDS_ROAMING) || defined(SVP_HOLDS_POWER_SAVE)
#include "esphome/components/wifi/wifi_component.h"
#endif

#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.stream";

static constexpr uint8_t UDISP_TYPE_JPG = 3;
static constexpr uint8_t UDISP_TYPE_PCM = 0x10;
static constexpr uint32_t STREAM_AUDIO_DEFAULT_RATE = 48000;
static constexpr size_t UDISP_HEADER_BYTES = 16;
// Wi-Fi roaming scans and power saving are held off while pictures or sound arrive, and allowed
// again once only heartbeats have arrived for this long. A read no longer than one header is a
// heartbeat (Portall's network.cpp).
static constexpr uint32_t ROAM_QUIET_MS = 15000;
static constexpr int HEARTBEAT_BYTES = 16;
static constexpr uint32_t STREAM_RECV_TIMEOUT_MS = 30000;
// How long a starting stream waits for the session before it to end.
static constexpr uint32_t SESSION_END_WAIT_MS = 10000;
static constexpr uint32_t STREAM_FRAME_WAIT_MS = 250;

static inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static void hold_wifi_for_stream(bool &held, bool want) {
#if defined(SVP_HOLDS_ROAMING) || defined(SVP_HOLDS_POWER_SAVE)
  if (held == want || wifi::global_wifi_component == nullptr)
    return;
  // Both are counted by the wifi component: every request is released exactly once, including on
  // every way out of a connection.
  if (want) {
#ifdef SVP_HOLDS_ROAMING
    wifi::global_wifi_component->request_roaming_suppression();
#endif
#ifdef SVP_HOLDS_POWER_SAVE
    wifi::global_wifi_component->request_high_performance();
#endif
  } else {
#ifdef SVP_HOLDS_ROAMING
    wifi::global_wifi_component->release_roaming_suppression();
#endif
#ifdef SVP_HOLDS_POWER_SAVE
    wifi::global_wifi_component->release_high_performance();
#endif
  }
  held = want;
#else
  (void) held;
  (void) want;
#endif
}

bool SimpleVideoPlayer::setup_stream_() {
  this->output_lock_ = xSemaphoreCreateMutex();
  if (this->output_lock_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create the stream lock");
    return false;
  }
  this->stream_empty_q_ = xQueueCreate(STREAM_FRAME_COUNT, sizeof(StreamFrame *));
  this->stream_filled_q_ = xQueueCreate(STREAM_FRAME_COUNT, sizeof(StreamFrame *));
  if (this->stream_empty_q_ == nullptr || this->stream_filled_q_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create stream queues");
    return false;
  }
  this->stream_recv_buf_ = this->pool_.alloc(STREAM_RECV_BYTES);
  if (this->stream_recv_buf_ == nullptr) {
    ESP_LOGE(TAG, "No room in the PSRAM pool for the receive buffer");
    return false;
  }
  // The frames stay at the start of the pool (plan_pool_()).
  for (auto &frame : this->stream_frames_) {
    frame.data = this->pool_.alloc(this->stream_max_frame_bytes_);
    if (frame.data == nullptr) {
      ESP_LOGE(TAG, "No room in the PSRAM pool for the stream frames");
      return false;
    }
    frame.capacity = this->stream_max_frame_bytes_;
    StreamFrame *p = &frame;
    xQueueSend(this->stream_empty_q_, &p, 0);
  }
#ifdef USE_AUDIO
  if (this->speaker_ != nullptr) {
    this->stream_audio_out_ch_ = this->speaker_channel_mode_ == SpeakerChannelMode::SPEAKER_CHANNEL_STEREO ? 2 : 1;
    this->stream_audio_block_size_ = static_cast<size_t>(AUDIO_SAMPLE_RATE) * STREAM_AUDIO_BLOCK_MS / 1000 *
                                     this->stream_audio_out_ch_ * sizeof(int16_t);
    this->stream_audio_block_ = this->pool_.alloc(this->stream_audio_block_size_);
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
  if (!create_psram_task(stream_draw_task_entry_, "svp_stream_draw", 4096, this, 2, &this->stream_draw_task_, 1)) {
    ESP_LOGE(TAG, "Failed to create the stream draw task");
    return false;
  }
  if (!create_psram_task(stream_net_task_entry_, "svp_stream_net", 4096, this, 4, nullptr, tskNO_AFFINITY)) {
    ESP_LOGE(TAG, "Failed to create the stream network task");
    return false;
  }
  ESP_LOGCONFIG(TAG, "Listening on port %u for AVI streams and udisp frames, made for the %s", this->stream_port_,
                this->stream_direct_() ? "display" : "canvas");
  return true;
}

void SimpleVideoPlayer::stream_set_awake_(bool awake) {
  if (this->output_lock_ == nullptr)
    return;
  this->stream_awake_.store(awake, std::memory_order_release);
  this->stream_status_pending_.store(true, std::memory_order_release);
}

void SimpleVideoPlayer::stream_sleep(bool asleep) {
  this->stream_asleep_.store(asleep, std::memory_order_release);
  this->stream_status_pending_.store(true, std::memory_order_release);
}

void SimpleVideoPlayer::set_stream_fullscreen(bool fullscreen) {
#if defined(SVP_DSI_OUTPUT) && defined(SVP_CANVAS_OUTPUT)
  if (this->stream_fullscreen_.exchange(fullscreen, std::memory_order_acq_rel) == fullscreen)
    return;
  ESP_LOGI(TAG, "Streams are made for the %s", fullscreen ? "display" : "canvas");
  // A connected sender gets the new picture ('V') and restarts its stream for it.
  this->stream_depth_pending_.store(true, std::memory_order_release);
  if (this->stream_draw_task_ != nullptr)
    xTaskNotifyGive(this->stream_draw_task_);
#else
  if (fullscreen != this->stream_direct_())
    ESP_LOGW(TAG, "Streams can only be made for the %s here", this->stream_direct_() ? "display" : "canvas");
#endif
}

void SimpleVideoPlayer::stream_net_task_entry_(void *param) { static_cast<SimpleVideoPlayer *>(param)->stream_net_loop_(); }

void SimpleVideoPlayer::stream_net_loop_() {
  uint8_t *const buffer = this->stream_recv_buf_;

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
#ifdef USE_AUDIO
      this->stream_audio_left_ = 0;
      this->stream_audio_carry_len_ = 0;
#endif
#ifdef USE_AUDIO
      this->stream_rate_pending_.store(this->speaker_ != nullptr, std::memory_order_release);
#endif
      this->stream_depth_pending_.store(true, std::memory_order_release);
      this->stream_status_pending_.store(true, std::memory_order_release);
#ifdef SVP_STREAM_TOUCH
      if (this->stream_touch_q_ != nullptr)
        xQueueReset(this->stream_touch_q_);
      this->stream_last_touch_valid_ = false;
#endif
      this->stream_stats_since_ms_ = millis();
      this->stream_drawn_ = 0;
      this->stream_draw_us_ = 0;
      this->stream_draw_us_max_ = 0;
      this->stream_dropped_no_buffer_ = 0;
      this->stream_dropped_decode_ = 0;
      this->stream_connected_.store(true, std::memory_order_release);

      uint32_t last_recv = millis();
      uint32_t last_stream = last_recv;
      bool wifi_held = false;
      bool kind_known = false;
      bool sink_seen = false;
      bool sender_done = false;  // the sender's AVI is complete: the connection stays until it has played
      while (true) {
        BufferedFileReader *sink = nullptr;
        if (this->stream_avi_.load(std::memory_order_relaxed)) {
          // An AVI goes into the file reader's ring; leaving the socket unread while it is full is
          // the flow control. The connection ends with the playback session.
          sink = this->stream_sink_.load(std::memory_order_acquire);
          const PlayerState st = this->state_.load(std::memory_order_acquire);
          if (sink != nullptr) {
            sink_seen = true;
          } else if (sink_seen || (st != PlayerState::PLAYING && st != PlayerState::PAUSED)) {
            // 'E': how the AVI's playback ended -- 1 played to its end, 0 stopped on the panel.
            const uint8_t message[2] = {'E',
                                        static_cast<uint8_t>(this->stream_played_to_end_.load(std::memory_order_acquire))};
            ::send(client, message, sizeof(message), MSG_DONTWAIT);
            break;
          }
          if (sender_done) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
          }
          if (sink == nullptr || sink->stream_room() < STREAM_RECV_BYTES) {
            this->stream_send_pending_(client);
            vTaskDelay(pdMS_TO_TICKS(5));
            last_recv = millis();
            continue;
          }
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(client, &readable);
        struct timeval slice = {.tv_sec = 0, .tv_usec = 5000};
        const int ready = ::select(client + 1, &readable, nullptr, nullptr, &slice);
        this->stream_send_pending_(client);
        if (wifi_held && millis() - last_stream > ROAM_QUIET_MS)
          hold_wifi_for_stream(wifi_held, false);
        if (ready == 0) {
          if (millis() - last_recv > STREAM_RECV_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Sender silent for %u s, disconnecting", static_cast<unsigned>(STREAM_RECV_TIMEOUT_MS / 1000));
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
            // An AVI (svp_relay): played like a file from SD. Not while something else plays or is
            // about to; a session that still ends is waited for here (this task may wait, the main
            // loop may not).
            if (this->state_.load(std::memory_order_acquire) == PlayerState::PLAYING ||
                this->state_.load(std::memory_order_acquire) == PlayerState::PAUSED)
              break;
            for (uint32_t waited = 0; this->session_busy_() && waited < SESSION_END_WAIT_MS; waited += 10)
              vTaskDelay(pdMS_TO_TICKS(10));
            if (xSemaphoreTake(this->session_lock_, pdMS_TO_TICKS(SESSION_END_WAIT_MS)) != pdTRUE)
              break;
            const PlayerState st = this->state_.load(std::memory_order_acquire);
            if (this->session_busy_() || this->start_pending_.load(std::memory_order_acquire) ||
                st == PlayerState::PLAYING || st == PlayerState::PAUSED) {
              xSemaphoreGive(this->session_lock_);
              break;
            }
            this->net_session_ = true;
            this->stream_played_to_end_.store(false, std::memory_order_release);
            this->start_playback_("network stream");
            xSemaphoreGive(this->session_lock_);
            this->stream_avi_.store(true, std::memory_order_release);
            this->stream_connected_.store(false, std::memory_order_release);
            continue;
          }
        }
        const int received = ::recv(client, buffer, STREAM_RECV_BYTES, 0);
        if (received == 0 && sink != nullptr) {
          // The sender has sent all of its AVI (svp_relay half-closes): the rest of the ring plays, and
          // closing the connection once it has played tells the sender the end was reached.
          sink->end_stream();
          sender_done = true;
          hold_wifi_for_stream(wifi_held, false);
          continue;
        }
        if (received <= 0) {
          if (sink == nullptr)
            ESP_LOGI(TAG, "Sender disconnected");
          break;
        }
        last_recv = millis();
        if (received > HEARTBEAT_BYTES) {
          last_stream = last_recv;
          hold_wifi_for_stream(wifi_held, true);
        }
        if (sink != nullptr) {
          sink->push(buffer, static_cast<size_t>(received));
        } else {
          this->stream_feed_(buffer, static_cast<size_t>(received));
        }
      }

      if (this->stream_avi_.exchange(false, std::memory_order_acq_rel)) {
        // The rest of the ring still plays; the reader reports the end once it is drained.
        BufferedFileReader *sink = this->stream_sink_.load(std::memory_order_acquire);
        if (sink != nullptr)
          sink->end_stream();
      }
      hold_wifi_for_stream(wifi_held, false);
      // The stream is over; only logged when no file plays either.
      const uint32_t dropped = this->stream_dropped_no_buffer_ + this->stream_dropped_decode_;
      if ((this->stream_drawn_ > 0 || dropped > 0) &&
          this->state_.load(std::memory_order_acquire) == PlayerState::STOPPED) {
        const float secs = (millis() - this->stream_stats_since_ms_) / 1000.0f;
        ESP_LOGI(TAG,
                 "stream stats: %" PRIu32 " frames drawn in %.1f s (%.1f fps), draw avg %" PRIu32 " / max %" PRIu32
                 " us, %" PRIu32 " dropped (%" PRIu32 " no buffer, %" PRIu32 " decode)",
                 this->stream_drawn_, secs, secs > 0 ? this->stream_drawn_ / secs : 0.0f,
                 this->stream_drawn_ ? static_cast<uint32_t>(this->stream_draw_us_ / this->stream_drawn_) : 0u,
                 this->stream_draw_us_max_, dropped, this->stream_dropped_no_buffer_, this->stream_dropped_decode_);
      }
      if (this->stream_current_ != nullptr) {
        xQueueSend(this->stream_empty_q_, &this->stream_current_, 0);
        this->stream_current_ = nullptr;
      }
      this->stream_connected_.store(false, std::memory_order_release);
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

#ifdef USE_AUDIO
    if (this->stream_audio_left_ > 0) {
      const size_t take = std::min<size_t>(this->stream_audio_left_, len);
      this->stream_on_audio_(data, take);
      this->stream_audio_left_ -= take;
      data += take;
      len -= take;
      continue;
    }
#endif

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

#ifdef USE_AUDIO
    // Sound: width carries the channel count (0 = 1), height the rate (0 = 48000).
    if (type == UDISP_TYPE_PCM && this->stream_audio_block_ != nullptr) {
      this->stream_audio_src_ch_ = w == 0 ? 1 : static_cast<uint8_t>(w);
      this->stream_audio_rate_ = h == 0 ? STREAM_AUDIO_DEFAULT_RATE : h;
      this->stream_audio_left_ = total;
      continue;
    }
#endif
    // Heartbeats/END (and sound without a speaker) are counted out like any payload.
    if (type != UDISP_TYPE_JPG) {
      this->stream_skip_ = total;
      continue;
    }
    // Each rectangle is for the output its stream is made for now: bounds and target go with it.
    const bool direct = this->stream_direct_();
    uint32_t out_w = 0, out_h = 0;
#ifdef SVP_DSI_OUTPUT
    if (direct) {
      out_w = this->dsi_out_w_;
      out_h = this->dsi_out_h_;
    }
#endif
#ifdef SVP_CANVAS_OUTPUT
    if (!direct) {
      out_w = this->canvas_size_w_;
      out_h = this->canvas_size_h_;
    }
#endif
    const bool usable = w > 0 && h > 0 && static_cast<uint32_t>(x) + w <= out_w &&
                        static_cast<uint32_t>(y) + h <= out_h && total > 0 &&
                        total <= this->stream_max_frame_bytes_;
    if (!usable) {
      if (!this->stream_logged_bad_header_) {
        this->stream_logged_bad_header_ = true;
        ESP_LOGW(TAG,
                 "Ignoring frames: %ux%u at %u,%u of %" PRIu32 " bytes is not a JPEG rectangle inside %ux%u of at most "
                 "%u bytes",
                 w, h, x, y, total, static_cast<unsigned>(out_w), static_cast<unsigned>(out_h),
                 static_cast<unsigned>(this->stream_max_frame_bytes_));
      }
      this->stream_skip_ = total;
      continue;
    }

    // No free buffer: not reading the socket for a moment is the flow control; after that the
    // rectangle is counted out so the next header is found.
    StreamFrame *frame = nullptr;
    if (xQueueReceive(this->stream_empty_q_, &frame, pdMS_TO_TICKS(STREAM_FRAME_WAIT_MS)) != pdTRUE) {
      this->stream_dropped_no_buffer_++;
      this->stream_skip_ = total;
      continue;
    }
    frame->x = x;
    frame->y = y;
    frame->w = w;
    frame->h = h;
    frame->total = total;
    frame->received = 0;
    frame->direct = direct;
    this->stream_current_ = frame;
  }
}

void SimpleVideoPlayer::stream_send_pending_(int client) {
  if (this->stream_depth_pending_.exchange(false, std::memory_order_acq_rel)) {
    // 'C': the display's depth (udisp senders). 'V': the picture to send -- width, height, the
    // quarter turns clockwise from the source onto it, and the largest JPEG still a udisp sender
    // may send.
    uint8_t message[12];
    size_t at = 0;
    uint16_t width = 0, height = 0;
    uint8_t turns = 0;
    const uint32_t still = this->stream_max_frame_bytes_;
#ifdef SVP_DSI_OUTPUT
    message[at++] = 'C';
    message[at++] = static_cast<uint8_t>(this->dsi_fb_bpp_ * 8);
    if (this->stream_direct_()) {
      width = this->dsi_out_w_;
      height = this->dsi_out_h_;
      display::DisplayRotation rotation = this->dsi_->get_rotation();
#ifdef SVP_USE_LVGL
      // With LVGL the rotation is LVGL's own (ESPHome rejects a display rotation under LVGL).
      if (this->lvgl_component_ != nullptr)
        rotation = this->lvgl_component_->get_rotation();
#endif
      turns = static_cast<uint8_t>(static_cast<int>(rotation) / 90);
    }
#endif
#ifdef SVP_CANVAS_OUTPUT
    if (!this->stream_direct_()) {
      // The canvas turns with LVGL's own rotation: the picture goes in upright.
      width = this->canvas_size_w_;
      height = this->canvas_size_h_;
    }
#endif
    message[at++] = 'V';
    message[at++] = static_cast<uint8_t>(width & 0xFF);
    message[at++] = static_cast<uint8_t>(width >> 8);
    message[at++] = static_cast<uint8_t>(height & 0xFF);
    message[at++] = static_cast<uint8_t>(height >> 8);
    message[at++] = turns;
    for (int shift = 0; shift < 32; shift += 8)
      message[at++] = static_cast<uint8_t>(still >> shift);
    if (::send(client, message, at, MSG_DONTWAIT) != static_cast<int>(at))
      this->stream_depth_pending_.store(true, std::memory_order_release);
  }
#ifdef USE_AUDIO
  // 'A': the rate this panel's speaker runs at (kHz, then 50 Hz steps), so the sender captures at it.
  // 'N': the channel count a played AVI's sound must have (the speaker's, see init_audio_decoder_).
  // 'F': the audio codecs a played AVI may carry (audio_codec): 1 PCM, 2 MP3, 4 FLAC.
  if (this->stream_rate_pending_.exchange(false, std::memory_order_acq_rel)) {
    uint8_t codecs = 0;
#ifdef SVP_AUDIO_SUPPORT_PCM
    codecs |= 1;
#endif
#ifdef SVP_AUDIO_SUPPORT_MP3
    codecs |= 2;
#endif
#ifdef SVP_AUDIO_SUPPORT_FLAC
    codecs |= 4;
#endif
    // Sent again whenever an audio router's selection changes them.
    const uint32_t rate = this->wanted_audio_rate_.load(std::memory_order_acquire);
    const uint8_t message[7] = {'A',
                                static_cast<uint8_t>(rate / 1000),
                                static_cast<uint8_t>((rate % 1000) / 50),
                                'N',
                                this->wanted_audio_channels_.load(std::memory_order_acquire),
                                'F',
                                codecs};
    if (::send(client, message, sizeof(message), MSG_DONTWAIT) != static_cast<int>(sizeof(message)))
      this->stream_rate_pending_.store(true, std::memory_order_release);
  }
#endif
  if (this->stream_status_pending_.exchange(false, std::memory_order_acq_rel)) {
    const bool shown = this->stream_awake_.load(std::memory_order_acquire) &&
                       !this->stream_asleep_.load(std::memory_order_acquire);
    const uint8_t message[2] = {'S', static_cast<uint8_t>(shown ? 1 : 0)};
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
  decode_cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

  while (true) {
    StreamFrame *frame = nullptr;
    if (xQueueReceive(this->stream_filled_q_, &frame, portMAX_DELAY) != pdTRUE)
      continue;
    uint8_t *target = nullptr;
    size_t target_len = 0;
    bool output_ok = false;
    // While a file plays, starts or ends its picture wins: nothing is drawn, nor its outputs taken.
    const bool file_playing = this->session_busy_() || this->start_pending_.load(std::memory_order_acquire);
#ifdef SVP_DSI_OUTPUT
    const display::ColorBitness bitness =
        this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
    if (frame->direct && !file_playing && this->ensure_direct_target_()) {
      decode_cfg.output_format =
          this->dsi_fb_bpp_ == 3 ? JPEG_DECODE_OUT_FORMAT_RGB888 : JPEG_DECODE_OUT_FORMAT_RGB565;
      // Draw only once loop() has LVGL (if any) paused; released by loop() via notification, also
      // when the stream is made for the canvas meanwhile.
      while (this->stream_connected_.load(std::memory_order_acquire) && this->stream_direct_() &&
             !this->stream_output_ok_.load(std::memory_order_acquire))
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      output_ok = this->stream_output_ok_.load(std::memory_order_acquire);
    }
#endif
#ifdef SVP_CANVAS_OUTPUT
    if (!frame->direct && !file_playing && this->ensure_canvas_outputs_()) {
      // Canvas output: RGB888 like the video. While no file plays (output_lock_), the second output
      // buffer takes the decoded rectangle and the first is the page the canvas shows.
      decode_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
      output_ok = this->stream_connected_.load(std::memory_order_acquire);
    }
#endif
    // A playing file holds output_lock_ for its whole session: its picture wins.
    if (output_ok && xSemaphoreTake(this->output_lock_, 0) == pdTRUE) {
      // Read under the lock: an output given back to the pool goes under it too.
#ifdef SVP_DSI_OUTPUT
      if (frame->direct) {
        target = this->decode_target_;
        target_len = this->decode_target_len_;
      }
#endif
#ifdef SVP_CANVAS_OUTPUT
      if (!frame->direct) {
        target = this->output_buffers_[1];
        target_len = this->output_buffer_size_;
      }
#endif
      uint32_t out_size = 0;
      if (target != nullptr &&
          jpeg_decoder_process(this->hw_jpeg_decoder_, &decode_cfg, frame->data, frame->received, target,
                               static_cast<uint32_t>(target_len), &out_size) == ESP_OK &&
          out_size > 0) {
        const uint32_t start = micros();
#ifdef SVP_DSI_OUTPUT
        if (frame->direct) {
          // Decoded rows are padded to 16; x_pad skips the padding at the end of each row.
          const uint16_t padded_w = (frame->w + 15) & ~15;
          this->dsi_->draw_pixels_at(frame->x, frame->y, frame->w, frame->h, target, display::COLOR_ORDER_RGB,
                                     bitness, false, 0, 0, padded_w - frame->w);
        }
#endif
#ifdef SVP_CANVAS_OUTPUT
        if (!frame->direct)
          this->stream_copy_to_canvas_(frame->x, frame->y, frame->w, frame->h);
#endif
        const uint32_t took = micros() - start;
        this->stream_draw_us_ += took;
        if (took > this->stream_draw_us_max_)
          this->stream_draw_us_max_ = took;
        this->stream_drawn_++;
      } else {
        this->stream_dropped_decode_++;
      }
      xSemaphoreGive(this->output_lock_);
    }
    xQueueSend(this->stream_empty_q_, &frame, 0);
  }
}

#ifdef SVP_CANVAS_OUTPUT
void SimpleVideoPlayer::stream_copy_to_canvas_(const uint16_t x, const uint16_t y, const uint16_t w, const uint16_t h) {
  // Rows of the decoded rectangle are padded to 16 pixels; the page's rows are the canvas buffer's
  // (ALIGN_UP(width,16), as loop() hands it to LVGL). Bounds were checked against the canvas size
  // in stream_feed_().
  uint8_t *page = this->output_buffers_[0];
  const uint8_t *rect = this->output_buffers_[1];
  const size_t page_stride = static_cast<size_t>((this->canvas_size_w_ + 15) & ~15) * 3;
  const size_t rect_stride = static_cast<size_t>((w + 15) & ~15) * 3;
  for (uint16_t row = 0; row < h; row++)
    std::memcpy(page + (static_cast<size_t>(y) + row) * page_stride + static_cast<size_t>(x) * 3,
                rect + static_cast<size_t>(row) * rect_stride, static_cast<size_t>(w) * 3);
  // Written back to PSRAM: the hardware decoder writes this buffer by DMA when a file plays next,
  // and a dirty line evicted after that would put these pixels back over the new picture (Portall's
  // copy_to_canvas_ does the same). esp_cache_msync() flags: esp_mm/include/esp_cache.h.
  uint8_t *first = page + static_cast<size_t>(y) * page_stride + static_cast<size_t>(x) * 3;
  uint8_t *last = page + (static_cast<size_t>(y) + h - 1) * page_stride + (static_cast<size_t>(x) + w) * 3;
  esp_cache_msync(first, last - first,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  portENTER_CRITICAL(&this->stream_dirty_lock_);
  if (this->stream_dirty_) {
    this->stream_dirty_x1_ = std::min<int32_t>(this->stream_dirty_x1_, x);
    this->stream_dirty_y1_ = std::min<int32_t>(this->stream_dirty_y1_, y);
    this->stream_dirty_x2_ = std::max<int32_t>(this->stream_dirty_x2_, x + w - 1);
    this->stream_dirty_y2_ = std::max<int32_t>(this->stream_dirty_y2_, y + h - 1);
  } else {
    this->stream_dirty_x1_ = x;
    this->stream_dirty_y1_ = y;
    this->stream_dirty_x2_ = x + w - 1;
    this->stream_dirty_y2_ = y + h - 1;
    this->stream_dirty_ = true;
  }
  portEXIT_CRITICAL(&this->stream_dirty_lock_);
}
#endif

#ifdef USE_AUDIO
void SimpleVideoPlayer::stream_on_audio_(const uint8_t *data, size_t len) {
  // A playing file owns the speaker. Sound not played drops a split frame too.
  if (this->state_.load(std::memory_order_acquire) != PlayerState::STOPPED) {
    this->stream_audio_carry_len_ = 0;
    return;
  }
  const uint32_t rate = this->wanted_audio_rate_.load(std::memory_order_acquire);
  if (AUDIO_BITS_PER_SAMPLE != 16 || this->stream_audio_rate_ != rate || this->stream_audio_src_ch_ > 2) {
    if (!this->stream_logged_audio_format_) {
      this->stream_logged_audio_format_ = true;
      ESP_LOGW(TAG, "Sound of %u Hz, %u channel(s) does not fit the speaker (%u Hz, %u bit): not played",
               static_cast<unsigned>(this->stream_audio_rate_), this->stream_audio_src_ch_,
               static_cast<unsigned>(rate), static_cast<unsigned>(AUDIO_BITS_PER_SAMPLE));
    }
    this->stream_audio_carry_len_ = 0;
    return;
  }
  this->stream_logged_audio_format_ = false;
  if (this->speaker_->is_running() && this->stream_speaker_rate_ != rate)
    this->speaker_->stop();  // the sender follows an audio router's new output: started again in its rate
  if (!this->speaker_->is_running()) {
    this->speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(AUDIO_BITS_PER_SAMPLE, this->stream_audio_out_ch_, rate));
    this->speaker_->start();
    this->stream_speaker_rate_ = rate;
  }
  this->stream_last_audio_ms_.store(millis(), std::memory_order_release);
  this->stream_audio_on_.store(true, std::memory_order_release);

  const size_t in_frame = static_cast<size_t>(this->stream_audio_src_ch_) * sizeof(int16_t);
  if (this->stream_audio_carry_len_ >= in_frame)
    this->stream_audio_carry_len_ = 0;  // the channel count changed with a split frame pending
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
#endif  // USE_AUDIO

#ifdef SVP_STREAM_TOUCH
void StreamTouchListener::update(const touchscreen::TouchPoints_t &points) { this->parent_->stream_on_touch(points); }

void StreamTouchListener::release() {
  const touchscreen::TouchPoints_t none;
  this->parent_->stream_on_touch(none);
}

void SimpleVideoPlayer::stream_on_touch(const touchscreen::TouchPoints_t &points) {
  if (this->stream_touch_q_ == nullptr)
    return;
  // Touches go to a udisp sender whose picture is shown, and to an AVI sender while it plays.
  const bool udisp_shown =
      this->stream_connected_.load(std::memory_order_acquire) && this->stream_awake_.load(std::memory_order_acquire);
  if ((!udisp_shown && !this->stream_avi_.load(std::memory_order_acquire)) ||
      this->stream_asleep_.load(std::memory_order_acquire))
    return;
#ifdef SVP_CANVAS_OUTPUT
  // Canvas output: a contact in the canvas's own pixels, as LVGL sees the screen (its rotation,
  // lvgl_esphome.cpp's touch read callback); one outside the canvas is LVGL's alone. Runs on the
  // main loop (touchscreen.cpp defers send_touches_()), so asking LVGL is safe.
  const bool on_canvas = this->stream_avi_.load(std::memory_order_acquire) ? !this->direct_session_()
                                                                            : !this->stream_direct_();
  lv_area_t at{};
  if (on_canvas)
    lv_obj_get_coords(this->canvas_, &at);
#endif
  StreamTouch event{};
  for (const auto &point : points) {
    if (event.count >= STREAM_TOUCH_MAX)
      break;
    int32_t x = point.x, y = point.y;
#ifdef SVP_CANVAS_OUTPUT
    if (on_canvas) {
      if (this->lvgl_component_ != nullptr)
        this->lvgl_component_->rotate_coordinates(x, y);
      x -= at.x1;
      y -= at.y1;
      if (x < 0 || y < 0 || x >= this->canvas_size_w_ || y >= this->canvas_size_h_)
        continue;
    }
#endif
    event.id[event.count] = point.id;
    event.x[event.count] = static_cast<uint16_t>(x);
    event.y[event.count] = static_cast<uint16_t>(y);
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
