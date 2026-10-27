#pragma once

#include "esphome/core/defines.h"

#ifdef USE_USB_ETHERNET

#include "esphome/components/network/ip_address.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/optional.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::usb_ethernet {

struct ManualIP {
  network::IPAddress static_ip;
  network::IPAddress gateway;
  network::IPAddress subnet;
  network::IPAddress dns1;
  network::IPAddress dns2;
};

class USBEthernet;

struct DataXfer {
  usb_transfer_t *xfer{nullptr};
  std::atomic<uint8_t> state{0};
  USBEthernet *owner{nullptr};
};

struct NetifGlue {
  esp_netif_driver_base_t base;
  USBEthernet *owner;
};

class USBEthernet : public usb_host::USBClient {
 public:
  USBEthernet(uint16_t vid, uint16_t pid);

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::ETHERNET; }

#ifdef USE_USB_ETHERNET_MANUAL_IP
  void set_manual_ip(const ManualIP &manual_ip) { this->manual_ip_ = manual_ip; }
#endif

  bool is_connected() const;
  esp_netif_t *get_esp_netif() const { return this->netif_; }
  network::IPAddresses get_ip_addresses();

 protected:
  enum class Proto : uint8_t { NONE, ECM, NCM };
  enum class Step : uint8_t { IDLE, MAC, NTB_PARAMS, NTB_INPUT, FILTER, ALT, RUNNING };

  static constexpr uint8_t XFER_FREE = 0;
  static constexpr uint8_t XFER_IDLE = 1;
  static constexpr uint8_t XFER_BUSY = 2;
  static constexpr size_t RX_COUNT = 2;
  static constexpr size_t TX_COUNT = 2;
  static constexpr size_t NTB_LIMIT = 2048;
  static constexpr size_t NTB_MAX = 16384;
  static constexpr uint32_t TX_WAIT_MS = 1000;

  void on_connected() override;
  void on_disconnected() override;
  bool reports_connection_itself() const override { return true; }
  usb_host::MatchTier refine_match_(const usb_host::RoutedDevice &dev, usb_host::MatchTier tier) const override;

  bool parse_descriptors_();
  void step_(uint32_t generation, bool ok);
  void next_(uint32_t generation, Step step);
  void control_(uint32_t generation, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                const std::vector<uint8_t> &data, int32_t w_length);
  void read_mac_(const uint8_t *data, size_t len);
  void read_ntb_params_(const uint8_t *data, size_t len);
  void fallback_mac_();
  bool start_data_();
  bool alloc_(DataXfer &x, size_t size, uint8_t ep, usb_transfer_cb_t cb);
  void stop_data_();
  void retire_(DataXfer &x);
  void release_slot_(DataXfer &x);
  bool submit_(DataXfer &x, size_t len);

  void apply_link_(uint32_t generation);
  void link_up_actions_();
  void link_down_actions_();

  static void rx_cb_(usb_transfer_t *xfer);
  static void tx_cb_(usb_transfer_t *xfer);
  static void notify_cb_(usb_transfer_t *xfer);
  void handle_rx_(const uint8_t *data, size_t len);
  void handle_ntb_(const uint8_t *data, size_t len);
  void deliver_(const uint8_t *data, size_t len);
  void handle_notification_(const uint8_t *data, size_t len);

  static esp_err_t post_attach_(esp_netif_t *netif, esp_netif_iodriver_handle h);
  static esp_err_t transmit_(void *h, void *buffer, size_t len);
  static void free_rx_buffer_(void *h, void *buffer);
  esp_err_t send_frame_(const uint8_t *frame, size_t len);
  static void ip_event_handler_(void *arg, esp_event_base_t base, int32_t id, void *data);

  esp_netif_t *netif_{nullptr};
  NetifGlue glue_{};
#ifdef USE_USB_ETHERNET_MANUAL_IP
  optional<ManualIP> manual_ip_{};
#endif

  Proto proto_{Proto::NONE};
  Step step_state_{Step::IDLE};
  uint8_t comm_intf_{0xFF};
  uint8_t data_intf_{0xFF};
  uint8_t data_alt_{0};
  bool comm_claimed_{false};
  bool data_claimed_{false};
  uint8_t notify_ep_{0};
  uint16_t notify_mps_{0};
  uint8_t in_ep_{0};
  uint16_t in_mps_{0};
  uint8_t out_ep_{0};
  uint16_t out_mps_{0};
  uint8_t mac_index_{0};
  uint8_t ncm_caps_{0};
  uint16_t max_segment_{1514};
  uint8_t mac_[6]{};
  bool mac_valid_{false};

  uint32_t ntb_in_max_{0};
  uint32_t ntb_out_max_{0};
  uint16_t ntb_out_divisor_{4};
  uint16_t ntb_out_remainder_{0};
  uint16_t ntb_out_alignment_{4};
  uint16_t ntb_sequence_{0};
  size_t rx_size_{0};
  size_t tx_size_{0};

  DataXfer rx_[RX_COUNT]{};
  DataXfer tx_[TX_COUNT]{};
  DataXfer notify_{};
  StaticSemaphore_t tx_sem_buf_{};
  SemaphoreHandle_t tx_sem_{nullptr};

  std::atomic<uint32_t> generation_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> link_target_{false};
  std::atomic<uint32_t> link_speed_{0};
  std::atomic<uint32_t> rx_dropped_{0};
  std::atomic<bool> got_ip4_{false};
  std::atomic<bool> got_ip6_{false};
  bool started_{false};
  bool link_up_{false};
};

extern USBEthernet *global_usb_ethernet;

}

#endif
