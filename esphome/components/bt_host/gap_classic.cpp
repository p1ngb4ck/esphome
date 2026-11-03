#include "gap_classic.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_bt_main.h>
#include <sdkconfig.h>
#ifdef CONFIG_BT_BLE_ENABLED
#include <esp_gap_ble_api.h>
#endif

#include <atomic>
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
#include "esphome/components/usb_bluetooth/hci_split.h"
#endif

#include <vector>

namespace esphome::bt_host {

static const char *const TAG = "bt_host";

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
std::atomic<uint8_t> discovery_users{0};
bool scan_connectable[SCAN_OWNERS]{};
bool scan_discoverable[SCAN_OWNERS]{};

void dispatch(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT && param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED)
    discovery_users.store(0);
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

bool is_active() {
  bool up = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  // Bluedroid also runs on the co-processor alone (LE), until the dongle is there.
  up = up && usb_bluetooth::hci_split::br_up();
#endif
  return up;
}

bool ensure_gap_callback() {
  if (!is_active()) {
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

esp_err_t start_discovery(uint8_t units) {
  if (discovery_users.fetch_add(1) != 0)
    return ESP_OK;
  const esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, units, 0);
  if (err != ESP_OK)
    discovery_users.store(0);
  return err;
}

void cancel_discovery() {
  uint8_t users = discovery_users.load();
  while (users != 0 && !discovery_users.compare_exchange_weak(users, users - 1)) {
  }
  if (users <= 1)
    esp_bt_gap_cancel_discovery();
}

void accept_ssp(const uint8_t *bda) { esp_bt_gap_ssp_confirm_reply(const_cast<uint8_t *>(bda), true); }

esp_err_t remove_bond(const uint8_t *bda, bool le) {
#ifdef CONFIG_BT_BLE_ENABLED
  if (le)
    return esp_ble_remove_bond_device(const_cast<uint8_t *>(bda));
#endif
  if (le)
    return ESP_ERR_NOT_SUPPORTED;
  return esp_bt_gap_remove_bond_device(const_cast<uint8_t *>(bda));
}

void set_scan_mode(ScanOwner owner, bool connectable, bool discoverable) {
  scan_connectable[owner] = connectable;
  scan_discoverable[owner] = discoverable;
  bool any_connectable = false;
  bool any_discoverable = false;
  for (uint8_t i = 0; i != SCAN_OWNERS; i++) {
    any_connectable = any_connectable || scan_connectable[i];
    any_discoverable = any_discoverable || scan_discoverable[i];
  }
  esp_bt_gap_set_scan_mode(any_connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
                           any_discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

}  // namespace esphome::bt_host

#endif  // USE_ESP32
