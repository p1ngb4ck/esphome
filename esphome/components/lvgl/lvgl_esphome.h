#pragma once
#include "esphome/core/defines.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif  // USE_BINARY_SENSOR
#ifdef USE_IMAGE
#include "esphome/components/image/image.h"
#endif  // USE_IMAGE
#ifdef USE_LVGL_ROTARY_ENCODER
#include "esphome/components/rotary_encoder/rotary_encoder.h"
#endif  // USE_LVGL_ROTARY_ENCODER

// required for clang-tidy
#ifndef LV_CONF_H
#define LV_CONF_SKIP 1  // NOLINT
#endif                  // LV_CONF_H

#include "esphome/components/display/display.h"
#include "esphome/components/display/display_color_utils.h"
#include "esphome/core/component.h"

#include <atomic>
#include <list>
#include <lvgl.h>
#include <map>
#include <utility>
#include <string>
#include <vector>

#ifdef USE_ESP32_VARIANT_ESP32P4
#include "driver/ppa.h"
#endif

#ifdef USE_FONT
#include "esphome/components/font/font.h"
#endif  // USE_FONT
#ifdef USE_TOUCHSCREEN
#include "esphome/components/touchscreen/touchscreen.h"
#endif  // USE_TOUCHSCREEN

#if defined(USE_LVGL_BUTTONMATRIX) || defined(USE_LVGL_KEYBOARD)
#include "esphome/components/key_provider/key_provider.h"
#endif  // USE_LVGL_BUTTONMATRIX
#ifdef USE_LVGL_HID
#include "esphome/components/hid/hid_input.h"
#include "key_bindings.h"
#endif  // USE_LVGL_HID

namespace esphome::lvgl {

#if LV_COLOR_DEPTH == 16
using lv_color_data = uint16_t;
#endif
#if LV_COLOR_DEPTH == 32
using lv_color_data = uint32_t;
#endif

extern lv_event_code_t lv_update_event;  // NOLINT
extern std::string lv_event_code_name_for(lv_event_t *event);

lv_obj_t *lv_container_create(lv_obj_t *parent);
#ifdef USE_LVGL_SCALE
void lv_scale_draw_event_cb(lv_event_t *e, int16_t range_start, int16_t range_end, lv_color_t color_start,
                            lv_color_t color_end, int width, bool local);
#endif
#ifdef USE_LVGL_TABLE
uint32_t lv_table_get_selected_row(lv_obj_t *obj);
uint32_t lv_table_get_selected_column(lv_obj_t *obj);
#endif
#if LV_COLOR_DEPTH == 16
static const display::ColorBitness LV_BITNESS = display::ColorBitness::COLOR_BITNESS_565;
#elif LV_COLOR_DEPTH == 32
static const display::ColorBitness LV_BITNESS = display::ColorBitness::COLOR_BITNESS_888;
#else   // LV_COLOR_DEPTH
static const display::ColorBitness LV_BITNESS = display::ColorBitness::COLOR_BITNESS_332;
#endif  // LV_COLOR_DEPTH

#if defined(USE_FONT) && defined(USE_LVGL_FONT)
inline void lv_obj_set_style_text_font(lv_obj_t *obj, const font::Font *font, lv_style_selector_t part) {
  lv_obj_set_style_text_font(obj, font->get_lv_font(), part);
}
inline void lv_style_set_text_font(lv_style_t *style, const font::Font *font) {
  lv_style_set_text_font(style, font->get_lv_font());
}
#endif

#ifdef USE_IMAGE
#ifdef USE_LVGL_IMAGE
// Shortcut / overload, so that the source of an image widget can easily be updated from within a lambda.
inline void lv_image_set_src(lv_obj_t *obj, image::Image *image) {
  ::lv_image_set_src(obj, image->get_lv_image_dsc());
  image->add_lvgl_widget(obj);
}

inline void lv_obj_set_style_bitmap_mask_src(lv_obj_t *obj, image::Image *image, lv_style_selector_t selector) {
  ::lv_obj_set_style_bitmap_mask_src(obj, image->get_lv_image_dsc(), selector);
}

inline void lv_obj_set_style_bg_image_src(lv_obj_t *obj, image::Image *image, lv_style_selector_t selector) {
  ::lv_obj_set_style_bg_image_src(obj, image->get_lv_image_dsc(), selector);
}
inline void lv_style_set_bg_image_src(lv_style_t *style, image::Image *image) {
  ::lv_style_set_bg_image_src(style, image->get_lv_image_dsc());
}
inline void lv_style_set_bitmap_mask_src(lv_style_t *style, image::Image *image) {
  ::lv_style_set_bitmap_mask_src(style, image->get_lv_image_dsc());
}
#endif

#ifdef USE_LVGL_ANIMIMG
inline void lv_animimg_set_src(lv_obj_t *img, std::vector<image::Image *> images) {
  auto *dsc = static_cast<std::vector<lv_image_dsc_t *> *>(lv_obj_get_user_data(img));
  if (dsc == nullptr) {
    // object will be lazily allocated but never freed.
    dsc = new std::vector<lv_image_dsc_t *>(images.size());  // NOLINT
    lv_obj_set_user_data(img, dsc);
  }
  dsc->clear();
  for (auto &image : images) {
    dsc->push_back(image->get_lv_image_dsc());
  }
  lv_animimg_set_src(img, (const void **) dsc->data(), dsc->size());
}
#endif  // USE_LVGL_ANIMIMG
#endif  // USE_IMAGE

#ifdef USE_LVGL_METER
int16_t lv_get_needle_angle_for_value(lv_obj_t *obj, int32_t value);
#endif

#ifdef USE_LVGL_LIST
// Returns the index, within `list`, of the entry that contains `child`: `child` itself if it's a
// direct child of `list`, or the ancestor of `child` that is, when `child` is nested inside a
// widget hierarchy added via `lvgl.list.add`. Returns -1 if `child` isn't inside `list` at all.
int lv_list_get_row_index(lv_obj_t *list, lv_obj_t *child);

// Returns the entry at `index` within `list`, or nullptr (logging why) if `index` is out of
// range -- shared by every `lvgl.list.remove` call site, since a templatable index can go out of
// range at runtime in ways config validation can't catch (e.g. driven by a sensor value).
lv_obj_t *lv_list_get_row_for_remove(lv_obj_t *list, int index);
#endif

#ifdef USE_LVGL_GRADIENT
/**
 *
 * @param dsc The gradient descriptor containing the color stops
 * @param pos The current position to calculate the color for
 * @return The color for the given position
 */

lv_color_t lv_grad_calculate_color(const lv_grad_dsc_t *dsc, int32_t pos);
#endif  // USE_LVGL_GRADIENT

#ifdef USE_LVGL_MENU
// True if `e` is a click on `menu`'s back button while already at the root page -- the one
// case LVGL's own docs say the app must handle itself (there is no default behavior).
bool lv_menu_back_button_is_root_click(lv_obj_t *menu, lv_event_t *e);
#endif  // USE_LVGL_MENU

// Parent class for things that wrap an LVGL object
class LvCompound {
 public:
  virtual ~LvCompound() = default;
  virtual void set_obj(lv_obj_t *lv_obj) { this->obj = lv_obj; }
  lv_obj_t *obj{};
};

// Frees a heap-allocated LvCompound wrapper on LV_EVENT_DELETE, since lv_obj_del() only knows how to destroy LVGL's own
// object tree, not a separate C++ object paired with one of its nodes.
template<typename T> void delete_lv_compound_on_delete(lv_event_t *e) {
  delete static_cast<T *>(lv_event_get_user_data(e));
}

class LvglComponent;
#ifdef USE_LVGL_HID
class LVHIDKeypadListener;
class LVHIDPointerListener;
#endif

class LvPageType : public Parented<LvglComponent> {
 public:
  LvPageType(bool skip) : skip(skip) {}

  void setup(size_t index) {
    this->index = index;
    this->obj = lv_obj_create(nullptr);
  }

  bool is_showing() const;

  lv_obj_t *obj{};
  size_t index{};
  bool skip;
};

using event_callback_t = void(lv_event_t *);

class LvLambdaComponent final : public Component {
 public:
  LvLambdaComponent(void (*callback)()) : callback_(callback) {}

  void setup() override { this->callback_(); }
  // execute after the LvglComponent is setup
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 5; }

 protected:
  void (*callback_)();
};

template<typename... Ts> class ObjUpdateAction final : public Action<Ts...> {
 public:
  explicit ObjUpdateAction(std::function<void(Ts...)> &&lamb) : lamb_(std::move(lamb)) {}

 protected:
  void play(const Ts &...x) override { this->lamb_(x...); }

  std::function<void(Ts...)> lamb_;
};
#ifdef USE_LVGL_ANIMIMG
void lv_animimg_stop(lv_obj_t *obj);
#endif  // USE_LVGL_ANIMIMG
enum RotationType : uint8_t {
  ROTATION_UNUSED,
  ROTATION_SOFTWARE,
  ROTATION_HARDWARE,
};

enum class Orientation : uint8_t {
  UNKNOWN,
  LANDSCAPE,
  PORTRAIT,
};

class LvglComponent final : public PollingComponent {
  constexpr static const char *const TAG = "lvgl";  // NOLINT: lvgl does not build for ESP8266

 public:
  LvglComponent(std::vector<display::Display *> displays, float buffer_frac, bool full_refresh, int draw_rounding,
                bool resume_on_input, bool update_when_display_idle, RotationType rotation_type);
  static void static_flush_cb(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p);
  /**
   *
   * @param obj A widget
   * @return The position of the last indev point relative to the widget's origin.
   */
  static lv_point_t get_touch_relative_to_obj(lv_obj_t *obj);

  float get_setup_priority() const override { return setup_priority::PROCESSOR; }
  void setup() override;
  void update() override;
  void loop() override;
  template<typename F> void add_on_idle_callback(F &&callback) { this->idle_callbacks_.add(std::forward<F>(callback)); }
  void add_on_draw_end_callback(std::function<void()> &&callback) {
    this->draw_end_callbacks_.add(std::move(callback));
    // setup() only wires up the LV_EVENT_REFR_READY listener if draw_end_callback_ (YAML
    // on_draw_end:) or update_when_display_idle_ was already set at that point -- a lower-
    // priority consumer calling this after setup() has already run needs to register it itself.
    // Guarded so it's wired exactly once regardless of which path gets there first.
    if (!this->draw_end_event_registered_) {
      lv_display_add_event_cb(this->disp_, render_end_cb, LV_EVENT_REFR_READY, this);
      this->draw_end_event_registered_ = true;
    }
  }

  static void render_end_cb(lv_event_t *event);
  static void render_start_cb(lv_event_t *event);
  void dump_config() override;
  lv_display_t *get_disp() { return this->disp_; }
  lv_obj_t *get_screen_active() { return lv_display_get_screen_active(this->disp_); }
  // Pause or resume the display.
  // @param paused If true, pause the display. If false, resume the display.
  // @param show_snow If true, show the snow effect when paused.
  void set_paused(bool paused, bool show_snow);
  void set_refresh_interval(uint32_t period) {
    this->refr_timer_period_ = period;
    if (this->refr_timer_ != nullptr)
      lv_timer_set_period(this->refr_timer_, period);
  }

  // Returns true if the display has been explicitly paused via set_paused().
  bool is_paused() const { return this->paused_; }
#ifdef USE_LVGL_HID
  // Keypads whose keys still run their actions while the display is paused.
  void add_hid_keypad(LVHIDKeypadListener *keypad) { this->hid_keypads_.push_back(keypad); }
  /// Takes keys, typed characters and pointer motion while the display is paused, for what shows on
  /// it instead. Keys and characters come from the main loop, pointer motion from wherever its source
  /// reports it. Each returns whether it was taken: navigation keys taken run no action here.
  class PausedInput {
   public:
    /// action: the key's KeyBindings action, -1 for none. codepoint: what the keyboard's layout puts on
    /// the key at its level now (hid::HIDInput::layout_char(): bit 31 a dead key), 0 for none; level3:
    /// Right Alt is the layout's AltGr.
    virtual bool paused_key(uint16_t page, uint16_t usage, uint8_t mods, bool pressed, int action,
                            uint32_t codepoint, bool level3, uint32_t source) = 0;
    virtual bool paused_text(uint32_t codepoint) = 0;
    virtual bool paused_pointer(int16_t dx, int16_t dy, int16_t wheel, uint8_t buttons) = 0;
  };
  void set_paused_input(PausedInput *input) { this->paused_input_ = input; }
  PausedInput *get_paused_input() const { return this->paused_input_; }
  class KeyTap {
   public:
    virtual bool key_tap(uint32_t source, const char *label, uint16_t page, uint16_t usage, bool pressed) = 0;
  };
  void set_key_tap(KeyTap *tap) { this->key_tap_.store(tap, std::memory_order_release); }
  KeyTap *get_key_tap() const { return this->key_tap_.load(std::memory_order_acquire); }
  /// The pointer the keypads' direction keys and Enter move and click in its key mode (the first
  /// hid_pointers entry).
  void set_key_pointer(LVHIDPointerListener *pointer) { this->key_pointer_ = pointer; }
  LVHIDPointerListener *get_key_pointer() const { return this->key_pointer_; }
#endif
  // If the display is paused and we have resume_on_input_ set to true, resume the display.
  void maybe_wakeup() {
    if (this->paused_ && this->resume_on_input_) {
      this->set_paused(false, false);
    }
  }

  /**
   * Initialize the LVGL library and register custom events.
   */
  static void esphome_lvgl_init();

  //  Convenience overloads for adding a callback for one or more events
  static void add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event, void *user_data = nullptr);
  static void add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1, lv_event_code_t event2,
                           void *user_data = nullptr);
  static void add_event_cb(lv_obj_t *obj, event_callback_t callback, lv_event_code_t event1, lv_event_code_t event2,
                           lv_event_code_t event3, void *user_data = nullptr);

  // Set or clear LV_OBJ_FLAG_LAYOUT_2, LV_OBJ_FLAG_WIDGET_1 or LV_OBJ_FLAG_WIDGET_2, which have no setter in LVGL 9.6
  static void lv_obj_set_custom_flag(lv_obj_t *obj, lv_obj_flag_t flag, bool value);

  // change the state of a widget and fire an event if changed (only needed for CHECKED)

  static void lv_obj_set_state_value(lv_obj_t *obj, lv_state_t state, bool value) {
    if (value != lv_obj_has_state(obj, state)) {
      if (value) {
        lv_obj_add_state(obj, state);
      } else {
        lv_obj_remove_state(obj, state);
      }
      if (state == LV_STATE_CHECKED)
        lv_obj_send_event(obj, lv_update_event, nullptr);
    }
  }

  // change the state of a buttonmatrix button and fire an event if changed (only needed for CHECKED)
#ifdef USE_LVGL_BUTTONMATRIX
  static void lv_buttonmatrix_set_button_ctrl_value(lv_obj_t *obj, uint32_t index, lv_buttonmatrix_ctrl_t ctrl,
                                                    bool value) {
    if (value != lv_buttonmatrix_has_button_ctrl(obj, index, ctrl)) {
      if (value) {
        lv_buttonmatrix_set_button_ctrl(obj, index, ctrl);
      } else {
        lv_buttonmatrix_clear_button_ctrl(obj, index, ctrl);
      }
      if (ctrl == LV_BUTTONMATRIX_CTRL_CHECKED)
        lv_obj_send_event(obj, lv_update_event, nullptr);
    }
  }
#endif

  void add_page(LvPageType *page);
  void show_page(size_t index, lv_screen_load_anim_t anim, uint32_t time);
  void show_next_page(lv_screen_load_anim_t anim, uint32_t time);
  void show_prev_page(lv_screen_load_anim_t anim, uint32_t time);
  void set_page_wrap(bool wrap) { this->page_wrap_ = wrap; }
  void set_big_endian(bool big_endian) { this->big_endian_ = big_endian; }
  size_t get_current_page() const;
  void set_focus_mark(lv_group_t *group) { this->focus_marks_[group] = lv_group_get_focused(group); }
  void restore_focus_mark(lv_group_t *group) {
    auto *mark = this->focus_marks_[group];
    if (mark != nullptr) {
      lv_group_focus_obj(mark);
    }
  }
  // rounding factor to align bounds of update area when drawing
  size_t draw_rounding{2};

  void set_pause_trigger(Trigger<> *trigger) { this->pause_callback_ = trigger; }
  void set_resume_trigger(Trigger<> *trigger) { this->resume_callback_ = trigger; }
  void set_draw_start_trigger(Trigger<> *trigger) { this->draw_start_callback_ = trigger; }
  void set_draw_end_trigger(Trigger<> *trigger) { this->draw_end_callback_ = trigger; }
  void set_landscape_trigger(Trigger<> *trigger) { this->landscape_callback_ = trigger; }
  void set_portrait_trigger(Trigger<> *trigger) { this->portrait_callback_ = trigger; }
  void set_rotation(display::DisplayRotation rotation);
  /// Set the rotation from an angle in degrees. Must be a multiple of 90.
  void set_rotation(int angle);
  display::DisplayRotation get_rotation() const { return this->rotation_; }
  void rotate_coordinates(int32_t &x, int32_t &y) const;

  uint16_t get_width() const { return lv_display_get_horizontal_resolution(this->disp_); }
  uint16_t get_height() const { return lv_display_get_vertical_resolution(this->disp_); }

#ifdef USE_ESP32
  /// Gets what LVGL draws as the LVGL screen shows it (its size, orientation and colour format): every
  /// area rendered while it is set goes into the buffer capture_target() gives, and capture_done()
  /// follows once a refresh is complete. All three on the LVGL thread (the main loop).
  class FrameCapture {
   public:
    /// The buffer this area goes into: capture_bytes() long, cache-line aligned, packed rows of the
    /// LVGL screen's width. nullptr: this area is not wanted.
    virtual uint8_t *capture_target() = 0;
    /// The area (clipped to the screen) is in the buffer now.
    virtual void capture_area(const lv_area_t &area) = 0;
    virtual void capture_done() = 0;
  };
  void set_frame_capture(FrameCapture *capture) { this->frame_capture_ = capture; }
  /// Bytes per pixel of the LVGL screen (2 RGB565, 3 RGB888) and the size of a buffer for its picture
  /// (whole cache lines).
  static size_t capture_bpp();
  size_t capture_bytes() const;
  /// LVGL thread, not paused: the picture shown (the display's frame buffer flipped in last) into dst
  /// (capture_bytes(), cache-line aligned), turned back into the LVGL screen's orientation. False when
  /// LVGL does not draw into the display's frame buffers itself.
  bool copy_shown(uint8_t *dst);
#endif
#if defined(USE_ESP32_VARIANT_ESP32P4)
  /// A camera's picture straight into the display's frame buffers (scanout with the PPA only), not drawn by LVGL:
  /// each frame copied 1:1 in the panel's own orientation (the camera mounted to match; never scaled or turned:
  /// only a YUV frame is converted) into the marker object's rectangle, centred and cut to it, then presented. Only while LVGL runs, the marker shows and nothing of the top layer lies over it. Main loop.
  /// False: no scanout or no PPA.
  bool camera_layer_start(lv_obj_t *marker);
  /// The layer ends: a pending frame is dropped (its done() called), the marker drawn by LVGL again.
  void camera_layer_stop();
  /// A frame: a pic_w x pic_h picture (colour mode cm, a ppa_srm_color_mode_t), its top-left w x h block shown.
  /// done() once the frame is not read any more (drawn, dropped, or replaced by a newer one). False: not taken (no
  /// layer, LVGL paused, the marker not shown or covered), done() is not called.
  bool camera_layer_frame(const void *data, uint16_t pic_w, uint16_t pic_h, uint16_t w, uint16_t h,
                          ppa_srm_color_mode_t cm, std::function<void()> &&done);
  /// The pending frame dropped now (its done() called): its buffer goes away.
  void camera_layer_drop();
  bool camera_layer_running() const { return this->cam_marker_ != nullptr; }
#endif

 protected:
  void set_resolution_() const;
  // Determine the current orientation from the effective resolution and fire the
  // landscape/portrait trigger if it has changed since the last check.
  void update_orientation_();
  void draw_end_();
  // Not checking for non-null callback since the
  // LVGL callback that calls it is not set in that case
  void draw_start_() const { this->draw_start_callback_->trigger(); }
  // Returns true if update_when_display_idle is enabled and at least one underlying display
  // component is currently busy (e.g. mid-refresh).
  bool displays_busy_() const;

  void write_random_();
  void draw_buffer_(const lv_area_t *area, lv_color_data *ptr);
#ifdef USE_ESP32_VARIANT_ESP32P4
  bool ppa_rotate_(const lv_color_data *src, lv_color_data *dst, uint16_t width, uint16_t height,
                   uint32_t height_rounded);
#endif
#ifdef USE_ESP32
  // A display scanning out of two frame buffers: LVGL draws into the one not shown and flips it in
  // once a refresh is complete, so a frame is never seen half drawn.
  bool setup_scanout_();
  bool scanout_clip_(const lv_area_t *area, lv_area_t *clipped) const;
  void scanout_rect_(const lv_area_t *area, int32_t &x, int32_t &y, int32_t &w, int32_t &h) const;
  void draw_scanout_(const lv_area_t *area, const uint8_t *src, uint8_t *fb);
  void sync_scanout_(const lv_area_t *area);
  void wait_scanout_flip_();
  static void scanout_sync_cb(lv_display_t *disp, const lv_area_t *area);
#ifdef USE_ESP32_VARIANT_ESP32P4
  bool ppa_blit_(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint16_t sx, uint16_t sy, uint16_t w, uint16_t h,
                 uint8_t *dst, uint16_t dx, uint16_t dy, ppa_srm_rotation_angle_t angle);
  /// The same into a picture of dst_w x dst_h (dst_size bytes).
  bool ppa_blit_to_(const uint8_t *src, uint16_t src_w, uint16_t src_h, uint16_t sx, uint16_t sy, uint16_t w,
                    uint16_t h, uint8_t *dst, uint16_t dst_w, uint16_t dst_h, size_t dst_size, uint16_t dx, uint16_t dy,
                    ppa_srm_rotation_angle_t angle);
#endif
#endif
  void flush_cb_(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *color_p);

  std::vector<display::Display *> displays_{};
  size_t buffer_frac_{1};
  bool full_refresh_{};
  bool resume_on_input_{};
  bool update_when_display_idle_{};

  uint8_t *draw_buf_{};
  lv_display_t *disp_{};
  // The display's own periodic refresh timer, effectively paused while the display is busy (see
  // displays_busy_()) so LVGL neither renders nor flushes to it, without losing track of
  // invalidated areas. Other timers (indev reading, animations, ...) keep running as normal.
  lv_timer_t *refr_timer_{};
  // Tracks whether refr_timer_ is currently paused, so loop() can detect the busy -> idle edge
  // and kick off an immediate refresh instead of waiting for the timer's next natural period.
  bool refr_timer_paused_{};
  uint32_t refr_timer_period_{16};
  uint16_t width_{};
  uint16_t height_{};
  bool paused_{};
#ifdef USE_LVGL_HID
  std::vector<LVHIDKeypadListener *> hid_keypads_{};
  PausedInput *paused_input_{nullptr};
  std::atomic<KeyTap *> key_tap_{nullptr};
  LVHIDPointerListener *key_pointer_{nullptr};
#endif
  std::vector<LvPageType *> pages_{};
  size_t current_page_{0};
  bool show_snow_{};
  bool page_wrap_{true};
  bool big_endian_{};
  std::map<lv_group_t *, lv_obj_t *> focus_marks_{};

  CallbackManager<void(uint32_t)> idle_callbacks_{};
  CallbackManager<void()> draw_end_callbacks_{};
  bool draw_end_event_registered_{false};
  Trigger<> *pause_callback_{};
  Trigger<> *resume_callback_{};
  Trigger<> *draw_start_callback_{};
  Trigger<> *draw_end_callback_{};
  Trigger<> *landscape_callback_{};
  Trigger<> *portrait_callback_{};
  Orientation orientation_{Orientation::UNKNOWN};
  void *rotate_buf_{};
  display::DisplayRotation rotation_{display::DISPLAY_ROTATION_0_DEGREES};
  RotationType rotation_type_;
#ifdef USE_ESP32_VARIANT_ESP32P4
  ppa_client_handle_t ppa_client_{};
  bool scanout_ppa_{};
#endif
#ifdef USE_ESP32
  uint8_t *scanout_[2]{};
  size_t scanout_stride_{};
  uint8_t scanout_back_{1};
  bool scanout_wait_{};
  uint32_t scanout_presented_{};
  FrameCapture *frame_capture_{nullptr};
  void capture_area_(const lv_area_t *area, const uint8_t *src);
#endif
#if defined(USE_ESP32_VARIANT_ESP32P4)
  bool cam_layer_shown_() const;
  void cam_layer_service_();
  void cam_layer_draw_();
  void cam_layer_reset_();
  void cam_layer_restore_();
  void panel_to_area_(int32_t x, int32_t y, int32_t w, int32_t h, lv_area_t *area) const;
  struct CamFrame {
    const void *data;
    uint16_t pic_w, pic_h, w, h;
    ppa_srm_color_mode_t cm;
  };
  lv_obj_t *cam_marker_{nullptr};
  CamFrame cam_frame_{};
  std::function<void()> cam_done_{};
  bool cam_pending_{false};
  bool cam_shown_{false};
  uint32_t cam_seq_[2]{};  // the picture each frame buffer holds, 0: none
  uint32_t cam_latest_{0};
  lv_area_t cam_box_{};  // the marker's area at the last frame (LVGL coordinates)
  lv_area_t cam_pic_{0, 0, -1, -1};  // the newest picture's area, empty: none
  uint32_t cam_frames_{0};      // pictures presented since cam_rate_since_ (the log's rate line)
  uint32_t cam_rate_since_{0};
#endif
};

class IdleTrigger final : public Trigger<> {
 public:
  explicit IdleTrigger(LvglComponent *parent, TemplatableFn<uint32_t> timeout);

 protected:
  TemplatableFn<uint32_t> timeout_;
  bool is_idle_{};
};

template<typename... Ts> class LvglAction final : public Action<Ts...>, public Parented<LvglComponent> {
 public:
  explicit LvglAction(std::function<void(LvglComponent *)> &&lamb) : action_(std::move(lamb)) {}

 protected:
  void play(const Ts &...x) override { this->action_(this->parent_); }
  std::function<void(LvglComponent *)> action_{};
};

template<typename Tc, typename... Ts> class LvglCondition final : public Condition<Ts...>, public Parented<Tc> {
 public:
  LvglCondition(std::function<bool(Tc *)> &&condition_lambda) : condition_lambda_(std::move(condition_lambda)) {}
  bool check(const Ts &...x) override { return this->condition_lambda_(this->parent_); }

 protected:
  std::function<bool(Tc *)> condition_lambda_{};
};

#ifdef USE_LVGL_TOUCHSCREEN
class LVTouchListener final : public touchscreen::TouchListener, public Parented<LvglComponent> {
 public:
  LVTouchListener(uint16_t long_press_time, uint16_t long_press_repeat_time, LvglComponent *parent);
  void update(const touchscreen::TouchPoints_t &tpoints) override;
  void release() override {
    touch_pressed_ = false;
    this->parent_->maybe_wakeup();
  }
  lv_indev_t *get_drv() { return this->drv_; }

 protected:
  lv_indev_t *drv_{};
  touchscreen::TouchPoint touch_point_{};
  bool touch_pressed_{};
};
#endif  // USE_LVGL_TOUCHSCREEN

#ifdef USE_LVGL_METER

class IndicatorLine : public LvCompound {
 public:
  IndicatorLine() = default;

  void set_obj(lv_obj_t *lv_obj) override;

  void set_value(int value);

 private:
  void update_length_();

  int16_t angle_{};
  lv_point_precise_t points_[2]{};
};
#endif

#ifdef USE_LVGL_KEY_LISTENER
class LVEncoderListener final : public Parented<LvglComponent> {
 public:
  LVEncoderListener(lv_indev_type_t type, uint16_t long_press_time, uint16_t long_press_repeat_time);

#ifdef USE_BINARY_SENSOR
  void add_button(binary_sensor::BinarySensor *button, lv_key_t key) {
    button->add_on_state_callback([this, key](bool state) { this->event(key, state); });
  }
#endif

#ifdef USE_LVGL_ROTARY_ENCODER
  void set_sensor(rotary_encoder::RotaryEncoderSensor *sensor) {
    sensor->register_listener([this](int32_t count) { this->set_count(count); });
  }
#endif  // USE_LVGL_ROTARY_ENCODER

  void event(int key, bool pressed) {
    if (!this->parent_->is_paused()) {
      this->pressed_ = pressed;
      this->key_ = key;
    } else if (!pressed) {
      // maybe wakeup on release if paused
      this->parent_->maybe_wakeup();
    }
  }

  void set_count(int32_t count) {
    if (!this->parent_->is_paused()) {
      this->count_ = count;
    } else {
      this->parent_->maybe_wakeup();
    }
  }

  lv_indev_t *get_drv() { return this->drv_; }

 protected:
  lv_indev_t *drv_{};
  bool pressed_{};
  int32_t count_{};
  int32_t last_count_{};
  int key_{};
};
#endif  //  USE_LVGL_KEY_LISTENER

#ifdef USE_LVGL_HID
// A HID keyboard or remote as an LVGL keypad. Its keys do what the key bindings say: the arrows
// move the focus to the nearest widget that way (or work the focused widget that uses them), Enter
// presses, Back is Escape, and an action of the configuration runs its automation. Typed text goes
// to a focused text area. Key changes are queued, so a press and release between two reads are kept.
class LVHIDKeypadListener final : public Parented<LvglComponent> {
 public:
  /// input nullptr: keys and text come only by push_action() / push_text() (remote input).
  LVHIDKeypadListener(hid::HIDInput *input, KeyBindings *bindings, uint16_t long_press_time,
                      uint16_t long_press_repeat_time);
  lv_indev_t *get_drv() { return this->drv_; }
  /// A key_bindings action (its index) as if one of its keys was pressed and let go; false when the queue is full.
  bool push_action(int action);
  /// Characters for a focused text area (UTF-8); Enter and Tab are left out, as typed.
  void push_text(const std::string &text);
  /// While the display is paused (main loop): keys and characters go to the paused input, if any; the
  /// keys run their actions' own automations (navigation only when not taken), and keys let go are let go.
  void run_paused();

 protected:
  enum EventKind : uint8_t { KEY_PRESS, KEY_RELEASE, TEXT, ACTION };
  struct Event {
    uint32_t code;         // page << 16 | usage, or a character
    uint32_t layout_char;  // a key's: hid::HIDInput::layout_char()
    uint8_t mods;
    EventKind kind;
    bool level3;
    uint32_t source;
  };
  struct Held {
    uint32_t code;
    uint32_t lv_key;
  };
  static constexpr uint8_t QUEUE_SIZE = 32;
  static constexpr uint8_t HELD_SIZE = 4;

  /// False when the queue is full (or the text has no place while paused).
  bool push_(uint32_t code, uint8_t mods, EventKind kind, uint32_t layout_char = 0, bool level3 = false,
             uint32_t source = 0);
  void read_(lv_indev_data_t *data);
  /// The LVGL key a key press becomes, 0 when it is done here (or nothing).
  uint32_t press_(uint16_t page, uint16_t usage, uint8_t mods);
  /// The keys held, for combos (KeyBindings::resolve_combo()).
  void track_(uint32_t code, bool pressed);
  uint32_t builtin_(int action, lv_obj_t *focused);
  lv_obj_t *focused_() const;
  /// A full-screen widget shown on the top layer (a popup's backdrop): only its widgets are reached.
  lv_obj_t *modal_() const;
  bool reachable_(lv_obj_t *obj, lv_obj_t *modal) const;
  /// To the nearest reachable widget that way (LV_KEY_UP..LEFT), or the first one when from is null.
  void move_focus_(uint32_t direction, lv_obj_t *from, lv_obj_t *modal);
  /// To the next or previous reachable widget in the group's order.
  void step_focus_(bool forward, lv_obj_t *from, lv_obj_t *modal);
  /// Scrolls the list around the focused widget by a page and focuses what is then in its place.
  void scroll_page_(bool down, lv_obj_t *from, lv_obj_t *modal);

  KeyBindings *bindings_;
  Event queue_[QUEUE_SIZE]{};
  std::atomic<uint8_t> head_{0};  // read by read_()
  std::atomic<uint8_t> tail_{0};  // written by push_()
  Held held_[HELD_SIZE]{};
  static constexpr uint8_t DOWN_SIZE = 8;
  uint32_t down_[DOWN_SIZE]{};
  uint8_t down_count_{0};
  uint32_t key_{0};
  bool pressed_{false};
  bool release_next_{false};
  lv_indev_t *drv_{};
};

// A HID mouse as an LVGL pointer with a cursor: motion moves it, the left button presses,
// the wheel scrolls whatever is under it.
class LVHIDPointerListener final : public Parented<LvglComponent> {
 public:
  LVHIDPointerListener(hid::HIDInput *input, uint16_t long_press_time, uint16_t long_press_repeat_time,
                       LvglComponent *parent);
#ifdef USE_LVGL_IMAGE
  void set_cursor_image(const void *src);
#endif
  void set_default_cursor();
  // The cursor hides after this long without motion (0: never) and shows again with the next.
  void set_hide_after(uint32_t ms);
  /// Motion times this (1: as the device reports it).
  void set_speed(float speed) { this->speed_ = speed; }
  /// Fast moves go further: a report's motion is multiplied more the larger it is (up to 3 times).
  void set_acceleration(bool on) { this->acceleration_ = on; }
  /// A wheel step scrolls this many lines (WHEEL_LINE each).
  void set_wheel_lines(uint8_t lines) { this->wheel_lines_ = lines; }
  void set_reverse_wheel(bool reverse) { this->reverse_wheel_ = reverse; }
  /// The built-in cursor's look (not a configured cursor image): style 0 dot, 1 ring, 2 arrow;
  /// size 0 small, 1 medium, 2 large. Drawn by LVGL itself: no image, no buffer.
  void set_cursor_look(uint8_t style, uint8_t size);
  /// Left-handed: the right button presses, the left one is the second button.
  void set_left_handed(bool on) { this->left_handed_ = on; }
  /// How long a press is held before it is a long press.
  void set_long_press_time(uint16_t ms) { lv_indev_set_long_press_time(this->drv_, ms); }
  /// The other buttons, pressed (true) and let go: 2 the second of left / right, 3 the middle one,
  /// 4 and 5 the side buttons. Called where the HID input reports (main loop).
  template<typename F> void add_on_button_callback(F &&cb) { this->button_cb_.add(std::forward<F>(cb)); }
  /// A long press on what is under the pointer (the clickable object or its clickable parent).
  void long_press_here();
  /// Key mode (remote controls): the keypads' direction keys move the pointer, Enter clicks.
  void set_key_mode(bool on);
  bool is_key_mode() const { return this->key_mode_; }
  /// Key mode: a step is this many pixels, every read while held.
  void set_key_step(uint8_t pixels) { this->key_step_ = pixels; }
  /// Key mode: held this long, the steps get faster, up to this many times after 2 s held.
  void set_key_hold(uint32_t after_ms, uint8_t most) {
    this->key_after_ms_ = after_ms;
    this->key_most_ = most;
  }
  /// A gamepad's touchpad as this pointer, while on; not while the capture widget shows (a page
  /// testing the gamepad). One finger moves it (a touchpad width: the display's width, times the
  /// speed, with the acceleration), a tap or the touchpad's button clicks, two fingers scroll (by the
  /// reverse wheel setting). Its fingers come as controls of this input and of those added here.
  void set_pad_touch(bool on) { this->pad_touch_ = on; }
  bool is_pad_touch() const { return this->pad_touch_; }
  void add_pad_touch_input(hid::HIDInput *input);
  /// Another input moving this pointer (a USB mouse next to the Bluetooth ones): its motion,
  /// buttons and wheel, and its touchpad as add_pad_touch_input().
  void add_input(hid::HIDInput *input);
  void set_pad_touch_capture(lv_obj_t *widget) { this->pad_capture_ = widget; }
  /// From a keypad (LVGL thread): its key code pressed as a direction (LV_KEY_UP..LEFT) or
  /// LV_KEY_ENTER, or a key let go (any code; those not held are passed over).
  void key_press(uint32_t code, uint32_t key);
  void key_release(uint32_t code);
  lv_indev_t *get_drv() { return this->drv_; }

  static constexpr int32_t WHEEL_LINE = 40;

 protected:
  void move_(int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons);
  void set_cursor_(lv_obj_t *cursor);

  void key_read_();
  /// A control of a pad touch input (main loop): its fingers' motion gathered for the next read.
  void pad_control_(uint16_t page, uint16_t usage, int32_t value);
  /// A key of a pad touch input (main loop): the touchpad's button.
  void pad_key_(uint16_t page, uint16_t usage, bool down);
  /// LVGL thread, each read: the gathered motion moves the pointer or scrolls; true while it clicks.
  bool pad_read_();
  void pad_reset_();

  // A tap: down this short, moved this little (across the touchpad, 0..65535), one finger.
  static constexpr uint32_t PAD_TAP_MS = 200;
  static constexpr int32_t PAD_TAP_MOVE = 1300;
  struct PadFinger {
    int32_t x, y;
    bool down, has_x, has_y;
  };
  PadFinger pad_fingers_[2]{};
  int32_t pad_dx_{0};  // gathered since the last read, 0..65535 across the touchpad
  int32_t pad_dy_{0};
  int32_t pad_sx_{0};  // two fingers: to scroll
  int32_t pad_sy_{0};
  int32_t pad_moved_{0};  // this touch, for the tap
  uint32_t pad_since_{0};
  uint8_t pad_most_{0};  // most fingers down at once this touch
  float pad_rest_x_{0.0f};
  float pad_rest_y_{0.0f};
  lv_obj_t *pad_capture_{nullptr};
  bool pad_touch_{false};
  bool pad_button_{false};
  bool pad_no_tap_{false};  // the button was pressed this touch
  bool pad_tap_{false};
  bool pad_tap_shown_{false};

  static constexpr uint8_t KEYS_HELD = 4;
  struct KeyHeld {
    uint32_t code;
    uint32_t key;
  };
  KeyHeld keys_[KEYS_HELD]{};
  uint32_t key_since_{0};
  uint32_t key_after_ms_{400};
  uint8_t key_step_{6};
  uint8_t key_most_{8};
  bool key_mode_{false};
  bool key_pressed_{false};
  float speed_{1.0f};
  float rest_x_{0.0f};  // motion below a pixel, carried to the next report
  float rest_y_{0.0f};
  bool acceleration_{false};
  bool reverse_wheel_{false};
  uint8_t wheel_lines_{1};
  int32_t x_{0};
  int32_t y_{0};
  bool pressed_{false};
  // A press LVGL has not read yet: a quick click's press and release can come between two reads
  // (USB HID reports arrive together in one loop pass), so the press is held for one read.
  bool press_unread_{false};
  bool left_handed_{false};
  bool own_cursor_{false};  // the built-in cursor (set_cursor_look() may change it)
  bool look_pending_{false};  // changed while LVGL was paused: made at the next motion
  uint8_t look_style_{0};
  uint8_t look_size_{1};
  void apply_look_();
  uint8_t last_buttons_{0};
  CallbackManager<void(uint8_t, bool)> button_cb_{};
  lv_indev_t *drv_{};
  lv_obj_t *cursor_{};
  uint32_t hide_after_{0};
  uint32_t last_motion_{0};
};

#ifdef USE_LVGL_REMOTE_INPUT
/// Input from outside (the API), for testing and debugging: a pointer at LVGL screen coordinates (tap, press, move,
/// release, swipe) as a touch gives it, and keys (key_bindings actions by name) and text as a keyboard gives them.
/// Main loop only. While LVGL is paused only keys are taken: their action fires at once.
class LVRemoteInput final : public Parented<LvglComponent> {
 public:
  /// The object in PSRAM, its indevs created; nullptr without PSRAM.
  static LVRemoteInput *create(KeyBindings *bindings, uint16_t long_press_time, uint16_t long_press_repeat_time,
                               LvglComponent *parent, lv_group_t *group);
  /// tap / press / move (x, y), release, swipe (x, y to x2, y2 in ms), key (text: an action's name), text (text).
  /// nullptr when taken, else why not.
  const char *run(const std::string &command, int32_t x, int32_t y, int32_t x2, int32_t y2, int32_t ms,
                  const std::string &text);
  /// The widget with the key focus, or nullptr.
  lv_obj_t *focused();
  /// A short name of the widget's type ("button", "textarea", ... "obj").
  static const char *type_name(lv_obj_t *obj);
  /// The text of a label, a text area, or of the widget's first label child; "" for none.
  static const char *text_of(lv_obj_t *obj);

 protected:
  LVRemoteInput(KeyBindings *bindings, uint16_t long_press_time, uint16_t long_press_repeat_time,
                LvglComponent *parent);
  enum StepKind : uint8_t { STEP_TAP, STEP_PRESS, STEP_MOVE, STEP_RELEASE, STEP_SWIPE };
  struct Step {
    int16_t x, y, x2, y2;
    uint16_t ms;
    StepKind kind;
  };
  static constexpr uint8_t STEPS = 8;
  bool push_(const Step &step);
  void read_(lv_indev_data_t *data);

  LVHIDKeypadListener keypad_;
  KeyBindings *bindings_;
  Step steps_[STEPS]{};
  Step current_{};
  uint32_t started_{0};
  lv_indev_t *drv_{};
  int16_t x_{0};
  int16_t y_{0};
  uint8_t head_{0};
  uint8_t count_{0};
  bool running_{false};
  // The step's first read is done (a tap's or a swipe's press).
  bool begun_{false};
  bool pressed_{false};
};
#endif  // USE_LVGL_REMOTE_INPUT
#endif  // USE_LVGL_HID

#ifdef USE_LVGL_LINE
class LvLineType : public LvCompound {
 public:
  void set_points(FixedVector<lv_point_precise_t> points) {
    this->points_ = std::move(points);
    lv_line_set_points(this->obj, this->points_.begin(), this->points_.size());
  }

 protected:
  FixedVector<lv_point_precise_t> points_{};
};
#endif
#ifdef USE_LVGL_TABLE
// Unlike most size properties, lv_table_set_column_width() only accepts a literal pixel
// count, so percentage column widths must be recomputed by hand whenever the table's own
// content width changes.
class LvTableType : public LvCompound {
 public:
  void set_obj(lv_obj_t *lv_obj) override;
  // count is the number of percentage-width columns, known at code-generation time.
  void init_column_pct(size_t count) { this->column_pct_.init(count); }
  void add_column_width_pct(uint32_t col, uint8_t pct);

 protected:
  void update_column_widths_();

  struct ColumnPct {
    uint32_t col;
    uint8_t pct;
  };
  FixedVector<ColumnPct> column_pct_{};
};
#endif  // USE_LVGL_TABLE
#if defined(USE_LVGL_DROPDOWN) || defined(LV_USE_ROLLER)
class LvSelectable : public LvCompound {
 public:
  virtual size_t get_selected_index() = 0;
  virtual void set_selected_index(size_t index, lv_anim_enable_t anim) = 0;
  void set_selected_text(const std::string &text, lv_anim_enable_t anim);
  std::string get_selected_text();
  const FixedVector<const char *> &get_options() { return this->options_; }
  void set_options(FixedVector<const char *> options);

 protected:
  virtual void set_option_string(const char *options) = 0;
  FixedVector<const char *> options_{};
};

#ifdef USE_LVGL_DROPDOWN
class LvDropdownType : public LvSelectable {
 public:
  size_t get_selected_index() override { return lv_dropdown_get_selected(this->obj); }
  void set_selected_index(size_t index, lv_anim_enable_t anim) override { lv_dropdown_set_selected(this->obj, index); }

 protected:
  void set_option_string(const char *options) override { lv_dropdown_set_options(this->obj, options); }
};
#endif  // USE_LVGL_DROPDOWN

#ifdef USE_LVGL_ROLLER
class LvRollerType : public LvSelectable {
 public:
  size_t get_selected_index() override { return lv_roller_get_selected(this->obj); }
  void set_selected_index(size_t index, lv_anim_enable_t anim) override {
    lv_roller_set_selected(this->obj, index, anim);
  }
  void set_mode(lv_roller_mode_t mode) { this->mode_ = mode; }

 protected:
  void set_option_string(const char *options) override { lv_roller_set_options(this->obj, options, this->mode_); }
  lv_roller_mode_t mode_{LV_ROLLER_MODE_NORMAL};
};
#endif
#endif  // defined(USE_LVGL_DROPDOWN) || defined(LV_USE_ROLLER)

#ifdef USE_LVGL_BUTTONMATRIX
class LvButtonMatrixType : public key_provider::KeyProvider, public LvCompound {
 public:
  void set_obj(lv_obj_t *lv_obj) override;
  uint16_t get_selected() { return lv_buttonmatrix_get_selected_button(this->obj); }
  void set_key(size_t idx, uint8_t key) { this->key_map_[idx] = key; }

 protected:
  std::map<size_t, uint8_t> key_map_{};
};
#endif  // USE_LVGL_BUTTONMATRIX

#ifdef USE_LVGL_KEYBOARD
class LvKeyboardType : public key_provider::KeyProvider, public LvCompound {
 public:
  void set_obj(lv_obj_t *lv_obj) override;
};
#endif  // USE_LVGL_KEYBOARD
}  // namespace esphome::lvgl
