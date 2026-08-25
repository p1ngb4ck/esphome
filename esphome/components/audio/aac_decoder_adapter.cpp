#include "aac_decoder_adapter.h"

#ifdef USE_ESP32
#ifdef USE_AUDIO_AAC_SUPPORT

#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

// ESP-IDF AAC decoder registration (from esp_aac_dec.h in esp_audio_codec component)
extern "C" {
extern esp_audio_err_t esp_aac_dec_register();
}

namespace esphome {
namespace audio {
namespace esp_audio_codec_adapter {

static const char *const TAG = "aac_decoder_adapter";

AACDecoder::AACDecoder() {
  // Register AAC decoder with esp_audio_codec framework (must be done before first use)
  static bool registered = false;
  if (!registered) {
    esp_audio_err_t reg_err = esp_aac_dec_register();
    if (reg_err != ESP_AUDIO_ERR_OK) {
      ESP_LOGE(TAG, "Failed to register AAC decoder: %d", reg_err);
      return;
    }
    ESP_LOGI(TAG, "AAC decoder registered with esp_audio_codec");
    registered = true;
  }

  // Configure AAC decoder using esp_audio_codec API
  esp_audio_dec_cfg_t config = {};
  config.type = ESP_AUDIO_TYPE_AAC;
  config.cfg = nullptr;  // Use default AAC configuration
  config.cfg_sz = 0;

  esp_audio_err_t err = esp_audio_dec_open(&config, &this->decoder_);

  if (err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open AAC decoder: %d", err);
    this->decoder_ = nullptr;
    return;
  }

  this->initialized_ = true;
  ESP_LOGI(TAG, "AAC decoder adapter initialized");
}

AACDecoder::~AACDecoder() {
  if (this->decoder_) {
    esp_audio_dec_close(this->decoder_);
    this->decoder_ = nullptr;
  }
}

AACDecodeResult AACDecoder::decode_frame(const uint8_t *input, size_t input_len, int16_t *output) {
  AACDecodeResult result = {AAC_DECODER_ERROR, 0, 0, 0, 0};

  if (!this->initialized_ || !input || !output) {
    return result;
  }

  // Prepare input data structure
  esp_audio_dec_in_raw_t raw_input = {};
  raw_input.buffer = const_cast<uint8_t *>(input);
  raw_input.len = input_len;
  raw_input.consumed = 0;

  // Prepare output data structure
  // AAC typical max frame size: 1024 samples * 2 channels * 2 bytes = 4096 bytes
  esp_audio_dec_out_frame_t frame_output = {};
  frame_output.buffer = reinterpret_cast<uint8_t *>(output);
  frame_output.len = 4096;  // Max buffer size available
  frame_output.decoded_size = 0;
  frame_output.needed_size = 0;

  // Decode the frame
  esp_audio_err_t err = esp_audio_dec_process(this->decoder_, &raw_input, &frame_output);

  if (err == ESP_AUDIO_ERR_OK || err == ESP_AUDIO_ERR_CONTINUE) {
    result.bytes_consumed = raw_input.consumed;
    result.output_samples = frame_output.decoded_size / sizeof(int16_t);

    // Get decoder info (sample rate, channels)
    esp_audio_dec_info_t info = {};
    if (esp_audio_dec_get_info(this->decoder_, &info) == ESP_AUDIO_ERR_OK) {
      result.sample_rate = info.sample_rate;
      result.channels = info.channel;

      // Cache for future calls
      this->last_sample_rate_ = info.sample_rate;
      this->last_channels_ = info.channel;
    } else {
      // Use cached values
      result.sample_rate = this->last_sample_rate_;
      result.channels = this->last_channels_;
    }

    result.status = AAC_DECODER_SUCCESS;
  } else if (err == ESP_AUDIO_ERR_DATA_LACK) {
    result.status = AAC_DECODER_OUT_OF_DATA;
    result.bytes_consumed = raw_input.consumed;
  } else {
    result.status = AAC_DECODER_ERROR;
  }

  return result;
}

}  // namespace esp_audio_codec_adapter
}  // namespace audio
}  // namespace esphome

#endif  // USE_AUDIO_AAC_SUPPORT
#endif  // USE_ESP32
