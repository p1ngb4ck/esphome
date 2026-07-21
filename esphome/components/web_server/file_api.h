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

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "esphome/components/storage/transfer_buffer.h"

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
  // Upload callbacks marshal all storage work through run_on_loop_(), so the multipart
  // body may be received on the web server's async upload task instead of the httpd task.
  bool supportsAsyncUpload() const override { return true; }
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_max_dir_entries(uint16_t n) { this->max_dir_entries_ = n; }
  // Optional single-storage scoping (mirrors http_file_api's storage_id option): when set,
  // only paths under this storage's mount path are served.
  void set_scoped_storage(storage::PathStorage *s) { this->scoped_storage_ = s; }
  // Per-operation access gates. A disallowed request is answered with 403; the allowed set is
  // also advertised in /files/storages so the browser hides buttons that could only fail.
  void set_enable_list(bool enable) { this->enable_list_ = enable; }
  void set_enable_read(bool enable) { this->enable_read_ = enable; }
  void set_enable_write(bool enable) { this->enable_write_ = enable; }
  void set_enable_delete(bool enable) { this->enable_delete_ = enable; }
  void set_enable_mount(bool enable) { this->enable_mount_ = enable; }
  void set_enable_unmount(bool enable) { this->enable_unmount_ = enable; }

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
#ifdef USE_STORAGE_WORKER
  // Recursive directory transfer orchestrator: copies one file at a time through the async
  // worker (main loop stays responsive), directories via mkdir + descend. Memory-bounded:
  // no entry lists — entry #i of a directory is found by re-enumerating with a counting
  // callback (the same RAM-free trick remove_recursive() uses, O(n²) listings), and the
  // walker keeps one subpath buffer plus a per-level index stack instead of paths per level.
  // For moves, each file is deleted right after its copy completed and each directory is

#endif
  void handle_job_(AsyncWebServerRequest *request);
  void handle_mount_(AsyncWebServerRequest *request, bool mount);
  // Sends a 403 with a small JSON body; `what` names the disallowed operation group for the log.
  void send_forbidden_(AsyncWebServerRequest *request, const char *what);
  void handle_upload_response_(AsyncWebServerRequest *request);

  static void send_error_(AsyncWebServerRequest *request, storage::StorageError err);
  static int http_status_for_(storage::StorageError err);

  web_server_base::WebServerBase *base_{nullptr};
  storage::PathStorage *scoped_storage_{nullptr};
  uint16_t max_dir_entries_{64};
  // All default true: an instance constructed without explicit setters (shouldn't happen via
  // codegen, but keeps the default permissive) behaves as before.
  bool enable_list_{true};
  bool enable_read_{true};
  bool enable_write_{true};
  bool enable_delete_{true};
  bool enable_mount_{true};
  bool enable_unmount_{true};
  SemaphoreHandle_t op_done_{nullptr};

  // Everything this component does in loop() — walking a directory transfer, draining a staged
  // upload — only happens when Application's component phase runs, and that phase is gated
  // (loop_interval_, a high-frequency request, or an explicit wake). Nothing else asks for it:
  // an HTTP request drives nothing, it only reads status, and the browser sends nothing more
  // once a transfer is under way. Held while there is work, released when there is none.
  HighFrequencyLoopRequester loop_requester_;

  // /files/changes — serves the storage registry's directory-change feed (see storage.h:
  // note_dir_changed()): the ring itself lives there so the worker and the raw API feed it
  // too; this endpoint only reads it against the client's cursor.
  void handle_changes_(AsyncWebServerRequest *request);

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

  // /files/job serves two kinds of id, told apart by their top two bits: the worker's own
  // TransferJob handles are small counters and leave both clear, anything tagged here is ours.
  // Keep every id space in this one place — two of them silently shared a single flag bit
  // once, and every job answered 404 because the wrong branch claimed it.
  static constexpr uint32_t JOB_SPACE_MASK = 0xC0000000u;
  static constexpr uint32_t JOB_COUNTER_MASK = 0x3FFFFFFFu;

#endif

  // Per-request upload state. httpd serves uploads sequentially per handler instance; a
  // concurrent second upload is rejected with NOT_READY (active_ guard) instead of
  // interleaving handles.
  // --- Async download pipeline. The one-and-only httpd server task hands finished-
  // validated downloads to a single bounded transfer task via httpd_req_async_handler_begin(),
  // so the server task (and with it the v3 event stream) stays responsive during transfers.
  // Storage access keeps marshalling through run_on_loop_() — storage is main-loop-only.
  struct DownloadJob {
    httpd_req_t *req{nullptr};  // async request copy; owns the socket until complete()
    storage::PathStorage *ps{nullptr};
    storage::FileHandle *handle{nullptr};  // FILESYSTEM only
    bool is_fs{false};
    uint64_t size{0};
    char rel[256]{};
    // httpd_resp_set_hdr() stores the pointer (no copy) — must outlive every send.
    char disposition[300]{};
  };
  static constexpr size_t DL_QUEUE_DEPTH = 4;
  void pump_download_(DownloadJob *job);
  static void download_task_trampoline_(void *arg);
  void download_task_();
  QueueHandle_t dl_queue_{nullptr};
  TaskHandle_t dl_task_{nullptr};

  struct UploadState {
    bool active{false};
    storage::PathStorage *storage{nullptr};
    storage::FileHandle *handle{nullptr};
    bool handle_open{false};
    bool dst_is_fs{false};
    char rel_path[256]{};
    uint64_t offset{0};
    storage::StorageError error{storage::StorageError::OK};
    // True once a staged upload handed its bytes to flush_: completion (and the directory-
    // change note) then belongs to the flush drain in loop(), not to the request's end.
    bool staged_handoff{false};
#ifdef USE_STORAGE_TRANSFER_BUFFER
    // Borrowed PSRAM arena: data callbacks memcpy at network speed instead of one
    // main-loop hop per chunk; nullptr keeps the plain streaming path.
    uint8_t *staged{nullptr};
    size_t staged_cap{0};
#endif
  } upload_{};

#ifdef USE_STORAGE_TRANSFER_BUFFER
  // Post-response flush of a staged upload: loop() drains the arena to storage chunk-wise
  // (storage is main-loop-only by contract), queryable through /files/job. Its own id space
  // (see JOB_SPACE_MASK) — distinct from the directory transfers' as well as the worker's.
  static constexpr uint32_t FLUSH_JOB_FLAG = 0xC0000000u;
  struct StagedFlush {
    bool active{false};
    bool finished{false};
    uint32_t job{0};
    storage::PathStorage *storage{nullptr};
    storage::FileHandle *handle{nullptr};
    bool dst_is_fs{false};
    char rel_path[256]{};
    const uint8_t *data{nullptr};
    size_t total{0};
    size_t done{0};
    storage::StorageError result{storage::StorageError::OK};
  } flush_{};
  uint32_t flush_job_counter_{0};
#endif
};

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_FILE_API && USE_ESP_IDF
