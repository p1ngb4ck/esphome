#include "ftp_client.h"

#ifdef USE_FTP_TFTP_CLIENT

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace esphome::ftp {

static const char *const TAG = "ftp.tftp";

static constexpr uint16_t OP_RRQ = 1;
static constexpr uint16_t OP_WRQ = 2;
static constexpr uint16_t OP_DATA = 3;
static constexpr uint16_t OP_ACK = 4;
static constexpr uint16_t OP_ERROR = 5;
static constexpr uint16_t OP_OACK = 6;
static constexpr int RETRIES = 5;
static constexpr int TIMEOUT_S = 2;

static uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

static std::string remote_of(const char *path) {
  while (*path == '/')
    path++;
  return path;
}

storage::StorageError FTPClient::tftp_mount_() {
  if (this->connected_)
    return storage::StorageError::STORAGE_ERROR_OK;
  if (!network::is_connected())
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *res = nullptr;
  char port[8];
  snprintf(port, sizeof(port), "%u", this->port_);
  if (getaddrinfo(this->server_.c_str(), port, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "%s not resolved", this->server_.c_str());
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  }
  std::memcpy(&this->tftp_addr_, res->ai_addr, res->ai_addrlen);
  this->tftp_addr_len_ = res->ai_addrlen;
  freeaddrinfo(res);
  this->connected_ = true;
  ESP_LOGI(TAG, "%s:%u mounted at %s", this->server_.c_str(), this->port_, this->get_mount_path());
  return storage::StorageError::STORAGE_ERROR_OK;
}

void FTPClient::tftp_close_() {
  TftpSession *s = this->tftp_session_;
  if (s == nullptr)
    return;
  if (s->fd >= 0)
    ::close(s->fd);
  s->~TftpSession();
  RAMAllocator<TftpSession>(RAMAllocator<TftpSession>::ALLOC_EXTERNAL).deallocate(s, 1);
  this->tftp_session_ = nullptr;
}

int FTPClient::tftp_open_socket_() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;
  struct timeval tv {};
  tv.tv_sec = TIMEOUT_S;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

bool FTPClient::tftp_send_request_(int fd, uint16_t op, const std::string &path, bool tsize) {
  uint8_t req[4 + 256 + 16];
  if (path.size() > 255)
    return false;
  size_t n = 0;
  put16(req, op);
  n = 2;
  std::memcpy(req + n, path.c_str(), path.size() + 1);
  n += path.size() + 1;
  std::memcpy(req + n, "octet", 6);
  n += 6;
  if (tsize) {
    std::memcpy(req + n, "tsize\0" "0", 8);
    n += 8;
  }
  return ::sendto(fd, req, n, 0, reinterpret_cast<const sockaddr *>(&this->tftp_addr_), this->tftp_addr_len_) ==
         static_cast<ssize_t>(n);
}

int FTPClient::tftp_recv_(int fd, uint8_t *buf, size_t cap, struct sockaddr_storage *from, socklen_t *from_len) {
  *from_len = sizeof(*from);
  return static_cast<int>(::recvfrom(fd, buf, cap, 0, reinterpret_cast<sockaddr *>(from), from_len));
}

static storage::StorageError error_of(const uint8_t *pkt, int n) {
  const uint16_t code = n >= 4 ? get16(pkt + 2) : 0;
  return code == 1 ? storage::StorageError::STORAGE_ERROR_NOT_FOUND
         : code == 2 ? storage::StorageError::STORAGE_ERROR_PERMISSION_DENIED
                     : storage::StorageError::STORAGE_ERROR_READ_ERROR;
}

static bool oack_tsize(const uint8_t *pkt, int n, uint64_t *size) {
  int i = 2;
  while (i < n) {
    const char *key = reinterpret_cast<const char *>(pkt + i);
    const size_t klen = strnlen(key, n - i);
    i += klen + 1;
    if (i >= n)
      break;
    const char *value = reinterpret_cast<const char *>(pkt + i);
    const size_t vlen = strnlen(value, n - i);
    i += vlen + 1;
    if (strcasecmp(key, "tsize") == 0) {
      *size = strtoull(value, nullptr, 10);
      return true;
    }
  }
  return false;
}

storage::StorageError FTPClient::tftp_start_read_(const std::string &path, uint64_t *size) {
  this->tftp_close_();
  void *mem = RAMAllocator<TftpSession>(RAMAllocator<TftpSession>::ALLOC_EXTERNAL).allocate(1);
  if (mem == nullptr)
    return storage::StorageError::STORAGE_ERROR_NO_SPACE;
  TftpSession *s = new (mem) TftpSession();
  this->tftp_session_ = s;
  strncpy(s->path, path.c_str(), sizeof(s->path) - 1);
  s->fd = this->tftp_open_socket_();
  if (s->fd < 0)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  for (int attempt = 0; attempt < RETRIES; attempt++) {
    if (!this->tftp_send_request_(s->fd, OP_RRQ, remote_of(path.c_str()), true))
      return storage::StorageError::STORAGE_ERROR_READ_ERROR;
    const int n = this->tftp_recv_(s->fd, s->pkt, sizeof(s->pkt), &s->peer, &s->peer_len);
    if (n < 4)
      continue;
    const uint16_t op = get16(s->pkt);
    if (op == OP_ERROR)
      return error_of(s->pkt, n);
    if (op == OP_OACK) {
      uint64_t tsize = 0;
      const bool known = oack_tsize(s->pkt, n, &tsize);
      if (size != nullptr) {
        *size = known ? tsize : UINT64_MAX;
        return storage::StorageError::STORAGE_ERROR_OK;
      }
      uint8_t ack[4];
      put16(ack, OP_ACK);
      put16(ack + 2, 0);
      ::sendto(s->fd, ack, 4, 0, reinterpret_cast<sockaddr *>(&s->peer), s->peer_len);
      s->block = 0;
      return this->tftp_next_block_();
    }
    if (op == OP_DATA && get16(s->pkt + 2) == 1) {
      if (size != nullptr) {
        *size = UINT64_MAX;
        return storage::StorageError::STORAGE_ERROR_OK;
      }
      s->block = 1;
      s->have = static_cast<size_t>(n - 4);
      s->used = 0;
      s->last = s->have < TFTP_BLOCK;
      return storage::StorageError::STORAGE_ERROR_OK;
    }
  }
  return storage::StorageError::STORAGE_ERROR_TIMEOUT;
}

storage::StorageError FTPClient::tftp_next_block_() {
  TftpSession *s = this->tftp_session_;
  uint8_t ack[4];
  put16(ack, OP_ACK);
  for (int attempt = 0; attempt < RETRIES; attempt++) {
    if (s->block != 0 || attempt != 0) {
      put16(ack + 2, s->block);
      ::sendto(s->fd, ack, 4, 0, reinterpret_cast<sockaddr *>(&s->peer), s->peer_len);
    }
    struct sockaddr_storage from {};
    socklen_t from_len = 0;
    const int n = this->tftp_recv_(s->fd, s->pkt, sizeof(s->pkt), &from, &from_len);
    if (n < 4)
      continue;
    const uint16_t op = get16(s->pkt);
    if (op == OP_ERROR)
      return error_of(s->pkt, n);
    if (op == OP_DATA && get16(s->pkt + 2) == static_cast<uint16_t>(s->block + 1)) {
      s->block++;
      s->have = static_cast<size_t>(n - 4);
      s->used = 0;
      s->last = s->have < TFTP_BLOCK;
      return storage::StorageError::STORAGE_ERROR_OK;
    }
  }
  return storage::StorageError::STORAGE_ERROR_TIMEOUT;
}

storage::StorageError FTPClient::tftp_read_(const char *path, uint8_t *buf, uint64_t offset, size_t len, size_t *done) {
  *done = 0;
  if (!this->connected_ && this->tftp_mount_() != storage::StorageError::STORAGE_ERROR_OK)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  TftpSession *s = this->tftp_session_;
  if (s == nullptr || s->write || strcmp(s->path, path) != 0 || s->pos != offset) {
    const storage::StorageError err = this->tftp_start_read_(std::string(path), nullptr);
    if (err != storage::StorageError::STORAGE_ERROR_OK) {
      this->tftp_close_();
      return err;
    }
    s = this->tftp_session_;
    while (s->pos + (s->have - s->used) <= offset) {
      s->pos += s->have - s->used;
      s->used = s->have;
      if (s->last)
        return storage::StorageError::STORAGE_ERROR_OK;
      const storage::StorageError next = this->tftp_next_block_();
      if (next != storage::StorageError::STORAGE_ERROR_OK) {
        this->tftp_close_();
        return next;
      }
    }
    s->used += static_cast<size_t>(offset - s->pos);
    s->pos = offset;
  }
  while (*done < len) {
    if (s->used == s->have) {
      if (s->last)
        break;
      const storage::StorageError next = this->tftp_next_block_();
      if (next != storage::StorageError::STORAGE_ERROR_OK) {
        this->tftp_close_();
        return next;
      }
    }
    const size_t take = std::min(len - *done, s->have - s->used);
    std::memcpy(buf + *done, s->pkt + 4 + s->used, take);
    s->used += take;
    s->pos += take;
    *done += take;
  }
  if (s->last && s->used == s->have) {
    uint8_t ack[4];
    put16(ack, OP_ACK);
    put16(ack + 2, s->block);
    ::sendto(s->fd, ack, 4, 0, reinterpret_cast<sockaddr *>(&s->peer), s->peer_len);
  }
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError FTPClient::tftp_stat_(const char *path, storage::FileStat *out) {
  if (!this->connected_ && this->tftp_mount_() != storage::StorageError::STORAGE_ERROR_OK)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  uint64_t size = 0;
  const storage::StorageError err = this->tftp_start_read_(std::string(path), &size);
  TftpSession *s = this->tftp_session_;
  if (s != nullptr && s->fd >= 0 && s->peer_len != 0) {
    uint8_t abort[5] = {0, OP_ERROR, 0, 0, 0};
    ::sendto(s->fd, abort, sizeof(abort), 0, reinterpret_cast<sockaddr *>(&s->peer), s->peer_len);
  }
  this->tftp_close_();
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    return err;
  if (size == UINT64_MAX)
    return storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED;
  std::memset(out, 0, sizeof(*out));
  const char *base = strrchr(path, '/');
  base = base != nullptr ? base + 1 : path;
  strncpy(out->name, base, sizeof(out->name) - 1);
  out->is_dir = false;
  out->size = size;
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError FTPClient::tftp_send_block_(size_t len) {
  TftpSession *s = this->tftp_session_;
  s->block++;
  put16(s->pkt, OP_DATA);
  put16(s->pkt + 2, s->block);
  uint8_t reply[4 + 64];
  for (int attempt = 0; attempt < RETRIES; attempt++) {
    ::sendto(s->fd, s->pkt, 4 + len, 0, reinterpret_cast<sockaddr *>(&s->peer), s->peer_len);
    struct sockaddr_storage from {};
    socklen_t from_len = 0;
    const int n = this->tftp_recv_(s->fd, reply, sizeof(reply), &from, &from_len);
    if (n < 4)
      continue;
    if (get16(reply) == OP_ERROR)
      return error_of(reply, n);
    if (get16(reply) == OP_ACK && get16(reply + 2) == s->block)
      return storage::StorageError::STORAGE_ERROR_OK;
  }
  return storage::StorageError::STORAGE_ERROR_TIMEOUT;
}

storage::StorageError FTPClient::tftp_write_(const char *path, const uint8_t *buf, uint64_t offset, size_t len,
                                            size_t *done) {
  *done = 0;
  if (!this->connected_ && this->tftp_mount_() != storage::StorageError::STORAGE_ERROR_OK)
    return storage::StorageError::STORAGE_ERROR_NOT_READY;
  TftpSession *s = this->tftp_session_;
  if (s == nullptr || !s->write || strcmp(s->path, path) != 0 || s->pos != offset) {
    if (offset != 0)
      return storage::StorageError::STORAGE_ERROR_NOT_SUPPORTED;
    this->tftp_close_();
    void *mem = RAMAllocator<TftpSession>(RAMAllocator<TftpSession>::ALLOC_EXTERNAL).allocate(1);
    if (mem == nullptr)
      return storage::StorageError::STORAGE_ERROR_NO_SPACE;
    s = new (mem) TftpSession();
    this->tftp_session_ = s;
    s->write = true;
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->fd = this->tftp_open_socket_();
    if (s->fd < 0)
      return storage::StorageError::STORAGE_ERROR_NOT_READY;
    bool acked = false;
    for (int attempt = 0; attempt < RETRIES && !acked; attempt++) {
      if (!this->tftp_send_request_(s->fd, OP_WRQ, remote_of(path), false))
        break;
      uint8_t reply[4 + 64];
      const int n = this->tftp_recv_(s->fd, reply, sizeof(reply), &s->peer, &s->peer_len);
      if (n < 4)
        continue;
      if (get16(reply) == OP_ERROR) {
        const storage::StorageError err = error_of(reply, n);
        this->tftp_close_();
        return err;
      }
      acked = get16(reply) == OP_ACK && get16(reply + 2) == 0;
    }
    if (!acked) {
      this->tftp_close_();
      return storage::StorageError::STORAGE_ERROR_TIMEOUT;
    }
  }
  while (*done < len) {
    const size_t take = std::min(len - *done, TFTP_BLOCK - s->have);
    std::memcpy(s->pkt + 4 + s->have, buf + *done, take);
    s->have += take;
    s->pos += take;
    *done += take;
    if (s->have == TFTP_BLOCK) {
      const storage::StorageError err = this->tftp_send_block_(TFTP_BLOCK);
      if (err != storage::StorageError::STORAGE_ERROR_OK) {
        this->tftp_close_();
        return err;
      }
      s->have = 0;
    }
  }
  return storage::StorageError::STORAGE_ERROR_OK;
}

storage::StorageError FTPClient::tftp_finish_(const char *path, uint64_t size) {
  TftpSession *s = this->tftp_session_;
  if (s == nullptr || !s->write || strcmp(s->path, path) != 0) {
    if (size != 0)
      return storage::StorageError::STORAGE_ERROR_OK;
    size_t done = 0;
    const storage::StorageError err = this->tftp_write_(path, nullptr, 0, 0, &done);
    if (err != storage::StorageError::STORAGE_ERROR_OK)
      return err;
    s = this->tftp_session_;
  }
  const storage::StorageError err = this->tftp_send_block_(s->have);
  this->tftp_close_();
  return err;
}

}

#endif
