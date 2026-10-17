#pragma once

#include "esphome/components/key_provider/key_provider.h"
#include "esphome/core/helpers.h"
#include "gamepad.h"
#include "hid_parser.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esphome {
class Component;
}

namespace esphome::hid {

static constexpr uint16_t PAGE_GENERIC_DESKTOP = 0x01;
static constexpr uint16_t PAGE_KEYBOARD = 0x07;
static constexpr uint16_t PAGE_LED = 0x08;
static constexpr uint16_t PAGE_BUTTON = 0x09;
static constexpr uint16_t PAGE_CONSUMER = 0x0C;

enum Modifier : uint8_t {
  MOD_LEFT_CTRL = 0x01,
  MOD_LEFT_SHIFT = 0x02,
  MOD_LEFT_ALT = 0x04,
  MOD_LEFT_GUI = 0x08,
  MOD_RIGHT_CTRL = 0x10,
  MOD_RIGHT_SHIFT = 0x20,
  MOD_RIGHT_ALT = 0x40,
  MOD_RIGHT_GUI = 0x80,
};

// Bits of the boot keyboard LED report (HID usage tables, LED page 0x01..0x03).
enum Led : uint8_t {
  LED_NUM_LOCK = 0x01,
  LED_CAPS_LOCK = 0x02,
  LED_SCROLL_LOCK = 0x04,
};

// A keyboard layout as hid/__init__.py emits it: what each key types at each level (plain,
// Shift, AltGr, Shift+AltGr), which of those are dead keys, and what dead keys combine to.
struct LayoutKey {
  uint8_t usage;
  uint8_t dead;     // bit per level: the character is a combining mark, put on the next one
  bool alphabetic;  // Caps Lock acts as Shift on it
  char32_t chars[4];
};
struct LayoutCompose {
  char32_t mark;
  char32_t base;
  char32_t result;
};
struct KeyboardLayout {
  const LayoutKey *keys;
  uint16_t key_count;
  const LayoutCompose *compose;
  uint16_t compose_count;
};
static_assert(sizeof(LayoutKey) == 20 && sizeof(LayoutCompose) == 12, "the .kbl file holds them as they are");

/// A layout file as hid/layouts' kbl() writes it: "KBL1", the key and composition counts (uint16),
/// the keys, the compositions. `out` points into `data` (4-byte aligned), which must outlive it.
bool parse_layout(const uint8_t *data, size_t len, KeyboardLayout *out);

/// A device behind an input (USB: the one connected; Bluetooth: each paired one), for a layout of
/// its own: key is what names it across connections, "usb:046d:c31c" (":" and its serial number
/// when it reports one) or "bt:AA:BB:CC:DD:EE:FF".
struct InputDevice {
  std::string key;
  std::string name;
  bool connected;
};

// Key names, emitted by hid/__init__.py from its table.
struct KeyName {
  uint16_t page;
  uint16_t usage;
  const char *name;
};
extern const KeyName KEY_NAMES[];
extern const uint16_t KEY_NAMES_COUNT;

/// The table's name of a key, or "page:usage" in hex for one it has none for.
std::string key_name(uint16_t page, uint16_t usage);
void append_utf8(std::string &out, char32_t c);

/// The output report of a report descriptor that carries the Num, Caps and Scroll Lock LEDs,
/// with the bits of `leds` set, without its report ID byte. False if the device has none.
bool build_led_report(const HIDReportMap &map, uint8_t leds, uint8_t &report_id, std::vector<uint8_t> &payload);

// What any HID transport (USB, BLE, Bluetooth Classic) feeds its decoded reports into: keys,
// typed text, pointer motion and every other control, for automations, sensors, LVGL and
// key_provider (key_collector) alike. Typed characters go to key_provider as well when they
// are ASCII, Enter as '\n' and Backspace as '\b'.
class HIDInput : public key_provider::KeyProvider {
 public:
  void set_layout(const KeyboardLayout *layout) { this->layout_ = layout; }
  const KeyboardLayout *get_layout() const { return this->layout_; }
  /// What the layout puts on a keyboard key (page 7) with the modifiers and Caps Lock as they are now,
  /// regardless of Ctrl, Alt and GUI: for a dead key LAYOUT_DEAD | its combining mark; 0 for a key it
  /// has nothing on.
  static constexpr char32_t LAYOUT_DEAD = 0x80000000;
  char32_t layout_char(uint16_t usage) const;
  /// Whether the layout puts anything on AltGr (Right Alt): then it is a level, not Alt.
  bool layout_has_level3() const;
  void set_num_lock(bool on) { this->leds_ = on ? (this->leds_ | LED_NUM_LOCK) : (this->leds_ & ~LED_NUM_LOCK); }
  /// The device's id, for the key log.
  void set_log_name(const char *name) { this->log_name_ = name; }

  /// The values the report parser decoded from one report.
  void process_hid_values(const std::vector<HIDReportItemValue> &values);
  /// The device went away: everything it held is released.
  void release_all_hid();

  /// (page, usage, pressed) for every key and button.
  template<typename F> void add_on_key_event_callback(F &&cb) { this->key_cb_.add(std::forward<F>(cb)); }
  /// The text a key press typed, UTF-8.
  template<typename F> void add_on_text_callback(F &&cb) { this->text_cb_.add(std::forward<F>(cb)); }
  /// (dx, dy, wheel, pan, buttons) once per report that moved or clicked a pointer.
  template<typename F> void add_on_pointer_callback(F &&cb) { this->pointer_cb_.add(std::forward<F>(cb)); }
  /// (page, usage, value) for every other control: axes, hat switches, sliders.
  template<typename F> void add_on_control_callback(F &&cb) { this->control_cb_.add(std::forward<F>(cb)); }

  /// HIDDeviceKind bits of the devices connected behind this input; 0 when none is.
  virtual uint8_t get_device_kinds() const { return 0; }

  /// Main loop: the devices behind it.
  virtual void list_devices(std::vector<InputDevice> &out) const {}
  /// What one of them types with; null: the input's own (set_layout()).
  virtual void set_device_layout(const std::string &key, const KeyboardLayout *layout) {}
  /// Main loop: a device behind it came or went, was paired or forgotten.
  template<typename F> void add_on_devices_callback(F &&cb) { this->devices_cb_.add(std::forward<F>(cb)); }

  // -- Gamepads ----------------------------------------------------------------------------
  /// Rumble, 0..255 per motor (strong: low frequency, weak: high frequency). Duration 0 holds
  /// it until the next call.
  virtual void rumble(uint8_t strong, uint8_t weak, uint32_t duration_ms);
  /// Player number on the pad's LEDs, 0 off.
  virtual void set_player_led(uint8_t player);
  virtual void set_light_bar(uint8_t red, uint8_t green, uint8_t blue);
  const GamepadOutputs &get_gamepad_outputs() const { return this->pad_outputs_; }

  // For the gamepad drivers: the transport. Report ID first where the device numbers its reports.
  /// An output report on the interrupt channel (USB interrupt OUT, Bluetooth interrupt channel,
  /// BLE output report characteristic).
  virtual bool send_output_report(const uint8_t *data, size_t len) { return false; }
  /// SET_REPORT on the control channel (BLE: the report characteristic of that type).
  virtual bool set_report(HIDReportKind kind, const uint8_t *data, size_t len) { return false; }
  /// GET_REPORT of a feature report of len bytes (its ID included); answered through
  /// GamepadDriver::feature().
  virtual bool request_feature_report(uint8_t id, uint16_t len) { return false; }
  /// The component the drivers' timeouts run on.
  virtual Component *hid_component() { return nullptr; }
  HIDBus get_bus() const { return this->bus_; }
  /// A driver's decoded state; what changed is reported as keys and controls.
  void gamepad_state(const GamepadState &state);

  uint8_t get_modifiers() const { return this->modifiers_; }
  uint8_t get_leds() const { return this->leds_; }
  uint8_t get_pointer_buttons() const { return this->buttons_; }
  bool is_key_down(uint16_t page, uint16_t usage) const;

 protected:
  /// A transport that can light the lock LEDs sends them to the keyboard.
  virtual void write_leds_(uint8_t leds) {}

  /// Transport, main loop: the device is there, by bus and identity; picks its driver.
  void gamepad_attach_(HIDBus bus, uint16_t vid, uint16_t pid, uint16_t version);
  /// A transport's own driver, for pads that are not HID.
  void gamepad_attach_(HIDBus bus, std::unique_ptr<GamepadDriver> driver);
  /// Transport: it can send to the device now.
  void gamepad_start_();
  void gamepad_detach_();
  /// True when the driver took the report (report ID first where the device numbers them).
  bool gamepad_report_(const uint8_t *data, size_t len);
  void gamepad_feature_(uint8_t id, const uint8_t *data, size_t len);
  bool gamepad_axis_(const HIDReportItemValue &v);
  void gamepad_hat_(const HIDReportItemValue &v);

  void key_(uint16_t page, uint16_t usage, bool pressed);
  void type_key_(uint16_t usage);
  const LayoutKey *layout_key_(uint16_t usage) const;
  /// The level of the key (plain, Shift, AltGr, Shift+AltGr) with the modifiers and Caps Lock now.
  uint8_t level_(const LayoutKey &key) const;
  void type_(char32_t c);
  char32_t compose_(char32_t mark, char32_t base) const;

  CallbackManager<void(uint16_t, uint16_t, bool)> key_cb_;
  CallbackManager<void(const std::string &)> text_cb_;
  CallbackManager<void(int16_t, int16_t, int16_t, int16_t, uint8_t)> pointer_cb_;
  CallbackManager<void(uint16_t, uint16_t, int32_t)> control_cb_;
  CallbackManager<void()> devices_cb_;
  /// The layout typed with: the device's own where it has one (device_layout_), else the input's.
  const KeyboardLayout *active_layout_() const {
    return this->device_layout_ != nullptr ? this->device_layout_ : this->layout_;
  }
  const KeyboardLayout *layout_{nullptr};
  const KeyboardLayout *device_layout_{nullptr};
  const char *log_name_{""};
  std::vector<uint32_t> down_;  // page << 16 | usage of every key held
  char32_t dead_{0};
  uint8_t modifiers_{0};
  uint8_t leds_{LED_NUM_LOCK};
  uint8_t buttons_{0};
  std::unique_ptr<GamepadDriver> pad_;
  GamepadState pad_state_;
  GamepadOutputs pad_outputs_;
  HIDBus bus_{HIDBus::USB};
};

}  // namespace esphome::hid
