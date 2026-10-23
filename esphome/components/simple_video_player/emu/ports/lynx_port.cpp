#include "emu_ports.h"

#include "emu_mem.h"
#include "handy.h"

static CSystem *s_lynx;
static uint16_t *s_frame;
static int16_t *s_mix;
static size_t s_mix_frames;
static uint8_t s_channels;
static uint16_t s_width;
static uint16_t s_height;
static ULONG s_up;
static ULONG s_down;
static ULONG s_left;
static ULONG s_right;

static void lynx_rotation(void)
{
    int rotation = 0;
    switch (s_lynx->mCart->CRC32())
    {
        case 0x97501709:
        case 0x0271B6E9:
        case 0x006FD398:
        case 0xBCD10C3A:
            rotation = 1;
            break;
        case 0x7F0EC7AD:
        case 0xAC564BAA:
        case 0xA53649F1:
            rotation = 2;
            break;
        default:
            if (s_lynx->mCart->CartGetRotate() == CART_ROTATE_LEFT)
                rotation = 1;
            if (s_lynx->mCart->CartGetRotate() == CART_ROTATE_RIGHT)
                rotation = 2;
            break;
    }
    if (rotation == 1)
    {
        s_width = HANDY_SCREEN_HEIGHT;
        s_height = HANDY_SCREEN_WIDTH;
        s_lynx->mMikie->SetRotation(MIKIE_ROTATE_L);
        s_up = BUTTON_RIGHT;
        s_down = BUTTON_LEFT;
        s_left = BUTTON_UP;
        s_right = BUTTON_DOWN;
    }
    else if (rotation == 2)
    {
        s_width = HANDY_SCREEN_HEIGHT;
        s_height = HANDY_SCREEN_WIDTH;
        s_lynx->mMikie->SetRotation(MIKIE_ROTATE_R);
        s_up = BUTTON_LEFT;
        s_down = BUTTON_RIGHT;
        s_left = BUTTON_DOWN;
        s_right = BUTTON_UP;
    }
    else
    {
        s_width = HANDY_SCREEN_WIDTH;
        s_height = HANDY_SCREEN_HEIGHT;
        s_lynx->mMikie->SetRotation(MIKIE_NO_ROTATE);
        s_up = BUTTON_UP;
        s_down = BUTTON_DOWN;
        s_left = BUTTON_LEFT;
        s_right = BUTTON_RIGHT;
    }
}

bool lynx_port_load(uint8_t *rom, size_t size, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    s_lynx = emu_new<CSystem>((const UBYTE *)rom, (ULONG)size, (long)MIKIE_PIXEL_FORMAT_16BPP_565,
                              (long)HANDY_AUDIO_SAMPLE_FREQ);
    if (s_lynx == nullptr || s_lynx->mFileType == HANDY_FILETYPE_ILLEGAL)
        return false;
    s_frame = (uint16_t *)calloc(HANDY_SCREEN_WIDTH * HANDY_SCREEN_WIDTH, sizeof(uint16_t));
    gAudioBuffer = (SWORD *)calloc(HANDY_AUDIO_BUFFER_LENGTH, sizeof(SWORD));
    s_mix = (int16_t *)calloc(HANDY_AUDIO_BUFFER_LENGTH, sizeof(int16_t));
    if (s_frame == nullptr || gAudioBuffer == nullptr || s_mix == nullptr)
        return false;
    gPrimaryFrameBuffer = (UBYTE *)s_frame;
    gAudioEnabled = 1;
    gAudioBufferPointer = 0;
    lynx_rotation();
    return true;
}

void lynx_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad2;
    ULONG buttons = 0;
    if (pad1 & EMU_PORT_UP)
        buttons |= s_up;
    if (pad1 & EMU_PORT_DOWN)
        buttons |= s_down;
    if (pad1 & EMU_PORT_LEFT)
        buttons |= s_left;
    if (pad1 & EMU_PORT_RIGHT)
        buttons |= s_right;
    if (pad1 & EMU_PORT_A)
        buttons |= BUTTON_A;
    if (pad1 & EMU_PORT_B)
        buttons |= BUTTON_B;
    if (pad1 & EMU_PORT_START)
        buttons |= BUTTON_OPT2;
    if (pad1 & EMU_PORT_SELECT)
        buttons |= BUTTON_OPT1;
    s_lynx->SetButtonData(buttons);
    gAudioBufferPointer = 0;
    s_lynx->UpdateFrame(true);
    const size_t frames = gAudioBufferPointer / 2;
    if (s_channels == 2)
    {
        memcpy(s_mix, gAudioBuffer, frames * 2 * sizeof(int16_t));
    }
    else
    {
        for (size_t i = 0; i < frames; i++)
            s_mix[i] = (int16_t)(((int)gAudioBuffer[i * 2] + (int)gAudioBuffer[i * 2 + 1]) / 2);
    }
    s_mix_frames = frames;
}

void lynx_port_video(emu_port_video_t *video)
{
    video->pixels = (const uint8_t *)s_frame;
    video->palette = NULL;
    video->stride = HANDY_SCREEN_WIDTH * 2;
    video->width = s_width;
    video->height = s_height;
}

const int16_t *lynx_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t lynx_port_frame_us(void)
{
    return 13333u;
}

size_t lynx_port_save_state(uint8_t *buf, size_t cap)
{
    emu_mem_arm(buf, cap, 0);
    FILE *fp = fopen("state", "wb");
    if (fp == nullptr)
        return 0;
    const bool ok = s_lynx->ContextSave(fp) && !emu_mem_failed();
    fclose(fp);
    const size_t len = emu_mem_length();
    emu_mem_arm(nullptr, 0, 0);
    return ok ? len : 0;
}

bool lynx_port_load_state(const uint8_t *buf, size_t len)
{
    emu_mem_arm((uint8_t *)buf, len, len);
    FILE *fp = fopen("state", "rb");
    if (fp == nullptr)
        return false;
    const bool ok = s_lynx->ContextLoad(fp);
    fclose(fp);
    emu_mem_arm(nullptr, 0, 0);
    return ok;
}
