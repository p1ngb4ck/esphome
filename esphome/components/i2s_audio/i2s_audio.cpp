#include "i2s_audio.h"

#if defined(USE_ESP32) && (defined(USE_I2S_AUDIO_FULL_DUPLEX) || defined(USE_I2S_AUDIO_NVS))

#include "esphome/core/log.h"
#ifdef USE_I2S_AUDIO_NVS
#include <nvs.h>
#endif

namespace esphome::i2s_audio {

ESPHOME_LOG_TAG(TAG, "i2s_audio");

#ifdef USE_I2S_AUDIO_NVS
static uint8_t claimed_ports = 0;

static int8_t read_nvs_i8(nvs_handle_t handle, const char *key) {
  int8_t value = -1;
  if (key == nullptr || key[0] == '\0' || nvs_get_i8(handle, key, &value) != ESP_OK)
    return -1;
  return value;
}

void I2SAudioComponent::apply_nvs_() {
  if (this->nvs_namespace_ == nullptr)
    return;
  nvs_handle_t handle;
  if (nvs_open(this->nvs_namespace_, NVS_READONLY, &handle) != ESP_OK)
    return;
  const int8_t enable = read_nvs_i8(handle, this->nvs_keys_[0]);
  if (enable >= 0)
    this->enabled_ = enable != 0;
  this->type_ = read_nvs_i8(handle, this->nvs_keys_[1]);
  const int8_t lrclk = read_nvs_i8(handle, this->nvs_keys_[2]);
  const int8_t bclk = read_nvs_i8(handle, this->nvs_keys_[3]);
  const int8_t mclk = read_nvs_i8(handle, this->nvs_keys_[4]);
  this->nvs_dout_pin_ = read_nvs_i8(handle, this->nvs_keys_[5]);
  this->nvs_din_pin_ = read_nvs_i8(handle, this->nvs_keys_[6]);
  this->nvs_enable_pin_ = read_nvs_i8(handle, this->nvs_keys_[7]);
  nvs_close(handle);
  if (lrclk >= 0)
    this->lrclk_pin_ = lrclk;
  if (bclk >= 0)
    this->bclk_pin_ = bclk;
  if (mclk >= 0)
    this->mclk_pin_ = mclk;
}

int I2SAudioComponent::claim_port_() const {
  if (this->port_ >= 0)
    return this->port_;
  if (this->port0_ || this->is_nvs_pdm()) {
    if ((claimed_ports & 1) == 0) {
      claimed_ports |= 1;
      this->port_ = 0;
    }
    return this->port_;
  }
  for (int port = I2S_AUDIO_NVS_PORTS - 1; port >= 0; port--) {
    if ((claimed_ports & (1 << port)) == 0) {
      claimed_ports |= 1 << port;
      this->port_ = port;
      break;
    }
  }
  return this->port_;
}
#endif

void I2SAudioComponent::setup() {
#ifdef USE_I2S_AUDIO_NVS
  this->apply_nvs_();
#endif
#ifdef USE_I2S_AUDIO_FULL_DUPLEX
  if (!this->is_full_duplex())
    return;

  // ESP-IDF only shares the bit and word clocks between channels allocated in the same call, and deleting either
  // channel ends the pairing. Allocate both once here and keep them for the lifetime of the device.
  i2s_chan_config_t chan_cfg{};
  i2s_std_config_t tx_cfg{};
  i2s_std_config_t rx_cfg{};
  this->audio_out_->build_full_duplex_config(chan_cfg, tx_cfg);
  this->audio_in_->build_full_duplex_config(rx_cfg);

  esp_err_t err = i2s_new_channel(&chan_cfg, &this->tx_handle_, &this->rx_handle_);
  if (err == ESP_OK) {
    // The first initialized channel drives the shared clocks. Use RX so the microphone runs on its own.
    err = i2s_channel_init_std_mode(this->rx_handle_, &rx_cfg);
  }
  if (err == ESP_OK) {
    err = i2s_channel_init_std_mode(this->tx_handle_, &tx_cfg);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Full duplex channel setup failed: %s", esp_err_to_name(err));
    if (this->tx_handle_ != nullptr) {
      i2s_del_channel(this->tx_handle_);
      this->tx_handle_ = nullptr;
    }
    if (this->rx_handle_ != nullptr) {
      i2s_del_channel(this->rx_handle_);
      this->rx_handle_ = nullptr;
    }
    this->mark_failed();
  }
#endif
}

#ifdef USE_I2S_AUDIO_FULL_DUPLEX
i2s_chan_handle_t I2SAudioComponent::acquire_(bool &in_use, i2s_chan_handle_t handle) {
  if (handle == nullptr)
    return nullptr;
  in_use = true;
  if (!this->update_rx_channel_()) {
    in_use = false;
    return nullptr;
  }
  return handle;
}

i2s_chan_handle_t I2SAudioComponent::acquire_rx_channel() { return this->acquire_(this->rx_in_use_, this->rx_handle_); }

void I2SAudioComponent::release_rx_channel() {
  this->rx_in_use_ = false;
  this->update_rx_channel_();
}

i2s_chan_handle_t I2SAudioComponent::acquire_tx_channel() {
  // Speakers sharing the bus take turns; a second one must not get the channel while the first is playing
  if (this->tx_in_use_)
    return nullptr;
  return this->acquire_(this->tx_in_use_, this->tx_handle_);
}

void I2SAudioComponent::release_tx_channel() {
  // The speaker task may have left the channel disabled already, so an invalid state error is expected here
  i2s_channel_disable(this->tx_handle_);
  this->tx_in_use_ = false;
  this->update_rx_channel_();
}

bool I2SAudioComponent::update_rx_channel_() {
  const bool needed = this->rx_in_use_ || this->tx_in_use_;
  if (needed == this->rx_enabled_)
    return true;

  esp_err_t err = needed ? i2s_channel_enable(this->rx_handle_) : i2s_channel_disable(this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to %s RX channel: %s", needed ? LOG_STR_LITERAL("enable") : LOG_STR_LITERAL("disable"),
             esp_err_to_name(err));
    return false;
  }
  this->rx_enabled_ = needed;
  return true;
}
#endif

}  // namespace esphome::i2s_audio

#endif
