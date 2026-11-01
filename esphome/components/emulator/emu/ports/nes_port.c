#include "emu_ports.h"

#include "emu_mem.h"
#include "nofrendo.h"
#include "nes/state.h"

static nes_t *s_nes;
static uint8_t *s_vidbuf;
static uint16_t *s_palette;

static int nes_pad(uint16_t pad)
{
    int out = 0;
    if (pad & EMU_PORT_UP)
        out |= NES_PAD_UP;
    if (pad & EMU_PORT_DOWN)
        out |= NES_PAD_DOWN;
    if (pad & EMU_PORT_LEFT)
        out |= NES_PAD_LEFT;
    if (pad & EMU_PORT_RIGHT)
        out |= NES_PAD_RIGHT;
    if (pad & EMU_PORT_A)
        out |= NES_PAD_A;
    if (pad & EMU_PORT_B)
        out |= NES_PAD_B;
    if (pad & EMU_PORT_START)
        out |= NES_PAD_START;
    if (pad & EMU_PORT_SELECT)
        out |= NES_PAD_SELECT;
    return out;
}

bool nes_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels)
{
    s_nes = nes_init(SYS_DETECT, (int)rate, channels > 1, NULL);
    if (s_nes == NULL)
        return false;
    if (nes_insertcart(rom_loadmem(rom, size)) < 0)
        return false;
    s_vidbuf = malloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
    s_palette = nofrendo_buildpalette(NES_PALETTE_PVM, 16);
    if (s_vidbuf == NULL || s_palette == NULL)
        return false;
    memset(s_vidbuf, 0, NES_SCREEN_PITCH * NES_SCREEN_HEIGHT);
    nes_setvidbuf(s_vidbuf);
    ppu_setopt(PPU_LIMIT_SPRITES, true);
    return true;
}

void nes_port_frame(uint16_t pad1, uint16_t pad2)
{
    input_update(0, nes_pad(pad1));
    input_update(1, nes_pad(pad2));
    nes_emulate(true);
}

void nes_port_video(emu_port_video_t *video)
{
    const int crop = s_nes->overscan;
    video->pixels = s_vidbuf + crop * NES_SCREEN_PITCH + NES_SCREEN_OVERDRAW;
    video->palette = s_palette;
    video->stride = NES_SCREEN_PITCH;
    video->width = NES_SCREEN_WIDTH;
    video->height = NES_SCREEN_HEIGHT - crop * 2;
}

const int16_t *nes_port_audio(size_t *frames)
{
    *frames = (size_t)s_nes->apu->samples_per_frame;
    return s_nes->apu->buffer;
}

uint32_t nes_port_frame_us(void)
{
    return s_nes->refresh_rate > 0 ? 1000000u / (uint32_t)s_nes->refresh_rate : 16667u;
}

uint8_t *nes_port_sram(size_t *size)
{
    if (s_nes == NULL || s_nes->cart == NULL || !s_nes->cart->battery || s_nes->cart->prg_ram_banks <= 0)
        return NULL;
    *size = (size_t)s_nes->cart->prg_ram_banks * ROM_PRG_BANK_SIZE;
    return s_nes->cart->prg_ram;
}

size_t nes_port_save_state(uint8_t *buf, size_t cap)
{
    emu_mem_arm(buf, cap, 0);
    const bool ok = state_save("state") == 0 && !emu_mem_failed();
    const size_t len = emu_mem_length();
    emu_mem_arm(NULL, 0, 0);
    return ok ? len : 0;
}

bool nes_port_load_state(const uint8_t *buf, size_t len)
{
    emu_mem_arm((uint8_t *)buf, len, len);
    const bool ok = state_load("state") == 0;
    emu_mem_arm(NULL, 0, 0);
    return ok;
}
