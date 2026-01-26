#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"

#ifdef USE_SOCKET_IMPL_LWIP_TCP
#include "esphome/components/socket/socket.h"
#endif
#ifdef USE_SOCKET_IMPL_BSD_SOCKETS
#include "esphome/components/socket/socket.h"
#endif

#include <vector>
#include <queue>
#include <map>
#include <functional>
#include <memory>

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <esp_psram.h>
#endif

namespace esphome {
namespace opendps {

// Datalogger output format
enum class DatalogFormat : uint8_t {
  CSV = 0,     // CSV format (timestamp,vin,vout,iout,power,enabled)
  BINARY = 1,  // Binary format (compact, faster writes)
};

// Datalogger column flags (bitmask)
enum DatalogColumn : uint16_t {
  DATALOG_COL_TIMESTAMP = (1 << 0),
  DATALOG_COL_VOLTAGE_IN = (1 << 1),
  DATALOG_COL_VOLTAGE_OUT = (1 << 2),
  DATALOG_COL_CURRENT_OUT = (1 << 3),
  DATALOG_COL_POWER_OUT = (1 << 4),
  DATALOG_COL_OUTPUT_ENABLED = (1 << 5),
  DATALOG_COL_TEMP1 = (1 << 6),
  DATALOG_COL_TEMP2 = (1 << 7),
  // Default: all columns except temperature
  DATALOG_COL_DEFAULT = DATALOG_COL_TIMESTAMP | DATALOG_COL_VOLTAGE_IN | DATALOG_COL_VOLTAGE_OUT |
                        DATALOG_COL_CURRENT_OUT | DATALOG_COL_POWER_OUT | DATALOG_COL_OUTPUT_ENABLED,
};

// Datalogger configuration
struct DataloggerConfig {
  std::string storage_path{"/sd/logs"};  // Base path for log files
  std::string filename_format;           // strftime format, e.g., "dps_%Y%m%d_%H%M%S.csv"
  std::string filename_id;               // Fixed filename ID, e.g., "session1" -> "session1.csv"
  uint32_t buffer_size{65536};           // Buffer size in bytes (default 64KB)
  DatalogFormat format{DatalogFormat::CSV};
  uint16_t columns{DATALOG_COL_DEFAULT};
  uint32_t flush_interval_ms{5000};  // Auto-flush interval (0 = disabled)
};

// OpenDPS Protocol Commands
enum OpenDPSCommand : uint8_t {
  CMD_PING = 1,
  CMD_QUERY = 4,
  CMD_WIFI_STATUS = 6,
  CMD_LOCK = 7,
  CMD_OCP_EVENT = 8,
  CMD_UPGRADE_START = 9,
  CMD_UPGRADE_DATA = 10,
  CMD_SET_FUNCTION = 11,
  CMD_ENABLE_OUTPUT = 12,
  CMD_LIST_FUNCTIONS = 13,
  CMD_SET_PARAMETERS = 14,
  CMD_LIST_PARAMETERS = 15,
  CMD_TEMPERATURE_REPORT = 16,
  CMD_VERSION = 17,
  CMD_CAL_REPORT = 18,
  CMD_SET_CALIBRATION = 19,
  CMD_CLEAR_CALIBRATION = 20,
  CMD_CHANGE_SCREEN = 21,
  CMD_SET_BRIGHTNESS = 22,
  CMD_RESPONSE = 0x80
};

// Frame delimiters
static const uint8_t FRAME_SOF = 0x7E;
static const uint8_t FRAME_EOF = 0x7F;
static const uint8_t FRAME_DLE = 0x7D;
static const uint8_t FRAME_XOR = 0x20;

// Upgrade status codes
enum UpgradeStatus : uint8_t {
  UPGRADE_CONTINUE = 0,
  UPGRADE_BOOTCOM_ERROR = 1,
  UPGRADE_CRC_ERROR = 2,
  UPGRADE_ERASE_ERROR = 3,
  UPGRADE_FLASH_ERROR = 4,
  UPGRADE_OVERFLOW_ERROR = 5,
  UPGRADE_SUCCESS = 16
};

// Connection status codes (sent to OpenDPS to update display icon)
enum ConnectionStatus : uint8_t {
  // WiFi status (original OpenDPS values)
  CONN_WIFI_OFF = 0,
  CONN_WIFI_CONNECTING = 1,
  CONN_WIFI_CONNECTED = 2,
  CONN_WIFI_ERROR = 3,
  CONN_WIFI_UPGRADING = 4,
  // Ethernet status (extended for ESPHome)
  CONN_ETHERNET_OFF = 10,
  CONN_ETHERNET_CONNECTING = 11,
  CONN_ETHERNET_CONNECTED = 12,
  CONN_ETHERNET_ERROR = 13,
  // DPS firmware upgrade (shows arrow icon)
  CONN_DPS_UPGRADING = 20
};

struct OpenDPSData {
  float v_in{0};
  float v_out{0};
  float i_out{0};
  bool output_enabled{false};
  float temp1{0};
  float temp2{0};
  bool temp_shutdown{false};
  std::string cur_func;
  std::map<std::string, std::string> params;
  uint32_t last_update{0};
};

// Calibration assistant parameters
struct CalibrationAssistantParams {
  float vin_measured_mv{0};    // Measured input voltage in mV (from multimeter)
  float load_resistance{0};    // Load resistance in ohms for current calibration
  float load_max_wattage{0};   // Load max wattage for current calibration
  float max_dps_current{5.0};  // Max output current of DPS model (default 5A)
};

// Calibration assistant states
enum CalibrationAssistantState : uint8_t {
  CAL_IDLE = 0,
  // Input voltage calibration (single-point with measured value)
  CAL_VIN_START,
  CAL_VIN_MEASURE,
  CAL_VIN_CALCULATE,
  // Output voltage calibration
  CAL_VOUT_START,
  CAL_VOUT_SWEEP,
  CAL_VOUT_MEASURE_LOW,
  CAL_VOUT_WAIT_HIGH,
  CAL_VOUT_MEASURE_HIGH,
  CAL_VOUT_CALCULATE,
  // Output current calibration (requires load)
  CAL_IOUT_START,
  CAL_IOUT_SWEEP,
  CAL_IOUT_CALCULATE,
  // Current limit calibration (requires short)
  CAL_ILIMIT_START,
  CAL_ILIMIT_SWEEP,
  CAL_ILIMIT_MEASURE,
  CAL_ILIMIT_CALCULATE,
  // Done
  CAL_COMPLETE,
  CAL_ERROR
};

// Calibration report data (raw ADC/DAC readings and calibration coefficients)
struct CalibrationData {
  // Raw ADC/DAC readings
  uint16_t vout_adc{0};
  uint16_t vin_adc{0};
  uint16_t iout_adc{0};
  uint16_t iout_dac{0};
  uint16_t vout_dac{0};
  // Calibration coefficients
  float a_adc_k{0};
  float a_adc_c{0};
  float a_dac_k{0};
  float a_dac_c{0};
  float v_adc_k{0};
  float v_adc_c{0};
  float v_dac_k{0};
  float v_dac_c{0};
  float vin_adc_k{0};
  float vin_adc_c{0};
  uint32_t last_update{0};
};

class OpenDPS;

/// Component to communicate with OpenDPS power supply via UART
class OpenDPS : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Configuration
  void set_update_interval(uint32_t interval) { this->update_interval_ = interval; }
  void set_default_brightness(uint8_t brightness) { this->default_brightness_ = brightness; }
  void set_tcp_bridge_enabled(bool enabled) { this->tcp_bridge_enabled_ = enabled; }
  void set_tcp_bridge_port(uint16_t port) { this->tcp_bridge_port_ = port; }

  // Sensor registration
  void set_voltage_in_sensor(sensor::Sensor *sensor) { this->voltage_in_sensor_ = sensor; }
  void set_voltage_out_sensor(sensor::Sensor *sensor) { this->voltage_out_sensor_ = sensor; }
  void set_current_out_sensor(sensor::Sensor *sensor) { this->current_out_sensor_ = sensor; }
  void set_power_out_sensor(sensor::Sensor *sensor) { this->power_out_sensor_ = sensor; }
  void set_temp1_sensor(sensor::Sensor *sensor) { this->temp1_sensor_ = sensor; }
  void set_temp2_sensor(sensor::Sensor *sensor) { this->temp2_sensor_ = sensor; }
  void set_output_enabled_binary_sensor(binary_sensor::BinarySensor *sensor) {
    this->output_enabled_binary_sensor_ = sensor;
  }

  // Actions
  void enable_output(bool enable);
  void set_function(const std::string &function);
  void set_parameter(const std::string &key, const std::string &value);
  void set_voltage(float voltage);
  void set_current(float current);
  void set_brightness(uint8_t brightness);
  void send_ping();
  void request_version();
  void lock(bool locked);
  void send_connection_status(ConnectionStatus status);

  // Calibration
  void request_calibration_report();
  void set_calibration(const std::string &name, float value);
  void clear_calibration();

  // Calibration Assistant - interactive calibration like dpsctl.py -C
  // Call start_calibration_assistant() to begin, then call calibration_assistant_step()
  // with user-provided measurements at each step. Check get_calibration_assistant_state()
  // for current state and get_calibration_assistant_prompt() for user instructions.
  void start_calibration_assistant(const CalibrationAssistantParams &params);
  void calibration_assistant_step(float measured_value);
  void cancel_calibration_assistant();
  CalibrationAssistantState get_calibration_assistant_state() const { return this->cal_assistant_state_; }
  const char *get_calibration_assistant_prompt() const;

  // Firmware upgrade
  void start_firmware_upgrade(const std::string &firmware_path);
  void set_upgrade_progress_callback(std::function<void(uint8_t)> &&callback) {
    this->upgrade_progress_callback_ = std::move(callback);
  }

  // Get current data
  const OpenDPSData &get_data() const { return this->data_; }
  const CalibrationData &get_calibration_data() const { return this->calibration_data_; }

  // Get current settings (from params, in user units)
  float get_voltage_setting() const;
  float get_current_setting() const;
  uint8_t get_brightness_setting() const { return this->brightness_; }

  // Connection trigger
  void add_on_connect_callback(std::function<void()> callback) { this->on_connect_callback_.add(std::move(callback)); }

  // Calibration data callback
  void add_on_calibration_callback(std::function<void()> callback) {
    this->on_calibration_callback_.add(std::move(callback));
  }

  //========================================================================
  // Datalogger - High-speed data logging with PSRAM buffering
  //========================================================================

  /// Configure datalogger settings (call before start_datalog)
  void set_datalogger_config(const DataloggerConfig &config) { this->datalog_config_ = config; }

  /// Start data logging
  /// @param filename Optional filename (uses config format/id if empty)
  /// @return true if logging started successfully
  bool start_datalog(const std::string &filename = "");

  /// Stop data logging and flush remaining buffer
  void stop_datalog();

  /// Force flush buffer to storage
  void flush_datalog();

  /// Check if datalogger is active
  bool is_datalog_active() const { return this->datalog_active_; }

  /// Get current log file path
  const std::string &get_datalog_filepath() const { return this->datalog_filepath_; }

  /// Get number of samples logged in current session
  uint32_t get_datalog_sample_count() const { return this->datalog_sample_count_; }

 protected:
  // Frame handling
  void send_frame_(const std::vector<uint8_t> &payload);
  bool read_frame_();
  void process_frame_(const std::vector<uint8_t> &payload);

  // Frame building helpers
  void pack8_(std::vector<uint8_t> &frame, uint8_t byte);
  void pack16_(std::vector<uint8_t> &frame, uint16_t halfword);
  void pack_cstr_(std::vector<uint8_t> &frame, const std::string &str);

  // Frame parsing helpers
  uint8_t unpack8_(const std::vector<uint8_t> &frame, size_t &pos);
  uint16_t unpack16_(const std::vector<uint8_t> &frame, size_t &pos);
  uint32_t unpack32_(const std::vector<uint8_t> &frame, size_t &pos);
  float unpack_float_(const std::vector<uint8_t> &frame, size_t &pos);
  std::string unpack_cstr_(const std::vector<uint8_t> &frame, size_t &pos);

  // CRC calculation
  uint16_t crc16_ccitt_(uint16_t crc, uint8_t data);
  uint16_t calculate_crc_(const std::vector<uint8_t> &data);

  // Escape/unescape
  std::vector<uint8_t> escape_frame_(const std::vector<uint8_t> &payload, uint16_t crc);
  bool unescape_frame_(std::vector<uint8_t> &frame);

  // Send commands
  void send_query_();

  // Firmware upgrade helpers
  void send_upgrade_start_(uint16_t chunk_size, uint16_t crc);
  void send_upgrade_data_(const std::vector<uint8_t> &data);
  void send_command_(OpenDPSCommand cmd);

  // Sensors
  sensor::Sensor *voltage_in_sensor_{nullptr};
  sensor::Sensor *voltage_out_sensor_{nullptr};
  sensor::Sensor *current_out_sensor_{nullptr};
  sensor::Sensor *power_out_sensor_{nullptr};
  sensor::Sensor *temp1_sensor_{nullptr};
  sensor::Sensor *temp2_sensor_{nullptr};
  binary_sensor::BinarySensor *output_enabled_binary_sensor_{nullptr};

  // State
  OpenDPSData data_;
  CalibrationData calibration_data_;
  uint32_t update_interval_{1000};  // Default 1Hz, can be set much faster
  uint32_t last_query_{0};

  // Brightness (stored in preferences since device doesn't report it)
  uint8_t brightness_{50};
  uint8_t default_brightness_{50};
  ESPPreferenceObject brightness_pref_;
  uint32_t brightness_set_time_{0};  // Time when brightness was last set (to skip sync briefly)

  // Frame reception state
  std::vector<uint8_t> rx_buffer_;
  bool receiving_frame_{false};
  uint32_t frame_start_time_{0};
  static const uint32_t FRAME_TIMEOUT_MS = 500;

  // Firmware upgrade
  std::function<void(uint8_t)> upgrade_progress_callback_{nullptr};
  bool upgrade_in_progress_{false};
  std::vector<uint8_t> upgrade_firmware_data_;
  size_t upgrade_offset_{0};
  uint16_t upgrade_chunk_size_{1024};
  uint32_t upgrade_last_chunk_time_{0};
  uint8_t upgrade_retry_count_{0};
  uint16_t upgrade_crc_{0};
  static const uint32_t UPGRADE_CHUNK_TIMEOUT_MS = 3000;
  static const uint8_t UPGRADE_MAX_RETRIES = 3;

  // Connection state
  bool connected_{false};
  CallbackManager<void()> on_connect_callback_;
  CallbackManager<void()> on_calibration_callback_;

  // Calibration assistant state
  CalibrationAssistantState cal_assistant_state_{CAL_IDLE};
  CalibrationAssistantParams cal_assistant_params_;
  std::vector<float> cal_samples_x_;  // X values for linear regression
  std::vector<float> cal_samples_y_;  // Y values for linear regression
  uint16_t cal_sweep_step_{0};
  uint16_t cal_max_v_dac_{4095};
  float cal_v_adc_k_{0}, cal_v_adc_c_{0};
  float cal_v_dac_k_{0}, cal_v_dac_c_{0};
  float cal_a_adc_k_{0}, cal_a_adc_c_{0};
  uint16_t cal_a_dac_lower_{0}, cal_a_dac_upper_{4095};
  uint32_t cal_last_action_time_{0};

  // Calibration assistant helpers
  void cal_assistant_process_();
  void cal_assistant_collect_sample_();
  std::pair<float, float> cal_best_fit_(const std::vector<float> &x, const std::vector<float> &y);

  // Firmware upgrade helpers
  void send_next_upgrade_chunk_();

  // TCP bridge configuration (always present for Python codegen compatibility)
  bool tcp_bridge_enabled_{false};
  uint16_t tcp_bridge_port_{5005};

  //========================================================================
  // Datalogger state
  //========================================================================
  DataloggerConfig datalog_config_;
  bool datalog_active_{false};
  std::string datalog_filepath_;
  uint32_t datalog_sample_count_{0};
  uint32_t datalog_start_time_{0};
  uint32_t datalog_last_flush_{0};

  // PSRAM buffer for high-speed logging
  uint8_t *datalog_buffer_{nullptr};
  size_t datalog_buffer_size_{0};
  size_t datalog_buffer_pos_{0};
  bool datalog_buffer_in_psram_{false};

  // Datalogger helper methods
  void datalog_write_sample_();
  void datalog_write_csv_header_();
  bool datalog_ensure_buffer_();
  void datalog_flush_buffer_();
  std::string datalog_generate_filename_();
  size_t datalog_format_csv_row_(char *buffer, size_t max_len);
  size_t datalog_format_binary_row_(uint8_t *buffer, size_t max_len);

#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
  // TCP bridge for dpsctl.py access (port 5005 by default)
  void setup_tcp_bridge_();
  void loop_tcp_bridge_();
  bool tcp_bridge_initialized_{false};

  std::unique_ptr<socket::Socket> tcp_server_socket_;
  std::unique_ptr<socket::Socket> tcp_client_socket_;
  std::vector<uint8_t> tcp_uart_buffer_;  // Buffer for assembling complete UART frames
#endif
};

// Trigger for on_connect
class OpenDPSConnectTrigger : public Trigger<> {
 public:
  explicit OpenDPSConnectTrigger(OpenDPS *parent) {
    parent->add_on_connect_callback([this]() { this->trigger(); });
  }
};

}  // namespace opendps
}  // namespace esphome
