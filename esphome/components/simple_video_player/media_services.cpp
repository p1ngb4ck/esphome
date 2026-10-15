#include "simple_video_player.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "esphome/core/log.h"

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.media";

void SimpleVideoPlayer::set_volume(float volume) {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr)
    this->speaker_->set_volume(volume);
#endif
#ifdef SVP_MEDIA_PLAYER
  if (this->media_player_ != nullptr) {
    this->media_player_->make_call().set_volume(volume).perform();
    return;  // the media player's state callback reports the change
  }
#endif
  this->status_callbacks_.call();
}

float SimpleVideoPlayer::get_volume() {
#ifdef SVP_MEDIA_PLAYER
  if (this->media_player_ != nullptr)
    return this->media_player_->volume;
#endif
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr)
    return this->speaker_->get_volume();
#endif
  return 0.0f;
}

std::string SimpleVideoPlayer::status_json() {
  const char *state = "NONE";
  bool muted = false;
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr)
    muted = this->speaker_->get_mute_state();
#endif
#ifdef SVP_MEDIA_PLAYER
  if (this->media_player_ != nullptr) {
    state = media_player::media_player_state_to_string(this->media_player_->state);
    muted = this->media_player_->is_muted();
  }
#endif
  uint16_t port = 0;
#ifdef SVP_STREAM
  port = this->stream_port_;
#endif
  const char *traits = "\"pause\":false,\"formats\":[]";
#ifdef SVP_MEDIA_PLAYER
  if (this->media_player_ != nullptr)
    traits = this->media_traits_json_.c_str();
#endif
  bool playlist = false;
#ifdef SVP_AUDIO_FILES
  playlist = this->audio_playlist_active_();
#endif
  char json[160];
  snprintf(json, sizeof(json), "{\"svp\":1,\"port\":%u,\"state\":\"%s\",\"volume\":%.2f,\"muted\":%s,\"playlist\":%s,",
           port, state, this->get_volume(), muted ? "true" : "false", playlist ? "true" : "false");
  return std::string(json) + traits + "}";
}

#ifdef SVP_MEDIA_PLAYER
void SimpleVideoPlayer::setup_media_player_() {
  if (this->media_player_ == nullptr)
    return;
  // Home Assistant names these esphome.<device name>_svp_play_audio etc.
  this->register_service(&SimpleVideoPlayer::on_svp_play_audio, "svp_play_audio", {"url", "announce"});
  this->register_service(&SimpleVideoPlayer::on_svp_audio_command, "svp_audio_command", {"command"});
  this->register_service(&SimpleVideoPlayer::on_svp_set_volume, "svp_set_volume", {"volume"});
#ifdef SVP_STREAM_STATS
  this->register_service(&SimpleVideoPlayer::set_stream_statistics, "svp_stream_statistics", {"enable"});
#endif
  this->build_media_traits_();
  this->media_player_->add_on_state_callback([this](media_player::MediaPlayerState state) {
#if defined(SVP_AUDIO_FILES) && defined(SVP_CHANNEL_LIST)
    this->audio_remote_player_(state);
#endif
#ifdef SVP_SPICE
    // A SPICE session's sound comes first on the device they share.
    if ((state == media_player::MEDIA_PLAYER_STATE_PLAYING || state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING) &&
        this->spice_sound_shares_media_player_ && this->spice_sound_on_.load(std::memory_order_acquire))
      this->spice_sound_claim_.store(true, std::memory_order_release);
#endif
    this->status_callbacks_.call();
  });
  this->status_callbacks_.call();  // whatever published the status before this setup
}

void SimpleVideoPlayer::build_media_traits_() {
  if (this->media_player_ == nullptr)
    return;
  // Pause, and the formats the media player decodes, [format, rate, channels, sample bytes, purpose]: svp_relay
  // converts what it plays to one of them (Home Assistant's ESPHome ffmpeg proxy), as Home Assistant
  // does for an ESPHome media player it sees. Through an audio router: in its selected output's rate and
  // channel count, which the player passes on as it is.
  auto traits = this->media_player_->get_traits();
  std::string formats = traits.get_supports_pause() ? "\"pause\":true,\"formats\":[" : "\"pause\":false,\"formats\":[";
  const size_t start = formats.size();
  for (const auto &format : traits.get_supported_formats()) {
    uint32_t rate = format.sample_rate;
    uint32_t channels = format.num_channels;
#ifdef SVP_AUDIO_ROUTER
    if (this->audio_router_ != nullptr) {
      rate = this->wanted_audio_rate_.load(std::memory_order_acquire);
      channels = this->wanted_audio_channels_.load(std::memory_order_acquire);
    }
#endif
    char entry[64];
    snprintf(entry, sizeof(entry), "%s[\"%s\",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%u]", formats.size() > start ? "," : "",
             format.format.c_str(), rate, channels, format.sample_bytes, static_cast<unsigned>(format.purpose));
    if (formats.size() + strlen(entry) > 140)
      break;  // the status must stay within Home Assistant's 255 characters
    formats += entry;
  }
  this->media_traits_json_ = formats + "]";
}

void SimpleVideoPlayer::on_svp_play_audio(std::string url, bool announce) {
  ESP_LOGD(TAG, "Play audio%s: %s", announce ? " (announcement)" : "", url.c_str());
#if defined(SVP_AUDIO_FILES) && defined(SVP_CHANNEL_LIST)
  // svp_relay plays the queue's Home Assistant track asked for.
  uint8_t asked = AUDIO_REMOTE_ASKED;
  if (!announce && this->audio_remote_.compare_exchange_strong(asked, AUDIO_REMOTE_PLAYING, std::memory_order_acq_rel)) {
    this->audio_remote_play_ms_ = millis();
    this->audio_changed_.store(true, std::memory_order_release);
  }
#endif
  this->media_player_->make_call().set_media_url(url).set_announcement(announce).perform();
}

void SimpleVideoPlayer::on_svp_audio_command(std::string command) {
  ESP_LOGD(TAG, "Audio command: %s", command.c_str());
#if defined(SVP_AUDIO_FILES) && defined(SVP_CHANNEL_LIST)
  // Stopped from Home Assistant: the queue's track stops as on the panel (not its end: no next).
  if (strcasecmp(command.c_str(), "STOP") == 0 && this->audio_remote_.load(std::memory_order_acquire) != AUDIO_REMOTE_NONE) {
    this->stop();
    return;
  }
#endif
#ifdef USE_SPEAKER
  // Mute covers the video's sound too.
  if (this->speaker_ != nullptr) {
    if (strcasecmp(command.c_str(), "MUTE") == 0)
      this->speaker_->set_mute_state(true);
    else if (strcasecmp(command.c_str(), "UNMUTE") == 0)
      this->speaker_->set_mute_state(false);
  }
#endif
  this->media_player_->make_call().set_command(command).perform();
}

void SimpleVideoPlayer::on_svp_set_volume(float volume) { this->set_volume(clamp(volume, 0.0f, 1.0f)); }
#endif

#ifdef SVP_AUDIO_ROUTER
void SimpleVideoPlayer::on_audio_output_changed_() {
  // Main loop: the router switched outputs. Senders are told the new output's format; the relay
  // plays on in it.
  const audio::AudioStreamInfo &format = this->audio_router_->active_format();
  this->wanted_audio_rate_.store(format.get_sample_rate(), std::memory_order_release);
  this->wanted_audio_min_rate_.store(this->audio_router_->active_min_sample_rate(), std::memory_order_release);
  this->wanted_audio_max_rate_.store(this->audio_router_->active_max_sample_rate(), std::memory_order_release);
  this->wanted_audio_channels_.store(std::min<uint8_t>(format.get_channels(), AUDIO_SOURCE_CHANNELS),
                                     std::memory_order_release);
#ifdef SVP_STREAM
  this->stream_rate_pending_.store(this->speaker_ != nullptr, std::memory_order_release);
#endif
#ifdef SVP_MEDIA_PLAYER
  this->build_media_traits_();
#endif
  this->status_callbacks_.call();
}
#endif

}  // namespace esphome::simple_video_player
