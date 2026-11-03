#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <esp_gap_bt_api.h>

namespace esphome::bt_host {

/// Runs on Bluedroid's BTC task for every Classic GAP event. Returns true when it answered a
/// pairing request (PIN, SSP confirmation or passkey) for that device; a request nobody
/// answers is refused.
using GapHandler = bool (*)(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param, void *arg);

/// Main loop, before Bluedroid comes up; handlers are never removed.
void add_gap_handler(GapHandler handler, void *arg);
/// Offered a pairing request no handler answered, in order until one answers it; a device
/// no component knows yet (pairing started from its side).
void add_gap_fallback(GapHandler handler, void *arg);

/// Bluedroid runs with a controller for Classic.
bool is_active();
/// Main loop: registers the dispatcher with Bluedroid once it is enabled (again after it was
/// disabled), and sets the IO capability and PIN type all components share. True while
/// registered.
bool ensure_gap_callback();

enum ScanOwner : uint8_t { SCAN_OWNER_HID = 0, SCAN_OWNER_AUDIO = 1, SCAN_OWNERS = 2 };

esp_err_t start_discovery(uint8_t units);
void cancel_discovery();
void accept_ssp(const uint8_t *bda);
esp_err_t remove_bond(const uint8_t *bda, bool le);
void set_scan_mode(ScanOwner owner, bool connectable, bool discoverable);

}  // namespace esphome::bt_host

#endif  // USE_ESP32
