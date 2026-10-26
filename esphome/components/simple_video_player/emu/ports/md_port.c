#include "emu_ports.h"

#include "gwenesis.h"

#define MD_AUDIO_RATE 53267
#define MD_AUDIO_BUFFER_LENGTH (MD_AUDIO_RATE / 60 + 1)

extern unsigned char *VRAM;
extern int zclk;
extern unsigned char gwenesis_vdp_regs[];
extern unsigned short gwenesis_vdp_status;
extern unsigned short *CRAM565;
extern unsigned char *ZRAM;
extern unsigned char *SAT_CACHE;
extern unsigned int *sin_tab;
extern int screen_width, screen_height;
extern int hint_pending;

int system_clock;
int scan_line;
int16_t gwenesis_sn76489_buffer[MD_AUDIO_BUFFER_LENGTH];
int sn76489_index;
int sn76489_clock;
int16_t gwenesis_ym2612_buffer[MD_AUDIO_BUFFER_LENGTH];
int ym2612_index;
int ym2612_clock;

typedef struct
{
    char key[28];
    uint32_t length;
} md_svar_t;

static uint8_t *s_buffer;
static uint8_t *s_frame;
static int16_t *s_mix;
static size_t s_mix_frames;
static uint8_t s_channels;
static uint16_t s_pad;
static uint8_t *s_state;
static size_t s_state_cap;
static size_t s_state_len;
static size_t s_state_pos;
static int s_state_errors;

SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    (void)fileName;
    return (SaveState *)1;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    (void)fileName;
    return (SaveState *)1;
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName, void *buffer, int length)
{
    (void)state;
    const size_t initial = s_state_pos;
    bool from_start = false;
    while (!from_start || s_state_pos < initial)
    {
        if (s_state_pos + sizeof(md_svar_t) > s_state_len)
        {
            if (!from_start)
            {
                s_state_pos = 0;
                from_start = true;
                continue;
            }
            break;
        }
        md_svar_t var;
        memcpy(&var, s_state + s_state_pos, sizeof(var));
        s_state_pos += sizeof(var);
        if (strncmp(var.key, tagName, sizeof(var.key)) == 0)
        {
            size_t n = var.length < (uint32_t)length ? var.length : (size_t)length;
            if (s_state_pos + n > s_state_len)
                break;
            memcpy(buffer, s_state + s_state_pos, n);
            s_state_pos += var.length;
            return;
        }
        s_state_pos += var.length;
    }
    s_state_errors++;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName, void *buffer, int length)
{
    (void)state;
    md_svar_t var = {{0}, (uint32_t)length};
    strncpy(var.key, tagName, sizeof(var.key) - 1);
    if (s_state_len + sizeof(var) + (size_t)length > s_state_cap)
    {
        s_state_errors++;
        return;
    }
    memcpy(s_state + s_state_len, &var, sizeof(var));
    s_state_len += sizeof(var);
    memcpy(s_state + s_state_len, buffer, (size_t)length);
    s_state_len += (size_t)length;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof(int));
    return value;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof(int));
}

void gwenesis_io_get_buttons()
{
}

bool md_port_load(uint8_t *rom, size_t size, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    m68k_ptr = emu_calloc_fast(1, sizeof(m68ki_cpu_core));
    if (m68k_ptr == NULL)
        return false;
    ZRAM = emu_calloc_hot(1, MAX_Z80_RAM_SIZE);
    sin_tab = emu_calloc_hot(1024, sizeof(unsigned int));
    SAT_CACHE = emu_calloc_hot(1, SAT_CACHE_MAX_SIZE);
    CRAM565 = emu_calloc_hot(CRAM_MAX_SIZE * 4, sizeof(unsigned short));
    if (ZRAM == NULL || sin_tab == NULL || SAT_CACHE == NULL || CRAM565 == NULL)
        return false;
    s_buffer = calloc(1, 320 * 241);
    VRAM = calloc(1, VRAM_MAX_SIZE);
    s_mix = calloc(MD_AUDIO_BUFFER_LENGTH, sizeof(int16_t));
    if (s_buffer == NULL || VRAM == NULL || s_mix == NULL)
        return false;
    s_frame = s_buffer + 160;
    load_cartridge(rom, size);
    power_on();
    reset_emulation();
    s_pad = 0;
    return true;
}

static void md_buttons(uint16_t pad)
{
    static const uint16_t map[8] = {EMU_PORT_UP, EMU_PORT_DOWN, EMU_PORT_LEFT, EMU_PORT_RIGHT,
                                    EMU_PORT_Y,  EMU_PORT_B,    EMU_PORT_A,    EMU_PORT_START};
    for (int i = 0; i < 8; i++)
    {
        if ((pad & map[i]) == (s_pad & map[i]))
            continue;
        if (pad & map[i])
            gwenesis_io_pad_press_button(0, i);
        else
            gwenesis_io_pad_release_button(0, i);
    }
    s_pad = pad;
}

void md_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad2;
    md_buttons(pad1);
    const int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    int hint_counter = gwenesis_vdp_regs[10];
    screen_width = REG12_MODE_H40 ? 320 : 256;
    screen_height = REG1_PAL ? 240 : 224;
    gwenesis_vdp_set_buffer((unsigned short *)s_frame);
    gwenesis_vdp_render_config();
    system_clock = 0;
    zclk = 0;
    ym2612_clock = 0;
    ym2612_index = 0;
    sn76489_clock = 0x1000000;
    sn76489_index = 0;
    scan_line = 0;
    while (scan_line < lines_per_frame)
    {
        m68k_run(system_clock + VDP_CYCLES_PER_LINE);
        z80_run(system_clock + VDP_CYCLES_PER_LINE);
        if (GWENESIS_AUDIO_ACCURATE == 0)
        {
            gwenesis_SN76489_run(system_clock + VDP_CYCLES_PER_LINE);
            ym2612_run(system_clock + VDP_CYCLES_PER_LINE);
        }
        if (scan_line < screen_height)
            gwenesis_vdp_render_line(scan_line);
        if ((scan_line == 0) || (scan_line > screen_height))
            hint_counter = REG10_LINE_COUNTER;
        if (--hint_counter < 0)
        {
            if ((REG0_LINE_INTERRUPT != 0) && (scan_line <= screen_height))
            {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
            }
            hint_counter = REG10_LINE_COUNTER;
        }
        scan_line++;
        if (scan_line == screen_height)
        {
            if (REG1_VBLANK_INTERRUPT != 0)
            {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            z80_irq_line(1);
        }
        if (scan_line == screen_height + 1)
            z80_irq_line(0);
        system_clock += VDP_CYCLES_PER_LINE;
    }
    if (GWENESIS_AUDIO_ACCURATE == 1)
    {
        gwenesis_SN76489_run(system_clock);
        ym2612_run(system_clock);
    }
    m68k.cycles -= system_clock;
    const size_t frames = (size_t)(ym2612_index > 0 ? ym2612_index : 0) / 2;
    if (s_channels == 2)
    {
        memcpy(s_mix, gwenesis_ym2612_buffer, frames * 2 * sizeof(int16_t));
    }
    else
    {
        for (size_t i = 0; i < frames; i++)
            s_mix[i] = (int16_t)(((int)gwenesis_ym2612_buffer[i * 2] + (int)gwenesis_ym2612_buffer[i * 2 + 1]) / 2);
    }
    s_mix_frames = frames;
}

void md_port_video(emu_port_video_t *video)
{
    video->pixels = s_frame;
    video->palette = CRAM565;
    video->stride = 320;
    video->width = (uint16_t)screen_width;
    video->height = (uint16_t)screen_height;
}

const int16_t *md_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t md_port_frame_us(void)
{
    return REG1_PAL ? 20000u : 16667u;
}

size_t md_port_save_state(uint8_t *buf, size_t cap)
{
    s_state = buf;
    s_state_cap = cap;
    s_state_len = 0;
    s_state_pos = 0;
    s_state_errors = 0;
    gwenesis_save_state();
    return s_state_errors == 0 ? s_state_len : 0;
}

bool md_port_load_state(const uint8_t *buf, size_t len)
{
    s_state = (uint8_t *)buf;
    s_state_cap = len;
    s_state_len = len;
    s_state_pos = 0;
    s_state_errors = 0;
    gwenesis_load_state();
    return s_state_errors == 0;
}
