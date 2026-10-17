#include "xbox.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include <cstring>

namespace esphome::usb_hid {

using namespace hid;

static inline uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
// Xbox sticks are up positive; the canonical layout is down positive (xpad: ~value).
static inline int32_t down(const uint8_t *p) { return ~static_cast<int32_t>(static_cast<int16_t>(le16(p))); }

// -- Xbox 360 --------------------------------------------------------------------------------

void Xbox360Driver::start() {
  if (!this->wireless_)
    this->outputs(this->input_->get_gamepad_outputs());
}

bool Xbox360Driver::report(const uint8_t *d, size_t len) {
  if (this->wireless_) {
    // Presence of the pad on the receiver.
    if (len >= 2 && (d[0] & 0x08)) {
      const bool present = (d[1] & 0x80) != 0;
      if (present && !this->present_) {
        this->present_ = true;
        this->player_ = 0xFF;
        this->outputs(this->input_->get_gamepad_outputs());
      } else if (!present && this->present_) {
        this->present_ = false;
        this->input_->gamepad_state(GamepadState{});
      }
      return true;
    }
    if (len < 4 + 14 || d[1] != 0x01)
      return true;
    d += 4;
    len -= 4;
  }
  if (len < 14 || d[0] != 0x00)
    return true;
  GamepadState s;
  static constexpr uint8_t D2[4] = {DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT};
  static constexpr uint32_t B2[4] = {PAD_START, PAD_SELECT, PAD_L3, PAD_R3};
  static constexpr uint32_t B3[8] = {PAD_L1, PAD_R1, PAD_HOME, 0, PAD_SOUTH, PAD_EAST, PAD_WEST, PAD_NORTH};
  for (int i = 0; i < 4; i++) {
    if ((d[2] >> i) & 1)
      s.dpad |= D2[i];
    if ((d[2] >> (i + 4)) & 1)
      s.buttons |= B2[i];
  }
  for (int i = 0; i < 8; i++) {
    if ((d[3] >> i) & 1)
      s.buttons |= B3[i];
  }
  s.axes[PAD_LT] = pad_trigger(d[4], 0, 255);
  s.axes[PAD_RT] = pad_trigger(d[5], 0, 255);
  s.axes[PAD_LX] = static_cast<int16_t>(le16(d + 6));
  s.axes[PAD_LY] = down(d + 8);
  s.axes[PAD_RX] = static_cast<int16_t>(le16(d + 10));
  s.axes[PAD_RY] = down(d + 12);
  this->input_->gamepad_state(s);
  return true;
}

void Xbox360Driver::outputs(const GamepadOutputs &out) {
  if (this->wireless_ && !this->present_)
    return;
  if (out.player != this->player_)
    this->leds_(out.player);
  if (this->wireless_) {
    const uint8_t r[12] = {0x00, 0x01, 0x0F, 0xC0, 0x00, out.strong, out.weak};
    this->input_->send_output_report(r, sizeof(r));
  } else {
    const uint8_t r[8] = {0x00, 0x08, 0x00, out.strong, out.weak};
    this->input_->send_output_report(r, sizeof(r));
  }
}

// LED ring: 0 off, 6..9 quadrant 1..4 on.
void Xbox360Driver::leds_(uint8_t player) {
  this->player_ = player;
  const uint8_t command = player >= 1 && player <= 4 ? 5 + player : 0;
  if (this->wireless_) {
    const uint8_t r[12] = {0x00, 0x00, 0x08, static_cast<uint8_t>(0x40 + command)};
    this->input_->send_output_report(r, sizeof(r));
  } else {
    const uint8_t r[3] = {0x01, 0x03, command};
    this->input_->send_output_report(r, sizeof(r));
  }
}

// -- Xbox One / Series -----------------------------------------------------------------------

static constexpr uint8_t GIP_CMD_ACK = 0x01;
static constexpr uint8_t GIP_CMD_ANNOUNCE = 0x02;
static constexpr uint8_t GIP_CMD_VIRTUAL_KEY = 0x07;
static constexpr uint8_t GIP_CMD_FIRMWARE = 0x0C;
static constexpr uint8_t GIP_CMD_INPUT = 0x20;
static constexpr uint8_t GIP_OPT_ACK = 0x10;
static constexpr uint8_t GIP_OPT_INTERNAL = 0x20;

struct VidPid {
  uint16_t vid, pid;
};
// Share button 18 bytes from the end of the input packet (xpad MAP_SHARE_BUTTON).
static constexpr VidPid SHARE_18[] = {
    {0x03f0, 0x08B6}, {0x0b05, 0x1a38}, {0x0b05, 0x1c96}, {0x0b05, 0x1d04}, {0x0f0d, 0x01b2},
    {0x10f5, 0x7008}, {0x10f5, 0x7073}, {0x20d6, 0x2064}, {0x20d6, 0x400b}, {0x20d6, 0x890b},
    {0x2dc8, 0x200f}, {0x2dc8, 0x201e}, {0x2e24, 0x0423}, {0x2e95, 0x0504}, {0x366c, 0x0005},
};

XboxOneDriver::XboxOneDriver(HIDInput *input, uint16_t vid, uint16_t pid, uint16_t bcd)
    : GamepadDriver(input), vid_(vid), pid_(pid), bcd_(bcd) {
  if (vid == 0x045e && pid == 0x0b12)
    this->share_offset_ = 26;
  for (const auto &v : SHARE_18) {
    if (v.vid == vid && v.pid == pid)
      this->share_offset_ = 18;
  }
  // Dongles that drop init packets sent before the pad announced itself.
  this->delay_init_ = vid == 0x366c && pid == 0x0005;
}

void XboxOneDriver::start() {
  if (!this->delay_init_)
    this->init_();
}

void XboxOneDriver::send_(uint8_t *packet, size_t len) {
  packet[2] = this->seq_++;
  this->input_->send_output_report(packet, len);
}

// xpad's init packets, in its order, each to the pads it names.
void XboxOneDriver::init_() {
  this->initialized_ = true;
  const bool ms = this->vid_ == 0x045e;
  if ((this->vid_ == 0x0e6f && this->pid_ == 0x0165) || (this->vid_ == 0x0f0d && this->pid_ == 0x0067)) {
    uint8_t hori_ack[] = {GIP_CMD_ACK, GIP_OPT_INTERNAL, 0, 9, 0x00, 0x04, GIP_OPT_INTERNAL, 0x3a, 0, 0, 0, 0x80, 0};
    this->send_(hori_ack, sizeof(hori_ack));
  }
  uint8_t power_on[] = {0x05, GIP_OPT_INTERNAL, 0, 1, 0x00};
  this->send_(power_on, sizeof(power_on));
  if (ms && (this->pid_ == 0x02ea || this->pid_ == 0x0b00)) {
    uint8_t s_init[] = {0x05, GIP_OPT_INTERNAL, 0, 0x0f, 0x06};
    this->send_(s_init, sizeof(s_init));
  }
  if (ms && this->pid_ == 0x0b00) {
    uint8_t extra_input[] = {0x4d, 0x10, 0x01, 0x02, 0x07, 0x00};
    this->send_(extra_input, sizeof(extra_input));
  }
  uint8_t led_on[] = {0x0a, GIP_OPT_INTERNAL, 0, 3, 0x00, 0x01, 0x14};
  this->send_(led_on, sizeof(led_on));
  uint8_t auth_done[] = {0x06, GIP_OPT_INTERNAL, 0, 2, 0x01, 0x00};
  this->send_(auth_done, sizeof(auth_done));
  if (this->vid_ == 0x24c6 && (this->pid_ == 0x541a || this->pid_ == 0x542a || this->pid_ == 0x543a)) {
    // PowerA pads start reporting after a rumble; the second one stops it at once.
    uint8_t begin[] = {0x09, 0x00, 0, 9, 0x00, 0x0F, 0x00, 0x00, 0x1D, 0x1D, 0xFF, 0x00, 0x00};
    this->send_(begin, sizeof(begin));
    uint8_t end[] = {0x09, 0x00, 0, 9, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    this->send_(end, sizeof(end));
  }
}

bool XboxOneDriver::report(const uint8_t *d, size_t len) {
  if (len < 4)
    return true;
  switch (d[0]) {
    case GIP_CMD_ANNOUNCE:
      if (this->delay_init_ && !this->initialized_)
        this->init_();
      return true;
    case GIP_CMD_VIRTUAL_KEY: {
      // The pad repeats it until it is acknowledged.
      if (d[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
        uint8_t ack[] = {GIP_CMD_ACK, GIP_OPT_INTERNAL, 0, 9, 0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL, 0x02,
                         0,           0,                0, 0, 0};
        ack[2] = d[2];
        this->input_->send_output_report(ack, sizeof(ack));
      }
      if (len >= 5)
        this->guide_ = (d[4] & 0x03) != 0;
      this->publish_();
      return true;
    }
    case GIP_CMD_FIRMWARE:
      // Elite 2 with 5.11 firmware: paddles in a packet of their own, muted in a custom profile.
      if (this->vid_ == 0x045e && this->pid_ == 0x0b00 && this->bcd_ >= 0x050b && len >= 20) {
        this->paddles_ = d[19] != 0 ? 0 : d[18];
        this->publish_();
      }
      return true;
    case GIP_CMD_INPUT:
      break;
    default:
      return true;
  }
  if (len < 18)
    return true;
  GamepadState s;
  static constexpr uint32_t B4[4] = {PAD_START, PAD_SELECT, PAD_SOUTH, PAD_EAST};
  for (int i = 0; i < 4; i++) {
    if ((d[4] >> (i + 2)) & 1)
      s.buttons |= B4[i];
  }
  if (d[4] & 0x40)
    s.buttons |= PAD_WEST;
  if (d[4] & 0x80)
    s.buttons |= PAD_NORTH;
  static constexpr uint8_t D5[4] = {DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT};
  static constexpr uint32_t B5[4] = {PAD_L1, PAD_R1, PAD_L3, PAD_R3};
  for (int i = 0; i < 4; i++) {
    if ((d[5] >> i) & 1)
      s.dpad |= D5[i];
    if ((d[5] >> (i + 4)) & 1)
      s.buttons |= B5[i];
  }
  if (this->share_offset_ != 0 && len >= this->share_offset_ && (d[len - this->share_offset_] & 0x01))
    s.buttons |= PAD_CAPTURE;
  s.axes[PAD_LT] = pad_trigger(le16(d + 6), 0, 1023);
  s.axes[PAD_RT] = pad_trigger(le16(d + 8), 0, 1023);
  s.axes[PAD_LX] = static_cast<int16_t>(le16(d + 10));
  s.axes[PAD_LY] = down(d + 12);
  s.axes[PAD_RX] = static_cast<int16_t>(le16(d + 14));
  s.axes[PAD_RY] = down(d + 16);
  // Paddles (bits GRIPR, GRIPR2, GRIPL, GRIPL2 as xpad names them), not passed on while the
  // pad maps them to other buttons.
  if (this->vid_ == 0x045e && this->pid_ == 0x02e3 && len >= 33) {
    const uint8_t p = memcmp(d + 4, d + 18, 2) != 0 ? 0 : d[32];
    this->paddles_ = static_cast<uint8_t>(((p >> 1) & 1) | (((p >> 3) & 1) << 1) | ((p & 1) << 2) |
                                          (((p >> 2) & 1) << 3));
  } else if (this->vid_ == 0x045e && this->pid_ == 0x0b00 && this->bcd_ < 0x0500 && len >= 20) {
    this->paddles_ = d[19] != 0 ? 0 : d[18];
  } else if (this->vid_ == 0x045e && this->pid_ == 0x0b00 && this->bcd_ < 0x050b && len >= 24) {
    this->paddles_ = d[23] != 0 ? 0 : d[22];
  }
  this->state_ = s;
  this->publish_();
  return true;
}

void XboxOneDriver::publish_() {
  GamepadState s = this->state_;
  if (this->guide_)
    s.buttons |= PAD_HOME;
  static constexpr uint32_t P[4] = {PAD_PADDLE1, PAD_PADDLE2, PAD_PADDLE3, PAD_PADDLE4};
  for (int i = 0; i < 4; i++) {
    if ((this->paddles_ >> i) & 1)
      s.buttons |= P[i];
  }
  this->input_->gamepad_state(s);
}

// Both motors; magnitudes 0..127 as xpad scales them, the trigger motors off.
void XboxOneDriver::outputs(const GamepadOutputs &out) {
  if (!this->initialized_)
    return;
  uint8_t r[13] = {0x09, 0x00, 0, 9, 0x00, 0x0F, 0x00, 0x00,
                   static_cast<uint8_t>(out.strong >> 1), static_cast<uint8_t>(out.weak >> 1), 0xFF, 0x00, 0xFF};
  this->send_(r, sizeof(r));
}

}  // namespace esphome::usb_hid

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
