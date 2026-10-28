#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_bluetooth.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "usb/usb_helpers.h"

#ifdef USE_USB_BLUETOOTH_BLUEDROID
#include "esp_bluedroid_hci.h"
#include "esp_bt_main.h"
#ifdef USE_BT_PERSISTENCE
#include "esphome/components/bt_persistence/bt_persistence.h"
#endif
#endif
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
#include "hci_split.h"
#include "esphome/components/esp32_ble/ble.h"
#endif
#ifdef USE_USB_BLUETOOTH_C6_OFFLOAD
#include "c6_offload.h"
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
// Realtek vendor commands (btrtl.c).
static constexpr uint16_t OP_RTL_DOWNLOAD = 0xFC20;
static constexpr uint16_t OP_RTL_READ_ROM_VERSION = 0xFC6D;
static constexpr size_t RTL_FRAGMENT = 252;

static constexpr uint16_t MANUFACTURER_REALTEK = 93;

#ifdef USE_USB_BLUETOOTH_H4
// H4 packet types, the byte Bluedroid's transport puts in front of every packet.
static constexpr uint8_t H4_COMMAND = 0x01;
static constexpr uint8_t H4_ACL = 0x02;
static constexpr uint8_t H4_SCO = 0x03;
static constexpr uint8_t H4_EVENT = 0x04;
// Transfer slots a send leaves free (the two readers hold theirs all the time).
static constexpr size_t SPARE_SLOTS = 1;
#endif

#ifdef USE_USB_BLUETOOTH_BLUEDROID

// The transport operations are plain C function pointers without a user argument.
static USBBluetooth *g_instance = nullptr;
static const esp_bluedroid_hci_driver_callbacks_t *g_host_cb = nullptr;
static bool g_stack_started = false;
static std::atomic<bool> g_stack_starting{false};

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
  g_stack_starting.store(false, std::memory_order_release);
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

// The SCO interface (isochronous voice endpoints) is never claimed: SCO is not carried over this transport.
// Its endpoints stay out of the host channel budget; once SCO is carried, ignoring it becomes an option.
uint32_t USBBluetooth::skipped_endpoints_(const usb_device_desc_t * /*desc*/, const usb_config_desc_t *config,
                                          const usb_intf_desc_t *alt) const {
  uint32_t all = 0;
  bool isoc = false;
  usb_host::for_each_alt_endpoint(config, alt, [&](const usb_ep_desc_t *ep) {
    all |= usb_host::endpoint_bit(ep);
    if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_ISOC)
      isoc = true;
  });
  return isoc ? all : 0;
}

bool USBBluetooth::find_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;
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
      // FF/01/01 only where routing handed it over: a vendor match or a dongle named by
      // vid/pid. The vendor class says nothing about Bluetooth on its own.
      if (intf->bInterfaceClass == HCI_CLASS ||
          (intf->bInterfaceClass == 0xFF && this->interface_routed_(intf->bInterfaceNumber))) {
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
  this->probe_start_();
}

void USBBluetooth::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->link_up_.store(false, std::memory_order_release);
  this->stack_attached_.store(false, std::memory_order_release);
  this->cancel_timeout("hci");
  this->pending_opcode_ = 0;
  this->pending_ = nullptr;
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  // BR/EDR commands are answered "unknown" from here on rather than left waiting.
  hci_split::set_br(nullptr);
#endif
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
#ifdef USE_USB_BLUETOOTH_H4
  if (this->send_blocked_.exchange(false, std::memory_order_acq_rel)) {
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
    hci_split::br_send_available();
#elif defined(USE_USB_BLUETOOTH_C6_OFFLOAD)
    c6_offload::send_available(true);
#else
    if (g_host_cb != nullptr && g_host_cb->notify_host_send_available != nullptr)
      g_host_cb->notify_host_send_available();
#endif
  }
#endif
}

#ifdef USE_USB_BLUETOOTH_H4
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

// USB task.
void USBBluetooth::to_stack_(uint8_t *h4, size_t len) {
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  hci_split::br_recv(h4, len);
#elif defined(USE_USB_BLUETOOTH_C6_OFFLOAD)
  c6_offload::up(h4, len);
#else
  if (g_host_cb != nullptr && g_host_cb->notify_host_recv != nullptr)
    g_host_cb->notify_host_recv(h4, static_cast<uint16_t>(len));
#endif
}

#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
// With usb_bluetooth in the config, esp32_ble waits for the dongle (codegen): Bluedroid starts once, on both
// controllers. A dongle plugged after that (or replugged) restarts it: it reads its controllers only at start.
void USBBluetooth::start_split_() {
  hci_split::set_br(this);
  this->stack_attached_.store(true, std::memory_order_release);
  this->set_interval("acl_check", 10000, [this]() {
    hci_split::report_acl();
    if (this->send_blocked_.load(std::memory_order_acquire))
      ESP_LOGW(TAG, "Bluedroid waits for a free transfer: %u of %u in use",
               static_cast<unsigned>(__builtin_popcount(this->get_trq_in_use())),
               static_cast<unsigned>(usb_host::MAX_REQUESTS));
  });
  if (esp32_ble::global_ble == nullptr)
    return;
  esp32_ble::global_ble->usb_bluetooth_ready();
  if (esp32_ble::global_ble->is_active()) {
    // Plugged after Bluedroid came up (or replugged): it reads its controllers only at its start
    ESP_LOGI(TAG, "Restarting Bluedroid for the dongle (BR/EDR)");
    esp32_ble::global_ble->restart();
  } else {
    esp32_ble::global_ble->enable();  // the one start, on both controllers, once WiFi's start-up is done
  }
}
#endif

#endif

#ifdef USE_USB_BLUETOOTH_BLUEDROID
void USBBluetooth::start_bluedroid_() {
#ifdef USE_BT_PERSISTENCE
  // Bluedroid reads its pairing data once, when it starts.
  if (bt_persistence::global_bt_persistence != nullptr && !bt_persistence::global_bt_persistence->is_ready()) {
    if (!this->waiting_for_store_.exchange(true)) {
      this->defer([this]() {
        bt_persistence::global_bt_persistence->when_ready([this]() {
          this->waiting_for_store_ = false;
          this->start_bluedroid_();
        });
      });
    }
    return;
  }
#endif
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
  g_stack_starting.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(bluedroid_start_task, "usb_bt_start", 4096, nullptr, 5, nullptr, LOOP_TASK_CORE) !=
      pdPASS) {
    g_stack_starting.store(false, std::memory_order_release);
    ESP_LOGE(TAG, "Starting Bluedroid failed: no task");
  }
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
#ifdef USE_USB_BLUETOOTH_H4
          if (this->stack_attached_.load(std::memory_order_acquire)) {
            this->event_[0] = H4_EVENT;
            this->to_stack_(this->event_, len + 1);
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
        // A byte stream, as btusb_recv_bulk() takes it: one USB packet may end an ACL packet and
        // begin the next.
        uint8_t *acl = this->acl_ + 1;
        const uint8_t *in = status.data;
        size_t count = status.data_len;
        while (count > 0) {
          const size_t want = this->acl_len_ < 4 ? 4 - this->acl_len_ : 4u + (acl[2] | (acl[3] << 8)) - this->acl_len_;
          const size_t take = std::min(want, count);
          memcpy(acl + this->acl_len_, in, take);
          this->acl_len_ += take;
          in += take;
          count -= take;
          if (this->acl_len_ < 4)
            continue;
          const size_t total = 4u + (acl[2] | (acl[3] << 8));
          if (total > ACL_MAX) {
            // Larger than any controller sends: the stream is lost until the next USB packet.
            this->acl_len_ = 0;
            break;
          }
          if (this->acl_len_ < total)
            continue;
#ifdef USE_USB_BLUETOOTH_H4
          if (this->stack_attached_.load(std::memory_order_acquire)) {
            this->acl_[0] = H4_ACL;
            this->to_stack_(this->acl_, total + 1);
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

void USBBluetooth::ask_(uint16_t opcode, std::vector<uint8_t> params,
                        std::function<void(const uint8_t *ret, size_t n)> &&then) {
  std::vector<uint8_t> command(3 + params.size());
  command[0] = static_cast<uint8_t>(opcode & 0xFF);
  command[1] = static_cast<uint8_t>(opcode >> 8);
  command[2] = static_cast<uint8_t>(params.size());
  if (!params.empty())
    memcpy(command.data() + 3, params.data(), params.size());
  this->pending_opcode_ = opcode;
  this->pending_ = std::move(then);
  if (!this->send_command(command.data(), command.size())) {
    ESP_LOGE(TAG, "HCI command 0x%04X not submitted", opcode);
    this->probe_done_(false);
    return;
  }
  this->set_timeout("hci", COMMAND_TIMEOUT_MS, [this]() {
    ESP_LOGE(TAG, "No answer to HCI command 0x%04X", this->pending_opcode_);
    this->probe_done_(false);
  });
}

void USBBluetooth::handle_event_(const std::vector<uint8_t> &event) {
  // Command Complete: code, length, Num_HCI_Command_Packets, opcode, status, return parameters.
  if (event[0] != HCI_EVENT_COMMAND_COMPLETE || event.size() < 6) {
    ESP_LOGV(TAG, "Event 0x%02X, %u bytes", event[0], static_cast<unsigned>(event.size()));
    return;
  }
  const uint16_t opcode = static_cast<uint16_t>(event[3] | (event[4] << 8));
  if (opcode == 0 || opcode != this->pending_opcode_)
    return;
  this->cancel_timeout("hci");
  this->pending_opcode_ = 0;
  if (event[5] != 0) {
    ESP_LOGE(TAG, "HCI command 0x%04X failed, status 0x%02X", opcode, event[5]);
    this->probe_done_(false);
    return;
  }
  auto then = std::move(this->pending_);
  this->pending_ = nullptr;
  if (then)
    then(event.data() + 6, event.size() - 6);
}

void USBBluetooth::probe_start_() {
  this->ask_(OP_RESET, {}, [this](const uint8_t *, size_t) {
    this->ask_(OP_READ_LOCAL_VERSION, {}, [this](const uint8_t *p, size_t n) {
      if (n < 8) {
        this->probe_done_(false);
        return;
      }
      const uint16_t hci_rev = static_cast<uint16_t>(p[1] | (p[2] << 8));
      this->manufacturer_ = static_cast<uint16_t>(p[4] | (p[5] << 8));
      this->lmp_subver_ = static_cast<uint16_t>(p[6] | (p[7] << 8));
      ESP_LOGI(TAG, "HCI %u (Bluetooth %s), revision 0x%04X, LMP subversion 0x%04X, manufacturer %u", p[0],
               core_version(p[0]), hci_rev, this->lmp_subver_, this->manufacturer_);
      if (this->manufacturer_ == MANUFACTURER_REALTEK) {
        this->realtek_();
      } else {
        this->probe_identity_();
      }
    });
  });
}

void USBBluetooth::probe_identity_() {
  this->ask_(OP_READ_BD_ADDR, {}, [this](const uint8_t *p, size_t n) {
    if (n >= 6)
      ESP_LOGI(TAG, "Address %02X:%02X:%02X:%02X:%02X:%02X", p[5], p[4], p[3], p[2], p[1], p[0]);
    this->ask_(OP_READ_LOCAL_NAME, {}, [this](const uint8_t *name, size_t size) {
      const size_t len = strnlen(reinterpret_cast<const char *>(name), size);
      ESP_LOGI(TAG, "Name '%.*s'", static_cast<int>(len), reinterpret_cast<const char *>(name));
      this->probe_done_(true);
    });
  });
}

// A Realtek controller still on its ROM reports the lmp_subver of its chip; a patched one
// reports its firmware's. Only the former gets a firmware (btrtl.c does the same by its table).
void USBBluetooth::realtek_() {
  const RealtekImage *for_chip = nullptr;
  for (const auto &image : this->realtek_images_) {
    if (image.lmp_subver == this->lmp_subver_)
      for_chip = &image;
  }
  if (for_chip == nullptr) {
    if (this->realtek_images_.empty())
      ESP_LOGW(TAG, "Realtek controller without a firmware configured: if it is on its ROM it will not "
                    "reach anything on the air");
    this->probe_identity_();
    return;
  }
  this->ask_(OP_RTL_READ_ROM_VERSION, {}, [this](const uint8_t *p, size_t n) {
    if (n < 1) {
      this->probe_done_(false);
      return;
    }
    const uint8_t rom = p[0];
    this->download_ = nullptr;
    for (const auto &image : this->realtek_images_) {
      if (image.lmp_subver == this->lmp_subver_ && image.rom_version == rom)
        this->download_ = &image;
    }
    if (this->download_ == nullptr) {
      ESP_LOGE(TAG, "The firmware has no patch for ROM version %u", rom);
      this->probe_identity_();
      return;
    }
    ESP_LOGI(TAG, "Realtek ROM version %u: downloading %u bytes", rom, static_cast<unsigned>(this->download_->length));
    this->download_fragment_(0);
  });
}

// Fragments of RTL_FRAGMENT bytes, the last one flagged 0x80 and carrying the remainder;
// indexes run 0..0x7F, then wrap to 1 (rtl_download_firmware() in btrtl.c).
void USBBluetooth::download_fragment_(uint32_t i) {
  const uint32_t count = this->download_->length / RTL_FRAGMENT + 1;
  uint8_t index = i <= 0x7F ? static_cast<uint8_t>(i) : static_cast<uint8_t>((i - 0x80) % 0x7F + 1);
  size_t take = RTL_FRAGMENT;
  if (i + 1 == count) {
    index |= 0x80;
    take = this->download_->length % RTL_FRAGMENT;
  }
  std::vector<uint8_t> params(1 + take);
  params[0] = index;
  if (take != 0)
    memcpy(params.data() + 1, this->download_->data + i * RTL_FRAGMENT, take);
  this->ask_(OP_RTL_DOWNLOAD, std::move(params), [this, i, count](const uint8_t *, size_t) {
    if (i + 1 < count) {
      this->download_fragment_(i + 1);
      return;
    }
    this->ask_(OP_READ_LOCAL_VERSION, {}, [this](const uint8_t *p, size_t n) {
      if (n >= 8)
        ESP_LOGI(TAG, "Firmware loaded: revision 0x%04X, LMP subversion 0x%04X",
                 static_cast<unsigned>(p[1] | (p[2] << 8)), static_cast<unsigned>(p[6] | (p[7] << 8)));
      this->probe_identity_();
    });
  });
}

bool USBBluetooth::starting_() const {
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  return this->stack_attached_.load(std::memory_order_acquire) && esp32_ble::global_ble != nullptr &&
         !esp32_ble::global_ble->is_active() && !esp32_ble::global_ble->is_failed();
#elif defined(USE_USB_BLUETOOTH_BLUEDROID)
#ifdef USE_BT_PERSISTENCE
  if (this->waiting_for_store_.load(std::memory_order_acquire))
    return true;
#endif
  return g_stack_starting.load(std::memory_order_acquire);
#else
  return false;
#endif
}

void USBBluetooth::probe_done_(bool ok) {
  this->pending_opcode_ = 0;
  this->pending_ = nullptr;
  this->report_connected_();
#ifdef USE_USB_BLUETOOTH_HCI_SPLIT
  if (ok)
    this->start_split_();
#elif defined(USE_USB_BLUETOOTH_BLUEDROID)
  if (ok)
    this->start_bluedroid_();
#elif defined(USE_USB_BLUETOOTH_C6_OFFLOAD)
  if (ok && c6_offload::start(this))
    this->stack_attached_.store(true, std::memory_order_release);
#endif
}

}  // namespace esphome::usb_bluetooth

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
