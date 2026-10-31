/**************************************************************************/
/*!
  Author: Gustavo Ambrozio
  Based on work by: Atsushi Sasaki (https://github.com/aselectroworks/Arduino-FT6336U)
*/
/**************************************************************************/

#include "ft63x6.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

// Registers
// Reference: https://focuslcds.com/content/FT6236.pdf

namespace esphome::ft63x6 {
static const uint8_t FT6X36_ADDR_DEVICE_MODE = 0x00;

static const uint8_t FT63X6_ADDR_TD_STATUS = 0x02;
static const uint8_t FT63X6_ADDR_TOUCH1_STATE = 0x03;
static const uint8_t FT63X6_ADDR_TOUCH1_X = 0x03;
static const uint8_t FT63X6_ADDR_TOUCH1_ID = 0x05;
static const uint8_t FT63X6_ADDR_TOUCH1_Y = 0x05;
static const uint8_t FT63X6_ADDR_TOUCH1_WEIGHT = 0x07;
static const uint8_t FT63X6_ADDR_TOUCH1_MISC = 0x08;
static const uint8_t FT6X36_ADDR_THRESHHOLD = 0x80;
static const uint8_t FT6X36_ADDR_TOUCHRATE_ACTIVE = 0x88;
static const uint8_t FT63X6_ADDR_CHIP_ID = 0xA3;

ESPHOME_LOG_TAG(TAG, "FT63X6");

void FT63X6Touchscreen::setup() {
  if (this->interrupt_pin_ != nullptr) {
    this->interrupt_pin_->pin_mode(gpio::FLAG_INPUT | gpio::FLAG_PULLUP);
    this->interrupt_pin_->setup();
    this->attach_interrupt_(this->interrupt_pin_, gpio::INTERRUPT_ANY_EDGE);
  }

  pulse_reset(this->reset_pin_, {.low_ms = 10});

  // Get touch resolution
  if (this->x_raw_max_ == this->x_raw_min_) {
    this->x_raw_max_ = this->display_->get_native_width();
  }
  if (this->y_raw_max_ == this->y_raw_min_) {
    this->y_raw_max_ = this->display_->get_native_height();
  }
  uint8_t chip_id = this->read_byte_(FT63X6_ADDR_CHIP_ID);
  if (chip_id != 0) {
    ESP_LOGI(TAG, "FT6336U touch driver started chipid: %d", chip_id);
  } else {
    ESP_LOGE(TAG, "FT6336U touch driver failed to start");
  }
  this->write_byte(FT6X36_ADDR_DEVICE_MODE, 0x00);
  this->write_byte(FT6X36_ADDR_THRESHHOLD, this->threshold_);
  this->write_byte(FT6X36_ADDR_TOUCHRATE_ACTIVE, 0x0E);
}

void FT63X6Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "FT63X6 Touchscreen:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Interrupt Pin: ", this->interrupt_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  ESP_LOGCONFIG(TAG,
                "  X Calibration: [%d, %d]\n"
                "  Y Calibration: [%d, %d]",
                this->x_raw_min_, this->x_raw_max_, this->y_raw_min_, this->y_raw_max_);
  LOG_UPDATE_INTERVAL(this);
}

void FT63X6Touchscreen::update_touches() {
  uint16_t touch_id, x, y;

  uint8_t touches = this->read_touch_number_();
  ESP_LOGV(TAG, "Touches found: %d", touches);
  if ((touches == 0x00) || (touches == 0xff)) {
    // ESP_LOGD(TAG, "No touches detected");
    return;
  }

  for (auto point = 0; point < touches; point++) {
    if (((this->read_touch_event_(point)) & 0x01) == 0) {  // checking event flag bit 6 if it is null
      touch_id = this->read_touch_id_(point);              // id1 = 0 or 1
      x = this->read_touch_x_(point);
      y = this->read_touch_y_(point);
      if ((x == 0) && (y == 0)) {
        ESP_LOGW(TAG, "Reporting a (0,0) touch on %d", touch_id);
      }
      this->add_raw_touch_position_(touch_id, x, y, this->read_touch_weight_(point));
    }
  }
}

uint8_t FT63X6Touchscreen::read_touch_number_() { return this->read_byte_(FT63X6_ADDR_TD_STATUS) & 0x0F; }
// Touch 1 functions
uint16_t FT63X6Touchscreen::read_touch_x_(uint8_t touch) {
  uint8_t read_buf[2];
  read_buf[0] = this->read_byte_(FT63X6_ADDR_TOUCH1_X + (touch * 6));
  read_buf[1] = this->read_byte_(FT63X6_ADDR_TOUCH1_X + 1 + (touch * 6));
  return encode_uint16(read_buf[0] & 0x0f, read_buf[1]);
}
uint16_t FT63X6Touchscreen::read_touch_y_(uint8_t touch) {
  uint8_t read_buf[2];
  read_buf[0] = this->read_byte_(FT63X6_ADDR_TOUCH1_Y + (touch * 6));
  read_buf[1] = this->read_byte_(FT63X6_ADDR_TOUCH1_Y + 1 + (touch * 6));
  return encode_uint16(read_buf[0] & 0x0f, read_buf[1]);
}
uint8_t FT63X6Touchscreen::read_touch_event_(uint8_t touch) {
  return this->read_byte_(FT63X6_ADDR_TOUCH1_X + (touch * 6)) >> 6;
}
uint8_t FT63X6Touchscreen::read_touch_id_(uint8_t touch) {
  return this->read_byte_(FT63X6_ADDR_TOUCH1_ID + (touch * 6)) >> 4;
}
uint8_t FT63X6Touchscreen::read_touch_weight_(uint8_t touch) {
  return this->read_byte_(FT63X6_ADDR_TOUCH1_WEIGHT + (touch * 6));
}
uint8_t FT63X6Touchscreen::read_touch_misc_(uint8_t touch) {
  return this->read_byte_(FT63X6_ADDR_TOUCH1_MISC + (touch * 6)) >> 4;
}

uint8_t FT63X6Touchscreen::read_byte_(uint8_t addr) {
  uint8_t byte = 0;

  if (!this->edt_read_variant_) {
    this->read_byte(addr, &byte);
    return byte;
  }

  // EDT-M06 work-mode register read.
  const uint8_t cmd[2] = {
      0xFC,
      static_cast<uint8_t>((addr & 0x3F) | 0x40),
  };
  uint8_t response[2] = {0, 0};

  const auto err =
      this->bus_->write_readv(this->address_, cmd, sizeof(cmd),
                              response, sizeof(response));

  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "EDT read failed for register 0x%02X: %d", addr, err);
    return 0;
  }

  // EDT-M06 response: data byte followed by XOR checksum.
  const uint8_t crc = cmd[0] ^ cmd[1] ^ response[0];

  if (crc != response[1]) {
    ESP_LOGW(TAG, "EDT read CRC mismatch for register 0x%02X: "
                  "expected 0x%02X, received 0x%02X",
             addr, crc, response[1]);
    return 0;
  }

  return response[0];
}

}  // namespace esphome::ft63x6
