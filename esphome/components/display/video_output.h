#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "display.h"
#include "esphome/core/component.h"
#include "scanout_jpeg.h"
#ifdef USE_LVGL
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include "driver/jpeg_decode.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace esphome::display {

class VideoCanvas {
 public:
  void set_display(Display *display) { this->display_ = display; }
  void set_min_fps(uint8_t fps) { this->min_fps_ = fps; }
  void set_combined(bool combined) { this->combined_ = combined; }
#ifdef USE_LVGL
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
  void set_canvas(lv_obj_t *canvas) { this->canvas_ = canvas; }
  bool show_frame(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t fps, uint8_t bpp,
                  Component *owner);
  void drop_frames();
  bool open_page(uint32_t width, uint32_t height, Component *owner);
  bool page_rect(const uint8_t *jpeg, size_t len, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  static uint8_t page_bpp();
  bool page_fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t *pixel);
  bool page_pixels(const uint8_t *pixels, size_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  bool page_copy(uint32_t from_x, uint32_t from_y, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  bool page_read(uint8_t *pixels, size_t stride, uint32_t x, uint32_t y, uint32_t width, uint32_t height) const;
  void page_show();
  bool page_size(uint32_t *width, uint32_t *height, bool combined) const;
  bool canvas_point(int32_t *x, int32_t *y) const;
  bool open_rows(Component *owner);
  bool rows_begin(lvgl::LvglComponent::VideoCanvasTarget *target, uint32_t wait_ms);
  void rows_end(bool ok);
  DisplayRotation rotation() const;
#endif
  Display *display() const { return this->display_; }
  bool fits(uint32_t width, uint32_t height, uint32_t fps, bool rows, bool combined) const;
  bool within_half() const;
  bool open(uint32_t width, uint32_t height, uint32_t fps, bool worker, Component *owner);
  bool frame(const uint8_t *jpeg, size_t len, uint32_t wait_ms);
  bool submit(const uint8_t *jpeg, size_t len, std::function<void()> &&done);
  void stop_feed();
  void close();
  void loop();
  size_t report(uint8_t *out, size_t room) const;
  bool is_open() const { return this->open_; }
  bool claim() {
    bool expected = false;
    return this->claimed_.compare_exchange_strong(expected, true);
  }
  bool held_by(const Component *owner) const { return this->owner_ == owner && this->claimed_.load(); }
  bool busy() const { return this->job_busy_; }
  bool idle() const { return !this->claimed_.load(); }
  uint32_t width() const { return this->width_; }
  uint32_t height() const { return this->height_; }

 protected:
  enum Mode : uint8_t { MODE_SCREEN, MODE_ROWS, MODE_PICTURE, MODE_FRAMES, MODE_PAGE };

  bool screen_fits_(uint32_t width, uint32_t height) const;
  bool buffer_frame_(const uint8_t *jpeg, size_t len, uint32_t wait_ms);
  static void worker_entry_(void *arg);
  void wake_();
  void release_();

  Display *display_{nullptr};
  Component *owner_{nullptr};
  jpeg_decoder_handle_t decoder_{nullptr};
  ScanoutJpeg scanout_;
  std::atomic<uint8_t> mode_{MODE_SCREEN};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t fps_{0};
  uint8_t min_fps_{25};
  bool combined_{false};
  bool open_{false};
  bool closing_{false};
  std::atomic<bool> claimed_{false};
  std::atomic<bool> sized_{false};
  TaskHandle_t worker_{nullptr};
  const uint8_t *job_data_{nullptr};
  size_t job_len_{0};
  std::function<void()> job_done_{};
  bool job_busy_{false};
  std::atomic<bool> job_pending_{false};
  std::atomic<bool> job_finished_{false};
#ifdef USE_LVGL
  void screen_size_(uint32_t *width, uint32_t *height, bool combined) const;
  bool rows_fit_(uint32_t width, uint32_t height, bool combined) const;
  bool picture_fit_(uint32_t width, uint32_t height, uint32_t fps, bool combined) const;
  bool turned_() const;
  uint64_t turned_rate_() const;
  bool allocate_buffers_();
  void attach_(uint8_t *pixels);
  bool lvgl_runs_() const { return this->lvgl_ != nullptr && !this->lvgl_->is_paused(); }

  lvgl::LvglComponent *lvgl_{nullptr};
  lv_obj_t *canvas_{nullptr};
  uint32_t canvas_w_{0};
  uint32_t canvas_h_{0};
  std::atomic<bool> resize_pending_{false};
  uint8_t *buffers_[2]{};
  size_t buffer_size_{0};
  uint32_t stride_{0};
  uint8_t next_{0};
  std::atomic<int8_t> ready_{-1};
  lv_obj_t *attached_{nullptr};
  lv_draw_buf_t draw_buf_{};
  TaskHandle_t waiter_{nullptr};
  std::atomic<bool> waiting_{false};
  bool rows_started_{false};
  uint32_t refused_{0};
  bool page_wanted_{false};
  bool rows_wanted_{false};
  std::atomic<bool> page_dirty_{false};
#endif
};

class VideoOutput {
 public:
  static constexpr uint8_t MAX_CANVASES = 2;

  void add_canvas(VideoCanvas *canvas) {
    if (canvas != nullptr && this->count_ != MAX_CANVASES)
      this->canvases_[this->count_++] = canvas;
  }
  VideoCanvas *open(uint32_t width, uint32_t height, uint32_t fps, bool worker, Component *owner);
#ifdef USE_LVGL
  VideoCanvas *show_frame(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t fps, uint8_t bpp,
                          Component *owner);
  VideoCanvas *open_page(Component *owner);
  VideoCanvas *open_rows(Component *owner);
  bool page_size(uint32_t *width, uint32_t *height, const Component *owner) const;
#endif
  VideoCanvas *held(const Component *owner) const;
  bool fits(uint32_t width, uint32_t height, uint32_t fps, bool rows, const Component *owner) const;
  bool fits_screen(uint32_t width, uint32_t height, uint32_t fps, bool rows) const;
  void loop();
  size_t report(uint8_t *out, size_t room) const;

 protected:
  VideoCanvas *acquire_(bool *combined);
  bool others_claimed_(const VideoCanvas *self) const;

  VideoCanvas *canvases_[MAX_CANVASES]{};
  uint8_t count_{0};
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
};

}

#endif
