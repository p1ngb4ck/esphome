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

// Bytes per read_chunk() call. The stream API imposes no ceiling, so this is only about how
// much of one asset moves per round trip through the worker.
static constexpr size_t READ_CHUNK = 4096;

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
#ifdef USE_STORAGE_WORKER
  // A read is already running; its callbacks carry it forward.
  if (this->loading_ != NO_LOAD)
    return;
#endif
  const uint32_t now = millis();
  if (now - this->last_try_ms_ < RETRY_INTERVAL_MS)
    return;
  this->last_try_ms_ = now;

#ifdef USE_STORAGE_WORKER
  for (size_t i = 0; i < this->asset_count_; i++) {
    if (this->assets_[i].psram == nullptr) {
      this->start_load_(i);
      return;  // one at a time
    }
  }
#endif
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

#ifdef USE_STORAGE_WORKER

void FileExplorerAssets::start_load_(size_t index) {
  Asset &asset = this->assets_[index];
  if (storage::global_storage_worker == nullptr) {
    if (!this->warned_pending_) {
      ESP_LOGE(TAG, "'%s': no storage worker configured -- storage-backed assets need one", asset.url);
      this->warned_pending_ = true;
    }
    return;
  }
  if (storage::global_storage_registry == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(asset.storage_path, &rel);
  if (ps == nullptr)
    return;  // not mounted yet -- loop() comes back

  // One stat for the size. Metadata only, so it is not the bulk read the ceiling guards
  // against, and the buffer has to be sized before the first chunk lands in it.
  storage::FileStat st{};
  if (ps->stat(rel, &st) != storage::StorageError::OK || st.is_dir)
    return;
  if (st.size == 0 || st.size > SIZE_MAX)
    return;

  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  auto want = static_cast<size_t>(st.size);
  uint8_t *buf = allocator.allocate(want);
  if (buf == nullptr) {
    if (!this->warned_pending_) {
      ESP_LOGE(TAG, "no PSRAM for '%s' (%u bytes)", asset.url, (unsigned) want);
      this->warned_pending_ = true;
    }
    return;
  }

  this->loading_ = index;
  this->pending_buf_ = buf;
  this->pending_len_ = want;
  this->pending_off_ = 0;
  this->last_read_ = 0;
  this->stream_open_ = false;

  storage::StorageError err = storage::global_storage_worker->begin_read(
      ps, rel, &this->stream_, [this](storage::StorageError e) { this->on_open_(e); });
  if (err != storage::StorageError::OK) {
    // on_open is not invoked when the call itself failed, so unwind here. NOT_READY just means
    // the stream pool is busy; loop() retries.
    this->abandon_load_("open rejected", err);
  }
}

void FileExplorerAssets::on_open_(storage::StorageError err) {
  if (err != storage::StorageError::OK) {
    this->abandon_load_("open failed", err);
    return;
  }
  this->stream_open_ = true;
  this->on_read_(storage::StorageError::OK);  // issues the first chunk
}

void FileExplorerAssets::on_read_(storage::StorageError err) {
  if (this->loading_ == NO_LOAD)
    return;
  if (err != storage::StorageError::OK) {
    this->abandon_load_("read failed", err);
    return;
  }
  // last_read_ is 0 both before the first chunk and at EOF; pending_off_ tells the two apart.
  this->pending_off_ += this->last_read_;
  if (this->pending_off_ > 0 && this->last_read_ == 0) {
    // EOF before the size stat promised: the file changed under us. Treat as a failure and
    // let loop() start over rather than serve a truncated asset.
    this->abandon_load_("short read", storage::StorageError::READ_ERROR);
    return;
  }
  if (this->pending_off_ >= this->pending_len_) {
    storage::StorageError e = storage::global_storage_worker->end_read(
        this->stream_, [this](storage::StorageError ce) { this->on_closed_(ce); });
    if (e != storage::StorageError::OK)
      this->abandon_load_("close rejected", e);
    return;
  }

  const size_t remaining = this->pending_len_ - this->pending_off_;
  const size_t want = remaining < READ_CHUNK ? remaining : READ_CHUNK;
  this->last_read_ = 0;
  storage::StorageError e = storage::global_storage_worker->read_chunk(
      this->stream_, this->pending_buf_ + this->pending_off_, want, &this->last_read_,
      [this](storage::StorageError re) { this->on_read_(re); });
  if (e != storage::StorageError::OK)
    this->abandon_load_("read rejected", e);
}

void FileExplorerAssets::on_closed_(storage::StorageError err) {
  if (this->loading_ == NO_LOAD)
    return;
  this->stream_open_ = false;
  if (err != storage::StorageError::OK) {
    this->abandon_load_("close failed", err);
    return;
  }
  Asset &asset = this->assets_[this->loading_];
  asset.psram = this->pending_buf_;
  asset.len = this->pending_off_;
  ESP_LOGD(TAG, "'%s': %u bytes from '%s' into PSRAM", asset.url, (unsigned) asset.len, asset.storage_path);
  this->pending_buf_ = nullptr;
  this->pending_len_ = 0;
  this->pending_off_ = 0;
  this->loading_ = NO_LOAD;
  this->finish_if_complete_();
}

void FileExplorerAssets::abandon_load_(const char *reason, storage::StorageError err) {
  if (this->loading_ != NO_LOAD) {
    ESP_LOGW(TAG, "'%s': %s (%s)", this->assets_[this->loading_].storage_path, reason, storage::error_to_string(err));
  }
  if (this->stream_open_) {
    // Release the pool slot even after a failed chunk, as end_read()'s contract demands. Its
    // own result is of no further interest -- the load is already lost.
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
    this->stream_open_ = false;
  }
  if (this->pending_buf_ != nullptr) {
    RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
    allocator.deallocate(this->pending_buf_, this->pending_len_);
    this->pending_buf_ = nullptr;
  }
  this->pending_len_ = 0;
  this->pending_off_ = 0;
  this->loading_ = NO_LOAD;
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

#endif  // USE_STORAGE_WORKER

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
