// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "usb_host.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#if defined(USE_ESP32_VARIANT_ESP32P4)
#include "driver/gpio.h"
#include "hal/usb_wrap_ll.h"
#endif

namespace esphome::usb_host {

// Routing client events; a hub brings up several devices at once.
static constexpr int HOST_EVENT_QUEUE_SIZE = 16;
// After a mode switch the device is left alone this long, to re-enumerate in its new mode.
static constexpr uint32_t MODE_SWITCH_SETTLE_MS = 5000;
// The same ids back within this long after a switch: the switch did not take.
static constexpr uint32_t MODE_SWITCH_RETRY_MS = 15000;
// A bulk step the device does not complete is cancelled after this long.
static constexpr uint32_t MODE_SWITCH_STEP_TIMEOUT_MS = 5000;

static USBHost *&usb_host_ref() {
  static USBHost *instance = nullptr;
  return instance;
}

USBHost *get_usb_host() { return usb_host_ref(); }

void USBHost::setup() {
#if defined(USE_ESP32_VARIANT_ESP32P4)
  // The full-speed OTG controller shares two internal FSLS PHYs with USB-Serial-JTAG. Out of
  // reset the controller gets PHY 1 (GPIO26/27) and USB-Serial-JTAG gets PHY 0 (GPIO24/25).
  // A board that wires its full-speed connector to GPIO24/25 needs the other mapping, which
  // is a software register in LP_SYS -- no eFuse required. usb_phy.c does not touch this
  // selector, so it has to happen here, before usb_host_install() brings the PHY up.
  if (this->fs_phy_index_ == 0) {
    usb_wrap_ll_phy_select(nullptr, 0);
    // usb_phy.c raises the pad drive strength on GPIO26/27 unconditionally, because that is
    // what soc's internal_phy_io names. After the swap the controller drives the other pair,
    // so raise it there too rather than relying on the fixed constant.
    gpio_set_drive_capability(GPIO_NUM_24, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_25, GPIO_DRIVE_CAP_3);
    ESP_LOGI(TAG, "USB FS PHY 0 selected (D- GPIO24, D+ GPIO25)");
  }
#endif
  usb_host_config_t config{};
#if defined(USE_ESP32_VARIANT_ESP32P4) && defined(USE_USB_HOST_PER_PORT_FIFO_BIAS)
  if (this->peripheral_map_ != 0) {
    // Bit 0 is the high-speed controller, bit 1 the full-speed one.
    config.peripheral_map = this->peripheral_map_;
    ESP_LOGI(TAG, "USB host on %s", this->peripheral_map_ == 0b11  ? "both controllers"
                                    : this->peripheral_map_ == 0b10 ? "the full-speed controller"
                                                                     : "the high-speed controller");
  }
#endif
#ifdef USE_USB_HOST_PER_PORT_FIFO_BIAS
  // Each controller resolves its own split from its own FIFO depth, so setting one here
  // leaves the other on ESP-IDF's Kconfig default rather than moving it too.
  for (uint8_t i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {
    config.fifo_bias_per_port[i] = static_cast<usb_host_fifo_bias_t>(this->port_fifo_bias_[i]);
    if (this->port_fifo_bias_[i] != 0) {
      ESP_LOGI(TAG, "USB controller %u FIFO bias %u", i, this->port_fifo_bias_[i]);
    }
  }
  config.fsls_only = this->full_speed_only_;
  if (this->full_speed_only_)
    ESP_LOGI(TAG, "High-speed controller limited to full/low speed");
#endif
#ifdef USE_USB_HOST_CHANNEL_BUDGET
  // Every device's host channels are counted before it is configured (channel_budget_cb)
  config.enum_budget_cb = USBHost::channel_budget_cb;
  config.enum_budget_cb_arg = this;
#endif
  if (usb_host_install(&config) != ESP_OK) {
    this->status_set_error(LOG_STR("usb_host_install failed"));
    this->mark_failed();
    return;
  }
  // The routing client: it opens every new device to read its descriptors, and its events
  // are handled from loop(), so it needs no task of its own.
  usb_host_client_config_t client_config{
      .is_synchronous = false,
      .max_num_event_msg = HOST_EVENT_QUEUE_SIZE,
      .async = {.client_event_callback = USBHost::host_event_cb, .callback_arg = this}};
  if (usb_host_client_register(&client_config, &this->client_handle_) != ESP_OK) {
    this->status_set_error(LOG_STR("Routing client register failed"));
    this->mark_failed();
    return;
  }
#ifdef USE_USB_HOST_SHARED_TASK
  xTaskCreatePinnedToCore(USBHost::client_task_fn, "usb_task", SHARED_USB_TASK_STACK_SIZE, this, USB_TASK_PRIORITY,
                          &this->client_task_, LOOP_TASK_CORE);
  if (this->client_task_ == nullptr) {
    this->status_set_error(LOG_STR("USB task create failed"));
    this->mark_failed();
    return;
  }
  usb_host_client_set_wake_task(this->client_task_);
#endif
  usb_host_ref() = this;
}

#ifdef USE_USB_HOST_SHARED_TASK
void USBHost::client_task_fn(void *arg) { static_cast<USBHost *>(arg)->client_task_loop_(); }

void USBHost::client_task_loop_() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (auto &slot : this->active_) {
      USBClient *client = slot.load(std::memory_order_acquire);
      if (client == nullptr)
        continue;
      usb_host_client_handle_events(client->handle_, 0);
      if (client->release_.load(std::memory_order_acquire) != USBClient::RELEASE_WANTED)
        continue;
      if (usb_host_client_deregister(client->handle_) == ESP_OK) {
        slot.store(nullptr, std::memory_order_release);
        client->handle_ = nullptr;
        client->release_.store(USBClient::RELEASE_DONE, std::memory_order_release);
      } else {
        client->release_.store(USBClient::RELEASE_FAILED, std::memory_order_release);
      }
      client->enable_loop_soon_any_context();
    }
  }
}

bool USBHost::attach_client(USBClient *client) {
  for (auto &slot : this->active_) {
    USBClient *expected = nullptr;
    if (slot.compare_exchange_strong(expected, client, std::memory_order_acq_rel))
      return true;
  }
  return false;
}

void USBHost::release_client(USBClient *client) {
  client->release_.store(USBClient::RELEASE_WANTED, std::memory_order_release);
  xTaskNotifyGive(this->client_task_);
}
#endif

void USBHost::loop() {
  int err;
  // usb_host_lib_handle_events() does not write event_flags on ESP_ERR_TIMEOUT, which is
  // the normal case for a zero timeout, so reading it uninitialised below is UB.
  uint32_t event_flags = 0;
  err = usb_host_lib_handle_events(0, &event_flags);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
    ESP_LOGD(TAG, "lib_handle_events failed: %s", esp_err_to_name(err));
  }
  if (event_flags != 0) {
    ESP_LOGD(TAG, "Event flags %" PRIu32 "X", event_flags);
  }
  usb_host_client_handle_events(this->client_handle_, 0);
  for (auto &dev : this->devices_) {
    if (dev.taking)
      this->continue_take_(dev);
  }
  if (this->reroute_) {
    this->reroute_ = false;
    for (auto &dev : this->devices_)
      this->route_(dev);
  }
}

// -- Routing -------------------------------------------------------------------

void USBHost::register_client(USBClient *client) {
  this->clients_.push_back(client);
  // A device that came up before this driver did is offered to it now.
  this->reroute_ = true;
}

// Main loop: called from usb_host_client_handle_events() in loop().
void USBHost::host_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
  auto *host = static_cast<USBHost *>(arg);
  switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      host->device_new_(event_msg->new_dev.address);
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      host->device_gone_(event_msg->dev_gone.dev_hdl);
      break;
    default:
      break;
  }
}

void USBHost::device_new_(uint8_t addr) {
  RoutedDevice dev;
  dev.addr = addr;
  esp_err_t err = usb_host_device_open(this->client_handle_, addr, &dev.handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Device %u open failed: %s", addr, esp_err_to_name(err));
    return;
  }
  if ((err = usb_host_get_device_descriptor(dev.handle, &dev.desc)) != ESP_OK ||
      (err = usb_host_get_active_config_descriptor(dev.handle, &dev.config)) != ESP_OK ||
      (err = usb_host_device_info(dev.handle, &dev.info)) != ESP_OK) {
    ESP_LOGW(TAG, "Device %u descriptors unavailable: %s", addr, esp_err_to_name(err));
    usb_host_device_close(this->client_handle_, dev.handle);
    return;
  }
  ESP_LOGD(TAG, "Device %u: %04X:%04X", addr, dev.desc->idVendor, dev.desc->idProduct);
  this->devices_.push_back(std::move(dev));
  if (!this->start_mode_switch_(this->devices_.back()))
    this->route_(this->devices_.back());
  this->devices_callback_.call();
}

RoutedDevice *USBHost::find_device_(usb_device_handle_t handle) {
  for (auto &dev : this->devices_) {
    if (dev.handle == handle)
      return &dev;
  }
  return nullptr;
}

void USBHost::forget_device_(usb_device_handle_t handle) {
  for (auto it = this->devices_.begin(); it != this->devices_.end(); ++it) {
    if (it->handle != handle)
      continue;
    if (it->switch_intf != 0xFF)
      usb_host_interface_release(this->client_handle_, it->handle, it->switch_intf);
    usb_host_device_close(this->client_handle_, it->handle);
    this->devices_.erase(it);
    this->devices_callback_.call();
    return;
  }
}

void USBHost::device_gone_(usb_device_handle_t handle) {
  RoutedDevice *dev = this->find_device_(handle);
  if (dev == nullptr)
    return;
  ESP_LOGD(TAG, "Device %u gone", dev->addr);
  if (dev->switch_xfer != nullptr) {
    // Its transfer still has to come back; the device is closed from there.
    dev->gone = true;
    return;
  }
  this->forget_device_(handle);
}

// -- Mode switching --------------------------------------------------------------

bool USBHost::start_mode_switch_(RoutedDevice &dev) {
  for (auto &ms : this->mode_switches_) {
    if (ms.vid != dev.desc->idVendor || ms.pid != dev.desc->idProduct)
      continue;
    const uint32_t now = millis();
    if (ms.sent_ms != 0 && now - ms.sent_ms < MODE_SWITCH_RETRY_MS) {
      ESP_LOGW(TAG, "%04X:%04X is back unchanged after its mode switch; routing it as it is", ms.vid, ms.pid);
      return false;
    }
    ms.sent_ms = now != 0 ? now : 1;
    if (!ms.messages.empty()) {
      // The first interface with a bulk OUT endpoint, as usb_modeswitch picks it.
      int offset = 0;
      const auto *next = reinterpret_cast<const usb_standard_desc_t *>(dev.config);
      uint8_t intf = 0xFF;
      while ((next = usb_parse_next_descriptor(next, dev.config->wTotalLength, &offset)) != nullptr) {
        if (next->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
          const auto *desc = reinterpret_cast<const usb_intf_desc_t *>(next);
          if (dev.switch_ep_out != 0)
            break;
          intf = desc->bAlternateSetting == 0 ? desc->bInterfaceNumber : 0xFF;
          dev.switch_ep_in = 0;
          continue;
        }
        if (next->bDescriptorType != USB_W_VALUE_DT_ENDPOINT || intf == 0xFF)
          continue;
        const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(next);
        if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_BULK)
          continue;
        if (USB_EP_DESC_GET_EP_DIR(ep)) {
          if (dev.switch_ep_in == 0) {
            dev.switch_ep_in = ep->bEndpointAddress;
            dev.switch_mps_in = USB_EP_DESC_GET_MPS(ep);
          }
        } else if (dev.switch_ep_out == 0) {
          dev.switch_ep_out = ep->bEndpointAddress;
          dev.switch_intf = intf;
        }
      }
      if (dev.switch_ep_out == 0 || (ms.need_response && dev.switch_ep_in == 0)) {
        ESP_LOGW(TAG, "%04X:%04X: no bulk endpoint for its mode switch", ms.vid, ms.pid);
        dev.switch_intf = 0xFF;
        return false;
      }
      const uint8_t intf_num = dev.switch_intf;
      dev.switch_intf = 0xFF;
      esp_err_t err = usb_host_interface_claim(this->client_handle_, dev.handle, intf_num, 0);
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "%04X:%04X: claiming interface %u for the mode switch failed: %s", ms.vid, ms.pid, intf_num,
                 esp_err_to_name(err));
        return false;
      }
      dev.switch_intf = intf_num;
    }
    dev.mode_switch = &ms;
    dev.switch_step = 0;
    ESP_LOGI(TAG, "%04X:%04X: sending its mode switch", ms.vid, ms.pid);
    if (this->submit_switch_step_(dev))
      return true;
    this->finish_mode_switch_(dev.handle);
    return true;
  }
  return false;
}

bool USBHost::submit_switch_step_(RoutedDevice &dev) {
  const ModeSwitch &ms = *dev.mode_switch;
  usb_transfer_t *xfer = nullptr;
  esp_err_t err;
  if (ms.messages.empty()) {
    const size_t len = ms.data.size();
    if ((err = usb_host_transfer_alloc(SETUP_PACKET_SIZE + len, 0, &xfer)) != ESP_OK)
      return false;
    auto *setup = reinterpret_cast<usb_setup_packet_t *>(xfer->data_buffer);
    setup->bmRequestType = ms.request_type;
    setup->bRequest = ms.request;
    setup->wValue = ms.value;
    setup->wIndex = ms.index;
    setup->wLength = static_cast<uint16_t>(len);
    if (len != 0)
      memcpy(xfer->data_buffer + SETUP_PACKET_SIZE, ms.data.data(), len);
    xfer->num_bytes = static_cast<int>(SETUP_PACKET_SIZE + len);
    xfer->bEndpointAddress = 0;
  } else {
    const size_t per_message = ms.need_response ? 2 : 1;
    const auto &message = ms.messages[dev.switch_step / per_message];
    const bool response = ms.need_response && (dev.switch_step % 2) == 1;
    const size_t len = response ? dev.switch_mps_in : message.size();
    if ((err = usb_host_transfer_alloc(len, 0, &xfer)) != ESP_OK)
      return false;
    if (!response)
      memcpy(xfer->data_buffer, message.data(), len);
    xfer->num_bytes = static_cast<int>(len);
    xfer->bEndpointAddress = response ? dev.switch_ep_in : dev.switch_ep_out;
  }
  xfer->device_handle = dev.handle;
  xfer->callback = USBHost::switch_cb;
  xfer->context = this;
  xfer->timeout_ms = 0;
  err = ms.messages.empty() ? usb_host_transfer_submit_control(this->client_handle_, xfer)
                            : usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Mode switch transfer refused: %s", esp_err_to_name(err));
    usb_host_transfer_free(xfer);
    return false;
  }
  dev.switch_xfer = xfer;
  if (!ms.messages.empty()) {
    // A bulk step the device never completes is cancelled; that ends the switch.
    const usb_device_handle_t handle = dev.handle;
    const uint8_t ep = xfer->bEndpointAddress;
    this->set_timeout(dev.addr, MODE_SWITCH_STEP_TIMEOUT_MS, [this, handle, ep]() {
      RoutedDevice *d = this->find_device_(handle);
      if (d == nullptr || d->switch_xfer == nullptr)
        return;
      ESP_LOGW(TAG, "Mode switch step not completed; cancelling it");
      if (usb_host_endpoint_halt(handle, ep) == ESP_OK)
        usb_host_endpoint_flush(handle, ep);
    });
  }
  return true;
}

// Main loop: called from usb_host_client_handle_events() in loop().
void USBHost::switch_cb(usb_transfer_t *xfer) {
  auto *host = static_cast<USBHost *>(xfer->context);
  const usb_device_handle_t handle = xfer->device_handle;
  const bool ok = xfer->status == USB_TRANSFER_STATUS_COMPLETED;
  usb_host_transfer_free(xfer);
  RoutedDevice *dev = host->find_device_(handle);
  if (dev == nullptr)
    return;
  dev->switch_xfer = nullptr;
  host->cancel_timeout(dev->addr);
  const ModeSwitch &ms = *dev->mode_switch;
  const size_t steps = ms.messages.empty() ? 1 : ms.messages.size() * (ms.need_response ? 2 : 1);
  // A control switch usually ends with the device dropping off the bus, which is success.
  if (ok && !dev->gone && ++dev->switch_step < steps && host->submit_switch_step_(*dev))
    return;
  // The transfer still counts as in flight until its callback returns, so releasing the
  // interface here fails, and closing the device with it: the device is never freed and its
  // port never sees the device come back in its new mode.
  host->defer([host, handle]() { host->finish_mode_switch_(handle); });
}

void USBHost::finish_mode_switch_(usb_device_handle_t handle) {
  RoutedDevice *dev = this->find_device_(handle);
  if (dev == nullptr)
    return;
  if (dev->gone) {
    this->forget_device_(handle);
    return;
  }
  if (dev->switch_intf != 0xFF) {
    usb_host_interface_release(this->client_handle_, dev->handle, dev->switch_intf);
    dev->switch_intf = 0xFF;
  }
  dev->mode_switch = nullptr;
  dev->switched_ms = millis();
  if (dev->switched_ms == 0)
    dev->switched_ms = 1;
  // Still here once it had time to leave: route it as it is.
  this->set_timeout(MODE_SWITCH_SETTLE_MS, [this]() { this->reroute_ = true; });
}

void USBHost::client_released(USBClient *client, int addr) {
  for (auto &dev : this->devices_) {
    if (dev.addr != addr)
      continue;
    auto bound = std::find(dev.bound.begin(), dev.bound.end(), client);
    if (bound != dev.bound.end()) {
      dev.bound.erase(bound);
      // A taker gives the device back to the drivers it was taken from; they did not decline it.
      if (dev.taken_by == client) {
        dev.taken_by = nullptr;
      } else if (dev.taken_by == nullptr) {
        dev.declined.push_back(client);
      }
      auto leaving = std::find(dev.leaving.begin(), dev.leaving.end(), client);
      if (leaving != dev.leaving.end())
        dev.leaving.erase(leaving);
    }
    // Recomputed rather than subtracted: interfaces can overlap between drivers' masks.
    dev.assigned = 0;
    for (auto *other : dev.bound)
      dev.assigned |= other->routed_interfaces_;
    break;
  }
  // The driver is free again, and so are the interfaces it held.
  this->reroute_ = true;
}

bool USBHost::take_device(USBClient *taker, uint8_t addr) {
  RoutedDevice *dev = nullptr;
  for (auto &d : this->devices_) {
    if (d.addr == addr && !d.gone)
      dev = &d;
  }
  if (dev == nullptr || dev->mode_switch != nullptr)
    return false;
  if (dev->taken_by != nullptr)
    return dev->taken_by == taker;
  if (!taker->is_free_())
    return false;
  dev->taken_by = taker;
  dev->taking = true;
  dev->taking_ms = millis();
  dev->leaving.clear();
  // Every transfer of the holders comes back cancelled and a new one is refused (a halted pipe takes
  // none, hcd_dwc.c _check_port_pipe_state()), as their transfers end on an unplug.
  int offset = 0;
  const auto *next = reinterpret_cast<const usb_standard_desc_t *>(dev->config);
  while ((next = usb_parse_next_descriptor_of_type(next, dev->config->wTotalLength, USB_W_VALUE_DT_ENDPOINT,
                                                   &offset)) != nullptr) {
    const uint8_t ep = reinterpret_cast<const usb_ep_desc_t *>(next)->bEndpointAddress;
    // Not claimed by anyone: refused, nothing to stop there.
    if (usb_host_endpoint_halt(dev->handle, ep) == ESP_OK)
      usb_host_endpoint_flush(dev->handle, ep);
  }
  this->continue_take_(*dev);
  return true;
}

// Main loop: each holder is told the device is gone (its own unplug path) once its transfers are back;
// the taker gets the device when all have let go.
void USBHost::continue_take_(RoutedDevice &dev) {
  static constexpr uint32_t TAKE_WAIT_MS = 500;
  bool held = false;
  for (auto *client : dev.bound) {
    if (client == dev.taken_by)
      continue;
    held = true;
    if (client->state_ == USB_CLIENT_OPEN) {
      // Bound, not opened yet: nothing to let go of.
      client->state_ = USB_CLIENT_INIT;
      client->device_addr_ = -1;
      client->routed_interfaces_ = 0;
      this->client_released(client, dev.addr);
      return;  // bound changed
    }
    if (std::find(dev.leaving.begin(), dev.leaving.end(), client) != dev.leaving.end())
      continue;
    if (client->trq_in_use_.load(std::memory_order_acquire) != 0 && millis() - dev.taking_ms < TAKE_WAIT_MS)
      continue;
    UsbEvent *event = client->event_pool.allocate();
    if (event == nullptr)
      continue;
    event->type = EVENT_DEVICE_GONE;
    event->data.device_gone.handle = client->device_handle_;
    client->event_queue.push(event);
    client->enable_loop_soon_any_context();
    dev.leaving.push_back(client);
  }
  if (held)
    return;
  dev.taking = false;
  dev.leaving.clear();
  const uint32_t all = 0xFFFFFFFFu;
  dev.assigned = all;
  dev.bound.push_back(dev.taken_by);
  ESP_LOGI(TAG, "%04X:%04X -> %s", dev.desc->idVendor, dev.desc->idProduct,
           LOG_STR_ARG(dev.taken_by->get_component_log_str()));
  dev.taken_by->bind_(dev.addr, all);
}

void USBHost::route_(RoutedDevice &dev) {
  if (dev.taken_by != nullptr)
    return;
  if (dev.mode_switch != nullptr || (dev.switched_ms != 0 && millis() - dev.switched_ms < MODE_SWITCH_SETTLE_MS))
    return;
  auto listed = [](const std::vector<USBClient *> &list, USBClient *client) {
    return std::find(list.begin(), list.end(), client) != list.end();
  };
  // The interfaces a driver names (by vid / pid ...) are reserved for the drivers that name them, busy or not; the
  // device's other interfaces go to the drivers that match them by vendor or class (a camera's UVC and storage
  // beside its microphone that one driver names).
  bool named = false;
  uint32_t named_interfaces = 0;
  uint32_t interfaces;
  for (auto *client : this->clients_) {
    if (!listed(dev.declined, client) && client->match_(dev, interfaces) == MatchTier::NAMED) {
      named = true;
      named_interfaces |= interfaces;
    }
  }
  bool routed = false;
  for (MatchTier tier : {MatchTier::NAMED, MatchTier::VENDOR, MatchTier::CLASS}) {
    const uint32_t reserved = tier == MatchTier::NAMED ? 0 : named_interfaces;
    for (auto *client : this->clients_) {
      if (!client->is_free_() || listed(dev.bound, client) || listed(dev.declined, client))
        continue;
      if (client->match_(dev, interfaces) != tier)
        continue;
      const uint32_t free_interfaces = interfaces & ~dev.assigned & ~reserved;
      if (free_interfaces == 0)
        continue;
      ESP_LOGI(TAG, "%04X:%04X interfaces 0x%02" PRIX32 " -> %s", dev.desc->idVendor, dev.desc->idProduct,
               free_interfaces, LOG_STR_ARG(client->get_component_log_str()));
      dev.assigned |= free_interfaces;
      dev.bound.push_back(client);
      if (client->accepts_more_devices_()) {
        client->bind_device_(dev.addr, free_interfaces);
      } else {
        client->bind_(dev.addr, free_interfaces);
      }
      routed = true;
    }
  }
  if (!routed && dev.bound.empty() && !dev.reported_unrouted) {
    dev.reported_unrouted = true;
    // Why nobody took it: refused by a blacklist, every driver of its kind busy, or none for it
    USBClient *busy = nullptr;
    USBClient *refused = nullptr;
    for (auto *client : this->clients_) {
      if (client->blacklisted_(dev.desc)) {
        refused = client;
      } else if (busy == nullptr && !client->is_free_() && client->match_(dev, interfaces) != MatchTier::NONE) {
        busy = client;
      }
    }
    if (named) {
      ESP_LOGI(TAG, "%04X:%04X waits for the driver that names it", dev.desc->idVendor, dev.desc->idProduct);
    } else if (busy != nullptr) {
      ESP_LOGW(TAG, "%04X:%04X refused: every %s entry has a device (disallowed by config: a further device "
                    "needs its own entry)",
               dev.desc->idVendor, dev.desc->idProduct, LOG_STR_ARG(busy->get_component_log_str()));
    } else if (refused != nullptr) {
      ESP_LOGI(TAG, "%04X:%04X refused: on the blacklist of %s", dev.desc->idVendor, dev.desc->idProduct,
               LOG_STR_ARG(refused->get_component_log_str()));
    } else {
      ESP_LOGI(TAG, "%04X:%04X: no driver", dev.desc->idVendor, dev.desc->idProduct);
    }
  }
}

// -- Submission engine ---------------------------------------------------------

bool USBHost::submit_transfer(TransferRequest *trq) {
  esp_err_t err = usb_host_transfer_submit(trq->transfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "submit_transfer failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// -- Interface claim / release -------------------------------------------------

// The driver's claim opens pipes only for the endpoints of the alt-setting it uses (skipped_endpoints_()).
bool USBHost::do_claim_interface(USBClient *client, usb_host_client_handle_t client_handle,
                                 usb_device_handle_t device_handle, uint8_t interface_num, uint8_t alt_setting) {
#ifndef USE_USB_HOST_CHANNEL_BUDGET
  esp_err_t err = usb_host_interface_claim(client_handle, device_handle, interface_num, alt_setting);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "claim_interface %u alt %u failed: %s", interface_num, alt_setting, esp_err_to_name(err));
    return false;
  }
  return true;
#else
  uint32_t skip = 0;
  const usb_device_desc_t *desc = nullptr;
  const usb_config_desc_t *config = nullptr;
  if (usb_host_get_device_descriptor(device_handle, &desc) == ESP_OK &&
      usb_host_get_active_config_descriptor(device_handle, &config) == ESP_OK) {
    int offset = 0;
    const usb_intf_desc_t *alt = usb_parse_interface_descriptor(config, interface_num, alt_setting, &offset);
    if (alt != nullptr)
      skip = client->skipped_endpoints_(desc, config, alt);
  }
  esp_err_t err = usb_host_interface_claim_endpoints(client_handle, device_handle, interface_num, alt_setting, skip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "claim_interface %u alt %u failed: %s", interface_num, alt_setting, esp_err_to_name(err));
    return false;
  }
  if (skip != 0)
    ESP_LOGD(TAG, "Interface %u alt %u claimed without endpoints 0x%08" PRIX32, interface_num, alt_setting, skip);
  return true;
#endif
}

// -- Host channel budget -------------------------------------------------------------
#ifdef USE_USB_HOST_CHANNEL_BUDGET

int USBHost::channel_budget_cb(const usb_device_desc_t *desc, const usb_config_desc_t *config, void *arg) {
  return static_cast<USBHost *>(arg)->channel_need_(desc, config);
}

// The default pipe, then what routing would hand out: every interface the drivers would take (by the
// same tiers as route_(), busy or not), each at its widest alternate setting without the endpoints its
// driver skips. A hub's status pipe is the hub driver's; a device with a bulk mode switch claims a bulk
// pair for it first.
int USBHost::channel_need_(const usb_device_desc_t *desc, const usb_config_desc_t *config) const {
  int need = 1;
  auto endpoints_of_alt0 = [config](uint8_t number) {
    int offset = 0;
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config, number, 0, &offset);
    return intf != nullptr ? intf->bNumEndpoints : 0;
  };
  if (desc->bDeviceClass == USB_CLASS_HUB)
    return need + endpoints_of_alt0(0);
  RoutedDevice dev;
  dev.desc = desc;
  dev.config = config;
  dev.strings_known = false;
  bool named = false;
  uint32_t interfaces;
  for (auto *client : this->clients_) {
    if (client->match_(dev, interfaces) == MatchTier::NAMED)
      named = true;
  }
  int drivers = 0;
  for (MatchTier tier : {MatchTier::NAMED, MatchTier::VENDOR, MatchTier::CLASS}) {
    if (named && tier != MatchTier::NAMED)
      break;
    for (auto *client : this->clients_) {
      if (client->match_(dev, interfaces) != tier)
        continue;
      const uint32_t free_interfaces = interfaces & ~dev.assigned;
      dev.assigned |= free_interfaces;
      for (uint8_t n = 0; n < 32; n++) {
        if (free_interfaces & (1u << n))
          drivers += client->interface_channels_(desc, config, n);
      }
    }
  }
  for (const auto &ms : this->mode_switches_) {
    if (ms.vid == desc->idVendor && ms.pid == desc->idProduct && !ms.messages.empty())
      drivers = std::max(drivers, 2);  // its bulk OUT / IN pair, claimed before any driver
  }
  return need + drivers;
}
#endif  // USE_USB_HOST_CHANNEL_BUDGET

bool USBHost::do_release_interface(usb_host_client_handle_t client_handle, usb_device_handle_t device_handle,
                                   uint8_t interface_num) {
  esp_err_t err = usb_host_interface_release(client_handle, device_handle, interface_num);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "release_interface %u failed: %s", interface_num, esp_err_to_name(err));
    return false;
  }
  return true;
}

// -- Control transfer submission -----------------------------------------------
#ifdef USE_USB_CONTROL_TRANSFERS

bool USBHost::submit_control(usb_host_client_handle_t client_handle, TransferRequest *trq) {
  esp_err_t err = usb_host_transfer_submit_control(client_handle, trq->transfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "submit_control failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

#endif  // USE_USB_CONTROL_TRANSFERS

// -- Isochronous ---------------------------------------------------------------
#ifdef USE_USB_ISOC_TRANSFERS

usb_transfer_t *USBHost::do_isoc_alloc(uint8_t ep_addr, usb_device_handle_t device_handle, uint16_t mps,
                                       uint8_t num_packets, usb_transfer_cb_t callback, void *context) {
  if (mps == 0 || num_packets == 0) {
    ESP_LOGE(TAG, "isoc_alloc: invalid mps=%u or num_packets=%u", mps, num_packets);
    return nullptr;
  }
  size_t data_size = static_cast<size_t>(mps) * num_packets;
  usb_transfer_t *xfer = nullptr;
  esp_err_t err = usb_host_transfer_alloc(data_size, num_packets, &xfer);
  if (err != ESP_OK || xfer == nullptr) {
    ESP_LOGE(TAG, "isoc_alloc: alloc failed: %s", esp_err_to_name(err));
    return nullptr;
  }
  xfer->device_handle = device_handle;
  xfer->bEndpointAddress = ep_addr;
  xfer->num_bytes = static_cast<int>(data_size);
  xfer->timeout_ms = 0;
  xfer->callback = callback;
  xfer->context = context;
  for (int i = 0; i < num_packets; i++) {
    xfer->isoc_packet_desc[i].num_bytes = mps;
  }
  return xfer;
}

bool USBHost::do_isoc_submit(usb_transfer_t *xfer) {
  if (xfer == nullptr)
    return false;
  esp_err_t err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "isoc_submit failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

void USBHost::do_isoc_free(usb_transfer_t *xfer) {
  if (xfer != nullptr)
    usb_host_transfer_free(xfer);
}

// Next OUT packet size for the sample-clock pacing: the floor plus one audio frame whenever
// the fractional accumulator wraps (keeps non-integer frames-per-service-interval rates such
// as 44.1 kHz frame-aligned). Clamped to mps as a safety net. Advances stream state.
static uint32_t isoc_next_packet_size(IsocStream *stream) {
  uint32_t psize;
  const uint32_t fb = stream->fb_value.load(std::memory_order_relaxed);
  if (fb != 0) {
    // Asynchronous endpoint: the device-reported feedback (samples per service interval in
    // Q10.14 / Q16.16) drives the rate instead of the nominal sample_rate.
    const uint32_t one = 1u << stream->fb_shift;
    psize = (fb >> stream->fb_shift) * stream->frame_size;
    stream->fb_accum += fb & (one - 1);
    if (stream->fb_accum >= one) {
      stream->fb_accum -= one;
      psize += stream->frame_size;
    }
  } else {
    psize = stream->packet_size;
    stream->frac_accum += stream->packet_size_frac;
    if (stream->frac_div != 0 && stream->frac_accum >= stream->frac_div) {
      stream->frac_accum -= stream->frac_div;
      psize += stream->frame_size;
    }
  }
  if (psize == 0 || psize > stream->mps)
    psize = stream->mps;
  return psize;
}

// CALLBACK CONTEXT: USB task
void USBHost::isoc_cb(usb_transfer_t *xfer) {
  auto *ctx = static_cast<IsocCbCtx *>(xfer->context);
  USBClient *client = ctx->client;
  IsocStream *stream = ctx->stream;

  auto finish_urb = [xfer, stream, client]() {
    usb_host_transfer_free(xfer);
    if (stream->pending_urbs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      // Last URB gone. If nobody asked for a close, the stream stopped by itself and is now
      // silently dead: flag it so the owner can tear it down instead of writing into a
      // buffer that is never drained again.
      if (stream->streaming.exchange(false, std::memory_order_acq_rel)) {
        stream->died.store(true, std::memory_order_release);
        client->enable_loop_soon_any_context();  // the owner tears it down: wake it (USB task)
      }
      get_usb_host()->defer([stream, client] {
        stream->xfers.reset();
        stream->ctxs.reset();
        // Read the handles here rather than snapshotting them in the USB-task callback.
        // This lambda and disconnect() both run on the main loop, so either the device is
        // still open and the handles are valid, or disconnect() already closed it and
        // device_handle_ is null. Snapshotting would turn that null into a stale handle:
        // releasing an interface on a device the IDF has freed, or -- if the defer wins the
        // race -- leaving the interface claimed so usb_host_device_close() refuses and the
        // device object leaks for the life of the process.
        usb_device_handle_t device_handle = stream_device_handle_(*stream, client);
        if (device_handle != nullptr) {
          get_usb_host()->stream_release(*stream, client, client->handle_, device_handle);
        }
        stream->open_state.store(IsocOpenState::CLOSED, std::memory_order_release);
        client->enable_loop();  // closed: an owner waiting to open it again runs now (main loop)
        ESP_LOGD(TAG, "stream_close deferred: ep=0x%02X", stream->ep_addr);
      });
    }
  };

  if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE || xfer->status == USB_TRANSFER_STATUS_CANCELED) {
    finish_urb();
    return;
  }

  if (stream->streaming.load(std::memory_order_acquire)) {
    uint8_t *payload = xfer->data_buffer;
    uint32_t total_bytes = 0;
    for (int i = 0; i < xfer->num_isoc_packets; i++) {
      usb_isoc_packet_desc_t *desc = &xfer->isoc_packet_desc[i];
      const bool error = (desc->status != USB_TRANSFER_STATUS_COMPLETED && desc->status != USB_TRANSFER_STATUS_SKIPPED);
      // OUT: the sample clock decides how many bytes this packet may carry and on_isoc_fill()
      // writes into the URB. IN: report what was actually received, and keep asking for a
      // full mps.
      //
      // The payload stride is the packet size, not mps: the host controller lays the packets
      // out back to back, advancing by isoc_packet_desc[].num_bytes (see _buffer_fill_isoc()
      // in hcd_dwc.c), so a short packet moves every following packet down by that shortfall.
      // Writing at an mps stride only matches while every packet happens to be exactly mps.
      const uint32_t fill = stream->is_output ? isoc_next_packet_size(stream) : stream->mps;
      if (stream->is_output) {
        if (error)
          client->on_stream_out_error(*stream);
        const size_t written = client->on_stream_fill(*stream, payload, fill);
        // Whatever the client did not produce is silence, not the previous packet again.
        if (written < fill)
          memset(payload + written, 0, fill - written);
      } else {
        client->on_stream_packet(*stream, payload, desc->actual_num_bytes, error,
                                 desc->status == USB_TRANSFER_STATUS_SKIPPED);
      }
      desc->num_bytes = fill;
      payload += fill;
      total_bytes += fill;
    }
    // Isochronous transfers are described by the packet list, but keep the transfer length
    // consistent with it rather than leaving the allocation size behind.
    xfer->num_bytes = static_cast<int>(total_bytes);
    if (usb_host_transfer_submit(xfer) != ESP_OK) {
      // Resubmission failed (e.g. periodic scheduler rejected it): retire this URB instead of
      // leaving it dangling. The stream keeps running on the remaining URBs, and when the last
      // one is retired finish_urb() tears the stream down cleanly rather than stalling silently.
      ESP_LOGW(TAG, "isoc resubmit failed (ep=0x%02X), retiring URB", xfer->bEndpointAddress);
      finish_urb();
    }
  } else {
    // stream_close() set streaming=false -- free and defer cleanup when all URBs done.
    finish_urb();
  }
}

bool USBHost::stream_start_urbs(IsocStream &stream, USBClient *cb, usb_device_handle_t device_handle) {
  stream.xfers = std::make_unique<usb_transfer_t *[]>(stream.num_urbs);
  stream.ctxs = std::make_unique<IsocCbCtx[]>(stream.num_urbs);
  stream.pending_urbs.store(stream.num_urbs, std::memory_order_relaxed);
  stream.died.store(false, std::memory_order_relaxed);
  stream.streaming.store(true, std::memory_order_release);

  for (uint8_t i = 0; i < stream.num_urbs; i++) {
    stream.ctxs[i].client = cb;
    stream.ctxs[i].stream = &stream;
    stream.xfers[i] = this->do_isoc_alloc(stream.ep_addr, device_handle, stream.mps, stream.packets_per_urb,
                                          USBHost::isoc_cb, &stream.ctxs[i]);
    if (stream.xfers[i] == nullptr) {
      ESP_LOGE(TAG, "stream_open: URB %u alloc failed", i);
      stream.streaming.store(false, std::memory_order_release);
      for (uint8_t j = 0; j < i; j++)
        this->do_isoc_free(stream.xfers[j]);
      stream.pending_urbs.store(0, std::memory_order_release);
      stream.ctxs.reset();
      stream.xfers.reset();
      return false;
    }
  }

  if (stream.is_output) {
    // Seed the initial OUT URBs with silence sized to the sample clock, so the very first
    // transfers already fit the negotiated bandwidth instead of a full mps.
    for (uint8_t i = 0; i < stream.num_urbs; i++) {
      memset(stream.xfers[i]->data_buffer, 0, static_cast<size_t>(stream.mps) * stream.packets_per_urb);
      uint32_t total_bytes = 0;
      for (uint8_t j = 0; j < stream.packets_per_urb; j++) {
        const uint32_t fill = isoc_next_packet_size(&stream);
        stream.xfers[i]->isoc_packet_desc[j].num_bytes = fill;
        total_bytes += fill;
      }
      stream.xfers[i]->num_bytes = static_cast<int>(total_bytes);
    }
  }

  for (uint8_t i = 0; i < stream.num_urbs; i++) {
    if (!this->do_isoc_submit(stream.xfers[i])) {
      ESP_LOGE(TAG, "stream_open: URB %u submit failed", i);
      // URBs 0..i-1 are already in flight and free themselves through isoc_cb. Stop the
      // resubmission first, then free the ones that never went out and take them off the
      // count relatively: a blind store would overwrite a decrement an in-flight URB has
      // already made, and the count would never reach zero.
      stream.streaming.store(false, std::memory_order_release);
      for (uint8_t j = i; j < stream.num_urbs; j++) {
        usb_host_transfer_free(stream.xfers[j]);
        stream.xfers[j] = nullptr;
      }
      const uint8_t never_submitted = stream.num_urbs - i;
      if (stream.pending_urbs.fetch_sub(never_submitted, std::memory_order_acq_rel) == never_submitted) {
        // Every submitted URB had already retired, so nothing will call finish_urb() again
        // and the buffers are ours to drop right here.
        stream.xfers.reset();
        stream.ctxs.reset();
      }
      return false;
    }
  }

  ESP_LOGD(TAG, "stream_open: ep=0x%02X mps=%u urbs=%u pkts/urb=%u", stream.ep_addr, stream.mps, stream.num_urbs,
           stream.packets_per_urb);
  return true;
}

void USBHost::stream_release(IsocStream &stream, USBClient *cb, usb_host_client_handle_t client_handle,
                             usb_device_handle_t device_handle) {
  if (!stream.owns_interface)
    return;
  // Only the owner may touch the alt-setting: a feedback stream shares its interface with
  // the data stream, and resetting it here would tear that one down.
  if (stream.alt_setting != 0) {
    auto done = [](const TransferStatus &status) {
      if (!status.success)
        ESP_LOGW(TAG, "alt-setting reset failed");
    };
    // A several-devices driver's stream: on its own device (the client has no single device then)
    if (stream.device != nullptr) {
      cb->set_interface(*stream.device, stream.interface_num, 0, done);
    } else {
      cb->set_interface(stream.interface_num, 0, done);
    }
  }
  this->do_release_interface(client_handle, device_handle, stream.interface_num);
}

bool USBHost::stream_open(IsocStream &stream, USBClient *cb, usb_host_client_handle_t client_handle,
                          usb_device_handle_t device_handle) {
  // Precondition violations report through the return value only. Firing on_stream_open()
  // here would tell the owner of an already running stream that its stream just failed.
  if (stream.ep_addr == 0 || stream.mps == 0 || stream.num_urbs == 0 || stream.packets_per_urb == 0) {
    ESP_LOGE(TAG, "stream_open: invalid parameters");
    return false;
  }
  // Opening on top of a stream that is still draining would overwrite xfers/ctxs while
  // in-flight callbacks still point into them.
  if (!stream.is_closed()) {
    ESP_LOGW(TAG, "stream_open: ep=0x%02X is not closed yet", stream.ep_addr);
    return false;
  }
  // Past this point the open has been accepted, so every exit -- here or from the
  // SET_INTERFACE completion -- calls on_stream_open() exactly once.

  if (!stream.owns_interface) {
    // Shares an already selected interface with the stream that owns it, so there is
    // nothing to claim and nothing to switch.
    const bool ok = this->stream_start_urbs(stream, cb, device_handle);
    stream.open_state.store(ok ? IsocOpenState::RUNNING : IsocOpenState::CLOSED, std::memory_order_release);
    cb->on_stream_open(stream, ok);
    return ok;
  }

  if (!this->do_claim_interface(cb, client_handle, device_handle, stream.interface_num, stream.alt_setting)) {
    cb->on_stream_open(stream, false);
    return false;
  }

  if (stream.alt_setting == 0) {
    const bool ok = this->stream_start_urbs(stream, cb, device_handle);
    // Only give the interface back when nothing is draining: if URBs did go out, the last
    // one to retire runs the same teardown and doing it twice releases it twice.
    if (!ok && stream.pending_urbs.load(std::memory_order_acquire) == 0)
      this->stream_release(stream, cb, client_handle, device_handle);
    stream.open_state.store(ok ? IsocOpenState::RUNNING : IsocOpenState::CLOSED, std::memory_order_release);
    cb->on_stream_open(stream, ok);
    return ok;
  }

  // The endpoint only exists once the device has switched to the alt-setting, and switching
  // is a control transfer. Submit it and finish the open from its completion rather than
  // blocking the caller -- every caller here is the main loop.
  stream.open_state.store(IsocOpenState::SELECTING_ALT, std::memory_order_release);
  auto selected_cb = [this, &stream, cb](const TransferStatus &status) {
    // Completion runs on the USB task; hand the rest to the main loop so the whole open
    // sequence stays on the task that started it.
    const bool selected = status.success;
    this->defer([this, &stream, cb, selected]() {
      // Read the handles here rather than snapshotting them at submit time. A full
      // control round trip fits in this window, and disconnect() runs on the main loop
      // just like this lambda, so either the device is still open and the handles are
      // valid, or it is gone and device_handle_ is null. A snapshot would turn that
      // null into a stale handle and release an interface on a device the IDF freed.
      // Not const-qualified: both are pointer typedefs, so a leading const would apply
      // to the pointer rather than the pointee and trips clang-tidy misc-misplaced-const.
      usb_host_client_handle_t client_handle = cb->handle_;
      usb_device_handle_t device_handle = stream_device_handle_(stream, cb);
      if (device_handle == nullptr) {
        // The device went away mid-switch. It took the claimed interface with it, so
        // there is nothing to release.
        stream.open_state.store(IsocOpenState::CLOSED, std::memory_order_release);
        cb->on_stream_open(stream, false);
        return;
      }
      // The owner may have closed the stream while the device was being switched. Give
      // the interface back rather than starting URBs nobody asked for any more, and
      // only then report the stream closed: this is the completion the ABORTING state
      // was waiting for.
      if (stream.open_state.load(std::memory_order_acquire) != IsocOpenState::SELECTING_ALT) {
        this->stream_release(stream, cb, client_handle, device_handle);
        stream.open_state.store(IsocOpenState::CLOSED, std::memory_order_release);
        cb->on_stream_open(stream, false);
        return;
      }
      bool ok = selected;
      if (!ok) {
        ESP_LOGE(TAG, "stream_open: SET_INTERFACE %u alt %u failed", stream.interface_num, stream.alt_setting);
      } else {
        ok = this->stream_start_urbs(stream, cb, device_handle);
      }
      if (!ok) {
        if (stream.pending_urbs.load(std::memory_order_acquire) == 0)
          this->stream_release(stream, cb, client_handle, device_handle);
        stream.open_state.store(IsocOpenState::CLOSED, std::memory_order_release);
      } else {
        stream.open_state.store(IsocOpenState::RUNNING, std::memory_order_release);
      }
      cb->on_stream_open(stream, ok);
    });
  };
  const bool submitted = stream.device != nullptr
                             ? cb->set_interface(*stream.device, stream.interface_num, stream.alt_setting, selected_cb)
                             : cb->set_interface(stream.interface_num, stream.alt_setting, selected_cb);
  if (!submitted) {
    ESP_LOGE(TAG, "stream_open: SET_INTERFACE %u alt %u not submitted", stream.interface_num, stream.alt_setting);
    this->stream_release(stream, cb, client_handle, device_handle);
    stream.open_state.store(IsocOpenState::CLOSED, std::memory_order_release);
    cb->on_stream_open(stream, false);
    return false;
  }
  return true;
}

usb_device_handle_t USBHost::stream_device_handle_(const IsocStream &stream, const USBClient *client) {
  return stream.device != nullptr ? stream.device->handle : client->device_handle_;
}

void USBHost::stream_close(IsocStream &stream) {
  // An open that has not started its URBs yet is cancelled by taking it out of
  // SELECTING_ALT: the SET_INTERFACE completion then gives the interface back instead of
  // starting a stream nobody wants. There is no URB to drain, but the interface is still
  // claimed and the completion is still queued, so the stream is not closed yet -- going
  // straight to CLOSED here would make is_closed() true and let a reopen claim an
  // interface the old open never released, with the old completion then driving the new
  // open. ABORTING keeps is_closed() false until that completion has run.
  auto selecting = IsocOpenState::SELECTING_ALT;
  if (stream.open_state.compare_exchange_strong(selecting, IsocOpenState::ABORTING, std::memory_order_acq_rel))
    return;

  if (!stream.streaming.load(std::memory_order_acquire) && !stream.xfers)
    return;

  // Signal callbacks to stop resubmitting. In-flight URBs will return, free themselves, and
  // the last one defers alt-setting reset, interface release and buffer cleanup to the main
  // loop via defer() -- no blocking needed. is_closed() reports when that has happened.
  stream.streaming.store(false, std::memory_order_release);
}

#endif  // USE_USB_ISOC_TRANSFERS

}  // namespace esphome::usb_host
#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
