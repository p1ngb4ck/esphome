#include "emulator.h"

#ifdef SVP_EMULATOR

#include <new>

#include "emu_ports.h"

namespace esphome::simple_video_player {

static_assert(static_cast<int>(EMU_UP) == static_cast<int>(EMU_PORT_UP) &&
                  static_cast<int>(EMU_START) == static_cast<int>(EMU_PORT_START) &&
                  static_cast<int>(EMU_SELECT) == static_cast<int>(EMU_PORT_SELECT),
              "button bits differ from the ports'");

#ifdef SVP_EMU_NES
class NesCore final : public EmuCore {
 public:
  bool load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels) override {
    this->rate_ = rate;
    this->channels_ = channels;
    return nes_port_load(rom, size, rate, channels);
  }
  void run_frame(const uint16_t *pads) override { nes_port_frame(pads[0], pads[1]); }
  EmuVideo video() override {
    emu_port_video_t v{};
    nes_port_video(&v);
    return {v.pixels, v.palette, v.stride, v.width, v.height};
  }
  const int16_t *audio(size_t *frames) override { return nes_port_audio(frames); }
  uint32_t sample_rate() const override { return this->rate_; }
  uint8_t channels() const override { return this->channels_; }
  uint32_t frame_us() const override { return nes_port_frame_us(); }

 protected:
  uint32_t rate_{32000};
  uint8_t channels_{1};
};
static_assert(sizeof(NesCore) <= EMU_CORE_STORE, "core object too large");
#endif

EmuCore *emu_make_core(EmuSystem system, void *store) {
  switch (system) {
#ifdef SVP_EMU_NES
    case EmuSystem::NES:
      return new (store) NesCore();
#endif
    default:
      return nullptr;
  }
}

}

#endif
