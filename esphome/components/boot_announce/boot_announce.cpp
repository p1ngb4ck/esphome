#include "boot_announce.h"

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#ifdef USE_ESP32
#include <esp_attr.h>
#include <esp_system.h>
#endif

namespace esphome::boot_announce {

static const char *const TAG = "boot_announce";

static constexpr uint32_t ARMED = 0xB0075EE5;
static constexpr uint32_t RESEND_MS = 2000;  // without an answer, while the network is up
static constexpr uint32_t RESEND_ID = 0;

#ifdef USE_ESP32
static RTC_NOINIT_ATTR uint32_t s_armed;  // kept over a restart, a crash, an OTA reboot; random at power-on
#else
static uint32_t s_armed;
#endif

void BootAnnounce::watch() {
  s_armed = ARMED;
  ESP_LOGD(TAG, "Armed: the next boot is announced");
}

void BootAnnounce::setup() {
  if (s_armed != ARMED || this->udp_->is_failed())
    return;  // nobody waits (or no udp): nothing runs
  s_armed = 0;  // once: a later boot is announced only when armed again
  this->waiting_ = true;
  this->udp_->add_listener([this](std::span<const uint8_t> data) {
    static constexpr char ACK[] = " boot ack";
    const StringRef name = App.get_name();
    if (this->waiting_ && data.size() == name.size() + sizeof(ACK) - 1 &&
        memcmp(data.data(), name.c_str(), name.size()) == 0 &&
        memcmp(data.data() + name.size(), ACK, sizeof(ACK) - 1) == 0) {
      ESP_LOGD(TAG, "Boot acknowledged");
      this->waiting_ = false;
      this->stop_();
    }
  });
  // Online already (the network came up during setup): the event for it has passed
  if (network::is_connected())
    this->start_();
}

void BootAnnounce::on_active_interface(network::NetworkInterface interface) {
  if (!this->waiting_)
    return;
  if (interface == network::NetworkInterface::NETWORK_INTERFACE_NONE) {
    this->stop_();  // offline: nothing would arrive; the next online event starts again
  } else {
    this->start_();
  }
}

void BootAnnounce::start_() {
  this->send_();
  // UDP has no delivery: resent until the receiver's ack cancels it
  this->set_interval(RESEND_ID, RESEND_MS, [this]() { this->send_(); });
}

void BootAnnounce::stop_() { this->cancel_interval(RESEND_ID); }

void BootAnnounce::send_() {
  char built[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(built);
  int reset = 0;
#ifdef USE_ESP32
  reset = static_cast<int>(esp_reset_reason());
#endif
  char text[128];
  const int n = snprintf(text, sizeof(text), "%s boot built=%s reset=%d uptime_ms=%" PRIu32, App.get_name().c_str(),
                         built, reset, millis());
  if (n > 0)
    this->udp_->send_packet(reinterpret_cast<const uint8_t *>(text), std::min<size_t>(n, sizeof(text) - 1));
}

void BootAnnounce::dump_config() { ESP_LOGCONFIG(TAG, "Boot announce (armed by boot_announce.watch)"); }

}  // namespace esphome::boot_announce
