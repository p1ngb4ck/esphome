// usbredir host for the SPICE client: wire layouts and behaviour after usbredir's
// usbredirparser/usbredirproto.h, docs/usb-redirection-protocol.md and usbredirhost/usbredirhost.c.

#include "spice_usb.h"

#include <new>

#ifdef SVP_SPICE_USB

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.spice_usb";

namespace {

enum : uint32_t {
  REDIR_HELLO = 0,
  REDIR_DEVICE_CONNECT = 1,
  REDIR_DEVICE_DISCONNECT = 2,
  REDIR_RESET = 3,
  REDIR_INTERFACE_INFO = 4,
  REDIR_EP_INFO = 5,
  REDIR_SET_CONFIGURATION = 6,
  REDIR_GET_CONFIGURATION = 7,
  REDIR_CONFIGURATION_STATUS = 8,
  REDIR_SET_ALT_SETTING = 9,
  REDIR_GET_ALT_SETTING = 10,
  REDIR_ALT_SETTING_STATUS = 11,
  REDIR_START_ISO_STREAM = 12,
  REDIR_STOP_ISO_STREAM = 13,
  REDIR_ISO_STREAM_STATUS = 14,
  REDIR_START_INTERRUPT_RECEIVING = 15,
  REDIR_STOP_INTERRUPT_RECEIVING = 16,
  REDIR_INTERRUPT_RECEIVING_STATUS = 17,
  REDIR_ALLOC_BULK_STREAMS = 18,
  REDIR_FREE_BULK_STREAMS = 19,
  REDIR_BULK_STREAMS_STATUS = 20,
  REDIR_CANCEL_DATA_PACKET = 21,
  REDIR_FILTER_REJECT = 22,
  REDIR_FILTER_FILTER = 23,
  REDIR_DEVICE_DISCONNECT_ACK = 24,
  REDIR_CONTROL_PACKET = 100,
  REDIR_BULK_PACKET = 101,
  REDIR_ISO_PACKET = 102,
  REDIR_INTERRUPT_PACKET = 103,
};

enum : uint8_t {
  ST_SUCCESS = 0,
  ST_CANCELLED = 1,
  ST_INVAL = 2,
  ST_IOERROR = 3,
  ST_STALL = 4,
  ST_TIMEOUT = 5,
  ST_BABBLE = 6,
};

enum : uint8_t { TYPE_CONTROL = 0, TYPE_ISO = 1, TYPE_BULK = 2, TYPE_INTERRUPT = 3, TYPE_INVALID = 255 };

constexpr uint32_t CAP_CONNECT_DEVICE_VERSION = 1u << 1;
constexpr uint32_t CAP_DEVICE_DISCONNECT_ACK = 1u << 3;
constexpr uint32_t CAP_EP_INFO_MAX_PACKET_SIZE = 1u << 4;
constexpr uint32_t CAP_64BITS_IDS = 1u << 5;
constexpr uint32_t CAP_32BITS_BULK_LENGTH = 1u << 6;
constexpr uint32_t OUR_CAPS = CAP_CONNECT_DEVICE_VERSION | CAP_DEVICE_DISCONNECT_ACK | CAP_EP_INFO_MAX_PACKET_SIZE |
                              CAP_64BITS_IDS | CAP_32BITS_BULK_LENGTH;

// One bulk transfer is one qTD, whose size field has 17 bits (hal usb_dwc_ll.h); an IN one a whole
// number of packets.
constexpr size_t BULK_MAX = 131071;

constexpr uint8_t USB_DT_INTERFACE = 4;
constexpr uint8_t USB_DT_ENDPOINT = 5;
constexpr uint8_t REQ_SET_INTERFACE = 11;

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// usbredirhost's EP2I(): 0-15 OUT, 16-31 IN.
inline uint8_t ep_index(uint8_t ep) { return static_cast<uint8_t>(((ep & 0x80) >> 3) | (ep & 0x0F)); }

uint8_t redir_status(usb_transfer_status_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
      return ST_SUCCESS;
    case USB_TRANSFER_STATUS_TIMED_OUT:
      return ST_TIMEOUT;
    case USB_TRANSFER_STATUS_CANCELED:
      return ST_CANCELLED;
    case USB_TRANSFER_STATUS_STALL:
      return ST_STALL;
    case USB_TRANSFER_STATUS_OVERFLOW:
      return ST_BABBLE;
    default:
      return ST_IOERROR;
  }
}

}  // namespace

//========================================================================
// Main loop: the select, routing
//========================================================================

void SpiceUsbSlot::setup() {
  usb_host::USBClient::setup();
  if (this->is_failed())
    return;
  this->usb_ = usb_host::get_usb_host();
  this->labels_ = static_cast<char(*)[LABEL_BYTES]>(
      heap_caps_calloc(MAX_DEVICES + 1, LABEL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->dev_lock_ = xSemaphoreCreateMutex();
  this->done_q_ = xQueueCreate(GEN_XFERS + INT_XFERS + ISO_STREAMS * ISO_URBS + USBIP_ISO_XFERS, sizeof(Xfer *));
#ifdef SVP_DESKTOP_VNC
  this->usbip_lock_ = xSemaphoreCreateMutex();
  this->usbip_offsets_ = static_cast<uint32_t *>(heap_caps_malloc(
      static_cast<size_t>(USBIP_ISO_XFERS) * USBIP_ISO_PKTS * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->usbip_entry_ = static_cast<uint8_t *>(heap_caps_calloc(1, USBIP_ENTRY_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // Zeroed: each Xfer as its defaults (nothing, not busy).
  this->usbip_iso_ =
      static_cast<Xfer *>(heap_caps_calloc(USBIP_ISO_XFERS, sizeof(Xfer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  this->pool_bufs_ =
      static_cast<PoolBuf *>(heap_caps_calloc(POOL_BUFS, sizeof(PoolBuf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  // The transfers come with a session (link_open()), their buffers from the player's pool.
  bool ok = this->labels_ != nullptr && this->dev_lock_ != nullptr && this->done_q_ != nullptr &&
            this->pool_bufs_ != nullptr;
#ifdef SVP_DESKTOP_VNC
  ok = ok && this->usbip_lock_ != nullptr && this->usbip_offsets_ != nullptr && this->usbip_entry_ != nullptr &&
       this->usbip_iso_ != nullptr;
#endif
  if (!ok) {
    ESP_LOGE(TAG, "No memory for the USB redirection");
    this->mark_failed();
    return;
  }
  std::snprintf(this->labels_[0], LABEL_BYTES, "None");
  this->label_count_ = 1;
  this->usb_->add_on_devices_callback([this]() { this->refresh_options(); });
  this->refresh_options();
}

void SpiceUsbSlot::refresh_options() {
  if (this->labels_ == nullptr)
    return;
  uint8_t count = 1;
  bool want_present = false;
  for (const auto &dev : this->usb_->get_devices()) {
    if (count > MAX_DEVICES || dev.gone || dev.desc == nullptr)
      continue;
    char product[usb_host::DESC_STRING_BUF_SIZE] = "";
    usb_host::copy_descriptor_string(dev.info.str_desc_product, product);
    if (product[0] != '\0') {
      std::snprintf(this->labels_[count], LABEL_BYTES, "%.30s (%04X:%04X #%u)", product, dev.desc->idVendor,
                    dev.desc->idProduct, dev.addr);
    } else {
      std::snprintf(this->labels_[count], LABEL_BYTES, "%04X:%04X #%u", dev.desc->idVendor, dev.desc->idProduct,
                    dev.addr);
    }
    this->label_addr_[count] = dev.addr;
    want_present = want_present || dev.addr == this->want_addr_;
    count++;
  }
  this->label_count_ = count;
  if (this->want_addr_ == 0 && this->want_vid_ != 0 && (this->want_addr_ = this->find_wanted_()) != 0) {
    want_present = true;
    this->parent_->spice_usb_selected(this, this->want_addr_);  // the other slots let it go
  }
  // A take under way for a device that went: nothing is bound any more.
  bool taking_present = false;
  for (uint8_t i = 1; i < count; i++)
    taking_present = taking_present || this->label_addr_[i] == this->taking_addr_;
  if (!taking_present)
    this->taking_addr_ = 0;
  FixedVector<const char *> options;
  options.init(count);
  for (uint8_t i = 0; i < count; i++)
    options.push_back(this->labels_[i]);
  this->traits.set_options(options);
  if (!want_present)
    this->want_addr_ = 0;
  uint8_t shown = 0;
  for (uint8_t i = 1; i < count; i++) {
    if (this->label_addr_[i] == this->want_addr_ && this->want_addr_ != 0)
      shown = i;
  }
  this->publish_state(this->labels_[shown]);
  this->apply_();
#ifdef SVP_DESKTOP_VNC
  this->usbip_snapshot_();
#endif
}

void SpiceUsbSlot::control(const std::string &value) {
  uint8_t addr = 0;
  for (uint8_t i = 1; i < this->label_count_; i++) {
    if (value == this->labels_[i])
      addr = this->label_addr_[i];
  }
  if (addr != 0)
    this->parent_->spice_usb_selected(this, addr);  // the other slots let it go
  this->want_addr_ = addr;
  this->want_vid_ = 0;
  this->publish_state(value);
  this->apply_();
  this->parent_->spice_usb_changed(this);
}

void SpiceUsbSlot::forget_selection(uint8_t addr) {
  if (this->want_addr_ != addr)
    return;
  this->want_addr_ = 0;
  this->want_vid_ = 0;
  this->publish_state(this->labels_[0]);
  this->apply_();
  this->parent_->spice_usb_changed(this);
}

void SpiceUsbSlot::want_device(uint16_t vid, uint16_t pid, const char *serial) {
  this->want_vid_ = vid;
  this->want_pid_ = pid;
  std::snprintf(this->want_serial_, sizeof(this->want_serial_), "%s", serial != nullptr ? serial : "");
  this->want_addr_ = 0;
  this->refresh_options();
}

uint8_t SpiceUsbSlot::find_wanted_() const {
  for (const auto &dev : this->usb_->get_devices()) {
    if (dev.gone || dev.desc == nullptr || dev.desc->idVendor != this->want_vid_ ||
        dev.desc->idProduct != this->want_pid_)
      continue;
    char serial[usb_host::DESC_STRING_BUF_SIZE] = "";
    usb_host::copy_descriptor_string(dev.info.str_desc_serial_num, serial);
    if (this->want_serial_[0] == '\0' || std::strcmp(serial, this->want_serial_) == 0)
      return dev.addr;
  }
  return 0;
}

void SpiceUsbSlot::selected_device(uint16_t &vid, uint16_t &pid, char *serial, size_t serial_len) const {
  vid = pid = 0;
  serial[0] = '\0';
  for (const auto &dev : this->usb_->get_devices()) {
    if (this->want_addr_ == 0 || dev.addr != this->want_addr_ || dev.gone || dev.desc == nullptr)
      continue;
    vid = dev.desc->idVendor;
    pid = dev.desc->idProduct;
    char text[usb_host::DESC_STRING_BUF_SIZE] = "";
    usb_host::copy_descriptor_string(dev.info.str_desc_serial_num, text);
    std::snprintf(serial, serial_len, "%s", text);
  }
}

void SpiceUsbSlot::loop() {
  usb_host::USBClient::loop();
  this->apply_();
}

void SpiceUsbSlot::apply_() {
  const bool link = this->link_up_.load(std::memory_order_acquire);
  if (this->state_ == usb_host::USB_CLIENT_OPEN)
    return;  // being opened: given back, if no longer wanted, once it is
  if (this->state_ == usb_host::USB_CLIENT_CONNECTED &&
      (this->device_addr_ != this->want_addr_ || this->want_addr_ == 0 || !link)) {
    if (this->device_ready_.load(std::memory_order_acquire) && link &&
        !this->drained_.load(std::memory_order_acquire)) {
      // The spice task stops what runs on it first.
      if (!this->drain_req_.exchange(true, std::memory_order_acq_rel))
        this->parent_->spice_wake();
      return;
    }
    this->drain_req_.store(false, std::memory_order_release);
    this->drained_.store(false, std::memory_order_release);
    this->disconnect();  // the drivers it was taken from get it back (USBHost::client_released())
  }
  if (this->taking_addr_ != 0 && this->state_ == usb_host::USB_CLIENT_INIT) {
    // Still under way? Else it ended without the device (gone, or it would not open).
    bool taking = false;
    for (const auto &dev : this->usb_->get_devices())
      taking = taking || dev.taken_by == this;
    if (!taking)
      this->taking_addr_ = 0;
  }
  // One take at a time: USBHost binds this slot once the device's drivers have let go.
  if (this->state_ == usb_host::USB_CLIENT_INIT && this->taking_addr_ == 0 && this->want_addr_ != 0 && link &&
      this->usb_->take_device(this, this->want_addr_))
    this->taking_addr_ = this->want_addr_;
}

void SpiceUsbSlot::on_connected() {
  // Given back at once if no longer wanted (apply_()).
  this->taking_addr_ = 0;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  this->claimed_count_ = 0;
  std::memset(this->alts_, 0, sizeof(this->alts_));
  const uint8_t *p = reinterpret_cast<const uint8_t *>(this->config_desc_);
  const uint16_t total = this->config_desc_->wTotalLength;
  this->config_value_ = this->config_desc_->bConfigurationValue;
  for (uint16_t at = 0; at + 2 <= total && p[at] >= 2; at += p[at]) {
    if (p[at + 1] != USB_DT_INTERFACE || at + 4 > total || p[at + 3] != 0)
      continue;
    const uint8_t number = p[at + 2];
    if (number >= 32 || this->claimed_count_ >= 32)
      continue;
    if (this->claim_interface(number, 0))
      this->claimed_[this->claimed_count_++] = number;
  }
  this->parse_eps_();
  this->announced_ = false;
  xSemaphoreGive(this->dev_lock_);
  this->device_ready_.store(true, std::memory_order_release);
  this->parent_->spice_wake();
  this->enable_loop();
}

void SpiceUsbSlot::on_disconnected() {
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  this->device_ready_.store(false, std::memory_order_release);
  for (uint8_t i = 0; i < this->claimed_count_; i++)
    this->release_interface(this->claimed_[i]);
  this->claimed_count_ = 0;
  xSemaphoreGive(this->dev_lock_);
  usb_host::USBClient::on_disconnected();
  this->parent_->spice_wake();  // the guest is told
}

void SpiceUsbSlot::dump_config() {
  LOG_SELECT("", "SPICE USB redirection", this);
  usb_host::USBClient::dump_config();
}

//========================================================================
// Spice task: the protocol
//========================================================================

void SpiceUsbSlot::xfer_done_(usb_transfer_t *t) {
  // USB task: handed to the spice task.
  auto *x = static_cast<Xfer *>(t->context);
  xQueueSend(x->slot->done_q_, &x, 0);
  x->slot->parent_->spice_wake();
}

// As the USB host's own urb_alloc() sets them (usb_private.c): the fields are const for its users only.
static void set_buffer(usb_transfer_t *t, uint8_t *buffer, size_t size) {
  *const_cast<uint8_t **>(&t->data_buffer) = buffer;
  *const_cast<size_t *>(&t->data_buffer_size) = size;
}

bool SpiceUsbSlot::xfer_alloc_(usb_transfer_t **out, size_t bytes, int packets) {
  *out = nullptr;
  PoolBuf *entry = nullptr;
  for (size_t i = 0; entry == nullptr && i < POOL_BUFS; i++) {
    if (this->pool_bufs_[i].t == nullptr)
      entry = &this->pool_bufs_[i];
  }
  // Whole cache lines (the host syncs data_buffer_size of it), as the pool hands them out.
  const size_t size = (bytes + PsramPool::ALIGN - 1) & ~(PsramPool::ALIGN - 1);
  usb_transfer_t *t = nullptr;
  if (entry == nullptr || usb_host_transfer_alloc(1, packets, &t) != ESP_OK || t == nullptr)
    return false;
  uint8_t *piece = this->parent_->desktop_alloc(size);
  if (piece == nullptr) {
    usb_host_transfer_free(t);
    return false;
  }
  *entry = PoolBuf{t, t->data_buffer, t->data_buffer_size, piece};
  set_buffer(t, piece, size);
  *out = t;
  return true;
}

void SpiceUsbSlot::xfer_free_(usb_transfer_t *t) {
  if (t == nullptr)
    return;
  for (size_t i = 0; i < POOL_BUFS; i++) {
    PoolBuf &entry = this->pool_bufs_[i];
    if (entry.t != t)
      continue;
    set_buffer(t, entry.own, entry.own_size);
    this->parent_->desktop_free(entry.piece);
    entry = PoolBuf{};
    break;
  }
  usb_host_transfer_free(t);
}

bool SpiceUsbSlot::release_buffers() {
  if (this->link_up_.load(std::memory_order_acquire) || this->pool_bufs_ == nullptr || this->xs_ == nullptr)
    return false;
  bool released = false;
  auto give = [this, &released](Xfer &x) {
    if (x.t != nullptr && !x.in_flight) {
      this->xfer_free_(x.t);
      x.t = nullptr;
      released = true;
    }
  };
  for (auto &x : this->xs_->gen)
    give(x);
  for (auto &x : this->xs_->intr)
    give(x);
  for (uint8_t i = 0; i < USBIP_ISO_XFERS; i++)
    give(this->usbip_iso_[i]);
  // Nothing of the session holds a buffer any more: its state goes too
  bool held = false;
  for (auto &x : this->xs_->gen)
    held = held || x.t != nullptr;
  for (auto &x : this->xs_->intr)
    held = held || x.t != nullptr;
  for (auto &st : this->xs_->iso) {
    for (auto &x : st.x)
      held = held || x.t != nullptr;
  }
  if (!held) {
    this->xs_->~Session();
    heap_caps_free(this->xs_);
    this->xs_ = nullptr;
    this->stage_ = nullptr;
  }
  return released;
}

bool SpiceUsbSlot::link_open(SpiceVmcLink *link, Proto proto) {
  if (this->xs_ == nullptr) {
    void *mem = heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
      ESP_LOGE(TAG, "No internal RAM for the USB redirection's transfers (%u bytes)", static_cast<unsigned>(sizeof(Session)));
      return false;
    }
    this->xs_ = new (mem) Session();
    for (auto &x : this->xs_->intr)
      x.kind = Kind::INTERRUPT_IN;
    this->stage_ = this->xs_->stage;
  }
  // What came back after the last channel ended is for no one.
  Xfer *x;
  while (xQueueReceive(this->done_q_, &x, 0) == pdTRUE) {
    x->in_flight = false;
    this->complete_(x);
  }
  // The session's transfers (kept from the last one, or from the pool now).
  bool ok = true;
  auto take = [this, &ok](Xfer &xfer, size_t bytes) {
    if (ok && xfer.t == nullptr)
      ok = this->xfer_alloc_(&xfer.t, bytes, 0);
    if (xfer.t != nullptr) {
      xfer.slot = this;
      xfer.t->callback = xfer_done_;
      xfer.t->context = &xfer;
    }
  };
  for (auto &g : this->xs_->gen)
    take(g, GEN_BYTES);
  for (auto &i : this->xs_->intr)
    take(i, INT_BYTES);
  if (!ok) {
    ESP_LOGE(TAG, "No room in the PSRAM pool for the USB redirection");
    return false;
  }
  this->link_ = link;
  this->peer_hello_ = false;
  this->ids64_ = false;
  this->announced_ = false;
  this->rx_ = Rx::HEADER;
  this->have_ = 0;
  this->rx_xfer_ = nullptr;
  this->proto_ = proto;
  this->usbip_packets_ = -1;
  if (proto == Proto::USBREDIR)
    this->send_hello_();
  this->link_up_.store(true, std::memory_order_release);
  this->enable_loop_soon_any_context();
  return true;
}

void SpiceUsbSlot::link_close() {
  if (this->link_ == nullptr)
    return;
  this->drain_();
  this->link_ = nullptr;
  this->link_up_.store(false, std::memory_order_release);
  this->enable_loop_soon_any_context();
}

bool SpiceUsbSlot::send_(uint32_t type, uint64_t id, const void *type_header, size_t type_header_len,
                         const uint8_t *data, size_t data_len) {
  if (this->link_ == nullptr)
    return false;
  uint8_t head[16 + 32];
  const size_t hl = this->header_len_();
  put32(head, type);
  put32(head + 4, static_cast<uint32_t>(type_header_len + data_len));
  put32(head + 8, static_cast<uint32_t>(id));
  if (hl == 16)
    put32(head + 12, static_cast<uint32_t>(id >> 32));
  if (type_header_len <= 32) {
    std::memcpy(head + hl, type_header, type_header_len);
    return this->link_->vmc_send(head, hl + type_header_len, data, data_len);
  }
  // A long one (interface_info, ep_info) carries no data: it goes as the data part.
  return this->link_->vmc_send(head, hl, static_cast<const uint8_t *>(type_header), type_header_len);
}

bool SpiceUsbSlot::send_hello_() {
  // Always with 32-bit ids: the peer's caps are not known yet.
  uint8_t head[12 + 64 + 4] = {};
  put32(head, REDIR_HELLO);
  put32(head + 4, 64 + 4);
  std::snprintf(reinterpret_cast<char *>(head + 12), 64, "ESPHome simple_video_player");
  put32(head + 12 + 64, OUR_CAPS);
  return this->link_->vmc_send(head, sizeof(head), nullptr, 0);
}

void SpiceUsbSlot::parse_eps_() {
  for (uint8_t i = 0; i < 32; i++)
    this->eps_[i] = {static_cast<uint8_t>((i & 0x0F) == 0 ? TYPE_CONTROL : TYPE_INVALID), 0, 0, 0};
  const uint16_t mps0 = this->device_desc_ != nullptr ? this->device_desc_->bMaxPacketSize0 : 64;
  this->eps_[0].mps = mps0;
  this->eps_[16].mps = mps0;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(this->config_desc_);
  const uint16_t total = this->config_desc_->wTotalLength;
  bool current = false;
  uint8_t number = 0;
  const bool high = this->device_speed_high_();
  for (uint16_t at = 0; at + 2 <= total && p[at] >= 2; at += p[at]) {
    if (p[at + 1] == USB_DT_INTERFACE && at + 4 <= total) {
      number = p[at + 2];
      current = number < 32 && p[at + 3] == this->alts_[number];
    } else if (p[at + 1] == USB_DT_ENDPOINT && current && at + 7 <= total) {
      const uint8_t ep = p[at + 2];
      const uint8_t type = p[at + 3] & 0x03;
      const uint16_t raw = rd16(p + at + 4);
      uint16_t mps = raw & 0x7FF;
      // usbredirhost_set_max_packetsize(): high-bandwidth isochronous carries up to 3 per microframe.
      if (high && type == TYPE_ISO)
        mps = static_cast<uint16_t>(mps * (1 + ((raw >> 11) & 3)));
      this->eps_[ep_index(ep)] = {type, p[at + 6], number, mps};
    }
  }
}

bool SpiceUsbSlot::device_speed_high_() const {
  usb_device_info_t info{};
  return this->device_handle_ != nullptr && usb_host_device_info(this->device_handle_, &info) == ESP_OK &&
         info.speed == USB_SPEED_HIGH;
}

bool SpiceUsbSlot::send_device_info_() {
  // interface_info, ep_info, then device_connect (usbredirhost_send_device_connect()).
  uint8_t info[4 + 4 * 32] = {};
  uint32_t count = 0;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(this->config_desc_);
  const uint16_t total = this->config_desc_->wTotalLength;
  for (uint16_t at = 0; at + 2 <= total && p[at] >= 2 && count < 32; at += p[at]) {
    if (p[at + 1] != USB_DT_INTERFACE || at + 8 > total || p[at + 2] >= 32 || p[at + 3] != this->alts_[p[at + 2]])
      continue;
    info[4 + count] = p[at + 2];
    info[4 + 32 + count] = p[at + 5];
    info[4 + 64 + count] = p[at + 6];
    info[4 + 96 + count] = p[at + 7];
    count++;
  }
  put32(info, count);
  if (!this->send_(REDIR_INTERFACE_INFO, 0, info, sizeof(info)))
    return false;
  uint8_t ep[32 * 3 + 64] = {};
  for (uint8_t i = 0; i < 32; i++) {
    ep[i] = this->eps_[i].type;
    ep[32 + i] = this->eps_[i].interval;
    ep[64 + i] = this->eps_[i].interface;
    put16(ep + 96 + 2 * i, this->eps_[i].mps);
  }
  return this->send_(REDIR_EP_INFO, 0, ep, this->peer_ep_mps_ ? sizeof(ep) : 96);
}

bool SpiceUsbSlot::service() {
  if (this->link_ == nullptr)
    return true;
#ifdef SVP_DESKTOP_VNC
  if (this->proto_ == Proto::USBIP)
    return this->usbip_service_();
#endif
  Xfer *x;
  while (xQueueReceive(this->done_q_, &x, 0) == pdTRUE) {
    x->in_flight = false;
    if (!this->complete_(x))
      return false;
  }
  // The device to let go: what runs on it stops first.
  if (this->drain_req_.load(std::memory_order_acquire) && !this->drained_.load(std::memory_order_acquire)) {
    this->drain_();
    if (this->announced_) {
      this->announced_ = false;
      if (!this->send_(REDIR_DEVICE_DISCONNECT, 0, nullptr, 0))
        return false;
    }
    this->drained_.store(true, std::memory_order_release);
    this->enable_loop_soon_any_context();
  }
  const bool ready = this->usable_();
  if (!ready && this->announced_) {
    this->announced_ = false;
    if (!this->send_(REDIR_DEVICE_DISCONNECT, 0, nullptr, 0))
      return false;
  }
  if (ready && !this->announced_ && this->peer_hello_ && !this->drain_req_.load(std::memory_order_acquire)) {
    xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
    bool ok = true;
    if (this->usable_()) {
      ok = this->send_device_info_();
      uint8_t connect[10];
      usb_device_info_t info{};
      usb_host_device_info(this->device_handle_, &info);
      connect[0] = info.speed == USB_SPEED_LOW ? 0 : info.speed == USB_SPEED_FULL ? 1 : 2;
      connect[1] = this->device_desc_->bDeviceClass;
      connect[2] = this->device_desc_->bDeviceSubClass;
      connect[3] = this->device_desc_->bDeviceProtocol;
      put16(connect + 4, this->device_desc_->idVendor);
      put16(connect + 6, this->device_desc_->idProduct);
      put16(connect + 8, this->device_desc_->bcdDevice);
      ok = ok && this->send_(REDIR_DEVICE_CONNECT, 0, connect, this->peer_connect_version_ ? 10 : 8);
      this->announced_ = ok;
    }
    xSemaphoreGive(this->dev_lock_);
    if (!ok)
      return false;
  }
  switch (this->rx_) {
    case Rx::WAIT_DATA:
      return this->begin_data_packet_();
    case Rx::WAIT_BODY:
      this->rx_ = Rx::HEADER;
      return this->handle_body_();
    case Rx::WAIT_ALT:
      return this->alt_step_();
    default:
      return true;
  }
}

uint8_t *SpiceUsbSlot::rx_space(size_t &room) {
#ifdef SVP_DESKTOP_VNC
  if (this->proto_ == Proto::USBIP)
    return this->usbip_rx_space_(room);
#endif
  switch (this->rx_) {
    case Rx::HEADER:
      room = (this->peer_hello_ ? this->header_len_() : 12) - this->have_;
      return this->hdr_ + this->have_;
    case Rx::BODY:
      room = this->body_len_ - this->have_;
      return this->stage_ + this->have_;
    case Rx::TYPE_HEADER:
      room = this->type_header_len_ - this->have_;
      return this->stage_ + this->have_;
    case Rx::DATA: {
      room = this->data_len_ - this->have_;
      const size_t setup = this->rx_xfer_->kind == Kind::CONTROL ? 8 : 0;
      return this->rx_xfer_->t->data_buffer + setup + this->have_;
    }
    case Rx::SKIP:
      room = std::min<size_t>(this->skip_left_, STAGE_BYTES);
      return this->stage_;
    default:
      room = 0;
      return this->stage_;
  }
}

bool SpiceUsbSlot::rx_done(size_t n) {
#ifdef SVP_DESKTOP_VNC
  if (this->proto_ == Proto::USBIP)
    return this->usbip_rx_done_(n);
#endif
  this->have_ += n;
  switch (this->rx_) {
    case Rx::HEADER: {
      const size_t need = this->peer_hello_ ? this->header_len_() : 12;
      if (this->have_ < need)
        return true;
      this->type_ = rd32(this->hdr_);
      this->body_len_ = rd32(this->hdr_ + 4);
      this->id_ = rd32(this->hdr_ + 8);
      if (need == 16)
        this->id_ |= static_cast<uint64_t>(rd32(this->hdr_ + 12)) << 32;
      this->have_ = 0;
      if (this->peer_hello_ && (this->type_ == REDIR_CONTROL_PACKET || this->type_ == REDIR_BULK_PACKET)) {
        this->type_header_len_ = this->type_ == REDIR_CONTROL_PACKET ? 10 : (this->peer_32bit_bulk_ ? 10 : 8);
        if (this->body_len_ < this->type_header_len_) {
          ESP_LOGW(TAG, "Bad usbredir packet");
          return false;
        }
        this->rx_ = Rx::TYPE_HEADER;
        return true;
      }
      if (this->body_len_ > STAGE_BYTES) {
        ESP_LOGW(TAG, "usbredir packet %" PRIu32 " of %" PRIu32 " bytes skipped", this->type_, this->body_len_);
        this->skip_left_ = this->body_len_;
        this->rx_ = Rx::SKIP;
        return true;
      }
      if (this->body_len_ == 0)
        return this->handle_body_();
      this->rx_ = Rx::BODY;
      return true;
    }
    case Rx::BODY:
      if (this->have_ < this->body_len_)
        return true;
      this->have_ = 0;
      this->rx_ = Rx::HEADER;
      return this->handle_body_();
    case Rx::TYPE_HEADER:
      if (this->have_ < this->type_header_len_)
        return true;
      this->have_ = 0;
      return this->begin_data_packet_();
    case Rx::DATA: {
      if (this->have_ < this->data_len_)
        return true;
      this->have_ = 0;
      this->rx_ = Rx::HEADER;
      Xfer *x = this->rx_xfer_;
      this->rx_xfer_ = nullptr;
      return this->submit_(x);
    }
    case Rx::SKIP:
      this->skip_left_ -= static_cast<uint32_t>(n);
      this->have_ = 0;
      if (this->skip_left_ == 0)
        this->rx_ = Rx::HEADER;
      return true;
    default:
      return true;
  }
}

SpiceUsbSlot::Xfer *SpiceUsbSlot::gen_take_() {
  for (auto &x : this->xs_->gen) {
    if (!x.busy) {
      x = Xfer{x.t, this};
      x.t->callback = xfer_done_;
      x.t->context = &x;
      x.busy = true;
      return &x;
    }
  }
  return nullptr;
}

// Control or bulk packet, its type header in stage_: a transfer for it (WAIT_DATA while none is free),
// then its OUT data straight into the transfer's buffer.
bool SpiceUsbSlot::begin_data_packet_() {
  const uint8_t *h = this->stage_;
  Xfer *x = this->gen_take_();
  if (x == nullptr) {
    this->rx_ = Rx::WAIT_DATA;
    return true;
  }
  this->rx_ = Rx::HEADER;
  x->id = this->id_;
  x->ep = h[0];
  const size_t extra = this->body_len_ - this->type_header_len_;
  bool in;
  size_t wanted;
  if (this->type_ == REDIR_CONTROL_PACKET) {
    x->kind = Kind::CONTROL;
    x->request = h[1];
    x->requesttype = h[2];
    x->value = rd16(h + 4);
    x->index = rd16(h + 6);
    x->length = rd16(h + 8);
    in = (x->requesttype & 0x80) != 0;
    wanted = x->length;
  } else {
    x->kind = Kind::BULK;
    x->want = rd16(h + 2);
    if (this->peer_32bit_bulk_)
      x->want |= static_cast<uint32_t>(rd16(h + 8)) << 16;
    in = (x->ep & 0x80) != 0;
    wanted = x->want;
  }
  if (in ? extra != 0 : extra != wanted) {
    x->busy = false;
    ESP_LOGW(TAG, "Bad usbredir data packet");
    return false;
  }
  const size_t room = x->t->data_buffer_size - (x->kind == Kind::CONTROL ? 8 : 0);
  if (wanted > room || (x->kind == Kind::BULK && wanted > BULK_MAX)) {
    // Too large for one transfer: counted out, answered as failed.
    x->t->status = USB_TRANSFER_STATUS_ERROR;
    x->t->actual_num_bytes = 0;
    if (extra > 0) {
      this->skip_left_ = static_cast<uint32_t>(extra);
      this->rx_ = Rx::SKIP;
    }
    return this->complete_(x);
  }
  if (extra > 0) {
    this->rx_xfer_ = x;
    this->data_len_ = extra;
    this->have_ = 0;
    this->rx_ = Rx::DATA;
    return true;
  }
  return this->submit_(x);
}

bool SpiceUsbSlot::submit_(Xfer *x) {
  usb_transfer_t *t = x->t;
  bool ok = false;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  if (this->usable_() && this->announced_) {
    t->device_handle = this->device_handle_;
    t->timeout_ms = 0;
    t->flags = 0;
    if (x->kind == Kind::CONTROL) {
      const bool in = (x->requesttype & 0x80) != 0;
      const uint16_t mps0 = this->eps_[0].mps != 0 ? this->eps_[0].mps : 64;
      // An IN data stage takes whole packets (the host sizes it from the buffer).
      const size_t data = in ? (static_cast<size_t>(x->length) + mps0 - 1) / mps0 * mps0 : x->length;
      if (8 + data <= t->data_buffer_size) {
        uint8_t *setup = t->data_buffer;
        setup[0] = x->requesttype;
        setup[1] = x->request;
        put16(setup + 2, x->value);
        put16(setup + 4, x->index);
        put16(setup + 6, x->length);
        t->bEndpointAddress = x->requesttype & 0x80;
        t->num_bytes = static_cast<int>(8 + data);
        ok = usb_host_transfer_submit_control(this->handle_, t) == ESP_OK;
      }
    } else if (x->kind == Kind::BULK) {
      const uint8_t idx = ep_index(x->ep);
      const uint16_t mps = this->eps_[idx].mps;
      size_t length = x->want;
      // An IN transfer is a whole number of packets.
      if ((x->ep & 0x80) != 0 && mps != 0)
        length = (length + mps - 1) / mps * mps;
      if (this->eps_[idx].type == TYPE_BULK && length <= BULK_MAX && length <= t->data_buffer_size) {
        t->bEndpointAddress = x->ep;
        t->num_bytes = static_cast<int>(length);
        ok = usb_host_transfer_submit(t) == ESP_OK;
      }
    } else if (x->kind == Kind::INTERRUPT_OUT) {
      t->bEndpointAddress = x->ep;
      t->num_bytes = x->length;
      ok = usb_host_transfer_submit(t) == ESP_OK;
    }
  }
  if (ok)
    x->in_flight = true;
  xSemaphoreGive(this->dev_lock_);
  if (ok)
    return true;
  // Not submitted: answered as failed.
  t->status = USB_TRANSFER_STATUS_ERROR;
  t->actual_num_bytes = 0;
  return this->complete_(x);
}

// A transfer back (or never submitted): its answer to the guest.
bool SpiceUsbSlot::complete_(Xfer *x) {
#ifdef SVP_DESKTOP_VNC
  if (this->proto_ == Proto::USBIP)
    return this->usbip_complete_(x);
#endif
  usb_transfer_t *t = x->t;
  if ((x->kind == Kind::INTERRUPT_IN && x->intr == nullptr) || (x->kind == Kind::ISO && x->iso == nullptr)) {
    x->busy = false;
    return true;
  }
  const uint8_t status = redir_status(t->status);
  const uint8_t ep = x->kind == Kind::ISO ? x->iso->ep : x->kind == Kind::INTERRUPT_IN ? x->intr->ep : x->ep;
  if (status == ST_STALL && (ep & 0x7F) != 0 && x->kind != Kind::CONTROL) {
    // The host side of a stalled endpoint takes transfers again; the guest clears the device's halt.
    xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
    if (this->usable_())
      usb_host_endpoint_clear(this->device_handle_, ep);
    xSemaphoreGive(this->dev_lock_);
  }
  const auto got = static_cast<size_t>(std::max(t->actual_num_bytes, 0));
  switch (x->kind) {
    case Kind::SET_ALT:
      x->busy = false;
      return this->alt_done_(t->status == USB_TRANSFER_STATUS_COMPLETED);
    case Kind::CONTROL: {
      const bool in = (x->requesttype & 0x80) != 0;
      const size_t data = std::min<size_t>(got > 8 ? got - 8 : 0, x->length);
      uint8_t h[10];
      h[0] = x->ep;
      h[1] = x->request;
      h[2] = x->requesttype;
      h[3] = status;
      put16(h + 4, x->value);
      put16(h + 6, x->index);
      put16(h + 8, static_cast<uint16_t>(in ? data : (status == ST_SUCCESS ? x->length : 0)));
      x->busy = false;
      return this->send_(REDIR_CONTROL_PACKET, x->id, h, sizeof(h), in ? t->data_buffer + 8 : nullptr, in ? data : 0);
    }
    case Kind::BULK: {
      const bool in = (x->ep & 0x80) != 0;
      uint8_t st = status;
      size_t data = in ? got : (status == ST_SUCCESS ? x->want : got);
      if (in && data > x->want) {
        st = ST_BABBLE;  // more than was asked for: the transfer was rounded up to whole packets
        data = x->want;
      }
      uint8_t h[10];
      h[0] = x->ep;
      h[1] = st;
      put16(h + 2, static_cast<uint16_t>(data & 0xFFFF));
      put32(h + 4, 0);
      put16(h + 8, static_cast<uint16_t>(data >> 16));
      x->busy = false;
      return this->send_(REDIR_BULK_PACKET, x->id, h, this->peer_32bit_bulk_ ? 10 : 8, in ? t->data_buffer : nullptr,
                         in ? data : 0);
    }
    case Kind::INTERRUPT_OUT: {
      uint8_t h[4];
      h[0] = x->ep;
      h[1] = status;
      put16(h + 2, static_cast<uint16_t>(status == ST_SUCCESS ? x->length : 0));
      x->busy = false;
      return this->send_(REDIR_INTERRUPT_PACKET, x->id, h, sizeof(h));
    }
    case Kind::INTERRUPT_IN: {
      IntIn *in = x->intr;
      bool ok = true;
      const bool ended = t->status == USB_TRANSFER_STATUS_CANCELED || t->status == USB_TRANSFER_STATUS_NO_DEVICE;
      if (!in->stopping && !ended) {
        uint8_t h[4];
        h[0] = in->ep;
        h[1] = status;
        put16(h + 2, static_cast<uint16_t>(got));
        ok = this->send_(REDIR_INTERRUPT_PACKET, in->next_id++, h, sizeof(h), t->data_buffer, got);
      }
      if (in->on && !in->stopping && !ended && status != ST_STALL) {
        xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
        const bool again = this->usable_() && usb_host_transfer_submit(t) == ESP_OK;
        xSemaphoreGive(this->dev_lock_);
        if (again) {
          x->in_flight = true;
          return ok;
        }
      }
      // This one ends; the receiver once its last one has.
      x->busy = false;
      x->intr = nullptr;
      bool any = false;
      for (auto *&other : in->x) {
        if (other == x)
          other = nullptr;
        any = any || other != nullptr;
      }
      if (!any) {
        if (in->on && !in->stopping) {
          // Stopped on its own: usb_redir_stall, as usbredirhost reports it.
          uint8_t s[2] = {ST_STALL, in->ep};
          ok = ok && this->send_(REDIR_INTERRUPT_RECEIVING_STATUS, 0, s, sizeof(s));
        }
        in->on = false;
        in->stopping = false;
      }
      return ok;
    }
    case Kind::ISO: {
      IsoStream *s = x->iso;
      bool ok = true;
      const bool in = (s->ep & 0x80) != 0;
      const bool ended = t->status == USB_TRANSFER_STATUS_CANCELED || t->status == USB_TRANSFER_STATUS_NO_DEVICE;
      if (in && !s->stopping && !ended) {
        const uint8_t *at = t->data_buffer;
        for (int i = 0; i < t->num_isoc_packets && ok; i++) {
          const usb_isoc_packet_desc_t &d = t->isoc_packet_desc[i];
          uint8_t h[4];
          h[0] = s->ep;
          h[1] = redir_status(d.status);
          put16(h + 2, static_cast<uint16_t>(std::max(d.actual_num_bytes, 0)));
          ok = this->send_(REDIR_ISO_PACKET, s->next_id++, h, sizeof(h), at,
                           static_cast<size_t>(std::max(d.actual_num_bytes, 0)));
          at += s->pkt_size;
        }
      }
      x->filled = 0;
      if (s->on && !s->stopping && !ended) {
        if (!in)
          return ok;  // empty again: filled by the next packets (iso_out_())
        for (int i = 0; i < t->num_isoc_packets; i++)
          t->isoc_packet_desc[i].num_bytes = s->pkt_size;
        t->num_bytes = static_cast<int>(s->pkt_size) * s->pkts;
        xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
        const bool again = this->usable_() && usb_host_transfer_submit(t) == ESP_OK;
        xSemaphoreGive(this->dev_lock_);
        if (again) {
          x->in_flight = true;
          return ok;
        }
      }
      bool any = false;
      for (uint8_t i = 0; i < s->urbs; i++)
        any = any || s->x[i].in_flight;
      if (any)
        return ok;
      if (s->on && !s->stopping) {
        uint8_t st[2] = {ST_STALL, s->ep};
        ok = ok && this->send_(REDIR_ISO_STREAM_STATUS, 0, st, sizeof(st));
      }
      for (uint8_t i = 0; i < s->urbs; i++) {
        if (s->x[i].t != nullptr)
          this->xfer_free_(s->x[i].t);
        s->x[i] = Xfer{};
      }
      s->on = false;
      s->stopping = false;
      s->urbs = 0;
      return ok;
    }
  }
  return true;
}

bool SpiceUsbSlot::handle_body_() {
  const uint8_t *b = this->stage_;
  const uint32_t len = this->body_len_;
  const uint64_t id = this->id_;
  if (!this->peer_hello_) {
    if (this->type_ != REDIR_HELLO || len < 64) {
      ESP_LOGW(TAG, "usbredir: no hello from the guest");
      return false;
    }
    const uint32_t caps = len >= 68 ? rd32(b + 64) : 0;
    this->peer_hello_ = true;
    // Both sides' caps decide (usbredirparser_have_cap()).
    this->ids64_ = (caps & OUR_CAPS & CAP_64BITS_IDS) != 0;
    this->peer_connect_version_ = (caps & CAP_CONNECT_DEVICE_VERSION) != 0;
    this->peer_ep_mps_ = (caps & CAP_EP_INFO_MAX_PACKET_SIZE) != 0;
    this->peer_32bit_bulk_ = (caps & CAP_32BITS_BULK_LENGTH) != 0;
    ESP_LOGD(TAG, "usbredir guest %.*s", 64, reinterpret_cast<const char *>(b));
    return true;
  }
  switch (this->type_) {
    case REDIR_RESET:
      // libusb resets the device here; ESP-IDF offers a client no reset, the guest goes on with it as it is.
      return true;
    case REDIR_SET_CONFIGURATION: {
      // ESP-IDF runs the configuration it enumerated; the guest may set that one.
      const uint8_t wanted = len >= 1 ? b[0] : 0;
      bool ok = true;
      uint8_t st = ST_IOERROR;
      xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
      if (wanted == this->config_value_ && this->usable_()) {
        ok = this->send_device_info_();
        st = ST_SUCCESS;
      }
      xSemaphoreGive(this->dev_lock_);
      uint8_t s[2] = {st, this->config_value_};
      return ok && this->send_(REDIR_CONFIGURATION_STATUS, id, s, sizeof(s));
    }
    case REDIR_GET_CONFIGURATION: {
      uint8_t s[2] = {static_cast<uint8_t>(this->usable_() ? ST_SUCCESS : ST_IOERROR), this->config_value_};
      return this->send_(REDIR_CONFIGURATION_STATUS, id, s, sizeof(s));
    }
    case REDIR_SET_ALT_SETTING:
      return len >= 2 && this->set_alt_(id, b[0], b[1]);
    case REDIR_GET_ALT_SETTING: {
      const uint8_t intf = len >= 1 ? b[0] : 0;
      uint8_t s[3] = {static_cast<uint8_t>(this->usable_() && intf < 32 ? ST_SUCCESS : ST_INVAL), intf,
                      static_cast<uint8_t>(intf < 32 ? this->alts_[intf] : 0)};
      return this->send_(REDIR_ALT_SETTING_STATUS, id, s, sizeof(s));
    }
    case REDIR_START_ISO_STREAM:
      return len >= 3 && this->start_iso_(id, b[0], b[1], b[2]);
    case REDIR_STOP_ISO_STREAM:
      return len >= 1 && this->stop_iso_(id, b[0], true);
    case REDIR_START_INTERRUPT_RECEIVING:
      return len >= 1 && this->start_int_(id, b[0]);
    case REDIR_STOP_INTERRUPT_RECEIVING:
      return len >= 1 && this->stop_int_(id, b[0], true);
    case REDIR_ALLOC_BULK_STREAMS:
    case REDIR_FREE_BULK_STREAMS: {
      // USB 3 bulk streams: there is no USB 3 here.
      uint8_t s[9] = {};
      if (len >= 4)
        std::memcpy(s, b, 4);
      s[8] = ST_INVAL;
      return this->send_(REDIR_BULK_STREAMS_STATUS, id, s, sizeof(s));
    }
    case REDIR_CANCEL_DATA_PACKET:
      for (auto &x : this->xs_->gen) {
        if (x.busy && x.in_flight && x.id == id && x.kind != Kind::CONTROL)
          this->cancel_ep_(x.ep);
      }
      return true;
    case REDIR_FILTER_REJECT:
      ESP_LOGW(TAG, "The SPICE server's filter rejects the device");
      return true;
    case REDIR_ISO_PACKET:
      return len < 4 || this->iso_out_(b[0], b + 4, std::min<size_t>(rd16(b + 2), len - 4));
    case REDIR_INTERRUPT_PACKET: {
      if (len < 4)
        return true;
      const uint8_t ep = b[0];
      const size_t data = std::min<size_t>(rd16(b + 2), len - 4);
      Xfer *x = this->gen_take_();
      if (x == nullptr) {
        this->rx_ = Rx::WAIT_BODY;  // kept in stage_ until a transfer is free
        return true;
      }
      x->kind = Kind::INTERRUPT_OUT;
      x->id = id;
      x->ep = ep;
      x->length = static_cast<uint16_t>(data);
      if ((ep & 0x80) != 0 || this->eps_[ep_index(ep)].type != TYPE_INTERRUPT || data > x->t->data_buffer_size) {
        x->t->status = USB_TRANSFER_STATUS_ERROR;
        x->t->actual_num_bytes = 0;
        return this->complete_(x);
      }
      std::memcpy(x->t->data_buffer, b + 4, data);
      return this->submit_(x);
    }
    default:
      // filter_filter, device_disconnect_ack, buffered bulk (not offered): nothing to do.
      return true;
  }
}

void SpiceUsbSlot::cancel_ep_(uint8_t ep) {
  // ESP-IDF cancels by endpoint: halted and flushed, its transfers come back cancelled.
  if ((ep & 0x7F) == 0)
    return;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  if (this->usable_() && usb_host_endpoint_halt(this->device_handle_, ep) == ESP_OK) {
    usb_host_endpoint_flush(this->device_handle_, ep);
    usb_host_endpoint_clear(this->device_handle_, ep);
  }
  xSemaphoreGive(this->dev_lock_);
}

bool SpiceUsbSlot::interface_busy_(uint8_t interface) const {
  for (const auto &s : this->xs_->iso) {
    if (s.urbs != 0 && this->eps_[ep_index(s.ep)].interface == interface)
      return true;
  }
  for (const auto &in : this->xs_->int_in) {
    if ((in.on || in.stopping) && this->eps_[ep_index(in.ep)].interface == interface)
      return true;
  }
  return false;
}

// set_alt_setting is a control packet of the protocol: nothing else is read until it is answered
// (WAIT_ALT), as usbredirhost handles it before the next one.
bool SpiceUsbSlot::set_alt_(uint64_t id, uint8_t interface, uint8_t alt) {
  if (interface >= 32 || !this->usable_()) {
    uint8_t s[3] = {ST_INVAL, interface, 0};
    return this->send_(REDIR_ALT_SETTING_STATUS, id, s, sizeof(s));
  }
  this->alt_ = {true, false, id, interface, alt};
  // Its streams end first.
  for (auto &s : this->xs_->iso) {
    if (s.on && this->eps_[ep_index(s.ep)].interface == interface)
      this->stop_iso_(0, s.ep, false);
  }
  for (auto &in : this->xs_->int_in) {
    if (in.on && this->eps_[ep_index(in.ep)].interface == interface)
      this->stop_int_(0, in.ep, false);
  }
  this->rx_ = Rx::WAIT_ALT;
  return this->alt_step_();
}

bool SpiceUsbSlot::alt_step_() {
  if (!this->alt_.active || this->alt_.sent || this->interface_busy_(this->alt_.interface))
    return true;
  Xfer *x = this->gen_take_();
  if (x == nullptr)
    return true;
  x->kind = Kind::SET_ALT;
  this->alt_.sent = true;
  usb_transfer_t *t = x->t;
  bool sent = false;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  if (this->usable_()) {
    uint8_t *setup = t->data_buffer;
    setup[0] = 0x01;  // host to device, standard, interface
    setup[1] = REQ_SET_INTERFACE;
    put16(setup + 2, this->alt_.alt);
    put16(setup + 4, this->alt_.interface);
    put16(setup + 6, 0);
    t->device_handle = this->device_handle_;
    t->bEndpointAddress = 0;
    t->num_bytes = 8;
    t->timeout_ms = 0;
    t->flags = 0;
    sent = usb_host_transfer_submit_control(this->handle_, t) == ESP_OK;
  }
  xSemaphoreGive(this->dev_lock_);
  if (sent) {
    x->in_flight = true;
    return true;
  }
  x->busy = false;
  return this->alt_done_(false);
}

bool SpiceUsbSlot::alt_done_(bool ok) {
  const uint8_t interface = this->alt_.interface;
  uint8_t st = ST_IOERROR;
  bool sent = true;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  // ESP-IDF opens the endpoints of the alternate setting an interface is claimed with.
  if (ok && this->usable_() && this->release_interface(interface)) {
    if (this->claim_interface(interface, this->alt_.alt)) {
      this->alts_[interface] = this->alt_.alt;
    } else {
      this->claim_interface(interface, this->alts_[interface]);
      ok = false;
    }
    this->parse_eps_();
    if (ok) {
      sent = this->send_device_info_();
      st = ST_SUCCESS;
    }
  }
  xSemaphoreGive(this->dev_lock_);
  this->alt_.active = false;
  this->rx_ = Rx::HEADER;
  uint8_t s[3] = {st, interface, this->alts_[interface]};
  return sent && this->send_(REDIR_ALT_SETTING_STATUS, this->alt_.id, s, sizeof(s));
}

bool SpiceUsbSlot::start_int_(uint64_t id, uint8_t ep) {
  uint8_t st = ST_INVAL;
  const uint8_t idx = ep_index(ep);
  IntIn *in = nullptr;
  if ((ep & 0x80) != 0 && this->eps_[idx].type == TYPE_INTERRUPT && this->eps_[idx].mps != 0 &&
      this->eps_[idx].mps <= INT_BYTES && this->usable_()) {
    bool running = false;
    for (auto &i : this->xs_->int_in)
      running = running || ((i.on || i.stopping) && i.ep == ep);
    for (auto &i : this->xs_->int_in) {
      if (!running && in == nullptr && !i.on && !i.stopping)
        in = &i;
    }
  }
  Xfer *taken[INT_PER_EP]{};
  uint8_t count = 0;
  if (in != nullptr) {
    for (auto &x : this->xs_->intr) {
      if (count < INT_PER_EP && !x.busy)
        taken[count++] = &x;
    }
  }
  if (in != nullptr && count == INT_PER_EP) {
    st = ST_SUCCESS;
    in->ep = ep;
    in->next_id = 0;
    in->on = true;
    in->stopping = false;
    xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
    for (uint8_t i = 0; i < INT_PER_EP; i++) {
      Xfer *x = taken[i];
      x->busy = true;
      x->kind = Kind::INTERRUPT_IN;  // USB/IP uses them for URBs of either direction
      x->intr = in;
      in->x[i] = x;
      usb_transfer_t *t = x->t;
      t->device_handle = this->device_handle_;
      t->bEndpointAddress = ep;
      t->num_bytes = this->eps_[idx].mps;
      t->timeout_ms = 0;
      t->flags = 0;
      if (this->usable_() && usb_host_transfer_submit(t) == ESP_OK) {
        x->in_flight = true;
      } else {
        x->busy = false;
        x->intr = nullptr;
        in->x[i] = nullptr;
        st = ST_IOERROR;
      }
    }
    xSemaphoreGive(this->dev_lock_);
    if (st != ST_SUCCESS) {
      in->stopping = true;  // what went out comes back and frees it
      bool any = false;
      for (auto *x : in->x)
        any = any || x != nullptr;
      if (any) {
        this->cancel_ep_(ep);
      } else {
        in->on = false;
        in->stopping = false;
      }
    }
  } else if (in != nullptr) {
    st = ST_IOERROR;
  }
  uint8_t s[2] = {st, ep};
  return this->send_(REDIR_INTERRUPT_RECEIVING_STATUS, id, s, sizeof(s));
}

bool SpiceUsbSlot::stop_int_(uint64_t id, uint8_t ep, bool reply) {
  for (auto &in : this->xs_->int_in) {
    if (in.on && !in.stopping && in.ep == ep) {
      in.stopping = true;
      this->cancel_ep_(ep);  // freed as they come back (complete_())
    }
  }
  if (!reply)
    return true;
  uint8_t s[2] = {ST_SUCCESS, ep};
  return this->send_(REDIR_INTERRUPT_RECEIVING_STATUS, id, s, sizeof(s));
}

bool SpiceUsbSlot::start_iso_(uint64_t id, uint8_t ep, uint8_t pkts, uint8_t urbs) {
  uint8_t st = ST_INVAL;
  const uint8_t idx = ep_index(ep);
  const uint16_t mps = this->eps_[idx].mps;
  IsoStream *s = nullptr;
  if (this->eps_[idx].type == TYPE_ISO && mps != 0 && pkts >= 1 && pkts <= ISO_PKTS && urbs >= 1 && urbs <= ISO_URBS &&
      this->usable_()) {
    bool running = false;
    for (auto &i : this->xs_->iso)
      running = running || (i.urbs != 0 && i.ep == ep);
    for (auto &i : this->xs_->iso) {
      if (!running && s == nullptr && i.urbs == 0)
        s = &i;
    }
  }
  if (s != nullptr) {
    st = ST_SUCCESS;
    *s = IsoStream{};
    s->ep = ep;
    s->pkts = pkts;
    s->pkt_size = mps;
    s->urbs = urbs;
    // Sized by the guest's request: made for this stream, freed when it ends.
    for (uint8_t i = 0; i < urbs && st == ST_SUCCESS; i++) {
      Xfer &x = s->x[i];
      if (!this->xfer_alloc_(&x.t, static_cast<size_t>(mps) * pkts, pkts)) {
        st = ST_IOERROR;
        break;
      }
      x.slot = this;
      x.kind = Kind::ISO;
      x.iso = s;
      x.busy = true;
      x.t->callback = xfer_done_;
      x.t->context = &x;
      x.t->bEndpointAddress = ep;
      x.t->num_bytes = static_cast<int>(mps) * pkts;
      x.t->timeout_ms = 0;
      x.t->flags = 0;
      for (int p = 0; p < pkts; p++)
        x.t->isoc_packet_desc[p].num_bytes = mps;
    }
    if (st == ST_SUCCESS) {
      s->on = true;
      if ((ep & 0x80) != 0) {
        // Input: all of them go out now.
        s->started = true;
        xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
        for (uint8_t i = 0; i < urbs; i++) {
          Xfer &x = s->x[i];
          x.t->device_handle = this->device_handle_;
          if (this->usable_() && usb_host_transfer_submit(x.t) == ESP_OK) {
            x.in_flight = true;
          } else {
            st = ST_IOERROR;
          }
        }
        xSemaphoreGive(this->dev_lock_);
      }
    }
    if (st != ST_SUCCESS) {
      s->on = false;
      bool any = false;
      for (uint8_t i = 0; i < urbs; i++)
        any = any || s->x[i].in_flight;
      if (any) {
        s->stopping = true;
        this->cancel_ep_(ep);  // freed as they come back
      } else {
        for (uint8_t i = 0; i < urbs; i++) {
          if (s->x[i].t != nullptr)
            this->xfer_free_(s->x[i].t);
          s->x[i] = Xfer{};
        }
        s->urbs = 0;
      }
    }
  }
  uint8_t r[2] = {st, ep};
  return this->send_(REDIR_ISO_STREAM_STATUS, id, r, sizeof(r));
}

bool SpiceUsbSlot::stop_iso_(uint64_t id, uint8_t ep, bool reply) {
  for (auto &s : this->xs_->iso) {
    if (s.urbs == 0 || s.ep != ep || s.stopping)
      continue;
    s.stopping = true;
    s.on = false;
    bool any = false;
    for (uint8_t i = 0; i < s.urbs; i++)
      any = any || s.x[i].in_flight;
    if (any) {
      this->cancel_ep_(ep);  // freed as they come back (complete_())
    } else {
      for (uint8_t i = 0; i < s.urbs; i++) {
        if (s.x[i].t != nullptr)
          this->xfer_free_(s.x[i].t);
        s.x[i] = Xfer{};
      }
      s.urbs = 0;
      s.stopping = false;
    }
  }
  if (!reply)
    return true;
  uint8_t r[2] = {ST_SUCCESS, ep};
  return this->send_(REDIR_ISO_STREAM_STATUS, id, r, sizeof(r));
}

// An isochronous OUT packet from the guest: into the URB being filled; the stream starts once half of
// its packets are buffered, and drops back to half after an overflow (usbredirhost_iso_packet()).
bool SpiceUsbSlot::iso_out_(uint8_t ep, const uint8_t *data, size_t len) {
  IsoStream *s = nullptr;
  for (auto &i : this->xs_->iso) {
    if (i.on && i.ep == ep && (ep & 0x80) == 0)
      s = &i;
  }
  if (s == nullptr || len > s->pkt_size)
    return true;
  if (s->drop > 0) {
    s->drop--;
    return true;
  }
  Xfer &x = s->x[s->fill];
  if (x.in_flight) {
    s->drop = static_cast<uint16_t>(s->pkts * s->urbs / 2);
    return true;
  }
  usb_transfer_t *t = x.t;
  std::memcpy(t->data_buffer + static_cast<size_t>(x.filled) * s->pkt_size, data, len);
  t->isoc_packet_desc[x.filled].num_bytes = static_cast<int>(len);
  x.filled++;
  if (x.filled < s->pkts)
    return true;
  s->fill = static_cast<uint8_t>((s->fill + 1) % s->urbs);
  auto submit = [this, s](Xfer &u) {
    u.t->device_handle = this->device_handle_;
    u.t->num_bytes = static_cast<int>(s->pkt_size) * s->pkts;
    if (usb_host_transfer_submit(u.t) == ESP_OK)
      u.in_flight = true;
  };
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  if (this->usable_()) {
    if (s->started) {
      submit(x);
    } else {
      uint16_t full = 0;
      for (uint8_t i = 0; i < s->urbs; i++)
        full = static_cast<uint16_t>(full + (s->x[i].filled == s->pkts ? s->pkts : 0));
      if (full >= s->pkts * s->urbs / 2) {
        s->started = true;
        for (uint8_t i = 0; i < s->urbs; i++) {
          if (s->x[i].filled == s->pkts && !s->x[i].in_flight)
            submit(s->x[i]);
        }
      }
    }
  }
  xSemaphoreGive(this->dev_lock_);
  return true;
}

void SpiceUsbSlot::drain_() {
  if (this->xs_ == nullptr)
    return;  // no session's transfers
  // Streams and receivers stop; every transfer comes back (cancelled, or with the device gone).
  for (auto &s : this->xs_->iso) {
    if (s.urbs != 0)
      this->stop_iso_(0, s.ep, false);
  }
  for (auto &in : this->xs_->int_in) {
    if (in.on)
      this->stop_int_(0, in.ep, false);
  }
  for (auto &x : this->xs_->gen) {
    if (x.in_flight && x.kind != Kind::CONTROL && x.kind != Kind::SET_ALT)
      this->cancel_ep_(x.ep);
  }
  // USB/IP's URBs on interrupt and isochronous endpoints too.
  for (auto *pool : {static_cast<Xfer *>(this->xs_->intr), static_cast<Xfer *>(this->usbip_iso_)}) {
    const size_t count = pool == this->xs_->intr ? INT_XFERS : USBIP_ISO_XFERS;
    for (size_t i = 0; i < count; i++) {
      if (this->proto_ == Proto::USBIP && pool[i].in_flight)
        this->cancel_ep_(pool[i].ep);
    }
  }
  if (this->rx_xfer_ != nullptr) {
    this->rx_xfer_->busy = false;
    this->rx_xfer_ = nullptr;
  }
  this->alt_.active = false;
  this->rx_ = Rx::HEADER;
  this->have_ = 0;
  // Nothing more to the guest from here. A control transfer cannot be cancelled; the device answers it.
  static constexpr uint32_t DRAIN_MS = 1000;
  SpiceVmcLink *link = this->link_;
  this->link_ = nullptr;
  const uint32_t start = millis();
  while (true) {
    Xfer *x;
    while (xQueueReceive(this->done_q_, &x, 0) == pdTRUE) {
      x->in_flight = false;
      this->complete_(x);
    }
    bool any = false;
    for (auto &g : this->xs_->gen)
      any = any || g.in_flight;
    for (auto &i : this->xs_->intr)
      any = any || i.in_flight;
    for (uint8_t i = 0; i < USBIP_ISO_XFERS; i++)
      any = any || this->usbip_iso_[i].in_flight;
    for (auto &s : this->xs_->iso) {
      for (uint8_t i = 0; i < s.urbs; i++)
        any = any || s.x[i].in_flight;
    }
    if (!any || millis() - start > DRAIN_MS)
      break;
    if (xQueueReceive(this->done_q_, &x, pdMS_TO_TICKS(20)) == pdTRUE) {
      x->in_flight = false;
      this->complete_(x);
    }
  }
  this->link_ = link;
}

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE_USB
