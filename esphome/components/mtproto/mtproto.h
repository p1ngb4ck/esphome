#pragma once

#include "tl.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/static_task.h"

#ifdef USE_NETWORK
#include "esphome/components/network/network_component.h"
#endif
#ifdef USE_API
#include "esphome/components/api/custom_api_device.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>
#include <string>

namespace esphome::mtproto {

enum class State : uint8_t { IDLE = 0, NO_API, CONNECTING, LOGIN_QR, PASSWORD, LOGGED_IN, ERROR };

const char *state_name(State state);

class Mtproto final : public Component
#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
    ,
                      public network::ActiveInterfaceListener
#endif
#ifdef USE_API
    ,
                      public api::CustomAPIDevice
#endif
{
 public:
  static constexpr size_t BUFFER_SIZE = 64 * 1024;
  static constexpr size_t MAX_PACKET = 2 * 1024 * 1024;
  static constexpr size_t MAX_PENDING = 8;
  static constexpr size_t MAX_ACKS = 16;
  static constexpr uint32_t TASK_STACK = 12 * 1024;
  static constexpr UBaseType_t TASK_PRIORITY = 1;

  static Mtproto *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_default_api(int32_t api_id, const char *api_hash);
  void set_client_info(const char *device_model, const char *app_version, const char *lang_code);
  void enable() { this->enabled_ = true; }
#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
  void on_active_interface(network::NetworkInterface interface) override;
#endif

  void set_api(int32_t api_id, std::string api_hash);
  void check_password(std::string password);
  void logout();

  State state() const { return this->state_; }
  bool logged_in() const { return this->state_ == State::LOGGED_IN; }
  bool has_api() const { return this->api_id_ != 0; }
  int64_t user_id() const { return this->user_id_; }
  std::string qr_url() const;
  std::string user_name() const;
  std::string password_hint() const;
  std::string error() const;

  void add_on_state_callback(std::function<void(State)> &&callback) {
    this->state_callbacks_.add(std::move(callback));
  }

  using RpcCallback = std::function<void(const char *error, TlReader result)>;
  bool invoke(const uint8_t *query, size_t length, RpcCallback &&callback);
  bool invoke_dc(int dc, const uint8_t *query, size_t length, RpcCallback &&callback);
  void close_dc();
  int home_dc() const { return this->home_dc_; }
  void add_update_listener(std::function<void(TlReader updates)> &&listener) {
    this->update_listeners_.add(std::move(listener));
  }
  Trigger<std::string> *get_state_trigger() { return &this->state_trigger_; }

 protected:
  enum class CommandType : uint8_t { WAKE, SET_API, PASSWORD, LOGOUT, INVOKE, CLOSE_DC };
  enum class EventType : uint8_t { STATE, SAVE_SESSION };
  enum class Request : uint8_t {
    NONE = 0,
    GET_SELF,
    EXPORT_TOKEN,
    IMPORT_TOKEN,
    GET_PASSWORD,
    CHECK_PASSWORD,
    LOGOUT,
    GET_STATE,
    USER,
    EXPORT_AUTH,
    IMPORT_AUTH,
  };

  struct Job {
    uint8_t *body;
    size_t length;
    RpcCallback callback;
    int dc;
  };

  struct Command {
    CommandType type;
    int32_t api_id;
    Job *job;
    char text[136];
  };

  struct Event {
    EventType type;
    State state;
  };

  struct SessionPref {
    uint32_t version;
    int32_t dc;
    int64_t user_id;
    int32_t api_id;
    char api_hash[33];
    char name[64];
    uint8_t auth_key[256];
  };

  struct Pending {
    uint64_t msg_id;
    Request request;
    uint8_t *body;
    uint32_t length;
    Job *job;
  };

  struct Link {
    int fd;
    int dc;
    uint8_t auth_key[256];
    uint8_t auth_key_id[8];
    bool has_key;
    bool home_key;
    bool init_sent;
    bool authorized;
    uint64_t server_salt;
    uint64_t session_id;
    uint32_t seq_count;
    Pending pending[MAX_PENDING];
    uint64_t acks[MAX_ACKS];
    size_t ack_count;
    uint32_t ack_deadline;
    uint32_t next_ping;
  };

  struct Password {
    bool valid;
    uint32_t g;
    uint64_t srp_id;
    uint8_t p[256];
    uint8_t salt1[64];
    size_t salt1_len;
    uint8_t salt2[64];
    size_t salt2_len;
    uint8_t srp_b[256];
    size_t srp_b_len;
  };

  Mtproto() = default;

  static void task_entry_(void *param);
  void run_();
  void post_command_(CommandType type, int32_t api_id, const char *text);
  void post_event_(EventType type);
  void set_state_(State state);
  void wake_();

  bool open_buffers_();
  void close_buffers_();
  bool connect_(int dc);
  void disconnect_();
  bool send_packet_(const uint8_t *data, size_t length);
  int read_packet_(size_t &length, uint32_t timeout_ms);
  bool read_exact_(uint8_t *out, size_t length, uint32_t timeout_ms);

  bool handshake_();
  bool plain_call_(const uint8_t *body, size_t length, const uint8_t *&reply, size_t &reply_len);

  uint64_t next_msg_id_();
  bool send_message_(const uint8_t *body, size_t length, bool content, uint64_t &msg_id);
  bool send_request_(Request request, const uint8_t *body, size_t length, Job *job = nullptr);
  void finish_job_(Job *job, const char *error, TlReader result);
  void request_state_();
  bool resend_(uint64_t bad_msg_id);
  bool send_acks_();
  bool send_ping_();
  bool receive_(uint32_t timeout_ms);
  void handle_object_(TlReader reader, uint64_t msg_id, int depth);
  void handle_result_(uint64_t req_msg_id, TlReader result);
  void handle_error_(Request request, int32_t code, const char *message, size_t length);
  void handle_updates_(TlReader updates);
  Pending *take_pending_(uint64_t msg_id);
  void drop_pending_();
  bool has_pending_(Job *job) const;

  void invoke_on_link_(Job *job);
  bool open_link_(int dc);
  void close_link_();
  void swap_link_();
  bool pump_(const std::function<bool()> &done, uint32_t timeout_ms);

  void handle_command_(const Command &command);
  void start_session_();
  void request_self_();
  void request_login_token_();
  void request_import_(const uint8_t *token, size_t length);
  void request_password_();
  void submit_password_(const char *password);
  void login_done_(TlReader authorization);
  void store_user_(TlReader user);
  void switch_dc_(int dc);
  void fail_(const char *text);

  bool inflate_(const uint8_t *data, size_t length, uint8_t *&out, size_t &out_len);
  void encode_wrapped_(TlWriter &writer, const uint8_t *query, size_t length);

  std::atomic<State> state_{State::IDLE};
  bool enabled_{false};
  bool started_{false};
  CallbackManager<void(State)> state_callbacks_;
  CallbackManager<void(TlReader)> update_listeners_;
  Trigger<std::string> state_trigger_;
  ESPPreferenceObject pref_;

  int32_t api_id_{0};
  char api_hash_[33]{};
  const char *device_model_{"ESPHome"};
  const char *app_version_{"1.0"};
  const char *lang_code_{"en"};

  StaticTask task_;
  QueueHandle_t commands_{nullptr};
  QueueHandle_t events_{nullptr};
  int wake_fd_{-1};
  std::atomic<bool> network_up_{true};

  mutable Mutex lock_;
  char qr_url_[160]{};
  char user_name_[64]{};
  char hint_[64]{};
  char error_[96]{};
  SessionPref session_{};

  int fd_{-1};
  int dc_{2};
  int home_dc_{2};
  int64_t user_id_{0};
  uint8_t auth_key_[256]{};
  uint8_t auth_key_id_[8]{};
  bool has_key_{false};
  bool home_key_{false};
  uint64_t server_salt_{0};
  uint64_t session_id_{0};
  uint32_t seq_count_{0};
  uint64_t last_msg_id_{0};
  int32_t time_offset_{0};
  bool init_sent_{false};
  uint8_t *rx_{nullptr};
  size_t rx_size_{0};
  uint8_t *tx_{nullptr};
  Pending pending_[MAX_PENDING]{};
  uint64_t acks_[MAX_ACKS]{};
  size_t ack_count_{0};
  uint32_t next_ping_{0};
  uint32_t ack_deadline_{0};
  uint32_t token_deadline_{0};
  uint32_t retry_delay_{0};
  uint32_t backoff_ms_{0};
  int target_dc_{0};
  uint8_t migrate_token_[256]{};
  size_t migrate_token_len_{0};
  Password password_{};
  bool logging_out_{false};
  Link *link_{nullptr};
  int8_t link_result_{0};
  int64_t export_id_{0};
  uint8_t *export_bytes_{nullptr};
  size_t export_len_{0};
};

template<typename... Ts> class LogoutAction : public Action<Ts...>, public Parented<Mtproto> {
 public:
  void play(const Ts &...x) override { this->parent_->logout(); }
};

template<typename... Ts> class CheckPasswordAction : public Action<Ts...>, public Parented<Mtproto> {
 public:
  TEMPLATABLE_VALUE(std::string, password)
  void play(const Ts &...x) override { this->parent_->check_password(this->password_.value(x...)); }
};

template<typename... Ts> class SetApiAction : public Action<Ts...>, public Parented<Mtproto> {
 public:
  TEMPLATABLE_VALUE(int32_t, api_id)
  TEMPLATABLE_VALUE(std::string, api_hash)
  void play(const Ts &...x) override { this->parent_->set_api(this->api_id_.value(x...), this->api_hash_.value(x...)); }
};

}
