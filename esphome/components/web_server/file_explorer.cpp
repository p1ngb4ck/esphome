#include "file_explorer.h"
#if defined(USE_WEBSERVER_FILE_EXPLORER) && defined(USE_ESP_IDF)

#include <cstring>

#include "esphome/components/storage/storage.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace web_server {

static const char *const TAG = "web_server.file_explorer";

// How often a storage-backed asset is retried while its storage is not mounted yet. Long
// enough that a card that never appears costs nothing, short enough that a card inserted by
// hand shows up without a reboot.
static constexpr uint32_t RETRY_INTERVAL_MS = 2000;


void FileExplorerAssets::setup() {
  bool all = true;
  for (size_t i = 0; i < this->asset_count_; i++) {
    Asset &a = this->assets_[i];
    if (a.flash != nullptr) {
      if (!this->load_from_flash_(a)) {
        // Nothing to retry: the flash copy is there, the PSRAM was not. Fail the component
        // rather than answer 503 forever over a condition that will not change.
        ESP_LOGE(TAG, "no PSRAM for '%s' (%u bytes)", a.url, (unsigned) a.flash_len);
        this->mark_failed();
        return;
      }
    } else {
      // Storage-backed: nothing to do here. The read is asynchronous and the storage may not
      // even be mounted yet, so loop() drives it.
      all = false;
    }
  }
  this->ready_ = all;
  if (!this->ready_) {
    ESP_LOGI(TAG, "waiting for the storage holding the browser assets");
  }
}

void FileExplorerAssets::loop() {
  if (this->ready_ || this->assets_ == nullptr)
    return;
  const uint32_t now = millis();
  if (now - this->last_try_ms_ < RETRY_INTERVAL_MS)
    return;
  this->last_try_ms_ = now;

  for (size_t i = 0; i < this->asset_count_; i++) {
    if (this->assets_[i].psram == nullptr) {
      this->start_load_(i);
      return;  // one per pass -- read_file() blocks the loop for the read
    }
  }
  this->finish_if_complete_();
}

bool FileExplorerAssets::load_from_flash_(Asset &asset) {
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  uint8_t *buf = allocator.allocate(asset.flash_len);
  if (buf == nullptr)
    return false;
  memcpy(buf, asset.flash, asset.flash_len);
  asset.psram = buf;
  asset.len = asset.flash_len;
  ESP_LOGD(TAG, "'%s': %u bytes from flash into PSRAM", asset.url, (unsigned) asset.len);
  return true;
}

void FileExplorerAssets::start_load_(size_t index) {
  Asset &asset = this->assets_[index];
  if (storage::global_storage_registry == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(asset.storage_path, &rel);
  if (ps == nullptr)
    return;  // not mounted yet -- loop() comes back

  // The storage interface reads the whole file: storage::read_file() does the stat, the single
  // buffer, the read loop and EOF, feeding the watchdog, and hands back a PSRAM RamBuffer. No
  // hand-rolled chunk loop and no data-plane calls of our own -- the loader is just this bridge.
  storage::RamBuffer buf;
  size_t size = 0;
  storage::StorageError err = storage::read_file(ps, rel, buf, &size);
  if (err != storage::StorageError::OK) {
    if (!this->warned_pending_) {
      ESP_LOGW(TAG, "'%s': read failed (%s)", asset.storage_path, storage::error_to_string(err));
      this->warned_pending_ = true;
    }
    return;  // retry on the next interval
  }

  // The asset lives for the whole run, so release the buffer's ownership (never freed, exactly
  // like the flash copy) and serve straight from it.
  asset.psram = buf.release();
  asset.len = size;
  ESP_LOGD(TAG, "'%s': %u bytes from '%s' into PSRAM", asset.url, (unsigned) size, asset.storage_path);
  this->finish_if_complete_();
}

void FileExplorerAssets::finish_if_complete_() {
  for (size_t i = 0; i < this->asset_count_; i++) {
    if (this->assets_[i].psram == nullptr)
      return;
  }
  this->ready_ = true;
  ESP_LOGI(TAG, "browser assets loaded from storage");
  // Nothing left to poll for.
  this->disable_loop();
}

const FileExplorerAssets::Asset *FileExplorerAssets::find(StringRef url) const {
  for (size_t i = 0; i < this->asset_count_; i++) {
    if (url == this->assets_[i].url)
      return &this->assets_[i];
  }
  return nullptr;
}

void FileExplorerAssets::dump_config() {
  ESP_LOGCONFIG(TAG, "File explorer assets:");
  for (size_t i = 0; i < this->asset_count_; i++) {
    const Asset &a = this->assets_[i];
    ESP_LOGCONFIG(TAG, "  %s: %s, %u bytes%s", a.url, a.flash != nullptr ? "flash" : a.storage_path,
                  (unsigned) (a.psram != nullptr ? a.len : 0), a.psram != nullptr ? " (in PSRAM)" : " (pending)");
  }
}

}  // namespace web_server
}  // namespace esphome

#endif  // USE_WEBSERVER_FILE_EXPLORER && USE_ESP_IDF
