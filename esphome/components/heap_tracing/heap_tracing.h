#pragma once

#include "esphome/core/defines.h"

#ifdef USE_HEAP_TRACING_RECORDS

#include <cstddef>
#include <cstdint>

#include "sdkconfig.h"

namespace esphome::heap_tracing {

struct Owner {
  uint32_t address;
  uint32_t size;
  uint32_t callers[CONFIG_HEAP_TRACING_STACK_DEPTH];
};

bool started();
size_t snapshot_internal();
size_t owner_count();
const Owner *owner(size_t index);
size_t traced_count();
bool overflowed();

}

#endif
