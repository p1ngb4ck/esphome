#include "emu_ports.h"

#include "shared.h"

static uint8_t *s_bitmap;
static uint16_t s_palette[256];
static int16_t *s_mix;
static size_t s_mix_frames;
static uint8_t s_channels;

static int sms_pad(uint16_t pad)
{
    int out = 0;
    if (pad & EMU_PORT_UP)
        out |= INPUT_UP;
    if (pad & EMU_PORT_DOWN)
        out |= INPUT_DOWN;
    if (pad & EMU_PORT_LEFT)
        out |= INPUT_LEFT;
    if (pad & EMU_PORT_RIGHT)
        out |= INPUT_RIGHT;
    if (pad & EMU_PORT_A)
        out |= INPUT_BUTTON2;
    if (pad & EMU_PORT_B)
        out |= INPUT_BUTTON1;
    return out;
}

bool sms_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels, int console)
{
    sms_ptr = calloc(1, sizeof(sms_t));
    vdp_ptr = calloc(1, sizeof(vdp_t));
    z80_ptr = calloc(1, sizeof(Z80_Regs));
    if (sms_ptr == NULL || vdp_ptr == NULL || z80_ptr == NULL)
        return false;
    system_reset_config();
    option.sndrate = (int)rate;
    option.overscan = 0;
    option.extra_gg = 0;
    option.tms_pal = 0;
    option.console = console;
    if (size > 0x4000 && ((size / 512) & 1))
    {
        rom += 512;
        size -= 512;
    }
    uint8_t *data = rom;
    size_t buf_size = size;
    if (size < 0x4000)
    {
        data = calloc(1, 0x4000);
        if (data == NULL)
            return false;
        memcpy(data, rom, size);
        buf_size = 0x4000;
    }
    if (!load_rom(data, (int)buf_size, (int)size))
        return false;
    s_bitmap = calloc(1, SMS_WIDTH * SMS_HEIGHT);
    if (s_bitmap == NULL)
        return false;
    bitmap.width = SMS_WIDTH;
    bitmap.height = SMS_HEIGHT;
    bitmap.pitch = SMS_WIDTH;
    bitmap.data = s_bitmap;
    system_poweron();
    if (snd.stream[0] == NULL || snd.stream[1] == NULL)
        return false;
    s_channels = channels > 1 ? 2 : 1;
    s_mix = calloc((size_t)snd.sample_count * s_channels, sizeof(int16_t));
    return s_mix != NULL;
}

void sms_port_frame(uint16_t pad1, uint16_t pad2)
{
    input.pad[0] = sms_pad(pad1);
    input.pad[1] = sms_pad(pad2);
    input.system = 0;
    if (IS_SMS)
    {
        if (pad1 & EMU_PORT_START)
            input.system |= INPUT_PAUSE;
        if (pad1 & EMU_PORT_SELECT)
            input.system |= INPUT_START;
    }
    else if (IS_GG)
    {
        if (pad1 & EMU_PORT_START)
            input.system |= INPUT_START;
        if (pad1 & EMU_PORT_SELECT)
            input.system |= INPUT_PAUSE;
    }
    else
    {
        coleco.keypad[0] = 0xff;
        coleco.keypad[1] = 0xff;
        if (pad1 & EMU_PORT_X)
            coleco.keypad[0] = 1;
        else if (pad1 & EMU_PORT_Y)
            coleco.keypad[0] = 2;
        else if (pad1 & EMU_PORT_L)
            coleco.keypad[0] = 10;
        else if (pad1 & EMU_PORT_R)
            coleco.keypad[0] = 11;
        if (pad1 & EMU_PORT_SELECT)
            system_reset();
    }
    system_frame(0);
    uint16_t pal[256];
    if (render_copy_palette(pal))
    {
        for (int i = 0; i < 256; i++)
            s_palette[i] = (uint16_t)((pal[i] << 8) | (pal[i] >> 8));
    }
    const size_t count = (size_t)snd.sample_count;
    for (size_t i = 0; i < count; i++)
    {
        int l = (int)(snd.stream[0][i] * 2.75f);
        int r = (int)(snd.stream[1][i] * 2.75f);
        l = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
        r = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
        if (s_channels == 2)
        {
            s_mix[i * 2] = (int16_t)l;
            s_mix[i * 2 + 1] = (int16_t)r;
        }
        else
        {
            s_mix[i] = (int16_t)((l + r) / 2);
        }
    }
    s_mix_frames = count;
}

void sms_port_video(emu_port_video_t *video)
{
    video->pixels = s_bitmap + bitmap.viewport.x;
    video->palette = s_palette;
    video->stride = (uint32_t)bitmap.pitch;
    video->width = (uint16_t)bitmap.viewport.w;
    video->height = (uint16_t)bitmap.viewport.h;
}

const int16_t *sms_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t sms_port_frame_us(void)
{
    return sms.display == DISPLAY_NTSC ? 16667u : 20000u;
}
