#pragma once
#include "esphome/core/defines.h"
// The store is built on the storage interface (ESP-IDF only) and validates certificates with
// mbedTLS, which ESP-IDF provides.
#ifdef USE_ESP_IDF

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/storage/storage.h"
// Reads run through the storage worker's async stream API (part of the storage interface). Codegen
// requests the worker, so this is always present for a configured cert_store.
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace esphome {
namespace cert_store {

// What a stored blob is, so consumers and validation can treat it correctly. The kind decides what
// (if anything) is validated and how the bytes are meant to be used.
enum class CertKind : uint8_t {
  CA,              // trust anchor(s), PEM/DER -- x509-validated
  CLIENT_CERT,     // own certificate for mutual TLS, PEM/DER -- x509-validated
  PRIVATE_KEY,     // private key for a client cert, PEM/DER -- kept raw (format varies)
  PSK,             // raw pre-shared/symmetric key bytes
  SSH_KNOWN_HOST,  // OpenSSH known_hosts / server pubkey, kept raw
  SSH_USER_KEY,    // OpenSSH/PEM user private key for pubkey auth, kept raw
  RAW,             // anything else -- just bytes
};

// Where an entry's bytes come from.
enum class CertSource : uint8_t {
  EMBEDDED,  // compile-time PEM literal in flash (.rodata) -- parsed in place, never held in RAM
  STORAGE,   // a file on a mounted storage, read on demand through the worker stream API
};

const char *cert_kind_to_string(CertKind kind);

// A central cert/key store built entirely on the storage interface. Nothing is cached: an entry is
// materialised only for the moment it is used, then released. There are three ways an entry's bytes
// reach mbedTLS, all handled transparently for the consumer:
//   - EMBEDDED: the PEM is a flash string literal; mbedTLS parses it straight out of .rodata.
//   - STORAGE:  the bytes are streamed on demand through the storage worker into one short-lived
//               RAMAllocator buffer that is freed the instant the parse finishes.
//   - the built-in esp_crt_bundle (the Mozilla roots ESP-IDF ships): the default trust anchor when
//     a CA is requested without naming an entry.
// It never re-implements any storage operation -- resolution, mounting and the read all belong to
// the storage component and its worker.
//
// Single instance: consumers reach it through global_cert_store (below), so they never name it.
class CertStore : public Component {
 public:
  // Fired when apply_ca_async() has finished (or failed): OK means the ssl config now trusts the
  // requested anchor and the handshake may proceed.
  using ApplyCallback = std::function<void(storage::StorageError)>;

  struct Entry {
    const char *id;
    CertKind kind;
    CertSource source;
    // EMBEDDED: NUL-terminated PEM literal in flash; len excludes the NUL. STORAGE: unused.
    const char *embedded_data;
    size_t embedded_len;
    // STORAGE: full VFS path; the registry resolves which storage it lives on. EMBEDDED: unused.
    const char *path;
  };

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Codegen emits one call per configured entry. All strings are literals in the generated code,
  // so the store never owns them.
  void add_embedded(const char *id, CertKind kind, const char *data, size_t len) {
    this->entries_.push_back(Entry{id, kind, CertSource::EMBEDDED, data, len, nullptr});
  }
  void add_storage(const char *id, CertKind kind, const char *path) {
    this->entries_.push_back(Entry{id, kind, CertSource::STORAGE, nullptr, 0, path});
  }

  const Entry *find(StringRef id) const;

  // Configure `conf` to trust the CA named by `id`, then invoke on_done. Everything is handled
  // here so no consumer re-implements it:
  //   - id empty or unknown  -> the built-in esp_crt_bundle (immediate)
  //   - EMBEDDED entry       -> parse from flash into `ca_out` (immediate)
  //   - STORAGE entry        -> stream through the worker into a short-lived buffer, parse into
  //                             `ca_out`, free the buffer; on_done fires from the stream
  //                             completion (never blocks the loop)
  // `ca_out` is owned by the consumer (its S3Connection etc.) and freed with the rest of its TLS
  // context; the cert_store keeps nothing. For the bundle path `ca_out` is left untouched.
  void apply_ca_async(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_out, StringRef id, ApplyCallback &&on_done);

 protected:
  // Parse a complete PEM/DER (NUL-terminated, len excludes the NUL) into ca_out and attach it to
  // conf as the trust chain. Shared by the EMBEDDED and STORAGE paths.
  static storage::StorageError attach_parsed_(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_out,
                                              const uint8_t *data, size_t len);
  // Attach the built-in Mozilla bundle to conf (the default trust anchor).
  static storage::StorageError attach_bundle_(mbedtls_ssl_config *conf);

#ifdef USE_STORAGE_WORKER
  // One STORAGE apply at a time: the worker stream is a single shared resource. A second request
  // while one is in flight is rejected NOT_READY (TLS setup is serialised per connection, so this
  // does not happen in practice).
  void issue_read_();
  void on_open_(storage::StorageError err);
  void on_read_(storage::StorageError err);
  void on_closed_(storage::StorageError err);
  void finish_storage_(storage::StorageError err);
  void release_buffer_();
#endif

  std::vector<Entry> entries_;

#ifdef USE_STORAGE_WORKER
  // In-flight STORAGE apply state. Buffer and stream exist only between apply_ca_async() and its
  // on_done; release_buffer_() returns the RAM the moment the parse (or a failure) is done.
  bool applying_{false};
  mbedtls_ssl_config *apply_conf_{nullptr};
  mbedtls_x509_crt *apply_ca_{nullptr};
  ApplyCallback apply_cb_;
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  RAMAllocator<uint8_t> alloc_{};
  uint8_t *buf_{nullptr};
  size_t buf_cap_{0};
  size_t buf_off_{0};
  size_t last_read_{0};
#endif
};

// The one and only cert_store, for consumers to reach without naming it (mirrors
// storage::global_storage_registry). nullptr when no cert_store is configured.
extern CertStore *global_cert_store;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
