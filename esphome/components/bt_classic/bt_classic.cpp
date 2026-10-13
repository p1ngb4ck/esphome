#include "bt_classic.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_bt_main.h>
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
#include "esphome/components/usb_bluetooth/hci_split.h"
#endif

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

std::vector<Handler> &fallbacks() {
  static std::vector<Handler> list;
  return list;
}

bool is_pairing_request(esp_bt_gap_cb_event_t event) {
  return event == ESP_BT_GAP_PIN_REQ_EVT || event == ESP_BT_GAP_CFM_REQ_EVT || event == ESP_BT_GAP_KEY_REQ_EVT;
}

bool registered = false;

void dispatch(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  bool answered = false;
  for (const auto &h : handlers())
    answered = h.handler(event, param, h.arg) || answered;
  if (answered || !is_pairing_request(event))
    return;
  for (const auto &h : fallbacks()) {
    if (h.handler(event, param, h.arg))
      return;
  }
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

void add_gap_fallback(GapHandler handler, void *arg) { fallbacks().push_back({handler, arg}); }

bool ensure_gap_callback() {
  bool up = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  // Bluedroid also runs on the co-processor alone (LE), until the dongle is there.
  up = up && usb_bluetooth::hci_split::br_up();
#endif
  if (!up) {
    registered = false;
    return false;
  }
  if (!registered) {
    if (esp_err_t err = esp_bt_gap_register_callback(dispatch); err != ESP_OK) {
      ESP_LOGE(TAG, "GAP callback registration failed: %s", esp_err_to_name(err));
      return false;
    }
    registered = true;
    // One setting for every component. DisplayYesNo: a keyboard types the passkey shown in
    // the log (passkey entry), a device with a display is confirmed (numeric comparison),
    // and one without input or output pairs with Just Works.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    // Variable: legacy PIN pairing asks through ESP_BT_GAP_PIN_REQ_EVT, per device.
    esp_bt_pin_code_t unused{};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused);
  }
  return true;
}

}  // namespace esphome::bt_classic

#endif  // USE_ESP32
