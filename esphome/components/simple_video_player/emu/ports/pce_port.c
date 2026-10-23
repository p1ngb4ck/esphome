#include "emu_ports.h"

#include "pce-go.h"
#include "pce.h"
#include "psg.h"

#define PCE_AUDIO_CHUNK 64

static uint8_t *s_frame;
static uint16_t *s_palette;
static int16_t *s_stereo;
static int16_t *s_mix;
static size_t s_mix_frames;
static size_t s_frames;
static uint32_t s_rate;
static uint32_t s_acc;
static uint8_t s_channels;
static uint16_t s_width = 256;
static uint16_t s_height = 240;

uint8_t *osd_gfx_framebuffer(int width, int height)
{
    if (width > 0 && height > 0)
    {
        s_width = (uint16_t)width;
        s_height = (uint16_t)height;
    }
    return s_frame != NULL ? s_frame + 16 : NULL;
}

void osd_input_read(uint8_t joypads[8])
{
    (void)joypads;
}

void osd_vsync(void)
{
}

static uint8_t pce_pad(uint16_t pad)
{
    uint8_t out = 0;
    if (pad & EMU_PORT_UP)
        out |= JOY_UP;
    if (pad & EMU_PORT_DOWN)
        out |= JOY_DOWN;
    if (pad & EMU_PORT_LEFT)
        out |= JOY_LEFT;
    if (pad & EMU_PORT_RIGHT)
        out |= JOY_RIGHT;
    if (pad & EMU_PORT_A)
        out |= JOY_A;
    if (pad & EMU_PORT_B)
        out |= JOY_B;
    if (pad & EMU_PORT_START)
        out |= JOY_RUN;
    if (pad & EMU_PORT_SELECT)
        out |= JOY_SELECT;
    return out;
}

bool pce_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    s_rate = rate;
    s_frames = rate / 60 + 1;
    s_frame = calloc(1, XBUF_WIDTH * XBUF_HEIGHT);
    s_stereo = calloc(s_frames * 2, sizeof(int16_t));
    s_mix = calloc(s_frames * s_channels, sizeof(int16_t));
    if (s_frame == NULL || s_stereo == NULL || s_mix == NULL)
        return false;
    if (InitPCE((int)rate, true) != 0)
        return false;
    if (LoadCard(rom, size) != 0)
        return false;
    s_palette = PalettePCE(16);
    return s_palette != NULL;
}

void pce_port_frame(uint16_t pad1, uint16_t pad2)
{
    PCE.Joypad.regs[0] = pce_pad(pad1);
    PCE.Joypad.regs[1] = pce_pad(pad2);
    pce_run();
    s_acc += s_rate;
    const size_t frames = s_acc / 60;
    s_acc %= 60;
    for (size_t done = 0; done < frames;)
    {
        size_t n = frames - done;
        if (n > PCE_AUDIO_CHUNK)
            n = PCE_AUDIO_CHUNK;
        psg_update(s_stereo + done * 2, n, 0xFF);
        done += n;
    }
    if (s_channels == 2)
    {
        memcpy(s_mix, s_stereo, frames * 2 * sizeof(int16_t));
    }
    else
    {
        for (size_t i = 0; i < frames; i++)
            s_mix[i] = (int16_t)(((int)s_stereo[i * 2] + (int)s_stereo[i * 2 + 1]) / 2);
    }
    s_mix_frames = frames;
}

void pce_port_video(emu_port_video_t *video)
{
    video->pixels = s_frame + 16;
    video->palette = s_palette;
    video->stride = XBUF_WIDTH;
    video->width = s_width;
    video->height = s_height;
}

const int16_t *pce_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t pce_port_frame_us(void)
{
    return 16667u;
}
