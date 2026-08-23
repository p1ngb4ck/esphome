#pragma once

// USB Audio Class 1.0 descriptor types, subtype codes, control request codes,
// Feature Unit control selectors, and Type I format descriptor structures.
// Sourced from: USB Device Class Definition for Audio Devices, revision 1.0.
// No dependency on any Espressif UAC host library.

#include <stdint.h>

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

namespace esphome {
namespace usb_audio {

// -- Interface subclass codes (bInterfaceSubClass) ----------------------------
static constexpr uint8_t UAC_SC_AUDIOCONTROL   = 0x01;
static constexpr uint8_t UAC_SC_AUDIOSTREAMING  = 0x02;
static constexpr uint8_t UAC_SC_MIDISTREAMING   = 0x03;

// -- Class-specific descriptor types (bDescriptorType) ------------------------
static constexpr uint8_t UAC_CS_INTERFACE = 0x24;
static constexpr uint8_t UAC_CS_ENDPOINT  = 0x25;

// -- AudioControl interface descriptor subtypes (bDescriptorSubType) ----------
static constexpr uint8_t UAC_AC_HEADER          = 0x01;
static constexpr uint8_t UAC_AC_INPUT_TERMINAL  = 0x02;
static constexpr uint8_t UAC_AC_OUTPUT_TERMINAL = 0x03;
static constexpr uint8_t UAC_AC_MIXER_UNIT      = 0x04;
static constexpr uint8_t UAC_AC_SELECTOR_UNIT   = 0x05;
static constexpr uint8_t UAC_AC_FEATURE_UNIT    = 0x06;
static constexpr uint8_t UAC_AC_PROCESSING_UNIT = 0x07;
static constexpr uint8_t UAC_AC_EXTENSION_UNIT  = 0x08;

// -- AudioStreaming interface descriptor subtypes (bDescriptorSubType) ---------
static constexpr uint8_t UAC_AS_GENERAL     = 0x01;
static constexpr uint8_t UAC_AS_FORMAT_TYPE = 0x02;

// -- Class-specific endpoint descriptor subtypes -------------------------------
static constexpr uint8_t UAC_EP_GENERAL = 0x01;

// -- Audio class request codes (bRequest) -------------------------------------
static constexpr uint8_t UAC_SET_CUR = 0x01;
static constexpr uint8_t UAC_GET_CUR = 0x81;
static constexpr uint8_t UAC_SET_MIN = 0x02;
static constexpr uint8_t UAC_GET_MIN = 0x82;
static constexpr uint8_t UAC_SET_MAX = 0x03;
static constexpr uint8_t UAC_GET_MAX = 0x83;

// -- bmRequestType values ------------------------------------------------------
// Class request to interface:  OUT=0x21  IN=0xA1
// Class request to endpoint:   OUT=0x22  IN=0xA2
static constexpr uint8_t UAC_REQ_TYPE_INTF_SET = 0x21;
static constexpr uint8_t UAC_REQ_TYPE_INTF_GET = 0xA1;
static constexpr uint8_t UAC_REQ_TYPE_EP_SET   = 0x22;
static constexpr uint8_t UAC_REQ_TYPE_EP_GET   = 0xA2;

// -- Feature Unit control selectors (wValue high byte) ------------------------
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

// -- Sampling Frequency endpoint control selector ------------------------------
static constexpr uint8_t UAC_EP_SAMPLING_FREQ_CONTROL = 0x01;

// -- Feature Unit channel mask bits (bmaControls) -----------------------------
static constexpr uint8_t UAC_FU_CTL_MUTE   = (1 << 0);
static constexpr uint8_t UAC_FU_CTL_VOLUME = (1 << 1);

// -- Terminal types (specification section 2.1) -------------------------------
// An Output Terminal of this type sends audio to the host, so it belongs to the capture
// path. Any other Output Terminal type is a physical sink and belongs to playback.
static constexpr uint16_t UAC_TERMINAL_TYPE_USB_STREAMING = 0x0101;

// -- Maximum number of discrete sample frequencies per alt-setting -------------
static constexpr uint8_t UAC_MAX_SAMPLE_FREQS = 8;

// -- Maximum number of AudioControl units and terminals tracked ----------------
static constexpr uint8_t UAC_MAX_TOPOLOGY_NODES = 16;

// -- One AudioControl unit or terminal, reduced to what a topology walk needs --
struct UacTopologyNode {
  uint8_t  id{0};
  uint8_t  subtype{0};
  // Upstream connection. 0 means the node has none, which is also the specification's
  // "no connection" value, so it doubles as a chain terminator.
  uint8_t  first_source{0};
  uint16_t terminal_type{0};  // Input and Output Terminals only
};

// -- Described audio streaming alt-setting parameters -------------------------
// Filled during descriptor parsing for each AS interface alternate setting > 0.
struct UacAltInfo {
  uint8_t  alt_setting{0};
  uint8_t  ep_addr{0};        // isochronous endpoint address (direction embedded)
  uint8_t  ep_attr{0};        // endpoint bmAttributes (sync type in bits 3:2)
  uint8_t  feedback_ep_addr{0};  // companion async feedback IN endpoint (0 = none)
  uint16_t feedback_mps{0};      // feedback endpoint max packet size
  uint16_t mps{0};            // max packet size x mult
  uint8_t  b_interval{0};     // endpoint bInterval (service interval; 0 = not read)
  uint8_t  channels{0};
  uint8_t  sub_frame_size{0}; // bytes per audio sample (1, 2, 3, or 4)
  uint8_t  bit_resolution{0}; // actual bits used (e.g. 16 or 24)
  uint8_t  sample_freq_type{0};  // 0 = continuous range; N = N discrete freqs
  uint32_t sample_freq_lower{0}; // used when sample_freq_type == 0
  uint32_t sample_freq_upper{0}; // used when sample_freq_type == 0
  uint32_t sample_freq[UAC_MAX_SAMPLE_FREQS]{};
  bool     valid{false};
};

// -- Feature Unit bookkeeping -------------------------------------------------
struct UacFeatureUnit {
  uint8_t unit_id{0};
  uint8_t source_id{0};
  // bmaControls entries: the master entry plus one per logical channel.
  uint8_t control_entries{0};
  uint8_t control_size{0};  // bControlSize: bytes per bmaControls entry
  // bmaControls[0] = master, [1..n] = per-channel
  uint8_t master_controls{0};
  bool    has_mute{false};    // master entry
  bool    has_volume{false};  // master entry
  // Channels that carry the control when the master entry does not. Bit i stands for
  // logical channel i + 1, so up to eight channels are covered.
  uint8_t mute_channels{0};
  uint8_t volume_channels{0};

  bool mute_available() const { return this->has_mute || this->mute_channels != 0; }
  bool volume_available() const { return this->has_volume || this->volume_channels != 0; }
};

// Channel number addressing every channel of a Feature Unit at once.
static constexpr uint8_t UAC_FU_MASTER_CHANNEL = 0;
// Channels addressable through the per-channel bitmaps above.
static constexpr uint8_t UAC_FU_MAX_CHANNELS = 8;

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
