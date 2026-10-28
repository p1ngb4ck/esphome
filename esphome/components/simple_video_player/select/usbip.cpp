// USB/IP export of a slot's device (a VNC session's USB): the panel is the USB/IP server's stub side.
// Wire layouts as the kernel's Documentation/usb/usbip_protocol.rst and drivers/usb/usbip/
// usbip_common.h (all fields big-endian; the setup packet as on the bus); the handling of URBs, their
// unlinking and the requests the stub takes itself (SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE
// ENDPOINT_HALT) after drivers/usb/usbip/stub_rx.c. The device, its transfers and their completions
// are the slot's (spice_usb.cpp); the connection is the spice task's (spice_client.cpp).

#include "spice_usb.h"

#if defined(SVP_SPICE_USB) && defined(SVP_DESKTOP_VNC)

#include "esphome/core/log.h"
#include "esp_memory_utils.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::simple_video_player {

static const char *const TAG = "simple_video_player.usbip";

namespace {

constexpr uint32_t CMD_SUBMIT = 1, CMD_UNLINK = 2, RET_SUBMIT = 3, RET_UNLINK = 4;
constexpr size_t HEADER_BYTES = 48;
constexpr size_t ISO_DESC_BYTES = 16;
constexpr uint16_t VERSION = 0x0111;
constexpr uint16_t REP_IMPORT = 0x0003;
constexpr size_t ENTRY_BYTES = 0x138;  // a device as OP_REP_DEVLIST lists it, before its interfaces
// Linux errno values the client's URBs complete with.
constexpr int32_t E_PIPE = -32, E_NODEV = -19, E_PROTO = -71, E_OVERFLOW = -75, E_CONNRESET = -104,
                  E_TIMEDOUT = -110, E_XDEV = -18, E_INVAL = -22;
// Standard requests the stub handles itself.
constexpr uint8_t REQ_CLEAR_FEATURE = 1, REQ_SET_CONFIGURATION = 9, REQ_SET_INTERFACE = 11;
constexpr uint8_t TYPE_CONTROL = 0, TYPE_ISO = 1, TYPE_BULK = 2, TYPE_INTERRUPT = 3;
constexpr size_t BULK_MAX = 131071;  // spice_usb.cpp: one qTD

inline uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline void put_be32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
inline void put_be16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint8_t ep_index(uint8_t ep) { return static_cast<uint8_t>(((ep & 0x80) >> 3) | (ep & 0x0F)); }

int32_t urb_status(usb_transfer_status_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
      return 0;
    case USB_TRANSFER_STATUS_TIMED_OUT:
      return E_TIMEDOUT;
    case USB_TRANSFER_STATUS_CANCELED:
      return E_CONNRESET;
    case USB_TRANSFER_STATUS_STALL:
      return E_PIPE;
    case USB_TRANSFER_STATUS_OVERFLOW:
      return E_OVERFLOW;
    case USB_TRANSFER_STATUS_NO_DEVICE:
      return E_NODEV;
    case USB_TRANSFER_STATUS_SKIPPED:
      return E_XDEV;
    default:
      return E_PROTO;
  }
}

// RET_SUBMIT / RET_UNLINK header: command, seqnum, devid, direction, ep (0 from the server), then
// status, actual_length, start_frame, number_of_packets, error_count, padding.
void ret_header(uint8_t *h, uint32_t command, uint32_t seqnum, int32_t status, uint32_t actual, int32_t packets,
                uint32_t errors) {
  std::memset(h, 0, HEADER_BYTES);
  put_be32(h, command);
  put_be32(h + 4, seqnum);
  put_be32(h + 20, static_cast<uint32_t>(status));
  put_be32(h + 24, actual);
  put_be32(h + 32, static_cast<uint32_t>(packets));
  put_be32(h + 36, errors);
}

}  // namespace

//========================================================================
// Main loop: the picked device as OP_REP_DEVLIST lists it
//========================================================================

void SpiceUsbSlot::usbip_snapshot_() {
  if (this->usbip_lock_ == nullptr)
    return;
  uint8_t e[USBIP_ENTRY_BYTES] = {};
  size_t len = 0;
  uint8_t ifaces = 0;
  const unsigned index = this->index_;
  for (const auto &dev : this->usb_->get_devices()) {
    if (this->want_addr_ == 0 || dev.addr != this->want_addr_ || dev.gone || dev.desc == nullptr)
      continue;
    std::snprintf(reinterpret_cast<char *>(e), 256, "/sys/devices/esphome/usb1/1-%u", index + 1);
    std::snprintf(reinterpret_cast<char *>(e + 0x100), 32, "1-%u", index + 1);  // the bus id
    put_be32(e + 0x120, 1);         // busnum
    put_be32(e + 0x124, dev.addr);  // devnum
    // Linux's enum usb_device_speed: 1 low, 2 full, 3 high.
    put_be32(e + 0x128, dev.info.speed == USB_SPEED_LOW ? 1 : dev.info.speed == USB_SPEED_FULL ? 2 : 3);
    put_be16(e + 0x12C, dev.desc->idVendor);
    put_be16(e + 0x12E, dev.desc->idProduct);
    put_be16(e + 0x130, dev.desc->bcdDevice);
    e[0x132] = dev.desc->bDeviceClass;
    e[0x133] = dev.desc->bDeviceSubClass;
    e[0x134] = dev.desc->bDeviceProtocol;
    e[0x135] = dev.info.bConfigurationValue;
    e[0x136] = dev.desc->bNumConfigurations;
    // Its interfaces (alternate setting 0): class, subclass, protocol, padding.
    if (dev.config != nullptr) {
      const uint8_t *p = reinterpret_cast<const uint8_t *>(dev.config);
      const uint16_t total = dev.config->wTotalLength;
      for (uint16_t at = 0; at + 2 <= total && p[at] >= 2 && ifaces < 32; at += p[at]) {
        if (p[at + 1] != 4 || at + 8 > total || p[at + 3] != 0)
          continue;
        e[ENTRY_BYTES + 4 * ifaces] = p[at + 5];
        e[ENTRY_BYTES + 4 * ifaces + 1] = p[at + 6];
        e[ENTRY_BYTES + 4 * ifaces + 2] = p[at + 7];
        ifaces++;
      }
    }
    e[0x137] = ifaces;
    len = ENTRY_BYTES;
  }
  xSemaphoreTake(this->usbip_lock_, portMAX_DELAY);
  std::memcpy(this->usbip_entry_, e, USBIP_ENTRY_BYTES);
  this->usbip_entry_len_ = len;
  this->usbip_entry_ifaces_ = ifaces;
  xSemaphoreGive(this->usbip_lock_);
}

size_t SpiceUsbSlot::usbip_entry(uint8_t *out, size_t cap, bool interfaces) const {
  if (this->usbip_lock_ == nullptr)
    return 0;
  xSemaphoreTake(this->usbip_lock_, portMAX_DELAY);
  size_t len = this->usbip_entry_len_;
  if (len != 0 && interfaces)
    len += 4 * static_cast<size_t>(this->usbip_entry_ifaces_);
  if (len > cap)
    len = 0;
  if (len != 0)
    std::memcpy(out, this->usbip_entry_, len);
  xSemaphoreGive(this->usbip_lock_);
  return len;
}

//========================================================================
// Spice task: the URBs
//========================================================================

bool SpiceUsbSlot::usbip_service_() {
  Xfer *x;
  while (xQueueReceive(this->done_q_, &x, 0) == pdTRUE) {
    x->in_flight = false;
    if (!this->usbip_complete_(x))
      return false;
  }
  // The device goes (unplugged, picked elsewhere, or taken back): the client's connection ends with it.
  if (this->drain_req_.load(std::memory_order_acquire) && !this->drained_.load(std::memory_order_acquire)) {
    this->drain_();
    this->drained_.store(true, std::memory_order_release);
    this->enable_loop_soon_any_context();
    return false;
  }
  if (this->announced_ && !this->usable_())
    return false;
  if (!this->announced_ && this->usable_()) {
    // OP_REP_IMPORT once the device is opened here: the client attaches it then.
    uint8_t reply[8 + ENTRY_BYTES] = {};
    put_be16(reply, VERSION);
    put_be16(reply + 2, REP_IMPORT);
    if (this->usbip_entry(reply + 8, ENTRY_BYTES, false) != ENTRY_BYTES)
      return false;
    if (!this->link_->vmc_send(reply, sizeof(reply), nullptr, 0))
      return false;
    this->announced_ = true;
  }
  if (this->rx_ == Rx::WAIT_DATA)
    return this->usbip_begin_submit_();
  return true;
}

uint8_t *SpiceUsbSlot::usbip_rx_space_(size_t &room) {
  switch (this->rx_) {
    case Rx::HEADER:
      room = this->announced_ ? HEADER_BYTES - this->have_ : 0;  // nothing comes before the reply
      return this->hdr_ + this->have_;
    case Rx::DATA: {
      room = this->data_len_ - this->have_;
      const size_t setup = this->rx_xfer_->kind == Kind::CONTROL ? 8 : 0;
      return this->rx_xfer_->t->data_buffer + setup + this->have_;
    }
    case Rx::BODY:
      room = this->body_len_ - this->have_;
      return this->stage_ + this->have_;
    case Rx::SKIP:
      room = std::min<size_t>(this->skip_left_, STAGE_BYTES);
      return this->stage_;
    default:
      room = 0;
      return this->stage_;
  }
}

bool SpiceUsbSlot::usbip_rx_done_(size_t n) {
  this->have_ += n;
  switch (this->rx_) {
    case Rx::HEADER: {
      if (this->have_ < HEADER_BYTES)
        return true;
      this->have_ = 0;
      const uint32_t command = be32(this->hdr_);
      this->id_ = be32(this->hdr_ + 4);
      if (command == CMD_SUBMIT)
        return this->usbip_begin_submit_();
      if (command == CMD_UNLINK)
        return this->usbip_unlink_(static_cast<uint32_t>(this->id_), be32(this->hdr_ + 20));
      ESP_LOGW(TAG, "Unknown USB/IP command %" PRIu32, command);
      return false;
    }
    case Rx::DATA: {
      if (this->have_ < this->data_len_)
        return true;
      this->have_ = 0;
      Xfer *x = this->rx_xfer_;
      if (this->usbip_packets_ > 0) {
        this->body_len_ = static_cast<uint32_t>(this->usbip_packets_) * ISO_DESC_BYTES;
        this->rx_ = Rx::BODY;
        return true;
      }
      this->rx_xfer_ = nullptr;
      this->rx_ = Rx::HEADER;
      return this->usbip_submit_(x);
    }
    case Rx::BODY: {
      if (this->have_ < this->body_len_)
        return true;
      this->have_ = 0;
      this->rx_ = Rx::HEADER;
      Xfer *x = this->rx_xfer_;
      this->rx_xfer_ = nullptr;
      // The packets as the client laid them: their sizes here, their offsets for the answer.
      uint32_t *offsets = this->usbip_offsets_ + static_cast<size_t>(x->usbip_index) * USBIP_ISO_PKTS;
      size_t total = 0;
      for (uint16_t i = 0; i < x->usbip_packets; i++) {
        const uint8_t *d = this->stage_ + i * ISO_DESC_BYTES;
        offsets[i] = be32(d);
        x->t->isoc_packet_desc[i].num_bytes = static_cast<int>(be32(d + 4));
        total += be32(d + 4);
      }
      if (total > x->t->data_buffer_size || (!x->usbip_in && total != x->usbip_len)) {
        x->t->status = USB_TRANSFER_STATUS_ERROR;
        x->t->actual_num_bytes = 0;
        return this->usbip_complete_(x);
      }
      x->t->num_bytes = static_cast<int>(total);
      return this->usbip_submit_(x);
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

SpiceUsbSlot::Xfer *SpiceUsbSlot::usbip_take_(Kind kind, size_t bytes, uint16_t packets) {
  if (kind == Kind::CONTROL || kind == Kind::BULK)
    return this->gen_take_();
  if (kind == Kind::INTERRUPT_IN || kind == Kind::INTERRUPT_OUT) {
    for (auto &x : this->xs_->intr) {
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
  // Isochronous: a transfer of this many packets, kept for the next URB of the same shape.
  for (uint8_t i = 0; i < USBIP_ISO_XFERS; i++) {
    Xfer &x = this->usbip_iso_[i];
    if (x.busy)
      continue;
    usb_transfer_t *t = x.t;
    if (t == nullptr || t->num_isoc_packets != packets || t->data_buffer_size < bytes) {
      this->xfer_free_(t);
      t = nullptr;
      if (!this->xfer_alloc_(&t, bytes, packets)) {
        x.t = nullptr;
        return nullptr;
      }
    }
    x = Xfer{t, this};
    t->callback = xfer_done_;
    t->context = &x;
    x.busy = true;
    x.usbip_index = i;
    return &x;
  }
  return nullptr;
}

// CMD_SUBMIT, its header in hdr_: a transfer for it (WAIT_DATA while none is free), then its OUT data
// and isochronous packet descriptors.
bool SpiceUsbSlot::usbip_begin_submit_() {
  const uint8_t *h = this->hdr_;
  const bool in = be32(h + 12) != 0;
  const uint8_t ep = static_cast<uint8_t>((be32(h + 16) & 0x0F) | (in ? 0x80 : 0));
  const uint32_t len = be32(h + 24);
  const auto packets = static_cast<int32_t>(be32(h + 32));
  const uint8_t *setup = h + 40;
  this->rx_ = Rx::HEADER;
  const uint8_t type = (ep & 0x0F) == 0 ? TYPE_CONTROL : this->eps_[ep_index(ep)].type;
  Kind kind;
  size_t bytes = len;
  const uint16_t mps = this->eps_[ep_index(ep)].mps;
  auto whole = [mps](size_t n) { return mps != 0 ? (n + mps - 1) / mps * mps : n; };
  switch (type) {
    case TYPE_CONTROL:
      kind = Kind::CONTROL;
      bytes = 8 + (in ? whole(len) : len);
      break;
    case TYPE_BULK:
      kind = Kind::BULK;
      bytes = in ? whole(len) : len;
      break;
    case TYPE_INTERRUPT:
      kind = in ? Kind::INTERRUPT_IN : Kind::INTERRUPT_OUT;
      bytes = in ? whole(len) : len;
      break;
    case TYPE_ISO:
      kind = Kind::ISO;
      break;
    default:
      kind = Kind::SET_ALT;  // none: answered as failed below
      break;
  }
  const bool iso_ok = kind != Kind::ISO || (packets > 0 && packets <= USBIP_ISO_PKTS);
  const size_t follows = (in ? 0 : len) + (packets > 0 ? static_cast<size_t>(packets) * ISO_DESC_BYTES : 0);
  bool fits = kind != Kind::SET_ALT && iso_ok && (kind != Kind::BULK || bytes <= BULK_MAX);
  Xfer *x = nullptr;
  if (fits) {
    // An isochronous URB's buffer: what the descriptors ask for, at most a packet size each.
    const size_t want = kind == Kind::ISO ? std::max<size_t>(len, static_cast<size_t>(packets) * mps) : bytes;
    bool free = kind != Kind::ISO;
    for (uint8_t i = 0; i < USBIP_ISO_XFERS; i++)
      free = free || !this->usbip_iso_[i].busy;
    x = free ? this->usbip_take_(kind, want, kind == Kind::ISO ? static_cast<uint16_t>(packets) : 0) : nullptr;
    if (x == nullptr && (kind != Kind::ISO || !free)) {
      this->rx_ = Rx::WAIT_DATA;  // the header waits in hdr_ for a free transfer
      return true;
    }
    fits = x != nullptr && x->t->data_buffer_size >= want;
    if (x != nullptr && !fits)
      x->busy = false;
  }
  if (!fits) {
    // Not one the device or a transfer here takes: answered as failed, what follows counted out.
    uint8_t r[HEADER_BYTES];
    ret_header(r, RET_SUBMIT, static_cast<uint32_t>(this->id_), kind == Kind::SET_ALT ? E_PIPE : E_INVAL, 0,
               packets > 0 ? packets : -1, 0);
    if (follows > 0) {
      this->skip_left_ = static_cast<uint32_t>(follows);
      this->rx_ = Rx::SKIP;
    }
    return this->link_->vmc_send(r, sizeof(r), nullptr, 0);
  }
  x->kind = kind;
  x->id = this->id_;
  x->ep = ep;
  x->usbip_in = in;
  x->usbip_len = len;
  x->usbip_packets = kind == Kind::ISO ? static_cast<uint16_t>(packets) : 0;
  this->usbip_packets_ = kind == Kind::ISO ? packets : -1;
  if (kind == Kind::CONTROL) {
    std::memcpy(x->t->data_buffer, setup, 8);
    x->requesttype = setup[0];
    x->request = setup[1];
    x->value = rd16(setup + 2);
    x->index = rd16(setup + 4);
    x->length = rd16(setup + 6);
  } else {
    x->length = static_cast<uint16_t>(len);
    x->want = len;
  }
  if (!in && len > 0) {
    this->rx_xfer_ = x;
    this->data_len_ = len;
    this->have_ = 0;
    this->rx_ = Rx::DATA;
    return true;
  }
  if (kind == Kind::ISO) {
    this->rx_xfer_ = x;
    this->body_len_ = static_cast<uint32_t>(packets) * ISO_DESC_BYTES;
    this->have_ = 0;
    this->rx_ = Rx::BODY;
    return true;
  }
  return this->usbip_submit_(x);
}

bool SpiceUsbSlot::usbip_submit_(Xfer *x) {
  usb_transfer_t *t = x->t;
  // SET_CONFIGURATION: ESP-IDF runs the configuration it enumerated; that one is set, no other.
  if (x->kind == Kind::CONTROL && x->requesttype == 0x00 && x->request == REQ_SET_CONFIGURATION) {
    t->status = x->value == this->config_value_ ? USB_TRANSFER_STATUS_COMPLETED : USB_TRANSFER_STATUS_STALL;
    t->actual_num_bytes = 8;
    return this->usbip_complete_(x);
  }
  bool ok = false;
  xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
  if (this->usable_() && this->announced_) {
    t->device_handle = this->device_handle_;
    t->timeout_ms = 0;
    t->flags = 0;
    const uint16_t mps = this->eps_[ep_index(x->ep)].mps;
    auto whole = [mps](size_t n) { return mps != 0 ? (n + mps - 1) / mps * mps : n; };
    switch (x->kind) {
      case Kind::CONTROL: {
        const bool in = (x->requesttype & 0x80) != 0;
        const uint16_t mps0 = this->eps_[0].mps != 0 ? this->eps_[0].mps : 64;
        const size_t data = in ? (static_cast<size_t>(x->length) + mps0 - 1) / mps0 * mps0 : x->length;
        t->bEndpointAddress = x->requesttype & 0x80;
        t->num_bytes = static_cast<int>(8 + data);
        ok = 8 + data <= t->data_buffer_size && usb_host_transfer_submit_control(this->handle_, t) == ESP_OK;
        break;
      }
      case Kind::BULK:
      case Kind::INTERRUPT_IN:
      case Kind::INTERRUPT_OUT:
        t->bEndpointAddress = x->ep;
        t->num_bytes = static_cast<int>(x->usbip_in ? whole(x->usbip_len) : x->usbip_len);
        ok = usb_host_transfer_submit(t) == ESP_OK;
        break;
      case Kind::ISO:
        t->bEndpointAddress = x->ep;
        ok = usb_host_transfer_submit(t) == ESP_OK;
        break;
      default:
        break;
    }
  }
  if (ok)
    x->in_flight = true;
  xSemaphoreGive(this->dev_lock_);
  if (ok)
    return true;
  t->status = USB_TRANSFER_STATUS_ERROR;
  t->actual_num_bytes = 0;
  return this->usbip_complete_(x);
}

// A URB back (or never submitted): RET_SUBMIT, or RET_UNLINK when it was unlinked meanwhile.
bool SpiceUsbSlot::usbip_complete_(Xfer *x) {
  usb_transfer_t *t = x->t;
  const int32_t status = urb_status(t->status);
  const auto got = static_cast<size_t>(std::max(t->actual_num_bytes, 0));
  const uint32_t seqnum = static_cast<uint32_t>(x->id);
  const uint32_t unlink = x->usbip_unlink;
  x->busy = false;
  if (this->link_ == nullptr)
    return true;  // the connection ends (drain_()): nothing more to the client
  if (status != 0 && x->kind != Kind::CONTROL && x->kind != Kind::ISO && t->status == USB_TRANSFER_STATUS_STALL) {
    // The host side of a stalled endpoint takes transfers again; the client clears the device's halt.
    xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
    if (this->usable_())
      usb_host_endpoint_clear(this->device_handle_, x->ep);
    xSemaphoreGive(this->dev_lock_);
  }
  if (unlink != 0 && t->status == USB_TRANSFER_STATUS_CANCELED)
    return this->usbip_send_ret_unlink_(unlink, E_CONNRESET);  // no RET_SUBMIT for it
  uint8_t h[HEADER_BYTES];
  bool ok = true;
  switch (x->kind) {
    case Kind::CONTROL: {
      const bool in = (x->requesttype & 0x80) != 0;
      const size_t data = std::min<size_t>(got > 8 ? got - 8 : 0, x->length);
      if (status == 0 && x->requesttype == 0x01 && x->request == REQ_SET_INTERFACE && x->index < 32) {
        // The new alternate setting's endpoints are the ones ESP-IDF opens for the claimed interface.
        const uint8_t interface = static_cast<uint8_t>(x->index);
        xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
        if (this->usable_() && this->release_interface(interface)) {
          if (this->claim_interface(interface, static_cast<uint8_t>(x->value))) {
            this->alts_[interface] = static_cast<uint8_t>(x->value);
          } else {
            this->claim_interface(interface, this->alts_[interface]);
          }
          this->parse_eps_();
        }
        xSemaphoreGive(this->dev_lock_);
      }
      if (status == 0 && x->requesttype == 0x02 && x->request == REQ_CLEAR_FEATURE && x->value == 0) {
        // ENDPOINT_HALT cleared on the device: and on the host side here.
        xSemaphoreTake(this->dev_lock_, portMAX_DELAY);
        if (this->usable_())
          usb_host_endpoint_clear(this->device_handle_, static_cast<uint8_t>(x->index));
        xSemaphoreGive(this->dev_lock_);
      }
      ret_header(h, RET_SUBMIT, seqnum, status, static_cast<uint32_t>(in ? data : (status == 0 ? x->length : 0)), -1,
                 0);
      ok = this->link_->vmc_send(h, sizeof(h), in ? t->data_buffer + 8 : nullptr, in ? data : 0);
      break;
    }
    case Kind::BULK:
    case Kind::INTERRUPT_IN:
    case Kind::INTERRUPT_OUT: {
      int32_t st = status;
      size_t data = x->usbip_in ? got : (status == 0 ? x->usbip_len : got);
      if (x->usbip_in && data > x->usbip_len) {
        st = E_OVERFLOW;  // more than was asked for: the transfer was rounded up to whole packets
        data = x->usbip_len;
      }
      ret_header(h, RET_SUBMIT, seqnum, st, static_cast<uint32_t>(data), -1, 0);
      ok = this->link_->vmc_send(h, sizeof(h), x->usbip_in ? t->data_buffer : nullptr, x->usbip_in ? data : 0);
      break;
    }
    case Kind::ISO: {
      // IN data without the gaps between the packets, then each packet's descriptor.
      const uint32_t *offsets = this->usbip_offsets_ + static_cast<size_t>(x->usbip_index) * USBIP_ISO_PKTS;
      uint8_t desc[USBIP_ISO_PKTS * ISO_DESC_BYTES];
      size_t packed = 0, at = 0;
      uint32_t actual = 0, errors = 0;
      for (uint16_t i = 0; i < x->usbip_packets; i++) {
        const usb_isoc_packet_desc_t &d = t->isoc_packet_desc[i];
        const auto n = static_cast<size_t>(std::max(d.actual_num_bytes, 0));
        const int32_t pst = urb_status(d.status);
        if (x->usbip_in && n > 0) {
          std::memmove(t->data_buffer + packed, t->data_buffer + at, n);
          packed += n;
        }
        at += static_cast<size_t>(std::max(d.num_bytes, 0));
        actual += static_cast<uint32_t>(n);
        errors += pst != 0 ? 1 : 0;
        uint8_t *e = desc + i * ISO_DESC_BYTES;
        put_be32(e, offsets[i]);
        put_be32(e + 4, static_cast<uint32_t>(std::max(d.num_bytes, 0)));
        put_be32(e + 8, static_cast<uint32_t>(n));
        put_be32(e + 12, static_cast<uint32_t>(pst));
      }
      ret_header(h, RET_SUBMIT, seqnum, status, actual, x->usbip_packets, errors);
      ok = this->link_->vmc_send(h, sizeof(h), x->usbip_in ? t->data_buffer : nullptr, x->usbip_in ? packed : 0) &&
           this->link_->vmc_send(desc, static_cast<size_t>(x->usbip_packets) * ISO_DESC_BYTES, nullptr, 0);
      break;
    }
    default:
      break;
  }
  if (ok && unlink != 0)
    ok = this->usbip_send_ret_unlink_(unlink, 0);  // it completed before the unlink took
  return ok;
}

bool SpiceUsbSlot::usbip_send_ret_unlink_(uint32_t seqnum, int32_t status) {
  uint8_t h[HEADER_BYTES];
  ret_header(h, RET_UNLINK, seqnum, status, 0, 0, 0);
  return this->link_->vmc_send(h, sizeof(h), nullptr, 0);
}

// CMD_UNLINK: the URB cancelled where it is in flight (ESP-IDF cancels by endpoint); one already
// answered, or a control transfer (which cannot be cancelled), unlinks after its answer.
bool SpiceUsbSlot::usbip_unlink_(uint32_t seqnum, uint32_t target) {
  auto find = [target](Xfer *pool, size_t count) -> Xfer * {
    for (size_t i = 0; i < count; i++) {
      if (pool[i].busy && pool[i].in_flight && static_cast<uint32_t>(pool[i].id) == target)
        return &pool[i];
    }
    return nullptr;
  };
  Xfer *x = find(this->xs_->gen, GEN_XFERS);
  if (x == nullptr)
    x = find(this->xs_->intr, INT_XFERS);
  if (x == nullptr)
    x = find(this->usbip_iso_, USBIP_ISO_XFERS);
  if (x == nullptr)
    return this->usbip_send_ret_unlink_(seqnum, 0);
  x->usbip_unlink = seqnum;
  if (x->kind != Kind::CONTROL)
    this->cancel_ep_(x->ep);
  return true;
}

}  // namespace esphome::simple_video_player

#endif  // SVP_SPICE_USB
