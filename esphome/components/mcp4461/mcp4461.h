#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace mcp4461 {

struct WiperState {
  bool terminal_a = true;
  bool terminal_b = true;
  bool terminal_w = true;
  bool terminal_hw = true;
  uint16_t state;
  bool enabled = true;
};

enum class Mcp4461Defaults { WIPER_VALUE = 0x80 };
enum class Mcp4461Commands : uint8_t { WRITE = 0x00, INCREMENT = 0x04, DECREMENT = 0x08, READ = 0x0C };

enum class Mcp4461Addresses {
  MCP4461_VW0 = 0x00,
  MCP4461_VW1 = 0x10,
  MCP4461_VW2 = 0x60,
  MCP4461_VW3 = 0x70,
  MCP4461_STATUS = 0x50,
  MCP4461_TCON0 = 0x40,
  MCP4461_TCON1 = 0xA0,
  MCP4461_EEPROM_1 = 0xB0
};

enum MCP4461WiperIdx {
  MCP4461_WIPER_0 = 0,
  MCP4461_WIPER_1 = 1,
  MCP4461_WIPER_2 = 2,
  MCP4461_WIPER_3 = 3,
  MCP4461_WIPER_4 = 4,
  MCP4461_WIPER_5 = 5,
  MCP4461_WIPER_6 = 6,
  MCP4461_WIPER_7 = 7
};

enum MCP4461EEPRomLocation {
  MCP4461_EEPROM_0 = 0,
  MCP4461_EEPROM_1 = 1,
  MCP4461_EEPROM_2 = 2,
  MCP4461_EEPROM_3 = 3,
  MCP4461_EEPROM_4 = 4
};

enum class Mcp4461TerminalIdx { MCP4461_TERMINAL_0 = 0, MCP4461_TERMINAL_1 = 1 };

class Mcp4461Wiper;

// Mcp4461Component
class Mcp4461Component : public Component, public i2c::I2CDevice {
 public:
  Mcp4461Component(bool disable_wiper_0, bool disable_wiper_1, bool disable_wiper_2, bool disable_wiper_3)
      : wiper_0_enabled_(false), wiper_1_enabled_(false), wiper_2_enabled_(false), wiper_3_enabled_(false) {
    this->reg_[0].enabled = this->wiper_0_enabled_;
    this->reg_[1].enabled = this->wiper_1_enabled_;
    this->reg_[2].enabled = this->wiper_2_enabled_;
    this->reg_[3].enabled = this->wiper_3_enabled_;
  }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void loop() override;

  uint16_t get_status_register();
  uint16_t get_wiper_level(uint8_t wiper);
  void set_wiper_level(uint8_t wiper, uint16_t value);
  void update_wiper_level(uint8_t wiper);
  void enable_wiper(uint8_t wiper);
  void disable_wiper(uint8_t wiper);
  void increase_wiper(uint8_t wiper);
  void decrease_wiper(uint8_t wiper);
  void enable_terminal(uint8_t wiper, char terminal);
  void disable_terminal(uint8_t wiper, char terminal);
  void update_terminal_register(Mcp4461TerminalIdx terminal_connector);
  uint8_t get_terminal_register(Mcp4461TerminalIdx terminal_connector);
  void set_terminal_register(Mcp4461TerminalIdx terminal_connector, uint8_t data);
  uint16_t get_eeprom_value(MCP4461EEPRomLocation location);
  void set_eeprom_value(MCP4461EEPRomLocation location, uint16_t value);

 protected:
  friend Mcp4461Wiper;
  bool is_writing_();
  uint8_t get_wiper_address_(uint8_t wiper);
  void write_wiper_level_(uint8_t wiper, uint16_t value);
  void mcp4461_write_(uint8_t addr, uint16_t data);
  uint8_t calc_terminal_connector_byte_(Mcp4461TerminalIdx terminal_connector);

  WiperState reg_[8];
  void begin_();
  bool update_{false};
  bool wiper_0_enabled_{true};
  bool wiper_1_enabled_{true};
  bool wiper_2_enabled_{true};
  bool wiper_3_enabled_{true};
};
}  // namespace mcp4461
}  // namespace esphome
