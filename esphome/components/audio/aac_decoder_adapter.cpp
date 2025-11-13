#include "aac_decoder_adapter.h"

#ifdef USE_ESP32
#ifdef USE_AUDIO_AAC_SUPPORT

#include "esphome/core/defines.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

// Include esp_audio_codec headers here (not in .h to avoid compilation issues)
extern "C" {
#include <aac_decoder.h>
#include <audio_element.h>
#include <audio_pipeline.h>
}

namespace esphome {
namespace audio {
namespace esp_audio_codec_adapter {

static const char *const TAG = "aac_decoder_adapter";

static const size_t INPUT_RINGBUF_SIZE = 8192;
static const size_t OUTPUT_RINGBUF_SIZE = 8192;

AACDecoder::AACDecoder() {
  // Create ring buffers for audio_element communication
  this->input_rb_ = rb_create(INPUT_RINGBUF_SIZE, 1);
  this->output_rb_ = rb_create(OUTPUT_RINGBUF_SIZE, 1);

  if (!this->input_rb_ || !this->output_rb_) {
    ESP_LOGE(TAG, "Failed to create ring buffers");
    return;
  }

  // Create audio pipeline
  audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
  this->pipeline_ = audio_pipeline_init(&pipeline_cfg);

  if (!this->pipeline_) {
    ESP_LOGE(TAG, "Failed to create audio pipeline");
    return;
  }

  // Configure AAC decoder
  aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
  aac_cfg.out_rb_size = OUTPUT_RINGBUF_SIZE;
  aac_cfg.task_stack = 8 * 1024;  // Increase stack size for AAC decoding

  this->decoder_ = aac_decoder_init(&aac_cfg);

  if (!this->decoder_) {
    ESP_LOGE(TAG, "Failed to create AAC decoder");
    return;
  }

  // Set ring buffers
  audio_element_set_input_ringbuf(this->decoder_, this->input_rb_);
  audio_element_set_output_ringbuf(this->decoder_, this->output_rb_);

  // Register decoder in pipeline
  audio_pipeline_register(this->pipeline_, this->decoder_, "aac");

  // Link elements (single element pipeline)
  audio_pipeline_link(this->pipeline_, (const char *[]){"aac"}, 1);

  // Start pipeline
  audio_pipeline_run(this->pipeline_);

  this->initialized_ = true;
  ESP_LOGI(TAG, "AAC decoder adapter initialized");
}

AACDecoder::~AACDecoder() {
  if (this->pipeline_) {
    audio_pipeline_stop(this->pipeline_);
    audio_pipeline_wait_for_stop(this->pipeline_);
    audio_pipeline_terminate(this->pipeline_);

    if (this->decoder_) {
      audio_pipeline_unregister(this->pipeline_, this->decoder_);
    }

    audio_pipeline_deinit(this->pipeline_);
    this->pipeline_ = nullptr;
  }

  if (this->decoder_) {
    audio_element_deinit(this->decoder_);
    this->decoder_ = nullptr;
  }

  if (this->input_rb_) {
    rb_destroy(this->input_rb_);
    this->input_rb_ = nullptr;
  }

  if (this->output_rb_) {
    rb_destroy(this->output_rb_);
    this->output_rb_ = nullptr;
  }
}

AACDecodeResult AACDecoder::decode_frame(const uint8_t *input, size_t input_len, int16_t *output) {
  AACDecodeResult result = {AAC_DECODER_ERROR, 0, 0, 0, 0};

  if (!this->initialized_ || !input || !output) {
    return result;
  }

  // Write input data to ring buffer
  int bytes_written = rb_write(this->input_rb_, (char *) input, input_len, pdMS_TO_TICKS(10));

  if (bytes_written <= 0) {
    result.status = AAC_DECODER_OUT_OF_DATA;
    return result;
  }

  result.bytes_consumed = bytes_written;

  // Give decoder time to process
  delay(5);

  // Read decoded PCM data from output ring buffer
  int bytes_read = rb_read(this->output_rb_, (char *) output, OUTPUT_RINGBUF_SIZE, pdMS_TO_TICKS(10));

  if (bytes_read > 0) {
    result.output_samples = bytes_read / sizeof(int16_t);

    // Try to get audio info from the decoder element
    audio_element_info_t info = {};
    if (audio_element_getinfo(this->decoder_, &info) == ESP_OK) {
      result.sample_rate = info.sample_rates;
      result.channels = info.channels;

      // Cache for future calls
      this->last_sample_rate_ = info.sample_rates;
      this->last_channels_ = info.channels;
    } else {
      // Use cached values
      result.sample_rate = this->last_sample_rate_;
      result.channels = this->last_channels_;
    }

    result.status = AAC_DECODER_SUCCESS;
  } else if (bytes_read == 0) {
    result.status = AAC_DECODER_OUT_OF_DATA;
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
