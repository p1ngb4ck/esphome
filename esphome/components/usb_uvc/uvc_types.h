#pragma once

// UVC-specific descriptor types, subtype codes, control selectors and
// payload header — all sourced from:
//   USB Device Class Definition for Video Devices 1.5
// No dependency on any Espressif UVC library.

#include <stdint.h>
#include "usb/usb_types_ch9.h"  // USB_DESC_ATTR

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

namespace esphome {
namespace usb_uvc {

// ── Interface subclass codes (bInterfaceSubClass) ────────────────────────────
static constexpr uint8_t UVC_SC_VIDEOCONTROL  = 0x01;
static constexpr uint8_t UVC_SC_VIDEOSTREAMING = 0x02;

// ── Class-specific descriptor types (bDescriptorType) ────────────────────────
static constexpr uint8_t UVC_CS_INTERFACE = 0x24;
static constexpr uint8_t UVC_CS_ENDPOINT  = 0x25;

// ── VideoStreaming interface descriptor subtypes (bDescriptorSubType) ─────────
static constexpr uint8_t UVC_VS_INPUT_HEADER        = 0x01;
static constexpr uint8_t UVC_VS_FORMAT_UNCOMPRESSED = 0x04;
static constexpr uint8_t UVC_VS_FRAME_UNCOMPRESSED  = 0x05;
static constexpr uint8_t UVC_VS_FORMAT_MJPEG        = 0x06;
static constexpr uint8_t UVC_VS_FRAME_MJPEG         = 0x07;

// ── VideoStreaming control selectors (wValue high byte) ───────────────────────
static constexpr uint8_t UVC_VS_PROBE_CONTROL  = 0x01;
static constexpr uint8_t UVC_VS_COMMIT_CONTROL = 0x02;

// ── Class-specific request codes (bRequest) ───────────────────────────────────
static constexpr uint8_t UVC_SET_CUR = 0x01;
static constexpr uint8_t UVC_GET_CUR = 0x81;

// ── bmRequestType for class requests to a VideoStreaming interface ─────────────
//   OUT | CLASS | INTERFACE = 0x21
//   IN  | CLASS | INTERFACE = 0xA1
static constexpr uint8_t UVC_REQ_TYPE_SET = 0x21;
static constexpr uint8_t UVC_REQ_TYPE_GET = 0xA1;

// ── Probe/Commit control structure (UVC 1.5, table 4-75, 48 bytes) ────────────
struct __attribute__((packed)) UvcVsCtrl {
  uint16_t bm_hint;
  uint8_t  b_format_index;
  uint8_t  b_frame_index;
  uint32_t dw_frame_interval;     // 100-ns units
  uint16_t w_key_frame_rate;
  uint16_t w_p_frame_rate;
  uint16_t w_comp_quality;
  uint16_t w_comp_window_size;
  uint16_t w_delay;
  uint32_t dw_max_video_frame_size;
  uint32_t dw_max_payload_transfer_size;
  // UVC 1.1+
  uint32_t dw_clock_frequency;
  uint8_t  bm_framing_info;
  uint8_t  b_preferred_version;
  uint8_t  b_min_version;
  uint8_t  b_max_version;
  // UVC 1.5+
  uint8_t  b_usage;
  uint8_t  b_bit_depth_luma;
  uint8_t  bm_settings;
  uint8_t  b_max_number_of_ref_frames_plus1;
  uint16_t bm_rate_control_modes;
  uint8_t  bm_layout_per_stream[8];
};
static_assert(sizeof(UvcVsCtrl) == 48, "UvcVsCtrl must be 48 bytes");

// ── Isochronous payload header (UVC 1.5, table 2-5) ──────────────────────────
struct __attribute__((packed)) UvcPayloadHeader {
  uint8_t b_header_length;
  union {
    struct {
      uint8_t frame_id          : 1;
      uint8_t end_of_frame      : 1;
      uint8_t presentation_time : 1;
      uint8_t source_clock_ref  : 1;
      uint8_t payload_specific  : 1;
      uint8_t still_image       : 1;
      uint8_t error             : 1;
      uint8_t end_of_header     : 1;
    };
    uint8_t val;
  } bm_header_info;
};

// ── Video format enum (subset we handle) ─────────────────────────────────────
enum class UvcFormat : uint8_t {
  MJPEG        = UVC_VS_FORMAT_MJPEG,
  UNCOMPRESSED = UVC_VS_FORMAT_UNCOMPRESSED,
};

// ── Descriptor of a format+frame combo found during enumeration ───────────────
struct UvcFrameDesc {
  UvcFormat format;
  uint8_t   format_index;
  uint8_t   frame_index;
  uint16_t  width;
  uint16_t  height;
  uint32_t  default_interval;   // 100-ns units (= 10 000 000 / fps)
  uint32_t  max_frame_size;
};

}  // namespace usb_uvc
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
