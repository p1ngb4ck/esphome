#include "ftp_client.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <sys/time.h>
#include <cctype>
#include <cerrno>
#include <cstring>

namespace esphome {
namespace ftp_client {

static const char *const TAG = "ftp_client";

static constexpr uint32_t FTP_TIMEOUT_MS = 10000;
static constexpr uint32_t FTP_INLINE_MOUNT_MIN_INTERVAL_MS = 3000;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void set_socket_timeouts(int fd) {
  struct timeval tv;
  tv.tv_sec = FTP_TIMEOUT_MS / 1000;
  tv.tv_usec = (FTP_TIMEOUT_MS % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static const char *basename_of(const std::string &path) {
  size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

static void fill_name(storage::FileStat *out, const char *name) {
  strncpy(out->name, name, storage::STORAGE_NAME_MAX);
  out->name[storage::STORAGE_NAME_MAX] = '\0';
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void FTPClient::setup() {
  ESP_LOGCONFIG(TAG, "Setting up FTP client...");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->get_mount_path());

  // Register before the first connect, like nfs_client/sd_storage: the device is visible to
  // path routing and the mount/unmount actions even while unmounted (get_info() reports the
  // mounted state).
  if (storage::global_storage_registry != nullptr) {
    if (storage::global_storage_registry->register_storage(this) != storage::StorageError::OK) {
      ESP_LOGE(TAG, "Storage registration failed");
      this->mark_failed();
    }
  }
}

void FTPClient::loop() {
  // Auto-connect on each rising edge of network connectivity (wifi/eth/modem/openthread),
  // same event-edge pattern as nfs_client -- no periodic retry.
  const bool net_connected = network::is_connected();
  if (this->auto_connect_ && net_connected && !this->network_was_connected_ && !this->connected_) {
    this->mount_requested_ = true;
  }
  this->network_was_connected_ = net_connected;

  if (this->mount_requested_ && !this->connected_) {
    this->mount_requested_ = false;
    this->do_connect_();
  }
}

void FTPClient::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP client:");
  ESP_LOGCONFIG(TAG, "  Server: %s:%u", this->server_.c_str(), this->port_);
  ESP_LOGCONFIG(TAG, "  Username: %s", this->username_.c_str());
  // Password is a secret -- never logged, same as the wifi password.
  ESP_LOGCONFIG(TAG, "  Mount path: %s", this->get_mount_path());
  ESP_LOGCONFIG(TAG, "  Auto connect: %s", YESNO(this->auto_connect_));
  ESP_LOGCONFIG(TAG, "  Status: %s", this->connected_ ? "connected" : "disconnected");
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

bool FTPClient::ensure_connected_() {
  if (this->connected_)
    return true;
  // Rate-limited inline connect so a dead server does not turn every action into a fresh
  // stack of socket timeouts (mirrors nfs_client::ensure_mounted_).
  uint32_t now = millis();
  if (now - this->last_inline_mount_ms_ < FTP_INLINE_MOUNT_MIN_INTERVAL_MS)
    return false;
  this->last_inline_mount_ms_ = now;
  return this->do_connect_() == storage::StorageError::OK;
}

storage::StorageError FTPClient::do_connect_() {
  if (this->connected_)
    return storage::StorageError::OK;
  if (!network::is_connected())
    return storage::StorageError::NOT_READY;

  this->control_fd_ = this->connect_tcp_(this->server_, this->port_);
  if (this->control_fd_ < 0) {
    ESP_LOGW(TAG, "Control connection to %s:%u failed", this->server_.c_str(), this->port_);
    return storage::StorageError::NOT_READY;
  }

  // Greeting
  if (this->read_reply_(nullptr) != 220) {
    ESP_LOGW(TAG, "No 220 greeting");
    this->control_close_();
    return storage::StorageError::NOT_READY;
  }

  int c = this->send_cmd_("USER " + this->username_);
  if (c == 331) {
    c = this->send_cmd_("PASS " + this->password_);
  }
  if (c != 230) {
    ESP_LOGW(TAG, "Login failed (code %d)", c);
    this->control_close_();
    return storage::StorageError::PERMISSION_DENIED;
  }

  // Binary transfers.
  this->send_cmd_("TYPE I");

  this->connected_ = true;
  ESP_LOGI(TAG, "Connected to %s:%u as %s", this->server_.c_str(), this->port_, this->username_.c_str());
  return storage::StorageError::OK;
}

void FTPClient::do_disconnect_() {
  if (this->control_fd_ >= 0) {
    this->send_cmd_("QUIT");
    this->control_close_();
  }
  this->connected_ = false;
}

void FTPClient::control_close_() {
  if (this->control_fd_ >= 0) {
    ::close(this->control_fd_);
    this->control_fd_ = -1;
  }
}

int FTPClient::connect_tcp_(const std::string &host, uint16_t port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);

  struct addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS/resolve failed for %s", host.c_str());
    return -1;
  }

  int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(res);
    return -1;
  }
  set_socket_timeouts(fd);
  if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
    ::close(fd);
    freeaddrinfo(res);
    return -1;
  }
  freeaddrinfo(res);
  return fd;
}

// ---------------------------------------------------------------------------
// FTP control protocol
// ---------------------------------------------------------------------------

int FTPClient::read_reply_(std::string *text) {
  int code = -1;
  bool have_code = false;
  uint32_t start = millis();

  while (true) {
    std::string line;
    // Read one CRLF-terminated line.
    while (true) {
      char ch;
      int n = ::recv(this->control_fd_, &ch, 1, 0);
      if (n == 1) {
        if (ch == '\n')
          break;
        if (ch != '\r')
          line.push_back(ch);
        continue;
      }
      if (n == 0)
        return -1;  // peer closed
      if (millis() - start > FTP_TIMEOUT_MS)
        return -1;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        delay(1);
        continue;
      }
      return -1;
    }

    if (text != nullptr) {
      *text += line;
      text->push_back('\n');
    }

    if (line.size() >= 3 && isdigit((unsigned char) line[0]) && isdigit((unsigned char) line[1]) &&
        isdigit((unsigned char) line[2])) {
      int this_code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
      char sep = line.size() >= 4 ? line[3] : ' ';
      if (!have_code) {
        code = this_code;
        have_code = true;
        if (sep != '-')
          return code;  // single-line reply
        // else: multi-line, keep reading until the terminating "code " line
      } else if (this_code == code && sep == ' ') {
        return code;  // end of multi-line reply
      }
    }
    // Non-code or intermediate continuation line: keep going.
  }
}

int FTPClient::send_cmd_(const std::string &cmd, std::string *reply_text) {
  if (this->control_fd_ < 0)
    return -1;
  std::string line = cmd + "\r\n";
  if (!this->send_all_(this->control_fd_, reinterpret_cast<const uint8_t *>(line.data()), line.size()))
    return -1;
  return this->read_reply_(reply_text);
}

bool FTPClient::send_all_(int fd, const uint8_t *data, size_t len) {
  size_t sent = 0;
  uint32_t start = millis();
  while (sent < len) {
    int n = ::send(fd, data + sent, len - sent, 0);
    if (n > 0) {
      sent += (size_t) n;
      continue;
    }
    if (millis() - start > FTP_TIMEOUT_MS)
      return false;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      delay(1);
      continue;
    }
    return false;
  }
  return true;
}

int FTPClient::open_pasv_data_() {
  std::string text;
  if (this->send_cmd_("PASV", &text) != 227)
    return -1;

  // "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
  size_t open = text.find('(');
  size_t close = text.find(')', open);
  if (open == std::string::npos || close == std::string::npos)
    return -1;
  int v[6] = {0};
  if (sscanf(text.c_str() + open + 1, "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return -1;

  char ip[16];
  snprintf(ip, sizeof(ip), "%d.%d.%d.%d", v[0], v[1], v[2], v[3]);
  uint16_t port = (uint16_t) ((v[4] << 8) | v[5]);
  return this->connect_tcp_(ip, port);
}

std::string FTPClient::remote_path_(const char *vfs_path) const {
  std::string full(vfs_path == nullptr ? "" : vfs_path);
  const char *mp = this->get_mount_path();
  if (mp != nullptr) {
    size_t mp_len = strlen(mp);
    if (full.compare(0, mp_len, mp) == 0)
      full.erase(0, mp_len);
  }
  if (full.empty())
    full = "/";
  else if (full[0] != '/')
    full.insert(full.begin(), '/');
  return full;
}

// ---------------------------------------------------------------------------
// MountableStorage
// ---------------------------------------------------------------------------

storage::StorageError FTPClient::mount() { return this->do_connect_(); }

storage::StorageError FTPClient::unmount() {
  this->do_disconnect_();
  return storage::StorageError::OK;
}

// ---------------------------------------------------------------------------
// Storage / NetworkStorage
// ---------------------------------------------------------------------------

storage::StorageError FTPClient::get_info(storage::StorageInfo *info) {
  info->id = this->get_mount_path();
  info->name = this->server_.c_str();
  info->kind = "ftp";
  info->total_bytes = 0;  // FTP has no portable "df"
  info->free_bytes = 0;
  info->block_size = 512;
  info->is_mounted = this->connected_;
  info->is_removable = true;
  info->is_read_only = false;
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::read_chunk(const char *path, uint8_t *buf, uint64_t offset, size_t len,
                                            size_t *bytes_transferred) {
  *bytes_transferred = 0;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  int data_fd = this->open_pasv_data_();
  if (data_fd < 0)
    return storage::StorageError::READ_ERROR;

  if (offset > 0) {
    if (this->send_cmd_("REST " + std::to_string((unsigned long long) offset)) != 350) {
      ::close(data_fd);
      return storage::StorageError::READ_ERROR;
    }
  }

  int c = this->send_cmd_("RETR " + remote);
  if (c != 150 && c != 125) {
    ::close(data_fd);
    return c == 550 ? storage::StorageError::NOT_FOUND : storage::StorageError::READ_ERROR;
  }

  size_t total = 0;
  uint32_t start = millis();
  while (total < len) {
    int n = ::recv(data_fd, buf + total, len - total, 0);
    if (n > 0) {
      total += (size_t) n;
      continue;
    }
    if (n == 0)
      break;  // EOF
    if (millis() - start > FTP_TIMEOUT_MS)
      break;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      delay(1);
      continue;
    }
    break;
  }
  ::close(data_fd);
  this->read_reply_(nullptr);  // 226/426

  *bytes_transferred = total;  // partial (< len) means EOF, per the read contract
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::write_chunk(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                             size_t *bytes_transferred) {
  *bytes_transferred = 0;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  int data_fd = this->open_pasv_data_();
  if (data_fd < 0)
    return storage::StorageError::WRITE_ERROR;

  // Full-file only: the first chunk stores (truncating), later contiguous chunks append.
  const char *verb = offset == 0 ? "STOR " : "APPE ";
  int c = this->send_cmd_(std::string(verb) + remote);
  if (c != 150 && c != 125) {
    ::close(data_fd);
    return storage::StorageError::WRITE_ERROR;
  }

  bool ok = this->send_all_(data_fd, buf, len);
  ::close(data_fd);
  int rc = this->read_reply_(nullptr);
  if (!ok || (rc != 226 && rc != 250))
    return storage::StorageError::WRITE_ERROR;

  *bytes_transferred = len;
  return storage::StorageError::OK;
}

storage::StorageError FTPClient::truncate(const char *path, uint64_t size) {
  // Full-file write only: the copy path calls truncate(path, 0) before the first chunk to
  // create/empty the file, which STOR of the first chunk then overwrites. Any non-zero
  // truncation would need random-offset rewrites, which FTP here does not support.
  if (size != 0)
    return storage::StorageError::NOT_SUPPORTED;
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  int data_fd = this->open_pasv_data_();
  if (data_fd < 0)
    return storage::StorageError::WRITE_ERROR;
  int c = this->send_cmd_("STOR " + remote);
  if (c != 150 && c != 125) {
    ::close(data_fd);
    return storage::StorageError::WRITE_ERROR;
  }
  ::close(data_fd);  // close with nothing written -> empty file
  int rc = this->read_reply_(nullptr);
  return (rc == 226 || rc == 250) ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::stat(const char *path, storage::FileStat *out) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  *out = storage::FileStat{};
  fill_name(out, basename_of(remote));

  std::string text;
  int c = this->send_cmd_("SIZE " + remote, &text);
  if (c == 213) {
    out->is_dir = false;
    out->size = (uint64_t) strtoull(text.c_str() + 3, nullptr, 10);
    return storage::StorageError::OK;
  }
  // SIZE fails on directories on most servers -- probe with CWD (all our paths are absolute,
  // so a changed working directory does not affect later commands).
  if (this->send_cmd_("CWD " + remote) == 250) {
    out->is_dir = true;
    out->size = 0;
    return storage::StorageError::OK;
  }
  return storage::StorageError::NOT_FOUND;
}

storage::StorageError FTPClient::list_dir(const char *path,
                                          bool (*callback)(const storage::FileStat *entry, void *ctx), void *ctx) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  std::string remote = this->remote_path_(path);

  int data_fd = this->open_pasv_data_();
  if (data_fd < 0)
    return storage::StorageError::READ_ERROR;

  // Prefer MLSD (machine-readable). Fall back to LIST on servers that reject it.
  bool mlsd = true;
  int c = this->send_cmd_("MLSD " + remote);
  if (c != 150 && c != 125) {
    ::close(data_fd);
    mlsd = false;
    data_fd = this->open_pasv_data_();
    if (data_fd < 0)
      return storage::StorageError::READ_ERROR;
    c = this->send_cmd_("LIST " + remote);
    if (c != 150 && c != 125) {
      ::close(data_fd);
      return c == 550 ? storage::StorageError::NOT_FOUND : storage::StorageError::READ_ERROR;
    }
  }

  // Slurp the listing.
  std::string listing;
  char buf[512];
  uint32_t start = millis();
  while (true) {
    int n = ::recv(data_fd, buf, sizeof(buf), 0);
    if (n > 0) {
      listing.append(buf, (size_t) n);
      continue;
    }
    if (n == 0)
      break;
    if (millis() - start > FTP_TIMEOUT_MS)
      break;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      delay(1);
      continue;
    }
    break;
  }
  ::close(data_fd);
  this->read_reply_(nullptr);  // 226

  size_t pos = 0;
  while (pos < listing.size()) {
    size_t nl = listing.find('\n', pos);
    std::string line = listing.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? listing.size() : nl + 1;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;

    storage::FileStat e{};
    std::string name;
    if (mlsd) {
      // "type=file;size=123;modify=YYYYMMDDHHMMSS; name"
      size_t sp = line.find(' ');
      if (sp == std::string::npos)
        continue;
      std::string facts = line.substr(0, sp);
      name = line.substr(sp + 1);
      bool is_dir = false, skip = false;
      size_t fp = 0;
      while (fp < facts.size()) {
        size_t semi = facts.find(';', fp);
        std::string fact = facts.substr(fp, semi == std::string::npos ? std::string::npos : semi - fp);
        fp = semi == std::string::npos ? facts.size() : semi + 1;
        if (fact.compare(0, 5, "type=") == 0) {
          std::string t = fact.substr(5);
          if (t == "cdir" || t == "pdir")
            skip = true;  // "." / ".."
          is_dir = (t == "dir" || t == "cdir" || t == "pdir");
        } else if (fact.compare(0, 5, "size=") == 0) {
          e.size = (uint64_t) strtoull(fact.c_str() + 5, nullptr, 10);
        }
      }
      if (skip)
        continue;
      e.is_dir = is_dir;
    } else {
      // Basic "ls -l" parse: perms, links, owner, group, size, month, day, time/year, name...
      e.is_dir = line[0] == 'd';
      int field = 0;
      size_t i = 0;
      size_t name_start = std::string::npos;
      while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
          i++;
        size_t tok = i;
        while (i < line.size() && line[i] != ' ')
          i++;
        if (field == 4)
          e.size = (uint64_t) strtoull(line.c_str() + tok, nullptr, 10);
        field++;
        if (field == 8) {
          name_start = i < line.size() ? i + 1 : std::string::npos;
          break;
        }
      }
      if (name_start == std::string::npos || name_start >= line.size())
        continue;
      name = line.substr(name_start);
      if (name == "." || name == "..")
        continue;
    }

    fill_name(&e, basename_of(name));
    if (!callback(&e, ctx))
      break;
  }

  return storage::StorageError::OK;
}

storage::StorageError FTPClient::mkdir(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("MKD " + this->remote_path_(path));
  if (c == 257)
    return storage::StorageError::OK;
  return c == 550 ? storage::StorageError::ALREADY_EXISTS : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::rmdir(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("RMD " + this->remote_path_(path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

storage::StorageError FTPClient::remove(const char *path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  int c = this->send_cmd_("DELE " + this->remote_path_(path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::NOT_FOUND;
}

storage::StorageError FTPClient::rename(const char *old_path, const char *new_path) {
  if (!this->ensure_connected_())
    return storage::StorageError::NOT_READY;
  if (this->send_cmd_("RNFR " + this->remote_path_(old_path)) != 350)
    return storage::StorageError::NOT_FOUND;
  int c = this->send_cmd_("RNTO " + this->remote_path_(new_path));
  return c == 250 ? storage::StorageError::OK : storage::StorageError::WRITE_ERROR;
}

}  // namespace ftp_client
}  // namespace esphome

#endif  // USE_ESP32
