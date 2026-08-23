#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_audio.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "usb/usb_helpers.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <memory>

namespace esphome {
namespace usb_audio {

static const char *const TAG = "usb_audio";

// Timeout for each blocking control transfer during connection setup.
static constexpr uint32_t CTRL_TIMEOUT_MS = 1000;

// Largest data stage any control request in this component uses.
static constexpr size_t UAC_CTRL_MAX_DATA_LEN = 8;

// URBs per stream (triple-buffering).
static constexpr uint8_t ISOC_NUM_URBS = 3;

// Packets per URB - small for audio to minimise latency.
static constexpr uint8_t ISOC_PACKETS_PER_URB = 4;

// Service intervals per second for an isochronous endpoint. At full speed the endpoint is
// serviced every bInterval frames (1 ms each), at high speed every 2^(bInterval-1)
// microframes (125 us each). bInterval 0 is illegal for isochronous endpoints; treat it as
// "every interval" so a sloppy descriptor still yields the common 1 ms / 125 us case.
static uint32_t uac_service_intervals_per_second(bool high_speed, uint8_t b_interval) {
  if (high_speed) {
    const uint8_t exponent = (b_interval >= 1 && b_interval <= 16) ? static_cast<uint8_t>(b_interval - 1) : 0;
    return std::max<uint32_t>(1, 8000u >> exponent);
  }
  const uint32_t period = (b_interval >= 1 && b_interval <= 16) ? b_interval : 1;
  return std::max<uint32_t>(1, 1000u / period);
}

// -----------------------------------------------------------------------------
// Configuration setters
// -----------------------------------------------------------------------------

void USBAudioClient::set_microphone_params(uint8_t channels, uint16_t bits, uint32_t sample_rate) {
  this->mic_cfg_.channels = channels;
  this->mic_cfg_.bits_per_sample = bits;
  this->mic_cfg_.sample_rate = sample_rate;
  this->mic_cfg_.configured = true;
}

void USBAudioClient::set_speaker_params(uint8_t channels, uint16_t bits, uint32_t sample_rate) {
  this->spk_cfg_.channels = channels;
  this->spk_cfg_.bits_per_sample = bits;
  this->spk_cfg_.sample_rate = sample_rate;
  this->spk_cfg_.configured = true;
}

// -----------------------------------------------------------------------------
// Component lifecycle
// -----------------------------------------------------------------------------

void USBAudioClient::setup() {
  if (this->spk_buf_size_ == 0)
    this->spk_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;
  if (this->mic_buf_size_ == 0)
    this->mic_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;

  usb_host::USBClient::setup();
}

void USBAudioClient::loop() {
  this->process_usb_events_();

  // A stream can stop on its own: the device went away mid-transfer, or the periodic
  // scheduler rejected every resubmission until no URB was left. Nothing is drained after
  // that, so every write would block until timeout forever. Tear it down here so the state
  // reflects reality and the normal reopen path (with backoff) can take over.
  if (this->spk_stream_open_ && this->spk_stream_.died.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Speaker stream stopped unexpectedly; closing");
    this->close_speaker_stream_();
    this->note_open_result_(false, this->spk_reopen_at_ms_, this->spk_open_fails_);
  }
  if (this->mic_stream_open_ && this->mic_stream_.died.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Microphone stream stopped unexpectedly; closing");
    this->close_microphone_stream_();
    this->note_open_result_(false, this->mic_reopen_at_ms_, this->mic_open_fails_);
  }

  if (this->device_connected_ && (int32_t) (millis() - this->spk_ctrl_retry_at_ms_) >= 0) {
    if (this->pending_spk_volume_)
      this->apply_speaker_volume_();
    if (this->pending_spk_mute_)
      this->apply_speaker_mute_();
  }
}

// Backoff helpers. next_attempt_ms is an absolute millis() deadline; fail_count only sets
// how far it is pushed out.
bool USBAudioClient::reopen_due_(const uint32_t &next_attempt_ms, uint8_t fail_count) const {
  if (fail_count == 0)
    return true;
  return (int32_t) (millis() - next_attempt_ms) >= 0;
}

void USBAudioClient::note_open_result_(bool ok, uint32_t &next_attempt_ms, uint8_t &fail_count) {
  if (ok) {
    fail_count = 0;
    next_attempt_ms = millis();
    return;
  }
  uint32_t delay_ms = STREAM_REOPEN_BASE_MS;
  for (uint8_t i = 0; i < fail_count && delay_ms < STREAM_REOPEN_MAX_MS; i++)
    delay_ms *= 2;
  if (delay_ms > STREAM_REOPEN_MAX_MS)
    delay_ms = STREAM_REOPEN_MAX_MS;
  if (fail_count < 255)
    fail_count++;
  next_attempt_ms = millis() + delay_ms;
  ESP_LOGD(TAG, "Stream open failed (%u in a row); next attempt in %" PRIu32 " ms", fail_count, delay_ms);
}

void USBAudioClient::set_stream_error_(const char *what) {
  ESP_LOGE(TAG, "%s", what);
  this->status_set_error();
}

uint32_t USBAudioClient::get_speaker_queued_bytes() const {
  if (this->spk_rb_ == nullptr)
    return 0;
  const size_t free_bytes = xRingbufferGetCurFreeSize(this->spk_rb_);
  return (this->spk_buf_size_ > free_bytes) ? static_cast<uint32_t>(this->spk_buf_size_ - free_bytes) : 0;
}

void USBAudioClient::flush_speaker_buffer_() {
  if (this->spk_rb_ == nullptr)
    return;
  // Drain whatever is left so a reopened stream does not start by playing stale audio.
  while (true) {
    size_t received = 0;
    void *item = xRingbufferReceiveUpTo(this->spk_rb_, &received, 0, this->spk_buf_size_);
    if (item == nullptr)
      break;
    vRingbufferReturnItem(this->spk_rb_, item);
    if (received == 0)
      break;
  }
}

// -----------------------------------------------------------------------------
// Descriptor parsing
// -----------------------------------------------------------------------------

bool USBAudioClient::find_ac_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType != USB_W_VALUE_DT_INTERFACE)
      continue;
    const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
    if (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
        id->bAlternateSetting == 0) {
      this->ac_intf_ = id->bInterfaceNumber;
      return true;
    }
  }
  ESP_LOGE(TAG, "No AudioControl interface found");
  return false;
}

// Offset of the first upstream source ID inside an AudioControl unit or terminal
// descriptor. The leading fields of these descriptors are the same in UAC 1.0 and 2.0, so
// a walk built on them does not depend on the audio class version. Returns 0 for
// descriptors that have no upstream connection.
static uint8_t uac_first_source_offset(uint8_t subtype) {
  switch (subtype) {
    case UAC_AC_OUTPUT_TERMINAL:
      return 7;  // bSourceID
    case UAC_AC_FEATURE_UNIT:
      return 4;  // bSourceID
    case UAC_AC_MIXER_UNIT:
    case UAC_AC_SELECTOR_UNIT:
      return 5;  // baSourceID[0], preceded by bNrInPins
    case UAC_AC_PROCESSING_UNIT:
    case UAC_AC_EXTENSION_UNIT:
      return 7;  // baSourceID[0], preceded by wProcessType and bNrInPins
    default:
      return 0;
  }
}

static const UacTopologyNode *uac_find_node(const UacTopologyNode *nodes, uint8_t count, uint8_t id) {
  for (uint8_t i = 0; i < count; i++) {
    if (nodes[i].id == id)
      return &nodes[i];
  }
  return nullptr;
}

// Follow the signal path upstream from the Output Terminal of one direction and return the
// first Feature Unit on it, or 0 when there is none. A unit with several inputs is followed
// on its first pin only, which is where a device that offers one volume control puts it.
static uint8_t uac_find_feature_unit(const UacTopologyNode *nodes, uint8_t count, bool capture) {
  for (uint8_t i = 0; i < count; i++) {
    if (nodes[i].subtype != UAC_AC_OUTPUT_TERMINAL)
      continue;
    if ((nodes[i].terminal_type == UAC_TERMINAL_TYPE_USB_STREAMING) != capture)
      continue;
    uint8_t next = nodes[i].first_source;
    // A malformed descriptor can describe a cycle, so bound the walk by the node count.
    for (uint8_t hop = 0; hop < count && next != 0; hop++) {
      const UacTopologyNode *node = uac_find_node(nodes, count, next);
      if (node == nullptr)
        break;
      if (node->subtype == UAC_AC_FEATURE_UNIT)
        return node->id;
      next = node->first_source;
    }
  }
  return 0;
}

uint8_t USBAudioClient::collect_topology_(UacTopologyNode *nodes) {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return 0;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  uint8_t count = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 4)
      continue;
    if (count >= UAC_MAX_TOPOLOGY_NODES) {
      ESP_LOGW(TAG, "Audio function has more than %u units; topology is incomplete", UAC_MAX_TOPOLOGY_NODES);
      break;
    }

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    const uint8_t subtype = d[2];
    const bool is_terminal = (subtype == UAC_AC_INPUT_TERMINAL || subtype == UAC_AC_OUTPUT_TERMINAL);
    const uint8_t src_off = uac_first_source_offset(subtype);
    if (src_off == 0 && !is_terminal)
      continue;  // header or an unknown descriptor: not part of the signal path

    UacTopologyNode node{};
    node.subtype = subtype;
    node.id = d[3];
    if (is_terminal) {
      if (desc->bLength < 6)
        continue;
      node.terminal_type = static_cast<uint16_t>(d[4] | (d[5] << 8));
    }
    if (src_off != 0) {
      if (desc->bLength <= src_off)
        continue;
      node.first_source = d[src_off];
    }
    nodes[count++] = node;
  }
  return count;
}

uint8_t USBAudioClient::collect_feature_units_(UacFeatureUnit *units, uint8_t max_count) {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return 0;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  uint8_t count = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr && count < max_count) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 7)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_FEATURE_UNIT)
      continue;

    // Feature Unit layout (UAC 1.0 section 4.3.2.5):
    // [3] bUnitID  [4] bSourceID  [5] bControlSize
    // [6..] bmaControls[0] (master), then one entry per channel.
    UacFeatureUnit fu{};
    fu.unit_id      = d[3];
    fu.source_id    = d[4];
    fu.control_size = d[5];
    if (fu.control_size == 0 || (7 + fu.control_size) > desc->bLength)
      continue;
    fu.master_controls = d[6];
    fu.has_mute   = (fu.master_controls & UAC_FU_CTL_MUTE) != 0;
    fu.has_volume = (fu.master_controls & UAC_FU_CTL_VOLUME) != 0;
    fu.control_entries = (desc->bLength - 7) / fu.control_size;

    // Not every device puts its controls on the master entry; some describe them per
    // channel only. Record those so a control that exists can still be reached.
    for (uint8_t entry = 1; entry < fu.control_entries && entry <= UAC_FU_MAX_CHANNELS; entry++) {
      const uint16_t control_off = 6 + static_cast<uint16_t>(entry) * fu.control_size;
      if (control_off >= desc->bLength)
        break;
      const uint8_t controls = d[control_off];
      if (controls & UAC_FU_CTL_MUTE)
        fu.mute_channels |= static_cast<uint8_t>(1 << (entry - 1));
      if (controls & UAC_FU_CTL_VOLUME)
        fu.volume_channels |= static_cast<uint8_t>(1 << (entry - 1));
    }

    units[count++] = fu;
  }
  return count;
}

bool USBAudioClient::parse_feature_units_() {
  // Another device may have been attached before, so start from nothing.
  this->speaker_fu_ = {};
  this->mic_fu_ = {};

  UacTopologyNode nodes[UAC_MAX_TOPOLOGY_NODES];
  const uint8_t node_count = this->collect_topology_(nodes);

  UacFeatureUnit units[UAC_MAX_TOPOLOGY_NODES];
  const uint8_t unit_count = this->collect_feature_units_(units, UAC_MAX_TOPOLOGY_NODES);

  uint8_t spk_fu_id = uac_find_feature_unit(nodes, node_count, false);
  uint8_t mic_fu_id = uac_find_feature_unit(nodes, node_count, true);

  // A device may describe a topology this walk cannot follow. Picking a Feature Unit by
  // descriptor order would then be a guess, and sending a control request to the wrong unit
  // changes the wrong thing, so only fall back when there is nothing to guess between.
  if (spk_fu_id == 0 && mic_fu_id == 0 && unit_count == 1) {
    ESP_LOGD(TAG, "Could not follow the audio topology; using the only Feature Unit found");
    spk_fu_id = units[0].unit_id;
    mic_fu_id = units[0].unit_id;
  }

  for (uint8_t i = 0; i < unit_count; i++) {
    if (this->spk_cfg_.configured && units[i].unit_id == spk_fu_id)
      this->speaker_fu_ = units[i];
    if (this->mic_cfg_.configured && units[i].unit_id == mic_fu_id)
      this->mic_fu_ = units[i];
  }

  // Unit ID 0 is reserved by the specification, so it marks "nothing found" here.
  if (this->spk_cfg_.configured) {
    if (this->speaker_fu_.unit_id != 0) {
      ESP_LOGD(TAG, "Speaker Feature Unit: id=%u mute=%s vol=%s", this->speaker_fu_.unit_id,
               YESNO(this->speaker_fu_.mute_available()), YESNO(this->speaker_fu_.volume_available()));
    } else {
      ESP_LOGW(TAG, "No Feature Unit in the playback path; volume and mute are unavailable");
    }
  }
  if (this->mic_cfg_.configured) {
    if (this->mic_fu_.unit_id != 0) {
      ESP_LOGD(TAG, "Mic Feature Unit: id=%u mute=%s vol=%s", this->mic_fu_.unit_id, YESNO(this->mic_fu_.mute_available()),
               YESNO(this->mic_fu_.volume_available()));
    } else {
      ESP_LOGW(TAG, "No Feature Unit in the capture path; volume and mute are unavailable");
    }
  }

  return true;  // non-fatal if not found - volume/mute will be silently skipped
}

bool USBAudioClient::parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits, uint32_t sample_rate,
                                          uint8_t &intf_out, UacAltInfo &alt_out) {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

  // Isochronous high-bandwidth transactions (the wMaxPacketSize MULT field) exist only at
  // high speed. Resolve the device speed once so a full-speed UAC device does not get its
  // mps inflated below (see the endpoint parse). ESP32-P4 is the first HS-capable host, so
  // this only diverges there; older FS-only hosts always take the clamped path anyway.
  usb_device_info_t dev_info;
  const bool is_high_speed =
      usb_host_device_info(this->device_handle_, &dev_info) == ESP_OK && dev_info.speed == USB_SPEED_HIGH;
  this->device_is_high_speed_ = is_high_speed;

  uint8_t cur_intf = 0;
  uint8_t cur_alt  = 0;
  bool    cur_is_as = false;
  UacAltInfo cur_alt_info{};
  bool format_seen = false;

  auto try_commit = [&]() {
    if (!format_seen || !cur_alt_info.valid)
      return;
    // Check if this alt-setting matches the requested format.
    if (cur_alt_info.channels != channels || cur_alt_info.bit_resolution != bits)
      return;
    // Check sample rate.
    bool freq_ok = false;
    if (cur_alt_info.sample_freq_type == 0) {
      freq_ok = (sample_rate >= cur_alt_info.sample_freq_lower && sample_rate <= cur_alt_info.sample_freq_upper);
    } else {
      for (uint8_t i = 0; i < cur_alt_info.sample_freq_type && i < UAC_MAX_SAMPLE_FREQS; i++) {
        if (cur_alt_info.sample_freq[i] == sample_rate) {
          freq_ok = true;
          break;
        }
      }
    }
    if (!freq_ok)
      return;
    // Check endpoint direction.
    bool ep_is_out = (cur_alt_info.ep_addr & 0x80) == 0;
    if (ep_is_out != want_out)
      return;
    // Accept this alt-setting.
    intf_out = cur_intf;
    alt_out  = cur_alt_info;
  };

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    const uint8_t dtype = desc->bDescriptorType;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);

    if (dtype == USB_W_VALUE_DT_INTERFACE) {
      try_commit();
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      cur_intf   = id->bInterfaceNumber;
      cur_alt    = id->bAlternateSetting;
      cur_is_as  = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOSTREAMING);
      cur_alt_info = {};
      format_seen  = false;
      // alt 0 is zero-bandwidth - skip but stay on interface
      continue;
    }

    if (!cur_is_as || cur_alt == 0)
      continue;

    if (dtype == UAC_CS_INTERFACE && desc->bLength >= 3) {
      uint8_t sub = d[2];

      if (sub == UAC_AS_FORMAT_TYPE && desc->bLength >= 8) {
        // Type I Format Type descriptor (UAC 1.0 table 4-21):
        // [0] bLength [1] bDescriptorType [2] bDescriptorSubType
        // [3] bFormatType (must be 0x01 for Type I)
        // [4] bNrChannels [5] bSubFrameSize [6] bBitResolution
        // [7] bSamFreqType  then: continuous=(lower 3B, upper 3B); discrete=Nx3B each
        if (d[3] != 0x01)  // only Type I
          continue;
        cur_alt_info.channels       = d[4];
        cur_alt_info.sub_frame_size = d[5];
        cur_alt_info.bit_resolution = d[6];
        cur_alt_info.sample_freq_type = d[7];
        if (cur_alt_info.sample_freq_type == 0 && desc->bLength >= 14) {
          // Continuous: tLowerSamFreq at bytes 8-10, tUpperSamFreq at 11-13
          cur_alt_info.sample_freq_lower = static_cast<uint32_t>(d[8]) |
                                           (static_cast<uint32_t>(d[9]) << 8) |
                                           (static_cast<uint32_t>(d[10]) << 16);
          cur_alt_info.sample_freq_upper = static_cast<uint32_t>(d[11]) |
                                           (static_cast<uint32_t>(d[12]) << 8) |
                                           (static_cast<uint32_t>(d[13]) << 16);
        } else {
          uint8_t n = std::min<uint8_t>(cur_alt_info.sample_freq_type, UAC_MAX_SAMPLE_FREQS);
          for (uint8_t i = 0; i < n; i++) {
            uint16_t base = 8 + static_cast<uint16_t>(i) * 3;
            if (base + 2 >= desc->bLength)
              break;
            cur_alt_info.sample_freq[i] = static_cast<uint32_t>(d[base]) |
                                           (static_cast<uint32_t>(d[base + 1]) << 8) |
                                           (static_cast<uint32_t>(d[base + 2]) << 16);
          }
        }
        format_seen = true;
        continue;
      }
    }

    if (dtype == USB_W_VALUE_DT_ENDPOINT && cur_is_as && cur_alt > 0 && desc->bLength >= sizeof(usb_ep_desc_t)) {
      const auto *ep = reinterpret_cast<const usb_ep_desc_t *>(desc);
      if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_BM_ATTRIBUTES_XFER_ISOC)
        continue;
      const bool ep_is_in = (ep->bEndpointAddress & 0x80) != 0;
      const bool want_in = !want_out;
      if (ep_is_in == want_in) {
        // Data endpoint for this stream's direction.
        cur_alt_info.ep_addr = ep->bEndpointAddress;
        cur_alt_info.ep_attr = ep->bmAttributes;
        // MULT (wMaxPacketSize bits 12:11) is a high-speed-only high-bandwidth multiplier; on
        // full speed those bits are reserved and an isochronous packet is capped at 1023 bytes.
        // Applying MULT to a full-speed device inflates the ISO DMA buffer past what the link
        // moves per frame and crashes the HS host controller (ESP32-P4 with a full-speed UAC
        // device), so honour MULT only at high speed and clamp otherwise.
        cur_alt_info.mps = is_high_speed
                               ? static_cast<uint16_t>(USB_EP_DESC_GET_MPS(ep) * (USB_EP_DESC_GET_MULT(ep) + 1))
                               : std::min<uint16_t>(USB_EP_DESC_GET_MPS(ep), 1023);
        // bInterval selects the service interval and therefore how many bytes per interval
        // the sample clock has to produce; without it the pacing can only guess.
        cur_alt_info.b_interval = ep->bInterval;
        // UAC1 audio endpoint descriptor (bLength 9) carries bSynchAddress at offset 8: the
        // companion feedback IN endpoint address for an asynchronous OUT endpoint.
        if (desc->bLength >= 9 && reinterpret_cast<const uint8_t *>(ep)[8] != 0)
          cur_alt_info.feedback_ep_addr = reinterpret_cast<const uint8_t *>(ep)[8];
        cur_alt_info.alt_setting = cur_alt;
        cur_alt_info.valid = true;
      } else {
        // Opposite-direction isochronous endpoint in the same AS interface: the explicit
        // feedback endpoint (UAC2, or UAC1 where bSynchAddress was not used).
        cur_alt_info.feedback_ep_addr = ep->bEndpointAddress;
        cur_alt_info.feedback_mps = USB_EP_DESC_GET_MPS(ep);
      }
      continue;
    }
  }

  // Commit the final accumulated alt-setting.
  try_commit();

  if (!alt_out.valid) {
    ESP_LOGE(TAG, "No %s AS alt-setting found for channels=%u bits=%u rate=%" PRIu32,
             want_out ? "speaker" : "microphone", channels, bits, sample_rate);
    return false;
  }

  ESP_LOGI(TAG, "%s: intf=%u alt=%u ep=0x%02X mps=%u channels=%u bits=%u",
           want_out ? "Speaker" : "Mic", intf_out, alt_out.alt_setting, alt_out.ep_addr,
           alt_out.mps, alt_out.channels, alt_out.bit_resolution);
  return true;
}

// -----------------------------------------------------------------------------
// Control transfer helpers
// -----------------------------------------------------------------------------

namespace {
// Result of a control transfer. The caller stops waiting once the timeout expires, but the
// transfer stays submitted and its callback runs later on the USB task, so the callback
// must not write to anything owned by the caller's stack frame. Both sides hold a shared
// pointer to this block instead, and it lives until the last of them is gone.
struct UacControlResult {
  std::atomic<bool> done{false};
  bool success{false};
  uint8_t data[UAC_CTRL_MAX_DATA_LEN]{};
  size_t data_len{0};
};
}  // namespace

bool USBAudioClient::uac_control_transfer_(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                                            const uint8_t *out_data, size_t out_len, uint8_t *in_data,
                                            size_t in_len) {
  if (out_len > UAC_CTRL_MAX_DATA_LEN || in_len > UAC_CTRL_MAX_DATA_LEN) {
    ESP_LOGE(TAG, "Control request 0x%02X wants more than %u bytes of data", request,
             static_cast<unsigned>(UAC_CTRL_MAX_DATA_LEN));
    return false;
  }

  auto result = std::make_shared<UacControlResult>();
  std::vector<uint8_t> payload(out_data, out_data + out_len);

  if (!this->control_transfer(req_type, request, value, index,
                              [result](const usb_host::TransferStatus &s) {
                                result->success = s.success;
                                if (s.success && s.data != nullptr) {
                                  result->data_len = std::min<size_t>(s.data_len, UAC_CTRL_MAX_DATA_LEN);
                                  memcpy(result->data, s.data, result->data_len);
                                }
                                result->done.store(true, std::memory_order_release);
                              },
                              payload)) {
    // Nothing was handed to the host, so no callback is coming. Waiting for one would spend
    // the full timeout and then report a device that did not answer, which is not what
    // happened.
    ESP_LOGW(TAG, "Control request 0x%02X was not submitted", request);
    return false;
  }

  const uint32_t started = millis();
  while (!result->done.load(std::memory_order_acquire) && (millis() - started) < CTRL_TIMEOUT_MS)
    vTaskDelay(pdMS_TO_TICKS(5));

  if (!result->done.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Control request 0x%02X timed out", request);
    return false;
  }
  if (!result->success)
    return false;
  if (in_data != nullptr) {
    if (result->data_len < in_len)
      return false;
    memcpy(in_data, result->data, in_len);
  }
  return true;
}

bool USBAudioClient::uac_set_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             const uint8_t *data, size_t len) {
  // wValue: (selector << 8) | channel
  // wIndex: (unit_id << 8) | ac_intf_num
  const uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  const uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  return this->uac_control_transfer_(UAC_REQ_TYPE_INTF_SET, UAC_SET_CUR, wValue, wIndex, data, len, nullptr, 0);
}

bool USBAudioClient::uac_get_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             uint8_t *data, size_t len) {
  const uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  const uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  return this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, UAC_GET_CUR, wValue, wIndex, nullptr, 0, data, len);
}

bool USBAudioClient::uac_set_cur_endpoint_(uint8_t ep_addr, uint8_t selector,
                                            const uint8_t *data, size_t len) {
  const uint16_t wValue = static_cast<uint16_t>(selector << 8);
  return this->uac_control_transfer_(UAC_REQ_TYPE_EP_SET, UAC_SET_CUR, wValue, ep_addr, data, len, nullptr, 0);
}

bool USBAudioClient::set_sampling_frequency_(uint8_t ep_addr, uint32_t freq) {
  // Sampling frequency is 3 bytes little-endian.
  uint8_t buf[3] = {
      static_cast<uint8_t>(freq & 0xFF),
      static_cast<uint8_t>((freq >> 8) & 0xFF),
      static_cast<uint8_t>((freq >> 16) & 0xFF),
  };
  bool ok = this->uac_set_cur_endpoint_(ep_addr, UAC_EP_SAMPLING_FREQ_CONTROL, buf, sizeof(buf));
  if (!ok)
    ESP_LOGW(TAG, "SET_CUR sampling freq ep=0x%02X freq=%" PRIu32 " failed (may not be supported)", ep_addr, freq);
  return ok;
}

// -- Volume / mute -------------------------------------------------------------

// Channels a Feature Unit control has to be written to. A device that describes the
// control on the master entry takes a single request for all channels; one that describes
// it per channel needs a request per channel. Returns the number written to channels.
static uint8_t uac_control_channels(bool on_master, uint8_t channel_mask, uint8_t *channels) {
  if (on_master) {
    channels[0] = UAC_FU_MASTER_CHANNEL;
    return 1;
  }
  uint8_t count = 0;
  for (uint8_t i = 0; i < UAC_FU_MAX_CHANNELS; i++) {
    if (channel_mask & (1 << i))
      channels[count++] = static_cast<uint8_t>(i + 1);
  }
  return count;
}

uint8_t USBAudioClient::speaker_volume_channels_(uint8_t *channels) const {
  return uac_control_channels(this->speaker_fu_.has_volume, this->speaker_fu_.volume_channels, channels);
}

void USBAudioClient::probe_speaker_volume_range_() {
  if (!this->speaker_fu_.volume_available())
    return;

  // Query the range on the same channel the value will be written to.
  uint8_t vol_channels[UAC_FU_MAX_CHANNELS];
  const uint8_t vol_channel =
      this->speaker_volume_channels_(vol_channels) > 0 ? vol_channels[0] : UAC_FU_MASTER_CHANNEL;
  auto get_vol_range = [this, vol_channel](uint8_t bRequest, int16_t &out) {
    uint8_t buf[2] = {0, 0};
    const uint16_t wValue = static_cast<uint16_t>((UAC_FU_VOLUME_CONTROL << 8) | vol_channel);
    const uint16_t wIndex = static_cast<uint16_t>((this->speaker_fu_.unit_id << 8) | this->ac_intf_);
    if (!this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, bRequest, wValue, wIndex, nullptr, 0, buf, sizeof(buf)))
      return false;
    out = static_cast<int16_t>(buf[0] | (buf[1] << 8));
    return true;
  };
  const bool min_ok = get_vol_range(UAC_GET_MIN, this->spk_vol_min_);
  const bool max_ok = get_vol_range(UAC_GET_MAX, this->spk_vol_max_);
  // 0x8000 is the reserved "silence" code and never a range endpoint, and a range that is
  // not strictly increasing means the reads did not return a usable answer. Fall back to
  // the full signed range so the control still spans something sensible.
  if (!min_ok || !max_ok || this->spk_vol_min_ == UAC_VOLUME_SILENCE || this->spk_vol_max_ <= this->spk_vol_min_) {
    ESP_LOGW(TAG, "Speaker volume range unusable; falling back to %d..0 dB", UAC_VOLUME_FALLBACK_MIN / 256);
    this->spk_vol_min_ = UAC_VOLUME_FALLBACK_MIN;
    this->spk_vol_max_ = 0;
  }
  // State the range the way a user reads volume: how far below the device's reference
  // level (its maximum) the control can go.
  ESP_LOGD(TAG, "Speaker volume range: 0.00 to -%.2f dB below reference",
           static_cast<float>(this->spk_vol_max_ - this->spk_vol_min_) / 256.0f);
}

bool USBAudioClient::apply_speaker_volume_() {
  uint8_t channels[UAC_FU_MAX_CHANNELS];
  const uint8_t channel_count = this->speaker_volume_channels_(channels);

  if (channel_count == 0) {
    // The descriptor says there is no volume control. That is a property of the device, not
    // a transient failure, so stop asking. Later requests are still answered, at debug
    // level, because going silent leaves nothing to explain why the volume does not move.
    if (this->spk_volume_supported_) {
      ESP_LOGW(TAG, "Speaker volume control not supported by device");
      this->spk_volume_supported_ = false;
    } else {
      ESP_LOGD(TAG, "Speaker volume %.0f%% ignored: device has no volume control", this->spk_volume_ * 100.0f);
    }
    this->pending_spk_volume_ = false;
    return false;
  }
  if (!this->spk_volume_supported_) {
    ESP_LOGD(TAG, "Speaker volume %.0f%% ignored: volume control was given up on", this->spk_volume_ * 100.0f);
    return false;
  }

  // A UAC Feature Unit volume is logarithmic: a signed 1/256 dB value, where the device's
  // reported maximum is its reference level. ESPHome's volume is a linear amplitude
  // fraction. Mapping the fraction straight onto the dB range puts half volume near the
  // bottom of the scale and sounds all but muted, so convert amplitude to dB first.
  const float clamped = std::clamp(this->spk_volume_, 0.0f, 1.0f);
  const float min_db = static_cast<float>(this->spk_vol_min_) / 256.0f;
  const float max_db = static_cast<float>(this->spk_vol_max_) / 256.0f;
  int16_t vol;
  if (clamped <= 0.0f) {
    vol = this->spk_vol_min_;
  } else {
    const float db = std::clamp(max_db + 20.0f * std::log10(clamped), min_db, max_db);
    vol = static_cast<int16_t>(std::lround(db * 256.0f));
  }

  uint8_t buf[2] = {static_cast<uint8_t>(vol & 0xFF), static_cast<uint8_t>((vol >> 8) & 0xFF)};
  bool ok = true;
  for (uint8_t i = 0; i < channel_count; i++) {
    if (!this->uac_set_cur_interface_(this->speaker_fu_.unit_id, UAC_FU_VOLUME_CONTROL, channels[i], buf,
                                      sizeof(buf)))
      ok = false;
  }
  if (ok) {
    this->pending_spk_volume_ = false;
    this->spk_volume_fails_ = 0;
    // Read the value back. A device may clamp it to its own range or quantise it to a
    // coarser step (GET_RES), so reporting what we sent would be reporting an assumption.
    // Attenuation is stated as a positive number below the device's reference level, which
    // is how volume is normally read.
    const float want_att_db = max_db - static_cast<float>(vol) / 256.0f;
    uint8_t rb[2] = {0, 0};
    if (this->uac_get_cur_interface_(this->speaker_fu_.unit_id, UAC_FU_VOLUME_CONTROL, channels[0], rb,
                                     sizeof(rb))) {
      const int16_t actual = static_cast<int16_t>(rb[0] | (rb[1] << 8));
      const float actual_att_db = max_db - static_cast<float>(actual) / 256.0f;
      ESP_LOGD(TAG, "Speaker volume %.0f%% -> -%.2f dB (device reports -%.2f dB)", clamped * 100.0f, want_att_db,
               actual_att_db);
      // One step is 1/256 dB; anything past a quarter dB is the device overriding us, not
      // rounding.
      if (std::fabs(actual_att_db - want_att_db) > 0.25f) {
        ESP_LOGW(TAG, "Speaker volume not applied as requested: asked -%.2f dB, device is at -%.2f dB", want_att_db,
                 actual_att_db);
      }
    } else {
      ESP_LOGD(TAG, "Speaker volume %.0f%% -> -%.2f dB (readback unavailable)", clamped * 100.0f, want_att_db);
    }
    return true;
  }
  // The control exists but the transfer failed. Devices commonly NAK control requests while
  // they are busy setting up the stream, so retry a few times before giving up on it.
  this->spk_volume_fails_++;
  this->spk_ctrl_retry_at_ms_ = millis() + UAC_CTRL_RETRY_MS;
  if (this->spk_volume_fails_ >= UAC_CTRL_MAX_FAILS) {
    ESP_LOGW(TAG, "Speaker volume control failed %u times; giving up on it", this->spk_volume_fails_);
    this->pending_spk_volume_ = false;
    this->spk_volume_supported_ = false;
  } else {
    ESP_LOGD(TAG, "Failed to set speaker volume; retry %u of %u", this->spk_volume_fails_, UAC_CTRL_MAX_FAILS);
  }
  return false;
}

bool USBAudioClient::apply_speaker_mute_() {
  uint8_t channels[UAC_FU_MAX_CHANNELS];
  const uint8_t channel_count =
      uac_control_channels(this->speaker_fu_.has_mute, this->speaker_fu_.mute_channels, channels);

  if (channel_count == 0) {
    if (this->spk_mute_supported_) {
      ESP_LOGW(TAG, "Speaker mute control not supported by device");
      this->spk_mute_supported_ = false;
    } else {
      ESP_LOGD(TAG, "Speaker mute %s ignored: device has no mute control", ONOFF(this->spk_muted_));
    }
    this->pending_spk_mute_ = false;
    return false;
  }
  if (!this->spk_mute_supported_) {
    ESP_LOGD(TAG, "Speaker mute %s ignored: mute control was given up on", ONOFF(this->spk_muted_));
    return false;
  }

  uint8_t val = this->spk_muted_ ? 1 : 0;
  bool ok = true;
  for (uint8_t i = 0; i < channel_count; i++) {
    if (!this->uac_set_cur_interface_(this->speaker_fu_.unit_id, UAC_FU_MUTE_CONTROL, channels[i], &val, 1))
      ok = false;
  }
  if (ok) {
    this->pending_spk_mute_ = false;
    this->spk_mute_fails_ = 0;
    return true;
  }
  this->spk_mute_fails_++;
  this->spk_ctrl_retry_at_ms_ = millis() + UAC_CTRL_RETRY_MS;
  if (this->spk_mute_fails_ >= UAC_CTRL_MAX_FAILS) {
    ESP_LOGW(TAG, "Speaker mute control failed %u times; giving up on it", this->spk_mute_fails_);
    this->pending_spk_mute_ = false;
    this->spk_mute_supported_ = false;
  } else {
    ESP_LOGD(TAG, "Failed to set speaker mute; retry %u of %u", this->spk_mute_fails_, UAC_CTRL_MAX_FAILS);
  }
  return false;
}

bool USBAudioClient::open_speaker_stream_() {
  if (this->spk_stream_open_)
    return true;
  if (!this->spk_cfg_.configured || !this->spk_alt_.valid)
    return false;

  // Create ring buffer if not yet done.
  if (this->spk_rb_ == nullptr) {
    this->spk_rb_ = xRingbufferCreate(this->spk_buf_size_, RINGBUF_TYPE_BYTEBUF);
    if (this->spk_rb_ == nullptr) {
      this->set_stream_error_("Failed to allocate speaker ring buffer");
      return false;
    }
  }

  this->spk_stream_.ep_addr        = this->spk_alt_.ep_addr;
  this->spk_stream_.mps            = this->spk_alt_.mps;
  this->spk_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->spk_stream_.num_urbs       = ISOC_NUM_URBS;
  this->spk_stream_.interface_num  = this->spk_as_intf_;
  this->spk_stream_.alt_setting    = this->spk_alt_.alt_setting;

  // Sample-clock pacing for the OUT stream. frame_size = channels * subframe; the service
  // interval follows from the device speed and the endpoint's bInterval. packet_size is the
  // floor bytes per interval, packet_size_frac the per-interval frame remainder that the
  // accumulator folds back in (e.g. 44.1 kHz).
  {
    const uint32_t frame_size =
        static_cast<uint32_t>(this->spk_cfg_.channels) * (this->spk_cfg_.bits_per_sample / 8);
    const uint32_t derived_ips =
        uac_service_intervals_per_second(this->device_is_high_speed_, this->spk_alt_.b_interval);
    uint32_t frac_div = derived_ips;

    // A UAC endpoint sizes its mps to carry exactly one service interval of audio (plus at
    // most one extra frame for fractional rates and async slack). If the derived interval
    // rate asks for no more than half of mps, the speed or bInterval it came from does not
    // describe this endpoint - most UAC devices are full-speed and are serviced once per
    // 1 ms frame, so an inflated interval rate starves the device by that same factor.
    // Halve back down to what mps actually describes, never below the 1 ms frame rate.
    while (frac_div > 1000u && this->spk_alt_.mps != 0 &&
           ((this->spk_cfg_.sample_rate / frac_div) * frame_size) * 2 <= static_cast<uint32_t>(this->spk_alt_.mps)) {
      frac_div /= 2;
    }
    if (frac_div != derived_ips) {
      ESP_LOGW(TAG, "Speaker pacing: %" PRIu32 " intervals/s does not fit mps=%u, using %" PRIu32 "/s",
               derived_ips, this->spk_alt_.mps, frac_div);
    }

    this->spk_stream_.is_output       = true;
    this->spk_stream_.frame_size      = frame_size;
    this->spk_stream_.frac_div        = frac_div;
    this->spk_stream_.packet_size      = (this->spk_cfg_.sample_rate / frac_div) * frame_size;
    this->spk_stream_.packet_size_frac = this->spk_cfg_.sample_rate % frac_div;
    this->spk_stream_.frac_accum      = 0;
    this->spk_stream_.fb_value.store(0, std::memory_order_relaxed);
    this->spk_stream_.fb_accum        = 0;

    ESP_LOGI(TAG, "Speaker pacing: %s bInterval=%u %" PRIu32 " intervals/s, %" PRIu32
                  " bytes/packet (frac %" PRIu32 "/%" PRIu32 "), frame=%" PRIu32 " bytes, mps=%u",
             this->device_is_high_speed_ ? "HS" : "FS", this->spk_alt_.b_interval, frac_div,
             this->spk_stream_.packet_size, this->spk_stream_.packet_size_frac, frac_div, frame_size,
             this->spk_alt_.mps);
  }

  if (!this->stream_open(this->spk_stream_, this)) {
    ESP_LOGE(TAG, "stream_open_ failed for speaker");
    return false;
  }

  // Asynchronous OUT endpoint (bmAttributes sync type 0b01) with a companion feedback IN
  // endpoint: open it on the already-claimed AS interface so the device's own clock paces
  // playback. Adaptive/synchronous endpoints (or when disabled) stay on nominal pacing.
  const uint8_t sync_type = (this->spk_alt_.ep_attr >> 2) & 0x03;
  if (this->feedback_enabled_ && sync_type == 0x01 && this->spk_alt_.feedback_ep_addr != 0) {
    this->spk_stream_.fb_shift        = this->device_is_high_speed_ ? 16 : 14;
    this->spk_fb_stream_.ep_addr      = this->spk_alt_.feedback_ep_addr;
    this->spk_fb_stream_.mps          = this->spk_alt_.feedback_mps != 0
                                            ? this->spk_alt_.feedback_mps
                                            : (this->device_is_high_speed_ ? 4 : 3);
    this->spk_fb_stream_.packets_per_urb = 1;
    this->spk_fb_stream_.num_urbs        = 2;
    this->spk_fb_stream_.interface_num   = this->spk_as_intf_;
    this->spk_fb_stream_.alt_setting     = this->spk_alt_.alt_setting;
    this->spk_fb_stream_.is_output       = false;
    this->spk_fb_stream_.owns_interface  = false;  // shares the speaker OUT interface
    if (this->stream_open(this->spk_fb_stream_, this)) {
      this->spk_fb_open_ = true;
      ESP_LOGI(TAG, "Speaker async feedback stream open (ep=0x%02X)", this->spk_alt_.feedback_ep_addr);
    } else {
      ESP_LOGW(TAG, "Speaker feedback stream open failed; using nominal pacing");
    }
  } else {
    ESP_LOGD(TAG, "Speaker OUT sync type %u, feedback ep 0x%02X (feedback %s)", sync_type,
             this->spk_alt_.feedback_ep_addr, this->feedback_enabled_ ? "enabled" : "disabled");
  }

  // Set sampling frequency on the endpoint.
  this->set_sampling_frequency_(this->spk_alt_.ep_addr, this->spk_cfg_.sample_rate);

  this->spk_stream_open_ = true;

  ESP_LOGI(TAG, "Speaker stream open");
  return true;
}

bool USBAudioClient::open_microphone_stream_() {
  if (this->mic_stream_open_)
    return true;
  if (!this->mic_cfg_.configured || !this->mic_alt_.valid)
    return false;

  if (this->mic_rb_ == nullptr) {
    this->mic_rb_ = xRingbufferCreate(this->mic_buf_size_, RINGBUF_TYPE_BYTEBUF);
    if (this->mic_rb_ == nullptr) {
      this->set_stream_error_("Failed to allocate microphone ring buffer");
      return false;
    }
  }

  this->mic_stream_.ep_addr        = this->mic_alt_.ep_addr;
  this->mic_stream_.mps            = this->mic_alt_.mps;
  this->mic_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->mic_stream_.num_urbs       = ISOC_NUM_URBS;
  this->mic_stream_.interface_num  = this->mic_as_intf_;
  this->mic_stream_.alt_setting    = this->mic_alt_.alt_setting;

  if (!this->stream_open(this->mic_stream_, this)) {
    ESP_LOGE(TAG, "stream_open_ failed for microphone");
    return false;
  }

  this->set_sampling_frequency_(this->mic_alt_.ep_addr, this->mic_cfg_.sample_rate);
  this->mic_stream_open_ = true;
  ESP_LOGI(TAG, "Microphone stream open");
  return true;
}

void USBAudioClient::close_speaker_stream_() {
  if (!this->spk_stream_open_)
    return;
  if (this->spk_fb_open_) {
    this->stream_close(this->spk_fb_stream_);
    this->spk_fb_open_ = false;
  }
  this->stream_close(this->spk_stream_);
  this->spk_stream_open_ = false;
  this->spk_stream_.fb_value.store(0, std::memory_order_relaxed);
  this->flush_speaker_buffer_();
  ESP_LOGI(TAG, "Speaker stream closed");
}

void USBAudioClient::close_microphone_stream_() {
  if (!this->mic_stream_open_)
    return;
  this->stream_close(this->mic_stream_);
  this->mic_stream_open_ = false;
  ESP_LOGI(TAG, "Microphone stream closed");
}

// -----------------------------------------------------------------------------
// Connection lifecycle
// -----------------------------------------------------------------------------

void USBAudioClient::on_connected() {
  const usb_device_desc_t *dev = this->get_device_desc_();
  if (dev != nullptr)
    ESP_LOGI(TAG, "USB Audio device connected: VID=0x%04X PID=0x%04X", dev->idVendor, dev->idProduct);

  // Fresh device: forget everything the previous one taught us.
  this->spk_reopen_at_ms_ = millis();
  this->mic_reopen_at_ms_ = millis();
  this->spk_open_fails_ = 0;
  this->mic_open_fails_ = 0;
  this->spk_format_ok_ = false;
  this->mic_format_ok_ = false;
  this->status_clear_error();

  if (!this->find_ac_interface_()) {
    this->set_stream_error_("Device has no AudioControl interface; not usable as USB audio");
    return;
  }
  this->parse_feature_units_();

  if (this->spk_cfg_.configured) {
    this->spk_format_ok_ = this->parse_as_interface_(true, this->spk_cfg_.channels,
                                                     static_cast<uint8_t>(this->spk_cfg_.bits_per_sample),
                                                     this->spk_cfg_.sample_rate,
                                                     this->spk_as_intf_, this->spk_alt_);
    if (!this->spk_format_ok_) {
      // The device cannot do the configured format. Retrying will not change that, so say so
      // once and leave the status set until a different device shows up.
      this->set_stream_error_("Device does not offer the configured speaker format");
    }
  }

  if (this->mic_cfg_.configured) {
    this->mic_format_ok_ = this->parse_as_interface_(false, this->mic_cfg_.channels,
                                                     static_cast<uint8_t>(this->mic_cfg_.bits_per_sample),
                                                     this->mic_cfg_.sample_rate,
                                                     this->mic_as_intf_, this->mic_alt_);
    if (!this->mic_format_ok_) {
      this->set_stream_error_("Device does not offer the configured microphone format");
    }
  }

  this->device_connected_ = true;

  // Feature Unit requests go to the AudioControl interface, so the saved volume and mute
  // state can be sent as soon as the device is there. Nothing about them depends on an
  // AudioStreaming alt-setting being selected or a stream being open.
  this->probe_speaker_volume_range_();
  this->pending_spk_volume_ = true;
  this->pending_spk_mute_ = true;
  this->apply_speaker_volume_();
  this->apply_speaker_mute_();

  // Open whichever streams are needed immediately.
  if (this->spk_cfg_.configured && this->spk_format_ok_ && !this->spk_suspended_)
    this->note_open_result_(this->open_speaker_stream_(), this->spk_reopen_at_ms_, this->spk_open_fails_);
  if (this->mic_cfg_.configured && this->mic_format_ok_ && !this->mic_suspended_)
    this->note_open_result_(this->open_microphone_stream_(), this->mic_reopen_at_ms_, this->mic_open_fails_);
}

void USBAudioClient::on_disconnected() {
  this->close_speaker_stream_();
  this->close_microphone_stream_();
  this->device_connected_ = false;
  this->spk_format_ok_ = false;
  this->mic_format_ok_ = false;
  this->spk_open_fails_ = 0;
  this->mic_open_fails_ = 0;
  this->spk_volume_fails_ = 0;
  this->spk_mute_fails_   = 0;
  this->spk_volume_supported_ = true;
  this->spk_mute_supported_   = true;
  this->pending_spk_volume_ = true;
  this->pending_spk_mute_   = true;
  // The device is gone, so nothing will ever drain the queued audio. Drop it rather than
  // playing it into the next device that shows up.
  this->flush_speaker_buffer_();
  this->status_clear_error();
  ESP_LOGI(TAG, "USB Audio device disconnected");
}

// -----------------------------------------------------------------------------
// Isochronous packet dispatch (USB-task context)
// -----------------------------------------------------------------------------

void USBAudioClient::on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) {
  // Async OUT feedback endpoint: the device reports its desired rate (samples per service
  // interval, Q10.14 at full speed / Q16.16 at high speed). Feed it into the OUT pacing.
  if (this->spk_fb_open_ && ep_addr == this->spk_alt_.feedback_ep_addr) {
    const size_t need = this->device_is_high_speed_ ? 4u : 3u;
    if (!error && len >= need) {
      uint32_t value = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                       (static_cast<uint32_t>(data[2]) << 16);
      if (this->device_is_high_speed_)
        value |= static_cast<uint32_t>(data[3]) << 24;
      if (value != 0)
        this->spk_stream_.fb_value.store(value, std::memory_order_relaxed);
    }
    return;
  }

  if (error || len == 0)
    return;

  const bool is_in = (ep_addr & 0x80) != 0;

  if (!is_in) {
    // -- Speaker OUT: drain ring buffer into this packet's payload area ------
    // `data` points into the URB's data_buffer. The isoc_cb_ trampoline calls
    // on_isoc_packet() BEFORE resubmitting, so writing here fills the buffer
    // for the next transmission.
    // `len` = actual_num_bytes (bytes sent in the completed transfer), which
    // may be 0 for SKIPPED packets. We always fill a full mps-worth of bytes
    // so that the resubmitted packet carries real data.
    // `len` is the sample-clock packet size the pacing layer chose for this packet; fill
    // exactly that many bytes so the stream matches the negotiated endpoint bandwidth.
    if (this->spk_rb_ == nullptr || this->spk_suspended_) {
      memset(const_cast<uint8_t *>(data), 0, len);
      return;
    }
    const size_t fill_bytes = len;
    uint8_t *dst = const_cast<uint8_t *>(data);
    size_t filled = 0;
    // A byte ring buffer only hands out contiguous memory, so a receive that lands on the
    // wrap returns a short block. Reading once and padding the rest with silence would drop
    // the packet's remaining bytes' worth of playback time on every wrap: the data stays
    // queued, the drain rate falls below real time and the buffer backs up permanently.
    // Read again for the remainder instead.
    while (filled < fill_bytes) {
      size_t received = 0;
      void *item = xRingbufferReceiveUpTo(this->spk_rb_, &received, 0, fill_bytes - filled);
      if (item == nullptr)
        break;
      if (received > 0) {
        memcpy(dst + filled, item, received);
        filled += received;
      }
      vRingbufferReturnItem(this->spk_rb_, item);
      if (received == 0)
        break;
    }
    if (filled < fill_bytes)
      memset(dst + filled, 0, fill_bytes - filled);
  } else {
    // -- Microphone IN: write PCM into the ring buffer ------------------------
    if (this->mic_rb_ == nullptr || this->mic_suspended_)
      return;
    // Non-blocking send; if full, drop oldest data (overwrite) is not possible
    // with RINGBUF_TYPE_BYTEBUF, so we just skip if full to avoid blocking.
    xRingbufferSend(this->mic_rb_, data, len, 0);
  }
}

// -----------------------------------------------------------------------------
// Subcomponent API
// -----------------------------------------------------------------------------

bool USBAudioClient::ensure_started_speaker() {
  if (!this->device_connected_ || !this->spk_format_ok_)
    return false;
  if (this->spk_stream_open_)
    return true;
  if (!this->reopen_due_(this->spk_reopen_at_ms_, this->spk_open_fails_))
    return false;
  const bool ok = this->open_speaker_stream_();
  this->note_open_result_(ok, this->spk_reopen_at_ms_, this->spk_open_fails_);
  return ok;
}

bool USBAudioClient::ensure_started_microphone() {
  if (!this->device_connected_ || !this->mic_format_ok_)
    return false;
  if (this->mic_stream_open_)
    return true;
  if (!this->reopen_due_(this->mic_reopen_at_ms_, this->mic_open_fails_))
    return false;
  const bool ok = this->open_microphone_stream_();
  this->note_open_result_(ok, this->mic_reopen_at_ms_, this->mic_open_fails_);
  return ok;
}

void USBAudioClient::resume_speaker() {
  this->spk_suspended_ = false;
  this->ensure_started_speaker();
}

void USBAudioClient::suspend_speaker() {
  this->spk_suspended_ = true;
}

void USBAudioClient::resume_microphone() {
  this->mic_suspended_ = false;
  this->ensure_started_microphone();
}

void USBAudioClient::suspend_microphone() {
  this->mic_suspended_ = true;
}

void USBAudioClient::set_speaker_volume_level(float volume) {
  this->spk_volume_ = std::clamp(volume, 0.0f, 1.0f);
  // A new value from the user is a fresh attempt, so allow the retries again. Only when the
  // descriptor has no volume control is the answer permanent.
  if (this->speaker_fu_.volume_available()) {
    this->spk_volume_fails_ = 0;
    this->spk_volume_supported_ = true;
  }
  this->pending_spk_volume_ = true;
  this->apply_speaker_volume_();
}

void USBAudioClient::set_speaker_mute_state(bool mute_state) {
  this->spk_muted_ = mute_state;
  if (this->speaker_fu_.mute_available()) {
    this->spk_mute_fails_ = 0;
    this->spk_mute_supported_ = true;
  }
  this->pending_spk_mute_ = true;
  this->apply_speaker_mute_();
}

esp_err_t USBAudioClient::write_speaker(const uint8_t *data, size_t length, uint32_t timeout_ms) {
  if (this->spk_rb_ == nullptr || !this->spk_stream_open_)
    return ESP_ERR_INVALID_STATE;
  TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  BaseType_t ok = xRingbufferSend(this->spk_rb_, data, length, ticks);
  if (ok != pdTRUE)
    return ESP_ERR_TIMEOUT;
  return ESP_OK;
}

esp_err_t USBAudioClient::read_microphone(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms) {
  if (this->mic_rb_ == nullptr || !this->mic_stream_open_) {
    if (bytes_read != nullptr)
      *bytes_read = 0;
    return ESP_ERR_INVALID_STATE;
  }
  TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
  size_t received = 0;
  void *item = xRingbufferReceiveUpTo(this->mic_rb_, &received, ticks, size);
  if (item == nullptr) {
    if (bytes_read != nullptr)
      *bytes_read = 0;
    return ESP_ERR_TIMEOUT;
  }
  memcpy(buffer, item, received);
  vRingbufferReturnItem(this->mic_rb_, item);
  if (bytes_read != nullptr)
    *bytes_read = received;
  return ESP_OK;
}

}  // namespace usb_audio
}  // namespace esphome

#endif  // USE_ESP32_VARIANT_ESP32P4 || USE_ESP32_VARIANT_ESP32S2 || USE_ESP32_VARIANT_ESP32S3
