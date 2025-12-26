#include "ads1115.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ads1115 {

static const char *const TAG = "ads1115";
static const uint8_t ADS1115_REGISTER_CONVERSION = 0x00;
static const uint8_t ADS1115_REGISTER_CONFIG = 0x01;

void ADS1115Component::setup() {
  uint16_t value;
  if (!this->read_byte_16(ADS1115_REGISTER_CONVERSION, &value)) {
    this->mark_failed();
    return;
  }

  uint16_t config = 0;
  // Clear single-shot bit
  //        0b0xxxxxxxxxxxxxxx
  config |= 0b0000000000000000;
  // Setup multiplexer
  //        0bx000xxxxxxxxxxxx
  config |= ADS1115_MULTIPLEXER_P0_N1 << 12;

  // Setup Gain
  //        0bxxxx000xxxxxxxxx
  config |= ADS1115_GAIN_6P144 << 9;

  if (this->continuous_mode_) {
    // Set continuous mode
    //        0bxxxxxxx0xxxxxxxx
    config |= 0b0000000000000000;
  } else {
    // Set singleshot mode
    //        0bxxxxxxx1xxxxxxxx
    config |= 0b0000000100000000;
  }

  // Set data rate - 860 samples per second
  //        0bxxxxxxxx100xxxxx
  config |= ADS1115_860SPS << 5;

  // Set comparator mode - hysteresis
  //        0bxxxxxxxxxxx0xxxx
  config |= 0b0000000000000000;

  // Set comparator polarity - active low
  //        0bxxxxxxxxxxxx0xxx
  config |= 0b0000000000000000;

  // Set comparator latch enabled - false
  //        0bxxxxxxxxxxxxx0xx
  config |= 0b0000000000000000;

  // Set comparator que mode - disabled
  //        0bxxxxxxxxxxxxxx11
  config |= 0b0000000000000011;

  if (!this->write_byte_16(ADS1115_REGISTER_CONFIG, config)) {
    this->mark_failed();
    return;
  }
  this->prev_config_ = config;

#ifdef USE_ESP32
  // Create mutex for managing channel tasks
  this->tasks_mutex_ = xSemaphoreCreateMutex();
#endif
}
void ADS1115Component::dump_config() {
  ESP_LOGCONFIG(TAG, "ADS1115:");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, ESP_LOG_MSG_COMM_FAIL);
  }
}

void ADS1115Component::loop() {
  // No longer needed - per-channel tasks handle everything
}

#ifdef USE_ESP32
void ADS1115Component::channel_task_func_(void *param) {
  ChannelTask *task = static_cast<ChannelTask *>(param);
  ADS1115Component *parent = task->parent;

  while (true) {
    MeasurementRequest req;
    bool has_request = false;

    // Check for pending requests
    if (xSemaphoreTake(task->queue_mutex, portMAX_DELAY) == pdTRUE) {
      if (!task->request_queue.empty()) {
        req = task->request_queue.front();
        task->request_queue.pop();
        has_request = true;
      }
      xSemaphoreGive(task->queue_mutex);
    }

    if (has_request) {
      // Check if this is a burst mode request
      if (req.is_burst) {
        task->in_burst_mode = true;
      }

      // Perform measurement (I2C bus has its own locking)
      float result = parent->do_measurement_(req.multiplexer, req.gain, req.resolution, req.samplerate);

      // Handle result delivery
      if (req.callback) {
        // Asynchronous request with callback
        req.callback(result);
      }

      if (req.result_ptr != nullptr) {
        // Synchronous request - store result and signal completion
        *req.result_ptr = result;
      }

      if (req.completion_sem != nullptr) {
        // Signal that measurement is complete
        xSemaphoreGive(req.completion_sem);

        // If burst mode, wait briefly to allow next sample to queue before clearing flag
        if (req.is_burst) {
          vTaskDelay(pdMS_TO_TICKS(1));
          // Check if queue is empty - if yes, burst is complete
          bool queue_empty = false;
          if (xSemaphoreTake(task->queue_mutex, portMAX_DELAY) == pdTRUE) {
            queue_empty = task->request_queue.empty();
            xSemaphoreGive(task->queue_mutex);
          }
          if (queue_empty) {
            task->in_burst_mode = false;
          }
        }
      }
    } else {
      // No requests, exit burst mode and yield to other tasks
      task->in_burst_mode = false;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void ADS1115Component::ensure_channel_task_(ADS1115Multiplexer multiplexer) {
  uint8_t channel = static_cast<uint8_t>(multiplexer);

  if (xSemaphoreTake(this->tasks_mutex_, portMAX_DELAY) == pdTRUE) {
    // Check if task already exists
    if (this->channel_tasks_.find(channel) == this->channel_tasks_.end()) {
      // Create new channel task
      ChannelTask *task = new ChannelTask();
      task->channel = multiplexer;
      task->parent = this;
      task->queue_mutex = xSemaphoreCreateMutex();

      // Create FreeRTOS task for this channel
      char task_name[32];
      snprintf(task_name, sizeof(task_name), "ads1115_ch%d", channel);

      xTaskCreatePinnedToCore(channel_task_func_, task_name, 4096, task, 1, &task->task_handle, 1);

      this->channel_tasks_[channel] = task;
      ESP_LOGD(TAG, "Created background task for channel %d", channel);
    }
    xSemaphoreGive(this->tasks_mutex_);
  }
}
#endif

float ADS1115Component::request_measurement(ADS1115Multiplexer multiplexer, ADS1115Gain gain,
                                            ADS1115Resolution resolution, ADS1115Samplerate samplerate) {
#ifdef USE_ESP32
  // Ensure background task exists for this channel
  this->ensure_channel_task_(multiplexer);

  uint8_t channel = static_cast<uint8_t>(multiplexer);
  ChannelTask *task = this->channel_tasks_[channel];

  // Create semaphore for synchronous wait
  SemaphoreHandle_t completion_sem = xSemaphoreCreateBinary();
  float result = NAN;

  // Add request to channel's queue
  MeasurementRequest req{multiplexer, gain, resolution, samplerate, nullptr, millis()};
  req.completion_sem = completion_sem;
  req.result_ptr = &result;

  // Detect burst mode: if task is already in burst mode OR queue has pending requests, mark as burst
  req.is_burst = task->in_burst_mode || !task->request_queue.empty();

  if (xSemaphoreTake(task->queue_mutex, portMAX_DELAY) == pdTRUE) {
    task->request_queue.push(req);
    xSemaphoreGive(task->queue_mutex);
  }

  // Wait for measurement to complete (non-blocking for the main loop, blocking for this caller)
  if (xSemaphoreTake(completion_sem, portMAX_DELAY) == pdTRUE) {
    // Result is now in 'result' variable
  }

  vSemaphoreDelete(completion_sem);
  return result;
#else
  // Non-ESP32: Direct measurement
  return this->do_measurement_(multiplexer, gain, resolution, samplerate);
#endif
}

#ifdef USE_ESP32
void ADS1115Component::start_burst_mode(ADS1115Multiplexer multiplexer) {
  this->ensure_channel_task_(multiplexer);
  uint8_t channel = static_cast<uint8_t>(multiplexer);
  ChannelTask *task = this->channel_tasks_[channel];
  task->in_burst_mode = true;
}

void ADS1115Component::end_burst_mode(ADS1115Multiplexer multiplexer) {
  uint8_t channel = static_cast<uint8_t>(multiplexer);
  if (this->channel_tasks_.find(channel) != this->channel_tasks_.end()) {
    this->channel_tasks_[channel]->in_burst_mode = false;
  }
}
#endif

float ADS1115Component::do_measurement_(ADS1115Multiplexer multiplexer, ADS1115Gain gain, ADS1115Resolution resolution,
                                        ADS1115Samplerate samplerate) {
  uint16_t config = this->prev_config_;
  // Multiplexer
  //        0bxBBBxxxxxxxxxxxx
  config &= 0b1000111111111111;
  config |= (multiplexer & 0b111) << 12;

  // Gain
  //        0bxxxxBBBxxxxxxxxx
  config &= 0b1111000111111111;
  config |= (gain & 0b111) << 9;

  // Sample rate
  //        0bxxxxxxxxBBBxxxxx
  config &= 0b1111111100011111;
  config |= (samplerate & 0b111) << 5;

  if (!this->continuous_mode_) {
    // Start conversion
    config |= 0b1000000000000000;
  }

  if (!this->continuous_mode_ || this->prev_config_ != config) {
    if (!this->write_byte_16(ADS1115_REGISTER_CONFIG, config)) {
      this->status_set_warning();
      return NAN;
    }
    this->prev_config_ = config;

    // Delay calculated as: ceil((1000/SPS)+.5)
    if (resolution == ADS1015_12_BITS) {
      switch (samplerate) {
        case ADS1115_8SPS:
          delay(9);
          break;
        case ADS1115_16SPS:
          delay(5);
          break;
        case ADS1115_32SPS:
          delay(3);
          break;
        case ADS1115_64SPS:
        case ADS1115_128SPS:
          delay(2);
          break;
        default:
          delay(1);
          break;
      }
    } else {
      switch (samplerate) {
        case ADS1115_8SPS:
          delay(126);  // NOLINT
          break;
        case ADS1115_16SPS:
          delay(63);  // NOLINT
          break;
        case ADS1115_32SPS:
          delay(32);
          break;
        case ADS1115_64SPS:
          delay(17);
          break;
        case ADS1115_128SPS:
          delay(9);
          break;
        case ADS1115_250SPS:
          delay(5);
          break;
        case ADS1115_475SPS:
          delay(3);
          break;
        case ADS1115_860SPS:
          delay(2);
          break;
      }
    }

    // in continuous mode, conversion will always be running, rely on the delay
    // to ensure conversion is taking place with the correct settings
    // can we use the rdy pin to trigger when a conversion is done?
    if (!this->continuous_mode_) {
      uint32_t start = millis();
      while (this->read_byte_16(ADS1115_REGISTER_CONFIG, &config) && (config >> 15) == 0) {
        if (millis() - start > 100) {
          ESP_LOGW(TAG, "Reading ADS1115 timed out");
          this->status_set_warning();
          return NAN;
        }
        yield();
      }
    }
  }

  uint16_t raw_conversion;
  if (!this->read_byte_16(ADS1115_REGISTER_CONVERSION, &raw_conversion)) {
    this->status_set_warning();
    return NAN;
  }

  if (resolution == ADS1015_12_BITS) {
    bool negative = (raw_conversion >> 15) == 1;

    // shift raw_conversion as it's only 12-bits, left justified
    raw_conversion = raw_conversion >> (16 - ADS1015_12_BITS);

    // check if number was negative in order to keep the sign
    if (negative) {
      // the number was negative
      // 1) set the negative bit back
      raw_conversion |= 0x8000;
      // 2) reset the former (shifted) negative bit
      raw_conversion &= 0xF7FF;
    }
  }

  auto signed_conversion = static_cast<int16_t>(raw_conversion);

  float millivolts;
  float divider = (resolution == ADS1115_16_BITS) ? 32768.0f : 2048.0f;
  switch (gain) {
    case ADS1115_GAIN_6P144:
      millivolts = (signed_conversion * 6144) / divider;
      break;
    case ADS1115_GAIN_4P096:
      millivolts = (signed_conversion * 4096) / divider;
      break;
    case ADS1115_GAIN_2P048:
      millivolts = (signed_conversion * 2048) / divider;
      break;
    case ADS1115_GAIN_1P024:
      millivolts = (signed_conversion * 1024) / divider;
      break;
    case ADS1115_GAIN_0P512:
      millivolts = (signed_conversion * 512) / divider;
      break;
    case ADS1115_GAIN_0P256:
      millivolts = (signed_conversion * 256) / divider;
      break;
    default:
      millivolts = NAN;
  }

  this->status_clear_warning();
  return millivolts / 1e3f;
}

}  // namespace ads1115
}  // namespace esphome
