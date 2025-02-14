#pragma once

#include "esphome/core/component.h"
#include <boost/algorithm/string/trim.hpp>

namespace esphome {
namespace dynamic_lamp {

enum SupportedSaveModes : uint8_t {
  SAVE_MODE_NONE = 0,
  SAVE_MODE_FRAM = 1
};

enum LinkedOutputModeIdx : uint8_t {
  MODE_EQUAL = 0,
  MODE_STATIC = 1,
  MODE_PERCENT = 2,
  MODE_FUNCTION = 3
};

struct LinkedOutput {
  bool active = false;
  std::string output_id = "";
  uint8_t mode = 0;
  optional<float> min_value;
  optional<float> max_value;
  bool update_level = false;
};

struct CombinedLamp {
  bool active = false;
  LinkedOutput used_outputs[16];
};

class DynamicLamp : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void set_lamp_count(uint8_t lamp_count);
  void set_available_outputs(std::string output_list);
  void set_save_mode(uint8_t save_mode);

 protected:
  void restore_lamp_values_(uint8_t lamp_number);
  void set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value);

  CombinedLamp active_lamps_[16];
  std::string available_outputs_[16] = { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" };
  uint8_t lamp_count_;
  uint8_t save_mode_;
};


}  // namespace dynamic_lamp
}  // namespace esphome
