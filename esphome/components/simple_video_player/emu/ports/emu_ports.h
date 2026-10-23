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
} emu_port_video_t;

bool nes_port_load(uint8_t *rom, size_t size, uint32_t rate, uint8_t channels);
void nes_port_frame(uint16_t pad1, uint16_t pad2);
void nes_port_video(emu_port_video_t *video);
const int16_t *nes_port_audio(size_t *frames);
uint32_t nes_port_frame_us(void);

#ifdef __cplusplus
}
#endif
