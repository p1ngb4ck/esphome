#pragma once

#include "esphome/core/defines.h"

// Compiled in only when `web_server: raw_api:` is configured (codegen sets the define) and only
// on the ESP-IDF backend — same gating as the file API next door.
#if defined(USE_WEBSERVER_RAW_API) && defined(USE_ESP_IDF)

#include "esphome/components/storage/storage.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <string>

namespace esphome::web_server {

// Address-based HTTP access to raw media (NOR flash, FRAM, EEPROM) — the counterpart to the
// file API, which speaks paths a raw medium does not have.
//
// Endpoints (all under /raw/, device selected by its storage id):
//   GET  /raw/devices
//   GET  /raw/read?device=<id>&address=<n>&size=<n>   (or &all=1)
//   POST /raw/write?device=<id>&address=<n>[&erase=1] — payload is the raw request body
//   POST /raw/erase?device=<id>&address=<n>&size=<n>  (or &all=1)
//
// What a device accepts is never assumed: /raw/devices reports the geometry the driver itself
// reports (capacity, write page, erase units, capability bits), so a client only offers what
// the medium can actually do, and erase()'s verdict is passed through rather than smoothed over.
//
// Writing and erasing are off unless enabled in YAML: a stray click on a flash chip is not
// undoable, and this API has no confirmation of its own.
//
// Threading: handlers run on the httpd server task, but RawStorage drivers are main-loop-only
// by contract (binary_storage never sets STORAGE_CAP_IO_TASK_SAFE — they share their I2C/SPI
// bus with main-loop components), so every device call is marshalled with run_on_loop_(), the
// same way the file API does it.
class WebServerRawApi : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_enable_write(bool enable_write) { this->enable_write_ = enable_write; }
  void set_enable_erase(bool enable_erase) { this->enable_erase_ = enable_erase; }
  // Optional scope: with a device set, every other raw device stays invisible to this API.
  void set_scoped_device(storage::RawStorage *device) { this->scoped_device_ = device; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  // Runs `op` on the main loop and blocks the calling (httpd) task until it completed. Returns
  // false on timeout — the op may still run later, so it must only touch state that stays valid.
  bool run_on_loop_(std::function<void()> &&op, uint32_t timeout_ms = 10000);

  // Resolves the ?device= parameter against the registry (honoring the optional scope).
  storage::RawStorage *find_device_(AsyncWebServerRequest *request);
  // Parses ?address=/?size=/?all= and clamps them against the device's own geometry.
  bool parse_range_(AsyncWebServerRequest *request, storage::RawStorage *device, uint64_t *address, uint64_t *size);

  void handle_devices_(AsyncWebServerRequest *request);
  void handle_read_(AsyncWebServerRequest *request);
  void handle_erase_(AsyncWebServerRequest *request);

  web_server_base::WebServerBase *base_{nullptr};
  storage::RawStorage *scoped_device_{nullptr};
  bool enable_write_{false};
  bool enable_erase_{false};
  SemaphoreHandle_t op_done_{nullptr};

  // In-flight /raw/write. The body arrives in chunks on the httpd task; each is written at
  // address_ + index, so nothing is buffered beyond the chunk itself.
  struct WriteState {
    bool active{false};
    bool failed{false};
    storage::RawStorage *device{nullptr};
    uint64_t address{0};
    uint64_t written{0};
    const char *error{nullptr};
  } write_{};
};

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_RAW_API && USE_ESP_IDF
