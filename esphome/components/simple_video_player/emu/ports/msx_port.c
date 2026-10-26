#include "emu_ports.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#define BPS16
#define BPP16
#define UNIX
#define LSB_FIRST
#define NARROW
#define GenericSetVideo SetVideo
#define WIDTH 256
#define HEIGHT 228

void PutImage(void);

static uint16_t BPal[256];
static uint16_t XPal[80];
static uint16_t XPal0;
static uint16_t *XBuf;

#include "fmsx.h"
#include "carts_sha.h"

#define MSX_MAX_FILES 16

typedef struct
{
    const char *name;
    const uint8_t *data;
    size_t size;
} msx_file_t;

int UseSound, UseZoom, SyncFreq, UseEffects;
int ARGC;
char **ARGV;
const char *Title = "fMSX";
const char *Disks[2][MAXDISKS + 1];

static msx_file_t s_files[MSX_MAX_FILES];
static size_t s_file_count;
static Image s_screen;
static uint16_t *s_frame;
static int16_t *s_mix;
static size_t s_mix_frames;
static size_t s_mix_cap;
static uint8_t s_channels;
static uint32_t s_rate;
static uint16_t s_pad;
static volatile uint32_t s_keys[8];

static const uint8_t HID_MSX[0x63] = {
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E', [0x09] = 'F', [0x0A] = 'G',
    [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J', [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N',
    [0x12] = 'O', [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T', [0x18] = 'U',
    [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y', [0x1D] = 'Z', [0x1E] = '1', [0x1F] = '2',
    [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9',
    [0x27] = '0', [0x28] = KBD_ENTER, [0x29] = KBD_ESCAPE, [0x2A] = KBD_BS, [0x2B] = KBD_TAB,
    [0x2C] = KBD_SPACE, [0x2D] = '-', [0x2E] = '=', [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/', [0x39] = KBD_CAPSLOCK,
    [0x3A] = KBD_F1, [0x3B] = KBD_F2, [0x3C] = KBD_F3, [0x3D] = KBD_F4, [0x3E] = KBD_F5, [0x48] = KBD_STOP,
    [0x49] = KBD_INSERT, [0x4A] = KBD_HOME, [0x4C] = KBD_DELETE, [0x4D] = KBD_SELECT, [0x4F] = KBD_RIGHT,
    [0x50] = KBD_LEFT, [0x51] = KBD_DOWN, [0x52] = KBD_UP, [0x59] = KBD_NUMPAD1, [0x5A] = KBD_NUMPAD2,
    [0x5B] = KBD_NUMPAD3, [0x5C] = KBD_NUMPAD4, [0x5D] = KBD_NUMPAD5, [0x5E] = KBD_NUMPAD6, [0x5F] = KBD_NUMPAD7,
    [0x60] = KBD_NUMPAD8, [0x61] = KBD_NUMPAD9, [0x62] = KBD_NUMPAD0,
};

static uint8_t hid_msx(uint16_t usage)
{
    if (usage < sizeof(HID_MSX))
        return HID_MSX[usage];
    switch (usage)
    {
        case 0xE0:
        case 0xE4:
            return KBD_CONTROL;
        case 0xE1:
        case 0xE5:
            return KBD_SHIFT;
        case 0xE2:
            return KBD_GRAPH;
        case 0xE6:
            return KBD_COUNTRY;
        default:
            return 0;
    }
}

static const msx_file_t *msx_file(const char *path)
{
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    for (size_t i = 0; i < s_file_count; i++)
    {
        if (strcasecmp(name, s_files[i].name) == 0)
            return &s_files[i];
    }
    return NULL;
}

void msx_port_add_file(const char *name, const uint8_t *data, size_t size)
{
    if (s_file_count < MSX_MAX_FILES)
        s_files[s_file_count++] = (msx_file_t){name, data, size};
}

FILE *msx_fopen(const char *path, const char *mode)
{
    if (mode[0] != 'r' || strchr(mode, '+') != NULL)
        return NULL;
    const msx_file_t *file = msx_file(path);
    if (file == NULL)
        return NULL;
    return fmemopen((void *)file->data, file->size, "rb");
}

int msx_stat(const char *path, struct stat *sbuf)
{
    const msx_file_t *file = msx_file(path);
    if (file == NULL)
        return -1;
    if (sbuf != NULL)
    {
        memset(sbuf, 0, sizeof(*sbuf));
        sbuf->st_mode = S_IFREG;
        sbuf->st_size = (off_t)file->size;
    }
    return 0;
}

DIR *msx_opendir(const char *path)
{
    (void)path;
    return NULL;
}

struct dirent *msx_readdir(DIR *dir)
{
    (void)dir;
    return NULL;
}

int msx_unlink(const char *path)
{
    (void)path;
    return -1;
}

int msx_rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return -1;
}

int msx_chdir(const char *path)
{
    (void)path;
    return 0;
}

char *msx_getcwd(char *buf, size_t size)
{
    if (buf == NULL || size < 2)
        return NULL;
    strcpy(buf, "/");
    return buf;
}

int InitMachine(void)
{
    s_screen = (Image){.Data = s_frame, .W = WIDTH, .H = HEIGHT, .L = WIDTH, .D = 16};
    XBuf = s_frame;
    SetScreenDepth(16);
    SetVideo(&s_screen, 0, 0, WIDTH, HEIGHT);
    for (int J = 0; J < 80; J++)
        SetColor(J, 0, 0, 0);
    for (int J = 0; J < 256; J++)
        BPal[J] = PIXEL(((J >> 2) & 0x07) * 255 / 7, ((J >> 5) & 0x07) * 255 / 7, (J & 0x03) * 255 / 3);
    InitSound(s_rate, 150);
    SetChannels(64, 0xFFFFFFFF);
    return 1;
}

void TrashMachine(void)
{
    TrashSound();
}

void SetColor(byte N, byte R, byte G, byte B)
{
    const uint16_t color = PIXEL(R, G, B);
    if (N)
        XPal[N] = color;
    else
        XPal0 = color;
}

void PutImage(void)
{
}

unsigned int Joystick(void)
{
    unsigned int joy = 0;
    if (s_pad & EMU_PORT_UP)
        joy |= JST_UP;
    if (s_pad & EMU_PORT_DOWN)
        joy |= JST_DOWN;
    if (s_pad & EMU_PORT_LEFT)
        joy |= JST_LEFT;
    if (s_pad & EMU_PORT_RIGHT)
        joy |= JST_RIGHT;
    if (s_pad & EMU_PORT_A)
        joy |= JST_FIREA;
    if (s_pad & EMU_PORT_B)
        joy |= JST_FIREB;
    return joy;
}

void Keyboard(void)
{
    uint16_t pad2 = 0;
    if (!emu_host_frame(&s_pad, &pad2))
    {
        ExitNow = 1;
        return;
    }
    s_mix_frames = 0;
    for (int i = 0; i < 16; i++)
        KeyState[i] = 0xFF;
    for (int word = 0; word < 8; word++)
    {
        uint32_t bits = s_keys[word];
        while (bits != 0)
        {
            const int bit = __builtin_ctz(bits);
            bits &= bits - 1;
            const uint8_t key = hid_msx((uint16_t)(word * 32 + bit));
            if (key != 0)
                KBD_SET(key);
        }
    }
    if (s_pad & EMU_PORT_UP)
        KBD_SET(KBD_UP);
    if (s_pad & EMU_PORT_DOWN)
        KBD_SET(KBD_DOWN);
    if (s_pad & EMU_PORT_LEFT)
        KBD_SET(KBD_LEFT);
    if (s_pad & EMU_PORT_RIGHT)
        KBD_SET(KBD_RIGHT);
    if (s_pad & EMU_PORT_A)
        KBD_SET(KBD_SPACE);
    if (s_pad & EMU_PORT_START)
        KBD_SET(KBD_ENTER);
    if (s_pad & EMU_PORT_SELECT)
        KBD_SET(KBD_ESCAPE);
}

unsigned int Mouse(byte N)
{
    (void)N;
    return 0;
}

int ShowVideo(void)
{
    return 1;
}

unsigned int GetJoystick(void)
{
    return 0;
}

unsigned int GetMouse(void)
{
    return 0;
}

unsigned int GetKey(void)
{
    return 0;
}

unsigned int WaitKey(void)
{
    return 0;
}

unsigned int WaitKeyOrMouse(void)
{
    return 0;
}

unsigned int InitAudio(unsigned int Rate, unsigned int Latency)
{
    (void)Rate;
    (void)Latency;
    return s_rate;
}

void TrashAudio(void)
{
}

unsigned int GetFreeAudio(void)
{
    return 1024;
}

void PlayAllSound(int uSec)
{
    RenderAndPlayAudio(2 * (unsigned int)uSec * s_rate / 1000000u);
}

unsigned int WriteAudio(sample *Data, unsigned int Length)
{
    const size_t frames = Length / 2;
    for (size_t i = 0; i < frames && s_mix_frames < s_mix_cap; i++, s_mix_frames++)
    {
        if (s_channels == 2)
        {
            s_mix[s_mix_frames * 2] = Data[i * 2];
            s_mix[s_mix_frames * 2 + 1] = Data[i * 2 + 1];
        }
        else
        {
            s_mix[s_mix_frames] = (int16_t)(((int)Data[i * 2] + (int)Data[i * 2 + 1]) / 2);
        }
    }
    return Length;
}

bool msx_port_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels)
{
    s_channels = channels > 1 ? 2 : 1;
    s_rate = rate;
    s_mix_cap = rate / 25 + 1;
    memset(&msx_cpu, 0, sizeof(msx_cpu));
    s_frame = calloc(WIDTH * HEIGHT, sizeof(uint16_t));
    s_mix = calloc(s_mix_cap * s_channels, sizeof(int16_t));
    if (s_frame == NULL || s_mix == NULL)
        return false;
    const char *dot = strrchr(path, '.');
    const bool disk = dot != NULL && strcasecmp(dot, ".dsk") == 0;
    msx_port_add_file(disk ? "GAME.DSK" : "GAME.ROM", rom, size);
    msx_port_add_file("CARTS.SHA", carts_sha, sizeof(carts_sha));
    return true;
}

void msx_port_frame(uint16_t pad1, uint16_t pad2)
{
    (void)pad1;
    (void)pad2;
    static const char *argv_rom[] = {"fmsx", "-ram", "2", "-vram", "2", "-skip", "0", "-home", "/", "-joy", "1",
                                     "GAME.ROM", NULL};
    static const char *argv_disk[] = {"fmsx", "-ram", "2", "-vram", "2", "-skip", "0", "-home", "/", "-joy", "1",
                                      "-diska", "GAME.DSK", NULL};
    const bool disk = msx_file("GAME.DSK") != NULL;
    const char **argv = disk ? argv_disk : argv_rom;
    int argc = 0;
    while (argv[argc] != NULL)
        argc++;
    fmsx_main(argc, (char **)argv);
}

void msx_port_key(uint16_t usage, bool pressed)
{
    if (usage >= 256)
        return;
    const uint32_t mask = 1u << (usage & 31);
    if (pressed)
        __atomic_fetch_or(&s_keys[usage >> 5], mask, __ATOMIC_RELAXED);
    else
        __atomic_fetch_and(&s_keys[usage >> 5], ~mask, __ATOMIC_RELAXED);
}

void msx_port_video(emu_port_video_t *video)
{
    video->pixels = (const uint8_t *)s_frame;
    video->palette = NULL;
    video->stride = WIDTH * 2;
    video->width = WIDTH;
    video->height = HEIGHT;
}

const int16_t *msx_port_audio(size_t *frames)
{
    *frames = s_mix_frames;
    return s_mix;
}

uint32_t msx_port_frame_us(void)
{
    return 16667u;
}

size_t msx_port_save_state(uint8_t *buf, size_t cap)
{
    return SaveState(buf, (unsigned int)cap);
}

bool msx_port_load_state(const uint8_t *buf, size_t len)
{
    return LoadState((unsigned char *)buf, (unsigned int)len) != 0;
}
