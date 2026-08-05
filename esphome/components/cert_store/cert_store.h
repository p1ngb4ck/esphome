#pragma once
#include "esphome/core/defines.h"
// The store is built on the storage interface (ESP-IDF only) and validates certificates with
// mbedTLS, which ESP-IDF provides.
#ifdef USE_ESP_IDF

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/storage/storage.h"

namespace esphome {
namespace cert_store {

// What a stored blob is, so consumers and validation can treat it correctly. The bytes are always
// available raw; the kind only decides what (if anything) is validated and how it is meant to be
// used.
enum class CertKind : uint8_t {
  CA,              // trust anchor(s), PEM/DER -- x509-validated
  CLIENT_CERT,     // own certificate for mutual TLS, PEM/DER -- x509-validated
  PRIVATE_KEY,     // private key for a client cert, PEM/DER -- kept raw (format varies)
  PSK,             // raw pre-shared/symmetric key bytes
  SSH_KNOWN_HOST,  // OpenSSH known_hosts / server pubkey, kept raw
  SSH_USER_KEY,    // OpenSSH/PEM user private key for pubkey auth, kept raw
  RAW,             // anything else -- just bytes
};

const char *cert_kind_to_string(CertKind kind);

// A central, storage-backed cert/key store. Every entry names a file on a PathStorage; the store
// loads it ONCE through the storage interface (storage::read_file), caches it in RAM NUL-terminated
// (so PEM consumers get a ready C-string), validates certificates with mbedTLS, and hands it out by
// id. It never re-implements any storage operation -- mounting, path resolution and file reading
// all belong to the storage component.
class CertStore : public Component {
 public:
  struct Entry {
    const char *id;                          // handle used by find()/consumers
    CertKind kind;
    const char *path;                        // relative to `storage` if set, else a full VFS path
    storage::PathStorage *storage{nullptr};  // explicit storage, or nullptr -> resolve via registry
    // Runtime state, filled once the file is read:
    uint8_t *data{nullptr};  // NUL-terminated cache (data[len] == 0); owned for the run
    size_t len{0};           // content length, excluding the terminating NUL
    bool loaded{false};
    bool valid{false};   // x509-parsed OK for CA/CLIENT_CERT; true for kinds that are not validated
    bool warned{false};  // so a missing file does not log every retry
  };

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // Codegen emits one call per configured entry. The id/path are string literals in the generated
  // code, so the store never owns them.
  void add_entry(const char *id, CertKind kind, const char *path, storage::PathStorage *storage) {
    this->entries_.push_back(Entry{id, kind, path, storage});
  }

  // Handle lookup. Returns nullptr when the id is unknown or the entry is not loaded yet.
  const Entry *find(StringRef id) const;
  // Convenience: NUL-terminated bytes of a loaded entry (or nullptr); *len_out gets the content
  // length when provided.
  const uint8_t *get(StringRef id, size_t *len_out = nullptr) const;

  // True once every configured entry is loaded.
  bool ready() const { return this->ready_; }

 protected:
  // Reads one entry through the storage interface and caches + validates it. Returns false when the
  // storage is not mounted yet or the read failed, so loop() retries.
  bool load_entry_(Entry &entry);
  // mbedTLS x509 parse-check for CA/CLIENT_CERT; other kinds are accepted as-is.
  static bool validate_(const Entry &entry);

  std::vector<Entry> entries_;
  bool ready_{false};
  uint32_t last_try_ms_{0};
};

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
