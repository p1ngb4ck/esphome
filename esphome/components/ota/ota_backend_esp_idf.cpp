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
#include <sdkconfig.h>
#include <spi_flash_mmap.h>
#ifdef USE_OTA_DOWNGRADE_PROTECTION
#include <esp_app_desc.h>
#endif

namespace esphome::ota {

static const char *const TAG = "ota";

std::unique_ptr<IDFOTABackend> make_ota_backend() { return make_unique<IDFOTABackend>(); }

#ifdef USE_OTA_PARTITIONS
// The in-band pre-fill header a stock sender streams unchanged (layout documented in
// esphome_esp_littlefs tools/make_prefill_ota.py): magic "EPF2" | app_size u32 BE |
// image_size u32 BE | label (32, NUL-padded) | image MD5 (16) | reserved (4).
static constexpr uint8_t PREFILL_MAGIC[4] = {'E', 'P', 'F', '2'};

OTAResponseTypes IDFOTABackend::write_app_(const uint8_t *data, size_t len) {
  // No md5 here: every caller has already hashed these bytes when they arrived.
  esp_err_t err = esp_ota_write(this->update_handle_, data, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed (err=0x%X)", err);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
      return OTA_RESPONSE_ERROR_MAGIC;
    if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL)
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
  return OTA_RESPONSE_OK;
}

OTAResponseTypes IDFOTABackend::decide_stream_head_() {
  this->head_decided_ = true;
  if (memcmp(this->head_buf_, PREFILL_MAGIC, sizeof(PREFILL_MAGIC)) != 0) {
    // A plain app image: begin the slot for the announced total and replay the buffered
    // head — from here everything behaves exactly like the immediate-begin path.
    OTAResponseTypes result = this->app_slot_begin_(this->pending_total_);
    if (result != OTA_RESPONSE_OK)
      return result;
    return this->write_app_(this->head_buf_, this->head_have_);
  }
  const uint8_t *h = this->head_buf_;
  size_t app_size =
      (static_cast<size_t>(h[4]) << 24) | (static_cast<size_t>(h[5]) << 16) | (static_cast<size_t>(h[6]) << 8) | h[7];
  size_t image_size =
      (static_cast<size_t>(h[8]) << 24) | (static_cast<size_t>(h[9]) << 16) | (static_cast<size_t>(h[10]) << 8) | h[11];
  char label[33];
  memcpy(label, h + 12, 32);
  label[32] = '\0';
  if (app_size == 0 || image_size == 0 || sizeof(this->head_buf_) + app_size + image_size != this->pending_total_) {
    ESP_LOGE(TAG, "Pre-fill header sizes inconsistent with the stream");
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  }
  this->data_partition_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
  if (this->data_partition_ == nullptr) {
    ESP_LOGE(TAG, "Pre-fill data partition '%s' not found", label);
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  }
  if (image_size > this->data_partition_->size)
    return OTA_RESPONSE_ERROR_DATA_PARTITION;
  this->data_app_size_ = app_size;
  this->data_image_size_ = image_size;
  this->data_stream_pos_ = 0;
  this->data_listener_ = find_data_partition_listener(label);
  ESP_LOGI(TAG, "OTA carries a pre-fill image for '%s' (app %zu + image %zu bytes)", label, app_size, image_size);
  // The mounted filesystem lets go of the flash before anything changes under it.
  if (this->data_listener_ != nullptr)
    this->data_listener_->on_ota_data_partition_before_write();
  this->data_active_ = true;
  {
    // Same erase-budget arithmetic as the app slot: ~10ms/KiB over a 15s floor.
    size_t erase_size = (image_size + SPI_FLASH_SEC_SIZE - 1) & ~(SPI_FLASH_SEC_SIZE - 1);
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
  }
  return this->app_slot_begin_(app_size);
}

OTAResponseTypes IDFOTABackend::route_stream_(const uint8_t *data, size_t len) {
  // One pass, split at the [app][image] seam; a chunk may straddle it.
  size_t pos = this->data_stream_pos_;
  this->data_stream_pos_ += len;
  size_t app_part = 0;
  if (pos < this->data_app_size_) {
    app_part = std::min(len, this->data_app_size_ - pos);
    OTAResponseTypes result = this->write_app_(data, app_part);
    if (result != OTA_RESPONSE_OK)
      return result;
  }
  if (app_part < len) {
    size_t data_off = pos + app_part - this->data_app_size_;
    if (data_off + (len - app_part) > this->data_image_size_)
      return OTA_RESPONSE_ERROR_DATA_PARTITION;
    esp_err_t err = esp_partition_write(this->data_partition_, data_off, data + app_part, len - app_part);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_partition_write failed (err=0x%X)", err);
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
  }
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

#ifdef USE_OTA_PARTITIONS
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP) {
    // A completely stock sender may be streaming a pre-fill artifact
    // ([64-byte header][app][littlefs image], built by our esp_littlefs component) — that
    // is only knowable from the first bytes. Defer esp_ota_begin() until write() has seen
    // them: with a header the slot is sized/erased for the app alone and the image bytes
    // route straight into the named data partition; without one this behaves exactly as
    // the immediate begin below.
    this->pending_total_ = image_size;
    this->head_have_ = 0;
    this->head_decided_ = false;
    this->md5_.init();
    return OTA_RESPONSE_OK;
  }
#endif

  OTAResponseTypes result = this->app_slot_begin_(image_size);
  if (result == OTA_RESPONSE_OK)
    this->md5_.init();
  return result;
}

OTAResponseTypes IDFOTABackend::app_slot_begin_(size_t image_size) {
  // esp_ota_begin() erases the destination region, which blocks loopTask and
  // scales with the erase size -- a fixed watchdog overruns on large OTA slots.
  // An unknown size (0, e.g. web_server uploads) erases the whole partition, so
  // budget against the bytes actually erased. ~10ms/KiB (conservative
  // ~100 KiB/s erase) over a 15s floor; panic stays on so a stuck erase still
  // resets rather than hanging forever.
  size_t erase_size = image_size;
  if (erase_size == 0 || erase_size > this->partition_->size) {
    erase_size = this->partition_->size;
  // Both lazy-erase paths below replace esp_ota_begin()'s blocking full erase.
  // Size check replaces the one that erase performed (0 = unknown size,
  // e.g. web_server uploads).
  if (image_size != 0 && image_size > this->partition_->size) {
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
  this->written_ = 0;
  esp_err_t err;
#ifdef USE_OTA_BLOCK_ERASE_AHEAD
  this->erased_end_ = 0;
  // Unlike esp_ota_begin(), esp_ota_resume() does not reject a running app in
  // ESP_OTA_IMG_PENDING_VERIFY; that state is unreachable here because the app
  // was marked valid at boot (esp32/hal.cpp) or just above under USE_OTA_ROLLBACK.
  // erase_size 0 (!= OTA_WITH_SEQUENTIAL_WRITES) means no erase; erase_ahead_() handles it
  err = esp_ota_resume(this->partition_, 0, 0, &this->update_handle_);
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  // esp_ota_begin() does this on IDF 5.5+; esp_ota_resume() does not. Prevents
  // booting a half-written slot after a crash mid-OTA. Not available on the
  // 5.3.3/5.4.2 backports, whose esp_ota_begin() did not invalidate either.
  if (err == ESP_OK) {
    esp_ota_invalidate_inactive_ota_data_slot();
  }
#endif
#else
  err = esp_ota_begin(this->partition_, OTA_WITH_SEQUENTIAL_WRITES, &this->update_handle_);
#endif

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "OTA begin failed (err=0x%X)", err);
    esp_ota_abort(this->update_handle_);
    this->update_handle_ = 0;
    if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
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
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP && !this->head_decided_) {
    // Undecided stream head: gather 64 bytes, then decide once.
    this->md5_.add(data, len);
    while (len > 0 && this->head_have_ < sizeof(this->head_buf_)) {
      this->head_buf_[this->head_have_++] = *data++;
      len--;
    }
    if (this->head_have_ < sizeof(this->head_buf_))
      return OTA_RESPONSE_OK;  // decision pending; end() flushes streams shorter than a header
    OTAResponseTypes result = this->decide_stream_head_();
    if (result != OTA_RESPONSE_OK)
      return result;
    if (len == 0)
      return OTA_RESPONSE_OK;
    // Remainder of the deciding chunk: seam-routed for a pre-fill stream, straight to the
    // slot for a plain app (its bytes are already hashed above).
    if (this->data_active_)
      return this->route_stream_(data, len);
    return this->write_app_(data, len);
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP && this->data_active_) {
    this->md5_.add(data, len);
    return this->route_stream_(data, len);
  }
  if (!this->is_app_or_bootloader_update_()) {
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;
  }
#endif
  // Overflow can only happen on unknown-size uploads (web_server); known
  // sizes were rejected in begin().
  if (this->written_ + len > this->partition_->size) {
    return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
  }
#ifdef USE_OTA_BLOCK_ERASE_AHEAD
  OTAResponseTypes erase_result = this->erase_ahead_(len);
  if (erase_result != OTA_RESPONSE_OK) {
    return erase_result;
  }
#endif
  esp_err_t err = esp_ota_write(this->update_handle_, data, len);
  this->md5_.add(data, len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed (err=0x%X)", err);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
      return OTA_RESPONSE_ERROR_MAGIC;
    } else if (err == ESP_ERR_INVALID_SIZE) {
      // Sequential-writes fallback: IDF's lazy erase reports overflow here
      return OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE;
    } else if (err == ESP_ERR_FLASH_OP_TIMEOUT || err == ESP_ERR_FLASH_OP_FAIL) {
      return OTA_RESPONSE_ERROR_WRITING_FLASH;
    }
    return OTA_RESPONSE_ERROR_UNKNOWN;
  }
  this->written_ += len;
  return OTA_RESPONSE_OK;
}

#ifdef USE_OTA_BLOCK_ERASE_AHEAD
OTAResponseTypes IDFOTABackend::erase_ahead_(size_t len) {
  const size_t end = this->written_ + len;
  if (this->erased_end_ >= end) {
    return OTA_RESPONSE_OK;
  }
  // Round up to a block boundary, clamped to the partition end; IDF splits the
  // range into 64 KiB block erases where aligned, sector erases elsewhere.
  const size_t erase_to = next_erase_end(end, this->partition_->size);
  // A block erase is one uninterruptible flash op (typically ~150 ms, seconds
  // on aged flash) and the transfer loop may not have fed the WDT for ~1s.
  watchdog::WatchdogManager watchdog(15000);
  esp_err_t err = esp_partition_erase_range(this->partition_, this->erased_end_, erase_to - this->erased_end_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_partition_erase_range failed (err=0x%X)", err);
    return err == ESP_ERR_INVALID_SIZE ? OTA_RESPONSE_ERROR_ESP32_NOT_ENOUGH_SPACE : OTA_RESPONSE_ERROR_WRITING_FLASH;
  }
  this->erased_end_ = erase_to;
  return OTA_RESPONSE_OK;
}
#endif

OTAResponseTypes IDFOTABackend::end() {
  if (this->md5_set_) {
    this->md5_.calculate();
    if (!this->md5_.equals_hex(this->expected_bin_md5_)) {
      this->abort();
      return OTA_RESPONSE_ERROR_MD5_MISMATCH;
    }
  }
#ifdef USE_OTA_PARTITIONS
  // A partition-table update carries an MD5 (checked by IDF), not a Secure Boot
  // signature, and only re-points boot at an already-installed app -- so it is
  // intentionally not run through the signature verifier below.
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_PARTITION_TABLE) {
    return this->update_partition_table();
  }
  if (this->ota_type_ == ota::OTA_TYPE_UPDATE_APP && !this->head_decided_ && this->head_have_ > 0) {
    // A stream shorter than a pre-fill header cannot be one: begin the slot for the real
    // total and flush the buffered head as plain app bytes (already hashed on receive).
    OTAResponseTypes result = this->app_slot_begin_(this->pending_total_);
    if (result == OTA_RESPONSE_OK)
      result = this->write_app_(this->head_buf_, this->head_have_);
    this->head_decided_ = true;
    if (result != OTA_RESPONSE_OK)
      return result;
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
#ifdef USE_OTA_SIGNED_VERIFICATION_MULTI_KEY
    // IDF's built-in on-update check is disabled for this scheme (it only
    // matches the incoming image's first signature block against the running
    // app's first). Verify here against every key the running app trusts, so
    // rotation and backup keys are accepted. Leaving the boot partition
    // unchanged means a rejected image never boots.
    if (!this->verify_signed_image_(this->partition_)) {
      return OTA_RESPONSE_ERROR_SIGNATURE_INVALID;
    }
#endif
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
  this->written_ = 0;
#ifdef USE_OTA_BLOCK_ERASE_AHEAD
  this->erased_end_ = 0;
#endif
}

}  // namespace esphome::ota
#endif  // USE_ESP32
