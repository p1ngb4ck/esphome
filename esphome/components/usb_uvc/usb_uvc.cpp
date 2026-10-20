#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_uvc.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::usb_uvc {

static const char *const TAG = "usb_uvc";

// Packets per isochronous URB.
static constexpr uint8_t PACKETS_PER_URB = 4;

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
      for (auto &cb : this->frame_callbacks_)
        cb(static_cast<uint8_t>(cam->dev->addr), data, cam->ready_len, info);
      cam->ready.store(false, std::memory_order_release);
    }
    // A stream that stopped by itself (its URBs refused): closed like a stopped one
    if (cam->state == StreamState::STREAMING && cam->stream.died.exchange(false, std::memory_order_acq_rel)) {
      ESP_LOGW(TAG, "The stream stopped");
      cam->wanted = false;
      this->close_(*cam);
    }
    // A closed stream: its buffers back; a switch opens it again in the new selection
    if (cam->state == StreamState::CLOSING && cam->stream.is_closed()) {
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
  cam->wanted = true;
  if (cam->state == StreamState::IDLE) {
    this->open_(*cam);
  } else if (cam->state == StreamState::CLOSING) {
    cam->reopen = true;
  }
}

void UVCClient::stop(uint8_t camera) {
  Camera *cam = this->find_(camera);
  if (cam == nullptr)
    return;
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
      alts.push_back({cur_alt, ep->bEndpointAddress, mps});
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
  cam.stream.packets_per_urb = PACKETS_PER_URB;
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
  ESP_LOGI(TAG, "Camera disconnected");
  for (auto &cb : this->camera_callbacks_)
    cb(static_cast<uint8_t>(dev.addr), false);
}

bool UVCClient::device_busy_(const usb_host::UsbDevice &dev) const {
  const auto *cam = static_cast<const Camera *>(dev.context);
  return cam != nullptr && !cam->stream.is_closed();
}

void UVCClient::free_device_context(usb_host::UsbDevice &dev) {
  this->destroy_(static_cast<Camera *>(dev.context));
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

void UVCClient::on_stream_packet(usb_host::IsocStream &stream, const uint8_t *data, size_t len, bool error) {
  if (error || len < 2 || stream.device == nullptr)
    return;
  auto &cam = *static_cast<Camera *>(stream.device->context);
  const auto *hdr = reinterpret_cast<const UvcPayloadHeader *>(data);
  if (hdr->b_header_length < 2 || hdr->b_header_length > len)
    return;

  if (hdr->bm_header_info.frame_id != cam.frame_id) {
    cam.frame_id = hdr->bm_header_info.frame_id;
    cam.fill_len = 0;
    cam.skip_frame = false;
  }
  if (hdr->bm_header_info.error)
    cam.skip_frame = true;

  const size_t payload_len = len - hdr->b_header_length;
  if (!cam.skip_frame && payload_len > 0) {
    if (cam.fill_len + payload_len > cam.buffer_size) {
      cam.skip_frame = true;
    } else {
      memcpy(cam.buffers[cam.fill] + cam.fill_len, data + hdr->b_header_length, payload_len);
      cam.fill_len += payload_len;
    }
  }

  if (hdr->bm_header_info.end_of_frame) {
    // Handed over only while the main loop is done with the previous frame; otherwise this
    // one is not delivered and its buffer takes the next frame.
    if (!cam.skip_frame && cam.fill_len > 0 && !cam.ready.load(std::memory_order_acquire)) {
      cam.ready_index = cam.fill;
      cam.ready_len = cam.fill_len;
      cam.fill ^= 1;
      cam.ready.store(true, std::memory_order_release);
      this->enable_loop_soon_any_context();
    }
    cam.fill_len = 0;
    cam.frame_id = 2;
  }
}

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
