#pragma once
#include "esphome/core/defines.h"
#if defined(USE_WEBSERVER_FILE_EXPLORER) && defined(USE_ESP_IDF)

#include <cstddef>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
// The storage-backed path reads through the worker's stream API. asset_source: flash needs no
// storage at all, and storage_worker.h is itself behind this define, so the whole half is
// conditional. Codegen requests the worker whenever asset_source: storage is configured.
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

namespace esphome {
namespace web_server {

// Serves the advanced file browser's assets, always out of PSRAM.
//
// PSRAM is the only place these are ever read from: it is external RAM, there is a lot of it
// on the variants this component is limited to, and it keeps ~300 kB of markup out of both
// internal RAM and the response path. What differs is where the bytes come from before they
// get there:
//
//   FLASH   -- a gzipped copy is compiled in as PROGMEM and copied to PSRAM once at setup().
//             Always available, costs the flash.
//   STORAGE -- the files sit on a PathStorage and are read into PSRAM as soon as that storage
//             is mounted, which may be seconds after boot or never. Costs no flash; the
//             browser answers 503 until the read succeeds.
//
// Either way the served bytes are the same gzipped blob and the response is identical, so the
// choice is a deployment question and not a functional one.
class FileExplorerAssets : public Component {
 public:
  // One asset: a URL, its type, and where its bytes come from.
  struct Asset {
    const char *url;              // "/file-explorer/file-explorer.js"
    const char *content_type;     // "text/javascript"
    const uint8_t *flash;         // gzipped bytes in PROGMEM, or nullptr for STORAGE
    size_t flash_len;             // valid when flash != nullptr
    const char *storage_path;     // "/sdcard/fe/file-explorer.js.gz", or nullptr for FLASH
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

#ifdef USE_STORAGE_WORKER
  // Storage-backed assets are read through the worker's stream API, so the read is chunked,
  // runs off the main loop where the storage allows it, and is not subject to the blocking
  // helpers' max_blocking_transfer_size ceiling -- which is what rejected these files before:
  // the ceiling exists precisely to route bulk reads here (see storage.h).
  //
  // One asset is in flight at a time. Not a limitation worth engineering around: there are at
  // most a handful, they are read once at startup, and a single stream slot keeps this from
  // competing with the file API for the worker's pool.
  void start_load_(size_t index);
  void on_open_(storage::StorageError err);
  void on_read_(storage::StorageError err);
  void on_closed_(storage::StorageError err);
  // Releases the in-flight buffer and clears the slot, so loop() can try again later.
  void abandon_load_(const char *reason, storage::StorageError err);
  // Called once the last storage-backed asset has landed.
  void finish_if_complete_();

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
  // The destination buffer stays owned here until the asset is complete: read_chunk() needs it
  // to stay valid until its callback fires, and a failed read must free it rather than publish
  // a half-filled asset.
  uint8_t *pending_buf_{nullptr};
  size_t pending_len_{0};
  size_t pending_off_{0};
  // read_chunk() fills this before invoking the callback, so it has to outlive the call.
  size_t last_read_{0};
#endif
};

}  // namespace web_server
}  // namespace esphome

#endif  // USE_WEBSERVER_FILE_EXPLORER && USE_ESP32
