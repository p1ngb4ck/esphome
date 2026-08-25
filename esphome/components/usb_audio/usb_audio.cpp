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

// Audio devices in the field reject a class request now and then and answer the very same
// request a moment later, so one refusal does not mean the control is not there. The Linux
// USB audio mixer allows ten attempts before it gives up on a request; the same count is
// used here. A request the host never completed is not retried: that is a host or link
// problem and repeating it only spends the timeout again.
static constexpr uint8_t UAC_CTRL_ATTEMPTS = 10;

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

  // Remember which task runs the component loop; a blocking control transfer issued from it
  // has to keep the host library's event handler running, see uac_control_transfer_.
  this->loop_task_ = xTaskGetCurrentTaskHandle();

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

void USBAudioClient::detect_uac_version_() {
  // Assume the older version until the device says otherwise: a device that does not
  // describe itself is far more likely to be a 1.0 one, and the 1.0 layouts are the ones a
  // 2.0 device would never be parsed with by accident.
  this->uac_version_ = UAC_VERSION_1;

  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0);
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 5)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_HEADER)
      continue;

    // Class-specific AC interface header: bcdADC at [3..4]. This is read before the
    // streaming interfaces are picked, so it is the first audio function's header. A device
    // that describes two functions of different class versions is not something the
    // specification provides for.
    const uint16_t bcd = static_cast<uint16_t>(d[3] | (d[4] << 8));
    this->uac_version_ = (bcd >= UAC_VERSION_2) ? UAC_VERSION_2 : UAC_VERSION_1;
    ESP_LOGD(TAG, "Device speaks audio class %u.%u", static_cast<unsigned>(bcd >> 8),
             static_cast<unsigned>((bcd >> 4) & 0x0F));
    return;
  }

  ESP_LOGD(TAG, "No AudioControl header found; assuming audio class 1.0");
}

bool USBAudioClient::find_ac_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr)
    return false;

  // The AudioStreaming interfaces this component is going to use, when they are known.
  uint8_t wanted[2];
  uint8_t wanted_count = 0;
  if (this->spk_cfg_.configured && this->spk_format_ok_)
    wanted[wanted_count++] = this->spk_as_intf_;
  if (this->mic_cfg_.configured && this->mic_format_ok_)
    wanted[wanted_count++] = this->mic_as_intf_;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  bool found_any = false;
  uint8_t current_ac = 0;
  uint8_t first_ac = 0;

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0);
      if (in_ac) {
        current_ac = id->bInterfaceNumber;
        if (!found_any) {
          first_ac = current_ac;
          found_any = true;
        }
      }
      continue;
    }
    if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 8)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_HEADER)
      continue;

    // Class-specific AC interface header: bInCollection at [7], then one interface number
    // per entry. The Collection is what ties an AudioControl interface to the
    // AudioStreaming interfaces of the same audio function, and a device may hold several
    // such functions.
    const uint8_t in_collection = d[7];
    for (uint8_t i = 0; i < in_collection && (8 + i) < desc->bLength; i++) {
      for (uint8_t w = 0; w < wanted_count; w++) {
        if (d[8 + i] != wanted[w])
          continue;
        this->ac_intf_ = current_ac;
        ESP_LOGD(TAG, "AudioControl interface %u owns streaming interface %u", current_ac, wanted[w]);
        return true;
      }
    }
  }

  if (!found_any) {
    ESP_LOGE(TAG, "No AudioControl interface found");
    return false;
  }

  this->ac_intf_ = first_ac;
  if (wanted_count != 0) {
    ESP_LOGW(TAG, "No AudioControl Collection lists streaming interface %u; addressing interface %u", wanted[0],
             first_ac);
  }
  return true;
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

    // Feature Unit layout:
    //   UAC 1.0 (section 4.3.2.5): [3] bUnitID [4] bSourceID [5] bControlSize
    //                              [6..] bmaControls[0] (master), then one per channel,
    //                              bControlSize bytes each, one bit per control.
    //   UAC 2.0 (section 4.7.2.8): [3] bUnitID [4] bSourceID
    //                              [5..] bmaControls[0] (master), then one per channel,
    //                              always four bytes each, two bits per control.
    // Both end with iFeature, which is why the trailing byte is not counted as an entry.
    const bool v2 = (this->uac_version_ >= UAC_VERSION_2);
    UacFeatureUnit fu{};
    fu.unit_id      = d[3];
    fu.source_id    = d[4];
    fu.control_size = v2 ? UAC2_FU_CONTROL_SIZE : d[5];
    const uint8_t controls_off = v2 ? 5 : 6;
    if (fu.control_size == 0 || (controls_off + fu.control_size + 1) > desc->bLength)
      continue;
    fu.control_entries = (desc->bLength - controls_off - 1) / fu.control_size;
    if (fu.control_entries == 0)
      continue;

    // What one bmaControls entry says about mute and volume, in whichever encoding the
    // class version uses. A UAC 2.0 control that can only be read is of no use here, so it
    // counts as not being there.
    auto entry_controls = [&](uint8_t entry, bool &mute, bool &volume) {
      const uint16_t off = controls_off + static_cast<uint16_t>(entry) * fu.control_size;
      if (off + fu.control_size > desc->bLength) {
        mute = volume = false;
        return;
      }
      if (!v2) {
        mute = (d[off] & UAC_FU_CTL_MUTE) != 0;
        volume = (d[off] & UAC_FU_CTL_VOLUME) != 0;
        return;
      }
      const uint32_t bm = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
                          (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
      mute = (bm & UAC2_FU_CTL_MUTE_MASK) == UAC2_FU_CTL_MUTE_RW;
      volume = (bm & UAC2_FU_CTL_VOLUME_MASK) == UAC2_FU_CTL_VOLUME_RW;
    };

    entry_controls(0, fu.has_mute, fu.has_volume);
    fu.master_controls = d[controls_off];

    // Not every device puts its controls on the master entry; some describe them per
    // channel only. Record those so a control that exists can still be reached.
    for (uint8_t entry = 1; entry < fu.control_entries && entry <= UAC_FU_MAX_CHANNELS; entry++) {
      bool mute = false;
      bool volume = false;
      entry_controls(entry, mute, volume);
      if (mute)
        fu.mute_channels |= static_cast<uint8_t>(1 << (entry - 1));
      if (volume)
        fu.volume_channels |= static_cast<uint8_t>(1 << (entry - 1));
    }

    units[count++] = fu;
  }
  return count;
}

bool USBAudioClient::parse_feature_units_() {
  // Another device may have been attached before, so start from nothing.
  this->spk_ctl_.fu = {};
  this->mic_ctl_.fu = {};
  this->spk_ctl_.clock_id = 0;
  this->mic_ctl_.clock_id = 0;

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
      this->spk_ctl_.fu = units[i];
    if (this->mic_cfg_.configured && units[i].unit_id == mic_fu_id)
      this->mic_ctl_.fu = units[i];
  }

  // Unit ID 0 is reserved by the specification, so it marks "nothing found" here.
  if (this->spk_cfg_.configured) {
    if (this->spk_ctl_.fu.unit_id != 0) {
      ESP_LOGD(TAG, "Speaker Feature Unit: id=%u mute=%s vol=%s", this->spk_ctl_.fu.unit_id,
               YESNO(this->spk_ctl_.fu.mute_available()), YESNO(this->spk_ctl_.fu.volume_available()));
    } else {
      ESP_LOGW(TAG, "No Feature Unit in the playback path; volume and mute are unavailable");
    }
  }
  if (this->mic_cfg_.configured) {
    if (this->mic_ctl_.fu.unit_id != 0) {
      ESP_LOGD(TAG, "Mic Feature Unit: id=%u mute=%s vol=%s", this->mic_ctl_.fu.unit_id,
               YESNO(this->mic_ctl_.fu.mute_available()), YESNO(this->mic_ctl_.fu.volume_available()));
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

  // The two class versions describe a streaming format in different descriptors, so which
  // one the device speaks has to be settled before its descriptors are read.
  const bool uac2 = (this->uac_version_ >= UAC_VERSION_2);

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
    // Check sample rate. An alt-setting whose rate is a property of a Clock Source has
    // nothing in its descriptors to check against; the clock is asked when the stream
    // opens.
    bool freq_ok = cur_alt_info.freq_from_clock;
    if (freq_ok) {
      // nothing to check here
    } else if (cur_alt_info.sample_freq_type == 0) {
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

      if (sub == UAC_AS_GENERAL && desc->bLength >= 4) {
        // Class-specific AS interface descriptor. Both versions start with bTerminalLink,
        // which is the Terminal of the AudioControl interface this stream belongs to and
        // therefore the way to the Clock Source in UAC 2.0.
        cur_alt_info.terminal_link = d[3];
        // UAC 2.0 (section 4.9.2) moved the channel count here:
        // [3] bTerminalLink [4] bmControls [5] bFormatType [6..9] bmFormats
        // [10] bNrChannels [11..14] bmChannelConfig [15] iChannelNames
        if (uac2 && desc->bLength >= 11) {
          cur_alt_info.channels = d[10];
          format_seen = true;
        }
        continue;
      }

      if (sub == UAC_AS_FORMAT_TYPE && uac2 && desc->bLength >= 6) {
        // Type I Format Type descriptor (UAC 2.0 table 2-2). The sample frequencies are
        // not here: in this version the Clock Source entity answers for them.
        // [3] bFormatType [4] bSubslotSize [5] bBitResolution
        if (d[3] != 0x01)  // only Type I
          continue;
        cur_alt_info.sub_frame_size = d[4];
        cur_alt_info.bit_resolution = d[5];
        cur_alt_info.freq_from_clock = true;
        continue;
      }

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
  uint16_t status{0};  // usb_transfer_status_t as the host reported it
  uint8_t data[UAC_CTRL_MAX_DATA_LEN]{};
  size_t data_len{0};
};

// What the host said about a finished transfer. A request the device rejected, one that
// never reached it and one that was cut short are three different answers, and which one
// came back decides where to look next.
const char *uac_transfer_status_str(uint16_t status) {
  switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
      return "completed";
    case USB_TRANSFER_STATUS_ERROR:
      return "bus error";
    case USB_TRANSFER_STATUS_TIMED_OUT:
      return "timed out";
    case USB_TRANSFER_STATUS_CANCELED:
      return "canceled";
    case USB_TRANSFER_STATUS_STALL:
      return "stalled by device";
    case USB_TRANSFER_STATUS_OVERFLOW:
      return "overflow";
    case USB_TRANSFER_STATUS_SKIPPED:
      return "skipped";
    case USB_TRANSFER_STATUS_NO_DEVICE:
      return "no device";
    default:
      return "unknown";
  }
}
}  // namespace

bool USBAudioClient::uac_control_transfer_(uint8_t req_type, uint8_t request, uint16_t value, uint16_t index,
                                            const uint8_t *out_data, size_t out_len, uint8_t *in_data,
                                            size_t in_len) {
  if (out_len > UAC_CTRL_MAX_DATA_LEN || in_len > UAC_CTRL_MAX_DATA_LEN) {
    ESP_LOGE(TAG, "Control request 0x%02X wants more than %u bytes of data", request,
             static_cast<unsigned>(UAC_CTRL_MAX_DATA_LEN));
    return false;
  }

  // wLength is the length the class definition gives the request: two bytes for a volume,
  // one for a mute, three for a sampling frequency. It is part of what identifies the
  // request to the device, so it is sent as defined and never rounded.
  //
  // The buffer behind it is a separate matter. The host controller sizes an IN data stage
  // from the buffer rather than from wLength, and it needs whole endpoint 0 packets, so a
  // read rounds its buffer up to one. Anything the device does not send simply stays
  // untouched in it.
  size_t buffer_len = out_len;
  const size_t w_length = (in_data != nullptr) ? in_len : out_len;
  if (in_data != nullptr) {
    const usb_device_desc_t *dev_desc = this->get_device_desc_();
    if (dev_desc == nullptr || dev_desc->bMaxPacketSize0 == 0) {
      ESP_LOGW(TAG, "Control request 0x%02X skipped: endpoint 0 packet size is unknown", request);
      return false;
    }
    const size_t mps0 = dev_desc->bMaxPacketSize0;
    buffer_len = ((in_len + mps0 - 1) / mps0) * mps0;
  }

  // A control transfer only makes progress while usb_host_lib_handle_events() is being
  // called, and the only caller of it is USBHost::loop(), which runs on the component loop
  // task. Waiting on that task without driving it means waiting for something this wait is
  // itself preventing, which is why these requests never completed while a request issued
  // from the stream task did. Drive it here for as long as the wait lasts. Requests from
  // other tasks must not touch it, since it belongs to the loop task.
  const bool on_loop_task = (this->loop_task_ != nullptr && xTaskGetCurrentTaskHandle() == this->loop_task_);

  uint16_t last_status = USB_TRANSFER_STATUS_COMPLETED;
  size_t last_len = 0;
  bool short_answer = false;
  for (uint8_t attempt = 0; attempt < UAC_CTRL_ATTEMPTS; attempt++) {
    auto result = std::make_shared<UacControlResult>();
    std::vector<uint8_t> payload(buffer_len);
    if (out_data != nullptr && out_len != 0)
      memcpy(payload.data(), out_data, out_len);

    if (!this->control_transfer(req_type, request, value, index,
                                [result](const usb_host::TransferStatus &s) {
                                  result->success = s.success;
                                  result->status = s.error_code;
                                  if (s.success && s.data != nullptr) {
                                    result->data_len = std::min<size_t>(s.data_len, UAC_CTRL_MAX_DATA_LEN);
                                    memcpy(result->data, s.data, result->data_len);
                                  }
                                  result->done.store(true, std::memory_order_release);
                                },
                                payload, static_cast<int32_t>(w_length))) {
      // Nothing was handed to the host, so no callback is coming. Waiting for one would
      // spend the full timeout and then report a device that did not answer, which is not
      // what happened.
      ESP_LOGW(TAG, "Control request 0x%02X was not submitted", request);
      return false;
    }

    const uint32_t started = millis();
    while (!result->done.load(std::memory_order_acquire) && (millis() - started) < CTRL_TIMEOUT_MS) {
      if (on_loop_task) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(0, &event_flags);
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!result->done.load(std::memory_order_acquire)) {
      // Submitted, but the host never reported it finishing either way. That is not the
      // device turning the request down, so repeating it would only spend the timeout
      // again.
      ESP_LOGW(TAG, "Control request 0x%02X to %s 0x%04X: no completion within %" PRIu32 " ms",
               request, (req_type & usb_host::USB_DIR_IN) != 0 ? "get" : "set", index, CTRL_TIMEOUT_MS);
      return false;
    }
    if (result->success) {
      if (in_data == nullptr)
        return true;
      if (result->data_len >= in_len) {
        memcpy(in_data, result->data, in_len);
        return true;
      }
      // A short answer is the device declining to give what the request asked for; it can
      // answer the next attempt in full.
      short_answer = true;
      last_len = result->data_len;
      continue;
    }
    short_answer = false;
    last_status = result->status;
  }

  if (short_answer) {
    ESP_LOGW(TAG, "Control request 0x%02X to 0x%04X after %u attempts: returned %u of %u bytes", request, index,
             static_cast<unsigned>(UAC_CTRL_ATTEMPTS), static_cast<unsigned>(last_len),
             static_cast<unsigned>(in_len));
  } else {
    ESP_LOGW(TAG, "Control request 0x%02X to 0x%04X after %u attempts: %s", request, index,
             static_cast<unsigned>(UAC_CTRL_ATTEMPTS), uac_transfer_status_str(last_status));
  }
  return false;
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
  // UAC 1.0 has one request code per attribute, so reading the current value has its own.
  // UAC 2.0 has one code for the value in either direction, and bmRequestType says which.
  const uint8_t request = (this->uac_version_ >= UAC_VERSION_2) ? UAC2_CS_CUR : UAC_GET_CUR;
  return this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, request, wValue, wIndex, nullptr, 0, data, len);
}

bool USBAudioClient::uac_set_cur_endpoint_(uint8_t ep_addr, uint8_t selector,
                                            const uint8_t *data, size_t len) {
  const uint16_t wValue = static_cast<uint16_t>(selector << 8);
  return this->uac_control_transfer_(UAC_REQ_TYPE_EP_SET, UAC_SET_CUR, wValue, ep_addr, data, len, nullptr, 0);
}

uint8_t USBAudioClient::find_clock_source_(uint8_t terminal_id) {
  const usb_config_desc_t *cfg = this->get_config_desc_();
  if (cfg == nullptr || terminal_id == 0)
    return 0;

  // Two passes over the AudioControl interface. The first asks the Terminal which clock it
  // is fed from, the second follows a Clock Selector to the entity behind it, because a
  // Selector is not something a frequency can be set on.
  auto scan = [&](uint8_t want_id, bool follow_selector) -> uint8_t {
    uint16_t total = cfg->wTotalLength;
    int offset = 0;
    const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
    bool in_ac = false;
    while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
      if (desc->bDescriptorType == USB_W_VALUE_DT_INTERFACE) {
        const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
        in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
                 id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
        continue;
      }
      if (!in_ac || desc->bDescriptorType != UAC_CS_INTERFACE || desc->bLength < 4)
        continue;
      const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
      if (d[3] != want_id)
        continue;
      const uint8_t subtype = d[2];
      if (!follow_selector) {
        // UAC 2.0 Terminal descriptors: bCSourceID sits after bAssocTerminal on an Input
        // Terminal and after bSourceID as well on an Output Terminal.
        if (subtype == UAC_AC_INPUT_TERMINAL && desc->bLength > 7)
          return d[7];
        if (subtype == UAC_AC_OUTPUT_TERMINAL && desc->bLength > 8)
          return d[8];
        return 0;
      }
      if (subtype == UAC2_AC_CLOCK_SOURCE)
        return want_id;
      // Clock Selector: baCSourceID[0] follows bNrInPins.
      if (subtype == UAC2_AC_CLOCK_SELECTOR && desc->bLength > 5)
        return d[5];
      // Clock Multiplier: bCSourceID follows bClockID.
      if (subtype == UAC2_AC_CLOCK_MULTIPLIER && desc->bLength > 4)
        return d[4];
      return 0;
    }
    return 0;
  };

  const uint8_t source = scan(terminal_id, false);
  if (source == 0)
    return 0;
  const uint8_t clock = scan(source, true);
  return (clock != 0) ? clock : source;
}

bool USBAudioClient::set_sample_rate_(const UacAltInfo &alt, const UacControlState &ctl, uint32_t freq) {
  if (this->uac_version_ < UAC_VERSION_2) {
    // UAC 1.0: a three byte control on the isochronous endpoint itself. A device is allowed
    // not to have it when it runs at one fixed rate, so a refusal is not fatal.
    uint8_t buf[3] = {
        static_cast<uint8_t>(freq & 0xFF),
        static_cast<uint8_t>((freq >> 8) & 0xFF),
        static_cast<uint8_t>((freq >> 16) & 0xFF),
    };
    const bool ok = this->uac_set_cur_endpoint_(alt.ep_addr, UAC_EP_SAMPLING_FREQ_CONTROL, buf, sizeof(buf));
    if (!ok) {
      ESP_LOGW(TAG, "SET_CUR sampling freq ep=0x%02X freq=%" PRIu32 " failed (may not be supported)", alt.ep_addr,
               freq);
    }
    return ok;
  }

  // UAC 2.0: a four byte control on the Clock Source entity that drives the terminal the
  // stream is attached to. Nothing about it is addressed through the endpoint any more.
  if (ctl.clock_id == 0) {
    ESP_LOGW(TAG, "No Clock Source behind terminal %u; leaving the rate as the device has it", alt.terminal_link);
    return false;
  }
  uint8_t buf[4] = {
      static_cast<uint8_t>(freq & 0xFF),
      static_cast<uint8_t>((freq >> 8) & 0xFF),
      static_cast<uint8_t>((freq >> 16) & 0xFF),
      static_cast<uint8_t>((freq >> 24) & 0xFF),
  };
  const uint16_t wValue = static_cast<uint16_t>(UAC2_CS_SAM_FREQ_CONTROL << 8);
  const uint16_t wIndex = static_cast<uint16_t>((ctl.clock_id << 8) | this->ac_intf_);
  if (!this->uac_control_transfer_(UAC_REQ_TYPE_INTF_SET, UAC2_CS_CUR, wValue, wIndex, buf, sizeof(buf), nullptr,
                                   0)) {
    ESP_LOGW(TAG, "SET_CUR sample rate on clock %u to %" PRIu32 " Hz failed", ctl.clock_id, freq);
    return false;
  }

  // The clock is what the stream is then timed by, so what it ended up at decides whether
  // the audio comes out at the right pitch. Saying what we asked for would say nothing.
  uint8_t rb[4] = {0, 0, 0, 0};
  if (this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, UAC2_CS_CUR, wValue, wIndex, nullptr, 0, rb, sizeof(rb))) {
    const uint32_t actual = static_cast<uint32_t>(rb[0]) | (static_cast<uint32_t>(rb[1]) << 8) |
                            (static_cast<uint32_t>(rb[2]) << 16) | (static_cast<uint32_t>(rb[3]) << 24);
    if (actual != freq) {
      ESP_LOGW(TAG, "Clock %u is at %" PRIu32 " Hz, not the requested %" PRIu32 " Hz", ctl.clock_id, actual, freq);
    }
  }
  return true;
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

void USBAudioClient::probe_volume_range_(UacControlState &ctl, const char *what) {
  ctl.range_known = false;
  if (ctl.fu.unit_id == 0 || !ctl.fu.volume_available())
    return;

  // Ask on the same channel the value will be written to.
  uint8_t vol_channels[UAC_FU_MAX_CHANNELS];
  const uint8_t vol_count = uac_control_channels(ctl.fu.has_volume, ctl.fu.volume_channels, vol_channels);
  const uint8_t vol_channel = (vol_count > 0) ? vol_channels[0] : UAC_FU_MASTER_CHANNEL;
  const uint16_t wValue = static_cast<uint16_t>((UAC_FU_VOLUME_CONTROL << 8) | vol_channel);
  const uint16_t wIndex = static_cast<uint16_t>((ctl.fu.unit_id << 8) | this->ac_intf_);

  int16_t vol_min = 0;
  int16_t vol_max = 0;
  int16_t vol_res = 0;
  bool range_ok = false;

  if (this->uac_version_ >= UAC_VERSION_2) {
    // One RANGE request answers with the number of sub-ranges followed by a (MIN, MAX, RES)
    // triplet for each, two bytes per value for a volume. A control split into several
    // sub-ranges has gaps in it that cannot be expressed as one scale, so only the first is
    // taken and the rest is left to the device to clamp.
    uint8_t buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, UAC2_CS_RANGE, wValue, wIndex, nullptr, 0, buf,
                                    sizeof(buf))) {
      const uint16_t subranges = static_cast<uint16_t>(buf[0] | (buf[1] << 8));
      if (subranges >= 1) {
        vol_min = static_cast<int16_t>(buf[2] | (buf[3] << 8));
        vol_max = static_cast<int16_t>(buf[4] | (buf[5] << 8));
        vol_res = static_cast<int16_t>(buf[6] | (buf[7] << 8));
        range_ok = true;
        if (subranges > 1) {
          ESP_LOGD(TAG, "%s volume has %u sub-ranges; using the first", what,
                   static_cast<unsigned>(subranges));
        }
      }
    }
  } else {
    auto get_vol_range = [this, wValue, wIndex](uint8_t bRequest, int16_t &out) {
      uint8_t buf[2] = {0, 0};
      if (!this->uac_control_transfer_(UAC_REQ_TYPE_INTF_GET, bRequest, wValue, wIndex, nullptr, 0, buf,
                                       sizeof(buf)))
        return false;
      ESP_LOGD(TAG, "%s RANGE req=0x%02X raw=%02X %02X", what, bRequest, buf[0], buf[1]);
      out = static_cast<int16_t>(buf[0] | (buf[1] << 8));
      return true;
    };
    // MAX before MIN, so a device that only answers the first request leaves the more
    // informative of the two behind.
    const bool max_ok = get_vol_range(UAC_GET_MAX, vol_max);
    const bool min_ok = get_vol_range(UAC_GET_MIN, vol_min);
    range_ok = max_ok && min_ok;
    // The resolution says which settings between the endpoints the device can actually
    // take. A device that does not answer is treated as taking every step.
    if (range_ok && !get_vol_range(UAC_GET_RES, vol_res))
      vol_res = 0;
  }

  if (!range_ok) {
    ESP_LOGW(TAG, "%s reported no volume range; falling back to the absolute dB scale", what);
    return;
  }
  // 0x8000 is the reserved "silence" code and never a range endpoint, and a range that is
  // not strictly increasing means the answer is not a scale.
  if (vol_min == UAC_VOLUME_SILENCE || vol_max <= vol_min) {
    ESP_LOGW(TAG, "%s reported an unusable volume range (%d..%d); falling back to the absolute dB scale", what,
             static_cast<int>(vol_min), static_cast<int>(vol_max));
    return;
  }
  // A scale whose loudest setting is still that far down is not a scale a device is
  // playing on; it is a stand-in a device returns when it has nothing to report. Mapping
  // onto it would leave every setting inaudible.
  if (vol_max <= UAC_VOLUME_BOGUS_MAX) {
    ESP_LOGW(TAG, "%s reported a volume range topping out at %.2f dB; treating it as bogus", what,
             static_cast<float>(vol_max) / 256.0f);
    return;
  }
  // A step that is not positive, or one no smaller than the whole scale, describes nothing
  // that can be landed on. Take every step instead.
  if (vol_res <= 0 || vol_res >= static_cast<int32_t>(vol_max) - vol_min)
    vol_res = 1;

  ctl.vol_min = vol_min;
  ctl.vol_max = vol_max;
  ctl.vol_res = vol_res;
  ctl.range_known = true;
  ESP_LOGD(TAG, "%s volume range: %.2f dB to %.2f dB in steps of %.2f dB", what,
           static_cast<float>(ctl.vol_min) / 256.0f, static_cast<float>(ctl.vol_max) / 256.0f,
           static_cast<float>(ctl.vol_res) / 256.0f);
}

bool USBAudioClient::apply_volume_(UacControlState &ctl, const char *what) {
  uint8_t channels[UAC_FU_MAX_CHANNELS];
  const uint8_t channel_count = uac_control_channels(ctl.fu.has_volume, ctl.fu.volume_channels, channels);
  if (ctl.fu.unit_id == 0 || channel_count == 0) {
    ESP_LOGD(TAG, "%s volume %.0f%% ignored: device has no volume control", what, ctl.volume * 100.0f);
    return false;
  }

  // A Feature Unit volume is a dB scale, and MIN, MAX and RES are what the device says
  // about that scale. How a 0..1 setting is placed on it is the configured curve: as a
  // position on the scale, or as an amplitude converted onto it.
  //
  // The reserved 0x8000 silence code is not sent for any of this. It is not part of the
  // scale, a device is free to treat it as an out of range value, and the bottom of the
  // range is the quietest setting the device itself offers anyway.
  const float clamped = std::clamp(ctl.volume, 0.0f, 1.0f);
  int16_t vol;
  if (ctl.range_known) {
    int32_t raw;
    if (this->volume_curve_ == VolumeCurve::LOGARITHMIC && clamped > 0.0f) {
      // Full scale is the top of what the device offers, and each halving of the amplitude
      // is 6.02 dB below that.
      raw = static_cast<int32_t>(ctl.vol_max) + std::lround(20.0 * std::log10(static_cast<double>(clamped)) * 256.0);
    } else if (this->volume_curve_ == VolumeCurve::LOGARITHMIC) {
      raw = ctl.vol_min;
    } else {
      const int32_t span = static_cast<int32_t>(ctl.vol_max) - ctl.vol_min;
      raw = ctl.vol_min + std::lround(static_cast<double>(span) * clamped);
    }
    raw = std::clamp<int32_t>(raw, ctl.vol_min, ctl.vol_max);
    // Settings between two steps are not addressable, so land on the nearest one rather
    // than always on the one below.
    const int32_t res = ctl.vol_res;
    const int32_t stepped = ctl.vol_min + std::lround(static_cast<double>(raw - ctl.vol_min) / res) * res;
    vol = static_cast<int16_t>(std::clamp<int32_t>(stepped, ctl.vol_min, ctl.vol_max));
  } else if (clamped <= 0.0f) {
    vol = UAC_VOLUME_MIN_GAIN;
  } else {
    // With no endpoints from the device the absolute definition is all that is left: the
    // value is gain in dB, so full volume is unity gain and each halving of the amplitude
    // is 6.02 dB below it. There is no scale to place a position on, so the curve setting
    // has nothing to choose between here.
    const int32_t raw = std::lround(20.0 * std::log10(static_cast<double>(clamped)) * 256.0);
    vol = static_cast<int16_t>(std::clamp<int32_t>(raw, UAC_VOLUME_MIN_GAIN, 0));
  }

  uint8_t buf[2] = {static_cast<uint8_t>(vol & 0xFF), static_cast<uint8_t>((vol >> 8) & 0xFF)};
  ESP_LOGD(TAG, "%s SET volume %.0f%%: vol=%d bytes=%02X %02X",
         what, clamped * 100.0f, static_cast<int>(vol), buf[0], buf[1]);
  bool ok = true;
  for (uint8_t i = 0; i < channel_count; i++) {
    if (!this->uac_set_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channels[i], buf, sizeof(buf)))
      ok = false;
  }
  const float sent_db = static_cast<float>(vol) / 256.0f;
  if (!ok) {
    ESP_LOGW(TAG, "%s volume %.0f%% (%.2f dB) was not accepted by the device", what, clamped * 100.0f, sent_db);
    return false;
  }

  // Read the value back. A device may clamp it to its own range or quantise it to a
  // coarser step, so reporting what we sent would be reporting an assumption.
  uint8_t rb[2] = {0, 0};
  if (this->uac_get_cur_interface_(ctl.fu.unit_id, UAC_FU_VOLUME_CONTROL, channels[0], rb, sizeof(rb))) {
    ESP_LOGD(TAG,
           "%s volume GET RAW: [%02X %02X] LE=0x%04X",
           what,
           rb[0],
           rb[1],
           static_cast<unsigned>(rb[0] | (rb[1] << 8)));
    const int16_t actual = static_cast<int16_t>(rb[0] | (rb[1] << 8));
    const float actual_db = static_cast<float>(actual) / 256.0f;
    ESP_LOGD(TAG, "%s volume %.0f%% -> %.2f dB (device reports %.2f dB)", what, clamped * 100.0f, sent_db,
             actual_db);
    // A device may only be settable in steps of its reported resolution, so landing within
    // one of them is it rounding. Anything past that is the device overriding us.
    const float tolerance = ctl.range_known ? (static_cast<float>(ctl.vol_res) / 256.0f) : 0.25f;
    if (std::fabs(actual_db - sent_db) > tolerance) {
      ESP_LOGW(TAG, "%s volume not applied as requested: asked %.2f dB, device is at %.2f dB", what, sent_db,
               actual_db);
    }
  } else {
    ESP_LOGD(TAG, "%s volume %.0f%% -> %.2f dB (readback unavailable)", what, clamped * 100.0f, sent_db);
  }
  return true;
}

bool USBAudioClient::apply_mute_(UacControlState &ctl, const char *what) {
  uint8_t channels[UAC_FU_MAX_CHANNELS];
  const uint8_t channel_count = uac_control_channels(ctl.fu.has_mute, ctl.fu.mute_channels, channels);
  if (ctl.fu.unit_id == 0 || channel_count == 0) {
    ESP_LOGD(TAG, "%s mute %s ignored: device has no mute control", what, ONOFF(ctl.muted));
    return false;
  }

  uint8_t val = ctl.muted ? 1 : 0;
  bool ok = true;
  for (uint8_t i = 0; i < channel_count; i++) {
    if (!this->uac_set_cur_interface_(ctl.fu.unit_id, UAC_FU_MUTE_CONTROL, channels[i], &val, 1))
      ok = false;
  }
  if (!ok) {
    ESP_LOGW(TAG, "%s mute %s was not accepted by the device", what, ONOFF(ctl.muted));
    return false;
  }
  ESP_LOGD(TAG, "%s mute %s", what, ONOFF(ctl.muted));
  return true;
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
  this->set_sample_rate_(this->spk_alt_, this->spk_ctl_, this->spk_cfg_.sample_rate);

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

  this->set_sample_rate_(this->mic_alt_, this->mic_ctl_, this->mic_cfg_.sample_rate);
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

  // Which version of the class the device speaks decides how its descriptors are laid out,
  // so it is settled before any of them is read.
  this->detect_uac_version_();

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

  // Which AudioControl interface to address depends on which streaming interfaces are in
  // use, so this has to come after they have been picked.
  if (!this->find_ac_interface_()) {
    this->set_stream_error_("Device has no AudioControl interface; not usable as USB audio");
    return;
  }
  this->parse_feature_units_();

  // In UAC 2.0 the sample rate is a property of a Clock Source entity rather than of the
  // endpoint, so the clock behind each stream's terminal is looked up once here.
  if (this->uac_version_ >= UAC_VERSION_2) {
    if (this->spk_format_ok_)
      this->spk_ctl_.clock_id = this->find_clock_source_(this->spk_alt_.terminal_link);
    if (this->mic_format_ok_)
      this->mic_ctl_.clock_id = this->find_clock_source_(this->mic_alt_.terminal_link);
    ESP_LOGD(TAG, "Clock sources: speaker=%u mic=%u", this->spk_ctl_.clock_id, this->mic_ctl_.clock_id);
  }

  this->device_connected_ = true;

  // Feature Unit requests go to the AudioControl interface, so the saved volume and mute
  // state can be sent as soon as the device is there. Nothing about them depends on an
  // AudioStreaming alt-setting being selected or a stream being open.
  this->probe_volume_range_(this->spk_ctl_, "Speaker");
  this->apply_volume_(this->spk_ctl_, "Speaker");
  this->apply_mute_(this->spk_ctl_, "Speaker");
  this->spk_ctl_.volume_sent = true;
  this->spk_ctl_.mute_sent = true;
  this->probe_volume_range_(this->mic_ctl_, "Mic");
  this->apply_volume_(this->mic_ctl_, "Mic");
  this->apply_mute_(this->mic_ctl_, "Mic");
  this->mic_ctl_.volume_sent = true;
  this->mic_ctl_.mute_sent = true;

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
  // The next device has to be told the state from scratch.
  this->spk_ctl_.volume_sent = false;
  this->spk_ctl_.mute_sent = false;
  this->spk_ctl_.range_known = false;
  this->mic_ctl_.volume_sent = false;
  this->mic_ctl_.mute_sent = false;
  this->mic_ctl_.range_known = false;
  this->spk_format_ok_ = false;
  this->mic_format_ok_ = false;
  this->spk_open_fails_ = 0;
  this->mic_open_fails_ = 0;
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

// Take a volume or mute request for one direction. A request that cannot change what the
// device was last told is dropped: a media player drives several speakers that all end up
// here, so the same setting arrives more than once, and asking the device again for what
// it was just asked is a control request that cannot change anything.
void USBAudioClient::set_volume_level_(UacControlState &ctl, const char *what, float volume) {
  const float clamped = std::clamp(volume, 0.0f, 1.0f);
  if (ctl.volume_sent && clamped == ctl.volume)
    return;
  ctl.volume = clamped;
  if (!this->device_connected_)
    return;
  this->apply_volume_(ctl, what);
  ctl.volume_sent = true;
}

void USBAudioClient::set_mute_state_(UacControlState &ctl, const char *what, bool mute_state) {
  if (ctl.mute_sent && mute_state == ctl.muted)
    return;
  ctl.muted = mute_state;
  if (!this->device_connected_)
    return;
  this->apply_mute_(ctl, what);
  ctl.mute_sent = true;
}

void USBAudioClient::set_speaker_volume_level(float volume) {
  this->set_volume_level_(this->spk_ctl_, "Speaker", volume);
}

void USBAudioClient::set_speaker_mute_state(bool mute_state) {
  this->set_mute_state_(this->spk_ctl_, "Speaker", mute_state);
}

void USBAudioClient::set_microphone_volume_level(float volume) {
  this->set_volume_level_(this->mic_ctl_, "Mic", volume);
}

void USBAudioClient::set_microphone_mute_state(bool mute_state) {
  this->set_mute_state_(this->mic_ctl_, "Mic", mute_state);
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
