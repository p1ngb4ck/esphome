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

      // If a TCP client is connected, skip normal processing (bridge mode active)
      if (this->tcp_client_socket_ != nullptr) {
        return;
      }
    }
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

uint32_t OpenDPS::unpack32_(const std::vector<uint8_t> &frame, size_t &pos) {
  uint32_t value = static_cast<uint32_t>(this->unpack8_(frame, pos)) << 24;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos)) << 16;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos)) << 8;
  value |= static_cast<uint32_t>(this->unpack8_(frame, pos));
  return value;
}

float OpenDPS::unpack_float_(const std::vector<uint8_t> &frame, size_t &pos) {
  // Little-endian float (4 bytes)
  union {
    float f;
    uint8_t bytes[4];
  } float_bytes;
  for (int i = 0; i < 4; i++) {
    float_bytes.bytes[i] = this->unpack8_(frame, pos);
  }
  return float_bytes.f;
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

        // Write sample to datalogger if active
        if (this->datalog_active_) {
          this->datalog_write_sample_();
        }

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

      case CMD_CAL_REPORT: {
        // Parse calibration report response
        // Format: status, vout_adc(16), vin_adc(16), iout_adc(16), iout_dac(16), vout_dac(16),
        //         a_adc_k(f), a_adc_c(f), a_dac_k(f), a_dac_c(f), v_adc_k(f), v_adc_c(f),
        //         v_dac_k(f), v_dac_c(f), vin_adc_k(f), vin_adc_c(f)
        if (status != 0) {
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

  // Store brightness in preferences and record set time
  this->brightness_ = brightness;
  this->brightness_pref_.save(&this->brightness_);
  this->brightness_set_time_ = millis();

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

  // Pack float value as 4 bytes (little-endian, IEEE 754)
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

void OpenDPS::start_calibration_assistant(const CalibrationAssistantParams &params) {
  if (this->cal_assistant_state_ != CAL_IDLE) {
    ESP_LOGW(TAG, "Calibration assistant already running, cancelling previous");
    this->cancel_calibration_assistant();
  }

  this->cal_assistant_params_ = params;
  this->cal_samples_x_.clear();
  this->cal_samples_y_.clear();
  this->cal_sweep_step_ = 0;
  this->cal_max_v_dac_ = 4095;
  this->cal_last_action_time_ = millis();

  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "CALIBRATION ASSISTANT STARTED");
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "Parameters:");
  ESP_LOGI(TAG, "  Vin Measured: %.0f mV", params.vin_measured_mv);
  ESP_LOGI(TAG, "  Load Resistance: %.2f ohm", params.load_resistance);
  ESP_LOGI(TAG, "  Load Max Wattage: %.1f W", params.load_max_wattage);
  ESP_LOGI(TAG, "  Max DPS Current: %.1f A", params.max_dps_current);

  // Start with input voltage calibration
  this->cal_assistant_state_ = CAL_VIN_START;
  this->cal_assistant_process_();
}

void OpenDPS::calibration_assistant_step(float measured_value) {
  ESP_LOGI(TAG, "Calibration step received value: %.3f", measured_value);

  switch (this->cal_assistant_state_) {
    case CAL_VIN_MEASURE:
      // User confirmed input voltage reading - we have ADC value and measured voltage
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vin_adc));
      this->cal_samples_y_.push_back(this->cal_assistant_params_.vin_measured_mv);
      ESP_LOGI(TAG, "Recorded Vin: ADC=%d, measured=%.0f mV", this->calibration_data_.vin_adc,
               this->cal_assistant_params_.vin_measured_mv);
      this->cal_assistant_state_ = CAL_VIN_CALCULATE;
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_MEASURE_LOW:
      // User provided measured output voltage at 10% DAC
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      this->cal_samples_y_.push_back(measured_value);
      ESP_LOGI(TAG, "Recorded Vout low: ADC=%d, measured=%.0f mV, DAC=%d", this->calibration_data_.vout_adc,
               measured_value, static_cast<int>(this->cal_max_v_dac_ * 0.1f));
      this->cal_assistant_state_ = CAL_VOUT_WAIT_HIGH;
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_MEASURE_HIGH:
      // User provided measured output voltage at 90% DAC
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      this->cal_samples_y_.push_back(measured_value);
      ESP_LOGI(TAG, "Recorded Vout high: ADC=%d, measured=%.0f mV, DAC=%d", this->calibration_data_.vout_adc,
               measured_value, static_cast<int>(this->cal_max_v_dac_ * 0.9f));
      this->cal_assistant_state_ = CAL_VOUT_CALCULATE;
      this->cal_assistant_process_();
      break;

    case CAL_IOUT_START:
      // User confirmed load is connected, start current calibration
      this->cal_assistant_state_ = CAL_IOUT_SWEEP;
      this->cal_sweep_step_ = 0;
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_process_();
      break;

    case CAL_ILIMIT_START:
      // User confirmed output is shorted, start current limit calibration
      this->cal_assistant_state_ = CAL_ILIMIT_SWEEP;
      this->cal_sweep_step_ = 0;
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
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

const char *OpenDPS::get_calibration_assistant_prompt() const {
  switch (this->cal_assistant_state_) {
    case CAL_IDLE:
      return "Calibration not started";
    case CAL_VIN_START:
    case CAL_VIN_MEASURE:
      return "Reading input voltage ADC, then call step()";
    case CAL_VIN_CALCULATE:
      return "Calculating input voltage calibration...";
    case CAL_VOUT_START:
    case CAL_VOUT_SWEEP:
      return "Finding maximum V_DAC...";
    case CAL_VOUT_MEASURE_LOW:
      return "Measure output voltage with multimeter (10% point), call step(mV)";
    case CAL_VOUT_WAIT_HIGH:
    case CAL_VOUT_MEASURE_HIGH:
      return "Measure output voltage with multimeter (90% point), call step(mV)";
    case CAL_VOUT_CALCULATE:
      return "Calculating output voltage calibration...";
    case CAL_IOUT_START:
      return "Connect load resistor, then call step()";
    case CAL_IOUT_SWEEP:
      return "Measuring output current at various voltages...";
    case CAL_IOUT_CALCULATE:
      return "Calculating output current calibration...";
    case CAL_ILIMIT_START:
      return "SHORT the output with thick wire, then call step()";
    case CAL_ILIMIT_SWEEP:
      return "Sweeping A_DAC range...";
    case CAL_ILIMIT_MEASURE:
      return "Measuring current limit DAC points...";
    case CAL_ILIMIT_CALCULATE:
      return "Calculating current limit calibration...";
    case CAL_COMPLETE:
      return "Calibration complete!";
    case CAL_ERROR:
      return "Calibration error occurred";
    default:
      return "Unknown state";
  }
}

void OpenDPS::cal_assistant_process_() {
  ESP_LOGI(TAG, "Processing calibration state: %d", this->cal_assistant_state_);

  switch (this->cal_assistant_state_) {
    case CAL_VIN_START:
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 1: Input Voltage Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "Using measured input voltage: %.0f mV", this->cal_assistant_params_.vin_measured_mv);
      ESP_LOGI(TAG, "Reading ADC value from DPS...");
      ESP_LOGI(TAG, "Then call: opendps.calibration_assistant_step with value 0");
      this->request_calibration_report();
      this->cal_assistant_state_ = CAL_VIN_MEASURE;
      break;

    case CAL_VIN_CALCULATE: {
      // Single-point calibration using the measured voltage and ADC reading
      // We assume the ADC is linear and passes through (0, 0), so we calculate:
      // VIN_ADC_K = measured_mV / adc_reading
      // VIN_ADC_C = 0 (assuming linear ADC with zero offset)
      float adc_reading = this->cal_samples_x_[0];
      float measured_mv = this->cal_samples_y_[0];

      if (adc_reading <= 0) {
        ESP_LOGE(TAG, "Invalid ADC reading: %.0f - cannot calibrate", adc_reading);
        this->cal_assistant_state_ = CAL_ERROR;
        this->cal_assistant_process_();
        break;
      }

      float k = measured_mv / adc_reading;
      float c = 0.0f;  // Assuming linear through origin

      ESP_LOGI(TAG, "Input voltage calibration (single-point):");
      ESP_LOGI(TAG, "  ADC reading: %.0f, Measured: %.0f mV", adc_reading, measured_mv);
      ESP_LOGI(TAG, "  VIN_ADC_K=%.6f, VIN_ADC_C=%.6f", k, c);
      this->set_calibration("VIN_ADC_K", k);
      this->set_calibration("VIN_ADC_C", c);

      // Move to output voltage calibration
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_VOUT_START;
      this->cal_assistant_process_();
      break;
    }

    case CAL_VOUT_START:
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 2: Output Voltage Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "Finding maximum V_DAC value...");

      // Set up for sweep: V_DAC=0, A_DAC=4095 (max current limit)
      this->set_parameter("V_DAC", "0");
      this->set_parameter("A_DAC", "4095");
      this->enable_output(true);
      this->cal_sweep_step_ = 0;
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_last_action_time_ = millis();
      this->cal_assistant_state_ = CAL_VOUT_SWEEP;
      break;

    case CAL_VOUT_SWEEP: {
      // Sweep V_DAC from 0 to 4095 in 100 steps
      if (this->cal_sweep_step_ <= 100) {
        uint16_t v_dac = (this->cal_sweep_step_ * 4095) / 100;
        this->set_parameter("V_DAC", std::to_string(v_dac));
        this->request_calibration_report();

        // Store the DAC value and wait for ADC reading
        this->cal_samples_x_.push_back(static_cast<float>(v_dac));
        this->cal_sweep_step_++;
        this->cal_last_action_time_ = millis();

        if (this->cal_sweep_step_ % 20 == 0) {
          ESP_LOGI(TAG, "V_DAC sweep: %d%%", this->cal_sweep_step_);
        }
      } else {
        // Analyze sweep results to find max linear V_DAC
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

        // Now measure at 10% and 90% of max
        this->cal_samples_x_.clear();
        this->cal_samples_y_.clear();

        uint16_t v_dac_low = static_cast<uint16_t>(this->cal_max_v_dac_ * 0.1f);
        this->set_parameter("V_DAC", std::to_string(v_dac_low));
        this->cal_samples_x_.push_back(static_cast<float>(v_dac_low));  // Store DAC value for V_DAC calibration
        this->request_calibration_report();

        ESP_LOGI(TAG, "Set V_DAC to 10%% (%d)", v_dac_low);
        ESP_LOGI(TAG, "Measure the OUTPUT VOLTAGE with a multimeter (in mV)");
        ESP_LOGI(TAG, "Then call: opendps.calibration_assistant_step with the measured value");
        this->cal_assistant_state_ = CAL_VOUT_MEASURE_LOW;
      }
      break;
    }

    case CAL_VOUT_WAIT_HIGH: {
      uint16_t v_dac_high = static_cast<uint16_t>(this->cal_max_v_dac_ * 0.9f);
      this->set_parameter("V_DAC", std::to_string(v_dac_high));
      this->cal_samples_x_.push_back(static_cast<float>(v_dac_high));  // Store DAC value
      this->request_calibration_report();

      ESP_LOGI(TAG, "Set V_DAC to 90%% (%d)", v_dac_high);
      ESP_LOGI(TAG, "Measure the OUTPUT VOLTAGE with a multimeter (in mV)");
      ESP_LOGI(TAG, "Then call: opendps.calibration_assistant_step with the measured value");
      this->cal_assistant_state_ = CAL_VOUT_MEASURE_HIGH;
      break;
    }

    case CAL_VOUT_CALCULATE: {
      this->enable_output(false);

      // We have: DAC values in cal_samples_x_[0,2] and measured voltages in cal_samples_y_[0,1]
      // ADC values in cal_samples_x_[1,3] (stored when we recorded the measurements)
      // Actually we need to track this better - let me fix the data collection

      // For V_DAC: measured_voltage -> DAC  (we want to set voltage, get DAC)
      // For V_ADC: ADC -> measured_voltage (we want to read ADC, get voltage)

      std::vector<float> dac_values = {this->cal_samples_x_[0], this->cal_samples_x_[2]};  // DAC values
      std::vector<float> adc_values = {this->cal_samples_x_[1], this->cal_samples_x_[3]};  // ADC values
      std::vector<float> measured = {this->cal_samples_y_[0], this->cal_samples_y_[1]};    // Measured voltages

      // V_DAC: measured_mV -> DAC value
      auto [v_dac_k, v_dac_c] = this->cal_best_fit_(measured, dac_values);
      this->cal_v_dac_k_ = v_dac_k;
      this->cal_v_dac_c_ = v_dac_c;
      ESP_LOGI(TAG, "Output voltage DAC calibration: V_DAC_K=%.6f, V_DAC_C=%.6f", v_dac_k, v_dac_c);
      this->set_calibration("V_DAC_K", v_dac_k);
      this->set_calibration("V_DAC_C", v_dac_c);

      // V_ADC: ADC -> measured_mV
      auto [v_adc_k, v_adc_c] = this->cal_best_fit_(adc_values, measured);
      this->cal_v_adc_k_ = v_adc_k;
      this->cal_v_adc_c_ = v_adc_c;
      ESP_LOGI(TAG, "Output voltage ADC calibration: V_ADC_K=%.6f, V_ADC_C=%.6f", v_adc_k, v_adc_c);
      this->set_calibration("V_ADC_K", v_adc_k);
      this->set_calibration("V_ADC_C", v_adc_c);

      // Check if we should do current calibration
      if (this->cal_assistant_params_.load_resistance > 0) {
        this->cal_samples_x_.clear();
        this->cal_samples_y_.clear();
        this->cal_assistant_state_ = CAL_IOUT_START;
        ESP_LOGI(TAG, "----------------------------------------");
        ESP_LOGI(TAG, "STEP 3: Output Current Calibration");
        ESP_LOGI(TAG, "----------------------------------------");
        ESP_LOGI(TAG, "Connect a %.2f ohm load to the output", this->cal_assistant_params_.load_resistance);
        ESP_LOGI(TAG, "Then call: opendps.calibration_assistant_step with value 0");
      } else {
        ESP_LOGI(TAG, "Skipping current calibration (no load specified)");
        this->cal_assistant_state_ = CAL_COMPLETE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_IOUT_SWEEP: {
      // Calculate max safe voltage based on constraints
      float v_max_input = this->cal_assistant_params_.vin_measured_mv * 0.9f;
      float v_max_wattage =
          sqrtf(this->cal_assistant_params_.load_max_wattage * this->cal_assistant_params_.load_resistance) * 1000.0f;
      float v_max_current =
          this->cal_assistant_params_.max_dps_current * this->cal_assistant_params_.load_resistance * 1000.0f;
      float max_output_mv = std::min({v_max_input, v_max_wattage, v_max_current});

      ESP_LOGI(TAG, "Max safe output voltage: %.0f mV", max_output_mv);

      // Sweep output voltage and measure current
      uint8_t num_steps = 15;
      if (this->cal_sweep_step_ < num_steps) {
        float output_voltage = max_output_mv * (static_cast<float>(this->cal_sweep_step_) / num_steps);
        uint16_t output_dac = static_cast<uint16_t>(this->cal_v_dac_k_ * output_voltage + this->cal_v_dac_c_);

        this->set_parameter("V_DAC", std::to_string(output_dac));
        this->enable_output(true);
        this->request_calibration_report();

        this->cal_sweep_step_++;
        ESP_LOGI(TAG, "Current calibration sweep: %d/%d (V_DAC=%d)", this->cal_sweep_step_, num_steps, output_dac);
      } else {
        this->cal_assistant_state_ = CAL_IOUT_CALCULATE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_IOUT_CALCULATE: {
      this->enable_output(false);

      // Calculate A_ADC calibration
      auto [a_adc_k, a_adc_c] = this->cal_best_fit_(this->cal_samples_x_, this->cal_samples_y_);
      this->cal_a_adc_k_ = a_adc_k;
      this->cal_a_adc_c_ = a_adc_c;
      ESP_LOGI(TAG, "Output current ADC calibration: A_ADC_K=%.6f, A_ADC_C=%.6f", a_adc_k, a_adc_c);
      this->set_calibration("A_ADC_K", a_adc_k);
      this->set_calibration("A_ADC_C", a_adc_c);

      // Move to current limit calibration
      this->cal_samples_x_.clear();
      this->cal_samples_y_.clear();
      this->cal_assistant_state_ = CAL_ILIMIT_START;
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "STEP 4: Current Limit Calibration");
      ESP_LOGI(TAG, "----------------------------------------");
      ESP_LOGI(TAG, "SHORT the output with a thick wire capable of %.1f A",
               this->cal_assistant_params_.max_dps_current);
      ESP_LOGI(TAG, "Then call: opendps.calibration_assistant_step with value 0");
      break;
    }

    case CAL_ILIMIT_SWEEP: {
      // Set V_DAC to max
      this->set_parameter("V_DAC", "4095");

      // Sweep A_DAC to find workable range
      uint8_t num_steps = 100;
      if (this->cal_sweep_step_ <= num_steps) {
        uint16_t a_dac = (this->cal_sweep_step_ * 4095) / num_steps;
        this->set_parameter("A_DAC", std::to_string(a_dac));
        this->enable_output(true);
        this->request_calibration_report();

        this->cal_samples_x_.push_back(static_cast<float>(a_dac));
        this->cal_sweep_step_++;

        if (this->cal_sweep_step_ % 20 == 0) {
          ESP_LOGI(TAG, "A_DAC sweep: %d%%", this->cal_sweep_step_);
        }
      } else {
        this->enable_output(false);

        // Analyze sweep to find linear range
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

        // Now measure current at points in this range
        this->cal_samples_x_.clear();
        this->cal_samples_y_.clear();
        this->cal_sweep_step_ = 0;
        this->cal_assistant_state_ = CAL_ILIMIT_MEASURE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_ILIMIT_MEASURE: {
      uint8_t num_steps = 15;
      if (this->cal_sweep_step_ < num_steps) {
        float ratio = static_cast<float>(this->cal_sweep_step_) / num_steps;
        uint16_t a_dac =
            this->cal_a_dac_lower_ + static_cast<uint16_t>((this->cal_a_dac_upper_ - this->cal_a_dac_lower_) * ratio);

        this->set_parameter("A_DAC", std::to_string(a_dac));
        this->enable_output(true);
        this->request_calibration_report();

        // Calculate current from ADC reading using previously calibrated A_ADC
        float i_out = this->calibration_data_.iout_adc * this->cal_a_adc_k_ + this->cal_a_adc_c_;
        this->cal_samples_x_.push_back(i_out);
        this->cal_samples_y_.push_back(static_cast<float>(a_dac));

        this->cal_sweep_step_++;
        ESP_LOGI(TAG, "Current limit calibration: %d/%d (A_DAC=%d, I=%.3f)", this->cal_sweep_step_, num_steps, a_dac,
                 i_out);
      } else {
        this->cal_assistant_state_ = CAL_ILIMIT_CALCULATE;
        this->cal_assistant_process_();
      }
      break;
    }

    case CAL_ILIMIT_CALCULATE: {
      this->enable_output(false);

      // Calculate A_DAC calibration: current -> DAC
      auto [a_dac_k, a_dac_c] = this->cal_best_fit_(this->cal_samples_x_, this->cal_samples_y_);
      ESP_LOGI(TAG, "Current limit DAC calibration: A_DAC_K=%.6f, A_DAC_C=%.6f", a_dac_k, a_dac_c);
      this->set_calibration("A_DAC_K", a_dac_k);
      this->set_calibration("A_DAC_C", a_dac_c);

      this->cal_assistant_state_ = CAL_COMPLETE;
      this->cal_assistant_process_();
      break;
    }

    case CAL_COMPLETE:
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "CALIBRATION COMPLETE!");
      ESP_LOGI(TAG, "========================================");
      ESP_LOGI(TAG, "To reset to defaults, use: opendps.clear_calibration");
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
  // Store the relevant ADC value based on current state
  switch (this->cal_assistant_state_) {
    case CAL_VOUT_SWEEP:
      // Store vout_adc for sweep analysis
      this->cal_samples_y_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      // Continue sweep
      this->cal_assistant_process_();
      break;

    case CAL_VOUT_MEASURE_LOW:
    case CAL_VOUT_MEASURE_HIGH:
      // Store ADC value - the measured voltage will be provided by user
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.vout_adc));
      break;

    case CAL_IOUT_SWEEP: {
      // Calculate current from voltage across known resistance
      float v_out = this->calibration_data_.vout_adc * this->cal_v_adc_k_ + this->cal_v_adc_c_;
      float i_out = v_out / this->cal_assistant_params_.load_resistance;
      this->cal_samples_x_.push_back(static_cast<float>(this->calibration_data_.iout_adc));
      this->cal_samples_y_.push_back(i_out);
      this->cal_assistant_process_();
      break;
    }

    case CAL_ILIMIT_SWEEP:
      // Store iout_adc for sweep analysis
      this->cal_samples_y_.push_back(static_cast<float>(this->calibration_data_.iout_adc));
      this->cal_assistant_process_();
      break;

    default:
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
  if (this->datalog_buffer_a_ != nullptr) {
    return true;  // Already allocated
  }

  size_t requested_size = this->datalog_config_.buffer_size;
  if (requested_size == 0) {
    requested_size = 65536;  // Default 64KB
  }

  // We need two buffers for double-buffering
  size_t total_size = requested_size * 2;

#ifdef USE_ESP32
  // Try to allocate in PSRAM first
  if (esp_psram_is_initialized()) {
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram >= total_size + 8192) {  // Leave some margin
      this->datalog_buffer_a_ = static_cast<uint8_t *>(heap_caps_malloc(requested_size, MALLOC_CAP_SPIRAM));
      this->datalog_buffer_b_ = static_cast<uint8_t *>(heap_caps_malloc(requested_size, MALLOC_CAP_SPIRAM));
      if (this->datalog_buffer_a_ != nullptr && this->datalog_buffer_b_ != nullptr) {
        this->datalog_buffer_size_ = requested_size;
        this->datalog_buffer_in_psram_ = true;

        // Create FreeRTOS synchronization primitives
        this->datalog_mutex_ = xSemaphoreCreateMutex();
        this->datalog_write_sem_ = xSemaphoreCreateBinary();

        if (this->datalog_mutex_ == nullptr || this->datalog_write_sem_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create FreeRTOS semaphores");
          heap_caps_free(this->datalog_buffer_a_);
          heap_caps_free(this->datalog_buffer_b_);
          this->datalog_buffer_a_ = nullptr;
          this->datalog_buffer_b_ = nullptr;
          return false;
        }

        // Create background write task with lower priority than main loop
        this->datalog_task_running_ = true;
        BaseType_t result = xTaskCreatePinnedToCore(datalog_write_task_, "datalog_write", 4096, this,
                                                    1,  // Priority 1 (low, main loop is higher)
                                                    &this->datalog_task_handle_,
                                                    1  // Run on core 1 (app core)
        );
        if (result != pdPASS) {
          ESP_LOGE(TAG, "Failed to create datalog write task");
          vSemaphoreDelete(this->datalog_mutex_);
          vSemaphoreDelete(this->datalog_write_sem_);
          heap_caps_free(this->datalog_buffer_a_);
          heap_caps_free(this->datalog_buffer_b_);
          this->datalog_buffer_a_ = nullptr;
          this->datalog_buffer_b_ = nullptr;
          this->datalog_mutex_ = nullptr;
          this->datalog_write_sem_ = nullptr;
          return false;
        }

        ESP_LOGI(TAG, "Datalogger double-buffer allocated in PSRAM: 2x%u bytes, async task started", requested_size);
        return true;
      }
      // Cleanup partial allocation
      if (this->datalog_buffer_a_ != nullptr)
        heap_caps_free(this->datalog_buffer_a_);
      if (this->datalog_buffer_b_ != nullptr)
        heap_caps_free(this->datalog_buffer_b_);
      this->datalog_buffer_a_ = nullptr;
      this->datalog_buffer_b_ = nullptr;
    }
    ESP_LOGW(TAG, "PSRAM allocation failed, falling back to regular heap (synchronous writes)");
  }

  // Fall back to regular heap with single smaller buffer (synchronous writes)
  size_t heap_size = std::min(requested_size, static_cast<size_t>(8192));  // Max 8KB in regular heap
  this->datalog_buffer_a_ = static_cast<uint8_t *>(malloc(heap_size));
  if (this->datalog_buffer_a_ != nullptr) {
    this->datalog_buffer_size_ = heap_size;
    this->datalog_buffer_in_psram_ = false;
    this->datalog_buffer_b_ = nullptr;  // No double-buffering without PSRAM
    ESP_LOGI(TAG, "Datalogger single-buffer allocated in heap: %u bytes (synchronous writes)", heap_size);
    return true;
  }
#else
  // Non-ESP32: single buffer, synchronous writes
  size_t heap_size = std::min(requested_size, static_cast<size_t>(8192));
  this->datalog_buffer_a_ = static_cast<uint8_t *>(malloc(heap_size));
  if (this->datalog_buffer_a_ != nullptr) {
    this->datalog_buffer_size_ = heap_size;
    this->datalog_buffer_in_psram_ = false;
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

  if (cols & DATALOG_COL_TIMESTAMP)
    header += "timestamp_ms";
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
  if (device != nullptr && device->is_available()) {
    // Use write_file to create file with header
    if (!device->write_file(this->datalog_relative_path_.c_str(), reinterpret_cast<const uint8_t *>(header.data()),
                            header.size())) {
      ESP_LOGW(TAG, "Failed to write CSV header to %s", this->datalog_filepath_.c_str());
      return;
    }
  } else {
    // Fall back to global storage POSIX methods
    if (!storage::global_storage->write_file(this->datalog_filepath_, header)) {
      ESP_LOGW(TAG, "Failed to write CSV header via POSIX to %s", this->datalog_filepath_.c_str());
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

  uint32_t timestamp = millis() - this->datalog_start_time_;

  if (cols & DATALOG_COL_TIMESTAMP) {
    pos += snprintf(buffer + pos, max_len - pos, "%lu", (unsigned long) timestamp);
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
  // Header byte (0xAA) + timestamp(4) + vin(4) + vout(4) + iout(4) + power(4) + enabled(1) + temp1(4) + temp2(4)
  // Total: 30 bytes per sample (all columns)

  if (max_len < 30) {
    return 0;
  }

  size_t pos = 0;
  uint16_t cols = this->datalog_config_.columns;

  // Start marker
  buffer[pos++] = 0xAA;

  // Column presence flags (1 byte)
  buffer[pos++] = static_cast<uint8_t>(cols & 0xFF);

  if (cols & DATALOG_COL_TIMESTAMP) {
    uint32_t timestamp = millis() - this->datalog_start_time_;
    memcpy(buffer + pos, &timestamp, 4);
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
  if (!this->datalog_active_ || this->datalog_buffer_a_ == nullptr) {
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

  // Get the active buffer (buffer_b may be nullptr on non-ESP32 or without PSRAM)
  uint8_t *active_buffer = this->datalog_buffer_a_;
  if (!this->datalog_use_buffer_a_ && this->datalog_buffer_b_ != nullptr) {
    active_buffer = this->datalog_buffer_b_;
  }

  // Check if we need to swap/flush first
  if (this->datalog_buffer_pos_ + row_len > this->datalog_buffer_size_) {
    this->datalog_request_flush_();
  }

  // Copy row to active buffer
  if (this->datalog_buffer_pos_ + row_len <= this->datalog_buffer_size_) {
    memcpy(active_buffer + this->datalog_buffer_pos_, row_buffer, row_len);
    this->datalog_buffer_pos_ += row_len;
    this->datalog_sample_count_++;
  }

  // Auto-flush if buffer is 75% full or flush interval elapsed
  uint32_t now = millis();
  bool should_flush = false;

  if (this->datalog_buffer_pos_ > (this->datalog_buffer_size_ * 3 / 4)) {
    should_flush = true;
  }

  if (this->datalog_config_.flush_interval_ms > 0 &&
      (now - this->datalog_last_flush_) >= this->datalog_config_.flush_interval_ms) {
    should_flush = true;
  }

  if (should_flush && this->datalog_buffer_pos_ > 0) {
    this->datalog_request_flush_();
  }
}

#if defined(USE_ESP32) && defined(USE_STORAGE)
void OpenDPS::datalog_swap_buffers_() {
  // Swap active buffer (called with mutex held or from main thread before task signal)
  this->datalog_pending_size_ = this->datalog_buffer_pos_;
  this->datalog_buffer_pos_ = 0;
  this->datalog_use_buffer_a_ = !this->datalog_use_buffer_a_;
}

void OpenDPS::datalog_write_task_(void *arg) {
  OpenDPS *self = static_cast<OpenDPS *>(arg);

  while (self->datalog_task_running_) {
    // Wait for signal to write (with timeout for graceful shutdown)
    if (xSemaphoreTake(self->datalog_write_sem_, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!self->datalog_task_running_) {
        break;
      }

      // Get the buffer that needs writing (the one we just swapped away from)
      uint8_t *write_buffer = self->datalog_use_buffer_a_ ? self->datalog_buffer_b_ : self->datalog_buffer_a_;
      size_t write_size = self->datalog_pending_size_;

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
        self->datalog_pending_size_ = 0;
        self->datalog_last_flush_ = millis();
      }
    }
  }

  // Task cleanup
  vTaskDelete(nullptr);
}
#endif

void OpenDPS::datalog_request_flush_() {
#if defined(USE_ESP32) && defined(USE_STORAGE)
  // If we have double-buffering with async task, swap and signal
  if (this->datalog_buffer_b_ != nullptr && this->datalog_write_sem_ != nullptr) {
    // Only swap if pending buffer is empty (previous write completed)
    if (this->datalog_pending_size_ == 0) {
      this->datalog_swap_buffers_();
      xSemaphoreGive(this->datalog_write_sem_);
    } else {
      // Previous write still pending - drop samples or wait?
      // For now, just log a warning (data will accumulate until next flush succeeds)
      ESP_LOGW(TAG, "Datalog write still pending, skipping flush request");
    }
    return;
  }
#endif
  // Fallback to synchronous flush
  this->datalog_flush_buffer_();
}

void OpenDPS::datalog_flush_buffer_() {
#ifdef USE_STORAGE
  if (storage::global_storage == nullptr || this->datalog_buffer_pos_ == 0) {
    return;
  }

  // Get the active buffer for synchronous flush
  uint8_t *active_buffer = this->datalog_use_buffer_a_ ? this->datalog_buffer_a_ : this->datalog_buffer_b_;
  if (active_buffer == nullptr) {
    active_buffer = this->datalog_buffer_a_;  // Fallback for single-buffer mode
  }

  ESP_LOGD(TAG, "Flushing datalog buffer: %u bytes to %s", this->datalog_buffer_pos_, this->datalog_filepath_.c_str());

  // Use cached storage device with relative path
  storage::StorageDevice *device = this->datalog_storage_device_;
  if (device != nullptr && device->is_available()) {
    // Use append_file with relative path for efficient appending
    if (!device->append_file(this->datalog_relative_path_.c_str(), active_buffer, this->datalog_buffer_pos_)) {
      ESP_LOGW(TAG, "Failed to append to datalog file: %s", this->datalog_relative_path_.c_str());
    }
  } else {
    // Fall back to POSIX with full path (e.g., for network storage)
    // POSIX append mode
    FILE *f = fopen(this->datalog_filepath_.c_str(), "ab");
    if (f != nullptr) {
      size_t written = fwrite(active_buffer, 1, this->datalog_buffer_pos_, f);
      fclose(f);
      if (written != this->datalog_buffer_pos_) {
        ESP_LOGW(TAG, "Partial write to datalog file: %u/%u bytes", written, this->datalog_buffer_pos_);
      }
    } else {
      ESP_LOGW(TAG, "Failed to open datalog file for append: %s", this->datalog_filepath_.c_str());
    }
  }

  this->datalog_buffer_pos_ = 0;
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

  // Find mount point by checking registered devices
  this->datalog_storage_device_ = nullptr;
  for (auto *device : storage::global_storage->get_all_devices()) {
    if (device->supports_filesystem() && device->is_available()) {
      std::string device_mount = device->get_mount_path();
      // Check if storage_path starts with this mount point
      if (!device_mount.empty() && storage_path.find(device_mount) == 0) {
        // Found a matching device
        if (device_mount.length() > mount_point.length()) {
          // Prefer longer (more specific) mount paths
          mount_point = device_mount;
          this->datalog_storage_device_ = device;
        }
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
  this->datalog_buffer_pos_ = 0;
  this->datalog_sample_count_ = 0;
  this->datalog_start_time_ = millis();
  this->datalog_last_flush_ = millis();
  this->datalog_active_ = true;

  // Write CSV header if using CSV format (this also creates the file)
  if (this->datalog_config_.format == "csv") {
    this->datalog_write_csv_header_();
  } else {
    // For binary format, create empty file first
    if (this->datalog_storage_device_ != nullptr) {
      this->datalog_storage_device_->write_file(this->datalog_relative_path_.c_str(), nullptr, 0);
    }
  }

  ESP_LOGI(TAG, "Datalogger started: %s (buffer: %u bytes in %s)", this->datalog_filepath_.c_str(),
           this->datalog_buffer_size_, this->datalog_buffer_in_psram_ ? "PSRAM" : "heap");

  return true;
#endif
}

void OpenDPS::stop_datalog() {
  if (!this->datalog_active_) {
    return;
  }

#if defined(USE_ESP32) && defined(USE_STORAGE)
  // If using async task, wait for pending write to complete first
  if (this->datalog_write_sem_ != nullptr && this->datalog_pending_size_ > 0) {
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
      ESP_LOGI(TAG, "TCP bridge: client connected - switching to bridge mode (normal OpenDPS queries paused)");
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
      ESP_LOGI(TAG, "TCP->UART: %d bytes: %s", tcp_len, format_hex_pretty(tcp_buf, tcp_len).c_str());
      this->write_array(tcp_buf, tcp_len);
      this->flush();
    } else if (tcp_len == 0) {
      // Connection closed gracefully
      ESP_LOGI(TAG, "TCP bridge: client disconnected (connection closed) - resuming normal OpenDPS operation");
      this->tcp_client_socket_.reset();
      return;
    } else if (tcp_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      // Error
      ESP_LOGI(TAG, "TCP bridge: client disconnected (error %d) - resuming normal OpenDPS operation", errno);
      this->tcp_client_socket_.reset();
      return;
    }

    // Read from UART, write to TCP - buffer complete frames before sending
    // OpenDPS frames are: SOF (0x7E) ... data ... EOF (0x7F)
    while (this->available()) {
      uint8_t byte;
      this->read_byte(&byte);
      this->tcp_uart_buffer_.push_back(byte);

      // Check if we have a complete frame (ends with EOF)
      if (byte == FRAME_EOF && !this->tcp_uart_buffer_.empty() && this->tcp_uart_buffer_[0] == FRAME_SOF) {
        // Send the complete frame
        ESP_LOGI(TAG, "UART->TCP: %d bytes: %s", this->tcp_uart_buffer_.size(),
                 format_hex_pretty(this->tcp_uart_buffer_.data(), this->tcp_uart_buffer_.size()).c_str());
        ssize_t written = this->tcp_client_socket_->write(this->tcp_uart_buffer_.data(), this->tcp_uart_buffer_.size());
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          ESP_LOGI(TAG, "TCP bridge: write error, disconnecting");
          this->tcp_client_socket_.reset();
          this->tcp_uart_buffer_.clear();
          return;
        }
        this->tcp_uart_buffer_.clear();
      }

      // Safety: prevent buffer from growing too large
      if (this->tcp_uart_buffer_.size() > 1024) {
        ESP_LOGW(TAG, "TCP bridge: UART buffer overflow, clearing");
        this->tcp_uart_buffer_.clear();
      }
    }
  }
}

#endif  // USE_SOCKET_IMPL_LWIP_TCP || USE_SOCKET_IMPL_BSD_SOCKETS

}  // namespace opendps
}  // namespace esphome
