#include "opendps.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/time.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

#ifdef USE_STORAGE
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_device.h"
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

  // Check if we were in the middle of a firmware upgrade when we rebooted
  this->upgrade_state_pref_ = global_preferences->make_preference<uint32_t>(fnv1_hash("opendps_upgrade_state"));
  uint32_t upgrade_flag = 0;
  if (this->upgrade_state_pref_.load(&upgrade_flag) && upgrade_flag > 0) {
    ESP_LOGW(TAG, "Detected incomplete firmware upgrade - DPS may be in bootloader mode");
    this->firmware_baud_rate_ = this->parent_->get_baud_rate();
    if (this->bootloader_legacy_) {
      // Legacy bootloader starts at whatever baud was saved (stored in upgrade_flag)
      uint32_t boot_baud = (upgrade_flag > 1) ? upgrade_flag : this->firmware_baud_rate_;
      ESP_LOGI(TAG, "Legacy bootloader: switching UART to %u", boot_baud);
      this->parent_->flush();
      this->parent_->set_baud_rate(boot_baud);
      this->parent_->load_settings();
    } else {
      // New bootloader always starts at 9600
      ESP_LOGI(TAG, "Switching UART to 9600 for bootloader");
      this->parent_->flush();
      this->parent_->set_baud_rate(9600);
      this->parent_->load_settings();
    }
  }

  // Note: TCP bridge setup is deferred to loop() to ensure network is ready

  // Send initial ping to check connection
  this->send_ping();
}

void OpenDPS::loop() {
#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
  // If TCP bridge is enabled, set it up when network is ready
  if (this->tcp_bridge_enabled_) {
    if (!this->tcp_bridge_initialized_) {
      if (network::is_connected()) {
        this->setup_tcp_bridge_();
        this->tcp_bridge_initialized_ = true;
      }
      // Continue with normal processing while waiting for network
    } else {
      // Check for new TCP connections and handle bridge mode
      this->loop_tcp_bridge_();

      // If a TCP client is connected or in disconnect grace period, skip normal processing (bridge mode active)
      if (this->tcp_client_socket_ != nullptr || this->tcp_client_disconnect_time_ > 0) {
        return;
      }
    }
  }
#endif

  // When UART is paused (e.g., during calibration supply swap), skip all UART I/O
  if (this->cal_assistant_state_ == CAL_UART_PAUSED) {
    return;
  }

  // Read incoming frames
  while (this->available()) {
    if (this->read_frame_()) {
      // Frame successfully received and processed
    }
  }

  uint32_t now = millis();

  // Wait for bootloader to be ready before sending first chunk
  if (this->upgrade_bootloader_ready_time_ > 0) {
    if (now - this->upgrade_bootloader_ready_time_ >= 500) {
      this->upgrade_bootloader_ready_time_ = 0;
      // Switch to faster baud if configured, before first data chunk (new bootloader only)
      if (!this->bootloader_legacy_ && this->bootloader_baud_rate_ > 0 && this->bootloader_baud_rate_ != 9600) {
        ESP_LOGI(TAG, "Switching bootloader to %u for data transfer", this->bootloader_baud_rate_);
        std::vector<uint8_t> payload;
        this->pack8_(payload, CMD_SET_BAUD);
        this->pack8_(payload, (this->bootloader_baud_rate_ >> 24) & 0xFF);
        this->pack8_(payload, (this->bootloader_baud_rate_ >> 16) & 0xFF);
        this->pack8_(payload, (this->bootloader_baud_rate_ >> 8) & 0xFF);
        this->pack8_(payload, this->bootloader_baud_rate_ & 0xFF);
        this->send_frame_(payload);
        this->parent_->flush();
        this->parent_->set_baud_rate(this->bootloader_baud_rate_);
        this->parent_->load_settings();
      }
      this->upgrade_last_chunk_time_ = now;
      this->send_next_upgrade_chunk_();
    }
    return;
  }

  // Wait before restoring calibration after firmware upgrade
  if (this->upgrade_cal_restore_time_ > 0) {
    if (now - this->upgrade_cal_restore_time_ >= 500) {
      this->upgrade_cal_restore_time_ = 0;
      if (this->restore_calibration_from_storage()) {
        ESP_LOGI(TAG, "Calibration auto-restored successfully after firmware upgrade");
      } else {
        ESP_LOGW(TAG, "Failed to auto-restore calibration after firmware upgrade");
      }
    }
    return;
  }

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
        // Restore firmware baud rate if we switched for bootloader
        if (this->firmware_baud_rate_ > 0) {
          ESP_LOGI(TAG, "Restoring UART baud rate to %u", this->firmware_baud_rate_);
          this->parent_->flush();
          this->parent_->set_baud_rate(this->firmware_baud_rate_);
          this->parent_->load_settings();
          this->firmware_baud_rate_ = 0;
        }
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
  LOG_SENSOR("  ", "Voltage Set", this->voltage_set_sensor_);
  LOG_SENSOR("  ", "Current Set", this->current_set_sensor_);
  LOG_BINARY_SENSOR("  ", "Output Enabled", this->output_enabled_binary_sensor_);
  LOG_BINARY_SENSOR("  ", "Connected", this->connected_binary_sensor_);
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

uint32_t OpenDPS::unpack32_(const std::vector<uint8_t> &frame, size_t &pos) {
  uint32_t value = static_cast<uint32_t>(this->unpack8_(frame, pos)) << 24;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos)) << 16;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos)) << 8;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos));
  return value;
}

float OpenDPS::unpack_float_(const std::vector<uint8_t> &frame, size_t &pos) {
  // Float is packed as big-endian uint32 via pack32() in OpenDPS firmware
  union {
    float f;
    uint32_t u32;
  } float_union;
  float_union.u32 = this->unpack32_(frame, pos);
  return float_union.f;
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
#if defined(USE_SOCKET_IMPL_LWIP_TCP) || defined(USE_SOCKET_IMPL_BSD_SOCKETS)
  // Don't send frames while TCP bridge client is connected - would interfere with dpsctl.py
  if (this->tcp_client_socket_ != nullptr) {
    ESP_LOGW(TAG, "Ignoring frame send - TCP bridge active (dpsctl.py has exclusive UART access)");
    return;
  }
#endif

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
    char hex_buf[format_hex_pretty_size(64)];
    ESP_LOGV(TAG, "TX Frame (%u bytes): %s", frame.size(), format_hex_pretty_to(hex_buf, frame));
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

        char hex_buf[format_hex_pretty_size(64)];
        ESP_LOGV(TAG, "RX Frame (%d bytes): %s", this->rx_buffer_.size(),
                 format_hex_pretty_to(hex_buf, this->rx_buffer_));

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

        // Sync brightness from DPS query response (if reported)
        // Skip sync briefly after setting brightness to allow DPS to update
        auto brightness_it = this->data_.params.find("brightness");
        if (brightness_it != this->data_.params.end()) {
          uint32_t time_since_set = millis() - this->brightness_set_time_;
          if (time_since_set > 350) {
            uint8_t dps_brightness = static_cast<uint8_t>(std::atoi(brightness_it->second.c_str()));
            if (dps_brightness != this->brightness_) {
              ESP_LOGD(TAG, "Brightness synced from DPS: %d -> %d", this->brightness_, dps_brightness);
              this->brightness_ = dps_brightness;
            }
          }
        }

        // Publish vset/iset from params (mV/mA → V/A), only on change
        auto voltage_it = this->data_.params.find("voltage");
        if (voltage_it != this->data_.params.end() && this->voltage_set_sensor_ != nullptr) {
          float vset = std::atoi(voltage_it->second.c_str()) / 1000.0f;
          if (!this->voltage_set_sensor_->has_state() || this->voltage_set_sensor_->state != vset)
            this->voltage_set_sensor_->publish_state(vset);
        }
        auto current_it = this->data_.params.find("current");
        if (current_it != this->data_.params.end() && this->current_set_sensor_ != nullptr) {
          float iset = std::atoi(current_it->second.c_str()) / 1000.0f;
          if (!this->current_set_sensor_->has_state() || this->current_set_sensor_->state != iset)
            this->current_set_sensor_->publish_state(iset);
        }

        this->data_.last_update = millis();

        // Publish sensor values, only on change
        if (this->voltage_in_sensor_ != nullptr &&
            (!this->voltage_in_sensor_->has_state() || this->voltage_in_sensor_->state != this->data_.v_in))
          this->voltage_in_sensor_->publish_state(this->data_.v_in);
        if (this->voltage_out_sensor_ != nullptr &&
            (!this->voltage_out_sensor_->has_state() || this->voltage_out_sensor_->state != this->data_.v_out))
          this->voltage_out_sensor_->publish_state(this->data_.v_out);
        if (this->current_out_sensor_ != nullptr &&
            (!this->current_out_sensor_->has_state() || this->current_out_sensor_->state != this->data_.i_out))
          this->current_out_sensor_->publish_state(this->data_.i_out);
        if (this->power_out_sensor_ != nullptr) {
          float power = this->data_.v_out * this->data_.i_out;
          if (!this->power_out_sensor_->has_state() || this->power_out_sensor_->state != power)
            this->power_out_sensor_->publish_state(power);
        }
        if (this->temp1_sensor_ != nullptr && this->data_.temp1 != 0 &&
            (!this->temp1_sensor_->has_state() || this->temp1_sensor_->state != this->data_.temp1))
          this->temp1_sensor_->publish_state(this->data_.temp1);
        if (this->temp2_sensor_ != nullptr && this->data_.temp2 != 0 &&
            (!this->temp2_sensor_->has_state() || this->temp2_sensor_->state != this->data_.temp2))
          this->temp2_sensor_->publish_state(this->data_.temp2);
        if (this->output_enabled_binary_sensor_ != nullptr &&
            (!this->output_enabled_binary_sensor_->has_state() ||
             this->output_enabled_binary_sensor_->state != this->data_.output_enabled))
          this->output_enabled_binary_sensor_->publish_state(this->data_.output_enabled);

        ESP_LOGD(TAG, "Query: Vin=%.3fV Vout=%.3fV Iout=%.3fA Out=%s", this->data_.v_in, this->data_.v_out,
                 this->data_.i_out, this->data_.output_enabled ? "ON" : "OFF");

        // Write sample to datalogger if active
        if (this->datalog_active_) {
          this->datalog_write_sample_();
        }

        // Fire on_connect trigger on first successful query
        if (!this->connected_) {
          this->connected_ = true;
          if (this->connected_binary_sensor_ != nullptr)
            this->connected_binary_sensor_->publish_state(true);
          ESP_LOGI(TAG, "Connected to OpenDPS device");
          // Send connection status to update display icon (ethernet or wifi)
#ifdef USE_ETHERNET
          this->send_connection_status(CONN_ETHERNET_CONNECTED);
#else
          this->send_connection_status(CONN_WIFI_CONNECTED);
#endif
          // Restore saved brightness to DPS on first connection
          uint8_t saved_brightness = this->default_brightness_;
          if (this->brightness_pref_.load(&saved_brightness)) {
            ESP_LOGI(TAG, "Restoring saved brightness to DPS: %d", saved_brightness);
            this->set_brightness(saved_brightness);
          }
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
          this->upgrade_bootloader_ready_time_ = static_cast<uint32_t>(millis()) | 1u;
        } else {
          ESP_LOGE(TAG, "Device rejected firmware upgrade (status: %d)", status);
          this->upgrade_in_progress_ = false;
          this->upgrade_firmware_data_.clear();
          this->upgrade_last_chunk_time_ = 0;
          // Stay at 9600 on rejection - device may still be in bootloader
          // Preference flag preserved for reboot recovery; user can retry or power cycle
          if (this->firmware_baud_rate_ > 0) {
            ESP_LOGW(TAG, "Staying at 9600 - retry upgrade or power cycle DPS");
          }
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
          // New firmware starts at 9600; if operational baud differs, send cmd_set_baud
          uint32_t target_baud = (this->firmware_baud_rate_ > 0) ? this->firmware_baud_rate_ : 9600;
          if (target_baud != 9600) {
            ESP_LOGI(TAG, "Sending cmd_set_baud %u to new firmware", target_baud);
            this->set_uart_baud(target_baud);
          } else if (this->firmware_baud_rate_ > 0 && this->firmware_baud_rate_ != this->parent_->get_baud_rate()) {
            // firmware_baud_rate_ was 9600 but ESPHome UART may have been changed during upgrade
            this->parent_->flush();
            this->parent_->set_baud_rate(this->firmware_baud_rate_);
            this->parent_->load_settings();
          }
          this->firmware_baud_rate_ = 0;
          // Clear upgrade state from preferences
          uint32_t zero = 0;
          this->upgrade_state_pref_.save(&zero);
          global_preferences->sync();
          // Auto-restore calibration if enabled and backup exists
          if (this->auto_restore_calibration_ && this->has_calibration_backup()) {
            ESP_LOGI(TAG, "Auto-restoring calibration from backup...");
            // Small delay to let new firmware fully initialize before restoring calibration
            this->upgrade_cal_restore_time_ = static_cast<uint32_t>(millis()) | 1u;
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
          // Stay at 9600 on failure - device may still be in bootloader mode
          // User can retry upgrade or power cycle the DPS to restart it
          if (this->firmware_baud_rate_ > 0) {
            ESP_LOGW(TAG, "Staying at 9600 - retry upgrade or power cycle DPS");
          }
          // Restore connection icon after failed upgrade
#ifdef USE_ETHERNET
          this->send_connection_status(CONN_ETHERNET_CONNECTED);
#else
          this->send_connection_status(CONN_WIFI_CONNECTED);
#endif
        }
        break;
      }

      case CMD_CAL_REPORT: {
        // Parse calibration report response
        // Format: status, vout_adc(16), vin_adc(16), iout_adc(16), iout_dac(16), vout_dac(16),
        //         a_adc_k(f), a_adc_c(f), a_dac_k(f), a_dac_c(f), v_adc_k(f), v_adc_c(f),
        //         v_dac_k(f), v_dac_c(f), vin_adc_k(f), vin_adc_c(f)
        if (!status) {
          ESP_LOGW(TAG, "Calibration report failed with status: %d", status);
          break;
        }

        // Raw ADC/DAC readings
        this->calibration_data_.vout_adc = this->unpack16_(payload, pos);
        this->calibration_data_.vin_adc = this->unpack16_(payload, pos);
        this->calibration_data_.iout_adc = this->unpack16_(payload, pos);
        this->calibration_data_.iout_dac = this->unpack16_(payload, pos);
        this->calibration_data_.vout_dac = this->unpack16_(payload, pos);

        // Calibration coefficients (10 floats, little-endian)
        this->calibration_data_.a_adc_k = this->unpack_float_(payload, pos);
        this->calibration_data_.a_adc_c = this->unpack_float_(payload, pos);
        this->calibration_data_.a_dac_k = this->unpack_float_(payload, pos);
        this->calibration_data_.a_dac_c = this->unpack_float_(payload, pos);
        this->calibration_data_.v_adc_k = this->unpack_float_(payload, pos);
        this->calibration_data_.v_adc_c = this->unpack_float_(payload, pos);
        this->calibration_data_.v_dac_k = this->unpack_float_(payload, pos);
        this->calibration_data_.v_dac_c = this->unpack_float_(payload, pos);
        this->calibration_data_.vin_adc_k = this->unpack_float_(payload, pos);
        this->calibration_data_.vin_adc_c = this->unpack_float_(payload, pos);

        this->calibration_data_.last_update = millis();

        ESP_LOGI(TAG, "Calibration report received:");
        ESP_LOGI(TAG, "  ADC/DAC: vout_adc=%d, vin_adc=%d, iout_adc=%d, iout_dac=%d, vout_dac=%d",
                 this->calibration_data_.vout_adc, this->calibration_data_.vin_adc, this->calibration_data_.iout_adc,
                 this->calibration_data_.iout_dac, this->calibration_data_.vout_dac);
        ESP_LOGI(TAG, "  Current: A_ADC_K=%.6f, A_ADC_C=%.6f, A_DAC_K=%.6f, A_DAC_C=%.6f",
                 this->calibration_data_.a_adc_k, this->calibration_data_.a_adc_c, this->calibration_data_.a_dac_k,
                 this->calibration_data_.a_dac_c);
        ESP_LOGI(TAG, "  Voltage: V_ADC_K=%.6f, V_ADC_C=%.6f, V_DAC_K=%.6f, V_DAC_C=%.6f",
                 this->calibration_data_.v_adc_k, this->calibration_data_.v_adc_c, this->calibration_data_.v_dac_k,
                 this->calibration_data_.v_dac_c);
        ESP_LOGI(TAG, "  Vin: VIN_ADC_K=%.6f, VIN_ADC_C=%.6f", this->calibration_data_.vin_adc_k,
                 this->calibration_data_.vin_adc_c);

        // Fire calibration callback
        this->on_calibration_callback_.call();

        // If calibration assistant is running, collect sample
        if (this->cal_assistant_state_ != CAL_IDLE) {
          this->cal_assistant_collect_sample_();
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
  char buf[6];
  snprintf(buf, sizeof(buf), "%u", voltage_mv);
  this->set_parameter("voltage", buf);
}

void OpenDPS::set_current(float current) {
  // Convert current to milliamps
  uint16_t current_ma = static_cast<uint16_t>(current * 1000.0f);
  char buf[6];
  snprintf(buf, sizeof(buf), "%u", current_ma);
  this->set_parameter("current", buf);
}

void OpenDPS::set_brightness(uint8_t brightness) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_BRIGHTNESS);
  this->pack8_(payload, brightness);
  this->send_frame_(payload);

  // Store brightness in preferences and record set time
  this->brightness_ = brightness;
  this->brightness_pref_.save(&this->brightness_);
  this->brightness_set_time_ = millis();

  ESP_LOGI(TAG, "Set brightness: %d", brightness);
}

void OpenDPS::set_uart_baud(uint32_t baud) {
  static const uint32_t valid_bauds[] = {9600, 19200, 38400, 57600, 115200};
  bool valid = false;
  for (uint32_t v : valid_bauds) {
    if (baud == v) {
      valid = true;
      break;
    }
  }
  if (!valid) {
    ESP_LOGE(TAG, "Invalid baud rate: %u (valid: 9600, 19200, 38400, 57600, 115200)", baud);
    return;
  }

  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_BAUD);
  this->pack8_(payload, (baud >> 24) & 0xFF);
  this->pack8_(payload, (baud >> 16) & 0xFF);
  this->pack8_(payload, (baud >> 8) & 0xFF);
  this->pack8_(payload, baud & 0xFF);
  this->send_frame_(payload);

  // Device ACKs at old baud then switches; switch ESPHome UART after flush
  this->parent_->flush();
  this->parent_->set_baud_rate(baud);
  this->parent_->load_settings();
  ESP_LOGI(TAG, "UART baud rate changed to %u", baud);
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

void OpenDPS::request_calibration_report() {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_CAL_REPORT);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Requested calibration report");
}

void OpenDPS::set_calibration(const std::string &name, float value) {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_SET_CALIBRATION);

  // Pack calibration name as C string
  this->pack_cstr_(payload, name);

  // Pack float value as raw bytes (native byte order, matching firmware's direct cast)
  union {
    float f;
    uint8_t bytes[4];
  } float_bytes;
  float_bytes.f = value;
  for (int i = 0; i < 4; i++) {
    this->pack8_(payload, float_bytes.bytes[i]);
  }

  // Null terminator to indicate end of parameters
  this->pack8_(payload, 0);

  this->send_frame_(payload);
  ESP_LOGI(TAG, "Set calibration %s = %f", name.c_str(), value);
}

void OpenDPS::clear_calibration() {
  std::vector<uint8_t> payload;
  this->pack8_(payload, CMD_CLEAR_CALIBRATION);
  this->send_frame_(payload);
  ESP_LOGI(TAG, "Cleared calibration (reset to defaults)");
}

// ============================================================================
// Calibration Backup/Restore
// ============================================================================

bool OpenDPS::has_calibration_backup() const { return this->has_calibration_backup(this->calibration_backup_path_); }

bool OpenDPS::has_calibration_backup(const std::string &path) const {
  if (storage::global_storage == nullptr) {
    return false;
  }
  return storage::global_storage->file_exists(path);
}

bool OpenDPS::save_calibration_to_storage() {
  return this->save_calibration_to_storage(this->calibration_backup_path_);
}

bool OpenDPS::save_calibration_to_storage(const std::string &path) {
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Cannot save calibration: storage not available");
    return false;
  }

  // Check if we have valid calibration data
  if (this->calibration_data_.last_update == 0) {
    ESP_LOGW(TAG, "No calibration data to save - request calibration report first");
    return false;
  }

  // Text format (.cfg file) - human readable and editable
  // Format: KEY=VALUE pairs, one per line
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
           "# OpenDPS Calibration Backup\n"
           "# Generated by ESPHome OpenDPS component\n"
           "# Format: KEY=VALUE (do not modify structure)\n"
           "\n"
           "A_ADC_K=%.8f\n"
           "A_ADC_C=%.8f\n"
           "A_DAC_K=%.8f\n"
           "A_DAC_C=%.8f\n"
           "V_ADC_K=%.8f\n"
           "V_ADC_C=%.8f\n"
           "V_DAC_K=%.8f\n"
           "V_DAC_C=%.8f\n"
           "VIN_ADC_K=%.8f\n"
           "VIN_ADC_C=%.8f\n",
           this->calibration_data_.a_adc_k, this->calibration_data_.a_adc_c, this->calibration_data_.a_dac_k,
           this->calibration_data_.a_dac_c, this->calibration_data_.v_adc_k, this->calibration_data_.v_adc_c,
           this->calibration_data_.v_dac_k, this->calibration_data_.v_dac_c, this->calibration_data_.vin_adc_k,
           this->calibration_data_.vin_adc_c);

  std::string data_str(buffer);
  if (!storage::global_storage->write_file(path, data_str)) {
    ESP_LOGE(TAG, "Failed to write calibration backup to %s", path.c_str());
    return false;
  }

  ESP_LOGI(TAG, "Calibration saved to %s", path.c_str());
  ESP_LOGI(TAG, "  A: ADC_K=%.6f, ADC_C=%.6f, DAC_K=%.6f, DAC_C=%.6f", this->calibration_data_.a_adc_k,
           this->calibration_data_.a_adc_c, this->calibration_data_.a_dac_k, this->calibration_data_.a_dac_c);
  ESP_LOGI(TAG, "  V: ADC_K=%.6f, ADC_C=%.6f, DAC_K=%.6f, DAC_C=%.6f", this->calibration_data_.v_adc_k,
           this->calibration_data_.v_adc_c, this->calibration_data_.v_dac_k, this->calibration_data_.v_dac_c);
  ESP_LOGI(TAG, "  Vin: ADC_K=%.6f, ADC_C=%.6f", this->calibration_data_.vin_adc_k, this->calibration_data_.vin_adc_c);

  return true;
}

bool OpenDPS::restore_calibration_from_storage() {
  return this->restore_calibration_from_storage(this->calibration_backup_path_);
}

bool OpenDPS::restore_calibration_from_storage(const std::string &path) {
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Cannot restore calibration: storage not available");
    return false;
  }

  if (!storage::global_storage->file_exists(path)) {
    ESP_LOGW(TAG, "Calibration backup not found: %s", path.c_str());
    return false;
  }

  // Read file
  std::string file_data = storage::global_storage->read_file(path);
  if (file_data.empty()) {
    ESP_LOGE(TAG, "Failed to read calibration backup from %s", path.c_str());
    return false;
  }

  // Parse text format: KEY=VALUE pairs
  std::map<std::string, float> values;
  size_t pos = 0;
  while (pos < file_data.size()) {
    // Find end of line
    size_t eol = file_data.find('\n', pos);
    if (eol == std::string::npos) {
      eol = file_data.size();
    }

    std::string line = file_data.substr(pos, eol - pos);
    pos = eol + 1;

    // Skip empty lines and comments
    if (line.empty() || line[0] == '#' || line[0] == '\r') {
      continue;
    }

    // Remove trailing CR if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Parse KEY=VALUE
    size_t eq_pos = line.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = line.substr(0, eq_pos);
      std::string value_str = line.substr(eq_pos + 1);
      float value = strtof(value_str.c_str(), nullptr);
      values[key] = value;
    }
  }

  // Verify we have all required keys
  const char *required_keys[] = {"A_ADC_K", "A_ADC_C", "A_DAC_K", "A_DAC_C",   "V_ADC_K",
                                 "V_ADC_C", "V_DAC_K", "V_DAC_C", "VIN_ADC_K", "VIN_ADC_C"};
  for (const char *key : required_keys) {
    if (values.find(key) == values.end()) {
      ESP_LOGE(TAG, "Calibration backup missing required key: %s", key);
      return false;
    }
  }

  ESP_LOGI(TAG, "Restoring calibration from %s", path.c_str());
  ESP_LOGI(TAG, "  A: ADC_K=%.6f, ADC_C=%.6f, DAC_K=%.6f, DAC_C=%.6f", values["A_ADC_K"], values["A_ADC_C"],
           values["A_DAC_K"], values["A_DAC_C"]);
  ESP_LOGI(TAG, "  V: ADC_K=%.6f, ADC_C=%.6f, DAC_K=%.6f, DAC_C=%.6f", values["V_ADC_K"], values["V_ADC_C"],
           values["V_DAC_K"], values["V_DAC_C"]);
  ESP_LOGI(TAG, "  Vin: ADC_K=%.6f, ADC_C=%.6f", values["VIN_ADC_K"], values["VIN_ADC_C"]);

  // Apply calibration values to OpenDPS
  this->set_calibration("A_ADC_K", values["A_ADC_K"]);
  this->set_calibration("A_ADC_C", values["A_ADC_C"]);
  this->set_calibration("A_DAC_K", values["A_DAC_K"]);
  this->set_calibration("A_DAC_C", values["A_DAC_C"]);
  this->set_calibration("V_ADC_K", values["V_ADC_K"]);
  this->set_calibration("V_ADC_C", values["V_ADC_C"]);
  this->set_calibration("V_DAC_K", values["V_DAC_K"]);
  this->set_calibration("V_DAC_C", values["V_DAC_C"]);
  this->set_calibration("VIN_ADC_K", values["VIN_ADC_K"]);
  this->set_calibration("VIN_ADC_C", values["VIN_ADC_C"]);

  ESP_LOGI(TAG, "Calibration restored successfully");
  return true;
}

// ============================================================================
// Calibration Assistant
// ============================================================================

std::pair<float, float> OpenDPS::cal_best_fit_(const std::vector<float> &x, const std::vector<float> &y) {
  // Linear regression: y = k*x + c
  // Returns (k, c)
  if (x.size() < 2 || x.size() != y.size()) {
    return {1.0f, 0.0f};
  }

  float sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
  size_t n = x.size();

  for (size_t i = 0; i < n; i++) {
    sum_x += x[i];
    sum_y += y[i];
    sum_xy += x[i] * y[i];
    sum_xx += x[i] * x[i];
  }

  float x_bar = sum_x / n;
  float y_bar = sum_y / n;

  float ss_xy = sum_xy - n * x_bar * y_bar;
  float ss_xx = sum_xx - n * x_bar * x_bar;

  if (ss_xx == 0) {
    return {1.0f, 0.0f};
  }

  float k = ss_xy / ss_xx;
  float c = y_bar - k * x_bar;

  return {k, c};
}

void OpenDPS::start_calibration_assistant() {
  if (this->cal_assistant_state_ != CAL_IDLE) {
    ESP_LOGW(TAG, "Calibration assistant already running, cancelling previous");
    this->cancel_calibration_assistant();
  }

  // Reset all calibration state
  this->cal_samples_x_.clear();
  this->cal_samples_y_.clear();
  this->cal_sweep_step_ = 0;
  this->cal_max_v_dac_ = 4095;
  this->cal_vin_low_mv_ = 0;
  this->cal_vin_high_mv_ = 0;
  this->cal_max_dps_current_ = 5.0f;
  this->cal_load_resistance_ = 0;
  this->cal_load_max_wattage_ = 0;
  this->cal_last_action_time_ = millis();

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "CALIBRATION ASSISTANT STARTED");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "For calibration you will need:");
  ESP_LOGI(TAG, "  - A multimeter");
  ESP_LOGI(TAG, "  - A known load capable of handling the required power");
  ESP_LOGI(TAG, "  - A thick wire for shorting the output");
  ESP_LOGI(TAG, "  - 2 stable input voltages");
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "ENSURE NOTHING IS CONNECTED TO OUTPUT BEFORE STARTING!");
  ESP_LOGI(TAG, "");

  // Start with input voltage calibration
  this->cal_assistant_state_ = CAL_VIN_LOW_WAIT_INPUT;
  this->cal_assistant_process_();
}

void OpenDPS::calibration_assistant_step(float value) {
  ESP_LOGI(TAG, "Calibration step received value: %.3f", value);
  this->cal_last_action_time_ = millis();

  switch (this->cal_assistant_state_) {
    // === Input Voltage Calibration ===
    case CAL_VIN_LOW_WAIT_INPUT:
      // User entered measured input voltage LOW in mV
      this->cal_vin_low_mv_ = value;
      ESP_LOGI(TAG, "Vin LOW set to: %.0f mV", value);
      this->cal_assistant_state_ = CAL_VIN_LOW_RECORD;
      this->cal_assistant_process_();
      break;

    case CAL_UART_PAUSED:
      // User swapped supply, entered high voltage mV, and pressed Step
      // Resume UART, drain garbage, then use value as high voltage input
      ESP_LOGI(TAG, "Resuming UART communication...");
      this->rx_buffer_.clear();
      this->receiving_frame_ = false;
      while (this->available()) {
        uint8_t dummy;
        this->read_byte(&dummy);
      }
      this->connected_ = false;  // Force re-detection via next ping/query
      this->cal_vin_high_mv_ = value;
      ESP_LOGI(TAG, "Vin HIGH set to: %.0f mV", value);
      this->cal_assistant_state_ = CAL_VIN_HIGH_RECORD;
      this->cal_assistant_process_();
      break;

    // === Output Voltage Calibration ===
    case CAL_VOUT_LOW_WAIT_INPUT:
      // User entered measured output voltage at 10% DAC
      this->cal_samples_y_.push_back(value);  // Measured voltage
      ESP_LOGI(TAG, "Vout LOW measured: %.0f mV (V_ADC=%d)", value, this->calibration_data_.vout_adc);
      this->cal_assistant_state_ = CAL_VOUT_LOW_RECORD;
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_HIGH_WAIT_INPUT:
      // User entered measured output voltage at 90% DAC
      this->cal_samples_y_.push_back(value);  // Measured voltage
      ESP_LOGI(TAG, "Vout HIGH measured: %.0f mV (V_ADC=%d)", value, this->calibration_data_.vout_adc);
      this->cal_assistant_state_ = CAL_VOUT_HIGH_RECORD;
      this->cal_assistant_process_();
      break;

    // === Output Current Calibration ===
    case CAL_IOUT_MAX_CURRENT_INPUT:
      // User entered max DPS current (A)
      this->cal_max_dps_current_ = value;
      ESP_LOGI(TAG, "Max DPS current set to: %.1f A", value);
      this->cal_assistant_state_ = CAL_IOUT_LOAD_RESISTANCE_INPUT;
      this->cal_assistant_process_();
      break;

    case CAL_IOUT_LOAD_RESISTANCE_INPUT:
      // User entered load resistance (Ohm)
      this->cal_load_resistance_ = value;
      ESP_LOGI(TAG, "Load resistance set to: %.2f Ohm", value);
      this->cal_assistant_state_ = CAL_IOUT_LOAD_WATTAGE_INPUT;
      this->cal_assistant_process_();
      break;

    case CAL_IOUT_LOAD_WATTAGE_INPUT:
      // User entered load max wattage (W)
      this->cal_load_max_wattage_ = value;
      ESP_LOGI(TAG, "Load max wattage set to: %.1f W", value);
      this->cal_assistant_state_ = CAL_IOUT_CONNECT_LOAD;
      this->cal_assistant_process_();
      break;

    case CAL_IOUT_CONNECT_LOAD:
      // User confirmed load is connected (value ignored)
      ESP_LOGI(TAG, "Load connected, starting current calibration sweep");
      this->cal_sweep_step_ = 0;
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_IOUT_SWEEP;
      this->cal_assistant_process_();
      break;

    // === Constant Current (CC) Calibration ===
    case CAL_CC_SHORT_OUTPUT:
      // User confirmed output is shorted (value ignored)
      ESP_LOGI(TAG, "Output shorted, starting CC calibration sweep");
      this->cal_sweep_step_ = 0;
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_CC_SWEEP;
      this->cal_assistant_process_();
      break;

    default:
      ESP_LOGW(TAG, "Unexpected calibration step call in state %d", this->cal_assistant_state_);
      break;
  }
}

void OpenDPS::cancel_calibration_assistant() {
  ESP_LOGI(TAG, "Calibration assistant cancelled");
  this->cal_assistant_state_ = CAL_IDLE;
  this->enable_output(false);
  this->cal_samples_x_.clear();
  this->cal_samples_y_.clear();
}

void OpenDPS::cal_assistant_process_() {
  ESP_LOGD(TAG, "Processing calibration state: %d", this->cal_assistant_state_);

  switch (this->cal_assistant_state_) {
    // ==========================================================================
    // Input Voltage Calibration (two-point)
    // ==========================================================================
    case CAL_VIN_LOW_WAIT_INPUT:
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 1a: Input Voltage Calibration (Low Point)");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "Connect the LOWER supply voltage to the DPS");
      ESP_LOGI(TAG, "Measure the input voltage with a multimeter");
      ESP_LOGI(TAG, "Enter the measured value in mV using:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <measured_mV>");
      break;

    case CAL_VIN_LOW_RECORD:
      // Request fresh calibration report - callback will record ADC and advance
      ESP_LOGI(TAG, "Reading Vin ADC for low point...");
      this->request_calibration_report();
      this->cal_last_action_time_ = millis();
      // Don't proceed here - callback will handle recording and state transition
      break;

    case CAL_UART_PAUSED:
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "STEP 1b: Input Voltage Calibration (High Point)");
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "UART communication paused - OpenDPS can be powered off safely.");
      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "  1. Power OFF the OpenDPS device");
      ESP_LOGI(TAG, "  2. Disconnect the lower supply voltage");
      ESP_LOGI(TAG, "  3. Connect the HIGHER supply voltage");
      ESP_LOGI(TAG, "  4. Power the OpenDPS device back ON");
      ESP_LOGI(TAG, "  5. Measure the higher supply voltage with a multimeter");
      ESP_LOGI(TAG, "  6. Enter the measured voltage (mV) in the Calibration Input field");
      ESP_LOGI(TAG, "  7. Press the Calibration Step button");
      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "UART will resume automatically when Step is pressed.");
      break;

    case CAL_VIN_HIGH_RECORD:
      // Request fresh calibration report - callback will record ADC and advance
      ESP_LOGI(TAG, "Reading Vin ADC for high point...");
      this->request_calibration_report();
      this->cal_last_action_time_ = millis();
      // Don't proceed here - callback will handle recording and state transition
      break;

    case CAL_VIN_CALCULATE: {
      if (this->cal_samples_x_.size() < 2) {
        ESP_LOGE(TAG, "Not enough samples for two-point calibration");
        this->cal_assistant_state_ = CAL_ERROR;
        this->cal_assistant_process_();
        break;
      }

      auto [k, c] = this->cal_best_fit_(this->cal_samples_x_, this->cal_samples_y_);

      ESP_LOGI(TAG, "Input voltage calibration complete:");
      ESP_LOGI(TAG, "  Point 1: ADC=%.0f, Measured=%.0f mV", this->cal_samples_x_[0], this->cal_samples_y_[0]);
      ESP_LOGI(TAG, "  Point 2: ADC=%.0f, Measured=%.0f mV", this->cal_samples_x_[1], this->cal_samples_y_[1]);
      ESP_LOGI(TAG, "  VIN_ADC_K=%.6f, VIN_ADC_C=%.6f", k, c);
      this->set_calibration("VIN_ADC_K", k);
      this->set_calibration("VIN_ADC_C", c);

      // Move to output voltage calibration
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_VOUT_SWEEP;
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 2: Output Voltage Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "Finding maximum V_DAC value...");

      // Set up for sweep: V_DAC=0, A_DAC=4095 (max current limit)
      this->set_parameter("V_DAC", "0");
      this->set_parameter("A_DAC", "4095");
      this->enable_output(true);
      this->cal_sweep_step_ = 0;
      this->cal_last_action_time_ = millis();
      break;
    }

    // ==========================================================================
    // Output Voltage Calibration
    // ==========================================================================
    case CAL_VOUT_SWEEP: {
      // Async sweep: V_DAC from 0 to 4095 in 100 steps
      if (this->cal_sweep_step_ <= 100) {
        uint16_t v_dac = (this->cal_sweep_step_ * 4095) / 100;
        char dac_buf[6];
        snprintf(dac_buf, sizeof(dac_buf), "%u", v_dac);
        this->set_parameter("V_DAC", dac_buf);
        // Store DAC value now, ADC value will be stored in callback
        this->cal_samples_x_.push_back(static_cast<float>(v_dac));
        // Request calibration report - callback will collect sample and continue
        this->request_calibration_report();
        this->cal_last_action_time_ = millis();
        // Don't increment step here - done in callback after data arrives
      } else {
        ESP_LOGI(TAG, "V_DAC sweep complete, analyzing...");

        // Find where gradient drops to near zero (output saturates)
        this->cal_max_v_dac_ = 4095;
        for (size_t i = 1; i < this->cal_samples_y_.size(); i++) {
          float gradient = this->cal_samples_y_[i] - this->cal_samples_y_[i - 1];
          if (gradient < 0.1f && i > 10) {
            this->cal_max_v_dac_ = static_cast<uint16_t>(this->cal_samples_x_[i - 1]);
            break;
          }
        }
        ESP_LOGI(TAG, "Maximum usable V_DAC: %d", this->cal_max_v_dac_);

        // Clear samples and prepare for user measurements
        this->cal_samples_x_.clear();
        this->cal_samples_y_.clear();

        // Set V_DAC to 10%
        uint16_t v_dac_low = static_cast<uint16_t>(this->cal_max_v_dac_ * 0.1f);
        char dac_buf[6];
        snprintf(dac_buf, sizeof(dac_buf), "%u", v_dac_low);
        this->set_parameter("V_DAC", dac_buf);
        this->cal_samples_x_.push_back(static_cast<float>(v_dac_low));  // Store DAC value
        this->request_calibration_report();

        this->cal_assistant_state_ = CAL_VOUT_LOW_WAIT_INPUT;
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "Calibration Point 1 of 2 (10%% of max)");
        ESP_LOGI(TAG, "V_DAC set to: %d", v_dac_low);
        ESP_LOGI(TAG, "Measure the OUTPUT VOLTAGE with a multimeter");
        ESP_LOGI(TAG, "Enter the measured value in mV using:");
        ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <measured_mV>");
      }
      break;
    }

    case CAL_VOUT_LOW_RECORD:
      // Request fresh calibration report - callback will record ADC and advance
      ESP_LOGI(TAG, "Reading V_ADC for low point...");
      this->request_calibration_report();
      this->cal_last_action_time_ = millis();
      // Don't proceed here - callback will handle recording and state transition
      break;

    case CAL_VOUT_HIGH_RECORD:
      // Request fresh calibration report - callback will record ADC and advance
      ESP_LOGI(TAG, "Reading V_ADC for high point...");
      this->request_calibration_report();
      this->cal_last_action_time_ = millis();
      // Don't proceed here - callback will handle recording and state transition
      break;

    case CAL_VOUT_CALCULATE: {
      this->enable_output(false);

      // cal_samples_x_ contains: [dac_low, adc_low, dac_high, adc_high]
      // cal_samples_y_ contains: [measured_low, measured_high]
      std::vector<float> dac_values = {this->cal_samples_x_[0], this->cal_samples_x_[2]};
      std::vector<float> adc_values = {this->cal_samples_x_[1], this->cal_samples_x_[3]};
      std::vector<float> measured = {this->cal_samples_y_[0], this->cal_samples_y_[1]};

      // V_DAC: measured_mV -> DAC value
      auto [v_dac_k, v_dac_c] = this->cal_best_fit_(measured, dac_values);
      this->cal_v_dac_k_ = v_dac_k;
      this->cal_v_dac_c_ = v_dac_c;
      ESP_LOGI(TAG, "V_DAC calibration: V_DAC_K=%.6f, V_DAC_C=%.6f", v_dac_k, v_dac_c);
      this->set_calibration("V_DAC_K", v_dac_k);
      this->set_calibration("V_DAC_C", v_dac_c);

      // V_ADC: ADC -> measured_mV
      auto [v_adc_k, v_adc_c] = this->cal_best_fit_(adc_values, measured);
      this->cal_v_adc_k_ = v_adc_k;
      this->cal_v_adc_c_ = v_adc_c;
      ESP_LOGI(TAG, "V_ADC calibration: V_ADC_K=%.6f, V_ADC_C=%.6f", v_adc_k, v_adc_c);
      this->set_calibration("V_ADC_K", v_adc_k);
      this->set_calibration("V_ADC_C", v_adc_c);

      // Move to output current calibration
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_IOUT_MAX_CURRENT_INPUT;
      this->cal_assistant_process_();
      break;
    }

    // ==========================================================================
    // Output Current Calibration (requires load)
    // ==========================================================================
    case CAL_IOUT_MAX_CURRENT_INPUT:
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 3: Output Current Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "Enter the max output current of your DPS in Amps");
      ESP_LOGI(TAG, "(e.g., 5 for DPS5005, 3 for DPS5003)");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <max_current_A>");
      break;

    case CAL_IOUT_LOAD_RESISTANCE_INPUT:
      ESP_LOGI(TAG, "Enter your load resistance in Ohms:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <resistance_ohm>");
      break;

    case CAL_IOUT_LOAD_WATTAGE_INPUT:
      ESP_LOGI(TAG, "Enter your load's wattage rating in Watts:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <max_wattage_W>");
      break;

    case CAL_IOUT_CONNECT_LOAD: {
      // Calculate max safe voltage
      float v_max_input = this->cal_vin_high_mv_ * 0.9f;
      float v_max_wattage = sqrtf(this->cal_load_max_wattage_ * this->cal_load_resistance_) * 1000.0f;
      float v_max_current = this->cal_max_dps_current_ * this->cal_load_resistance_ * 1000.0f;
      float max_output_mv = std::min({v_max_input, v_max_wattage, v_max_current});

      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "Calibration parameters:");
      ESP_LOGI(TAG, "  Max DPS current: %.1f A", this->cal_max_dps_current_);
      ESP_LOGI(TAG, "  Load resistance: %.2f Ohm", this->cal_load_resistance_);
      ESP_LOGI(TAG, "  Load max wattage: %.1f W", this->cal_load_max_wattage_);
      ESP_LOGI(TAG, "  Max safe output voltage: %.0f mV", max_output_mv);
      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "Connect the load to the output of the DPS");
      ESP_LOGI(TAG, "Then call step to continue:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: 0");
      break;
    }

    case CAL_IOUT_SWEEP: {
      // Async sweep: each step sets parameters and waits for cal report callback
      float v_max_input = this->cal_vin_high_mv_ * 0.9f;
      float v_max_wattage = sqrtf(this->cal_load_max_wattage_ * this->cal_load_resistance_) * 1000.0f;
      float v_max_current = this->cal_max_dps_current_ * this->cal_load_resistance_ * 1000.0f;
      float max_output_mv = std::min({v_max_input, v_max_wattage, v_max_current});

      uint8_t num_steps = 15;
      if (this->cal_sweep_step_ < num_steps) {
        float output_voltage = max_output_mv * (static_cast<float>(this->cal_sweep_step_) / num_steps);
        uint16_t output_dac = static_cast<uint16_t>(this->cal_v_dac_k_ * output_voltage + this->cal_v_dac_c_);

        char dac_buf[6];
        snprintf(dac_buf, sizeof(dac_buf), "%u", output_dac);
        this->set_parameter("V_DAC", dac_buf);
        this->enable_output(true);
        // Request calibration report - callback will collect sample and continue
        this->request_calibration_report();
        this->cal_last_action_time_ = millis();
        // Don't increment step here - done in callback after data arrives
      } else {
        this->cal_assistant_state_ = CAL_IOUT_CALCULATE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_IOUT_CALCULATE: {
      this->enable_output(false);

      // A_ADC: ADC -> current (mA or A depending on scale)
      auto [a_adc_k, a_adc_c] = this->cal_best_fit_(this->cal_samples_x_, this->cal_samples_y_);
      this->cal_a_adc_k_ = a_adc_k;
      this->cal_a_adc_c_ = a_adc_c;
      ESP_LOGI(TAG, "A_ADC calibration: A_ADC_K=%.6f, A_ADC_C=%.6f", a_adc_k, a_adc_c);
      this->set_calibration("A_ADC_K", a_adc_k);
      this->set_calibration("A_ADC_C", a_adc_c);

      // Move to CC calibration
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_CC_SHORT_OUTPUT;
      this->cal_assistant_process_();
      break;
    }

    // ==========================================================================
    // Constant Current (CC) Calibration (requires short)
    // ==========================================================================
    case CAL_CC_SHORT_OUTPUT:
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 4: Constant Current Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "SHORT the output with a thick wire capable of %.1f A", this->cal_max_dps_current_);
      ESP_LOGI(TAG, "Then call step to continue:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: 0");
      break;

    case CAL_CC_SWEEP: {
      // Phase 1: Async sweep to find linear range of A_DAC
      // Set V_DAC to max at start of sweep
      if (this->cal_sweep_step_ == 0) {
        this->set_parameter("V_DAC", "4095");
      }

      uint8_t num_steps = 100;
      if (this->cal_sweep_step_ <= num_steps) {
        uint16_t a_dac = (this->cal_sweep_step_ * 4095) / num_steps;
        char dac_buf[6];
        snprintf(dac_buf, sizeof(dac_buf), "%u", a_dac);
        this->set_parameter("A_DAC", dac_buf);
        this->enable_output(true);
        // Store DAC value now, ADC value will be stored in callback
        this->cal_samples_x_.push_back(static_cast<float>(a_dac));
        // Request calibration report - callback will collect sample and continue
        this->request_calibration_report();
        this->cal_last_action_time_ = millis();
        // Don't increment step here - done in callback after data arrives
      } else {
        this->enable_output(false);
        ESP_LOGI(TAG, "A_DAC sweep complete, analyzing...");

        // Find linear range
        this->cal_a_dac_lower_ = 0;
        this->cal_a_dac_upper_ = 4095;

        for (size_t i = 1; i < this->cal_samples_y_.size(); i++) {
          float gradient = this->cal_samples_y_[i] - this->cal_samples_y_[i - 1];
          if (gradient > 0.1f && this->cal_a_dac_lower_ == 0) {
            this->cal_a_dac_lower_ = static_cast<uint16_t>(this->cal_samples_x_[i]);
          }
          if (gradient < 0.1f && this->cal_a_dac_lower_ > 0) {
            this->cal_a_dac_upper_ = static_cast<uint16_t>(this->cal_samples_x_[i - 1]);
            break;
          }
        }

        // Trim by 10% on each side
        uint16_t range = this->cal_a_dac_upper_ - this->cal_a_dac_lower_;
        this->cal_a_dac_lower_ += range / 10;
        this->cal_a_dac_upper_ -= range / 10;
        ESP_LOGI(TAG, "A_DAC linear range: %d - %d", this->cal_a_dac_lower_, this->cal_a_dac_upper_);

        // Prepare for measurement sweep in linear range
        this->cal_samples_x_.clear();
        this->cal_samples_y_.clear();
        this->cal_sweep_step_ = 0;

        // Move to measurement phase
        this->cal_assistant_state_ = CAL_CC_MEASURE_SWEEP;
        ESP_LOGI(TAG, "Calibrating output current DAC...");
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_CC_MEASURE_SWEEP: {
      // Phase 2: Async measurement sweep in the linear range
      uint8_t measure_steps = 15;
      if (this->cal_sweep_step_ < measure_steps) {
        float ratio = static_cast<float>(this->cal_sweep_step_) / measure_steps;
        uint16_t a_dac =
            this->cal_a_dac_lower_ + static_cast<uint16_t>((this->cal_a_dac_upper_ - this->cal_a_dac_lower_) * ratio);

        char dac_buf[6];
        snprintf(dac_buf, sizeof(dac_buf), "%u", a_dac);
        this->set_parameter("A_DAC", dac_buf);
        this->enable_output(true);
        // Store DAC value now for later use in callback
        // We use a temporary storage approach - store DAC in cal_max_v_dac_ temporarily
        this->cal_max_v_dac_ = a_dac;
        // Request calibration report - callback will collect sample and continue
        this->request_calibration_report();
        this->cal_last_action_time_ = millis();
        // Don't increment step here - done in callback after data arrives
      } else {
        this->enable_output(false);
        this->cal_assistant_state_ = CAL_CC_CALCULATE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_CC_CALCULATE: {
      // A_DAC: current -> DAC
      auto [a_dac_k, a_dac_c] = this->cal_best_fit_(this->cal_samples_x_, this->cal_samples_y_);
      ESP_LOGI(TAG, "A_DAC calibration: A_DAC_K=%.6f, A_DAC_C=%.6f", a_dac_k, a_dac_c);
      this->set_calibration("A_DAC_K", a_dac_k);
      this->set_calibration("A_DAC_C", a_dac_c);

      this->cal_assistant_state_ = CAL_COMPLETE;
      this->cal_assistant_process_();
      break;
    }

    // ==========================================================================
    // Done
    // ==========================================================================
    case CAL_COMPLETE:
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "CALIBRATION COMPLETE!");
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "To reset to defaults: opendps.clear_calibration");
      ESP_LOGI(TAG, "To save backup: opendps.save_calibration");
      this->cal_assistant_state_ = CAL_IDLE;
      break;

    case CAL_ERROR:
      ESP_LOGE(TAG, "Calibration error occurred");
      this->enable_output(false);
      this->cal_assistant_state_ = CAL_IDLE;
      break;

    default:
      break;
  }
}

void OpenDPS::cal_assistant_collect_sample_() {
  // Called when a calibration report is received during calibration
  // Store the relevant ADC value based on current state and continue the sweep
  switch (this->cal_assistant_state_) {
    case CAL_VIN_LOW_RECORD:
      // Record ADC reading for low point, then pause UART for supply swap
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vin_adc));
      this->cal_samples_y_.push_back(this->cal_vin_low_mv_);
      ESP_LOGI(TAG, "Recorded Vin LOW: ADC=%d, measured=%.0f mV", this->calibration_data_.vin_adc,
               this->cal_vin_low_mv_);
      this->cal_assistant_state_ = CAL_UART_PAUSED;
      this->cal_assistant_process_();
      break;

    case CAL_VIN_HIGH_RECORD:
      // Record ADC reading for high point and advance to calculate
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vin_adc));
      this->cal_samples_y_.push_back(this->cal_vin_high_mv_);
      ESP_LOGI(TAG, "Recorded Vin HIGH: ADC=%d, measured=%.0f mV", this->calibration_data_.vin_adc,
               this->cal_vin_high_mv_);
      this->cal_assistant_state_ = CAL_VIN_CALCULATE;
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_LOW_RECORD: {
      // Record ADC reading for low point, then set up for high point
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      ESP_LOGI(TAG, "Recorded: V_DAC=%.0f, V_ADC=%d, measured=%.0f mV", this->cal_samples_x_[0],
               this->calibration_data_.vout_adc, this->cal_samples_y_[0]);

      // Set V_DAC to 90% for the high measurement
      uint16_t v_dac_high = static_cast<uint16_t>(this->cal_max_v_dac_ * 0.9f);
      char dac_buf[6];
      snprintf(dac_buf, sizeof(dac_buf), "%u", v_dac_high);
      this->set_parameter("V_DAC", dac_buf);
      this->cal_samples_x_.push_back(static_cast<float>(v_dac_high));  // Store DAC value

      this->cal_assistant_state_ = CAL_VOUT_HIGH_WAIT_INPUT;
      ESP_LOGI(TAG, "");
      ESP_LOGI(TAG, "Calibration Point 2 of 2 (90%% of max)");
      ESP_LOGI(TAG, "V_DAC set to: %d", v_dac_high);
      ESP_LOGI(TAG, "Measure the OUTPUT VOLTAGE with a multimeter");
      ESP_LOGI(TAG, "Enter the measured value in mV using:");
      ESP_LOGI(TAG, "  opendps.calibration_assistant_step: <measured_mV>");
      break;
    }

    case CAL_VOUT_HIGH_RECORD:
      // Record ADC reading for high point and advance to calculate
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      ESP_LOGI(TAG, "Recorded: V_DAC=%.0f, V_ADC=%d, measured=%.0f mV", this->cal_samples_x_[2],
               this->calibration_data_.vout_adc, this->cal_samples_y_[1]);
      this->cal_assistant_state_ = CAL_VOUT_CALCULATE;
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_SWEEP:
      // Store vout_adc for sweep analysis
      this->cal_samples_y_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      this->cal_sweep_step_++;
      if (this->cal_sweep_step_ % 25 == 0) {
        ESP_LOGI(TAG, "V_DAC sweep: %d%%", this->cal_sweep_step_);
      }
      // Continue sweep
      this->cal_assistant_process_();
      break;

    case CAL_IOUT_SWEEP: {
      // Calculate actual current: I = V / R (using calibrated V_ADC)
      float actual_voltage = this->calibration_data_.vout_adc * this->cal_v_adc_k_ + this->cal_v_adc_c_;
      float actual_current = actual_voltage / this->cal_load_resistance_;
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.iout_adc));
      this->cal_samples_y_.push_back(actual_current);
      this->cal_sweep_step_++;
      if (this->cal_sweep_step_ % 5 == 0) {
        ESP_LOGI(TAG, "Current calibration: %d/15", this->cal_sweep_step_);
      }
      // Continue sweep
      this->cal_assistant_process_();
      break;
    }

    case CAL_CC_SWEEP:
      // Store iout_adc for sweep analysis (DAC value already stored before request)
      this->cal_samples_y_.push_back(static_cast<float>(this->calibration_data_.iout_adc));
      this->cal_sweep_step_++;
      if (this->cal_sweep_step_ % 25 == 0) {
        ESP_LOGI(TAG, "A_DAC sweep: %d%%", this->cal_sweep_step_);
      }
      // Continue sweep
      this->cal_assistant_process_();
      break;

    case CAL_CC_MEASURE_SWEEP: {
      // Calculate current from ADC using previously calibrated A_ADC
      float i_out = this->calibration_data_.iout_adc * this->cal_a_adc_k_ + this->cal_a_adc_c_;
      // Retrieve DAC value from temporary storage
      uint16_t a_dac = this->cal_max_v_dac_;
      this->cal_samples_x_.push_back(i_out);                      // Current
      this->cal_samples_y_.push_back(static_cast<float>(a_dac));  // DAC value
      this->cal_sweep_step_++;
      ESP_LOGI(TAG, "CC calibration: %d/15 (A_DAC=%d, I=%.3f A)", this->cal_sweep_step_, a_dac, i_out);
      // Continue sweep
      this->cal_assistant_process_();
      break;
    }

    default:
      // Other states don't need sample collection from callback
      break;
  }
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

// ============================================================================
// Datalogger Implementation
// ============================================================================

bool OpenDPS::datalog_ensure_buffer_() {
  if (this->datalog_buffers_[0] != nullptr) {
    return true;  // Already allocated
  }

  size_t requested_size = this->datalog_config_.buffer_size;
  if (requested_size == 0) {
    requested_size = 65536;  // Default 64KB
  }

  // We need three buffers for triple-buffering
  size_t total_size = requested_size * DATALOG_NUM_BUFFERS;

#ifdef USE_ESP32
  // Try to allocate in PSRAM first
  if (esp_psram_is_initialized()) {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram >= total_size + 8192) {  // Leave some margin
      bool alloc_ok = true;
      for (uint8_t i = 0; i < DATALOG_NUM_BUFFERS; i++) {
        this->datalog_buffers_[i] = static_cast<uint8_t *>(heap_caps_malloc(requested_size, MALLOC_CAP_SPIRAM));
        if (this->datalog_buffers_[i] == nullptr) {
          alloc_ok = false;
          break;
        }
        this->datalog_buffer_sizes_[i] = 0;
      }

      if (alloc_ok) {
        this->datalog_buffer_capacity_ = requested_size;
        this->datalog_buffer_in_psram_ = true;
        this->datalog_write_idx_ = 0;
        this->datalog_queue_idx_ = 0xFF;  // No buffer queued initially
        this->datalog_flush_idx_ = 0xFF;  // No buffer flushing initially

        // Create FreeRTOS synchronization primitives
        this->datalog_mutex_ = xSemaphoreCreateMutex();
        this->datalog_write_sem_ = xSemaphoreCreateBinary();

        if (this->datalog_mutex_ == nullptr || this->datalog_write_sem_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create FreeRTOS semaphores");
          for (uint8_t i = 0; i < DATALOG_NUM_BUFFERS; i++) {
            if (this->datalog_buffers_[i] != nullptr) {
              heap_caps_free(this->datalog_buffers_[i]);
              this->datalog_buffers_[i] = nullptr;
            }
          }
          return false;
        }

        // Create background write task on core 0 to avoid blocking main loop on core 1
        this->datalog_task_running_ = true;
        BaseType_t result = xTaskCreatePinnedToCore(datalog_write_task_, "datalog_write", 4096, this,
                                                    5,  // Priority 5 (runs when semaphore given)
                                                    &this->datalog_task_handle_,
                                                    0  // Run on core 0 (separate from main loop)
        );
        if (result != pdPASS) {
          ESP_LOGE(TAG, "Failed to create datalog write task");
          vSemaphoreDelete(this->datalog_mutex_);
          vSemaphoreDelete(this->datalog_write_sem_);
          for (uint8_t i = 0; i < DATALOG_NUM_BUFFERS; i++) {
            if (this->datalog_buffers_[i] != nullptr) {
              heap_caps_free(this->datalog_buffers_[i]);
              this->datalog_buffers_[i] = nullptr;
            }
          }
          this->datalog_mutex_ = nullptr;
          this->datalog_write_sem_ = nullptr;
          return false;
        }

        ESP_LOGI(TAG, "Datalogger triple-buffer allocated in PSRAM: 3x%u bytes, async task started", requested_size);
        return true;
      }
      // Cleanup partial allocation
      for (uint8_t i = 0; i < DATALOG_NUM_BUFFERS; i++) {
        if (this->datalog_buffers_[i] != nullptr) {
          heap_caps_free(this->datalog_buffers_[i]);
          this->datalog_buffers_[i] = nullptr;
        }
      }
    }
    ESP_LOGW(TAG, "PSRAM allocation failed, falling back to regular heap (synchronous writes)");
  }

  // Fall back to regular heap with single smaller buffer (synchronous writes)
  size_t heap_size = std::min(requested_size, static_cast<size_t>(8192));  // Max 8KB in regular heap
  this->datalog_buffers_[0] = static_cast<uint8_t *>(malloc(heap_size));
  if (this->datalog_buffers_[0] != nullptr) {
    this->datalog_buffer_capacity_ = heap_size;
    this->datalog_buffer_sizes_[0] = 0;
    this->datalog_buffer_in_psram_ = false;
    this->datalog_write_idx_ = 0;
    this->datalog_queue_idx_ = 0xFF;
    this->datalog_flush_idx_ = 0xFF;
    ESP_LOGI(TAG, "Datalogger single-buffer allocated in heap: %u bytes (synchronous writes)", heap_size);
    return true;
  }
#else
  // Non-ESP32: single buffer, synchronous writes
  size_t heap_size = std::min(requested_size, static_cast<size_t>(8192));
  this->datalog_buffers_[0] = static_cast<uint8_t *>(malloc(heap_size));
  if (this->datalog_buffers_[0] != nullptr) {
    this->datalog_buffer_capacity_ = heap_size;
    this->datalog_buffer_sizes_[0] = 0;
    this->datalog_buffer_in_psram_ = false;
    this->datalog_write_idx_ = 0;
    ESP_LOGI(TAG, "Datalogger buffer allocated: %u bytes", heap_size);
    return true;
  }
#endif

  ESP_LOGE(TAG, "Failed to allocate datalogger buffer");
  return false;
}

std::string OpenDPS::datalog_generate_filename_() {
  std::string filename;

  // Priority: explicit filename > filename_id > filename_format > default
  if (!this->datalog_config_.filename_id.empty()) {
    // Use fixed ID with appropriate extension
    filename = this->datalog_config_.filename_id;
    if (this->datalog_config_.format == "csv") {
      if (filename.find(".csv") == std::string::npos) {
        filename += ".csv";
      }
    } else {
      if (filename.find(".bin") == std::string::npos) {
        filename += ".bin";
      }
    }
  } else if (!this->datalog_config_.filename_format.empty()) {
    // Use strftime format with ESPTime
    ESPTime now = ESPTime::from_epoch_local(::time(nullptr));
    char time_buffer[ESPTime::STRFTIME_BUFFER_SIZE];
    now.strftime(time_buffer, sizeof(time_buffer), this->datalog_config_.filename_format.c_str());
    filename = time_buffer;
  } else {
    // Default format: dps_YYYYMMDD_HHMMSS.csv
    ESPTime now = ESPTime::from_epoch_local(::time(nullptr));
    char time_buffer[ESPTime::STRFTIME_BUFFER_SIZE];
    if (this->datalog_config_.format == "csv") {
      now.strftime(time_buffer, sizeof(time_buffer), "dps_%Y%m%d_%H%M%S.csv");
    } else {
      now.strftime(time_buffer, sizeof(time_buffer), "dps_%Y%m%d_%H%M%S.bin");
    }
    filename = time_buffer;
  }

  // Combine with storage path
  std::string full_path = this->datalog_config_.storage_path;
  if (!full_path.empty() && full_path.back() != '/') {
    full_path += '/';
  }
  full_path += filename;

  return full_path;
}

void OpenDPS::datalog_write_csv_header_() {
#ifdef USE_STORAGE
  if (storage::global_storage == nullptr) {
    return;
  }

  std::string header;
  uint16_t cols = this->datalog_config_.columns;

  if (cols & DATALOG_COL_ELAPSED_MS)
    header += "elapsed_ms";
  if (cols & DATALOG_COL_SYSTEM_TIME) {
    if (!header.empty())
      header += ",";
    header += "system_time";
  }
  if (cols & DATALOG_COL_VOLTAGE_IN) {
    if (!header.empty())
      header += ",";
    header += "voltage_in_v";
  }
  if (cols & DATALOG_COL_VOLTAGE_OUT) {
    if (!header.empty())
      header += ",";
    header += "voltage_out_v";
  }
  if (cols & DATALOG_COL_CURRENT_OUT) {
    if (!header.empty())
      header += ",";
    header += "current_out_a";
  }
  if (cols & DATALOG_COL_POWER_OUT) {
    if (!header.empty())
      header += ",";
    header += "power_out_w";
  }
  if (cols & DATALOG_COL_OUTPUT_ENABLED) {
    if (!header.empty())
      header += ",";
    header += "output_enabled";
  }
  if (cols & DATALOG_COL_TEMP1) {
    if (!header.empty())
      header += ",";
    header += "temp1_c";
  }
  if (cols & DATALOG_COL_TEMP2) {
    if (!header.empty())
      header += ",";
    header += "temp2_c";
  }
  header += "\n";

  // Write header directly to file using StorageDevice
  storage::StorageDevice *device = this->datalog_storage_device_;
  ESP_LOGV(TAG, "Datalogger: writing CSV header, device=%p, relative_path=%s", device,
           this->datalog_relative_path_.c_str());
  if (device != nullptr && device->is_available()) {
    // Use write_file to create file with header
    ESP_LOGV(TAG, "Datalogger: calling device->write_file(%s, %d bytes)", this->datalog_relative_path_.c_str(),
             header.size());
    bool result = device->write_file(this->datalog_relative_path_.c_str(),
                                     reinterpret_cast<const uint8_t *>(header.data()), header.size());
    ESP_LOGV(TAG, "Datalogger: write_file returned %d", result);
    if (!result) {
      ESP_LOGE(TAG, "Failed to write CSV header to %s", this->datalog_filepath_.c_str());
      return;
    }
  } else {
    // Fall back to global storage POSIX methods
    ESP_LOGV(TAG, "Datalogger: using POSIX fallback for %s", this->datalog_filepath_.c_str());
    if (!storage::global_storage->write_file(this->datalog_filepath_, header)) {
      ESP_LOGE(TAG, "Failed to write CSV header via POSIX to %s", this->datalog_filepath_.c_str());
      return;
    }
  }
  ESP_LOGD(TAG, "Wrote CSV header to %s", this->datalog_filepath_.c_str());
#endif
}

size_t OpenDPS::datalog_format_csv_row_(char *buffer, size_t max_len) {
  size_t pos = 0;
  uint16_t cols = this->datalog_config_.columns;
  bool first = true;

  // Elapsed time since log start (milliseconds)
  if (cols & DATALOG_COL_ELAPSED_MS) {
    uint32_t elapsed = millis() - this->datalog_start_time_;
    pos += snprintf(buffer + pos, max_len - pos, "%lu", (unsigned long) elapsed);
    first = false;
  }

  // System time from time component (ISO8601 format: YYYY-MM-DD HH:MM:SS)
  if (cols & DATALOG_COL_SYSTEM_TIME) {
    if (!first && pos < max_len - 1) {
      buffer[pos++] = ',';
    }
#ifdef USE_TIME
    if (this->datalog_time_ != nullptr) {
      ESPTime now = this->datalog_time_->now();
      if (now.is_valid()) {
        pos += snprintf(buffer + pos, max_len - pos, "%04d-%02d-%02d %02d:%02d:%02d", now.year, now.month,
                        now.day_of_month, now.hour, now.minute, now.second);
      } else {
        // Time not yet synchronized
        pos += snprintf(buffer + pos, max_len - pos, "----");
      }
    } else {
      // No time component configured
      pos += snprintf(buffer + pos, max_len - pos, "N/A");
    }
#else
    pos += snprintf(buffer + pos, max_len - pos, "N/A");
#endif
    first = false;
  }

  if (cols & DATALOG_COL_VOLTAGE_IN) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.3f" : ",%.3f", this->data_.v_in);
    first = false;
  }
  if (cols & DATALOG_COL_VOLTAGE_OUT) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.3f" : ",%.3f", this->data_.v_out);
    first = false;
  }
  if (cols & DATALOG_COL_CURRENT_OUT) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.3f" : ",%.3f", this->data_.i_out);
    first = false;
  }
  if (cols & DATALOG_COL_POWER_OUT) {
    float power = this->data_.v_out * this->data_.i_out;
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.3f" : ",%.3f", power);
    first = false;
  }
  if (cols & DATALOG_COL_OUTPUT_ENABLED) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%d" : ",%d", this->data_.output_enabled ? 1 : 0);
    first = false;
  }
  if (cols & DATALOG_COL_TEMP1) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.1f" : ",%.1f", this->data_.temp1);
    first = false;
  }
  if (cols & DATALOG_COL_TEMP2) {
    pos += snprintf(buffer + pos, max_len - pos, first ? "%.1f" : ",%.1f", this->data_.temp2);
    first = false;
  }

  // Add newline
  if (pos < max_len - 1) {
    buffer[pos++] = '\n';
  }

  return pos;
}

size_t OpenDPS::datalog_format_binary_row_(uint8_t *buffer, size_t max_len) {
  // Binary format: fixed-size struct for fast parsing
  // Header byte (0xAA) + column_flags(2) + elapsed_ms(4) + system_time(4) + vin(4) + vout(4) + iout(4) + power(4) +
  // enabled(1) + temp1(4) + temp2(4) Total: ~36 bytes per sample (all columns)

  if (max_len < 40) {
    return 0;
  }

  size_t pos = 0;
  uint16_t cols = this->datalog_config_.columns;

  // Start marker
  buffer[pos++] = 0xAA;

  // Column presence flags (2 bytes for extended columns)
  buffer[pos++] = static_cast<uint8_t>(cols & 0xFF);
  buffer[pos++] = static_cast<uint8_t>((cols >> 8) & 0xFF);

  // Elapsed time since log start (milliseconds)
  if (cols & DATALOG_COL_ELAPSED_MS) {
    uint32_t elapsed = millis() - this->datalog_start_time_;
    memcpy(buffer + pos, &elapsed, 4);
    pos += 4;
  }

  // System time as Unix timestamp (seconds since epoch)
  if (cols & DATALOG_COL_SYSTEM_TIME) {
    uint32_t unix_time = 0;
#ifdef USE_TIME
    if (this->datalog_time_ != nullptr) {
      ESPTime now = this->datalog_time_->now();
      if (now.is_valid()) {
        unix_time = static_cast<uint32_t>(now.timestamp);
      }
    }
#endif
    memcpy(buffer + pos, &unix_time, 4);
    pos += 4;
  }

  if (cols & DATALOG_COL_VOLTAGE_IN) {
    memcpy(buffer + pos, &this->data_.v_in, 4);
    pos += 4;
  }
  if (cols & DATALOG_COL_VOLTAGE_OUT) {
    memcpy(buffer + pos, &this->data_.v_out, 4);
    pos += 4;
  }
  if (cols & DATALOG_COL_CURRENT_OUT) {
    memcpy(buffer + pos, &this->data_.i_out, 4);
    pos += 4;
  }
  if (cols & DATALOG_COL_POWER_OUT) {
    float power = this->data_.v_out * this->data_.i_out;
    memcpy(buffer + pos, &power, 4);
    pos += 4;
  }
  if (cols & DATALOG_COL_OUTPUT_ENABLED) {
    buffer[pos++] = this->data_.output_enabled ? 1 : 0;
  }
  if (cols & DATALOG_COL_TEMP1) {
    memcpy(buffer + pos, &this->data_.temp1, 4);
    pos += 4;
  }
  if (cols & DATALOG_COL_TEMP2) {
    memcpy(buffer + pos, &this->data_.temp2, 4);
    pos += 4;
  }

  return pos;
}

void OpenDPS::datalog_write_sample_() {
  if (!this->datalog_active_ || this->datalog_buffers_[0] == nullptr) {
    return;
  }

  // Format the row into a temporary buffer
  char row_buffer[256];
  size_t row_len;

  if (this->datalog_config_.format == "csv") {
    row_len = this->datalog_format_csv_row_(row_buffer, sizeof(row_buffer));
  } else {
    row_len = this->datalog_format_binary_row_(reinterpret_cast<uint8_t *>(row_buffer), sizeof(row_buffer));
  }

  if (row_len == 0) {
    return;
  }

  // Get the active write buffer
  uint8_t *active_buffer = this->datalog_buffers_[this->datalog_write_idx_];
  size_t &buffer_pos = this->datalog_buffer_sizes_[this->datalog_write_idx_];

  // Check if we need to rotate buffers first (buffer full)
  bool just_rotated = false;
  if (buffer_pos + row_len > this->datalog_buffer_capacity_) {
    this->datalog_request_flush_();
    just_rotated = true;
    // After rotation, update active buffer reference
    active_buffer = this->datalog_buffers_[this->datalog_write_idx_];
    buffer_pos = this->datalog_buffer_sizes_[this->datalog_write_idx_];
  }

  // Copy row to active buffer
  if (buffer_pos + row_len <= this->datalog_buffer_capacity_) {
    memcpy(active_buffer + buffer_pos, row_buffer, row_len);
    buffer_pos += row_len;
    this->datalog_sample_count_++;
  }

  // Auto-flush if buffer is 75% full or flush interval elapsed
  // Skip if we just rotated to avoid double-rotation
  if (!just_rotated) {
    uint32_t now = millis();
    bool should_flush = false;

    if (buffer_pos > (this->datalog_buffer_capacity_ * 3 / 4)) {
      should_flush = true;
    }

    if (this->datalog_config_.flush_interval_ms > 0 &&
        (now - this->datalog_last_flush_) >= this->datalog_config_.flush_interval_ms) {
      should_flush = true;
    }

    if (should_flush && buffer_pos > 0) {
      this->datalog_request_flush_();
    }
  }
}

#if defined(USE_ESP32) && defined(USE_STORAGE)
void OpenDPS::datalog_rotate_buffers_() {
  // Triple-buffer rotation: move current write buffer to queue, get a free buffer for writing
  // Called from main thread when requesting a flush
  // Mutex must be held by caller

  // Current write buffer becomes queued (if no queue, or queue becomes flush target)
  uint8_t old_write_idx = this->datalog_write_idx_;

  // Find a free buffer (not being written, queued, or flushed)
  for (uint8_t i = 0; i < DATALOG_NUM_BUFFERS; i++) {
    if (i != old_write_idx && i != this->datalog_queue_idx_ && i != this->datalog_flush_idx_) {
      // Found a free buffer - use it for writing
      this->datalog_write_idx_ = i;
      this->datalog_buffer_sizes_[i] = 0;  // Reset new write buffer

      // Old write buffer becomes queued
      if (this->datalog_queue_idx_ == 0xFF) {
        // No buffer was queued, old write buffer becomes the queue
        this->datalog_queue_idx_ = old_write_idx;
      } else {
        // There was already a queued buffer - this shouldn't happen with triple buffering
        // but if it does, we'll overwrite the queue (data loss, but keeps system running)
        ESP_LOGW(TAG, "Triple buffer overflow - replacing queued buffer");
        this->datalog_queue_idx_ = old_write_idx;
      }
      return;
    }
  }

  // No free buffer found - all buffers are in use (shouldn't happen with triple buffering)
  ESP_LOGW(TAG, "No free buffer available for rotation");
}

void OpenDPS::datalog_write_task_(void *arg) {
  OpenDPS *self = static_cast<OpenDPS *>(arg);

  while (self->datalog_task_running_) {
    // Wait for signal to write (with timeout for graceful shutdown)
    if (xSemaphoreTake(self->datalog_write_sem_, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!self->datalog_task_running_) {
        break;
      }

      // Take mutex for entire operation - task is on separate core so won't block main loop
      if (xSemaphoreTake(self->datalog_mutex_, portMAX_DELAY) != pdTRUE) {
        continue;
      }

      // Check if there's a buffer to flush
      if (self->datalog_flush_idx_ == 0xFF) {
        xSemaphoreGive(self->datalog_mutex_);
        continue;  // Nothing to flush
      }

      uint8_t flush_idx = self->datalog_flush_idx_;
      uint8_t *write_buffer = self->datalog_buffers_[flush_idx];
      size_t write_size = self->datalog_buffer_sizes_[flush_idx];

      if (write_size > 0 && storage::global_storage != nullptr) {
        storage::StorageDevice *device = self->datalog_storage_device_;
        if (device != nullptr && device->is_available()) {
          if (!device->append_file(self->datalog_relative_path_.c_str(), write_buffer, write_size)) {
            ESP_LOGW(TAG, "Async write failed: %s", self->datalog_relative_path_.c_str());
          }
        } else {
          // POSIX fallback
          FILE *f = fopen(self->datalog_filepath_.c_str(), "ab");
          if (f != nullptr) {
            fwrite(write_buffer, 1, write_size, f);
            fclose(f);
          }
        }
        self->datalog_last_flush_ = millis();
      }

      // Mark buffer as free (clear size and flush index)
      self->datalog_buffer_sizes_[flush_idx] = 0;
      self->datalog_flush_idx_ = 0xFF;

      // If there's a queued buffer, move it to flushing
      if (self->datalog_queue_idx_ != 0xFF) {
        self->datalog_flush_idx_ = self->datalog_queue_idx_;
        self->datalog_queue_idx_ = 0xFF;
        // Signal ourselves to process the newly queued buffer
        xSemaphoreGive(self->datalog_write_sem_);
      }

      xSemaphoreGive(self->datalog_mutex_);
    }
  }

  // Task cleanup
  vTaskDelete(nullptr);
}
#endif

void OpenDPS::datalog_request_flush_() {
#if defined(USE_ESP32) && defined(USE_STORAGE)
  // If we have triple-buffering with async task, rotate and signal
  if (this->datalog_buffers_[1] != nullptr && this->datalog_write_sem_ != nullptr && this->datalog_mutex_ != nullptr) {
    // Take mutex to safely modify buffer indices
    if (xSemaphoreTake(this->datalog_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
      return;  // Failed to get mutex, skip this flush cycle
    }

    // Rotate buffers - main loop gets a fresh buffer, current buffer goes to queue
    this->datalog_rotate_buffers_();

    // If nothing is currently flushing, start flushing the queued buffer
    if (this->datalog_flush_idx_ == 0xFF && this->datalog_queue_idx_ != 0xFF) {
      this->datalog_flush_idx_ = this->datalog_queue_idx_;
      this->datalog_queue_idx_ = 0xFF;
      xSemaphoreGive(this->datalog_write_sem_);
    }

    xSemaphoreGive(this->datalog_mutex_);
    return;
  }
#endif
  // Fallback to synchronous flush
  this->datalog_flush_buffer_();
}

void OpenDPS::datalog_flush_buffer_() {
#ifdef USE_STORAGE
  // For synchronous flush, use the current write buffer
  uint8_t *active_buffer = this->datalog_buffers_[this->datalog_write_idx_];
  size_t &buffer_pos = this->datalog_buffer_sizes_[this->datalog_write_idx_];

  if (storage::global_storage == nullptr || buffer_pos == 0 || active_buffer == nullptr) {
    return;
  }

  ESP_LOGD(TAG, "Flushing datalog buffer: %u bytes to %s", buffer_pos, this->datalog_filepath_.c_str());

  // Use cached storage device with relative path
  storage::StorageDevice *device = this->datalog_storage_device_;
  if (device != nullptr && device->is_available()) {
    // Use append_file with relative path for efficient appending
    if (!device->append_file(this->datalog_relative_path_.c_str(), active_buffer, buffer_pos)) {
      ESP_LOGW(TAG, "Failed to append to datalog file: %s", this->datalog_relative_path_.c_str());
    }
  } else {
    // Fall back to POSIX with full path (e.g., for network storage)
    // POSIX append mode
    FILE *f = fopen(this->datalog_filepath_.c_str(), "ab");
    if (f != nullptr) {
      size_t written = fwrite(active_buffer, 1, buffer_pos, f);
      fclose(f);
      if (written != buffer_pos) {
        ESP_LOGW(TAG, "Partial write to datalog file: %u/%u bytes", written, buffer_pos);
      }
    } else {
      ESP_LOGW(TAG, "Failed to open datalog file for append: %s", this->datalog_filepath_.c_str());
    }
  }

  buffer_pos = 0;
  this->datalog_last_flush_ = millis();
#endif
}

bool OpenDPS::start_datalog(const std::string &filename) {
#ifndef USE_STORAGE
  ESP_LOGE(TAG, "Datalogger requires storage component");
  return false;
#else
  if (storage::global_storage == nullptr) {
    ESP_LOGE(TAG, "Storage component not initialized");
    return false;
  }

  if (this->datalog_active_) {
    ESP_LOGW(TAG, "Datalogger already active, stopping previous session");
    this->stop_datalog();
  }

  // Allocate buffer
  if (!this->datalog_ensure_buffer_()) {
    return false;
  }

  // Generate or use provided filename
  if (!filename.empty()) {
    this->datalog_filepath_ = filename;
    // Ensure path is absolute
    if (this->datalog_filepath_[0] != '/') {
      std::string path = this->datalog_config_.storage_path;
      if (!path.empty() && path.back() != '/') {
        path += '/';
      }
      this->datalog_filepath_ = path + this->datalog_filepath_;
    }
  } else {
    this->datalog_filepath_ = this->datalog_generate_filename_();
  }

  // Extract mount point from storage_path (e.g., "/sd" from "/sd/logs")
  // and find the corresponding storage device
  std::string mount_point;
  std::string storage_path = this->datalog_config_.storage_path;

  ESP_LOGV(TAG, "Datalogger: looking for storage device for path: %s", storage_path.c_str());
  ESP_LOGV(TAG, "Datalogger: full filepath will be: %s", this->datalog_filepath_.c_str());

  // Find mount point by checking registered devices
  this->datalog_storage_device_ = nullptr;
  auto all_devices = storage::global_storage->get_all_devices();
  ESP_LOGV(TAG, "Datalogger: found %d storage devices", all_devices.size());

  for (auto *device : all_devices) {
    std::string device_mount = device->get_mount_path();
    bool supports_fs = device->supports_filesystem();
    bool is_avail = device->is_available();
    ESP_LOGV(TAG, "Datalogger: device mount=%s, supports_fs=%d, available=%d", device_mount.c_str(), supports_fs,
             is_avail);

    if (!supports_fs) {
      ESP_LOGW(TAG, "Datalogger: device %s does not support filesystem", device_mount.c_str());
      continue;
    }
    if (!is_avail) {
      ESP_LOGW(TAG, "Datalogger: device %s is not available", device_mount.c_str());
      continue;
    }

    // Check if storage_path starts with this mount point
    if (!device_mount.empty() && storage_path.find(device_mount) == 0) {
      // Found a matching device
      if (device_mount.length() > mount_point.length()) {
        // Prefer longer (more specific) mount paths
        mount_point = device_mount;
        this->datalog_storage_device_ = device;
        ESP_LOGV(TAG, "Datalogger: selected device with mount: %s", mount_point.c_str());
      }
    }
  }

  // Calculate relative path from mount point
  if (this->datalog_storage_device_ != nullptr && !mount_point.empty()) {
    // datalog_filepath_ is like "/sd/logs/file.csv", mount_point is "/sd"
    // relative path should be "logs/file.csv"
    if (this->datalog_filepath_.find(mount_point) == 0) {
      this->datalog_relative_path_ = this->datalog_filepath_.substr(mount_point.length());
      // Remove leading slash if present
      if (!this->datalog_relative_path_.empty() && this->datalog_relative_path_[0] == '/') {
        this->datalog_relative_path_ = this->datalog_relative_path_.substr(1);
      }
    } else {
      this->datalog_relative_path_ = this->datalog_filepath_;
    }
    ESP_LOGD(TAG, "Using storage device at %s, relative path: %s", mount_point.c_str(),
             this->datalog_relative_path_.c_str());
  } else {
    // No device found, will use POSIX fallback
    this->datalog_relative_path_ = this->datalog_filepath_;
    ESP_LOGD(TAG, "No storage device found for %s, using POSIX fallback", storage_path.c_str());
  }

  // Ensure the directory exists
  std::string dir_path = this->datalog_filepath_.substr(0, this->datalog_filepath_.rfind('/'));
  if (!dir_path.empty() && this->datalog_storage_device_ != nullptr) {
    // Calculate relative directory path
    std::string rel_dir = this->datalog_relative_path_;
    size_t last_slash = rel_dir.rfind('/');
    if (last_slash != std::string::npos) {
      rel_dir = rel_dir.substr(0, last_slash);
      if (!this->datalog_storage_device_->dir_exists(rel_dir.c_str())) {
        if (this->datalog_storage_device_->create_dir(rel_dir.c_str())) {
          ESP_LOGD(TAG, "Created directory: %s", rel_dir.c_str());
        } else {
          ESP_LOGW(TAG, "Failed to create directory: %s", rel_dir.c_str());
        }
      }
    }
  }

  // Initialize state
  this->datalog_buffer_sizes_[this->datalog_write_idx_] = 0;
  this->datalog_sample_count_ = 0;
  this->datalog_start_time_ = millis();
  this->datalog_last_flush_ = millis();
  this->datalog_active_ = true;

  // Write CSV header if using CSV format (this also creates the file)
  ESP_LOGV(TAG, "Datalogger: format=%s, writing header", this->datalog_config_.format.c_str());
  if (this->datalog_config_.format == "csv") {
    this->datalog_write_csv_header_();
  } else {
    // For binary format, create empty file first
    if (this->datalog_storage_device_ != nullptr) {
      ESP_LOGV(TAG, "Datalogger: creating empty binary file: %s", this->datalog_relative_path_.c_str());
      this->datalog_storage_device_->write_file(this->datalog_relative_path_.c_str(), nullptr, 0);
    }
  }

  ESP_LOGI(TAG, "Datalogger started: %s (buffer: %u bytes in %s)", this->datalog_filepath_.c_str(),
           this->datalog_buffer_capacity_, this->datalog_buffer_in_psram_ ? "PSRAM" : "heap");

  return true;
#endif
}

void OpenDPS::stop_datalog() {
  if (!this->datalog_active_) {
    return;
  }

#if defined(USE_ESP32) && defined(USE_STORAGE)
  // If using async task, wait for any pending/flushing buffers to complete
  if (this->datalog_write_sem_ != nullptr && (this->datalog_flush_idx_ != 0xFF || this->datalog_queue_idx_ != 0xFF)) {
    // Signal task and wait for it to complete current write
    xSemaphoreGive(this->datalog_write_sem_);
    // Brief delay to let task complete
    vTaskDelay(pdMS_TO_TICKS(100));
  }
#endif

  // Flush remaining buffer (synchronous for final flush)
  this->datalog_flush_buffer_();

  this->datalog_active_ = false;

  ESP_LOGI(TAG, "Datalogger stopped: %u samples logged to %s", this->datalog_sample_count_,
           this->datalog_filepath_.c_str());
}

void OpenDPS::flush_datalog() {
  if (this->datalog_active_) {
    this->datalog_request_flush_();
  }
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

    // Switch UART to correct bootloader baud and save recovery state
    this->firmware_baud_rate_ = this->parent_->get_baud_rate();
    if (this->bootloader_legacy_) {
      // Legacy bootloader: starts at bootloader_baud_rate_ (or current baud if unset)
      uint32_t boot_baud = (this->bootloader_baud_rate_ > 0) ? this->bootloader_baud_rate_ : this->firmware_baud_rate_;
      if (boot_baud != this->firmware_baud_rate_) {
        ESP_LOGI(TAG, "Legacy bootloader: switching UART to %u (was %u)", boot_baud, this->firmware_baud_rate_);
        this->parent_->flush();
        this->parent_->set_baud_rate(boot_baud);
        this->parent_->load_settings();
      }
      // Save boot_baud so setup() can recover to the right baud after reboot
      this->upgrade_state_pref_.save(&boot_baud);
    } else {
      // New bootloader always starts at 9600
      if (this->firmware_baud_rate_ != 9600) {
        ESP_LOGI(TAG, "Switching UART to 9600 for bootloader (was %u)", this->firmware_baud_rate_);
        this->parent_->flush();
        this->parent_->set_baud_rate(9600);
        this->parent_->load_settings();
      }
      // Save flag=1 (new bootloader always recovers at 9600)
      uint32_t flag = 1;
      this->upgrade_state_pref_.save(&flag);
    }
    global_preferences->sync();

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

    // Switch UART to correct bootloader baud and save recovery state
    this->firmware_baud_rate_ = this->parent_->get_baud_rate();
    if (this->bootloader_legacy_) {
      // Legacy bootloader: starts at bootloader_baud_rate_ (or current baud if unset)
      uint32_t boot_baud = (this->bootloader_baud_rate_ > 0) ? this->bootloader_baud_rate_ : this->firmware_baud_rate_;
      if (boot_baud != this->firmware_baud_rate_) {
        ESP_LOGI(TAG, "Legacy bootloader: switching UART to %u (was %u)", boot_baud, this->firmware_baud_rate_);
        this->parent_->flush();
        this->parent_->set_baud_rate(boot_baud);
        this->parent_->load_settings();
      }
      // Save boot_baud so setup() can recover to the right baud after reboot
      this->upgrade_state_pref_.save(&boot_baud);
    } else {
      // New bootloader always starts at 9600
      if (this->firmware_baud_rate_ != 9600) {
        ESP_LOGI(TAG, "Switching UART to 9600 for bootloader (was %u)", this->firmware_baud_rate_);
        this->parent_->flush();
        this->parent_->set_baud_rate(9600);
        this->parent_->load_settings();
      }
      // Save flag=1 (new bootloader always recovers at 9600)
      uint32_t flag = 1;
      this->upgrade_state_pref_.save(&flag);
    }
    global_preferences->sync();

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

  uint32_t now = millis();

  // Handle disconnect grace period - stay in bridge mode briefly to allow reconnection
  // This helps tools like dpsctl.py that may send multiple commands in sequence (e.g., calibration)
  if (this->tcp_client_disconnect_time_ > 0) {
    if (now - this->tcp_client_disconnect_time_ < this->tcp_bridge_disconnect_delay_ms_) {
      // Still in grace period - check for new connection
      struct sockaddr_storage client_addr;
      socklen_t addr_len = sizeof(client_addr);
      auto client = this->tcp_server_socket_->accept((struct sockaddr *) &client_addr, &addr_len);
      if (client != nullptr) {
        ESP_LOGI(TAG, "TCP bridge: client reconnected during grace period");
        client->setblocking(false);
        this->tcp_client_socket_ = std::move(client);
        this->tcp_client_disconnect_time_ = 0;
        // Clear buffers for clean start
        this->tcp_uart_buffer_.clear();
        this->tcp_uart_buffer_start_time_ = 0;
      }
      return;  // Stay in bridge mode during grace period
    } else {
      // Grace period expired - fully exit bridge mode
      ESP_LOGI(TAG, "TCP bridge: grace period expired - resuming normal OpenDPS operation");
      this->tcp_client_disconnect_time_ = 0;
      this->tcp_uart_buffer_.clear();
      this->tcp_uart_buffer_start_time_ = 0;
      return;
    }
  }

  // Accept new connections
  if (this->tcp_client_socket_ == nullptr) {
    struct sockaddr_storage client_addr;
    socklen_t addr_len = sizeof(client_addr);
    auto client = this->tcp_server_socket_->accept((struct sockaddr *) &client_addr, &addr_len);
    if (client != nullptr) {
      ESP_LOGI(TAG, "TCP bridge: client connected - switching to bridge mode (normal OpenDPS queries paused)");
      client->setblocking(false);
      this->tcp_client_socket_ = std::move(client);

      // Clear any partial frame state from normal operation to ensure clean start
      this->rx_buffer_.clear();
      this->receiving_frame_ = false;
      this->tcp_uart_buffer_.clear();
      this->tcp_uart_buffer_start_time_ = 0;

      // Drain any pending UART data that might be from previous operations
      while (this->available()) {
        uint8_t dummy;
        this->read_byte(&dummy);
      }
    }
  }

  // Handle connected client - bridge TCP <-> UART
  if (this->tcp_client_socket_ != nullptr) {
    // Read from TCP, write to UART
    uint8_t tcp_buf[256];
    ssize_t tcp_len = this->tcp_client_socket_->read(tcp_buf, sizeof(tcp_buf));
    if (tcp_len > 0) {
      char hex_buf[format_hex_pretty_size(256)];
      ESP_LOGI(TAG, "TCP->UART: %d bytes: %s", tcp_len, format_hex_pretty_to(hex_buf, tcp_buf, tcp_len));
      this->write_array(tcp_buf, tcp_len);
      this->flush();
    } else if (tcp_len == 0) {
      // Connection closed gracefully - start grace period
      ESP_LOGI(TAG, "TCP bridge: client disconnected - starting %ums grace period",
               this->tcp_bridge_disconnect_delay_ms_);
      this->tcp_client_socket_.reset();
      this->tcp_client_disconnect_time_ = now;
      return;
    } else if (tcp_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      // Error - start grace period
      ESP_LOGI(TAG, "TCP bridge: client disconnected (error %d) - starting %ums grace period", errno,
               this->tcp_bridge_disconnect_delay_ms_);
      this->tcp_client_socket_.reset();
      this->tcp_client_disconnect_time_ = now;
      return;
    }

    // Read from UART, write to TCP - buffer complete frames before sending
    // OpenDPS frames are: SOF (0x7E) ... data ... EOF (0x7F)
    while (this->available()) {
      uint8_t byte;
      this->read_byte(&byte);

      // Start of new frame - track time for timeout
      if (this->tcp_uart_buffer_.empty() && byte == FRAME_SOF) {
        this->tcp_uart_buffer_start_time_ = now;
      }

      this->tcp_uart_buffer_.push_back(byte);

      // Check if we have a complete frame (ends with EOF)
      if (byte == FRAME_EOF && !this->tcp_uart_buffer_.empty() && this->tcp_uart_buffer_[0] == FRAME_SOF) {
        // Send the complete frame
        char hex_buf2[format_hex_pretty_size(256)];
        ESP_LOGI(TAG, "UART->TCP: %d bytes: %s", this->tcp_uart_buffer_.size(),
                 format_hex_pretty_to(hex_buf2, this->tcp_uart_buffer_.data(), this->tcp_uart_buffer_.size()));
        ssize_t written = this->tcp_client_socket_->write(this->tcp_uart_buffer_.data(), this->tcp_uart_buffer_.size());
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGI(TAG, "TCP bridge: write error, disconnecting");
          this->tcp_client_socket_.reset();
          this->tcp_uart_buffer_.clear();
          this->tcp_uart_buffer_start_time_ = 0;
          this->tcp_client_disconnect_time_ = now;
          return;
        }
        this->tcp_uart_buffer_.clear();
        this->tcp_uart_buffer_start_time_ = 0;
      }

      // Safety: prevent buffer from growing too large
      if (this->tcp_uart_buffer_.size() > 1024) {
        ESP_LOGW(TAG, "TCP bridge: UART buffer overflow, clearing");
        this->tcp_uart_buffer_.clear();
        this->tcp_uart_buffer_start_time_ = 0;
      }
    }

    // Check for UART buffer timeout (incomplete frame)
    if (!this->tcp_uart_buffer_.empty() && this->tcp_uart_buffer_start_time_ > 0) {
      if (now - this->tcp_uart_buffer_start_time_ > this->tcp_bridge_frame_timeout_ms_) {
        ESP_LOGW(TAG, "TCP bridge: UART frame timeout (%u bytes), clearing buffer", this->tcp_uart_buffer_.size());
        this->tcp_uart_buffer_.clear();
        this->tcp_uart_buffer_start_time_ = 0;
      }
    }
  }
}

#endif  // USE_SOCKET_IMPL_LWIP_TCP || USE_SOCKET_IMPL_BSD_SOCKETS

}  // namespace opendps
}  // namespace esphome
