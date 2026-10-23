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
    return {v.pixels, v.palette, v.stride, v.width, v.height};
  }
  const int16_t *audio(size_t *frames) override { return this->api_->audio(frames); }
  uint32_t sample_rate() const override { return this->rate_; }
  uint8_t channels() const override { return this->channels_; }
  uint32_t frame_us() const override { return this->api_->frame_us(); }

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
static const PortApi NES_API = {nes_load, nes_port_frame, nes_port_video, nes_port_audio, nes_port_frame_us, 0};
#endif

#ifdef SVP_EMU_GB
static bool gb_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return gb_port_load(rom, size, rate, channels);
}
static const PortApi GB_API = {gb_load, gb_port_frame, gb_port_video, gb_port_audio, gb_port_frame_us, 0};
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
static const PortApi SMS_API = {sms_load, sms_port_frame, sms_port_video, sms_port_audio, sms_port_frame_us, 0};
#endif

#ifdef SVP_EMU_PCE
static bool pce_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return pce_port_load(rom, size, rate, channels);
}
static const PortApi PCE_API = {pce_load, pce_port_frame, pce_port_video, pce_port_audio, pce_port_frame_us, 0};
#endif

#ifdef SVP_EMU_LYNX
static bool lynx_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return lynx_port_load(rom, size, channels);
}
static const PortApi LYNX_API = {lynx_load,       lynx_port_frame,    lynx_port_video, lynx_port_audio,
                                 lynx_port_frame_us, 32000};
#endif

#ifdef SVP_EMU_SNES
static bool snes_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return snes_port_load(rom, size, rate, channels);
}
static const PortApi SNES_API = {snes_load, snes_port_frame, snes_port_video, snes_port_audio, snes_port_frame_us, 0};
#endif

#ifdef SVP_EMU_MD
static bool md_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return md_port_load(rom, size, channels);
}
static const PortApi MD_API = {md_load, md_port_frame, md_port_video, md_port_audio, md_port_frame_us, 26633};
#endif

#ifdef SVP_EMU_GW
static bool gw_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels) {
  return gw_port_load(rom, size, channels);
}
static const PortApi GW_API = {gw_load, gw_port_frame, gw_port_video, gw_port_audio, gw_port_frame_us, 32768};
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
    default:
      break;
  }
  return api != nullptr ? new (store) PortCore(api) : nullptr;
}

}

#endif
