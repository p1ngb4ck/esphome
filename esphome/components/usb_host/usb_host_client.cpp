// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "usb_host.h"

#include <algorithm>
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/components/bytebuffer/bytebuffer.h"

#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <atomic>
#include <span>
#include <string>

namespace esphome::usb_host {

#pragma GCC diagnostic ignored "-Wparentheses"

using namespace bytebuffer;

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
static void print_ep_desc(const usb_ep_desc_t *ep_desc) {
  const char *ep_type_str;
  int type = ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
  switch (type) {
    case USB_BM_ATTRIBUTES_XFER_CONTROL:
      ep_type_str = "CTRL";
      break;
    case USB_BM_ATTRIBUTES_XFER_ISOC:
      ep_type_str = "ISOC";
      break;
    case USB_BM_ATTRIBUTES_XFER_BULK:
      ep_type_str = "BULK";
      break;
    case USB_BM_ATTRIBUTES_XFER_INT:
      ep_type_str = "INT";
      break;
    default:
      ep_type_str = NULL;
      break;
  }
  ESP_LOGV(TAG,
           "\t\t*** Endpoint descriptor ***\n"
           "\t\tbLength %d\n"
           "\t\tbDescriptorType %d\n"
           "\t\tbEndpointAddress 0x%x\tEP %d %s\n"
           "\t\tbmAttributes 0x%x\t%s\n"
           "\t\twMaxPacketSize %d\n"
           "\t\tbInterval %d",
           ep_desc->bLength, ep_desc->bDescriptorType, ep_desc->bEndpointAddress, USB_EP_DESC_GET_EP_NUM(ep_desc),
           USB_EP_DESC_GET_EP_DIR(ep_desc) ? "IN" : "OUT", ep_desc->bmAttributes, ep_type_str, ep_desc->wMaxPacketSize,
           ep_desc->bInterval);
}

static void usbh_print_intf_desc(const usb_intf_desc_t *intf_desc) {
  ESP_LOGV(TAG,
           "\t*** Interface descriptor ***\n"
           "\tbLength %d\n"
           "\tbDescriptorType %d\n"
           "\tbInterfaceNumber %d\n"
           "\tbAlternateSetting %d\n"
           "\tbNumEndpoints %d\n"
           "\tbInterfaceClass 0x%x\n"
           "\tiInterface %d",
           intf_desc->bLength, intf_desc->bDescriptorType, intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting,
           intf_desc->bNumEndpoints, intf_desc->bInterfaceClass, intf_desc->iInterface);
}

static void usbh_print_cfg_desc(const usb_config_desc_t *cfg_desc) {
  ESP_LOGV(TAG,
           "*** Configuration descriptor ***\n"
           "  bLength %d\n"
           "  bDescriptorType %d\n"
           "  wTotalLength %d\n"
           "  bNumInterfaces %d\n"
           "  bConfigurationValue %d\n"
           "  iConfiguration %d\n"
           "  bmAttributes 0x%x\n"
           "  bMaxPower %dmA",
           cfg_desc->bLength, cfg_desc->bDescriptorType, cfg_desc->wTotalLength, cfg_desc->bNumInterfaces,
           cfg_desc->bConfigurationValue, cfg_desc->iConfiguration, cfg_desc->bmAttributes, cfg_desc->bMaxPower * 2);
}

static void usb_client_print_device_descriptor(const usb_device_desc_t *devc_desc) {
  if (devc_desc == nullptr)
    return;
  ESP_LOGV(TAG,
           "*** Device descriptor ***\n"
           "  bLength %d\n"
           "  bDescriptorType %d\n"
           "  bcdUSB %d.%d0\n"
           "  bDeviceClass 0x%x\n"
           "  bDeviceSubClass 0x%x\n"
           "  bDeviceProtocol 0x%x\n"
           "  bMaxPacketSize0 %d\n"
           "  idVendor 0x%x\n"
           "  idProduct 0x%x\n"
           "  bcdDevice %d.%d0\n"
           "  iManufacturer %d\n"
           "  iProduct %d\n"
           "  iSerialNumber %d\n"
           "  bNumConfigurations %d",
           devc_desc->bLength, devc_desc->bDescriptorType, ((devc_desc->bcdUSB >> 8) & 0xF),
           ((devc_desc->bcdUSB >> 4) & 0xF), devc_desc->bDeviceClass, devc_desc->bDeviceSubClass,
           devc_desc->bDeviceProtocol, devc_desc->bMaxPacketSize0, devc_desc->idVendor, devc_desc->idProduct,
           ((devc_desc->bcdDevice >> 8) & 0xF), ((devc_desc->bcdDevice >> 4) & 0xF), devc_desc->iManufacturer,
           devc_desc->iProduct, devc_desc->iSerialNumber, devc_desc->bNumConfigurations);
}

static void usb_client_print_config_descriptor(const usb_config_desc_t *cfg_desc,
                                               print_class_descriptor_cb class_specific_cb) {
  if (cfg_desc == nullptr)
    return;
  int offset = 0;
  uint16_t w_total_length = cfg_desc->wTotalLength;
  const usb_standard_desc_t *next_desc = reinterpret_cast<const usb_standard_desc_t *>(cfg_desc);
  do {
    switch (next_desc->bDescriptorType) {
      case USB_W_VALUE_DT_CONFIG:
        usbh_print_cfg_desc(reinterpret_cast<const usb_config_desc_t *>(next_desc));
        break;
      case USB_W_VALUE_DT_INTERFACE:
        usbh_print_intf_desc(reinterpret_cast<const usb_intf_desc_t *>(next_desc));
        break;
      case USB_W_VALUE_DT_ENDPOINT:
        print_ep_desc(reinterpret_cast<const usb_ep_desc_t *>(next_desc));
        break;
      default:
        if (class_specific_cb)
          class_specific_cb(next_desc);
        break;
    }
    next_desc = usb_parse_next_descriptor(next_desc, w_total_length, &offset);
  } while (next_desc != nullptr);
}
#endif  // ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
// bLength (uint8_t, max 255) includes the 2-byte header (bLength and bDescriptorType),
// so character count = (bLength - 2) / 2.
bool copy_descriptor_string(const usb_str_desc_t *desc, std::span<char, DESC_STRING_BUF_SIZE> buffer) {
  buffer[0] = '\0';
  if (desc == nullptr || desc->bLength < 2)
    return true;
  int char_count = (desc->bLength - 2) / 2;
  char *p = buffer.data();
  char *end = p + buffer.size() - 1;
  for (int i = 0; i != char_count && p < end; i++) {
    auto c = desc->wData[i];
    // TODO: encode non-ASCII code units as UTF-8 if a device with such descriptors turns up
    if (c >= 0x80) {
      buffer[0] = '\0';
      return false;
    }
    *p++ = static_cast<char>(c);
  }
  *p = '\0';
  return true;
}

// Folds UTF-16 to Latin-1 for logging, dropping anything that does not fit
template<typename T>
static const char *utf16_to_latin1(const T *data, size_t count, std::span<char, DESC_STRING_BUF_SIZE> buffer) {
  char *p = buffer.data();
  char *end = p + buffer.size() - 1;
  for (size_t i = 0; i != count && p < end; i++) {
    if (data[i] < 0x100)
      *p++ = static_cast<char>(data[i]);
  }
  *p = '\0';
  return buffer.data();
}

static const char *get_descriptor_string(const usb_str_desc_t *desc, std::span<char, DESC_STRING_BUF_SIZE> buffer) {
  if (desc == nullptr || desc->bLength < 2)
    return "(unspecified)";
  // wData is a packed member: copied out rather than read through an unaligned pointer.
  uint16_t chars[(UINT8_MAX - 2) / 2];
  const size_t count = (desc->bLength - 2) / 2;
  std::memcpy(chars, reinterpret_cast<const uint8_t *>(desc) + offsetof(usb_str_desc_t, wData), count * 2);
  return utf16_to_latin1(chars, count, buffer);
}

static const char *filter_string(const char16_t *filter, std::span<char, DESC_STRING_BUF_SIZE> buffer) {
  return utf16_to_latin1(filter, std::char_traits<char16_t>::length(filter), buffer);
}

// Both sides are UTF-16: the descriptor by specification, the filter because code
// generation emits it as a u"" literal
static bool descriptor_string_equals(const usb_str_desc_t *desc, const char16_t *expected) {
  const int char_count = (desc == nullptr || desc->bLength < 2) ? 0 : (desc->bLength - 2) / 2;
  for (int i = 0; i != char_count; i++) {
    if (expected[i] == u'\0' || desc->wData[i] != expected[i])
      return false;
  }
  return expected[char_count] == u'\0';
}

bool USBClient::get_device_info(const UsbDevice &dev, UsbDeviceInfo &info) const {
  if (!dev.opened || dev.desc == nullptr)
    return false;
  info.vendor_id = dev.desc->idVendor;
  info.product_id = dev.desc->idProduct;
  info.bcd_device = dev.desc->bcdDevice;
  usb_device_info_t dev_info;
  if (usb_host_device_info(dev.handle, &dev_info) != ESP_OK)
    return false;
  copy_descriptor_string(dev_info.str_desc_manufacturer, info.manufacturer);
  copy_descriptor_string(dev_info.str_desc_product, info.product);
  copy_descriptor_string(dev_info.str_desc_serial_num, info.serial_number);
  return true;
}

bool USBClient::get_device_info(UsbDeviceInfo &info) const {
  if (!this->is_connected())
    return false;
  const usb_device_desc_t *desc;
  esp_err_t err = usb_host_get_device_descriptor(this->device_handle_, &desc);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device descriptor query failed: %s", esp_err_to_name(err));
    return false;
  }
  info.vendor_id = desc->idVendor;
  info.product_id = desc->idProduct;
  info.bcd_device = desc->bcdDevice;
  usb_device_info_t dev_info;
  err = usb_host_device_info(this->device_handle_, &dev_info);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device info query failed: %s", esp_err_to_name(err));
    return false;
  }
  if (!copy_descriptor_string(dev_info.str_desc_manufacturer, info.manufacturer)) {
    ESP_LOGW(TAG, "Manufacturer string descriptor is not ASCII");
  }
  if (!copy_descriptor_string(dev_info.str_desc_product, info.product)) {
    ESP_LOGW(TAG, "Product string descriptor is not ASCII");
  }
  if (!copy_descriptor_string(dev_info.str_desc_serial_num, info.serial_number)) {
    ESP_LOGW(TAG, "Serial number string descriptor is not ASCII");
  }
  return true;
}

#if defined(USE_USB_BULK_TRANSFERS) || defined(USE_USB_CONTROL_TRANSFERS)
// -- Static transfer callbacks (USB task context) ------------------------------

// Shared completion logic: fill status, fire callback, release slot.
// setup_offset is the number of leading bytes in data_buffer that are not payload. For
// control transfers the buffer starts with the 8 byte setup packet the host wrote, which is
// the request header and not the device's answer; bulk and interrupt buffers hold payload
// from byte 0.
static void complete_trq(TransferRequest *trq, const usb_transfer_t *xfer, size_t setup_offset) {
  trq->status.error_code = xfer->status;
  trq->status.success = xfer->status == USB_TRANSFER_STATUS_COMPLETED;
  trq->status.endpoint = xfer->bEndpointAddress;
  const size_t actual = (xfer->actual_num_bytes > 0) ? static_cast<size_t>(xfer->actual_num_bytes) : 0;
  trq->status.data = xfer->data_buffer + setup_offset;
  trq->status.data_len = (actual > setup_offset) ? (actual - setup_offset) : 0;
  if (trq->callback != nullptr)
    trq->callback(trq->status);
  trq->client->release_trq(trq);
}
#endif

#ifdef USE_USB_CONTROL_TRANSFERS
static void control_callback(const usb_transfer_t *xfer) {
  complete_trq(static_cast<TransferRequest *>(xfer->context), xfer, SETUP_PACKET_SIZE);
}
#endif

#ifdef USE_USB_BULK_TRANSFERS
static void transfer_callback(usb_transfer_t *xfer) {
  complete_trq(static_cast<TransferRequest *>(xfer->context), xfer, 0);
}
#endif

// -- USBClient event callback (USB task context) -------------------------------

void USBClient::client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
  auto *client = static_cast<USBClient *>(arg);
  // Every client hears of every new device; USBHost decides which driver gets it.
  if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    return;
  UsbEvent *event = client->event_pool.allocate();
  if (event == nullptr) {
    client->event_queue.increment_dropped_count();
    return;
  }
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      ESP_LOGD(TAG, "Device gone");
      event->type = EVENT_DEVICE_GONE;
      event->data.device_gone.handle = event_msg->dev_gone.dev_hdl;
      break;
    default:
      ESP_LOGD(TAG, "Unknown event %d", event_msg->event);
      client->event_pool.release(event);
      return;
  }
  client->event_queue.push(event);
  client->enable_loop_soon_any_context();
  App.wake_loop_threadsafe();
}

// -- USBClient lifecycle -------------------------------------------------------

void USBClient::setup() {
#ifndef USE_USB_HOST_SHARED_TASK
  usb_host_client_config_t config{.is_synchronous = false,
                                  .max_num_event_msg = 5,
                                  .async = {.client_event_callback = USBClient::client_event_cb, .callback_arg = this}};
  auto err = usb_host_client_register(&config, &this->handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "client register failed: %s", esp_err_to_name(err));
    this->status_set_error(LOG_STR("Client register failed"));
    this->mark_failed();
    return;
  }
#endif
  this->host_ = get_usb_host();
  if (this->host_ == nullptr) {
    ESP_LOGE(TAG, "USB host component not available");
    this->status_set_error(LOG_STR("USB host not available"));
    this->mark_failed();
    return;
  }
  this->host_->register_client(this);
  // The transfer buffers are not allocated here. Their size depends on the device, and no
  // device is attached yet: a client that ends up on the high-speed controller sees 512 byte
  // bulk packets where one on the full-speed controller sees 64, and both run at the same
  // time from this same code. The pool is allocated in handle_open_state_() once the
  // descriptors say what this device needs, and released again in disconnect().
#ifndef USE_USB_HOST_SHARED_TASK
  xTaskCreatePinnedToCore(usb_task_fn, "usb_task", USB_TASK_STACK_SIZE, this, USB_TASK_PRIORITY,
                          &this->usb_task_handle_, LOOP_TASK_CORE);
  if (this->usb_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create USB task");
    this->mark_failed();
  }
#endif
}

#ifdef USE_USB_HOST_SHARED_TASK
USBClient::Registration USBClient::ensure_registered_() {
  if (this->registered_) {
    const uint8_t release = this->release_.load(std::memory_order_acquire);
    if (release == RELEASE_WANTED)
      return Registration::WAIT;
    if (release == RELEASE_FAILED) {
      this->release_.store(RELEASE_NONE, std::memory_order_release);
      return Registration::READY;
    }
    if (release == RELEASE_NONE)
      return Registration::READY;
    this->registered_ = false;
    this->release_.store(RELEASE_NONE, std::memory_order_release);
  }
  usb_host_client_config_t config{.is_synchronous = false,
                                  .max_num_event_msg = 5,
                                  .async = {.client_event_callback = USBClient::client_event_cb, .callback_arg = this}};
  const esp_err_t err = usb_host_client_register(&config, &this->handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "client register failed: %s", esp_err_to_name(err));
    this->handle_ = nullptr;
    return Registration::FAIL;
  }
  if (!this->host_->attach_client(this)) {
    ESP_LOGE(TAG, "No slot for another active USB client");
    usb_host_client_deregister(this->handle_);
    this->handle_ = nullptr;
    return Registration::FAIL;
  }
  this->registered_ = true;
  return Registration::READY;
}

bool USBClient::unopened_devices_() const {
  for (const auto &dev : this->devices_) {
    if (!dev->opened)
      return true;
  }
  return false;
}

void USBClient::release_if_idle_() {
  if (!this->registered_ || this->release_.load(std::memory_order_acquire) != RELEASE_NONE)
    return;
  if (this->state_ != USB_CLIENT_INIT || !this->devices_.empty() || !this->dead_devices_.empty() || this->starting_())
    return;
  this->host_->release_client(this);
}
#else
void USBClient::usb_task_fn(void *arg) { static_cast<USBClient *>(arg)->usb_task_loop_(); }

void USBClient::usb_task_loop_() const {
  while (true)
    usb_host_client_handle_events(this->handle_, portMAX_DELAY);
}
#endif

bool USBClient::process_usb_events_() {
  bool had_work = false;
  UsbEvent *event;
  while ((event = this->event_queue.pop()) != nullptr) {
    had_work = true;
    switch (event->type) {
      case EVENT_DEVICE_GONE:
        this->on_removed(event->data.device_gone.handle);
        break;
    }
    this->event_pool.release(event);
  }
  uint16_t dropped = this->event_queue.get_and_reset_dropped_count();
  if (dropped > 0)
    ESP_LOGW(TAG, "Dropped %u USB events due to queue overflow", dropped);
#ifdef USE_USB_HOST_SHARED_TASK
  if (this->state_ == USB_CLIENT_OPEN || (this->accepts_more_devices_() && this->unopened_devices_())) {
    const Registration registration = this->ensure_registered_();
    if (registration == Registration::WAIT)
      return had_work;
    if (registration == Registration::FAIL) {
      if (this->state_ == USB_CLIENT_OPEN) {
        const int addr = this->device_addr_;
        this->state_ = USB_CLIENT_INIT;
        this->device_addr_ = -1;
        this->routed_interfaces_ = 0;
        this->host_->client_released(this, addr);
      }
      for (size_t i = 0; i < this->devices_.size();) {
        if (!this->devices_[i]->opened) {
          this->drop_device_(this->devices_[i].get());
          continue;
        }
        i++;
      }
      return true;
    }
  }
#endif
  if (this->state_ == USB_CLIENT_OPEN) {
    had_work = true;
    this->handle_open_state_();
  }
  if (this->accepts_more_devices_() && this->open_devices_())
    had_work = true;
  if (!this->dead_devices_.empty() && this->reap_devices_())
    had_work = true;
  return had_work;
}

void USBClient::loop() {
  const bool work = this->process_usb_events_();
#ifdef USE_USB_HOST_SHARED_TASK
  this->release_if_idle_();
#endif
  if (!work)
    this->disable_loop();
}

void USBClient::handle_open_state_() {
  const int addr = this->device_addr_;
  esp_err_t err = usb_host_device_open(this->handle_, addr, &this->device_handle_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device %d open failed: %s", addr, esp_err_to_name(err));
    this->state_ = USB_CLIENT_INIT;
    this->device_addr_ = -1;
    this->routed_interfaces_ = 0;
    this->host_->client_released(this, addr);
    return;
  }
  const usb_device_desc_t *desc;
  err = usb_host_get_device_descriptor(this->device_handle_, &desc);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device get_desc failed: %s", esp_err_to_name(err));
    this->disconnect();
    return;
  }
  this->device_desc_ = desc;
  const usb_config_desc_t *cfg_desc;
  err = usb_host_get_active_config_descriptor(this->device_handle_, &cfg_desc);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device get_config_desc failed: %s", esp_err_to_name(err));
    this->disconnect();
    return;
  }
  this->config_desc_ = cfg_desc;
  usb_device_info_t dev_info;
  err = usb_host_device_info(this->device_handle_, &dev_info);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device info failed: %s", esp_err_to_name(err));
    this->disconnect();
    return;
  }
  if (!this->alloc_transfer_pool_()) {
    this->disconnect();
    return;
  }
  this->state_ = USB_CLIENT_CONNECTED;
  char buf_manuf[DESC_STRING_BUF_SIZE];
  char buf_product[DESC_STRING_BUF_SIZE];
  char buf_serial[DESC_STRING_BUF_SIZE];
  ESP_LOGD(TAG, "Device connected: Manuf: %s; Prod: %s; Serial: %s",
           get_descriptor_string(dev_info.str_desc_manufacturer, buf_manuf),
           get_descriptor_string(dev_info.str_desc_product, buf_product),
           get_descriptor_string(dev_info.str_desc_serial_num, buf_serial));
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  usb_client_print_device_descriptor(this->device_desc_);
  usb_client_print_config_descriptor(this->config_desc_, nullptr);
#endif
  this->on_connected();
  // on_connected() may have rejected the device (no usable interface, say) and closed it
  if (this->state_ == USB_CLIENT_CONNECTED && !this->reports_connection_itself()) {
    this->report_connected_();
  }
}

bool USBClient::settled_() const {
  if (this->accepts_more_devices_()) {
    for (const auto &dev : this->devices_) {
      if (!dev->opened)
        return false;
    }
  } else if (this->state_ == USB_CLIENT_CONNECTED) {
    if (this->reports_connection_itself() && !this->connection_reported_)
      return false;
  } else if (this->state_ != USB_CLIENT_INIT) {
    return false;
  }
  return !this->starting_();
}

void USBClient::report_connected_() {
  if (this->state_ != USB_CLIENT_CONNECTED || this->connection_reported_) {
    return;
  }
  this->connection_reported_ = true;
  this->connection_callback_.call(true);
}

static size_t buffer_size_for(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc) {
  // Every endpoint of the active configuration, not only the ones this driver claims: the
  // pool is shared across them and a driver may claim another interface later. This is what
  // lets the same code serve a device on the high-speed controller and one on the
  // full-speed controller at the same time, each with the packet size it actually declares.
  // USB_EP_DESC_GET_MPS() masks off the high-speed multiplier bits, which is what a single
  // packet holds; the isochronous path allocates its own transfers and handles MULT itself.
  size_t largest = device_desc != nullptr ? device_desc->bMaxPacketSize0 : 0;
  if (config_desc != nullptr) {
    int offset = 0;
    const usb_standard_desc_t *next = reinterpret_cast<const usb_standard_desc_t *>(config_desc);
    while ((next = usb_parse_next_descriptor_of_type(next, config_desc->wTotalLength, USB_W_VALUE_DT_ENDPOINT,
                                                     &offset)) != nullptr) {
      const size_t mps = USB_EP_DESC_GET_MPS(reinterpret_cast<const usb_ep_desc_t *>(next));
      if (mps > largest)
        largest = mps;
    }
  }
  // The configured value is a floor, for a driver whose control data stage is larger than
  // any endpoint packet on the device. It never caps what the device asks for.
  if (largest < USB_MIN_TRANSFER_BUFFER)
    largest = USB_MIN_TRANSFER_BUFFER;
  // A control transfer keeps the 8 byte setup packet in the same buffer as its data stage,
  // so the buffer has to be that much larger than the largest packet or the data stage is
  // capped below one packet. That bites on a 64 byte device, where it would leave 56.
  return SETUP_PACKET_SIZE + largest;
}

size_t USBClient::transfer_buffer_size_() const { return buffer_size_for(this->device_desc_, this->config_desc_); }

bool USBClient::alloc_transfer_pool_() {
  const size_t size = this->transfer_buffer_size_();
  ESP_LOGD(TAG, "Allocating %u transfer buffers of %u bytes", static_cast<unsigned>(MAX_REQUESTS),
           static_cast<unsigned>(size));
  for (auto &request : this->requests_) {
    // A failure leaves request.transfer null, and the first get_trq_() handing out that slot
    // would dereference it. Give this device up instead of crashing on the first transfer.
    const auto err = usb_host_transfer_alloc(size, 0, &request.transfer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Transfer buffer alloc failed: %s", esp_err_to_name(err));
      this->free_transfer_pool_();
      return false;
    }
    request.client = this;
  }
  return true;
}

void USBClient::free_transfer_pool_() {
  for (auto &request : this->requests_) {
    if (request.transfer == nullptr)
      continue;
    usb_host_transfer_free(request.transfer);
    request.transfer = nullptr;
  }
  this->trq_in_use_.store(0);
}

// Bit per bInterfaceNumber of every interface (any alt-setting) the pattern matches.
static uint32_t interfaces_matching(const usb_config_desc_t *cfg, const UsbMatch &m) {
  uint32_t mask = 0;
  int offset = 0;
  const auto *next = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((next = usb_parse_next_descriptor_of_type(next, cfg->wTotalLength, USB_W_VALUE_DT_INTERFACE, &offset)) !=
         nullptr) {
    const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(next);
    if (intf->bInterfaceNumber >= 32)
      continue;
    if ((m.cls < 0 || intf->bInterfaceClass == m.cls) && (m.subclass < 0 || intf->bInterfaceSubClass == m.subclass) &&
        (m.protocol < 0 || intf->bInterfaceProtocol == m.protocol))
      mask |= 1u << intf->bInterfaceNumber;
  }
  return mask;
}

// The interfaces this driver leaves alone (interface blacklist) are never its, whatever it matches them by.
MatchTier USBClient::match_(const RoutedDevice &dev, uint32_t &interfaces) const {
  MatchTier tier = this->match_all_(dev, interfaces);
  if constexpr (USB_INTERFACE_BLACKLIST_USED) {
    if (tier != MatchTier::NONE) {
      interfaces &= ~this->blacklisted_interfaces_(dev.desc);
      if (interfaces == 0)
        tier = MatchTier::NONE;
    }
  }
  return tier;
}

MatchTier USBClient::match_all_(const RoutedDevice &dev, uint32_t &interfaces) const {
  interfaces = 0;
  // Blacklisted: never this driver's, whatever its class or tables say
  if (this->blacklisted_(dev.desc))
    return MatchTier::NONE;
  const uint32_t all = interfaces_matching(dev.config, {0, 0, -1, -1, -1});
  uint32_t by_class = 0;
  for (const auto &m : this->class_matches_)
    by_class |= interfaces_matching(dev.config, m);
  uint32_t by_vendor = 0;
  for (const auto &m : this->vendor_matches_) {
    if (m.vid != dev.desc->idVendor || (m.pid != 0 && m.pid != dev.desc->idProduct))
      continue;
    by_vendor |= m.cls < 0 ? all : interfaces_matching(dev.config, m);
  }
  if constexpr (USB_WHITELIST_USED) {
    if (this->has_id_filter_()) {
      // Named (whitelist): this device and no other, whatever its class says.
      if ((this->vid_ != 0 && dev.desc->idVendor != this->vid_) || (this->pid_ != 0 && dev.desc->idProduct != this->pid_))
        return MatchTier::NONE;
      if (dev.strings_known && this->manufacturer_filter_ != nullptr &&
          !descriptor_string_equals(dev.info.str_desc_manufacturer, this->manufacturer_filter_))
        return MatchTier::NONE;
      if (dev.strings_known && this->product_filter_ != nullptr &&
          !descriptor_string_equals(dev.info.str_desc_product, this->product_filter_))
        return MatchTier::NONE;
      // Its own interfaces where its tables say which those are, so two drivers named to one
      // composite device (sound card with buttons) each get theirs; the whole device otherwise.
      interfaces = (by_class | by_vendor) != 0 ? (by_class | by_vendor) : all;
      return MatchTier::NAMED;
    }
  }
  MatchTier tier = MatchTier::NONE;
  if (by_vendor != 0) {
    interfaces = by_vendor;
    tier = MatchTier::VENDOR;
  } else if (by_class != 0) {
    interfaces = by_class;
    tier = MatchTier::CLASS;
  } else {
    return MatchTier::NONE;
  }
  tier = this->refine_match_(dev, tier);
  if (tier == MatchTier::NONE)
    interfaces = 0;
  return tier;
}

uint8_t USBClient::interface_channels_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                                       uint8_t interface_num) const {
  uint8_t widest = 0;
  int offset = 0;
  const auto *next = reinterpret_cast<const usb_standard_desc_t *>(config);
  while ((next = usb_parse_next_descriptor_of_type(next, config->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE,
                                                   &offset)) != nullptr) {
    const auto *alt = reinterpret_cast<const usb_intf_desc_t *>(next);
    if (alt->bInterfaceNumber != interface_num)
      continue;
    const uint32_t skip = this->skipped_endpoints_(desc, config, alt);
    uint8_t channels = 0;
    for_each_alt_endpoint(config, alt, [&](const usb_ep_desc_t *ep) {
      if ((skip & endpoint_bit(ep)) == 0)
        channels++;
    });
    widest = std::max(widest, channels);
  }
  return widest;
}

void USBClient::bind_(uint8_t addr, uint32_t interfaces) {
  this->device_addr_ = addr;
  this->routed_interfaces_ = interfaces;
  this->state_ = USB_CLIENT_OPEN;
  this->enable_loop();
}

void USBClient::on_removed(usb_device_handle_t handle) {
  if (this->accepts_more_devices_()) {
    for (auto &dev : this->devices_) {
      if (dev->handle == handle) {
        this->drop_device_(dev.get());
        return;
      }
    }
    return;
  }
  if (this->device_handle_ == handle)
    this->disconnect();
}

void USBClient::disconnect() {
  // Also reached for a device this client opened and then declined, or lost before it was
  // ready; neither was reported as connected, so neither is reported as removed
  const bool was_reported = this->connection_reported_;
  this->connection_reported_ = false;
  this->on_disconnected();
  auto err = usb_host_device_close(this->handle_, this->device_handle_);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "Device close failed: %s", esp_err_to_name(err));
  // After the close, so nothing is still in flight against these buffers. The next device
  // this client picks up gets a pool sized for itself.
  this->free_transfer_pool_();
  const int addr = this->device_addr_;
  this->state_ = USB_CLIENT_INIT;
  this->device_handle_ = nullptr;
  this->device_addr_ = -1;
  this->device_desc_ = nullptr;
  this->config_desc_ = nullptr;
  this->routed_interfaces_ = 0;
  if (was_reported) {
    this->connection_callback_.call(false);
  }
  if (addr >= 0)
    this->host_->client_released(this, addr);
#ifdef USE_USB_HOST_SHARED_TASK
  this->enable_loop();
#endif
}

void USBClient::dump_config() {
  ESP_LOGCONFIG(TAG, "USBClient %s", LOG_STR_ARG(this->get_component_log_str()));
  if (this->vid_ != 0 || this->pid_ != 0)
    ESP_LOGCONFIG(TAG, "  Vendor id %04X\n  Product id %04X", this->vid_, this->pid_);
  // The match rules only at VERY_VERBOSE: a driver's device list can be long
  for (const auto &m : this->class_matches_)
    ESP_LOGVV(TAG, "  Class %02X/%d/%d", m.cls, m.subclass, m.protocol);
  for (const auto &m : this->vendor_matches_)
    ESP_LOGVV(TAG, "  Vendor %04X:%04X class %d", m.vid, m.pid, m.cls);
  char buf[DESC_STRING_BUF_SIZE];
  if (this->manufacturer_filter_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Manufacturer %s", filter_string(this->manufacturer_filter_, buf));
  }
  if (this->product_filter_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Product %s", filter_string(this->product_filter_, buf));
  }
}

// -- TransferRequest pool management (lock-free, thread-safe) -----------------

TransferRequest *USBClient::get_trq_() {
  trq_bitmask_t mask = this->trq_in_use_.load(std::memory_order_acquire);
  for (;;) {
    if (mask == ALL_REQUESTS_IN_USE) {
      ESP_LOGE(TAG, "All %zu transfer slots in use", MAX_REQUESTS);
      return nullptr;
    }
    trq_bitmask_t lsb = ~mask & (mask + 1);
    trq_bitmask_t desired = mask | lsb;
    if (this->trq_in_use_.compare_exchange_weak(mask, desired, std::memory_order::acquire)) {
      auto i = __builtin_ctz(lsb);
      auto *trq = &this->requests_[i];
      if (trq->transfer == nullptr) {
        // No device attached, so the pool has not been sized yet. Only reachable if a driver
        // submits outside the connected state.
        ESP_LOGE(TAG, "No transfer buffers: no device attached");
        this->release_trq(trq);
        return nullptr;
      }
      trq->transfer->context = trq;
      trq->transfer->device_handle = this->device_handle_;
      return trq;
    }
  }
}

// A free slot of a further device's pool (several-devices drivers); null when all are in use.
static TransferRequest *device_trq(UsbDevice &dev) {
  trq_bitmask_t mask = dev.in_use.load(std::memory_order_acquire);
  for (;;) {
    if (mask == ALL_REQUESTS_IN_USE) {
      ESP_LOGE(TAG, "All %zu transfer slots of device %d in use", MAX_REQUESTS, dev.addr);
      return nullptr;
    }
    const trq_bitmask_t lsb = ~mask & (mask + 1);
    if (dev.in_use.compare_exchange_weak(mask, mask | lsb, std::memory_order_acquire)) {
      auto *trq = &dev.requests[__builtin_ctz(lsb)];
      trq->transfer->context = trq;
      trq->transfer->device_handle = dev.handle;
      return trq;
    }
  }
}

void USBClient::release_trq(TransferRequest *trq) {
  if (trq == nullptr)
    return;
  if (trq->device != nullptr) {
    // A further device's pool (several-devices drivers)
    UsbDevice *dev = trq->device;
    const size_t at = trq - dev->requests;
    if (at < MAX_REQUESTS) {
      const trq_bitmask_t bit = static_cast<trq_bitmask_t>(1) << at;
      const trq_bitmask_t left = dev->in_use.fetch_and(~bit, std::memory_order_acq_rel) & ~bit;
      if (left == 0 && dev->dead)
        this->enable_loop_soon_any_context();  // its memory can go now (reap_devices_, main loop)
    }
    return;
  }
  size_t index = trq - this->requests_;
  if (index >= MAX_REQUESTS) {
    ESP_LOGE(TAG, "Invalid TransferRequest pointer");
    return;
  }
  trq_bitmask_t mask = ~(static_cast<trq_bitmask_t>(1) << index);
  this->trq_in_use_.fetch_and(mask, std::memory_order_release);
}

// -- Thin forwarders -- allocate trq, fill fields, delegate to USBHost ----------

#ifdef USE_USB_BULK_TRANSFERS

bool USBClient::transfer_in(uint8_t ep_address, const transfer_cb_t &callback, uint16_t length) {
  auto *trq = this->get_trq_();
  if (trq == nullptr) {
    ESP_LOGE(TAG, "transfer_in: no free slots");
    return false;
  }
  if (length > trq->transfer->data_buffer_size) {
    ESP_LOGE(TAG, "transfer_in: length %u > buffer %u", length, trq->transfer->data_buffer_size);
    this->release_trq(trq);
    return false;
  }
  trq->callback = callback;
  trq->transfer->callback = transfer_callback;
  trq->transfer->bEndpointAddress = ep_address | USB_DIR_IN;
  // Callers pass the endpoint's wMaxPacketSize, so the host's "IN length must be a whole
  // number of packets" rule is already satisfied at the call site. Rounding here would ask
  // for more than the caller's buffer holds.
  trq->transfer->num_bytes = length;
  if (!this->host_->submit_transfer(trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}

bool USBClient::transfer_out(uint8_t ep_address, const transfer_cb_t &callback, const uint8_t *data, uint16_t length) {
  auto *trq = this->get_trq_();
  if (trq == nullptr) {
    ESP_LOGE(TAG, "transfer_out: no free slots");
    return false;
  }
  if (length > trq->transfer->data_buffer_size) {
    ESP_LOGE(TAG, "transfer_out: length %u > buffer %u", length, trq->transfer->data_buffer_size);
    this->release_trq(trq);
    return false;
  }
  trq->callback = callback;
  trq->transfer->callback = transfer_callback;
  trq->transfer->bEndpointAddress = ep_address | USB_DIR_OUT;
  trq->transfer->num_bytes = length;
  memcpy(trq->transfer->data_buffer, data, length);
  if (!this->host_->submit_transfer(trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}

#endif  // USE_USB_BULK_TRANSFERS

#ifdef USE_USB_CONTROL_TRANSFERS

// The setup packet and data stage of a control transfer into a slot; false when the data does not fit
// (the caller releases the slot).
static bool fill_control(TransferRequest *trq, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                         const transfer_cb_t &callback, const std::vector<uint8_t> &data, int32_t w_length) {
  auto length = data.size();
  if (length > trq->transfer->data_buffer_size - SETUP_PACKET_SIZE) {
    ESP_LOGE(TAG, "control_transfer: data too large: %u", length);
    return false;
  }
  const size_t setup_length = (w_length < 0) ? length : static_cast<size_t>(w_length);
  if (setup_length > length) {
    // The device is being told it may send or receive more than the buffer holds.
    ESP_LOGE(TAG, "control_transfer: wLength %u exceeds the %u byte buffer", static_cast<unsigned>(setup_length),
             static_cast<unsigned>(length));
    return false;
  }
  auto control_packet = ByteBuffer(SETUP_PACKET_SIZE, LITTLE);
  control_packet.put_uint8(type);
  control_packet.put_uint8(request);
  control_packet.put_uint16(value);
  control_packet.put_uint16(index);
  control_packet.put_uint16(setup_length);
  memcpy(trq->transfer->data_buffer, control_packet.get_data().data(), SETUP_PACKET_SIZE);
  if (length != 0 && !(type & USB_DIR_IN))
    memcpy(trq->transfer->data_buffer + SETUP_PACKET_SIZE, data.data(), length);
  trq->callback = callback;
  trq->transfer->bEndpointAddress = type & USB_DIR_MASK;
  trq->transfer->num_bytes = static_cast<int>(length + SETUP_PACKET_SIZE);
  trq->transfer->callback = reinterpret_cast<usb_transfer_cb_t>(control_callback);
  return true;
}

bool USBClient::control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                                 const transfer_cb_t &callback, const std::vector<uint8_t> &data, int32_t w_length) {
  auto *trq = this->get_trq_();
  if (trq == nullptr)
    return false;
  if (!fill_control(trq, type, request, value, index, callback, data, w_length)) {
    this->release_trq(trq);
    return false;
  }
  if (!this->host_->submit_control(this->handle_, trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}

bool USBClient::set_interface(uint8_t interface_num, uint8_t alt_setting, const transfer_cb_t &callback) {
  static constexpr uint8_t REQ_TYPE = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
  static constexpr uint8_t B_REQUEST_SET_INTERFACE = 11;
  // Submission only. The device has not switched until the callback runs.
  return this->control_transfer(REQ_TYPE, B_REQUEST_SET_INTERFACE, alt_setting, interface_num, callback);
}

bool USBClient::set_interface(UsbDevice &dev, uint8_t interface_num, uint8_t alt_setting,
                              const transfer_cb_t &callback) {
  static constexpr uint8_t REQ_TYPE = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
  static constexpr uint8_t B_REQUEST_SET_INTERFACE = 11;
  return this->control_transfer(dev, REQ_TYPE, B_REQUEST_SET_INTERFACE, alt_setting, interface_num, callback);
}

#endif  // USE_USB_CONTROL_TRANSFERS

bool USBClient::claim_interface(uint8_t interface_num, uint8_t alt_setting) {
  return this->host_->do_claim_interface(this, this->handle_, this->device_handle_, interface_num, alt_setting);
}

bool USBClient::release_interface(uint8_t interface_num) {
  return this->host_->do_release_interface(this->handle_, this->device_handle_, interface_num);
}

// -- Several devices per client ---------------------------------------------------

#ifdef USE_USB_BULK_TRANSFERS
bool USBClient::transfer_in(UsbDevice &dev, uint8_t ep_address, const transfer_cb_t &callback, uint16_t length) {
  auto *trq = device_trq(dev);
  if (trq == nullptr)
    return false;
  if (length > trq->transfer->data_buffer_size) {
    ESP_LOGE(TAG, "transfer_in: length %u > buffer %u", length, trq->transfer->data_buffer_size);
    this->release_trq(trq);
    return false;
  }
  trq->callback = callback;
  trq->transfer->callback = transfer_callback;
  trq->transfer->bEndpointAddress = ep_address | USB_DIR_IN;
  trq->transfer->num_bytes = length;
  if (!this->host_->submit_transfer(trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}

bool USBClient::transfer_out(UsbDevice &dev, uint8_t ep_address, const transfer_cb_t &callback, const uint8_t *data,
                             uint16_t length) {
  auto *trq = device_trq(dev);
  if (trq == nullptr)
    return false;
  if (length > trq->transfer->data_buffer_size) {
    ESP_LOGE(TAG, "transfer_out: length %u > buffer %u", length, trq->transfer->data_buffer_size);
    this->release_trq(trq);
    return false;
  }
  trq->callback = callback;
  trq->transfer->callback = transfer_callback;
  trq->transfer->bEndpointAddress = ep_address | USB_DIR_OUT;
  trq->transfer->num_bytes = length;
  memcpy(trq->transfer->data_buffer, data, length);
  if (!this->host_->submit_transfer(trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}
#endif  // USE_USB_BULK_TRANSFERS

#ifdef USE_USB_CONTROL_TRANSFERS
bool USBClient::control_transfer(UsbDevice &dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                                 const transfer_cb_t &callback, const std::vector<uint8_t> &data, int32_t w_length) {
  auto *trq = device_trq(dev);
  if (trq == nullptr)
    return false;
  if (!fill_control(trq, type, request, value, index, callback, data, w_length)) {
    this->release_trq(trq);
    return false;
  }
  if (!this->host_->submit_control(this->handle_, trq)) {
    this->release_trq(trq);
    return false;
  }
  return true;
}
#endif  // USE_USB_CONTROL_TRANSFERS

bool USBClient::claim_interface(UsbDevice &dev, uint8_t interface_num, uint8_t alt_setting) {
  return this->host_->do_claim_interface(this, this->handle_, dev.handle, interface_num, alt_setting);
}

bool USBClient::release_interface(UsbDevice &dev, uint8_t interface_num) {
  return this->host_->do_release_interface(this->handle_, dev.handle, interface_num);
}

// Routing (main loop) handed a further device over; it is opened on this client's next loop pass.
void USBClient::bind_device_(uint8_t addr, uint32_t interfaces) {
  auto dev = std::make_unique<UsbDevice>();
  dev->addr = addr;
  dev->interfaces = interfaces;
  this->devices_.push_back(std::move(dev));
  this->enable_loop();
}

bool USBClient::open_devices_() {
  bool work = false;
  for (size_t i = 0; i < this->devices_.size();) {
    UsbDevice *dev = this->devices_[i].get();
    if (!dev->opened) {
      work = true;
      if (!this->open_device_(*dev)) {
        this->drop_device_(dev);  // erases it: the same index is the next device
        continue;
      }
      // on_device_connected() may have closed it again (erased too)
      if (i >= this->devices_.size() || this->devices_[i].get() != dev)
        continue;
    }
    i++;
  }
  return work;
}

bool USBClient::open_device_(UsbDevice &dev) {
  esp_err_t err = usb_host_device_open(this->handle_, dev.addr, &dev.handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device %d open failed: %s", dev.addr, esp_err_to_name(err));
    dev.handle = nullptr;
    return false;
  }
  if (usb_host_get_device_descriptor(dev.handle, &dev.desc) != ESP_OK ||
      usb_host_get_active_config_descriptor(dev.handle, &dev.config) != ESP_OK) {
    ESP_LOGW(TAG, "Device %d: descriptors not readable", dev.addr);
    return false;
  }
  // Sized for this device, as the single-device pool is (alloc_transfer_pool_)
  const size_t size = buffer_size_for(dev.desc, dev.config);
  for (auto &request : dev.requests) {
    if (usb_host_transfer_alloc(size, 0, &request.transfer) != ESP_OK) {
      ESP_LOGE(TAG, "Device %d: transfer buffer alloc failed", dev.addr);
      return false;
    }
    request.client = this;
    request.device = &dev;
  }
  dev.opened = true;
  ESP_LOGD(TAG, "Device %d (%04X:%04X) opened by %s", dev.addr, dev.desc->idVendor, dev.desc->idProduct,
           LOG_STR_ARG(this->get_component_log_str()));
  this->on_device_connected(dev);
  // The connection callbacks: the first device of the client came (on_device_connected may have closed it)
  if (!this->connection_reported_ && !this->devices_.empty()) {
    this->connection_reported_ = true;
    this->connection_callback_.call(true);
  }
  return true;
}

void USBClient::close_device(UsbDevice &dev) { this->drop_device_(&dev); }

// Main loop: the let-go devices whose transfers are all back - their buffers, the driver's context, the record.
bool USBClient::reap_devices_() {
  bool any = false;
  for (size_t i = 0; i < this->dead_devices_.size();) {
    UsbDevice *dev = this->dead_devices_[i].get();
    if (dev->in_use.load(std::memory_order_acquire) != 0 || this->device_busy_(*dev)) {
      i++;
      continue;
    }
    for (auto &request : dev->requests) {
      if (request.transfer != nullptr) {
        usb_host_transfer_free(request.transfer);
        request.transfer = nullptr;
      }
    }
    this->free_device_context(*dev);
    this->dead_devices_.erase(this->dead_devices_.begin() + i);
    any = true;
  }
  return any;
}

void USBClient::drop_device_(UsbDevice *dev) {
  auto it = std::find_if(this->devices_.begin(), this->devices_.end(),
                         [dev](const std::unique_ptr<UsbDevice> &d) { return d.get() == dev; });
  if (it == this->devices_.end())
    return;
  if (dev->opened) {
    dev->opened = false;
    this->on_device_disconnected(*dev);
  }
  if (dev->handle != nullptr) {
    const esp_err_t err = usb_host_device_close(this->handle_, dev->handle);
    if (err != ESP_OK)
      ESP_LOGE(TAG, "Device %d close failed: %s", dev->addr, esp_err_to_name(err));
    dev->handle = nullptr;  // a stream still draining sees the device closed (stream_device_handle_)
  }
  const int addr = dev->addr;
  // Its memory waits until no transfer of it is out (a flushed transfer still completes on the USB task):
  // the last one coming back wakes the loop, reap_devices_() frees it.
  dev->dead = true;
  this->dead_devices_.push_back(std::move(*it));
  this->devices_.erase(it);
  this->reap_devices_();
  if (this->devices_.empty() && this->connection_reported_) {
    this->connection_reported_ = false;
    this->connection_callback_.call(false);
  }
  if (addr >= 0)
    this->host_->client_released(this, addr);
#ifdef USE_USB_HOST_SHARED_TASK
  this->enable_loop();
#endif
}

// -- Isochronous thin forwarders -----------------------------------------------
#ifdef USE_USB_ISOC_TRANSFERS

usb_transfer_t *USBClient::isoc_alloc(uint8_t ep_addr, uint16_t mps, uint8_t num_packets, usb_transfer_cb_t callback,
                                      void *context) {
  return this->host_->do_isoc_alloc(ep_addr, this->device_handle_, mps, num_packets, callback, context);
}

bool USBClient::isoc_submit(usb_transfer_t *xfer) { return this->host_->do_isoc_submit(xfer); }

void USBClient::isoc_free(usb_transfer_t *xfer) { this->host_->do_isoc_free(xfer); }

bool USBClient::stream_open(IsocStream &stream, USBClient *cb) {
  return this->host_->stream_open(stream, cb, this->handle_, this->device_handle_);
}

bool USBClient::stream_open(UsbDevice &dev, IsocStream &stream, USBClient *cb) {
  stream.device = &dev;
  return this->host_->stream_open(stream, cb, this->handle_, dev.handle);
}

void USBClient::stream_close(IsocStream &stream) { this->host_->stream_close(stream); }

#endif  // USE_USB_ISOC_TRANSFERS

}  // namespace esphome::usb_host
#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
