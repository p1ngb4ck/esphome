#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/storage/storage.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace ftp_client {

// A minimal FTP client exposed to ESPHome as a network storage device, analogous to nfs_client:
// it registers a mount point and serves the generic storage actions / file browser through the
// current storage interface (chunked read/write, stat, list_dir, ...). Passive mode only.
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

 protected:
  // Connection lifecycle
  bool ensure_connected_();
  storage::StorageError do_connect_();
  void do_disconnect_();
  int connect_tcp_(const std::string &host, uint16_t port);
  void control_close_();

  // FTP control protocol
  int send_cmd_(const std::string &cmd, std::string *reply_text = nullptr);  // returns 3-digit code, or -1
  int read_reply_(std::string *text);                                        // returns 3-digit code, or -1
  int open_pasv_data_();                                                     // returns data fd, or -1
  bool send_all_(int fd, const uint8_t *data, size_t len);

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

  int control_fd_{-1};
};

}  // namespace ftp_client
}  // namespace esphome

#endif  // USE_ESP32
