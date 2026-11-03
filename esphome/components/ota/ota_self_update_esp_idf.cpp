#include "ota_backend_esp_idf.h"

#if defined(USE_ESP32) && defined(USE_OTA_SELF_UPDATE) && defined(USE_OTA_PARTITIONS)
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_app_desc.h>
#include <esp_flash.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_private/esp_flash_internal.h>
#include <spi_flash_mmap.h>

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstring>

namespace esphome::ota {

ESPHOME_LOG_TAG(TAG, "ota.self");

static constexpr size_t APP_DESC_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
static constexpr size_t WRITE_BLOCK = 16 * SPI_FLASH_SEC_SIZE;

void PsramFree::operator()(uint8_t *data) const {
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(data, 0);
}

bool is_app_image(const uint8_t *head, size_t len) {
  if (len < APP_DESC_OFFSET + sizeof(uint32_t) || head[0] != ESP_IMAGE_HEADER_MAGIC)
    return false;
  uint32_t magic;
  memcpy(&magic, head + APP_DESC_OFFSET, sizeof(magic));
  return magic == ESP_APP_DESC_MAGIC_WORD;
}

OTAResponseTypes IDFOTABackend::self_image_begin_() {
#ifndef CONFIG_SPIRAM_XIP_FROM_PSRAM
  ESP_LOGE(TAG, "Updating the running firmware needs execute_from_psram");
  return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
#else
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (this->pending_total_ == 0 || this->pending_total_ > running->size) {
    ESP_LOGE(TAG, "Firmware image of %zu bytes does not fit its partition (%" PRIu32 " bytes)", this->pending_total_,
             running->size);
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
  uint8_t *buffer = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(this->pending_total_);
  if (buffer == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the firmware image (%zu bytes)", this->pending_total_);
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
  this->self_image_.reset(buffer);
  this->self_have_ = 0;
  ESP_LOGI(TAG, "Firmware image (%zu bytes): received into PSRAM before it is written", this->pending_total_);
  return this->self_store_(this->head_buf_, this->head_have_);
#endif
}

OTAResponseTypes IDFOTABackend::self_store_(const uint8_t *data, size_t len) {
  if (len > this->pending_total_ - this->self_have_)
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  memcpy(this->self_image_.get() + this->self_have_, data, len);
  this->self_have_ += len;
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::self_image_finish_() {
  std::unique_ptr<uint8_t[], PsramFree> image = std::move(this->self_image_);
  const size_t size = this->self_have_;
  this->self_have_ = 0;
  if (size != this->pending_total_ || !is_app_image(image.get(), size)) {
    ESP_LOGE(TAG, "Firmware image incomplete (%zu of %zu bytes)", size, this->pending_total_);
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  const size_t erase_size = (size + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
  esp_err_t err = ESP_OK;
  {
    watchdog::WatchdogManager watchdog(15000 + (erase_size >> 10) * 10);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    int reported = -1;
    for (size_t at = 0; at < erase_size && err == ESP_OK; at += WRITE_BLOCK) {
      const size_t block = std::min<size_t>(WRITE_BLOCK, erase_size - at);
      err = esp_partition_erase_range(running, at, block);
      if (err == ESP_OK)
        err = esp_partition_write(running, at, image.get() + at, std::min<size_t>(block, size - at));
      const int percent = static_cast<int>(static_cast<uint64_t>(at + block) * 100 / erase_size);
      if (err == ESP_OK && percent != reported && this->write_progress_ != nullptr) {
        reported = percent;
        this->write_progress_(this->write_progress_context_, static_cast<float>(percent));
      }
    }
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Writing the firmware failed (err=0x%X)", err);
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  const esp_partition_pos_t position = {.offset = running->address, .size = running->size};
  esp_image_metadata_t metadata{};
  err = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The written firmware does not verify (err=0x%X)", err);
    return OTA_RESPONSE_ERROR_UPDATE_END;
  }
  ESP_LOGI(TAG, "Firmware updated");
  return OTA_RESPONSE_OK;
}

}

#endif
