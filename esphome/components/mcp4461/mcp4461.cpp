#include "mcp4461.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace mcp4461 {

static const char *const TAG = "mcp4461";

void Mcp4461Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up mcp4461 (0x%02" PRIX8 ")...", this->address_);
  auto err = this->write(nullptr, 0);
  if (err != i2c::ERROR_OK) {
    this->mark_failed();
    return;
  }
  this->begin_();
}

void Mcp4461Component::begin_() {
  for (uint8_t i = 0; i < 8; i++) {
    if (this->reg_[i].enabled) {
      this->reg_[i].state = this->get_wiper_level(i);
    }
  }
}

void Mcp4461Component::dump_config() {
  ESP_LOGCONFIG(TAG, "mcp4461:");
  LOG_I2C_DEVICE(this);
  for (uint8_t i = 0; i < 8; ++i) {
    ESP_LOGCONFIG(TAG, "Wiper [%" PRIu8 "]: %s", i, ONOFF(this->reg_[i].enabled));
    ESP_LOGCONFIG(TAG, "  ├── State: %" PRIu16, this->reg_[i].state);
    ESP_LOGCONFIG(TAG, "  ├── Terminal A: %s", ONOFF(this->reg_[i].terminal_a));
    ESP_LOGCONFIG(TAG, "  ├── Terminal B: %s", ONOFF(this->reg_[i].terminal_b));
    ESP_LOGCONFIG(TAG, "  ├── Terminal W: %s", ONOFF(this->reg_[i].terminal_w));
    ESP_LOGCONFIG(TAG, "  └── Terminal HW: %s", ONOFF(this->reg_[i].terminal_hw));
  }
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with mcp4461 failed!");
  }
}

void Mcp4461Component::loop() {
  if (this->update_) {
    uint8_t i;
    for (i = 0; i < 8; i++) {
      // set wiper i state if changed
      if (this->reg_[i].state != this->get_wiper_level(i)) {
        this->write_wiper_level_(i, this->reg_[i].state);
      }
      // terminal register changes only applicable to wipers 0-3 !
      if (i < 4) {
        // set terminal register changes
        if (i == 0 || i == 2) {
          Mcp4461TerminalIdx terminal_connector = Mcp4461TerminalIdx::MCP4461_TERMINAL_0;
          if (i > 0) {
            terminal_connector = Mcp4461TerminalIdx::MCP4461_TERMINAL_1;
          }
          uint8_t new_terminal_value = this->calc_terminal_connector_byte_(terminal_connector);
          if (new_terminal_value != this->get_terminal_register(terminal_connector)) {
            ESP_LOGV(TAG, "updating terminal %" PRIu8 " to new value %" PRIu8, static_cast<uint8_t>(terminal_connector), new_terminal_value);
            this->set_terminal_register(terminal_connector, new_terminal_value);
          }
        }
      }
    }
    this->update_ = false;
  }
}

uint16_t Mcp4461Component::get_status_register() {
  uint8_t reg = 0;
  reg |= static_cast<uint8_t>(Mcp4461Addresses::MCP4461_STATUS);
  reg |= static_cast<uint8_t>(Mcp4461Commands::READ);
  uint16_t buf;
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching status register value");
    return 0;
  }
  return buf;
}

bool Mcp4461Component::is_writing_() { return (bool) ((this->get_status_register() >> 4) & 0x01); }

uint8_t Mcp4461Component::get_wiper_address_(uint8_t wiper) {
  uint8_t addr;
  bool nonvolatile = false;
  if (wiper > 3) {
    nonvolatile = true;
    wiper = wiper - 4;
  }
  switch (wiper) {
    case 0:
      addr = (uint8_t) Mcp4461Addresses::MCP4461_VW0;
      break;
    case 1:
      addr = (uint8_t) Mcp4461Addresses::MCP4461_VW1;
      break;
    case 2:
      addr = (uint8_t) Mcp4461Addresses::MCP4461_VW2;
      break;
    case 3:
      addr = (uint8_t) Mcp4461Addresses::MCP4461_VW3;
      break;
    default:
      ESP_LOGE(TAG, "unknown wiper specified");
      return 0;
  }
  if (nonvolatile) {
    addr = addr + 0x20;
  }
  return addr;
}

uint16_t Mcp4461Component::get_wiper_level(uint8_t wiper) {
  uint8_t reg = 0;
  uint16_t buf = 0;
  reg |= this->get_wiper_address_(wiper);
  reg |= (uint8_t) Mcp4461Commands::READ;
  if (wiper > 3) {
    while (this->is_writing_()) {
      ESP_LOGVV(TAG, "delaying during eeprom write");
      yield();
    }
  }
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching %swiper %" PRIu8 " value", (wiper > 3) ? "nonvolatile " : "", wiper);
    return 0;
  }
  return buf;
}

void Mcp4461Component::update_wiper_level(uint8_t wiper) {
  uint16_t data;
  data = this->get_wiper_level(wiper);
  ESP_LOGV(TAG, "Got value %" PRIu16 " from wiper %" PRIu8, data, wiper);
  this->reg_[wiper].state = data;
}

void Mcp4461Component::set_wiper_level(uint8_t wiper, uint16_t value) {
  ESP_LOGV(TAG, "Setting MCP4461 wiper %" PRIu8 " to %" PRIu16 "!", wiper, value);
  this->reg_[wiper].state = value;
  this->update_ = true;
}

void Mcp4461Component::write_wiper_level_(uint8_t wiper, uint16_t value) {
  if (wiper > 3) {
    while (this->is_writing_()) {
      ESP_LOGV(TAG, "delaying during eeprom write");
      yield();
    }
  }
  this->mcp4461_write_(this->get_wiper_address_(wiper), value);
}

void Mcp4461Component::enable_wiper(uint8_t wiper) {
  ESP_LOGV(TAG, "Enabling wiper %d", wiper);
  this->reg_[wiper].terminal_hw = true;
  this->update_ = true;
}

void Mcp4461Component::disable_wiper(uint8_t wiper) {
  ESP_LOGV(TAG, "Disabling wiper %d", wiper);
  this->reg_[wiper].terminal_hw = false;
  this->update_ = true;
}

void Mcp4461Component::increase_wiper(uint8_t wiper) {
  ESP_LOGV(TAG, "Increasing wiper %d", wiper);
  uint8_t reg = 0;
  uint8_t addr;
  addr = this->get_wiper_address_(wiper);
  reg |= addr;
  reg |= (uint8_t) Mcp4461Commands::INCREMENT;
  this->write(&this->address_, reg, sizeof(reg));
}

void Mcp4461Component::decrease_wiper(uint8_t wiper) {
  ESP_LOGV(TAG, "Decreasing wiper %d", wiper);
  uint8_t reg = 0;
  uint8_t addr;
  addr = this->get_wiper_address_(wiper);
  reg |= addr;
  reg |= (uint8_t) Mcp4461Commands::DECREMENT;
  this->write(&this->address_, reg, sizeof(reg));
}

uint8_t Mcp4461Component::calc_terminal_connector_byte_(Mcp4461TerminalIdx terminal_connector) {
  uint8_t i;
  if (((uint8_t) terminal_connector == 0 || (uint8_t) terminal_connector == 1)) {
    i = 0;
  } else {
    i = 2;
  }
  uint8_t new_value_byte_array[8];
  new_value_byte_array[0] = (uint8_t) this->reg_[i].terminal_b;
  new_value_byte_array[1] = (uint8_t) this->reg_[i].terminal_w;
  new_value_byte_array[2] = (uint8_t) this->reg_[i].terminal_a;
  new_value_byte_array[3] = (uint8_t) this->reg_[i].terminal_hw;
  new_value_byte_array[4] = (uint8_t) this->reg_[(i + 1)].terminal_b;
  new_value_byte_array[5] = (uint8_t) this->reg_[(i + 1)].terminal_w;
  new_value_byte_array[6] = (uint8_t) this->reg_[(i + 1)].terminal_a;
  new_value_byte_array[7] = (uint8_t) this->reg_[(i + 1)].terminal_hw;
  unsigned char new_value_byte = 0;
  uint8_t b;
  for (b = 0; b < 8; b++) {
    new_value_byte += (new_value_byte_array[b] << (7 - b));
  }
  return (uint8_t) new_value_byte;
}

uint8_t Mcp4461Component::get_terminal_register(Mcp4461TerminalIdx terminal_connector) {
  uint8_t reg = 0;
  if ((uint8_t) terminal_connector == 0) {
    reg |= (uint8_t) Mcp4461Addresses::MCP4461_TCON0;
  } else {
    reg |= (uint8_t) Mcp4461Addresses::MCP4461_TCON1;
  }
  reg |= (uint8_t) Mcp4461Commands::READ;
  uint16_t buf;
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching terminal register value");
    return 0;
  }
  return (uint8_t) (buf & 0x00ff);
}

void Mcp4461Component::update_terminal_register(Mcp4461TerminalIdx terminal_connector) {
  if (((uint8_t) terminal_connector != 0 && (uint8_t) terminal_connector != 1)) {
    return;
  }
  uint8_t terminal_data;
  terminal_data = this->get_terminal_register(terminal_connector);
  ESP_LOGV(TAG, "Got terminal register %d data %0xh", (uint8_t) terminal_connector, terminal_data);
  uint8_t wiper_index = 0;
  if ((uint8_t) terminal_connector == 1) {
    wiper_index = 2;
  }
  this->reg_[wiper_index].terminal_b = ((terminal_data >> 0) & 0x01);
  this->reg_[wiper_index].terminal_w = ((terminal_data >> 1) & 0x01);
  this->reg_[wiper_index].terminal_a = ((terminal_data >> 2) & 0x01);
  this->reg_[wiper_index].terminal_hw = ((terminal_data >> 3) & 0x01);
  this->reg_[(wiper_index + 1)].terminal_b = ((terminal_data >> 4) & 0x01);
  this->reg_[(wiper_index + 1)].terminal_w = ((terminal_data >> 5) & 0x01);
  this->reg_[(wiper_index + 1)].terminal_a = ((terminal_data >> 6) & 0x01);
  this->reg_[(wiper_index + 1)].terminal_hw = ((terminal_data >> 7) & 0x01);
}

void Mcp4461Component::set_terminal_register(Mcp4461TerminalIdx terminal_connector, uint8_t data) {
  uint8_t addr;
  if ((uint8_t) terminal_connector == 0) {
    addr = (uint8_t) Mcp4461Addresses::MCP4461_TCON0;
  } else if ((uint8_t) terminal_connector == 1) {
    addr = (uint8_t) Mcp4461Addresses::MCP4461_TCON1;
  } else {
    return;
  }
  this->mcp4461_write_(addr, data);
}

void Mcp4461Component::enable_terminal(uint8_t wiper, char terminal) {
  if (wiper > 3) {
    return;
  }
  ESP_LOGV(TAG, "Enabling terminal %c of wiper %d", terminal, wiper);
  switch (terminal) {
    case 'h':
      this->reg_[wiper].terminal_hw = true;
      break;
    case 'a':
      this->reg_[wiper].terminal_a = true;
      break;
    case 'b':
      this->reg_[wiper].terminal_b = true;
      break;
    case 'w':
      this->reg_[wiper].terminal_w = true;
      break;
    default:
      this->status_set_warning();
      ESP_LOGW(TAG, "Unknown terminal %c specified", terminal);
      return;
  }
  this->update_ = true;
}

void Mcp4461Component::disable_terminal(uint8_t wiper, char terminal) {
  if (wiper > 3) {
    return;
  }
  ESP_LOGV(TAG, "Disabling terminal %c of wiper %d", terminal, wiper);
  switch (terminal) {
    case 'h':
      this->reg_[wiper].terminal_hw = false;
      break;
    case 'a':
      this->reg_[wiper].terminal_a = false;
      break;
    case 'b':
      this->reg_[wiper].terminal_b = false;
      break;
    case 'w':
      this->reg_[wiper].terminal_w = false;
      break;
    default:
      this->status_set_warning();
      ESP_LOGW(TAG, "Unknown terminal %c specified", terminal);
      return;
  }
  this->update_ = true;
}

uint16_t Mcp4461Component::get_eeprom_value(MCP4461EEPRomLocation location) {
  uint8_t reg = 0;
  reg |= (uint8_t) (MCP4461_EEPROM_1 + ((uint8_t) location * 0x10));
  reg |= (uint8_t) Mcp4461Commands::READ;
  uint16_t buf;
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching EEPRom location value");
    return 0;
  }
  return buf;
}

void Mcp4461Component::set_eeprom_value(MCP4461EEPRomLocation location, uint16_t value) {
  uint8_t addr = 0;
  if (value > 256) {
    return;
  } else if (value == 256) {
    addr = 1;
  }
  uint8_t data;
  addr |= (uint8_t) (MCP4461_EEPROM_1 + ((uint8_t) location * 0x10));
  data = (uint8_t) (value & 0x00ff);
  while (this->is_writing_()) {
    ESP_LOGV(TAG, "delaying during eeprom write");
  }
  this->write_byte(addr, data);
}

void Mcp4461Component::mcp4461_write_(uint8_t addr, uint16_t data) {
  uint8_t reg = 0;
  if (data > 0x100) {
    return;
  }
  if (data > 0xFF) {
    reg = 1;
  }
  uint8_t value_byte;
  value_byte = (uint8_t) (data & 0x00ff);
  ESP_LOGV(TAG, "Writing value %d", data);
  reg |= addr;
  reg |= (uint8_t) Mcp4461Commands::WRITE;
  this->write_byte(reg, value_byte);
}
}  // namespace mcp4461
}  // namespace esphome
