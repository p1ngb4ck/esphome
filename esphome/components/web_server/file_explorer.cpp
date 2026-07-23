#include "file_explorer.h"
#if defined(USE_WEBSERVER_FILE_EXPLORER) && defined(USE_ESP32)

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
      all &= this->load_from_storage_(a);
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

  bool all = true;
  for (size_t i = 0; i < this->asset_count_; i++) {
    Asset &a = this->assets_[i];
    if (a.psram != nullptr)
      continue;  // already loaded
    all &= this->load_from_storage_(a);
  }
  if (all) {
    this->ready_ = true;
    ESP_LOGI(TAG, "browser assets loaded from storage");
    // Nothing left to poll for.
    this->disable_loop();
  }
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

bool FileExplorerAssets::load_from_storage_(Asset &asset) {
  if (storage::global_storage_registry == nullptr)
    return false;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(asset.storage_path, &rel);
  if (ps == nullptr)
    return false;  // not mounted yet — loop() comes back

  storage::FileStat st{};
  if (ps->stat(rel, &st) != storage::StorageError::OK || st.is_dir)
    return false;
  if (st.size == 0 || st.size > SIZE_MAX)
    return false;

  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  auto want = static_cast<size_t>(st.size);
  uint8_t *buf = allocator.allocate(want);
  if (buf == nullptr) {
    if (!this->warned_pending_) {
      ESP_LOGE(TAG, "no PSRAM for '%s' (%u bytes)", asset.url, (unsigned) want);
      this->warned_pending_ = true;
    }
    return false;
  }

  // Read through the blocking helper: this is setup-time work on files of a known size, which
  // is exactly what it is for. max_blocking_transfer_size has to allow it, so codegen warns
  // when the configured ceiling is below the largest asset.
  storage::RamBuffer holder;
  size_t got = 0;
  storage::StorageError err = storage::read_file(ps, rel, holder, &got);
  if (err != storage::StorageError::OK || got != want) {
    allocator.deallocate(buf, want);
    ESP_LOGW(TAG, "'%s': read failed (%s)", asset.storage_path, storage::error_to_string(err));
    return false;
  }
  memcpy(buf, holder.get(), got);
  asset.psram = buf;
  asset.len = got;
  ESP_LOGD(TAG, "'%s': %u bytes from '%s' into PSRAM", asset.url, (unsigned) got, asset.storage_path);
  return true;
}

bool FileExplorerAssets::matches(const std::string &url) const {
  for (size_t i = 0; i < this->asset_count_; i++) {
    if (url == this->assets_[i].url)
      return true;
  }
  return false;
}

bool FileExplorerAssets::handle(AsyncWebServerRequest *request) {
  const std::string url = request->url().c_str();
  for (size_t i = 0; i < this->asset_count_; i++) {
    Asset &a = this->assets_[i];
    if (url != a.url)
      continue;
    if (a.psram == nullptr) {
      // Storage-backed and not there yet. 503 with Retry-After rather than 404: the asset is
      // configured, it is simply not loaded, and a reloading browser should try again.
      AsyncWebServerResponse *resp = request->beginResponse(503, "text/plain", "assets not loaded yet");
      resp->addHeader("Retry-After", "5");
      request->send(resp);
      return true;
    }
    AsyncWebServerResponse *resp =
        request->beginResponse_P(200, a.content_type, a.psram, a.len);
    if (a.gzipped)
      resp->addHeader("Content-Encoding", "gzip");
    // These change only with the firmware or the files on the card; letting the browser keep
    // them saves re-sending ~300 kB on every page load.
    resp->addHeader("Cache-Control", "public, max-age=86400");
    request->send(resp);
    return true;
  }
  return false;
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

#endif  // USE_WEBSERVER_FILE_EXPLORER && USE_ESP32
