#include "raw_api.h"

#if defined(USE_WEBSERVER_RAW_API) && defined(USE_ESP_IDF)

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::web_server {

static const char *const TAG = "web_server.raw_api";

// Chunk size for streaming reads: small enough to keep each main-loop hop short, large enough
// that the per-hop overhead does not dominate.
static constexpr size_t RAW_API_CHUNK = 1024;

void WebServerRawApi::setup() {
  this->op_done_ = xSemaphoreCreateBinary();
  this->base_->add_handler(this);
}

void WebServerRawApi::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Raw API:\n"
                "  Write enabled: %s",
                YESNO(this->enable_write_));
}

bool WebServerRawApi::run_on_loop_(std::function<void()> &&op, uint32_t timeout_ms) {
  SemaphoreHandle_t done = this->op_done_;
  this->defer([op = std::move(op), done]() {
    op();
    xSemaphoreGive(done);
  });
  // Wait WITHOUT a deadline — same defect and same fix as the file API's marshaller: every
  // op captures references into this handler's stack frame, so a timed-out wait returned
  // into a frame the still-queued op would later write through. Sliced re-take only so a
  // long op (a whole-chip erase) is a visible wait, never an abandonment.
  (void) timeout_ms;
  while (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
  }
  return true;
}

static void append_json_escaped(std::string &out, const char *s) {
  for (const char *p = s; *p != '\0'; p++) {
    if (*p == '"' || *p == '\\') {
      out += '\\';
      out += *p;
    } else if (static_cast<uint8_t>(*p) >= 0x20) {
      out += *p;
    }
  }
}

bool WebServerRawApi::canHandle(AsyncWebServerRequest *request) const {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  if (strncmp(url.c_str(), "/raw/", 5) != 0)
    return false;
  const auto method = request->method();
  return method == HTTP_GET || method == HTTP_POST;
}

storage::RawStorage *WebServerRawApi::find_device_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("device");
  if (param == nullptr)
    return nullptr;
  const std::string wanted = param->value();

  struct Ctx {
    const std::string *wanted;
    storage::RawStorage *scope;
    storage::RawStorage *found;
  } ctx{&wanted, this->scoped_device_, nullptr};

  this->run_on_loop_([&ctx]() {
    if (storage::global_storage_registry == nullptr)
      return;
    storage::global_storage_registry->for_each_raw(
        [](storage::RawStorage *s, void *c) {
          auto *ctx = static_cast<Ctx *>(c);
          if (ctx->scope != nullptr && s != ctx->scope)
            return;
          storage::StorageInfo info{};
          if (s->get_info(&info) != storage::StorageError::OK || info.id == nullptr)
            return;
          if (*ctx->wanted == info.id)
            ctx->found = s;
        },
        &ctx);
  });
  return ctx.found;
}

bool WebServerRawApi::parse_range_(AsyncWebServerRequest *request, storage::RawStorage *device, uint64_t *address,
                                   uint64_t *size) {
  storage::RawGeometry geo;
  this->run_on_loop_([device, &geo]() { device->get_raw_geometry(&geo); });

  auto *all = request->getParam("all");
  if (all != nullptr && all->value() == "1") {
    *address = 0;
    *size = geo.capacity;
    return true;
  }

  auto *addr_param = request->getParam("address");
  auto *size_param = request->getParam("size");
  if (size_param == nullptr)
    return false;
  *address = addr_param != nullptr ? strtoull(addr_param->value().c_str(), nullptr, 0) : 0;
  *size = strtoull(size_param->value().c_str(), nullptr, 0);
  // The device is the authority on its own bounds; reject rather than clamp, so a client never
  // silently gets a different range than it asked for.
  return *size > 0 && *address < geo.capacity && *size <= geo.capacity - *address;
}

void WebServerRawApi::handle_devices_(AsyncWebServerRequest *request) {
  std::string json;
  bool ok = this->run_on_loop_([this, &json]() {
    json = "[";
    struct Ctx {
      std::string *out;
      storage::RawStorage *scope;
      bool enable_write;
      bool enable_erase;
      bool first{true};
    } ctx{&json, this->scoped_device_, this->enable_write_, this->enable_write_};
    if (storage::global_storage_registry == nullptr)
      return;
    storage::global_storage_registry->for_each_raw(
        [](storage::RawStorage *s, void *c) {
          auto *ctx = static_cast<Ctx *>(c);
          if (ctx->scope != nullptr && s != ctx->scope)
            return;
          storage::StorageInfo info{};
          if (s->get_info(&info) != storage::StorageError::OK)
            return;
          storage::RawGeometry geo;
          s->get_raw_geometry(&geo);
          if (!ctx->first)
            *ctx->out += ',';
          ctx->first = false;
          *ctx->out += "{\"id\":\"";
          append_json_escaped(*ctx->out, info.id != nullptr ? info.id : "");
#ifdef USE_STORAGE_DEVICE_NODES
          // The browser labels the node with this; it addresses the device by "id" above.
          *ctx->out += "\",\"node_name\":\"";
          append_json_escaped(*ctx->out, s->get_device_node_name() != nullptr ? s->get_device_node_name() : "");
#endif
          *ctx->out += "\",\"name\":\"";
          append_json_escaped(*ctx->out, info.name != nullptr ? info.name : "");
          *ctx->out += "\",\"kind\":\"";
          append_json_escaped(*ctx->out, info.kind != nullptr ? info.kind : "");
          // Sized generously above the worst case (template + a 20-digit capacity, three
          // 10-digit geometry fields and seven "false") — snprintf would silently truncate a
          // shorter buffer and hand the browser JSON it cannot parse.
          char buf[320];
          // The geometry is what a client needs to offer only what this medium can do — the
          // capability bits, not a guess from the kind string.
          snprintf(
              buf, sizeof(buf),
              "\",\"capacity\":%" PRIu64 ",\"write_page\":%" PRIu32 ",\"erase_sector\":%" PRIu32
              ",\"erase_block\":%" PRIu32 ",\"write_needs_erase\":%s,\"can_erase_sector\":%s,\"can_erase_block\":%s"
              ",\"can_erase_chip\":%s,\"writable\":%s,\"erasable\":%s,\"pseudo_erase\":%s,\"node\":%s}",
              geo.capacity, geo.write_page, geo.erase_sector, geo.erase_block,
              (geo.caps & storage::RAW_WRITE_NEEDS_ERASE) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_SECTOR) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_BLOCK) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_CHIP) != 0 ? "true" : "false",
              // What this build allows, so the browser never offers a button that can only 403.
              ctx->enable_write ? "true" : "false",
              // Erasable when allowed here — media with a real erase use the driver's erase(),
              // overwrite-in-place media (no RAW_ERASE_* caps: EEPROM, FRAM) get the worker's
              // pseudo erase, a chunked 0xFF fill via write(). Either way the endpoint works.
              ctx->enable_erase ? "true" : "false",
              // Tells the browser WHICH of the two it is, so labels don't promise an opcode
              // the medium does not have.
              (geo.caps & (storage::RAW_ERASE_SECTOR | storage::RAW_ERASE_BLOCK | storage::RAW_ERASE_CHIP)) == 0
                  ? "true"
                  : "false",
#ifdef USE_STORAGE_DEVICE_NODES
              // Presentation hint for the browser — the API itself serves every device.
              s->has_device_node() ? "true" : "false"
#else
              "false"
#endif
          );
          *ctx->out += buf;
        },
        &ctx);
    json += ']';
  });
  if (!ok) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  request->send(200, "application/json", json.c_str());
}

void WebServerRawApi::cache_job_result_(storage::TransferJob job, storage::StorageError result) {
  if (job == storage::INVALID_TRANSFER_JOB)
    return;
  JobCacheEntry &e = this->job_cache_[this->job_cache_next_];
  this->job_cache_next_ = (this->job_cache_next_ + 1) % JOB_CACHE_SIZE;
  e.job = job;
  e.result = result;
}

void WebServerRawApi::handle_job_(AsyncWebServerRequest *request) {
  auto *param = request->getParam("id");
  if (param == nullptr) {
    request->send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }
  auto job = static_cast<storage::TransferJob>(strtoul(param->value().c_str(), nullptr, 10));
  storage::TransferStatus st{};
  bool found = false;
  bool done_cached = false;
  storage::StorageError cached_result = storage::StorageError::OK;
  this->run_on_loop_([this, job, &st, &found, &done_cached, &cached_result]() {
    // Cache first: a DONE job usually had its slot recycled already (same as /files/job).
    for (const auto &e : this->job_cache_) {
      if (e.job == job) {
        cached_result = e.result;
        done_cached = true;
        found = true;
        return;
      }
    }
    if (storage::global_storage_worker != nullptr) {
      found = storage::global_storage_worker->get_transfer_status(job, &st);
    }
  });
  if (!found) {
    request->send(404, "application/json", "{\"error\":\"unknown or expired job\"}");
    return;
  }
  char buf[160];
  if (done_cached) {
    snprintf(buf, sizeof(buf), "{\"state\":\"done\",\"result\":\"%s\",\"bytes_done\":0,\"bytes_total\":0}",
             storage::error_to_string(cached_result));
    request->send(200, "application/json", buf);
    return;
  }
  const char *state = "pending";
  if (st.state == storage::RequestState::RUNNING || st.state == storage::RequestState::CANCELLED) {
    state = "running";
  } else if (st.state == storage::RequestState::DONE) {
    state = "done";
  }
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"result\":\"%s\",\"bytes_done\":%" PRIu64 ",\"bytes_total\":%" PRIu64 "}",
           state, storage::error_to_string(st.result), st.bytes_done, st.bytes_total);
  request->send(200, "application/json", buf);
}

// Submits a raw worker job (translator contract: no driver I/O in HTTP context) and
// answers {"job":N} — the browser polls /files/job like it does for copy/move. Submission
// itself touches only the worker's pool and runs marshalled on the main loop.
void WebServerRawApi::submit_and_answer_(
    AsyncWebServerRequest *request,
    std::function<storage::StorageError(storage::TransferJob *, storage::CompletionCallback &&)> &&submit) {
  if (storage::global_storage_worker == nullptr) {
    request->send(501, "application/json", "{\"error\":\"no storage worker\"}");
    return;
  }
  storage::TransferJob job = storage::INVALID_TRANSFER_JOB;
  storage::StorageError err = storage::StorageError::OK;
  this->run_on_loop_([this, &submit, &job, &err]() {
    // The job id only exists after submission; the completion callback reads it through a
    // small heap slot filled right below — safe because submission and completion both run
    // on the main loop, strictly in that order (same pattern as the file API's copy/move).
    auto *job_slot = new storage::TransferJob(storage::INVALID_TRANSFER_JOB);  // NOLINT
    err = submit(&job, [this, job_slot](storage::StorageError result) {
      this->cache_job_result_(*job_slot, result);
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory)
    });
    if (err != storage::StorageError::OK) {
      delete job_slot;  // NOLINT(cppcoreguidelines-owning-memory) — callback will not fire
    } else {
      *job_slot = job;
    }
  });
  if (err != storage::StorageError::OK) {
    char ebuf[96];
    snprintf(ebuf, sizeof(ebuf), "{\"error\":\"%s\"}", storage::error_to_string(err));
    request->send(400, "application/json", ebuf);
    return;
  }
  char okbuf[48];
  snprintf(okbuf, sizeof(okbuf), "{\"job\":%u}", static_cast<unsigned>(job));
  request->send(200, "application/json", okbuf);
}

void WebServerRawApi::handle_read_(AsyncWebServerRequest *request) {
  storage::RawStorage *device = this->find_device_(request);
  if (device == nullptr) {
    request->send(404, "application/json", "{\"error\":\"no such device\"}");
    return;
  }
  uint64_t address = 0, size = 0;
  if (!this->parse_range_(request, device, &address, &size)) {
    request->send(400, "application/json", "{\"error\":\"bad or out-of-bounds range\"}");
    return;
  }

  // to_path: the node reads into a file itself; nothing streams to the client.
  if (auto *to_path = request->getParam("to_path")) {
    std::string to = to_path->value().c_str();
    bool overwrite = request->hasParam("overwrite");
    this->submit_and_answer_(
        request, [device, address, size, to, overwrite](storage::TransferJob *job, storage::CompletionCallback &&done) {
          const char *rel = nullptr;
          storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                         ? storage::global_storage_registry->resolve_path(to.c_str(), &rel)
                                         : nullptr;
          if (ps == nullptr)
            return storage::StorageError::NOT_FOUND;
          return storage::global_storage_worker->async_raw_read(device, address, size, ps, rel, std::move(done), job,
                                                                overwrite);
        });
    return;
  }

  httpd_req_t *req = *request;
  httpd_resp_set_type(req, "application/octet-stream");
  char disposition[96];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"raw_%08" PRIX32 "_%" PRIu32 ".bin\"",
           (uint32_t) address, (uint32_t) size);
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);

  auto *buf = new uint8_t[RAW_API_CHUNK];  // NOLINT(cppcoreguidelines-owning-memory)
  uint64_t offset = 0;
  while (offset < size) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(RAW_API_CHUNK, size - offset));
    size_t got = 0;
    storage::StorageError err = storage::StorageError::OK;
    // One main-loop hop per chunk: the drivers are main-loop-only, and a raw read is far too
    // fast for that to matter at these sizes.
    bool loop_ok = this->run_on_loop_(
        [device, address, offset, buf, want, &got, &err]() { err = device->read(address + offset, buf, want, &got); });
    if (!loop_ok || err != storage::StorageError::OK || got == 0) {
      ESP_LOGW(TAG, "read failed at 0x%08" PRIX32 " (%s)", (uint32_t) (address + offset),
               storage::error_to_string(err));
      break;  // response already started — end it short rather than lie with a status code
    }
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(buf), got) != ESP_OK)
      break;  // client went away
    offset += got;
  }
  delete[] buf;  // NOLINT(cppcoreguidelines-owning-memory)
  httpd_resp_send_chunk(req, nullptr, 0);
}

void WebServerRawApi::handle_erase_(AsyncWebServerRequest *request) {
  // Erase rides the write permission: it is a technical necessity of some media, not a
  // capability of its own — a device you may write is a device you may erase.
  if (!this->enable_write_) {
    request->send(403, "application/json", "{\"error\":\"erase disabled\"}");
    return;
  }
  storage::RawStorage *device = this->find_device_(request);
  if (device == nullptr) {
    request->send(404, "application/json", "{\"error\":\"no such device\"}");
    return;
  }
  uint64_t address = 0, size = 0;
  if (!this->parse_range_(request, device, &address, &size)) {
    request->send(400, "application/json", "{\"error\":\"bad or out-of-bounds range\"}");
    return;
  }

  this->submit_and_answer_(request,
                           [device, address, size](storage::TransferJob *job, storage::CompletionCallback &&done) {
                             return storage::global_storage_worker->async_raw_erase(device, address, size,
                                                                                    std::move(done), job);
                           });
}

void WebServerRawApi::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  const char *tail = url.c_str() + 5;  // past "/raw/"

  if (request->method() == HTTP_GET) {
    if (strcmp(tail, "devices") == 0) {
      this->handle_devices_(request);
      return;
    }
    if (strcmp(tail, "read") == 0) {
      this->handle_read_(request);
      return;
    }
    if (strcmp(tail, "job") == 0) {
      this->handle_job_(request);
      return;
    }
  } else if (request->method() == HTTP_POST) {
    if (strcmp(tail, "erase") == 0) {
      this->handle_erase_(request);
      return;
    }
    if (strcmp(tail, "write") == 0) {
      if (!this->enable_write_) {
        request->send(403, "application/json", "{\"error\":\"write disabled\"}");
        return;
      }
      // from_path: the payload is a file on the node, not the request body.
      if (auto *from_path = request->getParam("from_path")) {
        storage::RawStorage *device = this->find_device_(request);
        if (device == nullptr) {
          request->send(404, "application/json", "{\"error\":\"no such device\"}");
          return;
        }
        auto *addr_param = request->getParam("address");
        uint64_t address = addr_param != nullptr ? strtoull(addr_param->value().c_str(), nullptr, 0) : 0;
        auto *erase = request->getParam("erase");
        uint64_t written = 0;
        std::string from = from_path->value().c_str();
        bool erase_first = request->hasParam("erase");
        this->submit_and_answer_(
            request,
            [device, address, from, erase_first](storage::TransferJob *job, storage::CompletionCallback &&done) {
              const char *rel = nullptr;
              storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                             ? storage::global_storage_registry->resolve_path(from.c_str(), &rel)
                                             : nullptr;
              if (ps == nullptr)
                return storage::StorageError::NOT_FOUND;
              return storage::global_storage_worker->async_raw_write(ps, rel, device, address, erase_first,
                                                                     std::move(done), job);
            });
      }
      // Otherwise the body handler did the work; report its outcome.
      if (this->write_.failed) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", this->write_.error != nullptr ? this->write_.error : "failed");
        request->send(400, "application/json", buf);
      } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"written\":%" PRIu64 "}", this->write_.written);
        request->send(200, "application/json", buf);
      }
      this->write_ = WriteState{};
      return;
    }
  }
  request->send(404, "application/json", "{\"error\":\"unknown endpoint\"}");
}

void WebServerRawApi::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                 size_t total) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(url_buf);
  if (strcmp(url.c_str(), "/raw/write") != 0 || !this->enable_write_)
    return;

  if (index == 0) {
    this->write_ = WriteState{};
    this->write_.active = true;
    this->write_.device = this->find_device_(request);
    if (this->write_.device == nullptr) {
      this->write_.failed = true;
      this->write_.error = "no such device";
      return;
    }
    auto *addr_param = request->getParam("address");
    this->write_.address = addr_param != nullptr ? strtoull(addr_param->value().c_str(), nullptr, 0) : 0;

    storage::RawGeometry geo;
    storage::RawStorage *device = this->write_.device;
    this->run_on_loop_([device, &geo]() { device->get_raw_geometry(&geo); });
    if (this->write_.address >= geo.capacity || total > geo.capacity - this->write_.address) {
      this->write_.failed = true;
      this->write_.error = "out of bounds";
      return;
    }

    auto *erase = request->getParam("erase");
    if (erase != nullptr && erase->value() == "1") {
      // Erasing rounds outward to whole sectors and would take the neighbouring data with it,
      // so demand an aligned start instead of guessing what the caller meant.
      if (geo.erase_sector == 0 || (this->write_.address % geo.erase_sector) != 0) {
        this->write_.failed = true;
        this->write_.error = "address not sector aligned";
        return;
      }
      uint64_t erase_len = total;
      if ((erase_len % geo.erase_sector) != 0)
        erase_len += geo.erase_sector - (erase_len % geo.erase_sector);
      storage::StorageError err = storage::StorageError::OK;
      uint64_t addr = this->write_.address;
      this->run_on_loop_([device, addr, erase_len, &err]() { err = device->erase(addr, erase_len); }, 120000);
      if (err != storage::StorageError::OK) {
        this->write_.failed = true;
        this->write_.error = storage::error_to_string(err);
        return;
      }
    }
  }

  if (!this->write_.active || this->write_.failed)
    return;

  storage::RawStorage *device = this->write_.device;
  uint64_t at = this->write_.address + index;
  size_t done = 0;
  while (done < len && !this->write_.failed) {
    size_t written = 0;
    storage::StorageError err = storage::StorageError::OK;
    bool loop_ok = this->run_on_loop_([device, at, data, done, len, &written, &err]() {
      err = device->write(at + done, data + done, len - done, &written);
    });
    if (!loop_ok || err != storage::StorageError::OK || written == 0) {
      this->write_.failed = true;
      this->write_.error = loop_ok ? storage::error_to_string(err) : "device busy";
      return;
    }
    done += written;
  }
  this->write_.written += done;
}

}  // namespace esphome::web_server

#endif  // USE_WEBSERVER_RAW_API && USE_ESP_IDF
