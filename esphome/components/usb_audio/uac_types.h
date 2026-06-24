#pragma once

// USB Audio Class 1.0 descriptor types, subtype codes, control request codes,
// Feature Unit control selectors, and Type I format descriptor structures.
// Sourced from: USB Device Class Definition for Audio Devices, revision 1.0.
// No dependency on any Espressif UAC host library.

#include <stdint.h>

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

namespace esphome {
namespace usb_audio {

// ── Interface subclass codes (bInterfaceSubClass) ────────────────────────────
static constexpr uint8_t UAC_SC_AUDIOCONTROL   = 0x01;
static constexpr uint8_t UAC_SC_AUDIOSTREAMING  = 0x02;
static constexpr uint8_t UAC_SC_MIDISTREAMING   = 0x03;

// ── Class-specific descriptor types (bDescriptorType) ────────────────────────
static constexpr uint8_t UAC_CS_INTERFACE = 0x24;
static constexpr uint8_t UAC_CS_ENDPOINT  = 0x25;

// ── AudioControl interface descriptor subtypes (bDescriptorSubType) ──────────
static constexpr uint8_t UAC_AC_HEADER          = 0x01;
static constexpr uint8_t UAC_AC_INPUT_TERMINAL  = 0x02;
static constexpr uint8_t UAC_AC_OUTPUT_TERMINAL = 0x03;
static constexpr uint8_t UAC_AC_MIXER_UNIT      = 0x04;
static constexpr uint8_t UAC_AC_SELECTOR_UNIT   = 0x05;
static constexpr uint8_t UAC_AC_FEATURE_UNIT    = 0x06;
static constexpr uint8_t UAC_AC_PROCESSING_UNIT = 0x07;
static constexpr uint8_t UAC_AC_EXTENSION_UNIT  = 0x08;

// ── AudioStreaming interface descriptor subtypes (bDescriptorSubType) ─────────
static constexpr uint8_t UAC_AS_GENERAL     = 0x01;
static constexpr uint8_t UAC_AS_FORMAT_TYPE = 0x02;

// ── Class-specific endpoint descriptor subtypes ───────────────────────────────
static constexpr uint8_t UAC_EP_GENERAL = 0x01;

// ── Audio class request codes (bRequest) ─────────────────────────────────────
static constexpr uint8_t UAC_SET_CUR = 0x01;
static constexpr uint8_t UAC_GET_CUR = 0x81;
static constexpr uint8_t UAC_SET_MIN = 0x02;
static constexpr uint8_t UAC_GET_MIN = 0x82;
static constexpr uint8_t UAC_SET_MAX = 0x03;
static constexpr uint8_t UAC_GET_MAX = 0x83;

// ── bmRequestType values ──────────────────────────────────────────────────────
// Class request to interface:  OUT=0x21  IN=0xA1
// Class request to endpoint:   OUT=0x22  IN=0xA2
static constexpr uint8_t UAC_REQ_TYPE_INTF_SET = 0x21;
static constexpr uint8_t UAC_REQ_TYPE_INTF_GET = 0xA1;
static constexpr uint8_t UAC_REQ_TYPE_EP_SET   = 0x22;
static constexpr uint8_t UAC_REQ_TYPE_EP_GET   = 0xA2;

// ── Feature Unit control selectors (wValue high byte) ────────────────────────
static constexpr uint8_t UAC_FU_MUTE_CONTROL      = 0x01;
static constexpr uint8_t UAC_FU_VOLUME_CONTROL     = 0x02;
static constexpr uint8_t UAC_FU_BASS_CONTROL       = 0x03;
static constexpr uint8_t UAC_FU_MID_CONTROL        = 0x04;
static constexpr uint8_t UAC_FU_TREBLE_CONTROL     = 0x05;
static constexpr uint8_t UAC_FU_EQ_CONTROL         = 0x06;
static constexpr uint8_t UAC_FU_AGC_CONTROL        = 0x07;
static constexpr uint8_t UAC_FU_DELAY_CONTROL      = 0x08;
static constexpr uint8_t UAC_FU_BASS_BOOST_CONTROL = 0x09;
static constexpr uint8_t UAC_FU_LOUDNESS_CONTROL   = 0x0A;

// ── Sampling Frequency endpoint control selector ──────────────────────────────
static constexpr uint8_t UAC_EP_SAMPLING_FREQ_CONTROL = 0x01;

// ── Feature Unit channel mask bits (bmaControls) ─────────────────────────────
static constexpr uint8_t UAC_FU_CTL_MUTE   = (1 << 0);
static constexpr uint8_t UAC_FU_CTL_VOLUME = (1 << 1);

// ── Maximum number of discrete sample frequencies per alt-setting ─────────────
static constexpr uint8_t UAC_MAX_SAMPLE_FREQS = 8;

// ── Described audio streaming alt-setting parameters ─────────────────────────
// Filled during descriptor parsing for each AS interface alternate setting > 0.
struct UacAltInfo {
  uint8_t  alt_setting{0};
  uint8_t  ep_addr{0};        // isochronous endpoint address (direction embedded)
  uint16_t mps{0};            // max packet size × mult
  uint8_t  channels{0};
  uint8_t  sub_frame_size{0}; // bytes per audio sample (1, 2, 3, or 4)
  uint8_t  bit_resolution{0}; // actual bits used (e.g. 16 or 24)
  uint8_t  sample_freq_type{0};  // 0 = continuous range; N = N discrete freqs
  uint32_t sample_freq_lower{0}; // used when sample_freq_type == 0
  uint32_t sample_freq_upper{0}; // used when sample_freq_type == 0
  uint32_t sample_freq[UAC_MAX_SAMPLE_FREQS]{};
  bool     valid{false};
};

// ── Feature Unit bookkeeping ─────────────────────────────────────────────────
struct UacFeatureUnit {
  uint8_t unit_id{0};
  uint8_t source_id{0};
  uint8_t channel_count{0};
  uint8_t control_size{0};  // bControlSize: bytes per bmaControls entry
  // bmaControls[0] = master, [1..n] = per-channel — only master stored here
  uint8_t master_controls{0};
  bool    has_mute{false};
  bool    has_volume{false};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
