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
                "  Write enabled: %s\n"
                "  Erase enabled: %s",
                YESNO(this->enable_write_), YESNO(this->enable_erase_));
}

bool WebServerRawApi::run_on_loop_(std::function<void()> &&op, uint32_t timeout_ms) {
  // defer() is the documented thread-safe FIFO bridge into the main loop — same mechanism the
  // file API uses from this very httpd task.
  SemaphoreHandle_t done = this->op_done_;
  this->defer([op = std::move(op), done]() {
    op();
    xSemaphoreGive(done);
  });
  return xSemaphoreTake(done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
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
    } ctx{&json, this->scoped_device_, this->enable_write_, this->enable_erase_};
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
          // Worst case is 250 bytes: 169 of template, a 20-digit capacity, three 10-digit
          // geometry fields and six "false" — snprintf would silently truncate a shorter one
          // and hand the browser JSON it cannot parse.
          char buf[256];
          // The geometry is what a client needs to offer only what this medium can do — the
          // capability bits, not a guess from the kind string.
          snprintf(
              buf, sizeof(buf),
              "\",\"capacity\":%" PRIu64 ",\"write_page\":%" PRIu32 ",\"erase_sector\":%" PRIu32
              ",\"erase_block\":%" PRIu32 ",\"write_needs_erase\":%s,\"can_erase_sector\":%s,\"can_erase_block\":%s"
              ",\"can_erase_chip\":%s,\"writable\":%s,\"erasable\":%s,\"node\":%s}",
              geo.capacity, geo.write_page, geo.erase_sector, geo.erase_block,
              (geo.caps & storage::RAW_WRITE_NEEDS_ERASE) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_SECTOR) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_BLOCK) != 0 ? "true" : "false",
              (geo.caps & storage::RAW_ERASE_CHIP) != 0 ? "true" : "false",
              // What this build allows, so the browser never offers a button that can only 403.
              ctx->enable_write ? "true" : "false",
              // Erasable means both: allowed here and the medium actually has an erase.
              ctx->enable_erase &&
                      (geo.caps & (storage::RAW_ERASE_SECTOR | storage::RAW_ERASE_BLOCK | storage::RAW_ERASE_CHIP)) != 0
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

bool WebServerRawApi::read_to_path_(storage::RawStorage *device, uint64_t address, uint64_t size, const char *path,
                                    const char **error) {
  auto buf_size = static_cast<size_t>(size);
  uint8_t *raw = RAMAllocator<uint8_t>().allocate(buf_size);
  if (raw == nullptr) {
    *error = "out of memory";
    return false;
  }
  storage::RamBuffer buf(raw, storage::RamBufferDeleter{buf_size});

  uint64_t offset = 0;
  while (offset < size) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(RAW_API_CHUNK, size - offset));
    size_t got = 0;
    storage::StorageError err = storage::StorageError::OK;
    uint8_t *dst = buf.get() + offset;
    bool ok = this->run_on_loop_(
        [device, address, offset, dst, want, &got, &err]() { err = device->read(address + offset, dst, want, &got); });
    if (!ok || err != storage::StorageError::OK || got == 0) {
      *error = ok ? storage::error_to_string(err) : "device busy";
      return false;
    }
    offset += got;
  }

  storage::StorageError werr = storage::StorageError::OK;
  const uint8_t *data = buf.get();
  bool ok = this->run_on_loop_(
      [path, data, buf_size, &werr]() {
        const char *rel = nullptr;
        storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                       ? storage::global_storage_registry->resolve_path(path, &rel)
                                       : nullptr;
        werr = ps == nullptr ? storage::StorageError::NOT_FOUND : storage::write_file(ps, rel, data, buf_size);
      },
      60000);
  if (!ok || werr != storage::StorageError::OK) {
    *error = ok ? storage::error_to_string(werr) : "storage busy";
    return false;
  }
  return true;
}

bool WebServerRawApi::write_from_path_(storage::RawStorage *device, uint64_t address, const char *path,
                                       bool erase_first, uint64_t *written, const char **error) {
  storage::RamBuffer buf;
  size_t size = 0;
  storage::StorageError rerr = storage::StorageError::OK;
  bool ok = this->run_on_loop_(
      [path, &buf, &size, &rerr]() {
        const char *rel = nullptr;
        storage::PathStorage *ps = storage::global_storage_registry != nullptr
                                       ? storage::global_storage_registry->resolve_path(path, &rel)
                                       : nullptr;
        rerr = ps == nullptr ? storage::StorageError::NOT_FOUND : storage::read_file(ps, rel, buf, &size);
      },
      60000);
  if (!ok || rerr != storage::StorageError::OK) {
    *error = ok ? storage::error_to_string(rerr) : "storage busy";
    return false;
  }

  storage::RawGeometry geo;
  this->run_on_loop_([device, &geo]() { device->get_raw_geometry(&geo); });
  if (address >= geo.capacity || size > geo.capacity - address) {
    *error = "file does not fit at this address";
    return false;
  }
  if (erase_first) {
    if (geo.erase_sector == 0 || (address % geo.erase_sector) != 0) {
      *error = "address not sector aligned";
      return false;
    }
    uint64_t erase_len = size;
    if ((erase_len % geo.erase_sector) != 0)
      erase_len += geo.erase_sector - (erase_len % geo.erase_sector);
    storage::StorageError eerr = storage::StorageError::OK;
    bool eok =
        this->run_on_loop_([device, address, erase_len, &eerr]() { eerr = device->erase(address, erase_len); }, 120000);
    if (!eok || eerr != storage::StorageError::OK) {
      *error = eok ? storage::error_to_string(eerr) : "device busy";
      return false;
    }
  }

  uint64_t done = 0;
  while (done < size) {
    size_t want = static_cast<size_t>(std::min<uint64_t>(RAW_API_CHUNK, size - done));
    size_t n = 0;
    storage::StorageError werr = storage::StorageError::OK;
    const uint8_t *src = buf.get() + done;
    bool wok = this->run_on_loop_(
        [device, address, done, src, want, &n, &werr]() { werr = device->write(address + done, src, want, &n); });
    if (!wok || werr != storage::StorageError::OK || n == 0) {
      *error = wok ? storage::error_to_string(werr) : "device busy";
      return false;
    }
    done += n;
  }
  *written = done;
  return true;
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
    const char *error = nullptr;
    if (!this->read_to_path_(device, address, size, to_path->value().c_str(), &error)) {
      char ebuf[128];
      snprintf(ebuf, sizeof(ebuf), "{\"error\":\"%s\"}", error != nullptr ? error : "failed");
      request->send(400, "application/json", ebuf);
      return;
    }
    char okbuf[64];
    snprintf(okbuf, sizeof(okbuf), "{\"read\":%" PRIu64 "}", size);
    request->send(200, "application/json", okbuf);
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
  if (!this->enable_erase_) {
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

  storage::StorageError err = storage::StorageError::OK;
  // A whole-device erase can take a minute on a large flash; the timeout covers the worst case
  // rather than reporting a failure while the chip is still busy.
  bool ok = this->run_on_loop_([device, address, size, &err]() { err = device->erase(address, size); }, 120000);
  if (!ok) {
    request->send(504, "application/json", "{\"error\":\"erase timed out\"}");
    return;
  }
  if (err != storage::StorageError::OK) {
    // NOT_SUPPORTED (medium has no erase) and INVALID_ARGS (range not sector-aligned) travel
    // verbatim: the client asked for something this device cannot do, and should say so.
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", storage::error_to_string(err));
    request->send(err == storage::StorageError::NOT_SUPPORTED ? 501 : 400, "application/json", buf);
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"erased\":%" PRIu64 "}", size);
  request->send(200, "application/json", buf);
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
        const char *error = nullptr;
        if (!this->write_from_path_(device, address, from_path->value().c_str(),
                                    erase != nullptr && erase->value() == "1", &written, &error)) {
          char ebuf[128];
          snprintf(ebuf, sizeof(ebuf), "{\"error\":\"%s\"}", error != nullptr ? error : "failed");
          request->send(400, "application/json", ebuf);
          return;
        }
        char okbuf[64];
        snprintf(okbuf, sizeof(okbuf), "{\"written\":%" PRIu64 "}", written);
        request->send(200, "application/json", okbuf);
        return;
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
