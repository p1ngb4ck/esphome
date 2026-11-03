#include "cert_store.h"

#ifdef USE_ESP_IDF

#include "esphome/core/log.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
#endif

#include <cstdio>
#include <cstring>
#include <new>

namespace esphome::cert_store {

static const char *const TAG = "cert_store";

CertStore *global_cert_store = nullptr;

CertStore *CertStore::create() {
  void *mem = RAMAllocator<CertStore>(RAMAllocator<CertStore>::ALLOC_EXTERNAL).allocate(1);
  return mem != nullptr ? new (mem) CertStore() : new CertStore();
}

void CertStore::setup() { global_cert_store = this; }

void CertStore::dump_config() {
  ESP_LOGCONFIG(TAG, "Cert store:\n  Directory: %s", this->directory_ != nullptr ? this->directory_ : "none");
#ifdef USE_CERT_STORE_LINK
  ESP_LOGCONFIG(TAG, "  Relay: %s\n  HTTP sources: %u", YESNO(this->relay_source_), this->http_count_);
#endif
}

const CertRef &CertStore::ref_(uint8_t step) const {
  return step == STEP_CA ? this->refs_.ca : step == STEP_CERT ? this->refs_.cert : this->refs_.key;
}

void CertStore::apply_async(mbedtls_ssl_config *conf, TlsMaterial *out, const TlsRefs &refs, Rng rng, void *rng_ctx,
                            ApplyCallback &&done) {
  if (this->applying_) {
    done(storage::StorageError::STORAGE_ERROR_NOT_READY);
    return;
  }
  this->applying_ = true;
  this->conf_ = conf;
  this->out_ = out;
  this->refs_ = refs;
  this->rng_ = rng;
  this->rng_ctx_ = rng_ctx;
  this->done_ = std::move(done);
  this->step_ = STEP_CA;
  this->next_();
}

void CertStore::next_() {
  while (this->step_ != STEP_DONE) {
    const CertRef &ref = this->ref_(this->step_);
    if (ref.source != CertSource::NONE) {
      this->load_(ref);
      return;
    }
    if (this->step_ == STEP_CA) {
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
      if (esp_crt_bundle_attach(this->conf_) != 0) {
        ESP_LOGE(TAG, "Attaching the CA bundle failed");
        this->finish_(storage::StorageError::STORAGE_ERROR_READ_ERROR);
        return;
      }
#else
      this->finish_(storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED);
      return;
#endif
    }
    this->step_++;
  }
  if (this->refs_.cert.source != CertSource::NONE &&
      mbedtls_ssl_conf_own_cert(this->conf_, &this->out_->cert, &this->out_->key) != 0) {
    ESP_LOGE(TAG, "Client certificate and key not taken");
    this->finish_(storage::StorageError::STORAGE_ERROR_INVALID_ARGS);
    return;
  }
  this->finish_(storage::StorageError::STORAGE_ERROR_OK);
}

void CertStore::load_(const CertRef &ref) {
  this->from_directory_ = false;
  switch (ref.source) {
    case CertSource::EMBEDDED:
      this->loaded_(reinterpret_cast<const uint8_t *>(ref.data), ref.len);
      return;
    case CertSource::STORAGE:
      if (!this->read_path_(ref.data))
        this->finish_(storage::StorageError::STORAGE_ERROR_NOT_READY);
      return;
    default:
      break;
  }
  if (this->directory_ != nullptr &&
      snprintf(this->path_, sizeof(this->path_), "%s/%s", this->directory_, ref.data) < static_cast<int>(sizeof(this->path_))) {
    this->from_directory_ = true;
    if (this->read_path_(this->path_))
      return;
  }
#ifdef USE_CERT_STORE_LINK
  if (this->fetch_remote_(0))
    return;
#endif
  ESP_LOGW(TAG, "Certificate '%s' not found", ref.data);
  this->finish_(storage::StorageError::STORAGE_ERROR_NOT_FOUND);
}

void CertStore::loaded_(const uint8_t *data, size_t len) {
  int ret;
  if (this->step_ == STEP_CA) {
    ret = mbedtls_x509_crt_parse(&this->out_->ca, data, len + 1);
    if (ret == 0)
      mbedtls_ssl_conf_ca_chain(this->conf_, &this->out_->ca, nullptr);
  } else if (this->step_ == STEP_CERT) {
    ret = mbedtls_x509_crt_parse(&this->out_->cert, data, len + 1);
  } else {
    ret = mbedtls_pk_parse_key(&this->out_->key, data, len + 1, nullptr, 0, this->rng_, this->rng_ctx_);
  }
  this->release_buffer_();
  if (ret != 0) {
    ESP_LOGE(TAG, "%s not parsed (-0x%04X)",
             this->step_ == STEP_CA ? "CA" : this->step_ == STEP_CERT ? "Client certificate" : "Client key",
             static_cast<unsigned>(-ret));
    this->finish_(storage::StorageError::STORAGE_ERROR_READ_ERROR);
    return;
  }
  this->step_++;
  this->next_();
}

void CertStore::finish_(storage::StorageError err) {
  if (this->stream_open_ && storage::global_storage_worker != nullptr) {
    storage::global_storage_worker->end_read(this->stream_, [](storage::StorageError) {});
    this->stream_open_ = false;
  }
  this->release_buffer_();
  ApplyCallback done = std::move(this->done_);
  this->done_ = nullptr;
  this->conf_ = nullptr;
  this->out_ = nullptr;
  this->step_ = STEP_DONE;
  this->applying_ = false;
  if (done)
    done(err);
}

bool CertStore::grow_(size_t need) {
  if (need <= this->buf_cap_)
    return true;
  size_t cap = this->buf_cap_ == 0 ? READ_CHUNK + 1 : this->buf_cap_;
  while (cap < need)
    cap *= 2;
  if (cap > MAX_BYTES + 1)
    cap = MAX_BYTES + 1;
  if (cap < need) {
    ESP_LOGE(TAG, "Certificate over %u bytes", static_cast<unsigned>(MAX_BYTES));
    return false;
  }
  RAMAllocator<uint8_t> alloc(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  uint8_t *bigger = alloc.allocate(cap);
  if (bigger == nullptr)
    return false;
  if (this->buf_ != nullptr) {
    std::memcpy(bigger, this->buf_, this->buf_off_);
    alloc.deallocate(this->buf_, this->buf_cap_);
  }
  this->buf_ = bigger;
  this->buf_cap_ = cap;
  return true;
}

void CertStore::release_buffer_() {
  if (this->buf_ != nullptr)
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->buf_, this->buf_cap_);
  this->buf_ = nullptr;
  this->buf_cap_ = 0;
  this->buf_off_ = 0;
}

bool CertStore::read_path_(const char *path) {
  if (storage::global_storage_worker == nullptr || storage::global_storage_registry == nullptr)
    return false;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path, &rel);
  if (ps == nullptr)
    return false;
  this->buf_off_ = 0;
  this->last_read_ = 0;
  this->stream_open_ = false;
  return storage::global_storage_worker->begin_read(ps, rel, &this->stream_, [this](storage::StorageError e) {
           this->on_open_(e);
         }) == storage::StorageError::STORAGE_ERROR_OK;
}

void CertStore::on_open_(storage::StorageError err) {
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->read_failed_(err);
    return;
  }
  this->stream_open_ = true;
  this->issue_read_();
}

void CertStore::issue_read_() {
  if (!this->grow_(this->buf_off_ + READ_CHUNK + 1)) {
    this->finish_(storage::StorageError::STORAGE_ERROR_NO_SPACE);
    return;
  }
  this->last_read_ = 0;
  const storage::StorageError e = storage::global_storage_worker->read_chunk(
      this->stream_, this->buf_ + this->buf_off_, READ_CHUNK, &this->last_read_,
      [this](storage::StorageError re) { this->on_read_(re); });
  if (e != storage::StorageError::STORAGE_ERROR_OK)
    this->finish_(e);
}

void CertStore::on_read_(storage::StorageError err) {
  if (!this->applying_)
    return;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->finish_(err);
    return;
  }
  this->buf_off_ += this->last_read_;
  if (this->last_read_ != 0) {
    this->issue_read_();
    return;
  }
  const storage::StorageError e =
      storage::global_storage_worker->end_read(this->stream_, [this](storage::StorageError ce) { this->on_closed_(ce); });
  if (e != storage::StorageError::STORAGE_ERROR_OK)
    this->finish_(e);
}

void CertStore::on_closed_(storage::StorageError err) {
  if (!this->applying_)
    return;
  this->stream_open_ = false;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->finish_(err);
    return;
  }
  this->buf_[this->buf_off_] = 0;
  this->loaded_(this->buf_, this->buf_off_);
}

void CertStore::read_failed_(storage::StorageError err) {
#ifdef USE_CERT_STORE_LINK
  if (this->from_directory_ && this->fetch_remote_(0))
    return;
#endif
  this->finish_(err);
}

#ifdef USE_CERT_STORE_LINK
uint8_t CertStore::next_source_(uint8_t from) const {
  if (this->link_ == nullptr)
    return NO_SOURCE;
  for (uint8_t s = from; s <= this->http_count_; s++) {
    if (s != 0 || (this->relay_source_ && this->link_->has_url()))
      return s;
  }
  return NO_SOURCE;
}

bool CertStore::fetch_remote_(uint8_t from) {
  const uint8_t source = this->next_source_(from);
  if (source == NO_SOURCE || !this->grow_(MAX_BYTES + 1))
    return false;
  return this->link_->submit(&CertStore::fetch_job_, this, source);
}

void CertStore::fetch_job_(void *ctx, uint16_t source) {
  auto *self = static_cast<CertStore *>(ctx);
  const std::string path = "/cert?name=" + relay_link::RelayLink::url_encoded(self->ref_(self->step_).data);
  size_t len = 0;
  const bool ok = source == 0 ? self->link_->http(path.c_str(), nullptr, nullptr, &len, self->buf_, MAX_BYTES)
                              : self->link_->http_at(self->http_sources_[source - 1], nullptr, path.c_str(), nullptr,
                                                     nullptr, &len, self->buf_, MAX_BYTES);
  if (ok && len != 0 && len <= MAX_BYTES) {
    self->buf_[len] = 0;
    self->loaded_(self->buf_, len);
    return;
  }
  if (!self->fetch_remote_(static_cast<uint8_t>(source + 1))) {
    ESP_LOGW(TAG, "Certificate '%s' not found", self->ref_(self->step_).data);
    self->finish_(storage::StorageError::STORAGE_ERROR_NOT_FOUND);
  }
}
#endif

}

#endif
