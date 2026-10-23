#include "emu_shim.h"

#include <string.h>

#include "esp_rom_crc.h"
#include "rg_system.h"

extern char _emu_bss_start;
extern char _emu_bss_end;
extern char _emu_common_start;
extern char _emu_common_end;

static emu_alloc_fn_t s_alloc;
static emu_realloc_fn_t s_realloc;
static emu_free_fn_t s_free;
static void *s_ctx;

void emu_bss_clear(void)
{
    memset(&_emu_bss_start, 0, (size_t)(&_emu_bss_end - &_emu_bss_start));
    memset(&_emu_common_start, 0, (size_t)(&_emu_common_end - &_emu_common_start));
}

void emu_set_allocator(emu_alloc_fn_t alloc, emu_realloc_fn_t realloc_fn, emu_free_fn_t free_fn, void *ctx)
{
    s_alloc = alloc;
    s_realloc = realloc_fn;
    s_free = free_fn;
    s_ctx = ctx;
}

void *emu_malloc(size_t size)
{
    return s_alloc != NULL ? s_alloc(s_ctx, size, false) : NULL;
}

void *emu_calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        return NULL;
    return s_alloc != NULL ? s_alloc(s_ctx, count * size, true) : NULL;
}

void *emu_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return emu_malloc(size);
    return s_realloc != NULL ? s_realloc(s_ctx, ptr, size) : NULL;
}

void emu_free(void *ptr)
{
    if (ptr != NULL && s_free != NULL)
        s_free(s_ctx, ptr);
}

void rg_system_log(int level, const char *context, const char *format, ...)
{
    (void)level;
    (void)context;
    (void)format;
}

uint32_t rg_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    return esp_rom_crc32_le(crc, buf, (uint32_t)len);
}

uint32_t rg_input_read_gamepad(void)
{
    return 0;
}
