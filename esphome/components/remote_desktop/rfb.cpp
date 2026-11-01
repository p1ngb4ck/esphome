#include "remote_desktop.h"

#ifdef USE_REMOTE_DESKTOP

#include "vnc_des.h"
#ifdef USE_REMOTE_DESKTOP_USB
#include "select/usb_slot.h"
#endif

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/select.h>
#include <unistd.h>

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::remote_desktop {

static const char *const TAG = "remote_desktop";

static constexpr int CONNECT_TIMEOUT_S = 5;
static constexpr uint32_t PROBE_WAIT_MS = 1500;
static constexpr int RECV_TIMEOUT_S = 30;
static constexpr int SEND_TIMEOUT_S = 5;

static constexpr int32_t ENC_RAW = 0;
static constexpr int32_t ENC_COPYRECT = 1;
static constexpr int32_t ENC_TIGHT = 7;
static constexpr int32_t ENC_JPEG_QUALITY = -24;
static constexpr int32_t ENC_COMPRESS_LEVEL = -254;
static constexpr int32_t ENC_CURSOR = -239;
static constexpr int32_t ENC_DESKTOP_SIZE = -223;
static constexpr int32_t ENC_LAST_RECT = -224;
static constexpr int32_t ENC_QEMU_KEY = -258;
static constexpr int32_t ENC_EXTENDED_DESKTOP_SIZE = -308;

const uint16_t KEYBOARD_XT[0xE8] = {
    0x000, 0x000, 0x000, 0x000, 0x01E, 0x030, 0x02E, 0x020, 0x012, 0x021, 0x022, 0x023, 0x017, 0x024, 0x025, 0x026,
    0x032, 0x031, 0x018, 0x019, 0x010, 0x013, 0x01F, 0x014, 0x016, 0x02F, 0x011, 0x02D, 0x015, 0x02C, 0x002, 0x003,
    0x004, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00A, 0x00B, 0x01C, 0x001, 0x00E, 0x00F, 0x039, 0x00C, 0x00D, 0x01A,
    0x01B, 0x02B, 0x02B, 0x027, 0x028, 0x029, 0x033, 0x034, 0x035, 0x03A, 0x03B, 0x03C, 0x03D, 0x03E, 0x03F, 0x040,
    0x041, 0x042, 0x043, 0x044, 0x057, 0x058, 0x054, 0x046, 0x146, 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D,
    0x14B, 0x150, 0x148, 0x045, 0x135, 0x037, 0x04A, 0x04E, 0x11C, 0x04F, 0x050, 0x051, 0x04B, 0x04C, 0x04D, 0x047,
    0x048, 0x049, 0x052, 0x053, 0x056, 0x15D, 0x15E, 0x059, 0x05D, 0x05E, 0x05F, 0x055, 0x103, 0x177, 0x104, 0x05A,
    0x074, 0x179, 0x06D, 0x06F, 0x064, 0x175, 0x11E, 0x10C, 0x168, 0x105, 0x107, 0x13C, 0x178, 0x065, 0x141, 0x120,
    0x130, 0x12E, 0x000, 0x000, 0x000, 0x07E, 0x000, 0x073, 0x070, 0x07D, 0x079, 0x07B, 0x05C, 0x000, 0x000, 0x000,
    0x0F2, 0x0F1, 0x078, 0x077, 0x076, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x176, 0x17B, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x01D, 0x02A, 0x038, 0x15B, 0x11D, 0x036, 0x138, 0x15C,
};

static const char KEY_PLAIN[] = "abcdefghijklmnopqrstuvwxyz1234567890";
static const char KEY_SHIFTED[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()";
static const char PUNCT_PLAIN[] = "-=[]\\#;'`,./";
static const char PUNCT_SHIFTED[] = "_+{}|~:\"~<>?";

static uint32_t keysym_for(uint16_t usage, bool shift) {
  if (usage >= 0x04 && usage <= 0x27)
    return static_cast<uint8_t>(shift ? KEY_SHIFTED[usage - 0x04] : KEY_PLAIN[usage - 0x04]);
  if (usage >= 0x2D && usage <= 0x38)
    return static_cast<uint8_t>(shift ? PUNCT_SHIFTED[usage - 0x2D] : PUNCT_PLAIN[usage - 0x2D]);
  if (usage >= 0x3A && usage <= 0x45)
    return 0xFFBE + (usage - 0x3A);
  if (usage >= 0x59 && usage <= 0x61)
    return 0xFFB1 + (usage - 0x59);
  switch (usage) {
    case 0x28:
      return 0xFF0D;
    case 0x29:
      return 0xFF1B;
    case 0x2A:
      return 0xFF08;
    case 0x2B:
      return 0xFF09;
    case 0x2C:
      return 0x20;
    case 0x39:
      return 0xFFE5;
    case 0x46:
      return 0xFF61;
    case 0x47:
      return 0xFF14;
    case 0x48:
      return 0xFF13;
    case 0x49:
      return 0xFF63;
    case 0x4A:
      return 0xFF50;
    case 0x4B:
      return 0xFF55;
    case 0x4C:
      return 0xFFFF;
    case 0x4D:
      return 0xFF57;
    case 0x4E:
      return 0xFF56;
    case 0x4F:
      return 0xFF53;
    case 0x50:
      return 0xFF51;
    case 0x51:
      return 0xFF54;
    case 0x52:
      return 0xFF52;
    case 0x53:
      return 0xFF7F;
    case 0x54:
      return 0xFFAF;
    case 0x55:
      return 0xFFAA;
    case 0x56:
      return 0xFFAD;
    case 0x57:
      return 0xFFAB;
    case 0x58:
      return 0xFF8D;
    case 0x62:
      return 0xFFB0;
    case 0x63:
      return 0xFFAE;
    case 0x64:
      return shift ? 0x3E : 0x3C;
    case 0x65:
      return 0xFF67;
    case 0xE0:
      return 0xFFE3;
    case 0xE1:
      return 0xFFE1;
    case 0xE2:
      return 0xFFE9;
    case 0xE3:
      return 0xFFEB;
    case 0xE4:
      return 0xFFE4;
    case 0xE5:
      return 0xFFE2;
    case 0xE6:
      return 0xFE03;
    case 0xE7:
      return 0xFFEC;
    default:
      return 0;
  }
}

struct Inflate {
  tinfl_decompressor decompressor;
  uint8_t window[TINFL_LZ_DICT_SIZE];
  size_t at;
};

#ifdef USE_REMOTE_DESKTOP_USB
static constexpr uint16_t USBIP_PORT = 3240;
static constexpr uint16_t USBIP_VERSION = 0x0111;
static constexpr uint16_t USBIP_REQ_DEVLIST = 0x8005;
static constexpr uint16_t USBIP_REP_DEVLIST = 0x0005;
static constexpr uint16_t USBIP_REQ_IMPORT = 0x8003;
static constexpr uint16_t USBIP_REP_IMPORT = 0x0003;

static bool send_all(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    const ssize_t sent = ::send(fd, data, len, 0);
    if (sent <= 0)
      return false;
    data += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

static bool recv_all(int fd, uint8_t *data, size_t len) {
  while (len > 0) {
    const ssize_t got = ::recv(fd, data, len, 0);
    if (got <= 0)
      return false;
    data += got;
    len -= static_cast<size_t>(got);
  }
  return true;
}

struct UsbipLink : UsbLink {
  int fd{-1};
  bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) override {
    return send_all(this->fd, head, head_len) && (data_len == 0 || send_all(this->fd, data, data_len));
  }
};

static int usbip_listen() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(USBIP_PORT);
  if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(fd, 2) < 0) {
    ESP_LOGE(TAG, "USB/IP: port %u not available", USBIP_PORT);
    ::close(fd);
    return -1;
  }
  return fd;
}

static void put_be16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

static void put_be32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
#endif

static uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
static void put16(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
static void put32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

static int connect_to(const char *host, uint16_t port) {
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  char port_text[6];
  snprintf(port_text, sizeof(port_text), "%u", port);
  if (::getaddrinfo(host, port_text, &hints, &result) != 0 || result == nullptr) {
    ESP_LOGE(TAG, "%s not found", host);
    return -1;
  }
  struct sockaddr_in addr {};
  std::memcpy(&addr, result->ai_addr, sizeof(addr));
  ::freeaddrinfo(result);
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int connected = ::connect(fd, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
  if (connected < 0 && errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    struct timeval tv = {.tv_sec = CONNECT_TIMEOUT_S, .tv_usec = 0};
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (::select(fd + 1, nullptr, &writable, nullptr, &tv) == 1 &&
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0)
      connected = 0;
  }
  if (connected < 0) {
    ::close(fd);
    return -1;
  }
  ::fcntl(fd, F_SETFL, flags);
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  struct timeval recv_timeout = {.tv_sec = RECV_TIMEOUT_S, .tv_usec = 0};
  struct timeval send_timeout = {.tv_sec = SEND_TIMEOUT_S, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
  return fd;
}

bool RemoteDesktop::read_(void *data, size_t len) {
  auto *at = static_cast<uint8_t *>(data);
  while (len != 0) {
    if (this->stop_.load())
      return false;
    const ssize_t n = ::recv(this->fd_, at, len, 0);
    if (n <= 0)
      return false;
    at += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool RemoteDesktop::write_(const void *data, size_t len) {
  const auto *at = static_cast<const uint8_t *>(data);
  while (len != 0) {
    const ssize_t n = ::send(this->fd_, at, len, 0);
    if (n <= 0)
      return false;
    at += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool RemoteDesktop::skip_(size_t len) {
  while (len != 0) {
    const size_t take = std::min<size_t>(len, IN_BYTES);
    if (!this->read_(this->in_, take))
      return false;
    len -= take;
  }
  return true;
}

bool RemoteDesktop::read_compact_(uint32_t *len) {
  uint8_t b = 0;
  uint32_t value = 0;
  for (int i = 0; i != 3; i++) {
    if (!this->read_(&b, 1))
      return false;
    value |= static_cast<uint32_t>(i == 2 ? b : (b & 0x7F)) << (7 * i);
    if (i == 2 || (b & 0x80) == 0)
      break;
  }
  *len = value;
  return true;
}

RemoteDesktop::Protocol RemoteDesktop::probe_() {
  const int fd = connect_to(this->host_, this->port_);
  if (fd < 0) {
    ESP_LOGW(TAG, "%s:%u not reached", this->host_, this->port_);
    return PROTOCOL_AUTO;
  }
  Protocol kind = PROTOCOL_SPICE;
  for (const uint32_t start = millis();;) {
    const uint32_t spent = millis() - start;
    if (spent >= PROBE_WAIT_MS || this->stop_.load())
      break;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    FD_SET(this->wake_fd_, &readable);
    const uint32_t left = PROBE_WAIT_MS - spent;
    struct timeval wait = {.tv_sec = static_cast<time_t>(left / 1000),
                           .tv_usec = static_cast<suseconds_t>((left % 1000) * 1000)};
    if (::select(std::max(fd, this->wake_fd_) + 1, &readable, nullptr, nullptr, &wait) <= 0)
      break;
    if (FD_ISSET(this->wake_fd_, &readable)) {
      uint64_t count;
      ::read(this->wake_fd_, &count, sizeof(count));
    }
    if (!FD_ISSET(fd, &readable))
      continue;
    uint8_t greeting[4];
    const int got = ::recv(fd, greeting, sizeof(greeting), MSG_PEEK);
    if (got <= 0)
      break;
    if (got < static_cast<int>(sizeof(greeting))) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (std::memcmp(greeting, "RFB ", sizeof(greeting)) == 0)
      kind = PROTOCOL_VNC;
    break;
  }
  ::close(fd);
  return kind;
}

void RemoteDesktop::session_() {
  if (this->protocol_ == PROTOCOL_AUTO) {
    this->protocol_ = this->probe_();
    if (this->protocol_ == PROTOCOL_AUTO) {
      this->close_page_.store(true);
      std::memset(this->password_, 0, sizeof(this->password_));
      return;
    }
  }
  if (this->protocol_ == PROTOCOL_SPICE) {
    this->spice_session_();
    this->close_page_.store(true);
    std::memset(this->password_, 0, sizeof(this->password_));
    return;
  }
  if (!this->allocate_()) {
    ESP_LOGE(TAG, "No PSRAM for the session");
    this->free_();
    return;
  }
  this->inflate_state_ =
      static_cast<Inflate *>(heap_caps_malloc(sizeof(Inflate) * STREAMS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->inflate_state_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the session");
    this->free_();
    return;
  }
  for (int i = 0; i != STREAMS; i++) {
    tinfl_init(&this->inflate_state_[i].decompressor);
    this->inflate_state_[i].at = 0;
  }
  this->fd_ = connect_to(this->host_, this->port_);
  bool ok = this->fd_ >= 0;
  if (!ok)
    ESP_LOGW(TAG, "%s:%u not reached", this->host_, this->port_);
  ok = ok && this->handshake_();
  if (ok) {
    this->state_.store(STATE_RUNNING);
    this->changed_();
    ESP_LOGI(TAG, "Connected to %s:%u (%ux%u)", this->host_, this->port_, static_cast<unsigned>(this->fb_w_),
             static_cast<unsigned>(this->fb_h_));
  }
#ifdef USE_REMOTE_DESKTOP_USB
  UsbipLink links[USB_SLOTS_MAX];
  const int listener = ok && this->usb_count_ > 0 ? usbip_listen() : -1;
  auto drop = [this, &links](uint8_t i) {
    this->usb_[i]->link_close();
    ::close(links[i].fd);
    links[i].fd = -1;
  };
  auto accept = [this, &links, listener]() {
    const int fd = ::accept(listener, nullptr, nullptr);
    if (fd < 0)
      return;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval timeout = {.tv_sec = RECV_TIMEOUT_S, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    uint8_t op[8];
    if (!recv_all(fd, op, sizeof(op))) {
      ::close(fd);
      return;
    }
    const uint16_t code = static_cast<uint16_t>((op[2] << 8) | op[3]);
    if (code == USBIP_REQ_DEVLIST) {
      uint8_t *out = this->out_;
      size_t at = 12;
      uint32_t count = 0;
      for (uint8_t i = 0; i < this->usb_count_; i++) {
        const size_t n = this->usb_[i]->usbip_entry(out + at, OUT_BYTES - at, true);
        at += n;
        count += n != 0 ? 1 : 0;
      }
      put_be16(out, USBIP_VERSION);
      put_be16(out + 2, USBIP_REP_DEVLIST);
      put_be32(out + 4, 0);
      put_be32(out + 8, count);
      send_all(fd, out, at);
      ::close(fd);
      return;
    }
    uint8_t busid[32];
    int slot = -1;
    if (code == USBIP_REQ_IMPORT && recv_all(fd, busid, sizeof(busid))) {
      busid[sizeof(busid) - 1] = '\0';
      unsigned bus = 0, n = 0;
      if (std::sscanf(reinterpret_cast<const char *>(busid), "%u-%u", &bus, &n) == 2 && bus == 1 && n >= 1 &&
          n <= this->usb_count_ && links[n - 1].fd < 0) {
        uint8_t entry[0x138];
        if (this->usb_[n - 1]->usbip_entry(entry, sizeof(entry), false) != 0)
          slot = static_cast<int>(n - 1);
      }
    }
    if (slot >= 0 && !this->usb_[slot]->link_open(&links[slot], UsbSlot::Proto::USBIP))
      slot = -1;
    if (slot < 0) {
      uint8_t reply[8];
      put_be16(reply, USBIP_VERSION);
      put_be16(reply + 2, USBIP_REP_IMPORT);
      put_be32(reply + 4, 1);
      send_all(fd, reply, sizeof(reply));
      ::close(fd);
      return;
    }
    links[slot].fd = fd;
    ESP_LOGI(TAG, "USB/IP: slot %d imported", slot + 1);
  };
#endif
  while (ok && !this->stop_.load()) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(this->fd_, &readable);
    FD_SET(this->wake_fd_, &readable);
    int top = std::max(this->fd_, this->wake_fd_);
#ifdef USE_REMOTE_DESKTOP_USB
    if (listener >= 0) {
      FD_SET(listener, &readable);
      top = std::max(top, listener);
    }
    for (uint8_t i = 0; i < this->usb_count_; i++) {
      size_t room = 0;
      if (links[i].fd >= 0)
        this->usb_[i]->rx_space(room);
      if (room > 0) {
        FD_SET(links[i].fd, &readable);
        top = std::max(top, links[i].fd);
      }
    }
#endif
    if (::select(top + 1, &readable, nullptr, nullptr, nullptr) < 0)
      break;
    if (FD_ISSET(this->wake_fd_, &readable)) {
      uint64_t count = 0;
      ::read(this->wake_fd_, &count, sizeof(count));
      ok = this->inputs_();
    }
    if (ok && FD_ISSET(this->fd_, &readable))
      ok = this->message_();
#ifdef USE_REMOTE_DESKTOP_USB
    if (listener >= 0 && FD_ISSET(listener, &readable))
      accept();
    for (uint8_t i = 0; i < this->usb_count_; i++) {
      if (links[i].fd < 0)
        continue;
      UsbSlot *slot = this->usb_[i];
      bool alive = true;
      if (FD_ISSET(links[i].fd, &readable)) {
        size_t room = 0;
        uint8_t *into = slot->rx_space(room);
        const int got = room > 0 ? ::recv(links[i].fd, into, room, 0) : 0;
        alive = got > 0 && slot->rx_done(static_cast<size_t>(got));
      }
      if (!alive || !slot->service()) {
        ESP_LOGI(TAG, "USB/IP: slot %u detached", i + 1);
        drop(i);
      }
    }
#endif
  }
#ifdef USE_REMOTE_DESKTOP_USB
  for (uint8_t i = 0; i < this->usb_count_; i++) {
    if (links[i].fd >= 0)
      drop(i);
  }
  if (listener >= 0)
    ::close(listener);
#endif
  if (this->fd_ >= 0)
    ::close(this->fd_);
  this->fd_ = -1;
  this->close_page_.store(true);
  heap_caps_free(this->inflate_state_);
  this->inflate_state_ = nullptr;
  this->free_();
  this->cursor_shown_ = false;
  this->cursor_w_ = 0;
  ESP_LOGI(TAG, "Disconnected from %s:%u", this->host_, this->port_);
  std::memset(this->password_, 0, sizeof(this->password_));
}

bool RemoteDesktop::handshake_() {
  uint8_t buf[24];
  if (!this->read_(buf, 12) || std::memcmp(buf, "RFB ", 4) != 0)
    return false;
  const int minor = (buf[8] - '0') * 100 + (buf[9] - '0') * 10 + (buf[10] - '0');
  const bool v38 = minor >= 8;
  const bool v37 = minor >= 7;
  const char *reply = v38 ? "RFB 003.008\n" : (v37 ? "RFB 003.007\n" : "RFB 003.003\n");
  if (!this->write_(reply, 12))
    return false;
  uint32_t security = 0;
  if (v37) {
    uint8_t count = 0;
    if (!this->read_(&count, 1))
      return false;
    if (count == 0) {
      ESP_LOGW(TAG, "The server refused the connection");
      return false;
    }
    uint8_t types[255];
    if (!this->read_(types, count))
      return false;
    bool none = false;
    bool vnc = false;
    for (uint8_t i = 0; i != count; i++) {
      none = none || types[i] == 1;
      vnc = vnc || types[i] == 2;
    }
    security = vnc && this->password_[0] != '\0' ? 2 : (none ? 1 : (vnc ? 2 : 0));
    if (security == 0) {
      ESP_LOGW(TAG, "No security type this client takes (None, VNC)");
      return false;
    }
    const uint8_t chosen = static_cast<uint8_t>(security);
    if (!this->write_(&chosen, 1))
      return false;
  } else {
    if (!this->read_(buf, 4))
      return false;
    security = be32(buf);
    if (security != 1 && security != 2) {
      ESP_LOGW(TAG, "The server refused the connection");
      return false;
    }
  }
  if (security == 2) {
    uint8_t challenge[16];
    uint8_t response[16];
    if (!this->read_(challenge, 16))
      return false;
    vnc_response(this->password_, challenge, response);
    if (!this->write_(response, 16))
      return false;
  }
  if (security == 2 || v38) {
    if (!this->read_(buf, 4))
      return false;
    if (be32(buf) != 0) {
      ESP_LOGW(TAG, "Authentication failed");
      return false;
    }
  }
  const uint8_t shared = 1;
  if (!this->write_(&shared, 1) || !this->read_(buf, 24))
    return false;
  this->fb_w_ = be16(buf);
  this->fb_h_ = be16(buf + 2);
  if (!this->skip_(be32(buf + 20)))
    return false;

  this->canvas_ = this->output_->open_page(this);
  if (this->canvas_ == nullptr) {
    ESP_LOGW(TAG, "No canvas free for the desktop");
    return false;
  }
  this->page_w_ = this->canvas_->width();
  this->page_h_ = this->canvas_->height();
  this->page_bpp_ = display::VideoCanvas::page_bpp();
  uint8_t format[20]{};
  format[0] = 0;
  uint8_t *pf = format + 4;
  if (this->page_bpp_ == 2) {
    pf[0] = 16;
    pf[1] = 16;
    pf[2] = 0;
    pf[3] = 1;
    put16(pf + 4, 31);
    put16(pf + 6, 63);
    put16(pf + 8, 31);
    pf[10] = 11;
    pf[11] = 5;
    pf[12] = 0;
    this->pixel_bytes_ = 2;
    this->tpixel_bytes_ = 2;
  } else {
    pf[0] = 32;
    pf[1] = 24;
    pf[2] = 0;
    pf[3] = 1;
    put16(pf + 4, 255);
    put16(pf + 6, 255);
    put16(pf + 8, 255);
    pf[10] = 16;
    pf[11] = 8;
    pf[12] = 0;
    this->pixel_bytes_ = 4;
    this->tpixel_bytes_ = 3;
  }
  if (!this->write_(format, sizeof(format)))
    return false;
  const int32_t encodings[] = {ENC_TIGHT,   ENC_COPYRECT,     ENC_RAW,      ENC_JPEG_QUALITY,
                               ENC_COMPRESS_LEVEL, ENC_CURSOR, ENC_DESKTOP_SIZE, ENC_LAST_RECT,
                               ENC_QEMU_KEY, ENC_EXTENDED_DESKTOP_SIZE};
  const size_t count = sizeof(encodings) / sizeof(encodings[0]);
  uint8_t set[4 + count * 4];
  set[0] = 2;
  set[1] = 0;
  put16(set + 2, count);
  for (size_t i = 0; i != count; i++)
    put32(set + 4 + i * 4, static_cast<uint32_t>(encodings[i]));
  if (!this->write_(set, sizeof(set)))
    return false;
  this->pointer_x_ = static_cast<int32_t>(std::min(this->fb_w_, this->page_w_) / 2);
  this->pointer_y_ = static_cast<int32_t>(std::min(this->fb_h_, this->page_h_) / 2);
  this->pointer_buttons_ = 0;
  this->resize_asked_ = false;
  this->qemu_keys_ = false;
  this->shift_ = false;
  std::memset(this->pressed_keysym_, 0, sizeof(this->pressed_keysym_));
  return this->request_update_(false);
}

bool RemoteDesktop::request_update_(bool incremental) {
  uint8_t req[10];
  req[0] = 3;
  req[1] = incremental ? 1 : 0;
  put16(req + 2, 0);
  put16(req + 4, 0);
  put16(req + 6, this->fb_w_);
  put16(req + 8, this->fb_h_);
  return this->write_(req, sizeof(req));
}

bool RemoteDesktop::message_() {
  uint8_t type = 0;
  if (!this->read_(&type, 1))
    return false;
  uint8_t buf[8];
  switch (type) {
    case 0:
      return this->update_();
    case 1: {
      if (!this->read_(buf, 5))
        return false;
      return this->skip_(static_cast<size_t>(be16(buf + 3)) * 6);
    }
    case 2:
      return true;
    case 3: {
      if (!this->read_(buf, 7))
        return false;
      return this->skip_(be32(buf + 3));
    }
    default:
      ESP_LOGW(TAG, "Unknown server message %u", type);
      return false;
  }
}

bool RemoteDesktop::update_() {
  uint8_t buf[12];
  if (!this->read_(buf, 3))
    return false;
  const uint16_t rects = be16(buf + 1);
  this->cursor_hide_();
  bool ok = true;
  for (uint32_t i = 0; ok && i != rects; i++) {
    if (!this->read_(buf, 12))
      return false;
    const uint32_t x = be16(buf);
    const uint32_t y = be16(buf + 2);
    const uint32_t w = be16(buf + 4);
    const uint32_t h = be16(buf + 6);
    const auto encoding = static_cast<int32_t>(be32(buf + 8));
    if (encoding == ENC_LAST_RECT)
      break;
    switch (encoding) {
      case ENC_RAW:
        ok = this->rect_raw_(x, y, w, h);
        break;
      case ENC_COPYRECT:
        ok = this->rect_copy_(x, y, w, h);
        break;
      case ENC_TIGHT:
        ok = this->rect_tight_(x, y, w, h);
        break;
      case ENC_CURSOR:
        ok = this->rect_cursor_(x, y, w, h);
        break;
      case ENC_DESKTOP_SIZE:
        this->fb_w_ = w;
        this->fb_h_ = h;
        break;
      case ENC_EXTENDED_DESKTOP_SIZE:
        ok = this->rect_desktop_size_(x, y, w, h);
        break;
      case ENC_QEMU_KEY:
        this->qemu_keys_ = true;
        break;
      default:
        ESP_LOGW(TAG, "Unknown encoding %d", static_cast<int>(encoding));
        ok = false;
        break;
    }
  }
  if (!ok)
    return false;
  this->cursor_show_();
  this->canvas_->page_show();
  return this->request_update_(true);
}

bool RemoteDesktop::rect_desktop_size_(uint32_t reason, uint32_t status, uint32_t w, uint32_t h) {
  uint8_t head[4];
  if (!this->read_(head, 4) || !this->skip_(static_cast<size_t>(head[0]) * 16))
    return false;
  if (status == 0) {
    this->fb_w_ = w;
    this->fb_h_ = h;
  }
  if (!this->resize_asked_ && reason == 0 && (w != this->page_w_ || h != this->page_h_)) {
    this->resize_asked_ = true;
    uint8_t msg[24]{};
    msg[0] = 251;
    put16(msg + 2, this->page_w_);
    put16(msg + 4, this->page_h_);
    msg[6] = 1;
    put32(msg + 8, 0);
    put16(msg + 12, 0);
    put16(msg + 14, 0);
    put16(msg + 16, this->page_w_);
    put16(msg + 18, this->page_h_);
    put32(msg + 20, 0);
    return this->write_(msg, sizeof(msg));
  }
  return true;
}

void RemoteDesktop::tpixel_to_page_(const uint8_t *in, uint8_t *out) const {
  if (this->page_bpp_ == 2) {
    out[0] = in[0];
    out[1] = in[1];
  } else {
    out[0] = in[2];
    out[1] = in[1];
    out[2] = in[0];
  }
}

void RemoteDesktop::pixel_to_page_(const uint8_t *in, uint8_t *out) const {
  out[0] = in[0];
  out[1] = in[1];
  if (this->page_bpp_ == 3)
    out[2] = in[2];
}

bool RemoteDesktop::draw_row_(const uint8_t *pixels, uint32_t x, uint32_t y, uint32_t w) {
  if (y >= this->page_h_ || x >= this->page_w_)
    return true;
  const uint32_t visible = std::min<uint32_t>(w, this->page_w_ - x);
  this->canvas_->page_pixels(pixels, static_cast<size_t>(visible) * this->page_bpp_, x, y, visible, 1);
  return true;
}

bool RemoteDesktop::draw_fill_(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *pixel) {
  if (x >= this->page_w_ || y >= this->page_h_)
    return true;
  this->canvas_->page_fill(x, y, std::min<uint32_t>(w, this->page_w_ - x), std::min<uint32_t>(h, this->page_h_ - y),
                           pixel);
  return true;
}

bool RemoteDesktop::rect_raw_(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  const size_t row = static_cast<size_t>(w) * this->pixel_bytes_;
  if (row == 0 || row > OUT_BYTES)
    return row == 0;
  for (uint32_t r = 0; r != h; r++) {
    if (!this->read_(this->out_, row))
      return false;
    uint8_t *line = this->in_;
    for (uint32_t i = 0; i != w; i++)
      this->pixel_to_page_(this->out_ + static_cast<size_t>(i) * this->pixel_bytes_,
                           line + static_cast<size_t>(i) * this->page_bpp_);
    this->draw_row_(line, x, y + r, w);
  }
  return true;
}

bool RemoteDesktop::rect_copy_(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint8_t src[4];
  if (!this->read_(src, 4))
    return false;
  const uint32_t sx = be16(src);
  const uint32_t sy = be16(src + 2);
  if (sx + w <= this->page_w_ && sy + h <= this->page_h_ && x + w <= this->page_w_ && y + h <= this->page_h_)
    this->canvas_->page_copy(sx, sy, x, y, w, h);
  return true;
}

bool RemoteDesktop::inflate_(int stream, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
  Inflate &z = this->inflate_state_[stream];
  size_t produced = 0;
  while (produced < out_len) {
    size_t in_size = in_len;
    size_t out_size = TINFL_LZ_DICT_SIZE - z.at;
    const tinfl_status status =
        tinfl_decompress(&z.decompressor, in, &in_size, z.window, z.window + z.at, &out_size,
                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
    in += in_size;
    in_len -= in_size;
    const size_t take = std::min(out_size, out_len - produced);
    std::memcpy(out + produced, z.window + z.at, take);
    produced += take;
    z.at = (z.at + out_size) & (TINFL_LZ_DICT_SIZE - 1);
    if (status < TINFL_STATUS_DONE)
      return false;
    if (in_len == 0 && out_size == 0)
      break;
    if (out_size > take)
      return false;
  }
  return produced == out_len;
}

bool RemoteDesktop::rect_tight_(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint8_t control = 0;
  if (!this->read_(&control, 1))
    return false;
  for (int i = 0; i != STREAMS; i++) {
    if (control & (1 << i)) {
      tinfl_init(&this->inflate_state_[i].decompressor);
      this->inflate_state_[i].at = 0;
    }
  }
  const uint8_t kind = control >> 4;
  const size_t tp = this->tpixel_bytes_;
  if (kind == 0x08) {
    uint8_t in[4];
    uint8_t pixel[4];
    if (!this->read_(in, tp))
      return false;
    this->tpixel_to_page_(in, pixel);
    return this->draw_fill_(x, y, w, h, pixel);
  }
  if (kind == 0x09) {
    uint32_t len = 0;
    if (!this->read_compact_(&len))
      return false;
    if (len > IN_BYTES)
      return this->skip_(len);
    if (!this->read_(this->in_, len))
      return false;
    if (x + w <= this->page_w_ && y + h <= this->page_h_)
      this->canvas_->page_rect(this->in_, len, x, y, w, h);
    return true;
  }
  if (kind > 0x09) {
    ESP_LOGW(TAG, "Tight subencoding %u not asked for", kind);
    return false;
  }
  const int stream = (control >> 4) & 0x03;
  uint8_t filter = 0;
  if (control & 0x40) {
    if (!this->read_(&filter, 1))
      return false;
  }
  uint8_t palette[256 * 3];
  uint32_t colors = 0;
  size_t size = static_cast<size_t>(w) * h * tp;
  size_t row_bytes = static_cast<size_t>(w) * tp;
  if (filter == 1) {
    uint8_t n = 0;
    if (!this->read_(&n, 1))
      return false;
    colors = static_cast<uint32_t>(n) + 1;
    uint8_t in[4];
    for (uint32_t i = 0; i != colors; i++) {
      if (!this->read_(in, tp))
        return false;
      this->tpixel_to_page_(in, palette + static_cast<size_t>(i) * this->page_bpp_);
    }
    row_bytes = colors == 2 ? (w + 7) / 8 : w;
    size = row_bytes * h;
  } else if (filter > 2) {
    ESP_LOGW(TAG, "Tight filter %u unknown", filter);
    return false;
  }
  if (size > OUT_BYTES)
    return false;
  if (size < 12) {
    if (!this->read_(this->out_, size))
      return false;
  } else {
    uint32_t len = 0;
    if (!this->read_compact_(&len) || len > IN_BYTES || !this->read_(this->in_, len))
      return false;
    if (!this->inflate_(stream, this->in_, len, this->out_, size)) {
      ESP_LOGW(TAG, "Tight data not inflated");
      return false;
    }
  }
  uint8_t *line = this->in_;
  const size_t bpp = this->page_bpp_;
  for (uint32_t r = 0; r != h; r++) {
    const uint8_t *src = this->out_ + static_cast<size_t>(r) * row_bytes;
    if (filter == 1) {
      for (uint32_t i = 0; i != w; i++) {
        const uint32_t index = colors == 2 ? (src[i / 8] >> (7 - (i & 7))) & 1 : src[i];
        std::memcpy(line + i * bpp, palette + std::min(index, colors - 1) * bpp, bpp);
      }
    } else if (filter == 2) {
      uint8_t *cur = this->out_ + static_cast<size_t>(r) * row_bytes;
      const uint8_t *up = r != 0 ? cur - row_bytes : nullptr;
      if (tp == 3) {
        for (uint32_t i = 0; i != w; i++) {
          for (uint32_t c = 0; c != 3; c++) {
            const int left = i != 0 ? cur[(i - 1) * 3 + c] : 0;
            const int above = up != nullptr ? up[i * 3 + c] : 0;
            const int corner = (i != 0 && up != nullptr) ? up[(i - 1) * 3 + c] : 0;
            const int predicted = std::max(0, std::min(255, left + above - corner));
            cur[i * 3 + c] = static_cast<uint8_t>(cur[i * 3 + c] + predicted);
          }
          this->tpixel_to_page_(cur + i * 3, line + i * bpp);
        }
      } else {
        static constexpr int MAX[3] = {31, 63, 31};
        static constexpr int SHIFT[3] = {11, 5, 0};
        auto px = [](const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); };
        for (uint32_t i = 0; i != w; i++) {
          const uint16_t diff = px(cur + i * 2);
          uint16_t value = 0;
          for (int c = 0; c != 3; c++) {
            const int left = i != 0 ? (px(cur + (i - 1) * 2) >> SHIFT[c]) & MAX[c] : 0;
            const int above = up != nullptr ? (px(up + i * 2) >> SHIFT[c]) & MAX[c] : 0;
            const int corner = (i != 0 && up != nullptr) ? (px(up + (i - 1) * 2) >> SHIFT[c]) & MAX[c] : 0;
            const int predicted = std::max(0, std::min(MAX[c], left + above - corner));
            const int comp = (((diff >> SHIFT[c]) & MAX[c]) + predicted) & MAX[c];
            value = static_cast<uint16_t>(value | (comp << SHIFT[c]));
          }
          cur[i * 2] = static_cast<uint8_t>(value);
          cur[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
          line[i * 2] = cur[i * 2];
          line[i * 2 + 1] = cur[i * 2 + 1];
        }
      }
    } else {
      for (uint32_t i = 0; i != w; i++)
        this->tpixel_to_page_(src + static_cast<size_t>(i) * tp, line + static_cast<size_t>(i) * bpp);
    }
    this->draw_row_(line, x, y + r, w);
  }
  return true;
}

bool RemoteDesktop::rect_cursor_(uint32_t hot_x, uint32_t hot_y, uint32_t w, uint32_t h) {
  const size_t pixels = static_cast<size_t>(w) * h * this->pixel_bytes_;
  const size_t mask = static_cast<size_t>((w + 7) / 8) * h;
  if (w > CURSOR_MAX || h > CURSOR_MAX) {
    this->cursor_w_ = 0;
    return this->skip_(pixels + mask);
  }
  if (!this->read_(this->out_, pixels) || !this->read_(this->cursor_mask_, mask))
    return false;
  for (size_t i = 0; i != static_cast<size_t>(w) * h; i++)
    this->pixel_to_page_(this->out_ + i * this->pixel_bytes_, this->cursor_pixels_ + i * this->page_bpp_);
  this->cursor_w_ = w;
  this->cursor_h_ = h;
  this->cursor_hot_x_ = hot_x;
  this->cursor_hot_y_ = hot_y;
  return true;
}

void RemoteDesktop::cursor_hide_() {
  if (!this->cursor_shown_)
    return;
  this->cursor_shown_ = false;
  this->canvas_->page_pixels(this->cursor_under_, static_cast<size_t>(this->under_w_) * this->page_bpp_,
                             this->under_x_, this->under_y_, this->under_w_, this->under_h_);
}

void RemoteDesktop::cursor_show_() {
  if (this->cursor_shown_ || this->cursor_w_ == 0)
    return;
  const int32_t left = this->pointer_x_ - static_cast<int32_t>(this->cursor_hot_x_);
  const int32_t top = this->pointer_y_ - static_cast<int32_t>(this->cursor_hot_y_);
  const int32_t x0 = std::max<int32_t>(0, left);
  const int32_t y0 = std::max<int32_t>(0, top);
  const int32_t x1 = std::min<int32_t>(static_cast<int32_t>(this->page_w_), left + static_cast<int32_t>(this->cursor_w_));
  const int32_t y1 = std::min<int32_t>(static_cast<int32_t>(this->page_h_), top + static_cast<int32_t>(this->cursor_h_));
  if (x1 <= x0 || y1 <= y0)
    return;
  this->under_x_ = static_cast<uint32_t>(x0);
  this->under_y_ = static_cast<uint32_t>(y0);
  this->under_w_ = static_cast<uint32_t>(x1 - x0);
  this->under_h_ = static_cast<uint32_t>(y1 - y0);
  const size_t stride = static_cast<size_t>(this->under_w_) * this->page_bpp_;
  if (!this->canvas_->page_read(this->cursor_under_, stride, this->under_x_, this->under_y_, this->under_w_,
                                this->under_h_))
    return;
  uint8_t *shape = this->out_;
  std::memcpy(shape, this->cursor_under_, stride * this->under_h_);
  const uint32_t mask_row = (this->cursor_w_ + 7) / 8;
  for (int32_t y = y0; y != y1; y++) {
    const uint32_t cy = static_cast<uint32_t>(y - top);
    for (int32_t x = x0; x != x1; x++) {
      const uint32_t cx = static_cast<uint32_t>(x - left);
      if ((this->cursor_mask_[cy * mask_row + cx / 8] >> (7 - (cx & 7))) & 1)
        std::memcpy(shape + static_cast<size_t>(y - y0) * stride + static_cast<size_t>(x - x0) * this->page_bpp_,
                    this->cursor_pixels_ + (static_cast<size_t>(cy) * this->cursor_w_ + cx) * this->page_bpp_,
                    this->page_bpp_);
    }
  }
  this->canvas_->page_pixels(shape, stride, this->under_x_, this->under_y_, this->under_w_, this->under_h_);
  this->cursor_shown_ = true;
}

bool RemoteDesktop::send_key_(uint16_t usage, bool pressed) {
  if (usage == 0xE1 || usage == 0xE5)
    this->shift_ = pressed;
  uint32_t keysym = pressed ? keysym_for(usage, this->shift_) : this->pressed_keysym_[usage];
  if (pressed)
    this->pressed_keysym_[usage] = keysym;
  else
    this->pressed_keysym_[usage] = 0;
  if (this->qemu_keys_ && usage < sizeof(KEYBOARD_XT) / sizeof(KEYBOARD_XT[0]) && KEYBOARD_XT[usage] != 0) {
    const uint16_t xt = KEYBOARD_XT[usage];
    const uint32_t keycode = xt < 0x100 ? xt : (0x80u | (xt & 0xFF));
    uint8_t msg[12];
    msg[0] = 255;
    msg[1] = 0;
    put16(msg + 2, pressed ? 1 : 0);
    put32(msg + 4, keysym);
    put32(msg + 8, keycode);
    return this->write_(msg, sizeof(msg));
  }
  if (keysym == 0)
    return true;
  uint8_t msg[8]{};
  msg[0] = 4;
  msg[1] = pressed ? 1 : 0;
  put32(msg + 4, keysym);
  return this->write_(msg, sizeof(msg));
}

bool RemoteDesktop::send_pointer_() {
  uint8_t msg[6];
  msg[0] = 5;
  msg[1] = this->pointer_buttons_;
  put16(msg + 2, static_cast<uint32_t>(this->pointer_x_));
  put16(msg + 4, static_cast<uint32_t>(this->pointer_y_));
  return this->write_(msg, sizeof(msg));
}

bool RemoteDesktop::inputs_() {
  Input input;
  bool moved = false;
  const int32_t max_x = static_cast<int32_t>(std::min(this->fb_w_, this->page_w_)) - 1;
  const int32_t max_y = static_cast<int32_t>(std::min(this->fb_h_, this->page_h_)) - 1;
  while (xQueueReceive(this->inputs_q_, &input, 0) == pdTRUE) {
    if (input.kind == INPUT_KEY) {
      if (!this->send_key_(input.code, input.buttons != 0))
        return false;
      continue;
    }
    if (input.kind == INPUT_POINTER) {
      this->pointer_x_ = std::max<int32_t>(0, std::min<int32_t>(max_x, this->pointer_x_ + input.x));
      this->pointer_y_ = std::max<int32_t>(0, std::min<int32_t>(max_y, this->pointer_y_ + input.y));
    } else if (input.x >= 0 && input.y >= 0) {
      this->pointer_x_ = std::min<int32_t>(max_x, input.x);
      this->pointer_y_ = std::min<int32_t>(max_y, input.y);
    }
    this->pointer_buttons_ = input.buttons;
    moved = true;
    if (!this->send_pointer_())
      return false;
    if (input.kind == INPUT_POINTER && input.code != 0) {
      this->pointer_buttons_ = static_cast<uint8_t>(input.buttons | input.code);
      if (!this->send_pointer_())
        return false;
      this->pointer_buttons_ = input.buttons;
      if (!this->send_pointer_())
        return false;
    }
  }
  if (moved && this->cursor_w_ != 0) {
    this->cursor_hide_();
    this->cursor_show_();
    this->canvas_->page_show();
  }
  return true;
}

}

#endif
