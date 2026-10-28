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
/// In a VNC session the same device is exported over USB/IP instead (usbip.cpp): bus id "1-<n+1>",
/// imported by the desktop's host with `usbip attach`.
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
  /// Its place among the slots: the usbredir channel id, and USB/IP bus id 1-<index+1>.
  void set_index(uint8_t index) { this->index_ = index; }
  uint8_t index() const { return this->index_; }

  /// Main loop: the device list changed (USBHost), or another slot took `addr`.
  void refresh_options();
  void forget_selection(uint8_t addr);
  uint8_t wanted_addr() const { return this->want_addr_; }
  /// Main loop: the device of a saved connection (vid 0: none), picked once it is plugged in.
  void want_device(uint16_t vid, uint16_t pid, const char *serial);
  /// The picked device's ids (vid 0: none).
  void selected_device(uint16_t &vid, uint16_t &pid, char *serial, size_t serial_len) const;

  enum class Proto : uint8_t { USBREDIR, USBIP };
  /// USB/IP: the device as OP_REP_DEVLIST describes it (with its interfaces), its size; 0: none picked.
  /// Any task (a snapshot the main loop keeps).
  size_t usbip_entry(uint8_t *out, size_t cap, bool interfaces) const;

  // Spice task. false: no room in the pool for its transfers (the channel is not opened).
  bool link_open(SpiceVmcLink *link, Proto proto = Proto::USBREDIR);
  void link_close();
  /// No session: the transfers' buffers back to the player's pool (those not in flight). Any task, under
  /// the player's desktop_lock_.
  bool release_buffers();
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
    // USB/IP: the URB's direction, size asked for, and an UNLINK waiting for it (seqnum, 0: none).
    bool usbip_in{false};
    uint32_t usbip_len{0};
    uint32_t usbip_unlink{0};
    uint16_t usbip_packets{0};
    uint8_t usbip_index{0};  // iso: which of usbip_iso_
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
  /// Main loop: the picked device's USB/IP description, for usbip_entry().
  void usbip_snapshot_();
  /// The device present that matches want_vid_/pid_/serial_, 0 if none.
  uint8_t find_wanted_() const;
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
  // USB/IP (usbip.cpp), spice task.
#ifdef SVP_DESKTOP_VNC
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
  // Transfer buffers from the player's pool, only while a remote desktop session has them: the transfer
  // is made with a buffer of its own of a cache line, the pool's piece takes its place meanwhile and it
  // goes back before usb_host_transfer_free() (which frees the transfer's own).
  struct PoolBuf {
    usb_transfer_t *t;
    uint8_t *own;
    size_t own_size;
    uint8_t *piece;
  };
  static constexpr size_t POOL_BUFS = GEN_XFERS + INT_XFERS + ISO_STREAMS * ISO_URBS + USBIP_ISO_XFERS;
  bool xfer_alloc_(usb_transfer_t **t, size_t bytes, int packets);
  void xfer_free_(usb_transfer_t *t);
  PoolBuf *pool_bufs_{nullptr};  // PSRAM: POOL_BUFS

  SimpleVideoPlayer *parent_{nullptr};
  uint8_t index_{0};
  usb_host::USBHost *usb_{nullptr};
  // Options: [0] "None", then a device each (main loop).
  char (*labels_)[LABEL_BYTES]{nullptr};
  uint8_t label_addr_[MAX_DEVICES + 1]{};
  uint8_t label_count_{0};
  uint8_t want_addr_{0};    // 0: none
  // A saved connection's device, picked when it comes (want_vid_ 0: none).
  uint16_t want_vid_{0}, want_pid_{0};
  char want_serial_[64]{};
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
  // A redirection session's transfers: allocated by link_open() (internal: the session's hot path), freed by
  // release_buffers() once none of them holds a buffer - nothing of it while no session runs
  struct Session {
    Xfer gen[GEN_XFERS];
    Xfer intr[INT_XFERS];
    IntIn int_in[MAX_INT_IN];
    IsoStream iso[ISO_STREAMS];
    uint8_t stage[STAGE_BYTES];
  };
  Session *xs_{nullptr};
  uint8_t *stage_{nullptr};  // STAGE_BYTES, internal RAM
  Rx rx_{Rx::HEADER};
  AltChange alt_;
  uint8_t hdr_[48]{};  // usbredir: up to 16; USB/IP: 48
  size_t have_{0};
  uint32_t type_{0};
  uint32_t body_len_{0};
  uint64_t id_{0};
  uint32_t skip_left_{0};
  Xfer *rx_xfer_{nullptr};
  size_t data_len_{0};
  size_t type_header_len_{0};
  // USB/IP.
  Proto proto_{Proto::USBREDIR};
  Xfer *usbip_iso_{nullptr};          // PSRAM: USBIP_ISO_XFERS
  uint32_t *usbip_offsets_{nullptr};  // PSRAM: USBIP_ISO_PKTS offsets per iso transfer, as the client laid them
  uint32_t usbip_flags_{0};           // the URB being read: its transfer_flags
  int32_t usbip_packets_{-1};         // its number_of_packets (-1: not isochronous)
  SemaphoreHandle_t usbip_lock_{nullptr};
  static constexpr size_t USBIP_ENTRY_BYTES = 0x138 + 4 * 32;
  uint8_t *usbip_entry_{nullptr};  // PSRAM; main loop: the picked device, as OP_REP_DEVLIST lists it
  size_t usbip_entry_len_{0};
  uint8_t usbip_entry_ifaces_{0};
};

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE_USB
