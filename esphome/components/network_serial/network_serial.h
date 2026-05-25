#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart_component.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"


#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace esphome {
namespace network_serial {

static const char *const TAG = "network_serial";

enum TelnetCommand : uint8_t {
  TELNET_SE = 240,
  TELNET_NOP = 241,
  TELNET_DM = 242,
  TELNET_BRK = 243,
  TELNET_IP = 244,
  TELNET_AO = 245,
  TELNET_AYT = 246,
  TELNET_EC = 247,
  TELNET_EL = 248,
  TELNET_GA = 249,
  TELNET_SB = 250,
  TELNET_WILL = 251,
  TELNET_WONT = 252,
  TELNET_DO = 253,
  TELNET_DONT = 254,
  TELNET_IAC = 255,
};

enum TelnetOption : uint8_t {
  TELNET_BINARY = 0,
  TELNET_ECHO = 1,
  TELNET_SUPPRESS_GO_AHEAD = 3,
  TELNET_STATUS = 5,
  TELNET_TIMING_MARK = 6,
  TELNET_COM_PORT = 44,
};

enum RFC2217ServerCommand : uint8_t {
  RFC2217_SET_BAUDRATE = 1,
  RFC2217_SET_DATASIZE = 2,
  RFC2217_SET_PARITY = 3,
  RFC2217_SET_STOPSIZE = 4,
  RFC2217_SET_CONTROL = 5,
  RFC2217_NOTIFY_LINESTATE = 6,
  RFC2217_NOTIFY_MODEMSTATE = 7,
  RFC2217_FLOWCONTROL_SUSPEND = 8,
  RFC2217_FLOWCONTROL_RESUME = 9,
  RFC2217_SET_LINESTATE_MASK = 10,
  RFC2217_SET_MODEMSTATE_MASK = 11,
  RFC2217_PURGE_DATA = 12,
};

enum RFC2217ClientCommand : uint8_t {
  RFC2217_SET_BAUDRATE_CS = 101,
  RFC2217_SET_DATASIZE_CS = 102,
  RFC2217_SET_PARITY_CS = 103,
  RFC2217_SET_STOPSIZE_CS = 104,
  RFC2217_SET_CONTROL_CS = 105,
  RFC2217_NOTIFY_LINESTATE_CS = 106,
  RFC2217_NOTIFY_MODEMSTATE_CS = 107,
  RFC2217_FLOWCONTROL_SUSPEND_CS = 108,
  RFC2217_FLOWCONTROL_RESUME_CS = 109,
  RFC2217_SET_LINESTATE_MASK_CS = 110,
  RFC2217_SET_MODEMSTATE_MASK_CS = 111,
  RFC2217_PURGE_DATA_CS = 112,
};

enum ParityMode : uint8_t {
  PARITY_NONE = 1,
  PARITY_ODD = 2,
  PARITY_EVEN = 3,
  PARITY_MARK = 4,
  PARITY_SPACE = 5,
};

enum DataSize : uint8_t {
  DATA_SIZE_5 = 5,
  DATA_SIZE_6 = 6,
  DATA_SIZE_7 = 7,
  DATA_SIZE_8 = 8,
};

enum StopBits : uint8_t {
  STOP_BITS_1 = 1,
  STOP_BITS_2 = 2,
  STOP_BITS_1_5 = 3,
};

enum FlowControl : uint8_t {
  FLOW_NONE = 1,
  FLOW_XONXOFF = 2,
  FLOW_HARDWARE = 3,
};

enum ModemState : uint8_t {
  MODEM_STATE_DELTA_CTS = (1 << 0),
  MODEM_STATE_DELTA_DSR = (1 << 1),
  MODEM_STATE_TRAILING_EDGE_RI = (1 << 2),
  MODEM_STATE_DELTA_DCD = (1 << 3),
  MODEM_STATE_CTS = (1 << 4),
  MODEM_STATE_DSR = (1 << 5),
  MODEM_STATE_RI = (1 << 6),
  MODEM_STATE_DCD = (1 << 7),
};

enum LineState : uint8_t {
  LINE_STATE_DATA_READY = (1 << 0),
  LINE_STATE_OVERRUN_ERROR = (1 << 1),
  LINE_STATE_PARITY_ERROR = (1 << 2),
  LINE_STATE_FRAMING_ERROR = (1 << 3),
  LINE_STATE_BREAK_DETECT = (1 << 4),
  LINE_STATE_TX_HOLDING_EMPTY = (1 << 5),
  LINE_STATE_TX_SHIFT_EMPTY = (1 << 6),
  LINE_STATE_TIMEOUT_ERROR = (1 << 7),
};

enum ControlSignal : uint8_t {
  CONTROL_DTR = (1 << 0),
  CONTROL_RTS = (1 << 1),
  CONTROL_DTR_ON = (1 << 3),
  CONTROL_DTR_OFF = (1 << 4),
  CONTROL_RTS_ON = (1 << 5),
  CONTROL_RTS_OFF = (1 << 6),
  CONTROL_FLOW_CONTROL = (1 << 7),
};

enum PurgeData : uint8_t {
  PURGE_RX_BUFFER = 1,
  PURGE_TX_BUFFER = 2,
  PURGE_BOTH = 3,
};

struct SerialConfig {
  uint32_t baudrate{9600};
  DataSize data_size{DATA_SIZE_8};
  ParityMode parity{PARITY_NONE};
  StopBits stop_bits{STOP_BITS_1};
  FlowControl flow_control{FLOW_NONE};
  bool dtr{false};
  bool rts{false};

  SerialConfig() = default;
};

class NetworkSerialClient : public Component {
 public:
  NetworkSerialClient() = default;
  ~NetworkSerialClient();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_host(const std::string &host) { this->host_ = host; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_baudrate(uint32_t baudrate) { this->config_.baudrate = baudrate; }
  void set_data_bits(uint8_t data_bits);
  void set_parity(const std::string &parity);
  void set_stop_bits(uint8_t stop_bits);
  void set_flow_control(const std::string &flow_control);
  void set_dtr(bool dtr) { this->config_.dtr = dtr; }
  void set_rts(bool rts) { this->config_.rts = rts; }
  void set_device_node(const std::string &device_node) { this->device_node_ = device_node; }

  const std::string &get_device_node() const { return this->device_node_; }

  bool is_connected() const { return this->connected_; }
  bool connect();
  void disconnect();

  size_t write(const uint8_t *data, size_t length);
  size_t read(uint8_t *data, size_t length);
  size_t available() const { return this->rx_buffer_.size(); }
  void flush();
  void purge(PurgeData purge_flags);


  bool set_baudrate_runtime(uint32_t baudrate);
  bool set_data_size_runtime(DataSize data_size);
  bool set_parity_runtime(ParityMode parity);
  bool set_stop_bits_runtime(StopBits stop_bits);
  bool set_flow_control_runtime(FlowControl flow_control);

  bool set_dtr_runtime(bool state);
  bool set_rts_runtime(bool state);
  uint8_t get_modem_state() const { return this->modem_state_; }
  uint8_t get_line_state() const { return this->line_state_; }

  static const add_on_connect_callback(std::function<void()> callback) { this->on_connect_callbacks_.push_back(callback); }
  static const add_on_disconnect_callback(std::function<void()> callback) {
    this->on_disconnect_callbacks_.push_back(callback);
  }
  void add_on_data_callback(std::function<void(const std::vector<uint8_t> &)> callback) {
    this->on_data_callbacks_.push_back(callback);
  }

 protected:
  std::string host_;
  uint16_t port_{2217};
  SerialConfig config_;
  std::string device_node_;

#ifdef USE_ESP_IDF
  int socket_{-1};
#else
  std::unique_ptr<WiFiClient> client_;
#endif

  bool connected_{false};
  bool telnet_negotiated_{false};
  uint32_t last_connect_attempt_{0};
  static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;

  uint8_t modem_state_{0};
  uint8_t line_state_{0};
  bool flow_suspended_{false};

  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> tx_buffer_;
  std::vector<uint8_t> telnet_buffer_;

  static constexpr size_t MAX_BUFFER_SIZE = 2048;

  std::vector<std::function<void()>> on_connect_callbacks_;
  std::vector<std::function<void()>> on_disconnect_callbacks_;
  std::vector<std::function<void(const std::vector<uint8_t> &)>> on_data_callbacks_;

  bool connect_socket_();
  void close_socket_();
  bool send_raw_(const uint8_t *data, size_t length);
  size_t receive_raw_(uint8_t *buffer, size_t length);

  bool negotiate_telnet_();
  void send_telnet_command_(TelnetCommand cmd, TelnetOption option);
  void send_telnet_subnegotiation_(const uint8_t *data, size_t length);
  void process_telnet_data_(const uint8_t *data, size_t length);
  void handle_telnet_command_(TelnetCommand cmd, TelnetOption option);
  void handle_telnet_subnegotiation_(const uint8_t *data, size_t length);

  bool send_com_port_command_(uint8_t command, const uint8_t *data, size_t length);
  bool send_com_port_command_uint32_(uint8_t command, uint32_t value);
  void handle_com_port_control_(const uint8_t *data, size_t length);
  void apply_serial_config_();
};
class NetworkSerialServer : public Component {
 public:
  NetworkSerialServer() = default;
  ~NetworkSerialServer();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_port(uint16_t port) { this->port_ = port; }
  void set_uart(uart::UARTComponent *uart) { this->uart_ = uart; }
  bool has_client() const { return this->client_connected_; }

 protected:
  uint16_t port_{2217};
  uart::UARTComponent *uart_{nullptr};
  int listen_socket_{-1};
  int client_socket_{-1};

  bool client_connected_{false};
  bool telnet_negotiated_{false};

  SerialConfig client_config_;
  uint8_t modem_state_{0};
  uint8_t line_state_{0};
  uint8_t modem_state_mask_{0xFF};
  uint8_t line_state_mask_{0};
  std::vector<uint8_t> telnet_parse_buf_;
  static constexpr size_t BRIDGE_BUF_SIZE = 256;

  bool start_listening_();
  void stop_listening_();
  void accept_client_();
  void disconnect_client_();
  bool send_to_client_(const uint8_t *data, size_t length);
  size_t receive_from_client_(uint8_t *buffer, size_t length);

  void send_telnet_negotiation_();
  void send_telnet_command_(TelnetCommand cmd, TelnetOption option);
  void send_telnet_subnegotiation_(const uint8_t *data, size_t length);
  void process_client_data_(const uint8_t *data, size_t length);
  void handle_telnet_command_(TelnetCommand cmd, TelnetOption option);
  void handle_telnet_subnegotiation_(const uint8_t *data, size_t length);

  void send_com_port_response_(uint8_t command, const uint8_t *data, size_t length);
  void send_com_port_response_uint32_(uint8_t command, uint32_t value);
  void handle_com_port_command_(const uint8_t *data, size_t length);
  void apply_uart_config_();
  void send_modem_state_();
  void send_initial_modem_state_();

  void bridge_uart_to_client_();
};

}  // namespace network_serial
}  // namespace esphome
