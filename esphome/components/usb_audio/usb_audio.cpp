#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_audio.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "usb/usb_helpers.h"

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace esphome {
namespace usb_audio {

static const char *const TAG = "usb_audio";

// Timeout for each blocking control transfer during connection setup.
static constexpr uint32_t CTRL_TIMEOUT_MS = 1000;

// URBs per stream (triple-buffering).
static constexpr uint8_t ISOC_NUM_URBS = 3;

// Packets per URB — small for audio to minimise latency.
static constexpr uint8_t ISOC_PACKETS_PER_URB = 4;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration setters
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// Component lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void USBAudioClient::setup() {
  if (this->spk_buf_size_ == 0)
    this->spk_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;
  if (this->mic_buf_size_ == 0)
    this->mic_buf_size_ = USB_AUDIO_DEFAULT_BUFFER_SIZE;

  usb_host::USBClient::setup();
}

void USBAudioClient::loop() {
  this->process_usb_events_();

  if (this->spk_stream_open_ && !this->spk_suspended_) {
    if (this->pending_spk_volume_)
      this->apply_speaker_volume_();
    if (this->pending_spk_mute_)
      this->apply_speaker_mute_();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Descriptor parsing
// ─────────────────────────────────────────────────────────────────────────────

bool USBAudioClient::find_ac_interface_() {
  const usb_config_desc_t *cfg = this->get_config_desc();
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

bool USBAudioClient::parse_feature_units_() {
  const usb_config_desc_t *cfg = this->get_config_desc();
  if (cfg == nullptr)
    return false;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);
  bool in_ac = false;
  bool spk_found = false;
  bool mic_found = false;

  // Simple heuristic: the first Feature Unit after the AC interface is the speaker FU;
  // the second (if present) is the microphone FU. Most single-chip UAC devices follow this.
  // More robust: we'd walk the terminal chain, but that requires tracking terminal IDs —
  // this simpler approach works for the vast majority of USB headsets/speakers.

  while ((desc = usb_parse_next_descriptor(desc, total, &offset)) != nullptr) {
    const uint8_t dtype = desc->bDescriptorType;

    if (dtype == USB_W_VALUE_DT_INTERFACE) {
      const auto *id = reinterpret_cast<const usb_intf_desc_t *>(desc);
      in_ac = (id->bInterfaceClass == USB_CLASS_AUDIO && id->bInterfaceSubClass == UAC_SC_AUDIOCONTROL &&
               id->bAlternateSetting == 0 && id->bInterfaceNumber == this->ac_intf_);
      continue;
    }

    if (!in_ac || dtype != UAC_CS_INTERFACE)
      continue;
    if (desc->bLength < 3)
      continue;

    const uint8_t *d = reinterpret_cast<const uint8_t *>(desc);
    if (d[2] != UAC_AC_FEATURE_UNIT)
      continue;

    // Feature Unit layout (UAC 1.0 table 4-7):
    // [0] bLength  [1] bDescriptorType  [2] bDescriptorSubtype
    // [3] bUnitID  [4] bSourceID  [5] bControlSize
    // [6..6+bControlSize-1] bmaControls[0] (master)
    // (followed by per-channel controls but we only need master)
    if (desc->bLength < 7)
      continue;

    UacFeatureUnit fu{};
    fu.unit_id      = d[3];
    fu.source_id    = d[4];
    fu.control_size = d[5];
    if (fu.control_size == 0 || (7 + fu.control_size) > desc->bLength)
      continue;
    fu.master_controls = d[6];
    fu.has_mute   = (fu.master_controls & UAC_FU_CTL_MUTE) != 0;
    fu.has_volume = (fu.master_controls & UAC_FU_CTL_VOLUME) != 0;
    // channel_count inferred from descriptor length
    fu.channel_count = (desc->bLength - 7) / fu.control_size;

    if (this->spk_cfg_.configured && !spk_found) {
      this->speaker_fu_ = fu;
      spk_found = true;
      ESP_LOGD(TAG, "Speaker Feature Unit: id=%u mute=%s vol=%s", fu.unit_id, YESNO(fu.has_mute),
               YESNO(fu.has_volume));
    } else if (this->mic_cfg_.configured && !mic_found) {
      this->mic_fu_ = fu;
      mic_found = true;
      ESP_LOGD(TAG, "Mic Feature Unit: id=%u mute=%s vol=%s", fu.unit_id, YESNO(fu.has_mute), YESNO(fu.has_volume));
    }

    if ((!this->spk_cfg_.configured || spk_found) && (!this->mic_cfg_.configured || mic_found))
      break;
  }

  return true;  // non-fatal if not found — volume/mute will be silently skipped
}

bool USBAudioClient::parse_as_interface_(bool want_out, uint8_t channels, uint8_t bits, uint32_t sample_rate,
                                          uint8_t &intf_out, UacAltInfo &alt_out) {
  const usb_config_desc_t *cfg = this->get_config_desc();
  if (cfg == nullptr)
    return false;

  uint16_t total = cfg->wTotalLength;
  int offset = 0;
  const usb_standard_desc_t *desc = reinterpret_cast<const usb_standard_desc_t *>(cfg);

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
      // alt 0 is zero-bandwidth — skip but stay on interface
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
        // [7] bSamFreqType  then: continuous=(lower 3B, upper 3B); discrete=N×3B each
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
      bool is_isoc = (USB_EP_DESC_GET_XFERTYPE(ep) == USB_BM_ATTRIBUTES_XFER_ISOC);
      if (!is_isoc)
        continue;
      cur_alt_info.ep_addr    = ep->bEndpointAddress;
      cur_alt_info.mps        = USB_EP_DESC_GET_MPS(ep) * (USB_EP_DESC_GET_MULT(ep) + 1);
      cur_alt_info.alt_setting = cur_alt;
      cur_alt_info.valid      = true;
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

// ─────────────────────────────────────────────────────────────────────────────
// Control transfer helpers
// ─────────────────────────────────────────────────────────────────────────────

bool USBAudioClient::uac_set_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             const uint8_t *data, size_t len) {
  std::vector<uint8_t> buf(data, data + len);
  bool ok = false;
  bool done = false;
  // wValue: (selector << 8) | channel
  // wIndex: (unit_id << 8) | ac_intf_num
  uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  this->control_transfer(UAC_REQ_TYPE_INTF_SET, UAC_SET_CUR, wValue, wIndex,
                         [&ok, &done](const usb_host::TransferStatus &s) {
                           ok = s.success;
                           done = true;
                         },
                         buf);
  uint32_t t = millis();
  while (!done && (millis() - t) < CTRL_TIMEOUT_MS)
    vTaskDelay(pdMS_TO_TICKS(5));
  return ok && done;
}

bool USBAudioClient::uac_get_cur_interface_(uint8_t unit_id, uint8_t selector, uint8_t channel,
                                             uint8_t *data, size_t len) {
  bool ok = false;
  bool done = false;
  uint16_t wValue = static_cast<uint16_t>((selector << 8) | channel);
  uint16_t wIndex = static_cast<uint16_t>((unit_id << 8) | this->ac_intf_);
  this->control_transfer(UAC_REQ_TYPE_INTF_GET, UAC_GET_CUR, wValue, wIndex,
                         [&ok, &done, data, len](const usb_host::TransferStatus &s) {
                           ok = s.success;
                           done = true;
                           if (s.success && s.data_len >= len)
                             memcpy(data, s.data, len);
                         });
  uint32_t t = millis();
  while (!done && (millis() - t) < CTRL_TIMEOUT_MS)
    vTaskDelay(pdMS_TO_TICKS(5));
  return ok && done;
}

bool USBAudioClient::uac_set_cur_endpoint_(uint8_t ep_addr, uint8_t selector,
                                            const uint8_t *data, size_t len) {
  std::vector<uint8_t> buf(data, data + len);
  bool ok = false;
  bool done = false;
  uint16_t wValue = static_cast<uint16_t>(selector << 8);
  uint16_t wIndex = ep_addr;
  this->control_transfer(UAC_REQ_TYPE_EP_SET, UAC_SET_CUR, wValue, wIndex,
                         [&ok, &done](const usb_host::TransferStatus &s) {
                           ok = s.success;
                           done = true;
                         },
                         buf);
  uint32_t t = millis();
  while (!done && (millis() - t) < CTRL_TIMEOUT_MS)
    vTaskDelay(pdMS_TO_TICKS(5));
  return ok && done;
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

// ── Volume / mute ─────────────────────────────────────────────────────────────

bool USBAudioClient::apply_speaker_volume_() {
  if (!this->spk_stream_open_ || !this->speaker_fu_.has_volume) {
    if (!this->speaker_fu_.has_volume && !this->spk_volume_warned_) {
      ESP_LOGW(TAG, "Speaker volume control not supported by device");
      this->spk_volume_warned_ = true;
    }
    this->pending_spk_volume_ = false;
    this->spk_volume_supported_ = false;
    return false;
  }

  // Map [0.0, 1.0] linearly across [spk_vol_min_, spk_vol_max_].
  float clamped = std::clamp(this->spk_volume_, 0.0f, 1.0f);
  int16_t vol = static_cast<int16_t>(
      std::lround(static_cast<float>(this->spk_vol_min_) +
                  clamped * static_cast<float>(this->spk_vol_max_ - this->spk_vol_min_)));

  uint8_t buf[2] = {static_cast<uint8_t>(vol & 0xFF), static_cast<uint8_t>((vol >> 8) & 0xFF)};
  bool ok = this->uac_set_cur_interface_(this->speaker_fu_.unit_id, UAC_FU_VOLUME_CONTROL, 0, buf, sizeof(buf));
  if (ok) {
    this->pending_spk_volume_ = false;
    this->spk_volume_warned_ = false;
  } else if (!this->spk_volume_warned_) {
    ESP_LOGW(TAG, "Failed to set speaker volume");
    this->spk_volume_warned_ = true;
  }
  return ok;
}

bool USBAudioClient::apply_speaker_mute_() {
  if (!this->spk_stream_open_ || !this->speaker_fu_.has_mute) {
    if (!this->speaker_fu_.has_mute && !this->spk_mute_warned_) {
      ESP_LOGW(TAG, "Speaker mute control not supported by device");
      this->spk_mute_warned_ = true;
    }
    this->pending_spk_mute_ = false;
    this->spk_mute_supported_ = false;
    return false;
  }

  uint8_t val = this->spk_muted_ ? 1 : 0;
  bool ok = this->uac_set_cur_interface_(this->speaker_fu_.unit_id, UAC_FU_MUTE_CONTROL, 0, &val, 1);
  if (ok) {
    this->pending_spk_mute_ = false;
    this->spk_mute_warned_ = false;
  } else if (!this->spk_mute_warned_) {
    ESP_LOGW(TAG, "Failed to set speaker mute");
    this->spk_mute_warned_ = true;
  }
  return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream open / close
// ─────────────────────────────────────────────────────────────────────────────

bool USBAudioClient::open_speaker_stream_() {
  if (this->spk_stream_open_)
    return true;
  if (!this->spk_cfg_.configured || !this->spk_alt_.valid)
    return false;

  // Create ring buffer if not yet done.
  if (this->spk_rb_ == nullptr) {
    this->spk_rb_ = xRingbufferCreate(this->spk_buf_size_, RINGBUF_TYPE_BYTEBUF);
    if (this->spk_rb_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create speaker ring buffer");
      return false;
    }
  }

  this->spk_stream_.ep_addr        = this->spk_alt_.ep_addr;
  this->spk_stream_.mps            = this->spk_alt_.mps;
  this->spk_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->spk_stream_.num_urbs       = ISOC_NUM_URBS;
  this->spk_stream_.interface_num  = this->spk_as_intf_;
  this->spk_stream_.alt_setting    = this->spk_alt_.alt_setting;

  if (!this->stream_open_(this->spk_stream_, this)) {
    ESP_LOGE(TAG, "stream_open_ failed for speaker");
    return false;
  }

  // Set sampling frequency on the endpoint.
  this->set_sampling_frequency_(this->spk_alt_.ep_addr, this->spk_cfg_.sample_rate);

  // Read volume range so we can map [0,1] correctly.
  // Use a separate lambda that issues GET_MIN / GET_MAX (different bRequest, same selector).
  if (this->speaker_fu_.has_volume) {
    auto get_vol_range = [this](uint8_t bRequest, int16_t &out) {
      bool ok = false;
      bool done = false;
      uint16_t wValue = static_cast<uint16_t>((UAC_FU_VOLUME_CONTROL << 8) | 0);  // selector | master channel
      uint16_t wIndex = static_cast<uint16_t>((this->speaker_fu_.unit_id << 8) | this->ac_intf_);
      this->control_transfer(UAC_REQ_TYPE_INTF_GET, bRequest, wValue, wIndex,
                             [&ok, &done, &out](const usb_host::TransferStatus &s) {
                               ok = s.success;
                               done = true;
                               if (s.success && s.data_len >= 2)
                                 out = static_cast<int16_t>(s.data[0] | (s.data[1] << 8));
                             });
      uint32_t t = millis();
      while (!done && (millis() - t) < CTRL_TIMEOUT_MS)
        vTaskDelay(pdMS_TO_TICKS(5));
      return ok && done;
    };
    get_vol_range(UAC_GET_MIN, this->spk_vol_min_);
    get_vol_range(UAC_GET_MAX, this->spk_vol_max_);
    ESP_LOGD(TAG, "Speaker vol range: min=0x%04X max=0x%04X",
             static_cast<uint16_t>(this->spk_vol_min_), static_cast<uint16_t>(this->spk_vol_max_));
  }

  this->spk_stream_open_ = true;
  this->pending_spk_volume_ = true;
  this->pending_spk_mute_   = true;
  this->apply_speaker_volume_();
  this->apply_speaker_mute_();

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
      ESP_LOGE(TAG, "Failed to create microphone ring buffer");
      return false;
    }
  }

  this->mic_stream_.ep_addr        = this->mic_alt_.ep_addr;
  this->mic_stream_.mps            = this->mic_alt_.mps;
  this->mic_stream_.packets_per_urb = ISOC_PACKETS_PER_URB;
  this->mic_stream_.num_urbs       = ISOC_NUM_URBS;
  this->mic_stream_.interface_num  = this->mic_as_intf_;
  this->mic_stream_.alt_setting    = this->mic_alt_.alt_setting;

  if (!this->stream_open_(this->mic_stream_, this)) {
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
  this->stream_close_(this->spk_stream_);
  this->spk_stream_open_ = false;
  ESP_LOGI(TAG, "Speaker stream closed");
}

void USBAudioClient::close_microphone_stream_() {
  if (!this->mic_stream_open_)
    return;
  this->stream_close_(this->mic_stream_);
  this->mic_stream_open_ = false;
  ESP_LOGI(TAG, "Microphone stream closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void USBAudioClient::on_connected() {
  const usb_device_desc_t *dev = this->get_device_desc();
  if (dev != nullptr)
    ESP_LOGI(TAG, "USB Audio device connected: VID=0x%04X PID=0x%04X", dev->idVendor, dev->idProduct);

  if (!this->find_ac_interface_())
    return;
  this->parse_feature_units_();

  bool spk_ok = true;
  bool mic_ok = true;

  if (this->spk_cfg_.configured) {
    spk_ok = this->parse_as_interface_(true, this->spk_cfg_.channels,
                                        static_cast<uint8_t>(this->spk_cfg_.bits_per_sample),
                                        this->spk_cfg_.sample_rate,
                                        this->spk_as_intf_, this->spk_alt_);
  }

  if (this->mic_cfg_.configured) {
    mic_ok = this->parse_as_interface_(false, this->mic_cfg_.channels,
                                        static_cast<uint8_t>(this->mic_cfg_.bits_per_sample),
                                        this->mic_cfg_.sample_rate,
                                        this->mic_as_intf_, this->mic_alt_);
  }

  this->device_connected_ = true;

  // Open whichever streams are needed immediately.
  if (this->spk_cfg_.configured && spk_ok && !this->spk_suspended_)
    this->open_speaker_stream_();
  if (this->mic_cfg_.configured && mic_ok && !this->mic_suspended_)
    this->open_microphone_stream_();
}

void USBAudioClient::on_disconnected() {
  this->close_speaker_stream_();
  this->close_microphone_stream_();
  this->device_connected_ = false;
  this->spk_volume_warned_ = false;
  this->spk_mute_warned_   = false;
  this->spk_volume_supported_ = true;
  this->spk_mute_supported_   = true;
  this->pending_spk_volume_ = true;
  this->pending_spk_mute_   = true;
  ESP_LOGI(TAG, "USB Audio device disconnected");
}

// ─────────────────────────────────────────────────────────────────────────────
// Isochronous packet dispatch (USB-task context)
// ─────────────────────────────────────────────────────────────────────────────

void USBAudioClient::on_isoc_packet(uint8_t ep_addr, const uint8_t *data, size_t len, bool error) {
  if (error || len == 0)
    return;

  const bool is_in = (ep_addr & 0x80) != 0;

  if (!is_in) {
    // ── Speaker OUT: drain ring buffer into this packet's payload area ──────
    // `data` points into the URB's data_buffer. The isoc_cb_ trampoline calls
    // on_isoc_packet() BEFORE resubmitting, so writing here fills the buffer
    // for the next transmission.
    // `len` = actual_num_bytes (bytes sent in the completed transfer), which
    // may be 0 for SKIPPED packets. We always fill a full mps-worth of bytes
    // so that the resubmitted packet carries real data.
    if (this->spk_rb_ == nullptr || this->spk_suspended_) {
      memset(const_cast<uint8_t *>(data), 0, this->spk_alt_.mps);
      return;
    }
    const size_t fill_bytes = this->spk_alt_.mps;
    size_t received = 0;
    void *item = xRingbufferReceiveUpTo(this->spk_rb_, &received, 0, fill_bytes);
    if (item != nullptr) {
      memcpy(const_cast<uint8_t *>(data), item, received);
      vRingbufferReturnItem(this->spk_rb_, item);
      if (received < fill_bytes)
        memset(const_cast<uint8_t *>(data) + received, 0, fill_bytes - received);
    } else {
      memset(const_cast<uint8_t *>(data), 0, fill_bytes);
    }
  } else {
    // ── Microphone IN: write PCM into the ring buffer ────────────────────────
    if (this->mic_rb_ == nullptr || this->mic_suspended_)
      return;
    // Non-blocking send; if full, drop oldest data (overwrite) is not possible
    // with RINGBUF_TYPE_BYTEBUF, so we just skip if full to avoid blocking.
    xRingbufferSend(this->mic_rb_, data, len, 0);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Subcomponent API
// ─────────────────────────────────────────────────────────────────────────────

bool USBAudioClient::ensure_started_speaker() {
  if (!this->device_connected_)
    return false;
  if (!this->spk_stream_open_)
    return this->open_speaker_stream_();
  return true;
}

bool USBAudioClient::ensure_started_microphone() {
  if (!this->device_connected_)
    return false;
  if (!this->mic_stream_open_)
    return this->open_microphone_stream_();
  return true;
}

void USBAudioClient::resume_speaker() {
  this->spk_suspended_ = false;
  if (this->device_connected_ && !this->spk_stream_open_)
    this->open_speaker_stream_();
}

void USBAudioClient::suspend_speaker() {
  this->spk_suspended_ = true;
}

void USBAudioClient::resume_microphone() {
  this->mic_suspended_ = false;
  if (this->device_connected_ && !this->mic_stream_open_)
    this->open_microphone_stream_();
}

void USBAudioClient::suspend_microphone() {
  this->mic_suspended_ = true;
}

void USBAudioClient::set_speaker_volume_level(float volume) {
  this->spk_volume_ = std::clamp(volume, 0.0f, 1.0f);
  this->spk_volume_warned_ = false;
  this->pending_spk_volume_ = true;
  this->apply_speaker_volume_();
}

void USBAudioClient::set_speaker_mute_state(bool mute_state) {
  this->spk_muted_ = mute_state;
  this->spk_mute_warned_ = false;
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
