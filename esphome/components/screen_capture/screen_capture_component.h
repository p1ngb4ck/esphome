#pragma once

#include "screen_capture.h"

#ifdef USE_SCREEN_CAPTURE_COMPONENT

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#ifdef USE_SCREEN_CAPTURE_RELAY
#include "esphome/components/relay_link/relay_link.h"
#endif

namespace esphome::screen_capture {

class StorageRingBuffer : public ring_buffer::RingBuffer {
 public:
  ~StorageRingBuffer() {
    if (this->created_)
      vRingbufferDelete(this->handle_());
  }
  static std::shared_ptr<ring_buffer::RingBuffer> create(uint8_t *storage, size_t len);

 protected:
  bool created_{false};
};

class ScreenCaptureComponent : public Component, public CaptureHost {
 public:
  static ScreenCaptureComponent *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_lvgl(lvgl::LvglComponent *lvgl) { this->capture_.set_lvgl(lvgl); }
#ifdef USE_SCREEN_CAPTURE_RELAY
  void set_relay(relay_link::RelayLink *relay) { this->relay_ = relay; }
#endif
  ScreenCapture &capture() { return this->capture_; }

  void set_screenshot_directory(const std::string &dir) { this->capture_.set_screenshot_directory(dir); }
  void set_screencast_directory(const std::string &dir) { this->capture_.set_screencast_directory(dir); }
  void set_capture_quality(uint8_t quality) { this->capture_.set_capture_quality(quality); }
  void set_screencast_fps(uint8_t fps) { this->capture_.set_screencast_fps(fps); }
  void set_screencast_target(uint8_t target) { this->capture_.set_screencast_target(target); }
  void set_screencast_sound(bool on) { this->capture_.set_screencast_sound(on); }
#ifdef USE_SCREEN_CAPTURE_MIC
  void set_capture_microphone(microphone::MicrophoneSource *mic) { this->capture_.set_capture_microphone(mic); }
#endif
  Trigger<std::string> *get_screenshot_trigger() { return this->capture_.get_screenshot_trigger(); }
  Trigger<> *get_screencast_started_trigger() { return this->capture_.get_screencast_started_trigger(); }
  Trigger<std::string> *get_screencast_stopped_trigger() { return this->capture_.get_screencast_stopped_trigger(); }

  uint8_t *capture_alloc(size_t size) override;
  void capture_free(uint8_t *piece) override;
  uint8_t capture_blocked() override { return 0; }
  void capture_defer(std::function<void()> &&f) override { this->defer(std::move(f)); }
  std::shared_ptr<ring_buffer::RingBuffer> capture_ring(uint8_t *storage, size_t len) override {
    return StorageRingBuffer::create(storage, len);
  }
  const char *capture_relay_url() override;
  bool capture_relay_ask(bool start) override;
  uint16_t capture_stream_port() override;
  bool capture_http(const char *path, const char *payload, char **body, size_t *len) override;
  void capture_wake() override { this->enable_loop_soon_any_context(); }

 protected:
#ifdef USE_SCREEN_CAPTURE_RELAY
  static void cast_job_(void *ctx, uint16_t index);
  relay_link::RelayLink *relay_{nullptr};
#endif
  ScreenCapture capture_{this};
};

template<typename... Ts> class ScreenshotAction : public Action<Ts...>, public Parented<ScreenCaptureComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->capture().screenshot(); }
};

template<typename... Ts> class ScreencastAction : public Action<Ts...>, public Parented<ScreenCaptureComponent> {
 public:
  void set_command(uint8_t command) { this->command_ = command; }
  void play(const Ts &...x) override {
    if (this->command_ == 0) {
      this->parent_->capture().screencast_start();
    } else if (this->command_ == 1) {
      this->parent_->capture().screencast_stop();
    } else {
      this->parent_->capture().screencast_toggle();
    }
  }

 protected:
  uint8_t command_{2};
};

}

#endif
