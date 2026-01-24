#include "opendps.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#endif

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <esp_psram.h>
#endif

#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
#include "esphome/components/network/util.h"
#endif

namespace esphome {
namespace opendps {

static const char *const TAG = "opendps";

void OpenDPS::setup() {
  ESP_LOGCONFIG(TAG, "Setting up OpenDPS...");

  // Load brightness from preferences, use default if not set
  this->brightness_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("opendps_brightness"));
  if (!this->brightness_pref_.load(&this->brightness_)) {
    this->brightness_ = this->default_brightness_;
    ESP_LOGD(TAG, "No stored brightness, using default: %d", this->brightness_);
  } else {
    ESP_LOGD(TAG, "Loaded brightness from preferences: %d", this->brightness_);
  }

  // Note: TCP bridge setup is deferred to loop() to ensure network is ready

  // Send initial ping to check connection (skip if TCP bridge mode)
  if (!this->tcp_bridge_enabled_) {
    this->send_ping();
  }
}

void OpenDPS::loop() {
#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
  // If TCP bridge is enabled, handle bridge mode
  if (this->tcp_bridge_enabled_) {
    // Defer TCP bridge setup until network is ready
    if (!this->tcp_bridge_initialized_) {
      if (network::is_connected()) {
        this->setup_tcp_bridge_();
        this->tcp_bridge_initialized_ = true;
      }
      return;  // Wait for network before doing anything in bridge mode
    }
    this->loop_tcp_bridge_();
    return;  // Skip normal processing when in bridge mode
  }
#endif

  // Read incoming frames
  while (this->available()) {
    if (this->read_frame_()) {
      // Frame successfully received and processed
    }
  }

  uint32_t now = millis();

  // Handle upgrade timeout and retry
  if (this->upgrade_in_progress_ && this->upgrade_last_chunk_time_ > 0) {
    if (now - this->upgrade_last_chunk_time_ >= UPGRADE_CHUNK_TIMEOUT_MS) {
      if (this->upgrade_retry_count_ < UPGRADE_MAX_RETRIES) {
        this->upgrade_retry_count_++;
        ESP_LOGW(TAG, "Upgrade chunk timeout, retry %d/%d - resending start command", this->upgrade_retry_count_,
                 UPGRADE_MAX_RETRIES);
        // Reset offset and resend start command to re-sync with bootloader
        this->upgrade_offset_ = 0;
        this->upgrade_last_chunk_time_ = now;
        this->send_upgrade_start_(this->upgrade_chunk_size_, this->upgrade_crc_);
      } else {
        ESP_LOGE(TAG, "Upgrade failed after %d retries", UPGRADE_MAX_RETRIES);
        this->upgrade_in_progress_ = false;
        this->upgrade_firmware_data_.clear();
        this->upgrade_last_chunk_time_ = 0;
        this->upgrade_retry_count_ = 0;
      }
    }
  }

  // Send periodic query based on update interval (but not during upgrade)
  if (!this->upgrade_in_progress_ && now - this->last_query_ >= this->update_interval_) {
    this->send_query_();
    this->last_query_ = now;
  }
}

void OpenDPS::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenDPS:");
  ESP_LOGCONFIG(TAG, "  Update Interval: %dms", this->update_interval_);
#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
  if (this->tcp_bridge_enabled_) {
    ESP_LOGCONFIG(TAG, "  TCP Bridge: Enabled on port %d", this->tcp_bridge_port_);
    ESP_LOGCONFIG(TAG, "    Use: dpsctl.py -d tcp:<esp-ip>");
  }
#endif
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

  // Log frame details (at DEBUG level for upgrade frames which can be large)
  if (payload.size() > 100) {
    ESP_LOGD(TAG, "TX Frame: payload_len=%u, crc=0x%04X, frame_len=%u", payload.size(), crc, frame.size());
    // Log first few bytes for debugging
    if (frame.size() >= 10) {
      ESP_LOGD(TAG, "TX Frame start: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", frame[0], frame[1], frame[2],
               frame[3], frame[4], frame[5], frame[6], frame[7], frame[8], frame[9]);
    }
  } else {
    ESP_LOGV(TAG, "TX Frame (%u bytes): %s", frame.size(), format_hex_pretty(frame).c_str());
  }

  // Send frame
  this->write_array(frame.data(), frame.size());
  this->flush();
}

// ============================================================================
// Frame Reception
// ============================================================================

bool OpenDPS::read_frame_() {
  uint32_t now = millis();

  while (this->available()) {
    uint8_t byte;
    this->read_byte(&byte);

    // Log raw bytes during upgrade for debugging
    if (this->upgrade_in_progress_) {
      ESP_LOGD(TAG, "RX byte: 0x%02X", byte);
    }

    // Check for frame timeout
    if (this->receiving_frame_ && (now - this->frame_start_time_) > FRAME_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Frame timeout, resetting buffer (had %u bytes)", this->rx_buffer_.size());
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

        // Fire on_connect trigger on first successful query
        if (!this->connected_) {
          this->connected_ = true;
          ESP_LOGI(TAG, "Connected to OpenDPS device");
          // Send connection status to update display icon (ethernet or wifi)
#ifdef USE_ETHERNET
          this->send_connection_status(CONN_ETHERNET_CONNECTED);
#else
          this->send_connection_status(CONN_WIFI_CONNECTED);
#endif
          this->on_connect_callback_.call();
        }
        break;
      }

      case CMD_VERSION: {
        std::string boot_hash = this->unpack_cstr_(payload, pos);
        std::string app_hash = this->unpack_cstr_(payload, pos);
        ESP_LOGI(TAG, "Version - Boot: %s, App: %s", boot_hash.c_str(), app_hash.c_str());
        break;
      }

      case CMD_PING:
        ESP_LOGI(TAG, "Ping response received");
        break;

      case CMD_UPGRADE_START: {
        uint16_t device_chunk_size = this->unpack16_(payload, pos);
        // Bootloader also sends 'reason' byte, read it if available
        uint8_t reason = (pos < payload.size()) ? this->unpack8_(payload, pos) : 0;
        ESP_LOGI(TAG, "Upgrade start response - status: %d, chunk_size: %d, reason: %d", status, device_chunk_size,
                 reason);

        if (status == UPGRADE_CONTINUE) {
          ESP_LOGI(TAG, "Device accepted firmware upgrade, starting data transfer");
          // Reset retry count on successful response
          this->upgrade_retry_count_ = 0;
          // Use device's preferred chunk size if smaller than ours
          if (device_chunk_size > 0 && device_chunk_size < this->upgrade_chunk_size_) {
            this->upgrade_chunk_size_ = device_chunk_size;
            ESP_LOGI(TAG, "Using device chunk size: %d", this->upgrade_chunk_size_);
          }
          if (this->upgrade_progress_callback_) {
            this->upgrade_progress_callback_(0);
          }
          // Longer delay to ensure bootloader is ready to receive
          // The bootloader enters its receive loop after sending this response,
          // but we need to ensure it's fully ready before we start sending data.
          // At 9600 baud, the bootloader's response takes ~10ms to send.
          // We add extra margin for flash operations and loop entry.
          ESP_LOGI(TAG, "Waiting 500ms for bootloader to be ready...");
          delay(500);
          // Update timeout tracker and send first chunk
          this->upgrade_last_chunk_time_ = millis();
          this->send_next_upgrade_chunk_();
        } else {
          ESP_LOGE(TAG, "Device rejected firmware upgrade (status: %d)", status);
          this->upgrade_in_progress_ = false;
          this->upgrade_firmware_data_.clear();
          this->upgrade_last_chunk_time_ = 0;
        }
        break;
      }

      case CMD_UPGRADE_DATA: {
        // Reset timeout on any response
        this->upgrade_last_chunk_time_ = millis();
        this->upgrade_retry_count_ = 0;

        if (status == UPGRADE_CONTINUE) {
          // Send next chunk
          this->send_next_upgrade_chunk_();
        } else if (status == UPGRADE_SUCCESS) {
          ESP_LOGI(TAG, "Firmware upgrade completed successfully!");
          this->upgrade_in_progress_ = false;
          this->upgrade_firmware_data_.clear();
          this->upgrade_last_chunk_time_ = 0;
          if (this->upgrade_progress_callback_) {
            this->upgrade_progress_callback_(100);
          }
          // Restore connection icon after successful upgrade
#ifdef USE_ETHERNET
          this->send_connection_status(CONN_ETHERNET_CONNECTED);
#else
          this->send_connection_status(CONN_WIFI_CONNECTED);
#endif
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
          this->upgrade_in_progress_ = false;
          this->upgrade_firmware_data_.clear();
          this->upgrade_last_chunk_time_ = 0;
          // Restore connection icon after failed upgrade
#ifdef USE_ETHERNET
          this->send_connection_status(CONN_ETHERNET_CONNECTED);
#else
          this->send_connection_status(CONN_WIFI_CONNECTED);
#endif
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
  this->set_parameter("voltage", std::to_string(voltage_mv));
}

void OpenDPS::set_current(float current) {
  // Convert current to milliamps
  uint16_t current_ma = static_cast<uint16_t>(current * 1000.0f);
  this->set_parameter("current", std::to_string(current_ma));
}

void OpenDPS::set_brightness(uint8_t brightness) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_BRIGHTNESS);
  this->pack8_(payload, brightness);
  this->send_frame_(payload);

  // Store brightness in preferences
  this->brightness_ = brightness;
  this->brightness_pref_.save(&this->brightness_);

  ESP_LOGI(TAG, "Set brightness: %d", brightness);
}

void OpenDPS::lock(bool locked) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_LOCK);
  this->pack8_(payload, locked ? 1 : 0);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Device %s", locked ? "locked" : "unlocked");
}

void OpenDPS::send_connection_status(ConnectionStatus status) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_WIFI_STATUS);
  this->pack8_(payload, static_cast<uint8_t>(status));
  this->send_frame_(payload);
  ESP_LOGD(TAG, "Sent connection status: %d", status);
}

float OpenDPS::get_voltage_setting() const {
  auto it = this->data_.params.find("voltage");
  if (it != this->data_.params.end()) {
    return std::stof(it->second) / 1000.0f;  // Convert mV to V
  }
  return 0.0f;
}

float OpenDPS::get_current_setting() const {
  auto it = this->data_.params.find("current");
  if (it != this->data_.params.end()) {
    return std::stof(it->second) / 1000.0f;  // Convert mA to A
  }
  return 0.0f;
}

void OpenDPS::start_firmware_upgrade(const std::string &firmware_path) {
#ifndef USE_STORAGE
  ESP_LOGE(TAG, "Firmware upgrade requires storage component");
  return;
#else
  ESP_LOGI(TAG, "Starting firmware upgrade from: %s", firmware_path.c_str());

  // Show upgrade icon on DPS display
  this->send_connection_status(CONN_DPS_UPGRADING);

  // Check if storage is available
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Storage component not initialized");
    return;
  }

  // Determine storage type
  bool is_network_path = storage::global_storage->is_network_path(firmware_path);

  // Check PSRAM availability
  bool has_psram = false;
  size_t free_psram = 0;
  size_t total_psram = 0;

#ifdef USE_ESP32
  has_psram = esp_psram_is_initialized();
  if (has_psram) {
    total_psram = esp_psram_get_size();
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  }
#endif

  ESP_LOGI(TAG, "PSRAM: %s (total: %u bytes, free: %u bytes)", has_psram ? "available" : "not available", total_psram,
           free_psram);

  // Network paths require PSRAM for buffering
  if (is_network_path && !has_psram) {
    ESP_LOGE(TAG, "Network storage requires PSRAM for firmware buffering");
    ESP_LOGE(TAG, "This device has no PSRAM - use local storage (USB/SD/LittleFS) instead");
    return;
  }

  // Check if file exists
  bool file_exists = false;
  if (is_network_path) {
    file_exists = storage::global_storage->network_file_exists(firmware_path);
  } else {
    file_exists = storage::global_storage->file_exists(firmware_path);
  }

  if (!file_exists) {
    ESP_LOGE(TAG, "Firmware file not found: %s", firmware_path.c_str());
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

    // Validate firmware - check for reasonable size (OpenDPS firmware is typically 40-80KB)
    if (firmware_size < 1024) {
      ESP_LOGE(TAG, "Firmware file too small (%u bytes) - likely corrupt or empty", firmware_size);
      return;
    }
    // Log first bytes for debugging
    if (firmware_size >= 8) {
      ESP_LOGD(TAG, "Firmware header: %02X %02X %02X %02X %02X %02X %02X %02X", firmware_data[0], firmware_data[1],
               firmware_data[2], firmware_data[3], firmware_data[4], firmware_data[5], firmware_data[6],
               firmware_data[7]);
    }

    // Store firmware data for chunked transfer (move to avoid copy)
    this->upgrade_firmware_data_ = std::move(firmware_data);
    this->upgrade_offset_ = 0;
    this->upgrade_chunk_size_ = 1024;
    this->upgrade_in_progress_ = true;
    this->upgrade_retry_count_ = 0;

    // Calculate CRC-16 CCITT (XMODEM) of entire firmware
    ESP_LOGI(TAG, "Calculating CRC-16...");
    this->upgrade_crc_ = 0;
    for (uint8_t byte : this->upgrade_firmware_data_) {
      this->upgrade_crc_ = this->crc16_ccitt_(this->upgrade_crc_, byte);
    }
    ESP_LOGI(TAG, "Firmware CRC: 0x%04X", this->upgrade_crc_);

    // Send upgrade start command
    ESP_LOGI(TAG, "Sending upgrade start command (chunk_size=%d, crc=0x%04X)", this->upgrade_chunk_size_,
             this->upgrade_crc_);
    this->upgrade_last_chunk_time_ = millis();
    this->send_upgrade_start_(this->upgrade_chunk_size_, this->upgrade_crc_);

    ESP_LOGI(TAG, "Firmware upgrade initiated - waiting for device confirmation");
  } else {
    // Local storage (USB/SD/LittleFS) - can work without PSRAM
    ESP_LOGI(TAG, "Reading firmware from local storage...");
    std::string firmware_data = storage::global_storage->read_file(firmware_path);

    if (firmware_data.empty()) {
      ESP_LOGE(TAG, "Failed to read firmware file or file is empty");
      return;
    }

    size_t firmware_size = firmware_data.size();
    ESP_LOGI(TAG, "Firmware size: %u bytes", firmware_size);

    // Validate firmware - check for reasonable size (OpenDPS firmware is typically 40-80KB)
    if (firmware_size < 1024) {
      ESP_LOGE(TAG, "Firmware file too small (%u bytes) - likely corrupt or empty", firmware_size);
      return;
    }
    // Log first bytes for debugging
    if (firmware_size >= 8) {
      ESP_LOGD(TAG, "Firmware header: %02X %02X %02X %02X %02X %02X %02X %02X", static_cast<uint8_t>(firmware_data[0]),
               static_cast<uint8_t>(firmware_data[1]), static_cast<uint8_t>(firmware_data[2]),
               static_cast<uint8_t>(firmware_data[3]), static_cast<uint8_t>(firmware_data[4]),
               static_cast<uint8_t>(firmware_data[5]), static_cast<uint8_t>(firmware_data[6]),
               static_cast<uint8_t>(firmware_data[7]));
    }

    // Store firmware data for chunked transfer
    this->upgrade_firmware_data_.assign(firmware_data.begin(), firmware_data.end());
    this->upgrade_offset_ = 0;
    this->upgrade_chunk_size_ = 1024;
    this->upgrade_in_progress_ = true;
    this->upgrade_retry_count_ = 0;

    // Calculate CRC-16 CCITT (XMODEM) of entire firmware
    ESP_LOGI(TAG, "Calculating CRC-16...");
    this->upgrade_crc_ = 0;
    for (uint8_t byte : this->upgrade_firmware_data_) {
      this->upgrade_crc_ = this->crc16_ccitt_(this->upgrade_crc_, byte);
    }
    ESP_LOGI(TAG, "Firmware CRC: 0x%04X", this->upgrade_crc_);

    // Send upgrade start command
    ESP_LOGI(TAG, "Sending upgrade start command (chunk_size=%d, crc=0x%04X)", this->upgrade_chunk_size_,
             this->upgrade_crc_);
    this->upgrade_last_chunk_time_ = millis();
    this->send_upgrade_start_(this->upgrade_chunk_size_, this->upgrade_crc_);

    ESP_LOGI(TAG, "Firmware upgrade initiated - waiting for device confirmation");
  }
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

  // Calculate what the frame will look like
  uint16_t payload_crc = this->calculate_crc_(payload);
  ESP_LOGI(TAG, "Sending upgrade data: cmd=0x%02X, data_len=%u, payload_crc=0x%04X", CMD_UPGRADE_DATA, data.size(),
           payload_crc);

  // Log first few data bytes for debugging
  if (data.size() >= 8) {
    ESP_LOGD(TAG, "Data start: %02X %02X %02X %02X %02X %02X %02X %02X", data[0], data[1], data[2], data[3], data[4],
             data[5], data[6], data[7]);
  }

  this->send_frame_(payload);
}

void OpenDPS::send_next_upgrade_chunk_() {
  if (!this->upgrade_in_progress_ || this->upgrade_firmware_data_.empty()) {
    ESP_LOGW(TAG, "No upgrade in progress or no firmware data");
    return;
  }

  size_t remaining = this->upgrade_firmware_data_.size() - this->upgrade_offset_;
  if (remaining == 0) {
    ESP_LOGI(TAG, "All firmware data sent, waiting for final confirmation");
    return;
  }

  size_t chunk_len = std::min(static_cast<size_t>(this->upgrade_chunk_size_), remaining);
  std::vector<uint8_t> chunk(this->upgrade_firmware_data_.begin() + this->upgrade_offset_,
                             this->upgrade_firmware_data_.begin() + this->upgrade_offset_ + chunk_len);

  uint8_t progress = static_cast<uint8_t>((this->upgrade_offset_ * 100) / this->upgrade_firmware_data_.size());
  ESP_LOGI(TAG, "Sending chunk at offset %u, size %u bytes (%u%%)", this->upgrade_offset_, chunk_len, progress);

  this->send_upgrade_data_(chunk);
  this->upgrade_offset_ += chunk_len;

  if (this->upgrade_progress_callback_) {
    this->upgrade_progress_callback_(progress);
  }
}

// ============================================================================
// TCP Bridge for dpsctl.py access
// ============================================================================

#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)

void OpenDPS::setup_tcp_bridge_() {
  if (!this->tcp_bridge_enabled_) {
    return;
  }

  ESP_LOGI(TAG, "Setting up TCP bridge on port %d for dpsctl.py access", this->tcp_bridge_port_);

  this->tcp_server_socket_ = socket::socket_ip(SOCK_STREAM, 0);
  if (this->tcp_server_socket_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create TCP server socket");
    return;
  }

  int enable = 1;
  int err = this->tcp_server_socket_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  if (err != 0) {
    ESP_LOGW(TAG, "TCP bridge: setsockopt SO_REUSEADDR failed: %d", err);
  }

  err = this->tcp_server_socket_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "TCP bridge: setblocking failed: %d", err);
    this->tcp_server_socket_.reset();
    return;
  }

  struct sockaddr_storage server_addr;
  socklen_t addr_len =
      socket::set_sockaddr_any((struct sockaddr *) &server_addr, sizeof(server_addr), this->tcp_bridge_port_);
  if (addr_len == 0) {
    ESP_LOGE(TAG, "TCP bridge: set_sockaddr_any failed");
    this->tcp_server_socket_.reset();
    return;
  }

  err = this->tcp_server_socket_->bind((struct sockaddr *) &server_addr, addr_len);
  if (err != 0) {
    ESP_LOGE(TAG, "TCP bridge: bind failed: %d", errno);
    this->tcp_server_socket_.reset();
    return;
  }

  err = this->tcp_server_socket_->listen(1);
  if (err != 0) {
    ESP_LOGE(TAG, "TCP bridge: listen failed: %d", errno);
    this->tcp_server_socket_.reset();
    return;
  }

  ESP_LOGI(TAG, "TCP bridge listening on port %d - use dpsctl.py -d tcp:<esp-ip>", this->tcp_bridge_port_);
}

void OpenDPS::loop_tcp_bridge_() {
  if (!this->tcp_bridge_enabled_ || this->tcp_server_socket_ == nullptr) {
    return;
  }

  // Accept new connections
  if (this->tcp_client_socket_ == nullptr) {
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    auto client = this->tcp_server_socket_->accept((struct sockaddr *) &client_addr, &addr_len);
    if (client != nullptr) {
      ESP_LOGI(TAG, "TCP bridge: client connected");
      client->setblocking(false);
      this->tcp_client_socket_ = std::move(client);
    }
  }

  // Handle connected client - bridge TCP <-> UART
  if (this->tcp_client_socket_ != nullptr) {
    // Read from TCP, write to UART
    uint8_t tcp_buf[256];
    ssize_t tcp_len = this->tcp_client_socket_->read(tcp_buf, sizeof(tcp_buf));
    if (tcp_len > 0) {
      ESP_LOGV(TAG, "TCP->UART: %d bytes", tcp_len);
      this->write_array(tcp_buf, tcp_len);
      this->flush();
    } else if (tcp_len == 0 || (tcp_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      // Connection closed or error
      ESP_LOGI(TAG, "TCP bridge: client disconnected");
      this->tcp_client_socket_.reset();
    }

    // Read from UART, write to TCP
    while (this->available() && this->tcp_client_socket_ != nullptr) {
      uint8_t uart_buf[256];
      size_t uart_len = 0;
      while (this->available() && uart_len < sizeof(uart_buf)) {
        uint8_t byte;
        this->read_byte(&byte);
        uart_buf[uart_len++] = byte;
      }
      if (uart_len > 0) {
        ESP_LOGV(TAG, "UART->TCP: %d bytes", uart_len);
        ssize_t written = this->tcp_client_socket_->write(uart_buf, uart_len);
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGI(TAG, "TCP bridge: write error, disconnecting");
          this->tcp_client_socket_.reset();
        }
      }
    }
  }
}

#endif  // USE_SOCKET_IMPL_LWIP_TCP || USE_SOCKET_IMPL_BSD_SOCKETS

}  // namespace opendps
}  // namespace esphome
