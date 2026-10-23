#include "emu_mem.h"

#include <string.h>

typedef struct
{
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t pos;
    bool open;
    bool failed;
} emu_mem_t;

static emu_mem_t s_mem;

void emu_mem_arm(uint8_t *buf, size_t cap, size_t len)
{
    s_mem.buf = buf;
    s_mem.cap = cap;
    s_mem.len = len;
    s_mem.pos = 0;
    s_mem.open = false;
    s_mem.failed = false;
}

size_t emu_mem_length(void)
{
    return s_mem.len;
}

bool emu_mem_failed(void)
{
    return s_mem.failed;
}

FILE *emu_mem_fopen(const char *name, const char *mode)
{
    (void)name;
    if (s_mem.buf == NULL || s_mem.open)
        return NULL;
    s_mem.open = true;
    s_mem.pos = 0;
    if (mode[0] == 'w')
        s_mem.len = 0;
    return (FILE *)&s_mem;
}

int emu_mem_fclose(FILE *fp)
{
    if (fp != (FILE *)&s_mem)
        return EOF;
    s_mem.open = false;
    return 0;
}

size_t emu_mem_fread(void *ptr, size_t size, size_t count, FILE *fp)
{
    if (fp != (FILE *)&s_mem || size == 0)
        return 0;
    const size_t avail = s_mem.pos < s_mem.len ? s_mem.len - s_mem.pos : 0;
    size_t items = avail / size;
    if (items > count)
        items = count;
    memcpy(ptr, s_mem.buf + s_mem.pos, items * size);
    s_mem.pos += items * size;
    return items;
}

size_t emu_mem_fwrite(const void *ptr, size_t size, size_t count, FILE *fp)
{
    if (fp != (FILE *)&s_mem || size == 0)
        return 0;
    const size_t room = s_mem.pos < s_mem.cap ? s_mem.cap - s_mem.pos : 0;
    size_t items = room / size;
    if (items > count)
        items = count;
    memcpy(s_mem.buf + s_mem.pos, ptr, items * size);
    s_mem.pos += items * size;
    if (s_mem.pos > s_mem.len)
        s_mem.len = s_mem.pos;
    if (items < count)
        s_mem.failed = true;
    return items;
}

int emu_mem_fseek(FILE *fp, long offset, int whence)
{
    if (fp != (FILE *)&s_mem)
        return -1;
    long base = 0;
    if (whence == SEEK_CUR)
        base = (long)s_mem.pos;
    else if (whence == SEEK_END)
        base = (long)s_mem.len;
    const long target = base + offset;
    if (target < 0 || (size_t)target > s_mem.cap)
        return -1;
    s_mem.pos = (size_t)target;
    return 0;
}

long emu_mem_ftell(FILE *fp)
{
    if (fp != (FILE *)&s_mem)
        return -1;
    return (long)s_mem.pos;
}
