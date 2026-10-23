#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*emu_alloc_fn_t)(void *ctx, size_t size, bool zero);
typedef void *(*emu_realloc_fn_t)(void *ctx, void *ptr, size_t size);
typedef void (*emu_free_fn_t)(void *ctx, void *ptr);

void emu_bss_clear(void);
void emu_set_allocator(emu_alloc_fn_t alloc, emu_realloc_fn_t realloc_fn, emu_free_fn_t free_fn, void *ctx);

void *emu_malloc(size_t size);
void *emu_calloc(size_t count, size_t size);
void *emu_realloc(void *ptr, size_t size);
void emu_free(void *ptr);

#ifdef __cplusplus
}
#endif
