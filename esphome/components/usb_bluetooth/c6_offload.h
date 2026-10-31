#pragma once

#include "esphome/core/defines.h"

#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD

#include <cstddef>
#include <cstdint>

namespace esphome::usb_bluetooth {

class USBBluetooth;

namespace c6_offload {

bool start(USBBluetooth *dongle);
void up(const uint8_t *h4, size_t len);
void send_available(bool available);
bool post_filled(uint32_t msg_id, size_t len, size_t (*fill)(uint8_t *dst, size_t len, void *ctx), void *ctx);

}
}

#endif
