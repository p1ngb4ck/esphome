#include "ids.h"

#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace package_manager {

static const char *const TAG = "package_manager.ids";

struct Table {
  const void *owner;
  const IdEntry *entries;
  size_t count;
};

static const IdEntry *core_entries = nullptr;
static size_t core_count = 0;
static Table *tables = nullptr;
static uint8_t table_count = 0;
static uint8_t table_capacity = 0;

void set_core_ids(const IdEntry *entries, size_t count) {
  core_entries = entries;
  core_count = count;
}

static const IdEntry *find_in(const IdEntry *entries, size_t count, const char *name) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    const size_t mid = (low + high) / 2;
    const int order = std::strcmp(entries[mid].name, name);
    if (order == 0)
      return &entries[mid];
    if (order < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return nullptr;
}

static const IdEntry *find(const char *name) {
  const IdEntry *found = find_in(core_entries, core_count, name);
  for (uint8_t i = 0; found == nullptr && i < table_count; i++)
    found = find_in(tables[i].entries, tables[i].count, name);
  return found;
}

bool register_ids(const void *owner, const IdEntry *entries, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (find(entries[i].name) != nullptr) {
      ESP_LOGE(TAG, "id '%s' is registered already", entries[i].name);
      return false;
    }
  }
  if (table_count == table_capacity) {
    const uint8_t capacity = table_capacity == 0 ? 4 : table_capacity * 2;
    RAMAllocator<Table> allocator(RAMAllocator<Table>::ALLOC_EXTERNAL);
    Table *grown = allocator.allocate(capacity);
    if (grown == nullptr)
      return false;
    if (tables != nullptr) {
      std::memcpy(grown, tables, table_count * sizeof(Table));
      allocator.deallocate(tables, table_capacity);
    }
    tables = grown;
    table_capacity = capacity;
  }
  tables[table_count++] = Table{owner, entries, count};
  return true;
}

void unregister_ids(const void *owner) {
  for (uint8_t i = 0; i < table_count; i++) {
    if (tables[i].owner != owner)
      continue;
    std::memmove(&tables[i], &tables[i + 1], (table_count - i - 1) * sizeof(Table));
    table_count--;
    break;
  }
  if (table_count == 0 && tables != nullptr) {
    RAMAllocator<Table>(RAMAllocator<Table>::ALLOC_EXTERNAL).deallocate(tables, table_capacity);
    tables = nullptr;
    table_capacity = 0;
  }
}

void *query(const char *name, uint32_t type) {
  const IdEntry *found = find(name);
  return found != nullptr && found->type == type ? found->addr : nullptr;
}

bool entity_key_free(const EntityBase *entity) {
  const uint32_t key = entity->get_object_id_hash();
#define ENTITY_TYPE_(type, singular, plural, count, upper) \
  for (auto *other : App.get_##plural()) { \
    if (static_cast<const EntityBase *>(other) != entity && other->get_object_id_hash() == key) \
      return false; \
  }
#define ENTITY_CONTROLLER_TYPE_(type, singular, plural, count, upper, callback) \
  ENTITY_TYPE_(type, singular, plural, count, upper)
#include "esphome/core/entity_types.h"
#undef ENTITY_TYPE_
#undef ENTITY_CONTROLLER_TYPE_
  return true;
}

}
}

extern "C" void *package_manager_query_id(const char *name, uint32_t type) {
  return esphome::package_manager::query(name, type);
}
