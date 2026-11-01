#pragma once

// In-RAM stand-in for the NVS calls Bluedroid makes. Lives in ESPHome's copy of the bt
// component; ESPHome loads it from and saves it to a file.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esphome_nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle);
void esphome_nvs_close(nvs_handle_t handle);
esp_err_t esphome_nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length);
esp_err_t esphome_nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length);
esp_err_t esphome_nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t esphome_nvs_erase_all(nvs_handle_t handle);
esp_err_t esphome_nvs_commit(nvs_handle_t handle);

// Until a load succeeded every call above answers ESP_ERR_NVS_NOT_INITIALIZED.
bool esphome_nvs_store_init(void);
// Replaces the whole content; len 0 is an empty store. False on a damaged image.
bool esphome_nvs_store_load(const uint8_t *data, size_t len);
// Bumped on every change.
uint32_t esphome_nvs_store_generation(void);
// Writes the image into buf. False with *len set to the size needed when cap is too small.
bool esphome_nvs_store_serialize(uint8_t *buf, size_t cap, size_t *len, uint32_t *generation);

#ifdef __cplusplus
}
#endif
