#pragma once

#include <new>

#include "emu_shim.h"

template<class T, class... Args> T *emu_new(Args &&...args)
{
    void *mem = emu_malloc(sizeof(T));
    return mem != nullptr ? new (mem) T(static_cast<Args &&>(args)...) : nullptr;
}

template<class T> void emu_delete(T *ptr)
{
    if (ptr != nullptr)
    {
        ptr->~T();
        emu_free(ptr);
    }
}
