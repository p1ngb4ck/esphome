#include "simple_video_player.h"

#include <cstdio>
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
  char json[128];
  snprintf(json, sizeof(json), "{\"svp\":1,\"port\":%u,\"audio\":%s,\"state\":\"%s\",\"volume\":%.2f,\"muted\":%s}",
           port,
#ifdef SVP_MEDIA_PLAYER
           this->media_player_ != nullptr ? "true" : "false",
#else
           "false",
#endif
           state, this->get_volume(), muted ? "true" : "false");
  return json;
}

#ifdef SVP_MEDIA_PLAYER
void SimpleVideoPlayer::setup_media_player_() {
  if (this->media_player_ == nullptr)
    return;
  // Home Assistant names these esphome.<device name>_svp_play_audio etc.
  this->register_service(&SimpleVideoPlayer::on_svp_play_audio, "svp_play_audio", {"url", "announce"});
  this->register_service(&SimpleVideoPlayer::on_svp_audio_command, "svp_audio_command", {"command"});
  this->register_service(&SimpleVideoPlayer::on_svp_set_volume, "svp_set_volume", {"volume"});
  this->media_player_->add_on_state_callback(
      [this](media_player::MediaPlayerState) { this->status_callbacks_.call(); });
}

void SimpleVideoPlayer::on_svp_play_audio(std::string url, bool announce) {
  ESP_LOGD(TAG, "Play audio%s: %s", announce ? " (announcement)" : "", url.c_str());
  this->media_player_->make_call().set_media_url(url).set_announcement(announce).perform();
}

void SimpleVideoPlayer::on_svp_audio_command(std::string command) {
  ESP_LOGD(TAG, "Audio command: %s", command.c_str());
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

}  // namespace esphome::simple_video_player
