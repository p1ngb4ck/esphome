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
typedef bool (*emu_host_frame_fn_t)(void *ctx, uint16_t *pad1, uint16_t *pad2);
typedef bool (*emu_link_send_fn_t)(void *ctx, uint8_t type, uint8_t a, uint16_t b);
typedef bool (*emu_link_recv_fn_t)(void *ctx, uint8_t *type, uint8_t *a, uint16_t *b, uint32_t wait_ms);

void emu_bss_clear(void);
void emu_set_allocator(emu_alloc_fn_t alloc, emu_realloc_fn_t realloc_fn, emu_free_fn_t free_fn, void *ctx);
void emu_set_host(emu_host_frame_fn_t frame, void *ctx);
bool emu_host_frame(uint16_t *pad1, uint16_t *pad2);
void emu_set_link(emu_link_send_fn_t send, emu_link_recv_fn_t recv, void *ctx);
bool emu_link_active(void);
bool emu_link_send(uint8_t type, uint8_t a, uint16_t b);
bool emu_link_recv(uint8_t *type, uint8_t *a, uint16_t *b, uint32_t wait_ms);

void *emu_malloc(size_t size);
void *emu_calloc(size_t count, size_t size);
void *emu_realloc(void *ptr, size_t size);
void emu_free(void *ptr);

#ifdef __cplusplus
}
#endif
