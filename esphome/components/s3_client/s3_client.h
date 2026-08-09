#pragma once

// WARNING: This component is EXPERIMENTAL. The API may change at any time
// without following the normal breaking changes policy. Use at your own risk.

#include "esphome/core/defines.h"
#ifdef USE_ESP_IDF

#include "esphome/components/network/util.h"
#include "esphome/components/socket/socket.h"
#include "esphome/components/storage/storage.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_CERT_STORE
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#endif

#include <cstdint>
#include <string>

namespace esphome {
namespace s3_client {

// Response of one HTTP exchange: status plus the small subset of headers the client acts on.
// Bodies are streamed into caller storage (read_chunk) or a bounded accumulator (list/stat).
struct HttpResponse {
  int status{0};
  uint64_t content_length{0};
  bool has_content_length{false};
  bool chunked{false};
  uint32_t date_epoch{0};  // Date: header, parsed; 0 if absent/unparsable
  uint64_t last_modified_epoch{0};
};

// One TCP (optionally TLS) connection for exactly one request/response pair. S3 operations are
// stateless per request (Connection: close), which keeps the client free of keep-alive and
// session bookkeeping -- the price is one handshake per operation, documented in the component
// docs. All blocking I/O runs under the storage worker's serialization (task-safe).
class S3Connection {
 public:
  bool open(const char *host, uint16_t port, uint32_t timeout_ms);
#ifdef USE_CERT_STORE
  // TLS over the open socket; `ca_pem` must stay valid for the handshake (cert_store owns it).
  bool start_tls(const char *host, const char *ca_pem);
#endif
  // Fully sends `len` bytes; false on any socket/TLS error.
  bool send_all(const uint8_t *data, size_t len);
  // Reads up to `len` bytes; returns <0 on error, 0 on orderly close.
  int recv_some(uint8_t *buf, size_t len);
  void close();
  ~S3Connection() { this->close(); }

 private:
  std::unique_ptr<esphome::socket::Socket> sock_;
  uint32_t timeout_ms_{10000};
#ifdef USE_CERT_STORE
  bool tls_active_{false};
  mbedtls_ssl_context ssl_{};
  mbedtls_ssl_config conf_{};
  mbedtls_x509_crt cacert_{};
  mbedtls_ctr_drbg_context ctr_drbg_{};
  mbedtls_entropy_context entropy_{};
  static int tls_send_(void *ctx, const unsigned char *buf, size_t len);
  static int tls_recv_(void *ctx, unsigned char *buf, size_t len);
#endif
};

class S3Client final : public Component, public storage::NetworkStorage, public storage::MountableStorage {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_endpoint(const std::string &endpoint) { this->endpoint_ = endpoint; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_bucket(const std::string &bucket) { this->bucket_ = bucket; }
  void set_region(const std::string &region) { this->region_ = region; }
  void set_access_key(const std::string &k) { this->access_key_ = k; }
  void set_secret_key(const std::string &k) { this->secret_key_ = k; }
  void set_path_style(bool v) { this->path_style_ = v; }
  void set_auto_connect(bool v) { this->auto_connect_ = v; }
#ifdef USE_CERT_STORE
  void set_tls(bool v) { this->tls_ = v; }
  void set_ca_entry(const std::string &id) { this->ca_entry_ = id; }
#endif

  // Feeds the inherited PathStorage mount path -- resolve_path()/consumers read it from there.
  void set_mount_path(const char *mount_path) { this->set_mount_path_(mount_path); }
  bool is_mounted() const { return this->mounted_; }
  // No-RTTI downcast hook -- see PathStorage::as_mountable().
  storage::MountableStorage *as_mountable() override { return this; }

  // ---- MountableStorage ----
  uint8_t get_mount_caps() const override { return MOUNT_CAP_MOUNT | MOUNT_CAP_UNMOUNT; }
  storage::StorageError mount() override;
  storage::StorageError unmount() override;

  // ---- NetworkStorage / PathStorage ----
  storage::StorageError connect() override { return this->mount(); }
  storage::StorageError disconnect() override { return this->unmount(); }
  storage::StorageError get_info(storage::StorageInfo *info) override;
  uint8_t get_capabilities() const override { return storage::STORAGE_CAP_IO_TASK_SAFE; }
  storage::StorageError stat(const char *path, storage::FileStat *st) override;
  storage::StorageError list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                 void *ctx) override;
  storage::StorageError mkdir(const char *path) override;
  storage::StorageError rmdir(const char *path) override;
  storage::StorageError remove(const char *path) override;
  storage::StorageError rename(const char *old_path, const char *new_path) override;
  storage::StorageError read_chunk(const char *path, uint8_t *buf, uint64_t offset, size_t len,
                                   size_t *bytes_transferred) override;
  storage::StorageError write_chunk(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                    size_t *bytes_transferred) override;
  storage::StorageError truncate(const char *path, uint64_t size) override;

 protected:
  // ---- write episodes ----
  // S3 objects are immutable; there is no offset write. The storage interface delimits write
  // episodes for us: truncate(path, 0) always precedes the first chunk (worker copy, raw-to-file,
  // write streams and the copy() helper all do it), chunks then arrive strictly sequentially, and
  // the worker's serialization guarantees a single writer per path. The client therefore
  // accumulates one episode in RAM (PSRAM preferred) and uploads it as a single PUT when the
  // episode ends: on any other operation touching the key, on a new truncate, on unmount, or
  // after an idle timeout. A lone out-of-episode chunk at the current object size (append_file)
  // falls back to one GET + PUT.
  struct WriteEpisode {
    bool active{false};
    char key[storage::STORAGE_PATH_MAX]{};
    uint8_t *data{nullptr};
    size_t size{0};
    size_t cap{0};
    uint32_t last_ms{0};
  };
  static constexpr uint32_t EPISODE_IDLE_FLUSH_MS = 2000;

  storage::StorageError flush_episode_();
  void drop_episode_();
  bool episode_matches_(const char *key) const;
  bool episode_reserve_(size_t need);
  storage::StorageError flush_if_key_(const char *key);

  // ---- request plumbing ----
  // Performs one signed request. `body`/`body_len` is the payload (PUT); the response body is
  // either copied into `out` (up to out_cap, rest drained), streamed nowhere (out == nullptr),
  // or appended to `accum` when non-null (bounded by accum_limit). Exactly one of out/accum.
  storage::StorageError request_(const char *method, const std::string &key_enc, const std::string &query,
                                 const uint8_t *body, size_t body_len, const char *extra_header, uint8_t *out,
                                 size_t out_cap, size_t *out_len, std::string *accum, size_t accum_limit,
                                 HttpResponse *resp);
  storage::StorageError map_status_(int status) const;
  std::string host_() const;
  std::string uri_for_(const std::string &key_enc) const;
  static std::string uri_encode_(const char *s, bool keep_slash);
  std::string key_of_(const char *path) const;  // strips the leading '/'

  // ---- SigV4 ----
  void signing_key_(const char *yyyymmdd, uint8_t out[32]);
  std::string authorization_(const char *method, const std::string &canonical_uri, const std::string &query,
                             const std::string &host, const char *amz_date, const char *yyyymmdd);
  uint32_t now_epoch_() const;

  std::string endpoint_;
  uint16_t port_{443};
  std::string bucket_;
  std::string region_{"us-east-1"};
  std::string access_key_;
  std::string secret_key_;
  bool path_style_{true};
  bool auto_connect_{true};
#ifdef USE_CERT_STORE
  bool tls_{true};
  std::string ca_entry_;
#endif

  bool mounted_{false};
  bool was_connected_{false};  // rising-edge tracking for auto_connect (NFS pattern)
  int32_t clock_offset_{0};    // server Date minus local time(), learned at mount()
  char signing_day_[9]{};      // cached key derivation, one HMAC chain per UTC day
  uint8_t signing_key_cache_[32]{};
  WriteEpisode episode_{};
  RAMAllocator<uint8_t> episode_alloc_{};
};

}  // namespace s3_client
}  // namespace esphome

#endif  // USE_ESP_IDF
