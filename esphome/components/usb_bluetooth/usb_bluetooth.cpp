#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_bluetooth.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"

#ifdef USE_USB_BLUETOOTH_BLUEDROID
#include "esp_bluedroid_hci.h"
#include "esp_bt_main.h"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::usb_bluetooth {

static const char *const TAG = "usb_bluetooth";

// HCI over USB: a command is a class request to the device on endpoint 0.
static constexpr uint8_t HCI_COMMAND_REQUEST_TYPE =
    USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
static constexpr uint8_t HCI_EVENT_COMMAND_COMPLETE = 0x0E;
static constexpr uint32_t COMMAND_TIMEOUT_MS = 3000;

static constexpr uint16_t OP_RESET = 0x0C03;
static constexpr uint16_t OP_READ_LOCAL_NAME = 0x0C14;
static constexpr uint16_t OP_READ_LOCAL_VERSION = 0x1001;
static constexpr uint16_t OP_READ_BD_ADDR = 0x1009;
static constexpr uint16_t PROBE[] = {OP_RESET, OP_READ_LOCAL_VERSION, OP_READ_BD_ADDR, OP_READ_LOCAL_NAME};

static constexpr uint16_t MANUFACTURER_REALTEK = 93;

#ifdef USE_USB_BLUETOOTH_BLUEDROID
// H4 packet types, the byte Bluedroid's transport puts in front of every packet.
static constexpr uint8_t H4_COMMAND = 0x01;
static constexpr uint8_t H4_ACL = 0x02;
static constexpr uint8_t H4_SCO = 0x03;
static constexpr uint8_t H4_EVENT = 0x04;
// Transfer slots a send leaves free (the two readers hold theirs all the time).
static constexpr size_t SPARE_SLOTS = 1;

// The transport operations are plain C function pointers without a user argument.
static USBBluetooth *g_instance = nullptr;
static const esp_bluedroid_hci_driver_callbacks_t *g_host_cb = nullptr;
static bool g_stack_started = false;

static void hci_drv_send(uint8_t *data, uint16_t len) {
  if (g_instance != nullptr)
    g_instance->hci_send(data, len);
}
static bool hci_drv_check_send_available() { return g_instance != nullptr && g_instance->hci_send_available(); }
static esp_err_t hci_drv_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback) {
  g_host_cb = callback;
  return ESP_OK;
}
static const esp_bluedroid_hci_driver_operations_t HCI_OPS = {
    .send = hci_drv_send,
    .check_send_available = hci_drv_check_send_available,
    .register_host_callback = hci_drv_register_host_callback,
};

// esp_bluedroid_enable() waits for the controller to answer Bluedroid's start-up commands,
// which need the main loop running: it gets a task of its own.
static void bluedroid_start_task(void * /*arg*/) {
  esp_err_t err = esp_bluedroid_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(err));
  } else if ((err = esp_bluedroid_enable()) != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "Bluedroid running on the USB dongle");
  }
  vTaskDelete(nullptr);
}
#endif

// Bluetooth SIG assigned numbers: HCI/LMP version.
static const char *core_version(uint8_t version) {
  static const char *const NAMES[] = {"1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1",
                                      "4.2",  "5.0", "5.1", "5.2", "5.3", "5.4", "6.0", "6.1"};
  return version < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[version] : "?";
}

// -- Device ------------------------------------------------------------------------------

bool USBBluetooth::find_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;
  // FF/01/01 is accepted only for a dongle named by vid/pid: the vendor class says nothing
  // about Bluetooth on its own.
  const bool vendor_allowed = this->vid_ != 0 || this->pid_ != 0;
  this->intf_ = 0xFF;
  this->evt_ep_ = this->acl_in_ep_ = this->acl_out_ep_ = 0;

  bool in_hci = false;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_hci = false;
      if (this->intf_ != 0xFF || intf->bAlternateSetting != 0 || intf->bInterfaceSubClass != 0x01 ||
          intf->bInterfaceProtocol != 0x01)
        continue;
      if (intf->bInterfaceClass == HCI_CLASS || (vendor_allowed && intf->bInterfaceClass == 0xFF)) {
        this->intf_ = intf->bInterfaceNumber;
        this->vendor_class_ = intf->bInterfaceClass == 0xFF;
        in_hci = true;
      }
      continue;
    }
    if (desc->bDescriptorType != USB_W_VALUE_DT_ENDPOINT || !in_hci)
      continue;
    const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
    const uint8_t type = USB_EP_DESC_GET_XFERTYPE(ep);
    const bool in = USB_EP_DESC_GET_EP_DIR(ep);
    if (type == USB_BM_ATTRIBUTES_XFER_INT && in) {
      this->evt_ep_ = ep->bEndpointAddress;
      this->evt_mps_ = USB_EP_DESC_GET_MPS(ep);
    } else if (type == USB_BM_ATTRIBUTES_XFER_BULK && in) {
      this->acl_in_ep_ = ep->bEndpointAddress;
      this->acl_in_mps_ = USB_EP_DESC_GET_MPS(ep);
    } else if (type == USB_BM_ATTRIBUTES_XFER_BULK) {
      this->acl_out_ep_ = ep->bEndpointAddress;
      this->acl_out_mps_ = USB_EP_DESC_GET_MPS(ep);
    }
  }
  if (this->intf_ == 0xFF)
    return false;
  if (this->evt_ep_ == 0 || this->acl_in_ep_ == 0 || this->acl_out_ep_ == 0 || this->evt_mps_ == 0 ||
      this->acl_in_mps_ == 0 || this->acl_out_mps_ == 0) {
    ESP_LOGE(TAG, "Interface %u lacks the event or ACL endpoints", this->intf_);
    return false;
  }
  return true;
}

void USBBluetooth::on_connected() {
  const uint32_t generation = this->generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!this->find_interface_()) {
    ESP_LOGW(TAG, "No Bluetooth controller interface on this device");
    this->report_connected_();
    return;
  }
  if (!this->claim_interface(this->intf_, 0)) {
    ESP_LOGE(TAG, "Claiming interface %u failed", this->intf_);
    this->intf_ = 0xFF;
    this->report_connected_();
    return;
  }
  ESP_LOGI(TAG, "Bluetooth controller on interface %u%s (events 0x%02X, ACL 0x%02X/0x%02X)", this->intf_,
           this->vendor_class_ ? " (vendor class)" : "", this->evt_ep_, this->acl_in_ep_, this->acl_out_ep_);
  this->event_len_ = 0;
  this->acl_len_ = 0;
  this->link_up_.store(true, std::memory_order_release);
  this->read_event_(generation);
  this->read_acl_(generation);
  this->probe_step_ = 0;
  this->probe_next_();
}

void USBBluetooth::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->link_up_.store(false, std::memory_order_release);
  this->stack_attached_.store(false, std::memory_order_release);
  this->cancel_timeout("hci");
  if (this->intf_ != 0xFF) {
    this->release_interface(this->intf_);
    this->intf_ = 0xFF;
    ESP_LOGI(TAG, "Bluetooth controller removed");
  }
  usb_host::USBClient::on_disconnected();
}

// -- Transport ---------------------------------------------------------------------------

bool USBBluetooth::send_command(const uint8_t *packet, size_t len) {
  if (this->intf_ == 0xFF || len < 3 || len != 3u + packet[2])
    return false;
  std::vector<uint8_t> data(packet, packet + len);
  const uint16_t opcode = static_cast<uint16_t>(packet[0] | (packet[1] << 8));
  return this->control_transfer(
      HCI_COMMAND_REQUEST_TYPE, 0, 0, this->intf_,
      [this, opcode](const usb_host::TransferStatus &status) {
        if (!status.success)
          ESP_LOGW(TAG, "HCI command 0x%04X not taken (status %u)", opcode, status.error_code);
        this->send_done_();
      },
      data);
}

// USB task.
void USBBluetooth::send_done_() {
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  if (this->send_blocked_.exchange(false, std::memory_order_acq_rel) && g_host_cb != nullptr &&
      g_host_cb->notify_host_send_available != nullptr)
    g_host_cb->notify_host_send_available();
#endif
}

#ifdef USE_USB_BLUETOOTH_BLUEDROID
bool USBBluetooth::hci_send_available() {
  const size_t used = __builtin_popcount(this->get_trq_in_use());
  const bool available =
      this->link_up_.load(std::memory_order_acquire) && used + 1 + SPARE_SLOTS <= usb_host::MAX_REQUESTS;
  if (!available)
    this->send_blocked_.store(true, std::memory_order_release);
  return available;
}

// Bluedroid's HCI task.
void USBBluetooth::hci_send(uint8_t *data, uint16_t len) {
  if (!this->link_up_.load(std::memory_order_acquire) || data == nullptr || len < 2)
    return;
  switch (data[0]) {
    case H4_COMMAND:
      if (!this->send_command(data + 1, len - 1))
        ESP_LOGW(TAG, "HCI command 0x%02X%02X dropped", data[2], data[1]);
      break;
    case H4_ACL:
      if (!this->send_acl_(data + 1, len - 1))
        ESP_LOGW(TAG, "ACL packet of %u bytes dropped", len - 1);
      break;
    case H4_SCO: {
      static bool said = false;
      if (!said) {
        said = true;
        ESP_LOGW(TAG, "SCO (voice) is not carried over this transport");
      }
      break;
    }
    default:
      break;
  }
}

// One ACL packet as consecutive bulk OUT transfers: each but the last a whole number of
// packets, so the device sees one unbroken transfer.
bool USBBluetooth::send_acl_(const uint8_t *data, size_t len) {
  const size_t capacity = this->transfer_capacity();
  const size_t chunk = (capacity / this->acl_out_mps_) * this->acl_out_mps_;
  if (chunk == 0)
    return false;
  const size_t needed = (len + chunk - 1) / chunk;
  const size_t used = __builtin_popcount(this->get_trq_in_use());
  if (used + needed + SPARE_SLOTS > usb_host::MAX_REQUESTS)
    return false;
  for (size_t offset = 0; offset < len; offset += chunk) {
    const size_t n = std::min(chunk, len - offset);
    if (!this->transfer_out(
            this->acl_out_ep_,
            [this](const usb_host::TransferStatus &status) {
              if (!status.success)
                ESP_LOGW(TAG, "ACL data not taken (status %u)", status.error_code);
              this->send_done_();
            },
            data + offset, static_cast<uint16_t>(n)))
      return false;
  }
  return true;
}

void USBBluetooth::start_bluedroid_() {
  if (g_stack_started) {
    // Bluedroid is attached once, before it starts, and keeps the state of the controller it
    // started with.
    ESP_LOGW(TAG, "Bluedroid was started with the previous dongle; restart to use this one");
    return;
  }
  g_instance = this;
  esp_err_t err = esp_bluedroid_attach_hci_driver(&HCI_OPS);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Bluedroid did not take the transport: %s", esp_err_to_name(err));
    return;
  }
  this->stack_attached_.store(true, std::memory_order_release);
  g_stack_started = true;
  if (xTaskCreate(bluedroid_start_task, "usb_bt_start", 4096, nullptr, 5, nullptr) != pdPASS)
    ESP_LOGE(TAG, "Starting Bluedroid failed: no task");
}
#endif

// USB task.
void USBBluetooth::read_event_(uint32_t generation) {
  const bool submitted = this->transfer_in(
      this->evt_ep_,
      [this, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          ESP_LOGW(TAG, "Event endpoint stopped (status %u)", status.error_code);
          return;
        }
        uint8_t *event = this->event_ + 1;
        const size_t take = std::min(status.data_len, EVENT_MAX - this->event_len_);
        memcpy(event + this->event_len_, status.data, take);
        this->event_len_ += take;
        // An event: code, parameter length, parameters.
        if (this->event_len_ >= 2 && this->event_len_ >= 2u + event[1]) {
          const size_t len = 2u + event[1];
          this->event_len_ = 0;
#ifdef USE_USB_BLUETOOTH_BLUEDROID
          if (this->stack_attached_.load(std::memory_order_acquire)) {
            this->event_[0] = H4_EVENT;
            if (g_host_cb != nullptr && g_host_cb->notify_host_recv != nullptr)
              g_host_cb->notify_host_recv(this->event_, static_cast<uint16_t>(len + 1));
            this->read_event_(generation);
            return;
          }
#endif
          this->defer([this, generation, copy = std::vector<uint8_t>(event, event + len)]() {
            if (generation == this->generation_.load(std::memory_order_acquire))
              this->handle_event_(copy);
          });
        }
        this->read_event_(generation);
      },
      this->evt_mps_);
  if (!submitted)
    ESP_LOGE(TAG, "Event read not submitted");
}

// USB task.
void USBBluetooth::read_acl_(uint32_t generation) {
  const bool submitted = this->transfer_in(
      this->acl_in_ep_,
      [this, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          ESP_LOGW(TAG, "ACL endpoint stopped (status %u)", status.error_code);
          return;
        }
        uint8_t *acl = this->acl_ + 1;
        const size_t take = std::min(status.data_len, ACL_MAX - this->acl_len_);
        memcpy(acl + this->acl_len_, status.data, take);
        this->acl_len_ += take;
        // ACL: handle and flags, little-endian data length, data. Only Bluedroid takes it.
        if (this->acl_len_ >= 4 && this->acl_len_ >= 4u + (acl[2] | (acl[3] << 8))) {
#ifdef USE_USB_BLUETOOTH_BLUEDROID
          if (this->stack_attached_.load(std::memory_order_acquire) && g_host_cb != nullptr &&
              g_host_cb->notify_host_recv != nullptr) {
            this->acl_[0] = H4_ACL;
            g_host_cb->notify_host_recv(this->acl_, static_cast<uint16_t>(this->acl_len_ + 1));
          }
#endif
          this->acl_len_ = 0;
        }
        this->read_acl_(generation);
      },
      this->acl_in_mps_);
  if (!submitted)
    ESP_LOGE(TAG, "ACL read not submitted");
}

// -- Probe (main loop) -------------------------------------------------------------------

void USBBluetooth::probe_next_() {
  if (this->probe_step_ >= sizeof(PROBE) / sizeof(PROBE[0])) {
    this->probe_done_(true);
    return;
  }
  this->probe_opcode_ = PROBE[this->probe_step_];
  const uint8_t command[3] = {static_cast<uint8_t>(this->probe_opcode_ & 0xFF),
                              static_cast<uint8_t>(this->probe_opcode_ >> 8), 0};
  if (!this->send_command(command, sizeof(command))) {
    ESP_LOGE(TAG, "HCI command 0x%04X not submitted", this->probe_opcode_);
    this->probe_done_(false);
    return;
  }
  this->set_timeout("hci", COMMAND_TIMEOUT_MS, [this]() {
    ESP_LOGE(TAG, "No answer to HCI command 0x%04X", this->probe_opcode_);
    this->probe_done_(false);
  });
}

void USBBluetooth::probe_done_(bool ok) {
  this->probe_opcode_ = 0;
  if (ok && this->manufacturer_ == MANUFACTURER_REALTEK)
    ESP_LOGW(TAG, "Realtek controller: it runs its ROM until a firmware is loaded, which this does not do yet");
  this->report_connected_();
#ifdef USE_USB_BLUETOOTH_BLUEDROID
  if (ok)
    this->start_bluedroid_();
#endif
}

void USBBluetooth::handle_event_(const std::vector<uint8_t> &event) {
  // Command Complete: code, length, Num_HCI_Command_Packets, opcode, status, return parameters.
  if (event[0] != HCI_EVENT_COMMAND_COMPLETE || event.size() < 6) {
    ESP_LOGV(TAG, "Event 0x%02X, %u bytes", event[0], static_cast<unsigned>(event.size()));
    return;
  }
  const uint16_t opcode = static_cast<uint16_t>(event[3] | (event[4] << 8));
  if (opcode != this->probe_opcode_ || opcode == 0)
    return;
  this->cancel_timeout("hci");
  const uint8_t status = event[5];
  if (status != 0) {
    ESP_LOGE(TAG, "HCI command 0x%04X failed, status 0x%02X", opcode, status);
    this->probe_done_(false);
    return;
  }
  const uint8_t *p = event.data() + 6;
  const size_t n = event.size() - 6;
  switch (opcode) {
    case OP_RESET:
      ESP_LOGD(TAG, "Controller reset");
      break;
    case OP_READ_LOCAL_VERSION:
      if (n >= 8) {
        this->manufacturer_ = static_cast<uint16_t>(p[4] | (p[5] << 8));
        ESP_LOGI(TAG, "HCI %u (Bluetooth %s), revision %u, LMP subversion 0x%04X, manufacturer %u", p[0],
                 core_version(p[0]), static_cast<unsigned>(p[1] | (p[2] << 8)),
                 static_cast<unsigned>(p[6] | (p[7] << 8)), this->manufacturer_);
      }
      break;
    case OP_READ_BD_ADDR:
      if (n >= 6)
        ESP_LOGI(TAG, "Address %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
      break;
    case OP_READ_LOCAL_NAME: {
      const size_t len = strnlen(reinterpret_cast<const char *>(p), n);
      ESP_LOGI(TAG, "Name '%.*s'", static_cast<int>(len), reinterpret_cast<const char *>(p));
      break;
    }
    default:
      break;
  }
  this->probe_step_++;
  this->probe_next_();
}

}  // namespace esphome::usb_bluetooth

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
