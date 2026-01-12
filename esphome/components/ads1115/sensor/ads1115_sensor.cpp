#include "ads1115_sensor.h"

#include "esphome/core/log.h"

namespace esphome {
namespace ads1115 {

static const char *const TAG = "ads1115.sensor";

#ifdef USE_ESP32
void ADS1115Sensor::setup() {
  // Create mutex for data synchronization
  this->data_mutex_ = xSemaphoreCreateMutex();

  // Create background task for non-blocking ADC measurements
  this->task_running_ = true;
  xTaskCreatePinnedToCore(sampling_task_, "ads1115_sensor", 4096, this, 1, &this->sampling_task_handle_, 1);

  ESP_LOGD(TAG, "Background sampling task created for '%s'", this->get_name().c_str());
}
#endif

float ADS1115Sensor::sample() {
#ifdef USE_ESP32
  // Return cached value from background task (non-blocking)
  if (this->data_mutex_ != nullptr && xSemaphoreTake(this->data_mutex_, 0) == pdTRUE) {
    float voltage = this->cached_voltage_;
    xSemaphoreGive(this->data_mutex_);
    return voltage;
  }
  return NAN;
#else
  // Non-ESP32: Direct blocking measurement
  return this->parent_->request_measurement(this->multiplexer_, this->gain_, this->resolution_, this->samplerate_);
#endif
}

void ADS1115Sensor::update() {
#ifdef USE_ESP32
  // Trigger background task to take a new measurement
  if (this->sampling_task_handle_ != nullptr) {
    xTaskNotifyGive(this->sampling_task_handle_);
  }
#else
  // Non-ESP32: Direct measurement
  float v = this->sample();
  if (!std::isnan(v)) {
    ESP_LOGD(TAG, "'%s': Got Voltage=%fV", this->get_name().c_str(), v);
    this->publish_state(v);
  }
#endif
}

#ifdef USE_ESP32
void ADS1115Sensor::loop() {
  // Check if task is still running
  if (this->sampling_task_handle_ != nullptr && !this->task_running_) {
    ESP_LOGE(TAG, "Background sampling task has stopped unexpectedly!");
    this->mark_failed();
    return;
  }

  // Check for new measurement data and publish immediately when available
  if (this->data_mutex_ != nullptr && xSemaphoreTake(this->data_mutex_, 0) == pdTRUE) {
    if (this->new_data_available_) {
      float voltage = this->cached_voltage_;
      this->new_data_available_ = false;
      xSemaphoreGive(this->data_mutex_);

      if (!std::isnan(voltage)) {
        ESP_LOGD(TAG, "'%s': Got Voltage=%fV", this->get_name().c_str(), voltage);
        this->publish_state(voltage);
      }
    } else {
      xSemaphoreGive(this->data_mutex_);
    }
  }
}
#endif

void ADS1115Sensor::dump_config() {
  LOG_SENSOR("  ", "ADS1115 Sensor", this);
  ESP_LOGCONFIG(TAG,
                "    Multiplexer: %u\n"
                "    Gain: %u\n"
                "    Resolution: %u\n"
                "    Sample rate: %u",
                this->multiplexer_, this->gain_, this->resolution_, this->samplerate_);
}

#ifdef USE_ESP32
void ADS1115Sensor::sampling_task_(void *param) {
  auto *sensor = static_cast<ADS1115Sensor *>(param);
  ESP_LOGI(TAG, "Background sampling task started for '%s', waiting for triggers", sensor->get_name().c_str());

  while (sensor->task_running_) {
    // Wait indefinitely for notification from update()
    uint32_t notification_value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (notification_value > 0 && sensor->task_running_) {
      // Perform measurement (will block on burst_mutex_ if RMS measurement in progress)
      float voltage = sensor->parent_->request_measurement(sensor->multiplexer_, sensor->gain_, sensor->resolution_,
                                                           sensor->samplerate_);

      // Store result with mutex protection
      if (sensor->data_mutex_ != nullptr && xSemaphoreTake(sensor->data_mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
        sensor->cached_voltage_ = voltage;
        sensor->new_data_available_ = true;
        xSemaphoreGive(sensor->data_mutex_);
      }
    }
  }

  ESP_LOGI(TAG, "Background sampling task stopped for '%s'", sensor->get_name().c_str());
  vTaskDelete(nullptr);
}
#endif

}  // namespace ads1115
}  // namespace esphome
