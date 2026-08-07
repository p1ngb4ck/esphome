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
// Reads run through the storage worker's async stream API (part of the storage interface). Codegen
// requests the worker, so this is always present for a configured cert_store.
#ifdef USE_STORAGE_WORKER
#include "esphome/components/storage/storage_worker.h"
#endif

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

// A central, storage-backed cert/key store. Every entry names a file by its full VFS path; the store
// loads it ONCE through the storage worker (the registry resolves which storage the path lives on --
// no manual storage selection), caches it in RAM NUL-terminated (so PEM consumers get a ready
// C-string), validates certificates with mbedTLS, and hands it out by id. It never re-implements any
// storage operation -- resolution, mounting and the read all belong to the storage component.
//
// Single instance: consumers reach it through global_cert_store (below), so they never name it.
class CertStore : public Component {
 public:
  struct Entry {
    const char *id;    // handle used by find()/consumers
    CertKind kind;
    const char *path;  // full VFS path; the registry resolves which storage it lives on
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
  void add_entry(const char *id, CertKind kind, const char *path) {
    this->entries_.push_back(Entry{id, kind, path});
  }

  // Handle lookup. Returns nullptr when the id is unknown or the entry is not loaded yet.
  const Entry *find(StringRef id) const;
  // NUL-terminated bytes of a loaded entry (or nullptr); *len_out gets the content length.
  const uint8_t *get(StringRef id, size_t *len_out = nullptr) const;
  // PEM/text convenience: the NUL-terminated bytes as a C-string (mbedTLS / esp-tls / libssh2 all
  // take const char *), or nullptr when the id is unknown or not loaded yet.
  const char *str(StringRef id) const {
    const Entry *entry = this->find(id);
    return entry != nullptr ? reinterpret_cast<const char *>(entry->data) : nullptr;
  }

  // True once every configured entry is loaded.
  bool ready() const { return this->ready_; }

 protected:
  // Marks ready_ + stops polling once every entry has landed.
  void finish_if_complete_();
  // mbedTLS x509 parse-check for CA/CLIENT_CERT; other kinds are accepted as-is.
  static bool validate_(const Entry &entry);

#ifdef USE_STORAGE_WORKER
  // One entry is read at a time through the worker's async stream API (begin_read/read_chunk/
  // end_read): the worker runs it on its task for task-safe media, loop-sliced otherwise, and never
  // touches the storage directly from here. Read runs until EOF into a growing buffer.
  void start_load_(size_t index);
  void issue_read_();
  void on_open_(storage::StorageError err);
  void on_read_(storage::StorageError err);
  void on_closed_(storage::StorageError err);
  void abandon_load_(const char *reason, storage::StorageError err);
#endif

  std::vector<Entry> entries_;
  bool ready_{false};
  uint32_t last_try_ms_{0};

#ifdef USE_STORAGE_WORKER
  static constexpr size_t NO_LOAD = SIZE_MAX;
  size_t loading_{NO_LOAD};
  storage::StreamHandle stream_{};
  bool stream_open_{false};
  uint8_t *pending_buf_{nullptr};  // grows (doubling) as chunks arrive; +1 slack for the NUL
  size_t pending_len_{0};          // allocated capacity of pending_buf_
  size_t pending_off_{0};          // bytes read so far
  size_t last_read_{0};            // read_chunk() writes the last chunk size here
#endif
};

// The one and only cert_store, for consumers to reach without naming it (mirrors
// storage::global_storage_registry). nullptr when no cert_store is configured.
extern CertStore *global_cert_store;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
