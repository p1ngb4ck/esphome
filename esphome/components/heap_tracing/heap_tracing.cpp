#include "heap_tracing.h"

#ifdef USE_HEAP_TRACING_RECORDS

#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_memory_utils.h"

namespace esphome::heap_tracing {

static heap_trace_record_t *s_records = nullptr;
static bool s_started = false;
static bool s_snapped = false;
static Owner *s_owners = nullptr;
static size_t s_owner_count = 0;
static size_t s_traced = 0;
static bool s_overflowed = false;

__attribute__((constructor(101))) static void heap_tracing_start() {
  s_records = static_cast<heap_trace_record_t *>(
      heap_caps_calloc(USE_HEAP_TRACING_RECORDS, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_records == nullptr)
    return;
  if (heap_trace_init_standalone(s_records, USE_HEAP_TRACING_RECORDS) != ESP_OK)
    return;
  s_started = heap_trace_start(HEAP_TRACE_LEAKS) == ESP_OK;
}

bool started() { return s_started; }

size_t snapshot_internal() {
  if (!s_started || s_snapped)
    return s_owner_count;
  heap_trace_stop();
  s_snapped = true;
  heap_trace_summary_t summary{};
  if (heap_trace_summary(&summary) == ESP_OK)
    s_overflowed = summary.has_overflowed;
  s_traced = heap_trace_get_count();
  size_t internal = 0;
  heap_trace_record_t record{};
  for (size_t i = 0; i < s_traced; i++) {
    if (heap_trace_get(i, &record) == ESP_OK && record.address != nullptr && esp_ptr_internal(record.address))
      internal++;
  }
  if (internal == 0)
    return 0;
  s_owners = static_cast<Owner *>(heap_caps_malloc(internal * sizeof(Owner), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_owners == nullptr)
    return 0;
  for (size_t i = 0; i < s_traced && s_owner_count < internal; i++) {
    if (heap_trace_get(i, &record) != ESP_OK || record.address == nullptr || !esp_ptr_internal(record.address))
      continue;
    Owner &o = s_owners[s_owner_count++];
    o.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(record.address));
    o.size = static_cast<uint32_t>(record.size);
    for (int d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH; d++)
      o.callers[d] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(record.alloced_by[d]));
  }
  return s_owner_count;
}

size_t owner_count() { return s_owner_count; }

const Owner *owner(size_t index) { return index < s_owner_count ? &s_owners[index] : nullptr; }

size_t traced_count() { return s_traced; }

bool overflowed() { return s_overflowed; }

}

#endif
