#pragma once

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/usb_host/usb_host.h"
#include "esphome/core/lock_free_queue.h"
#include "esphome/core/event_pool.h"
#include "esphome/core/log.h"
#include <atomic>
#include <functional>

namespace esphome::usb_uart {

class USBUartTypeCdcAcm;
class USBUartTypeCH934X;
class USBUartClient;
class USBUartComponent;
class USBUartChannel;
class USBUartChannelBase;
class USBUartTypePL2303;

ESPHOME_LOG_TAG(TAG, "usb_uart");

static constexpr uint8_t USB_CDC_SUBCLASS_ACM = 0x02;
static constexpr uint8_t USB_SUBCLASS_COMMON = 0x02;
static constexpr uint8_t USB_SUBCLASS_NULL = 0x00;
static constexpr uint8_t USB_PROTOCOL_NULL = 0x00;
static constexpr uint8_t USB_DEVICE_PROTOCOL_IAD = 0x01;
static constexpr uint8_t USB_VENDOR_IFC = usb_host::USB_TYPE_VENDOR | usb_host::USB_RECIP_INTERFACE;
static constexpr uint8_t USB_VENDOR_DEV = usb_host::USB_TYPE_VENDOR | usb_host::USB_RECIP_DEVICE;

struct CdcEps {
  const usb_ep_desc_t *notify_ep;
  const usb_ep_desc_t *in_ep;
  const usb_ep_desc_t *out_ep;
  // 0xFF marks a channel that was never matched to a CDC function on the device
  uint8_t bulk_interface_number{0xFF};
  // Also the wIndex target for CDC class requests (SET_LINE_CODING etc.), so it
  // must remain valid even when the interface itself is not claimed.
  uint8_t interrupt_interface_number{0xFF};
  bool interrupt_interface_claimed{false};
  // bInterfaceClass of each interface above, which decides how a host binds the port
  uint8_t bulk_interface_class{0};
  uint8_t interrupt_interface_class{0};
};

enum CH34xChipType : uint8_t {
  CHIP_CH342F = 0,
  CHIP_CH342K,
  CHIP_CH343GP,
  CHIP_CH343G_AUTOBAUD,
  CHIP_CH343K,
  CHIP_CH343J,
  CHIP_CH344L,
  CHIP_CH344L_V2,
  CHIP_CH344Q,
  CHIP_CH347TF,
  CHIP_CH9101UH,
  CHIP_CH9101RY,
  CHIP_CH9102F,
  CHIP_CH9102X,
  CHIP_CH9103M,
  CHIP_CH9104L,
  CHIP_CH340B,
  CHIP_CH339W,
  CHIP_CH9111L_M0,
  CHIP_CH9111L_M1,
  CHIP_CH9114L,
  CHIP_CH9114W,
  CHIP_CH9114F,
  CHIP_CH346C_M0,
  CHIP_CH346C_M1,
  CHIP_CH346C_M2,
  CHIP_UNKNOWN = 0xFF,
};

struct Ch934xEps {
  const usb_ep_desc_t *in_ep{nullptr};
  const usb_ep_desc_t *out_ep{nullptr};
  const usb_ep_desc_t *ep_cmd_read{nullptr};
  const usb_ep_desc_t *ep_cmd_write{nullptr};
  uint8_t data_interface{0};
};

// clang-format off
enum CH934xChipType : uint8_t {
  CHIP_CH9344L = 0,
  CHIP_CH9344Q,
  CHIP_CH348L,
  CHIP_CH348Q,
  CHIP_CH934X_UNKNOWN = 0xFF,
};
// clang-format on

// 1 and 2 are the stop bit count the uart component uses, so a count set through the
// UARTComponent API (as serial_proxy does) means the same here. 1.5 has no count.
enum UARTStopBitsOptions {
  UART_CONFIG_STOP_BITS_1 = 1,
  UART_CONFIG_STOP_BITS_2 = 2,
  UART_CONFIG_STOP_BITS_1_5 = 3,
};

// Stop bits and parity as adapters take them on the wire: CDC SET_LINE_CODING, and the
// CP210x, FTDI and PL2303 requests, all use these values
enum class LineStopBits : uint8_t {
  LINE_STOP_BITS_1 = 0,
  LINE_STOP_BITS_1_5 = 1,
  LINE_STOP_BITS_2 = 2,
};

enum class LineParity : uint8_t {
  LINE_PARITY_NONE = 0,
  LINE_PARITY_ODD = 1,
  LINE_PARITY_EVEN = 2,
  LINE_PARITY_MARK = 3,
  LINE_PARITY_SPACE = 4,
};

// Indexed by LineParity
static const char *const PARITY_NAMES[] = {"NONE", "ODD", "EVEN", "MARK", "SPACE"};
// Indexed by LineStopBits
static const char *const STOP_BITS_NAMES[] = {"1", "1.5", "2"};

/// What the config state machine is applying
enum class ConfigMode : uint8_t {
  CONFIG_MODE_INIT,    ///< Full initialisation of every channel after the device connected
  CONFIG_MODE_RELOAD,  ///< Re-apply the line settings of one already-open channel
  CONFIG_MODE_MODEM,   ///< Apply the DTR/RTS state of one already-open channel
};

class RingBuffer {
 public:
  RingBuffer(uint16_t buffer_size) : buffer_size_(buffer_size), buffer_(nullptr) {}
  bool allocate() {
    this->buffer_ = new (std::nothrow) uint8_t[this->buffer_size_];
    return this->buffer_ != nullptr;
  }
  void free_buffer() {
    delete[] this->buffer_;
    this->buffer_ = nullptr;
    this->read_pos_ = this->insert_pos_ = 0;
  }
  bool has_buffer() const { return this->buffer_ != nullptr; }
  bool is_empty() const { return this->buffer_ == nullptr || this->read_pos_ == this->insert_pos_; }
  size_t get_available() const {
    if (this->buffer_ == nullptr)
      return 0;
    return (this->insert_pos_ + this->buffer_size_ - this->read_pos_) % this->buffer_size_;
  };
  size_t get_free_space() const {
    if (this->buffer_ == nullptr)
      return 0;
    return this->buffer_size_ - 1 - this->get_available();
  }
  uint8_t peek() const { return this->buffer_[this->read_pos_]; }
  void push(uint8_t item);
  void push(const uint8_t *data, size_t len);
  uint8_t pop();
  size_t pop(uint8_t *data, size_t len);
  void clear() { this->read_pos_ = this->insert_pos_ = 0; }

 protected:
  uint16_t insert_pos_ = 0;
  uint16_t read_pos_ = 0;
  uint16_t buffer_size_;
  uint8_t *buffer_;
};

// Structure for queuing received USB data chunks.
//
// The payload is not stored inline. One chunk holds one packet of the IN endpoint, and
// wMaxPacketSize is a property of that endpoint: 64 bytes on a full-speed device, 512 on a
// high-speed one, and on a chip with two host controllers both can be attached at the same
// time. A compile-time array would force the endpoint to fit the buffer, which is backwards.
// The payload therefore points into a slab that RxChunkPool sizes from the endpoint once the
// device is open.
struct UsbDataChunk {
  uint8_t *data;
  uint16_t length;
  USBUartChannelBase *channel;
};

// Structure for queuing outgoing USB data chunks. Unlike IN, a bulk OUT transfer may be any
// length up to the host's transfer buffer -- a short packet is legal and simply ends the
// transfer -- so an OUT chunk does not have to hold a whole wMaxPacketSize and stays a
// compile-time buffer.
struct UsbOutputChunk {
  static constexpr size_t MAX_CHUNK_SIZE = usb_host::USB_MAX_PACKET_SIZE;
  uint8_t data[MAX_CHUNK_SIZE];
  uint16_t length;

  // Required for EventPool - no cleanup needed for POD types
  void release() {}
};

// Fixed set of receive chunks whose payloads are equal slices of a single allocation.
//
// The slice size is the IN endpoint's wMaxPacketSize, so the storage cannot exist before a
// device is attached: it is allocated when the endpoints are known and released once the
// queue has drained after a disconnect. Slots are handed out with the same atomic bitmask as
// the USB host's transfer pool, because allocate() runs in USB-task context while release()
// runs on the main loop.
template<uint8_t COUNT> class RxChunkPool {
  static_assert(COUNT >= 1 && COUNT <= 32, "RxChunkPool tracks its slots in a 32 bit mask");

 public:
  bool allocate_storage(uint16_t slice_size) {
    this->free_storage();
    if (slice_size == 0)
      return false;
    RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
    this->base_ = allocator.allocate(static_cast<size_t>(slice_size) * COUNT);
    if (this->base_ == nullptr)
      return false;
    this->slice_size_ = slice_size;
    for (uint8_t i = 0; i < COUNT; i++)
      this->chunks_[i].data = this->base_ + static_cast<size_t>(i) * slice_size;
    return true;
  }

  // Only safe once nothing holds a chunk any more: the queue is drained and the USB task is
  // no longer submitting. See the deferred free in USBUartComponent::loop().
  void free_storage() {
    if (this->base_ != nullptr) {
      RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
      allocator.deallocate(this->base_, static_cast<size_t>(this->slice_size_) * COUNT);
      this->base_ = nullptr;
    }
    for (auto &chunk : this->chunks_)
      chunk.data = nullptr;
    this->slice_size_ = 0;
    this->in_use_.store(0, std::memory_order_release);
  }

  bool has_storage() const { return this->base_ != nullptr; }
  uint16_t slice_size() const { return this->slice_size_; }

  UsbDataChunk *allocate() {
    if (this->base_ == nullptr)
      return nullptr;
    uint32_t mask = this->in_use_.load(std::memory_order_acquire);
    for (;;) {
      if (mask == ALL_IN_USE)
        return nullptr;
      const uint32_t lsb = ~mask & (mask + 1);
      if (this->in_use_.compare_exchange_weak(mask, mask | lsb, std::memory_order_acquire))
        return &this->chunks_[__builtin_ctz(lsb)];
    }
  }

  void release(UsbDataChunk *chunk) {
    if (chunk == nullptr)
      return;
    const size_t index = chunk - this->chunks_;
    if (index >= COUNT)
      return;
    this->in_use_.fetch_and(~(static_cast<uint32_t>(1) << index), std::memory_order_release);
  }

 protected:
  static constexpr uint32_t ALL_IN_USE = COUNT == 32 ? ~0U : (1U << COUNT) - 1;
  UsbDataChunk chunks_[COUNT]{};
  std::atomic<uint32_t> in_use_{0};
  uint8_t *base_{nullptr};
  uint16_t slice_size_{0};
};

// Common, non-final base for all USB UART channel implementations.
// Concrete channel types (USBUartChannel for CDC-style devices, vendor-specific
// multiplexed channels like CH934X) derive from this and are themselves final,
// per the "configurable classes are final" convention.
class USBUartChannelBase : public uart::UARTComponent, public Parented<USBUartComponent> {
  friend class USBUartClient;
  friend class USBUartComponent;
  friend class USBUartTypeCdcAcm;
  friend class USBUartTypeCP210X;
  friend class USBUartTypeCH34X;
  friend class USBUartTypeCH934X;
  friend class USBUartTypeFT23XX;
  friend class USBUartTypePL2303;
  friend class USBUartChannel;
  friend class CH934XChannel;

 public:
  // Number of output chunk slots per channel, derived from buffer_size config.
  // Computed as ceil(buffer_size / 64) + 1 in Python codegen; defaults to 5 (256 / 64 + 1).
  static constexpr uint8_t USB_OUTPUT_CHUNK_COUNT = USB_UART_OUTPUT_CHUNK_COUNT;

  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override { return this->input_buffer_.get_available(); }
  bool is_connected() override { return this->initialised_.load(); }
  uart::UARTFlushResult flush() override;
  // Re-apply the current line settings (baud, parity, etc) to this already-open channel.
  void load_settings(bool dump_config) override;
  using UARTComponent::load_settings;  // also bring in the no-arg overload for convenience
  void set_debug(bool debug) { this->debug_ = debug; }
  void set_dummy_receiver(bool dummy_receiver) { this->dummy_receiver_ = dummy_receiver; }
  void set_claim_notification_ep(bool claim) { this->claim_notification_ep_ = claim; }
  void set_debug_prefix(const char *prefix) { this->debug_prefix_ = StringRef(prefix); }
#ifdef USE_UART_DEBUGGER
#ifdef UART_DEBUGGER_ADD_SETTINGS
  void set_debug_add_settings(bool add) { this->debug_add_settings_ = add; }
#endif
#endif
  void set_flush_timeout(uint32_t flush_timeout_ms) override { this->flush_timeout_ms_ = flush_timeout_ms; }

  /// Register a callback invoked immediately after data is pushed to the input ring buffer.
  /// Called from USBUartComponent::loop() in the main loop context.
  /// Allows consumers (e.g. ZigbeeProxy) to process bytes in the same loop iteration
  /// they arrive, eliminating one full main-loop-wakeup cycle of latency.
  void set_rx_callback(std::function<void()> cb) { this->rx_callback_ = std::move(cb); }

  /// USB interface number a host driver binds to for this channel: the communication
  /// interface of a CDC ACM function, otherwise the data interface.
  uint8_t get_interface_number() const {
    return this->cdc_dev_.interrupt_interface_number != 0xFF ? this->cdc_dev_.interrupt_interface_number
                                                             : this->cdc_dev_.bulk_interface_number;
  }

  /// bInterfaceClass of the interface `get_interface_number()` names
  uint8_t get_interface_class() const {
    return this->cdc_dev_.interrupt_interface_number != 0xFF ? this->cdc_dev_.interrupt_interface_class
                                                             : this->cdc_dev_.bulk_interface_class;
  }

  /// Index of this port among the ports on its interface. Every driver here exposes one
  /// port per interface; a multiplexed driver would index its channels.
  uint8_t get_port_number() const { return 0; }

  /// Store DTR/RTS and send them to the device. False when not connected.
  bool set_modem_control(bool dtr, bool rts);
  bool get_dtr() const { return this->dtr_; }
  bool get_rts() const { return this->rts_; }

 protected:
  // Not directly instantiable; construct a concrete channel type instead.
  USBUartChannelBase(uint8_t index, uint16_t buffer_size) : input_buffer_(RingBuffer(buffer_size)), index_(index) {}

 public:
  // A channel made for an adapter without a YAML entry is deleted with it
  virtual ~USBUartChannelBase() = default;

 protected:
  void check_logger_conflict() override {}
  LineStopBits line_stop_bits_() const;
  LineParity line_parity_() const;
  // Larger structures first (8+ bytes)
  RingBuffer input_buffer_;
  LockFreeQueue<UsbOutputChunk, USB_OUTPUT_CHUNK_COUNT> output_queue_;
  // Pool sized to queue capacity (SIZE-1) because LockFreeQueue<T,N> is a ring
  // buffer that holds N-1 elements. This guarantees allocate() returns nullptr
  // before push() can fail, preventing a pool slot leak.
  EventPool<UsbOutputChunk, USB_OUTPUT_CHUNK_COUNT - 1> output_pool_;
  std::function<void()> rx_callback_{};
  CdcEps cdc_dev_{};
  StringRef debug_prefix_{};
#ifdef USE_UART_DEBUGGER
#ifdef UART_DEBUGGER_ADD_SETTINGS
  bool debug_add_settings_{false};
#endif
#endif
  // 4-byte fields
  uint32_t flush_timeout_ms_{100};
  // 1-byte fields (no padding between groups)
  std::atomic<bool> input_started_{true};
  std::atomic<bool> output_started_{true};
  std::atomic<bool> initialised_{false};
  std::atomic<bool> destroying_{false};
  const uint8_t index_;
  bool debug_{};
  bool dummy_receiver_{};
  bool claim_notification_ep_{false};
  // Asserted whenever a device connects, as a host driver does when it opens the port
  bool dtr_{true};
  bool rts_{true};
  // Line settings or DTR/RTS changed while the config machine was busy; sent once it is idle
  bool reload_pending_{false};
  bool modem_pending_{false};
};

// Concrete channel type for CDC-style USB serial devices (2 bulk endpoints per
// channel). All shared behavior lives in USBUartChannelBase.
class USBUartChannel final : public USBUartChannelBase {
 public:
  USBUartChannel(uint8_t index, uint16_t buffer_size) : USBUartChannelBase(index, buffer_size) {}
};

/// The serial adapter types; an adapter without a YAML entry gets its type from its vid:pid, else CDC ACM.
enum class UartType : uint8_t { CDC_ACM, CH34X, CH934X, CP210X, FT23XX, PL2303 };

/// An optional YAML entry: an adapter it names (vid / pid) gets its type and its channels (with ids that
/// consumers bind by uart_id).
struct USBUartEntry {
  uint16_t vid;
  uint16_t pid;
  UartType type;
  uint8_t init_lanes;
  bool claim_comm_interface;
  std::vector<USBUartChannelBase *> channels;
};

// One plugged serial adapter: the driver of its type on one UsbDevice of the USBUartClient. Made when the
// adapter connects, deleted once it is gone and none of its transfers is out any more. The client's calls
// for this device are behind the same names the type drivers have always used (transfer_in, disconnect ...).
class USBUartComponent {
  friend class USBUartClient;

 public:
  USBUartComponent(USBUartClient *client, usb_host::UsbDevice *dev);
  virtual ~USBUartComponent();
  virtual void dump_config();
  std::vector<USBUartChannelBase *> get_channels() { return this->channels_; }

  void add_channel(USBUartChannelBase *channel) { this->channels_.push_back(channel); }

  virtual void start_input(USBUartChannelBase *channel);
  void start_output(USBUartChannelBase *channel);

  // Begin configuring all channels (full initialisation). Called from on_connected().
  virtual void enable_channels();
  // Re-apply line settings to a single, already-open channel (used by
  // USBUartChannelBase::load_settings()).
  void apply_channel_settings(USBUartChannelBase *channel);
  // Apply the DTR/RTS state of a single, already-open channel (used by
  // USBUartChannelBase::set_modem_control()).
  void apply_modem_control(USBUartChannelBase *channel);

  // The client's calls for this adapter's device
#ifdef USE_USB_BULK_TRANSFERS
  bool transfer_in(uint8_t ep_address, const usb_host::transfer_cb_t &callback, uint16_t length);
  bool transfer_out(uint8_t ep_address, const usb_host::transfer_cb_t &callback, const uint8_t *data, uint16_t length);
#endif
  bool control_transfer(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                        const usb_host::transfer_cb_t &callback, const std::vector<uint8_t> &data = {});
  void enable_loop();
  void enable_loop_soon_any_context();

  // Called from loop() when input_buffer_ has insufficient space for the incoming chunk.
  // Default is a no-op; override in device-specific subclasses that need resync on overflow.
  virtual void on_rx_overflow(USBUartChannelBase *channel) {}

  // Lock-free data transfer from USB task to main loop
  static constexpr int USB_DATA_QUEUE_SIZE = 32;
  LockFreeQueue<UsbDataChunk, USB_DATA_QUEUE_SIZE> usb_data_queue_;
  // Pool sized to queue capacity (SIZE-1) — see USBUartChannelBase::output_pool_ comment.
  // Its payload storage is sized from the IN endpoint when the device is opened.
  RxChunkPool<USB_DATA_QUEUE_SIZE - 1> chunk_pool_;

 protected:
  // Issue one control transfer as part of the setup state machine. The completion
  // callback (USB-task context) records the result/IN data, marks the step done and
  // wakes the loop so run_config_machine_() advances on the loop thread. Call exactly
  // once from config_step_()/config_device_step_() when issuing a step.
  void config_transfer_(uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                        const std::vector<uint8_t> &data = {});
  // (Re)start the config state machine. INIT runs over all channels; the other modes act
  // on cfg_single_ only.
  void start_config_(ConfigMode mode);
  // Advance the config state machine; called from loop(). Returns true if it did work.
  bool run_config_machine_();

  // The client's main loop: queued data, the config machine. False when there was nothing to do.
  bool loop();
  virtual void on_connected() = 0;
  virtual void on_disconnected() {}
  /// No YAML entry: @p count channels with the defaults (115200 8N1, 256 byte buffers), deleted with the adapter.
  void add_default_channels_(uint8_t count);
  virtual USBUartChannelBase *make_channel_(uint8_t index, uint16_t buffer_size);
  /// The adapter is given up (closed; deleted once nothing of it is out).
  void disconnect();
  void status_set_error(const LogString *message);
  void status_set_warning(const LogString *message);
  void status_clear_error();

  // Per-subclass per-channel settings sequence. For the given zero-based step, issue the
  // next control transfer via config_transfer_() and return true, or return false when the
  // channel has no more steps. reload=true ⇒ apply only baud/parity/stop/data (skip
  // enable/reset/DTR-RTS). ok/response carry the previous step's result and IN data.
  virtual bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok,
                           const uint8_t *response) = 0;
  // Optional one-time device-level setup run before the per-channel phase on init only
  // (e.g. CH34x chip detection). Same contract as config_step_(). Default: no steps.
  virtual bool config_device_step(uint8_t step, bool ok, const uint8_t *response) { return false; }
  // Issue the one control transfer, via config_transfer_(), that puts the channel's dtr_
  // and rts_ on the wire.
  virtual void modem_control_transfer(USBUartChannelBase *channel) = 0;

  USBUartClient *client_;
  usb_host::UsbDevice *dev_;
  usb_host_client_handle_t handle_;
  usb_device_handle_t device_handle_;
  uint16_t vid_;
  uint16_t pid_;
  bool connected_{true};
  bool own_channels_{false};  // made by add_default_channels_(), deleted with the adapter

  std::vector<USBUartChannelBase *> channels_{};

  // Config state machine
  USBUartChannelBase *cfg_single_{nullptr};  // non-null: single-channel run (RELOAD or MODEM)
  std::atomic<bool> cfg_done_{false};        // synchronizes cfg_ok_/cfg_response_ across threads
  uint8_t cfg_response_[8]{};                // last IN transfer payload (for detection reads)
  uint8_t cfg_channel_idx_{0};
  uint8_t cfg_step_{0};
  ConfigMode cfg_mode_{ConfigMode::CONFIG_MODE_INIT};
  bool cfg_active_{false};
  bool cfg_device_phase_{false};
  bool cfg_in_flight_{false};
  bool cfg_ok_{true};
};

class USBUartTypeCdcAcm : public USBUartComponent {
 public:
  USBUartTypeCdcAcm(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartComponent(client, dev) {}
  void set_claim_comm_interface(bool claim) { this->claim_comm_interface_ = claim; }

 protected:
  virtual std::vector<CdcEps> parse_descriptors(usb_device_handle_t dev_hdl);
  void on_connected() override;
  void on_disconnected() override;
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  void modem_control_transfer(USBUartChannelBase *channel) override;
  // Each claimed interface pins one host hardware channel per endpoint; skipping
  // the comm (interrupt) interface frees one on channel-poor hosts (ESP32-S3: 8).
  bool claim_comm_interface_{true};
};

class USBUartTypeCP210X : public USBUartTypeCdcAcm {
 public:
  USBUartTypeCP210X(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartTypeCdcAcm(client, dev) {}

 protected:
  std::vector<CdcEps> parse_descriptors(usb_device_handle_t dev_hdl) override;
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  void modem_control_transfer(USBUartChannelBase *channel) override;
};
class USBUartTypeCH34X : public USBUartTypeCdcAcm {
 public:
  USBUartTypeCH34X(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartTypeCdcAcm(client, dev) {
    // ch34x doesn't use the interrupt endpoint, and we don't have endpoints to spare. The
    // comm interface is left in place unclaimed: on the CDC-style chips it is still the
    // interface a host binds, and so the one the port is identified by.
    this->claim_comm_interface_ = false;
  }
  void dump_config() override;

 protected:
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  void modem_control_transfer(USBUartChannelBase *channel) override;
  bool config_device_step(uint8_t step, bool ok, const uint8_t *response) override;

 private:
  CH34xChipType chiptype_{CHIP_UNKNOWN};
  const char *chip_name_{"unknown"};
  uint8_t num_ports_{1};
};

class USBUartTypeCH934X : public USBUartComponent {
 public:
  USBUartTypeCH934X(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartComponent(client, dev) {}

  void start_input(USBUartChannelBase *channel) override;
  // Max number of channels initialised in parallel (one in-flight command write each).
  // Set from codegen and capped at the usb_host transfer-request pool size.
  void set_init_lanes(uint8_t lanes) { this->init_lanes_ = lanes; }

 protected:
  void on_connected() override;
  void on_disconnected() override;
  // Chip detection + one-time device/channel register setup. The CH934x configures its
  // ports via fire-and-forget bulk writes on a command endpoint (not control transfers),
  // so all init work is done here once detection completes; config_step() only re-applies
  // per-channel settings for load_settings().
  bool config_device_step(uint8_t step, bool ok, const uint8_t *response) override;
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  // DTR and RTS as two command writes (not a control transfer); the second one's completion ends the step.
  void modem_control_transfer(USBUartChannelBase *channel) override;

  bool parse_descriptors_(usb_device_handle_t dev_hdl);
  USBUartChannelBase *make_channel_(uint8_t index, uint16_t buffer_size) override;
  // Build the idx-th init register write for a channel into a stack buffer. Stateless and
  // deterministic, so the paced config machine can re-derive any write per round without
  // storing them. Returns false when idx is past this chip's per-channel write count.
  bool build_channel_write_(USBUartChannelBase *channel, uint8_t idx, uint8_t *buffer, uint8_t *len);
  // Submit one init command write on the shared command endpoint as part of a paced round:
  // records per-port failure, and the write that finishes the round releases the config
  // machine (cfg_done_). Returns false if submission failed (no free transfer slot).
  bool config_bulk_write_(USBUartChannelBase *channel, const uint8_t *data, uint16_t len);
  // Wire the shared TX endpoint/routing, mark successfully-configured channels initialised,
  // and start the RX/CMD readers. Runs once, after every channel's registers are written.
  void finalize_init_();
  uint8_t get_reg_address_(uint8_t portnum);

  void start_rx_reader_();
  bool demux_rx_data_(const uint8_t *data, size_t len);
  void start_command_reader_();
  void handle_command_data_(const uint8_t *data, size_t len);

  Ch934xEps uart_host_dev_{};
  CH934xChipType chiptype_{CHIP_CH934X_UNKNOWN};
  uint8_t num_ports_{0};
  uint8_t port_offset_{0};
  std::atomic<bool> rx_running_{false};
  std::atomic<bool> cmd_running_{false};

  // Paced parallel init state (see config_device_step()).
  uint8_t init_lanes_{1};                        // max channels configured in parallel per round
  uint8_t channel_write_count_{0};               // register writes per channel for the detected chip
  uint8_t init_group_start_{0};                  // first channel index of the current parallel group
  uint8_t init_write_idx_{0};                    // write index within the current group's lockstep rounds
  std::atomic<int> init_pending_{0};             // command writes still outstanding in the current round
  std::atomic<uint8_t> init_failed_mask_{0};     // per-port failure bits (bit N = port N failed)
  std::atomic<bool> init_device_failed_{false};  // a device-level init write failed
};

class CH934XChannel final : public USBUartChannelBase {
  friend class USBUartTypeCH934X;

 public:
  // TX header is 3 bytes: [port, len_lo, len_hi] — max data per packet is reduced accordingly
  static constexpr size_t TX_HEADER_SIZE = 3;
  static constexpr size_t TX_MAX_DATA = UsbOutputChunk::MAX_CHUNK_SIZE - TX_HEADER_SIZE;

  CH934XChannel(uint8_t index, uint16_t buffer_size) : USBUartChannelBase(index, buffer_size) {}
  void write_array(const uint8_t *data, size_t len) override;
  uart::UARTFlushResult flush() override;

 protected:
  USBUartChannelBase *tx_shared_channel_{nullptr};
  uint8_t tx_port_byte_{0};
};

class USBUartTypeFT23XX : public USBUartTypeCdcAcm {
 public:
  USBUartTypeFT23XX(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartTypeCdcAcm(client, dev) {}

  void start_input(USBUartChannelBase *channel) override;
  void on_rx_overflow(USBUartChannelBase *channel) override;

 protected:
  std::vector<CdcEps> parse_descriptors(usb_device_handle_t dev_hdl) override;
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  void modem_control_transfer(USBUartChannelBase *channel) override;

  uint8_t chip_type_{255};
};

enum Pl2303ChipType : uint8_t {
  PL2303_TYPE_H = 0,  // Legacy, max 1.2Mbaud
  PL2303_TYPE_HX,     // max 6Mbaud, divisor encoding
  PL2303_TYPE_TA,     // max 6Mbaud, alt divisor encoding
  PL2303_TYPE_TB,     // max 12Mbaud, alt divisor encoding
  PL2303_TYPE_HXD,    // max 12Mbaud, divisor encoding
  PL2303_TYPE_HXN,    // G-series, max 12Mbaud, direct encoding only
  PL2303_TYPE_UNKNOWN = 0xFF,
};

class USBUartTypePL2303 : public USBUartTypeCdcAcm {
  friend class USBUartChannelBase;

 public:
  USBUartTypePL2303(USBUartClient *client, usb_host::UsbDevice *dev) : USBUartTypeCdcAcm(client, dev) {}

 protected:
  std::vector<CdcEps> parse_descriptors(usb_device_handle_t dev_hdl) override;
  bool config_step(USBUartChannelBase *channel, uint8_t step, bool reload, bool ok, const uint8_t *response) override;
  void modem_control_transfer(USBUartChannelBase *channel) override;

  Pl2303ChipType chip_type_{PL2303_TYPE_UNKNOWN};
};

// ─────────────────────────────────────────────────────────────────────────────
// USBUartClient: one usb_host client for every serial adapter (accepts_more_devices_()). An adapter gets
// the channels of the YAML entry naming it, else its type from its vid:pid and default channels.
// ─────────────────────────────────────────────────────────────────────────────
class USBUartClient : public usb_host::USBClient {
  friend class USBUartComponent;

 public:
  USBUartClient(uint16_t vid, uint16_t pid);
  void loop() override;
  void dump_config() override;

  /// A YAML entry; its channels follow with add_entry_channel().
  void add_entry(uint16_t vid, uint16_t pid, UartType type, uint8_t init_lanes, bool claim_comm_interface) {
    this->entries_.push_back({vid, pid, type, init_lanes, claim_comm_interface, {}});
    if (vid != 0)
      this->add_vendor_match(vid, pid);  // routed here even when its class does not say serial
  }
  void add_entry_channel(USBUartChannelBase *channel) { this->entries_.back().channels.push_back(channel); }

 protected:
  bool accepts_more_devices_() const override { return true; }
  void on_device_connected(usb_host::UsbDevice &dev) override;
  void on_device_disconnected(usb_host::UsbDevice &dev) override;
  void free_device_context(usb_host::UsbDevice &dev) override;

  /// The entry naming this adapter whose channels are free (nullptr: none).
  USBUartEntry *find_entry_(uint16_t vid, uint16_t pid);

  std::vector<USBUartEntry> entries_{};
  std::vector<USBUartComponent *> adapters_{};
};

}  // namespace esphome::usb_uart

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3 ||
        // USE_ESP32_VARIANT_ESP32S31 || USE_ESP32_VARIANT_ESP32H4
