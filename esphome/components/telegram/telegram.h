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

#ifdef USE_TELEGRAM_CAMERA
#include "esphome/components/camera/camera.h"
#endif
#ifdef USE_TELEGRAM_MICROPHONE
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#endif
#if defined(USE_STORAGE) && defined(USE_STORAGE_WORKER)
#include "esphome/components/storage/storage.h"
#include "esphome/components/storage/storage_worker.h"
#define USE_TELEGRAM_STORAGE
#endif
#if defined(USE_NETWORK_DEFAULT_ROUTE) && defined(NETWORK_ACTIVE_INTERFACE_LISTENER_COUNT)
#include "esphome/components/network/network_component.h"
#define USE_TELEGRAM_NETWORK_LISTENER
#endif

namespace esphome::telegram {

enum class FileKind : uint8_t { NONE, PHOTO, VOICE, AUDIO, DOCUMENT, VIDEO };
const char *file_kind_name(FileKind kind);

struct Inbound {
  int64_t chat_id;
  uint32_t date;
  uint32_t file_size;
  FileKind kind;
  char from[64];
  char text[1024];
  char file_id[192];
  char file_name[128];
};

struct HistoryEntry {
  int64_t chat_id;
  uint32_t date;
  bool outgoing;
  char from[64];
  char text[512];
  char file_id[192];
  char path[192];
};

enum class JobKind : uint8_t { TEXT, FILE, DOWNLOAD, VOICE };

struct Job {
  JobKind kind{JobKind::TEXT};
  FileKind file_kind{FileKind::NONE};
  int64_t chat_id{0};
  std::string body;
  char text[1024]{};
  char name[128]{};
  char path[256]{};
};

class TelegramBot : public Component
#ifdef USE_TELEGRAM_CAMERA
    ,
                    public camera::CameraListener
#endif
#ifdef USE_TELEGRAM_NETWORK_LISTENER
    ,
                    public network::ActiveInterfaceListener
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_http_request(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_token(const std::string &token);
  void set_allowed_chats(const std::string &list);
  void set_default_chat(int64_t chat_id) { this->default_chat_ = chat_id; }
  void set_poll_timeout(uint32_t seconds) { this->poll_timeout_s_ = seconds; }
  void set_max_upload(size_t bytes) { this->max_upload_ = bytes; }
#ifdef USE_TELEGRAM_CAMERA
  void set_camera(camera::Camera *cam);
  void on_camera_image(const std::shared_ptr<camera::CameraImage> &image) override;
#endif
#ifdef USE_TELEGRAM_MICROPHONE
  void set_microphone_source(microphone::MicrophoneSource *source);
  void set_max_voice_seconds(uint32_t seconds) { this->max_voice_seconds_ = seconds; }
#endif
#ifdef USE_TELEGRAM_NETWORK_LISTENER
  void on_active_interface(network::NetworkInterface interface) override;
#endif

  bool has_token() const;
  bool is_chat_allowed(int64_t chat_id) const;
  int64_t default_chat() const;

  bool send_message(int64_t chat_id, const std::string &text);
  bool send_file(int64_t chat_id, const std::string &path, FileKind kind, const std::string &caption);
  bool send_camera_photo(int64_t chat_id, const std::string &caption);
  bool download_file(const std::string &file_id, const std::string &path);
  bool voice_start();
  bool voice_stop_and_send(int64_t chat_id);
  void voice_cancel();
  bool is_recording() const { return this->recording_.load(std::memory_order_acquire); }

  size_t history_count() const;
  std::string history_from(size_t index) const;
  std::string history_text(size_t index) const;
  bool history_outgoing(size_t index) const;
  uint32_t history_date(size_t index) const;
  std::string history_path(size_t index) const;

  Trigger<int64_t, std::string, std::string> *get_message_trigger() { return &this->message_trigger_; }
  Trigger<int64_t, std::string, std::string, std::string, std::string, uint32_t> *get_file_trigger() {
    return &this->file_trigger_;
  }
  Trigger<std::string, std::string> *get_downloaded_trigger() { return &this->downloaded_trigger_; }
  Trigger<std::string, std::string> *get_sent_trigger() { return &this->sent_trigger_; }
  Trigger<> *get_history_trigger() { return &this->history_trigger_; }

 protected:
  static constexpr size_t INBOUND_SLOTS = 8;
  static constexpr size_t HISTORY_SLOTS = 16;
  static constexpr size_t MAX_ALLOWED = 8;
  static constexpr size_t JOB_QUEUE_LEN = 8;
  static constexpr uint32_t TASK_STACK = 10240;
  static constexpr size_t MAX_RESPONSE = 65536;
  static constexpr size_t DOWNLOAD_CHUNK = 16384;

  static void poll_task_entry_(void *param);
  static void send_task_entry_(void *param);
  void poll_loop_();
  void send_loop_();
  bool poll_once_();
  void parse_updates_(const std::string &body);
  void add_history_(int64_t chat_id, const char *from, const char *text, bool outgoing, uint32_t date,
                    const char *file_id = nullptr);
  void set_history_path_(const std::string &file_id, const std::string &path);
  const HistoryEntry *history_at_(size_t index) const;
  std::string api_url_(const char *method) const;
  bool read_body_(http_request::HttpContainer *container, std::string &out, size_t cap);
  bool check_ok_(const std::string &body, std::string *error);
  bool run_text_(Job *job, std::string *error);
  bool run_file_(Job *job, std::string *error);
  bool run_download_(Job *job, std::string *error);
  bool run_voice_(Job *job, std::string *error);
  bool begin_body_(Job *job, size_t payload);
  void end_body_(Job *job);
  bool queue_job_(Job *job);
  static Job *new_job_(JobKind kind);
  static void free_job_(Job *job);
  void finish_job_(Job *job, bool ok, const std::string &error);
#ifdef USE_TELEGRAM_STORAGE
  enum class IoOp : uint8_t { OPEN, WRITE, CLOSE };
  void read_upload_step_();
  void finish_upload_read_(bool ok, const char *error);
  bool storage_io_(IoOp op, const uint8_t *data, size_t len, const char *path, std::string *error);
  void run_io_();
  void io_done_(storage::StorageError err);
#endif
#ifdef USE_TELEGRAM_MICROPHONE
  void voice_end_capture_();
  void voice_release_();
#endif

  http_request::HttpRequestComponent *http_{nullptr};
  mutable Mutex lock_;
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
  int64_t allowed_[MAX_ALLOWED]{};
  size_t allowed_count_{0};
  int64_t default_chat_{0};
  int64_t offset_{0};
  uint32_t poll_timeout_s_{50};
  size_t max_upload_{4 * 1024 * 1024};
  char token_[96]{};
  std::atomic<bool> token_changed_{false};
  std::atomic<bool> history_changed_{false};

#ifdef USE_TELEGRAM_STORAGE
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

#ifdef USE_TELEGRAM_CAMERA
  camera::Camera *camera_{nullptr};
  Job *camera_job_{nullptr};
#endif

#ifdef USE_TELEGRAM_MICROPHONE
  microphone::MicrophoneSource *mic_source_{nullptr};
  std::unique_ptr<ring_buffer::RingBuffer> voice_ring_;
  uint32_t max_voice_seconds_{60};
  uint32_t voice_rate_{16000};
  std::atomic<int64_t> voice_chat_{0};
  std::atomic<bool> voice_busy_{false};
  std::atomic<bool> voice_cancelled_{false};
#endif
  std::atomic<bool> recording_{false};

  Trigger<int64_t, std::string, std::string> message_trigger_;
  Trigger<int64_t, std::string, std::string, std::string, std::string, uint32_t> file_trigger_;
  Trigger<std::string, std::string> downloaded_trigger_;
  Trigger<std::string, std::string> sent_trigger_;
  Trigger<> history_trigger_;
};

template<typename... Ts> class SendMessageAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  TEMPLATABLE_VALUE(int64_t, chat_id)
  TEMPLATABLE_VALUE(std::string, text)
  void play(const Ts &...x) override {
    const int64_t chat = this->chat_id_.has_value() ? this->chat_id_.value(x...) : this->parent_->default_chat();
    this->parent_->send_message(chat, this->text_.value(x...));
  }
};

template<typename... Ts> class SendFileAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  TEMPLATABLE_VALUE(int64_t, chat_id)
  TEMPLATABLE_VALUE(std::string, path)
  TEMPLATABLE_VALUE(std::string, caption)
  void set_kind(FileKind kind) { this->kind_ = kind; }
  void play(const Ts &...x) override {
    const int64_t chat = this->chat_id_.has_value() ? this->chat_id_.value(x...) : this->parent_->default_chat();
    const std::string caption = this->caption_.has_value() ? this->caption_.value(x...) : std::string();
    this->parent_->send_file(chat, this->path_.value(x...), this->kind_, caption);
  }

 protected:
  FileKind kind_{FileKind::DOCUMENT};
};

template<typename... Ts> class SendCameraPhotoAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  TEMPLATABLE_VALUE(int64_t, chat_id)
  TEMPLATABLE_VALUE(std::string, caption)
  void play(const Ts &...x) override {
    const int64_t chat = this->chat_id_.has_value() ? this->chat_id_.value(x...) : this->parent_->default_chat();
    const std::string caption = this->caption_.has_value() ? this->caption_.value(x...) : std::string();
    this->parent_->send_camera_photo(chat, caption);
  }
};

template<typename... Ts> class DownloadFileAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  TEMPLATABLE_VALUE(std::string, file_id)
  TEMPLATABLE_VALUE(std::string, path)
  void play(const Ts &...x) override { this->parent_->download_file(this->file_id_.value(x...), this->path_.value(x...)); }
};

template<typename... Ts> class VoiceStartAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  void play(const Ts &...x) override { this->parent_->voice_start(); }
};

template<typename... Ts> class VoiceSendAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  TEMPLATABLE_VALUE(int64_t, chat_id)
  void play(const Ts &...x) override {
    const int64_t chat = this->chat_id_.has_value() ? this->chat_id_.value(x...) : this->parent_->default_chat();
    this->parent_->voice_stop_and_send(chat);
  }
};

template<typename... Ts> class VoiceCancelAction : public Action<Ts...>, public Parented<TelegramBot> {
 public:
  void play(const Ts &...x) override { this->parent_->voice_cancel(); }
};

template<typename... Ts> class IsRecordingCondition : public Condition<Ts...>, public Parented<TelegramBot> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_recording(); }
};

}

#endif
