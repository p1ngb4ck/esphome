#include "messenger.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>

#include <strings.h>

#include <esp_heap_caps.h>
#include <esp_random.h>

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_MESSENGER_MICROPHONE
#include <opus.h>
#endif

namespace esphome::messenger {

static const char *const TAG = "messenger";
static const char *const TELEGRAM_HOST = "https://api.telegram.org";
static const char *const GRAPH_HOST = "https://graph.facebook.com/";
static const char *const BOUNDARY = "p4panelMessengerBoundary7d3a";
static constexpr size_t READ_CHUNK = 1024;
static constexpr size_t SMALL_RESPONSE = 4096;
static constexpr size_t BODY_OVERHEAD = 2048;
static constexpr size_t UPLOAD_READ_CHUNK = 65536;
static constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
static constexpr uint32_t UPLOAD_TIMEOUT_MS = 120000;
static constexpr uint32_t IO_WAIT_MS = 30000;
static constexpr uint32_t RETRY_MS = 5000;
static constexpr uint32_t CAMERA_WAIT_MS = 10000;
static constexpr uint32_t VOICE_BITRATE = 24000;
static constexpr uint32_t VOICE_RELEASE_MS = 200;
static constexpr size_t VOICE_RING_SECONDS = 1;
static constexpr size_t OPUS_MAX_PACKET = 1276;

const char *channel_name(Channel channel) {
  switch (channel) {
    case Channel::WHATSAPP:
      return "whatsapp";
    case Channel::FACEBOOK:
      return "facebook";
    default:
      return "telegram";
  }
}

const char *file_kind_name(FileKind kind) {
  switch (kind) {
    case FileKind::PHOTO:
      return "photo";
    case FileKind::VOICE:
      return "voice";
    case FileKind::AUDIO:
      return "audio";
    case FileKind::DOCUMENT:
      return "document";
    case FileKind::VIDEO:
      return "video";
    default:
      return "";
  }
}

FileKind file_kind_from_name(const std::string &name) {
  if (name == "photo" || name == "image")
    return FileKind::PHOTO;
  if (name == "voice")
    return FileKind::VOICE;
  if (name == "audio")
    return FileKind::AUDIO;
  if (name == "video")
    return FileKind::VIDEO;
  if (name == "document" || name == "file")
    return FileKind::DOCUMENT;
  return FileKind::NONE;
}

static void copy_str(char *dst, size_t cap, const char *src) {
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool psram_has(size_t bytes) {
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes;
}

static void post_to_loop(Component *component, uint32_t delay_ms, std::function<void()> &&fn) {
  App.scheduler.set_timeout(component, static_cast<const char *>(nullptr), delay_ms, std::move(fn));
}

static bool ends_with(const char *name, const char *ext) {
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  if (n < e)
    return false;
  for (size_t i = 0; i < e; i++) {
    if (tolower(static_cast<unsigned char>(name[n - e + i])) != ext[i])
      return false;
  }
  return true;
}

static const char *mime_of(const char *name, FileKind kind) {
  static const char *const TABLE[][2] = {
      {".jpg", "image/jpeg"},       {".jpeg", "image/jpeg"},     {".png", "image/png"},
      {".mp4", "video/mp4"},        {".ogg", "audio/ogg"},       {".opus", "audio/ogg"},
      {".mp3", "audio/mpeg"},       {".m4a", "audio/mp4"},       {".aac", "audio/aac"},
      {".amr", "audio/amr"},        {".wav", "audio/wav"},       {".flac", "audio/flac"},
      {".pdf", "application/pdf"},  {".txt", "text/plain"},      {".log", "text/plain"},
      {".json", "application/json"}, {".zip", "application/zip"},
  };
  for (const auto &row : TABLE) {
    if (ends_with(name, row[0]))
      return row[1];
  }
  if (kind == FileKind::PHOTO)
    return "image/jpeg";
  if (kind == FileKind::VOICE)
    return "audio/ogg";
  return "application/octet-stream";
}

static const char *graph_type(Channel channel, FileKind kind) {
  switch (kind) {
    case FileKind::PHOTO:
      return "image";
    case FileKind::VOICE:
    case FileKind::AUDIO:
      return "audio";
    case FileKind::VIDEO:
      return "video";
    default:
      return channel == Channel::WHATSAPP ? "document" : "file";
  }
}

Messenger::Messenger() {
  this->configs_ = RAMAllocator<ChannelConfig>(RAMAllocator<ChannelConfig>::ALLOC_EXTERNAL).allocate(CHANNEL_COUNT);
  if (this->configs_ != nullptr)
    memset(this->configs_, 0, sizeof(ChannelConfig) * CHANNEL_COUNT);
}

void Messenger::setup() {
  if (this->configs_ == nullptr || this->http_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the messenger");
    this->mark_failed();
    return;
  }
  this->jobs_ = xQueueCreateStatic(JOB_QUEUE_LEN, sizeof(Job *), this->jobs_storage_, &this->jobs_queue_);
#ifdef USE_MESSENGER_CALL
  this->call_.attach_camera();
#endif
  this->inbound_ = RAMAllocator<Inbound>(RAMAllocator<Inbound>::ALLOC_EXTERNAL).allocate(INBOUND_SLOTS);
  this->history_ = RAMAllocator<HistoryEntry>(RAMAllocator<HistoryEntry>::ALLOC_EXTERNAL).allocate(HISTORY_SLOTS);
  if (this->jobs_ == nullptr || this->inbound_ == nullptr || this->history_ == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the messenger");
    this->mark_failed();
    return;
  }
  memset(this->inbound_, 0, sizeof(Inbound) * INBOUND_SLOTS);
  memset(this->history_, 0, sizeof(HistoryEntry) * HISTORY_SLOTS);
  if (!this->send_task_.create(&Messenger::send_task_entry_, "msg_send", TASK_STACK, this, 1, true, LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for the send task");
    this->mark_failed();
    return;
  }
  if (this->direct_(Channel::TELEGRAM) &&
      !this->poll_task_.create(&Messenger::poll_task_entry_, "msg_poll", TASK_STACK, this, 1, true, LOOP_TASK_CORE)) {
    ESP_LOGE(TAG, "No memory for the Telegram task");
    this->mark_failed();
    return;
  }
  this->disable_loop();
}

void Messenger::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Messenger:\n"
                "  Default channel: %s\n"
                "  Max upload: %u B",
                channel_name(this->default_channel_), static_cast<unsigned>(this->max_upload_));
  if (this->configs_ == nullptr)
    return;
  for (size_t i = 0; i < CHANNEL_COUNT; i++) {
    const Channel channel = static_cast<Channel>(i);
    if (!this->is_enabled(channel))
      continue;
    ESP_LOGCONFIG(TAG, "  %s: %s, %s", channel_name(channel),
                  this->direct_(channel) ? "direct" : (this->is_account(channel) ? "account" : "Home Assistant"),
                  this->is_ready(channel) ? "ready" : "no credentials");
  }
}

void Messenger::enable_channel(Channel channel, Transport transport) {
  if (this->configs_ == nullptr)
    return;
  ChannelConfig &config = this->config_(channel);
  config.enabled = true;
  config.transport = transport;
}

bool Messenger::is_enabled(Channel channel) const {
  return this->configs_ != nullptr && this->config_(channel).enabled;
}

bool Messenger::direct_(Channel channel) const {
  return this->is_enabled(channel) && this->config_(channel).transport == Transport::DIRECT;
}

#ifdef USE_MESSENGER_CALL
bool Messenger::call_open_chat(bool video) {
  int64_t id = 0;
  int64_t access_hash = 0;
  if (!this->is_ready(Channel::TELEGRAM) || !this->account_.open_user(&id, &access_hash))
    return false;
  return this->call_.start(id, access_hash, this->account_.open_title(), video);
}
#endif

void Messenger::set_save_dir(const std::string &dir) {
  copy_str(this->save_dir_, sizeof(this->save_dir_), dir.c_str());
  const size_t n = strlen(this->save_dir_);
  if (n > 1 && this->save_dir_[n - 1] == '/')
    this->save_dir_[n - 1] = '\0';
}

bool Messenger::run_account_(Job *job, std::string *error) {
#ifdef USE_MESSENGER_ACCOUNT
  const char *mime = job->file_kind == FileKind::VOICE ? "audio/ogg" : mime_of(job->name, job->file_kind);
  const bool ok = this->account_.send_media(static_cast<uint8_t>(job->file_kind),
                                            reinterpret_cast<const uint8_t *>(job->body.data()), job->body.size(),
                                            job->name[0] != '\0' ? job->name : "file", mime, job->text, job->duration,
                                            error);
#ifdef USE_MESSENGER_STORAGE
  if (ok)
    this->save_sent_(job);
#endif
  job->body.clear();
  job->body.shrink_to_fit();
  return ok;
#else
  *error = "no account";
  return false;
#endif
}

bool Messenger::is_account(Channel channel) const {
  return this->is_enabled(channel) && this->config_(channel).transport == Transport::ACCOUNT;
}

bool Messenger::is_ready(Channel channel) const {
  if (!this->is_enabled(channel) || this->is_failed())
    return false;
  if (this->is_account(channel)) {
#ifdef USE_MESSENGER_ACCOUNT
    return this->mtproto_ != nullptr && this->mtproto_->logged_in();
#else
    return false;
#endif
  }
  if (!this->direct_(channel))
    return true;
  LockGuard guard(this->lock_);
  const ChannelConfig &config = this->config_(channel);
  if (config.token[0] == '\0')
    return false;
  return channel != Channel::WHATSAPP || config.account[0] != '\0';
}

void Messenger::set_token(Channel channel, const std::string &token) {
  if (this->configs_ == nullptr)
    return;
  {
    LockGuard guard(this->lock_);
    ChannelConfig &config = this->config_(channel);
    if (strncmp(config.token, token.c_str(), sizeof(config.token)) == 0)
      return;
    copy_str(config.token, sizeof(config.token), token.c_str());
  }
  if (channel != Channel::TELEGRAM)
    return;
  this->token_changed_.store(true, std::memory_order_release);
  if (this->poll_task_.is_created())
    xTaskNotifyGive(this->poll_task_.get_handle());
}

void Messenger::set_account(Channel channel, const std::string &account) {
  if (this->configs_ == nullptr)
    return;
  LockGuard guard(this->lock_);
  ChannelConfig &config = this->config_(channel);
  copy_str(config.account, sizeof(config.account), account.c_str());
}

void Messenger::set_allowed_chats(Channel channel, const std::string &list) {
  if (this->configs_ == nullptr)
    return;
  int64_t parsed[MAX_ALLOWED]{};
  size_t count = 0;
  const char *p = list.c_str();
  while (*p != '\0' && count < MAX_ALLOWED) {
    while (*p == ',' || *p == ' ' || *p == ';' || *p == '+')
      p++;
    if (*p == '\0')
      break;
    char *end = nullptr;
    const long long value = strtoll(p, &end, 10);
    if (end == p)
      break;
    parsed[count++] = static_cast<int64_t>(value);
    p = end;
  }
  LockGuard guard(this->lock_);
  ChannelConfig &config = this->config_(channel);
  memcpy(config.allowed, parsed, sizeof(parsed));
  config.allowed_count = count;
}

void Messenger::set_default_chat(Channel channel, int64_t chat_id) {
  if (this->configs_ == nullptr)
    return;
  LockGuard guard(this->lock_);
  this->config_(channel).default_chat = chat_id;
}

bool Messenger::is_chat_allowed(Channel channel, int64_t chat_id) const {
  if (!this->is_enabled(channel))
    return false;
  LockGuard guard(this->lock_);
  const ChannelConfig &config = this->config_(channel);
  if (config.allowed_count == 0)
    return config.transport == Transport::HOME_ASSISTANT;
  for (size_t i = 0; i < config.allowed_count; i++) {
    if (config.allowed[i] == chat_id)
      return true;
  }
  return false;
}

int64_t Messenger::default_chat(Channel channel) const {
  if (!this->is_enabled(channel))
    return 0;
  LockGuard guard(this->lock_);
  const ChannelConfig &config = this->config_(channel);
  if (config.default_chat != 0)
    return config.default_chat;
  return config.allowed_count > 0 ? config.allowed[0] : 0;
}

Channel Messenger::resolve_channel(const std::string &name) const {
  const char *n = name.c_str();
  if (strcasecmp(n, "telegram") == 0)
    return Channel::TELEGRAM;
  if (strcasecmp(n, "whatsapp") == 0)
    return Channel::WHATSAPP;
  if (strcasecmp(n, "facebook") == 0 || strcasecmp(n, "messenger") == 0)
    return Channel::FACEBOOK;
  return this->default_channel_;
}

#ifdef USE_MESSENGER_NETWORK_LISTENER
void Messenger::on_active_interface(network::NetworkInterface interface) {
  if (interface != network::NetworkInterface::NETWORK_INTERFACE_NONE && this->poll_task_.is_created())
    xTaskNotifyGive(this->poll_task_.get_handle());
}
#endif

std::string Messenger::telegram_url_(const char *method) const {
  std::string url;
  url.reserve(160);
  url = TELEGRAM_HOST;
  url += "/bot";
  {
    LockGuard guard(this->lock_);
    url += this->config_(Channel::TELEGRAM).token;
  }
  url += '/';
  url += method;
  return url;
}

std::string Messenger::graph_url_(const char *path) const {
  std::string url;
  url.reserve(160);
  url = GRAPH_HOST;
  url += this->api_version_;
  url += '/';
  url += path;
  return url;
}

std::string Messenger::bearer_(Channel channel) const {
  std::string auth;
  auth.reserve(340);
  auth = "Bearer ";
  LockGuard guard(this->lock_);
  auth += this->config_(channel).token;
  return auth;
}

bool Messenger::read_body_(http_request::HttpContainer *container, std::string &out, size_t cap) {
  uint8_t buf[READ_CHUNK];
  uint32_t last_data = millis();
  while (true) {
    const int got = container->read(buf, sizeof(buf));
    const auto result =
        http_request::http_read_loop_result(got, last_data, REQUEST_TIMEOUT_MS, container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    if (result == http_request::HttpReadLoopResult::COMPLETE)
      return true;
    if (result != http_request::HttpReadLoopResult::DATA)
      return false;
    if (out.size() + static_cast<size_t>(got) > cap)
      return false;
    out.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(got));
  }
}

int Messenger::request_(const std::string &url, const char *method, const std::string &body, const char *content_type,
                        const std::string &auth, uint32_t timeout_ms, std::string &answer, std::string *error) {
  std::vector<http_request::Header> headers;
  if (content_type != nullptr)
    headers.push_back({"Content-Type", content_type});
  if (!auth.empty())
    headers.push_back({"Authorization", auth});
  auto container =
      this->http_->start(url.c_str(), method, body, headers, std::vector<std::string>{}, timeout_ms);
  if (container == nullptr) {
    *error = "no connection";
    return -1;
  }
  answer.reserve(SMALL_RESPONSE);
  const bool read = this->read_body_(container.get(), answer, MAX_RESPONSE);
  const int status = container->status_code;
  container->end();
  if (!read) {
    *error = "no answer";
    return -1;
  }
  return status;
}

bool Messenger::check_answer_(Channel channel, int status, const std::string &body, std::string *error) {
  if (status < 0)
    return false;
  bool ok = false;
  if (channel == Channel::TELEGRAM) {
    json::parse_json(body, [&ok, error](JsonObject root) -> bool {
      ok = root["ok"] | false;
      if (!ok)
        *error = root["description"] | "request refused";
      return true;
    });
  } else {
    ok = status >= 200 && status < 300;
    json::parse_json(body, [&ok, error](JsonObject root) -> bool {
      JsonObject err = root["error"];
      if (!err.isNull()) {
        ok = false;
        *error = err["message"] | "request refused";
      }
      return true;
    });
    if (!ok && error->empty())
      *error = "HTTP " + to_string(status);
  }
  if (!ok && error->empty())
    *error = "no valid answer";
  return ok;
}

void Messenger::poll_task_entry_(void *param) { static_cast<Messenger *>(param)->poll_loop_(); }
void Messenger::send_task_entry_(void *param) { static_cast<Messenger *>(param)->send_loop_(); }

void Messenger::poll_loop_() {
  while (true) {
    if (!this->is_ready(Channel::TELEGRAM)) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    if (!network::is_connected()) {
#ifdef USE_MESSENGER_NETWORK_LISTENER
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#else
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
#endif
      continue;
    }
    if (this->token_changed_.exchange(false, std::memory_order_acq_rel))
      this->offset_ = 0;
    if (!this->poll_once_())
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RETRY_MS));
  }
}

bool Messenger::poll_once_() {
  std::string url = this->telegram_url_("getUpdates");
  url += "?timeout=";
  url += to_string(this->poll_timeout_s_);
  url += "&offset=";
  url += to_string(this->offset_);
  url += "&allowed_updates=%5B%22message%22%5D";
  std::string body;
  std::string error;
  const int status = this->request_(url, "GET", "", nullptr, "", (this->poll_timeout_s_ + 15) * 1000, body, &error);
  if (status != 200) {
    if (status > 0)
      ESP_LOGW(TAG, "getUpdates: HTTP %d", status);
    return false;
  }
  this->parse_updates_(body);
  return true;
}

Inbound *Messenger::reserve_inbound_() {
  Inbound *in = nullptr;
  {
    LockGuard guard(this->lock_);
    if (static_cast<uint8_t>(this->inbound_tail_ - this->inbound_head_) < INBOUND_SLOTS)
      in = &this->inbound_[this->inbound_tail_ % INBOUND_SLOTS];
  }
  if (in == nullptr) {
    ESP_LOGW(TAG, "Inbound queue full: message dropped");
    return nullptr;
  }
  memset(in, 0, sizeof(Inbound));
  return in;
}

void Messenger::commit_inbound_() {
  {
    LockGuard guard(this->lock_);
    this->inbound_tail_++;
  }
  this->enable_loop_soon_any_context();
}

void Messenger::parse_updates_(const std::string &body) {
  json::parse_json(body, [this](JsonObject root) -> bool {
    if (!(root["ok"] | false))
      return false;
    JsonArray result = root["result"];
    for (JsonObject update : result) {
      const int64_t update_id = update["update_id"] | static_cast<int64_t>(0);
      if (update_id + 1 > this->offset_)
        this->offset_ = update_id + 1;
      JsonObject msg = update["message"];
      if (msg.isNull())
        continue;
      const int64_t chat_id = msg["chat"]["id"] | static_cast<int64_t>(0);
      if (!this->is_chat_allowed(Channel::TELEGRAM, chat_id)) {
        ESP_LOGW(TAG, "Telegram chat %" PRId64 " ignored (not allowed)", chat_id);
        continue;
      }
      Inbound *in = this->reserve_inbound_();
      if (in == nullptr)
        continue;
      in->channel = Channel::TELEGRAM;
      in->chat_id = chat_id;
      in->date = msg["date"] | 0u;
      std::string name = msg["from"]["first_name"] | "";
      const char *last = msg["from"]["last_name"] | "";
      if (last[0] != '\0') {
        name += ' ';
        name += last;
      }
      if (name.empty())
        name = msg["chat"]["title"] | "";
      copy_str(in->from, sizeof(in->from), name.c_str());
      const char *text = msg["text"] | "";
      if (text[0] == '\0')
        text = msg["caption"] | "";
      copy_str(in->text, sizeof(in->text), text);
      JsonObject file;
      const char *default_name = "";
      if (msg["photo"].is<JsonArray>()) {
        JsonArray photos = msg["photo"];
        if (photos.size() > 0)
          file = photos[photos.size() - 1];
        in->kind = FileKind::PHOTO;
        default_name = "photo.jpg";
      } else if (msg["voice"].is<JsonObject>()) {
        file = msg["voice"];
        in->kind = FileKind::VOICE;
        default_name = "voice.ogg";
      } else if (msg["audio"].is<JsonObject>()) {
        file = msg["audio"];
        in->kind = FileKind::AUDIO;
        default_name = "audio";
      } else if (msg["video"].is<JsonObject>()) {
        file = msg["video"];
        in->kind = FileKind::VIDEO;
        default_name = "video.mp4";
      } else if (msg["document"].is<JsonObject>()) {
        file = msg["document"];
        in->kind = FileKind::DOCUMENT;
        default_name = "document";
      }
      if (in->kind != FileKind::NONE && !file.isNull()) {
        copy_str(in->file_ref, sizeof(in->file_ref), file["file_id"] | "");
        copy_str(in->file_name, sizeof(in->file_name), file["file_name"] | default_name);
        in->file_size = file["file_size"] | 0u;
      }
      this->commit_inbound_();
    }
    return true;
  });
}

bool Messenger::receive(Channel channel, int64_t chat_id, const std::string &sender, const std::string &text,
                        const std::string &kind, const std::string &file_ref, const std::string &file_name,
                        uint32_t size) {
  if (this->is_failed() || this->inbound_ == nullptr || !this->is_chat_allowed(channel, chat_id)) {
    ESP_LOGW(TAG, "%s chat %" PRId64 " ignored (not allowed)", channel_name(channel), chat_id);
    return false;
  }
  Inbound *in = this->reserve_inbound_();
  if (in == nullptr)
    return false;
  in->channel = channel;
  in->chat_id = chat_id;
  in->kind = file_ref.empty() ? FileKind::NONE : file_kind_from_name(kind);
  if (in->kind == FileKind::NONE && !file_ref.empty())
    in->kind = FileKind::DOCUMENT;
  in->file_size = size;
  copy_str(in->from, sizeof(in->from), sender.c_str());
  copy_str(in->text, sizeof(in->text), text.c_str());
  copy_str(in->file_ref, sizeof(in->file_ref), file_ref.c_str());
  copy_str(in->file_name, sizeof(in->file_name), file_name.c_str());
  this->commit_inbound_();
  return true;
}

bool Messenger::receive_webhook(const std::string &json) {
  if (this->is_failed() || this->inbound_ == nullptr)
    return false;
  return json::parse_json(json, [this](JsonObject root) -> bool {
    const char *object = root["object"] | "";
    if (strcmp(object, "whatsapp_business_account") == 0) {
      if (!this->is_enabled(Channel::WHATSAPP))
        return false;
      JsonArray entries = root["entry"];
      for (JsonObject entry : entries) {
        JsonArray changes = entry["changes"];
        for (JsonObject change : changes) {
          JsonObject value = change["value"];
          const char *profile = value["contacts"][0]["profile"]["name"] | "";
          JsonArray messages = value["messages"];
          for (JsonObject msg : messages) {
            const char *from = msg["from"] | "";
            const int64_t chat_id = strtoll(from, nullptr, 10);
            if (!this->is_chat_allowed(Channel::WHATSAPP, chat_id)) {
              ESP_LOGW(TAG, "WhatsApp chat %" PRId64 " ignored (not allowed)", chat_id);
              continue;
            }
            Inbound *in = this->reserve_inbound_();
            if (in == nullptr)
              continue;
            in->channel = Channel::WHATSAPP;
            in->chat_id = chat_id;
            in->date = static_cast<uint32_t>(strtoul(msg["timestamp"] | "0", nullptr, 10));
            copy_str(in->from, sizeof(in->from), profile[0] != '\0' ? profile : from);
            const std::string type = msg["type"] | "";
            if (type == "text") {
              copy_str(in->text, sizeof(in->text), msg["text"]["body"] | "");
            } else if (type == "image" || type == "audio" || type == "video" || type == "document" ||
                       type == "sticker") {
              JsonObject media = msg[type];
              in->kind = type == "image" || type == "sticker" ? FileKind::PHOTO
                         : type == "audio"                    ? ((media["voice"] | false) ? FileKind::VOICE
                                                                                          : FileKind::AUDIO)
                         : type == "video"                    ? FileKind::VIDEO
                                                              : FileKind::DOCUMENT;
              copy_str(in->text, sizeof(in->text), media["caption"] | "");
              copy_str(in->file_ref, sizeof(in->file_ref), media["id"] | "");
              const char *fallback = in->kind == FileKind::PHOTO   ? "photo.jpg"
                                     : in->kind == FileKind::VOICE ? "voice.ogg"
                                     : in->kind == FileKind::VIDEO ? "video.mp4"
                                                                   : "file";
              copy_str(in->file_name, sizeof(in->file_name), media["filename"] | fallback);
            } else {
              copy_str(in->text, sizeof(in->text), ("[" + type + "]").c_str());
            }
            this->commit_inbound_();
          }
        }
      }
      return true;
    }
    if (strcmp(object, "page") == 0) {
      if (!this->is_enabled(Channel::FACEBOOK))
        return false;
      JsonArray entries = root["entry"];
      for (JsonObject entry : entries) {
        JsonArray events = entry["messaging"];
        for (JsonObject event : events) {
          JsonObject msg = event["message"];
          if (msg.isNull() || (msg["is_echo"] | false))
            continue;
          const char *sender = event["sender"]["id"] | "";
          const int64_t chat_id = strtoll(sender, nullptr, 10);
          if (!this->is_chat_allowed(Channel::FACEBOOK, chat_id)) {
            ESP_LOGW(TAG, "Facebook chat %" PRId64 " ignored (not allowed)", chat_id);
            continue;
          }
          Inbound *in = this->reserve_inbound_();
          if (in == nullptr)
            continue;
          in->channel = Channel::FACEBOOK;
          in->chat_id = chat_id;
          in->date = static_cast<uint32_t>((event["timestamp"] | static_cast<int64_t>(0)) / 1000);
          copy_str(in->from, sizeof(in->from), sender);
          copy_str(in->text, sizeof(in->text), msg["text"] | "");
          JsonObject attachment = msg["attachments"][0];
          if (!attachment.isNull()) {
            const std::string type = attachment["type"] | "";
            in->kind = type == "image"   ? FileKind::PHOTO
                       : type == "audio" ? FileKind::AUDIO
                       : type == "video" ? FileKind::VIDEO
                                         : FileKind::DOCUMENT;
            copy_str(in->file_ref, sizeof(in->file_ref), attachment["payload"]["url"] | "");
            const char *fallback = in->kind == FileKind::PHOTO   ? "photo.jpg"
                                   : in->kind == FileKind::AUDIO ? "audio.mp4"
                                   : in->kind == FileKind::VIDEO ? "video.mp4"
                                                                 : "file";
            copy_str(in->file_name, sizeof(in->file_name), fallback);
            if (in->file_ref[0] == '\0')
              in->kind = FileKind::NONE;
          }
          this->commit_inbound_();
        }
      }
      return true;
    }
    return false;
  });
}

void Messenger::add_history_(Channel channel, int64_t chat_id, const char *from, const char *text, bool outgoing,
                             uint32_t date, const char *file_ref) {
  {
    LockGuard guard(this->lock_);
    HistoryEntry &entry = this->history_[this->history_next_];
    entry.channel = channel;
    entry.chat_id = chat_id;
    entry.date = date;
    entry.outgoing = outgoing;
    copy_str(entry.from, sizeof(entry.from), from);
    copy_str(entry.text, sizeof(entry.text), text);
    copy_str(entry.file_ref, sizeof(entry.file_ref), file_ref != nullptr ? file_ref : "");
    entry.path[0] = '\0';
    this->history_next_ = (this->history_next_ + 1) % HISTORY_SLOTS;
    if (this->history_count_ < HISTORY_SLOTS)
      this->history_count_++;
  }
  this->history_changed_.store(true, std::memory_order_release);
  this->enable_loop_soon_any_context();
}

void Messenger::set_history_path_(const std::string &file_ref, const std::string &path) {
  {
    LockGuard guard(this->lock_);
    for (size_t i = 0; i < HISTORY_SLOTS; i++) {
      HistoryEntry &entry = this->history_[i];
      if (entry.file_ref[0] != '\0' && file_ref == entry.file_ref)
        copy_str(entry.path, sizeof(entry.path), path.c_str());
    }
  }
  this->history_changed_.store(true, std::memory_order_release);
  this->enable_loop();
}

const HistoryEntry *Messenger::history_at_(size_t index) const {
  if (this->history_ == nullptr || index >= this->history_count_)
    return nullptr;
  return &this->history_[(this->history_next_ + HISTORY_SLOTS - this->history_count_ + index) % HISTORY_SLOTS];
}

size_t Messenger::history_count() const {
  LockGuard guard(this->lock_);
  return this->history_count_;
}

Channel Messenger::history_channel(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? entry->channel : this->default_channel_;
}

int64_t Messenger::history_chat(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? entry->chat_id : 0;
}

std::string Messenger::history_from(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? std::string(entry->from) : std::string();
}

std::string Messenger::history_text(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? std::string(entry->text) : std::string();
}

bool Messenger::history_outgoing(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr && entry->outgoing;
}

uint32_t Messenger::history_date(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? entry->date : 0;
}

std::string Messenger::history_path(size_t index) const {
  LockGuard guard(this->lock_);
  const HistoryEntry *entry = this->history_at_(index);
  return entry != nullptr ? std::string(entry->path) : std::string();
}

void Messenger::loop() {
  Inbound *in = nullptr;
  {
    LockGuard guard(this->lock_);
    if (this->inbound_head_ != this->inbound_tail_)
      in = &this->inbound_[this->inbound_head_ % INBOUND_SLOTS];
  }
  bool more = false;
  if (in != nullptr) {
    const std::string channel = channel_name(in->channel);
    if (in->kind == FileKind::NONE) {
      this->message_trigger_.trigger(channel, in->chat_id, std::string(in->from), std::string(in->text));
      this->add_history_(in->channel, in->chat_id, in->from, in->text, false, in->date);
    } else {
      this->file_trigger_.trigger(channel, in->chat_id, std::string(in->from), std::string(file_kind_name(in->kind)),
                                  std::string(in->file_ref), std::string(in->file_name), in->file_size);
      std::string line = "[";
      line += file_kind_name(in->kind);
      line += "] ";
      line += in->text[0] != '\0' ? in->text : in->file_name;
      this->add_history_(in->channel, in->chat_id, in->from, line.c_str(), false, in->date, in->file_ref);
    }
    LockGuard guard(this->lock_);
    this->inbound_head_++;
    more = this->inbound_head_ != this->inbound_tail_;
  }
  if (this->history_changed_.exchange(false, std::memory_order_acq_rel))
    this->history_trigger_.trigger();
#ifdef USE_MESSENGER_CALL
  if (this->call_.take_change())
    this->call_trigger_.trigger(static_cast<uint8_t>(this->call_.state()));
#endif
#ifdef USE_MESSENGER_ACCOUNT
  AccountIncomingFile incoming{};
  while (this->account_.take_incoming_file(&incoming)) {
    static const char *const KINDS[] = {"document", "photo", "voice", "video", "document", "document"};
    char ref[24];
    snprintf(ref, sizeof(ref), "tg:%ld", static_cast<long>(incoming.message_id));
    this->file_trigger_.trigger(channel_name(Channel::TELEGRAM), incoming.chat_id, std::string(incoming.from),
                                std::string(KINDS[incoming.kind < 6 ? incoming.kind : 0]), std::string(ref),
                                std::string(incoming.file.name), incoming.file.size);
  }
  const uint8_t changes = this->account_.take_changes();
  if (changes != 0)
    this->account_trigger_.trigger(changes);
#endif
  if (!more)
    this->disable_loop();
}

Job *Messenger::new_job_(JobKind kind) {
  void *mem = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(sizeof(Job));
  if (mem == nullptr)
    return nullptr;
  Job *job = new (mem) Job();
  job->kind = kind;
  return job;
}

void Messenger::free_job_(Job *job) {
  if (job == nullptr)
    return;
  job->~Job();
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(reinterpret_cast<uint8_t *>(job), sizeof(Job));
}

bool Messenger::queue_job_(Job *job) {
  if (this->jobs_ == nullptr || xQueueSend(this->jobs_, &job, 0) != pdTRUE) {
    ESP_LOGW(TAG, "Send queue full");
    free_job_(job);
    return false;
  }
  return true;
}

void Messenger::outbound_(Channel channel, int64_t chat_id, FileKind kind, const std::string &text,
                          const std::string &path) {
  const char *kind_name = kind == FileKind::NONE ? "text" : file_kind_name(kind);
  this->outbound_trigger_.trigger(std::string(channel_name(channel)), chat_id, std::string(kind_name), text, path);
  std::string line;
  if (kind == FileKind::NONE) {
    line = text;
  } else {
    line = "[";
    line += kind_name;
    line += "] ";
    line += text.empty() ? path.substr(path.rfind('/') + 1) : text;
  }
  this->add_history_(channel, chat_id, "", line.c_str(), true, 0);
}

static void append_part_header(std::string &body, const char *name, const char *filename, const char *type) {
  body += "--";
  body += BOUNDARY;
  body += "\r\nContent-Disposition: form-data; name=\"";
  body += name;
  body += '"';
  if (filename != nullptr) {
    body += "; filename=\"";
    for (const char *c = filename; *c != '\0'; c++)
      body += (*c == '"' || *c == '\r' || *c == '\n') ? '_' : *c;
    body += '"';
  }
  body += "\r\n";
  if (type != nullptr) {
    body += "Content-Type: ";
    body += type;
    body += "\r\n";
  }
  body += "\r\n";
}

static void append_field(std::string &body, const char *name, const std::string &value) {
  append_part_header(body, name, nullptr, nullptr);
  body += value;
  body += "\r\n";
}

static const char *telegram_method(FileKind kind, const char **field) {
  switch (kind) {
    case FileKind::PHOTO:
      *field = "photo";
      return "sendPhoto";
    case FileKind::VOICE:
      *field = "voice";
      return "sendVoice";
    case FileKind::AUDIO:
      *field = "audio";
      return "sendAudio";
    case FileKind::VIDEO:
      *field = "video";
      return "sendVideo";
    default:
      *field = "document";
      return "sendDocument";
  }
}

bool Messenger::begin_body_(Job *job, size_t payload) {
  const size_t cap = payload + BODY_OVERHEAD + strlen(job->text) + strlen(job->name);
  if (!psram_has(cap)) {
    ESP_LOGW(TAG, "No PSRAM for %u B to send", static_cast<unsigned>(cap));
    return false;
  }
  job->body.reserve(cap);
  if (job->spool)
    return true;
  const char *field = "file";
  if (job->channel == Channel::TELEGRAM) {
    telegram_method(job->file_kind, &field);
  } else if (job->channel == Channel::FACEBOOK) {
    field = "filedata";
  }
  append_part_header(job->body, field, job->name[0] != '\0' ? job->name : field, mime_of(job->name, job->file_kind));
  return true;
}

void Messenger::end_body_(Job *job) {
  if (job->spool)
    return;
  std::string &body = job->body;
  body += "\r\n";
  if (job->channel == Channel::TELEGRAM) {
    append_field(body, "chat_id", to_string(job->chat_id));
    if (job->text[0] != '\0')
      append_field(body, "caption", job->text);
  } else if (job->channel == Channel::WHATSAPP) {
    append_field(body, "messaging_product", "whatsapp");
    append_field(body, "type", mime_of(job->name, job->file_kind));
  } else {
    append_field(body, "recipient", "{\"id\":\"" + to_string(job->chat_id) + "\"}");
    append_field(body, "messaging_type", "RESPONSE");
    append_field(body, "message",
                 std::string("{\"attachment\":{\"type\":\"") + graph_type(job->channel, job->file_kind) +
                     "\",\"payload\":{\"is_reusable\":false}}}");
  }
  body += "--";
  body += BOUNDARY;
  body += "--\r\n";
}

void Messenger::photo_release() {
  heap_caps_free(this->photo_);
  this->photo_ = nullptr;
  this->photo_len_ = 0;
}

bool Messenger::send_photo(Channel channel, int64_t chat_id, const std::string &caption) {
  if (this->photo_ == nullptr || !this->is_ready(channel) || (chat_id == 0 && !this->is_account(channel)))
    return false;
  Job *job = new_job_(JobKind::FILE);
  if (job == nullptr)
    return false;
  job->channel = channel;
  job->spool = !this->direct_(channel);
  job->chat_id = chat_id;
  job->file_kind = FileKind::PHOTO;
  copy_str(job->text, sizeof(job->text), caption.c_str());
  copy_str(job->name, sizeof(job->name), "photo.jpg");
  if (!this->begin_body_(job, this->photo_len_)) {
    free_job_(job);
    return false;
  }
  job->body.append(reinterpret_cast<const char *>(this->photo_), this->photo_len_);
  this->end_body_(job);
  return this->queue_job_(job);
}

bool Messenger::send_message(Channel channel, int64_t chat_id, const std::string &text) {
  if (this->is_account(channel)) {
#ifdef USE_MESSENGER_ACCOUNT
    return this->is_ready(channel) && this->account_.send_text(text);
#else
    return false;
#endif
  }
  if (!this->is_ready(channel) || chat_id == 0 || text.empty())
    return false;
  if (!this->direct_(channel)) {
    this->outbound_(channel, chat_id, FileKind::NONE, text, "");
    return true;
  }
  Job *job = new_job_(JobKind::TEXT);
  if (job == nullptr)
    return false;
  job->channel = channel;
  job->chat_id = chat_id;
  copy_str(job->text, sizeof(job->text), text.c_str());
  return this->queue_job_(job);
}

void Messenger::send_loop_() {
  while (true) {
    Job *job = nullptr;
    if (xQueueReceive(this->jobs_, &job, portMAX_DELAY) != pdTRUE || job == nullptr)
      continue;
    std::string error;
    bool ok = false;
    if (job->kind == JobKind::VOICE) {
      ok = this->run_voice_(job, &error);
    } else if (job->kind == JobKind::FILE && this->is_account(job->channel)) {
      ok = this->run_account_(job, &error);
    } else if (job->spool) {
      ok = this->run_spool_(job, &error);
    } else if (!network::is_connected()) {
      error = "no network";
    } else if (job->kind == JobKind::TEXT) {
      ok = this->run_text_(job, &error);
    } else if (job->kind == JobKind::FILE) {
      ok = this->run_file_(job, &error);
    } else {
      ok = this->run_download_(job, &error);
    }
    this->finish_job_(job, ok, error);
  }
}

void Messenger::finish_job_(Job *job, bool ok, const std::string &error) {
  const bool download = job->kind == JobKind::DOWNLOAD;
  const std::string channel = channel_name(job->channel);
  const std::string what = job->kind == JobKind::TEXT ? std::string("text")
                           : download                 ? std::string(job->path)
                                                      : std::string(file_kind_name(job->file_kind));
  const std::string file_ref = download && ok ? std::string(job->ref) : std::string();
  free_job_(job);
  if (!ok && error.empty())
    return;
  if (!ok)
    ESP_LOGW(TAG, "%s %s failed: %s", channel.c_str(), download ? "download" : what.c_str(), error.c_str());
  post_to_loop(this, 0, [this, download, channel, what, error, file_ref]() {
    if (download) {
      if (!file_ref.empty())
        this->set_history_path_(file_ref, what);
      this->downloaded_trigger_.trigger(what, error);
    } else {
      this->sent_trigger_.trigger(channel, what, error);
    }
  });
}

bool Messenger::run_text_(Job *job, std::string *error) {
  const int64_t chat_id = job->chat_id;
  const char *text = job->text;
  std::string url;
  std::string body;
  std::string auth;
  if (job->channel == Channel::TELEGRAM) {
    url = this->telegram_url_("sendMessage");
    body = json::build_json([chat_id, text](JsonObject root) {
      root["chat_id"] = chat_id;
      root["text"] = text;
    });
  } else if (job->channel == Channel::WHATSAPP) {
    std::string account;
    {
      LockGuard guard(this->lock_);
      account = this->config_(Channel::WHATSAPP).account;
    }
    url = this->graph_url_((account + "/messages").c_str());
    auth = this->bearer_(Channel::WHATSAPP);
    body = json::build_json([chat_id, text](JsonObject root) {
      root["messaging_product"] = "whatsapp";
      root["recipient_type"] = "individual";
      root["to"] = to_string(chat_id);
      root["type"] = "text";
      root["text"].to<JsonObject>()["body"] = text;
    });
  } else {
    url = this->graph_url_("me/messages");
    auth = this->bearer_(Channel::FACEBOOK);
    body = json::build_json([chat_id, text](JsonObject root) {
      root["recipient"].to<JsonObject>()["id"] = to_string(chat_id);
      root["messaging_type"] = "RESPONSE";
      root["message"].to<JsonObject>()["text"] = text;
    });
  }
  std::string answer;
  const int status = this->request_(url, "POST", body, "application/json", auth, REQUEST_TIMEOUT_MS, answer, error);
  if (!this->check_answer_(job->channel, status, answer, error))
    return false;
  this->add_history_(job->channel, job->chat_id, "", job->text, true, 0);
  return true;
}

bool Messenger::run_file_(Job *job, std::string *error) {
  if (job->body.empty()) {
    *error = "no data";
    return false;
  }
  const std::string content_type = std::string("multipart/form-data; boundary=") + BOUNDARY;
  std::string url;
  std::string auth;
  std::string account;
  if (job->channel == Channel::TELEGRAM) {
    const char *field = nullptr;
    url = this->telegram_url_(telegram_method(job->file_kind, &field));
  } else if (job->channel == Channel::WHATSAPP) {
    {
      LockGuard guard(this->lock_);
      account = this->config_(Channel::WHATSAPP).account;
    }
    url = this->graph_url_((account + "/media").c_str());
    auth = this->bearer_(Channel::WHATSAPP);
  } else {
    url = this->graph_url_("me/messages");
    auth = this->bearer_(Channel::FACEBOOK);
  }
  std::string answer;
  int status = this->request_(url, "POST", job->body, content_type.c_str(), auth, UPLOAD_TIMEOUT_MS, answer, error);
  job->body.clear();
  job->body.shrink_to_fit();
  if (!this->check_answer_(job->channel, status, answer, error))
    return false;
  if (job->channel == Channel::WHATSAPP) {
    std::string media_id;
    json::parse_json(answer, [&media_id](JsonObject root) -> bool {
      media_id = root["id"] | "";
      return true;
    });
    if (media_id.empty()) {
      *error = "no media id";
      return false;
    }
    const int64_t chat_id = job->chat_id;
    const char *type = graph_type(job->channel, job->file_kind);
    const char *caption = job->text;
    const char *name = job->name;
    const FileKind kind = job->file_kind;
    const std::string body = json::build_json([&](JsonObject root) {
      root["messaging_product"] = "whatsapp";
      root["recipient_type"] = "individual";
      root["to"] = to_string(chat_id);
      root["type"] = type;
      JsonObject media = root[type].to<JsonObject>();
      media["id"] = media_id;
      if (kind == FileKind::VOICE)
        media["voice"] = true;
      if (caption[0] != '\0' && strcmp(type, "audio") != 0)
        media["caption"] = caption;
      if (strcmp(type, "document") == 0)
        media["filename"] = name;
    });
    answer.clear();
    status = this->request_(this->graph_url_((account + "/messages").c_str()), "POST", body, "application/json", auth,
                            REQUEST_TIMEOUT_MS, answer, error);
    if (!this->check_answer_(job->channel, status, answer, error))
      return false;
  } else if (job->channel == Channel::FACEBOOK && job->text[0] != '\0') {
    const int64_t chat_id = job->chat_id;
    const char *caption = job->text;
    const std::string body = json::build_json([chat_id, caption](JsonObject root) {
      root["recipient"].to<JsonObject>()["id"] = to_string(chat_id);
      root["messaging_type"] = "RESPONSE";
      root["message"].to<JsonObject>()["text"] = caption;
    });
    answer.clear();
    status = this->request_(url, "POST", body, "application/json", auth, REQUEST_TIMEOUT_MS, answer, error);
    if (!this->check_answer_(job->channel, status, answer, error))
      return false;
  }
  std::string line = "[";
  line += file_kind_name(job->file_kind);
  line += "] ";
  line += job->text[0] != '\0' ? job->text : job->name;
  this->add_history_(job->channel, job->chat_id, "", line.c_str(), true, 0);
  return true;
}

#ifdef USE_MESSENGER_STORAGE
bool Messenger::send_file(Channel channel, int64_t chat_id, const std::string &path, FileKind kind,
                          const std::string &caption) {
  if (!this->is_ready(channel) || (chat_id == 0 && !this->is_account(channel)) || path.empty())
    return false;
  if (!this->direct_(channel) && !this->is_account(channel)) {
    this->outbound_(channel, chat_id, kind, caption, path);
    return true;
  }
  if (this->upload_job_ != nullptr || storage::global_storage_registry == nullptr ||
      storage::global_storage_worker == nullptr)
    return false;
  const char *rel = nullptr;
  storage::PathStorage *ps = storage::global_storage_registry->resolve_path(path.c_str(), &rel);
  if (ps == nullptr || rel == nullptr || storage::global_storage_worker->has_active_task_io(ps)) {
    ESP_LOGW(TAG, "'%s' is not available now", path.c_str());
    return false;
  }
  storage::FileStat st{};
  if (ps->stat(rel, &st) != storage::StorageError::STORAGE_ERROR_OK || st.is_dir || st.size == 0) {
    ESP_LOGW(TAG, "'%s' is not a file", path.c_str());
    return false;
  }
  if (st.size > this->max_upload_) {
    ESP_LOGW(TAG, "'%s' is larger than the upload limit (%u B)", path.c_str(),
             static_cast<unsigned>(this->max_upload_));
    return false;
  }
  Job *job = new_job_(JobKind::FILE);
  if (job == nullptr)
    return false;
  job->channel = channel;
  job->chat_id = chat_id;
  job->file_kind = kind;
  job->spool = this->is_account(channel);
  copy_str(job->text, sizeof(job->text), caption.c_str());
  copy_str(job->path, sizeof(job->path), path.c_str());
  const char *slash = strrchr(job->path, '/');
  copy_str(job->name, sizeof(job->name), slash != nullptr ? slash + 1 : job->path);
  if (!this->begin_body_(job, static_cast<size_t>(st.size))) {
    free_job_(job);
    return false;
  }
  this->upload_offset_ = job->body.size();
  this->upload_size_ = static_cast<size_t>(st.size);
  this->upload_got_ = 0;
  job->body.resize(this->upload_offset_ + this->upload_size_);
  this->upload_job_ = job;
  const storage::StorageError err =
      storage::global_storage_worker->begin_read(ps, rel, &this->upload_stream_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->finish_upload_read_(false, storage::error_to_string(e));
          return;
        }
        this->read_upload_step_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK) {
    this->upload_job_ = nullptr;
    free_job_(job);
    return false;
  }
  this->upload_stream_open_ = true;
  return true;
}

void Messenger::read_upload_step_() {
  Job *job = this->upload_job_;
  const size_t want = std::min(UPLOAD_READ_CHUNK, this->upload_size_ - this->upload_got_);
  if (want == 0) {
    this->finish_upload_read_(true, nullptr);
    return;
  }
  this->upload_read_ = 0;
  uint8_t *dst = reinterpret_cast<uint8_t *>(&job->body[this->upload_offset_ + this->upload_got_]);
  const storage::StorageError err = storage::global_storage_worker->read_chunk(
      this->upload_stream_, dst, want, &this->upload_read_, [this](storage::StorageError e) {
        if (e != storage::StorageError::STORAGE_ERROR_OK) {
          this->finish_upload_read_(false, storage::error_to_string(e));
          return;
        }
        if (this->upload_read_ == 0) {
          this->finish_upload_read_(false, "file shorter than its size");
          return;
        }
        this->upload_got_ += this->upload_read_;
        this->read_upload_step_();
      });
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->finish_upload_read_(false, storage::error_to_string(err));
}

void Messenger::finish_upload_read_(bool ok, const char *error) {
  if (this->upload_stream_open_) {
    storage::global_storage_worker->end_read(this->upload_stream_, [](storage::StorageError) {});
    this->upload_stream_open_ = false;
  }
  Job *job = this->upload_job_;
  this->upload_job_ = nullptr;
  if (!ok) {
    ESP_LOGW(TAG, "Reading '%s' failed: %s", job->path, error != nullptr ? error : "");
    this->sent_trigger_.trigger(std::string(channel_name(job->channel)), std::string(file_kind_name(job->file_kind)),
                                std::string(error != nullptr ? error : ""));
    free_job_(job);
    return;
  }
  this->end_body_(job);
  this->queue_job_(job);
}

bool Messenger::download_file(Channel channel, const std::string &file_ref, const std::string &path) {
  if (!this->is_enabled(channel) || this->is_failed() || file_ref.empty() || path.empty())
    return false;
  Job *job = new_job_(JobKind::DOWNLOAD);
  if (job == nullptr)
    return false;
  job->channel = channel;
  copy_str(job->ref, sizeof(job->ref), file_ref.c_str());
  copy_str(job->path, sizeof(job->path), path.c_str());
  return this->queue_job_(job);
}

bool Messenger::storage_io_(IoOp op, const uint8_t *data, size_t len, const char *path, std::string *error) {
  this->io_op_ = op;
  this->io_data_ = data;
  this->io_len_ = len;
  this->io_path_ = path;
  this->io_result_.store(-1, std::memory_order_release);
  this->io_abandoned_.store(false, std::memory_order_release);
  ulTaskNotifyTake(pdTRUE, 0);
  post_to_loop(this, 0, [this]() { this->run_io_(); });
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IO_WAIT_MS)) == 0) {
    this->io_abandoned_.store(true, std::memory_order_release);
    *error = "storage did not answer";
    return false;
  }
  const int result = this->io_result_.load(std::memory_order_acquire);
  if (result != 0) {
    *error = storage::error_to_string(static_cast<storage::StorageError>(result));
    return false;
  }
  return true;
}

void Messenger::io_done_(storage::StorageError err) {
  if (this->io_abandoned_.load(std::memory_order_acquire))
    return;
  this->io_result_.store(static_cast<int>(err), std::memory_order_release);
  xTaskNotifyGive(this->send_task_.get_handle());
}

void Messenger::run_io_() {
  storage::StorageError err = storage::StorageError::STORAGE_ERROR_OK;
  if (this->io_op_ == IoOp::OPEN) {
    const char *rel = nullptr;
    storage::PathStorage *ps = storage::global_storage_registry == nullptr
                                   ? nullptr
                                   : storage::global_storage_registry->resolve_path(this->io_path_, &rel);
    if (ps == nullptr || rel == nullptr || storage::global_storage_worker == nullptr) {
      this->io_done_(storage::StorageError::STORAGE_ERROR_NOT_FOUND);
      return;
    }
    const char *slash = strrchr(rel, '/');
    if (slash != nullptr && slash != rel && !storage::global_storage_worker->has_active_task_io(ps)) {
      const std::string parent(rel, slash - rel);
      ps->mkdir(parent.c_str());
    }
    err = storage::global_storage_worker->begin_write(ps, rel, &this->download_stream_,
                                                      [this](storage::StorageError e) { this->io_done_(e); });
  } else if (this->io_op_ == IoOp::WRITE) {
    err = storage::global_storage_worker->write_chunk(this->download_stream_, this->io_data_, this->io_len_,
                                                      [this](storage::StorageError e) { this->io_done_(e); });
  } else {
    err = storage::global_storage_worker->end_write(this->download_stream_,
                                                    [this](storage::StorageError e) { this->io_done_(e); });
  }
  if (err != storage::StorageError::STORAGE_ERROR_OK)
    this->io_done_(err);
}

bool Messenger::stream_to_storage_(const std::string &url, const std::string &auth, const char *path,
                                   std::string *error) {
  std::vector<http_request::Header> headers;
  if (!auth.empty())
    headers.push_back({"Authorization", auth});
  auto container = this->http_->start(url.c_str(), "GET", "", headers, std::vector<std::string>{}, REQUEST_TIMEOUT_MS);
  if (container == nullptr || container->status_code != 200) {
    if (container != nullptr)
      container->end();
    *error = "download refused";
    return false;
  }
  uint8_t *chunk = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(DOWNLOAD_CHUNK);
  if (chunk == nullptr) {
    container->end();
    *error = "no PSRAM";
    return false;
  }
  bool ok = this->storage_io_(IoOp::OPEN, nullptr, 0, path, error);
  const bool opened = ok;
  uint32_t last_data = millis();
  size_t filled = 0;
  while (ok) {
    const int got = container->read(chunk + filled, DOWNLOAD_CHUNK - filled);
    const auto result =
        http_request::http_read_loop_result(got, last_data, REQUEST_TIMEOUT_MS, container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    if (result == http_request::HttpReadLoopResult::ERROR || result == http_request::HttpReadLoopResult::TIMEOUT) {
      *error = "download interrupted";
      ok = false;
      break;
    }
    if (result == http_request::HttpReadLoopResult::DATA) {
      filled += static_cast<size_t>(got);
      if (filled < DOWNLOAD_CHUNK)
        continue;
    }
    if (filled > 0) {
      ok = this->storage_io_(IoOp::WRITE, chunk, filled, nullptr, error);
      filled = 0;
    }
    if (result == http_request::HttpReadLoopResult::COMPLETE)
      break;
  }
  container->end();
  if (opened && !this->io_abandoned_.load(std::memory_order_acquire)) {
    std::string close_error;
    if (!this->storage_io_(IoOp::CLOSE, nullptr, 0, nullptr, &close_error) && ok) {
      *error = close_error;
      ok = false;
    }
  }
  if (!this->io_abandoned_.load(std::memory_order_acquire))
    RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(chunk, DOWNLOAD_CHUNK);
  return ok;
}

bool Messenger::run_download_(Job *job, std::string *error) {
#ifdef USE_MESSENGER_ACCOUNT
  if (this->is_account(job->channel) && strncmp(job->ref, "tgview:", 7) == 0)
    return this->run_account_view_(job, error);
  if (this->is_account(job->channel) && strncmp(job->ref, "tgavatar:", 9) == 0)
    return this->run_account_avatar_(job, error);
  if (this->is_account(job->channel) && strncmp(job->ref, "tgthumb:", 8) == 0)
    return this->run_account_thumb_(job, error);
  if (this->is_account(job->channel))
    return this->run_account_download_(job, error);
#endif
  const std::string ref = job->ref;
  if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0)
    return this->stream_to_storage_(ref, "", job->path, error);
  if (!this->direct_(job->channel)) {
    *error = "no URL for the file";
    return false;
  }
  std::string answer;
  if (job->channel == Channel::TELEGRAM) {
    std::string url = this->telegram_url_("getFile");
    url += "?file_id=";
    url += ref;
    const int status = this->request_(url, "GET", "", nullptr, "", REQUEST_TIMEOUT_MS, answer, error);
    if (!this->check_answer_(Channel::TELEGRAM, status, answer, error))
      return false;
    std::string file_path;
    json::parse_json(answer, [&file_path](JsonObject root) -> bool {
      file_path = root["result"]["file_path"] | "";
      return true;
    });
    if (file_path.empty()) {
      *error = "no file path";
      return false;
    }
    url = TELEGRAM_HOST;
    url += "/file/bot";
    {
      LockGuard guard(this->lock_);
      url += this->config_(Channel::TELEGRAM).token;
    }
    url += '/';
    url += file_path;
    return this->stream_to_storage_(url, "", job->path, error);
  }
  if (job->channel == Channel::WHATSAPP) {
    std::string account;
    {
      LockGuard guard(this->lock_);
      account = this->config_(Channel::WHATSAPP).account;
    }
    const std::string auth = this->bearer_(Channel::WHATSAPP);
    const int status = this->request_(this->graph_url_((ref + "?phone_number_id=" + account).c_str()), "GET", "",
                                      nullptr, auth, REQUEST_TIMEOUT_MS, answer, error);
    if (!this->check_answer_(Channel::WHATSAPP, status, answer, error))
      return false;
    std::string media_url;
    json::parse_json(answer, [&media_url](JsonObject root) -> bool {
      media_url = root["url"] | "";
      return true;
    });
    if (media_url.empty()) {
      *error = "no media URL";
      return false;
    }
    return this->stream_to_storage_(media_url, auth, job->path, error);
  }
  *error = "no URL for the file";
  return false;
}

#ifdef USE_MESSENGER_ACCOUNT
void Messenger::save_sent_(Job *job) {
  if (this->save_dir_[0] == '\0' || job->path[0] != '\0' || job->body.empty())
    return;
  char stamp[24];
  const time_t now = ::time(nullptr);
  struct tm local {};
  if (now > 1600000000 && localtime_r(&now, &local) != nullptr) {
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  } else {
    snprintf(stamp, sizeof(stamp), "%" PRIu32, static_cast<uint32_t>(millis()));
  }
  char path[sizeof(this->save_dir_) + 48];
  snprintf(path, sizeof(path), "%s/sent_%s%s", this->save_dir_, stamp,
           job->file_kind == FileKind::VOICE ? ".ogg" : job->file_kind == FileKind::PHOTO ? ".jpg" : ".bin");
  const uint8_t *data = reinterpret_cast<const uint8_t *>(job->body.data());
  const size_t len = job->body.size();
  std::string error;
  bool ok = this->storage_io_(IoOp::OPEN, nullptr, 0, path, &error);
  const bool opened = ok;
  for (size_t at = 0; ok && at < len; at += DOWNLOAD_CHUNK)
    ok = this->storage_io_(IoOp::WRITE, data + at, std::min(DOWNLOAD_CHUNK, len - at), nullptr, &error);
  if (opened && !this->io_abandoned_.load(std::memory_order_acquire)) {
    std::string close_error;
    this->storage_io_(IoOp::CLOSE, nullptr, 0, nullptr, &close_error);
  }
  if (this->io_abandoned_.load(std::memory_order_acquire))
    new std::string(std::move(job->body));
  if (!ok)
    ESP_LOGW(TAG, "Not saved to %s: %s", path, error.c_str());
}

bool Messenger::load_avatar(int64_t peer_id) {
  if (!this->is_ready(Channel::TELEGRAM) || !this->is_account(Channel::TELEGRAM) || peer_id == 0)
    return false;
  Job *job = new_job_(JobKind::DOWNLOAD);
  if (job == nullptr)
    return false;
  job->channel = Channel::TELEGRAM;
  snprintf(job->ref, sizeof(job->ref), "tgavatar:%lld", static_cast<long long>(peer_id));
  return this->queue_job_(job);
}

void Messenger::avatar_release() {
  heap_caps_free(this->avatar_);
  this->avatar_ = nullptr;
  this->avatar_len_ = 0;
}

bool Messenger::run_account_avatar_(Job *job, std::string *error) {
  const int64_t peer_id = static_cast<int64_t>(strtoll(job->ref + 9, nullptr, 10));
  AccountPeer peer{};
  int64_t photo_id = 0;
  int32_t dc = 0;
  uint8_t *data = nullptr;
  size_t length = 0;
  bool ok = this->account_.photo_location(peer_id, &peer, &photo_id, &dc);
  if (ok) {
    data = static_cast<uint8_t *>(heap_caps_malloc(AVATAR_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ok = data != nullptr;
  }
  while (ok && length + TelegramAccount::DOWNLOAD_PART <= AVATAR_MAX_BYTES) {
    size_t got = 0;
    ok = this->account_.download_peer_photo_part(peer, photo_id, dc, static_cast<int64_t>(length), data + length,
                                                 TelegramAccount::DOWNLOAD_PART, &got, error);
    length += got;
    if (got < TelegramAccount::DOWNLOAD_PART)
      break;
  }
  this->account_.end_download();
  if (!ok || length == 0) {
    heap_caps_free(data);
    post_to_loop(this, 0, [this, peer_id]() { this->avatar_trigger_.trigger(peer_id, false); });
    error->clear();
    return true;
  }
  post_to_loop(this, 0, [this, data, length, peer_id]() {
    this->avatar_release();
    this->avatar_ = data;
    this->avatar_len_ = length;
    this->avatar_trigger_.trigger(peer_id, true);
  });
  return true;
}

bool Messenger::view_account_photo(int32_t message_id) {
  if (!this->is_ready(Channel::TELEGRAM) || !this->is_account(Channel::TELEGRAM))
    return false;
  Job *job = new_job_(JobKind::DOWNLOAD);
  if (job == nullptr)
    return false;
  job->channel = Channel::TELEGRAM;
  snprintf(job->ref, sizeof(job->ref), "tgview:%ld", static_cast<long>(message_id));
  return this->queue_job_(job);
}

bool Messenger::run_account_view_(Job *job, std::string *error) {
  AccountFile file{};
  if (!this->account_.file_of(static_cast<int32_t>(strtol(job->ref + 7, nullptr, 10)), &file) ||
      file.thumb[0] == '\0') {
    *error = "picture no longer in the chat";
    return false;
  }
  memcpy(file.thumb, file.view_thumb, sizeof(file.thumb));
  file.size = file.view_size;
  uint8_t *data = nullptr;
  size_t length = 0;
  if (!this->fetch_photo_size_(file, VIEW_MAX_BYTES, &data, &length, error))
    return false;
  post_to_loop(this, 0, [this, data, length]() {
    this->photo_release();
    this->photo_ = data;
    this->photo_len_ = length;
    this->photo_trigger_.trigger();
  });
  return true;
}

bool Messenger::fetch_photo_size_(AccountFile &file, size_t max_bytes, uint8_t **data, size_t *length,
                                  std::string *error) {
  if (file.size == 0 || file.size > max_bytes) {
    *error = "picture too large";
    return false;
  }
  const size_t capacity = (file.size + JPEG_STREAM_SLACK + TelegramAccount::DOWNLOAD_PART - 1) /
                          TelegramAccount::DOWNLOAD_PART * TelegramAccount::DOWNLOAD_PART;
  auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer == nullptr) {
    *error = "no PSRAM";
    return false;
  }
  size_t got_total = 0;
  bool ok = true;
  while (ok && got_total + TelegramAccount::DOWNLOAD_PART <= capacity) {
    size_t got = 0;
    ok = this->account_.download_part(file, static_cast<int64_t>(got_total), buffer + got_total,
                                      TelegramAccount::DOWNLOAD_PART, &got, error);
    got_total += got;
    if (got < TelegramAccount::DOWNLOAD_PART)
      break;
  }
  this->account_.end_download();
  if (!ok || got_total == 0) {
    heap_caps_free(buffer);
    if (ok)
      *error = "empty picture";
    return false;
  }
  *data = buffer;
  *length = got_total;
  return true;
}

bool Messenger::load_thumb(int32_t message_id) {
  if (!this->is_ready(Channel::TELEGRAM) || !this->is_account(Channel::TELEGRAM))
    return false;
  Job *job = new_job_(JobKind::DOWNLOAD);
  if (job == nullptr)
    return false;
  job->channel = Channel::TELEGRAM;
  snprintf(job->ref, sizeof(job->ref), "tgthumb:%ld", static_cast<long>(message_id));
  return this->queue_job_(job);
}

void Messenger::thumb_release() {
  heap_caps_free(this->thumb_);
  this->thumb_ = nullptr;
  this->thumb_len_ = 0;
}

bool Messenger::run_account_thumb_(Job *job, std::string *error) {
  const int32_t message_id = static_cast<int32_t>(strtol(job->ref + 8, nullptr, 10));
  AccountFile file{};
  uint8_t *data = nullptr;
  size_t length = 0;
  bool ok = this->account_.file_of(message_id, &file) && file.small_thumb[0] != '\0';
  if (ok) {
    memcpy(file.thumb, file.small_thumb, sizeof(file.thumb));
    file.size = file.small_size;
    ok = this->fetch_photo_size_(file, THUMB_MAX_BYTES, &data, &length, error);
  }
  error->clear();
  if (!ok) {
    post_to_loop(this, 0, [this, message_id]() { this->thumb_trigger_.trigger(message_id, false); });
    return true;
  }
  post_to_loop(this, 0, [this, data, length, message_id]() {
    this->thumb_release();
    this->thumb_ = data;
    this->thumb_len_ = length;
    this->thumb_trigger_.trigger(message_id, true);
  });
  return true;
}

bool Messenger::run_account_download_(Job *job, std::string *error) {
  AccountFile file{};
  if (strncmp(job->ref, "tg:", 3) != 0 ||
      !this->account_.file_of(static_cast<int32_t>(strtol(job->ref + 3, nullptr, 10)), &file)) {
    *error = "file no longer in the chat";
    return false;
  }
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  uint8_t *part = allocator.allocate(TelegramAccount::DOWNLOAD_PART);
  if (part == nullptr) {
    *error = "no PSRAM";
    return false;
  }
  bool ok = this->storage_io_(IoOp::OPEN, nullptr, 0, job->path, error);
  const bool opened = ok;
  int64_t offset = 0;
  while (ok) {
    size_t got = 0;
    ok = this->account_.download_part(file, offset, part, TelegramAccount::DOWNLOAD_PART, &got, error);
    for (size_t at = 0; ok && at < got; at += DOWNLOAD_CHUNK)
      ok = this->storage_io_(IoOp::WRITE, part + at, std::min(DOWNLOAD_CHUNK, got - at), nullptr, error);
    if (!ok || got < TelegramAccount::DOWNLOAD_PART)
      break;
    offset += static_cast<int64_t>(got);
  }
  this->account_.end_download();
  if (opened && !this->io_abandoned_.load(std::memory_order_acquire)) {
    std::string close_error;
    if (!this->storage_io_(IoOp::CLOSE, nullptr, 0, nullptr, &close_error) && ok) {
      *error = close_error;
      ok = false;
    }
  }
  if (!this->io_abandoned_.load(std::memory_order_acquire))
    allocator.deallocate(part, TelegramAccount::DOWNLOAD_PART);
  return ok;
}
#endif

bool Messenger::run_spool_(Job *job, std::string *error) {
  if (this->spool_dir_[0] == '\0') {
    *error = "no spool_dir for Home Assistant";
    return false;
  }
  const char *dot = strrchr(job->name, '.');
  snprintf(job->path, sizeof(job->path), "%s/%s-%" PRIu32 "%s", this->spool_dir_, file_kind_name(job->file_kind),
           static_cast<uint32_t>(millis()), dot != nullptr ? dot : "");
  const uint8_t *data = reinterpret_cast<const uint8_t *>(job->body.data());
  const size_t len = job->body.size();
  bool ok = this->storage_io_(IoOp::OPEN, nullptr, 0, job->path, error);
  const bool opened = ok;
  for (size_t at = 0; ok && at < len; at += DOWNLOAD_CHUNK)
    ok = this->storage_io_(IoOp::WRITE, data + at, std::min(DOWNLOAD_CHUNK, len - at), nullptr, error);
  if (opened && !this->io_abandoned_.load(std::memory_order_acquire)) {
    std::string close_error;
    if (!this->storage_io_(IoOp::CLOSE, nullptr, 0, nullptr, &close_error) && ok) {
      *error = close_error;
      ok = false;
    }
  }
  if (this->io_abandoned_.load(std::memory_order_acquire)) {
    new std::string(std::move(job->body));
    return false;
  }
  job->body.clear();
  job->body.shrink_to_fit();
  if (!ok)
    return false;
  const Channel channel = job->channel;
  const int64_t chat_id = job->chat_id;
  const FileKind kind = job->file_kind;
  const std::string caption = job->text;
  const std::string path = job->path;
  post_to_loop(this, 0, [this, channel, chat_id, kind, caption, path]() {
    this->outbound_(channel, chat_id, kind, caption, path);
  });
  return true;
}
#else
bool Messenger::send_file(Channel channel, int64_t chat_id, const std::string &path, FileKind kind,
                          const std::string &caption) {
  if (!this->is_ready(channel) || this->direct_(channel) || chat_id == 0 || path.empty())
    return false;
  this->outbound_(channel, chat_id, kind, caption, path);
  return true;
}
bool Messenger::download_file(Channel channel, const std::string &file_ref, const std::string &path) { return false; }
bool Messenger::run_download_(Job *job, std::string *error) {
  *error = "no storage";
  return false;
}
bool Messenger::run_spool_(Job *job, std::string *error) {
  *error = "no storage";
  return false;
}
#endif

#ifdef USE_MESSENGER_CAMERA
void Messenger::set_camera(camera::Camera *cam) {
  this->camera_ = cam;
  cam->add_listener(this);
}

bool Messenger::send_camera_photo(Channel channel, int64_t chat_id, const std::string &caption) {
  if (!this->is_ready(channel) || (chat_id == 0 && !this->is_account(channel)) || this->camera_ == nullptr ||
      this->camera_job_ != nullptr || this->capture_pending_)
    return false;
  Job *job = new_job_(JobKind::FILE);
  if (job == nullptr)
    return false;
  job->channel = channel;
  job->spool = !this->direct_(channel);
  job->chat_id = chat_id;
  job->file_kind = FileKind::PHOTO;
  copy_str(job->text, sizeof(job->text), caption.c_str());
  copy_str(job->name, sizeof(job->name), "photo.jpg");
  this->camera_job_ = job;
  this->camera_->request_image(camera::WEB_REQUESTER);
  this->set_timeout("msg_camera", CAMERA_WAIT_MS, [this]() {
    if (this->camera_job_ == nullptr)
      return;
    ESP_LOGW(TAG, "No picture from the camera");
    const std::string channel = channel_name(this->camera_job_->channel);
    free_job_(this->camera_job_);
    this->camera_job_ = nullptr;
    this->sent_trigger_.trigger(channel, std::string("photo"), std::string("no picture from the camera"));
  });
  return true;
}

bool Messenger::capture_photo() {
  if (this->camera_ == nullptr || this->camera_job_ != nullptr || this->capture_pending_)
    return false;
  this->capture_pending_ = true;
  this->camera_->request_image(camera::WEB_REQUESTER);
  this->set_timeout("msg_camera", CAMERA_WAIT_MS, [this]() {
    if (!this->capture_pending_)
      return;
    this->capture_pending_ = false;
    ESP_LOGW(TAG, "No picture from the camera");
    this->sent_trigger_.trigger(std::string(), std::string("photo"), std::string("no picture from the camera"));
  });
  return true;
}

void Messenger::on_camera_image(const std::shared_ptr<camera::CameraImage> &image) {
  if (this->capture_pending_ && image->was_requested_by(camera::WEB_REQUESTER)) {
    const size_t len = image->get_data_length();
    if (len == 0)
      return;
    this->capture_pending_ = false;
    this->cancel_timeout("msg_camera");
    auto *copy = static_cast<uint8_t *>(heap_caps_malloc(len + JPEG_STREAM_SLACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (copy == nullptr) {
      this->sent_trigger_.trigger(std::string(), std::string("photo"), std::string("no PSRAM for the picture"));
      return;
    }
    memcpy(copy, image->get_data_buffer(), len);
    this->photo_release();
    this->photo_ = copy;
    this->photo_len_ = len;
    this->photo_trigger_.trigger();
    return;
  }
  Job *job = this->camera_job_;
  if (job == nullptr || !image->was_requested_by(camera::WEB_REQUESTER))
    return;
  const size_t len = image->get_data_length();
  if (len == 0)
    return;
  this->camera_job_ = nullptr;
  this->cancel_timeout("msg_camera");
  if (!this->begin_body_(job, len)) {
    const std::string channel = channel_name(job->channel);
    free_job_(job);
    this->sent_trigger_.trigger(channel, std::string("photo"), std::string("no PSRAM for the picture"));
    return;
  }
  job->body.append(reinterpret_cast<const char *>(image->get_data_buffer()), len);
  this->end_body_(job);
  this->queue_job_(job);
}
#else
bool Messenger::send_camera_photo(Channel channel, int64_t chat_id, const std::string &caption) { return false; }
bool Messenger::capture_photo() { return false; }
#endif

#ifdef USE_MESSENGER_MICROPHONE
void Messenger::set_microphone_source(microphone::MicrophoneSource *source) {
  this->mic_source_ = source;
  source->add_data_callback([this](const std::vector<uint8_t> &data) {
    if (!this->recording_.load(std::memory_order_acquire))
      return;
    ring_buffer::RingBuffer *ring = this->voice_ring_.get();
    if (ring != nullptr)
      ring->write_without_replacement(data.data(), data.size(), 0);
  });
}

bool Messenger::voice_start(Channel channel, int64_t chat_id) {
  if (this->mic_source_ == nullptr || this->voice_busy_.load() || !this->is_ready(channel) ||
      (chat_id == 0 && !this->is_account(channel)))
    return false;
  const audio::AudioStreamInfo info = this->mic_source_->get_audio_stream_info();
  const uint32_t rate = info.get_sample_rate();
  if (info.get_bits_per_sample() != 16 || info.get_channels() != 1 ||
      (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000)) {
    ESP_LOGW(TAG, "The microphone's format does not suit Opus (16 bit mono, 8/12/16/24/48 kHz)");
    return false;
  }
  const size_t ring_bytes = VOICE_RING_SECONDS * rate * 2;
  if (!psram_has(ring_bytes))
    return false;
  this->voice_ring_ = ring_buffer::RingBuffer::create(ring_bytes);
  if (this->voice_ring_ == nullptr)
    return false;
  Job *job = new_job_(JobKind::VOICE);
  if (job == nullptr) {
    this->voice_ring_.reset();
    return false;
  }
  job->file_kind = FileKind::VOICE;
  job->channel = channel;
  job->chat_id = chat_id;
  job->spool = !this->direct_(channel);
  copy_str(job->name, sizeof(job->name), "voice.ogg");
  this->voice_rate_ = rate;
  this->voice_channel_.store(static_cast<uint8_t>(channel));
  this->voice_chat_.store(chat_id);
  this->voice_cancelled_.store(false);
  this->voice_busy_.store(true);
  this->recording_.store(true, std::memory_order_release);
  if (!this->queue_job_(job)) {
    this->recording_.store(false);
    this->voice_busy_.store(false);
    this->voice_ring_.reset();
    return false;
  }
  this->mic_source_->start();
  return true;
}

void Messenger::voice_end_capture_() {
  if (!this->recording_.exchange(false, std::memory_order_acq_rel))
    return;
  this->mic_source_->stop();
  const int16_t wake = 0;
  if (this->voice_ring_ != nullptr)
    this->voice_ring_->write_without_replacement(&wake, sizeof(wake), 0);
}

bool Messenger::voice_stop_and_send() {
  if (!this->recording_.load())
    return false;
  this->voice_end_capture_();
  return true;
}

void Messenger::voice_cancel() {
  if (!this->voice_busy_.load())
    return;
  this->voice_cancelled_.store(true);
  this->voice_end_capture_();
}

void Messenger::voice_release_() {
  post_to_loop(this, VOICE_RELEASE_MS, [this]() {
    this->voice_ring_.reset();
    this->voice_busy_.store(false);
  });
}

static uint32_t ogg_crc(const uint8_t *data, size_t len, uint32_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
  }
  return crc;
}

static void put_le(uint8_t *p, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; i++)
    p[i] = static_cast<uint8_t>(value >> (8 * i));
}

static void append_ogg_page(std::string &out, uint8_t type, uint64_t granule, uint32_t serial, uint32_t sequence,
                            const uint8_t *packet, size_t len) {
  const size_t segments = len / 255 + 1;
  uint8_t header[27 + 255];
  memcpy(header, "OggS", 4);
  header[4] = 0;
  header[5] = type;
  put_le(header + 6, granule, 8);
  put_le(header + 14, serial, 4);
  put_le(header + 18, sequence, 4);
  put_le(header + 22, 0, 4);
  header[26] = static_cast<uint8_t>(segments);
  for (size_t i = 0; i < segments; i++)
    header[27 + i] = i + 1 < segments ? 255 : static_cast<uint8_t>(len % 255);
  const size_t header_len = 27 + segments;
  uint32_t crc = ogg_crc(header, header_len, 0);
  crc = ogg_crc(packet, len, crc);
  put_le(header + 22, crc, 4);
  out.append(reinterpret_cast<const char *>(header), header_len);
  out.append(reinterpret_cast<const char *>(packet), len);
}

bool Messenger::run_voice_(Job *job, std::string *error) {
  const uint32_t rate = this->voice_rate_;
  const size_t frame = rate / 50;
  const size_t frame_bytes = frame * 2;
  const size_t max_frames = static_cast<size_t>(this->max_voice_seconds_) * 50;
  const size_t scratch_bytes = frame_bytes + 2 * OPUS_MAX_PACKET;
  uint8_t *scratch = RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).allocate(scratch_bytes);
  int err = 0;
  OpusEncoder *encoder =
      scratch == nullptr ? nullptr
                         : opus_encoder_create(static_cast<opus_int32>(rate), 1, OPUS_APPLICATION_VOIP, &err);
  const size_t estimate = max_frames * (VOICE_BITRATE / 8 / 50 + 32);
  if (scratch == nullptr || encoder == nullptr || err != OPUS_OK || !this->begin_body_(job, estimate)) {
    if (encoder != nullptr)
      opus_encoder_destroy(encoder);
    if (scratch != nullptr)
      RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(scratch, scratch_bytes);
    post_to_loop(this, 0, [this]() { this->voice_end_capture_(); });
    this->voice_release_();
    *error = "no memory for the voice note";
    return false;
  }
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(VOICE_BITRATE));
  opus_int32 lookahead = 0;
  opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));
  const uint32_t to_48k = 48000 / rate;
  const uint16_t pre_skip = static_cast<uint16_t>(lookahead * to_48k);

  std::string &body = job->body;
  const uint32_t serial = esp_random();
  uint32_t sequence = 0;
  uint8_t head[19];
  memcpy(head, "OpusHead", 8);
  head[8] = 1;
  head[9] = 1;
  put_le(head + 10, pre_skip, 2);
  put_le(head + 12, rate, 4);
  put_le(head + 16, 0, 2);
  head[18] = 0;
  append_ogg_page(body, 0x02, 0, serial, sequence++, head, sizeof(head));
  static const char VENDOR[] = "esphome";
  uint8_t tags[8 + 4 + sizeof(VENDOR) - 1 + 4];
  memcpy(tags, "OpusTags", 8);
  put_le(tags + 8, sizeof(VENDOR) - 1, 4);
  memcpy(tags + 12, VENDOR, sizeof(VENDOR) - 1);
  put_le(tags + 12 + sizeof(VENDOR) - 1, 0, 4);
  append_ogg_page(body, 0x00, 0, serial, sequence++, tags, sizeof(tags));

  uint8_t *pcm = scratch;
  uint8_t *packets[2] = {scratch + frame_bytes, scratch + frame_bytes + OPUS_MAX_PACKET};
  size_t pending_len = 0;
  int pending = -1;
  uint64_t granule = 0;
  size_t frames = 0;
  bool ok = true;
  bool ended = false;
  ring_buffer::RingBuffer *ring = this->voice_ring_.get();
  while (!ended) {
    size_t have = 0;
    while (have < frame_bytes) {
      const bool capturing = this->recording_.load(std::memory_order_acquire);
      const size_t got = ring->read(pcm + have, frame_bytes - have, capturing ? portMAX_DELAY : 0);
      have += got;
      if (got == 0 && !capturing)
        break;
    }
    if (have < frame_bytes) {
      ended = true;
      if (have == 0)
        break;
      memset(pcm + have, 0, frame_bytes - have);
    }
    const int slot = pending == 0 ? 1 : 0;
    const opus_int32 bytes = opus_encode(encoder, reinterpret_cast<const opus_int16 *>(pcm), static_cast<int>(frame),
                                         packets[slot], static_cast<opus_int32>(OPUS_MAX_PACKET));
    if (bytes < 0) {
      *error = "Opus encoding failed";
      ok = false;
      post_to_loop(this, 0, [this]() { this->voice_end_capture_(); });
      while (this->recording_.load(std::memory_order_acquire))
        ring->read(pcm, frame_bytes, portMAX_DELAY);
      break;
    }
    if (pending >= 0)
      append_ogg_page(body, 0x00, granule, serial, sequence++, packets[pending], pending_len);
    granule += static_cast<uint64_t>(frame) * to_48k;
    pending = slot;
    pending_len = static_cast<size_t>(bytes);
    if (++frames >= max_frames && !ended) {
      post_to_loop(this, 0, [this]() { this->voice_end_capture_(); });
      while (this->recording_.load(std::memory_order_acquire))
        ring->read(pcm, frame_bytes, portMAX_DELAY);
      ended = true;
    }
  }
  if (ok && pending >= 0)
    append_ogg_page(body, 0x04, granule, serial, sequence++, packets[pending], pending_len);
  opus_encoder_destroy(encoder);
  RAMAllocator<uint8_t>(RAMAllocator<uint8_t>::ALLOC_EXTERNAL).deallocate(scratch, scratch_bytes);
  this->voice_release_();
  if (this->voice_cancelled_.load()) {
    error->clear();
    return false;
  }
  if (!ok)
    return false;
  if (frames < 10) {
    *error = "recording too short";
    return false;
  }
  job->duration = static_cast<uint32_t>(frames / 50);
  if (this->is_account(job->channel))
    return this->run_account_(job, error);
  if (job->spool)
    return this->run_spool_(job, error);
  if (!network::is_connected()) {
    *error = "no network";
    return false;
  }
  this->end_body_(job);
  return this->run_file_(job, error);
}
#else
bool Messenger::voice_start(Channel channel, int64_t chat_id) { return false; }
bool Messenger::voice_stop_and_send() { return false; }
void Messenger::voice_cancel() {}
bool Messenger::run_voice_(Job *job, std::string *error) {
  *error = "no microphone";
  return false;
}
#endif

}

#endif
