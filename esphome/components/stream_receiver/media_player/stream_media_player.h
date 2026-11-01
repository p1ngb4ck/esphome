#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/components/display/display.h"
#include "esphome/components/display/video_output.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/core/component.h"
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#ifdef USE_STREAM_RESAMPLER
#include "esphome/components/resampler/speaker/resampler_speaker.h"
#endif
#ifdef USE_STREAM_AVI
#include "esphome/components/avi/media_player/avi_media_player.h"
#endif
#ifdef USE_STREAM_TOUCH
#include "esphome/components/touchscreen/touchscreen.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome::stream_receiver {

class StreamMediaPlayer;

#ifdef USE_STREAM_TOUCH
class StreamTouchListener : public touchscreen::TouchListener {
 public:
  explicit StreamTouchListener(StreamMediaPlayer *parent) : parent_(parent) {}
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;

 protected:
  StreamMediaPlayer *parent_;
};
#endif

class StreamMediaPlayer : public Component, public media_player::MediaPlayer {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_frame_size(size_t size) { this->max_frame_bytes_ = size; }
  void set_video_output(display::VideoOutput *output) { this->output_ = output; }
  void set_codecs(uint8_t codecs) { this->codecs_ = codecs; }
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_sound(uint32_t rate, uint8_t channels) {
    this->sound_rate_ = rate;
    this->sound_channels_ = channels;
  }
#endif
#ifdef USE_STREAM_RESAMPLER
  void set_resampler(resampler::ResamplerSpeaker *resampler) { this->resampler_ = resampler; }
#endif
#ifdef USE_STREAM_AVI
  void set_avi_player(avi::AviMediaPlayer *player) { this->avi_ = player; }
#endif
#ifdef USE_STREAM_TOUCH
  void set_touchscreen(touchscreen::Touchscreen *touchscreen) { this->touchscreen_ = touchscreen; }
  void on_touch(const touchscreen::TouchPoints_t &points);
#endif

  media_player::MediaPlayerTraits get_traits() override;

 protected:
  struct Frame {
    uint8_t *data{nullptr};
    size_t capacity{0};
    uint16_t x{0};
    uint16_t y{0};
    uint16_t w{0};
    uint16_t h{0};
    uint32_t total{0};
    uint32_t received{0};
  };
  static constexpr uint8_t FRAME_COUNT = 4;
  static constexpr uint8_t TOUCH_MAX = 5;
  struct Touch {
    uint8_t count{0};
    uint8_t id[TOUCH_MAX]{};
    uint16_t x[TOUCH_MAX]{};
    uint16_t y[TOUCH_MAX]{};
  };

  void control(const media_player::MediaPlayerCall &call) override;
  static void net_entry_(void *arg);
  void net_loop_();
  void serve_(int client);
  static void draw_entry_(void *arg);
  void draw_loop_();
  void wake_();
  static void room_cb_(void *arg);
  bool take_buffers_();
  void give_buffers_();
  void feed_(const uint8_t *data, size_t len);
  void send_pending_(int client);
  void hold_wifi_(bool &held, bool want);
#ifdef USE_SPEAKER
  void play_sound_(const uint8_t *data, size_t len);
  void emit_sound_(const uint8_t *src, size_t frames);
  void flush_sound_();
#endif

  uint16_t port_{0};
  size_t max_frame_bytes_{128 * 1024};
  uint8_t codecs_{1};
  display::VideoOutput *output_{nullptr};
  display::VideoCanvas *canvas_{nullptr};
  int wake_fd_{-1};
  QueueHandle_t empty_q_{nullptr};
  QueueHandle_t filled_q_{nullptr};
  StaticQueue_t empty_store_{};
  StaticQueue_t filled_store_{};
  Frame *empty_slots_[FRAME_COUNT]{};
  Frame *filled_slots_[FRAME_COUNT]{};
  TaskHandle_t draw_task_{nullptr};
  Frame frames_[FRAME_COUNT]{};
  Frame *current_{nullptr};
  uint8_t *recv_buf_{nullptr};
  uint8_t header_[16]{};
  size_t header_len_{0};
  uint32_t skip_{0};
  uint16_t page_w_{0};
  uint16_t page_h_{0};
  bool logged_bad_header_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> drop_{false};
  std::atomic<bool> draw_busy_{false};
  std::atomic<bool> close_page_{false};
  std::atomic<bool> state_changed_{false};
  std::atomic<bool> view_pending_{false};
  std::atomic<bool> sound_pending_{false};
  std::atomic<bool> status_pending_{false};
  std::atomic<uint32_t> echo_{0};
#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
  uint32_t sound_rate_{48000};
  uint8_t sound_channels_{2};
  uint8_t *sound_block_{nullptr};
  size_t sound_block_size_{0};
  size_t sound_block_used_{0};
  uint32_t sound_left_{0};
  uint32_t sound_in_rate_{48000};
  uint8_t sound_in_ch_{1};
  uint8_t sound_carry_[4]{};
  size_t sound_carry_len_{0};
  bool logged_sound_format_{false};
#endif
#ifdef USE_STREAM_RESAMPLER
  resampler::ResamplerSpeaker *resampler_{nullptr};
#endif
#ifdef USE_STREAM_AVI
  avi::AviMediaPlayer *avi_{nullptr};
  std::atomic<bool> avi_wanted_{false};
  std::atomic<bool> avi_refused_{false};
  std::atomic<bool> avi_stop_{false};
  std::atomic<bool> avi_streaming_{false};
#endif
#ifdef USE_STREAM_TOUCH
  touchscreen::Touchscreen *touchscreen_{nullptr};
  StreamTouchListener touch_listener_{this};
  QueueHandle_t touch_q_{nullptr};
  StaticQueue_t touch_store_{};
  Touch touch_slots_[8]{};
  Touch last_touch_{};
  bool last_touch_valid_{false};
#endif
};

}

#endif
