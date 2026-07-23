#pragma once
#include "esphome/core/defines.h"
#if defined(USE_WEBSERVER_FILE_EXPLORER) && defined(USE_ESP32)

#include <cstddef>
#include <cstdint>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome {
namespace web_server {

// Serves the advanced file browser's assets, always out of PSRAM.
//
// PSRAM is the only place these are ever read from: it is external RAM, there is a lot of it
// on the variants this component is limited to, and it keeps ~300 kB of markup out of both
// internal RAM and the response path. What differs is where the bytes come from before they
// get there:
//
//   FLASH   — a gzipped copy is compiled in as PROGMEM and copied to PSRAM once at setup().
//             Always available, costs the flash.
//   STORAGE — the files sit on a PathStorage and are read into PSRAM as soon as that storage
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

  void set_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  // Codegen emits one call per asset; the array it points at is a static in the generated
  // code, so this object never owns it.
  void set_assets(Asset *assets, size_t count) {
    this->assets_ = assets;
    this->asset_count_ = count;
  }

  // True once every asset has its bytes in PSRAM. Until then the handler answers 503.
  bool ready() const { return this->ready_; }

  // Handles a GET for one of the asset URLs. Returns false when the URL is not ours.
  bool handle(AsyncWebServerRequest *request);
  bool matches(const std::string &url) const;

 protected:
  // Copies a PROGMEM asset into PSRAM. Returns false when PSRAM could not be had.
  bool load_from_flash_(Asset &asset);
  // Reads a STORAGE asset into PSRAM if its storage is mounted. Returns false when the file
  // is not available yet — that is not an error, loop() tries again.
  bool load_from_storage_(Asset &asset);

  web_server_base::WebServerBase *base_{nullptr};
  Asset *assets_{nullptr};
  size_t asset_count_{0};
  bool ready_{false};
  // Only set once, so a storage that never appears does not log on every loop.
  bool warned_pending_{false};
  uint32_t last_try_ms_{0};
};

}  // namespace web_server
}  // namespace esphome

#endif  // USE_WEBSERVER_FILE_EXPLORER && USE_ESP32
