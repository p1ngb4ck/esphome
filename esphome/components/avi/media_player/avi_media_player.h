#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/component.h"
#include "esphome/components/avi/avi_parser.h"
#include "esphome/components/avi/buffered_file_reader.h"
#include "esphome/components/display/display.h"
#include "esphome/components/display/video_output.h"
#include "esphome/components/media_player/media_player.h"
#ifdef USE_SPEAKER
#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#endif

#include "esp_timer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace esphome::avi {

class AviMediaPlayer : public Component, public media_player::MediaPlayer {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_display(display::Display *display) { this->display_ = display; }
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#endif
  void set_buffer_size(size_t size) { this->ring_size_ = size; }
  void set_frame_size(size_t size) { this->frame_size_ = size; }
  void set_audio_latency(uint32_t ms) { this->audio_latency_us_ = ms * 1000; }
  void set_audio_latency_lambda(std::function<uint32_t()> &&latency) { this->latency_fn_ = std::move(latency); }
  enum Error : uint8_t { ERROR_NONE, ERROR_NOT_FOUND, ERROR_FORMAT, ERROR_MEMORY, ERROR_DECODE };
  Error last_error() const { return this->error_.load(); }
  const std::string &current_file() const { return this->path_; }
  void set_video_output(display::VideoOutput *output) { this->output_ = output; }
  bool play_stream(void (*room)(void *), void *arg);
  BufferedFileReader *stream_sink() const { return this->sink_.load(); }
  bool is_idle() const { return this->run_.load() == RUN_IDLE; }
  bool played_to_end() const { return this->played_to_end_.load(); }

  media_player::MediaPlayerTraits get_traits() override;

 protected:
  enum Run : uint8_t { RUN_IDLE, RUN_STARTING, RUN_PLAYING, RUN_PAUSED, RUN_STOPPING };

  void control(const media_player::MediaPlayerCall &call) override;
  bool start_(const std::string &path);
  void stop_();
  void end_();
  void play_index_(size_t index);
  static void task_entry_(void *arg);
  void task_();
  bool open_();
  void play_();
  bool frame_(const uint8_t *data, size_t len);
  void sound_(const uint8_t *data, size_t len);
  void wait_until_(int64_t when_us);
#ifdef USE_SPEAKER
  bool sound_start_(const AVIStreamInfo *info);
  void sound_stop_();
  void sound_chunk_(const uint8_t *data, size_t len);
  static void sound_entry_(void *arg);
  void sound_loop_();
#endif
  static void pace_cb_(void *arg);
  void release_();

  display::Display *display_{nullptr};
#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
#endif
  size_t ring_size_{2 * 1024 * 1024};
  size_t frame_size_{512 * 1024};

  std::string path_;
  std::vector<std::string> list_;
  size_t index_{0};
  bool advance_{true};
  int pending_{-1};
  std::atomic<uint8_t> run_{RUN_IDLE};
  volatile bool abort_{false};
  TaskHandle_t task_handle_{nullptr};
  esp_timer_handle_t pace_timer_{nullptr};
  uint8_t *ring_{nullptr};
  uint8_t *frame_{nullptr};
  std::unique_ptr<BufferedFileReader> reader_;
  std::unique_ptr<AVIParser> parser_;
  uint32_t frame_us_{40000};
  uint32_t audio_latency_us_{0};
  std::function<uint32_t()> latency_fn_{};
  std::atomic<Error> error_{ERROR_NONE};
  bool pcm_{false};
#ifdef USE_SPEAKER
  std::unique_ptr<audio::AudioDecoder> sound_decoder_;
  std::shared_ptr<ring_buffer::RingBuffer> sound_ring_;
  uint8_t *sound_mem_{nullptr};
  TaskHandle_t sound_task_{nullptr};
  TaskHandle_t sound_waiter_{nullptr};
  std::atomic<bool> sound_end_{false};
  std::atomic<bool> sound_eof_{false};
  std::atomic<bool> sound_done_{false};
#endif
  std::atomic<bool> ended_{false};
  std::atomic<BufferedFileReader *> sink_{nullptr};
  std::atomic<bool> played_to_end_{false};
  bool stream_source_{false};
  void (*room_cb_)(void *){nullptr};
  void *room_arg_{nullptr};
  bool frame_failed_{false};
  bool holds_() const { return this->canvas_ != nullptr && this->canvas_->held_by(this); }
  display::VideoCanvas own_canvas_;
  display::VideoOutput own_output_;
  display::VideoOutput *output_{&this->own_output_};
  display::VideoCanvas *canvas_{nullptr};
};

}

#endif
