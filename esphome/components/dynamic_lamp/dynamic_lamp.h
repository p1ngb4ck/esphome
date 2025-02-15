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
  bool available = false;
  bool in_use = false;
  std::string output_id;
  uint8_t output_index;
  output::FloatOutput *output;
  float state;
  uint8_t mode = 0;
  optional<float> min_value;
  optional<float> max_value;
  bool update_level = false;
};

enum DynamicLampIdx : uint8_t {
  LAMP_1 = 0,
  LAMP_2 = 1,
  LAMP_3 = 2,
  LAMP_4 = 3,
  LAMP_5 = 4,
  LAMP_6 = 5,
  LAMP_7 = 6,
  LAMP_8 = 7,
  LAMP_9 = 8,
  LAMP_10 = 9,
  LAMP_11 = 10,
  LAMP_12 = 11,
  LAMP_13 = 12,
  LAMP_14 = 13,
  LAMP_15 = 14,
  LAMP_16 = 15,
};

struct CombinedLamp {
  bool active = false;
  std::string name = "";
  uint8_t lamp_index;
  float state;
  bool used_outputs[16];
};

class DynamicLamp;

class DynamicLampComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void begin();
  void add_available_output(output::FloatOutput * output, std::string output_id);
  void set_save_mode(uint8_t save_mode);
  void add_lamp(std::string name);
  void remove_lamp(std::string name);
  void add_output_to_lamp(std::string lamp_name, LinkedOutput *output);
  void remove_output_from_lamp(std::string lamp_name, LinkedOutput *output);
  std::array<bool, 16> get_lamp_outputs(uint8_t lamp_number);
  std::array<bool, 16> get_lamp_outputs_by_name(std::string lamp_name);

 protected:
  friend class DynamicLamp;
  void set_lamp_level(std::string lamp_name, float state);
  void restore_lamp_values_(uint8_t lamp_number);
  void set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value);

  CombinedLamp active_lamps_[16];
  LinkedOutput available_outputs_[16];
  uint8_t save_mode_;
  uint8_t lamp_count_ = 0;
};


}  // namespace dynamic_lamp
}  // namespace esphome
