#include "emulator.h"

#ifdef SVP_EMULATOR

#include <cerrno>
#include <cstring>

#include "emu_shim.h"
#include "esphome/core/log.h"

#ifdef SVP_EMU_LINK
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include "esp_rom_crc.h"
#include "esp_vfs_eventfd.h"
#endif

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.emulator";

void GameEmulator::link_note_(uint8_t type) {
#ifdef SVP_EMU_LINK
  if (this->link_fd_ >= 0 && (type == LINK_BYE || !this->link_cable_))
    this->link_send_(type, 0, 0, 0);
#endif
}

void GameEmulator::link_wake_() {
#ifdef SVP_EMU_LINK
  if (this->link_efd_ >= 0) {
    const uint64_t one = 1;
    ::write(this->link_efd_, &one, sizeof(one));
  }
#endif
}

void GameEmulator::link_close_() {
#ifdef SVP_EMU_LINK
  if (this->link_fd_ >= 0) {
    emu_set_link(nullptr, nullptr, nullptr);
    ::close(this->link_fd_);
    this->link_fd_ = -1;
  }
  if (this->link_efd_ >= 0) {
    ::close(this->link_efd_);
    this->link_efd_ = -1;
  }
  this->link_buf_len_ = 0;
  this->link_mode_ = LinkMode::OFF;
  this->link_state_.store(static_cast<uint8_t>(LinkState::OFF), std::memory_order_release);
#endif
}

#ifdef SVP_EMU_LINK
static constexpr uint8_t LINK_VERSION = 1;
static constexpr uint32_t LINK_TIMEOUT_MS = 5000;
static constexpr uint32_t LINK_FOREVER = UINT32_MAX;
static constexpr int LINK_READY = 1;
static constexpr int LINK_IDLE = 0;
static constexpr int LINK_WOKEN = -1;
static constexpr int LINK_ERROR = -2;

void GameEmulator::link_begin_() {
  const esp_vfs_eventfd_config_t config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&config);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    this->link_efd_ = eventfd(0, 0);
  if (this->link_efd_ < 0) {
    ESP_LOGE(TAG, "No eventfd for the link");
    this->link_mode_ = LinkMode::OFF;
    this->link_state_.store(static_cast<uint8_t>(LinkState::LOST), std::memory_order_release);
    this->parent_->status_callbacks_.call();
    this->go_panel_();
    return;
  }
  this->state_ = State::LINKING;
  this->task_pause_.store(false, std::memory_order_release);
  this->create_task_();
}

void GameEmulator::link_connect_() {
  this->link_state_.store(static_cast<uint8_t>(LinkState::WAITING), std::memory_order_release);
  this->link_changed_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
  const int fd = this->link_mode_ == LinkMode::HOST ? this->link_listen_() : this->link_dial_();
  if (fd < 0) {
    if (!this->task_stop_.load(std::memory_order_acquire))
      ESP_LOGW(TAG, "No link: the game runs alone");
    this->link_state_.store(static_cast<uint8_t>(LinkState::LOST), std::memory_order_release);
    this->link_changed_.store(true, std::memory_order_release);
    App.wake_loop_threadsafe();
    return;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  struct timeval send_timeout = {.tv_sec = LINK_TIMEOUT_MS / 1000, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
  this->link_fd_ = fd;
  this->link_buf_len_ = 0;
  this->peer_paused_ = false;
  this->net_frame_ = 0;
  this->net_sent_ = false;
  this->net_local_ = 0;
  const uint32_t crc = esp_rom_crc32_le(0, this->rom_, static_cast<uint32_t>(this->rom_size_));
  const auto kind = static_cast<uint16_t>(static_cast<uint16_t>(this->system_) | (this->link_cable_ ? 0x100 : 0));
  if (!this->link_send_(LINK_HELLO, LINK_VERSION, kind, crc))
    return;
  LinkMsg hello{};
  const int got = this->link_take_(&hello, LINK_TIMEOUT_MS);
  if (got != LINK_READY || hello.type != LINK_HELLO || hello.a != LINK_VERSION || hello.b != kind || hello.c != crc) {
    this->link_lost_(got == LINK_READY ? "the other panel runs another game" : "no answer from the other panel");
    return;
  }
  if (this->link_cable_)
    emu_set_link(&GameEmulator::link_send_cb_, &GameEmulator::link_recv_cb_, this);
  this->link_state_.store(static_cast<uint8_t>(LinkState::CONNECTED), std::memory_order_release);
  this->link_changed_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
  ESP_LOGI(TAG, "Linked with the other panel (%s)", this->link_cable_ ? "cable" : "netplay");
}

int GameEmulator::link_listen_() {
  const int lfd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (lfd < 0)
    return -1;
  int one = 1;
  ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->link_port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  int fd = -1;
  if (::bind(lfd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0 && ::listen(lfd, 1) == 0) {
    ESP_LOGI(TAG, "Waiting for the other panel on port %u", this->link_port_);
    if (this->link_wait_(lfd, false, LINK_FOREVER) == LINK_READY)
      fd = ::accept(lfd, nullptr, nullptr);
  }
  ::close(lfd);
  return fd;
}

int GameEmulator::link_dial_() {
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char port_text[6];
  snprintf(port_text, sizeof(port_text), "%u", this->link_port_);
  struct addrinfo *found = nullptr;
  if (::getaddrinfo(this->link_peer_, port_text, &hints, &found) != 0 || found == nullptr) {
    ESP_LOGW(TAG, "%s not found", this->link_peer_);
    return -1;
  }
  struct sockaddr_in addr {};
  memcpy(&addr, found->ai_addr, sizeof(addr));
  ::freeaddrinfo(found);
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int result = ::connect(fd, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
  if (result < 0 && errno == EINPROGRESS) {
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (this->link_wait_(fd, true, LINK_TIMEOUT_MS) == LINK_READY &&
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0)
      result = 0;
  }
  if (result < 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFL, flags);
  return fd;
}

int GameEmulator::link_wait_(int fd, bool write, uint32_t timeout_ms) {
  fd_set readable;
  fd_set writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_SET(this->link_efd_, &readable);
  if (write) {
    FD_SET(fd, &writable);
  } else {
    FD_SET(fd, &readable);
  }
  struct timeval tv = {.tv_sec = static_cast<time_t>(timeout_ms / 1000),
                       .tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
  const int n = ::select(std::max(fd, this->link_efd_) + 1, &readable, write ? &writable : nullptr, nullptr,
                         timeout_ms == LINK_FOREVER ? nullptr : &tv);
  if (n < 0)
    return LINK_ERROR;
  if (FD_ISSET(this->link_efd_, &readable)) {
    uint64_t value = 0;
    ::read(this->link_efd_, &value, sizeof(value));
    return LINK_WOKEN;
  }
  return n == 0 ? LINK_IDLE : LINK_READY;
}

bool GameEmulator::link_send_(uint8_t type, uint8_t a, uint16_t b, uint32_t c) {
  if (this->link_fd_ < 0)
    return false;
  const LinkMsg msg{type, a, b, c};
  const auto *data = reinterpret_cast<const uint8_t *>(&msg);
  size_t sent = 0;
  while (sent < sizeof(msg)) {
    const ssize_t n = ::send(this->link_fd_, data + sent, sizeof(msg) - sent, 0);
    if (n <= 0) {
      this->link_lost_("send failed");
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

int GameEmulator::link_take_(LinkMsg *msg, uint32_t wait_ms) {
  while (true) {
    if (this->link_buf_len_ >= sizeof(LinkMsg)) {
      memcpy(msg, this->link_buf_, sizeof(LinkMsg));
      this->link_buf_len_ -= sizeof(LinkMsg);
      memmove(this->link_buf_, this->link_buf_ + sizeof(LinkMsg), this->link_buf_len_);
      if (msg->type != LINK_BYE)
        return LINK_READY;
      this->link_lost_("the other panel left");
      return LINK_ERROR;
    }
    if (this->link_fd_ < 0)
      return LINK_ERROR;
    if (wait_ms != 0) {
      const int ready = this->link_wait_(this->link_fd_, false, wait_ms);
      if (ready == LINK_ERROR)
        this->link_lost_("select failed");
      if (ready != LINK_READY)
        return ready;
    }
    const ssize_t n = ::recv(this->link_fd_, this->link_buf_ + this->link_buf_len_,
                             sizeof(this->link_buf_) - this->link_buf_len_, MSG_DONTWAIT);
    if (n > 0) {
      this->link_buf_len_ += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (wait_ms == 0)
        return LINK_IDLE;
      continue;
    }
    this->link_lost_(n == 0 ? "the other panel closed the link" : "receive failed");
    return LINK_ERROR;
  }
}

int GameEmulator::net_pads_(uint16_t *pads) {
  if (!this->net_sent_) {
    this->net_next_ = static_cast<uint16_t>(this->pads_[0].load(std::memory_order_relaxed) |
                                            this->pads_[1].load(std::memory_order_relaxed));
    if (!this->link_send_(LINK_PAD, 0, this->net_next_, this->net_frame_ + 1))
      return 0;
    this->net_sent_ = true;
  }
  uint16_t peer = 0;
  if (this->net_frame_ != 0) {
    while (true) {
      LinkMsg msg{};
      const int got = this->link_take_(&msg, this->peer_paused_ ? LINK_FOREVER : LINK_TIMEOUT_MS);
      if (got == LINK_WOKEN)
        return -1;
      if (got == LINK_READY && msg.type == LINK_PAUSE) {
        this->peer_paused_ = true;
        continue;
      }
      if (got == LINK_READY && msg.type == LINK_RESUME) {
        this->peer_paused_ = false;
        continue;
      }
      if (got == LINK_READY && msg.type == LINK_PAD && msg.c == this->net_frame_) {
        peer = msg.b;
        break;
      }
      this->link_lost_(got == LINK_IDLE ? "no input from the other panel" : "the panels are out of step");
      return 0;
    }
  }
  const bool host = this->link_mode_ == LinkMode::HOST;
  pads[0] = host ? this->net_local_ : peer;
  pads[1] = host ? peer : this->net_local_;
  this->net_local_ = this->net_next_;
  this->net_frame_++;
  this->net_sent_ = false;
  return 1;
}

void GameEmulator::link_lost_(const char *why) {
  if (this->link_fd_ < 0)
    return;
  ESP_LOGW(TAG, "Link ended: %s", why);
  emu_set_link(nullptr, nullptr, nullptr);
  ::close(this->link_fd_);
  this->link_fd_ = -1;
  this->link_buf_len_ = 0;
  this->link_state_.store(static_cast<uint8_t>(LinkState::LOST), std::memory_order_release);
  this->link_changed_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
}

bool GameEmulator::link_send_cb_(void *ctx, uint8_t type, uint8_t a, uint16_t b) {
  return static_cast<GameEmulator *>(ctx)->link_send_(type, a, b, 0);
}

bool GameEmulator::link_recv_cb_(void *ctx, uint8_t *type, uint8_t *a, uint16_t *b, uint32_t wait_ms) {
  auto *emu = static_cast<GameEmulator *>(ctx);
  LinkMsg msg{};
  while (emu->link_take_(&msg, wait_ms) == LINK_READY) {
    if (msg.type >= LINK_PAD)
      continue;
    *type = msg.type;
    *a = msg.a;
    *b = msg.b;
    return true;
  }
  return false;
}
#endif

}

#endif
