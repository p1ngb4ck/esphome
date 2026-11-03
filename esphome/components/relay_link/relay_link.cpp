#include "relay_link.h"

#ifdef USE_ESP32

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace esphome::relay_link {

static const char *const TAG = "relay_link";

RelayLink *global_relay_link = nullptr;

void RelayLink::set_url(const char *url) {
  std::snprintf(this->url_, sizeof(this->url_), "%s", url);
  this->url_from_config_ = true;
}

void RelayLink::set_token(const char *token) { std::snprintf(this->token_, sizeof(this->token_), "%s", token); }

#ifdef USE_RELAY_LINK_API
void RelayLink::on_set(std::string url, std::string token, int32_t port) {
  if (this->url_from_config_)
    return;
  while (!url.empty() && url.back() == '/')
    url.pop_back();
  if (url.size() >= sizeof(this->url_) || token.size() >= sizeof(this->token_)) {
    ESP_LOGW(TAG, "URL or token too long");
    return;
  }
  Stored stored{};
  std::snprintf(stored.url, sizeof(stored.url), "%s", url.c_str());
  std::snprintf(stored.token, sizeof(stored.token), "%s", token.c_str());
  stored.port = port > 0 && port < 65536 ? static_cast<uint16_t>(port) : 0;
  if (this->lock_ != nullptr)
    this->lock();
  std::memcpy(this->url_, stored.url, sizeof(this->url_));
  std::memcpy(this->token_, stored.token, sizeof(this->token_));
  this->stream_port_ = stored.port;
  if (this->lock_ != nullptr)
    this->unlock();
  this->pref_.save(&stored);
  ESP_LOGI(TAG, "Relay %s (port %u)", this->url_, this->stream_port_);
}
#endif

RelayLink *RelayLink::create() {
  void *mem = RAMAllocator<RelayLink>(RAMAllocator<RelayLink>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) RelayLink();
  return new RelayLink();
}

bool RelayLink::reserve_lists_() {
  bool ok = this->channel_names_.reserve(CHANNEL_MAX) && this->channel_new_names_.reserve(CHANNEL_MAX) &&
            this->channel_refs_.reserve(CHANNEL_MAX) && this->channel_new_refs_.reserve(CHANNEL_MAX) &&
            this->ha_reqs_.reserve(HA_REQS_MAX) && this->ha_src_items_.reserve(HA_SOURCES_MAX) &&
            this->ha_items_.reserve(HA_ITEMS_MAX) && this->ha_new_items_.reserve(HA_ITEMS_MAX) &&
            this->opt_values_.reserve(OPT_VALUES_MAX) && this->opt_new_values_.reserve(OPT_VALUES_MAX) &&
            this->opt_choices_.reserve(OPT_CHOICES_MAX) && this->opt_new_choices_.reserve(OPT_CHOICES_MAX) &&
            this->opt_sets_.reserve(OPT_SETS_MAX);
  for (HaSources &sources : this->ha_new_srcs_)
    ok = ok && sources.items.reserve(HA_SOURCES_MAX);
  return ok;
}

void RelayLink::setup() {
  global_relay_link = this;
#ifdef USE_RELAY_LINK_CHANNELS
  void *texts = heap_caps_malloc(sizeof(HaTexts), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (texts != nullptr)
    this->ha_texts_ = new (texts) HaTexts();
  if (this->ha_texts_ == nullptr || !this->reserve_lists_()) {
    ESP_LOGE(TAG, "No PSRAM for the channel and Home Assistant lists");
    this->mark_failed();
    return;
  }
#endif
  this->pref_ = global_preferences->make_preference<Stored>(fnv1_hash("relay_link"));
  if (!this->url_from_config_) {
    Stored stored{};
    if (this->pref_.load(&stored)) {
      stored.url[sizeof(stored.url) - 1] = '\0';
      stored.token[sizeof(stored.token) - 1] = '\0';
      std::memcpy(this->url_, stored.url, sizeof(this->url_));
      std::memcpy(this->token_, stored.token, sizeof(this->token_));
      if (this->stream_port_ == 0)
        this->stream_port_ = stored.port;
    }
  }
#ifdef USE_RELAY_LINK_API
  this->register_service(&RelayLink::on_set, "relay_link_set", {"url", "token", "port"});
#endif
  this->lock_ = xSemaphoreCreateMutexStatic(&this->lock_store_);
  this->queue_ =
      xQueueCreateStatic(JOBS, sizeof(Job), reinterpret_cast<uint8_t *>(this->queue_slots_), &this->queue_store_);
  if (!this->task_.create(&RelayLink::task_entry_, "relay_link", 8192, this, 1, true, 0)) {
    this->queue_ = nullptr;
    ESP_LOGE(TAG, "Failed to start its task");
    this->mark_failed();
  }
}

void RelayLink::dump_config() {
  ESP_LOGCONFIG(TAG,
                "relay_link:\n"
                "  URL: %s\n"
                "  Stream port: %u",
                this->url_, this->stream_port_);
}

bool RelayLink::submit(JobFn fn, void *ctx, uint16_t index) {
  if (this->queue_ == nullptr)
    return false;
  const Job job{fn, ctx, index};
  return xQueueSend(this->queue_, &job, 0) == pdTRUE;
}

bool RelayLink::fetch_start(Fetch &fetch, JobFn fn, void *ctx, uint8_t *into, size_t cap) {
  if ((fetch.state.load(std::memory_order_acquire) & 7) != FETCH_IDLE)
    return false;
  if (++fetch.gen == 0)
    fetch.gen = 1;
  const uint32_t base = static_cast<uint32_t>(fetch.gen) << 3;
  fetch.into.store(into, std::memory_order_relaxed);
  fetch.cap.store(cap, std::memory_order_relaxed);
  fetch.state.store(base | FETCH_QUEUED, std::memory_order_release);
  if (this->submit(fn, ctx, fetch.gen))
    return true;
  fetch.state.store(base | FETCH_IDLE, std::memory_order_release);
  return false;
}

bool RelayLink::fetch_done(Fetch &fetch) {
  const uint32_t done = static_cast<uint32_t>(fetch.gen) << 3 | FETCH_DONE;
  if (fetch.state.load(std::memory_order_acquire) != done)
    return false;
  fetch.state.store(done & ~7u, std::memory_order_release);
  return true;
}

bool RelayLink::fetch_drop(Fetch &fetch) {
  uint32_t state = fetch.state.load(std::memory_order_acquire);
  while (true) {
    const uint32_t base = state & ~7u;
    switch (state & 7) {
      case FETCH_QUEUED:
        if (fetch.state.compare_exchange_weak(state, base | FETCH_IDLE, std::memory_order_acq_rel))
          return true;
        continue;
      case FETCH_WRITING:
        if (fetch.state.compare_exchange_weak(state, base | FETCH_STOP, std::memory_order_acq_rel))
          return false;
        continue;
      case FETCH_STOP:
        return false;
      case FETCH_DONE:
        fetch.state.store(base | FETCH_IDLE, std::memory_order_release);
        return true;
      default:
        return true;
    }
  }
}

bool RelayLink::fetch_begin(Fetch &fetch, uint16_t gen, uint8_t **into, size_t *cap) {
  if (fetch.state.load(std::memory_order_acquire) != (static_cast<uint32_t>(gen) << 3 | FETCH_QUEUED))
    return false;
  *into = fetch.into.load(std::memory_order_relaxed);
  *cap = fetch.cap.load(std::memory_order_relaxed);
  return true;
}

void RelayLink::fetch_end(Fetch &fetch, uint16_t gen, bool ok, size_t len) {
  const uint32_t base = static_cast<uint32_t>(gen) << 3;
  uint32_t state = fetch.state.load(std::memory_order_acquire);
  if (state != (base | FETCH_QUEUED) && state != (base | FETCH_WRITING)) {
    if (state == (base | FETCH_STOP))
      fetch.state.store(base | FETCH_IDLE, std::memory_order_release);
    return;
  }
  fetch.len = len;
  fetch.ok = ok;
  if (!fetch.state.compare_exchange_strong(state, base | FETCH_DONE, std::memory_order_acq_rel) &&
      state == (base | FETCH_STOP))
    fetch.state.store(base | FETCH_IDLE, std::memory_order_release);
}

std::string RelayLink::url_encoded(const char *text) {
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

uint32_t RelayLink::host_address() const {
  const std::string url = this->url_;
  size_t from = url.find("://");
  from = from == std::string::npos ? 0 : from + 3;
  const size_t to = url.find_first_of(":/", from);
  const std::string name = url.substr(from, to == std::string::npos ? std::string::npos : to - from);
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *result = nullptr;
  uint32_t address = 0;
  if (::getaddrinfo(name.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
    address = reinterpret_cast<struct sockaddr_in *>(result->ai_addr)->sin_addr.s_addr;
    ::freeaddrinfo(result);
  }
  if (address == 0)
    ESP_LOGE(TAG, "Address of %s not found", name.c_str());
  return address;
}

void RelayLink::ha_browse(const std::string &type, const std::string &id) {
  if (this->lock_ == nullptr)
    return;
  if (!HaType::fits(type.size()) || !RelayId::fits(id.size())) {
    ESP_LOGW(TAG, "Media id too long: %s", id.c_str());
    return;
  }
  this->lock();
  for (const auto &req : this->ha_reqs_) {
    if (std::strcmp(req.first.c_str(), type.c_str()) == 0 && std::strcmp(req.second.c_str(), id.c_str()) == 0) {
      this->unlock();
      return;
    }
  }
  const bool queued = this->ha_reqs_.emplace_back(type.c_str(), id.c_str()) != nullptr;
  this->unlock();
  if (!queued) {
    ESP_LOGW(TAG, "Too many listings asked for: %s not listed", id.c_str());
    return;
  }
  if (this->ha_browse_waiting_ != 0 || !network::is_connected()) {
    this->ha_browse_waiting_++;
    this->enable_loop();
    return;
  }
  if (this->submit(&RelayLink::browse_job_, this, 0))
    return;
  this->lock();
  if (!this->ha_reqs_.empty())
    this->ha_reqs_.pop_back();
  this->unlock();
}

void RelayLink::ha_play(const std::string &id) {
  if (this->lock_ == nullptr || id.empty())
    return;
  if (!RelayId::fits(id.size())) {
    ESP_LOGW(TAG, "Media id too long: %s", id.c_str());
    return;
  }
  this->lock();
  this->ha_texts_->play_id.assign(id.c_str());
  this->unlock();
  this->submit(&RelayLink::ha_play_job_, this, 0);
}

std::string RelayLink::relay_option(const std::string &key) const {
  for (const auto &value : this->opt_values_) {
    if (key == value.first.c_str())
      return value.second.c_str();
  }
  return "";
}

std::string RelayLink::relay_choices(const std::string &key) const {
  for (const auto &choices : this->opt_choices_) {
    if (key == choices.first.c_str())
      return choices.second.c_str();
  }
  return "";
}

void RelayLink::relay_option_set(const std::string &key, const std::string &value) {
  if (this->lock_ == nullptr)
    return;
  if (!OptPair::first_type::fits(key.size()) || !OptPair::second_type::fits(value.size())) {
    ESP_LOGW(TAG, "Relay setting %s: too long", key.c_str());
    return;
  }
  this->lock();
  const bool queued = this->opt_sets_.emplace_back(key.c_str(), value.c_str()) != nullptr;
  this->unlock();
  if (!queued) {
    ESP_LOGW(TAG, "Relay setting %s not sent: too many waiting", key.c_str());
    return;
  }
  bool known = false;
  for (auto &option : this->opt_values_) {
    if (key == option.first.c_str()) {
      option.second.assign(value.c_str());
      known = true;
    }
  }
  if (!known)
    this->opt_values_.emplace_back(key.c_str(), value.c_str());
  this->submit(&RelayLink::options_set_job_, this, 0);
}

void RelayLink::channel_play() {
#ifdef USE_RELAY_LINK_WIDGET
  if (this->channel_widget_ != nullptr) {
    this->channel_play(static_cast<int>(this->channel_widget_->get_selected_index()));
    return;
  }
#endif
  this->channel_play(this->channel_index_);
}

void RelayLink::channel_play(int index) {
  if (index < 0 || static_cast<size_t>(index) >= this->channel_names_.size())
    return;
  this->channel_index_ = index;
  this->submit(&RelayLink::play_job_, this, static_cast<uint16_t>(index));
}

void RelayLink::loop() {
#ifdef USE_RELAY_LINK_CHANNELS
  this->channel_loop_();
  this->ha_loop_();
  if (this->channel_requested_once_ && this->ha_browse_waiting_ == 0 &&
      !this->channel_new_.load(std::memory_order_acquire) && !this->ha_new_src_.load(std::memory_order_acquire) &&
      !this->ha_new_.load(std::memory_order_acquire) && !this->opt_new_.load(std::memory_order_acquire))
    this->disable_loop();
#else
  this->disable_loop();
#endif
}

void RelayLink::channel_loop_() {
  if (!this->channel_requested_once_ && network::is_connected()) {
    this->channel_requested_once_ = true;
    this->channel_list_refresh();
  }
  while (this->ha_browse_waiting_ != 0 && network::is_connected() &&
         this->submit(&RelayLink::browse_job_, this, 0))
    this->ha_browse_waiting_--;
#ifdef USE_RELAY_LINK_WIDGET
  if (this->channel_widget_ != nullptr && this->lvgl_ != nullptr && this->lvgl_->is_paused())
    return;
#endif
  if (!this->channel_new_.load(std::memory_order_acquire))
    return;
  this->lock();
  if (!this->channel_new_.load(std::memory_order_relaxed)) {
    this->unlock();
    return;
  }
  this->channel_new_.store(false, std::memory_order_relaxed);
  this->channel_names_.swap(this->channel_new_names_);
  this->channel_refs_.swap(this->channel_new_refs_);
#ifdef USE_RELAY_LINK_WIDGET
  if (this->channel_widget_ != nullptr) {
    FixedVector<const char *> options;
    options.init(this->channel_names_.empty() ? 1 : this->channel_names_.size());
    if (this->channel_names_.empty())
      options.push_back("(no channels)");
    for (const auto &name : this->channel_names_)
      options.push_back(name.c_str());
    this->channel_widget_->set_options(std::move(options));
  }
#endif
  this->unlock();
  if (static_cast<size_t>(this->channel_index_ + 1) > this->channel_names_.size())
    this->channel_index_ = -1;
  this->channel_version_++;
  this->channels_callbacks_.call();
}

void RelayLink::ha_loop_() {
  if (this->ha_new_src_.load(std::memory_order_acquire)) {
    while (true) {
      this->lock();
      if (this->ha_srcs_count_ == 0) {
        this->ha_new_src_.store(false, std::memory_order_relaxed);
        this->unlock();
        break;
      }
      HaSources &sources = this->ha_new_srcs_[this->ha_srcs_head_];
      this->ha_src_items_.swap(sources.items);
      this->ha_texts_->sources_type = sources.type;
      this->ha_src_ok_ = sources.ok;
      this->ha_srcs_head_ = static_cast<uint8_t>((this->ha_srcs_head_ + 1) % HA_SOURCE_LISTS);
      this->ha_srcs_count_--;
      this->unlock();
      this->ha_last_sources_ = true;
      this->ha_callbacks_.call();
    }
  }
  if (this->ha_new_.load(std::memory_order_acquire)) {
    this->lock();
    const bool listed = this->ha_new_.load(std::memory_order_relaxed);
    if (listed) {
      this->ha_new_.store(false, std::memory_order_relaxed);
      this->ha_items_.swap(this->ha_new_items_);
      this->ha_texts_->shown ^= 1;
      this->ha_ok_ = this->ha_new_ok_;
    }
    this->unlock();
    if (listed) {
      this->ha_last_sources_ = false;
      this->ha_callbacks_.call();
    }
  }
  if (this->opt_new_.load(std::memory_order_acquire)) {
    this->lock();
    const bool listed = this->opt_new_.load(std::memory_order_relaxed);
    if (listed) {
      this->opt_new_.store(false, std::memory_order_relaxed);
      if (this->opt_new_ok_) {
        this->opt_values_.swap(this->opt_new_values_);
        this->opt_choices_.swap(this->opt_new_choices_);
      }
      this->opt_ok_ = this->opt_new_ok_;
    }
    this->unlock();
    if (listed)
      this->opt_callbacks_.call();
  }
}

void RelayLink::task_entry_(void *param) { static_cast<RelayLink *>(param)->task_loop_(); }

void RelayLink::task_loop_() {
  Job job;
  while (true) {
    if (xQueueReceive(this->queue_, &job, portMAX_DELAY) == pdTRUE)
      job.fn(job.ctx, job.index);
  }
}

void RelayLink::refresh_job_(void *ctx, uint16_t index) { static_cast<RelayLink *>(ctx)->refresh_(); }
void RelayLink::play_job_(void *ctx, uint16_t index) { static_cast<RelayLink *>(ctx)->play_(index); }
void RelayLink::stop_job_(void *ctx, uint16_t index) {
  static_cast<RelayLink *>(ctx)->http("/stop", "{}", nullptr, nullptr);
}
void RelayLink::browse_job_(void *ctx, uint16_t index) { static_cast<RelayLink *>(ctx)->ha_browse_(); }
void RelayLink::options_get_job_(void *ctx, uint16_t index) { static_cast<RelayLink *>(ctx)->options_get_(); }
void RelayLink::options_set_job_(void *ctx, uint16_t index) { static_cast<RelayLink *>(ctx)->options_set_(); }

void RelayLink::ha_play_job_(void *ctx, uint16_t index) {
  auto *relay = static_cast<RelayLink *>(ctx);
  relay->lock();
  const std::string id = relay->ha_texts_->play_id.c_str();
  relay->unlock();
  const std::string payload = json::build_json([&](JsonObject root) {
    root["ref"] = id;
    root["port"] = relay->stream_port_;
  });
  relay->http("/play", payload.c_str(), nullptr, nullptr);
}

void RelayLink::refresh_() {
  char *body = nullptr;
  size_t len = 0;
  if (!this->http("/channels?refresh=1", nullptr, &body, &len))
    return;
  this->lock();
  this->channel_new_.store(false, std::memory_order_relaxed);
  this->unlock();
  auto &names = this->channel_new_names_;
  auto &refs = this->channel_new_refs_;
  names.clear();
  refs.clear();
  const bool ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
    JsonArray channels = root["channels"];
    for (JsonObject channel : channels) {
      if (names.full() || refs.full())
        break;
      const char *name = channel["name"] | "";
      const char *ref = channel["ref"] | "";
      if (*ref == '\0')
        continue;
      if (!FixedString<256>::fits(ref)) {
        ESP_LOGW(TAG, "Channel %s left out: its reference is too long", name);
        continue;
      }
      names.emplace_back(name);
      refs.emplace_back(ref);
    }
    return true;
  });
  heap_caps_free(body);
  if (!ok) {
    ESP_LOGW(TAG, "Channel list is not valid JSON");
    return;
  }
  ESP_LOGI(TAG, "%u channels", static_cast<unsigned>(names.size()));
  this->lock();
  this->channel_new_.store(true, std::memory_order_release);
  this->unlock();
  this->enable_loop_soon_any_context();
}

void RelayLink::play_(uint16_t index) {
  std::string ref;
  this->lock();
  if (index < this->channel_refs_.size())
    ref = this->channel_refs_[index].c_str();
  this->unlock();
  if (ref.empty())
    return;
  const std::string payload = json::build_json([&](JsonObject root) {
    root["ref"] = ref;
    root["port"] = this->stream_port_;
  });
  ESP_LOGI(TAG, "Requesting %s", ref.c_str());
  this->http("/play", payload.c_str(), nullptr, nullptr);
}

void RelayLink::ha_browse_() {
  this->lock();
  if (this->ha_reqs_.empty()) {
    this->unlock();
    return;
  }
  const std::string type = this->ha_reqs_.front().first.c_str();
  const std::string id = this->ha_reqs_.front().second.c_str();
  this->ha_reqs_.erase(this->ha_reqs_.begin());
  this->unlock();
  std::string path = "/browse?type=" + url_encoded(type.c_str());
  if (!id.empty())
    path += "&id=" + url_encoded(id.c_str());
  char *body = nullptr;
  size_t len = 0;
  bool ok = this->http(path.c_str(), nullptr, &body, &len);
  HaSources *sources = nullptr;
  HaListing *listing = nullptr;
  this->lock();
  if (id.empty()) {
    if (this->ha_srcs_count_ == HA_SOURCE_LISTS) {
      this->ha_srcs_head_ = static_cast<uint8_t>((this->ha_srcs_head_ + 1) % HA_SOURCE_LISTS);
      this->ha_srcs_count_--;
    }
    sources = &this->ha_new_srcs_[(this->ha_srcs_head_ + this->ha_srcs_count_) % HA_SOURCE_LISTS];
  } else {
    this->ha_new_.store(false, std::memory_order_relaxed);
    listing = &this->ha_texts_->listings[this->ha_texts_->shown ^ 1];
  }
  this->unlock();
  FixedList<HaItem> &items = sources != nullptr ? sources->items : this->ha_new_items_;
  items.clear();
  if (listing != nullptr) {
    listing->type.assign(type.c_str());
    listing->id.assign(id.c_str());
    listing->title.clear();
    listing->path.clear();
    listing->parent.clear();
  }
  if (ok) {
    ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
      if (listing != nullptr) {
        listing->title.assign(root["title"] | "");
        listing->path.assign(root["path"] | "");
        const char *parent = root["parent"] | "";
        if (RelayId::fits(parent))
          listing->parent.assign(parent);
      }
      JsonArray list = root["items"].as<JsonArray>();
      for (JsonObject item : list) {
        if (items.full())
          break;
        const char *item_id = item["id"] | "";
        if (*item_id == '\0')
          continue;
        if (!RelayId::fits(item_id)) {
          ESP_LOGW(TAG, "%s left out: its id is too long", item["title"] | "");
          continue;
        }
        items.emplace_back(HaItem{item["title"] | "", item_id, item["dir"] | false});
      }
      return true;
    });
    heap_caps_free(body);
  }
  if (sources != nullptr) {
    sources->type.assign(type.c_str());
    sources->ok = ok;
  }
  this->lock();
  if (sources != nullptr) {
    this->ha_srcs_count_++;
    this->ha_new_src_.store(true, std::memory_order_release);
  } else {
    this->ha_new_ok_ = ok;
    this->ha_new_.store(true, std::memory_order_release);
  }
  this->unlock();
  this->enable_loop_soon_any_context();
}

void RelayLink::options_get_() {
  char path[32];
  snprintf(path, sizeof(path), "/options?port=%u", static_cast<unsigned>(this->stream_port_));
  char *body = nullptr;
  size_t len = 0;
  bool ok = this->http(path, nullptr, &body, &len);
  this->lock();
  this->opt_new_.store(false, std::memory_order_relaxed);
  this->unlock();
  auto &values = this->opt_new_values_;
  auto &choices = this->opt_new_choices_;
  values.clear();
  choices.clear();
  if (ok) {
    ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
      JsonObject values_in = root["values"].as<JsonObject>();
      for (JsonPair pair : values_in) {
        JsonVariant value = pair.value();
        std::string text;
        if (value.is<bool>()) {
          text = value.as<bool>() ? "true" : "false";
        } else if (value.is<long>()) {
          char number[24];
          snprintf(number, sizeof(number), "%ld", value.as<long>());
          text = number;
        } else {
          text = value.as<const char *>() != nullptr ? value.as<const char *>() : "";
        }
        if (!OptPair::first_type::fits(pair.key().c_str()) || !OptPair::second_type::fits(text.size())) {
          ESP_LOGW(TAG, "Relay setting %s left out: too long", pair.key().c_str());
          continue;
        }
        if (values.emplace_back(pair.key().c_str(), text) == nullptr)
          ESP_LOGW(TAG, "Relay setting %s left out: too many", pair.key().c_str());
      }
      JsonObject choices_in = root["choices"].as<JsonObject>();
      for (JsonPair pair : choices_in) {
        std::string lines;
        bool first = true;
        for (JsonVariant choice : pair.value().as<JsonArray>()) {
          if (!first)
            lines += '\n';
          first = false;
          lines += choice.as<const char *>() != nullptr ? choice.as<const char *>() : "";
        }
        if (!OptPair::first_type::fits(pair.key().c_str()) || !OptPair::second_type::fits(lines.size())) {
          ESP_LOGW(TAG, "Choices of %s left out: too long", pair.key().c_str());
          continue;
        }
        if (choices.emplace_back(pair.key().c_str(), lines) == nullptr)
          ESP_LOGW(TAG, "Choices of %s left out: too many", pair.key().c_str());
      }
      return true;
    });
    heap_caps_free(body);
  }
  this->lock();
  this->opt_new_ok_ = ok;
  this->opt_new_.store(true, std::memory_order_release);
  this->unlock();
  this->enable_loop_soon_any_context();
}

void RelayLink::options_set_() {
  bool installed = false;
  while (true) {
    this->lock();
    if (this->opt_sets_.empty()) {
      this->unlock();
      break;
    }
    const std::string key = this->opt_sets_.front().first.c_str();
    const std::string value = this->opt_sets_.front().second.c_str();
    this->opt_sets_.erase(this->opt_sets_.begin());
    this->unlock();
    const std::string payload = json::build_json([&](JsonObject root) {
      root["port"] = this->stream_port_;
      root["key"] = key;
      root["value"] = value;
    });
    const bool install = key == "keyboard_install";
    this->http("/options", payload.c_str(), nullptr, nullptr, nullptr, 0, install ? INSTALL_TIMEOUT_MS : 0);
    installed |= install;
  }
  if (installed)
    this->options_get_();
}

bool RelayLink::http(const char *path, const char *body, char **response, size_t *response_len, uint8_t *into,
                     size_t into_cap, uint32_t timeout_ms, Fetch *fetch, uint16_t gen) {
  char base[sizeof(this->url_)];
  char token[sizeof(this->token_)];
  this->lock();
  std::memcpy(base, this->url_, sizeof(base));
  std::memcpy(token, this->token_, sizeof(token));
  this->unlock();
  if (base[0] == '\0')
    return false;
  return this->http_at(base, token, path, body, response, response_len, into, into_cap, timeout_ms, fetch, gen);
}

bool RelayLink::http_at(const char *base, const char *token, const char *path, const char *body, char **response,
                        size_t *response_len, uint8_t *into, size_t into_cap, uint32_t timeout_ms, Fetch *fetch,
                        uint16_t gen) {
  if (this->http_ == nullptr)
    return false;
  const uint32_t timeout = timeout_ms != 0 ? timeout_ms : this->http_->get_timeout();
  const std::string url = std::string(base) + path;
  std::vector<http_request::Header> headers;
  if (token != nullptr && *token != '\0')
    headers.push_back({"Authorization", std::string("Bearer ") + token});
  if (body != nullptr)
    headers.push_back({"Content-Type", "application/json"});
  auto container = this->http_->start(url.c_str(), body != nullptr ? "POST" : "GET",
                                      body != nullptr ? std::string(body) : std::string(), headers, {}, timeout);
  if (container == nullptr) {
    ESP_LOGW(TAG, "%s%s failed", base, path);
    return false;
  }
  const int status = container->status_code;
  bool ok = status == 200;
  char *buf = nullptr;
  size_t used = 0;
  const uint32_t writing = static_cast<uint32_t>(gen) << 3 | FETCH_WRITING;
  bool dropped = false;
  if (ok && into != nullptr && fetch != nullptr) {
    uint32_t queued = static_cast<uint32_t>(gen) << 3 | FETCH_QUEUED;
    dropped = !fetch->state.compare_exchange_strong(queued, writing, std::memory_order_acq_rel);
    ok = !dropped;
  }
  auto stopped = [&]() {
    dropped = dropped || (fetch != nullptr && fetch->state.load(std::memory_order_acquire) != writing);
    return dropped;
  };
  auto read_into = [&](uint8_t *dst, size_t room) -> int {
    uint32_t last = millis();
    while (true) {
      if (stopped())
        return -1;
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
        auto *bigger = grow <= RESPONSE_MAX
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
    if (!dropped)
      ESP_LOGW(TAG, "%s%s failed (HTTP %d)", base, path, status);
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

}

#endif
