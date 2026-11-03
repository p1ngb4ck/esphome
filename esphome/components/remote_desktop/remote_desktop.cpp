#include "remote_desktop.h"

#ifdef USE_REMOTE_DESKTOP

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_REMOTE_DESKTOP_USB
#include "select/usb_slot.h"
#endif

#include <esp_vfs_eventfd.h>
#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace esphome::remote_desktop {

static const char *const TAG = "remote_desktop";

RemoteDesktop *RemoteDesktop::create() {
  void *mem = RAMAllocator<RemoteDesktop>(RAMAllocator<RemoteDesktop>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) RemoteDesktop();
  return new RemoteDesktop();
}

void RemoteDesktop::add_hid(hid::HIDInput *input) {
  if (this->hid_count_ < MAX_HIDS)
    this->hids_[this->hid_count_++] = input;
}

void RemoteDesktop::setup() {
  this->inputs_q_ =
      xQueueCreateStatic(INPUTS, sizeof(Input), reinterpret_cast<uint8_t *>(this->input_slots_), &this->inputs_store_);
  const esp_vfs_eventfd_config_t eventfd_config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&eventfd_config);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "eventfd: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->wake_fd_ = eventfd(0, 0);
  if (this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "No eventfd");
    this->mark_failed();
    return;
  }
  for (size_t i = 0; i != this->hid_count_; i++) {
    hid::HIDInput *input = this->hids_[i];
    input->add_on_key_event_callback([this, input](uint16_t page, uint16_t usage, bool pressed) {
      if (!this->is_connected() || this->close_key_(input, page, usage, pressed))
        return;
      if (page == 0x07 && usage < 256)
        this->post_(Input{INPUT_KEY, static_cast<uint8_t>(pressed ? 1 : 0), usage, 0, 0});
    });
    this->hids_[i]->add_on_pointer_callback([this](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
      if (!this->is_connected())
        return;
      uint8_t mask = 0;
      if (buttons & 0x01)
        mask |= 0x01;
      if (buttons & 0x04)
        mask |= 0x02;
      if (buttons & 0x02)
        mask |= 0x04;
      uint16_t scroll = 0;
      if (wheel > 0)
        scroll |= 0x08;
      if (wheel < 0)
        scroll |= 0x10;
      if (pan < 0)
        scroll |= 0x20;
      if (pan > 0)
        scroll |= 0x40;
      this->post_(Input{INPUT_POINTER, mask, scroll, dx, dy});
    });
  }
#ifdef USE_REMOTE_DESKTOP_TOUCH
  if (this->touchscreen_ != nullptr)
    this->touchscreen_->register_listener(&this->touch_listener_);
#endif
  this->saved_setup_();
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  this->set_close_action(this->close_name_);
#endif
  this->disable_loop();
}

void RemoteDesktop::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Remote desktop:\n"
                "  Keyboards / mice: %u",
                static_cast<unsigned>(this->hid_count_));
}

void RemoteDesktop::wake() {
  if (this->wake_fd_ < 0)
    return;
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

#ifdef USE_REMOTE_DESKTOP_USB
void RemoteDesktop::add_usb_slot(UsbSlot *slot) {
  if (this->usb_count_ < USB_SLOTS_MAX) {
    slot->set_index(this->usb_count_);
    this->usb_[this->usb_count_++] = slot;
  }
}

void RemoteDesktop::usb_selected(UsbSlot *slot, uint8_t addr) {
  for (uint8_t i = 0; i < this->usb_count_; i++) {
    if (this->usb_[i] != slot)
      this->usb_[i]->forget_selection(addr);
  }
}

uint8_t *RemoteDesktop::usb_alloc(size_t size) {
  uint8_t *piece = static_cast<uint8_t *>(heap_caps_aligned_calloc(128, 1, size, MALLOC_CAP_SPIRAM));
  if (piece == nullptr)
    ESP_LOGE(TAG, "No PSRAM for a USB transfer of %zu bytes", size);
  return piece;
}

void RemoteDesktop::usb_free(uint8_t *piece) { heap_caps_free(piece); }
#endif

void RemoteDesktop::post_(const Input &input) {
  if (this->inputs_q_ == nullptr || xQueueSend(this->inputs_q_, &input, 0) != pdTRUE)
    return;
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

#ifdef USE_REMOTE_DESKTOP_TOUCH
void DesktopTouchListener::update(const touchscreen::TouchPoints_t &points) { this->parent_->on_touch(points); }

void DesktopTouchListener::release() {
  const touchscreen::TouchPoints_t none;
  this->parent_->on_touch(none);
}

void RemoteDesktop::on_touch(const touchscreen::TouchPoints_t &points) {
  if (!this->is_connected() || this->canvas_ == nullptr || !this->canvas_->held_by(this))
    return;
  if (points.size() >= 2) {
    if (!this->fingers_) {
      this->fingers_ = true;
      this->fingers_at_ = millis();
      if (this->touch_down_) {
        this->touch_down_ = false;
        this->post_(Input{INPUT_TOUCH, 0, 0, -1, -1});
      }
      this->close_check_();
    }
    return;
  }
  if (this->fingers_) {
    this->fingers_ = false;
    this->close_check_();
  }
  if (points.empty()) {
    if (this->touch_down_) {
      this->touch_down_ = false;
      this->post_(Input{INPUT_TOUCH, 0, 0, -1, -1});
    }
    return;
  }
  int32_t x = points.front().x;
  int32_t y = points.front().y;
  if (!this->canvas_->canvas_point(&x, &y))
    return;
  this->touch_down_ = true;
  this->post_(Input{INPUT_TOUCH, 0x01, 0, static_cast<int16_t>(x), static_cast<int16_t>(y)});
}
#endif

bool RemoteDesktop::connect(Protocol protocol, const std::string &host, uint16_t port, const std::string &password) {
  if (this->wake_fd_ < 0 || this->output_ == nullptr || this->state_.load() != STATE_IDLE)
    return false;
  if (host.empty() || host.size() >= HOST_MAX || password.size() >= PASSWORD_MAX) {
    ESP_LOGW(TAG, "Host or password too long");
    return false;
  }
  std::memcpy(this->host_, host.c_str(), host.size() + 1);
  std::memcpy(this->password_, password.c_str(), password.size() + 1);
  this->port_ = port;
  this->protocol_ = protocol;
  this->active_saved_ = -1;
  this->stop_.store(false);
  xQueueReset(this->inputs_q_);
  this->failed_.store(false);
  this->session_++;
  this->state_.store(STATE_STARTING);
  if (!this->task_.create(&RemoteDesktop::task_entry_, "remote_desktop", 8192, this, 4, true, 0)) {
    ESP_LOGE(TAG, "No session task");
    this->state_.store(STATE_IDLE);
    return false;
  }
  this->enable_loop();
  this->changed_();
  return true;
}

void RemoteDesktop::disconnect() {
  if (this->state_.load() == STATE_IDLE)
    return;
  this->stop_.store(true);
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

bool RemoteDesktop::close_key_(hid::HIDInput *input, uint16_t page, uint16_t usage, bool pressed) {
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  if (pressed && !this->key_held_ && this->bindings_ != nullptr && this->close_action_ >= 0) {
    const int action = this->bindings_->resolve(page, usage, lvgl::KeyBindings::modifiers_of(input->get_modifiers()));
    if (action == this->close_action_) {
      this->key_held_ = true;
      this->key_page_ = page;
      this->key_usage_ = usage;
      this->key_at_ = millis();
      this->close_check_();
      return true;
    }
  }
  if (!pressed && this->key_held_ && page == this->key_page_ && usage == this->key_usage_) {
    this->key_held_ = false;
    this->close_check_();
    if (page == 0x07 && usage < 256) {
      this->post_(Input{INPUT_KEY, 1, usage, 0, 0});
      this->post_(Input{INPUT_KEY, 0, usage, 0, 0});
    }
    return true;
  }
#endif
  return false;
}

void RemoteDesktop::close_check_() {
  const uint32_t now = millis();
  uint32_t due = UINT32_MAX;
  auto left = [now](uint32_t since, uint32_t hold) { return now - since >= hold ? 0 : hold - (now - since); };
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  if (this->key_held_)
    due = std::min(due, left(this->key_at_, this->close_hold_ms_));
#endif
  if (this->fingers_)
    due = std::min(due, left(this->fingers_at_, this->touch_hold_ms_));
  if (due == UINT32_MAX) {
    this->cancel_timeout("close");
    return;
  }
  if (due != 0) {
    this->set_timeout("close", due, [this]() { this->close_check_(); });
    return;
  }
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  this->key_held_ = false;
#endif
  this->fingers_ = false;
  ESP_LOGI(TAG, "Closed by holding");
  this->disconnect();
}

void RemoteDesktop::loop() {
  this->output_->loop();
  if (this->changed_flag_.exchange(false))
    this->change_callbacks_.call();
  if (this->close_page_.exchange(false) && this->canvas_ != nullptr) {
    if (this->canvas_->held_by(this))
      this->canvas_->close();
    this->canvas_ = nullptr;
  }
  if (this->state_.load() == STATE_ENDED && this->task_.deallocate()) {
    this->state_.store(STATE_IDLE);
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
    this->key_held_ = false;
#endif
    this->fingers_ = false;
    this->cancel_timeout("close");
    this->change_callbacks_.call();
    this->disable_loop();
  }
}

void RemoteDesktop::task_entry_(void *param) {
  auto *self = static_cast<RemoteDesktop *>(param);
  self->run_session_();
  self->failed_.store(self->state_.load() == STATE_STARTING);
  self->state_.store(STATE_ENDED);
  self->enable_loop_soon_any_context();
  vTaskSuspend(nullptr);
}

bool RemoteDesktop::allocate_() {
  this->in_ = static_cast<uint8_t *>(heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->out_ = static_cast<uint8_t *>(heap_caps_malloc(OUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const size_t cursor = static_cast<size_t>(CURSOR_MAX) * CURSOR_MAX;
  this->cursor_pixels_ = static_cast<uint8_t *>(heap_caps_malloc(cursor * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->cursor_mask_ = static_cast<uint8_t *>(heap_caps_malloc(cursor / 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->cursor_under_ = static_cast<uint8_t *>(heap_caps_malloc(cursor * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return this->in_ != nullptr && this->out_ != nullptr && this->cursor_pixels_ != nullptr &&
         this->cursor_mask_ != nullptr && this->cursor_under_ != nullptr;
}

void RemoteDesktop::free_() {
  heap_caps_free(this->in_);
  heap_caps_free(this->out_);
  heap_caps_free(this->cursor_pixels_);
  heap_caps_free(this->cursor_mask_);
  heap_caps_free(this->cursor_under_);
  this->in_ = nullptr;
  this->out_ = nullptr;
  this->cursor_pixels_ = nullptr;
  this->cursor_mask_ = nullptr;
  this->cursor_under_ = nullptr;
}

}

#endif
