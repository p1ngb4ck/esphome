#pragma once
#include "esphome/core/defines.h"
#if defined(USE_WEBSERVER_FILE_EXPLORER) && defined(USE_ESP_IDF)

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
// The storage-backed half reads through the storage worker's async stream API; storage_worker.h
// is itself behind this define, and asset_source: flash needs no storage at all. Codegen requests
// the worker whenever asset_source: storage is configured.
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

namespace esphome {
namespace web_server {

// Serves the advanced file browser's assets, always out of PSRAM.
//
// PSRAM is the only place these are ever served from: it is external RAM, there is a lot of it on
// the variants this component is limited to, and it keeps ~300 kB of markup out of both internal
// RAM and the response path. What differs is where the bytes come from before they get there:
//
//   FLASH   -- a gzipped copy is compiled in as PROGMEM and copied to PSRAM once at setup(),
//             then served with Content-Encoding: gzip. Always available, costs the flash.
//   STORAGE -- the RAW files sit on a PathStorage and are read into PSRAM through the storage
//             worker's async stream API as soon as that storage is mounted, which may be seconds
//             after boot or never, then served as-is. Costs no flash. External storage has room
//             to spare, so nothing is compressed there.
//
// Either way the load happens ONCE and every request is served straight from the PSRAM buffer;
// the serve path never reads storage. (When there is no PSRAM to preload into, the file API
// falls back to streaming a storage-backed asset from the medium per request.)
class FileExplorerAssets : public Component {
 public:
  // One asset: a URL, its type, and where its bytes come from.
  struct Asset {
    const char *url;              // "/file-explorer/file-explorer.js"
    const char *content_type;     // "text/javascript"
    const uint8_t *flash;         // gzipped bytes in PROGMEM, or nullptr for STORAGE
    size_t flash_len;             // valid when flash != nullptr
    const char *storage_path;     // "/sdcard/fe/file-explorer.js" (raw), or nullptr for FLASH
    bool gzipped;                 // sets Content-Encoding: gzip
    uint8_t *psram;               // where it is served from; null until loaded
    size_t len;                   // bytes at psram
  };

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  void dump_config() override;

  // Codegen emits one call per asset; the array it points at is a static in the generated
  // code, so this object never owns it.
  void set_assets(Asset *assets, size_t count) {
    this->assets_ = assets;
    this->asset_count_ = count;
  }

  // True once every asset has its bytes in PSRAM. Until then the handler answers 503.
  bool ready() const { return this->ready_; }

  // Looks up the asset for a URL. Returns nullptr when the URL is not one of ours. The
  // returned asset may still have a null psram: it is configured but not loaded yet, and it
  // is the caller that decides what to answer in that case.
  const Asset *find(StringRef url) const;

 protected:
  // Copies a PROGMEM asset into PSRAM. Returns false when PSRAM could not be had.
  bool load_from_flash_(Asset &asset);
  // Called once the last storage-backed asset has landed.
  void finish_if_complete_();

#ifdef USE_STORAGE_WORKER
  // Storage-backed assets are read through the worker's async stream API (begin_read/read_chunk/
  // end_read): the worker runs it on its own task for task-safe media, loop-sliced otherwise, and
  // stages its own PSRAM-DMA chunks -- this component never touches the storage directly and takes
  // no direct stat. One asset is in flight at a time; they are read once at startup and there are
  // only a handful. The read runs until read_chunk() reports EOF, into an EXTERNAL (PSRAM) buffer
  // that grows as chunks arrive.
  void start_load_(size_t index);
  void issue_read_();
  void on_open_(storage::StorageError err);
  void on_read_(storage::StorageError err);
  void on_closed_(storage::StorageError err);
  // Releases the in-flight buffer and the stream slot, so loop() can try again later.
  void abandon_load_(const char *reason, storage::StorageError err);
#endif

  Asset *assets_{nullptr};
  size_t asset_count_{0};
  bool ready_{false};
  // Only set once, so a storage that never appears does not log on every loop.
  bool warned_pending_{false};
  uint32_t last_try_ms_{0};

#ifdef USE_STORAGE_WORKER
  // In-flight read. loading_ is the index into assets_, or NO_LOAD when nothing is open.
  static constexpr size_t NO_LOAD = SIZE_MAX;
  size_t loading_{NO_LOAD};
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  // Whole-file destination in PSRAM; grows (doubling) as chunks arrive, since the size is never
  // taken from a direct stat -- the read runs until read_chunk() reports EOF.
  uint8_t *pending_buf_{nullptr};
  size_t pending_len_{0};  // allocated capacity of pending_buf_
  size_t pending_off_{0};  // bytes read so far
  // read_chunk() fills this before invoking its callback, so it has to outlive the call.
  size_t last_read_{0};
#endif
};

}  // namespace web_server
}  // namespace esphome

#endif  // USE_WEBSERVER_FILE_EXPLORER && USE_ESP_IDF
