#include "usb_hid.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/log.h"
#include "driver_registry.h"

#include <cstring>
#include <map>

namespace esphome {
namespace usb_hid {

static const char *const TAG = "usb_hid";

// ── Per-endpoint persistent transfer helper ──────────────────────────────────
// Submits a new transfer_in and, in the callback, dispatches + resubmits.
// Using a plain member function avoids unbounded lambda nesting.

void USBHIDClient::start_in_transfer_(HIDDevice *dev, uint8_t ep, uint16_t mps, bool media) {
  this->transfer_in(
      ep,
      [this, dev, ep, mps, media](const usb_host::TransferStatus &ts) {
        if (ts.success && ts.data_len > 0)
          this->on_transfer_in_(ep, ts.data, ts.data_len, media);
        // Resubmit while the device is still active
        if (dev->active && (media ? dev->media_active : true))
          this->start_in_transfer_(dev, ep, mps, media);
      },
      mps);
}

void USBHIDClient::setup() {
  register_all_drivers(this);
  ESP_LOGI(TAG, "Registered %zu device drivers", this->drivers_.size());
  usb_host::USBClient::setup();
}

void USBHIDClient::loop() {
  usb_host::USBClient::loop();
}

#ifdef USE_BINARY_SENSOR
void USBHIDClient::register_keyboard_key_sensor(binary_sensor::BinarySensor *sensor, uint8_t keycode) {
  this->keyboard_key_sensors_[keycode] = sensor;
}
#endif

// ── on_connected ────────────────────────────────────────────────────────────

void USBHIDClient::on_connected() {
  if (!this->parse_hid_interface_()) {
    ESP_LOGD(TAG, "Not a HID device or no usable interface, closing");
    this->disconnect();
  }
}

// ── parse_hid_interface_ ─────────────────────────────────────────────────────

bool USBHIDClient::parse_hid_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  const usb_device_desc_t *dev_desc = this->get_device_desc_();
  if (!cfg || !dev_desc)
    return false;

  uint16_t vid = dev_desc->idVendor;
  uint16_t pid = dev_desc->idProduct;

  bool is_xbox360 = (vid == XBOX360_VID && (pid == XBOX360_PID_WIRED || pid == XBOX360_PID_WIRELESS)) ||
                    (vid == BITDO_VID && pid == BITDO_PID_XBOX);
  bool is_8bitdo = (vid == BITDO_VID && pid != BITDO_PID_XBOX);

  ESP_LOGI(TAG, "USB device VID:PID = %04X:%04X DevClass=0x%02X", vid, pid, dev_desc->bDeviceClass);

  if (dev_desc->bDeviceClass != 0x03 && dev_desc->bDeviceClass != 0x00 && !is_xbox360 && !is_8bitdo) {
    ESP_LOGD(TAG, "Device class 0x%02X not handled", dev_desc->bDeviceClass);
    return false;
  }

  const usb_intf_desc_t *hid_intf = nullptr;
  const usb_ep_desc_t *in_ep_desc = nullptr;
  uint8_t out_ep = 0;
  int offset = 0;

  // Find a usable interface.
  //
  // Linux binds usbhid to every interface of class 0x03 and looks at nothing else --
  // hid_usb_ids[] carries USB_DEVICE_ID_MATCH_INT_CLASS and no interface number -- and only
  // then reads bInterfaceNumber to address its control requests. Restricting the search to
  // interface 0 kept this from ever seeing the HID interface of a composite device such as a
  // sound card, where interface 0 is AudioControl and the buttons sit further down.
  //
  // The vendor-class exception stays on interface 0: those devices are not HID at all and
  // are recognised by their vid and pid, so widening the search would only make an unrelated
  // vendor interface look like one of them.
  while (offset < cfg->wTotalLength) {
    const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(
        reinterpret_cast<const uint8_t *>(cfg) + offset);

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      bool cls_ok = intf->bInterfaceClass == USB_CLASS_HID ||
                    ((is_xbox360 || is_8bitdo) && intf->bInterfaceClass == 0xFF &&
                     intf->bInterfaceNumber == 0);
      if (cls_ok && hid_intf == nullptr)
        hid_intf = intf;
    }
    offset += desc->bLength;
  }

  if (!hid_intf) {
    ESP_LOGW(TAG, "No HID or vendor interface on this device");
    return false;
  }

  // Scan endpoints for this interface
  offset = reinterpret_cast<const uint8_t *>(hid_intf) - reinterpret_cast<const uint8_t *>(cfg) +
           hid_intf->bLength;
  while (offset < cfg->wTotalLength) {
    const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(
        reinterpret_cast<const uint8_t *>(cfg) + offset);

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
      break;

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if ((ep->bmAttributes & EP_TYPE_MASK) == EP_TYPE_INTERRUPT) {
        if (ep->bInterval == 0)
          const_cast<usb_ep_desc_t *>(ep)->bInterval = 1;
        if (ep->bEndpointAddress & EP_DIR_IN) {
          in_ep_desc = ep;
          ESP_LOGI(TAG, "INT IN ep 0x%02X mps=%d", ep->bEndpointAddress, ep->wMaxPacketSize);
        } else {
          out_ep = ep->bEndpointAddress;
          ESP_LOGI(TAG, "INT OUT ep 0x%02X", ep->bEndpointAddress);
        }
      }
    }
    offset += desc->bLength;
  }

  if (!in_ep_desc) {
    ESP_LOGW(TAG, "No interrupt IN endpoint found");
    return false;
  }

  if (!this->claim_interface(hid_intf->bInterfaceNumber)) {
    ESP_LOGE(TAG, "Failed to claim HID interface %d", hid_intf->bInterfaceNumber);
    return false;
  }

  HIDDevice *d = &this->device_;
  d->dev_hdl = this->device_handle_;
  d->dev_addr = static_cast<uint8_t>(this->device_addr_);
  d->vid = vid;
  d->pid = pid;
  d->protocol = hid_intf->bInterfaceProtocol;
  d->interface_num = hid_intf->bInterfaceNumber;
  d->in_ep = in_ep_desc->bEndpointAddress;
  d->out_ep = out_ep;
  d->active = true;

  // Match driver
  for (auto *drv : this->drivers_) {
    if (drv->match_device(d->protocol, d->vid, d->pid)) {
      d->driver = drv;
      ESP_LOGI(TAG, "Matched to driver: %s", drv->get_name());
      break;
    }
  }
  if (!d->driver && this->raw_driver_ != nullptr) {
    d->driver = this->raw_driver_;
    ESP_LOGI(TAG, "Matched to driver: %s", this->raw_driver_->get_name());
  }
  if (!d->driver)
    ESP_LOGW(TAG, "No driver for protocol=%d VID=%04X PID=%04X", d->protocol, d->vid, d->pid);

  this->start_in_transfer_(d, d->in_ep, in_ep_desc->wMaxPacketSize, false);

  ESP_LOGI(TAG, "HID device ready on interface %d (protocol %d)", d->interface_num, d->protocol);

  if (d->protocol == HID_PROTO_KEYBOARD && !is_xbox360)
    this->setup_media_interface_(d);

#ifdef USE_TEXT_SENSOR
  if (this->device_name_sensor_ != nullptr)
    this->device_name_sensor_->publish_state(d->driver != nullptr ? d->driver->get_name() : "Unknown");
#endif

  return true;
}

// ── setup_media_interface_ ───────────────────────────────────────────────────

bool USBHIDClient::setup_media_interface_(HIDDevice *dev) {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (!cfg)
    return false;

  // The second HID interface of a keyboard, the one that carries the media keys. Found by
  // the same rule as the first -- any interface of class 0x03 -- rather than by assuming it
  // is number 1: that only holds for a plain keyboard, and the main search stopped assuming
  // it when it was widened to composite devices.
  const usb_intf_desc_t *media_intf = nullptr;
  const usb_ep_desc_t *media_ep = nullptr;
  int offset = 0;

  while (offset < cfg->wTotalLength) {
    const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(
        reinterpret_cast<const uint8_t *>(cfg) + offset);

    if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      if (media_intf != nullptr)
        break;  // Endpoints of the interface after it are not ours.
      if (intf->bInterfaceClass == USB_CLASS_HID && intf->bInterfaceNumber != dev->interface_num)
        media_intf = intf;
    } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && media_intf) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if ((ep->bEndpointAddress & EP_DIR_IN) && (ep->bmAttributes & EP_TYPE_MASK) == EP_TYPE_INTERRUPT) {
        media_ep = ep;
        break;
      }
    }
    offset += desc->bLength;
  }

  if (!media_intf || !media_ep) {
    ESP_LOGD(TAG, "No media interface found");
    return false;
  }

  if (!this->claim_interface(media_intf->bInterfaceNumber)) {
    ESP_LOGW(TAG, "Failed to claim media interface %d", media_intf->bInterfaceNumber);
    return false;
  }

  dev->media_in_ep = media_ep->bEndpointAddress;
  dev->media_active = true;

  this->start_in_transfer_(dev, dev->media_in_ep, media_ep->wMaxPacketSize, true);
  ESP_LOGI(TAG, "Media interface %d ready on ep 0x%02X", media_intf->bInterfaceNumber, dev->media_in_ep);
  return true;
}

// ── on_transfer_in_ ──────────────────────────────────────────────────────────

void USBHIDClient::on_transfer_in_(uint8_t ep, const uint8_t *data, size_t len, bool media) {
  // Both endpoints belong to the one device this client serves, so which of them a report
  // arrived on says nothing that the media flag does not already say.
  (void) ep;
  HIDDevice *dev = &this->device_;
  if (!dev->active || dev->driver == nullptr)
    return;

  if (media) {
    uint8_t buf[65];
    buf[0] = HID_MEDIA_REPORT_MARKER;
    size_t copy = (len > 64) ? 64 : len;
    memcpy(&buf[1], data, copy);
    dev->driver->process_report(buf, copy + 1, dev);
  } else {
    dev->driver->process_report(data, len, dev);
  }
}

// ── on_disconnected ───────────────────────────────────────────────────────────

void USBHIDClient::on_disconnected() {
  this->device_ = HIDDevice{};

#ifdef USE_TEXT_SENSOR
  if (this->device_name_sensor_)
    this->device_name_sensor_->publish_state("None");
#endif

  usb_host::USBClient::on_disconnected();
}

// ── LED / output helpers ─────────────────────────────────────────────────────

void USBHIDClient::update_keyboard_leds(HIDDevice *device, uint8_t led_state) {
  if (!device || !device->dev_hdl)
    return;
  std::vector<uint8_t> payload{led_state};
  this->control_transfer(
      HID_REQ_TYPE_HOST_TO_DEV,
      HID_REQ_SET_REPORT,
      static_cast<uint16_t>((HID_REPORT_TYPE_OUTPUT << 8) | 0),
      static_cast<uint16_t>(device->interface_num),
      [](const usb_host::TransferStatus &) {},
      payload);
}

void USBHIDClient::send_output_report(HIDDevice *device, const uint8_t *data, size_t len) {
  if (!device || !device->dev_hdl || len == 0)
    return;
  if (device->out_ep != 0) {
    this->transfer_out(
        device->out_ep,
        [](const usb_host::TransferStatus &) {},
        data, static_cast<uint16_t>(len));
  } else {
    std::vector<uint8_t> payload(data, data + len);
    this->control_transfer(
        HID_REQ_TYPE_HOST_TO_DEV,
        HID_REQ_SET_REPORT,
        static_cast<uint16_t>((HID_REPORT_TYPE_OUTPUT << 8) | 0),
        static_cast<uint16_t>(device->interface_num),
        [](const usb_host::TransferStatus &) {},
        payload);
  }
}

}  // namespace usb_hid
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_*
