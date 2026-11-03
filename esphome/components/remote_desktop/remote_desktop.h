#pragma once

#include "esphome/core/defines.h"
#ifdef USE_REMOTE_DESKTOP

#include "esphome/components/display/video_output.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"
#ifdef USE_REMOTE_DESKTOP_TOUCH
#include "esphome/components/touchscreen/touchscreen.h"
#endif
#ifdef USE_REMOTE_DESKTOP_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif
#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
#include "esphome/components/lvgl/key_bindings.h"
#endif
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
#include "esphome/components/storage/storage_worker.h"
#endif
#include "esphome/core/preferences.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <string>

namespace esphome::remote_desktop {

class RemoteDesktop;

#ifdef USE_REMOTE_DESKTOP_TOUCH
class DesktopTouchListener : public touchscreen::TouchListener {
 public:
  explicit DesktopTouchListener(RemoteDesktop *parent) : parent_(parent) {}
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;

 protected:
  RemoteDesktop *parent_;
};
#endif

struct Inflate;
class UsbSlot;
class SpiceSession;
extern const uint16_t KEYBOARD_XT[0xE8];

class RemoteDesktop : public Component {
 public:
  static RemoteDesktop *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_video_output(display::VideoOutput *output) { this->output_ = output; }
  void add_hid(hid::HIDInput *input);
#ifdef USE_REMOTE_DESKTOP_SPEAKER
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
#endif
#ifdef USE_REMOTE_DESKTOP_USB
  void add_usb_slot(UsbSlot *slot);
  void usb_selected(UsbSlot *slot, uint8_t addr);
  uint8_t *usb_alloc(size_t size);
  void usb_free(uint8_t *piece);
  void usb_changed(UsbSlot *slot);
#endif
  void wake();
  static constexpr uint8_t USB_SLOTS_MAX = 4;
#ifdef USE_REMOTE_DESKTOP_TOUCH
  void set_touchscreen(touchscreen::Touchscreen *touchscreen) { this->touchscreen_ = touchscreen; }
  void on_touch(const touchscreen::TouchPoints_t &points);
#endif

  enum Protocol : uint8_t { PROTOCOL_VNC, PROTOCOL_SPICE, PROTOCOL_AUTO };
  bool connect(Protocol protocol, const std::string &host, uint16_t port, const std::string &password);
  void disconnect();
  bool is_connected() const { return this->state_.load() == STATE_RUNNING; }
  bool is_connecting() const { return this->state_.load() == STATE_STARTING; }
  bool failed() const { return this->failed_.load(); }
  uint32_t session() const { return this->session_; }

  void set_connections(uint8_t count) { this->saved_count_ = count; }
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
  void set_connections_file(const char *path) { this->file_path_ = path; }
#endif
  uint8_t saved_count() const { return this->saved_count_; }
  bool saved(uint8_t index, std::string &name, std::string &host, uint16_t &port) const;
  void save(uint8_t index, const std::string &name, const std::string &host, uint16_t port);
  int save_new(const std::string &name, const std::string &host, uint16_t port);
  void forget(uint8_t index);
  bool connect_saved(uint8_t index, const std::string &password);
  void add_on_change_callback(std::function<void()> &&callback) { this->change_callbacks_.add(std::move(callback)); }

#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  void set_key_bindings(lvgl::KeyBindings *bindings) { this->bindings_ = bindings; }
  void set_close_action_name(const char *name) { this->close_name_ = name; }
  void set_close_action(const std::string &name) {
    this->close_action_ = this->bindings_ != nullptr ? this->bindings_->find_action(name) : -1;
  }
  void set_close_hold(float seconds) { this->close_hold_ms_ = static_cast<uint32_t>(seconds * 1000); }
#endif
  void set_touch_hold(float seconds) { this->touch_hold_ms_ = static_cast<uint32_t>(seconds * 1000); }

 protected:
  friend class SpiceSession;
  enum State : uint8_t { STATE_IDLE, STATE_STARTING, STATE_RUNNING, STATE_ENDED };
  enum : uint8_t { INPUT_KEY, INPUT_POINTER, INPUT_TOUCH };
  struct Input {
    uint8_t kind;
    uint8_t buttons;
    uint16_t code;
    int16_t x;
    int16_t y;
  };
  static constexpr size_t INPUTS = 32;
  static constexpr size_t IN_BYTES = 512 * 1024;
  static constexpr size_t OUT_BYTES = 256 * 1024;
  static constexpr uint32_t CURSOR_MAX = 64;
  static constexpr int STREAMS = 4;
  static constexpr size_t HOST_MAX = 64;
  static constexpr size_t PASSWORD_MAX = 64;

  void post_(const Input &input);
  bool close_key_(hid::HIDInput *input, uint16_t page, uint16_t usage, bool pressed);
  void close_check_();
  Protocol probe_();
  void changed_();
  void saved_setup_();
  void saved_changed_(uint8_t index);
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
  void file_load_();
  void file_read_();
  void file_read_done_(storage::StorageError err);
  void file_parse_();
  void file_save_();
  void file_write_();
  void file_free_();
#endif
  static void task_entry_(void *param);
  void run_session_();
  void spice_session_();
  bool allocate_();
  void free_();

  bool read_(void *data, size_t len);
  bool write_(const void *data, size_t len);
  bool skip_(size_t len);
  bool read_compact_(uint32_t *len);
  bool handshake_();
  bool message_();
  bool update_();
  bool rect_raw_(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  bool rect_copy_(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  bool rect_tight_(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
  bool rect_cursor_(uint32_t hot_x, uint32_t hot_y, uint32_t w, uint32_t h);
  bool rect_desktop_size_(uint32_t reason, uint32_t status, uint32_t w, uint32_t h);
  bool inflate_(int stream, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
  bool request_update_(bool incremental);
  void tpixel_to_page_(const uint8_t *in, uint8_t *out) const;
  void pixel_to_page_(const uint8_t *in, uint8_t *out) const;
  bool draw_row_(const uint8_t *pixels, uint32_t x, uint32_t y, uint32_t w);
  bool draw_fill_(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *pixel);
  bool inputs_();
  bool send_key_(uint16_t usage, bool pressed);
  bool send_pointer_();
  void cursor_hide_();
  void cursor_show_();

  display::VideoOutput *output_{nullptr};
  display::VideoCanvas *canvas_{nullptr};
#ifdef USE_REMOTE_DESKTOP_SPEAKER
  speaker::Speaker *speaker_{nullptr};
#endif
#ifdef USE_REMOTE_DESKTOP_USB
  UsbSlot *usb_[USB_SLOTS_MAX]{};
  uint8_t usb_count_{0};
  void vnc_usb_session_();
#endif
  static constexpr size_t MAX_HIDS = 4;
  hid::HIDInput *hids_[MAX_HIDS]{};
  size_t hid_count_{0};
#ifdef USE_REMOTE_DESKTOP_TOUCH
  touchscreen::Touchscreen *touchscreen_{nullptr};
  DesktopTouchListener touch_listener_{this};
  bool touch_down_{false};
#endif

  std::atomic<uint8_t> state_{STATE_IDLE};
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  uint32_t session_{0};
  std::atomic<bool> close_page_{false};
  StaticTask task_;
  int fd_{-1};
  int wake_fd_{-1};
  char host_[HOST_MAX]{};
  uint16_t port_{0};
  Protocol protocol_{PROTOCOL_VNC};
  char password_[PASSWORD_MAX]{};

  QueueHandle_t inputs_q_{nullptr};
  StaticQueue_t inputs_store_{};
  Input input_slots_[INPUTS]{};

  uint8_t *in_{nullptr};
  uint8_t *out_{nullptr};
  Inflate *inflate_state_{nullptr};
  uint8_t page_bpp_{2};
  uint8_t pixel_bytes_{2};
  uint8_t tpixel_bytes_{2};
  uint32_t page_w_{0};
  uint32_t page_h_{0};
  uint32_t fb_w_{0};
  uint32_t fb_h_{0};
  bool resize_asked_{false};
  bool qemu_keys_{false};
  bool shift_{false};
  uint32_t pressed_keysym_[256]{};
  int32_t pointer_x_{0};
  int32_t pointer_y_{0};
  uint8_t pointer_buttons_{0};

  uint8_t *cursor_pixels_{nullptr};
  uint8_t *cursor_mask_{nullptr};
  uint8_t *cursor_under_{nullptr};
  uint32_t cursor_w_{0};
  uint32_t cursor_h_{0};
  uint32_t cursor_hot_x_{0};
  uint32_t cursor_hot_y_{0};
  bool cursor_shown_{false};
  uint32_t under_x_{0};
  uint32_t under_y_{0};
  uint32_t under_w_{0};
  uint32_t under_h_{0};

  struct Saved {
    char name[32];
    char host[64];
    uint16_t port;
    bool used;
  };
  struct SavedUsb {
    uint16_t vid;
    uint16_t pid;
    char serial[64];
  };
  static constexpr uint8_t SAVED_USB = 4;
  static constexpr size_t FILE_LINE = 512;
  uint8_t saved_count_{8};
  Saved *saved_{nullptr};
  SavedUsb *saved_usb_{nullptr};
  ESPPreferenceObject *saved_prefs_{nullptr};
  int8_t active_saved_{-1};
  bool usb_applying_{false};
  CallbackManager<void()> change_callbacks_;
  std::atomic<bool> changed_flag_{false};
#ifdef USE_REMOTE_DESKTOP_CONNECTIONS_FILE
  const char *file_path_{nullptr};
  char *file_buf_{nullptr};
  size_t file_len_{0};
  size_t file_got_{0};
  storage::StreamHandle file_handle_{};
  bool file_loaded_{false};
  bool file_busy_{false};
  bool file_dirty_{false};
#endif

#ifdef USE_REMOTE_DESKTOP_KEY_BINDINGS
  lvgl::KeyBindings *bindings_{nullptr};
  int close_action_{-1};
  const char *close_name_{"home"};
  uint32_t close_hold_ms_{1000};
  bool key_held_{false};
  uint16_t key_page_{0};
  uint16_t key_usage_{0};
  uint32_t key_at_{0};
#endif
  uint32_t touch_hold_ms_{1000};
  bool fingers_{false};
  uint32_t fingers_at_{0};
};

template<typename... Ts> class ConnectAction : public Action<Ts...>, public Parented<RemoteDesktop> {
 public:
  void set_protocol(RemoteDesktop::Protocol protocol) { this->protocol_ = protocol; }
  TEMPLATABLE_VALUE(std::string, host)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(std::string, password)
  void play(const Ts &...x) override {
    this->parent_->connect(this->protocol_, this->host_.value(x...), this->port_.value(x...),
                           this->password_.value(x...));
  }

 protected:
  RemoteDesktop::Protocol protocol_{RemoteDesktop::PROTOCOL_VNC};
};

template<typename... Ts> class ConnectSavedAction : public Action<Ts...>, public Parented<RemoteDesktop> {
 public:
  TEMPLATABLE_VALUE(uint8_t, index)
  TEMPLATABLE_VALUE(std::string, password)
  void play(const Ts &...x) override {
    this->parent_->connect_saved(this->index_.value(x...), this->password_.value(x...));
  }
};

template<typename... Ts> class SaveAction : public Action<Ts...>, public Parented<RemoteDesktop> {
 public:
  TEMPLATABLE_VALUE(uint8_t, index)
  TEMPLATABLE_VALUE(std::string, name)
  TEMPLATABLE_VALUE(std::string, host)
  TEMPLATABLE_VALUE(uint16_t, port)
  void play(const Ts &...x) override {
    this->parent_->save(this->index_.value(x...), this->name_.value(x...), this->host_.value(x...),
                        this->port_.value(x...));
  }
};

template<typename... Ts> class ForgetAction : public Action<Ts...>, public Parented<RemoteDesktop> {
 public:
  TEMPLATABLE_VALUE(uint8_t, index)
  void play(const Ts &...x) override { this->parent_->forget(this->index_.value(x...)); }
};

template<typename... Ts> class DisconnectAction : public Action<Ts...>, public Parented<RemoteDesktop> {
 public:
  void play(const Ts &...x) override { this->parent_->disconnect(); }
};

template<typename... Ts> class ConnectedCondition : public Condition<Ts...>, public Parented<RemoteDesktop> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_connected(); }
};

}

#endif
