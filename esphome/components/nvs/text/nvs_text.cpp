#include "nvs_text.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <nvs.h>
#include <nvs_flash.h>

#include <string>

namespace esphome::nvs {

static const char *const TAG = "nvs.text";

void NvsText::setup() {
  std::string value = this->initial_value_;
  nvs_handle_t handle;
  if (nvs_flash_init() == ESP_OK && nvs_open(this->namespace_, NVS_READONLY, &handle) == ESP_OK) {
    size_t len = 0;
    if (nvs_get_str(handle, this->key_, nullptr, &len) == ESP_OK && len > 0) {
      value.resize(len);
      if (nvs_get_str(handle, this->key_, value.data(), &len) == ESP_OK) {
        value.resize(len - 1);
      } else {
        value = this->initial_value_;
      }
    }
    nvs_close(handle);
  }
  this->publish_state(value);
}

void NvsText::control(const std::string &value) {
  nvs_handle_t handle;
  if (nvs_open(this->namespace_, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "%s: NVS namespace %s not writable", this->key_, this->namespace_);
    return;
  }
  esp_err_t err = value.empty() ? nvs_erase_key(handle, this->key_) : nvs_set_str(handle, this->key_, value.c_str());
  if (err == ESP_ERR_NVS_NOT_FOUND)
    err = ESP_OK;
  if (err == ESP_OK)
    err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: not saved (%s)", this->key_, esp_err_to_name(err));
    return;
  }
  this->publish_state(value);
}

void NvsText::dump_config() {
  LOG_TEXT("", "NVS Text", this);
  ESP_LOGCONFIG(TAG, "  Namespace: %s\n  Key: %s", this->namespace_, this->key_);
}

}  // namespace esphome::nvs

#endif
