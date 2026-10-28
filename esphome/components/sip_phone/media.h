#pragma once

#ifdef USE_ESP32

#include "sdp.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include <lwip/sockets.h>

#include <atomic>
#include <memory>
#include <vector>

namespace esphome::sip_phone {

class G722Encoder;
class G722Decoder;

class MediaListener {
 public:
  virtual void on_media_dtmf(char digit) = 0;
  virtual void on_media_parked() = 0;
};

class MediaTransport {
 public:
  virtual void on_media_payload(const uint8_t *payload, size_t len, uint32_t timestamp) = 0;
};

struct MediaConfig {
  MediaFormat format;
  uint32_t pcm_rate{16000};
  uint8_t pcm_channels{1};
  int socket_fd{-1};
  struct sockaddr_in remote {};
  uint32_t ssrc{0};
  uint32_t opus_bitrate{64000};
  uint8_t opus_complexity{2};
  uint16_t playout_ms{60};
  bool send{true};
  bool receive{true};
  MediaTransport *transport{nullptr};
};

class CallMedia {
 public:
  static constexpr size_t MAX_PAYLOAD = 1280;
  static constexpr uint8_t HELD_PACKETS = 3;

  bool start(const MediaConfig &config, speaker::Speaker *speaker, MediaListener *listener, bool task_stack_in_psram);
  void request_stop();
  bool is_running() const { return this->task_.is_created(); }
  bool parked() const { return this->parked_.load(); }
  bool release();
  void on_mic_data(std::span<const uint8_t> data);
  void on_payload(const uint8_t *payload, size_t len);
  void set_mute(bool mute) { this->mute_.store(mute); }
  void send_dtmf(const char *digits);
  void update_remote(const struct sockaddr_in &remote, bool send, bool receive);

 protected:
  struct Held {
    bool used;
    uint16_t seq;
    uint16_t len;
    uint8_t data[MAX_PAYLOAD];
  };

  static void task_entry_(void *param);
  void run_();
  void drain_socket_();
  void drain_payloads_();
  void start_playout_();
  void handle_packet_(const uint8_t *packet, size_t len);
  void handle_dtmf_(const uint8_t *payload, size_t len, uint32_t timestamp);
  void play_payload_(const uint8_t *payload, size_t len);
  void conceal_(uint16_t frames, const uint8_t *next_payload, size_t next_len);
  void flush_held_();
  void output_pcm_(const int16_t *pcm, size_t samples);
  void process_tx_();
  void send_audio_frame_();
  bool send_dtmf_frame_();
  void send_rtp_(uint8_t payload_type, bool marker, uint32_t timestamp, const uint8_t *payload, size_t len);
  bool alloc_codec_();
  void free_codec_();

  MediaConfig config_;
  speaker::Speaker *speaker_{nullptr};
  MediaListener *listener_{nullptr};
  StaticTask task_;
  int wake_fd_{-1};
  std::atomic<bool> stop_{false};
  std::atomic<bool> parked_{false};
  std::atomic<bool> mute_{false};
  std::atomic<bool> tx_active_{false};
  std::atomic<uint8_t> mic_users_{0};

  std::unique_ptr<ring_buffer::RingBuffer> tx_ring_;
  std::unique_ptr<ring_buffer::RingBuffer> rx_ring_;
  uint16_t rx_pending_{0};
  uint8_t *rx_packet_{nullptr};
  uint8_t *tx_packet_{nullptr};
  int16_t *pcm_{nullptr};
  int16_t *last_frame_{nullptr};
  Held *held_{nullptr};
  size_t frame_bytes_{0};
  size_t frame_samples_{0};
  size_t pcm_capacity_{0};
  uint32_t ts_step_{0};

  void *opus_encoder_{nullptr};
  void *opus_decoder_{nullptr};
  G722Encoder *g722_encoder_{nullptr};
  G722Decoder *g722_decoder_{nullptr};

  uint16_t tx_seq_{0};
  uint32_t tx_ts_{0};
  bool tx_marker_{true};
  uint16_t rx_expected_{0};
  bool rx_started_{false};
  uint32_t last_dtmf_ts_{0};
  bool dtmf_seen_{false};

  Mutex remote_lock_;
  struct sockaddr_in remote_ {};
  std::atomic<bool> send_{true};
  std::atomic<bool> receive_{true};

  Mutex dtmf_lock_;
  char dtmf_queue_[33]{};
  uint8_t dtmf_head_{0};
  uint8_t dtmf_tail_{0};
  int8_t dtmf_event_{-1};
  uint8_t dtmf_frames_{0};
  uint32_t dtmf_ts_{0};
  uint8_t dtmf_gap_{0};
};

}

#endif
