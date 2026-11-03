#include "uart_tcp.h"

#ifdef USE_UART_TCP_RFC2217

#include "esphome/core/log.h"

#include <cinttypes>
#include <cstring>

namespace esphome::uart_tcp {

ESPHOME_LOG_TAG(TAG, "uart_tcp");

static constexpr uint8_t IAC = 255;
static constexpr uint8_t DONT = 254;
static constexpr uint8_t DO = 253;
static constexpr uint8_t WONT = 252;
static constexpr uint8_t WILL = 251;
static constexpr uint8_t SB = 250;
static constexpr uint8_t SE = 240;

static constexpr uint8_t OPT_BINARY = 0;
static constexpr uint8_t OPT_SGA = 3;
static constexpr uint8_t OPT_COM_PORT = 44;

static constexpr uint8_t CPC_SIGNATURE = 0;
static constexpr uint8_t CPC_SET_BAUDRATE = 1;
static constexpr uint8_t CPC_SET_DATASIZE = 2;
static constexpr uint8_t CPC_SET_PARITY = 3;
static constexpr uint8_t CPC_SET_STOPSIZE = 4;
static constexpr uint8_t CPC_SET_CONTROL = 5;
static constexpr uint8_t CPC_FLOWCONTROL_SUSPEND = 8;
static constexpr uint8_t CPC_FLOWCONTROL_RESUME = 9;
static constexpr uint8_t CPC_SET_LINESTATE_MASK = 10;
static constexpr uint8_t CPC_SET_MODEMSTATE_MASK = 11;
static constexpr uint8_t CPC_PURGE_DATA = 12;
static constexpr uint8_t CPC_SERVER = 100;

static constexpr uint8_t CONTROL_FLOW_REQUEST = 0;
static constexpr uint8_t CONTROL_FLOW_NONE = 1;
static constexpr uint8_t CONTROL_FLOW_HARDWARE = 3;
static constexpr uint8_t CONTROL_BREAK_REQUEST = 4;
static constexpr uint8_t CONTROL_BREAK_OFF = 6;
static constexpr uint8_t CONTROL_DTR_REQUEST = 7;
static constexpr uint8_t CONTROL_DTR_ON = 8;
static constexpr uint8_t CONTROL_DTR_OFF = 9;
static constexpr uint8_t CONTROL_RTS_REQUEST = 10;
static constexpr uint8_t CONTROL_RTS_ON = 11;
static constexpr uint8_t CONTROL_RTS_OFF = 12;
static constexpr uint8_t CONTROL_INBOUND_REQUEST = 13;
static constexpr uint8_t CONTROL_INBOUND_NONE = 14;
static constexpr uint8_t CONTROL_INBOUND_HARDWARE = 16;
static constexpr uint8_t CONTROL_DCD_FLOW = 17;
static constexpr uint8_t CONTROL_DSR_FLOW = 19;

static constexpr uint8_t PURGE_RX = 1;
static constexpr uint8_t PURGE_BOTH = 3;

static constexpr char SIGNATURE[] = "ESPHome uart_tcp";

static uint8_t option_bit(uint8_t option) {
  switch (option) {
    case OPT_BINARY:
      return 1 << 0;
    case OPT_SGA:
      return 1 << 1;
    case OPT_COM_PORT:
      return 1 << 2;
    default:
      return 0;
  }
}

static uint8_t parity_to_rfc(uart::UARTParityOptions parity) {
  switch (parity) {
    case uart::UART_CONFIG_PARITY_ODD:
      return 2;
    case uart::UART_CONFIG_PARITY_EVEN:
      return 3;
    case uart::UART_CONFIG_PARITY_MARK:
      return 4;
    case uart::UART_CONFIG_PARITY_SPACE:
      return 5;
    default:
      return 1;
  }
}

static bool parity_from_rfc(uint8_t value, uart::UARTParityOptions &parity) {
  switch (value) {
    case 1:
      parity = uart::UART_CONFIG_PARITY_NONE;
      return true;
    case 2:
      parity = uart::UART_CONFIG_PARITY_ODD;
      return true;
    case 3:
      parity = uart::UART_CONFIG_PARITY_EVEN;
      return true;
    case 4:
      parity = uart::UART_CONFIG_PARITY_MARK;
      return true;
    case 5:
      parity = uart::UART_CONFIG_PARITY_SPACE;
      return true;
    default:
      return false;
  }
}

size_t UartTcp::rfc_escape_(uint8_t *data, size_t len) {
  size_t ff = 0;
  for (size_t i = 0; i < len; i++) {
    if (data[i] == IAC) {
      ff++;
    }
  }
  if (ff == 0) {
    return len;
  }
  size_t out = len + ff;
  for (size_t i = len; i-- > 0;) {
    data[--out] = data[i];
    if (data[i] == IAC) {
      data[--out] = IAC;
    }
  }
  return len + ff;
}

void UartTcp::rfc_option_(uint8_t verb, uint8_t option) {
  const uint8_t msg[3] = {IAC, verb, option};
  this->tx_put_(msg, sizeof(msg));
}

void UartTcp::rfc_reply_(uint8_t command, const uint8_t *value, size_t len) {
  uint8_t msg[RFC_REPLY_MAX];
  size_t n = 0;
  msg[n++] = IAC;
  msg[n++] = SB;
  msg[n++] = OPT_COM_PORT;
  msg[n++] = command;
  for (size_t i = 0; i < len && n + 4 <= sizeof(msg); i++) {
    msg[n++] = value[i];
    if (value[i] == IAC) {
      msg[n++] = IAC;
    }
  }
  msg[n++] = IAC;
  msg[n++] = SE;
  this->tx_put_(msg, n);
}

void UartTcp::rfc_reply_byte_(uint8_t command, uint8_t value) { this->rfc_reply_(command, &value, 1); }

void UartTcp::rfc_reply_baud_(uint8_t command, uint32_t baud) {
  const uint8_t value[4] = {static_cast<uint8_t>(baud >> 24), static_cast<uint8_t>(baud >> 16),
                            static_cast<uint8_t>(baud >> 8), static_cast<uint8_t>(baud)};
  this->rfc_reply_(command, value, sizeof(value));
}

void UartTcp::rfc_start_() {
  this->rfc_ = Rfc2217{};
  if (this->server_) {
    return;
  }
  this->rfc_option_(WILL, OPT_COM_PORT);
  this->rfc_option_(WILL, OPT_BINARY);
  this->rfc_option_(DO, OPT_BINARY);
  this->rfc_.local_sent = option_bit(OPT_COM_PORT) | option_bit(OPT_BINARY);
  this->rfc_.remote_sent = option_bit(OPT_BINARY);
}

void UartTcp::rfc_send_line_settings_() {
  if (this->parent_ == nullptr) {
    return;
  }
  this->rfc_reply_baud_(CPC_SET_BAUDRATE, this->parent_->get_baud_rate());
  this->rfc_reply_byte_(CPC_SET_DATASIZE, this->parent_->get_data_bits());
  this->rfc_reply_byte_(CPC_SET_PARITY, parity_to_rfc(this->parent_->get_parity()));
  this->rfc_reply_byte_(CPC_SET_STOPSIZE, this->parent_->get_stop_bits() == 2 ? 2 : 1);
}

void UartTcp::line_settings_changed() {
  if (this->rfc2217_ && !this->server_ && this->link_was_up_ && this->rfc_.com_port) {
    this->rfc_send_line_settings_();
  }
}

void UartTcp::rfc_negotiate_(uint8_t verb, uint8_t option) {
  const uint8_t bit = option_bit(option);
  const bool remote = verb == WILL || verb == WONT;
  const bool on = verb == WILL || verb == DO;
  const bool accept = remote ? bit != 0 : (bit != 0 && (option != OPT_COM_PORT || !this->server_));
  uint8_t &sent = remote ? this->rfc_.remote_sent : this->rfc_.local_sent;
  if (!on) {
    sent &= ~bit;
    if (!remote && option == OPT_COM_PORT) {
      this->rfc_.com_port = false;
    }
    return;
  }
  if (!accept) {
    this->rfc_option_(remote ? DONT : WONT, option);
    return;
  }
  if ((sent & bit) == 0) {
    sent |= bit;
    this->rfc_option_(remote ? DO : WILL, option);
  }
  if (!remote && option == OPT_COM_PORT && !this->rfc_.com_port) {
    this->rfc_.com_port = true;
    this->rfc_send_line_settings_();
  }
}

void UartTcp::rfc_apply_line_(uint8_t command, const uint8_t *value, size_t len) {
  if (this->parent_ == nullptr) {
    return;
  }
  bool changed = false;
  switch (command) {
    case CPC_SET_BAUDRATE: {
      const uint32_t baud = len >= 4 ? (static_cast<uint32_t>(value[0]) << 24) |
                                           (static_cast<uint32_t>(value[1]) << 16) |
                                           (static_cast<uint32_t>(value[2]) << 8) | value[3]
                                     : 0;
      if (baud != 0 && baud != this->parent_->get_baud_rate()) {
        this->parent_->set_baud_rate(baud);
        changed = true;
      }
      break;
    }
    case CPC_SET_DATASIZE:
      if (len >= 1 && value[0] >= 5 && value[0] <= 8 && value[0] != this->parent_->get_data_bits()) {
        this->parent_->set_data_bits(value[0]);
        changed = true;
      }
      break;
    case CPC_SET_PARITY: {
      uart::UARTParityOptions parity;
      if (len >= 1 && parity_from_rfc(value[0], parity) && parity != this->parent_->get_parity()) {
        this->parent_->set_parity(parity);
        changed = true;
      }
      break;
    }
    case CPC_SET_STOPSIZE:
      if (len >= 1 && (value[0] == 1 || value[0] == 2) && value[0] != this->parent_->get_stop_bits()) {
        this->parent_->set_stop_bits(value[0]);
        changed = true;
      }
      break;
    default:
      break;
  }
  if (changed) {
#if defined(USE_ESP8266) || defined(USE_ESP32)
    this->parent_->load_settings(false);
#endif
    ESP_LOGD(TAG, "RFC 2217: %" PRIu32 " baud, %u data bits, parity %u, %u stop bits", this->parent_->get_baud_rate(),
             this->parent_->get_data_bits(), static_cast<unsigned>(this->parent_->get_parity()),
             this->parent_->get_stop_bits());
  }
  switch (command) {
    case CPC_SET_BAUDRATE:
      this->rfc_reply_baud_(command + CPC_SERVER, this->parent_->get_baud_rate());
      break;
    case CPC_SET_DATASIZE:
      this->rfc_reply_byte_(command + CPC_SERVER, this->parent_->get_data_bits());
      break;
    case CPC_SET_PARITY:
      this->rfc_reply_byte_(command + CPC_SERVER, parity_to_rfc(this->parent_->get_parity()));
      break;
    default:
      this->rfc_reply_byte_(command + CPC_SERVER, this->parent_->get_stop_bits() == 2 ? 2 : 1);
      break;
  }
}

void UartTcp::rfc_control_(uint8_t value) {
  uint8_t reply;
  switch (value) {
    case CONTROL_FLOW_REQUEST:
    case CONTROL_FLOW_NONE:
    case 2:
    case CONTROL_FLOW_HARDWARE:
    case CONTROL_DCD_FLOW:
    case CONTROL_DSR_FLOW:
      reply = CONTROL_FLOW_NONE;
      break;
    case CONTROL_BREAK_REQUEST:
    case 5:
    case CONTROL_BREAK_OFF:
      reply = CONTROL_BREAK_OFF;
      break;
    case CONTROL_DTR_ON:
    case CONTROL_DTR_OFF:
    case CONTROL_RTS_ON:
    case CONTROL_RTS_OFF: {
#ifdef USE_UART_TCP_MODEM_CONTROL
      bool dtr = this->rfc_.dtr;
      bool rts = this->rfc_.rts;
      if (value == CONTROL_DTR_ON || value == CONTROL_DTR_OFF) {
        dtr = value == CONTROL_DTR_ON;
      } else {
        rts = value == CONTROL_RTS_ON;
      }
      if (this->modem_ != nullptr && this->modem_->set_modem_control(dtr, rts)) {
        this->rfc_.dtr = dtr;
        this->rfc_.rts = rts;
      }
#endif
      reply = value <= CONTROL_DTR_OFF ? (this->rfc_.dtr ? CONTROL_DTR_ON : CONTROL_DTR_OFF)
                                       : (this->rfc_.rts ? CONTROL_RTS_ON : CONTROL_RTS_OFF);
      break;
    }
    case CONTROL_DTR_REQUEST:
      reply = this->rfc_.dtr ? CONTROL_DTR_ON : CONTROL_DTR_OFF;
      break;
    case CONTROL_RTS_REQUEST:
      reply = this->rfc_.rts ? CONTROL_RTS_ON : CONTROL_RTS_OFF;
      break;
    case CONTROL_INBOUND_REQUEST:
    case CONTROL_INBOUND_NONE:
    case 15:
    case CONTROL_INBOUND_HARDWARE:
    case 18:
      reply = CONTROL_INBOUND_NONE;
      break;
    default:
      return;
  }
  this->rfc_reply_byte_(CPC_SET_CONTROL + CPC_SERVER, reply);
}

void UartTcp::rfc_command_() {
  if (this->rfc_.sb_len < 2 || this->rfc_.sb[0] != OPT_COM_PORT) {
    return;
  }
  const uint8_t command = this->rfc_.sb[1];
  const uint8_t *value = this->rfc_.sb + 2;
  const size_t len = this->rfc_.sb_len - 2;
  if (!this->server_) {
    if (command == CPC_FLOWCONTROL_SUSPEND + CPC_SERVER) {
      this->rfc_.suspended = true;
    } else if (command == CPC_FLOWCONTROL_RESUME + CPC_SERVER) {
      this->rfc_.suspended = false;
    }
    return;
  }
  switch (command) {
    case CPC_SIGNATURE:
      if (len == 0) {
        this->rfc_reply_(CPC_SIGNATURE + CPC_SERVER, reinterpret_cast<const uint8_t *>(SIGNATURE),
                         sizeof(SIGNATURE) - 1);
      }
      break;
    case CPC_SET_BAUDRATE:
    case CPC_SET_DATASIZE:
    case CPC_SET_PARITY:
    case CPC_SET_STOPSIZE:
      this->rfc_apply_line_(command, value, len);
      break;
    case CPC_SET_CONTROL:
      if (len >= 1) {
        this->rfc_control_(value[0]);
      }
      break;
    case CPC_FLOWCONTROL_SUSPEND:
      this->rfc_.suspended = true;
      break;
    case CPC_FLOWCONTROL_RESUME:
      this->rfc_.suspended = false;
      break;
    case CPC_SET_LINESTATE_MASK:
    case CPC_SET_MODEMSTATE_MASK:
      if (len >= 1) {
        this->rfc_reply_byte_(command + CPC_SERVER, value[0]);
      }
      break;
    case CPC_PURGE_DATA:
      if (len >= 1 && value[0] >= PURGE_RX && value[0] <= PURGE_BOTH) {
        if (value[0] & PURGE_RX) {
          this->discard_uart_();
        }
        this->rfc_reply_byte_(command + CPC_SERVER, value[0]);
      }
      break;
    default:
      break;
  }
}

size_t UartTcp::rfc_decode_(uint8_t *data, size_t len) {
  size_t out = 0;
  Rfc2217 &s = this->rfc_;
  for (size_t i = 0; i < len; i++) {
    const uint8_t c = data[i];
    switch (s.state) {
      case Rfc2217::DATA:
        if (c == IAC) {
          s.state = Rfc2217::COMMAND;
        } else {
          data[out++] = c;
        }
        break;
      case Rfc2217::COMMAND:
        if (c == IAC) {
          data[out++] = IAC;
          s.state = Rfc2217::DATA;
        } else if (c >= WILL && c <= DONT) {
          s.verb = c;
          s.state = Rfc2217::OPTION;
        } else if (c == SB) {
          s.sb_len = 0;
          s.state = Rfc2217::SUBNEG;
        } else {
          s.state = Rfc2217::DATA;
        }
        break;
      case Rfc2217::OPTION:
        this->rfc_negotiate_(s.verb, c);
        s.state = Rfc2217::DATA;
        break;
      case Rfc2217::SUBNEG:
        if (c == IAC) {
          s.state = Rfc2217::SUBNEG_IAC;
        } else if (s.sb_len < sizeof(s.sb)) {
          s.sb[s.sb_len++] = c;
        }
        break;
      case Rfc2217::SUBNEG_IAC:
        if (c == SE) {
          this->rfc_command_();
          s.state = Rfc2217::DATA;
        } else {
          if (s.sb_len < sizeof(s.sb)) {
            s.sb[s.sb_len++] = c;
          }
          s.state = Rfc2217::SUBNEG;
        }
        break;
    }
  }
  return out;
}

}

#endif
