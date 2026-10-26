#include "emulator.h"

#ifdef SVP_EMULATOR

#include <cstring>
#include <new>
#include <strings.h>

#include "emu_ports.h"

namespace esphome::simple_video_player {

static_assert(static_cast<int>(EMU_UP) == static_cast<int>(EMU_PORT_UP) &&
                  static_cast<int>(EMU_START) == static_cast<int>(EMU_PORT_START) &&
                  static_cast<int>(EMU_SELECT) == static_cast<int>(EMU_PORT_SELECT),
              "button bits differ from the ports'");

struct PortApi {
  bool (*load)(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels);
  void (*frame)(uint16_t pad1, uint16_t pad2);
  void (*video)(emu_port_video_t *video);
  const int16_t *(*audio)(size_t *frames);
  uint32_t (*frame_us)();
  uint32_t fixed_rate;
  void (*key)(uint16_t usage, bool pressed);
  void (*add_file)(const char *name, const uint8_t *data, size_t size);
  uint8_t *(*sram)(size_t *size);
  size_t (*save_state)(uint8_t *buf, size_t cap);
  bool (*load_state)(const uint8_t *buf, size_t len);
  void (*mix_begin)();
  void (*mix_run)();
};

class PortCore final : public EmuCore {
 public:
  explicit PortCore(const PortApi *api) : api_(api) {}
  bool load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) override {
    this->rate_ = this->api_->fixed_rate != 0 ? this->api_->fixed_rate : rate;
    this->channels_ = channels > 1 ? 2 : 1;
    return this->api_->load(rom, size, path, this->rate_, this->channels_);
  }
  void run_frame(const uint16_t *pads) override { this->api_->frame(pads[0], pads[1]); }
  EmuVideo video() override {
    emu_port_video_t v{};
    this->api_->video(&v);
    return {v.pixels, v.palette, v.stride, v.width, v.height, v.buffers > 1};
  }
  const int16_t *audio(size_t *frames) override { return this->api_->audio(frames); }
  uint32_t sample_rate() const override { return this->rate_; }
  uint8_t channels() const override { return this->channels_; }
  uint32_t frame_us() const override { return this->api_->frame_us(); }
  bool owns_loop() const override { return this->api_->key != nullptr; }
  bool keyboard() const override { return this->api_->key != nullptr; }
  void key(uint16_t usage, bool pressed) override {
    if (this->api_->key != nullptr)
      this->api_->key(usage, pressed);
  }
  void add_file(const char *name, const uint8_t *data, size_t size) override {
    if (this->api_->add_file != nullptr)
      this->api_->add_file(name, data, size);
  }
  uint8_t *sram(size_t *size) override { return this->api_->sram != nullptr ? this->api_->sram(size) : nullptr; }
  size_t save_state(uint8_t *buf, size_t cap) override {
    return this->api_->save_state != nullptr ? this->api_->save_state(buf, cap) : 0;
  }
  bool load_state(const uint8_t *buf, size_t len) override {
    return this->api_->load_state != nullptr && this->api_->load_state(buf, len);
  }
  bool split_mix() const override { return this->api_->mix_run != nullptr; }
  void mix_begin() override {
    if (this->api_->mix_begin != nullptr)
      this->api_->mix_begin();
  }
  void mix_run() override {
    if (this->api_->mix_run != nullptr)
      this->api_->mix_run();
  }

 protected:
  const PortApi *api_;
  uint32_t rate_{32000};
  uint8_t channels_{1};
};
static_assert(sizeof(PortCore) <= EMU_CORE_STORE, "core object too large");

static bool has_extension(const char *path, const char *ext) {
  const char *dot = strrchr(path, '.');
  return dot != nullptr && strcasecmp(dot + 1, ext) == 0;
}

#ifdef SVP_EMU_NES
static bool nes_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return nes_port_load(rom, size, rate, channels);
}
static const PortApi NES_API = {
    .load = nes_load,
    .frame = nes_port_frame,
    .video = nes_port_video,
    .audio = nes_port_audio,
    .frame_us = nes_port_frame_us,
    .fixed_rate = 0,
    .sram = nes_port_sram,
    .save_state = nes_port_save_state,
    .load_state = nes_port_load_state,
};
#endif

#ifdef SVP_EMU_GB
static bool gb_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return gb_port_load(rom, size, rate, channels);
}
static const PortApi GB_API = {
    .load = gb_load,
    .frame = gb_port_frame,
    .video = gb_port_video,
    .audio = gb_port_audio,
    .frame_us = gb_port_frame_us,
    .fixed_rate = 0,
    .sram = gb_port_sram,
    .save_state = gb_port_save_state,
    .load_state = gb_port_load_state,
};
#endif

#ifdef SVP_EMU_SMS
static bool sms_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  int console = 0;
  if (has_extension(path, "sg"))
    console = 5;
  else if (has_extension(path, "col"))
    console = 6;
  return sms_port_load(rom, size, rate, channels, console);
}
static const PortApi SMS_API = {
    .load = sms_load,
    .frame = sms_port_frame,
    .video = sms_port_video,
    .audio = sms_port_audio,
    .frame_us = sms_port_frame_us,
    .fixed_rate = 0,
    .sram = sms_port_sram,
    .save_state = sms_port_save_state,
    .load_state = sms_port_load_state,
};
#endif

#ifdef SVP_EMU_PCE
static bool pce_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return pce_port_load(rom, size, rate, channels);
}
static const PortApi PCE_API = {
    .load = pce_load,
    .frame = pce_port_frame,
    .video = pce_port_video,
    .audio = pce_port_audio,
    .frame_us = pce_port_frame_us,
    .fixed_rate = 0,
    .save_state = pce_port_save_state,
    .load_state = pce_port_load_state,
};
#endif

#ifdef SVP_EMU_LYNX
static bool lynx_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return lynx_port_load(rom, size, channels);
}
static const PortApi LYNX_API = {
    .load = lynx_load,
    .frame = lynx_port_frame,
    .video = lynx_port_video,
    .audio = lynx_port_audio,
    .frame_us = lynx_port_frame_us,
    .fixed_rate = 32000,
    .save_state = lynx_port_save_state,
    .load_state = lynx_port_load_state,
};
#endif

#ifdef SVP_EMU_SNES
static bool snes_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return snes_port_load(rom, size, rate, channels);
}
static const PortApi SNES_API = {
    .load = snes_load,
    .frame = snes_port_frame,
    .video = snes_port_video,
    .audio = snes_port_audio,
    .frame_us = snes_port_frame_us,
    .fixed_rate = 0,
    .sram = snes_port_sram,
    .save_state = snes_port_save_state,
    .load_state = snes_port_load_state,
    .mix_begin = snes_port_mix_begin,
    .mix_run = snes_port_mix_run,
};
#endif

#ifdef SVP_EMU_MD
static bool md_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return md_port_load(rom, size, channels);
}
static const PortApi MD_API = {
    .load = md_load,
    .frame = md_port_frame,
    .video = md_port_video,
    .audio = md_port_audio,
    .frame_us = md_port_frame_us,
    .fixed_rate = 26633,
    .save_state = md_port_save_state,
    .load_state = md_port_load_state,
};
#endif

#ifdef SVP_EMU_GW
static bool gw_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return gw_port_load(rom, size, channels);
}
static const PortApi GW_API = {
    .load = gw_load,
    .frame = gw_port_frame,
    .video = gw_port_video,
    .audio = gw_port_audio,
    .frame_us = gw_port_frame_us,
    .fixed_rate = 32768,
    .save_state = gw_port_save_state,
    .load_state = gw_port_load_state,
};
#endif

#ifdef SVP_EMU_MSX
static const PortApi MSX_API = {
    .load = msx_port_load,
    .frame = msx_port_frame,
    .video = msx_port_video,
    .audio = msx_port_audio,
    .frame_us = msx_port_frame_us,
    .fixed_rate = 0,
    .key = msx_port_key,
    .add_file = msx_port_add_file,
    .save_state = msx_port_save_state,
    .load_state = msx_port_load_state,
};
#endif

EmuCore *emu_make_core(EmuSystem system, void *store) {
  const PortApi *api = nullptr;
  switch (system) {
#ifdef SVP_EMU_NES
    case EmuSystem::NES:
      api = &NES_API;
      break;
#endif
#ifdef SVP_EMU_GB
    case EmuSystem::GB:
      api = &GB_API;
      break;
#endif
#ifdef SVP_EMU_SMS
    case EmuSystem::SMS:
      api = &SMS_API;
      break;
#endif
#ifdef SVP_EMU_PCE
    case EmuSystem::PCE:
      api = &PCE_API;
      break;
#endif
#ifdef SVP_EMU_LYNX
    case EmuSystem::LYNX:
      api = &LYNX_API;
      break;
#endif
#ifdef SVP_EMU_SNES
    case EmuSystem::SNES:
      api = &SNES_API;
      break;
#endif
#ifdef SVP_EMU_MD
    case EmuSystem::MD:
      api = &MD_API;
      break;
#endif
#ifdef SVP_EMU_GW
    case EmuSystem::GW:
      api = &GW_API;
      break;
#endif
#ifdef SVP_EMU_MSX
    case EmuSystem::MSX:
      api = &MSX_API;
      break;
#endif
    default:
      break;
  }
  return api != nullptr ? new (store) PortCore(api) : nullptr;
}

}

#endif
