#pragma once

#include "esphome/core/defines.h"

#ifdef SVP_SPICE_USB

#include "esphome/components/select/select.h"
#include "esphome/components/usb_host/usb_host.h"
#include "../simple_video_player.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <atomic>

namespace esphome::simple_video_player {

/// Where a slot's usbredir bytes go: its usbredir channel of the SPICE session (spice task).
class SpiceVmcLink {
 public:
  virtual bool vmc_send(const uint8_t *head, size_t head_len, const uint8_t *data, size_t data_len) = 0;
};

/// One usbredir channel of the SPICE server (the n-th slot takes channel id n): the panel's USB device
/// picked here is redirected to it while a session runs, taken from the drivers that hold it until then.
/// The panel is the usb-host of the usbredir protocol (usbredir's docs/usb-redirection-protocol.md).
///
/// Threads: the select, routing and the device's open/close on the main loop; the protocol, transfers
/// and their completions on the spice task (completions are handed over from the USB task).
class SpiceUsbSlot : public select::Select, public usb_host::USBClient {
 public:
  SpiceUsbSlot() : usb_host::USBClient(0, 0) {}
  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_parent(SimpleVideoPlayer *parent) { this->parent_ = parent; }

  /// Main loop: the device list changed (USBHost), or another slot took `addr`.
  void refresh_options();
  void forget_selection(uint8_t addr);
  uint8_t wanted_addr() const { return this->want_addr_; }

  // Spice task.
  void link_open(SpiceVmcLink *link);
  void link_close();
  /// Where the next bytes from the channel go and how many; 0: waits for a free transfer.
  uint8_t *rx_space(size_t &room);
  /// n bytes arrived at rx_space(). false: a protocol error, the channel ends.
  bool rx_done(size_t n);
  /// Completions, the device coming or going, a packet waiting for a transfer. false: the channel ends.
  bool service();

 protected:
  static constexpr size_t MAX_DEVICES = 8;
  static constexpr size_t LABEL_BYTES = 48;
  static constexpr size_t STAGE_BYTES = 4096;      // internal RAM: headers and small packets
  static constexpr uint8_t GEN_XFERS = 4;          // control and bulk
  static constexpr size_t GEN_BYTES = 8 + 131072;  // a bulk transfer of up to 128 KiB (usb-storage asks 120)
  static constexpr uint8_t INT_XFERS = 8;          // interrupt IN receiving
  static constexpr size_t INT_BYTES = 3072;
  static constexpr uint8_t ISO_STREAMS = 2;
  static constexpr uint8_t ISO_URBS = 16;  // usbredirhost's MAX_TRANSFER_COUNT
  static constexpr uint8_t ISO_PKTS = 32;  // usbredirhost's MAX_PACKETS_PER_TRANSFER
  static constexpr uint8_t INT_PER_EP = 2;
  static constexpr uint8_t MAX_INT_IN = INT_XFERS / INT_PER_EP;

  enum class Kind : uint8_t { CONTROL, BULK, INTERRUPT_OUT, INTERRUPT_IN, ISO, SET_ALT };
  struct IsoStream;
  struct IntIn;
  struct Xfer {
    usb_transfer_t *t{nullptr};
    SpiceUsbSlot *slot{nullptr};
    uint64_t id{0};
    Kind kind{Kind::CONTROL};
    uint8_t ep{0};
    uint16_t length{0};  // asked for (control, interrupt)
    uint32_t want{0};    // asked for (bulk)
    uint8_t request{0}, requesttype{0};
    uint16_t value{0}, index{0};
    bool busy{false};
    bool in_flight{false};
    IsoStream *iso{nullptr};
    IntIn *intr{nullptr};
    uint8_t filled{0};  // ISO OUT: packets in it
  };
  struct IsoStream {
    bool on{false};
    bool stopping{false};
    bool started{false};
    uint8_t ep{0};
    uint8_t pkts{0};
    uint8_t urbs{0};
    uint16_t pkt_size{0};
    uint8_t fill{0};   // OUT: the URB being filled
    uint16_t drop{0};  // OUT: packets to drop after an overflow
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
  // WAIT_DATA: a control or bulk packet waits for a transfer; WAIT_BODY: another packet (in stage_)
  // does; WAIT_ALT: set_alt_setting runs (a control packet: nothing else is read meanwhile).
  enum class Rx : uint8_t { HEADER, BODY, TYPE_HEADER, DATA, WAIT_DATA, WAIT_BODY, WAIT_ALT, SKIP };
  struct AltChange {
    bool active{false};
    bool sent{false};
    uint64_t id{0};
    uint8_t interface{0};
    uint8_t alt{0};
  };

  void control(const std::string &value) override;
  void on_connected() override;
  void on_disconnected() override;
  /// Main loop: take, give back or change the device as wanted.
  void apply_();

  // Spice task.
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
  /// The alternate setting change goes on: streams of the interface back, then SET_INTERFACE.
  bool alt_step_();
  bool alt_done_(bool ok);
  bool interface_busy_(uint8_t interface) const;
  bool device_speed_high_() const;
  /// All transfers back, streams stopped (the device goes, or the channel ends).
  void drain_();
  size_t header_len_() const { return this->ids64_ ? 16 : 12; }
  bool usable_() const { return this->device_ready_.load(std::memory_order_acquire); }

  SimpleVideoPlayer *parent_{nullptr};
  usb_host::USBHost *usb_{nullptr};
  // Options: [0] "None", then a device each (main loop).
  char (*labels_)[LABEL_BYTES]{nullptr};
  uint8_t label_addr_[MAX_DEVICES + 1]{};
  uint8_t label_count_{0};
  uint8_t want_addr_{0};    // 0: none
  uint8_t taking_addr_{0};  // take_device() asked for it, not bound yet (main loop)

  std::atomic<bool> link_up_{false};
  std::atomic<bool> device_ready_{false};  // opened, interfaces claimed (main loop sets)
  std::atomic<bool> drain_req_{false};     // main loop: let the device go (spice task drains)
  std::atomic<bool> drained_{false};       // spice task: nothing in flight any more
  SemaphoreHandle_t dev_lock_{nullptr};    // the device's handles and descriptors

  // Spice task.
  SpiceVmcLink *link_{nullptr};
  bool peer_hello_{false};
  bool ids64_{false};
  bool peer_connect_version_{false};
  bool peer_ep_mps_{false};
  bool peer_32bit_bulk_{false};
  bool announced_{false};  // device_connect sent
  uint8_t alts_[32]{};
  uint8_t config_value_{1};
  EpInfo eps_[32]{};
  uint8_t claimed_[32]{};
  uint8_t claimed_count_{0};
  QueueHandle_t done_q_{nullptr};
  Xfer gen_[GEN_XFERS];
  Xfer int_[INT_XFERS];
  IntIn int_in_[MAX_INT_IN];
  IsoStream iso_[ISO_STREAMS];
  uint8_t *stage_{nullptr};  // STAGE_BYTES, internal RAM
  Rx rx_{Rx::HEADER};
  AltChange alt_;
  uint8_t hdr_[16]{};
  size_t have_{0};
  uint32_t type_{0};
  uint32_t body_len_{0};
  uint64_t id_{0};
  uint32_t skip_left_{0};
  Xfer *rx_xfer_{nullptr};
  size_t data_len_{0};
  size_t type_header_len_{0};
};

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE_USB
