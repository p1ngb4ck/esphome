#include "ota_backend_esp_idf.h"

#if defined(USE_ESP32) && defined(USE_OTA_SYSTEM_SELF_UPDATE) && defined(USE_OTA_PARTITIONS)
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_app_desc.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <spi_flash_mmap.h>

#include <cinttypes>
#include <cstddef>
#include <cstring>

namespace esphome::ota {

ESPHOME_LOG_TAG(TAG, "ota.system");

static constexpr size_t APP_DESC_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
static constexpr size_t APP_DESC_VERSION_OFFSET = APP_DESC_OFFSET + offsetof(esp_app_desc_t, version);
static constexpr char SYSTEM_IMAGE_MARKER[] = "ota-system";

void PsramFree::operator()(uint8_t *data) const {
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(data, 0);
}

bool is_system_image(const uint8_t *head, size_t len) {
  if (len < APP_DESC_VERSION_OFFSET + sizeof(SYSTEM_IMAGE_MARKER) || head[0] != ESP_IMAGE_HEADER_MAGIC)
    return false;
  uint32_t magic;
  memcpy(&magic, head + APP_DESC_OFFSET, sizeof(magic));
  return magic == ESP_APP_DESC_MAGIC_WORD &&
         memcmp(head + APP_DESC_VERSION_OFFSET, SYSTEM_IMAGE_MARKER, sizeof(SYSTEM_IMAGE_MARKER)) == 0;
}

OTAResponseTypes IDFOTABackend::system_image_begin_() {
#ifndef CONFIG_SPIRAM_XIP_FROM_PSRAM
  ESP_LOGE(TAG, "Updating the OTA system itself needs execute_from_psram");
  return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
#else
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (this->pending_total_ == 0 || this->pending_total_ > running->size) {
    ESP_LOGE(TAG, "OTA system image of %zu bytes does not fit its partition (%" PRIu32 " bytes)", this->pending_total_,
             running->size);
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
  uint8_t *buffer = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(this->pending_total_);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the OTA system image (%zu bytes)", this->pending_total_);
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
  this->system_image_.reset(buffer);
  this->system_have_ = 0;
  ESP_LOGI(TAG, "OTA system image (%zu bytes): received into PSRAM before it is written", this->pending_total_);
  return this->system_store_(this->head_buf_, this->head_have_);
#endif
}

OTAResponseTypes IDFOTABackend::system_store_(const uint8_t *data, size_t len) {
  if (len > this->pending_total_ - this->system_have_)
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  memcpy(this->system_image_.get() + this->system_have_, data, len);
  this->system_have_ += len;
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::system_image_finish_() {
  std::unique_ptr<uint8_t[], PsramFree> image = std::move(this->system_image_);
  const size_t size = this->system_have_;
  this->system_have_ = 0;
  if (size != this->pending_total_ || !is_system_image(image.get(), size)) {
    ESP_LOGE(TAG, "OTA system image incomplete (%zu of %zu bytes)", size, this->pending_total_);
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  const size_t erase_size = (size + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
  esp_err_t err;
  {
    watchdog::WatchdogManager watchdog(15000 + (erase_size >> 10) * 10);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    err = esp_partition_erase_range(running, 0, erase_size);
    if (err == ESP_OK)
      err = esp_partition_write(running, 0, image.get(), size);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Writing the OTA system failed (err=0x%X)", err);
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  const esp_partition_pos_t position = {.offset = running->address, .size = running->size};
  esp_image_metadata_t metadata{};
  err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The written OTA system does not verify (err=0x%X)", err);
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  ESP_LOGI(TAG, "OTA system updated");
  return OTA_RESPONSE_OK;
}

}

#endif
