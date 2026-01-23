#include "opendps.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace opendps {

static const char *const TAG = "opendps";

void OpenDPS::setup() {
  ESP_LOGCONFIG(TAG, "Setting up OpenDPS...");
  // Send initial ping to check connection
  this->send_ping();
}

void OpenDPS::loop() {
  // Read incoming frames
  while (this->available()) {
    if (this->read_frame_()) {
      // Frame successfully received and processed
    }
  }

  // Send periodic query based on update interval
  uint32_t now = millis();
  if (now - this->last_query_ >= this->update_interval_) {
    this->send_query_();
    this->last_query_ = now;
  }
}

void OpenDPS::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenDPS:");
  ESP_LOGCONFIG(TAG, "  Update Interval: %dms", this->update_interval_);
  LOG_SENSOR("  ", "Voltage In", this->voltage_in_sensor_);
  LOG_SENSOR("  ", "Voltage Out", this->voltage_out_sensor_);
  LOG_SENSOR("  ", "Current Out", this->current_out_sensor_);
  LOG_SENSOR("  ", "Power Out", this->power_out_sensor_);
  LOG_SENSOR("  ", "Temperature 1", this->temp1_sensor_);
  LOG_SENSOR("  ", "Temperature 2", this->temp2_sensor_);
  LOG_BINARY_SENSOR("  ", "Output Enabled", this->output_enabled_binary_sensor_);
}

// ============================================================================
// CRC Calculation (CRC-16 CCITT)
// ============================================================================

uint16_t OpenDPS::crc16_ccitt_(uint16_t crc, uint8_t data) {
  uint8_t msb = crc >> 8;
  uint8_t lsb = crc & 0xFF;
  uint8_t x = data ^ msb;
  x ^= (x >> 4);
  msb = (lsb ^ (x >> 3) ^ (x << 4)) & 0xFF;
  lsb = (x ^ (x << 5)) & 0xFF;
  return (msb << 8) + lsb;
}

uint16_t OpenDPS::calculate_crc_(const std::vector<uint8_t> &data) {
  uint16_t crc = 0;
  for (uint8_t byte : data) {
    crc = this->crc16_ccitt_(crc, byte);
  }
  return crc;
}

// ============================================================================
// Frame Escaping / Unescaping
// ============================================================================

std::vector<uint8_t> OpenDPS::escape_frame_(const std::vector<uint8_t> &payload, uint16_t crc) {
  std::vector<uint8_t> frame;
  frame.reserve(payload.size() + 10);  // Reserve space

  // Start of frame
  frame.push_back(FRAME_SOF);

  // Escape and add payload
  for (uint8_t byte : payload) {
    if (byte == FRAME_SOF || byte == FRAME_EOF || byte == FRAME_DLE) {
      frame.push_back(FRAME_DLE);
      frame.push_back(byte ^ FRAME_XOR);
    } else {
      frame.push_back(byte);
    }
  }

  // Add CRC (MSB first, LSB second)
  uint8_t crc_msb = (crc >> 8) & 0xFF;
  uint8_t crc_lsb = crc & 0xFF;

  if (crc_msb == FRAME_SOF || crc_msb == FRAME_EOF || crc_msb == FRAME_DLE) {
    frame.push_back(FRAME_DLE);
    frame.push_back(crc_msb ^ FRAME_XOR);
  } else {
    frame.push_back(crc_msb);
  }

  if (crc_lsb == FRAME_SOF || crc_lsb == FRAME_EOF || crc_lsb == FRAME_DLE) {
    frame.push_back(FRAME_DLE);
    frame.push_back(crc_lsb ^ FRAME_XOR);
  } else {
    frame.push_back(crc_lsb);
  }

  // End of frame
  frame.push_back(FRAME_EOF);

  return frame;
}

bool OpenDPS::unescape_frame_(std::vector<uint8_t> &frame) {
  std::vector<uint8_t> unescaped;
  unescaped.reserve(frame.size());

  bool seen_dle = false;
  for (size_t i = 1; i < frame.size() - 1; i++) {  // Skip SOF and EOF
    uint8_t byte = frame[i];
    if (byte == FRAME_DLE) {
      seen_dle = true;
    } else if (seen_dle) {
      unescaped.push_back(byte ^ FRAME_XOR);
      seen_dle = false;
    } else {
      unescaped.push_back(byte);
    }
  }

  frame = unescaped;
  return true;
}

// ============================================================================
// Frame Packing Helpers
// ============================================================================

void OpenDPS::pack8_(std::vector<uint8_t> &frame, uint8_t byte) { frame.push_back(byte); }

void OpenDPS::pack16_(std::vector<uint8_t> &frame, uint16_t halfword) {
  frame.push_back((halfword >> 8) & 0xFF);  // MSB first
  frame.push_back(halfword & 0xFF);         // LSB second
}

void OpenDPS::pack_cstr_(std::vector<uint8_t> &frame, const std::string &str) {
  for (char ch : str) {
    frame.push_back(static_cast<uint8_t>(ch));
  }
  frame.push_back(0);  // Null terminator
}

// ============================================================================
// Frame Unpacking Helpers
// ============================================================================

uint8_t OpenDPS::unpack8_(const std::vector<uint8_t> &frame, size_t &pos) {
  if (pos >= frame.size())
    return 0;
  return frame[pos++];
}

uint16_t OpenDPS::unpack16_(const std::vector<uint8_t> &frame, size_t &pos) {
  uint16_t value = this->unpack8_(frame, pos) << 8;
  value |= this->unpack8_(frame, pos);
  return value;
}

std::string OpenDPS::unpack_cstr_(const std::vector<uint8_t> &frame, size_t &pos) {
  std::string str;
  while (pos < frame.size()) {
    uint8_t byte = frame[pos++];
    if (byte == 0)
      break;
    str += static_cast<char>(byte);
  }
  return str;
}

// ============================================================================
// Frame Transmission
// ============================================================================

void OpenDPS::send_frame_(const std::vector<uint8_t> &payload) {
  // Calculate CRC
  uint16_t crc = this->calculate_crc_(payload);

  // Escape frame
  std::vector<uint8_t> frame = this->escape_frame_(payload, crc);

  // Send frame
  this->write_array(frame.data(), frame.size());
  this->flush();

  ESP_LOGV(TAG, "TX Frame (%d bytes): %s", frame.size(), format_hex_pretty(frame).c_str());
}

// ============================================================================
// Frame Reception
// ============================================================================

bool OpenDPS::read_frame_() {
  uint32_t now = millis();

  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);

    // Check for frame timeout
    if (this->receiving_frame_ && (now - this->frame_start_time_) > FRAME_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Frame timeout, resetting buffer");
      this->rx_buffer_.clear();
      this->receiving_frame_ = false;
    }

    // Start of frame
    if (byte == FRAME_SOF) {
      this->rx_buffer_.clear();
      this->rx_buffer_.push_back(byte);
      this->receiving_frame_ = true;
      this->frame_start_time_ = now;
      continue;
    }

    // Receiving frame data
    if (this->receiving_frame_) {
      this->rx_buffer_.push_back(byte);

      // End of frame
      if (byte == FRAME_EOF) {
        this->receiving_frame_ = false;

        ESP_LOGV(TAG, "RX Frame (%d bytes): %s", this->rx_buffer_.size(), format_hex_pretty(this->rx_buffer_).c_str());

        // Validate frame
        if (this->rx_buffer_.size() < 4) {
          ESP_LOGW(TAG, "Frame too short");
          this->rx_buffer_.clear();
          return false;
        }

        // Unescape frame
        if (!this->unescape_frame_(this->rx_buffer_)) {
          ESP_LOGW(TAG, "Failed to unescape frame");
          this->rx_buffer_.clear();
          return false;
        }

        // Check CRC
        if (this->rx_buffer_.size() < 2) {
          ESP_LOGW(TAG, "Frame too short after unescape");
          this->rx_buffer_.clear();
          return false;
        }

        // Extract CRC from end of frame
        uint16_t received_crc =
            (this->rx_buffer_[this->rx_buffer_.size() - 2] << 8) | this->rx_buffer_[this->rx_buffer_.size() - 1];

        // Calculate CRC of payload (without CRC bytes)
        std::vector<uint8_t> payload(this->rx_buffer_.begin(), this->rx_buffer_.end() - 2);
        uint16_t calculated_crc = this->calculate_crc_(payload);

        if (received_crc != calculated_crc) {
          ESP_LOGW(TAG, "CRC mismatch: received 0x%04X, calculated 0x%04X", received_crc, calculated_crc);
          this->rx_buffer_.clear();
          return false;
        }

        // Process valid frame
        this->process_frame_(payload);
        this->rx_buffer_.clear();
        return true;
      }
    }
  }

  return false;
}

// ============================================================================
// Frame Processing
// ============================================================================

void OpenDPS::process_frame_(const std::vector<uint8_t> &payload) {
  if (payload.empty()) {
    ESP_LOGW(TAG, "Empty payload");
    return;
  }

  uint8_t command = payload[0];
  size_t pos = 1;

  ESP_LOGV(TAG, "Processing command: 0x%02X", command);

  // Handle response flag
  if (command & CMD_RESPONSE) {
    uint8_t original_cmd = command & ~CMD_RESPONSE;
    uint8_t status = this->unpack8_(payload, pos);

    ESP_LOGV(TAG, "Response to command 0x%02X, status: %d", original_cmd, status);

    switch (original_cmd) {
      case CMD_QUERY: {
        // Parse query response
        uint16_t v_in_raw = this->unpack16_(payload, pos);
        uint16_t v_out_raw = this->unpack16_(payload, pos);
        uint16_t i_out_raw = this->unpack16_(payload, pos);
        uint8_t output_enabled = this->unpack8_(payload, pos);

        // Convert to float (values are in mV and mA)
        this->data_.v_in = v_in_raw / 1000.0f;
        this->data_.v_out = v_out_raw / 1000.0f;
        this->data_.i_out = i_out_raw / 1000.0f;
        this->data_.output_enabled = output_enabled != 0;

        // Temperature readings (optional, check if data available)
        if (pos + 4 < payload.size()) {
          int16_t temp1_raw = this->unpack16_(payload, pos);
          int16_t temp2_raw = this->unpack16_(payload, pos);

          if (temp1_raw != -1) {
            this->data_.temp1 = temp1_raw / 10.0f;
          }
          if (temp2_raw != -1) {
            this->data_.temp2 = temp2_raw / 10.0f;
          }

          if (pos < payload.size()) {
            this->data_.temp_shutdown = this->unpack8_(payload, pos) != 0;
          }
        }

        // Current function name
        if (pos < payload.size()) {
          this->data_.cur_func = this->unpack_cstr_(payload, pos);
        }

        // Parameters (key-value pairs)
        this->data_.params.clear();
        while (pos < payload.size()) {
          std::string key = this->unpack_cstr_(payload, pos);
          if (key.empty())
            break;
          std::string value = this->unpack_cstr_(payload, pos);
          this->data_.params[key] = value;
        }

        this->data_.last_update = millis();

        // Publish sensor values
        if (this->voltage_in_sensor_ != nullptr)
          this->voltage_in_sensor_->publish_state(this->data_.v_in);
        if (this->voltage_out_sensor_ != nullptr)
          this->voltage_out_sensor_->publish_state(this->data_.v_out);
        if (this->current_out_sensor_ != nullptr)
          this->current_out_sensor_->publish_state(this->data_.i_out);
        if (this->power_out_sensor_ != nullptr) {
          float power = this->data_.v_out * this->data_.i_out;
          this->power_out_sensor_->publish_state(power);
        }
        if (this->temp1_sensor_ != nullptr && this->data_.temp1 != 0)
          this->temp1_sensor_->publish_state(this->data_.temp1);
        if (this->temp2_sensor_ != nullptr && this->data_.temp2 != 0)
          this->temp2_sensor_->publish_state(this->data_.temp2);
        if (this->output_enabled_binary_sensor_ != nullptr)
          this->output_enabled_binary_sensor_->publish_state(this->data_.output_enabled);

        ESP_LOGD(TAG, "Query: Vin=%.3fV Vout=%.3fV Iout=%.3fA Out=%s", this->data_.v_in, this->data_.v_out,
                 this->data_.i_out, this->data_.output_enabled ? "ON" : "OFF");
        break;
      }

      case CMD_VERSION: {
        std::string boot_hash = this->unpack_cstr_(payload, pos);
        std::string app_hash = this->unpack_cstr_(payload, pos);
        ESP_LOGI(TAG, "Version - Boot: %s, App: %s", boot_hash.c_str(), app_hash.c_str());
        break;
      }

      case CMD_PING:
        ESP_LOGD(TAG, "Ping response received");
        break;

      default:
        ESP_LOGV(TAG, "Response to command 0x%02X (status: %d)", original_cmd, status);
        break;
    }
  }
}

// ============================================================================
// Send Commands
// ============================================================================

void OpenDPS::send_command_(OpenDPSCommand cmd) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, cmd);
  this->send_frame_(payload);
}

void OpenDPS::send_query_() { this->send_command_(CMD_QUERY); }

void OpenDPS::send_ping() { this->send_command_(CMD_PING); }

void OpenDPS::request_version() { this->send_command_(CMD_VERSION); }

void OpenDPS::enable_output(bool enable) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_ENABLE_OUTPUT);
  this->pack8_(payload, enable ? 1 : 0);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Output %s", enable ? "enabled" : "disabled");
}

void OpenDPS::set_function(const std::string &function) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_FUNCTION);
  this->pack_cstr_(payload, function);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Set function: %s", function.c_str());
}

void OpenDPS::set_parameter(const std::string &key, const std::string &value) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_PARAMETERS);
  this->pack_cstr_(payload, key);
  this->pack_cstr_(payload, value);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Set parameter: %s=%s", key.c_str(), value.c_str());
}

void OpenDPS::set_voltage(float voltage) {
  // Convert voltage to millivolts
  uint16_t voltage_mv = static_cast<uint16_t>(voltage * 1000.0f);
  this->set_parameter("vset", std::to_string(voltage_mv));
}

void OpenDPS::set_current(float current) {
  // Convert current to milliamps
  uint16_t current_ma = static_cast<uint16_t>(current * 1000.0f);
  this->set_parameter("iset", std::to_string(current_ma));
}

void OpenDPS::set_brightness(uint8_t brightness) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_BRIGHTNESS);
  this->pack8_(payload, brightness);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Set brightness: %d", brightness);
}

void OpenDPS::lock(bool locked) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_LOCK);
  this->pack8_(payload, locked ? 1 : 0);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Device %s", locked ? "locked" : "unlocked");
}

void OpenDPS::start_firmware_upgrade(const std::string &firmware_url) {
  ESP_LOGI(TAG, "Firmware upgrade not yet implemented");
  ESP_LOGI(TAG, "Firmware URL: %s", firmware_url.c_str());
  // TODO: Implement firmware download and upgrade
  // 1. Download firmware from URL using HTTP client
  // 2. Calculate CRC-16 XMODEM of firmware data
  // 3. Send CMD_UPGRADE_START with chunk size and CRC
  // 4. Wait for UPGRADE_CONTINUE response
  // 5. Send firmware data in chunks using CMD_UPGRADE_DATA
  // 6. Handle upgrade status responses
  // 7. Report progress via callback
}

}  // namespace opendps
}  // namespace esphome
