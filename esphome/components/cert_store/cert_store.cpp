#include "cert_store.h"
#ifdef USE_ESP_IDF

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif

namespace esphome {
namespace cert_store {

static const char *const TAG = "cert_store";

CertStore *global_cert_store = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Bytes requested per worker read_chunk() call for the STORAGE path.
static constexpr size_t READ_CHUNK = 2048;
// A CA/chain far larger than this is not a device trust anchor -- refuse rather than grow without
// bound (the whole point of the interface is no runaway RAM on an MCU).
static constexpr size_t MAX_CERT_BYTES = 10240;

const char *cert_kind_to_string(CertKind kind) {
  switch (kind) {
    case CertKind::CA:
      return "ca";
    case CertKind::CLIENT_CERT:
      return "client_cert";
    case CertKind::PRIVATE_KEY:
      return "private_key";
    case CertKind::PSK:
      return "psk";
    case CertKind::SSH_KNOWN_HOST:
      return "ssh_known_host";
    case CertKind::SSH_USER_KEY:
      return "ssh_user_key";
    default:
      return "raw";
  }
}

static const char *source_to_string(CertSource s) { return s == CertSource::EMBEDDED ? "embedded" : "storage"; }

void CertStore::setup() {
  // Publish the single instance so consumers can reach it without naming it. Nothing is loaded
  // here -- entries are materialised on demand when a consumer applies them.
  global_cert_store = this;
}

const CertStore::Entry *CertStore::find(StringRef id) const {
  for (const auto &entry : this->entries_) {
    if (id == entry.id)
      return &entry;
  }
  return nullptr;
}

storage::StorageError CertStore::attach_parsed_(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_out,
                                                const uint8_t *data, size_t len) {
  mbedtls_x509_crt_init(ca_out);
  // PEM parsing requires the terminating NUL to be counted in the length.
  if (mbedtls_x509_crt_parse(ca_out, data, len + 1) != 0) {
    ESP_LOGE(TAG, "CA parse failed");
    return storage::StorageError::STORAGE_ERROR_READ_ERROR;
  }
  mbedtls_ssl_conf_ca_chain(conf, ca_out, nullptr);
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError CertStore::attach_bundle_(mbedtls_ssl_config *conf) {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
  // The Mozilla root bundle ESP-IDF ships lives in flash and is attached directly to the config --
  // no per-cert RAM, no storage read. This is the default trust anchor for public endpoints.
  if (esp_crt_bundle_attach(conf) != 0) {
    ESP_LOGE(TAG, "attaching the built-in CA bundle failed");
    return storage::StorageError::STORAGE_ERROR_READ_ERROR;
  }
  return storage::StorageError::STORAGE_ERROR_OK;
#else
  ESP_LOGE(TAG, "no CA entry given and the built-in bundle is not compiled in");
  return storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED;
#endif
}

void CertStore::apply_ca_async(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_out, StringRef id,
                               ApplyCallback &&on_done) {
  const Entry *entry = id.size() == 0 ? nullptr : this->find(id);

  // No entry named (or unknown id) -> the built-in bundle. Immediate, no RAM, no worker.
  if (entry == nullptr) {
    if (id.size() != 0)
      ESP_LOGW(TAG, "CA entry '%.*s' unknown -- falling back to the built-in bundle", (int) id.size(), id.c_str());
    on_done(this->attach_bundle_(conf));
    return;
  }

  if (entry->kind != CertKind::CA) {
    ESP_LOGE(TAG, "'%s' is a %s entry, not a CA", entry->id, cert_kind_to_string(entry->kind));
    on_done(storage::StorageError::STORAGE_ERROR_INVALID_ARGS);
    return;
  }

  // EMBEDDED -> parse straight out of flash, no RAM buffer at all.
  if (entry->source == CertSource::EMBEDDED) {
    on_done(this->attach_parsed_(conf, ca_out, reinterpret_cast<const uint8_t *>(entry->embedded_data),
                                 entry->embedded_len));
    return;
  }

  // STORAGE -> stream the file on demand through the worker, parse, then free the buffer.
#ifdef USE_STORAGE_WORKER
  if (this->applying_) {
    on_done(storage::StorageError::STORAGE_ERROR_NOT_READY);  // one apply in flight; caller retries
    return;
  }
  if (storage::global_storage_worker == nullptr || storage::global_storage_registry == nullptr) {
    on_done(storage::StorageError::STORAGE_ERROR_NOT_READY);
    return;
  }
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(entry->path, &rel);
  if (ps == nullptr) {
    ESP_LOGW(TAG, "'%s': storage for '%s' not mounted yet", entry->id, entry->path);
    on_done(storage::StorageError::STORAGE_ERROR_NOT_READY);
    return;
  }
  this->applying_ = true;
  this->apply_conf_ = conf;
  this->apply_ca_ = ca_out;
  this->apply_cb_ = std::move(on_done);
  this->buf_ = nullptr;
  this->buf_cap_ = 0;
  this->buf_off_ = 0;
  this->last_read_ = 0;
  this->stream_open_ = false;
  storage::StorageError err = storage::global_storage_worker->begin_read(
      ps, rel, &this->stream_, [this](storage::StorageError e) { this->on_open_(e); });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->finish_storage_(err);
#else
  on_done(storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED);
#endif
}

#ifdef USE_STORAGE_WORKER

void CertStore::release_buffer_() {
  if (this->buf_ != nullptr) {
    this->alloc_.deallocate(this->buf_, this->buf_cap_);
    this->buf_ = nullptr;
  }
  this->buf_cap_ = 0;
  this->buf_off_ = 0;
}

void CertStore::on_open_(storage::StorageError err) {
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->finish_storage_(err);
    return;
  }
  this->stream_open_ = true;
  this->issue_read_();
}

void CertStore::issue_read_() {
  // Grow (doubling) to hold one more chunk plus a byte of slack for the terminating NUL.
  if (this->buf_cap_ < this->buf_off_ + READ_CHUNK + 1) {
    size_t cap = this->buf_cap_ == 0 ? READ_CHUNK + 1 : this->buf_cap_;
    while (cap < this->buf_off_ + READ_CHUNK + 1)
      cap *= 2;
    if (cap > MAX_CERT_BYTES + 1) {
      ESP_LOGE(TAG, "certificate exceeds %u bytes -- refusing", (unsigned) MAX_CERT_BYTES);
      this->finish_storage_(storage::StorageError::STORAGE_ERROR_NO_SPACE);
      return;
    }
    uint8_t *bigger = this->alloc_.allocate(cap);
    if (bigger == nullptr) {
      ESP_LOGE(TAG, "out of memory (%u bytes)", (unsigned) cap);
      this->finish_storage_(storage::StorageError::STORAGE_ERROR_READ_ERROR);
      return;
    }
    if (this->buf_ != nullptr) {
      std::memcpy(bigger, this->buf_, this->buf_off_);
      this->alloc_.deallocate(this->buf_, this->buf_cap_);
    }
    this->buf_ = bigger;
    this->buf_cap_ = cap;
  }
  this->last_read_ = 0;
  storage::StorageError e = storage::global_storage_worker->read_chunk(
      this->stream_, this->buf_ + this->buf_off_, READ_CHUNK, &this->last_read_,
      [this](storage::StorageError re) { this->on_read_(re); });
  if (e != storage::StorageError::STORAGE_ERROR_OK)
    this->finish_storage_(e);
}

void CertStore::on_read_(storage::StorageError err) {
  if (!this->applying_)
    return;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->finish_storage_(err);
    return;
  }
  this->buf_off_ += this->last_read_;
  if (this->last_read_ == 0) {
    storage::StorageError e = storage::global_storage_worker->end_read(
        this->stream_, [this](storage::StorageError ce) { this->on_closed_(ce); });
    if (e != storage::StorageError::STORAGE_ERROR_OK)
      this->finish_storage_(e);
    return;
  }
  this->issue_read_();
}

void CertStore::on_closed_(storage::StorageError err) {
  if (!this->applying_)
    return;
  this->stream_open_ = false;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->finish_storage_(err);
    return;
  }
  // Terminate so mbedTLS PEM parsing has its NUL, parse into the consumer's ca_out, attach.
  this->buf_[this->buf_off_] = 0;
  storage::StorageError perr = attach_parsed_(this->apply_conf_, this->apply_ca_, this->buf_, this->buf_off_);
  this->finish_storage_(perr);
}

void CertStore::finish_storage_(storage::StorageError err) {
  // Single exit for the STORAGE path: close a still-open stream, free the buffer NOW (the parse,
  // if any, is already done), then fire the consumer callback and clear all in-flight state.
  if (this->stream_open_ && storage::global_storage_worker != nullptr) {
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
    this->stream_open_ = false;
  }
  this->release_buffer_();
  ApplyCallback cb = std::move(this->apply_cb_);
  this->apply_cb_ = nullptr;
  this->apply_conf_ = nullptr;
  this->apply_ca_ = nullptr;
  this->applying_ = false;
  if (cb)
    cb(err);
}

#endif  // USE_STORAGE_WORKER

void CertStore::dump_config() {
  ESP_LOGCONFIG(TAG, "Cert store:");
  for (const auto &entry : this->entries_) {
    ESP_LOGCONFIG(TAG, "  %s (%s, %s)", entry.id, cert_kind_to_string(entry.kind), source_to_string(entry.source));
  }
}

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
