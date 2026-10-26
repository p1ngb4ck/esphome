#pragma once

// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include <memory>
#include "esphome/core/helpers.h"
#include <vector>
#include "usb/usb_host.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esphome/core/lock_free_queue.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/log.h"
#include <atomic>
#include <span>

namespace esphome::usb_host {

// Which matching the configuration uses, set by usb_host's final codegen coroutine once every driver has
// registered its options (0 when unused: the code below compiles out through `if constexpr`).
#ifndef USB_HOST_WHITELIST
#define USB_HOST_WHITELIST 0
#endif
#ifndef USB_HOST_BLACKLIST
#define USB_HOST_BLACKLIST 0
#endif
#ifndef USB_HOST_INTERFACE_BLACKLIST
#define USB_HOST_INTERFACE_BLACKLIST 0
#endif
/// Some driver names its device by vid / pid / manufacturer / product (the whitelist, 0 = wildcard).
static constexpr bool USB_WHITELIST_USED = USB_HOST_WHITELIST != 0;
/// Some driver refuses listed devices (exact vid:pid pairs).
static constexpr bool USB_BLACKLIST_USED = USB_HOST_BLACKLIST != 0;
/// Some driver leaves single interfaces of listed devices alone (exact vid:pid plus interface numbers).
static constexpr bool USB_INTERFACE_BLACKLIST_USED = USB_HOST_INTERFACE_BLACKLIST != 0;

/// Bit of an endpoint in a skipped-endpoints mask (USBClient::skipped_endpoints_()): its number, plus 16 for IN.
inline uint32_t endpoint_bit(const usb_ep_desc_t *ep) {
  return 1u << ((ep->bEndpointAddress & 0x0F) + ((ep->bEndpointAddress & 0x80) ? 16 : 0));
}

/// Calls `fn` for each endpoint descriptor of one alternate setting (`alt` points into `config`).
template<typename F> void for_each_alt_endpoint(const usb_config_desc_t *config, const usb_intf_desc_t *alt, F &&fn) {
  int offset = reinterpret_cast<const uint8_t *>(alt) - reinterpret_cast<const uint8_t *>(config);
  const auto *next = reinterpret_cast<const usb_standard_desc_t *>(alt);
  while ((next = usb_parse_next_descriptor(next, config->wTotalLength, &offset)) != nullptr) {
    if (next->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE)
      return;
    if (next->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT)
      fn(reinterpret_cast<const usb_ep_desc_t *>(next));
  }
}

// THREADING MODEL:
// This component uses a dedicated USB task for event processing to prevent data loss.
// - USB Task (high priority): Handles USB events, executes transfer callbacks, releases transfer slots
// - Main Loop Task: Initiates transfers, processes device connect/disconnect events
//
// Thread-safe communication:
// - Lock-free queues for USB task -> main loop events (SPSC pattern)
// - Lock-free TransferRequest pool using atomic bitmask (MCMP pattern)
//
// Transfer submission engine (USBHost) is stateless w.r.t. client memory -- the
// Linux URB model: client owns the pool, fills a slot, hands it to USBHost.
//
// TransferRequest pool access pattern:
// - get_trq_() [allocate]: called from BOTH the USB task and the main loop
//   * USB task: via input callbacks that restart their transfer immediately
//   * Main loop: for output transfers and flow-controlled input restarts
// - release_trq() [deallocate]: called from BOTH the USB task and the main loop
//   * USB task: right after a transfer callback completes (this is what keeps the
//     pool from running dry under load)
//   * Main loop: when a submission fails
//
// The dual-threaded allocation/deallocation is intentional rather than an oversight:
// - the USB task can restart an input transfer and give the slot back without a
//   context switch to the main loop
// - the main loop keeps control of backpressure by deciding when to restart after
//   it has consumed the data
// The atomic bitmask makes that safe without taking a mutex on either side.

ESPHOME_LOG_TAG(TAG, "usb_host");

// Forward declarations
struct TransferRequest;
class USBClient;
class USBHost;

// constants for setup packet type
static constexpr uint8_t USB_RECIP_DEVICE = 0;
static constexpr uint8_t USB_RECIP_INTERFACE = 1;
static constexpr uint8_t USB_RECIP_ENDPOINT = 2;
static constexpr uint8_t USB_TYPE_STANDARD = 0 << 5;
static constexpr uint8_t USB_TYPE_CLASS = 1 << 5;
static constexpr uint8_t USB_TYPE_VENDOR = 2 << 5;
static constexpr uint8_t USB_DIR_MASK = 1 << 7;
static constexpr uint8_t USB_DIR_IN = 1 << 7;
static constexpr uint8_t USB_DIR_OUT = 0;
static constexpr size_t SETUP_PACKET_SIZE = 8;

static constexpr size_t MAX_REQUESTS = USB_HOST_MAX_REQUESTS;
static_assert(MAX_REQUESTS >= 1 && MAX_REQUESTS <= 32, "MAX_REQUESTS must be between 1 and 32");

// Bitmask type tracking which TransferRequest slots are in use. It needs at least as many
// bits as MAX_REQUESTS, hence uint16_t up to 16 requests and uint32_t for 17-32. This is
// tied to the static_assert above: raising MAX_REQUESTS beyond 32 means changing both.
using trq_bitmask_t = std::conditional<(MAX_REQUESTS <= 16), uint16_t, uint32_t>::type;
static constexpr trq_bitmask_t ALL_REQUESTS_IN_USE = MAX_REQUESTS == 32 ? ~0 : (1 << MAX_REQUESTS) - 1;

// Compile-time packet size from the usb_host max_packet_size option. Used where a buffer has
// to have a fixed size at compile time (usb_uart's chunk pool), which cannot follow a device.
static constexpr size_t USB_MAX_PACKET_SIZE = USB_HOST_MAX_PACKET_SIZE;
// Floor for a client's transfer buffers. The size actually used is derived per device from
// its own descriptors, so a device on the high-speed controller gets its 512 byte packets and
// one on the full-speed controller gets its 64, at the same time. This only raises the floor,
// for a driver whose control data stage is larger than any endpoint packet on the device.
static constexpr size_t USB_MIN_TRANSFER_BUFFER = USB_HOST_MAX_PACKET_SIZE;
static constexpr size_t USB_EVENT_QUEUE_SIZE = 32;
static constexpr size_t USB_TASK_STACK_SIZE = 4096;
// This task only ever blocks in usb_host_client_handle_events() and wakes to service transfer
// completions / release transfer slots, so it must run PROMPTLY once a transfer finishes -- a
// caller blocked in a bulk read (e.g. storage_worker -> usb_storage fread) cannot return until
// this task has processed the completion. At the old value of 5 it sat below application tasks
// (audio/decode run at 10, storage_worker at 9), so those starved it and the read never
// completed. 12 keeps it above the application tier and well below the Wi-Fi/RTOS range; it is
// mostly blocked, so the higher priority costs nothing when idle.
static constexpr UBaseType_t USB_TASK_PRIORITY = 12;
static constexpr size_t SHARED_USB_TASK_STACK_SIZE = 6144;
static constexpr size_t MAX_ACTIVE_CLIENTS = 16;

// Transfer status reported to callback
struct TransferStatus {
  uint8_t *data;
  size_t data_len;
  void *user_data;
  uint16_t error_code;
  uint8_t endpoint;
  bool success;
};

using transfer_cb_t = std::function<void(const TransferStatus &)>;

// TransferRequest -- our URB equivalent.
// Client owns the pool, fills a slot, passes it to USBHost::submit_*().
// USBHost never allocates or frees these.
struct UsbDevice;

struct TransferRequest {
  usb_transfer_t *transfer;
  transfer_cb_t callback;
  TransferStatus status;
  USBClient *client;
  UsbDevice *device{nullptr};  // the pool it belongs to: a further device's (null: the client's first device)
};

/// One device of a client that serves several (USBClient::accepts_more_devices_(): usb_hid, usb_storage):
/// its handle, descriptors, routed interfaces and its own transfer pool. Allocated when routing hands the
/// device over, freed when it is gone; the driver keeps its per-device state in `context`.
struct UsbDevice {
  int addr{-1};
  usb_device_handle_t handle{nullptr};
  const usb_device_desc_t *desc{nullptr};
  const usb_config_desc_t *config{nullptr};
  uint32_t interfaces{0};
  bool opened{false};
  std::atomic<bool> dead{false};  // let go (unplugged / closed): freed once no transfer of it is out any more
  void *context{nullptr};
  TransferRequest requests[MAX_REQUESTS]{};
  std::atomic<trq_bitmask_t> in_use{0};
  /// Whether routing handed this interface of the device to its driver.
  bool interface_routed(uint8_t number) const { return number < 32 && (this->interfaces & (1u << number)) != 0; }
};

// A client only hears about the devices it opened: which device a driver gets is decided
// by USBHost, not by the driver's own IDF client.
enum EventType : uint8_t {
  EVENT_DEVICE_GONE,
};

struct UsbEvent {
  EventType type;
  union {
    struct {
      usb_device_handle_t handle;
    } device_gone;
  } data;
  void release() {}
};

// One entry of a driver's match table, in the manner of Linux's usb_device_id. -1 (or 0
// for vid/pid) matches anything.
struct UsbMatch {
  uint16_t vid;
  uint16_t pid;
  int16_t cls;
  int16_t subclass;
  int16_t protocol;
};

// How a driver matched a device. Higher wins, and the interfaces a driver names by its
// filters (vid/pid/manufacturer/product from the YAML) are reserved for such drivers; the
// device's other interfaces still go to the drivers that match them.
enum class MatchTier : uint8_t {
  NONE,
  CLASS,   // one of its class patterns matches an interface
  VENDOR,  // its built-in table names the device (devices that cannot be routed by class)
  NAMED,   // its configured filters name the device
};

// A message that takes a device out of the mode it enumerates in (a Bluetooth dongle in HID
// proxy mode, a camera or modem posing as mass storage), from the usb_host mode_switch option.
// Either a control request, or bulk messages to the first bulk OUT endpoint as usb_modeswitch
// sends its MessageContent.
struct ModeSwitch {
  uint16_t vid;
  uint16_t pid;
  uint8_t request_type;
  uint8_t request;
  uint16_t value;
  uint16_t index;
  std::vector<uint8_t> data;
  std::vector<std::vector<uint8_t>> messages;
  bool need_response;
  // When it was last sent; a device that comes back with the same ids soon after is routed
  // as it is instead of being switched again.
  uint32_t sent_ms;
};

// A device USBHost has open, from enumeration until it is gone.
struct RoutedDevice {
  uint8_t addr{0};
  usb_device_handle_t handle{nullptr};
  const usb_device_desc_t *desc{nullptr};
  const usb_config_desc_t *config{nullptr};
  usb_device_info_t info{};
  // False while the device is still being enumerated (the host channel budget): its string descriptors are
  // not read yet, so a manufacturer / product filter counts as matching.
  bool strings_known{true};
  // Interfaces handed to drivers, one bit per bInterfaceNumber.
  uint32_t assigned{0};
  std::vector<USBClient *> bound{};
  // Drivers that let the device go again; not offered it until it is plugged in anew.
  std::vector<USBClient *> declined{};
  bool reported_unrouted{false};
  // Mode switch in progress: not routed until it is done and the device had time to leave.
  const ModeSwitch *mode_switch{nullptr};
  uint8_t switch_step{0};
  usb_transfer_t *switch_xfer{nullptr};
  uint8_t switch_intf{0xFF};
  uint8_t switch_ep_out{0};
  uint8_t switch_ep_in{0};
  uint16_t switch_mps_in{0};
  uint32_t switched_ms{0};
  bool gone{false};
  // Given to this driver alone by take_device(): not routed to others until it lets go.
  USBClient *taken_by{nullptr};
  // take_device() under way: the holders' endpoints are halted, each is told the device is gone (as
  // on an unplug) once its transfers are back, and the taker gets it when all have let go.
  bool taking{false};
  uint32_t taking_ms{0};
  std::vector<USBClient *> leaving{};
};

// USB string descriptors hold at most 126 characters; one more for the terminator
static constexpr size_t DESC_STRING_BUF_SIZE = 128;

/// Identity of a connected USB device, copied out of the descriptors the USB host
/// stack caches for the lifetime of the connection
struct UsbDeviceInfo {
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t bcd_device;
  char manufacturer[DESC_STRING_BUF_SIZE];
  char product[DESC_STRING_BUF_SIZE];
  char serial_number[DESC_STRING_BUF_SIZE];
};

/// Copy a USB string descriptor into a NUL-terminated buffer. A missing descriptor copies as
/// an empty string. Returns false when a descriptor contains non-ASCII characters,
/// UTF-16 to UTF-8 conversion is not currently implemented.
bool copy_descriptor_string(const usb_str_desc_t *desc, std::span<char, DESC_STRING_BUF_SIZE> buffer);

enum ClientState {
  USB_CLIENT_INIT = 0,
  USB_CLIENT_OPEN,
  USB_CLIENT_CLOSE,
  USB_CLIENT_GET_DESC,
  USB_CLIENT_GET_INFO,
  USB_CLIENT_CONNECTED,
};

// -----------------------------------------------------------------------------
// Isochronous stream types -- compiled only when USE_USB_ISOC_TRANSFERS is set, which
// usb_host.require_isoc_transfers() does. That helper requests control transfers too,
// so the #error below only fires for a build that defines the macros by hand.
// -----------------------------------------------------------------------------
#if defined(USE_USB_ISOC_TRANSFERS) && !defined(USE_USB_CONTROL_TRANSFERS)
#error "USE_USB_ISOC_TRANSFERS requires USE_USB_CONTROL_TRANSFERS (alt-setting selection)"
#endif

#ifdef USE_USB_ISOC_TRANSFERS

// Where a stream is in its open sequence. Selecting the alt-setting is a control transfer
// and therefore asynchronous, so a stream is not usable the moment stream_open() returns:
// it is usable when the state reaches RUNNING, which USBClient::on_stream_open() reports.
enum class IsocOpenState : uint8_t {
  CLOSED,         // nothing claimed, no URBs, safe to open
  SELECTING_ALT,  // SET_INTERFACE submitted, waiting for the device to answer
  ABORTING,       // closed while SELECTING_ALT; its completion still has to release
  RUNNING,        // URBs submitted
};

struct IsocCbCtx {
  USBClient *client;  // vtable dispatch target for on_isoc_packet()
  struct IsocStream *stream;
};

struct IsocStream {
  // A several-devices driver's stream: the device it runs on (set by stream_open(UsbDevice &, ...));
  // null: the client's single device.
  UsbDevice *device{nullptr};
  std::unique_ptr<usb_transfer_t *[]> xfers {};
  std::unique_ptr<IsocCbCtx[]> ctxs{};
  std::atomic<uint8_t> pending_urbs{0};
  uint8_t num_urbs{0};
  uint8_t ep_addr{0};
  uint16_t mps{0};
  uint8_t packets_per_urb{0};
  uint8_t interface_num{0};
  uint8_t alt_setting{0};
  // Written by the main loop (stream_open/stream_close), read and written by the USB task
  // (isoc_cb). It carries the whole stop protocol, so it is atomic like every other field
  // shared between those two.
  std::atomic<bool> streaming{false};
  std::atomic<IsocOpenState> open_state{IsocOpenState::CLOSED};
  // Set from the USB task when the last URB retires while streaming was still requested,
  // i.e. the stream stopped on its own (device gone, or the periodic scheduler rejected
  // every resubmission). Nothing is transferred any more, but no close() was asked for, so
  // the owner has to notice and tear the stream down.
  std::atomic<bool> died{false};
  // UAC OUT pacing (isochronous sample clock). For an output stream each packet carries a
  // sample-rate-derived byte count instead of a full mps: sending mps-sized packets is ~2x
  // real time, overruns the device and, on the HS periodic scheduler (ESP32-P4), fails
  // resubmission. Mirrors the packet-size accumulator in Espressif's usb_host_uac.
  bool is_output{false};
  uint32_t packet_size{0};       // floor bytes per service interval (frames_floor * frame_size)
  uint32_t packet_size_frac{0};  // sample_rate % frac_div (remainder in frames per second)
  uint32_t frac_div{1000};       // service intervals per second (FS frame 1000, HS uframe 8000)
  uint32_t frac_accum{0};        // running fractional accumulator (0..frac_div-1)
  uint32_t frame_size{0};        // bytes per audio frame (channels * subframe_size)
  // Asynchronous OUT feedback: device-reported rate (samples per (micro)frame, Q10.14 at
  // full speed / Q16.16 at high speed). 0 = no feedback yet, use the nominal pacing above.
  std::atomic<uint32_t> fb_value{0};
  uint8_t fb_shift{14};  // fractional bits of fb_value (14 FS, 16 HS)
  uint32_t fb_accum{0};  // feedback fractional accumulator
  // A feedback stream shares its AS interface with the data stream, so it must not claim
  // or release that interface itself.
  bool owns_interface{true};

  // True once the URBs are in flight. Between stream_open() and this, the device is still
  // being switched to the alt-setting and the endpoint does not exist yet.
  bool is_running() const { return this->open_state.load(std::memory_order_acquire) == IsocOpenState::RUNNING; }
  // True when nothing is claimed and no URB is left, i.e. the stream may be opened again.
  bool is_closed() const {
    return this->open_state.load(std::memory_order_acquire) == IsocOpenState::CLOSED &&
           this->pending_urbs.load(std::memory_order_acquire) == 0;
  }
};

#endif  // USE_USB_ISOC_TRANSFERS

// -----------------------------------------------------------------------------
// USBClient -- device state machine + thin forwarding layer to USBHost.
// -----------------------------------------------------------------------------
class USBClient : public Component {
  friend class USBHost;

 public:
  // vid/pid (and the manufacturer/product filters) name one device from the YAML. Nonzero,
  // they override routing by class: the driver takes that device and only that device.
  USBClient(uint16_t vid, uint16_t pid) : trq_in_use_(0), vid_(vid), pid_(pid) {}
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::IO; }
  virtual void on_removed(usb_device_handle_t handle);
  void dump_config() override;
  // THREAD CONTEXT: both the USB task and the main loop
  // - USB task: immediately after a transfer callback completes
  // - Main loop: when a transfer submission failed
  void release_trq(TransferRequest *trq);
  trq_bitmask_t get_trq_in_use() const { return trq_in_use_; }
  /// Interfaces of this class (and subclass/protocol, -1 for any) are routed to this driver.
  void add_class_match(uint8_t cls, int16_t subclass = -1, int16_t protocol = -1) {
    this->class_matches_.push_back({0, 0, cls, subclass, protocol});
  }
  /// A device this driver serves although its class does not say so. pid 0 is any product
  /// of the vendor; cls -1 is the whole device, otherwise only interfaces of that class.
  void add_vendor_match(uint16_t vid, uint16_t pid, int16_t cls = -1, int16_t subclass = -1, int16_t protocol = -1) {
    this->vendor_matches_.push_back({vid, pid, cls, subclass, protocol});
  }
  /// Devices this driver refuses: `count` vid, pid pairs (exact, no wildcard), a codegen table in flash.
  void set_blacklist(const uint16_t *pairs, uint8_t count) {
    this->blacklist_ = pairs;
    this->blacklist_count_ = count;
  }
  /// Single interfaces this driver leaves alone: `count` entries of two words, vid << 16 | pid and the
  /// interfaces as a bit mask (bit = bInterfaceNumber), a codegen table in flash.
  void set_interface_blacklist(const uint32_t *entries, uint8_t count) {
    this->interface_blacklist_ = entries;
    this->interface_blacklist_count_ = count;
  }
  void set_manufacturer_filter(const char16_t *manufacturer) { this->manufacturer_filter_ = manufacturer; }
  void set_product_filter(const char16_t *product) { this->product_filter_ = product; }

  /// Whether a device has been opened and its setup by the subclass has finished
  bool is_connected() const { return this->connection_reported_; }

  /// Copy the connected device's identity out of the cached USB descriptors.
  /// Returns false when no device is connected or the host stack refused the query.
  bool get_device_info(UsbDeviceInfo &info) const;

  // -- Several devices per client (drivers with accepts_more_devices_()): the same calls for one of them --
#ifdef USE_USB_BULK_TRANSFERS
  bool transfer_in(UsbDevice &dev, uint8_t ep_address, const transfer_cb_t &callback, uint16_t length);
  bool transfer_out(UsbDevice &dev, uint8_t ep_address, const transfer_cb_t &callback, const uint8_t *data,
                    uint16_t length);
#endif
#ifdef USE_USB_CONTROL_TRANSFERS
  bool control_transfer(UsbDevice &dev, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                        const transfer_cb_t &callback, const std::vector<uint8_t> &data = {}, int32_t w_length = -1);
#endif
  bool claim_interface(UsbDevice &dev, uint8_t interface_num, uint8_t alt_setting = 0);
  bool release_interface(UsbDevice &dev, uint8_t interface_num);
  bool get_device_info(const UsbDevice &dev, UsbDeviceInfo &info) const;
  /// The driver gives one device up (declined, or done with it): closed and handed back to routing.
  void close_device(UsbDevice &dev);
  /// The devices this client serves (several-devices drivers), main loop.
  const std::vector<std::unique_ptr<UsbDevice>> &devices() const { return this->devices_; }

  /// Register a callback for the device this client claims being connected (true) or
  /// removed (false). Fires only for a device that was fully opened, so a device another
  /// client claims is never reported. Called from the main loop: connected once the device
  /// has been enumerated and the subclass has finished its setup of it (whether or not that
  /// setup succeeded), removed after on_disconnected() has run. This tracks the device's
  /// presence, not whether a given channel is usable.
  template<typename F> void add_on_connection_callback(F &&callback) {
    this->connection_callback_.add(std::forward<F>(callback));
  }

  // Lock-free event queue and pool -- public for static callbacks
  LockFreeQueue<UsbEvent, USB_EVENT_QUEUE_SIZE> event_queue;
  // The pool is one entry smaller than the queue because LockFreeQueue<T, N> is a ring
  // buffer holding N-1 elements. Sizing it that way makes allocate() return nullptr before
  // push() can fail, so a pool slot is never leaked on a full queue. The "- 1" is that
  // guarantee, not an off-by-one.
  EventPool<UsbEvent, USB_EVENT_QUEUE_SIZE - 1> event_pool;

  // -- Bulk / interrupt transfers ----------------------------------------------
  // Compiled only when a component calls usb_host.require_bulk_transfers() from its
  // to_code(); without that, these members do not exist and calls fail to compile.
#ifdef USE_USB_BULK_TRANSFERS
  // THREAD CONTEXT: both the USB task and the main loop
  // - USB task: an input callback restarting its own transfer for immediate reception
  // - Main loop: initial setup, and flow-controlled restarts after data was consumed
  bool transfer_in(uint8_t ep_address, const transfer_cb_t &callback, uint16_t length);
  // THREAD CONTEXT: both the USB task and the main loop
  // - USB task: an output callback restarting output directly, without a defer
  // - Main loop: the initial output trigger from write_array() and loop()
  bool transfer_out(uint8_t ep_address, const transfer_cb_t &callback, const uint8_t *data, uint16_t length);
#endif

  // -- Control transfers -------------------------------------------------------
  // Compiled only when a component calls usb_host.require_control_transfers() from its
  // to_code(); without that, these members do not exist and calls fail to compile.
#ifdef USE_USB_CONTROL_TRANSFERS
  // w_length is what goes into the setup packet's wLength field, i.e. the number of bytes
  // the request itself is defined to carry. It defaults to the size of data, which is what
  // a request whose data stage is exactly its buffer wants.
  //
  // They are separate because the host controller sizes the data stage from the buffer and
  // not from wLength (see the comment in the IDF's _buffer_fill_ctrl), and an IN data stage
  // needs a buffer of whole endpoint packets. A control read therefore rounds its buffer up
  // and passes the length the class defines here, so what goes on the wire stays the
  // request the device expects. Negative means "take it from data".
  bool control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index, const transfer_cb_t &callback,
                        const std::vector<uint8_t> &data = {}, int32_t w_length = -1);
#endif

  // -- Interface claim / release / alt-setting ---------------------------------
  bool claim_interface(uint8_t interface_num, uint8_t alt_setting = 0);
  bool release_interface(uint8_t interface_num);

#ifdef USE_USB_CONTROL_TRANSFERS
  bool set_interface(uint8_t interface_num, uint8_t alt_setting, const transfer_cb_t &callback);
  bool set_interface(UsbDevice &dev, uint8_t interface_num, uint8_t alt_setting, const transfer_cb_t &callback);
#endif

  // -- Isochronous support -----------------------------------------------------
  // Compiled only when a component calls usb_host.require_isoc_transfers() from its
  // to_code(); without that, these members do not exist and calls fail to compile.
#ifdef USE_USB_ISOC_TRANSFERS
  usb_transfer_t *isoc_alloc(uint8_t ep_addr, uint16_t mps, uint8_t num_packets, usb_transfer_cb_t callback,
                             void *context);
  bool isoc_submit(usb_transfer_t *xfer);
  void isoc_free(usb_transfer_t *xfer);

  // Starts the open sequence. A true return only means it started: an owning stream has to
  // select its alt-setting first, which is a control transfer. on_stream_open() reports the
  // outcome, and is_running() tells whether the stream is carrying data.
  //
  // Callback contract: once the open is accepted, on_stream_open() is called exactly once,
  // whether the sequence succeeds or fails, so a state machine may key off it alone. The
  // two precondition checks (invalid parameters, stream not closed yet) reject the call
  // before it is accepted and report through the return value only -- they say nothing
  // about the stream that is already there.
  bool stream_open(IsocStream &stream, USBClient *cb);
  /// Several-devices drivers: the same on one of their devices.
  bool stream_open(UsbDevice &dev, IsocStream &stream, USBClient *cb);
  void stream_close(IsocStream &stream);

  // Override in subclass to consume one received isochronous packet (IN endpoints).
  // Called from USB-task context -- must be fast and non-blocking.
  virtual void on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) {}
  // The same with its stream (several-devices drivers: stream.device says whose) and whether the host skipped the
  // packet's (micro)frame (no data, not an error for on_isoc_packet); by default on_isoc_packet().
  virtual void on_stream_packet(IsocStream &stream, const uint8_t *data, size_t len, bool error, bool skipped) {
    this->on_isoc_packet(stream.ep_addr, data, len, error);
  }
  // Override in subclass to produce one isochronous packet (OUT endpoints). max_len is what
  // the sample clock allows this packet to carry; return how many bytes were written. The
  // rest of the packet is zero filled, so a short return is silence rather than stale audio.
  // Called from USB-task context -- must be fast and non-blocking.
  virtual size_t on_isoc_fill(uint8_t ep_addr, uint8_t *data, size_t max_len) { return 0; }
  virtual void on_isoc_out_error(uint8_t ep_addr) {}
  virtual size_t on_stream_fill(IsocStream &stream, uint8_t *data, size_t max_len) {
    return this->on_isoc_fill(stream.ep_addr, data, max_len);
  }
  virtual void on_stream_out_error(IsocStream &stream) { this->on_isoc_out_error(stream.ep_addr); }
  // Called on the main loop once the open sequence finished, successfully or not.
  virtual void on_stream_open(IsocStream &stream, bool ok) {}
#endif

  // Bytes a single transfer of this client can carry. Sized from the attached device's
  // descriptors, so it differs between a device on the high-speed controller and one on the
  // full-speed controller, and is only meaningful while a device is attached.
  size_t transfer_capacity() const {
    return this->requests_[0].transfer != nullptr ? this->requests_[0].transfer->data_buffer_size : 0;
  }

 protected:
  // Routing, called by USBHost on the main loop.
  bool has_id_filter_() const {
    return this->vid_ != 0 || this->pid_ != 0 || this->manufacturer_filter_ != nullptr ||
           this->product_filter_ != nullptr;
  }
  bool is_free_() const { return this->accepts_more_devices_() || this->state_ == USB_CLIENT_INIT; }
  /// A driver that serves any number of devices at once (no device-specific configuration needed or one
  /// it derives): each device gets a UsbDevice; the single-device members below stay unused for it.
  virtual bool accepts_more_devices_() const { return false; }
  /// Several-devices drivers: a device opened (descriptors read, transfer pool ready) - main loop.
  virtual void on_device_connected(UsbDevice &dev) {}
  /// Several-devices drivers: a device is gone or being closed - stop its endpoints and release its
  /// interfaces here; nothing new may be submitted for it afterwards. Its context stays valid until
  /// free_device_context(), which comes once no transfer of it is out any more.
  virtual void on_device_disconnected(UsbDevice &dev) {}
  /// Several-devices drivers: the device's memory goes now (main loop) - free what `context` points to.
  virtual void free_device_context(UsbDevice &dev) {}
  /// Several-devices drivers: the device's context is still used beyond its transfer pool (an isochronous stream
  /// still draining); its memory waits, the stream's close wakes the loop.
  virtual bool device_busy_(const UsbDevice &dev) const { return false; }
  virtual bool starting_() const { return false; }
  bool reap_devices_();
  void bind_device_(uint8_t addr, uint32_t interfaces);
  bool open_devices_();
  bool open_device_(UsbDevice &dev);
  void drop_device_(UsbDevice *dev);
  /// The device is on this driver's blacklist (false when no configuration uses a blacklist).
  bool blacklisted_(const usb_device_desc_t *desc) const {
    if constexpr (USB_BLACKLIST_USED) {
      for (uint8_t i = 0; i < this->blacklist_count_; i++) {
        if (this->blacklist_[2 * i] == desc->idVendor && this->blacklist_[2 * i + 1] == desc->idProduct)
          return true;
      }
    }
    return false;
  }
  /// The interfaces of the device this driver leaves alone (0 when no configuration lists any).
  uint32_t blacklisted_interfaces_(const usb_device_desc_t *desc) const {
    if constexpr (USB_INTERFACE_BLACKLIST_USED) {
      const uint32_t id = (static_cast<uint32_t>(desc->idVendor) << 16) | desc->idProduct;
      for (uint8_t i = 0; i < this->interface_blacklist_count_; i++) {
        if (this->interface_blacklist_[2 * i] == id)
          return this->interface_blacklist_[2 * i + 1];
      }
    }
    return 0;
  }
  // How this driver matches the device, and which of its interfaces it would serve.
  MatchTier match_(const RoutedDevice &dev, uint32_t &interfaces) const;
  MatchTier match_all_(const RoutedDevice &dev, uint32_t &interfaces) const;
  /// Endpoints of this alternate setting the driver never uses: its claim opens no pipe (and holds no host
  /// channel) for them. Bit (bEndpointAddress & 0x0F), plus 16 for an IN endpoint. Decided from the
  /// descriptors alone: the host channel budget asks the same while the device is still being enumerated.
  virtual uint32_t skipped_endpoints_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                                      const usb_intf_desc_t *alt) const {
    return 0;
  }
  /// Host channels this driver's claims of an interface take at most: the endpoints of its widest
  /// alternate setting, without the skipped ones.
  uint8_t interface_channels_(const usb_device_desc_t *desc, const usb_config_desc_t *config,
                              uint8_t interface_num) const;
  /// A driver's own say on a class or vendor match: NONE to pass the device by, NAMED when
  /// its configuration names it after all (a usb_storage mount with the stick's vid/pid).
  virtual MatchTier refine_match_(const RoutedDevice &dev, MatchTier tier) const { return tier; }
  void bind_(uint8_t addr, uint32_t interfaces);
  /// Whether routing handed this interface of the current device to this driver.
  bool interface_routed_(uint8_t interface_num) const {
    return interface_num < 32 && (this->routed_interfaces_ & (1u << interface_num)) != 0;
  }

  bool process_usb_events_();
  void handle_open_state_();
  // Largest transfer buffer this device can need: the widest wMaxPacketSize it declares,
  // including its control endpoint, and never below USB_MIN_TRANSFER_BUFFER.
  size_t transfer_buffer_size_() const;
  bool alloc_transfer_pool_();
  void free_transfer_pool_();
  TransferRequest *get_trq_();
  virtual void disconnect();
  virtual void on_connected() {}

  /// Whether the subclass reports the device as connected itself, once its own setup of
  /// the device has finished, rather than as soon as the device has been opened
  virtual bool reports_connection_itself() const { return false; }
  /// Report the claimed device to the connection callbacks. Idempotent; a subclass that
  /// reports itself calls this once the device is ready to use.
  void report_connected_();
  virtual void on_disconnected() { this->trq_in_use_.store(0); }

#ifdef USE_USB_HOST_SHARED_TASK
  enum class Registration : uint8_t { READY, WAIT, FAIL };
  static constexpr uint8_t RELEASE_NONE = 0;
  static constexpr uint8_t RELEASE_WANTED = 1;
  static constexpr uint8_t RELEASE_DONE = 2;
  static constexpr uint8_t RELEASE_FAILED = 3;
  Registration ensure_registered_();
  void release_if_idle_();
  bool unopened_devices_() const;
#else
  static void usb_task_fn(void *arg);
  [[noreturn]] void usb_task_loop_() const;
#endif
  static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);

  TransferRequest requests_[MAX_REQUESTS]{};
#ifdef USE_USB_HOST_SHARED_TASK
  std::atomic<uint8_t> release_{RELEASE_NONE};
  bool registered_{false};
#else
  TaskHandle_t usb_task_handle_{nullptr};
#endif
  usb_host_client_handle_t handle_{};
  usb_device_handle_t device_handle_{};
  const usb_device_desc_t *device_desc_{nullptr};
  const usb_config_desc_t *config_desc_{nullptr};
  int device_addr_{-1};
  int state_{USB_CLIENT_INIT};
  LazyCallbackManager<void(bool)> connection_callback_;
  // Lock-free pool management, no dynamic allocation: bit i set means requests_[i] is in
  // use. Both threads allocate and deallocate, hence the atomic (see the header comment).
  std::atomic<trq_bitmask_t> trq_in_use_;
  const char16_t *manufacturer_filter_{nullptr};
  const char16_t *product_filter_{nullptr};
  uint16_t vid_{};
  uint16_t pid_{};
  std::vector<std::unique_ptr<UsbDevice>> devices_{};  // several-devices drivers: one per device
  std::vector<std::unique_ptr<UsbDevice>> dead_devices_{};  // closed, transfers still out
  const uint16_t *blacklist_{nullptr};  // vid, pid pairs in flash (set_blacklist)
  uint8_t blacklist_count_{0};
  const uint32_t *interface_blacklist_{nullptr};  // vid << 16 | pid, interface mask (set_interface_blacklist)
  uint8_t interface_blacklist_count_{0};
  std::vector<UsbMatch> class_matches_{};
  std::vector<UsbMatch> vendor_matches_{};
  uint32_t routed_interfaces_{0};
  // Resolved once in setup(); the forwarders below would otherwise dereference the global
  // accessor unchecked on every call, including on the RX hot path.
  USBHost *host_{nullptr};
  // Whether the connection callbacks were told about the current device, so a removal is
  // only ever reported for a device that was reported connected
  bool connection_reported_{false};

  const usb_device_desc_t *get_device_desc_() const { return this->device_desc_; }
  const usb_config_desc_t *get_config_desc_() const { return this->config_desc_; }
};
// -----------------------------------------------------------------------------
// USBHost -- USB host stack + stateless transfer submission engine.
//
// Acts as the Linux host controller driver: owns usb_host_install(), the lib
// event loop, and all ESP-IDF transfer submission calls.  Never touches client
// memory -- clients own their TransferRequest pools and hand filled slots here.
// -----------------------------------------------------------------------------
class USBHost final : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::BUS; }
  void loop() override;
  void setup() override;

  // Which ESP32-P4 controllers run as host (controllers option): usb_host_config_t's
  // peripheral_map, BIT0 high-speed, BIT1 full-speed. Only the patched espressif/usb component
  // sets up a PHY per controller (IDF's 5.x built-in sets up one), hence the guards at the use
  // site and in codegen.
  void set_peripheral_map(uint8_t map) { this->peripheral_map_ = map; }
#ifdef USE_USB_HOST_CHANNEL_BUDGET
  /// Host channels of a controller (0 high-speed, 1 full-speed): allocated now, committed to the configured devices,
  /// the hardware's total. False while the controller has no port (not in use).
  static bool channel_usage(uint8_t controller, int &used, int &committed, int &total) {
    return usb_host_channel_usage(controller, &used, &committed, &total) == ESP_OK;
  }
#endif
  // Which of the ESP32-P4's two internal FSLS PHYs the full-speed OTG controller uses:
  // 0 is GPIO24/25, 1 is GPIO26/27 (the reset default). Set from the fs_pins option.
  void set_fs_phy_index(uint8_t index) { this->fs_phy_index_ = index; }
#ifdef USE_USB_HOST_PER_PORT_FIFO_BIAS
  // How one host controller divides its hardware FIFO between received data, non-periodic
  // OUT and periodic OUT. The division caps the largest packet each direction can carry, so
  // a controller carrying a wide isochronous OUT stream and one carrying high-speed bulk
  // need different divisions. ESP-IDF's own option applies one division to every
  // controller; the patched component this build uses takes one per controller, indexed
  // like peripheral_map. Values are usb_host_fifo_bias_t; 0 keeps the Kconfig default.
  void set_port_fifo_bias(uint8_t peripheral_index, uint8_t bias) {
    if (peripheral_index < SOC_USB_OTG_PERIPH_NUM)
      this->port_fifo_bias_[peripheral_index] = bias;
  }
  // The high-speed controller as a full/low-speed host (full_speed_only option).
  void set_full_speed_only(bool full_speed_only) { this->full_speed_only_ = full_speed_only; }
#endif

  // -- Submission engine (called by USBClient thin forwarders) ----------------

  // Bulk / interrupt IN and OUT -- always compiled if any client uses them
  bool submit_transfer(TransferRequest *trq);

  // Control transfer submission -- guarded
#ifdef USE_USB_CONTROL_TRANSFERS
  bool submit_control(usb_host_client_handle_t client_handle, TransferRequest *trq);
#endif

  // Interface claim / release -- always needed
  bool do_claim_interface(USBClient *client, usb_host_client_handle_t client_handle, usb_device_handle_t device_handle,
                          uint8_t interface_num, uint8_t alt_setting);
  bool do_release_interface(usb_host_client_handle_t client_handle, usb_device_handle_t device_handle,
                            uint8_t interface_num);

  // -- Isochronous -------------------------------------------------------------
#ifdef USE_USB_ISOC_TRANSFERS
  usb_transfer_t *do_isoc_alloc(uint8_t ep_addr, usb_device_handle_t device_handle, uint16_t mps, uint8_t num_packets,
                                usb_transfer_cb_t callback, void *context);
  bool do_isoc_submit(usb_transfer_t *xfer);
  void do_isoc_free(usb_transfer_t *xfer);

  bool stream_open(IsocStream &stream, USBClient *cb, usb_host_client_handle_t client_handle,
                   usb_device_handle_t device_handle);
  void stream_close(IsocStream &stream);

  // Second half of the open sequence: allocate the URBs, seed an OUT stream with silence
  // and submit. Split out so it can run either straight from stream_open() or from the
  // SET_INTERFACE completion.
  bool stream_start_urbs(IsocStream &stream, USBClient *cb, usb_device_handle_t device_handle);
  // The device a stream runs on, now: its UsbDevice's handle or the client's (null once closed).
  static usb_device_handle_t stream_device_handle_(const IsocStream &stream, const USBClient *client);
  // Give back whatever the stream holds of its interface. Does nothing for a stream that
  // shares its interface with another one, and skips the alt-setting reset when there is
  // nothing to reset.
  void stream_release(IsocStream &stream, USBClient *cb, usb_host_client_handle_t client_handle,
                      usb_device_handle_t device_handle);

  // Static trampoline stored as xfer->callback for every ISOC URB.
  // Iterates isoc_packet_desc[], calls client->on_isoc_packet() per packet, resubmits.
  static void isoc_cb(usb_transfer_t *xfer);
#endif

  // -- Routing ------------------------------------------------------------------
  // Every driver registers in its setup(). USBHost opens each new device once, reads its
  // descriptors and hands it, interface by interface, to the drivers that match it.
  void register_client(USBClient *client);
#ifdef USE_USB_HOST_SHARED_TASK
  bool attach_client(USBClient *client);
  void release_client(USBClient *client);
#endif
  // A driver let its device go: declined it, lost it, or failed to open it.
  void client_released(USBClient *client, int addr);
  /// The devices open now (main loop only).
  const std::vector<RoutedDevice> &get_devices() const { return this->devices_; }
  /// Main loop: called after a device came or went.
  template<typename F> void add_on_devices_callback(F &&callback) {
    this->devices_callback_.add(std::forward<F>(callback));
  }
  /// Main loop: the device at addr goes to `taker` alone (a driver that redirects whole devices), away
  /// from the drivers that hold it, which let go as on an unplug; they get it back once the taker lets
  /// it go. The taker is bound (bind_) once they have. false: not there, being switched, or taken by
  /// another.
  bool take_device(USBClient *taker, uint8_t addr);

  // -- Mode switching (mode_switch option) ---------------------------------------
  void add_mode_switch_control(uint16_t vid, uint16_t pid, uint8_t request_type, uint8_t request, uint16_t value,
                               uint16_t index, std::vector<uint8_t> data) {
    this->mode_switches_.push_back({vid, pid, request_type, request, value, index, std::move(data), {}, false, 0});
  }
  void add_mode_switch_bulk(uint16_t vid, uint16_t pid, std::vector<std::vector<uint8_t>> messages,
                            bool need_response) {
    this->mode_switches_.push_back({vid, pid, 0, 0, 0, 0, {}, std::move(messages), need_response, 0});
  }

 protected:
  static void host_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);
  void device_new_(uint8_t addr);
  void device_gone_(usb_device_handle_t handle);
  void route_(RoutedDevice &dev);
  RoutedDevice *find_device_(usb_device_handle_t handle);
  /// Enumeration (the IDF, from usb_host_lib_handle_events() in loop()): the host channels a device takes
  /// once configured - its default pipe and every pipe its drivers will open, streams opened only while
  /// used included. A device that does not fit on its controller is not configured (patched espressif/usb).
#ifdef USE_USB_HOST_CHANNEL_BUDGET
  static int channel_budget_cb(const usb_device_desc_t *desc, const usb_config_desc_t *config, void *arg);
  int channel_need_(const usb_device_desc_t *desc, const usb_config_desc_t *config) const;
#endif
  bool start_mode_switch_(RoutedDevice &dev);
  bool submit_switch_step_(RoutedDevice &dev);
  static void switch_cb(usb_transfer_t *xfer);
  void finish_mode_switch_(usb_device_handle_t handle);
  void forget_device_(usb_device_handle_t handle);
  void continue_take_(RoutedDevice &dev);
#ifdef USE_USB_HOST_SHARED_TASK
  static void client_task_fn(void *arg);
  [[noreturn]] void client_task_loop_();
  TaskHandle_t client_task_{nullptr};
  std::atomic<USBClient *> active_[MAX_ACTIVE_CLIENTS]{};
#endif

  std::vector<USBClient *> clients_{};
  std::vector<ModeSwitch> mode_switches_{};
  usb_host_client_handle_t client_handle_{nullptr};
  std::vector<RoutedDevice> devices_{};
  LazyCallbackManager<void()> devices_callback_;
  bool reroute_{false};
  uint8_t peripheral_map_{0};  // 0: ESP-IDF's default, the high-speed controller
  uint8_t fs_phy_index_{1};
#ifdef USE_USB_HOST_PER_PORT_FIFO_BIAS
  uint8_t port_fifo_bias_[SOC_USB_OTG_PERIPH_NUM]{};
  bool full_speed_only_{false};
#endif
};

// Returns the global USBHost singleton, set during USBHost::setup().
USBHost *get_usb_host();

}  // namespace esphome::usb_host

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
