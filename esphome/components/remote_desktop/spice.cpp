#include "remote_desktop.h"

#ifdef USE_REMOTE_DESKTOP

#include "esphome/core/log.h"
#ifdef USE_REMOTE_DESKTOP_USB
#include "select/usb_slot.h"
#endif

#include <esp_idf_version.h>
#include <esp_random.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <new>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define REMOTE_DESKTOP_PSA
#include <psa/crypto.h>
#else
#include <mbedtls/rsa.h>
#endif

namespace esphome::remote_desktop {

static const char *const TAG = "remote_desktop.spice";

namespace {

constexpr uint32_t SPICE_MAGIC = 0x51444552;
constexpr uint32_t VERSION_MAJOR = 2;
constexpr uint32_t VERSION_MINOR = 2;
constexpr size_t PUBKEY_BYTES = 162;
constexpr size_t TICKET_BYTES = 128;
constexpr size_t LINK_MESS_BYTES = 18;
constexpr size_t LINK_REPLY_FIXED = 4 + PUBKEY_BYTES + 12;
constexpr uint8_t CHANNEL_MAIN = 1;
constexpr uint8_t CHANNEL_DISPLAY = 2;
constexpr uint8_t CHANNEL_INPUTS = 3;
constexpr uint8_t CHANNEL_CURSOR = 4;
constexpr uint8_t CHANNEL_PLAYBACK = 5;
constexpr uint8_t CHANNEL_USBREDIR = 9;
constexpr uint16_t MSG_SPICEVMC_DATA = 101;
constexpr uint16_t MSGC_SPICEVMC_DATA = 101;
constexpr uint16_t MSG_PLAYBACK_DATA = 101;
constexpr uint16_t MSG_PLAYBACK_MODE = 102;
constexpr uint16_t MSG_PLAYBACK_START = 103;
constexpr uint16_t MSG_PLAYBACK_STOP = 104;
constexpr uint16_t AUDIO_DATA_MODE_RAW = 1;
constexpr uint16_t AUDIO_FMT_S16 = 1;
constexpr uint32_t COMMON_CAP_AUTH_SELECTION = 1u << 0;
constexpr uint32_t COMMON_CAP_AUTH_SPICE = 1u << 1;
constexpr uint32_t COMMON_CAP_MINI_HEADER = 1u << 3;
constexpr uint32_t AUTH_SPICE = 1;
constexpr uint32_t LINK_ERR_NEED_SECURED = 5;
constexpr uint32_t LINK_ERR_PERMISSION_DENIED = 7;
constexpr uint32_t DISPLAY_CAP_SIZED_STREAM = 1u << 0;
constexpr uint32_t DISPLAY_CAP_PREF_COMPRESSION = 1u << 6;
constexpr uint32_t DISPLAY_CAP_MULTI_CODEC = 1u << 8;
constexpr uint32_t DISPLAY_CAP_CODEC_MJPEG = 1u << 9;
constexpr uint32_t DISPLAY_CAP_PREF_VIDEO_CODEC_TYPE = 1u << 12;

constexpr uint16_t MSG_SET_ACK = 3;
constexpr uint16_t MSG_PING = 4;
constexpr uint16_t MSG_DISCONNECTING = 6;
constexpr uint16_t MSG_NOTIFY = 7;
constexpr uint16_t MSG_LIST = 8;
constexpr uint16_t MSGC_ACK_SYNC = 1;
constexpr uint16_t MSGC_ACK = 2;
constexpr uint16_t MSGC_PONG = 3;
constexpr uint16_t MSG_MAIN_INIT = 103;
constexpr uint16_t MSG_MAIN_CHANNELS_LIST = 104;
constexpr uint16_t MSG_MAIN_MOUSE_MODE = 105;
constexpr uint16_t MSGC_MAIN_ATTACH_CHANNELS = 104;
constexpr uint16_t MSGC_MAIN_MOUSE_MODE_REQUEST = 105;
constexpr uint16_t MOUSE_MODE_CLIENT = 1u << 1;
constexpr uint16_t MSGC_INPUTS_KEY_DOWN = 101;
constexpr uint16_t MSGC_INPUTS_KEY_UP = 102;
constexpr uint16_t MSGC_INPUTS_MOUSE_MOTION = 111;
constexpr uint16_t MSGC_INPUTS_MOUSE_POSITION = 112;
constexpr uint16_t MSGC_INPUTS_MOUSE_PRESS = 113;
constexpr uint16_t MSGC_INPUTS_MOUSE_RELEASE = 114;
constexpr uint16_t MSG_INPUTS_MOUSE_MOTION_ACK = 111;
constexpr uint32_t MOTION_ACK_BUNCH = 4;
constexpr uint16_t MSG_CURSOR_INIT = 101;
constexpr uint16_t MSG_CURSOR_RESET = 102;
constexpr uint16_t MSG_CURSOR_SET = 103;
constexpr uint16_t MSG_CURSOR_MOVE = 104;
constexpr uint16_t MSG_CURSOR_HIDE = 105;
constexpr uint8_t CURSOR_TYPE_ALPHA = 0;
constexpr uint8_t CURSOR_TYPE_MONO = 1;
constexpr uint8_t CURSOR_TYPE_COLOR32 = 6;
constexpr uint16_t CURSOR_FLAGS_NONE = 1u << 0;
constexpr uint16_t CURSOR_FLAGS_FROM_CACHE = 1u << 2;

constexpr uint16_t MSG_DISPLAY_RESET = 103;
constexpr uint16_t MSG_DISPLAY_COPY_BITS = 104;
constexpr uint16_t MSG_DISPLAY_STREAM_CREATE = 122;
constexpr uint16_t MSG_DISPLAY_STREAM_DATA = 123;
constexpr uint16_t MSG_DISPLAY_STREAM_DESTROY = 125;
constexpr uint16_t MSG_DISPLAY_STREAM_DESTROY_ALL = 126;
constexpr uint16_t MSG_DISPLAY_DRAW_FILL = 302;
constexpr uint16_t MSG_DISPLAY_DRAW_OPAQUE = 303;
constexpr uint16_t MSG_DISPLAY_DRAW_COPY = 304;
constexpr uint16_t MSG_DISPLAY_DRAW_BLEND = 305;
constexpr uint16_t MSG_DISPLAY_DRAW_BLACKNESS = 306;
constexpr uint16_t MSG_DISPLAY_DRAW_WHITENESS = 307;
constexpr uint16_t MSG_DISPLAY_DRAW_INVERS = 308;
constexpr uint16_t MSG_DISPLAY_SURFACE_CREATE = 314;
constexpr uint16_t MSG_DISPLAY_STREAM_DATA_SIZED = 316;
constexpr uint16_t MSGC_DISPLAY_INIT = 101;
constexpr uint16_t MSGC_DISPLAY_PREFERRED_COMPRESSION = 103;
constexpr uint16_t MSGC_DISPLAY_PREFERRED_VIDEO_CODEC_TYPE = 105;
constexpr uint8_t IMAGE_COMPRESSION_OFF = 1;
constexpr uint8_t VIDEO_CODEC_MJPEG = 1;
constexpr uint8_t CLIP_RECTS = 1;
constexpr uint8_t BRUSH_SOLID = 1;
constexpr uint8_t IMAGE_BITMAP = 0;
constexpr uint8_t IMAGE_JPEG = 105;
constexpr uint8_t BITMAP_FMT_8BIT = 5;
constexpr uint8_t BITMAP_FMT_16BIT = 6;
constexpr uint8_t BITMAP_FMT_24BIT = 7;
constexpr uint8_t BITMAP_FMT_32BIT = 8;
constexpr uint8_t BITMAP_FMT_RGBA = 9;
constexpr uint8_t BITMAP_FLAGS_PAL_FROM_CACHE = 1u << 1;
constexpr uint8_t BITMAP_FLAGS_TOP_DOWN = 1u << 2;
constexpr uint32_t SURFACE_FLAGS_PRIMARY = 1u << 0;
constexpr uint16_t ROPD_OP_PUT = 1u << 3;
constexpr uint8_t STREAMS_MAX = 16;

constexpr uint8_t BUTTON_LEFT = 1, BUTTON_MIDDLE = 2, BUTTON_RIGHT = 3, BUTTON_UP = 4, BUTTON_DOWN = 5;
constexpr uint16_t button_mask(uint8_t button) { return static_cast<uint16_t>(1u << (button - 1)); }

constexpr int CONNECT_TIMEOUT_S = 5;
constexpr int RECV_TIMEOUT_S = 30;
constexpr int SEND_TIMEOUT_S = 10;
constexpr size_t SCRATCH_BYTES = 64 * 1024;
constexpr size_t DISPLAY_MESSAGE_MAX = 8 * 1024 * 1024;

uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t rd64(const uint8_t *p) { return rd32(p) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool send_all(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    const int sent = ::send(fd, data, len, 0);
    if (sent <= 0)
      return false;
    data += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

bool recv_all(int fd, uint8_t *data, size_t len) {
  while (len > 0) {
    const int got = ::recv(fd, data, len, 0);
    if (got <= 0)
      return false;
    data += got;
    len -= static_cast<size_t>(got);
  }
  return true;
}

bool der_take(const uint8_t *&p, const uint8_t *end, uint8_t tag, const uint8_t *&content, size_t &len) {
  if (end - p < 2 || p[0] != tag)
    return false;
  size_t length = p[1];
  const uint8_t *q = p + 2;
  if (length & 0x80) {
    const size_t bytes = length & 0x7F;
    if (bytes == 0 || bytes > 2 || static_cast<size_t>(end - q) < bytes)
      return false;
    length = 0;
    for (size_t i = 0; i < bytes; i++)
      length = (length << 8) | *q++;
  }
  if (static_cast<size_t>(end - q) < length)
    return false;
  content = q;
  len = length;
  p = q + length;
  return true;
}

#ifndef REMOTE_DESKTOP_PSA
int spice_random(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}
#endif

bool encrypt_ticket(const uint8_t *spki, const char *password, uint8_t *out) {
  const uint8_t *p = spki, *end = spki + PUBKEY_BYTES, *seq, *alg, *bits;
  size_t seq_len, alg_len, bits_len;
  if (!der_take(p, end, 0x30, seq, seq_len))
    return false;
  const uint8_t *q = seq, *seq_end = seq + seq_len;
  if (!der_take(q, seq_end, 0x30, alg, alg_len) || !der_take(q, seq_end, 0x03, bits, bits_len) || bits_len < 2 ||
      bits[0] != 0)
    return false;
  const uint8_t *rsa_key = bits + 1;
  const size_t rsa_key_len = bits_len - 1;
  const size_t input_len = std::strlen(password) + 1;
#ifdef REMOTE_DESKTOP_PSA
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attr, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1));
  mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
  if (psa_import_key(&attr, rsa_key, rsa_key_len, &key) != PSA_SUCCESS)
    return false;
  size_t out_len = 0;
  const bool ok =
      psa_asymmetric_encrypt(key, PSA_ALG_RSA_OAEP(PSA_ALG_SHA_1), reinterpret_cast<const uint8_t *>(password),
                             input_len, nullptr, 0, out, TICKET_BYTES, &out_len) == PSA_SUCCESS &&
      out_len == TICKET_BYTES;
  psa_destroy_key(key);
  return ok;
#else
  const uint8_t *r = rsa_key, *r_end = rsa_key + rsa_key_len, *body, *modulus, *exponent;
  size_t body_len, modulus_len, exponent_len;
  if (!der_take(r, r_end, 0x30, body, body_len))
    return false;
  const uint8_t *b = body, *b_end = body + body_len;
  if (!der_take(b, b_end, 0x02, modulus, modulus_len) || !der_take(b, b_end, 0x02, exponent, exponent_len))
    return false;
  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa);
  const bool ok = mbedtls_rsa_import_raw(&rsa, modulus, modulus_len, nullptr, 0, nullptr, 0, nullptr, 0, exponent,
                                         exponent_len) == 0 &&
                  mbedtls_rsa_complete(&rsa) == 0 &&
                  mbedtls_rsa_set_padding(&rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1) == 0 &&
                  mbedtls_rsa_get_len(&rsa) == TICKET_BYTES &&
                  mbedtls_rsa_pkcs1_encrypt(&rsa, spice_random, nullptr, input_len,
                                            reinterpret_cast<const unsigned char *>(password), out) == 0;
  mbedtls_rsa_free(&rsa);
  return ok;
#endif
}

uint32_t key_code(uint16_t xt, bool release) {
  uint16_t code = static_cast<uint16_t>((xt & 0x37F) | (release ? 0x80 : 0));
  if (code < 0x100)
    return code;
  return 0xE0u | (static_cast<uint32_t>(code & 0xFF) << 8);
}

}

struct Channel {
  int fd{-1};
  uint8_t type{0};
  uint8_t id{0};
  bool mini{false};
  uint64_t serial{0};
  uint32_t ack_window{0};
  uint32_t ack_count{0};
  uint32_t vmc_left{0};
};

struct Rect {
  int32_t top, left, bottom, right;
};

struct Stream {
  bool used{false};
  uint32_t id{0};
  Rect dest{};
  uint32_t width{0};
  uint32_t height{0};
};

class SpiceSession {
 public:
  explicit SpiceSession(RemoteDesktop *rd) : rd_(rd) {}
  void run();

 protected:
  bool link_(Channel &ch, uint8_t type, uint8_t id, uint32_t channel_caps);
  bool send_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t len);
  bool send_parts_(Channel &ch, uint16_t type, const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);
  void playback_message_(uint16_t type, const uint8_t *body, uint32_t size);
#ifdef USE_REMOTE_DESKTOP_USB
  struct VmcLink : UsbLink {
    SpiceSession *session{nullptr};
    Channel *channel{nullptr};
    bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) override {
      return this->session->send_parts_(*this->channel, MSGC_SPICEVMC_DATA, head, head_len, data, data_len);
    }
  };
  bool usb_read_(Channel &ch, UsbSlot *slot);
#endif
  bool handle_one_(Channel &ch);
  bool handle_message_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t size);
  bool handle_channels_(const uint8_t *body, uint32_t size);
  bool display_open_();
  void display_message_(uint16_t type, const uint8_t *body, uint32_t size);
  bool base_(const uint8_t *body, uint32_t size, uint32_t *at, Rect *box, const uint8_t **clip, uint32_t *clips);
  template<typename F> void each_clip_(const Rect &box, const uint8_t *clip, uint32_t clips, F &&draw);
  void pixel_(uint32_t xrgb, uint8_t *out) const;
  void draw_fill_(const uint8_t *body, uint32_t size);
  void draw_copy_(const uint8_t *body, uint32_t size);
  void draw_bits_(const uint8_t *body, uint32_t size, uint16_t type);
  void copy_bits_(const uint8_t *body, uint32_t size);
  bool image_rows_(const uint8_t *body, uint32_t size, uint32_t image, const Rect &src, const Rect &dst,
                   const uint8_t *clip, uint32_t clips);
  void stream_create_(const uint8_t *body, uint32_t size);
  void stream_data_(const uint8_t *body, uint32_t size, bool sized);
  Stream *stream_(uint32_t id);
  bool inputs_();
  bool motion_flush_(bool force);
  bool buttons_(uint16_t now);
  void cursor_message_(uint16_t type, const uint8_t *body, uint32_t size);
  bool cursor_shape_(const uint8_t *cp, const uint8_t *end);
  void drop_(Channel &ch);
  void close_();

  RemoteDesktop *rd_;
  struct sockaddr_in addr_ {};
  Channel main_;
  Channel display_;
  Channel inputs_;
  Channel cursor_;
  Channel playback_;
  Channel usb_[RemoteDesktop::USB_SLOTS_MAX];
#ifdef USE_REMOTE_DESKTOP_USB
  VmcLink usb_link_[RemoteDesktop::USB_SLOTS_MAX];
#endif
  bool sound_ok_{false};
  bool sound_started_{false};
  uint32_t session_id_{0};
  bool channels_done_{false};
  bool client_mouse_{false};
  uint16_t mouse_supported_{0};
  uint16_t sent_buttons_{0};
  uint32_t motion_count_{0};
  int32_t motion_dx_{0}, motion_dy_{0};
  bool position_pending_{false};
  uint8_t *scratch_{nullptr};
  uint8_t *message_{nullptr};
  size_t message_cap_{0};
  uint8_t *rows_{nullptr};
  Stream streams_[STREAMS_MAX]{};
  uint32_t warned_{0};
};

static int connect_addr(const struct sockaddr_in &addr) {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int result = ::connect(fd, reinterpret_cast<const struct sockaddr *>(&addr), sizeof(addr));
  if (result < 0 && errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    struct timeval tv = {.tv_sec = CONNECT_TIMEOUT_S, .tv_usec = 0};
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (::select(fd + 1, nullptr, &writable, nullptr, &tv) == 1 &&
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0 && error == 0)
      result = 0;
  }
  if (result < 0) {
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

bool SpiceSession::link_(Channel &ch, uint8_t type, uint8_t id, uint32_t channel_caps) {
  ch.type = type;
  ch.id = id;
  ch.fd = connect_addr(this->addr_);
  if (ch.fd < 0) {
    ESP_LOGW(TAG, "%s:%u not reached", this->rd_->host_, this->rd_->port_);
    return false;
  }
  const uint32_t num_channel_caps = channel_caps != 0 ? 1 : 0;
  uint8_t out[16 + LINK_MESS_BYTES + 8];
  put32(out, SPICE_MAGIC);
  put32(out + 4, VERSION_MAJOR);
  put32(out + 8, VERSION_MINOR);
  put32(out + 12, LINK_MESS_BYTES + 4 * (1 + num_channel_caps));
  put32(out + 16, type == CHANNEL_MAIN ? 0 : this->session_id_);
  out[20] = type;
  out[21] = id;
  put32(out + 22, 1);
  put32(out + 26, num_channel_caps);
  put32(out + 30, LINK_MESS_BYTES);
  put32(out + 34, COMMON_CAP_AUTH_SELECTION | COMMON_CAP_AUTH_SPICE | COMMON_CAP_MINI_HEADER);
  put32(out + 38, channel_caps);
  if (!send_all(ch.fd, out, 38 + 4 * num_channel_caps))
    return false;
  uint8_t header[16];
  if (!recv_all(ch.fd, header, sizeof(header)) || rd32(header) != SPICE_MAGIC || rd32(header + 4) != VERSION_MAJOR) {
    ESP_LOGW(TAG, "Not a SPICE server (or another protocol version)");
    return false;
  }
  const uint32_t reply_len = rd32(header + 12);
  uint8_t *reply = this->scratch_;
  if (reply_len < LINK_REPLY_FIXED || reply_len > SCRATCH_BYTES || !recv_all(ch.fd, reply, reply_len))
    return false;
  const uint32_t error = rd32(reply);
  if (error != 0) {
    if (error == LINK_ERR_NEED_SECURED) {
      ESP_LOGW(TAG, "The server takes TLS connections only");
    } else {
      ESP_LOGW(TAG, "Link refused (%" PRIu32 ")", error);
    }
    return false;
  }
  const uint32_t num_common = rd32(reply + 4 + PUBKEY_BYTES);
  const uint32_t caps_offset = rd32(reply + 4 + PUBKEY_BYTES + 8);
  const uint32_t common = num_common > 0 && caps_offset + 4 <= reply_len ? rd32(reply + caps_offset) : 0;
  if (common & COMMON_CAP_AUTH_SELECTION) {
    if (!(common & COMMON_CAP_AUTH_SPICE)) {
      ESP_LOGW(TAG, "The server takes no SPICE ticket");
      return false;
    }
    uint8_t mechanism[4];
    put32(mechanism, AUTH_SPICE);
    if (!send_all(ch.fd, mechanism, sizeof(mechanism)))
      return false;
  }
  uint8_t ticket[TICKET_BYTES];
  if (!encrypt_ticket(reply + 4, this->rd_->password_, ticket)) {
    ESP_LOGW(TAG, "The password could not be encrypted with the server's key");
    return false;
  }
  const bool sent = send_all(ch.fd, ticket, sizeof(ticket));
  std::memset(ticket, 0, sizeof(ticket));
  uint8_t result[4];
  if (!sent || !recv_all(ch.fd, result, sizeof(result)))
    return false;
  if (rd32(result) != 0) {
    if (rd32(result) == LINK_ERR_PERMISSION_DENIED) {
      ESP_LOGW(TAG, "Wrong password");
    } else {
      ESP_LOGW(TAG, "Login refused (%" PRIu32 ")", rd32(result));
    }
    return false;
  }
  ch.mini = (common & COMMON_CAP_MINI_HEADER) != 0;
  return true;
}

bool SpiceSession::send_parts_(Channel &ch, uint16_t type, const uint8_t *a, size_t a_len, const uint8_t *b,
                               size_t b_len) {
  uint8_t out[18];
  const auto len = static_cast<uint32_t>(a_len + b_len);
  size_t at;
  if (ch.mini) {
    put16(out, type);
    put32(out + 2, len);
    at = 6;
  } else {
    put64(out, ++ch.serial);
    put16(out + 8, type);
    put32(out + 10, len);
    put32(out + 14, 0);
    at = 18;
  }
  return send_all(ch.fd, out, at) && (a_len == 0 || send_all(ch.fd, a, a_len)) &&
         (b_len == 0 || send_all(ch.fd, b, b_len));
}

bool SpiceSession::send_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t len) {
  uint8_t out[18 + 64];
  size_t at;
  if (ch.mini) {
    put16(out, type);
    put32(out + 2, len);
    at = 6;
  } else {
    put64(out, ++ch.serial);
    put16(out + 8, type);
    put32(out + 10, len);
    put32(out + 14, 0);
    at = 18;
  }
  if (len <= 64) {
    if (len != 0)
      std::memcpy(out + at, body, len);
    return send_all(ch.fd, out, at + len);
  }
  return send_all(ch.fd, out, at) && send_all(ch.fd, body, len);
}

bool SpiceSession::handle_message_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t size) {
  switch (type) {
    case MSG_SET_ACK: {
      if (size < 8)
        return true;
      uint8_t sync[4];
      std::memcpy(sync, body, 4);
      ch.ack_window = ch.ack_count = rd32(body + 4);
      return this->send_(ch, MSGC_ACK_SYNC, sync, sizeof(sync));
    }
    case MSG_PING:
      return size < 12 || this->send_(ch, MSGC_PONG, body, 12);
    case MSG_DISCONNECTING:
      ESP_LOGI(TAG, "The server disconnects");
      return true;
    case MSG_NOTIFY:
      if (size >= 24 && rd32(body + 20) <= size - 24)
        ESP_LOGI(TAG, "Server: %.*s", static_cast<int>(rd32(body + 20)), reinterpret_cast<const char *>(body + 24));
      return true;
    case MSG_LIST:
      return true;
    default:
      break;
  }
  if (&ch == &this->main_) {
    if (type == MSG_MAIN_INIT && size >= 16) {
      this->session_id_ = rd32(body);
      this->mouse_supported_ = static_cast<uint16_t>(rd32(body + 8));
      this->client_mouse_ = (rd32(body + 12) & MOUSE_MODE_CLIENT) != 0;
      if (!this->send_(ch, MSGC_MAIN_ATTACH_CHANNELS, nullptr, 0))
        return false;
      if (!this->client_mouse_ && (this->mouse_supported_ & MOUSE_MODE_CLIENT)) {
        uint8_t mode[2];
        put16(mode, MOUSE_MODE_CLIENT);
        return this->send_(ch, MSGC_MAIN_MOUSE_MODE_REQUEST, mode, sizeof(mode));
      }
      return true;
    }
    if (type == MSG_MAIN_MOUSE_MODE && size >= 4) {
      this->mouse_supported_ = rd16(body);
      this->client_mouse_ = (rd16(body + 2) & MOUSE_MODE_CLIENT) != 0;
      return true;
    }
    if (type == MSG_MAIN_CHANNELS_LIST && !this->channels_done_) {
      this->channels_done_ = true;
      return this->handle_channels_(body, size);
    }
    return true;
  }
  if (&ch == &this->inputs_) {
    if (type == MSG_INPUTS_MOUSE_MOTION_ACK) {
      this->motion_count_ = this->motion_count_ > MOTION_ACK_BUNCH ? this->motion_count_ - MOTION_ACK_BUNCH : 0;
      return this->motion_flush_(false);
    }
    return true;
  }
  if (&ch == &this->cursor_) {
    this->cursor_message_(type, body, size);
    return true;
  }
  if (&ch == &this->display_) {
    this->display_message_(type, body, size);
    return true;
  }
  if (&ch == &this->playback_) {
    this->playback_message_(type, body, size);
    return true;
  }
  return true;
}

void SpiceSession::playback_message_(uint16_t type, const uint8_t *body, uint32_t size) {
#ifdef USE_REMOTE_DESKTOP_SPEAKER
  speaker::Speaker *speaker = this->rd_->speaker_;
  if (speaker == nullptr)
    return;
  switch (type) {
    case MSG_PLAYBACK_START: {
      if (size < 10)
        return;
      const uint32_t channels = rd32(body);
      this->sound_ok_ = rd16(body + 4) == AUDIO_FMT_S16 && channels >= 1 && channels <= 2;
      if (!this->sound_ok_) {
        ESP_LOGW(TAG, "Sound of %" PRIu32 " channels in format %u is not played", channels, rd16(body + 4));
        return;
      }
      speaker->set_audio_stream_info(audio::AudioStreamInfo(16, static_cast<uint8_t>(channels), rd32(body + 6)));
      speaker->start();
      this->sound_started_ = true;
      return;
    }
    case MSG_PLAYBACK_MODE:
      if (size >= 6 && rd16(body + 4) != AUDIO_DATA_MODE_RAW) {
        this->sound_ok_ = false;
        ESP_LOGW(TAG, "Compressed sound is not played");
      }
      return;
    case MSG_PLAYBACK_DATA:
      if (this->sound_ok_ && size > 4)
        speaker->play(body + 4, size - 4);
      return;
    case MSG_PLAYBACK_STOP:
      if (this->sound_started_)
        speaker->stop();
      this->sound_started_ = false;
      return;
    default:
      return;
  }
#endif
}

#ifdef USE_REMOTE_DESKTOP_USB
bool SpiceSession::usb_read_(Channel &ch, UsbSlot *slot) {
  if (ch.vmc_left == 0)
    return this->handle_one_(ch);
  size_t room = 0;
  uint8_t *at = slot->rx_space(room);
  if (room == 0)
    return true;
  const int got = ::recv(ch.fd, at, std::min<size_t>(room, ch.vmc_left), 0);
  if (got <= 0)
    return false;
  ch.vmc_left -= static_cast<uint32_t>(got);
  return slot->rx_done(static_cast<size_t>(got));
}
#endif

bool SpiceSession::handle_one_(Channel &ch) {
  uint8_t header[18];
  const size_t header_len = ch.mini ? 6 : 18;
  if (!recv_all(ch.fd, header, header_len))
    return false;
  const uint16_t type = ch.mini ? rd16(header) : rd16(header + 8);
  const uint32_t size = ch.mini ? rd32(header + 2) : rd32(header + 10);
  if (ch.ack_count > 0 && --ch.ack_count == 0) {
    ch.ack_count = ch.ack_window;
    if (!this->send_(ch, MSGC_ACK, nullptr, 0))
      return false;
  }
  if (ch.type == CHANNEL_USBREDIR && type == MSG_SPICEVMC_DATA) {
    ch.vmc_left = size;
    return true;
  }
  uint8_t *buf = this->scratch_;
  if (size > SCRATCH_BYTES) {
    if (&ch != &this->display_ || size > DISPLAY_MESSAGE_MAX) {
      uint32_t left = size;
      while (left != 0) {
        const uint32_t take = std::min<uint32_t>(left, SCRATCH_BYTES);
        if (!recv_all(ch.fd, this->scratch_, take))
          return false;
        left -= take;
      }
      return true;
    }
    if (size > this->message_cap_) {
      auto *bigger =
          static_cast<uint8_t *>(heap_caps_realloc(this->message_, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (bigger == nullptr)
        return false;
      this->message_ = bigger;
      this->message_cap_ = size;
    }
    buf = this->message_;
  }
  return recv_all(ch.fd, buf, size) && this->handle_message_(ch, type, buf, size);
}

bool SpiceSession::display_open_() {
  const uint32_t caps = DISPLAY_CAP_SIZED_STREAM | DISPLAY_CAP_PREF_COMPRESSION | DISPLAY_CAP_MULTI_CODEC |
                        DISPLAY_CAP_CODEC_MJPEG | DISPLAY_CAP_PREF_VIDEO_CODEC_TYPE;
  if (!this->link_(this->display_, CHANNEL_DISPLAY, 0, caps))
    return false;
  uint8_t init[14]{};
  init[0] = 0;
  put64(init + 1, 0);
  init[9] = 0;
  put32(init + 10, 0);
  const uint8_t compression = IMAGE_COMPRESSION_OFF;
  const uint8_t codecs[2] = {1, VIDEO_CODEC_MJPEG};
  return this->send_(this->display_, MSGC_DISPLAY_INIT, init, sizeof(init)) &&
         this->send_(this->display_, MSGC_DISPLAY_PREFERRED_COMPRESSION, &compression, 1) &&
         this->send_(this->display_, MSGC_DISPLAY_PREFERRED_VIDEO_CODEC_TYPE, codecs, sizeof(codecs));
}

bool SpiceSession::handle_channels_(const uint8_t *body, uint32_t size) {
  if (size < 4)
    return false;
  const uint32_t count = rd32(body);
  auto listed = [body, size, count](uint8_t type, uint8_t id) {
    for (uint32_t i = 0; i < count && 4 + 2 * (i + 1) <= size; i++) {
      if (body[4 + 2 * i] == type && body[5 + 2 * i] == id)
        return true;
    }
    return false;
  };
  if (!listed(CHANNEL_DISPLAY, 0)) {
    ESP_LOGW(TAG, "The server has no display");
    return false;
  }
  if (!this->display_open_())
    return false;
  if (listed(CHANNEL_INPUTS, 0) && !this->link_(this->inputs_, CHANNEL_INPUTS, 0, 0))
    this->drop_(this->inputs_);
  if (listed(CHANNEL_CURSOR, 0) && !this->link_(this->cursor_, CHANNEL_CURSOR, 0, 0))
    this->drop_(this->cursor_);
#ifdef USE_REMOTE_DESKTOP_SPEAKER
  if (this->rd_->speaker_ != nullptr && listed(CHANNEL_PLAYBACK, 0) &&
      !this->link_(this->playback_, CHANNEL_PLAYBACK, 0, 0))
    this->drop_(this->playback_);
#endif
#ifdef USE_REMOTE_DESKTOP_USB
  for (uint8_t i = 0; i < this->rd_->usb_count_; i++) {
    if (!listed(CHANNEL_USBREDIR, i))
      continue;
    if (!this->link_(this->usb_[i], CHANNEL_USBREDIR, i, 0)) {
      this->drop_(this->usb_[i]);
      continue;
    }
    this->usb_link_[i].session = this;
    this->usb_link_[i].channel = &this->usb_[i];
    if (!this->rd_->usb_[i]->link_open(&this->usb_link_[i]))
      this->drop_(this->usb_[i]);
  }
#endif
  this->rd_->state_.store(RemoteDesktop::STATE_RUNNING);
  this->rd_->changed_();
  ESP_LOGI(TAG, "Connected to %s:%u", this->rd_->host_, this->rd_->port_);
  return true;
}

void SpiceSession::pixel_(uint32_t xrgb, uint8_t *out) const {
  const uint8_t r = static_cast<uint8_t>(xrgb >> 16);
  const uint8_t g = static_cast<uint8_t>(xrgb >> 8);
  const uint8_t b = static_cast<uint8_t>(xrgb);
  if (this->rd_->page_bpp_ == 2) {
    const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    out[0] = static_cast<uint8_t>(v);
    out[1] = static_cast<uint8_t>(v >> 8);
  } else {
    out[0] = b;
    out[1] = g;
    out[2] = r;
  }
}

bool SpiceSession::base_(const uint8_t *body, uint32_t size, uint32_t *at, Rect *box, const uint8_t **clip,
                         uint32_t *clips) {
  if (size < 21)
    return false;
  if (rd32(body) != 0)
    return false;
  box->top = static_cast<int32_t>(rd32(body + 4));
  box->left = static_cast<int32_t>(rd32(body + 8));
  box->bottom = static_cast<int32_t>(rd32(body + 12));
  box->right = static_cast<int32_t>(rd32(body + 16));
  *clip = nullptr;
  *clips = 0;
  uint32_t pos = 21;
  if (body[20] == CLIP_RECTS) {
    if (size < 25)
      return false;
    const uint32_t offset = rd32(body + 21);
    pos = 25;
    if (offset + 4 > size)
      return false;
    const uint32_t count = rd32(body + offset);
    if (offset + 4 + static_cast<uint64_t>(count) * 16 > size)
      return false;
    *clip = body + offset + 4;
    *clips = count;
    if (count == 0)
      return false;
  }
  *at = pos;
  return true;
}

template<typename F> void SpiceSession::each_clip_(const Rect &box, const uint8_t *clip, uint32_t clips, F &&draw) {
  const int32_t page_w = static_cast<int32_t>(this->rd_->page_w_);
  const int32_t page_h = static_cast<int32_t>(this->rd_->page_h_);
  auto one = [&](Rect r) {
    r.left = std::max<int32_t>(r.left, 0);
    r.top = std::max<int32_t>(r.top, 0);
    r.right = std::min<int32_t>(r.right, page_w);
    r.bottom = std::min<int32_t>(r.bottom, page_h);
    if (r.right > r.left && r.bottom > r.top)
      draw(r);
  };
  if (clip == nullptr) {
    one(box);
    return;
  }
  for (uint32_t i = 0; i != clips; i++) {
    const uint8_t *c = clip + i * 16;
    Rect r{std::max<int32_t>(box.top, static_cast<int32_t>(rd32(c))),
           std::max<int32_t>(box.left, static_cast<int32_t>(rd32(c + 4))),
           std::min<int32_t>(box.bottom, static_cast<int32_t>(rd32(c + 8))),
           std::min<int32_t>(box.right, static_cast<int32_t>(rd32(c + 12)))};
    one(r);
  }
}

void SpiceSession::draw_fill_(const uint8_t *body, uint32_t size) {
  uint32_t at = 0;
  Rect box{};
  const uint8_t *clip = nullptr;
  uint32_t clips = 0;
  if (!this->base_(body, size, &at, &box, &clip, &clips) || at + 5 > size)
    return;
  if (body[at] != BRUSH_SOLID) {
    if ((this->warned_ & 1) == 0)
      ESP_LOGW(TAG, "Pattern fills are not drawn");
    this->warned_ |= 1;
    return;
  }
  uint8_t pixel[3];
  this->pixel_(rd32(body + at + 1), pixel);
  display::VideoCanvas *canvas = this->rd_->canvas_;
  this->each_clip_(box, clip, clips, [&](const Rect &r) {
    canvas->page_fill(static_cast<uint32_t>(r.left), static_cast<uint32_t>(r.top), static_cast<uint32_t>(r.right - r.left),
                      static_cast<uint32_t>(r.bottom - r.top), pixel);
  });
}

void SpiceSession::draw_bits_(const uint8_t *body, uint32_t size, uint16_t type) {
  uint32_t at = 0;
  Rect box{};
  const uint8_t *clip = nullptr;
  uint32_t clips = 0;
  if (!this->base_(body, size, &at, &box, &clip, &clips))
    return;
  display::VideoCanvas *canvas = this->rd_->canvas_;
  const uint8_t bpp = this->rd_->page_bpp_;
  if (type != MSG_DISPLAY_DRAW_INVERS) {
    uint8_t pixel[3];
    this->pixel_(type == MSG_DISPLAY_DRAW_WHITENESS ? 0xFFFFFF : 0, pixel);
    this->each_clip_(box, clip, clips, [&](const Rect &r) {
      canvas->page_fill(static_cast<uint32_t>(r.left), static_cast<uint32_t>(r.top),
                        static_cast<uint32_t>(r.right - r.left), static_cast<uint32_t>(r.bottom - r.top), pixel);
    });
    return;
  }
  this->each_clip_(box, clip, clips, [&](const Rect &r) {
    const uint32_t w = static_cast<uint32_t>(r.right - r.left);
    for (int32_t y = r.top; y != r.bottom; y++) {
      canvas->page_read(this->rows_, static_cast<size_t>(w) * bpp, static_cast<uint32_t>(r.left),
                        static_cast<uint32_t>(y), w, 1);
      for (size_t i = 0; i != static_cast<size_t>(w) * bpp; i++)
        this->rows_[i] = static_cast<uint8_t>(~this->rows_[i]);
      canvas->page_pixels(this->rows_, static_cast<size_t>(w) * bpp, static_cast<uint32_t>(r.left),
                          static_cast<uint32_t>(y), w, 1);
    }
  });
}

void SpiceSession::copy_bits_(const uint8_t *body, uint32_t size) {
  uint32_t at = 0;
  Rect box{};
  const uint8_t *clip = nullptr;
  uint32_t clips = 0;
  if (!this->base_(body, size, &at, &box, &clip, &clips) || at + 8 > size)
    return;
  const int32_t sx = static_cast<int32_t>(rd32(body + at));
  const int32_t sy = static_cast<int32_t>(rd32(body + at + 4));
  display::VideoCanvas *canvas = this->rd_->canvas_;
  this->each_clip_(box, clip, clips, [&](const Rect &r) {
    const int32_t fx = sx + (r.left - box.left);
    const int32_t fy = sy + (r.top - box.top);
    if (fx < 0 || fy < 0)
      return;
    canvas->page_copy(static_cast<uint32_t>(fx), static_cast<uint32_t>(fy), static_cast<uint32_t>(r.left),
                      static_cast<uint32_t>(r.top), static_cast<uint32_t>(r.right - r.left),
                      static_cast<uint32_t>(r.bottom - r.top));
  });
}

bool SpiceSession::image_rows_(const uint8_t *body, uint32_t size, uint32_t image, const Rect &src, const Rect &dst,
                               const uint8_t *clip, uint32_t clips) {
  if (image + 18 > size)
    return false;
  const uint8_t type = body[image + 8];
  const uint32_t width = rd32(body + image + 10);
  const uint32_t height = rd32(body + image + 14);
  const uint8_t *u = body + image + 18;
  const uint32_t room = size - image - 18;
  display::VideoCanvas *canvas = this->rd_->canvas_;
  const uint8_t bpp = this->rd_->page_bpp_;
  if (dst.right - dst.left != src.right - src.left || dst.bottom - dst.top != src.bottom - src.top) {
    if ((this->warned_ & 2) == 0)
      ESP_LOGW(TAG, "Scaled images are not drawn");
    this->warned_ |= 2;
    return false;
  }
  if (type == IMAGE_JPEG) {
    if (room < 4 || rd32(u) > room - 4)
      return false;
    if (src.left == 0 && src.top == 0 && static_cast<uint32_t>(src.right) == width &&
        static_cast<uint32_t>(src.bottom) == height && clip == nullptr && dst.left >= 0 && dst.top >= 0 &&
        static_cast<uint32_t>(dst.right) <= this->rd_->page_w_ && static_cast<uint32_t>(dst.bottom) <= this->rd_->page_h_)
      canvas->page_rect(u + 4, rd32(u), static_cast<uint32_t>(dst.left), static_cast<uint32_t>(dst.top), width,
                        height);
    return true;
  }
  if (type != IMAGE_BITMAP) {
    if ((this->warned_ & (1u << 8 << (type & 7))) == 0)
      ESP_LOGW(TAG, "Images of type %u are not drawn yet", type);
    this->warned_ |= 1u << 8 << (type & 7);
    return false;
  }
  if (room < 14)
    return false;
  const uint8_t format = u[0];
  const uint8_t flags = u[1];
  const uint32_t rows = rd32(u + 6);
  const uint32_t stride = rd32(u + 10);
  uint32_t pos = 14;
  const uint8_t *palette = nullptr;
  uint32_t palette_n = 0;
  if (flags & BITMAP_FLAGS_PAL_FROM_CACHE) {
    pos += 8;
  } else {
    const uint32_t offset = rd32(u + 14);
    pos += 4;
    if (offset != 0 && offset + 10 <= size) {
      palette_n = rd16(body + offset + 8);
      if (offset + 10 + palette_n * 4 <= size)
        palette = body + offset + 10;
    }
  }
  if (room < pos || static_cast<uint64_t>(stride) * rows > room - pos)
    return false;
  const uint8_t *data = u + pos;
  const bool top_down = (flags & BITMAP_FLAGS_TOP_DOWN) != 0;
  auto source = [&](uint32_t x, uint32_t y) -> uint32_t {
    const uint8_t *row = data + static_cast<size_t>(top_down ? y : rows - 1 - y) * stride;
    switch (format) {
      case BITMAP_FMT_32BIT:
      case BITMAP_FMT_RGBA:
        return rd32(row + x * 4) & 0xFFFFFF;
      case BITMAP_FMT_24BIT:
        return row[x * 3] | (row[x * 3 + 1] << 8) | (row[x * 3 + 2] << 16);
      case BITMAP_FMT_16BIT: {
        const uint16_t v = rd16(row + x * 2);
        return ((v & 0x7C00) << 9) | ((v & 0x03E0) << 6) | ((v & 0x001F) << 3);
      }
      case BITMAP_FMT_8BIT:
        return palette != nullptr && row[x] < palette_n ? rd32(palette + static_cast<size_t>(row[x]) * 4) & 0xFFFFFF : 0;
      default:
        return 0;
    }
  };
  if (format != BITMAP_FMT_32BIT && format != BITMAP_FMT_RGBA && format != BITMAP_FMT_24BIT &&
      format != BITMAP_FMT_16BIT && format != BITMAP_FMT_8BIT) {
    if ((this->warned_ & 4) == 0)
      ESP_LOGW(TAG, "Bitmaps of format %u are not drawn", format);
    this->warned_ |= 4;
    return false;
  }
  this->each_clip_(dst, clip, clips, [&](const Rect &r) {
    const uint32_t w = static_cast<uint32_t>(r.right - r.left);
    for (int32_t y = r.top; y != r.bottom; y++) {
      const uint32_t sy = static_cast<uint32_t>(src.top + (y - dst.top));
      if (sy >= rows)
        continue;
      for (uint32_t i = 0; i != w; i++) {
        const uint32_t sx = static_cast<uint32_t>(src.left + (r.left - dst.left)) + i;
        this->pixel_(sx < width ? source(sx, sy) : 0, this->rows_ + static_cast<size_t>(i) * bpp);
      }
      canvas->page_pixels(this->rows_, static_cast<size_t>(w) * bpp, static_cast<uint32_t>(r.left),
                          static_cast<uint32_t>(y), w, 1);
    }
  });
  return true;
}

void SpiceSession::draw_copy_(const uint8_t *body, uint32_t size) {
  uint32_t at = 0;
  Rect box{};
  const uint8_t *clip = nullptr;
  uint32_t clips = 0;
  if (!this->base_(body, size, &at, &box, &clip, &clips) || at + 20 > size)
    return;
  const uint32_t image = rd32(body + at);
  const Rect src{static_cast<int32_t>(rd32(body + at + 4)), static_cast<int32_t>(rd32(body + at + 8)),
                 static_cast<int32_t>(rd32(body + at + 12)), static_cast<int32_t>(rd32(body + at + 16))};
  if (image == 0)
    return;
  this->image_rows_(body, size, image, src, box, clip, clips);
}

Stream *SpiceSession::stream_(uint32_t id) {
  for (Stream &s : this->streams_) {
    if (s.used && s.id == id)
      return &s;
  }
  return nullptr;
}

void SpiceSession::stream_create_(const uint8_t *body, uint32_t size) {
  if (size < 51)
    return;
  const uint32_t id = rd32(body + 4);
  const uint8_t codec = body[9];
  Stream *slot = this->stream_(id);
  for (Stream &s : this->streams_) {
    if (slot == nullptr && !s.used)
      slot = &s;
  }
  if (slot == nullptr)
    return;
  if (codec != VIDEO_CODEC_MJPEG) {
    if ((this->warned_ & 8) == 0)
      ESP_LOGW(TAG, "A video stream of codec %u is not shown", codec);
    this->warned_ |= 8;
    return;
  }
  slot->used = true;
  slot->id = id;
  slot->width = rd32(body + 18);
  slot->height = rd32(body + 22);
  slot->dest = Rect{static_cast<int32_t>(rd32(body + 34)), static_cast<int32_t>(rd32(body + 38)),
                    static_cast<int32_t>(rd32(body + 42)), static_cast<int32_t>(rd32(body + 46))};
}

void SpiceSession::stream_data_(const uint8_t *body, uint32_t size, bool sized) {
  if (size < (sized ? 36u : 12u))
    return;
  Stream *s = this->stream_(rd32(body));
  if (s == nullptr)
    return;
  uint32_t width = s->width;
  uint32_t height = s->height;
  Rect dest = s->dest;
  uint32_t at = 8;
  if (sized) {
    width = rd32(body + 8);
    height = rd32(body + 12);
    dest = Rect{static_cast<int32_t>(rd32(body + 16)), static_cast<int32_t>(rd32(body + 20)),
                static_cast<int32_t>(rd32(body + 24)), static_cast<int32_t>(rd32(body + 28))};
    at = 32;
  }
  const uint32_t len = rd32(body + at);
  if (len > size - at - 4)
    return;
  if (static_cast<uint32_t>(dest.right - dest.left) != width || static_cast<uint32_t>(dest.bottom - dest.top) != height) {
    if ((this->warned_ & 16) == 0)
      ESP_LOGW(TAG, "A scaled video stream is not shown");
    this->warned_ |= 16;
    return;
  }
  if (dest.left < 0 || dest.top < 0 || static_cast<uint32_t>(dest.right) > this->rd_->page_w_ ||
      static_cast<uint32_t>(dest.bottom) > this->rd_->page_h_)
    return;
  this->rd_->canvas_->page_rect(body + at + 4, len, static_cast<uint32_t>(dest.left), static_cast<uint32_t>(dest.top),
                                width, height);
}

void SpiceSession::display_message_(uint16_t type, const uint8_t *body, uint32_t size) {
  RemoteDesktop *rd = this->rd_;
  if (rd->canvas_ == nullptr)
    return;
  rd->cursor_hide_();
  switch (type) {
    case MSG_DISPLAY_SURFACE_CREATE:
      if (size >= 20 && (rd32(body + 16) & SURFACE_FLAGS_PRIMARY)) {
        rd->fb_w_ = rd32(body + 4);
        rd->fb_h_ = rd32(body + 8);
      }
      break;
    case MSG_DISPLAY_RESET:
      for (Stream &s : this->streams_)
        s.used = false;
      break;
    case MSG_DISPLAY_COPY_BITS:
      this->copy_bits_(body, size);
      break;
    case MSG_DISPLAY_DRAW_FILL:
      this->draw_fill_(body, size);
      break;
    case MSG_DISPLAY_DRAW_COPY:
    case MSG_DISPLAY_DRAW_OPAQUE:
    case MSG_DISPLAY_DRAW_BLEND:
      this->draw_copy_(body, size);
      break;
    case MSG_DISPLAY_DRAW_BLACKNESS:
    case MSG_DISPLAY_DRAW_WHITENESS:
    case MSG_DISPLAY_DRAW_INVERS:
      this->draw_bits_(body, size, type);
      break;
    case MSG_DISPLAY_STREAM_CREATE:
      this->stream_create_(body, size);
      break;
    case MSG_DISPLAY_STREAM_DATA:
      this->stream_data_(body, size, false);
      break;
    case MSG_DISPLAY_STREAM_DATA_SIZED:
      this->stream_data_(body, size, true);
      break;
    case MSG_DISPLAY_STREAM_DESTROY:
      if (size >= 4) {
        Stream *s = this->stream_(rd32(body));
        if (s != nullptr)
          s->used = false;
      }
      break;
    case MSG_DISPLAY_STREAM_DESTROY_ALL:
      for (Stream &s : this->streams_)
        s.used = false;
      break;
    default:
      break;
  }
  rd->cursor_show_();
  rd->canvas_->page_show();
}

bool SpiceSession::motion_flush_(bool force) {
  if (this->inputs_.fd < 0)
    return true;
  if (!force && this->motion_count_ >= MOTION_ACK_BUNCH * 2)
    return true;
  RemoteDesktop *rd = this->rd_;
  if (this->client_mouse_ && this->position_pending_) {
    uint8_t msg[11];
    put32(msg, static_cast<uint32_t>(rd->pointer_x_));
    put32(msg + 4, static_cast<uint32_t>(rd->pointer_y_));
    put16(msg + 8, this->sent_buttons_);
    msg[10] = 0;
    this->position_pending_ = false;
    this->motion_count_++;
    return this->send_(this->inputs_, MSGC_INPUTS_MOUSE_POSITION, msg, sizeof(msg));
  }
  if (!this->client_mouse_ && (this->motion_dx_ != 0 || this->motion_dy_ != 0)) {
    uint8_t msg[10];
    put32(msg, static_cast<uint32_t>(this->motion_dx_));
    put32(msg + 4, static_cast<uint32_t>(this->motion_dy_));
    put16(msg + 8, this->sent_buttons_);
    this->motion_dx_ = this->motion_dy_ = 0;
    this->motion_count_++;
    return this->send_(this->inputs_, MSGC_INPUTS_MOUSE_MOTION, msg, sizeof(msg));
  }
  return true;
}

bool SpiceSession::buttons_(uint16_t now) {
  if (this->inputs_.fd < 0 || now == this->sent_buttons_)
    return true;
  if (!this->motion_flush_(true))
    return false;
  for (uint8_t button : {BUTTON_LEFT, BUTTON_MIDDLE, BUTTON_RIGHT}) {
    const uint16_t bit = button_mask(button);
    if ((now & bit) == (this->sent_buttons_ & bit))
      continue;
    this->sent_buttons_ = static_cast<uint16_t>((this->sent_buttons_ & ~bit) | (now & bit));
    uint8_t msg[3];
    msg[0] = button;
    put16(msg + 1, this->sent_buttons_);
    if (!this->send_(this->inputs_, (now & bit) ? MSGC_INPUTS_MOUSE_PRESS : MSGC_INPUTS_MOUSE_RELEASE, msg,
                     sizeof(msg)))
      return false;
  }
  return true;
}

bool SpiceSession::inputs_() {
  RemoteDesktop *rd = this->rd_;
  RemoteDesktop::Input input;
  bool moved = false;
  const int32_t max_x = static_cast<int32_t>(std::min(rd->fb_w_, rd->page_w_)) - 1;
  const int32_t max_y = static_cast<int32_t>(std::min(rd->fb_h_, rd->page_h_)) - 1;
  while (xQueueReceive(rd->inputs_q_, &input, 0) == pdTRUE) {
    if (this->inputs_.fd < 0)
      continue;
    if (input.kind == RemoteDesktop::INPUT_KEY) {
      if (input.code >= sizeof(KEYBOARD_XT) / sizeof(KEYBOARD_XT[0]) || KEYBOARD_XT[input.code] == 0)
        continue;
      uint8_t code[4];
      put32(code, key_code(KEYBOARD_XT[input.code], input.buttons == 0));
      if (!this->send_(this->inputs_, input.buttons != 0 ? MSGC_INPUTS_KEY_DOWN : MSGC_INPUTS_KEY_UP, code,
                       sizeof(code)))
        return false;
      continue;
    }
    if (input.kind == RemoteDesktop::INPUT_POINTER) {
      rd->pointer_x_ = std::max<int32_t>(0, std::min<int32_t>(max_x, rd->pointer_x_ + input.x));
      rd->pointer_y_ = std::max<int32_t>(0, std::min<int32_t>(max_y, rd->pointer_y_ + input.y));
      this->motion_dx_ += input.x;
      this->motion_dy_ += input.y;
    } else if (input.x >= 0 && input.y >= 0) {
      rd->pointer_x_ = std::min<int32_t>(max_x, input.x);
      rd->pointer_y_ = std::min<int32_t>(max_y, input.y);
    }
    this->position_pending_ = true;
    moved = true;
    if (!this->motion_flush_(false) || !this->buttons_(input.buttons & 0x07))
      return false;
    if (input.kind == RemoteDesktop::INPUT_POINTER && (input.code & 0x18) != 0) {
      const uint8_t button = (input.code & 0x08) ? BUTTON_UP : BUTTON_DOWN;
      uint8_t msg[3];
      msg[0] = button;
      put16(msg + 1, static_cast<uint16_t>(this->sent_buttons_ | button_mask(button)));
      if (!this->send_(this->inputs_, MSGC_INPUTS_MOUSE_PRESS, msg, sizeof(msg)))
        return false;
      put16(msg + 1, this->sent_buttons_);
      if (!this->send_(this->inputs_, MSGC_INPUTS_MOUSE_RELEASE, msg, sizeof(msg)))
        return false;
    }
  }
  if (moved && this->client_mouse_ && rd->cursor_w_ != 0 && rd->canvas_ != nullptr) {
    rd->cursor_hide_();
    rd->cursor_show_();
    rd->canvas_->page_show();
  }
  return true;
}

bool SpiceSession::cursor_shape_(const uint8_t *cp, const uint8_t *end) {
  RemoteDesktop *rd = this->rd_;
  if (end - cp < 2)
    return false;
  const uint16_t flags = rd16(cp);
  if ((flags & CURSOR_FLAGS_NONE) || (flags & CURSOR_FLAGS_FROM_CACHE) || end - cp < 19)
    return false;
  const uint8_t type = cp[10];
  const uint16_t w = rd16(cp + 11);
  const uint16_t h = rd16(cp + 13);
  const uint8_t *data = cp + 19;
  const size_t len = static_cast<size_t>(end - data);
  const uint32_t cw = std::min<uint32_t>(w, RemoteDesktop::CURSOR_MAX);
  const uint32_t ch = std::min<uint32_t>(h, RemoteDesktop::CURSOR_MAX);
  const size_t bpl = (static_cast<size_t>(w) + 7) / 8;
  const uint32_t mask_row = (cw + 7) / 8;
  if (w == 0 || h == 0)
    return false;
  if ((type == CURSOR_TYPE_ALPHA && len < static_cast<size_t>(w) * h * 4) ||
      (type == CURSOR_TYPE_MONO && len < 2 * bpl * h) ||
      (type == CURSOR_TYPE_COLOR32 && len < static_cast<size_t>(w) * h * 4 + bpl * h) ||
      (type != CURSOR_TYPE_ALPHA && type != CURSOR_TYPE_MONO && type != CURSOR_TYPE_COLOR32))
    return false;
  std::memset(rd->cursor_mask_, 0, static_cast<size_t>(mask_row) * ch);
  auto bit = [](const uint8_t *mask, size_t row_bytes, uint32_t x, uint32_t y) {
    return (mask[y * row_bytes + x / 8] & (0x80 >> (x % 8))) != 0;
  };
  for (uint32_t y = 0; y != ch; y++) {
    for (uint32_t x = 0; x != cw; x++) {
      bool shown = false;
      uint32_t color = 0;
      if (type == CURSOR_TYPE_ALPHA) {
        const uint32_t v = rd32(data + (static_cast<size_t>(y) * w + x) * 4);
        shown = (v >> 24) >= 0x80;
        color = v & 0xFFFFFF;
      } else if (type == CURSOR_TYPE_MONO) {
        const bool a = bit(data, bpl, x, y);
        const bool o = bit(data + bpl * h, bpl, x, y);
        shown = !a || o;
        color = (!a && o) ? 0xFFFFFF : 0;
      } else {
        const bool m = bit(data + static_cast<size_t>(w) * h * 4, bpl, x, y);
        shown = !m;
        color = rd32(data + (static_cast<size_t>(y) * w + x) * 4) & 0xFFFFFF;
      }
      if (shown)
        rd->cursor_mask_[y * mask_row + x / 8] |= static_cast<uint8_t>(0x80 >> (x & 7));
      this->pixel_(color, rd->cursor_pixels_ + (static_cast<size_t>(y) * cw + x) * rd->page_bpp_);
    }
  }
  rd->cursor_w_ = cw;
  rd->cursor_h_ = ch;
  rd->cursor_hot_x_ = std::min<uint32_t>(rd16(cp + 15), cw);
  rd->cursor_hot_y_ = std::min<uint32_t>(rd16(cp + 17), ch);
  return true;
}

void SpiceSession::cursor_message_(uint16_t type, const uint8_t *body, uint32_t size) {
  RemoteDesktop *rd = this->rd_;
  if (rd->canvas_ == nullptr)
    return;
  rd->cursor_hide_();
  switch (type) {
    case MSG_CURSOR_INIT:
    case MSG_CURSOR_SET: {
      const uint32_t at = type == MSG_CURSOR_INIT ? 9 : 5;
      if (size < at)
        break;
      if (!this->client_mouse_) {
        rd->pointer_x_ = static_cast<int16_t>(rd16(body));
        rd->pointer_y_ = static_cast<int16_t>(rd16(body + 2));
      }
      if (body[at - 1] == 0 || !this->cursor_shape_(body + at, body + size))
        rd->cursor_w_ = 0;
      break;
    }
    case MSG_CURSOR_MOVE:
      if (size >= 4 && !this->client_mouse_) {
        rd->pointer_x_ = static_cast<int16_t>(rd16(body));
        rd->pointer_y_ = static_cast<int16_t>(rd16(body + 2));
      }
      break;
    case MSG_CURSOR_HIDE:
    case MSG_CURSOR_RESET:
      rd->cursor_w_ = 0;
      break;
    default:
      break;
  }
  rd->cursor_show_();
  rd->canvas_->page_show();
}

void SpiceSession::drop_(Channel &ch) {
  if (ch.fd < 0)
    return;
  ESP_LOGW(TAG, "Channel %u:%u ended", ch.type, ch.id);
  ::close(ch.fd);
  ch.fd = -1;
  ch.vmc_left = 0;
#ifdef USE_REMOTE_DESKTOP_USB
  for (uint8_t i = 0; i < this->rd_->usb_count_; i++) {
    if (&ch == &this->usb_[i])
      this->rd_->usb_[i]->link_close();
  }
#endif
}

void SpiceSession::close_() {
#ifdef USE_REMOTE_DESKTOP_USB
  for (uint8_t i = 0; i < this->rd_->usb_count_; i++) {
    if (this->usb_[i].fd >= 0)
      this->rd_->usb_[i]->link_close();
  }
#endif
#ifdef USE_REMOTE_DESKTOP_SPEAKER
  if (this->sound_started_ && this->rd_->speaker_ != nullptr)
    this->rd_->speaker_->stop();
  this->sound_started_ = false;
#endif
  for (Channel &ch : this->usb_) {
    if (ch.fd >= 0) {
      ::close(ch.fd);
      ch.fd = -1;
    }
  }
  for (Channel *ch : {&this->main_, &this->display_, &this->inputs_, &this->cursor_, &this->playback_}) {
    if (ch->fd >= 0) {
      ::close(ch->fd);
      ch->fd = -1;
    }
  }
}

void SpiceSession::run() {
  RemoteDesktop *rd = this->rd_;
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  char port_text[6];
  snprintf(port_text, sizeof(port_text), "%u", rd->port_);
  if (::getaddrinfo(rd->host_, port_text, &hints, &result) != 0 || result == nullptr) {
    ESP_LOGW(TAG, "%s not found", rd->host_);
    return;
  }
  std::memcpy(&this->addr_, result->ai_addr, sizeof(this->addr_));
  ::freeaddrinfo(result);
  rd->canvas_ = rd->output_->open_page(rd);
  if (rd->canvas_ == nullptr) {
    ESP_LOGW(TAG, "No canvas free for the desktop");
    return;
  }
  rd->page_w_ = rd->canvas_->width();
  rd->page_h_ = rd->canvas_->height();
  rd->page_bpp_ = display::VideoCanvas::page_bpp();
  rd->fb_w_ = rd->page_w_;
  rd->fb_h_ = rd->page_h_;
  rd->pointer_x_ = static_cast<int32_t>(rd->page_w_ / 2);
  rd->pointer_y_ = static_cast<int32_t>(rd->page_h_ / 2);
  rd->cursor_w_ = 0;
  rd->cursor_shown_ = false;
  this->scratch_ = static_cast<uint8_t *>(heap_caps_malloc(SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->rows_ = static_cast<uint8_t *>(
      heap_caps_malloc(static_cast<size_t>(rd->page_w_) * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const size_t cursor = static_cast<size_t>(RemoteDesktop::CURSOR_MAX) * RemoteDesktop::CURSOR_MAX;
  rd->cursor_pixels_ = static_cast<uint8_t *>(heap_caps_malloc(cursor * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rd->cursor_mask_ = static_cast<uint8_t *>(heap_caps_malloc(cursor / 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rd->cursor_under_ = static_cast<uint8_t *>(heap_caps_malloc(cursor * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  rd->out_ = static_cast<uint8_t *>(heap_caps_malloc(cursor * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (this->scratch_ != nullptr && this->rows_ != nullptr && rd->cursor_pixels_ != nullptr &&
      rd->cursor_mask_ != nullptr && rd->cursor_under_ != nullptr && rd->out_ != nullptr &&
      this->link_(this->main_, CHANNEL_MAIN, 0, 0)) {
    Channel *const others[] = {&this->display_, &this->inputs_, &this->cursor_, &this->playback_};
    while (!rd->stop_.load()) {
      fd_set readable;
      FD_ZERO(&readable);
      int top = rd->wake_fd_;
      FD_SET(top, &readable);
      FD_SET(this->main_.fd, &readable);
      top = std::max(top, this->main_.fd);
      for (Channel *ch : others) {
        if (ch->fd >= 0) {
          FD_SET(ch->fd, &readable);
          top = std::max(top, ch->fd);
        }
      }
#ifdef USE_REMOTE_DESKTOP_USB
      for (uint8_t i = 0; i < rd->usb_count_; i++) {
        if (this->usb_[i].fd < 0)
          continue;
        size_t room = 1;
        if (this->usb_[i].vmc_left > 0)
          rd->usb_[i]->rx_space(room);
        if (room > 0) {
          FD_SET(this->usb_[i].fd, &readable);
          top = std::max(top, this->usb_[i].fd);
        }
      }
#endif
      if (::select(top + 1, &readable, nullptr, nullptr, nullptr) < 0)
        break;
      if (FD_ISSET(rd->wake_fd_, &readable)) {
        uint64_t count = 0;
        ::read(rd->wake_fd_, &count, sizeof(count));
        if (!this->inputs_())
          this->drop_(this->inputs_);
      }
      if (rd->stop_.load())
        break;
      if (FD_ISSET(this->main_.fd, &readable) && !this->handle_one_(this->main_))
        break;
      if (this->display_.fd >= 0 && FD_ISSET(this->display_.fd, &readable) && !this->handle_one_(this->display_))
        break;
      for (Channel *ch : {&this->inputs_, &this->cursor_, &this->playback_}) {
        if (ch->fd >= 0 && FD_ISSET(ch->fd, &readable) && !this->handle_one_(*ch))
          this->drop_(*ch);
      }
#ifdef USE_REMOTE_DESKTOP_USB
      for (uint8_t i = 0; i < rd->usb_count_; i++) {
        Channel &ch = this->usb_[i];
        if (ch.fd < 0)
          continue;
        if ((FD_ISSET(ch.fd, &readable) && !this->usb_read_(ch, rd->usb_[i])) || !rd->usb_[i]->service())
          this->drop_(ch);
      }
#endif
    }
  } else {
    ESP_LOGW(TAG, "No SPICE session with %s:%u", rd->host_, rd->port_);
  }
  this->close_();
  heap_caps_free(this->scratch_);
  heap_caps_free(this->message_);
  heap_caps_free(this->rows_);
  this->scratch_ = nullptr;
  this->message_ = nullptr;
  this->rows_ = nullptr;
  rd->free_();
  rd->cursor_w_ = 0;
  rd->cursor_shown_ = false;
  ESP_LOGI(TAG, "Disconnected from %s:%u", rd->host_, rd->port_);
}

void RemoteDesktop::spice_session_() {
  RAMAllocator<SpiceSession> allocator(RAMAllocator<SpiceSession>::ALLOC_EXTERNAL);
  SpiceSession *session = allocator.allocate(1);
  if (session == nullptr)
    return;
  new (session) SpiceSession(this);
  session->run();
  session->~SpiceSession();
  allocator.deallocate(session, 1);
}

}

#endif
