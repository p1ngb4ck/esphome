#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_uvc.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "usb/usb_helpers.h"

#include <cinttypes>
#include <cstring>
#include <algorithm>

namespace esphome {
namespace usb_uvc {

static const char *const TAG = "usb_uvc";

// ── Frame buffer pre-allocation ───────────────────────────────────────────────
// Reserve enough for a full MJPEG frame at the requested resolution.
// Worst case ~1 byte/pixel for MJPEG; we cap at 1 MB for embedded sanity.
static constexpr size_t FRAME_BUF_RESERVE = 512 * 1024;  // 512 KB

void UVCClient::setup() {
  this->frame_buf_.reserve(FRAME_BUF_RESERVE);
  usb_host::USBClient::setup();
}

void UVCClient::loop() {
  this->process_usb_events_();

  if (this->frame_ready_) {
    this->frame_ready_ = false;
    for (auto &cb : this->frame_callbacks_) {
      cb(this->frame_buf_.data(), this->frame_buf_.size(), this->selected_frame_.format);
    }
  }
}

// ── Descriptor parsing ────────────────────────────────────────────────────────

bool UVCClient::find_streaming_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc();
  if (cfg == nullptr)
    return false;

  const uint8_t *base = reinterpret_cast<const uint8_t *>(cfg);
  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

  // State machine: track current interface and alternate setting while walking.
  uint8_t cur_intf = 0;
  uint8_t cur_alt  = 0;

  // Candidate: best matching VideoStreaming interface found so far.
  bool    found_vs         = false;
  uint8_t cand_intf        = 0;
  uint8_t cand_alt         = 0;
  uint8_t cand_ep          = 0;
  uint16_t cand_mps        = 0;
  UvcFrameDesc cand_frame  = {};

  // Per-format tracking within current VS interface alternate 0 descriptors.
  uint8_t  cur_format_type  = 0;
  uint8_t  cur_format_index = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    const uint8_t dtype = desc->bDescriptorType;
    const uint8_t *d    = reinterpret_cast<const uint8_t *>(desc);

    if (dtype == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      cur_intf = id->bInterfaceNumber;
      cur_alt  = id->bAlternateSetting;

      // Reset format tracking when entering a new interface.
      if (cur_alt == 0) {
        cur_format_type  = 0;
        cur_format_index = 0;
      }
      continue;
    }

    // Class-specific interface descriptor (0x24) — VS format/frame descriptors
    // appear under alt-setting 0 of a VideoStreaming interface.
    if (dtype == UVC_CS_INTERFACE && cur_alt == 0) {
      if (desc->bLength < 3)
        continue;
      uint8_t subtype = d[2];

      if (subtype == UVC_VS_FORMAT_MJPEG || subtype == UVC_VS_FORMAT_UNCOMPRESSED) {
        // bFormatIndex is at byte 3.
        cur_format_type  = subtype;
        cur_format_index = (desc->bLength > 3) ? d[3] : 0;
        continue;
      }

      if (subtype == UVC_VS_FRAME_MJPEG || subtype == UVC_VS_FRAME_UNCOMPRESSED) {
        // Common frame descriptor layout (MJPEG and Uncompressed share fields 0-8):
        //   [0] bLength  [1] bDescriptorType  [2] bDescriptorSubType
        //   [3] bFrameIndex  [4] bmCapabilities
        //   [5-6] wWidth  [7-8] wHeight
        //   ... dwMinBitRate dwMaxBitRate ...
        //   For MJPEG/Uncompressed: dwMaxVideoFrameBufferSize at [17-20],
        //   dwDefaultFrameInterval at [21-24].
        if (desc->bLength < 26)
          continue;

        UvcFrameDesc fd;
        fd.format       = (cur_format_type == UVC_VS_FORMAT_MJPEG) ? UvcFormat::MJPEG : UvcFormat::UNCOMPRESSED;
        fd.format_index = cur_format_index;
        fd.frame_index  = d[3];
        fd.width        = static_cast<uint16_t>(d[5] | (d[6] << 8));
        fd.height       = static_cast<uint16_t>(d[7] | (d[8] << 8));
        // dwMaxVideoFrameBufferSize: bytes 17-20
        fd.max_frame_size = static_cast<uint32_t>(d[17]) | (static_cast<uint32_t>(d[18]) << 8) |
                            (static_cast<uint32_t>(d[19]) << 16) | (static_cast<uint32_t>(d[20]) << 24);
        // dwDefaultFrameInterval: bytes 21-24
        fd.default_interval = static_cast<uint32_t>(d[21]) | (static_cast<uint32_t>(d[22]) << 8) |
                              (static_cast<uint32_t>(d[23]) << 16) | (static_cast<uint32_t>(d[24]) << 24);

        // Check if this matches the requested format/resolution.
        bool fmt_ok = (fd.format == this->req_format_);
        bool w_ok   = (this->req_width_  == 0 || fd.width  == this->req_width_);
        bool h_ok   = (this->req_height_ == 0 || fd.height == this->req_height_);
        if (fmt_ok && w_ok && h_ok) {
          // Good candidate — remember this interface/frame but keep scanning
          // for the endpoint (which appears in alt-setting > 0 below).
          cand_frame = fd;
          // Mark that we have a matching frame in this interface.
          // We'll confirm with an endpoint below.
          found_vs  = true;
          cand_intf = cur_intf;
        }
        continue;
      }
    }

    // Standard endpoint descriptor — look for ISOC IN endpoint on a VS
    // interface alternate setting > 0 that matches our candidate interface.
    if (dtype == USB_W_VALUE_DT_ENDPOINT && found_vs && cur_intf == cand_intf && cur_alt > 0) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      bool is_isoc = (USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_ISOC);
      bool is_in   = (USB_EP_DESC_GET_EP_DIR(ep) == 1);
      if (is_isoc && is_in) {
        uint16_t mps = USB_EP_DESC_GET_MPS(ep) * (USB_EP_DESC_GET_MULT(ep) + 1);
        // Prefer higher bandwidth alternate settings (larger MPS).
        if (mps > cand_mps) {
          cand_mps = mps;
          cand_alt = cur_alt;
          cand_ep  = ep->bEndpointAddress;
        }
      }
    }
  }

  if (!found_vs || cand_ep == 0) {
    ESP_LOGE(TAG, "No matching VideoStreaming interface found (fmt=%u %ux%u)",
             static_cast<uint8_t>(this->req_format_), this->req_width_, this->req_height_);
    return false;
  }

  this->vs_intf_       = cand_intf;
  this->vs_alt_        = cand_alt;
  this->vs_ep_         = cand_ep;
  this->vs_mps_        = cand_mps;
  this->selected_frame_ = cand_frame;

  ESP_LOGI(TAG, "VS intf=%u alt=%u ep=0x%02X mps=%u  %ux%u fmt=%u",
           this->vs_intf_, this->vs_alt_, this->vs_ep_, this->vs_mps_,
           cand_frame.width, cand_frame.height, static_cast<uint8_t>(cand_frame.format));
  return true;
}

// ── Probe / Commit ─────────────────────────────────────────────────────────────

bool UVCClient::probe_commit_() {
  UvcVsCtrl ctrl{};
  ctrl.bm_hint        = 0x0001;  // hint: dwFrameInterval
  ctrl.b_format_index = this->selected_frame_.format_index;
  ctrl.b_frame_index  = this->selected_frame_.frame_index;

  // Convert requested FPS to frame interval (100-ns units).
  if (this->req_fps_ > 0) {
    ctrl.dw_frame_interval = 10000000u / this->req_fps_;
  } else {
    ctrl.dw_frame_interval = this->selected_frame_.default_interval;
  }

  // ── PROBE SET_CUR ──
  {
    std::vector<uint8_t> data(reinterpret_cast<uint8_t *>(&ctrl),
                               reinterpret_cast<uint8_t *>(&ctrl) + sizeof(ctrl));
    bool ok = false;
    bool done = false;
    this->control_transfer(UVC_REQ_TYPE_SET, UVC_SET_CUR,
                           static_cast<uint16_t>(UVC_VS_PROBE_CONTROL << 8),
                           this->vs_intf_,
                           [&ok, &done](const usb_host::TransferStatus &s) {
                             ok = s.success;
                             done = true;
                           },
                           data);
    // Busy-wait — we're in on_connected() (main loop), USB task fires callback.
    uint32_t t = millis();
    while (!done && (millis() - t) < 1000)
      vTaskDelay(pdMS_TO_TICKS(5));
    if (!ok) {
      ESP_LOGE(TAG, "PROBE SET_CUR failed");
      return false;
    }
  }

  // ── PROBE GET_CUR — read negotiated parameters back ──
  {
    UvcVsCtrl result{};
    bool ok = false;
    bool done = false;
    this->control_transfer(UVC_REQ_TYPE_GET, UVC_GET_CUR,
                           static_cast<uint16_t>(UVC_VS_PROBE_CONTROL << 8),
                           this->vs_intf_,
                           [&ok, &done, &result](const usb_host::TransferStatus &s) {
                             ok = s.success;
                             done = true;
                             if (s.success && s.data_len >= sizeof(UvcVsCtrl))
                               memcpy(&result, s.data, sizeof(UvcVsCtrl));
                           });
    uint32_t t = millis();
    while (!done && (millis() - t) < 1000)
      vTaskDelay(pdMS_TO_TICKS(5));
    if (!ok) {
      ESP_LOGE(TAG, "PROBE GET_CUR failed");
      return false;
    }
    // Use negotiated frame size for buffer sizing.
    this->selected_frame_.max_frame_size = result.dw_max_video_frame_size;
    ctrl = result;
    ESP_LOGD(TAG, "Negotiated: interval=%" PRIu32 " max_frame=%" PRIu32,
             result.dw_frame_interval, result.dw_max_video_frame_size);
  }

  // ── COMMIT SET_CUR ──
  {
    std::vector<uint8_t> data(reinterpret_cast<uint8_t *>(&ctrl),
                               reinterpret_cast<uint8_t *>(&ctrl) + sizeof(ctrl));
    bool ok = false;
    bool done = false;
    this->control_transfer(UVC_REQ_TYPE_SET, UVC_SET_CUR,
                           static_cast<uint16_t>(UVC_VS_COMMIT_CONTROL << 8),
                           this->vs_intf_,
                           [&ok, &done](const usb_host::TransferStatus &s) {
                             ok = s.success;
                             done = true;
                           },
                           data);
    uint32_t t = millis();
    while (!done && (millis() - t) < 1000)
      vTaskDelay(pdMS_TO_TICKS(5));
    if (!ok) {
      ESP_LOGE(TAG, "COMMIT SET_CUR failed");
      return false;
    }
  }

  return true;
}

// ── Connection lifecycle ──────────────────────────────────────────────────────

void UVCClient::on_connected() {
  const usb_device_desc_t *dev = this->get_device_desc();
  if (dev != nullptr) {
    ESP_LOGI(TAG, "UVC device connected: VID=0x%04X PID=0x%04X", dev->idVendor, dev->idProduct);
  }

  if (!this->find_streaming_interface_()) {
    ESP_LOGE(TAG, "Failed to find streaming interface — aborting");
    return;
  }

  if (!this->probe_commit_()) {
    ESP_LOGE(TAG, "Probe/Commit negotiation failed — aborting");
    return;
  }

  // Reserve frame buffer for worst-case frame size.
  size_t reserve = std::max<size_t>(this->selected_frame_.max_frame_size, FRAME_BUF_RESERVE);
  this->frame_buf_.clear();
  this->frame_buf_.reserve(reserve);
  this->current_frame_id_ = 2;  // invalid — triggers start-of-frame detection
  this->skip_frame_  = false;
  this->frame_ready_ = false;

  // Small delay: some cameras need time between COMMIT and SET_INTERFACE.
  vTaskDelay(pdMS_TO_TICKS(10));

  this->stream_.ep_addr        = this->vs_ep_;
  this->stream_.mps            = this->vs_mps_;
  this->stream_.packets_per_urb = 4;  // 4 packets/URB — balances latency vs interrupt rate
  this->stream_.num_urbs       = this->num_urbs_;
  this->stream_.interface_num  = this->vs_intf_;
  this->stream_.alt_setting    = this->vs_alt_;

  if (!this->stream_open_(this->stream_, this)) {
    ESP_LOGE(TAG, "stream_open_ failed");
  }
}

void UVCClient::on_disconnected() {
  this->stream_close_(this->stream_);
  this->frame_ready_ = false;
  this->frame_buf_.clear();
  ESP_LOGI(TAG, "UVC device disconnected");
}

// ── Isochronous packet handler (USB-task context) ─────────────────────────────

void UVCClient::on_isoc_packet(uint8_t /*ep_addr*/, const uint8_t *data, size_t len, bool error) {
  if (error || len < 2)
    return;  // SKIPPED or corrupted — ignore silently

  const auto *hdr = reinterpret_cast<const UvcPayloadHeader *>(data);
  if (hdr->b_header_length < 2 || hdr->b_header_length > len)
    return;  // malformed header

  if (hdr->bm_header_info.error) {
    this->skip_frame_ = true;
  }

  const bool start_of_frame = (hdr->bm_header_info.frame_id != this->current_frame_id_);
  if (start_of_frame) {
    this->current_frame_id_ = hdr->bm_header_info.frame_id;
    this->frame_buf_.clear();
    this->skip_frame_ = hdr->bm_header_info.error;
  }

  if (!this->skip_frame_) {
    const uint8_t *payload = data + hdr->b_header_length;
    size_t payload_len     = len - hdr->b_header_length;

    // Guard against buffer overflow — drop oversized frames.
    if (this->frame_buf_.size() + payload_len > this->frame_buf_.capacity()) {
      this->skip_frame_ = true;
    } else if (payload_len > 0) {
      this->frame_buf_.insert(this->frame_buf_.end(), payload, payload + payload_len);
    }
  }

  if (hdr->bm_header_info.end_of_frame && !this->skip_frame_ && !this->frame_buf_.empty()) {
    // Signal main loop to fire callbacks. We do NOT call them here — USB-task context.
    this->frame_ready_ = true;
  }
}

}  // namespace usb_uvc
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
