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
#include <opus.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>

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
constexpr uint32_t FRAME_MS = 20;
constexpr size_t FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS / 1000;
constexpr size_t DECODE_SAMPLES = SAMPLE_RATE * 120 / 1000;
constexpr size_t MIC_RING_BYTES = SAMPLE_RATE * 2 * 200 / 1000;
constexpr size_t MAX_PACKET = 1500;
constexpr uint32_t PUMP_MS = 10;
constexpr uint32_t RPC_WAIT_MS = 20000;
constexpr size_t MAX_SERVERS = 8;
constexpr size_t MAX_CANDIDATES = 16;
constexpr size_t SDP_SIZE = 2048;
constexpr int DEFAULT_PT = 111;
constexpr uint32_t PLAYOUT_MS = 60;
constexpr uint32_t PLAY_WAIT_MS = 20;

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
  OpusEncoder *encoder;
  OpusDecoder *decoder;
  ring_buffer::RingBuffer *ring;
  int16_t pcm[DECODE_SAMPLES];
  uint8_t packet[MAX_PACKET];
  uint32_t pts;
  bool playing;
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
  esp_peer_cfg_t peer_cfg;
  esp_peer_default_cfg_t peer_extra;
  TelegramCall *owner;
};

namespace {

std::atomic<uint8_t> mic_users{0};

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

}

void TelegramCall::attach(mtproto::Mtproto *mtproto, Component *owner) {
  this->mtproto_ = mtproto;
  this->owner_ = owner;
  mtproto->add_update_listener([this](TlReader updates) { this->on_updates_(updates); });
}

void TelegramCall::set_audio(resampler::ResamplerMicrophone *microphone, speaker::Speaker *speaker) {
  this->microphone_ = microphone;
  this->speaker_ = speaker;
  microphone->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (!this->mic_on_.load(std::memory_order_acquire))
      return;
    mic_users.fetch_add(1);
    CallSession *s = this->session_;
    if (this->mic_on_.load(std::memory_order_acquire) && s != nullptr && s->ring != nullptr)
      s->ring->write_without_replacement(data.data(), data.size(), 0);
    mic_users.fetch_sub(1);
  });
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

bool TelegramCall::start(int64_t user_id, int64_t access_hash, const std::string &name) {
  if (this->state_.load() != CallState::IDLE || this->microphone_ == nullptr || this->mtproto_ == nullptr ||
      !this->mtproto_->logged_in())
    return false;
  if (!this->begin_(true, 0, 0, nullptr))
    return false;
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
    if (this->state_.load() != CallState::IDLE || this->microphone_ == nullptr) {
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
    w.u32(0);
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
    if (xQueueReceive(s->events, &event, wait) == pdTRUE) {
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
      this->pump_audio_(s);
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
  int err = 0;
  s->encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (s->encoder != nullptr && err == OPUS_OK) {
    opus_encoder_ctl(s->encoder, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(s->encoder, OPUS_SET_COMPLEXITY(2));
    opus_encoder_ctl(s->encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s->encoder, OPUS_SET_PACKET_LOSS_PERC(10));
  }
  s->decoder = opus_decoder_create(SAMPLE_RATE, 1, &err);
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= MIC_RING_BYTES)
    s->ring = ring_buffer::RingBuffer::create(MIC_RING_BYTES).release();
  if (s->encoder == nullptr || s->decoder == nullptr || s->ring == nullptr) {
    this->fail_("No memory for the call's audio");
    return false;
  }
  s->media = true;
  App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() {
    if (this->session_ == nullptr || !this->session_->media)
      return;
    this->microphone_->set_target_format(SAMPLE_RATE, 1);
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, SAMPLE_RATE));
    this->speaker_->start();
    this->microphone_->start();
    this->mic_on_.store(true, std::memory_order_release);
  });

  esp_peer_default_cfg_t &extra = s->peer_extra;
  extra.agent_recv_timeout = 100;
  extra.rtp_cfg.audio_recv_jitter.cache_size = 16384;
  extra.rtp_cfg.send_pool_size = 16384;
  extra.rtp_cfg.send_queue_num = 32;
  extra.max_candidates = 8;
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
  cfg.no_auto_reconnect = true;
  cfg.enable_data_channel = false;
  cfg.extra_cfg = &extra;
  cfg.extra_size = sizeof(extra);
  cfg.ctx = s;
  cfg.on_state = [](esp_peer_state_t state, void *ctx) -> int {
    auto *session = static_cast<CallSession *>(ctx);
    TelegramCall *self = session->owner;
    if (state == ESP_PEER_STATE_CONNECTED) {
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
    if (frame != nullptr && frame->data != nullptr && frame->size > 0)
      session->owner->play_audio_(session, frame->data, static_cast<size_t>(frame->size));
    return 0;
  };
  esp_peer_pre_generate_cert();
  if (esp_peer_open(&cfg, esp_peer_get_default_impl(), &s->peer) != ESP_PEER_ERR_NONE || s->peer == nullptr) {
    s->peer = nullptr;
    this->fail_("No call connection possible");
    return false;
  }
  esp_peer_new_connection(s->peer);
  this->set_state_(CallState::CONNECTING);
  return true;
}

void TelegramCall::stop_media_(CallSession *s) {
  if (s->peer != nullptr) {
    esp_peer_close(s->peer);
    s->peer = nullptr;
  }
  this->mic_on_.store(false, std::memory_order_release);
  while (mic_users.load() != 0)
    vTaskDelay(1);
  if (s->media) {
    s->media = false;
    App.scheduler.set_timeout(this->owner_, static_cast<const char *>(nullptr), 0, [this]() {
      this->microphone_->stop();
      this->speaker_->stop();
    });
  }
  delete s->ring;
  s->ring = nullptr;
  if (s->encoder != nullptr)
    opus_encoder_destroy(s->encoder);
  if (s->decoder != nullptr)
    opus_decoder_destroy(s->decoder);
  s->encoder = nullptr;
  s->decoder = nullptr;
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
      for (JsonObject content : root["contents"].as<JsonArray>()) {
        const char *media = content["type"] | "";
        if (strcmp(media, "audio") == 0) {
          s->pending_offer_ssrc = json_u32(content["ssrc"]);
          s->remote_ssrc = s->pending_offer_ssrc;
        }
      }
      tl_read::copy(s->pending_offer_id, sizeof(s->pending_offer_id), id, strlen(id));
      s->pending_offer = true;
      this->answer_offer_(s);
    } else if (strcmp(type, "Candidates") == 0) {
      for (JsonObject candidate : root["candidates"].as<JsonArray>()) {
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

void TelegramCall::set_remote_sdp_(CallSession *s) {
  if (s->peer == nullptr || s->remote_set || s->remote_ufrag[0] == '\0' || s->remote_fingerprint[0] == '\0')
    return;
  if (s->outgoing && !s->local_ready)
    return;
  const int pt = s->local_pt >= 0 ? s->local_pt : DEFAULT_PT;
  const char *mid = s->local_mid[0] != '\0' ? s->local_mid : "0";
  int n = snprintf(s->sdp, SDP_SIZE,
                   "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE %s\r\n"
                   "m=audio 9 UDP/TLS/RTP/SAVPF %d\r\nc=IN IP4 0.0.0.0\r\na=rtcp:9 IN IP4 0.0.0.0\r\n"
                   "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=fingerprint:%s %s\r\na=setup:%s\r\na=mid:%s\r\n"
                   "a=sendrecv\r\na=rtcp-mux\r\na=rtpmap:%d opus/48000/2\r\na=fmtp:%d minptime=10;useinbandfec=1\r\n",
                   mid, pt, s->remote_ufrag, s->remote_pwd, s->remote_hash, s->remote_fingerprint,
                   s->outgoing ? "passive" : "active", mid, pt, pt);
  if (n > 0 && static_cast<size_t>(n) < SDP_SIZE && s->remote_ssrc != 0)
    snprintf(s->sdp + n, SDP_SIZE - n, "a=ssrc:%" PRIu32 " cname:telegram\r\n", s->remote_ssrc);
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
  line_value(sdp, "a=mid:", s->local_mid, sizeof(s->local_mid));
  if (line_value(sdp, "a=fingerprint:", fingerprint, sizeof(fingerprint))) {
    const char *space = strchr(fingerprint, ' ');
    if (space != nullptr) {
      tl_read::copy(s->local_hash, sizeof(s->local_hash), fingerprint, static_cast<size_t>(space - fingerprint));
      tl_read::copy(s->local_fingerprint, sizeof(s->local_fingerprint), space + 1, strlen(space + 1));
    }
  }
  for (const char *at = strstr(sdp, "a=rtpmap:"); at != nullptr; at = strstr(at + 9, "a=rtpmap:")) {
    char *end = nullptr;
    const long pt = strtol(at + 9, &end, 10);
    if (end != nullptr && strncasecmp(end, " opus/", 6) == 0) {
      s->local_pt = static_cast<int>(pt);
      break;
    }
  }
  const char *ssrc = strstr(sdp, "a=ssrc:");
  if (ssrc != nullptr)
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

void TelegramCall::pump_audio_(CallSession *s) {
  if (s->ring == nullptr || s->encoder == nullptr || s->peer == nullptr)
    return;
  const size_t frame_bytes = FRAME_SAMPLES * sizeof(int16_t);
  while (s->ring->available() >= frame_bytes) {
    s->ring->read(s->pcm, frame_bytes, 0);
    if (this->state_.load() != CallState::ACTIVE)
      continue;
    if (this->mute_.load())
      memset(s->pcm, 0, frame_bytes);
    const opus_int32 bytes = opus_encode(s->encoder, s->pcm, FRAME_SAMPLES, s->packet, MAX_PACKET);
    if (bytes > 0) {
      esp_peer_audio_frame_t frame{};
      frame.pts = s->pts;
      frame.data = s->packet;
      frame.size = bytes;
      esp_peer_send_audio(s->peer, &frame);
    }
    s->pts += FRAME_MS;
  }
}

void TelegramCall::play_audio_(CallSession *s, const uint8_t *data, size_t length) {
  if (s->decoder == nullptr)
    return;
  if (!s->playing) {
    s->playing = true;
    memset(s->pcm, 0, sizeof(s->pcm));
    size_t silence = SAMPLE_RATE * sizeof(int16_t) * PLAYOUT_MS / 1000;
    while (silence > 0) {
      const size_t chunk = std::min(silence, sizeof(s->pcm));
      this->speaker_->play(reinterpret_cast<const uint8_t *>(s->pcm), chunk, 0);
      silence -= chunk;
    }
  }
  const int frames = opus_decode(s->decoder, data, static_cast<opus_int32>(length), s->pcm,
                                 static_cast<int>(DECODE_SAMPLES), 0);
  if (frames <= 0)
    return;
  const uint8_t *pcm = reinterpret_cast<const uint8_t *>(s->pcm);
  size_t left = static_cast<size_t>(frames) * sizeof(int16_t);
  while (left > 0) {
    const size_t written = this->speaker_->play(pcm, left, pdMS_TO_TICKS(PLAY_WAIT_MS));
    if (written == 0)
      break;
    pcm += written;
    left -= written;
  }
}

}

#endif
