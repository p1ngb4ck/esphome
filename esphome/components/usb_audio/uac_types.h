#pragma once

// USB Audio Class 1.0 descriptor types, subtype codes, control request codes,
// Feature Unit control selectors, and Type I format descriptor structures.
// Sourced from: USB Device Class Definition for Audio Devices, revision 1.0.
// No dependency on any Espressif UAC host library.

#include <stdint.h>

#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

namespace esphome {
namespace usb_audio {

// -- Audio class version (bcdADC in the class-specific AC interface header) ---
// The two versions share their descriptor headers and their addressing, but they differ in
// how a Feature Unit lists its controls, in how a parameter's range is asked for, and in
// where the sample clock lives. Everything that differs is selected from this value.
static constexpr uint16_t UAC_VERSION_1 = 0x0100;
static constexpr uint16_t UAC_VERSION_2 = 0x0200;

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
// UAC 2.0 only: the sample clock became an entity of its own.
static constexpr uint8_t UAC2_AC_CLOCK_SOURCE     = 0x0A;
static constexpr uint8_t UAC2_AC_CLOCK_SELECTOR   = 0x0B;
static constexpr uint8_t UAC2_AC_CLOCK_MULTIPLIER = 0x0C;

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
static constexpr uint8_t UAC_SET_RES = 0x04;
static constexpr uint8_t UAC_GET_RES = 0x84;

// UAC 2.0 replaced the per-attribute request codes with two: CUR carries the value and
// RANGE returns every (MIN, MAX, RES) triplet the control offers. The direction lives in
// bmRequestType, so the same code is used for reading and writing.
static constexpr uint8_t UAC2_CS_CUR   = 0x01;
static constexpr uint8_t UAC2_CS_RANGE = 0x02;

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
// UAC 1.0: the sample rate is a control on the isochronous endpoint itself.
static constexpr uint8_t UAC_EP_SAMPLING_FREQ_CONTROL = 0x01;
// UAC 2.0: it is a control on the Clock Source entity feeding the terminal, and the value
// is four bytes instead of three.
static constexpr uint8_t UAC2_CS_SAM_FREQ_CONTROL = 0x01;

// -- Feature Unit control bits (bmaControls) ----------------------------------
// UAC 1.0 gives each control one bit per entry.
static constexpr uint8_t UAC_FU_CTL_MUTE   = (1 << 0);
static constexpr uint8_t UAC_FU_CTL_VOLUME = (1 << 1);
// UAC 2.0 gives each control two bits in a fixed four byte entry: 0 means the control is
// not there, 1 that it can only be read, 3 that the host may set it. Only a control the
// host may set is of use here, and 2 is not a defined value.
static constexpr uint32_t UAC2_FU_CTL_MUTE_MASK    = 0x00000003u;
static constexpr uint32_t UAC2_FU_CTL_MUTE_RW      = 0x00000003u;
static constexpr uint32_t UAC2_FU_CTL_VOLUME_MASK  = 0x0000000Cu;
static constexpr uint32_t UAC2_FU_CTL_VOLUME_RW    = 0x0000000Cu;
// Bytes per bmaControls entry in UAC 2.0; the descriptor has no bControlSize field.
static constexpr uint8_t UAC2_FU_CONTROL_SIZE = 4;

// -- Terminal types (specification section 2.1) -------------------------------
// An Output Terminal of this type sends audio to the host, so it belongs to the capture
// path. Any other Output Terminal type is a physical sink and belongs to playback.
static constexpr uint16_t UAC_TERMINAL_TYPE_USB_STREAMING = 0x0101;

// -- Spatial channel positions (UAC 1.0 wChannelConfig / UAC 2.0 bmChannelConfig) --
// The first twelve bits are identical in both class versions. A channel's position inside
// an audio frame is its index among the set bits, counting up from bit 0, so the bitmap is
// both the layout and the ordering.
static constexpr uint32_t UAC_CH_FRONT_LEFT            = 1u << 0;
static constexpr uint32_t UAC_CH_FRONT_RIGHT           = 1u << 1;
static constexpr uint32_t UAC_CH_FRONT_CENTER          = 1u << 2;
static constexpr uint32_t UAC_CH_LFE                   = 1u << 3;
static constexpr uint32_t UAC_CH_BACK_LEFT             = 1u << 4;
static constexpr uint32_t UAC_CH_BACK_RIGHT            = 1u << 5;
static constexpr uint32_t UAC_CH_FRONT_LEFT_OF_CENTER  = 1u << 6;
static constexpr uint32_t UAC_CH_FRONT_RIGHT_OF_CENTER = 1u << 7;
static constexpr uint32_t UAC_CH_BACK_CENTER           = 1u << 8;
static constexpr uint32_t UAC_CH_SIDE_LEFT             = 1u << 9;
static constexpr uint32_t UAC_CH_SIDE_RIGHT            = 1u << 10;
static constexpr uint32_t UAC_CH_TOP_CENTER            = 1u << 11;

// Which pair of a multichannel stream is carried as stereo. ESPHome has no surround path,
// so a 5.1 or 7.1 device is driven as one stereo pair and the other channels stay silent.
// Front is the pair a card marks as the main output.
enum class UacChannelPair : uint8_t {
  FRONT = 0,
  SIDE = 1,
  BACK = 2,
};

// The two position bits making up a pair.
inline uint32_t uac_pair_mask(UacChannelPair pair) {
  switch (pair) {
    case UacChannelPair::SIDE: return UAC_CH_SIDE_LEFT | UAC_CH_SIDE_RIGHT;
    case UacChannelPair::BACK: return UAC_CH_BACK_LEFT | UAC_CH_BACK_RIGHT;
    case UacChannelPair::FRONT:
    default: return UAC_CH_FRONT_LEFT | UAC_CH_FRONT_RIGHT;
  }
}

// Frame positions a pair takes when the device declares no bitmap at all. The class then
// leaves the layout undefined and the convention, which the Linux driver follows too, is the
// order of the position list above.
inline void uac_pair_default_offsets(UacChannelPair pair, uint8_t &left, uint8_t &right) {
  switch (pair) {
    case UacChannelPair::SIDE:
      left = 9;
      right = 10;
      break;
    case UacChannelPair::BACK:
      left = 4;
      right = 5;
      break;
    case UacChannelPair::FRONT:
    default:
      left = 0;
      right = 1;
      break;
  }
}

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
  // Spatial layout of those channels, 0 when the device declares none.
  uint32_t channel_config{0};
  // Set when the device offers no plain stereo alt-setting and a pair is being lifted out of
  // a multichannel frame. map_left and map_right are the pair's frame positions; every other
  // position in the frame is written as silence on playback and dropped on capture.
  bool     channel_map_active{false};
  uint8_t  map_left{0};
  uint8_t  map_right{0};
  uint8_t  sub_frame_size{0}; // bytes per audio sample (1, 2, 3, or 4)
  uint8_t  bit_resolution{0}; // actual bits used (e.g. 16 or 24)
  uint8_t  sample_freq_type{0};  // 0 = continuous range; N = N discrete freqs
  uint32_t sample_freq_lower{0}; // used when sample_freq_type == 0
  uint32_t sample_freq_upper{0}; // used when sample_freq_type == 0
  uint32_t sample_freq[UAC_MAX_SAMPLE_FREQS]{};
  // bTerminalLink from the class-specific AS interface descriptor: the Terminal of the
  // AudioControl interface this stream is the USB end of. It is how the Clock Source that
  // drives the stream is found.
  uint8_t  terminal_link{0};
  // UAC 2.0 does not list sample frequencies in the descriptors; the Clock Source entity
  // answers for them. An alt-setting marked this way is not matched against a frequency
  // during parsing, and the rate is set on the clock when the stream opens.
  bool     freq_from_clock{false};
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

// -- Volume and mute state of one direction -----------------------------------
// Playback and capture differ only in which Feature Unit they address, so both keep the
// same block and the same code drives them.
struct UacControlState {
  UacFeatureUnit fu{};
  // The last requested values. A request made while no device is attached is held here, so
  // these can be ahead of what the device has been told.
  float volume{1.0f};
  bool  muted{false};
  // Whether the values above have already been sent to the device currently attached, so
  // an unchanged setting is not requested again.
  bool  volume_sent{false};
  bool  mute_sent{false};

  // What the device answered when asked what its volume scale is. The endpoints are only
  // meaningful while range_known is set: they are the device's own, never a stand-in.
  bool    range_known{false};
  int16_t vol_min{0};
  int16_t vol_max{0};
  int16_t vol_res{1};  // the step the device can actually be set to

  // Whether the device answers GET_CUR with a constant regardless of what was last set.
  // A device like that says nothing about whether SET_CUR took effect, so its answers are
  // not read back into anything and the requested value is what gets reported.
  bool    get_cur_broken{false};

  // Sample clock entity driving this direction (UAC 2.0 only; 0 when there is none).
  uint8_t clock_id{0};
};

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
