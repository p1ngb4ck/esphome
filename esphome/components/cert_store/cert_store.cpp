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

// How often an entry is retried while its storage is not mounted yet. Long enough that a card that
// never appears costs nothing; short enough that a card inserted by hand shows up without a reboot.
static constexpr uint32_t RETRY_INTERVAL_MS = 2000;

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
  // Nothing is read here: the files live on storages that may mount seconds after boot (or never),
  // so loop() drives the loads and retries.
  if (this->entries_.empty())
    this->ready_ = true;
}

void CertStore::loop() {
  if (this->ready_)
    return;
  const uint32_t now = millis();
  if (now - this->last_try_ms_ < RETRY_INTERVAL_MS)
    return;
  this->last_try_ms_ = now;

  bool all = true;
  for (auto &entry : this->entries_) {
    if (!entry.loaded && !this->load_entry_(entry))
      all = false;
  }
  if (all) {
    this->ready_ = true;
    ESP_LOGI(TAG, "all entries loaded");
    this->disable_loop();
  }
}

bool CertStore::load_entry_(Entry &entry) {
  storage::PathStorage *ps = entry.storage;
  const char *rel = entry.path;
  if (ps == nullptr) {
    if (storage::global_storage_registry == nullptr)
      return false;
    ps = storage::global_storage_registry->resolve_path(entry.path, &rel);
    if (ps == nullptr)
      return false;  // not mounted yet -- loop() comes back
  }

  // Certs and keys are small (KB), so the blocking whole-file helper is the right tool -- the async
  // worker path exists for bulk transfers, not for this. The read itself is the storage interface's
  // job; the store only decides where the bytes go afterwards.
  storage::RamBuffer buf;
  size_t size = 0;
  storage::StorageError err = storage::read_file(ps, rel, buf, &size);
  if (err != storage::StorageError::OK) {
    if (!entry.warned) {
      ESP_LOGW(TAG, "'%s': read of '%s' failed (%s)", entry.id, entry.path, storage::error_to_string(err));
      entry.warned = true;
    }
    return false;  // retry on the next interval
  }

  // Cache NUL-terminated: PEM consumers get a ready C-string, and mbedTLS PEM parsing needs the
  // terminator counted in the length. One allocation, held for the run like any embedded cert.
  RAMAllocator<uint8_t> allocator;
  uint8_t *store = allocator.allocate(size + 1);
  if (store == nullptr) {
    ESP_LOGE(TAG, "'%s': out of memory for %u bytes", entry.id, (unsigned) (size + 1));
    return false;
  }
  if (size > 0)
    std::memcpy(store, buf.get(), size);
  store[size] = 0;

  entry.data = store;
  entry.len = size;
  entry.loaded = true;
  entry.valid = validate_(entry);
  ESP_LOGD(TAG, "'%s' (%s): %u bytes from '%s'%s", entry.id, cert_kind_to_string(entry.kind),
           (unsigned) entry.len, entry.path, entry.valid ? "" : " -- INVALID");
  if (!entry.valid)
    ESP_LOGW(TAG, "'%s': does not parse as a valid certificate", entry.id);
  return true;
}

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
                  entry.loaded ? "loaded" : "pending",
                  entry.loaded && !entry.valid ? ", INVALID" : "");
  }
}

}  // namespace cert_store
}  // namespace esphome

#endif  // USE_ESP_IDF
