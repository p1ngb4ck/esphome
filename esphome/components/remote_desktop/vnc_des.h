#pragma once

#include <cstdint>

namespace esphome::remote_desktop {

void vnc_response(const char *password, const uint8_t challenge[16], uint8_t response[16]);

}
