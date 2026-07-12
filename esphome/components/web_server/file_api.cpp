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

  if (is_get) {
    if (url == "/files/storages")
      return this->handle_storages_(request);
    if (url == "/files/list")
      return this->handle_list_(request);
    if (url == "/files/stat")
      return this->handle_stat_(request);
    if (url == "/files/download")
      return this->handle_download_(request);
    if (url == "/files/job")
      return this->handle_job_(request);
  } else {
    if (url == "/files/mkdir")
      return this->handle_mkdir_(request);
    if (url == "/files/delete")
      return this->handle_delete_(request);
    if (url == "/files/copy")
      return this->handle_copy_move_(request, false);
    if (url == "/files/move")
      return this->handle_copy_move_(request, true);
    if (url == "/files/mount")
      return this->handle_mount_(request, true);
    if (url == "/files/unmount")
      return this->handle_mount_(request, false);
    if (url == "/files/upload")
      return this->handle_upload_response_(request);
  }
  request->send(404);
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
  return xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// ---------------------------------------------------------------------------
// Path handling / error mapping
// ---------------------------------------------------------------------------

bool WebServerFileApi::path_is_safe_(const char *path) {
  if (path == nullptr || path[0] != '/')
    return false;
  // Reject any '..' segment — resolve_path() does longest-prefix matching on the string, so
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
  // reported as absence — NOT_READY is 503, only NOT_FOUND is 404.
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
    json = "[";
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
          *ctx->out += "\",\"mountable\":";
          *ctx->out += s->as_mountable() != nullptr ? "true" : "false";
          storage::StorageInfo info{};
          if (s->get_info(&info) == storage::StorageError::OK) {
            *ctx->out += ",\"name\":\"";
            append_json_escaped(*ctx->out, info.name != nullptr ? info.name : "");
            *ctx->out += "\"";
          }
          *ctx->out += '}';
        },
        &ctx);
    json += ']';
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
            return false;  // stop enumeration — not an error per the list_dir contract
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

void WebServerFileApi::handle_mkdir_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();
  storage::StorageError err = storage::StorageError::OK;
  bool ok = this->run_on_loop_([this, &path, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
    if (ps == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    err = ps->mkdir(rel);
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
    if (m == nullptr) {
      found_not_mountable = true;
      return;
    }
    err = mount ? m->mount() : m->unmount();
  });
  if (!ok) {
    request->send(504);
    return;
  }
  if (found_not_mountable) {
    request->send(400, "application/json", "{\"error\":\"not a mountable storage\"}");
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
  // Copying/moving a directory into itself would recurse forever — reject early on the
  // full VFS strings ('/a' -> '/a/b' style; exact-prefix with a path boundary).
  if (to_s.size() > from_s.size() && to_s.compare(0, from_s.size(), from_s) == 0 && to_s[from_s.size()] == '/') {
    request->send(400, "application/json", "{\"error\":\"destination is inside the source\"}");
    return;
  }
  storage::StorageError err = storage::StorageError::OK;
  storage::TransferJob job = storage::INVALID_TRANSFER_JOB;
  bool ok = this->run_on_loop_([this, &from_s, &to_s, is_move, &err, &job]() {
    const char *src_rel = nullptr;
    const char *dst_rel = nullptr;
    storage::PathStorage *src = this->resolve_(from_s.c_str(), &src_rel);
    storage::PathStorage *dst = this->resolve_(to_s.c_str(), &dst_rel);
    if (src == nullptr || dst == nullptr) {
      err = storage::StorageError::NOT_FOUND;
      return;
    }
    // Directories: a same-storage MOVE is a pure rename (the worker takes that fast path
    // before opening any handles). Everything else — directory COPY and cross-storage
    // directory moves — goes through the per-file orchestrator.
    storage::FileStat src_stat{};
    if (src->stat(src_rel, &src_stat) == storage::StorageError::OK && src_stat.is_dir) {
      if (!is_move || src != dst) {
        if (!this->start_dir_transfer_(src, src_rel, dst, dst_rel, is_move)) {
          err = this->dir_.active ? storage::StorageError::NOT_READY : storage::StorageError::INVALID_ARGS;
          return;
        }
        job = this->dir_.id;
        return;
      }
    }
    if (storage::global_storage_worker == nullptr) {
      err = storage::StorageError::NOT_SUPPORTED;
      return;
    }
    // Completion parks the final status in the job cache (this callback runs on the main
    // loop) — the worker recycles its slot right after, so polling alone would miss DONE.
    // The job id only exists after submission, so the callback reads it through a small
    // heap slot filled right below; safe because both submission and completion run on the
    // main loop, strictly in that order. Freed by the callback (fires exactly once).
    auto *job_slot = new storage::TransferJob(storage::INVALID_TRANSFER_JOB);  // NOLINT
    auto on_done = [this, job_slot](storage::StorageError result) {
      this->cache_job_result_(*job_slot, result);
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory)
    };
    storage::StorageWorker *w = storage::global_storage_worker;
    err = is_move ? w->async_move(src, src_rel, dst, dst_rel, on_done, &job)
                  : w->async_copy(src, src_rel, dst, dst_rel, on_done, &job);
    if (err != storage::StorageError::OK) {
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory) — callback will not fire
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
  // done == total == 0 (unknown) — the UI switches to "finished" on state DONE anyway.
  e.status.bytes_done = 0;
  e.status.bytes_total = 0;
}
#endif

#ifdef USE_STORAGE_WORKER
// ---------------------------------------------------------------------------
// Recursive directory transfer orchestrator (see the header for the design notes)
// ---------------------------------------------------------------------------

namespace {
// Finds entry #target in a directory by re-enumeration (RAM-free, remove_recursive's trick).
struct FindEntryCtx {
  uint16_t target;
  uint16_t seen{0};
  bool found{false};
  storage::FileStat entry{};
};
bool find_entry_cb(const storage::FileStat *entry, void *ctx_raw) {
  auto *ctx = static_cast<FindEntryCtx *>(ctx_raw);
  if (ctx->seen == ctx->target) {
    ctx->entry = *entry;
    ctx->found = true;
    return false;  // stop enumeration — not an error per the list_dir contract
  }
  ctx->seen++;
  return true;
}
// Bounded "<root>[/<sub>][/<name>]" join; false on truncation.
bool join_path(char *out, size_t out_size, const char *root, const char *sub, const char *name) {
  int n;
  if (name != nullptr) {
    n = (sub[0] != '\0') ? snprintf(out, out_size, "%s/%s/%s", root, sub, name)
                         : snprintf(out, out_size, "%s/%s", root, name);
  } else {
    n = (sub[0] != '\0') ? snprintf(out, out_size, "%s/%s", root, sub) : snprintf(out, out_size, "%s", root);
  }
  return n > 0 && static_cast<size_t>(n) < out_size;
}
}  // namespace

bool WebServerFileApi::start_dir_transfer_(storage::PathStorage *src, const char *src_rel, storage::PathStorage *dst,
                                           const char *dst_rel, bool is_move) {
  if (this->dir_.active)
    return false;
  if (strlen(src_rel) >= sizeof(this->dir_.src_root) || strlen(dst_rel) >= sizeof(this->dir_.dst_root))
    return false;
  this->dir_ = DirTransfer{};
  this->dir_.active = true;
  this->dir_.is_move = is_move;
  this->dir_.src = src;
  this->dir_.dst = dst;
  strncpy(this->dir_.src_root, src_rel, sizeof(this->dir_.src_root) - 1);
  strncpy(this->dir_.dst_root, dst_rel, sizeof(this->dir_.dst_root) - 1);
  this->dir_.id = DIR_JOB_FLAG | (++this->dir_job_counter_ & 0x7FFFFFFFu);

  storage::StorageError err = this->dir_.dst->mkdir(this->dir_.dst_root);
  if (err != storage::StorageError::OK && err != storage::StorageError::ALREADY_EXISTS) {
    this->finish_dir_transfer_(err);
    return true;  // started (and already finished with an error) — id is queryable
  }
  ESP_LOGD(TAG, "dir %s started: '%s' -> '%s' (job %" PRIu32 ")", is_move ? "move" : "copy", src_rel, dst_rel,
           this->dir_.id);
  // Kick the walker from a fresh main-loop slice.
  this->defer([this]() { this->advance_dir_transfer_(); });
  return true;
}

void WebServerFileApi::finish_dir_transfer_(storage::StorageError result) {
  ESP_LOGD(TAG, "dir %s finished: %s (%" PRIu32 " files, %" PRIu64 " bytes)", this->dir_.is_move ? "move" : "copy",
           storage::error_to_string(result), this->dir_.files_done, this->dir_.bytes_done);
  this->dir_.result = result;
  this->dir_.done = true;
  this->dir_.active = false;
  if (result != storage::StorageError::OK) {
    ESP_LOGW(TAG, "directory %s failed (%s)", this->dir_.is_move ? "move" : "copy", storage::error_to_string(result));
  }
}

void WebServerFileApi::advance_dir_transfer_() {
  if (!this->dir_.active)
    return;
  DirTransfer &d = this->dir_;
  // Bound the inline control steps per main-loop slice (empty-directory chains would
  // otherwise walk arbitrarily long in one go); continue via defer.
  for (int steps = 0; steps < 8; steps++) {
    char src_dir[300];
    if (!join_path(src_dir, sizeof(src_dir), d.src_root, d.sub, nullptr)) {
      this->finish_dir_transfer_(storage::StorageError::INVALID_ARGS);
      return;
    }
    ESP_LOGD(TAG, "dir walk: depth=%u idx=%u in '%s'", d.depth, d.index_stack[d.depth], src_dir);
    FindEntryCtx ctx{d.index_stack[d.depth]};
    storage::StorageError err = d.src->list_dir(src_dir, find_entry_cb, &ctx);
    if (err != storage::StorageError::OK) {
      this->finish_dir_transfer_(err);
      return;
    }
    if (!ctx.found) {
      ESP_LOGD(TAG, "dir walk: '%s' drained", src_dir);
      // Directory drained. For moves the emptied source directory goes away now.
      if (d.is_move) {
        err = d.src->rmdir(src_dir);
        if (err != storage::StorageError::OK) {
          this->finish_dir_transfer_(err);
          return;
        }
      }
      if (d.depth == 0) {
        this->finish_dir_transfer_(storage::StorageError::OK);
        return;
      }
      // Ascend: strip the last component; the parent's index was already advanced on descend.
      char *slash = strrchr(d.sub, '/');
      if (slash != nullptr) {
        *slash = '\0';
      } else {
        d.sub[0] = '\0';
      }
      d.depth--;
      continue;
    }
    // Entry-position bookkeeping differs by mode: a COPY leaves the source untouched, so the
    // walker counts upward through stable positions. A MOVE removes every processed entry
    // from the source (files right after their copy, directories via rmdir once drained), so
    // remaining entries slide down and the next unprocessed entry is always #0 — advancing
    // the index here would skip entries and derail the counting re-enumeration.
    if (!d.is_move)
      d.index_stack[d.depth]++;
    if (ctx.entry.is_dir) {
      if (d.depth + 1 >= DirTransfer::MAX_DEPTH) {
        this->finish_dir_transfer_(storage::StorageError::NOT_SUPPORTED);
        return;
      }
      char dst_dir[300];
      if (!join_path(dst_dir, sizeof(dst_dir), d.dst_root, d.sub, ctx.entry.name)) {
        this->finish_dir_transfer_(storage::StorageError::INVALID_ARGS);
        return;
      }
      ESP_LOGD(TAG, "dir walk: descend into '%s'", ctx.entry.name);
      err = d.dst->mkdir(dst_dir);
      if (err != storage::StorageError::OK && err != storage::StorageError::ALREADY_EXISTS) {
        this->finish_dir_transfer_(err);
        return;
      }
      // Descend
      size_t sub_len = strlen(d.sub);
      size_t name_len = strlen(ctx.entry.name);
      if (sub_len + name_len + 2 >= sizeof(d.sub)) {
        this->finish_dir_transfer_(storage::StorageError::INVALID_ARGS);
        return;
      }
      if (sub_len != 0)
        d.sub[sub_len++] = '/';
      memcpy(d.sub + sub_len, ctx.entry.name, name_len + 1);
      d.depth++;
      d.index_stack[d.depth] = 0;
      continue;
    }
    // Regular file: hand it to the async worker and wait for its completion callback.
    if (!join_path(d.cur_src, sizeof(d.cur_src), d.src_root, d.sub, ctx.entry.name) ||
        !join_path(d.cur_dst, sizeof(d.cur_dst), d.dst_root, d.sub, ctx.entry.name)) {
      this->finish_dir_transfer_(storage::StorageError::INVALID_ARGS);
      return;
    }
    d.cur_size = ctx.entry.size;
    auto on_done = [this](storage::StorageError result) {
      DirTransfer &dt = this->dir_;
      ESP_LOGD(TAG, "dir walk: file done (%s), active=%d", storage::error_to_string(result), dt.active);
      if (!dt.active)
        return;  // finished/aborted meanwhile
      if (result != storage::StorageError::OK) {
        this->finish_dir_transfer_(result);
        return;
      }
      dt.bytes_done += dt.cur_size;
      dt.files_done++;
      if (dt.is_move) {
        storage::StorageError del_err = dt.src->remove(dt.cur_src);
        if (del_err != storage::StorageError::OK) {
          this->finish_dir_transfer_(del_err);
          return;
        }
      }
      dt.cur_job = storage::INVALID_TRANSFER_JOB;
      // Fresh main-loop slice for the next walker step (avoids callback recursion).
      this->defer([this]() { this->advance_dir_transfer_(); });
    };
    err = storage::global_storage_worker->async_copy(d.src, d.cur_src, d.dst, d.cur_dst, on_done, &d.cur_job);
    ESP_LOGD(TAG, "dir walk: file '%s' -> '%s' submitted (%s)", d.cur_src, d.cur_dst, storage::error_to_string(err));
    if (err != storage::StorageError::OK) {
      this->finish_dir_transfer_(err);
    }
    return;  // wait for the completion callback
  }
  this->defer([this]() { this->advance_dir_transfer_(); });
}
#endif  // USE_STORAGE_WORKER

void WebServerFileApi::handle_job_(AsyncWebServerRequest *request) {
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
    if ((job & DIR_JOB_FLAG) != 0) {
      // Directory transfer: single slot, final state queryable until the next one starts.
      DirTransfer &d = this->dir_;
      if (job != d.id || (!d.active && !d.done))
        return;
      found = true;
      st.state = d.done ? storage::RequestState::DONE : storage::RequestState::RUNNING;
      st.result = d.result;
      st.bytes_total = 0;  // unknown without a pre-scan — UI treats it as indeterminate
      st.bytes_done = d.bytes_done;
      // add live progress of the file currently in flight
      storage::TransferStatus cur{};
      if (d.cur_job != storage::INVALID_TRANSFER_JOB && storage::global_storage_worker != nullptr &&
          storage::global_storage_worker->get_transfer_status(d.cur_job, &cur)) {
        st.bytes_done += cur.bytes_done;
      }
      return;
    }
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
  snprintf(buf, sizeof(buf),
           "{\"state\":\"%s\",\"result\":\"%s\",\"bytes_done\":%" PRIu64 ",\"bytes_total\":%" PRIu64 "}", state,
           storage::error_to_string(st.result), st.bytes_done, st.bytes_total);
  request->send(200, "application/json", buf);
#else
  request->send(501);
#endif
}

// ---------------------------------------------------------------------------
// Download (chunked, one main-loop hop per chunk)
// ---------------------------------------------------------------------------

void WebServerFileApi::handle_download_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("path");
  if (param == nullptr) {
    request->send(400);
    return;
  }
  std::string path = param->value();

  storage::PathStorage *ps = nullptr;
  storage::FileHandle *handle = nullptr;
  bool is_fs = false;
  uint64_t size = 0;
  char rel_buf[256]{};
  storage::StorageError err = storage::StorageError::OK;

  bool ok = this->run_on_loop_([this, &path, &ps, &handle, &is_fs, &size, &rel_buf, &err]() {
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
    is_fs = ps->get_storage_type() == storage::StorageType::FILESYSTEM;
    if (is_fs) {
      err = static_cast<storage::FilesystemStorage *>(ps)->open(rel, handle, storage::OpenMode::READ);
    }
  });
  if (!ok || err != storage::StorageError::OK) {
    if (!ok) {
      request->send(504);
    } else {
      send_error_(request, err);
    }
    return;
  }

  // Raw chunked httpd response: this handler is IDF-only by construction, so using the native
  // chunk API directly is fine and avoids buffering the file.
  httpd_req_t *req = *request;
  httpd_resp_set_type(req, "application/octet-stream");
  // Pass the real file name to the browser's save dialog. httpd_resp_set_hdr() stores the
  // pointer (no copy), so the buffer must outlive every send below — function scope does.
  char disposition[300];
  {
    const char *base = strrchr(path.c_str(), '/');
    base = (base != nullptr && base[1] != '\0') ? base + 1 : path.c_str();
    std::string safe_name;
    for (const char *p = base; *p != '\0'; p++) {
      if (*p != '"' && *p != '\\' && static_cast<uint8_t>(*p) >= 0x20)
        safe_name += *p;
    }
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", safe_name.c_str());
  }
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);

  auto *buf = new uint8_t[FILE_API_CHUNK];  // NOLINT(cppcoreguidelines-owning-memory)
  uint64_t offset = 0;
  bool failed = false;
  while (true) {
    size_t got = 0;
    bool loop_ok = this->run_on_loop_([this, ps, handle, is_fs, &rel_buf, &offset, buf, &got, &err]() {
      if (is_fs) {
        err = static_cast<storage::FilesystemStorage *>(ps)->read(handle, buf, FILE_API_CHUNK, &got);
      } else {
        err = static_cast<storage::NetworkStorage *>(ps)->read_chunk(rel_buf, buf, offset, FILE_API_CHUNK, &got);
      }
    });
    if (!loop_ok || err != storage::StorageError::OK) {
      failed = true;
      break;
    }
    if (got == 0)
      break;  // EOF
    offset += got;
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf), got) != ESP_OK) {
      failed = true;  // client went away — still close the handle below
      break;
    }
    if (offset >= size && size != 0)
      break;
  }
  delete[] buf;  // NOLINT(cppcoreguidelines-owning-memory)

  if (is_fs && handle != nullptr) {
    this->run_on_loop_([ps, handle]() { static_cast<storage::FilesystemStorage *>(ps)->close(handle); });
  }
  if (failed) {
    ESP_LOGW(TAG, "download '%s' aborted (%s)", path.c_str(), storage::error_to_string(err));
    httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }
  httpd_resp_send_chunk(req, nullptr, 0);  // finish chunked response
}

// ---------------------------------------------------------------------------
// Upload (multipart; one main-loop hop per received chunk)
// ---------------------------------------------------------------------------

void WebServerFileApi::handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,
                                    uint8_t *data, size_t len, bool final) {
  if (index == 0 && data == nullptr) {
    ESP_LOGD(TAG, "upload start: '%s'", filename.c_str());
    // Start marker. Target path comes from the query (?path=/sdcard/dir/file.bin); the
    // multipart filename is only a fallback appended to ?dir=.
    if (this->upload_.active) {
      this->upload_.error = storage::StorageError::NOT_READY;  // one upload at a time
      return;
    }
    this->upload_ = UploadState{};
    this->upload_.active = true;

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
    bool ok = this->run_on_loop_([this, &path, &err]() {
      const char *rel = nullptr;
      storage::PathStorage *ps = this->resolve_(path.c_str(), &rel);
      if (ps == nullptr) {
        err = storage::StorageError::NOT_FOUND;
        return;
      }
      this->upload_.storage = ps;
      strncpy(this->upload_.rel_path, rel, sizeof(this->upload_.rel_path) - 1);
      this->upload_.dst_is_fs = ps->get_storage_type() == storage::StorageType::FILESYSTEM;
      if (this->upload_.dst_is_fs) {
        err = static_cast<storage::FilesystemStorage *>(ps)->open(this->upload_.rel_path, this->upload_.handle,
                                                                  storage::OpenMode::WRITE);
        this->upload_.handle_open = err == storage::StorageError::OK;
      }
    });
    if (!ok)
      err = storage::StorageError::NOT_READY;
    this->upload_.error = err;
    return;
  }

  if (this->upload_.error != storage::StorageError::OK) {
    return;  // sink remaining data after a failure; response is sent in handleRequest
  }

  if (data != nullptr && len > 0) {
    storage::StorageError err = storage::StorageError::OK;
    bool ok = this->run_on_loop_([this, data, len, &err]() {
      size_t written = 0;
      if (this->upload_.dst_is_fs) {
        err = static_cast<storage::FilesystemStorage *>(this->upload_.storage)
                  ->write(this->upload_.handle, data, len, &written);
      } else {
        err = static_cast<storage::NetworkStorage *>(this->upload_.storage)
                  ->write_chunk(this->upload_.rel_path, data, this->upload_.offset, len, &written);
      }
      if (err == storage::StorageError::OK && written != len)
        err = storage::StorageError::WRITE_ERROR;
      this->upload_.offset += written;
    });
    if (!ok)
      err = storage::StorageError::NOT_READY;
    this->upload_.error = err;
  }

  if (final) {
    ESP_LOGD(TAG, "upload end: %" PRIu64 " bytes (%s)", this->upload_.offset,
             storage::error_to_string(this->upload_.error));
    storage::StorageError close_err = storage::StorageError::OK;
    if (this->upload_.handle_open) {
      this->run_on_loop_([this, &close_err]() {
        // Close errors must surface — FATFS-backed drivers flush on close.
        close_err = static_cast<storage::FilesystemStorage *>(this->upload_.storage)->close(this->upload_.handle);
        this->upload_.handle_open = false;
      });
    }
    if (this->upload_.error == storage::StorageError::OK)
      this->upload_.error = close_err;
  }
}

void WebServerFileApi::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                  size_t total) {
  // Diagnostic: if this fires for /files/upload, the POST went down the RAW body path —
  // i.e. the backend's content-type check never classified the request as multipart.
  if (index == 0) {
    ESP_LOGW(TAG, "upload: request took the raw-body path (not classified as multipart)");
  }
}

void WebServerFileApi::handle_upload_response_(AsyncWebServerRequest *request) {
  if (!this->upload_.active) {
    // handleRequest fired for POST /files/upload but handleUpload never received a start
    // marker — the multipart body produced no file part for this handler.
    ESP_LOGW(TAG, "upload: no multipart file part reached the handler");
  }
  storage::StorageError err = this->upload_.error;
  this->upload_.active = false;
  if (err != storage::StorageError::OK) {
    ESP_LOGW(TAG, "upload failed (%s)", storage::error_to_string(err));
    send_error_(request, err);
    return;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "{\"bytes\":%" PRIu64 "}", this->upload_.offset);
  request->send(200, "application/json", buf);
}

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_FILE_API && USE_ESP_IDF
