#include "firmware_update.h"

#include "esphome/components/md5/md5.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::firmware_update {

static const char *const TAG = "firmware_update";

// Firmware read chunk size. One reused buffer on the stack — never the whole image in RAM.
static constexpr size_t CHUNK_SIZE = 1024;

void FirmwareUpdateComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Firmware Update (from storage):");
  ESP_LOGCONFIG(TAG, "  Path: %s", this->path_.c_str());
}

void FirmwareUpdateComponent::flash() {
  if (this->path_.empty()) {
    ESP_LOGE(TAG, "Path not set; cannot start update");
    return;
  }

  ESP_LOGI(TAG, "Starting firmware update from '%s'", this->path_.c_str());
#ifdef USE_OTA_STATE_LISTENER
  this->notify_state_(ota::OTA_STARTED, 0.0f, 0);
#endif

#ifdef USE_STORAGE
  auto backend = ota::make_ota_backend();
  uint8_t ota_status = this->stream_from_storage_(backend);
#else
  uint8_t ota_status = ota::OTA_RESPONSE_ERROR_UNKNOWN;
  ESP_LOGE(TAG, "Storage support is not enabled in this build");
#endif

  switch (ota_status) {
    case ota::OTA_RESPONSE_OK:
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_COMPLETED, 100.0f, ota_status);
#endif
      ESP_LOGI(TAG, "Update complete; rebooting");
      delay(10);
      App.safe_reboot();
      break;

    default:
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_ERROR, 0.0f, ota_status);
#endif
      ESP_LOGE(TAG, "Update failed (code 0x%02X)", ota_status);
      break;
  }
}

#ifdef USE_STORAGE
uint8_t FirmwareUpdateComponent::stream_from_storage_(ota::OTABackendPtr &backend) {
  if (storage::global_storage_registry == nullptr) {
    ESP_LOGE(TAG, "Storage registry not available");
    return ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }

  // Resolve the configured path to a mounted PathStorage (filesystem or network) and its
  // storage-relative remainder. resolve_path returns a pointer into resolver scratch, so the
  // relative path is copied for the streaming loop.
  const char *rel_raw = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->path_.c_str(), &rel_raw);
  if (ps == nullptr || rel_raw == nullptr) {
    ESP_LOGE(TAG, "'%s' does not resolve to a mounted storage", this->path_.c_str());
    return ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }
  std::string rel(rel_raw);

  // Stat for the total size (also an early existence/type check).
  storage::FileStat st{};
  storage::StorageError serr = ps->stat(rel.c_str(), &st);
  if (serr != storage::StorageError::OK) {
    ESP_LOGE(TAG, "Cannot stat '%s' (%s)", this->path_.c_str(), storage::error_to_string(serr));
    return ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }
  if (st.is_dir || st.size == 0) {
    ESP_LOGE(TAG, "'%s' is not a readable firmware file", this->path_.c_str());
    return ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }
  const uint64_t total_size = st.size;
  ESP_LOGI(TAG, "Firmware size: %llu bytes", static_cast<unsigned long long>(total_size));

  // FILESYSTEM storages stream through a DATA-PLANE handle; NETWORK (NFS) is stateless and reads
  // by path+offset. get_storage_type() is the no-RTTI discrimination hook the API provides.
  const bool is_fs = ps->get_storage_type() == storage::StorageType::FILESYSTEM;
  storage::FileHandle *handle = nullptr;
  if (is_fs) {
    serr = static_cast<storage::FilesystemStorage *>(ps)->open(rel.c_str(), handle, storage::OpenMode::READ);
    if (serr != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Opening '%s' failed (%s)", this->path_.c_str(), storage::error_to_string(serr));
      return ota::OTA_RESPONSE_ERROR_UNKNOWN;
    }
  } else if (ps->get_storage_type() != storage::StorageType::NETWORK) {
    ESP_LOGE(TAG, "Storage type of '%s' does not support streaming reads", this->path_.c_str());
    return ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }

  // Begin the OTA slot for the whole image. The backend inspects the stream head itself: a plain
  // app image sizes the app slot; a combined pre-fill image (EPF2 header) sizes the app slot and
  // prepares the named data partition, routing each chunk at the app/image seam.
  auto begin_result = backend->begin(static_cast<size_t>(total_size));
  if (begin_result != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "backend->begin error: %d", begin_result);
    if (is_fs)
      static_cast<storage::FilesystemStorage *>(ps)->close(handle);
    return begin_result;
  }

  // Stream while computing MD5, exactly as the HTTP path does — the backend verifies this digest
  // against the image at end(), on top of the app image's own intrinsic (esp_ota) validation.
  md5::MD5Digest md5{};
  md5.init();

  uint8_t buffer[CHUNK_SIZE];
  uint64_t offset = 0;
  uint32_t last_progress = 0;
  uint8_t result = ota::OTA_RESPONSE_OK;
  while (offset < total_size) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), total_size - offset));
    size_t got = 0;
    if (is_fs) {
      serr = static_cast<storage::FilesystemStorage *>(ps)->read(handle, buffer, want, &got);
    } else {
      serr = static_cast<storage::NetworkStorage *>(ps)->read_chunk(rel.c_str(), buffer, offset, want, &got);
    }
    App.feed_wdt();
    yield();
    if (serr != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Read failed at %llu (%s)", static_cast<unsigned long long>(offset),
               storage::error_to_string(serr));
      result = ota::OTA_RESPONSE_ERROR_UNKNOWN;
      break;
    }
    if (got == 0) {  // unexpected short read before total_size
      ESP_LOGE(TAG, "Firmware truncated at %llu", static_cast<unsigned long long>(offset));
      result = ota::OTA_RESPONSE_ERROR_UNKNOWN;
      break;
    }

    md5.add(buffer, got);
    this->update_started_ = true;
    result = backend->write(buffer, got);
    if (result != ota::OTA_RESPONSE_OK) {
      ESP_LOGE(TAG, "Writing to flash failed at %llu (code 0x%02X)", static_cast<unsigned long long>(offset), result);
      break;
    }
    offset += got;

    uint32_t now = millis();
    if (now - last_progress > 1000 || offset == total_size) {
      last_progress = now;
      float percentage = offset * 100.0f / total_size;
      ESP_LOGD(TAG, "Progress: %0.1f%%", percentage);
#ifdef USE_OTA_STATE_LISTENER
      this->notify_state_(ota::OTA_IN_PROGRESS, percentage, 0);
#endif
    }
  }

  if (is_fs)
    static_cast<storage::FilesystemStorage *>(ps)->close(handle);

  if (result != ota::OTA_RESPONSE_OK || offset != total_size) {
    if (this->update_started_)
      backend->abort();
    return result != ota::OTA_RESPONSE_OK ? result : ota::OTA_RESPONSE_ERROR_UNKNOWN;
  }

  // Hand the computed digest to the backend and finalize. end() checks the digest, lets the app
  // image validate itself (esp_ota_end) and only then switches the boot slot — a mismatch or an
  // invalid image aborts without activating anything.
  char md5_str[33];
  md5.calculate();
  md5.get_hex(md5_str);
  backend->set_update_md5(md5_str);

  App.feed_wdt();
  yield();

  result = backend->end();
  if (result != ota::OTA_RESPONSE_OK) {
    ESP_LOGE(TAG, "backend->end error: 0x%02X", result);
    return result;
  }
  return ota::OTA_RESPONSE_OK;
}
#endif  // USE_STORAGE

}  // namespace esphome::firmware_update
