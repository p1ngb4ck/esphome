#include "cert_store.h"
#ifdef USE_ESP_IDF

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "mbedtls/x509_crt.h"

namespace esphome {
namespace cert_store {

static const char *const TAG = "cert_store";

CertStore *global_cert_store = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// How often an entry is retried while its storage is not mounted yet.
static constexpr uint32_t RETRY_INTERVAL_MS = 2000;
// Bytes requested per worker read_chunk() call.
static constexpr size_t READ_CHUNK = 4096;

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

void CertStore::setup() {
  // Publish the single instance so consumers can reach it without naming it.
  global_cert_store = this;
  // Files live on storages that may mount seconds after boot (or never), so loop() drives the loads.
  if (this->entries_.empty())
    this->ready_ = true;
}

void CertStore::loop() {
  if (this->ready_)
    return;
#ifdef USE_STORAGE_WORKER
  // A read is already in flight; its stream callbacks carry it forward.
  if (this->loading_ != NO_LOAD)
    return;
#endif
  const uint32_t now = millis();
  if (now - this->last_try_ms_ < RETRY_INTERVAL_MS)
    return;
  this->last_try_ms_ = now;

#ifdef USE_STORAGE_WORKER
  for (size_t i = 0; i < this->entries_.size(); i++) {
    if (!this->entries_[i].loaded) {
      this->start_load_(i);
      return;  // one entry in flight at a time
    }
  }
#endif
  this->finish_if_complete_();
}

void CertStore::finish_if_complete_() {
  for (auto &entry : this->entries_) {
    if (!entry.loaded)
      return;
  }
  this->ready_ = true;
  ESP_LOGI(TAG, "all entries loaded");
  this->disable_loop();
}

#ifdef USE_STORAGE_WORKER

void CertStore::start_load_(size_t index) {
  Entry &entry = this->entries_[index];
  if (storage::global_storage_worker == nullptr || storage::global_storage_registry == nullptr)
    return;
  // The registry resolves which storage this path lives on -- no manual storage selection.
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(entry.path, &rel);
  if (ps == nullptr)
    return;  // not mounted yet -- loop() comes back

  this->loading_ = index;
  this->pending_buf_ = nullptr;
  this->pending_len_ = 0;
  this->pending_off_ = 0;
  this->last_read_ = 0;
  this->stream_open_ = false;

  storage::StorageError err = storage::global_storage_worker->begin_read(
      ps, rel, &this->stream_, [this](storage::StorageError e) { this->on_open_(e); });
  if (err != storage::StorageError::OK)
    this->abandon_load_("open rejected", err);
}

void CertStore::on_open_(storage::StorageError err) {
  if (err != storage::StorageError::OK) {
    this->abandon_load_("open failed", err);
    return;
  }
  this->stream_open_ = true;
  this->issue_read_();
}

void CertStore::issue_read_() {
  // Grow (doubling) to hold one more chunk plus a byte of slack for the terminating NUL.
  if (this->pending_len_ < this->pending_off_ + READ_CHUNK + 1) {
    size_t cap = this->pending_len_ == 0 ? READ_CHUNK + 1 : this->pending_len_;
    while (cap < this->pending_off_ + READ_CHUNK + 1)
      cap *= 2;
    RAMAllocator<uint8_t> allocator;
    uint8_t *bigger = allocator.allocate(cap);
    if (bigger == nullptr) {
      ESP_LOGE(TAG, "'%s': out of memory (%u bytes)", this->entries_[this->loading_].id, (unsigned) cap);
      this->abandon_load_("no memory", storage::StorageError::READ_ERROR);
      return;
    }
    if (this->pending_buf_ != nullptr) {
      std::memcpy(bigger, this->pending_buf_, this->pending_off_);
      allocator.deallocate(this->pending_buf_, this->pending_len_);
    }
    this->pending_buf_ = bigger;
    this->pending_len_ = cap;
  }
  this->last_read_ = 0;
  storage::StorageError e = storage::global_storage_worker->read_chunk(
      this->stream_, this->pending_buf_ + this->pending_off_, READ_CHUNK, &this->last_read_,
      [this](storage::StorageError re) { this->on_read_(re); });
  if (e != storage::StorageError::OK)
    this->abandon_load_("read rejected", e);
}

void CertStore::on_read_(storage::StorageError err) {
  if (this->loading_ == NO_LOAD)
    return;
  if (err != storage::StorageError::OK) {
    this->abandon_load_("read failed", err);
    return;
  }
  this->pending_off_ += this->last_read_;
  if (this->last_read_ == 0) {
    storage::StorageError e = storage::global_storage_worker->end_read(
        this->stream_, [this](storage::StorageError ce) { this->on_closed_(ce); });
    if (e != storage::StorageError::OK)
      this->abandon_load_("close rejected", e);
    return;
  }
  this->issue_read_();
}

void CertStore::on_closed_(storage::StorageError err) {
  if (this->loading_ == NO_LOAD)
    return;
  this->stream_open_ = false;
  if (err != storage::StorageError::OK) {
    this->abandon_load_("close failed", err);
    return;
  }
  Entry &entry = this->entries_[this->loading_];
  // The buffer always has slack for the NUL (see issue_read_), so PEM consumers get a C-string and
  // mbedTLS PEM parsing has its terminator.
  this->pending_buf_[this->pending_off_] = 0;
  entry.data = this->pending_buf_;
  entry.len = this->pending_off_;
  entry.loaded = true;
  entry.valid = validate_(entry);
  ESP_LOGD(TAG, "'%s' (%s): %u bytes from '%s'%s", entry.id, cert_kind_to_string(entry.kind),
           (unsigned) entry.len, entry.path, entry.valid ? "" : " -- INVALID");
  if (!entry.valid)
    ESP_LOGW(TAG, "'%s': does not parse as a valid certificate", entry.id);

  this->pending_buf_ = nullptr;
  this->pending_len_ = 0;
  this->pending_off_ = 0;
  this->loading_ = NO_LOAD;
  this->finish_if_complete_();
}

void CertStore::abandon_load_(const char *reason, storage::StorageError err) {
  if (this->loading_ != NO_LOAD) {
    Entry &entry = this->entries_[this->loading_];
    if (!entry.warned) {
      ESP_LOGW(TAG, "'%s': %s (%s)", entry.path, reason, storage::error_to_string(err));
      entry.warned = true;
    }
  }
  if (this->stream_open_) {
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
    this->stream_open_ = false;
  }
  if (this->pending_buf_ != nullptr) {
    RAMAllocator<uint8_t> allocator;
    allocator.deallocate(this->pending_buf_, this->pending_len_);
    this->pending_buf_ = nullptr;
  }
  this->pending_len_ = 0;
  this->pending_off_ = 0;
  this->loading_ = NO_LOAD;
}

#endif  // USE_STORAGE_WORKER

bool CertStore::validate_(const Entry &entry) {
  // Only certificates are structurally checked. Private keys and SSH material come in formats
  // mbedTLS x509 does not cover (PKCS#8, OpenSSH, raw); they are handed to their consumers as-is.
  if (entry.kind != CertKind::CA && entry.kind != CertKind::CLIENT_CERT)
    return true;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  // +1 so the terminating NUL is included, as PEM parsing requires.
  const int ret = mbedtls_x509_crt_parse(&crt, entry.data, entry.len + 1);
  mbedtls_x509_crt_free(&crt);
  return ret == 0;
}

const CertStore::Entry *CertStore::find(StringRef id) const {
  for (const auto &entry : this->entries_) {
    if (entry.loaded && id == entry.id)
      return &entry;
  }
  return nullptr;
}

const uint8_t *CertStore::get(StringRef id, size_t *len_out) const {
  const Entry *entry = this->find(id);
  if (entry == nullptr)
    return nullptr;
  if (len_out != nullptr)
    *len_out = entry->len;
  return entry->data;
}

void CertStore::dump_config() {
  ESP_LOGCONFIG(TAG, "Cert store:");
  for (const auto &entry : this->entries_) {
    ESP_LOGCONFIG(TAG, "  %s (%s): %s%s", entry.id, cert_kind_to_string(entry.kind),
                  entry.loaded ? "loaded" : "pending", entry.loaded && !entry.valid ? ", INVALID" : "");
  }
}

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
