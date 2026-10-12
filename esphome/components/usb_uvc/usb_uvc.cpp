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

void UVCClient::loop() {
  if (this->ready_.load(std::memory_order_acquire)) {
    const uint8_t *data = this->buffers_[this->ready_index_];
    for (auto &cb : this->frame_callbacks_)
      cb(data, this->ready_len_, this->selected_frame_.format);
    this->ready_.store(false, std::memory_order_release);
  }
  usb_host::USBClient::loop();
}

// -- Descriptors ---------------------------------------------------------------------------

bool UVCClient::parse_descriptors_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;

  usb_device_info_t info;
  this->high_speed_ = usb_host_device_info(this->device_handle_, &info) == ESP_OK && info.speed == USB_SPEED_HIGH;

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
          this->vs_intf_ = cur_intf;
        }
      }
      continue;
    }
    if (desc->bDescriptorType == USB_W_VALUE_DT_ENDPOINT && found && cur_intf == this->vs_intf_ && cur_alt > 0) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_ISOC || !USB_EP_DESC_GET_EP_DIR(ep))
        continue;
      // MULT is high-speed only; at full speed an isochronous packet is at most 1023 bytes.
      const uint16_t mps = this->high_speed_
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
  this->selected_frame_ = best;
  this->alts_ = std::move(alts);
  // Probe/commit structure: 26 bytes in UVC 1.0, 34 in 1.1, 48 from 1.5.
  this->probe_len_ = bcd_uvc >= 0x0150 ? 48 : bcd_uvc >= 0x0110 ? 34 : 26;
  ESP_LOGD(TAG, "UVC %X.%02X, streaming interface %u, %ux%u, %u alt-settings", bcd_uvc >> 8, bcd_uvc & 0xFF,
           this->vs_intf_, best.width, best.height, static_cast<unsigned>(this->alts_.size()));
  return true;
}

// -- Probe / commit ------------------------------------------------------------------------

void UVCClient::probe_request_(uint8_t request, uint8_t selector, std::function<void(bool)> &&next) {
  const bool in = request == UVC_GET_CUR;
  size_t buffer_len = this->probe_len_;
  if (in) {
    // An IN data stage needs whole endpoint 0 packets; wLength stays the class-defined size.
    const usb_device_desc_t *dev = this->get_device_desc_();
    const size_t mps0 = dev != nullptr && dev->bMaxPacketSize0 != 0 ? dev->bMaxPacketSize0 : 64;
    buffer_len = ((buffer_len + mps0 - 1) / mps0) * mps0;
  }
  std::vector<uint8_t> data(buffer_len, 0);
  if (!in)
    memcpy(data.data(), &this->probe_, this->probe_len_);

  const uint32_t generation = this->generation_.load(std::memory_order_acquire);
  auto then = std::make_shared<std::function<void(bool)>>(std::move(next));
  const bool submitted = this->control_transfer(
      in ? UVC_REQ_TYPE_GET : UVC_REQ_TYPE_SET, request, static_cast<uint16_t>(selector << 8), this->vs_intf_,
      [this, generation, in, then](const usb_host::TransferStatus &status) {
        // USB task.
        if (generation != this->generation_.load(std::memory_order_acquire))
          return;
        bool ok = status.success;
        if (ok && in) {
          ok = status.data_len >= 26;
          if (ok)
            memcpy(&this->probe_, status.data, std::min<size_t>(status.data_len, this->probe_len_));
        }
        this->defer([this, generation, ok, then]() {
          if (generation == this->generation_.load(std::memory_order_acquire))
            (*then)(ok);
        });
      },
      data, this->probe_len_);
  if (!submitted)
    (*then)(false);
}

void UVCClient::on_connected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (dev != nullptr)
    ESP_LOGI(TAG, "Camera %04X:%04X connected", dev->idVendor, dev->idProduct);
  if (!this->parse_descriptors_()) {
    this->finish_setup_(false);
    return;
  }

  memset(&this->probe_, 0, sizeof(this->probe_));
  this->probe_.bm_hint = 0x0001;  // keep dwFrameInterval
  this->probe_.b_format_index = this->selected_frame_.format_index;
  this->probe_.b_frame_index = this->selected_frame_.frame_index;
  this->probe_.dw_frame_interval =
      this->req_fps_ > 0 ? 10000000u / this->req_fps_ : this->selected_frame_.default_interval;

  this->probe_request_(UVC_SET_CUR, UVC_VS_PROBE_CONTROL, [this](bool ok) {
    if (!ok) {
      ESP_LOGE(TAG, "PROBE SET_CUR failed");
      this->finish_setup_(false);
      return;
    }
    this->probe_request_(UVC_GET_CUR, UVC_VS_PROBE_CONTROL, [this](bool ok) {
      if (!ok) {
        ESP_LOGE(TAG, "PROBE GET_CUR failed");
        this->finish_setup_(false);
        return;
      }
      this->commit_();
    });
  });
}

void UVCClient::commit_() {
  ESP_LOGD(TAG, "Negotiated: interval %" PRIu32 ", frame %" PRIu32 " bytes, payload %" PRIu32 " bytes",
           this->probe_.dw_frame_interval, this->probe_.dw_max_video_frame_size,
           this->probe_.dw_max_payload_transfer_size);
  this->probe_request_(UVC_SET_CUR, UVC_VS_COMMIT_CONTROL, [this](bool ok) {
    if (!ok) {
      ESP_LOGE(TAG, "COMMIT SET_CUR failed");
      this->finish_setup_(false);
      return;
    }
    this->start_stream_();
  });
}

void UVCClient::start_stream_() {
  // The smallest alt-setting that carries the negotiated payload: the rest of the bus stays
  // free for the host's other devices.
  const uint32_t payload = this->probe_.dw_max_payload_transfer_size;
  const AltSetting *chosen = nullptr;
  for (const auto &alt : this->alts_) {
    if (payload != 0 && alt.mps >= payload) {
      if (chosen == nullptr || alt.mps < chosen->mps)
        chosen = &alt;
    }
  }
  if (chosen == nullptr) {
    for (const auto &alt : this->alts_) {
      if (chosen == nullptr || alt.mps > chosen->mps)
        chosen = &alt;
    }
  }

  size_t frame_size = this->probe_.dw_max_video_frame_size;
  if (frame_size == 0)
    frame_size = this->selected_frame_.max_frame_size;
  if (!this->ensure_buffers_(frame_size)) {
    this->finish_setup_(false);
    return;
  }
  this->fill_ = 0;
  this->fill_len_ = 0;
  this->frame_id_ = 2;
  this->skip_frame_ = false;
  this->ready_.store(false, std::memory_order_release);

  this->stream_.ep_addr = chosen->ep;
  this->stream_.mps = chosen->mps;
  this->stream_.packets_per_urb = PACKETS_PER_URB;
  this->stream_.num_urbs = this->num_urbs_;
  this->stream_.interface_num = this->vs_intf_;
  this->stream_.alt_setting = chosen->alt;
  ESP_LOGD(TAG, "Streaming on alt %u, endpoint 0x%02X, %u bytes per packet", chosen->alt, chosen->ep, chosen->mps);
  // An accepted open reports through on_stream_open(); a refused one only through the return
  // value. Reporting the connection is idempotent, so a failure reaching both is harmless.
  if (!this->stream_open(this->stream_, this))
    this->finish_setup_(false);
}

bool UVCClient::ensure_buffers_(size_t size) {
  if (size == 0) {
    ESP_LOGE(TAG, "The camera reports no frame size");
    return false;
  }
  if (size <= this->buffer_size_)
    return true;
  if (!this->stream_.is_closed()) {
    ESP_LOGE(TAG, "Previous stream still draining");
    return false;
  }
  for (auto &buffer : this->buffers_) {
    heap_caps_free(buffer);
    buffer = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (this->buffers_[0] == nullptr || this->buffers_[1] == nullptr) {
    ESP_LOGE(TAG, "No PSRAM for two %u byte frame buffers", static_cast<unsigned>(size));
    for (auto &buffer : this->buffers_) {
      heap_caps_free(buffer);
      buffer = nullptr;
    }
    this->buffer_size_ = 0;
    return false;
  }
  this->buffer_size_ = size;
  return true;
}

void UVCClient::on_stream_open(usb_host::IsocStream &stream, bool ok) {
  if (&stream != &this->stream_)
    return;
  if (!ok)
    ESP_LOGE(TAG, "Opening the isochronous stream failed");
  this->finish_setup_(ok);
}

void UVCClient::finish_setup_(bool ok) {
  if (ok)
    ESP_LOGI(TAG, "Streaming %ux%u", this->selected_frame_.width, this->selected_frame_.height);
  this->report_connected_();
}

void UVCClient::on_disconnected() {
  this->generation_.fetch_add(1, std::memory_order_acq_rel);
  this->stream_close(this->stream_);
  this->ready_.store(false, std::memory_order_release);
  ESP_LOGI(TAG, "Camera disconnected");
  usb_host::USBClient::on_disconnected();
}

// -- Frame assembly (USB task) -------------------------------------------------------------

void UVCClient::on_isoc_packet(uint8_t /*ep_addr*/, const uint8_t *data, size_t len, bool error) {
  if (error || len < 2)
    return;
  const auto *hdr = reinterpret_cast<const UvcPayloadHeader *>(data);
  if (hdr->b_header_length < 2 || hdr->b_header_length > len)
    return;

  if (hdr->bm_header_info.frame_id != this->frame_id_) {
    this->frame_id_ = hdr->bm_header_info.frame_id;
    this->fill_len_ = 0;
    this->skip_frame_ = false;
  }
  if (hdr->bm_header_info.error)
    this->skip_frame_ = true;

  const size_t payload_len = len - hdr->b_header_length;
  if (!this->skip_frame_ && payload_len > 0) {
    if (this->fill_len_ + payload_len > this->buffer_size_) {
      this->skip_frame_ = true;
    } else {
      memcpy(this->buffers_[this->fill_] + this->fill_len_, data + hdr->b_header_length, payload_len);
      this->fill_len_ += payload_len;
    }
  }

  if (hdr->bm_header_info.end_of_frame) {
    // Handed over only while the main loop is done with the previous frame; otherwise this
    // one is not delivered and its buffer takes the next frame.
    if (!this->skip_frame_ && this->fill_len_ > 0 && !this->ready_.load(std::memory_order_acquire)) {
      this->ready_index_ = this->fill_;
      this->ready_len_ = this->fill_len_;
      this->fill_ ^= 1;
      this->ready_.store(true, std::memory_order_release);
      this->enable_loop_soon_any_context();
    }
    this->fill_len_ = 0;
    this->frame_id_ = 2;
  }
}

}  // namespace esphome::usb_uvc

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
