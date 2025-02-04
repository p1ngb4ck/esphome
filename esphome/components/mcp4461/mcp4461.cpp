#include "mcp4461.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

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
      this->reg_[i].state = this->read_wiper_level_(i);
    } else { 
      Mcp4461WiperIdx wiper_idx;
      wiper_idx = static_cast<Mcp4461WiperIdx>(i);
      this->disable_wiper(wiper_idx);
    }
}

void Mcp4461Component::dump_config() {
  ESP_LOGCONFIG(TAG, "mcp4461:");
  LOG_I2C_DEVICE(this);
  for (uint8_t i = 0; i < 8; ++i) {
    ESP_LOGCONFIG(TAG, "Wiper [%" PRIu8 "] level: %" PRIu16, i, this->reg_[i].state);
    // terminals only valid for volatile wipers 0-3 - enable/disable is terminal hw
    // so also invalid for nonvolatile. For these, only print current level.
    if (i < 4) {
      ESP_LOGCONFIG(TAG, "  ├── Status: %s", ONOFF(this->reg_[i].enabled));
      ESP_LOGCONFIG(TAG, "  ├── Terminal A: %s", ONOFF(this->reg_[i].terminal_a));
      ESP_LOGCONFIG(TAG, "  ├── Terminal B: %s", ONOFF(this->reg_[i].terminal_b));
      ESP_LOGCONFIG(TAG, "  ├── Terminal W: %s", ONOFF(this->reg_[i].terminal_w));
      ESP_LOGCONFIG(TAG, "  └── Terminal HW: %s", ONOFF(this->reg_[i].terminal_hw));
    }
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
      if (this->reg_[i].state != this->read_wiper_level_(i)) {
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
            ESP_LOGV(TAG, "updating terminal %" PRIu8 " to new value %" PRIu8, static_cast<uint8_t>(terminal_connector),
                     new_terminal_value);
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
    ESP_LOGE(TAG, "Error fetching status register value");
    return 0;
  }
  return buf;
}

bool Mcp4461Component::is_writing_() { return static_cast<bool>((this->get_status_register() >> 4) & 0x01); }

uint8_t Mcp4461Component::get_wiper_address_(uint8_t wiper) {
  uint8_t addr;
  bool nonvolatile = false;
  if (wiper > 3) {
    nonvolatile = true;
    wiper = wiper - 4;
  }
  switch (wiper) {
    case 0:
      addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_VW0);
      break;
    case 1:
      addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_VW1);
      break;
    case 2:
      addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_VW2);
      break;
    case 3:
      addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_VW3);
      break;
    default:
      ESP_LOGW(TAG, "unknown wiper specified");
      return 0;
  }
  if (nonvolatile) {
    addr = addr + 0x20;
  }
  return addr;
}

uint16_t Mcp4461Component::get_wiper_level(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  if (!(this->reg_[wiper_idx].enabled)) {
    ESP_LOGW(TAG, "reading from disabled volatile wiper %" PRIu8 ", returning 0", wiper_idx);
    return static_cast<uint16_t>(0);
  }
  return this->read_wiper_level_(wiper_idx);
}

uint16_t Mcp4461Component::read_wiper_level_(uint8_t wiper) {
  uint8_t reg = 0;
  uint16_t buf = 0;
  reg |= this->get_wiper_address_(wiper);
  reg |= static_cast<uint8_t>(Mcp4461Commands::READ);
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

void Mcp4461Component::update_wiper_level(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  uint16_t data;
  data = this->get_wiper_level(wiper);
  ESP_LOGV(TAG, "Got value %" PRIu16 " from wiper %" PRIu8, data, wiper_idx);
  this->reg_[wiper_idx].state = data;
}

void Mcp4461Component::set_wiper_level(Mcp4461WiperIdx wiper, uint16_t value) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  if (value > 0x100) {
    ESP_LOGW(TAG, "ignoring invalid wiper level %" PRIu16 "!");
    return;
  }
  ESP_LOGV(TAG, "Setting MCP4461 wiper %" PRIu8 " to %" PRIu16 "!", wiper_idx, value);
  this->reg_[wiper_idx].state = value;
  this->update_ = true;
}

void Mcp4461Component::write_wiper_level_(uint8_t wiper, uint16_t value) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  bool nonvolatile = false;
  if (wiper_idx > 3) {
    nonvolatile = true;
  }
  this->mcp4461_write_(this->get_wiper_address_(wiper_idx), value, nonvolatile);
}

void Mcp4461Component::enable_wiper(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  ESP_LOGV(TAG, "Enabling wiper %" PRIu8, wiper_idx);
  this->reg_[wiper_idx].terminal_hw = true;
  this->update_ = true;
}

void Mcp4461Component::disable_wiper(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  ESP_LOGV(TAG, "Disabling wiper %" PRIu8, wiper_idx);
  this->reg_[wiper_idx].terminal_hw = false;
  this->update_ = true;
}

void Mcp4461Component::increase_wiper(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  ESP_LOGV(TAG, "Increasing wiper %" PRIu8 "", wiper_idx);
  uint8_t reg = 0;
  uint8_t addr;
  addr = this->get_wiper_address_(wiper_idx);
  reg |= addr;
  reg |= static_cast<uint8_t>(Mcp4461Commands::INCREMENT);
  this->write(&this->address_, reg, sizeof(reg));
  this->reg_[wiper_idx].state++;
}

void Mcp4461Component::decrease_wiper(Mcp4461WiperIdx wiper) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  ESP_LOGV(TAG, "Decreasing wiper %" PRIu8 "", wiper_idx);
  uint8_t reg = 0;
  uint8_t addr;
  addr = this->get_wiper_address_(wiper_idx);
  reg |= addr;
  reg |= static_cast<uint8_t>(Mcp4461Commands::DECREMENT);
  this->write(&this->address_, reg, sizeof(reg));
  this->reg_[wiper_idx].state--;
}

uint8_t Mcp4461Component::calc_terminal_connector_byte_(Mcp4461TerminalIdx terminal_connector) {
  uint8_t i;
  if ((static_cast<uint8_t>(terminal_connector) == 0 || static_cast<uint8_t>(terminal_connector) == 1)) {
    i = 0;
  } else {
    i = 2;
  }
  uint8_t new_value_byte_array[8];
  new_value_byte_array[0] = static_cast<uint8_t>(this->reg_[i].terminal_b);
  new_value_byte_array[1] = static_cast<uint8_t>(this->reg_[i].terminal_w);
  new_value_byte_array[2] = static_cast<uint8_t>(this->reg_[i].terminal_a);
  new_value_byte_array[3] = static_cast<uint8_t>(this->reg_[i].terminal_hw);
  new_value_byte_array[4] = static_cast<uint8_t>(this->reg_[(i + 1)].terminal_b);
  new_value_byte_array[5] = static_cast<uint8_t>(this->reg_[(i + 1)].terminal_w);
  new_value_byte_array[6] = static_cast<uint8_t>(this->reg_[(i + 1)].terminal_a);
  new_value_byte_array[7] = static_cast<uint8_t>(this->reg_[(i + 1)].terminal_hw);
  unsigned char new_value_byte = 0;
  uint8_t b;
  for (b = 0; b < 8; b++) {
    new_value_byte += (new_value_byte_array[b] << (7 - b));
  }
  return static_cast<uint8_t>(new_value_byte);
}

uint8_t Mcp4461Component::get_terminal_register(Mcp4461TerminalIdx terminal_connector) {
  uint8_t reg = 0;
  if (static_cast<uint8_t>(terminal_connector) == 0) {
    reg |= static_cast<uint8_t>(Mcp4461Addresses::MCP4461_TCON0);
  } else {
    reg |= static_cast<uint8_t>(Mcp4461Addresses::MCP4461_TCON1);
  }
  reg |= static_cast<uint8_t>(Mcp4461Commands::READ);
  uint16_t buf;
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching terminal register value");
    return 0;
  }
  return static_cast<uint8_t>(buf & 0x00ff);
}

void Mcp4461Component::update_terminal_register(Mcp4461TerminalIdx terminal_connector) {
  if ((static_cast<uint8_t>(terminal_connector) != 0 && static_cast<uint8_t>(terminal_connector) != 1)) {
    return;
  }
  uint8_t terminal_data;
  terminal_data = this->get_terminal_register(terminal_connector);
  if (terminal_data == 0) {
    return;
  }
  ESP_LOGV(TAG, "Got terminal register %" PRIu8 " data %0xh", static_cast<uint8_t>(terminal_connector), terminal_data);
  uint8_t wiper_index = 0;
  if (static_cast<uint8_t>(terminal_connector) == 1) {
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
  if (static_cast<uint8_t>(terminal_connector) == 0) {
    addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_TCON0);
  } else if (static_cast<uint8_t>(terminal_connector) == 1) {
    addr = static_cast<uint8_t>(Mcp4461Addresses::MCP4461_TCON1);
  } else {
    ESP_LOGW(TAG, "Invalid terminal connector id %" PRIu8 " specified", static_cast<uint8_t>(terminal_connector));
    return;
  }
  this->mcp4461_write_(addr, data);
}

void Mcp4461Component::enable_terminal(Mcp4461WiperIdx wiper, char terminal) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  if (wiper_idx > 3) {
    return;
  }
  ESP_LOGV(TAG, "Enabling terminal %c of wiper %" PRIu8 "", terminal, wiper_idx);
  switch (terminal) {
    case 'h':
      this->reg_[wiper_idx].terminal_hw = true;
      break;
    case 'a':
      this->reg_[wiper_idx].terminal_a = true;
      break;
    case 'b':
      this->reg_[wiper_idx].terminal_b = true;
      break;
    case 'w':
      this->reg_[wiper_idx].terminal_w = true;
      break;
    default:
      this->status_set_warning();
      ESP_LOGW(TAG, "Unknown terminal %c specified", terminal);
      return;
  }
  this->update_ = true;
}

void Mcp4461Component::disable_terminal(Mcp4461WiperIdx wiper, char terminal) {
  uint8_t wiper_idx = static_cast<uint8_t>(wiper);
  if (wiper_idx > 3) {
    return;
  }
  ESP_LOGV(TAG, "Disabling terminal %c of wiper %" PRIu8 "", terminal, wiper_idx);
  switch (terminal) {
    case 'h':
      this->reg_[wiper_idx].terminal_hw = false;
      break;
    case 'a':
      this->reg_[wiper_idx].terminal_a = false;
      break;
    case 'b':
      this->reg_[wiper_idx].terminal_b = false;
      break;
    case 'w':
      this->reg_[wiper_idx].terminal_w = false;
      break;
    default:
      this->status_set_warning();
      ESP_LOGW(TAG, "Unknown terminal %c specified", terminal);
      return;
  }
  this->update_ = true;
}

uint16_t Mcp4461Component::get_eeprom_value(Mcp4461EepromLocation location) {
  uint8_t reg = 0;
  reg |= static_cast<uint8_t>(Mcp4461EepromLocation::MCP4461_EEPROM_1) + (static_cast<uint8_t>(location) * 0x10);
  reg |= static_cast<uint8_t>(Mcp4461Commands::READ);
  uint16_t buf;
  if (!this->read_byte_16(reg, &buf)) {
    this->status_set_warning();
    ESP_LOGW(TAG, "Error fetching EEPRom location value");
    return 0;
  }
  return buf;
}

void Mcp4461Component::set_eeprom_value(Mcp4461EepromLocation location, uint16_t value) {
  uint8_t addr = 0;
  if (value > 511) {
    return;
  }
  if (value > 256) {
    addr = 1;
  }
  addr |= static_cast<uint8_t>(Mcp4461EepromLocation::MCP4461_EEPROM_1) + (static_cast<uint8_t>(location) * 0x10);
  this->mcp4461_write_(addr, value, true);
}

void Mcp4461Component::mcp4461_write_(uint8_t addr, uint16_t data, bool nonvolatile) {
  uint8_t reg = 0;
  if (data > 0xFF) {
    reg = 1;
  }
  uint8_t value_byte;
  value_byte = static_cast<uint8_t>(data & 0x00ff);
  ESP_LOGV(TAG, "Writing value %" PRIu16 " to address %" PRIu8 "", data, addr);
  reg |= addr;
  reg |= static_cast<uint8_t>(Mcp4461Commands::WRITE);
  if (nonvolatile) {
    while (this->is_writing_()) {
      ESP_LOGV(TAG, "Waiting while eeprom busy");
      yield();
    }
  }
  this->write_byte(reg, value_byte);
}
}  // namespace mcp4461
}  // namespace esphome
