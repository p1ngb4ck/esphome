#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"

#include <vector>
#include <queue>

namespace esphome {
namespace opendps {

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

  // Firmware upgrade
  void start_firmware_upgrade(const std::string &firmware_url);
  void upgrade_progress_callback(std::function<void(uint8_t)> callback) { this->upgrade_progress_callback_ = callback; }

  // Get current data
  const OpenDPSData &get_data() const { return this->data_; }

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
  std::string unpack_cstr_(const std::vector<uint8_t> &frame, size_t &pos);

  // CRC calculation
  uint16_t crc16_ccitt_(uint16_t crc, uint8_t data);
  uint16_t calculate_crc_(const std::vector<uint8_t> &data);

  // Escape/unescape
  std::vector<uint8_t> escape_frame_(const std::vector<uint8_t> &payload, uint16_t crc);
  bool unescape_frame_(std::vector<uint8_t> &frame);

  // Send commands
  void send_query_();
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
  uint32_t update_interval_{1000};  // Default 1Hz, can be set much faster
  uint32_t last_query_{0};

  // Frame reception state
  std::vector<uint8_t> rx_buffer_;
  bool receiving_frame_{false};
  uint32_t frame_start_time_{0};
  static const uint32_t FRAME_TIMEOUT_MS = 500;
};

}  // namespace opendps
}  // namespace esphome
