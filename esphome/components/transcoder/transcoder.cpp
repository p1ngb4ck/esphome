#include "transcoder.h"

namespace esphome {
namespace transcoder {

static const char *const TAG = "transcoder";

// Global transcoder instance
Transcoder *global_transcoder = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

Transcoder::Transcoder() {
  global_transcoder = this;

#ifdef USE_HARDWARE_JPEG_DECODER
  // Create mutex for JPEG decoder exclusive access
  this->jpeg_decoder_mutex_ = xSemaphoreCreateMutex();
#endif
}

void Transcoder::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Transcoder...");

  // Note: JPEG decoder now uses lazy initialization (created on first use)
  // This matches the old storage_image behavior and ensures hardware is ready
#ifdef TRANSCODER_ENABLE_JPEG_DECODER
#ifdef USE_ESP_NEW_JPEG_DECODER
  // ESP32-S2/S3: esp_new_jpeg decoder will be lazily initialized on first use
  ESP_LOGI(TAG, "ESP_NEW_JPEG decoder ready for lazy initialization (ESP32-S2/S3)");
#endif
#endif

  // Initialize JPEG Encoder (only if requested by components)
#ifdef TRANSCODER_ENABLE_JPEG_ENCODER
#ifdef USE_HARDWARE_JPEG_ENCODER
  // ESP32-P4: Hardware JPEG encoder
  jpeg_encode_engine_cfg_t encode_eng_cfg = {};
  encode_eng_cfg.intr_priority = 0;
  encode_eng_cfg.timeout_ms = 200;

  esp_err_t ret = jpeg_new_encoder_engine(&encode_eng_cfg, &this->jpeg_encoder_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create hardware JPEG encoder: %s", esp_err_to_name(ret));
    // Don't mark as failed - encoder is optional
  } else {
    ESP_LOGI(TAG, "Hardware JPEG encoder initialized (ESP32-P4)");
  }
#endif
#endif

  // Initialize H.264 Decoder (only if requested by components)
#ifdef TRANSCODER_ENABLE_H264_DECODER
#ifdef USE_ESP_H264_DECODER
  // ESP32-P4/S3: Software H.264 decoder (TinyH264)
  // Using esp_h264 v1.1.2 from ESP Component Registry
  esp_h264_dec_cfg_sw_t dec_config = {};
  dec_config.pic_type = ESP_H264_RAW_FMT_I420;

  esp_h264_err_t h264_ret = esp_h264_dec_sw_new(&dec_config, &this->h264_decoder_);
  if (h264_ret != ESP_H264_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create H.264 decoder: %d", h264_ret);
    // Don't mark as failed - decoder is optional
  } else {
    h264_ret = esp_h264_dec_open(this->h264_decoder_);
    if (h264_ret != ESP_H264_ERR_OK) {
      ESP_LOGE(TAG, "Failed to open H.264 decoder: %d", h264_ret);
      esp_h264_dec_del(this->h264_decoder_);
      this->h264_decoder_ = nullptr;
    } else {
      ESP_LOGI(TAG, "H.264 decoder initialized (software TinyH264)");
    }
  }
#endif
#endif

  // Initialize H.264 Encoder (only if requested by components)
#ifdef TRANSCODER_ENABLE_H264_ENCODER
#ifdef USE_ESP_H264_ENCODER
  // ESP32-P4: Hardware encoder
  // ESP32-S3: Software encoder
  // Using esp_h264 v1.1.2 from ESP Component Registry
  esp_h264_enc_config_t enc_config = {};
  enc_config.width = 640;  // Default resolution (will be reconfigured by component)
  enc_config.height = 480;
  enc_config.fps = 30;
  enc_config.gop = 30;
  enc_config.bitrate = 1000000;  // 1 Mbps
  enc_config.timeout = 200;

  ret = esp_h264_enc_open(&enc_config, &this->h264_encoder_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create H.264 encoder: %s", esp_err_to_name(ret));
    // Don't mark as failed - encoder is optional
  } else {
    ESP_LOGI(TAG, "H.264 encoder initialized");
  }
#endif
#endif

  ESP_LOGCONFIG(TAG, "Transcoder setup complete");
}

// Lazy initialization for JPEG decoder (matches old storage_image behavior)
#ifdef USE_HARDWARE_JPEG_DECODER
jpeg_decoder_handle_t Transcoder::get_jpeg_decoder() {
  // Return existing decoder if already initialized
  if (this->jpeg_decoder_ != nullptr) {
    return this->jpeg_decoder_;
  }

  // Initialize decoder on first use (after all hardware is ready)
  ESP_LOGI(TAG, "Lazy-initializing hardware JPEG decoder (ESP32-P4)...");

  jpeg_decode_engine_cfg_t decode_eng_cfg = {};
  decode_eng_cfg.intr_priority = 0;
  decode_eng_cfg.timeout_ms = 200;

  esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &this->jpeg_decoder_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create hardware JPEG decoder: %s", esp_err_to_name(ret));
    return nullptr;
  }

  ESP_LOGI(TAG, "Hardware JPEG decoder initialized successfully (handle: %p)", this->jpeg_decoder_);
  return this->jpeg_decoder_;
}

void Transcoder::release_jpeg_decoder() {
  if (this->jpeg_decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->jpeg_decoder_);
    this->jpeg_decoder_ = nullptr;
    ESP_LOGD(TAG, "Hardware JPEG decoder released (workaround for ESP32-P4 state corruption bug)");
  }
}

bool Transcoder::acquire_jpeg_decoder_exclusive(const char *owner_name) {
  if (this->jpeg_decoder_mutex_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder mutex not initialized");
    return false;
  }

  ESP_LOGD(TAG, "Component '%s' waiting to acquire JPEG decoder...", owner_name);

  // Block until we can acquire the mutex
  if (xSemaphoreTake(this->jpeg_decoder_mutex_, portMAX_DELAY) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire JPEG decoder mutex");
    return false;
  }

  // Store owner for debugging
  this->jpeg_decoder_owner_ = owner_name;

  // Initialize decoder if needed
  if (this->jpeg_decoder_ == nullptr) {
    ESP_LOGI(TAG, "Component '%s' initializing JPEG decoder...", owner_name);

    jpeg_decode_engine_cfg_t decode_eng_cfg = {};
    decode_eng_cfg.intr_priority = 0;
    decode_eng_cfg.timeout_ms = 200;

    esp_err_t ret = jpeg_new_decoder_engine(&decode_eng_cfg, &this->jpeg_decoder_);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to create hardware JPEG decoder: %s", esp_err_to_name(ret));
      xSemaphoreGive(this->jpeg_decoder_mutex_);
      this->jpeg_decoder_owner_ = nullptr;
      return false;
    }

    ESP_LOGI(TAG, "JPEG decoder initialized for '%s' (handle: %p)", owner_name, this->jpeg_decoder_);
  } else {
    ESP_LOGD(TAG, "Component '%s' acquired existing JPEG decoder (handle: %p)", owner_name, this->jpeg_decoder_);
  }

  return true;
}

void Transcoder::release_jpeg_decoder_exclusive() {
  if (this->jpeg_decoder_mutex_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder mutex not initialized");
    return;
  }

  const char *previous_owner = this->jpeg_decoder_owner_;
  this->jpeg_decoder_owner_ = nullptr;

  // Delete decoder (workaround for ESP32-P4 state corruption bug)
  if (this->jpeg_decoder_ != nullptr) {
    jpeg_del_decoder_engine(this->jpeg_decoder_);
    this->jpeg_decoder_ = nullptr;
    ESP_LOGD(TAG, "Component '%s' released JPEG decoder", previous_owner ? previous_owner : "unknown");
  }

  // Release mutex so other components can acquire
  xSemaphoreGive(this->jpeg_decoder_mutex_);
}
#endif

// ESP_NEW_JPEG decoder implementation (ESP32-S2/S3)
#ifdef USE_ESP_NEW_JPEG_DECODER
jpeg_dec_handle_t Transcoder::get_esp_new_jpeg_decoder() {
  // Return existing decoder if already initialized
  if (this->esp_new_jpeg_decoder_ != nullptr) {
    return this->esp_new_jpeg_decoder_;
  }

  // Initialize decoder on first use
  ESP_LOGI(TAG, "Lazy-initializing ESP_NEW_JPEG decoder (ESP32-S2/S3)...");

  // Default configuration - will be reconfigured by caller
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();

  jpeg_error_t ret = jpeg_dec_open(&config, &this->esp_new_jpeg_decoder_);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Failed to create ESP_NEW_JPEG decoder: %d", ret);
    return nullptr;
  }

  ESP_LOGI(TAG, "ESP_NEW_JPEG decoder initialized successfully (handle: %p)", this->esp_new_jpeg_decoder_);
  return this->esp_new_jpeg_decoder_;
}

void Transcoder::release_esp_new_jpeg_decoder() {
  if (this->esp_new_jpeg_decoder_ != nullptr) {
    jpeg_dec_close(this->esp_new_jpeg_decoder_);
    this->esp_new_jpeg_decoder_ = nullptr;
    ESP_LOGD(TAG, "ESP_NEW_JPEG decoder released");
  }
}
#endif

void Transcoder::dump_config() {
  ESP_LOGCONFIG(TAG, "Transcoder:");

  // Report JPEG Decoder status (only if enabled)
#ifdef TRANSCODER_ENABLE_JPEG_DECODER
  ESP_LOGCONFIG(TAG, "  JPEG Decoder: %s", this->is_jpeg_decoder_available() ? "Available" : "Not available");
#ifdef USE_HARDWARE_JPEG_DECODER
  ESP_LOGCONFIG(TAG, "    Type: Hardware (ESP32-P4)");
  if (this->jpeg_decoder_) {
    ESP_LOGCONFIG(TAG, "    Handle: %p", this->jpeg_decoder_);
  }
#elif defined(USE_ESP_NEW_JPEG_DECODER)
  ESP_LOGCONFIG(TAG, "    Type: ESP_NEW_JPEG v1.0.0 (ESP32-S2/S3 with SIMD)");
  if (this->esp_new_jpeg_decoder_) {
    ESP_LOGCONFIG(TAG, "    Handle: %p", this->esp_new_jpeg_decoder_);
  }
#elif defined(USE_JPEGDEC)
  ESP_LOGCONFIG(TAG, "    Type: JPEGDec (fallback)");
#endif
#endif

  // Report JPEG Encoder status (only if enabled)
#ifdef TRANSCODER_ENABLE_JPEG_ENCODER
  ESP_LOGCONFIG(TAG, "  JPEG Encoder: %s", this->is_jpeg_encoder_available() ? "Available" : "Not available");
#ifdef USE_HARDWARE_JPEG_ENCODER
  ESP_LOGCONFIG(TAG, "    Type: Hardware (ESP32-P4)");
  if (this->jpeg_encoder_) {
    ESP_LOGCONFIG(TAG, "    Handle: %p", this->jpeg_encoder_);
  }
#endif
#endif

  // Report H.264 Decoder status (only if enabled)
#ifdef TRANSCODER_ENABLE_H264_DECODER
  ESP_LOGCONFIG(TAG, "  H.264 Decoder: %s", this->is_h264_decoder_available() ? "Available" : "Not available");
#ifdef USE_ESP_H264_DECODER
  ESP_LOGCONFIG(TAG, "    Type: esp_h264 v1.1.2 (ESP32-P4/S3)");
  if (this->h264_decoder_) {
    ESP_LOGCONFIG(TAG, "    Handle: %p", this->h264_decoder_);
  }
#endif
#endif

  // Report H.264 Encoder status (only if enabled)
#ifdef TRANSCODER_ENABLE_H264_ENCODER
  ESP_LOGCONFIG(TAG, "  H.264 Encoder: %s", this->is_h264_encoder_available() ? "Available" : "Not available");
#ifdef USE_ESP_H264_ENCODER
  ESP_LOGCONFIG(TAG, "    Type: esp_h264 v1.1.2 (ESP32-P4/S3)");
  if (this->h264_encoder_) {
    ESP_LOGCONFIG(TAG, "    Handle: %p", this->h264_encoder_);
  }
#endif
#endif
}

}  // namespace transcoder
}  // namespace esphome
