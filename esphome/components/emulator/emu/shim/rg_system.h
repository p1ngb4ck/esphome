#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { RG_LOG_PRINTF = 0 };

void rg_system_log(int level, const char *context, const char *format, ...) __attribute__((format(printf, 3, 4)));
uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len);
uint32_t rg_input_read_gamepad(void);

#ifdef __cplusplus
}
#endif
