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

void DynamicLampComponent::setup() {
  this->begin();
}

void DynamicLampComponent::begin() {
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
  this->add_lamp("First Lamp");
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[0]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[1]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[2]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[3]);
  */
}

void DynamicLampComponent::loop() {
  uint8_t i = 0;
  for (i = 0; i < this->lamp_count_; i++) {
    if (this->active_lamps_[i].active && this->active_lamps_[i].update_) {
      uint8_t j = 0;
      for (j = 0; j < 16; j++) {
        if (this->active_lamps_[i].used_outputs[j]) {
          // Update level
          float new_state;
          new_state = this->active_lamps_[i].state_;
          switch (this->available_outputs_[j].mode) {
            case MODE_EQUAL:
              if (this->available_outputs_[j].min_value && new_state < *this->available_outputs_[j].min_value) {
                new_state = *this->available_outputs_[j].min_value;
              }
              else if (this->available_outputs_[j].max_value && new_state > *this->available_outputs_[j].max_value) {
                new_state = *this->available_outputs_[j].max_value;
              }
              break;
            case MODE_STATIC:
              new_state = this->available_outputs_[j].mode_value;
              break;
            case MODE_PERCENTAGE:
              new_state = this->active_lamps_[i].state_ * this->available_outputs_[j].mode_value;
              if (this->available_outputs_[j].min_value && new_state < *this->available_outputs_[j].min_value) {
                new_state = *this->available_outputs_[j].min_value;
              }
              else if (this->available_outputs_[j].max_value && new_state > *this->available_outputs_[j].max_value) {
                new_state = *this->available_outputs_[j].max_value;
              }
              break;
            case MODE_FUNCTION:
              // ToDo - yet to be implemented
              ESP_LOGW(TAG, "Mode %d for output %s is not implemented yet, sorry", this->available_outputs_[j].mode, this->available_outputs_[j].output_id.c_str());
              this->status_set_warning();
              continue;
            default:
              // Unknown
              ESP_LOGW(TAG, "Unknown mode %d for output %s", this->available_outputs_[j].mode, this->available_outputs_[j].output_id.c_str());
              this->status_set_warning();
              continue;
          }
          ESP_LOGV(TAG, "Setting output %s to level %f", this->available_outputs_[j].output_id.c_str(), state);
          this->available_outputs_[j].output->set_level(new_state);
        }
      }
      this->active_lamps_[i].update_ = false;
    }
  }
}

void DynamicLampComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Dynamic Lamp feature loaded");
  switch(this->save_mode_) {
    case SAVE_MODE_NONE:
      ESP_LOGCONFIG(TAG, "Save mode set to NONE");
      break;
    case SAVE_MODE_LOCAL:
      ESP_LOGCONFIG(TAG, "Save mode set to LOCAL");
      break;
    case SAVE_MODE_FRAM:
      ESP_LOGCONFIG(TAG, "Save mode set to FRAM");
      break;
    default:
      ESP_LOGCONFIG(TAG, "Currently only NONE(0), LOCAL(1) & FRAM(2) save modes supported, ignoring value %" PRIu8 " and defaulting to NONE!", this->save_mode_);
      this->save_mode_ = 0;
  }
  for (uint8_t i = 0; i < 16; i++) {
    if (this->available_outputs_[i].available) {
      ESP_LOGCONFIG(TAG, "Using output with id %s as output number %" PRIu8 "", this->available_outputs_[i].output_id.c_str(), i);
    }
  }
  this->add_lamp("First Lamp");
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[0]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[1]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[2]);
  this->add_output_to_lamp("First Lamp", &this->available_outputs_[3]);
}

void DynamicLampComponent::set_save_mode(uint8_t save_mode) {
  this->save_mode_ = save_mode;
}

void DynamicLampComponent::add_available_output(output::FloatOutput * output, std::string output_id) {
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

void DynamicLampComponent::add_lamp(std::string name) {
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

void DynamicLampComponent::remove_lamp(std::string lamp_name) {
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

void DynamicLampComponent::add_output_to_lamp(std::string lamp_name, LinkedOutput *output) {
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

void DynamicLampComponent::remove_output_from_lamp(std::string lamp_name, LinkedOutput *output) {
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

std::array<bool, 16> DynamicLampComponent::get_lamp_outputs(uint8_t lamp_number) {
  std::array<bool, 16> bool_array;
  for (uint8_t i = 0; i < 16; i++) {
        bool_array[i] = this->active_lamps_[lamp_number].used_outputs[i];  
  }
  return bool_array;
}

std::array<bool, 16> DynamicLampComponent::get_lamp_outputs_by_name(std::string lamp_name) {
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

void DynamicLampComponent::set_lamp_level(std::string lamp_name, float state) {
  
}

bool DynamicLampComponent::write_state_(uint8_t lamp_number, float state) {
  if (this->active_lamps_[lamp_number].active) {
    this->active_lamps_[lamp_number].state_ = state;
    this->active_lamps_[lamp_number].update_ = true;
    return true;
  }
  return false;
}

void DynamicLampComponent::set_lamp_values_(uint8_t lamp_number, bool active, uint16_t selected_outputs, uint8_t mode, uint8_t mode_value) {

}

void DynamicLampComponent::restore_lamp_values_(uint8_t lamp_number) {
  this->active_lamps_[lamp_number].active = false;
}

}  // namespace dynamic_lamp
}  // namespace esphome
