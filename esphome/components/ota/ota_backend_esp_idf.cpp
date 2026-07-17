#ifdef USE_ESP32
#include "ota_backend_esp_idf.h"

#include "esphome/components/md5/md5.h"
#include "esphome/components/watchdog/watchdog.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <esp_task_wdt.h>
#include <spi_flash_mmap.h>
#ifdef USE_OTA_DOWNGRADE_PROTECTION
#include <esp_app_desc.h>
#endif

namespace esphome::ota {

static const char *const TAG = "ota.idf";

std::unique_ptr<IDFOTABackend> make_ota_backend() { return make_unique<IDFOTABackend>(); }

#ifdef USE_OTA_PARTITIONS
OTAResponseTypes IDFOTABackend::set_data_partition(const char *label, size_t app_size) {
  this->data_partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
  if (this->data_partition_ == nullptr) {
    ESP_LOGE(TAG, "Data partition '%s' not found", label);
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  }
  this->data_app_size_ = app_size;
  this->data_listener_ = find_data_partition_listener(label);
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::prepare_data_partition_(size_t total_size) {
  // set_data_partition() must have run (the sub-header precedes the size on the wire).
  if (this->data_partition_ == nullptr || this->data_app_size_ > total_size)
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  this->data_image_size_ = total_size - this->data_app_size_;
  if (this->data_image_size_ == 0 || this->data_image_size_ > this->data_partition_->size)
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  // The mounted filesystem lets go of the flash before anything changes under it.
  if (this->data_listener_ != nullptr)
    this->data_listener_->on_ota_data_partition_before_write();
  this->data_active_ = true;
  // Same erase-budget arithmetic as the app slot below: ~10ms/KiB over a 15s floor.
  size_t erase_size = (this->data_image_size_ + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
  if (erase_size > this->data_partition_->size)
    erase_size = this->data_partition_->size;
  const uint32_t erase_budget_ms = 15000 + (erase_size >> 10) * 10;
  watchdog::WatchdogManager watchdog(erase_budget_ms);
  esp_err_t err = esp_partition_erase_range(this->data_partition_, 0, erase_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Erasing data partition failed (err=0x%X)", err);
    this->finish_data_partition_(false);
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  this->data_stream_pos_ = 0;
  return OTA_RESPONSE_OK;
}

void IDFOTABackend::finish_data_partition_(bool success) {
  if (!this->data_active_)
    return;
  this->data_active_ = false;
  // Success or not, the filesystem gets the flash back — on failure it remounts whatever is
  // there now; a half-written image fails the mount and auto_format heals it to empty.
  if (this->data_listener_ != nullptr)
    this->data_listener_->on_ota_data_partition_after_write(success);
}
#endif  // USE_OTA_PARTITIONS

OTAResponseTypes IDFOTABackend::begin(size_t image_size, ota::OTAType ota_type) {
#ifdef USE_OTA_PARTITIONS
  this->ota_type_ = ota_type;
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_PARTITION_TABLE) {
    // Reject any size other than ESP_PARTITION_TABLE_MAX_LEN
    if (image_size != ESP_PARTITION_TABLE_MAX_LEN) {
      ESP_LOGE(TAG, "Wrong partition table size: expected %u bytes, got %zu", ESP_PARTITION_TABLE_MAX_LEN, image_size);
      return OTA_RESPONSE_ERROR_PARTITION_TABLE_VERIFY;
    }
    memset(this->buf_, 0xFF, sizeof this->buf_);
    this->buf_written_ = 0;
    this->image_size_ = image_size;
    this->md5_.init();
    return OTA_RESPONSE_OK;
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_BOOTLOADER) {
    OTAResponseTypes result = this->prepare_bootloader_update_(image_size);
    if (result != OTA_RESPONSE_OK) {
      return result;
    }
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP_WITH_DATA) {
    // image_size is the whole stream here; the data part is prepared now, the app part (if
    // any) falls through to the regular esp_ota path below, sized to the app bytes alone.
    OTAResponseTypes result = this->prepare_data_partition_(image_size);
    if (result != OTA_RESPONSE_OK)
      return result;
    if (this->data_app_size_ == 0) {
      // Pure data update: no app slot involved, nothing else to prepare.
      this->md5_.init();
      return OTA_RESPONSE_OK;
    }
    image_size = this->data_app_size_;
  }
  if (!this->is_app_or_bootloader_update_()) {
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  }
#else
  if (ota_type != ota::OTA_TYPE_UPDATE_APP) {
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  }
#endif
#ifdef USE_OTA_ROLLBACK
  // If we're starting an OTA, the current boot is good enough - mark it valid
  // to prevent rollback and allow the OTA to proceed even if the safe mode
  // timer hasn't expired yet.
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  this->partition_ = esp_ota_get_next_update_partition(nullptr);
  if (this->partition_ == nullptr) {
    return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
  }

  // esp_ota_begin() erases the destination region, which blocks loopTask and
  // scales with the erase size -- a fixed watchdog overruns on large OTA slots.
  // An unknown size (0, e.g. web_server uploads) erases the whole partition, so
  // budget against the bytes actually erased. ~10ms/KiB (conservative
  // ~100 KiB/s erase) over a 15s floor; panic stays on so a stuck erase still
  // resets rather than hanging forever.
  size_t erase_size = image_size;
  if (erase_size == 0 || erase_size > this->partition_->size) {
    erase_size = this->partition_->size;
  }
  const uint32_t erase_budget_ms = 15000 + (erase_size >> 10) * 10;
  watchdog::WatchdogManager watchdog(erase_budget_ms);
  esp_err_t err = esp_ota_begin(this->partition_, image_size, &this->update_handle_);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed (err=0x%X)", err);
    esp_ota_abort(this->update_handle_);
    this->update_handle_ = 0;
    if (err == ESP_ERR_INVALID_SIZE) {
      return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
    } else if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    } else if (err == ESP_ERR_OTA_PARTITION_CONFLICT) {
      // This error appears with 1 factory and 1 ota partition
      return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
    }
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
#ifdef USE_OTA_PARTITIONS
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_BOOTLOADER) {
    OTAResponseTypes result = this->setup_bootloader_staging_();
    if (result != OTA_RESPONSE_OK) {
      return result;
    }
  }
#endif
  this->md5_.init();
  return OTA_RESPONSE_OK;
}

void IDFOTABackend::set_update_md5(const char *expected_md5) {
  memcpy(this->expected_bin_md5_, expected_md5, 32);
  this->md5_set_ = true;
}

OTAResponseTypes IDFOTABackend::write(uint8_t *data, size_t len) {
#ifdef USE_OTA_PARTITIONS
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_PARTITION_TABLE) {
    if (len > PARTITION_TABLE_BUFFER_SIZE - this->buf_written_) {
      ESP_LOGE(TAG, "Wrong partition table size");
      return OTA_RESPONSE_ERROR_PARTITION_TABLE_VERIFY;
    }
    memcpy(this->buf_ + this->buf_written_, data, len);
    this->buf_written_ += len;
    this->md5_.add(data, len);
    return OTA_RESPONSE_OK;
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP_WITH_DATA) {
    // One MD5 over the whole stream — end() verifies before anything boot-relevant happens.
    this->md5_.add(data, len);
    size_t pos = this->data_stream_pos_;
    this->data_stream_pos_ += len;
    size_t app_part = 0;
    if (pos < this->data_app_size_) {
      // The chunk may straddle the [app][image] seam.
      app_part = std::min(len, this->data_app_size_ - pos);
      esp_err_t app_err = esp_ota_write(this->update_handle_, data, app_part);
      if (app_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed (err=0x%X)", app_err);
        return OTA_RESPONSE_ERROR_WRITING_FLASH;
      }
    }
    if (app_part < len) {
      size_t data_off = pos + app_part - this->data_app_size_;
      if (data_off + (len - app_part) > this->data_image_size_)
        return OTA_RESPONSE_ERROR_DATA_PARTITION;
      esp_err_t data_err = esp_partition_write(this->data_partition_, data_off, data + app_part, len - app_part);
      if (data_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_write failed (err=0x%X)", data_err);
        return OTA_RESPONSE_ERROR_WRITING_FLASH;
      }
    }
    return OTA_RESPONSE_OK;
  }
  if (!this->is_app_or_bootloader_update_()) {
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  }
#endif
  esp_err_t err = esp_ota_write(this->update_handle_, data, len);
  this->md5_.add(data, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed (err=0x%X)", err);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      return OTA_RESPONSE_ERROR_MAGIC;
    } else if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::end() {
  if (this->md5_set_) {
    this->md5_.calculate();
    if (!this->md5_.equals_hex(this->expected_bin_md5_)) {
      this->abort();
      return OTA_RESPONSE_ERROR_MD5_MISMATCH;
    }
  }
#ifdef USE_OTA_PARTITIONS
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_PARTITION_TABLE) {
    return this->update_partition_table();
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP_WITH_DATA && this->data_app_size_ == 0) {
    // Data-only: every byte is on flash and the MD5 above covered them all. Hand the
    // filesystem back; the caller skips the reboot.
    this->finish_data_partition_(true);
    return OTA_RESPONSE_OK;
  }
  if (!this->is_app_or_bootloader_update_()) {
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  }
#endif
  esp_err_t err = esp_ota_end(this->update_handle_);
  this->update_handle_ = 0;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (err=0x%X)", err);
  }
#ifdef USE_OTA_PARTITIONS
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_BOOTLOADER) {
    return this->finalize_bootloader_update_(err);
  }
#endif
  if (err == ESP_OK) {
#ifdef USE_OTA_DOWNGRADE_PROTECTION
    // The image is written and (when signing is enabled) signature-verified by
    // esp_ota_end(), so its embedded project version can be trusted. Reject the
    // update if it is older than the running version by leaving the boot
    // partition unchanged -- the staged image simply never boots.
    esp_app_desc_t incoming;
    esp_err_t desc_err = esp_ota_get_partition_description(this->partition_, &incoming);
    if (desc_err != ESP_OK) {
      // Couldn't read the staged image's version, so the comparison is skipped.
      // Warn so the bypassed check is observable rather than silent.
      ESP_LOGW(TAG, "Downgrade protection: could not read image version (err=0x%X); allowing update", desc_err);
    } else if (version_is_older(incoming.version, ESPHOME_PROJECT_VERSION)) {
      ESP_LOGE(TAG, "Rejecting downgrade: image version '%s' is older than running version '%s'", incoming.version,
               ESPHOME_PROJECT_VERSION);
      return OTA_RESPONSE_ERROR_VERSION_DOWNGRADE;
    }
#endif
    err = esp_ota_set_boot_partition(this->partition_);
    if (err == ESP_OK) {
#ifdef USE_OTA_PARTITIONS
      // Only now — boot switch done — does the data side count as committed. On any earlier
      // failure the old app keeps booting and never meets the new data.
      this->finish_data_partition_(true);
#endif
      return OTA_RESPONSE_OK;
    }
  }
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
#ifdef USE_OTA_SIGNED_VERIFICATION
    ESP_LOGE(TAG, "OTA validation failed (err=0x%X) - possible signature verification failure", err);
    return OTA_RESPONSE_ERROR_SIGNATURE_INVALID;
#else
    return OTA_RESPONSE_ERROR_UPDATE_END;
#endif
  }
  if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
    return OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  return OTA_RESPONSE_ERROR_UNKNOWN;
}

void IDFOTABackend::abort() {
#ifdef USE_OTA_PARTITIONS
  this->finish_data_partition_(false);
  if (this->partition_table_part_ != nullptr) {
    esp_partition_deregister_external(this->partition_table_part_);
    this->partition_table_part_ = nullptr;
  }
  if (this->bootloader_part_ != nullptr) {
    esp_partition_deregister_external(this->bootloader_part_);
    this->bootloader_part_ = nullptr;
  }
#endif
  // esp_ota_abort with handle 0 returns ESP_ERR_INVALID_ARG harmlessly, so this is safe whether
  // or not an update is in flight.
  esp_ota_abort(this->update_handle_);
  this->update_handle_ = 0;
}

}  // namespace esphome::ota
#endif  // USE_ESP32
