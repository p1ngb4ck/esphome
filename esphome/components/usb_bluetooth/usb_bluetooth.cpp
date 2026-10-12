#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_bluetooth.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"

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
    }
  }
  if (this->intf_ == 0xFF)
    return false;
  if (this->evt_ep_ == 0 || this->acl_in_ep_ == 0 || this->acl_out_ep_ == 0 || this->evt_mps_ == 0 ||
      this->acl_in_mps_ == 0) {
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
  this->read_event_(generation);
  this->read_acl_(generation);
  this->probe_step_ = 0;
  this->probe_next_();
}

void USBBluetooth::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
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
      [opcode](const usb_host::TransferStatus &status) {
        if (!status.success)
          ESP_LOGW(TAG, "HCI command 0x%04X not taken (status %u)", opcode, status.error_code);
      },
      data);
}

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
        const size_t take = std::min(status.data_len, sizeof(this->event_) - this->event_len_);
        memcpy(this->event_ + this->event_len_, status.data, take);
        this->event_len_ += take;
        // An event: code, parameter length, parameters.
        if (this->event_len_ >= 2 && this->event_len_ >= 2u + this->event_[1]) {
          std::vector<uint8_t> event(this->event_, this->event_ + 2 + this->event_[1]);
          this->event_len_ = 0;
          this->defer([this, generation, event = std::move(event)]() {
            if (generation == this->generation_.load(std::memory_order_acquire))
              this->handle_event_(event);
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
        const size_t take = std::min(status.data_len, sizeof(this->acl_) - this->acl_len_);
        memcpy(this->acl_ + this->acl_len_, status.data, take);
        this->acl_len_ += take;
        // ACL: handle and flags, little-endian data length, data. Not used yet: dropped.
        if (this->acl_len_ >= 4 && this->acl_len_ >= 4u + (this->acl_[2] | (this->acl_[3] << 8)))
          this->acl_len_ = 0;
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
