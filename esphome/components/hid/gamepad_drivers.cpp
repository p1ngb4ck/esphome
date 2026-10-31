// Gamepad protocols, from Linux (hid-sony.c, hid-playstation.c, hid-nintendo.c, hid-steam.c,
// hid-google-stadiaff.c) and SDL's hidapi drivers (Wii remote, Steam Controller BLE, Xbox over
// Bluetooth), mapped to the canonical layout of gamepad.h.

#include "gamepad.h"
#include "hid_input.h"

#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#ifdef USE_BT_CLASSIC
#include <esp_bt_device.h>
#endif

namespace esphome::hid {

static const char *const TAG = "hid.gamepad";

static inline uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static inline int32_t neg16(int32_t v) { return std::clamp<int32_t>(-v, -32768, 32767); }

// CRC-32 (IEEE, reflected) over a seed byte and the data, as the PlayStation pads sign their
// Bluetooth output reports with (Linux ps_crc32: the seed is the HIDP header).
static uint32_t ps_crc32(uint8_t seed, const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  auto feed = [&crc](uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
  };
  feed(seed);
  for (size_t i = 0; i < len; i++)
    feed(data[i]);
  return ~crc;
}

static void put_crc(uint8_t *report, size_t len) {
  const uint32_t crc = ps_crc32(0xA2, report, len - 4);
  report[len - 4] = crc & 0xFF;
  report[len - 3] = (crc >> 8) & 0xFF;
  report[len - 2] = (crc >> 16) & 0xFF;
  report[len - 1] = (crc >> 24) & 0xFF;
}

// An output report on the interrupt channel, else as SET_REPORT.
static bool send_out(HIDInput *input, const uint8_t *data, size_t len) {
  return input->send_output_report(data, len) || input->set_report(HIDReportKind::OUTPUT, data, len);
}

// -- DualShock 3 / Sixaxis, Navigation controller (hid-sony.c) -------------------------------

class SixaxisDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "DualShock 3"; }

  void start() override {
    if (this->input_->get_bus() == HIDBus::USB) {
      // Operational mode: two feature reads, then a one-byte output report.
      this->input_->request_feature_report(0xF2, 17);
      return;
    }
    static const uint8_t ENABLE[] = {0xF4, 0x42, 0x03, 0x00, 0x00};
    this->input_->set_report(HIDReportKind::FEATURE, ENABLE, sizeof(ENABLE));
    this->outputs(this->input_->get_gamepad_outputs());
  }

  void feature(uint8_t id, const uint8_t *data, size_t len) override {
    if (id == 0xF2) {
      this->input_->request_feature_report(0xF5, 8);
      return;
    }
    if (id == 0xF5) {
      if (len != 0)
        this->input_->send_output_report(data, 1);
#ifdef USE_BT_CLASSIC
      // Pair it to this device: plugged in once, it connects here over Bluetooth after.
      if (const uint8_t *mac = esp_bt_dev_get_address()) {
        uint8_t master[9] = {0xF5, 0x01, 0x00};
        memcpy(master + 3, mac, 6);
        this->input_->set_report(HIDReportKind::FEATURE, master, sizeof(master));
      }
#endif
      this->outputs(this->input_->get_gamepad_outputs());
    }
  }

  bool report(const uint8_t *d, size_t len) override {
    if (d[0] != 0x01)
      return false;
    // Over Bluetooth a report with 0xFF second and zeros after is not the pad's state.
    if (len < 20 || d[1] == 0xFF)
      return true;
    GamepadState s;
    static constexpr uint32_t B2[8] = {PAD_SELECT, PAD_L3, PAD_R3, PAD_START, 0, 0, 0, 0};
    static constexpr uint8_t D2[8] = {0, 0, 0, 0, DPAD_UP, DPAD_RIGHT, DPAD_DOWN, DPAD_LEFT};
    static constexpr uint32_t B3[8] = {PAD_L2, PAD_R2, PAD_L1, PAD_R1, PAD_NORTH, PAD_EAST, PAD_SOUTH, PAD_WEST};
    for (int i = 0; i < 8; i++) {
      if ((d[2] >> i) & 1) {
        s.buttons |= B2[i];
        s.dpad |= D2[i];
      }
      if ((d[3] >> i) & 1)
        s.buttons |= B3[i];
    }
    if (d[4] & 0x01)
      s.buttons |= PAD_HOME;
    s.axes[PAD_LX] = pad_stick(d[6], 0, 255);
    s.axes[PAD_LY] = pad_stick(d[7], 0, 255);
    s.axes[PAD_RX] = pad_stick(d[8], 0, 255);
    s.axes[PAD_RY] = pad_stick(d[9], 0, 255);
    s.axes[PAD_LT] = pad_trigger(d[18], 0, 255);
    s.axes[PAD_RT] = pad_trigger(d[19], 0, 255);
    this->input_->gamepad_state(s);
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    uint8_t r[36] = {
        0x01, 0x01, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27,
        0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    r[3] = out.weak != 0 ? 1 : 0;  // right (small) motor: on or off
    r[5] = out.strong;             // left (large) motor
    // LED n is bit n; none at all is 0x20.
    r[10] = out.player >= 1 && out.player <= 4 ? static_cast<uint8_t>(1u << out.player) : 0x20;
    this->input_->set_report(HIDReportKind::OUTPUT, r, sizeof(r));
  }
};

// -- PlayStation buttons (DualShock 4 and DualSense share them) ------------------------------

static void ps_buttons(GamepadState &s, const uint8_t *b) {
  s.dpad = pad_hat(b[0] & 0x0F);
  if (b[0] & 0x10)
    s.buttons |= PAD_WEST;
  if (b[0] & 0x20)
    s.buttons |= PAD_SOUTH;
  if (b[0] & 0x40)
    s.buttons |= PAD_EAST;
  if (b[0] & 0x80)
    s.buttons |= PAD_NORTH;
  static constexpr uint32_t B1[8] = {PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_SELECT, PAD_START, PAD_L3, PAD_R3};
  for (int i = 0; i < 8; i++) {
    if ((b[1] >> i) & 1)
      s.buttons |= B1[i];
  }
  if (b[2] & 0x01)
    s.buttons |= PAD_HOME;
  if (b[2] & 0x02)
    s.buttons |= PAD_TOUCHPAD;
}

// Two touch points of 4 bytes (hid-playstation.c): bit 7 of the first set while not touched, then
// x in 12 bits and y in 12 bits. Both pads are 1920 wide.
static void ps_touch(GamepadState &s, const uint8_t *p, uint16_t height) {
  s.touch_width = 1920;
  s.touch_height = height;
  for (uint8_t i = 0; i < PAD_TOUCH_COUNT; i++, p += 4) {
    s.touch[i].down = (p[0] & 0x80) == 0;
    s.touch[i].x = static_cast<uint16_t>(((p[2] & 0x0F) << 8) | p[1]);
    s.touch[i].y = static_cast<uint16_t>((p[3] << 4) | (p[2] >> 4));
  }
}

// The battery in a status byte (hid-playstation.c): DualSense, charging state in the high nibble.
static void ds_battery(HIDInput *input, uint8_t status) {
  const auto level = static_cast<int8_t>(std::min((status & 0x0F) * 10 + 5, 100));
  switch (status >> 4) {
    case 0x0:
      input->set_battery(level, HIDBattery::DISCHARGING);
      break;
    case 0x1:
      input->set_battery(level, HIDBattery::CHARGING);
      break;
    case 0x2:
      input->set_battery(100, HIDBattery::FULL);
      break;
    case 0xA:  // voltage or temperature out of range
    case 0xB:  // temperature error
      input->set_battery(0, HIDBattery::NOT_CHARGING);
      break;
    default:
      input->set_battery(-1, HIDBattery::UNKNOWN);
      break;
  }
}

// DualShock 4: bit 4 set while the cable is in; 11 is full then, 14 and 15 are errors.
static void ds4_battery(HIDInput *input, uint8_t status) {
  const uint8_t data = status & 0x0F;
  if ((status & 0x10) == 0) {
    input->set_battery(static_cast<int8_t>(data < 10 ? data * 10 + 5 : 100), HIDBattery::DISCHARGING);
  } else if (data <= 10) {
    input->set_battery(static_cast<int8_t>(data < 10 ? data * 10 + 5 : 100), HIDBattery::CHARGING);
  } else if (data == 11) {
    input->set_battery(100, HIDBattery::FULL);
  } else {
    input->set_battery(-1, HIDBattery::UNKNOWN);
  }
}

// -- DualShock 4 (hid-playstation.c) ---------------------------------------------------------

class DualShock4Driver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "DualShock 4"; }

  void start() override {
    // Over Bluetooth the pad sends the short report 0x01 until its calibration is read; the
    // firmware info follows it.
    if (this->input_->get_bus() != HIDBus::USB) {
      this->input_->request_feature_report(0x05, 41);
    } else {
      this->input_->request_feature_report(0xA3, 49);
    }
    this->outputs(this->input_->get_gamepad_outputs());
  }

  void feature(uint8_t id, const uint8_t *data, size_t len) override {
    if (id == 0x05) {
      this->input_->request_feature_report(0xA3, 49);
    } else if (id == 0xA3 && len >= 43) {
      char text[8];
      snprintf(text, sizeof(text), "%04X", le16(data + 41));
      this->input_->set_firmware(text);
    }
  }

  bool report(const uint8_t *d, size_t len) override {
    const uint8_t *c;
    if (d[0] == 0x01 && len >= 10) {
      c = d + 1;  // USB, and Bluetooth before the full report
    } else if (d[0] == 0x11 && len >= 12) {
      c = d + 3;
    } else {
      return d[0] == 0x01 || d[0] == 0x11;
    }
    GamepadState s;
    s.axes[PAD_LX] = pad_stick(c[0], 0, 255);
    s.axes[PAD_LY] = pad_stick(c[1], 0, 255);
    s.axes[PAD_RX] = pad_stick(c[2], 0, 255);
    s.axes[PAD_RY] = pad_stick(c[3], 0, 255);
    ps_buttons(s, c + 4);
    s.axes[PAD_LT] = pad_trigger(c[7], 0, 255);
    s.axes[PAD_RT] = pad_trigger(c[8], 0, 255);
    // After the 32 common bytes: the number of touch reports, then 9 bytes each (a timestamp, two
    // points); the last one is the latest.
    const size_t at = c - d;
    if (len >= at + 33 && c[32] >= 1 && len >= at + 33 + 9 * static_cast<size_t>(c[32])) {
      ps_touch(s, c + 33 + 9 * (c[32] - 1) + 1, 942);
    }
    if (len >= at + 30)
      ds4_battery(this->input_, c[29]);
    this->input_->gamepad_state(s);
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    // Some third-party pads expect rumble and light bar together; Linux always sends both.
    if (this->input_->get_bus() == HIDBus::USB) {
      uint8_t r[32] = {0x05, 0x03};
      r[4] = out.weak;
      r[5] = out.strong;
      r[6] = out.red;
      r[7] = out.green;
      r[8] = out.blue;
      this->input_->send_output_report(r, sizeof(r));
      return;
    }
    uint8_t r[78] = {0x11, 0x80 | 0x40 | 4};  // HID data, CRC, 4 ms poll interval
    r[3] = 0x03;
    r[6] = out.weak;
    r[7] = out.strong;
    r[8] = out.red;
    r[9] = out.green;
    r[10] = out.blue;
    put_crc(r, sizeof(r));
    this->input_->send_output_report(r, sizeof(r));
  }
};

// -- DualSense, DualSense Edge (hid-playstation.c) -------------------------------------------

class DualSenseDriver : public GamepadDriver {
 public:
  DualSenseDriver(HIDInput *input, bool edge) : GamepadDriver(input), vibration_v2_(edge) {}
  const char *name() const override { return "DualSense"; }

  void start() override {
    if (this->input_->get_bus() != HIDBus::USB) {
      // The full report 0x31 starts once the calibration has been read.
      this->input_->request_feature_report(0x05, 41);
      return;
    }
    this->input_->request_feature_report(0x20, 64);
  }

  void feature(uint8_t id, const uint8_t *data, size_t len) override {
    if (id == 0x05) {
      this->input_->request_feature_report(0x20, 64);
      return;
    }
    if (id != 0x20)
      return;
    if (len >= 32) {
      char text[12];
      snprintf(text, sizeof(text), "%08" PRIX32, le16(data + 28) | (static_cast<uint32_t>(le16(data + 30)) << 16));
      this->input_->set_firmware(text);
    }
    // Feature version 2.21 brought the vibration the Edge always has.
    if (len >= 46 && le16(data + 44) >= 0x0215)
      this->vibration_v2_ = true;
    // The light bar has to be set up before it can be programmed.
    uint8_t common[47] = {};
    common[38] = 0x02;  // valid_flag2: lightbar setup
    common[41] = 0x02;  // fade the light out
    this->send_(common);
    this->ready_ = true;
    this->outputs(this->input_->get_gamepad_outputs());
  }

  bool report(const uint8_t *d, size_t len) override {
    const uint8_t *r;
    if (this->input_->get_bus() == HIDBus::USB && d[0] == 0x01 && len >= 12) {
      r = d + 1;
    } else if (d[0] == 0x31 && len >= 13) {
      r = d + 2;
    } else {
      return d[0] == 0x01 || d[0] == 0x31;
    }
    GamepadState s;
    s.axes[PAD_LX] = pad_stick(r[0], 0, 255);
    s.axes[PAD_LY] = pad_stick(r[1], 0, 255);
    s.axes[PAD_RX] = pad_stick(r[2], 0, 255);
    s.axes[PAD_RY] = pad_stick(r[3], 0, 255);
    s.axes[PAD_LT] = pad_trigger(r[4], 0, 255);
    s.axes[PAD_RT] = pad_trigger(r[5], 0, 255);
    ps_buttons(s, r + 7);
    if (r[9] & 0x04)
      s.buttons |= PAD_MUTE;
    // Two touch points at offset 32 (hid-playstation.c dualsense_input_report).
    if (len >= static_cast<size_t>(r - d) + 40)
      ps_touch(s, r + 32, 1080);
    // The status byte after the touch points and 12 reserved bytes.
    if (len >= static_cast<size_t>(r - d) + 53)
      ds_battery(this->input_, r[52]);
    this->input_->gamepad_state(s);
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    if (!this->ready_)
      return;
    // Player numbers centered across the five LEDs, as on the console.
    static constexpr uint8_t PLAYERS[5] = {0x04, 0x0A, 0x15, 0x1B, 0x1F};
    uint8_t c[47] = {};
    c[0] = 0x02;  // haptics select: classic rumble
    if (this->vibration_v2_) {
      c[38] |= 0x04;
    } else {
      c[0] |= 0x01;
    }
    c[2] = out.weak;
    c[3] = out.strong;
    c[1] = 0x04 | 0x10;  // light bar, player LEDs
    c[43] = out.player >= 1 && out.player <= 5 ? PLAYERS[out.player - 1] : 0;
    c[44] = out.red;
    c[45] = out.green;
    c[46] = out.blue;
    this->send_(c);
  }

 protected:
  void send_(const uint8_t *common) {
    if (this->input_->get_bus() == HIDBus::USB) {
      uint8_t r[63] = {0x02};
      memcpy(r + 1, common, 47);
      this->input_->send_output_report(r, sizeof(r));
      return;
    }
    uint8_t r[78] = {0x31};
    r[1] = static_cast<uint8_t>(this->seq_ << 4);
    this->seq_ = (this->seq_ + 1) & 0x0F;
    r[2] = 0x10;
    memcpy(r + 3, common, 47);
    put_crc(r, sizeof(r));
    this->input_->send_output_report(r, sizeof(r));
  }

  bool vibration_v2_;
  bool ready_{false};
  uint8_t seq_{0};
};

// -- Switch Pro Controller (hid-nintendo.c) --------------------------------------------------

class SwitchProDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  ~SwitchProDriver() override { this->cancel_timeout_(); }
  const char *name() const override { return "Switch Pro Controller"; }

  void start() override {
    this->step_ = this->input_->get_bus() == HIDBus::USB ? USB_HANDSHAKE : MAGIC_LEFT;
    this->send_step_();
  }

  bool report(const uint8_t *d, size_t len) override {
    const uint8_t id = d[0];
    if (id == 0x81 && len >= 2 && this->step_ <= USB_HANDSHAKE2 && d[1] == USB_COMMANDS[this->step_]) {
      this->next_();
      return true;
    }
    if (id == 0x21 && len >= 15 && this->step_ >= MAGIC_LEFT && this->step_ < READY &&
        d[14] == this->subcommand_()) {
      this->reply_(d + 15, len - 15);
      this->next_();
    }
    if ((id == 0x30 || id == 0x31 || id == 0x21) && len >= 12) {
      if (this->step_ == READY)
        this->state_(d);
      return true;
    }
    return id == 0x3F || id == 0x21 || id == 0x81;
  }

  void outputs(const GamepadOutputs &out) override {
    if (this->step_ != READY)
      return;
    if (out.player != this->player_) {
      this->player_ = out.player;
      const uint8_t leds = this->leds_(out.player);
      this->subcommand_send_(0x30, &leds, 1);
    }
    uint8_t r[10] = {0x10, this->next_packet_()};
    this->rumble_(r + 2, out.strong, out.weak);
    this->input_->send_output_report(r, sizeof(r));
  }

 protected:
  enum Step : uint8_t {
    USB_HANDSHAKE,
    USB_BAUD,
    USB_HANDSHAKE2,
    MAGIC_LEFT,
    MAGIC_RIGHT,
    CAL_LEFT,
    CAL_RIGHT,
    REPORT_MODE,
    VIBRATION,
    PLAYER,
    READY,
  };
  static constexpr uint8_t USB_COMMANDS[3] = {0x02, 0x03, 0x02};
  struct Cal {
    int32_t center{2000}, min{500}, max{3500};
  };

  uint8_t subcommand_() const {
    switch (this->step_) {
      case MAGIC_LEFT:
      case MAGIC_RIGHT:
      case CAL_LEFT:
      case CAL_RIGHT:
        return 0x10;  // SPI flash read
      case REPORT_MODE:
        return 0x03;
      case VIBRATION:
        return 0x48;
      case PLAYER:
        return 0x30;
      default:
        return 0;
    }
  }

  void send_step_() {
    switch (this->step_) {
      case USB_HANDSHAKE:
      case USB_BAUD:
      case USB_HANDSHAKE2: {
        const uint8_t r[2] = {0x80, USB_COMMANDS[this->step_]};
        this->input_->send_output_report(r, sizeof(r));
        break;
      }
      case MAGIC_LEFT:
        this->spi_read_(0x8010, 2);
        break;
      case MAGIC_RIGHT:
        this->spi_read_(0x801B, 2);
        break;
      case CAL_LEFT:
        this->spi_read_(this->user_left_ ? 0x8012 : 0x603D, 9);
        break;
      case CAL_RIGHT:
        this->spi_read_(this->user_right_ ? 0x801D : 0x6046, 9);
        break;
      case REPORT_MODE: {
        const uint8_t mode = 0x30;
        this->subcommand_send_(0x03, &mode, 1);
        break;
      }
      case VIBRATION: {
        const uint8_t on = 0x01;
        this->subcommand_send_(0x48, &on, 1);
        break;
      }
      case PLAYER: {
        this->player_ = this->input_->get_gamepad_outputs().player;
        const uint8_t leds = this->leds_(this->player_);
        this->subcommand_send_(0x30, &leds, 1);
        break;
      }
      case READY:
        this->cancel_timeout_();
        ESP_LOGV(TAG, "Switch Pro Controller ready");
        return;
    }
    // A command the pad dropped is sent once more, then the step is given up.
    {
      // Keyed by this driver: several pads can sit on one component (usb_hid), the timer dies with it
      App.scheduler.set_timeout(this, 1000, [this]() {
        if (this->retried_) {
          ESP_LOGW(TAG, "Switch Pro Controller: no answer to init step %u", this->step_);
          this->next_();
          return;
        }
        this->retried_ = true;
        this->send_step_();
      });
    }
  }

  void next_() {
    if (this->step_ == USB_HANDSHAKE2) {
      // Keep it in USB mode; it does not answer this one.
      const uint8_t r[2] = {0x80, 0x04};
      this->input_->send_output_report(r, sizeof(r));
    }
    this->step_ = static_cast<Step>(this->step_ + 1);
    this->retried_ = false;
    this->send_step_();
  }

  void cancel_timeout_() { App.scheduler.cancel_timeout(this); }

  // Subcommand reply data: for an SPI read, address (4), size (1), then the bytes.
  void reply_(const uint8_t *data, size_t len) {
    if (this->subcommand_() != 0x10 || len < 5)
      return;
    const uint8_t *b = data + 5;
    const size_t n = std::min<size_t>(data[4], len - 5);
    switch (this->step_) {
      case MAGIC_LEFT:
        this->user_left_ = n >= 2 && b[0] == 0xB2 && b[1] == 0xA1;
        break;
      case MAGIC_RIGHT:
        this->user_right_ = n >= 2 && b[0] == 0xB2 && b[1] == 0xA1;
        break;
      case CAL_LEFT:
      case CAL_RIGHT: {
        if (n < 9)
          break;
        auto lo = [b](int i) { return static_cast<int32_t>(b[i] | ((b[i + 1] & 0x0F) << 8)); };
        auto hi = [b](int i) { return static_cast<int32_t>((b[i + 1] >> 4) | (b[i + 2] << 4)); };
        Cal x, y;
        if (this->step_ == CAL_LEFT) {
          x.center = lo(3);
          y.center = hi(3);
          x.max = x.center + lo(0);
          y.max = y.center + hi(0);
          x.min = x.center - lo(6);
          y.min = y.center - hi(6);
        } else {
          x.center = lo(0);
          y.center = hi(0);
          x.min = x.center - lo(3);
          y.min = y.center - hi(3);
          x.max = x.center + lo(6);
          y.max = y.center + hi(6);
        }
        if (x.min >= x.center || x.center >= x.max || y.min >= y.center || y.center >= y.max)
          break;  // implausible: keep the defaults
        Cal *cal = this->step_ == CAL_LEFT ? this->cal_ : this->cal_ + 2;
        cal[0] = x;
        cal[1] = y;
        break;
      }
      default:
        break;
    }
  }

  static int32_t stick_(const Cal &cal, int32_t v) {
    int32_t r = v > cal.center ? (v - cal.center) * 32767 / (cal.max - cal.center)
                               : (cal.center - v) * -32767 / (cal.center - cal.min);
    return std::clamp<int32_t>(r, -32767, 32767);
  }

  void state_(const uint8_t *d) {
    const uint32_t btn = d[3] | (d[4] << 8) | (d[5] << 16);
    static constexpr struct {
      uint32_t bit, button;
    } MAP[] = {
        {1u << 0, PAD_WEST},     {1u << 1, PAD_NORTH},  {1u << 2, PAD_SOUTH},  {1u << 3, PAD_EAST},
        {1u << 6, PAD_R1},       {1u << 7, PAD_R2},     {1u << 8, PAD_SELECT}, {1u << 9, PAD_START},
        {1u << 10, PAD_R3},      {1u << 11, PAD_L3},    {1u << 12, PAD_HOME},  {1u << 13, PAD_CAPTURE},
        {1u << 22, PAD_L1},      {1u << 23, PAD_L2},
    };
    GamepadState s;
    for (const auto &m : MAP) {
      if (btn & m.bit)
        s.buttons |= m.button;
    }
    if (btn & (1u << 16))
      s.dpad |= DPAD_DOWN;
    if (btn & (1u << 17))
      s.dpad |= DPAD_UP;
    if (btn & (1u << 18))
      s.dpad |= DPAD_RIGHT;
    if (btn & (1u << 19))
      s.dpad |= DPAD_LEFT;
    auto x = [](const uint8_t *p) { return static_cast<int32_t>(p[0] | ((p[1] & 0x0F) << 8)); };
    auto y = [](const uint8_t *p) { return static_cast<int32_t>((p[1] >> 4) | (p[2] << 4)); };
    s.axes[PAD_LX] = stick_(this->cal_[0], x(d + 6));
    s.axes[PAD_LY] = -stick_(this->cal_[1], y(d + 6));
    s.axes[PAD_RX] = stick_(this->cal_[2], x(d + 9));
    s.axes[PAD_RY] = -stick_(this->cal_[3], y(d + 9));
    this->input_->gamepad_state(s);
  }

  static uint8_t leds_(uint8_t player) {
    static constexpr uint8_t PATTERNS[4] = {0x1, 0x3, 0x7, 0xF};
    return player >= 1 && player <= 4 ? PATTERNS[player - 1] : 0;
  }

  uint8_t next_packet_() {
    const uint8_t n = this->packet_;
    this->packet_ = (this->packet_ + 1) & 0x0F;
    return n;
  }

  void spi_read_(uint32_t address, uint8_t size) {
    const uint8_t data[5] = {static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
                             static_cast<uint8_t>(address >> 16), static_cast<uint8_t>(address >> 24), size};
    this->subcommand_send_(0x10, data, sizeof(data));
  }

  void subcommand_send_(uint8_t id, const uint8_t *data, size_t len) {
    uint8_t r[16] = {0x01, this->next_packet_()};
    const GamepadOutputs &out = this->input_->get_gamepad_outputs();
    this->rumble_(r + 2, this->step_ == READY ? out.strong : 0, this->step_ == READY ? out.weak : 0);
    r[10] = id;
    memcpy(r + 11, data, std::min<size_t>(len, 5));
    this->input_->send_output_report(r, 11 + std::min<size_t>(len, 5));
  }

  // 160 Hz low and 320 Hz high band, amplitude from Linux's table (whose encoding steps by
  // one row per index): left (strong) motor in the first four bytes, right (weak) after.
  static void rumble_(uint8_t *r, uint8_t strong, uint8_t weak) {
    static constexpr uint16_t AMP[101] = {
        0,   10,  12,  14,  17,  20,  24,  28,  33,  40,  47,  56,  67,  80,  95,  112, 117, 123, 128, 134, 140,
        146, 152, 159, 166, 173, 181, 189, 198, 206, 215, 225, 230, 235, 240, 245, 251, 256, 262, 268, 273, 279,
        286, 292, 298, 305, 311, 318, 325, 332, 340, 347, 355, 362, 370, 378, 387, 395, 404, 413, 422, 431, 440,
        450, 460, 470, 480, 491, 501, 512, 524, 535, 547, 559, 571, 584, 596, 609, 623, 636, 650, 665, 679, 694,
        709, 725, 741, 757, 773, 790, 808, 825, 843, 862, 881, 900, 920, 940, 960, 981, 1003,
    };
    auto encode = [](uint8_t *p, uint8_t value) {
      const uint32_t amp = value * 257u * 1003u / 65535u;
      size_t i = 0;
      if (amp > AMP[0]) {
        for (i = 1; i < 100; i++) {
          if (amp > AMP[i - 1] && amp <= AMP[i])
            break;
        }
      }
      p[0] = 0x00;
      p[1] = static_cast<uint8_t>(0x01 + 2 * i);
      p[2] = static_cast<uint8_t>(0x40 + ((i & 1) ? 0x80 : 0x00));
      p[3] = static_cast<uint8_t>(0x40 + i / 2);
    };
    encode(r, strong);
    encode(r + 4, weak);
  }

  Step step_{READY};
  bool retried_{false};
  bool user_left_{false};
  bool user_right_{false};
  uint8_t packet_{0};
  uint8_t player_{0};
  Cal cal_[4];  // left x, left y, right x, right y
};

// Nintendo-layout HID pads (PowerA, HORI): buttons Y, B, A, X, L, R, ZL, ZR, -, +, LS, RS,
// Home, Capture, by position.
class SwitchLayoutDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "Nintendo-layout pad"; }
  bool report(const uint8_t *data, size_t len) override { return false; }
  uint16_t button(uint16_t usage) const override {
    static constexpr uint8_t MAP[14] = {3, 1, 2, 4, 5, 6, 12, 13, 7, 8, 9, 10, 11, 14};
    return usage >= 1 && usage <= 14 ? MAP[usage - 1] : usage;
  }
};

// -- Wii Remote (SDL_hidapi_wii.c), Bluetooth only -------------------------------------------

class WiimoteDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "Wii Remote"; }

  void start() override {
    this->leds_(this->input_->get_gamepad_outputs());
    this->report_mode_();
  }

  bool report(const uint8_t *d, size_t len) override {
    if (d[0] < 0x20)
      return false;
    // A status report resets the reporting mode.
    if (d[0] == 0x20)
      this->report_mode_();
    if (len < 3 || d[0] == 0x3D)
      return true;
    GamepadState s;
    static constexpr uint8_t D1[4] = {DPAD_LEFT, DPAD_RIGHT, DPAD_DOWN, DPAD_UP};
    for (int i = 0; i < 4; i++) {
      if ((d[1] >> i) & 1)
        s.dpad |= D1[i];
    }
    if (d[1] & 0x10)
      s.buttons |= PAD_START;  // +
    static constexpr uint32_t B2[8] = {PAD_NORTH, PAD_WEST, PAD_SOUTH, PAD_EAST, PAD_SELECT, 0, 0, PAD_HOME};
    for (int i = 0; i < 8; i++) {
      if ((d[2] >> i) & 1)
        s.buttons |= B2[i];
    }
    this->input_->gamepad_state(s);
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    const bool rumble = out.strong != 0 || out.weak != 0;
    if (rumble != this->rumble_) {
      this->rumble_ = rumble;
      const uint8_t r[2] = {0x10, static_cast<uint8_t>(rumble)};
      this->input_->send_output_report(r, sizeof(r));
    }
    if (out.player != this->player_)
      this->leds_(out);
  }

 protected:
  void report_mode_() {
    const uint8_t r[3] = {0x12, static_cast<uint8_t>(this->rumble_), 0x30};  // core buttons, on change
    this->input_->send_output_report(r, sizeof(r));
  }
  void leds_(const GamepadOutputs &out) {
    this->player_ = out.player;
    static constexpr uint8_t LEDS[4] = {0x10, 0x20, 0x40, 0x80};
    const uint8_t r[2] = {0x11, static_cast<uint8_t>((out.player >= 1 && out.player <= 4 ? LEDS[out.player - 1] : 0) |
                                                     static_cast<uint8_t>(this->rumble_))};
    this->input_->send_output_report(r, sizeof(r));
  }
  bool rumble_{false};
  uint8_t player_{0};
};

// -- Steam Controller (hid-steam.c; BLE from SDL_hidapi_steam.c) -----------------------------

class SteamDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "Steam Controller"; }

  void start() override { this->lizard_off_(); }

  bool report(const uint8_t *d, size_t len) override {
    if (this->input_->get_bus() == HIDBus::BLE)
      return this->ble_(d, len);
    if (len != 64 || d[0] != 0x01 || d[1] != 0x00)
      return false;
    if (d[2] == 0x03) {  // wireless: 0x02 connected
      if (d[4] == 0x02)
        this->lizard_off_();
      return true;
    }
    if (d[2] != 0x01)
      return true;
    this->buttons_ = d[8] | (d[9] << 8) | (d[10] << 16);
    // X/Y are the left pad while it is touched, unless both are used at once.
    const bool pad = (d[10] & 0x08) != 0;
    const bool both = (d[10] & 0x80) != 0;
    if (!pad) {
      this->lx_ = static_cast<int16_t>(le16(d + 16));
      this->ly_ = static_cast<int16_t>(le16(d + 18));
    } else if (!both) {
      this->lx_ = this->ly_ = 0;
    }
    this->rx_ = static_cast<int16_t>(le16(d + 20));
    this->ry_ = static_cast<int16_t>(le16(d + 22));
    this->lt_ = d[11];
    this->rt_ = d[12];
    this->publish_();
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    this->pulse_(0, out.strong);
    this->pulse_(1, out.weak);
  }

 protected:
  // BLE: report 0x03 in 20-byte segments (header 0x80 data, 0x40 last, low bits the number).
  bool ble_(const uint8_t *d, size_t len) {
    if (d[0] != 0x03)
      return false;
    if (len != 20 || !(d[1] & 0x80))
      return true;
    const uint8_t segment = d[1] & 0x07;
    if (segment != this->segment_) {
      this->segment_ = 0;
      if (segment != 0)
        return true;
    }
    memcpy(this->packet_ + segment * 18, d + 2, 18);
    if (!(d[1] & 0x40)) {
      this->segment_++;
      return true;
    }
    this->segment_ = 0;
    const uint8_t *p = this->packet_;
    const uint8_t *end = this->packet_ + (segment + 1) * 18;
    if ((p[0] & 0x0F) != 4)  // state
      return true;
    const uint32_t mask = (p[0] & 0xF0) | (p[1] << 8);
    p += 2;
    auto take = [&p, end](size_t n) {
      const uint8_t *at = p;
      p += n;
      return p <= end ? at : nullptr;
    };
    if (mask & 0x10) {
      if (const uint8_t *b = take(3))
        this->buttons_ = b[0] | (b[1] << 8) | (b[2] << 16);
    }
    if (mask & 0x20) {
      if (const uint8_t *t = take(2)) {
        this->lt_ = t[0];
        this->rt_ = t[1];
      }
    }
    if (mask & 0x40)
      take(3);
    if (mask & 0x80) {
      if (const uint8_t *s = take(4)) {
        this->lx_ = static_cast<int16_t>(le16(s));
        this->ly_ = static_cast<int16_t>(le16(s + 2));
      }
    }
    if (mask & 0x100)
      take(4);
    if (mask & 0x200) {
      if (const uint8_t *s = take(4)) {
        this->rx_ = static_cast<int16_t>(le16(s));
        this->ry_ = static_cast<int16_t>(le16(s + 2));
      }
    }
    this->publish_();
    return true;
  }

  void publish_() {
    static constexpr struct {
      uint32_t bit, button;
    } MAP[] = {
        {1u << 0, PAD_R2},      {1u << 1, PAD_L2},        {1u << 2, PAD_R1},      {1u << 3, PAD_L1},
        {1u << 4, PAD_NORTH},   {1u << 5, PAD_EAST},      {1u << 6, PAD_WEST},    {1u << 7, PAD_SOUTH},
        {1u << 12, PAD_SELECT}, {1u << 13, PAD_HOME},     {1u << 14, PAD_START},  {1u << 15, PAD_PADDLE1},
        {1u << 16, PAD_PADDLE2}, {1u << 17, PAD_TOUCHPAD}, {1u << 18, PAD_R3},     {1u << 22, PAD_L3},
    };
    GamepadState s;
    for (const auto &m : MAP) {
      if (this->buttons_ & m.bit)
        s.buttons |= m.button;
    }
    if (this->buttons_ & (1u << 8))
      s.dpad |= DPAD_UP;
    if (this->buttons_ & (1u << 9))
      s.dpad |= DPAD_RIGHT;
    if (this->buttons_ & (1u << 10))
      s.dpad |= DPAD_LEFT;
    if (this->buttons_ & (1u << 11))
      s.dpad |= DPAD_DOWN;
    s.axes[PAD_LX] = this->lx_;
    s.axes[PAD_LY] = neg16(this->ly_);
    s.axes[PAD_RX] = this->rx_;
    s.axes[PAD_RY] = neg16(this->ry_);
    // Full travel is 26000 of (v << 7 | v), as SDL scales it.
    s.axes[PAD_LT] = pad_trigger(std::min<int32_t>(this->lt_ * 129, 26000), 0, 26000);
    s.axes[PAD_RT] = pad_trigger(std::min<int32_t>(this->rt_ * 129, 26000), 0, 26000);
    this->input_->gamepad_state(s);
  }

  // Lizard mode off: no keyboard keys, no mouse on either trackpad.
  void lizard_off_() {
    static const uint8_t CLEAR[] = {0x81};
    static const uint8_t SETTINGS[] = {0x87, 0x06, 0x07, 0x07, 0x00, 0x08, 0x07, 0x00};
    this->feature_(CLEAR, sizeof(CLEAR));
    this->feature_(SETTINGS, sizeof(SETTINGS));
  }

  // Rumble through the haptic pulse of one trackpad (0 left, 1 right): a 4 ms period, on for
  // the share of it the value gives, repeated until the next one.
  void pulse_(uint8_t pad, uint8_t value) {
    const uint16_t on = static_cast<uint16_t>(value * 4000u / 255u);
    const uint16_t off = static_cast<uint16_t>(4000u - on);
    const uint16_t count = value != 0 ? 0xFFFF : 0;
    const uint8_t cmd[10] = {0x8F,
                             0x08,
                             pad,
                             static_cast<uint8_t>(on),
                             static_cast<uint8_t>(on >> 8),
                             static_cast<uint8_t>(off),
                             static_cast<uint8_t>(off >> 8),
                             static_cast<uint8_t>(count),
                             static_cast<uint8_t>(count >> 8),
                             0};
    this->feature_(cmd, sizeof(cmd));
  }

  // A command as feature report: 64 bytes on USB; segmented report 0x03 over BLE.
  void feature_(const uint8_t *cmd, size_t len) {
    if (this->input_->get_bus() != HIDBus::BLE) {
      uint8_t r[64] = {};
      memcpy(r, cmd, std::min<size_t>(len, sizeof(r)));
      this->input_->set_report(HIDReportKind::FEATURE, r, sizeof(r));
      return;
    }
    uint8_t segment = 0;
    while (len > 0) {
      const size_t n = std::min<size_t>(len, 18);
      len -= n;
      uint8_t r[20] = {0x03, static_cast<uint8_t>(0x80 | segment | (len == 0 ? 0x40 : 0))};
      memcpy(r + 2, cmd, n);
      cmd += n;
      segment++;
      this->input_->set_report(HIDReportKind::FEATURE, r, sizeof(r));
    }
  }

  uint8_t packet_[18 * 8]{};
  uint8_t segment_{0};
  uint32_t buttons_{0};
  int32_t lx_{0}, ly_{0}, rx_{0}, ry_{0};
  int32_t lt_{0}, rt_{0};
};

// -- Stadia (hid-google-stadiaff.c): the descriptor reads it; rumble is report 5 -------------

class StadiaDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "Stadia Controller"; }
  bool report(const uint8_t *data, size_t len) override { return false; }
  void outputs(const GamepadOutputs &out) override {
    const uint16_t strong = out.strong * 257;
    const uint16_t weak = out.weak * 257;
    const uint8_t r[5] = {0x05, static_cast<uint8_t>(strong), static_cast<uint8_t>(strong >> 8),
                          static_cast<uint8_t>(weak), static_cast<uint8_t>(weak >> 8)};
    send_out(this->input_, r, sizeof(r));
  }
};

// -- Xbox One S / Series / Elite 2 over Bluetooth (SDL_hidapi_xboxone.c) --------------------

class XboxBluetoothDriver : public GamepadDriver {
 public:
  XboxBluetoothDriver(HIDInput *input, uint16_t pid)
      : GamepadDriver(input), share_(pid == 0x0B13), paddles_(pid == 0x0B05 || pid == 0x0B22) {}
  const char *name() const override { return "Xbox Wireless Controller"; }

  bool report(const uint8_t *d, size_t len) override {
    if (d[0] == 0x02) {  // separate guide button report
      this->guide_report_ = true;
      this->guide_ = len >= 2 && (d[1] & 0x01);
      this->publish_();
      return true;
    }
    if (d[0] == 0x04)
      return true;
    if (d[0] != 0x01 || len < 16)
      return false;
    GamepadState s;
    if (len == 16) {
      static constexpr uint32_t B14[8] = {PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH,
                                          PAD_L1,    PAD_R1,   PAD_SELECT, PAD_START};
      for (int i = 0; i < 8; i++) {
        if ((d[14] >> i) & 1)
          s.buttons |= B14[i];
      }
      if (d[15] & 0x01)
        s.buttons |= PAD_L3;
      if (d[15] & 0x02)
        s.buttons |= PAD_R3;
    } else {
      static constexpr struct {
        uint8_t mask;
        uint32_t button;
      } B14[] = {{0x01, PAD_SOUTH}, {0x02, PAD_EAST}, {0x08, PAD_WEST},
                 {0x10, PAD_NORTH}, {0x40, PAD_L1},   {0x80, PAD_R1}};
      for (const auto &b : B14) {
        if (d[14] & b.mask)
          s.buttons |= b.button;
      }
      if (d[15] & 0x08)
        s.buttons |= PAD_START;
      if (d[15] & 0x20)
        s.buttons |= PAD_L3;
      if (d[15] & 0x40)
        s.buttons |= PAD_R3;
      if (d[15] & 0x10)
        s.buttons |= PAD_HOME;
      if (this->share_) {
        if (d[15] & 0x04)
          s.buttons |= PAD_SELECT;
        if (len > 16 && (d[16] & 0x01))
          s.buttons |= PAD_CAPTURE;
      } else if ((d[15] & 0x04) || (len > 16 && (d[16] & 0x01))) {
        s.buttons |= PAD_SELECT;
      }
      // Paddles, by the firmware's report size; not reported while mapped to other buttons.
      if (this->paddles_ && (len == 20 || len == 39 || len == 55)) {
        const size_t at = len == 55 ? 33 : len == 39 ? 17 : 19;
        const bool mapped = len == 55 ? d[35] != 0 : len == 39 ? d[19] != 0 : d[17] != 0;
        if (!mapped) {
          static constexpr uint32_t P[4] = {PAD_PADDLE1, PAD_PADDLE2, PAD_PADDLE3, PAD_PADDLE4};
          for (int i = 0; i < 4; i++) {
            if ((d[at] >> i) & 1)
              s.buttons |= P[i];
          }
        }
      }
    }
    s.dpad = pad_hat(static_cast<int32_t>(d[13]) - 1);
    s.axes[PAD_LT] = pad_trigger(le16(d + 9), 0, 1023);
    s.axes[PAD_RT] = pad_trigger(le16(d + 11), 0, 1023);
    s.axes[PAD_LX] = static_cast<int32_t>(le16(d + 1)) - 0x8000;
    s.axes[PAD_LY] = static_cast<int32_t>(le16(d + 3)) - 0x8000;
    s.axes[PAD_RX] = static_cast<int32_t>(le16(d + 5)) - 0x8000;
    s.axes[PAD_RY] = static_cast<int32_t>(le16(d + 7)) - 0x8000;
    this->state_ = s;
    this->publish_();
    return true;
  }

  void outputs(const GamepadOutputs &out) override {
    // Magnitudes 0..100: triggers, left (strong), right (weak); on 255, off 0, repeat 235.
    const uint8_t r[9] = {0x03, 0x0F, 0, 0, static_cast<uint8_t>(out.strong * 100 / 255),
                          static_cast<uint8_t>(out.weak * 100 / 255), 0xFF, 0x00, 0xEB};
    this->input_->send_output_report(r, sizeof(r));
  }

 protected:
  // Guide comes in a report of its own on firmware that has one.
  void publish_() {
    GamepadState s = this->state_;
    if (this->guide_report_)
      s.buttons = this->guide_ ? (s.buttons | PAD_HOME) : (s.buttons & ~PAD_HOME);
    this->input_->gamepad_state(s);
  }
  GamepadState state_;
  bool share_;
  bool paddles_;
  bool guide_report_{false};
  bool guide_{false};
};

// -- Logitech Unifying / Nano receivers -------------------------------------------------------
// Keyboard and mouse come as plain HID reports; HID++ and DJ messages (reports 0x10, 0x11,
// 0x20, 0x21 on the receiver's vendor interface) carry no input and are kept out of the
// controls.

class LogitechReceiverDriver : public GamepadDriver {
 public:
  using GamepadDriver::GamepadDriver;
  const char *name() const override { return "Logitech receiver"; }
  bool report(const uint8_t *data, size_t len) override {
    return data[0] == 0x10 || data[0] == 0x11 || data[0] == 0x20 || data[0] == 0x21;
  }
};

// -- Which driver ----------------------------------------------------------------------------

std::unique_ptr<GamepadDriver> make_gamepad_driver(HIDInput *input, HIDBus bus, uint16_t vid, uint16_t pid,
                                                   uint16_t version) {
  switch (vid) {
    case 0x054C:  // Sony
      if (pid == 0x0268 || pid == 0x042F)
        return std::make_unique<SixaxisDriver>(input);
      if (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0)
        return std::make_unique<DualShock4Driver>(input);
      if (pid == 0x0CE6 || pid == 0x0DF2)
        return std::make_unique<DualSenseDriver>(input, pid == 0x0DF2);
      break;
    case 0x057E:  // Nintendo
      if (pid == 0x2009)
        return std::make_unique<SwitchProDriver>(input);
      if (pid == 0x0306 || pid == 0x0330)
        return std::make_unique<WiimoteDriver>(input);
      break;
    case 0x28DE:  // Valve
      if (pid == 0x1102 || pid == 0x1142 || pid == 0x1106)
        return std::make_unique<SteamDriver>(input);
      break;
    case 0x18D1:  // Google
      if (pid == 0x9400)
        return std::make_unique<StadiaDriver>(input);
      break;
    case 0x045E:  // Microsoft, over Bluetooth
      if (bus != HIDBus::USB && (pid == 0x02E0 || pid == 0x02FD || pid == 0x0B05 || pid == 0x0B0C ||
                                 pid == 0x0B13 || pid == 0x0B20 || pid == 0x0B21 || pid == 0x0B22))
        return std::make_unique<XboxBluetoothDriver>(input, pid);
      break;
    case 0x046D:  // Logitech
      if (pid == 0xC52B || pid == 0xC532 || pid == 0xC52F || pid == 0xC534 || pid == 0xC539 || pid == 0xC53F ||
          pid == 0xC548 || pid == 0x4024 || pid == 0x4025)
        return std::make_unique<LogitechReceiverDriver>(input);
      break;
    case 0x20D6:  // PowerA
      if (pid == 0xA713)
        return std::make_unique<SwitchLayoutDriver>(input);
      break;
    default:
      break;
  }
  return nullptr;
}

// The drivers' constructors only keep the input, so one made without it answers the question; it is
// dropped again at once (no timer of it started: that happens in start()).
bool has_gamepad_driver(HIDBus bus, uint16_t vid, uint16_t pid) {
  return make_gamepad_driver(nullptr, bus, vid, pid, 0) != nullptr;
}

}  // namespace esphome::hid
