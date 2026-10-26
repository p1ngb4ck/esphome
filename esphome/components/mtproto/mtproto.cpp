#include "mtproto.h"
#include "crypto.h"

#include "esphome/components/esp32/hal.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_vfs_eventfd.h>
#include <freertos/idf_additions.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <miniz.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace esphome::mtproto {

static const char *const TAG = "mtproto";

namespace {

static constexpr uint32_t SESSION_VERSION = 1;
static constexpr uint16_t DC_PORT = 443;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t READ_TIMEOUT_MS = 15000;
static constexpr uint32_t PING_INTERVAL_MS = 60000;
static constexpr int32_t PING_DISCONNECT_S = 75;
static constexpr uint32_t ACK_DELAY_MS = 10000;
static constexpr uint32_t RETRY_MIN_MS = 2000;
static constexpr uint32_t RETRY_MAX_MS = 60000;
static constexpr size_t SCRATCH_OFFSET = Mtproto::BUFFER_SIZE / 2;
static constexpr size_t MESSAGE_OFFSET = 4 + 24;
static constexpr size_t INNER_HEADER = 32;

static constexpr uint32_t ID_REQ_PQ_MULTI = 0xbe7e8ef1;
static constexpr uint32_t ID_RES_PQ = 0x05162463;
static constexpr uint32_t ID_P_Q_INNER_DATA_DC = 0xa9f55f95;
static constexpr uint32_t ID_REQ_DH_PARAMS = 0xd712e4be;
static constexpr uint32_t ID_SERVER_DH_PARAMS_OK = 0xd0e8075c;
static constexpr uint32_t ID_SERVER_DH_INNER_DATA = 0xb5890dba;
static constexpr uint32_t ID_CLIENT_DH_INNER_DATA = 0x6643b654;
static constexpr uint32_t ID_SET_CLIENT_DH_PARAMS = 0xf5045f1f;
static constexpr uint32_t ID_DH_GEN_OK = 0x3bcbf734;
static constexpr uint32_t ID_RPC_RESULT = 0xf35c6d01;
static constexpr uint32_t ID_RPC_ERROR = 0x2144ca19;
static constexpr uint32_t ID_MSG_CONTAINER = 0x73f1f8dc;
static constexpr uint32_t ID_GZIP_PACKED = 0x3072cfa1;
static constexpr uint32_t ID_MSGS_ACK = 0x62d6b459;
static constexpr uint32_t ID_BAD_MSG_NOTIFICATION = 0xa7eff811;
static constexpr uint32_t ID_BAD_SERVER_SALT = 0xedab447b;
static constexpr uint32_t ID_NEW_SESSION_CREATED = 0x9ec20908;
static constexpr uint32_t ID_PONG = 0x347773c5;
static constexpr uint32_t ID_PING_DELAY_DISCONNECT = 0xf3427b8c;
static constexpr uint32_t ID_INVOKE_WITH_LAYER = 0xda9b0d0d;
static constexpr uint32_t ID_INIT_CONNECTION = 0xc1cd5ea9;
static constexpr uint32_t ID_USERS_GET_USERS = 0x0d91a548;
static constexpr uint32_t ID_INPUT_USER_SELF = 0xf7c1b13f;
static constexpr uint32_t ID_AUTH_EXPORT_LOGIN_TOKEN = 0xb7e085fe;
static constexpr uint32_t ID_AUTH_IMPORT_LOGIN_TOKEN = 0x95ac5ce4;
static constexpr uint32_t ID_AUTH_LOGIN_TOKEN = 0x629f1980;
static constexpr uint32_t ID_AUTH_LOGIN_TOKEN_MIGRATE_TO = 0x068e9916;
static constexpr uint32_t ID_AUTH_LOGIN_TOKEN_SUCCESS = 0x390d5c5e;
static constexpr uint32_t ID_AUTH_AUTHORIZATION = 0x2ea2c0d4;
static constexpr uint32_t ID_ACCOUNT_GET_PASSWORD = 0x548a30f5;
static constexpr uint32_t ID_ACCOUNT_PASSWORD = 0x957b50fb;
static constexpr uint32_t ID_PASSWORD_KDF_ALGO_MODPOW = 0x3a912d4a;
static constexpr uint32_t ID_AUTH_CHECK_PASSWORD = 0xd18b4d16;
static constexpr uint32_t ID_INPUT_CHECK_PASSWORD_SRP = 0xd27ff082;
static constexpr uint32_t ID_AUTH_LOG_OUT = 0x3e72ba19;
static constexpr uint32_t ID_USER = 0x31774388;
static constexpr uint32_t ID_UPDATE_LOGIN_TOKEN = 0x564fe691;
static constexpr uint32_t ID_UPDATES_TOO_LONG = 0xe317af7e;
static constexpr uint32_t ID_UPDATE_SHORT = 0x78d4dec1;
static constexpr uint32_t ID_UPDATES_COMBINED = 0x725b04c3;
static constexpr uint32_t ID_UPDATES = 0x74ae4240;

struct DcAddress {
  int dc;
  const char *ip;
};

const DcAddress DC_ADDRESSES[] = {
    {1, "149.154.175.53"}, {2, "149.154.167.51"}, {3, "149.154.175.100"},
    {4, "149.154.167.91"}, {5, "91.108.56.130"},
};

const char *dc_ip(int dc) {
  for (const DcAddress &address : DC_ADDRESSES) {
    if (address.dc == dc)
      return address.ip;
  }
  return nullptr;
}

void copy_text(char *dest, size_t size, const char *src, size_t length) {
  const size_t n = length < size - 1 ? length : size - 1;
  memcpy(dest, src, n);
  dest[n] = '\0';
}

uint32_t elapsed_until(uint32_t deadline, uint32_t now) {
  if (deadline == 0)
    return UINT32_MAX;
  const int32_t diff = static_cast<int32_t>(deadline - now);
  return diff <= 0 ? 0 : static_cast<uint32_t>(diff);
}

void base64url(const uint8_t *data, size_t length, char *out, size_t out_size) {
  static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  for (size_t i = 0; i < length && o + 4 < out_size; i += 3) {
    const uint32_t chunk = (static_cast<uint32_t>(data[i]) << 16) |
                           (i + 1 < length ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                           (i + 2 < length ? static_cast<uint32_t>(data[i + 2]) : 0);
    out[o++] = ALPHABET[(chunk >> 18) & 0x3F];
    out[o++] = ALPHABET[(chunk >> 12) & 0x3F];
    if (i + 1 < length)
      out[o++] = ALPHABET[(chunk >> 6) & 0x3F];
    if (i + 2 < length)
      out[o++] = ALPHABET[chunk & 0x3F];
  }
  out[o] = '\0';
}

void put_u32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

void put_u64(uint8_t *p, uint64_t value) {
  put_u32(p, static_cast<uint32_t>(value));
  put_u32(p + 4, static_cast<uint32_t>(value >> 32));
}

uint32_t get_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t get_u64(const uint8_t *p) { return static_cast<uint64_t>(get_u32(p)) | (static_cast<uint64_t>(get_u32(p + 4)) << 32); }

void kdf(const uint8_t *auth_key, const uint8_t *msg_key, int x, uint8_t *aes_key, uint8_t *aes_iv) {
  uint8_t a[32];
  uint8_t b[32];
  sha256_parts(msg_key, 16, auth_key + x, 36, a);
  sha256_parts(auth_key + 40 + x, 36, msg_key, 16, b);
  memcpy(aes_key, a, 8);
  memcpy(aes_key + 8, b + 8, 16);
  memcpy(aes_key + 24, a + 24, 8);
  memcpy(aes_iv, b, 8);
  memcpy(aes_iv + 8, a + 8, 16);
  memcpy(aes_iv + 24, b + 24, 8);
}

bool read_bytes_field(TlReader object, const char *name, const uint8_t *&data, size_t &length) {
  TlReader field;
  return tl_field(object, tl_hash(name), field) && field.bytes(data, length);
}

bool read_text_field(TlReader object, const char *name, char *out, size_t size) {
  const uint8_t *data = nullptr;
  size_t length = 0;
  if (!read_bytes_field(object, name, data, length)) {
    out[0] = '\0';
    return false;
  }
  copy_text(out, size, reinterpret_cast<const char *>(data), length);
  return true;
}

}

const char *state_name(State state) {
  switch (state) {
    case State::IDLE:
      return "idle";
    case State::NO_API:
      return "no_api";
    case State::CONNECTING:
      return "connecting";
    case State::LOGIN_QR:
      return "login_qr";
    case State::PASSWORD:
      return "password";
    case State::LOGGED_IN:
      return "logged_in";
    case State::ERROR:
      return "error";
  }
  return "unknown";
}

Mtproto *Mtproto::create() {
  void *mem = RAMAllocator<Mtproto>(RAMAllocator<Mtproto>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) Mtproto();
  return new Mtproto();
}

void Mtproto::set_default_api(int32_t api_id, const char *api_hash) {
  this->api_id_ = api_id;
  copy_text(this->api_hash_, sizeof(this->api_hash_), api_hash, strlen(api_hash));
}

void Mtproto::set_client_info(const char *device_model, const char *app_version, const char *lang_code) {
  this->device_model_ = device_model;
  this->app_version_ = app_version;
  this->lang_code_ = lang_code;
}

void Mtproto::setup() {
  this->pref_ = global_preferences->make_preference<SessionPref>(fnv1_hash("mtproto_session"));
  SessionPref saved{};
  if (this->pref_.load(&saved) && saved.version == SESSION_VERSION) {
    this->session_ = saved;
    if (saved.api_id != 0) {
      this->api_id_ = saved.api_id;
      copy_text(this->api_hash_, sizeof(this->api_hash_), saved.api_hash, strlen(saved.api_hash));
    }
    if (saved.dc > 0 && dc_ip(saved.dc) != nullptr) {
      this->dc_ = saved.dc;
      this->home_dc_ = saved.dc;
    }
    if (saved.user_id != 0) {
      this->user_id_ = saved.user_id;
      memcpy(this->auth_key_, saved.auth_key, sizeof(this->auth_key_));
      this->has_key_ = true;
      this->home_key_ = true;
      copy_text(this->user_name_, sizeof(this->user_name_), saved.name, strlen(saved.name));
    }
  }
  if (!this->enabled_) {
    this->disable_loop();
    return;
  }
  this->commands_ = xQueueCreateWithCaps(4, sizeof(Command), MALLOC_CAP_SPIRAM);
  this->events_ = xQueueCreateWithCaps(8, sizeof(Event), MALLOC_CAP_SPIRAM);
  const esp_vfs_eventfd_config_t efd_config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&efd_config);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    this->wake_fd_ = eventfd(0, 0);
  if (this->commands_ == nullptr || this->events_ == nullptr || this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "No PSRAM for the queues or no eventfd");
    this->mark_failed();
    return;
  }
#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
  this->network_up_.store(network::global_network_component != nullptr &&
                          network::global_network_component->get_active_interface() !=
                              network::NetworkInterface::NETWORK_INTERFACE_NONE);
#endif
  if (!this->task_.create(task_entry_, "mtproto", TASK_STACK, this, TASK_PRIORITY, false, LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for the task");
    this->mark_failed();
    return;
  }
#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
  this->register_service(&Mtproto::check_password, "mtproto_check_password", {"password"});
  this->register_service(&Mtproto::logout, "mtproto_logout");
  this->register_service(&Mtproto::set_api, "mtproto_set_api", {"api_id", "api_hash"});
#endif
  this->started_ = true;
  this->set_state_(this->api_id_ == 0 ? State::NO_API : State::CONNECTING);
  this->disable_loop();
}

void Mtproto::dump_config() {
  ESP_LOGCONFIG(TAG,
                "MTProto:\n"
                "  Enabled: %s\n"
                "  Layer: %d\n"
                "  API id set: %s\n"
                "  Data center: %d\n"
                "  Logged in: %s",
                YESNO(this->enabled_), MTPROTO_LAYER, YESNO(this->api_id_ != 0), this->home_dc_,
                YESNO(this->user_id_ != 0));
}

void Mtproto::loop() {
  Event event;
  while (xQueueReceive(this->events_, &event, 0) == pdTRUE) {
    switch (event.type) {
      case EventType::STATE:
        this->state_callbacks_.call(event.state);
        this->state_trigger_.trigger(state_name(event.state));
        break;
      case EventType::SAVE_SESSION: {
        SessionPref copy;
        {
          LockGuard guard(this->lock_);
          copy = this->session_;
        }
        this->pref_.save(&copy);
        global_preferences->sync();
        memset(copy.auth_key, 0, sizeof(copy.auth_key));
        break;
      }
    }
  }
  this->disable_loop();
}

#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
void Mtproto::on_active_interface(network::NetworkInterface interface) {
  this->network_up_.store(interface != network::NetworkInterface::NETWORK_INTERFACE_NONE);
  if (this->started_)
    this->post_command_(CommandType::WAKE, 0, "");
}
#endif

void Mtproto::set_api(int32_t api_id, std::string api_hash) {
  if (this->started_)
    this->post_command_(CommandType::SET_API, api_id, api_hash.c_str());
}

void Mtproto::check_password(std::string password) {
  if (this->started_)
    this->post_command_(CommandType::PASSWORD, 0, password.c_str());
}

void Mtproto::logout() {
  if (this->started_)
    this->post_command_(CommandType::LOGOUT, 0, "");
}

std::string Mtproto::qr_url() const {
  LockGuard guard(this->lock_);
  return this->qr_url_;
}

std::string Mtproto::user_name() const {
  LockGuard guard(this->lock_);
  return this->user_name_;
}

std::string Mtproto::password_hint() const {
  LockGuard guard(this->lock_);
  return this->hint_;
}

std::string Mtproto::error() const {
  LockGuard guard(this->lock_);
  return this->error_;
}

void Mtproto::post_command_(CommandType type, int32_t api_id, const char *text) {
  Command command{};
  command.type = type;
  command.api_id = api_id;
  copy_text(command.text, sizeof(command.text), text, strlen(text));
  if (xQueueSend(this->commands_, &command, 0) != pdTRUE)
    ESP_LOGW(TAG, "Command queue full");
  memset(command.text, 0, sizeof(command.text));
  this->wake_();
}

void Mtproto::wake_() {
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

void Mtproto::post_event_(EventType type) {
  Event event{};
  event.type = type;
  event.state = this->state_;
  xQueueSend(this->events_, &event, 0);
  this->enable_loop_soon_any_context();
}

void Mtproto::set_state_(State state) {
  this->state_ = state;
  this->post_event_(EventType::STATE);
}

void Mtproto::fail_(const char *text) {
  {
    LockGuard guard(this->lock_);
    copy_text(this->error_, sizeof(this->error_), text, strlen(text));
  }
  ESP_LOGW(TAG, "%s", text);
}

void Mtproto::task_entry_(void *param) { static_cast<Mtproto *>(param)->run_(); }

bool Mtproto::open_buffers_() {
  if (this->rx_ == nullptr) {
    this->rx_ = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    this->rx_size_ = BUFFER_SIZE;
  }
  if (this->tx_ == nullptr)
    this->tx_ = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return this->rx_ != nullptr && this->tx_ != nullptr;
}

void Mtproto::close_buffers_() {
  this->drop_pending_();
  heap_caps_free(this->rx_);
  heap_caps_free(this->tx_);
  this->rx_ = nullptr;
  this->tx_ = nullptr;
  this->rx_size_ = 0;
}

void Mtproto::run_() {
  for (;;) {
    const bool can_run = this->api_id_ != 0 && this->network_up_.load();
    if (!can_run || this->retry_delay_ != 0) {
      if (this->api_id_ == 0 && this->state_ != State::NO_API)
        this->set_state_(State::NO_API);
      const uint32_t wait = can_run ? this->retry_delay_ : UINT32_MAX;
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(this->wake_fd_, &readable);
      struct timeval tv = {.tv_sec = static_cast<time_t>(wait / 1000),
                           .tv_usec = static_cast<suseconds_t>((wait % 1000) * 1000)};
      const int n = ::select(this->wake_fd_ + 1, &readable, nullptr, nullptr, wait == UINT32_MAX ? nullptr : &tv);
      if (n > 0) {
        uint64_t value = 0;
        ::read(this->wake_fd_, &value, sizeof(value));
        Command command;
        while (xQueueReceive(this->commands_, &command, 0) == pdTRUE) {
          this->handle_command_(command);
          memset(command.text, 0, sizeof(command.text));
        }
      } else if (n == 0) {
        this->retry_delay_ = 0;
      }
      continue;
    }
    if (this->user_id_ == 0 || this->state_ != State::LOGGED_IN)
      this->set_state_(State::CONNECTING);
    if (!this->open_buffers_()) {
      this->fail_("No PSRAM for the connection buffers");
      this->set_state_(State::ERROR);
      this->retry_delay_ = RETRY_MAX_MS;
      continue;
    }
    if (this->target_dc_ != 0) {
      this->dc_ = this->target_dc_;
      this->target_dc_ = 0;
    }
    bool ok = this->connect_(this->dc_);
    if (ok && !this->has_key_) {
      ok = this->handshake_();
      if (!ok && this->error().empty())
        this->fail_("Key exchange with Telegram failed");
    }
    if (!ok) {
      this->disconnect_();
      this->backoff_ms_ = std::min(std::max(this->backoff_ms_ * 2, RETRY_MIN_MS), RETRY_MAX_MS);
      this->retry_delay_ = this->backoff_ms_;
      if (this->state_ != State::LOGGED_IN)
        this->set_state_(State::ERROR);
      continue;
    }
    this->start_session_();
    if (this->migrate_token_len_ != 0) {
      this->request_import_(this->migrate_token_, this->migrate_token_len_);
      this->migrate_token_len_ = 0;
    } else if (this->user_id_ != 0) {
      this->request_self_();
    } else {
      this->request_login_token_();
    }

    bool reconnect = false;
    uint32_t rx_deadline = 0;
    while (!reconnect) {
      const uint32_t now = millis();
      if (this->target_dc_ != 0 || this->logging_out_ || this->api_id_ == 0)
        break;
      if (this->next_ping_ != 0 && elapsed_until(this->next_ping_, now) == 0) {
        if (!this->send_ping_())
          break;
        this->next_ping_ = now + PING_INTERVAL_MS;
        rx_deadline = now + READ_TIMEOUT_MS * 2;
      }
      if (this->ack_count_ != 0 && this->ack_deadline_ != 0 && elapsed_until(this->ack_deadline_, now) == 0) {
        if (!this->send_acks_())
          break;
      }
      if (this->token_deadline_ != 0 && elapsed_until(this->token_deadline_, now) == 0) {
        this->token_deadline_ = 0;
        if (this->user_id_ == 0 && (this->state_ == State::LOGIN_QR || this->state_ == State::ERROR))
          this->request_login_token_();
      }
      if (rx_deadline != 0 && elapsed_until(rx_deadline, now) == 0) {
        ESP_LOGW(TAG, "No answer from the server, reconnecting");
        break;
      }
      uint32_t wait = elapsed_until(this->next_ping_, now);
      if (this->ack_count_ != 0)
        wait = std::min(wait, elapsed_until(this->ack_deadline_, now));
      wait = std::min(wait, elapsed_until(this->token_deadline_, now));
      wait = std::min(wait, elapsed_until(rx_deadline, now));
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(this->fd_, &readable);
      FD_SET(this->wake_fd_, &readable);
      struct timeval tv = {.tv_sec = static_cast<time_t>(wait / 1000),
                           .tv_usec = static_cast<suseconds_t>((wait % 1000) * 1000)};
      const int n = ::select(std::max(this->fd_, this->wake_fd_) + 1, &readable, nullptr, nullptr,
                             wait == UINT32_MAX ? nullptr : &tv);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (n > 0 && FD_ISSET(this->wake_fd_, &readable)) {
        uint64_t value = 0;
        ::read(this->wake_fd_, &value, sizeof(value));
        Command command;
        while (xQueueReceive(this->commands_, &command, 0) == pdTRUE) {
          if (command.type == CommandType::WAKE && !this->network_up_.load())
            reconnect = true;
          this->handle_command_(command);
          memset(command.text, 0, sizeof(command.text));
        }
      }
      if (n > 0 && FD_ISSET(this->fd_, &readable)) {
        if (!this->receive_(READ_TIMEOUT_MS)) {
          reconnect = true;
          break;
        }
        rx_deadline = 0;
      }
    }
    if (this->logging_out_) {
      this->logging_out_ = false;
      this->has_key_ = false;
      this->home_key_ = false;
      this->user_id_ = 0;
      {
        LockGuard guard(this->lock_);
        this->user_name_[0] = '\0';
        this->session_.user_id = 0;
        memset(this->session_.auth_key, 0, sizeof(this->session_.auth_key));
        this->session_.name[0] = '\0';
      }
      this->post_event_(EventType::SAVE_SESSION);
    }
    this->disconnect_();
    if (this->target_dc_ == 0)
      this->retry_delay_ = RETRY_MIN_MS;
  }
}

void Mtproto::handle_command_(const Command &command) {
  switch (command.type) {
    case CommandType::WAKE:
      break;
    case CommandType::SET_API: {
      if (command.api_id <= 0 || strlen(command.text) != 32) {
        this->fail_("API ID / hash invalid");
        this->set_state_(State::NO_API);
        break;
      }
      this->api_id_ = command.api_id;
      copy_text(this->api_hash_, sizeof(this->api_hash_), command.text, strlen(command.text));
      {
        LockGuard guard(this->lock_);
        this->session_.version = SESSION_VERSION;
        this->session_.api_id = this->api_id_;
        copy_text(this->session_.api_hash, sizeof(this->session_.api_hash), this->api_hash_, strlen(this->api_hash_));
        this->error_[0] = '\0';
      }
      this->post_event_(EventType::SAVE_SESSION);
      this->retry_delay_ = 0;
      if (this->fd_ >= 0 && this->user_id_ == 0)
        this->request_login_token_();
      break;
    }
    case CommandType::PASSWORD:
      if (this->state_ == State::PASSWORD && this->fd_ >= 0)
        this->submit_password_(command.text);
      break;
    case CommandType::LOGOUT:
      if (this->fd_ >= 0 && this->user_id_ != 0) {
        TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
        w.u32(ID_AUTH_LOG_OUT);
        this->send_request_(Request::LOGOUT, w.data(), w.size());
      }
      this->logging_out_ = true;
      break;
  }
}

bool Mtproto::connect_(int dc) {
  const char *ip = dc_ip(dc);
  if (ip == nullptr)
    return false;
  this->fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->fd_ < 0) {
    this->fail_("No free socket for the Telegram connection");
    return false;
  }
  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DC_PORT);
  inet_aton(ip, &addr.sin_addr);
  const int flags = fcntl(this->fd_, F_GETFL, 0);
  fcntl(this->fd_, F_SETFL, flags | O_NONBLOCK);
  int result = ::connect(this->fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
  if (result != 0 && errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(this->fd_, &writable);
    struct timeval tv = {.tv_sec = CONNECT_TIMEOUT_MS / 1000, .tv_usec = 0};
    result = ::select(this->fd_ + 1, nullptr, &writable, nullptr, &tv) > 0 ? 0 : -1;
    if (result == 0) {
      int error = 0;
      socklen_t len = sizeof(error);
      getsockopt(this->fd_, SOL_SOCKET, SO_ERROR, &error, &len);
      result = error == 0 ? 0 : -1;
    }
  }
  if (result != 0) {
    char text[48];
    snprintf(text, sizeof(text), "No connection to Telegram DC%d", dc);
    this->fail_(text);
    return false;
  }
  fcntl(this->fd_, F_SETFL, flags & ~O_NONBLOCK);
  struct timeval timeout = {.tv_sec = READ_TIMEOUT_MS / 1000, .tv_usec = 0};
  setsockopt(this->fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(this->fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  const int one = 1;
  setsockopt(this->fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  static const uint8_t INTERMEDIATE[4] = {0xee, 0xee, 0xee, 0xee};
  if (::send(this->fd_, INTERMEDIATE, sizeof(INTERMEDIATE), 0) != sizeof(INTERMEDIATE)) {
    this->fail_("Telegram connection closed at once");
    return false;
  }
  this->dc_ = dc;
  ESP_LOGI(TAG, "Connected to DC%d", dc);
  return true;
}

void Mtproto::disconnect_() {
  if (this->fd_ >= 0) {
    ::close(this->fd_);
    this->fd_ = -1;
  }
  this->drop_pending_();
  this->ack_count_ = 0;
  this->next_ping_ = 0;
  this->ack_deadline_ = 0;
  this->token_deadline_ = 0;
}

bool Mtproto::send_packet_(const uint8_t *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const ssize_t n = ::send(this->fd_, data + sent, length - sent, 0);
    if (n <= 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool Mtproto::read_exact_(uint8_t *out, size_t length, uint32_t timeout_ms) {
  size_t got = 0;
  while (got < length) {
    const ssize_t n = ::recv(this->fd_, out + got, length - got, 0);
    if (n <= 0)
      return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

int Mtproto::read_packet_(size_t &length, uint32_t timeout_ms) {
  uint8_t header[4];
  if (!this->read_exact_(header, sizeof(header), timeout_ms))
    return -1;
  length = get_u32(header);
  if (length == 4) {
    uint8_t code[4];
    if (!this->read_exact_(code, sizeof(code), timeout_ms))
      return -1;
    ESP_LOGW(TAG, "Transport error %" PRId32, static_cast<int32_t>(get_u32(code)));
    if (static_cast<int32_t>(get_u32(code)) == -404) {
      this->has_key_ = false;
      this->home_key_ = false;
    }
    return -1;
  }
  if (length < 8 || length > MAX_PACKET)
    return -1;
  if (length > this->rx_size_) {
    void *bigger = heap_caps_realloc(this->rx_, length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bigger == nullptr)
      return -1;
    this->rx_ = static_cast<uint8_t *>(bigger);
    this->rx_size_ = length;
  }
  return this->read_exact_(this->rx_, length, timeout_ms) ? 1 : -1;
}

uint64_t Mtproto::next_msg_id_() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  const uint64_t seconds = static_cast<uint64_t>(static_cast<int64_t>(tv.tv_sec) + this->time_offset_);
  uint64_t msg_id = (seconds << 32) | ((static_cast<uint64_t>(tv.tv_usec) << 32) / 1000000ULL);
  msg_id &= ~static_cast<uint64_t>(3);
  if (msg_id <= this->last_msg_id_)
    msg_id = this->last_msg_id_ + 4;
  this->last_msg_id_ = msg_id;
  return msg_id;
}

bool Mtproto::plain_call_(const uint8_t *body, size_t length, const uint8_t *&reply, size_t &reply_len) {
  uint8_t *p = this->tx_;
  put_u32(p, static_cast<uint32_t>(20 + length));
  memset(p + 4, 0, 8);
  put_u64(p + 12, this->next_msg_id_());
  put_u32(p + 20, static_cast<uint32_t>(length));
  memcpy(p + 24, body, length);
  if (!this->send_packet_(p, 24 + length))
    return false;
  size_t packet = 0;
  if (this->read_packet_(packet, READ_TIMEOUT_MS) != 1 || packet < 20)
    return false;
  const uint32_t body_len = get_u32(this->rx_ + 16);
  if (get_u64(this->rx_) != 0 || body_len > packet - 20)
    return false;
  reply = this->rx_ + 20;
  reply_len = body_len;
  return true;
}

bool Mtproto::handshake_() {
  uint8_t nonce[16];
  uint8_t server_nonce[16];
  uint8_t new_nonce[32];
  random_bytes(nonce, sizeof(nonce));
  uint8_t *scratch = this->tx_ + SCRATCH_OFFSET;
  const size_t scratch_size = BUFFER_SIZE - SCRATCH_OFFSET;

  TlWriter w(scratch, scratch_size);
  w.u32(ID_REQ_PQ_MULTI);
  w.raw(nonce, sizeof(nonce));
  const uint8_t *reply = nullptr;
  size_t reply_len = 0;
  if (!this->plain_call_(w.data(), w.size(), reply, reply_len))
    return false;
  TlReader r(reply, reply_len);
  uint32_t id = 0;
  uint8_t check[16];
  const uint8_t *pq_bytes = nullptr;
  size_t pq_len = 0;
  uint32_t vector_id = 0;
  uint32_t count = 0;
  if (!r.u32(id) || id != ID_RES_PQ || !r.raw(check, 16) || memcmp(check, nonce, 16) != 0 ||
      !r.raw(server_nonce, 16) || !r.bytes(pq_bytes, pq_len) || pq_len > 8 || !r.u32(vector_id) || !r.u32(count))
    return false;
  const ServerKey *key = nullptr;
  for (uint32_t i = 0; i < count; i++) {
    uint64_t fingerprint = 0;
    if (!r.u64(fingerprint))
      return false;
    if (key == nullptr)
      key = find_server_key(fingerprint);
  }
  if (key == nullptr) {
    this->fail_("No known server key");
    return false;
  }
  uint64_t pq = 0;
  for (size_t i = 0; i < pq_len; i++)
    pq = (pq << 8) | pq_bytes[i];
  const uint64_t p_value = factor_pq(pq);
  if (p_value == 0)
    return false;
  const uint64_t q_value = pq / p_value;
  uint8_t p_bytes[4];
  uint8_t q_bytes[4];
  for (int i = 0; i < 4; i++) {
    p_bytes[i] = static_cast<uint8_t>(p_value >> (24 - 8 * i));
    q_bytes[i] = static_cast<uint8_t>(q_value >> (24 - 8 * i));
  }
  random_bytes(new_nonce, sizeof(new_nonce));

  uint8_t inner[192];
  TlWriter iw(inner, sizeof(inner));
  iw.u32(ID_P_Q_INNER_DATA_DC);
  iw.bytes(pq_bytes, pq_len);
  iw.bytes(p_bytes, 4);
  iw.bytes(q_bytes, 4);
  iw.raw(nonce, 16);
  iw.raw(server_nonce, 16);
  iw.raw(new_nonce, 32);
  iw.i32(this->dc_);
  if (!iw.ok())
    return false;
  random_bytes(inner + iw.size(), sizeof(inner) - iw.size());

  uint8_t padded[RSA_BYTES];
  uint8_t encrypted[RSA_BYTES];
  bool below = false;
  for (int attempt = 0; attempt < 16 && !below; attempt++) {
    uint8_t temp_key[32];
    random_bytes(temp_key, sizeof(temp_key));
    uint8_t *aes_part = padded + 32;
    for (size_t i = 0; i < sizeof(inner); i++)
      aes_part[i] = inner[sizeof(inner) - 1 - i];
    sha256_parts(temp_key, 32, inner, sizeof(inner), aes_part + 192);
    static const uint8_t ZERO_IV[32] = {};
    if (!aes_ige(temp_key, ZERO_IV, aes_part, 224, true))
      return false;
    uint8_t hash[32];
    sha256_parts(aes_part, 224, nullptr, 0, hash);
    for (int i = 0; i < 32; i++)
      padded[i] = temp_key[i] ^ hash[i];
    if (!rsa_encrypt(*key, padded, encrypted, below))
      return false;
  }
  if (!below)
    return false;

  w = TlWriter(scratch, scratch_size);
  w.u32(ID_REQ_DH_PARAMS);
  w.raw(nonce, 16);
  w.raw(server_nonce, 16);
  w.bytes(p_bytes, 4);
  w.bytes(q_bytes, 4);
  w.u64(key->fingerprint);
  w.bytes(encrypted, RSA_BYTES);
  if (!this->plain_call_(w.data(), w.size(), reply, reply_len))
    return false;
  r = TlReader(reply, reply_len);
  const uint8_t *answer = nullptr;
  size_t answer_len = 0;
  if (!r.u32(id) || id != ID_SERVER_DH_PARAMS_OK || !r.raw(check, 16) || memcmp(check, nonce, 16) != 0 ||
      !r.raw(check, 16) || memcmp(check, server_nonce, 16) != 0 || !r.bytes(answer, answer_len) ||
      answer_len % 16 != 0 || answer_len > 1024)
    return false;

  uint8_t h1[20];
  uint8_t h2[20];
  uint8_t h3[20];
  sha1_parts(new_nonce, 32, server_nonce, 16, h1);
  sha1_parts(server_nonce, 16, new_nonce, 32, h2);
  sha1_parts(new_nonce, 32, new_nonce, 32, h3);
  uint8_t tmp_key[32];
  uint8_t tmp_iv[32];
  memcpy(tmp_key, h1, 20);
  memcpy(tmp_key + 20, h2, 12);
  memcpy(tmp_iv, h2 + 12, 8);
  memcpy(tmp_iv + 8, h3, 20);
  memcpy(tmp_iv + 28, new_nonce, 4);

  uint8_t *decrypted = scratch;
  memcpy(decrypted, answer, answer_len);
  if (!aes_ige(tmp_key, tmp_iv, decrypted, answer_len, false))
    return false;
  TlReader inner_reader(decrypted + 20, answer_len - 20);
  TlReader start = inner_reader;
  int32_t g = 0;
  const uint8_t *dh_prime = nullptr;
  size_t dh_prime_len = 0;
  const uint8_t *g_a = nullptr;
  size_t g_a_len = 0;
  int32_t server_time = 0;
  if (!inner_reader.u32(id) || id != ID_SERVER_DH_INNER_DATA || !inner_reader.raw(check, 16) ||
      memcmp(check, nonce, 16) != 0 || !inner_reader.raw(check, 16) || memcmp(check, server_nonce, 16) != 0 ||
      !inner_reader.i32(g) || !inner_reader.bytes(dh_prime, dh_prime_len) || !inner_reader.bytes(g_a, g_a_len) ||
      !inner_reader.i32(server_time))
    return false;
  uint8_t answer_hash[20];
  sha1(start.pos(), static_cast<size_t>(inner_reader.pos() - start.pos()), answer_hash);
  if (memcmp(answer_hash, decrypted, 20) != 0 || g_a_len > DH_BYTES) {
    this->fail_("Server DH answer corrupt");
    return false;
  }
  if (!dh_check(static_cast<uint32_t>(g), dh_prime, dh_prime_len) ||
      !dh_check_value(g_a, g_a_len, dh_prime, dh_prime_len)) {
    this->fail_("Server DH parameters rejected");
    return false;
  }
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  this->time_offset_ = server_time - static_cast<int32_t>(tv.tv_sec);

  uint8_t prime[DH_BYTES];
  uint8_t ga[DH_BYTES];
  memcpy(prime, dh_prime, DH_BYTES);
  memset(ga, 0, sizeof(ga));
  memcpy(ga + DH_BYTES - g_a_len, g_a, g_a_len);
  uint8_t b[DH_BYTES];
  uint8_t g_b[DH_BYTES];
  uint8_t g_byte[1] = {static_cast<uint8_t>(g)};
  bool b_ok = false;
  for (int attempt = 0; attempt < 4 && !b_ok; attempt++) {
    random_bytes(b, sizeof(b));
    b_ok = mod_exp(g_byte, 1, b, sizeof(b), prime, sizeof(prime), g_b) &&
           dh_check_value(g_b, sizeof(g_b), prime, sizeof(prime));
  }
  if (!b_ok)
    return false;

  uint8_t client_inner[DH_BYTES + 64];
  TlWriter cw(client_inner + 20, sizeof(client_inner) - 20 - 16);
  cw.u32(ID_CLIENT_DH_INNER_DATA);
  cw.raw(nonce, 16);
  cw.raw(server_nonce, 16);
  cw.u64(0);
  cw.bytes(g_b, sizeof(g_b));
  if (!cw.ok())
    return false;
  sha1(client_inner + 20, cw.size(), client_inner);
  size_t total = 20 + cw.size();
  const size_t padding = (16 - total % 16) % 16;
  random_bytes(client_inner + total, padding);
  total += padding;
  if (!aes_ige(tmp_key, tmp_iv, client_inner, total, true))
    return false;

  w = TlWriter(scratch, scratch_size);
  w.u32(ID_SET_CLIENT_DH_PARAMS);
  w.raw(nonce, 16);
  w.raw(server_nonce, 16);
  w.bytes(client_inner, total);
  if (!this->plain_call_(w.data(), w.size(), reply, reply_len))
    return false;
  r = TlReader(reply, reply_len);
  uint8_t nonce_hash[16];
  if (!r.u32(id) || id != ID_DH_GEN_OK || !r.raw(check, 16) || memcmp(check, nonce, 16) != 0 || !r.raw(check, 16) ||
      memcmp(check, server_nonce, 16) != 0 || !r.raw(nonce_hash, 16)) {
    this->fail_("Key exchange refused");
    return false;
  }
  if (!mod_exp(ga, sizeof(ga), b, sizeof(b), prime, sizeof(prime), this->auth_key_))
    return false;
  memset(b, 0, sizeof(b));
  uint8_t key_hash[20];
  sha1(this->auth_key_, sizeof(this->auth_key_), key_hash);
  uint8_t aux[41];
  memcpy(aux, new_nonce, 32);
  aux[32] = 1;
  memcpy(aux + 33, key_hash, 8);
  uint8_t expected[20];
  sha1(aux, sizeof(aux), expected);
  if (memcmp(expected + 4, nonce_hash, 16) != 0) {
    this->fail_("Key exchange check failed");
    return false;
  }
  memcpy(this->auth_key_id_, key_hash + 12, 8);
  uint64_t salt = 0;
  for (int i = 0; i < 8; i++)
    reinterpret_cast<uint8_t *>(&salt)[i] = new_nonce[i] ^ server_nonce[i];
  this->server_salt_ = salt;
  this->has_key_ = true;
  this->home_key_ = this->dc_ == this->home_dc_;
  ESP_LOGI(TAG, "Auth key created on DC%d", this->dc_);
  return true;
}

void Mtproto::start_session_() {
  uint8_t key_hash[20];
  sha1(this->auth_key_, sizeof(this->auth_key_), key_hash);
  memcpy(this->auth_key_id_, key_hash + 12, 8);
  random_bytes(reinterpret_cast<uint8_t *>(&this->session_id_), sizeof(this->session_id_));
  this->seq_count_ = 0;
  this->init_sent_ = false;
  this->ack_count_ = 0;
  this->next_ping_ = millis() + PING_INTERVAL_MS;
}

void Mtproto::encode_wrapped_(TlWriter &w, const uint8_t *query, size_t length) {
  if (!this->init_sent_) {
    w.u32(ID_INVOKE_WITH_LAYER);
    w.i32(MTPROTO_LAYER);
    w.u32(ID_INIT_CONNECTION);
    w.u32(0);
    w.i32(this->api_id_);
    w.string(this->device_model_);
    w.string("ESP-IDF");
    w.string(this->app_version_);
    w.string(this->lang_code_);
    w.string("");
    w.string(this->lang_code_);
  }
  w.raw(query, length);
}

bool Mtproto::send_message_(const uint8_t *body, size_t length, bool content, uint64_t &msg_id) {
  if (this->fd_ < 0 || !this->has_key_)
    return false;
  const size_t inner_len = INNER_HEADER + length;
  size_t padding = 12 + (16 - (inner_len + 12) % 16) % 16;
  const size_t total = inner_len + padding;
  if (MESSAGE_OFFSET + total > SCRATCH_OFFSET)
    return false;
  uint8_t *inner = this->tx_ + MESSAGE_OFFSET;
  msg_id = this->next_msg_id_();
  const uint32_t seq = content ? this->seq_count_++ * 2 + 1 : this->seq_count_ * 2;
  put_u64(inner, this->server_salt_);
  put_u64(inner + 8, this->session_id_);
  put_u64(inner + 16, msg_id);
  put_u32(inner + 24, seq);
  put_u32(inner + 28, static_cast<uint32_t>(length));
  memmove(inner + INNER_HEADER, body, length);
  random_bytes(inner + inner_len, padding);
  uint8_t large[32];
  sha256_parts(this->auth_key_ + 88, 32, inner, total, large);
  uint8_t *msg_key = this->tx_ + 4 + 8;
  memcpy(msg_key, large + 8, 16);
  uint8_t aes_key[32];
  uint8_t aes_iv[32];
  kdf(this->auth_key_, msg_key, 0, aes_key, aes_iv);
  if (!aes_ige(aes_key, aes_iv, inner, total, true))
    return false;
  put_u32(this->tx_, static_cast<uint32_t>(24 + total));
  memcpy(this->tx_ + 4, this->auth_key_id_, 8);
  return this->send_packet_(this->tx_, 4 + 24 + total);
}

bool Mtproto::send_request_(Request request, const uint8_t *body, size_t length) {
  Pending *slot = nullptr;
  for (Pending &pending : this->pending_) {
    if (pending.request == Request::NONE) {
      slot = &pending;
      break;
    }
  }
  if (slot == nullptr)
    return false;
  if (this->ack_count_ != 0 && !this->send_acks_())
    return false;
  uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(length + 512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (copy == nullptr)
    return false;
  TlWriter w(copy, length + 512);
  this->encode_wrapped_(w, body, length);
  if (!w.ok()) {
    heap_caps_free(copy);
    return false;
  }
  uint64_t msg_id = 0;
  if (!this->send_message_(copy, w.size(), true, msg_id)) {
    heap_caps_free(copy);
    return false;
  }
  this->init_sent_ = true;
  slot->msg_id = msg_id;
  slot->request = request;
  slot->body = copy;
  slot->length = w.size();
  return true;
}

bool Mtproto::resend_(uint64_t bad_msg_id) {
  for (Pending &pending : this->pending_) {
    if (pending.request != Request::NONE && pending.msg_id == bad_msg_id) {
      uint64_t msg_id = 0;
      if (!this->send_message_(pending.body, pending.length, true, msg_id))
        return false;
      pending.msg_id = msg_id;
      return true;
    }
  }
  return true;
}

bool Mtproto::send_acks_() {
  uint8_t body[8 + 8 + 8 * MAX_ACKS];
  TlWriter w(body, sizeof(body));
  w.u32(ID_MSGS_ACK);
  w.vector(static_cast<uint32_t>(this->ack_count_));
  for (size_t i = 0; i < this->ack_count_; i++)
    w.u64(this->acks_[i]);
  this->ack_count_ = 0;
  this->ack_deadline_ = 0;
  uint64_t msg_id = 0;
  return this->send_message_(body, w.size(), false, msg_id);
}

bool Mtproto::send_ping_() {
  uint8_t body[16];
  TlWriter w(body, sizeof(body));
  w.u32(ID_PING_DELAY_DISCONNECT);
  uint64_t ping_id = 0;
  random_bytes(reinterpret_cast<uint8_t *>(&ping_id), sizeof(ping_id));
  w.u64(ping_id);
  w.i32(PING_DISCONNECT_S);
  uint64_t msg_id = 0;
  return this->send_message_(body, w.size(), true, msg_id);
}

bool Mtproto::receive_(uint32_t timeout_ms) {
  size_t length = 0;
  if (this->read_packet_(length, timeout_ms) != 1)
    return false;
  if (length < 24 + INNER_HEADER || (length - 24) % 16 != 0 || memcmp(this->rx_, this->auth_key_id_, 8) != 0)
    return false;
  uint8_t msg_key[16];
  memcpy(msg_key, this->rx_ + 8, 16);
  uint8_t aes_key[32];
  uint8_t aes_iv[32];
  kdf(this->auth_key_, msg_key, 8, aes_key, aes_iv);
  uint8_t *inner = this->rx_ + 24;
  const size_t total = length - 24;
  if (!aes_ige(aes_key, aes_iv, inner, total, false))
    return false;
  uint8_t large[32];
  sha256_parts(this->auth_key_ + 96, 32, inner, total, large);
  if (memcmp(large + 8, msg_key, 16) != 0) {
    ESP_LOGW(TAG, "Message key mismatch");
    return false;
  }
  const uint64_t session = get_u64(inner + 8);
  const uint64_t msg_id = get_u64(inner + 16);
  const uint32_t seq = get_u32(inner + 24);
  const uint32_t body_len = get_u32(inner + 28);
  if (session != this->session_id_ || body_len > total - INNER_HEADER || total - INNER_HEADER - body_len < 12 ||
      total - INNER_HEADER - body_len > 1024)
    return false;
  if (seq & 1) {
    if (this->ack_count_ < MAX_ACKS)
      this->acks_[this->ack_count_++] = msg_id;
    if (this->ack_deadline_ == 0)
      this->ack_deadline_ = millis() + ACK_DELAY_MS;
  }
  this->handle_object_(TlReader(inner + INNER_HEADER, body_len), msg_id, 0);
  if (this->ack_count_ >= MAX_ACKS)
    return this->send_acks_();
  return true;
}

bool Mtproto::inflate_(const uint8_t *data, size_t length, uint8_t *&out, size_t &out_len) {
  if (length < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8)
    return false;
  const uint8_t flags = data[3];
  size_t offset = 10;
  if (flags & 0x04) {
    if (offset + 2 > length)
      return false;
    offset += 2 + (data[offset] | (data[offset + 1] << 8));
  }
  if (flags & 0x08) {
    while (offset < length && data[offset] != 0)
      offset++;
    offset++;
  }
  if (flags & 0x10) {
    while (offset < length && data[offset] != 0)
      offset++;
    offset++;
  }
  if (flags & 0x02)
    offset += 2;
  if (offset + 8 > length)
    return false;
  const uint32_t size = get_u32(data + length - 4);
  if (size == 0 || size > MAX_PACKET)
    return false;
  out = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *decompressor =
      static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (out == nullptr || decompressor == nullptr) {
    heap_caps_free(out);
    heap_caps_free(decompressor);
    out = nullptr;
    return false;
  }
  tinfl_init(decompressor);
  size_t in_size = length - offset - 8;
  size_t produced = size;
  const tinfl_status status = tinfl_decompress(decompressor, data + offset, &in_size, out, out, &produced,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  heap_caps_free(decompressor);
  if (status != TINFL_STATUS_DONE || produced != size) {
    heap_caps_free(out);
    out = nullptr;
    return false;
  }
  out_len = size;
  return true;
}

void Mtproto::handle_object_(TlReader reader, uint64_t msg_id, int depth) {
  uint32_t id = 0;
  if (depth > 4 || !reader.peek_u32(id))
    return;
  switch (id) {
    case ID_MSG_CONTAINER: {
      uint32_t count = 0;
      reader.skip(4);
      if (!reader.u32(count))
        return;
      for (uint32_t i = 0; i < count; i++) {
        uint64_t inner_id = 0;
        uint32_t seq = 0;
        uint32_t bytes = 0;
        if (!reader.u64(inner_id) || !reader.u32(seq) || !reader.u32(bytes) || reader.remaining() < bytes)
          return;
        if ((seq & 1) && this->ack_count_ < MAX_ACKS) {
          this->acks_[this->ack_count_++] = inner_id;
          if (this->ack_deadline_ == 0)
            this->ack_deadline_ = millis() + ACK_DELAY_MS;
        }
        this->handle_object_(TlReader(reader.pos(), bytes), inner_id, depth + 1);
        reader.skip(bytes);
      }
      return;
    }
    case ID_RPC_RESULT: {
      uint64_t req_msg_id = 0;
      reader.skip(4);
      if (!reader.u64(req_msg_id))
        return;
      uint32_t inner = 0;
      if (reader.peek_u32(inner) && inner == ID_GZIP_PACKED) {
        reader.skip(4);
        const uint8_t *packed = nullptr;
        size_t packed_len = 0;
        uint8_t *plain = nullptr;
        size_t plain_len = 0;
        if (reader.bytes(packed, packed_len) && this->inflate_(packed, packed_len, plain, plain_len)) {
          this->handle_result_(req_msg_id, TlReader(plain, plain_len));
          heap_caps_free(plain);
        } else {
          Pending *pending = this->take_pending_(req_msg_id);
          if (pending != nullptr) {
            const Request request = pending->request;
            heap_caps_free(pending->body);
            *pending = Pending{};
            this->handle_error_(request, 500, "GZIP_FAILED", 11);
          }
        }
        return;
      }
      this->handle_result_(req_msg_id, reader);
      return;
    }
    case ID_GZIP_PACKED: {
      reader.skip(4);
      const uint8_t *packed = nullptr;
      size_t packed_len = 0;
      uint8_t *plain = nullptr;
      size_t plain_len = 0;
      if (reader.bytes(packed, packed_len) && this->inflate_(packed, packed_len, plain, plain_len)) {
        this->handle_object_(TlReader(plain, plain_len), msg_id, depth + 1);
        heap_caps_free(plain);
      }
      return;
    }
    case ID_BAD_SERVER_SALT: {
      uint64_t bad_msg_id = 0;
      uint32_t seq = 0;
      int32_t code = 0;
      uint64_t salt = 0;
      reader.skip(4);
      if (reader.u64(bad_msg_id) && reader.u32(seq) && reader.i32(code) && reader.u64(salt)) {
        this->server_salt_ = salt;
        this->resend_(bad_msg_id);
      }
      return;
    }
    case ID_BAD_MSG_NOTIFICATION: {
      uint64_t bad_msg_id = 0;
      uint32_t seq = 0;
      int32_t code = 0;
      reader.skip(4);
      if (!reader.u64(bad_msg_id) || !reader.u32(seq) || !reader.i32(code))
        return;
      if (code == 16 || code == 17) {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        this->time_offset_ = static_cast<int32_t>(static_cast<int64_t>(msg_id >> 32) - tv.tv_sec);
        this->last_msg_id_ = 0;
        this->resend_(bad_msg_id);
      } else if (code == 32 || code == 33) {
        this->seq_count_ += 16;
        this->resend_(bad_msg_id);
      } else {
        ESP_LOGW(TAG, "Message refused, code %" PRId32, code);
      }
      return;
    }
    case ID_NEW_SESSION_CREATED: {
      uint64_t first = 0;
      uint64_t unique = 0;
      uint64_t salt = 0;
      reader.skip(4);
      if (reader.u64(first) && reader.u64(unique) && reader.u64(salt))
        this->server_salt_ = salt;
      return;
    }
    case ID_UPDATES_TOO_LONG:
    case ID_UPDATE_SHORT:
    case ID_UPDATES_COMBINED:
    case ID_UPDATES:
      this->handle_updates_(reader);
      return;
    default:
      return;
  }
}

void Mtproto::handle_updates_(TlReader updates) {
  uint32_t id = 0;
  if (!updates.peek_u32(id))
    return;
  if (id == ID_UPDATE_SHORT) {
    TlReader update;
    uint32_t update_id = 0;
    if (tl_field(updates, tl_hash("update"), update) && update.peek_u32(update_id) &&
        update_id == ID_UPDATE_LOGIN_TOKEN && this->user_id_ == 0)
      this->request_login_token_();
    return;
  }
  if (id == ID_UPDATES || id == ID_UPDATES_COMBINED) {
    TlReader list;
    uint32_t vector_id = 0;
    uint32_t count = 0;
    if (!tl_field(updates, tl_hash("updates"), list) || !list.u32(vector_id) || !list.u32(count))
      return;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t update_id = 0;
      if (!list.peek_u32(update_id))
        return;
      if (update_id == ID_UPDATE_LOGIN_TOKEN && this->user_id_ == 0)
        this->request_login_token_();
      if (!tl_skip_object(list))
        return;
    }
  }
}

Mtproto::Pending *Mtproto::take_pending_(uint64_t msg_id) {
  for (Pending &pending : this->pending_) {
    if (pending.request != Request::NONE && pending.msg_id == msg_id)
      return &pending;
  }
  return nullptr;
}

void Mtproto::drop_pending_() {
  for (Pending &pending : this->pending_) {
    heap_caps_free(pending.body);
    pending = Pending{};
  }
}

void Mtproto::handle_result_(uint64_t req_msg_id, TlReader result) {
  Pending *pending = this->take_pending_(req_msg_id);
  if (pending == nullptr)
    return;
  const Request request = pending->request;
  heap_caps_free(pending->body);
  *pending = Pending{};
  uint32_t id = 0;
  if (!result.peek_u32(id))
    return;
  if (id == ID_RPC_ERROR) {
    int32_t code = 0;
    const uint8_t *message = nullptr;
    size_t message_len = 0;
    result.skip(4);
    if (result.i32(code) && result.bytes(message, message_len))
      this->handle_error_(request, code, reinterpret_cast<const char *>(message), message_len);
    return;
  }
  switch (request) {
    case Request::GET_SELF: {
      uint32_t vector_id = 0;
      uint32_t count = 0;
      if (result.u32(vector_id) && result.u32(count) && count > 0) {
        this->store_user_(result);
        this->backoff_ms_ = 0;
        this->set_state_(State::LOGGED_IN);
      }
      break;
    }
    case Request::EXPORT_TOKEN:
    case Request::IMPORT_TOKEN: {
      if (id == ID_AUTH_LOGIN_TOKEN) {
        int32_t expires = 0;
        const uint8_t *token = nullptr;
        size_t token_len = 0;
        result.skip(4);
        if (!result.i32(expires) || !result.bytes(token, token_len))
          break;
        char url[160];
        memcpy(url, "tg://login?token=", 17);
        base64url(token, token_len, url + 17, sizeof(url) - 17);
        {
          LockGuard guard(this->lock_);
          copy_text(this->qr_url_, sizeof(this->qr_url_), url, strlen(url));
          this->error_[0] = '\0';
        }
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        int32_t valid = expires - (static_cast<int32_t>(tv.tv_sec) + this->time_offset_);
        if (valid < 5 || valid > 120)
          valid = 30;
        this->token_deadline_ = millis() + static_cast<uint32_t>(valid) * 1000;
        this->backoff_ms_ = 0;
        this->set_state_(State::LOGIN_QR);
      } else if (id == ID_AUTH_LOGIN_TOKEN_MIGRATE_TO) {
        int32_t dc = 0;
        const uint8_t *token = nullptr;
        size_t token_len = 0;
        result.skip(4);
        if (!result.i32(dc) || !result.bytes(token, token_len) || token_len > sizeof(this->migrate_token_))
          break;
        memcpy(this->migrate_token_, token, token_len);
        this->migrate_token_len_ = token_len;
        this->switch_dc_(dc);
      } else if (id == ID_AUTH_LOGIN_TOKEN_SUCCESS) {
        TlReader authorization;
        if (tl_field(result, tl_hash("authorization"), authorization))
          this->login_done_(authorization);
      }
      break;
    }
    case Request::GET_PASSWORD: {
      if (id != ID_ACCOUNT_PASSWORD)
        break;
      Password &pw = this->password_;
      pw = Password{};
      TlReader algo;
      uint32_t algo_id = 0;
      if (!tl_field(result, tl_hash("current_algo"), algo) || !algo.peek_u32(algo_id) ||
          algo_id != ID_PASSWORD_KDF_ALGO_MODPOW) {
        this->fail_("Unsupported password algorithm");
        this->set_state_(State::ERROR);
        break;
      }
      const uint8_t *data = nullptr;
      size_t length = 0;
      int32_t g = 0;
      TlReader g_field;
      if (!read_bytes_field(algo, "salt1", data, length) || length > sizeof(pw.salt1))
        break;
      memcpy(pw.salt1, data, length);
      pw.salt1_len = length;
      if (!read_bytes_field(algo, "salt2", data, length) || length > sizeof(pw.salt2))
        break;
      memcpy(pw.salt2, data, length);
      pw.salt2_len = length;
      if (!tl_field(algo, tl_hash("g"), g_field) || !g_field.i32(g))
        break;
      pw.g = static_cast<uint32_t>(g);
      if (!read_bytes_field(algo, "p", data, length) || length != sizeof(pw.p))
        break;
      memcpy(pw.p, data, length);
      if (!read_bytes_field(result, "srp_B", data, length) || length > sizeof(pw.srp_b))
        break;
      memcpy(pw.srp_b, data, length);
      pw.srp_b_len = length;
      TlReader srp_id;
      uint64_t srp_value = 0;
      if (!tl_field(result, tl_hash("srp_id"), srp_id) || !srp_id.u64(srp_value))
        break;
      pw.srp_id = srp_value;
      pw.valid = true;
      {
        LockGuard guard(this->lock_);
        read_text_field(result, "hint", this->hint_, sizeof(this->hint_));
      }
      this->set_state_(State::PASSWORD);
      break;
    }
    case Request::CHECK_PASSWORD:
      if (id == ID_AUTH_AUTHORIZATION)
        this->login_done_(result);
      break;
    default:
      break;
  }
}

void Mtproto::handle_error_(Request request, int32_t code, const char *message, size_t length) {
  char text[96];
  copy_text(text, sizeof(text), message, length);
  ESP_LOGW(TAG, "RPC error %" PRId32 " %s", code, text);
  if (code == 303) {
    const char *underscore = strrchr(text, '_');
    if (underscore != nullptr) {
      const int dc = atoi(underscore + 1);
      if (dc_ip(dc) != nullptr) {
        this->switch_dc_(dc);
        return;
      }
    }
  }
  if (strcmp(text, "SESSION_PASSWORD_NEEDED") == 0) {
    this->request_password_();
    return;
  }
  if (strcmp(text, "PASSWORD_HASH_INVALID") == 0) {
    this->fail_("Wrong password");
    this->set_state_(State::PASSWORD);
    return;
  }
  if (strncmp(text, "FLOOD_WAIT_", 11) == 0) {
    const int seconds = atoi(text + 11);
    this->fail_("Telegram asks to wait before trying again");
    this->token_deadline_ = millis() + static_cast<uint32_t>(seconds > 0 ? seconds : 30) * 1000;
    return;
  }
  if (request == Request::GET_SELF &&
      (code == 401 || strcmp(text, "AUTH_KEY_UNREGISTERED") == 0 || strcmp(text, "SESSION_REVOKED") == 0)) {
    this->user_id_ = 0;
    {
      LockGuard guard(this->lock_);
      this->user_name_[0] = '\0';
      this->session_.user_id = 0;
      this->session_.name[0] = '\0';
    }
    this->post_event_(EventType::SAVE_SESSION);
    this->request_login_token_();
    return;
  }
  if (request == Request::EXPORT_TOKEN || request == Request::IMPORT_TOKEN) {
    if (strncmp(text, "AUTH_TOKEN_", 11) == 0) {
      this->request_login_token_();
      return;
    }
    if (strncmp(text, "API_ID_", 7) == 0) {
      this->api_id_ = 0;
      {
        LockGuard guard(this->lock_);
        this->session_.api_id = 0;
        this->session_.api_hash[0] = '\0';
      }
      this->post_event_(EventType::SAVE_SESSION);
      this->fail_("Telegram refused the API ID / hash");
      this->set_state_(State::NO_API);
      return;
    }
    this->fail_(text);
    this->token_deadline_ = millis() + RETRY_MAX_MS / 6;
    this->set_state_(State::ERROR);
    return;
  }
  if (request == Request::CHECK_PASSWORD) {
    this->fail_(text);
    this->set_state_(State::PASSWORD);
    return;
  }
  this->fail_(text);
}

void Mtproto::request_self_() {
  TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
  w.u32(ID_USERS_GET_USERS);
  w.vector(1);
  w.u32(ID_INPUT_USER_SELF);
  this->send_request_(Request::GET_SELF, w.data(), w.size());
}

void Mtproto::request_login_token_() {
  TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
  w.u32(ID_AUTH_EXPORT_LOGIN_TOKEN);
  w.i32(this->api_id_);
  w.string(this->api_hash_);
  w.vector(0);
  this->send_request_(Request::EXPORT_TOKEN, w.data(), w.size());
}

void Mtproto::request_import_(const uint8_t *token, size_t length) {
  TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
  w.u32(ID_AUTH_IMPORT_LOGIN_TOKEN);
  w.bytes(token, length);
  this->send_request_(Request::IMPORT_TOKEN, w.data(), w.size());
}

void Mtproto::request_password_() {
  this->token_deadline_ = 0;
  TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
  w.u32(ID_ACCOUNT_GET_PASSWORD);
  this->send_request_(Request::GET_PASSWORD, w.data(), w.size());
}

void Mtproto::submit_password_(const char *password) {
  if (!this->password_.valid) {
    this->request_password_();
    return;
  }
  const Password &pw = this->password_;
  SrpInput input{pw.g, pw.p, sizeof(pw.p), pw.salt1, pw.salt1_len, pw.salt2, pw.salt2_len, pw.srp_b, pw.srp_b_len,
                 password};
  uint8_t a[DH_BYTES];
  uint8_t m1[32];
  if (!srp_check(input, a, m1)) {
    this->fail_("Password check could not be computed");
    return;
  }
  this->password_.valid = false;
  TlWriter w(this->tx_ + SCRATCH_OFFSET, BUFFER_SIZE - SCRATCH_OFFSET);
  w.u32(ID_AUTH_CHECK_PASSWORD);
  w.u32(ID_INPUT_CHECK_PASSWORD_SRP);
  w.u64(pw.srp_id);
  w.bytes(a, sizeof(a));
  w.bytes(m1, sizeof(m1));
  this->send_request_(Request::CHECK_PASSWORD, w.data(), w.size());
}

void Mtproto::store_user_(TlReader user) {
  uint32_t id = 0;
  if (!user.peek_u32(id) || id != ID_USER)
    return;
  TlReader id_field;
  int64_t user_id = 0;
  if (!tl_field(user, tl_hash("id"), id_field) || !id_field.i64(user_id))
    return;
  char first[48];
  char last[48];
  read_text_field(user, "first_name", first, sizeof(first));
  read_text_field(user, "last_name", last, sizeof(last));
  char name[100];
  snprintf(name, sizeof(name), "%s%s%s", first, last[0] != '\0' ? " " : "", last);
  this->user_id_ = user_id;
  LockGuard guard(this->lock_);
  copy_text(this->user_name_, sizeof(this->user_name_), name, strlen(name));
  copy_text(this->session_.name, sizeof(this->session_.name), name, strlen(name));
}

void Mtproto::login_done_(TlReader authorization) {
  uint32_t id = 0;
  if (!authorization.peek_u32(id) || id != ID_AUTH_AUTHORIZATION) {
    this->fail_("This phone number has no Telegram account");
    this->set_state_(State::ERROR);
    return;
  }
  TlReader user;
  if (!tl_field(authorization, tl_hash("user"), user))
    return;
  this->store_user_(user);
  if (this->user_id_ == 0)
    return;
  this->home_dc_ = this->dc_;
  this->home_key_ = true;
  this->token_deadline_ = 0;
  {
    LockGuard guard(this->lock_);
    this->session_.version = SESSION_VERSION;
    this->session_.dc = this->dc_;
    this->session_.user_id = this->user_id_;
    this->session_.api_id = this->api_id_;
    copy_text(this->session_.api_hash, sizeof(this->session_.api_hash), this->api_hash_, strlen(this->api_hash_));
    memcpy(this->session_.auth_key, this->auth_key_, sizeof(this->auth_key_));
    this->qr_url_[0] = '\0';
    this->hint_[0] = '\0';
    this->error_[0] = '\0';
  }
  this->post_event_(EventType::SAVE_SESSION);
  this->set_state_(State::LOGGED_IN);
  ESP_LOGI(TAG, "Logged in on DC%d", this->dc_);
}

void Mtproto::switch_dc_(int dc) {
  if (dc == this->dc_)
    return;
  this->target_dc_ = dc;
  this->has_key_ = false;
  if (dc == this->home_dc_ && this->session_.user_id != 0) {
    memcpy(this->auth_key_, this->session_.auth_key, sizeof(this->auth_key_));
    this->has_key_ = true;
  }
}

}
