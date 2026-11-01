#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_TCP)

#include "esphome/components/storage/storage.h"
#include "esphome/components/socket/socket.h"
#ifdef USE_FTP_CLIENT_TLS
#include "esphome/components/cert_store/cert_store.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace esphome {
namespace ftp {

// A control/data connection: an ESPHome socket, optionally wrapped in TLS. read()/write() go raw
// or through mbedTLS depending on start_tls(), so the FTP protocol code above is oblivious. The
// mbedTLS bits only exist on ESP-IDF; plain FTP works on every socket platform.
class FtpStream {
 public:
  FtpStream() = default;
  ~FtpStream() { this->close(); }
  FtpStream(const FtpStream &) = delete;
  FtpStream &operator=(const FtpStream &) = delete;

  void set_socket(std::unique_ptr<socket::Socket> sock) {
    this->close();
    this->sock_ = std::move(sock);
  }
  bool valid() const { return this->sock_ != nullptr; }

  ssize_t read(void *buf, size_t len);
  ssize_t write(const void *buf, size_t len);
  void close();

#ifdef USE_FTP_CLIENT_TLS
  // Hands the current socket to mbedTLS and runs the handshake using the caller-owned config.
  // hostname is used for SNI and (when the config verifies) certificate hostname checking.
  bool start_tls(mbedtls_ssl_config *conf, const char *hostname);
  bool is_tls() const { return this->tls_; }
#endif

 protected:
  std::unique_ptr<socket::Socket> sock_;
#ifdef USE_FTP_CLIENT_TLS
  bool tls_{false};
  mbedtls_ssl_context ssl_{};
  static int bio_send_(void *ctx, const unsigned char *buf, size_t len);
  static int bio_recv_(void *ctx, unsigned char *buf, size_t len);
#endif
};


// A minimal FTP client exposed to ESPHome as a network storage device, analogous to nfs_client:
// it registers a mount point and serves the generic storage actions / file browser through the
// current storage interface (chunked read/write, stat, list_dir, ...). Passive mode only.
//
// Sockets go through ESPHome's socket abstraction (esphome::socket::Socket), same as api/ota;
// only hostname resolution still uses getaddrinfo (the abstraction has no resolver).
//
// Scope of this (quick-test) implementation:
//   - reads are chunked via REST + RETR (a fresh passive data connection per chunk).
//   - writes are FULL-FILE only: offset 0 -> STOR, subsequent contiguous chunks -> APPE.
//     random-offset writes are not supported and return NOT_SUPPORTED.
class FTPClient final : public storage::NetworkStorage, public storage::MountableStorage {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ---- configuration (username/password arrive via !secret, same as the wifi password) ----
  void set_server(const char *server) { this->server_ = server; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_username(const char *username) { this->username_ = username; }
  void set_password(const char *password) { this->password_ = password; }
  // Feeds the inherited PathStorage mount path (resolve_path()/consumers read it from there).
  void set_mount_path(const char *mount_path) { this->set_mount_path_(mount_path); }
  void set_auto_connect(bool auto_connect) { this->auto_connect_ = auto_connect; }
#ifdef USE_FTP_TFTP_CLIENT
  void set_tftp(bool tftp) { this->tftp_ = tftp; }
#endif
#ifdef USE_FTP_CLIENT_TLS
  // FTPS (AUTH TLS). Off by default. The CA is looked up in the single cert_store
  // (cert_store::global_cert_store) by entry id and is required when auth_tls is on -- the server
  // is always verified against it.
  void set_auth_tls(bool auth_tls) { this->auth_tls_ = auth_tls; }
  void set_tls_refs(const cert_store::TlsRefs &refs) { this->tls_refs_ = refs; }
#endif

  bool is_mounted() const { return this->connected_; }

  // FTP I/O is blocking sockets with no shared mutable state between calls, so it is safe to
  // run on the async transfer worker task.
  uint8_t get_capabilities() const override { return storage::STORAGE_CAP_IO_TASK_SAFE; }

  // ---- MountableStorage ----
  storage::MountableStorage *as_mountable() override { return this; }
  storage::StorageError mount() override;
  storage::StorageError unmount() override;

  // ---- Storage / PathStorage / NetworkStorage ----
  storage::StorageError get_info(storage::StorageInfo *info) override;
  storage::StorageError connect() override { return this->mount(); }
  storage::StorageError disconnect() override { return this->unmount(); }
  storage::StorageError read_chunk(const char *path, uint8_t *buf, uint64_t offset, size_t len,
                                   size_t *bytes_transferred) override;
  storage::StorageError write_chunk(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                    size_t *bytes_transferred) override;
  storage::StorageError truncate(const char *path, uint64_t size) override;
  storage::StorageError stat(const char *path, storage::FileStat *out) override;
  storage::StorageError list_dir(const char *path, bool (*callback)(const storage::FileStat *entry, void *ctx),
                                 void *ctx) override;
  storage::StorageError mkdir(const char *path) override;
  storage::StorageError rmdir(const char *path) override;
  storage::StorageError remove(const char *path) override;
  storage::StorageError rename(const char *old_path, const char *new_path) override;
  storage::StorageError write_done(const char *path, uint64_t size) override;

 protected:
  // Connection lifecycle
  bool ensure_connected_();
  storage::StorageError do_connect_();
  void do_disconnect_();
  // Resolves host (getaddrinfo) and returns a connected, blocking socket, or nullptr.
  std::unique_ptr<socket::Socket> open_tcp_(const std::string &host, uint16_t port);
  void control_close_();

  // FTP control protocol (operates on control_)
  int send_cmd_(const std::string &cmd, std::string *reply_text = nullptr);  // returns 3-digit code, or -1
  int read_reply_(std::string *text);                                        // returns 3-digit code, or -1
  std::unique_ptr<FtpStream> open_pasv_data_();                             // PASV + connect (+TLS), or nullptr
  bool send_all_(FtpStream *stream, const uint8_t *data, size_t len);

  // Maps a full VFS path (mount point + rest) to a server-side path.
  std::string remote_path_(const char *vfs_path) const;

  std::string server_;
  uint16_t port_{21};
  std::string username_;
  std::string password_;
  bool auto_connect_{true};

  bool connected_{false};
  bool network_was_connected_{false};
  bool mount_requested_{false};
  uint32_t last_inline_mount_ms_{0};

  std::unique_ptr<FtpStream> control_;

#ifdef USE_FTP_TFTP_CLIENT
  static constexpr size_t TFTP_BLOCK = 512;
  struct TftpSession {
    int fd{-1};
    bool write{false};
    bool last{false};
    uint16_t block{0};
    uint64_t pos{0};
    size_t have{0};
    size_t used{0};
    struct sockaddr_storage peer {};
    socklen_t peer_len{0};
    char path[256]{};
    uint8_t pkt[4 + TFTP_BLOCK]{};
  };
  storage::StorageError tftp_mount_();
  void tftp_close_();
  int tftp_open_socket_();
  bool tftp_send_request_(int fd, uint16_t op, const std::string &path, bool tsize);
  int tftp_recv_(int fd, uint8_t *buf, size_t cap, struct sockaddr_storage *from, socklen_t *from_len);
  storage::StorageError tftp_start_read_(const std::string &path, uint64_t *size);
  storage::StorageError tftp_next_block_();
  storage::StorageError tftp_send_block_(size_t len);
  storage::StorageError tftp_read_(const char *path, uint8_t *buf, uint64_t offset, size_t len, size_t *done);
  storage::StorageError tftp_write_(const char *path, const uint8_t *buf, uint64_t offset, size_t len, size_t *done);
  storage::StorageError tftp_stat_(const char *path, storage::FileStat *out);
  storage::StorageError tftp_finish_(const char *path, uint64_t size);
  bool tftp_{false};
  struct sockaddr_storage tftp_addr_ {};
  socklen_t tftp_addr_len_{0};
  TftpSession *tftp_session_{nullptr};
#endif

#ifdef USE_FTP_CLIENT_TLS
  // FTPS. The mbedTLS config is built once (setup_tls_) and shared by the control connection and
  // every data connection; the CA comes from the single cert_store by entry id.
  bool setup_tls_();
  bool auth_tls_{false};
  cert_store::TlsRefs tls_refs_{};
  bool tls_ready_{false};
  mbedtls_ssl_config conf_{};
  cert_store::TlsMaterial tls_material_{};
  mbedtls_ctr_drbg_context drbg_{};
  mbedtls_entropy_context entropy_{};
#endif
};

}  // namespace ftp
}  // namespace esphome

#endif  // socket impl available
