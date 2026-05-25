#include "network_serial.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cstring>
#include <algorithm>

namespace esphome {
namespace network_serial {

static const char *const TAG_SERVER = "network_serial.server";

NetworkSerialServer::~NetworkSerialServer() {
  this->disconnect_client_();
  this->stop_listening_();
}

void NetworkSerialServer::setup() {
  ESP_LOGCONFIG(TAG_SERVER, "Setting up Network Serial Server...");
  ESP_LOGCONFIG(TAG_SERVER, "  Port: %u", this->port_);

  this->telnet_parse_buf_.reserve(128);

  if (!this->start_listening_()) {
    ESP_LOGE(TAG_SERVER, "Failed to start listening on port %u", this->port_);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG_SERVER, "Listening on port %u", this->port_);
}

void NetworkSerialServer::loop() {
  if (!this->client_connected_) {
    this->accept_client_();
  }

  if (!this->client_connected_) {
    return;
  }

  uint8_t buffer[BRIDGE_BUF_SIZE];
  size_t received = this->receive_from_client_(buffer, sizeof(buffer));
  if (received > 0) {
    this->process_client_data_(buffer, received);
  }
  this->bridge_uart_to_client_();
}

void NetworkSerialServer::dump_config() {
  ESP_LOGCONFIG(TAG_SERVER, "Network Serial Server:");
  ESP_LOGCONFIG(TAG_SERVER, "  Port: %u", this->port_);
  ESP_LOGCONFIG(TAG_SERVER, "  Client: %s", this->client_connected_ ? "Connected" : "None");
}

bool NetworkSerialServer::start_listening_() {
#ifdef USE_ESP_IDF
  this->listen_socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->listen_socket_ < 0) {
    ESP_LOGE(TAG_SERVER, "Failed to create socket: errno %d", errno);
    return false;
  }

  int reuse = 1;
  setsockopt(this->listen_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(this->listen_socket_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG_SERVER, "Failed to bind: errno %d", errno);
    close(this->listen_socket_);
    this->listen_socket_ = -1;
    return false;
  }

  if (listen(this->listen_socket_, 1) < 0) {
    ESP_LOGE(TAG_SERVER, "Failed to listen: errno %d", errno);
    close(this->listen_socket_);
    this->listen_socket_ = -1;
    return false;
  }

  int flags = fcntl(this->listen_socket_, F_GETFL, 0);
  fcntl(this->listen_socket_, F_SETFL, flags | O_NONBLOCK);

  return true;
#else
  this->server_ = std::make_unique<WiFiServer>(this->port_);
  this->server_->begin();
  this->server_->setNoDelay(true);
  return true;
#endif
}

void NetworkSerialServer::stop_listening_() {
#ifdef USE_ESP_IDF
  if (this->listen_socket_ >= 0) {
    close(this->listen_socket_);
    this->listen_socket_ = -1;
  }
#else
  if (this->server_) {
    this->server_->stop();
    this->server_ = nullptr;
  }
#endif
}

void NetworkSerialServer::accept_client_() {
#ifdef USE_ESP_IDF
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);
  int new_socket = accept(this->listen_socket_, (struct sockaddr *) &client_addr, &addr_len);
  if (new_socket < 0) {
    return;
  }

  int flags = fcntl(new_socket, F_GETFL, 0);
  fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);
  int nodelay = 1;
  setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  this->client_socket_ = new_socket;
  this->client_connected_ = true;
  this->telnet_negotiated_ = false;

  char addr_str[INET_ADDRSTRLEN];
  inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
  ESP_LOGI(TAG_SERVER, "Client connected from %s:%u", addr_str, ntohs(client_addr.sin_port));
  this->send_telnet_negotiation_();
#else
  if (!this->server_->hasClient()) {
    return;
  }

  this->client_ = std::make_unique<WiFiClient>(this->server_->accept());
  if (!this->client_ || !this->client_->connected()) {
    this->client_ = nullptr;
    return;
  }

  this->client_->setNoDelay(true);
  this->client_connected_ = true;
  this->telnet_negotiated_ = false;

  ESP_LOGI(TAG_SERVER, "Client connected");

  this->send_telnet_negotiation_();
#endif
}

void NetworkSerialServer::disconnect_client_() {
  if (!this->client_connected_) {
    return;
  }

  ESP_LOGI(TAG_SERVER, "Client disconnected");

#ifdef USE_ESP_IDF
  if (this->client_socket_ >= 0) {
    close(this->client_socket_);
    this->client_socket_ = -1;
  }
#else
  if (this->client_) {
    this->client_->stop();
    this->client_ = nullptr;
  }
#endif

  this->client_connected_ = false;
  this->telnet_negotiated_ = false;
  this->telnet_parse_buf_.clear();
}

bool NetworkSerialServer::send_to_client_(const uint8_t *data, size_t length) {
  if (!this->client_connected_) {
    return false;
  }

#ifdef USE_ESP_IDF
  int sent = send(this->client_socket_, data, length, 0);
  if (sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return false;
    }
    ESP_LOGW(TAG_SERVER, "Send error: errno %d", errno);
    this->disconnect_client_();
    return false;
  }
  return sent > 0;
#else
  if (!this->client_ || !this->client_->connected()) {
    this->disconnect_client_();
    return false;
  }
  return this->client_->write(data, length) > 0;
#endif
}

size_t NetworkSerialServer::receive_from_client_(uint8_t *buffer, size_t length) {
  if (!this->client_connected_) {
    return 0;
  }

  int received = recv(this->client_socket_, buffer, length, 0);
  if (received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    ESP_LOGW(TAG_SERVER, "Receive error: errno %d", errno);
    this->disconnect_client_();
    return 0;
  }
  if (received == 0) {
    this->disconnect_client_();
    return 0;
  }
  return received;
}

void NetworkSerialServer::send_telnet_negotiation_() {
  ESP_LOGD(TAG_SERVER, "Sending Telnet negotiation...");
  this->send_telnet_command_(TELNET_WILL, TELNET_BINARY);
  this->send_telnet_command_(TELNET_DO, TELNET_BINARY);

  this->send_telnet_command_(TELNET_WILL, TELNET_SUPPRESS_GO_AHEAD);
  this->send_telnet_command_(TELNET_DO, TELNET_SUPPRESS_GO_AHEAD);
  this->send_telnet_command_(TELNET_WILL, TELNET_COM_PORT);

  this->telnet_negotiated_ = true;
  ESP_LOGD(TAG_SERVER, "Telnet negotiation sent");
  this->send_initial_modem_state_();
}

void NetworkSerialServer::send_telnet_command_(TelnetCommand cmd, TelnetOption option) {
  uint8_t buffer[3] = {TELNET_IAC, cmd, option};
  this->send_to_client_(buffer, sizeof(buffer));
}

void NetworkSerialServer::send_telnet_subnegotiation_(const uint8_t *data, size_t length) {
  std::vector<uint8_t> buffer;
  buffer.reserve(length + 4);
  buffer.push_back(TELNET_IAC);
  buffer.push_back(TELNET_SB);
  buffer.insert(buffer.end(), data, data + length);
  buffer.push_back(TELNET_IAC);
  buffer.push_back(TELNET_SE);

  this->send_to_client_(buffer.data(), buffer.size());
}

void NetworkSerialServer::process_client_data_(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    uint8_t byte = data[i];

    if (byte == TELNET_IAC) {
      if (i + 1 < length) {
        uint8_t cmd = data[++i];

        if (cmd == TELNET_IAC) {
          this->uart_->write_byte(0xFF);
        } else if (cmd == TELNET_SB) {
          this->telnet_parse_buf_.clear();
          i++;
          while (i < length) {
            if (data[i] == TELNET_IAC && i + 1 < length && data[i + 1] == TELNET_SE) {
              i++;
              this->handle_telnet_subnegotiation_(this->telnet_parse_buf_.data(), this->telnet_parse_buf_.size());
              break;
            }
            this->telnet_parse_buf_.push_back(data[i++]);
          }
        } else if (cmd == TELNET_WILL || cmd == TELNET_WONT || cmd == TELNET_DO || cmd == TELNET_DONT) {
          if (i + 1 < length) {
            uint8_t option = data[++i];
            this->handle_telnet_command_(static_cast<TelnetCommand>(cmd), static_cast<TelnetOption>(option));
          }
        }
      }
    } else {
      this->uart_->write_byte(byte);
    }
  }
}

void NetworkSerialServer::handle_telnet_command_(TelnetCommand cmd, TelnetOption option) {
  ESP_LOGVV(TAG_SERVER, "Client Telnet: %u %u", cmd, option);

  switch (cmd) {
    case TELNET_DO:
      if (option == TELNET_COM_PORT || option == TELNET_BINARY || option == TELNET_SUPPRESS_GO_AHEAD) {
        this->send_telnet_command_(TELNET_WILL, option);
      } else {
        this->send_telnet_command_(TELNET_WONT, option);
      }
      break;

    case TELNET_WILL:
      if (option == TELNET_COM_PORT || option == TELNET_BINARY || option == TELNET_SUPPRESS_GO_AHEAD) {
        this->send_telnet_command_(TELNET_DO, option);
      } else {
        this->send_telnet_command_(TELNET_DONT, option);
      }
      break;

    case TELNET_WONT:
    case TELNET_DONT:
    default:
      break;
  }
}

void NetworkSerialServer::handle_telnet_subnegotiation_(const uint8_t *data, size_t length) {
  if (length < 1) {
    return;
  }

  if (data[0] == TELNET_COM_PORT) {
    if (length >= 2) {
      this->handle_com_port_command_(data + 1, length - 1);
    }
  }
}

void NetworkSerialServer::send_com_port_response_(uint8_t command, const uint8_t *data, size_t length) {
  std::vector<uint8_t> subneg_data;
  subneg_data.reserve(2 + length);
  subneg_data.push_back(TELNET_COM_PORT);
  subneg_data.push_back(command);
  if (data && length > 0) {
    subneg_data.insert(subneg_data.end(), data, data + length);
  }
  this->send_telnet_subnegotiation_(subneg_data.data(), subneg_data.size());
}

void NetworkSerialServer::send_com_port_response_uint32_(uint8_t command, uint32_t value) {
  uint8_t data[4];
  data[0] = (value >> 24) & 0xFF;
  data[1] = (value >> 16) & 0xFF;
  data[2] = (value >> 8) & 0xFF;
  data[3] = value & 0xFF;
  this->send_com_port_response_(command, data, sizeof(data));
}

void NetworkSerialServer::handle_com_port_command_(const uint8_t *data, size_t length) {
  if (length < 1) {
    return;
  }

  uint8_t command = data[0];
  const uint8_t *param_data = data + 1;
  size_t param_length = length - 1;

  ESP_LOGD(TAG_SERVER, "RFC2217 client command: %u, params: %zu bytes", command, param_length);

  switch (command) {
    case RFC2217_SET_BAUDRATE:
      if (param_length >= 4) {
        uint32_t baudrate = (param_data[0] << 24) | (param_data[1] << 16) | (param_data[2] << 8) | param_data[3];
        if (baudrate == 0) {
          baudrate = this->uart_->get_baud_rate();
        } else {
          ESP_LOGI(TAG_SERVER, "Client set baudrate: %u", baudrate);
          this->uart_->set_baud_rate(baudrate);
          this->apply_uart_config_();
        }
        this->send_com_port_response_uint32_(RFC2217_SET_BAUDRATE_CS, this->uart_->get_baud_rate());
      }
      break;

    case RFC2217_SET_DATASIZE:
      if (param_length >= 1) {
        uint8_t data_size = param_data[0];
        if (data_size == 0) {
          data_size = this->uart_->get_data_bits();
        } else {
          ESP_LOGI(TAG_SERVER, "Client set data size: %u", data_size);
          this->uart_->set_data_bits(data_size);
          this->apply_uart_config_();
        }
        uint8_t resp = this->uart_->get_data_bits();
        this->send_com_port_response_(RFC2217_SET_DATASIZE_CS, &resp, 1);
      }
      break;

    case RFC2217_SET_PARITY:
      if (param_length >= 1) {
        uint8_t parity = param_data[0];
        if (parity == 0) {
        } else {
          ESP_LOGI(TAG_SERVER, "Client set parity: %u", parity);
          switch (parity) {
            case PARITY_NONE:
              this->uart_->set_parity(uart::UART_CONFIG_PARITY_NONE);
              break;
            case PARITY_ODD:
              this->uart_->set_parity(uart::UART_CONFIG_PARITY_ODD);
              break;
            case PARITY_EVEN:
              this->uart_->set_parity(uart::UART_CONFIG_PARITY_EVEN);
              break;
            default:
              ESP_LOGW(TAG_SERVER, "Unsupported parity mode: %u, using NONE", parity);
              this->uart_->set_parity(uart::UART_CONFIG_PARITY_NONE);
              break;
          }
          this->apply_uart_config_();
        }
        uint8_t resp;
        switch (this->uart_->get_parity()) {
          case uart::UART_CONFIG_PARITY_NONE:
            resp = PARITY_NONE;
            break;
          case uart::UART_CONFIG_PARITY_ODD:
            resp = PARITY_ODD;
            break;
          case uart::UART_CONFIG_PARITY_EVEN:
            resp = PARITY_EVEN;
            break;
          default:
            resp = PARITY_NONE;
            break;
        }
        this->send_com_port_response_(RFC2217_SET_PARITY_CS, &resp, 1);
      }
      break;

    case RFC2217_SET_STOPSIZE:
      if (param_length >= 1) {
        uint8_t stop_bits = param_data[0];
        if (stop_bits == 0) {
          stop_bits = this->uart_->get_stop_bits();
        } else {
          ESP_LOGI(TAG_SERVER, "Client set stop bits: %u", stop_bits);
          this->uart_->set_stop_bits(stop_bits);
          this->apply_uart_config_();
        }
        uint8_t resp = this->uart_->get_stop_bits();
        this->send_com_port_response_(RFC2217_SET_STOPSIZE_CS, &resp, 1);
      }
      break;

    case RFC2217_SET_CONTROL:
      if (param_length >= 1) {
        uint8_t control = param_data[0];
        ESP_LOGD(TAG_SERVER, "Client set control: %u", control);
        uint8_t resp = control;
        switch (control) {
          case 0:
            resp = FLOW_NONE;
            break;
          case 1:
          case 2:
          case 3:
            resp = control;
            break;
          case 8:
            this->modem_state_ |= MODEM_STATE_DSR;
            this->send_modem_state_();
            resp = 8;
            break;
          case 9:
            this->modem_state_ &= ~MODEM_STATE_DSR;
            this->send_modem_state_();
            resp = 9;
            break;
          case 11:
            this->modem_state_ |= MODEM_STATE_CTS;
            this->send_modem_state_();
            resp = 11;
            break;
          case 12:
            this->modem_state_ &= ~MODEM_STATE_CTS;
            this->send_modem_state_();
            resp = 12;
            break;
          default:
            break;
        }
        this->send_com_port_response_(RFC2217_SET_CONTROL_CS, &resp, 1);
      }
      break;

    case RFC2217_SET_LINESTATE_MASK:
      if (param_length >= 1) {
        this->line_state_mask_ = param_data[0];
        ESP_LOGD(TAG_SERVER, "Client set line state mask: 0x%02X", this->line_state_mask_);
        this->send_com_port_response_(RFC2217_SET_LINESTATE_MASK_CS, &this->line_state_mask_, 1);
      }
      break;

    case RFC2217_SET_MODEMSTATE_MASK:
      if (param_length >= 1) {
        this->modem_state_mask_ = param_data[0];
        ESP_LOGD(TAG_SERVER, "Client set modem state mask: 0x%02X", this->modem_state_mask_);
        this->send_com_port_response_(RFC2217_SET_MODEMSTATE_MASK_CS, &this->modem_state_mask_, 1);
      }
      break;

    case RFC2217_PURGE_DATA:
      if (param_length >= 1) {
        ESP_LOGD(TAG_SERVER, "Client purge: %u", param_data[0]);
        this->send_com_port_response_(RFC2217_PURGE_DATA_CS, param_data, 1);
      }
      break;

    case RFC2217_FLOWCONTROL_SUSPEND:
      ESP_LOGD(TAG_SERVER, "Client flow control suspend");
      this->send_com_port_response_(RFC2217_FLOWCONTROL_SUSPEND_CS, nullptr, 0);
      break;

    case RFC2217_FLOWCONTROL_RESUME:
      ESP_LOGD(TAG_SERVER, "Client flow control resume");
      this->send_com_port_response_(RFC2217_FLOWCONTROL_RESUME_CS, nullptr, 0);
      break;

    default:
      ESP_LOGV(TAG_SERVER, "Unhandled RFC2217 client command: %u", command);
      break;
  }
}

void NetworkSerialServer::apply_uart_config_() {
#if defined(USE_ESP8266) || defined(USE_ESP32)
  ESP_LOGD(TAG_SERVER, "Applying UART configuration...");
  this->uart_->load_settings(true);
#else
  ESP_LOGW(TAG_SERVER, "Runtime UART reconfiguration not supported on this platform");
#endif
}

void NetworkSerialServer::send_modem_state_() {
  uint8_t masked = this->modem_state_ & this->modem_state_mask_;
  this->send_com_port_response_(RFC2217_NOTIFY_MODEMSTATE_CS, &masked, 1);
}

void NetworkSerialServer::send_initial_modem_state_() {
  this->modem_state_ = MODEM_STATE_DSR | MODEM_STATE_CTS;
  this->send_modem_state_();
}

void NetworkSerialServer::bridge_uart_to_client_() {
  size_t avail = this->uart_->available();
  if (avail == 0) {
    return;
  }

  uint8_t uart_buf[BRIDGE_BUF_SIZE];
  size_t to_read = std::min(avail, sizeof(uart_buf));

  if (!this->uart_->read_array(uart_buf, to_read)) {
    return;
  }

  uint8_t out_buf[BRIDGE_BUF_SIZE * 2];
  size_t out_len = 0;

  for (size_t i = 0; i < to_read && out_len < sizeof(out_buf) - 1; i++) {
    out_buf[out_len++] = uart_buf[i];
    if (uart_buf[i] == TELNET_IAC) {
      out_buf[out_len++] = TELNET_IAC;
    }
  }

  this->send_to_client_(out_buf, out_len);
}

}  // namespace network_serial
}  // namespace esphome
