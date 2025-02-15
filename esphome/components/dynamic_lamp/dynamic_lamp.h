#pragma once

#include "esphome/core/component.h"
#include "esphome/components/output/float_output.h"

namespace esphome {
namespace dynamic_lamp {

enum SupportedSaveModes : uint8_t {
  SAVE_MODE_NONE = 0,
  SAVE_MODE_FRAM = 1
};

enum LinkedOutputModeIdx : uint8_t {
  MODE_EQUAL = 0,
  MODE_STATIC = 1,
  MODE_PERCENTAGE = 2,
  MODE_FUNCTION = 3
};

struct LinkedOutput {
  bool active = false;
  std::string output_id;
  uint8_t output_index;
  output::FloatOutput *output;
  float state;
  uint8_t mode = 0;
  optional<float> min_value;
  optional<float> max_value;
  bool update_level = false;
};

struct CombinedLamp {
  bool active = false;
  bool used_outputs[16];
};

class DynamicLamp : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void add_available_output(output::FloatOutput * output, std::string output_id);
  void set_save_mode(uint8_t save_mode);

 protected:
  uint8_t add_lamp_();
  std::array<bool, 16> get_lamp_outputs_(uint8_t lamp_number);
  void add_lamp_output_(uint8_t lamp_number, LinkedOutput output);
  void restore_lamp_values_(uint8_t lamp_number);
  void set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value);
  std::string_view ltrim_(std::string_view str);
  std::string_view rtrim_(std::string_view str);
  std::string_view trim_(std::string_view str);

  CombinedLamp active_lamps_[16];
  LinkedOutput available_outputs_[16];
  uint8_t save_mode_;
  uint8_t lamp_count_ = 0;
};


}  // namespace dynamic_lamp
}  // namespace esphome
