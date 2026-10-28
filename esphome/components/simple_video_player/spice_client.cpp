// SPICE client: a SPICE server's desktop (QEMU/libvirt, x11spice, ...) on the stream's output. Its
// display channel is svp_relay's, which joins the session and sends whole JPEGs of the output's size
// as the stream's sender (open_display_()); a VNC server's desktop comes all through the relay
// (desktop_vnc_()). Here: the session's inputs channel fed with the
// panel's keys, mouse and touches, its cursor drawn over the desktop by the draw task, its sound played
// on the speaker, and USB devices redirected through its usbredir channels (select/spice_usb.cpp). The
// wire layouts follow spice-protocol (spice/protocol.h, spice/enums.h) and the demarshallers
// spice-common's spice_codegen.py generates from spice.proto; the behaviour spice-gtk's channels.

#include "simple_video_player.h"

#ifdef SVP_SPICE

#ifdef SVP_SPICE_USB
#include "select/spice_usb.h"
#endif
#include "esp_cache.h"

#include "esphome/core/log.h"
#include "esphome/components/storage/storage_worker.h"
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

#ifdef SVP_DESKTOP_SPICE
constexpr uint32_t SPICE_MAGIC = 0x51444552;  // "REDQ"
constexpr uint32_t SPICE_VERSION_MAJOR = 2;
constexpr uint32_t SPICE_VERSION_MINOR = 2;
constexpr size_t PUBKEY_BYTES = 162;  // SPICE_TICKET_PUBKEY_BYTES
constexpr size_t TICKET_BYTES = 128;  // SPICE_TICKET_KEY_PAIR_LENGTH / 8
#endif
constexpr size_t PASSWORD_MAX = 60;   // SPICE_MAX_PASSWORD_LENGTH
#ifdef SVP_DESKTOP_SPICE
constexpr size_t LINK_MESS_BYTES = 18;
constexpr size_t LINK_REPLY_FIXED = 4 + PUBKEY_BYTES + 12;
constexpr uint8_t CHANNEL_MAIN = 1;
constexpr uint8_t CHANNEL_DISPLAY = 2;
constexpr uint8_t CHANNEL_INPUTS = 3;
constexpr uint8_t CHANNEL_CURSOR = 4;
constexpr uint8_t CHANNEL_PLAYBACK = 5;
constexpr uint8_t CHANNEL_USBREDIR = 9;
constexpr uint32_t COMMON_CAP_AUTH_SELECTION = 1u << 0;
constexpr uint32_t COMMON_CAP_AUTH_SPICE = 1u << 1;
constexpr uint32_t COMMON_CAP_MINI_HEADER = 1u << 3;
constexpr uint32_t AUTH_SPICE = 1;  // SPICE_COMMON_CAP_AUTH_SPICE
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
constexpr uint32_t MOTION_ACK_BUNCH = 4;  // SPICE_INPUT_MOTION_ACK_BUNCH
constexpr uint16_t MSG_CURSOR_INIT = 101;
constexpr uint16_t MSG_CURSOR_RESET = 102;
constexpr uint16_t MSG_CURSOR_SET = 103;
constexpr uint16_t MSG_CURSOR_MOVE = 104;
constexpr uint16_t MSG_CURSOR_HIDE = 105;
constexpr uint16_t MSG_CURSOR_INVAL_ONE = 107;
constexpr uint16_t MSG_CURSOR_INVAL_ALL = 108;
constexpr uint8_t CURSOR_TYPE_ALPHA = 0;
constexpr uint8_t CURSOR_TYPE_MONO = 1;
constexpr uint8_t CURSOR_TYPE_COLOR4 = 2;
constexpr uint8_t CURSOR_TYPE_COLOR16 = 4;
constexpr uint8_t CURSOR_TYPE_COLOR32 = 6;
constexpr uint16_t CURSOR_FLAGS_NONE = 1u << 0;
constexpr uint16_t CURSOR_FLAGS_CACHE_ME = 1u << 1;
constexpr uint16_t CURSOR_FLAGS_FROM_CACHE = 1u << 2;
constexpr uint16_t MSG_PLAYBACK_DATA = 101;
constexpr uint16_t MSG_PLAYBACK_MODE = 102;
constexpr uint16_t MSG_PLAYBACK_START = 103;
constexpr uint16_t AUDIO_DATA_MODE_RAW = 1;
constexpr uint16_t AUDIO_FMT_S16 = 1;
constexpr uint16_t MSG_SPICEVMC_DATA = 101;
constexpr uint16_t MSGC_SPICEVMC_DATA = 101;
// SPICE_MOUSE_BUTTON_* and their masks.
constexpr uint8_t BUTTON_LEFT = 1, BUTTON_MIDDLE = 2, BUTTON_RIGHT = 3, BUTTON_UP = 4, BUTTON_DOWN = 5, BUTTON_SIDE = 6,
                  BUTTON_EXTRA = 7;
constexpr uint16_t button_mask(uint8_t button) { return static_cast<uint16_t>(1u << (button - 1)); }
#endif
constexpr uint32_t CONNECT_TIMEOUT_S = 5;
constexpr uint32_t RECV_TIMEOUT_S = 30;
constexpr uint32_t SEND_TIMEOUT_S = 10;
constexpr uint32_t TAKEOVER_WAIT_MS = 3000;

// Warned once a session.
enum : uint32_t {
  WARN_SUB_LIST = 1u << 8,
  WARN_CURSOR = 1u << 10,
  WARN_CURSOR_CACHE = 1u << 11,
  WARN_SOUND = 1u << 12,
  WARN_USB = 1u << 13,
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
inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
inline void put64(uint8_t *p, uint64_t v) {
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

#ifdef SVP_DESKTOP_SPICE
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
  uint8_t type{0};
  uint8_t id{0};
  bool mini{false};
  uint64_t serial{0};
  uint32_t ack_window{0};
  uint32_t ack_count{0};
  uint32_t vmc_left{0};  // usbredir: bytes of the data message being read into its slot
};

// HID keyboard usages (page 7) to PC XT scancodes, 0x100 for those with the 0xE0 prefix: keycodemapdb's
// keymaps.csv ("USB Keycodes" to "AT set1 keycode"), as spice-gtk sends them (its xtkbd keymap).
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

// HID consumer usages (page 0x0C), the same way (Linux hid-input's mapping, keycodemapdb's codes).
struct ConsumerKey {
  uint16_t usage, xt;
};
const ConsumerKey CONSUMER_XT[] = {
    {0x0030, 0x15E}, {0x0032, 0x15F}, {0x0040, 0x11E}, {0x00B5, 0x119}, {0x00B6, 0x110}, {0x00B7, 0x124},
    {0x00CD, 0x122}, {0x00E2, 0x120}, {0x00E9, 0x130}, {0x00EA, 0x12E}, {0x0183, 0x16D}, {0x018A, 0x16C},
    {0x0192, 0x121}, {0x0194, 0x16B}, {0x0221, 0x165}, {0x0223, 0x132}, {0x0224, 0x16A}, {0x0225, 0x169},
    {0x0226, 0x168}, {0x0227, 0x167}, {0x022A, 0x166},
};

// SpiceMsgcKeyDown/Up code (spice_make_scancode(), spice-gtk): the 0xE0 prefix in the low byte.
uint32_t key_code(uint16_t xt, bool release) {
  uint16_t code = static_cast<uint16_t>((xt & 0x37F) | (release ? 0x80 : 0));
  if (code < 0x100)
    return code;
  return 0xE0u | (static_cast<uint32_t>(code & 0xFF) << 8);
}
#endif

bool resolve_host(const char *host, uint16_t port, struct sockaddr_in &addr) {
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
  std::memcpy(&addr, result->ai_addr, sizeof(addr));
  ::freeaddrinfo(result);
  return true;
}

// A TCP connection to addr, its timeouts set; -1: none.
int connect_to(const struct sockaddr_in &addr) {
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

}  // namespace

#ifdef SVP_DESKTOP_SPICE
class SpiceSession {
 public:
  using StreamFrame = SimpleVideoPlayer::StreamFrame;
#ifdef SVP_STREAM_TOUCH
  using StreamTouch = SimpleVideoPlayer::StreamTouch;
#endif
  using SpiceCursorShape = SimpleVideoPlayer::SpiceCursorShape;

  explicit SpiceSession(SimpleVideoPlayer *player) : p_(player) {}
  ~SpiceSession() { this->close_(); }
  void run(const char *host, uint16_t port, const char *password);
  bool took_panel() const { return this->took_panel_; }

 protected:
#ifdef SVP_SPICE_USB
  /// A slot's bytes into its usbredir channel.
  struct VmcLink : SpiceVmcLink {
    SpiceSession *session{nullptr};
    Channel *channel{nullptr};
    bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) override {
      return this->session->send_parts_(*this->channel, MSGC_SPICEVMC_DATA, head, head_len, data, data_len);
    }
  };
#endif
  static constexpr uint8_t USB_CHANNELS = 4;

  bool resolve_(const char *host, uint16_t port);
  int connect_();
  bool link_(Channel &ch, uint8_t type, uint8_t id, uint32_t channel_caps);
  bool send_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t len);
  bool send_parts_(Channel &ch, uint16_t type, const uint8_t *a, size_t a_len, const uint8_t *b, size_t b_len);
  bool skip_(Channel &ch, uint32_t len);
  bool handle_one_(Channel &ch);
  bool handle_message_(Channel &ch, uint16_t type, const uint8_t *body, uint32_t size);
  bool handle_list_(Channel &ch, const uint8_t *body, uint32_t size);
  bool handle_channels_(const uint8_t *body, uint32_t size);
  bool open_display_();
  bool take_panel_();
  void release_panel_();
  // Inputs.
  bool inputs_pending_();
#ifdef SVP_STREAM_INPUT
  bool key_(const uint8_t *message);
  bool send_key_(uint16_t xt, bool pressed);
  bool pointer_(const uint8_t *message);
#endif
  /// The ms until the close key or two fingers held end the session (UINT32_MAX: none held); 0: now.
  uint32_t close_due_() const;
#ifdef SVP_STREAM_TOUCH
  bool touch_(const StreamTouch &touch);
#endif
  /// The pointer to (x, y) and buttons: a position in client mouse mode, else a motion of dx, dy.
  bool move_(int32_t x, int32_t y, int32_t dx, int32_t dy);
  bool buttons_(uint16_t now);
  /// What moved and was not sent yet; force: also beyond the unacknowledged limit (before a button).
  bool motion_flush_(bool force = false);
  // Cursor.
  void cursor_message_(uint16_t type, const uint8_t *body, uint32_t size);
  /// The SpiceCursor at c (flags, header, data to end): the shape shown, or false (hidden).
  bool cursor_shape_(const uint8_t *c, const uint8_t *end);
  bool cursor_convert_(SpiceCursorShape &out, uint8_t type, uint16_t w, uint16_t h, const uint8_t *data, size_t len);
  void cursor_changed_();
  // Sound.
  void playback_message_(uint16_t type, const uint8_t *body, uint32_t size);
#ifdef SVP_SPICE_USB
  bool usb_read_(Channel &ch, SpiceUsbSlot *slot);
#endif
  /// A channel other than main and display ends; the session goes on without it.
  void drop_(Channel &ch);
  /// Whether this is the first time this session (for a warning given once).
  bool first_(uint32_t what);
  bool stopping_() const { return this->p_->spice_request_.load(std::memory_order_acquire) != 0; }
  void close_();

  SimpleVideoPlayer *p_;
  Channel main_;
  Channel inputs_;
  Channel cursor_;
  Channel playback_;
  Channel usb_[USB_CHANNELS];
#ifdef SVP_SPICE_USB
  VmcLink usb_link_[USB_CHANNELS];
#endif
  struct sockaddr_in addr_ {};
  const char *password_{nullptr};
  const char *host_{nullptr};
  uint16_t port_{0};
  bool channels_done_{false};
  uint32_t session_id_{0};
  uint32_t out_w_{0}, out_h_{0};
  bool direct_{false};
  uint32_t warned_{0};
  bool panel_held_{false};
  bool took_panel_{false};
  bool wifi_held_{false};
  // Inputs (spice task).
  bool client_mouse_{false};  // SPICE_MOUSE_MODE_CLIENT: positions, else motions
  uint16_t mouse_supported_{0};
  int32_t pointer_x_{0}, pointer_y_{0};  // client mode: where the pointer is
  uint16_t mouse_buttons_{0};            // SPICE button mask from the mouse
  bool touch_down_{false};
  uint8_t touch_id_{0};
  int32_t touch_x_{0}, touch_y_{0};
  uint16_t sent_buttons_{0};
  uint32_t motion_count_{0};             // motions/positions sent, less those acknowledged
  int32_t motion_dx_{0}, motion_dy_{0};  // server mode: not sent yet
  bool position_pending_{false};         // client mode: not sent yet
  uint32_t cursor_tick_{0};
  bool sound_ok_{false};  // the playback stream is one the speaker plays
  // Leaving the session on the panel (as the dashboard): the close key held, two fingers held.
  SimpleVideoPlayer::DesktopClose closing_;
};

bool SpiceSession::first_(uint32_t what) {
  if (this->warned_ & what)
    return false;
  this->warned_ |= what;
  return true;
}

bool SpiceSession::resolve_(const char *host, uint16_t port) { return resolve_host(host, port, this->addr_); }

int SpiceSession::connect_() { return connect_to(this->addr_); }

// The link (spice_channel_send_link(), spice_channel_recv_link_msg(), spice_channel_recv_auth()).
bool SpiceSession::link_(Channel &ch, uint8_t type, uint8_t id, uint32_t channel_caps) {
  ch.type = type;
  ch.id = id;
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
  out[21] = id;
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
  return this->send_parts_(ch, type, body, len, nullptr, 0);
}

// One message of two parts (a header of the body and its data), the small ones sent in one piece.
bool SpiceSession::send_parts_(Channel &ch, uint16_t type, const uint8_t *a, size_t a_len, const uint8_t *b,
                               size_t b_len) {
  static constexpr size_t INLINE = 192;
  uint8_t out[18 + INLINE];
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
  if (a_len <= INLINE) {
    if (a_len > 0)
      std::memcpy(out + at, a, a_len);
    at += a_len;
    if (b_len > 0 && b_len <= INLINE - a_len) {
      std::memcpy(out + at, b, b_len);
      return send_all(ch.fd, out, at + b_len);
    }
    return send_all(ch.fd, out, at) && (b_len == 0 || send_all(ch.fd, b, b_len));
  }
  return send_all(ch.fd, out, at) && send_all(ch.fd, a, a_len) && (b_len == 0 || send_all(ch.fd, b, b_len));
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
    if (type == MSG_MAIN_INIT && size >= 16) {
      this->session_id_ = rd32(body);
      this->mouse_supported_ = static_cast<uint16_t>(rd32(body + 8));
      this->client_mouse_ = (rd32(body + 12) & MOUSE_MODE_CLIENT) != 0;
      if (!this->send_(ch, MSGC_MAIN_ATTACH_CHANNELS, nullptr, 0))
        return false;
      // Touches and the mouse land where they are: client mouse mode, where the server offers it (an
      // agent or a tablet in the guest).
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
      ESP_LOGD(TAG, "Mouse mode %s", this->client_mouse_ ? "client" : "server");
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
      return this->motion_flush_();
    }
    return true;  // init, key modifiers
  }
  if (&ch == &this->cursor_) {
    this->cursor_message_(type, body, size);
    return true;
  }
  if (&ch == &this->playback_) {
    this->playback_message_(type, body, size);
    return true;
  }
  return true;  // usbredir: its data is read by usb_read_()
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
  if (ch.type == CHANNEL_USBREDIR && type == MSG_SPICEVMC_DATA) {
    ch.vmc_left = size;  // read into its slot as it takes it (usb_read_())
    ok = true;
  } else if (&ch == &this->cursor_ && size > SimpleVideoPlayer::SPICE_SCRATCH_BYTES) {
    if (this->first_(WARN_CURSOR))
      ESP_LOGW(TAG, "A cursor larger than %ux%u is not drawn", SimpleVideoPlayer::SPICE_CURSOR_MAX,
               SimpleVideoPlayer::SPICE_CURSOR_MAX);
    this->cursor_message_(MSG_CURSOR_HIDE, nullptr, 0);
    ok = this->skip_(ch, size);
  } else if (size <= SimpleVideoPlayer::SPICE_SCRATCH_BYTES) {
    ok = recv_all(ch.fd, this->p_->spice_scratch_, size) &&
         this->handle_message_(ch, type, this->p_->spice_scratch_, size);
  } else {
    ok = this->skip_(ch, size);
  }
  return ok;
}

// The display channel is the relay's (whole JPEGs of the output's size, as the stream's sender): it
// joins this session by its id. The panel is taken first, so the relay's sender is let in.
bool SpiceSession::open_display_() {
  if (!this->take_panel_())
    return false;
  if (this->p_->desktop_relay_start_("spice", this->host_, this->port_, this->password_, this->session_id_))
    return true;
  this->p_->desktop_took_ = false;
  return false;
}

// The panel to this session, before everything else (desktop_take_panel_()); LVGL pauses for it
// (loop()) as for a sender. What plays sound stops only on the device the session's sound takes
// (spice_sound_take_()).
bool SpiceSession::take_panel_() {
  SimpleVideoPlayer *p = this->p_;
  if (!p->desktop_take_panel_(this->direct_, this->wifi_held_, true))
    return false;
  for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++)
    p->spice_cursor_cache_[i].valid = false;
  this->pointer_x_ = static_cast<int32_t>(this->out_w_ / 2);
  this->pointer_y_ = static_cast<int32_t>(this->out_h_ / 2);
  this->panel_held_ = true;
  this->took_panel_ = true;
  ESP_LOGI(TAG, "Connected to %s:%u", inet_ntoa(this->addr_.sin_addr), ntohs(this->addr_.sin_port));
  return true;
}

void SpiceSession::release_panel_() { this->p_->desktop_release_panel_(this->panel_held_, this->wifi_held_); }

void SpiceSession::drop_(Channel &ch) {
  if (ch.fd < 0)
    return;
  ESP_LOGW(TAG, "Channel %u:%u ended", ch.type, ch.id);
  ::close(ch.fd);
  ch.fd = -1;
  ch.vmc_left = 0;
  if (&ch == &this->inputs_)
    this->p_->spice_inputs_on_.store(false, std::memory_order_release);
  if (&ch == &this->playback_)
    this->p_->spice_sound_on_.store(false, std::memory_order_release);
  if (&ch == &this->cursor_)
    this->cursor_message_(MSG_CURSOR_HIDE, nullptr, 0);
#ifdef SVP_SPICE_USB
  for (uint8_t i = 0; i < USB_CHANNELS; i++) {
    if (&ch == &this->usb_[i] && i < this->p_->spice_usb_count_)
      this->p_->spice_usb_[i]->link_close();
  }
#endif
}

void SpiceSession::close_() {
#ifdef SVP_SPICE_USB
  for (uint8_t i = 0; i < USB_CHANNELS && i < this->p_->spice_usb_count_; i++) {
    if (this->usb_[i].fd >= 0)
      this->p_->spice_usb_[i]->link_close();
  }
#endif
  for (Channel *ch : {&this->main_, &this->inputs_, &this->cursor_, &this->playback_, &this->usb_[0],
                      &this->usb_[1], &this->usb_[2], &this->usb_[3]}) {
    if (ch->fd >= 0) {
      ::close(ch->fd);
      ch->fd = -1;
    }
  }
}

// CHANNELS_LIST: the display first (the panel is taken with it), then what the panel has a use for.
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
    ESP_LOGE(TAG, "The server has no display");
    return false;
  }
  if (!this->open_display_())
    return false;
  SimpleVideoPlayer *p = this->p_;
#if defined(SVP_STREAM_INPUT) || defined(SVP_STREAM_TOUCH)
  bool inputs = false;
#ifdef SVP_STREAM_INPUT
  inputs = inputs || p->stream_input_q_ != nullptr;
#endif
#ifdef SVP_STREAM_TOUCH
  inputs = inputs || p->stream_touch_q_ != nullptr;
#endif
  if (inputs && listed(CHANNEL_INPUTS, 0)) {
    if (this->link_(this->inputs_, CHANNEL_INPUTS, 0, 0)) {
      // What came before the session is not its.
#ifdef SVP_STREAM_INPUT
      if (p->stream_input_q_ != nullptr)
        xQueueReset(p->stream_input_q_);
#endif
#ifdef SVP_STREAM_TOUCH
      if (p->stream_touch_q_ != nullptr)
        xQueueReset(p->stream_touch_q_);
#endif
      p->spice_inputs_on_.store(true, std::memory_order_release);
    } else {
      this->drop_(this->inputs_);
    }
  }
#endif
  if (listed(CHANNEL_CURSOR, 0) && !this->link_(this->cursor_, CHANNEL_CURSOR, 0, 0))
    this->drop_(this->cursor_);
#ifdef USE_AUDIO
  if (p->speaker_ != nullptr && p->stream_audio_block_ != nullptr && listed(CHANNEL_PLAYBACK, 0)) {
    // No codec offered: the server sends raw S16 (snd_desired_audio_mode()).
    if (this->link_(this->playback_, CHANNEL_PLAYBACK, 0, 0)) {
      p->spice_sound_on_.store(true, std::memory_order_release);
      p->spice_sound_claim_.store(true, std::memory_order_release);
      App.wake_loop_threadsafe();
    } else {
      this->drop_(this->playback_);
    }
  }
#endif
#ifdef SVP_SPICE_USB
  for (uint8_t i = 0; i < p->spice_usb_count_ && i < USB_CHANNELS; i++) {
    if (!listed(CHANNEL_USBREDIR, i)) {
      if (this->first_(WARN_USB))
        ESP_LOGW(TAG, "The server has fewer usbredir channels than the panel has USB redirection slots");
      continue;
    }
    if (!this->link_(this->usb_[i], CHANNEL_USBREDIR, i, 0)) {
      this->drop_(this->usb_[i]);
      continue;
    }
    this->usb_link_[i].session = this;
    this->usb_link_[i].channel = &this->usb_[i];
    if (!p->spice_usb_[i]->link_open(&this->usb_link_[i]))
      this->drop_(this->usb_[i]);
  }
#endif
  return true;
}

//========================================================================
// Inputs: keys, mouse and touches into the inputs channel (spice-gtk's channel-inputs.c)
//========================================================================

bool SpiceSession::inputs_pending_() {
  if (this->inputs_.fd < 0)
    return true;
  SimpleVideoPlayer *p = this->p_;
#ifdef SVP_STREAM_INPUT
  if (p->stream_input_q_ != nullptr) {
    SimpleVideoPlayer::StreamInput input;
    while (xQueueReceive(p->stream_input_q_, &input, 0) == pdTRUE) {
      bool ok = true;
      if (input.data[0] == 'K' && input.length >= 8) {
        ok = this->key_(input.data);
      } else if (input.data[0] == 'P' && input.length >= 7) {
        ok = this->pointer_(input.data);
      }  // 'X': typed characters come as their keys too
      if (!ok)
        return false;
    }
  }
#endif
#ifdef SVP_STREAM_TOUCH
  if (p->stream_touch_q_ != nullptr) {
    StreamTouch touch;
    while (xQueueReceive(p->stream_touch_q_, &touch, 0) == pdTRUE) {
      if (!this->touch_(touch))
        return false;
    }
  }
#endif
  (void) p;
  return true;
}

#ifdef SVP_STREAM_INPUT
// 'K' action page:2 usage:2 mods pressed.
bool SpiceSession::key_(const uint8_t *m) {
  const uint8_t action = m[1];
  const uint16_t page = rd16(m + 2);
  const uint16_t usage = rd16(m + 4);
  const bool pressed = m[7] != 0;
  uint16_t xt = 0;
  if (page == 0x07 && usage < sizeof(KEYBOARD_XT) / sizeof(KEYBOARD_XT[0])) {
    xt = KEYBOARD_XT[usage];
  } else if (page == 0x0C) {
    for (const auto &key : CONSUMER_XT) {
      if (key.usage == usage)
        xt = key.xt;
    }
  }
  // The close key: held long it ends the session (close_due_()), let go before it is the desktop's.
  const auto close = this->closing_.key(this->p_, action, page, usage, pressed);
  if (close == SimpleVideoPlayer::DesktopClose::TAKEN)
    return true;
  if (close == SimpleVideoPlayer::DesktopClose::TAPPED)
    return xt == 0 || (this->send_key_(xt, true) && this->send_key_(xt, false));
  if (xt == 0)
    return true;  // a mouse button (page 9) comes with the pointer
  return this->send_key_(xt, pressed);
}

bool SpiceSession::send_key_(uint16_t xt, bool pressed) {
  uint8_t code[4];
  put32(code, key_code(xt, !pressed));
  return this->send_(this->inputs_, pressed ? MSGC_INPUTS_KEY_DOWN : MSGC_INPUTS_KEY_UP, code, sizeof(code));
}

// 'P' dx:2 dy:2 wheel buttons: HID buttons 1 (primary), 2 (secondary), 3 (middle), 4, 5.
bool SpiceSession::pointer_(const uint8_t *m) {
  const auto dx = static_cast<int16_t>(rd16(m + 1));
  const auto dy = static_cast<int16_t>(rd16(m + 3));
  const auto wheel = static_cast<int8_t>(m[5]);
  const uint8_t hid = m[6];
  uint16_t mask = 0;
  if (hid & 0x01)
    mask |= button_mask(BUTTON_LEFT);
  if (hid & 0x02)
    mask |= button_mask(BUTTON_RIGHT);
  if (hid & 0x04)
    mask |= button_mask(BUTTON_MIDDLE);
  if (hid & 0x08)
    mask |= button_mask(BUTTON_SIDE);
  if (hid & 0x10)
    mask |= button_mask(BUTTON_EXTRA);
  this->mouse_buttons_ = mask;
  if ((dx != 0 || dy != 0) && !this->move_(this->pointer_x_ + dx, this->pointer_y_ + dy, dx, dy))
    return false;
  const uint16_t now = static_cast<uint16_t>(mask | (this->touch_down_ ? button_mask(BUTTON_LEFT) : 0));
  if (!this->buttons_(now))
    return false;
  // A wheel step is a press and release of UP or DOWN (spice-gtk's scroll events).
  const uint8_t button = wheel > 0 ? BUTTON_UP : BUTTON_DOWN;
  for (int step = 0; step < (wheel > 0 ? wheel : -wheel); step++) {
    uint8_t msg[3];
    msg[0] = button;
    put16(msg + 1, static_cast<uint16_t>(this->sent_buttons_ | button_mask(button)));
    if (!this->send_(this->inputs_, MSGC_INPUTS_MOUSE_PRESS, msg, sizeof(msg)))
      return false;
    put16(msg + 1, this->sent_buttons_);
    if (!this->send_(this->inputs_, MSGC_INPUTS_MOUSE_RELEASE, msg, sizeof(msg)))
      return false;
  }
  return true;
}

#endif

#ifdef SVP_STREAM_TOUCH
// The first finger is the left button where it touches; the others are not passed on. Two fingers
// are the panel's gesture: held long they end the session (close_due_()).
bool SpiceSession::touch_(const StreamTouch &touch) {
  if (touch.count >= 2) {
    if (this->closing_.two_fingers(true) && this->touch_down_) {
      this->touch_down_ = false;
      return this->buttons_(this->mouse_buttons_);
    }
    return true;
  }
  this->closing_.two_fingers(false);
  int found = -1;
  for (uint8_t i = 0; i < touch.count; i++) {
    if (!this->touch_down_ || touch.id[i] == this->touch_id_) {
      found = i;
      break;
    }
  }
  if (found < 0) {
    if (!this->touch_down_)
      return true;
    this->touch_down_ = false;
    return this->buttons_(this->mouse_buttons_);
  }
  const int32_t x = std::min<int32_t>(touch.x[found], static_cast<int32_t>(this->out_w_) - 1);
  const int32_t y = std::min<int32_t>(touch.y[found], static_cast<int32_t>(this->out_h_) - 1);
  const bool first = !this->touch_down_;
  // Server mouse mode moves relatively: from where the finger was, not from where it lands.
  const int32_t dx = first ? 0 : x - this->touch_x_;
  const int32_t dy = first ? 0 : y - this->touch_y_;
  this->touch_x_ = x;
  this->touch_y_ = y;
  if ((this->client_mouse_ || dx != 0 || dy != 0) && !this->move_(x, y, dx, dy))
    return false;
  if (first) {
    this->touch_down_ = true;
    this->touch_id_ = touch.id[found];
    return this->buttons_(static_cast<uint16_t>(this->mouse_buttons_ | button_mask(BUTTON_LEFT)));
  }
  return true;
}
#endif

bool SpiceSession::move_(int32_t x, int32_t y, int32_t dx, int32_t dy) {
  if (this->client_mouse_) {
    this->pointer_x_ = std::clamp<int32_t>(x, 0, static_cast<int32_t>(this->out_w_) - 1);
    this->pointer_y_ = std::clamp<int32_t>(y, 0, static_cast<int32_t>(this->out_h_) - 1);
    this->position_pending_ = true;
    // The server does not move the cursor in client mode: it is drawn where the pointer is.
    SimpleVideoPlayer *p = this->p_;
    xSemaphoreTake(p->spice_cursor_lock_, portMAX_DELAY);
    p->spice_cursor_->x = this->pointer_x_;
    p->spice_cursor_->y = this->pointer_y_;
    xSemaphoreGive(p->spice_cursor_lock_);
    this->cursor_changed_();
  } else {
    this->motion_dx_ += dx;
    this->motion_dy_ += dy;
  }
  return this->motion_flush_();
}

// Motions and positions wait while more than two bunches are unacknowledged (spice-gtk).
bool SpiceSession::motion_flush_(bool force) {
  if (!force && this->motion_count_ >= MOTION_ACK_BUNCH * 2)
    return true;
  if (this->client_mouse_ && this->position_pending_) {
    uint8_t msg[11];
    put32(msg, static_cast<uint32_t>(this->pointer_x_));
    put32(msg + 4, static_cast<uint32_t>(this->pointer_y_));
    put16(msg + 8, this->sent_buttons_);
    msg[10] = 0;  // display id
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
  if (now == this->sent_buttons_)
    return true;
  // The pointer is where it is before a button changes (spice-gtk sends motion and position first).
  if (!this->motion_flush_(true))
    return false;
  for (uint8_t button : {BUTTON_LEFT, BUTTON_MIDDLE, BUTTON_RIGHT, BUTTON_SIDE, BUTTON_EXTRA}) {
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

//========================================================================
// Cursor channel: the shape and where it is (spice-gtk's channel-cursor.c), drawn by the draw task
//========================================================================

void SpiceSession::cursor_changed_() {
  this->p_->spice_cursor_dirty_.store(true, std::memory_order_release);
  // The draw task takes a null frame for a cursor change; a full queue brings it with the next frame.
  StreamFrame *none = nullptr;
  xQueueSend(this->p_->stream_filled_q_, &none, 0);
}

void SpiceSession::cursor_message_(uint16_t type, const uint8_t *body, uint32_t size) {
  SimpleVideoPlayer *p = this->p_;
  SimpleVideoPlayer::SpiceCursor *c = p->spice_cursor_;
  const uint8_t *end = body + size;
  switch (type) {
    case MSG_CURSOR_INIT:   // position, trail_length, trail_frequency, visible, cursor
    case MSG_CURSOR_SET: {  // position, visible, cursor
      const uint32_t at = type == MSG_CURSOR_INIT ? 9 : 5;
      if (size < at)
        return;
      xSemaphoreTake(p->spice_cursor_lock_, portMAX_DELAY);
      if (!this->client_mouse_) {
        c->x = static_cast<int16_t>(rd16(body));
        c->y = static_cast<int16_t>(rd16(body + 2));
      }
      const bool shape = this->cursor_shape_(body + at, end);
      c->visible = body[at - 1] != 0 && shape;
      xSemaphoreGive(p->spice_cursor_lock_);
      break;
    }
    case MSG_CURSOR_MOVE:
      if (size < 4 || this->client_mouse_)
        return;
      xSemaphoreTake(p->spice_cursor_lock_, portMAX_DELAY);
      c->x = static_cast<int16_t>(rd16(body));
      c->y = static_cast<int16_t>(rd16(body + 2));
      xSemaphoreGive(p->spice_cursor_lock_);
      break;
    case MSG_CURSOR_HIDE:
      xSemaphoreTake(p->spice_cursor_lock_, portMAX_DELAY);
      c->visible = false;
      xSemaphoreGive(p->spice_cursor_lock_);
      break;
    case MSG_CURSOR_RESET:
      for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++)
        p->spice_cursor_cache_[i].valid = false;
      xSemaphoreTake(p->spice_cursor_lock_, portMAX_DELAY);
      c->visible = false;
      c->shape.valid = false;
      xSemaphoreGive(p->spice_cursor_lock_);
      break;
    case MSG_CURSOR_INVAL_ONE:
      if (size >= 8) {
        const uint64_t id = rd64(body);
        for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++) {
          if (p->spice_cursor_cache_[i].valid && p->spice_cursor_cache_[i].id == id)
            p->spice_cursor_cache_[i].valid = false;
        }
      }
      return;
    case MSG_CURSOR_INVAL_ALL:
      for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++)
        p->spice_cursor_cache_[i].valid = false;
      return;
    default:
      return;  // trail
  }
  this->cursor_changed_();
}

// SpiceCursor: flags:2, [unique:8 type:1 width:2 height:2 hot_spot_x:2 hot_spot_y:2], data to the
// end. Into spice_cursor_->shape (lock held).
bool SpiceSession::cursor_shape_(const uint8_t *cp, const uint8_t *end) {
  SimpleVideoPlayer *p = this->p_;
  SpiceCursorShape &shape = p->spice_cursor_->shape;
  if (end - cp < 2)
    return false;
  const uint16_t flags = rd16(cp);
  if (flags & CURSOR_FLAGS_NONE)
    return false;
  if (end - cp < 19)
    return false;
  const uint64_t id = rd64(cp + 2);
  if (flags & CURSOR_FLAGS_FROM_CACHE) {
    for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++) {
      SpiceCursorShape &cached = p->spice_cursor_cache_[i];
      if (cached.valid && cached.id == id) {
        cached.used = ++this->cursor_tick_;
        std::memcpy(&shape, &cached, sizeof(shape));
        return true;
      }
    }
    // Fewer kept here than the server's 256: shown again once it sends it anew.
    if (this->first_(WARN_CURSOR_CACHE))
      ESP_LOGW(TAG, "A cursor no longer kept here is not drawn");
    return false;
  }
  const uint8_t type = cp[10];
  const uint16_t w = rd16(cp + 11), h = rd16(cp + 13);
  shape.valid = false;
  if (w == 0 || h == 0 || !this->cursor_convert_(shape, type, w, h, cp + 19, static_cast<size_t>(end - (cp + 19))))
    return false;
  shape.id = id;
  shape.hot_x = std::min<uint16_t>(rd16(cp + 15), shape.w);
  shape.hot_y = std::min<uint16_t>(rd16(cp + 17), shape.h);
  shape.valid = true;
  if (flags & CURSOR_FLAGS_CACHE_ME) {
    SpiceCursorShape *slot = &p->spice_cursor_cache_[0];
    for (uint8_t i = 0; i < SimpleVideoPlayer::SPICE_CURSOR_CACHE; i++) {
      SpiceCursorShape &cached = p->spice_cursor_cache_[i];
      if (!cached.valid) {
        slot = &cached;
        break;
      }
      if (cached.used < slot->used)
        slot = &cached;
    }
    std::memcpy(slot, &shape, sizeof(shape));
    slot->used = ++this->cursor_tick_;
  }
  return true;
}

// The cursor types spice-gtk draws (set_cursor()), as straight ARGB. A mask bit over white inverts
// what is under it (spice-gtk draws a pattern there instead); larger than SPICE_CURSOR_MAX: its top left.
bool SpiceSession::cursor_convert_(SpiceCursorShape &out, uint8_t type, uint16_t w, uint16_t h, const uint8_t *data,
                                   size_t len) {
  constexpr uint32_t INVERT = SimpleVideoPlayer::SPICE_CURSOR_INVERT;
  const uint16_t ow = std::min<uint16_t>(w, SimpleVideoPlayer::SPICE_CURSOR_MAX);
  const uint16_t oh = std::min<uint16_t>(h, SimpleVideoPlayer::SPICE_CURSOR_MAX);
  if ((ow != w || oh != h) && this->first_(WARN_CURSOR))
    ESP_LOGW(TAG, "A cursor larger than %ux%u is cut", SimpleVideoPlayer::SPICE_CURSOR_MAX,
             SimpleVideoPlayer::SPICE_CURSOR_MAX);
  const size_t bpl = (static_cast<size_t>(w) + 7) / 8;  // a mask row
  const size_t pixels = static_cast<size_t>(w) * h;
  auto bit = [](const uint8_t *mask, size_t row_bytes, uint16_t x, uint16_t y) {
    return (mask[y * row_bytes + x / 8] & (0x80 >> (x % 8))) != 0;
  };
  out.w = ow;
  out.h = oh;
  uint32_t *px = out.px;
  switch (type) {
    case CURSOR_TYPE_ALPHA:
      if (len < pixels * 4)
        return false;
      for (uint16_t y = 0; y < oh; y++) {
        for (uint16_t x = 0; x < ow; x++) {
          const uint32_t v = rd32(data + (static_cast<size_t>(y) * w + x) * 4);
          px[y * ow + x] = (v >> 24) == 0 ? 0 : v;
        }
      }
      return true;
    case CURSOR_TYPE_MONO: {
      // AND mask, then XOR mask: transparent, black, white, or inverted.
      if (len < 2 * bpl * h)
        return false;
      const uint8_t *xor_mask = data + bpl * h;
      for (uint16_t y = 0; y < oh; y++) {
        for (uint16_t x = 0; x < ow; x++) {
          const bool a = bit(data, bpl, x, y), o = bit(xor_mask, bpl, x, y);
          px[y * ow + x] = a ? (o ? INVERT : 0) : (o ? 0xFFFFFFFF : 0xFF000000);
        }
      }
      return true;
    }
    case CURSOR_TYPE_COLOR32:
    case CURSOR_TYPE_COLOR16:
    case CURSOR_TYPE_COLOR4: {
      // Colours, (COLOR4: a 16-entry palette,) then the AND mask.
      size_t colors;
      size_t row = 0;
      if (type == CURSOR_TYPE_COLOR32) {
        colors = pixels * 4;
      } else if (type == CURSOR_TYPE_COLOR16) {
        colors = pixels * 2;
      } else {
        row = (static_cast<size_t>(w) + 1) / 2;
        colors = row * h + 16 * 4;
      }
      if (len < colors + bpl * h)
        return false;
      const uint8_t *mask = data + colors;
      for (uint16_t y = 0; y < oh; y++) {
        for (uint16_t x = 0; x < ow; x++) {
          const size_t i = static_cast<size_t>(y) * w + x;
          uint32_t c;
          bool white;
          if (type == CURSOR_TYPE_COLOR32) {
            c = rd32(data + i * 4) & 0xFFFFFF;
            white = c == 0xFFFFFF;
          } else if (type == CURSOR_TYPE_COLOR16) {
            const uint16_t v = rd16(data + i * 2);
            c = ((v & 0x1F) << 3) | ((v & 0x3E0) << 6) | ((v & 0x7C00) << 9);
            white = v == 0x7FFF;
          } else {
            const uint8_t b = data[y * row + x / 2];
            const uint8_t idx = (x & 1) ? (b & 0x0F) : (b >> 4);
            c = rd32(data + row * h + idx * 4) & 0xFFFFFF;
            white = c == 0xFFFFFF;
          }
          const bool m = bit(mask, bpl, x, y);
          px[y * ow + x] = m ? (white ? INVERT : 0) : (0xFF000000 | c);
        }
      }
      return true;
    }
    default:
      if (this->first_(WARN_CURSOR))
        ESP_LOGW(TAG, "Cursors of type %u are not drawn", type);
      return false;
  }
}

//========================================================================
// Playback channel: raw S16 into the speaker, as a sender's sound
//========================================================================

void SpiceSession::playback_message_(uint16_t type, const uint8_t *body, uint32_t size) {
#ifdef USE_AUDIO
  SimpleVideoPlayer *p = this->p_;
  switch (type) {
    case MSG_PLAYBACK_START: {  // channels:4 format:2 frequency:4 time:4
      if (size < 10)
        return;
      const uint32_t channels = rd32(body);
      this->sound_ok_ = rd16(body + 4) == AUDIO_FMT_S16 && channels >= 1 && channels <= 2;
      if (!this->sound_ok_) {
        if (this->first_(WARN_SOUND))
          ESP_LOGW(TAG, "Sound of %" PRIu32 " channels in format %u is not played", channels, rd16(body + 4));
        return;
      }
      p->stream_audio_src_ch_ = static_cast<uint8_t>(channels);
      p->stream_audio_rate_ = rd32(body + 6);
      p->stream_audio_carry_len_ = 0;
      return;
    }
    case MSG_PLAYBACK_MODE:  // time:4 mode:2
      if (size >= 6 && rd16(body + 4) != AUDIO_DATA_MODE_RAW) {
        this->sound_ok_ = false;
        if (this->first_(WARN_SOUND))
          ESP_LOGW(TAG, "Compressed sound is not played");
      }
      return;
    case MSG_PLAYBACK_DATA:  // time:4, samples
      if (this->sound_ok_ && size > 4)
        p->stream_play_audio_(body + 4, size - 4);
      return;
    default:
      return;  // stop (the speaker is let go once sound stops coming), volume, mute, latency
  }
#else
  (void) type;
  (void) body;
  (void) size;
#endif
}

//========================================================================
// usbredir channels: the bytes of their data messages straight into the slots
//========================================================================

#ifdef SVP_SPICE_USB
bool SpiceSession::usb_read_(Channel &ch, SpiceUsbSlot *slot) {
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

uint32_t SpiceSession::close_due_() const { return this->closing_.due(this->p_); }

void SpiceSession::run(const char *host, uint16_t port, const char *password) {
  SimpleVideoPlayer *p = this->p_;
  this->password_ = password;
  this->host_ = host;
  this->port_ = port;
  p->desktop_output_(this->direct_, this->out_w_, this->out_h_);
  ESP_LOGI(TAG, "Connecting to %s:%u", host, port);
  if (!this->resolve_(host, port) || !this->link_(this->main_, CHANNEL_MAIN, 0, 0)) {
    this->close_();
    return;
  }
  Channel *const others[] = {&this->inputs_, &this->cursor_, &this->playback_};
  while (true) {
    fd_set readable;
    FD_ZERO(&readable);
    int top = p->spice_wake_fd_;
    FD_SET(top, &readable);
    auto add = [&readable, &top](const Channel &ch) {
      if (ch.fd >= 0) {
        FD_SET(ch.fd, &readable);
        top = std::max(top, ch.fd);
      }
    };
    add(this->main_);
    for (Channel *ch : others)
      add(*ch);
#ifdef SVP_SPICE_USB
    for (uint8_t i = 0; i < p->spice_usb_count_ && i < USB_CHANNELS; i++) {
      size_t room = 1;
      if (this->usb_[i].vmc_left > 0)
        p->spice_usb_[i]->rx_space(room);
      if (room > 0)  // else it waits for a free transfer: read no further
        add(this->usb_[i]);
    }
#endif
    // Woken by the sockets and the wake fd; a held close key or two fingers bring their own deadline.
    const uint32_t due = this->close_due_();
    struct timeval wait = {.tv_sec = static_cast<time_t>(due / 1000),
                           .tv_usec = static_cast<suseconds_t>((due % 1000) * 1000)};
    if (::select(top + 1, &readable, nullptr, nullptr, due == UINT32_MAX ? nullptr : &wait) < 0)
      break;
    if (FD_ISSET(p->spice_wake_fd_, &readable)) {
      uint64_t count;
      ::read(p->spice_wake_fd_, &count, sizeof(count));
    }
    if (this->stopping_())
      break;
    if (p->desktop_sender_gone_.load(std::memory_order_acquire)) {
      ESP_LOGW(TAG, "svp_relay's picture ended");
      break;
    }
    if (this->close_due_() == 0) {
      ESP_LOGI(TAG, "Ended on the panel");
      break;
    }
    if (FD_ISSET(this->main_.fd, &readable) && !this->handle_one_(this->main_))
      break;
    for (Channel *ch : others) {
      if (ch->fd >= 0 && FD_ISSET(ch->fd, &readable) && !this->handle_one_(*ch))
        this->drop_(*ch);
    }
    if (!this->inputs_pending_())
      this->drop_(this->inputs_);
#ifdef SVP_SPICE_USB
    for (uint8_t i = 0; i < p->spice_usb_count_ && i < USB_CHANNELS; i++) {
      Channel &ch = this->usb_[i];
      if (ch.fd < 0)
        continue;
      if ((FD_ISSET(ch.fd, &readable) && !this->usb_read_(ch, p->spice_usb_[i])) || !p->spice_usb_[i]->service())
        this->drop_(ch);
    }
#endif
  }
  ESP_LOGI(TAG, "Disconnected");
  this->close_();
  p->desktop_sender_wanted_.store(false, std::memory_order_release);
  this->release_panel_();
}
#endif

//========================================================================
// A desktop session's share of the panel (SPICE, VNC), spice task
//========================================================================

void SimpleVideoPlayer::desktop_output_(bool &direct, uint32_t &w, uint32_t &h) {
#ifdef SVP_DSI_OUTPUT
  if (this->stream_direct_()) {
    direct = true;
    w = this->dsi_out_w_;
    h = this->dsi_out_h_;
#ifdef SVP_USE_LVGL
    if (this->lvgl_component_ != nullptr &&
        this->lvgl_component_->get_rotation() != display::DISPLAY_ROTATION_0_DEGREES)
      ESP_LOGW(TAG, "The panel is turned: the desktop shows on its native %" PRIu32 "x%" PRIu32 " unturned", w, h);
#endif
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  if (!this->stream_direct_()) {
    direct = false;
    w = this->canvas_size_w_;
    h = this->canvas_size_h_;
  }
#endif
}

// A playing video ends, a stream sender is let go, and both are refused from then on
// (spice_refuses_(), the network task).
bool SimpleVideoPlayer::desktop_take_panel_(bool direct, bool &wifi_held, bool with_sound) {
  // Under session_lock_: a video run_pending_start_() is starting either runs already (ended here)
  // or does not start (start_pending_ cleared; it looks again under the lock). With sound (SPICE)
  // nothing plays beside the session; without (VNC) an audio file plays on while the pool has room
  // for both.
  if (xSemaphoreTake(this->session_lock_, pdMS_TO_TICKS(TAKEOVER_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "A starting session did not let go of the panel");
    return false;
  }
  this->spice_on_.store(true, std::memory_order_release);
  this->app_end_req_.store(true, std::memory_order_release);  // pictures / the camera end (main loop)
  this->start_pending_.store(false, std::memory_order_release);
#ifdef SVP_AUDIO_FILES
  if (with_sound)
    this->audio_start_pending_.store(false, std::memory_order_release);
#endif
  if (with_sound ? this->session_busy_() : this->picture_session_busy_())
    this->end_session_();
  xSemaphoreGive(this->session_lock_);
  if (with_sound) {
    this->sound_takeover_req_.store(true, std::memory_order_release);  // the queue's HA track, the media player's
    App.wake_loop_threadsafe();
  }
  this->stream_drop_sender_.store(true, std::memory_order_release);
  this->stream_wake_();
  const bool free = this->desktop_wait_free_(with_sound);
  this->stream_drop_sender_.store(false, std::memory_order_release);
  if (!free) {
    this->spice_on_.store(false, std::memory_order_release);
    return false;
  }
  bool taken = this->desktop_buffers_take_();
#ifdef SVP_AUDIO_FILES
  if (!taken && !with_sound && this->audio_session_.load(std::memory_order_acquire)) {
    // No room for both: the audio file ends, its room is the session's.
    ESP_LOGI(TAG, "Audio file stopped: the pool has no room for it beside the desktop");
    if (xSemaphoreTake(this->session_lock_, pdMS_TO_TICKS(TAKEOVER_WAIT_MS)) == pdTRUE) {
      this->audio_start_pending_.store(false, std::memory_order_release);
      this->end_session_();
      xSemaphoreGive(this->session_lock_);
    }
    taken = this->desktop_wait_free_(true) && this->desktop_buffers_take_();
  }
#endif
  if (!taken) {
    this->spice_on_.store(false, std::memory_order_release);
    return false;
  }
  // No cursor yet; the draw task holds nothing of one (it draws nothing of this session yet).
  xSemaphoreTake(this->spice_cursor_lock_, portMAX_DELAY);
  this->spice_cursor_->visible = false;
  this->spice_cursor_->shape.valid = false;
  xSemaphoreGive(this->spice_cursor_lock_);
  this->spice_cursor_drawn_ = false;
  this->spice_cursor_dirty_.store(false, std::memory_order_release);
  this->spice_direct_out_.store(direct, std::memory_order_release);
  this->stream_connected_.store(true, std::memory_order_release);
  this->desktop_took_ = true;
  this->spice_connecting_.store(false, std::memory_order_release);
  hold_wifi_(wifi_held, true);
  this->spice_changed_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
  return true;
}

bool SimpleVideoPlayer::desktop_wait_free_(bool with_sound) {
  // Woken (spice_wake()) when the sender or the session's tasks let go. Wakes taken here for others
  // (requests, USB) are given again once done.
  auto busy = [this, with_sound]() { return with_sound ? this->session_busy_() : this->picture_session_busy_(); };
  bool woken = false;
  for (const uint32_t start = millis(); this->stream_sender_busy_.load(std::memory_order_acquire) || busy();) {
    const uint32_t waited = millis() - start;
    if (waited > TAKEOVER_WAIT_MS || this->spice_request_.load(std::memory_order_acquire) != 0) {
      if (waited > TAKEOVER_WAIT_MS)
        ESP_LOGE(TAG, "The playing video or stream sender did not let go of the panel");
      if (woken)
        this->spice_wake();
      return false;
    }
    const uint32_t left_ms = TAKEOVER_WAIT_MS + 1 - waited;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(this->spice_wake_fd_, &readable);
    struct timeval wait = {.tv_sec = static_cast<time_t>(left_ms / 1000),
                           .tv_usec = static_cast<suseconds_t>((left_ms % 1000) * 1000)};
    if (::select(this->spice_wake_fd_ + 1, &readable, nullptr, nullptr, &wait) == 1) {
      uint64_t count;
      ::read(this->spice_wake_fd_, &count, sizeof(count));
      woken = true;
    }
  }
  if (woken)
    this->spice_wake();
  return true;
}

uint8_t *SimpleVideoPlayer::desktop_alloc(size_t size) {
  // On the spice task the owner table's desktop row hands back when it does not fit (the main loop
  // answers); elsewhere the pool alone.
  uint8_t *piece = xTaskGetCurrentTaskHandle() == this->spice_task_ ? this->pool_take_(size, POOL_USER_DESKTOP)
                                                                     : this->pool_.alloc(size);
  if (piece == nullptr)
    ESP_LOGE(TAG, "PSRAM pool: no room for the desktop's %zu bytes (%zu of %zu used, largest free %zu)", size,
             this->pool_.used(), this->pool_.size(), this->pool_.largest_free());
  return piece;
}

bool SimpleVideoPlayer::desktop_buffers_take_() {
  // From the pool; only when they do not fit, pictures' and an idle video's buffers come back
  // (desktop_alloc()). Taken without desktop_lock_ (the main loop answering may want it in
  // release_desktop_()), set under it; release_desktop_() leaves them be while spice_on_.
  auto *cursor = this->spice_cursor_ != nullptr
                     ? this->spice_cursor_
                     : reinterpret_cast<SpiceCursor *>(this->desktop_alloc(sizeof(SpiceCursor)));
  auto *cache = this->spice_cursor_cache_ != nullptr ? this->spice_cursor_cache_
                                                     : reinterpret_cast<SpiceCursorShape *>(this->desktop_alloc(
                                                           SPICE_CURSOR_CACHE * sizeof(SpiceCursorShape)));
  uint8_t *under = this->spice_cursor_under_ != nullptr ? this->spice_cursor_under_
                                                        : this->desktop_alloc(SPICE_CURSOR_MAX * SPICE_CURSOR_MAX * 3);
  uint8_t *patch = this->spice_cursor_patch_ != nullptr ? this->spice_cursor_patch_
                                                        : this->desktop_alloc(SPICE_CURSOR_MAX * SPICE_CURSOR_MAX * 3);
  xSemaphoreTake(this->desktop_lock_, portMAX_DELAY);
  this->spice_cursor_ = cursor;
  this->spice_cursor_cache_ = cache;
  this->spice_cursor_under_ = under;
  this->spice_cursor_patch_ = patch;
  const bool ok = this->spice_cursor_ != nullptr && this->spice_cursor_cache_ != nullptr &&
                  this->spice_cursor_under_ != nullptr && this->spice_cursor_patch_ != nullptr;
  if (ok) {
    std::memset(this->spice_cursor_, 0, sizeof(SpiceCursor));
    std::memset(this->spice_cursor_cache_, 0, SPICE_CURSOR_CACHE * sizeof(SpiceCursorShape));
  }
  xSemaphoreGive(this->desktop_lock_);
  return ok;
}

bool SimpleVideoPlayer::release_desktop_() {
  if (this->desktop_lock_ == nullptr || this->spice_on_.load(std::memory_order_acquire))
    return false;
  xSemaphoreTake(this->desktop_lock_, portMAX_DELAY);
  bool released = false;
  if (!this->spice_on_.load(std::memory_order_acquire)) {
    // Nothing draws a cursor any more (the draw task under output_lock_, the senders under spice_cursor_lock_).
    if (this->output_lock_ != nullptr)
      xSemaphoreTake(this->output_lock_, portMAX_DELAY);
    xSemaphoreTake(this->spice_cursor_lock_, portMAX_DELAY);
    released = this->spice_cursor_ != nullptr || this->spice_cursor_cache_ != nullptr ||
               this->spice_cursor_under_ != nullptr || this->spice_cursor_patch_ != nullptr;
    this->pool_.free(reinterpret_cast<uint8_t *>(this->spice_cursor_));
    this->pool_.free(reinterpret_cast<uint8_t *>(this->spice_cursor_cache_));
    this->pool_.free(this->spice_cursor_under_);
    this->pool_.free(this->spice_cursor_patch_);
    this->spice_cursor_ = nullptr;
    this->spice_cursor_cache_ = nullptr;
    this->spice_cursor_under_ = nullptr;
    this->spice_cursor_patch_ = nullptr;
    this->spice_cursor_drawn_ = false;
    xSemaphoreGive(this->spice_cursor_lock_);
    if (this->output_lock_ != nullptr)
      xSemaphoreGive(this->output_lock_);
#ifdef SVP_SPICE_USB
    for (uint8_t i = 0; i < this->spice_usb_count_; i++)
      released = this->spice_usb_[i]->release_buffers() || released;
#endif
  }
  xSemaphoreGive(this->desktop_lock_);
  return released;
}

void SimpleVideoPlayer::desktop_release_panel_(bool &panel_held, bool &wifi_held) {
  this->spice_inputs_on_.store(false, std::memory_order_release);
  this->spice_sound_on_.store(false, std::memory_order_release);
  hold_wifi_(wifi_held, false);
  if (panel_held) {
    panel_held = false;
    this->stream_connected_.store(false, std::memory_order_release);
    if (this->stream_draw_task_ != nullptr)
      xTaskNotifyGive(this->stream_draw_task_);  // no longer waits for the output
  }
  this->spice_on_.store(false, std::memory_order_release);
  this->spice_changed_.store(true, std::memory_order_release);
  App.wake_loop_threadsafe();
}

SimpleVideoPlayer::DesktopClose::Key SimpleVideoPlayer::DesktopClose::key(const SimpleVideoPlayer *p, uint8_t action,
                                                                          uint16_t page, uint16_t usage,
                                                                          bool pressed) {
  const uint8_t keys = p->spice_close_keys_.load(std::memory_order_acquire);
#ifdef SVP_STREAM_INPUT
  const bool close_key =
      (action == lvgl::KeyBindings::ACT_BACK && (keys & 1)) || (action == lvgl::KeyBindings::ACT_HOME && (keys & 2));
#else
  const bool close_key = false;
  (void) keys;
  (void) action;
#endif
  if (pressed && close_key && !this->key_held) {
    this->key_held = true;
    this->page = page;
    this->usage = usage;
    this->key_at = millis();
    return TAKEN;
  }
  if (!pressed && this->key_held && page == this->page && usage == this->usage) {
    this->key_held = false;
    return TAPPED;
  }
  return OTHER;
}

bool SimpleVideoPlayer::DesktopClose::two_fingers(bool down) {
  if (!down) {
    this->fingers = false;
    return false;
  }
  if (this->fingers)
    return false;
  this->fingers = true;
  this->fingers_at = millis();
  return true;
}

uint32_t SimpleVideoPlayer::DesktopClose::due(const SimpleVideoPlayer *p) const {
  const uint32_t now = millis();
  uint32_t due = UINT32_MAX;
  auto left = [now](uint32_t since, uint32_t hold) { return now - since >= hold ? 0 : hold - (now - since); };
  if (this->key_held)
    due = std::min(due, left(this->key_at, p->spice_close_hold_ms_.load(std::memory_order_acquire)));
  if (this->fingers)
    due = std::min(due, left(this->fingers_at, p->spice_touch_hold_ms_.load(std::memory_order_acquire)));
  return due;
}

// A greeting comes within this after the connection, from a server that sends first.
static constexpr uint32_t PROBE_WAIT_MS = 1500;

SimpleVideoPlayer::DesktopKind SimpleVideoPlayer::desktop_probe_(const char *host, uint16_t port) {
  struct sockaddr_in addr {};
  if (!resolve_host(host, port, addr))
    return DesktopKind::UNREACHED;
  const int fd = connect_to(addr);
  if (fd < 0) {
    ESP_LOGE(TAG, "Could not connect to %s:%u", host, port);
    return DesktopKind::UNREACHED;
  }
  // Woken by the greeting, a disconnect or the wake fd; nothing read (MSG_PEEK) nor sent.
  DesktopKind kind = DesktopKind::SPICE;
  for (const uint32_t start = millis();;) {
    const uint32_t spent = millis() - start;
    if (spent >= PROBE_WAIT_MS || this->spice_request_.load(std::memory_order_acquire) != 0)
      break;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    FD_SET(this->spice_wake_fd_, &readable);
    const uint32_t left = PROBE_WAIT_MS - spent;
    struct timeval wait = {.tv_sec = static_cast<time_t>(left / 1000),
                           .tv_usec = static_cast<suseconds_t>((left % 1000) * 1000)};
    if (::select(std::max(fd, this->spice_wake_fd_) + 1, &readable, nullptr, nullptr, &wait) <= 0)
      break;
    if (FD_ISSET(this->spice_wake_fd_, &readable)) {
      uint64_t count;
      ::read(this->spice_wake_fd_, &count, sizeof(count));
    }
    if (!FD_ISSET(fd, &readable))
      continue;
    uint8_t greeting[4];
    const int got = ::recv(fd, greeting, sizeof(greeting), MSG_PEEK);
    if (got <= 0)
      break;
    if (got < static_cast<int>(sizeof(greeting))) {
      vTaskDelay(pdMS_TO_TICKS(10));  // the rest of a greeting split up
      continue;
    }
    if (std::memcmp(greeting, "RFB ", sizeof(greeting)) == 0)
      kind = DesktopKind::VNC;  // rfbproto: ProtocolVersion, "RFB 003.008\n"
    break;
  }
  ::close(fd);
  return kind;
}

#if defined(SVP_SPICE_USB) && defined(SVP_DESKTOP_VNC)
namespace {
constexpr uint16_t USBIP_PORT = 3240;
constexpr uint16_t USBIP_VERSION = 0x0111;
constexpr uint16_t USBIP_REQ_DEVLIST = 0x8005, USBIP_REP_DEVLIST = 0x0005;
constexpr uint16_t USBIP_REQ_IMPORT = 0x8003, USBIP_REP_IMPORT = 0x0003;

// A slot's USB/IP connection: what the slot sends goes out on it.
struct UsbipLink : SpiceVmcLink {
  int fd{-1};
  bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) override {
    return send_all(this->fd, head, head_len) && (data_len == 0 || send_all(this->fd, data, data_len));
  }
};

int usbip_listen() {
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

inline void put_be16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
inline void put_be32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
}  // namespace
#endif

#ifdef SVP_DESKTOP_VNC
// The panel, then the relay's picture; until the panel ends it or the relay's sender goes. The USB
// slots' devices are exported over USB/IP meanwhile (select/usbip.cpp).
void SimpleVideoPlayer::desktop_vnc_(const char *kind, const char *host, uint16_t port, const char *password) {
  bool direct = true, wifi_held = false;
  uint32_t out_w = 0, out_h = 0;
  this->desktop_output_(direct, out_w, out_h);
  if (!this->desktop_take_panel_(direct, wifi_held, false))
    return;
  bool panel_held = true;
  // Keys, mouse and touches go to the relay's sender; what came before the session is not its.
#ifdef SVP_STREAM_INPUT
  if (this->stream_input_q_ != nullptr)
    xQueueReset(this->stream_input_q_);
#endif
#ifdef SVP_STREAM_TOUCH
  if (this->stream_touch_q_ != nullptr)
    xQueueReset(this->stream_touch_q_);
#endif
  this->desktop_relay_inputs_.store(true, std::memory_order_release);
  if (!this->desktop_relay_start_(kind, host, port, password, 0)) {
    this->desktop_took_ = false;
  } else {
    if (std::strcmp(kind, "browser") == 0) {
      ESP_LOGI(TAG, "Browser of svp_relay's host");
    } else {
      ESP_LOGI(TAG, "VNC desktop of %s:%u through svp_relay", host, port);
    }
#ifdef SVP_SPICE_USB
    UsbipLink links[SPICE_USB_SLOTS];
    const int listener = this->spice_usb_count_ > 0 ? usbip_listen() : -1;
    auto attached = [this, &links]() {
      uint8_t bits = 0;
      for (uint8_t i = 0; i < this->spice_usb_count_; i++)
        bits = static_cast<uint8_t>(bits | (links[i].fd >= 0 ? 1u << i : 0));
      if (this->usbip_attached_.exchange(bits, std::memory_order_acq_rel) != bits) {
        this->spice_changed_.store(true, std::memory_order_release);
        App.wake_loop_threadsafe();
      }
    };
    auto drop = [this, &links, &attached](uint8_t i) {
      this->spice_usb_[i]->link_close();
      ::close(links[i].fd);
      links[i].fd = -1;
      attached();
    };
    // A client's first request: the list of the slots' devices, or one of them to import.
    auto accept = [this, &links, &attached, listener]() {
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
        uint8_t *out = this->spice_scratch_;
        size_t at = 12;
        uint32_t count = 0;
        for (uint8_t i = 0; i < this->spice_usb_count_; i++) {
          const size_t n = this->spice_usb_[i]->usbip_entry(out + at, SPICE_SCRATCH_BYTES - at, true);
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
            n <= this->spice_usb_count_ && links[n - 1].fd < 0) {
          uint8_t entry[0x138];
          if (this->spice_usb_[n - 1]->usbip_entry(entry, sizeof(entry), false) != 0)
            slot = static_cast<int>(n - 1);
        }
      }
      // Its transfers from the pool: none, and the import is refused as for no device.
      if (slot >= 0 && !this->spice_usb_[slot]->link_open(&links[slot], SpiceUsbSlot::Proto::USBIP))
        slot = -1;
      if (slot < 0) {
        // Status 1: no such device here, or it is imported already (OP_REP_IMPORT ends with it).
        uint8_t reply[8];
        put_be16(reply, USBIP_VERSION);
        put_be16(reply + 2, USBIP_REP_IMPORT);
        put_be32(reply + 4, 1);
        send_all(fd, reply, sizeof(reply));
        ::close(fd);
        return;
      }
      // Answered once the slot has the device open (its service()).
      links[slot].fd = fd;
      attached();
      ESP_LOGI(TAG, "USB/IP: slot %d imported", slot + 1);
    };
#endif
    while (this->spice_request_.load(std::memory_order_acquire) == 0 &&
           !this->desktop_sender_gone_.load(std::memory_order_acquire)) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(this->spice_wake_fd_, &readable);
      int top = this->spice_wake_fd_;
#ifdef SVP_SPICE_USB
      if (listener >= 0) {
        FD_SET(listener, &readable);
        top = std::max(top, listener);
      }
      for (uint8_t i = 0; i < this->spice_usb_count_; i++) {
        size_t room = 0;
        if (links[i].fd >= 0)
          this->spice_usb_[i]->rx_space(room);
        if (room > 0) {  // else it waits for a free transfer or the device: read no further
          FD_SET(links[i].fd, &readable);
          top = std::max(top, links[i].fd);
        }
      }
#endif
      if (::select(top + 1, &readable, nullptr, nullptr, nullptr) < 0)
        break;
      if (FD_ISSET(this->spice_wake_fd_, &readable)) {
        uint64_t count;
        ::read(this->spice_wake_fd_, &count, sizeof(count));
      }
#ifdef SVP_SPICE_USB
      if (listener >= 0 && FD_ISSET(listener, &readable))
        accept();
      for (uint8_t i = 0; i < this->spice_usb_count_; i++) {
        if (links[i].fd < 0)
          continue;
        SpiceUsbSlot *slot = this->spice_usb_[i];
        bool ok = true;
        if (FD_ISSET(links[i].fd, &readable)) {
          size_t room = 0;
          uint8_t *into = slot->rx_space(room);
          const int got = room > 0 ? ::recv(links[i].fd, into, room, 0) : 0;
          ok = got > 0 && slot->rx_done(static_cast<size_t>(got));
        }
        if (!ok || !slot->service()) {
          ESP_LOGI(TAG, "USB/IP: slot %u detached", i + 1);
          drop(i);
        }
      }
#endif
    }
#ifdef SVP_SPICE_USB
    for (uint8_t i = 0; i < this->spice_usb_count_; i++) {
      if (links[i].fd >= 0)
        drop(i);
    }
    if (listener >= 0)
      ::close(listener);
#endif
    if (!this->desktop_sender_gone_.load(std::memory_order_acquire))
      this->desktop_relay_stop_();
    ESP_LOGI(TAG, "Disconnected");
  }
  this->desktop_relay_inputs_.store(false, std::memory_order_release);
  this->desktop_sender_wanted_.store(false, std::memory_order_release);
  this->desktop_release_panel_(panel_held, wifi_held);
}
#endif

void SimpleVideoPlayer::desktop_cursor_set_(const SpiceCursorShape *shape) {
  xSemaphoreTake(this->spice_cursor_lock_, portMAX_DELAY);
  if (this->spice_cursor_ == nullptr) {  // the session's buffers given back meanwhile
    xSemaphoreGive(this->spice_cursor_lock_);
    return;
  }
  SpiceCursorShape &now = this->spice_cursor_->shape;
  now.w = shape->w;
  now.h = shape->h;
  now.hot_x = shape->hot_x;
  now.hot_y = shape->hot_y;
  now.valid = shape->valid;
  if (shape->valid)
    std::memcpy(now.px, shape->px, static_cast<size_t>(shape->w) * shape->h * sizeof(uint32_t));
  this->spice_cursor_->visible = shape->valid;
  xSemaphoreGive(this->spice_cursor_lock_);
  this->desktop_cursor_changed_();
}

void SimpleVideoPlayer::desktop_cursor_changed_() {
  this->spice_cursor_dirty_.store(true, std::memory_order_release);
  // The draw task takes a null frame for a cursor change; a full queue brings it with the next frame.
  StreamFrame *none = nullptr;
  xQueueSend(this->stream_filled_q_, &none, 0);
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
#ifdef SVP_EMULATOR
  if (this->emu_refuses_())
    return;
#endif
  if (password.size() > PASSWORD_MAX) {
    ESP_LOGE(TAG, "The password is longer than %u characters", static_cast<unsigned>(PASSWORD_MAX));
    return;
  }
  this->spice_active_saved_ = -1;  // a saved one's is set by spice_connect_saved()
  xSemaphoreTake(this->spice_lock_, portMAX_DELAY);
  std::snprintf(this->spice_->host, sizeof(this->spice_->host), "%s", host.c_str());
  std::snprintf(this->spice_->password, sizeof(this->spice_->password), "%s", password.c_str());
  this->spice_->port = port;
  this->spice_browser_.store(false, std::memory_order_release);
  xSemaphoreGive(this->spice_lock_);
  // A connection still being made ends first; the task then connects anew.
  this->spice_request_.store(1, std::memory_order_release);
  this->spice_wake();
  xTaskNotifyGive(this->spice_task_);
}

void SimpleVideoPlayer::spice_connect_browser() {
#ifndef SVP_DESKTOP_VNC
  ESP_LOGE(TAG, "The Browser is a VNC desktop: this panel takes SPICE desktops only");
  return;
#endif
  if (this->spice_task_ == nullptr) {
    ESP_LOGE(TAG, "The SPICE client is not running");
    return;
  }
  if (this->spice_on_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "A SPICE session runs: disconnect it first");
    return;
  }
#ifdef SVP_EMULATOR
  if (this->emu_refuses_())
    return;
#endif
  this->spice_active_saved_ = -1;
  xSemaphoreTake(this->spice_lock_, portMAX_DELAY);
  this->spice_->host[0] = 0;
  this->spice_->password[0] = 0;
  this->spice_->port = 0;
  this->spice_browser_.store(true, std::memory_order_release);
  xSemaphoreGive(this->spice_lock_);
  this->spice_request_.store(1, std::memory_order_release);
  this->spice_wake();
  xTaskNotifyGive(this->spice_task_);
}

void SimpleVideoPlayer::spice_disconnect() {
  if (this->spice_task_ == nullptr)
    return;
  this->spice_request_.store(2, std::memory_order_release);
  this->spice_wake();
  xTaskNotifyGive(this->spice_task_);
}

void SimpleVideoPlayer::spice_wake() {
  if (this->spice_wake_fd_ < 0)
    return;
  const uint64_t one = 1;
  ::write(this->spice_wake_fd_, &one, sizeof(one));
}

#ifdef SVP_SPICE_USB
void SimpleVideoPlayer::add_spice_usb_slot(SpiceUsbSlot *slot) {
  if (this->spice_usb_count_ < SPICE_USB_SLOTS) {
    slot->set_index(this->spice_usb_count_);
    this->spice_usb_[this->spice_usb_count_++] = slot;
  }
}

void SimpleVideoPlayer::spice_usb_selected(SpiceUsbSlot *slot, uint8_t addr) {
  for (uint8_t i = 0; i < this->spice_usb_count_; i++) {
    if (this->spice_usb_[i] != slot)
      this->spice_usb_[i]->forget_selection(addr);
  }
}
#endif

void SimpleVideoPlayer::spice_sound_take_() {
  if (!this->spice_sound_on_.load(std::memory_order_acquire))
    return;
#ifdef SVP_AUDIO_FILES
  if (this->spice_sound_shares_files_ && this->audio_session_.load(std::memory_order_acquire)) {
    ESP_LOGI(TAG, "Audio file stopped: the SPICE session's sound plays on its device");
    this->end_session_();
  }
#endif
#ifdef SVP_MEDIA_PLAYER
  if (this->spice_sound_shares_media_player_ && this->media_player_ != nullptr &&
      (this->media_player_->state == media_player::MEDIA_PLAYER_STATE_PLAYING ||
       this->media_player_->state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING)) {
    ESP_LOGI(TAG, "Media player stopped: the SPICE session's sound plays on its device");
    this->media_player_->make_call().set_command(media_player::MEDIA_PLAYER_COMMAND_STOP).perform();
  }
#endif
}

//========================================================================
// SPICE cursor on the output (draw task)
//========================================================================

bool SimpleVideoPlayer::spice_cursor_over_(int32_t x, int32_t y, int32_t w, int32_t h) const {
  return this->spice_cursor_drawn_ && x < this->spice_cursor_rx_ + this->spice_cursor_rw_ &&
         this->spice_cursor_rx_ < x + w && y < this->spice_cursor_ry_ + this->spice_cursor_rh_ &&
         this->spice_cursor_ry_ < y + h;
}

void SimpleVideoPlayer::spice_out_read_(int32_t x, int32_t y, int32_t w, int32_t h, uint8_t *dst) {
#ifdef SVP_DSI_OUTPUT
  if (this->spice_direct_out_.load(std::memory_order_acquire)) {
    display::ColorBitness bitness;
    const uint8_t *fb = this->dsi_->get_draw_buffer(&bitness);
    const size_t bpp = this->dsi_fb_bpp_;
    const size_t stride = static_cast<size_t>(this->dsi_out_w_) * bpp;
    // The display's DMA wrote these rows: what the cache holds of them is dropped first (the CPU
    // wrote none of them back unsynced: LVGL is paused, draw_bitmap() writes its copies back).
    static constexpr uintptr_t LINE = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    const uintptr_t fb_start = reinterpret_cast<uintptr_t>(fb);
    const uintptr_t fb_end = (fb_start + stride * this->dsi_out_h_) & ~(LINE - 1);
    const uintptr_t from = (fb_start + static_cast<size_t>(y) * stride) & ~(LINE - 1);
    uintptr_t to = (fb_start + static_cast<size_t>(y + h) * stride + LINE - 1) & ~(LINE - 1);
    to = std::min(to, fb_end);
    if (to > from)
      esp_cache_msync(reinterpret_cast<void *>(from), to - from,
                      ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    for (int32_t row = 0; row < h; row++)
      std::memcpy(dst + static_cast<size_t>(row) * w * bpp, fb + (static_cast<size_t>(y) + row) * stride + x * bpp,
                  static_cast<size_t>(w) * bpp);
    return;
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  const uint8_t *page = this->output_buffers_[0];
  const size_t page_stride = static_cast<size_t>((this->canvas_size_w_ + 15) & ~15) * 3;
  for (int32_t row = 0; row < h; row++)
    std::memcpy(dst + static_cast<size_t>(row) * w * 3, page + (static_cast<size_t>(y) + row) * page_stride + x * 3,
                static_cast<size_t>(w) * 3);
#endif
}

void SimpleVideoPlayer::spice_out_write_(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *src) {
#ifdef SVP_DSI_OUTPUT
  if (this->spice_direct_out_.load(std::memory_order_acquire)) {
    const display::ColorBitness bitness =
        this->dsi_fb_bpp_ == 3 ? display::COLOR_BITNESS_888 : display::COLOR_BITNESS_565;
    this->dsi_->draw_pixels_at(x, y, w, h, src, bitness, false, 0, 0, 0);
    return;
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  uint8_t *page = this->output_buffers_[0];
  const size_t page_stride = static_cast<size_t>((this->canvas_size_w_ + 15) & ~15) * 3;
  for (int32_t row = 0; row < h; row++)
    std::memcpy(page + (static_cast<size_t>(y) + row) * page_stride + x * 3, src + static_cast<size_t>(row) * w * 3,
                static_cast<size_t>(w) * 3);
  // As stream_copy_to_canvas_(): written back, and the part LVGL shows anew.
  uint8_t *first = page + static_cast<size_t>(y) * page_stride + static_cast<size_t>(x) * 3;
  uint8_t *last = page + (static_cast<size_t>(y) + h - 1) * page_stride + (static_cast<size_t>(x) + w) * 3;
  esp_cache_msync(first, last - first,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  portENTER_CRITICAL(&this->stream_dirty_lock_);
  if (this->stream_dirty_) {
    this->stream_dirty_x1_ = std::min<int32_t>(this->stream_dirty_x1_, x);
    this->stream_dirty_y1_ = std::min<int32_t>(this->stream_dirty_y1_, y);
    this->stream_dirty_x2_ = std::max<int32_t>(this->stream_dirty_x2_, x + w - 1);
    this->stream_dirty_y2_ = std::max<int32_t>(this->stream_dirty_y2_, y + h - 1);
  } else {
    this->stream_dirty_x1_ = x;
    this->stream_dirty_y1_ = y;
    this->stream_dirty_x2_ = x + w - 1;
    this->stream_dirty_y2_ = y + h - 1;
    this->stream_dirty_ = true;
  }
  portEXIT_CRITICAL(&this->stream_dirty_lock_);
#endif
}

void SimpleVideoPlayer::spice_cursor_hide_() {
  if (!this->spice_cursor_drawn_ || this->spice_cursor_under_ == nullptr)
    return;
  this->spice_cursor_drawn_ = false;
  this->spice_out_write_(this->spice_cursor_rx_, this->spice_cursor_ry_, this->spice_cursor_rw_, this->spice_cursor_rh_,
                         this->spice_cursor_under_);
}

void SimpleVideoPlayer::spice_cursor_show_() {
  if (this->spice_cursor_drawn_)
    return;
  int32_t out_w = 0, out_h = 0;
  size_t bpp = 3;
  const bool direct = this->spice_direct_out_.load(std::memory_order_acquire);
#ifdef SVP_DSI_OUTPUT
  if (direct) {
    display::ColorBitness bitness;
    if (this->dsi_->get_draw_buffer(&bitness) == nullptr)
      return;  // nothing to read what is under it back from
    out_w = this->dsi_out_w_;
    out_h = this->dsi_out_h_;
    bpp = this->dsi_fb_bpp_;
  }
#endif
#ifdef SVP_CANVAS_OUTPUT
  if (!direct) {
    out_w = this->canvas_size_w_;
    out_h = this->canvas_size_h_;
  }
#endif
  xSemaphoreTake(this->spice_cursor_lock_, portMAX_DELAY);
  if (this->spice_cursor_ == nullptr || this->spice_cursor_under_ == nullptr || this->spice_cursor_patch_ == nullptr) {
    xSemaphoreGive(this->spice_cursor_lock_);  // the session's buffers given back meanwhile
    return;
  }
  const SpiceCursor &c = *this->spice_cursor_;
  const SpiceCursorShape &s = c.shape;
  if (!c.visible || !s.valid) {
    xSemaphoreGive(this->spice_cursor_lock_);
    return;
  }
  const int32_t left = c.x - s.hot_x, top = c.y - s.hot_y;
  const int32_t x0 = std::max<int32_t>(left, 0), y0 = std::max<int32_t>(top, 0);
  const int32_t x1 = std::min<int32_t>(left + s.w, out_w), y1 = std::min<int32_t>(top + s.h, out_h);
  if (x1 <= x0 || y1 <= y0) {
    xSemaphoreGive(this->spice_cursor_lock_);
    return;
  }
  const int32_t w = x1 - x0, h = y1 - y0;
  this->spice_out_read_(x0, y0, w, h, this->spice_cursor_under_);
  // The shape over what is under it, in the output's layout: B, G, R, or RGB565 little-endian.
  for (int32_t row = 0; row < h; row++) {
    const uint32_t *src = s.px + static_cast<size_t>(y0 - top + row) * s.w + (x0 - left);
    const uint8_t *under = this->spice_cursor_under_ + static_cast<size_t>(row) * w * bpp;
    uint8_t *out = this->spice_cursor_patch_ + static_cast<size_t>(row) * w * bpp;
    for (int32_t col = 0; col < w; col++, under += bpp, out += bpp) {
      const uint32_t v = src[col];
      const uint32_t a = v >> 24;
      uint32_t b, g, r;
      if (bpp == 3) {
        b = under[0];
        g = under[1];
        r = under[2];
      } else {
        const uint16_t u = static_cast<uint16_t>(under[0] | (under[1] << 8));
        r = ((u >> 11) & 0x1F) << 3;
        g = ((u >> 5) & 0x3F) << 2;
        b = (u & 0x1F) << 3;
      }
      if (v == SPICE_CURSOR_INVERT) {
        b = 255 - b;
        g = 255 - g;
        r = 255 - r;
      } else if (a == 255) {
        b = v & 0xFF;
        g = (v >> 8) & 0xFF;
        r = (v >> 16) & 0xFF;
      } else if (a != 0) {
        b = (((v & 0xFF) * a) + b * (255 - a)) / 255;
        g = ((((v >> 8) & 0xFF) * a) + g * (255 - a)) / 255;
        r = ((((v >> 16) & 0xFF) * a) + r * (255 - a)) / 255;
      } else if (bpp == 2) {
        out[0] = under[0];  // as it was, not through 8 bits a channel
        out[1] = under[1];
        continue;
      }
      if (bpp == 3) {
        out[0] = static_cast<uint8_t>(b);
        out[1] = static_cast<uint8_t>(g);
        out[2] = static_cast<uint8_t>(r);
      } else {
        const uint16_t o = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        out[0] = static_cast<uint8_t>(o & 0xFF);
        out[1] = static_cast<uint8_t>(o >> 8);
      }
    }
  }
  xSemaphoreGive(this->spice_cursor_lock_);
  this->spice_out_write_(x0, y0, w, h, this->spice_cursor_patch_);
  this->spice_cursor_rx_ = x0;
  this->spice_cursor_ry_ = y0;
  this->spice_cursor_rw_ = w;
  this->spice_cursor_rh_ = h;
  this->spice_cursor_drawn_ = true;
}

void SimpleVideoPlayer::spice_cursor_update_() {
  if (!this->spice_on_.load(std::memory_order_acquire) || !this->spice_cursor_dirty_.load(std::memory_order_acquire))
    return;
  bool ok = false;
#ifdef SVP_DSI_OUTPUT
  if (this->spice_direct_out_.load(std::memory_order_acquire))
    ok = this->stream_output_ok_.load(std::memory_order_acquire);
#endif
#ifdef SVP_CANVAS_OUTPUT
  if (!this->spice_direct_out_.load(std::memory_order_acquire))
    ok = this->stream_connected_.load(std::memory_order_acquire) && this->ensure_canvas_outputs_();
#endif
  // Not drawable now: the next picture brings it.
  if (!ok || this->picture_session_busy_() || xSemaphoreTake(this->output_lock_, 0) != pdTRUE)
    return;
  if (this->spice_cursor_dirty_.exchange(false, std::memory_order_acq_rel)) {
    this->spice_cursor_hide_();
    this->spice_cursor_show_();
  }
  xSemaphoreGive(this->output_lock_);
}

//========================================================================
// Saved connections: name, host, port in preferences, or with their USB devices in the connections
// file (the password never: it is given at each connect)
//========================================================================

void SimpleVideoPlayer::spice_saved_setup_() {
  if (!this->spice_file_path_.empty()) {
    for (uint8_t i = 0; i < this->spice_saved_count_; i++)
      this->spice_saved_[i] = SpiceSaved{};
    std::memset(this->spice_saved_usb_, 0, sizeof(SpiceSavedUsb) * this->spice_saved_count_ * SPICE_SAVED_USB);
    if (storage::global_storage_registry == nullptr)
      return;
    // Its storage may come later (a card put in): read, or written, once it is there.
    storage::global_storage_registry->add_on_registered_callback([this](storage::Storage *) {
      if (!this->spice_file_loaded_) {
        this->spice_file_load_();
      } else if (this->spice_file_dirty_) {
        this->spice_file_save_();
      }
    });
    this->spice_file_load_();
    return;
  }
  const uint32_t key = fnv1_hash("simple_video_player.spice_saved");
  for (uint8_t i = 0; i < this->spice_saved_count_; i++) {
    new (&this->spice_saved_prefs_[i]) ESPPreferenceObject(global_preferences->make_preference<SpiceSaved>(key + i));
    SpiceSaved &entry = this->spice_saved_[i];
    if (!this->spice_saved_prefs_[i].load(&entry))
      entry = SpiceSaved{};
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.host[sizeof(entry.host) - 1] = '\0';
    entry.used = entry.used && entry.host[0] != '\0';
  }
}

bool SimpleVideoPlayer::spice_saved(uint8_t index, std::string &name, std::string &host, uint16_t &port) const {
  if (this->spice_saved_ == nullptr || index >= this->spice_saved_count_ || !this->spice_saved_[index].used)
    return false;
  const SpiceSaved &entry = this->spice_saved_[index];
  name = entry.name;
  host = entry.host;
  port = entry.port;
  return true;
}

void SimpleVideoPlayer::spice_save(uint8_t index, const std::string &name, const std::string &host, uint16_t port) {
  if (this->spice_saved_ == nullptr || index >= this->spice_saved_count_) {
    ESP_LOGE(TAG, "No saved connection %u (spice: connections)", index);
    return;
  }
  if (host.empty() || host.size() >= sizeof(SpiceSaved::host)) {
    ESP_LOGE(TAG, "The host must be 1 to %u characters", static_cast<unsigned>(sizeof(SpiceSaved::host) - 1));
    return;
  }
  SpiceSaved &entry = this->spice_saved_[index];
  entry = SpiceSaved{};
  std::snprintf(entry.name, sizeof(entry.name), "%s", name.empty() ? host.c_str() : name.c_str());
  std::snprintf(entry.host, sizeof(entry.host), "%s", host.c_str());
  entry.port = port;
  entry.used = true;
  this->spice_saved_changed_(index);
}

// The entry at index changed: kept where the connections are.
void SimpleVideoPlayer::spice_saved_changed_(uint8_t index) {
  if (this->spice_file_path_.empty()) {
    this->spice_saved_prefs_[index].save(&this->spice_saved_[index]);
  } else {
    this->spice_file_save_();
  }
  this->spice_callbacks_.call();
}

int SimpleVideoPlayer::spice_save_new(const std::string &name, const std::string &host, uint16_t port) {
  for (uint8_t i = 0; this->spice_saved_ != nullptr && i < this->spice_saved_count_; i++) {
    if (!this->spice_saved_[i].used) {
      this->spice_save(i, name, host, port);
      return this->spice_saved_[i].used ? i : -1;
    }
  }
  ESP_LOGW(TAG, "All %u saved connections are taken (spice: connections)", this->spice_saved_count_);
  return -1;
}

void SimpleVideoPlayer::spice_forget(uint8_t index) {
  if (this->spice_saved_ == nullptr || index >= this->spice_saved_count_)
    return;
  this->spice_saved_[index] = SpiceSaved{};
  if (this->spice_saved_usb_ != nullptr)
    std::memset(&this->spice_saved_usb_[index * SPICE_SAVED_USB], 0, sizeof(SpiceSavedUsb) * SPICE_SAVED_USB);
  if (this->spice_active_saved_ == index)
    this->spice_active_saved_ = -1;
  this->spice_saved_changed_(index);
}

void SimpleVideoPlayer::spice_connect_saved(uint8_t index, const std::string &password) {
  std::string name, host;
  uint16_t port;
  if (!this->spice_saved(index, name, host, port)) {
    ESP_LOGE(TAG, "No saved connection %u", index);
    return;
  }
  this->spice_connect(host, port, password);
  if (this->spice_saved_usb_ == nullptr)
    return;
  this->spice_active_saved_ = static_cast<int8_t>(index);
#ifdef SVP_SPICE_USB
  // The slots take this connection's devices (where plugged in, else as they come).
  this->spice_usb_applying_ = true;
  for (uint8_t i = 0; i < this->spice_usb_count_ && i < SPICE_SAVED_USB; i++) {
    const SpiceSavedUsb &usb = this->spice_saved_usb_[index * SPICE_SAVED_USB + i];
    this->spice_usb_[i]->want_device(usb.vid, usb.pid, usb.serial);
  }
  this->spice_usb_applying_ = false;
#endif
}

#ifdef SVP_SPICE_USB
void SimpleVideoPlayer::spice_usb_changed(SpiceUsbSlot *slot) {
  if (this->spice_usb_applying_ || this->spice_active_saved_ < 0 || this->spice_saved_usb_ == nullptr)
    return;
  for (uint8_t i = 0; i < this->spice_usb_count_ && i < SPICE_SAVED_USB; i++) {
    if (this->spice_usb_[i] != slot)
      continue;
    SpiceSavedUsb &usb = this->spice_saved_usb_[this->spice_active_saved_ * SPICE_SAVED_USB + i];
    SpiceSavedUsb now{};
    slot->selected_device(now.vid, now.pid, now.serial, sizeof(now.serial));
    if (now.vid == usb.vid && now.pid == usb.pid && std::strcmp(now.serial, usb.serial) == 0)
      return;
    usb = now;
    this->spice_saved_changed_(static_cast<uint8_t>(this->spice_active_saved_));
  }
}
#endif

//------------------------------------------------------------------------
// The connections file: "index\tname\thost\tport" and per USB slot "\tvid:pid:serial" ("-": none),
// one line per connection. Read and written whole, on the main loop through the storage worker (each
// step after the last one's completion, the next one deferred to the loop).
//------------------------------------------------------------------------

void SimpleVideoPlayer::spice_file_load_() {
  storage::StorageWorker *worker = storage::global_storage_worker;
  if (this->spice_file_busy_ || this->spice_file_loaded_ || worker == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->spice_file_path_.c_str(), &rel);
  if (ps == nullptr)
    return;  // its storage registers later
  this->spice_file_busy_ = true;
  this->spice_file_len_ = 0;
  const storage::StorageError err =
      worker->begin_read(ps, rel, &this->spice_file_handle_, [this](storage::StorageError e) {
        if (e == storage::StorageError::STORAGE_ERROR_OK) {
          this->defer([this]() { this->spice_file_read_(); });
          return;
        }
        this->spice_file_busy_ = false;
        if (e == storage::StorageError::STORAGE_ERROR_NOT_FOUND) {
          // None yet: made with the first connection saved.
          this->spice_file_loaded_ = true;
          if (this->spice_file_dirty_)
            this->spice_file_save_();
        } else {
          ESP_LOGW(TAG, "%s not read: %s", this->spice_file_path_.c_str(), storage::error_to_string(e));
        }
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->spice_file_busy_ = false;
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->set_timeout("spice_file", 500, [this]() { this->spice_file_load_(); });
  }
}

void SimpleVideoPlayer::spice_file_read_() {
  const size_t room = this->spice_file_bytes_() - 1 - this->spice_file_len_;
  if (room == 0) {
    this->spice_file_read_done_(storage::StorageError::STORAGE_ERROR_OK);
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->spice_file_handle_, reinterpret_cast<uint8_t *>(this->spice_file_buf_) + this->spice_file_len_, room,
      &this->spice_file_got_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK || this->spice_file_got_ == 0) {
          this->defer([this, e]() { this->spice_file_read_done_(e); });
          return;
        }
        this->spice_file_len_ += this->spice_file_got_;
        this->defer([this]() { this->spice_file_read_(); });
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY) {
    this->defer([this]() { this->spice_file_read_(); });
  } else if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->spice_file_read_done_(err);
  }
}

void SimpleVideoPlayer::spice_file_read_done_(storage::StorageError err) {
  const storage::StorageError closed =
      storage::global_storage_worker->end_read(this->spice_file_handle_, [this, err](storage::StorageError) {
        this->spice_file_busy_ = false;
        if (err != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not read: %s", this->spice_file_path_.c_str(), storage::error_to_string(err));
          return;
        }
        this->spice_file_parse_();
        this->spice_file_loaded_ = true;
        this->spice_callbacks_.call();
        if (this->spice_file_dirty_)
          this->spice_file_save_();
      });
  if (closed == storage::StorageError::STORAGE_ERROR_NOT_READY)
    this->defer([this, err]() { this->spice_file_read_done_(err); });
}

void SimpleVideoPlayer::spice_file_parse_() {
  char *text = this->spice_file_buf_;
  text[this->spice_file_len_] = '\0';
  for (char *line = text; line != nullptr && *line != '\0';) {
    char *next = std::strchr(line, '\n');
    if (next != nullptr)
      *next++ = '\0';
    char *fields[4 + SPICE_SAVED_USB] = {};
    size_t count = 0;
    for (char *field = line; field != nullptr && count < 4 + SPICE_SAVED_USB;) {
      fields[count++] = field;
      field = std::strchr(field, '\t');
      if (field != nullptr)
        *field++ = '\0';
    }
    line = next;
    if (count < 4)
      continue;
    const unsigned long index = std::strtoul(fields[0], nullptr, 10);
    if (index >= this->spice_saved_count_ || fields[2][0] == '\0')
      continue;
    SpiceSaved &entry = this->spice_saved_[index];
    std::snprintf(entry.name, sizeof(entry.name), "%s", fields[1]);
    std::snprintf(entry.host, sizeof(entry.host), "%s", fields[2]);
    entry.port = static_cast<uint16_t>(std::strtoul(fields[3], nullptr, 10));
    entry.used = true;
    for (size_t i = 0; i < SPICE_SAVED_USB; i++) {
      SpiceSavedUsb &usb = this->spice_saved_usb_[index * SPICE_SAVED_USB + i];
      usb = SpiceSavedUsb{};
      unsigned vid = 0, pid = 0;
      int at = 0;
      if (4 + i < count && std::sscanf(fields[4 + i], "%4x:%4x:%n", &vid, &pid, &at) == 2 && at > 0) {
        usb.vid = static_cast<uint16_t>(vid);
        usb.pid = static_cast<uint16_t>(pid);
        std::snprintf(usb.serial, sizeof(usb.serial), "%s", fields[4 + i] + at);
      }
    }
  }
}

// Written whole once the file was read (never over one not read yet); a change meanwhile writes again.
void SimpleVideoPlayer::spice_file_save_() {
  if (this->spice_file_path_.empty())
    return;
  this->spice_file_dirty_ = true;
  if (!this->spice_file_loaded_ || this->spice_file_busy_ || storage::global_storage_worker == nullptr)
    return;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(this->spice_file_path_.c_str(), &rel);
  if (ps == nullptr)
    return;  // written once its storage is there
  // Tabs and line breaks would end a field or line: spaces in their place.
  auto clean = [](const char *text, char *out, size_t size) {
    std::snprintf(out, size, "%s", text);
    for (char *c = out; *c != '\0'; c++) {
      if (*c == '\t' || *c == '\n' || *c == '\r')
        *c = ' ';
    }
  };
  size_t len = 0;
  const size_t cap = this->spice_file_bytes_();
  for (uint8_t i = 0; i < this->spice_saved_count_; i++) {
    const SpiceSaved &entry = this->spice_saved_[i];
    if (!entry.used)
      continue;
    char name[sizeof(entry.name)], host[sizeof(entry.host)];
    clean(entry.name, name, sizeof(name));
    clean(entry.host, host, sizeof(host));
    len += std::snprintf(this->spice_file_buf_ + len, cap - len, "%u\t%s\t%s\t%u", i, name, host, entry.port);
    for (uint8_t u = 0; u < SPICE_SAVED_USB && len < cap; u++) {
      const SpiceSavedUsb &usb = this->spice_saved_usb_[i * SPICE_SAVED_USB + u];
      char serial[sizeof(usb.serial)];
      clean(usb.serial, serial, sizeof(serial));
      if (usb.vid == 0) {
        len += std::snprintf(this->spice_file_buf_ + len, cap - len, "\t-");
      } else {
        len += std::snprintf(this->spice_file_buf_ + len, cap - len, "\t%04x:%04x:%s", usb.vid, usb.pid, serial);
      }
    }
    if (len < cap)
      len += std::snprintf(this->spice_file_buf_ + len, cap - len, "\n");
    len = std::min(len, cap - 1);
  }
  this->spice_file_len_ = len;
  this->spice_file_busy_ = true;
  this->spice_file_dirty_ = false;
  const storage::StorageError err =
      storage::global_storage_worker->begin_write(ps, rel, &this->spice_file_handle_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->spice_file_path_.c_str(), storage::error_to_string(e));
          this->spice_file_busy_ = false;
          this->spice_file_dirty_ = true;
          return;
        }
        this->defer([this]() { this->spice_file_write_(); });
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->spice_file_busy_ = false;
    this->spice_file_dirty_ = true;
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->set_timeout("spice_file", 500, [this]() { this->spice_file_save_(); });
  }
}

void SimpleVideoPlayer::spice_file_write_() {
  auto close = [this]() {
    const storage::StorageError err =
        storage::global_storage_worker->end_write(this->spice_file_handle_, [this](storage::StorageError) {
          this->spice_file_busy_ = false;
          if (this->spice_file_dirty_)
            this->defer([this]() { this->spice_file_save_(); });
        });
    if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
      this->defer([this]() { this->spice_file_write_(); });
  };
  if (this->spice_file_len_ == 0) {
    close();
    return;
  }
  const storage::StorageError err = storage::global_storage_worker->write_chunk(
      this->spice_file_handle_, reinterpret_cast<const uint8_t *>(this->spice_file_buf_), this->spice_file_len_,
      [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          ESP_LOGW(TAG, "%s not written: %s", this->spice_file_path_.c_str(), storage::error_to_string(e));
          this->spice_file_dirty_ = true;
        }
        this->spice_file_len_ = 0;  // written: what is left is to close it
        this->defer([this]() { this->spice_file_write_(); });
      });
  if (err == storage::StorageError::STORAGE_ERROR_NOT_READY)
    this->defer([this]() { this->spice_file_write_(); });
  else if (err != storage::StorageError::STORAGE_ERROR_OK)
    close();
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
    const bool browser = this->spice_browser_.load(std::memory_order_acquire);
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
    this->spice_scratch_ = this->pool_.alloc(SPICE_SCRATCH_BYTES);
    if (this->spice_scratch_ == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for the desktop's %u B message buffer", static_cast<unsigned>(SPICE_SCRATCH_BYTES));
      this->spice_failed_.store(true, std::memory_order_release);
      this->spice_changed_.store(true, std::memory_order_release);
      App.wake_loop_threadsafe();
      continue;
    }
    this->spice_failed_.store(false, std::memory_order_release);
    this->spice_connecting_.store(true, std::memory_order_release);
    this->spice_changed_.store(true, std::memory_order_release);
    App.wake_loop_threadsafe();
    this->desktop_took_ = false;
    this->desktop_sender_gone_.store(false, std::memory_order_release);
    // A VNC server greets first, a SPICE server waits for the client. The Browser's server is
    // started by the relay: no probe.
    const DesktopKind kind = browser ? DesktopKind::VNC : this->desktop_probe_(host, port);
    if (this->spice_request_.load(std::memory_order_acquire) != 0) {
      // Asked anew meanwhile.
    } else if (kind == DesktopKind::VNC) {
#ifdef SVP_DESKTOP_VNC
      this->desktop_vnc_(browser ? "browser" : "vnc", host, port, password);
#else
      ESP_LOGE(TAG, "%s:%u is a VNC server: this panel takes SPICE desktops only", host, port);
#endif
    } else if (kind == DesktopKind::SPICE) {
#ifdef SVP_DESKTOP_SPICE
      SpiceSession session(this);
      session.run(host, port, password);
#else
      ESP_LOGE(TAG, "%s:%u is a SPICE server: this panel takes VNC desktops only", host, port);
#endif
    }
    this->pool_.free(this->spice_scratch_);
    this->spice_scratch_ = nullptr;
    std::memset(password, 0, sizeof(password));
    this->spice_failed_.store(!this->desktop_took_, std::memory_order_release);
    this->spice_connecting_.store(false, std::memory_order_release);
    this->spice_changed_.store(true, std::memory_order_release);
    App.wake_loop_threadsafe();
  }
}

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE
