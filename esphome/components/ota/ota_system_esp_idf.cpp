#include "ota_system_esp_idf.h"

#if defined(USE_ESP32) && (defined(USE_OTA_REBOOT_TO_SYSTEM) || defined(USE_OTA_REBOOT_TO_APP))
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace esphome::ota {

ESPHOME_LOG_TAG(TAG, "ota.system");

#ifdef USE_OTA_REBOOT_TO_SYSTEM
bool reboot_to_system(bool safe) {
  const esp_partition_t *system =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (system == nullptr) {
    ESP_LOGE(TAG, "No OTA system partition");
    return false;
  }
  esp_err_t err = esp_ota_set_boot_partition(system);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Setting the OTA system as boot partition failed (err=0x%X)", err);
    return false;
  }
  ESP_LOGI(TAG, "Rebooting into the OTA system");
  if (safe) {
    App.safe_reboot();
  } else {
    App.reboot();
  }
  return true;
}
#endif

#ifdef USE_OTA_REBOOT_TO_APP
bool reboot_to_app() {
  const esp_partition_t *app =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (app == nullptr) {
    ESP_LOGE(TAG, "No main firmware partition");
    return false;
  }
  esp_err_t err = esp_ota_set_boot_partition(app);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Setting the main firmware as boot partition failed (err=0x%X)", err);
    return false;
  }
  ESP_LOGI(TAG, "Rebooting into the main firmware");
  App.safe_reboot();
  return true;
}
#endif

}

#endif
