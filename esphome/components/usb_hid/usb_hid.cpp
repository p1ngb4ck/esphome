#include "usb_hid.h"

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::usb_hid {

static const char *const TAG = "usb_hid";

static constexpr uint8_t USB_DESC_HID = 0x21;
static constexpr uint8_t USB_DESC_HID_REPORT = 0x22;
static constexpr uint8_t USB_REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t REQ_TYPE_STD_INTERFACE_IN = 0x81;
// Xbox 360 pads: vendor class, subclass 0x5D; protocol 0x01 wired, 0x81 on the wireless receiver.
static constexpr uint8_t XBOX_SUBCLASS = 0x5D;
static constexpr uint8_t XBOX_PROTOCOL_WIRED = 0x01;
static constexpr uint8_t XBOX_PROTOCOL_WIRELESS = 0x81;

void USBHIDClient::setup() {
  this->add_class_match(USB_CLASS_HID);
  this->add_vendor_match(XBOX360_VID, XBOX360_PID_WIRED, 0xFF, XBOX_SUBCLASS);
  this->add_vendor_match(XBOX360_VID, XBOX360_PID_WIRELESS, 0xFF, XBOX_SUBCLASS);
  this->add_vendor_match(BITDO_VID, BITDO_PID_XBOX, 0xFF, XBOX_SUBCLASS);
  usb_host::USBClient::setup();
}

void USBHIDClient::dump_config() {
  usb_host::USBClient::dump_config();
  ESP_LOGCONFIG(TAG, "  Num Lock at start: %s", YESNO((this->leds_ & hid::LED_NUM_LOCK) != 0));
}

void USBHIDClient::loop() {
  Report *report;
  while ((report = this->reports_.pop()) != nullptr) {
    if (report->interface < this->interfaces_.size())
      this->report_(this->interfaces_[report->interface], report->data, report->length);
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
  this->fetch_descriptor_(0);
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
    if (intf.xbox) {
      kinds |= hid::HID_DEVICE_GAMEPAD;
    } else if (intf.map != nullptr) {
      kinds |= intf.map->device_kinds();
    }
  }
  return kinds;
}

bool USBHIDClient::find_interfaces_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;
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
      const bool xbox =
          intf->bInterfaceClass == 0xFF && intf->bInterfaceSubClass == XBOX_SUBCLASS &&
          (intf->bInterfaceProtocol == XBOX_PROTOCOL_WIRED || intf->bInterfaceProtocol == XBOX_PROTOCOL_WIRELESS);
      if (intf->bInterfaceClass != USB_CLASS_HID && !xbox)
        continue;
      // The wireless receiver has a data and a headset interface per pad: the first pad only.
      if (xbox && !this->interfaces_.empty())
        continue;
      Interface entry;
      entry.number = intf->bInterfaceNumber;
      entry.xbox = xbox;
      entry.xbox_wireless = xbox && intf->bInterfaceProtocol == XBOX_PROTOCOL_WIRELESS;
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
    if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_INT)
      continue;
    // bInterval 0 is not legal for an interrupt endpoint and the pipe is built from it.
    if (ep->bInterval == 0) {
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
         (this->interfaces_[index].xbox || this->interfaces_[index].descriptor_length == 0 ||
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
#ifdef USE_TEXT_SENSOR
  if (this->device_name_sensor_ != nullptr) {
    usb_host::UsbDeviceInfo info;
    this->report_connected_();
    if (this->get_device_info(info))
      this->device_name_sensor_->publish_state(info.product);
    return;
  }
#endif
  this->report_connected_();
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

void USBHIDClient::report_(Interface &intf, const uint8_t *data, size_t length) {
#ifdef USB_HID_ENABLE_RAW
  if (this->raw_driver_ != nullptr)
    this->raw_driver_->process_report(data, length);
#endif
  if (intf.xbox) {
    this->xbox360_(intf, data, length);
    return;
  }
  if (intf.map == nullptr || length == 0)
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

// Linux xpad.c, xpad360_process_packet() and xpad360w_process_packet().
void USBHIDClient::xbox360_(Interface &intf, const uint8_t *data, size_t length) {
  if (intf.xbox_wireless) {
    if (length < 4 + 14 || data[1] != 0x01)
      return;
    data += 4;
    length -= 4;
  }
  if (length < 14 || data[0] != 0x00)
    return;
  const hid::HIDUsage gamepad(0x05, hid::PAGE_GENERIC_DESKTOP);
  std::vector<hid::HIDReportItemValue> values;
  auto add = [&](uint16_t page, uint16_t usage, int64_t value) {
    hid::HIDReportItemValue v(hid::HIDUsage(usage, page), value, value);
    v.application_usage = gamepad;
    values.push_back(v);
  };
  // Buttons: (byte, bit, page, usage). D-pad as the Generic Desktop D-pad usages.
  static constexpr struct {
    uint8_t byte, bit;
    uint16_t page, usage;
  } BUTTONS[] = {
      {2, 0, hid::PAGE_GENERIC_DESKTOP, 0x90},
      {2, 1, hid::PAGE_GENERIC_DESKTOP, 0x91},
      {2, 2, hid::PAGE_GENERIC_DESKTOP, 0x93},
      {2, 3, hid::PAGE_GENERIC_DESKTOP, 0x92},
      {2, 4, hid::PAGE_BUTTON, 8},   // Start
      {2, 5, hid::PAGE_BUTTON, 7},   // Back
      {2, 6, hid::PAGE_BUTTON, 9},   // left stick
      {2, 7, hid::PAGE_BUTTON, 10},  // right stick
      {3, 0, hid::PAGE_BUTTON, 5},   // LB
      {3, 1, hid::PAGE_BUTTON, 6},   // RB
      {3, 2, hid::PAGE_BUTTON, 11},  // Guide
      {3, 4, hid::PAGE_BUTTON, 1},   // A
      {3, 5, hid::PAGE_BUTTON, 2},   // B
      {3, 6, hid::PAGE_BUTTON, 3},   // X
      {3, 7, hid::PAGE_BUTTON, 4},   // Y
  };
  for (const auto &b : BUTTONS) {
    const bool now = (data[b.byte] >> b.bit) & 1;
    if (now != ((intf.xbox_last[b.byte] >> b.bit) & 1))
      add(b.page, b.usage, now ? 1 : 0);
  }
  auto s16 = [](const uint8_t *p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); };
  static constexpr struct {
    uint8_t offset;
    uint16_t usage;
    bool invert;
  } STICKS[] = {{6, 0x30, false}, {8, 0x31, true}, {10, 0x33, false}, {12, 0x34, true}};
  for (const auto &s : STICKS) {
    if (s16(data + s.offset) != s16(intf.xbox_last + s.offset)) {
      const int16_t v = s16(data + s.offset);
      add(hid::PAGE_GENERIC_DESKTOP, s.usage, s.invert ? ~v : v);
    }
  }
  if (data[4] != intf.xbox_last[4])
    add(hid::PAGE_GENERIC_DESKTOP, 0x32, data[4]);  // left trigger
  if (data[5] != intf.xbox_last[5])
    add(hid::PAGE_GENERIC_DESKTOP, 0x35, data[5]);  // right trigger
  memcpy(intf.xbox_last, data, sizeof(intf.xbox_last));
  if (!values.empty())
    this->process_hid_values(values);
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
