#pragma once

#include "esphome/core/defines.h"

#ifdef USE_BT_OFFLOAD

#include "esphome/core/component.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/lock_free_queue.h"
#include "esphome/components/hid/hid_input.h"
#include "esphome/components/hid/hid_parser.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace esphome::bt_offload {

class BTOffloadSpeaker;

struct OffloadEvent {
  static constexpr size_t INLINE_MAX = 80;
  uint32_t msg_id{0};
  uint16_t len{0};
  uint8_t inline_data[INLINE_MAX];
  uint8_t *large{nullptr};
  const uint8_t *data() const { return this->large != nullptr ? this->large : this->inline_data; }
  void release();
};

class BTOffload : public Component, public hid::HIDInput {
 public:
  static constexpr uint8_t EVENT_SLOTS = 12;

  struct Found {
    std::array<uint8_t, 6> address;
    uint8_t transport;
    uint8_t addr_type;
    std::string name;
  };

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void scan(uint8_t seconds);
  bool connect(const std::string &address);
  bool audio_connect(const std::string &address);
  bool audio_disconnect();
  bool set_audio_volume(uint8_t volume);
  bool is_audio_connected() const { return this->audio_connected_; }
  void set_speaker(BTOffloadSpeaker *speaker) { this->speaker_ = speaker; }
  const std::vector<Found> &found() const { return this->found_; }

  void on_open_(const uint8_t *data, size_t len);
  void on_input_(const uint8_t *data, size_t len);
  void on_close_(const uint8_t *data, size_t len);
  void on_found_(const uint8_t *data, size_t len);
  void on_audio_state_(const uint8_t *data, size_t len);
  void on_audio_credit_(const uint8_t *data, size_t len);
  void receive_(uint32_t msg_id, const uint8_t *data, size_t len);

 protected:
  struct Device {
    std::array<uint8_t, 6> address;
    std::string name;
    std::unique_ptr<hid::HIDReportMap> map;
  };
  Device *device_(const uint8_t *address);
  bool command_(uint8_t type, const uint8_t *data, size_t len);

  std::vector<Device> devices_;
  std::vector<Found> found_;
  BTOffloadSpeaker *speaker_{nullptr};
  std::array<uint8_t, 6> audio_address_{};
  bool audio_connected_{false};
  EventPool<OffloadEvent, EVENT_SLOTS> pool_;
  LockFreeQueue<OffloadEvent, EVENT_SLOTS + 1> queue_;
};

}

#endif
