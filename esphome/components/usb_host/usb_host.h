#pragma once

// Should not be needed, but it's required to pass CI clang-tidy checks
#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32P4)
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include <vector>
#include <set>
#include "usb/usb_host.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esphome/core/lock_free_queue.h"
#include "esphome/core/event_pool.h"
#include <atomic>

namespace esphome::usb_host {

// THREADING MODEL:
// This component uses a dedicated USB task for event processing to prevent data loss.
// - USB Task (high priority): Handles USB events, executes transfer callbacks, releases transfer slots
// - Main Loop Task: Initiates transfers, processes device connect/disconnect events
//
// Thread-safe communication:
// - Lock-free queues for USB task -> main loop events (SPSC pattern)
// - Lock-free TransferRequest pool using atomic bitmask (MCMP pattern - multi-consumer, multi-producer)
//
// TransferRequest pool access pattern:
// - get_trq_() [allocate]: Called from BOTH USB task and main loop threads
//   * USB task: via USB UART input callbacks that restart transfers immediately
//   * Main loop: for output transfers and flow-controlled input restarts
// - release_trq() [deallocate]: Called from BOTH USB task and main loop threads
//   * USB task: immediately after transfer callback completes (critical for preventing slot exhaustion)
//   * Main loop: when transfer submission fails
//
// The multi-threaded allocation/deallocation is intentional for performance:
// - USB task can immediately restart input transfers and release slots without context switching
// - Main loop controls backpressure by deciding when to restart after consuming data
// The atomic bitmask ensures thread-safe allocation/deallocation without mutex blocking.

static const char *const TAG = "usb_host";

// Forward declarations
struct TransferRequest;
class USBClient;
class USBHost;

// NEW: Interface for class-based device handlers (e.g., MSC, HID)
// Operates in parallel to VID/PID-based USBClient system
class USBDeviceHandler {
 public:
  virtual ~USBDeviceHandler() = default;
  // Check if this handler supports the device based on interface descriptors
  virtual bool matches_device(const usb_config_desc_t *config_desc) = 0;
  // Called when device is matched and claimed by this handler
  virtual void on_device_connected(usb_device_handle_t device_handle, uint8_t addr) = 0;
  // Called when device is disconnected
  virtual void on_device_disconnected(usb_device_handle_t device_handle) = 0;
};

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

#ifdef USE_ESP32_VARIANT_ESP32P4
static constexpr uint16_t USB_MAX_PACKET_SIZE = 512;
#else
static constexpr uint8_t USB_MAX_PACKET_SIZE = 64;
#endif

static constexpr size_t MAX_REQUESTS = USB_HOST_MAX_REQUESTS;  // maximum number of outstanding requests possible.
static_assert(MAX_REQUESTS >= 1 && MAX_REQUESTS <= 32, "MAX_REQUESTS must be between 1 and 32");

// Select appropriate bitmask type for tracking allocation of TransferRequest slots.
// The bitmask must have at least as many bits as MAX_REQUESTS, so:
// - Use uint16_t for up to 16 requests (MAX_REQUESTS <= 16)
// - Use uint32_t for 17-32 requests (MAX_REQUESTS > 16)
// This is tied to the static_assert above, which enforces MAX_REQUESTS is between 1 and 32.
// If MAX_REQUESTS is increased above 32, this logic and the static_assert must be updated.
using trq_bitmask_t = std::conditional<(MAX_REQUESTS <= 16), uint16_t, uint32_t>::type;
static constexpr trq_bitmask_t ALL_REQUESTS_IN_USE = MAX_REQUESTS == 32 ? ~0 : (1 << MAX_REQUESTS) - 1;

static constexpr size_t USB_EVENT_QUEUE_SIZE = 32;  // Size of event queue between USB task and main loop
#ifndef USB_TASK_STACK_SIZE
static constexpr size_t USB_TASK_STACK_SIZE = 4096;  // Stack size for USB task
#endif
static constexpr UBaseType_t USB_TASK_PRIORITY = 5;  // Higher priority than main loop (tskIDLE_PRIORITY + 5)

// used to report a transfer status
struct TransferStatus {
  uint8_t *data;
  size_t data_len;
  void *user_data;
  uint16_t error_code;
  uint8_t endpoint;
  bool success;
};

using transfer_cb_t = std::function<void(const TransferStatus &)>;

class USBClient;

// struct used to capture all data needed for a transfer
struct TransferRequest {
  usb_transfer_t *transfer;
  transfer_cb_t callback;
  TransferStatus status;
  USBClient *client;
};

enum EventType : uint8_t {
  EVENT_DEVICE_NEW,
  EVENT_DEVICE_GONE,
};

struct UsbEvent {
  EventType type;
  union {
    struct {
      uint8_t address;
    } device_new;
    struct {
      usb_device_handle_t handle;
    } device_gone;
  } data;

  // Required for EventPool - no cleanup needed for POD types
  void release() {}
};

// callback function type.

enum ClientState {
  USB_CLIENT_INIT = 0,
  USB_CLIENT_OPEN,
  USB_CLIENT_CLOSE,
  USB_CLIENT_GET_DESC,
  USB_CLIENT_GET_INFO,
  USB_CLIENT_CONNECTED,
};
class USBClient : public Component, public Parented<USBHost> {
  friend class USBHost;

 public:
  USBClient(uint16_t vid, uint16_t pid) : trq_in_use_(0), vid_(vid), pid_(pid) {}
  void setup() override;
  void loop() override;
  // setup must happen after the host bus has been setup
  float get_setup_priority() const override { return setup_priority::IO; }
  void on_opened(uint8_t addr);
  void on_removed(usb_device_handle_t handle);
  bool transfer_in(uint8_t ep_address, const transfer_cb_t &callback, uint16_t length);
  bool transfer_out(uint8_t ep_address, const transfer_cb_t &callback, const uint8_t *data, uint16_t length);
  void dump_config() override;
  void release_trq(TransferRequest *trq);
  trq_bitmask_t get_trq_in_use() const { return trq_in_use_; }
  bool control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index, const transfer_cb_t &callback,
                        const std::vector<uint8_t> &data = {});

  // Lock-free event queue and pool for USB task to main loop communication
  // Must be public for access from static callbacks
  LockFreeQueue<UsbEvent, USB_EVENT_QUEUE_SIZE> event_queue;
  EventPool<UsbEvent, USB_EVENT_QUEUE_SIZE> event_pool;

 protected:
  // Process USB events from the queue. Returns true if any work was done.
  // Subclasses should call this instead of USBClient::loop() to combine
  // with their own work check for a single disable_loop() decision.
  bool process_usb_events_();
  void handle_open_state_();
  TransferRequest *get_trq_();  // Lock-free allocation using atomic bitmask (multi-consumer safe)
  virtual void disconnect();
  virtual void on_connected() {}
  virtual void on_disconnected() {
    // Reset all requests to available (all bits to 0)
    this->trq_in_use_.store(0);
  }

  // USB task management
  static void usb_task_fn(void *arg);
  [[noreturn]] void usb_task_loop() const;

  // Members ordered to minimize struct padding on 32-bit platforms
  TransferRequest requests_[MAX_REQUESTS]{};
  TaskHandle_t usb_task_handle_{nullptr};
  usb_host_client_handle_t handle_{};
  usb_device_handle_t device_handle_{};
  int device_addr_{-1};
  int state_{USB_CLIENT_INIT};
  // Lock-free pool management using atomic bitmask (no dynamic allocation)
  // Bit i = 1: requests_[i] is in use, Bit i = 0: requests_[i] is available
  // Supports multiple concurrent consumers and producers (both threads can allocate/deallocate)
  std::atomic<trq_bitmask_t> trq_in_use_;
  uint16_t vid_{};
  uint16_t pid_{};
};
class USBHost : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::BUS; }
  void loop() override;
  void setup() override;

  // NEW: Device claiming system for coordination between VID/PID clients and interface-class handlers
  bool try_claim_device(uint8_t address);
  void release_device(uint8_t address);

  // NEW: Register interface-class based handlers (e.g., MSC, HID)
  void register_device_handler(USBDeviceHandler *handler);

  // NEW: Try to dispatch device to interface-class handlers (called when not claimed by VID/PID)
  void try_dispatch_to_handlers(uint8_t address);

  // NEW: Close a device handle (for handlers that need to re-open with different client)
  void close_device_handle(usb_device_handle_t device_handle);

  // Device whitelist management
  void add_device_to_whitelist(uint16_t vid, uint16_t pid);
  bool is_device_whitelisted(uint16_t vid, uint16_t pid) const;

#ifdef USE_USB_HOST_DUAL_INSTANCE
  // Dual USB Host support (ESP32-P4 only)
  void set_controller_type(uint8_t controller) { this->controller_type_ = controller; }
  void *get_tuh_instance() const { return this->tuh_instance_; }
#endif

 protected:
  std::vector<USBClient *> clients_{};                             // EXISTING: VID/PID based clients
  std::vector<USBDeviceHandler *> handlers_{};                     // NEW: Interface-class based handlers
  std::set<uint8_t> claimed_devices_{};                            // NEW: Track devices claimed by VID/PID clients
  usb_host_client_handle_t coordinator_handle_{};                  // NEW: Handle for handler dispatch
  std::vector<std::pair<uint16_t, uint16_t>> device_whitelist_{};  // Whitelist of allowed devices (VID, PID)

#ifdef USE_USB_HOST_DUAL_INSTANCE
  uint8_t controller_type_{0};   // 0 = FS, 1 = HS
  void *tuh_instance_{nullptr};  // TinyUSB instance handle (void* to avoid including TinyUSB headers)
#endif
};

}  // namespace esphome::usb_host

#endif  // USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
