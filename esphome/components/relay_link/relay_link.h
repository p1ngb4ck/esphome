#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESP32

#include "esphome/components/http_request/http_request.h"
#ifdef USE_RELAY_LINK_API
#include "esphome/components/api/custom_api_device.h"
#endif
#include "esphome/core/preferences.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"
#ifdef USE_RELAY_LINK_WIDGET
#include "esphome/components/lvgl/lvgl_esphome.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>
#include <functional>
#include <string>

#include "esphome/components/fixed_containers/fixed_containers.h"

namespace esphome::relay_link {

using fixed_containers::FixedList;
using fixed_containers::FixedString;
using fixed_containers::TextView;

using RelayPath = FixedString<256>;
using RelayId = FixedString<384>;
using RelayTitle = FixedString<128>;
using HaType = FixedString<16>;

struct HaItem {
  RelayTitle title;
  RelayId id;
  bool dir{false};
};

class RelayLink : public Component
#ifdef USE_RELAY_LINK_API
    , public api::CustomAPIDevice
#endif
{
 public:
  static RelayLink *create();

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_url(const char *url);
  void set_token(const char *token);
#ifdef USE_RELAY_LINK_API
  void on_set(std::string url, std::string token, int32_t port);
#endif
  void set_http_request(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_stream_port(uint16_t port) { this->stream_port_ = port; }
#ifdef USE_RELAY_LINK_WIDGET
  void set_channel_widget(lvgl::LvDropdownType *widget) { this->channel_widget_ = widget; }
  void set_lvgl(lvgl::LvglComponent *lvgl) { this->lvgl_ = lvgl; }
#endif

  const char *get_url() const { return this->url_; }
  uint16_t get_stream_port() const { return this->stream_port_; }
  bool is_ready() const { return this->queue_ != nullptr; }

  using JobFn = void (*)(void *ctx, uint16_t index);
  bool submit(JobFn fn, void *ctx, uint16_t index);
  void lock() { xSemaphoreTake(this->lock_, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(this->lock_); }

  enum : uint8_t { FETCH_IDLE, FETCH_QUEUED, FETCH_WRITING, FETCH_STOP, FETCH_DONE };
  struct Fetch {
    std::atomic<uint32_t> state{0};
    std::atomic<uint8_t *> into{nullptr};
    std::atomic<size_t> cap{0};
    uint16_t gen{0};
    size_t len{0};
    bool ok{false};
  };
  bool fetch_start(Fetch &fetch, JobFn fn, void *ctx, uint8_t *into, size_t cap);
  static bool fetch_done(Fetch &fetch);
  static bool fetch_drop(Fetch &fetch);
  static bool fetch_begin(Fetch &fetch, uint16_t gen, uint8_t **into, size_t *cap);
  static void fetch_end(Fetch &fetch, uint16_t gen, bool ok, size_t len);

  static constexpr uint32_t SLOW_TIMEOUT_MS = 15000;
  static constexpr uint32_t INSTALL_TIMEOUT_MS = 180000;
  bool http(const char *path, const char *body, char **response, size_t *response_len, uint8_t *into = nullptr,
            size_t into_cap = 0, uint32_t timeout_ms = 0, Fetch *fetch = nullptr, uint16_t gen = 0);
  bool http_at(const char *base, const char *token, const char *path, const char *body, char **response,
               size_t *response_len, uint8_t *into = nullptr, size_t into_cap = 0, uint32_t timeout_ms = 0,
               Fetch *fetch = nullptr, uint16_t gen = 0);
  bool has_url() const { return this->url_[0] != '\0'; }
  static std::string url_encoded(const char *text);
  uint32_t host_address() const;

  void channel_list_refresh() { this->submit(&RelayLink::refresh_job_, this, 0); }
  void channel_play();
  void channel_play(int index);
  void channel_stop() { this->submit(&RelayLink::stop_job_, this, 0); }
  const FixedList<FixedString<64>> &get_channel_names() const { return this->channel_names_; }
  uint32_t get_channel_list_version() const { return this->channel_version_; }
  int get_channel_index() const { return this->channel_index_; }
  void add_on_channels_callback(std::function<void()> &&callback) { this->channels_callbacks_.add(std::move(callback)); }

  void ha_browse(const std::string &type, const std::string &id);
  const FixedList<HaItem> &ha_source_items() const { return this->ha_src_items_; }
  TextView ha_sources_type() const { return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->sources_type) : TextView(); }
  bool ha_sources_listed() const { return this->ha_src_ok_; }
  bool ha_last_sources() const { return this->ha_last_sources_; }
  const FixedList<HaItem> &ha_items() const { return this->ha_items_; }
  TextView ha_type() const { return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->listing().type) : TextView(); }
  TextView ha_id() const { return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->listing().id) : TextView(); }
  TextView ha_title() const {
    return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->listing().title) : TextView();
  }
  TextView ha_path() const { return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->listing().path) : TextView(); }
  TextView ha_parent() const {
    return this->ha_texts_ != nullptr ? TextView(this->ha_texts_->listing().parent) : TextView();
  }
  bool ha_listed() const { return this->ha_ok_; }
  void ha_play(const std::string &id);
  void add_on_ha_browse_callback(std::function<void()> &&callback) { this->ha_callbacks_.add(std::move(callback)); }

  void relay_options_fetch() { this->submit(&RelayLink::options_get_job_, this, 0); }
  bool relay_options_ok() const { return this->opt_ok_; }
  std::string relay_option(const std::string &key) const;
  std::string relay_choices(const std::string &key) const;
  void relay_option_set(const std::string &key, const std::string &value);
  void add_on_relay_options_callback(std::function<void()> &&callback) {
    this->opt_callbacks_.add(std::move(callback));
  }

 protected:
  struct Job {
    JobFn fn;
    void *ctx;
    uint16_t index;
  };
  static constexpr size_t JOBS = 8;
  static constexpr size_t RESPONSE_MAX = 256 * 1024;
  static constexpr size_t CHANNEL_MAX = 300;
  static constexpr size_t HA_ITEMS_MAX = 500;
  static constexpr size_t HA_SOURCES_MAX = 32;
  static constexpr size_t HA_REQS_MAX = 8;
  static constexpr size_t HA_SOURCE_LISTS = 3;
  static constexpr size_t OPT_VALUES_MAX = 32;
  static constexpr size_t OPT_CHOICES_MAX = 16;
  static constexpr size_t OPT_SETS_MAX = 8;

  struct HaListing {
    HaType type;
    RelayId id;
    RelayTitle title;
    RelayPath path;
    RelayId parent;
  };
  struct HaTexts {
    HaListing listings[2];
    uint8_t shown{0};
    const HaListing &listing() const { return this->listings[this->shown]; }
    HaType sources_type;
    RelayId play_id;
  };
  struct HaSources {
    HaType type;
    FixedList<HaItem> items;
    bool ok{false};
  };
  using OptPair = std::pair<FixedString<32>, FixedString<256>>;

  bool reserve_lists_();
  void channel_loop_();
  void ha_loop_();
  static void task_entry_(void *param);
  void task_loop_();
  static void refresh_job_(void *ctx, uint16_t index);
  static void play_job_(void *ctx, uint16_t index);
  static void stop_job_(void *ctx, uint16_t index);
  static void browse_job_(void *ctx, uint16_t index);
  static void ha_play_job_(void *ctx, uint16_t index);
  static void options_get_job_(void *ctx, uint16_t index);
  static void options_set_job_(void *ctx, uint16_t index);
  void refresh_();
  void play_(uint16_t index);
  void ha_browse_();
  void options_get_();
  void options_set_();

  struct Stored {
    char url[128];
    char token[256];
    uint16_t port;
  };
  char url_[128]{};
  char token_[256]{};
  bool url_from_config_{false};
  ESPPreferenceObject pref_;
  http_request::HttpRequestComponent *http_{nullptr};
  uint16_t stream_port_{0};
#ifdef USE_RELAY_LINK_WIDGET
  lvgl::LvDropdownType *channel_widget_{nullptr};
  lvgl::LvglComponent *lvgl_{nullptr};
#endif
  QueueHandle_t queue_{nullptr};
  StaticQueue_t queue_store_{};
  Job queue_slots_[JOBS]{};
  SemaphoreHandle_t lock_{nullptr};
  StaticSemaphore_t lock_store_{};
  StaticTask task_;

  FixedList<FixedString<64>> channel_names_, channel_new_names_;
  FixedList<FixedString<256>> channel_refs_, channel_new_refs_;
  std::atomic<bool> channel_new_{false};
  bool channel_requested_once_{false};
  uint32_t channel_version_{0};
  int channel_index_{-1};
  CallbackManager<void()> channels_callbacks_;

  HaTexts *ha_texts_{nullptr};
  FixedList<std::pair<HaType, RelayId>> ha_reqs_;
  uint16_t ha_browse_waiting_{0};
  FixedList<HaItem> ha_src_items_;
  bool ha_src_ok_{false}, ha_last_sources_{false};
  HaSources ha_new_srcs_[HA_SOURCE_LISTS];
  uint8_t ha_srcs_head_{0}, ha_srcs_count_{0};
  std::atomic<bool> ha_new_src_{false};
  FixedList<HaItem> ha_items_, ha_new_items_;
  bool ha_ok_{false}, ha_new_ok_{false};
  std::atomic<bool> ha_new_{false};
  CallbackManager<void()> ha_callbacks_;

  FixedList<OptPair> opt_values_, opt_new_values_, opt_choices_, opt_new_choices_, opt_sets_;
  bool opt_ok_{false}, opt_new_ok_{false};
  std::atomic<bool> opt_new_{false};
  CallbackManager<void()> opt_callbacks_;
};

extern RelayLink *global_relay_link;

}

#endif
