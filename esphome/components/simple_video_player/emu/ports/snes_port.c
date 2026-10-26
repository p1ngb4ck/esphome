#include "emu_ports.h"

#include "emu_mem.h"
#include "snes9x.h"

static uint16_t *s_frame;
static uint16_t *s_frames[2];
static uint8_t s_back;
static int16_t *s_stereo;
static int16_t *s_stereo_bufs[2];
static uint8_t s_mix_target;
static int16_t *s_mix;
static const int16_t *s_out;
static size_t s_mix_frames;
static size_t s_next_frames;
static size_t s_req_frames;
static size_t s_mixed_frames;
static uint8_t s_channels;
static uint32_t s_rate;
static uint32_t s_acc;
static uint16_t s_pad;

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = (uint8_t *)s_frame;
    GFX.SubScreen = malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED);
    GFX.ZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    GFX.SubZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    (void)port;
    if (port != 0)
        return 0;
    uint32_t joypad = 0;
    if (s_pad & EMU_PORT_UP)
        joypad |= SNES_UP_MASK;
    if (s_pad & EMU_PORT_DOWN)
        joypad |= SNES_DOWN_MASK;
    if (s_pad & EMU_PORT_LEFT)
        joypad |= SNES_LEFT_MASK;
    if (s_pad & EMU_PORT_RIGHT)
        joypad |= SNES_RIGHT_MASK;
    if (s_pad & EMU_PORT_A)
        joypad |= SNES_A_MASK;
    if (s_pad & EMU_PORT_B)
        joypad |= SNES_B_MASK;
    if (s_pad & EMU_PORT_X)
        joypad |= SNES_X_MASK;
    if (s_pad & EMU_PORT_Y)
        joypad |= SNES_Y_MASK;
    if (s_pad & EMU_PORT_L)
        joypad |= SNES_TL_MASK;
    if (s_pad & EMU_PORT_R)
        joypad |= SNES_TR_MASK;
    if (s_pad & EMU_PORT_START)
        joypad |= SNES_START_MASK;
    if (s_pad & EMU_PORT_SELECT)
        joypad |= SNES_SELECT_MASK;
    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

bool snes_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels)
{
    uint8_t *vram = (uint8_t *)emu_calloc_hot(1, VRAM_SIZE);
    if (vram == NULL || !S9xInitHot())
        return false;
    Memory.VRAM = vram;
    s_channels = channels > 1 ? 2 : 1;
    s_rate = rate;
    s_frames[0] = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    s_frames[1] = calloc(SNES_WIDTH * SNES_HEIGHT_EXTENDED, sizeof(uint16_t));
    s_back = 0;
    s_frame = s_frames[0];
    s_mix_frames = 0;
    s_next_frames = 0;
    s_req_frames = 0;
    s_mixed_frames = 0;
    s_stereo_bufs[0] = calloc((rate / 50 + 1) * 2, sizeof(int16_t));
    s_stereo_bufs[1] = calloc((rate / 50 + 1) * 2, sizeof(int16_t));
    s_mix_target = 0;
    s_stereo = s_stereo_bufs[0];
    s_mix = s_channels == 2 ? NULL : calloc((rate / 50 + 1), sizeof(int16_t));
    s_out = s_stereo_bufs[1];
    if (s_frames[0] == NULL || s_frames[1] == NULL || s_stereo_bufs[0] == NULL || s_stereo_bufs[1] == NULL ||
        (s_channels != 2 && s_mix == NULL))
        return false;
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = rate;
    Settings.SoundInputRate = rate;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = false;
    Memory.ROM = rom;
    Memory.ROM_AllocSize = size;
    if (!S9xInitDisplay() || !S9xInitMemory() || !S9xInitAPU() || !S9xInitSound(0, 0) || !S9xInitGFX())
        return false;
    if (!LoadROM(NULL))
        return false;
    S9xSetPlaybackRate(Settings.SoundPlaybackRate);
    return true;
}

void snes_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad2;
    s_pad = pad1;
    IPPU.RenderThisFrame = true;
    GFX.Screen = (uint8_t *)s_frames[s_back];
    S9xMainLoop();
    s_frame = s_frames[s_back];
    s_back ^= 1;
    const uint32_t fps = Memory.ROMFramesPerSecond > 0 ? (uint32_t)Memory.ROMFramesPerSecond : 60;
    s_acc += s_rate;
    s_next_frames = s_acc / fps;
    s_acc %= fps;
}

void snes_port_mix_begin(void)
{
    const size_t frames = s_mixed_frames;
    const int16_t *mixed = s_stereo_bufs[s_mix_target];
    if (s_channels == 2)
    {
        s_out = mixed;
    }
    else
    {
        for (size_t i = 0; i < frames; i++)
            s_mix[i] = (int16_t)(((int)mixed[i * 2] + (int)mixed[i * 2 + 1]) / 2);
        s_out = s_mix;
    }
    s_mix_frames = frames;
    s_mix_target ^= 1;
    s_stereo = s_stereo_bufs[s_mix_target];
    s_req_frames = s_next_frames;
}

void snes_port_mix_run(void)
{
    const size_t frames = s_req_frames;
    for (size_t done = 0; done < frames;)
    {
        const size_t n = frames - done < SOUND_BUFFER_SIZE / 2 ? frames - done : SOUND_BUFFER_SIZE / 2;
        S9xMixSamples(s_stereo + done * 2, (int32_t)(n * 2));
        done += n;
    }
    s_mixed_frames = frames;
}

void snes_port_video(emu_port_video_t *video)
{
    video->pixels = (const uint8_t *)s_frame;
    video->palette = NULL;
    video->stride = SNES_WIDTH * 2;
    video->buffers = 2;
    video->width = SNES_WIDTH;
    video->height = (uint16_t)(IPPU.RenderedScreenHeight > 0 && IPPU.RenderedScreenHeight <= SNES_HEIGHT_EXTENDED
                                   ? IPPU.RenderedScreenHeight
                                   : SNES_HEIGHT);
}

const int16_t *snes_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_out;
}

uint32_t snes_port_frame_us(void)
{
    return Memory.ROMFramesPerSecond == 50 ? 20000u : 16667u;
}

uint8_t *snes_port_sram(size_t *size)
{
    if (Memory.SRAM == NULL || Memory.SRAMSize == 0)
        return NULL;
    size_t bytes = (size_t)1024 << Memory.SRAMSize;
    if (bytes > SRAM_SIZE)
        bytes = SRAM_SIZE;
    *size = bytes;
    return Memory.SRAM;
}

size_t snes_port_save_state(uint8_t *buf, size_t cap)
{
    emu_mem_arm(buf, cap, 0);
    const bool ok = S9xSaveState("state") && !emu_mem_failed();
    const size_t len = emu_mem_length();
    emu_mem_arm(NULL, 0, 0);
    return ok ? len : 0;
}

bool snes_port_load_state(const uint8_t *buf, size_t len)
{
    emu_mem_arm((uint8_t *)buf, len, len);
    const bool ok = S9xLoadState("state");
    emu_mem_arm(NULL, 0, 0);
    return ok;
}
