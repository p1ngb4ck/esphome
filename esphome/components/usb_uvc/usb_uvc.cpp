#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_uvc.h"
#include "esphome/core/log.h"
#include "usb/usb_helpers.h"
#include "esp_heap_caps.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <memory>

namespace esphome::usb_uvc {

static const char *const TAG = "usb_uvc";

// Packets per isochronous URB.
static constexpr uint8_t PACKETS_PER_URB = 4;

UVCClient::Camera::~Camera() {
  for (auto &buffer : this->buffers)
    heap_caps_free(buffer);
}

void UVCClient::loop() {
  for (auto *cam : this->cameras_) {
    if (cam->ready.load(std::memory_order_acquire)) {
      const uint8_t *data = cam->buffers[cam->ready_index];
      for (auto &cb : this->frame_callbacks_)
        cb(static_cast<uint8_t>(cam->dev->addr), data, cam->ready_len, cam->selected_frame.format);
      cam->ready.store(false, std::memory_order_release);
    }
  }
  usb_host::USBClient::loop();
}

UVCClient::Camera *UVCClient::find_camera_(uint32_t id) const {
  for (auto *cam : this->cameras_) {
    if (cam->id == id)
      return cam;
  }
  return nullptr;
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
  uint8_t format_type = 0, format_index = 0;
  bool found = false;
  UvcFrameDesc best{};
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
      if (cur_alt == 0)
        format_type = format_index = 0;
      continue;
    }
    if (desc->bDescriptorType == UVC_CS_INTERFACE && cur_alt == 0 && desc->bLength >= 3) {
      const uint8_t subtype = d[2];
      if (cur_subclass == UVC_SC_VIDEOCONTROL && subtype == UVC_VC_HEADER && desc->bLength >= 5) {
        bcd_uvc = static_cast<uint16_t>(d[3] | (d[4] << 8));
        continue;
      }
      if (cur_subclass != UVC_SC_VIDEOSTREAMING || found)
        continue;
      if (subtype == UVC_VS_FORMAT_MJPEG || subtype == UVC_VS_FORMAT_UNCOMPRESSED) {
        format_type = subtype;
        format_index = desc->bLength > 3 ? d[3] : 0;
        continue;
      }
      if ((subtype == UVC_VS_FRAME_MJPEG || subtype == UVC_VS_FRAME_UNCOMPRESSED) && desc->bLength >= 26 &&
          format_type == static_cast<uint8_t>(this->req_format_)) {
        UvcFrameDesc fd;
        fd.format = this->req_format_;
        fd.format_index = format_index;
        fd.frame_index = d[3];
        fd.width = static_cast<uint16_t>(d[5] | (d[6] << 8));
        fd.height = static_cast<uint16_t>(d[7] | (d[8] << 8));
        fd.max_frame_size = static_cast<uint32_t>(d[17]) | (static_cast<uint32_t>(d[18]) << 8) |
                            (static_cast<uint32_t>(d[19]) << 16) | (static_cast<uint32_t>(d[20]) << 24);
        fd.default_interval = static_cast<uint32_t>(d[21]) | (static_cast<uint32_t>(d[22]) << 8) |
                              (static_cast<uint32_t>(d[23]) << 16) | (static_cast<uint32_t>(d[24]) << 24);
        if ((this->req_width_ == 0 || fd.width == this->req_width_) &&
            (this->req_height_ == 0 || fd.height == this->req_height_)) {
          best = fd;
          found = true;
          cam.vs_intf = cur_intf;
        }
      }
      continue;
    }
    if (desc->bDescriptorType == USB_W_VALUE_DT_ENDPOINT && found && cur_intf == cam.vs_intf && cur_alt > 0) {
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

  if (!found || alts.empty()) {
    ESP_LOGE(TAG, "No %s %ux%u stream on this camera", this->req_format_ == UvcFormat::MJPEG ? "MJPEG" : "uncompressed",
             this->req_width_, this->req_height_);
    return false;
  }
  cam.selected_frame = best;
  cam.alts = std::move(alts);
  // Probe/commit structure: 26 bytes in UVC 1.0, 34 in 1.1, 48 from 1.5.
  cam.probe_len = bcd_uvc >= 0x0150 ? 48 : bcd_uvc >= 0x0110 ? 34 : 26;
  ESP_LOGD(TAG, "UVC %X.%02X, streaming interface %u, %ux%u, %u alt-settings", bcd_uvc >> 8, bcd_uvc & 0xFF,
           cam.vs_intf, best.width, best.height, static_cast<unsigned>(cam.alts.size()));
  return true;
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
    this->finish_setup_(*cam, false);
    return;
  }

  memset(&cam->probe, 0, sizeof(cam->probe));
  cam->probe.bm_hint = 0x0001;  // keep dwFrameInterval
  cam->probe.b_format_index = cam->selected_frame.format_index;
  cam->probe.b_frame_index = cam->selected_frame.frame_index;
  cam->probe.dw_frame_interval =
      this->req_fps_ > 0 ? 10000000u / this->req_fps_ : cam->selected_frame.default_interval;

  this->probe_request_(*cam, UVC_SET_CUR, UVC_VS_PROBE_CONTROL, [this, cam](bool ok) {
    if (!ok) {
      ESP_LOGE(TAG, "PROBE SET_CUR failed");
      this->finish_setup_(*cam, false);
      return;
    }
    this->probe_request_(*cam, UVC_GET_CUR, UVC_VS_PROBE_CONTROL, [this, cam](bool ok) {
      if (!ok) {
        ESP_LOGE(TAG, "PROBE GET_CUR failed");
        this->finish_setup_(*cam, false);
        return;
      }
      this->commit_(*cam);
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
      this->finish_setup_(*camp, false);
      return;
    }
    this->start_stream_(*camp);
  });
}

void UVCClient::start_stream_(Camera &cam) {
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

  size_t frame_size = cam.probe.dw_max_video_frame_size;
  if (frame_size == 0)
    frame_size = cam.selected_frame.max_frame_size;
  if (!this->alloc_buffers_(cam, frame_size)) {
    this->finish_setup_(cam, false);
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
  // An accepted open reports through on_stream_open(); a refused one only through the return value.
  if (!this->stream_open(*cam.dev, cam.stream, this))
    this->finish_setup_(cam, false);
}

// Made with the camera's stream, freed with the camera.
bool UVCClient::alloc_buffers_(Camera &cam, size_t size) {
  if (size == 0) {
    ESP_LOGE(TAG, "The camera reports no frame size");
    return false;
  }
  for (auto &buffer : cam.buffers)
    buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (cam.buffers[0] == nullptr || cam.buffers[1] == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for two %u byte frame buffers", static_cast<unsigned>(size));
    for (auto &buffer : cam.buffers) {
      heap_caps_free(buffer);
      buffer = nullptr;
    }
    return false;
  }
  cam.buffer_size = size;
  return true;
}

void UVCClient::on_stream_open(usb_host::IsocStream &stream, bool ok) {
  if (stream.device == nullptr || stream.device->context == nullptr)
    return;
  auto *cam = static_cast<Camera *>(stream.device->context);
  if (this->find_camera_(cam->id) == nullptr)
    return;  // gone meanwhile: nothing to set up
  if (!ok)
    ESP_LOGE(TAG, "Opening the isochronous stream failed");
  this->finish_setup_(*cam, ok);
}

void UVCClient::finish_setup_(Camera &cam, bool ok) {
  if (ok) {
    ESP_LOGI(TAG, "Streaming %ux%u", cam.selected_frame.width, cam.selected_frame.height);
    return;
  }
  this->close_device(*cam.dev);  // on_device_disconnected(), then free_device_context() once drained
}

void UVCClient::on_device_disconnected(usb_host::UsbDevice &dev) {
  auto *cam = static_cast<Camera *>(dev.context);
  if (cam == nullptr)
    return;
  this->stream_close(cam->stream);
  cam->ready.store(false, std::memory_order_release);
  this->cameras_.erase(std::remove(this->cameras_.begin(), this->cameras_.end(), cam), this->cameras_.end());
  ESP_LOGI(TAG, "Camera disconnected");
}

bool UVCClient::device_busy_(const usb_host::UsbDevice &dev) const {
  const auto *cam = static_cast<const Camera *>(dev.context);
  return cam != nullptr && !cam->stream.is_closed();
}

void UVCClient::free_device_context(usb_host::UsbDevice &dev) {
  delete static_cast<Camera *>(dev.context);  // NOLINT
  dev.context = nullptr;
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
