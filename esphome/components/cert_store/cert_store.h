#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#ifdef USE_CERT_STORE_LINK
#include "esphome/components/relay_link/relay_link.h"
#endif

#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <functional>

namespace esphome::cert_store {

enum class CertSource : uint8_t { NONE, EMBEDDED, STORAGE, NAMED };

struct CertRef {
  CertSource source{CertSource::NONE};
  const char *data{nullptr};
  size_t len{0};
};

struct TlsRefs {
  CertRef ca;
  CertRef cert;
  CertRef key;
};

struct TlsMaterial {
  mbedtls_x509_crt ca;
  mbedtls_x509_crt cert;
  mbedtls_pk_context key;
  void init() {
    mbedtls_x509_crt_init(&this->ca);
    mbedtls_x509_crt_init(&this->cert);
    mbedtls_pk_init(&this->key);
  }
  void free() {
    mbedtls_x509_crt_free(&this->ca);
    mbedtls_x509_crt_free(&this->cert);
    mbedtls_pk_free(&this->key);
  }
};

class CertStore : public Component {
 public:
  using ApplyCallback = std::function<void(storage::StorageError)>;
  using Rng = int (*)(void *, unsigned char *, size_t);

  static CertStore *create();

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_directory(const char *directory) { this->directory_ = directory; }
#ifdef USE_CERT_STORE_LINK
  void set_relay_link(relay_link::RelayLink *link) { this->link_ = link; }
  void set_relay(bool relay) { this->relay_source_ = relay; }
  void add_http_source(const char *url) {
    if (this->http_count_ < MAX_HTTP)
      this->http_sources_[this->http_count_++] = url;
  }
#endif

  void apply_async(mbedtls_ssl_config *conf, TlsMaterial *out, const TlsRefs &refs, Rng rng, void *rng_ctx,
                   ApplyCallback &&done);

 protected:
  static constexpr size_t READ_CHUNK = 2048;
  static constexpr size_t MAX_BYTES = 10240;
  static constexpr uint8_t MAX_HTTP = 4;
  static constexpr uint8_t NO_SOURCE = 0xFF;
  enum Step : uint8_t { STEP_CA, STEP_CERT, STEP_KEY, STEP_DONE };

  const CertRef &ref_(uint8_t step) const;
  void next_();
  void load_(const CertRef &ref);
  void loaded_(const uint8_t *data, size_t len);
  void finish_(storage::StorageError err);
  bool read_path_(const char *path);
  void issue_read_();
  void on_open_(storage::StorageError err);
  void on_read_(storage::StorageError err);
  void on_closed_(storage::StorageError err);
  void read_failed_(storage::StorageError err);
  bool grow_(size_t need);
  void release_buffer_();
#ifdef USE_CERT_STORE_LINK
  uint8_t next_source_(uint8_t from) const;
  bool fetch_remote_(uint8_t from);
  static void fetch_job_(void *ctx, uint16_t source);
#endif

  const char *directory_{nullptr};
#ifdef USE_CERT_STORE_LINK
  relay_link::RelayLink *link_{nullptr};
  bool relay_source_{false};
  const char *http_sources_[MAX_HTTP]{};
  uint8_t http_count_{0};
#endif

  bool applying_{false};
  uint8_t step_{STEP_DONE};
  bool from_directory_{false};
  mbedtls_ssl_config *conf_{nullptr};
  TlsMaterial *out_{nullptr};
  TlsRefs refs_{};
  Rng rng_{nullptr};
  void *rng_ctx_{nullptr};
  ApplyCallback done_;
  char path_[storage::STORAGE_VFS_PATH_MAX + 1]{};
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  uint8_t *buf_{nullptr};
  size_t buf_cap_{0};
  size_t buf_off_{0};
  size_t last_read_{0};
};

extern CertStore *global_cert_store;

}

#endif
