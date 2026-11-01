#include "video_output.h"

#ifdef USE_ESP32_VARIANT_ESP32P4

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "hal/efuse_hal.h"

#include <cstring>

namespace esphome::display {

static const char *const TAG = "video_output";

static constexpr size_t ALIGN = 128;
static constexpr uint32_t WORKER_WAIT_MS = 100;
static constexpr uint64_t TURNED_PIXEL_RATE_BEFORE_V3 = 8000000;
static constexpr uint64_t TURNED_PIXEL_RATE = 12000000;

#ifdef USE_LVGL
static constexpr uint8_t BUFFER_BPP = LV_COLOR_DEPTH == 16 ? 2 : 3;
static constexpr lv_color_format_t BUFFER_FORMAT = LV_COLOR_DEPTH == 16 ? LV_COLOR_FORMAT_RGB565
                                                                         : LV_COLOR_FORMAT_RGB888;

bool VideoCanvas::turned_() const {
  const DisplayRotation rotation = this->lvgl_->get_rotation();
  return rotation == DISPLAY_ROTATION_90_DEGREES || rotation == DISPLAY_ROTATION_270_DEGREES;
}

uint64_t VideoCanvas::turned_rate_() const {
  return efuse_hal_chip_revision() >= 300 ? TURNED_PIXEL_RATE : TURNED_PIXEL_RATE_BEFORE_V3;
}

void VideoCanvas::screen_size_(uint32_t *width, uint32_t *height, bool combined) const {
  const uint32_t native_w = static_cast<uint32_t>(this->display_->get_native_width());
  const uint32_t native_h = static_cast<uint32_t>(this->display_->get_native_height());
  const bool turned = this->turned_();
  *width = turned ? native_h : native_w;
  *height = turned ? native_w : native_h;
  if (!combined)
    return;
  if (*width >= *height) {
    *width /= 2;
  } else {
    *height /= 2;
  }
}

bool VideoCanvas::rows_fit_(uint32_t width, uint32_t height, bool combined) const {
  return !combined && this->display_ != nullptr && this->lvgl_ != nullptr && this->lvgl_->has_scanout() &&
         width == static_cast<uint32_t>(this->display_->get_native_width()) &&
         ((height + 15) & ~15u) <= static_cast<uint32_t>(this->display_->get_native_height());
}

bool VideoCanvas::picture_fit_(uint32_t width, uint32_t height, uint32_t fps, bool combined) const {
  if (this->display_ == nullptr || this->lvgl_ == nullptr)
    return false;
  uint32_t screen_w = 0;
  uint32_t screen_h = 0;
  this->screen_size_(&screen_w, &screen_h, combined);
  if (width > screen_w || height > screen_h)
    return false;
  if (this->lvgl_->get_rotation() == DISPLAY_ROTATION_0_DEGREES)
    return true;
  return static_cast<uint64_t>(width) * height * fps <= this->turned_rate_();
}

bool VideoCanvas::allocate_buffers_() {
  this->stride_ = ((this->width_ + 15) & ~15u) * BUFFER_BPP;
  this->buffer_size_ = static_cast<size_t>(this->stride_) * ((this->height_ + 15) & ~15u);
  const size_t size = (this->buffer_size_ + ALIGN - 1) & ~(ALIGN - 1);
  for (auto &buffer : this->buffers_)
    buffer = static_cast<uint8_t *>(heap_caps_aligned_calloc(ALIGN, 1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->buffers_[0] != nullptr && this->buffers_[1] != nullptr) {
    this->next_ = 0;
    return true;
  }
  ESP_LOGE(TAG, "No memory for two %ux%u pictures", static_cast<unsigned>(this->width_),
           static_cast<unsigned>(this->height_));
  return false;
}

bool VideoCanvas::show_frame(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t fps, uint8_t bpp,
                             Component *owner) {
  if (pixels == nullptr || this->closing_ || this->canvas_ == nullptr || bpp != BUFFER_BPP || !this->lvgl_runs_()) {
    if (!this->open_ && !this->closing_)
      this->release_();
    return false;
  }
  if (this->open_ && (this->owner_ != owner || this->mode_.load() != MODE_FRAMES))
    return false;
  if (!this->open_ || width != this->width_ || height != this->height_) {
    if (!this->picture_fit_(width, height, fps, this->combined_)) {
      const uint32_t key = (width << 16) | height;
      if (this->refused_ != key) {
        this->refused_ = key;
        ESP_LOGW(TAG, "Frames of %ux%u at %u fps: larger than %s takes", static_cast<unsigned>(width),
                 static_cast<unsigned>(height), static_cast<unsigned>(fps),
                 this->combined_ ? "half the screen (another canvas shows)" : "the screen");
      }
      if (!this->open_)
        this->release_();
      return false;
    }
    this->width_ = width;
    this->height_ = height;
    this->fps_ = fps;
    this->stride_ = width * bpp;
    this->buffer_size_ = static_cast<size_t>(this->stride_) * height;
    this->mode_.store(MODE_FRAMES);
    this->owner_ = owner;
    this->open_ = true;
    this->canvas_w_ = width;
    this->canvas_h_ = height;
    this->sized_.store(true);
    lv_obj_set_size(this->canvas_, static_cast<int32_t>(width), static_cast<int32_t>(height));
  }
  this->attach_(const_cast<uint8_t *>(pixels));
  return true;
}

bool VideoCanvas::page_size(uint32_t *width, uint32_t *height, bool combined) const {
  if (this->display_ == nullptr || this->lvgl_ == nullptr)
    return false;
  this->screen_size_(width, height, combined);
  return true;
}

bool VideoCanvas::open_page(uint32_t width, uint32_t height, Component *owner) {
  this->page_wanted_ = true;
  const bool ok = this->open(width, height, 0, false, owner);
  this->page_wanted_ = false;
  return ok;
}

bool VideoCanvas::page_rect(const uint8_t *jpeg, size_t len, uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height) {
  if (this->decoder_ == nullptr || jpeg == nullptr || this->mode_.load() != MODE_PAGE || width == 0 ||
      height == 0 || x + width > this->width_ || y + height > this->height_)
    return false;
  if (!decode_jpeg_into(this->decoder_, jpeg, len, this->buffers_[1], this->buffer_size_, BUFFER_BPP))
    return false;
  const size_t rect_stride = static_cast<size_t>((width + 15) & ~15u) * BUFFER_BPP;
  uint8_t *page = this->buffers_[0];
  for (uint32_t row = 0; row != height; row++)
    std::memcpy(page + (static_cast<size_t>(y) + row) * this->stride_ + static_cast<size_t>(x) * BUFFER_BPP,
                this->buffers_[1] + static_cast<size_t>(row) * rect_stride, static_cast<size_t>(width) * BUFFER_BPP);
  uint8_t *first = page + static_cast<size_t>(y) * this->stride_ + static_cast<size_t>(x) * BUFFER_BPP;
  uint8_t *last =
      page + (static_cast<size_t>(y) + height - 1) * this->stride_ + (static_cast<size_t>(x) + width) * BUFFER_BPP;
  esp_cache_msync(first, last - first, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  this->page_dirty_.store(true);
  this->wake_();
  return true;
}

bool VideoCanvas::canvas_point(int32_t *x, int32_t *y) const {
  if (this->canvas_ == nullptr || this->lvgl_ == nullptr || !this->open_)
    return false;
  lv_area_t at{};
  lv_obj_get_coords(this->canvas_, &at);
  this->lvgl_->rotate_coordinates(*x, *y);
  *x -= at.x1;
  *y -= at.y1;
  return *x >= 0 && *y >= 0 && *x < static_cast<int32_t>(this->width_) && *y < static_cast<int32_t>(this->height_);
}

void VideoCanvas::drop_frames() {
  if (this->attached_ == nullptr || !this->lvgl_runs_())
    return;
  lv_image_set_src(this->attached_, nullptr);
  this->attached_ = nullptr;
  std::memset(&this->draw_buf_, 0, sizeof(this->draw_buf_));
}

void VideoCanvas::attach_(uint8_t *pixels) {
  if (this->attached_ == this->canvas_ && this->draw_buf_.header.w == this->width_ &&
      this->draw_buf_.header.h == this->height_ && this->draw_buf_.header.stride == this->stride_) {
    this->draw_buf_.data = pixels;
    this->draw_buf_.unaligned_data = pixels;
  } else {
    this->attached_ = nullptr;
    if (lv_draw_buf_init(&this->draw_buf_, this->width_, this->height_, BUFFER_FORMAT, this->stride_, pixels,
                         static_cast<uint32_t>(this->buffer_size_)) == LV_RESULT_OK) {
      lv_canvas_set_draw_buf(this->canvas_, &this->draw_buf_);
      this->attached_ = this->canvas_;
    }
  }
  lv_obj_invalidate(this->canvas_);
}
#endif

bool VideoCanvas::screen_fits_(uint32_t width, uint32_t height) const {
  void *fbs[2]{};
  ColorBitness bitness{};
  return this->display_ != nullptr && this->display_->get_scanout_buffers(fbs, 2, &bitness) == 2 &&
         width == static_cast<uint32_t>(this->display_->get_native_width()) &&
         ((height + 15) & ~15u) <= static_cast<uint32_t>(this->display_->get_native_height());
}

bool VideoCanvas::fits(uint32_t width, uint32_t height, uint32_t fps, bool rows, bool combined) const {
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr)
    return (rows && this->rows_fit_(width, height, combined)) || this->picture_fit_(width, height, fps, combined);
#endif
  return rows && !combined && this->screen_fits_(width, height);
}

bool VideoCanvas::within_half() const {
  if (this->idle())
    return true;
  if (!this->sized_.load())
    return false;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr && this->display_ != nullptr) {
    uint32_t half_w = 0;
    uint32_t half_h = 0;
    this->screen_size_(&half_w, &half_h, true);
    return this->mode_.load() != MODE_ROWS && this->canvas_w_ <= half_w && this->canvas_h_ <= half_h;
  }
#endif
  return false;
}

size_t VideoCanvas::report(uint8_t *out, size_t room) const {
  if (out == nullptr || this->display_ == nullptr || room < 16)
    return 0;
  uint8_t turns = 0;
  uint8_t bits = 0;
  uint32_t rate = 0;
  void *fbs[2]{};
  ColorBitness bitness{};
  bool rows = this->display_->get_scanout_buffers(fbs, 2, &bitness) == 2;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr) {
    turns = static_cast<uint8_t>(static_cast<int>(this->lvgl_->get_rotation()) / 90);
    bits = BUFFER_BPP * 8;
    rate = static_cast<uint32_t>(this->turned_rate_());
    rows = this->lvgl_->has_scanout();
  }
#endif
  size_t at = 0;
  auto u16 = [&](uint32_t value) {
    out[at++] = static_cast<uint8_t>(value & 0xFF);
    out[at++] = static_cast<uint8_t>((value >> 8) & 0xFF);
  };
  out[at++] = 'G';
  out[at++] = 1;
  u16(static_cast<uint32_t>(this->display_->get_native_width()));
  u16(static_cast<uint32_t>(this->display_->get_native_height()));
  out[at++] = turns;
  out[at++] = bits;
  u16(efuse_hal_chip_revision());
  for (int shift = 0; shift < 32; shift += 8)
    out[at++] = static_cast<uint8_t>(rate >> shift);
  out[at++] = this->min_fps_;
  out[at++] = rows ? 1 : 0;
  return at;
}

bool VideoCanvas::open(uint32_t width, uint32_t height, uint32_t fps, bool worker, Component *owner) {
  if (this->open_ || this->closing_)
    return false;
  if (this->display_ == nullptr) {
    this->release_();
    return false;
  }
  this->width_ = width;
  this->height_ = height;
  this->fps_ = fps;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr) {
    if (this->canvas_ == nullptr) {
      this->release_();
      return false;
    }
    if (this->page_wanted_) {
      if (!this->picture_fit_(width, height, 0, this->combined_)) {
        this->release_();
        return false;
      }
      this->mode_.store(MODE_PAGE);
      if (!this->allocate_buffers_()) {
        this->closing_ = true;
        return false;
      }
      this->canvas_w_ = width;
      this->canvas_h_ = height;
    } else if (this->rows_fit_(width, height, this->combined_)) {
      this->mode_.store(MODE_ROWS);
      const uint32_t rows = ((height + 15) & ~15u) + 1;
      const bool turned = this->turned_();
      this->canvas_w_ = turned ? rows : width;
      this->canvas_h_ = turned ? width : rows;
    } else if (this->picture_fit_(width, height, fps, this->combined_)) {
      this->mode_.store(MODE_PICTURE);
      if (!this->allocate_buffers_()) {
        this->closing_ = true;
        return false;
      }
      this->canvas_w_ = width;
      this->canvas_h_ = height;
    } else {
      this->release_();
      return false;
    }
    this->sized_.store(true);
  } else
#endif
  {
    if (!this->screen_fits_(width, height)) {
      this->release_();
      return false;
    }
    this->mode_.store(MODE_SCREEN);
  }
  jpeg_decode_engine_cfg_t eng{};
  eng.timeout_ms = 200;
  if (jpeg_new_decoder_engine(&eng, &this->decoder_) != ESP_OK) {
    this->decoder_ = nullptr;
    this->closing_ = true;
    return false;
  }
  if (worker && xTaskCreatePinnedToCoreWithCaps(&VideoCanvas::worker_entry_, "video_out", 4096, this, 1,
                                                &this->worker_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    this->worker_ = nullptr;
    this->closing_ = true;
    return false;
  }
  this->scanout_.reset();
  this->owner_ = owner;
  this->open_ = true;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr)
    this->resize_pending_.store(true);
#endif
  this->wake_();
  return true;
}

bool VideoCanvas::frame(const uint8_t *jpeg, size_t len, uint32_t wait_ms) {
  if (this->decoder_ == nullptr || jpeg == nullptr)
    return false;
#ifdef USE_LVGL
  if (this->lvgl_ != nullptr) {
    const uint8_t mode = this->mode_.load();
    if (mode == MODE_ROWS) {
      lvgl::LvglComponent::VideoCanvasTarget target{};
      if (!this->lvgl_->video_canvas_begin(&target, wait_ms))
        return true;
      const bool ok = this->scanout_.decode(this->decoder_, jpeg, len, target.rows, target.width, target.height,
                                            target.bpp, target.index, target.layout);
      this->lvgl_->video_canvas_end(ok);
      return ok;
    }
    if (mode == MODE_PICTURE)
      return this->buffer_frame_(jpeg, len, wait_ms);
    return true;
  }
#endif
  return this->scanout_.present(this->display_, this->decoder_, jpeg, len, wait_ms);
}

bool VideoCanvas::buffer_frame_(const uint8_t *jpeg, size_t len, uint32_t wait_ms) {
#ifdef USE_LVGL
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = pdMS_TO_TICKS(wait_ms);
  this->waiter_ = xTaskGetCurrentTaskHandle();
  while (this->ready_.load() >= 0) {
    const TickType_t spent = xTaskGetTickCount() - start;
    if (spent >= limit || !this->lvgl_runs_())
      return true;
    this->waiting_.store(true);
    if (this->ready_.load() < 0) {
      this->waiting_.store(false);
      break;
    }
    ulTaskNotifyTake(pdTRUE, limit - spent);
  }
  if (!this->lvgl_runs_())
    return true;
  if (!decode_jpeg_into(this->decoder_, jpeg, len, this->buffers_[this->next_], this->buffer_size_, BUFFER_BPP))
    return false;
  this->ready_.store(static_cast<int8_t>(this->next_));
  this->next_ ^= 1;
  this->wake_();
  return true;
#else
  return false;
#endif
}

void VideoCanvas::release_() {
  this->owner_ = nullptr;
  this->sized_.store(false);
  this->claimed_.store(false);
}

void VideoCanvas::wake_() {
  if (this->owner_ != nullptr) {
    this->owner_->enable_loop_soon_any_context();
  } else {
    App.wake_loop_threadsafe();
  }
}

bool VideoCanvas::submit(const uint8_t *jpeg, size_t len, std::function<void()> &&done) {
  if (this->worker_ == nullptr || this->job_busy_ || this->closing_)
    return false;
  this->job_data_ = jpeg;
  this->job_len_ = len;
  this->job_done_ = std::move(done);
  this->job_busy_ = true;
  this->job_pending_.store(true);
  xTaskNotifyGive(this->worker_);
  return true;
}

void VideoCanvas::worker_entry_(void *arg) {
  auto *output = static_cast<VideoCanvas *>(arg);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!output->job_pending_.exchange(false))
      continue;
    output->frame(output->job_data_, output->job_len_, WORKER_WAIT_MS);
    output->job_finished_.store(true);
    output->wake_();
  }
}

void VideoCanvas::stop_feed() {
#ifdef USE_LVGL
  if (this->mode_.load() == MODE_ROWS && this->lvgl_ != nullptr)
    this->lvgl_->video_canvas_close();
#endif
}

void VideoCanvas::close() {
  if (!this->open_ && !this->closing_)
    return;
  this->open_ = false;
  this->closing_ = true;
  this->loop();
}

void VideoCanvas::loop() {
  if (this->job_finished_.exchange(false)) {
    this->job_busy_ = false;
    auto done = std::move(this->job_done_);
    this->job_done_ = nullptr;
    if (done)
      done();
  }
#ifdef USE_LVGL
  if (!this->closing_ && this->lvgl_runs_() && this->resize_pending_.exchange(false)) {
    lv_obj_set_size(this->canvas_, static_cast<int32_t>(this->canvas_w_), static_cast<int32_t>(this->canvas_h_));
    if (this->mode_.load() == MODE_PAGE)
      this->attach_(this->buffers_[0]);
    if (this->mode_.load() == MODE_ROWS) {
      this->drop_frames();
      if (!this->rows_started_)
        this->rows_started_ = this->lvgl_->video_canvas_start(this->canvas_);
      if (!this->rows_started_)
        ESP_LOGE(TAG, "The canvas cannot show frames straight from the decoder (no scanout)");
    }
  }
  if (this->mode_.load() == MODE_PAGE && !this->closing_ && this->attached_ != nullptr && this->lvgl_runs_() &&
      this->page_dirty_.exchange(false))
    lv_obj_invalidate(this->canvas_);
  const int8_t ready = this->ready_.load();
  if (ready >= 0 && !this->closing_ && this->lvgl_runs_()) {
    if (this->mode_.load() == MODE_PICTURE)
      this->attach_(this->buffers_[ready]);
    this->ready_.store(-1);
    if (this->waiting_.exchange(false))
      xTaskNotifyGive(this->waiter_);
  }
#endif
  if (!this->closing_ || this->job_busy_)
    return;
  if (this->worker_ != nullptr) {
    vTaskDeleteWithCaps(this->worker_);
    this->worker_ = nullptr;
  }
  if (this->decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->decoder_);
    this->decoder_ = nullptr;
  }
#ifdef USE_LVGL
  this->resize_pending_.store(false);
  if (this->rows_started_) {
    this->lvgl_->video_canvas_stop();
    this->rows_started_ = false;
  }
  if (this->attached_ != nullptr) {
    this->drop_frames();
    if (this->attached_ != nullptr)
      return;
  }
  for (auto &buffer : this->buffers_) {
    heap_caps_free(buffer);
    buffer = nullptr;
  }
  this->buffer_size_ = 0;
  this->ready_.store(-1);
  if (this->waiting_.exchange(false))
    xTaskNotifyGive(this->waiter_);
#endif
  this->closing_ = false;
  this->release_();
}

VideoCanvas *VideoOutput::held(const Component *owner) const {
  for (uint8_t i = 0; i != this->count_; i++) {
    if (this->canvases_[i]->held_by(owner))
      return this->canvases_[i];
  }
  return nullptr;
}

VideoCanvas *VideoOutput::acquire_(bool *combined) {
  VideoCanvas *free = nullptr;
  bool other = false;
  bool allowed = true;
  portENTER_CRITICAL(&this->lock_);
  for (uint8_t i = 0; i != this->count_; i++) {
    VideoCanvas *canvas = this->canvases_[i];
    if (canvas->idle()) {
      if (free == nullptr)
        free = canvas;
    } else {
      other = true;
      allowed = allowed && canvas->within_half();
    }
  }
  if (free != nullptr && (!allowed || !free->claim()))
    free = nullptr;
  portEXIT_CRITICAL(&this->lock_);
  *combined = other;
  return free;
}

bool VideoOutput::others_claimed_(const VideoCanvas *self) const {
  for (uint8_t i = 0; i != this->count_; i++) {
    if (this->canvases_[i] != self && !this->canvases_[i]->idle())
      return true;
  }
  return false;
}

VideoCanvas *VideoOutput::open(uint32_t width, uint32_t height, uint32_t fps, bool worker, Component *owner) {
  if (this->held(owner) != nullptr)
    return nullptr;
  bool combined = false;
  VideoCanvas *canvas = this->acquire_(&combined);
  if (canvas == nullptr)
    return nullptr;
  canvas->set_combined(combined);
  return canvas->open(width, height, fps, worker, owner) ? canvas : nullptr;
}

#ifdef USE_LVGL
VideoCanvas *VideoOutput::show_frame(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t fps,
                                     uint8_t bpp, Component *owner) {
  VideoCanvas *canvas = this->held(owner);
  if (canvas == nullptr) {
    bool combined = false;
    canvas = this->acquire_(&combined);
    if (canvas == nullptr)
      return nullptr;
    canvas->set_combined(combined);
  } else {
    canvas->set_combined(this->others_claimed_(canvas));
  }
  return canvas->show_frame(pixels, width, height, fps, bpp, owner) ? canvas : nullptr;
}
#endif

bool VideoOutput::fits(uint32_t width, uint32_t height, uint32_t fps, bool rows, const Component *owner) const {
  if (this->count_ == 0)
    return false;
  bool combined = false;
  for (uint8_t i = 0; i != this->count_; i++) {
    const VideoCanvas *canvas = this->canvases_[i];
    if (canvas->idle() || canvas->held_by(owner))
      continue;
    if (!canvas->within_half())
      return false;
    combined = true;
  }
  return this->canvases_[0]->fits(width, height, fps, rows, combined);
}

VideoCanvas *VideoOutput::open_page(Component *owner) {
  if (this->held(owner) != nullptr || this->count_ == 0)
    return nullptr;
  bool combined = false;
  VideoCanvas *canvas = this->acquire_(&combined);
  if (canvas == nullptr)
    return nullptr;
  canvas->set_combined(combined);
  uint32_t width = 0;
  uint32_t height = 0;
  if (!canvas->page_size(&width, &height, combined)) {
    canvas->open_page(0, 0, owner);
    return nullptr;
  }
  return canvas->open_page(width, height, owner) ? canvas : nullptr;
}

bool VideoOutput::page_size(uint32_t *width, uint32_t *height, const Component *owner) const {
  if (this->count_ == 0)
    return false;
  bool combined = false;
  for (uint8_t i = 0; i != this->count_; i++) {
    const VideoCanvas *canvas = this->canvases_[i];
    if (!canvas->idle() && !canvas->held_by(owner))
      combined = true;
  }
  return this->canvases_[0]->page_size(width, height, combined);
}

bool VideoOutput::fits_screen(uint32_t width, uint32_t height, uint32_t fps, bool rows) const {
  return this->count_ != 0 && this->canvases_[0]->fits(width, height, fps, rows, false);
}

void VideoOutput::loop() {
  for (uint8_t i = 0; i != this->count_; i++)
    this->canvases_[i]->loop();
}

size_t VideoOutput::report(uint8_t *out, size_t room) const {
  return this->count_ != 0 ? this->canvases_[0]->report(out, room) : 0;
}

}

#endif
