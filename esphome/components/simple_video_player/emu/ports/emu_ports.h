#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EMU_PORT_UP = 1 << 0,
    EMU_PORT_DOWN = 1 << 1,
    EMU_PORT_LEFT = 1 << 2,
    EMU_PORT_RIGHT = 1 << 3,
    EMU_PORT_A = 1 << 4,
    EMU_PORT_B = 1 << 5,
    EMU_PORT_X = 1 << 6,
    EMU_PORT_Y = 1 << 7,
    EMU_PORT_L = 1 << 8,
    EMU_PORT_R = 1 << 9,
    EMU_PORT_START = 1 << 10,
    EMU_PORT_SELECT = 1 << 11,
};

typedef struct {
    const uint8_t *pixels;
    const uint16_t *palette;
    uint32_t stride;
    uint16_t width;
    uint16_t height;
    uint8_t buffers;
} emu_port_video_t;

bool emu_host_frame(uint16_t *pad1, uint16_t *pad2);
bool emu_link_active(void);
bool emu_link_send(uint8_t type, uint8_t a, uint16_t b);
bool emu_link_recv(uint8_t *type, uint8_t *a, uint16_t *b, uint32_t wait_ms);

bool nes_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels);
void nes_port_frame(uint16_t pad1, uint16_t pad2);
void nes_port_video(emu_port_video_t *video);
const int16_t *nes_port_audio(size_t *frames);
uint32_t nes_port_frame_us(void);
uint8_t *nes_port_sram(size_t *size);
size_t nes_port_save_state(uint8_t *buf, size_t cap);
bool nes_port_load_state(const uint8_t *buf, size_t len);

bool gb_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels);
void gb_port_frame(uint16_t pad1, uint16_t pad2);
void gb_port_video(emu_port_video_t *video);
const int16_t *gb_port_audio(size_t *frames);
uint32_t gb_port_frame_us(void);
uint8_t *gb_port_sram(size_t *size);
size_t gb_port_save_state(uint8_t *buf, size_t cap);
bool gb_port_load_state(const uint8_t *buf, size_t len);

bool sms_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels, int console);
void sms_port_frame(uint16_t pad1, uint16_t pad2);
void sms_port_video(emu_port_video_t *video);
const int16_t *sms_port_audio(size_t *frames);
uint32_t sms_port_frame_us(void);
uint8_t *sms_port_sram(size_t *size);
size_t sms_port_save_state(uint8_t *buf, size_t cap);
bool sms_port_load_state(const uint8_t *buf, size_t len);

bool pce_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels);
void pce_port_frame(uint16_t pad1, uint16_t pad2);
void pce_port_video(emu_port_video_t *video);
const int16_t *pce_port_audio(size_t *frames);
uint32_t pce_port_frame_us(void);
size_t pce_port_save_state(uint8_t *buf, size_t cap);
bool pce_port_load_state(const uint8_t *buf, size_t len);

bool lynx_port_load(uint8_t *rom, size_t size, uint8_t channels);
void lynx_port_frame(uint16_t pad1, uint16_t pad2);
void lynx_port_video(emu_port_video_t *video);
const int16_t *lynx_port_audio(size_t *frames);
uint32_t lynx_port_frame_us(void);
size_t lynx_port_save_state(uint8_t *buf, size_t cap);
bool lynx_port_load_state(const uint8_t *buf, size_t len);

bool snes_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels);
void snes_port_frame(uint16_t pad1, uint16_t pad2);
void snes_port_mix_begin(void);
void snes_port_mix_run(void);
void snes_port_video(emu_port_video_t *video);
const int16_t *snes_port_audio(size_t *frames);
uint32_t snes_port_frame_us(void);
uint8_t *snes_port_sram(size_t *size);
size_t snes_port_save_state(uint8_t *buf, size_t cap);
bool snes_port_load_state(const uint8_t *buf, size_t len);

bool md_port_load(uint8_t *rom, size_t size, uint8_t channels);
void md_port_frame(uint16_t pad1, uint16_t pad2);
void md_port_video(emu_port_video_t *video);
const int16_t *md_port_audio(size_t *frames);
uint32_t md_port_frame_us(void);
size_t md_port_save_state(uint8_t *buf, size_t cap);
bool md_port_load_state(const uint8_t *buf, size_t len);

bool gw_port_load(uint8_t *rom, size_t size, uint8_t channels);
void gw_port_frame(uint16_t pad1, uint16_t pad2);
void gw_port_video(emu_port_video_t *video);
const int16_t *gw_port_audio(size_t *frames);
uint32_t gw_port_frame_us(void);
size_t gw_port_save_state(uint8_t *buf, size_t cap);
bool gw_port_load_state(const uint8_t *buf, size_t len);

bool msx_port_load(uint8_t *rom, size_t size, const char *path, uint32_t rate, uint8_t channels);
void msx_port_add_file(const char *name, const uint8_t *data, size_t size);
void msx_port_frame(uint16_t pad1, uint16_t pad2);
void msx_port_key(uint16_t usage, bool pressed);
void msx_port_video(emu_port_video_t *video);
const int16_t *msx_port_audio(size_t *frames);
uint32_t msx_port_frame_us(void);
size_t msx_port_save_state(uint8_t *buf, size_t cap);
bool msx_port_load_state(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
