#include "file_api.h"

#if defined(USE_WEBSERVER_FILE_API) && defined(USE_ESP_IDF)

#include "esphome/components/web_server_idf/web_server_idf.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <esp_http_server.h>

#include <cinttypes>
#include <cstring>

namespace esphome::web_server {

static const char *const TAG = "web_server.file_api";

// Chunk size for download/upload marshalling. Each chunk is one main-loop hop; small enough
// to keep individual blocking storage calls short (the storage contract), large enough to
// keep HTTP throughput reasonable.
static constexpr size_t FILE_API_CHUNK = 4096;

// ---------------------------------------------------------------------------
// Setup / dispatch
// ---------------------------------------------------------------------------

void WebServerFileApi::setup() {
  // Async downloads (tier 1): bounded pipeline of one transfer task + small pointer queue.
  this->dl_queue_ = xQueueCreate(DL_QUEUE_DEPTH, sizeof(DownloadJob *));
  if (this->dl_queue_ != nullptr) {
    xTaskCreate(WebServerFileApi::download_task_trampoline_, "fileapi_dl", 6144, this, 3, &this->dl_task_);
  }
  this->op_done_ = xSemaphoreCreateBinary();
  this->base_->add_handler(this);
}

void WebServerFileApi::dump_config() {
  ESP_LOGCONFIG(TAG, "File API:\n  Endpoints: /files/*\n  Max dir entries: %u", this->max_dir_entries_);
  if (this->scoped_storage_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Scoped to: %s", this->scoped_storage_->get_mount_path());
  }
#ifdef USE_WEBSERVER_FILE_BROWSER
  ESP_LOGCONFIG(TAG, "  File browser module: enabled (/file_browser.js)");
#endif
}

bool WebServerFileApi::canHandle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  const auto method = request->method();
#ifdef USE_WEBSERVER_FILE_BROWSER
  if (method == HTTP_GET && url == "/file_browser.js")
    return true;
#endif
#ifdef USE_WEBSERVER_FILE_EXPLORER
  if (method == HTTP_GET && this->file_explorer_ != nullptr && this->file_explorer_->find(url) != nullptr)
    return true;
#endif
  if (strncmp(url.c_str(), "/files/", 7) != 0)
    return false;
  return method == HTTP_GET || method == HTTP_POST;
}

void WebServerFileApi::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  const bool is_get = request->method() == HTTP_GET;

#ifdef USE_WEBSERVER_FILE_BROWSER
  if (is_get && url == "/file_browser.js") {
    auto *response = request->beginResponse(200, "text/javascript", ESPHOME_WEBSERVER_FILE_BROWSER_JS,
                                            ESPHOME_WEBSERVER_FILE_BROWSER_JS_SIZE);
    response->addHeader("Content-Encoding", "gzip");
    response->addHeader("Cache-Control", "public, max-age=3600");
    request->send(response);
    return;
  }
#endif

#ifdef USE_WEBSERVER_FILE_EXPLORER
  if (is_get && this->file_explorer_ != nullptr) {
    const auto *asset = this->file_explorer_->find(url);
    if (asset != nullptr) {
      if (asset->psram != nullptr) {
        // Already resident in PSRAM: loaded ONCE -- compiled in for flash, preloaded via
        // storage::read_file() for storage -- and served straight from that buffer on every
        // request, no per-request storage read. The bytes go out through the same async transfer
        // task the file downloads use (a single blocking send of a big buffer returns 500,
        // chunked async does not), as a memory-source job.
        auto *job = new DownloadJob{};  // NOLINT(cppcoreguidelines-owning-memory)
        job->mem = asset->psram;
        job->mem_len = asset->len;
        job->type = asset->content_type;
        job->gzip = asset->gzipped;

        httpd_req_t *areq = *request;
        httpd_req_t *async_req = nullptr;
        if (this->dl_task_ == nullptr || httpd_req_async_handler_begin(areq, &async_req) != ESP_OK) {
          // No async slot -- degrade to a synchronous chunked pump (still not a single-shot send).
          job->req = areq;
          this->pump_download_(job);
          delete job;  // NOLINT(cppcoreguidelines-owning-memory)
          return;
        }
        job->req = async_req;
        if (xQueueSend(this->dl_queue_, &job, 0) != pdTRUE) {
          httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer queue full");
          httpd_req_async_handler_complete(async_req);
          delete job;  // NOLINT(cppcoreguidelines-owning-memory)
          return;
        }
        return;
      }
      if (asset->flash == nullptr) {
        // Storage-backed and NOT preloaded (no PSRAM to preload into, or it is not mounted yet):
        // fall back to serving it straight from the medium through /files/download, which streams
        // any storage file to the browser. This reads per request -- the slow path -- and only
        // runs when the PSRAM preload could not.
        std::string loc = "/files/download?inline=1&path=";
        for (const char *p = asset->storage_path; *p != '\0'; p++) {
          const unsigned char ch = static_cast<unsigned char>(*p);
          const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '-' || ch == '_';
          if (safe) {
            loc += static_cast<char>(ch);
          } else {
            static const char HEX[] = "0123456789ABCDEF";
            loc += '%';
            loc += HEX[ch >> 4];
            loc += HEX[ch & 0x0F];
          }
        }
        request->redirect(loc);
        return;
      }
      // Flash asset whose PSRAM copy is not up yet (only during early setup) -- ask the browser
      // to retry.
      auto *pending = request->beginResponse(503, "text/plain", "assets not loaded yet");
      pending->addHeader("Retry-After", "5");
      request->send(pending);
      return;
    }
  }
#endif

  // Per-operation access enforced here, at the single dispatch point. Endpoints not listed
  // (/files/storages, /files/changes, /files/job) stay open: they are the browser's bootstrap
  // and status channels, not data operations. A disallowed operation is rejected with 403.
  if (is_get) {
    if (url == "/files/storages")
      return this->handle_storages_(request);
    if (url == "/files/list")
      return this->enable_list_ ? this->handle_list_(request) : this->send_forbidden_(request, "list");
    if (url == "/files/stat")
      return this->enable_list_ ? this->handle_stat_(request) : this->send_forbidden_(request, "list");
    if (url == "/files/download")
      return this->enable_read_ ? this->handle_download_(request) : this->send_forbidden_(request, "read");
    if (url == "/files/job")
      return this->handle_job_(request);
    if (url == "/files/changes")
      return this->handle_changes_(request);
  } else {
    if (url == "/files/mkdir")
      return this->enable_write_ ? this->handle_mkdir_(request) : this->send_forbidden_(request, "write");
    if (url == "/files/delete")
      return this->enable_delete_ ? this->handle_delete_(request) : this->send_forbidden_(request, "delete");
    if (url == "/files/copy")  // copy reads the source and writes a new file: a read operation here
      return this->enable_read_ ? this->handle_copy_move_(request, false) : this->send_forbidden_(request, "read");
    if (url == "/files/move")  // move deletes the source, so it is gated as a delete
      return this->enable_delete_ ? this->handle_copy_move_(request, true) : this->send_forbidden_(request, "delete");
    if (url == "/files/mount")
      return this->enable_mount_ ? this->handle_mount_(request, true) : this->send_forbidden_(request, "mount");
    if (url == "/files/unmount")
      return this->enable_unmount_ ? this->handle_mount_(request, false) : this->send_forbidden_(request, "unmount");
    if (url == "/files/format")
      return this->enable_format_ ? this->handle_format_(request) : this->send_forbidden_(request, "format");
    if (url == "/files/upload")
      return this->enable_write_ ? this->handle_upload_response_(request) : this->send_forbidden_(request, "write");
  }
  request->send(404);
}

void WebServerFileApi::send_forbidden_(AsyncWebServerRequest *request, const char *what) {
  ESP_LOGD(TAG, "Rejecting '%s' request: disabled by config", what);
  request->send(403, "application/json", "{\"error\":\"operation disabled\"}");
}

// ---------------------------------------------------------------------------
// Main-loop marshalling
// ---------------------------------------------------------------------------

bool WebServerFileApi::run_on_loop_(std::function<void()> &&op, uint32_t timeout_ms) {
  // defer() is the documented thread-safe FIFO bridge into the main loop (dedicated defer
  // queue in the scheduler); web_server's own entity actions use the same mechanism from
  // this very httpd task.
  SemaphoreHandle_t done = this->op_done_;
  this->defer([op = std::move(op), done]() {
    op();
    xSemaphoreGive(done);
  });
  // Wait WITHOUT an arbitrary deadline. Every op captures references into this handler's
  // stack frame (&err, &json, result buffers) -- a timed-out wait returned into a frame the
  // still-queued op would later write through: undefined behavior that surfaced as transfers
  // frozen at 0 bytes and, after one slow storage call, a wedged file API for good. The op
  // is bounded by the storage contract (short blocking calls; the NFS inline mount caps at
  // 8 s), and if the main loop truly never runs it, the device is gone anyway -- a stuck
  // httpd task is then the honest symptom, not corrupted memory.
  (void) timeout_ms;
  while (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
    // Re-take in slices so a genuinely long op does not trip esp_http_server's socket
    // supervision silently -- the loop is purely a wait, never a bail-out.
  }
  return true;
}

// ---------------------------------------------------------------------------
// Path handling / error mapping
// ---------------------------------------------------------------------------

bool WebServerFileApi::path_is_safe_(const char *path) {
  if (path == nullptr || path[0] != '/')
    return false;
  // Reject any '..' segment -- resolve_path() does longest-prefix matching on the string, so
  // traversal must be stopped before it reaches a driver.
  return strstr(path, "..") == nullptr;
}

storage::PathStorage *WebServerFileApi::resolve_(const char *vfs_path, const char **rel_out) const {
  if (!path_is_safe_(vfs_path))
    return nullptr;
  if (storage::global_storage_registry == nullptr)
    return nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(vfs_path, rel_out);
  if (ps == nullptr)
    return nullptr;
  if (this->scoped_storage_ != nullptr && ps != this->scoped_storage_)
    return nullptr;
  return ps;
}

int WebServerFileApi::http_status_for_(storage::StorageError err) {
  // The exists()/stat() lesson applies HTTP-wide: a faulted/unmounted medium must never be
  // reported as absence -- NOT_READY is 503, only NOT_FOUND is 404.
  switch (err) {
    case storage::StorageError::OK:
      return 200;
    case storage::StorageError::NOT_FOUND:
      return 404;
    case storage::StorageError::NOT_READY:
      return 503;
    case storage::StorageError::PERMISSION_DENIED:
      return 403;
    case storage::StorageError::NOT_EMPTY:
    case storage::StorageError::ALREADY_EXISTS:
      return 409;
    case storage::StorageError::TRANSFER_TOO_LARGE:
      return 413;
    case storage::StorageError::INVALID_ARGS:
      return 400;
    default:
      return 500;
  }
}

void WebServerFileApi::send_error_(AsyncWebServerRequest *request, storage::StorageError err) {
  std::string body = "{\"error\":\"";
  body += storage::error_to_string(err);
  body += "\"}";
  request->send(http_status_for_(err), "application/json", body.c_str());
}

// Minimal JSON string escaping for file names (quotes and backslashes; control chars dropped).
// Brings a VFS path into exactly one shape: duplicate separators collapsed, trailing ones
// stripped (a lone "/" survives). Done once at the edge, before anything compares or splits it.
//
// FatFs itself tolerates "//" and "dir/" (it skips duplicate separators and ignores a
// terminating one), which is why this looked harmless -- but our own string logic does not: the
// self-copy guard below compares prefixes, and "from=/a/" made "/a/b" look like it was NOT
// inside the source. The registry's rel path carries the same slashes on to the drivers, and
// network ones have no reason to be as forgiving as FatFs.
static void normalize_vfs_path(std::string &path) {
  std::string out;
  out.reserve(path.size());
  bool prev_slash = false;
  for (char ch : path) {
    if (ch == '/') {
      if (prev_slash)
        continue;
      prev_slash = true;
    } else {
      prev_slash = false;
    }
    out += ch;
  }
  while (out.size() > 1 && out.back() == '/')
    out.pop_back();
  path = std::move(out);
}

static void append_json_escaped(std::string &out, const char *s) {
  for (; *s != '\0'; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      out += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Simple endpoints
// ---------------------------------------------------------------------------

void WebServerFileApi::handle_storages_(AsyncWebServerRequest *request) {
  std::string json;
  bool ok = this->run_on_loop_([this, &json]() {
    // Advertise the per-operation access this build allows, so the browser only renders buttons
    // that can actually work (a disabled operation is also enforced server-side with 403).
    json = "{\"access\":{\"list\":";
    json += this->enable_list_ ? "true" : "false";
    json += ",\"read\":";
    json += this->enable_read_ ? "true" : "false";
    json += ",\"write\":";
    json += this->enable_write_ ? "true" : "false";
    json += ",\"delete\":";
    json += this->enable_delete_ ? "true" : "false";
    json += ",\"mount\":";
    json += this->enable_mount_ ? "true" : "false";
    json += ",\"unmount\":";
    json += this->enable_unmount_ ? "true" : "false";
    json += ",\"format\":";
    json += this->enable_format_ ? "true" : "false";
    json += "},\"storages\":[";
    struct Ctx {
      std::string *out;
      storage::PathStorage *scope;
      bool first{true};
    } ctx{&json, this->scoped_storage_};
    storage::global_storage_registry->for_each_path_based(
        [](storage::PathStorage *s, storage::StorageType type, void *c) {
          auto *ctx = static_cast<Ctx *>(c);
          if (ctx->scope != nullptr && s != ctx->scope)
            return;
          if (!ctx->first)
            *ctx->out += ',';
          ctx->first = false;
          *ctx->out += "{\"mount_path\":\"";
          append_json_escaped(*ctx->out, s->get_mount_path());
          *ctx->out += "\",\"type\":\"";
          *ctx->out += type == storage::StorageType::NETWORK ? "network" : "filesystem";
          // Close the type string value, then per-direction capabilities: a plain "mountable"
          // bool cannot express drivers whose medium mounts itself (USB hotplug) and only
          // supports safe-eject -- the UI must gate each button separately (see
          // MountableStorage::get_mount_caps()).
          storage::MountableStorage *m = s->as_mountable();
          uint8_t caps = m != nullptr ? m->get_mount_caps() : 0;
          *ctx->out += "\",\"can_mount\":";
          *ctx->out += (caps & storage::MountableStorage::MOUNT_CAP_MOUNT) != 0 ? "true" : "false";
          *ctx->out += ",\"can_unmount\":";
          *ctx->out += (caps & storage::MountableStorage::MOUNT_CAP_UNMOUNT) != 0 ? "true" : "false";
          storage::StorageInfo info{};
          storage::StorageError info_err = s->get_info(&info);
          // is_mounted is filled by drivers even on NOT_READY (unmounted); the zero-initialized
          // struct covers any other failure, so the flag is always safe to emit.
          *ctx->out += ",\"mounted\":";
          *ctx->out += info.is_mounted ? "true" : "false";
          *ctx->out += ",\"can_format\":";
          *ctx->out += s->as_filesystem() != nullptr ? "true" : "false";
          if (info_err == storage::StorageError::OK) {
            *ctx->out += ",\"name\":\"";
            append_json_escaped(*ctx->out, info.name != nullptr ? info.name : "");
            *ctx->out += "\"";
            // Optional: only drivers that report a medium kind emit it (see StorageInfo).
            if (info.kind != nullptr) {
              *ctx->out += ",\"kind\":\"";
              append_json_escaped(*ctx->out, info.kind);
              *ctx->out += "\"";
            }
            // Medium capacity for the browser's storage properties (used = total - free). Drivers
            // that cannot report sizes leave the zero-initialized struct; suppressed then so the
            // UI shows nothing instead of "0 B".
            if (info.total_bytes > 0) {
              char sz[72];
              snprintf(sz, sizeof(sz), ",\"total_bytes\":%llu,\"free_bytes\":%llu",
                       (unsigned long long) info.total_bytes, (unsigned long long) info.free_bytes);
              *ctx->out += sz;
            }
          }
          *ctx->out += '}';
        },
        &ctx);
    json += "]}";
  });
  if (!ok) {
    request->send(504);
    return;
  }
  request->send(200, "application/json", json.c_str());
}

void WebServerFileApi::handle_list_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  std::string json;
  storage::StorageError err = storage::StorageError::OK;
  bool ok = this->run_on_loop_([this, &path, &json, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    struct Ctx {
      std::string *out;
      uint16_t remaining;
      bool truncated{false};
      bool first{true};
    } ctx{&json, this->max_dir_entries_};
    json = "{\"entries\":[";
    err = ps->list_dir(
        rel,
        [](const storage::FileStat *entry, void *c) -> bool {
          auto *ctx = static_cast<Ctx *>(c);
          if (ctx->remaining == 0) {
            ctx->truncated = true;
            return false;  // stop enumeration -- not an error per the list_dir contract
          }
          ctx->remaining--;
          if (!ctx->first)
            *ctx->out += ',';
          ctx->first = false;
          *ctx->out += "{\"name\":\"";
          append_json_escaped(*ctx->out, entry->name);
          char buf[64];
          snprintf(buf, sizeof(buf), "\",\"size\":%" PRIu64 ",\"is_dir\":%s,\"mtime\":%" PRIu32 "}", entry->size,
                   entry->is_dir ? "true" : "false", entry->mtime);
          *ctx->out += buf;
          return true;
        },
        &ctx);
    json += "],\"truncated\":";
    json += ctx.truncated ? "true" : "false";
    json += '}';
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  request->send(200, "application/json", json.c_str());
}

void WebServerFileApi::handle_stat_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  storage::FileStat st{};
  storage::StorageError err = storage::StorageError::OK;
  bool ok = this->run_on_loop_([this, &path, &st, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    err = ps->stat(rel, &st);
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  std::string json = "{\"name\":\"";
  append_json_escaped(json, st.name);
  char buf[64];
  snprintf(buf, sizeof(buf), "\",\"size\":%" PRIu64 ",\"is_dir\":%s,\"mtime\":%" PRIu32 "}", st.size,
           st.is_dir ? "true" : "false", st.mtime);
  json += buf;
  request->send(200, "application/json", json.c_str());
}

void WebServerFileApi::handle_changes_(AsyncWebServerRequest *request) {
  uint32_t since = 0;
  auto *param = request->getParam("since");
  if (param != nullptr)
    since = strtoul(param->value().c_str(), nullptr, 10);
  std::string json;
  bool ok = this->run_on_loop_([since, &json]() {
    auto *reg = storage::global_storage_registry;
    if (reg == nullptr) {
      json = "{\"seq\":0,\"dirs\":[]}";
      return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"seq\":%" PRIu32, reg->change_seq());
    json = buf;
    if (since == 0) {
      // First contact: the client renders fresh anyway -- it only needs a cursor.
      json += ",\"dirs\":[]}";
      return;
    }
    uint32_t oldest = 0;
    for (size_t i = 0; i < storage::StorageRegistry::DIR_CHANGES_SIZE; i++) {
      const auto &e = reg->dir_change(i);
      if (e.seq != 0 && (oldest == 0 || e.seq < oldest))
        oldest = e.seq;
    }
    if (oldest != 0 && oldest > since + 1) {
      // Entries between the cursor and the ring's oldest were evicted -- what exactly changed
      // is unknowable, so the client is told to consider everything it shows dirty.
      json += ",\"reset\":true,\"dirs\":[]}";
      return;
    }
    json += ",\"dirs\":[";
    bool first = true;
    for (size_t i = 0; i < storage::StorageRegistry::DIR_CHANGES_SIZE; i++) {
      const auto &e = reg->dir_change(i);
      if (e.seq == 0 || e.seq <= since)
        continue;
      // The ring's coalescing only merges adjacent repeats; dedupe the rest while emitting.
      bool dup = false;
      for (size_t j = 0; j < i && !dup; j++)
        dup = reg->dir_change(j).seq > since && reg->dir_change(j).dir == e.dir;
      if (dup)
        continue;
      if (!first)
        json += ",";
      first = false;
      json += "\"";
      append_json_escaped(json, e.dir.c_str());
      json += "\"";
    }
    json += "]}";
  });
  if (!ok) {
    request->send(504);
    return;
  }
  request->send(200, "application/json", json.c_str());
}

void WebServerFileApi::handle_mkdir_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  storage::StorageError err = storage::StorageError::OK;
  bool ok = this->run_on_loop_([this, &path, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    err = ps->mkdir(rel);
    if (err == storage::StorageError::OK)
      storage::global_storage_registry->note_parent_changed(path);
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  request->send(200, "application/json", "{}");
}

void WebServerFileApi::handle_delete_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  auto *rec = request->getParam("recursive");
  bool recursive = rec != nullptr && rec->value() == "1";
  storage::StorageError err = storage::StorageError::OK;
  bool ok = this->run_on_loop_([this, &path, recursive, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    err = recursive ? storage::remove_recursive(ps, rel) : ps->remove(rel);
    if (err == storage::StorageError::OK)
      storage::global_storage_registry->note_parent_changed(path);
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  request->send(200, "application/json", "{}");
}

void WebServerFileApi::handle_mount_(AsyncWebServerRequest *request, bool mount) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  storage::StorageError err = storage::StorageError::OK;
  bool found_not_mountable = false;
  bool ok = this->run_on_loop_([this, &path, mount, &err, &found_not_mountable]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr || rel[0] != '\0') {  // must target a mount path exactly, not a file
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    storage::MountableStorage *m = ps->as_mountable();
    // Gate on the per-direction capability, not just interface presence: e.g. USB storage
    // auto-mounts on insertion and only supports the safe-eject direction.
    uint8_t need = mount ? storage::MountableStorage::MOUNT_CAP_MOUNT : storage::MountableStorage::MOUNT_CAP_UNMOUNT;
    if (m == nullptr || (m->get_mount_caps() & need) == 0) {
      found_not_mountable = true;
      return;
    }
    if (mount) {
#ifdef USE_STORAGE_WORKER
      // Async-first: the worker routes the blocking mount work per driver capability (task for
      // task-safe media, loop-sliced otherwise). The HTTP reply only acknowledges the request;
      // the storages poll and the change feed pick up the state flip when the mount completes.
      if (storage::global_storage_worker != nullptr) {
        err = storage::global_storage_worker->async_mount(
            ps,
            [](storage::StorageError r) {
              if (r == storage::StorageError::OK && storage::global_storage_registry != nullptr)
                storage::global_storage_registry->note_dir_changed("");  // roots level: a mount arrived
            },
            nullptr);
        return;
      }
#endif
      err = m->mount();
    } else {
      // Unmount stays synchronous: the driver's quiesce drain is owned by the main loop.
      err = m->unmount();
    }
    if (err == storage::StorageError::OK)
      storage::global_storage_registry->note_dir_changed("");  // the roots level: a mount came or went
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (found_not_mountable) {
    request->send(400, "application/json", "{\"error\":\"operation not supported by this storage\"}");
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  request->send(200, "application/json", "{}");
}

void WebServerFileApi::handle_format_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);
  storage::StorageError err = storage::StorageError::OK;
  bool not_formattable = false;
  bool still_mounted = false;
  bool ok = this->run_on_loop_([this, &path, &err, &not_formattable, &still_mounted]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr || rel[0] != '\0') {  // must target a mount path exactly, not a file
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    storage::FilesystemStorage *fs = ps->as_filesystem();
    if (fs == nullptr) {
      not_formattable = true;
      return;
    }
    // Format is destructive and operates on the raw volume: require the filesystem to be unmounted
    // first (the UI unmounts, then formats). Refuse while mounted rather than silently unmounting.
    storage::StorageInfo info{};
    if (fs->get_info(&info) == storage::StorageError::OK && info.is_mounted) {
      still_mounted = true;
      return;
    }
    err = fs->format();
    if (err == storage::StorageError::OK)
      storage::global_storage_registry->note_dir_changed("");  // the roots level: state changed
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (not_formattable) {
    request->send(400, "application/json", "{\"error\":\"operation not supported by this storage\"}");
    return;
  }
  if (still_mounted) {
    request->send(409, "application/json", "{\"error\":\"unmount before formatting\"}");
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  request->send(200, "application/json", "{}");
}

// ---------------------------------------------------------------------------
// Async transfers (copy/move + job status)
// ---------------------------------------------------------------------------

void WebServerFileApi::handle_copy_move_(AsyncWebServerRequest *request, bool is_move) {
#ifdef USE_STORAGE_WORKER
  auto *from = request->getParam("from");
  auto *to = request->getParam("to");
  if (from == nullptr || to == nullptr) {
    request->send(400);
    return;
  }
  std::string from_s = from->value();
  std::string to_s = to->value();
  normalize_vfs_path(from_s);
  normalize_vfs_path(to_s);
  // Existing destinations are refused, not silently replaced: same-storage moves went through
  // rename() (which cannot overwrite at all), while copies and cross-storage moves happily
  // truncated whatever was there -- the same command meant two different things depending on
  // which medium the destination sat on. Now both refuse with ALREADY_EXISTS (409) unless the
  // caller says otherwise.
  auto *ow = request->getParam("overwrite");
  const bool overwrite = ow != nullptr && ow->value() == "1";
  // Copying/moving a directory into itself would recurse forever -- reject early on the
  // full VFS strings ('/a' -> '/a/b' style; exact-prefix with a path boundary).
  if (to_s.size() > from_s.size() && to_s.compare(0, from_s.size(), from_s) == 0 && to_s[from_s.size()] == '/') {
    request->send(400, "application/json", "{\"error\":\"destination is inside the source\"}");
    return;
  }
  storage::StorageError err = storage::StorageError::OK;
  storage::TransferJob job = storage::INVALID_TRANSFER_JOB;
  bool ok = this->run_on_loop_([this, &from_s, &to_s, is_move, overwrite, &err, &job]() {
    const char *src_rel = nullptr;
    const char *dst_rel = nullptr;
    storage::PathStorage *src = this->resolve_(from_s.c_str(), &src_rel);
    storage::PathStorage *dst = this->resolve_(to_s.c_str(), &dst_rel);
    if (src == nullptr || dst == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    // Pure translator by architecture contract: no driver I/O happens here. Existence
    // checks, the overwrite decision, destination clearing and the tree-vs-file
    // classification all run inside the worker, in the engine context that owns the
    // storages (see run_chunk_'s pre-phase). This handler resolves, submits, answers.
    if (storage::global_storage_worker == nullptr) {
      err = storage::StorageError::NOT_SUPPORTED;
      return;
    }
    // A directory that cannot be renamed into place is a tree job -- the worker walks it. This
    // endpoint's part is over once the job is submitted: it hands back the id and nothing here
    // touches the transfer again. Asking for its status is optional and drives nothing.
    // Completion parks the final status in the job cache (this callback runs on the main
    // loop) -- the worker recycles its slot right after, so polling alone would miss DONE.
    // The job id only exists after submission, so the callback reads it through a small
    // heap slot filled right below; safe because both submission and completion run on the
    // main loop, strictly in that order. Freed by the callback (fires exactly once).
    auto *job_slot = new storage::TransferJob(storage::INVALID_TRANSFER_JOB);  // NOLINT
    // The change feed needs nothing from this callback: the worker notes every completed
    // transfer itself at its dispatch point -- including ones no HTTP request submitted.
    auto on_done = [this, job_slot](storage::StorageError result) {
      this->cache_job_result_(*job_slot, result);
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory)
    };
    storage::StorageWorker *w = storage::global_storage_worker;
    err = is_move ? w->async_move(src, src_rel, dst, dst_rel, on_done, &job, overwrite)
                  : w->async_copy(src, src_rel, dst, dst_rel, on_done, &job, overwrite);
    if (err != storage::StorageError::OK) {
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory) -- callback will not fire
    } else {
      *job_slot = job;
    }
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (err != storage::StorageError::OK) {
    send_error_(request, err);
    return;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"job\":%" PRIu32 "}", job);
  request->send(200, "application/json", buf);
#else
  request->send(501, "application/json", "{\"error\":\"storage worker not compiled in\"}");
#endif
}

#ifdef USE_STORAGE_WORKER
void WebServerFileApi::cache_job_result_(storage::TransferJob job, storage::StorageError result) {
  if (job == storage::INVALID_TRANSFER_JOB)
    return;
  JobCacheEntry &e = this->job_cache_[this->job_cache_next_];
  this->job_cache_next_ = (this->job_cache_next_ + 1) % JOB_CACHE_SIZE;
  e.job = job;
  e.status.state = storage::RequestState::DONE;
  e.status.result = result;
  // bytes are best-effort in the cache: the pool slot may already be recycled, so report
  // done == total == 0 (unknown) -- the UI switches to "finished" on state DONE anyway.
  e.status.bytes_done = 0;
  e.status.bytes_total = 0;
}
#endif

#ifdef USE_STORAGE_TRANSFER_BUFFER
void WebServerFileApi::cache_flush_result_(uint32_t job, storage::StorageError result, uint32_t done, uint32_t total) {
  FlushCacheEntry &e = this->flush_cache_[this->flush_cache_next_];
  this->flush_cache_next_ = (this->flush_cache_next_ + 1) % FLUSH_CACHE_SIZE;
  e.job = job;
  e.result = result;
  e.done = done;
  e.total = total;
}
#endif

void WebServerFileApi::loop() {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (this->flush_.active && !this->flush_.finished) {
    if (this->flush_.writing || this->flush_.closing) {
      // A worker write/close is in flight; its callback advances the drain. Nothing to do here.
    } else if (this->flush_.result == storage::StorageError::OK && this->flush_.done < this->flush_.total) {
      size_t chunk = this->flush_.total - this->flush_.done;
      if (chunk > 4 * FILE_API_CHUNK)
        chunk = 4 * FILE_API_CHUNK;
      this->flush_.writing = true;
      storage::StorageError rej = storage::global_storage_worker->write_chunk(
          this->flush_.stream, this->flush_.data + this->flush_.done, chunk,
          [this, chunk](storage::StorageError e) {
            this->flush_.writing = false;
            if (e == storage::StorageError::OK)
              this->flush_.done += chunk;  // the worker writes the whole chunk or reports an error
            else
              this->flush_.result = e;
          });
      if (rej != storage::StorageError::OK) {
        this->flush_.writing = false;
        this->flush_.result = rej;
      }
    } else {
      // All bytes written (or a write failed): close the stream through the worker, then finalize
      // (publish/rename, change note, cache) in the completion callback.
      this->flush_.closing = true;
      storage::StorageError rej =
          storage::global_storage_worker->end_write(this->flush_.stream, [this](storage::StorageError e) {
            this->flush_.closing = false;
            this->flush_.stream_open = false;
            if (this->flush_.result == storage::StorageError::OK)
              this->flush_.result = e;  // a close error surfaces
            this->finalize_flush_();
          });
      if (rej != storage::StorageError::OK) {
        this->flush_.closing = false;
        this->flush_.stream_open = false;
        if (this->flush_.result == storage::StorageError::OK)
          this->flush_.result = rej;
        this->finalize_flush_();
      }
    }
  }
#endif
}

#ifdef USE_STORAGE_TRANSFER_BUFFER
void WebServerFileApi::finalize_flush_() {
  storage::global_transfer_buffer->release();
  if (this->flush_.result == storage::StorageError::OK) {
    // Publish the staged upload atomically (rename temp -> final).
    this->flush_.result = this->publish_upload_(this->flush_.storage, this->flush_.rel_path,
                                                this->flush_.final_path, this->flush_.overwrite);
  }
  if (this->flush_.result != storage::StorageError::OK && this->flush_.storage != nullptr) {
    // Drop the temp so a failed publish does not leave it behind.
    this->flush_.storage->remove(this->flush_.rel_path);
  }
  this->flush_.finished = true;  // stays queryable via /files/job until the next staged upload
  this->cache_flush_result_(this->flush_.job, this->flush_.result, (uint32_t) this->flush_.done,
                            (uint32_t) this->flush_.total);
  if (this->flush_.result == storage::StorageError::OK && this->flush_.storage != nullptr) {
    storage::global_storage_registry->note_parent_changed(std::string(this->flush_.storage->get_mount_path()) +
                                                          "/" + this->flush_.final_path);
  }
  this->loop_requester_.stop();
  ESP_LOGD(TAG, "staged upload flushed: %u/%u bytes (%s)", (unsigned) this->flush_.done,
           (unsigned) this->flush_.total, storage::error_to_string(this->flush_.result));
}
#endif  // USE_STORAGE_TRANSFER_BUFFER

void WebServerFileApi::handle_job_(AsyncWebServerRequest *request) {
#ifdef USE_STORAGE_TRANSFER_BUFFER
  {
    auto *p = request->getParam("id");
    uint32_t fid = p != nullptr ? (uint32_t) strtoul(p->value().c_str(), nullptr, 10) : 0;
    if ((fid & JOB_SPACE_MASK) == FLUSH_JOB_FLAG) {
      char jbuf[128];
      if (this->flush_.active && this->flush_.job == fid) {
        if (this->flush_.finished) {
          snprintf(jbuf, sizeof(jbuf), "{\"state\":\"done\",\"result\":\"%s\",\"bytes_done\":%u,\"bytes_total\":%u}",
                   storage::error_to_string(this->flush_.result), (unsigned) this->flush_.done,
                   (unsigned) this->flush_.total);
        } else {
          snprintf(jbuf, sizeof(jbuf), "{\"state\":\"running\",\"bytes_done\":%u,\"bytes_total\":%u}",
                   (unsigned) this->flush_.done, (unsigned) this->flush_.total);
        }
        request->send(200, "application/json", jbuf);
        return;
      }
      // flush_ has already been reused by a newer upload -- resolve this finished job from the cache.
      for (const auto &e : this->flush_cache_) {
        if (e.job == fid) {
          snprintf(jbuf, sizeof(jbuf), "{\"state\":\"done\",\"result\":\"%s\",\"bytes_done\":%u,\"bytes_total\":%u}",
                   storage::error_to_string(e.result), (unsigned) e.done, (unsigned) e.total);
          request->send(200, "application/json", jbuf);
          return;
        }
      }
      request->send(404);
      return;
    }
  }
#endif
#ifdef USE_STORAGE_WORKER
  auto *param = request->getParam("id");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  auto job = static_cast<storage::TransferJob>(strtoul(param->value().c_str(), nullptr, 10));
  storage::TransferStatus st{};
  bool found = false;
  bool ok = this->run_on_loop_([this, job, &st, &found]() {
    // Cache first: a DONE job usually had its slot recycled already.
    for (const auto &e : this->job_cache_) {
      if (e.job == job) {
        st = e.status;
        found = true;
        return;
      }
    }
    if (storage::global_storage_worker != nullptr) {
      found = storage::global_storage_worker->get_transfer_status(job, &st);
    }
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (!found) {
    request->send(404, "application/json", "{\"error\":\"unknown or expired job\"}");
    return;
  }
  const char *state = "pending";
  if (st.state == storage::RequestState::RUNNING || st.state == storage::RequestState::CANCELLED) {
    state = "running";
  } else if (st.state == storage::RequestState::DONE) {
    state = "done";
  }
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"result\":\"%s\",\"bytes_done\":%" PRIu64 ",\"bytes_total\":%" PRIu64,
           state, storage::error_to_string(st.result), st.bytes_done, st.bytes_total);
  std::string json = buf;
  if (st.file[0] != '\0') {
    // The file currently in flight -- for a tree job the only place a percentage can honestly
    // come from, since the tree's bytes_total is unknown (see TransferStatus).
    json += ",\"file\":\"";
    append_json_escaped(json, st.file);
    snprintf(buf, sizeof(buf), "\",\"file_done\":%" PRIu64 ",\"file_total\":%" PRIu64, st.file_done, st.file_total);
    json += buf;
  }
  json += "}";
  request->send(200, "application/json", json.c_str());
#else
  request->send(501);
#endif
}

// ---------------------------------------------------------------------------
// Download (chunked, one main-loop hop per chunk)
// ---------------------------------------------------------------------------

// Extensions worth naming: the ones a browser can show inline. Everything else keeps the
// octet-stream every download used to get, so nothing that worked before changes.
const char *WebServerFileApi::content_type_for(const char *name) {
  const char *dot = strrchr(name, '.');
  if (dot == nullptr)
    return "application/octet-stream";
  struct Entry {
    const char *ext;
    const char *type;
  };
  static const Entry TABLE[] = {
      {".png", "image/png"},   {".jpg", "image/jpeg"},        {".jpeg", "image/jpeg"},     {".gif", "image/gif"},
      {".webp", "image/webp"}, {".bmp", "image/bmp"},         {".svg", "image/svg+xml"},   {".ico", "image/x-icon"},
      {".txt", "text/plain"},  {".log", "text/plain"},        {".yaml", "text/plain"},     {".yml", "text/plain"},
      {".conf", "text/plain"}, {".cfg", "text/plain"},        {".ini", "text/plain"},      {".csv", "text/csv"},
      {".md", "text/plain"},   {".json", "application/json"}, {".html", "text/html"},      {".htm", "text/html"},
      {".css", "text/css"},    {".js", "text/javascript"},    {".pdf", "application/pdf"}, {".wav", "audio/wav"},
      {".mp3", "audio/mpeg"},  {".flac", "audio/flac"},       {".ogg", "audio/ogg"},       {".mp4", "video/mp4"},
  };
  for (const auto &e : TABLE) {
    if (strcasecmp(dot, e.ext) == 0)
      return e.type;
  }
  return "application/octet-stream";
}

void WebServerFileApi::handle_download_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  normalize_vfs_path(path);

  storage::PathStorage *ps = nullptr;
  uint64_t size = 0;
  char rel_buf[storage::STORAGE_PATH_MAX]{};
  storage::StorageError err = storage::StorageError::OK;

  // Control plane only: resolve + stat are synchronous and polymorphic (any storage type). The
  // data read is the worker's job, so no handle is opened here.
  bool ok = this->run_on_loop_([this, &path, &ps, &size, &rel_buf, &err]() {
    const char *rel = nullptr;
    ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    strncpy(rel_buf, rel, sizeof(rel_buf) - 1);
    storage::FileStat st{};
    err = ps->stat(rel, &st);
    if (err != storage::StorageError::OK)
      return;
    if (st.is_dir) {
      err = storage::StorageError::INVALID_ARGS;
      return;
    }
    size = st.size;
  });
  if (!ok || err != storage::StorageError::OK) {
    if (!ok) {
      request->send(504);
    } else {
      send_error_(request, err);
    }
    return;
  }

  // Build the transfer job up front; the disposition buffer must live inside the job
  // because httpd_resp_set_hdr() keeps the pointer until the response is finished.
  auto *job = new DownloadJob{};  // NOLINT(cppcoreguidelines-owning-memory)
  // inline=1 asks the browser to render the file rather than save it, which is what an image
  // preview or a text editor wants. Absent, the response is byte-for-byte what it always was.
  const auto *inline_param = request->getParam("inline");
  const bool want_inline = inline_param != nullptr && inline_param->value() == "1";
  if (want_inline)
    job->type = content_type_for(path.c_str());
  // Range: bytes=START-[END]. Only the single-range form, which is all a browser sends for
  // media and all a log tail needs. Anything else is ignored and the whole file goes out.
  const optional<std::string> range_hdr = request->get_header("Range");
  if (range_hdr.has_value() && size > 0) {
    const std::string &spec = range_hdr.value();
    if (spec.rfind("bytes=", 0) == 0) {
      const size_t dash = spec.find('-', 6);
      if (dash != std::string::npos) {
        char *endp = nullptr;
        const uint64_t start = strtoull(spec.c_str() + 6, &endp, 10);
        const bool has_start = endp != spec.c_str() + 6;
        uint64_t end = size - 1;
        if (dash + 1 < spec.size()) {
          char *endp2 = nullptr;
          const uint64_t parsed = strtoull(spec.c_str() + dash + 1, &endp2, 10);
          if (endp2 != spec.c_str() + dash + 1)
            end = parsed;
        }
        if (has_start && start < size) {
          job->has_range = true;
          job->range_start = start;
          job->range_end = end < size ? end : size - 1;
          snprintf(job->content_range, sizeof(job->content_range), "bytes %llu-%llu/%llu",
                   (unsigned long long) job->range_start, (unsigned long long) job->range_end,
                   (unsigned long long) size);
        }
      }
    }
  }
  job->ps = ps;
  job->size = size;
  strncpy(job->rel, rel_buf, sizeof(job->rel) - 1);
  {
    const char *base = strrchr(path.c_str(), '/');
    base = (base != nullptr && base[1] != '\0') ? base + 1 : path.c_str();
    std::string safe_name;
    for (const char *p = base; *p != '\0'; p++) {
      if (*p != '"' && *p != '\\' && static_cast<uint8_t>(*p) >= 0x20)
        safe_name += *p;
    }
    snprintf(job->disposition, sizeof(job->disposition), "%s; filename=\"%s\"", want_inline ? "inline" : "attachment",
             safe_name.c_str());
  }

  httpd_req_t *req = *request;
  httpd_req_t *async_req = nullptr;
  if (this->dl_task_ == nullptr || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
    // No async slot (or no pipeline) -- degrade gracefully to the old synchronous pump.
    // The server blocks for this one transfer, but the download still succeeds.
    job->req = req;
    this->pump_download_(job);
    delete job;  // NOLINT(cppcoreguidelines-owning-memory)
    return;
  }
  job->req = async_req;
  if (xQueueSend(this->dl_queue_, &job, 0) != pdTRUE) {
    // Pipeline full: report on the async copy, then release socket and handle.
    httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer queue full");
    httpd_req_async_handler_complete(async_req);
    delete job;  // NOLINT(cppcoreguidelines-owning-memory)
    return;
  }
  // Handler returns immediately; the transfer task finishes the response and calls
  // httpd_req_async_handler_complete().
}

// Streams one download job to its request: headers, chunk loop (one main-loop hop per
// chunk), storage-handle close and the terminating zero chunk. Does NOT call
// httpd_req_async_handler_complete() -- only the task path owns an async copy.
storage::StorageError WebServerFileApi::worker_await_(
    std::function<storage::StorageError(storage::CompletionCallback)> call) {
  // The worker stream pool is main-loop-only and its completion callbacks fire on the main loop,
  // so defer the call there and block this transfer task on a private semaphore until the callback
  // (or an immediate rejection) releases it -- the same main-loop bridge run_on_loop_() uses, but
  // for the worker's async streams.
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr)
    return storage::StorageError::NOT_READY;
  storage::StorageError result = storage::StorageError::OK;
  this->defer([call = std::move(call), done, &result]() {
    storage::StorageError rej = call([done, &result](storage::StorageError e) {
      result = e;
      xSemaphoreGive(done);
    });
    if (rej != storage::StorageError::OK) {
      result = rej;  // rejected synchronously -- no callback will fire
      xSemaphoreGive(done);
    }
  });
  while (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
  }
  vSemaphoreDelete(done);
  return result;
}

void WebServerFileApi::pump_download_(DownloadJob *job) {
  httpd_req_t *req = job->req;
  httpd_resp_set_type(req, job->type);

  if (job->mem != nullptr) {
    // Asset already in PSRAM: no storage read and no disposition -- just stream the buffer in
    // chunks so a large raw file goes out without a single blocking send (which is what 500'd).
    if (job->gzip)
      httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    size_t off = 0;
    while (off < job->mem_len) {
      const size_t n = (job->mem_len - off) < FILE_API_CHUNK ? (job->mem_len - off) : FILE_API_CHUNK;
      if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(job->mem + off), n) != ESP_OK)
        break;
      off += n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }

  httpd_resp_set_hdr(req, "Content-Disposition", job->disposition);
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

  uint64_t remaining = job->size;
  const bool ranged = job->has_range;
  if (ranged) {
    httpd_resp_set_status(req, "206 Partial Content");
    httpd_resp_set_hdr(req, "Content-Range", job->content_range);
    remaining = job->range_end - job->range_start + 1;
  }

  // Open + stream the file through the worker's stream API (file -> HTTP download). Polymorphic:
  // the worker does the right thing for a filesystem or a network storage. No direct file access.
  storage::StreamHandle stream{};
  storage::StorageError err = this->worker_await_([job, &stream](storage::CompletionCallback cb) {
    return storage::global_storage_worker->begin_read(job->ps, job->rel, &stream, std::move(cb));
  });
  bool stream_open = err == storage::StorageError::OK;
  bool failed = !stream_open;

  if (stream_open) {
    // Range: seek to range_start through the worker (SET) -- a real seek, no read-and-discard.
    if (ranged && job->range_start > 0) {
      err = this->worker_await_([&stream, job](storage::CompletionCallback cb) {
        return storage::global_storage_worker->seek(stream, static_cast<int64_t>(job->range_start),
                                                    storage::SeekMode::SET, std::move(cb));
      });
      if (err != storage::StorageError::OK)
        failed = true;
    }
    auto *buf = new uint8_t[FILE_API_CHUNK];  // NOLINT(cppcoreguidelines-owning-memory)
    while (!failed) {
      size_t got = 0;
      err = this->worker_await_([&stream, buf, &got](storage::CompletionCallback cb) {
        return storage::global_storage_worker->read_chunk(stream, buf, FILE_API_CHUNK, &got, std::move(cb));
      });
      if (err != storage::StorageError::OK) {
        failed = true;
        break;
      }
      if (got == 0)
        break;  // EOF
      size_t send_n = got;
      if (ranged && send_n > remaining)
        send_n = static_cast<size_t>(remaining);
      if (send_n > 0 &&
          httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf), send_n) != ESP_OK) {
        failed = true;  // client went away
        break;
      }
      if (ranged) {
        remaining -= send_n;
        if (remaining == 0)
          break;
      }
    }
    delete[] buf;  // NOLINT(cppcoreguidelines-owning-memory)
    this->worker_await_([&stream](storage::CompletionCallback cb) {
      return storage::global_storage_worker->end_read(stream, std::move(cb));
    });
  }

  if (failed) {
    ESP_LOGW(TAG, "download '%s' aborted (%s)", job->rel, storage::error_to_string(err));
  }
  httpd_resp_send_chunk(req, nullptr, 0);  // finish chunked response (also on abort)
}

void WebServerFileApi::download_task_trampoline_(void *arg) { static_cast<WebServerFileApi *>(arg)->download_task_(); }

void WebServerFileApi::download_task_() {
  for (;;) {
    DownloadJob *job = nullptr;
    if (xQueueReceive(this->dl_queue_, &job, portMAX_DELAY) != pdTRUE || job == nullptr)
      continue;
    this->pump_download_(job);
    httpd_req_async_handler_complete(job->req);
    delete job;  // NOLINT(cppcoreguidelines-owning-memory)
  }
}

// ---------------------------------------------------------------------------
// Upload (multipart; one main-loop hop per received chunk)
// ---------------------------------------------------------------------------

storage::StorageError WebServerFileApi::publish_upload_(storage::PathStorage *ps, const char *temp,
                                                        const char *final_path, bool overwrite) {
  // Main-loop-only. rename() cannot replace, so an overwrite is remove-then-rename -- a tiny
  // window in which the old file is gone, but never a half-written one. A same-directory
  // rename is atomic: the final path flips from absent/old to complete in one step.
  if (overwrite) {
    storage::StorageError rerr = ps->remove(final_path);
    if (rerr != storage::StorageError::OK && rerr != storage::StorageError::NOT_FOUND)
      return rerr;
  }
  return ps->rename(temp, final_path);
}

void WebServerFileApi::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                                    uint8_t *data, size_t len, bool final) {
  // Start marker: index 0, no data -- and not final. An explicitly-created empty file
  // (?create=1) drives its End marker as (index 0, no data, final=true) as well, so without
  // the !final guard that End would be misread as a second Start, hit the one-upload-at-a-time
  // refusal below and return before the final block ever closes and publishes the .uploading
  // temp -- the file would be left as a stray .uploading and never appear.
  if (index == 0 && data == nullptr && !final) {
    ESP_LOGD(TAG, "upload start: '%s'", filename.c_str());
    // Start marker. Target path comes from the query (?path=/sdcard/dir/file.bin); the
    // multipart filename is only a fallback appended to ?dir=.
    if (this->upload_.active) {
      this->upload_.error = storage::StorageError::NOT_READY;  // one upload at a time
      return;
    }
    this->upload_ = UploadState{};
    this->upload_.active = true;
    this->upload_.staged_handoff = false;

    auto *param = request->getParam("path");
    std::string path;
    if (param != nullptr) {
      path = param->value();
    } else {
      auto *dir = request->getParam("dir");
      if (dir == nullptr) {
        this->upload_.error = storage::StorageError::INVALID_ARGS;
        return;
      }
      path = dir->value();
      if (path.empty() || path.back() != '/')
        path += '/';
      path += filename;
    }
    storage::StorageError err = storage::StorageError::OK;
    // Default-safe like copy/move: an existing destination is refused with ALREADY_EXISTS unless
    // ?overwrite=1 is set. (The write/append distinction is an action-level contract elsewhere;
    // an interactive upload has no such choice, so the modal offers an overwrite checkbox.)
    auto *ow = request->getParam("overwrite");
    const bool overwrite = ow != nullptr && ow->value() == "1";
    bool ok = this->run_on_loop_([this, &path, &err, overwrite]() {
      const char *rel = nullptr;
      storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
      if (ps == nullptr) {
        err = storage::StorageError::NOT_FOUND;
        return;
      }
      this->upload_.storage = ps;
      // Stream into a temp sibling and publish atomically at the end (see publish_upload_).
      strncpy(this->upload_.final_path, rel, sizeof(this->upload_.final_path) - 1);
      int tn = snprintf(this->upload_.rel_path, sizeof(this->upload_.rel_path), "%s.uploading", rel);
      if (tn < 0 || (size_t) tn >= sizeof(this->upload_.rel_path)) {
        err = storage::StorageError::INVALID_ARGS;  // path + suffix would not fit
        return;
      }
      this->upload_.overwrite = overwrite;
      if (!overwrite) {
        // Refuse a silent overwrite: if the final destination already exists, answer
        // ALREADY_EXISTS. stat() OK means it exists; NOT_FOUND is the wanted case.
        storage::FileStat st{};
        storage::StorageError serr = ps->stat(this->upload_.final_path, &st);
        if (serr == storage::StorageError::OK) {
          err = storage::StorageError::ALREADY_EXISTS;
          return;
        }
        if (serr != storage::StorageError::NOT_FOUND) {
          err = serr;
          return;
        }
      }
      // Drop any leftover temp from an earlier aborted upload before opening.
      ps->remove(this->upload_.rel_path);
    });
    if (!ok)
      err = storage::StorageError::NOT_READY;
    if (err == storage::StorageError::OK) {
      // Open the temp for writing through the worker's stream API (polymorphic: it opens/truncates
      // for filesystem and network storages alike). On the httpd task via the worker bridge -- not
      // nested in run_on_loop_, which would deadlock the main loop against its own callback.
      err = this->worker_await_([this](storage::CompletionCallback cb) {
        return storage::global_storage_worker->begin_write(this->upload_.storage, this->upload_.rel_path,
                                                           &this->upload_.stream, std::move(cb));
      });
      this->upload_.stream_open = err == storage::StorageError::OK;
    }
    this->upload_.error = err;
#ifdef USE_STORAGE_TRANSFER_BUFFER
    // Stage into the PSRAM arena when the whole request body fits (content_len is a safe
    // upper bound for the file part). Strictly additive: nullptr keeps streaming.
    if (this->upload_.error == storage::StorageError::OK && storage::global_transfer_buffer != nullptr &&
        (!this->flush_.active || this->flush_.finished)) {
      httpd_req_t *raw = *request;
      this->upload_.staged = storage::global_transfer_buffer->try_acquire(raw->content_len);
      this->upload_.staged_cap = this->upload_.staged != nullptr ? raw->content_len : 0;
    }
#endif
    return;
  }

  if (this->upload_.error != storage::StorageError::OK) {
    return;  // sink remaining data after a failure; response is sent in handleRequest
  }

  if (data != nullptr && len > 0) {
    storage::StorageError err = storage::StorageError::OK;
#ifdef USE_STORAGE_TRANSFER_BUFFER
    if (this->upload_.staged != nullptr) {
      // Staged: memcpy into the PSRAM arena now; the worker write happens in the flush drain.
      if (this->upload_.offset + len > this->upload_.staged_cap) {
        err = storage::StorageError::WRITE_ERROR;  // cannot happen: cap = content_len
      } else {
        memcpy(this->upload_.staged + this->upload_.offset, data, len);
        this->upload_.offset += len;
      }
    } else
#endif
    {
      // Straight to the file through the worker's write stream (polymorphic; no direct write()).
      // The worker writes the whole chunk or reports an error.
      err = this->worker_await_([this, data, len](storage::CompletionCallback cb) {
        return storage::global_storage_worker->write_chunk(this->upload_.stream, data, len, std::move(cb));
      });
      if (err == storage::StorageError::OK)
        this->upload_.offset += len;
    }
    this->upload_.error = err;
  }

  if (final) {
#ifdef USE_STORAGE_TRANSFER_BUFFER
    if (this->upload_.staged != nullptr) {
      if (this->upload_.error == storage::StorageError::OK) {
        this->flush_ = StagedFlush{};
        this->flush_.active = true;
        // Drained in loop(), so the same applies here as to the walker: ask for the phase.
        this->loop_requester_.start();
        this->flush_.job = FLUSH_JOB_FLAG | (++this->flush_job_counter_ & JOB_COUNTER_MASK);
        this->flush_.storage = this->upload_.storage;
        this->flush_.stream = this->upload_.stream;
        this->flush_.stream_open = this->upload_.stream_open;
        memcpy(this->flush_.rel_path, this->upload_.rel_path, sizeof(this->flush_.rel_path));
        memcpy(this->flush_.final_path, this->upload_.final_path, sizeof(this->flush_.final_path));
        this->flush_.overwrite = this->upload_.overwrite;
        this->flush_.data = this->upload_.staged;
        this->flush_.total = this->upload_.offset;
        this->upload_.stream_open = false;    // the flush owns and closes the stream now
        this->upload_.staged_handoff = true;  // the change note fires at flush completion
      } else {
        storage::global_transfer_buffer->release();  // receive failed -- nothing to flush
      }
      this->upload_.staged = nullptr;
    }
#endif
    ESP_LOGD(TAG, "upload end: %" PRIu64 " bytes (%s)", this->upload_.offset,
             storage::error_to_string(this->upload_.error));
    storage::StorageError close_err = storage::StorageError::OK;
    if (this->upload_.stream_open) {
      // Close through the worker (end_write); close errors must surface -- backends flush on close.
      close_err = this->worker_await_([this](storage::CompletionCallback cb) {
        return storage::global_storage_worker->end_write(this->upload_.stream, std::move(cb));
      });
      this->upload_.stream_open = false;
    }
    if (this->upload_.error == storage::StorageError::OK)
      this->upload_.error = close_err;
    if (this->upload_.error == storage::StorageError::OK && this->upload_.storage != nullptr &&
        !this->upload_.staged_handoff) {
      // Publish atomically (rename temp -> final), then note the final path's directory.
      // Both the rename and the note are main-loop-only.
      std::string abs = std::string(this->upload_.storage->get_mount_path()) + "/" + this->upload_.final_path;
      this->run_on_loop_([this, &abs]() {
        this->upload_.error = this->publish_upload_(this->upload_.storage, this->upload_.rel_path,
                                                    this->upload_.final_path, this->upload_.overwrite);
        if (this->upload_.error == storage::StorageError::OK)
          storage::global_storage_registry->note_parent_changed(abs);
      });
    }
    if (this->upload_.error != storage::StorageError::OK && this->upload_.storage != nullptr &&
        !this->upload_.staged_handoff && this->upload_.rel_path[0] != '\0') {
      // Failed before (or during) publish -- drop the partial temp so it does not linger.
      this->run_on_loop_([this]() { this->upload_.storage->remove(this->upload_.rel_path); });
    }
  }
}

void WebServerFileApi::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                  size_t total) {
  // Diagnostic: if this fires for /files/upload, the POST went down the RAW body path --
  // i.e. the backend's content-type check never classified the request as multipart.
  if (index == 0) {
    ESP_LOGW(TAG, "upload: request took the raw-body path (not classified as multipart)");
  }
}

void WebServerFileApi::handle_upload_response_(AsyncWebServerRequest *request) {
  if (!this->upload_.active) {
    // handleRequest fired for POST /files/upload but handleUpload never received a start
    // marker -- the multipart body produced no file part for this handler.
    ESP_LOGW(TAG, "upload: no multipart file part reached the handler");
  }
  storage::StorageError err = this->upload_.error;
  this->upload_.active = false;
  if (err != storage::StorageError::OK) {
    ESP_LOGW(TAG, "upload failed (%s)", storage::error_to_string(err));
    send_error_(request, err);
    return;
  }
#ifdef USE_STORAGE_TRANSFER_BUFFER
  if (this->flush_.active && !this->flush_.finished) {
    char jbuf[96];
    snprintf(jbuf, sizeof(jbuf), "{\"success\":true,\"bytes\":%" PRIu64 ",\"job\":%" PRIu32 "}", this->upload_.offset,
             this->flush_.job);
    request->send(200, "application/json", jbuf);
    return;
  }
#endif
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"success\":true,\"bytes\":%" PRIu64 "}", this->upload_.offset);
  request->send(200, "application/json", buf);
}

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_FILE_API && USE_ESP_IDF
