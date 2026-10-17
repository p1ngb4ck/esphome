#include "usb_hid.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace esphome::usb_hid {

static const char *const TAG = "usb_hid";

static constexpr uint8_t USB_DESC_HID = 0x21;
static constexpr uint8_t USB_DESC_HID_REPORT = 0x22;
static constexpr uint8_t USB_REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t REQ_TYPE_STD_INTERFACE_IN = 0x81;
// Xbox 360 pads: vendor class, subclass 0x5D; protocol 0x01 wired, 0x81 on the wireless receiver.
// Xbox One and Series pads (GIP): subclass 0x47, protocol 0xD0. By class, so every vendor's.
static constexpr uint8_t XBOX_SUBCLASS = 0x5D;
static constexpr uint8_t XBOX_PROTOCOL_WIRED = 0x01;
static constexpr uint8_t XBOX_PROTOCOL_WIRELESS = 0x81;
static constexpr uint8_t XBOXONE_SUBCLASS = 0x47;
static constexpr uint8_t XBOXONE_PROTOCOL = 0xD0;

void USBHIDClient::setup() {
  this->add_class_match(USB_CLASS_HID);
  this->add_class_match(0xFF, XBOX_SUBCLASS, XBOX_PROTOCOL_WIRED);
  this->add_class_match(0xFF, XBOX_SUBCLASS, XBOX_PROTOCOL_WIRELESS);
  this->add_class_match(0xFF, XBOXONE_SUBCLASS, XBOXONE_PROTOCOL);
  // MCE IR receivers, by vendor and product as mceusb.c lists them; the multi-function one
  // only for its vendor-class IR interface.
  MCEReceiverDriver::for_each([this](const MCEReceiverModel &m) {
    this->add_vendor_match(m.vid, m.pid, (m.flags & MCEReceiverDriver::INTERFACE_2) ? 0xFF : -1);
  });
  // Which device it is, for a layout of its own (hid::InputDevice).
  this->add_on_connection_callback([this](bool connected) {
    this->device_key_.clear();
    this->device_name_.clear();
    this->device_layout_ = nullptr;
    usb_host::UsbDeviceInfo info{};
    if (connected && this->get_device_info(info)) {
      char key[16];
      snprintf(key, sizeof(key), "usb:%04x:%04x", info.vendor_id, info.product_id);
      this->device_key_ = key;
      if (info.serial_number[0] != '\0')
        this->device_key_ += std::string(":") + info.serial_number;
      this->device_name_ = info.product[0] != '\0' ? info.product : std::string("USB ") + (key + 4);
    }
    this->devices_cb_.call();
  });
  usb_host::USBClient::setup();
}

void USBHIDClient::list_devices(std::vector<hid::InputDevice> &out) const {
  if (!this->device_key_.empty())
    out.push_back({this->device_key_, this->device_name_, true});
}

void USBHIDClient::set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) {
  if (key == this->device_key_)
    this->device_layout_ = layout;
}

void USBHIDClient::dump_config() {
  usb_host::USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  Num Lock at start: %s", YESNO((this->leds_ & hid::LED_NUM_LOCK) != 0));
}

void USBHIDClient::loop() {
  Report *report;
  while ((report = this->reports_.pop()) != nullptr) {
    if (report->interface < this->interfaces_.size())
      this->report_(report->interface, report->data, report->length);
    this->report_pool_.release(report);
  }
  usb_host::USBClient::loop();
}

// -- Connection ----------------------------------------------------------------------------

void USBHIDClient::on_connected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->interfaces_.clear();
  if (!this->find_interfaces_()) {
    ESP_LOGW(TAG, "No usable HID interface on this device");
    this->disconnect();
    return;
  }
  for (auto &intf : this->interfaces_) {
    if (!this->claim_interface(intf.number, 0)) {
      ESP_LOGE(TAG, "Claiming interface %u failed", intf.number);
      intf.in_ep = 0;
    }
  }
  this->attach_pad_();
  this->fetch_descriptor_(0);
}

// The gamepad driver of the device, and the interface it talks to: the vendor pad interface,
// else the first HID interface that is not a boot keyboard or mouse.
void USBHIDClient::attach_pad_() {
  this->pad_index_ = 0;
  bool found = false;
  for (size_t i = 0; i < this->interfaces_.size() && !found; i++) {
    if (this->interfaces_[i].vendor != VENDOR_NONE) {
      this->pad_index_ = i;
      found = true;
    }
  }
  for (size_t i = 0; i < this->interfaces_.size() && !found; i++) {
    if (this->interfaces_[i].protocol == HID_PROTO_NONE && this->interfaces_[i].in_ep != 0) {
      this->pad_index_ = i;
      found = true;
    }
  }
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (dev == nullptr)
    return;
  switch (this->interfaces_[this->pad_index_].vendor) {
    case VENDOR_XBOX360:
      this->gamepad_attach_(hid::HIDBus::USB, std::make_unique<Xbox360Driver>(this, false));
      break;
    case VENDOR_XBOX360_WIRELESS:
      this->gamepad_attach_(hid::HIDBus::USB, std::make_unique<Xbox360Driver>(this, true));
      break;
    case VENDOR_MCE:
      this->gamepad_attach_(hid::HIDBus::USB, std::make_unique<MCEReceiverDriver>(
                                                  this, MCEReceiverDriver::model(dev->idVendor, dev->idProduct)));
      break;
    case VENDOR_XBOXONE: {
      this->gamepad_attach_(hid::HIDBus::USB,
                            std::make_unique<XboxOneDriver>(this, dev->idVendor, dev->idProduct, dev->bcdDevice));
      // The audio interface off: some pads (PowerA) only report the guide button then.
      const usb_config_desc_t *cfg = this->get_config_desc_();
      if (cfg != nullptr && cfg->bNumInterfaces > 1)
        this->set_interface(1, 0, [](const usb_host::TransferStatus &) {});
      break;
    }
    default:
      this->gamepad_attach_(hid::HIDBus::USB, dev->idVendor, dev->idProduct, dev->bcdDevice);
      break;
  }
}

void USBHIDClient::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  // A read is always in flight on each IN endpoint, and the interface cannot be released
  // (nor the device closed, nor the transfer buffers freed) while it is.
  for (auto &intf : this->interfaces_) {
    if (intf.in_ep == 0)
      continue;
    usb_host_endpoint_halt(this->device_handle_, intf.in_ep);
    usb_host_endpoint_flush(this->device_handle_, intf.in_ep);
    this->release_interface(intf.number);
  }
  this->interfaces_.clear();
  this->gamepad_detach_();
  this->release_all_hid();
#ifdef USE_TEXT_SENSOR
  if (this->device_name_sensor_ != nullptr)
    this->device_name_sensor_->publish_state("");
#endif
  usb_host::USBClient::on_disconnected();
}

uint8_t USBHIDClient::get_device_kinds() const {
  if (!this->is_connected())
    return 0;
  uint8_t kinds = 0;
  for (const auto &intf : this->interfaces_) {
    if (intf.vendor == VENDOR_MCE) {
      kinds |= hid::HID_DEVICE_REMOTE;
    } else if (intf.vendor != VENDOR_NONE) {
      kinds |= hid::HID_DEVICE_GAMEPAD;
    } else if (intf.map != nullptr) {
      kinds |= intf.map->device_kinds();
    }
  }
  return kinds;
}

bool USBHIDClient::find_interfaces_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (cfg == nullptr || dev == nullptr)
    return false;
  const uint8_t mce = MCEReceiverDriver::model(dev->idVendor, dev->idProduct);
  Interface *current = nullptr;
  int offset = 0;
  const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      current = nullptr;
      if (intf->bAlternateSetting != 0 || !this->interface_routed_(intf->bInterfaceNumber) ||
          this->interfaces_.size() >= MAX_INTERFACES)
        continue;
      Vendor vendor = VENDOR_NONE;
      if (intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == XBOX_SUBCLASS) {
        if (intf->bInterfaceProtocol == XBOX_PROTOCOL_WIRED)
          vendor = VENDOR_XBOX360;
        if (intf->bInterfaceProtocol == XBOX_PROTOCOL_WIRELESS)
          vendor = VENDOR_XBOX360_WIRELESS;
      } else if (intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == XBOXONE_SUBCLASS &&
                 intf->bInterfaceProtocol == XBOXONE_PROTOCOL) {
        vendor = VENDOR_XBOXONE;
      }
      if (mce != 0) {
        // The receiver's IR interface, whatever its class; none of its others.
        if (intf->bInterfaceNumber != ((mce & MCEReceiverDriver::INTERFACE_2) ? 2 : 0))
          continue;
        vendor = VENDOR_MCE;
      }
      if (intf->bInterfaceClass != USB_CLASS_HID && vendor == VENDOR_NONE)
        continue;
      // The wireless receiver has a data and a headset interface per pad, an Xbox One pad a
      // data and an audio interface: the first one only.
      if (vendor != VENDOR_NONE && !this->interfaces_.empty())
        continue;
      Interface entry;
      entry.number = intf->bInterfaceNumber;
      entry.protocol = intf->bInterfaceProtocol;
      entry.vendor = vendor;
      this->interfaces_.push_back(std::move(entry));
      current = &this->interfaces_.back();
      continue;
    }
    if (current == nullptr)
      continue;
    if (desc->bDescriptorType == USB_DESC_HID && desc->bLength >= 9) {
      // bcdHID, bCountryCode, bNumDescriptors, then (type, length) pairs.
      const auto *hid = reinterpret_cast<const uint8_t *>(desc);
      for (uint8_t i = 0; i < hid[5] && 6u + 3u * i + 2u < desc->bLength; i++) {
        if (hid[6 + 3 * i] == USB_DESC_HID_REPORT) {
          current->descriptor_length = static_cast<uint16_t>(hid[7 + 3 * i] | (hid[8 + 3 * i] << 8));
          break;
        }
      }
      continue;
    }
    if (desc->bDescriptorType != USB_W_VALUE_DT_ENDPOINT)
      continue;
    const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
    // Interrupt endpoints; an MCE receiver has bulk ones as often.
    if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_INT &&
        !(current->vendor == VENDOR_MCE && USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_BULK))
      continue;
    // bInterval 0 is not legal for an interrupt endpoint and the pipe is built from it.
    if (ep->bInterval == 0 && USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_INT) {
      ESP_LOGW(TAG, "Endpoint 0x%02X declares bInterval 0; using 1", ep->bEndpointAddress);
      const_cast<usb_ep_desc_t *>(ep)->bInterval = 1;
    }
    if (USB_EP_DESC_GET_EP_DIR(ep)) {
      if (current->in_ep == 0) {
        current->in_ep = ep->bEndpointAddress;
        current->in_mps = USB_EP_DESC_GET_MPS(ep);
      }
    } else if (current->out_ep == 0) {
      current->out_ep = ep->bEndpointAddress;
    }
  }
  return !this->interfaces_.empty();
}

// -- Report descriptors (one at a time, each continued from the last one's completion) -----

struct DescriptorRequest {
  USBHIDClient *client;
  size_t index;
  uint32_t generation;
};

void USBHIDClient::fetch_descriptor_(size_t index) {
  while (index < this->interfaces_.size() &&
         (this->interfaces_[index].vendor != VENDOR_NONE || this->interfaces_[index].descriptor_length == 0 ||
          this->interfaces_[index].in_ep == 0))
    index++;
  if (index >= this->interfaces_.size()) {
    this->start_reading_();
    return;
  }
  const Interface &intf = this->interfaces_[index];
  // A control read of the whole descriptor, which can be larger than the client's transfer
  // buffers: its own transfer, sized for it and whole packets of the control endpoint.
  const uint16_t mps0 = this->get_device_desc_()->bMaxPacketSize0;
  const size_t data = (intf.descriptor_length + mps0 - 1) / mps0 * mps0;
  usb_transfer_t *xfer = nullptr;
  if (usb_host_transfer_alloc(usb_host::SETUP_PACKET_SIZE + data, 0, &xfer) != ESP_OK) {
    ESP_LOGE(TAG, "No buffer for the report descriptor of interface %u", intf.number);
    this->fetch_descriptor_(index + 1);
    return;
  }
  auto *setup = reinterpret_cast<usb_setup_packet_t *>(xfer->data_buffer);
  setup->bmRequestType = REQ_TYPE_STD_INTERFACE_IN;
  setup->bRequest = USB_REQ_GET_DESCRIPTOR;
  setup->wValue = USB_DESC_HID_REPORT << 8;
  setup->wIndex = intf.number;
  setup->wLength = intf.descriptor_length;
  xfer->num_bytes = static_cast<int>(usb_host::SETUP_PACKET_SIZE + data);
  xfer->device_handle = this->device_handle_;
  xfer->bEndpointAddress = 0;
  xfer->context = new DescriptorRequest{this, index, this->generation_.load(std::memory_order_acquire)};
  // USB task.
  xfer->callback = [](usb_transfer_t *t) {
    auto *req = static_cast<DescriptorRequest *>(t->context);
    std::vector<uint8_t> copy;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED &&
        t->actual_num_bytes > static_cast<int>(usb_host::SETUP_PACKET_SIZE))
      copy.assign(t->data_buffer + usb_host::SETUP_PACKET_SIZE, t->data_buffer + t->actual_num_bytes);
    usb_host_transfer_free(t);
    USBHIDClient *client = req->client;
    const size_t index = req->index;
    const uint32_t generation = req->generation;
    delete req;
    client->defer([client, index, generation, copy = std::move(copy)]() {
      if (generation == client->generation_.load(std::memory_order_acquire))
        client->descriptor_done_(index, copy.data(), copy.size());
    });
  };
  if (usb_host_transfer_submit_control(this->handle_, xfer) != ESP_OK) {
    delete static_cast<DescriptorRequest *>(xfer->context);
    usb_host_transfer_free(xfer);
    ESP_LOGE(TAG, "Report descriptor request of interface %u not submitted", intf.number);
    this->fetch_descriptor_(index + 1);
  }
}

void USBHIDClient::descriptor_done_(size_t index, const uint8_t *data, size_t length) {
  Interface &intf = this->interfaces_[index];
  if (length == 0) {
    ESP_LOGW(TAG, "Interface %u: no report descriptor", intf.number);
  } else {
    intf.map.reset(hid::HIDReportMap::parse_report_map_data(data, length));
    if (intf.map == nullptr) {
      ESP_LOGW(TAG, "Interface %u: report descriptor (%u bytes) not understood", intf.number,
               static_cast<unsigned>(length));
    } else {
      ESP_LOGD(TAG, "Interface %u: %u input reports, %u fields", intf.number,
               static_cast<unsigned>(intf.map->reports().size()), static_cast<unsigned>(intf.map->fields().size()));
    }
  }
  this->fetch_descriptor_(index + 1);
}

void USBHIDClient::start_reading_() {
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  for (size_t i = 0; i < this->interfaces_.size(); i++) {
    const Interface &intf = this->interfaces_[i];
    if (intf.in_ep != 0)
      this->read_(static_cast<uint8_t>(i), intf.number, intf.in_ep, intf.in_mps, generation);
  }
  ESP_LOGI(TAG, "HID device ready (%u interfaces)", static_cast<unsigned>(this->interfaces_.size()));
  this->write_leds_(this->leds_);
  this->report_connected_();
#ifdef USE_TEXT_SENSOR
  if (this->device_name_sensor_ != nullptr) {
    usb_host::UsbDeviceInfo info;
    if (this->get_device_info(info))
      this->device_name_sensor_->publish_state(info.product);
  }
#endif
  this->gamepad_start_();
  this->thrustmaster_();
}

// USB task (and the main loop for the first read). Takes copies rather than reading
// interfaces_, which the main loop clears on disconnect.
void USBHIDClient::read_(uint8_t index, uint8_t number, uint8_t ep, uint16_t mps, uint32_t generation) {
  const bool submitted = this->transfer_in(
      ep,
      [this, index, number, ep, mps, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          ESP_LOGW(TAG, "Interface %u stopped (status %u)", number, status.error_code);
          return;
        }
        if (status.data_len > 0) {
          Report *report = this->report_pool_.allocate();
          if (report != nullptr) {
            report->interface = index;
            report->length = static_cast<uint8_t>(std::min(status.data_len, REPORT_MAX));
            memcpy(report->data, status.data, report->length);
            this->reports_.push(report);
            this->enable_loop_soon_any_context();
          }
        }
        this->read_(index, number, ep, mps, generation);
      },
      mps);
  if (!submitted)
    ESP_LOGE(TAG, "Read on interface %u not submitted", number);
}

// -- Reports (main loop) -------------------------------------------------------------------

void USBHIDClient::report_(size_t index, const uint8_t *data, size_t length) {
  Interface &intf = this->interfaces_[index];
#ifdef USB_HID_ENABLE_RAW
  if (this->raw_driver_ != nullptr)
    this->raw_driver_->process_report(data, length);
#endif
  // The device's driver, for its vendor interface or a HID one that is not a boot keyboard or
  // mouse (whose first byte is no report ID).
  if ((intf.vendor != VENDOR_NONE || intf.protocol == HID_PROTO_NONE) && this->gamepad_report_(data, length)) {
    this->pad_index_ = index;
    return;
  }
  if (intf.vendor != VENDOR_NONE || intf.map == nullptr || length == 0)
    return;
  hid::HIDReportSource source;
  source.characteristic_handle = intf.number;
  if (intf.map->uses_report_ids()) {
    source.has_report_id = true;
    source.report_id = data[0];
    data++;
    length--;
  }
  this->process_hid_values(intf.map->parse(source, data, length));
}

// -- Gamepad driver transport --------------------------------------------------------------

bool USBHIDClient::send_output_report(const uint8_t *data, size_t len) {
  if (this->pad_index_ >= this->interfaces_.size() || this->interfaces_[this->pad_index_].out_ep == 0)
    return false;
  return this->transfer_out(this->interfaces_[this->pad_index_].out_ep, [](const usb_host::TransferStatus &) {},
                            data, static_cast<uint16_t>(len));
}

bool USBHIDClient::set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) {
  if (this->pad_index_ >= this->interfaces_.size() || len == 0)
    return false;
  const Interface &intf = this->interfaces_[this->pad_index_];
  const uint8_t id = intf.map != nullptr && intf.map->uses_report_ids() ? data[0] : 0;
  return this->control_transfer(HID_REQ_TYPE_HOST_TO_DEV, HID_REQ_SET_REPORT,
                                static_cast<uint16_t>(static_cast<uint8_t>(kind) << 8 | id), intf.number,
                                [](const usb_host::TransferStatus &) {}, std::vector<uint8_t>(data, data + len));
}

// The answer goes to the driver from the main loop; len 0 when the request failed.
bool USBHIDClient::request_feature_report(uint8_t id, uint16_t len) {
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (this->pad_index_ >= this->interfaces_.size() || dev == nullptr || len == 0)
    return false;
  // A control read takes whole packets of the control endpoint.
  const uint16_t mps0 = dev->bMaxPacketSize0;
  std::vector<uint8_t> buffer((len + mps0 - 1) / mps0 * mps0);
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  const bool submitted = this->control_transfer(
      HID_REQ_TYPE_DEV_TO_HOST, HID_REQ_GET_REPORT,
      static_cast<uint16_t>(static_cast<uint8_t>(hid::HIDReportKind::FEATURE) << 8 | id),
      this->interfaces_[this->pad_index_].number,
      [this, id, generation](const usb_host::TransferStatus &status) {
        std::vector<uint8_t> copy;
        if (status.success)
          copy.assign(status.data, status.data + status.data_len);
        this->defer([this, id, generation, copy = std::move(copy)]() {
          if (generation == this->generation_.load(std::memory_order_acquire))
            this->gamepad_feature_(id, copy.data(), copy.size());
        });
      },
      buffer, len);
  if (!submitted)
    this->gamepad_feature_(id, nullptr, 0);
  return submitted;
}

// Thrustmaster wheels (T150, T300, T500) come up as a generic "FFB wheel" (044F:B65D) and
// switch to their own mode on request, then enumerate again (Linux hid-thrustmaster.c).
void USBHIDClient::thrustmaster_() {
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (dev == nullptr || dev->idVendor != 0x044F || dev->idProduct != 0xB65D)
    return;
  static const uint8_t SETUP0[] = {0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t SETUP1[] = {0x0a, 0x04, 0x90, 0x03, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t SETUP2[] = {0x0a, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t SETUP3[] = {0x0a, 0x04, 0x12, 0x10, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t SETUP4[] = {0x0a, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
  this->send_output_report(SETUP0, sizeof(SETUP0));
  this->send_output_report(SETUP1, sizeof(SETUP1));
  this->send_output_report(SETUP2, sizeof(SETUP2));
  this->send_output_report(SETUP3, sizeof(SETUP3));
  this->send_output_report(SETUP4, sizeof(SETUP4));
  // Model query (vendor, device to host, 73), 16 bytes: type 0x49 or 0x47, model at 6.
  const uint16_t mps0 = dev->bMaxPacketSize0;
  std::vector<uint8_t> buffer((16 + mps0 - 1) / mps0 * mps0);
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  this->control_transfer(
      0xC1, 73, 0, 0,
      [this, generation](const usb_host::TransferStatus &status) {
        uint16_t model = 0;
        bool known = status.success && status.data_len >= 8 &&
                     (status.data[0] == 0x49 || status.data[0] == 0x47) && status.data[1] == 0x00;
        if (known)
          model = static_cast<uint16_t>(status.data[6] | (status.data[7] << 8));
        this->defer([this, generation, known, model]() {
          if (generation != this->generation_.load(std::memory_order_acquire))
            return;
          static constexpr struct {
            uint16_t model, mode;
          } WHEELS[] = {{0x0306, 0x0006}, {0x0200, 0x0005}, {0x0206, 0x0005}, {0x0209, 0x0005},
                        {0x020a, 0x0005}, {0x0204, 0x0005}, {0x0002, 0x0002}};
          for (const auto &w : WHEELS) {
            if (known && w.model == model) {
              ESP_LOGI(TAG, "Thrustmaster wheel model 0x%04X: switching it to its own mode", model);
              // The wheel goes away before it answers; whatever comes back is fine.
              this->control_transfer(0x41, 83, w.mode, 0, [](const usb_host::TransferStatus &) {});
              return;
            }
          }
          ESP_LOGW(TAG, "Thrustmaster wheel: model 0x%04X unknown, left in generic mode", model);
        });
      },
      buffer, 16);
}

// -- Lock LEDs -----------------------------------------------------------------------------

void USBHIDClient::write_leds_(uint8_t leds) {
  for (auto &intf : this->interfaces_) {
    if (intf.map == nullptr)
      continue;
    uint8_t report_id = 0;
    std::vector<uint8_t> payload;
    if (!hid::build_led_report(*intf.map, leds, report_id, payload))
      continue;
    if (intf.map->uses_report_ids())
      payload.insert(payload.begin(), report_id);
    this->control_transfer(
        HID_REQ_TYPE_HOST_TO_DEV, HID_REQ_SET_REPORT, static_cast<uint16_t>(HID_REPORT_TYPE_OUTPUT << 8 | report_id),
        intf.number, [](const usb_host::TransferStatus &) {}, payload);
    return;
  }
}

}  // namespace esphome::usb_hid

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
