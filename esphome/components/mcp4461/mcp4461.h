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
  uint16_t state = 0;
  bool enabled = true;
  bool wiper_lock_active = false;
};

enum class Mcp4461Defaults : uint8_t { WIPER_VALUE = 0x80 };
enum class Mcp4461Commands : uint8_t { WRITE = 0x00, INCREMENT = 0x04, DECREMENT = 0x08, READ = 0x0C };

enum class Mcp4461Addresses : uint8_t {
  MCP4461_VW0 = 0x00,
  MCP4461_VW1 = 0x10,
  MCP4461_VW2 = 0x60,
  MCP4461_VW3 = 0x70,
  MCP4461_STATUS = 0x50,
  MCP4461_TCON0 = 0x40,
  MCP4461_TCON1 = 0xA0,
  MCP4461_EEPROM_1 = 0xB0
};

enum Mcp4461WiperIdx : uint8_t {
  MCP4461_WIPER_0 = 0,
  MCP4461_WIPER_1 = 1,
  MCP4461_WIPER_2 = 2,
  MCP4461_WIPER_3 = 3,
  MCP4461_WIPER_4 = 4,
  MCP4461_WIPER_5 = 5,
  MCP4461_WIPER_6 = 6,
  MCP4461_WIPER_7 = 7
};

enum class Mcp4461EepromLocation : uint8_t {
  MCP4461_EEPROM_0 = 0,
  MCP4461_EEPROM_1 = 1,
  MCP4461_EEPROM_2 = 2,
  MCP4461_EEPROM_3 = 3,
  MCP4461_EEPROM_4 = 4
};

enum class Mcp4461TerminalIdx : uint8_t { MCP4461_TERMINAL_0 = 0, MCP4461_TERMINAL_1 = 1 };

class Mcp4461Wiper;

// Mcp4461Component
class Mcp4461Component : public Component, public i2c::I2CDevice {
 public:
  Mcp4461Component(bool disable_wiper_0, bool disable_wiper_1, bool disable_wiper_2, bool disable_wiper_3)
      : wiper_0_disabled_(disable_wiper_0), wiper_1_disabled_(disable_wiper_1), wiper_2_disabled_(disable_wiper_2), wiper_3_disabled_(disable_wiper_3) {
    this->reg_[0].enabled = !wiper_0_disabled_;
    this->reg_[1].enabled = !wiper_1_disabled_;
    this->reg_[2].enabled = !wiper_2_disabled_;
    this->reg_[3].enabled = !wiper_3_disabled_;
  }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  void loop() override;

  uint8_t get_status_register();
  uint16_t get_wiper_level(Mcp4461WiperIdx wiper);
  void set_wiper_level(Mcp4461WiperIdx wiper, uint16_t value);
  void update_wiper_level(Mcp4461WiperIdx wiper);
  void enable_wiper(Mcp4461WiperIdx wiper);
  void disable_wiper(Mcp4461WiperIdx wiper);
  void increase_wiper(Mcp4461WiperIdx wiper);
  void decrease_wiper(Mcp4461WiperIdx wiper);
  void enable_terminal(Mcp4461WiperIdx wiper, char terminal);
  void disable_terminal(Mcp4461WiperIdx, char terminal);
  void update_terminal_register(Mcp4461TerminalIdx terminal_connector);
  uint8_t get_terminal_register(Mcp4461TerminalIdx terminal_connector);
  void set_terminal_register(Mcp4461TerminalIdx terminal_connector, uint8_t data);
  uint16_t get_eeprom_value(Mcp4461EepromLocation location);
  void set_eeprom_value(Mcp4461EepromLocation location, uint16_t value);

 protected:
  friend class Mcp4461Wiper;
  void set_write_protection_status_();
  bool is_writing_();
  bool is_eeprom_busy_();
  uint8_t get_wiper_address_(uint8_t wiper);
  uint16_t read_wiper_level_(uint8_t wiper);
  void write_wiper_level_(uint8_t wiper, uint16_t value);
  bool mcp4461_write_(uint8_t addr, uint16_t data, bool nonvolatile = false);
  uint8_t calc_terminal_connector_byte_(Mcp4461TerminalIdx terminal_connector);

  WiperState reg_[8];
  void begin_();
  bool update_{false};
  uint32_t previous_write_exec_time_;
  bool write_protected_{false};
  bool wiper_0_disabled_{false};
  bool wiper_1_disabled_{false};
  bool wiper_2_disabled_{false};
  bool wiper_3_disabled_{false};
};
}  // namespace mcp4461
}  // namespace esphome
