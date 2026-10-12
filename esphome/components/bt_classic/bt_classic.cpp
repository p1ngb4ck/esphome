#include "bt_classic.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_bt_main.h>

#include <vector>

namespace esphome::bt_classic {

static const char *const TAG = "bt_classic";

namespace {

struct Handler {
  GapHandler handler;
  void *arg;
};

// Complete before Bluedroid comes up, read by the BTC task without a lock.
std::vector<Handler> &handlers() {
  static std::vector<Handler> list;
  return list;
}

bool registered = false;

void dispatch(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  bool answered = false;
  for (const auto &h : handlers())
    answered = h.handler(event, param, h.arg) || answered;
  if (answered)
    return;
  switch (event) {
    case ESP_BT_GAP_PIN_REQ_EVT: {
      esp_bt_pin_code_t code{};
      esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, code);
      break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
      esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
      break;
    case ESP_BT_GAP_KEY_REQ_EVT:
      esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
      break;
    default:
      break;
  }
}

}  // namespace

void add_gap_handler(GapHandler handler, void *arg) { handlers().push_back({handler, arg}); }

bool ensure_gap_callback() {
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
    registered = false;
    return false;
  }
  if (!registered) {
    if (esp_err_t err = esp_bt_gap_register_callback(dispatch); err != ESP_OK) {
      ESP_LOGE(TAG, "GAP callback registration failed: %s", esp_err_to_name(err));
      return false;
    }
    registered = true;
  }
  return true;
}

}  // namespace esphome::bt_classic

#endif  // USE_ESP32
