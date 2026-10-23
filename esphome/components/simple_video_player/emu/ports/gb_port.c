#include "emu_ports.h"

#include <time.h>

#include "emu_mem.h"
#include "gnuboy.h"
#include "hw.h"

static uint16_t *s_frame;
static int16_t *s_sound;
static const int16_t *s_audio;
static size_t s_audio_count;
static uint8_t s_channels;
static int s_pad;

static void gb_video_cb(void *buffer)
{
    (void)buffer;
}

static void gb_audio_cb(void *buffer, size_t length)
{
    s_audio = buffer;
    s_audio_count = length;
}

bool gb_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    if (gnuboy_init((int)rate, s_channels == 2 ? GB_AUDIO_STEREO_S16 : GB_AUDIO_MONO_S16, GB_PIXEL_565_LE,
                    gb_video_cb, gb_audio_cb) < 0)
        return false;
    const size_t len = (rate / 50 + 1) * 2 * s_channels;
    s_frame = calloc(1, GB_WIDTH * GB_HEIGHT * 2);
    s_sound = calloc(len, sizeof(int16_t));
    if (s_frame == NULL || s_sound == NULL)
        return false;
    gnuboy_set_framebuffer(s_frame);
    gnuboy_set_soundbuffer(s_sound, len);
    if (gnuboy_load_rom(rom, size) < 0)
        return false;
    gnuboy_set_palette(GB_PALETTE_DMG);
    gnuboy_reset(true);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    gnuboy_set_time(tm.tm_yday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    s_pad = -1;
    return true;
}

void gb_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad2;
    int pad = 0;
    if (pad1 & EMU_PORT_UP)
        pad |= GB_PAD_UP;
    if (pad1 & EMU_PORT_DOWN)
        pad |= GB_PAD_DOWN;
    if (pad1 & EMU_PORT_LEFT)
        pad |= GB_PAD_LEFT;
    if (pad1 & EMU_PORT_RIGHT)
        pad |= GB_PAD_RIGHT;
    if (pad1 & EMU_PORT_A)
        pad |= GB_PAD_A;
    if (pad1 & EMU_PORT_B)
        pad |= GB_PAD_B;
    if (pad1 & EMU_PORT_START)
        pad |= GB_PAD_START;
    if (pad1 & EMU_PORT_SELECT)
        pad |= GB_PAD_SELECT;
    if (pad != s_pad)
    {
        gnuboy_set_pad(pad);
        s_pad = pad;
    }
    s_audio_count = 0;
    if (emu_link_active())
        gb_link_poll();
    gnuboy_run(true);
}

void gb_port_video(emu_port_video_t *video)
{
    video->pixels = (const uint8_t *)s_frame;
    video->palette = NULL;
    video->stride = GB_WIDTH * 2;
    video->width = GB_WIDTH;
    video->height = GB_HEIGHT;
}

const int16_t *gb_port_audio(size_t *frames)
{
    *frames = s_audio_count / s_channels;
    return s_audio;
}

uint32_t gb_port_frame_us(void)
{
    return 16742u;
}

uint8_t *gb_port_sram(size_t *size)
{
    if (!cart.has_battery || cart.rambanks == NULL || cart.ramsize <= 0)
        return NULL;
    *size = (size_t)cart.ramsize * 8192;
    return (uint8_t *)cart.rambanks;
}

size_t gb_port_save_state(uint8_t *buf, size_t cap)
{
    emu_mem_arm(buf, cap, 0);
    const bool ok = gnuboy_save_state("state") == 0 && !emu_mem_failed();
    const size_t len = emu_mem_length();
    emu_mem_arm(NULL, 0, 0);
    return ok ? len : 0;
}

bool gb_port_load_state(const uint8_t *buf, size_t len)
{
    emu_mem_arm((uint8_t *)buf, len, len);
    const bool ok = gnuboy_load_state("state") == 0;
    emu_mem_arm(NULL, 0, 0);
    return ok;
}
