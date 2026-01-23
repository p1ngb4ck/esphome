#include "opendps.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

#ifdef USE_BINARY_STORAGE
#include "esphome/components/binary_storage/binary_storage.h"
#endif

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

      case CMD_UPGRADE_START: {
        uint16_t device_chunk_size = this->unpack16_(payload, pos);
        ESP_LOGI(TAG, "Upgrade start response - status: %d, chunk_size: %d", status, device_chunk_size);

        if (status == UPGRADE_CONTINUE) {
          ESP_LOGI(TAG, "Device accepted firmware upgrade");
          if (this->upgrade_progress_callback_) {
            this->upgrade_progress_callback_(0);
          }
          // TODO: Continue with firmware data transfer
          // This would need to be implemented with a state machine
          // to avoid blocking the main loop
        } else {
          ESP_LOGE(TAG, "Device rejected firmware upgrade (status: %d)", status);
        }
        break;
      }

      case CMD_UPGRADE_DATA: {
        ESP_LOGV(TAG, "Upgrade data response - status: %d", status);

        if (status == UPGRADE_CONTINUE) {
          // Continue sending next chunk
          if (this->upgrade_progress_callback_) {
            // Calculate progress percentage
            // TODO: Track actual progress
          }
        } else if (status == UPGRADE_SUCCESS) {
          ESP_LOGI(TAG, "Firmware upgrade completed successfully!");
          if (this->upgrade_progress_callback_) {
            this->upgrade_progress_callback_(100);
          }
        } else {
          const char *error_msg = "Unknown error";
          switch (status) {
            case UPGRADE_CRC_ERROR:
              error_msg = "CRC error";
              break;
            case UPGRADE_ERASE_ERROR:
              error_msg = "Erase error";
              break;
            case UPGRADE_FLASH_ERROR:
              error_msg = "Flash error";
              break;
            case UPGRADE_OVERFLOW_ERROR:
              error_msg = "Overflow error";
              break;
            case UPGRADE_BOOTCOM_ERROR:
              error_msg = "Bootloader communication error";
              break;
          }
          ESP_LOGE(TAG, "Firmware upgrade failed: %s (status: %d)", error_msg, status);
        }
        break;
      }

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

void OpenDPS::start_firmware_upgrade(const std::string &firmware_path) {
#ifndef USE_STORAGE
  ESP_LOGE(TAG, "Firmware upgrade requires storage component");
  return;
#else
  ESP_LOGI(TAG, "Starting firmware upgrade from: %s", firmware_path.c_str());

  // Check if storage is available
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Storage component not initialized");
    return;
  }

  // Determine storage type
  bool is_network_path = storage::global_storage->is_network_path(firmware_path);
  bool is_device_node = storage::global_storage->is_device_node(firmware_path);

  // Check PSRAM availability
  bool has_psram = false;
  size_t free_psram = 0;
  size_t total_psram = 0;

#ifdef USE_ESP32
  total_psram = ESP.getPsramSize();
  free_psram = ESP.getFreePsram();
  has_psram = total_psram > 0;
#endif

  ESP_LOGI(TAG, "PSRAM: %s (total: %u bytes, free: %u bytes)", has_psram ? "available" : "not available", total_psram,
           free_psram);

  // Network paths require PSRAM for buffering
  if (is_network_path && !has_psram) {
    ESP_LOGE(TAG, "Network storage requires PSRAM for firmware buffering");
    ESP_LOGE(TAG, "This device has no PSRAM - use local storage (USB/SD/binary_storage) instead");
    return;
  }

  // Check if file/device exists
  bool file_exists = false;
  if (is_network_path) {
    file_exists = storage::global_storage->network_file_exists(firmware_path);
  } else if (is_device_node) {
    // Device nodes (binary_storage) always "exist" if registered
    auto *dev_node = storage::global_storage->find_device_node(firmware_path);
    file_exists = (dev_node != nullptr);
  } else {
    file_exists = storage::global_storage->file_exists(firmware_path);
  }

  if (!file_exists) {
    ESP_LOGE(TAG, "Firmware file/device not found: %s", firmware_path.c_str());
    return;
  }

  // For network paths, we need to load entire file into PSRAM
  if (is_network_path) {
    ESP_LOGI(TAG, "Reading firmware from network storage into PSRAM...");

    // Read file into vector (will use PSRAM via allocator if available)
    std::vector<uint8_t> firmware_data;
    if (!storage::global_storage->network_read_file(firmware_path, firmware_data)) {
      ESP_LOGE(TAG, "Failed to read firmware file from network storage");
      return;
    }

    if (firmware_data.empty()) {
      ESP_LOGE(TAG, "Firmware file is empty");
      return;
    }

    size_t firmware_size = firmware_data.size();
    ESP_LOGI(TAG, "Firmware size: %u bytes", firmware_size);

    // Verify we have enough PSRAM (need at least firmware size + safety margin)
    size_t required_psram = firmware_size + (64 * 1024);  // 64KB safety margin
    if (free_psram < required_psram) {
      ESP_LOGE(TAG, "Insufficient PSRAM: need %u bytes, have %u bytes free", required_psram, free_psram);
      return;
    }

    // Validate firmware (check for magic byte at offset 0x06)
    if (firmware_size > 6) {
      uint8_t magic = firmware_data[6];
      if (magic != 0x20) {
        ESP_LOGW(TAG, "Firmware magic byte mismatch (expected 0x20, got 0x%02X)", magic);
        ESP_LOGW(TAG, "Continuing anyway - use with caution!");
      }
    }

    // Calculate CRC-16 CCITT (XMODEM) of entire firmware
    ESP_LOGI(TAG, "Calculating CRC-16...");
    uint16_t firmware_crc = 0;
    for (uint8_t byte : firmware_data) {
      firmware_crc = this->crc16_ccitt_(firmware_crc, byte);
    }
    ESP_LOGI(TAG, "Firmware CRC: 0x%04X", firmware_crc);

    // Send upgrade start command
    uint16_t chunk_size = 1024;  // Default chunk size
    ESP_LOGI(TAG, "Sending upgrade start command (chunk_size=%d, crc=0x%04X)", chunk_size, firmware_crc);
    this->send_upgrade_start_(chunk_size, firmware_crc);

    ESP_LOGI(TAG, "Firmware upgrade initiated from network storage - check logs for progress");
  } else if (is_device_node) {
    // Binary storage device (internal flash partition, external SPI flash, etc.)
    ESP_LOGI(TAG, "Reading firmware from binary_storage device...");

#ifdef USE_BINARY_STORAGE
    auto *dev_node = storage::global_storage->find_device_node(firmware_path);
    if (!dev_node || !dev_node->device) {
      ESP_LOGE(TAG, "Binary storage device not found");
      return;
    }

    binary_storage::BinaryStorage *device = dev_node->device;
    size_t device_size = device->get_size();

    ESP_LOGI(TAG, "Binary storage device: %s (%u bytes)", dev_node->device_type.c_str(), device_size);

    // Read firmware from binary storage
    std::vector<uint8_t> firmware_data;
    firmware_data.resize(device_size);

    if (!device->read(0, firmware_data.data(), device_size)) {
      ESP_LOGE(TAG, "Failed to read from binary storage device");
      return;
    }

    // Find actual firmware size (may be less than device size)
    // Look for 0xFF padding at the end
    size_t firmware_size = device_size;
    while (firmware_size > 0 && firmware_data[firmware_size - 1] == 0xFF) {
      firmware_size--;
    }

    if (firmware_size == 0) {
      ESP_LOGE(TAG, "Binary storage device appears empty (all 0xFF)");
      return;
    }

    firmware_data.resize(firmware_size);
    ESP_LOGI(TAG, "Firmware size: %u bytes (device size: %u bytes)", firmware_size, device_size);

    // Validate firmware (check for magic byte at offset 0x06)
    if (firmware_size > 6) {
      uint8_t magic = firmware_data[6];
      if (magic != 0x20) {
        ESP_LOGW(TAG, "Firmware magic byte mismatch (expected 0x20, got 0x%02X)", magic);
        ESP_LOGW(TAG, "Continuing anyway - use with caution!");
      }
    }

    // Calculate CRC-16 CCITT (XMODEM) of entire firmware
    ESP_LOGI(TAG, "Calculating CRC-16...");
    uint16_t firmware_crc = 0;
    for (uint8_t byte : firmware_data) {
      firmware_crc = this->crc16_ccitt_(firmware_crc, byte);
    }
    ESP_LOGI(TAG, "Firmware CRC: 0x%04X", firmware_crc);

    // Send upgrade start command
    uint16_t chunk_size = 1024;  // Default chunk size
    ESP_LOGI(TAG, "Sending upgrade start command (chunk_size=%d, crc=0x%04X)", chunk_size, firmware_crc);
    this->send_upgrade_start_(chunk_size, firmware_crc);

    ESP_LOGI(TAG, "Firmware upgrade initiated from binary_storage - check logs for progress");
#else
    ESP_LOGE(TAG, "Binary storage support not compiled in");
    return;
#endif
  } else {
    // Local storage (USB/SD) - can work without PSRAM
    ESP_LOGI(TAG, "Reading firmware from local storage...");
    std::string firmware_data = storage::global_storage->read_file(firmware_path);

    if (firmware_data.empty()) {
      ESP_LOGE(TAG, "Failed to read firmware file or file is empty");
      return;
    }

    size_t firmware_size = firmware_data.size();
    ESP_LOGI(TAG, "Firmware size: %u bytes", firmware_size);

    // Validate firmware (check for magic byte at offset 0x06)
    if (firmware_size > 6) {
      uint8_t magic = static_cast<uint8_t>(firmware_data[6]);
      if (magic != 0x20) {
        ESP_LOGW(TAG, "Firmware magic byte mismatch (expected 0x20, got 0x%02X)", magic);
        ESP_LOGW(TAG, "Continuing anyway - use with caution!");
      }
    }

    // Calculate CRC-16 CCITT (XMODEM) of entire firmware
    ESP_LOGI(TAG, "Calculating CRC-16...");
    uint16_t firmware_crc = 0;
    for (char byte : firmware_data) {
      firmware_crc = this->crc16_ccitt_(firmware_crc, static_cast<uint8_t>(byte));
    }
    ESP_LOGI(TAG, "Firmware CRC: 0x%04X", firmware_crc);

    // Send upgrade start command
    uint16_t chunk_size = 1024;  // Default chunk size
    ESP_LOGI(TAG, "Sending upgrade start command (chunk_size=%d, crc=0x%04X)", chunk_size, firmware_crc);
    this->send_upgrade_start_(chunk_size, firmware_crc);

    ESP_LOGI(TAG, "Firmware upgrade initiated from local storage - check logs for progress");
  }

  // Wait for response (this is blocking - might need to make async in the future)
  // For now, we'll process the response in process_frame_ and continue there
  // TODO: Make this fully async with state machine
#endif
}

void OpenDPS::send_upgrade_start_(uint16_t chunk_size, uint16_t crc) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_UPGRADE_START);
  this->pack16_(payload, chunk_size);
  this->pack16_(payload, crc);
  this->send_frame_(payload);
}

void OpenDPS::send_upgrade_data_(const std::vector<uint8_t> &data) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_UPGRADE_DATA);
  for (uint8_t byte : data) {
    this->pack8_(payload, byte);
  }
  this->send_frame_(payload);
}

}  // namespace opendps
}  // namespace esphome
