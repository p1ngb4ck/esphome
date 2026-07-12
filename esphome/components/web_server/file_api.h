#pragma once

#include "esphome/core/defines.h"

// Compiled in only when `web_server: file_api:` is configured (codegen sets the define) and
// only on the ESP-IDF backend — codegen validation rejects Arduino and web_server versions
// other than 3, so no runtime fallbacks live here.
#if defined(USE_WEBSERVER_FILE_API) && defined(USE_ESP_IDF)

#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <string>

#ifdef USE_WEBSERVER_FILE_BROWSER
// Gzipped browser module, embedded by codegen from file_browser.js. Global namespace on
// purpose: add_resource_as_progmem() defines the constexpr symbol at global scope in
// main.cpp — this extern declaration (visible there via this header) is what gives that
// definition external linkage, same pattern as web_server.h's CSS/JS_INCLUDE symbols.
extern const uint8_t ESPHOME_WEBSERVER_FILE_BROWSER_JS[] PROGMEM;
extern const size_t ESPHOME_WEBSERVER_FILE_BROWSER_JS_SIZE;
#endif

namespace esphome::web_server {

// REST file operations on top of the storage interface, served under /files/* as part of the
// existing web_server (same port, same auth once web_server grows one — not a separate page).
//
// Threading: httpd callbacks (handleRequest/handleUpload) run on the httpd server task, but
// the storage contract is main-loop-only for control-plane calls. Every storage access is
// therefore marshalled onto the main loop via Component::defer() — the documented thread-safe
// FIFO bridge, the same one web_server's entity actions use — while the httpd task blocks on
// a semaphore until the loop ran the operation. Chunked transfers (download/upload) repeat
// that per chunk, which is exactly the loop-sliced pacing the storage design prescribes.
class WebServerFileApi : public Component, public AsyncWebHandler {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_max_dir_entries(uint16_t n) { this->max_dir_entries_ = n; }
  // Optional single-storage scoping (mirrors http_file_api's storage_id option): when set,
  // only paths under this storage's mount path are served.
  void set_scoped_storage(storage::PathStorage *s) { this->scoped_storage_ = s; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

 protected:
  // Runs `op` on the main loop and blocks the calling (httpd) task until it completed.
  // Returns false on timeout — the op may still run later, so ops must only touch state that
  // stays valid (members of this component). See the class comment for the rationale.
  bool run_on_loop_(std::function<void()> &&op, uint32_t timeout_ms = 10000);

  // Path handling. resolve_() rejects missing/relative/'..'-containing paths and applies the
  // optional storage scope; on success it yields the storage and the driver-relative path.
  storage::PathStorage *resolve_(const char *vfs_path, const char **rel_out) const;
  static bool path_is_safe_(const char *path);

  // Endpoint handlers (each runs on the httpd task and marshals via run_on_loop_).
  void handle_storages_(AsyncWebServerRequest *request);
  void handle_list_(AsyncWebServerRequest *request);
  void handle_stat_(AsyncWebServerRequest *request);
  void handle_download_(AsyncWebServerRequest *request);
  void handle_mkdir_(AsyncWebServerRequest *request);
  void handle_delete_(AsyncWebServerRequest *request);
  void handle_copy_move_(AsyncWebServerRequest *request, bool is_move);
  void handle_job_(AsyncWebServerRequest *request);
  void handle_mount_(AsyncWebServerRequest *request, bool mount);
  void handle_upload_response_(AsyncWebServerRequest *request);

  static void send_error_(AsyncWebServerRequest *request, storage::StorageError err);
  static int http_status_for_(storage::StorageError err);

  web_server_base::WebServerBase *base_{nullptr};
  storage::PathStorage *scoped_storage_{nullptr};
  uint16_t max_dir_entries_{64};
  SemaphoreHandle_t op_done_{nullptr};

#ifdef USE_STORAGE_WORKER
  // Completed async transfers stay queryable after the worker recycles its pool slot: the
  // completion callback (main loop) parks the final status here; /files/job checks this cache
  // before asking the worker. Small ring, oldest entry overwritten.
  static constexpr size_t JOB_CACHE_SIZE = 8;
  struct JobCacheEntry {
    storage::TransferJob job{storage::INVALID_TRANSFER_JOB};
    storage::TransferStatus status{};
  };
  JobCacheEntry job_cache_[JOB_CACHE_SIZE]{};
  size_t job_cache_next_{0};
  void cache_job_result_(storage::TransferJob job, storage::StorageError result);
#endif

  // Per-request upload state. httpd serves uploads sequentially per handler instance; a
  // concurrent second upload is rejected with NOT_READY (active_ guard) instead of
  // interleaving handles.
  struct UploadState {
    bool active{false};
    storage::PathStorage *storage{nullptr};
    storage::FileHandle *handle{nullptr};
    bool handle_open{false};
    bool dst_is_fs{false};
    char rel_path[256]{};
    uint64_t offset{0};
    storage::StorageError error{storage::StorageError::OK};
  } upload_{};
};

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_FILE_API && USE_ESP_IDF
