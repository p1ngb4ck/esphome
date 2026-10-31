#include "esphome/core/defines.h"

#ifdef USE_ESP32_P4_SMALL_ALLOC_RETENTION

#include <cstddef>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

namespace esphome::esp32 {}

#ifdef CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH
#define SMALL_ALLOC_ATTR
#else
#define SMALL_ALLOC_ATTR IRAM_ATTR
#endif

#if defined(CONFIG_SPIRAM_USE_MALLOC) && CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL < 128
static constexpr size_t SMALL_ALLOC_MAX = CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
#else
static constexpr size_t SMALL_ALLOC_MAX = 128;
#endif

extern "C" void *__real_heap_caps_malloc_default(size_t size);

extern "C" SMALL_ALLOC_ATTR void *__wrap_heap_caps_malloc_default(size_t size) {
  if (size != 0 && size <= SMALL_ALLOC_MAX) {
    void *ptr = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_RETENTION | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
    if (ptr != nullptr)
      return ptr;
  }
  return __real_heap_caps_malloc_default(size);
}

#endif
