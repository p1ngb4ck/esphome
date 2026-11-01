#pragma once

#include "esphome/core/defines.h"

#ifdef USE_REMOTE_DESKTOP_USB

#include "esphome/components/select/select.h"
#include "esphome/components/usb_host/usb_host.h"
#include "../remote_desktop.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>

namespace esphome::remote_desktop {

class UsbLink {
 public:
  virtual bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) = 0;
};

class UsbSlot : public select::Select, public usb_host::USBClient {
 public:
  UsbSlot() : usb_host::USBClient(0, 0) {}
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_parent(RemoteDesktop *parent) { this->parent_ = parent; }
  void set_index(uint8_t index) { this->index_ = index; }
  uint8_t index() const { return this->index_; }

  void refresh_options();
  void forget_selection(uint8_t addr);
  uint8_t wanted_addr() const { return this->want_addr_; }
  void want_device(uint16_t vid, uint16_t pid, const char *serial);
  void selected_device(uint16_t &vid, uint16_t &pid, char *serial, size_t serial_len) const;

  enum class Proto : uint8_t { USBREDIR, USBIP };
  size_t usbip_entry(uint8_t *out, size_t cap, bool interfaces) const;

  bool link_open(UsbLink *link, Proto proto = Proto::USBREDIR);
  void link_close();
  bool release_buffers();
  uint8_t *rx_space(size_t &room);
  bool rx_done(size_t n);
  bool service();

 protected:
  static constexpr size_t MAX_DEVICES = 8;
  static constexpr size_t LABEL_BYTES = 48;
  static constexpr size_t STAGE_BYTES = 4096;
  static constexpr uint8_t GEN_XFERS = 4;
  static constexpr size_t GEN_BYTES = 8 + 131072;
  static constexpr uint8_t INT_XFERS = 8;
  static constexpr size_t INT_BYTES = 3072;
  static constexpr uint8_t ISO_STREAMS = 2;
  static constexpr uint8_t ISO_URBS = 16;
  static constexpr uint8_t ISO_PKTS = 32;
  static constexpr uint8_t INT_PER_EP = 2;
  static constexpr uint8_t MAX_INT_IN = INT_XFERS / INT_PER_EP;

  enum class Kind : uint8_t { CONTROL, BULK, INTERRUPT_OUT, INTERRUPT_IN, ISO, SET_ALT };
  struct IsoStream;
  struct IntIn;
  struct Xfer {
    usb_transfer_t *t{nullptr};
    UsbSlot *slot{nullptr};
    uint64_t id{0};
    Kind kind{Kind::CONTROL};
    uint8_t ep{0};
    uint16_t length{0};
    uint32_t want{0};
    uint8_t request{0}, requesttype{0};
    uint16_t value{0}, index{0};
    bool busy{false};
    bool in_flight{false};
    IsoStream *iso{nullptr};
    IntIn *intr{nullptr};
    uint8_t filled{0};
    bool usbip_in{false};
    uint32_t usbip_len{0};
    uint32_t usbip_unlink{0};
    uint16_t usbip_packets{0};
    uint8_t usbip_index{0};
  };
  struct IsoStream {
    bool on{false};
    bool stopping{false};
    bool started{false};
    uint8_t ep{0};
    uint8_t pkts{0};
    uint8_t urbs{0};
    uint16_t pkt_size{0};
    uint8_t fill{0};
    uint16_t drop{0};
    uint64_t next_id{0};
    Xfer x[ISO_URBS];
  };
  struct IntIn {
    bool on{false};
    bool stopping{false};
    uint8_t ep{0};
    uint64_t next_id{0};
    Xfer *x[INT_PER_EP]{};
  };
  struct EpInfo {
    uint8_t type, interval, interface;
    uint16_t mps;
  };
  enum class Rx : uint8_t { HEADER, BODY, TYPE_HEADER, DATA, WAIT_DATA, WAIT_BODY, WAIT_ALT, SKIP };
  struct AltChange {
    bool active{false};
    bool sent{false};
    uint64_t id{0};
    uint8_t interface{0};
    uint8_t alt{0};
  };

  void control(const std::string &value) override;
  void usbip_snapshot_();
  uint8_t find_wanted_() const;
  void on_connected() override;
  void on_disconnected() override;
  void apply_();

  static void xfer_done_(usb_transfer_t *t);
  Xfer *gen_take_();
  void parse_eps_();
  bool send_(uint32_t type, uint64_t id, const void *type_header, size_t type_header_len, const uint8_t *data = nullptr,
             size_t data_len = 0);
  bool send_hello_();
  bool send_device_info_();
  bool handle_body_();
  bool begin_data_packet_();
  bool submit_(Xfer *x);
  bool complete_(Xfer *x);
  bool start_iso_(uint64_t id, uint8_t ep, uint8_t pkts, uint8_t urbs);
  bool stop_iso_(uint64_t id, uint8_t ep, bool reply);
  bool iso_out_(uint8_t ep, const uint8_t *data, size_t len);
  bool start_int_(uint64_t id, uint8_t ep);
  bool stop_int_(uint64_t id, uint8_t ep, bool reply);
  void cancel_ep_(uint8_t ep);
  bool set_alt_(uint64_t id, uint8_t interface, uint8_t alt);
  bool alt_step_();
  bool alt_done_(bool ok);
  bool interface_busy_(uint8_t interface) const;
  bool device_speed_high_() const;
  void drain_();
  size_t header_len_() const { return this->ids64_ ? 16 : 12; }
#ifdef USE_REMOTE_DESKTOP_USB
  static constexpr uint8_t USBIP_ISO_XFERS = 8;
#else
  static constexpr uint8_t USBIP_ISO_XFERS = 0;
#endif
  static constexpr uint16_t USBIP_ISO_PKTS = 64;
  bool usbip_service_();
  uint8_t *usbip_rx_space_(size_t &room);
  bool usbip_rx_done_(size_t n);
  bool usbip_begin_submit_();
  bool usbip_submit_(Xfer *x);
  bool usbip_complete_(Xfer *x);
  bool usbip_unlink_(uint32_t seqnum, uint32_t target);
  bool usbip_send_ret_unlink_(uint32_t seqnum, int32_t status);
  Xfer *usbip_take_(Kind kind, size_t bytes, uint16_t packets);
  bool usable_() const { return this->device_ready_.load(std::memory_order_acquire); }
  struct PoolBuf {
    usb_transfer_t *t;
    uint8_t *own;
    size_t own_size;
    uint8_t *piece;
  };
  static constexpr size_t POOL_BUFS = GEN_XFERS + INT_XFERS + ISO_STREAMS * ISO_URBS + USBIP_ISO_XFERS;
  bool xfer_alloc_(usb_transfer_t **t, size_t bytes, int packets);
  void xfer_free_(usb_transfer_t *t);
  PoolBuf *pool_bufs_{nullptr};

  RemoteDesktop *parent_{nullptr};
  uint8_t index_{0};
  usb_host::USBHost *usb_{nullptr};
  char (*labels_)[LABEL_BYTES]{nullptr};
  uint8_t label_addr_[MAX_DEVICES + 1]{};
  uint8_t label_count_{0};
  uint8_t want_addr_{0};
  uint16_t want_vid_{0}, want_pid_{0};
  char want_serial_[64]{};
  uint8_t taking_addr_{0};

  std::atomic<bool> link_up_{false};
  std::atomic<bool> device_ready_{false};
  std::atomic<bool> drain_req_{false};
  std::atomic<bool> drained_{false};
  SemaphoreHandle_t dev_lock_{nullptr};

  UsbLink *link_{nullptr};
  bool peer_hello_{false};
  bool ids64_{false};
  bool peer_connect_version_{false};
  bool peer_ep_mps_{false};
  bool peer_32bit_bulk_{false};
  bool announced_{false};
  uint8_t alts_[32]{};
  uint8_t config_value_{1};
  EpInfo eps_[32]{};
  uint8_t claimed_[32]{};
  uint8_t claimed_count_{0};
  QueueHandle_t done_q_{nullptr};
  struct Session {
    Xfer gen[GEN_XFERS];
    Xfer intr[INT_XFERS];
    IntIn int_in[MAX_INT_IN];
    IsoStream iso[ISO_STREAMS];
    uint8_t stage[STAGE_BYTES];
  };
  Session *xs_{nullptr};
  uint8_t *stage_{nullptr};
  Rx rx_{Rx::HEADER};
  AltChange alt_;
  uint8_t hdr_[48]{};
  size_t have_{0};
  uint32_t type_{0};
  uint32_t body_len_{0};
  uint64_t id_{0};
  uint32_t skip_left_{0};
  Xfer *rx_xfer_{nullptr};
  size_t data_len_{0};
  size_t type_header_len_{0};
  Proto proto_{Proto::USBREDIR};
  Xfer *usbip_iso_{nullptr};
  uint32_t *usbip_offsets_{nullptr};
  uint32_t usbip_flags_{0};
  int32_t usbip_packets_{-1};
  SemaphoreHandle_t usbip_lock_{nullptr};
  static constexpr size_t USBIP_ENTRY_BYTES = 0x138 + 4 * 32;
  uint8_t *usbip_entry_{nullptr};
  size_t usbip_entry_len_{0};
  uint8_t usbip_entry_ifaces_{0};
};

}

#endif
