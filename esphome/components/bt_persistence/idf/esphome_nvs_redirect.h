#pragma once

// Included after nvs.h: the NVS calls of the including file go to the ESPHome store.

#include "osi/esphome_nvs_store.h"

#define nvs_open esphome_nvs_open
#define nvs_close esphome_nvs_close
#define nvs_get_blob esphome_nvs_get_blob
#define nvs_set_blob esphome_nvs_set_blob
#define nvs_erase_key esphome_nvs_erase_key
#define nvs_erase_all esphome_nvs_erase_all
#define nvs_commit esphome_nvs_commit
