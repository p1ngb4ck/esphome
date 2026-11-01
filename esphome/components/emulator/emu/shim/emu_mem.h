#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void emu_mem_arm(uint8_t *buf, size_t cap, size_t len);
size_t emu_mem_length(void);
bool emu_mem_failed(void);

FILE *emu_mem_fopen(const char *name, const char *mode);
int emu_mem_fclose(FILE *fp);
size_t emu_mem_fread(void *ptr, size_t size, size_t count, FILE *fp);
size_t emu_mem_fwrite(const void *ptr, size_t size, size_t count, FILE *fp);
int emu_mem_fseek(FILE *fp, long offset, int whence);
long emu_mem_ftell(FILE *fp);

#ifdef __cplusplus
}
#endif
