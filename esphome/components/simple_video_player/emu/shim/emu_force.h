#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

void *emu_malloc(size_t size);
void *emu_calloc(size_t count, size_t size);
void *emu_realloc(void *ptr, size_t size);
void emu_free(void *ptr);

#ifdef __cplusplus
}
#endif

#define malloc(size) emu_malloc(size)
#define calloc(count, size) emu_calloc(count, size)
#define realloc(ptr, size) emu_realloc(ptr, size)
#define free(ptr) emu_free(ptr)

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
