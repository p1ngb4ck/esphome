// Channel list for the network stream: fetched from the svp_relay Home Assistant integration, optionally
// into an LVGL dropdown; channels.play asks the relay to stream one to this panel's stream_port.

#include "simple_video_player.h"

#ifdef SVP_CHANNEL_LIST

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"

#ifdef SVP_SPICE
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#endif

#include <atomic>
#include <cctype>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.channels";

static constexpr size_t CHANNEL_MAX = 300;
static constexpr size_t HA_ITEMS_MAX = 500;  // as the relay sends at most
static constexpr size_t CHANNEL_RESPONSE_MAX = 256 * 1024;
static constexpr uint32_t RELAY_SLOW_TIMEOUT_MS = 15000;  // /desktop: once the desktop is up; /image: once fitted
// /options keyboard_install: answered once svp_relay has downloaded and made the keyboard layouts.
static constexpr uint32_t RELAY_INSTALL_TIMEOUT_MS = 180000;

bool SimpleVideoPlayer::setup_channel_list_() {
  this->channel_q_ = xQueueCreate(8, sizeof(ChannelCmd));
  this->channel_lock_ = xSemaphoreCreateMutex();
  if (this->channel_q_ == nullptr || this->channel_lock_ == nullptr ||
      !create_psram_task(channel_task_entry_, "svp_channels", 8192, this, 1, nullptr, 0)) {
    ESP_LOGE(TAG, "Failed to start the channel list task");
    return false;
  }
  return true;
}

bool SimpleVideoPlayer::channel_request_(uint8_t cmd, uint16_t index) {
  if (this->channel_q_ == nullptr)
    return false;
  const ChannelCmd request{cmd, index};
  return xQueueSend(this->channel_q_, &request, 0) == pdTRUE;
}

// For a query string: everything but unreserved characters percent-encoded.
static std::string url_encoded(const char *text) {
  static const char *const HEX = "0123456789ABCDEF";
  std::string out;
  for (const char *c = text; *c != '\0'; c++) {
    const auto b = static_cast<uint8_t>(*c);
    if (isalnum(b) || b == '-' || b == '_' || b == '.' || b == '~') {
      out += static_cast<char>(b);
    } else {
      out += '%';
      out += HEX[b >> 4];
      out += HEX[b & 0x0F];
    }
  }
  return out;
}

void SimpleVideoPlayer::ha_browse(const std::string &type, const std::string &id) {
  if (this->channel_lock_ == nullptr)
    return;
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->ha_reqs_.emplace_back(PsramString(type.c_str()), PsramString(id.c_str()));
  xSemaphoreGive(this->channel_lock_);
  if (this->channel_request_(CHANNEL_CMD_HA_BROWSE, 0))
    return;
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);  // the task's queue is full: not asked for
  if (!this->ha_reqs_.empty())
    this->ha_reqs_.pop_back();
  xSemaphoreGive(this->channel_lock_);
}

void SimpleVideoPlayer::ha_play(const std::string &id) {
#ifdef SVP_AUDIO_FILES
  this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);  // not the queue's track
#endif
  this->ha_play_request_(id);
}

#if defined(SVP_AUDIO_FILES) && defined(SVP_MEDIA_PLAYER)
void SimpleVideoPlayer::audio_remote_player_(media_player::MediaPlayerState state) {
  const uint8_t remote = this->audio_remote_.load(std::memory_order_acquire);
  if (remote == AUDIO_REMOTE_NONE)
    return;
  // Idle soon after its svp_play_audio: the last track stopping, or the new one not begun yet.
  static constexpr uint32_t END_GUARD_MS = 2000;
  if (remote == AUDIO_REMOTE_ASKED && state == media_player::MEDIA_PLAYER_STATE_PLAYING) {
    // Started without its svp_play_audio seen here.
    this->audio_remote_play_ms_ = millis();
    this->audio_remote_.store(AUDIO_REMOTE_PLAYING, std::memory_order_release);
  } else if (remote == AUDIO_REMOTE_PLAYING && state == media_player::MEDIA_PLAYER_STATE_IDLE &&
      millis() - this->audio_remote_play_ms_ >= END_GUARD_MS) {
    this->audio_advance_req_.store(true, std::memory_order_release);  // played to its end: loop() goes on
  }
  this->audio_changed_.store(true, std::memory_order_release);
}

bool SimpleVideoPlayer::audio_remote_command_(media_player::MediaPlayerCommand command) {
  if (this->media_player_ == nullptr)
    return false;
  if (command == media_player::MEDIA_PLAYER_COMMAND_STOP) {
    // Not the end of the track: the queue stays where it is.
    if (this->audio_remote_.exchange(AUDIO_REMOTE_NONE, std::memory_order_acq_rel) == AUDIO_REMOTE_NONE)
      return false;
    this->audio_advance_req_.store(false, std::memory_order_release);
    this->audio_changed_.store(true, std::memory_order_release);
  } else if (this->audio_remote_.load(std::memory_order_acquire) != AUDIO_REMOTE_PLAYING) {
    return false;
  }
  this->media_player_->make_call().set_command(command).perform();
  return true;
}
#endif

void SimpleVideoPlayer::ha_play_request_(const std::string &id) {
  if (this->channel_lock_ == nullptr || id.empty())
    return;
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->ha_play_id_.assign(id.c_str());
  xSemaphoreGive(this->channel_lock_);
  this->channel_request_(CHANNEL_CMD_HA_PLAY, 0);
}

void SimpleVideoPlayer::relay_options_fetch() { this->channel_request_(CHANNEL_CMD_OPTIONS_GET, 0); }

std::string SimpleVideoPlayer::relay_option(const std::string &key) const {
  for (const auto &value : this->opt_values_) {
    if (key == value.first.c_str())
      return value.second.c_str();
  }
  return "";
}

std::string SimpleVideoPlayer::relay_choices(const std::string &key) const {
  for (const auto &choices : this->opt_choices_) {
    if (key == choices.first.c_str())
      return choices.second.c_str();
  }
  return "";
}

void SimpleVideoPlayer::relay_option_set(const std::string &key, const std::string &value) {
  if (this->channel_lock_ == nullptr)
    return;
  // Shown as set at once; the relay sets the panel up again with it.
  bool known = false;
  for (auto &option : this->opt_values_) {
    if (key == option.first.c_str()) {
      option.second.assign(value.c_str());
      known = true;
    }
  }
  if (!known)
    this->opt_values_.emplace_back(PsramString(key.c_str()), PsramString(value.c_str()));
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->opt_sets_.emplace_back(PsramString(key.c_str()), PsramString(value.c_str()));
  xSemaphoreGive(this->channel_lock_);
  this->channel_request_(CHANNEL_CMD_OPTIONS_SET, 0);
}

#ifndef SVP_PICTURES
bool SimpleVideoPlayer::ha_show_pictures(size_t index) { return false; }
#endif

void SimpleVideoPlayer::channel_play() {
  this->channel_play(this->channel_widget_ != nullptr ? static_cast<int>(this->channel_widget_->get_selected_index())
                                                      : this->channel_index_);
}

void SimpleVideoPlayer::channel_play(int index) {
  if (index < 0 || static_cast<size_t>(index) >= this->channel_names_.size())
    return;
  this->channel_index_ = index;
#ifdef SVP_AUDIO_FILES
  this->audio_remote_.store(AUDIO_REMOTE_NONE, std::memory_order_release);
#endif
  this->channel_request_(CHANNEL_CMD_PLAY, static_cast<uint16_t>(index));
}

void SimpleVideoPlayer::channel_loop_() {
  if (!this->channel_requested_once_ && this->channel_q_ != nullptr && network::is_connected()) {
    this->channel_requested_once_ = true;
    this->channel_request_(CHANNEL_CMD_REFRESH, 0);
  }
#ifdef SVP_USE_LVGL
  // The dropdown's options are LVGL's: a list that came while LVGL is paused waits for it to run.
  if (this->channel_widget_ != nullptr && this->lvgl_component_ != nullptr && this->lvgl_component_->is_paused())
    return;
#endif
  if (!this->channel_new_.exchange(false, std::memory_order_acq_rel))
    return;
  // LVGL keeps pointers into channel_names_: swap the lists and set the options in one go, here on
  // the LVGL thread.
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->channel_names_.swap(this->channel_new_names_);
  this->channel_refs_.swap(this->channel_new_refs_);
  if (this->channel_widget_ != nullptr) {
    FixedVector<const char *> options;
    options.init(this->channel_names_.empty() ? 1 : this->channel_names_.size());
    if (this->channel_names_.empty())
      options.push_back("(no channels)");
    for (const auto &name : this->channel_names_)
      options.push_back(name.c_str());
    this->channel_widget_->set_options(std::move(options));
  }
  xSemaphoreGive(this->channel_lock_);
  if (static_cast<size_t>(this->channel_index_ + 1) > this->channel_names_.size())
    this->channel_index_ = -1;
  this->channel_version_++;
  this->channels_callbacks_.call();
}

void SimpleVideoPlayer::ha_loop_() {
  if (this->ha_new_src_.exchange(false, std::memory_order_acq_rel)) {
    PsramVector<HaSources> listed;
    xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
    listed.swap(this->ha_new_srcs_);
    xSemaphoreGive(this->channel_lock_);
    for (auto &sources : listed) {
      this->ha_src_items_ = std::move(sources.items);
      this->ha_src_type_ = std::move(sources.type);
      this->ha_src_ok_ = sources.ok;
      this->ha_last_sources_ = true;
      this->ha_callbacks_.call();
    }
  }
  if (this->ha_new_.exchange(false, std::memory_order_acq_rel)) {
    xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
    this->ha_items_.swap(this->ha_new_items_);
    this->ha_type_.swap(this->ha_new_type_);
    this->ha_id_.swap(this->ha_new_id_);
    this->ha_title_.swap(this->ha_new_title_);
    this->ha_path_.swap(this->ha_new_path_);
    this->ha_parent_.swap(this->ha_new_parent_);
    this->ha_ok_ = this->ha_new_ok_;
    xSemaphoreGive(this->channel_lock_);
    this->ha_last_sources_ = false;
    this->ha_callbacks_.call();
  }
  if (this->opt_new_.exchange(false, std::memory_order_acq_rel)) {
    xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
    if (this->opt_new_ok_) {  // else what was read before stays shown
      this->opt_values_.swap(this->opt_new_values_);
      this->opt_choices_.swap(this->opt_new_choices_);
    }
    this->opt_ok_ = this->opt_new_ok_;
    xSemaphoreGive(this->channel_lock_);
    this->opt_callbacks_.call();
  }
}

void SimpleVideoPlayer::channel_task_entry_(void *param) { static_cast<SimpleVideoPlayer *>(param)->channel_task_loop_(); }

void SimpleVideoPlayer::channel_task_loop_() {
  ChannelCmd request;
  while (true) {
    if (xQueueReceive(this->channel_q_, &request, portMAX_DELAY) != pdTRUE)
      continue;

    if (request.cmd == CHANNEL_CMD_REFRESH) {
      char *body = nullptr;
      size_t len = 0;
      if (!this->channel_http_("/channels?refresh=1", nullptr, &body, &len))
        continue;
      PsramVector<PsramString> names, refs;
      const bool ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
        JsonArray channels = root["channels"];
        for (JsonObject channel : channels) {
          if (names.size() >= CHANNEL_MAX)
            break;
          const char *name = channel["name"] | "";
          const char *ref = channel["ref"] | "";
          if (*ref == '\0')
            continue;
          names.emplace_back(name);
          refs.emplace_back(ref);
        }
        return true;
      });
      heap_caps_free(body);
      if (!ok) {
        ESP_LOGW(TAG, "Channel list is not valid JSON");
        continue;
      }
      ESP_LOGI(TAG, "%u channels", static_cast<unsigned>(names.size()));
      xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
      this->channel_new_names_ = std::move(names);
      this->channel_new_refs_ = std::move(refs);
      xSemaphoreGive(this->channel_lock_);
      this->channel_new_.store(true, std::memory_order_release);
      continue;
    }

    if (request.cmd == CHANNEL_CMD_PLAY) {
      std::string ref;
      xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
      if (request.index < this->channel_refs_.size())
        ref = this->channel_refs_[request.index].c_str();
      xSemaphoreGive(this->channel_lock_);
      if (ref.empty())
        continue;
      const std::string payload = json::build_json([&](JsonObject root) {
        root["ref"] = ref;
        root["port"] = this->stream_port_;
      });
      ESP_LOGI(TAG, "Requesting %s", ref.c_str());
      this->channel_http_("/play", payload.c_str(), nullptr, nullptr);
      continue;
    }

    if (request.cmd == CHANNEL_CMD_STOP) {
      this->channel_http_("/stop", "{}", nullptr, nullptr);
    } else if (request.cmd == CHANNEL_CMD_HA_BROWSE) {
      this->ha_browse_task_();
    } else if (request.cmd == CHANNEL_CMD_HA_PLAY) {
      xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
      const std::string id = this->ha_play_id_.c_str();
      xSemaphoreGive(this->channel_lock_);
      const std::string payload = json::build_json([&](JsonObject root) {
        root["ref"] = id;
        root["port"] = this->stream_port_;
      });
      this->channel_http_("/play", payload.c_str(), nullptr, nullptr);
#ifdef SVP_PICTURES
    } else if (request.cmd == CHANNEL_CMD_IMAGE) {
      this->image_task_();
#endif
    } else if (request.cmd == CHANNEL_CMD_OPTIONS_GET) {
      this->options_get_task_();
    } else if (request.cmd == CHANNEL_CMD_OPTIONS_SET) {
      this->options_set_task_();
#ifdef SVP_AUDIO_COVER
    } else if (request.cmd == CHANNEL_CMD_COVER) {
      this->cover_task_();
#endif
#ifdef SVP_KEYBOARD
    } else if (request.cmd == CHANNEL_CMD_KEYBOARD_LIST) {
      this->keyboard_list_task_();
    } else if (request.cmd == CHANNEL_CMD_KEYBOARD_GET) {
      this->keyboard_get_task_();
#endif
    }
  }
}

// Channel task: the sources of a type, or a folder.
void SimpleVideoPlayer::ha_browse_task_() {
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  if (this->ha_reqs_.empty()) {
    xSemaphoreGive(this->channel_lock_);
    return;
  }
  const std::string type = this->ha_reqs_.front().first.c_str();
  const std::string id = this->ha_reqs_.front().second.c_str();
  this->ha_reqs_.erase(this->ha_reqs_.begin());
  xSemaphoreGive(this->channel_lock_);
  std::string path = "/browse?type=" + url_encoded(type.c_str());
  if (!id.empty())
    path += "&id=" + url_encoded(id.c_str());
  char *body = nullptr;
  size_t len = 0;
  PsramVector<HaItem> items;
  PsramString title, where, parent;
  bool ok = this->channel_http_(path.c_str(), nullptr, &body, &len);
  if (ok) {
    ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
      title.assign(root["title"] | "");
      where.assign(root["path"] | "");
      parent.assign(root["parent"] | "");
      JsonArray list = root["items"].as<JsonArray>();
      for (JsonObject item : list) {
        if (items.size() >= HA_ITEMS_MAX)
          break;
        const char *item_id = item["id"] | "";
        if (*item_id == '\0')
          continue;
        items.push_back(HaItem{PsramString(item["title"] | ""), PsramString(item_id), item["dir"] | false});
      }
      return true;
    });
    heap_caps_free(body);
  }
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  if (id.empty()) {
    this->ha_new_srcs_.push_back(HaSources{PsramString(type.c_str()), std::move(items), ok});
  } else {
    this->ha_new_items_ = std::move(items);
    this->ha_new_type_.assign(type.c_str());
    this->ha_new_id_.assign(id.c_str());
    this->ha_new_title_ = std::move(title);
    this->ha_new_path_ = std::move(where);
    this->ha_new_parent_ = std::move(parent);
    this->ha_new_ok_ = ok;
  }
  xSemaphoreGive(this->channel_lock_);
  (id.empty() ? this->ha_new_src_ : this->ha_new_).store(true, std::memory_order_release);
}

#ifdef SVP_PICTURES
bool SimpleVideoPlayer::remote_image_(const ImageEntry &entry) { return entry.path.rfind("media-source://", 0) == 0; }

// Channel task: the picture of pic_read_ into its file, fitted by the relay to the display and to the
// file's room; pic_read_ stays the main loop's but for what is set here before done.
void SimpleVideoPlayer::image_task_() {
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  const std::string id = this->pic_remote_id_.c_str();
  xSemaphoreGive(this->channel_lock_);
  PictureRead &read = this->pic_read_;
  char query[64];
  snprintf(query, sizeof(query), "&w=%d&h=%d&max=%u", static_cast<int>(this->pic_remote_w_),
           static_cast<int>(this->pic_remote_h_), static_cast<unsigned>(read.limit - 1));
  const std::string path = "/image?id=" + url_encoded(id.c_str()) + query;
  size_t len = 0;
  const bool ok =
      this->channel_http_(path.c_str(), nullptr, nullptr, &len, read.file->data, read.limit, RELAY_SLOW_TIMEOUT_MS);
  read.length = ok ? len : 0;
  read.failed = !ok || len == 0 || len >= read.limit;
  std::atomic_thread_fence(std::memory_order_release);
  read.done = true;
}
#endif

#ifdef SVP_KEYBOARD
// Channel task: the keyboard layouts, "name\tdescription\n" lines, into kbd_list_ (the main loop leaves
// it be till done).
void SimpleVideoPlayer::keyboard_list_task_() {
  size_t len = 0;
  const bool ok =
      this->channel_http_("/keyboard", nullptr, nullptr, &len, reinterpret_cast<uint8_t *>(this->kbd_list_),
                          KBD_LIST_BYTES - 1);
  this->kbd_list_len_ = ok ? len : 0;
  this->kbd_list_ok_ = ok;
  this->kbd_list_done_.store(true, std::memory_order_release);
}

// Channel task: the layout kbd_loading_name_ into the slot being read (the main loop leaves both be till done).
void SimpleVideoPlayer::keyboard_get_task_() {
  const std::string path = "/keyboard?layout=" + url_encoded(this->kbd_loading_name_.c_str());
  size_t len = 0;
  const bool ok = this->channel_http_(path.c_str(), nullptr, nullptr, &len, this->kbd_slots_[this->kbd_loading_].data,
                                      KBD_LAYOUT_BYTES);
  this->kbd_get_len_ = ok ? len : 0;
  this->kbd_get_ok_ = ok;
  this->kbd_get_done_.store(true, std::memory_order_release);
}
#endif

// Channel task: this panel's relay settings, the values and what each takes.
void SimpleVideoPlayer::options_get_task_() {
  char path[32];
  snprintf(path, sizeof(path), "/options?port=%u", static_cast<unsigned>(this->stream_port_));
  char *body = nullptr;
  size_t len = 0;
  PsramVector<std::pair<PsramString, PsramString>> values, choices;
  bool ok = this->channel_http_(path, nullptr, &body, &len);
  if (ok) {
    ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
      JsonObject values_in = root["values"].as<JsonObject>();
      for (JsonPair pair : values_in) {
        JsonVariant value = pair.value();
        PsramString text;
        if (value.is<bool>()) {
          text = value.as<bool>() ? "true" : "false";
        } else if (value.is<long>()) {
          text = std::to_string(value.as<long>()).c_str();
        } else {
          text = value.as<const char *>() != nullptr ? value.as<const char *>() : "";
        }
        values.emplace_back(PsramString(pair.key().c_str()), std::move(text));
      }
      JsonObject choices_in = root["choices"].as<JsonObject>();
      for (JsonPair pair : choices_in) {
        PsramString lines;
        bool first = true;
        for (JsonVariant choice : pair.value().as<JsonArray>()) {
          if (!first)
            lines += '\n';
          first = false;
          lines += choice.as<const char *>() != nullptr ? choice.as<const char *>() : "";
        }
        choices.emplace_back(PsramString(pair.key().c_str()), std::move(lines));
      }
      return true;
    });
    heap_caps_free(body);
  }
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->opt_new_values_ = std::move(values);
  this->opt_new_choices_ = std::move(choices);
  this->opt_new_ok_ = ok;
  xSemaphoreGive(this->channel_lock_);
  this->opt_new_.store(true, std::memory_order_release);
}

// Channel task: the settings changed on the panel, to the relay.
void SimpleVideoPlayer::options_set_task_() {
  PsramVector<std::pair<PsramString, PsramString>> sets;
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  sets.swap(this->opt_sets_);
  xSemaphoreGive(this->channel_lock_);
  bool installed = false;
  for (const auto &set : sets) {
    const std::string payload = json::build_json([&](JsonObject root) {
      root["port"] = this->stream_port_;
      root["key"] = set.first.c_str();
      root["value"] = set.second.c_str();
    });
    const bool install = set.first == "keyboard_install";
    this->channel_http_("/options", payload.c_str(), nullptr, nullptr, nullptr, 0,
                        install ? RELAY_INSTALL_TIMEOUT_MS : 0);
    installed |= install;
  }
  // What the installation came to (keyboard_status), shown once it is done.
  if (installed)
    this->options_get_task_();
}

#ifdef SVP_SPICE
// Spice task (blocking): the relay connects to the stream port and draws the desktop, then answers.
bool SimpleVideoPlayer::desktop_relay_start_(const char *kind, const char *host, uint16_t port, const char *password,
                                             uint32_t connection_id) {
  // Its sender is let in from the relay's address only.
  const std::string &url = this->channel_url_;
  size_t from = url.find("://");
  from = from == std::string::npos ? 0 : from + 3;
  const size_t to = url.find_first_of(":/", from);
  const std::string name = url.substr(from, to == std::string::npos ? std::string::npos : to - from);
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  uint32_t relay = 0;
  if (::getaddrinfo(name.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
    relay = reinterpret_cast<struct sockaddr_in *>(result->ai_addr)->sin_addr.s_addr;
    ::freeaddrinfo(result);
  }
  if (relay == 0) {
    ESP_LOGE(TAG, "svp_relay's address %s not found", name.c_str());
    return false;
  }
  this->desktop_relay_addr_.store(relay, std::memory_order_release);
  this->desktop_sender_gone_.store(false, std::memory_order_release);
  this->desktop_sender_wanted_.store(true, std::memory_order_release);
  const std::string payload = json::build_json([&](JsonObject root) {
    root["port"] = this->stream_port_;
    root["action"] = "start";
    root["kind"] = kind;
    root["host"] = host;
    root["server_port"] = port;
    root["password"] = password;
    root["connection_id"] = connection_id;
  });
  if (this->channel_http_("/desktop", payload.c_str(), nullptr, nullptr, nullptr, 0, RELAY_SLOW_TIMEOUT_MS))
    return true;
  this->desktop_sender_wanted_.store(false, std::memory_order_release);
  ESP_LOGE(TAG, "svp_relay did not show the desktop (its log says why)");
  return false;
}

void SimpleVideoPlayer::desktop_relay_stop_() {
  const std::string payload = json::build_json([&](JsonObject root) {
    root["port"] = this->stream_port_;
    root["action"] = "stop";
  });
  this->channel_http_("/desktop", payload.c_str(), nullptr, nullptr);
}
#endif

bool SimpleVideoPlayer::channel_http_(const char *path, const char *body, char **response, size_t *response_len,
                                      uint8_t *into, size_t into_cap, uint32_t timeout_ms) {
  if (this->channel_http_request_ == nullptr)
    return false;
  const uint32_t timeout = timeout_ms != 0 ? timeout_ms : this->channel_http_request_->get_timeout();
  const std::string url = this->channel_url_ + path;
  std::vector<http_request::Header> headers;
  if (!this->channel_token_.empty())
    headers.push_back({"Authorization", "Bearer " + this->channel_token_});
  if (body != nullptr)
    headers.push_back({"Content-Type", "application/json"});
  auto container = this->channel_http_request_->start(url.c_str(), body != nullptr ? "POST" : "GET",
                                                      body != nullptr ? std::string(body) : std::string(), headers,
                                                      {}, timeout);
  if (container == nullptr) {
    ESP_LOGW(TAG, "%s%s failed", this->channel_url_.c_str(), path);
    return false;
  }
  const int status = container->status_code;
  bool ok = status == 200;
  char *buf = nullptr;
  size_t used = 0;
  // Read till the whole answer is there; what does not fit fails.
  auto read_into = [&](uint8_t *dst, size_t room) -> int {
    uint32_t last = millis();
    while (true) {
      const int n = container->read(dst, room);
      switch (http_request::http_read_loop_result(n, last, timeout, container->is_read_complete())) {
        case http_request::HttpReadLoopResult::DATA:
          return n;
        case http_request::HttpReadLoopResult::COMPLETE:
          return 0;
        case http_request::HttpReadLoopResult::RETRY:
          continue;
        default:
          return -1;
      }
    }
  };
  if (ok && into != nullptr) {
    while (true) {
      if (used == into_cap) {
        ok = false;
        break;
      }
      const int n = read_into(into + used, into_cap - used);
      if (n < 0) {
        ok = false;
        break;
      }
      if (n == 0)
        break;
      used += static_cast<size_t>(n);
    }
  } else if (ok && response != nullptr) {
    size_t cap = 0;
    while (true) {
      if (used == cap) {
        const size_t grow = cap == 0 ? 16 * 1024 : cap * 2;
        auto *bigger = grow <= CHANNEL_RESPONSE_MAX
                           ? static_cast<char *>(heap_caps_realloc(buf, grow, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                           : nullptr;
        if (bigger == nullptr) {
          ok = false;
          break;
        }
        buf = bigger;
        cap = grow;
      }
      const int n = read_into(reinterpret_cast<uint8_t *>(buf) + used, cap - used);
      if (n < 0) {
        ok = false;
        break;
      }
      if (n == 0)
        break;
      used += static_cast<size_t>(n);
    }
  }
  container->end();

  if (!ok) {
    ESP_LOGW(TAG, "%s%s failed (HTTP %d)", this->channel_url_.c_str(), path, status);
    heap_caps_free(buf);
    return false;
  }
  if (into != nullptr && response_len != nullptr) {
    *response_len = used;
  } else if (response != nullptr) {
    *response = buf;
    *response_len = used;
  }
  return true;
}

}  // namespace esphome::simple_video_player

#endif  // SVP_CHANNEL_LIST
