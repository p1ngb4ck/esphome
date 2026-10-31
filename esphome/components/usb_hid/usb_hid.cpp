#include "usb_hid.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

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

// =============================================================================================
// USBHIDClient: the component, the input everything listens to
// =============================================================================================

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
  usb_host::USBClient::setup();
}

void USBHIDClient::dump_config() {
  usb_host::USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  Num Lock at start: %s", YESNO((this->leds_ & hid::LED_NUM_LOCK) != 0));
}

USBHIDDevice *USBHIDClient::find_device_(uint32_t serial) const {
  for (auto *device : this->hid_devices_) {
    if (device->serial_ == serial)
      return device;
  }
  return nullptr;
}

void USBHIDClient::loop() {
  Report *report;
  while ((report = this->reports_.pop()) != nullptr) {
    // A report of a device let go meanwhile finds none
    USBHIDDevice *device = this->find_device_(report->serial);
    if (device != nullptr && report->interface < device->interfaces_.size())
      device->report_(report->interface, report->data, report->length);
    this->report_pool_.release(report);
  }
  usb_host::USBClient::loop();
}

// A HID interface's output reports go over the control pipe (SET_REPORT): its interrupt OUT gets no pipe
// (and holds no host channel) unless a pad driver talks to the device over it (hid/gamepad.h), or the
// Thrustmaster wheel setup (thrustmaster_()). Vendor pad interfaces (Xbox, MCE) keep theirs.
uint32_t USBHIDClient::skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                                          const usb_intf_desc_t *alt) const {
  if (alt->bInterfaceClass != USB_CLASS_HID)
    return 0;
  if ((desc->idVendor == 0x044F && desc->idProduct == 0xB65D) ||
      hid::has_gamepad_driver(hid::HIDBus::USB, desc->idVendor, desc->idProduct))
    return 0;
  uint32_t skip = 0;
  usb_host::for_each_alt_endpoint(config, alt, [&skip](const usb_ep_desc_t *ep) {
    if ((ep->bEndpointAddress & 0x80) == 0 &&
        (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_INT)
      skip |= usb_host::endpoint_bit(ep);
  });
  return skip;
}

USBHIDDevice *USBHIDClient::find_device(uint16_t vid, uint16_t pid) const {
  for (auto *device : this->hid_devices_) {
    const usb_device_desc_t *desc = device->device_desc();
    if (desc != nullptr && desc->idVendor == vid && desc->idProduct == pid)
      return device;
  }
  return nullptr;
}

void USBHIDClient::on_device_connected(usb_host::UsbDevice &dev) {
  auto *device = new USBHIDDevice(this, &dev, this->next_serial_++);  // NOLINT: freed in free_device_context
  dev.context = device;
  this->hid_devices_.push_back(device);
  if (!device->start_()) {
    ESP_LOGW(TAG, "Device %d: no usable HID interface", dev.addr);
    this->close_device(dev);  // stop_() and free_device_context() follow
    return;
  }
}

void USBHIDClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *device = static_cast<USBHIDDevice *>(dev.context);
  if (device == nullptr)
    return;
  device->stop_();
  this->hid_devices_.erase(std::remove(this->hid_devices_.begin(), this->hid_devices_.end(), device),
                           this->hid_devices_.end());
  this->devices_cb_.call();
  this->publish_name_();
}

void USBHIDClient::free_device_context(usb_host::UsbDevice &dev) {
  delete static_cast<USBHIDDevice *>(dev.context);  // NOLINT
  dev.context = nullptr;
}

void USBHIDClient::publish_name_() {
#ifdef USE_TEXT_SENSOR
  // The device connected last that is ready; "" when none is
  if (this->device_name_sensor_ == nullptr)
    return;
  for (auto it = this->hid_devices_.rbegin(); it != this->hid_devices_.rend(); ++it) {
    if ((*it)->ready_) {
      this->device_name_sensor_->publish_state((*it)->device_name_);
      return;
    }
  }
  this->device_name_sensor_->publish_state("");
#endif
}

uint8_t USBHIDClient::get_device_kinds() const {
  uint8_t kinds = 0;
  for (const auto *device : this->hid_devices_)
    kinds |= device->get_device_kinds();
  return kinds;
}

uint8_t USBHIDClient::input_device_kinds() const {
  uint8_t kinds = 0;
  for (const auto *device : this->hid_devices_) {
    if (!device->radio_)
      kinds |= device->get_device_kinds();
  }
  return kinds;
}

bool USBHIDClient::is_radio(const std::string &key) const {
  for (const auto *device : this->hid_devices_) {
    if (device->device_key_ == key)
      return device->radio_;
  }
  return false;
}

void USBHIDClient::list_devices(std::vector<hid::InputDevice> &out) const {
  for (const auto *device : this->hid_devices_) {
    if (!device->device_key_.empty())
      out.push_back({device->device_key_, device->device_name_, true, device->get_device_kinds()});
  }
}

void USBHIDClient::set_device_layout(const std::string &key, const hid::KeyboardLayout *layout) {
  for (auto *device : this->hid_devices_) {
    if (device->device_key_ == key)
      device->device_layout_ = layout;
  }
}

hid::HIDInput *USBHIDClient::device_input(const std::string &key) {
  for (auto *device : this->hid_devices_) {
    if (device->device_key_ == key)
      return device;
  }
  return nullptr;
}

void USBHIDClient::rumble(uint8_t strong, uint8_t weak, uint32_t duration_ms) {
  for (auto *device : this->hid_devices_)
    device->rumble(strong, weak, duration_ms);
}

void USBHIDClient::set_player_led(uint8_t player) {
  for (auto *device : this->hid_devices_)
    device->set_player_led(player);
}

void USBHIDClient::set_light_bar(uint8_t red, uint8_t green, uint8_t blue) {
  for (auto *device : this->hid_devices_)
    device->set_light_bar(red, green, blue);
}

// The input's lock LEDs (set_num_lock from the configuration): every keyboard shows them.
void USBHIDClient::write_leds_(uint8_t leds) {
  for (auto *device : this->hid_devices_) {
    device->leds_ = leds;
    device->write_leds_(leds);
  }
}

// =============================================================================================
// USBHIDDevice: one connected device
// =============================================================================================

USBHIDDevice::USBHIDDevice(USBHIDClient *client, usb_host::UsbDevice *dev, uint32_t serial)
    : client_(client), dev_(dev), serial_(serial) {
  // What it types with, its log name and the lock LEDs as the input has them
  this->layout_ = client->layout_;
  this->log_name_ = client->log_name_;
  this->leds_ = client->leds_;
  // Its keys, text, pointer and controls are the input's (the client is what everything listens to)
  this->add_on_key_event_callback([client, this](uint16_t page, uint16_t usage, bool pressed) {
    client->key_source_ = this->key_source_;
    client->key_source_label_ = this->key_source_label_;
    client->key_cb_.call(page, usage, pressed);
  });
  this->add_on_text_callback([client](const std::string &text) { client->text_cb_.call(text); });
  this->add_on_pointer_callback([client](int16_t dx, int16_t dy, int16_t wheel, int16_t pan, uint8_t buttons) {
    client->pointer_cb_.call(dx, dy, wheel, pan, buttons);
  });
  this->add_on_control_callback(
      [client](uint16_t page, uint16_t usage, int32_t value) { client->control_cb_.call(page, usage, value); });
  this->add_on_key_callback([client](uint8_t key) { client->send_key_(key); });
}

Component *USBHIDDevice::hid_component() { return this->client_; }

bool USBHIDDevice::control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                                    const usb_host::transfer_cb_t &callback, const std::vector<uint8_t> &data,
                                    int32_t w_length) {
  return this->client_->control_transfer(*this->dev_, type, request, value, index, callback, data, w_length);
}

bool USBHIDDevice::start_() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->interfaces_.clear();
  if (!this->find_interfaces_())
    return false;
  for (auto &intf : this->interfaces_) {
    if (!this->client_->claim_interface(*this->dev_, intf.number, 0)) {
      ESP_LOGE(TAG, "Claiming interface %u failed", intf.number);
      intf.in_ep = 0;
    }
  }
  // Which device it is, for a layout of its own (hid::InputDevice)
  usb_host::UsbDeviceInfo info{};
  if (this->client_->get_device_info(*this->dev_, info)) {
    char key[16];
    snprintf(key, sizeof(key), "usb:%04x:%04x", info.vendor_id, info.product_id);
    this->device_key_ = key;
    if (info.serial_number[0] != '\0')
      this->device_key_ += std::string(":") + info.serial_number;
    this->device_name_ = info.product[0] != '\0' ? info.product : std::string("USB ") + (key + 4);
    // bcdDevice: the device's release, unless its driver reads its firmware.
    char release[8];
    snprintf(release, sizeof(release), "%X.%02X", info.bcd_device >> 8, info.bcd_device & 0xFF);
    this->set_firmware(release);
  }
  this->attach_pad_();
  this->fetch_descriptor_(0);
  return true;
}

void USBHIDDevice::stop_() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->ready_ = false;
  // A read is always in flight on each IN endpoint, and the interface cannot be released
  // (nor the device closed, nor the transfer buffers freed) while it is.
  for (auto &intf : this->interfaces_) {
    if (intf.in_ep == 0)
      continue;
    usb_host_endpoint_halt(this->dev_->handle, intf.in_ep);
    usb_host_endpoint_flush(this->dev_->handle, intf.in_ep);
    this->client_->release_interface(*this->dev_, intf.number);
  }
  this->interfaces_.clear();
  this->gamepad_detach_();
  this->release_all_hid();
}

// The gamepad driver of the device, and the interface it talks to: the vendor pad interface,
// else the first HID interface that is not a boot keyboard or mouse.
void USBHIDDevice::attach_pad_() {
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
  const usb_device_desc_t *dev = this->dev_->desc;
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
      const usb_config_desc_t *cfg = this->dev_->config;
      if (cfg != nullptr && cfg->bNumInterfaces > 1) {
        static constexpr uint8_t REQ_TYPE = usb_host::USB_DIR_OUT | usb_host::USB_TYPE_STANDARD |
                                            usb_host::USB_RECIP_INTERFACE;
        static constexpr uint8_t B_REQUEST_SET_INTERFACE = 11;
        this->control_transfer(REQ_TYPE, B_REQUEST_SET_INTERFACE, 0, 1, [](const usb_host::TransferStatus &) {});
      }
      break;
    }
    default:
      this->gamepad_attach_(hid::HIDBus::USB, dev->idVendor, dev->idProduct, dev->bcdDevice);
      break;
  }
}

uint8_t USBHIDDevice::get_device_kinds() const {
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

bool USBHIDDevice::find_interfaces_() {
  const usb_config_desc_t *cfg = this->dev_->config;
  const usb_device_desc_t *dev = this->dev_->desc;
  if (cfg == nullptr || dev == nullptr)
    return false;
  const uint8_t mce = MCEReceiverDriver::model(dev->idVendor, dev->idProduct);
  this->radio_ = dev->bDeviceClass == USB_CLASS_WIRELESS_CONTROLLER;
  Interface *current = nullptr;
  uint32_t skipped = 0;  // the current interface's endpoints its claim opens no pipe for
  int offset = 0;
  const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      current = nullptr;
      if (intf->bInterfaceClass == USB_CLASS_WIRELESS_CONTROLLER)
        this->radio_ = true;
      if (intf->bAlternateSetting != 0 || !this->dev_->interface_routed(intf->bInterfaceNumber) ||
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
      skipped = this->client_->skipped_endpoints_(dev, cfg, intf);
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
    } else if (current->out_ep == 0 && (skipped & usb_host::endpoint_bit(ep)) == 0) {
      current->out_ep = ep->bEndpointAddress;
    }
  }
  return !this->interfaces_.empty();
}

// -- Report descriptors (one at a time, each continued from the last one's completion) -----

struct DescriptorRequest {
  USBHIDClient *client;
  uint32_t serial;
  size_t index;
  uint32_t generation;
};

void USBHIDDevice::fetch_descriptor_(size_t index) {
  while (index < this->interfaces_.size() &&
         (this->interfaces_[index].vendor != VENDOR_NONE || this->interfaces_[index].descriptor_length == 0 ||
          this->interfaces_[index].in_ep == 0))
    index++;
  if (index >= this->interfaces_.size()) {
    this->start_reading_();
    return;
  }
  const Interface &intf = this->interfaces_[index];
  // A control read of the whole descriptor, which can be larger than the device's transfer
  // buffers: its own transfer, sized for it and whole packets of the control endpoint.
  const uint16_t mps0 = this->dev_->desc->bMaxPacketSize0;
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
  xfer->device_handle = this->dev_->handle;
  xfer->bEndpointAddress = 0;
  // By the device's serial, not a pointer: the device may be gone when it completes
  xfer->context =
      new DescriptorRequest{this->client_, this->serial_, index, this->generation_.load(std::memory_order_acquire)};
  // USB task.
  xfer->callback = [](usb_transfer_t *t) {
    auto *req = static_cast<DescriptorRequest *>(t->context);
    std::vector<uint8_t> copy;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED &&
        t->actual_num_bytes > static_cast<int>(usb_host::SETUP_PACKET_SIZE))
      copy.assign(t->data_buffer + usb_host::SETUP_PACKET_SIZE, t->data_buffer + t->actual_num_bytes);
    usb_host_transfer_free(t);
    USBHIDClient *client = req->client;
    const uint32_t serial = req->serial;
    const size_t index = req->index;
    const uint32_t generation = req->generation;
    delete req;
    client->defer([client, serial, index, generation, copy = std::move(copy)]() {
      USBHIDDevice *device = client->find_device_(serial);
      if (device != nullptr && generation == device->generation_.load(std::memory_order_acquire))
        device->descriptor_done_(index, copy.data(), copy.size());
    });
  };
  if (usb_host_transfer_submit_control(this->client_->handle_, xfer) != ESP_OK) {
    delete static_cast<DescriptorRequest *>(xfer->context);
    usb_host_transfer_free(xfer);
    ESP_LOGE(TAG, "Report descriptor request of interface %u not submitted", intf.number);
    this->fetch_descriptor_(index + 1);
  }
}

void USBHIDDevice::descriptor_done_(size_t index, const uint8_t *data, size_t length) {
  Interface &intf = this->interfaces_[index];
  if (length == 0) {
    ESP_LOGW(TAG, "Interface %u: no report descriptor", intf.number);
  } else {
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
    // The raw descriptor, once per connect: output / feature reports the parser does not list (stack, no heap)
    for (size_t pos = 0; pos < length; pos += 32) {
      char hex[32 * 3 + 1];
      size_t n = 0;
      for (size_t i = pos; i < length && i < pos + 32; i++)
        n += snprintf(hex + n, sizeof(hex) - n, "%02X ", data[i]);
      ESP_LOGD(TAG, "Interface %u report descriptor [%u]: %s", intf.number, static_cast<unsigned>(pos), hex);
    }
#endif
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

void USBHIDDevice::start_reading_() {
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  for (size_t i = 0; i < this->interfaces_.size(); i++) {
    const Interface &intf = this->interfaces_[i];
    if (intf.in_ep != 0)
      this->read_(static_cast<uint8_t>(i), intf.number, intf.in_ep, intf.in_mps, generation);
  }
  ESP_LOGI(TAG, "HID device ready: %s (%u interfaces)", this->device_name_.c_str(),
           static_cast<unsigned>(this->interfaces_.size()));
  this->ready_ = true;
  this->write_leds_(this->leds_);
  // What the device is (get_device_kinds()) is known from here on.
  this->client_->devices_cb_.call();
  this->client_->publish_name_();
  this->gamepad_start_();
  this->thrustmaster_();
}

// USB task (and the main loop for the first read). Takes copies rather than reading
// interfaces_, which the main loop clears when the device goes. The device's memory stays until
// its transfers are back (USBClient::reap_devices_), so `this` is valid in the completion.
void USBHIDDevice::read_(uint8_t index, uint8_t number, uint8_t ep, uint16_t mps, uint32_t generation) {
  const bool submitted = this->client_->transfer_in(
      *this->dev_, ep,
      [this, index, number, ep, mps, generation](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        if (!status.success) {
          ESP_LOGW(TAG, "Interface %u stopped (status %u)", number, status.error_code);
          return;
        }
        if (status.data_len > 0) {
          USBHIDClient *client = this->client_;
          USBHIDClient::Report *report = client->report_pool_.allocate();
          if (report != nullptr) {
            report->serial = this->serial_;
            report->interface = index;
            report->length = static_cast<uint8_t>(std::min(status.data_len, USBHIDClient::REPORT_MAX));
            memcpy(report->data, status.data, report->length);
            client->reports_.push(report);
            client->enable_loop_soon_any_context();
          }
        }
        this->read_(index, number, ep, mps, generation);
      },
      mps);
  if (!submitted)
    ESP_LOGE(TAG, "Read on interface %u not submitted", number);
}

// -- Reports (main loop) -------------------------------------------------------------------

void USBHIDDevice::report_(size_t index, const uint8_t *data, size_t length) {
  Interface &intf = this->interfaces_[index];
#ifdef USB_HID_ENABLE_RAW
  if (this->client_->raw_driver_ != nullptr)
    this->client_->raw_driver_->process_report(data, length);
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

bool USBHIDDevice::send_output_report(const uint8_t *data, size_t len) {
  if (this->pad_index_ >= this->interfaces_.size() || this->interfaces_[this->pad_index_].out_ep == 0)
    return false;
  return this->client_->transfer_out(*this->dev_, this->interfaces_[this->pad_index_].out_ep,
                                     [](const usb_host::TransferStatus &) {}, data, static_cast<uint16_t>(len));
}

bool USBHIDDevice::set_report(hid::HIDReportKind kind, const uint8_t *data, size_t len) {
  if (this->pad_index_ >= this->interfaces_.size() || len == 0)
    return false;
  const Interface &intf = this->interfaces_[this->pad_index_];
  const uint8_t id = intf.map != nullptr && intf.map->uses_report_ids() ? data[0] : 0;
  return this->control_transfer(HID_REQ_TYPE_HOST_TO_DEV, HID_REQ_SET_REPORT,
                                static_cast<uint16_t>(static_cast<uint8_t>(kind) << 8 | id), intf.number,
                                [](const usb_host::TransferStatus &) {}, std::vector<uint8_t>(data, data + len));
}

// The answer goes to the driver from the main loop; len 0 when the request failed.
bool USBHIDDevice::request_feature_report(uint8_t id, uint16_t len) {
  const usb_device_desc_t *dev = this->dev_->desc;
  if (this->pad_index_ >= this->interfaces_.size() || dev == nullptr || len == 0)
    return false;
  // A control read takes whole packets of the control endpoint.
  const uint16_t mps0 = dev->bMaxPacketSize0;
  std::vector<uint8_t> buffer((len + mps0 - 1) / mps0 * mps0);
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  USBHIDClient *client = this->client_;
  const uint32_t serial = this->serial_;
  const bool submitted = this->control_transfer(
      HID_REQ_TYPE_DEV_TO_HOST, HID_REQ_GET_REPORT,
      static_cast<uint16_t>(static_cast<uint8_t>(hid::HIDReportKind::FEATURE) << 8 | id),
      this->interfaces_[this->pad_index_].number,
      [client, serial, id, generation](const usb_host::TransferStatus &status) {
        std::vector<uint8_t> copy;
        if (status.success)
          copy.assign(status.data, status.data + status.data_len);
        client->defer([client, serial, id, generation, copy = std::move(copy)]() {
          USBHIDDevice *device = client->find_device_(serial);
          if (device != nullptr && generation == device->generation_.load(std::memory_order_acquire))
            device->gamepad_feature_(id, copy.data(), copy.size());
        });
      },
      buffer, len);
  if (!submitted)
    this->gamepad_feature_(id, nullptr, 0);
  return submitted;
}

// Thrustmaster wheels (T150, T300, T500) come up as a generic "FFB wheel" (044F:B65D) and
// switch to their own mode on request, then enumerate again (Linux hid-thrustmaster.c).
void USBHIDDevice::thrustmaster_() {
  const usb_device_desc_t *dev = this->dev_->desc;
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
  USBHIDClient *client = this->client_;
  const uint32_t serial = this->serial_;
  this->control_transfer(
      0xC1, 73, 0, 0,
      [client, serial, generation](const usb_host::TransferStatus &status) {
        uint16_t model = 0;
        bool known = status.success && status.data_len >= 8 &&
                     (status.data[0] == 0x49 || status.data[0] == 0x47) && status.data[1] == 0x00;
        if (known)
          model = static_cast<uint16_t>(status.data[6] | (status.data[7] << 8));
        client->defer([client, serial, generation, known, model]() {
          USBHIDDevice *device = client->find_device_(serial);
          if (device == nullptr || generation != device->generation_.load(std::memory_order_acquire))
            return;
          static constexpr struct {
            uint16_t model, mode;
          } WHEELS[] = {{0x0306, 0x0006}, {0x0200, 0x0005}, {0x0206, 0x0005}, {0x0209, 0x0005},
                        {0x020a, 0x0005}, {0x0204, 0x0005}, {0x0002, 0x0002}};
          for (const auto &w : WHEELS) {
            if (known && w.model == model) {
              ESP_LOGI(TAG, "Thrustmaster wheel model 0x%04X: switching it to its own mode", model);
              // The wheel goes away before it answers; whatever comes back is fine.
              device->control_transfer(0x41, 83, w.mode, 0, [](const usb_host::TransferStatus &) {});
              return;
            }
          }
          ESP_LOGW(TAG, "Thrustmaster wheel: model 0x%04X unknown, left in generic mode", model);
        });
      },
      buffer, 16);
}

// -- Lock LEDs -----------------------------------------------------------------------------

void USBHIDDevice::write_leds_(uint8_t leds) {
  for (auto &intf : this->interfaces_) {
    if (intf.map == nullptr)
      continue;
    uint8_t report_id = 0;
    std::vector<uint8_t> payload;
    if (!hid::build_led_report(*intf.map, leds, report_id, payload))
      continue;
    if (intf.map->uses_report_ids())
      payload.insert(payload.begin(), report_id);
    this->control_transfer(HID_REQ_TYPE_HOST_TO_DEV, HID_REQ_SET_REPORT,
                           static_cast<uint16_t>(HID_REPORT_TYPE_OUTPUT << 8 | report_id), intf.number,
                           [](const usb_host::TransferStatus &) {}, payload);
    return;
  }
}

}  // namespace esphome::usb_hid

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31
