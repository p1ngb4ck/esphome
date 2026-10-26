#pragma once

#include "esphome/core/defines.h"

#ifdef USE_MESSENGER_CALL

#include "esphome/components/mtproto/mtproto.h"
#include "esphome/components/resampler/microphone/resampler_microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/static_task.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace esphome::messenger {

enum class CallState : uint8_t { IDLE = 0, OUTGOING, RINGING, INCOMING, CONNECTING, ACTIVE, ENDED };

struct CallSession;

class TelegramCall {
 public:
  static constexpr uint32_t TASK_STACK = 16384;
  static constexpr UBaseType_t TASK_PRIORITY = 5;

  void attach(mtproto::Mtproto *mtproto, Component *owner);
  void set_audio(resampler::ResamplerMicrophone *microphone, speaker::Speaker *speaker);

  bool start(int64_t user_id, int64_t access_hash, const std::string &name);
  void accept();
  void hangup();
  void set_mute(bool mute) { this->mute_.store(mute); }
  bool muted() const { return this->mute_.load(); }
  CallState state() const { return this->state_.load(); }
  std::string peer_name() const;
  std::string error() const;
  uint32_t seconds() const;
  bool take_change() { return this->changed_.exchange(false); }

 protected:
  void on_updates_(mtproto::TlReader updates);
  void on_update_(mtproto::TlReader update);
  void on_phone_call_(mtproto::TlReader call);
  void post_(uint8_t type, const uint8_t *data, size_t length);
  void set_state_(CallState state);
  void fail_(const char *text);
  bool begin_(bool outgoing, int64_t call_id, int64_t call_hash, const uint8_t *g_a_hash);
  void finish_();

  static void task_entry_(void *param);
  void run_();
  bool rpc_(const uint8_t *body, size_t length, std::function<bool(mtproto::TlReader)> &&handler);
  void send_(const uint8_t *body, size_t length);
  bool handle_call_object_(CallSession *s, const uint8_t *data, size_t length, bool *hangup);
  bool start_media_(CallSession *s, mtproto::TlReader call);
  void stop_media_(CallSession *s);
  bool decrypt_signaling_(CallSession *s, const uint8_t *data, size_t length, std::string *json);
  void send_signaling_(CallSession *s, const std::string &json);
  void handle_signaling_(CallSession *s, const std::string &json);
  void on_local_sdp_(CallSession *s, const char *sdp);
  void on_local_candidate_(CallSession *s, const char *candidate);
  void send_initial_setup_(CallSession *s);
  void send_offer_(CallSession *s);
  void answer_offer_(CallSession *s);
  void send_candidate_(CallSession *s, const char *candidate);
  void set_remote_sdp_(CallSession *s);
  void pump_audio_(CallSession *s);
  void play_audio_(CallSession *s, const uint8_t *data, size_t length);

  mtproto::Mtproto *mtproto_{nullptr};
  Component *owner_{nullptr};
  resampler::ResamplerMicrophone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  std::atomic<CallState> state_{CallState::IDLE};
  std::atomic<bool> changed_{false};
  std::atomic<bool> mute_{false};
  std::atomic<bool> mic_on_{false};
  std::atomic<int64_t> call_id_{0};
  mutable Mutex lock_;
  char name_[64]{};
  char error_[64]{};
  uint32_t active_since_{0};

  StaticTask task_;
  CallSession *session_{nullptr};
  Mutex wait_lock_;
  uint32_t wait_serial_{0};
  bool wait_ok_{false};

  friend struct CallSession;
};

}

#endif
