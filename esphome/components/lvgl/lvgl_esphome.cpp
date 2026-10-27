#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "lvgl_esphome.h"

#include "core/lv_global.h"
#include "core/lv_obj_class_private.h"
#include "core/lv_obj_private.h"
#include "display/lv_display_private.h"
#include "misc/lv_area_private.h"
#include "misc/lv_text_private.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <numeric>

#ifdef USE_ESP32
#include <esp_cache.h>
#endif

static void *lv_alloc_draw_buf(size_t size, bool internal);
static void *draw_buf_alloc_cb(size_t size, lv_color_format_t color_format) { return lv_alloc_draw_buf(size, false); };

namespace esphome::lvgl {
ESPHOME_LOG_TAG(TAG, "lvgl");

static const size_t MIN_BUFFER_FRAC = 8;     // buffer must be at least 1/8 of the display size
static const size_t MIN_BUFFER_SIZE = 2048;  // Sensible minimum buffer size

static const char *const EVENT_NAMES[] = {
    "NONE",
    "PRESSED",
    "PRESSING",
    "PRESS_LOST",
    "SHORT_CLICKED",
    "LONG_PRESSED",
    "LONG_PRESSED_REPEAT",
    "CLICKED",
    "RELEASED",
    "SCROLL_BEGIN",
    "SCROLL_END",
    "SCROLL",
    "GESTURE",
    "KEY",
    "FOCUSED",
    "DEFOCUSED",
    "LEAVE",
    "HIT_TEST",
    "COVER_CHECK",
    "REFR_EXT_DRAW_SIZE",
    "DRAW_MAIN_BEGIN",
    "DRAW_MAIN",
    "DRAW_MAIN_END",
    "DRAW_POST_BEGIN",
    "DRAW_POST",
    "DRAW_POST_END",
    "DRAW_PART_BEGIN",
    "DRAW_PART_END",
    "VALUE_CHANGED",
    "INSERT",
    "REFRESH",
    "READY",
    "CANCEL",
    "DELETE",
    "CHILD_CHANGED",
    "CHILD_CREATED",
    "CHILD_DELETED",
    "SCREEN_UNLOAD_START",
    "SCREEN_LOAD_START",
    "SCREEN_LOADED",
    "SCREEN_UNLOADED",
    "SIZE_CHANGED",
    "STYLE_CHANGED",
    "LAYOUT_CHANGED",
    "GET_SELF_SIZE",
};

static const unsigned LOG_LEVEL_MAP[] = {
    ESPHOME_LOG_LEVEL_DEBUG, ESPHOME_LOG_LEVEL_INFO,  ESPHOME_LOG_LEVEL_WARN,
    ESPHOME_LOG_LEVEL_ERROR, ESPHOME_LOG_LEVEL_ERROR, ESPHOME_LOG_LEVEL_NONE,

};

std::string lv_event_code_name_for(lv_event_t *event) {
  auto event_code = lv_event_get_code(event);
  if (event_code < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0])) {
    return EVENT_NAMES[event_code];
  }
  // max 4 bytes: "%u" with uint8_t (max 255, 3 digits) + null
  char buf[4];
  snprintf(buf, sizeof(buf), "%u", event_code);
  return buf;
}

void LvglComponent::set_rotation(display::DisplayRotation rotation) {
  if (this->rotation_type_ == RotationType::ROTATION_UNUSED) {
    ESP_LOGW(TAG, "Display rotation cannot be changed unless rotation was enabled during setup.");
    return;
  }
  this->rotation_ = rotation;
  if (this->is_ready()) {
    this->set_resolution_();
    this->update_orientation_();
    lv_obj_update_layout(this->get_screen_active());
    lv_obj_invalidate(this->get_screen_active());
  }
}

void LvglComponent::set_rotation(int angle) {
  // Normalize to [0, 360). The DisplayRotation enum values are the angles in degrees.
  angle %= 360;
  if (angle < 0)
    angle += 360;
  if (angle % 90 != 0) {
    ESP_LOGW(TAG, "Invalid rotation angle %d; must be a multiple of 90 degrees.", angle);
    return;
  }
  this->set_rotation(static_cast<display::DisplayRotation>(angle));
}

void LvglComponent::rotate_coordinates(int32_t &x, int32_t &y) const {
  switch (this->rotation_) {
    default:
      break;

    case display::DISPLAY_ROTATION_180_DEGREES: {
      x = this->width_ - x - 1;
      y = this->height_ - y - 1;
      break;
    }
    case display::DISPLAY_ROTATION_270_DEGREES: {
      auto tmp = x;
      x = this->height_ - y - 1;
      y = tmp;
      break;
    }
    case display::DISPLAY_ROTATION_90_DEGREES: {
      auto tmp = y;
      y = this->width_ - x - 1;
      x = tmp;
      break;
    }
  }
}

static void rounder_cb(lv_event_t *event) {
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  auto *area = static_cast<lv_area_t *>(lv_event_get_param(event));
  // cater for display driver chips with special requirements for bounds of partial
  // draw areas. Extend the draw area to satisfy:
  // * Coordinates must be a multiple of draw_rounding
  auto draw_rounding = comp->draw_rounding;
  // round down the start coordinates
  area->x1 = area->x1 / draw_rounding * draw_rounding;
  area->y1 = area->y1 / draw_rounding * draw_rounding;
  // round up the end coordinates
  area->x2 = (area->x2 + draw_rounding) / draw_rounding * draw_rounding - 1;
  area->y2 = (area->y2 + draw_rounding) / draw_rounding * draw_rounding - 1;
}

void LvglComponent::render_end_cb(lv_event_t *event) {
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  comp->draw_end_();
}

void LvglComponent::render_start_cb(lv_event_t *event) {
  ESP_LOGVV(TAG, "Draw start");
  auto *comp = static_cast<LvglComponent *>(lv_event_get_user_data(event));
  comp->draw_start_();
}

lv_event_code_t lv_update_event;  // NOLINT
void LvglComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LVGL:\n"
                "  Display width/height: %d x %d\n"
                "  Buffer size: %zu%%\n"
                "  Rotation: %d\n"
                "  Draw rounding: %d",
                this->width_, this->height_, 100 / this->buffer_frac_, this->rotation_, (int) this->draw_rounding);
  if (this->rotation_type_ != ROTATION_UNUSED) {
    const char *rot_type = "hardware via display driver";
    if (this->rotation_type_ == RotationType::ROTATION_SOFTWARE) {
#ifdef USE_ESP32_VARIANT_ESP32P4
      rot_type = this->ppa_client_ != nullptr ? "software (PPA accelerated)" : "software";
#else
      rot_type = "software";
#endif
    }
    ESP_LOGCONFIG(TAG, "  Rotation type: %s", rot_type);
  }
#ifdef USE_ESP32
  if (this->scanout_[0] != nullptr)
    ESP_LOGCONFIG(TAG, "  Drawing: into the display's frame buffers, flipped per refresh");
#endif
}

void LvglComponent::set_paused(bool paused, bool show_snow) {
  this->paused_ = paused;
  this->show_snow_ = show_snow;
#ifdef USE_ESP32
  // Whoever drew meanwhile may have flipped the scanout buffers: the one shown now is the front.
  if (!paused && this->scanout_[0] != nullptr) {
    display::ColorBitness bitness{};
    this->scanout_back_ = this->displays_[0]->get_draw_buffer(&bitness) == this->scanout_[0] ? 1 : 0;
  }
#endif
#ifdef USE_ESP32_VARIANT_ESP32P4
  // Paused, others draw into the frame buffers: no picture in them is the camera's any more (the screen is drawn anew)
  if (this->cam_marker_ != nullptr) {
    this->camera_layer_drop();
    this->cam_shown_ = false;
    this->cam_seq_[0] = 0;
    this->cam_seq_[1] = 0;
    this->cam_latest_ = 0;
    this->cam_box_ = lv_area_t{};
    this->cam_pic_ = lv_area_t{0, 0, -1, -1};
  }
#endif
  if (!paused && lv_screen_active() != nullptr) {
    lv_display_trigger_activity(this->disp_);  // resets the inactivity time
    lv_obj_invalidate(lv_screen_active());
  }
  if (paused && this->pause_callback_ != nullptr)
    this->pause_callback_->trigger();
  if (!paused && this->resume_callback_ != nullptr)
    this->resume_callback_->trigger();
}

void LvglComponent::esphome_lvgl_init() {
  lv_init();
  // override draw buf alloc to ensure proper alignment for PPA
  LV_GLOBAL_DEFAULT()->draw_buf_handlers.buf_malloc_cb = draw_buf_alloc_cb;
  LV_GLOBAL_DEFAULT()->draw_buf_handlers.buf_free_cb = lv_free_core;
  LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers.buf_malloc_cb = draw_buf_alloc_cb;
  LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers.buf_free_cb = lv_free_core;
  LV_GLOBAL_DEFAULT()->font_draw_buf_handlers.buf_malloc_cb = draw_buf_alloc_cb;
  LV_GLOBAL_DEFAULT()->font_draw_buf_handlers.buf_free_cb = lv_free_core;
  lv_tick_set_cb([] { return millis(); });
  lv_update_event = static_cast<lv_event_code_t>(lv_event_register_id());
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event, void *user_data) {
  lv_obj_add_event_cb(obj, callback, event, user_data);
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1,
                                 lv_event_code_t event2, void *user_data) {
  add_event_cb(obj, callback, event1, user_data);
  add_event_cb(obj, callback, event2, user_data);
}

void LvglComponent::add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1,
                                 lv_event_code_t event2, lv_event_code_t event3, void *user_data) {
  add_event_cb(obj, callback, event1, user_data);
  add_event_cb(obj, callback, event2, user_data);
  add_event_cb(obj, callback, event3, user_data);
}

void LvglComponent::lv_obj_set_custom_flag(lv_obj_t *obj, lv_obj_flag_t flag, bool value) {
  // LVGL 9.6 keeps these flags in spec_attr->user_flags, in the same bits as its deprecated lv_obj_add_flag()
  uint8_t mask;
  switch (flag) {
    case LV_OBJ_FLAG_LAYOUT_2:
      mask = 1 << 1;
      break;
    case LV_OBJ_FLAG_WIDGET_1:
      mask = 1 << 2;
      break;
    case LV_OBJ_FLAG_WIDGET_2:
      mask = 1 << 3;
      break;
    default:
      return;
  }
  if (value) {
    lv_obj_spec_attr_t *spec_attr = lv_obj_allocate_spec_attr(obj);
    if (spec_attr == nullptr)
      return;
    spec_attr->user_flags |= mask;
  } else if (obj->spec_attr != nullptr) {
    obj->spec_attr->user_flags &= ~mask;
  }
  if (flag == LV_OBJ_FLAG_LAYOUT_2) {
    lv_obj_mark_layout_as_dirty(lv_obj_get_parent(obj));
    lv_obj_mark_layout_as_dirty(obj);
  }
}

void LvglComponent::add_page(LvPageType *page) {
  this->pages_.push_back(page);
  page->set_parent(this);
  lv_display_set_default(this->disp_);
  page->setup(this->pages_.size() - 1);
}

void LvglComponent::show_page(size_t index, lv_screen_load_anim_t anim, uint32_t time) {
  if (index >= this->pages_.size())
    return;
  this->current_page_ = index;
  if (anim == LV_SCREEN_LOAD_ANIM_NONE) {
    lv_screen_load(this->pages_[this->current_page_]->obj);
  } else {
    lv_screen_load_anim(this->pages_[this->current_page_]->obj, anim, time, 0, false);
  }
}

void LvglComponent::show_next_page(lv_screen_load_anim_t anim, uint32_t time) {
  if (this->pages_.empty() || (this->current_page_ == this->pages_.size() - 1 && !this->page_wrap_))
    return;
  size_t start = this->current_page_;
  do {
    this->current_page_ = (this->current_page_ + 1) % this->pages_.size();
    if (this->current_page_ == start)
      return;  // all pages have skip=true (guaranteed not to happen by YAML validation)
  } while (this->pages_[this->current_page_]->skip);  // skip empty pages()
  this->show_page(this->current_page_, anim, time);
}

void LvglComponent::show_prev_page(lv_screen_load_anim_t anim, uint32_t time) {
  if (this->pages_.empty() || (this->current_page_ == 0 && !this->page_wrap_))
    return;
  size_t start = this->current_page_;
  do {
    this->current_page_ = (this->current_page_ + this->pages_.size() - 1) % this->pages_.size();
    if (this->current_page_ == start)
      return;  // all pages have skip=true (guaranteed not to happen by YAML validation)
  } while (this->pages_[this->current_page_]->skip);  // skip empty pages()
  this->show_page(this->current_page_, anim, time);
}

size_t LvglComponent::get_current_page() const { return this->current_page_; }
bool LvPageType::is_showing() const { return this->parent_->get_current_page() == this->index; }

#ifdef USE_ESP32_VARIANT_ESP32P4
bool LvglComponent::ppa_rotate_(const lv_color_data *src, lv_color_data *dst, uint16_t width, uint16_t height,
                                uint32_t height_rounded) {
  ppa_srm_rotation_angle_t angle;
  uint16_t out_w, out_h;

  // Map ESPHome clockwise display rotation to PPA counter-clockwise angles
  switch (this->rotation_) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      angle = PPA_SRM_ROTATION_ANGLE_270;  // 270° CCW = 90° CW
      out_w = height_rounded;
      out_h = width;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      angle = PPA_SRM_ROTATION_ANGLE_180;
      out_w = width;
      out_h = height;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      angle = PPA_SRM_ROTATION_ANGLE_90;  // 90° CCW = 270° CW
      out_w = height_rounded;
      out_h = width;
      break;
    default:
      return false;  // No rotation needed
  }

  // Align buffer size to cache line (LV_DRAW_BUF_ALIGN) as required by PPA DMA
  // the underlying buffer will be large enough as the size is also padded when allocating.
#if LV_COLOR_DEPTH == 32
  size_t out_buf_size = static_cast<size_t>(out_w) * out_h * 3;  // RGB888 -- 3 bytes/px on P4
#else
  size_t out_buf_size = out_w * out_h * sizeof(lv_color_data);
#endif
  out_buf_size = LV_ROUND_UP(out_buf_size, LV_DRAW_BUF_ALIGN);

  ppa_srm_oper_config_t srm_config{};
  srm_config.in.buffer = src;
  srm_config.in.pic_w = width;
  srm_config.in.pic_h = height;
  srm_config.in.block_w = width;
  srm_config.in.block_h = height;
#if LV_COLOR_DEPTH == 16
  srm_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#elif LV_COLOR_DEPTH == 32
  srm_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
#endif
  srm_config.out.buffer = dst;
  srm_config.out.buffer_size = out_buf_size;
  srm_config.out.pic_w = out_w;
  srm_config.out.pic_h = out_h;
#if LV_COLOR_DEPTH == 16
  srm_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#elif LV_COLOR_DEPTH == 32
  srm_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
#endif
  srm_config.rotation_angle = angle;
  srm_config.scale_x = 1.0f;
  srm_config.scale_y = 1.0f;
  srm_config.mode = PPA_TRANS_MODE_BLOCKING;

  esp_err_t ret = ppa_do_scale_rotate_mirror(this->ppa_client_, &srm_config);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "PPA rotation failed: %s", esp_err_to_name(ret));
    ESP_LOGW(TAG, "PPA SRM: in=%ux%u src=%p, out=%ux%u dst=%p size=%zu, angle=%d", width, height, src, out_w, out_h,
             dst, out_buf_size, (int) angle);
    return false;
  }
  return true;
}
#endif  // USE_ESP32_VARIANT_ESP32P4

void LvglComponent::draw_buffer_(const lv_area_t *area, lv_color_data *ptr) {
#ifdef USE_ESP32
  if (this->scanout_[0] != nullptr) {
    this->draw_scanout_(area, reinterpret_cast<const uint8_t *>(ptr), this->scanout_[this->scanout_back_ ^ 1]);
    return;
  }
#endif
  auto width = lv_area_get_width(area);
  auto height = lv_area_get_height(area);
  auto height_rounded = (height + this->draw_rounding - 1) / this->draw_rounding * this->draw_rounding;
  auto x1 = area->x1;
  auto y1 = area->y1;
  if (this->rotation_type_ == ROTATION_SOFTWARE) {
    lv_color_data *dst = reinterpret_cast<lv_color_data *>(this->rotate_buf_);
#ifdef USE_ESP32_VARIANT_ESP32P4
    bool ppa_done = this->ppa_client_ != nullptr && this->ppa_rotate_(ptr, dst, width, height, height_rounded);
    if (!ppa_done)
#endif
    {
#if LV_COLOR_DEPTH == 32
      // RGB888: 3 bytes/pixel. dst/ptr are lv_color_data* (uint32_t*) but each pixel is only
      // 3 bytes, so the rotate copy must step 3 bytes, not sizeof(lv_color_data).
      auto *dst8 = reinterpret_cast<uint8_t *>(this->rotate_buf_);
      auto *src8 = reinterpret_cast<const uint8_t *>(ptr);
      switch (this->rotation_) {
        case display::DISPLAY_ROTATION_90_DEGREES:
          for (lv_coord_t x = height; x-- != 0;)
            for (lv_coord_t y = 0; y != width; y++) {
              uint8_t *d = dst8 + (static_cast<size_t>(y) * height_rounded + x) * 3;
              d[0] = *src8++;
              d[1] = *src8++;
              d[2] = *src8++;
            }
          break;
        case display::DISPLAY_ROTATION_180_DEGREES:
          for (lv_coord_t y = height; y-- != 0;)
            for (lv_coord_t x = width; x-- != 0;) {
              uint8_t *d = dst8 + (static_cast<size_t>(y) * width + x) * 3;
              d[0] = *src8++;
              d[1] = *src8++;
              d[2] = *src8++;
            }
          break;
        case display::DISPLAY_ROTATION_270_DEGREES:
          for (lv_coord_t x = 0; x != height; x++)
            for (lv_coord_t y = width; y-- != 0;) {
              uint8_t *d = dst8 + (static_cast<size_t>(y) * height_rounded + x) * 3;
              d[0] = *src8++;
              d[1] = *src8++;
              d[2] = *src8++;
            }
          break;
        default:
          dst = ptr;
          break;
      }
#else
      switch (this->rotation_) {
        case display::DISPLAY_ROTATION_90_DEGREES:
          for (lv_coord_t x = height; x-- != 0;) {
            for (lv_coord_t y = 0; y != width; y++) {
              dst[y * height_rounded + x] = *ptr++;
            }
          }
          break;

        case display::DISPLAY_ROTATION_180_DEGREES:
          for (lv_coord_t y = height; y-- != 0;) {
            for (lv_coord_t x = width; x-- != 0;) {
              dst[y * width + x] = *ptr++;
            }
          }
          break;

        case display::DISPLAY_ROTATION_270_DEGREES:
          for (lv_coord_t x = 0; x != height; x++) {
            for (lv_coord_t y = width; y-- != 0;) {
              dst[y * height_rounded + x] = *ptr++;
            }
          }
          break;

        default:
          dst = ptr;
          break;
      }
#endif
    }
    // Coordinate adjustments apply regardless of PPA or SW rotation
    switch (this->rotation_) {
      case display::DISPLAY_ROTATION_90_DEGREES:
        y1 = x1;
        x1 = this->width_ - area->y1 - height;
        height = width;
        width = height_rounded;
        break;

      case display::DISPLAY_ROTATION_180_DEGREES:
        x1 = this->width_ - x1 - width;
        y1 = this->height_ - y1 - height;
        break;

      case display::DISPLAY_ROTATION_270_DEGREES:
        x1 = y1;
        y1 = this->height_ - area->x1 - width;
        height = width;
        width = height_rounded;
        break;

      default:
        break;
    }
    ptr = dst;
  }
  for (auto *display : this->displays_) {
    display->draw_pixels_at(x1, y1, width, height, (const uint8_t *) ptr, LV_BITNESS, this->big_endian_);
  }
}

#ifdef USE_ESP32
static constexpr size_t SCANOUT_BPP = LV_COLOR_DEPTH == 32 ? 3 : LV_COLOR_DEPTH / 8;
struct ScanoutPixel {
  uint8_t b[SCANOUT_BPP];
};
// Longer than any scan frame: a panel that stopped scanning must not hold LVGL.
static constexpr uint32_t SCANOUT_FLIP_TIMEOUT_MS = 100;

bool LvglComponent::setup_scanout_() {
  if (this->displays_.size() != 1 || this->rotation_type_ == ROTATION_HARDWARE)
    return false;
  auto *display = this->displays_[0];
  void *fbs[2]{};
  display::ColorBitness bitness{};
  if (display->get_scanout_buffers(fbs, 2, &bitness) != 2 || bitness != LV_BITNESS)
    return false;
  this->scanout_[0] = static_cast<uint8_t *>(fbs[0]);
  this->scanout_[1] = static_cast<uint8_t *>(fbs[1]);
  this->scanout_stride_ = static_cast<size_t>(this->width_) * SCANOUT_BPP;
#ifdef USE_ESP32_VARIANT_ESP32P4
  // The PPA writes into PSRAM only in whole L2 cache lines.
  static constexpr size_t LINE = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
  const size_t fb_size = this->scanout_stride_ * this->height_;
  this->scanout_ppa_ = fb_size % LINE == 0 && reinterpret_cast<uintptr_t>(fbs[0]) % LINE == 0 &&
                       reinterpret_cast<uintptr_t>(fbs[1]) % LINE == 0;
#endif
  display->present_scanout_buffer(fbs[0]);
  this->scanout_back_ = 1;
  this->scanout_wait_ = true;
  this->scanout_presented_ = millis();
  lv_display_set_sync_cb(this->disp_, scanout_sync_cb);
  return true;
}

void LvglComponent::wait_scanout_flip_() {
  if (!this->scanout_wait_)
    return;
  // Only a refresh started outside loop() gets here before the flip.
  while (!this->displays_[0]->scanout_flipped() && millis() - this->scanout_presented_ < SCANOUT_FLIP_TIMEOUT_MS)
    delay(1);
  this->scanout_wait_ = false;
}

bool LvglComponent::scanout_clip_(const lv_area_t *area, lv_area_t *clipped) const {
  const int32_t hor = lv_display_get_horizontal_resolution(this->disp_);
  const int32_t ver = lv_display_get_vertical_resolution(this->disp_);
  clipped->x1 = std::max<int32_t>(area->x1, 0);
  clipped->y1 = std::max<int32_t>(area->y1, 0);
  clipped->x2 = std::min<int32_t>(area->x2, hor - 1);
  clipped->y2 = std::min<int32_t>(area->y2, ver - 1);
  return clipped->x1 <= clipped->x2 && clipped->y1 <= clipped->y2;
}

// The panel rectangle an area of the LVGL screen lands on.
void LvglComponent::scanout_rect_(const lv_area_t *area, int32_t &x, int32_t &y, int32_t &w, int32_t &h) const {
  const int32_t aw = lv_area_get_width(area);
  const int32_t ah = lv_area_get_height(area);
  const auto rotation =
      this->rotation_type_ == ROTATION_SOFTWARE ? this->rotation_ : display::DISPLAY_ROTATION_0_DEGREES;
  switch (rotation) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      x = this->width_ - area->y1 - ah;
      y = area->x1;
      w = ah;
      h = aw;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      x = this->width_ - area->x1 - aw;
      y = this->height_ - area->y1 - ah;
      w = aw;
      h = ah;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      x = area->y1;
      y = this->height_ - area->x1 - aw;
      w = ah;
      h = aw;
      break;
    default:
      x = area->x1;
      y = area->y1;
      w = aw;
      h = ah;
      break;
  }
}

// Writes a rendered area (packed rows of its full width) into frame buffer fb.
void LvglComponent::draw_scanout_(const lv_area_t *area, const uint8_t *src, uint8_t *fb) {
  lv_area_t clip;
  if (!this->scanout_clip_(area, &clip))
    return;
  const int32_t src_w = lv_area_get_width(area);
  const int32_t sx = clip.x1 - area->x1;
  const int32_t sy = clip.y1 - area->y1;
  const int32_t cw = lv_area_get_width(&clip);
  const int32_t ch = lv_area_get_height(&clip);
  int32_t x, y, w, h;
  this->scanout_rect_(&clip, x, y, w, h);
  const auto rotation =
      this->rotation_type_ == ROTATION_SOFTWARE ? this->rotation_ : display::DISPLAY_ROTATION_0_DEGREES;

#ifdef USE_ESP32_VARIANT_ESP32P4
  if (this->scanout_ppa_ && this->ppa_client_ != nullptr) {
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;
    if (rotation == display::DISPLAY_ROTATION_90_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_270;  // PPA turns counter-clockwise
    } else if (rotation == display::DISPLAY_ROTATION_180_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_180;
    } else if (rotation == display::DISPLAY_ROTATION_270_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_90;
    }
    if (this->ppa_blit_(src, src_w, lv_area_get_height(area), sx, sy, cw, ch, fb, x, y, angle))
      return;
  }
#endif

  const auto *in = reinterpret_cast<const ScanoutPixel *>(src);
  auto *out = reinterpret_cast<ScanoutPixel *>(fb);
  const size_t stride = this->width_;
  for (int32_t r = 0; r != ch; r++) {
    const ScanoutPixel *row = in + static_cast<size_t>(sy + r) * src_w + sx;
    switch (rotation) {
      case display::DISPLAY_ROTATION_90_DEGREES:
        for (int32_t c = 0; c != cw; c++)
          out[static_cast<size_t>(y + c) * stride + x + ch - 1 - r] = row[c];
        break;
      case display::DISPLAY_ROTATION_180_DEGREES:
        for (int32_t c = 0; c != cw; c++)
          out[static_cast<size_t>(y + ch - 1 - r) * stride + x + cw - 1 - c] = row[c];
        break;
      case display::DISPLAY_ROTATION_270_DEGREES:
        for (int32_t c = 0; c != cw; c++)
          out[static_cast<size_t>(y + cw - 1 - c) * stride + x + r] = row[c];
        break;
      default:
        std::memcpy(out + static_cast<size_t>(y + r) * stride + x, row, cw * SCANOUT_BPP);
        break;
    }
  }
  esp_cache_msync(fb + y * this->scanout_stride_, h * this->scanout_stride_,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// Before a refresh: what the shown buffer got last time, and this refresh does not redraw, goes
// into the buffer drawn into now.
void LvglComponent::sync_scanout_(const lv_area_t *area) {
  this->wait_scanout_flip_();
  lv_area_t clip;
  if (!this->scanout_clip_(area, &clip))
    return;
  int32_t x, y, w, h;
  this->scanout_rect_(&clip, x, y, w, h);
  const uint8_t *front = this->scanout_[this->scanout_back_ ^ 1];
  uint8_t *back = this->scanout_[this->scanout_back_];
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (lv_area_is_on(&clip, &this->cam_pic_))  // the shown buffer's picture comes with it
    this->cam_seq_[this->scanout_back_] = this->cam_seq_[this->scanout_back_ ^ 1];
  if (this->scanout_ppa_ && this->ppa_client_ != nullptr &&
      this->ppa_blit_(front, this->width_, this->height_, x, y, w, h, back, x, y, PPA_SRM_ROTATION_ANGLE_0))
    return;
#endif
  const size_t offset = x * SCANOUT_BPP;
  for (int32_t r = 0; r != h; r++) {
    const size_t row = static_cast<size_t>(y + r) * this->scanout_stride_ + offset;
    std::memcpy(back + row, front + row, w * SCANOUT_BPP);
  }
  esp_cache_msync(back + y * this->scanout_stride_, h * this->scanout_stride_,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void LvglComponent::scanout_sync_cb(lv_display_t *disp, const lv_area_t *area) {
  static_cast<LvglComponent *>(lv_display_get_user_data(disp))->sync_scanout_(area);
  lv_display_sync_ready(disp);
}

size_t LvglComponent::capture_bpp() { return SCANOUT_BPP; }

size_t LvglComponent::capture_bytes() const {
#ifdef USE_ESP32_VARIANT_ESP32P4
  static constexpr size_t LINE = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
#else
  static constexpr size_t LINE = 64;
#endif
  const size_t bytes = static_cast<size_t>(lv_display_get_horizontal_resolution(this->disp_)) *
                       lv_display_get_vertical_resolution(this->disp_) * SCANOUT_BPP;
  return (bytes + LINE - 1) / LINE * LINE;
}

// A rendered area (packed rows of its full width) also into the capture's buffer, as LVGL has it.
void LvglComponent::capture_area_(const lv_area_t *area, const uint8_t *src) {
  lv_area_t clip;
  if (!this->scanout_clip_(area, &clip))
    return;
  uint8_t *dst = this->frame_capture_->capture_target();
  if (dst == nullptr)
    return;
  const int32_t hor = lv_display_get_horizontal_resolution(this->disp_);
  const int32_t src_w = lv_area_get_width(area);
  const int32_t sx = clip.x1 - area->x1;
  const int32_t sy = clip.y1 - area->y1;
  const int32_t cw = lv_area_get_width(&clip);
  const int32_t ch = lv_area_get_height(&clip);
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (this->ppa_client_ != nullptr &&
      this->ppa_blit_to_(src, src_w, lv_area_get_height(area), sx, sy, cw, ch, dst, hor,
                         lv_display_get_vertical_resolution(this->disp_), this->capture_bytes(), clip.x1, clip.y1,
                         PPA_SRM_ROTATION_ANGLE_0)) {
    this->frame_capture_->capture_area(clip);
    return;
  }
#endif
  const size_t stride = static_cast<size_t>(hor) * SCANOUT_BPP;
  for (int32_t r = 0; r != ch; r++) {
    std::memcpy(dst + static_cast<size_t>(clip.y1 + r) * stride + clip.x1 * SCANOUT_BPP,
                src + (static_cast<size_t>(sy + r) * src_w + sx) * SCANOUT_BPP, cw * SCANOUT_BPP);
  }
  esp_cache_msync(dst + clip.y1 * stride, ch * stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  this->frame_capture_->capture_area(clip);
}

bool LvglComponent::copy_shown(uint8_t *dst) {
  if (this->scanout_[0] == nullptr || this->paused_ || dst == nullptr)
    return false;
  const uint8_t *front = this->scanout_[this->scanout_back_ ^ 1];
  const int32_t hor = lv_display_get_horizontal_resolution(this->disp_);
  const int32_t ver = lv_display_get_vertical_resolution(this->disp_);
  const auto rotation =
      this->rotation_type_ == ROTATION_SOFTWARE ? this->rotation_ : display::DISPLAY_ROTATION_0_DEGREES;
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (this->scanout_ppa_ && this->ppa_client_ != nullptr) {
    // Back the other way than draw_scanout_() turns it (the PPA turns counter-clockwise).
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;
    if (rotation == display::DISPLAY_ROTATION_90_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_90;
    } else if (rotation == display::DISPLAY_ROTATION_180_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_180;
    } else if (rotation == display::DISPLAY_ROTATION_270_DEGREES) {
      angle = PPA_SRM_ROTATION_ANGLE_270;
    }
    if (this->ppa_blit_to_(front, this->width_, this->height_, 0, 0, this->width_, this->height_, dst, hor, ver,
                           this->capture_bytes(), 0, 0, angle))
      return true;
  }
#endif
  // LVGL pixel (lx, ly) is where draw_scanout_() put it on the panel.
  const auto *in = reinterpret_cast<const ScanoutPixel *>(front);
  auto *out = reinterpret_cast<ScanoutPixel *>(dst);
  const size_t panel = this->width_;
  for (int32_t ly = 0; ly != ver; ly++) {
    ScanoutPixel *row = out + static_cast<size_t>(ly) * hor;
    switch (rotation) {
      case display::DISPLAY_ROTATION_90_DEGREES:
        for (int32_t lx = 0; lx != hor; lx++)
          row[lx] = in[static_cast<size_t>(lx) * panel + this->width_ - 1 - ly];
        break;
      case display::DISPLAY_ROTATION_180_DEGREES:
        for (int32_t lx = 0; lx != hor; lx++)
          row[lx] = in[static_cast<size_t>(this->height_ - 1 - ly) * panel + this->width_ - 1 - lx];
        break;
      case display::DISPLAY_ROTATION_270_DEGREES:
        for (int32_t lx = 0; lx != hor; lx++)
          row[lx] = in[static_cast<size_t>(this->height_ - 1 - lx) * panel + ly];
        break;
      default:
        std::memcpy(row, in + static_cast<size_t>(ly) * panel, hor * SCANOUT_BPP);
        break;
    }
  }
  esp_cache_msync(dst, this->capture_bytes(), ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  return true;
}

#ifdef USE_ESP32_VARIANT_ESP32P4
bool LvglComponent::ppa_blit_(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint16_t sx, uint16_t sy, uint16_t w,
                              uint16_t h, uint8_t *dst, uint16_t dx, uint16_t dy, ppa_srm_rotation_angle_t angle) {
  return this->ppa_blit_to_(src, src_w, src_h, sx, sy, w, h, dst, this->width_, this->height_,
                            this->scanout_stride_ * this->height_, dx, dy, angle);
}

bool LvglComponent::ppa_blit_to_(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint16_t sx, uint16_t sy,
                                 uint16_t w, uint16_t h, uint8_t *dst, uint16_t dst_w, uint16_t dst_h, size_t dst_size,
                                 uint16_t dx, uint16_t dy, ppa_srm_rotation_angle_t angle) {
  ppa_srm_oper_config_t cfg{};
  cfg.in.buffer = src;
  cfg.in.pic_w = src_w;
  cfg.in.pic_h = src_h;
  cfg.in.block_w = w;
  cfg.in.block_h = h;
  cfg.in.block_offset_x = sx;
  cfg.in.block_offset_y = sy;
  cfg.out.buffer = dst;
  cfg.out.buffer_size = dst_size;
  cfg.out.pic_w = dst_w;
  cfg.out.pic_h = dst_h;
  cfg.out.block_offset_x = dx;
  cfg.out.block_offset_y = dy;
#if LV_COLOR_DEPTH == 16
  cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#elif LV_COLOR_DEPTH == 32
  cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
  cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
#endif
  cfg.rotation_angle = angle;
  cfg.scale_x = 1.0f;
  cfg.scale_y = 1.0f;
  cfg.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(this->ppa_client_, &cfg) == ESP_OK;
}

// The area of the LVGL screen a panel rectangle shows (the inverse of scanout_rect_())
void LvglComponent::panel_to_area_(int32_t x, int32_t y, int32_t w, int32_t h, lv_area_t *area) const {
  const auto rotation =
      this->rotation_type_ == ROTATION_SOFTWARE ? this->rotation_ : display::DISPLAY_ROTATION_0_DEGREES;
  int32_t ax, ay, aw, ah;
  switch (rotation) {
    case display::DISPLAY_ROTATION_90_DEGREES:
      ax = y;
      ay = this->width_ - x - w;
      aw = h;
      ah = w;
      break;
    case display::DISPLAY_ROTATION_180_DEGREES:
      ax = this->width_ - x - w;
      ay = this->height_ - y - h;
      aw = w;
      ah = h;
      break;
    case display::DISPLAY_ROTATION_270_DEGREES:
      ax = this->height_ - y - h;
      ay = x;
      aw = h;
      ah = w;
      break;
    default:
      ax = x;
      ay = y;
      aw = w;
      ah = h;
      break;
  }
  area->x1 = ax;
  area->y1 = ay;
  area->x2 = ax + aw - 1;
  area->y2 = ay + ah - 1;
}

/* ---------------- camera layer ---------------- */

bool LvglComponent::camera_layer_start(lv_obj_t *marker) {
  if (marker == nullptr || this->scanout_[0] == nullptr || !this->scanout_ppa_ || this->ppa_client_ == nullptr)
    return false;
  if (this->cam_marker_ != nullptr && this->cam_marker_ != marker)
    this->camera_layer_stop();
  this->cam_marker_ = marker;
  this->cam_shown_ = false;
  this->cam_layer_reset_();
  return true;
}

void LvglComponent::camera_layer_stop() {
  if (this->cam_marker_ == nullptr)
    return;
  this->camera_layer_drop();
  this->cam_layer_reset_();
  this->cam_marker_ = nullptr;
  this->cam_shown_ = false;
}

bool LvglComponent::camera_layer_frame(const void *data, uint16_t pic_w, uint16_t pic_h, uint16_t w, uint16_t h,
                                       ppa_srm_color_mode_t cm, std::function<void()> &&done) {
  if (this->cam_marker_ == nullptr || !this->cam_shown_ || this->paused_ || data == nullptr || w == 0 || h == 0 ||
      w > pic_w || h > pic_h)
    return false;
  this->camera_layer_drop();  // an older one not drawn yet: the newer one instead
  this->cam_frame_ = CamFrame{data, pic_w, pic_h, w, h, cm};
  this->cam_done_ = std::move(done);
  this->cam_pending_ = true;
  return true;
}

void LvglComponent::camera_layer_drop() {
  if (!this->cam_pending_)
    return;
  this->cam_pending_ = false;
  auto done = std::move(this->cam_done_);
  this->cam_done_ = nullptr;
  if (done)
    done();
}

// Nothing of LVGL over the picture: the marker on the screen shown, visible, no top-layer object over it
bool LvglComponent::cam_layer_shown_() const {
  if (this->paused_ || lv_obj_get_screen(this->cam_marker_) != lv_screen_active() ||
      !lv_obj_is_visible(this->cam_marker_))
    return false;
  lv_area_t box;
  lv_obj_get_coords(this->cam_marker_, &box);
  lv_obj_t *top = lv_layer_top();
  const uint32_t count = lv_obj_get_child_count(top);
  for (uint32_t i = 0; i != count; i++) {
    lv_obj_t *child = lv_obj_get_child(top, static_cast<int32_t>(i));
    if (lv_obj_is_hidden(child))
      continue;
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    if (lv_area_is_on(&area, &box))
      return false;
  }
  return true;
}

// The marker drawn by LVGL again (black, its sync carries it into the other buffer); no buffer holds a picture
void LvglComponent::cam_layer_reset_() {
  if (!this->paused_ && this->cam_marker_ != nullptr)
    lv_obj_invalidate(this->cam_marker_);
  this->cam_seq_[0] = 0;
  this->cam_seq_[1] = 0;
  this->cam_latest_ = 0;
  this->cam_box_ = lv_area_t{};
  this->cam_pic_ = lv_area_t{0, 0, -1, -1};
  this->cam_rate_since_ = 0;
}

// Main loop, outside LVGL's refresh
void LvglComponent::cam_layer_service_() {
  const bool shown = this->cam_layer_shown_();
  if (shown != this->cam_shown_) {
    this->cam_shown_ = shown;
    this->cam_layer_reset_();
  }
  if (!this->cam_pending_)
    return;
  if (!shown) {
    this->camera_layer_drop();
    return;
  }
  if (!this->scanout_wait_)
    this->cam_layer_draw_();  // else after the flip, a later pass
}

void LvglComponent::cam_layer_draw_() {
  const CamFrame f = this->cam_frame_;
  lv_area_t box;
  lv_obj_get_coords(this->cam_marker_, &box);
  if (!this->scanout_clip_(&box, &box)) {
    this->camera_layer_drop();
    return;
  }
  if (!lv_area_is_equal(&box, &this->cam_box_)) {
    this->cam_layer_reset_();  // moved or resized (full screen): LVGL draws it black, then the pictures
    this->cam_box_ = box;
  }
  // The frame as it comes, in the panel's own orientation (the camera is mounted to match): 1:1, never scaled or
  // turned (the device does not resize at runtime). Centred in the box's panel rectangle, cut to it when larger.
  int32_t bx, by, bw, bh;
  this->scanout_rect_(&box, bx, by, bw, bh);
  const int32_t step = std::max<int32_t>(1, std::min<int32_t>(16, std::min<int32_t>(bw * 16 / f.w, bh * 16 / f.h)));
  const bool scaled = step < 16;
  const int32_t ow =
      (scaled ? f.w * step / 16 : std::min<int32_t>(f.w, bw)) & ~1;  // even: a YUV422 frame is cut between pixel pairs
  const int32_t oh = scaled ? f.h * step / 16 : std::min<int32_t>(f.h, bh);
  const int32_t sx = scaled ? 0 : ((f.w - ow) / 2) & ~1;
  const int32_t sy = scaled ? 0 : (f.h - oh) / 2;
  const int32_t x = bx + (bw - ow) / 2;
  const int32_t y = by + (bh - oh) / 2;
  lv_area_t pic;
  this->panel_to_area_(x, y, ow, oh, &pic);
  if (!lv_area_is_equal(&pic, &this->cam_pic_) && this->cam_latest_ != 0) {
    // Another picture size: the old one's edges go black (LVGL), both buffers
    lv_obj_invalidate(this->cam_marker_);
    this->cam_seq_[0] = 0;
    this->cam_seq_[1] = 0;
  }
  uint8_t *back = this->scanout_[this->scanout_back_];
  // LVGL's last areas are in the shown buffer only: into this one first, it is presented before LVGL's next refresh
  lv_ll_t *areas = &this->disp_->sync_areas;
  for (auto *area = static_cast<lv_area_t *>(lv_ll_get_head(areas)); area != nullptr;
       area = static_cast<lv_area_t *>(lv_ll_get_next(areas, area)))
    this->sync_scanout_(area);
  lv_ll_clear(areas);

  ppa_srm_oper_config_t cfg{};
  cfg.in.buffer = f.data;
  cfg.in.pic_w = f.pic_w;
  cfg.in.pic_h = f.pic_h;
  cfg.in.block_w = scaled ? f.w & ~1 : ow;
  cfg.in.block_h = scaled ? f.h : oh;
  cfg.in.block_offset_x = sx;
  cfg.in.block_offset_y = sy;
  cfg.in.srm_cm = f.cm;
  cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
  cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  cfg.out.buffer = back;
  cfg.out.buffer_size = this->scanout_stride_ * this->height_;
  cfg.out.pic_w = this->width_;
  cfg.out.pic_h = this->height_;
  cfg.out.block_offset_x = x;
  cfg.out.block_offset_y = y;
#if LV_COLOR_DEPTH == 16
  cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#elif LV_COLOR_DEPTH == 32
  cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
#endif
  // A copy (and a YUV conversion): no scaling, no rotation
  cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  cfg.scale_x = static_cast<float>(step) / 16.0f;
  cfg.scale_y = static_cast<float>(step) / 16.0f;
  cfg.mode = PPA_TRANS_MODE_BLOCKING;
  const bool ok = ppa_do_scale_rotate_mirror(this->ppa_client_, &cfg) == ESP_OK;
  this->camera_layer_drop();  // read: the camera's buffer goes back
  if (!ok)
    return;
  this->cam_pic_ = pic;
  if (++this->cam_latest_ == 0)
    this->cam_latest_ = 1;
  this->cam_seq_[this->scanout_back_] = this->cam_latest_;
  this->displays_[0]->present_scanout_buffer(back);
  this->scanout_back_ ^= 1;
  this->scanout_wait_ = true;
  this->scanout_presented_ = millis();
  // The pictures' rate: one line per 10 s while they come (counted per picture, nothing runs between them)
  this->cam_frames_++;
  const uint32_t elapsed = this->scanout_presented_ - this->cam_rate_since_;
  if (this->cam_rate_since_ == 0) {
    this->cam_rate_since_ = this->scanout_presented_;
    this->cam_frames_ = 0;
  } else if (elapsed >= 10000) {
    ESP_LOGI(TAG, "Camera layer: %.1f pictures/s", this->cam_frames_ * 1000.0f / elapsed);
    this->cam_rate_since_ = this->scanout_presented_;
    this->cam_frames_ = 0;
  }
}

// LVGL presents its buffer (last flush): an older picture in it gets the newest one from the shown buffer
void LvglComponent::cam_layer_restore_() {
  const uint8_t b = this->scanout_back_;
  if (this->cam_latest_ == 0 || this->cam_seq_[b] == this->cam_latest_ || this->cam_seq_[b ^ 1] != this->cam_latest_)
    return;
  int32_t x, y, w, h;
  this->scanout_rect_(&this->cam_pic_, x, y, w, h);
  if (this->ppa_blit_(this->scanout_[b ^ 1], this->width_, this->height_, x, y, w, h, this->scanout_[b], x, y,
                      PPA_SRM_ROTATION_ANGLE_0))
    this->cam_seq_[b] = this->cam_latest_;
}
#endif  // USE_ESP32_VARIANT_ESP32P4
#endif  // USE_ESP32

void LvglComponent::flush_cb_(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p) {
  // no guard here for display busy, since LVGL will not call flush_cb until the refresh timer fires,
  // and while the display is busy this is reset to 5 minutes. If that expires and the display is still
  // busy there are bigger problems.
  if (!this->paused_) {
    auto now = millis();
#ifdef USE_ESP32
    if (this->frame_capture_ != nullptr)
      this->capture_area_(area, color_p);
    if (this->scanout_[0] != nullptr) {
      this->wait_scanout_flip_();
      this->draw_scanout_(area, color_p, this->scanout_[this->scanout_back_]);
#ifdef USE_ESP32_VARIANT_ESP32P4
      // Drawn over the picture: this buffer's is gone; before the present the newest one comes back
      if (this->cam_shown_ && lv_area_is_on(area, &this->cam_pic_))
        this->cam_seq_[this->scanout_back_] = 0;
      if (this->cam_shown_ && lv_display_flush_is_last(disp_drv)) {
        lv_area_t box;
        lv_obj_get_coords(this->cam_marker_, &box);
        if (this->scanout_clip_(&box, &box) && lv_area_is_equal(&box, &this->cam_box_)) {
          this->cam_layer_restore_();
        } else {
          this->cam_seq_[0] = 0;
          this->cam_seq_[1] = 0;
          this->cam_latest_ = 0;
          this->cam_pic_ = lv_area_t{0, 0, -1, -1};
        }
      }
#endif
      if (lv_display_flush_is_last(disp_drv)) {
        this->displays_[0]->present_scanout_buffer(this->scanout_[this->scanout_back_]);
        this->scanout_back_ ^= 1;
        this->scanout_wait_ = true;
        this->scanout_presented_ = millis();
      }
    } else
#endif
      this->draw_buffer_(area, reinterpret_cast<lv_color_data *>(color_p));
#ifdef USE_ESP32
    if (this->frame_capture_ != nullptr && lv_display_flush_is_last(disp_drv))
      this->frame_capture_->capture_done();
#endif
    ESP_LOGV(TAG, "flush_cb, area=%d/%d, %d/%d took %dms", (int) area->x1, (int) area->y1,
             (int) lv_area_get_width(area), (int) lv_area_get_height(area), (int) (millis() - now));
  }
  lv_display_flush_ready(disp_drv);
}

IdleTrigger::IdleTrigger(LvglComponent *parent, TemplatableFn<uint32_t> timeout) : timeout_(timeout) {
  parent->add_on_idle_callback([this](uint32_t idle_time) {
    if (!this->is_idle_ && idle_time > this->timeout_.value()) {
      this->is_idle_ = true;
      this->trigger();
    } else if (this->is_idle_ && idle_time < this->timeout_.value()) {
      this->is_idle_ = false;
    }
  });
}

#ifdef USE_LVGL_TOUCHSCREEN
LVTouchListener::LVTouchListener(uint16_t long_press_time, uint16_t long_press_repeat_time, LvglComponent *parent) {
  this->set_parent(parent);
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_disp(this->drv_, parent->get_disp());
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    auto *l = static_cast<LVTouchListener *>(lv_indev_get_user_data(d));
    if (l->touch_pressed_) {
      data->point.x = l->touch_point_.x;
      data->point.y = l->touch_point_.y;
      l->parent_->rotate_coordinates(data->point.x, data->point.y);
      data->state = LV_INDEV_STATE_PRESSED;
    } else {
      data->state = LV_INDEV_STATE_RELEASED;
    }
  });
}

void LVTouchListener::update(const touchscreen::TouchPoints_t &tpoints) {
  this->touch_pressed_ = !this->parent_->is_paused() && !tpoints.empty();
  if (this->touch_pressed_)
    this->touch_point_ = tpoints[0];
}
#endif  // USE_LVGL_TOUCHSCREEN

#ifdef USE_LVGL_METER

int16_t lv_get_needle_angle_for_value(lv_obj_t *obj, int32_t value) {
  auto *scale = lv_obj_get_parent(obj);
  auto min_value = lv_scale_get_range_min_value(scale);
  auto max_value = lv_scale_get_range_max_value(scale);
  value = clamp(value, min_value, max_value);
  return ((value - min_value) * lv_scale_get_angle_range(scale) / (max_value - min_value) +
          lv_scale_get_rotation((scale))) %
         360;
}

void IndicatorLine::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_line_set_points(lv_obj, this->points_, 2);
  lv_obj_add_event_cb(
      lv_obj_get_parent(obj),
      [](lv_event_t *e) {
        auto *indicator = static_cast<IndicatorLine *>(lv_event_get_user_data(e));
        indicator->update_length_();
        ESP_LOGV(TAG, "Updated length, value = %d", indicator->angle_);
      },
      LV_EVENT_SIZE_CHANGED, this);
}

void IndicatorLine::set_value(int value) {
  auto angle = lv_get_needle_angle_for_value(this->obj, value);
  if (angle != this->angle_) {
    this->angle_ = angle;
    this->update_length_();
  }
}

void IndicatorLine::update_length_() {
  auto cx = lv_obj_get_width(lv_obj_get_parent(this->obj)) / 2;
  auto cy = lv_obj_get_height(lv_obj_get_parent(this->obj)) / 2;
  auto radius = clamp_at_most(cx, cy);
  auto length = lv_obj_get_style_length(this->obj, LV_PART_MAIN);
  auto radial_offset = lv_obj_get_style_radial_offset(this->obj, LV_PART_MAIN);
  if (LV_COORD_IS_PCT(radial_offset)) {
    radial_offset = radius * LV_COORD_GET_PCT(radial_offset) / 100;
  }
  if (LV_COORD_IS_PCT(length)) {
    length = radius * LV_COORD_GET_PCT(length) / 100;
  } else if (length < 0) {
    length += radius;
  }
  auto x = lv_trigo_cos(this->angle_) / 32768.0f;
  auto y = lv_trigo_sin(this->angle_) / 32768.0f;
  // radius here also represents the offset of the scale center from top left
  this->points_[0].x = radius + radial_offset * x;
  this->points_[0].y = radius + radial_offset * y;
  this->points_[1].x = radius + x * (radial_offset + length);
  this->points_[1].y = radius + y * (radial_offset + length);
  lv_obj_refresh_self_size(this->obj);
  lv_obj_invalidate(this->obj);
}
#endif

#ifdef USE_LVGL_TABLE
uint32_t lv_table_get_selected_row(lv_obj_t *obj) {
  uint32_t row;
  uint32_t column;
  lv_table_get_selected_cell(obj, &row, &column);
  return row;
}

uint32_t lv_table_get_selected_column(lv_obj_t *obj) {
  uint32_t row;
  uint32_t column;
  lv_table_get_selected_cell(obj, &row, &column);
  return column;
}

void LvTableType::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_obj_add_event_cb(
      lv_obj,
      [](lv_event_t *e) {
        auto *table = static_cast<LvTableType *>(lv_event_get_user_data(e));
        table->update_column_widths_();
      },
      LV_EVENT_SIZE_CHANGED, this);
}

void LvTableType::add_column_width_pct(uint32_t col, uint8_t pct) {
  for (auto &i : this->column_pct_) {
    if (i.col == col) {
      i.pct = pct;
      this->update_column_widths_();
      return;
    }
  }
  this->column_pct_.push_back({col, pct});
  this->update_column_widths_();
}

void LvTableType::update_column_widths_() {
  auto content_width = lv_obj_get_content_width(this->obj);
  for (const auto &col : this->column_pct_) {
    lv_table_set_column_width(this->obj, col.col, content_width * col.pct / 100);
  }
}
#endif  // USE_LVGL_TABLE

#ifdef USE_LVGL_KEY_LISTENER
LVEncoderListener::LVEncoderListener(lv_indev_type_t type, uint16_t long_press_time, uint16_t long_press_repeat_time) {
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, type);
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    auto *l = static_cast<LVEncoderListener *>(lv_indev_get_user_data(d));
    data->state = l->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->key = l->key_;
    data->enc_diff = (int16_t) (l->count_ - l->last_count_);
    l->last_count_ = l->count_;
    data->continue_reading = false;
  });
}
#endif  // USE_LVGL_KEY_LISTENER

#ifdef USE_LVGL_HID
// Keys that type into a focused text area rather than act: letters, digits, space, punctuation and
// the keypad but its Enter, without Ctrl or GUI.
static bool types_text(uint16_t page, uint16_t usage, uint8_t mods) {
  if (page != hid::PAGE_KEYBOARD || (mods & (KeyBindings::MOD_CTRL | KeyBindings::MOD_GUI)) != 0)
    return false;
  return (usage >= 0x04 && usage <= 0x27) || (usage >= 0x2C && usage <= 0x38) ||
         (usage >= 0x54 && usage <= 0x63 && usage != 0x58) || usage == 0x64 || (usage >= 0x87 && usage <= 0x89);
}

static bool is_textarea(lv_obj_t *obj) {
#ifdef USE_LVGL_TEXTAREA
  return obj != nullptr && lv_obj_check_type(obj, &lv_textarea_class);
#else
  return false;
#endif
}

// Whether the widget does something with the arrow key itself (a slider its value, an open
// dropdown its selection); otherwise the arrow moves the focus.
static bool uses_key(lv_obj_t *obj, uint32_t key) {
  [[maybe_unused]] const bool vertical = key == LV_KEY_UP || key == LV_KEY_DOWN;
#ifdef USE_LVGL_SLIDER
  if (lv_obj_check_type(obj, &lv_slider_class)) {
    const lv_slider_orientation_t orientation = lv_slider_get_orientation(obj);
    const bool upright = orientation == LV_SLIDER_ORIENTATION_VERTICAL ||
                         (orientation == LV_SLIDER_ORIENTATION_AUTO && lv_obj_get_height(obj) > lv_obj_get_width(obj));
    return vertical == upright;
  }
#endif
#ifdef USE_LVGL_ARC
  if (lv_obj_check_type(obj, &lv_arc_class))
    return !vertical;
#endif
#ifdef USE_LVGL_ROLLER
  if (lv_obj_check_type(obj, &lv_roller_class))
    return vertical;
#endif
#ifdef USE_LVGL_DROPDOWN
  if (lv_obj_check_type(obj, &lv_dropdown_class))
    return lv_dropdown_is_open(obj);
#endif
#ifdef USE_LVGL_TEXTAREA
  if (lv_obj_check_type(obj, &lv_textarea_class))
    return !vertical || !lv_textarea_get_one_line(obj);
#endif
#ifdef USE_LVGL_SPINBOX
  if (lv_obj_check_type(obj, &lv_spinbox_class))
    return true;
#endif
#ifdef USE_LVGL_BUTTONMATRIX
  if (lv_obj_check_type(obj, &lv_buttonmatrix_class))
    return true;
#endif
#ifdef USE_LVGL_KEYBOARD
  if (lv_obj_check_type(obj, &lv_keyboard_class))
    return true;
#endif
#ifdef USE_LVGL_TABLE
  if (lv_obj_check_type(obj, &lv_table_class))
    return true;
#endif
  return false;
}

LVHIDKeypadListener::LVHIDKeypadListener(hid::HIDInput *input, KeyBindings *bindings, uint16_t long_press_time,
                                         uint16_t long_press_repeat_time)
    : bindings_(bindings) {
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    static_cast<LVHIDKeypadListener *>(lv_indev_get_user_data(d))->read_(data);
  });
  if (input == nullptr)
    return;
  input->add_on_key_event_callback([this, input](uint16_t page, uint16_t usage, bool pressed) {
    LvglComponent::KeyTap *tap = this->parent_ != nullptr ? this->parent_->get_key_tap() : nullptr;
    if (tap != nullptr && tap->key_tap(input->key_source(), input->key_source_label(), page, usage, pressed))
      return;
    const bool keyboard = page == hid::PAGE_KEYBOARD;
    this->push_(static_cast<uint32_t>(page) << 16 | usage, KeyBindings::modifiers_of(input->get_modifiers()),
                pressed ? KEY_PRESS : KEY_RELEASE, keyboard ? input->layout_char(usage) : 0,
                keyboard && usage == 0xE6 && input->layout_has_level3(), input->key_source());
  });
  // Typed characters; Enter and Tab are keys.
  input->add_on_text_callback([this](const std::string &text) { this->push_text(text); });
}

void LVHIDKeypadListener::push_text(const std::string &text) {
  for (size_t i = 0; i < text.size();) {
    const auto lead = static_cast<uint8_t>(text[i]);
    const size_t n = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    uint32_t c = n == 1 ? lead : lead & (0x7F >> n);
    for (size_t j = 1; j < n && i + j < text.size(); j++)
      c = (c << 6) | (static_cast<uint8_t>(text[i + j]) & 0x3F);
    i += n;
    if (c == '\n' || c == '\t')
      continue;
    this->push_(c, 0, TEXT);
  }
}

bool LVHIDKeypadListener::push_action(int action) {
  return action >= 0 && this->push_(static_cast<uint32_t>(action), 0, ACTION);
}

// From the HID transport's task; read_() takes them on the LVGL thread.
bool LVHIDKeypadListener::push_(uint32_t code, uint8_t mods, EventKind kind, uint32_t layout_char, bool level3,
                                uint32_t source) {
  if (this->parent_->is_paused()) {
    if (kind == KEY_RELEASE)
      this->parent_->maybe_wakeup();
    if (kind == TEXT && this->parent_->get_paused_input() == nullptr)
      return false;
  }
  const uint8_t tail = this->tail_.load(std::memory_order_relaxed);
  if (static_cast<uint8_t>(tail - this->head_.load(std::memory_order_acquire)) == QUEUE_SIZE)
    return false;
  this->queue_[tail % QUEUE_SIZE] = {code, layout_char, mods, kind, level3, source};
  this->tail_.store(tail + 1, std::memory_order_release);
  return true;
}

void LVHIDKeypadListener::read_(lv_indev_data_t *data) {
  if (this->release_next_) {
    this->release_next_ = false;
    this->pressed_ = false;
  } else {
    uint8_t head = this->head_.load(std::memory_order_relaxed);
    while (head != this->tail_.load(std::memory_order_acquire)) {
      const Event event = this->queue_[head % QUEUE_SIZE];
      this->head_.store(++head, std::memory_order_release);
      if (event.kind == TEXT) {
        if (!is_textarea(this->focused_()))
          continue;
        this->key_ = lv_text_unicode_to_encoded(event.code);
        this->pressed_ = true;
        this->release_next_ = true;
        break;
      }
      // A key_bindings action given by name (remote input): what one of its keys does, pressed and let go.
      if (event.kind == ACTION) {
        const int action = static_cast<int>(event.code);
        const uint32_t key = action < KeyBindings::BUILTIN_COUNT ? this->builtin_(action, this->focused_()) : 0;
        this->bindings_->fire(action);
        if (key == 0)
          continue;
        this->key_ = key;
        this->pressed_ = true;
        this->release_next_ = true;
        break;
      }
      this->track_(event.code, event.kind == KEY_PRESS);
      if (event.kind == KEY_PRESS) {
        Held *slot = nullptr;
        for (Held &held : this->held_) {
          if (held.code == 0) {
            slot = &held;
            break;
          }
        }
        const uint32_t key = this->press_(event.code >> 16, event.code & 0xFFFF, event.mods);
        if (key == 0 || slot == nullptr)
          continue;
        *slot = {event.code, key};
        this->key_ = key;
        this->pressed_ = true;
        break;
      }
      if (this->bindings_->take_learned_release(event.code >> 16, event.code & 0xFFFF))
        continue;
      if (LVHIDPointerListener *pointer = this->parent_->get_key_pointer())
        pointer->key_release(event.code);
      Held *held = nullptr;
      for (Held &h : this->held_) {
        if (h.code == event.code)
          held = &h;
      }
      if (held == nullptr)
        continue;
      this->key_ = held->lv_key;
      held->code = 0;
      this->pressed_ = false;
      break;
    }
  }
  data->key = this->key_;
  data->state = this->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->continue_reading =
      this->release_next_ || this->head_.load(std::memory_order_relaxed) != this->tail_.load(std::memory_order_acquire);
}

void LVHIDKeypadListener::run_paused() {
  LvglComponent::PausedInput *taker = this->parent_->get_paused_input();
  uint8_t head = this->head_.load(std::memory_order_relaxed);
  while (head != this->tail_.load(std::memory_order_acquire)) {
    const Event event = this->queue_[head % QUEUE_SIZE];
    this->head_.store(++head, std::memory_order_release);
    if (event.kind == TEXT) {
      if (taker != nullptr)
        taker->paused_text(event.code);
      continue;
    }
    const uint16_t page = event.code >> 16;
    const uint16_t usage = event.code & 0xFFFF;
    this->track_(event.code, event.kind == KEY_PRESS);
    if (event.kind == KEY_RELEASE) {
      if (this->bindings_->take_learned_release(page, usage))
        continue;
      for (Held &held : this->held_) {
        if (held.code == event.code) {
          held.code = 0;
          this->pressed_ = false;
        }
      }
      if (taker != nullptr)
        taker->paused_key(page, usage, event.mods, false, this->bindings_->resolve(page, usage, event.mods),
                          event.layout_char, event.level3, event.source);
      continue;
    }
    if (event.kind != KEY_PRESS)
      continue;
    if (this->bindings_->take_learned(page, usage, event.mods))
      continue;
    const int combo = this->bindings_->resolve_combo(page, usage, this->down_, this->down_count_);
    const int action = combo >= 0 ? combo : this->bindings_->resolve(page, usage, event.mods);
    if (taker != nullptr &&
        taker->paused_key(page, usage, event.mods, true, action, event.layout_char, event.level3, event.source) &&
        action < KeyBindings::BUILTIN_COUNT)
      continue;
    this->bindings_->fire(action);
  }
}

void LVHIDKeypadListener::track_(uint32_t code, bool pressed) {
  uint8_t at = 0;
  while (at < this->down_count_ && this->down_[at] != code)
    at++;
  if (pressed && at == this->down_count_ && this->down_count_ < DOWN_SIZE) {
    this->down_[this->down_count_++] = code;
  } else if (!pressed && at < this->down_count_) {
    this->down_[at] = this->down_[--this->down_count_];
  }
}

uint32_t LVHIDKeypadListener::press_(uint16_t page, uint16_t usage, uint8_t mods) {
  if (this->bindings_->take_learned(page, usage, mods))
    return 0;
  const int combo = this->bindings_->resolve_combo(page, usage, this->down_, this->down_count_);
  // A page testing a gamepad gets its buttons and D-pad; only combos still act.
  if (this->bindings_->capturing() && KeyBindings::is_pad_key(page, usage)) {
    this->bindings_->fire(combo);
    return 0;
  }
  lv_obj_t *focused = this->focused_();
  if (combo >= 0) {
    const uint32_t key = combo < KeyBindings::BUILTIN_COUNT ? this->builtin_(combo, focused) : 0;
    this->bindings_->fire(combo);
    return key;
  }
  if (is_textarea(focused) && types_text(page, usage, mods))
    return 0;
  const int action = this->bindings_->resolve(page, usage, mods);
  if (action < 0)
    return 0;
  LVHIDPointerListener *pointer = this->parent_->get_key_pointer();
  if (pointer != nullptr && pointer->is_key_mode() &&
      ((action >= KeyBindings::ACT_UP && action <= KeyBindings::ACT_RIGHT) || action == KeyBindings::ACT_ENTER)) {
    static constexpr uint32_t KEYS[] = {LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT};
    pointer->key_press(static_cast<uint32_t>(page) << 16 | usage,
                       action == KeyBindings::ACT_ENTER ? static_cast<uint32_t>(LV_KEY_ENTER)
                                                        : KEYS[action - KeyBindings::ACT_UP]);
    return 0;
  }
  const uint32_t key = action < KeyBindings::BUILTIN_COUNT ? this->builtin_(action, focused) : 0;
  this->bindings_->fire(action);
  return key;
}

uint32_t LVHIDKeypadListener::builtin_(int action, lv_obj_t *focused) {
  lv_obj_t *modal = this->modal_();
  lv_obj_t *from = focused != nullptr && this->reachable_(focused, modal) ? focused : nullptr;
  switch (action) {
    case KeyBindings::ACT_UP:
    case KeyBindings::ACT_DOWN:
    case KeyBindings::ACT_LEFT:
    case KeyBindings::ACT_RIGHT: {
      static constexpr uint32_t ARROWS[] = {LV_KEY_UP, LV_KEY_DOWN, LV_KEY_LEFT, LV_KEY_RIGHT};
      const uint32_t key = ARROWS[action - KeyBindings::ACT_UP];
      if (from != nullptr && uses_key(from, key))
        return key;
      this->move_focus_(key, from, modal);
      return 0;
    }
    case KeyBindings::ACT_NEXT:
    case KeyBindings::ACT_PREV:
      this->step_focus_(action == KeyBindings::ACT_NEXT, from, modal);
      return 0;
    case KeyBindings::ACT_ENTER:
      if (from == nullptr) {
        this->move_focus_(0, nullptr, modal);
        return 0;
      }
      return LV_KEY_ENTER;
    case KeyBindings::ACT_BACK:
      return LV_KEY_ESC;
    case KeyBindings::ACT_HOME:
      return LV_KEY_HOME;
    case KeyBindings::ACT_END:
      return LV_KEY_END;
    case KeyBindings::ACT_PAGE_UP:
    case KeyBindings::ACT_PAGE_DOWN:
      this->scroll_page_(action == KeyBindings::ACT_PAGE_DOWN, from, modal);
      return 0;
    case KeyBindings::ACT_BACKSPACE:
      return LV_KEY_BACKSPACE;
    case KeyBindings::ACT_DELETE:
      return LV_KEY_DEL;
    default:
      return 0;
  }
}

lv_obj_t *LVHIDKeypadListener::focused_() const {
  lv_group_t *group = lv_indev_get_group(this->drv_);
  return group != nullptr ? lv_group_get_focused(group) : nullptr;
}

lv_obj_t *LVHIDKeypadListener::modal_() const {
  lv_obj_t *top = lv_display_get_layer_top(this->parent_->get_disp());
  if (top == nullptr)
    return nullptr;
  lv_area_t full;
  lv_obj_get_coords(top, &full);
  for (auto i = static_cast<int32_t>(lv_obj_get_child_count(top)) - 1; i >= 0; i--) {
    lv_obj_t *child = lv_obj_get_child(top, i);
    if (lv_obj_is_hidden(child))
      continue;
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    if (area.x1 <= full.x1 && area.y1 <= full.y1 && area.x2 >= full.x2 && area.y2 >= full.y2)
      return child;
  }
  return nullptr;
}

bool LVHIDKeypadListener::reachable_(lv_obj_t *obj, lv_obj_t *modal) const {
  if (lv_obj_is_disabled(obj))
    return false;
  bool in_modal = false;
  lv_obj_t *root = obj;
  for (lv_obj_t *o = obj; o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_is_hidden(o))
      return false;
    in_modal = in_modal || o == modal;
    root = o;
  }
  if (modal != nullptr)
    return in_modal;
  lv_display_t *disp = this->parent_->get_disp();
  return root == lv_display_get_screen_active(disp) || root == lv_display_get_layer_top(disp);
}

void LVHIDKeypadListener::move_focus_(uint32_t direction, lv_obj_t *from, lv_obj_t *modal) {
  lv_group_t *group = lv_indev_get_group(this->drv_);
  if (group == nullptr)
    return;
  lv_area_t cur{};
  if (from != nullptr)
    lv_obj_get_coords(from, &cur);
  const int32_t cx = (cur.x1 + cur.x2) / 2;
  const int32_t cy = (cur.y1 + cur.y2) / 2;
  lv_obj_t *best = nullptr;
  bool best_aligned = false;
  int64_t best_score = INT64_MAX;
  const uint32_t count = lv_group_get_obj_count(group);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *obj = lv_group_get_obj_by_index(group, i);
    if (obj == from || !this->reachable_(obj, modal))
      continue;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const int32_t ax = (area.x1 + area.x2) / 2;
    const int32_t ay = (area.y1 + area.y2) / 2;
    int64_t score;
    bool aligned = false;
    if (from == nullptr) {
      // The first one: top-most, then left-most.
      score = static_cast<int64_t>(area.y1) * 65536 + area.x1;
    } else {
      int32_t ahead, gap, offset;
      switch (direction) {
        case LV_KEY_RIGHT:
          if (ax <= cx)
            continue;
          ahead = area.x1 - cur.x2;
          gap = std::max(area.y1 - cur.y2, cur.y1 - area.y2);
          offset = std::abs(ay - cy);
          break;
        case LV_KEY_LEFT:
          if (ax >= cx)
            continue;
          ahead = cur.x1 - area.x2;
          gap = std::max(area.y1 - cur.y2, cur.y1 - area.y2);
          offset = std::abs(ay - cy);
          break;
        case LV_KEY_DOWN:
          if (ay <= cy)
            continue;
          ahead = area.y1 - cur.y2;
          gap = std::max(area.x1 - cur.x2, cur.x1 - area.x2);
          offset = std::abs(ax - cx);
          break;
        default:  // LV_KEY_UP
          if (ay >= cy)
            continue;
          ahead = cur.y1 - area.y2;
          gap = std::max(area.x1 - cur.x2, cur.x1 - area.x2);
          offset = std::abs(ax - cx);
          break;
      }
      // In line with the focused one (overlapping across the direction) wins; then the nearest.
      ahead = std::max<int32_t>(ahead, 0);
      aligned = gap <= 0;
      score = aligned ? static_cast<int64_t>(ahead) * 4 + offset : static_cast<int64_t>(ahead) + 2LL * gap;
    }
    if ((aligned && !best_aligned) || (aligned == best_aligned && score < best_score)) {
      best = obj;
      best_aligned = aligned;
      best_score = score;
    }
  }
  if (best != nullptr)
    lv_group_focus_obj(best);
}

void LVHIDKeypadListener::step_focus_(bool forward, lv_obj_t *from, lv_obj_t *modal) {
  lv_group_t *group = lv_indev_get_group(this->drv_);
  if (group == nullptr)
    return;
  const auto count = static_cast<int32_t>(lv_group_get_obj_count(group));
  int32_t start = forward ? -1 : 0;
  for (int32_t i = 0; from != nullptr && i < count; i++) {
    if (lv_group_get_obj_by_index(group, i) == from)
      start = i;
  }
  for (int32_t n = 1; n <= count; n++) {
    const int32_t index = ((start + (forward ? n : -n)) % count + count) % count;
    lv_obj_t *obj = lv_group_get_obj_by_index(group, index);
    if (obj != from && this->reachable_(obj, modal)) {
      lv_group_focus_obj(obj);
      return;
    }
  }
}

void LVHIDKeypadListener::scroll_page_(bool down, lv_obj_t *from, lv_obj_t *modal) {
  if (from == nullptr) {
    this->move_focus_(0, nullptr, modal);
    return;
  }
  lv_obj_t *list = nullptr;
  for (lv_obj_t *o = lv_obj_get_parent(from); o != nullptr; o = lv_obj_get_parent(o)) {
    if (lv_obj_is_scrollable(o) && (lv_obj_get_scroll_dir(o) & LV_DIR_VER) != 0 &&
        lv_obj_get_scroll_top(o) + lv_obj_get_scroll_bottom(o) > 0) {
      list = o;
      break;
    }
  }
  lv_group_t *group = lv_indev_get_group(this->drv_);
  if (list == nullptr || group == nullptr)
    return;
  lv_area_t before;
  lv_obj_get_coords(from, &before);
  const int32_t page = lv_obj_get_content_height(list);
  lv_obj_scroll_by_bounded(list, 0, down ? -page : page, LV_ANIM_OFF);
  const int32_t cx = (before.x1 + before.x2) / 2;
  const int32_t cy = (before.y1 + before.y2) / 2;
  lv_obj_t *best = nullptr;
  int64_t best_distance = INT64_MAX;
  const uint32_t count = lv_group_get_obj_count(group);
  for (uint32_t i = 0; i < count; i++) {
    lv_obj_t *obj = lv_group_get_obj_by_index(group, i);
    if (!this->reachable_(obj, modal))
      continue;
    bool inside = false;
    for (lv_obj_t *o = lv_obj_get_parent(obj); o != nullptr && !inside; o = lv_obj_get_parent(o))
      inside = o == list;
    if (!inside)
      continue;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const int64_t distance = std::abs((area.y1 + area.y2) / 2 - cy) + std::abs((area.x1 + area.x2) / 2 - cx);
    if (distance < best_distance) {
      best = obj;
      best_distance = distance;
    }
  }
  if (best != nullptr)
    lv_group_focus_obj(best);
}

LVHIDPointerListener::LVHIDPointerListener(hid::HIDInput *input, uint16_t long_press_time,
                                           uint16_t long_press_repeat_time, LvglComponent *parent) {
  this->set_parent(parent);
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(this->drv_, parent->get_disp());
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  if (parent->get_key_pointer() == nullptr)
    parent->set_key_pointer(this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    auto *l = static_cast<LVHIDPointerListener *>(lv_indev_get_user_data(d));
    const bool pad = l->pad_read_();
    if (l->key_mode_)
      l->key_read_();
    data->point.x = l->x_;
    data->point.y = l->y_;
    data->state = l->pressed_ || l->press_unread_ || l->key_pressed_ || pad ? LV_INDEV_STATE_PRESSED
                                                                            : LV_INDEV_STATE_RELEASED;
    l->press_unread_ = false;
    if (l->hide_after_ != 0 && l->cursor_ != nullptr && !l->pressed_ && !pad &&
        millis() - l->last_motion_ > l->hide_after_ && !lv_obj_is_hidden(l->cursor_))
      lv_obj_set_hidden(l->cursor_, true);
  });
  this->add_input(input);
}

void LVHIDPointerListener::add_input(hid::HIDInput *input) {
  input->add_on_pointer_callback([this](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
    this->move_(dx, dy, wheel, pan, buttons);
  });
  this->add_pad_touch_input(input);
}

void LVHIDPointerListener::add_pad_touch_input(hid::HIDInput *input) {
  input->add_on_control_callback(
      [this](uint16_t page, uint16_t usage, int32_t value) { this->pad_control_(page, usage, value); });
  input->add_on_key_event_callback(
      [this](uint16_t page, uint16_t usage, bool down) { this->pad_key_(page, usage, down); });
}

void LVHIDPointerListener::pad_reset_() {
  this->pad_dx_ = this->pad_dy_ = this->pad_sx_ = this->pad_sy_ = 0;
  this->pad_tap_ = this->pad_tap_shown_ = false;
}

void LVHIDPointerListener::pad_control_(uint16_t page, uint16_t usage, int32_t value) {
  if (page != hid::PAGE_DIGITIZER)
    return;
  const bool active = this->pad_touch_ && !this->parent_->is_paused();
  if (!active)
    this->pad_reset_();
  for (uint8_t i = 0; i < 2; i++) {
    PadFinger &finger = this->pad_fingers_[i];
    int32_t dx = 0;
    int32_t dy = 0;
    if (usage == hid::USAGE_FINGER_DOWN[i]) {
      const uint8_t before = this->pad_fingers_[0].down + this->pad_fingers_[1].down;
      finger = {finger.x, finger.y, value != 0, false, false};
      const uint8_t now = this->pad_fingers_[0].down + this->pad_fingers_[1].down;
      if (before == 0 && now != 0) {
        this->pad_since_ = millis();
        this->pad_moved_ = 0;
        this->pad_most_ = 0;
        this->pad_no_tap_ = this->pad_button_;
      }
      this->pad_most_ = std::max(this->pad_most_, now);
      if (active && now == 0 && before != 0 && this->pad_most_ == 1 && !this->pad_no_tap_ &&
          this->pad_moved_ < PAD_TAP_MOVE && millis() - this->pad_since_ < PAD_TAP_MS)
        this->pad_tap_ = true;
      return;
    }
    if (usage == hid::USAGE_FINGER_X[i]) {
      if (finger.has_x)
        dx = value - finger.x;
      finger.x = value;
      finger.has_x = true;
    } else if (usage == hid::USAGE_FINGER_Y[i]) {
      if (finger.has_y)
        dy = value - finger.y;
      finger.y = value;
      finger.has_y = true;
    } else {
      continue;
    }
    if (!active)
      return;
    this->pad_moved_ += std::abs(dx) + std::abs(dy);
    // Two fingers scroll by their mean motion, one moves the pointer.
    if (this->pad_fingers_[0].down && this->pad_fingers_[1].down) {
      this->pad_sx_ += dx;
      this->pad_sy_ += dy;
    } else {
      this->pad_dx_ += dx;
      this->pad_dy_ += dy;
    }
    return;
  }
}

void LVHIDPointerListener::pad_key_(uint16_t page, uint16_t usage, bool down) {
  // hid::PAD_TOUCHPAD's button.
  static constexpr uint16_t TOUCHPAD_BUTTON = __builtin_ctz(hid::PAD_TOUCHPAD) + 1;
  if (page != hid::PAGE_BUTTON || usage != TOUCHPAD_BUTTON)
    return;
  this->pad_button_ = down && this->pad_touch_ && !this->parent_->is_paused();
  if (down)
    this->pad_no_tap_ = true;
}

bool LVHIDPointerListener::pad_read_() {
  if (!this->pad_touch_)
    return false;
  if (this->pad_capture_ != nullptr && lv_obj_get_screen(this->pad_capture_) == lv_screen_active() &&
      lv_obj_is_visible(this->pad_capture_)) {
    this->pad_reset_();
    return false;
  }
  lv_display_t *disp = this->parent_->get_disp();
  const int32_t width = lv_display_get_horizontal_resolution(disp);
  const int32_t height = lv_display_get_vertical_resolution(disp);
  if (this->pad_dx_ != 0 || this->pad_dy_ != 0) {
    // A touchpad width is the display's width.
    const float px = static_cast<float>(this->pad_dx_) * width / 65535.0f;
    const float py = static_cast<float>(this->pad_dy_) * width / 65535.0f;
    this->pad_dx_ = this->pad_dy_ = 0;
    float factor = this->speed_;
    if (this->acceleration_)
      factor *= std::min(3.0f, 1.0f + (std::abs(px) + std::abs(py)) / 20.0f);
    const float fx = px * factor + this->pad_rest_x_;
    const float fy = py * factor + this->pad_rest_y_;
    const auto mx = static_cast<int32_t>(fx);
    const auto my = static_cast<int32_t>(fy);
    this->pad_rest_x_ = fx - mx;
    this->pad_rest_y_ = fy - my;
    this->x_ = std::clamp<int32_t>(this->x_ + mx, 0, width - 1);
    this->y_ = std::clamp<int32_t>(this->y_ + my, 0, height - 1);
    this->last_motion_ = millis();
    if (this->cursor_ != nullptr && lv_obj_is_hidden(this->cursor_))
      lv_obj_set_hidden(this->cursor_, false);
  }
  // The content follows the fingers (reversed by the reverse wheel setting); what is below a pixel
  // waits for the next read. Both fingers' motion is gathered: halved.
  const int32_t sx = this->pad_sx_ * width / (2 * 65535);
  const int32_t sy = this->pad_sy_ * width / (2 * 65535);
  if (sx != 0 || sy != 0) {
    this->pad_sx_ -= sx * 2 * 65535 / width;
    this->pad_sy_ -= sy * 2 * 65535 / width;
    const int32_t sign = this->reverse_wheel_ ? -1 : 1;
    lv_point_t point{this->x_, this->y_};
    for (lv_obj_t *obj = lv_indev_search_obj(lv_screen_active(), &point); obj != nullptr; obj = lv_obj_get_parent(obj)) {
      if (lv_obj_is_scrollable(obj)) {
        lv_obj_scroll_by_bounded(obj, sign * sx, sign * sy, LV_ANIM_OFF);
        break;
      }
    }
  }
  // A tap is pressed for one read, let go at the next.
  if (this->pad_tap_shown_) {
    this->pad_tap_ = this->pad_tap_shown_ = false;
  } else if (this->pad_tap_) {
    this->pad_tap_shown_ = true;
    return true;
  }
  return this->pad_button_;
}

void LVHIDPointerListener::set_key_mode(bool on) {
  this->key_mode_ = on;
  for (KeyHeld &held : this->keys_)
    held = {};
  this->key_pressed_ = false;
  if (on && this->cursor_ != nullptr && !this->parent_->is_paused()) {
    this->last_motion_ = millis();
    if (lv_obj_is_hidden(this->cursor_))
      lv_obj_set_hidden(this->cursor_, false);
  }
}

void LVHIDPointerListener::key_press(uint32_t code, uint32_t key) {
  if (key == LV_KEY_ENTER) {
    this->key_pressed_ = true;
  } else {
    bool any = false;
    for (const KeyHeld &held : this->keys_)
      any = any || (held.code != 0 && held.key != LV_KEY_ENTER);
    if (!any)
      this->key_since_ = millis();
  }
  for (KeyHeld &held : this->keys_) {
    if (held.code == 0) {
      held = {code, key};
      break;
    }
  }
  this->last_motion_ = millis();
  if (this->cursor_ != nullptr && lv_obj_is_hidden(this->cursor_))
    lv_obj_set_hidden(this->cursor_, false);
}

void LVHIDPointerListener::key_release(uint32_t code) {
  for (KeyHeld &held : this->keys_) {
    if (held.code == code) {
      if (held.key == LV_KEY_ENTER)
        this->key_pressed_ = false;
      held = {};
    }
  }
}

// Every read while a direction is held: a step that way, faster once held long enough.
void LVHIDPointerListener::key_read_() {
  int32_t dx = 0;
  int32_t dy = 0;
  for (const KeyHeld &held : this->keys_) {
    if (held.code == 0)
      continue;
    dx += held.key == LV_KEY_RIGHT ? 1 : held.key == LV_KEY_LEFT ? -1 : 0;
    dy += held.key == LV_KEY_DOWN ? 1 : held.key == LV_KEY_UP ? -1 : 0;
  }
  if (dx == 0 && dy == 0)
    return;
  const uint32_t held_ms = millis() - this->key_since_;
  float factor = 1.0f;
  if (held_ms > this->key_after_ms_ && this->key_most_ > 1) {
    const uint32_t ramp = this->key_after_ms_ < 2000 ? 2000 - this->key_after_ms_ : 1;
    factor += (this->key_most_ - 1) * std::min(1.0f, static_cast<float>(held_ms - this->key_after_ms_) / ramp);
  }
  const auto step = static_cast<int32_t>(this->key_step_ * factor);
  lv_display_t *disp = this->parent_->get_disp();
  this->x_ = std::clamp<int32_t>(this->x_ + dx * step, 0, lv_display_get_horizontal_resolution(disp) - 1);
  this->y_ = std::clamp<int32_t>(this->y_ + dy * step, 0, lv_display_get_vertical_resolution(disp) - 1);
  this->last_motion_ = millis();
}

void LVHIDPointerListener::set_cursor_(lv_obj_t *cursor) {
  lv_obj_set_clickable(cursor, false);
  lv_indev_set_cursor(this->drv_, cursor);
  this->cursor_ = cursor;
  if (this->hide_after_ != 0)
    lv_obj_set_hidden(cursor, true);  // until the pointer first moves
}

void LVHIDPointerListener::set_hide_after(uint32_t ms) {
  this->hide_after_ = ms;
  if (ms != 0 && this->cursor_ != nullptr)
    lv_obj_set_hidden(this->cursor_, true);
}

#ifdef USE_LVGL_IMAGE
void LVHIDPointerListener::set_cursor_image(const void *src) {
  lv_obj_t *cursor = lv_image_create(lv_layer_sys());
  lv_image_set_src(cursor, src);
  this->set_cursor_(cursor);
}
#endif

void LVHIDPointerListener::set_default_cursor() {
  this->own_cursor_ = true;
  this->set_cursor_look(0, 1);
}

// The arrow cursor: a white triangle on a dark one a pixel larger, its tip at the pointer.
static void draw_arrow_cursor(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_current_target_obj(e);
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int32_t h = lv_area_get_height(&a) - 3;
  const int32_t w = h * 3 / 4;
  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.opa = LV_OPA_COVER;
  for (int32_t edge = 1; edge >= 0; edge--) {
    dsc.color = edge != 0 ? lv_color_black() : lv_color_white();
    dsc.p[0].x = a.x1 + 1 - edge;
    dsc.p[0].y = a.y1 + 1 - 2 * edge;
    dsc.p[1].x = a.x1 + 1 - edge;
    dsc.p[1].y = a.y1 + 1 + h + edge;
    dsc.p[2].x = a.x1 + 1 + w + 2 * edge;
    dsc.p[2].y = a.y1 + 1 + w + edge;
    lv_draw_triangle(layer, &dsc);
  }
}

void LVHIDPointerListener::set_cursor_look(uint8_t style, uint8_t size) {
  if (!this->own_cursor_)
    return;  // a configured cursor image stays as it is
  this->look_style_ = style;
  this->look_size_ = std::min<uint8_t>(size, 2);
  // Nothing of LVGL while it is paused: the next motion (LVGL runs again) makes it.
  this->look_pending_ = this->parent_->is_paused();
  if (!this->look_pending_)
    this->apply_look_();
}

void LVHIDPointerListener::apply_look_() {
  this->look_pending_ = false;
  const uint8_t style = this->look_style_;
  static constexpr int32_t DIAMETER[3] = {10, 14, 20};
  const int32_t d = DIAMETER[this->look_size_];
  lv_obj_t *old = this->cursor_;
  const bool shown = old != nullptr && !lv_obj_is_hidden(old);
  lv_obj_t *cursor = lv_obj_create(lv_layer_sys());
  lv_obj_remove_style_all(cursor);
  if (style >= 2) {
    const int32_t h = d * 3 / 2;
    lv_obj_set_size(cursor, h * 3 / 4 + 4, h + 4);
    lv_obj_add_event_cb(cursor, draw_arrow_cursor, LV_EVENT_DRAW_MAIN, nullptr);
  } else {
    // Dot and ring: centred on the point
    lv_obj_set_size(cursor, d, d);
    lv_obj_set_style_translate_x(cursor, -d / 2, 0);
    lv_obj_set_style_translate_y(cursor, -d / 2, 0);
    lv_obj_set_style_radius(cursor, LV_RADIUS_CIRCLE, 0);
    if (style == 0) {
      lv_obj_set_style_bg_opa(cursor, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(cursor, lv_color_white(), 0);
      lv_obj_set_style_border_width(cursor, 2, 0);
      lv_obj_set_style_border_color(cursor, lv_color_black(), 0);
    } else {
      lv_obj_set_style_border_width(cursor, 3, 0);
      lv_obj_set_style_border_color(cursor, lv_color_white(), 0);
      lv_obj_set_style_outline_width(cursor, 1, 0);
      lv_obj_set_style_outline_color(cursor, lv_color_black(), 0);
      lv_obj_set_style_outline_opa(cursor, LV_OPA_COVER, 0);
    }
  }
  this->set_cursor_(cursor);
  if (shown)
    lv_obj_set_hidden(cursor, false);
  if (old != nullptr)
    lv_obj_delete(old);
}

void LVHIDPointerListener::long_press_here() {
  if (this->parent_->is_paused())
    return;
  lv_point_t point{this->x_, this->y_};
  lv_obj_t *obj = lv_indev_search_obj(lv_layer_top(), &point);
  if (obj == nullptr || obj == lv_layer_top())
    obj = lv_indev_search_obj(lv_screen_active(), &point);
  for (; obj != nullptr; obj = lv_obj_get_parent(obj)) {
    if (lv_obj_is_clickable(obj)) {
      lv_obj_send_event(obj, LV_EVENT_LONG_PRESSED, nullptr);
      return;
    }
  }
}

void LVHIDPointerListener::move_(int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
  if (this->parent_->is_paused()) {
    LvglComponent::PausedInput *taker = this->parent_->get_paused_input();
    if (taker != nullptr && taker->paused_pointer(dx, dy, wheel, buttons))
      return;
    if (buttons == 0)
      this->parent_->maybe_wakeup();
    return;
  }
  this->last_motion_ = millis();
  if (this->cursor_ != nullptr && lv_obj_is_hidden(this->cursor_))
    lv_obj_set_hidden(this->cursor_, false);
  if (this->look_pending_)
    this->apply_look_();
  lv_display_t *disp = this->parent_->get_disp();
  float factor = this->speed_;
  if (this->acceleration_)
    factor *= std::min(3.0f, 1.0f + static_cast<float>(std::abs(dx) + std::abs(dy)) / 20.0f);
  const float fx = dx * factor + this->rest_x_;
  const float fy = dy * factor + this->rest_y_;
  const auto mx = static_cast<int32_t>(fx);
  const auto my = static_cast<int32_t>(fy);
  this->rest_x_ = fx - mx;
  this->rest_y_ = fy - my;
  this->x_ = std::clamp<int32_t>(this->x_ + mx, 0, lv_display_get_horizontal_resolution(disp) - 1);
  this->y_ = std::clamp<int32_t>(this->y_ + my, 0, lv_display_get_vertical_resolution(disp) - 1);
  const uint8_t primary = this->left_handed_ ? 0x02 : 0x01;
  const uint8_t second = this->left_handed_ ? 0x01 : 0x02;
  this->pressed_ = (buttons & primary) != 0;
  if (this->pressed_)
    this->press_unread_ = true;
  const uint8_t changed = buttons ^ this->last_buttons_;
  this->last_buttons_ = buttons;
  if (changed & second)
    this->button_cb_.call(2, (buttons & second) != 0);
  for (uint8_t bit = 2; bit < 5; bit++) {  // middle, side 4, side 5
    if (changed & (1u << bit))
      this->button_cb_.call(bit + 1, ((buttons >> bit) & 1) != 0);
  }
  if (wheel == 0 && pan == 0)
    return;
  if (this->reverse_wheel_)
    wheel = -wheel;
  lv_point_t point{this->x_, this->y_};
  for (lv_obj_t *obj = lv_indev_search_obj(lv_screen_active(), &point); obj != nullptr; obj = lv_obj_get_parent(obj)) {
    if (lv_obj_is_scrollable(obj)) {
      const int32_t step = WHEEL_LINE * this->wheel_lines_;
      lv_obj_scroll_by_bounded(obj, -pan * step, wheel * step, LV_ANIM_ON);
      break;
    }
  }
}

#ifdef USE_LVGL_REMOTE_INPUT
LVRemoteInput *LVRemoteInput::create(KeyBindings *bindings, uint16_t long_press_time,
                                     uint16_t long_press_repeat_time, LvglComponent *parent, lv_group_t *group) {
  void *mem = RAMAllocator<LVRemoteInput>(RAMAllocator<LVRemoteInput>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the remote input");
    return nullptr;
  }
  auto *remote = new (mem) LVRemoteInput(bindings, long_press_time, long_press_repeat_time, parent);
  if (group != nullptr)
    lv_indev_set_group(remote->keypad_.get_drv(), group);
  return remote;
}

LVRemoteInput::LVRemoteInput(KeyBindings *bindings, uint16_t long_press_time, uint16_t long_press_repeat_time,
                             LvglComponent *parent)
    : keypad_(nullptr, bindings, long_press_time, long_press_repeat_time), bindings_(bindings) {
  this->set_parent(parent);
  this->keypad_.set_parent(parent);
  this->drv_ = lv_indev_create();
  lv_indev_set_type(this->drv_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_disp(this->drv_, parent->get_disp());
  lv_indev_set_long_press_time(this->drv_, long_press_time);
  lv_indev_set_long_press_repeat_time(this->drv_, long_press_repeat_time);
  lv_indev_set_user_data(this->drv_, this);
  lv_indev_set_read_cb(this->drv_, [](lv_indev_t *d, lv_indev_data_t *data) {
    static_cast<LVRemoteInput *>(lv_indev_get_user_data(d))->read_(data);
  });
}

bool LVRemoteInput::push_(const Step &step) {
  if (this->count_ == STEPS)
    return false;
  this->steps_[(this->head_ + this->count_) % STEPS] = step;
  this->count_++;
  return true;
}

const char *LVRemoteInput::run(const std::string &command, int32_t x, int32_t y, int32_t x2, int32_t y2, int32_t ms,
                               const std::string &text) {
  if (this->parent_->is_paused()) {
    // The screen is the player's (dashboard, video, desktop): only key_bindings actions, fired at once as
    // an HID key fires them then (LVHIDKeypadListener::run_paused); the queue is not read while paused.
    if (command != "key")
      return "LVGL is paused";
    const int action = this->bindings_ != nullptr ? this->bindings_->find_action(text) : -1;
    if (action < 0)
      return "no such key_bindings action";
    this->bindings_->fire(action);
    return nullptr;
  }
  auto coord = [](int32_t v) { return static_cast<int16_t>(clamp<int32_t>(v, 0, INT16_MAX)); };
  Step step{coord(x), coord(y), coord(x2), coord(y2), static_cast<uint16_t>(clamp<int32_t>(ms, 0, 10000)), STEP_TAP};
  if (command == "tap") {
    step.kind = STEP_TAP;
  } else if (command == "press") {
    step.kind = STEP_PRESS;
  } else if (command == "move") {
    step.kind = STEP_MOVE;
  } else if (command == "release") {
    step.kind = STEP_RELEASE;
  } else if (command == "swipe") {
    step.kind = STEP_SWIPE;
  } else if (command == "key") {
    const int action = this->bindings_ != nullptr ? this->bindings_->find_action(text) : -1;
    if (action < 0)
      return "no such key_bindings action";
    return this->keypad_.push_action(action) ? nullptr : "key queue full";
  } else if (command == "text") {
    if (!is_textarea(this->focused()))
      return "no text area has the focus";
    this->keypad_.push_text(text);
    return nullptr;
  } else {
    return "unknown command";
  }
  return this->push_(step) ? nullptr : "pointer queue full";
}

// LVGL thread, each read: the current step goes on, else the next one starts.
void LVRemoteInput::read_(lv_indev_data_t *data) {
  const uint32_t now = lv_tick_get();
  if (!this->running_ && this->count_ != 0) {
    this->current_ = this->steps_[this->head_];
    this->head_ = (this->head_ + 1) % STEPS;
    this->count_--;
    this->running_ = true;
    this->begun_ = false;
    this->started_ = now;
  }
  if (this->running_) {
    const Step &step = this->current_;
    switch (step.kind) {
      case STEP_TAP:
        // Down for one read, up the next: a click where it is
        if (!this->begun_) {
          this->x_ = step.x;
          this->y_ = step.y;
          this->pressed_ = true;
          this->begun_ = true;
        } else {
          this->pressed_ = false;
          this->running_ = false;
        }
        break;
      case STEP_PRESS:
        this->x_ = step.x;
        this->y_ = step.y;
        this->pressed_ = true;
        this->running_ = false;
        break;
      case STEP_MOVE:
        this->x_ = step.x;
        this->y_ = step.y;
        this->running_ = false;
        break;
      case STEP_RELEASE:
        this->pressed_ = false;
        this->running_ = false;
        break;
      case STEP_SWIPE: {
        const uint32_t elapsed = now - this->started_;
        if (!this->begun_) {
          this->x_ = step.x;
          this->y_ = step.y;
          this->pressed_ = true;
          this->begun_ = true;
        } else if (elapsed < step.ms) {
          this->x_ = static_cast<int16_t>(step.x + (step.x2 - step.x) * static_cast<int32_t>(elapsed) / step.ms);
          this->y_ = static_cast<int16_t>(step.y + (step.y2 - step.y) * static_cast<int32_t>(elapsed) / step.ms);
        } else if (this->x_ != step.x2 || this->y_ != step.y2) {
          this->x_ = step.x2;
          this->y_ = step.y2;
        } else {
          this->pressed_ = false;
          this->running_ = false;
        }
        break;
      }
    }
  }
  data->point.x = this->x_;
  data->point.y = this->y_;
  data->state = this->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_obj_t *LVRemoteInput::focused() {
  lv_group_t *group = lv_indev_get_group(this->keypad_.get_drv());
  return group != nullptr ? lv_group_get_focused(group) : nullptr;
}

const char *LVRemoteInput::type_name(lv_obj_t *obj) {
  if (obj == nullptr)
    return "";
#ifdef USE_LVGL_BUTTON
  if (lv_obj_check_type(obj, &lv_button_class))
    return "button";
#endif
#ifdef USE_LVGL_TEXTAREA
  if (lv_obj_check_type(obj, &lv_textarea_class))
    return "textarea";
#endif
#ifdef USE_LVGL_DROPDOWN
  if (lv_obj_check_type(obj, &lv_dropdown_class))
    return "dropdown";
#endif
#ifdef USE_LVGL_SLIDER
  if (lv_obj_check_type(obj, &lv_slider_class))
    return "slider";
#endif
#ifdef USE_LVGL_LABEL
  if (lv_obj_check_type(obj, &lv_label_class))
    return "label";
#endif
#ifdef USE_LVGL_SWITCH
  if (lv_obj_check_type(obj, &lv_switch_class))
    return "switch";
#endif
#ifdef USE_LVGL_CHECKBOX
  if (lv_obj_check_type(obj, &lv_checkbox_class))
    return "checkbox";
#endif
#ifdef USE_LVGL_ROLLER
  if (lv_obj_check_type(obj, &lv_roller_class))
    return "roller";
#endif
#ifdef USE_LVGL_KEYBOARD
  if (lv_obj_check_type(obj, &lv_keyboard_class))
    return "keyboard";
#endif
  return "obj";
}

const char *LVRemoteInput::text_of(lv_obj_t *obj) {
  if (obj == nullptr)
    return "";
#ifdef USE_LVGL_TEXTAREA
  if (lv_obj_check_type(obj, &lv_textarea_class))
    return lv_textarea_get_text(obj);
#endif
#ifdef USE_LVGL_LABEL
  if (lv_obj_check_type(obj, &lv_label_class))
    return lv_label_get_text(obj);
  const uint32_t children = lv_obj_get_child_count(obj);
  for (uint32_t i = 0; i < children; i++) {
    lv_obj_t *child = lv_obj_get_child(obj, static_cast<int32_t>(i));
    if (lv_obj_check_type(child, &lv_label_class))
      return lv_label_get_text(child);
  }
#endif
  return "";
}
#endif  // USE_LVGL_REMOTE_INPUT
#endif  // USE_LVGL_HID

#if defined(USE_LVGL_DROPDOWN) || defined(LV_USE_ROLLER)
std::string LvSelectable::get_selected_text() {
  auto selected = this->get_selected_index();
  if (selected >= this->options_.size())
    return "";
  return this->options_[selected];
}

static std::string join_string(const FixedVector<const char *> &options) {
  return std::accumulate(
      options.begin(), options.end(), std::string(),
      [](const std::string &a, const char *b) -> std::string { return a + (!a.empty() ? "\n" : "") + b; });
}

void LvSelectable::set_selected_text(const std::string &text, lv_anim_enable_t anim) {
  auto *index = std::find(this->options_.begin(), this->options_.end(), text);
  if (index != this->options_.end()) {
    this->set_selected_index(index - this->options_.begin(), anim);
    lv_obj_send_event(this->obj, lv_update_event, nullptr);
  }
}

void LvSelectable::set_options(FixedVector<const char *> options) {
  auto index = this->get_selected_index();
  if (index >= options.size())
    index = options.size() - 1;
  this->options_ = std::move(options);
  this->set_option_string(join_string(this->options_).c_str());
  lv_obj_send_event(this->obj, LV_EVENT_REFRESH, nullptr);
  this->set_selected_index(index, LV_ANIM_OFF);
}
#endif  // USE_LVGL_DROPDOWN || LV_USE_ROLLER

#ifdef USE_LVGL_BUTTONMATRIX
void LvButtonMatrixType::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_obj_add_event_cb(
      lv_obj,
      [](lv_event_t *event) {
        auto *self = static_cast<LvButtonMatrixType *>(lv_event_get_user_data(event));
        if (self->key_callback_.size() == 0)
          return;
        auto key_idx = lv_buttonmatrix_get_selected_button(self->obj);
        if (key_idx == LV_BUTTONMATRIX_BUTTON_NONE)
          return;
        if (self->key_map_.contains(key_idx)) {
          self->send_key_(self->key_map_[key_idx]);
          return;
        }
        const auto *str = lv_buttonmatrix_get_button_text(self->obj, key_idx);
        auto len = strlen(str);
        while (len--)
          self->send_key_(*str++);
      },
      LV_EVENT_PRESSED, this);
}
#endif  // USE_LVGL_BUTTONMATRIX

#ifdef USE_LVGL_KEYBOARD
static const char *const KB_SPECIAL_KEYS[] = {
    "abc",
    "ABC",
    "1#",
    // maybe add other special keys here
};

void LvKeyboardType::set_obj(lv_obj_t *lv_obj) {
  LvCompound::set_obj(lv_obj);
  lv_obj_add_event_cb(
      lv_obj,
      [](lv_event_t *event) {
        auto *self = static_cast<LvKeyboardType *>(lv_event_get_user_data(event));
        if (self->key_callback_.size() == 0)
          return;

        auto key_idx = lv_buttonmatrix_get_selected_button(self->obj);
        if (key_idx == LV_BUTTONMATRIX_BUTTON_NONE)
          return;
        const char *txt = lv_buttonmatrix_get_button_text(self->obj, key_idx);
        if (txt == nullptr)
          return;
        for (const auto *kb_special_key : KB_SPECIAL_KEYS) {
          if (strcmp(txt, kb_special_key) == 0)
            return;
        }
        while (*txt != 0)
          self->send_key_(*txt++);
      },
      LV_EVENT_PRESSED, this);
}
#endif  // USE_LVGL_KEYBOARD

void LvglComponent::draw_end_() {
  if (this->draw_end_callback_ != nullptr)
    this->draw_end_callback_->trigger();
  this->draw_end_callbacks_.call();
  // Only reachable once the display is idle again: while busy, the display's refr_timer_ is
  // paused (see loop()), so LVGL never renders/flushes and this event never fires.
  if (this->update_when_display_idle_) {
    for (auto *disp : this->displays_)
      disp->update();
  }
}

bool LvglComponent::displays_busy_() const {
  if (!this->update_when_display_idle_)
    return false;
  for (auto *disp : this->displays_) {
    if (!disp->is_idle())
      return true;
  }
  return false;
}

void LvglComponent::write_random_() {
  int iterations = 6 - lv_display_get_inactive_time(this->disp_) / 60000;
  if (iterations <= 0)
    iterations = 1;
  int16_t width = lv_display_get_horizontal_resolution(this->disp_);
  int16_t height = lv_display_get_vertical_resolution(this->disp_);
  while (iterations-- != 0) {
    int32_t col = random_uint32() % width;
    col = col / this->draw_rounding * this->draw_rounding;
    int32_t row = random_uint32() % height;
    row = row / this->draw_rounding * this->draw_rounding;
    // size will be between 8 and 32, and a multiple of draw_rounding
    int32_t size = (random_uint32() % 25 + 8) / this->draw_rounding * this->draw_rounding;
    lv_area_t area{.x1 = col, .y1 = row, .x2 = col + size - 1, .y2 = row + size - 1};
    // clip to display bounds just in case
    if (area.x2 >= width)
      area.x2 = width - 1;
    if (area.y2 >= height)
      area.y2 = height - 1;

    // line_len can't exceed 1024, and minimum buffer size is 2048, so this won't overflow the buffer
    size_t line_len = lv_area_get_width(&area) * lv_area_get_height(&area) / 2;
    for (size_t i = 0; i != line_len; i++) {
      reinterpret_cast<uint32_t *>(this->draw_buf_)[i] = random_uint32();
    }
    this->draw_buffer_(&area, reinterpret_cast<lv_color_data *>(this->draw_buf_));
  }
}

/**
 * @class LvglComponent
 * @brief Component for rendering LVGL.
 *
 * This component renders LVGL widgets on a display. Some initialisation must be done in the constructor
 * since LVGL needs to be initialised before any widgets can be created.
 *
 * @param displays a list of displays to render onto. All displays must have the same
 *                 resolution.
 * @param buffer_frac the fraction of the display resolution to use for the LVGL
 *                    draw buffer. A higher value will make animations smoother but
 *                    also increase memory usage.
 * @param full_refresh if true, the display will be fully refreshed on every frame.
 *                     If false, only changed areas will be updated.
 * @param draw_rounding the rounding to use when drawing. A value of 1 will draw
 *                      without any rounding, a value of 2 will round to the nearest
 *                      multiple of 2, and so on.
 * @param resume_on_input if true, this component will resume rendering when the user
 *                         presses a key or clicks on the screen.
 * @param rotation_type What rotation type to use, if any
 */
LvglComponent::LvglComponent(std::vector<display::Display *> displays, float buffer_frac, bool full_refresh,
                             int draw_rounding, bool resume_on_input, bool update_when_display_idle,
                             RotationType rotation_type)
    : draw_rounding(draw_rounding),
      displays_(std::move(displays)),
      buffer_frac_(buffer_frac),
      full_refresh_(full_refresh),
      resume_on_input_(resume_on_input),
      update_when_display_idle_(update_when_display_idle),
      rotation_type_(rotation_type) {
  this->disp_ = lv_display_create(240, 240);
}

void LvglComponent::set_resolution_() const {
  int32_t width = this->width_;
  int32_t height = this->height_;
  if (this->rotation_ == display::DISPLAY_ROTATION_90_DEGREES ||
      this->rotation_ == display::DISPLAY_ROTATION_270_DEGREES) {
    std::swap(width, height);
  }
  ESP_LOGD(TAG, "Setting resolution to %u x %u (rotation %d)", (unsigned) width, (unsigned) height,
           (int) this->rotation_);
  if (this->rotation_type_ == RotationType::ROTATION_HARDWARE) {
    for (auto *display : this->displays_)
      display->set_rotation(this->rotation_);
  }
  lv_display_set_resolution(this->disp_, width, height);
}

void LvglComponent::update_orientation_() {
  // A square display is treated as landscape.
  auto orientation = this->get_width() >= this->get_height() ? Orientation::LANDSCAPE : Orientation::PORTRAIT;
  if (orientation == this->orientation_)
    return;
  this->orientation_ = orientation;
  auto *trigger = orientation == Orientation::LANDSCAPE ? this->landscape_callback_ : this->portrait_callback_;
  if (trigger != nullptr)
    trigger->trigger();
}

void LvglComponent::setup() {
  auto *display = this->displays_[0];
  auto rounding = this->draw_rounding;
  this->width_ = display->get_native_width();
  this->height_ = display->get_native_height();
  // cater for displays with dimensions that don't divide by the required rounding
  auto width = (this->width_ + rounding - 1) / rounding * rounding;
  auto height = (this->height_ + rounding - 1) / rounding * rounding;
  auto frac = this->buffer_frac_;
  if (frac == 0)
    frac = 1;
#if LV_COLOR_DEPTH == 32
  constexpr size_t LV_BYTES_PER_PIXEL = 3;  // RGB888 (not ARGB8888) on ESP32-P4
#else
  constexpr size_t LV_BYTES_PER_PIXEL = LV_COLOR_DEPTH / 8;
#endif
  auto buf_bytes = clamp_at_least(width * height / frac * LV_BYTES_PER_PIXEL, MIN_BUFFER_SIZE);
  void *buffer = nullptr;
  // for small buffers, try to allocate in internal memory first to improve performance
  if (this->buffer_frac_ >= MIN_BUFFER_FRAC / 2)
    buffer = lv_alloc_draw_buf(buf_bytes, true);  // NOLINT
  if (buffer == nullptr)
    buffer = lv_alloc_draw_buf(buf_bytes, false);  // NOLINT
  // if specific buffer size not set and can't get 100%, try for a smaller one
  if (buffer == nullptr && this->buffer_frac_ == 0) {
    frac = MIN_BUFFER_FRAC;
    buf_bytes /= MIN_BUFFER_FRAC;
    buffer = lv_alloc_draw_buf(buf_bytes, false);  // NOLINT
  }
  this->buffer_frac_ = frac;
  if (buffer == nullptr) {
    this->status_set_error(LOG_STR("Memory allocation failure"));
    this->mark_failed();
    return;
  }
  this->draw_buf_ = static_cast<uint8_t *>(buffer);
  this->set_resolution_();
#if LV_COLOR_DEPTH == 32
  lv_display_set_color_format(this->disp_, LV_COLOR_FORMAT_RGB888);  // 3 bytes/px, PPA-blit friendly
#else
  lv_display_set_color_format(this->disp_, LV_COLOR_FORMAT_RGB565);
#endif
  lv_display_set_flush_cb(this->disp_, static_flush_cb);
  lv_display_set_user_data(this->disp_, this);
  lv_display_add_event_cb(this->disp_, rounder_cb, LV_EVENT_INVALIDATE_AREA, this);
  lv_display_set_buffers(this->disp_, this->draw_buf_, nullptr, buf_bytes,
                         this->full_refresh_ ? LV_DISPLAY_RENDER_MODE_FULL : LV_DISPLAY_RENDER_MODE_PARTIAL);
  bool scanout = false;
#ifdef USE_ESP32
  scanout = this->setup_scanout_();
#endif
  if (this->rotation_type_ == ROTATION_SOFTWARE && !scanout) {
    this->rotate_buf_ = static_cast<lv_color_t *>(lv_alloc_draw_buf(buf_bytes, false));  // NOLINT
    if (this->rotate_buf_ == nullptr) {
      this->status_set_error(LOG_STR("Memory allocation failure"));
      this->mark_failed();
      return;
    }
  }
  if (this->rotation_type_ == ROTATION_SOFTWARE || scanout) {
#ifdef USE_ESP32_VARIANT_ESP32P4
    ppa_client_config_t ppa_config{};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    ppa_config.max_pending_trans_num = 1;
    if (ppa_register_client(&ppa_config, &this->ppa_client_) != ESP_OK) {
      ESP_LOGW(TAG, "PPA client registration failed, using software rotation");
      this->ppa_client_ = nullptr;
    }
#endif
  }
  if (this->draw_start_callback_ != nullptr) {
    lv_display_add_event_cb(this->disp_, render_start_cb, LV_EVENT_RENDER_START, this);
  }
  if (this->draw_end_callback_ != nullptr || this->update_when_display_idle_) {
    lv_display_add_event_cb(this->disp_, render_end_cb, LV_EVENT_REFR_READY, this);
    this->draw_end_event_registered_ = true;
  }
  this->refr_timer_ = lv_display_get_refr_timer(this->disp_);
  lv_timer_set_period(this->refr_timer_, this->refr_timer_period_);
#if LV_USE_LOG
  lv_log_register_print_cb([](lv_log_level_t level, const char *buf) {
    auto next = strchr(buf, ')');
    if (next != nullptr)
      buf = next + 1;
    while (isspace(*buf))
      buf++;
    if (level >= sizeof(LOG_LEVEL_MAP) / sizeof(LOG_LEVEL_MAP[0]))
      level = sizeof(LOG_LEVEL_MAP) / sizeof(LOG_LEVEL_MAP[0]) - 1;
    esp_log_printf_(LOG_LEVEL_MAP[level], TAG, 0, "%.*s", (int) strlen(buf) - 1, buf);
  });
#endif
  this->show_page(0, LV_SCREEN_LOAD_ANIM_NONE, 0);
  lv_display_trigger_activity(this->disp_);
  this->update_orientation_();
}

void LvglComponent::update() {
  // update indicators
  if (this->is_paused()) {
    return;
  }
  this->idle_callbacks_.call(lv_display_get_inactive_time(this->disp_));
}

void LvglComponent::loop() {
  if (this->paused_) {
#ifdef USE_LVGL_HID
    for (auto *keypad : this->hid_keypads_)
      keypad->run_paused();
#endif
    if (this->show_snow_)
      this->write_random_();
    return;
  }
  // Pause/resume the display's own refresh timer to track its busy state. While paused, LVGL
  // still keeps track of invalidated areas but won't render or flush them, so nothing needs to
  // be discarded or replayed: once resumed, the accumulated areas are simply drawn as normal.
  // Input events and other timers keep being processed below regardless of this state.
  bool hold = this->update_when_display_idle_ && this->displays_busy_();
#ifdef USE_ESP32
  // Nothing is drawn into a buffer still being scanned out.
  if (this->scanout_wait_) {
    if (this->displays_[0]->scanout_flipped() || millis() - this->scanout_presented_ >= SCANOUT_FLIP_TIMEOUT_MS) {
      this->scanout_wait_ = false;
    } else {
      hold = true;
    }
  }
#endif
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (this->cam_marker_ != nullptr) {
    this->cam_layer_service_();
    hold = hold || this->scanout_wait_;  // a picture just presented: LVGL draws after its flip
  }
#endif
  if (hold && !this->refr_timer_paused_) {
    this->refr_timer_paused_ = true;
    // calling lv_timer_pause() here would be ineffective; LVGL pauses and resumes the timer based on its own internal
    // state, which is not aware of the display's busy state. Instead, we extend the timer period to avoid it firing
    // while the display is busy.
    lv_timer_set_period(this->refr_timer_, 5 * 60 * 1000);
  } else if (!hold && this->refr_timer_paused_) {
    this->refr_timer_paused_ = false;
    lv_timer_set_period(this->refr_timer_, this->refr_timer_period_);
    // Don't wait for the timer's next natural period: refresh right away now that the
    // display is idle again.
    lv_timer_ready(this->refr_timer_);
  }
  lv_timer_handler();
}

#ifdef USE_LVGL_ANIMIMG
void lv_animimg_stop(lv_obj_t *obj) {
  int32_t duration = lv_animimg_get_duration(obj);
  lv_animimg_set_duration(obj, 0);
  lv_animimg_start(obj);
  lv_animimg_set_duration(obj, duration);
}
#endif
void LvglComponent::static_flush_cb(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p) {
  reinterpret_cast<LvglComponent *>(lv_display_get_user_data(disp_drv))->flush_cb_(disp_drv, area, color_p);
}

#ifdef USE_LVGL_SCALE
/**
 * Function to apply colors to ticks based on position
 * @param e The event data
 * @param color_start The color to apply to the first tick
 * @param color_end  The color to apply to the last tick
 * @param width
 */
void lv_scale_draw_event_cb(lv_event_t *e, int16_t range_start, int16_t range_end, lv_color_t color_start,
                            lv_color_t color_end, int width, bool local) {
  auto *scale = static_cast<lv_obj_t *>(lv_event_get_target(e));
  lv_draw_task_t *task = lv_event_get_draw_task(e);

  if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LINE) {
    auto *line_dsc = static_cast<lv_draw_line_dsc_t *>(lv_draw_task_get_draw_dsc(task));
    int tick = line_dsc->base.id2;
    if (tick >= range_start && tick <= range_end) {
      int ratio;
      if (local) {
        int range = range_end - range_start;
        tick -= range_start;
        ratio = range == 0 ? 0 : (tick * 255) / range;
      } else {
        // total tick count is guaranteed to be at least 2.
        ratio = (line_dsc->base.id1 * 255) / (lv_scale_get_total_tick_count(scale) - 1);
      }
      line_dsc->color = lv_color_mix(color_end, color_start, ratio);
      line_dsc->width += width;
    }
  }
}
#endif  // USE_LVGL_SCALE

#ifdef USE_LVGL_GRADIENT
/**
 *
 * @param dsc The gradient descriptor containing the color stops
 * @param pos The current position to calculate the color for
 * @return The color for the given position
 */

lv_color_t lv_grad_calculate_color(const lv_grad_dsc_t *dsc, int32_t pos) {
  if (dsc->stops_count == 0)
    return lv_color_black();
  if (dsc->stops_count == 1 || pos <= dsc->stops[0].frac)
    return dsc->stops[0].color;
  if (pos >= dsc->stops[dsc->stops_count - 1].frac)
    return dsc->stops[dsc->stops_count - 1].color;
  int i = 1;
  while (i < dsc->stops_count && dsc->stops[i].frac < pos)
    i++;
  auto *stop1 = &dsc->stops[i - 1];
  auto *stop2 = &dsc->stops[i];
  int32_t range = stop2->frac - stop1->frac;
  int32_t offset = pos - stop1->frac;
  return lv_color_mix(stop2->color, stop1->color, range == 0 ? 0 : (offset * 255) / range);
}
#endif  // USE_LVGL_GRADIENT

#ifdef USE_LVGL_MENU
bool lv_menu_back_button_is_root_click(lv_obj_t *menu, lv_event_t *e) {
  return lv_event_get_code(e) == LV_EVENT_CLICKED &&
         lv_menu_back_button_is_root(menu, static_cast<lv_obj_t *>(lv_event_get_target(e)));
}
#endif  // USE_LVGL_MENU

lv_point_t LvglComponent::get_touch_relative_to_obj(lv_obj_t *obj) {
  auto *indev = lv_indev_get_act();
  if (indev == nullptr) {
    return {INT32_MAX, INT32_MAX};
  }
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  point.x -= coords.x1;
  point.y -= coords.y1;
  return point;
}

static void lv_container_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj) {
  LV_TRACE_OBJ_CREATE("begin");
  lv_obj_set_clickable(obj, false);
  LV_UNUSED(class_p);
}

// Container class. Name is based on LVGL naming convention but upper case to keep ESPHome clang-tidy happy
const lv_obj_class_t LV_CONTAINER_CLASS = {
    .base_class = &lv_obj_class,
    .constructor_cb = lv_container_constructor,
    .name = "lv_container",
};

lv_obj_t *lv_container_create(lv_obj_t *parent) {
  lv_obj_t *obj = lv_obj_class_create_obj(&LV_CONTAINER_CLASS, parent);
  lv_obj_class_init_obj(obj);
  return obj;
}

#ifdef USE_LVGL_LIST
int lv_list_get_row_index(lv_obj_t *list, lv_obj_t *child) {
  for (lv_obj_t *obj = child; obj != nullptr; obj = lv_obj_get_parent(obj)) {
    if (lv_obj_get_parent(obj) == list)
      return lv_obj_get_index(obj);
  }
  ESP_LOGW(TAG, "lvgl.list: entry is not inside the list it was added to");
  return -1;
}

lv_obj_t *lv_list_get_row_for_remove(lv_obj_t *list, int index) {
  lv_obj_t *child = index < 0 ? nullptr : lv_obj_get_child(list, index);
  if (child == nullptr) {
    ESP_LOGW(TAG, "lvgl.list.remove: index %d is out of range, ignoring", index);
  }
  return child;
}
#endif  // USE_LVGL_LIST
}  // namespace esphome::lvgl

lv_result_t lv_mem_test_core() { return LV_RESULT_OK; }

void lv_mem_init() {}

void lv_mem_deinit() {}

#ifndef USE_ESP32
void *lv_malloc_core(size_t size) {
  auto *ptr = malloc(size);  // NOLINT
  if (ptr == nullptr) {
    ESP_LOGE(esphome::lvgl::TAG, "Failed to allocate %zu bytes", size);
  }
  return ptr;
}
void lv_free_core(void *ptr) { return free(ptr); }                            // NOLINT
void *lv_realloc_core(void *ptr, size_t size) { return realloc(ptr, size); }  // NOLINT

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { memset(mon_p, 0, sizeof(lv_mem_monitor_t)); }
static void *lv_alloc_draw_buf(size_t size, bool internal) {
  return malloc(size);  // NOLINT
}

#else
static unsigned cap_bits = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // NOLINT

static void *lv_alloc_draw_buf(size_t size, bool internal) {
  void *buffer;
  size = LV_ROUND_UP(size, LV_DRAW_BUF_ALIGN);
  buffer = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, size, internal ? MALLOC_CAP_8BIT : cap_bits);  // NOLINT
  if (buffer == nullptr) {
    ESP_LOGW(esphome::lvgl::TAG, "Failed to allocate %zu bytes for %sdraw buffer", size, internal ? "internal " : "");
  }
  return buffer;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  multi_heap_info_t heap_info;
  heap_caps_get_info(&heap_info, cap_bits);
  mon_p->total_size = heap_info.total_allocated_bytes + heap_info.total_free_bytes;
  mon_p->free_size = heap_info.total_free_bytes;
  mon_p->max_used = heap_info.total_allocated_bytes;
  mon_p->free_biggest_size = heap_info.largest_free_block;
  mon_p->used_cnt = heap_info.allocated_blocks;
  mon_p->free_cnt = heap_info.free_blocks;
  mon_p->used_pct = heap_info.allocated_blocks * 100 / (heap_info.allocated_blocks + heap_info.free_blocks);
  mon_p->frag_pct = 0;
}

void *lv_malloc_core(size_t size) {
  void *ptr;
  ptr = heap_caps_malloc(size, cap_bits);
  if (ptr == nullptr) {
    cap_bits = MALLOC_CAP_8BIT;
    ptr = heap_caps_malloc(size, cap_bits);
  }
  if (ptr == nullptr) {
    ESP_LOGE(esphome::lvgl::TAG, "Failed to allocate %zu bytes", size);
    return nullptr;
  }
  ESP_LOGV(esphome::lvgl::TAG, "allocate %zu - > %p", size, ptr);
  return ptr;
}

void lv_free_core(void *ptr) {
  ESP_LOGV(esphome::lvgl::TAG, "free %p", ptr);
  if (ptr == nullptr)
    return;
  heap_caps_free(ptr);
}

void *lv_realloc_core(void *ptr, size_t size) {
  ESP_LOGV(esphome::lvgl::TAG, "realloc %p: %zu", ptr, size);
  return heap_caps_realloc(ptr, size, cap_bits);
}
#endif
