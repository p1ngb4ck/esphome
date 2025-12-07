#include "simple_video_player.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <algorithm>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "hal/color_types.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player";

SimpleVideoPlayer::~SimpleVideoPlayer() {}

void SimpleVideoPlayer::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Simple Video Playerr...");
  // Verify transcoder is available
  if (this->transcoder_ == nullptr) {
    ESP_LOGE(TAG, "Transcoder component not set");
    this->mark_failed();
    return;
  }
  // Verify JPEG decoder is available in transcoder
  if (!this->transcoder_->is_jpeg_decoder_available()) {
    ESP_LOGE(TAG, "JPEG decoder not available in transcoder");
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "Simple Video Player setup complete");
}

void SimpleVideoPlayer::loop() { uint32_t now = millis(); }

void SimpleVideoPlayer::dump_config() { ESP_LOGCONFIG(TAG, "Simple Video Player :"); }

}  // namespace esphome::simple_video_player
