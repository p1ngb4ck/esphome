#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace esphome::hid {

class HIDInput;

enum class HIDBus : uint8_t { USB, BT_CLASSIC, BLE };

// Canonical gamepad buttons, positional (south is A on Xbox, Cross on PlayStation, B on
// Nintendo). Bit n-1 is reported as BUTTON_n.
enum GamepadButton : uint32_t {
  PAD_SOUTH = 1u << 0,
  PAD_EAST = 1u << 1,
  PAD_WEST = 1u << 2,
  PAD_NORTH = 1u << 3,
  PAD_L1 = 1u << 4,
  PAD_R1 = 1u << 5,
  PAD_SELECT = 1u << 6,  // Back, View, Share (DS4), Create (DS5), Minus
  PAD_START = 1u << 7,   // Menu, Options, Plus
  PAD_L3 = 1u << 8,
  PAD_R3 = 1u << 9,
  PAD_HOME = 1u << 10,  // Guide, PS, Home, Steam
  PAD_L2 = 1u << 11,    // digital L2/ZL/LT
  PAD_R2 = 1u << 12,
  PAD_CAPTURE = 1u << 13,  // Capture, Xbox Share
  PAD_TOUCHPAD = 1u << 14,
  PAD_MUTE = 1u << 15,
  PAD_PADDLE1 = 1u << 16,
  PAD_PADDLE2 = 1u << 17,
  PAD_PADDLE3 = 1u << 18,
  PAD_PADDLE4 = 1u << 19,
};
static constexpr uint8_t PAD_BUTTON_COUNT = 20;

// D-pad bits, reported as the Generic Desktop D-pad usages 0x90 + bit index.
enum GamepadDpad : uint8_t {
  DPAD_UP = 1u << 0,
  DPAD_DOWN = 1u << 1,
  DPAD_RIGHT = 1u << 2,
  DPAD_LEFT = 1u << 3,
};

// Axes, reported as AXIS_X, AXIS_Y, AXIS_RX, AXIS_RY (sticks, -32768..32767, Y down positive)
// and AXIS_Z, AXIS_RZ (left and right trigger, 0..65535).
enum GamepadAxis : uint8_t { PAD_LX, PAD_LY, PAD_RX, PAD_RY, PAD_LT, PAD_RT, PAD_AXIS_COUNT };

// A finger on the pad's touchpad, x and y in the touchpad's range (GamepadState::touch_width/height).
struct GamepadTouch {
  bool down{false};
  uint16_t x{0};
  uint16_t y{0};
  bool operator!=(const GamepadTouch &other) const {
    return this->down != other.down || this->x != other.x || this->y != other.y;
  }
};
static constexpr uint8_t PAD_TOUCH_COUNT = 2;

struct GamepadState {
  uint32_t buttons{0};
  uint8_t dpad{0};
  int32_t axes[PAD_AXIS_COUNT]{};
  GamepadTouch touch[PAD_TOUCH_COUNT]{};
  uint16_t touch_width{0};  // 0: no touchpad
  uint16_t touch_height{0};
};

// What the outputs of a pad are set to: rumble motors (0..255), player number (0 off), light bar.
struct GamepadOutputs {
  uint8_t strong{0};
  uint8_t weak{0};
  uint8_t player{1};
  uint8_t red{0};
  uint8_t green{0};
  uint8_t blue{64};
};

/// v in [min, max] to -32768..32767, the middle of the range to 0.
int32_t pad_stick(int32_t v, int32_t min, int32_t max);
/// v in [min, max] to 0..65535.
int32_t pad_trigger(int32_t v, int32_t min, int32_t max);
/// A hat switch direction (0 north, clockwise to 7) to D-pad bits; anything else is centered.
uint8_t pad_hat(int32_t direction);

// The protocol of one kind of pad: its init sequence, its input reports and its outputs. It sends
// through the transport hooks of HIDInput and reports through HIDInput::gamepad_state().
class GamepadDriver {
 public:
  explicit GamepadDriver(HIDInput *input) : input_(input) {}
  virtual ~GamepadDriver() = default;
  virtual const char *name() const = 0;
  /// The transport can send now.
  virtual void start() {}
  /// One input report, its report ID first where the device numbers its reports. True when the
  /// driver took it; false leaves it to the report descriptor.
  virtual bool report(const uint8_t *data, size_t len) = 0;
  /// The answer to HIDInput::request_feature_report(), its report ID first; len 0 when it failed.
  virtual void feature(uint8_t id, const uint8_t *data, size_t len) {}
  virtual void outputs(const GamepadOutputs &out) {}
  /// Button usage of a report the descriptor decodes, to its canonical number (layouts that differ).
  virtual uint16_t button(uint16_t usage) const { return usage; }

 protected:
  HIDInput *input_;
};

/// The driver for a HID-class pad, by bus and vendor/product; null when the report descriptor
/// alone describes it.
std::unique_ptr<GamepadDriver> make_gamepad_driver(HIDInput *input, HIDBus bus, uint16_t vid, uint16_t pid,
                                                   uint16_t version);
/// Whether make_gamepad_driver() has a driver for the pad: one that talks to it (its output reports), so
/// a USB transport keeps the pad's interrupt OUT pipe.
bool has_gamepad_driver(HIDBus bus, uint16_t vid, uint16_t pid);

}  // namespace esphome::hid
