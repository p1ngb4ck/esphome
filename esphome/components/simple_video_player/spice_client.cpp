// SPICE client: a SPICE server's desktop (QEMU/libvirt, x11spice, ...) on the stream's output, drawn
// by the stream draw task like udisp rectangles (stream_receiver.cpp). The wire layouts follow
// spice-protocol (spice/protocol.h, spice/enums.h) and the demarshallers spice-common's
// spice_codegen.py generates from spice.proto.
//
// Pictures drawn: JPEG and JPEG_ALPHA (its JPEG part) of DRAW_COPY, MJPEG streams, and plain 32-bit
// bitmaps. The client asks for QUIC compression, which the server turns into JPEG where it may be
// lossy -- with jpeg-wan-compression=always (dcc_compress_image()). Other images (QUIC, LZ, GLZ, LZ4)
// are not drawn. The desktop is drawn as it comes: it should be the output's size.

#include "simple_video_player.h"

#ifdef SVP_SPICE

#include "esphome/core/log.h"
#include "esp_random.h"
#include "esp_idf_version.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
// mbedtls 4 (IDF 6) made the RSA API private: PSA, initialized by ESP-IDF at startup.
#define SVP_SPICE_PSA
#include <psa/crypto.h>
#else
#include <mbedtls/rsa.h>
#endif

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.spice";

namespace {

constexpr uint32_t SPICE_MAGIC = 0x51444552;  // "REDQ"
constexpr uint32_t SPICE_VERSION_MAJOR = 2;
constexpr uint32_t SPICE_VERSION_MINOR = 2;
constexpr size_t PUBKEY_BYTES = 162;  // SPICE_TICKET_PUBKEY_BYTES
constexpr size_t TICKET_BYTES = 128;  // SPICE_TICKET_KEY_PAIR_LENGTH / 8
constexpr size_t PASSWORD_MAX = 60;   // SPICE_MAX_PASSWORD_LENGTH
constexpr size_t LINK_MESS_BYTES = 18;
constexpr size_t LINK_REPLY_FIXED = 4 + PUBKEY_BYTES + 12;
constexpr uint8_t CHANNEL_MAIN = 1;
constexpr uint8_t CHANNEL_DISPLAY = 2;
constexpr uint32_t COMMON_CAP_AUTH_SELECTION = 1u << 0;
constexpr uint32_t COMMON_CAP_AUTH_SPICE = 1u << 1;
constexpr uint32_t COMMON_CAP_MINI_HEADER = 1u << 3;
constexpr uint32_t AUTH_SPICE = 1;  // SPICE_COMMON_CAP_AUTH_SPICE
// No MULTI_CODEC: the server then streams MJPEG only (dcc_create_video_encoder()).
constexpr uint32_t DISPLAY_CAPS = (1u << 0) | (1u << 6);  // SIZED_STREAM, PREF_COMPRESSION
constexpr uint32_t LINK_ERR_NEED_SECURED = 5;
constexpr uint32_t LINK_ERR_PERMISSION_DENIED = 7;

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
constexpr uint16_t MSGC_MAIN_ATTACH_CHANNELS = 104;
constexpr uint16_t MSG_DISPLAY_COPY_BITS = 104;
constexpr uint16_t MSG_DISPLAY_STREAM_CREATE = 122;
constexpr uint16_t MSG_DISPLAY_STREAM_DATA = 123;
constexpr uint16_t MSG_DISPLAY_STREAM_CLIP = 124;
constexpr uint16_t MSG_DISPLAY_STREAM_DESTROY = 125;
constexpr uint16_t MSG_DISPLAY_STREAM_DESTROY_ALL = 126;
constexpr uint16_t MSG_DISPLAY_DRAW_FILL = 302;
constexpr uint16_t MSG_DISPLAY_DRAW_COPY = 304;
constexpr uint16_t MSG_DISPLAY_DRAW_LAST = 313;  // DRAW_ALPHA_BLEND
constexpr uint16_t MSG_DISPLAY_SURFACE_CREATE = 314;
constexpr uint16_t MSG_DISPLAY_SURFACE_DESTROY = 315;
constexpr uint16_t MSG_DISPLAY_STREAM_DATA_SIZED = 316;
constexpr uint16_t MSG_DISPLAY_DRAW_COMPOSITE = 318;
constexpr uint16_t MSGC_DISPLAY_INIT = 101;
constexpr uint16_t MSGC_DISPLAY_PREFERRED_COMPRESSION = 103;

constexpr uint8_t IMAGE_TYPE_BITMAP = 0;
constexpr uint8_t IMAGE_TYPE_JPEG = 105;
constexpr uint8_t IMAGE_TYPE_JPEG_ALPHA = 108;
constexpr uint8_t BITMAP_FMT_32BIT = 8;
constexpr uint8_t BITMAP_FMT_RGBA = 9;
constexpr uint8_t BITMAP_FLAGS_PAL_FROM_CACHE = 1u << 1;
constexpr uint8_t BITMAP_FLAGS_TOP_DOWN = 1u << 2;
constexpr uint16_t ROPD_OP_PUT = 1u << 3;
constexpr uint8_t CLIP_TYPE_RECTS = 1;
constexpr uint8_t STREAM_FLAGS_TOP_DOWN = 1u << 0;
constexpr uint8_t VIDEO_CODEC_MJPEG = 1;
constexpr uint8_t IMAGE_COMPRESSION_QUIC = 4;
constexpr uint32_t SURFACE_FLAGS_PRIMARY = 1u << 0;

constexpr uint32_t CONNECT_TIMEOUT_S = 5;
constexpr uint32_t RECV_TIMEOUT_S = 30;
constexpr uint32_t SEND_TIMEOUT_S = 10;
constexpr uint32_t TAKEOVER_WAIT_MS = 3000;
constexpr uint32_t FRAME_WAIT_MS = 100;

// Warned once a session.
enum : uint32_t {
  WARN_IMAGE = 1u << 0,
  WARN_DRAW = 1u << 1,
  WARN_SCALED = 1u << 2,
  WARN_TOO_LARGE = 1u << 3,
  WARN_CLIPS = 1u << 4,
  WARN_COPY_BITS = 1u << 5,
  WARN_CODEC = 1u << 6,
  WARN_BOTTOM_UP = 1u << 7,
  WARN_SUB_LIST = 1u << 8,
  WARN_SURFACE = 1u << 9,
};

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

struct Rect {
  int32_t left, top, right, bottom;
  bool empty() const { return right <= left || bottom <= top; }
  Rect cut(const Rect &o) const {
    return {std::max(left, o.left), std::max(top, o.top), std::min(right, o.right), std::min(bottom, o.bottom)};
  }
};
// SpiceRect on the wire: top, left, bottom, right.
inline Rect rd_rect(const uint8_t *p) {
  return {static_cast<int32_t>(rd32(p + 4)), static_cast<int32_t>(rd32(p)), static_cast<int32_t>(rd32(p + 12)),
          static_cast<int32_t>(rd32(p + 8))};
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

// One DER element of the given tag at p: its content; p moves past it.
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

#ifndef SVP_SPICE_PSA
int spice_random(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}
#endif

// The password, NUL included, RSA-OAEP (SHA-1) encrypted with the server's key: a DER
// SubjectPublicKeyInfo (spice_channel_send_spice_ticket(), spice-gtk).
bool encrypt_ticket(const uint8_t *spki, const char *password, uint8_t *out) {
  const uint8_t *p = spki, *end = spki + PUBKEY_BYTES, *seq, *alg, *bits;
  size_t seq_len, alg_len, bits_len;
  if (!der_take(p, end, 0x30, seq, seq_len))
    return false;
  const uint8_t *q = seq, *seq_end = seq + seq_len;
  if (!der_take(q, seq_end, 0x30, alg, alg_len) || !der_take(q, seq_end, 0x03, bits, bits_len) || bits_len < 2 ||
      bits[0] != 0)
    return false;
  const uint8_t *rsa_key = bits + 1;  // RSAPublicKey
  const size_t rsa_key_len = bits_len - 1;
  const size_t input_len = std::strlen(password) + 1;
#ifdef SVP_SPICE_PSA
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

struct Channel {
  int fd{-1};
  bool mini{false};
  uint64_t serial{0};
  uint32_t ack_window{0};
  uint32_t ack_count{0};
};

}  // namespace

class SpiceSession {
 public:
  using StreamFrame = SimpleVideoPlayer::StreamFrame;
  using StreamRect = SimpleVideoPlayer::StreamRect;
  using SpiceStreamInfo = SimpleVideoPlayer::SpiceStreamInfo;

  explicit SpiceSession(SimpleVideoPlayer *player) : p_(player) {}
  ~SpiceSession() { this->close_(); }
  void run(const char *host, uint16_t port, const char *password);

 protected:
  bool resolve_(const char *host, uint16_t port);
  int connect_();
  bool link_(Channel &ch, uint8_t type, uint32_t channel_caps);
  bool send_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t len);
  bool skip_(Channel &ch, uint32_t len);
  bool handle_one_(Channel &ch);
  bool handle_message_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t size);
  bool handle_list_(Channel &ch, const uint8_t *body, uint32_t size);
  bool handle_picture_(Channel &ch, uint16_t type, uint32_t size, const uint8_t *body);
  bool open_display_();
  bool take_panel_();
  void release_panel_();
  bool draw_copy_(StreamFrame *f, uint32_t size);
  bool stream_data_(StreamFrame *f, uint32_t size, bool sized);
  void stream_create_(const uint8_t *body, uint32_t size);
  void stream_clip_(SpiceStreamInfo &stream, const uint8_t *clip, const uint8_t *end);
  bool parts_(StreamFrame *f, const Rect &area, const uint8_t *wire_rects, uint32_t wire_count,
              const StreamRect *stored, uint8_t stored_count);
  bool fits_output_(uint32_t w, uint32_t h) const;
  /// Whether this is the first time this session (for a warning given once).
  bool first_(uint32_t what);
  bool stopping_() const { return this->p_->spice_request_.load(std::memory_order_acquire) != 0; }
  void close_();

  SimpleVideoPlayer *p_;
  Channel main_;
  Channel display_;
  struct sockaddr_in addr_ {};
  const char *password_{nullptr};
  uint32_t session_id_{0};
  int64_t surface_id_{-1};
  uint32_t out_w_{0}, out_h_{0};
  bool direct_{false};
  uint32_t warned_{0};
  bool panel_held_{false};
  bool wifi_held_{false};
};

bool SpiceSession::first_(uint32_t what) {
  if (this->warned_ & what)
    return false;
  this->warned_ |= what;
  return true;
}

bool SpiceSession::resolve_(const char *host, uint16_t port) {
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  char port_text[6];
  std::snprintf(port_text, sizeof(port_text), "%u", port);
  if (::getaddrinfo(host, port_text, &hints, &result) != 0 || result == nullptr) {
    ESP_LOGE(TAG, "%s not found", host);
    return false;
  }
  std::memcpy(&this->addr_, result->ai_addr, sizeof(this->addr_));
  ::freeaddrinfo(result);
  return true;
}

int SpiceSession::connect_() {
  int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int result = ::connect(fd, reinterpret_cast<struct sockaddr *>(&this->addr_), sizeof(this->addr_));
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

// The link (spice_channel_send_link(), spice_channel_recv_link_msg(), spice_channel_recv_auth()).
bool SpiceSession::link_(Channel &ch, uint8_t type, uint32_t channel_caps) {
  ch.fd = this->connect_();
  if (ch.fd < 0) {
    ESP_LOGE(TAG, "Could not connect to %s:%u", inet_ntoa(this->addr_.sin_addr), ntohs(this->addr_.sin_port));
    return false;
  }
  const uint32_t num_channel_caps = channel_caps != 0 ? 1 : 0;
  uint8_t out[16 + LINK_MESS_BYTES + 8];
  put32(out, SPICE_MAGIC);
  put32(out + 4, SPICE_VERSION_MAJOR);
  put32(out + 8, SPICE_VERSION_MINOR);
  put32(out + 12, LINK_MESS_BYTES + 4 * (1 + num_channel_caps));
  put32(out + 16, type == CHANNEL_MAIN ? 0 : this->session_id_);
  out[20] = type;
  out[21] = 0;  // channel id
  put32(out + 22, 1);
  put32(out + 26, num_channel_caps);
  put32(out + 30, LINK_MESS_BYTES);
  put32(out + 34, COMMON_CAP_AUTH_SELECTION | COMMON_CAP_AUTH_SPICE | COMMON_CAP_MINI_HEADER);
  put32(out + 38, channel_caps);
  if (!send_all(ch.fd, out, 38 + 4 * num_channel_caps))
    return false;

  uint8_t header[16];
  if (!recv_all(ch.fd, header, sizeof(header)) || rd32(header) != SPICE_MAGIC ||
      rd32(header + 4) != SPICE_VERSION_MAJOR) {
    ESP_LOGE(TAG, "Not a SPICE server (or another protocol version)");
    return false;
  }
  const uint32_t reply_len = rd32(header + 12);
  uint8_t *reply = this->p_->spice_scratch_;
  if (reply_len < LINK_REPLY_FIXED || reply_len > SimpleVideoPlayer::SPICE_SCRATCH_BYTES ||
      !recv_all(ch.fd, reply, reply_len))
    return false;
  const uint32_t error = rd32(reply);
  if (error != 0) {
    if (error == LINK_ERR_NEED_SECURED) {
      ESP_LOGE(TAG, "The server takes TLS connections only (not supported here)");
    } else {
      ESP_LOGE(TAG, "Link refused (%" PRIu32 ")", error);
    }
    return false;
  }
  const uint32_t num_common = rd32(reply + 4 + PUBKEY_BYTES);
  const uint32_t caps_offset = rd32(reply + 4 + PUBKEY_BYTES + 8);
  const uint32_t common = num_common > 0 && caps_offset + 4 <= reply_len ? rd32(reply + caps_offset) : 0;
  if (common & COMMON_CAP_AUTH_SELECTION) {
    if (!(common & COMMON_CAP_AUTH_SPICE)) {
      ESP_LOGE(TAG, "The server takes no SPICE ticket (SASL only?)");
      return false;
    }
    uint8_t mechanism[4];
    put32(mechanism, AUTH_SPICE);
    if (!send_all(ch.fd, mechanism, sizeof(mechanism)))
      return false;
  }
  uint8_t ticket[TICKET_BYTES];
  if (!encrypt_ticket(reply + 4, this->password_, ticket)) {
    ESP_LOGE(TAG, "Could not encrypt the password with the server's key");
    return false;
  }
  const bool sent = send_all(ch.fd, ticket, sizeof(ticket));
  std::memset(ticket, 0, sizeof(ticket));
  uint8_t result[4];
  if (!sent || !recv_all(ch.fd, result, sizeof(result)))
    return false;
  if (rd32(result) != 0) {
    if (rd32(result) == LINK_ERR_PERMISSION_DENIED) {
      ESP_LOGE(TAG, "Wrong password");
    } else {
      ESP_LOGE(TAG, "Login refused (%" PRIu32 ")", rd32(result));
    }
    return false;
  }
  ch.mini = (common & COMMON_CAP_MINI_HEADER) != 0;
  return true;
}

bool SpiceSession::send_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t len) {
  uint8_t out[18 + 16];
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
  if (len <= 16) {
    std::memcpy(out + at, body, len);
    return send_all(ch.fd, out, at + len);
  }
  return send_all(ch.fd, out, at) && send_all(ch.fd, body, len);
}

bool SpiceSession::skip_(Channel &ch, uint32_t len) {
  while (len > 0) {
    const uint32_t take = std::min<uint32_t>(len, SimpleVideoPlayer::SPICE_SCRATCH_BYTES);
    if (!recv_all(ch.fd, this->p_->spice_scratch_, take))
      return false;
    len -= take;
  }
  return true;
}

bool SpiceSession::fits_output_(uint32_t w, uint32_t h) const {
  // Decoded rows and columns come padded to 16 into an output-sized buffer.
  return ((w + 15) & ~15u) * ((h + 15) & ~15u) <= ((this->out_w_ + 15) & ~15u) * ((this->out_h_ + 15) & ~15u);
}

// The parts of a picture that show: area (where it goes), cut by the clip rectangles (on the wire or
// a stream's), the output and the picture itself. false: none.
bool SpiceSession::parts_(StreamFrame *f, const Rect &area, const uint8_t *wire_rects, uint32_t wire_count,
                          const StreamRect *stored, uint8_t stored_count) {
  const Rect picture{f->x, f->y, f->x + f->w, f->y + f->h};
  const Rect output{0, 0, static_cast<int32_t>(this->out_w_), static_cast<int32_t>(this->out_h_)};
  const Rect base = area.cut(picture).cut(output);
  f->clip_count = 0;
  if (base.empty())
    return false;
  auto add = [f, &base](const Rect &r) {
    const Rect part = r.cut(base);
    if (part.empty())
      return true;
    if (f->clip_count == SimpleVideoPlayer::SPICE_MAX_CLIPS)
      return false;
    f->clips[f->clip_count++] = {static_cast<int16_t>(part.left), static_cast<int16_t>(part.top),
                                 static_cast<int16_t>(part.right - part.left),
                                 static_cast<int16_t>(part.bottom - part.top)};
    return true;
  };
  bool all_fit = true;
  if (wire_rects != nullptr) {
    for (uint32_t i = 0; i < wire_count && all_fit; i++)
      all_fit = add(rd_rect(wire_rects + 16 * i));
  } else if (stored != nullptr && stored_count > 0) {
    for (uint8_t i = 0; i < stored_count && all_fit; i++)
      all_fit = add({stored[i].x, stored[i].y, stored[i].x + stored[i].w, stored[i].y + stored[i].h});
  } else {
    add(base);
  }
  if (!all_fit) {
    if (this->first_(WARN_CLIPS))
      ESP_LOGW(TAG, "A picture in more than %u parts: drawn whole (it may cover a window over it)",
               SimpleVideoPlayer::SPICE_MAX_CLIPS);
    f->clip_count = 0;
    add(base);
  }
  return f->clip_count > 0;
}

// DRAW_COPY: DisplayBase, then Copy (src_bitmap, src_area, rop_descriptor, scale_mode, mask).
bool SpiceSession::draw_copy_(StreamFrame *f, uint32_t size) {
  const uint8_t *b = f->data;
  if (size < 21 || static_cast<int64_t>(rd32(b)) != this->surface_id_)
    return false;  // an off-screen surface: not shown
  const Rect box = rd_rect(b + 4);
  uint32_t at = 21, clip_count = 0;
  const uint8_t *clips = nullptr;
  if (b[20] == CLIP_TYPE_RECTS) {
    if (size < 25)
      return false;
    clip_count = rd32(b + 21);
    if (clip_count > (size - 25) / 16)
      return false;
    clips = b + 25;
    at = 25 + 16 * clip_count;
  }
  if (at + 36 > size)
    return false;
  const uint32_t image = rd32(b + at);
  const Rect src = rd_rect(b + at + 4);
  if (rd16(b + at + 20) != ROPD_OP_PUT || rd32(b + at + 32) != 0) {
    if (this->first_(WARN_DRAW))
      ESP_LOGW(TAG, "A masked or blended copy is not drawn");
    return false;
  }
  if (box.right - box.left != src.right - src.left || box.bottom - box.top != src.bottom - src.top) {
    if (this->first_(WARN_SCALED))
      ESP_LOGW(TAG, "A scaled copy is not drawn");
    return false;
  }
  if (image == 0 || image > size || size - image < 18)
    return false;
  const uint8_t *img = b + image;
  const uint8_t type = img[8];
  const uint32_t w = rd32(img + 10), h = rd32(img + 14);
  if (w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF)
    return false;
  uint32_t data_at, data_len;
  if (type == IMAGE_TYPE_JPEG) {
    if (size - image < 22)
      return false;
    data_len = rd32(img + 18);
    data_at = image + 22;
    f->kind = SimpleVideoPlayer::STREAM_PICTURE_JPEG;
  } else if (type == IMAGE_TYPE_JPEG_ALPHA) {
    // The JPEG, then the alpha plane (LZ): a desktop is opaque, the JPEG is all of it.
    if (size - image < 27)
      return false;
    data_len = rd32(img + 19);
    data_at = image + 27;
    f->kind = SimpleVideoPlayer::STREAM_PICTURE_JPEG;
  } else if (type == IMAGE_TYPE_BITMAP) {
    if (size - image < 32)
      return false;
    const uint8_t format = img[18], flags = img[19];
    const uint32_t stride = rd32(img + 28);
    if (format != BITMAP_FMT_32BIT && format != BITMAP_FMT_RGBA) {
      if (this->first_(WARN_IMAGE))
        ESP_LOGW(TAG, "Bitmaps of format %u are not drawn", format);
      return false;
    }
    if (rd32(img + 20) != w || rd32(img + 24) != h || stride < 4 * w)
      return false;
    data_at = image + 32 + ((flags & BITMAP_FLAGS_PAL_FROM_CACHE) ? 8 : 4);
    data_len = stride * h;
    f->kind = SimpleVideoPlayer::STREAM_PICTURE_BGRX;
    f->stride = stride;
    f->bottom_up = (flags & BITMAP_FLAGS_TOP_DOWN) == 0;
  } else {
    if (this->first_(WARN_IMAGE))
      ESP_LOGW(TAG,
               "Images of type %u are not drawn: start the server with image-compression=quic and "
               "jpeg-wan-compression=always",
               type);
    return false;
  }
  if (data_at > size || data_len > size - data_at)
    return false;
  if (!this->fits_output_(w, h)) {
    if (this->first_(WARN_TOO_LARGE))
      ESP_LOGW(TAG, "A picture larger than the output is not drawn");
    return false;
  }
  f->x = box.left - src.left;
  f->y = box.top - src.top;
  f->w = static_cast<uint16_t>(w);
  f->h = static_cast<uint16_t>(h);
  if (!this->parts_(f, box, clips, clip_count, nullptr, 0))
    return false;
  // The picture to the start of the buffer: the decoder reads it from there.
  std::memmove(f->data, f->data + data_at, data_len);
  f->received = data_len;
  f->total = data_len;
  return true;
}

// STREAM_DATA: id, multi_media_time, data_size, data. STREAM_DATA_SIZED: id, multi_media_time,
// width, height, dest, data_size, data.
bool SpiceSession::stream_data_(StreamFrame *f, uint32_t size, bool sized) {
  const uint8_t *b = f->data;
  const uint32_t header = sized ? 36 : 12;
  if (size < header)
    return false;
  const uint32_t id = rd32(b);
  if (id >= SimpleVideoPlayer::SPICE_MAX_STREAMS || !this->p_->spice_->streams[id].used)
    return false;
  const SpiceStreamInfo &stream = this->p_->spice_->streams[id];
  Rect dest{stream.left, stream.top, stream.right, stream.bottom};
  uint32_t w = stream.width, h = stream.height;
  if (sized) {
    w = rd32(b + 8);
    h = rd32(b + 12);
    dest = rd_rect(b + 16);
  }
  const uint32_t data_len = rd32(b + header - 4);
  if (data_len > size - header || w == 0 || h == 0 || w > 0x7FFF || h > 0x7FFF)
    return false;
  if (!stream.top_down) {
    if (this->first_(WARN_BOTTOM_UP))
      ESP_LOGW(TAG, "A bottom-up stream is not drawn");
    return false;
  }
  if (static_cast<int64_t>(dest.right) - dest.left != w || static_cast<int64_t>(dest.bottom) - dest.top != h) {
    if (this->first_(WARN_SCALED))
      ESP_LOGW(TAG, "A scaled stream is not drawn");
    return false;
  }
  if (!this->fits_output_(w, h)) {
    if (this->first_(WARN_TOO_LARGE))
      ESP_LOGW(TAG, "A picture larger than the output is not drawn");
    return false;
  }
  f->kind = SimpleVideoPlayer::STREAM_PICTURE_JPEG;
  f->x = dest.left;
  f->y = dest.top;
  f->w = static_cast<uint16_t>(w);
  f->h = static_cast<uint16_t>(h);
  if (!this->parts_(f, dest, nullptr, 0, stream.clips, stream.clip_count))
    return false;
  std::memmove(f->data, f->data + header, data_len);
  f->received = data_len;
  f->total = data_len;
  return true;
}

void SpiceSession::stream_clip_(SpiceStreamInfo &stream, const uint8_t *clip, const uint8_t *end) {
  stream.clip_count = 0;
  if (end - clip < 1 || clip[0] != CLIP_TYPE_RECTS)
    return;
  if (end - clip < 5)
    return;
  const uint32_t count = rd32(clip + 1);
  if (count > static_cast<uint32_t>(end - clip - 5) / 16)
    return;
  if (count == 0) {
    stream.clip_count = 0;  // empty: nothing shows
    stream.used = false;
    return;
  }
  if (count > SimpleVideoPlayer::SPICE_MAX_CLIPS) {
    if (this->first_(WARN_CLIPS))
      ESP_LOGW(TAG, "A stream in more than %u parts: drawn whole (it may cover a window over it)",
               SimpleVideoPlayer::SPICE_MAX_CLIPS);
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    const Rect r = rd_rect(clip + 5 + 16 * i);
    stream.clips[i] = {static_cast<int16_t>(r.left), static_cast<int16_t>(r.top),
                       static_cast<int16_t>(r.right - r.left), static_cast<int16_t>(r.bottom - r.top)};
  }
  stream.clip_count = static_cast<uint8_t>(count);
}

// STREAM_CREATE: surface_id, id, flags, codec_type, stamp, stream_width, stream_height, src_width,
// src_height, dest, clip.
void SpiceSession::stream_create_(const uint8_t *body, uint32_t size) {
  if (size < 51)
    return;
  const uint32_t id = rd32(body + 4);
  if (id >= SimpleVideoPlayer::SPICE_MAX_STREAMS)
    return;
  SpiceStreamInfo &stream = this->p_->spice_->streams[id];
  stream.used = false;
  if (static_cast<int64_t>(rd32(body)) != this->surface_id_)
    return;
  if (body[9] != VIDEO_CODEC_MJPEG) {
    if (this->first_(WARN_CODEC))
      ESP_LOGW(TAG, "A stream in codec %u is not drawn", body[9]);
    return;
  }
  stream.top_down = (body[8] & STREAM_FLAGS_TOP_DOWN) != 0;
  stream.width = rd32(body + 18);
  stream.height = rd32(body + 22);
  const Rect dest = rd_rect(body + 34);
  stream.left = dest.left;
  stream.top = dest.top;
  stream.right = dest.right;
  stream.bottom = dest.bottom;
  stream.used = true;
  this->stream_clip_(stream, body + 50, body + size);
}

bool SpiceSession::handle_list_(Channel &ch, const uint8_t *body, uint32_t size) {
  // SpiceSubMessageList: size:2, then the offsets of SpiceSubMessage (type:2, size:4, data).
  if (size < 2)
    return true;
  const uint16_t count = rd16(body);
  for (uint16_t i = 0; i < count; i++) {
    if (2 + 4u * (i + 1) > size)
      return true;
    const uint32_t at = rd32(body + 2 + 4 * i);
    if (at > size || size - at < 6)
      return true;
    const uint16_t type = rd16(body + at);
    const uint32_t len = rd32(body + at + 2);
    if (len > size - at - 6)
      return true;
    if (!this->handle_message_(ch, type, body + at + 6, len))
      return false;
  }
  return true;
}

// A picture message into a stream frame and to the draw task. body: the message already read
// (inside a list), else nullptr and it is read from the socket.
bool SpiceSession::handle_picture_(Channel &ch, uint16_t type, uint32_t size, const uint8_t *body) {
  StreamFrame *f = nullptr;
  if (size <= this->p_->stream_max_frame_bytes_) {
    while (xQueueReceive(this->p_->stream_empty_q_, &f, pdMS_TO_TICKS(FRAME_WAIT_MS)) != pdTRUE) {
      if (this->stopping_())
        return body != nullptr || this->skip_(ch, size);
    }
  }
  if (f == nullptr) {
    if (this->first_(WARN_TOO_LARGE))
      ESP_LOGW(TAG, "A picture of more than %u bytes is not drawn (psram_pool_size sets the limit)",
               static_cast<unsigned>(this->p_->stream_max_frame_bytes_));
    return body != nullptr || this->skip_(ch, size);
  }
  bool ok = true;
  if (body != nullptr) {
    std::memcpy(f->data, body, size);
  } else {
    ok = recv_all(ch.fd, f->data, size);
  }
  bool draw = false;
  if (ok) {
    f->direct = this->direct_;
    if (type == MSG_DISPLAY_DRAW_COPY) {
      draw = this->draw_copy_(f, size);
    } else {
      draw = this->stream_data_(f, size, type == MSG_DISPLAY_STREAM_DATA_SIZED);
    }
  }
  if (draw) {
    xQueueSend(this->p_->stream_filled_q_, &f, portMAX_DELAY);
  } else {
    xQueueSend(this->p_->stream_empty_q_, &f, 0);
  }
  return ok;
}

bool SpiceSession::handle_message_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t size) {
  switch (type) {
    case MSG_SET_ACK: {
      if (size < 8)
        return true;
      uint8_t sync[4];
      std::memcpy(sync, body, 4);  // generation
      ch.ack_window = ch.ack_count = rd32(body + 4);
      return this->send_(ch, MSGC_ACK_SYNC, sync, sizeof(sync));
    }
    case MSG_PING:
      // Answered with its id and timestamp (spice_channel_handle_ping()).
      return size < 12 || this->send_(ch, MSGC_PONG, body, 12);
    case MSG_DISCONNECTING:
      ESP_LOGI(TAG, "The server disconnects (%" PRIu32 ")", size >= 12 ? rd32(body + 8) : 0);
      return true;
    case MSG_NOTIFY:
      if (size >= 24 && rd32(body + 20) <= size - 24)
        ESP_LOGI(TAG, "Server: %.*s", static_cast<int>(rd32(body + 20)), reinterpret_cast<const char *>(body + 24));
      return true;
    case MSG_LIST:
      return this->handle_list_(ch, body, size);
    default:
      break;
  }
  if (&ch == &this->main_) {
    if (type == MSG_MAIN_INIT && size >= 4) {
      this->session_id_ = rd32(body);
      return this->send_(ch, MSGC_MAIN_ATTACH_CHANNELS, nullptr, 0);
    }
    if (type == MSG_MAIN_CHANNELS_LIST && size >= 4 && this->display_.fd < 0) {
      const uint32_t count = rd32(body);
      for (uint32_t i = 0; i < count && 4 + 2 * (i + 1) <= size; i++) {
        if (body[4 + 2 * i] == CHANNEL_DISPLAY && body[5 + 2 * i] == 0)
          return this->open_display_();
      }
      ESP_LOGE(TAG, "The server has no display");
      return false;
    }
    return true;
  }
  switch (type) {
    case MSG_DISPLAY_SURFACE_CREATE:
      // surface_id, width, height, format, flags
      if (size >= 20 && (rd32(body + 16) & SURFACE_FLAGS_PRIMARY)) {
        this->surface_id_ = rd32(body);
        const uint32_t w = rd32(body + 4), h = rd32(body + 8);
        if ((w != this->out_w_ || h != this->out_h_) && this->first_(WARN_SURFACE))
          ESP_LOGW(TAG, "The desktop is not the output's size: only what fits shows");
        ESP_LOGI(TAG, "Desktop %" PRIu32 "x%" PRIu32 ", output %" PRIu32 "x%" PRIu32, w, h, this->out_w_, this->out_h_);
      }
      return true;
    case MSG_DISPLAY_SURFACE_DESTROY:
      if (size >= 4 && static_cast<int64_t>(rd32(body)) == this->surface_id_)
        this->surface_id_ = -1;
      return true;
    case MSG_DISPLAY_STREAM_CREATE:
      this->stream_create_(body, size);
      return true;
    case MSG_DISPLAY_STREAM_CLIP:
      if (size >= 5 && rd32(body) < SimpleVideoPlayer::SPICE_MAX_STREAMS) {
        SpiceStreamInfo &stream = this->p_->spice_->streams[rd32(body)];
        if (stream.used)
          this->stream_clip_(stream, body + 4, body + size);
      }
      return true;
    case MSG_DISPLAY_STREAM_DESTROY:
      if (size >= 4 && rd32(body) < SimpleVideoPlayer::SPICE_MAX_STREAMS)
        this->p_->spice_->streams[rd32(body)].used = false;
      return true;
    case MSG_DISPLAY_STREAM_DESTROY_ALL:
      for (auto &stream : this->p_->spice_->streams)
        stream.used = false;
      return true;
    case MSG_DISPLAY_DRAW_COPY:
    case MSG_DISPLAY_STREAM_DATA:
    case MSG_DISPLAY_STREAM_DATA_SIZED:
      return this->handle_picture_(ch, type, size, body);
    case MSG_DISPLAY_COPY_BITS:
      if (this->first_(WARN_COPY_BITS))
        ESP_LOGW(TAG, "COPY_BITS is not drawn");
      return true;
    default:
      if (((type >= MSG_DISPLAY_DRAW_FILL && type <= MSG_DISPLAY_DRAW_LAST) || type == MSG_DISPLAY_DRAW_COMPOSITE) &&
          this->first_(WARN_DRAW))
        ESP_LOGW(TAG, "Drawing command %u is not drawn", type);
      return true;  // marks, resets, cache and monitor messages: nothing to do here
  }
}

bool SpiceSession::handle_one_(Channel &ch) {
  uint8_t header[18];
  const size_t header_len = ch.mini ? 6 : 18;
  if (!recv_all(ch.fd, header, header_len))
    return false;
  const uint16_t type = ch.mini ? rd16(header) : rd16(header + 8);
  const uint32_t size = ch.mini ? rd32(header + 2) : rd32(header + 10);
  if (!ch.mini && rd32(header + 14) != 0 && this->first_(WARN_SUB_LIST))
    ESP_LOGW(TAG, "Sub-messages of a message are not read");
  // Counted as it arrives, before it is handled (spice_channel_recv_msg()): a SET_ACK starts the count.
  if (ch.ack_count > 0 && --ch.ack_count == 0) {
    ch.ack_count = ch.ack_window;
    if (!this->send_(ch, MSGC_ACK, nullptr, 0))
      return false;
  }
  bool ok;
  if (&ch == &this->display_ &&
      (type == MSG_DISPLAY_DRAW_COPY || type == MSG_DISPLAY_STREAM_DATA || type == MSG_DISPLAY_STREAM_DATA_SIZED)) {
    ok = this->handle_picture_(ch, type, size, nullptr);
  } else if (size <= SimpleVideoPlayer::SPICE_SCRATCH_BYTES) {
    ok = recv_all(ch.fd, this->p_->spice_scratch_, size) &&
         this->handle_message_(ch, type, this->p_->spice_scratch_, size);
  } else {
    ok = this->skip_(ch, size);
  }
  return ok;
}

bool SpiceSession::open_display_() {
  if (!this->link_(this->display_, CHANNEL_DISPLAY, DISPLAY_CAPS))
    return false;
  // No image cache and no GLZ window: nothing comes from a cache (dcc_handle_init()).
  uint8_t init[14] = {};
  init[0] = 1;  // pixmap_cache_id; pixmap_cache_size 0
  init[9] = 1;  // glz_dictionary_id; glz_dictionary_window_size 0
  const uint8_t compression = IMAGE_COMPRESSION_QUIC;
  if (!this->send_(this->display_, MSGC_DISPLAY_INIT, init, sizeof(init)) ||
      !this->send_(this->display_, MSGC_DISPLAY_PREFERRED_COMPRESSION, &compression, 1))
    return false;
  return this->take_panel_();
}

// The panel to this session, before everything else: a playing video ends, a stream sender is let
// go, and both are refused from then on (spice_refuses_(), the network task); LVGL pauses for it
// (loop()) as for a sender. Music of the audio queue goes on.
bool SpiceSession::take_panel_() {
  SimpleVideoPlayer *p = this->p_;
  p->spice_on_.store(true, std::memory_order_release);
  p->start_pending_.store(false, std::memory_order_release);
  if (p->picture_session_busy_())
    p->end_session_();
  p->stream_drop_sender_.store(true, std::memory_order_release);
  p->stream_wake_();
  for (const uint32_t start = millis();
       p->stream_sender_busy_.load(std::memory_order_acquire) || p->picture_session_busy_();) {
    if (millis() - start > TAKEOVER_WAIT_MS) {
      ESP_LOGE(TAG, "The playing video or stream sender did not let go of the panel");
      p->stream_drop_sender_.store(false, std::memory_order_release);
      p->spice_on_.store(false, std::memory_order_release);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  p->stream_drop_sender_.store(false, std::memory_order_release);
  for (auto &stream : p->spice_->streams)
    stream.used = false;
  p->stream_connected_.store(true, std::memory_order_release);
  this->panel_held_ = true;
  SimpleVideoPlayer::hold_wifi_(this->wifi_held_, true);
  ESP_LOGI(TAG, "Connected to %s:%u", inet_ntoa(this->addr_.sin_addr), ntohs(this->addr_.sin_port));
  return true;
}

void SpiceSession::release_panel_() {
  SimpleVideoPlayer *p = this->p_;
  SimpleVideoPlayer::hold_wifi_(this->wifi_held_, false);
  if (this->panel_held_) {
    this->panel_held_ = false;
    p->stream_connected_.store(false, std::memory_order_release);
    if (p->stream_draw_task_ != nullptr)
      xTaskNotifyGive(p->stream_draw_task_);  // no longer waits for the output
    App.wake_loop_threadsafe();
  }
  p->spice_on_.store(false, std::memory_order_release);
}

void SpiceSession::close_() {
  for (Channel *ch : {&this->main_, &this->display_}) {
    if (ch->fd >= 0) {
      ::close(ch->fd);
      ch->fd = -1;
    }
  }
}

void SpiceSession::run(const char *host, uint16_t port, const char *password) {
  SimpleVideoPlayer *p = this->p_;
  this->password_ = password;
#ifdef SVP_DSI_OUTPUT
  if (p->stream_direct_()) {
    this->direct_ = true;
    this->out_w_ = p->dsi_out_w_;
    this->out_h_ = p->dsi_out_h_;
#ifdef SVP_USE_LVGL
    if (p->lvgl_component_ != nullptr && p->lvgl_component_->get_rotation() != display::DISPLAY_ROTATION_0_DEGREES)
      ESP_LOGW(TAG, "The panel is turned: the desktop shows on its native %" PRIu32 "x%" PRIu32 " unturned",
               this->out_w_, this->out_h_);
#endif
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  if (!p->stream_direct_()) {
    this->direct_ = false;
    this->out_w_ = p->canvas_size_w_;
    this->out_h_ = p->canvas_size_h_;
  }
#endif
  ESP_LOGI(TAG, "Connecting to %s:%u", host, port);
  if (!this->resolve_(host, port) || !this->link_(this->main_, CHANNEL_MAIN, 0)) {
    this->close_();
    return;
  }
  while (true) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(p->spice_wake_fd_, &readable);
    FD_SET(this->main_.fd, &readable);
    int top = std::max(p->spice_wake_fd_, this->main_.fd);
    if (this->display_.fd >= 0) {
      FD_SET(this->display_.fd, &readable);
      top = std::max(top, this->display_.fd);
    }
    if (::select(top + 1, &readable, nullptr, nullptr, nullptr) < 0)
      break;
    if (FD_ISSET(p->spice_wake_fd_, &readable)) {
      uint64_t count;
      ::read(p->spice_wake_fd_, &count, sizeof(count));
    }
    if (this->stopping_())
      break;
    if (FD_ISSET(this->main_.fd, &readable) && !this->handle_one_(this->main_))
      break;
    if (this->display_.fd >= 0 && FD_ISSET(this->display_.fd, &readable) && !this->handle_one_(this->display_))
      break;
  }
  ESP_LOGI(TAG, "Disconnected");
  this->close_();
  this->release_panel_();
}

bool SimpleVideoPlayer::spice_refuses_() const {
  if (!this->spice_on_.load(std::memory_order_acquire))
    return false;
  ESP_LOGW(TAG, "Refused: a SPICE session holds the panel");
  return true;
}

void SimpleVideoPlayer::spice_connect(const std::string &host, uint16_t port, const std::string &password) {
  if (this->spice_task_ == nullptr) {
    ESP_LOGE(TAG, "The SPICE client is not running");
    return;
  }
  if (this->spice_on_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "A SPICE session runs: disconnect it first");
    return;
  }
  if (password.size() > PASSWORD_MAX) {
    ESP_LOGE(TAG, "The password is longer than %u characters", static_cast<unsigned>(PASSWORD_MAX));
    return;
  }
  xSemaphoreTake(this->spice_lock_, portMAX_DELAY);
  std::snprintf(this->spice_->host, sizeof(this->spice_->host), "%s", host.c_str());
  std::snprintf(this->spice_->password, sizeof(this->spice_->password), "%s", password.c_str());
  this->spice_->port = port;
  xSemaphoreGive(this->spice_lock_);
  // A connection still being made ends first; the task then connects anew.
  this->spice_request_.store(1, std::memory_order_release);
  const uint64_t one = 1;
  ::write(this->spice_wake_fd_, &one, sizeof(one));
  xTaskNotifyGive(this->spice_task_);
}

void SimpleVideoPlayer::spice_disconnect() {
  if (this->spice_task_ == nullptr)
    return;
  this->spice_request_.store(2, std::memory_order_release);
  const uint64_t one = 1;
  ::write(this->spice_wake_fd_, &one, sizeof(one));
  xTaskNotifyGive(this->spice_task_);
}

void SimpleVideoPlayer::spice_task_entry_(void *param) { static_cast<SimpleVideoPlayer *>(param)->spice_loop_(); }

void SimpleVideoPlayer::spice_loop_() {
  char host[sizeof(SpiceState::host)];
  char password[sizeof(SpiceState::password)];
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (this->spice_request_.exchange(0, std::memory_order_acq_rel) != 1)
      continue;
    xSemaphoreTake(this->spice_lock_, portMAX_DELAY);
    std::memcpy(host, this->spice_->host, sizeof(host));
    std::memcpy(password, this->spice_->password, sizeof(password));
    const uint16_t port = this->spice_->port;
    xSemaphoreGive(this->spice_lock_);
    {
      // The wake count of the request itself.
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(this->spice_wake_fd_, &readable);
      struct timeval none = {.tv_sec = 0, .tv_usec = 0};
      if (::select(this->spice_wake_fd_ + 1, &readable, nullptr, nullptr, &none) == 1) {
        uint64_t count;
        ::read(this->spice_wake_fd_, &count, sizeof(count));
      }
    }
    SpiceSession session(this);
    session.run(host, port, password);
    std::memset(password, 0, sizeof(password));
  }
}

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE
