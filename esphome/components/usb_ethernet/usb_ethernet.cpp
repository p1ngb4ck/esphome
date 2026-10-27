#include "usb_ethernet.h"

#ifdef USE_USB_ETHERNET

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_NETWORK_DEFAULT_ROUTE
#include "esphome/components/network/network_component.h"
#endif

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif_defaults.h"
#include "usb/usb_helpers.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::usb_ethernet {

static const char *const TAG = "usb_ethernet";

USBEthernet *global_usb_ethernet = nullptr;

static constexpr uint8_t CDC_CLASS_COMM = 0x02;
static constexpr uint8_t CDC_CLASS_DATA = 0x0A;
static constexpr uint8_t CDC_SUBCLASS_ECM = 0x06;
static constexpr uint8_t CDC_SUBCLASS_NCM = 0x0D;
static constexpr uint8_t CS_INTERFACE = 0x24;
static constexpr uint8_t CDC_FUNC_UNION = 0x06;
static constexpr uint8_t CDC_FUNC_ETHERNET = 0x0F;
static constexpr uint8_t CDC_FUNC_NCM = 0x1A;

static constexpr uint8_t REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t REQ_SET_ETHERNET_PACKET_FILTER = 0x43;
static constexpr uint8_t REQ_GET_NTB_PARAMETERS = 0x80;
static constexpr uint8_t REQ_SET_NTB_INPUT_SIZE = 0x86;
static constexpr uint16_t PACKET_FILTER = 0x0E;
static constexpr uint16_t LANG_EN_US = 0x0409;
static constexpr uint8_t NCM_CAP_PACKET_FILTER = 0x01;
static constexpr uint8_t NCM_CAP_NTB_INPUT_SIZE_8 = 0x20;

static constexpr uint8_t NOTIFY_NETWORK_CONNECTION = 0x00;
static constexpr uint8_t NOTIFY_SPEED_CHANGE = 0x2A;

static constexpr uint32_t NTH16_SIGNATURE = 0x484D434E;
static constexpr uint32_t NDP16_SIGNATURE = 0x304D434E;
static constexpr uint32_t NDP16_CRC_SIGNATURE = 0x314D434E;
static constexpr size_t NTH16_SIZE = 12;
static constexpr size_t NDP16_SIZE = 16;
static constexpr size_t MAC_STRING_SIZE = 2 + 12 * 2;
static constexpr size_t NTB_PARAMETERS_SIZE = 28;
static constexpr size_t ETH_HEADER_SIZE = 14;
static constexpr size_t ETH_FRAME_MAX = 1514;

static uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
static void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  put16(p, static_cast<uint16_t>(v));
  put16(p + 2, static_cast<uint16_t>(v >> 16));
}
static size_t round_up(size_t n, size_t unit) { return unit == 0 ? n : (n + unit - 1) / unit * unit; }
static int hex_value(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

USBEthernet::USBEthernet(uint16_t vid, uint16_t pid) : usb_host::USBClient(vid, pid) {
  this->add_class_match(CDC_CLASS_COMM, CDC_SUBCLASS_ECM);
  this->add_class_match(CDC_CLASS_COMM, CDC_SUBCLASS_NCM);
  this->add_class_match(CDC_CLASS_DATA);
  global_usb_ethernet = this;
}

void USBEthernet::setup() {
  usb_host::USBClient::setup();
  if (this->is_failed())
    return;
  this->tx_sem_ = xSemaphoreCreateBinaryStatic(&this->tx_sem_buf_);
  esp_netif_inherent_config_t base = *ESP_NETIF_BASE_DEFAULT_ETH;
  base.if_key = "USB_ETH";
  base.if_desc = "usb_eth";
  base.route_prio = 40;
  esp_netif_config_t cfg = {.base = &base, .driver = nullptr, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
  this->netif_ = esp_netif_new(&cfg);
  if (this->netif_ == nullptr) {
    ESP_LOGE(TAG, "esp_netif_new failed");
    this->mark_failed();
    return;
  }
  this->glue_.base.post_attach = USBEthernet::post_attach_;
  this->glue_.owner = this;
  esp_err_t err = esp_netif_attach(this->netif_, &this->glue_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_attach failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, USBEthernet::ip_event_handler_, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "IP event handler: %s", esp_err_to_name(err));
    this->mark_failed();
  }
}

void USBEthernet::dump_config() {
  ESP_LOGCONFIG(TAG, "USB Ethernet (CDC-ECM, CDC-NCM):");
  usb_host::USBClient::dump_config();
#ifdef USE_USB_ETHERNET_MANUAL_IP
  if (this->manual_ip_.has_value()) {
    char ip[network::IP_ADDRESS_BUFFER_SIZE];
    ESP_LOGCONFIG(TAG, "  Static IP: %s", this->manual_ip_->static_ip.str_to(ip));
  }
#endif
}

bool USBEthernet::is_connected() const {
  return this->running_.load(std::memory_order_acquire) && this->link_up_ &&
         (this->got_ip4_.load(std::memory_order_acquire) || this->got_ip6_.load(std::memory_order_acquire));
}

network::IPAddresses USBEthernet::get_ip_addresses() {
  network::IPAddresses addresses;
  if (this->netif_ == nullptr)
    return addresses;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(this->netif_, &ip) == ESP_OK)
    addresses[0] = network::IPAddress(&ip.ip);
#if USE_NETWORK_IPV6
  network::fill_ip6_addresses(this->netif_, addresses);
#endif
  return addresses;
}

usb_host::MatchTier USBEthernet::refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const {
  if (dev.config == nullptr)
    return usb_host::MatchTier::NONE;
  int offset = 0;
  const auto *next = reinterpret_cast<const usb_standard_desc_t *>(dev.config);
  while ((next = usb_parse_next_descriptor_of_type(next, dev.config->wTotalLength, USB_W_VALUE_DT_INTERFACE,
                                                   &offset)) != nullptr) {
    const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(next);
    if (intf->bInterfaceClass == CDC_CLASS_COMM &&
        (intf->bInterfaceSubClass == CDC_SUBCLASS_ECM || intf->bInterfaceSubClass == CDC_SUBCLASS_NCM))
      return tier;
  }
  return usb_host::MatchTier::NONE;
}

bool USBEthernet::parse_descriptors_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;
  this->proto_ = Proto::NONE;
  this->comm_intf_ = this->data_intf_ = 0xFF;
  this->notify_ep_ = this->in_ep_ = this->out_ep_ = 0;
  this->notify_mps_ = this->in_mps_ = this->out_mps_ = 0;
  this->mac_index_ = 0;
  this->ncm_caps_ = 0;
  this->max_segment_ = ETH_FRAME_MAX;

  bool in_comm = false;
  int offset = 0;
  const auto *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    const auto *raw = reinterpret_cast<const uint8_t *>(desc);
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_comm = this->comm_intf_ == 0xFF && intf->bAlternateSetting == 0 && intf->bInterfaceClass == CDC_CLASS_COMM &&
                (intf->bInterfaceSubClass == CDC_SUBCLASS_ECM || intf->bInterfaceSubClass == CDC_SUBCLASS_NCM) &&
                this->interface_routed_(intf->bInterfaceNumber);
      if (in_comm) {
        this->comm_intf_ = intf->bInterfaceNumber;
        this->proto_ = intf->bInterfaceSubClass == CDC_SUBCLASS_NCM ? Proto::NCM : Proto::ECM;
      } else if (this->comm_intf_ != 0xFF) {
        break;
      }
      continue;
    }
    if (!in_comm)
      continue;
    if (desc->bDescriptorType == CS_INTERFACE && desc->bLength >= 3) {
      const uint8_t subtype = raw[2];
      if (subtype == CDC_FUNC_UNION && desc->bLength >= 5) {
        this->data_intf_ = raw[4];
      } else if (subtype == CDC_FUNC_ETHERNET && desc->bLength >= 13) {
        this->mac_index_ = raw[3];
        const uint16_t segment = le16(raw + 8);
        if (segment >= ETH_HEADER_SIZE)
          this->max_segment_ = segment;
      } else if (subtype == CDC_FUNC_NCM && desc->bLength >= 6) {
        this->ncm_caps_ = raw[5];
      }
    } else if (desc->bDescriptorType == USB_W_VALUE_DT_ENDPOINT) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_INT && USB_EP_DESC_GET_EP_DIR(ep) &&
          this->notify_ep_ == 0) {
        this->notify_ep_ = ep->bEndpointAddress;
        this->notify_mps_ = USB_EP_DESC_GET_MPS(ep);
      }
    }
  }
  if (this->comm_intf_ == 0xFF || this->data_intf_ == 0xFF)
    return false;

  offset = 0;
  desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_data = false;
  uint8_t alt = 0;
  uint8_t in_ep = 0, out_ep = 0;
  uint16_t in_mps = 0, out_mps = 0;
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_data = intf->bInterfaceNumber == this->data_intf_ && intf->bInterfaceClass == CDC_CLASS_DATA;
      alt = intf->bAlternateSetting;
      in_ep = out_ep = 0;
      in_mps = out_mps = 0;
      continue;
    }
    if (!in_data || desc->bDescriptorType != USB_W_VALUE_DT_ENDPOINT)
      continue;
    const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
    if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_BULK)
      continue;
    if (USB_EP_DESC_GET_EP_DIR(ep)) {
      in_ep = ep->bEndpointAddress;
      in_mps = USB_EP_DESC_GET_MPS(ep);
    } else {
      out_ep = ep->bEndpointAddress;
      out_mps = USB_EP_DESC_GET_MPS(ep);
    }
    if (in_ep != 0 && out_ep != 0 && in_mps != 0 && out_mps != 0) {
      this->data_alt_ = alt;
      this->in_ep_ = in_ep;
      this->in_mps_ = in_mps;
      this->out_ep_ = out_ep;
      this->out_mps_ = out_mps;
      return true;
    }
  }
  return false;
}

void USBEthernet::on_connected() {
  const uint32_t generation = this->generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (!this->parse_descriptors_()) {
    ESP_LOGW(TAG, "No CDC-ECM / CDC-NCM function with a data interface on this device");
    this->report_connected_();
    return;
  }
  if (!this->claim_interface(this->comm_intf_, 0)) {
    ESP_LOGE(TAG, "Claiming interface %u failed", this->comm_intf_);
    this->report_connected_();
    return;
  }
  this->comm_claimed_ = true;
  if (!this->claim_interface(this->data_intf_, this->data_alt_)) {
    ESP_LOGE(TAG, "Claiming interface %u alt %u failed", this->data_intf_, this->data_alt_);
    this->report_connected_();
    return;
  }
  this->data_claimed_ = true;
  ESP_LOGI(TAG, "%s on interfaces %u / %u (notify 0x%02X, data 0x%02X / 0x%02X)",
           this->proto_ == Proto::NCM ? "CDC-NCM" : "CDC-ECM", this->comm_intf_, this->data_intf_, this->notify_ep_,
           this->in_ep_, this->out_ep_);
  this->next_(generation, Step::MAC);
}

void USBEthernet::control_(uint32_t generation, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                           const std::vector<uint8_t> &data, int32_t w_length) {
  const Step step = this->step_state_;
  const bool submitted = this->control_transfer(
      type, request, value, index,
      [this, generation, step](const usb_host::TransferStatus &status) {
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        const bool ok = status.success;
        if (ok && step == Step::MAC)
          this->read_mac_(status.data, status.data_len);
        if (ok && step == Step::NTB_PARAMS)
          this->read_ntb_params_(status.data, status.data_len);
        this->defer([this, generation, ok]() { this->step_(generation, ok); });
      },
      data, w_length);
  if (!submitted)
    this->defer([this, generation]() { this->step_(generation, false); });
}

void USBEthernet::next_(uint32_t generation, Step step) {
  this->step_state_ = step;
  const uint8_t ep0 = this->get_device_desc_() != nullptr ? this->get_device_desc_()->bMaxPacketSize0 : 64;
  const uint8_t class_out = usb_host::USB_DIR_OUT | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE;
  const uint8_t class_in = usb_host::USB_DIR_IN | usb_host::USB_TYPE_CLASS | usb_host::USB_RECIP_INTERFACE;
  switch (step) {
    case Step::MAC:
      this->mac_valid_ = false;
      if (this->mac_index_ == 0) {
        this->defer([this, generation]() { this->step_(generation, false); });
        return;
      }
      this->control_(generation, usb_host::USB_DIR_IN | usb_host::USB_TYPE_STANDARD | usb_host::USB_RECIP_DEVICE,
                     REQ_GET_DESCRIPTOR, static_cast<uint16_t>((USB_W_VALUE_DT_STRING << 8) | this->mac_index_),
                     LANG_EN_US, std::vector<uint8_t>(round_up(MAC_STRING_SIZE, ep0)), MAC_STRING_SIZE);
      return;
    case Step::NTB_PARAMS:
      this->control_(generation, class_in, REQ_GET_NTB_PARAMETERS, 0, this->comm_intf_,
                     std::vector<uint8_t>(round_up(NTB_PARAMETERS_SIZE, ep0)), NTB_PARAMETERS_SIZE);
      return;
    case Step::NTB_INPUT: {
      std::vector<uint8_t> size((this->ncm_caps_ & NCM_CAP_NTB_INPUT_SIZE_8) ? 8 : 4, 0);
      put32(size.data(), NTB_LIMIT);
      this->control_(generation, class_out, REQ_SET_NTB_INPUT_SIZE, 0, this->comm_intf_, size, -1);
      return;
    }
    case Step::FILTER:
      if (this->proto_ == Proto::NCM && (this->ncm_caps_ & NCM_CAP_PACKET_FILTER) == 0) {
        this->defer([this, generation]() { this->step_(generation, true); });
        return;
      }
      this->control_(generation, class_out, REQ_SET_ETHERNET_PACKET_FILTER, PACKET_FILTER, this->comm_intf_, {}, -1);
      return;
    case Step::ALT: {
      const bool submitted =
          this->set_interface(this->data_intf_, this->data_alt_, [this, generation](const usb_host::TransferStatus &s) {
            if (generation != this->generation_.load(std::memory_order_acquire))
              return;
            const bool ok = s.success;
            this->defer([this, generation, ok]() { this->step_(generation, ok); });
          });
      if (!submitted)
        this->defer([this, generation]() { this->step_(generation, false); });
      return;
    }
    default:
      return;
  }
}

void USBEthernet::step_(uint32_t generation, bool ok) {
  if (generation != this->generation_.load(std::memory_order_acquire))
    return;
  switch (this->step_state_) {
    case Step::MAC:
      if (!ok || !this->mac_valid_)
        this->fallback_mac_();
      this->next_(generation, this->proto_ == Proto::NCM ? Step::NTB_PARAMS : Step::FILTER);
      return;
    case Step::NTB_PARAMS:
      if (!ok || this->ntb_in_max_ == 0 || this->ntb_out_max_ == 0) {
        ESP_LOGE(TAG, "GET_NTB_PARAMETERS failed");
        this->step_state_ = Step::IDLE;
        this->report_connected_();
        return;
      }
      if (this->ntb_in_max_ <= NTB_LIMIT) {
        this->rx_size_ = round_up(this->ntb_in_max_, this->in_mps_);
        this->next_(generation, Step::FILTER);
      } else {
        this->next_(generation, Step::NTB_INPUT);
      }
      return;
    case Step::NTB_INPUT:
      if (ok) {
        this->rx_size_ = round_up(NTB_LIMIT, this->in_mps_);
      } else if (this->ntb_in_max_ <= NTB_MAX) {
        this->rx_size_ = round_up(this->ntb_in_max_, this->in_mps_);
      } else {
        ESP_LOGE(TAG, "The device sends NTBs of %" PRIu32 " bytes and refuses a smaller size", this->ntb_in_max_);
        this->step_state_ = Step::IDLE;
        this->report_connected_();
        return;
      }
      this->next_(generation, Step::FILTER);
      return;
    case Step::FILTER:
      this->next_(generation, Step::ALT);
      return;
    case Step::ALT:
      if (!ok || !this->start_data_()) {
        ESP_LOGE(TAG, "Starting the data interface failed");
        this->step_state_ = Step::IDLE;
        this->report_connected_();
        return;
      }
      this->step_state_ = Step::RUNNING;
      this->report_connected_();
      if (this->notify_ep_ == 0) {
        this->link_target_.store(true, std::memory_order_release);
        this->apply_link_(generation);
      }
      return;
    default:
      return;
  }
}

void USBEthernet::read_mac_(const uint8_t *data, size_t len) {
  if (len < MAC_STRING_SIZE || data[1] != USB_W_VALUE_DT_STRING)
    return;
  uint8_t mac[6];
  for (size_t i = 0; i < 6; i++) {
    const int hi = hex_value(data[2 + i * 4]);
    const int lo = hex_value(data[2 + i * 4 + 2]);
    if (hi < 0 || lo < 0)
      return;
    mac[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  memcpy(this->mac_, mac, sizeof(mac));
  this->mac_valid_ = true;
}

void USBEthernet::read_ntb_params_(const uint8_t *data, size_t len) {
  if (len < NTB_PARAMETERS_SIZE)
    return;
  this->ntb_in_max_ = le32(data + 4);
  this->ntb_out_max_ = le32(data + 16);
  this->ntb_out_divisor_ = le16(data + 20);
  this->ntb_out_remainder_ = le16(data + 22);
  this->ntb_out_alignment_ = le16(data + 24);
}

void USBEthernet::fallback_mac_() {
  uint8_t base[6];
  esp_read_mac(base, ESP_MAC_BASE);
  esp_derive_local_mac(this->mac_, base);
  ESP_LOGW(TAG, "No MAC address from the device, using %02X:%02X:%02X:%02X:%02X:%02X", this->mac_[0], this->mac_[1],
           this->mac_[2], this->mac_[3], this->mac_[4], this->mac_[5]);
}

bool USBEthernet::alloc_(DataXfer &x, size_t size, uint8_t ep, usb_transfer_cb_t cb) {
  if (x.state.load(std::memory_order_acquire) != XFER_FREE)
    return false;
  if (usb_host_transfer_alloc(size, 0, &x.xfer) != ESP_OK) {
    x.xfer = nullptr;
    return false;
  }
  x.owner = this;
  x.xfer->device_handle = this->device_handle_;
  x.xfer->bEndpointAddress = ep;
  x.xfer->callback = cb;
  x.xfer->context = &x;
  x.xfer->timeout_ms = 0;
  x.state.store(XFER_IDLE, std::memory_order_release);
  return true;
}

bool USBEthernet::submit_(DataXfer &x, size_t len) {
  x.xfer->num_bytes = static_cast<int>(len);
  return usb_host_transfer_submit(x.xfer) == ESP_OK;
}

void USBEthernet::retire_(DataXfer &x) {
  if (x.xfer != nullptr)
    usb_host_transfer_free(x.xfer);
  x.xfer = nullptr;
  x.state.store(XFER_FREE, std::memory_order_release);
}

bool USBEthernet::start_data_() {
  if (this->proto_ == Proto::ECM) {
    this->rx_size_ = round_up(this->max_segment_, this->in_mps_);
    this->tx_size_ = round_up(this->max_segment_, this->out_mps_);
  } else {
    this->tx_size_ = std::min<size_t>(this->ntb_out_max_, NTB_LIMIT);
  }
  const size_t largest = std::max(this->rx_size_, this->tx_size_);
#ifdef USE_ESP32_VARIANT_ESP32P4
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < largest + 64) {
    ESP_LOGE(TAG, "No %u bytes of PSRAM for a data transfer", static_cast<unsigned>(largest));
    return false;
  }
#endif
  bool ok = true;
  for (auto &x : this->rx_)
    ok = ok && this->alloc_(x, this->rx_size_, this->in_ep_, USBEthernet::rx_cb_);
  for (auto &x : this->tx_)
    ok = ok && this->alloc_(x, this->tx_size_, this->out_ep_, USBEthernet::tx_cb_);
  if (ok && this->notify_ep_ != 0)
    ok = this->alloc_(this->notify_, this->notify_mps_, this->notify_ep_, USBEthernet::notify_cb_);
  if (!ok) {
    ESP_LOGE(TAG, "Failed to allocate the data transfers");
    this->stop_data_();
    return false;
  }
  this->running_.store(true, std::memory_order_release);
  for (auto &x : this->rx_) {
    x.state.store(XFER_BUSY, std::memory_order_release);
    if (!this->submit_(x, this->rx_size_))
      ok = false;
  }
  if (this->notify_ep_ != 0) {
    this->notify_.state.store(XFER_BUSY, std::memory_order_release);
    if (!this->submit_(this->notify_, this->notify_mps_))
      ok = false;
  }
  if (!ok) {
    this->stop_data_();
    return false;
  }
  esp_netif_set_mac(this->netif_, this->mac_);
  esp_netif_action_start(this->netif_, nullptr, 0, nullptr);
  this->started_ = true;
  ESP_LOGI(TAG, "MAC %02X:%02X:%02X:%02X:%02X:%02X, transfers %u / %u bytes", this->mac_[0], this->mac_[1],
           this->mac_[2], this->mac_[3], this->mac_[4], this->mac_[5], static_cast<unsigned>(this->rx_size_),
           static_cast<unsigned>(this->tx_size_));
  return true;
}

void USBEthernet::stop_data_() {
  this->running_.store(false, std::memory_order_release);
  for (uint8_t ep : {this->in_ep_, this->out_ep_, this->notify_ep_}) {
    if (ep != 0 && this->device_handle_ != nullptr && usb_host_endpoint_halt(this->device_handle_, ep) == ESP_OK)
      usb_host_endpoint_flush(this->device_handle_, ep);
  }
  auto sweep = [this](DataXfer &x) {
    uint8_t expected = XFER_IDLE;
    if (x.state.compare_exchange_strong(expected, XFER_BUSY, std::memory_order_acq_rel))
      this->retire_(x);
  };
  for (auto &x : this->rx_)
    sweep(x);
  for (auto &x : this->tx_)
    sweep(x);
  sweep(this->notify_);
}

void USBEthernet::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->stop_data_();
  if (this->link_up_)
    this->link_down_actions_();
  this->link_up_ = false;
  this->link_target_.store(false, std::memory_order_release);
  if (this->started_)
    esp_netif_action_stop(this->netif_, nullptr, 0, nullptr);
  this->started_ = false;
  if (this->data_claimed_)
    this->release_interface(this->data_intf_);
  if (this->comm_claimed_)
    this->release_interface(this->comm_intf_);
  if (this->data_claimed_ || this->comm_claimed_)
    ESP_LOGI(TAG, "Adapter removed");
  this->data_claimed_ = this->comm_claimed_ = false;
  this->step_state_ = Step::IDLE;
  this->proto_ = Proto::NONE;
  const uint32_t dropped = this->rx_dropped_.exchange(0, std::memory_order_acq_rel);
  if (dropped != 0)
    ESP_LOGW(TAG, "%" PRIu32 " received frames dropped (no PSRAM)", dropped);
  usb_host::USBClient::on_disconnected();
}

void USBEthernet::apply_link_(uint32_t generation) {
  if (generation != this->generation_.load(std::memory_order_acquire) || !this->started_)
    return;
  const bool target = this->link_target_.load(std::memory_order_acquire);
  if (target == this->link_up_)
    return;
  this->link_up_ = target;
  if (target) {
    this->link_up_actions_();
  } else {
    this->link_down_actions_();
  }
}

void USBEthernet::link_up_actions_() {
  const uint32_t speed = this->link_speed_.load(std::memory_order_acquire);
  if (speed != 0) {
    ESP_LOGI(TAG, "Link up, %" PRIu32 " Mbit/s", speed / 1000000);
  } else {
    ESP_LOGI(TAG, "Link up");
  }
  esp_netif_set_hostname(this->netif_, App.get_name().c_str());
#ifdef USE_USB_ETHERNET_MANUAL_IP
  if (this->manual_ip_.has_value()) {
    esp_netif_dhcpc_stop(this->netif_);
    esp_netif_ip_info_t info;
    info.ip = this->manual_ip_->static_ip;
    info.gw = this->manual_ip_->gateway;
    info.netmask = this->manual_ip_->subnet;
    esp_netif_set_ip_info(this->netif_, &info);
    esp_netif_dns_info_t dns{};
    if (this->manual_ip_->dns1.is_set()) {
      dns.ip = this->manual_ip_->dns1;
      esp_netif_set_dns_info(this->netif_, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (this->manual_ip_->dns2.is_set()) {
      dns.ip = this->manual_ip_->dns2;
      esp_netif_set_dns_info(this->netif_, ESP_NETIF_DNS_BACKUP, &dns);
    }
  }
#endif
  esp_netif_action_connected(this->netif_, nullptr, 0, nullptr);
#ifdef USE_USB_ETHERNET_MANUAL_IP
  if (!this->manual_ip_.has_value())
#endif
  {
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    if (esp_netif_dhcpc_get_status(this->netif_, &status) == ESP_OK && status != ESP_NETIF_DHCP_STARTED)
      esp_netif_dhcpc_start(this->netif_);
  }
#ifdef USE_NETWORK_DEFAULT_ROUTE
  network::restore_default_dns(this->netif_);
#endif
#if USE_NETWORK_IPV6
  esp_netif_create_ip6_linklocal(this->netif_);
#endif
}

void USBEthernet::link_down_actions_() {
  ESP_LOGI(TAG, "Link down");
  this->got_ip4_.store(false, std::memory_order_release);
  this->got_ip6_.store(false, std::memory_order_release);
  esp_netif_action_disconnected(this->netif_, nullptr, 0, nullptr);
}

void USBEthernet::rx_cb_(usb_transfer_t *xfer) {
  auto *x = static_cast<DataXfer *>(xfer->context);
  USBEthernet *self = x->owner;
  if (!self->running_.load(std::memory_order_acquire) || xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    const int status = xfer->status;
    const bool stopped = self->running_.load(std::memory_order_acquire) && status != USB_TRANSFER_STATUS_NO_DEVICE &&
                         status != USB_TRANSFER_STATUS_CANCELED;
    self->retire_(*x);
    if (stopped)
      self->defer([status]() { ESP_LOGW(TAG, "A receive transfer stopped (status %d)", status); });
    return;
  }
  if (xfer->actual_num_bytes > 0)
    self->handle_rx_(xfer->data_buffer, static_cast<size_t>(xfer->actual_num_bytes));
  if (!self->submit_(*x, self->rx_size_))
    self->retire_(*x);
}

void USBEthernet::notify_cb_(usb_transfer_t *xfer) {
  auto *x = static_cast<DataXfer *>(xfer->context);
  USBEthernet *self = x->owner;
  if (!self->running_.load(std::memory_order_acquire) || xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    self->retire_(*x);
    return;
  }
  if (xfer->actual_num_bytes > 0)
    self->handle_notification_(xfer->data_buffer, static_cast<size_t>(xfer->actual_num_bytes));
  if (!self->submit_(*x, self->notify_mps_))
    self->retire_(*x);
}

void USBEthernet::release_slot_(DataXfer &x) {
  x.state.store(XFER_IDLE, std::memory_order_release);
  if (!this->running_.load(std::memory_order_acquire)) {
    uint8_t expected = XFER_IDLE;
    if (x.state.compare_exchange_strong(expected, XFER_BUSY, std::memory_order_acq_rel))
      this->retire_(x);
  }
}

void USBEthernet::tx_cb_(usb_transfer_t *xfer) {
  auto *x = static_cast<DataXfer *>(xfer->context);
  USBEthernet *self = x->owner;
  if (xfer->status == USB_TRANSFER_STATUS_NO_DEVICE || xfer->status == USB_TRANSFER_STATUS_CANCELED) {
    self->retire_(*x);
    return;
  }
  self->release_slot_(*x);
  xSemaphoreGive(self->tx_sem_);
}

void USBEthernet::handle_rx_(const uint8_t *data, size_t len) {
  if (this->proto_ == Proto::NCM) {
    this->handle_ntb_(data, len);
  } else {
    this->deliver_(data, len);
  }
}

void USBEthernet::handle_ntb_(const uint8_t *data, size_t len) {
  if (len < NTH16_SIZE || le32(data) != NTH16_SIGNATURE)
    return;
  size_t block = le16(data + 8);
  if (block == 0 || block > len)
    block = len;
  size_t ndp = le16(data + 10);
  for (uint8_t chain = 0; ndp != 0 && chain < 8; chain++) {
    if (ndp + NDP16_SIZE > block)
      return;
    const uint32_t signature = le32(data + ndp);
    const bool crc = signature == NDP16_CRC_SIGNATURE;
    if (signature != NDP16_SIGNATURE && !crc)
      return;
    const size_t ndp_len = le16(data + ndp + 4);
    if (ndp_len < NDP16_SIZE || ndp + ndp_len > block)
      return;
    for (size_t entry = ndp + 8; entry + 4 <= ndp + ndp_len; entry += 4) {
      const size_t index = le16(data + entry);
      size_t length = le16(data + entry + 2);
      if (index == 0 || length == 0)
        break;
      if (crc) {
        if (length < 4)
          continue;
        length -= 4;
      }
      if (index + length > block)
        continue;
      this->deliver_(data + index, length);
    }
    ndp = le16(data + ndp + 6);
  }
}

void USBEthernet::deliver_(const uint8_t *data, size_t len) {
  if (len < ETH_HEADER_SIZE)
    return;
  auto *buf = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    this->rx_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  memcpy(buf, data, len);
  esp_netif_receive(this->netif_, buf, len, nullptr);
}

void USBEthernet::handle_notification_(const uint8_t *data, size_t len) {
  if (len < 8)
    return;
  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  if (data[1] == NOTIFY_NETWORK_CONNECTION) {
    this->link_target_.store(le16(data + 2) != 0, std::memory_order_release);
    this->defer([this, generation]() { this->apply_link_(generation); });
  } else if (data[1] == NOTIFY_SPEED_CHANGE && len >= 16) {
    this->link_speed_.store(le32(data + 8), std::memory_order_release);
  }
}

esp_err_t USBEthernet::post_attach_(esp_netif_t *netif, esp_netif_iodriver_handle h) {
  auto *glue = static_cast<NetifGlue *>(h);
  glue->base.netif = netif;
  esp_netif_driver_ifconfig_t driver{};
  driver.handle = glue->owner;
  driver.transmit = USBEthernet::transmit_;
  driver.driver_free_rx_buffer = USBEthernet::free_rx_buffer_;
  return esp_netif_set_driver_config(netif, &driver);
}

esp_err_t USBEthernet::transmit_(void *h, void *buffer, size_t len) {
  return static_cast<USBEthernet *>(h)->send_frame_(static_cast<const uint8_t *>(buffer), len);
}

void USBEthernet::free_rx_buffer_(void *h, void *buffer) { heap_caps_free(buffer); }

esp_err_t USBEthernet::send_frame_(const uint8_t *frame, size_t len) {
  if (!this->running_.load(std::memory_order_acquire))
    return ESP_FAIL;
  DataXfer *slot = nullptr;
  for (uint8_t attempt = 0; attempt < 2 && slot == nullptr; attempt++) {
    for (auto &x : this->tx_) {
      uint8_t expected = XFER_IDLE;
      if (x.state.compare_exchange_strong(expected, XFER_BUSY, std::memory_order_acq_rel)) {
        slot = &x;
        break;
      }
    }
    if (slot == nullptr && attempt == 0 && xSemaphoreTake(this->tx_sem_, pdMS_TO_TICKS(TX_WAIT_MS)) != pdTRUE)
      return ESP_ERR_TIMEOUT;
  }
  if (slot == nullptr)
    return ESP_ERR_TIMEOUT;
  if (!this->running_.load(std::memory_order_acquire)) {
    this->retire_(*slot);
    return ESP_FAIL;
  }
  uint8_t *out = slot->xfer->data_buffer;
  size_t total = len;
  if (this->proto_ == Proto::NCM) {
    const size_t alignment = std::max<size_t>(this->ntb_out_alignment_, 4);
    const size_t ndp = round_up(NTH16_SIZE, alignment);
    const size_t divisor = std::max<size_t>(this->ntb_out_divisor_, 1);
    const size_t remainder = this->ntb_out_remainder_ % divisor;
    size_t datagram = ndp + NDP16_SIZE;
    datagram += (remainder + divisor - datagram % divisor) % divisor;
    total = datagram + len;
    if (total > this->tx_size_) {
      this->release_slot_(*slot);
      return ESP_ERR_INVALID_SIZE;
    }
    memset(out, 0, datagram);
    put32(out, NTH16_SIGNATURE);
    put16(out + 4, NTH16_SIZE);
    put16(out + 6, this->ntb_sequence_++);
    put16(out + 8, static_cast<uint16_t>(total));
    put16(out + 10, static_cast<uint16_t>(ndp));
    put32(out + ndp, NDP16_SIGNATURE);
    put16(out + ndp + 4, NDP16_SIZE);
    put16(out + ndp + 8, static_cast<uint16_t>(datagram));
    put16(out + ndp + 10, static_cast<uint16_t>(len));
    memcpy(out + datagram, frame, len);
  } else {
    if (len > this->tx_size_) {
      this->release_slot_(*slot);
      return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, frame, len);
  }
  const bool zlp = total % this->out_mps_ == 0 && !(this->proto_ == Proto::NCM && total == this->ntb_out_max_);
  slot->xfer->flags = zlp ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
  if (!this->submit_(*slot, total)) {
    this->release_slot_(*slot);
    return ESP_FAIL;
  }
  return ESP_OK;
}

void USBEthernet::ip_event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data) {
  auto *self = static_cast<USBEthernet *>(arg);
  if (id == IP_EVENT_ETH_GOT_IP) {
    const auto *event = static_cast<const ip_event_got_ip_t *>(data);
    if (event->esp_netif != self->netif_)
      return;
    self->got_ip4_.store(true, std::memory_order_release);
    const esp_ip4_addr_t ip = event->ip_info.ip;
    self->defer([ip]() { ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ip)); });
  } else if (id == IP_EVENT_ETH_LOST_IP) {
    const auto *event = static_cast<const ip_event_got_ip_t *>(data);
    if (event->esp_netif != self->netif_)
      return;
    self->got_ip4_.store(false, std::memory_order_release);
#if USE_NETWORK_IPV6
  } else if (id == IP_EVENT_GOT_IP6) {
    const auto *event = static_cast<const ip_event_got_ip6_t *>(data);
    if (event->esp_netif != self->netif_)
      return;
    self->got_ip6_.store(true, std::memory_order_release);
#endif
  }
}

}

#endif
