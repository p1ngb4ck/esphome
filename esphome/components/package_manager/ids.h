#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"

namespace esphome {
namespace package_manager {

struct IdEntry {
  const char *name;
  void *addr;
  uint32_t type;
};

void set_core_ids(const IdEntry *entries, size_t count);
bool register_ids(const void *owner, const IdEntry *entries, size_t count);
void unregister_ids(const void *owner);
void *query(const char *name, uint32_t type);
bool entity_key_free(const EntityBase *entity);

}
}

extern "C" void *package_manager_query_id(const char *name, uint32_t type);
