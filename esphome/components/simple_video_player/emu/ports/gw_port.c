#include "emu_ports.h"

#include <time.h>

#include "gw_romloader.h"
#include "gw_system.h"
#include "sm510.h"

#define GW_STEPS_PER_FRAME 2

unsigned char *ROM_DATA;
unsigned int ROM_DATA_LENGTH;

static uint16_t *s_frame;
static int16_t *s_mix;
static size_t s_mix_frames;
static uint8_t s_channels;
static unsigned int s_buttons;
static bool s_soft_only;
static bool s_soft_time;
static bool s_soft_a;
static unsigned s_prev_halt;

unsigned int gw_get_buttons()
{
    unsigned int buttons = s_soft_only ? 0 : s_buttons;
    if (s_soft_a)
        buttons |= GW_BUTTON_A;
    if (s_soft_time)
        buttons |= GW_BUTTON_TIME;
    return buttons;
}

static void gw_sync_time(bool force)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (!force && tm.tm_sec != 30)
        return;
    gw_time_t t = {(unsigned char)tm.tm_hour, (unsigned char)tm.tm_min, (unsigned char)tm.tm_sec};
    gw_system_set_time(t);
}

bool gw_port_load(uint8_t *rom, size_t size, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    ROM_DATA = rom;
    ROM_DATA_LENGTH = (unsigned int)size;
    s_frame = calloc(GW_SCREEN_WIDTH * GW_SCREEN_HEIGHT, sizeof(uint16_t));
    s_mix = calloc(GW_AUDIO_BUFFER_LENGTH * GW_STEPS_PER_FRAME * s_channels, sizeof(int16_t));
    if (s_frame == NULL || s_mix == NULL)
        return false;
    if (!gw_system_romload())
        return false;
    gw_system_sound_init();
    if (!gw_system_config())
        return false;
    gw_system_start();
    gw_system_reset();
    s_soft_only = true;
    gw_system_run(GW_AUDIO_FREQ * 2);
    s_soft_time = true;
    gw_system_run(GW_AUDIO_FREQ / 2);
    s_soft_time = false;
    gw_system_run(GW_AUDIO_FREQ * 2);
    gw_sync_time(true);
    gw_system_run(GW_AUDIO_FREQ);
    s_soft_a = true;
    gw_system_run(GW_AUDIO_FREQ / 2);
    s_soft_a = false;
    gw_system_run(GW_AUDIO_FREQ);
    s_soft_only = false;
    s_prev_halt = 2;
    return true;
}

void gw_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad2;
    unsigned int buttons = 0;
    if (pad1 & EMU_PORT_LEFT)
        buttons |= GW_BUTTON_LEFT;
    if (pad1 & EMU_PORT_UP)
        buttons |= GW_BUTTON_UP;
    if (pad1 & EMU_PORT_RIGHT)
        buttons |= GW_BUTTON_RIGHT;
    if (pad1 & EMU_PORT_DOWN)
        buttons |= GW_BUTTON_DOWN;
    if (pad1 & EMU_PORT_A)
        buttons |= GW_BUTTON_A;
    if (pad1 & EMU_PORT_B)
        buttons |= GW_BUTTON_B;
    if (pad1 & EMU_PORT_SELECT)
        buttons |= GW_BUTTON_TIME;
    if (pad1 & EMU_PORT_START)
        buttons |= GW_BUTTON_GAME;
    s_buttons = buttons;
    size_t n = 0;
    for (int step = 0; step < GW_STEPS_PER_FRAME; step++)
    {
        if (s_prev_halt != m_halt)
            gw_sync_time(s_prev_halt == 2);
        s_prev_halt = m_halt;
        gw_system_run(GW_SYSTEM_CYCLES);
        for (size_t i = 0; i < GW_AUDIO_BUFFER_LENGTH; i++)
        {
            const int16_t v = (int16_t)(gw_audio_buffer[i] << 13);
            if (s_channels == 2)
            {
                s_mix[n * 2] = v;
                s_mix[n * 2 + 1] = v;
            }
            else
            {
                s_mix[n] = v;
            }
            n++;
        }
        gw_audio_buffer_copied = true;
    }
    s_mix_frames = n;
    gw_system_blit(s_frame);
}

void gw_port_video(emu_port_video_t *video)
{
    video->pixels = (const uint8_t *)s_frame;
    video->palette = NULL;
    video->stride = GW_SCREEN_WIDTH * 2;
    video->width = GW_SCREEN_WIDTH;
    video->height = GW_SCREEN_HEIGHT;
}

const int16_t *gw_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t gw_port_frame_us(void)
{
    return 1000000u * GW_STEPS_PER_FRAME / GW_REFRESH_RATE;
}

size_t gw_port_save_state(uint8_t *buf, size_t cap)
{
    if (cap < sizeof(gw_state_t))
        return 0;
    memset(buf, 0, sizeof(gw_state_t));
    return gw_state_save(buf) ? sizeof(gw_state_t) : 0;
}

bool gw_port_load_state(const uint8_t *buf, size_t len)
{
    if (len < sizeof(gw_state_t))
        return false;
    return gw_state_load((void *)buf);
}
