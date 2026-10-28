#include "sip_phone.h"

#ifdef USE_ESP32

#include "esphome/components/md5/md5.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_netif.h>
#include <esp_random.h>
#include <esp_vfs_eventfd.h>
#include <freertos/idf_additions.h>
#include <lwip/netdb.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <strings.h>
#include <unistd.h>

namespace esphome::sip_phone {

static const char *const TAG = "sip_phone";

static constexpr uint32_t SIG_TASK_STACK = 6144;
static constexpr UBaseType_t SIG_TASK_PRIORITY = 5;
static constexpr uint32_t T1_MS = 500;
static constexpr uint32_t T2_MS = 4000;
static constexpr uint32_t TIMER_B_MS = 32000;
static constexpr uint32_t RETRY_MIN_MS = 30000;
static constexpr uint32_t RETRY_MAX_MS = 600000;
static constexpr uint8_t COMMAND_QUEUE = 4;
static constexpr uint8_t EVENT_QUEUE = 8;
static constexpr const char *ALLOW = "INVITE, ACK, CANCEL, BYE, OPTIONS, MESSAGE, INFO, NOTIFY";
static constexpr const char *USER_AGENT = "ESPHome sip_phone";

struct FlagsPref {
  uint8_t dnd;
  uint8_t auto_answer;
};

struct ContactsPref {
  uint8_t count;
  Contact contacts[SipPhone::MAX_CONTACTS];
};

static bool due(uint32_t now, uint32_t at) { return at != 0 && static_cast<int32_t>(now - at) >= 0; }

static void copy_str(char *dst, size_t cap, const char *src) {
  if (cap == 0)
    return;
  if (src == nullptr)
    src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static const char *find_bytes(const char *haystack, size_t len, const char *needle) {
  const size_t needle_len = strlen(needle);
  if (needle_len == 0 || needle_len > len)
    return nullptr;
  for (size_t i = 0; i + needle_len <= len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0)
      return haystack + i;
  }
  return nullptr;
}

static void md5_hex(const char *text, char *out) {
  md5::MD5Digest digest;
  digest.init();
  digest.add(reinterpret_cast<const uint8_t *>(text), strlen(text));
  digest.calculate();
  digest.get_hex(out);
}

const char *phone_state_name(PhoneState state) {
  switch (state) {
    case PhoneState::REGISTERING:
      return "registering";
    case PhoneState::IDLE:
      return "idle";
    case PhoneState::RINGING:
      return "ringing";
    case PhoneState::CALLING:
      return "calling";
    case PhoneState::IN_CALL:
      return "in_call";
    default:
      return "unregistered";
  }
}

SipPhone *SipPhone::create() {
  void *mem = RAMAllocator<SipPhone>(RAMAllocator<SipPhone>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) SipPhone();
  auto *phone = new SipPhone();
  phone->no_psram_ = true;
  return phone;
}

void SipPhone::set_default_account(const char *server, uint16_t port, const char *username,
                                   const char *auth_username, const char *password, const char *domain,
                                   const char *display_name, uint32_t expiry) {
  copy_str(this->account_.server, sizeof(this->account_.server), server);
  copy_str(this->account_.username, sizeof(this->account_.username), username);
  copy_str(this->account_.auth_username, sizeof(this->account_.auth_username), auth_username);
  copy_str(this->account_.password, sizeof(this->account_.password), password);
  copy_str(this->account_.domain, sizeof(this->account_.domain), domain);
  copy_str(this->account_.display_name, sizeof(this->account_.display_name), display_name);
  this->account_.port = port;
  this->account_.expiry = expiry;
}

void SipPhone::add_codec(Codec codec) {
  if (this->codec_count_ < MAX_CODECS)
    this->codecs_[this->codec_count_++] = codec;
}

void SipPhone::add_config_contact(const char *name, const char *uri) {
  if (this->contact_count_ >= MAX_CONTACTS)
    return;
  Contact &contact = this->contacts_[this->contact_count_++];
  copy_str(contact.name, sizeof(contact.name), name);
  copy_str(contact.uri, sizeof(contact.uri), uri);
  this->config_contact_count_ = this->contact_count_;
}

void SipPhone::setup() {
  if (this->no_psram_) {
    ESP_LOGE(TAG, "No PSRAM for the phone");
    this->mark_failed();
    return;
  }
  if (this->account_.expiry == 0)
    this->account_.expiry = 600;
  this->account_pref_ = global_preferences->make_preference<Account>(fnv1_hash("sip_phone_account"));
  this->account_.saved = 0;
  if (this->account_pref_.load(&this->task_account_) && this->task_account_.saved == 1)
    this->account_ = this->task_account_;
  this->flags_pref_ = global_preferences->make_preference<FlagsPref>(fnv1_hash("sip_phone_flags"));
  FlagsPref flags{};
  if (this->flags_pref_.load(&flags)) {
    this->dnd_ = flags.dnd != 0;
    this->auto_answer_ = flags.auto_answer != 0;
  }
  this->contacts_pref_ = global_preferences->make_preference<ContactsPref>(fnv1_hash("sip_phone_contacts"));
  this->load_contacts_();
  this->task_account_ = this->account_;

  const audio::AudioStreamInfo mic_info = this->microphone_->get_audio_stream_info();
  const uint32_t rate = mic_info.get_sample_rate();
  if (rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000)
    this->opus_params_.sample_rate = rate;
  this->opus_params_.channels = mic_info.get_channels() > 1 ? 2 : 1;

  this->microphone_->add_data_callback(
      [this](const microphone::AudioCapture &capture) { this->media_.on_mic_data(capture.data); });

  this->commands_ = xQueueCreateWithCaps(COMMAND_QUEUE, sizeof(Command), MALLOC_CAP_SPIRAM);
  this->events_ = xQueueCreateWithCaps(EVENT_QUEUE, sizeof(Event), MALLOC_CAP_SPIRAM);
  if (this->commands_ == nullptr || this->events_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the queues");
    this->mark_failed();
    return;
  }

  this->sig_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in bind_addr {};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(this->sip_port_);
  bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (this->sig_fd_ < 0 ||
      ::bind(this->sig_fd_, reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr)) != 0) {
    ESP_LOGE(TAG, "SIP port %u: %d", this->sip_port_, errno);
    this->mark_failed();
    return;
  }
  const esp_vfs_eventfd_config_t efd_config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
  const esp_err_t err = esp_vfs_eventfd_register(&efd_config);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    this->wake_fd_ = eventfd(0, 0);
  if (this->wake_fd_ < 0) {
    ESP_LOGE(TAG, "No eventfd");
    this->mark_failed();
    return;
  }

#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
  this->network_up_.store(network::global_network_component != nullptr &&
                          network::global_network_component->get_active_interface() !=
                              network::NetworkInterface::NETWORK_INTERFACE_NONE);
#else
  this->network_up_.store(true);
#endif

  if (!this->sig_task_.create(sig_task_entry_, "sip_sig", SIG_TASK_STACK, this, SIG_TASK_PRIORITY, true,
                              LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for the signaling task");
    this->mark_failed();
    return;
  }

#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES)
  this->register_service(&SipPhone::call, "sip_phone_call", {"destination"});
  this->register_service(&SipPhone::answer, "sip_phone_answer");
  this->register_service(&SipPhone::hangup, "sip_phone_hangup");
  this->register_service(&SipPhone::decline, "sip_phone_decline");
  this->register_service(&SipPhone::send_dtmf, "sip_phone_send_dtmf", {"digits"});
  this->register_service(&SipPhone::send_message, "sip_phone_send_message", {"destination", "text"});
  this->register_service(&SipPhone::set_mute, "sip_phone_mute", {"mute"});
  this->register_service(&SipPhone::set_dnd, "sip_phone_set_dnd", {"dnd"});
  this->register_service(&SipPhone::set_auto_answer, "sip_phone_set_auto_answer", {"auto_answer"});
  this->register_service(&SipPhone::set_account, "sip_phone_set_account",
                         {"server", "username", "password", "auth_username", "domain", "display_name"});
  this->register_service(&SipPhone::set_contacts, "sip_phone_set_contacts", {"names", "uris"});
  this->register_service(&SipPhone::add_contact, "sip_phone_add_contact", {"name", "uri"});
  this->register_service(&SipPhone::remove_contact, "sip_phone_remove_contact", {"name"});
#endif

  this->set_state_(PhoneState::UNREGISTERED);
  this->publish_peer_();
  this->disable_loop();
}

void SipPhone::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SIP phone:\n"
                "  SIP port: %u, RTP port: %u\n"
                "  Registrar: %s\n"
                "  User: %s\n"
                "  Opus: %" PRIu32 " Hz, %u channel(s), %" PRIu32 " bit/s",
                this->sip_port_, this->rtp_port_, this->account_.server[0] != '\0' ? this->account_.server : "none",
                this->account_.username, this->opus_params_.sample_rate, this->opus_params_.channels,
                this->opus_bitrate_);
  for (uint8_t i = 0; i < this->codec_count_; i++)
    ESP_LOGCONFIG(TAG, "  Codec: %s", codec_name(this->codecs_[i]));
}

void SipPhone::loop() {
  while (xQueueReceive(this->events_, &this->pending_event_, 0) == pdTRUE)
    this->process_event_(this->pending_event_);
  if (this->media_parked_.load()) {
    if (this->media_.release()) {
      this->media_parked_.store(false);
      this->speaker_->stop();
    } else {
      return;
    }
  }
  this->disable_loop();
}

void SipPhone::call(std::string destination) {
  if (this->state_ != PhoneState::IDLE && this->state_ != PhoneState::UNREGISTERED &&
      this->state_ != PhoneState::REGISTERING) {
    ESP_LOGW(TAG, "Busy: no new call");
    return;
  }
  this->post_command_(CommandType::CALL, destination.c_str(), "");
}

void SipPhone::answer() {
  if (this->state_ == PhoneState::RINGING)
    this->post_command_(CommandType::ANSWER, "", "");
}

void SipPhone::hangup() {
  if (this->state_ == PhoneState::RINGING || this->state_ == PhoneState::CALLING ||
      this->state_ == PhoneState::IN_CALL)
    this->post_command_(CommandType::HANGUP, "", "");
}

void SipPhone::decline() {
  if (this->state_ == PhoneState::RINGING)
    this->post_command_(CommandType::DECLINE, "", "");
}

void SipPhone::send_dtmf(std::string digits) {
  if (this->state_ != PhoneState::IN_CALL)
    return;
  if (this->current_.codec[0] != '\0' && this->media_running_ && this->dtmf_rtp_) {
    this->media_.send_dtmf(digits.c_str());
  } else {
    this->post_command_(CommandType::DTMF_INFO, "", digits.c_str());
  }
}

void SipPhone::send_message(std::string destination, std::string text) {
  this->post_command_(CommandType::MESSAGE, destination.c_str(), text.c_str());
}

void SipPhone::set_mute(bool mute) {
  this->mute_ = mute;
  this->media_.set_mute(mute);
}

void SipPhone::set_dnd(bool dnd) {
  this->dnd_ = dnd;
  this->save_flags_();
}

void SipPhone::set_auto_answer(bool auto_answer) {
  this->auto_answer_ = auto_answer;
  this->save_flags_();
}

void SipPhone::set_account(std::string server, std::string username, std::string password,
                           std::string auth_username, std::string domain,
                           std::string display_name) {
  {
    LockGuard guard(this->account_lock_);
    copy_str(this->account_.server, sizeof(this->account_.server), server.c_str());
    copy_str(this->account_.username, sizeof(this->account_.username), username.c_str());
    if (!password.empty())
      copy_str(this->account_.password, sizeof(this->account_.password), password.c_str());
    copy_str(this->account_.auth_username, sizeof(this->account_.auth_username), auth_username.c_str());
    copy_str(this->account_.domain, sizeof(this->account_.domain), domain.c_str());
    copy_str(this->account_.display_name, sizeof(this->account_.display_name), display_name.c_str());
    const char *colon = strchr(this->account_.server, ':');
    if (colon != nullptr) {
      const int port = atoi(colon + 1);
      this->account_.port = port > 0 && port < 65536 ? static_cast<uint16_t>(port) : 5060;
      this->account_.server[colon - this->account_.server] = '\0';
    } else if (this->account_.port == 0) {
      this->account_.port = 5060;
    }
  }
  this->account_.saved = 1;
  this->account_pref_.save(&this->account_);
  this->post_command_(CommandType::ACCOUNT, "", "");
}

void SipPhone::set_contacts(std::vector<std::string> names, std::vector<std::string> uris) {
  this->contact_count_ = this->config_contact_count_;
  for (size_t i = 0; i < names.size() && i < uris.size() && this->contact_count_ < MAX_CONTACTS; i++) {
    Contact &contact = this->contacts_[this->contact_count_++];
    copy_str(contact.name, sizeof(contact.name), names[i].c_str());
    copy_str(contact.uri, sizeof(contact.uri), uris[i].c_str());
  }
  this->save_contacts_();
}

void SipPhone::add_contact(std::string name, std::string uri) {
  for (uint8_t i = this->config_contact_count_; i < this->contact_count_; i++) {
    if (strcmp(this->contacts_[i].name, name.c_str()) == 0) {
      copy_str(this->contacts_[i].uri, sizeof(this->contacts_[i].uri), uri.c_str());
      this->save_contacts_();
      return;
    }
  }
  if (this->contact_count_ >= MAX_CONTACTS)
    return;
  Contact &contact = this->contacts_[this->contact_count_++];
  copy_str(contact.name, sizeof(contact.name), name.c_str());
  copy_str(contact.uri, sizeof(contact.uri), uri.c_str());
  this->save_contacts_();
}

void SipPhone::remove_contact(std::string name) {
  for (uint8_t i = this->config_contact_count_; i < this->contact_count_; i++) {
    if (strcmp(this->contacts_[i].name, name.c_str()) != 0)
      continue;
    for (uint8_t j = i; j + 1 < this->contact_count_; j++)
      this->contacts_[j] = this->contacts_[j + 1];
    this->contact_count_--;
    this->save_contacts_();
    return;
  }
}

void SipPhone::load_contacts_() {
  RAMAllocator<ContactsPref> allocator(RAMAllocator<ContactsPref>::ALLOC_EXTERNAL);
  ContactsPref *pref = allocator.allocate(1);
  if (pref == nullptr)
    return;
  memset(pref, 0, sizeof(ContactsPref));
  if (this->contacts_pref_.load(pref)) {
    for (uint8_t i = 0; i < pref->count && i < MAX_CONTACTS && this->contact_count_ < MAX_CONTACTS; i++)
      this->contacts_[this->contact_count_++] = pref->contacts[i];
  }
  allocator.deallocate(pref, 1);
}

void SipPhone::save_contacts_() {
  RAMAllocator<ContactsPref> allocator(RAMAllocator<ContactsPref>::ALLOC_EXTERNAL);
  ContactsPref *pref = allocator.allocate(1);
  if (pref == nullptr)
    return;
  memset(pref, 0, sizeof(ContactsPref));
  for (uint8_t i = this->config_contact_count_; i < this->contact_count_; i++)
    pref->contacts[pref->count++] = this->contacts_[i];
  this->contacts_pref_.save(pref);
  allocator.deallocate(pref, 1);
}

void SipPhone::save_flags_() {
  FlagsPref flags{};
  flags.dnd = this->dnd_ ? 1 : 0;
  flags.auto_answer = this->auto_answer_ ? 1 : 0;
  this->flags_pref_.save(&flags);
}

uint32_t SipPhone::get_call_seconds() const {
  if (this->state_ != PhoneState::IN_CALL)
    return 0;
  return (millis() - this->current_.started) / 1000;
}

const CallRecord *SipPhone::recent(size_t index) const {
  if (index >= this->recent_count_)
    return nullptr;
  const size_t slot = (this->recent_head_ + MAX_RECENT - 1 - index) % MAX_RECENT;
  return &this->recent_[slot];
}

#ifdef USE_JSON
void SipPhone::status_json(JsonObject root) const {
  root["state"] = phone_state_name(this->state_);
  root["registered"] = this->registered_;
  root["registrar"] = this->account_.server;
  root["username"] = this->account_.username;
  root["last_error"] = this->last_error_;
  root["dnd"] = this->dnd_;
  root["auto_answer"] = this->auto_answer_;
  root["mute"] = this->mute_;
  if (this->call_active_) {
    JsonObject call = root["call"].to<JsonObject>();
    call["call_id"] = this->current_.call_id;
    call["direction"] = this->current_.outgoing ? "outgoing" : "incoming";
    call["peer_uri"] = this->current_.peer_uri;
    call["peer_name"] = this->current_.peer_name;
    call["codec"] = this->current_.codec;
    call["duration"] = this->get_call_seconds();
  }
  if (this->recent_count_ > 0) {
    const CallRecord *last = this->recent(0);
    JsonObject record = root["last_call"].to<JsonObject>();
    record["call_id"] = last->call_id;
    record["direction"] = last->outgoing ? "outgoing" : "incoming";
    record["peer_uri"] = last->peer_uri;
    record["peer_name"] = last->peer_name;
    record["codec"] = last->codec;
    record["answered"] = last->answered;
    record["duration"] = last->duration_s;
    record["reason"] = last->reason;
  }
  root["last_message_from"] = this->last_message_from_;
  root["last_message"] = this->last_message_;
}
#endif

void SipPhone::on_media_dtmf(char digit) {
  Event event{};
  event.type = EventType::DTMF;
  event.digit = digit;
  this->post_event_(event);
}

void SipPhone::on_media_parked() {
  this->media_parked_.store(true);
  this->enable_loop_soon_any_context();
}

#if defined(USE_NETWORK) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
void SipPhone::on_active_interface(network::NetworkInterface interface) {
  if (this->is_failed())
    return;
  this->post_command_(interface == network::NetworkInterface::NETWORK_INTERFACE_NONE ? CommandType::NETWORK_DOWN
                                                                                      : CommandType::NETWORK_UP,
                      "", "");
}
#endif

void SipPhone::post_command_(CommandType type, const char *target, const char *text) {
  if (this->commands_ == nullptr)
    return;
  this->pending_command_.type = type;
  copy_str(this->pending_command_.target, sizeof(this->pending_command_.target), target);
  copy_str(this->pending_command_.text, sizeof(this->pending_command_.text), text);
  if (xQueueSend(this->commands_, &this->pending_command_, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Command dropped");
    return;
  }
  this->wake_sig_();
}

void SipPhone::wake_sig_() {
  const uint64_t one = 1;
  ::write(this->wake_fd_, &one, sizeof(one));
}

void SipPhone::post_event_(Event &event) {
  xQueueSend(this->events_, &event, portMAX_DELAY);
  this->enable_loop_soon_any_context();
}

void SipPhone::set_state_(PhoneState state) {
  this->state_ = state;
#ifdef USE_TEXT_SENSOR
  if (this->state_sensor_ != nullptr)
    this->state_sensor_->publish_state(phone_state_name(state));
#endif
  this->state_trigger_.trigger(std::string(phone_state_name(state)));
}

void SipPhone::publish_peer_() {
#ifdef USE_TEXT_SENSOR
  if (this->peer_sensor_ == nullptr)
    return;
  if (!this->call_active_) {
    this->peer_sensor_->publish_state("");
    return;
  }
  char peer[160];
  if (this->current_.peer_name[0] != '\0') {
    snprintf(peer, sizeof(peer), "%s <%s>", this->current_.peer_name, this->current_.peer_uri);
  } else {
    copy_str(peer, sizeof(peer), this->current_.peer_uri);
  }
  this->peer_sensor_->publish_state(peer);
#endif
}

void SipPhone::fire_event_(const char *type) {
#ifdef USE_EVENT
  if (this->call_event_ != nullptr)
    this->call_event_->trigger(type);
#endif
}

void SipPhone::process_event_(const Event &event) {
  switch (event.type) {
    case EventType::REGISTERING:
      if (this->state_ == PhoneState::UNREGISTERED)
        this->set_state_(PhoneState::REGISTERING);
      break;
    case EventType::REGISTERED:
      this->registered_ = true;
      this->last_error_[0] = '\0';
      ESP_LOGI(TAG, "Registered at %s", event.text);
      if (this->state_ == PhoneState::UNREGISTERED || this->state_ == PhoneState::REGISTERING)
        this->set_state_(PhoneState::IDLE);
      this->registered_trigger_.trigger();
      this->fire_event_("registered");
      break;
    case EventType::REGISTRATION_FAILED:
    case EventType::UNREGISTERED:
      this->registered_ = false;
      if (event.type == EventType::REGISTRATION_FAILED) {
        copy_str(this->last_error_, sizeof(this->last_error_), event.text);
        ESP_LOGW(TAG, "Registration failed: %s", event.text);
      }
      if (this->state_ == PhoneState::IDLE || this->state_ == PhoneState::REGISTERING)
        this->set_state_(PhoneState::UNREGISTERED);
      if (event.type == EventType::REGISTRATION_FAILED) {
        this->registration_failed_trigger_.trigger(std::string(event.text));
        this->fire_event_("registration_failed");
      } else {
        this->fire_event_("unregistered");
      }
      break;
    case EventType::INCOMING:
    case EventType::OUTGOING: {
      memset(&this->current_, 0, sizeof(this->current_));
      copy_str(this->current_.call_id, sizeof(this->current_.call_id), event.call_id);
      copy_str(this->current_.peer_uri, sizeof(this->current_.peer_uri), event.peer_uri);
      copy_str(this->current_.peer_name, sizeof(this->current_.peer_name), event.peer_name);
      this->current_.outgoing = event.type == EventType::OUTGOING;
      this->current_.started = millis();
      this->call_active_ = true;
      this->mute_ = false;
      this->set_state_(event.type == EventType::OUTGOING ? PhoneState::CALLING : PhoneState::RINGING);
      this->publish_peer_();
      ESP_LOGI(TAG, "%s call: %s", this->current_.outgoing ? "Outgoing" : "Incoming", event.peer_uri);
      if (event.type == EventType::OUTGOING) {
        this->outgoing_call_trigger_.trigger(std::string(event.peer_uri));
        this->fire_event_("outgoing");
      } else {
        this->incoming_call_trigger_.trigger(std::string(event.peer_uri), std::string(event.peer_name));
        this->fire_event_("incoming");
        if (this->auto_answer_)
          this->answer();
      }
      break;
    }
    case EventType::ESTABLISHED:
      this->current_.answered = true;
      this->current_.started = millis();
      copy_str(this->current_.codec, sizeof(this->current_.codec), codec_name(event.media.format.codec));
      this->dtmf_rtp_ = event.media.format.dtmf_payload_type != 0;
      this->start_media_(event.media);
      this->set_state_(PhoneState::IN_CALL);
      ESP_LOGI(TAG, "Call established (%s)", this->current_.codec);
      this->call_established_trigger_.trigger();
      this->fire_event_("answered");
      break;
    case EventType::MEDIA_UPDATE:
      if (this->media_running_)
        this->media_.update_remote(event.media.remote, event.media.send, event.media.receive);
      break;
    case EventType::ENDED: {
      this->stop_media_();
      if (!this->call_active_) {
        memset(&this->current_, 0, sizeof(this->current_));
        copy_str(this->current_.call_id, sizeof(this->current_.call_id), event.call_id);
        copy_str(this->current_.peer_uri, sizeof(this->current_.peer_uri), event.peer_uri);
        copy_str(this->current_.peer_name, sizeof(this->current_.peer_name), event.peer_name);
        this->current_.outgoing = event.outgoing;
        this->current_.started = millis();
      }
      this->finish_call_record_(event.text);
      this->call_active_ = false;
      this->set_state_(this->registered_ ? PhoneState::IDLE : PhoneState::UNREGISTERED);
      this->publish_peer_();
      ESP_LOGI(TAG, "Call ended: %s", event.text);
      this->call_ended_trigger_.trigger(std::string(event.text));
      const char *reason = event.text;
      if (strcmp(reason, "missed") == 0 || strcmp(reason, "dnd") == 0) {
        this->fire_event_("missed");
      } else if (strcmp(reason, "declined") == 0) {
        this->fire_event_("rejected");
      } else if (strcmp(reason, "hangup") == 0 || strcmp(reason, "remote_hangup") == 0 ||
                 strcmp(reason, "cancelled") == 0) {
        this->fire_event_("ended");
      } else {
        this->fire_event_("failed");
      }
      break;
    }
    case EventType::DTMF:
      this->dtmf_trigger_.trigger(std::string(1, event.digit));
      this->fire_event_("dtmf");
      break;
    case EventType::MESSAGE:
      copy_str(this->last_message_, sizeof(this->last_message_), event.text);
      copy_str(this->last_message_from_, sizeof(this->last_message_from_), event.peer_uri);
#ifdef USE_TEXT_SENSOR
      if (this->message_sensor_ != nullptr)
        this->message_sensor_->publish_state(event.text);
#endif
      this->message_trigger_.trigger(std::string(event.peer_uri), std::string(event.text));
      this->fire_event_("message");
      break;
    case EventType::MESSAGE_SENT:
      this->fire_event_("message_sent");
      break;
    case EventType::MESSAGE_FAILED:
      this->fire_event_("message_failed");
      break;
  }
}

void SipPhone::finish_call_record_(const char *reason) {
  copy_str(this->current_.reason, sizeof(this->current_.reason), reason);
  this->current_.duration_s = this->current_.answered ? (millis() - this->current_.started) / 1000 : 0;
  this->recent_[this->recent_head_] = this->current_;
  this->recent_head_ = static_cast<uint8_t>((this->recent_head_ + 1) % MAX_RECENT);
  if (this->recent_count_ < MAX_RECENT)
    this->recent_count_++;
}

void SipPhone::start_media_(const MediaConfig &media) {
  if (this->media_running_ || this->media_parked_.load()) {
    ESP_LOGW(TAG, "Media still busy");
    this->hangup();
    return;
  }
  this->microphone_->set_target_format(media.pcm_rate, media.pcm_channels);
  this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, media.pcm_channels, media.pcm_rate));
  this->speaker_->start();
  this->media_.set_mute(this->mute_);
  if (!this->media_.start(media, this->speaker_, this, this->task_stack_in_psram_)) {
    this->speaker_->stop();
    this->hangup();
    return;
  }
  this->media_running_ = true;
  this->microphone_->start();
}

bool SipPhone::start_transport_media(MediaTransport *transport, bool mute) {
#ifdef USE_SIP_PHONE_OPUS
  if (this->media_running_ || this->media_parked_.load())
    return false;
  MediaConfig media;
  media.format.codec = Codec::OPUS;
  media.format.ptime = this->ptime_;
  media.pcm_rate = this->opus_params_.sample_rate;
  media.pcm_channels = this->opus_params_.channels;
  media.opus_bitrate = this->opus_bitrate_;
  media.opus_complexity = this->opus_complexity_;
  media.playout_ms = this->playout_ms_;
  media.transport = transport;
  this->start_media_(media);
  if (!this->media_running_)
    return false;
  this->transport_media_ = true;
  this->media_.set_mute(mute);
  return true;
#else
  return false;
#endif
}

void SipPhone::stop_transport_media() {
  if (!this->transport_media_)
    return;
  this->transport_media_ = false;
  this->stop_media_();
}

void SipPhone::stop_media_() {
  if (!this->media_running_ || this->transport_media_)
    return;
  this->media_running_ = false;
  this->microphone_->stop();
  this->media_.request_stop();
}

void SipPhone::sig_task_entry_(void *param) { static_cast<SipPhone *>(param)->sig_run_(); }

void SipPhone::sig_run_() {
  if (this->network_up_.load())
    this->start_register_(false);
  const int max_fd = std::max(this->sig_fd_, this->wake_fd_);
  for (;;) {
    const uint32_t now = millis();
    this->service_timers_(now);
    const uint32_t wait = this->next_deadline_(millis());
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(this->sig_fd_, &readable);
    FD_SET(this->wake_fd_, &readable);
    struct timeval tv = {.tv_sec = static_cast<time_t>(wait / 1000),
                         .tv_usec = static_cast<suseconds_t>((wait % 1000) * 1000)};
    const int n = ::select(max_fd + 1, &readable, nullptr, nullptr, wait == UINT32_MAX ? nullptr : &tv);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ESP_LOGE(TAG, "select: %d", errno);
      break;
    }
    if (n > 0 && FD_ISSET(this->wake_fd_, &readable)) {
      uint64_t value = 0;
      ::read(this->wake_fd_, &value, sizeof(value));
      Command command;
      while (xQueueReceive(this->commands_, &command, 0) == pdTRUE)
        this->handle_command_(command);
    }
    if (n > 0 && FD_ISSET(this->sig_fd_, &readable)) {
      for (;;) {
        struct sockaddr_in from {};
        socklen_t from_len = sizeof(from);
        const ssize_t len = ::recvfrom(this->sig_fd_, this->rx_buf_, RX_BUFFER, MSG_DONTWAIT,
                                       reinterpret_cast<struct sockaddr *>(&from), &from_len);
        if (len <= 0)
          break;
        this->rx_buf_[len] = '\0';
        this->handle_datagram_(static_cast<size_t>(len), from);
      }
    }
  }
  vTaskSuspend(nullptr);
}

void SipPhone::handle_command_(const Command &command) {
  switch (command.type) {
    case CommandType::CALL:
      this->start_call_(command.target);
      break;
    case CommandType::ANSWER:
      this->answer_call_();
      break;
    case CommandType::HANGUP:
      this->hangup_call_(false);
      break;
    case CommandType::DECLINE:
      this->hangup_call_(true);
      break;
    case CommandType::DTMF_INFO:
      this->send_info_dtmf_(command.text);
      break;
    case CommandType::MESSAGE:
      this->send_message_(command.target, command.text);
      break;
    case CommandType::ACCOUNT: {
      {
        LockGuard guard(this->account_lock_);
        this->task_account_ = this->account_;
      }
      this->registrar_resolved_ = false;
      this->reg_call_id_[0] = '\0';
      this->challenge_.valid = false;
      this->reg_registered_ = false;
      this->reg_refresh_at_ = 0;
      this->reg_retry_at_ = 0;
      this->reg_backoff_ms_ = 0;
      this->tx_(TxRole::REGISTER).active = false;
      if (!this->account_set_()) {
        Event event{};
        event.type = EventType::UNREGISTERED;
        this->post_event_(event);
      } else {
        this->start_register_(false);
      }
      break;
    }
    case CommandType::NETWORK_UP:
      this->network_up_.store(true);
      this->registrar_resolved_ = false;
      this->reg_retry_at_ = 0;
      this->reg_backoff_ms_ = 0;
      this->start_register_(false);
      break;
    case CommandType::NETWORK_DOWN: {
      this->network_up_.store(false);
      this->reg_registered_ = false;
      this->reg_refresh_at_ = 0;
      this->reg_retry_at_ = 0;
      for (auto &tx : this->transactions_)
        tx.active = false;
      if (this->dialog_.active)
        this->end_dialog_("network", this->dialog_.established);
      Event event{};
      event.type = EventType::REGISTRATION_FAILED;
      copy_str(event.text, sizeof(event.text), "network");
      if (this->account_set_())
        this->post_event_(event);
      break;
    }
  }
}

uint32_t SipPhone::next_deadline_(uint32_t now) const {
  uint32_t best = UINT32_MAX;
  auto consider = [&](uint32_t at) {
    if (at == 0)
      return;
    const int32_t diff = static_cast<int32_t>(at - now);
    const uint32_t wait = diff <= 0 ? 0 : static_cast<uint32_t>(diff);
    if (wait < best)
      best = wait;
  };
  for (const auto &tx : this->transactions_) {
    if (!tx.active)
      continue;
    consider(tx.deadline);
    if (!(tx.provisional && strcmp(tx.method, "INVITE") == 0))
      consider(tx.next_send);
  }
  if (this->dialog_.active) {
    consider(this->dialog_.ring_deadline);
    if (this->dialog_.ack_pending) {
      consider(this->dialog_.ok_next_send);
      consider(this->dialog_.ok_deadline);
    }
  }
  consider(this->reg_refresh_at_);
  consider(this->reg_retry_at_);
  return best;
}

void SipPhone::service_timers_(uint32_t now) {
  for (uint8_t i = 0; i < TRANSACTIONS; i++) {
    ClientTx &tx = this->transactions_[i];
    if (!tx.active)
      continue;
    const bool invite = strcmp(tx.method, "INVITE") == 0;
    if (due(now, tx.deadline)) {
      tx.active = false;
      switch (static_cast<TxRole>(i)) {
        case TxRole::REGISTER:
          this->schedule_register_retry_("timeout");
          break;
        case TxRole::INVITE:
          if (this->dialog_.active && !this->dialog_.established)
            this->end_dialog_("timeout", false);
          break;
        case TxRole::MESSAGE: {
          Event event{};
          event.type = EventType::MESSAGE_FAILED;
          copy_str(event.text, sizeof(event.text), "timeout");
          this->post_event_(event);
          break;
        }
        default:
          break;
      }
      continue;
    }
    if (invite && tx.provisional)
      continue;
    if (due(now, tx.next_send)) {
      this->send_raw_(tx.buf, tx.len, tx.dest);
      tx.interval = invite ? tx.interval * 2 : std::min(tx.interval * 2, T2_MS);
      tx.next_send = now + tx.interval;
    }
  }
  Dialog &d = this->dialog_;
  if (d.active && d.ack_pending) {
    if (due(now, d.ok_deadline)) {
      d.ack_pending = false;
      this->send_in_dialog_("BYE", nullptr, nullptr, 0);
      this->end_dialog_("timeout", true);
    } else if (due(now, d.ok_next_send)) {
      this->send_raw_(d.ok_buf, d.ok_len, d.peer);
      d.ok_interval = std::min(d.ok_interval * 2, T2_MS);
      d.ok_next_send = now + d.ok_interval;
    }
  }
  if (d.active && due(now, d.ring_deadline)) {
    d.ring_deadline = 0;
    if (d.outgoing) {
      this->send_cancel_();
      this->end_dialog_("no_answer", false);
    } else {
      this->respond_dialog_(480, "Temporarily Unavailable", nullptr, 0, false);
      this->end_dialog_("missed", false);
    }
  }
  if (due(now, this->reg_refresh_at_)) {
    this->reg_refresh_at_ = 0;
    this->start_register_(false);
  }
  if (due(now, this->reg_retry_at_)) {
    this->reg_retry_at_ = 0;
    this->start_register_(false);
  }
}

bool SipPhone::resolve_(const char *host, uint16_t port, struct sockaddr_in &out) {
  memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (inet_aton(host, &out.sin_addr) != 0)
    return true;
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *result = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &result) != 0 || result == nullptr)
    return false;
  out.sin_addr = reinterpret_cast<struct sockaddr_in *>(result->ai_addr)->sin_addr;
  freeaddrinfo(result);
  return true;
}

bool SipPhone::local_ip_for_(const struct sockaddr_in &) {
  esp_netif_t *netif = esp_netif_get_default_netif();
  esp_netif_ip_info_t info{};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
    return false;
  esp_ip4addr_ntoa(&info.ip, this->local_ip_, sizeof(this->local_ip_));
  return true;
}

void SipPhone::new_branch_(char *out, size_t len) {
  snprintf(out, len, "z9hG4bK%08" PRIx32 "%04" PRIx32, esp_random(), esp_random() & 0xFFFF);
}

void SipPhone::new_tag_(char *out, size_t len) { snprintf(out, len, "%08" PRIx32, esp_random()); }

void SipPhone::our_aor_(char *out, size_t len) const {
  const Account &a = this->task_account_;
  if (a.server[0] != '\0' && a.username[0] != '\0') {
    snprintf(out, len, "sip:%s@%s", a.username, a.domain[0] != '\0' ? a.domain : a.server);
  } else {
    snprintf(out, len, "sip:%s@%s", this->identity_, this->local_ip_);
  }
}

void SipPhone::make_uri_(const char *target, char *out, size_t len) const {
  out[0] = '\0';
  if (target == nullptr || target[0] == '\0')
    return;
  if (strncmp(target, "sip:", 4) == 0 || strncmp(target, "sips:", 5) == 0) {
    copy_str(out, len, target);
  } else if (strchr(target, '@') != nullptr) {
    snprintf(out, len, "sip:%s", target);
  } else if (this->account_set_()) {
    const Account &a = this->task_account_;
    snprintf(out, len, "sip:%s@%s", target, a.domain[0] != '\0' ? a.domain : a.server);
  }
}

void SipPhone::begin_request_(SipWriter &w, const char *method, const char *request_uri, const char *branch) {
  w.add("%s %s SIP/2.0\r\nVia: SIP/2.0/UDP %s:%u;branch=%s;rport\r\nMax-Forwards: 70\r\n", method, request_uri,
        this->local_ip_, this->sip_port_, branch);
}

bool SipPhone::store_challenge_(const SipMessage &msg) {
  Challenge &c = this->challenge_;
  Span header = msg.header("WWW-Authenticate");
  c.proxy = false;
  if (header.empty()) {
    header = msg.header("Proxy-Authenticate");
    c.proxy = true;
  }
  if (header.empty())
    return false;
  auth_param(header, "realm").copy_to(c.realm, sizeof(c.realm));
  auth_param(header, "nonce").copy_to(c.nonce, sizeof(c.nonce));
  auth_param(header, "opaque").copy_to(c.opaque, sizeof(c.opaque));
  Span qop = auth_param(header, "qop");
  c.qop_auth = false;
  for (uint16_t i = 0; i + 4 <= qop.n; i++) {
    if (Span{qop.p + i, static_cast<uint16_t>(qop.n - i)}.starts_with("auth") &&
        (i + 4 == qop.n || qop.p[i + 4] == ',' || qop.p[i + 4] == ' ')) {
      c.qop_auth = true;
      break;
    }
  }
  c.nc = 0;
  c.valid = true;
  return true;
}

void SipPhone::write_auth_(SipWriter &w, const char *method, const char *uri) {
  Challenge &c = this->challenge_;
  if (!c.valid)
    return;
  const Account &a = this->task_account_;
  const char *user = a.auth_username[0] != '\0' ? a.auth_username : a.username;
  char ha1[33];
  char ha2[33];
  char response[33];
  char *text = this->auth_text_;
  snprintf(text, sizeof(this->auth_text_), "%s:%s:%s", user, c.realm, a.password);
  md5_hex(text, ha1);
  snprintf(text, sizeof(this->auth_text_), "%s:%s", method, uri);
  md5_hex(text, ha2);
  char cnonce[12];
  char nc[9];
  if (c.qop_auth) {
    c.nc++;
    snprintf(nc, sizeof(nc), "%08" PRIx32, c.nc);
    snprintf(cnonce, sizeof(cnonce), "%08" PRIx32, esp_random());
    snprintf(text, sizeof(this->auth_text_), "%s:%s:%s:%s:auth:%s", ha1, c.nonce, nc, cnonce, ha2);
  } else {
    snprintf(text, sizeof(this->auth_text_), "%s:%s:%s", ha1, c.nonce, ha2);
  }
  md5_hex(text, response);
  w.add("%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
        c.proxy ? "Proxy-Authorization" : "Authorization", user, c.realm, c.nonce, uri, response);
  if (c.opaque[0] != '\0')
    w.add(", opaque=\"%s\"", c.opaque);
  if (c.qop_auth)
    w.add(", qop=auth, nc=%s, cnonce=\"%s\"", nc, cnonce);
  w.add("\r\n");
}

void SipPhone::send_raw_(const char *data, size_t len, const struct sockaddr_in &dest) {
  ::sendto(this->sig_fd_, data, len, 0, reinterpret_cast<const struct sockaddr *>(&dest), sizeof(dest));
}

void SipPhone::send_tx_(ClientTx &tx, size_t len, bool invite) {
  const uint32_t now = millis();
  tx.len = len;
  tx.active = true;
  tx.provisional = false;
  tx.interval = T1_MS;
  tx.next_send = now + T1_MS;
  tx.deadline = now + TIMER_B_MS;
  copy_str(tx.method, sizeof(tx.method), invite ? "INVITE" : tx.method);
  this->send_raw_(tx.buf, tx.len, tx.dest);
}

void SipPhone::start_register_(bool unregister) {
  if (!this->account_set_() || !this->network_up_.load())
    return;
  ClientTx &tx = this->tx_(TxRole::REGISTER);
  if (tx.active)
    return;
  const Account &a = this->task_account_;
  if (!this->registrar_resolved_) {
    if (!this->resolve_(a.server, a.port != 0 ? a.port : 5060, this->registrar_)) {
      this->schedule_register_retry_("dns");
      return;
    }
    this->registrar_resolved_ = true;
  }
  if (!this->local_ip_for_(this->registrar_)) {
    this->schedule_register_retry_("no_route");
    return;
  }
  if (this->reg_call_id_[0] == '\0') {
    snprintf(this->reg_call_id_, sizeof(this->reg_call_id_), "%08" PRIx32 "%08" PRIx32 "@%s", esp_random(),
             esp_random(), this->local_ip_);
    this->new_tag_(this->reg_tag_, sizeof(this->reg_tag_));
  }
  if (!this->reg_registered_) {
    Event event{};
    event.type = EventType::REGISTERING;
    this->post_event_(event);
  }
  this->reg_removing_ = unregister;
  this->reg_cseq_++;
  char aor[136];
  this->our_aor_(aor, sizeof(aor));
  char ruri[96];
  snprintf(ruri, sizeof(ruri), "sip:%s", a.domain[0] != '\0' ? a.domain : a.server);
  this->new_branch_(tx.branch, sizeof(tx.branch));
  copy_str(tx.method, sizeof(tx.method), "REGISTER");
  copy_str(tx.ruri, sizeof(tx.ruri), ruri);
  tx.dest = this->registrar_;
  tx.cseq = this->reg_cseq_;
  const uint32_t expiry = unregister ? 0 : a.expiry;
  SipWriter w(tx.buf, sizeof(tx.buf));
  this->begin_request_(w, "REGISTER", ruri, tx.branch);
  w.add("From: <%s>;tag=%s\r\nTo: <%s>\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " REGISTER\r\n", aor, this->reg_tag_, aor,
        this->reg_call_id_, this->reg_cseq_);
  w.add("Contact: <sip:%s@%s:%u>;expires=%" PRIu32 "\r\nExpires: %" PRIu32 "\r\n", a.username, this->local_ip_,
        this->sip_port_, expiry, expiry);
  this->write_auth_(w, "REGISTER", ruri);
  w.add("Allow: %s\r\nUser-Agent: %s\r\nContent-Length: 0\r\n\r\n", ALLOW, USER_AGENT);
  if (!w.ok()) {
    ESP_LOGE(TAG, "REGISTER too long");
    return;
  }
  this->send_tx_(tx, w.length(), false);
}

void SipPhone::schedule_register_retry_(const char *reason) {
  this->reg_registered_ = false;
  this->reg_refresh_at_ = 0;
  this->reg_backoff_ms_ =
      this->reg_backoff_ms_ == 0 ? RETRY_MIN_MS : std::min(this->reg_backoff_ms_ * 2, RETRY_MAX_MS);
  this->reg_retry_at_ = millis() + this->reg_backoff_ms_;
  if (this->reg_retry_at_ == 0)
    this->reg_retry_at_ = 1;
  this->registrar_resolved_ = false;
  Event event{};
  event.type = EventType::REGISTRATION_FAILED;
  copy_str(event.text, sizeof(event.text), reason);
  this->post_event_(event);
}

void SipPhone::handle_register_response_(ClientTx &tx, const SipMessage &msg) {
  const int code = msg.status();
  if (code < 200)
    return;
  tx.active = false;
  if (code >= 200 && code < 300) {
    if (this->reg_removing_) {
      this->reg_registered_ = false;
      return;
    }
    int expires = -1;
    const uint8_t contacts = msg.header_count("Contact");
    for (uint8_t i = 0; i < contacts && expires < 0; i++) {
      Span contact = msg.header("Contact", i);
      char needle[24];
      snprintf(needle, sizeof(needle), "@%s:", this->local_ip_);
      if (contacts == 1 || find_bytes(contact.p, contact.n, needle) != nullptr)
        expires = header_param(contact, "expires").to_int(-1);
    }
    if (expires < 0)
      expires = msg.header("Expires").to_int(static_cast<int>(this->task_account_.expiry));
    if (expires < 30)
      expires = 30;
    this->reg_registered_ = true;
    tx.auth_tried = false;
    this->reg_backoff_ms_ = 0;
    this->reg_retry_at_ = 0;
    const uint32_t refresh = static_cast<uint32_t>(expires) * 1000U / 10U * 8U;
    this->reg_refresh_at_ = millis() + refresh;
    if (this->reg_refresh_at_ == 0)
      this->reg_refresh_at_ = 1;
    Event event{};
    event.type = EventType::REGISTERED;
    this->our_aor_(event.text, sizeof(event.text));
    this->post_event_(event);
    return;
  }
  if (code == 401 || code == 407) {
    const Span challenge = msg.header(code == 401 ? "WWW-Authenticate" : "Proxy-Authenticate");
    const bool stale = auth_param(challenge, "stale").equals("true");
    if ((!tx.auth_tried || stale) && this->store_challenge_(msg)) {
      this->start_register_(this->reg_removing_);
      tx.auth_tried = true;
      return;
    }
  }
  if (code == 423) {
    const int min_expires = msg.header("Min-Expires").to_int(-1);
    if (min_expires > 0) {
      this->task_account_.expiry = static_cast<uint32_t>(min_expires);
      this->start_register_(false);
      return;
    }
  }
  tx.auth_tried = false;
  char reason[24];
  if (code == 401 || code == 407 || code == 403) {
    copy_str(reason, sizeof(reason), "auth_failed");
  } else {
    snprintf(reason, sizeof(reason), "error_%d", code);
  }
  this->schedule_register_retry_(reason);
}

bool SipPhone::open_rtp_() {
  this->close_rtp_();
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0)
    return false;
  for (uint16_t i = 0; i < 20; i++) {
    const uint16_t port = static_cast<uint16_t>((this->rtp_port_ & ~1U) + 2 * i);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0) {
      this->dialog_.rtp_fd = fd;
      this->dialog_.rtp_port = port;
      return true;
    }
  }
  ::close(fd);
  return false;
}

void SipPhone::close_rtp_() {
  if (this->dialog_.rtp_fd >= 0) {
    ::close(this->dialog_.rtp_fd);
    this->dialog_.rtp_fd = -1;
  }
}

bool SipPhone::fill_media_(MediaConfig &media, const char *remote_address, uint16_t remote_port) {
  media.format = this->dialog_.format;
  switch (media.format.codec) {
    case Codec::OPUS:
      media.pcm_rate = this->opus_params_.sample_rate;
      media.pcm_channels = this->opus_params_.channels;
      break;
    case Codec::G722:
      media.pcm_rate = 16000;
      media.pcm_channels = 1;
      break;
    default:
      media.pcm_rate = 8000;
      media.pcm_channels = 1;
      break;
  }
  media.socket_fd = this->dialog_.rtp_fd;
  memset(&media.remote, 0, sizeof(media.remote));
  media.remote.sin_family = AF_INET;
  media.remote.sin_port = htons(remote_port);
  if (inet_aton(remote_address, &media.remote.sin_addr) == 0)
    return false;
  media.ssrc = esp_random();
  media.opus_bitrate = this->opus_bitrate_;
  media.opus_complexity = this->opus_complexity_;
  media.playout_ms = this->playout_ms_;
  return true;
}

void SipPhone::event_from_dialog_(Event &event) const {
  copy_str(event.call_id, sizeof(event.call_id), this->dialog_.call_id);
  Span remote = make_span(this->dialog_.remote_hdr);
  uri_of(remote).copy_to(event.peer_uri, sizeof(event.peer_uri));
  display_name_of(remote).copy_to(event.peer_name, sizeof(event.peer_name));
  event.outgoing = this->dialog_.outgoing;
}

void SipPhone::dialog_dest_(struct sockaddr_in &dest) const {
  if (!this->dialog_.direct && this->account_set_() && this->registrar_resolved_) {
    dest = this->registrar_;
    return;
  }
  dest = this->dialog_.peer;
}

void SipPhone::end_dialog_(const char *reason, bool answered) {
  if (!this->dialog_.active)
    return;
  Event event{};
  event.type = EventType::ENDED;
  this->event_from_dialog_(event);
  copy_str(event.text, sizeof(event.text), reason);
  this->close_rtp_();
  this->dialog_.active = false;
  this->dialog_.ack_pending = false;
  this->dialog_.ring_deadline = 0;
  this->post_event_(event);
}

bool SipPhone::start_call_(const char *target) {
  Event failed{};
  failed.type = EventType::ENDED;
  failed.outgoing = true;
  copy_str(failed.peer_uri, sizeof(failed.peer_uri), target);
  if (this->dialog_.active) {
    copy_str(failed.text, sizeof(failed.text), "busy_local");
    this->post_event_(failed);
    return false;
  }
  char uri[128];
  this->make_uri_(target, uri, sizeof(uri));
  if (uri[0] == '\0') {
    copy_str(failed.text, sizeof(failed.text), "invalid");
    this->post_event_(failed);
    return false;
  }
  struct sockaddr_in dest {};
  bool routed = false;
  Span uri_span = make_span(uri);
  char host[64];
  uri_host(uri_span).copy_to(host, sizeof(host));
  const Account &account = this->task_account_;
  if (this->account_set_() && (strcasecmp(host, account.domain) == 0 || strcasecmp(host, account.server) == 0)) {
    if (!this->registrar_resolved_ &&
        this->resolve_(this->task_account_.server, this->task_account_.port != 0 ? this->task_account_.port : 5060,
                       this->registrar_))
      this->registrar_resolved_ = true;
    if (this->registrar_resolved_) {
      dest = this->registrar_;
      routed = true;
    }
  }
  const bool direct = !routed;
  if (!routed)
    routed = this->resolve_(host, uri_port(uri_span, 5060), dest);
  if (!routed || !this->local_ip_for_(dest)) {
    copy_str(failed.text, sizeof(failed.text), "not_found");
    this->post_event_(failed);
    return false;
  }
  Dialog &d = this->dialog_;
  memset(static_cast<void *>(&d), 0, offsetof(Dialog, ok_buf));
  d.rtp_fd = -1;
  if (!this->open_rtp_()) {
    copy_str(failed.text, sizeof(failed.text), "no_rtp_port");
    this->post_event_(failed);
    return false;
  }
  d.active = true;
  d.outgoing = true;
  d.direct = direct;
  snprintf(d.call_id, sizeof(d.call_id), "%08" PRIx32 "%08" PRIx32 "@%s", esp_random(), esp_random(),
           this->local_ip_);
  char aor[136];
  this->our_aor_(aor, sizeof(aor));
  char tag[16];
  this->new_tag_(tag, sizeof(tag));
  const char *display = this->task_account_.display_name;
  if (display[0] != '\0') {
    snprintf(d.local_hdr, sizeof(d.local_hdr), "\"%s\" <%s>;tag=%s", display, aor, tag);
  } else {
    snprintf(d.local_hdr, sizeof(d.local_hdr), "<%s>;tag=%s", aor, tag);
  }
  snprintf(d.remote_hdr, sizeof(d.remote_hdr), "<%s>", uri);
  copy_str(d.remote_target, sizeof(d.remote_target), uri);
  d.local_cseq = esp_random() % 1000 + 1;
  d.invite_cseq = d.local_cseq;
  d.peer = dest;
  d.sdp_session = esp_random() & 0x7FFFFFFF;
  d.sdp_version = 1;
  d.ring_deadline = millis() + this->ring_timeout_ms_;
  if (d.ring_deadline == 0)
    d.ring_deadline = 1;

  Event event{};
  event.type = EventType::OUTGOING;
  this->event_from_dialog_(event);
  this->post_event_(event);
  ClientTx &tx = this->tx_(TxRole::INVITE);
  tx.auth_tried = false;
  this->send_invite_(tx);
  return d.active;
}

void SipPhone::send_invite_(ClientTx &tx) {
  Dialog &d = this->dialog_;
  SipWriter body(this->scratch_, sizeof(this->scratch_));
  write_sdp_offer(body, this->local_ip_, d.rtp_port, d.sdp_session, d.sdp_version, this->codecs_,
                  this->codec_count_, this->opus_params_, this->ptime_, Direction::SENDRECV);
  this->new_branch_(tx.branch, sizeof(tx.branch));
  copy_str(tx.method, sizeof(tx.method), "INVITE");
  copy_str(tx.ruri, sizeof(tx.ruri), d.remote_target);
  tx.dest = d.peer;
  tx.cseq = d.invite_cseq;
  SipWriter w(tx.buf, sizeof(tx.buf));
  this->begin_request_(w, "INVITE", d.remote_target, tx.branch);
  w.add("From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " INVITE\r\n", d.local_hdr, d.remote_hdr, d.call_id,
        d.invite_cseq);
  w.add("Contact: <sip:%s@%s:%u>\r\n", this->account_set_() ? this->task_account_.username : this->identity_,
        this->local_ip_, this->sip_port_);
  this->write_auth_(w, "INVITE", d.remote_target);
  w.add("Allow: %s\r\nUser-Agent: %s\r\nContent-Type: application/sdp\r\nContent-Length: %u\r\n\r\n", ALLOW,
        USER_AGENT, static_cast<unsigned>(body.length()));
  w.add_span(Span{this->scratch_, static_cast<uint16_t>(body.length())});
  if (!w.ok() || !body.ok()) {
    ESP_LOGE(TAG, "INVITE too long");
    this->end_dialog_("failed", false);
    return;
  }
  this->send_tx_(tx, w.length(), true);
}

void SipPhone::send_ack_(const SipMessage &response, const ClientTx &tx, bool for_2xx) {
  SipWriter w(this->scratch_, sizeof(this->scratch_));
  Dialog &d = this->dialog_;
  if (for_2xx) {
    char branch[32];
    this->new_branch_(branch, sizeof(branch));
    this->begin_request_(w, "ACK", d.remote_target, branch);
    for (uint8_t i = 0; i < d.route_count; i++)
      w.add("Route: %s\r\n", d.routes[i]);
    w.add("From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " ACK\r\n", d.local_hdr, d.remote_hdr, d.call_id,
          d.invite_cseq);
    this->write_auth_(w, "ACK", d.remote_target);
    w.add("User-Agent: %s\r\nContent-Length: 0\r\n\r\n", USER_AGENT);
    struct sockaddr_in dest;
    this->dialog_dest_(dest);
    if (w.ok())
      this->send_raw_(w.buffer(), w.length(), dest);
    return;
  }
  w.add("ACK %s SIP/2.0\r\nVia: ", tx.ruri);
  w.add_span(response.header("Via"));
  w.add("\r\nMax-Forwards: 70\r\nFrom: ");
  w.add_span(response.header("From"));
  w.add("\r\nTo: ");
  w.add_span(response.header("To"));
  w.add("\r\nCall-ID: ");
  w.add_span(response.header("Call-ID"));
  w.add("\r\nCSeq: %" PRIu32 " ACK\r\nUser-Agent: %s\r\nContent-Length: 0\r\n\r\n", tx.cseq, USER_AGENT);
  if (w.ok())
    this->send_raw_(w.buffer(), w.length(), tx.dest);
}

void SipPhone::ack_bye_stray_(const SipMessage &response, const ClientTx &tx) {
  Span contact = uri_of(response.header("Contact"));
  char target[128];
  contact.copy_to(target, sizeof(target));
  if (target[0] == '\0')
    copy_str(target, sizeof(target), tx.ruri);
  const int cseq = response.cseq_number();
  for (int step = 0; step < 2; step++) {
    SipWriter w(this->scratch_, sizeof(this->scratch_));
    char branch[32];
    this->new_branch_(branch, sizeof(branch));
    this->begin_request_(w, step == 0 ? "ACK" : "BYE", target, branch);
    w.add("From: ");
    w.add_span(response.header("From"));
    w.add("\r\nTo: ");
    w.add_span(response.header("To"));
    w.add("\r\nCall-ID: ");
    w.add_span(response.header("Call-ID"));
    w.add("\r\nCSeq: %d %s\r\nUser-Agent: %s\r\nContent-Length: 0\r\n\r\n", cseq + step, step == 0 ? "ACK" : "BYE",
          USER_AGENT);
    if (w.ok())
      this->send_raw_(w.buffer(), w.length(), tx.dest);
  }
}

void SipPhone::send_cancel_() {
  ClientTx &invite = this->tx_(TxRole::INVITE);
  if (!invite.active)
    return;
  if (!invite.provisional) {
    this->dialog_.cancel_pending = true;
    return;
  }
  Dialog &d = this->dialog_;
  ClientTx &tx = this->tx_(TxRole::DIALOG);
  copy_str(tx.branch, sizeof(tx.branch), invite.branch);
  copy_str(tx.method, sizeof(tx.method), "CANCEL");
  copy_str(tx.ruri, sizeof(tx.ruri), invite.ruri);
  tx.dest = invite.dest;
  tx.cseq = invite.cseq;
  tx.auth_tried = true;
  SipWriter w(tx.buf, sizeof(tx.buf));
  this->begin_request_(w, "CANCEL", invite.ruri, invite.branch);
  char to[sizeof(Dialog::remote_hdr)];
  copy_str(to, sizeof(to), d.remote_hdr);
  char *tag = strstr(to, ";tag=");
  if (tag != nullptr)
    *tag = '\0';
  w.add("From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " CANCEL\r\nUser-Agent: %s\r\nContent-Length: 0\r\n\r\n",
        d.local_hdr, to, d.call_id, invite.cseq, USER_AGENT);
  if (w.ok())
    this->send_tx_(tx, w.length(), false);
}

void SipPhone::send_in_dialog_(const char *method, const char *extra, const char *body, size_t body_len) {
  Dialog &d = this->dialog_;
  ClientTx &tx = this->tx_(TxRole::DIALOG);
  d.local_cseq++;
  this->new_branch_(tx.branch, sizeof(tx.branch));
  copy_str(tx.method, sizeof(tx.method), method);
  copy_str(tx.ruri, sizeof(tx.ruri), d.remote_target);
  this->dialog_dest_(tx.dest);
  tx.cseq = d.local_cseq;
  tx.auth_tried = false;
  SipWriter w(tx.buf, sizeof(tx.buf));
  this->begin_request_(w, method, d.remote_target, tx.branch);
  for (uint8_t i = 0; i < d.route_count; i++)
    w.add("Route: %s\r\n", d.routes[i]);
  w.add("From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " %s\r\n", d.local_hdr, d.remote_hdr, d.call_id,
        d.local_cseq, method);
  if (extra != nullptr)
    w.add("%s", extra);
  this->write_auth_(w, method, d.remote_target);
  w.add("User-Agent: %s\r\nContent-Length: %u\r\n\r\n", USER_AGENT, static_cast<unsigned>(body_len));
  if (body_len > 0)
    w.add_span(Span{body, static_cast<uint16_t>(body_len)});
  if (w.ok())
    this->send_tx_(tx, w.length(), false);
}

void SipPhone::hangup_call_(bool decline) {
  Dialog &d = this->dialog_;
  if (!d.active)
    return;
  if (!d.outgoing && !d.established) {
    this->respond_dialog_(decline ? 603 : 486, decline ? "Decline" : "Busy Here", nullptr, 0, false);
    this->end_dialog_("declined", false);
    return;
  }
  if (d.outgoing && !d.established) {
    this->send_cancel_();
    this->end_dialog_("cancelled", false);
    return;
  }
  this->send_in_dialog_("BYE", nullptr, nullptr, 0);
  this->end_dialog_("hangup", true);
}

void SipPhone::send_info_dtmf_(const char *digits) {
  Dialog &d = this->dialog_;
  if (!d.active || !d.established)
    return;
  char body[32];
  for (const char *c = digits; *c != '\0'; c++) {
    const int len = snprintf(body, sizeof(body), "Signal=%c\r\nDuration=160\r\n", *c);
    this->send_in_dialog_("INFO", "Content-Type: application/dtmf-relay\r\n", body, static_cast<size_t>(len));
  }
}

void SipPhone::send_message_(const char *target, const char *text) {
  Event failed{};
  failed.type = EventType::MESSAGE_FAILED;
  char uri[128];
  this->make_uri_(target, uri, sizeof(uri));
  struct sockaddr_in dest {};
  bool routed = false;
  if (uri[0] != '\0' && this->account_set_() && this->registrar_resolved_) {
    dest = this->registrar_;
    routed = true;
  } else if (uri[0] != '\0') {
    Span uri_span = make_span(uri);
    char host[64];
    uri_host(uri_span).copy_to(host, sizeof(host));
    routed = this->resolve_(host, uri_port(uri_span, 5060), dest);
  }
  if (!routed || !this->local_ip_for_(dest)) {
    copy_str(failed.text, sizeof(failed.text), "not_found");
    this->post_event_(failed);
    return;
  }
  if (target != this->msg_target_)
    copy_str(this->msg_target_, sizeof(this->msg_target_), target);
  if (text != this->msg_text_)
    copy_str(this->msg_text_, sizeof(this->msg_text_), text);
  ClientTx &tx = this->tx_(TxRole::MESSAGE);
  if (tx.cseq == 0 || !tx.auth_tried) {
    snprintf(this->msg_call_id_, sizeof(this->msg_call_id_), "%08" PRIx32 "%08" PRIx32 "@%s", esp_random(),
             esp_random(), this->local_ip_);
    tx.cseq = 1;
  } else {
    tx.cseq++;
  }
  char aor[136];
  this->our_aor_(aor, sizeof(aor));
  char tag[16];
  this->new_tag_(tag, sizeof(tag));
  this->new_branch_(tx.branch, sizeof(tx.branch));
  copy_str(tx.method, sizeof(tx.method), "MESSAGE");
  copy_str(tx.ruri, sizeof(tx.ruri), uri);
  tx.dest = dest;
  SipWriter w(tx.buf, sizeof(tx.buf));
  this->begin_request_(w, "MESSAGE", uri, tx.branch);
  w.add("From: <%s>;tag=%s\r\nTo: <%s>\r\nCall-ID: %s\r\nCSeq: %" PRIu32 " MESSAGE\r\n", aor, tag, uri,
        this->msg_call_id_, tx.cseq);
  this->write_auth_(w, "MESSAGE", uri);
  const size_t len = strlen(this->msg_text_);
  w.add("User-Agent: %s\r\nContent-Type: text/plain;charset=UTF-8\r\nContent-Length: %u\r\n\r\n", USER_AGENT,
        static_cast<unsigned>(len));
  w.add_span(Span{this->msg_text_, static_cast<uint16_t>(len)});
  if (!w.ok()) {
    copy_str(failed.text, sizeof(failed.text), "too_long");
    this->post_event_(failed);
    return;
  }
  this->send_tx_(tx, w.length(), false);
}

void SipPhone::handle_datagram_(size_t len, const struct sockaddr_in &from) {
  if (len < 4 || (this->rx_buf_[0] == '\r' || this->rx_buf_[0] == '\n'))
    return;
  if (!this->rx_msg_.parse(this->rx_buf_, len))
    return;
  if (this->rx_msg_.is_request()) {
    this->handle_request_(this->rx_msg_, from);
  } else {
    this->handle_response_(this->rx_msg_);
  }
}

void SipPhone::handle_response_(const SipMessage &msg) {
  Span branch = header_param(msg.header("Via"), "branch");
  Span method = msg.cseq_method();
  for (uint8_t i = 0; i < TRANSACTIONS; i++) {
    ClientTx &tx = this->transactions_[i];
    if (!branch.equals(tx.branch) || !method.equals(tx.method))
      continue;
    switch (static_cast<TxRole>(i)) {
      case TxRole::REGISTER:
        if (tx.active)
          this->handle_register_response_(tx, msg);
        return;
      case TxRole::INVITE:
        this->handle_invite_response_(tx, msg);
        return;
      case TxRole::MESSAGE: {
        if (!tx.active || msg.status() < 200)
          return;
        tx.active = false;
        const int code = msg.status();
        if ((code == 401 || code == 407) && !tx.auth_tried && this->store_challenge_(msg)) {
          tx.auth_tried = true;
          this->send_message_(this->msg_target_, this->msg_text_);
          return;
        }
        tx.auth_tried = false;
        tx.cseq = 0;
        Event event{};
        event.type = code < 300 ? EventType::MESSAGE_SENT : EventType::MESSAGE_FAILED;
        snprintf(event.text, sizeof(event.text), "%d", code);
        this->post_event_(event);
        return;
      }
      case TxRole::DIALOG: {
        if (!tx.active)
          return;
        if (msg.status() < 200) {
          tx.provisional = true;
          return;
        }
        tx.active = false;
        const int code = msg.status();
        if ((code == 401 || code == 407) && !tx.auth_tried && this->store_challenge_(msg)) {
          tx.auth_tried = true;
          char method_copy[12];
          copy_str(method_copy, sizeof(method_copy), tx.method);
          if (strcmp(method_copy, "BYE") == 0) {
            this->send_in_dialog_("BYE", nullptr, nullptr, 0);
            this->tx_(TxRole::DIALOG).auth_tried = true;
          }
        }
        return;
      }
    }
  }
}

void SipPhone::handle_invite_response_(ClientTx &tx, const SipMessage &msg) {
  Dialog &d = this->dialog_;
  const int code = msg.status();
  const bool ours = d.active && d.outgoing && msg.header("Call-ID").equals(d.call_id);
  if (code < 200) {
    if (!tx.active)
      return;
    tx.provisional = true;
    tx.deadline = 0;
    if (d.cancel_pending && msg.header("Call-ID").equals(d.call_id)) {
      d.cancel_pending = false;
      this->send_cancel_();
    }
    return;
  }
  if (code < 300) {
    if (ours && d.established) {
      this->send_ack_(msg, tx, true);
      return;
    }
    tx.active = false;
    Span to = msg.header("To");
    if (!ours) {
      this->ack_bye_stray_(msg, tx);
      return;
    }
    to.copy_to(d.remote_hdr, sizeof(d.remote_hdr));
    uri_of(msg.header("Contact")).copy_to(d.remote_target, sizeof(d.remote_target));
    const uint8_t routes = msg.header_count("Record-Route");
    d.route_count = 0;
    for (int i = routes - 1; i >= 0 && d.route_count < MAX_ROUTES; i--)
      msg.header("Record-Route", static_cast<uint8_t>(i)).copy_to(d.routes[d.route_count++], sizeof(d.routes[0]));
    d.established = true;
    d.ring_deadline = 0;
    this->send_ack_(msg, tx, true);
    SdpInfo sdp;
    if (!parse_sdp(msg.body(), sdp)) {
      this->send_in_dialog_("BYE", nullptr, nullptr, 0);
      this->end_dialog_("no_codec", false);
      return;
    }
    d.format = choose_format(sdp, this->codecs_, this->codec_count_);
    d.format.ptime = sdp.ptime;
    if (d.format.codec == Codec::NONE) {
      this->send_in_dialog_("BYE", nullptr, nullptr, 0);
      this->end_dialog_("no_codec", false);
      return;
    }
    Event event{};
    event.type = EventType::ESTABLISHED;
    this->event_from_dialog_(event);
    if (!this->fill_media_(event.media, sdp.address, sdp.audio_port)) {
      this->send_in_dialog_("BYE", nullptr, nullptr, 0);
      this->end_dialog_("failed", false);
      return;
    }
    event.media.send = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::RECVONLY;
    event.media.receive = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::SENDONLY;
    d.rtp_fd = -1;
    this->post_event_(event);
    return;
  }
  this->send_ack_(msg, tx, false);
  if (!tx.active)
    return;
  tx.active = false;
  if ((code == 401 || code == 407) && ours && !tx.auth_tried && this->store_challenge_(msg)) {
    tx.auth_tried = true;
    d.local_cseq++;
    d.invite_cseq = d.local_cseq;
    this->send_invite_(tx);
    this->tx_(TxRole::INVITE).auth_tried = true;
    return;
  }
  if (!ours)
    return;
  const char *reason;
  switch (code) {
    case 486:
    case 600:
      reason = "busy";
      break;
    case 603:
      reason = "declined_remote";
      break;
    case 480:
      reason = "unavailable";
      break;
    case 404:
    case 604:
      reason = "not_found";
      break;
    case 487:
      reason = "cancelled";
      break;
    case 488:
    case 606:
      reason = "no_codec";
      break;
    default:
      reason = "failed";
      break;
  }
  this->end_dialog_(reason, false);
}

void SipPhone::respond_(const SipMessage &msg, const struct sockaddr_in &to, int code, const char *reason,
                        const char *to_tag, const char *extra) {
  SipWriter w(this->scratch_, sizeof(this->scratch_));
  w.add("SIP/2.0 %d %s\r\n", code, reason);
  const uint8_t vias = msg.header_count("Via");
  for (uint8_t i = 0; i < vias; i++) {
    Span via = msg.header("Via", i);
    w.add("Via: ");
    w.add_span(via);
    if (i == 0) {
      char ip[16];
      inet_ntoa_r(to.sin_addr, ip, sizeof(ip));
      if (header_param(via, "received").p == nullptr)
        w.add(";received=%s", ip);
    }
    w.add("\r\n");
  }
  w.add("From: ");
  w.add_span(msg.header("From"));
  w.add("\r\nTo: ");
  Span to_header = msg.header("To");
  w.add_span(to_header);
  if (to_tag != nullptr && header_param(to_header, "tag").empty())
    w.add(";tag=%s", to_tag);
  w.add("\r\nCall-ID: ");
  w.add_span(msg.header("Call-ID"));
  w.add("\r\nCSeq: ");
  w.add_span(msg.header("CSeq"));
  w.add("\r\n");
  if (extra != nullptr)
    w.add("%s", extra);
  w.add("User-Agent: %s\r\nContent-Length: 0\r\n\r\n", USER_AGENT);
  if (w.ok())
    this->send_raw_(w.buffer(), w.length(), to);
}

void SipPhone::respond_dialog_(int code, const char *reason, const char *body, size_t body_len, bool store_ok) {
  Dialog &d = this->dialog_;
  char *buf = store_ok ? d.ok_buf : this->scratch_;
  SipWriter w(buf, TX_BUFFER);
  w.add("SIP/2.0 %d %s\r\n", code, reason);
  for (uint8_t i = 0; i < d.via_count; i++)
    w.add("Via: %s\r\n", d.vias[i]);
  for (uint8_t i = 0; i < d.route_count; i++)
    w.add("Record-Route: %s\r\n", d.routes[i]);
  w.add("From: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n", d.remote_hdr, d.local_hdr, d.call_id, d.cseq_header);
  if (code < 300)
    w.add("Contact: <sip:%s@%s:%u>\r\nAllow: %s\r\n",
          this->account_set_() ? this->task_account_.username : this->identity_, this->local_ip_, this->sip_port_,
          ALLOW);
  w.add("User-Agent: %s\r\n", USER_AGENT);
  if (body_len > 0) {
    w.add("Content-Type: application/sdp\r\nContent-Length: %u\r\n\r\n", static_cast<unsigned>(body_len));
    w.add_span(Span{body, static_cast<uint16_t>(body_len)});
  } else {
    w.add("Content-Length: 0\r\n\r\n");
  }
  if (!w.ok())
    return;
  this->send_raw_(buf, w.length(), d.peer);
  if (store_ok) {
    const uint32_t now = millis();
    d.ok_len = w.length();
    d.ack_pending = true;
    d.ok_interval = T1_MS;
    d.ok_next_send = now + T1_MS;
    d.ok_deadline = now + TIMER_B_MS;
  }
}

void SipPhone::handle_request_(const SipMessage &msg, const struct sockaddr_in &from) {
  Span method = msg.method();
  Dialog &d = this->dialog_;
  const bool in_dialog = d.active && msg.header("Call-ID").equals(d.call_id);
  if (method.equals("INVITE")) {
    this->handle_invite_(msg, from);
  } else if (method.equals("ACK")) {
    if (!in_dialog || !d.ack_pending)
      return;
    d.ack_pending = false;
    if (d.late_offer && !msg.body().empty()) {
      d.late_offer = false;
      SdpInfo sdp;
      if (!parse_sdp(msg.body(), sdp)) {
        this->send_in_dialog_("BYE", nullptr, nullptr, 0);
        this->end_dialog_("no_codec", true);
        return;
      }
      d.format = choose_format(sdp, this->codecs_, this->codec_count_);
      d.format.ptime = sdp.ptime;
      Event event{};
      event.type = EventType::ESTABLISHED;
      this->event_from_dialog_(event);
      if (d.format.codec == Codec::NONE || !this->fill_media_(event.media, sdp.address, sdp.audio_port)) {
        this->send_in_dialog_("BYE", nullptr, nullptr, 0);
        this->end_dialog_("no_codec", true);
        return;
      }
      event.media.send = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::RECVONLY;
      event.media.receive = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::SENDONLY;
      d.rtp_fd = -1;
      this->post_event_(event);
    }
  } else if (method.equals("BYE")) {
    if (!in_dialog) {
      this->respond_(msg, from, 481, "Call/Transaction Does Not Exist", nullptr, nullptr);
      return;
    }
    this->respond_(msg, from, 200, "OK", nullptr, nullptr);
    this->end_dialog_("remote_hangup", d.established);
  } else if (method.equals("CANCEL")) {
    if (!in_dialog || d.outgoing || d.established) {
      this->respond_(msg, from, 481, "Call/Transaction Does Not Exist", nullptr, nullptr);
      return;
    }
    this->respond_(msg, from, 200, "OK", nullptr, nullptr);
    this->respond_dialog_(487, "Request Terminated", nullptr, 0, false);
    this->end_dialog_("missed", false);
  } else if (method.equals("OPTIONS")) {
    char extra[160];
    snprintf(extra, sizeof(extra), "Allow: %s\r\nAccept: application/sdp, text/plain\r\n", ALLOW);
    this->respond_(msg, from, 200, "OK", nullptr, extra);
  } else if (method.equals("MESSAGE")) {
    this->respond_(msg, from, 200, "OK", nullptr, nullptr);
    Event event{};
    event.type = EventType::MESSAGE;
    Span from_header = msg.header("From");
    uri_of(from_header).copy_to(event.peer_uri, sizeof(event.peer_uri));
    display_name_of(from_header).copy_to(event.peer_name, sizeof(event.peer_name));
    msg.body().copy_to(event.text, sizeof(event.text));
    this->post_event_(event);
  } else if (method.equals("INFO")) {
    this->respond_(msg, from, in_dialog ? 200 : 481, in_dialog ? "OK" : "Call/Transaction Does Not Exist", nullptr,
                   nullptr);
    if (!in_dialog)
      return;
    Span body = msg.body();
    char digit = '\0';
    const char *signal = find_bytes(body.p, body.n, "Signal=");
    if (signal != nullptr && signal + 7 < body.p + body.n) {
      digit = signal[7];
    } else if (msg.header("Content-Type").starts_with("application/dtmf") && body.n > 0) {
      digit = body.p[0];
    }
    if (digit != '\0') {
      Event event{};
      event.type = EventType::DTMF;
      event.digit = digit;
      this->post_event_(event);
    }
  } else if (method.equals("NOTIFY")) {
    this->respond_(msg, from, 200, "OK", nullptr, nullptr);
  } else {
    char extra[96];
    snprintf(extra, sizeof(extra), "Allow: %s\r\n", ALLOW);
    this->respond_(msg, from, 405, "Method Not Allowed", nullptr, extra);
  }
}

void SipPhone::handle_invite_(const SipMessage &msg, const struct sockaddr_in &from) {
  Dialog &d = this->dialog_;
  const bool same_call = d.active && msg.header("Call-ID").equals(d.call_id);
  if (!header_param(msg.header("To"), "tag").empty()) {
    if (same_call && d.established) {
      this->handle_reinvite_(msg, from);
    } else {
      this->respond_(msg, from, 481, "Call/Transaction Does Not Exist", nullptr, nullptr);
    }
    return;
  }
  if (same_call) {
    if (d.ack_pending) {
      this->send_raw_(d.ok_buf, d.ok_len, d.peer);
    } else if (!d.established && !d.outgoing) {
      this->respond_dialog_(180, "Ringing", nullptr, 0, false);
    }
    return;
  }
  if (d.active) {
    this->respond_(msg, from, 486, "Busy Here", nullptr, nullptr);
    return;
  }
  Span from_header = msg.header("From");
  if (this->dnd_) {
    this->respond_(msg, from, 486, "Busy Here", nullptr, nullptr);
    Event event{};
    event.type = EventType::ENDED;
    copy_str(event.text, sizeof(event.text), "dnd");
    msg.header("Call-ID").copy_to(event.call_id, sizeof(event.call_id));
    uri_of(from_header).copy_to(event.peer_uri, sizeof(event.peer_uri));
    display_name_of(from_header).copy_to(event.peer_name, sizeof(event.peer_name));
    this->post_event_(event);
    return;
  }
  Span body = msg.body();
  if (!body.empty()) {
    SdpInfo sdp;
    if (!parse_sdp(body, sdp) || choose_format(sdp, this->codecs_, this->codec_count_).codec == Codec::NONE) {
      this->respond_(msg, from, 488, "Not Acceptable Here", nullptr, nullptr);
      return;
    }
    if (body.n >= sizeof(d.offer)) {
      this->respond_(msg, from, 513, "Message Too Large", nullptr, nullptr);
      return;
    }
  }
  memset(static_cast<void *>(&d), 0, offsetof(Dialog, ok_buf));
  d.rtp_fd = -1;
  d.active = true;
  d.direct = !this->registrar_resolved_ || from.sin_addr.s_addr != this->registrar_.sin_addr.s_addr;
  msg.header("Call-ID").copy_to(d.call_id, sizeof(d.call_id));
  from_header.copy_to(d.remote_hdr, sizeof(d.remote_hdr));
  char tag[16];
  this->new_tag_(tag, sizeof(tag));
  Span to_header = msg.header("To");
  const size_t to_len = to_header.copy_to(d.local_hdr, sizeof(d.local_hdr));
  snprintf(d.local_hdr + to_len, sizeof(d.local_hdr) - to_len, ";tag=%s", tag);
  uri_of(msg.header("Contact")).copy_to(d.remote_target, sizeof(d.remote_target));
  d.via_count = 0;
  const uint8_t vias = msg.header_count("Via");
  for (uint8_t i = 0; i < vias && d.via_count < MAX_VIAS; i++) {
    Span via = msg.header("Via", i);
    const size_t len = via.copy_to(d.vias[d.via_count], sizeof(d.vias[0]));
    if (i == 0) {
      char ip[16];
      inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
      if (header_param(via, "received").p == nullptr)
        snprintf(d.vias[0] + len, sizeof(d.vias[0]) - len, ";received=%s", ip);
    }
    d.via_count++;
  }
  const uint8_t routes = msg.header_count("Record-Route");
  for (uint8_t i = 0; i < routes && d.route_count < MAX_ROUTES; i++)
    msg.header("Record-Route", i).copy_to(d.routes[d.route_count++], sizeof(d.routes[0]));
  msg.header("CSeq").copy_to(d.cseq_header, sizeof(d.cseq_header));
  d.local_cseq = esp_random() % 1000 + 1;
  d.peer = from;
  d.offer_len = body.copy_to(d.offer, sizeof(d.offer));
  d.late_offer = body.empty();
  d.sdp_session = esp_random() & 0x7FFFFFFF;
  d.sdp_version = 1;
  d.ring_deadline = millis() + this->ring_timeout_ms_;
  if (d.ring_deadline == 0)
    d.ring_deadline = 1;
  this->local_ip_for_(from);
  this->respond_dialog_(180, "Ringing", nullptr, 0, false);
  Event event{};
  event.type = EventType::INCOMING;
  this->event_from_dialog_(event);
  this->post_event_(event);
}

void SipPhone::answer_call_() {
  Dialog &d = this->dialog_;
  if (!d.active || d.outgoing || d.established)
    return;
  if (!this->open_rtp_()) {
    this->respond_dialog_(500, "Server Internal Error", nullptr, 0, false);
    this->end_dialog_("no_rtp_port", false);
    return;
  }
  d.established = true;
  d.ring_deadline = 0;
  char *body = this->scratch_;
  SipWriter w(body, TX_BUFFER);
  if (d.late_offer) {
    write_sdp_offer(w, this->local_ip_, d.rtp_port, d.sdp_session, d.sdp_version, this->codecs_, this->codec_count_,
                    this->opus_params_, this->ptime_, Direction::SENDRECV);
  } else {
    SdpInfo sdp;
    if (!parse_sdp(Span{d.offer, static_cast<uint16_t>(d.offer_len)}, sdp)) {
      this->respond_dialog_(488, "Not Acceptable Here", nullptr, 0, false);
      this->end_dialog_("no_codec", false);
      return;
    }
    d.format = choose_format(sdp, this->codecs_, this->codec_count_);
    write_sdp_answer(w, this->local_ip_, d.rtp_port, d.sdp_session, d.sdp_version, sdp, d.format, this->opus_params_,
                     answer_direction(sdp.direction));
    if (!w.ok()) {
      this->respond_dialog_(500, "Server Internal Error", nullptr, 0, false);
      this->end_dialog_("failed", false);
      return;
    }
    this->respond_dialog_(200, "OK", body, w.length(), true);
    Event event{};
    event.type = EventType::ESTABLISHED;
    this->event_from_dialog_(event);
    if (!this->fill_media_(event.media, sdp.address, sdp.audio_port)) {
      this->send_in_dialog_("BYE", nullptr, nullptr, 0);
      this->end_dialog_("failed", true);
      return;
    }
    event.media.send = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::RECVONLY;
    event.media.receive = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::SENDONLY;
    d.rtp_fd = -1;
    this->post_event_(event);
    return;
  }
  if (!w.ok())
    return;
  this->respond_dialog_(200, "OK", body, w.length(), true);
}

void SipPhone::handle_reinvite_(const SipMessage &msg, const struct sockaddr_in &from) {
  Dialog &d = this->dialog_;
  SdpInfo sdp;
  const bool has_offer = !msg.body().empty() && parse_sdp(msg.body(), sdp);
  if (!has_offer) {
    this->respond_(msg, from, 488, "Not Acceptable Here", nullptr, nullptr);
    return;
  }
  MediaFormat format = choose_format(sdp, this->codecs_, this->codec_count_);
  if (format.codec != d.format.codec) {
    this->respond_(msg, from, 488, "Not Acceptable Here", nullptr, nullptr);
    return;
  }
  format.ptime = d.format.ptime;
  d.sdp_version++;
  SipWriter body(this->scratch_, sizeof(this->scratch_));
  write_sdp_answer(body, this->local_ip_, d.rtp_port, d.sdp_session, d.sdp_version, sdp, format, this->opus_params_,
                   answer_direction(sdp.direction));
  if (!body.ok())
    return;
  d.via_count = 0;
  const uint8_t vias = msg.header_count("Via");
  for (uint8_t i = 0; i < vias && d.via_count < MAX_VIAS; i++)
    msg.header("Via", i).copy_to(d.vias[d.via_count++], sizeof(d.vias[0]));
  msg.header("CSeq").copy_to(d.cseq_header, sizeof(d.cseq_header));
  d.peer = from;
  this->respond_dialog_(200, "OK", this->scratch_, body.length(), true);
  Event event{};
  event.type = EventType::MEDIA_UPDATE;
  this->event_from_dialog_(event);
  event.media.remote.sin_family = AF_INET;
  event.media.remote.sin_port = htons(sdp.audio_port);
  inet_aton(sdp.address, &event.media.remote.sin_addr);
  event.media.send = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::RECVONLY;
  event.media.receive = sdp.direction == Direction::SENDRECV || sdp.direction == Direction::SENDONLY;
  this->post_event_(event);
}

}

#endif
