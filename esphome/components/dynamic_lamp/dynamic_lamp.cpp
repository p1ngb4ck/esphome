#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "dynamic_lamp.h"
#include <string>
#include <cstring>
#include <string_view>
#include <vector>

namespace esphome {
namespace dynamic_lamp {

static const char *TAG = "dynamic_lamp";

void DynamicLamp::setup() {
  uint8_t i = 0;
  bool valid = true;
  if(this->save_mode_ == 0) {
   for (i=0; i < 16; i++) {
     this->active_lamps_[i].active = false;
   }
  } else {
    while(i < 16) {
      this->restore_lamp_values_(i);
    }
  }
  // for testing only
  uint8_t first_lamp;
  first_lamp = this->add_lamp();
  this->add_lamp_output_(first_lamp, this->available_outputs_[0]);
  this->add_lamp_output_(first_lamp, this->available_outputs_[1]);
  this->add_lamp_output_(first_lamp, this->available_outputs_[2]);
  this->add_lamp_output_(first_lamp, this->available_outputs_[3]);
}

void DynamicLamp::loop() {
  uint8_t i = 0;
  for (i = 0; i < this->lamp_count_; i++) {
    if (this->active_lamps_[i].active) {
      uint8_t j = 0;
      for (j = 0; j < 16; j++) {
        if (this->active_lamps_[i].used_outputs[j]) {
          if (this->available_outputs_[j].update_level) {
            // Update level
            switch (this->available_outputs_[j].mode) {
              case MODE_EQUAL:
                // Equal
                break;
              case MODE_STATIC:
                // Static
                break;
              case MODE_PERCENT:
                // Percent
                break;
              case MODE_FUNCTION:
                // Function
                break;
              default:
                break;
            }
          }
        }
      }
    }
  }
}

void DynamicLamp::dump_config() {
  ESP_LOGCONFIG(TAG, "Dynamic Lamp feature loaded");
  switch(this->save_mode_) {
    case SAVE_MODE_NONE:
      ESP_LOGCONFIG(TAG, "Save mode set to NONE");
      break;
    case SAVE_MODE_FRAM:
      ESP_LOGCONFIG(TAG, "Save mode set to FRAM");
      break;
    default:
      ESP_LOGCONFIG(TAG, "Currently only NONE(0) && FRAM(1) save modes supported, ignoring value %" PRIu8 " and defaulting to NONE!", this->save_mode_);
      this->save_mode_ = 0;
  }
  for (uint8_t i = 0; i < 16; i++) {
    if (this->available_outputs_[i].active) {
      ESP_LOGCONFIG(TAG, "Using output with id %s as output number %" PRIu8 "", this->available_outputs_[i].output_id, i);
    }
  }
}

void DynamicLamp::set_save_mode(uint8_t save_mode) {
  this->save_mode_ = save_mode;
}

void DynamicLamp::set_available_outputs(std::string output_list) {
  uint8_t counter = 0;
  std::vector<std::string> v;
 
  char * token = strtok (&output_list[0],",");
  while (token != NULL)
  {
    v.push_back(token);
    token = strtok (NULL, ",");
  }
  for ( std::string s : v )
  {
    std::string input_id{ static_cast<std::string>(this->trim_(s.c_str())) };
    this->available_outputs_[counter] = LinkedOutput{true, input_id.c_str(), counter, 0, 0, 1.0, false};
    counter++;
  }
}

uint8_t DynamicLamp::add_lamp() {
  uint8_t i = 0;
  while (i < 16) {
    if (!this->active_lamps_[i].active) {
      this->active_lamps_[i].active = true;
      this->lamp_count_++;
      return i;
    }
    i++;
  }
  this->mark_failed();
  return 0;.c_str()
}

void DynamicLamp::add_lamp_output_(uint8_t lamp_number, LinkedOutput output) {
  this->active_lamps_[lamp_number].used_outputs[output.output_index] = true;
}

std::array<bool, 16> DynamicLamp::get_lamp_outputs_(uint8_t lamp_number) {
  std::array<bool, 16> bool_array;
  for (uint8_t i = 0; i < 16; i++) {
        bool_array[i] = this->active_lamps_[lamp_number].used_outputs[i];  
  }
  return bool_array;
}

void DynamicLamp::set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value) {

}

void DynamicLamp::restore_lamp_values_(uint8_t lamp_number) {
  this->active_lamps_[lamp_number].active = false;
}

std::string_view DynamicLamp::ltrim_(std::string_view str)
{
    const auto pos(str.find_first_not_of(" \t\n\r\f\v"));
    str.remove_prefix(std::min(pos, str.length()));
    return str;
}

std::string_view DynamicLamp::rtrim_(std::string_view str)
{
    const auto pos(str.find_last_not_of(" \t\n\r\f\v"));
    str.remove_suffix(std::min(str.length() - pos - 1, str.length()));
    return str;
}

std::string_view DynamicLamp::trim_(std::string_view str)
{
    str = this->ltrim_(str);
    str = this->rtrim_(str);
    return str;
}

}  // namespace dynamic_lamp
}  // namespace esphome
