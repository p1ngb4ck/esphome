#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "dynamic_lamp.h"
#include <string>
#include <cstring>
#include <string_view>
#include <vector>
#include <array>
#include <list>
#include <optional>
#include <algorithm>
#include <cinttypes>

namespace esphome {
namespace dynamic_lamp {

static const char *TAG = "dynamic_lamp";

void DynamicLamp::setup() {
  this->begin();
}

void DynamicLamp::begin() {
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
  /* keep example for future reference
  this->add_lamp_("First Lamp");
  this->add_lamp_output_("First Lamp", &this->available_outputs_[0]);
  this->add_lamp_output_("First Lamp", &this->available_outputs_[1]);
  this->add_lamp_output_("First Lamp", &this->available_outputs_[2]);
  this->add_lamp_output_("First Lamp", &this->available_outputs_[3]);
  */
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
              case MODE_PERCENTAGE:
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
    if (this->available_outputs_[i].available) {
      ESP_LOGCONFIG(TAG, "Using output with id %s as output number %" PRIu8 "", this->available_outputs_[i].output_id.c_str(), i);
    }
  }
}

void DynamicLamp::set_save_mode(uint8_t save_mode) {
  this->save_mode_ = save_mode;
}

void DynamicLamp::add_available_output(output::FloatOutput * output, std::string output_id) {
  uint8_t counter = 0;
  while (this->available_outputs_[counter].available) {
    counter++;
  }
  this->available_outputs_[counter] = LinkedOutput{
    true,
    false,
    output_id,
    counter,
    output,
    0, 0, 1.0, false
  };
  counter++;
}

void DynamicLamp::add_lamp(std::string name) {
  if (this->lamp_count_ < 15) {
    this->lamp_count_++;
    this->active_lamps_[this->lamp_count_].active = true;
    this->active_lamps_[this->lamp_count_].name = name;
    this->active_lamps_[this->lamp_count_].lamp_index = this->lamp_count_;
    for (uint8_t i = 0; i < 16; i++) {
      this->active_lamps_[this->lamp_count_].used_outputs[i] = false;
    }
    return;
  }
  ESP_LOGW(TAG, "No more lamps available, max 16 lamps supported!");
  this->status_set_warning();
}

void DynamicLamp::remove_lamp(std::string lamp_name) {
  uint8_t i = 0;
  while (i < this->lamp_count_) {
    if (this->active_lamps_[i].name == lamp_name) {
      for (uint8_t j = i; j < this->lamp_count_; j++) {
        this->active_lamps_[i].used_outputs[j] = false;
        this->available_outputs_[j].in_use = false;
      }
      this->active_lamps_[i].active = false;
      return;
    }
  }
  this->status_set_warning();
  ESP_LOGW(TAG, "No lamp with name %s defined !", lamp_name.c_str());
}

void DynamicLamp::add_output_to_lamp(std::string lamp_name, LinkedOutput *output) {
  uint8_t i = 0;
  while (i < 16) {
    if (this->active_lamps_[i].name == lamp_name) {
      this->active_lamps_[i].used_outputs[output->output_index] = true;
      output->in_use = true;
      ESP_LOGV(TAG, "Added output %s to lamp %s", output->output_id.c_str(), lamp_name.c_str());
      return;
    }
    i++;
  }
  this->status_set_warning();
  ESP_LOGW(TAG, "No lamp with name %s defined !", lamp_name.c_str());
}

void DynamicLamp::remove_output_from_lamp(std::string lamp_name, LinkedOutput *output) {
  uint8_t i = 0;
  while (i < 16) {
    if (this->active_lamps_[i].name == lamp_name) {
      this->active_lamps_[i].used_outputs[output->output_index] = false;
      output->in_use = false;
      ESP_LOGV(TAG, "Removed output %s from lamp %s", output->output_id.c_str(), lamp_name.c_str());
      return;
    }
    i++;
  }
  this->status_set_warning();
  ESP_LOGW(TAG, "No lamp with name %s defined !", lamp_name.c_str());
}

std::array<bool, 16> DynamicLamp::get_lamp_outputs(uint8_t lamp_number) {
  std::array<bool, 16> bool_array;
  for (uint8_t i = 0; i < 16; i++) {
        bool_array[i] = this->active_lamps_[lamp_number].used_outputs[i];  
  }
  return bool_array;
}

std::array<bool, 16> DynamicLamp::get_lamp_outputs_by_name(std::string lamp_name) {
  uint8_t i = 0;
  std::array<bool, 16> bool_array;
  for (i = 0; i < this->lamp_count_; i++) {
    if (this->active_lamps_[i].name == lamp_name) {
      return this->get_lamp_outputs(i);
    }
  }
  this->status_set_warning();
  ESP_LOGW(TAG, "No lamp with name %s defined !", lamp_name.c_str());
  return bool_array;
}

void DynamicLamp::set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value) {

}

void DynamicLamp::restore_lamp_values_(uint8_t lamp_number) {
  this->active_lamps_[lamp_number].active = false;
}

}  // namespace dynamic_lamp
}  // namespace esphome
