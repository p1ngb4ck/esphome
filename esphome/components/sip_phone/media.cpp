#include "media.h"

#ifdef USE_ESP32

#include "g711.h"
#include "g722.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_random.h>
#include <esp_vfs_eventfd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <new>
#include <unistd.h>

#ifdef USE_SIP_PHONE_OPUS
#include <opus.h>
#endif

namespace esphome::sip_phone {

static const char *const TAG = "sip_phone.media";

static constexpr uint32_t MEDIA_TASK_STACK = 8192;
static constexpr UBaseType_t MEDIA_TASK_PRIORITY = 10;
static constexpr size_t RX_PACKET_SIZE = 1500;
static constexpr uint32_t TX_RING_MS = 100;
static constexpr size_t RX_RING_BYTES = 4096;
static constexpr uint16_t MAX_CONCEAL = 5;
static constexpr uint8_t DTMF_FRAMES = 8;
static constexpr uint8_t DTMF_GAP_FRAMES = 3;

template<typename T> static T *alloc_external(size_t count) {
  RAMAllocator<T> allocator(RAMAllocator<T>::ALLOC_EXTERNAL);
  return allocator.allocate(count);
}

template<typename T> static void free_external(T *&ptr, size_t count) {
  if (ptr == nullptr)
    return;
  RAMAllocator<T> allocator(RAMAllocator<T>::ALLOC_EXTERNAL);
  allocator.deallocate(ptr, count);
  ptr = nullptr;
}

static int8_t dtmf_code(char digit) {
  if (digit >= '0' && digit <= '9')
    return static_cast<int8_t>(digit - '0');
  if (digit == '*')
    return 10;
  if (digit == '#')
    return 11;
  if (digit >= 'A' && digit <= 'D')
    return static_cast<int8_t>(12 + digit - 'A');
  if (digit >= 'a' && digit <= 'd')
    return static_cast<int8_t>(12 + digit - 'a');
  return -1;
}

static char dtmf_char(uint8_t code) {
  static const char DIGITS[] = "0123456789*#ABCD";
  return code < 16 ? DIGITS[code] : '\0';
}

bool CallMedia::start(const MediaConfig &config, speaker::Speaker *speaker, MediaListener *listener,
                      bool task_stack_in_psram) {
  this->config_ = config;
  this->speaker_ = speaker;
  this->listener_ = listener;
  const MediaFormat &format = config.format;
  this->frame_samples_ = config.pcm_rate * format.ptime / 1000;
  this->frame_bytes_ = this->frame_samples_ * config.pcm_channels * sizeof(int16_t);
  this->ts_step_ = codec_rtp_clock(format.codec) * format.ptime / 1000;
  const size_t opus_max = static_cast<size_t>(config.pcm_rate) * 60 / 1000 * config.pcm_channels;
  this->pcm_capacity_ = std::max(std::max(opus_max, this->frame_bytes_ / sizeof(int16_t)), 2 * MAX_PAYLOAD);

  this->rx_packet_ = alloc_external<uint8_t>(RX_PACKET_SIZE);
  this->tx_packet_ = alloc_external<uint8_t>(12 + MAX_PAYLOAD);
  this->pcm_ = alloc_external<int16_t>(this->pcm_capacity_);
  this->last_frame_ = alloc_external<int16_t>(this->frame_bytes_ / sizeof(int16_t));
  this->held_ = alloc_external<Held>(HELD_PACKETS);
  this->tx_ring_ = ring_buffer::RingBuffer::create(config.pcm_rate * config.pcm_channels * sizeof(int16_t) *
                                                   TX_RING_MS / 1000);
  if (config.transport != nullptr)
    this->rx_ring_ = ring_buffer::RingBuffer::create(RX_RING_BYTES);
  if (this->rx_packet_ == nullptr || this->tx_packet_ == nullptr || this->pcm_ == nullptr ||
      this->last_frame_ == nullptr || this->held_ == nullptr || this->tx_ring_ == nullptr ||
      (config.transport != nullptr && this->rx_ring_ == nullptr)) {
    ESP_LOGE(TAG, "No PSRAM for the call's buffers");
    this->release();
    return false;
  }
  memset(this->last_frame_, 0, this->frame_bytes_);
  for (uint8_t i = 0; i < HELD_PACKETS; i++)
    this->held_[i].used = false;

  const esp_vfs_eventfd_config_t efd_config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&efd_config);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    this->wake_fd_ = eventfd(0, 0);
  if (this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "No eventfd for the call");
    this->release();
    return false;
  }

  this->remote_ = config.remote;
  this->send_.store(config.send);
  this->receive_.store(config.receive);
  this->tx_seq_ = static_cast<uint16_t>(esp_random());
  this->tx_ts_ = esp_random();
  this->tx_marker_ = true;
  this->rx_started_ = false;
  this->rx_pending_ = 0;
  this->dtmf_seen_ = false;
  this->dtmf_event_ = -1;
  this->dtmf_gap_ = 0;
  this->dtmf_head_ = 0;
  this->dtmf_tail_ = 0;
  this->stop_.store(false);
  this->parked_.store(false);
  this->mute_.store(false);

  if (!this->task_.create(task_entry_, "sip_media", MEDIA_TASK_STACK, this, MEDIA_TASK_PRIORITY, task_stack_in_psram,
                          LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for the call's task");
    this->release();
    return false;
  }
  this->tx_active_.store(true);
  return true;
}

void CallMedia::request_stop() {
  this->tx_active_.store(false);
  this->stop_.store(true);
  if (this->wake_fd_ >= 0) {
    const uint64_t one = 1;
    ::write(this->wake_fd_, &one, sizeof(one));
  }
}

bool CallMedia::release() {
  if (this->mic_users_.load() != 0)
    return false;
  if (this->task_.is_created() && !this->task_.deallocate())
    return false;
  this->parked_.store(false);
  free_external(this->rx_packet_, RX_PACKET_SIZE);
  free_external(this->tx_packet_, 12 + MAX_PAYLOAD);
  free_external(this->pcm_, this->pcm_capacity_);
  free_external(this->last_frame_, this->frame_bytes_ / sizeof(int16_t));
  free_external(this->held_, HELD_PACKETS);
  this->tx_ring_.reset();
  this->rx_ring_.reset();
  if (this->wake_fd_ >= 0) {
    ::close(this->wake_fd_);
    this->wake_fd_ = -1;
  }
  if (this->config_.socket_fd >= 0) {
    ::close(this->config_.socket_fd);
    this->config_.socket_fd = -1;
  }
  return true;
}

void CallMedia::on_mic_data(const std::vector<uint8_t> &data) {
  this->mic_users_.fetch_add(1);
  if (this->tx_active_.load() && this->tx_ring_ != nullptr) {
    this->tx_ring_->write(data.data(), data.size());
    if (this->tx_ring_->available() >= this->frame_bytes_) {
      const uint64_t one = 1;
      ::write(this->wake_fd_, &one, sizeof(one));
    }
  }
  this->mic_users_.fetch_sub(1);
}

void CallMedia::on_payload(const uint8_t *payload, size_t len) {
  if (len == 0 || len > MAX_PAYLOAD)
    return;
  this->mic_users_.fetch_add(1);
  if (this->tx_active_.load() && this->rx_ring_ != nullptr && this->rx_ring_->free() >= sizeof(uint16_t) + len) {
    const uint16_t length = static_cast<uint16_t>(len);
    this->rx_ring_->write_without_replacement(&length, sizeof(length), 0);
    this->rx_ring_->write_without_replacement(payload, len, 0);
    const uint64_t one = 1;
    ::write(this->wake_fd_, &one, sizeof(one));
  }
  this->mic_users_.fetch_sub(1);
}

void CallMedia::send_dtmf(const char *digits) {
  LockGuard guard(this->dtmf_lock_);
  for (const char *c = digits; *c != '\0'; c++) {
    if (dtmf_code(*c) < 0)
      continue;
    const uint8_t next = static_cast<uint8_t>((this->dtmf_tail_ + 1) % sizeof(this->dtmf_queue_));
    if (next == this->dtmf_head_)
      break;
    this->dtmf_queue_[this->dtmf_tail_] = *c;
    this->dtmf_tail_ = next;
  }
}

void CallMedia::update_remote(const struct sockaddr_in &remote, bool send, bool receive) {
  {
    LockGuard guard(this->remote_lock_);
    this->remote_ = remote;
  }
  this->send_.store(send);
  this->receive_.store(receive);
}

void CallMedia::task_entry_(void *param) { static_cast<CallMedia *>(param)->run_(); }

void CallMedia::run_() {
  if (this->alloc_codec_()) {
    const int sock = this->config_.socket_fd;
    const int max_fd = std::max(sock, this->wake_fd_);
    while (!this->stop_.load()) {
      fd_set readable;
      FD_ZERO(&readable);
      if (sock >= 0)
        FD_SET(sock, &readable);
      FD_SET(this->wake_fd_, &readable);
      const int n = ::select(max_fd + 1, &readable, nullptr, nullptr, nullptr);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (FD_ISSET(this->wake_fd_, &readable)) {
        uint64_t value = 0;
        ::read(this->wake_fd_, &value, sizeof(value));
      }
      if (this->stop_.load())
        break;
      if (sock >= 0 && FD_ISSET(sock, &readable))
        this->drain_socket_();
      if (this->rx_ring_ != nullptr)
        this->drain_payloads_();
      this->process_tx_();
    }
  }
  this->free_codec_();
  this->parked_.store(true);
  this->listener_->on_media_parked();
  vTaskSuspend(nullptr);
}

bool CallMedia::alloc_codec_() {
  switch (this->config_.format.codec) {
#ifdef USE_SIP_PHONE_OPUS
    case Codec::OPUS: {
      int err = 0;
      const int channels = this->config_.pcm_channels;
      OpusEncoder *encoder = opus_encoder_create(static_cast<opus_int32>(this->config_.pcm_rate), channels,
                                                 channels > 1 ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &err);
      if (encoder == nullptr || err != OPUS_OK) {
        ESP_LOGE(TAG, "Opus encoder: %d", err);
        return false;
      }
      opus_encoder_ctl(encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(this->config_.opus_bitrate)));
      opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(this->config_.opus_complexity));
      opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
      opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
      if (channels > 1 && !this->config_.format.remote_stereo)
        opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(1));
      this->opus_encoder_ = encoder;
      OpusDecoder *decoder = opus_decoder_create(static_cast<opus_int32>(this->config_.pcm_rate), channels, &err);
      if (decoder == nullptr || err != OPUS_OK) {
        ESP_LOGE(TAG, "Opus decoder: %d", err);
        return false;
      }
      this->opus_decoder_ = decoder;
      return true;
    }
#endif
    case Codec::G722: {
      G722Encoder *encoder = alloc_external<G722Encoder>(1);
      G722Decoder *decoder = alloc_external<G722Decoder>(1);
      if (encoder == nullptr || decoder == nullptr) {
        free_external(encoder, 1);
        free_external(decoder, 1);
        ESP_LOGE(TAG, "No PSRAM for G.722");
        return false;
      }
      this->g722_encoder_ = new (encoder) G722Encoder();
      this->g722_decoder_ = new (decoder) G722Decoder();
      this->g722_encoder_->reset();
      this->g722_decoder_->reset();
      return true;
    }
    case Codec::PCMA:
    case Codec::PCMU:
      return true;
    default:
      ESP_LOGE(TAG, "Codec %s not built in", codec_name(this->config_.format.codec));
      return false;
  }
}

void CallMedia::free_codec_() {
#ifdef USE_SIP_PHONE_OPUS
  if (this->opus_encoder_ != nullptr) {
    opus_encoder_destroy(static_cast<OpusEncoder *>(this->opus_encoder_));
    this->opus_encoder_ = nullptr;
  }
  if (this->opus_decoder_ != nullptr) {
    opus_decoder_destroy(static_cast<OpusDecoder *>(this->opus_decoder_));
    this->opus_decoder_ = nullptr;
  }
#endif
  free_external(this->g722_encoder_, 1);
  free_external(this->g722_decoder_, 1);
}

void CallMedia::drain_socket_() {
  for (;;) {
    const ssize_t len =
        ::recvfrom(this->config_.socket_fd, this->rx_packet_, RX_PACKET_SIZE, MSG_DONTWAIT, nullptr, nullptr);
    if (len <= 0)
      return;
    this->handle_packet_(this->rx_packet_, static_cast<size_t>(len));
  }
}

void CallMedia::drain_payloads_() {
  for (;;) {
    if (this->rx_pending_ == 0) {
      if (this->rx_ring_->available() < sizeof(uint16_t))
        return;
      uint16_t length = 0;
      this->rx_ring_->read(&length, sizeof(length), 0);
      this->rx_pending_ = length;
    }
    if (this->rx_ring_->available() < this->rx_pending_)
      return;
    const size_t len = this->rx_ring_->read(this->rx_packet_, this->rx_pending_, 0);
    this->rx_pending_ = 0;
    if (!this->receive_.load())
      continue;
    this->start_playout_();
    this->play_payload_(this->rx_packet_, len);
  }
}

void CallMedia::start_playout_() {
  if (this->rx_started_)
    return;
  this->rx_started_ = true;
  memset(this->pcm_, 0, this->pcm_capacity_ * sizeof(int16_t));
  size_t silence = static_cast<size_t>(this->config_.pcm_rate) * this->config_.pcm_channels * sizeof(int16_t) *
                   this->config_.playout_ms / 1000;
  while (silence > 0) {
    const size_t chunk = std::min(silence, this->pcm_capacity_ * sizeof(int16_t));
    this->speaker_->play(reinterpret_cast<const uint8_t *>(this->pcm_), chunk, 0);
    silence -= chunk;
  }
}

void CallMedia::handle_packet_(const uint8_t *packet, size_t len) {
  if (len < 12 || (packet[0] >> 6) != 2 || !this->receive_.load())
    return;
  size_t header = 12 + 4 * (packet[0] & 0x0F);
  if (packet[0] & 0x10) {
    if (len < header + 4)
      return;
    header += 4 + 4 * ((packet[header + 2] << 8) | packet[header + 3]);
  }
  size_t end = len;
  if (packet[0] & 0x20) {
    const uint8_t padding = packet[len - 1];
    if (padding > end)
      return;
    end -= padding;
  }
  if (end <= header)
    return;
  const uint8_t pt = packet[1] & 0x7F;
  const uint16_t seq = static_cast<uint16_t>((packet[2] << 8) | packet[3]);
  const uint32_t timestamp = (static_cast<uint32_t>(packet[4]) << 24) | (static_cast<uint32_t>(packet[5]) << 16) |
                             (static_cast<uint32_t>(packet[6]) << 8) | packet[7];
  const uint8_t *payload = packet + header;
  const size_t payload_len = end - header;
  const MediaFormat &format = this->config_.format;
  if (format.dtmf_payload_type != 0 && pt == format.dtmf_payload_type) {
    this->handle_dtmf_(payload, payload_len, timestamp);
    return;
  }
  if (pt != format.payload_type || payload_len > MAX_PAYLOAD)
    return;

  if (!this->rx_started_) {
    this->rx_expected_ = seq;
    this->start_playout_();
  }
  const int16_t diff = static_cast<int16_t>(seq - this->rx_expected_);
  if (diff < 0)
    return;
  if (diff == 0) {
    this->play_payload_(payload, payload_len);
    this->rx_expected_++;
    this->flush_held_();
    return;
  }
  if (diff > 50) {
    for (uint8_t i = 0; i < HELD_PACKETS; i++)
      this->held_[i].used = false;
    this->play_payload_(payload, payload_len);
    this->rx_expected_ = static_cast<uint16_t>(seq + 1);
    return;
  }
  Held *slot = nullptr;
  for (uint8_t i = 0; i < HELD_PACKETS; i++) {
    if (this->held_[i].used && this->held_[i].seq == seq)
      return;
    if (!this->held_[i].used && slot == nullptr)
      slot = &this->held_[i];
  }
  if (slot != nullptr) {
    slot->used = true;
    slot->seq = seq;
    slot->len = static_cast<uint16_t>(payload_len);
    memcpy(slot->data, payload, payload_len);
    for (uint8_t i = 0; i < HELD_PACKETS; i++) {
      if (!this->held_[i].used)
        return;
    }
  }
  Held *lowest = nullptr;
  for (uint8_t i = 0; i < HELD_PACKETS; i++) {
    if (this->held_[i].used &&
        (lowest == nullptr || static_cast<int16_t>(this->held_[i].seq - lowest->seq) < 0))
      lowest = &this->held_[i];
  }
  if (lowest == nullptr)
    return;
  const uint16_t gap = static_cast<uint16_t>(lowest->seq - this->rx_expected_);
  this->conceal_(std::min(gap, MAX_CONCEAL), lowest->data, lowest->len);
  this->rx_expected_ = lowest->seq;
  this->flush_held_();
  if (slot == nullptr)
    this->handle_packet_(packet, len);
}

void CallMedia::flush_held_() {
  bool found = true;
  while (found) {
    found = false;
    for (uint8_t i = 0; i < HELD_PACKETS; i++) {
      Held &held = this->held_[i];
      if (held.used && held.seq == this->rx_expected_) {
        held.used = false;
        this->play_payload_(held.data, held.len);
        this->rx_expected_++;
        found = true;
      }
    }
  }
}

void CallMedia::handle_dtmf_(const uint8_t *payload, size_t len, uint32_t timestamp) {
  if (len < 4)
    return;
  if (this->dtmf_seen_ && timestamp == this->last_dtmf_ts_)
    return;
  this->dtmf_seen_ = true;
  this->last_dtmf_ts_ = timestamp;
  const char digit = dtmf_char(payload[0]);
  if (digit != '\0')
    this->listener_->on_media_dtmf(digit);
}

void CallMedia::play_payload_(const uint8_t *payload, size_t len) {
  size_t samples = 0;
  switch (this->config_.format.codec) {
#ifdef USE_SIP_PHONE_OPUS
    case Codec::OPUS: {
      const int frames = opus_decode(static_cast<OpusDecoder *>(this->opus_decoder_), payload,
                                     static_cast<opus_int32>(len), this->pcm_,
                                     static_cast<int>(this->pcm_capacity_ / this->config_.pcm_channels), 0);
      if (frames <= 0)
        return;
      samples = static_cast<size_t>(frames) * this->config_.pcm_channels;
      break;
    }
#endif
    case Codec::G722:
      samples = this->g722_decoder_->decode(payload, len, this->pcm_);
      break;
    case Codec::PCMA:
      for (size_t i = 0; i < len; i++)
        this->pcm_[i] = g711::alaw_to_linear(payload[i]);
      samples = len;
      break;
    case Codec::PCMU:
      for (size_t i = 0; i < len; i++)
        this->pcm_[i] = g711::ulaw_to_linear(payload[i]);
      samples = len;
      break;
    default:
      return;
  }
  if (this->config_.format.codec != Codec::OPUS) {
    const size_t keep = std::min(samples, this->frame_bytes_ / sizeof(int16_t));
    memcpy(this->last_frame_, this->pcm_ + samples - keep, keep * sizeof(int16_t));
  }
  this->output_pcm_(this->pcm_, samples);
}

void CallMedia::conceal_(uint16_t frames, const uint8_t *next_payload, size_t next_len) {
  const size_t frame_samples = this->frame_bytes_ / sizeof(int16_t);
  for (uint16_t i = 0; i < frames; i++) {
#ifdef USE_SIP_PHONE_OPUS
    if (this->config_.format.codec == Codec::OPUS) {
      const bool fec = i + 1 == frames && next_payload != nullptr;
      const int decoded = opus_decode(static_cast<OpusDecoder *>(this->opus_decoder_), fec ? next_payload : nullptr,
                                      fec ? static_cast<opus_int32>(next_len) : 0, this->pcm_,
                                      static_cast<int>(this->frame_samples_), fec ? 1 : 0);
      if (decoded > 0)
        this->output_pcm_(this->pcm_, static_cast<size_t>(decoded) * this->config_.pcm_channels);
      continue;
    }
#endif
    for (size_t s = 0; s < frame_samples; s++)
      this->last_frame_[s] = static_cast<int16_t>(this->last_frame_[s] >> 1);
    this->output_pcm_(this->last_frame_, frame_samples);
  }
}

void CallMedia::output_pcm_(const int16_t *pcm, size_t samples) {
  this->speaker_->play(reinterpret_cast<const uint8_t *>(pcm), samples * sizeof(int16_t), 0);
}

void CallMedia::process_tx_() {
  while (this->tx_ring_->available() >= this->frame_bytes_) {
    this->tx_ring_->read(this->pcm_, this->frame_bytes_, 0);
    if (!this->send_.load()) {
      this->tx_ts_ += this->ts_step_;
      continue;
    }
    if (this->send_dtmf_frame_())
      continue;
    if (this->mute_.load())
      memset(this->pcm_, 0, this->frame_bytes_);
    this->send_audio_frame_();
  }
}

void CallMedia::send_audio_frame_() {
  uint8_t *payload = this->tx_packet_ + 12;
  size_t len = 0;
  switch (this->config_.format.codec) {
#ifdef USE_SIP_PHONE_OPUS
    case Codec::OPUS: {
      const opus_int32 bytes = opus_encode(static_cast<OpusEncoder *>(this->opus_encoder_), this->pcm_,
                                           static_cast<int>(this->frame_samples_), payload, MAX_PAYLOAD);
      if (bytes <= 0)
        return;
      len = static_cast<size_t>(bytes);
      break;
    }
#endif
    case Codec::G722:
      len = this->g722_encoder_->encode(this->pcm_, this->frame_samples_, payload);
      break;
    case Codec::PCMA:
      for (size_t i = 0; i < this->frame_samples_; i++)
        payload[i] = g711::linear_to_alaw(this->pcm_[i]);
      len = this->frame_samples_;
      break;
    case Codec::PCMU:
      for (size_t i = 0; i < this->frame_samples_; i++)
        payload[i] = g711::linear_to_ulaw(this->pcm_[i]);
      len = this->frame_samples_;
      break;
    default:
      return;
  }
  this->send_rtp_(this->config_.format.payload_type, this->tx_marker_, this->tx_ts_, payload, len);
  this->tx_marker_ = false;
  this->tx_ts_ += this->ts_step_;
}

bool CallMedia::send_dtmf_frame_() {
  if (this->config_.format.dtmf_payload_type == 0)
    return false;
  if (this->dtmf_event_ < 0) {
    if (this->dtmf_gap_ > 0) {
      this->dtmf_gap_--;
      return false;
    }
    char digit = '\0';
    {
      LockGuard guard(this->dtmf_lock_);
      if (this->dtmf_head_ != this->dtmf_tail_) {
        digit = this->dtmf_queue_[this->dtmf_head_];
        this->dtmf_head_ = static_cast<uint8_t>((this->dtmf_head_ + 1) % sizeof(this->dtmf_queue_));
      }
    }
    if (digit == '\0')
      return false;
    this->dtmf_event_ = dtmf_code(digit);
    this->dtmf_frames_ = 0;
    this->dtmf_ts_ = this->tx_ts_;
  }
  this->dtmf_frames_++;
  const bool end = this->dtmf_frames_ >= DTMF_FRAMES;
  const uint32_t duration = this->dtmf_frames_ * this->ts_step_;
  uint8_t *payload = this->tx_packet_ + 12;
  payload[0] = static_cast<uint8_t>(this->dtmf_event_);
  payload[1] = static_cast<uint8_t>((end ? 0x80 : 0x00) | 10);
  payload[2] = static_cast<uint8_t>(duration >> 8);
  payload[3] = static_cast<uint8_t>(duration);
  const uint8_t copies = end ? 3 : 1;
  for (uint8_t i = 0; i < copies; i++)
    this->send_rtp_(this->config_.format.dtmf_payload_type, this->dtmf_frames_ == 1 && i == 0, this->dtmf_ts_,
                    payload, 4);
  this->tx_ts_ += this->ts_step_;
  if (end) {
    this->dtmf_event_ = -1;
    this->dtmf_gap_ = DTMF_GAP_FRAMES;
  }
  return true;
}

void CallMedia::send_rtp_(uint8_t payload_type, bool marker, uint32_t timestamp, const uint8_t *payload,
                          size_t len) {
  if (this->config_.transport != nullptr) {
    this->config_.transport->on_media_payload(payload, len, timestamp);
    return;
  }
  uint8_t *packet = this->tx_packet_;
  if (payload != packet + 12)
    memmove(packet + 12, payload, len);
  packet[0] = 0x80;
  packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (payload_type & 0x7F));
  packet[2] = static_cast<uint8_t>(this->tx_seq_ >> 8);
  packet[3] = static_cast<uint8_t>(this->tx_seq_);
  packet[4] = static_cast<uint8_t>(timestamp >> 24);
  packet[5] = static_cast<uint8_t>(timestamp >> 16);
  packet[6] = static_cast<uint8_t>(timestamp >> 8);
  packet[7] = static_cast<uint8_t>(timestamp);
  const uint32_t ssrc = this->config_.ssrc;
  packet[8] = static_cast<uint8_t>(ssrc >> 24);
  packet[9] = static_cast<uint8_t>(ssrc >> 16);
  packet[10] = static_cast<uint8_t>(ssrc >> 8);
  packet[11] = static_cast<uint8_t>(ssrc);
  this->tx_seq_++;
  struct sockaddr_in remote;
  {
    LockGuard guard(this->remote_lock_);
    remote = this->remote_;
  }
  ::sendto(this->config_.socket_fd, packet, 12 + len, 0, reinterpret_cast<const struct sockaddr *>(&remote),
           sizeof(remote));
}

}

#endif
