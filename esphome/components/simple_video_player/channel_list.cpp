// Channel list for the network stream: fetched from the svp_relay Home Assistant integration into an
// LVGL dropdown; choosing a channel asks the relay to stream it to this panel's stream_port.

#include "simple_video_player.h"

#ifdef SVP_CHANNEL_LIST

#include "esphome/components/json/json_util.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"

#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.channels";

static constexpr size_t CHANNEL_MAX = 300;
static constexpr size_t CHANNEL_RESPONSE_MAX = 256 * 1024;
static constexpr int CHANNEL_HTTP_TIMEOUT_MS = 15000;

bool SimpleVideoPlayer::setup_channel_list_() {
  this->channel_q_ = xQueueCreate(4, sizeof(ChannelCmd));
  this->channel_lock_ = xSemaphoreCreateMutex();
  if (this->channel_q_ == nullptr || this->channel_lock_ == nullptr ||
      xTaskCreatePinnedToCore(channel_task_entry_, "svp_channels", 8192, this, 1, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to start the channel list task");
    return false;
  }
  lv_obj_add_event_cb(this->channel_widget_->obj, channel_selected_cb_, LV_EVENT_VALUE_CHANGED, this);
  return true;
}

void SimpleVideoPlayer::channel_request_(uint8_t cmd, uint16_t index) {
  if (this->channel_q_ == nullptr)
    return;
  const ChannelCmd request{cmd, index};
  xQueueSend(this->channel_q_, &request, 0);
}

void SimpleVideoPlayer::channel_selected_cb_(lv_event_t *e) {
  auto *self = static_cast<SimpleVideoPlayer *>(lv_event_get_user_data(e));
  self->channel_request_(CHANNEL_CMD_PLAY, static_cast<uint16_t>(self->channel_widget_->get_selected_index()));
}

void SimpleVideoPlayer::channel_loop_() {
  if (!this->channel_requested_once_ && this->channel_q_ != nullptr && network::is_connected()) {
    this->channel_requested_once_ = true;
    this->channel_request_(CHANNEL_CMD_REFRESH, 0);
  }
  if (!this->channel_new_.exchange(false, std::memory_order_acq_rel))
    return;
  // LVGL keeps pointers into channel_names_: swap the lists and set the options in one go, here on
  // the LVGL thread.
  xSemaphoreTake(this->channel_lock_, portMAX_DELAY);
  this->channel_names_.swap(this->channel_new_names_);
  this->channel_refs_.swap(this->channel_new_refs_);
  FixedVector<const char *> options;
  options.init(this->channel_names_.empty() ? 1 : this->channel_names_.size());
  if (this->channel_names_.empty())
    options.push_back("(no channels)");
  for (const auto &name : this->channel_names_)
    options.push_back(name.c_str());
  this->channel_widget_->set_options(std::move(options));
  xSemaphoreGive(this->channel_lock_);
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
      std::vector<std::string> names, refs;
      const bool ok = json::parse_json(reinterpret_cast<const uint8_t *>(body), len, [&](JsonObject root) -> bool {
        for (JsonObject channel : root["channels"].as<JsonArray>()) {
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
        ref = this->channel_refs_[request.index];
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

    if (request.cmd == CHANNEL_CMD_STOP)
      this->channel_http_("/stop", "{}", nullptr, nullptr);
  }
}

bool SimpleVideoPlayer::channel_http_(const char *path, const char *body, char **response, size_t *response_len) {
  const std::string url = this->channel_url_ + path;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.timeout_ms = CHANNEL_HTTP_TIMEOUT_MS;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "HTTP client init failed");
    return false;
  }
  std::string auth;
  if (!this->channel_token_.empty()) {
    auth = "Bearer " + this->channel_token_;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  }
  const int body_len = body != nullptr ? static_cast<int>(strlen(body)) : 0;
  if (body != nullptr)
    esp_http_client_set_header(client, "Content-Type", "application/json");

  bool ok = false;
  int status = 0;
  char *buf = nullptr;
  size_t used = 0;
  esp_err_t err = esp_http_client_open(client, body_len);
  if (err == ESP_OK && (body_len == 0 || esp_http_client_write(client, body, body_len) == body_len)) {
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    ok = status == 200;
    if (ok && response != nullptr) {
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
        const int n = esp_http_client_read(client, buf + used, static_cast<int>(cap - used));
        if (n < 0) {
          ok = false;
          break;
        }
        if (n == 0)
          break;
        used += static_cast<size_t>(n);
      }
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (!ok) {
    ESP_LOGW(TAG, "%s%s failed (%s, HTTP %d)", this->channel_url_.c_str(), path, esp_err_to_name(err), status);
    heap_caps_free(buf);
    return false;
  }
  if (response != nullptr) {
    *response = buf;
    *response_len = used;
  }
  return true;
}

}  // namespace esphome::simple_video_player

#endif  // SVP_CHANNEL_LIST
