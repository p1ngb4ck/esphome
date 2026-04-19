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

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/time.h"
#endif

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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace esphome {

// Forward declaration for storage
namespace storage {
class StorageDevice;
}  // namespace storage

namespace opendps {

// Datalogger column flags (bitmask)
enum DatalogColumn : uint16_t {
  DATALOG_COL_ELAPSED_MS = (1 << 0),   // Time since log start (milliseconds)
  DATALOG_COL_SYSTEM_TIME = (1 << 1),  // System time (from time component, ISO8601 format)
  DATALOG_COL_VOLTAGE_IN = (1 << 2),
  DATALOG_COL_VOLTAGE_OUT = (1 << 3),
  DATALOG_COL_CURRENT_OUT = (1 << 4),
  DATALOG_COL_POWER_OUT = (1 << 5),
  DATALOG_COL_OUTPUT_ENABLED = (1 << 6),
  DATALOG_COL_TEMP1 = (1 << 7),
  DATALOG_COL_TEMP2 = (1 << 8),
  // Legacy alias for backwards compatibility
  DATALOG_COL_TIMESTAMP = DATALOG_COL_ELAPSED_MS,
  // Default: elapsed time + system time + measurements (no temperature)
  DATALOG_COL_DEFAULT = DATALOG_COL_ELAPSED_MS | DATALOG_COL_SYSTEM_TIME | DATALOG_COL_VOLTAGE_IN |
                        DATALOG_COL_VOLTAGE_OUT | DATALOG_COL_CURRENT_OUT | DATALOG_COL_POWER_OUT |
                        DATALOG_COL_OUTPUT_ENABLED,
};

// Datalogger configuration
struct DataloggerConfig {
  std::string storage_path{"/sd/logs"};  // Base path for log files
  std::string filename_format;           // strftime format, e.g., "dps_%Y%m%d_%H%M%S.csv"
  std::string filename_id;               // Fixed filename ID, e.g., "session1" -> "session1.csv"
  uint32_t buffer_size{65536};           // Buffer size in bytes (default 64KB)
  std::string format{"csv"};             // "csv" or "bin"
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
  CMD_SET_BAUD = 23,
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
  CONN_NETWORK_OFF = 0,
  CONN_WIFI_CONNECTING = 1,
  CONN_WIFI_CONNECTED = 2,
  CONN_WIFI_ERROR = 3,
  CONN_WIFI_UPGRADING = 4,
  // Ethernet status (extended for ESPHome)
  CONN_ETHERNET_CONNECTING = 5,
  CONN_ETHERNET_CONNECTED = 6,
  CONN_ETHERNET_ERROR = 7
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

// Calibration assistant states - matches dpsctl.py -C flow exactly
// All user inputs are collected via calibration_assistant_step(value)
enum CalibrationAssistantState : uint8_t {
  CAL_IDLE = 0,

  // === Input Voltage Calibration (two-point) ===
  // User connects lower supply voltage, enters measured mV
  CAL_VIN_LOW_WAIT_INPUT,  // Waiting for user to enter measured Vin LOW in mV
  CAL_VIN_LOW_RECORD,      // Record ADC reading for low point
  // UART paused: user swaps supply, enters higher voltage mV, presses step to resume + record
  CAL_UART_PAUSED,      // UART reading suspended, step resumes and records Vin HIGH
  CAL_VIN_HIGH_RECORD,  // Record ADC reading for high point
  CAL_VIN_CALCULATE,    // Calculate VIN_ADC_K and VIN_ADC_C

  // === Output Voltage Calibration ===
  CAL_VOUT_SWEEP,            // Auto-sweep to find max V_DAC (no user input)
  CAL_VOUT_LOW_WAIT_INPUT,   // Set V_DAC to 10%, wait for user measured mV
  CAL_VOUT_LOW_RECORD,       // Record V_ADC for low point
  CAL_VOUT_HIGH_WAIT_INPUT,  // Set V_DAC to 90%, wait for user measured mV
  CAL_VOUT_HIGH_RECORD,      // Record V_ADC for high point
  CAL_VOUT_CALCULATE,        // Calculate V_ADC_K/C and V_DAC_K/C

  // === Output Current Calibration (requires load) ===
  CAL_IOUT_MAX_CURRENT_INPUT,      // User enters max DPS current (A)
  CAL_IOUT_LOAD_RESISTANCE_INPUT,  // User enters load resistance (Ohm)
  CAL_IOUT_LOAD_WATTAGE_INPUT,     // User enters load max wattage (W)
  CAL_IOUT_CONNECT_LOAD,           // User connects load, presses step to confirm
  CAL_IOUT_SWEEP,                  // Auto-sweep current readings (no user input)
  CAL_IOUT_CALCULATE,              // Calculate A_ADC_K and A_ADC_C

  // === Constant Current (CC) Calibration (requires short) ===
  CAL_CC_SHORT_OUTPUT,   // User shorts output, presses step to confirm
  CAL_CC_SWEEP,          // Auto-sweep A_DAC range to find linear region (no user input)
  CAL_CC_MEASURE_SWEEP,  // Measure current at points in linear range (no user input)
  CAL_CC_CALCULATE,      // Calculate A_DAC_K and A_DAC_C

  // === Done ===
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
  void set_tcp_bridge_disconnect_delay(uint32_t delay_ms) { this->tcp_bridge_disconnect_delay_ms_ = delay_ms; }
  void set_tcp_bridge_frame_timeout(uint32_t timeout_ms) { this->tcp_bridge_frame_timeout_ms_ = timeout_ms; }
  void set_bootloader_baud_rate(uint32_t baud_rate) { this->bootloader_baud_rate_ = baud_rate; }
  void set_bootloader_legacy(bool legacy) { this->bootloader_legacy_ = legacy; }

  // Sensor registration
  void set_voltage_in_sensor(sensor::Sensor *sensor) { this->voltage_in_sensor_ = sensor; }
  void set_voltage_out_sensor(sensor::Sensor *sensor) { this->voltage_out_sensor_ = sensor; }
  void set_current_out_sensor(sensor::Sensor *sensor) { this->current_out_sensor_ = sensor; }
  void set_power_out_sensor(sensor::Sensor *sensor) { this->power_out_sensor_ = sensor; }
  void set_temp1_sensor(sensor::Sensor *sensor) { this->temp1_sensor_ = sensor; }
  void set_temp2_sensor(sensor::Sensor *sensor) { this->temp2_sensor_ = sensor; }
  void set_voltage_set_sensor(sensor::Sensor *sensor) { this->voltage_set_sensor_ = sensor; }
  void set_current_set_sensor(sensor::Sensor *sensor) { this->current_set_sensor_ = sensor; }
  void set_output_enabled_binary_sensor(binary_sensor::BinarySensor *sensor) {
    this->output_enabled_binary_sensor_ = sensor;
  }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *sensor) { this->connected_binary_sensor_ = sensor; }

  // Actions
  void enable_output(bool enable);
  void set_function(const std::string &function);
  void set_parameter(const std::string &key, const std::string &value);
  void set_voltage(float voltage);
  void set_current(float current);
  void set_brightness(uint8_t brightness);
  void set_uart_baud(uint32_t baud);
  void send_ping();
  void request_version();
  void lock(bool locked);
  void send_connection_status(ConnectionStatus status);

  // Calibration
  void request_calibration_report();
  void set_calibration(const std::string &name, float value);
  void clear_calibration();

  // Calibration backup/restore to storage
  void set_calibration_backup_path(const std::string &path) { this->calibration_backup_path_ = path; }
  void set_auto_restore_calibration(bool enabled) { this->auto_restore_calibration_ = enabled; }
  bool save_calibration_to_storage();
  bool save_calibration_to_storage(const std::string &path);
  bool restore_calibration_from_storage();
  bool restore_calibration_from_storage(const std::string &path);
  bool has_calibration_backup() const;
  bool has_calibration_backup(const std::string &path) const;

  // Calibration Assistant - interactive calibration like dpsctl.py -C
  // Call start_calibration_assistant() to begin, then call calibration_assistant_step()
  // with user-provided values at each step. Monitor logs for prompts/instructions.
  void start_calibration_assistant();
  void calibration_assistant_step(float value);
  void cancel_calibration_assistant();
  CalibrationAssistantState get_calibration_assistant_state() const { return this->cal_assistant_state_; }

  // Firmware upgrade
  void start_firmware_upgrade(const std::string &firmware_path);
  void set_upgrade_progress_callback(std::function<void(uint8_t)> &&callback) {
    this->upgrade_progress_callback_ = std::move(callback);
  }
  void set_upgrade_complete_callback(std::function<void(bool, const char *)> &&callback) {
    this->upgrade_complete_callback_ = std::move(callback);
  }

  // Get current data
  const OpenDPSData &get_data() const { return this->data_; }
  const CalibrationData &get_calibration_data() const { return this->calibration_data_; }

  // Get current settings (from params, in user units)
  float get_voltage_setting() const;
  float get_current_setting() const;
  uint8_t get_brightness_setting() const { return this->brightness_; }
  bool is_connected() const { return this->connected_; }

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

#ifdef USE_TIME
  /// Set time component for system_time column in datalogger
  void set_datalog_time(time::RealTimeClock *rtc) { this->datalog_time_ = rtc; }
#endif

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
  sensor::Sensor *voltage_set_sensor_{nullptr};
  sensor::Sensor *current_set_sensor_{nullptr};
  binary_sensor::BinarySensor *output_enabled_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_binary_sensor_{nullptr};

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
  std::function<void(bool, const char *)> upgrade_complete_callback_{nullptr};
  bool upgrade_in_progress_{false};
  uint8_t *upgrade_firmware_data_{nullptr};  // PSRAM-allocated buffer
  size_t upgrade_firmware_size_{0};
  size_t upgrade_offset_{0};
  uint16_t upgrade_chunk_size_{1024};
  uint32_t upgrade_last_chunk_time_{0};
  uint32_t upgrade_bootloader_ready_time_{0};
  uint32_t upgrade_cal_restore_time_{0};
  uint8_t upgrade_retry_count_{0};
  uint16_t upgrade_crc_{0};
  uint32_t bootloader_baud_rate_{0};        // Bootloader baud rate (0 = use same as UART)
  uint32_t firmware_baud_rate_{0};          // Saved firmware baud rate to restore after upgrade
  bool bootloader_legacy_{false};           // Legacy bootloader: starts at configured baud, no cmd_set_baud
  ESPPreferenceObject upgrade_state_pref_;  // Persists upgrade state across reboots
  static const uint32_t UPGRADE_CHUNK_TIMEOUT_MS = 3000;
  static const uint8_t UPGRADE_MAX_RETRIES = 3;

  // Connection state
  bool connected_{false};
  CallbackManager<void()> on_connect_callback_;
  CallbackManager<void()> on_calibration_callback_;

  // Calibration assistant state
  CalibrationAssistantState cal_assistant_state_{CAL_IDLE};
  std::vector<float> cal_samples_x_;  // X values for linear regression
  std::vector<float> cal_samples_y_;  // Y values for linear regression
  uint16_t cal_sweep_step_{0};
  uint16_t cal_max_v_dac_{4095};
  // Intermediate calibration coefficients (calculated during calibration)
  float cal_v_adc_k_{0}, cal_v_adc_c_{0};
  float cal_v_dac_k_{0}, cal_v_dac_c_{0};
  float cal_a_adc_k_{0}, cal_a_adc_c_{0};
  uint16_t cal_a_dac_lower_{0}, cal_a_dac_upper_{4095};
  // User-provided calibration parameters (collected during assistant)
  float cal_vin_low_mv_{0};          // Measured input voltage LOW (mV)
  float cal_vin_high_mv_{0};         // Measured input voltage HIGH (mV)
  float cal_max_dps_current_{5.0f};  // Max output current of DPS model (A)
  float cal_load_resistance_{0};     // Load resistance (Ohm)
  float cal_load_max_wattage_{0};    // Load max wattage (W)
  uint32_t cal_last_action_time_{0};
  void cal_assistant_collect_sample_();

  // Calibration assistant helpers
  void cal_assistant_process_();
  std::pair<float, float> cal_best_fit_(const std::vector<float> &x, const std::vector<float> &y);

  // Calibration backup/restore
  std::string calibration_backup_path_{"/sd/opendps_calibration.cfg"};  // Default path for calibration backup
  bool auto_restore_calibration_{false};  // Auto-restore calibration after successful firmware upgrade

  // Firmware upgrade helpers
  void send_next_upgrade_chunk_();
  void start_upgrade_sequence_();
  bool upgrade_firmware_alloc_(size_t size);
  void upgrade_firmware_free_();

  // TCP bridge configuration (always present for Python codegen compatibility)
  bool tcp_bridge_enabled_{false};
  uint16_t tcp_bridge_port_{5005};
  uint32_t tcp_bridge_disconnect_delay_ms_{500};  // Grace period before exiting bridge mode
  uint32_t tcp_bridge_frame_timeout_ms_{500};     // Timeout for incomplete UART frames

  //========================================================================
  // Datalogger state
  //========================================================================
  DataloggerConfig datalog_config_;
  bool datalog_active_{false};
#ifdef USE_TIME
  time::RealTimeClock *datalog_time_{nullptr};  // Optional time component for system_time column
#endif
  std::string datalog_filepath_;                             // Full absolute path (e.g., "/sd/logs/file.csv")
  std::string datalog_relative_path_;                        // Path relative to mount point (e.g., "logs/file.csv")
  storage::StorageDevice *datalog_storage_device_{nullptr};  // Cached storage device
  uint32_t datalog_sample_count_{0};
  uint32_t datalog_start_time_{0};
  uint32_t datalog_last_flush_{0};

  // Triple-buffer system for async writes (main loop always has a buffer to write to)
  // Buffer states: WRITING (main loop), QUEUED (waiting for flush), FLUSHING (being written to SD)
  static const uint8_t DATALOG_NUM_BUFFERS = 3;
  uint8_t *datalog_buffers_[DATALOG_NUM_BUFFERS]{nullptr, nullptr, nullptr};
  size_t datalog_buffer_sizes_[DATALOG_NUM_BUFFERS]{0, 0, 0};  // Data size in each buffer
  size_t datalog_buffer_capacity_{0};                          // Capacity of each buffer
  uint8_t datalog_write_idx_{0};                               // Buffer currently being written by main loop
  uint8_t datalog_queue_idx_{0xFF};                            // Buffer queued for flushing (or 0xFF if none)
  uint8_t datalog_flush_idx_{0xFF};                            // Buffer currently being flushed (0xFF if none)
  bool datalog_buffer_in_psram_{false};

#ifdef USE_ESP32
  // FreeRTOS synchronization for async writes
  SemaphoreHandle_t datalog_mutex_{nullptr};      // Protects buffer state changes
  SemaphoreHandle_t datalog_write_sem_{nullptr};  // Signals write task
  TaskHandle_t datalog_task_handle_{nullptr};     // Background write task
  bool datalog_task_running_{false};
  static void datalog_write_task_(void *arg);  // FreeRTOS task function
  void datalog_rotate_buffers_();              // Rotate buffers for triple-buffering
#endif

  // Datalogger helper methods
  void datalog_write_sample_();
  void datalog_write_csv_header_();
  bool datalog_ensure_buffer_();
  void datalog_flush_buffer_();
  void datalog_request_flush_();  // Non-blocking: signals write task
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
  std::vector<uint8_t> tcp_uart_buffer_;    // Buffer for assembling complete UART frames
  uint32_t tcp_uart_buffer_start_time_{0};  // Timeout tracking for UART frame buffering
  uint32_t tcp_client_disconnect_time_{0};  // When client disconnected (for grace period)
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
