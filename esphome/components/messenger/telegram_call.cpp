#include "telegram_call.h"

#ifdef USE_MESSENGER_CALL

#include "tl_read.h"

#include "esphome/components/json/json_util.h"
#include "esphome/components/mtproto/crypto.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_peer.h>
#include <esp_peer_default.h>
#include <esp_random.h>
#include <freertos/idf_additions.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <strings.h>
#include <unistd.h>

namespace esphome::messenger {

static const char *const TAG = "messenger.call";

using mtproto::TlReader;
using mtproto::TlWriter;
using mtproto::tl_field;
using mtproto::tl_hash;
using mtproto::tl_skip_object;

namespace {

constexpr uint32_t ID_UPDATE_SHORT = 0x78d4dec1;
constexpr uint32_t ID_UPDATES = 0x74ae4240;
constexpr uint32_t ID_UPDATES_COMBINED = 0x725b04c3;
constexpr uint32_t ID_UPDATE_PHONE_CALL = 0xab0f6b1e;
constexpr uint32_t ID_UPDATE_PHONE_CALL_SIGNALING = 0x2661bf09;
constexpr uint32_t ID_GET_DH_CONFIG = 0x26cf8950;
constexpr uint32_t ID_DH_CONFIG = 0x2c221edd;
constexpr uint32_t ID_REQUEST_CALL = 0x42ff96ed;
constexpr uint32_t ID_ACCEPT_CALL = 0x3bd2b4a0;
constexpr uint32_t ID_CONFIRM_CALL = 0x2efe1722;
constexpr uint32_t ID_RECEIVED_CALL = 0x17d54f61;
constexpr uint32_t ID_DISCARD_CALL = 0xb2cbc1c0;
constexpr uint32_t ID_SEND_SIGNALING = 0xff7a9383;
constexpr uint32_t ID_PHONE_PHONE_CALL = 0xec82e140;
constexpr uint32_t ID_PROTOCOL = 0xfc878fc8;
constexpr uint32_t ID_CALL_WAITING = 0xc5226f17;
constexpr uint32_t ID_CALL_REQUESTED = 0x14b0ed0c;
constexpr uint32_t ID_CALL_ACCEPTED = 0x3660c311;
constexpr uint32_t ID_CALL = 0x30535af5;
constexpr uint32_t ID_CALL_DISCARDED = 0x50ca4de1;
constexpr uint32_t ID_CONNECTION_WEBRTC = 0x635fe375;
constexpr uint32_t ID_INPUT_PHONE_CALL = 0x1e36fded;
constexpr uint32_t ID_INPUT_USER = 0xf21158c6;
constexpr uint32_t ID_REASON_HANGUP = 0x57adc690;
constexpr uint32_t ID_REASON_BUSY = 0xfaf7e8c9;
constexpr uint32_t ID_REASON_DISCONNECT = 0xe095c1a0;
constexpr int32_t MIN_LAYER = 65;
constexpr int32_t MAX_LAYER = 92;
constexpr const char *LIBRARY_VERSION = "7.0.0";

constexpr uint8_t EVENT_UPDATE = 0;
constexpr uint8_t EVENT_SIGNALING = 1;
constexpr uint8_t EVENT_HANGUP = 2;
constexpr uint8_t EVENT_REMOTE_END = 3;
constexpr uint8_t EVENT_DECLINE = 4;

constexpr size_t QUEUE_DEPTH = 24;
constexpr uint32_t SAMPLE_RATE = 48000;
constexpr size_t MAX_PACKET = 1500;
constexpr uint32_t PUMP_MS = 10;
constexpr uint32_t RPC_WAIT_MS = 20000;
constexpr size_t MAX_SERVERS = 8;
constexpr size_t MAX_CANDIDATES = 16;
constexpr size_t SDP_SIZE = 2048;
constexpr int DEFAULT_PT = 111;
constexpr size_t TX_RING_BYTES = 4096;
constexpr size_t MAX_FRAME = 1280;
constexpr int VIDEO_WIDTH = 1280;
constexpr int VIDEO_HEIGHT = 720;
constexpr int VIDEO_FPS = 30;
constexpr size_t VIDEO_RING_BYTES = 256 * 1024;
constexpr size_t VIDEO_SEND_MAX = 16 * 1024;
constexpr size_t MAX_VIDEO_GROUPS = 2;
constexpr size_t MAX_GROUP_SSRCS = 4;
constexpr size_t MAX_VIDEO_PARAMS = 4;
constexpr uint8_t RELAY_HANGUP = 'X';

struct Event {
  uint8_t type;
  size_t length;
  uint8_t data[];
};

}

struct CallSession {
  QueueHandle_t events;
  TaskHandle_t waiter;
  bool outgoing;
  bool ended;
  int64_t user_id;
  int64_t user_hash;
  int64_t call_id;
  int64_t call_hash;
  uint8_t g_a_hash[32];
  uint32_t g;
  uint8_t prime[mtproto::DH_BYTES];
  uint8_t secret[mtproto::DH_BYTES];
  uint8_t own_public[mtproto::DH_BYTES];
  uint8_t key[mtproto::DH_BYTES];
  bool have_key;
  int64_t fingerprint;
  bool media;
  uint32_t seq_out;
  uint32_t seen[64];
  uint8_t seen_count;
  esp_peer_handle_t peer;
  esp_peer_ice_server_cfg_t servers[MAX_SERVERS];
  char server_url[MAX_SERVERS][64];
  char server_user[MAX_SERVERS][96];
  char server_pass[MAX_SERVERS][96];
  uint8_t server_count;
  uint8_t packet[MAX_PACKET];
  uint32_t ts_base;
  bool ts_set;
  ring_buffer::RingBuffer *tx;
  uint16_t tx_pending;
  uint8_t frame[MAX_FRAME];
  char local_ufrag[64];
  char local_pwd[128];
  char local_hash[16];
  char local_fingerprint[128];
  char local_mid[16];
  int local_pt;
  uint32_t local_ssrc;
  bool local_ready;
  char remote_ufrag[64];
  char remote_pwd[128];
  char remote_hash[16];
  char remote_fingerprint[128];
  uint32_t remote_ssrc;
  bool remote_set;
  char candidates[MAX_CANDIDATES][256];
  uint8_t candidate_count;
  char offer_id[16];
  char pending_offer_id[24];
  uint32_t pending_offer_ssrc;
  bool pending_offer;
  char sdp[SDP_SIZE];
  bool video;
  bool connected;
  bool sdp_dirty;
  bool relay_hangup;
  char local_vmid[16];
  int local_vpt;
  uint32_t remote_vssrc;
  bool pending_offer_video;
  uint32_t video_groups[MAX_VIDEO_GROUPS][MAX_GROUP_SSRCS];
  uint8_t video_group_size[MAX_VIDEO_GROUPS];
  char video_group_semantics[MAX_VIDEO_GROUPS][8];
  uint8_t video_group_count;
  char video_param_name[MAX_VIDEO_PARAMS][32];
  char video_param_value[MAX_VIDEO_PARAMS][32];
  uint8_t video_param_count;
  int relay_fd;
  bool relay_connected;
  ring_buffer::RingBuffer *video_ring;
  bool video_wait_key;
  uint8_t *video_item;
  size_t video_item_len;
  size_t video_item_off;
  esp_peer_cfg_t peer_cfg;
  esp_peer_default_cfg_t peer_extra;
  TelegramCall *owner;
};

namespace {

std::atomic<uint8_t> peer_users{0};

void put_be32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

uint32_t get_be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void signaling_key_iv(const uint8_t *key, const uint8_t *msg_key, size_t x, uint8_t *aes_key, uint8_t *aes_iv) {
  uint8_t a[32];
  uint8_t b[32];
  mtproto::sha256_parts(msg_key, 16, key + x, 36, a);
  mtproto::sha256_parts(key + 40 + x, 36, msg_key, 16, b);
  memcpy(aes_key, a, 8);
  memcpy(aes_key + 8, b + 8, 16);
  memcpy(aes_key + 24, a + 24, 8);
  memcpy(aes_iv, b, 4);
  memcpy(aes_iv + 4, a + 8, 8);
  memcpy(aes_iv + 12, b + 24, 4);
}

bool line_value(const char *sdp, const char *prefix, char *out, size_t size) {
  const char *at = strstr(sdp, prefix);
  if (at == nullptr) {
    out[0] = '\0';
    return false;
  }
  at += strlen(prefix);
  size_t n = strcspn(at, "\r\n");
  tl_read::copy(out, size, at, n);
  return true;
}

uint32_t json_u32(JsonVariant value) {
  if (value.is<const char *>())
    return static_cast<uint32_t>(strtoul(value.as<const char *>(), nullptr, 10));
  return value.as<uint32_t>();
}

bool section_of(const char *sdp, const char *media, const char **begin, const char **end) {
  const char *at = strstr(sdp, media);
  while (at != nullptr && at != sdp && at[-1] != '\n')
    at = strstr(at + 1, media);
  if (at == nullptr)
    return false;
  const char *next = strstr(at + 2, "\nm=");
  *begin = at;
  *end = next != nullptr ? next : at + strlen(at);
  return true;
}

bool section_value(const char *begin, const char *end, const char *prefix, char *out, size_t size) {
  const size_t len = strlen(prefix);
  for (const char *at = begin; at != nullptr && at < end; at = strchr(at, '\n')) {
    if (*at == '\n')
      at++;
    if (static_cast<size_t>(end - at) >= len && strncmp(at, prefix, len) == 0) {
      at += len;
      tl_read::copy(out, size, at, std::min<size_t>(strcspn(at, "\r\n"), static_cast<size_t>(end - at)));
      return true;
    }
  }
  out[0] = '\0';
  return false;
}

int section_pt(const char *begin, const char *end, const char *codec) {
  const size_t len = strlen(codec);
  for (const char *at = strstr(begin, "a=rtpmap:"); at != nullptr && at < end; at = strstr(at + 9, "a=rtpmap:")) {
    char *after = nullptr;
    const long pt = strtol(at + 9, &after, 10);
    if (after != nullptr && strncasecmp(after, codec, len) == 0)
      return static_cast<int>(pt);
  }
  return -1;
}

bool access_unit_has_key(const uint8_t *data, size_t size) {
  for (size_t i = 0; i + 3 < size; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      const uint8_t type = data[i + 3] & 0x1F;
      if (type == 5 || type == 7)
        return true;
      i += 2;
    }
  }
  return false;
}

}

void TelegramCall::attach(mtproto::Mtproto *mtproto, Component *owner) {
  this->mtproto_ = mtproto;
  this->owner_ = owner;
  mtproto->add_update_listener([this](TlReader updates) { this->on_updates_(updates); });
}

void TelegramCall::set_mute(bool mute) {
  this->mute_.store(mute);
  if (this->phone_ != nullptr)
    this->phone_->set_transport_mute(mute);
}

std::string TelegramCall::peer_name() const {
  LockGuard guard(this->lock_);
  return this->name_;
}

std::string TelegramCall::error() const {
  LockGuard guard(this->lock_);
  return this->error_;
}

uint32_t TelegramCall::seconds() const {
  if (this->state_.load() != CallState::ACTIVE || this->active_since_ == 0)
    return 0;
  return (millis() - this->active_since_) / 1000;
}

void TelegramCall::set_state_(CallState state) {
  this->state_.store(state);
  if (state == CallState::ACTIVE)
    this->active_since_ = millis();
  this->changed_.store(true);
  if (this->owner_ != nullptr)
    this->owner_->enable_loop_soon_any_context();
}

void TelegramCall::fail_(const char *text) {
  {
    LockGuard guard(this->lock_);
    tl_read::copy(this->error_, sizeof(this->error_), text, strlen(text));
  }
  ESP_LOGW(TAG, "%s", text);
}

bool TelegramCall::begin_(bool outgoing, int64_t call_id, int64_t call_hash, const uint8_t *g_a_hash) {
  if (this->session_ != nullptr || this->task_.is_created())
    return false;
  auto *s = static_cast<CallSession *>(heap_caps_calloc(1, sizeof(CallSession), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s == nullptr) {
    this->fail_("No PSRAM for the call");
    return false;
  }
  s->events = xQueueCreateWithCaps(QUEUE_DEPTH, sizeof(Event *), MALLOC_CAP_SPIRAM);
  if (s->events == nullptr) {
    heap_caps_free(s);
    this->fail_("No PSRAM for the call");
    return false;
  }
  s->owner = this;
  s->outgoing = outgoing;
  s->call_id = call_id;
  s->call_hash = call_hash;
  s->local_pt = -1;
  s->local_vpt = -1;
  s->relay_fd = -1;
  if (g_a_hash != nullptr)
    memcpy(s->g_a_hash, g_a_hash, 32);
  this->mute_.store(false);
  this->active_since_ = 0;
  {
    LockGuard guard(this->lock_);
    this->error_[0] = '\0';
    this->session_ = s;
  }
  this->call_id_.store(call_id);
  return true;
}

void TelegramCall::finish_() {
  CallSession *s = this->session_;
  if (s == nullptr)
    return;
  if (this->task_.is_created() && !this->task_.deallocate()) {
    App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 50, [this]() { this->finish_(); });
    return;
  }
  this->call_id_.store(0);
  {
    LockGuard guard(this->lock_);
    this->session_ = nullptr;
    Event *event = nullptr;
    while (xQueueReceive(s->events, &event, 0) == pdTRUE)
      heap_caps_free(event);
    vQueueDeleteWithCaps(s->events);
    heap_caps_free(s);
  }
  this->set_state_(CallState::IDLE);
}

bool TelegramCall::start(int64_t user_id, int64_t access_hash, const std::string &name, bool video) {
  if (this->state_.load() != CallState::IDLE || this->phone_ == nullptr || this->mtproto_ == nullptr ||
      !this->mtproto_->logged_in() || (video && this->video_host_ == nullptr))
    return false;
  if (!this->begin_(true, 0, 0, nullptr))
    return false;
  this->session_->video = video;
  {
    LockGuard guard(this->lock_);
    tl_read::copy(this->name_, sizeof(this->name_), name.c_str(), name.size());
  }
  this->session_->user_id = user_id;
  this->session_->user_hash = access_hash;
  this->set_state_(CallState::OUTGOING);
  if (!this->task_.create(&TelegramCall::task_entry_, "tg_call", TASK_STACK, this, TASK_PRIORITY, false,
                          LOOP_TASK_CORE)) {
    this->fail_("No memory for the call task");
    this->finish_();
    return false;
  }
  return true;
}

void TelegramCall::accept() {
  if (this->state_.load() != CallState::INCOMING || this->session_ == nullptr || this->task_.is_created())
    return;
  this->set_state_(CallState::CONNECTING);
  if (!this->task_.create(&TelegramCall::task_entry_, "tg_call", TASK_STACK, this, TASK_PRIORITY, false,
                          LOOP_TASK_CORE)) {
    this->fail_("No memory for the call task");
    this->post_(EVENT_DECLINE, nullptr, 0);
    this->hangup();
  }
}

void TelegramCall::hangup() {
  CallSession *s = this->session_;
  if (s == nullptr)
    return;
  if (this->task_.is_created()) {
    this->post_(EVENT_HANGUP, nullptr, 0);
    return;
  }
  uint8_t body[64];
  TlWriter w(body, sizeof(body));
  w.u32(ID_DISCARD_CALL);
  w.u32(0);
  w.u32(ID_INPUT_PHONE_CALL);
  w.i64(s->call_id);
  w.i64(s->call_hash);
  w.i32(0);
  w.u32(ID_REASON_BUSY);
  w.i64(0);
  this->mtproto_->invoke(w.data(), w.size(), nullptr);
  this->set_state_(CallState::ENDED);
  this->finish_();
}

void TelegramCall::post_(uint8_t type, const uint8_t *data, size_t length) {
  LockGuard guard(this->lock_);
  CallSession *s = this->session_;
  if (s == nullptr)
    return;
  auto *event = static_cast<Event *>(heap_caps_malloc(sizeof(Event) + length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (event == nullptr)
    return;
  event->type = type;
  event->length = length;
  if (length != 0)
    memcpy(event->data, data, length);
  if (xQueueSend(s->events, &event, 0) != pdTRUE)
    heap_caps_free(event);
}

void TelegramCall::on_updates_(TlReader updates) {
  uint32_t ctor = 0;
  if (!updates.peek_u32(ctor))
    return;
  if (ctor == ID_UPDATE_SHORT) {
    TlReader update;
    if (tl_field(updates, tl_hash("update"), update))
      this->on_update_(update);
    return;
  }
  if (ctor != ID_UPDATES && ctor != ID_UPDATES_COMBINED)
    return;
  TlReader list;
  uint32_t count = 0;
  if (!tl_read::field_vector(updates, "updates", list, count))
    return;
  TlReader users;
  uint32_t user_count = 0;
  const bool have_users = tl_read::field_vector(updates, "users", users, user_count);
  for (uint32_t i = 0; i < count; i++) {
    TlReader item = list;
    uint32_t update_ctor = 0;
    if (!list.peek_u32(update_ctor))
      return;
    if (update_ctor == ID_UPDATE_PHONE_CALL) {
      TlReader call;
      uint32_t call_ctor = 0;
      if (tl_field(item, tl_hash("phone_call"), call) && call.peek_u32(call_ctor) && call_ctor == ID_CALL_REQUESTED &&
          have_users && this->state_.load() == CallState::IDLE) {
        int64_t admin = 0;
        char name[64];
        if (tl_read::i64(call, "admin_id", admin) && tl_read::user_name(users, user_count, admin, name, sizeof(name))) {
          LockGuard guard(this->lock_);
          tl_read::copy(this->name_, sizeof(this->name_), name, strlen(name));
        }
      }
    }
    if (update_ctor == ID_UPDATE_PHONE_CALL || update_ctor == ID_UPDATE_PHONE_CALL_SIGNALING)
      this->on_update_(item);
    if (!tl_skip_object(list))
      return;
  }
}

void TelegramCall::on_update_(TlReader update) {
  uint32_t ctor = 0;
  if (!update.peek_u32(ctor))
    return;
  if (ctor == ID_UPDATE_PHONE_CALL_SIGNALING) {
    int64_t id = 0;
    const uint8_t *data = nullptr;
    size_t length = 0;
    if (tl_read::i64(update, "phone_call_id", id) && id != 0 && id == this->call_id_.load() &&
        tl_read::bytes(update, "data", data, length))
      this->post_(EVENT_SIGNALING, data, length);
    return;
  }
  if (ctor != ID_UPDATE_PHONE_CALL)
    return;
  TlReader call;
  if (tl_field(update, tl_hash("phone_call"), call))
    this->on_phone_call_(call);
}

void TelegramCall::on_phone_call_(TlReader call) {
  uint32_t ctor = 0;
  int64_t id = 0;
  if (!call.peek_u32(ctor) || !tl_read::i64(call, "id", id))
    return;
  if (ctor == ID_CALL_REQUESTED) {
    int64_t hash = 0;
    const uint8_t *g_a_hash = nullptr;
    size_t hash_len = 0;
    tl_read::i64(call, "access_hash", hash);
    if (!tl_read::bytes(call, "g_a_hash", g_a_hash, hash_len) || hash_len != 32)
      return;
    uint8_t body[64];
    TlWriter w(body, sizeof(body));
    if (this->state_.load() != CallState::IDLE || this->phone_ == nullptr) {
      w.u32(ID_DISCARD_CALL);
      w.u32(0);
      w.u32(ID_INPUT_PHONE_CALL);
      w.i64(id);
      w.i64(hash);
      w.i32(0);
      w.u32(ID_REASON_BUSY);
      w.i64(0);
      this->mtproto_->invoke(w.data(), w.size(), nullptr);
      return;
    }
    if (!this->begin_(false, id, hash, g_a_hash))
      return;
    uint32_t flags = 0;
    tl_read::u32(call, "flags", flags);
    this->session_->video = (flags & (1u << 6)) != 0 && this->video_host_ != nullptr;
    {
      LockGuard guard(this->lock_);
      if (this->name_[0] == '\0')
        tl_read::copy(this->name_, sizeof(this->name_), "Telegram", 8);
    }
    w.u32(ID_RECEIVED_CALL);
    w.u32(ID_INPUT_PHONE_CALL);
    w.i64(id);
    w.i64(hash);
    this->mtproto_->invoke(w.data(), w.size(), nullptr);
    this->set_state_(CallState::INCOMING);
    return;
  }
  if (id == 0 || id != this->call_id_.load())
    return;
  if (ctor == ID_CALL_DISCARDED) {
    if (this->task_.is_created()) {
      this->post_(EVENT_REMOTE_END, nullptr, 0);
    } else {
      App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() {
        if (this->task_.is_created()) {
          this->post_(EVENT_REMOTE_END, nullptr, 0);
          return;
        }
        this->set_state_(CallState::ENDED);
        this->finish_();
      });
    }
    return;
  }
  const uint8_t *data = nullptr;
  size_t length = 0;
  if (tl_read::object_span(call, data, length))
    this->post_(EVENT_UPDATE, data, length);
}

void TelegramCall::task_entry_(void *param) { static_cast<TelegramCall *>(param)->run_(); }

bool TelegramCall::rpc_(const uint8_t *body, size_t length, std::function<bool(TlReader)> &&handler) {
  uint32_t serial = 0;
  CallSession *s = this->session_;
  {
    LockGuard guard(this->wait_lock_);
    serial = ++this->wait_serial_;
    s->waiter = xTaskGetCurrentTaskHandle();
    this->wait_ok_ = false;
  }
  ulTaskNotifyTake(pdTRUE, 0);
  const bool queued = this->mtproto_->invoke(
      body, length, [this, serial, handler = std::move(handler)](const char *error, TlReader result) {
        LockGuard guard(this->wait_lock_);
        if (serial != this->wait_serial_ || this->session_ == nullptr)
          return;
        if (error != nullptr) {
          this->fail_(error);
        } else {
          this->wait_ok_ = handler(result);
        }
        xTaskNotifyGive(this->session_->waiter);
      });
  if (!queued)
    return false;
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RPC_WAIT_MS)) == 0) {
    LockGuard guard(this->wait_lock_);
    this->wait_serial_++;
    this->fail_("No answer from Telegram");
    return false;
  }
  LockGuard guard(this->wait_lock_);
  return this->wait_ok_;
}

void TelegramCall::send_(const uint8_t *body, size_t length) { this->mtproto_->invoke(body, length, nullptr); }

namespace {

void write_protocol(TlWriter &w) {
  w.u32(ID_PROTOCOL);
  w.u32(3);
  w.i32(MIN_LAYER);
  w.i32(MAX_LAYER);
  w.vector(1);
  w.string(LIBRARY_VERSION);
}

void write_peer(TlWriter &w, const CallSession *s) {
  w.u32(ID_INPUT_PHONE_CALL);
  w.i64(s->call_id);
  w.i64(s->call_hash);
}

bool dh_prepare(CallSession *s,
                const std::function<bool(const uint8_t *, size_t, std::function<bool(TlReader)> &&)> &rpc) {
  uint8_t body[12];
  TlWriter w(body, sizeof(body));
  w.u32(ID_GET_DH_CONFIG);
  w.i32(0);
  w.i32(mtproto::DH_BYTES);
  uint8_t server_random[mtproto::DH_BYTES] = {};
  const bool ok = rpc(w.data(), w.size(), [s, &server_random](TlReader result) {
    uint32_t ctor = 0;
    int32_t g = 0;
    const uint8_t *prime = nullptr;
    size_t prime_len = 0;
    const uint8_t *random = nullptr;
    size_t random_len = 0;
    if (!result.peek_u32(ctor) || ctor != ID_DH_CONFIG || !tl_read::i32(result, "g", g) ||
        !tl_read::bytes(result, "p", prime, prime_len) || prime_len != mtproto::DH_BYTES ||
        !tl_read::bytes(result, "random", random, random_len))
      return false;
    s->g = static_cast<uint32_t>(g);
    memcpy(s->prime, prime, prime_len);
    memcpy(server_random, random, std::min(random_len, sizeof(server_random)));
    return true;
  });
  if (!ok || !mtproto::dh_check(s->g, s->prime, sizeof(s->prime)))
    return false;
  mtproto::random_bytes(s->secret, sizeof(s->secret));
  for (size_t i = 0; i < sizeof(s->secret); i++)
    s->secret[i] ^= server_random[i];
  uint8_t g_bytes[4];
  put_be32(g_bytes, s->g);
  return mtproto::mod_exp(g_bytes, sizeof(g_bytes), s->secret, sizeof(s->secret), s->prime, sizeof(s->prime),
                          s->own_public) &&
         mtproto::dh_check_value(s->own_public, sizeof(s->own_public), s->prime, sizeof(s->prime));
}

bool make_key(CallSession *s, const uint8_t *remote, size_t remote_len) {
  uint8_t padded[mtproto::DH_BYTES] = {};
  if (remote_len == 0 || remote_len > sizeof(padded))
    return false;
  memcpy(padded + sizeof(padded) - remote_len, remote, remote_len);
  if (!mtproto::dh_check_value(padded, sizeof(padded), s->prime, sizeof(s->prime)) ||
      !mtproto::mod_exp(padded, sizeof(padded), s->secret, sizeof(s->secret), s->prime, sizeof(s->prime), s->key))
    return false;
  uint8_t hash[20];
  mtproto::sha1(s->key, sizeof(s->key), hash);
  uint64_t fp = 0;
  for (int i = 7; i >= 0; i--)
    fp = (fp << 8) | hash[12 + i];
  s->fingerprint = static_cast<int64_t>(fp);
  s->have_key = true;
  return true;
}

}

void TelegramCall::run_() {
  CallSession *s = this->session_;
  auto rpc = [this](const uint8_t *body, size_t length, std::function<bool(TlReader)> &&handler) {
    return this->rpc_(body, length, std::move(handler));
  };
  bool ok = dh_prepare(s, rpc);
  if (!ok)
    this->fail_("Call key exchange failed");
  uint8_t *body = s->packet;
  if (ok && s->outgoing) {
    uint8_t g_a_hash[32];
    mtproto::sha256_parts(s->own_public, sizeof(s->own_public), nullptr, 0, g_a_hash);
    TlWriter w(body, MAX_PACKET);
    w.u32(ID_REQUEST_CALL);
    w.u32(s->video ? 1 : 0);
    w.u32(ID_INPUT_USER);
    w.i64(s->user_id);
    w.i64(s->user_hash);
    w.i32(static_cast<int32_t>(esp_random() & 0x7FFFFFFF));
    w.bytes(g_a_hash, sizeof(g_a_hash));
    write_protocol(w);
    ok = w.ok() && this->rpc_(w.data(), w.size(), [this, s](TlReader result) {
      uint32_t ctor = 0;
      TlReader call;
      if (!result.peek_u32(ctor) || ctor != ID_PHONE_PHONE_CALL || !tl_field(result, tl_hash("phone_call"), call) ||
          !tl_read::i64(call, "id", s->call_id) || !tl_read::i64(call, "access_hash", s->call_hash))
        return false;
      this->call_id_.store(s->call_id);
      return true;
    });
    if (ok)
      this->set_state_(CallState::RINGING);
  } else if (ok) {
    TlWriter w(body, MAX_PACKET);
    w.u32(ID_ACCEPT_CALL);
    write_peer(w, s);
    w.bytes(s->own_public, sizeof(s->own_public));
    write_protocol(w);
    ok = w.ok() && this->rpc_(w.data(), w.size(), [](TlReader result) {
      uint32_t ctor = 0;
      return result.peek_u32(ctor) && ctor == ID_PHONE_PHONE_CALL;
    });
  }
  bool hangup = ok;
  uint32_t reason = ok ? ID_REASON_HANGUP : ID_REASON_DISCONNECT;
  while (ok) {
    Event *event = nullptr;
    const TickType_t wait = s->peer != nullptr ? pdMS_TO_TICKS(PUMP_MS) : portMAX_DELAY;
    if (xQueueReceive(s->events, &event, wait) == pdTRUE && event != nullptr) {
      const uint8_t type = event->type;
      if (type == EVENT_HANGUP || type == EVENT_DECLINE) {
        ok = false;
        if (type == EVENT_DECLINE)
          reason = ID_REASON_BUSY;
      } else if (type == EVENT_REMOTE_END) {
        ok = false;
        hangup = false;
      } else if (type == EVENT_SIGNALING) {
        std::string json;
        if (this->decrypt_signaling_(s, event->data, event->length, &json))
          this->handle_signaling_(s, json);
      } else if (type == EVENT_UPDATE) {
        ok = this->handle_call_object_(s, event->data, event->length, &hangup);
        if (!ok && hangup)
          reason = ID_REASON_DISCONNECT;
      }
      heap_caps_free(event);
    }
    if (s->peer != nullptr) {
      esp_peer_main_loop(s->peer);
      this->send_frames_(s);
      if (s->sdp_dirty && s->connected && this->write_remote_sdp_(s)) {
        s->sdp_dirty = false;
        esp_peer_msg_t msg{};
        msg.type = ESP_PEER_MSG_TYPE_SDP;
        msg.data = reinterpret_cast<uint8_t *>(s->sdp);
        msg.size = static_cast<int>(strlen(s->sdp));
        esp_peer_send_msg(s->peer, &msg);
      }
      this->pump_video_relay_(s);
    }
    if (s->relay_hangup) {
      ok = false;
    }
    if (s->ended) {
      ok = false;
      reason = ID_REASON_DISCONNECT;
    }
  }
  this->stop_media_(s);
  if (hangup && s->call_id != 0) {
    TlWriter w(body, MAX_PACKET);
    w.u32(ID_DISCARD_CALL);
    w.u32(0);
    write_peer(w, s);
    w.i32(static_cast<int32_t>(this->seconds()));
    w.u32(reason);
    w.i64(0);
    this->send_(w.data(), w.size());
  }
  this->set_state_(CallState::ENDED);
  App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() { this->finish_(); });
  vTaskSuspend(nullptr);
}

bool TelegramCall::handle_call_object_(CallSession *s, const uint8_t *data, size_t length, bool *hangup) {
  TlReader call(data, length);
  uint32_t ctor = 0;
  if (!call.peek_u32(ctor))
    return true;
  if (ctor == ID_CALL_DISCARDED) {
    *hangup = false;
    return false;
  }
  if (ctor == ID_CALL_ACCEPTED && s->outgoing && !s->have_key) {
    const uint8_t *g_b = nullptr;
    size_t g_b_len = 0;
    if (!tl_read::bytes(call, "g_b", g_b, g_b_len) || !make_key(s, g_b, g_b_len)) {
      this->fail_("Call key check failed");
      return false;
    }
    TlWriter w(s->packet, MAX_PACKET);
    w.u32(ID_CONFIRM_CALL);
    write_peer(w, s);
    w.bytes(s->own_public, sizeof(s->own_public));
    w.i64(s->fingerprint);
    write_protocol(w);
    uint8_t *copy = nullptr;
    size_t copy_len = 0;
    const bool ok = w.ok() && this->rpc_(w.data(), w.size(), [&copy, &copy_len](TlReader result) {
      uint32_t result_ctor = 0;
      TlReader phone_call;
      const uint8_t *span = nullptr;
      size_t span_len = 0;
      if (!result.peek_u32(result_ctor) || result_ctor != ID_PHONE_PHONE_CALL ||
          !tl_field(result, tl_hash("phone_call"), phone_call) || !tl_read::object_span(phone_call, span, span_len))
        return false;
      copy = static_cast<uint8_t *>(heap_caps_malloc(span_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (copy == nullptr)
        return false;
      memcpy(copy, span, span_len);
      copy_len = span_len;
      return true;
    });
    bool started = true;
    if (ok) {
      TlReader confirmed(copy, copy_len);
      uint32_t confirmed_ctor = 0;
      if (confirmed.peek_u32(confirmed_ctor) && confirmed_ctor == ID_CALL && !s->media)
        started = this->start_media_(s, confirmed);
    }
    heap_caps_free(copy);
    return ok && started;
  }
  if (ctor == ID_CALL && !s->media) {
    if (!s->outgoing) {
      const uint8_t *g_a = nullptr;
      size_t g_a_len = 0;
      int64_t fingerprint = 0;
      uint8_t hash[32];
      if (!tl_read::bytes(call, "g_a_or_b", g_a, g_a_len) || !tl_read::i64(call, "key_fingerprint", fingerprint))
        return false;
      mtproto::sha256_parts(g_a, g_a_len, nullptr, 0, hash);
      if (memcmp(hash, s->g_a_hash, sizeof(hash)) != 0 || !make_key(s, g_a, g_a_len) ||
          fingerprint != s->fingerprint) {
        this->fail_("Call key check failed");
        return false;
      }
    } else if (!s->have_key) {
      return true;
    }
    return this->start_media_(s, call);
  }
  return true;
}

bool TelegramCall::start_media_(CallSession *s, TlReader call) {
  TlReader connections;
  uint32_t count = 0;
  s->server_count = 0;
  if (tl_read::field_vector(call, "connections", connections, count)) {
    for (uint32_t i = 0; i < count && s->server_count < MAX_SERVERS; i++) {
      TlReader item = connections;
      uint32_t ctor = 0;
      if (!connections.peek_u32(ctor))
        break;
      if (ctor == ID_CONNECTION_WEBRTC) {
        uint32_t flags = 0;
        int32_t port = 0;
        char ip[48];
        tl_read::u32(item, "flags", flags);
        tl_read::i32(item, "port", port);
        if (tl_read::text(item, "ip", ip, sizeof(ip)) && ip[0] != '\0' && port > 0 && (flags & 3) != 0) {
          const uint8_t k = s->server_count;
          const bool turn = (flags & 1) != 0;
          snprintf(s->server_url[k], sizeof(s->server_url[k]), "%s:%s:%d", turn ? "turn" : "stun", ip,
                   static_cast<int>(port));
          tl_read::text(item, "username", s->server_user[k], sizeof(s->server_user[k]));
          tl_read::text(item, "password", s->server_pass[k], sizeof(s->server_pass[k]));
          s->servers[k].stun_url = s->server_url[k];
          s->servers[k].user = turn ? s->server_user[k] : nullptr;
          s->servers[k].psw = turn ? s->server_pass[k] : nullptr;
          s->server_count++;
        }
      }
      if (!tl_skip_object(connections))
        break;
    }
  }
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= TX_RING_BYTES)
    s->tx = ring_buffer::RingBuffer::create(TX_RING_BYTES).release();
  if (s->tx == nullptr) {
    this->fail_("No memory for the call's audio");
    return false;
  }
  s->tx_pending = 0;
  if (s->video) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= VIDEO_RING_BYTES)
      s->video_ring = ring_buffer::RingBuffer::create(VIDEO_RING_BYTES).release();
    if (s->video_ring == nullptr) {
      this->fail_("No memory for the call's video");
      return false;
    }
    s->video_wait_key = true;
    this->open_video_relay_(s);
  }
  esp_peer_default_cfg_t &extra = s->peer_extra;
  extra.agent_recv_timeout = 500;
  esp_peer_cfg_t &cfg = s->peer_cfg;
  cfg.server_lists = s->servers;
  cfg.server_num = s->server_count;
  cfg.role = s->outgoing ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED;
  cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
  cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
  cfg.audio_info.sample_rate = SAMPLE_RATE;
  cfg.audio_info.channel = 1;
  cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
  cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
  if (s->video) {
    cfg.video_info.codec = ESP_PEER_VIDEO_CODEC_H264;
    cfg.video_info.width = VIDEO_WIDTH;
    cfg.video_info.height = VIDEO_HEIGHT;
    cfg.video_info.fps = VIDEO_FPS;
    cfg.video_dir = ESP_PEER_MEDIA_DIR_RECV_ONLY;
  }
  cfg.no_auto_reconnect = true;
  cfg.enable_data_channel = false;
  cfg.extra_cfg = &extra;
  cfg.extra_size = sizeof(extra);
  cfg.ctx = s;
  cfg.on_state = [](esp_peer_state_t state, void *ctx) -> int {
    auto *session = static_cast<CallSession *>(ctx);
    TelegramCall *self = session->owner;
    if (state == ESP_PEER_STATE_CONNECTED) {
      session->connected = true;
      self->set_state_(CallState::ACTIVE);
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED ||
               (state == ESP_PEER_STATE_DISCONNECTED && self->state_.load() == CallState::ACTIVE)) {
      self->fail_("Call connection lost");
      session->ended = true;
    }
    return 0;
  };
  cfg.on_msg = [](esp_peer_msg_t *msg, void *ctx) -> int {
    auto *session = static_cast<CallSession *>(ctx);
    if (msg == nullptr || msg->data == nullptr || msg->size <= 0)
      return 0;
    char line[256];
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
      session->owner->on_local_sdp_(session, reinterpret_cast<const char *>(msg->data));
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
      tl_read::copy(line, sizeof(line), reinterpret_cast<const char *>(msg->data), static_cast<size_t>(msg->size));
      session->owner->on_local_candidate_(session, line);
    }
    return 0;
  };
  cfg.on_audio_data = [](esp_peer_audio_frame_t *frame, void *ctx) -> int {
    auto *session = static_cast<CallSession *>(ctx);
    TelegramCall *self = session->owner;
    if (self->media_on_.load(std::memory_order_acquire) && frame != nullptr && frame->data != nullptr &&
        frame->size > 0)
      self->phone_->transport_payload(frame->data, static_cast<size_t>(frame->size));
    return 0;
  };
  cfg.on_video_data = [](esp_peer_video_frame_t *frame, void *ctx) -> int {
    auto *session = static_cast<CallSession *>(ctx);
    if (frame != nullptr && frame->data != nullptr && frame->size > 0)
      session->owner->on_video_frame_(session, frame->data, static_cast<size_t>(frame->size));
    return 0;
  };
  esp_peer_pre_generate_cert();
  if (esp_peer_open(&cfg, esp_peer_get_default_impl(), &s->peer) != ESP_PEER_ERR_NONE || s->peer == nullptr) {
    s->peer = nullptr;
    this->fail_("No call connection possible");
    return false;
  }
  esp_peer_new_connection(s->peer);
  s->media = true;
  App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() {
    if (this->session_ == nullptr || !this->session_->media)
      return;
    if (!this->phone_->start_transport_media(this, this->mute_.load())) {
      this->fail_("Call audio busy");
      this->hangup();
      return;
    }
    this->media_on_.store(true, std::memory_order_release);
  });
  this->set_state_(CallState::CONNECTING);
  return true;
}

void TelegramCall::stop_media_(CallSession *s) {
  this->media_on_.store(false, std::memory_order_release);
  while (peer_users.load() != 0)
    vTaskDelay(1);
  if (s->peer != nullptr) {
    esp_peer_close(s->peer);
    s->peer = nullptr;
  }
  delete s->tx;
  s->tx = nullptr;
  this->close_video_relay_(s);
  delete s->video_ring;
  s->video_ring = nullptr;
  if (s->media) {
    s->media = false;
    App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() {
      this->media_on_.store(false, std::memory_order_release);
      this->phone_->stop_transport_media();
    });
  }
}

bool TelegramCall::decrypt_signaling_(CallSession *s, const uint8_t *data, size_t length, std::string *json) {
  if (!s->have_key || length < 21 || length > 128 * 1024)
    return false;
  const size_t x = (s->outgoing ? 8 : 0) + 128;
  const size_t n = length - 16;
  auto *plain = static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (plain == nullptr)
    return false;
  memcpy(plain, data + 16, n);
  uint8_t aes_key[32];
  uint8_t aes_iv[16];
  signaling_key_iv(s->key, data, x, aes_key, aes_iv);
  uint8_t large[32];
  bool ok = mtproto::aes_ctr(aes_key, aes_iv, plain, n);
  if (ok) {
    mtproto::sha256_parts(s->key + 88 + x, 32, plain, n, large);
    ok = memcmp(large + 8, data, 16) == 0;
  }
  if (ok) {
    const uint32_t seq = get_be32(plain) & 0x3FFFFFFFu;
    uint32_t highest = 0;
    for (uint8_t i = 0; i < s->seen_count; i++) {
      ok = ok && s->seen[i] != seq;
      highest = std::max(highest, s->seen[i]);
    }
    ok = ok && seq + 64 > highest;
    if (ok) {
      if (s->seen_count < 64) {
        s->seen[s->seen_count++] = seq;
      } else {
        uint8_t lowest = 0;
        for (uint8_t i = 1; i < 64; i++) {
          if (s->seen[i] < s->seen[lowest])
            lowest = i;
        }
        s->seen[lowest] = seq;
      }
      json->assign(reinterpret_cast<const char *>(plain) + 4, n - 4);
    }
  }
  heap_caps_free(plain);
  return ok;
}

void TelegramCall::send_signaling_(CallSession *s, const std::string &json) {
  if (!s->have_key || json.empty())
    return;
  const size_t n = 4 + json.size();
  const size_t total = 16 + n;
  auto *packet = static_cast<uint8_t *>(heap_caps_malloc(total + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *body = static_cast<uint8_t *>(heap_caps_malloc(total + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (packet == nullptr || body == nullptr) {
    heap_caps_free(packet);
    heap_caps_free(body);
    return;
  }
  uint8_t *plain = packet + 16;
  put_be32(plain, ++s->seq_out);
  memcpy(plain + 4, json.data(), json.size());
  const size_t x = (s->outgoing ? 0 : 8) + 128;
  uint8_t large[32];
  mtproto::sha256_parts(s->key + 88 + x, 32, plain, n, large);
  memcpy(packet, large + 8, 16);
  uint8_t aes_key[32];
  uint8_t aes_iv[16];
  signaling_key_iv(s->key, packet, x, aes_key, aes_iv);
  if (mtproto::aes_ctr(aes_key, aes_iv, plain, n)) {
    TlWriter w(body, total + 64);
    w.u32(ID_SEND_SIGNALING);
    write_peer(w, s);
    w.bytes(packet, total);
    if (w.ok())
      this->send_(w.data(), w.size());
  }
  heap_caps_free(packet);
  heap_caps_free(body);
}

void TelegramCall::handle_signaling_(CallSession *s, const std::string &json) {
  json::parse_json(json, [this, s](JsonObject root) -> bool {
    const char *type = root["@type"] | "";
    if (strcmp(type, "InitialSetup") == 0) {
      const char *ufrag = root["ufrag"] | "";
      const char *pwd = root["pwd"] | "";
      tl_read::copy(s->remote_ufrag, sizeof(s->remote_ufrag), ufrag, strlen(ufrag));
      tl_read::copy(s->remote_pwd, sizeof(s->remote_pwd), pwd, strlen(pwd));
      JsonArray fingerprints = root["fingerprints"].as<JsonArray>();
      if (!fingerprints.isNull() && fingerprints.size() > 0) {
        JsonObject fp = fingerprints[0].as<JsonObject>();
        const char *hash = fp["hash"] | "sha-256";
        const char *value = fp["fingerprint"] | "";
        tl_read::copy(s->remote_hash, sizeof(s->remote_hash), hash, strlen(hash));
        tl_read::copy(s->remote_fingerprint, sizeof(s->remote_fingerprint), value, strlen(value));
      }
      this->set_remote_sdp_(s);
    } else if (strcmp(type, "NegotiateChannels") == 0) {
      char id[24];
      JsonVariant exchange = root["exchangeId"];
      if (exchange.is<const char *>()) {
        tl_read::copy(id, sizeof(id), exchange.as<const char *>(), strlen(exchange.as<const char *>()));
      } else {
        snprintf(id, sizeof(id), "%" PRIu32, exchange.as<uint32_t>());
      }
      if (strcmp(id, s->offer_id) == 0)
        return true;
      JsonArray contents = root["contents"].as<JsonArray>();
      s->pending_offer_video = false;
      for (JsonObject content : contents) {
        const char *media = content["type"] | "";
        if (strcmp(media, "audio") == 0) {
          s->pending_offer_ssrc = json_u32(content["ssrc"]);
          s->remote_ssrc = s->pending_offer_ssrc;
        } else if (strcmp(media, "video") == 0 && s->video) {
          JsonObject h264;
          for (JsonObject type : content["payloadTypes"].as<JsonArray>()) {
            const char *name = type["name"] | "";
            const char *mode = type["parameters"]["packetization-mode"] | "0";
            const char *profile = type["parameters"]["profile-level-id"] | "";
            if (strcasecmp(name, "H264") != 0 || strcmp(mode, "1") != 0)
              continue;
            if (h264.isNull() || strncasecmp(profile, "42", 2) == 0)
              h264 = type;
          }
          const uint32_t ssrc = json_u32(content["ssrc"]);
          if (h264.isNull() || ssrc == 0)
            continue;
          s->video_param_count = 0;
          for (JsonPair param : h264["parameters"].as<JsonObject>()) {
            if (s->video_param_count >= MAX_VIDEO_PARAMS)
              break;
            const char *key = param.key().c_str();
            const char *value = param.value() | "";
            tl_read::copy(s->video_param_name[s->video_param_count], sizeof(s->video_param_name[0]), key,
                          strlen(key));
            tl_read::copy(s->video_param_value[s->video_param_count], sizeof(s->video_param_value[0]), value,
                          strlen(value));
            s->video_param_count++;
          }
          s->video_group_count = 0;
          for (JsonObject group : content["ssrcGroups"].as<JsonArray>()) {
            if (s->video_group_count >= MAX_VIDEO_GROUPS)
              break;
            const uint8_t g = s->video_group_count;
            const char *semantics = group["semantics"] | "";
            tl_read::copy(s->video_group_semantics[g], sizeof(s->video_group_semantics[0]), semantics,
                          strlen(semantics));
            s->video_group_size[g] = 0;
            for (JsonVariant member : group["ssrcs"].as<JsonArray>()) {
              if (s->video_group_size[g] >= MAX_GROUP_SSRCS)
                break;
              s->video_groups[g][s->video_group_size[g]++] = json_u32(member);
            }
            s->video_group_count++;
          }
          if (ssrc != s->remote_vssrc) {
            s->remote_vssrc = ssrc;
            if (s->remote_set)
              s->sdp_dirty = true;
          }
          s->pending_offer_video = true;
        }
      }
      tl_read::copy(s->pending_offer_id, sizeof(s->pending_offer_id), id, strlen(id));
      s->pending_offer = true;
      this->answer_offer_(s);
    } else if (strcmp(type, "Candidates") == 0) {
      JsonArray candidates = root["candidates"].as<JsonArray>();
      for (JsonObject candidate : candidates) {
        const char *line = candidate["sdpString"] | "";
        if (line[0] == '\0')
          continue;
        if (s->remote_set && s->peer != nullptr) {
          esp_peer_msg_t msg{};
          msg.type = ESP_PEER_MSG_TYPE_CANDIDATE;
          msg.data = reinterpret_cast<uint8_t *>(const_cast<char *>(line));
          msg.size = static_cast<int>(strlen(line));
          esp_peer_send_msg(s->peer, &msg);
        } else if (s->candidate_count < MAX_CANDIDATES) {
          tl_read::copy(s->candidates[s->candidate_count], sizeof(s->candidates[0]), line, strlen(line));
          s->candidate_count++;
        }
      }
    }
    return true;
  });
}

bool TelegramCall::write_remote_sdp_(CallSession *s) {
  const int pt = s->local_pt >= 0 ? s->local_pt : DEFAULT_PT;
  const char *mid = s->local_mid[0] != '\0' ? s->local_mid : "0";
  const bool video = s->video && s->local_vpt >= 0 && s->local_vmid[0] != '\0';
  const char *setup = s->outgoing ? "passive" : "active";
  size_t n = 0;
  auto add = [s, &n](const char *format, auto... args) {
    if (n >= SDP_SIZE)
      return;
    const int written = snprintf(s->sdp + n, SDP_SIZE - n, format, args...);
    n = written < 0 ? SDP_SIZE : n + static_cast<size_t>(written);
  };
  add("v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE %s%s%s\r\n", mid, video ? " " : "",
      video ? s->local_vmid : "");
  add("m=audio 9 UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 0.0.0.0\r\na=rtcp:9 IN IP4 0.0.0.0\r\n"
      "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:%s %s\r\na=setup:%s\r\na=mid:%s\r\n"
      "a=sendrecv\r\na=rtcp-mux\r\na=rtpmap:%d opus/48000/2\r\na=fmtp:%d minptime=10;useinbandfec=1\r\n",
      pt, s->remote_ufrag, s->remote_pwd, s->remote_hash, s->remote_fingerprint, setup, mid, pt, pt);
  if (s->remote_ssrc != 0)
    add("a=ssrc:%" PRIu32 " cname:telegram\r\n", s->remote_ssrc);
  if (video) {
    const int vpt = s->local_vpt;
    add("m=video 9 UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 0.0.0.0\r\na=rtcp:9 IN IP4 0.0.0.0\r\n"
        "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:%s %s\r\na=setup:%s\r\na=mid:%s\r\n%s"
        "a=rtcp-mux\r\na=rtpmap:%d H264/90000\r\na=rtcp-fb:%d nack\r\na=rtcp-fb:%d nack pli\r\n"
        "a=rtcp-fb:%d ccm fir\r\n",
        vpt, s->remote_ufrag, s->remote_pwd, s->remote_hash, s->remote_fingerprint, setup, s->local_vmid,
        s->remote_vssrc != 0 ? "a=sendonly\r\n" : "a=inactive\r\n", vpt, vpt, vpt, vpt);
    if (s->video_param_count > 0) {
      add("a=fmtp:%d ", vpt);
      for (uint8_t i = 0; i < s->video_param_count; i++)
        add("%s%s=%s", i == 0 ? "" : ";", s->video_param_name[i], s->video_param_value[i]);
      add("%s", "\r\n");
    }
    if (s->remote_vssrc != 0)
      add("a=ssrc:%" PRIu32 " cname:telegram\r\n", s->remote_vssrc);
  }
  return n < SDP_SIZE;
}

void TelegramCall::set_remote_sdp_(CallSession *s) {
  if (s->peer == nullptr || s->remote_set || s->remote_ufrag[0] == '\0' || s->remote_fingerprint[0] == '\0')
    return;
  if (s->outgoing && !s->local_ready)
    return;
  if (!this->write_remote_sdp_(s))
    return;
  s->sdp_dirty = false;
  esp_peer_msg_t msg{};
  msg.type = ESP_PEER_MSG_TYPE_SDP;
  msg.data = reinterpret_cast<uint8_t *>(s->sdp);
  msg.size = static_cast<int>(strlen(s->sdp));
  esp_peer_send_msg(s->peer, &msg);
  s->remote_set = true;
  for (uint8_t i = 0; i < s->candidate_count; i++) {
    esp_peer_msg_t candidate{};
    candidate.type = ESP_PEER_MSG_TYPE_CANDIDATE;
    candidate.data = reinterpret_cast<uint8_t *>(s->candidates[i]);
    candidate.size = static_cast<int>(strlen(s->candidates[i]));
    esp_peer_send_msg(s->peer, &candidate);
  }
  s->candidate_count = 0;
}

void TelegramCall::on_local_sdp_(CallSession *s, const char *sdp) {
  char fingerprint[160];
  line_value(sdp, "a=ice-ufrag:", s->local_ufrag, sizeof(s->local_ufrag));
  line_value(sdp, "a=ice-pwd:", s->local_pwd, sizeof(s->local_pwd));
  const char *audio_begin = sdp;
  const char *audio_end = sdp + strlen(sdp);
  section_of(sdp, "m=audio", &audio_begin, &audio_end);
  section_value(audio_begin, audio_end, "a=mid:", s->local_mid, sizeof(s->local_mid));
  const char *video_begin = nullptr;
  const char *video_end = nullptr;
  if (s->video && section_of(sdp, "m=video", &video_begin, &video_end)) {
    section_value(video_begin, video_end, "a=mid:", s->local_vmid, sizeof(s->local_vmid));
    s->local_vpt = section_pt(video_begin, video_end, " H264/");
  }
  if (line_value(sdp, "a=fingerprint:", fingerprint, sizeof(fingerprint))) {
    const char *space = strchr(fingerprint, ' ');
    if (space != nullptr) {
      tl_read::copy(s->local_hash, sizeof(s->local_hash), fingerprint, static_cast<size_t>(space - fingerprint));
      tl_read::copy(s->local_fingerprint, sizeof(s->local_fingerprint), space + 1, strlen(space + 1));
    }
  }
  const int opus_pt = section_pt(audio_begin, audio_end, " opus/");
  if (opus_pt >= 0)
    s->local_pt = opus_pt;
  const char *ssrc = strstr(audio_begin, "a=ssrc:");
  if (ssrc != nullptr && ssrc < audio_end)
    s->local_ssrc = static_cast<uint32_t>(strtoul(ssrc + 7, nullptr, 10));
  if (s->local_ufrag[0] == '\0' || s->local_fingerprint[0] == '\0' || s->local_pt < 0)
    return;
  const bool first = !s->local_ready;
  s->local_ready = true;
  if (first) {
    this->send_initial_setup_(s);
    this->send_offer_(s);
  }
  for (const char *at = strstr(sdp, "a=candidate:"); at != nullptr; at = strstr(at + 12, "a=candidate:")) {
    char line[256];
    tl_read::copy(line, sizeof(line), at + 2, strcspn(at + 2, "\r\n"));
    this->send_candidate_(s, line);
  }
  this->answer_offer_(s);
  this->set_remote_sdp_(s);
}

void TelegramCall::on_local_candidate_(CallSession *s, const char *candidate) {
  const char *line = strncmp(candidate, "a=", 2) == 0 ? candidate + 2 : candidate;
  char clean[256];
  tl_read::copy(clean, sizeof(clean), line, strcspn(line, "\r\n"));
  if (clean[0] != '\0')
    this->send_candidate_(s, clean);
}

void TelegramCall::send_initial_setup_(CallSession *s) {
  const std::string json = json::build_json([s](JsonObject root) {
    root["@type"] = "InitialSetup";
    root["ufrag"] = s->local_ufrag;
    root["pwd"] = s->local_pwd;
    root["renomination"] = false;
    JsonArray fingerprints = root["fingerprints"].to<JsonArray>();
    JsonObject fp = fingerprints.add<JsonObject>();
    fp["hash"] = s->local_hash[0] != '\0' ? s->local_hash : "sha-256";
    fp["setup"] = s->outgoing ? "actpass" : "passive";
    fp["fingerprint"] = s->local_fingerprint;
  });
  this->send_signaling_(s, json);
}

namespace {

void add_video_content(JsonArray contents, const CallSession *s) {
  JsonObject content = contents.add<JsonObject>();
  char text[12];
  snprintf(text, sizeof(text), "%" PRIu32, s->remote_vssrc);
  content["type"] = "video";
  content["ssrc"] = text;
  JsonArray groups = content["ssrcGroups"].to<JsonArray>();
  for (uint8_t g = 0; g < s->video_group_count; g++) {
    JsonObject group = groups.add<JsonObject>();
    group["semantics"] = s->video_group_semantics[g];
    JsonArray ssrcs = group["ssrcs"].to<JsonArray>();
    for (uint8_t i = 0; i < s->video_group_size[g]; i++) {
      snprintf(text, sizeof(text), "%" PRIu32, s->video_groups[g][i]);
      ssrcs.add(text);
    }
  }
  JsonArray types = content["payloadTypes"].to<JsonArray>();
  JsonObject h264 = types.add<JsonObject>();
  h264["id"] = s->local_vpt;
  h264["name"] = "H264";
  h264["clockrate"] = 90000;
  h264["channels"] = 0;
  JsonArray feedback = h264["feedbackTypes"].to<JsonArray>();
  JsonObject nack = feedback.add<JsonObject>();
  nack["type"] = "nack";
  nack["subtype"] = "";
  JsonObject pli = feedback.add<JsonObject>();
  pli["type"] = "nack";
  pli["subtype"] = "pli";
  JsonObject fir = feedback.add<JsonObject>();
  fir["type"] = "ccm";
  fir["subtype"] = "fir";
  JsonObject parameters = h264["parameters"].to<JsonObject>();
  for (uint8_t i = 0; i < s->video_param_count; i++)
    parameters[s->video_param_name[i]] = s->video_param_value[i];
  content["rtpExtensions"].to<JsonArray>();
}

void add_audio_content(JsonArray contents, uint32_t ssrc, int pt) {
  JsonObject content = contents.add<JsonObject>();
  char text[12];
  snprintf(text, sizeof(text), "%" PRIu32, ssrc);
  content["type"] = "audio";
  content["ssrc"] = text;
  JsonArray types = content["payloadTypes"].to<JsonArray>();
  JsonObject opus = types.add<JsonObject>();
  opus["id"] = pt;
  opus["name"] = "opus";
  opus["clockrate"] = SAMPLE_RATE;
  opus["channels"] = 2;
  opus["feedbackTypes"].to<JsonArray>();
  JsonObject parameters = opus["parameters"].to<JsonObject>();
  parameters["minptime"] = "10";
  parameters["useinbandfec"] = "1";
  content["rtpExtensions"].to<JsonArray>();
}

}

void TelegramCall::send_offer_(CallSession *s) {
  if (s->local_ssrc == 0)
    s->local_ssrc = esp_random() & 0x7FFFFFFFu;
  snprintf(s->offer_id, sizeof(s->offer_id), "%" PRIu32, esp_random() & 0x7FFFFFFFu);
  const std::string json = json::build_json([s](JsonObject root) {
    root["@type"] = "NegotiateChannels";
    root["exchangeId"] = s->offer_id;
    add_audio_content(root["contents"].to<JsonArray>(), s->local_ssrc, s->local_pt);
  });
  this->send_signaling_(s, json);
}

void TelegramCall::answer_offer_(CallSession *s) {
  if (!s->pending_offer || !s->local_ready)
    return;
  s->pending_offer = false;
  const std::string json = json::build_json([s](JsonObject root) {
    root["@type"] = "NegotiateChannels";
    root["exchangeId"] = s->pending_offer_id;
    JsonArray contents = root["contents"].to<JsonArray>();
    if (s->pending_offer_ssrc != 0)
      add_audio_content(contents, s->pending_offer_ssrc, s->local_pt);
    if (s->pending_offer_video && s->local_vpt >= 0)
      add_video_content(contents, s);
  });
  this->send_signaling_(s, json);
}

void TelegramCall::send_candidate_(CallSession *s, const char *candidate) {
  const std::string json = json::build_json([candidate](JsonObject root) {
    root["@type"] = "Candidates";
    JsonArray list = root["candidates"].to<JsonArray>();
    JsonObject item = list.add<JsonObject>();
    item["sdpString"] = candidate;
  });
  this->send_signaling_(s, json);
}

void TelegramCall::on_media_payload(const uint8_t *payload, size_t len, uint32_t timestamp) {
  if (len == 0 || len > MAX_FRAME)
    return;
  peer_users.fetch_add(1);
  CallSession *s = this->session_;
  if (this->media_on_.load(std::memory_order_acquire) && s != nullptr && s->tx != nullptr &&
      this->state_.load() == CallState::ACTIVE &&
      s->tx->free() >= sizeof(uint16_t) + sizeof(uint32_t) + len) {
    const bool idle = s->tx->available() == 0;
    const uint16_t length = static_cast<uint16_t>(len);
    s->tx->write_without_replacement(&length, sizeof(length), 0);
    s->tx->write_without_replacement(&timestamp, sizeof(timestamp), 0);
    s->tx->write_without_replacement(payload, len, 0);
    if (idle) {
      Event *wake = nullptr;
      xQueueSend(s->events, &wake, 0);
    }
  }
  peer_users.fetch_sub(1);
}

void TelegramCall::open_video_relay_(CallSession *s) {
  struct sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(this->video_port_);
  if (inet_aton(this->video_host_, &address.sin_addr) == 0) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = nullptr;
    if (getaddrinfo(this->video_host_, nullptr, &hints, &result) != 0 || result == nullptr) {
      ESP_LOGW(TAG, "Video relay %s not found", this->video_host_);
      return;
    }
    address.sin_addr = reinterpret_cast<struct sockaddr_in *>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return;
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (::connect(fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0 && errno != EINPROGRESS) {
    ::close(fd);
    ESP_LOGW(TAG, "Video relay %s:%u not reachable", this->video_host_, this->video_port_);
    return;
  }
  s->relay_fd = fd;
  s->relay_connected = false;
}

void TelegramCall::close_video_relay_(CallSession *s) {
  if (s->video_item != nullptr) {
    s->video_ring->receive_release(s->video_item);
    s->video_item = nullptr;
  }
  if (s->relay_fd >= 0) {
    ::close(s->relay_fd);
    s->relay_fd = -1;
  }
  s->relay_connected = false;
}

void TelegramCall::on_video_frame_(CallSession *s, const uint8_t *data, size_t size) {
  if (s->video_ring == nullptr || size + sizeof(uint32_t) > VIDEO_RING_BYTES)
    return;
  const bool key = access_unit_has_key(data, size);
  if (s->video_wait_key && !key)
    return;
  if (s->video_ring->free() < sizeof(uint32_t) + size) {
    s->video_wait_key = true;
    return;
  }
  s->video_wait_key = false;
  uint8_t length[4];
  put_be32(length, static_cast<uint32_t>(size));
  s->video_ring->write_without_replacement(length, sizeof(length), 0);
  s->video_ring->write_without_replacement(data, size, 0);
}

void TelegramCall::pump_video_relay_(CallSession *s) {
  if (s->relay_fd < 0)
    return;
  if (!s->relay_connected) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(s->relay_fd, &writable);
    struct timeval now {};
    if (::select(s->relay_fd + 1, nullptr, &writable, nullptr, &now) <= 0)
      return;
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(s->relay_fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
      ESP_LOGW(TAG, "Video relay %s:%u not reachable", this->video_host_, this->video_port_);
      this->close_video_relay_(s);
      return;
    }
    s->relay_connected = true;
  }
  uint8_t command[8];
  const int got = ::recv(s->relay_fd, command, sizeof(command), MSG_DONTWAIT);
  if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
    this->close_video_relay_(s);
    return;
  }
  if (got > 0 && memchr(command, RELAY_HANGUP, static_cast<size_t>(got)) != nullptr) {
    s->relay_hangup = true;
    return;
  }
  for (;;) {
    if (s->video_item == nullptr) {
      size_t length = 0;
      s->video_item = static_cast<uint8_t *>(s->video_ring->receive_acquire(length, VIDEO_SEND_MAX, 0));
      if (s->video_item == nullptr)
        return;
      s->video_item_len = length;
      s->video_item_off = 0;
    }
    const int sent = ::send(s->relay_fd, s->video_item + s->video_item_off, s->video_item_len - s->video_item_off,
                            MSG_DONTWAIT);
    if (sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        this->close_video_relay_(s);
      return;
    }
    s->video_item_off += static_cast<size_t>(sent);
    if (s->video_item_off < s->video_item_len)
      return;
    s->video_ring->receive_release(s->video_item);
    s->video_item = nullptr;
  }
}

void TelegramCall::send_frames_(CallSession *s) {
  if (s->tx == nullptr)
    return;
  for (;;) {
    if (s->tx_pending == 0) {
      if (s->tx->available() < sizeof(uint16_t))
        return;
      uint16_t length = 0;
      s->tx->read(&length, sizeof(length), 0);
      s->tx_pending = length;
    }
    if (s->tx->available() < sizeof(uint32_t) + s->tx_pending)
      return;
    uint32_t timestamp = 0;
    s->tx->read(&timestamp, sizeof(timestamp), 0);
    const size_t len = s->tx->read(s->frame, s->tx_pending, 0);
    s->tx_pending = 0;
    if (!s->ts_set) {
      s->ts_base = timestamp;
      s->ts_set = true;
    }
    esp_peer_audio_frame_t frame{};
    frame.pts = (timestamp - s->ts_base) / (SAMPLE_RATE / 1000);
    frame.data = s->frame;
    frame.size = static_cast<int>(len);
    esp_peer_send_audio(s->peer, &frame);
  }
}

}

#endif
