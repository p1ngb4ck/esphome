#pragma once

#ifdef USE_ESP32

#include "media.h"
#include "sdp.h"
#include "sip_message.h"

#include "esphome/components/resampler/microphone/resampler_microphone.h"
#include "esphome/components/speaker/speaker.h"
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
#ifdef USE_JSON
#include "esphome/components/json/json_util.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_EVENT
#include "esphome/components/event/event.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lwip/sockets.h>

#include <atomic>
#include <string>
#include <vector>

namespace esphome::sip_phone {

enum class PhoneState : uint8_t { UNREGISTERED = 0, REGISTERING, IDLE, RINGING, CALLING, IN_CALL };

const char *phone_state_name(PhoneState state);

struct Account {
  char server[64];
  char username[64];
  char auth_username[64];
  char password[64];
  char domain[64];
  char display_name[48];
  uint16_t port;
  uint32_t expiry;
  uint8_t saved;
};

struct Contact {
  char name[32];
  char uri[96];
};

struct CallRecord {
  char call_id[64];
  char peer_uri[96];
  char peer_name[48];
  char reason[24];
  char codec[8];
  bool outgoing;
  bool answered;
  uint32_t started;
  uint32_t duration_s;
};

class SipPhone final : public Component,
                       public MediaListener
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
  static constexpr uint8_t MAX_CODECS = 4;
  static constexpr uint8_t MAX_CONTACTS = 24;
  static constexpr uint8_t MAX_RECENT = 10;
  static constexpr size_t RX_BUFFER = 2048;
  static constexpr size_t TX_BUFFER = 1536;
  static constexpr uint8_t TRANSACTIONS = 4;
  static constexpr uint8_t MAX_ROUTES = 3;
  static constexpr uint8_t MAX_VIAS = 4;

  static SipPhone *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_microphone(resampler::ResamplerMicrophone *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }
  void set_sip_port(uint16_t port) { this->sip_port_ = port; }
  void set_rtp_port(uint16_t port) { this->rtp_port_ = port; }
  void set_default_account(const char *server, uint16_t port, const char *username, const char *auth_username,
                           const char *password, const char *domain, const char *display_name, uint32_t expiry);
  void add_codec(Codec codec);
  void set_opus(uint32_t bitrate, uint8_t complexity) {
    this->opus_bitrate_ = bitrate;
    this->opus_complexity_ = complexity;
  }
  void set_ptime(uint16_t ptime) { this->ptime_ = ptime; }
  void set_playout_delay(uint16_t ms) { this->playout_ms_ = ms; }
  void set_ring_timeout(uint32_t ms) { this->ring_timeout_ms_ = ms; }
  void set_task_stack_in_psram(bool psram) { this->task_stack_in_psram_ = psram; }
  void set_identity(const char *name) { this->identity_ = name; }
  void add_config_contact(const char *name, const char *uri);
#ifdef USE_TEXT_SENSOR
  void set_state_text_sensor(text_sensor::TextSensor *sensor) { this->state_sensor_ = sensor; }
  void set_peer_text_sensor(text_sensor::TextSensor *sensor) { this->peer_sensor_ = sensor; }
  void set_message_text_sensor(text_sensor::TextSensor *sensor) { this->message_sensor_ = sensor; }
#endif
#ifdef USE_EVENT
  void set_call_event(event::Event *event) { this->call_event_ = event; }
#endif

  void call(std::string destination);
  void answer();
  void hangup();
  void decline();
  void send_dtmf(std::string digits);
  void send_message(std::string destination, std::string text);
  void set_mute(bool mute);
  void set_dnd(bool dnd);
  void set_auto_answer(bool auto_answer);
  void set_account(std::string server, std::string username, std::string password,
                   std::string auth_username, std::string domain, std::string display_name);
  void set_contacts(std::vector<std::string> names, std::vector<std::string> uris);
  void add_contact(std::string name, std::string uri);
  void remove_contact(std::string name);

  PhoneState get_state() const { return this->state_; }
  const char *get_state_name() const { return phone_state_name(this->state_); }
  bool is_registered() const { return this->registered_; }
  bool get_mute() const { return this->mute_; }
  bool get_dnd() const { return this->dnd_; }
  bool get_auto_answer() const { return this->auto_answer_; }
  const char *get_peer_uri() const { return this->current_.peer_uri; }
  const char *get_peer_name() const { return this->current_.peer_name; }
  const char *get_codec() const { return this->current_.codec; }
  uint32_t get_call_seconds() const;
  const char *get_registrar() const { return this->account_.server; }
  const Account &get_account() const { return this->account_; }
  const char *get_last_error() const { return this->last_error_; }
  const char *get_last_message() const { return this->last_message_; }
  const char *get_last_message_from() const { return this->last_message_from_; }
  size_t contact_count() const { return this->contact_count_; }
  const char *contact_name(size_t index) const { return index < this->contact_count_ ? this->contacts_[index].name : ""; }
  const char *contact_uri(size_t index) const { return index < this->contact_count_ ? this->contacts_[index].uri : ""; }
  size_t recent_count() const { return this->recent_count_; }
  const CallRecord *recent(size_t index) const;
#ifdef USE_JSON
  void status_json(JsonObject root) const;
#endif

  Trigger<> *get_registered_trigger() { return &this->registered_trigger_; }
  Trigger<std::string> *get_registration_failed_trigger() { return &this->registration_failed_trigger_; }
  Trigger<std::string, std::string> *get_incoming_call_trigger() { return &this->incoming_call_trigger_; }
  Trigger<std::string> *get_outgoing_call_trigger() { return &this->outgoing_call_trigger_; }
  Trigger<> *get_call_established_trigger() { return &this->call_established_trigger_; }
  Trigger<std::string> *get_call_ended_trigger() { return &this->call_ended_trigger_; }
  Trigger<std::string> *get_dtmf_trigger() { return &this->dtmf_trigger_; }
  Trigger<std::string, std::string> *get_message_trigger() { return &this->message_trigger_; }
  Trigger<std::string> *get_state_trigger() { return &this->state_trigger_; }

  bool start_transport_media(MediaTransport *transport, bool mute);
  void stop_transport_media();
  void transport_payload(const uint8_t *payload, size_t len) { this->media_.on_payload(payload, len); }
  void set_transport_mute(bool mute) {
    if (this->transport_media_)
      this->media_.set_mute(mute);
  }

  void on_media_dtmf(char digit) override;
  void on_media_parked() override;
#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
  void on_active_interface(network::NetworkInterface interface) override;
#endif

 protected:
  enum class EventType : uint8_t {
    REGISTERING,
    REGISTERED,
    REGISTRATION_FAILED,
    UNREGISTERED,
    INCOMING,
    OUTGOING,
    ESTABLISHED,
    MEDIA_UPDATE,
    ENDED,
    DTMF,
    MESSAGE,
    MESSAGE_SENT,
    MESSAGE_FAILED,
  };
  struct Event {
    EventType type;
    bool outgoing;
    char digit;
    char call_id[64];
    char peer_uri[96];
    char peer_name[48];
    char text[256];
    MediaConfig media;
  };
  enum class CommandType : uint8_t {
    CALL,
    ANSWER,
    HANGUP,
    DECLINE,
    DTMF_INFO,
    MESSAGE,
    ACCOUNT,
    NETWORK_UP,
    NETWORK_DOWN,
  };
  struct Command {
    CommandType type;
    char target[128];
    char text[256];
  };
  enum class TxRole : uint8_t { REGISTER = 0, INVITE, DIALOG, MESSAGE };
  struct ClientTx {
    bool active;
    bool provisional;
    bool auth_tried;
    char method[12];
    char branch[32];
    char ruri[128];
    uint32_t cseq;
    uint32_t next_send;
    uint32_t interval;
    uint32_t deadline;
    struct sockaddr_in dest;
    size_t len;
    char buf[TX_BUFFER];
  };
  struct Challenge {
    bool valid;
    bool proxy;
    char realm[64];
    char nonce[96];
    char opaque[64];
    bool qop_auth;
    uint32_t nc;
  };
  struct Dialog {
    bool active;
    bool outgoing;
    bool established;
    bool cancel_pending;
    bool ack_pending;
    bool late_offer;
    bool direct;
    char call_id[64];
    char local_hdr[224];
    char remote_hdr[224];
    char remote_target[128];
    char routes[MAX_ROUTES][128];
    uint8_t route_count;
    uint32_t local_cseq;
    uint32_t invite_cseq;
    struct sockaddr_in peer;
    int rtp_fd;
    uint16_t rtp_port;
    uint32_t sdp_session;
    uint32_t sdp_version;
    MediaFormat format;
    char vias[MAX_VIAS][160];
    uint8_t via_count;
    char cseq_header[32];
    char offer[1024];
    size_t offer_len;
    uint32_t ring_deadline;
    uint32_t ok_next_send;
    uint32_t ok_interval;
    uint32_t ok_deadline;
    size_t ok_len;
    char ok_buf[TX_BUFFER];
  };

  SipPhone() = default;

  static void sig_task_entry_(void *param);
  void sig_run_();
  void post_command_(CommandType type, const char *target, const char *text);
  void post_event_(Event &event);
  void wake_sig_();
  void handle_command_(const Command &command);
  void handle_datagram_(size_t len, const struct sockaddr_in &from);
  void handle_request_(const SipMessage &msg, const struct sockaddr_in &from);
  void handle_response_(const SipMessage &msg);
  void handle_invite_(const SipMessage &msg, const struct sockaddr_in &from);
  void handle_reinvite_(const SipMessage &msg, const struct sockaddr_in &from);
  void handle_invite_response_(ClientTx &tx, const SipMessage &msg);
  void handle_register_response_(ClientTx &tx, const SipMessage &msg);
  void service_timers_(uint32_t now);
  uint32_t next_deadline_(uint32_t now) const;

  bool account_set_() const { return this->task_account_.server[0] != '\0' && this->task_account_.username[0] != '\0'; }
  bool resolve_(const char *host, uint16_t port, struct sockaddr_in &out);
  bool local_ip_for_(const struct sockaddr_in &dest);
  void start_register_(bool unregister);
  void schedule_register_retry_(const char *reason);
  bool start_call_(const char *target);
  void answer_call_();
  void hangup_call_(bool decline);
  void end_dialog_(const char *reason, bool answered);
  void send_message_(const char *target, const char *text);
  void send_info_dtmf_(const char *digits);
  bool open_rtp_();
  void close_rtp_();
  bool fill_media_(MediaConfig &media, const char *remote_address, uint16_t remote_port);

  ClientTx &tx_(TxRole role) { return this->transactions_[static_cast<uint8_t>(role)]; }
  void begin_request_(SipWriter &w, const char *method, const char *request_uri, const char *branch);
  void write_auth_(SipWriter &w, const char *method, const char *uri);
  void send_tx_(ClientTx &tx, size_t len, bool invite);
  void send_raw_(const char *data, size_t len, const struct sockaddr_in &dest);
  void respond_(const SipMessage &msg, const struct sockaddr_in &to, int code, const char *reason,
                const char *to_tag, const char *extra);
  void respond_dialog_(int code, const char *reason, const char *body, size_t body_len, bool store_ok);
  void send_in_dialog_(const char *method, const char *extra, const char *body, size_t body_len);
  void send_ack_(const SipMessage &response, const ClientTx &tx, bool for_2xx);
  void ack_bye_stray_(const SipMessage &response, const ClientTx &tx);
  void send_invite_(ClientTx &tx);
  void send_cancel_();
  void new_branch_(char *out, size_t len);
  void new_tag_(char *out, size_t len);
  bool store_challenge_(const SipMessage &msg);
  void make_uri_(const char *target, char *out, size_t len) const;
  void our_aor_(char *out, size_t len) const;
  void dialog_dest_(struct sockaddr_in &dest) const;
  void event_from_dialog_(Event &event) const;

  void process_event_(const Event &event);
  void set_state_(PhoneState state);
  void start_media_(const MediaConfig &media);
  void stop_media_();
  void finish_call_record_(const char *reason);
  void fire_event_(const char *type);
  void save_flags_();
  void save_contacts_();
  void load_contacts_();
  void publish_peer_();

  resampler::ResamplerMicrophone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  const char *identity_{"esphome"};
  uint16_t sip_port_{5060};
  uint16_t rtp_port_{40000};
  Codec codecs_[MAX_CODECS]{};
  uint8_t codec_count_{0};
  uint32_t opus_bitrate_{64000};
  uint8_t opus_complexity_{2};
  OpusParams opus_params_;
  uint16_t ptime_{20};
  uint16_t playout_ms_{60};
  uint32_t ring_timeout_ms_{60000};
  bool task_stack_in_psram_{false};
  bool no_psram_{false};

  Account account_{};
  Account task_account_{};
  Mutex account_lock_;
  ESPPreferenceObject account_pref_;
  ESPPreferenceObject flags_pref_;
  ESPPreferenceObject contacts_pref_;

  StaticTask sig_task_;
  QueueHandle_t commands_{nullptr};
  QueueHandle_t events_{nullptr};
  int sig_fd_{-1};
  int wake_fd_{-1};
  std::atomic<bool> network_up_{false};

  char rx_buf_[RX_BUFFER + 1];
  char scratch_[TX_BUFFER];
  char auth_text_[320];
  SipMessage rx_msg_;
  ClientTx transactions_[TRANSACTIONS];
  Challenge challenge_{};
  Dialog dialog_{};
  struct sockaddr_in registrar_ {};
  bool registrar_resolved_{false};
  char local_ip_[16]{};
  char reg_call_id_[48]{};
  char reg_tag_[16]{};
  char msg_target_[128]{};
  char msg_text_[256]{};
  char msg_call_id_[48]{};
  uint32_t reg_cseq_{0};
  bool reg_registered_{false};
  bool reg_removing_{false};
  uint32_t reg_refresh_at_{0};
  uint32_t reg_retry_at_{0};
  uint32_t reg_backoff_ms_{0};

  PhoneState state_{PhoneState::UNREGISTERED};
  bool registered_{false};
  bool mute_{false};
  bool dnd_{false};
  bool auto_answer_{false};
  bool media_running_{false};
  bool transport_media_{false};
  bool dtmf_rtp_{false};
  std::atomic<bool> media_parked_{false};
  char last_error_[64]{};
  char last_message_[256]{};
  char last_message_from_[96]{};
  CallRecord current_{};
  bool call_active_{false};
  CallRecord recent_[MAX_RECENT]{};
  uint8_t recent_count_{0};
  uint8_t recent_head_{0};
  Contact contacts_[MAX_CONTACTS]{};
  uint8_t contact_count_{0};
  uint8_t config_contact_count_{0};
  CallMedia media_;
  Event pending_event_{};
  Command pending_command_{};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *state_sensor_{nullptr};
  text_sensor::TextSensor *peer_sensor_{nullptr};
  text_sensor::TextSensor *message_sensor_{nullptr};
#endif
#ifdef USE_EVENT
  event::Event *call_event_{nullptr};
#endif

  Trigger<> registered_trigger_;
  Trigger<std::string> registration_failed_trigger_;
  Trigger<std::string, std::string> incoming_call_trigger_;
  Trigger<std::string> outgoing_call_trigger_;
  Trigger<> call_established_trigger_;
  Trigger<std::string> call_ended_trigger_;
  Trigger<std::string> dtmf_trigger_;
  Trigger<std::string, std::string> message_trigger_;
  Trigger<std::string> state_trigger_;
};

}

#endif
