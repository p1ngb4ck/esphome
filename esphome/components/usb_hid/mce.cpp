#include "mce.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_hid.h"

#include "esphome/core/application.h"

#include <vector>

namespace esphome::usb_hid {

using F = MCEReceiverDriver::Flags;

// mceusb.c's device table: GEN1 the original Microsoft receiver, GEN3 no gen2 init,
// INTERFACE_2 the IR on interface 2 of a multi-function device.
const MCEReceiverModel MCE_RECEIVERS[] = {
    {0x045e, 0x006d, F::GEN1}, {0x0471, 0x0608, 0}, {0x0471, 0x060c, 0}, {0x0471, 0x060d, 0},
    {0x0471, 0x060f, 0},       {0x0471, 0x0613, 0}, {0x0471, 0x0815, 0}, {0x0471, 0x206c, 0},
    {0x0471, 0x2088, 0},       {0x0471, 0x2093, 0}, {0x0bda, 0x0161, F::INTERFACE_2},
    {0x0609, 0x031d, 0},       {0x0609, 0x0322, 0}, {0x0609, 0x0334, 0}, {0x0609, 0x0338, 0},
    {0x0609, 0x0353, 0},       {0x0609, 0x0357, 0}, {0x1460, 0x9150, 0}, {0x1308, 0xc001, 0},
    {0x051c, 0xc001, 0},       {0x107b, 0x3009, 0}, {0x03ee, 0x2501, 0}, {0x1784, 0x0001, 0},
    {0x1784, 0x0006, 0},       {0x1784, 0x0007, 0}, {0x1784, 0x0008, F::GEN3}, {0x1784, 0x000a, 0},
    {0x1784, 0x0011, F::GEN3}, {0x179d, 0x0010, 0}, {0x195d, 0x7002, 0}, {0x1509, 0x9242, 0},
    {0x043e, 0x9803, 0},       {0x045e, 0x00a0, 0}, {0x147a, 0xe015, 0}, {0x147a, 0xe016, 0},
    {0x147a, 0xe017, 0},       {0x147a, 0xe018, 0}, {0x147a, 0xe03a, 0}, {0x147a, 0xe03c, 0},
    {0x147a, 0xe03e, 0},       {0x147a, 0xe042, 0}, {0x1934, 0x5168, 0}, {0x1934, 0x0602, 0},
    {0x1934, 0x0702, 0},       {0x2304, 0x0225, F::GEN3}, {0x1019, 0x0f38, 0}, {0x0fb8, 0x0002, 0},
    {0x185b, 0x3020, 0},       {0x185b, 0x3082, 0}, {0x04eb, 0xe004, 0}, {0x04eb, 0xe033, 0},
    {0x105a, 0x2000, 0},       {0x0572, 0x58a1, 0}, {0x0572, 0x58a5, 0}, {0x2596, 0x8008, 0},
    {0x2596, 0x8016, 0},       {0x2596, 0x8042, 0}, {0x2040, 0xb130, 0}, {0x2040, 0xb131, 0},
    {0x2040, 0xb138, 0},       {0x2040, 0xb139, 0}, {0x2040, 0xb151, 0}, {0x2040, 0xb123, 0},
    {0x2040, 0xb150, 0},       {0x2013, 0x0259, 0}, {0x2013, 0x025e, 0}, {0x03f3, 0x0094, 0},
    {0x1b80, 0xd3b2, 0},       {0x15f4, 0x0135, 0},
};
const size_t MCE_RECEIVER_COUNT = sizeof(MCE_RECEIVERS) / sizeof(MCE_RECEIVERS[0]);

uint8_t MCEReceiverDriver::model(uint16_t vid, uint16_t pid) {
  for (const auto &m : MCE_RECEIVERS) {
    if (m.vid == vid && m.pid == pid)
      return m.flags | KNOWN;
  }
  return 0;
}

static constexpr uint8_t MCE_CMD_NULL = 0x00;
static constexpr uint8_t MCE_CMD_PORT_IR = 0x9f;
static constexpr uint8_t MCE_CMD_PORT_SYS = 0xff;
static constexpr uint8_t MCE_COMMAND_IRDATA = 0x80;
static constexpr uint8_t MCE_PORT_MASK = 0xe0;
static constexpr uint8_t MCE_PACKET_LENGTH_MASK = 0x1f;
static constexpr uint32_t MCE_TIME_UNIT = 50;  // us
static constexpr uint32_t RX_TIMEOUT_US = 100000;
static constexpr uint32_t KEY_UP_MS = 250;
static const char *const KEY_UP = "mce_key_up";

// Length of the data of a command response (mceusb_cmd_datasize()).
static uint8_t datasize(uint8_t cmd, uint8_t subcmd) {
  switch (cmd) {
    case MCE_CMD_NULL:
      return subcmd == MCE_CMD_PORT_SYS ? 1 : 0;
    case MCE_CMD_PORT_SYS:
      switch (subcmd) {
        case 0x11:  // port status
          return 5;
        case 0x18:  // wake version
        case 0x0b:  // revision
          return 4;
        case 0x20:  // wake support
        case 0x17:  // wake source
        case 0x21:  // device details
        case 0x22:  // emulator version
          return 1;
        default:
          return 0;
      }
    case MCE_CMD_PORT_IR:
      switch (subcmd) {
        case 0x04:  // unknown
        case 0x06:  // carrier
        case 0x0c:  // timeout
        case 0x15:  // carrier count
        case 0x16:  // number of ports
          return 2;
        case 0x01:  // signal end
        case 0x08:  // tx ports
        case 0x14:  // rx ports
          return 1;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

MCEReceiverDriver::MCEReceiverDriver(USBHIDClient *client, uint8_t flags)
    : GamepadDriver(client), client_(client), flags_(flags) {}

MCEReceiverDriver::~MCEReceiverDriver() { App.scheduler.cancel_timeout(this->client_, KEY_UP); }

void MCEReceiverDriver::command_(const uint8_t *cmd, size_t len) { this->input_->send_output_report(cmd, len); }

// mceusb_dev_probe(): emulator version, the generation's init, the parameters.
void MCEReceiverDriver::start() {
  static const uint8_t GET_EMVER[] = {MCE_CMD_PORT_SYS, 0x22};
  static const uint8_t DEVICE_RESUME[] = {MCE_CMD_NULL, MCE_CMD_PORT_SYS, 0xaa};
  this->command_(GET_EMVER, sizeof(GET_EMVER));
  if (this->flags_ & GEN1) {
    // What Windows sends the original receiver: a vendor "set address" read, 38400 bps, 8 bit
    // characters, DTR/DSR handshake.
    const usb_device_desc_t *dev = this->client_->get_device_desc_();
    const uint16_t mps0 = dev != nullptr ? dev->bMaxPacketSize0 : 8;
    this->client_->control_transfer(0xC0, 5, 0, 0, [](const usb_host::TransferStatus &) {},
                                    std::vector<uint8_t>(mps0), 2);
    this->client_->control_transfer(0x40, 3, 0xc04e, 0, [](const usb_host::TransferStatus &) {});
    this->client_->control_transfer(0x40, 4, 0x0808, 0, [](const usb_host::TransferStatus &) {});
    this->client_->control_transfer(0x40, 2, 0x0000, 0x0100, [](const usb_host::TransferStatus &) {});
    static const uint8_t GET_REVISION[] = {MCE_CMD_PORT_SYS, 0x0b};
    this->command_(DEVICE_RESUME, sizeof(DEVICE_RESUME));
    this->command_(GET_REVISION, sizeof(GET_REVISION));
  } else if (!(this->flags_ & GEN3)) {
    static const uint8_t GET_WAKEVERSION[] = {MCE_CMD_PORT_SYS, 0x18};
    static const uint8_t GET_UNKNOWN2[] = {MCE_CMD_PORT_IR, 0x05};
    this->command_(DEVICE_RESUME, sizeof(DEVICE_RESUME));
    this->command_(GET_WAKEVERSION, sizeof(GET_WAKEVERSION));
    this->command_(GET_UNKNOWN2, sizeof(GET_UNKNOWN2));
  }
  static const uint8_t GET_NUM_PORTS[] = {MCE_CMD_PORT_IR, 0x16};
  static const uint8_t GET_CARRIER_FREQ[] = {MCE_CMD_PORT_IR, 0x07};
  static const uint8_t GET_RX_TIMEOUT[] = {MCE_CMD_PORT_IR, 0x0d};
  static const uint8_t GET_RX_SENSOR[] = {MCE_CMD_PORT_IR, 0x15};
  this->command_(GET_NUM_PORTS, sizeof(GET_NUM_PORTS));
  this->command_(GET_CARRIER_FREQ, sizeof(GET_CARRIER_FREQ));
  this->command_(GET_RX_TIMEOUT, sizeof(GET_RX_TIMEOUT));
  this->command_(GET_RX_SENSOR, sizeof(GET_RX_SENSOR));
}

// mceusb_process_ir_data(): command responses are skipped, IR data packets (0x80 + n, then n
// samples) are samples of 50 us units, MSB set for a pulse; a packet of length 0 ends a signal.
bool MCEReceiverDriver::report(const uint8_t *d, size_t len) {
  size_t i = (this->flags_ & GEN1) ? 2 : 0;  // the original receiver's 0xb1 0x60 header
  for (; i < len; i++) {
    switch (this->parser_) {
      case SUBCMD:
        this->rem_ = datasize(this->cmd_, d[i]);
        this->parser_ = CMD_DATA;
        break;
      case PARSE_IRDATA: {
        this->rem_--;
        const uint32_t units = d[i] & 0x7f;
        if (units != 0)
          this->sample_((d[i] & 0x80) != 0, units * MCE_TIME_UNIT);
        break;
      }
      case CMD_DATA:
        this->rem_--;
        break;
      case CMD_HEADER:
        this->cmd_ = d[i];
        if (this->cmd_ == MCE_CMD_PORT_IR || (this->cmd_ & MCE_PORT_MASK) != MCE_COMMAND_IRDATA) {
          this->parser_ = SUBCMD;
          continue;
        }
        this->rem_ = this->cmd_ & MCE_PACKET_LENGTH_MASK;
        if (this->rem_ != 0) {
          this->parser_ = PARSE_IRDATA;
        } else {
          this->timeout_();
        }
        break;
    }
    if (this->parser_ != CMD_HEADER && this->rem_ == 0)
      this->parser_ = CMD_HEADER;
  }
  // IR data may go on in the next transfer; a command response may not.
  if (this->parser_ != PARSE_IRDATA || this->rem_ == 0)
    this->parser_ = CMD_HEADER;
  return true;
}

// Samples of one polarity are one event (ir_raw_event_store_with_filter()).
void MCEReceiverDriver::sample_(bool pulse, uint32_t us) {
  if (this->acc_valid_ && pulse == this->acc_pulse_) {
    this->acc_us_ += us;
    return;
  }
  if (this->acc_valid_)
    this->rc6_(this->acc_pulse_, this->acc_us_);
  this->acc_valid_ = true;
  this->acc_pulse_ = pulse;
  this->acc_us_ = us;
}

void MCEReceiverDriver::timeout_() {
  if (this->acc_valid_)
    this->rc6_(this->acc_pulse_, this->acc_us_);
  this->acc_valid_ = false;
  this->rc6_(false, RX_TIMEOUT_US);
}

// ir_rc6_decode(), as Linux has it.
static constexpr uint32_t RC6_UNIT = 444;
static inline bool eq_margin(uint32_t d, uint32_t t, uint32_t m) { return d > t - m && d < t + m; }
static inline bool geq_margin(uint32_t d, uint32_t t, uint32_t m) { return d > t - m; }
static inline void decrease(uint32_t &d, uint32_t by) { d = d > by ? d - by : 0; }

void MCEReceiverDriver::rc6_(bool pulse, uint32_t duration) {
  // RC6 mode from the header: 0, or 6A with the toggle (trailer) bit clear.
  auto mode = [this]() -> int {
    switch (this->header_ & 0x07) {
      case 0:
        return 0;
      case 6:
        return this->toggle_ ? -1 : 6;
      default:
        return -1;
    }
  };
  if (!geq_margin(duration, RC6_UNIT, RC6_UNIT / 2)) {
    this->state_ = INACTIVE;
    return;
  }
  while (true) {
    if (!geq_margin(duration, RC6_UNIT, RC6_UNIT / 2))
      return;
    switch (this->state_) {
      case INACTIVE:
        if (!pulse || !eq_margin(duration, 6 * RC6_UNIT, RC6_UNIT))
          break;
        this->state_ = PREFIX_SPACE;
        this->count_ = 0;
        return;
      case PREFIX_SPACE:
        if (pulse || !eq_margin(duration, 2 * RC6_UNIT, RC6_UNIT / 2))
          break;
        this->state_ = HEADER_BIT_START;
        this->header_ = 0;
        return;
      case HEADER_BIT_START:
        if (!eq_margin(duration, RC6_UNIT, RC6_UNIT / 2))
          break;
        this->header_ = static_cast<uint8_t>((this->header_ << 1) | (pulse ? 1 : 0));
        this->count_++;
        this->state_ = HEADER_BIT_END;
        return;
      case HEADER_BIT_END:
        this->state_ = this->count_ == 4 ? TOGGLE_START : HEADER_BIT_START;
        decrease(duration, RC6_UNIT);
        continue;
      case TOGGLE_START:
        if (!eq_margin(duration, 2 * RC6_UNIT, RC6_UNIT / 2))
          break;
        this->toggle_ = pulse;
        this->state_ = TOGGLE_END;
        return;
      case TOGGLE_END:
        if (!(this->header_ & 0x08))
          break;  // start bit
        this->state_ = BODY_BIT_START;
        decrease(duration, 2 * RC6_UNIT);
        this->count_ = 0;
        this->body_ = 0;
        if (mode() == 0) {
          this->wanted_bits_ = 16;
        } else if (mode() == 6) {
          this->wanted_bits_ = 128;
        } else {
          break;
        }
        continue;
      case BODY_BIT_START:
        if (eq_margin(duration, RC6_UNIT, RC6_UNIT / 2)) {
          if (this->count_++ < 64)
            this->body_ = (this->body_ << 1) | (pulse ? 1 : 0);
          this->state_ = BODY_BIT_END;
          return;
        }
        if (mode() == 6 && !pulse && geq_margin(duration, 6 * RC6_UNIT, RC6_UNIT / 2)) {
          this->state_ = FINISHED;
          continue;
        }
        break;
      case BODY_BIT_END:
        this->state_ = this->count_ == this->wanted_bits_ ? FINISHED : BODY_BIT_START;
        decrease(duration, RC6_UNIT);
        continue;
      case FINISHED:
        if (pulse)
          break;
        if (mode() == 6 && this->count_ == 32) {
          const auto scancode = static_cast<uint32_t>(this->body_);
          const uint32_t customer = scancode & 0xffff0000;
          if (customer == 0x800f0000 || customer == 0x80460000 || customer == 0x80340000)
            this->scancode_(scancode & ~0x8000u, (scancode & 0x8000) != 0);
        }
        this->state_ = INACTIVE;
        return;
    }
    // Not this protocol after all.
    this->state_ = INACTIVE;
    return;
  }
}

// rc-rc6-mce.c's keys as HID usages: keyboard keys where a keyboard has them, consumer
// controls for the rest; codes without either as 0xFF00:0x04xx.
struct MCEKey {
  uint8_t code;
  uint16_t page, usage;
};
static constexpr uint16_t KB = 0x07, CC = 0x0C;
static constexpr MCEKey MCE_KEYS[] = {
    {0x00, KB, 0x27},  {0x01, KB, 0x1E},  {0x02, KB, 0x1F},  {0x03, KB, 0x20},  {0x04, KB, 0x21},
    {0x05, KB, 0x22},  {0x06, KB, 0x23},  {0x07, KB, 0x24},  {0x08, KB, 0x25},  {0x09, KB, 0x26},
    {0x0a, KB, 0x4C},   // Delete
    {0x0b, KB, 0x58},   // Enter (keypad)
    {0x0c, CC, 0x32},   // Sleep
    {0x0d, CC, 0x88},   // Windows MCE button: Media Select Computer
    {0x0e, CC, 0xE2},   // Mute
    {0x0f, CC, 0x60},   // Info
    {0x10, CC, 0xE9},  {0x11, CC, 0xEA},  // Volume
    {0x12, CC, 0x9C},  {0x13, CC, 0x9D},  // Channel
    {0x14, CC, 0xB3},   // Fast forward
    {0x15, CC, 0xB4},   // Rewind
    {0x16, CC, 0xB0},   // Play
    {0x17, CC, 0xB2},   // Record
    {0x18, CC, 0xB1},   // Pause
    {0x19, CC, 0xB7},   // Stop
    {0x1a, CC, 0xB5},  {0x1b, CC, 0xB6},  // Next, previous
    {0x1c, KB, 0xCC},   // #
    {0x1d, KB, 0x55},   // *
    {0x1e, KB, 0x52},  {0x1f, KB, 0x51},  {0x20, KB, 0x50},  {0x21, KB, 0x4F},  // Up, down, left, right
    {0x22, KB, 0x28},   // OK
    {0x23, CC, 0x224},  // Back
    {0x24, CC, 0x8B},   // DVD
    {0x25, CC, 0x93},   // Live TV
    {0x26, CC, 0x8D},   // Guide
    {0x27, CC, 0x232},  // Aspect: zoom
    {0x33, CC, 0x188},  // Slide show
    {0x34, CC, 0xB8},   // Eject
    {0x3a, CC, 0x6F},   // Brightness up
    {0x46, CC, 0x89},   // TV
    {0x47, CC, 0x1B7},  // My Music
    {0x48, CC, 0x9A},   // Recorded TV
    {0x49, CC, 0x65},   // Pictures
    {0x4a, CC, 0x1B8},  // Videos
    {0x4e, CC, 0x208},  // Print
    {0x5a, CC, 0x61},   // Teletext / captions
    {0x5b, CC, 0x69},  {0x5c, CC, 0x6A},  {0x5d, CC, 0x6C},  {0x5e, CC, 0x6B},  // Red, green, yellow, blue
    {0x65, CC, 0x30},   // TV power
    {0x69, CC, 0x1BC},  // Messenger
    {0x6e, CC, 0xCD},   // Play/pause
    {0x6f, CC, 0x193},  // Media player
    {0x80, CC, 0x70},   // Brightness down
    {0x81, CC, 0xCD},   // Play/pause
};

// A held key repeats with the same toggle; the next press flips it.
void MCEReceiverDriver::scancode_(uint32_t scancode, bool toggle) {
  if (this->held_ && this->held_scancode_ == scancode && this->held_toggle_ == toggle) {
    App.scheduler.set_timeout(this->client_, KEY_UP, KEY_UP_MS, [this]() { this->release_(); });
    return;
  }
  this->release_();
  uint16_t page = 0xFF00;
  uint16_t usage = static_cast<uint16_t>(scancode & 0xFFFF);
  if ((scancode & 0xffffff00) == 0x800f0400) {
    usage = static_cast<uint16_t>(0x0400 | (scancode & 0xFF));
    for (const auto &k : MCE_KEYS) {
      if (k.code == (scancode & 0xFF)) {
        page = k.page;
        usage = k.usage;
        break;
      }
    }
  }
  this->held_ = true;
  this->held_scancode_ = scancode;
  this->held_toggle_ = toggle;
  this->held_page_ = page;
  this->held_usage_ = usage;
  this->input_->driver_key(page, usage, true);
  App.scheduler.set_timeout(this->client_, KEY_UP, KEY_UP_MS, [this]() { this->release_(); });
}

void MCEReceiverDriver::release_() {
  if (!this->held_)
    return;
  this->held_ = false;
  this->input_->driver_key(this->held_page_, this->held_usage_, false);
}

}  // namespace esphome::usb_hid

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
