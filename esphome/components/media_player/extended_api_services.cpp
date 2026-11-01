#include "extended_api_services.h"

#ifdef USE_MEDIA_PLAYER_EXTENDED_API

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_MEDIA_PLAYER_COVER
#include <driver/jpeg_decode.h>
#include <esp_cache.h>
#endif

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <strings.h>

namespace esphome::media_player {

static const char *const TAG = "media_player.extended_api";

ExtendedApiServices *ExtendedApiServices::create() {
  void *mem = RAMAllocator<ExtendedApiServices>(RAMAllocator<ExtendedApiServices>::ALLOC_EXTERNAL).allocate(1);
  if (mem != nullptr)
    return new (mem) ExtendedApiServices();
  return new ExtendedApiServices();
}

void ExtendedApiServices::setup() {
  this->register_service(&ExtendedApiServices::on_play_audio, "media_play_audio", {"url", "announce"});
  this->register_service(&ExtendedApiServices::on_audio_command, "media_audio_command", {"command"});
  this->register_service(&ExtendedApiServices::on_set_volume, "media_set_volume", {"volume"});
#ifdef USE_MEDIA_PLAYER_COVER
  this->register_service(&ExtendedApiServices::on_cover, "media_cover");
#endif
  this->build_traits_();
  this->player_->add_on_state_callback([this](MediaPlayerState) { this->publish_status_(); });
  this->publish_status_();
  this->disable_loop();
}

void ExtendedApiServices::build_traits_() {
  auto traits = this->player_->get_traits();
  size_t len = std::snprintf(this->traits_, sizeof(this->traits_), "\"pause\":%s,\"formats\":[",
                             traits.get_supports_pause() ? "true" : "false");
  const size_t start = len;
  for (const auto &format : traits.get_supported_formats()) {
    char entry[64];
    std::snprintf(entry, sizeof(entry), "%s[\"%s\",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u]", len > start ? "," : "",
                  format.format.c_str(), format.sample_rate, format.num_channels, format.sample_bytes,
                  static_cast<unsigned>(format.purpose));
    if (len + std::strlen(entry) + 2 > sizeof(this->traits_))
      break;
    len += std::snprintf(this->traits_ + len, sizeof(this->traits_) - len, "%s", entry);
  }
  std::snprintf(this->traits_ + len, sizeof(this->traits_) - len, "]");
}

void ExtendedApiServices::publish_status_() {
  if (this->status_ == nullptr)
    return;
  uint16_t port = 0;
#ifdef USE_MEDIA_PLAYER_COVER
  if (this->relay_ != nullptr)
    port = this->relay_->get_stream_port();
#endif
  char json[384];
  std::snprintf(json, sizeof(json),
                "{\"extended_api\":1,\"port\":%u,\"state\":\"%s\",\"volume\":%.2f,\"muted\":%s,\"playlist\":false,"
                "\"video\":false,\"game\":\"\",\"link\":\"off\",%s}",
                port, media_player_state_to_string(this->player_->state), this->player_->volume,
                this->player_->is_muted() ? "true" : "false", this->traits_);
  this->status_->publish_state(json);
}

void ExtendedApiServices::on_play_audio(std::string url, bool announce) {
  ESP_LOGD(TAG, "Play audio%s: %s", announce ? " (announcement)" : "", url.c_str());
  this->player_->make_call().set_media_url(url).set_announcement(announce).perform();
}

void ExtendedApiServices::on_audio_command(std::string command) {
  ESP_LOGD(TAG, "Audio command: %s", command.c_str());
  this->player_->make_call().set_command(command).perform();
}

void ExtendedApiServices::on_set_volume(float volume) {
  this->player_->make_call().set_volume(clamp(volume, 0.0f, 1.0f)).perform();
}

#ifdef USE_MEDIA_PLAYER_COVER
static constexpr size_t COVER_ALIGN = 128;

static constexpr size_t align_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

static size_t decoded_size(const jpeg_decode_picture_info_t &info, uint32_t *padded_w) {
  const bool wide = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 || info.sample_method == JPEG_DOWN_SAMPLING_YUV420;
  const uint32_t pw = align_up(info.width, wide ? 16 : 8);
  const uint32_t ph = align_up(info.height, info.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8);
  if (padded_w != nullptr)
    *padded_w = pw;
  return static_cast<size_t>(pw) * ph * 2;
}

void ExtendedApiServices::on_cover() {
  this->cover_wanted_ = true;
  this->enable_loop();
}

void ExtendedApiServices::cover_sizes_() {
  for (size_t i = 0; i < this->cover_view_count_; i++) {
    CoverView &view = this->cover_views_[i];
    view.w = lv_obj_get_style_width(view.canvas, LV_PART_MAIN);
    view.h = lv_obj_get_style_height(view.canvas, LV_PART_MAIN);
    if (LV_COORD_IS_SPEC(view.w) || LV_COORD_IS_SPEC(view.h)) {
      lv_obj_update_layout(view.canvas);
      view.w = lv_obj_get_width(view.canvas);
      view.h = lv_obj_get_height(view.canvas);
    }
    view.capacity =
        view.w > 0 && view.h > 0 ? align_up(align_up(view.w, 16) * align_up(view.h, 16) * 2, COVER_ALIGN) : 0;
  }
  this->cover_sized_ = true;
}

void ExtendedApiServices::cover_detach_(CoverView &view) {
  if (view.shown_w == 0)
    return;
  lv_image_set_src(view.canvas, nullptr);
  view.shown_w = 0;
  view.shown_h = 0;
}

void ExtendedApiServices::cover_clear_views_() {
  for (size_t i = 0; i < this->cover_view_count_; i++)
    this->cover_detach_(this->cover_views_[i]);
  this->cover_shown_ = false;
}

void ExtendedApiServices::cover_release_() {
  heap_caps_free(this->cover_file_);
  this->cover_file_ = nullptr;
  if (this->cover_shown_)
    return;
  for (size_t i = 0; i < this->cover_view_count_; i++) {
    heap_caps_free(this->cover_views_[i].data);
    this->cover_views_[i].data = nullptr;
  }
}

void ExtendedApiServices::loop() {
  if (this->cover_view_count_ == 0 || this->relay_ == nullptr || !this->relay_->is_ready() ||
      this->lvgl_ == nullptr || this->lvgl_->is_paused())
    return;
  if (this->cover_step_ == CoverStep::FETCHING) {
    if (!relay_link::RelayLink::fetch_done(this->cover_fetch_)) {
      this->disable_loop();
      return;
    }
    this->cover_step_ = CoverStep::IDLE;
    const bool ok = this->cover_fetch_.ok && this->cover_fetch_.len <= COVER_FILE_MAX;
    if (this->cover_wanted_) {
      this->cover_view_ = 0;
    } else if (!ok) {
      this->cover_view_ = this->cover_view_count_;
    } else if (this->cover_fetch_.len == 0) {
      this->cover_clear_views_();
      this->cover_view_ = this->cover_view_count_;
    } else {
      CoverView &view = this->cover_current_();
      if (this->cover_decode_ok_.load()) {
        lv_draw_buf_init(&view.buf, view.shown_w, view.shown_h, LV_COLOR_FORMAT_RGB565, view.stride, view.data,
                         view.capacity);
        lv_canvas_set_draw_buf(view.canvas, &view.buf);
        lv_obj_invalidate(view.canvas);
        this->cover_shown_ = true;
      }
      this->cover_view_++;
    }
  }
  if (this->cover_wanted_) {
    this->cover_wanted_ = false;
    if (!this->cover_sized_)
      this->cover_sizes_();
    bool ok = true;
    for (size_t i = 0; ok && i < this->cover_view_count_; i++) {
      CoverView &view = this->cover_views_[i];
      if (view.data == nullptr && view.capacity != 0)
        view.data = static_cast<uint8_t *>(heap_caps_aligned_calloc(COVER_ALIGN, 1, view.capacity, MALLOC_CAP_SPIRAM));
      ok = view.data != nullptr;
    }
    if (ok && this->cover_file_ == nullptr)
      this->cover_file_ = static_cast<uint8_t *>(
          heap_caps_aligned_calloc(COVER_ALIGN, 1, align_up(COVER_FILE_MAX + 16, COVER_ALIGN), MALLOC_CAP_SPIRAM));
    if (!ok || this->cover_file_ == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for the cover art");
      this->cover_clear_views_();
      this->cover_release_();
      this->disable_loop();
      return;
    }
    size_t first = this->cover_view_count_;
    for (size_t i = 0; i < this->cover_view_count_; i++) {
      if (lv_obj_is_visible(this->cover_views_[i].canvas)) {
        first = i;
        break;
      }
    }
    size_t n = 0;
    if (first < this->cover_view_count_)
      this->cover_order_[n++] = static_cast<uint8_t>(first);
    for (size_t i = 0; i < this->cover_view_count_; i++) {
      if (i != first)
        this->cover_order_[n++] = static_cast<uint8_t>(i);
    }
    this->cover_view_ = 0;
  }
  if (this->cover_view_ >= this->cover_view_count_) {
    this->cover_release_();
    this->disable_loop();
    return;
  }
  CoverView &view = this->cover_current_();
  this->cover_detach_(view);
  this->cover_fetch_w_ = view.w;
  this->cover_fetch_h_ = view.h;
  this->cover_decode_ok_.store(false);
  if (!this->relay_->fetch_start(this->cover_fetch_, &ExtendedApiServices::cover_job_, this, this->cover_file_,
                                 COVER_FILE_MAX + 1))
    return;
  this->cover_step_ = CoverStep::FETCHING;
  this->disable_loop();
}

void ExtendedApiServices::cover_job_(void *ctx, uint16_t gen) { static_cast<ExtendedApiServices *>(ctx)->cover_task_(gen); }

void ExtendedApiServices::cover_task_(uint16_t gen) {
  uint8_t *into = nullptr;
  size_t cap = 0;
  if (!relay_link::RelayLink::fetch_begin(this->cover_fetch_, gen, &into, &cap)) {
    this->enable_loop_soon_any_context();
    return;
  }
  char path[96];
  std::snprintf(path, sizeof(path), "/cover?port=%u&w=%d&h=%d&max=%u",
                static_cast<unsigned>(this->relay_->get_stream_port()), static_cast<int>(this->cover_fetch_w_),
                static_cast<int>(this->cover_fetch_h_), static_cast<unsigned>(COVER_FILE_MAX));
  size_t len = 0;
  bool ok = this->relay_->http(path, nullptr, nullptr, &len, into, cap, 0, &this->cover_fetch_, gen);
  if (ok && len != 0 && len <= COVER_FILE_MAX)
    this->cover_decode_ok_.store(this->cover_decode_(into, len));
  relay_link::RelayLink::fetch_end(this->cover_fetch_, gen, ok, len);
  this->enable_loop_soon_any_context();
}

bool ExtendedApiServices::cover_decode_(uint8_t *file, size_t len) {
  CoverView &view = this->cover_current_();
  jpeg_decode_picture_info_t info{};
  if (jpeg_decoder_get_info(file, static_cast<uint32_t>(len), &info) != ESP_OK || info.width == 0 ||
      info.height == 0 || static_cast<int32_t>(info.width) > view.w || static_cast<int32_t>(info.height) > view.h ||
      decoded_size(info, nullptr) > view.capacity) {
    ESP_LOGW(TAG, "Cover art for %" PRId32 "x%" PRId32 ": not a JPEG that fits", view.w, view.h);
    return false;
  }
  jpeg_decoder_handle_t decoder = nullptr;
  jpeg_decode_engine_cfg_t engine{};
  engine.timeout_ms = 200;
  if (jpeg_new_decoder_engine(&engine, &decoder) != ESP_OK)
    return false;
  uint32_t pw = 0;
  decoded_size(info, &pw);
  jpeg_decode_cfg_t cfg{};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  esp_cache_msync(view.data, view.capacity, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  uint32_t out = 0;
  const bool ok = jpeg_decoder_process(decoder, &cfg, file, static_cast<uint32_t>(align_up(len, 16)), view.data,
                                       static_cast<uint32_t>(view.capacity), &out) == ESP_OK &&
                  out != 0;
  jpeg_del_decoder_engine(decoder);
  if (!ok) {
    ESP_LOGW(TAG, "Cover art: decoding failed");
    return false;
  }
  view.shown_w = static_cast<int32_t>(info.width);
  view.shown_h = static_cast<int32_t>(info.height);
  view.stride = pw * 2;
  return true;
}
#else
void ExtendedApiServices::loop() { this->disable_loop(); }
#endif

}

#endif
