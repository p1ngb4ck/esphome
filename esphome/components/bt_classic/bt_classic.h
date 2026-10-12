#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <esp_gap_bt_api.h>

namespace esphome::bt_classic {

/// Runs on Bluedroid's BTC task for every Classic GAP event. Returns true when it answered a
/// pairing request (PIN, SSP confirmation or passkey) for that device; a request nobody
/// answers is refused.
using GapHandler = bool (*)(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg);

/// Main loop, before Bluedroid comes up; handlers are never removed.
void add_gap_handler(GapHandler handler, void *arg);

/// Main loop: registers the dispatcher with Bluedroid once it is enabled (again after it was
/// disabled). True while registered.
bool ensure_gap_callback();

}  // namespace esphome::bt_classic

#endif  // USE_ESP32
