#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"
#include "esphome/components/http_request/http_request.h"

#ifdef USE_MESSENGER_CAMERA
#include "esphome/components/camera/camera.h"
#endif
#ifdef USE_MESSENGER_MICROPHONE
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#endif
#if defined(USE_STORAGE) && defined(USE_STORAGE_WORKER)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#define USE_MESSENGER_STORAGE
#endif
#if defined(USE_NETWORK_DEFAULT_ROUTE) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
#include "esphome/components/network/network_component.h"
#define USE_MESSENGER_NETWORK_LISTENER
#endif
#ifdef USE_MESSENGER_ACCOUNT
#include "esphome/components/mtproto/mtproto.h"
#include "telegram_account.h"
#endif
#ifdef USE_MESSENGER_CALL
#include "telegram_call.h"
#endif

namespace esphome::messenger {

enum class Channel : uint8_t { TELEGRAM, WHATSAPP, FACEBOOK };
static constexpr size_t CHANNEL_COUNT = 3;
const char *channel_name(Channel channel);

enum class Transport : uint8_t { DIRECT, HOME_ASSISTANT, ACCOUNT };

enum class FileKind : uint8_t { NONE, PHOTO, VOICE, AUDIO, DOCUMENT, VIDEO };
const char *file_kind_name(FileKind kind);
FileKind file_kind_from_name(const std::string &name);

struct ChannelConfig {
  bool enabled;
  Transport transport;
  int64_t default_chat;
  size_t allowed_count;
  int64_t allowed[8];
  char token[320];
  char account[32];
};

struct Inbound {
  Channel channel;
  FileKind kind;
  int64_t chat_id;
  uint32_t date;
  uint32_t file_size;
  char from[64];
  char text[1024];
  char file_ref[512];
  char file_name[128];
};

struct HistoryEntry {
  Channel channel;
  bool outgoing;
  int64_t chat_id;
  uint32_t date;
  char from[64];
  char text[512];
  char file_ref[512];
  char path[192];
};

enum class JobKind : uint8_t { TEXT, FILE, DOWNLOAD, VOICE };

struct Job {
  JobKind kind{JobKind::TEXT};
  Channel channel{Channel::TELEGRAM};
  FileKind file_kind{FileKind::NONE};
  bool spool{false};
  int64_t chat_id{0};
  std::string body;
  char text[1024]{};
  char name[128]{};
  char path[256]{};
  char ref[512]{};
  uint32_t duration{0};
};

class Messenger : public Component
#ifdef USE_MESSENGER_CAMERA
    ,
                  public camera::CameraListener
#endif
#ifdef USE_MESSENGER_NETWORK_LISTENER
    ,
                  public network::ActiveInterfaceListener
#endif
{
 public:
  Messenger();
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_http_request(http_request::HttpRequestComponent *http) { this->http_ = http; }
#ifdef USE_MESSENGER_ACCOUNT
  void set_mtproto(mtproto::Mtproto *mtproto) {
    this->mtproto_ = mtproto;
    this->account_.attach(mtproto, this);
#ifdef USE_MESSENGER_CALL
    this->call_.attach(mtproto, this);
#endif
  }
  TelegramAccount *account() { return &this->account_; }
  bool load_avatar(int64_t peer_id);
  const uint8_t *avatar_data() const { return this->avatar_; }
  size_t avatar_size() const { return this->avatar_len_; }
  void avatar_release();
  Trigger<int64_t, bool> *get_avatar_trigger() { return &this->avatar_trigger_; }
  Trigger<uint8_t> *get_account_trigger() { return &this->account_trigger_; }
#endif
#ifdef USE_MESSENGER_CALL
  void set_call_phone(sip_phone::SipPhone *phone) { this->call_.set_phone(phone); }
  TelegramCall *call() { return &this->call_; }
  bool call_open_chat();
  Trigger<uint8_t> *get_call_trigger() { return &this->call_trigger_; }
#endif
  bool is_account(Channel channel) const;
  void enable_channel(Channel channel, Transport transport);
  void set_default_channel(Channel channel) { this->default_channel_ = channel; }
  void set_api_version(const char *version) { this->api_version_ = version; }
  void set_spool_dir(const char *dir) { this->spool_dir_ = dir; }
  void set_save_dir(const std::string &dir);
  void set_token(Channel channel, const std::string &token);
  void set_account(Channel channel, const std::string &account);
  void set_allowed_chats(Channel channel, const std::string &list);
  void set_default_chat(Channel channel, int64_t chat_id);
  void set_poll_timeout(uint32_t seconds) { this->poll_timeout_s_ = seconds; }
  void set_max_upload(size_t bytes) { this->max_upload_ = bytes; }
#ifdef USE_MESSENGER_CAMERA
  void set_camera(camera::Camera *cam);
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override;
#endif
#ifdef USE_MESSENGER_MICROPHONE
  void set_microphone_source(microphone::MicrophoneSource *source);
  void set_max_voice_seconds(uint32_t seconds) { this->max_voice_seconds_ = seconds; }
#endif
#ifdef USE_MESSENGER_NETWORK_LISTENER
  void on_active_interface(network::NetworkInterface interface) override;
#endif

  bool is_enabled(Channel channel) const;
  bool is_ready(Channel channel) const;
  bool is_chat_allowed(Channel channel, int64_t chat_id) const;
  Channel resolve_channel(const std::string &name) const;
  Channel default_channel() const { return this->default_channel_; }
  int64_t default_chat(Channel channel) const;

  bool send_message(Channel channel, int64_t chat_id, const std::string &text);
  bool send_file(Channel channel, int64_t chat_id, const std::string &path, FileKind kind, const std::string &caption);
  bool send_camera_photo(Channel channel, int64_t chat_id, const std::string &caption);
  bool capture_photo();
  bool send_photo(Channel channel, int64_t chat_id, const std::string &caption);
  const uint8_t *photo_data() const { return this->photo_; }
  size_t photo_size() const { return this->photo_len_; }
  void photo_release();
  bool download_file(Channel channel, const std::string &file_ref, const std::string &path);
  bool voice_start(Channel channel, int64_t chat_id);
  bool voice_stop_and_send();
  void voice_cancel();
  bool is_recording() const { return this->recording_.load(std::memory_order_acquire); }
  bool receive(Channel channel, int64_t chat_id, const std::string &sender, const std::string &text,
               const std::string &kind, const std::string &file_ref, const std::string &file_name, uint32_t size);
  bool receive_webhook(const std::string &json);

  size_t history_count() const;
  Channel history_channel(size_t index) const;
  int64_t history_chat(size_t index) const;
  std::string history_from(size_t index) const;
  std::string history_text(size_t index) const;
  bool history_outgoing(size_t index) const;
  uint32_t history_date(size_t index) const;
  std::string history_path(size_t index) const;

  Trigger<std::string, int64_t, std::string, std::string> *get_message_trigger() { return &this->message_trigger_; }
  Trigger<std::string, int64_t, std::string, std::string, std::string, std::string, uint32_t> *get_file_trigger() {
    return &this->file_trigger_;
  }
  Trigger<std::string, std::string> *get_downloaded_trigger() { return &this->downloaded_trigger_; }
  Trigger<std::string, std::string, std::string> *get_sent_trigger() { return &this->sent_trigger_; }
  Trigger<> *get_history_trigger() { return &this->history_trigger_; }
  Trigger<> *get_photo_trigger() { return &this->photo_trigger_; }
  Trigger<std::string, int64_t, std::string, std::string, std::string> *get_outbound_trigger() {
    return &this->outbound_trigger_;
  }

 protected:
  static constexpr size_t INBOUND_SLOTS = 8;
  static constexpr size_t HISTORY_SLOTS = 16;
  static constexpr size_t MAX_ALLOWED = 8;
  static constexpr size_t JOB_QUEUE_LEN = 8;
  static constexpr uint32_t TASK_STACK = 10240;
  static constexpr size_t MAX_RESPONSE = 65536;
  static constexpr size_t DOWNLOAD_CHUNK = 16384;
  static constexpr size_t AVATAR_MAX_BYTES = 64 * 1024;

  ChannelConfig &config_(Channel channel) const { return this->configs_[static_cast<size_t>(channel)]; }
  bool direct_(Channel channel) const;
  bool run_account_(Job *job, std::string *error);
  bool run_account_download_(Job *job, std::string *error);
  bool run_account_avatar_(Job *job, std::string *error);
  void save_sent_(Job *job);
  static void poll_task_entry_(void *param);
  static void send_task_entry_(void *param);
  void poll_loop_();
  void send_loop_();
  bool poll_once_();
  void parse_updates_(const std::string &body);
  Inbound *reserve_inbound_();
  void commit_inbound_();
  void add_history_(Channel channel, int64_t chat_id, const char *from, const char *text, bool outgoing, uint32_t date,
                    const char *file_ref = nullptr);
  void set_history_path_(const std::string &file_ref, const std::string &path);
  const HistoryEntry *history_at_(size_t index) const;
  std::string telegram_url_(const char *method) const;
  std::string graph_url_(const char *path) const;
  std::string bearer_(Channel channel) const;
  bool read_body_(http_request::HttpContainer *container, std::string &out, size_t cap);
  int request_(const std::string &url, const char *method, const std::string &body, const char *content_type,
                const std::string &auth, uint32_t timeout_ms, std::string &answer, std::string *error);
  bool check_answer_(Channel channel, int status, const std::string &body, std::string *error);
  bool run_text_(Job *job, std::string *error);
  bool run_file_(Job *job, std::string *error);
  bool run_download_(Job *job, std::string *error);
  bool run_voice_(Job *job, std::string *error);
  bool run_spool_(Job *job, std::string *error);
  bool begin_body_(Job *job, size_t payload);
  void end_body_(Job *job);
  bool queue_job_(Job *job);
  static Job *new_job_(JobKind kind);
  static void free_job_(Job *job);
  void finish_job_(Job *job, bool ok, const std::string &error);
  void outbound_(Channel channel, int64_t chat_id, FileKind kind, const std::string &text, const std::string &path);
#ifdef USE_MESSENGER_STORAGE
  enum class IoOp : uint8_t { OPEN, WRITE, CLOSE };
  void read_upload_step_();
  void finish_upload_read_(bool ok, const char *error);
  bool storage_io_(IoOp op, const uint8_t *data, size_t len, const char *path, std::string *error);
  bool stream_to_storage_(const std::string &url, const std::string &auth, const char *path, std::string *error);
  void run_io_();
  void io_done_(storage::StorageError err);
#endif
#ifdef USE_MESSENGER_MICROPHONE
  void voice_end_capture_();
  void voice_release_();
#endif

  http_request::HttpRequestComponent *http_{nullptr};
  mutable Mutex lock_;
  ChannelConfig *configs_{nullptr};
  Channel default_channel_{Channel::TELEGRAM};
  const char *api_version_{"v26.0"};
  const char *spool_dir_{""};
  char save_dir_[128]{};
  StaticQueue_t jobs_queue_{};
  uint8_t jobs_storage_[JOB_QUEUE_LEN * sizeof(Job *)]{};
  QueueHandle_t jobs_{nullptr};
  StaticTask poll_task_;
  StaticTask send_task_;
  Inbound *inbound_{nullptr};
  HistoryEntry *history_{nullptr};
  uint8_t inbound_head_{0};
  uint8_t inbound_tail_{0};
  size_t history_next_{0};
  size_t history_count_{0};
  int64_t offset_{0};
  uint32_t poll_timeout_s_{50};
  size_t max_upload_{4 * 1024 * 1024};
  std::atomic<bool> token_changed_{false};
  std::atomic<bool> history_changed_{false};

#ifdef USE_MESSENGER_STORAGE
  Job *upload_job_{nullptr};
  storage::StreamHandle upload_stream_{};
  size_t upload_offset_{0};
  size_t upload_size_{0};
  size_t upload_got_{0};
  size_t upload_read_{0};
  bool upload_stream_open_{false};
  storage::StreamHandle download_stream_{};
  IoOp io_op_{IoOp::OPEN};
  const uint8_t *io_data_{nullptr};
  size_t io_len_{0};
  const char *io_path_{nullptr};
  std::atomic<int> io_result_{0};
  std::atomic<bool> io_abandoned_{false};
#endif

#ifdef USE_MESSENGER_ACCOUNT
  mtproto::Mtproto *mtproto_{nullptr};
  TelegramAccount account_;
  Trigger<uint8_t> account_trigger_;
  Trigger<int64_t, bool> avatar_trigger_;
  uint8_t *avatar_{nullptr};
  size_t avatar_len_{0};
#endif
#ifdef USE_MESSENGER_CALL
  TelegramCall call_;
  Trigger<uint8_t> call_trigger_;
#endif
  uint8_t *photo_{nullptr};
  size_t photo_len_{0};
  bool capture_pending_{false};
  Trigger<> photo_trigger_;
#ifdef USE_MESSENGER_CAMERA
  camera::Camera *camera_{nullptr};
  Job *camera_job_{nullptr};
#endif

#ifdef USE_MESSENGER_MICROPHONE
  microphone::MicrophoneSource *mic_source_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> voice_ring_;
  uint32_t max_voice_seconds_{60};
  uint32_t voice_rate_{16000};
  std::atomic<uint8_t> voice_channel_{0};
  std::atomic<int64_t> voice_chat_{0};
  std::atomic<bool> voice_busy_{false};
  std::atomic<bool> voice_cancelled_{false};
#endif
  std::atomic<bool> recording_{false};

  Trigger<std::string, int64_t, std::string, std::string> message_trigger_;
  Trigger<std::string, int64_t, std::string, std::string, std::string, std::string, uint32_t> file_trigger_;
  Trigger<std::string, std::string> downloaded_trigger_;
  Trigger<std::string, std::string, std::string> sent_trigger_;
  Trigger<> history_trigger_;
  Trigger<std::string, int64_t, std::string, std::string, std::string> outbound_trigger_;
};

template<typename... Ts> class MessengerAction : public Action<Ts...>, public Parented<Messenger> {
 public:
  TEMPLATABLE_VALUE(std::string, channel)
  TEMPLATABLE_VALUE(int64_t, chat_id)

 protected:
  Channel channel_of_(const Ts &...x) {
    return this->channel_.has_value() ? this->parent_->resolve_channel(this->channel_.value(x...))
                                      : this->parent_->default_channel();
  }
  int64_t chat_of_(Channel channel, const Ts &...x) {
    return this->chat_id_.has_value() ? this->chat_id_.value(x...) : this->parent_->default_chat(channel);
  }
};

template<typename... Ts> class SendMessageAction : public MessengerAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, text)
  void play(const Ts &...x) override {
    const Channel channel = this->channel_of_(x...);
    this->parent_->send_message(channel, this->chat_of_(channel, x...), this->text_.value(x...));
  }
};

template<typename... Ts> class SendFileAction : public MessengerAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::string, caption)
  void set_kind(FileKind kind) { this->kind_ = kind; }
  void play(const Ts &...x) override {
    const Channel channel = this->channel_of_(x...);
    const std::string caption = this->caption_.has_value() ? this->caption_.value(x...) : std::string();
    this->parent_->send_file(channel, this->chat_of_(channel, x...), this->path_.value(x...), this->kind_, caption);
  }

 protected:
  FileKind kind_{FileKind::DOCUMENT};
};

template<typename... Ts> class SendCameraPhotoAction : public MessengerAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, caption)
  void play(const Ts &...x) override {
    const Channel channel = this->channel_of_(x...);
    const std::string caption = this->caption_.has_value() ? this->caption_.value(x...) : std::string();
    this->parent_->send_camera_photo(channel, this->chat_of_(channel, x...), caption);
  }
};

template<typename... Ts> class DownloadFileAction : public MessengerAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, file_ref)
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override {
    this->parent_->download_file(this->channel_of_(x...), this->file_ref_.value(x...), this->path_.value(x...));
  }
};

template<typename... Ts> class VoiceStartAction : public MessengerAction<Ts...> {
 public:
  void play(const Ts &...x) override {
    const Channel channel = this->channel_of_(x...);
    this->parent_->voice_start(channel, this->chat_of_(channel, x...));
  }
};

template<typename... Ts> class VoiceSendAction : public Action<Ts...>, public Parented<Messenger> {
 public:
  void play(const Ts &...x) override { this->parent_->voice_stop_and_send(); }
};

template<typename... Ts> class VoiceCancelAction : public Action<Ts...>, public Parented<Messenger> {
 public:
  void play(const Ts &...x) override { this->parent_->voice_cancel(); }
};

template<typename... Ts> class ReceiveAction : public MessengerAction<Ts...> {
 public:
  TEMPLATABLE_VALUE(std::string, sender)
  TEMPLATABLE_VALUE(std::string, text)
  TEMPLATABLE_VALUE(std::string, kind)
  TEMPLATABLE_VALUE(std::string, file_ref)
  TEMPLATABLE_VALUE(std::string, file_name)
  TEMPLATABLE_VALUE(uint32_t, size)
  void play(const Ts &...x) override {
    const Channel channel = this->channel_of_(x...);
    this->parent_->receive(channel, this->chat_id_.value(x...), this->sender_.has_value() ? this->sender_.value(x...) : "",
                           this->text_.has_value() ? this->text_.value(x...) : "",
                           this->kind_.has_value() ? this->kind_.value(x...) : "",
                           this->file_ref_.has_value() ? this->file_ref_.value(x...) : "",
                           this->file_name_.has_value() ? this->file_name_.value(x...) : "",
                           this->size_.has_value() ? this->size_.value(x...) : 0);
  }
};

template<typename... Ts> class ReceiveWebhookAction : public Action<Ts...>, public Parented<Messenger> {
 public:
  TEMPLATABLE_VALUE(std::string, payload)
  void play(const Ts &...x) override { this->parent_->receive_webhook(this->payload_.value(x...)); }
};

template<typename... Ts> class IsRecordingCondition : public Condition<Ts...>, public Parented<Messenger> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_recording(); }
};

}

#endif
