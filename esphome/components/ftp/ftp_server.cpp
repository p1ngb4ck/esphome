#include "ftp_server.h"

#ifdef USE_FTP_SERVER

#include "esphome/core/log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <strings.h>

namespace esphome::ftp {

static const char *const TAG = "ftp.server";
static constexpr uint32_t TASK_STACK = 6144;
static constexpr int DATA_TIMEOUT_S = 10;

FtpServer *FtpServer::create() {
  void *mem = RAMAllocator<FtpServer>(RAMAllocator<FtpServer>::ALLOC_EXTERNAL).allocate(1);
  return mem != nullptr ? new (mem) FtpServer() : new FtpServer();
}

void FtpServer::setup() {
  void *mem = RAMAllocator<Session>(RAMAllocator<Session>::ALLOC_EXTERNAL).allocate(this->max_sessions_);
  if (mem == nullptr) {
    this->mark_failed();
    return;
  }
  this->sessions_ = static_cast<Session *>(mem);
  for (uint8_t i = 0; i < this->max_sessions_; i++)
    new (&this->sessions_[i]) Session();
  if (!this->task_.create(&FtpServer::task_entry_, "ftp_server", TASK_STACK, this, 3, true, 1)) {
    ESP_LOGE(TAG, "No task");
    this->mark_failed();
  }
}

void FtpServer::dump_config() {
  ESP_LOGCONFIG(TAG, "FTP server:\n  Port: %u\n  Root: %s\n  Users: %u\n  Sessions: %u", this->port_, this->root_,
                this->user_count_, this->max_sessions_);
#ifdef USE_FTP_TFTP_SERVER
  ESP_LOGCONFIG(TAG, "  TFTP port: %u (%s)", this->tftp_port_, this->tftp_write_ ? "read / write" : "read only");
#endif
}

void FtpServer::task_entry_(void *arg) { static_cast<FtpServer *>(arg)->run_(); }

bool FtpServer::open_listener_(int *fd, uint16_t port, int type) {
  const int s = ::socket(AF_INET, type, 0);
  if (s < 0)
    return false;
  int one = 1;
  ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
      (type == SOCK_STREAM && ::listen(s, 2) != 0)) {
    ::close(s);
    return false;
  }
  *fd = s;
  return true;
}

void FtpServer::run_() {
  if (!this->open_listener_(&this->listen_fd_, this->port_, SOCK_STREAM)) {
    ESP_LOGE(TAG, "Port %u not opened", this->port_);
    vTaskSuspend(nullptr);
  }
#ifdef USE_FTP_TFTP_SERVER
  if (!this->open_listener_(&this->tftp_fd_, this->tftp_port_, SOCK_DGRAM))
    ESP_LOGE(TAG, "TFTP port %u not opened", this->tftp_port_);
#endif
  ESP_LOGI(TAG, "Listening on %u", this->port_);
  while (true) {
    fd_set rd;
    FD_ZERO(&rd);
    int top = this->listen_fd_;
    FD_SET(this->listen_fd_, &rd);
#ifdef USE_FTP_TFTP_SERVER
    if (this->tftp_fd_ >= 0) {
      FD_SET(this->tftp_fd_, &rd);
      top = std::max(top, this->tftp_fd_);
    }
#endif
    for (uint8_t i = 0; i < this->max_sessions_; i++) {
      if (this->sessions_[i].ctrl >= 0) {
        FD_SET(this->sessions_[i].ctrl, &rd);
        top = std::max(top, this->sessions_[i].ctrl);
      }
    }
    if (::select(top + 1, &rd, nullptr, nullptr, nullptr) <= 0)
      continue;
    if (FD_ISSET(this->listen_fd_, &rd))
      this->accept_();
#ifdef USE_FTP_TFTP_SERVER
    if (this->tftp_fd_ >= 0 && FD_ISSET(this->tftp_fd_, &rd))
      this->tftp_request_();
#endif
    for (uint8_t i = 0; i < this->max_sessions_; i++) {
      Session &s = this->sessions_[i];
      if (s.ctrl >= 0 && FD_ISSET(s.ctrl, &rd))
        this->input_(s);
    }
  }
}

void FtpServer::accept_() {
  const int fd = ::accept(this->listen_fd_, nullptr, nullptr);
  if (fd < 0)
    return;
  for (uint8_t i = 0; i < this->max_sessions_; i++) {
    Session &s = this->sessions_[i];
    if (s.ctrl < 0) {
      s = Session();
      s.ctrl = fd;
      strcpy(s.cwd, "/");
      this->reply_(s, "220 ESPHome FTP");
      return;
    }
  }
  static const char BUSY[] = "421 Too many users\r\n";
  ::send(fd, BUSY, sizeof(BUSY) - 1, 0);
  ::close(fd);
}

void FtpServer::close_session_(Session &s) {
  if (s.pasv >= 0)
    ::close(s.pasv);
  if (s.ctrl >= 0)
    ::close(s.ctrl);
  s = Session();
}

void FtpServer::reply_(Session &s, const char *text) {
  if (s.ctrl < 0)
    return;
  ::send(s.ctrl, text, strlen(text), 0);
  ::send(s.ctrl, "\r\n", 2, 0);
}

void FtpServer::input_(Session &s) {
  const ssize_t n = ::recv(s.ctrl, s.line + s.line_len, sizeof(s.line) - 1 - s.line_len, 0);
  if (n <= 0) {
    this->close_session_(s);
    return;
  }
  s.line_len += static_cast<size_t>(n);
  s.line[s.line_len] = '\0';
  char *end;
  while ((end = strstr(s.line, "\r\n")) != nullptr || (end = strchr(s.line, '\n')) != nullptr) {
    const size_t skip = end[0] == '\r' ? 2 : 1;
    *end = '\0';
    char command[LINE_MAX_LEN];
    strncpy(command, s.line, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    const size_t used = static_cast<size_t>(end - s.line) + skip;
    memmove(s.line, s.line + used, s.line_len - used + 1);
    s.line_len -= used;
    this->command_(s, command);
    if (s.ctrl < 0)
      return;
  }
  if (s.line_len >= sizeof(s.line) - 1) {
    this->reply_(s, "500 Line too long");
    s.line_len = 0;
  }
}

bool FtpServer::path_of_(const Session &s, const char *arg, char *out) const {
  char joined[PATH_MAX_LEN * 2];
  if (arg == nullptr || arg[0] == '\0') {
    snprintf(joined, sizeof(joined), "%s", s.cwd);
  } else if (arg[0] == '/') {
    snprintf(joined, sizeof(joined), "%s", arg);
  } else {
    snprintf(joined, sizeof(joined), "%s/%s", s.cwd, arg);
  }
  char norm[PATH_MAX_LEN] = "";
  size_t len = 0;
  char *save = nullptr;
  for (char *part = strtok_r(joined, "/", &save); part != nullptr; part = strtok_r(nullptr, "/", &save)) {
    if (strcmp(part, ".") == 0)
      continue;
    if (strcmp(part, "..") == 0) {
      char *slash = strrchr(norm, '/');
      len = slash != nullptr ? static_cast<size_t>(slash - norm) : 0;
      norm[len] = '\0';
      continue;
    }
    const int w = snprintf(norm + len, sizeof(norm) - len, "/%s", part);
    if (w < 0 || static_cast<size_t>(w) >= sizeof(norm) - len)
      return false;
    len += static_cast<size_t>(w);
  }
  const int w = snprintf(out, PATH_MAX_LEN, "%s%s", strcmp(this->root_, "/") == 0 ? "" : this->root_,
                         norm[0] != '\0' ? norm : "/");
  return w > 0 && w < static_cast<int>(PATH_MAX_LEN);
}

storage::PathStorage *FtpServer::resolve_(const char *full, const char **rel) const {
  if (storage::global_storage_registry == nullptr)
    return nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(full, rel);
  if (ps != nullptr && (*rel == nullptr || (*rel)[0] == '\0'))
    *rel = "/";
  return ps;
}

void FtpServer::on_loop_(std::function<void()> &&op) {
  this->defer([this, op = std::move(op)]() {
    op();
    xTaskNotifyGive(this->task_.get_handle());
  });
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

storage::StorageError FtpServer::await_(
    std::function<storage::StorageError(storage::CompletionCallback &&)> &&start) {
  this->defer([this, start = std::move(start)]() {
    const storage::StorageError err = start([this](storage::StorageError result) {
      this->cb_err_ = result;
      xTaskNotifyGive(this->task_.get_handle());
    });
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      this->cb_err_ = err;
      xTaskNotifyGive(this->task_.get_handle());
    }
  });
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return this->cb_err_;
}

storage::StorageError FtpServer::stat_(const char *full, storage::FileStat *st) {
  storage::StorageError err = storage::StorageError::STORAGE_ERROR_NOT_FOUND;
  this->on_loop_([this, full, st, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(full, &rel);
    if (ps == nullptr)
      return;
    if (strcmp(rel, "/") == 0) {
      std::memset(st, 0, sizeof(*st));
      st->is_dir = true;
      err = storage::StorageError::STORAGE_ERROR_OK;
      return;
    }
    err = ps->stat(rel, st);
  });
  return err;
}

uint8_t *FtpServer::take_buffer_() {
  if (this->buf_ == nullptr)
    this->buf_ = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(CHUNK);
  return this->buf_;
}

void FtpServer::give_buffer_() {
  if (this->buf_ != nullptr)
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(this->buf_, CHUNK);
  this->buf_ = nullptr;
}

void FtpServer::pasv_(Session &s, bool extended) {
  if (s.pasv >= 0) {
    ::close(s.pasv);
    s.pasv = -1;
  }
  if (!this->open_listener_(&s.pasv, 0, SOCK_STREAM)) {
    this->reply_(s, "425 No data port");
    return;
  }
  struct sockaddr_in local {};
  socklen_t len = sizeof(local);
  ::getsockname(s.pasv, reinterpret_cast<sockaddr *>(&local), &len);
  const uint16_t port = ntohs(local.sin_port);
  char text[80];
  if (extended) {
    snprintf(text, sizeof(text), "229 Entering Extended Passive Mode (|||%u|)", port);
  } else {
    struct sockaddr_in ctrl {};
    len = sizeof(ctrl);
    ::getsockname(s.ctrl, reinterpret_cast<sockaddr *>(&ctrl), &len);
    const uint32_t ip = ntohl(ctrl.sin_addr.s_addr);
    snprintf(text, sizeof(text), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", (ip >> 24) & 0xFF,
             (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port >> 8, port & 0xFF);
  }
  this->reply_(s, text);
}

int FtpServer::data_accept_(Session &s) {
  if (s.pasv < 0) {
    this->reply_(s, "425 Use PASV first");
    return -1;
  }
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(s.pasv, &rd);
  struct timeval tv {};
  tv.tv_sec = DATA_TIMEOUT_S;
  int fd = -1;
  if (::select(s.pasv + 1, &rd, nullptr, nullptr, &tv) > 0)
    fd = ::accept(s.pasv, nullptr, nullptr);
  ::close(s.pasv);
  s.pasv = -1;
  if (fd < 0) {
    this->reply_(s, "425 No data connection");
    return -1;
  }
  struct timeval io {};
  io.tv_sec = DATA_TIMEOUT_S;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io, sizeof(io));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io, sizeof(io));
  return fd;
}

static bool send_all(int fd, const void *data, size_t len) {
  const auto *p = static_cast<const uint8_t *>(data);
  while (len != 0) {
    const ssize_t n = ::send(fd, p, len, 0);
    if (n <= 0)
      return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

void FtpServer::list_(Session &s, const char *arg, uint8_t style) {
  while (arg != nullptr && arg[0] == '-') {
    arg = strchr(arg, ' ');
    if (arg != nullptr)
      arg++;
  }
  char full[PATH_MAX_LEN];
  if (!this->path_of_(s, arg, full)) {
    this->reply_(s, "501 Bad path");
    return;
  }
  void *mem = RAMAllocator<Entries>(RAMAllocator<Entries>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr) {
    this->reply_(s, "451 No memory");
    return;
  }
  auto *entries = new (mem) Entries(RAMAllocator<storage::FileStat>(RAMAllocator<storage::FileStat>::ALLOC_EXTERNAL));
  storage::StorageError err = storage::StorageError::STORAGE_ERROR_NOT_FOUND;
  this->on_loop_([this, &full, entries, &err]() {
    const char *rel = nullptr;
    storage::PathStorage *ps = this->resolve_(full, &rel);
    if (ps == nullptr)
      return;
    err = ps->list_dir(
        rel,
        [](const storage::FileStat *entry, void *ctx) -> bool {
          auto *list = static_cast<Entries *>(ctx);
          list->push_back(*entry);
          return list->size() < LIST_MAX;
        },
        entries);
  });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    entries->~Entries();
    RAMAllocator<Entries>(RAMAllocator<Entries>::ALLOC_EXTERNAL).deallocate(entries, 1);
    this->reply_(s, "550 Not a directory");
    return;
  }
  this->reply_(s, "150 Listing");
  const int fd = this->data_accept_(s);
  bool ok = fd >= 0;
  for (const auto &e : *entries) {
    if (!ok)
      break;
    char line[storage::STORAGE_NAME_MAX + 96];
    struct tm t {};
    const time_t mtime = static_cast<time_t>(e.mtime);
    gmtime_r(&mtime, &t);
    int n;
    if (style == 0) {
      char when[16];
      strftime(when, sizeof(when), "%b %d %H:%M", &t);
      n = snprintf(line, sizeof(line), "%s 1 owner group %llu %s %s\r\n", e.is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                   static_cast<unsigned long long>(e.size), when, e.name);
    } else if (style == 1) {
      n = snprintf(line, sizeof(line), "%s\r\n", e.name);
    } else {
      char when[16];
      strftime(when, sizeof(when), "%Y%m%d%H%M%S", &t);
      n = snprintf(line, sizeof(line), "type=%s;size=%llu;modify=%s; %s\r\n", e.is_dir ? "dir" : "file",
                   static_cast<unsigned long long>(e.size), when, e.name);
    }
    ok = n > 0 && send_all(fd, line, std::min<size_t>(static_cast<size_t>(n), sizeof(line) - 1));
  }
  if (fd >= 0)
    ::close(fd);
  entries->~Entries();
  RAMAllocator<Entries>(RAMAllocator<Entries>::ALLOC_EXTERNAL).deallocate(entries, 1);
  if (fd >= 0)
    this->reply_(s, ok ? "226 Done" : "426 Transfer aborted");
}

void FtpServer::retr_(Session &s, const char *arg) {
  char full[PATH_MAX_LEN];
  const char *rel = nullptr;
  storage::PathStorage *ps = nullptr;
  if (!this->path_of_(s, arg, full) || (ps = this->resolve_(full, &rel)) == nullptr) {
    this->reply_(s, "550 Not found");
    return;
  }
  uint8_t *buf = this->take_buffer_();
  if (buf == nullptr) {
    this->reply_(s, "451 No memory");
    return;
  }
  storage::StreamHandle handle{};
  storage::StorageError err = this->await_([ps, rel, &handle](storage::CompletionCallback &&done) {
    return storage::global_storage_worker->begin_read(ps, rel, &handle, std::move(done));
  });
  if (err == storage::StorageError::STORAGE_ERROR_OK && s.rest != 0) {
    const int64_t at = static_cast<int64_t>(s.rest);
    err = this->await_([&handle, at](storage::CompletionCallback &&done) {
      return storage::global_storage_worker->seek(handle, at, storage::SeekMode::SEEK_MODE_SET, std::move(done));
    });
  }
  s.rest = 0;
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->await_([&handle](storage::CompletionCallback &&done) {
      return storage::global_storage_worker->end_read(handle, std::move(done));
    });
    this->give_buffer_();
    this->reply_(s, "550 Not readable");
    return;
  }
  this->reply_(s, "150 Sending");
  const int fd = this->data_accept_(s);
  bool ok = fd >= 0;
  while (ok) {
    size_t got = 0;
    err = this->await_([&handle, buf, &got](storage::CompletionCallback &&done) {
      return storage::global_storage_worker->read_chunk(handle, buf, CHUNK, &got, std::move(done));
    });
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      ok = false;
      break;
    }
    if (got == 0)
      break;
    ok = send_all(fd, buf, got);
  }
  this->await_([&handle](storage::CompletionCallback &&done) {
    return storage::global_storage_worker->end_read(handle, std::move(done));
  });
  this->give_buffer_();
  if (fd >= 0) {
    ::close(fd);
    this->reply_(s, ok ? "226 Done" : "426 Transfer aborted");
  }
}

void FtpServer::stor_(Session &s, const char *arg) {
  char full[PATH_MAX_LEN];
  const char *rel = nullptr;
  storage::PathStorage *ps = nullptr;
  if (!this->may_write_(s)) {
    this->reply_(s, "550 Read only");
    return;
  }
  if (!this->path_of_(s, arg, full) || (ps = this->resolve_(full, &rel)) == nullptr) {
    this->reply_(s, "553 Bad path");
    return;
  }
  uint8_t *buf = this->take_buffer_();
  if (buf == nullptr) {
    this->reply_(s, "451 No memory");
    return;
  }
  storage::StreamHandle handle{};
  storage::StorageError err = this->await_([ps, rel, &handle](storage::CompletionCallback &&done) {
    return storage::global_storage_worker->begin_write(ps, rel, &handle, std::move(done));
  });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->give_buffer_();
    this->reply_(s, "553 Not writable");
    return;
  }
  this->reply_(s, "150 Receiving");
  const int fd = this->data_accept_(s);
  bool ok = fd >= 0;
  while (ok) {
    const ssize_t n = ::recv(fd, buf, CHUNK, 0);
    if (n == 0)
      break;
    if (n < 0) {
      ok = false;
      break;
    }
    const size_t len = static_cast<size_t>(n);
    err = this->await_([&handle, buf, len](storage::CompletionCallback &&done) {
      return storage::global_storage_worker->write_chunk(handle, buf, len, std::move(done));
    });
    ok = err == storage::StorageError::STORAGE_ERROR_OK;
  }
  err = this->await_([&handle](storage::CompletionCallback &&done) {
    return storage::global_storage_worker->end_write(handle, std::move(done));
  });
  this->give_buffer_();
  if (fd >= 0) {
    ::close(fd);
    this->reply_(s, ok && err == storage::StorageError::STORAGE_ERROR_OK ? "226 Done" : "451 Write failed");
  }
}

void FtpServer::command_(Session &s, char *line) {
  char *arg = strchr(line, ' ');
  if (arg != nullptr) {
    *arg++ = '\0';
    while (*arg == ' ')
      arg++;
  }
  for (char *c = line; *c != '\0'; c++)
    *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
  if (strcmp(line, "USER") == 0) {
    strncpy(s.name, arg != nullptr ? arg : "", sizeof(s.name) - 1);
    s.logged = false;
    s.user = -1;
    this->reply_(s, "331 Password");
    return;
  }
  if (strcmp(line, "PASS") == 0) {
    for (uint8_t i = 0; i < this->user_count_; i++) {
      if (strcmp(this->users_[i].name, s.name) == 0 && strcmp(this->users_[i].password, arg != nullptr ? arg : "") == 0) {
        s.user = i;
        s.logged = true;
      }
    }
    this->reply_(s, s.logged ? "230 Logged in" : "530 Login incorrect");
    return;
  }
  if (strcmp(line, "QUIT") == 0) {
    this->reply_(s, "221 Bye");
    this->close_session_(s);
    return;
  }
  if (strcmp(line, "FEAT") == 0) {
    this->reply_(s, "211-Features:\r\n SIZE\r\n MDTM\r\n EPSV\r\n PASV\r\n MLSD\r\n REST STREAM\r\n UTF8\r\n211 End");
    return;
  }
  if (strcmp(line, "SYST") == 0) {
    this->reply_(s, "215 UNIX Type: L8");
    return;
  }
  if (strcmp(line, "NOOP") == 0 || strcmp(line, "OPTS") == 0) {
    this->reply_(s, "200 OK");
    return;
  }
  if (!s.logged) {
    this->reply_(s, "530 Not logged in");
    return;
  }
  char full[PATH_MAX_LEN];
  storage::FileStat st{};
  if (strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 || strcmp(line, "STRU") == 0) {
    this->reply_(s, "200 OK");
  } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
    char text[PATH_MAX_LEN + 32];
    snprintf(text, sizeof(text), "257 \"%s\"", s.cwd);
    this->reply_(s, text);
  } else if (strcmp(line, "CWD") == 0 || strcmp(line, "CDUP") == 0) {
    char next[PATH_MAX_LEN];
    const char *target = strcmp(line, "CDUP") == 0 ? ".." : arg;
    if (!this->path_of_(s, target, full) || this->stat_(full, &st) != storage::StorageError::STORAGE_ERROR_OK ||
        !st.is_dir) {
      this->reply_(s, "550 No such directory");
      return;
    }
    const size_t root_len = strcmp(this->root_, "/") == 0 ? 0 : strlen(this->root_);
    snprintf(next, sizeof(next), "%s", full[root_len] != '\0' ? full + root_len : "/");
    strcpy(s.cwd, next);
    this->reply_(s, "250 OK");
  } else if (strcmp(line, "PASV") == 0) {
    this->pasv_(s, false);
  } else if (strcmp(line, "EPSV") == 0) {
    this->pasv_(s, true);
  } else if (strcmp(line, "REST") == 0) {
    s.rest = arg != nullptr ? strtoull(arg, nullptr, 10) : 0;
    this->reply_(s, "350 Restarting");
  } else if (strcmp(line, "LIST") == 0) {
    this->list_(s, arg, 0);
  } else if (strcmp(line, "NLST") == 0) {
    this->list_(s, arg, 1);
  } else if (strcmp(line, "MLSD") == 0) {
    this->list_(s, arg, 2);
  } else if (strcmp(line, "RETR") == 0) {
    this->retr_(s, arg);
  } else if (strcmp(line, "STOR") == 0) {
    this->stor_(s, arg);
  } else if (strcmp(line, "SIZE") == 0 || strcmp(line, "MDTM") == 0) {
    if (!this->path_of_(s, arg, full) || this->stat_(full, &st) != storage::StorageError::STORAGE_ERROR_OK ||
        st.is_dir) {
      this->reply_(s, "550 Not a file");
      return;
    }
    char text[64];
    if (line[0] == 'S') {
      snprintf(text, sizeof(text), "213 %llu", static_cast<unsigned long long>(st.size));
    } else {
      struct tm t {};
      const time_t mtime = static_cast<time_t>(st.mtime);
      gmtime_r(&mtime, &t);
      char when[16];
      strftime(when, sizeof(when), "%Y%m%d%H%M%S", &t);
      snprintf(text, sizeof(text), "213 %s", when);
    }
    this->reply_(s, text);
  } else if (strcmp(line, "DELE") == 0 || strcmp(line, "RMD") == 0 || strcmp(line, "MKD") == 0 ||
             strcmp(line, "XMKD") == 0 || strcmp(line, "XRMD") == 0) {
    if (!this->may_write_(s)) {
      this->reply_(s, "550 Read only");
      return;
    }
    if (!this->path_of_(s, arg, full)) {
      this->reply_(s, "501 Bad path");
      return;
    }
    const char op = line[0] == 'D' ? 'd' : strstr(line, "RMD") != nullptr ? 'r' : 'm';
    storage::StorageError err = storage::StorageError::STORAGE_ERROR_NOT_FOUND;
    this->on_loop_([this, &full, op, &err]() {
      const char *rel = nullptr;
      storage::PathStorage *ps = this->resolve_(full, &rel);
      if (ps == nullptr)
        return;
      err = op == 'd' ? ps->remove(rel) : op == 'r' ? ps->rmdir(rel) : ps->mkdir(rel);
    });
    this->reply_(s, err == storage::StorageError::STORAGE_ERROR_OK ? (op == 'm' ? "257 Created" : "250 OK")
                                                                   : "550 Failed");
  } else if (strcmp(line, "RNFR") == 0) {
    if (!this->may_write_(s) || !this->path_of_(s, arg, s.rnfr)) {
      this->reply_(s, "550 Not allowed");
      return;
    }
    this->reply_(s, "350 Ready");
  } else if (strcmp(line, "RNTO") == 0) {
    if (s.rnfr[0] == '\0' || !this->path_of_(s, arg, full)) {
      this->reply_(s, "503 RNFR first");
      return;
    }
    storage::StorageError err = storage::StorageError::STORAGE_ERROR_NOT_FOUND;
    this->on_loop_([this, &s, &full, &err]() {
      const char *from_rel = nullptr;
      const char *to_rel = nullptr;
      storage::PathStorage *from = this->resolve_(s.rnfr, &from_rel);
      storage::PathStorage *to = this->resolve_(full, &to_rel);
      if (from != nullptr && from == to)
        err = from->rename(from_rel, to_rel);
    });
    s.rnfr[0] = '\0';
    this->reply_(s, err == storage::StorageError::STORAGE_ERROR_OK ? "250 OK" : "550 Failed");
  } else {
    this->reply_(s, "502 Not implemented");
  }
}

#ifdef USE_FTP_TFTP_SERVER
static constexpr uint16_t TFTP_RRQ = 1;
static constexpr uint16_t TFTP_WRQ = 2;
static constexpr uint16_t TFTP_DATA = 3;
static constexpr uint16_t TFTP_ACK = 4;
static constexpr uint16_t TFTP_ERROR = 5;
static constexpr size_t TFTP_BLOCK = 512;
static constexpr int TFTP_RETRIES = 5;

static uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

void FtpServer::tftp_error_(int fd, const void *to, uint32_t to_len, uint16_t code, const char *text) {
  uint8_t pkt[64];
  put16(pkt, TFTP_ERROR);
  put16(pkt + 2, code);
  const size_t n = std::min<size_t>(strlen(text), sizeof(pkt) - 5);
  std::memcpy(pkt + 4, text, n);
  pkt[4 + n] = 0;
  ::sendto(fd, pkt, 5 + n, 0, static_cast<const sockaddr *>(to), to_len);
}

void FtpServer::tftp_request_() {
  uint8_t req[TFTP_BLOCK + 4];
  struct sockaddr_in from {};
  socklen_t from_len = sizeof(from);
  const ssize_t n = ::recvfrom(this->tftp_fd_, req, sizeof(req) - 1, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
  if (n < 4)
    return;
  req[n] = 0;
  const uint16_t op = get16(req);
  const char *name = reinterpret_cast<const char *>(req + 2);
  int fd = -1;
  if (!this->open_listener_(&fd, 0, SOCK_DGRAM))
    return;
  struct timeval tv {};
  tv.tv_sec = 2;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  Session root{};
  strcpy(root.cwd, "/");
  char full[PATH_MAX_LEN];
  if (!this->path_of_(root, name, full)) {
    this->tftp_error_(fd, &from, from_len, 2, "Bad path");
  } else if (op == TFTP_RRQ) {
    this->tftp_send_file_(fd, &from, from_len, full);
  } else if (op == TFTP_WRQ && this->tftp_write_) {
    this->tftp_receive_file_(fd, &from, from_len, full);
  } else {
    this->tftp_error_(fd, &from, from_len, op == TFTP_WRQ ? 2 : 4, op == TFTP_WRQ ? "Read only" : "Illegal operation");
  }
  ::close(fd);
}

void FtpServer::tftp_send_file_(int fd, const void *to, uint32_t to_len, const char *full) {
  const char *rel = nullptr;
  storage::PathStorage *ps = this->resolve_(full, &rel);
  uint8_t *buf = ps != nullptr ? this->take_buffer_() : nullptr;
  if (buf == nullptr) {
    this->tftp_error_(fd, to, to_len, 1, "Not found");
    return;
  }
  storage::StreamHandle handle{};
  if (this->await_([ps, rel, &handle](storage::CompletionCallback &&done) {
        return storage::global_storage_worker->begin_read(ps, rel, &handle, std::move(done));
      }) != storage::StorageError::STORAGE_ERROR_OK) {
    this->give_buffer_();
    this->tftp_error_(fd, to, to_len, 1, "Not found");
    return;
  }
  uint16_t block = 0;
  bool more = true;
  while (more) {
    size_t got = 0;
    if (this->await_([&handle, buf, &got](storage::CompletionCallback &&done) {
          return storage::global_storage_worker->read_chunk(handle, buf + 4, TFTP_BLOCK, &got, std::move(done));
        }) != storage::StorageError::STORAGE_ERROR_OK)
      break;
    more = got == TFTP_BLOCK;
    block++;
    put16(buf, TFTP_DATA);
    put16(buf + 2, block);
    bool acked = false;
    for (int attempt = 0; attempt < TFTP_RETRIES && !acked; attempt++) {
      ::sendto(fd, buf, 4 + got, 0, static_cast<const sockaddr *>(to), to_len);
      uint8_t ack[8];
      const ssize_t n = ::recv(fd, ack, sizeof(ack), 0);
      acked = n >= 4 && get16(ack) == TFTP_ACK && get16(ack + 2) == block;
    }
    if (!acked)
      break;
  }
  this->await_([&handle](storage::CompletionCallback &&done) {
    return storage::global_storage_worker->end_read(handle, std::move(done));
  });
  this->give_buffer_();
}

void FtpServer::tftp_receive_file_(int fd, const void *to, uint32_t to_len, const char *full) {
  const char *rel = nullptr;
  storage::PathStorage *ps = this->resolve_(full, &rel);
  uint8_t *buf = ps != nullptr ? this->take_buffer_() : nullptr;
  if (buf == nullptr) {
    this->tftp_error_(fd, to, to_len, 2, "Not writable");
    return;
  }
  storage::StreamHandle handle{};
  if (this->await_([ps, rel, &handle](storage::CompletionCallback &&done) {
        return storage::global_storage_worker->begin_write(ps, rel, &handle, std::move(done));
      }) != storage::StorageError::STORAGE_ERROR_OK) {
    this->give_buffer_();
    this->tftp_error_(fd, to, to_len, 2, "Not writable");
    return;
  }
  uint8_t ack[4];
  put16(ack, TFTP_ACK);
  uint16_t block = 0;
  uint32_t total = 0;
  bool done = false;
  while (!done) {
    put16(ack + 2, block);
    ssize_t n = -1;
    for (int attempt = 0; attempt < TFTP_RETRIES; attempt++) {
      ::sendto(fd, ack, 4, 0, static_cast<const sockaddr *>(to), to_len);
      n = ::recv(fd, buf, 4 + TFTP_BLOCK, 0);
      if (n >= 4 && get16(buf) == TFTP_DATA && get16(buf + 2) == static_cast<uint16_t>(block + 1))
        break;
      n = -1;
    }
    if (n < 4)
      break;
    block++;
    const size_t len = static_cast<size_t>(n - 4);
    total += len;
    if (total > this->tftp_max_) {
      this->tftp_error_(fd, to, to_len, 3, "Too large");
      break;
    }
    if (len != 0 && this->await_([&handle, buf, len](storage::CompletionCallback &&cb) {
          return storage::global_storage_worker->write_chunk(handle, buf + 4, len, std::move(cb));
        }) != storage::StorageError::STORAGE_ERROR_OK) {
      this->tftp_error_(fd, to, to_len, 3, "Write failed");
      break;
    }
    if (len < TFTP_BLOCK) {
      put16(ack + 2, block);
      ::sendto(fd, ack, 4, 0, static_cast<const sockaddr *>(to), to_len);
      done = true;
    }
  }
  this->await_([&handle](storage::CompletionCallback &&cb) {
    return storage::global_storage_worker->end_write(handle, std::move(cb));
  });
  this->give_buffer_();
}
#endif

}

#endif
