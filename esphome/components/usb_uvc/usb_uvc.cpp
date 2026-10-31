#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31)

#include "usb_uvc.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::usb_uvc {

static const char *const TAG = "usb_uvc";

// Packets per isochronous URB: one per (micro)frame at bInterval 1. 32 keep 32 (micro)frames per URB on the bus, so a
// resubmit that comes late does not lose packets (with 4, num_urbs x 4 = 12 microframes at high speed were too few:
// lost packets left holes in the frames). The HCD takes up to 61 (64 qTDs, 3 of them margin; hcd_dwc.c).
static constexpr uint8_t PACKETS_PER_URB = 32;

static inline uint16_t rd16(const uint8_t *d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }
static inline uint32_t rd32(const uint8_t *d) {
  return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) | (static_cast<uint32_t>(d[2]) << 16) |
         (static_cast<uint32_t>(d[3]) << 24);
}

static const char *format_name(const UvcFrameInfo &f, char *buf, size_t size) {
  if (f.format == UvcFormat::MJPEG)
    return "MJPEG";
  // The GUID's fourcc, printable characters only
  for (size_t i = 0; i < 4 && i + 1 < size; i++) {
    const char c = static_cast<char>((f.fourcc >> (8 * i)) & 0xFF);
    buf[i] = c >= 0x20 && c < 0x7F ? c : '?';
  }
  buf[std::min<size_t>(4, size - 1)] = '\0';
  return buf;
}

void UVCClient::loop() {
  for (size_t i = 0; i < this->cameras_.size(); i++) {
    Camera *cam = this->cameras_[i];
    if (cam->ready.load(std::memory_order_acquire)) {
      const uint8_t *data = cam->buffers[cam->ready_index];
      const UvcFrameInfo &info = cam->frames[cam->selected];
      cam->restarts = 0;  // it streams
      // What became of the stream's frames: one line per 10 s while frames come (checked per delivered frame)
      const uint32_t now = millis();
      if (cam->stats_since == 0) {
        cam->stats_since = now;
      } else if (now - cam->stats_since >= 10000) {
        ESP_LOGI(TAG, "Frames in %u s: %u whole, %u with a lost / skipped packet, %u broken, %u while the last was read",
                 static_cast<unsigned>((now - cam->stats_since) / 1000), cam->n_ok.exchange(0), cam->n_holes.exchange(0),
                 cam->n_bad.exchange(0), cam->n_busy.exchange(0));
        cam->stats_since = now;
      }
      for (auto &cb : this->frame_callbacks_)
        cb(static_cast<uint8_t>(cam->dev->addr), data, cam->ready_len, info);
      if (!cam->held)
        cam->ready.store(false, std::memory_order_release);
    }
    // A stream that stopped by itself (its URBs refused: the host halted the endpoint on a transfer error, seen right
    // after a start): closed and opened again while it is still wanted, as a switch does; given up after
    // MAX_RESTARTS in a row without a frame
    if (cam->state == StreamState::STREAMING && cam->stream.died.exchange(false, std::memory_order_acq_rel)) {
      if (cam->wanted && cam->restarts < MAX_RESTARTS) {
        cam->restarts++;
        ESP_LOGW(TAG, "The stream stopped; opening it again (%u of %u)", cam->restarts, MAX_RESTARTS);
        cam->reopen = true;
      } else {
        ESP_LOGW(TAG, "The stream stopped");
        cam->wanted = false;
      }
      this->close_(*cam);
    }
    // A closed stream: its buffers back; a switch opens it again in the new selection
    if (cam->state == StreamState::CLOSING && cam->stream.is_closed() && !cam->held) {
      this->free_buffers_(*cam);
      cam->state = StreamState::IDLE;
      if (cam->reopen && cam->wanted) {
        cam->reopen = false;
        this->open_(*cam);
      }
    }
  }
  usb_host::USBClient::loop();
}

UVCClient::Camera *UVCClient::find_(uint8_t camera) const {
  for (auto *cam : this->cameras_) {
    if (cam->dev->addr == camera)
      return cam;
  }
  return nullptr;
}

UVCClient::Camera *UVCClient::find_camera_(uint32_t id) const {
  for (auto *cam : this->cameras_) {
    if (cam->id == id)
      return cam;
  }
  return nullptr;
}

uint8_t UVCClient::camera_at(uint8_t index) const {
  return index < this->cameras_.size() ? static_cast<uint8_t>(this->cameras_[index]->dev->addr) : 0;
}

size_t UVCClient::frame_count(uint8_t camera) const {
  const Camera *cam = this->find_(camera);
  return cam != nullptr ? cam->frame_count : 0;
}

const UvcFrameInfo *UVCClient::frame_info(uint8_t camera, size_t index) const {
  const Camera *cam = this->find_(camera);
  return cam != nullptr && index < cam->frame_count ? &cam->frames[index] : nullptr;
}

int UVCClient::selected(uint8_t camera) const {
  const Camera *cam = this->find_(camera);
  return cam != nullptr ? cam->selected : -1;
}

uint32_t UVCClient::selected_interval(uint8_t camera) const {
  const Camera *cam = this->find_(camera);
  return cam != nullptr ? cam->interval : 0;
}

bool UVCClient::is_streaming(uint8_t camera) const {
  const Camera *cam = this->find_(camera);
  return cam != nullptr && cam->state == StreamState::STREAMING;
}

uint32_t UVCClient::hold_frame(uint8_t camera) {
  Camera *cam = this->find_(camera);
  if (cam == nullptr || !cam->ready.load(std::memory_order_acquire))
    return 0;
  cam->held++;
  return cam->id;
}

void UVCClient::release_frame(uint32_t token) {
  Camera *cam = this->find_camera_(token);
  if (cam != nullptr) {
    if (cam->held > 0 && --cam->held == 0) {
      cam->ready.store(false, std::memory_order_release);
      this->enable_loop();  // a stream closing meanwhile gives its buffers back now
    }
    return;
  }
  for (auto *gone : this->gone_) {
    if (gone->id == token) {
      if (gone->held > 0)
        gone->held--;
      this->enable_loop();  // the reap frees it now (device_busy_)
      return;
    }
  }
}

bool UVCClient::select(uint8_t camera, size_t index, uint32_t interval) {
  Camera *cam = this->find_(camera);
  if (cam == nullptr || index >= cam->frame_count)
    return false;
  cam->selected = static_cast<int16_t>(index);
  cam->interval = interval != 0 ? interval : cam->frames[index].default_interval;
  if (cam->state == StreamState::IDLE)
    return true;
  // Running (or on its way): closed, then opened again in the new selection
  cam->reopen = true;
  if (cam->state != StreamState::CLOSING)
    this->close_(*cam);
  return true;
}

void UVCClient::start(uint8_t camera) {
  Camera *cam = this->find_(camera);
  if (cam == nullptr)
    return;
  cam->starts++;
  cam->wanted = true;
  if (cam->state == StreamState::IDLE) {
    this->open_(*cam);
  } else if (cam->state == StreamState::CLOSING) {
    cam->reopen = true;
  }
}

void UVCClient::stop(uint8_t camera) {
  Camera *cam = this->find_(camera);
  if (cam == nullptr || cam->starts == 0)
    return;
  if (--cam->starts > 0)
    return;  // another one still wants it
  cam->wanted = false;
  cam->reopen = false;
  if (cam->state == StreamState::NEGOTIATING || cam->state == StreamState::STREAMING)
    this->close_(*cam);
}

// -- Descriptors ---------------------------------------------------------------------------

bool UVCClient::parse_descriptors_(Camera &cam) {
  const usb_config_desc_t *cfg = cam.dev->config;
  if (cfg == nullptr)
    return false;

  usb_device_info_t info;
  cam.high_speed = usb_host_device_info(cam.dev->handle, &info) == ESP_OK && info.speed == USB_SPEED_HIGH;

  uint16_t bcd_uvc = 0x0100;
  uint8_t cur_intf = 0, cur_alt = 0, cur_subclass = 0;
  int vs_intf = -1;  // the first streaming interface with formats
  UvcFormat format = UvcFormat::MJPEG;
  uint8_t format_index = 0;
  uint32_t fourcc = 0;
  bool in_format = false;
  std::vector<UvcFrameInfo> frames;  // while parsing only
  std::vector<AltSetting> alts;

  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  while ((desc = usb_parse_next_descriptor(desc, cfg->wTotalLength, &offset)) != nullptr) {
    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *intf = reinterpret_cast<const usb_intf_desc_t *>(desc);
      cur_intf = intf->bInterfaceNumber;
      cur_alt = intf->bAlternateSetting;
      cur_subclass = intf->bInterfaceClass == USB_CLASS_VIDEO ? intf->bInterfaceSubClass : 0;
      in_format = false;
      continue;
    }
    if (desc->bDescriptorType == UVC_CS_INTERFACE && cur_alt == 0 && desc->bLength >= 3) {
      const uint8_t subtype = d[2];
      if (cur_subclass == UVC_SC_VIDEOCONTROL && subtype == UVC_VC_HEADER && desc->bLength >= 5) {
        bcd_uvc = rd16(d + 3);
        continue;
      }
      if (cur_subclass != UVC_SC_VIDEOSTREAMING || (vs_intf >= 0 && cur_intf != vs_intf))
        continue;
      if ((subtype == UVC_VS_FORMAT_MJPEG && desc->bLength >= 11) ||
          ((subtype == UVC_VS_FORMAT_UNCOMPRESSED || subtype == UVC_VS_FORMAT_FRAME_BASED) && desc->bLength >= 21)) {
        format = static_cast<UvcFormat>(subtype);
        format_index = d[3];
        fourcc = subtype == UVC_VS_FORMAT_MJPEG ? 0 : rd32(d + 5);
        in_format = true;
        vs_intf = cur_intf;
        continue;
      }
      const bool frame_based = subtype == UVC_VS_FRAME_FRAME_BASED;
      if (!in_format ||
          !((subtype == UVC_VS_FRAME_MJPEG && format == UvcFormat::MJPEG) ||
            (subtype == UVC_VS_FRAME_UNCOMPRESSED && format == UvcFormat::UNCOMPRESSED) ||
            (frame_based && format == UvcFormat::FRAME_BASED)))
        continue;
      // MJPEG / uncompressed frame: ... dwMaxVideoFrameBufferSize (17), dwDefaultFrameInterval (21),
      // bFrameIntervalType (25), intervals (26); frame-based: dwDefaultFrameInterval (17), bFrameIntervalType (21),
      // dwBytesPerLine (22), intervals (26)
      const size_t type_at = frame_based ? 21 : 25;
      if (desc->bLength < type_at + 1)
        continue;
      UvcFrameInfo f{};
      f.format = format;
      f.format_index = format_index;
      f.frame_index = d[3];
      f.fourcc = fourcc;
      f.width = rd16(d + 5);
      f.height = rd16(d + 7);
      f.max_frame_size = frame_based ? 0 : rd32(d + 17);
      f.default_interval = rd32(d + (frame_based ? 17 : 21));
      const uint8_t type = d[type_at];
      if (type == 0 && desc->bLength >= 38) {
        f.min_interval = rd32(d + 26);
        f.max_interval = rd32(d + 30);
        f.interval_step = rd32(d + 34);
      } else {
        for (uint8_t i = 0; i < type && f.interval_count < UvcFrameInfo::MAX_INTERVALS; i++) {
          if (desc->bLength < 26 + 4 * (i + 1))
            break;
          f.intervals[f.interval_count++] = rd32(d + 26 + 4 * i);
        }
      }
      frames.push_back(f);
      continue;
    }
    if (desc->bDescriptorType == USB_W_VALUE_DT_ENDPOINT && vs_intf >= 0 && cur_intf == vs_intf && cur_alt > 0) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_ISOC || !USB_EP_DESC_GET_EP_DIR(ep))
        continue;
      // MULT is high-speed only; at full speed an isochronous packet is at most 1023 bytes.
      const uint16_t mps = cam.high_speed
                               ? static_cast<uint16_t>(USB_EP_DESC_GET_MPS(ep) * (USB_EP_DESC_GET_MULT(ep) + 1))
                               : std::min<uint16_t>(USB_EP_DESC_GET_MPS(ep), 1023);
      const uint8_t b_interval = std::max<uint8_t>(1, std::min<uint8_t>(ep->bInterval, 16));
      alts.push_back({cur_alt, ep->bEndpointAddress, mps, static_cast<uint16_t>(1U << (b_interval - 1))});
    }
  }

  if (frames.empty() || alts.empty()) {
    ESP_LOGE(TAG, "No video stream on this camera");
    return false;
  }
  // The list lives in PSRAM while the camera is plugged
  RAMAllocator<UvcFrameInfo> allocator(RAMAllocator<UvcFrameInfo>::ALLOC_EXTERNAL);
  cam.frames = allocator.allocate(frames.size());
  if (cam.frames == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for the camera's %u frame sizes", static_cast<unsigned>(frames.size()));
    return false;
  }
  memcpy(cam.frames, frames.data(), frames.size() * sizeof(UvcFrameInfo));
  cam.frame_count = static_cast<uint16_t>(frames.size());
  cam.vs_intf = static_cast<uint8_t>(vs_intf);
  cam.alts = std::move(alts);
  // Probe/commit structure: 26 bytes in UVC 1.0, 34 in 1.1, 48 from 1.5.
  cam.probe_len = bcd_uvc >= 0x0150 ? 48 : bcd_uvc >= 0x0110 ? 34 : 26;
  ESP_LOGD(TAG, "UVC %X.%02X, streaming interface %u, %u frame sizes, %u alt-settings", bcd_uvc >> 8, bcd_uvc & 0xFF,
           cam.vs_intf, cam.frame_count, static_cast<unsigned>(cam.alts.size()));
  char name[5];
  for (uint16_t i = 0; i < cam.frame_count; i++) {
    const UvcFrameInfo &f = cam.frames[i];
    ESP_LOGD(TAG, "  %u: %s %ux%u, default %" PRIu32 " fps", i, format_name(f, name, sizeof(name)), f.width, f.height,
             f.default_interval != 0 ? 10000000u / f.default_interval : 0);
  }
  return true;
}

// The configured format / size / rate where the camera reports it, else its first frame size that the panel can show
// (MJPEG or uncompressed), else its first one.
void UVCClient::select_default_(Camera &cam) {
  int pick = -1;
  for (uint16_t i = 0; i < cam.frame_count && pick < 0; i++) {
    const UvcFrameInfo &f = cam.frames[i];
    if (f.format == this->req_format_ && (this->req_width_ == 0 || f.width == this->req_width_) &&
        (this->req_height_ == 0 || f.height == this->req_height_))
      pick = i;
  }
  for (uint16_t i = 0; i < cam.frame_count && pick < 0; i++) {
    if (cam.frames[i].format != UvcFormat::FRAME_BASED)
      pick = i;
  }
  if (pick < 0)
    pick = 0;
  cam.selected = static_cast<int16_t>(pick);
  cam.interval = this->req_fps_ > 0 ? 10000000u / this->req_fps_ : cam.frames[pick].default_interval;
}

// -- Probe / commit ------------------------------------------------------------------------

void UVCClient::probe_request_(Camera &cam, uint8_t request, uint8_t selector, std::function<void(bool)> &&next) {
  const bool in = request == UVC_GET_CUR;
  size_t buffer_len = cam.probe_len;
  if (in) {
    // An IN data stage needs whole endpoint 0 packets; wLength stays the class-defined size.
    const usb_device_desc_t *dev = cam.dev->desc;
    const size_t mps0 = dev != nullptr && dev->bMaxPacketSize0 != 0 ? dev->bMaxPacketSize0 : 64;
    buffer_len = ((buffer_len + mps0 - 1) / mps0) * mps0;
  }
  std::vector<uint8_t> data(buffer_len, 0);
  if (!in)
    memcpy(data.data(), &cam.probe, cam.probe_len);

  // The camera stays allocated while this transfer is out (its pool); the continuation runs only
  // while the camera is still connected.
  Camera *camp = &cam;
  const uint32_t id = cam.id;
  auto then = std::make_shared<std::function<void(bool)>>(std::move(next));
  const bool submitted = this->control_transfer(
      *cam.dev, in ? UVC_REQ_TYPE_GET : UVC_REQ_TYPE_SET, request, static_cast<uint16_t>(selector << 8), cam.vs_intf,
      [this, camp, id, in, then](const usb_host::TransferStatus &status) {
        // USB task.
        bool ok = status.success;
        if (ok && in) {
          ok = status.data_len >= 26;
          if (ok)
            memcpy(&camp->probe, status.data, std::min<size_t>(status.data_len, camp->probe_len));
        }
        this->defer([this, id, ok, then]() {
          if (this->find_camera_(id) != nullptr)
            (*then)(ok);
        });
      },
      data, cam.probe_len);
  if (!submitted)
    (*then)(false);
}

void UVCClient::on_device_connected(usb_host::UsbDevice &dev) {
  auto *cam = new Camera();  // NOLINT: deleted in free_device_context
  cam->dev = &dev;
  cam->id = ++this->next_id_;
  dev.context = cam;
  this->cameras_.push_back(cam);
  ESP_LOGI(TAG, "Camera %04X:%04X connected", dev.desc->idVendor, dev.desc->idProduct);
  if (!this->parse_descriptors_(*cam)) {
    this->close_device(dev);  // on_device_disconnected(), then free_device_context()
    return;
  }
  this->select_default_(*cam);
  for (auto &cb : this->camera_callbacks_)
    cb(static_cast<uint8_t>(dev.addr), true);
}

// Main loop: the selected frame size negotiated, then the stream opened
void UVCClient::open_(Camera &cam) {
  cam.stats_since = 0;
  for (auto *n : {&cam.n_ok, &cam.n_holes, &cam.n_bad, &cam.n_busy})
    n->store(0, std::memory_order_relaxed);
  if (this->alloc_ == nullptr || this->free_ == nullptr) {
    ESP_LOGE(TAG, "No buffer allocator set (set_buffer_allocator)");
    return;
  }
  if (cam.selected < 0)
    return;
  const UvcFrameInfo &f = cam.frames[cam.selected];
  cam.state = StreamState::NEGOTIATING;
  memset(&cam.probe, 0, sizeof(cam.probe));
  cam.probe.bm_hint = 0x0001;  // keep dwFrameInterval
  cam.probe.b_format_index = f.format_index;
  cam.probe.b_frame_index = f.frame_index;
  cam.probe.dw_frame_interval = cam.interval != 0 ? cam.interval : f.default_interval;

  Camera *camp = &cam;
  this->probe_request_(cam, UVC_SET_CUR, UVC_VS_PROBE_CONTROL, [this, camp](bool ok) {
    if (!ok) {
      ESP_LOGE(TAG, "PROBE SET_CUR failed");
      this->fail_(*camp);
      return;
    }
    this->probe_request_(*camp, UVC_GET_CUR, UVC_VS_PROBE_CONTROL, [this, camp](bool ok) {
      if (!ok) {
        ESP_LOGE(TAG, "PROBE GET_CUR failed");
        this->fail_(*camp);
        return;
      }
      this->commit_(*camp);
    });
  });
}

void UVCClient::commit_(Camera &cam) {
  ESP_LOGD(TAG, "Negotiated: interval %" PRIu32 ", frame %" PRIu32 " bytes, payload %" PRIu32 " bytes",
           cam.probe.dw_frame_interval, cam.probe.dw_max_video_frame_size, cam.probe.dw_max_payload_transfer_size);
  Camera *camp = &cam;
  this->probe_request_(cam, UVC_SET_CUR, UVC_VS_COMMIT_CONTROL, [this, camp](bool ok) {
    if (!ok) {
      ESP_LOGE(TAG, "COMMIT SET_CUR failed");
      this->fail_(*camp);
      return;
    }
    this->start_stream_(*camp);
  });
}

void UVCClient::start_stream_(Camera &cam) {
  // Stopped or switched while negotiating: not opened (a switch negotiates again in the new selection)
  if (!cam.wanted || cam.reopen) {
    cam.state = StreamState::IDLE;
    if (cam.reopen && cam.wanted) {
      cam.reopen = false;
      this->open_(cam);
    }
    return;
  }
  // The smallest alt-setting that carries the negotiated payload: the rest of the bus stays
  // free for the host's other devices.
  const uint32_t payload = cam.probe.dw_max_payload_transfer_size;
  const AltSetting *chosen = nullptr;
  for (const auto &alt : cam.alts) {
    if (payload != 0 && alt.mps >= payload) {
      if (chosen == nullptr || alt.mps < chosen->mps)
        chosen = &alt;
    }
  }
  if (chosen == nullptr) {
    for (const auto &alt : cam.alts) {
      if (chosen == nullptr || alt.mps > chosen->mps)
        chosen = &alt;
    }
  }

  const UvcFrameInfo &f = cam.frames[cam.selected];
  size_t frame_size = cam.probe.dw_max_video_frame_size;
  if (frame_size == 0)
    frame_size = f.max_frame_size;
  if (frame_size == 0 && f.format == UvcFormat::UNCOMPRESSED)
    frame_size = static_cast<size_t>(f.width) * f.height * 2;
  if (!this->alloc_buffers_(cam, frame_size)) {
    this->fail_(cam);
    return;
  }
  cam.fill = 0;
  cam.fill_len = 0;
  cam.frame_id = 2;
  cam.skip_frame = false;
  cam.ready.store(false, std::memory_order_release);

  cam.stream.ep_addr = chosen->ep;
  cam.stream.mps = chosen->mps;
  // The HCD's list takes packets x interval <= 61 (micro)frames
  cam.stream.packets_per_urb =
      static_cast<uint8_t>(std::max(1, std::min<int>(PACKETS_PER_URB, 61 / std::max<int>(1, chosen->interval))));
  cam.stream.num_urbs = this->num_urbs_;
  cam.stream.interface_num = cam.vs_intf;
  cam.stream.alt_setting = chosen->alt;
  ESP_LOGD(TAG, "Streaming on alt %u, endpoint 0x%02X, %u bytes per packet", chosen->alt, chosen->ep, chosen->mps);
  cam.state = StreamState::STREAMING;
  // An accepted open reports through on_stream_open(); a refused one only through the return value.
  if (!this->stream_open(*cam.dev, cam.stream, this))
    this->fail_(cam);
}

// From the allocator for this stream, given back once it is closed.
bool UVCClient::alloc_buffers_(Camera &cam, size_t size) {
  if (size == 0) {
    ESP_LOGE(TAG, "The camera reports no frame size");
    return false;
  }
  for (auto &buffer : cam.buffers)
    buffer = this->alloc_(size);
  if (cam.buffers[0] == nullptr || cam.buffers[1] == nullptr) {
    ESP_LOGE(TAG, "No room for two %u byte frame buffers", static_cast<unsigned>(size));
    this->free_buffers_(cam);
    return false;
  }
  cam.buffer_size = size;
  return true;
}

void UVCClient::free_buffers_(Camera &cam) {
  for (auto &buffer : cam.buffers) {
    if (buffer != nullptr && this->free_ != nullptr)
      this->free_(buffer);
    buffer = nullptr;
  }
  cam.buffer_size = 0;
}

void UVCClient::close_(Camera &cam) {
  cam.ready.store(false, std::memory_order_release);
  if (cam.state == StreamState::NEGOTIATING)
    return;  // start_stream_() sees it (wanted / reopen) when the negotiation ends
  cam.state = StreamState::CLOSING;
  this->stream_close(cam.stream);
  this->enable_loop();  // the buffers go once the stream is closed (loop())
}

void UVCClient::fail_(Camera &cam) {
  cam.wanted = false;
  cam.reopen = false;
  if (cam.state == StreamState::STREAMING) {
    this->close_(cam);
    return;
  }
  this->free_buffers_(cam);
  cam.state = StreamState::IDLE;
}

void UVCClient::on_stream_open(usb_host::IsocStream &stream, bool ok) {
  if (stream.device == nullptr || stream.device->context == nullptr)
    return;
  auto *cam = static_cast<Camera *>(stream.device->context);
  if (this->find_camera_(cam->id) == nullptr)
    return;  // gone meanwhile
  if (!ok) {
    ESP_LOGE(TAG, "Opening the isochronous stream failed");
    cam->wanted = false;
    cam->reopen = false;
    cam->state = StreamState::CLOSING;  // the buffers go in loop() once it is closed
    this->enable_loop();
    return;
  }
  const UvcFrameInfo &f = cam->frames[cam->selected];
  char name[5];
  ESP_LOGI(TAG, "Streaming %s %ux%u", format_name(f, name, sizeof(name)), f.width, f.height);
}

void UVCClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *cam = static_cast<Camera *>(dev.context);
  if (cam == nullptr)
    return;
  this->stream_close(cam->stream);
  cam->ready.store(false, std::memory_order_release);
  this->cameras_.erase(std::remove(this->cameras_.begin(), this->cameras_.end(), cam), this->cameras_.end());
  if (cam->held)
    this->gone_.push_back(cam);  // its frame is still read: freed once released
  ESP_LOGI(TAG, "Camera disconnected");
  for (auto &cb : this->camera_callbacks_)
    cb(static_cast<uint8_t>(dev.addr), false);
}

bool UVCClient::device_busy_(const usb_host::UsbDevice &dev) const {
  const auto *cam = static_cast<const Camera *>(dev.context);
  return cam != nullptr && (!cam->stream.is_closed() || cam->held);
}

void UVCClient::free_device_context(usb_host::UsbDevice &dev) {
  auto *cam = static_cast<Camera *>(dev.context);
  this->gone_.erase(std::remove(this->gone_.begin(), this->gone_.end(), cam), this->gone_.end());
  this->destroy_(cam);
  dev.context = nullptr;
}

void UVCClient::destroy_(Camera *cam) {
  if (cam == nullptr)
    return;
  this->free_buffers_(*cam);
  if (cam->frames != nullptr) {
    RAMAllocator<UvcFrameInfo> allocator(RAMAllocator<UvcFrameInfo>::ALLOC_EXTERNAL);
    allocator.deallocate(cam->frames, cam->frame_count);
  }
  delete cam;  // NOLINT
}

// -- Frame assembly (USB task) -------------------------------------------------------------

void UVCClient::on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error,
                                 bool skipped) {
  if (stream.device == nullptr)
    return;
  auto &cam = *static_cast<Camera *>(stream.device->context);
  // A lost or skipped packet leaves a hole in the frame: that frame is not delivered (a decoder would choke on it)
  if (error || skipped) {
    cam.skip_frame = true;
    cam.skip_why |= 1;
    return;
  }
  if (len < 2)
    return;
  const auto *hdr = reinterpret_cast<const UvcPayloadHeader *>(data);
  if (hdr->b_header_length < 2 || hdr->b_header_length > len)
    return;

  if (hdr->bm_header_info.frame_id != cam.frame_id) {
    cam.frame_id = hdr->bm_header_info.frame_id;
    cam.fill_len = 0;
    cam.skip_frame = false;
    cam.skip_why = 0;
  }
  if (hdr->bm_header_info.error) {
    cam.skip_frame = true;
    cam.skip_why |= 2;
  }

  const size_t payload_len = len - hdr->b_header_length;
  if (!cam.skip_frame && payload_len > 0) {
    if (cam.fill_len + payload_len > cam.buffer_size) {
      cam.skip_frame = true;
      cam.skip_why |= 2;
    } else {
      memcpy(cam.buffers[cam.fill] + cam.fill_len, data + hdr->b_header_length, payload_len);
      cam.fill_len += payload_len;
    }
  }

  if (hdr->bm_header_info.end_of_frame) {
    // An MJPEG frame must be whole: SOI first, EOI last (padding after it allowed). Anything else never reaches a
    // decoder (a broken stream can leave the P4's JPEG engine and its 2D-DMA stuck)
    if (!cam.skip_frame && cam.fill_len > 0 && cam.selected >= 0 &&
        cam.frames[cam.selected].format == UvcFormat::MJPEG) {
      const uint8_t *f = cam.buffers[cam.fill];
      bool eoi = false;
      for (size_t i = cam.fill_len; i >= 2 && i + 32 > cam.fill_len; i--) {
        if (f[i - 2] == 0xFF && f[i - 1] == 0xD9) {
          eoi = true;
          break;
        }
      }
      if (cam.fill_len < 4 || f[0] != 0xFF || f[1] != 0xD8 || !eoi) {
        cam.skip_frame = true;
        cam.skip_why |= 2;
      }
    }
    if (cam.skip_frame) {
      ((cam.skip_why & 1) != 0 ? cam.n_holes : cam.n_bad).fetch_add(1, std::memory_order_relaxed);
    } else if (cam.fill_len > 0 && cam.ready.load(std::memory_order_acquire)) {
      cam.n_busy.fetch_add(1, std::memory_order_relaxed);
    }
    // Handed over only while the main loop is done with the previous frame; otherwise this
    // one is not delivered and its buffer takes the next frame.
    if (!cam.skip_frame && cam.fill_len > 0 && !cam.ready.load(std::memory_order_acquire)) {
      cam.ready_index = cam.fill;
      cam.ready_len = cam.fill_len;
      cam.fill ^= 1;
      cam.ready.store(true, std::memory_order_release);
      cam.n_ok.fetch_add(1, std::memory_order_relaxed);
      this->enable_loop_soon_any_context();
    }
    cam.fill_len = 0;
    cam.frame_id = 2;
    cam.skip_why = 0;
  }
}

#ifdef USE_CAMERA
// -- Home Assistant camera entity ----------------------------------------------------------

void UvcCamera::setup() {
  this->client_->add_frame_callback([this](uint8_t camera, const uint8_t *data, size_t length,
                                           const UvcFrameInfo &info) { this->on_frame_(camera, data, length, info); });
  // An unplugged camera ends what this entity started on it
  this->client_->add_camera_callback([this](uint8_t camera, bool connected) {
    if (!connected && camera == this->camera_)
      this->camera_ = 0;
    if (connected && this->camera_ == 0)
      this->update_stream_();
  });
}

void UvcCamera::request_image(camera::CameraRequester requester) {
  this->single_ |= static_cast<uint8_t>(1U << requester);
  this->update_stream_();
}

void UvcCamera::start_stream(camera::CameraRequester requester) {
  this->streaming_ |= static_cast<uint8_t>(1U << requester);
  this->update_stream_();
}

void UvcCamera::start_stream_as(camera::CameraRequester requester, camera::ImageFormat format) {
  if (format == camera::IMAGE_FORMAT_JPEG) {
    this->start_stream(requester);
    return;
  }
  this->h264_ |= static_cast<uint8_t>(1U << requester);
  this->request_key_frame();
  this->update_stream_();
}

void UvcCamera::stop_stream_as(camera::CameraRequester requester, camera::ImageFormat format) {
  if (format == camera::IMAGE_FORMAT_JPEG) {
    this->stop_stream(requester);
    return;
  }
  this->h264_ &= static_cast<uint8_t>(~(1U << requester));
  this->update_stream_();
}

void UvcCamera::request_key_frame() {
  if (this->h264_encoder_ != nullptr)
    this->h264_encoder_->request_key_frame();
}

void UvcCamera::on_encoded_released() {
  this->encoded_held_.store(false);
  this->defer([this]() {
    if (this->h264_ == 0)
      this->stop_encoder_();
  });
}

void UvcCamera::stop_encoder_() {
  if (this->encoder_started_ && !this->encoded_held_.load()) {
    this->h264_encoder_->stop();
    this->encoder_started_ = false;
  }
}

UvcEncodedImage::~UvcEncodedImage() { this->camera_->on_encoded_released(); }

static constexpr uint32_t FOURCC_H264 = 0x34363248;
static constexpr uint32_t FOURCC_YUY2 = 0x32595559;

void UvcCamera::pick_format_() {
  if (this->camera_ == 0)
    return;
  const bool h264 = this->h264_ != 0 && this->single_ == 0 && this->streaming_ == 0;
  if (h264 && !this->h264_picked_) {
    int native = -1;
    int raw = -1;
    for (size_t i = 0; i < this->client_->frame_count(this->camera_); i++) {
      const UvcFrameInfo *f = this->client_->frame_info(this->camera_, i);
      const uint32_t area = static_cast<uint32_t>(f->width) * f->height;
      if (f->format == UvcFormat::FRAME_BASED && f->fourcc == FOURCC_H264 && f->width <= 1920 &&
          (native < 0 || area > static_cast<uint32_t>(this->client_->frame_info(this->camera_, native)->width) *
                                    this->client_->frame_info(this->camera_, native)->height))
        native = static_cast<int>(i);
      if (this->h264_encoder_ != nullptr && f->format == UvcFormat::UNCOMPRESSED && f->fourcc == FOURCC_YUY2 &&
          f->width % 16 == 0 && f->height % 16 == 0 && area <= 1280u * 720u &&
          (raw < 0 || area > static_cast<uint32_t>(this->client_->frame_info(this->camera_, raw)->width) *
                                 this->client_->frame_info(this->camera_, raw)->height))
        raw = static_cast<int>(i);
    }
    const int pick = native >= 0 ? native : raw;
    if (pick < 0) {
      if (!this->h264_warned_)
        ESP_LOGW(TAG, "The camera offers neither H.264 nor YUY2 (with an h264_encoder): no H.264 stream");
      this->h264_warned_ = true;
      return;
    }
    this->jpeg_pick_ = static_cast<int16_t>(this->client_->selected(this->camera_));
    this->h264_picked_ = true;
    if (pick != this->jpeg_pick_)
      this->client_->select(this->camera_, static_cast<size_t>(pick));
  } else if (!h264 && this->h264_picked_) {
    this->h264_picked_ = false;
    if (this->jpeg_pick_ >= 0 && this->jpeg_pick_ != this->client_->selected(this->camera_))
      this->client_->select(this->camera_, static_cast<size_t>(this->jpeg_pick_));
    this->jpeg_pick_ = -1;
  }
  if (this->h264_ == 0)
    this->stop_encoder_();
}

static bool access_unit_has_key(const uint8_t *data, size_t size) {
  for (size_t i = 0; i + 3 < size; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      const uint8_t type = data[i + 3] & 0x1F;
      if (type == 5 || type == 7)
        return true;
      i += 2;
    }
  }
  return false;
}

void UvcCamera::on_h264_frame_(uint8_t camera, const uint8_t *data, size_t length, const UvcFrameInfo &info) {
  if (info.format == UvcFormat::FRAME_BASED && info.fourcc == FOURCC_H264) {
    const uint32_t token = this->client_->hold_frame(camera);
    if (token == 0)
      return;
    auto image = std::make_shared<UvcImage>(this->client_, token, const_cast<uint8_t *>(data), length, this->h264_,
                                            camera::IMAGE_FORMAT_H264, access_unit_has_key(data, length));
    for (auto *listener : this->listeners_)
      listener->on_camera_image(image);
    return;
  }
  if (this->h264_encoder_ == nullptr || info.format != UvcFormat::UNCOMPRESSED || info.fourcc != FOURCC_YUY2 ||
      this->encoded_held_.load())
    return;
  camera::CameraImageSpec spec{info.width, info.height, camera::PIXEL_FORMAT_YUYV};
  if (!this->encoder_started_) {
    if (!this->h264_encoder_->start(spec)) {
      this->h264_ = 0;
      this->pick_format_();
      this->update_stream_();
      return;
    }
    this->encoder_started_ = true;
  }
  class Frame : public camera::Buffer {
   public:
    Frame(const uint8_t *data, size_t length) : data_(const_cast<uint8_t *>(data)), length_(length) {}
    uint8_t *get_data_buffer() override { return this->data_; }
    size_t get_data_length() override { return this->length_; }

   protected:
    uint8_t *data_;
    size_t length_;
  };
  Frame frame(data, length);
  if (this->h264_encoder_->encode_pixels(&spec, &frame) != camera::ENCODER_ERROR_SUCCESS)
    return;
  camera::EncoderBuffer *out = this->h264_encoder_->get_output_buffer();
  this->encoded_held_.store(true);
  auto image = std::make_shared<UvcEncodedImage>(this, out->get_data(), out->get_size(), this->h264_,
                                                 this->h264_encoder_->is_key_frame());
  for (auto *listener : this->listeners_)
    listener->on_camera_image(image);
}

void UvcCamera::stop_stream(camera::CameraRequester requester) {
  this->streaming_ &= static_cast<uint8_t>(~(1U << requester));
  this->update_stream_();
}

// Started on the first plugged camera while anything is asked for, stopped (its start counted off) after
void UvcCamera::update_stream_() {
  const bool wanted = this->single_ != 0 || this->streaming_ != 0 || this->h264_ != 0;
  if (wanted && this->camera_ == 0) {
    const uint8_t camera = this->client_->camera_at(0);
    if (camera != 0) {
      this->camera_ = camera;
      this->pick_format_();
      this->client_->start(camera);
    }
  } else if (!wanted && this->camera_ != 0) {
    this->pick_format_();
    this->client_->stop(this->camera_);
    this->camera_ = 0;
  } else {
    this->pick_format_();
  }
}

void UvcCamera::on_frame_(uint8_t camera, const uint8_t *data, size_t length, const UvcFrameInfo &info) {
  if (camera == this->camera_ && this->h264_ != 0 && info.format != UvcFormat::MJPEG) {
    this->on_h264_frame_(camera, data, length, info);
    return;
  }
  const uint8_t requesters = this->single_ | this->streaming_;
  if (camera != this->camera_ || requesters == 0)
    return;
  if (info.format != UvcFormat::MJPEG) {
    if (!this->warned_)
      ESP_LOGW(TAG, "Home Assistant gets MJPEG frames only; the camera streams another format");
    this->warned_ = true;
    return;
  }
  const uint32_t token = this->client_->hold_frame(camera);
  if (token == 0)
    return;
  auto image = std::make_shared<UvcImage>(this->client_, token, const_cast<uint8_t *>(data), length, requesters);
  this->single_ = 0;
  for (auto *listener : this->listeners_)
    listener->on_camera_image(image);
  this->update_stream_();
}
#endif

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
